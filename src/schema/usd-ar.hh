// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdAR Schema

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <string>
#include <vector>

namespace lightusd {
namespace next {

// ============================================================
// Typed prims
// ============================================================

bool IsARAnchor(const UsdPrim& prim);
bool IsARImage(const UsdPrim& prim);
bool IsARFaceGeometry(const UsdPrim& prim);
bool IsARPlane(const UsdPrim& prim);
bool IsARPointCloud(const UsdPrim& prim);
bool IsARDevice(const UsdPrim& prim);
bool IsARScene(const UsdPrim& prim);

// ============================================================
// ARAnchor
// ============================================================

struct ARAnchorData {
  std::vector<float> location;  // vector3f
  std::vector<float> orientation; // quatf
  std::vector<float> scale;     // vector3f
  bool tracked = true;
};

bool GetARAnchorData(const Stage& stage, const UsdPrim& prim,
                     ARAnchorData* out, double time = 0.0);

// ============================================================
// ARImage
// ============================================================

struct ARImageData {
  std::string imageAsset; // asset path
  float physicalWidth = 0.0f; // in meters
  float physicalHeight = 0.0f;
};

bool GetARImageData(const Stage& stage, const UsdPrim& prim,
                    ARImageData* out);

// ============================================================
// ARFaceGeometry
// ============================================================

struct ARFaceGeometryData {
  std::vector<float> blendShapeCoefficients; // float[]
  std::vector<std::string> blendShapeNames;  // uniform token[]
};

bool GetARFaceGeometryData(const Stage& stage, const UsdPrim& prim,
                            ARFaceGeometryData* out, double time = 0.0);

// ============================================================
// ARPlane
// ============================================================

struct ARPlaneData {
  std::string semanticType; // "Horizontal", "Vertical", "Ceiling", "Floor", "Wall"
  std::vector<float> extent; // vector2f
  std::vector<float> boundary; // vector3f[] (outline polygon)
  std::string parentPlane; // rel ar:parentPlane
};

bool GetARPlaneData(const Stage& stage, const UsdPrim& prim,
                    ARPlaneData* out);

// ============================================================
// ARPointCloud
// ============================================================

struct ARPointCloudData {
  std::vector<float> points; // point3f[]
  std::vector<float> normals; // normal3f[]
  std::vector<float> confidences; // float[]
  uint64_t timestamp = 0;
};

bool GetARPointCloudData(const Stage& stage, const UsdPrim& prim,
                          ARPointCloudData* out, double time = 0.0);

// ============================================================
// ARDevice
// ============================================================

struct ARDeviceData {
  float fieldOfView = 0.0f;
  float aspectRatio = 0.0f;
  float nearPlane = 0.01f;
  float farPlane = 1000.0f;
  std::vector<float> resolution; // int2 (width, height)
};

bool GetARDeviceData(const Stage& stage, const UsdPrim& prim,
                     ARDeviceData* out);

// ============================================================
// ARScene (marker — no properties)
// ============================================================

bool IsARScene(const UsdPrim& prim);

// ============================================================
// Applied API schemas
// ============================================================

bool HasARAnchorAPI(const UsdPrim& prim);
bool HasARImagingAPI(const UsdPrim& prim);

struct ARAnchorAPIData {
  bool tracked = true;
  std::string anchorName;
};

bool GetARAnchorAPIData(const UsdPrim& prim, ARAnchorAPIData* out);

struct ARImagingAPIData {
  std::string imageAsset;
  float physicalWidth = 0.0f;
};

bool GetARImagingAPIData(const UsdPrim& prim, ARImagingAPIData* out);

} // namespace next
} // namespace lightusd
