// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-core-render.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lightusd {
namespace web_core {
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
    if (field == 11) {
      if (output) return output->purpose;
      return source_index >= 0 &&
                     static_cast<size_t>(source_index) < meshes_.size()
                 ? purposeCode_(meshes_[static_cast<size_t>(source_index)].GetPrim())
                 : -1;
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
      built_source = prepareCopyMesh_(mesh_id, source_index, &soup, &mesh_error);
      if (!built_source) mesh_view_error_ = mesh_error;
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
      built_source = prepareCopyMesh_(mesh_id, source_index, &soup, &mesh_error);
      if (!built_source) mesh_view_error_ = mesh_error;
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
      if (!chunked->copy_bytes_to(out, cap)) return -1;
    } else if (indices) {
      if (!indices->copy_bytes_to(out, cap)) return -1;
    } else {
      if (!joint_indices->copy_bytes_to(out, cap)) return -1;
    }
    return required;
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
    if (!values->copy_bytes_to(out, cap)) return -1;
    return required;
}

int RenderStream::pointsInfo(int points_id, lightusd_wasm_points_info* out) const {
    static_assert(sizeof(lightusd_wasm_points_info) == 40,
                  "points info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_wasm_points_info) ||
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
    } else {
      if (!values->copy_bytes_to(out, cap)) return -1;
    }
    return required;
}

int RenderStream::curvesInfo(int curves_id, lightusd_wasm_curves_info* out) const {
    static_assert(sizeof(lightusd_wasm_curves_info) == 48,
                  "curves info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_wasm_curves_info) ||
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
                                lightusd_wasm_instancer_info* out) const {
    static_assert(sizeof(lightusd_wasm_instancer_info) == 48,
                  "instancer info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_wasm_instancer_info) ||
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
    return CopyOutputString(value, out, cap);
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

}  // namespace web_core
}  // namespace lightusd
