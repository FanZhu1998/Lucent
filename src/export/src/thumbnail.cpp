#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

#include <lucent/export/encode.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/thumbnail.hpp>

#include "codecs.hpp"

namespace lucent::exporter {

namespace {

constexpr const char* kWhere = "export.thumbnail";

// One source sample's share of an output sample.
struct Tap {
    std::int32_t index = 0;
    float weight = 0.0f;
};

// Area averaging from `from` samples down to `to` (to <= from): output sample o covers the source
// interval [o * from / to, (o + 1) * from / to); each source sample contributes the fraction of
// that interval it overlaps. The weights of every output sample sum to 1.
std::vector<std::vector<Tap>> area_taps(std::int32_t from, std::int32_t to) {
    std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(to));
    const double scale = static_cast<double>(from) / static_cast<double>(to);
    for (std::int32_t o = 0; o < to; ++o) {
        const double begin = o * scale;
        const double end = (o + 1) * scale;
        const auto first = static_cast<std::int32_t>(std::floor(begin));
        const auto last = std::min(from, static_cast<std::int32_t>(std::ceil(end)));
        for (std::int32_t i = first; i < last; ++i) {
            const double overlap = std::min(end, i + 1.0) - std::max(begin, static_cast<double>(i));
            if (overlap > 0.0) {
                taps[static_cast<std::size_t>(o)].push_back(Tap{i, static_cast<float>(overlap / scale)});
            }
        }
    }
    return taps;
}

// Reduces interleaved 8-bit RGB from width x height to out_width x out_height (both no larger).
Status reduce(const std::vector<std::uint8_t>& rgb, std::int32_t width, std::int32_t height,
              std::int32_t out_width, std::int32_t out_height, std::vector<std::uint8_t>& out,
              const core::CancellationToken& cancel) {
    const std::vector<std::vector<Tap>> columns = area_taps(width, out_width);
    const std::vector<std::vector<Tap>> rows = area_taps(height, out_height);
    const std::size_t out_row = static_cast<std::size_t>(out_width) * 3;
    out.assign(out_row * static_cast<std::size_t>(out_height), 0);
    std::vector<float> accumulated(out_row);
    for (std::int32_t y = 0; y < out_height; ++y) {
        if (y % 64 == 0) {
            if (Status status = cancel.check(kWhere); !status) {
                return status;
            }
        }
        std::fill(accumulated.begin(), accumulated.end(), 0.0f);
        for (const Tap& row_tap : rows[static_cast<std::size_t>(y)]) {
            const std::uint8_t* source = rgb.data() + static_cast<std::size_t>(row_tap.index) * width * 3;
            for (std::int32_t x = 0; x < out_width; ++x) {
                float r = 0.0f;
                float g = 0.0f;
                float b = 0.0f;
                for (const Tap& column_tap : columns[static_cast<std::size_t>(x)]) {
                    const std::uint8_t* pixel = source + static_cast<std::size_t>(column_tap.index) * 3;
                    r += column_tap.weight * pixel[0];
                    g += column_tap.weight * pixel[1];
                    b += column_tap.weight * pixel[2];
                }
                float* target = accumulated.data() + static_cast<std::size_t>(x) * 3;
                target[0] += row_tap.weight * r;
                target[1] += row_tap.weight * g;
                target[2] += row_tap.weight * b;
            }
        }
        std::uint8_t* destination = out.data() + static_cast<std::size_t>(y) * out_row;
        for (std::size_t i = 0; i < out_row; ++i) {
            destination[i] = static_cast<std::uint8_t>(std::clamp(std::lround(accumulated[i]), 0L, 255L));
        }
    }
    return ok();
}

}  // namespace

Expected<JpegThumbnail> make_jpeg_thumbnail(std::span<const std::uint8_t> jpeg,
                                            const JpegThumbnailOptions& options,
                                            const core::CancellationToken& cancel) {
    const std::int32_t max_long_edge = options.max_long_edge;
    const std::int32_t quality = options.quality;
    if (max_long_edge < 1 || quality < 1 || quality > 100 || jpeg.empty()) {
        return fail(ErrorCode::InvalidArgument, "invalid thumbnail size, quality or input", kWhere);
    }
    try {
        auto size = detail::read_jpeg_stream_size(jpeg);
        if (!size) {
            return Unexpected(std::move(size.error()));
        }
        const auto [width, height] = *size;
        if (width < 1 || height < 1) {
            return fail(ErrorCode::Corrupt, "the JPEG stream has no pixels", kWhere);
        }
        const std::int32_t long_edge = std::max(width, height);
        if (long_edge <= max_long_edge && jpeg.size() <= options.max_unreduced_bytes) {
            return JpegThumbnail{std::vector<std::uint8_t>(jpeg.begin(), jpeg.end()), width, height, false};
        }
        // The strongest DCT reduction that keeps the long edge at or above the target (libjpeg
        // rounds scaled sides up).
        std::int32_t denominator = 1;
        for (const std::int32_t d : {8, 4, 2}) {
            if ((long_edge + d - 1) / d >= max_long_edge) {
                denominator = d;
                break;
            }
        }
        auto decoded = detail::decode_jpeg_stream(jpeg, denominator, cancel);
        if (!decoded) {
            return Unexpected(std::move(decoded.error()));
        }
        const bool landscape = decoded->width >= decoded->height;
        const std::int32_t decoded_long = landscape ? decoded->width : decoded->height;
        const std::int32_t decoded_short = landscape ? decoded->height : decoded->width;
        const std::int32_t out_long = std::min(max_long_edge, decoded_long);  // its own size if smaller
        const auto out_short = static_cast<std::int32_t>(std::max<long>(
            1,
            std::lround(static_cast<double>(decoded_short) * out_long / static_cast<double>(decoded_long))));

        OutputImage image;
        image.width = landscape ? out_long : out_short;
        image.height = landscape ? out_short : out_long;
        image.bit_depth = BitDepth::Eight;
        if (Status reduced = reduce(decoded->rgb, decoded->width, decoded->height, image.width, image.height,
                                    image.samples8, cancel);
            !reduced) {
            return Unexpected(std::move(reduced.error()));
        }
        decoded->rgb = {};  // release the decoded image before encoding
        auto encoded = detail::encode_jpeg(image, JpegSettings{quality}, decoded->icc, cancel);
        if (!encoded) {
            return Unexpected(std::move(encoded.error()));
        }
        return JpegThumbnail{std::move(*encoded), image.width, image.height, true};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "making a thumbnail ran out of memory", kWhere);
    }
}

}  // namespace lucent::exporter
