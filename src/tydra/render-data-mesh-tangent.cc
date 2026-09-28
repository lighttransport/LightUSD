// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
//
// Mesh conversion routines split from render-data.cc
//
#include <numeric>
#include <set>

#include "common-utils.hh"
#include "common-types.hh"
#include "../tiny-hashmap.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#include "linear-algebra.hh"
#include "math-util.inc"
#include "pprint-enum.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "value-pprint.hh"
#include "bone-util.hh"
#include "shape-to-mesh.hh"
#include "materialx-to-json.hh"
#include "mmap-array-ref.hh"
#include "safe-arithmetic.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// For tangent/binormal computation
// NOTE: HalfEdge is not used atm.
#include "external/half-edge.hh"

#if defined(LIGHTUSD_WITH_MESHOPT)
#include "external/meshoptimizer/meshoptimizer.h"
#endif

// MikkTSpace tangent computation
#include "mikktspace-tangent.hh"
// Optimized MikkTSpace reimplementation
#include "fast-mikktspace.hh"

// For triangulation.
// TODO: Use tinyobjloader's triangulation
#include "external/mapbox/earcut/earcut.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//
#include "common-macros.inc"
#include "math-util.inc"


//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/tangent-quantize.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

#include "tydra/render-data-mesh-internal.hh"

namespace lightusd {
namespace tydra {

#define PushError(msg) TYDRA_PUSH_ERROR(err, msg)

// Geometric (area-weighted) normal of a triangle. Also returns its area.
inline static value::float3 GeometricNormal(const value::float3 v0,
                                            const value::float3 v1,
                                            const value::float3 v2,
                                            float &area) {
  const value::float3 v10 = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
  const value::float3 v20 = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};

  value::float3 Nf = vcross(v10, v20);  // CCW
  float len = vlength(Nf);
  area = 0.5f * len;

  // Guard against degenerate triangles (collinear/coincident vertices).
  // Return zero normal; caller should check area before using.
  if (len < 1.0e-30f) {
    return {0.0f, 0.0f, 0.0f};
  }

  float inv = 1.0f / len;
  return {Nf[0] * inv, Nf[1] * inv, Nf[2] * inv};
}

bool ComputeTangentsAndBinormals(
    const std::vector<vec3> &vertices,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices,
    const std::vector<vec2> &texcoords, const std::vector<vec3> &normals,
    bool is_facevarying_input,  // false: 'vertex' varying
    std::vector<vec3> *tangents, std::vector<vec3> *binormals,
    std::vector<uint32_t> *out_vertex_indices, std::string *err,
    uint32_t max_vertex_valence, float dedup_eps) {
  if (!tangents) {
    PUSH_ERROR_AND_RETURN("tangents arg is nullptr.");
  }

  if (!binormals) {
    PUSH_ERROR_AND_RETURN("binormals arg is nullptr.");
  }

  if (!out_vertex_indices) {
    PUSH_ERROR_AND_RETURN("out_indices arg is nullptr.");
  }

  if (vertices.empty()) {
    PUSH_ERROR_AND_RETURN("vertices is empty.");
  }

  // At least 1 triangle face should exist.
  if (faceVertexIndices.size() < 3) {
    PUSH_ERROR_AND_RETURN("faceVertexIndices.size < 3");
  }

  if (texcoords.empty()) {
    PUSH_ERROR_AND_RETURN("texcoords is empty");
  }

  if (normals.empty()) {
    PUSH_ERROR_AND_RETURN("normals is empty");
  }

  if (is_facevarying_input) {
    if (vertices.size() != faceVertexIndices.size()) {
      PUSH_ERROR_AND_RETURN("vertices.size (" << vertices.size() << ") != faceVertexIndices.size (" << faceVertexIndices.size() << ")");
    }
    if (texcoords.size() != faceVertexIndices.size()) {
      PUSH_ERROR_AND_RETURN("texcoords.size (" << texcoords.size() << ") != faceVertexIndices.size (" << faceVertexIndices.size() << ")");
    }
    if (normals.size() != faceVertexIndices.size()) {
      PUSH_ERROR_AND_RETURN("normals.size (" << normals.size() << ") != faceVertexIndices.size (" << faceVertexIndices.size() << ")");
    }
  } else {
    uint32_t max_vert_index =
        *std::max_element(faceVertexIndices.begin(), faceVertexIndices.end());
    if (max_vert_index >= vertices.size()) {
      PUSH_ERROR_AND_RETURN("max vertex index (" << max_vert_index << ") >= vertices.size (" << vertices.size() << ")");
    }
    if (max_vert_index >= texcoords.size()) {
      PUSH_ERROR_AND_RETURN("max vertex index (" << max_vert_index << ") >= texcoords.size (" << texcoords.size() << ")");
    }
    if (max_vert_index >= normals.size()) {
      PUSH_ERROR_AND_RETURN("max vertex index (" << max_vert_index << ") >= normals.size (" << normals.size() << ")");
    }
  }

  bool hasFaceVertexCounts = true;
  if (faceVertexCounts.size() == 0) {
    // Assume all triangle faces.
    if ((faceVertexIndices.size() % 3) != 0) {
      PUSH_ERROR_AND_RETURN(
          "Invalid faceVertexIndices. It must be all triangles: "
          "faceVertexIndices.size % 3 == 0");
    }
    hasFaceVertexCounts = false;
  }

  // Helper: check if a float is finite (not NaN, not Inf)
  auto is_finite_f = [](float x) -> bool {
    return std::isfinite(x);
  };

  // Helper: check if a vec3/normal3f has all-finite components
  auto is_finite_v3 = [&is_finite_f](const value::normal3f &v) -> bool {
    return is_finite_f(v[0]) && is_finite_f(v[1]) && is_finite_f(v[2]);
  };

  // Helper: safe length with NaN protection (returns 0 for NaN/Inf input)
  auto safe_vlength = [&is_finite_v3](const value::normal3f &v) -> float {
    if (!is_finite_v3(v)) return 0.0f;
    return vlength(v);
  };

  // Helper: safe normalize - returns zero vector if input is degenerate/NaN/Inf
  // kTangentLengthEps is a constexpr local referenced inside the lambdas
  // below; MSVC requires it to be in the capture list, so use [&] rather
  // than naming individual captures.
  constexpr float kTangentLengthEps = 1.0e-7f;
  auto safe_vnormalize = [&](const value::normal3f &v) -> value::normal3f {
    float len = safe_vlength(v);
    if (len < kTangentLengthEps) {
      return {0.0f, 0.0f, 0.0f};
    }
    float inv = 1.0f / len;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
  };

  // Helper: generate a perpendicular tangent from a normal (fallback)
  auto generate_fallback_tangent =
      [&](const value::normal3f &n) -> value::normal3f {
    // Choose a reference axis not parallel to n
    value::normal3f ref = (std::fabs(n[1]) < 0.9f)
                              ? value::normal3f{0.0f, 1.0f, 0.0f}
                              : value::normal3f{1.0f, 0.0f, 0.0f};
    value::normal3f t = vcross(n, ref);
    float len = safe_vlength(t);
    if (len < kTangentLengthEps) {
      return {1.0f, 0.0f, 0.0f};  // last resort
    }
    float inv = 1.0f / len;
    return {t[0] * inv, t[1] * inv, t[2] * inv};
  };

  // tn, bn = facevarying (value-initialized to zero by constructor)
  std::vector<value::normal3f> tn(faceVertexIndices.size(), {0.0f, 0.0f, 0.0f});
  std::vector<value::normal3f> bn(faceVertexIndices.size(), {0.0f, 0.0f, 0.0f});

  //
  // 1. Compute facevarying tangent/binormal for each faceVertex.
  //
  // UV determinant epsilon: use a float-appropriate threshold.
  // Values below this produce unreliable tangent directions due to
  // amplification of floating-point noise.
  constexpr float kUVDetEps = 1.0e-6f;

  size_t faceVertexIndexOffset{0};
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    size_t nv = hasFaceVertexCounts ? faceVertexCounts[i] : 3;

    if ((faceVertexIndexOffset + nv) > faceVertexIndices.size()) {
      // Invalid faceVertexIndices
      PUSH_ERROR_AND_RETURN("Invalid value in faceVertexOffset.");
    }

    if (nv < 3) {
      PUSH_ERROR_AND_RETURN("Degenerated facet found.");
    }

    // Process each two-edges per facet.
    //
    // Example:
    //
    // fv3
    //  o----------------o fv2
    //   \              /
    //    \            /
    //     o----------o
    //    fv0         fv1

    // facet0:  fv0, fv1, fv2
    // facet1:  fv1, fv2, fv3

    for (size_t f = 0; f < nv - 2; f++) {
      size_t fid0 = faceVertexIndexOffset + f;
      size_t fid1 = faceVertexIndexOffset + f + 1;
      size_t fid2 = faceVertexIndexOffset + f + 2;

      uint32_t vf0 =
          is_facevarying_input ? uint32_t(fid0) : faceVertexIndices[fid0];
      uint32_t vf1 =
          is_facevarying_input ? uint32_t(fid1) : faceVertexIndices[fid1];
      uint32_t vf2 =
          is_facevarying_input ? uint32_t(fid2) : faceVertexIndices[fid2];

      if ((vf0 >= vertices.size()) || (vf1 >= vertices.size()) ||
          (vf2 >= vertices.size())) {
        // index out-of-range
        PUSH_ERROR_AND_RETURN(
            "Invalid value in faceVertexIndices. some exceeds vertices.size()");
      }

      if ((vf0 >= texcoords.size()) || (vf1 >= texcoords.size()) ||
          (vf2 >= texcoords.size())) {
        // index out-of-range
        PUSH_ERROR_AND_RETURN("Invalid index. some exceeds texcoords.size()");
      }

      vec3 v1 = vertices[vf0];
      vec3 v2 = vertices[vf1];
      vec3 v3 = vertices[vf2];

      vec2 uv1 = texcoords[vf0];
      vec2 uv2 = texcoords[vf1];
      vec2 uv3 = texcoords[vf2];

      // Skip triangle if any position or UV contains NaN/Inf
      if (!is_finite_f(v1[0]) || !is_finite_f(v1[1]) || !is_finite_f(v1[2]) ||
          !is_finite_f(v2[0]) || !is_finite_f(v2[1]) || !is_finite_f(v2[2]) ||
          !is_finite_f(v3[0]) || !is_finite_f(v3[1]) || !is_finite_f(v3[2]) ||
          !is_finite_f(uv1[0]) || !is_finite_f(uv1[1]) ||
          !is_finite_f(uv2[0]) || !is_finite_f(uv2[1]) ||
          !is_finite_f(uv3[0]) || !is_finite_f(uv3[1])) {
        // Leave tn/bn as zero for this face vertex - will get fallback later
        continue;
      }

      float x1 = v2[0] - v1[0];
      float x2 = v3[0] - v1[0];
      float y1 = v2[1] - v1[1];
      float y2 = v3[1] - v1[1];
      float z1 = v2[2] - v1[2];
      float z2 = v3[2] - v1[2];

      float s1 = uv2[0] - uv1[0];
      float s2 = uv3[0] - uv1[0];
      float t1 = uv2[1] - uv1[1];
      float t2 = uv3[1] - uv1[1];

      float det = s1 * t2 - s2 * t1;

      // Skip degenerate UV triangle: determinant too small means all UV
      // vertices are collinear (or coincident).  The tangent direction is
      // undefined; leave tn/bn as zero to trigger fallback later.
      if (std::fabs(det) < kUVDetEps) {
        continue;
      }

      float r = 1.0f / det;

      vec3 tdir{(t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r,
                (t2 * z1 - t1 * z2) * r};
      vec3 bdir{(s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r,
                (s1 * z2 - s2 * z1) * r};

      // Guard against Inf/NaN from extreme edge/UV ratios
      if (!is_finite_f(tdir[0]) || !is_finite_f(tdir[1]) ||
          !is_finite_f(tdir[2]) || !is_finite_f(bdir[0]) ||
          !is_finite_f(bdir[1]) || !is_finite_f(bdir[2])) {
        continue;
      }

      // Accumulate tangent/binormal contributions.
      // For triangles, += on zero-init is equivalent to =.
      // For quads/polygons, shared facevarying vertices get correct accumulation.
      tn[fid0][0] += tdir[0];
      tn[fid0][1] += tdir[1];
      tn[fid0][2] += tdir[2];

      tn[fid1][0] += tdir[0];
      tn[fid1][1] += tdir[1];
      tn[fid1][2] += tdir[2];

      tn[fid2][0] += tdir[0];
      tn[fid2][1] += tdir[1];
      tn[fid2][2] += tdir[2];

      bn[fid0][0] += bdir[0];
      bn[fid0][1] += bdir[1];
      bn[fid0][2] += bdir[2];

      bn[fid1][0] += bdir[0];
      bn[fid1][1] += bdir[1];
      bn[fid1][2] += bdir[2];

      bn[fid2][0] += bdir[0];
      bn[fid2][1] += bdir[1];
      bn[fid2][2] += bdir[2];
    }

    faceVertexIndexOffset += nv;
  }

  //
  // 2. Build indices(use same index for shared-vertex)
  //    Position-bucketed dedup: bucket by position index, linear scan within
  //    each bucket comparing only normal + uv (typical valence 4-8).
  //
  // Build facevarying normal lookup (used by both dedup and Gram-Schmidt).
  // normals[i] is facevarying when is_facevarying_input, otherwise indexed by
  // original vertex id → expand to facevarying.
  std::vector<value::normal3f> fv_normals(faceVertexIndices.size());
  if (is_facevarying_input) {
    for (size_t i = 0; i < faceVertexIndices.size(); i++) {
      fv_normals[i] = {normals[i][0], normals[i][1], normals[i][2]};
    }
  } else {
    for (size_t i = 0; i < faceVertexIndices.size(); i++) {
      const auto &n = normals[faceVertexIndices[i]];
      fv_normals[i] = {n[0], n[1], n[2]};
    }
  }

  std::vector<uint32_t> vertex_indices(faceVertexIndices.size());
  {
    // Expand texcoords to facevarying if needed (normals already expanded above).
    // Use fv_normals as vec3* for the dedup comparison — same memory layout.
    const vec3 *nrm_ptr = reinterpret_cast<const vec3 *>(fv_normals.data());
    const vec2 *uv_ptr = nullptr;
    std::vector<vec2> fv_uvs_expanded;

    if (is_facevarying_input) {
      uv_ptr = texcoords.data();
    } else {
      fv_uvs_expanded.resize(faceVertexIndices.size());
      for (size_t i = 0; i < faceVertexIndices.size(); i++) {
        fv_uvs_expanded[i] = texcoords[faceVertexIndices[i]];
      }
      uv_ptr = fv_uvs_expanded.data();
    }

    uint32_t numPoints = *std::max_element(faceVertexIndices.begin(),
                                           faceVertexIndices.end()) + 1;
    uint32_t next_vertex_id = 0;

    // Pre-count per-position degree to detect high-valence vertices.
    bool t_flatten = false;
    if (max_vertex_valence > 0) {
      std::vector<uint32_t> degree(numPoints, 0);
      for (size_t i = 0; i < faceVertexIndices.size(); i++) {
        degree[faceVertexIndices[i]]++;
      }
      uint32_t max_deg = *std::max_element(degree.begin(), degree.end());
      if (max_deg > max_vertex_valence) {
        DCOUT("tangent dedup: max vertex degree " << max_deg
              << " exceeds threshold " << max_vertex_valence
              << ", falling back to flatten.");
        t_flatten = true;
      }
    }

    if (t_flatten) {
      // Flatten: each face-vertex is its own unique vertex.
      for (size_t i = 0; i < faceVertexIndices.size(); i++) {
        vertex_indices[i] = uint32_t(i);
      }
      next_vertex_id = uint32_t(faceVertexIndices.size());
    } else {
      auto attribs_match = [&](size_t a, size_t b) -> bool {
        if (dedup_eps > 0.0f) {
          if (!math::is_close(nrm_ptr[a], nrm_ptr[b], dedup_eps)) return false;
          if (!math::is_close(uv_ptr[a], uv_ptr[b], dedup_eps)) return false;
        } else {
          if (memcmp(&nrm_ptr[a], &nrm_ptr[b], sizeof(vec3)) != 0) return false;
          if (memcmp(&uv_ptr[a], &uv_ptr[b], sizeof(vec2)) != 0) return false;
        }
        return true;
      };

      // Per-position buckets as intrusive singly-linked lists over a single
      // flat arena. This replaces `vector<vector<BucketEntry>>` (which made
      // `numPoints` heap allocations plus per-bucket reallocations) with a few
      // flat allocations: `head`/`tail` (one slot per position) and `arena`
      // (one entry per unique vertex).
      static constexpr uint32_t kNoEntry = ~0u;
      struct BucketEntry {
        uint32_t fv_index;
        uint32_t out_vertex_id;
        uint32_t next;  // arena index of next entry at this position, or kNoEntry
      };

      std::vector<uint32_t> head(numPoints, kNoEntry);
      std::vector<uint32_t> tail(numPoints, kNoEntry);
      std::vector<BucketEntry> arena;
      arena.reserve(numPoints);  // typical meshes dedup to ~numPoints vertices

      for (size_t i = 0; i < faceVertexIndices.size(); i++) {
        uint32_t pid = faceVertexIndices[i];
        uint32_t matched_id = kNoEntry;
        // Traverse oldest-to-newest (insertion order). With a tolerant
        // attribs_match (dedup_eps > 0) the match relation is not transitive,
        // so a query can match more than one entry; preserving insertion-order
        // traversal makes the first match identical to the previous
        // vector<vector> + push_back code, keeping vertex_indices[]
        // byte-identical.
        for (uint32_t e = head[pid]; e != kNoEntry; e = arena[e].next) {
          if (attribs_match(i, arena[e].fv_index)) {
            matched_id = arena[e].out_vertex_id;
            break;
          }
        }
        if (matched_id == kNoEntry) {
          matched_id = next_vertex_id++;
          const uint32_t idx = uint32_t(arena.size());
          arena.push_back({uint32_t(i), matched_id, kNoEntry});
          if (head[pid] == kNoEntry) {
            head[pid] = idx;
          } else {
            arena[tail[pid]].next = idx;
          }
          tail[pid] = idx;
        }
        vertex_indices[i] = matched_id;
      }
    }

    DCOUT("faceVertexIndices.size : " << faceVertexIndices.size());
    DCOUT("tangent dedup: " << next_vertex_id << " unique vertices from "
          << faceVertexIndices.size() << " face-vertices."
          << (t_flatten ? " (flattened)" : ""));
  }

  const uint32_t num_verts =
      *std::max_element(vertex_indices.begin(), vertex_indices.end()) + 1;

  //
  // 3. normalize * orthogonalize;
  //
  // fv_normals was already built above (before dedup block).

  // per-vertex tangents/binormals
  std::vector<value::normal3f> v_tn;
  v_tn.assign(num_verts, {0.0f, 0.0f, 0.0f});

  std::vector<value::normal3f> v_bn;
  v_bn.assign(num_verts, {0.0f, 0.0f, 0.0f});

  // Accumulate facevarying tangents into per-vertex.
  // tn[i] is the facevarying tangent at position i; vertex_indices[i] maps
  // facevarying position i to the unique vertex index.
  // Skip NaN/Inf contributions to prevent poisoning the accumulator.
  for (size_t i = 0; i < vertex_indices.size(); i++) {
    value::normal3f Tn = tn[i];
    value::normal3f Bn = bn[i];

    if (is_finite_v3(Tn)) {
      v_tn[vertex_indices[i]][0] += Tn[0];
      v_tn[vertex_indices[i]][1] += Tn[1];
      v_tn[vertex_indices[i]][2] += Tn[2];
    }

    if (is_finite_v3(Bn)) {
      v_bn[vertex_indices[i]][0] += Bn[0];
      v_bn[vertex_indices[i]][1] += Bn[1];
      v_bn[vertex_indices[i]][2] += Bn[2];
    }
  }

  // Normalize accumulated tangents/binormals with proper epsilon.
  // After accumulation the sum could overflow to Inf for vertices shared by
  // many triangles with large contributions.  safe_vnormalize handles this
  // gracefully by returning zero for degenerate/NaN/Inf input.
  for (size_t i = 0; i < size_t(num_verts); i++) {
    v_tn[i] = safe_vnormalize(v_tn[i]);
    v_bn[i] = safe_vnormalize(v_bn[i]);
  }

  tangents->assign(num_verts, {0.0f, 0.0f, 0.0f});
  binormals->assign(num_verts, {0.0f, 0.0f, 0.0f});

  for (size_t i = 0; i < vertex_indices.size(); i++) {
    value::normal3f n;

    // http://www.terathon.com/code/tangent.html
    // Use facevarying normal at position i (not the unique vertex index)
    n[0] = fv_normals[i][0];
    n[1] = fv_normals[i][1];
    n[2] = fv_normals[i][2];

    // Validate normal: must be finite and non-zero.
    // If degenerate, generate a fallback normal so we can still produce a
    // valid tangent frame.
    float nlen = safe_vlength(n);
    if (nlen < kTangentLengthEps) {
      n = {0.0f, 1.0f, 0.0f};  // arbitrary up direction
    } else {
      float inv = 1.0f / nlen;
      n = {n[0] * inv, n[1] * inv, n[2] * inv};
    }

    value::normal3f Tn = v_tn[vertex_indices[i]];
    value::normal3f Bn = v_bn[vertex_indices[i]];

    // Gram-Schmidt orthogonalize: remove the component of Tn along n.
    float d = vdot(n, Tn);
    if (is_finite_f(d)) {
      Tn = {Tn[0] - n[0] * d, Tn[1] - n[1] * d, Tn[2] - n[2] * d};
    } else {
      Tn = {0.0f, 0.0f, 0.0f};
    }

    // Normalize with a proper epsilon to avoid amplifying near-zero noise
    Tn = safe_vnormalize(Tn);

    if (safe_vlength(Tn) < kTangentLengthEps) {
      // Degenerate tangent after Gram-Schmidt (e.g. tangent was parallel to
      // normal, or all contributing triangles had degenerate UVs).
      // Generate a fallback tangent perpendicular to the normal.
      Tn = generate_fallback_tangent(n);
    }

    // Calculate handedness: flip tangent if the frame is left-handed.
    // Guard against NaN in the dot product from degenerate binormals.
    {
      value::normal3f cross_n_t = vcross(n, Tn);
      float hand = vdot(cross_n_t, Bn);
      if (is_finite_f(hand) && hand < 0.0f) {
        Tn = {Tn[0] * -1.0f, Tn[1] * -1.0f, Tn[2] * -1.0f};
      }
    }

    // Binormal: recompute if degenerate, NaN, or too short.
    float blen = safe_vlength(Bn);
    if (blen < kTangentLengthEps) {
      Bn = vcross(n, Tn);
      Bn = safe_vnormalize(Bn);
      // If still degenerate (n and Tn parallel - shouldn't happen but guard)
      if (safe_vlength(Bn) < kTangentLengthEps) {
        Bn = generate_fallback_tangent(Tn);
      }
    }

    // Final validation: ensure output is finite.  If not, use fallbacks.
    if (!is_finite_v3(Tn)) {
      Tn = generate_fallback_tangent(n);
    }
    if (!is_finite_v3(Bn)) {
      Bn = vcross(n, Tn);
      Bn = safe_vnormalize(Bn);
      if (safe_vlength(Bn) < kTangentLengthEps || !is_finite_v3(Bn)) {
        Bn = generate_fallback_tangent(Tn);
      }
    }

    ((*tangents)[vertex_indices[i]])[0] = Tn[0];
    ((*tangents)[vertex_indices[i]])[1] = Tn[1];
    ((*tangents)[vertex_indices[i]])[2] = Tn[2];

    ((*binormals)[vertex_indices[i]])[0] = Bn[0];
    ((*binormals)[vertex_indices[i]])[1] = Bn[1];
    ((*binormals)[vertex_indices[i]])[2] = Bn[2];
  }

  (*out_vertex_indices) = vertex_indices;

  return true;
}

bool ComputeNormals(const std::vector<vec3> &vertices,
                           const std::vector<uint32_t> &faceVertexCounts,
                           const std::vector<uint32_t> &faceVertexIndices,
                           std::vector<vec3> &normals, std::string *err,
                           const bool flip_normals) {
  normals.assign(vertices.size(), {0.0f, 0.0f, 0.0f});

  size_t faceVertexIndexOffset{0};
  for (size_t f = 0; f < faceVertexCounts.size(); f++) {
    size_t nv = faceVertexCounts[f];

    if (nv < 3) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Invalid face num {} at faceVertexCounts[{}]", nv, f));
    }

    // For quad/polygon, first three vertices are used to compute face normal
    // (Assume quad/polygon plane is co-planar)
    uint32_t vidx0 = faceVertexIndices[faceVertexIndexOffset + 0];
    uint32_t vidx1 = faceVertexIndices[faceVertexIndexOffset + 1];
    uint32_t vidx2 = faceVertexIndices[faceVertexIndexOffset + 2];

    if (vidx0 >= vertices.size()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("vertexIndex0 {} exceeds vertices.size {}", vidx0, vertices.size()));
    }

    if (vidx1 >= vertices.size()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("vertexIndex1 {} exceeds vertices.size {}", vidx1, vertices.size()));
    }

    if (vidx2 >= vertices.size()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("vertexIndex2 {} exceeds vertices.size {}", vidx2, vertices.size()));
    }

    float area{0.0f};
    value::float3 Nf = GeometricNormal(vertices[vidx0], vertices[vidx1],
                                       vertices[vidx2], area);

    // Skip degenerate faces (zero-area) to prevent NaN propagation.
    if (area < 1.0e-20f) {
      faceVertexIndexOffset += nv;
      continue;
    }

    if (flip_normals) {
      Nf[0] = -Nf[0];
      Nf[1] = -Nf[1];
      Nf[2] = -Nf[2];
    }

    for (size_t v = 0; v < nv; v++) {
      uint32_t vidx = faceVertexIndices[faceVertexIndexOffset + v];
      if (vidx >= vertices.size()) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "vertexIndex exceeds vertices.size {}", vertices.size()));
      }
      normals[vidx][0] += area * Nf[0];
      normals[vidx][1] += area * Nf[1];
      normals[vidx][2] += area * Nf[2];
    }

    faceVertexIndexOffset += nv;
  }

  // Normalize accumulated normals.  Vertices with no valid face
  // contribution keep a zero vector (no arbitrary fallback).
  for (size_t v = 0; v < normals.size(); v++) {
    float len = vlength(normals[v]);
    if (len > 1.0e-20f) {
      float inv = 1.0f / len;
      normals[v] = {normals[v][0] * inv, normals[v][1] * inv, normals[v][2] * inv};
    }
  }

  return true;
}

// Read a normal without assuming its alignment or storage width.
bool ReadNormalAttribute(const VertexAttribute &attribute, size_t index, vec3 *out) {
  if (!out || attribute.elementSize != 1) return false;
  size_t width = 0;
  switch (attribute.format) {
    case VertexAttributeFormat::Vec3: width = sizeof(vec3); break;
    case VertexAttributeFormat::Char3: width = 3; break;
    case VertexAttributeFormat::Short3: width = 6; break;
    case VertexAttributeFormat::Uint: width = 4; break;
    default: return false;
  }
  const size_t stride = attribute.stride ? attribute.stride : width;
  if (stride < width || attribute.data.size() < width ||
      index > (attribute.data.size() - width) / stride) return false;
  const uint8_t *src = attribute.data.data() + index * stride;
  using namespace tangent_quantize;
  switch (attribute.format) {
    case VertexAttributeFormat::Vec3: memcpy(out, src, sizeof(vec3)); break;
    case VertexAttributeFormat::Char3: {
      PackedNormalSNorm8x3 packed;
      memcpy(&packed, src, sizeof(packed));
      unpack_normal_snorm8(packed, (*out)[0], (*out)[1], (*out)[2]);
      break;
    }
    case VertexAttributeFormat::Short3: {
      PackedNormalSNorm16x3 packed;
      memcpy(&packed, src, sizeof(packed));
      unpack_normal_snorm16(packed, (*out)[0], (*out)[1], (*out)[2]);
      break;
    }
    case VertexAttributeFormat::Uint: {
      uint32_t packed;
      memcpy(&packed, src, sizeof(packed));
      unpack_normal_1010102(packed, (*out)[0], (*out)[1], (*out)[2]);
      break;
    }
    default: return false;
  }
  return true;
}

bool MeshAttributeCornerIndex(const RenderMesh &mesh, const VertexAttribute &attribute,
                              size_t corner, size_t face, size_t *out) {
  if (!out) return false;
  if (attribute.variability == VertexVariability::Constant) { *out = 0; return true; }
  if (attribute.variability == VertexVariability::Uniform) { *out = face; return true; }
  if (!attribute.indices.empty()) {
    if (corner >= attribute.indices.size()) return false;
    *out = attribute.indices[corner];
    return true;
  }
  if (mesh.is_single_indexable || attribute.is_vertex()) {
    const auto &indices = mesh.faceVertexIndices();
    if (corner >= indices.size()) return false;
    *out = indices[corner];
  } else {
    *out = corner;
  }
  return true;
}

bool ReadNormalForTangent(const RenderMesh &mesh, size_t index, vec3 *out) {
  size_t normal_index = index;
  if (mesh.tangents.variability == VertexVariability::FaceVarying) {
    size_t face = 0;
    if (mesh.normals.variability == VertexVariability::Uniform) {
      size_t first = 0;
      const auto &counts = mesh.faceVertexCounts();
      while (face < counts.size() && index - first >= counts[face]) {
        first += counts[face++];
      }
      if (face == counts.size()) return false;
    }
    if (!MeshAttributeCornerIndex(mesh, mesh.normals, index, face, &normal_index)) return false;
  } else if (mesh.normals.variability == VertexVariability::Constant) {
    normal_index = 0;
  }
  return ReadNormalAttribute(mesh.normals, normal_index, out);
}

bool QuantizeMeshTangents(
    RenderMesh &mesh,
    MeshConverterConfig::TangentStorageFormat format) {
  using namespace tangent_quantize;

  if (format == MeshConverterConfig::TangentStorageFormat::Float3) {
    return true;  // no quantization
  }

  // Only quantize from Vec3 float format
  if (mesh.tangents.format != VertexAttributeFormat::Vec3 ||
      mesh.binormals.format != VertexAttributeFormat::Vec3) {
    return true;
  }

  size_t nT = mesh.tangents.vertex_count();
  size_t nB = mesh.binormals.vertex_count();

  if (nT == 0 || nT != nB) return true;

  const value::float3 *T =
      reinterpret_cast<const value::float3 *>(mesh.tangents.data.data());
  const value::float3 *B =
      reinterpret_cast<const value::float3 *>(mesh.binormals.data.data());
  std::vector<value::float3> tangents_v(T, T + nT);
  std::vector<value::float3> binormals_v(B, B + nT);
  std::vector<value::float3> normals_v(nT);
  for (size_t i = 0; i < nT; ++i) {
    if (!ReadNormalForTangent(mesh, i, &normals_v[i])) return false;
  }

  VertexVariability var = mesh.tangents.variability;
  std::string err;

  switch (format) {
    case MeshConverterConfig::TangentStorageFormat::Packed1010102: {
      std::vector<PackedTangent1010102> packed;
      if (!QuantizeTangents1010102(tangents_v, binormals_v, normals_v, &packed,
                                   &err))
        return false;
      mesh.tangents = PackToVertexAttribute(packed);
      break;
    }
    case MeshConverterConfig::TangentStorageFormat::PackedSNorm8: {
      std::vector<PackedTangentSNorm8x4> packed;
      if (!QuantizeTangentsSNorm8(tangents_v, binormals_v, normals_v, &packed,
                                  &err))
        return false;
      mesh.tangents = PackToVertexAttribute(packed);
      break;
    }
    case MeshConverterConfig::TangentStorageFormat::PackedFp16: {
      std::vector<PackedTangentFp16x4> packed;
      if (!QuantizeTangentsFp16(tangents_v, binormals_v, normals_v, &packed,
                                &err))
        return false;
      mesh.tangents = PackToVertexAttribute(packed);
      break;
    }
    default:
      return true;
  }

  mesh.tangents.variability = var;
  mesh.tangents.stride = 0;
  mesh.tangents.elementSize = 1;

  // Clear binormals — reconstructed in shader as cross(N, T.xyz) * T.w
  mesh.binormals.data.clear();
  mesh.binormals.data.shrink_to_fit();

  return true;
}

bool TryQuantizedNormalDedup(
    VertexAttribute &normals,
    const std::vector<uint32_t> &faceVertexIndices) {
  using namespace tangent_quantize;

  if (!normals.is_facevarying()) return false;
  if (normals.format != VertexAttributeFormat::Vec3) return false;

  size_t nFV = normals.vertex_count();
  if (nFV == 0) return false;
  if (nFV != faceVertexIndices.size()) return false;

  const value::float3 *src =
      reinterpret_cast<const value::float3 *>(normals.data.data());

  // Find max vertex index to size the output.
  uint32_t maxIdx = 0;
  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    maxIdx = (std::max)(maxIdx, faceVertexIndices[i]);
  }
  uint32_t numVerts = maxIdx + 1;

  // Per-vertex: packed normal (set on first visit) and a flag.
  std::vector<uint32_t> vertPacked(numVerts, 0);
  std::vector<uint8_t>  vertVisited(numVerts, 0);
  // Keep one representative float3 per vertex (avoids unquantize round-trip).
  std::vector<value::float3> vertNormal(numVerts, {0.0f, 0.0f, 0.0f});

  for (size_t i = 0; i < nFV; i++) {
    uint32_t vi = faceVertexIndices[i];
    uint32_t packed = pack_normal_1010102(src[i][0], src[i][1], src[i][2]);

    if (vertVisited[vi]) {
      if (vertPacked[vi] != packed) {
        return false;  // mismatch — normals are truly face-varying
      }
    } else {
      vertPacked[vi] = packed;
      vertNormal[vi] = src[i];
      vertVisited[vi] = 1;
    }
  }

  // All face-vertices agree at 10-bit resolution.  Replace with vertex data.
  VertexAttribute attr;
  attr.format = VertexAttributeFormat::Vec3;
  attr.variability = VertexVariability::Vertex;
  size_t resize_size;
  if (!safe::n_to_size<value::float3>(numVerts, &resize_size)) {
    return false;  // Error handling - return false
  }
  attr.data.resize(resize_size);
  size_t memcpy_size;
  if (!safe::mul(numVerts, sizeof(value::float3), &memcpy_size)) {
    return false;
  }
  std::memcpy(attr.data.data(), vertNormal.data(), memcpy_size);
  attr.stride = 0;
  attr.elementSize = 1;
  attr.name = normals.name;

  normals = std::move(attr);
  return true;
}

bool QuantizeMeshNormals(
    RenderMesh &mesh,
    MeshConverterConfig::NormalStorageFormat format) {
  using namespace tangent_quantize;

  if (format == MeshConverterConfig::NormalStorageFormat::Float3) {
    return true;  // no quantization
  }

  if (mesh.normals.format != VertexAttributeFormat::Vec3) {
    return true;  // already quantized or not float3
  }

  size_t nN = mesh.normals.vertex_count();
  if (nN == 0) return true;

  const value::float3 *N =
      reinterpret_cast<const value::float3 *>(mesh.normals.data.data());

  VertexVariability var = mesh.normals.variability;

  if (format == MeshConverterConfig::NormalStorageFormat::PackedSNorm8) {
    std::vector<PackedNormalSNorm8x3> packed;
    QuantizeNormalsSNorm8x3(N, nN, &packed);

    VertexAttribute attr;
    attr.format = VertexAttributeFormat::Char3;
    size_t resize_size;
    if (!safe::mul(packed.size(), size_t(3), &resize_size)) {
      return false;
    }
    attr.data.resize(resize_size);
    size_t memcpy_size;
    if (!safe::mul(packed.size(), size_t(3), &memcpy_size)) {
      return false;
    }
    std::memcpy(attr.data.data(), packed.data(), memcpy_size);

    mesh.normals = std::move(attr);
  } else if (format == MeshConverterConfig::NormalStorageFormat::PackedSNorm16) {
    std::vector<PackedNormalSNorm16x3> packed;
    QuantizeNormalsSNorm16x3(N, nN, &packed);

    VertexAttribute attr;
    attr.format = VertexAttributeFormat::Short3;
    size_t resize_size;
    if (!safe::n_to_size<PackedNormalSNorm16x3>(packed.size(), &resize_size)) {
      return false;
    }
    attr.data.resize(resize_size);
    size_t memcpy_size;
    if (!safe::n_to_size<PackedNormalSNorm16x3>(packed.size(), &memcpy_size)) {
      return false;
    }
    std::memcpy(attr.data.data(), packed.data(), memcpy_size);

    mesh.normals = std::move(attr);
  } else {
    // Packed1010102
    std::vector<uint32_t> packed;
    QuantizeNormals1010102(N, nN, &packed);

    VertexAttribute attr;
    attr.format = VertexAttributeFormat::Uint;
    size_t resize_size;
    if (!safe::n_to_size<uint32_t>(packed.size(), &resize_size)) {
      return false;
    }
    attr.data.resize(resize_size);
    size_t memcpy_size;
    if (!safe::n_to_size<uint32_t>(packed.size(), &memcpy_size)) {
      return false;
    }
    std::memcpy(attr.data.data(), packed.data(), memcpy_size);

    mesh.normals = std::move(attr);
  }

  mesh.normals.variability = var;
  mesh.normals.stride = 0;
  mesh.normals.elementSize = 1;
  mesh.normals.name = "normals";

  return true;
}

}  // namespace tydra
}  // namespace lightusd
