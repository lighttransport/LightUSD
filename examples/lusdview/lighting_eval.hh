// SPDX-License-Identifier: Apache-2.0
// Backend-neutral light derivation shared by the legacy and `--core` loaders.
#pragma once

#include <algorithm>
#include <cmath>

#include "gpu_scene.hh"

namespace lusdview {

inline float ClampLightTemperature(float kelvin) {
  return std::max(1000.0f, std::min(40000.0f, kelvin));
}

inline void TemperatureRgb(float kelvin, float out[3]) {
  if (!out) return;
  const float temp = ClampLightTemperature(kelvin) / 100.0f;
  if (temp <= 66.0f) {
    out[0] = 1.0f;
  } else {
    out[0] = std::max(0.0f, std::min(1.0f,
        329.698727446f * std::pow(temp - 60.0f, -0.1332047592f) / 255.0f));
  }
  if (temp <= 66.0f) {
    out[1] = std::max(0.0f, std::min(1.0f,
        (99.4708025861f * std::log(temp) - 161.1195681661f) / 255.0f));
  } else {
    out[1] = std::max(0.0f, std::min(1.0f,
        288.1221695283f * std::pow(temp - 60.0f, -0.0755148492f) / 255.0f));
  }
  if (temp >= 66.0f) {
    out[2] = 1.0f;
  } else if (temp <= 19.0f) {
    out[2] = 0.0f;
  } else {
    out[2] = std::max(0.0f, std::min(1.0f,
        (138.5177312231f * std::log(temp - 10.0f) - 305.0447927307f) /
        255.0f));
  }
}

inline float LightShapeArea(const DrawLightCPU& light) {
  constexpr float kPi = 3.14159265358979323846f;
  switch (light.type) {
    case DrawLightCPU::Type::Sphere:
    case DrawLightCPU::Type::Point:
      return 4.0f * kPi * std::max(light.radius, 0.0f) *
             std::max(light.radius, 0.0f);
    case DrawLightCPU::Type::Disk:
      return kPi * std::max(light.radius, 0.0f) *
             std::max(light.radius, 0.0f);
    case DrawLightCPU::Type::Rect:
    case DrawLightCPU::Type::Portal:
      return std::max(light.width, 0.0f) * std::max(light.height, 0.0f);
    case DrawLightCPU::Type::Cylinder:
      return 2.0f * kPi * std::max(light.radius, 0.0f) *
             std::max(light.length, 0.0f);
    default:
      return 0.0f;
  }
}

inline void ApplyDerivedLightParams(DrawLightCPU* light) {
  if (!light) return;
  float temperature[3]{1.0f, 1.0f, 1.0f};
  if (light->enableColorTemperature) {
    TemperatureRgb(light->colorTemperature, temperature);
  }
  light->effectiveIntensity =
      light->intensity * std::pow(2.0f, light->exposure);
  for (int c = 0; c < 3; ++c) {
    light->effectiveColor[c] = light->color[c] * temperature[c] *
                               light->effectiveIntensity;
  }
  light->area = std::max(0.0f, LightShapeArea(*light));
  light->invArea = light->area > 0.0f ? 1.0f / light->area : 0.0f;
  for (int c = 0; c < 3; ++c) {
    light->normalizedColor[c] = light->effectiveColor[c] *
        (light->normalize && light->invArea > 0.0f ? light->invArea : 1.0f);
  }
  light->hasShaping = light->shapingConeAngle < 90.0f ||
                      !light->shapingIesFile.empty() ||
                      light->shapingFocus != 0.0f ||
                      light->shapingFocusTint[0] != 0.0f ||
                      light->shapingFocusTint[1] != 0.0f ||
                      light->shapingFocusTint[2] != 0.0f;
}

}  // namespace lusdview
