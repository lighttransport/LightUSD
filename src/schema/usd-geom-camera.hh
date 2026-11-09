// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomCamera Schema API
// Convenience accessors for USD camera

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"

#include <string>

namespace lightusd {
namespace next {

/// Camera projection type
enum class CameraProjection {
  Perspective,
  Orthographic
};

/// Camera data
struct CameraData {
  CameraProjection projection = CameraProjection::Perspective;

  // Lens properties
  float focal_length = 50.0f;         // mm
  float horizontal_aperture = 36.0f;  // mm (35mm full frame width)
  float vertical_aperture = 24.0f;    // mm (35mm full frame height)
  float horizontal_aperture_offset = 0.0f;
  float vertical_aperture_offset = 0.0f;

  // Focus
  float focus_distance = 0.0f;        // Distance to focus plane (0 = infinity)
  float f_stop = 0.0f;                // F-stop (0 = no DOF)

  // Clipping
  float clipping_near = 0.1f;
  float clipping_far = 10000.0f;

  // Computed values (derived from above)
  float fov_horizontal = 0.0f;        // In radians
  float fov_vertical = 0.0f;          // In radians
  float aspect_ratio = 1.5f;
};

struct BackPlateData {
  std::string image;
  std::string alpha_image;
  std::string depth_image;
  float depth_min_offset = 0.0f;
  float depth_normalizing_factor = 1.0f;
  float depth_camera_space_offset = 0.0f;
  float scale_tweak[2] = {1.0f, 1.0f};
  float rotate_xyz_tweak[3] = {0.0f, 0.0f, 0.0f};
  float translate_tweak[3] = {0.0f, 0.0f, 0.0f};
  float luma_gain[3] = {1.0f, 1.0f, 1.0f};
  float luma_lift[3] = {0.0f, 0.0f, 0.0f};
  float luma_gamma[3] = {1.0f, 1.0f, 1.0f};
  std::string plate_visibility = "solo";
};

/// Check if prim is a Camera
bool IsCamera(const UsdPrim& prim);

/// Get camera data
bool GetCameraData(const Stage& stage, const UsdPrim& prim,
                   CameraData* out, double time = 0.0);

bool HasBackPlateAPI(const UsdPrim& prim, const std::string& instance_name);

/// Evaluate one BackPlateAPI instance without loading its referenced images.
bool GetBackPlateData(const Stage& stage, const UsdPrim& prim,
                      const std::string& instance_name, BackPlateData* out,
                      double time = 0.0);

/// Get focal length in mm
float GetFocalLength(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get horizontal aperture in mm
float GetHorizontalAperture(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get vertical aperture in mm
float GetVerticalAperture(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get focus distance
float GetFocusDistance(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get f-stop
float GetFStop(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get near clipping distance
float GetClippingNear(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get far clipping distance
float GetClippingFar(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Compute horizontal field of view in radians
float ComputeHorizontalFOV(float focal_length_mm, float horizontal_aperture_mm);

/// Compute vertical field of view in radians
float ComputeVerticalFOV(float focal_length_mm, float vertical_aperture_mm);

/// Compute perspective projection matrix (column-major, OpenGL style)
void ComputeProjectionMatrix(const CameraData& camera, float* matrix16);

/// Compute perspective projection matrix with custom aspect ratio
void ComputeProjectionMatrix(const CameraData& camera, float aspect_ratio, float* matrix16);

}  // namespace next
}  // namespace lightusd
