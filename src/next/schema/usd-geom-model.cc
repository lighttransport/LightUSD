// SPDX-License-Identifier: Apache-2.0
#include "usd-geom-model.hh"
#include "../layer/prim-spec.hh"

#include "../eval/attribute-eval.hh"

namespace lightusd {
namespace next {

bool HasGeomModelAPI(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  for (const std::string& schema : prim.GetMeta().apiSchemas()) {
    if (schema == "GeomModelAPI") return true;
  }
  return false;
}

bool GetGeomModelData(const Stage& stage, const UsdPrim& prim,
                      GeomModelData* out, double time) {
  if (!out || !HasGeomModelAPI(prim)) return false;
  *out = GeomModelData{};
  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->apply_draw_mode = eval.EvalBool(prim, "model:applyDrawMode")
                             .value_or(false);
  if (const auto value = eval.EvalToken(prim, "model:drawMode"))
    out->draw_mode = *value;
  if (const auto value = eval.EvalToken(prim, "model:cardGeometry"))
    out->card_geometry = *value;
  if (const auto value = eval.EvalToken(prim, "model:cardVisibility"))
    out->card_visibility = *value;
  eval.EvalFloat3(prim, "model:drawModeColor", out->draw_mode_color);
  static constexpr const char* kTextures[] = {
      "model:cardTextureXNeg", "model:cardTextureXPos",
      "model:cardTextureYNeg", "model:cardTextureYPos",
      "model:cardTextureZNeg", "model:cardTextureZPos"};
  for (size_t i = 0; i < 6; ++i) {
    if (const auto value = eval.EvalAssetPath(prim, kTextures[i]))
      out->card_textures[i] = *value;
  }
  return true;
}

std::string ComputeModelCardVisibility(const Stage& stage,
                                       const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  for (UsdPrim current = prim; current.IsValid(); current = current.GetParent()) {
    if (const auto value = eval.EvalToken(current, "model:cardVisibility")) {
      if (*value == "full" || *value == "simple") return *value;
    }
  }
  return "full";
}

uint32_t ComputeModelCardFaceMask(const Stage& stage, const UsdPrim& prim,
                                  char up_axis, double time) {
  if (ComputeModelCardVisibility(stage, prim, time) != "simple")
    return kAllCardFaces;
  if (up_axis == 'X' || up_axis == 'x')
    return kAllCardFaces & ~(kCardXNeg | kCardXPos);
  if (up_axis == 'Y' || up_axis == 'y')
    return kAllCardFaces & ~(kCardYNeg | kCardYPos);
  return kAllCardFaces & ~(kCardZNeg | kCardZPos);
}

}  // namespace next
}  // namespace lightusd
