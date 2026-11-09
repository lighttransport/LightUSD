// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// ColorSpace API support for USD color management
// See: https://openusd.org/dev/user_guides/color_user_guide.html
//
#pragma once

#include <string>
#include "value-types.hh"

namespace lightusd {

///
/// ColorSpace tokens - canonical set of interoperable color spaces in OpenUSD
///
namespace colorspace {

// Linear color spaces
constexpr auto kLinAp1Scene = "lin_ap1_scene";           // ACEScg
constexpr auto kLinAp0Scene = "lin_ap0_scene";           // ACES2065-1
constexpr auto kLinRec709Scene = "lin_rec709_scene";     // Linear Rec.709/sRGB
constexpr auto kLinP3D65Scene = "lin_p3d65_scene";       // Linear P3-D65
constexpr auto kLinRec2020Scene = "lin_rec2020_scene";   // Linear Rec.2020
constexpr auto kLinAdobeRGBScene = "lin_adobergb_scene"; // Linear Adobe RGB
constexpr auto kLinCieXyzD65Scene = "lin_ciexyzd65_scene"; // CIE XYZ-D65

// Non-linear/encoded color spaces (sRGB transfer function)
constexpr auto kSrgbRec709Scene = "srgb_rec709_scene";   // sRGB Rec.709
constexpr auto kSrgbAp1Scene = "srgb_ap1_scene";         // sRGB AP1
constexpr auto kSrgbP3D65Scene = "srgb_p3d65_scene";     // sRGB P3-D65

// Non-linear/encoded color spaces (Gamma 2.2 transfer function)
constexpr auto kG22Rec709Scene = "g22_rec709_scene";     // Gamma 2.2 Rec.709
constexpr auto kG22Ap1Scene = "g22_ap1_scene";           // Gamma 2.2 AP1
constexpr auto kG22AdobeRGBScene = "g22_adobergb_scene"; // Gamma 2.2 Adobe RGB

// Non-linear/encoded color spaces (Gamma 1.8 transfer function)
constexpr auto kG18Rec709Scene = "g18_rec709_scene";     // Gamma 1.8 Rec.709

// Special designations
constexpr auto kData = "data";         // Non-color data (normals, displacement, etc.)
constexpr auto kUnknown = "unknown";   // Color space unspecified
constexpr auto kRaw = "raw";           // Legacy equivalent to "data"
constexpr auto kIdentity = "identity"; // Legacy equivalent to "unknown"

// Default color space (Linear Rec.709)
constexpr auto kDefault = kLinRec709Scene;

}  // namespace colorspace

///
/// ColorSpace enumeration for efficient runtime handling
///
enum class ColorSpace {
  // Linear spaces
  LinAp1Scene,        // ACEScg
  LinAp0Scene,        // ACES2065-1
  LinRec709Scene,     // Linear Rec.709/sRGB (DEFAULT)
  LinP3D65Scene,      // Linear P3-D65
  LinRec2020Scene,    // Linear Rec.2020
  LinAdobeRGBScene,   // Linear Adobe RGB
  LinCieXyzD65Scene,  // CIE XYZ-D65

  // Non-linear (sRGB OETF)
  SrgbRec709Scene,    // sRGB Rec.709
  SrgbAp1Scene,       // sRGB AP1
  SrgbP3D65Scene,     // sRGB P3-D65

  // Non-linear (Gamma 2.2)
  G22Rec709Scene,     // Gamma 2.2 Rec.709
  G22Ap1Scene,        // Gamma 2.2 AP1
  G22AdobeRGBScene,   // Gamma 2.2 Adobe RGB

  // Non-linear (Gamma 1.8)
  G18Rec709Scene,     // Gamma 1.8 Rec.709

  // Special
  Data,               // Non-color data
  Unknown,            // Unspecified
  Raw,                // Legacy: non-color data
  Identity,           // Legacy: unspecified
};

///
/// ColorSpace utility functions
///

/// Convert ColorSpace enum to token string
std::string to_token(ColorSpace cs);

/// Convert token string to ColorSpace enum
/// Returns ColorSpace::Unknown if token is not recognized
ColorSpace from_token(const std::string& token);

/// Check if color space is linear (no transfer function)
bool is_linear(ColorSpace cs);

/// Check if color space represents non-color data
bool is_data(ColorSpace cs);

/// Get default color space (Linear Rec.709)
inline ColorSpace get_default() {
  return ColorSpace::LinRec709Scene;
}

// Note: ColorSpaceAPI struct is defined in prim-types.hh
// This header provides the colorspace tokens and utility functions

}  // namespace lightusd
