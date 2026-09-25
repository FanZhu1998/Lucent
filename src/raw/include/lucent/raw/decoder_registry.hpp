#pragma once

// Internal registry of RAW decoder adapters (blueprint section 3: "Use an internal adapter
// registry first ... Avoid a general third-party native-plugin system in v1").
//
// Adapters are tried in priority order. Selection for a path probes each adapter in turn and
// takes the first that recognises the file; an adapter that answers ErrorCode::Unsupported
// ("not my format") passes the file on to the next one, while any other failure (the file is
// missing, corrupt, too large, cancelled, ...) ends the selection with that error, because a
// different adapter would not make such a file trustworthy.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::raw {

// Priorities used by create_default_registry(); lower values are tried first.
inline constexpr std::int32_t kLibRawDecoderPriority = 100;

class DecoderRegistry {
public:
    DecoderRegistry() = default;
    DecoderRegistry(DecoderRegistry&&) noexcept = default;
    DecoderRegistry& operator=(DecoderRegistry&&) noexcept = default;
    DecoderRegistry(const DecoderRegistry&) = delete;
    DecoderRegistry& operator=(const DecoderRegistry&) = delete;
    ~DecoderRegistry() = default;

    // Registers an adapter. Lower `priority` values are tried first; adapters with equal
    // priority keep their registration order.
    // Errors ("raw.registry.add"): InvalidArgument for a null decoder or an id that is empty or
    // not lower-case ASCII letters, digits, '-', '_' or '.'; AlreadyExists for a duplicate id.
    [[nodiscard]] Status add(std::unique_ptr<RawDecoder> decoder, std::int32_t priority);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    // The adapter with this id, or nullptr.
    [[nodiscard]] const RawDecoder* find(std::string_view id) const noexcept;

    // Adapters in the order they are tried. The pointers stay valid for the registry's lifetime.
    [[nodiscard]] std::vector<const RawDecoder*> decoders() const;

    struct Selection {
        const RawDecoder* decoder = nullptr;  // never null in a successful result
        ProbeResult probe;                    // that adapter's probe of the file
    };

    // Probes the adapters in priority order and returns the first successful probe.
    // Errors ("raw.registry.select"): Unsupported when the registry is empty or every adapter
    // answered Unsupported (the message lists them); otherwise the first adapter error that is
    // not Unsupported, unchanged.
    [[nodiscard]] Expected<Selection> select(const std::filesystem::path& path, const ProbeOptions& options,
                                             core::CancellationToken cancel) const;

    // Convenience: select() with the same subimage and limits, then decode with the selected
    // adapter.
    [[nodiscard]] Expected<DecodedRaw> decode(const std::filesystem::path& path, const DecodeOptions& options,
                                              core::CancellationToken cancel) const;

private:
    struct Entry {
        std::int32_t priority = 0;
        std::unique_ptr<RawDecoder> decoder;
    };
    std::vector<Entry> entries_;  // sorted by priority, stable
};

// A registry holding every adapter of this build (W1: the LibRaw adapter). Fails only when an
// adapter cannot be constructed (OutOfMemory).
[[nodiscard]] Expected<DecoderRegistry> create_default_registry();

}  // namespace lucent::raw
