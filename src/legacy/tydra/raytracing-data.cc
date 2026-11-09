// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.

#include "raytracing-data.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/safe-arithmetic.hh"

namespace lightusd {
namespace tydra {

//
// RTGeometry implementation
//

void RTGeometry::compute_bounds() {
  bounds.min[0] = (std::numeric_limits<float>::max)();
  bounds.min[1] = (std::numeric_limits<float>::max)();
  bounds.min[2] = (std::numeric_limits<float>::max)();

  bounds.max[0] = std::numeric_limits<float>::lowest();
  bounds.max[1] = std::numeric_limits<float>::lowest();
  bounds.max[2] = std::numeric_limits<float>::lowest();

  if (is_mesh()) {
    // Compute bounds from mesh vertices
    for (const auto& v : vertices) {
      bounds.expand(v);
    }
  } else if (is_primitive()) {
    // Compute bounds from analytic primitive parameters
    switch (geom_type) {
      case RTGeometryType::Sphere: {
        // Sphere: center +/- radius in all directions
        bounds.min[0] = prim_center[0] - prim_radius;
        bounds.min[1] = prim_center[1] - prim_radius;
        bounds.min[2] = prim_center[2] - prim_radius;
        bounds.max[0] = prim_center[0] + prim_radius;
        bounds.max[1] = prim_center[1] + prim_radius;
        bounds.max[2] = prim_center[2] + prim_radius;
        break;
      }
      case RTGeometryType::Cylinder:
      case RTGeometryType::Capsule:
      case RTGeometryType::Cone: {
        // Cylinder/Capsule/Cone: approximate bounds along axis
        // (Conservative AABB, actual shape is tighter)
        float half_h = prim_height * 0.5f;
        float r = prim_radius;

        // Axis-aligned approximation (assumes axis is normalized)
        vec3 axis_offset;
        axis_offset[0] = prim_axis[0] * half_h;
        axis_offset[1] = prim_axis[1] * half_h;
        axis_offset[2] = prim_axis[2] * half_h;

        vec3 p0, p1;
        p0[0] = prim_center[0] - axis_offset[0];
        p0[1] = prim_center[1] - axis_offset[1];
        p0[2] = prim_center[2] - axis_offset[2];

        p1[0] = prim_center[0] + axis_offset[0];
        p1[1] = prim_center[1] + axis_offset[1];
        p1[2] = prim_center[2] + axis_offset[2];

        // Expand by radius in all directions (conservative)
        bounds.min[0] = std::min(p0[0], p1[0]) - r;
        bounds.min[1] = std::min(p0[1], p1[1]) - r;
        bounds.min[2] = std::min(p0[2], p1[2]) - r;
        bounds.max[0] = std::max(p0[0], p1[0]) + r;
        bounds.max[1] = std::max(p0[1], p1[1]) + r;
        bounds.max[2] = std::max(p0[2], p1[2]) + r;
        break;
      }
      case RTGeometryType::TriangleMesh:
      case RTGeometryType::QuadMesh:
      case RTGeometryType::MixedMesh:
        // Already handled in is_mesh() branch above
        break;
    }
  }
}

//
// RaytracingScene implementation
//

bool RaytracingScene::build_acceleration_structure(
    const RTAccelerationStructure::BuildConfig& config) {
  (void)config;  // Unused in placeholder implementation

  // Basic BVH build placeholder
  // TODO: Implement actual BVH construction or integrate with Embree/OptiX

  accel_structure.type = RTAccelerationStructure::Type::BVH2;
  accel_structure.scene_bounds = compute_scene_bounds();

  // Reset stats
  accel_structure.stats.num_nodes = 0;
  accel_structure.stats.num_leaves = 0;
  accel_structure.stats.max_depth = 0;
  accel_structure.stats.build_time_ms = 0.0;
  accel_structure.stats.memory_bytes = 0;

  // Basic validation
  if (geometries.empty() && instances.empty()) {
    return false;  // No geometry to build
  }

  // TODO: Actual BVH construction
  // For now, just mark as built
  return true;
}

size_t RaytracingScene::estimate_memory_usage() const {
  // Safe estimation using saturating arithmetic: on overflow a contribution
  // is skipped rather than producing a silently-wrapped (under-)estimate.
  size_t total = 0;
  auto add_size = [&total](size_t count, size_t elem_size) {
    size_t bytes;
    if (safe::mul(count, elem_size, &bytes)) {
      size_t sum;
      if (safe::add(total, bytes, &sum)) total = sum;
    }
  };
  auto add_val = [&total](size_t val) {
    size_t sum;
    if (safe::add(total, val, &sum)) total = sum;
  };

  // Geometries
  for (const auto& geom : geometries) {
    add_size(geom.vertices.size(), sizeof(vec3));
    add_size(geom.indices.size(), sizeof(uint32_t));
    add_size(geom.normals.size(), sizeof(vec3));
    add_size(geom.texcoords0.size(), sizeof(vec2));
    add_size(geom.texcoords1.size(), sizeof(vec2));
    add_size(geom.colors.size(), sizeof(vec4));
    add_size(geom.tangents.size(), sizeof(vec4));
    add_size(geom.material_ids.size(), sizeof(uint32_t));
    add_size(geom.face_normals.size(), sizeof(vec3));
    add_size(geom.joint_indices.size(), sizeof(vec4));
    add_size(geom.joint_weights.size(), sizeof(vec4));
  }

  // Materials
  add_size(materials.size(), sizeof(RTMaterial));

  // Lights
  for (const auto& light : lights) {
    add_val(sizeof(RTLight));
    if (light.envmap_sampling.has_value()) {
      const auto& sampling = light.envmap_sampling.value();
      add_size(sampling.cdf.size(), sizeof(float));
      add_size(sampling.pdf.size(), sizeof(float));
    }
  }

  // Instances
  add_size(instances.size(), sizeof(RTInstance));
  for (const auto& inst : instances) {
    add_size(inst.material_overrides.size(), sizeof(uint32_t));
  }

  // Cameras
  add_size(cameras.size(), sizeof(RTCamera));

  // Acceleration structure
  add_val(accel_structure.stats.memory_bytes);

  return total;
}

bool RaytracingScene::validate(std::string* warn, std::string* err) const {
  bool valid = true;

  // Check geometries
  for (size_t i = 0; i < geometries.size(); ++i) {
    const auto& geom = geometries[i];

    // Validate based on geometry type
    if (geom.is_mesh()) {
      // Mesh validation
      if (geom.vertices.empty()) {
        if (err) {
          *err += "Geometry[" + std::to_string(i) + "] mesh has no vertices\n";
        }
        valid = false;
      }

      if (geom.indices.empty()) {
        if (err) {
          *err += "Geometry[" + std::to_string(i) + "] mesh has no indices\n";
        }
        valid = false;
      }

      // Check indices divisibility based on mesh type
      if (geom.geom_type == RTGeometryType::TriangleMesh) {
        if (geom.indices.size() % 3 != 0) {
          if (err) {
            *err += "Geometry[" + std::to_string(i) +
                    "] triangle mesh indices not divisible by 3\n";
          }
          valid = false;
        }
      } else if (geom.geom_type == RTGeometryType::QuadMesh) {
        if (geom.indices.size() % 4 != 0) {
          if (err) {
            *err += "Geometry[" + std::to_string(i) +
                    "] quad mesh indices not divisible by 4\n";
          }
          valid = false;
        }
      } else if (geom.geom_type == RTGeometryType::MixedMesh) {
        if (geom.face_vertex_counts.empty()) {
          if (err) {
            *err += "Geometry[" + std::to_string(i) +
                    "] mixed mesh missing face_vertex_counts\n";
          }
          valid = false;
        }
        // Verify total indices matches sum of face_vertex_counts
        size_t expected_indices = 0;
        for (auto count : geom.face_vertex_counts) {
          expected_indices += count;
        }
        if (geom.indices.size() != expected_indices) {
          if (err) {
            *err += "Geometry[" + std::to_string(i) +
                    "] mixed mesh indices count mismatch with face_vertex_counts\n";
          }
          valid = false;
        }
      }

      // Check index range
      for (const auto idx : geom.indices) {
        if (idx >= geom.vertices.size()) {
          if (err) {
            *err += "Geometry[" + std::to_string(i) +
                    "] has out-of-range index: " + std::to_string(idx) + "\n";
          }
          valid = false;
          break;
        }
      }

      // Check normals size if present
      if (!geom.normals.empty() && geom.normals.size() != geom.vertices.size()) {
        if (warn) {
          *warn += "Geometry[" + std::to_string(i) +
                   "] normals count mismatch with vertices\n";
        }
      }

    } else if (geom.is_primitive()) {
      // Primitive validation
      if (geom.prim_radius <= 0.0f) {
        if (warn) {
          *warn += "Geometry[" + std::to_string(i) +
                   "] primitive has invalid radius (<= 0)\n";
        }
      }

      if (geom.geom_type == RTGeometryType::Cylinder ||
          geom.geom_type == RTGeometryType::Capsule ||
          geom.geom_type == RTGeometryType::Cone) {
        if (geom.prim_height <= 0.0f) {
          if (warn) {
            *warn += "Geometry[" + std::to_string(i) +
                     "] primitive has invalid height (<= 0)\n";
          }
        }
      }
    }

    // Check material IDs
    if (!geom.material_ids.empty()) {
      for (const auto mat_id : geom.material_ids) {
        if (mat_id >= materials.size()) {
          if (warn) {
            *warn += "Geometry[" + std::to_string(i) +
                     "] references invalid material ID: " +
                     std::to_string(mat_id) + "\n";
          }
        }
      }
    }
  }

  // Check instances
  for (size_t i = 0; i < instances.size(); ++i) {
    const auto& inst = instances[i];

    if (inst.geometry_id >= geometries.size()) {
      if (err) {
        *err += "Instance[" + std::to_string(i) +
                "] references invalid geometry ID: " +
                std::to_string(inst.geometry_id) + "\n";
      }
      valid = false;
    }
  }

  // Check lights
  for (size_t i = 0; i < lights.size(); ++i) {
    const auto& light = lights[i];

    if (light.type == RTLight::Type::Mesh) {
      if (light.emissive_mesh_id < 0 ||
          static_cast<size_t>(light.emissive_mesh_id) >= geometries.size()) {
        if (err) {
          *err += "Light[" + std::to_string(i) +
                  "] references invalid emissive mesh ID\n";
        }
        valid = false;
      }
    }
  }

  return valid;
}

std::vector<uint32_t> RaytracingScene::get_emissive_geometry_ids() const {
  std::vector<uint32_t> emissive_ids;

  for (size_t i = 0; i < geometries.size(); ++i) {
    const auto& geom = geometries[i];

    // Check if geometry has emissive materials
    if (!geom.material_ids.empty()) {
      for (const auto mat_id : geom.material_ids) {
        if (mat_id < materials.size() && materials[mat_id].is_emissive()) {
          emissive_ids.push_back(static_cast<uint32_t>(i));
          break;  // Only add once per geometry
        }
      }
    }
  }

  return emissive_ids;
}

AABB RaytracingScene::compute_scene_bounds() const {
  AABB bounds;

  // Compute bounds from all geometries
  for (const auto& geom : geometries) {
    bounds.expand(geom.bounds);
  }

  // Expand by instances
  for (const auto& inst : instances) {
    if (inst.geometry_id < geometries.size()) {
      const auto& geom = geometries[inst.geometry_id];

      // Transform AABB corners by instance transform
      // (Simple approach: transform all 8 corners and recompute AABB)
      vec3 corners[8];
      corners[0] = {geom.bounds.min[0], geom.bounds.min[1], geom.bounds.min[2]};
      corners[1] = {geom.bounds.max[0], geom.bounds.min[1], geom.bounds.min[2]};
      corners[2] = {geom.bounds.min[0], geom.bounds.max[1], geom.bounds.min[2]};
      corners[3] = {geom.bounds.max[0], geom.bounds.max[1], geom.bounds.min[2]};
      corners[4] = {geom.bounds.min[0], geom.bounds.min[1], geom.bounds.max[2]};
      corners[5] = {geom.bounds.max[0], geom.bounds.min[1], geom.bounds.max[2]};
      corners[6] = {geom.bounds.min[0], geom.bounds.max[1], geom.bounds.max[2]};
      corners[7] = {geom.bounds.max[0], geom.bounds.max[1], geom.bounds.max[2]};

      for (int i = 0; i < 8; ++i) {
        // Transform corner by instance matrix
        // p' = M * p (assuming column-major matrix)
        const auto& m = inst.transform;
        vec3 transformed;
        transformed[0] = m.m[0][0] * corners[i][0] + m.m[0][1] * corners[i][1] +
                        m.m[0][2] * corners[i][2] + m.m[0][3];
        transformed[1] = m.m[1][0] * corners[i][0] + m.m[1][1] * corners[i][1] +
                        m.m[1][2] * corners[i][2] + m.m[1][3];
        transformed[2] = m.m[2][0] * corners[i][0] + m.m[2][1] * corners[i][1] +
                        m.m[2][2] * corners[i][2] + m.m[2][3];

        bounds.expand(transformed);
      }
    }
  }

  return bounds;
}

}  // namespace tydra
}  // namespace lightusd
