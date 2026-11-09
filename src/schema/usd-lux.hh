// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdLux Schema APIs
// Convenience accessors for USD light types

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"

namespace lightusd {

/// Light type enumeration
enum class LightType {
  Unknown,
  DistantLight,
  DomeLight,
  RectLight,
  DiskLight,
  SphereLight,
  CylinderLight
};

/// Get light type from prim
LightType GetLightType(const UsdPrim& prim);

/// Check if prim is any light type
bool IsLight(const UsdPrim& prim);

/// Light data struct (common properties)
struct LightData {
  float intensity = 1.0f;
  float exposure = 0.0f;
  float color[3] = {1.0f, 1.0f, 1.0f};
  float diffuse = 1.0f;
  float specular = 1.0f;
  bool normalize = false;
  // ShadowAPI (inputs:shadow:*). Distance/falloff use the UsdLux -1
  // "unset/infinite" sentinel.
  bool shadow_enable = true;
  float shadow_color[3] = {0.0f, 0.0f, 0.0f};
  float shadow_distance = -1.0f;
  float shadow_falloff = -1.0f;
  float shadow_falloff_gamma = 1.0f;
};

// ============================================================
// Base light accessors
// ============================================================

/// Get common light properties
bool GetLightData(const Stage& stage, const UsdPrim& prim,
                  LightData* out, double time = 0.0);

/// Get light intensity
float GetLightIntensity(const Stage& stage, const UsdPrim& prim, double time = 0.0);

/// Get light color
bool GetLightColor(const Stage& stage, const UsdPrim& prim,
                   float* r, float* g, float* b, double time = 0.0);

/// Get light exposure
float GetLightExposure(const Stage& stage, const UsdPrim& prim, double time = 0.0);

// ============================================================
// Distant light
// ============================================================

struct DistantLightData : LightData {
  float angle = 0.53f;  // Angular diameter of the sun in degrees
};

bool IsDistantLight(const UsdPrim& prim);
bool GetDistantLightData(const Stage& stage, const UsdPrim& prim,
                         DistantLightData* out, double time = 0.0);

// ============================================================
// Dome light
// ============================================================

struct DomeLightData : LightData {
  std::string texture_file;
  float texture_format = 0;  // 0=automatic, 1=latlong, 2=mirroredBall, 3=angular
};

bool IsDomeLight(const UsdPrim& prim);
bool GetDomeLightData(const Stage& stage, const UsdPrim& prim,
                      DomeLightData* out, double time = 0.0);

// ============================================================
// Rect light
// ============================================================

struct RectLightData : LightData {
  float width = 1.0f;
  float height = 1.0f;
  std::string texture_file;
};

bool IsRectLight(const UsdPrim& prim);
bool GetRectLightData(const Stage& stage, const UsdPrim& prim,
                      RectLightData* out, double time = 0.0);

// ============================================================
// Disk light
// ============================================================

struct DiskLightData : LightData {
  float radius = 0.5f;
};

bool IsDiskLight(const UsdPrim& prim);
bool GetDiskLightData(const Stage& stage, const UsdPrim& prim,
                      DiskLightData* out, double time = 0.0);

// ============================================================
// Sphere light
// ============================================================

struct SphereLightData : LightData {
  float radius = 0.5f;
  bool treat_as_point = false;
};

bool IsSphereLight(const UsdPrim& prim);
bool GetSphereLightData(const Stage& stage, const UsdPrim& prim,
                        SphereLightData* out, double time = 0.0);

// ============================================================
// Cylinder light
// ============================================================

struct CylinderLightData : LightData {
  float radius = 0.5f;
  float length = 1.0f;
  bool treat_as_line = false;
};

bool IsCylinderLight(const UsdPrim& prim);
bool GetCylinderLightData(const Stage& stage, const UsdPrim& prim,
                          CylinderLightData* out, double time = 0.0);

}  // namespace lightusd
