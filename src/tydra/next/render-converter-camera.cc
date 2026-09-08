// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Tydra Next - camera conversion
#include "render-converter.hh"
#include "next/schema/usd-geom-camera.hh"
#include <cstring>
#include <string>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::Stage; using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {

bool GetFloat(const UsdPrim& prim, const char* name, float* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const float* result = value->as_float()) { *out = *result; return true; }
  if (const double* result = value->as_double()) { *out = static_cast<float>(*result); return true; }
  return false;
}
bool GetDouble(const UsdPrim& prim, const char* name, double* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const double* result = value->as_double()) { *out = *result; return true; }
  if (const float* result = value->as_float()) { *out = *result; return true; }
  return false;
}
bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const std::string* result = value->as_token()) { *out = *result; return true; }
  if (const std::string* result = value->as_string()) { *out = *result; return true; }
  return false;
}

}  // namespace

bool RenderSceneConverter::ConvertCamera(const Stage& stage,
                                         const UsdPrim& prim,
                                         RenderCamera* out) {
  if (!out || !::lightusd::tydra::next::IsCamera(prim)) {
    SetLastError("Invalid camera prim");
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();

  // Projection type
  std::string projection;
  GetToken(prim, "projection", &projection);
  out->type = (projection == "orthographic") ?
              CameraType::Orthographic : CameraType::Perspective;

  // Lens parameters
  GetFloat(prim, "focalLength", &out->focal_length);
  GetFloat(prim, "horizontalAperture", &out->horizontal_aperture);
  GetFloat(prim, "verticalAperture", &out->vertical_aperture);
  GetFloat(prim, "horizontalApertureOffset",
           &out->horizontal_aperture_offset);
  GetFloat(prim, "verticalApertureOffset", &out->vertical_aperture_offset);

  // Clipping
  float clip_range[2] = {0.1f, 10000.0f};
  const Value* clip_val = prim.GetPropertyValue("clippingRange");
  if (clip_val) {
    const float* cr = clip_val->as_float2();
    if (cr) {
      clip_range[0] = cr[0];
      clip_range[1] = cr[1];
    }
  }
  out->near_clip = clip_range[0];
  out->far_clip = clip_range[1];

  // Depth of field / exposure
  GetFloat(prim, "focusDistance", &out->focus_distance);
  GetFloat(prim, "fStop", &out->fstop);
  GetFloat(prim, "exposure", &out->exposure);

  std::string stereo_role;
  if (GetToken(prim, "stereoRole", &stereo_role)) {
    if (stereo_role == "left") {
      out->stereo_role = RenderCamera::StereoRole::Left;
    } else if (stereo_role == "right") {
      out->stereo_role = RenderCamera::StereoRole::Right;
    }
  }
  if (const Value* planes = GetAttribute(prim, "clippingPlanes")) {
    if (const std::vector<float>* values = planes->as_float_array()) {
      out->clipping_planes.reserve(values->size() / 4);
      for (size_t i = 0; i + 3 < values->size(); i += 4) {
        out->clipping_planes.emplace_back(
            (*values)[i], (*values)[i + 1], (*values)[i + 2],
            (*values)[i + 3]);
      }
    }
  }

  // Motion-blur shutter interval
  GetDouble(prim, "shutter:open", &out->shutter_open);
  GetDouble(prim, "shutter:close", &out->shutter_close);

  // BackPlateAPI is a multiple-apply schema. Preserve every applied instance
  // in authored order; image decoding/compositing belongs to the backend.
  constexpr const char* kBackPlatePrefix = "BackPlateAPI:";
  for (const std::string& schema : prim.GetMeta().apiSchemas()) {
    if (schema.compare(0, std::strlen(kBackPlatePrefix), kBackPlatePrefix) != 0)
      continue;
    const std::string instance = schema.substr(std::strlen(kBackPlatePrefix));
    ::lightusd::next::BackPlateData source;
    if (!::lightusd::next::GetBackPlateData(stage, prim, instance, &source,
                                             config_.time_code)) {
      continue;
    }
    RenderBackPlate plate;
    plate.instance_name = instance;
    plate.image = source.image;
    plate.alpha_image = source.alpha_image;
    plate.depth_image = source.depth_image;
    plate.depth_min_offset = source.depth_min_offset;
    plate.depth_normalizing_factor = source.depth_normalizing_factor;
    plate.depth_camera_space_offset = source.depth_camera_space_offset;
    plate.scale_tweak = {source.scale_tweak[0], source.scale_tweak[1]};
    plate.rotate_xyz_tweak = {source.rotate_xyz_tweak[0],
                              source.rotate_xyz_tweak[1],
                              source.rotate_xyz_tweak[2]};
    plate.translate_tweak = {source.translate_tweak[0],
                             source.translate_tweak[1],
                             source.translate_tweak[2]};
    plate.luma_gain = {source.luma_gain[0], source.luma_gain[1],
                       source.luma_gain[2]};
    plate.luma_lift = {source.luma_lift[0], source.luma_lift[1],
                       source.luma_lift[2]};
    plate.luma_gamma = {source.luma_gamma[0], source.luma_gamma[1],
                        source.luma_gamma[2]};
    plate.plate_visibility = source.plate_visibility;
    out->back_plates.push_back(std::move(plate));
  }

  return true;
}
}}}  // namespace lightusd::tydra::next
