// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "next/schema/usd-shade.hh"
namespace lightusd {
namespace web_next {
void RenderStream::buildAnalyticOutputs_() {
    analytic_outputs_.clear();
    if (!render_scene_valid_) return;
    for (const tr::RenderMesh& source : render_scene_.meshes) {
      const lightusd::next::UsdPrim prim =
          stage_.GetPrimAtPath(source.prim_path);
      if (!prim.IsValid() || prim.GetTypeName() == "Mesh") continue;

      OutputMesh out;
      out.name = source.name;
      out.prim_path = source.prim_path;
      out.double_sided = matBool_(prim, "doubleSided", false);
      out.local_matrix = localMatrix_(prim);
      out.world_matrix = worldMatrixForPrim_(prim);

      lightusd::next::UsdPrim material;
      if (source.material_id >= 0 &&
          static_cast<size_t>(source.material_id) <
              render_scene_.materials.size()) {
        material = stage_.GetPrimAtPath(
            render_scene_.materials[static_cast<size_t>(source.material_id)]
                .prim_path);
      }
      if (!material.IsValid()) {
        material = lightusd::next::GetBoundMaterial(stage_, prim);
      }
      out.material_id = registerMaterial_(material);

      const size_t point_count = source.points.size() / 3;
      const bool vertex_normals =
          source.normals.empty() ||
          (source.normals_interp == tr::Interpolation::Vertex &&
           source.normals.size() == point_count * 3);
      const bool vertex_uvs =
          source.texcoords_0.empty() ||
          (source.texcoords_0_interp == tr::Interpolation::Vertex &&
           source.texcoords_0.size() == point_count * 2);

      if (vertex_normals && vertex_uvs) {
        copyChunked_(source.points, &out.points);
        copyChunked_(source.normals, &out.normals);
        copyChunked_(source.texcoords_0, &out.uv);
        out.indices.resize(source.triangulated_indices.size());
        for (size_t i = 0; i < source.triangulated_indices.size(); ++i) {
          out.indices[i] = source.triangulated_indices[i];
        }
      } else {
        // Face-varying analytic attributes need one vertex per triangulated
        // corner. Preserve the converter's authored-corner remap so generated
        // sphere/cone UV seams and normals stay aligned.
        out.soup = true;
        const size_t corners = source.triangulated_indices.size();
        out.points.reserve(corners * 3);
        if (!source.normals.empty()) out.normals.reserve(corners * 3);
        if (!source.texcoords_0.empty()) out.uv.reserve(corners * 2);
        for (size_t corner = 0; corner < corners; ++corner) {
          const uint32_t vertex = source.triangulated_indices[corner];
          if (vertex >= point_count) continue;
          for (size_t c = 0; c < 3; ++c) {
            out.points.push_back(source.points[static_cast<size_t>(vertex) * 3 + c]);
          }
          const size_t authored_corner =
              corner < source.triangulated_face_vertex_indices.size()
                  ? source.triangulated_face_vertex_indices[corner]
                  : corner;
          if (!source.normals.empty()) {
            const size_t normal_element =
                source.normals_interp == tr::Interpolation::FaceVarying
                    ? authored_corner
                    : static_cast<size_t>(vertex);
            if (normal_element * 3 + 2 < source.normals.size()) {
              for (size_t c = 0; c < 3; ++c) {
                out.normals.push_back(source.normals[normal_element * 3 + c]);
              }
            }
          }
          if (!source.texcoords_0.empty()) {
            const size_t uv_element =
                source.texcoords_0_interp == tr::Interpolation::FaceVarying
                    ? authored_corner
                    : static_cast<size_t>(vertex);
            if (uv_element * 2 + 1 < source.texcoords_0.size()) {
              out.uv.push_back(source.texcoords_0[uv_element * 2]);
              out.uv.push_back(source.texcoords_0[uv_element * 2 + 1]);
            }
          }
        }
        if (out.normals.size() != out.points.size()) out.normals.clear();
        if (out.uv.size() * 3 != out.points.size() * 2) out.uv.clear();
      }
      if (!out.points.empty()) analytic_outputs_.push_back(std::move(out));
    }
  }

// Tydra node data ids index render_scene_.meshes (traversal order), but the
// stream publishes authored Mesh outputs first and analytic gprims after
// them. Rewrite mesh node ids into that output index space so getNode()
// dataId addresses getMesh(). Sources folded into a merged output have no
// output of their own and become -1.
void RenderStream::remapNodeMeshIds_() {
    if (!render_scene_valid_) return;
    std::map<std::string, int32_t> output_by_path;
    const size_t authored = mesh_merge_ ? outputs_.size() : meshes_.size();
    for (size_t i = 0; i < authored; ++i) {
      int source_index = static_cast<int>(i);
      if (mesh_merge_) {
        if (outputs_[i].merged) continue;
        source_index = outputs_[i].source_index;
      }
      if (source_index < 0 ||
          static_cast<size_t>(source_index) >= meshes_.size()) continue;
      output_by_path.emplace(
          meshes_[static_cast<size_t>(source_index)].GetPrim().GetPath().str(),
          static_cast<int32_t>(i));
    }
    for (size_t i = 0; i < analytic_outputs_.size(); ++i) {
      output_by_path.emplace(analytic_outputs_[i].prim_path,
                             static_cast<int32_t>(authored + i));
    }
    for (tr::SceneNode& node : render_scene_.nodes) {
      if (node.type != tr::NodeType::Mesh || node.data_id < 0) continue;
      const size_t id = static_cast<size_t>(node.data_id);
      const auto it = id < render_scene_.meshes.size()
          ? output_by_path.find(render_scene_.meshes[id].prim_path)
          : output_by_path.end();
      node.data_id = it == output_by_path.end() ? -1 : it->second;
    }
  }

int RenderStream::meshView(int mesh_id, lightusd_next_mesh_view* out) {
    mesh_view_error_.clear();
    mesh_view_source_id_ = -1;
    if (!out || out->struct_size < sizeof(*out) || !loaded_ ||
        mesh_id < 0 || mesh_id >= meshCount()) return -1;
    const OutputMesh* output = nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size()) {
        const OutputMesh& record = outputs_[static_cast<size_t>(mesh_id)];
        if (record.merged) output = &record;
        else source_index = record.source_index;
      } else {
        output = &analytic_outputs_[static_cast<size_t>(mesh_id) - outputs_.size()];
      }
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    } else {
      output = &analytic_outputs_[static_cast<size_t>(mesh_id) - meshes_.size()];
    }

    const uint32_t requested_size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = requested_size;
    out->material_id = -1;
    out->skeleton_id = -1;
    auto set_buffer = [out](size_t slot, const void* data, size_t count) -> bool {
      if (count > (std::numeric_limits<uint32_t>::max)()) return false;
      out->ptr[slot] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
      out->length[slot] = static_cast<uint32_t>(count);
      return true;
    };

    if (output) {
      out->material_id = output->material_id;
      if (output->double_sided) out->flags |= 1u;
      std::memcpy(out->local_matrix, output->local_matrix.data(),
                  sizeof(out->local_matrix));
      std::memcpy(out->world_matrix, output->world_matrix.data(),
                  sizeof(out->world_matrix));
      if (!set_buffer(0, output->points.data(), output->points.size()) ||
          !set_buffer(1, output->soup ? nullptr : output->indices.data(),
                      output->soup ? 0 : output->indices.size()) ||
          !set_buffer(2, output->normals.data(), output->normals.size()) ||
          !set_buffer(3, output->uv.data(), output->uv.size())) return -1;
      return 0;
    }

    if (source_index < 0 || static_cast<size_t>(source_index) >= meshes_.size()) return -1;
    const lightusd::next::UsdPrim& prim = meshes_[static_cast<size_t>(source_index)].GetPrim();
    bool soup = false;
    std::string mesh_error;
    s_tangents_.clear();
    if (!buildRenderMesh_(prim, &soup, &mesh_error)) {
      mesh_view_error_ = mesh_error.empty() ? "mesh build failed" : mesh_error;
      return -2;
    }
    out->material_id = materialIdForBoundPrim_(prim);
    if (effectiveDoubleSided_(prim, out->material_id, s_points_)) out->flags |= 1u;
    const std::array<double, 16> local = localMatrix_(prim);
    const std::array<double, 16> world = worldMatrixForPrim_(prim);
    std::memcpy(out->local_matrix, local.data(), sizeof(out->local_matrix));
    std::memcpy(out->world_matrix, world.data(), sizeof(out->world_matrix));
    if (wantsTangents_(source_index)) computeScratchTangents_();
    const tr::RenderMesh* source = sourceRenderMesh_(mesh_id);
    prepareScratchSkin_(source);
    if (source && source->skin) {
      out->flags |= 2u;
      out->skeleton_id = source->skin->skeleton_id;
      out->element_size = static_cast<int32_t>(source->skin->influences_per_vertex);
      const std::array<double, 16> bind =
          MatrixToArray(source->skin->geom_bind_transform);
      std::memcpy(out->geom_bind_matrix, bind.data(), sizeof(out->geom_bind_matrix));
    }
    if (!set_buffer(0, s_points_.data(), s_points_.size()) ||
        !set_buffer(1, soup ? nullptr : s_indices_.data(),
                    soup ? 0 : s_indices_.size()) ||
        !set_buffer(2, s_normals_.data(), s_normals_.size()) ||
        !set_buffer(3, s_uv_.data(), s_uv_.size()) ||
        !set_buffer(4, s_tangents_.data(), s_tangents_.size()) ||
        !set_buffer(5, s_joint_indices_.data(), s_joint_indices_.size()) ||
        !set_buffer(6, s_joint_weights_.data(), s_joint_weights_.size())) return -1;
    mesh_view_source_id_ = mesh_id;
    return 0;
}

int RenderStream::meshViewStringCopy(int mesh_id, uint8_t kind, uint8_t* out,
                                     uint32_t cap) const {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount() || kind > 4) return -1;
    const OutputMesh* output = nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size()) {
        const OutputMesh& record = outputs_[static_cast<size_t>(mesh_id)];
        if (record.merged) output = &record;
        else source_index = record.source_index;
      } else {
        output = &analytic_outputs_[static_cast<size_t>(mesh_id) - outputs_.size()];
      }
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    } else {
      output = &analytic_outputs_[static_cast<size_t>(mesh_id) - meshes_.size()];
    }
    std::string value;
    if (kind == 4) {
      value = mesh_view_error_;
    } else if (output) {
      if (kind == 0) value = output->name;
      if (kind == 1) value = output->prim_path;
    } else if (source_index >= 0 &&
               static_cast<size_t>(source_index) < meshes_.size()) {
      const lightusd::next::UsdPrim& prim =
          meshes_[static_cast<size_t>(source_index)].GetPrim();
      if (kind == 0) value = prim.GetName();
      if (kind == 1) value = prim.GetPath().str();
      if (kind == 2) {
        const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
        if (mesh && mesh->skin) value = mesh->skin->skeleton_path;
      }
      if (kind == 3 && wantsTangents_(source_index)) value = tangent_method_;
    } else {
      return -1;
    }
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(value.size());
    if (out && cap >= value.size() && !value.empty())
      std::memcpy(out, value.data(), value.size());
    return required;
}

const tr::RenderMesh::BlendShape* RenderStream::outputBlendShape_(
    int mesh_id, int shape_id) const {
  const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
  if (!mesh || shape_id < 0 ||
      static_cast<size_t>(shape_id) >= mesh->blend_shapes.size()) return nullptr;
  return &mesh->blend_shapes[static_cast<size_t>(shape_id)];
}

int RenderStream::meshBlendShapeCount(int mesh_id) const {
  if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return -1;
  const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
  if (!mesh) return 0;
  if (mesh->blend_shapes.size() >
      static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
  return static_cast<int>(mesh->blend_shapes.size());
}

int RenderStream::meshBlendShapeInfo(
    int mesh_id, int shape_id, int inbetween_id,
    lightusd_next_blend_shape_info* out) const {
  if (!out || out->struct_size < sizeof(*out)) return -1;
  const tr::RenderMesh::BlendShape* shape = outputBlendShape_(mesh_id, shape_id);
  if (!shape || inbetween_id < -1 ||
      (inbetween_id >= 0 &&
       static_cast<size_t>(inbetween_id) >= shape->inbetweens.size())) return -1;
  const uint32_t requested_size = out->struct_size;
  std::memset(out, 0, sizeof(*out));
  out->struct_size = requested_size;
  if (inbetween_id < 0) {
    if (shape->inbetweens.size() >
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) return -1;
    out->weight = shape->weight;
    out->inbetween_count = static_cast<uint32_t>(shape->inbetweens.size());
    if (!shape->normal_offsets.empty()) out->flags |= 1u;
  } else {
    out->weight = shape->inbetweens[static_cast<size_t>(inbetween_id)].weight;
  }
  return 0;
}

int RenderStream::meshBlendShapeNameCopy(
    int mesh_id, int shape_id, int inbetween_id,
    uint8_t* out, uint32_t cap) const {
  const tr::RenderMesh::BlendShape* shape = outputBlendShape_(mesh_id, shape_id);
  if (!shape || inbetween_id < -1 ||
      (inbetween_id >= 0 &&
       static_cast<size_t>(inbetween_id) >= shape->inbetweens.size())) return -1;
  const std::string& name = inbetween_id < 0
      ? shape->name : shape->inbetweens[static_cast<size_t>(inbetween_id)].name;
  if (name.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return -1;
  if (out && cap >= name.size() && !name.empty())
    std::memcpy(out, name.data(), name.size());
  return static_cast<int>(name.size());
}

int RenderStream::meshBlendShapeOffsetsCopy(
    int mesh_id, int shape_id, int inbetween_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const tr::RenderMesh::BlendShape* shape = outputBlendShape_(mesh_id, shape_id);
  if (!shape || inbetween_id < -1 || kind > 1 ||
      (inbetween_id >= 0 && (kind != 0 ||
       static_cast<size_t>(inbetween_id) >= shape->inbetweens.size()))) return -1;
  const tr::FloatChunked& values = inbetween_id >= 0
      ? shape->inbetweens[static_cast<size_t>(inbetween_id)].point_offsets
      : kind == 0 ? shape->point_offsets : shape->normal_offsets;
  if (kind == 1 && values.empty()) return 0;
  if (mesh_view_source_id_ != mesh_id) {
    lightusd_next_mesh_view view{};
    view.struct_size = sizeof(view);
    if (meshView(mesh_id, &view) != 0 || mesh_view_source_id_ != mesh_id)
      return -1;
  }
  if (s_point_source_indices_.size() >
      static_cast<size_t>((std::numeric_limits<int>::max)()) /
          (3 * sizeof(float))) return -1;
  const int required = static_cast<int>(s_point_source_indices_.size() *
                                        3 * sizeof(float));
  if (!out || cap < static_cast<uint32_t>(required)) return required;
  std::unordered_map<uint32_t, size_t> sparse_index;
  for (size_t i = 0; i < shape->point_indices.size(); ++i)
    sparse_index.emplace(shape->point_indices[i], i);
  size_t output = 0;
  for (uint32_t source_point : s_point_source_indices_) {
    size_t source_offset = static_cast<size_t>(source_point) * 3;
    if (!shape->point_indices.empty()) {
      const auto it = sparse_index.find(source_point);
      source_offset = it == sparse_index.end()
                          ? (std::numeric_limits<size_t>::max)()
                          : it->second * 3;
    }
    for (size_t component = 0; component < 3; ++component) {
      const float value =
          source_offset != (std::numeric_limits<size_t>::max)() &&
                  source_offset + component < values.size()
              ? values[source_offset + component] : 0.0f;
      std::memcpy(out + output, &value, sizeof(value));
      output += sizeof(value);
    }
  }
  return required;
}

}  // namespace web_next
}  // namespace lightusd
