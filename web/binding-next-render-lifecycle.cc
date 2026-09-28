// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "next/schema/usd-shade.hh"
#include "binding-next-assets.hh"
#include "binding-next-layer.hh"
#include "c-api/lightusd-render-c.h"
#include "next/load-usd.hh"
#include "next/composition/composition.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/writer/usdc-writer.hh"
#include "image-loader.hh"
#include "safe-arithmetic.hh"
#include <cmath>
namespace lightusd {
namespace web_next {
static_assert(LIGHTUSD_RENDER_NODE == 0 && LIGHTUSD_RENDER_MESH == 1 &&
              LIGHTUSD_RENDER_MATERIAL == 2 && LIGHTUSD_RENDER_TEXTURE == 3 &&
              LIGHTUSD_RENDER_IMAGE == 4 && LIGHTUSD_RENDER_LIGHT == 5 &&
              LIGHTUSD_RENDER_CAMERA == 6 && LIGHTUSD_RENDER_SKELETON == 7 &&
              LIGHTUSD_RENDER_ANIMATION == 8 && LIGHTUSD_RENDER_UNSUPPORTED == 9 &&
              LIGHTUSD_RENDER_INSTANCER == 10 && LIGHTUSD_RENDER_ROOT_NODE == 11 &&
              LIGHTUSD_RENDER_POINTS == 12 && LIGHTUSD_RENDER_CURVES == 13 &&
              LIGHTUSD_RENDER_POINT_INSTANCE_DRAW == 14,
              "web resource kinds must match the native C render enum");
RenderStream::RenderStream() {
    // Route resolved memory identifiers through ReadAsset rather than a
    // filesystem open. Missing keys still fail at the byte-reader boundary.
    asset_resolver_.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
}
RenderStream::~RenderStream() = default;

int RenderStream::meshField(int mesh_id, uint8_t field) {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return -1;
    const OutputMesh* output = nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size()) {
        // Only merged records own output geometry; like the mesh view, an
        // unmerged record reads its source mesh.
        const OutputMesh& record = outputs_[static_cast<size_t>(mesh_id)];
        if (record.merged) output = &record;
        source_index = record.source_index;
      } else {
        output = &analytic_outputs_[static_cast<size_t>(mesh_id) - outputs_.size()];
      }
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    } else {
      output = &analytic_outputs_[static_cast<size_t>(mesh_id) - meshes_.size()];
    }
    const tr::RenderMesh* source = nullptr;
    if (source_index >= 0 && render_scene_valid_ &&
        static_cast<size_t>(source_index) < meshes_.size()) {
      const std::string path = meshes_[static_cast<size_t>(source_index)]
                                   .GetPrim().GetPath().str();
      const auto it = render_scene_.mesh_by_path.find(path);
      if (it != render_scene_.mesh_by_path.end() && it->second >= 0 &&
          static_cast<size_t>(it->second) < render_scene_.meshes.size()) {
        source = &render_scene_.meshes[static_cast<size_t>(it->second)];
      }
    }
    bool built_source = false;
    if (!output && source_index >= 0 &&
        static_cast<size_t>(source_index) < meshes_.size()) {
      bool soup = false;
      std::string mesh_error;
      built_source = buildRenderMesh_(meshes_[static_cast<size_t>(source_index)].GetPrim(),
                                      &soup, &mesh_error);
      (void)mesh_error;
    }
    const size_t vertices = output ? output->points.size() / 3
                                   : (built_source ? s_points_.size() / 3
                                                   : (source ? source->points.size() / 3 : 0));
    const size_t faces = output
                             ? (output->indices.empty()
                                    ? output->points.size() / 3
                                    : output->indices.size() / 3)
                             : (built_source ? (s_indices_.empty()
                                                    ? s_points_.size() / 3
                                                    : s_indices_.size() / 3)
                                              : (source ? source->face_vertex_counts.size() : 0));
    const int material = output ? output->material_id
                                : (source ? source->material_id : -1);
    switch (field) {
      case 0: return vertices > static_cast<size_t>((std::numeric_limits<int>::max)())
                        ? (std::numeric_limits<int>::max)()
                        : static_cast<int>(vertices);
      case 1: return faces > static_cast<size_t>((std::numeric_limits<int>::max)())
                     ? (std::numeric_limits<int>::max)()
                     : static_cast<int>(faces);
      case 2: return material;
      case 3: return output ? (!output->normals.empty() ? 1 : 0)
                            : (built_source ? (!s_normals_.empty() ? 1 : 0)
                                            : (source ? (!source->normals.empty() ? 1 : 0) : -1));
      case 4: return output ? (!output->uv.empty() ? 1 : 0)
                            : (built_source ? (!s_uv_.empty() ? 1 : 0)
                                            : (source ? (!source->texcoords_0.empty() ? 1 : 0) : -1));
      case 5: return output ? 0
                            : (built_source ? (wantsTangents_(source_index) && computeScratchTangents_() ? 1 : 0)
                                            : (source ? (!source->tangents.empty() ? 1 : 0) : -1));
      case 6: return source ? (!source->texcoords_1.empty() ? 1 : 0) : 0;
      case 7: return source ? (!source->colors.empty() ? 1 : 0) : 0;
      case 8: return source ? (source->skin ? 1 : 0) : 0;
      case 9: return source ? (source->has_bbox ? 1 : 0) : 0;
      case 10: return source && source->skin ? source->skin->skeleton_id : -1;
      default: return -1;
    }
}

const tr::RenderMesh* RenderStream::sourceRenderMesh_(int mesh_id) const {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size())
        source_index = outputs_[static_cast<size_t>(mesh_id)].source_index;
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    }
    if (source_index < 0 || !render_scene_valid_ ||
        static_cast<size_t>(source_index) >= meshes_.size()) return nullptr;
    const std::string path = meshes_[static_cast<size_t>(source_index)]
                                 .GetPrim().GetPath().str();
    const auto it = render_scene_.mesh_by_path.find(path);
    if (it == render_scene_.mesh_by_path.end() || it->second < 0 ||
        static_cast<size_t>(it->second) >= render_scene_.meshes.size()) return nullptr;
    return &render_scene_.meshes[static_cast<size_t>(it->second)];
}

int RenderStream::meshBufferCopy(int mesh_id, uint8_t kind, uint8_t* out,
                                 uint32_t cap) {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return -1;
    const OutputMesh* output = nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size()) {
        // Only merged records own output geometry; like the mesh view, an
        // unmerged record reads its source mesh.
        const OutputMesh& record = outputs_[static_cast<size_t>(mesh_id)];
        if (record.merged) output = &record;
        source_index = record.source_index;
      } else {
        output = &analytic_outputs_[static_cast<size_t>(mesh_id) - outputs_.size()];
      }
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    } else {
      output = &analytic_outputs_[static_cast<size_t>(mesh_id) - meshes_.size()];
    }
    const tr::RenderMesh* source = nullptr;
    if (source_index >= 0 && render_scene_valid_ &&
        static_cast<size_t>(source_index) < meshes_.size()) {
      const std::string path = meshes_[static_cast<size_t>(source_index)]
                                   .GetPrim().GetPath().str();
      const auto it = render_scene_.mesh_by_path.find(path);
      if (it != render_scene_.mesh_by_path.end() && it->second >= 0 &&
          static_cast<size_t>(it->second) < render_scene_.meshes.size()) {
        source = &render_scene_.meshes[static_cast<size_t>(it->second)];
      }
    }
    // Optimized output records only retain the core draw streams. Fall back
    // to the retained source mesh for optional typed attributes.
    if (output && kind >= 4 && source) output = nullptr;
    bool built_source = false;
    if (!output && source_index >= 0 &&
        static_cast<size_t>(source_index) < meshes_.size()) {
      bool soup = false;
      std::string mesh_error;
      built_source = buildRenderMesh_(meshes_[static_cast<size_t>(source_index)].GetPrim(),
                                      &soup, &mesh_error);
      (void)mesh_error;
    }

    size_t count = 0;
    size_t element_size = 0;
    const void* contiguous = nullptr;
    const auto* chunked = static_cast<const tr::FloatChunked*>(nullptr);
    const auto* indices = static_cast<const tr::UInt32Chunked*>(nullptr);
    const auto* joint_indices = static_cast<const tr::UInt16Chunked*>(nullptr);
    if (output) {
      switch (kind) {
        case 0: count = output->points.size(); element_size = sizeof(float); contiguous = output->points.data(); break;
        case 1: count = output->indices.size(); element_size = sizeof(uint32_t); contiguous = output->indices.data(); break;
        case 2: count = output->normals.size(); element_size = sizeof(float); contiguous = output->normals.data(); break;
        case 3: count = output->uv.size(); element_size = sizeof(float); contiguous = output->uv.data(); break;
        case 4: case 5: case 6: case 7: case 8: case 9:
          count = 0; element_size = sizeof(float); break;
        default: return -1;
      }
    } else if (built_source) {
      switch (kind) {
        case 0: count = s_points_.size(); element_size = sizeof(float); contiguous = s_points_.data(); break;
        case 1: count = s_indices_.size(); element_size = sizeof(uint32_t); contiguous = s_indices_.data(); break;
        case 2: count = s_normals_.size(); element_size = sizeof(float); contiguous = s_normals_.data(); break;
        case 3: count = s_uv_.size(); element_size = sizeof(float); contiguous = s_uv_.data(); break;
        case 4:
          if (wantsTangents_(source_index) && computeScratchTangents_()) {
            count = s_tangents_.size();
            contiguous = s_tangents_.data();
          }
          element_size = sizeof(float);
          break;
        case 5: chunked = source ? &source->colors : nullptr; element_size = sizeof(float); break;
        case 6: chunked = source ? &source->opacities : nullptr; element_size = sizeof(float); break;
        case 7:
        case 8:
          prepareScratchSkin_(source);
          if (kind == 7) {
            count = s_joint_indices_.size();
            element_size = sizeof(uint16_t);
            contiguous = s_joint_indices_.data();
          } else {
            count = s_joint_weights_.size();
            element_size = sizeof(float);
            contiguous = s_joint_weights_.data();
          }
          break;
        case 9: chunked = source ? &source->texcoords_1 : nullptr; element_size = sizeof(float); break;
        default: return -1;
      }
      if (chunked) {
        count = chunked->size();
        if (chunked->is_contiguous() && count) contiguous = chunked->chunk_data(0);
      }
    } else if (source) {
      switch (kind) {
        case 0: chunked = &source->points; element_size = sizeof(float); break;
        case 1: indices = source->triangulated_indices.empty()
                             ? &source->face_vertex_indices
                             : &source->triangulated_indices;
                element_size = sizeof(uint32_t); break;
        case 2: chunked = &source->normals; element_size = sizeof(float); break;
        case 3: chunked = &source->texcoords_0; element_size = sizeof(float); break;
        case 4: chunked = &source->tangents; element_size = sizeof(float); break;
        case 5: chunked = &source->colors; element_size = sizeof(float); break;
        case 6: chunked = &source->opacities; element_size = sizeof(float); break;
        case 7:
          if (!source->skin) return -1;
          joint_indices = &source->skin->joint_indices;
          element_size = sizeof(uint16_t);
          break;
        case 8:
          if (!source->skin) return -1;
          chunked = &source->skin->joint_weights;
          element_size = sizeof(float);
          break;
        case 9: chunked = &source->texcoords_1; element_size = sizeof(float); break;
        default: return -1;
      }
      count = chunked ? chunked->size()
                      : (indices ? indices->size() : joint_indices->size());
      if (chunked && chunked->is_contiguous() && count) contiguous = chunked->chunk_data(0);
      if (indices && indices->is_contiguous() && count) contiguous = indices->chunk_data(0);
      if (joint_indices && joint_indices->is_contiguous() && count)
        contiguous = joint_indices->chunk_data(0);
    } else {
      return -1;
    }
    const size_t bytes = count * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (contiguous) {
      std::memcpy(out, contiguous, bytes);
    } else if (chunked) {
      for (size_t i = 0; i < count; ++i)
        std::memcpy(out + i * element_size, &(*chunked)[i], element_size);
    } else if (indices) {
      for (size_t i = 0; i < count; ++i)
        std::memcpy(out + i * element_size, &(*indices)[i], element_size);
    } else {
      for (size_t i = 0; i < count; ++i)
        std::memcpy(out + i * element_size, &(*joint_indices)[i], element_size);
    }
    return required;
}

int RenderStream::nodePathCopy(int node_id, uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const std::string& path = render_scene_.nodes[static_cast<size_t>(node_id)].prim_path;
    if (path.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(path.size());
    if (!out || cap < path.size() || path.empty()) return required;
    std::memcpy(out, path.data(), path.size());
    return required;
}

int RenderStream::nativeInstanceNodeIdsCopy(uint8_t* out, uint32_t cap) const {
  if (!render_scene_valid_) return -1;
  size_t count = 0;
  for (const tr::SceneNode& node : render_scene_.nodes) {
    if (node.is_instance) ++count;
  }
  if (count > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                  sizeof(uint32_t)) return -1;
  const size_t bytes = count * sizeof(uint32_t);
  if (!out || cap < bytes) return static_cast<int>(bytes);
  size_t written = 0;
  for (size_t i = 0; i < render_scene_.nodes.size(); ++i) {
    if (!render_scene_.nodes[i].is_instance) continue;
    const uint32_t id = static_cast<uint32_t>(i);
    std::memcpy(out + written * sizeof(id), &id, sizeof(id));
    ++written;
  }
  return static_cast<int>(bytes);
}

int RenderStream::nodePrototypePathCopy(int node_id, uint8_t* out,
                                        uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const std::string& path =
        render_scene_.nodes[static_cast<size_t>(node_id)].prototype_path;
    if (path.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(path.size());
    if (!out || cap < path.size() || path.empty()) return required;
    std::memcpy(out, path.data(), path.size());
    return required;
}

int RenderStream::nodeTransformCopy(int node_id, uint8_t kind, uint8_t* out,
                                    uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size() || kind > 1) return -1;
    const tr::SceneNode& node = render_scene_.nodes[static_cast<size_t>(node_id)];
    const tr::Matrix4& matrix = kind == 0 ? node.local_transform : node.world_transform;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, matrix.m, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::resourcePathCopy(uint8_t kind, int resource_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || resource_id < 0) return -1;
    const std::string* path = nullptr;
    const size_t id = static_cast<size_t>(resource_id);
    switch (kind) {
      case 0: if (id < render_scene_.nodes.size()) path = &render_scene_.nodes[id].prim_path; break;
      case 1: if (id < render_scene_.meshes.size()) path = &render_scene_.meshes[id].prim_path; break;
      case 2: if (const tr::RenderMaterial* m = outputRenderMaterial_(resource_id)) path = &m->prim_path; break;
      case 3: if (id < render_scene_.textures.size()) path = &render_scene_.textures[id].prim_path; break;
      case 4: if (id < render_scene_.images.size()) path = &render_scene_.images[id].resolved_path; break;
      case 5: if (id < render_scene_.lights.size()) path = &render_scene_.lights[id].prim_path; break;
      case 6: if (id < render_scene_.cameras.size()) path = &render_scene_.cameras[id].prim_path; break;
      case 7: if (id < render_scene_.skeletons.size()) path = &render_scene_.skeletons[id].prim_path; break;
      case 8: if (id < render_scene_.animations.size()) path = &render_scene_.animations[id].prim_path; break;
      case 9: if (id < render_scene_.unsupported_renderables.size()) path = &render_scene_.unsupported_renderables[id].prim_path; break;
      case 10: if (id < render_scene_.point_instancers.size()) path = &render_scene_.point_instancers[id].prim_path; break;
      case 11:
        if (id < render_scene_.root_nodes.size()) {
          const int32_t node = render_scene_.root_nodes[id];
          if (node >= 0 && static_cast<size_t>(node) < render_scene_.nodes.size())
            path = &render_scene_.nodes[static_cast<size_t>(node)].prim_path;
        }
        break;
      case 12: if (id < render_scene_.points.size()) path = &render_scene_.points[id].prim_path; break;
      case 13: if (id < render_scene_.curves.size()) path = &render_scene_.curves[id].prim_path; break;
      case 14:
        if (id < render_scene_.point_instance_draws.size()) {
          const int32_t instancer = render_scene_.point_instance_draws[id].point_instancer_id;
          if (instancer >= 0 && static_cast<size_t>(instancer) < render_scene_.point_instancers.size())
            path = &render_scene_.point_instancers[static_cast<size_t>(instancer)].prim_path;
        }
        break;
      default: return -1;
    }
    if (!path || path->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(path->size());
    if (!out || cap < path->size() || path->empty()) return required;
    std::memcpy(out, path->data(), path->size());
    return required;
}

int RenderStream::resourceNameCopy(uint8_t kind, int resource_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || resource_id < 0) return -1;
    const std::string* name = nullptr;
    const size_t id = static_cast<size_t>(resource_id);
    switch (kind) {
      case 0: if (id < render_scene_.nodes.size()) name = &render_scene_.nodes[id].name; break;
      case 1: if (id < render_scene_.meshes.size()) name = &render_scene_.meshes[id].name; break;
      case 2: if (const tr::RenderMaterial* m = outputRenderMaterial_(resource_id)) name = &m->name; break;
      case 3: if (id < render_scene_.textures.size()) name = &render_scene_.textures[id].name; break;
      case 4: if (id < render_scene_.images.size()) name = &render_scene_.images[id].name; break;
      case 5: if (id < render_scene_.lights.size()) name = &render_scene_.lights[id].name; break;
      case 6: if (id < render_scene_.cameras.size()) name = &render_scene_.cameras[id].name; break;
      case 7: if (id < render_scene_.skeletons.size()) name = &render_scene_.skeletons[id].name; break;
      case 8: if (id < render_scene_.animations.size()) name = &render_scene_.animations[id].name; break;
      case 10: if (id < render_scene_.point_instancers.size()) name = &render_scene_.point_instancers[id].name; break;
      case 11:
        if (id < render_scene_.root_nodes.size()) {
          const int32_t node = render_scene_.root_nodes[id];
          if (node >= 0 && static_cast<size_t>(node) < render_scene_.nodes.size())
            name = &render_scene_.nodes[static_cast<size_t>(node)].name;
        }
        break;
      case 12: if (id < render_scene_.points.size()) name = &render_scene_.points[id].name; break;
      case 13: if (id < render_scene_.curves.size()) name = &render_scene_.curves[id].name; break;
      case 15: if (id < render_scene_.skeletons.size()) name = &render_scene_.skeletons[id].display_name; break;
      default: return -1;
    }
    if (!name || name->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(name->size());
    if (!out || cap < name->size() || name->empty()) return required;
    std::memcpy(out, name->data(), name->size());
    return required;
}

int RenderStream::recordPathCopy(uint8_t kind, int record_id, uint8_t* out,
                                 uint32_t cap) const {
    if (kind > LIGHTUSD_RENDER_POINT_INSTANCE_DRAW) return -1;
    return resourcePathCopy(kind, record_id, out, cap);
}

int RenderStream::pointsBufferCopy(int points_id, uint8_t kind, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || points_id < 0 ||
        static_cast<size_t>(points_id) >= render_scene_.points.size() || kind > 2) return -1;
    const tr::RenderPoints& points = render_scene_.points[static_cast<size_t>(points_id)];
    const tr::FloatChunked* values = nullptr;
    switch (kind) {
      case 0: values = &points.points; break;
      case 1: values = &points.widths; break;
      case 2: values = &points.colors; break;
    }
    const size_t bytes = values->size() * sizeof(float);
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (values->is_contiguous()) {
      std::memcpy(out, values->chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < values->size(); ++i) std::memcpy(out + i * sizeof(float), &(*values)[i], sizeof(float));
    }
    return required;
}

int RenderStream::pointsInfo(int points_id, lightusd_next_points_info* out) const {
    static_assert(sizeof(lightusd_next_points_info) == 40,
                  "points info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_points_info) ||
        !render_scene_valid_ || points_id < 0 ||
        static_cast<size_t>(points_id) >= render_scene_.points.size()) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    const tr::RenderPoints& points =
        render_scene_.points[static_cast<size_t>(points_id)];
    if (points.point_count() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
      return -1;
    out->point_count = static_cast<int32_t>(points.point_count());
    out->material_id = points.material_id;
    out->has_bounds = points.has_bbox ? 1 : 0;
    if (points.has_bbox) {
      out->bbox_min[0] = points.bbox_min.x;
      out->bbox_min[1] = points.bbox_min.y;
      out->bbox_min[2] = points.bbox_min.z;
      out->bbox_max[0] = points.bbox_max.x;
      out->bbox_max[1] = points.bbox_max.y;
      out->bbox_max[2] = points.bbox_max.z;
    }
    return 0;
}

int RenderStream::curvesBufferCopy(int curves_id, uint8_t kind, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || curves_id < 0 ||
        static_cast<size_t>(curves_id) >= render_scene_.curves.size() || kind > 9) return -1;
    const tr::RenderCurves& curves = render_scene_.curves[static_cast<size_t>(curves_id)];
    const tr::FloatChunked* values = nullptr;
    const std::vector<uint32_t>* counts = nullptr;
    switch (kind) {
      case 0: values = &curves.points; break;
      case 1: values = &curves.tessellated_points; break;
      case 2: values = &curves.widths; break;
      case 3: values = &curves.colors; break;
      case 4: counts = &curves.curve_vertex_counts; break;
      case 5: counts = &curves.tessellated_vertex_counts; break;
      case 6: values = &curves.tessellated_widths; break;
      case 7: values = &curves.tessellated_colors; break;
      case 8: values = &curves.opacities; break;
      case 9: values = &curves.tessellated_opacities; break;
    }
    const size_t element_count = counts ? counts->size() : values->size();
    if (element_count > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                            sizeof(uint32_t)) return -1;
    const size_t bytes = element_count * sizeof(uint32_t);
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (counts) {
      std::memcpy(out, counts->data(), bytes);
    } else if (values->is_contiguous()) {
      std::memcpy(out, values->chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < values->size(); ++i) std::memcpy(out + i * sizeof(float), &(*values)[i], sizeof(float));
    }
    return required;
}

int RenderStream::curvesInfo(int curves_id, lightusd_next_curves_info* out) const {
    static_assert(sizeof(lightusd_next_curves_info) == 48,
                  "curves info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_curves_info) ||
        !render_scene_valid_ || curves_id < 0 ||
        static_cast<size_t>(curves_id) >= render_scene_.curves.size()) return -1;
    const tr::RenderCurves& curves =
        render_scene_.curves[static_cast<size_t>(curves_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    if (curves.curve_count() > limit || curves.control_point_count() > limit ||
        curves.tessellated_point_count() > limit) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->curve_count = static_cast<int32_t>(curves.curve_count());
    out->control_point_count = static_cast<int32_t>(curves.control_point_count());
    out->tessellated_point_count = static_cast<int32_t>(curves.tessellated_point_count());
    out->material_id = curves.material_id;
    out->has_bounds = curves.has_bbox ? 1 : 0;
    if (curves.has_bbox) {
      out->bbox_min[0] = curves.bbox_min.x;
      out->bbox_min[1] = curves.bbox_min.y;
      out->bbox_min[2] = curves.bbox_min.z;
      out->bbox_max[0] = curves.bbox_max.x;
      out->bbox_max[1] = curves.bbox_max.y;
      out->bbox_max[2] = curves.bbox_max.z;
    }
    return 0;
}

int RenderStream::curvesField(int curves_id, uint8_t field) const {
    if (!render_scene_valid_ || curves_id < 0 ||
        static_cast<size_t>(curves_id) >= render_scene_.curves.size() || field > 7) return -1;
    const tr::RenderCurves& curves = render_scene_.curves[static_cast<size_t>(curves_id)];
    switch (field) {
      case 0: return static_cast<int>(curves.type);
      case 1: return static_cast<int>(curves.basis);
      case 2: return static_cast<int>(curves.wrap);
      case 3: return curves.is_nurbs ? 1 : 0;
      case 4: return curves.is_hermite ? 1 : 0;
      case 5: return static_cast<int>(curves.widths_interp);
      case 6: return static_cast<int>(curves.colors_interp);
      case 7: return static_cast<int>(curves.opacities_interp);
    }
    return -1;
}

int RenderStream::instancerBufferCopy(int instancer_id, uint8_t kind,
                                      uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || instancer_id < 0 ||
        static_cast<size_t>(instancer_id) >= render_scene_.point_instancers.size() || kind > 9) return -1;
    const tr::RenderPointInstancer& instancer =
        render_scene_.point_instancers[static_cast<size_t>(instancer_id)];
    const void* data = nullptr;
    size_t bytes = 0;
    switch (kind) {
      case 0: data = instancer.compact_instances.data(); bytes = instancer.compact_instances.size() * sizeof(tr::RenderPointInstancer::CompactInstance); break;
      case 1: data = instancer.positions.data(); bytes = instancer.positions.size() * sizeof(float); break;
      case 2: data = instancer.orientations.data(); bytes = instancer.orientations.size() * sizeof(float); break;
      case 3: data = instancer.scales.data(); bytes = instancer.scales.size() * sizeof(float); break;
      case 4: data = instancer.proto_indices.data(); bytes = instancer.proto_indices.size() * sizeof(int32_t); break;
      case 5: data = instancer.instance_visible.data(); bytes = instancer.instance_visible.size(); break;
      case 6: data = instancer.prototype_node_ids.data(); bytes = instancer.prototype_node_ids.size() * sizeof(int32_t); break;
      case 7: data = instancer.prototype_mesh_offsets.data(); bytes = instancer.prototype_mesh_offsets.size() * sizeof(uint32_t); break;
      case 8: data = instancer.prototype_mesh_ids.data(); bytes = instancer.prototype_mesh_ids.size() * sizeof(int32_t); break;
      case 9: data = instancer.prototype_mesh_transforms.data(); bytes = instancer.prototype_mesh_transforms.size() * sizeof(tr::Matrix4); break;
    }
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    std::memcpy(out, data, bytes);
    return required;
}

int RenderStream::instancerInfo(int instancer_id,
                                lightusd_next_instancer_info* out) const {
    static_assert(sizeof(lightusd_next_instancer_info) == 48,
                  "instancer info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_instancer_info) ||
        !render_scene_valid_ || instancer_id < 0 ||
        static_cast<size_t>(instancer_id) >= render_scene_.point_instancers.size())
      return -1;
    const tr::RenderPointInstancer& instancer =
        render_scene_.point_instancers[static_cast<size_t>(instancer_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    if (instancer.draw_start > limit || instancer.draw_count > limit ||
        instancer.prototype_count() > limit || instancer.instance_count() > limit ||
        instancer.visible_instance_count() > limit) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->draw_start = static_cast<int32_t>(instancer.draw_start);
    out->draw_count = static_cast<int32_t>(instancer.draw_count);
    out->prototype_count = static_cast<int32_t>(instancer.prototype_count());
    out->instance_count = static_cast<int32_t>(instancer.instance_count());
    out->visible_instance_count =
        static_cast<int32_t>(instancer.visible_instance_count());
    out->has_transforms = instancer.transforms.empty() ? 0 : 1;
    out->has_orientations = instancer.has_orientations() ? 1 : 0;
    out->has_scales = instancer.has_scales() ? 1 : 0;
    out->has_velocities = instancer.has_velocities() ? 1 : 0;
    out->has_angular_velocities = instancer.has_angular_velocities() ? 1 : 0;
    out->valid = instancer.valid ? 1 : 0;
    return 0;
}

int RenderStream::instancerStringCopy(int instancer_id, int prototype_id,
                                      uint8_t kind, uint8_t* out,
                                      uint32_t cap) const {
    if (!render_scene_valid_ || instancer_id < 0 || kind > 1 ||
        static_cast<size_t>(instancer_id) >= render_scene_.point_instancers.size()) return -1;
    const auto& instancer = render_scene_.point_instancers[static_cast<size_t>(instancer_id)];
    const std::string* value_ptr = &instancer.validation_error;
    if (kind == 0) {
      if (prototype_id < 0 ||
          static_cast<size_t>(prototype_id) >= instancer.prototype_paths.size()) return -1;
      value_ptr = &instancer.prototype_paths[static_cast<size_t>(prototype_id)];
    }
    const std::string& value = *value_ptr;
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value.size());
    if (!out || cap < value.size() || value.empty()) return required;
    std::memcpy(out, value.data(), value.size());
    return required;
}

int RenderStream::pointInstanceDrawField(int draw_id, uint8_t field) const {
    if (!render_scene_valid_ || draw_id < 0 ||
        static_cast<size_t>(draw_id) >= render_scene_.point_instance_draws.size())
      return -1;
    const tr::RenderPointInstanceDraw& draw =
        render_scene_.point_instance_draws[static_cast<size_t>(draw_id)];
    switch (field) {
      case 0: return draw.point_instancer_id;
      case 1: return draw.instance_index > static_cast<uint32_t>((std::numeric_limits<int>::max)())
                        ? (std::numeric_limits<int>::max)()
                        : static_cast<int>(draw.instance_index);
      case 2: return draw.prototype_index > static_cast<uint32_t>((std::numeric_limits<int>::max)())
                        ? (std::numeric_limits<int>::max)()
                        : static_cast<int>(draw.prototype_index);
      case 3: return draw.mesh_id;
      case 4: return draw.material_id;
      case 5: return draw.expanded_mesh_id;
      default: return -1;
    }
}

int RenderStream::pointInstanceDrawTransformCopy(int draw_id, uint8_t* out,
                                                 uint32_t cap) const {
    if (!render_scene_valid_ || draw_id < 0 ||
        static_cast<size_t>(draw_id) >= render_scene_.point_instance_draws.size())
      return -1;
    constexpr size_t kBytes = 16 * sizeof(float);
    const int required = static_cast<int>(kBytes);
    if (!out || cap < kBytes) return required;
    const tr::Matrix4& transform =
        render_scene_.point_instance_draws[static_cast<size_t>(draw_id)].transform;
    std::memcpy(out, transform.m, kBytes);
    return required;
}

int RenderStream::lightField(int light_id, uint8_t field) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)()) ? (std::numeric_limits<int>::min)()
           : v >= static_cast<double>((std::numeric_limits<int>::max)()) ? (std::numeric_limits<int>::max)()
           : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return static_cast<int>(light.type);
      case 1: return scaled(light.intensity);
      case 2: return scaled(light.exposure);
      case 3: return light.normalize ? 1 : 0;
      case 4: return light.enable_shadow ? 1 : 0;
      default: return -1;
    }
}

int RenderStream::lightTransformCopy(int light_id, uint8_t* out,
                                     uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, render_scene_.lights[static_cast<size_t>(light_id)].transform.m,
                kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::lightColorCopy(int light_id, uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    constexpr size_t kBytes = 3 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    const tr::Float3& color =
        render_scene_.lights[static_cast<size_t>(light_id)].color;
    std::memcpy(out, &color.x, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::lightInfo(int light_id, lightusd_next_light_info* out) const {
    static_assert(sizeof(lightusd_next_light_info) == 192,
                  "light info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_light_info) ||
        !render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    const size_t counts[] = {light.light_link_targets.size(),
        light.shadow_link_targets.size(), light.filter_targets.size(),
        light.light_link_mesh_indices.size(), light.shadow_link_mesh_indices.size()};
    for (size_t count : counts) if (count > limit) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->type = static_cast<int32_t>(light.type);
    out->flags = (light.normalize ? 1u : 0u) |
        (light.enable_color_temperature ? 2u : 0u) |
        (light.shaping_ies_normalize ? 4u : 0u) |
        (light.light_links_all ? 8u : 0u) |
        (light.shadow_links_all ? 16u : 0u) |
        (light.enable_shadow ? 32u : 0u);
    for (size_t i = 0; i < 5; ++i) out->counts[i] = static_cast<int32_t>(counts[i]);
    out->dome_texture_id = -1;
    float* v = out->values;
    v[0] = light.intensity;
    v[1] = light.exposure;
    v[2] = light.color_temperature;
    v[3] = light.diffuse;
    v[4] = light.specular;
    v[5] = light.shaping_focus;
    v[6] = light.shaping_focus_tint.x;
    v[7] = light.shaping_focus_tint.y;
    v[8] = light.shaping_focus_tint.z;
    v[9] = light.shaping_cone_softness;
    v[10] = light.shaping_ies_angle_scale;
    v[11] = light.shadow_distance;
    v[12] = light.shadow_falloff;
    v[13] = light.shadow_falloff_gamma;
    v[14] = light.color.x; v[15] = light.color.y; v[16] = light.color.z;
    v[17] = light.shadow_color.x;
    v[18] = light.shadow_color.y;
    v[19] = light.shadow_color.z;
    std::memcpy(out->transform, light.transform.m, sizeof(out->transform));
    switch (light.type) {
      case tr::LightType::Sphere: v[20] = light.params.sphere.radius; break;
      case tr::LightType::Rect:
        v[20] = light.params.rect.width; v[21] = light.params.rect.height; break;
      case tr::LightType::Disk: v[20] = light.params.disk.radius; break;
      case tr::LightType::Spot: v[20] = light.params.spot.angle; break;
      case tr::LightType::Cylinder:
        v[20] = light.params.cylinder.radius;
        v[21] = light.params.cylinder.length; break;
      case tr::LightType::Directional: v[20] = light.params.distant.angle; break;
      case tr::LightType::Dome:
        out->dome_texture_id = light.params.dome.texture_id;
        out->dome_texture_format = static_cast<int32_t>(light.params.dome.texture_format);
        if (out->dome_texture_id >= 0 &&
            static_cast<size_t>(out->dome_texture_id) < render_scene_.images.size())
          out->flags |= 64u;
        break;
      default: break;
    }
    return 0;
}

int RenderStream::lightStringCopy(int light_id, uint8_t kind, int item_id,
                                  uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 || kind > 4 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const std::string* value = nullptr;
    if (kind == 0) value = &light.shaping_ies_file;
    else if (kind == 4) {
      if (light.type != tr::LightType::Dome || light.params.dome.texture_id < 0 ||
          static_cast<size_t>(light.params.dome.texture_id) >= render_scene_.images.size())
        return -1;
      const auto& image = render_scene_.images[static_cast<size_t>(light.params.dome.texture_id)];
      value = image.name.empty() ? &image.resolved_path : &image.name;
    } else {
      const auto& strings = kind == 1 ? light.light_link_targets
          : kind == 2 ? light.shadow_link_targets : light.filter_targets;
      if (item_id < 0 || static_cast<size_t>(item_id) >= strings.size()) return -1;
      value = &strings[static_cast<size_t>(item_id)];
    }
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::lightMeshIdsCopy(int light_id, uint8_t kind, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 || kind > 1 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const auto& ids = kind == 0 ? light.light_link_mesh_indices
                                 : light.shadow_link_mesh_indices;
    if (ids.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                         sizeof(int32_t)) return -1;
    const int required = static_cast<int>(ids.size() * sizeof(int32_t));
    if (!out || cap < static_cast<uint32_t>(required) || ids.empty()) return required;
    std::memcpy(out, ids.data(), static_cast<size_t>(required));
    return required;
}

int RenderStream::cameraField(int camera_id, uint8_t field) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const tr::RenderCamera& camera = render_scene_.cameras[static_cast<size_t>(camera_id)];
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)()) ? (std::numeric_limits<int>::min)()
           : v >= static_cast<double>((std::numeric_limits<int>::max)()) ? (std::numeric_limits<int>::max)()
           : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return static_cast<int>(camera.type);
      case 1: return scaled(camera.focal_length);
      case 2: return scaled(camera.ortho_width);
      case 3: return scaled(camera.near_clip);
      case 4: return scaled(camera.far_clip);
      case 5: return scaled(camera.fov_x());
      case 6: return scaled(camera.fov_y());
      default: return -1;
    }
}

int RenderStream::cameraInfo(int camera_id, lightusd_next_camera_info* out) const {
    static_assert(sizeof(lightusd_next_camera_info) == 88,
                  "camera info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_camera_info) ||
        !render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    const tr::RenderCamera& camera =
        render_scene_.cameras[static_cast<size_t>(camera_id)];
    out->type = static_cast<int32_t>(camera.type);
    out->focal_length = camera.focal_length;
    out->horizontal_aperture = camera.horizontal_aperture;
    out->vertical_aperture = camera.vertical_aperture;
    out->ortho_width = camera.ortho_width;
    out->near_clip = camera.near_clip;
    out->far_clip = camera.far_clip;
    out->focus_distance = camera.focus_distance;
    out->fstop = camera.fstop;
    out->fov_x = camera.fov_x();
    out->fov_y = camera.fov_y();
    out->aspect = camera.aspect_ratio();
    out->horizontal_aperture_offset = camera.horizontal_aperture_offset;
    out->vertical_aperture_offset = camera.vertical_aperture_offset;
    out->exposure = camera.exposure;
    out->stereo_role = static_cast<int32_t>(camera.stereo_role);
    out->shutter_open = camera.shutter_open;
    out->shutter_close = camera.shutter_close;
    return 0;
}

int RenderStream::cameraTransformCopy(int camera_id, uint8_t* out,
                                      uint32_t cap) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, render_scene_.cameras[static_cast<size_t>(camera_id)].transform.m,
                kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::cameraOpticsCopy(int camera_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const tr::RenderCamera& camera = render_scene_.cameras[static_cast<size_t>(camera_id)];
    const std::array<float, 8> values = {
        camera.focal_length, camera.horizontal_aperture,
        camera.vertical_aperture, camera.ortho_width,
        camera.near_clip, camera.far_clip, camera.fov_x(), camera.fov_y()};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::skeletonField(int skeleton_id, uint8_t field) const {
    if (!render_scene_valid_ || skeleton_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size()) return -1;
    const tr::Skeleton& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    switch (field) {
      case 0: return skeleton.joints.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(skeleton.joints.size());
      case 1: return skeleton.root_joint;
      case 2: return skeleton.animation_id;
      default: return -1;
    }
}

int RenderStream::skeletonJointBufferCopy(int skeleton_id, uint8_t kind,
                                          uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size() || kind > 2) return -1;
    const tr::Skeleton& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    size_t count = skeleton.joints.size();
    size_t element_size = kind == 2 ? sizeof(int32_t) : 16 * sizeof(double);
    size_t bytes = count * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    for (size_t i = 0; i < count; ++i) {
      if (kind == 2) {
        std::memcpy(out + i * element_size, &skeleton.joints[i].parent_id, element_size);
      } else {
        const tr::Matrix4d& matrix = kind == 0 ? skeleton.joints[i].bind_transform
                                               : skeleton.joints[i].rest_transform;
        std::memcpy(out + i * element_size, matrix.m, element_size);
      }
    }
    return required;
}

int RenderStream::skeletonJointChildrenCopy(int skeleton_id, int joint_id,
                                            uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 || joint_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size())
      return -1;
    const auto& joints = render_scene_.skeletons[static_cast<size_t>(skeleton_id)].joints;
    if (static_cast<size_t>(joint_id) >= joints.size()) return -1;
    const auto& children = joints[static_cast<size_t>(joint_id)].children;
    if (children.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                              sizeof(int32_t)) return -1;
    const int required = static_cast<int>(children.size() * sizeof(int32_t));
    if (!out || cap < static_cast<uint32_t>(required) || children.empty())
      return required;
    std::memcpy(out, children.data(), static_cast<size_t>(required));
    return required;
}

int RenderStream::skeletonJointStringCopy(int skeleton_id, int joint_id,
                                          uint8_t kind, uint8_t* out,
                                          uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 || kind > 2 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size()) return -1;
    const auto& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    const std::string* value_ptr = &skeleton.animation_source_path;
    if (kind != 2) {
      if (joint_id < 0 || static_cast<size_t>(joint_id) >= skeleton.joints.size())
        return -1;
      const auto& joint = skeleton.joints[static_cast<size_t>(joint_id)];
      value_ptr = kind == 0 ? &joint.name : &joint.path;
    }
    const std::string& value = *value_ptr;
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value.size());
    if (!out || cap < value.size() || value.empty()) return required;
    std::memcpy(out, value.data(), value.size());
    return required;
}

int RenderStream::materialField(int material_id, uint8_t field) const {
    const tr::RenderMaterial* found = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !found) return -1;
    const tr::RenderMaterial& material = *found;
    switch (field) {
      case 0: return static_cast<int>(material.shader_type);
      case 1: return static_cast<int>(material.alpha_mode);
      case 2: return material.double_sided ? 1 : 0;
      case 3: {
        float opacity = 1.0f;
        if (material.preview_surface) opacity = material.preview_surface->opacity.as_float();
        else if (material.openpbr) opacity = material.openpbr->opacity.as_float();
        if (!std::isfinite(opacity)) return 0;
        const double scaled = static_cast<double>(opacity) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 4: {
        float roughness = 0.0f;
        if (material.preview_surface) roughness = material.preview_surface->roughness.as_float();
        else if (material.openpbr) roughness = material.openpbr->specular_roughness.as_float();
        if (!std::isfinite(roughness)) return 0;
        const double scaled = static_cast<double>(roughness) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 5: {
        float clearcoat = 0.0f;
        if (material.preview_surface) clearcoat = material.preview_surface->clearcoat.as_float();
        if (!std::isfinite(clearcoat)) return 0;
        const double scaled = static_cast<double>(clearcoat) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 6: {
        float clearcoat_roughness = 0.0f;
        if (material.preview_surface)
          clearcoat_roughness = material.preview_surface->clearcoat_roughness.as_float();
        if (!std::isfinite(clearcoat_roughness)) return 0;
        const double scaled = static_cast<double>(clearcoat_roughness) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 7: return material.default_fallback ? 1 : 0;
      default: return -1;
    }
}

int RenderStream::materialDiagnosticCount(int material_id) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material) return -1;
    const size_t count = material->diagnostics.size();
    return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(count)
               : -1;
}

int RenderStream::materialDiagnosticKind(int material_id,
                                         int diagnostic_id) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || diagnostic_id < 0) return -1;
    const auto& diagnostics = material->diagnostics;
    if (static_cast<size_t>(diagnostic_id) >= diagnostics.size()) return -1;
    return static_cast<int>(diagnostics[static_cast<size_t>(diagnostic_id)].kind);
}

int RenderStream::materialDiagnosticStringCopy(int material_id,
                                                int diagnostic_id,
                                                uint8_t kind, uint8_t* out,
                                                uint32_t cap) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || diagnostic_id < 0 || kind > 3) return -1;
    const auto& diagnostics = material->diagnostics;
    if (static_cast<size_t>(diagnostic_id) >= diagnostics.size()) return -1;
    const tr::MaterialDiagnostic& diagnostic =
        diagnostics[static_cast<size_t>(diagnostic_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &diagnostic.material_path; break;
      case 1: value = &diagnostic.node_path; break;
      case 2: value = &diagnostic.shader_id; break;
      case 3: value = &diagnostic.message; break;
      default: return -1;
    }
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

namespace {
const tr::ShaderParam* CommonMaterialParam(const tr::RenderMaterial& material,
                                           uint8_t param) {
  if (material.shader_type == tr::RenderMaterial::ShaderType::PreviewSurface &&
      material.preview_surface) {
    const auto& s = *material.preview_surface;
    switch (param) {
      case 0: return &s.diffuse_color;
      case 1: return &s.emissive_color;
      case 2: return &s.metallic;
      case 3: return &s.roughness;
      case 4: return &s.opacity;
    }
  } else if (material.shader_type == tr::RenderMaterial::ShaderType::OpenPBR &&
             material.openpbr) {
    const auto& s = *material.openpbr;
    switch (param) {
      case 0: return &s.base_color;
      case 1: return &s.emission_color;
      case 2: return &s.base_metalness;
      case 3: return &s.base_roughness;
      case 4: return &s.opacity;
    }
  }
  return nullptr;
}
}  // namespace

int RenderStream::materialParamBufferCopy(int material_id, uint8_t param,
                                          uint8_t* out, uint32_t cap) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || param > 4) return -1;
    const tr::ShaderParam* value = CommonMaterialParam(*material, param);
    if (!value) return -1;
    constexpr size_t kBytes = 4 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, &value->value, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::materialParamTextureId(int material_id, uint8_t param) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || param > 4) return -1;
    const tr::ShaderParam* value = CommonMaterialParam(*material, param);
    return value ? value->texture_id : -1;
}

int RenderStream::textureField(int texture_id, uint8_t field) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const tr::TextureImage* image =
        (texture.image_id >= 0 &&
         static_cast<size_t>(texture.image_id) < render_scene_.images.size())
            ? &render_scene_.images[static_cast<size_t>(texture.image_id)]
            : nullptr;
    switch (field) {
      case 0: return texture.image_id;
      case 1: return image ? static_cast<int>(image->width) : 0;
      case 2: return image ? static_cast<int>(image->height) : 0;
      case 3: return image ? image->channels : 0;
      case 4: return image ? image->mip_levels : 0;
      case 5: return image && image->is_loaded() ? 1 : 0;
      case 6: return static_cast<int>(texture.wrap_s);
      case 7: return static_cast<int>(texture.wrap_t);
      case 8: return static_cast<int>(texture.output_channel);
      case 9: {
        const double scaled = static_cast<double>(texture.rotation) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 10: return texture.has_transform2d ? 1 : 0;
      case 11: return texture.is_udim ? 1 : 0;
      case 12: return texture.udim_texture_id;
      default: return -1;
    }
}

int RenderStream::textureStringCopy(int texture_id, uint8_t kind,
                                    uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 || kind > 2 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &texture.uv_primvar; break;
      case 1: value = &texture.source_color_space; break;
      case 2: value = &texture.target_color_space; break;
      default: return -1;
    }
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::textureImageBufferCopy(int texture_id, uint8_t* out,
                                         uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    if (texture.image_id < 0 ||
        static_cast<size_t>(texture.image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image =
        render_scene_.images[static_cast<size_t>(texture.image_id)];
    const size_t bytes = image.data.size();
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (image.data.is_contiguous()) {
      std::memcpy(out, image.data.chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < bytes; ++i) out[i] = image.data[i];
    }
    return required;
}

int RenderStream::imageField(int image_id, uint8_t field) const {
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    switch (field) {
      case 0: return static_cast<int>(image.width);
      case 1: return static_cast<int>(image.height);
      case 2: return image.channels;
      case 3: return image.mip_levels;
      case 4: return image.is_loaded() ? 1 : 0;
      case 5: return static_cast<int>(image.component_type);
      case 6: return static_cast<int>(image.color_space);
      case 7:
        return image.data.size() <= (size_t{1} << 29)
            ? static_cast<int>(image.data.size()) : -1;
      default: return -1;
    }
}

int RenderStream::imageAssetIdentifierCopy(int image_id, uint8_t* out,
                                           uint32_t cap) const {
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const std::string& value = render_scene_.images[static_cast<size_t>(image_id)].asset_identifier;
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value.size());
    if (!out || cap < value.size() || value.empty()) return required;
    std::memcpy(out, value.data(), value.size());
    return required;
}

int RenderStream::imageBufferCopy(int image_id, uint8_t* out,
                                  uint32_t cap) const {
    constexpr size_t kMaxImageCopyBytes = size_t{1} << 29;
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    const size_t bytes = image.data.size();
    if (bytes > kMaxImageCopyBytes ||
        bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (image.data.is_contiguous()) {
      std::memcpy(out, image.data.chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < bytes; ++i) out[i] = image.data[i];
    }
    return required;
}

uintptr_t RenderStream::imageBufferData(int image_id) const {
    constexpr size_t kMaxImageCopyBytes = size_t{1} << 29;
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return 0;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    if (image.data.empty() || !image.data.is_contiguous() ||
        image.data.size() > kMaxImageCopyBytes) return 0;
    return reinterpret_cast<uintptr_t>(image.data.chunk_data(0));
}

int RenderStream::textureSamplingBufferCopy(int texture_id, uint8_t* out,
                                            uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 13> values = {
        texture.offset.x, texture.offset.y, texture.scale.x, texture.scale.y,
        texture.rotation,
        texture.bias.x, texture.bias.y, texture.bias.z, texture.bias.w,
        texture.scale_value.x, texture.scale_value.y,
        texture.scale_value.z, texture.scale_value.w};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::textureTransformBufferCopy(int texture_id, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 5> values = {
        texture.tx_rotation, texture.tx_scale.x, texture.tx_scale.y,
        texture.tx_translation.x, texture.tx_translation.y};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
  return static_cast<int>(kBytes);
}

int RenderStream::textureUDIMRemapBufferCopy(int texture_id, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 4> values = {
        texture.udim_uv_scale.x, texture.udim_uv_scale.y,
        texture.udim_uv_offset.x, texture.udim_uv_offset.y};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::udimTileCount(int udim_id) const {
    if (!render_scene_valid_ || udim_id < 0 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const size_t count = render_scene_.udim_textures[static_cast<size_t>(udim_id)].tiles.size();
    return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(count) : -1;
}

int RenderStream::udimTextureCount() const {
    if (!render_scene_valid_) return 0;
    return render_scene_.udim_textures.size() <=
                   static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(render_scene_.udim_textures.size()) : -1;
}

int RenderStream::udimTileBufferCopy(int udim_id, uint8_t* out,
                                     uint32_t cap) const {
    if (!render_scene_valid_ || udim_id < 0 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const auto& tiles = render_scene_.udim_textures[static_cast<size_t>(udim_id)].tiles;
    constexpr size_t kRecordBytes = 4 * sizeof(int32_t);
    if (tiles.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) / kRecordBytes) return -1;
    const size_t required = tiles.size() * kRecordBytes;
    if (!out || cap < required || required == 0) return static_cast<int>(required);
    for (size_t i = 0; i < tiles.size(); ++i) {
      const int32_t record[4] = {
          static_cast<int32_t>(tiles[i].udim),
          static_cast<int32_t>((tiles[i].udim - 1001u) % 10u),
          static_cast<int32_t>((tiles[i].udim - 1001u) / 10u),
          tiles[i].image_id};
      std::memcpy(out + i * kRecordBytes, record, kRecordBytes);
    }
    return static_cast<int>(required);
}

int RenderStream::udimStringCopy(int udim_id, uint8_t kind, uint8_t* out,
                                 uint32_t cap) const {
    if (!render_scene_valid_ || udim_id < 0 || kind > 3 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const auto& udim = render_scene_.udim_textures[static_cast<size_t>(udim_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &udim.prim_name; break;
      case 1: value = &udim.abs_path; break;
      case 2: value = &udim.display_name; break;
      case 3: value = &udim.asset_identifier; break;
      default: return -1;
    }
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::textureColorTransformBufferCopy(int texture_id,
                                                  uint8_t* out,
                                                  uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 14> values = {
        texture.color_transform_valid ? 1.0f : 0.0f,
        texture.color_transform_bypass ? 1.0f : 0.0f,
        texture.source_color_is_data ? 1.0f : 0.0f,
        texture.source_gamma,
        texture.source_linear_bias,
        texture.source_to_display_linear[0],
        texture.source_to_display_linear[1],
        texture.source_to_display_linear[2],
        texture.source_to_display_linear[3],
        texture.source_to_display_linear[4],
        texture.source_to_display_linear[5],
        texture.source_to_display_linear[6],
        texture.source_to_display_linear[7],
        texture.source_to_display_linear[8]};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::sceneField(uint8_t field) const {
    if (!render_scene_valid_) return -1;
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)())
                 ? (std::numeric_limits<int>::min)()
                 : v >= static_cast<double>((std::numeric_limits<int>::max)())
                       ? (std::numeric_limits<int>::max)()
                       : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return scaled(static_cast<double>(render_scene_.meters_per_unit) * 1000.0);
      case 1: return static_cast<int>(render_scene_.up_axis);
      case 2: return scaled(render_scene_.start_time);
      case 3: return scaled(render_scene_.end_time);
      case 4: return scaled(render_scene_.frames_per_second);
      default: return -1;
    }
}

int RenderStream::sceneStringCopy(uint8_t kind, uint8_t* out,
                                  uint32_t cap) const {
    if (!loaded_ || kind > 6) return -1;
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &render_scene_.name; break;
      case 1: value = &render_scene_.default_prim; break;
      case 2: value = &render_scene_.render_settings_path; break;
      case 3: value = &render_scene_.working_color_space; break;
      case 4: value = &stage_.GetMeta().upAxis; break;
      case 5: value = &stage_.GetMeta().comment; break;
      case 6: value = &stage_.GetMeta().copyright; break;
    }
    if (!value || value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap == 0 || value->empty()) return required;
    const size_t copy = std::min<size_t>(value->size(), cap - 1);
    std::memcpy(out, value->data(), copy);
    out[copy] = 0;
    return required;
}

int RenderStream::sceneMetadata(lightusd_next_scene_metadata* out) const {
    static_assert(sizeof(lightusd_next_scene_metadata) == 96,
                  "scene metadata POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_scene_metadata)) return -1;
    if (!loaded_) return 1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    const lightusd::next::StageMeta& meta = stage_.GetMeta();
    out->reserved = (meta.startTimeCode_set ? 1u : 0u) |
                    (meta.endTimeCode_set ? 2u : 0u) |
                    (meta.autoPlay_set ? 4u : 0u) |
                    (meta.autoPlay ? 8u : 0u);
    out->meters_per_unit = meta.metersPerUnit_set ? meta.metersPerUnit : 1.0;
    out->kilograms_per_unit = meta.kilogramsPerUnit;
    out->frames_per_second = meta.framesPerSecond;
    out->time_codes_per_second = meta.timeCodesPerSecond;
    out->start_time_code = meta.startTimeCode;
    out->end_time_code = meta.endTimeCode;
    std::memcpy(out->working_to_display_linear,
                render_scene_.working_to_display_linear,
                sizeof(out->working_to_display_linear));
    return 0;
}

int RenderStream::unsupportedStringCopy(int unsupported_id, uint8_t kind,
                                        uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || unsupported_id < 0 || kind > 2 ||
        static_cast<size_t>(unsupported_id) >=
            render_scene_.unsupported_renderables.size()) return -1;
    const auto& record =
        render_scene_.unsupported_renderables[static_cast<size_t>(unsupported_id)];
    const std::string* value = kind == 0 ? &record.prim_path
                              : kind == 1 ? &record.type_name : &record.reason;
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap == 0 || value->empty()) return required;
    const size_t copy = std::min<size_t>(value->size(), cap - 1);
    std::memcpy(out, value->data(), copy);
    out[copy] = 0;
    return required;
}

int RenderStream::renderStats(lightusd_next_render_stats* out) const {
    if (!out || out->struct_size < sizeof(lightusd_next_render_stats)) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->source_meshes = static_cast<int32_t>(stats_.source_mesh_count);
    out->source_materials = static_cast<int32_t>(
        std::max(stats_.source_material_count, source_material_keys_.size()));
    out->source_textures = static_cast<int32_t>(
        std::max(stats_.source_texture_count, source_texture_keys_.size()));
    out->optimized_meshes = meshCount();
    out->optimized_materials = static_cast<int32_t>(materials_.size());
    out->optimized_textures = static_cast<int32_t>(texture_keys_.size());
    out->merged_meshes = static_cast<int32_t>(stats_.merged_mesh_count);
    out->merge_groups = static_cast<int32_t>(stats_.merge_group_count);
    out->skipped_merge_meshes = static_cast<int32_t>(stats_.skipped_merge_count);
    out->stage_load_ms = stats_.stage_load_ms;
    out->input_copy_ms = stats_.input_copy_ms;
    out->input_bytes = static_cast<double>(stats_.input_bytes);
    out->stage_memory_bytes = static_cast<double>(stage_.GetMemoryUsage());
    out->geometry_borrowed_bytes = static_cast<double>(stats_.geometry_borrowed_bytes);
    out->geometry_materialized_bytes = static_cast<double>(stats_.geometry_materialized_bytes);
    if (render_scene_valid_) {
      out->render_scene_memory_bytes = static_cast<double>(render_scene_.memory_usage());
    }
    return 0;
}

int RenderStream::renderStatsDetail(lightusd_next_render_stats_detail* out) const {
    static_assert(sizeof(lightusd_next_render_stats_detail) == 200,
                  "render stats detail POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_render_stats_detail))
      return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->flags = (material_dedup_ ? 1u : 0u) |
                 (mesh_merge_ ? 2u : 0u) |
                 (mesh_merge_bake_transform_ ? 4u : 0u) |
                 (flatten_render_tree_ ? 8u : 0u) |
                 (render_scene_valid_ ? 16u : 0u);
    const double timings[] = {
        stats_.composition_ms, stats_.mesh_discovery_ms, stats_.optimize_ms,
        stats_.material_ms, stats_.material_identity_ms,
        stats_.material_conversion_ms, stats_.geometry_build_ms,
        stats_.merge_append_ms};
    std::memcpy(out->timings_ms, timings, sizeof(timings));
    out->cache_counts[0] = static_cast<int32_t>(stats_.material_identity_hits);
    out->cache_counts[1] = static_cast<int32_t>(stats_.material_identity_misses);
    out->cache_counts[2] = static_cast<int32_t>(stats_.material_graph_cache_hits);
    out->cache_counts[3] = static_cast<int32_t>(stats_.material_graph_cache_misses);
    size_t provided_asset_bytes = imported_asset_bytes_;
    for (const auto& asset : clip_assets_) provided_asset_bytes += asset.second.size();
    out->memory_bytes[0] = static_cast<double>(provided_asset_bytes);
    if (render_scene_valid_) {
      size_t mesh_points_bytes = 0;
      size_t mesh_normals_bytes = 0;
      size_t mesh_uv_bytes = 0;
      size_t mesh_topology_bytes = 0;
      size_t mesh_triangulation_bytes = 0;
      for (const tr::RenderMesh& mesh : render_scene_.meshes) {
        mesh_points_bytes += mesh.points.memory_usage();
        mesh_normals_bytes += mesh.normals.memory_usage();
        mesh_uv_bytes += mesh.texcoords_0.memory_usage() +
                         mesh.texcoords_1.memory_usage();
        mesh_topology_bytes += mesh.face_vertex_counts.memory_usage() +
                               mesh.face_vertex_indices.memory_usage();
        mesh_triangulation_bytes += mesh.triangulated_indices.memory_usage() +
                                    mesh.triangulated_face_vertex_indices.memory_usage();
      }
      out->memory_bytes[1] = static_cast<double>(mesh_points_bytes);
      out->memory_bytes[2] = static_cast<double>(mesh_normals_bytes);
      out->memory_bytes[3] = static_cast<double>(mesh_uv_bytes);
      out->memory_bytes[4] = static_cast<double>(mesh_topology_bytes);
      out->memory_bytes[5] = static_cast<double>(mesh_triangulation_bytes);
      const size_t counts[] = {
          render_scene_.nodes.size(), render_scene_.meshes.size(),
          render_scene_.points.size(), render_scene_.curves.size(),
          render_scene_.point_instancers.size(),
          render_scene_.point_instance_draws.size(),
          static_cast<size_t>(materialCount()), render_scene_.textures.size(),
          render_scene_.images.size(), render_scene_.lights.size(),
          render_scene_.cameras.size(), render_scene_.animations.size(),
          render_scene_.skeletons.size(),
          render_scene_.unsupported_renderables.size(),
          render_scene_warnings_.size()};
      for (size_t i = 0; i < 15; ++i)
        out->scene_counts[i] = static_cast<int32_t>(counts[i]);
    }
    return 0;
}

int RenderStream::animationInfo(int animation_id,
                                lightusd_next_animation_info* out) const {
    if (!out || out->struct_size < sizeof(lightusd_next_animation_info) ||
        !render_scene_valid_ || animation_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->id = animation_id;
    const tr::AnimationClip& clip =
        render_scene_.animations[static_cast<size_t>(animation_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    if (clip.channels.size() > limit || clip.clip_asset_paths.size() > limit)
      return -1;
    out->start_time = clip.start_time;
    out->end_time = clip.end_time;
    out->track_count = static_cast<int32_t>(clip.channels.size());
    out->clip_asset_count = static_cast<int32_t>(clip.clip_asset_paths.size());
    int32_t flags = clip.value_clip_baked ? 4 : 0;
    for (const tr::AnimationChannel& channel : clip.channels) {
      if (channel.is_skeletal) flags |= 1;
    }
    if (!clip.channels.empty()) flags |= 2;
    out->target_node_count = AnimationTargetNodeCount(clip);
    out->flags = flags;
    return 0;
}

int RenderStream::animationClipAssetCopy(int animation_id, int asset_id,
                                         uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || asset_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size())
      return -1;
    const auto& paths = render_scene_.animations[static_cast<size_t>(animation_id)].clip_asset_paths;
    if (static_cast<size_t>(asset_id) >= paths.size()) return -1;
    const std::string& path = paths[static_cast<size_t>(asset_id)];
    if (path.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(path.size());
    if (!out || cap < path.size() || path.empty()) return required;
    std::memcpy(out, path.data(), path.size());
    return required;
}

int RenderStream::animationChannelCount(int animation_id) const {
    if (!render_scene_valid_ || animation_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    return static_cast<int>(render_scene_.animations[static_cast<size_t>(animation_id)].channels.size());
}

int RenderStream::animationChannelField(int animation_id, int channel_id,
                                        uint8_t field) const {
    if (animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    switch (field) {
      case 0: return channel.target_node;
      case 1: return channel.target_skeleton;
      case 2: return static_cast<int>(channel.keyframes.size());
      case 3: return static_cast<int>(channel.element_count);
      case 4: return static_cast<int>(channel.value_stride);
      case 5: return static_cast<int>(channel.interpolation);
      case 6: return channel.is_skeletal ? 1 : 0;
      case 7: return static_cast<int>(channel.target_path);
      case 8: return static_cast<int>(channel.joint_order.size());
      case 9: return static_cast<int>(channel.blend_shape_order.size());
      case 10: return static_cast<int>(channel.joint_remap.size());
      default: return -1;
    }
}

int RenderStream::animationChannelBufferCopy(int animation_id, int channel_id,
                                              uint8_t kind, uint8_t* out,
                                              uint32_t cap) const {
    if (animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size() || kind > 3) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    size_t count = 0;
    size_t element_size = 0;
    const void* contiguous = nullptr;
    if (kind == 0) {
      count = channel.keyframes.size();
      element_size = sizeof(double);
    } else if (kind == 1 &&
               channel.target_path == tr::AnimationChannel::TargetPath::CustomProperty &&
               !channel.array_values.empty()) {
      // Custom-property keys are compact (value_stride x element_count lanes
      // per key), matching the legacy sampler values.
      count = channel.array_values.size();
      element_size = sizeof(float);
      contiguous = channel.array_values.data();
    } else if (kind == 1) {
      count = channel.keyframes.size() * AnimationComponentCount(channel);
      element_size = sizeof(float);
    } else if (kind == 2) {
      count = channel.array_values.size();
      element_size = sizeof(float);
      contiguous = channel.array_values.data();
    } else {
      count = channel.joint_remap.size();
      element_size = sizeof(int32_t);
      contiguous = channel.joint_remap.data();
    }
    const size_t bytes = count * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (contiguous) {
      std::memcpy(out, contiguous, bytes);
    } else if (kind == 0) {
      for (size_t i = 0; i < channel.keyframes.size(); ++i)
        std::memcpy(out + i * sizeof(double), &channel.keyframes[i].time, sizeof(double));
    } else {
      const size_t components = AnimationComponentCount(channel);
      for (size_t i = 0; i < channel.keyframes.size(); ++i)
        std::memcpy(out + i * components * sizeof(float),
                    &channel.keyframes[i].value, components * sizeof(float));
    }
    return required;
}

int RenderStream::animationArrayView(int animation_id, int channel_id,
                                     lightusd_next_animation_array_view* out) const {
    if (!out || out->struct_size < sizeof(*out) || !render_scene_valid_ ||
        animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    out->ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(channel.array_values.data()));
    out->length = static_cast<uint32_t>(channel.array_values.size());
    out->comps = channel.value_stride;
    return 0;
}

int RenderStream::animationChannelStringCopy(int animation_id, int channel_id,
                                             uint8_t kind, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || channel_id < 0 || kind > 2 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    const std::string* value = kind == 0 ? &channel.target_prim_path
                              : kind == 1 ? &channel.property_name
                                          : &channel.target_skeleton_path;
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::animationChannelOrderStringCopy(int animation_id, int channel_id,
                                                  uint8_t kind, int order_id,
                                                  uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || channel_id < 0 || order_id < 0 || kind > 1 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& values = kind == 0 ? channels[static_cast<size_t>(channel_id)].joint_order
                                   : channels[static_cast<size_t>(channel_id)].blend_shape_order;
    if (static_cast<size_t>(order_id) >= values.size()) return -1;
    const std::string& value = values[static_cast<size_t>(order_id)];
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value.size());
    if (!out || cap < value.size() || value.empty()) return required;
    std::memcpy(out, value.data(), value.size());
    return required;
}

int RenderStream::provideAssetBytes(const uint8_t* name, uint32_t name_size,
                                    const uint8_t* bytes, uint32_t byte_size) {
    if ((!name && name_size) || (!bytes && byte_size)) {
      error_ = "Invalid asset buffer";
      return -1;
    }
    if (byte_size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return -1;
    }
    std::string raw_name;
    if (name_size) raw_name.assign(reinterpret_cast<const char*>(name), name_size);
    std::string key = tn::AssetResolver::NormalizePath(raw_name);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    size_t used = 0;
    used = providedAssetMemoryBytes();
    if (used == std::numeric_limits<size_t>::max()) {
      error_ = "Provided asset accounting overflow";
      return -1;
    }
    const auto previous = clip_assets_.find(key);
    if (previous != clip_assets_.end()) used -= key.size() + previous->second.size();
    if (key.size() > std::numeric_limits<size_t>::max() - byte_size ||
        used > std::numeric_limits<size_t>::max() - key.size() - byte_size) {
      error_ = "Provided asset accounting overflow";
      return -1;
    }
    const size_t next_used = used + key.size() + byte_size;
    std::shared_ptr<void> reservation;
    if (payload_budget_ && byte_size) {
      reservation = payload_budget_->Reserve(byte_size);
      if (!reservation) {
        error_ = "Shared asset payload budget exceeded";
        return -1;
      }
    }
    if (next_used > maxMemoryLimitBytes()) {
      error_ = "Provided assets exceed configured resident memory limit";
      return -1;
    }
    if (key.size() > std::numeric_limits<size_t>::max() - byte_size ||
        key.size() + byte_size > remainingMemoryLimitBytes()) {
      error_ = "Provided asset replacement exceeds remaining resident memory";
      return -1;
    }
    if (provided_asset_byte_limit_ != 0 && next_used > provided_asset_byte_limit_) {
      error_ = "Provided assets exceed configured byte limit";
      return -1;
    }
    std::string data;
    if (byte_size) data.assign(reinterpret_cast<const char*>(bytes), byte_size);
    clip_assets_[key] = std::move(data);
    if (reservation) clip_asset_budget_reservations_[key] = std::move(reservation);
    else clip_asset_budget_reservations_.erase(key);
    return 0;
}

int RenderStream::importAssetStore(const NextAssetStore& store) {
    lightusd::next::AssetResolver candidate;
    candidate.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
    size_t imported_bytes = 0;
    if (store.copyAssetsTo(&candidate, &imported_bytes) != 0) {
      error_ = "Unable to import memory asset store";
      return -1;
    }
    const size_t current = providedAssetMemoryBytes();
    if (current < imported_asset_bytes_) {
      error_ = "Imported asset accounting is inconsistent";
      return -1;
    }
    const size_t other_assets = current - imported_asset_bytes_;
    const size_t memory_limit = maxMemoryLimitBytes();
    if (other_assets > memory_limit || imported_bytes > memory_limit - other_assets ||
        (provided_asset_byte_limit_ != 0 &&
         (other_assets > provided_asset_byte_limit_ ||
          imported_bytes > provided_asset_byte_limit_ - other_assets))) {
      error_ = "Imported assets exceed configured memory limit";
      return -1;
    }
    const auto next_budget = store.payloadBudget();
    if (payload_budget_ && next_budget != payload_budget_ &&
        (!streaming_assets_.empty() || !clip_asset_budget_reservations_.empty())) {
      error_ = "Cannot change shared payload budget while local assets are retained";
      return -1;
    }
    if (!payload_budget_ || payload_budget_ != next_budget) {
      std::map<std::string, std::shared_ptr<void>> reservations;
      for (const auto& item : clip_assets_) {
        auto lease = next_budget->Reserve(item.second.size());
        if (!item.second.empty() && !lease) {
          error_ = "Shared asset payload budget exceeded by provided asset";
          return -1;
        }
        reservations.emplace(item.first, std::move(lease));
      }
      std::map<std::string, std::shared_ptr<void>> stream_reservations;
      for (const auto& item : streaming_assets_) {
        auto lease = next_budget->Reserve(item.second.expected_size);
        if (item.second.expected_size && !lease) {
          error_ = "Shared asset payload budget exceeded by active stream";
          return -1;
        }
        stream_reservations.emplace(item.first, std::move(lease));
      }
      // Commit the newly acquired reservations only after every active stream
      // fits the target store's cap.
      clip_asset_budget_reservations_ = std::move(reservations);
      for (auto& item : streaming_assets_)
        item.second.budget_reservation =
            std::move(stream_reservations[item.first]);
    }
    asset_resolver_ = std::move(candidate);
    imported_asset_bytes_ = imported_bytes;
    payload_budget_ = next_budget;
    error_.clear();
    return 0;
}

int RenderStream::beginCachedAsset(NextAssetStore& store,
                                  const uint8_t* identifier,
                                  uint32_t identifier_size) {
    if (!identifier || identifier_size == 0) {
      error_ = "Invalid cached asset identifier";
      return 0;
    }
    const uint8_t* bytes = nullptr;
    uint32_t byte_size = 0;
    const int found = store.borrowedAssetView(
        identifier, identifier_size, &bytes, &byte_size);
    if (found <= 0) {
      error_ = found == 0 ? "Asset not found in cache" :
                            "Unable to read cached asset";
      return 0;
    }
    if (byte_size == 0) {
      error_ = "Cached asset is empty";
      return 0;
    }
    if (byte_size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    if (byte_size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return 0;
    }
    if (importAssetStore(store) != 0) return 0;
    return beginBytes(bytes, byte_size);
}

int RenderStream::clearImportedAssetStore() {
    lightusd::next::AssetResolver candidate;
    candidate.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
    asset_resolver_ = std::move(candidate);
    imported_asset_bytes_ = 0;
    error_.clear();
    return 0;
}

int RenderStream::startStreamingAsset(const uint8_t* name, uint32_t name_size,
                                     uint32_t expected_size) {
    if (!name || name_size == 0 || expected_size > (uint32_t{1} << 30)) {
      error_ = "Invalid streaming asset request";
      return -1;
    }
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const size_t current = providedAssetMemoryBytes();
    if (current == std::numeric_limits<size_t>::max()) return -1;
    const auto previous = streaming_assets_.find(key);
    size_t used = current;
    if (previous != streaming_assets_.end()) {
      const size_t old_size = key.size() + previous->second.expected_size;
      if (old_size > used) return -1;
      used -= old_size;
    }
    if (key.size() > std::numeric_limits<size_t>::max() - expected_size ||
        used > std::numeric_limits<size_t>::max() - key.size() - expected_size) {
      error_ = "Streaming asset accounting overflow";
      return -1;
    }
    const size_t next = used + key.size() + expected_size;
    const size_t range_bytes = previous == streaming_assets_.end()
        ? sizeof(StreamingAsset::written_ranges) : 0;
    if (key.size() > std::numeric_limits<size_t>::max() - expected_size ||
        key.size() + expected_size >
            std::numeric_limits<size_t>::max() - range_bytes) {
      error_ = "Streaming asset accounting overflow";
      return -1;
    }
    if (next > maxMemoryLimitBytes() ||
        (provided_asset_byte_limit_ && next > provided_asset_byte_limit_) ||
        key.size() + expected_size + range_bytes > remainingMemoryLimitBytes()) {
      error_ = "Streaming asset exceeds configured memory limit";
      return -1;
    }
    StreamingAsset stream;
    if (payload_budget_ && expected_size) {
      stream.budget_reservation = payload_budget_->Reserve(expected_size);
      if (!stream.budget_reservation) {
        error_ = "Shared asset payload budget exceeded";
        return -1;
      }
    }
    stream.expected_size = expected_size;
    stream.uuid = GenerateAssetUuid();
    stream.bytes.resize(expected_size);
    streaming_assets_[key] = std::move(stream);
    return 0;
}

int RenderStream::appendStreamingAsset(const uint8_t* name, uint32_t name_size,
                                      const uint8_t* bytes, uint32_t byte_size) {
    if (!name || !name_size || (!bytes && byte_size)) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    if (byte_size) {
      const int marked = markStreamingRange_(it->second, it->second.cursor, byte_size);
      if (marked != 1) return 0;
      std::memcpy(&it->second.bytes[it->second.cursor], bytes, byte_size);
      it->second.cursor += byte_size;
    }
    return 1;
}

int RenderStream::streamingAssetBytesWritten(const uint8_t* name,
                                             uint32_t name_size) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    return it == streaming_assets_.end() ? -1 : static_cast<int>(it->second.written_size);
}

int RenderStream::streamingAssetExpectedBytes(const uint8_t* name,
                                              uint32_t name_size) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    return it == streaming_assets_.end() ? -1 : static_cast<int>(it->second.expected_size);
}

int RenderStream::streamingAssetUuidCopy(const uint8_t* name,
                                         uint32_t name_size,
                                         uint8_t* out, uint32_t cap) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.uuid.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int length = static_cast<int>(it->second.uuid.size());
    if (out && cap >= it->second.uuid.size())
      std::memcpy(out, it->second.uuid.data(), it->second.uuid.size());
    return length;
}

uintptr_t RenderStream::streamingAssetViewPtr(const uint8_t* name,
                                              uint32_t name_size,
                                              uint32_t byte_size) const {
    if (!name || !name_size || !byte_size) return 0;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    return reinterpret_cast<uintptr_t>(
        it->second.bytes.data() + it->second.cursor);
}

uintptr_t RenderStream::streamingAssetViewPtrAt(const uint8_t* name,
                                               uint32_t name_size,
                                               uint32_t offset,
                                               uint32_t byte_size) const {
    if (!name || !name_size || !byte_size) return 0;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        offset > it->second.expected_size ||
        byte_size > it->second.expected_size - offset) return 0;
    return reinterpret_cast<uintptr_t>(it->second.bytes.data() + offset);
}

int RenderStream::markStreamingRange_(StreamingAsset& stream,
                                      uint32_t offset,
                                      uint32_t byte_size) {
    if (offset > stream.expected_size ||
        byte_size > stream.expected_size - offset) return 0;
    if (!byte_size) return 1;
    uint32_t merged_start = offset;
    uint32_t merged_end = offset + byte_size;
    uint32_t overlap = 0;
    size_t first = 0;
    while (first < stream.written_range_count &&
           stream.written_ranges[first].second < merged_start) ++first;
    size_t last = first;
    while (last < stream.written_range_count &&
           stream.written_ranges[last].first <= merged_end) {
      const auto range = stream.written_ranges[last];
      const uint32_t overlap_start = std::max(offset, range.first);
      const uint32_t overlap_end = std::min(offset + byte_size, range.second);
      if (overlap_end > overlap_start) overlap += overlap_end - overlap_start;
      merged_start = std::min(merged_start, range.first);
      merged_end = std::max(merged_end, range.second);
      ++last;
    }
    if (first == last &&
        stream.written_range_count == StreamingAsset::kMaxWrittenRanges) {
      error_ = "Streaming asset has too many disjoint written ranges";
      return -2;
    }
    const size_t removed = last - first;
    const size_t new_count = stream.written_range_count - removed + 1;
    if (removed == 0) {
      for (size_t i = stream.written_range_count; i > first; --i)
        stream.written_ranges[i] = stream.written_ranges[i - 1];
    } else {
      for (size_t src = last, dst = first + 1;
           src < stream.written_range_count; ++src, ++dst)
        stream.written_ranges[dst] = stream.written_ranges[src];
    }
    stream.written_ranges[first] = {merged_start, merged_end};
    stream.written_range_count = new_count;
    stream.written_size += byte_size - overlap;
    return 1;
}

int RenderStream::markStreamingAssetBytesWritten(const uint8_t* name,
                                                 uint32_t name_size,
                                                 uint32_t byte_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    const int marked = markStreamingRange_(it->second, it->second.cursor, byte_size);
    if (marked != 1) return marked;
    it->second.cursor += byte_size;
    return 1;
}

int RenderStream::markStreamingAssetRangeWritten(const uint8_t* name,
                                                 uint32_t name_size,
                                                 uint32_t offset,
                                                 uint32_t byte_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end()) return 0;
    return markStreamingRange_(it->second, offset, byte_size);
}

int RenderStream::finalizeStreamingAsset(const uint8_t* name, uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size != it->second.expected_size) return 0;
    clip_assets_[key] = std::move(it->second.bytes);
    clip_asset_budget_reservations_[key] = std::move(it->second.budget_reservation);
    streaming_assets_.erase(it);
    return 1;
}

int RenderStream::finalizeStreamingAssetToStore(const uint8_t* name,
                                                uint32_t name_size,
                                                NextAssetStore& store) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size != it->second.expected_size) return 0;
    if (it->second.expected_size > remainingMemoryLimitBytes()) {
      error_ = "Finalizing the stream requires additional resident memory";
      return -2;
    }
    std::vector<uint8_t> bytes(it->second.bytes.begin(), it->second.bytes.end());
    std::shared_ptr<void> reservation;
    if (payload_budget_ == store.payloadBudget())
      reservation = it->second.budget_reservation;
    const int status = store.adoptStreamingAsset(key, std::move(bytes),
                                                 it->second.uuid,
                                                 std::move(reservation));
    if (status != 0) {
      error_ = status == -2 ? "Asset store memory limit exceeded"
          : status == -4 ? "Asset store metadata limit exceeded"
                         : "Unable to transfer streamed asset";
      return status;
    }
    streaming_assets_.erase(it);
    error_.clear();
    return 1;
}

int RenderStream::cancelStreamingAsset(const uint8_t* name, uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    return streaming_assets_.erase(key) ? 1 : 0;
}

int RenderStream::beginStreamingAssetAsRoot(const uint8_t* name,
                                            uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end()) {
      error_ = "Streaming root asset not found";
      return 0;
    }
    if (it->second.written_size != it->second.expected_size) {
      error_ = "Streaming root asset is incomplete";
      return 0;
    }
    if (it->second.expected_size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    std::string root = std::move(it->second.bytes);
    streaming_assets_.erase(it);
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = root.size();
    return beginOwned(std::move(root)) ? 1 : 0;
}

int RenderStream::setProvidedAssetByteLimit(uint32_t limit) {
    if (limit != 0) {
      const size_t used = providedAssetMemoryBytes();
      if (used > limit) return -1;
    }
    provided_asset_byte_limit_ = limit;
    return 0;
}

int RenderStream::removeAssetBytes(const uint8_t* name, uint32_t name_size) {
    if (!name || name_size == 0) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const bool removed = clip_assets_.erase(key) != 0;
    clip_asset_budget_reservations_.erase(key);
    return removed ? 1 : 0;
}

int RenderStream::providedAssetCount() const {
    return clip_assets_.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(clip_assets_.size());
}

int RenderStream::providedAssetNameCopy(int asset_id, uint8_t* out,
                                        uint32_t cap) const {
    if (asset_id < 0 || static_cast<size_t>(asset_id) >= clip_assets_.size()) return -1;
    auto it = clip_assets_.begin();
    std::advance(it, asset_id);
    const std::string& name = it->first;
    if (name.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(name.size());
    if (!out || cap < name.size()) return required;
    if (required) std::memcpy(out, name.data(), name.size());
    return required;
}

int RenderStream::providedAssetBytesCopy(const uint8_t* name,
                                         uint32_t name_size, uint8_t* out,
                                         uint32_t cap) const {
    if (!name && name_size) return -1;
    std::string key;
    if (name_size) key.assign(reinterpret_cast<const char*>(name), name_size);
    key = tn::AssetResolver::NormalizePath(key);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    const auto it = clip_assets_.find(key);
    if (it == clip_assets_.end() ||
        it->second.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
      return -1;
    }
    const int required = static_cast<int>(it->second.size());
    if (!out || cap < it->second.size()) return required;
    if (required) std::memcpy(out, it->second.data(), it->second.size());
    return required;
}

int RenderStream::variantSetCount() const {
    return variant_sets_.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(variant_sets_.size());
  }

int RenderStream::variantNameCount(int set_id) const {
    if (set_id < 0 || static_cast<size_t>(set_id) >= variant_sets_.size()) return -1;
    const size_t count = variant_sets_[static_cast<size_t>(set_id)].variant_names.size();
    return count > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(count);
  }

int RenderStream::variantStringCopy(int set_id, int variant_id, uint8_t kind,
                                    uint8_t* out, uint32_t cap) const {
    if (set_id < 0 || kind > 3 ||
        static_cast<size_t>(set_id) >= variant_sets_.size()) return -1;
    const VariantSetInfo& info = variant_sets_[static_cast<size_t>(set_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &info.prim_path; break;
      case 1: value = &info.set_name; break;
      case 2: value = &info.selected; break;
      case 3:
        if (variant_id < 0 ||
            static_cast<size_t>(variant_id) >= info.variant_names.size()) return -1;
        value = &info.variant_names[static_cast<size_t>(variant_id)];
        break;
    }
    if (!value || value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (out && cap >= value->size() && !value->empty())
      std::memcpy(out, value->data(), value->size());
    return required;
}

namespace {
template <typename Visitor>
bool VisitLayerAssetPaths(const lightusd::next::Stage& stage, uint8_t kind,
                          Visitor&& visitor) {
    const lightusd::next::Layer* root = stage.GetRootLayer();
    if (!root || kind > 2) return true;
    if (kind == 0) {
      for (const std::string& path : root->meta().subLayers)
        if (!visitor(path)) return false;
      return true;
    }
    for (size_t i = 0; i < root->prim_count(); ++i) {
      const lightusd::next::PrimSpec* prim =
          root->prim(static_cast<uint32_t>(i));
      if (!prim) continue;
      const auto& arcs = kind == 1 ? prim->meta().references
                                   : prim->meta().payloads;
      for (const std::string& encoded : arcs) {
        const auto arc = kind == 1
            ? lightusd::next::Compositor::ParseReference(encoded)
            : lightusd::next::Compositor::ParsePayload(encoded);
        if (kind == 1 && arc.is_internal) continue;
        if (arc.asset_path.empty()) continue;
        if (!visitor(arc.asset_path)) return false;
      }
    }
    return true;
}
}  // namespace

int RenderStream::layerAssetPathCount(uint8_t kind) const {
    if (kind > 2) return -1;
    if (!loaded_) return 0;
    const size_t count = authored_arc_paths_[kind].size();
    return count > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(count);
}

int RenderStream::layerAssetPathCopy(uint8_t kind, int path_id,
                                     uint8_t* out, uint32_t cap) const {
    if (kind > 2 || path_id < 0 || !loaded_ ||
        static_cast<size_t>(path_id) >= authored_arc_paths_[kind].size()) return -1;
    const std::string& value = authored_arc_paths_[kind][static_cast<size_t>(path_id)];
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    if (out && cap >= value.size() && !value.empty())
      std::memcpy(out, value.data(), value.size());
    return static_cast<int>(value.size());
}

int RenderStream::layerArcPresent(uint8_t kind) const {
    if (kind > 4) return -1;
    if (kind == 3) return loaded_ && authored_has_inherits_ ? 1 : 0;
    if (kind == 4) return loaded_ && authored_has_specializes_ ? 1 : 0;
    // Authored arcs, snapshotted before in-place composition consumed them.
    return loaded_ && authored_arc_present_[kind] ? 1 : 0;
}

void RenderStream::setTangentMethod(const std::string &method) {
    tangent_method_ = method;
    std::transform(tangent_method_.begin(), tangent_method_.end(),
                   tangent_method_.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
  }

bool RenderStream::beginOwned(std::string &&crate) {
    // The next-only WASM surface accepts bytes, not filesystem paths. USDC
    // lazy arrays retain this owned input buffer rather than an mmap.
    end();
    error_.clear();
    warning_.clear();
    composition_report_ = lightusd::next::pcp::CompositionReport{};
    stats_ = Stats{};
    stats_.input_copy_ms = pending_input_copy_ms_;
    stats_.input_bytes = crate.size();
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = 0;
    if (crate.size() > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return false;
    }
    const size_t memory_limit = maxMemoryLimitBytes();
    const size_t asset_bytes = providedAssetMemoryBytes();
    if (asset_bytes >= memory_limit ||
        crate.size() >= memory_limit - asset_bytes) {
      error_ = "Input and provided assets leave no resident memory for the Stage";
      return false;
    }
    const size_t load_resident_limit =
        memory_limit - asset_bytes - crate.size();
    const double stage_load_start_ms = emscripten_get_now();
    if (IsUSDCBytes(crate)) {
      lightusd::next::USDCLoadOptions opts;
      opts.crate_options.max_memory = std::min(
          opts.crate_options.max_memory,
          std::min(static_cast<size_t>(max_input_bytes_),
                   load_resident_limit));
      opts.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      lightusd::next::USDCLoadResult res =
          lightusd::next::LoadUSDCFromMemoryOwned(std::move(crate), opts);
      if (!res.success) {
        error_ = res.error_summary.empty() ? std::string("USDC load failed")
                                           : res.error_summary;
        return false;
      }
      stage_ = std::move(res.stage);
    } else {
      lightusd::next::LoadUSDOptions opts;
      opts.limits.max_input_bytes = max_input_bytes_;
      opts.limits.max_resident_bytes = load_resident_limit;
      opts.usda_options.parse_options.enable_usda_lazy_arrays = true;
      opts.usda_options.parse_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      opts.usdc_options.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      std::string warn;
      std::string err;
      const bool ok = lightusd::next::LoadUSDFromMemoryOwned(
          std::move(crate), &stage_, opts, &warn, &err);
      if (!ok) {
        error_ = err.empty() ? std::string("USD memory load failed") : err;
        return false;
      }
      warning_ = std::move(warn);
    }
    stats_.stage_load_ms = emscripten_get_now() - stage_load_start_ms;
    bool authored_has_inherits = false;
    bool authored_has_specializes = false;
    if (const lightusd::next::Layer* root = stage_.GetRootLayer()) {
      for (size_t i = 0; i < root->prim_count(); ++i) {
        const lightusd::next::PrimSpec* prim =
            root->prim(static_cast<uint32_t>(i));
        if (!prim) continue;
        authored_has_inherits = authored_has_inherits ||
                                !prim->meta().inherits.empty();
        authored_has_specializes = authored_has_specializes ||
                                   !prim->meta().specializes.empty();
        if (authored_has_inherits && authored_has_specializes) break;
      }
    }
    // Record authored variant sets (consumed by composition below), then
    // compose in place when the layer carries composition arcs — variants,
    // internal references, inherits/specializes. External arcs cannot anchor
    // for memory roots and surface as warnings (multi-layer scenes go through
    // NextFlattenSession instead).
    const double composition_start_ms = emscripten_get_now();
    collectVariantSets_();
    // In-place composition consumes the root layer's arcs; snapshot the
    // authored sublayer/reference/payload asset paths for the arc queries.
    for (uint8_t kind = 0; kind < 3; ++kind) {
      authored_arc_paths_[kind].clear();
      VisitLayerAssetPaths(stage_, kind, [&](const std::string& value) {
        authored_arc_paths_[kind].push_back(value);
        return true;
      });
    }
    authored_arc_present_[0] = [&] {
      const lightusd::next::Layer* root = stage_.GetRootLayer();
      return root && !root->meta().subLayers.empty();
    }();
    for (uint8_t kind = 1; kind < 3; ++kind) {
      authored_arc_present_[kind] = false;
      if (const lightusd::next::Layer* root = stage_.GetRootLayer()) {
        for (size_t i = 0; i < root->prim_count() && !authored_arc_present_[kind]; ++i) {
          const lightusd::next::PrimSpec* prim = root->prim(static_cast<uint32_t>(i));
          if (prim) authored_arc_present_[kind] = !(kind == 1 ? prim->meta().references
                                                            : prim->meta().payloads).empty();
        }
      }
    }
    if (enable_composition_ && lightusd::next::StageNeedsComposition(stage_)) {
      lightusd::next::pcp::CompositionOptions copts;
      copts.variant_overrides = variant_overrides_;
      std::string cwarn, cerr;
      lightusd::next::LoadUSDOptions composition_options;
      composition_options.limits.max_input_bytes = max_input_bytes_;
      composition_options.limits.max_resident_bytes = load_resident_limit;
      if (!lightusd::next::ComposeLoadedStage(
              &stage_, asset_resolver_, "memory-root", composition_options,
              &cwarn, &cerr, &copts, &composition_report_)) {
        error_ = cerr.empty() ? std::string("in-memory composition failed")
                              : cerr;
        return false;
      }
      if (!cwarn.empty()) {
        if (!warning_.empty()) warning_ += "\n";
        warning_ += cwarn;
      }
    }
    if (stage_.GetMemoryUsage() >= load_resident_limit) {
      error_ = "Stage exceeds configured resident memory limit";
      stage_ = lightusd::next::Stage();
      return false;
    }
    stats_.composition_ms = emscripten_get_now() - composition_start_ms;
    const double mesh_discovery_start_ms = emscripten_get_now();
    meshes_ = lightusd::next::GetAllMeshes(stage_);
    tangent_requests_.assign(meshes_.size(), 0);
    stats_.mesh_discovery_ms =
        emscripten_get_now() - mesh_discovery_start_ms;
    stats_.source_mesh_count = meshes_.size();
    // GetAllMeshes() exposes composed instance children whose GetParent()
    // chain does not include the instance root. Seed transform caches from
    // the render traversal before mesh-only optimization so baking sees the
    // same composed world transforms as the node hierarchy. A full
    // RenderScene is intentionally not built for mesh-only workers.
    buildMeshTransformCaches_();
    if (!mesh_only_) {
      // Public material ids index materials_. Register every bound material
      // in mesh order now so ids are deterministic (like legacy) instead of
      // depending on which mesh a caller views first. Mesh-only workers
      // convert only the materials of the meshes they are asked for.
      // The fallback record for unbound meshes is registered last so it
      // never shifts the ids of bound materials.
      const double material_start_ms = emscripten_get_now();
      bool has_unbound_mesh = false;
      for (const auto& mesh : meshes_) {
        const lightusd::next::UsdPrim& prim = mesh.GetPrim();
        const lightusd::next::UsdPrim bound = lightusd::next::GetBoundMaterial(stage_, prim);
        if (bound.IsValid()) (void)registerMaterial_(bound);
        else has_unbound_mesh = true;
        for (const lightusd::next::UsdPrim& child : prim.GetChildren()) {
          if (!child.IsValid() || child.GetTypeName() != "GeomSubset") continue;
          const lightusd::next::UsdPrim subset_bound =
              lightusd::next::GetBoundMaterial(stage_, child);
          if (subset_bound.IsValid()) (void)registerMaterial_(subset_bound);
        }
      }
      if (has_unbound_mesh) (void)registerMaterial_(lightusd::next::UsdPrim());
      stats_.material_ms += emscripten_get_now() - material_start_ms;
    }
    if (mesh_merge_) {
      const double optimize_start_ms = emscripten_get_now();
      buildOptimizedOutputs_();
      stats_.optimize_ms = emscripten_get_now() - optimize_start_ms;
    }
    if (!mesh_only_) {
      buildRenderScene_();
      source_primvars_.clear();
      buildAnalyticOutputs_();
      remapNodeMeshIds_();
    }
    loaded_ = true;
    authored_has_inherits_ = authored_has_inherits;
    authored_has_specializes_ = authored_has_specializes;
    return true;
}

int RenderStream::preflightBeginInput(uint32_t size) {
    if (size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    if (size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return 0;
    }
    const size_t memory_limit = maxMemoryLimitBytes();
    const size_t asset_bytes = providedAssetMemoryBytes();
    if (asset_bytes >= memory_limit ||
        size >= memory_limit - asset_bytes) {
      error_ = "Input and provided assets leave no resident memory for the Stage";
      return 0;
    }
    if (size > remainingMemoryLimitBytes()) {
      error_ = "Input exceeds remaining resident memory allowance";
      return 0;
    }
    error_.clear();
    return 1;
}

int RenderStream::beginBytes(const uint8_t* bytes, uint32_t size) {
    const double input_copy_start_ms = emscripten_get_now();
    if (!bytes && size) {
      error_ = "Invalid input buffer";
      return 0;
    }
    if (!preflightBeginInput(size)) return 0;
    std::string s;
    if (size) s.assign(reinterpret_cast<const char*>(bytes), size);
    pending_input_copy_ms_ = emscripten_get_now() - input_copy_start_ms;
    pending_input_bytes_ = size;
    return beginOwned(std::move(s)) ? 1 : 0;
}

int RenderStream::beginFromLayerDocument(NextLayerDocument& document) {
    const int32_t size = document.exportUsdaSize();
    if (size < 0) {
      error_ = "Layer document could not export USDA";
      return 0;
    }
    const uintptr_t data = document.exportUsdaData();
    if (!data && size) {
      error_ = "Layer document USDA output is missing";
      return 0;
    }
    return beginBytes(reinterpret_cast<const uint8_t*>(data),
                      static_cast<uint32_t>(size));
}

int RenderStream::setMaxInputBytes(uint32_t limit) {
    if (limit == 0 || limit > (uint32_t{1} << 30)) return -1;
    max_input_bytes_ = limit;
    return 0;
}

int RenderStream::setMaxMemoryLimitMB(int32_t limit_mb) {
    if (limit_mb < 1 ||
        static_cast<uint64_t>(limit_mb) >
            (static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) >> 20)) {
      return -1;
    }
    const size_t new_limit = static_cast<size_t>(limit_mb) * (size_t{1} << 20);
    const size_t current_limit = maxMemoryLimitBytes();
    const size_t remaining = remainingMemoryLimitBytes();
    const size_t used = remaining >= current_limit ? 0 : current_limit - remaining;
    if (used >= new_limit) return -1;
    if (loaded_) {
      const size_t input_bytes = stats_.input_bytes;
      const size_t stage_bytes = stage_.GetMemoryUsage();
      if (input_bytes >= new_limit - used ||
          stage_bytes >= new_limit - used - input_bytes) return -1;
    }
    max_memory_limit_mb_ = limit_mb;
    return 0;
}

size_t RenderStream::providedAssetMemoryBytes() const {
    size_t used = imported_asset_bytes_;
    for (const auto& asset : clip_assets_) {
      if (asset.first.size() > std::numeric_limits<size_t>::max() - asset.second.size() ||
          used > std::numeric_limits<size_t>::max() - asset.first.size() - asset.second.size()) {
        return std::numeric_limits<size_t>::max();
      }
      used += asset.first.size() + asset.second.size();
    }
    for (const auto& asset : streaming_assets_) {
      if (asset.first.size() > std::numeric_limits<size_t>::max() - asset.second.expected_size ||
          used > std::numeric_limits<size_t>::max() - asset.first.size() - asset.second.expected_size) {
        return std::numeric_limits<size_t>::max();
      }
      used += asset.first.size() + asset.second.expected_size;
    }
    return used;
}

size_t RenderStream::streamingRangeBookkeepingBytes_() const {
    const size_t unit = sizeof(StreamingAsset::written_ranges);
    if (streaming_assets_.size() > std::numeric_limits<size_t>::max() / unit)
      return std::numeric_limits<size_t>::max();
    return streaming_assets_.size() * unit;
}

size_t RenderStream::remainingMemoryLimitBytes() const {
    const size_t limit = maxMemoryLimitBytes();
    size_t used = providedAssetMemoryBytes();
    if (stats_.input_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += stats_.input_bytes;
    const size_t stage_bytes = stage_.GetMemoryUsage();
    if (stage_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += stage_bytes;
    if (formatted_material_text_.size() >
        std::numeric_limits<size_t>::max() - used) return 0;
    used += formatted_material_text_.size();
    if (stage_usdc_export_.size() > std::numeric_limits<size_t>::max() - used) return 0;
    used += stage_usdc_export_.size();
    const size_t range_bytes = streamingRangeBookkeepingBytes_();
    if (range_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += range_bytes;
    if (used >= limit) return 0;
    return limit - used;
}

int RenderStream::setSphereSubdivisions(int value) {
    if (value < 0 || value > 6) return -1;
    sphere_subdivisions_ = value;
    return 0;
}

int RenderStream::setTargetBoneCount(uint32_t value) {
    if (value == 0 || value > 128) return -1;
    target_bone_count_ = value;
    return 0;
}

int RenderStream::setValueClipSetting(uint8_t field, double value) {
    if (!std::isfinite(value)) return -1;
    switch (field) {
      case 0:
        if (value < 0.0 || value > 10000.0) return -1;
        value_clip_sample_rate_ = static_cast<float>(value);
        return 0;
      case 1:
        if (value != 0.0 && value != 1.0) return -1;
        value_clip_use_time_range_ = value != 0.0;
        return 0;
      case 2: value_clip_start_time_ = value; return 0;
      case 3: value_clip_end_time_ = value; return 0;
      default: return -1;
    }
}

double RenderStream::valueClipSetting(uint8_t field) const {
    switch (field) {
      case 0: return value_clip_sample_rate_;
      case 1: return value_clip_use_time_range_ ? 1.0 : 0.0;
      case 2: return value_clip_start_time_;
      case 3: return value_clip_end_time_;
      default: return std::numeric_limits<double>::quiet_NaN();
    }
}

void RenderStream::end() {
    loaded_ = false;
    warning_.clear();
    composition_report_ = lightusd::next::pcp::CompositionReport{};
    authored_has_inherits_ = false;
    authored_has_specializes_ = false;
    for (auto& paths : authored_arc_paths_) paths.clear();
    for (bool& present : authored_arc_present_) present = false;
    source_primvars_.clear();
    mesh_view_error_.clear();
    mesh_view_source_id_ = -1;
    render_scene_valid_ = false;
    formatted_material_id_ = -1;
    formatted_material_format_ = 0xff;
    formatted_material_status_ = 0;
    formatted_material_text_.clear();
    render_scene_ = tr::RenderScene();
    render_scene_warnings_.clear();
    stage_usdc_export_.clear();
    meshes_.clear();
    meshes_.shrink_to_fit();
    tangent_requests_.clear();
    tangent_requests_.shrink_to_fit();
    outputs_.clear();
    outputs_.shrink_to_fit();
    analytic_outputs_.clear();
    analytic_outputs_.shrink_to_fit();
    materials_.clear();
    material_key_to_id_.clear();
    material_path_to_id_.clear();
    material_identity_to_id_.clear();
    local_matrix_cache_.clear();
    world_matrix_cache_.clear();
    source_material_keys_.clear();
    source_texture_keys_.clear();
    texture_keys_.clear();
    stage_ = lightusd::next::Stage();
    freeVec_(s_points_);
    freeVec_(s_normals_);
    freeVec_(s_uv_);
    freeVec_(s_tangents_);
    freeVec_(s_indices_);
    freeVec_(s_joint_indices_);
    freeVec_(s_joint_weights_);
    freeVec_(s_point_source_indices_);
  }

void RenderStream::reset() {
    end();
    clearVariantOverrides();
    variant_sets_.clear();
    clip_assets_.clear();
    clip_asset_budget_reservations_.clear();
    streaming_assets_.clear();
    asset_resolver_.ClearMemoryAssets();
    imported_asset_bytes_ = 0;
    payload_budget_.reset();
    stats_ = Stats{};
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = 0;
    formatted_light_id_ = -1;
    formatted_light_format_ = 0xff;
    formatted_light_status_ = 0;
    formatted_light_cached_ = false;
    formatted_light_text_.clear();
    error_.clear();
    warning_.clear();
  }

int RenderStream::exportStageUSDC() {
    stage_usdc_export_.clear();
    if (!loaded_) {
      error_ = "No stage loaded";
      return 0;
    }
    const size_t budget = remainingMemoryLimitBytes();
    if (!budget) {
      error_ = "USDC export exceeds remaining resident memory limit";
      return 0;
    }
    lightusd::next::USDCWriteOptions options;
    options.crate_options.max_file_size_bytes = budget;
    options.crate_options.max_memory_bytes = budget;
    const lightusd::next::USDCWriteResult result =
        lightusd::next::WriteUSDCToMemory(stage_usdc_export_, stage_, options);
    if (!result.success) {
      stage_usdc_export_.clear();
      error_ = result.error.empty() ? "USDC export failed" : result.error;
      return 0;
    }
    error_.clear();
    return 1;
}

int RenderStream::stageUSDCSize() const {
    return stage_usdc_export_.size() <=
            static_cast<size_t>(std::numeric_limits<int>::max())
        ? static_cast<int>(stage_usdc_export_.size()) : -1;
}

uintptr_t RenderStream::stageUSDCData() const {
    return stage_usdc_export_.empty()
        ? uintptr_t{0}
        : reinterpret_cast<uintptr_t>(stage_usdc_export_.data());
}

void RenderStream::buildRenderScene_() {
    tr::ConverterConfig cfg;
    cfg.time_code = 0.0;
    cfg.limits.max_resident_bytes = remainingMemoryLimitBytes();
    cfg.mesh.compute_tangents = compute_tangents_;
    cfg.mesh.sphere_subdivisions = sphere_subdivisions_;
    cfg.mesh.tangent_method = tangentMethod_();
    cfg.mesh.enable_bone_reduction = enable_bone_reduction_;
    cfg.mesh.target_bone_count = target_bone_count_;
    cfg.mesh.round_bone_count = round_bone_count_;
    // RenderStream builds one browser-facing mesh lazily from Stage. Keeping a
    // second, complete geometry copy in RenderScene only inflates the wasm
    // heap; retain its material/skinning/animation metadata instead. Keep the
    // compact earcut result used by the lazy mesh builder, plus generated
    // analytic geometry which has no authored Mesh payload to rebuild from.
    cfg.mesh.retain_geometry = false;
    cfg.mesh.retain_custom_primvars = true;
    cfg.mesh.retain_triangulation = true;
    cfg.mesh.retain_analytic_geometry = true;
    cfg.material.load_textures = load_texture_in_native_;
    cfg.material.combine_udim_tiles = combine_udim_tiles_;
    cfg.material.allow_missing_textures = true;
    size_t texture_reservation_bytes = 0;
    if (load_texture_in_native_) {
      cfg.material.custom_texture_loader = [this, &texture_reservation_bytes](
          const std::string& asset_path, tr::TextureImage* out) {
        if (!out || asset_path.empty()) return false;
        std::shared_ptr<const std::vector<uint8_t>> memory_view;
        const auto resolved = asset_resolver_.Resolve(asset_path, "memory-root");
        if (resolved.exists && !resolved.resolved_path.empty()) {
          memory_view = asset_resolver_.GetMemoryAssetView(resolved.resolved_path);
        }
        if (!memory_view) memory_view = asset_resolver_.GetMemoryAssetView(asset_path);
        if (!memory_view) return false;
        const uint8_t* encoded = memory_view->data();
        const size_t encoded_size = memory_view->size();
        if (!encoded || encoded_size == 0) return false;
        const auto info = lightusd::image::GetImageInfoFromMemory(
            encoded, encoded_size, asset_path);
        if (!info) return false;
        size_t pixels = 0;
        size_t estimate = 0;
        if (!lightusd::safe::mul(static_cast<size_t>(info.value().width),
                                 static_cast<size_t>(info.value().height),
                                 &pixels) ||
            !lightusd::safe::mul(pixels, size_t{16}, &estimate)) return false;
        const size_t remaining = remainingMemoryLimitBytes();
        if (texture_reservation_bytes > remaining ||
            estimate > remaining - texture_reservation_bytes) return false;
        auto decoded = lightusd::image::LoadImageFromMemory(
            encoded, encoded_size, asset_path);
        if (!decoded || decoded.value().image.width <= 0 ||
            decoded.value().image.height <= 0 ||
            decoded.value().image.channels <= 0 ||
            decoded.value().image.data.empty()) return false;
        const auto& image = decoded.value().image;
        if (image.data.size() > remaining - texture_reservation_bytes) return false;
        texture_reservation_bytes += estimate;
        out->width = static_cast<uint32_t>(image.width);
        out->height = static_cast<uint32_t>(image.height);
        out->channels = static_cast<uint8_t>(image.channels);
        if (image.format == lightusd::Image::PixelFormat::Float) {
          out->component_type = image.bpp == 16 ? tr::ComponentType::Float16
              : image.bpp == 64 ? tr::ComponentType::Float64
                                : tr::ComponentType::Float32;
        } else if (image.format == lightusd::Image::PixelFormat::Int) {
          out->component_type = image.bpp == 16 ? tr::ComponentType::Int16
              : image.bpp == 32 ? tr::ComponentType::Int32
                                : tr::ComponentType::Int8;
        } else {
          out->component_type = image.bpp == 16 ? tr::ComponentType::UInt16
              : image.bpp == 32 ? tr::ComponentType::UInt32
                                : tr::ComponentType::UInt8;
        }
        if (!out->data.append(image.data.data(), image.data.size())) return false;
        return true;
      };
    }
    cfg.material.render_settings_path = render_settings_path_;
    cfg.asset_resolver = &asset_resolver_;
    cfg.point_instancer.duplicate_meshes = false;
    if (enable_value_clips_) cfg.animation.clip_stage_loader =
        [this](const std::string& asset_path, tn::Stage* stage,
               std::string* warn, std::string* err) {
          std::string key = tn::AssetResolver::NormalizePath(asset_path);
          while (key.rfind("./", 0) == 0) key.erase(0, 2);
          auto it = clip_assets_.find(key);
          if (it == clip_assets_.end()) {
            for (const std::string& candidate :
                 tn::AssetResolver::SuffixCandidates(key)) {
              it = clip_assets_.find(candidate);
              if (it != clip_assets_.end()) break;
            }
          }
          if (it == clip_assets_.end()) {
            const tn::ResolvedAsset resolved =
                asset_resolver_.Resolve(key, "memory-root");
            std::vector<uint8_t> bytes;
            std::string read_error;
            if (!resolved.exists || !asset_resolver_.ReadAsset(
                    resolved.resolved_path, &bytes, &read_error)) {
              if (err) *err = "asset was not supplied to RenderStream";
              return false;
            }
            tn::LoadUSDOptions options;
            options.usda_options.parse_options.enable_usda_lazy_arrays = true;
            std::string owned;
            if (!bytes.empty())
              owned.assign(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
            return tn::LoadUSDFromMemoryOwned(std::move(owned), stage, options,
                                              warn, err);
          }
          tn::LoadUSDOptions options;
          options.usda_options.parse_options.enable_usda_lazy_arrays = true;
          std::string bytes = it->second;
          return tn::LoadUSDFromMemoryOwned(std::move(bytes), stage, options,
                                            warn, err);
        };
    cfg.animation.bake_value_clips = enable_value_clips_;
    cfg.animation.value_clip_sample_rate = value_clip_sample_rate_;
    cfg.animation.value_clip_use_time_range = value_clip_use_time_range_;
    cfg.animation.value_clip_start_time = value_clip_start_time_;
    cfg.animation.value_clip_end_time = value_clip_end_time_;
    tr::RenderSceneConverter converter(cfg);
    tr::ConvertResult result = converter.Convert(stage_);
    render_scene_ = std::move(result.scene);
    render_scene_warnings_ = result.warnings;
    render_scene_valid_ = result.success;
    if (!result.success && !result.error.empty()) {
      error_ = result.error;
    }
    if (render_scene_valid_ && flatten_render_tree_) flattenRenderTree_();
  }

// Legacy setNativeFlattenRenderTree: replace the hierarchy with one
// "OptimizedRenderRoot" whose direct children are the renderable nodes (in
// depth-first order) carrying their world transform as local transform.
// Node ids stay stable; bypassed Xform nodes remain addressable by id but are
// no longer reachable from the root, and animation channels targeting them
// are detached as in legacy.
void RenderStream::flattenRenderTree_() {
    auto& nodes = render_scene_.nodes;
    const auto keep = [](const tr::SceneNode& node) {
      return node.data_id >= 0 && node.type != tr::NodeType::Xform;
    };
    std::vector<int32_t> kept;
    std::vector<uint8_t> is_kept(nodes.size(), 0);
    std::vector<std::pair<int32_t, size_t>> stack;
    for (auto it = render_scene_.root_nodes.rbegin();
         it != render_scene_.root_nodes.rend(); ++it) stack.emplace_back(*it, 0);
    constexpr size_t kMaxDepth = 1u << 16;
    while (!stack.empty()) {
      const auto [id, depth] = stack.back();
      stack.pop_back();
      if (id < 0 || static_cast<size_t>(id) >= nodes.size() || depth >= kMaxDepth) continue;
      const tr::SceneNode& node = nodes[static_cast<size_t>(id)];
      if (keep(node) && !is_kept[static_cast<size_t>(id)]) {
        is_kept[static_cast<size_t>(id)] = 1;
        kept.push_back(id);
      }
      for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        stack.emplace_back(*child, depth + 1);
    }
    const int32_t root_id = static_cast<int32_t>(nodes.size());
    for (const int32_t id : kept) {
      tr::SceneNode& node = nodes[static_cast<size_t>(id)];
      node.local_transform = node.world_transform;
      node.children.clear();
      node.parent_id = root_id;
    }
    tr::SceneNode flat_root;
    flat_root.name = "OptimizedRenderRoot";
    flat_root.prim_path = "/OptimizedRenderRoot";
    flat_root.type = tr::NodeType::Xform;
    flat_root.children = std::move(kept);
    nodes.push_back(std::move(flat_root));
    render_scene_.node_by_path["/OptimizedRenderRoot"] = root_id;
    render_scene_.root_nodes.assign(1, root_id);
    for (tr::AnimationClip& clip : render_scene_.animations) {
      for (tr::AnimationChannel& channel : clip.channels) {
        if (channel.target_node >= 0 &&
            (static_cast<size_t>(channel.target_node) >= is_kept.size() ||
             !is_kept[static_cast<size_t>(channel.target_node)]))
          channel.target_node = -1;
      }
    }
  }

void RenderStream::collectVariantSets_() {
    variant_sets_.clear();
    const lightusd::next::Layer *root = stage_.GetRootLayer();
    if (!root) return;
    for (const auto &prim : root->prims()) {
      const auto &meta = prim.meta();
      for (const auto &vs : meta.variantSets()) {
        VariantSetInfo info;
        info.prim_path = prim.path().str();
        info.set_name = vs.name;
        // Authored selection lives in the prim's `variants = {...}` metadata
        // (variantSelections / legacy single variantSelection), not on the
        // VariantSetData itself.
        info.selected = vs.selected;
        for (const auto &sel : meta.variantSelections()) {
          if (sel.first == vs.name) {
            info.selected = sel.second;
            break;
          }
        }
        if (info.selected.empty() && !meta.variantSelection.empty()) {
          const std::string &legacy = meta.variantSelection;
          const size_t eq = legacy.find('=');
          if (eq != std::string::npos && legacy.substr(0, eq) == vs.name) {
            info.selected = legacy.substr(eq + 1);
          }
        }
        for (const auto &variant : vs.variants) {
          info.variant_names.push_back(variant.name);
        }
        variant_sets_.push_back(std::move(info));
      }
    }
  }
}  // namespace web_next
}  // namespace lightusd
