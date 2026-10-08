#pragma once

// Camera qualification mode key (ADR 0016; blueprint section 3, "Support files and capture modes,
// not just brand names"). Defined by the qualification harness (apps/lucent-qualify) in W10 phase
// 1 and moved here in phase 3 (ADR 0018), because the application derives the same key when it
// probes a file and looks it up in the camera support database (camera_support.hpp): the key the
// harness qualified and the key a library item is assessed under must come from one function.
//
// A mode key names one exact capture mode of one camera model as Lucent's decoder sees it, so that
// qualification evidence is attached to "Sony ILCE-7M4, lossless compressed size M" and never to
// "Sony" or ".ARW". Every component is derived from the decoded-RAW contract (decoded_raw.hpp),
// never from the file extension:
//
//   make         canonical maker id from CameraIdentity::make (alias table below), lower case
//   model        CameraIdentity::model as recorded (the decoder's normalized model when empty),
//                lower-cased; runs of characters other than [a-z0-9] become '-'; a leading copy
//                of the maker's own name ("canon eos r5") is removed
//   container    FormatInfo::container ("cr3", "tiff", "dng", "raf", "orf", "rw2", ...)
//   compression  DataCompression::name slugged like the model ("canon-crx-lossy-c-raw"); the
//                names are stable by contract (decoded_raw.hpp)
//   area         SensorGeometry::visible_area size in stored orientation, "<width>x<height>":
//                distinguishes crop and sensor-area modes (full frame / APS-C / 1.6x crop, Sony
//                L/M/S, drone resolutions)
//   bits         DNG: the largest BitsPerSample of the decoded raw IFD (provenance dng_tag);
//                other containers: the bits needed for the decoder's white level,
//                ceil(log2(white + 1)) (provenance lucent_rule); "f32" for floating-point data
//   representation  "mosaic:<CFA at the visible-area origin>" ("mosaic:RGGB",
//                "mosaic:GGRGGB/GGBGGR/..."), "linear-rgb" or "monochrome"
//   sample       "uint16" or "float32"
//   view         ViewLayout ("single-view", "dual-fisheye-360")
//   subimage     "<role>:<index>/<count>" ("primary:0/1", "primary:0/2" for image 0 of a Dual
//                Pixel RAW)
//
// Canonical text (version 1), fields joined by '|':
//   mk1|make|model|container|compression|area|<bits>bit|representation|sample|view|subimage
// for example
//   mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-view|primary:0/1
//
// The key is bound to the decoder's view: a decoder upgrade that changes, say, the visible area
// changes the key, and the mode then needs qualifying again. That is intended (ADR 0016).
//
// Probe and decode. The harness derives the key from the decoded metadata (stage AfterUnpack); the
// application derives it from the probe it makes at import (stage AfterOpen). The components are
// values a probe already knows for every corpus file (the white level of non-DNG files included):
// tests/qualification asserts that the probe's key equals the qualified key for every fixture.
//
// Versioning: kModeKeyVersion. Any change to a component's derivation or to the text form is a
// new version; keys of different versions never compare equal.

#include <cstdint>
#include <string>
#include <string_view>

#include <lucent/raw/decoded_raw.hpp>

namespace lucent::assets {

inline constexpr std::uint32_t kModeKeyVersion = 1;

struct ModeKey {
    std::uint32_t version = kModeKeyVersion;
    std::string make;
    std::string model;
    std::string container;
    std::string compression;
    std::string compression_lossiness;  // "lossless", "lossy", "unknown" (not part of the text)
    std::uint32_t area_width = 0;
    std::uint32_t area_height = 0;
    std::string bits;             // "14", "16", "f32"
    std::string bits_provenance;  // "dng_tag" or "lucent_rule" (not part of the text)
    std::string representation;
    std::string sample;
    std::string view;
    std::string subimage_role;
    std::uint32_t subimage_index = 0;
    std::uint32_t subimage_count = 1;
    // Metadata stage the key was derived from ("after_unpack" for a decode, "after_open" when the
    // decoder refused the pixel data and only a probe exists). Not part of the text.
    std::string stage;

    // The canonical text described above.
    [[nodiscard]] std::string to_string() const;
    // "make|model": groups the modes of one camera model.
    [[nodiscard]] std::string model_key() const;
    // "<area> <compression>" and the like, for tables; developer-facing.
    [[nodiscard]] std::string area_text() const;
};

// Canonical maker id: alias table ("nikon corporation" -> "nikon", "ricoh imaging company, ltd."
// -> "pentax", "arashi vision" -> "insta360", "om digital solutions" / "om digital" ->
// "om-system", "leica camera ag" -> "leica", "fujifilm", "sony", "canon", "dji", "hasselblad",
// "panasonic", "olympus", ...), otherwise slug(make).
[[nodiscard]] std::string canonical_make(std::string_view make);

// The model component for `model` of maker `canonical` (see the file comment).
[[nodiscard]] std::string canonical_model(std::string_view canonical, std::string_view model);

// Lower-case slug: [a-z0-9] kept, every other run of characters becomes one '-', no leading or
// trailing '-'; a '*' is spelled "star" as its own word ("HE*" -> "he-star"), so that modes whose
// names differ only by a star (Nikon HE and HE*) keep distinct keys.
[[nodiscard]] std::string slug(std::string_view text);

// Bits needed to represent `white` (ceil(log2(white + 1))); 0 for white <= 0.
[[nodiscard]] std::uint32_t bits_for_white_level(double white) noexcept;

// Derives the key from decoded (or, for refused files, probed) metadata.
[[nodiscard]] ModeKey derive_mode_key(const raw::RawMetadata& metadata);

}  // namespace lucent::assets
