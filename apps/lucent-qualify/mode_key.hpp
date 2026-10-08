#pragma once

// The camera qualification mode key (ADR 0016). Its definition moved to lucent_assets in W10
// phase 3 (lucent/assets/mode_key.hpp, ADR 0018) because the application derives the same key
// when it assesses a library item against the camera support database. The harness keeps using
// the names it has always used.

#include <lucent/assets/mode_key.hpp>

namespace lucent::qualify {

using assets::bits_for_white_level;
using assets::canonical_make;
using assets::canonical_model;
using assets::derive_mode_key;
using assets::kModeKeyVersion;
using assets::ModeKey;
using assets::slug;

}  // namespace lucent::qualify
