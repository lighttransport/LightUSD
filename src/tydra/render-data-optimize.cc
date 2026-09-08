// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Render-scene material/texture deduplication, tree flattening, and mesh merge.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common-macros.inc"
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "value-types.hh"
#include "xform.hh"
#include "../safe-arithmetic.hh"

namespace lightusd {
namespace tydra {
namespace {


}  // namespace

bool RenderSceneConverter::IsMeshMergeable(const RenderMesh &mesh) const {
  // Mesh cannot be merged if:
  // 1. Has skeletal animation
  if (mesh.skel_id >= 0) {
    return false;
  }

  // 2. Has blend shapes
  if (!mesh.targets.empty()) {
    return false;
  }

  // 3. Has per-face materials (GeomSubset)
  if (!mesh.material_subsetMap.empty()) {
    return false;
  }

  // 4. Is an area light (special rendering)
  if (mesh.is_area_light) {
    return false;
  }

  return true;
}

// Helper function to transform a vec3 point by a matrix4d
static vec3 TransformPoint(const value::matrix4d &m, const vec3 &p) {
  // Apply full 4x4 transform (position)
  double x = m.m[0][0] * double(p[0]) + m.m[1][0] * double(p[1]) + m.m[2][0] * double(p[2]) + m.m[3][0];
  double y = m.m[0][1] * double(p[0]) + m.m[1][1] * double(p[1]) + m.m[2][1] * double(p[2]) + m.m[3][1];
  double z = m.m[0][2] * double(p[0]) + m.m[1][2] * double(p[1]) + m.m[2][2] * double(p[2]) + m.m[3][2];
  double w = m.m[0][3] * double(p[0]) + m.m[1][3] * double(p[1]) + m.m[2][3] * double(p[2]) + m.m[3][3];

  if (std::abs(w) > 1e-10) {
    x /= w;
    y /= w;
    z /= w;
  }

  return vec3{float(x), float(y), float(z)};
}

// Helper function to transform a vec3 direction by a precomputed inverse
// matrix. The upper-left 3x3 of the inverse-transpose gives the correct normal
// transform under non-uniform scale.
static vec3 TransformNormalWithInverse(const value::matrix4d &inv,
                                       const vec3 &n) {
  double x = inv.m[0][0] * double(n[0]) + inv.m[0][1] * double(n[1]) + inv.m[0][2] * double(n[2]);
  double y = inv.m[1][0] * double(n[0]) + inv.m[1][1] * double(n[1]) + inv.m[1][2] * double(n[2]);
  double z = inv.m[2][0] * double(n[0]) + inv.m[2][1] * double(n[1]) + inv.m[2][2] * double(n[2]);

  // Normalize the result
  double len = std::sqrt(x*x + y*y + z*z);
  if (len > 1e-10) {
    x /= len;
    y /= len;
    z /= len;
  }

  return vec3{float(x), float(y), float(z)};
}

static bool CanBakeDirectionAttribute(const VertexAttribute &attr) {
  return attr.empty() ||
         (attr.format == VertexAttributeFormat::Vec3 &&
          attr.stride_bytes() == sizeof(vec3));
}

// Whether `src` can be appended onto `dst`. Two conditions:
//   - if both sides carry data, format and stride must match;
//   - once `dst` already holds vertices (`dst_has_vertices`), the attribute must
//     be present on both or neither. An attribute present on exactly one side
//     would leave the merged array shorter than (or misaligned with) the point
//     count, corrupting the mesh, so such a merge is refused.
static bool CompatibleVertexAttributeForAppend(const VertexAttribute &dst,
                                               const VertexAttribute &src,
                                               bool dst_has_vertices) {
  if (dst_has_vertices && (dst.empty() != src.empty())) {
    return false;
  }
  return dst.empty() || src.empty() ||
         (dst.format == src.format &&
          dst.stride_bytes() == src.stride_bytes());
}

static bool ValidateIndexedTopology(const std::vector<uint32_t> &counts,
                                    const std::vector<uint32_t> &indices,
                                    size_t point_count,
                                    const std::string &label,
                                    std::string *err) {
  if (counts.empty() && indices.empty()) {
    return true;
  }
  if (counts.empty() || indices.empty()) {
    if (err) {
      *err = "Cannot merge " + label +
             ": face counts and indices must both be present.";
    }
    return false;
  }

  size_t total = 0;
  for (uint32_t count : counts) {
    if (size_t(count) > indices.size() - total) {
      if (err) {
        *err = "Cannot merge " + label +
               ": face counts exceed index array length.";
      }
      return false;
    }
    total += size_t(count);
  }
  if (total != indices.size()) {
    if (err) {
      *err = "Cannot merge " + label +
             ": face counts do not match index array length.";
    }
    return false;
  }

  for (uint32_t idx : indices) {
    if (size_t(idx) >= point_count) {
      if (err) {
        *err = "Cannot merge " + label +
               ": face index is out of point range.";
      }
      return false;
    }
  }
  return true;
}

bool RenderSceneConverter::MergeMeshData(const RenderMesh &src,
                                         const value::matrix4d &src_transform,
                                         RenderMesh &dst,
                                         std::string *err) {
  auto set_merge_error = [&](const std::string &msg) {
    if (err) {
      *err = msg;
    }
  };

  // Check if transform is identity using lightusd::is_identity function
  bool transform_is_identity = lightusd::is_identity(src_transform);
  value::matrix4d src_transform_inverse = value::matrix4d::identity();
  const bool needs_direction_bake =
      !transform_is_identity &&
      (!src.normals.empty() || !src.tangents.empty() ||
       !src.binormals.empty());
  if (!transform_is_identity) {
    if (!CanBakeDirectionAttribute(src.normals) ||
        !CanBakeDirectionAttribute(src.tangents) ||
        !CanBakeDirectionAttribute(src.binormals)) {
      set_merge_error(
          "Cannot bake transform for packed or non-float3 direction attributes.");
      return false;
    }
    if (needs_direction_bake &&
        !lightusd::inverse(src_transform, src_transform_inverse)) {
      set_merge_error(
          "Cannot bake direction attributes with a non-invertible transform.");
      return false;
    }
  }

  // All attribute compatibility is validated up front, before `dst` is
  // mutated, so a refused merge leaves `dst` untouched and the caller can keep
  // the source as a standalone mesh.
  const bool dst_has_vertices = !dst.points.empty();
  if (!CompatibleVertexAttributeForAppend(dst.normals, src.normals,
                                          dst_has_vertices)) {
    set_merge_error("Cannot merge normals: incompatible format or presence.");
    return false;
  }
  if (!CompatibleVertexAttributeForAppend(dst.tangents, src.tangents,
                                          dst_has_vertices)) {
    set_merge_error("Cannot merge tangents: incompatible format or presence.");
    return false;
  }
  if (!CompatibleVertexAttributeForAppend(dst.binormals, src.binormals,
                                          dst_has_vertices)) {
    set_merge_error("Cannot merge binormals: incompatible format or presence.");
    return false;
  }
  if (!CompatibleVertexAttributeForAppend(dst.vertex_colors, src.vertex_colors,
                                          dst_has_vertices)) {
    set_merge_error(
        "Cannot merge vertex_colors: incompatible format or presence.");
    return false;
  }
  if (!CompatibleVertexAttributeForAppend(dst.vertex_opacities,
                                          src.vertex_opacities,
                                          dst_has_vertices)) {
    set_merge_error(
        "Cannot merge vertex_opacities: incompatible format or presence.");
    return false;
  }
  // Texcoords are keyed by slot; a slot present on exactly one side (once dst
  // has vertices) would leave a partially-filled UV set, so refuse it too.
  if (dst_has_vertices && dst.texcoords.size() != src.texcoords.size()) {
    set_merge_error("Cannot merge texcoords: mismatched UV slot sets.");
    return false;
  }
  for (const auto &src_tc : src.texcoords) {
    auto dst_tc_it = dst.texcoords.find(src_tc.first);
    if (dst_tc_it == dst.texcoords.end()) {
      if (dst_has_vertices) {
        set_merge_error("Cannot merge texcoords slot " +
                        std::to_string(src_tc.first) +
                        ": UV slot missing on merge target.");
        return false;
      }
      continue;
    }
    if (!CompatibleVertexAttributeForAppend(dst_tc_it->second, src_tc.second,
                                            dst_has_vertices)) {
      set_merge_error("Cannot merge texcoords slot " +
                      std::to_string(src_tc.first) +
                      ": incompatible format or presence.");
      return false;
    }
  }

  // Get the vertex offset for index adjustment.
#if SIZE_MAX > 0xFFFFFFFFu
  // Only meaningful where size_t is wider than uint32 (e.g. 64-bit). On a
  // 32-bit size_t (wasm32) points.size() can never exceed UINT32_MAX, so this
  // comparison is tautologically false and is compiled out.
  if (dst.points.size() >
      size_t((std::numeric_limits<uint32_t>::max)())) {
    set_merge_error("Cannot merge mesh: vertex offset exceeds uint32 range.");
    return false;
  }
#endif
  if (src.points.size() >
      size_t((std::numeric_limits<uint32_t>::max)()) - dst.points.size()) {
    set_merge_error("Cannot merge mesh: vertex count exceeds uint32 range.");
    return false;
  }
  if (!ValidateIndexedTopology(src.usdFaceVertexCounts,
                               src.usdFaceVertexIndices,
                               src.points.size(),
                               "face topology", err)) {
    return false;
  }
  if (!ValidateIndexedTopology(src.triangulatedFaceVertexCounts,
                               src.triangulatedFaceVertexIndices,
                               src.points.size(),
                               "triangulated topology", err)) {
    return false;
  }
  // With the vertex-count guard above, `vertex_offset + idx` cannot overflow
  // uint32 for any in-range index (idx < src.points.size()). The per-element
  // guards below therefore only catch malformed (out-of-range) source indices.
  uint32_t vertex_offset = static_cast<uint32_t>(dst.points.size());

  // Merge points (with transform if needed)
  if (transform_is_identity) {
    dst.points.insert(dst.points.end(), src.points.begin(), src.points.end());
  } else {
    for (const auto &p : src.points) {
      dst.points.push_back(TransformPoint(src_transform, p));
    }
  }

  // Merge face vertex indices (adjust by vertex offset)
  for (uint32_t idx : src.usdFaceVertexIndices) {
    if (idx > (std::numeric_limits<uint32_t>::max)() - vertex_offset) {
      set_merge_error("Cannot merge face indices: uint32 index overflow.");
      return false;
    }
    dst.usdFaceVertexIndices.push_back(idx + vertex_offset);
  }

  // Merge face vertex counts
  dst.usdFaceVertexCounts.insert(dst.usdFaceVertexCounts.end(),
                                  src.usdFaceVertexCounts.begin(),
                                  src.usdFaceVertexCounts.end());

  // Merge triangulated indices if present
  if (!src.triangulatedFaceVertexIndices.empty()) {
    for (uint32_t idx : src.triangulatedFaceVertexIndices) {
      if (idx > (std::numeric_limits<uint32_t>::max)() - vertex_offset) {
        set_merge_error(
            "Cannot merge triangulated indices: uint32 index overflow.");
        return false;
      }
      dst.triangulatedFaceVertexIndices.push_back(idx + vertex_offset);
    }
    dst.triangulatedFaceVertexCounts.insert(dst.triangulatedFaceVertexCounts.end(),
                                             src.triangulatedFaceVertexCounts.begin(),
                                             src.triangulatedFaceVertexCounts.end());
  }

  // Merge normals (transform direction if needed)
  if (!src.normals.empty()) {
    size_t src_normal_count = src.normals.vertex_count();

    // Ensure dst normals has same format
    if (dst.normals.empty()) {
      dst.normals = src.normals;
      if (!transform_is_identity) {
        // Transform the normals we just copied
        vec3 *normals_data = reinterpret_cast<vec3*>(dst.normals.data.data());
        for (size_t i = 0; i < src_normal_count; i++) {
          normals_data[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         normals_data[i]);
        }
      }
    } else {
      // Format/stride/presence already validated up front.
      // Append normals
      size_t old_size = dst.normals.data.size();
      size_t new_size;
      if (!safe::add(old_size, src.normals.data.size(), &new_size)) {
        return false;
      }
      dst.normals.data.resize(new_size);

      if (transform_is_identity) {
        memcpy(dst.normals.data.data() + old_size, src.normals.data.data(), src.normals.data.size());
      } else {
        const vec3 *src_normals = reinterpret_cast<const vec3*>(src.normals.data.data());
        vec3 *dst_normals = reinterpret_cast<vec3*>(dst.normals.data.data() + old_size);
        for (size_t i = 0; i < src_normal_count; i++) {
          dst_normals[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         src_normals[i]);
        }
      }
    }
  }

  // Merge texcoords (no transform needed)
  for (const auto &src_tc : src.texcoords) {
    uint32_t slot = src_tc.first;
    const auto &src_attr = src_tc.second;

    auto dst_tc_it = dst.texcoords.find(slot);
    if (dst_tc_it == dst.texcoords.end()) {
      dst.texcoords.emplace(slot, src_attr);
    } else {
      auto &dst_attr = dst_tc_it->second;
      // Format/stride/presence already validated up front.
      size_t old_size = dst_attr.data.size();
      size_t new_size;
      if (!safe::add(old_size, src_attr.data.size(), &new_size)) {
        return false;
      }
      dst_attr.data.resize(new_size);
      memcpy(dst_attr.data.data() + old_size, src_attr.data.data(), src_attr.data.size());
    }
  }

  // Merge tangents (transform direction if needed)
  if (!src.tangents.empty()) {
    if (dst.tangents.empty()) {
      dst.tangents = src.tangents;
      if (!transform_is_identity) {
        vec3 *tangents_data = reinterpret_cast<vec3*>(dst.tangents.data.data());
        size_t count = dst.tangents.vertex_count();
        for (size_t i = 0; i < count; i++) {
          tangents_data[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         tangents_data[i]);
        }
      }
    } else {
      // Format/stride/presence already validated up front.
      size_t old_size = dst.tangents.data.size();
      size_t src_count = src.tangents.vertex_count();
      size_t new_size;
      if (!safe::add(old_size, src.tangents.data.size(), &new_size)) {
        return false;
      }
      dst.tangents.data.resize(new_size);

      if (transform_is_identity) {
        memcpy(dst.tangents.data.data() + old_size, src.tangents.data.data(), src.tangents.data.size());
      } else {
        const vec3 *src_tangents = reinterpret_cast<const vec3*>(src.tangents.data.data());
        vec3 *dst_tangents = reinterpret_cast<vec3*>(dst.tangents.data.data() + old_size);
        for (size_t i = 0; i < src_count; i++) {
          dst_tangents[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         src_tangents[i]);
        }
      }
    }
  }

  // Merge binormals (transform direction if needed)
  if (!src.binormals.empty()) {
    if (dst.binormals.empty()) {
      dst.binormals = src.binormals;
      if (!transform_is_identity) {
        vec3 *binormals_data = reinterpret_cast<vec3*>(dst.binormals.data.data());
        size_t count = dst.binormals.vertex_count();
        for (size_t i = 0; i < count; i++) {
          binormals_data[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         binormals_data[i]);
        }
      }
    } else {
      // Format/stride/presence already validated up front.
      size_t old_size = dst.binormals.data.size();
      size_t src_count = src.binormals.vertex_count();
      size_t new_size;
      if (!safe::add(old_size, src.binormals.data.size(), &new_size)) {
        return false;
      }
      dst.binormals.data.resize(new_size);

      if (transform_is_identity) {
        memcpy(dst.binormals.data.data() + old_size, src.binormals.data.data(), src.binormals.data.size());
      } else {
        const vec3 *src_binormals = reinterpret_cast<const vec3*>(src.binormals.data.data());
        vec3 *dst_binormals = reinterpret_cast<vec3*>(dst.binormals.data.data() + old_size);
        for (size_t i = 0; i < src_count; i++) {
          dst_binormals[i] =
              TransformNormalWithInverse(src_transform_inverse,
                                         src_binormals[i]);
        }
      }
    }
  }

  // Merge vertex colors
  if (!src.vertex_colors.empty()) {
    if (dst.vertex_colors.empty()) {
      dst.vertex_colors = src.vertex_colors;
    } else {
      // Format/stride/presence already validated up front.
      size_t old_size = dst.vertex_colors.data.size();
      size_t new_size;
      if (!safe::add(old_size, src.vertex_colors.data.size(), &new_size)) {
        return false;
      }
      dst.vertex_colors.data.resize(new_size);
      memcpy(dst.vertex_colors.data.data() + old_size, src.vertex_colors.data.data(), src.vertex_colors.data.size());
    }
  }

  // Merge vertex opacities
  if (!src.vertex_opacities.empty()) {
    if (dst.vertex_opacities.empty()) {
      dst.vertex_opacities = src.vertex_opacities;
    } else {
      // Format/stride/presence already validated up front.
      size_t old_size = dst.vertex_opacities.data.size();
      size_t new_size;
      if (!safe::add(old_size, src.vertex_opacities.data.size(), &new_size)) {
        return false;
      }
      dst.vertex_opacities.data.resize(new_size);
      memcpy(dst.vertex_opacities.data.data() + old_size, src.vertex_opacities.data.data(), src.vertex_opacities.data.size());
    }
  }

  return true;
}

size_t RenderSceneConverter::FlattenOptimizedRenderTreeImpl() {
  std::vector<Node> flat_nodes;
  std::vector<int32_t> old_to_new_node_index;

  auto shouldKeep = [](const Node &node) {
    if (node.id < 0) {
      return false;
    }
    if (node.nodeType == NodeType::Mesh) {
      return true;
    }
    return node.category == NodeCategory::Camera ||
           node.category == NodeCategory::Light ||
           node.category == NodeCategory::Skeleton;
  };

  std::function<void(const Node &, int32_t)> collect =
      [&](const Node &node, int32_t depth) {
        if (size_t(depth) >= kMaxDefaultTraversalLimit) {
          return;
        }

        const size_t old_index = old_to_new_node_index.size();
        old_to_new_node_index.push_back(-1);
        if (shouldKeep(node)) {
          Node kept = node;
          kept.local_matrix = node.global_matrix;
          kept.children.clear();
          old_to_new_node_index[old_index] = int32_t(flat_nodes.size() + 1);
          flat_nodes.push_back(std::move(kept));
        }

        for (const Node &child : node.children) {
          collect(child, depth + 1);
        }
      };

  for (const Node &root : root_nodes) {
    collect(root, 0);
  }

  Node optimized_root;
  optimized_root.prim_name = "OptimizedRenderRoot";
  optimized_root.display_name = "Optimized Render Root";
  optimized_root.abs_path = "/OptimizedRenderRoot";
  optimized_root.category = NodeCategory::Group;
  optimized_root.nodeType = NodeType::Xform;
  optimized_root.id = -1;
  optimized_root.local_matrix = value::matrix4d::identity();
  optimized_root.global_matrix = value::matrix4d::identity();
  optimized_root.children = std::move(flat_nodes);

  const size_t kept_count = optimized_root.children.size();
  root_nodes.clear();
  root_nodes.push_back(std::move(optimized_root));
  root_nodeMap = StringAndIdMap{};
  root_nodeMap.add("/OptimizedRenderRoot", uint64_t(0));
  default_node = 0;

  for (AnimationClip &clip : animations) {
    for (AnimationChannel &channel : clip.channels) {
      if (channel.target_type != ChannelTargetType::SceneNode ||
          channel.target_node < 0) {
        continue;
      }
      const size_t old_index = size_t(channel.target_node);
      channel.target_node = old_index < old_to_new_node_index.size()
                                ? old_to_new_node_index[old_index]
                                : -1;
    }
  }

  return kept_count;
}

bool RenderSceneConverter::MergeMeshesImpl(const RenderSceneConverterEnv &env) {
  if (!env.scene_config.merge_meshes) {
    return true;  // Merging disabled, nothing to do
  }

  DCOUT("MergeMeshesImpl: Starting mesh merge...");

  // Build a map from mesh to its node and global transform
  // Structure: mesh_index -> (node_ptr, global_matrix)
  struct MeshNodeInfo {
    Node *node{nullptr};
    value::matrix4d global_matrix;
    size_t mesh_index{0};
  };

  std::vector<MeshNodeInfo> mesh_node_infos;
  mesh_node_infos.resize(meshes.size());
  std::vector<std::vector<Node *>> mesh_nodes_by_id(meshes.size());

  // Helper to traverse nodes and collect mesh info
  std::function<void(Node &, int32_t)> collectMeshNodes = [&](Node &node, int32_t depth) {
    if (size_t(depth) >= kMaxDefaultTraversalLimit) return;
    if (node.nodeType == NodeType::Mesh && node.id >= 0 &&
        size_t(node.id) < meshes.size()) {
      mesh_node_infos[size_t(node.id)].node = &node;
      mesh_node_infos[size_t(node.id)].global_matrix = node.global_matrix;
      mesh_node_infos[size_t(node.id)].mesh_index = size_t(node.id);
      mesh_nodes_by_id[size_t(node.id)].push_back(&node);
    }
    for (auto &child : node.children) {
      collectMeshNodes(child, depth + 1);
    }
  };

  for (auto &root : root_nodes) {
    collectMeshNodes(root, 0);
  }

  // Group meshes by material_id
  // Only include meshes that are mergeable
  std::unordered_map<int, std::vector<size_t>> material_to_meshes;
  material_to_meshes.reserve(meshes.size());

  for (size_t i = 0; i < meshes.size(); i++) {
    const auto &mesh = meshes[i];
    if (!IsMeshMergeable(mesh)) {
      continue;
    }

    // Skip meshes that don't have a node (shouldn't happen but be safe)
    if (!mesh_node_infos[i].node) {
      continue;
    }

    material_to_meshes[mesh.material_id].push_back(i);
  }

  // For each material group with 2+ meshes, merge them
  std::vector<RenderMesh> merged_meshes;
  std::vector<std::pair<int32_t, std::vector<size_t>>> merged_groups;
  [[maybe_unused]] size_t merged_source_mesh_count{0};

  // Keep deterministic output order by processing material IDs in ascending order.
  std::vector<int> sorted_material_ids;
  sorted_material_ids.reserve(material_to_meshes.size());
  for (const auto &kv : material_to_meshes) {
    sorted_material_ids.push_back(kv.first);
  }
  std::sort(sorted_material_ids.begin(), sorted_material_ids.end());

  for (int material_id : sorted_material_ids) {
    auto group_it = material_to_meshes.find(material_id);
    if (group_it == material_to_meshes.end()) {
      continue;
    }
    auto &mesh_indices = group_it->second;

    if (mesh_indices.size() < 2) {
      // Only one mesh with this material, no merging needed
      continue;
    }

    DCOUT("Merging " << mesh_indices.size() << " meshes with material_id=" << material_id);

    // Check if all meshes have the same global transform (when bake_transform is false)
    bool can_merge = true;
    if (!env.scene_config.merge_meshes_bake_transform) {
      const auto &first_matrix = mesh_node_infos[mesh_indices[0]].global_matrix;
      for (size_t i = 1; i < mesh_indices.size(); i++) {
        const auto &matrix = mesh_node_infos[mesh_indices[i]].global_matrix;
        // Compare matrices (with epsilon)
        bool same_transform = true;
        for (int r = 0; r < 4 && same_transform; r++) {
          for (int c = 0; c < 4 && same_transform; c++) {
            if (std::abs(first_matrix.m[r][c] - matrix.m[r][c]) > 1e-6) {
              same_transform = false;
            }
          }
        }
        if (!same_transform) {
          can_merge = false;
          break;
        }
      }
    }

    if (!can_merge) {
      DCOUT("Cannot merge meshes with material_id=" << material_id << " - different transforms");
      continue;
    }

    // Create merged mesh
    RenderMesh merged;
    merged.prim_name = "merged_material_" + std::to_string(material_id);
    merged.abs_path = "/merged/" + merged.prim_name;
    merged.display_name = "Merged mesh (material " + std::to_string(material_id) + ")";
    merged.material_id = material_id;

    // Copy properties from first mesh
    const auto &first_mesh = meshes[mesh_indices[0]];
    merged.doubleSided = first_mesh.doubleSided;
    merged.displayColor = first_mesh.displayColor;
    merged.displayOpacity = first_mesh.displayOpacity;
    merged.is_rightHanded = first_mesh.is_rightHanded;

    // If baking transforms, we transform all vertices to world space
    // The merged mesh will have identity transform
    bool drop_normals = false;
    bool drop_tangents = false;
    bool drop_binormals = false;
    if (env.scene_config.merge_meshes_bake_transform) {
      for (size_t idx : mesh_indices) {
        const auto &src_mesh = meshes[idx];
        const auto &node_info = mesh_node_infos[idx];
        if (lightusd::is_identity(node_info.global_matrix)) {
          continue;
        }
        drop_normals = drop_normals ||
                       !CanBakeDirectionAttribute(src_mesh.normals);
        drop_tangents = drop_tangents ||
                        !CanBakeDirectionAttribute(src_mesh.tangents);
        drop_binormals = drop_binormals ||
                         !CanBakeDirectionAttribute(src_mesh.binormals);
      }
    }

    std::vector<size_t> merged_sources;
    merged_sources.reserve(mesh_indices.size());

    for (size_t idx : mesh_indices) {
      const auto &src_mesh = meshes[idx];
      const auto &node_info = mesh_node_infos[idx];

      value::matrix4d relative_transform;
      if (env.scene_config.merge_meshes_bake_transform) {
        // Use world space transform
        relative_transform = node_info.global_matrix;
      } else {
        // All transforms should be the same (checked above)
        relative_transform = value::matrix4d::identity();
      }

      std::string merge_err;
      RenderMesh scratch;
      const RenderMesh *merge_src = &src_mesh;
      if (drop_normals || drop_tangents || drop_binormals) {
        scratch = src_mesh;
        if (drop_normals) {
          scratch.normals = VertexAttribute{};
        }
        if (drop_tangents) {
          scratch.tangents = VertexAttribute{};
        }
        if (drop_binormals) {
          scratch.binormals = VertexAttribute{};
        }
        merge_src = &scratch;
      }

      if (!MergeMeshData(*merge_src, relative_transform, merged, &merge_err)) {
        PushInfo("Skipping mesh merge for " + src_mesh.abs_path +
                 (merge_err.empty() ? std::string()
                                    : std::string(": ") + merge_err));
        continue;
      }

      merged_sources.push_back(idx);
    }

    if (merged_sources.size() < 2) {
      // Nothing useful to merge for this material group.
      continue;
    }

    merged_source_mesh_count += merged_sources.size();

    // The merged mesh is either in world space (if bake_transform) or
    // shares the transform of the first mesh
    merged.is_single_indexable = first_mesh.is_single_indexable;

    // Add merged mesh
    size_t new_mesh_index = meshes.size() + merged_meshes.size();
    merged_meshes.push_back(std::move(merged));

    merged_groups.emplace_back(static_cast<int32_t>(new_mesh_index),
                               std::move(merged_sources));
  }

  if (merged_meshes.empty()) {
    DCOUT("No meshes were merged");
    return true;
  }

  DCOUT("Created " << merged_meshes.size() << " merged meshes from "
                   << merged_source_mesh_count << " source meshes");

  // Add merged meshes to the mesh array
  for (auto &mm : merged_meshes) {
    meshes.push_back(std::move(mm));
  }

  // Update node references for merged sources.
  //
  // Baked merges: the merged vertices are in WORLD space, so they must render
  // with a net-identity world transform. We attach the merged mesh to a fresh
  // ROOT-level node (identity transform, no ancestors) and turn every source
  // node into a plain group, leaving its local_matrix INTACT. Crucially we do
  // NOT neutralize a source node's transform in place: source mesh nodes can be
  // nested under one another (e.g. a window mesh under a wall mesh), and
  // rewriting an ancestor's local would corrupt the world transform of any
  // descendant mesh node that was neutralized assuming the original ancestor —
  // the cause of the "floating mesh" artifact. Keeping every source node's
  // local untouched means descendants stay correctly placed; the root-level
  // merged node is independent of all of them.
  //
  // Non-baked merges: the vertices share the (identical) source-group local
  // space, so we keep the first source node carrying that transform and
  // invalidate the rest.
  //
  // The new baked-merge nodes must live INSIDE the subtree that consumers
  // traverse from the default root (getDefaultRootNode), not as detached
  // top-level siblings (which would never be rendered). Attach them as children
  // of the default root node, with a local that cancels that root's own world
  // transform so the world-space merged vertices end up net-identity.
  const size_t default_root_index =
      (default_node >= 0 && size_t(default_node) < root_nodes.size())
          ? size_t(default_node)
          : 0;
  value::matrix4d default_root_local = value::matrix4d::identity();
  if (env.scene_config.merge_meshes_bake_transform && !root_nodes.empty()) {
    value::matrix4d inv_root;
    if (lightusd::inverse(root_nodes[default_root_index].global_matrix,
                          inv_root)) {
      default_root_local = inv_root;
    }
  }

  std::vector<Node> new_root_merged_nodes;
  for (const auto &group : merged_groups) {
    int32_t new_id = group.first;
    const auto &source_ids = group.second;

    if (env.scene_config.merge_meshes_bake_transform) {
      const RenderMesh &mm = meshes[size_t(new_id)];
      Node mnode;
      mnode.prim_name = mm.prim_name;
      mnode.display_name = mm.display_name;
      mnode.abs_path = mm.abs_path;
      mnode.category = NodeCategory::Geom;
      mnode.nodeType = NodeType::Mesh;
      mnode.id = new_id;
      mnode.local_matrix = default_root_local;
      mnode.global_matrix = value::matrix4d::identity();
      new_root_merged_nodes.push_back(std::move(mnode));

      for (size_t old_id : source_ids) {
        if (old_id >= mesh_nodes_by_id.size()) {
          continue;
        }
        for (Node *node_ptr : mesh_nodes_by_id[old_id]) {
          if (!node_ptr) {
            continue;
          }
          // Drop the mesh content; keep the transform for descendants.
          node_ptr->category = NodeCategory::Group;
          node_ptr->nodeType = NodeType::Xform;
          node_ptr->id = -1;
        }
      }
    } else {
      bool first_assigned = false;
      for (size_t old_id : source_ids) {
        if (old_id >= mesh_nodes_by_id.size()) {
          continue;
        }
        for (Node *node_ptr : mesh_nodes_by_id[old_id]) {
          if (!node_ptr) {
            continue;
          }
          if (!first_assigned) {
            node_ptr->id = new_id;
            first_assigned = true;
          } else {
            node_ptr->category = NodeCategory::Group;
            node_ptr->nodeType = NodeType::Xform;
            node_ptr->id = -1;
          }
        }
      }
    }
  }
  // Attach under the default root so they are reachable from
  // getDefaultRootNode(); falls back to top-level if there is no root node.
  if (!new_root_merged_nodes.empty()) {
    if (!root_nodes.empty()) {
      Node &host = root_nodes[default_root_index];
      for (Node &n : new_root_merged_nodes) {
        host.children.push_back(std::move(n));
      }
    } else {
      for (Node &n : new_root_merged_nodes) {
        root_nodes.push_back(std::move(n));
      }
    }
  }

  // Drop mesh records that are no longer referenced by the node tree.
  // This keeps the exported mesh IDs compact and makes numMeshes() reflect the
  // effective renderable mesh count after native aggregation.
  std::vector<uint8_t> mesh_used(meshes.size(), uint8_t{0});
  std::function<void(const Node &)> markNodeMeshes = [&](const Node &node) {
    if (node.nodeType == NodeType::Mesh && node.id >= 0 &&
        size_t(node.id) < mesh_used.size()) {
      mesh_used[size_t(node.id)] = uint8_t{1};
    }
    for (const Node &child : node.children) {
      markNodeMeshes(child);
    }
  };
  for (const Node &root : root_nodes) {
    markNodeMeshes(root);
  }
  for (const RenderInstance &inst : instances) {
    if (inst.mesh_id >= 0 && size_t(inst.mesh_id) < mesh_used.size()) {
      mesh_used[size_t(inst.mesh_id)] = uint8_t{1};
    }
  }
  for (const RenderLight &light : lights) {
    if (light.geometry_mesh_id >= 0 &&
        size_t(light.geometry_mesh_id) < mesh_used.size()) {
      mesh_used[size_t(light.geometry_mesh_id)] = uint8_t{1};
    }
  }

  std::vector<int32_t> mesh_remap(meshes.size(), -1);
  std::vector<RenderMesh> compact_meshes;
  compact_meshes.reserve(meshes.size());
  for (size_t i = 0; i < meshes.size(); i++) {
    if (!mesh_used[i]) {
      continue;
    }
    mesh_remap[i] = int32_t(compact_meshes.size());
    compact_meshes.push_back(std::move(meshes[i]));
  }

  std::function<void(Node &)> remapNodeMeshes = [&](Node &node) {
    if (node.nodeType == NodeType::Mesh && node.id >= 0 &&
        size_t(node.id) < mesh_remap.size()) {
      node.id = mesh_remap[size_t(node.id)];
    }
    for (Node &child : node.children) {
      remapNodeMeshes(child);
    }
  };
  for (Node &root : root_nodes) {
    remapNodeMeshes(root);
  }
  for (RenderInstance &inst : instances) {
    if (inst.mesh_id >= 0 && size_t(inst.mesh_id) < mesh_remap.size()) {
      inst.mesh_id = mesh_remap[size_t(inst.mesh_id)];
    }
  }
  for (RenderLight &light : lights) {
    if (light.geometry_mesh_id >= 0 &&
        size_t(light.geometry_mesh_id) < mesh_remap.size()) {
      light.geometry_mesh_id = mesh_remap[size_t(light.geometry_mesh_id)];
    }
  }

  // Rebuild meshMap (abs_path -> mesh index) from the surviving node tree.
  // Note: meshes referenced only by instances or lights (not by any Mesh node)
  // are kept in `meshes` but intentionally omitted here. meshMap is a
  // converter-internal lookup used during conversion (e.g. PointInstancer
  // prototype resolution, which runs before this pass) and is not exported into
  // RenderScene, so node-only coverage is sufficient.
  StringAndIdMap remapped_mesh_map;
  std::function<void(const Node &)> remapMeshMapFromNodes =
      [&](const Node &node) {
    if (!node.abs_path.empty() && node.nodeType == NodeType::Mesh &&
        node.id >= 0 && size_t(node.id) < compact_meshes.size()) {
      remapped_mesh_map.add(node.abs_path, uint64_t(node.id));
    }
    for (const Node &child : node.children) {
      remapMeshMapFromNodes(child);
    }
  };
  for (const Node &root : root_nodes) {
    remapMeshMapFromNodes(root);
  }
  meshMap = std::move(remapped_mesh_map);

  const size_t before_compact = meshes.size();
  meshes = std::move(compact_meshes);
  PushInfo("Mesh merge compacted mesh records: " +
           std::to_string(before_compact) + " -> " +
           std::to_string(meshes.size()) + ".");

  return true;
}

}  // namespace tydra
}  // namespace lightusd
