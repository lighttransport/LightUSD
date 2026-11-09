// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "usd-render.hh"

#include "../eval/attribute-eval.hh"

namespace lightusd {
namespace next {
namespace {

void CopyRelationship(const UsdPrim& prim, const char* name,
                      std::vector<Path>* out) {
  out->clear();
  if (const std::vector<Path>* targets = prim.GetRelationship(name)) {
    *out = *targets;
  }
}

void CopyTokenArray(const AttributeEval& eval, const UsdPrim& prim,
                    const char* name, std::vector<std::string>* out) {
  const EvalResult result = eval.Eval(prim, name);
  if (result.success) {
    if (auto values = result.value.as_token_array()) {
      out->assign(values->begin(), values->end());
    }
  }
}

void ReadSettingsBase(const AttributeEval& eval, const UsdPrim& prim,
                      RenderSettingsBaseData* out) {
  CopyRelationship(prim, "camera", &out->camera);
  const EvalResult resolution = eval.Eval(prim, "resolution");
  if (resolution.success) {
    if (const int32_t* value = resolution.value.as_int2()) {
      out->resolution[0] = value[0];
      out->resolution[1] = value[1];
    }
  }
  out->pixel_aspect_ratio =
      eval.EvalOr(prim, "pixelAspectRatio", 1.0f);
  if (const auto value = eval.EvalToken(prim, "aspectRatioConformPolicy")) {
    out->aspect_ratio_conform_policy = *value;
  }
  (void)eval.EvalFloat4(prim, "dataWindowNDC", out->data_window_ndc);
  out->instantaneous_shutter =
      eval.EvalBool(prim, "instantaneousShutter").value_or(false);
  out->disable_motion_blur =
      eval.EvalBool(prim, "disableMotionBlur").value_or(false);
  out->disable_depth_of_field =
      eval.EvalBool(prim, "disableDepthOfField").value_or(false);
}

AttributeEval MakeEval(const Stage& stage, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval;
}

}  // namespace

bool IsRenderSettings(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "RenderSettings";
}
bool IsRenderProduct(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "RenderProduct";
}
bool IsRenderVar(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "RenderVar";
}
bool IsRenderPass(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "RenderPass";
}

bool GetRenderSettingsData(const Stage& stage, const UsdPrim& prim,
                           RenderSettingsData* out, double time) {
  if (!out || !IsRenderSettings(prim)) return false;
  *out = RenderSettingsData{};
  AttributeEval eval = MakeEval(stage, time);
  ReadSettingsBase(eval, prim, out);
  CopyRelationship(prim, "products", &out->products);
  CopyTokenArray(eval, prim, "includedPurposes", &out->included_purposes);
  CopyTokenArray(eval, prim, "materialBindingPurposes",
                 &out->material_binding_purposes);
  if (const auto value = eval.EvalToken(prim, "renderingColorSpace")) {
    out->rendering_color_space = *value;
  }
  return true;
}

bool GetRenderProductData(const Stage& stage, const UsdPrim& prim,
                          RenderProductData* out, double time) {
  if (!out || !IsRenderProduct(prim)) return false;
  *out = RenderProductData{};
  AttributeEval eval = MakeEval(stage, time);
  ReadSettingsBase(eval, prim, out);
  if (const auto value = eval.EvalToken(prim, "productType")) {
    out->product_type = *value;
  }
  if (const auto value = eval.EvalToken(prim, "productName")) {
    out->product_name = *value;
  }
  CopyRelationship(prim, "orderedVars", &out->ordered_vars);
  return true;
}

bool GetRenderVarData(const Stage& stage, const UsdPrim& prim,
                      RenderVarData* out, double time) {
  if (!out || !IsRenderVar(prim)) return false;
  *out = RenderVarData{};
  AttributeEval eval = MakeEval(stage, time);
  if (const auto value = eval.EvalToken(prim, "dataType")) {
    out->data_type = *value;
  }
  if (const auto value = eval.EvalString(prim, "sourceName")) {
    out->source_name = *value;
  }
  if (const auto value = eval.EvalToken(prim, "sourceType")) {
    out->source_type = *value;
  }
  return true;
}

bool GetRenderPassData(const Stage& stage, const UsdPrim& prim,
                       RenderPassData* out, double time) {
  if (!out || !IsRenderPass(prim)) return false;
  *out = RenderPassData{};
  AttributeEval eval = MakeEval(stage, time);
  if (const auto value = eval.EvalToken(prim, "passType")) {
    out->pass_type = *value;
  }
  const EvalResult command = eval.Eval(prim, "command");
  if (command.success) out->command = command.value;
  CopyRelationship(prim, "renderSource", &out->render_source);
  CopyRelationship(prim, "inputPasses", &out->input_passes);
  CopyRelationship(prim, "collection:renderVisibility:includes",
                   &out->render_visibility_includes);
  CopyRelationship(prim, "collection:renderVisibility:excludes",
                   &out->render_visibility_excludes);
  CopyRelationship(prim, "collection:cameraVisibility:includes",
                   &out->camera_visibility_includes);
  CopyRelationship(prim, "collection:cameraVisibility:excludes",
                   &out->camera_visibility_excludes);
  if (const auto value = eval.EvalAssetPath(prim, "fileName")) {
    out->file_name = *value;
  }
  out->render_visibility_include_root =
      eval.EvalBool(prim, "collection:renderVisibility:includeRoot")
          .value_or(true);
  out->camera_visibility_include_root =
      eval.EvalBool(prim, "collection:cameraVisibility:includeRoot")
          .value_or(true);
  return true;
}

}  // namespace next
}  // namespace lightusd
