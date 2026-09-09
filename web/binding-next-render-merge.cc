// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
bool RenderStream::hasGeomSubset_(const lightusd::next::UsdPrim &prim) {
    for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
      if (child.IsValid() && child.GetTypeName() == "GeomSubset") return true;
    }
    return false;
  }

bool RenderStream::sameMatrix_(const std::array<double, 16> &a,
                          const std::array<double, 16> &b) {
    for (size_t i = 0; i < 16; ++i) {
      if (std::abs(a[i] - b[i]) > 1.0e-12) return false;
    }
    return true;
  }

std::string RenderStream::matrixKey_(const std::array<double, 16> &m) {
    std::ostringstream ss;
    ss << std::setprecision(17);
    for (double v : m) ss << v << ",";
    return ss.str();
  }

void RenderStream::transformPoint_(const std::array<double, 16> &m,
                              float *x, float *y, float *z) {
    const double px = *x;
    const double py = *y;
    const double pz = *z;
    *x = static_cast<float>(m[0] * px + m[4] * py + m[8] * pz + m[12]);
    *y = static_cast<float>(m[1] * px + m[5] * py + m[9] * pz + m[13]);
    *z = static_cast<float>(m[2] * px + m[6] * py + m[10] * pz + m[14]);
  }

void RenderStream::transformNormal_(const std::array<double, 16> &m,
                               float *x, float *y, float *z) {
    const double nx = *x;
    const double ny = *y;
    const double nz = *z;
    double tx = m[0] * nx + m[4] * ny + m[8] * nz;
    double ty = m[1] * nx + m[5] * ny + m[9] * nz;
    double tz = m[2] * nx + m[6] * ny + m[10] * nz;
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (len > 0.0) {
      tx /= len;
      ty /= len;
      tz /= len;
    }
    *x = static_cast<float>(tx);
    *y = static_cast<float>(ty);
    *z = static_cast<float>(tz);
  }

void RenderStream::flushAccumulator_(MergeAccumulator *acc) {
    if (!acc || acc->source_count == 0) return;
    if (acc->source_count == 1) {
      // A singleton is not a merge. Keep the authored mesh and transform
      // hierarchy instead of baking it into world-space float vertices.
      OutputMesh out;
      out.merged = false;
      out.source_index = acc->first_source_index;
      outputs_.push_back(std::move(out));
      acc->mesh = OutputMesh{};
      acc->source_count = 0;
      acc->first_source_index = -1;
      return;
    }
    acc->mesh.merged = true;
    acc->mesh.name = "merged_material_" + std::to_string(acc->mesh.material_id);
    acc->mesh.prim_path = "/__lightusd_next_merged/" + acc->mesh.name + "_" +
                          std::to_string(outputs_.size());
    outputs_.push_back(std::move(acc->mesh));
    stats_.merge_group_count++;
    stats_.merged_mesh_count += acc->source_count;
    acc->mesh = OutputMesh{};
    acc->source_count = 0;
    acc->first_source_index = -1;
  }

bool RenderStream::appendToAccumulator_(const lightusd::next::UsdPrim &prim,
                            int source_index,
                            int32_t material_id,
                            bool double_sided,
                            bool soup,
                            MergeAccumulator *acc) {
    if (!acc) return false;
    // The first mesh transfers its buffers without allocation. For later
    // meshes, pre-flight vector growth (which can transiently need ~2x): a
    // failed probe keeps that mesh unmerged instead of abort()ing.
    if (acc->source_count != 0) {
      const size_t add_bytes =
          (s_points_.size() + s_normals_.size() + s_uv_.size()) * sizeof(float) +
          s_indices_.size() * sizeof(uint32_t);
      if (!tr::ProbeAlloc(add_bytes * 2 + acc->mesh.points.size() * sizeof(float))) {
        return false;
      }
    }
    const std::array<double, 16> world = worldMatrixForPrim_(prim);
    if (acc->source_count == 0) {
      acc->first_source_index = source_index;
      acc->mesh.soup = soup;
      acc->mesh.material_id = material_id;
      acc->mesh.double_sided = double_sided;
      acc->mesh.local_matrix = mesh_merge_bake_transform_ ? identityMatrix_()
                                                          : localMatrix_(prim);
      acc->mesh.world_matrix = mesh_merge_bake_transform_ ? identityMatrix_()
                                                          : world;
      acc->mesh.points = std::move(s_points_);
      acc->mesh.normals = std::move(s_normals_);
      acc->mesh.uv = std::move(s_uv_);
      if (!soup) acc->mesh.indices = std::move(s_indices_);
      if (mesh_merge_bake_transform_) {
        for (size_t off = 0; off + 2 < acc->mesh.points.size(); off += 3) {
          transformPoint_(world, &acc->mesh.points[off],
                          &acc->mesh.points[off + 1],
                          &acc->mesh.points[off + 2]);
        }
        for (size_t off = 0; off + 2 < acc->mesh.normals.size(); off += 3) {
          transformNormal_(world, &acc->mesh.normals[off],
                           &acc->mesh.normals[off + 1],
                           &acc->mesh.normals[off + 2]);
        }
      }
      acc->source_count = 1;
      return true;
    }
    const uint32_t vertex_offset =
        static_cast<uint32_t>(acc->mesh.points.size() / 3);
    const size_t point_base = acc->mesh.points.size();
    acc->mesh.points.insert(acc->mesh.points.end(), s_points_.begin(),
                            s_points_.end());
    if (!s_normals_.empty()) {
      acc->mesh.normals.insert(acc->mesh.normals.end(), s_normals_.begin(),
                               s_normals_.end());
    }
    if (!s_uv_.empty()) {
      acc->mesh.uv.insert(acc->mesh.uv.end(), s_uv_.begin(), s_uv_.end());
    }
    if (!soup) {
      acc->mesh.indices.reserve(acc->mesh.indices.size() + s_indices_.size());
      for (uint32_t idx : s_indices_) acc->mesh.indices.push_back(idx + vertex_offset);
    }
    if (mesh_merge_bake_transform_) {
      for (size_t off = point_base; off + 2 < acc->mesh.points.size(); off += 3) {
        transformPoint_(world, &acc->mesh.points[off],
                        &acc->mesh.points[off + 1],
                        &acc->mesh.points[off + 2]);
      }
      const size_t normal_base =
          acc->mesh.normals.size() >= s_normals_.size()
              ? acc->mesh.normals.size() - s_normals_.size()
              : acc->mesh.normals.size();
      for (size_t off = normal_base; off + 2 < acc->mesh.normals.size(); off += 3) {
        transformNormal_(world, &acc->mesh.normals[off],
                         &acc->mesh.normals[off + 1],
                         &acc->mesh.normals[off + 2]);
      }
    }
    acc->source_count++;
    return true;
  }

void RenderStream::buildOptimizedOutputs_() {
    outputs_.clear();
    std::unordered_map<std::string, MergeAccumulator> groups;
    constexpr size_t kMaxGroupVertices = size_t(1) << 20;
    constexpr size_t kMaxGroupIndices = size_t(3) << 20;

    for (size_t i = 0; i < meshes_.size(); ++i) {
      const lightusd::next::UsdPrim &prim = meshes_[i].GetPrim();
      const double material_start_ms = emscripten_get_now();
      const int32_t material_id = materialIdForBoundPrim_(prim);
      stats_.material_ms += emscripten_get_now() - material_start_ms;
      if (hasGeomSubset_(prim)) {
        OutputMesh out;
        out.merged = false;
        out.source_index = static_cast<int>(i);
        outputs_.push_back(out);
        stats_.skipped_merge_count++;
        continue;
      }

      bool soup = false;
      std::string mesh_err;
      const double geometry_start_ms = emscripten_get_now();
      if (!buildRenderMesh_(prim, &soup, &mesh_err)) {
        stats_.geometry_build_ms += emscripten_get_now() - geometry_start_ms;
        OutputMesh out;
        out.merged = false;
        out.source_index = static_cast<int>(i);
        outputs_.push_back(out);
        stats_.skipped_merge_count++;
        continue;
      }
      stats_.geometry_build_ms += emscripten_get_now() - geometry_start_ms;
      const bool has_normals = !s_normals_.empty();
      const bool has_uv = !s_uv_.empty();
      const bool double_sided =
          effectiveDoubleSided_(prim, material_id, s_points_);
      const std::array<double, 16> world = worldMatrixForPrim_(prim);
      std::ostringstream key;
      key << material_id << "|soup=" << soup << "|n=" << has_normals
          << "|uv=" << has_uv << "|double=" << double_sided;
      if (!mesh_merge_bake_transform_) key << "|m=" << matrixKey_(world);
      MergeAccumulator &acc = groups[key.str()];
      if (acc.source_count > 0 &&
          (acc.mesh.soup != soup ||
           acc.mesh.material_id != material_id ||
           acc.mesh.double_sided != double_sided ||
           (!mesh_merge_bake_transform_ &&
            !sameMatrix_(acc.mesh.world_matrix, world)))) {
        flushAccumulator_(&acc);
      }
      const size_t next_vertices = acc.mesh.points.size() / 3 + s_points_.size() / 3;
      const size_t next_indices =
          triangleIndexCount_(acc.mesh.indices, acc.mesh.points) +
          triangleIndexCount_(s_indices_, s_points_);
      if (acc.source_count > 0 &&
          (next_vertices > kMaxGroupVertices || next_indices > kMaxGroupIndices)) {
        flushAccumulator_(&acc);
      }
      const double append_start_ms = emscripten_get_now();
      if (!appendToAccumulator_(prim, static_cast<int>(i), material_id,
                                double_sided, soup, &acc)) {
        stats_.merge_append_ms += emscripten_get_now() - append_start_ms;
        // Heap too full to merge: flush the group and emit this mesh unmerged.
        flushAccumulator_(&acc);
        OutputMesh out;
        out.merged = false;
        out.source_index = static_cast<int>(i);
        outputs_.push_back(out);
        stats_.skipped_merge_count++;
      } else {
        stats_.merge_append_ms += emscripten_get_now() - append_start_ms;
      }
    }
    for (auto &kv : groups) flushAccumulator_(&kv.second);
    stats_.source_material_count = source_material_keys_.size();
    stats_.source_texture_count = source_texture_keys_.size();
  }

void RenderStream::buildMeshTransformCaches_() {
    tr::RenderExtractOptions options;
    options.collect_records = false;
    tr::RenderExtractResult extracted;
    if (!tr::CollectRenderPrims(stage_, options, &extracted)) return;
    for (const tr::RenderPrimRecord &record : extracted.meshes) {
      std::array<double, 16> local;
      std::array<double, 16> world;
      for (int i = 0; i < 16; ++i) {
        local[static_cast<size_t>(i)] = record.local[i];
        world[static_cast<size_t>(i)] = record.world[i];
      }
      local_matrix_cache_[record.path] = local;
      world_matrix_cache_[record.path] = world;
    }
  }

std::array<double, 16> RenderStream::localMatrix_(
      const lightusd::next::UsdPrim &prim) const {
    const std::string path = prim.GetPath().str();
    const auto cached = local_matrix_cache_.find(path);
    if (cached != local_matrix_cache_.end()) return cached->second;
    std::array<double, 16> m = identityMatrix_();
    lightusd::next::UsdGeomXform xform(prim);
    double raw[16];
    if (xform.ComputeLocalTransform(raw)) {
      for (int i = 0; i < 16; ++i) m[static_cast<size_t>(i)] = raw[i];
    }
    local_matrix_cache_.emplace(path, m);
    return m;
  }

std::array<double, 16> RenderStream::worldMatrix_(
      const lightusd::next::UsdPrim &prim) const {
    std::vector<lightusd::next::UsdPrim> chain;
    std::array<double, 16> world = identityMatrix_();
    for (lightusd::next::UsdPrim p = prim; p.IsValid(); p = p.GetParent()) {
      const auto cached = world_matrix_cache_.find(p.GetPath().str());
      if (cached != world_matrix_cache_.end()) {
        world = cached->second;
        break;
      }
      chain.push_back(p);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const std::array<double, 16> local = localMatrix_(*it);
      world = multiplyMatrix_(local, world);
      world_matrix_cache_[it->GetPath().str()] = world;
    }
    return world;
  }

std::array<double, 16> RenderStream::worldMatrixForPrim_(
      const lightusd::next::UsdPrim &prim) const {
    if (render_scene_valid_) {
      const auto it = render_scene_.node_by_path.find(prim.GetPath().str());
      if (it != render_scene_.node_by_path.end() && it->second >= 0 &&
          static_cast<size_t>(it->second) < render_scene_.nodes.size()) {
        const tr::SceneNode &node =
            render_scene_.nodes[static_cast<size_t>(it->second)];
        std::array<double, 16> world;
        for (int i = 0; i < 16; ++i) {
          world[static_cast<size_t>(i)] =
              static_cast<double>(node.world_transform.m[i]);
        }
        return world;
      }
    }
    return worldMatrix_(prim);
  }
}  // namespace web_next
}  // namespace lightusd
