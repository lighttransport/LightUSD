// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.

#include "raytracing-scene-converter.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "core/prim.hh"
#include "scene-access.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

void RaytracingSceneConverter::SetProgressCallback(
    RaytracingProgressCallback callback, void* userptr) {
  _progress_callback = callback;
  _progress_userptr = userptr;
}

bool RaytracingSceneConverter::ReportProgress(float progress,
                                               const std::string& message) {
  if (_progress_callback) {
    return _progress_callback(progress, message, _progress_userptr);
  }
  return true;  // Continue
}

void RaytracingSceneConverter::Clear() {
  _info.clear();
  _warn.clear();
  _err.clear();
}

bool RaytracingSceneConverter::ConvertToRaytracingScene(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  if (!scene) {
    _err = "Output scene pointer is null\n";
    return false;
  }

  Clear();

  // Clear output scene
  scene->geometries.clear();
  scene->materials.clear();
  scene->lights.clear();
  scene->cameras.clear();
  scene->instances.clear();

  if (!ReportProgress(0.0f, "Starting raytracing scene conversion")) {
    _warn += "Conversion cancelled by user\n";
    return false;
  }

  // Convert materials first (needed for geometry)
  if (env.config.convert_materials) {
    if (!ReportProgress(0.1f, "Converting materials")) {
      return false;
    }
    if (!ConvertMaterials(env, scene)) {
      _err += "Failed to convert materials\n";
      return false;
    }
  }

  // Convert geometry
  if (!ReportProgress(0.3f, "Converting geometry")) {
    return false;
  }
  if (!ConvertGeometry(env, scene)) {
    _err += "Failed to convert geometry\n";
    return false;
  }

  // Convert lights
  if (env.config.convert_lights) {
    if (!ReportProgress(0.6f, "Converting lights")) {
      return false;
    }
    if (!ConvertLights(env, scene)) {
      _err += "Failed to convert lights\n";
      return false;
    }
  }

  // Convert cameras
  if (!ReportProgress(0.7f, "Converting cameras")) {
    return false;
  }
  if (!ConvertCameras(env, scene)) {
    _err += "Failed to convert cameras\n";
    return false;
  }

  // Convert instances
  if (!ReportProgress(0.8f, "Converting instances")) {
    return false;
  }
  if (!ConvertInstances(env, scene)) {
    _err += "Failed to convert instances\n";
    return false;
  }

  // Build acceleration structure
  if (env.config.build_acceleration_structure) {
    if (!ReportProgress(0.9f, "Building acceleration structure")) {
      return false;
    }
    if (!scene->build_acceleration_structure(env.config.accel_config)) {
      _err += "Failed to build acceleration structure\n";
      return false;
    }
  }

  // Validate scene
  std::string validate_warn, validate_err;
  if (!scene->validate(&validate_warn, &validate_err)) {
    _err += "Scene validation failed:\n" + validate_err;
    return false;
  }
  if (!validate_warn.empty()) {
    _warn += "Scene validation warnings:\n" + validate_warn;
  }

  if (!ReportProgress(1.0f, "Conversion complete")) {
    return false;
  }

  _info += "Raytracing scene conversion successful\n";
  _info += "  Geometries: " + std::to_string(scene->geometries.size()) + "\n";
  _info += "  Materials: " + std::to_string(scene->materials.size()) + "\n";
  _info += "  Lights: " + std::to_string(scene->lights.size()) + "\n";
  _info += "  Cameras: " + std::to_string(scene->cameras.size()) + "\n";
  _info += "  Instances: " + std::to_string(scene->instances.size()) + "\n";

  size_t mem_usage = scene->estimate_memory_usage();
  _info += "  Estimated memory: " + std::to_string(mem_usage / (1024 * 1024)) +
           " MB\n";

  return true;
}

bool RaytracingSceneConverter::ConvertGeometry(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  (void)env;
  (void)scene;

  // TODO: Implement geometry conversion
  // - Traverse USD stage for GeomMesh prims
  // - For each mesh, extract vertices, indices, normals, UVs
  // - No primvar variability preprocessing needed (fetched at intersection)
  // - Triangulate if needed
  // - Compute bounds

  _info += "Geometry conversion not yet implemented\n";
  return true;  // Return true for now (placeholder)
}

bool RaytracingSceneConverter::ConvertMaterials(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  (void)env;
  (void)scene;

  // TODO: Implement material conversion
  // - Traverse USD stage for Material prims
  // - Flatten shader networks to simple PBR parameters
  // - Extract texture references

  _info += "Material conversion not yet implemented\n";
  return true;  // Return true for now (placeholder)
}

bool RaytracingSceneConverter::ConvertLights(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  (void)env;
  (void)scene;

  // TODO: Implement light conversion
  // - Traverse USD stage for Light prims (UsdLux)
  // - Convert to RTLight types
  // - Build importance sampling for environment maps if needed

  _info += "Light conversion not yet implemented\n";
  return true;  // Return true for now (placeholder)
}

bool RaytracingSceneConverter::ConvertCameras(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  (void)env;
  (void)scene;

  // TODO: Implement camera conversion
  // - Traverse USD stage for Camera prims
  // - Convert to RTCamera

  _info += "Camera conversion not yet implemented\n";
  return true;  // Return true for now (placeholder)
}

bool RaytracingSceneConverter::ConvertInstances(
    const RaytracingSceneConverterEnv& env, RaytracingScene* scene) {
  (void)env;
  (void)scene;

  // TODO: Implement instance conversion
  // - Detect instanced geometry (references/payloads)
  // - Create RTInstance entries with transforms
  // - Handle material overrides

  _info += "Instance conversion not yet implemented\n";
  return true;  // Return true for now (placeholder)
}

bool RaytracingSceneConverter::ConvertMesh(const GeomMesh& mesh,
                                            const RaytracingSceneConverterEnv& env,
                                            RTGeometry* geom) {
  (void)mesh;
  (void)env;
  (void)geom;

  // TODO: Implement individual mesh conversion
  // Note: Primvars are fetched at intersection, so just store raw data
  // - Copy vertices
  // - Copy indices (triangulate if needed)
  // - Copy normals (generate if missing)
  // - Copy UVs
  // - Copy colors, tangents if present
  // - Compute bounds

  return true;  // Placeholder
}

bool RaytracingSceneConverter::ConvertMaterial(
    const Material& material, const RaytracingSceneConverterEnv& env,
    RTMaterial* mat) {
  (void)material;
  (void)env;
  (void)mat;

  // TODO: Implement material conversion
  // - Extract PBR parameters from shader network
  // - Map USD material to RTMaterial structure

  return true;  // Placeholder
}

bool RaytracingSceneConverter::ConvertLight(const Prim& light_prim,
                                             const RaytracingSceneConverterEnv& env,
                                             RTLight* light) {
  (void)light_prim;
  (void)env;
  (void)light;

  // TODO: Implement light conversion

  return true;  // Placeholder
}

bool RaytracingSceneConverter::ConvertCamera(
    const Prim& camera_prim, const RaytracingSceneConverterEnv& env,
    RTCamera* camera) {
  (void)camera_prim;
  (void)env;
  (void)camera;

  // TODO: Implement camera conversion

  return true;  // Placeholder
}

bool RaytracingSceneConverter::TriangulateMesh(RTGeometry* geom) {
  (void)geom;

  // TODO: Implement triangulation
  // - Check if indices are already triangles
  // - If not, triangulate quads/polygons

  return true;  // Placeholder
}

bool RaytracingSceneConverter::ComputeSmoothNormals(RTGeometry* geom) {
  if (!geom || geom->vertices.empty() || geom->indices.empty()) {
    return false;
  }

  // Allocate normals array
  geom->normals.resize(geom->vertices.size());

  // Initialize to zero
  for (auto& n : geom->normals) {
    n[0] = 0.0f;
    n[1] = 0.0f;
    n[2] = 0.0f;
  }

  // Compute face normals and accumulate to vertices
  size_t num_triangles = geom->indices.size() / 3;
  for (size_t i = 0; i < num_triangles; ++i) {
    uint32_t i0 = geom->indices[i * 3 + 0];
    uint32_t i1 = geom->indices[i * 3 + 1];
    uint32_t i2 = geom->indices[i * 3 + 2];

    if (i0 >= geom->vertices.size() || i1 >= geom->vertices.size() ||
        i2 >= geom->vertices.size()) {
      continue;  // Skip invalid triangles
    }

    const auto& v0 = geom->vertices[i0];
    const auto& v1 = geom->vertices[i1];
    const auto& v2 = geom->vertices[i2];

    // Compute edge vectors
    vec3 e1, e2;
    e1[0] = v1[0] - v0[0];
    e1[1] = v1[1] - v0[1];
    e1[2] = v1[2] - v0[2];

    e2[0] = v2[0] - v0[0];
    e2[1] = v2[1] - v0[1];
    e2[2] = v2[2] - v0[2];

    // Cross product
    vec3 n;
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];

    // Accumulate to vertex normals
    geom->normals[i0][0] += n[0];
    geom->normals[i0][1] += n[1];
    geom->normals[i0][2] += n[2];

    geom->normals[i1][0] += n[0];
    geom->normals[i1][1] += n[1];
    geom->normals[i1][2] += n[2];

    geom->normals[i2][0] += n[0];
    geom->normals[i2][1] += n[1];
    geom->normals[i2][2] += n[2];
  }

  // Normalize
  for (auto& n : geom->normals) {
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > std::numeric_limits<float>::epsilon()) {
      n[0] /= len;
      n[1] /= len;
      n[2] /= len;
    } else {
      // Degenerate normal, set to up vector
      n[0] = 0.0f;
      n[1] = 1.0f;
      n[2] = 0.0f;
    }
  }

  return true;
}

bool RaytracingSceneConverter::ComputeTangents(RTGeometry* geom) {
  (void)geom;

  // TODO: Implement tangent computation using MikkTSpace or similar

  return true;  // Placeholder
}

}  // namespace tydra
}  // namespace lightusd
