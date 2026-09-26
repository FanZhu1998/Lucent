#include "lucent/export/encode.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <lucent/image/color_math.hpp>

#include "icc_builder.hpp"
#include "lcms_support.hpp"

namespace lucent::exporter {

Status OutputImage::validate() const {
    constexpr const char* kWhere = "export.output_image.validate";
    if (width < 1 || height < 1) {
        return fail(ErrorCode::InvalidArgument, "output image width and height must be >= 1", kWhere);
    }
    if (auto status = exporter::validate(bit_depth); !status) {
        return status;
    }
    if (auto status = exporter::validate(profile); !status) {
        return status;
    }
    const std::size_t expected = row_samples() * static_cast<std::size_t>(height);
    const bool eight = bit_depth == BitDepth::Eight;
    const std::size_t used = eight ? samples8.size() : samples16.size();
    const std::size_t other = eight ? samples16.size() : samples8.size();
    if (used != expected || other != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "output image sample buffer does not match width * height * 3 for its bit depth", kWhere);
    }
    return ok();
}

namespace {

constexpr const char* kWhere = "export.encode";

// Rows processed between cancellation checks.
constexpr std::int32_t kCancelCheckRows = 32;

// Parallelism: at most this many threads, and at least this many pixels per thread.
constexpr unsigned kMaxThreads = 8;
constexpr std::int64_t kMinPixelsPerThread = std::int64_t{1} << 18;

cmsUInt32Number lcms_intent(RenderingIntent intent) noexcept {
    switch (intent) {
        case RenderingIntent::Perceptual:
            return INTENT_PERCEPTUAL;
        case RenderingIntent::Saturation:
            return INTENT_SATURATION;
        case RenderingIntent::AbsoluteColorimetric:
            return INTENT_ABSOLUTE_COLORIMETRIC;
        case RenderingIntent::RelativeColorimetric:
            break;
    }
    return INTENT_RELATIVE_COLORIMETRIC;
}

// Result of one row band. `bad_index` (y * width + x) locates the first non-finite sample when
// status failed with InvalidArgument for that reason.
struct BandResult {
    Status status = ok();
    std::int64_t bad_index = std::numeric_limits<std::int64_t>::max();  // y * width + x
};

struct BandJob {
    image::ConstImageView<float> rgb;
    const image::TransferCurve* curve = nullptr;
    cmsHTRANSFORM transform = nullptr;
    OutputImage* out = nullptr;
    core::CancellationToken cancel;
};

BandResult run_band(const BandJob& job, std::int32_t y_begin, std::int32_t y_end) noexcept {
    BandResult result;
    try {
        const std::int32_t width = job.rgb.width();
        const std::size_t row_samples = static_cast<std::size_t>(width) * 3;
        const bool linear = job.curve->function() == image::TransferFunction::Linear;
        std::vector<float> scratch(row_samples);
        const bool eight = job.out->bit_depth == BitDepth::Eight;
        for (std::int32_t y = y_begin; y < y_end; ++y) {
            if ((y - y_begin) % kCancelCheckRows == 0) {
                if (auto status = job.cancel.check(kWhere); !status) {
                    result.status = std::move(status);
                    return result;
                }
            }
            const float* src = job.rgb.row(y);
            for (std::size_t i = 0; i < row_samples; ++i) {
                const float value = src[i];
                if (!std::isfinite(value)) {
                    const auto x = static_cast<std::int32_t>(i / 3);
                    result.bad_index = std::int64_t{y} * width + x;
                    result.status = fail(ErrorCode::InvalidArgument,
                                         "source pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                                             ") channel " + std::to_string(i % 3) +
                                             " is not finite (NaN or infinity)",
                                         kWhere);
                    return result;
                }
                scratch[i] = linear ? value : job.curve->decode(value);
            }
            void* dst = eight ? static_cast<void*>(job.out->samples8.data() + row_samples * static_cast<std::size_t>(y))
                              : static_cast<void*>(job.out->samples16.data() +
                                                   row_samples * static_cast<std::size_t>(y));
            cmsDoTransform(job.transform, scratch.data(), dst, static_cast<cmsUInt32Number>(width));
        }
    } catch (const std::bad_alloc&) {
        result.status = fail(ErrorCode::OutOfMemory, "cannot allocate a conversion row", kWhere);
    } catch (...) {
        result.status = fail(ErrorCode::Internal, "unexpected exception during output conversion", kWhere);
    }
    return result;
}

// Chooses the deterministic merged result: the earliest non-finite sample wins, then the first
// failing band in order.
Status merge(std::vector<BandResult>& results) {
    BandResult* first_bad = nullptr;
    for (auto& r : results) {
        if (!r.status && r.status.error().code == ErrorCode::InvalidArgument &&
            (first_bad == nullptr || r.bad_index < first_bad->bad_index)) {
            first_bad = &r;
        }
    }
    if (first_bad != nullptr) {
        return std::move(first_bad->status);
    }
    for (auto& r : results) {
        if (!r.status) {
            return std::move(r.status);
        }
    }
    return ok();
}

Status run_bands(const BandJob& job) {
    const std::int32_t height = job.rgb.height();
    const std::int64_t pixels = std::int64_t{job.rgb.width()} * height;
    unsigned threads = std::max(1u, std::min(kMaxThreads, std::thread::hardware_concurrency()));
    threads = static_cast<unsigned>(
        std::clamp<std::int64_t>(pixels / kMinPixelsPerThread, 1, static_cast<std::int64_t>(threads)));
    threads = std::min(threads, static_cast<unsigned>(height));

    std::vector<BandResult> results(threads);
    auto band_begin = [&](unsigned band) {
        return static_cast<std::int32_t>(std::int64_t{height} * band / threads);
    };
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        unsigned spawned = 1;  // band 0 runs on the calling thread
        try {
            for (; spawned < threads; ++spawned) {
                workers.emplace_back([&job, &results, band_begin, spawned] {
                    results[spawned] = run_band(job, band_begin(spawned), band_begin(spawned + 1));
                });
            }
        } catch (const std::system_error&) {
            // Could not start more threads: the remaining bands run on this thread below.
        }
        results[0] = run_band(job, band_begin(0), band_begin(1));
        for (unsigned band = spawned; band < threads; ++band) {
            results[band] = run_band(job, band_begin(band), band_begin(band + 1));
        }
    }  // joins the workers
    return merge(results);
}

Status check_source(image::ConstImageView<float> rgb, const image::ColorEncoding& source) {
    if (rgb.empty()) {
        return fail(ErrorCode::InvalidArgument, "source view is empty", kWhere);
    }
    if (rgb.channels() != 3) {
        return fail(ErrorCode::InvalidArgument,
                    "source must have exactly 3 channels (RGB), got " + std::to_string(rgb.channels()), kWhere);
    }
    if (auto status = source.validate(); !status) {
        return status;
    }
    if (source.layout != image::ChannelLayout::Rgb) {
        return fail(ErrorCode::InvalidArgument, "source encoding must have the RGB channel layout, got " +
                                                    std::string(image::to_string(source.layout)),
                    kWhere);
    }
    if (source.primaries == image::ColorPrimaries::CameraNative ||
        source.primaries == image::ColorPrimaries::CieXyz) {
        return fail(ErrorCode::InvalidArgument,
                    "source encoding must have RGB primaries with known chromaticities, got " +
                        std::string(image::to_string(source.primaries)),
                    kWhere);
    }
    if (source.reference != image::ImageReference::Display) {
        return fail(ErrorCode::InvalidArgument,
                    "source must be display-referred; render scene-referred data for display before export",
                    kWhere);
    }
    if (rgb.width() > kMaxOutputDimension || rgb.height() > kMaxOutputDimension) {
        return fail(ErrorCode::LimitExceeded, "image side exceeds " + std::to_string(kMaxOutputDimension) + " pixels",
                    kWhere);
    }
    return ok();
}

}  // namespace

Expected<OutputImage> encode_for_output(image::ConstImageView<float> rgb, const image::ColorEncoding& source,
                                        OutputProfile profile, BitDepth depth, const OutputTransformOptions& options,
                                        core::CancellationToken cancel) {
    if (auto status = validate(profile); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = validate(depth); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = validate(options.intent); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = check_source(rgb, source); !status) {
        return Unexpected(std::move(status.error()));
    }

    const std::size_t sample_count = static_cast<std::size_t>(rgb.width()) * static_cast<std::size_t>(rgb.height()) * 3;
    const std::size_t sample_bytes = depth == BitDepth::Eight ? 1 : 2;
    if (sample_count > kMaxOutputBytes / sample_bytes) {
        return fail(ErrorCode::LimitExceeded, "quantised output would exceed 2 GiB", kWhere);
    }

    auto curve = image::TransferCurve::from_encoding(source);
    if (!curve) {
        return Unexpected(std::move(curve.error()));
    }
    auto primaries = image::rgb_primaries(source.primaries);
    if (!primaries) {
        return Unexpected(std::move(primaries.error()));
    }
    auto white = image::white_chromaticity(source);
    if (!white) {
        return Unexpected(std::move(white.error()));
    }
    auto icc = icc_profile(profile);
    if (!icc) {
        return Unexpected(std::move(icc.error()));
    }

    try {
        auto context = detail::LcmsContext::create();
        if (!context) {
            return Unexpected(std::move(context.error()));
        }
        auto input = detail::create_linear_rgb_profile(*context, *primaries, *white);
        if (!input) {
            return Unexpected(std::move(input.error()));
        }
        detail::UniqueProfile output(
            cmsOpenProfileFromMemTHR(context->get(), icc->data(), static_cast<cmsUInt32Number>(icc->size())));
        if (!output) {
            return Unexpected(context->error(ErrorCode::Internal, "cannot open the output ICC profile", kWhere));
        }
        cmsUInt32Number flags = cmsFLAGS_NOCACHE;
        if (options.black_point_compensation) {
            flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
        }
        detail::UniqueTransform transform(cmsCreateTransformTHR(
            context->get(), input->get(), TYPE_RGB_FLT, output.get(),
            depth == BitDepth::Eight ? TYPE_RGB_8 : TYPE_RGB_16, lcms_intent(options.intent), flags));
        if (!transform) {
            return Unexpected(context->error(ErrorCode::Internal, "cannot create the output transform", kWhere));
        }

        OutputImage out;
        out.width = rgb.width();
        out.height = rgb.height();
        out.bit_depth = depth;
        out.profile = profile;
        if (depth == BitDepth::Eight) {
            out.samples8.resize(sample_count);
        } else {
            out.samples16.resize(sample_count);
        }

        const BandJob job{rgb, &*curve, transform.get(), &out, std::move(cancel)};
        if (auto status = run_bands(job); !status) {
            return Unexpected(std::move(status.error()));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the quantised output image", kWhere);
    }
}

}  // namespace lucent::exporter
