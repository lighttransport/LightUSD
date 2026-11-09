// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "stage/stage.hh"

#include <cstdint>
#include <string>

namespace lightusd {

enum ModelCardFace : uint32_t {
  kCardXNeg = 1u << 0,
  kCardXPos = 1u << 1,
  kCardYNeg = 1u << 2,
  kCardYPos = 1u << 3,
  kCardZNeg = 1u << 4,
  kCardZPos = 1u << 5,
  kAllCardFaces = (1u << 6) - 1u,
};

struct GeomModelData {
  bool apply_draw_mode = false;
  std::string draw_mode = "inherited";
  std::string card_geometry = "cross";
  std::string card_visibility = "inherited";
  float draw_mode_color[3] = {0.18f, 0.18f, 0.18f};
  std::string card_textures[6];
};

bool HasGeomModelAPI(const UsdPrim& prim);
bool GetGeomModelData(const Stage& stage, const UsdPrim& prim,
                      GeomModelData* out, double time = 0.0);

// Resolve the closest non-inherited opinion. The root fallback is `full`.
std::string ComputeModelCardVisibility(const Stage& stage,
                                       const UsdPrim& prim,
                                       double time = 0.0);

// Return the six-face selection for `full`, or suppress the two faces normal
// to the stage up-axis for `simple`.
uint32_t ComputeModelCardFaceMask(const Stage& stage, const UsdPrim& prim,
                                  char up_axis, double time = 0.0);

}  // namespace lightusd
