// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lightusd {
namespace tydra {

// LM-63 photometry in the light's local frame: -Z is the beam axis, X the
// luminaire width and Y its length. Candela rows are horizontal-major.
struct PreviewIesProfile {
  unsigned type = 1, tilt_geometry = 0;
  std::vector<float> vertical, horizontal, candela, tilt_angles, tilt_factors;
  float mean_candela = 0;
  float sample(std::array<float, 3> direction, float angle_scale = 0,
               bool normalize = false, float tilt_degrees = 0) const;
  float tiltMultiplier(float degrees) const;
};
using PreviewIesTiltLoader =
    std::function<bool(const std::string&, std::string*, std::string*)>;
bool ParsePreviewIes(std::string_view text, PreviewIesProfile*, std::string*,
                     const PreviewIesTiltLoader& external_tilt = {});
bool LoadPreviewIes(const std::string&, PreviewIesProfile*, std::string*,
                    std::vector<std::string>* dependencies = nullptr);
// Writes a self-contained standard IES file, including any external TILT data.
bool WritePreviewIes(const std::string&, const PreviewIesProfile&,
                     std::string*);

}  // namespace tydra
}  // namespace lightusd
