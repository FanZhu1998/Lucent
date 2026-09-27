#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <execution>
#include <format>
#include <new>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lucent/application/display_image.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhereDownscale = "app.display.downscale";
constexpr std::string_view kWhereEncode = "app.display.encode";

// 8-bit sRGB quantisation of linear light in [0, 1]. The code of v is the number of thresholds
// t[c - 1] = srgb_decode((c - 0.5) / 255), c = 1..255, that are <= v, which equals
// floor(255 * srgb_encode(v) + 0.5), the exporter's round-half-up quantisation of the sRGB curve
// (the curve is increasing). A 65536-bin table gives the code at each bin's lower edge; since no
// bin spans more than a twentieth of a code step (the curve's steepest slope, 12.92, gives 3294
// codes per unit), at most one threshold lies inside a bin, and one comparison completes the
// exact count.
class SrgbQuantizer {
public:
    static const SrgbQuantizer& instance() {
        static const SrgbQuantizer quantizer;
        return quantizer;
    }

    [[nodiscard]] std::uint8_t code(double v) const noexcept {
        v = std::clamp(v, 0.0, 1.0);
        const auto bin = static_cast<std::size_t>(v * static_cast<double>(kBins - 1));
        std::uint32_t c = bins_[bin];
        while (c < 255 && v >= thresholds_[c]) {
            ++c;
        }
        return static_cast<std::uint8_t>(c);
    }

private:
    static constexpr std::size_t kBins = 65536;

    SrgbQuantizer() {
        for (int c = 1; c <= 255; ++c) {
            thresholds_[static_cast<std::size_t>(c - 1)] = image::srgb_decode((c - 0.5) / 255.0);
        }
        std::uint32_t c = 0;
        for (std::size_t i = 0; i < kBins; ++i) {
            const double edge = static_cast<double>(i) / static_cast<double>(kBins - 1);
            while (c < 255 && edge >= thresholds_[c]) {
                ++c;
            }
            bins_[i] = static_cast<std::uint8_t>(c);
        }
    }

    std::array<double, 255> thresholds_{};
    std::array<std::uint8_t, kBins> bins_{};
};

// Source samples covering one output sample along an axis: source index `first` onward, with
// `weights` (overlap lengths in source pixels, summing to the scale factor).
struct Footprint {
    std::int32_t first = 0;
    std::vector<double> weights;
};

std::vector<Footprint> footprints(std::int32_t source, std::int32_t target) {
    const double scale = static_cast<double>(source) / static_cast<double>(target);
    std::vector<Footprint> out(static_cast<std::size_t>(target));
    for (std::int32_t i = 0; i < target; ++i) {
        const double begin = i * scale;
        const double end = (i + 1) * scale;
        const auto first = static_cast<std::int32_t>(std::floor(begin));
        const auto last = std::min(source - 1, static_cast<std::int32_t>(std::ceil(end)) - 1);
        Footprint& f = out[static_cast<std::size_t>(i)];
        f.first = first;
        for (std::int32_t s = first; s <= last; ++s) {
            const double overlap = std::min<double>(s + 1, end) - std::max<double>(s, begin);
            f.weights.push_back(overlap > 0.0 ? overlap / scale : 0.0);
        }
    }
    return out;
}

}  // namespace

image::Size fit_within(image::Size size, std::int32_t long_edge) noexcept {
    if (size.empty() || long_edge < 1) {
        return size;
    }
    const std::int32_t longer = std::max(size.width, size.height);
    if (longer <= long_edge) {
        return size;
    }
    const double scale = static_cast<double>(long_edge) / static_cast<double>(longer);
    const auto scaled = [&](std::int32_t v) {
        return std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(v * scale)));
    };
    if (size.width >= size.height) {
        return image::Size{long_edge, scaled(size.height)};
    }
    return image::Size{scaled(size.width), long_edge};
}

Expected<engine::ColorImage> downscale_area(const engine::ColorImage& source, image::Size target,
                                            const core::CancellationToken& cancel, bool parallel) {
    const image::ImageBuffer<float>& in = source.pixels;
    if (in.empty() || in.channels() != 3 || source.encoding.layout != image::ChannelLayout::Rgb) {
        return fail(ErrorCode::InvalidArgument, "downscaling needs a non-empty 3-channel RGB image",
                    std::string(kWhereDownscale));
    }
    if (source.encoding.transfer != image::TransferFunction::Linear) {
        return fail(ErrorCode::InvalidArgument, "downscaling averages light and needs linear values",
                    std::string(kWhereDownscale));
    }
    if (target.empty() || target.width > in.width() || target.height > in.height()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("cannot downscale {} x {} to {} x {}", in.width(), in.height(), target.width,
                                target.height),
                    std::string(kWhereDownscale));
    }
    try {
        auto out = image::ImageBuffer<float>::create_uninitialized(target.width, target.height, 3);
        if (!out) {
            return Unexpected(std::move(out.error()));
        }
        const std::vector<Footprint> columns = footprints(in.width(), target.width);
        const std::vector<Footprint> rows = footprints(in.height(), target.height);

        // Output rows are split into bands, each with its own scratch rows allocated here, so the
        // parallel section never allocates (an exception there would terminate the process).
        const std::size_t row_samples = static_cast<std::size_t>(target.width) * 3;
        const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
        const std::int32_t band_count =
            parallel ? std::clamp<std::int32_t>(static_cast<std::int32_t>(hardware) * 4, 1, target.height)
                     : 1;
        struct Band {
            std::int32_t begin = 0;
            std::int32_t end = 0;
            std::vector<double> accumulated;
            std::vector<double> horizontal;
        };
        std::vector<Band> bands(static_cast<std::size_t>(band_count));
        for (std::int32_t b = 0; b < band_count; ++b) {
            Band& band = bands[static_cast<std::size_t>(b)];
            band.begin = static_cast<std::int32_t>(std::int64_t{target.height} * b / band_count);
            band.end = static_cast<std::int32_t>(std::int64_t{target.height} * (b + 1) / band_count);
            band.accumulated.resize(row_samples);
            band.horizontal.resize(row_samples);
        }

        std::atomic<bool> cancelled{false};
        image::ImageBuffer<float>& output = *out;
        const auto run_band = [&](Band& band) {
            for (std::int32_t y = band.begin; y < band.end; ++y) {
                if (cancelled.load(std::memory_order_relaxed) || cancel.is_cancelled()) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                std::fill(band.accumulated.begin(), band.accumulated.end(), 0.0);
                const Footprint& vertical = rows[static_cast<std::size_t>(y)];
                for (std::size_t k = 0; k < vertical.weights.size(); ++k) {
                    const double wy = vertical.weights[k];
                    if (wy == 0.0) {
                        continue;
                    }
                    const float* src = in.row(vertical.first + static_cast<std::int32_t>(k));
                    for (std::int32_t x = 0; x < target.width; ++x) {
                        const Footprint& horizontal = columns[static_cast<std::size_t>(x)];
                        double r = 0.0;
                        double g = 0.0;
                        double b = 0.0;
                        const float* px = src + static_cast<std::ptrdiff_t>(horizontal.first) * 3;
                        for (std::size_t j = 0; j < horizontal.weights.size(); ++j, px += 3) {
                            const double wx = horizontal.weights[j];
                            r += wx * px[0];
                            g += wx * px[1];
                            b += wx * px[2];
                        }
                        double* acc = band.accumulated.data() + static_cast<std::size_t>(x) * 3;
                        acc[0] += wy * r;
                        acc[1] += wy * g;
                        acc[2] += wy * b;
                    }
                }
                float* dst = output.row(y);
                for (std::size_t i = 0; i < row_samples; ++i) {
                    dst[i] = static_cast<float>(band.accumulated[i]);
                }
            }
        };
        if (parallel && band_count > 1) {
            std::for_each(std::execution::par, bands.begin(), bands.end(), run_band);
        } else {
            for (Band& band : bands) {
                run_band(band);
            }
        }
        if (cancelled.load() || cancel.is_cancelled()) {
            return fail(ErrorCode::Cancelled, "downscaling was cancelled", std::string(kWhereDownscale));
        }
        engine::ColorImage result;
        result.pixels = std::move(output);
        result.encoding = source.encoding;
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "downscaling ran out of memory", std::string(kWhereDownscale));
    }
}

Expected<DisplayImage> encode_display_srgb8(const engine::ColorImage& image,
                                            const core::CancellationToken& cancel, bool parallel) {
    const image::ImageBuffer<float>& in = image.pixels;
    const image::ColorEncoding& encoding = image.encoding;
    if (in.empty() || in.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "display encoding needs a non-empty 3-channel image",
                    std::string(kWhereEncode));
    }
    if (auto valid = encoding.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, valid.error().message, std::string(kWhereEncode));
    }
    if (encoding.layout != image::ChannelLayout::Rgb ||
        encoding.reference != image::ImageReference::Display ||
        encoding.primaries == image::ColorPrimaries::CameraNative ||
        encoding.primaries == image::ColorPrimaries::CieXyz) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("display encoding needs display-referred RGB, not '{}'", encoding.to_string()),
            std::string(kWhereEncode));
    }
    if (in.width() > exporter::kMaxOutputDimension || in.height() > exporter::kMaxOutputDimension) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{} x {} exceeds the output limit", in.width(), in.height()),
                    std::string(kWhereEncode));
    }
    if (encoding.transfer != image::TransferFunction::Linear) {
        // Rare encodings: let the exporter decode their transfer function.
        auto encoded = exporter::encode_for_output(in.const_view(), encoding, exporter::OutputProfile::Srgb,
                                                   exporter::BitDepth::Eight, {}, cancel);
        if (!encoded) {
            return Unexpected(std::move(encoded.error()));
        }
        DisplayImage display;
        display.width = encoded->width;
        display.height = encoded->height;
        display.rgb = std::move(encoded->samples8);
        return display;
    }
    auto matrix = image::conversion_matrix(encoding, image::ColorEncoding::linear_srgb());
    if (!matrix) {
        return fail(ErrorCode::InvalidArgument, matrix.error().message, std::string(kWhereEncode));
    }
    const image::Matrix3d m = *matrix;
    const SrgbQuantizer& quantizer = SrgbQuantizer::instance();

    try {
        DisplayImage display;
        display.width = in.width();
        display.height = in.height();
        const std::size_t row_samples = static_cast<std::size_t>(in.width()) * 3;
        display.rgb.resize(row_samples * static_cast<std::size_t>(in.height()));

        const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
        const std::int32_t band_count =
            parallel ? std::clamp<std::int32_t>(static_cast<std::int32_t>(hardware) * 4, 1, in.height()) : 1;
        std::vector<std::pair<std::int32_t, std::int32_t>> bands(static_cast<std::size_t>(band_count));
        for (std::int32_t b = 0; b < band_count; ++b) {
            bands[static_cast<std::size_t>(b)] = {
                static_cast<std::int32_t>(std::int64_t{in.height()} * b / band_count),
                static_cast<std::int32_t>(std::int64_t{in.height()} * (b + 1) / band_count)};
        }
        std::atomic<bool> cancelled{false};
        std::atomic<bool> non_finite{false};
        const auto quantize = [&quantizer](double v) { return quantizer.code(v); };
        const auto run_band = [&](const std::pair<std::int32_t, std::int32_t>& band) {
            for (std::int32_t y = band.first; y < band.second; ++y) {
                if (cancelled.load(std::memory_order_relaxed) || cancel.is_cancelled()) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                const float* src = in.row(y);
                std::uint8_t* dst = display.rgb.data() + static_cast<std::size_t>(y) * row_samples;
                for (std::int32_t x = 0; x < in.width(); ++x, src += 3, dst += 3) {
                    if (!std::isfinite(src[0]) || !std::isfinite(src[1]) || !std::isfinite(src[2])) {
                        non_finite.store(true, std::memory_order_relaxed);
                        dst[0] = dst[1] = dst[2] = 0;
                        continue;
                    }
                    const image::Vec3d linear = m * image::Vec3d{src[0], src[1], src[2]};
                    dst[0] = quantize(linear[0]);
                    dst[1] = quantize(linear[1]);
                    dst[2] = quantize(linear[2]);
                }
            }
        };
        if (parallel && band_count > 1) {
            std::for_each(std::execution::par, bands.begin(), bands.end(), run_band);
        } else {
            for (const auto& band : bands) {
                run_band(band);
            }
        }
        if (non_finite.load()) {
            for (std::int32_t y = 0; y < in.height(); ++y) {
                const float* row = in.row(y);
                for (std::int32_t i = 0; i < in.width() * 3; ++i) {
                    if (!std::isfinite(row[i])) {
                        return fail(ErrorCode::InvalidArgument,
                                    std::format("pixel ({}, {}) has a non-finite sample", i / 3, y),
                                    std::string(kWhereEncode));
                    }
                }
            }
        }
        if (cancelled.load() || cancel.is_cancelled()) {
            return fail(ErrorCode::Cancelled, "display encoding was cancelled", std::string(kWhereEncode));
        }
        return display;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "display encoding ran out of memory", std::string(kWhereEncode));
    }
}

Histogram compute_histogram(const DisplayImage& image) noexcept {
    Histogram histogram;
    const std::size_t pixels = image.rgb.size() / 3;
    const std::uint8_t* p = image.rgb.data();
    for (std::size_t i = 0; i < pixels; ++i, p += 3) {
        ++histogram.red[p[0]];
        ++histogram.green[p[1]];
        ++histogram.blue[p[2]];
        // round(0.2126 R + 0.7152 G + 0.0722 B) in integer arithmetic (the weights sum to 10000).
        const std::uint32_t luma = (2126u * p[0] + 7152u * p[1] + 722u * p[2] + 5000u) / 10000u;
        ++histogram.luminance[std::min<std::uint32_t>(luma, 255u)];
    }
    histogram.pixels = pixels;
    return histogram;
}

}  // namespace lucent::app
