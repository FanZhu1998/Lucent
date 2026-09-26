#pragma once

// Colour-difference helpers for the render gates: linear sRGB -> CIELAB (D65 white, Y = 1) and
// CIEDE2000 (G. Sharma, W. Wu, E. N. Dalal, "The CIEDE2000 color-difference formula:
// implementation notes, supplementary test data, and mathematical observations", Color Research
// & Application 30(1), 2005). Test-only code; the same conventions as
// research/tools/render_reference.py.

#include <array>

namespace lucent::integration {

using Lab = std::array<double, 3>;
using Rgb = std::array<double, 3>;

// CIELAB of linear sRGB (BT.709 primaries, D65) relative to the D65 white with Y = 1.
[[nodiscard]] Lab lab_from_linear_srgb(const Rgb& rgb);

// Relative luminance Y of linear sRGB.
[[nodiscard]] double luminance_from_linear_srgb(const Rgb& rgb);

// CIEDE2000 with kL = kC = kH = 1.
[[nodiscard]] double ciede2000(const Lab& a, const Lab& b);

}  // namespace lucent::integration
