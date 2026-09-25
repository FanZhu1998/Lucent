#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/libraw_decoder.hpp>

namespace lucent::raw {

namespace {

constexpr std::string_view kWhereAdd = "raw.registry.add";
constexpr std::string_view kWhereSelect = "raw.registry.select";

bool is_valid_id(std::string_view id) noexcept {
    if (id.empty()) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

}  // namespace

Status DecoderRegistry::add(std::unique_ptr<RawDecoder> decoder, std::int32_t priority) {
    if (!decoder) {
        return fail(ErrorCode::InvalidArgument, "cannot register a null decoder", std::string(kWhereAdd));
    }
    const std::string_view id = decoder->id();
    if (!is_valid_id(id)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("decoder id '{}' must be lower-case ASCII letters, digits, '-', '_' or '.'", id),
            std::string(kWhereAdd));
    }
    if (find(id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, std::format("a decoder with id '{}' is already registered", id),
                    std::string(kWhereAdd));
    }
    try {
        // upper_bound keeps registration order among equal priorities.
        const auto position = std::upper_bound(entries_.begin(), entries_.end(), priority,
                                               [](std::int32_t p, const Entry& e) { return p < e.priority; });
        entries_.insert(position, Entry{priority, std::move(decoder)});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot grow the decoder registry", std::string(kWhereAdd));
    }
    return ok();
}

const RawDecoder* DecoderRegistry::find(std::string_view id) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.decoder->id() == id) {
            return entry.decoder.get();
        }
    }
    return nullptr;
}

std::vector<const RawDecoder*> DecoderRegistry::decoders() const {
    std::vector<const RawDecoder*> result;
    result.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        result.push_back(entry.decoder.get());
    }
    return result;
}

Expected<DecoderRegistry::Selection> DecoderRegistry::select(const std::filesystem::path& path,
                                                             const ProbeOptions& options,
                                                             core::CancellationToken cancel) const {
    if (entries_.empty()) {
        return fail(ErrorCode::Unsupported, "no RAW decoder is registered", std::string(kWhereSelect));
    }
    std::string declined;
    for (const Entry& entry : entries_) {
        if (auto status = cancel.check(kWhereSelect); !status) {
            return Unexpected(std::move(status.error()));
        }
        auto probe = entry.decoder->probe(path, options, cancel);
        if (probe) {
            return Selection{entry.decoder.get(), std::move(*probe)};
        }
        if (probe.error().code != ErrorCode::Unsupported) {
            return Unexpected(std::move(probe.error()));
        }
        if (!declined.empty()) {
            declined += "; ";
        }
        declined += std::format("{}: {}", entry.decoder->id(), probe.error().message);
    }
    return fail(ErrorCode::Unsupported, std::format("no registered decoder supports the file ({})", declined),
                std::string(kWhereSelect));
}

Expected<DecodedRaw> DecoderRegistry::decode(const std::filesystem::path& path, const DecodeOptions& options,
                                             core::CancellationToken cancel) const {
    ProbeOptions probe_options;
    probe_options.subimage = options.subimage;
    probe_options.limits = options.limits;
    probe_options.describe_subimages = false;
    auto selection = select(path, probe_options, cancel);
    if (!selection) {
        return Unexpected(std::move(selection.error()));
    }
    return selection->decoder->decode(path, options, std::move(cancel));
}

Expected<DecoderRegistry> create_default_registry() {
    DecoderRegistry registry;
    auto libraw = create_libraw_decoder();
    if (!libraw) {
        return Unexpected(std::move(libraw.error()));
    }
    if (auto added = registry.add(std::move(*libraw), kLibRawDecoderPriority); !added) {
        return Unexpected(std::move(added.error()));
    }
    return registry;
}

}  // namespace lucent::raw
