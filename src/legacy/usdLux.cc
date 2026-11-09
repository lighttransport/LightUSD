// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// UsdLux implementations

#include "usdLux.hh"

#include <sstream>
#include "str-util.hh"
#include "value-types.hh"
#include "common-macros.inc"

namespace lightusd {

//
// Utility functions for DomeLight::TextureFormat
//

std::string to_string(const DomeLight::TextureFormat &format) {
  switch (format) {
    case DomeLight::TextureFormat::Automatic:
      return "automatic";
    case DomeLight::TextureFormat::Latlong:
      return "latlong";
    case DomeLight::TextureFormat::MirroredBall:
      return "mirroredBall";
    case DomeLight::TextureFormat::Angular:
      return "angular";
  }
  return "[[InvalidTextureFormat]]";
}

std::string to_string(const DomeLight_1::TextureFormat &format) {
  switch (format) {
    case DomeLight_1::TextureFormat::Automatic:
      return "automatic";
    case DomeLight_1::TextureFormat::Latlong:
      return "latlong";
    case DomeLight_1::TextureFormat::MirroredBall:
      return "mirroredBall";
    case DomeLight_1::TextureFormat::Angular:
      return "angular";
  }
  return "[[InvalidTextureFormat]]";
}

bool DomeLight_TextureFormat_from_string(const std::string &str, DomeLight::TextureFormat *format) {
  if (!format) {
    return false;
  }

  if (str == "automatic") {
    (*format) = DomeLight::TextureFormat::Automatic;
    return true;
  } else if (str == "latlong") {
    (*format) = DomeLight::TextureFormat::Latlong;
    return true;
  } else if (str == "mirroredBall") {
    (*format) = DomeLight::TextureFormat::MirroredBall;
    return true;
  } else if (str == "angular") {
    (*format) = DomeLight::TextureFormat::Angular;
    return true;
  }

  return false;
}

//
// Helper functions for light primitives
//

bool IsLightFilterPrim(const Prim &prim) {
  // Note: LightFilter and PluginLightFilter would need TYPE_IDs added
  // to value-types.hh to be properly identified
  // For now, we can check by name
  return (prim.prim_type_name() == kLightFilter) ||
         (prim.prim_type_name() == kPluginLightFilter);
}

bool IsBoundableLight(const Prim &prim) {
  uint32_t tid = prim.type_id();
  return (tid == value::TYPE_ID_LUX_SPHERE) ||
         (tid == value::TYPE_ID_LUX_CYLINDER) ||
         (tid == value::TYPE_ID_LUX_DISK) ||
         (tid == value::TYPE_ID_LUX_RECT) ||
         (tid == value::TYPE_ID_LUX_PORTAL);
}

bool IsNonboundableLight(const Prim &prim) {
  uint32_t tid = prim.type_id();
  return (tid == value::TYPE_ID_LUX_DISTANT) ||
         (tid == value::TYPE_ID_LUX_DOME) ||
         (tid == value::TYPE_ID_LUX_GEOMETRY);
}

//
// Light API helper functions
//

// Compute effective light color including color temperature
value::color3f ComputeEffectiveLightColor(
    const value::color3f &baseColor,
    bool enableColorTemperature,
    float colorTemperature) {

  if (!enableColorTemperature) {
    return baseColor;
  }

  // Convert color temperature to RGB using approximate blackbody radiation
  // This is a simplified implementation based on common CG practices
  float temp = colorTemperature;
  temp = std::max(1000.0f, std::min(40000.0f, temp)); // clamp to reasonable range
  temp /= 100.0f;

  value::color3f tempColor;

  // Red channel
  if (temp <= 66.0f) {
    tempColor[0] = 1.0f;
  } else {
    float r = temp - 60.0f;
    r = 329.698727446f * std::pow(r, -0.1332047592f);
    tempColor[0] = std::max(0.0f, std::min(1.0f, r / 255.0f));
  }

  // Green channel
  if (temp <= 66.0f) {
    float g = temp;
    g = 99.4708025861f * std::log(g) - 161.1195681661f;
    tempColor[1] = std::max(0.0f, std::min(1.0f, g / 255.0f));
  } else {
    float g = temp - 60.0f;
    g = 288.1221695283f * std::pow(g, -0.0755148492f);
    tempColor[1] = std::max(0.0f, std::min(1.0f, g / 255.0f));
  }

  // Blue channel
  if (temp >= 66.0f) {
    tempColor[2] = 1.0f;
  } else if (temp <= 19.0f) {
    tempColor[2] = 0.0f;
  } else {
    float b = temp - 10.0f;
    b = 138.5177312231f * std::log(b) - 305.0447927307f;
    tempColor[2] = std::max(0.0f, std::min(1.0f, b / 255.0f));
  }

  // Multiply base color by temperature color
  return value::color3f({
      baseColor[0] * tempColor[0],
      baseColor[1] * tempColor[1],
      baseColor[2] * tempColor[2]
  });
}

// Compute light intensity from exposure (EV)
float ComputeLightIntensityFromExposure(float baseIntensity, float exposure) {
  // exposure is in EV (exposure value)
  // intensity_multiplier = 2^exposure
  return baseIntensity * std::pow(2.0f, exposure);
}

// Compute final light intensity combining base intensity and exposure
float ComputeFinalLightIntensity(float baseIntensity, float exposure) {
  return ComputeLightIntensityFromExposure(baseIntensity, exposure);
}

//
// Shaping API helper functions
//

// Check if a light has shaping applied (cone angle < 90 degrees or IES profile)
bool HasLightShaping(const ShapingAPI &shaping) {
  float coneAngle = 90.0f;
  shaping.shapingConeAngle.get_value().get_scalar(&coneAngle);
  bool hasConeShaping = coneAngle < 90.0f;
  bool hasIES = shaping.shapingIesFile.authored();
  return hasConeShaping || hasIES;
}

//
// Shadow API helper functions
//

// Check if shadows are enabled
bool AreShadowsEnabled(const ShadowAPI &shadow) {
  bool enabled = true;
  shadow.shadowEnable.get_value().get_scalar(&enabled);
  return enabled;
}

// Get effective shadow color
value::color3f GetEffectiveShadowColor(const ShadowAPI &shadow) {
  value::color3f color{0.0f, 0.0f, 0.0f};
  shadow.shadowColor.get_value().get_scalar(&color);
  return color;
}

} // namespace lightusd


