// Deterministic mutation tests for inspect_tiff().
//
// A valid synthetic DNG is mutated many times with a fixed-seed PRNG (single-byte, bit-flip,
// multi-byte, "interesting value" and truncation mutations biased towards the header and IFD
// tables), plus exhaustive sweeps over every structural byte and every prefix length. Each
// mutated stream must be inspected without crashing or throwing, and must yield either a
// summary that satisfies the documented invariants, or ErrorCode::Corrupt / Unsupported.
// Reproduce a failure from the reported variant, seed and iteration.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
using ts::Built;
using ts::Endian;
using ts::Format;

// SplitMix64 (Steele, Lea and Flood, 2014): a tiny, fully specified generator, so the mutation
// sequence is identical with every compiler and standard library (std:: distributions are not).
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform enough in [0, n) for test-case selection (modulo bias is irrelevant here).
    std::uint64_t below(std::uint64_t n) { return next() % n; }

private:
    std::uint64_t state_;
};

struct Outcomes {
    std::size_t ok = 0;
    std::size_t corrupt = 0;
    std::size_t unsupported = 0;
};

void put(std::vector<std::byte>& bytes, std::uint64_t offset, std::uint64_t value, std::size_t width,
         Endian endian) {
    for (std::size_t i = 0; i < width && offset + i < bytes.size(); ++i) {
        const std::size_t shift = endian == Endian::Little ? 8 * i : 8 * (width - 1 - i);
        bytes[static_cast<std::size_t>(offset + i)] = static_cast<std::byte>((value >> shift) & 0xFF);
    }
}

class Mutator {
public:
    Mutator(const Built& base, std::uint64_t seed) : base_(base), rng_(seed) {
        for (const auto& ifd : base.ifd_offsets) {
            ifd_offsets_.push_back(ifd.second);
        }
    }

    std::vector<std::byte> next() {
        std::vector<std::byte> bytes = base_.bytes;
        const std::uint64_t kind = rng_.below(100);
        if (kind < 20) {
            bytes[any(bytes)] = random_byte();
        } else if (kind < 40) {
            const std::size_t at = structural(bytes);
            bytes[at] ^= static_cast<std::byte>(1u << rng_.below(8));
        } else if (kind < 55) {
            const std::size_t at = structural(bytes);
            const std::size_t length = 2 + static_cast<std::size_t>(rng_.below(7));
            for (std::size_t i = 0; i < length && at + i < bytes.size(); ++i) {
                bytes[at + i] = random_byte();
            }
        } else if (kind < 75) {
            put(bytes, structural(bytes), interesting(bytes.size()), width(), base_.endian);
        } else if (kind < 85) {
            put(bytes, any(bytes), interesting(bytes.size()), width(), base_.endian);
        } else if (kind < 95) {
            const std::size_t mutations = 2 + static_cast<std::size_t>(rng_.below(5));
            for (std::size_t i = 0; i < mutations; ++i) {
                bytes[any(bytes)] = random_byte();
            }
        } else {
            bytes.resize(static_cast<std::size_t>(rng_.below(bytes.size())));
        }
        return bytes;
    }

private:
    std::byte random_byte() { return static_cast<std::byte>(rng_.below(256)); }
    std::size_t width() { return std::array<std::size_t, 3>{2, 4, 8}[rng_.below(3)]; }
    std::size_t any(const std::vector<std::byte>& bytes) {
        return static_cast<std::size_t>(rng_.below(bytes.size()));
    }

    // A position inside the header or an IFD table (count, entries, next offset).
    std::size_t structural(const std::vector<std::byte>& bytes) {
        const auto& [begin, end] = base_.structure[rng_.below(base_.structure.size())];
        const std::uint64_t at = begin + rng_.below(end - begin);
        return static_cast<std::size_t>(std::min<std::uint64_t>(at, bytes.size() - 1));
    }

    std::uint64_t interesting(std::uint64_t size) {
        const std::array<std::uint64_t, 22> values{0,
                                                   1,
                                                   2,
                                                   0x7F,
                                                   0x80,
                                                   0xFF,
                                                   0x7FFF,
                                                   0x8000,
                                                   0xFFFF,
                                                   4097,
                                                   0x7FFFFFFF,
                                                   0x80000000,
                                                   0xFFFFFFF0,
                                                   0xFFFFFFFF,
                                                   0x100000000ull,
                                                   0x7FFFFFFFFFFFFFFFull,
                                                   0xFFFFFFFFFFFFFFF0ull,
                                                   0xFFFFFFFFFFFFFFFFull,
                                                   size - 1,
                                                   size,
                                                   size + 1,
                                                   0};
        const std::uint64_t pick = rng_.below(values.size() + 1);
        if (pick == values.size()) {
            // An existing IFD offset: creates loops, shared IFDs and misplaced pointers.
            return ifd_offsets_[rng_.below(ifd_offsets_.size())];
        }
        return values[pick];
    }

    const Built& base_;
    SplitMix64 rng_;
    std::vector<std::uint64_t> ifd_offsets_;
};

// Checks the documented invariants of a successful inspection of `bytes`.
void expect_invariants(const TiffSummary& s, std::uint64_t size, const TiffInspectLimits& limits,
                       const std::string& context) {
    SCOPED_TRACE(context);
    ASSERT_FALSE(s.ifds.empty());
    EXPECT_LE(s.ifds.size(), limits.max_ifds);
    EXPECT_EQ(s.source_size, size);
    EXPECT_LE(s.warnings.size(), limits.max_warnings);
    const auto location_ok = [&](const std::optional<TiffValueLocation>& location) {
        if (location) {
            EXPECT_LE(location->offset, size);
            EXPECT_LE(location->byte_count, size - location->offset);
        }
    };
    for (std::size_t i = 0; i < s.ifds.size(); ++i) {
        const TiffIfdSummary& ifd = s.ifds[i];
        EXPECT_LT(ifd.offset, size);
        EXPECT_LE(ifd.depth, limits.max_ifd_depth);
        EXPECT_GE(ifd.entry_count, 1u);
        EXPECT_LE(ifd.entry_count, limits.max_entries_per_ifd);
        if (ifd.parent) {
            EXPECT_LT(*ifd.parent, i);
        } else {
            EXPECT_EQ(ifd.kind, TiffIfdKind::Main);
        }
        location_ok(ifd.linearization_table);
        location_ok(ifd.profile_gain_table_map);
        for (const auto& list : ifd.opcode_lists) {
            location_ok(list);
        }
        for (const DngOpcode& op : ifd.opcodes) {
            const auto& list = ifd.opcode_lists[static_cast<std::size_t>(op.list) - 1];
            ASSERT_TRUE(list);
            EXPECT_LE(op.parameter_offset + op.parameter_bytes, list->byte_count);
        }
        if (ifd.cfa_repeat_dim && !ifd.cfa_pattern.empty()) {
            EXPECT_EQ(ifd.cfa_pattern.size(),
                      std::size_t{(*ifd.cfa_repeat_dim)[0]} * (*ifd.cfa_repeat_dim)[1]);
        }
    }
    if (s.primary_raw_ifd) {
        EXPECT_LT(*s.primary_raw_ifd, s.ifds.size());
        EXPECT_EQ(s.raw_ifd_candidates.size(), 1u);
    }
    if (s.dng) {
        location_ok(s.dng->profile_gain_table_map2);
        for (const auto& data : s.dng->color.illuminant_data) {
            location_ok(data);
        }
    }
}

// Inspects one mutated stream and records its outcome; any other outcome is a test failure.
void check_one(const std::vector<std::byte>& bytes, Outcomes& outcomes, const std::string& context) {
    const TiffInspectLimits limits;
    MemorySource source(bytes);
    Expected<TiffSummary> result = TiffSummary{};
    ASSERT_NO_THROW(result = inspect_tiff(source, limits)) << context;
    if (result) {
        ++outcomes.ok;
        expect_invariants(*result, bytes.size(), limits, context);
        return;
    }
    const Error& error = result.error();
    if (error.code == ErrorCode::Corrupt) {
        ++outcomes.corrupt;
    } else if (error.code == ErrorCode::Unsupported) {
        ++outcomes.unsupported;
    } else {
        ADD_FAILURE() << context << ": unexpected outcome " << error.describe();
    }
    EXPECT_EQ(error.where.rfind("raw.", 0), 0u) << context << ": " << error.describe();
}

void run_random_mutations(Endian endian, Format format, std::uint64_t seed, std::size_t iterations) {
    const Built base = ts::build(ts::dng::document(endian, format));
    {
        MemorySource source(base.span());
        ASSERT_TRUE(inspect_tiff(source)) << "the unmutated base stream must be valid";
    }
    Mutator mutator(base, seed);
    Outcomes outcomes;
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::vector<std::byte> bytes = mutator.next();
        check_one(bytes, outcomes, "seed " + std::to_string(seed) + " iteration " + std::to_string(i));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
    ::testing::Test::RecordProperty("ok", static_cast<int>(outcomes.ok));
    ::testing::Test::RecordProperty("corrupt", static_cast<int>(outcomes.corrupt));
    ::testing::Test::RecordProperty("unsupported", static_cast<int>(outcomes.unsupported));
    EXPECT_EQ(outcomes.ok + outcomes.corrupt + outcomes.unsupported, iterations);
    // The fixed seed must exercise both acceptance and rejection paths.
    EXPECT_GT(outcomes.ok, iterations / 20);
    EXPECT_GT(outcomes.corrupt, iterations / 20);
}

TEST(TiffMutation, RandomMutationsClassicLittleEndian) {
    run_random_mutations(Endian::Little, Format::Classic, 0x4C55434E54000001ull, 6000);
}

TEST(TiffMutation, RandomMutationsClassicBigEndian) {
    run_random_mutations(Endian::Big, Format::Classic, 0x4C55434E54000002ull, 2000);
}

TEST(TiffMutation, RandomMutationsBigTiff) {
    run_random_mutations(Endian::Little, Format::BigTiff, 0x4C55434E54000003ull, 2000);
}

TEST(TiffMutation, EveryStructuralByte) {
    for (const Format format : {Format::Classic, Format::BigTiff}) {
        const Built base = ts::build(ts::dng::document(Endian::Big, format));
        Outcomes outcomes;
        std::size_t cases = 0;
        for (const auto& [begin, end] : base.structure) {
            for (std::uint64_t at = begin; at < end; ++at) {
                for (const int variant : {0, 1, 2}) {
                    std::vector<std::byte> bytes = base.bytes;
                    std::byte& b = bytes[static_cast<std::size_t>(at)];
                    b = variant == 0   ? std::byte{0x00}
                        : variant == 1 ? std::byte{0xFF}
                                       : (b ^ std::byte{0x80});
                    check_one(bytes, outcomes,
                              std::string(format == Format::Classic ? "classic" : "BigTIFF") + " byte " +
                                  std::to_string(at) + " variant " + std::to_string(variant));
                    ++cases;
                }
            }
        }
        EXPECT_EQ(outcomes.ok + outcomes.corrupt + outcomes.unsupported, cases);
        EXPECT_GT(cases, 1000u);
    }
}

TEST(TiffMutation, EveryPrefixLength) {
    for (const Format format : {Format::Classic, Format::BigTiff}) {
        const Built base = ts::build(ts::dng::document(Endian::Little, format));
        Outcomes outcomes;
        for (std::size_t length = 0; length < base.bytes.size(); ++length) {
            const std::vector<std::byte> bytes(base.bytes.begin(),
                                               base.bytes.begin() + static_cast<std::ptrdiff_t>(length));
            check_one(bytes, outcomes, "prefix " + std::to_string(length));
        }
        EXPECT_EQ(outcomes.unsupported, 0u);  // every prefix keeps a valid TIFF signature or is too short
        EXPECT_GT(outcomes.corrupt, 0u);
    }
}

}  // namespace
}  // namespace lucent::raw
