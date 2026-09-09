// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
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

emscripten::val RenderStream::outputSourceMesh_(int i) {
    emscripten::val out = emscripten::val::object();
    if (i < 0 || i >= static_cast<int>(meshes_.size())) {
      out.set("error", std::string("invalid source mesh index"));
      return out;
    }
    const lightusd::next::UsdPrim &prim = meshes_[static_cast<size_t>(i)].GetPrim();

    bool soup = false;  // indexed, or non-indexed soup
    std::string mesh_err;
    if (!buildRenderMesh_(prim, &soup, &mesh_err)) {
      out.set("error", mesh_err.empty() ? std::string("mesh build failed")
                                        : mesh_err);
      return out;
    }

    const int32_t material_id = materialIdForBoundPrim_(prim);
    out.set("vertexCount", static_cast<double>(s_points_.size() / 3));
    out.set("primName", prim.GetName());
    out.set("primPath", prim.GetPath().str());
    out.set("doubleSided",
            effectiveDoubleSided_(prim, material_id, s_points_));
    out.set("points", heapF_(s_points_, 3));
    if (!soup && !s_indices_.empty()) out.set("indices", heapU32_(s_indices_));
    if (!s_normals_.empty()) out.set("normals", heapF_(s_normals_, 3));
    if (!s_uv_.empty()) out.set("uv0", heapF_(s_uv_, 2));
    if (compute_tangents_ && computeScratchTangents_()) {
      out.set("tangents", heapF_(s_tangents_, 4));
      out.set("tangentMethod", tangent_method_);
    }
    s_joint_indices_.clear();
    s_joint_weights_.clear();
    if (render_scene_valid_) {
      auto it = render_scene_.mesh_by_path.find(prim.GetPath().str());
      if (it != render_scene_.mesh_by_path.end() && it->second >= 0 &&
          static_cast<size_t>(it->second) < render_scene_.meshes.size()) {
        const tr::RenderMesh& rmesh =
            render_scene_.meshes[static_cast<size_t>(it->second)];
        if (rmesh.skin) {
          const int element_size =
              static_cast<int>(rmesh.skin->influences_per_vertex);
          if (element_size > 0 &&
              s_point_source_indices_.size() == s_points_.size() / 3) {
            const size_t influences = static_cast<size_t>(element_size);
            s_joint_indices_.reserve(s_point_source_indices_.size() * influences);
            s_joint_weights_.reserve(s_point_source_indices_.size() * influences);
            for (uint32_t source_point : s_point_source_indices_) {
              const size_t source = static_cast<size_t>(source_point) * influences;
              if (source + influences > rmesh.skin->joint_indices.size() ||
                  source + influences > rmesh.skin->joint_weights.size()) {
                continue;
              }
              for (size_t influence = 0; influence < influences; ++influence) {
                s_joint_indices_.push_back(
                    rmesh.skin->joint_indices[source + influence]);
                s_joint_weights_.push_back(
                    rmesh.skin->joint_weights[source + influence]);
              }
            }
          }
          if (!s_joint_indices_.empty()) {
            out.set("jointIndices", heapU16_(s_joint_indices_));
          }
          if (!s_joint_weights_.empty()) {
            out.set("jointWeights", heapF_(s_joint_weights_, 1));
          }
          out.set("skel_id", rmesh.skin->skeleton_id);
          out.set("skeletonPath", rmesh.skin->skeleton_path);
          out.set("elementSize", element_size);
          out.set("hasGeomBindTransform", true);
          out.set("geomBindTransform",
                  MatrixValue(MatrixToArray(rmesh.skin->geom_bind_transform)));
        }
        if (!rmesh.blend_shapes.empty()) {
          auto float_array = [](const tr::FloatChunked& values) {
            emscripten::val array = emscripten::val::array();
            for (size_t index = 0; index < values.size(); ++index) {
              array.set(static_cast<unsigned>(index), values[index]);
            }
            return array;
          };
          auto remapped_offsets = [this](
              const tr::FloatChunked& values,
              const std::vector<uint32_t>& sparse_points) {
            emscripten::val array = emscripten::val::array();
            std::unordered_map<uint32_t, size_t> sparse_index;
            for (size_t i = 0; i < sparse_points.size(); ++i) {
              sparse_index.emplace(sparse_points[i], i);
            }
            size_t output = 0;
            for (uint32_t source_point : s_point_source_indices_) {
              size_t source_offset = static_cast<size_t>(source_point) * 3;
              if (!sparse_points.empty()) {
                const auto it = sparse_index.find(source_point);
                source_offset = it == sparse_index.end()
                                    ? (std::numeric_limits<size_t>::max)()
                                    : it->second * 3;
              }
              for (size_t component = 0; component < 3; ++component) {
                const float value =
                    source_offset != (std::numeric_limits<size_t>::max)() &&
                            source_offset + component < values.size()
                        ? values[source_offset + component]
                        : 0.0f;
                array.set(static_cast<unsigned>(output++), value);
              }
            }
            return array;
          };
          emscripten::val shapes = emscripten::val::array();
          for (size_t shape_index = 0;
               shape_index < rmesh.blend_shapes.size(); ++shape_index) {
            const tr::RenderMesh::BlendShape& shape =
                rmesh.blend_shapes[shape_index];
            emscripten::val value = emscripten::val::object();
            value.set("name", shape.name);
            value.set("weight", shape.weight);
            value.set("pointOffsets",
                      remapped_offsets(shape.point_offsets,
                                       shape.point_indices));
            value.set("normalOffsets",
                      shape.normal_offsets.empty()
                          ? float_array(shape.normal_offsets)
                          : remapped_offsets(shape.normal_offsets,
                                             shape.point_indices));
            value.set("pointIndices", emscripten::val::array());
            emscripten::val inbetweens = emscripten::val::array();
            for (size_t i = 0; i < shape.inbetweens.size(); ++i) {
              const tr::RenderMesh::BlendShape::Inbetween& source =
                  shape.inbetweens[i];
              emscripten::val inbetween = emscripten::val::object();
              inbetween.set("name", source.name);
              inbetween.set("weight", source.weight);
              inbetween.set("pointOffsets",
                            remapped_offsets(source.point_offsets,
                                             shape.point_indices));
              inbetweens.set(static_cast<unsigned>(i), inbetween);
            }
            value.set("inbetweens", inbetweens);
            shapes.set(static_cast<unsigned>(shape_index), value);
          }
          out.set("blendShapes", shapes);
        }
      }
    }
    out.set("localMatrix", matArray_(localMatrix_(prim)));
    out.set("worldMatrix", matArray_(worldMatrixForPrim_(prim)));
    out.set("materialId", material_id);
    out.set("material", materialObject_(material_id));
    addGeomSubsetMaterials_(prim, out);
    return out;
  }

emscripten::val RenderStream::outputMergedMesh_(const OutputMesh &record) const {
    emscripten::val out = emscripten::val::object();
    out.set("vertexCount", static_cast<double>(record.points.size() / 3));
    out.set("primName", record.name);
    out.set("primPath", record.prim_path);
    out.set("doubleSided", record.double_sided);
    out.set("points", heapF_(record.points, 3));
    if (!record.soup && !record.indices.empty()) {
      out.set("indices", heapU32_(record.indices));
    }
    if (!record.normals.empty()) out.set("normals", heapF_(record.normals, 3));
    if (!record.uv.empty()) out.set("uv0", heapF_(record.uv, 2));
    out.set("localMatrix", matArray_(record.local_matrix));
    out.set("worldMatrix", matArray_(record.world_matrix));
    out.set("materialId", record.material_id);
    out.set("material", materialObject_(record.material_id));
    return out;
  }
}  // namespace web_next
}  // namespace lightusd
