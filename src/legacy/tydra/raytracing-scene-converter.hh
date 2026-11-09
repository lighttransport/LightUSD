// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// RaytracingSceneConverter: Convert USD Stage to RaytracingScene
//
#pragma once

#include <string>
#include <vector>

#include "raytracing-data.hh"
#include "lightusd.hh"
#include "usdGeom.hh"   // GeomMesh (no longer re-exported by lightusd.hh)
#include "usdShade.hh"  // Material
#include "value-types.hh"

namespace lightusd {
namespace tydra {

// Forward declarations
class AssetResolutionResolver;

///
/// Configuration for raytracing scene conversion
///
struct RaytracingSceneConverterConfig {
  // Geometry options
  bool triangulate = true;  // Convert all geometry to triangles (if false, preserve quads)
  bool allow_quads = true;   // Allow quad faces (use ray-quad intersection to save tessellation)
  bool allow_mixed = false;  // Allow mixed tri/quad meshes

  bool convert_primitives_to_analytic = true;  // Convert Sphere/Cylinder/Capsule/Cone to analytic primitives
  bool tessellate_primitives = false;  // If true, tessellate primitives to meshes instead

  bool compute_normals_if_missing = true;  // Auto-generate smooth normals
  bool flip_normals = false;  // Flip normal direction
  bool compute_tangents = false;  // Compute tangent vectors for normal mapping

  // Material options
  bool convert_materials = true;  // Convert USD materials to RTMaterial
  bool flatten_material_network = true;  // Flatten shader networks to simple PBR

  // Light options
  bool convert_lights = true;  // Convert USD lights
  bool create_envmap_sampling = true;  // Build importance sampling for environment maps

  // Instance options
  bool flatten_instances = false;  // If true, bake instances into unique geometry

  // Acceleration structure
  bool build_acceleration_structure = true;  // Build BVH after conversion
  RTAccelerationStructure::BuildConfig accel_config;

  // Memory and performance
  size_t max_triangles_per_mesh = 10000000;  // 10M triangles max (or quads if allow_quads=true)
  bool verbose = false;
};

///
/// Environment for raytracing scene conversion
///
class RaytracingSceneConverterEnv {
 public:
  RaytracingSceneConverterEnv(const Stage& _stage) : stage(_stage) {}

  RaytracingSceneConverterConfig config;

  std::string usd_filename;  // Corresponding USD filename

  const Stage& stage;  // Reference to valid Stage object

  double timecode{value::TimeCode::Default()};
  value::TimeSampleInterpolationType tinterp{
      value::TimeSampleInterpolationType::Linear};
};

///
/// Callback for progress monitoring during conversion
///
/// @param[in] progress Progress value [0.0, 1.0]
/// @param[in] message Status message
/// @param[in] userptr User data pointer
/// @return true to continue, false to cancel
///
using RaytracingProgressCallback = bool (*)(float progress,
                                             const std::string& message,
                                             void* userptr);

///
/// Convert USD Stage to RaytracingScene
///
/// Note: In raytracing, primvars are fetched at intersection time,
/// so no preprocessing for variability is required. This makes the
/// converter simpler than RenderSceneConverter.
///
class RaytracingSceneConverter {
 public:
  RaytracingSceneConverter() = default;
  RaytracingSceneConverter(const RaytracingSceneConverter& rhs) = delete;
  RaytracingSceneConverter(RaytracingSceneConverter&& rhs) = delete;

  ///
  /// Set progress callback for monitoring conversion
  ///
  void SetProgressCallback(RaytracingProgressCallback callback,
                          void* userptr = nullptr);

  ///
  /// Convert Stage to RaytracingScene
  ///
  /// @param[in] env Conversion environment with config and stage
  /// @param[out] scene Output raytracing scene
  /// @return true on success, false on error
  ///
  bool ConvertToRaytracingScene(const RaytracingSceneConverterEnv& env,
                                 RaytracingScene* scene);

  ///
  /// Get conversion info/warning/error messages
  ///
  const std::string& GetInfo() const { return _info; }
  const std::string& GetWarning() const { return _warn; }
  const std::string& GetError() const { return _err; }

  ///
  /// Clear all conversion state
  ///
  void Clear();

 private:
  // Conversion helpers
  bool ConvertGeometry(const RaytracingSceneConverterEnv& env,
                       RaytracingScene* scene);
  bool ConvertMaterials(const RaytracingSceneConverterEnv& env,
                        RaytracingScene* scene);
  bool ConvertLights(const RaytracingSceneConverterEnv& env,
                     RaytracingScene* scene);
  bool ConvertCameras(const RaytracingSceneConverterEnv& env,
                      RaytracingScene* scene);
  bool ConvertInstances(const RaytracingSceneConverterEnv& env,
                        RaytracingScene* scene);

  // Mesh conversion
  bool ConvertMesh(const GeomMesh& mesh,
                   const RaytracingSceneConverterEnv& env,
                   RTGeometry* geom);

  // Material conversion
  bool ConvertMaterial(const Material& material,
                       const RaytracingSceneConverterEnv& env,
                       RTMaterial* mat);

  // Light conversion
  bool ConvertLight(const Prim& light_prim,
                    const RaytracingSceneConverterEnv& env,
                    RTLight* light);

  // Camera conversion
  bool ConvertCamera(const Prim& camera_prim,
                     const RaytracingSceneConverterEnv& env,
                     RTCamera* camera);

  // Utility functions
  bool TriangulateMesh(RTGeometry* geom);
  bool ComputeSmoothNormals(RTGeometry* geom);
  bool ComputeTangents(RTGeometry* geom);

  // Progress reporting
  bool ReportProgress(float progress, const std::string& message);

  // State
  RaytracingProgressCallback _progress_callback{nullptr};
  void* _progress_userptr{nullptr};

  std::string _info;
  std::string _warn;
  std::string _err;
};

}  // namespace tydra
}  // namespace lightusd
