// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdLux Schema Implementation

#include "usd-lux.hh"

namespace lightusd {
namespace next {

LightType GetLightType(const UsdPrim& prim) {
  if (!prim.IsValid()) return LightType::Unknown;

  const std::string& type = prim.GetTypeName();

  if (type == "DistantLight") return LightType::DistantLight;
  if (type == "DomeLight") return LightType::DomeLight;
  if (type == "RectLight") return LightType::RectLight;
  if (type == "DiskLight") return LightType::DiskLight;
  if (type == "SphereLight") return LightType::SphereLight;
  if (type == "CylinderLight") return LightType::CylinderLight;

  return LightType::Unknown;
}

bool IsLight(const UsdPrim& prim) {
  return GetLightType(prim) != LightType::Unknown;
}

bool IsDistantLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "DistantLight";
}

bool IsDomeLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "DomeLight";
}

bool IsRectLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "RectLight";
}

bool IsDiskLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "DiskLight";
}

bool IsSphereLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "SphereLight";
}

bool IsCylinderLight(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "CylinderLight";
}

bool GetLightData(const Stage& stage, const UsdPrim& prim,
                  LightData* out, double time) {
  if (!prim.IsValid() || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  const float default_intensity =
      GetLightType(prim) == LightType::DistantLight ? 50000.0f : 1.0f;
  out->intensity =
      eval.EvalOr(prim, "inputs:intensity", default_intensity);
  out->exposure = eval.EvalOr(prim, "inputs:exposure", 0.0f);
  out->diffuse = eval.EvalOr(prim, "inputs:diffuse", 1.0f);
  out->specular = eval.EvalOr(prim, "inputs:specular", 1.0f);
  out->normalize = eval.EvalOr(prim, "inputs:normalize", false);

  float color[3] = {1.0f, 1.0f, 1.0f};
  if (eval.EvalFloat3(prim, "inputs:color", color)) {
    out->color[0] = color[0];
    out->color[1] = color[1];
    out->color[2] = color[2];
  }

  // ShadowAPI
  out->shadow_enable = eval.EvalOr(prim, "inputs:shadow:enable", true);
  float shadow_color[3] = {0.0f, 0.0f, 0.0f};
  if (eval.EvalFloat3(prim, "inputs:shadow:color", shadow_color)) {
    out->shadow_color[0] = shadow_color[0];
    out->shadow_color[1] = shadow_color[1];
    out->shadow_color[2] = shadow_color[2];
  }
  out->shadow_distance = eval.EvalOr(prim, "inputs:shadow:distance", -1.0f);
  out->shadow_falloff = eval.EvalOr(prim, "inputs:shadow:falloff", -1.0f);
  out->shadow_falloff_gamma =
      eval.EvalOr(prim, "inputs:shadow:falloffGamma", 1.0f);

  return true;
}

float GetLightIntensity(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  const float default_intensity =
      GetLightType(prim) == LightType::DistantLight ? 50000.0f : 1.0f;
  return eval.EvalOr(prim, "inputs:intensity", default_intensity);
}

bool GetLightColor(const Stage& stage, const UsdPrim& prim,
                   float* r, float* g, float* b, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  float color[3];
  if (eval.EvalFloat3(prim, "inputs:color", color)) {
    *r = color[0];
    *g = color[1];
    *b = color[2];
    return true;
  }
  *r = *g = *b = 1.0f;
  return false;
}

float GetLightExposure(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "inputs:exposure", 0.0f);
}

bool GetDistantLightData(const Stage& stage, const UsdPrim& prim,
                         DistantLightData* out, double time) {
  if (!IsDistantLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->angle = eval.EvalOr(prim, "inputs:angle", 0.53f);

  return true;
}

bool GetDomeLightData(const Stage& stage, const UsdPrim& prim,
                      DomeLightData* out, double time) {
  if (!IsDomeLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);

  auto tex = eval.EvalAssetPath(prim, "inputs:texture:file");
  if (tex) out->texture_file = *tex;

  // inputs:texture:format token -> 0=automatic, 1=latlong, 2=mirroredBall,
  // 3=angular (matches the DomeLightData field comment).
  if (auto format = eval.EvalToken(prim, "inputs:texture:format")) {
    if (*format == "latlong") out->texture_format = 1;
    else if (*format == "mirroredBall") out->texture_format = 2;
    else if (*format == "angular") out->texture_format = 3;
    else out->texture_format = 0;
  }

  return true;
}

bool GetRectLightData(const Stage& stage, const UsdPrim& prim,
                      RectLightData* out, double time) {
  if (!IsRectLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->width = eval.EvalOr(prim, "inputs:width", 1.0f);
  out->height = eval.EvalOr(prim, "inputs:height", 1.0f);

  auto tex = eval.EvalAssetPath(prim, "inputs:texture:file");
  if (tex) out->texture_file = *tex;

  return true;
}

bool GetDiskLightData(const Stage& stage, const UsdPrim& prim,
                      DiskLightData* out, double time) {
  if (!IsDiskLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->radius = eval.EvalOr(prim, "inputs:radius", 0.5f);

  return true;
}

bool GetSphereLightData(const Stage& stage, const UsdPrim& prim,
                        SphereLightData* out, double time) {
  if (!IsSphereLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->radius = eval.EvalOr(prim, "inputs:radius", 0.5f);
  out->treat_as_point = eval.EvalOr(prim, "treatAsPoint", false);

  return true;
}

bool GetCylinderLightData(const Stage& stage, const UsdPrim& prim,
                          CylinderLightData* out, double time) {
  if (!IsCylinderLight(prim) || !out) return false;

  GetLightData(stage, prim, out, time);

  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->radius = eval.EvalOr(prim, "inputs:radius", 0.5f);
  out->length = eval.EvalOr(prim, "inputs:length", 1.0f);
  out->treat_as_line = eval.EvalOr(prim, "treatAsLine", false);

  return true;
}

}  // namespace next
}  // namespace lightusd
