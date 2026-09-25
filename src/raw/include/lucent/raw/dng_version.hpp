#pragma once

// Four-tier DNG specification version numbers.

#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace lucent::raw {

// A DNG version such as 1.7.1.0, most significant tier first.
//
// DNGVersion (tag 50706) and DNGBackwardVersion (tag 50707) store the four tiers as four BYTE
// values. Each opcode in an opcode list stores the version in which its opcode ID was defined
// as one big-endian 32-bit integer, e.g. 0x01030000 for 1.3.0.0 (DNG 1.7.1.0, chapter 7).
// Versions order lexicographically by tier, which matches the specification's intent.
struct DngVersion {
    std::array<std::uint8_t, 4> parts{};

    // Splits a packed version (first tier in the most significant byte).
    [[nodiscard]] static constexpr DngVersion from_packed(std::uint32_t packed) noexcept {
        return DngVersion{{static_cast<std::uint8_t>(packed >> 24), static_cast<std::uint8_t>(packed >> 16),
                           static_cast<std::uint8_t>(packed >> 8), static_cast<std::uint8_t>(packed)}};
    }

    // Inverse of from_packed().
    [[nodiscard]] constexpr std::uint32_t packed() const noexcept {
        return (std::uint32_t{parts[0]} << 24) | (std::uint32_t{parts[1]} << 16) |
               (std::uint32_t{parts[2]} << 8) | std::uint32_t{parts[3]};
    }

    // Dotted form, e.g. "1.7.1.0".
    [[nodiscard]] std::string to_string() const {
        std::string text;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) {
                text += '.';
            }
            text += std::to_string(parts[i]);
        }
        return text;
    }

    friend constexpr bool operator==(const DngVersion&, const DngVersion&) = default;
    friend constexpr auto operator<=>(const DngVersion&, const DngVersion&) = default;
};

// The DNG specification revision that Lucent's TIFF/DNG inspector was written against
// (Adobe, "Digital Negative (DNG) Specification", version 1.7.1.0, September 2023). Readers must
// not claim to process files whose DNGBackwardVersion, or opcodes whose version, exceeds this.
inline constexpr DngVersion kDngSpecificationVersion{{1, 7, 1, 0}};

}  // namespace lucent::raw
