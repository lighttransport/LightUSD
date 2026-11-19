// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "usd-render.hh"

#include "eval/attribute-eval.hh"

namespace lightusd {
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


namespace {

bool SpecAuthored(const UsdPrim& prim, const char* name) {
  const PrimSpec* spec = prim.GetPrimSpec();
  return spec && (spec->property(GetPropNameTable().find(name)) != nullptr ||
                  prim.GetTimeSamples(GetPropNameTable().find(name)) != nullptr);
}

// _ReadSettingsBase: values from `prim`; when `only_authored`, only the
// attributes the prim authors (composed opinion) override `pd`.
void ReadSpecBase(const UsdPrim& prim, bool only_authored, RenderSpecProduct* pd) {
  auto use = [&](const char* name) { return !only_authored || SpecAuthored(prim, name); };
  if (const std::vector<Path>* t = prim.GetRelationship("camera")) {
    if (!t->empty()) pd->camera = (*t)[0].str();
  }
  const Value* v = nullptr;
  if (use("resolution") && (v = prim.GetPropertyValue("resolution"))) {
    if (const int32_t* r = v->as_int2()) { pd->resolution[0] = r[0]; pd->resolution[1] = r[1]; }
  }
  if (use("pixelAspectRatio") && (v = prim.GetPropertyValue("pixelAspectRatio"))) {
    if (const float* f = v->as_float()) pd->pixel_aspect_ratio = *f;
  }
  if (use("aspectRatioConformPolicy") && (v = prim.GetPropertyValue("aspectRatioConformPolicy"))) {
    const StringView t = v->as_token();
    if (t.size()) pd->aspect_ratio_conform_policy.assign(t.data(), t.size());
  }
  if (use("dataWindowNDC") && (v = prim.GetPropertyValue("dataWindowNDC"))) {
    if (const float* f = v->as_float4()) for (int k = 0; k < 4; ++k) pd->data_window_ndc[k] = f[k];
  }
  if (use("disableMotionBlur") && (v = prim.GetPropertyValue("disableMotionBlur"))) {
    if (const bool* b = v->as_bool()) pd->disable_motion_blur = *b;
  }
  if (use("instantaneousShutter") && (v = prim.GetPropertyValue("instantaneousShutter"))) {
    if (const bool* b = v->as_bool(); b && *b) pd->disable_motion_blur = true;
  }
  if (use("disableDepthOfField") && (v = prim.GetPropertyValue("disableDepthOfField"))) {
    if (const bool* b = v->as_bool()) pd->disable_depth_of_field = *b;
  }
}

void ApplyConformPolicy(RenderSpecProduct* p) {
  const float rx = float(p->resolution[0]), ry = float(p->resolution[1]);
  float* size = p->aperture;
  if (rx <= 0 || ry <= 0 || size[0] <= 0 || size[1] <= 0) return;
  const float res_aspect = rx / ry;
  const float image_aspect = p->pixel_aspect_ratio * res_aspect;
  if (image_aspect <= 0) return;
  const float aperture_aspect = size[0] / size[1];
  const std::string& policy = p->aspect_ratio_conform_policy;
  enum { kWidth, kHeight, kNone } adjust = kNone;
  if (policy == "adjustPixelAspectRatio") {
    p->pixel_aspect_ratio = aperture_aspect / res_aspect;
  } else if (policy == "adjustApertureHeight") {
    adjust = kHeight;
  } else if (policy == "adjustApertureWidth") {
    adjust = kWidth;
  } else if (policy == "expandAperture") {
    adjust = aperture_aspect > image_aspect ? kHeight : kWidth;
  } else if (policy == "cropAperture") {
    adjust = aperture_aspect > image_aspect ? kWidth : kHeight;
  }
  if (adjust == kWidth) size[0] = size[1] * image_aspect;
  else if (adjust == kHeight) size[1] = size[0] / image_aspect;
}

}  // namespace

bool ComputeRenderSpec(const Stage& stage, const UsdPrim& settings, RenderSpec* out) {
  if (!out || !IsRenderSettings(settings)) return false;
  *out = RenderSpec();
  out->settings_path = settings.GetPath().str();
  RenderSpecProduct base;
  ReadSpecBase(settings, false, &base);
  if (const std::vector<Path>* products = settings.GetRelationship("products")) {
    for (const Path& path : *products) {
      const UsdPrim product = stage.GetPrimAtPath(path);
      if (!IsRenderProduct(product)) continue;
      RenderSpecProduct p = base;
      p.path = path.str();
      ReadSpecBase(product, true, &p);
      // A product without a resolvable Camera is skipped (UsdRenderComputeSpec).
      const UsdPrim cam = p.camera.empty() ? UsdPrim() : stage.GetPrimAtPath(p.camera);
      if (!cam.IsValid() || cam.GetTypeName() != "Camera") continue;
      {
        const Value* v = cam.GetPropertyValue("horizontalAperture");
        if (v && v->as_float()) p.aperture[0] = *v->as_float();
        v = cam.GetPropertyValue("verticalAperture");
        if (v && v->as_float()) p.aperture[1] = *v->as_float();
      }
      ApplyConformPolicy(&p);
      if (const std::vector<Path>* vars = product.GetRelationship("orderedVars")) {
        for (const Path& var : *vars)
          if (IsRenderVar(stage.GetPrimAtPath(var))) p.render_vars.push_back(var.str());
      }
      out->products.push_back(std::move(p));
    }
  }
  if (const Value* v = settings.GetPropertyValue("includedPurposes")) {
    if (auto tokens = v->as_token_array())
      for (const auto& t : *tokens) out->included_purposes.emplace_back(t);
  }
  return true;
}

}  // namespace lightusd
