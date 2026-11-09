// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.

#include "color-space.hh"
#include "tiny-hashmap.hh"

namespace lightusd {

namespace {

// Token to enum lookup table (heap-allocated to avoid exit-time destructor)
const lightusd::HashMap<std::string, ColorSpace>& GetTokenToEnumMap() {
  static lightusd::HashMap<std::string, ColorSpace>* kTokenToEnum =
    new lightusd::HashMap<std::string, ColorSpace>({
      // Linear spaces
      {colorspace::kLinAp1Scene, ColorSpace::LinAp1Scene},
      {colorspace::kLinAp0Scene, ColorSpace::LinAp0Scene},
      {colorspace::kLinRec709Scene, ColorSpace::LinRec709Scene},
      {colorspace::kLinP3D65Scene, ColorSpace::LinP3D65Scene},
      {colorspace::kLinRec2020Scene, ColorSpace::LinRec2020Scene},
      {colorspace::kLinAdobeRGBScene, ColorSpace::LinAdobeRGBScene},
      {colorspace::kLinCieXyzD65Scene, ColorSpace::LinCieXyzD65Scene},

      // Non-linear (sRGB OETF)
      {colorspace::kSrgbRec709Scene, ColorSpace::SrgbRec709Scene},
      {colorspace::kSrgbAp1Scene, ColorSpace::SrgbAp1Scene},
      {colorspace::kSrgbP3D65Scene, ColorSpace::SrgbP3D65Scene},

      // Non-linear (Gamma 2.2)
      {colorspace::kG22Rec709Scene, ColorSpace::G22Rec709Scene},
      {colorspace::kG22Ap1Scene, ColorSpace::G22Ap1Scene},
      {colorspace::kG22AdobeRGBScene, ColorSpace::G22AdobeRGBScene},

      // Non-linear (Gamma 1.8)
      {colorspace::kG18Rec709Scene, ColorSpace::G18Rec709Scene},

      // Special
      {colorspace::kData, ColorSpace::Data},
      {colorspace::kUnknown, ColorSpace::Unknown},
      {colorspace::kRaw, ColorSpace::Raw},
      {colorspace::kIdentity, ColorSpace::Identity},
    });
  return *kTokenToEnum;
}

// Enum to token lookup table (heap-allocated to avoid exit-time destructor)
const lightusd::HashMap<ColorSpace, std::string>& GetEnumToTokenMap() {
  static lightusd::HashMap<ColorSpace, std::string>* kEnumToToken =
    new lightusd::HashMap<ColorSpace, std::string>({
      // Linear spaces
      {ColorSpace::LinAp1Scene, colorspace::kLinAp1Scene},
      {ColorSpace::LinAp0Scene, colorspace::kLinAp0Scene},
      {ColorSpace::LinRec709Scene, colorspace::kLinRec709Scene},
      {ColorSpace::LinP3D65Scene, colorspace::kLinP3D65Scene},
      {ColorSpace::LinRec2020Scene, colorspace::kLinRec2020Scene},
      {ColorSpace::LinAdobeRGBScene, colorspace::kLinAdobeRGBScene},
      {ColorSpace::LinCieXyzD65Scene, colorspace::kLinCieXyzD65Scene},

      // Non-linear (sRGB OETF)
      {ColorSpace::SrgbRec709Scene, colorspace::kSrgbRec709Scene},
      {ColorSpace::SrgbAp1Scene, colorspace::kSrgbAp1Scene},
      {ColorSpace::SrgbP3D65Scene, colorspace::kSrgbP3D65Scene},

      // Non-linear (Gamma 2.2)
      {ColorSpace::G22Rec709Scene, colorspace::kG22Rec709Scene},
      {ColorSpace::G22Ap1Scene, colorspace::kG22Ap1Scene},
      {ColorSpace::G22AdobeRGBScene, colorspace::kG22AdobeRGBScene},

      // Non-linear (Gamma 1.8)
      {ColorSpace::G18Rec709Scene, colorspace::kG18Rec709Scene},

      // Special
      {ColorSpace::Data, colorspace::kData},
      {ColorSpace::Unknown, colorspace::kUnknown},
      {ColorSpace::Raw, colorspace::kRaw},
      {ColorSpace::Identity, colorspace::kIdentity},
    });
  return *kEnumToToken;
}

}  // namespace

std::string to_token(ColorSpace cs) {
  const auto& enumToToken = GetEnumToTokenMap();
  auto it = enumToToken.find(cs);
  if (it != enumToToken.end()) {
    return it->second;
  }
  return colorspace::kUnknown;
}

ColorSpace from_token(const std::string& token) {
  const auto& tokenToEnum = GetTokenToEnumMap();
  auto it = tokenToEnum.find(token);
  if (it != tokenToEnum.end()) {
    return it->second;
  }
  return ColorSpace::Unknown;
}

bool is_linear(ColorSpace cs) {
  switch (cs) {
    case ColorSpace::LinAp1Scene:
    case ColorSpace::LinAp0Scene:
    case ColorSpace::LinRec709Scene:
    case ColorSpace::LinP3D65Scene:
    case ColorSpace::LinRec2020Scene:
    case ColorSpace::LinAdobeRGBScene:
    case ColorSpace::LinCieXyzD65Scene:
      return true;
    case ColorSpace::SrgbRec709Scene:
    case ColorSpace::SrgbAp1Scene:
    case ColorSpace::SrgbP3D65Scene:
    case ColorSpace::G22Rec709Scene:
    case ColorSpace::G22Ap1Scene:
    case ColorSpace::G22AdobeRGBScene:
    case ColorSpace::G18Rec709Scene:
    case ColorSpace::Data:
    case ColorSpace::Unknown:
    case ColorSpace::Raw:
    case ColorSpace::Identity:
      return false;
  }
  return false; // unreachable but silences warning
}

bool is_data(ColorSpace cs) {
  return (cs == ColorSpace::Data) || (cs == ColorSpace::Raw);
}

}  // namespace lightusd
