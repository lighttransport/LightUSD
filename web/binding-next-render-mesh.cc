// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
bool RenderStream::buildRenderMesh_(const lightusd::next::UsdPrim &prim, bool *soup_out,
                        std::string *err) {
    if (soup_out) *soup_out = false;
    tr::ValueArrayRead<float> P;
    tr::ValueArrayRead<int32_t> fvc;
    tr::ValueArrayRead<int32_t> fvi;
    tr::ValueArrayRead<float> N;
    tr::ValueArrayRead<float> UV;
    tr::ValueArrayRead<int32_t> stIdx;
    (void)readFloatArray_(prim, "points", &P);
    (void)readIntArray_(prim, "faceVertexCounts", &fvc);
    (void)readIntArray_(prim, "faceVertexIndices", &fvi);
    (void)readFloatArray_(prim, "normals", &N);
    (void)readFloatArray_(prim, "primvars:st", &UV);
    if (UV.empty()) {
      UV = tr::ValueArrayRead<float>();
      (void)readFloatArray_(prim, "primvars:st0", &UV);
    }
    if (UV.empty()) {
      UV = tr::ValueArrayRead<float>();
      (void)readFloatArray_(prim, "st", &UV);
    }
    (void)readIntArray_(prim, "primvars:st:indices", &stIdx);

    const size_t vtxCount = P.size() / 3;
    const size_t faceVtx = fvi.size();
    const size_t uvCount = UV.size() / 2;
    const size_t nCount = N.size() / 3;

    // The next render converter already performs robust earcut triangulation,
    // handles left-handed winding, holes and topology sanitization, and keeps
    // a triangulated-corner -> authored-corner remap for face-varying data.
    // Reuse that result instead of independently fan-triangulating n-gons.
    const tr::RenderMesh *converted_mesh = nullptr;
    tr::RenderMesh mesh_only_triangulation;
    if (render_scene_valid_) {
      const auto it = render_scene_.mesh_by_path.find(prim.GetPath().str());
      if (it != render_scene_.mesh_by_path.end() && it->second >= 0 &&
          static_cast<size_t>(it->second) < render_scene_.meshes.size()) {
        const tr::RenderMesh &candidate =
            render_scene_.meshes[static_cast<size_t>(it->second)];
        if (candidate.is_triangulated &&
            (candidate.points.empty() || candidate.points.size() == P.size()) &&
            (candidate.triangulated_indices.size() % 3) == 0 &&
            candidate.triangulated_face_vertex_indices.size() ==
                candidate.triangulated_indices.size()) {
          bool valid = true;
          for (size_t corner = 0;
               corner < candidate.triangulated_indices.size(); ++corner) {
            if (candidate.triangulated_indices[corner] >= vtxCount ||
                candidate.triangulated_face_vertex_indices[corner] >= faceVtx) {
              valid = false;
              break;
            }
          }
          if (valid) converted_mesh = &candidate;
        }
      }
    }

    auto fail = [&](const std::string &msg) {
      if (err) *err = msg;
      return false;
    };
    if ((P.size() % 3) != 0) {
      return fail("Mesh points array length is not divisible by 3");
    }
    if ((N.size() % 3) != 0) {
      return fail("Mesh normals array length is not divisible by 3");
    }
    if ((UV.size() % 2) != 0) {
      return fail("Mesh texture coordinate array length is not divisible by 2");
    }
    if (!stIdx.empty()) {
      if (stIdx.size() != faceVtx) {
        return fail("Mesh texture coordinate index count does not match face vertex count");
      }
      for (int32_t idx : stIdx) {
        if (idx < 0 || static_cast<size_t>(idx) >= uvCount) {
          return fail("Mesh texture coordinate index is out of range");
        }
      }
    }
    if (fvc.empty()) {
      if (!fvi.empty() && (fvi.size() % 3) != 0) {
        return fail("Mesh indexed triangle list length is not divisible by 3");
      }
      for (int32_t idx : fvi) {
        if (idx < 0 || static_cast<size_t>(idx) >= vtxCount) {
          return fail("Mesh face index is out of point range");
        }
      }
    } else {
      size_t base = 0;
      for (int32_t n : fvc) {
        if (n < 0) {
          return fail("Mesh face vertex count is negative");
        }
        const size_t count = static_cast<size_t>(n);
        if (base > fvi.size() || count > fvi.size() - base) {
          return fail("Mesh face vertex counts exceed index array length");
        }
        base += count;
      }
      if (base != fvi.size()) {
        return fail("Mesh face vertex counts do not match index array length");
      }
      for (int32_t idx : fvi) {
        if (idx < 0 || static_cast<size_t>(idx) >= vtxCount) {
          return fail("Mesh face index is out of point range");
        }
      }
    }

    // meshOnly deliberately skips the full RenderScene conversion, but it
    // must not fall back to fan triangulation for polygonal meshes. Populate
    // only the topology needed by the shared robust converter so this path
    // matches animation/full-scene earcut, quad, winding and hole behavior.
    if (!converted_mesh &&
        std::any_of(fvc.begin(), fvc.end(), [](int32_t n) { return n != 3; })) {
      mesh_only_triangulation.prim_path = prim.GetPath().str();
      mesh_only_triangulation.points.append(P.view.data, P.size());
      for (int32_t n : fvc) {
        mesh_only_triangulation.face_vertex_counts.push_back(
            static_cast<uint32_t>(n));
      }
      for (int32_t index : fvi) {
        mesh_only_triangulation.face_vertex_indices.push_back(
            static_cast<uint32_t>(index));
      }
      if (const lightusd::next::Value *orientation =
              prim.GetPropertyValue("orientation")) {
        if (const std::string *token = orientation->as_token()) {
          mesh_only_triangulation.left_handed = (*token == "leftHanded");
        }
      }
      tr::ValueArrayRead<int32_t> holes;
      (void)readIntArray_(prim, "holeIndices", &holes);
      for (int32_t face : holes) {
        if (face >= 0 && static_cast<size_t>(face) < fvc.size()) {
          mesh_only_triangulation.hole_faces.push_back(
              static_cast<uint32_t>(face));
        }
      }
      std::sort(mesh_only_triangulation.hole_faces.begin(),
                mesh_only_triangulation.hole_faces.end());
      tr::ConverterConfig triangulation_config;
      triangulation_config.mesh.compute_normals = false;
      triangulation_config.mesh.compute_tangents = false;
      tr::RenderSceneConverter triangulator(triangulation_config);
      if (!mesh_only_triangulation.has_alloc_failure() &&
          triangulator.TriangulateMesh(&mesh_only_triangulation) &&
          mesh_only_triangulation.is_triangulated &&
          mesh_only_triangulation.triangulated_face_vertex_indices.size() ==
              mesh_only_triangulation.triangulated_indices.size()) {
        converted_mesh = &mesh_only_triangulation;
      }
    }

    const bool uvFaceVarying = !UV.empty() && uvCount != vtxCount &&
                               (uvCount == faceVtx || !stIdx.empty());
    const bool nFaceVarying = !N.empty() && nCount != vtxCount && nCount == faceVtx;
    const bool needExpand = uvFaceVarying || nFaceVarying || !stIdx.empty();

    s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
    s_point_source_indices_.clear();

    if (!needExpand) {
      s_points_.assign(P.begin(), P.end());
      s_point_source_indices_.resize(vtxCount);
      for (size_t i = 0; i < vtxCount; ++i) {
        s_point_source_indices_[i] = static_cast<uint32_t>(i);
      }
      if (converted_mesh) {
        s_indices_.resize(converted_mesh->triangulated_indices.size());
        for (size_t i = 0; i < s_indices_.size(); ++i) {
          s_indices_[i] = converted_mesh->triangulated_indices[i];
        }
      } else {
        triangulate_(P, fvi, fvc, s_indices_);
      }
      if (nCount == vtxCount) s_normals_.assign(N.begin(), N.end());
      else computeNormals_(s_points_, s_indices_, s_normals_);
      if (uvCount == vtxCount) s_uv_.assign(UV.begin(), UV.end());
      if (soup_out) *soup_out = false;
      return true;
    }

    const bool haveN = (nCount == vtxCount) || nFaceVarying;
    constexpr size_t kMaxRenderCorners = size_t(1) << 24;
    constexpr size_t kMaxEmittedVertices = size_t(1) << 24;
    auto readVec3 = [](const auto &src, int32_t idx,
                       float *x, float *y, float *z) {
      if (idx < 0) return false;
      const size_t i = static_cast<size_t>(idx);
      if (i >= (src.size() / 3)) return false;
      const size_t off = i * 3;
      if (off + 2 >= src.size()) return false;
      *x = src[off];
      *y = src[off + 1];
      *z = src[off + 2];
      return true;
    };
    auto readVec2 = [](const auto &src, int32_t idx,
                       float *x, float *y) {
      if (idx < 0) return false;
      const size_t i = static_cast<size_t>(idx);
      if (i >= (src.size() / 2)) return false;
      const size_t off = i * 2;
      if (off + 1 >= src.size()) return false;
      *x = src[off];
      *y = src[off + 1];
      return true;
    };
    auto indexFromSlot = [](size_t slot) -> int32_t {
      return slot <= static_cast<size_t>((std::numeric_limits<int32_t>::max)())
                 ? static_cast<int32_t>(slot)
                 : -1;
    };
    auto faceSpanAvailable = [](size_t base, int32_t n, size_t total) {
      if (n < 3) return false;
      const size_t count = static_cast<size_t>(n);
      return base <= total && count <= total - base;
    };
    auto advanceFaceBase = [](size_t base, int32_t n) {
      if (n <= 0) return base;
      const size_t add = static_cast<size_t>(n);
      if (base > (std::numeric_limits<size_t>::max)() - add) {
        return (std::numeric_limits<size_t>::max)();
      }
      return base + add;
    };
    auto quadUsesDiagonal13 = [&](size_t base) {
      if (base > fvi.size() || 4 > fvi.size() - base) return false;
      const int32_t ids[4] = {fvi[base], fvi[base + 1],
                              fvi[base + 2], fvi[base + 3]};
      for (int32_t id : ids) {
        if (id < 0 || static_cast<size_t>(id) >= vtxCount) return false;
      }
      auto distSq = [&](int32_t a, int32_t b) {
        const size_t ia = static_cast<size_t>(a) * 3;
        const size_t ib = static_cast<size_t>(b) * 3;
        const float dx = P[ia] - P[ib];
        const float dy = P[ia + 1] - P[ib + 1];
        const float dz = P[ia + 2] - P[ib + 2];
        return dx * dx + dy * dy + dz * dz;
      };
      return distSq(ids[1], ids[3]) < distSq(ids[0], ids[2]);
    };

    // Preserve the pre-existing adaptive behavior unless callers explicitly
    // request an index strategy.
    const bool doWeld = build_vertex_indices_set_ ? build_vertex_indices_ : [&]() {
      size_t triCount = converted_mesh
                            ? converted_mesh->triangulated_indices.size() / 3
                            : 0;
      if (!converted_mesh) {
        for (int32_t nn : fvc) {
          if (nn >= 3) {
            const size_t add = static_cast<size_t>(nn - 2);
            if (triCount > (std::numeric_limits<size_t>::max)() - add) {
              triCount = (std::numeric_limits<size_t>::max)();
              break;
            }
            triCount += add;
          }
        }
      }
      const size_t cornerCount =
          (triCount > (std::numeric_limits<size_t>::max)() / 3)
              ? (std::numeric_limits<size_t>::max)()
              : triCount * 3;
      return vtxCount > 0 && vtxCount < cornerCount / 3;
    }();

    if (!doWeld) {
      // Non-indexed triangle soup (the minimal form for unique-per-corner UVs).
      std::vector<size_t> slots;
      if (converted_mesh) {
        slots.resize(converted_mesh->triangulated_face_vertex_indices.size());
        for (size_t i = 0; i < slots.size(); ++i) {
          slots[i] = converted_mesh->triangulated_face_vertex_indices[i];
        }
      } else {
        size_t b = 0;
        for (int32_t n : fvc) {
          if (faceSpanAvailable(b, n, faceVtx)) {
            if (n == 4 && quadUsesDiagonal13(b)) {
              const size_t quad[6] = {b, b + 1, b + 3,
                                      b + 1, b + 2, b + 3};
              slots.insert(slots.end(), quad, quad + 6);
            } else for (int32_t k = 2; k < n; ++k) {
              if (slots.size() > kMaxRenderCorners - 3) {
                s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
                if (err) *err = "Mesh exceeds RenderStream triangle-corner limit";
                return false;
              }
              slots.push_back(b);
              slots.push_back(b + static_cast<size_t>(k) - 1);
              slots.push_back(b + static_cast<size_t>(k));
            }
          }
          b = advanceFaceBase(b, n);
        }
      }
      const size_t corners = slots.size();
      // Pre-flight the expanded-soup buffers (~44B per corner incl. this
      // scratch) so a huge mesh in a nearly-full heap fails this mesh with an
      // error instead of abort()ing the module (-fno-exceptions).
      if (!tr::ProbeAlloc(corners * 44)) {
        s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
        if (err) *err = "Out of memory expanding mesh corners";
        return false;
      }
      s_points_.resize(corners * 3);
      s_point_source_indices_.resize(corners, 0);
      if (!UV.empty()) s_uv_.assign(corners * 2, 0.0f);
      if (haveN) s_normals_.resize(corners * 3);
      for (size_t c = 0; c < corners; ++c) {
        const size_t slot = slots[c];
        const int32_t vi = (slot < faceVtx) ? fvi[slot] : -1;
        if (vi >= 0) s_point_source_indices_[c] = static_cast<uint32_t>(vi);
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        if (readVec3(P, vi, &px, &py, &pz)) {
          s_points_[c * 3] = px; s_points_[c * 3 + 1] = py; s_points_[c * 3 + 2] = pz;
        }
        if (!UV.empty()) {
          const int32_t ui = uvFaceVarying ? indexFromSlot(slot) : vi;  // st:indices is empty here
          float u = 0.0f, v = 0.0f;
          if (readVec2(UV, ui, &u, &v)) { s_uv_[c * 2] = u; s_uv_[c * 2 + 1] = v; }
        }
        if (haveN) {
          const int32_t ni = nFaceVarying ? indexFromSlot(slot) : vi;
          float nx = 0.0f, ny = 0.0f, nz = 0.0f;
          if (readVec3(N, ni, &nx, &ny, &nz)) {
            s_normals_[c * 3] = nx; s_normals_[c * 3 + 1] = ny; s_normals_[c * 3 + 2] = nz;
          }
        }
      }
      if (s_normals_.empty()) computeNormals_(s_points_, s_indices_, s_normals_);  // empty idx -> flat per-tri
      if (soup_out) *soup_out = true;
      return true;  // non-indexed soup
    }

    // De-index + weld: emit one welded vertex per unique (pos[,uv][,normal])
    // corner, producing an INDEXED mesh. Built inline as faces are walked, so the
    // full per-corner soup never exists — peak ~= welded verts + index buffer.
    struct WeldKey {
      uint32_t b[8];
      bool operator==(const WeldKey &o) const { return std::memcmp(b, o.b, sizeof(b)) == 0; }
    };
    struct WeldHash {
      size_t operator()(const WeldKey &k) const {
        uint64_t h = 1469598103934665603ull;  // FNV-1a (folded to size_t for wasm32)
        for (uint32_t w : k.b) { h ^= w; h *= 1099511628211ull; }
        return static_cast<size_t>(h ^ (h >> 32));
      }
    };
    std::unordered_map<WeldKey, uint32_t, WeldHash> weld;
    // Pre-flight a coarse budget for the weld structures: the index buffer is
    // ~4B per corner and the weld map ~56B per unique vertex (worst case one
    // per corner). This turns the realistic huge-mesh OOM into a per-mesh
    // error instead of an allocator abort; the map still grows incrementally.
    {
      size_t weld_corners = converted_mesh
                                ? converted_mesh->triangulated_indices.size()
                                : 0;
      if (!converted_mesh) {
        for (int32_t nn : fvc) {
          if (nn >= 3) weld_corners += static_cast<size_t>(nn - 2) * 3;
        }
      }
      const size_t probe = weld_corners * 4 +
                           std::min(weld_corners, vtxCount ? vtxCount * 2 : weld_corners) * 56;
      if (!tr::ProbeAlloc(probe)) {
        if (err) *err = "Out of memory welding mesh vertices";
        return false;
      }
    }
    // Welded vertices are bounded below by the point count; reserve to cut
    // rehash spikes (which transiently inflate the peak).
    constexpr size_t kMaxInitialWeldReserve = size_t(1) << 20;
    weld.reserve(vtxCount ? std::min(vtxCount, kMaxInitialWeldReserve) : 1024);

    auto emit = [&](size_t slot) -> bool {
      const int32_t vi = (slot < faceVtx) ? fvi[slot] : -1;
      float px = 0, py = 0, pz = 0, u = 0, v = 0, nx = 0, ny = 0, nz = 0;
      (void)readVec3(P, vi, &px, &py, &pz);
      if (!UV.empty()) {
        const int32_t ui = (!stIdx.empty() && slot < stIdx.size())
                                ? stIdx[slot]
                                : (uvFaceVarying ? indexFromSlot(slot) : vi);
        (void)readVec2(UV, ui, &u, &v);
      }
      if (haveN) {
        const int32_t ni = nFaceVarying ? indexFromSlot(slot) : vi;
        (void)readVec3(N, ni, &nx, &ny, &nz);
      }
      WeldKey key;
      std::memcpy(&key.b[0], &px, 4); std::memcpy(&key.b[1], &py, 4); std::memcpy(&key.b[2], &pz, 4);
      std::memcpy(&key.b[3], &u, 4); std::memcpy(&key.b[4], &v, 4);
      std::memcpy(&key.b[5], &nx, 4); std::memcpy(&key.b[6], &ny, 4); std::memcpy(&key.b[7], &nz, 4);
      auto it = weld.find(key);
      if (it != weld.end()) {
        s_indices_.push_back(it->second);
        return true;
      }
      const size_t nextIdx = s_points_.size() / 3;
      if (nextIdx >= kMaxEmittedVertices ||
          nextIdx > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        return false;
      }
      const uint32_t idx = static_cast<uint32_t>(nextIdx);
      s_points_.push_back(px); s_points_.push_back(py); s_points_.push_back(pz);
      s_point_source_indices_.push_back(
          vi < 0 ? uint32_t(0) : static_cast<uint32_t>(vi));
      if (!UV.empty()) { s_uv_.push_back(u); s_uv_.push_back(v); }
      if (haveN) { s_normals_.push_back(nx); s_normals_.push_back(ny); s_normals_.push_back(nz); }
      weld.emplace(key, idx);
      s_indices_.push_back(idx);
      return true;
    };

    if (converted_mesh) {
      for (size_t corner = 0;
           corner < converted_mesh->triangulated_face_vertex_indices.size();
           ++corner) {
        if (!emit(converted_mesh->triangulated_face_vertex_indices[corner])) {
          s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
          if (err) *err = "Mesh exceeds RenderStream emitted-vertex limit";
          return false;
        }
      }
    } else {
      size_t base = 0;
      for (int32_t n : fvc) {
        if (faceSpanAvailable(base, n, faceVtx)) {
          if (n == 4 && quadUsesDiagonal13(base)) {
            const size_t quad[6] = {base, base + 1, base + 3,
                                    base + 1, base + 2, base + 3};
            for (size_t slot : quad) {
              if (!emit(slot)) {
                s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
                if (err) *err = "Mesh exceeds RenderStream emitted-vertex limit";
                return false;
              }
            }
          } else for (int32_t k = 2; k < n; ++k) {
            if (!emit(base) ||
                !emit(base + static_cast<size_t>(k) - 1) ||
                !emit(base + static_cast<size_t>(k))) {
              s_points_.clear(); s_normals_.clear(); s_uv_.clear(); s_indices_.clear();
              if (err) *err = "Mesh exceeds RenderStream emitted-vertex limit";
              return false;
            }
          }
        }
        base = advanceFaceBase(base, n);
      }
    }
    // Normals not authored -> smooth normals on the welded indexed mesh.
    if (!haveN) computeNormals_(s_points_, s_indices_, s_normals_);
    if (soup_out) *soup_out = false;
    return true;  // welded result is INDEXED
  }
}  // namespace web_next
}  // namespace lightusd
