// Failure-path tests for inspect_tiff(): malformed structure, malformed interpreted tags, limits
// and source errors. Every case must fail cleanly with the documented code and location.

#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
namespace t = tiff_tag;
using ts::Built;
using ts::Document;
using ts::Endian;
using ts::Format;

Expected<TiffSummary> inspect_bytes(std::span<const std::byte> bytes, const TiffInspectLimits& limits = {}) {
    MemorySource source(bytes);
    return inspect_tiff(source, limits);
}

Expected<TiffSummary> inspect(const Built& built, const TiffInspectLimits& limits = {}) {
    return inspect_bytes(built.span(), limits);
}

Expected<TiffSummary> inspect(const Document& doc, const TiffInspectLimits& limits = {}) {
    return inspect(ts::build(doc), limits);
}

// Asserts that `result` failed with `code` at `where`.
void expect_error(const Expected<TiffSummary>& result, ErrorCode code, std::string_view where) {
    ASSERT_FALSE(result) << "inspection unexpectedly succeeded";
    EXPECT_EQ(result.error().code, code) << result.error().describe();
    EXPECT_EQ(result.error().where, where) << result.error().describe();
    EXPECT_FALSE(result.error().message.empty());
}

void expect_corrupt(const Expected<TiffSummary>& result, std::string_view where) {
    expect_error(result, ErrorCode::Corrupt, where);
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// ---- Header ----------------------------------------------------------------------------------

TEST(TiffCorrupt, EmptySource) {
    expect_corrupt(inspect_bytes({}), "raw.tiff.header");
}

TEST(TiffCorrupt, TruncatedClassicHeader) {
    const Built built = ts::build(ts::dng::document(Endian::Little, Format::Classic));
    for (const std::size_t size : {1u, 3u, 4u, 7u}) {
        SCOPED_TRACE(size);
        expect_corrupt(inspect_bytes(std::span(built.bytes).first(size)), "raw.tiff.header");
    }
}

TEST(TiffCorrupt, TruncatedBigTiffHeader) {
    const Built built = ts::build(ts::dng::document(Endian::Big, Format::BigTiff));
    for (const std::size_t size : {4u, 8u, 12u, 15u}) {
        SCOPED_TRACE(size);
        expect_corrupt(inspect_bytes(std::span(built.bytes).first(size)), "raw.tiff.header");
    }
}

TEST(TiffCorrupt, NonTiffSignaturesAreUnsupported) {
    const std::vector<std::vector<std::byte>> inputs{
        bytes_of({0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F'}),  // JPEG
        bytes_of({'I', 'I', 'R', 'O', 8, 0, 0, 0}),                     // Olympus ORF
        bytes_of({'I', 'I', 'U', 0, 8, 0, 0, 0}),                       // Panasonic RW2
        bytes_of({'M', 'M', 0, 44, 0, 0, 0, 8}),                        // unknown magic
        bytes_of({0, 0, 0, 0x18, 'f', 't', 'y', 'p'}),                  // ISO BMFF (CR3)
        bytes_of({'X'}),                                                // one non-TIFF byte
        bytes_of({'I', 'M', 42, 0, 8, 0, 0, 0}),                        // mixed byte-order mark
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE(i);
        expect_error(inspect_bytes(inputs[i]), ErrorCode::Unsupported, "raw.tiff.header");
    }
}

TEST(TiffCorrupt, BigTiffHeaderFields) {
    Built built = ts::build(ts::dng::document(Endian::Little, Format::BigTiff));
    Built future = built;
    future.put(4, 16, 2);  // 16-byte offsets: a possible future BigTIFF, not corrupt
    expect_error(inspect(future), ErrorCode::Unsupported, "raw.tiff.header");
    built.put(6, 1, 2);  // reserved field must be zero
    expect_corrupt(inspect(built), "raw.tiff.header");
}

TEST(TiffCorrupt, FirstIfdOffsetZero) {
    Built built = ts::build(ts::dng::document());
    built.put(4, 0, 4);
    expect_corrupt(inspect(built), "raw.tiff.header");
}

// ---- IFD placement and structure -----------------------------------------------------------

TEST(TiffCorrupt, IfdOffsetOutOfBounds) {
    for (const Format format : {Format::Classic, Format::BigTiff}) {
        SCOPED_TRACE(format == Format::Classic ? "classic" : "BigTIFF");
        Built built = ts::build(ts::dng::document(Endian::Big, format));
        const std::uint64_t link = format == Format::Classic ? 4 : 8;
        const std::size_t size = built.offset_size();
        for (const std::uint64_t offset :
             {std::uint64_t{built.bytes.size()}, std::uint64_t{built.bytes.size() - 1},
              std::uint64_t{0xFFFFFFFF}, std::uint64_t{2}}) {
            if (size == 4 && offset > 0xFFFFFFFF) {
                continue;
            }
            SCOPED_TRACE(offset);
            built.put(link, offset, size);
            expect_corrupt(inspect(built), "raw.tiff.ifd");
        }
        if (format == Format::BigTiff) {
            built.put(link, 0xFFFFFFFFFFFFFFF0ull, 8);
            expect_corrupt(inspect(built), "raw.tiff.ifd");
        }
    }
}

TEST(TiffCorrupt, SubIfdAndExifOffsetsOutOfBounds) {
    Built built = ts::build(ts::dng::document());
    const std::uint64_t sub_array = built.value("ifd0", t::kSubIfds);
    Built sub = built;
    sub.put(sub_array + 4, sub.bytes.size() + 100, 4);
    expect_corrupt(inspect(sub), "raw.tiff.ifd");
    Built zero = built;
    zero.put(sub_array, 0, 4);  // SubIFD offset 0 points into the header
    expect_corrupt(inspect(zero), "raw.tiff.ifd");
    Built exif = built;
    exif.put(built.value("ifd0", t::kExifIfd), 0xFFFFFFFF, 4);
    expect_corrupt(inspect(exif), "raw.tiff.ifd");
}

TEST(TiffCorrupt, TruncatedIfdTable) {
    const Built built = ts::build(ts::dng::document());
    const std::uint64_t ifd0 = built.ifd("ifd0");
    // Cut inside the IFD0 entry table and just before its next-IFD offset.
    for (const std::uint64_t size : {ifd0 + 1, ifd0 + 2 + 12 * 5 + 3, built.next_field("ifd0") + 3}) {
        SCOPED_TRACE(size);
        expect_corrupt(inspect_bytes(std::span(built.bytes).first(static_cast<std::size_t>(size))),
                       "raw.tiff.ifd");
    }
}

TEST(TiffCorrupt, NextIfdLoops) {
    {
        Built built = ts::build(ts::dng::document());
        built.put(built.next_field("ifd0"), built.ifd("ifd0"), 4);  // IFD0 -> IFD0
        expect_corrupt(inspect(built), "raw.tiff.ifd_tree");
    }
    {
        Document doc = ts::dng::document();
        doc.chain.push_back(ts::dng::preview_ifd());
        doc.chain.push_back(ts::dng::preview_ifd());
        Built built = ts::build(doc);
        built.put(built.next_field("ifd2"), built.ifd("ifd1"), 4);  // IFD2 -> IFD1
        expect_corrupt(inspect(built), "raw.tiff.ifd_tree");
    }
    {
        Built built = ts::build(ts::dng::document(Endian::Big, Format::BigTiff));
        built.put(built.next_field("ifd0"), built.ifd("ifd0.subifd1"), 8);  // chain -> already-visited SubIFD
        expect_corrupt(inspect(built), "raw.tiff.ifd_tree");
    }
}

TEST(TiffCorrupt, SubIfdPointingAtAncestorOrSibling) {
    Built built = ts::build(ts::dng::document());
    Built ancestor = built;
    ancestor.put(built.value("ifd0", t::kSubIfds) + 4, built.ifd("ifd0"), 4);
    expect_corrupt(inspect(ancestor), "raw.tiff.ifd_tree");
    Built sibling = built;
    sibling.put(built.value("ifd0", t::kSubIfds) + 4, built.ifd("ifd0.subifd0"), 4);
    expect_corrupt(inspect(sibling), "raw.tiff.ifd_tree");
    Built exif = built;
    exif.put(built.value("ifd0", t::kExifIfd), built.ifd("ifd0.subifd0"), 4);
    expect_corrupt(inspect(exif), "raw.tiff.ifd_tree");
}

TEST(TiffCorrupt, AbsurdEntryCounts) {
    {
        Built built = ts::build(ts::dng::document(Endian::Little, Format::Classic));
        built.put(built.ifd("ifd0"), 0xFFFF, 2);
        expect_corrupt(inspect(built), "raw.tiff.ifd");
        built.put(built.ifd("ifd0"), 0, 2);  // an IFD must have at least one entry
        expect_corrupt(inspect(built), "raw.tiff.ifd");
    }
    {
        Built built = ts::build(ts::dng::document(Endian::Big, Format::BigTiff));
        for (const std::uint64_t count :
             {0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x100000000ull, 4097ull}) {
            SCOPED_TRACE(count);
            built.put(built.ifd("ifd0.subifd0"), count, 8);
            expect_corrupt(inspect(built), "raw.tiff.ifd");
        }
    }
    {
        // Within the source but above the plausibility limit.
        TiffInspectLimits limits;
        limits.max_entries_per_ifd = 8;
        expect_corrupt(inspect(ts::dng::document(), limits), "raw.tiff.ifd");
    }
}

TEST(TiffCorrupt, IfdCountAndDepthLimits) {
    {
        TiffInspectLimits limits;
        limits.max_ifds = 3;  // the reference DNG has 4 IFDs
        expect_corrupt(inspect(ts::dng::document(), limits), "raw.tiff.ifd_tree");
    }
    {
        TiffInspectLimits limits;
        limits.max_ifd_depth = 0;  // SubIFDs and the Exif IFD are at depth 1
        expect_corrupt(inspect(ts::dng::document(), limits), "raw.tiff.ifd_tree");
    }
    {
        // Deep SubIFD nesting beyond the default depth limit.
        Document doc = ts::dng::document();
        ts::Ifd deepest = ts::dng::preview_ifd();
        for (int i = 0; i < 5; ++i) {
            ts::Ifd parent = ts::dng::preview_ifd();
            parent.sub_ifds = {deepest};
            deepest = parent;
        }
        doc.chain[0].sub_ifds[1] = deepest;
        expect_corrupt(inspect(doc), "raw.tiff.ifd_tree");
    }
    {
        // A SubIFDs tag listing more IFDs than the whole budget.
        Document doc = ts::dng::document();
        TiffInspectLimits limits;
        limits.max_ifds = 1;  // IFD 0 lists two SubIFDs
        expect_corrupt(inspect(doc, limits), "raw.tiff.tag");
    }
}

// ---- Entry values ----------------------------------------------------------------------------

TEST(TiffCorrupt, ValueOffsetNear4GiB) {
    Document doc = ts::dng::document(Endian::Little, Format::Classic);
    ts::dng::find(doc.chain[0], t::kColorMatrix1).offset_override = 0xFFFFFFF0u;  // 72 bytes: ends past 2^32
    expect_corrupt(inspect(doc), "raw.tiff.entry");
}

TEST(TiffCorrupt, ValueRangeBeyondSource) {
    Document doc = ts::dng::document(Endian::Big, Format::Classic);
    ts::Entry& black = ts::dng::find(doc.chain[0].sub_ifds[0], t::kBlackLevel);
    black.type = ts::type_code(TiffFieldType::Double);
    black.count_override = 0xFFFFFFFFu;  // 32 GiB of doubles
    ts::dng::find(doc.chain[0].sub_ifds[0], t::kBlackLevelRepeatDim).integers = {1, 1};
    expect_corrupt(inspect(doc), "raw.tiff.entry");
}

TEST(TiffCorrupt, ValueOffsetAndSizeOverflowNear2To64) {
    {
        Document doc = ts::dng::document(Endian::Little, Format::BigTiff);
        ts::dng::find(doc.chain[0], t::kColorMatrix1).offset_override = 0xFFFFFFFFFFFFFFF0ull;
        expect_corrupt(inspect(doc), "raw.tiff.entry");
    }
    {
        Document doc = ts::dng::document(Endian::Big, Format::BigTiff);
        ts::Entry& black = ts::dng::find(doc.chain[0].sub_ifds[0], t::kBlackLevel);
        black.type = ts::type_code(TiffFieldType::Double);
        black.count_override = 0x2000000000000001ull;  // count * 8 overflows 64 bits
        expect_corrupt(inspect(doc), "raw.tiff.entry");
    }
    {
        Document doc = ts::dng::document(Endian::Little, Format::BigTiff);
        ts::Entry& list = ts::dng::find(doc.chain[0].sub_ifds[0], t::kOpcodeList2);
        list.count_override = 0xFFFFFFFFFFFFFFFFull;
        expect_corrupt(inspect(doc), "raw.tiff.entry");
    }
}

TEST(TiffCorrupt, InterpretedTagWithWrongTypeOrCount) {
    const auto corrupt_with = [](auto&& mutate) {
        Document doc = ts::dng::document();
        mutate(doc);
        expect_corrupt(inspect(doc), "raw.tiff.tag");
    };
    corrupt_with([](Document& d) {
        ts::dng::erase(d.chain[0], t::kColorMatrix1);
        d.chain[0].entries.push_back(ts::ascii_entry(t::kColorMatrix1, "not a matrix"));
    });
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kDngVersion).integers = {1, 6, 0}; });
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kColorMatrix1).rationals.resize(8); });
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kCameraCalibration1).rationals.resize(8); });
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kImageWidth).integers = {1, 2}; });
    corrupt_with([](Document& d) {
        ts::Entry& compression = ts::dng::find(d.chain[0].sub_ifds[0], t::kCompression);
        compression.type = ts::type_code(TiffFieldType::Long);
        compression.integers = {0x10000};  // does not fit the 16-bit Compression value
    });
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kSamplesPerPixel).integers = {0}; });
    corrupt_with([](Document& d) {
        ts::Entry& list = ts::dng::find(d.chain[0].sub_ifds[0], t::kOpcodeList1);
        list.type = ts::type_code(TiffFieldType::Short);
        list.integers = {1, 2};
        list.raw.clear();
    });
}

// Real files (Pentax K-3 Mark III Monochrome DNGs) store BlackLevelRepeatDim as the single value
// 1. That is read as 1 x 1 with a warning; any other single value is still Corrupt.
TEST(TiffCorrupt, SingleValueBlackLevelRepeatDim) {
    {
        Document doc = ts::dng::document();
        ts::Ifd& raw = doc.chain[0].sub_ifds[0];
        ts::dng::find(raw, t::kBlackLevelRepeatDim).integers = {1};
        ts::dng::find(raw, t::kBlackLevel).rationals = {{256, 1}};
        const auto summary = inspect(doc);
        ASSERT_TRUE(summary) << summary.error().describe();
        ASSERT_NE(summary->primary_raw(), nullptr);
        EXPECT_EQ(summary->primary_raw()->black_level_repeat_dim, (std::array<std::uint16_t, 2>{1, 1}));
        EXPECT_EQ(summary->primary_raw()->black_level, std::vector<double>{256.0});
        const bool warned = std::any_of(
            summary->warnings.begin(), summary->warnings.end(),
            [](const TiffWarning& w) { return w.message.find("read as 1 x 1") != std::string::npos; });
        EXPECT_TRUE(warned);
    }
    {
        Document doc = ts::dng::document();
        ts::dng::find(doc.chain[0].sub_ifds[0], t::kBlackLevelRepeatDim).integers = {2};
        expect_corrupt(inspect(doc), "raw.tiff.tag");
    }
}

TEST(TiffCorrupt, InvalidNumericValues) {
    const auto corrupt_with = [](auto&& mutate) {
        Document doc = ts::dng::document();
        mutate(doc);
        expect_corrupt(inspect(doc), "raw.tiff.tag");
    };
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kAsShotNeutral).rationals[1] = {1, 0}; });
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kAsShotNeutral).rationals[1] = {0, 1}; });
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0], t::kColorMatrix2).rationals[4] = {7, 0}; });
    corrupt_with([](Document& d) {
        ts::dng::find(d.chain[0].sub_ifds[0], t::kNoiseProfile).reals = {
            std::numeric_limits<double>::quiet_NaN(), 1e-7};
    });
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kNoiseProfile).reals = {0.0, 1e-7}; });
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kNoiseProfile).reals = {1e-5, 1e-7, 1}; });
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kDefaultCropSize).integers = {0, 10}; });
    corrupt_with([](Document& d) {
        ts::dng::find(d.chain[0].sub_ifds[0], t::kCfaRepeatPatternDim).integers = {0, 2};
    });
    corrupt_with([](Document& d) {
        // JXLDistance is a FLOAT; a DOUBLE outside float range cannot be represented.
        d.chain[0].sub_ifds[0].entries.push_back(ts::double_entry(t::kJxlDistance, {1e300}));
    });
}

TEST(TiffCorrupt, InconsistentRelatedTags) {
    const auto corrupt_with = [](auto&& mutate, std::string_view where) {
        Document doc = ts::dng::document();
        mutate(doc);
        expect_corrupt(inspect(doc), where);
    };
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kCfaPattern).integers = {0, 1, 2}; },
        "raw.tiff.tag");
    corrupt_with(
        [](Document& d) {
            ts::dng::find(d.chain[0].sub_ifds[0], t::kActiveArea).integers = {0, 0, ts::dng::kRawHeight + 1,
                                                                              10};
        },
        "raw.tiff.tag");
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kActiveArea).integers = {10, 0, 10, 10}; },
        "raw.tiff.tag");
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kBlackLevel).rationals.resize(3); },
        "raw.tiff.tag");
    corrupt_with([](Document& d) { ts::dng::find(d.chain[0].sub_ifds[0], t::kWhiteLevel).integers = {1, 2}; },
                 "raw.tiff.tag");
    // Colour tags that disagree with the primary raw IFD's three colour planes.
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0], t::kColorMatrix1).rationals.assign(12, {1, 2}); },
        "raw.dng.color");
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0], t::kForwardMatrix1).rationals.assign(6, {1, 2}); },
        "raw.dng.color");
    corrupt_with(
        [](Document& d) { ts::dng::find(d.chain[0], t::kAsShotNeutral).rationals.assign(4, {1, 2}); },
        "raw.dng.color");
    corrupt_with(
        [](Document& d) {
            ts::dng::find(d.chain[0].sub_ifds[0], t::kNoiseProfile).reals = {1e-5, 1e-7, 1e-5, 1e-7};
        },
        "raw.dng.color");
}

TEST(TiffCorrupt, DuplicateInterpretedTag) {
    Document doc = ts::dng::document();
    doc.chain[0].sub_ifds[0].entries.push_back(ts::long_entry(t::kImageWidth, {16}));
    expect_corrupt(inspect(doc), "raw.tiff.tag");
}

// ---- Opcode lists inside a file ---------------------------------------------------------------

TEST(TiffCorrupt, OpcodeListFaults) {
    const auto corrupt_with = [](std::vector<std::byte> payload) {
        for (const Endian endian : {Endian::Little, Endian::Big}) {
            Document doc = ts::dng::document(endian, Format::Classic);
            ts::dng::find(doc.chain[0].sub_ifds[0], t::kOpcodeList3).raw = payload;
            expect_corrupt(inspect(doc), "raw.dng.opcode_list");
        }
    };
    corrupt_with({});                                                               // zero-length list
    corrupt_with(ts::opcode_list({{1, 0x01030000, 0, ts::filler(8)}}, 3));          // count exceeds payload
    corrupt_with(ts::opcode_list({{1, 0x01030000, 0, ts::filler(8)}}, 0xFFFFFFF));  // absurd count
    ts::OpcodeSpec op{9, 0x01030000, 0, ts::filler(8)};
    op.parameter_bytes_override = 4096;
    corrupt_with(ts::opcode_list({op}));  // parameters exceed payload
    auto trailing = ts::opcode_list({{6, 0x01030000, 0, ts::filler(16)}});
    trailing.push_back(std::byte{1});
    corrupt_with(trailing);  // trailing bytes
}

// ---- Limits ----------------------------------------------------------------------------------

TEST(TiffLimits, TagValueLargerThanLimit) {
    TiffInspectLimits limits;
    limits.max_value_bytes = 64;  // ColorMatrix1 decodes to 9 x 8 = 72 bytes
    expect_error(inspect(ts::dng::document(), limits), ErrorCode::LimitExceeded, "raw.tiff.limits");
}

TEST(TiffLimits, DecodedSizeIsCharged) {
    // 40 BYTE values are stored in 40 bytes but decode to 40 x 8 = 320 bytes.
    Document doc = ts::dng::document();
    ts::Entry& black = ts::dng::find(doc.chain[0].sub_ifds[0], t::kBlackLevel);
    black = ts::byte_entry(t::kBlackLevel, std::vector<std::uint64_t>(40, 64));
    TiffInspectLimits limits;
    limits.max_value_bytes = 256;  // above every other value of the reference DNG (at most 212 bytes)
    expect_error(inspect(doc, limits), ErrorCode::LimitExceeded, "raw.tiff.limits");
}

TEST(TiffLimits, TotalValueBudget) {
    TiffInspectLimits limits;
    limits.max_total_value_bytes = 256;  // the reference DNG needs far more in total
    expect_error(inspect(ts::dng::document(), limits), ErrorCode::LimitExceeded, "raw.tiff.limits");
    limits.max_total_value_bytes = std::uint64_t{64} << 20;
    EXPECT_TRUE(inspect(ts::dng::document(), limits));
}

TEST(TiffLimits, OpcodesPerList) {
    TiffInspectLimits limits;
    limits.max_opcodes_per_list = 2;  // OpcodeList3 of the reference DNG has three opcodes
    expect_error(inspect(ts::dng::document(), limits), ErrorCode::LimitExceeded, "raw.dng.opcode_list");
    limits.max_opcodes_per_list = 3;
    EXPECT_TRUE(inspect(ts::dng::document(), limits));
}

TEST(TiffLimits, InvalidLimitsAreRejected) {
    const Built built = ts::build(ts::dng::document());
    for (int which = 0; which < 4; ++which) {
        TiffInspectLimits limits;
        if (which == 0) {
            limits.max_ifds = 0;
        } else if (which == 1) {
            limits.max_entries_per_ifd = 0;
        } else if (which == 2) {
            limits.max_value_bytes = 0;
        } else {
            limits.max_total_value_bytes = 0;
        }
        expect_error(inspect(built, limits), ErrorCode::InvalidArgument, "raw.tiff.limits");
    }
}

// ---- Source failures -------------------------------------------------------------------------

// Serves `bytes` but fails, lies or throws on the read that starts at `trigger_offset`.
class FaultySource final : public RandomAccessSource {
public:
    enum class Fault { Error, ShortRead, ThrowBadAlloc, ThrowRuntime };

    FaultySource(std::span<const std::byte> bytes, std::uint64_t trigger_offset, Fault fault)
        : inner_(bytes), trigger_offset_(trigger_offset), fault_(fault) {}

    std::uint64_t size() const noexcept override { return inner_.size(); }

    Expected<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> destination) override {
        if (offset == trigger_offset_) {
            switch (fault_) {
                case Fault::Error:
                    return fail(ErrorCode::IoError, "device not ready", "core.file.read", 21);
                case Fault::ShortRead: {
                    auto read = inner_.read_at(offset, destination.first(destination.size() / 2));
                    return read;
                }
                case Fault::ThrowBadAlloc:
                    throw std::bad_alloc();
                case Fault::ThrowRuntime:
                    throw std::runtime_error("boom");
            }
        }
        return inner_.read_at(offset, destination);
    }

private:
    MemorySource inner_;
    std::uint64_t trigger_offset_;
    Fault fault_;
};

TEST(TiffSource, ErrorsArePassedThroughUnchanged) {
    const Built built = ts::build(ts::dng::document());
    for (const std::uint64_t offset :
         {std::uint64_t{0}, built.ifd("ifd0"), built.value("ifd0", t::kColorMatrix1)}) {
        SCOPED_TRACE(offset);
        FaultySource source(built.span(), offset, FaultySource::Fault::Error);
        auto result = inspect_tiff(source);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::IoError);
        EXPECT_EQ(result.error().where, "core.file.read");
        EXPECT_EQ(result.error().native_code, 21);
    }
}

TEST(TiffSource, ShortReadViolatesTheContract) {
    const Built built = ts::build(ts::dng::document());
    for (const std::uint64_t offset :
         {std::uint64_t{0}, built.ifd("ifd0.subifd0") + 2, built.value("ifd0.subifd0", t::kOpcodeList2)}) {
        SCOPED_TRACE(offset);
        FaultySource source(built.span(), offset, FaultySource::Fault::ShortRead);
        expect_error(inspect_tiff(source), ErrorCode::IoError, "raw.tiff.source");
    }
}

TEST(TiffSource, ExceptionsNeverEscape) {
    const Built built = ts::build(ts::dng::document());
    {
        FaultySource source(built.span(), built.ifd("ifd0") + 2, FaultySource::Fault::ThrowBadAlloc);
        Expected<TiffSummary> result = TiffSummary{};
        EXPECT_NO_THROW(result = inspect_tiff(source));
        expect_error(result, ErrorCode::OutOfMemory, "raw.tiff.inspect");
    }
    {
        FaultySource source(built.span(), 0, FaultySource::Fault::ThrowRuntime);
        Expected<TiffSummary> result = TiffSummary{};
        EXPECT_NO_THROW(result = inspect_tiff(source));
        expect_error(result, ErrorCode::Internal, "raw.tiff.inspect");
    }
}

}  // namespace
}  // namespace lucent::raw
