#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <lucent/assets/mode_key.hpp>

namespace lucent::assets {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// Enum identifiers of the contract use '_' ("dual_pixel_component"); key components use '-'.
std::string dashed(std::string_view text) {
    std::string out(text);
    std::replace(out.begin(), out.end(), '_', '-');
    return out;
}

}  // namespace

std::string slug(std::string_view text) {
    std::string out;
    bool pending_dash = false;
    for (const char raw : text) {
        char c = raw;
        if (c == '*') {
            // A star is part of some mode names (Nikon "HE*"): spelled out so that it survives.
            out += out.empty() ? "star" : "-star";
            pending_dash = true;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!keep) {
            pending_dash = !out.empty();
            continue;
        }
        if (pending_dash) {
            out.push_back('-');
            pending_dash = false;
        }
        out.push_back(c);
    }
    return out;
}

std::string canonical_make(std::string_view make) {
    const std::string m = lower(make);
    // Ordered: the first matching prefix wins.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 18> kAliases{{
        {"nikon", "nikon"},
        {"ricoh imaging", "pentax"},
        {"pentax", "pentax"},
        {"asahi", "pentax"},
        {"arashi vision", "insta360"},
        {"insta360", "insta360"},
        {"om digital", "om-system"},
        {"om system", "om-system"},
        {"olympus", "olympus"},
        {"leica", "leica"},
        {"fujifilm", "fujifilm"},
        {"sony", "sony"},
        {"canon", "canon"},
        {"dji", "dji"},
        {"hasselblad", "hasselblad"},
        {"panasonic", "panasonic"},
        {"samsung", "samsung"},
        {"sigma", "sigma"},
    }};
    for (const auto& [prefix, id] : kAliases) {
        if (starts_with(m, prefix)) {
            return std::string(id);
        }
    }
    return slug(make);
}

std::string canonical_model(std::string_view canonical, std::string_view model) {
    std::string s = slug(model);
    // A model string that repeats the maker ("Canon EOS R5", "PENTAX K-3 Mark III",
    // "Insta360 One2") is reduced to the model proper.
    const std::string prefix = std::string(canonical) + "-";
    if (s.size() > prefix.size() && starts_with(s, prefix)) {
        s.erase(0, prefix.size());
    }
    return s;
}

std::uint32_t bits_for_white_level(double white) noexcept {
    if (!(white > 0.0) || !std::isfinite(white)) {
        return 0;
    }
    std::uint32_t bits = 0;
    while (bits < 32 && std::exp2(static_cast<double>(bits)) < white + 1.0) {
        ++bits;
    }
    return bits;
}

std::string ModeKey::to_string() const {
    std::string text = "mk" + std::to_string(version);
    for (const std::string* part : {&make, &model, &container, &compression}) {
        text += "|" + *part;
    }
    text += "|" + area_text();
    text += "|" + bits + "bit";
    for (const std::string* part : {&representation, &sample, &view}) {
        text += "|" + *part;
    }
    text += "|" + subimage_role + ":" + std::to_string(subimage_index) + "/" + std::to_string(subimage_count);
    return text;
}

std::string ModeKey::model_key() const {
    return make + "|" + model;
}

std::string ModeKey::area_text() const {
    return std::to_string(area_width) + "x" + std::to_string(area_height);
}

ModeKey derive_mode_key(const raw::RawMetadata& metadata) {
    ModeKey key;
    const raw::CameraIdentity& camera = metadata.camera;
    const std::string_view model =
        !camera.model.empty() ? std::string_view(camera.model) : std::string_view(camera.normalized_model);
    const std::string_view make =
        !camera.make.empty() ? std::string_view(camera.make) : std::string_view(camera.normalized_make);
    key.make = canonical_make(make);
    // Ricoh Imaging writes one maker string for its Pentax and Ricoh lines; the model decides.
    if (key.make == "pentax" && !starts_with(lower(model), "pentax") && starts_with(lower(make), "ricoh")) {
        key.make = "ricoh";
    }
    key.model = canonical_model(key.make, model);
    key.container = std::string(raw::to_string(metadata.format.container));
    key.compression = slug(metadata.format.compression.name);
    key.compression_lossiness = std::string(raw::to_string(metadata.format.compression.lossiness));
    key.area_width = static_cast<std::uint32_t>(std::max(0, metadata.geometry.visible_area.width));
    key.area_height = static_cast<std::uint32_t>(std::max(0, metadata.geometry.visible_area.height));

    const raw::RepresentationInfo& rep = metadata.representation;
    if (rep.sample_encoding == raw::SampleEncoding::Float32) {
        key.bits = "f32";
        key.bits_provenance = metadata.dng ? "dng_tag" : "lucent_rule";
    } else if (metadata.dng && !metadata.dng->raw_ifd.bits_per_sample.empty()) {
        const auto& bps = metadata.dng->raw_ifd.bits_per_sample;
        key.bits = std::to_string(*std::max_element(bps.begin(), bps.end()));
        key.bits_provenance = "dng_tag";
    } else {
        key.bits = std::to_string(bits_for_white_level(metadata.levels.white));
        key.bits_provenance = "lucent_rule";
    }

    switch (rep.kind) {
        case raw::RawRepresentation::Mosaic: {
            const image::CfaPattern cfa = raw::cfa_at_visible_origin(metadata);
            key.representation = "mosaic:" + (cfa.empty() ? std::string("unknown") : cfa.to_string());
            break;
        }
        case raw::RawRepresentation::LinearColor:
            key.representation = "linear-rgb";
            break;
        case raw::RawRepresentation::Monochrome:
            key.representation = "monochrome";
            break;
    }
    key.sample = std::string(raw::to_string(rep.sample_encoding));
    key.view = dashed(raw::to_string(metadata.projection.layout));
    key.subimage_role = dashed(raw::to_string(metadata.subimage_role));
    key.subimage_index = metadata.subimage_index;
    key.subimage_count = metadata.subimage_count;
    key.stage = std::string(raw::to_string(metadata.stage));
    return key;
}

}  // namespace lucent::assets
