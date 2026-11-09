// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomCamera Schema Implementation

#include "usd-geom-camera.hh"
#include "../layer/prim-spec.hh"
#include "../prim/identifier.hh"
#include <cmath>

namespace lightusd {
namespace next {

namespace {
constexpr float kPI = 3.14159265358979323846f;

std::string BackPlateName(const std::string& instance_name,
                          const char* suffix) {
  return "backPlate:" + instance_name + ":" + suffix;
}
}

bool IsCamera(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Camera";
}

bool GetCameraData(const Stage& stage, const UsdPrim& prim,
                   CameraData* out, double time) {
  if (!IsCamera(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // Projection type
  auto proj = eval.EvalToken(prim, "projection");
  if (proj && *proj == "orthographic") {
    out->projection = CameraProjection::Orthographic;
  } else {
    out->projection = CameraProjection::Perspective;
  }

  // Lens properties
  out->focal_length = eval.EvalOr(prim, "focalLength", 50.0f);
  out->horizontal_aperture = eval.EvalOr(prim, "horizontalAperture", 36.0f);
  out->vertical_aperture = eval.EvalOr(prim, "verticalAperture", 24.0f);
  out->horizontal_aperture_offset = eval.EvalOr(prim, "horizontalApertureOffset", 0.0f);
  out->vertical_aperture_offset = eval.EvalOr(prim, "verticalApertureOffset", 0.0f);

  // Focus
  out->focus_distance = eval.EvalOr(prim, "focusDistance", 0.0f);
  out->f_stop = eval.EvalOr(prim, "fStop", 0.0f);

  // Clipping
  float clipping[2] = {0.1f, 10000.0f};
  if (eval.EvalFloat2(prim, "clippingRange", clipping)) {
    out->clipping_near = clipping[0];
    out->clipping_far = clipping[1];
  } else {
    out->clipping_near = 0.1f;
    out->clipping_far = 10000.0f;
  }

  // Compute derived values
  out->fov_horizontal = ComputeHorizontalFOV(out->focal_length, out->horizontal_aperture);
  out->fov_vertical = ComputeVerticalFOV(out->focal_length, out->vertical_aperture);
  out->aspect_ratio = out->horizontal_aperture / out->vertical_aperture;

  return true;
}

bool HasBackPlateAPI(const UsdPrim& prim, const std::string& instance_name) {
  if (!prim.IsValid() ||
      !IsValidNamespacedIdentifier(instance_name)) return false;
  const std::string schema = "BackPlateAPI:" + instance_name;
  for (const std::string& applied : prim.GetMeta().apiSchemas()) {
    if (applied == schema) return true;
  }
  return false;
}

bool GetBackPlateData(const Stage& stage, const UsdPrim& prim,
                      const std::string& instance_name, BackPlateData* out,
                      double time) {
  if (!out || !HasBackPlateAPI(prim, instance_name)) return false;
  *out = BackPlateData{};
  AttributeEval eval(&stage);
  eval.SetTime(time);

  auto asset = [&](const char* suffix, std::string* dst) {
    if (const auto value =
            eval.EvalAssetPath(prim, BackPlateName(instance_name, suffix))) {
      *dst = *value;
    }
  };
  asset("image", &out->image);
  asset("alpha:image", &out->alpha_image);
  asset("depth:image", &out->depth_image);
  out->depth_min_offset = eval.EvalOr(
      prim, BackPlateName(instance_name, "depth:minOffset"), 0.0f);
  out->depth_normalizing_factor = eval.EvalOr(
      prim, BackPlateName(instance_name, "depth:normalizingFactor"), 1.0f);
  out->depth_camera_space_offset = eval.EvalOr(
      prim, BackPlateName(instance_name, "depth:cameraSpaceOffset"), 0.0f);
  eval.EvalFloat2(prim, BackPlateName(instance_name, "scale:tweak"),
                  out->scale_tweak);
  eval.EvalFloat3(prim, BackPlateName(instance_name, "rotateXYZ:tweak"),
                  out->rotate_xyz_tweak);
  eval.EvalFloat3(prim, BackPlateName(instance_name, "translate:tweak"),
                  out->translate_tweak);
  eval.EvalFloat3(prim, BackPlateName(instance_name, "luma:gain"),
                  out->luma_gain);
  eval.EvalFloat3(prim, BackPlateName(instance_name, "luma:lift"),
                  out->luma_lift);
  eval.EvalFloat3(prim, BackPlateName(instance_name, "luma:gamma"),
                  out->luma_gamma);
  if (const auto value = eval.EvalToken(
          prim, BackPlateName(instance_name, "plateVisibility"))) {
    out->plate_visibility = *value;
  }
  return true;
}

float GetFocalLength(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "focalLength", 50.0f);
}

float GetHorizontalAperture(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "horizontalAperture", 36.0f);
}

float GetVerticalAperture(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "verticalAperture", 24.0f);
}

float GetFocusDistance(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "focusDistance", 0.0f);
}

float GetFStop(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalOr(prim, "fStop", 0.0f);
}

float GetClippingNear(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  float clipping[2] = {0.1f, 10000.0f};
  eval.EvalFloat2(prim, "clippingRange", clipping);
  return clipping[0];
}

float GetClippingFar(const Stage& stage, const UsdPrim& prim, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  float clipping[2] = {0.1f, 10000.0f};
  eval.EvalFloat2(prim, "clippingRange", clipping);
  return clipping[1];
}

float ComputeHorizontalFOV(float focal_length_mm, float horizontal_aperture_mm) {
  // FOV = 2 * atan(aperture / (2 * focal_length))
  if (focal_length_mm <= 0.0f) return kPI / 4.0f;  // 45 degrees default
  return 2.0f * std::atan(horizontal_aperture_mm / (2.0f * focal_length_mm));
}

float ComputeVerticalFOV(float focal_length_mm, float vertical_aperture_mm) {
  if (focal_length_mm <= 0.0f) return kPI / 4.0f;
  return 2.0f * std::atan(vertical_aperture_mm / (2.0f * focal_length_mm));
}

void ComputeProjectionMatrix(const CameraData& camera, float* matrix16) {
  ComputeProjectionMatrix(camera, camera.aspect_ratio, matrix16);
}

void ComputeProjectionMatrix(const CameraData& camera, float aspect_ratio, float* matrix16) {
  // Initialize to zero
  for (int i = 0; i < 16; ++i) {
    matrix16[i] = 0.0f;
  }

  if (camera.projection == CameraProjection::Orthographic) {
    // Orthographic projection
    float width = camera.horizontal_aperture;
    float height = camera.vertical_aperture;
    float n = camera.clipping_near;
    float f = camera.clipping_far;

    matrix16[0] = 2.0f / width;
    matrix16[5] = 2.0f / height;
    matrix16[10] = -2.0f / (f - n);
    matrix16[14] = -(f + n) / (f - n);
    matrix16[15] = 1.0f;
  } else {
    // Perspective projection
    float fov_y = camera.fov_vertical;
    float n = camera.clipping_near;
    float f = camera.clipping_far;

    float tan_half_fov = std::tan(fov_y / 2.0f);

    // Column-major order (OpenGL style)
    matrix16[0] = 1.0f / (aspect_ratio * tan_half_fov);
    matrix16[5] = 1.0f / tan_half_fov;
    matrix16[10] = -(f + n) / (f - n);
    matrix16[11] = -1.0f;
    matrix16[14] = -(2.0f * f * n) / (f - n);
  }
}

}  // namespace next
}  // namespace lightusd
