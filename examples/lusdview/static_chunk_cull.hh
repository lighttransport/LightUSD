// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <limits>

namespace lusdview {

// Conservative pixel radius for a world-space sphere. Use nearest view-space
// depth: Euclidean distance underestimates large/off-axis objects. Orthographic
// cameras have no distance attenuation; projection Y may have either sign.
inline float StaticChunkProjectedRadiusPx(const float center[3], float radius,
                                         const float view[16],
                                         const float proj[16], int height) {
  const float scale = 0.5f * static_cast<float>(height) * std::fabs(proj[5]);
  if (!(scale > 0.0f) || !std::isfinite(scale) ||
      !(radius >= 0.0f) || !std::isfinite(radius))
    return std::numeric_limits<float>::infinity();
  if (proj[15] == 1.0f && proj[11] == 0.0f) return radius * scale;
  const float depth = -(view[2] * center[0] + view[6] * center[1] +
                        view[10] * center[2] + view[14]);
  const float nearest = depth - radius;
  if (!(nearest > 0.0f)) return std::numeric_limits<float>::infinity();
  return radius * scale / nearest;
}

}  // namespace lusdview
