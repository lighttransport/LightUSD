// SPDX-License-Identifier: Apache-2.0 AND Zlib
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Optimized MikkTSpace-compatible tangent computation.
// This is an altered version based on the MikkTSpace algorithm.
//
// Original MikkTSpace by Morten S. Mikkelsen:
// https://github.com/mmikk/MikkTSpace
//
//  Copyright (C) 2011 by Morten S. Mikkelsen
//
//  This software is provided 'as-is', without any express or implied
//  warranty.  In no event will the authors be held liable for any damages
//  arising from the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software
//     in a product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//  2. Altered source versions must be plainly marked as such, and must not be
//     misrepresented as being the original software.
//  3. This notice may not be removed or altered from any source distribution.
//
// This is an altered source version: a reimplementation of the MikkTSpace
// algorithm with the following optimizations:
//
// 1. Direct array access (no callback overhead)
// 2. Hash-based vertex welding using all 8 floats (pos3+norm3+uv2)
// 3. Hash-based edge lookup for O(1) neighbor finding
// 4. For default 180° threshold: O(n) tangent averaging per group
//    (the original is O(n^2) per group doing subgroup formation that
//     is completely unnecessary at 180°)
// 5. Single-allocation strategy (one large buffer instead of many small mallocs)
//
// The output is MikkTSpace-compatible: same welding, grouping, and
// angle-weighted averaging semantics. Slight floating-point differences
// may occur due to operation reordering.
//
#pragma once

#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <numeric>

#ifdef LIGHTUSD_ENABLE_THREAD
#include <thread>
#endif

// Only value::float2 / float3 (std::array<float, N>) are used here, so declare
// them locally instead of pulling the legacy value-type layer. Redeclaring the
// aliases is legal even where value-types.hh is also included.
#include <array>

namespace lightusd {
namespace value {
using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
}  // namespace value
}  // namespace lightusd
#include "fast-math.hh"

namespace lightusd {
namespace tydra {

namespace fast_mikkt {

// ---------------------------------------------------------------------------
// Vec3/Vec2 helpers (inline, no overhead)
// ---------------------------------------------------------------------------
struct Vec3 {
  float x, y, z;
  Vec3() : x(0), y(0), z(0) {}
  Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};

static inline Vec3 vadd(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 vscale(float s, Vec3 v) { return {s*v.x, s*v.y, s*v.z}; }
static inline float vdot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float vlength2(Vec3 v) { return v.x*v.x + v.y*v.y + v.z*v.z; }
static inline float vlength(Vec3 v) { return std::sqrt(vlength2(v)); }
static inline bool vnotzero(Vec3 v) { return vlength2(v) > 1e-20f; }
static inline Vec3 vnormalize(Vec3 v) {
  float len = vlength(v);
  return (len > 1e-20f) ? vscale(1.0f / len, v) : Vec3{0,0,0};
}
// Fast normalize using rsqrt approximation (~fp16 precision)
static inline Vec3 vnormalize_fast(Vec3 v) {
  float len2 = vlength2(v);
  if (len2 < 1e-20f) return {0, 0, 0};
  float inv = fast_math::fast_rsqrt(len2);
  return {v.x * inv, v.y * inv, v.z * inv};
}
static inline bool veq(Vec3 a, Vec3 b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}
static inline bool notzero(float f) {
  return std::fabs(f) > 1e-20f;
}

// ---------------------------------------------------------------------------
// Parallel for: splits [begin, end) across std::threads when threading is on.
// Falls back to serial loop when LIGHTUSD_ENABLE_THREAD is not defined or
// the workload is too small to justify thread overhead.
// ---------------------------------------------------------------------------
template <typename Func>
static inline void tangent_parallel_for(size_t begin, size_t end, const Func &func) {
#ifdef LIGHTUSD_ENABLE_THREAD
  size_t n = end - begin;
  if (n > 8192) {
    unsigned hw = std::thread::hardware_concurrency();
    unsigned nThreads = (hw > 1) ? std::min(hw, 16u) : 1;
    if (nThreads > 1) {
      size_t chunk = (n + nThreads - 1) / nThreads;
      std::vector<std::thread> threads;
      threads.reserve(nThreads - 1);
      for (unsigned t = 1; t < nThreads; t++) {
        size_t lo = begin + t * chunk;
        size_t hi = std::min(lo + chunk, end);
        if (lo < hi) {
          threads.emplace_back([lo, hi, &func]() {
            for (size_t i = lo; i < hi; i++) func(i);
          });
        }
      }
      size_t hi0 = std::min(begin + chunk, end);
      for (size_t i = begin; i < hi0; i++) func(i);
      for (auto &t : threads) t.join();
      return;
    }
  }
#endif
  for (size_t i = begin; i < end; i++) func(i);
}

// ---------------------------------------------------------------------------
// Per-triangle info (compact: 48 bytes vs 72+ in original)
// ---------------------------------------------------------------------------
struct TriInfo {
  Vec3 vOs, vOt;           // normalized tangent/bitangent directions
  float fMagS, fMagT;      // original magnitudes
  int neighbors[3];        // adjacent triangle per edge (-1 if none)
  int groupId[3];          // group assignment per vertex (-1 = unassigned)
  int orgFace;             // original face number (for quad pairing)
  uint8_t flags;           // ORIENT_PRESERVING | GROUP_WITH_ANY | MARK_DEGENERATE
  uint8_t vertNum[3];      // face-vertex numbering (for quads: 0-3)
};

enum TriFlags : uint8_t {
  ORIENT_PRESERVING = 1,
  GROUP_WITH_ANY    = 2,
  MARK_DEGENERATE   = 4,
};

// ---------------------------------------------------------------------------
// Group info
// ---------------------------------------------------------------------------
struct Group {
  int vertexRep;        // welded vertex representative
  int firstTri;         // offset into group triangle list
  int numTris;          // number of triangles in this group
  bool orientPreserving;
};

// ---------------------------------------------------------------------------
// Hash for vertex welding: hash all 8 floats
// ---------------------------------------------------------------------------
struct VertexKey {
  float data[8]; // pos(3) + norm(3) + uv(2)
  bool operator==(const VertexKey &o) const {
    return std::memcmp(data, o.data, sizeof(data)) == 0;
  }
};

struct VertexKeyHash {
  size_t operator()(const VertexKey &k) const {
    // FNV-1a on raw bytes
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(k.data);
    for (size_t i = 0; i < sizeof(k.data); i++) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
    return static_cast<size_t>(h);
  }
};

// ---------------------------------------------------------------------------
// Edge hash for O(1) neighbor lookup
// ---------------------------------------------------------------------------
struct EdgeKey {
  int v0, v1; // welded vertex indices, v0 < v1
  bool operator==(const EdgeKey &o) const { return v0 == o.v0 && v1 == o.v1; }
};

struct EdgeKeyHash {
  size_t operator()(const EdgeKey &k) const {
    return size_t(k.v0) * 2654435761ULL ^ size_t(k.v1) * 40503ULL;
  }
};

// Edge record: which triangle and which edge index
struct EdgeRecord {
  int triIdx;
  int edgeIdx;
};

// ---------------------------------------------------------------------------
// Tangent space output
// ---------------------------------------------------------------------------
struct TSpace {
  Vec3 vOs, vOt;
  float fMagS, fMagT;
  bool orient;
  int counter; // for quad averaging
};

// ---------------------------------------------------------------------------
// Main optimized computation
// ---------------------------------------------------------------------------

/// Compute tangents using optimized MikkTSpace algorithm.
///
/// Input arrays must be facevarying (one entry per face-vertex).
/// Only triangles and quads are supported (consistent with MikkTSpace).
///
/// @param[in] positions   Facevarying positions (float3)
/// @param[in] normals     Facevarying normals (float3)
/// @param[in] texcoords   Facevarying texcoords (float2)
/// @param[in] faceVertexCounts  Per-face vertex counts (3 or 4)
/// @param[out] out_tangents   Facevarying tangents (float3)
/// @param[out] out_binormals  Facevarying binormals (float3)
/// @param[out] err  Error message on failure
/// @return true on success
inline bool ComputeTangentsFastMikkTSpace(
    const value::float3 *positions,
    const value::float3 *normals,
    const value::float2 *texcoords,
    const uint32_t *faceVertexCounts,
    size_t numFaces,
    size_t totalFaceVerts,
    std::vector<value::float3> *out_tangents,
    std::vector<value::float3> *out_binormals,
    std::string *err) {

  if (!out_tangents || !out_binormals) {
    if (err) *err = "Output pointers are nullptr.";
    return false;
  }
  if (totalFaceVerts == 0) {
    if (err) *err = "Empty mesh.";
    return false;
  }

  // =========================================================================
  // Phase 1: Count triangles, build face-vertex → (face,vert) mapping
  // =========================================================================
  int nrTriangles = 0;
  for (size_t f = 0; f < numFaces; f++) {
    if (faceVertexCounts[f] == 3) nrTriangles += 1;
    else if (faceVertexCounts[f] == 4) nrTriangles += 2;
    else {
      if (err) *err = "Only triangles and quads supported (face " +
                       std::to_string(f) + " has " +
                       std::to_string(faceVertexCounts[f]) + " verts).";
      return false;
    }
  }
  if (nrTriangles == 0) {
    if (err) *err = "No valid faces.";
    return false;
  }

  // =========================================================================
  // Phase 2: Build initial triangle list + TriInfo
  // =========================================================================
  // triList[t*3+i] = facevarying index for triangle t, vertex i
  std::vector<int> triList(static_cast<size_t>(nrTriangles) * 3);
  std::vector<TriInfo> triInfos(static_cast<size_t>(nrTriangles));

  {
    int dstTri = 0;
    size_t fvOff = 0;
    for (size_t f = 0; f < numFaces; f++) {
      uint32_t nv = faceVertexCounts[f];
      if (nv == 3) {
        int i0 = int(fvOff), i1 = int(fvOff + 1), i2 = int(fvOff + 2);
        triList[dstTri * 3 + 0] = i0;
        triList[dstTri * 3 + 1] = i1;
        triList[dstTri * 3 + 2] = i2;
        triInfos[dstTri].orgFace = int(f);
        triInfos[dstTri].vertNum[0] = 0;
        triInfos[dstTri].vertNum[1] = 1;
        triInfos[dstTri].vertNum[2] = 2;
        triInfos[dstTri].flags = 0;
        for (int k = 0; k < 3; k++) {
          triInfos[dstTri].neighbors[k] = -1;
          triInfos[dstTri].groupId[k] = -1;
        }
        dstTri++;
      } else { // nv == 4
        int i0 = int(fvOff), i1 = int(fvOff + 1), i2 = int(fvOff + 2), i3 = int(fvOff + 3);
        // Split quad by shorter diagonal (order-independent)
        const auto &p0 = positions[i0], &p1 = positions[i1];
        const auto &p2 = positions[i2], &p3 = positions[i3];
        float d02 = (p0[0]-p2[0])*(p0[0]-p2[0]) + (p0[1]-p2[1])*(p0[1]-p2[1]) + (p0[2]-p2[2])*(p0[2]-p2[2]);
        float d13 = (p1[0]-p3[0])*(p1[0]-p3[0]) + (p1[1]-p3[1])*(p1[1]-p3[1]) + (p1[2]-p3[2])*(p1[2]-p3[2]);

        if (d02 <= d13) {
          // Split 0-1-2 and 0-2-3
          triList[dstTri * 3 + 0] = i0;
          triList[dstTri * 3 + 1] = i1;
          triList[dstTri * 3 + 2] = i2;
          triInfos[dstTri].orgFace = int(f);
          triInfos[dstTri].vertNum[0] = 0; triInfos[dstTri].vertNum[1] = 1; triInfos[dstTri].vertNum[2] = 2;
          triInfos[dstTri].flags = 0;
          for (int k = 0; k < 3; k++) { triInfos[dstTri].neighbors[k] = -1; triInfos[dstTri].groupId[k] = -1; }
          dstTri++;

          triList[dstTri * 3 + 0] = i0;
          triList[dstTri * 3 + 1] = i2;
          triList[dstTri * 3 + 2] = i3;
          triInfos[dstTri].orgFace = int(f);
          triInfos[dstTri].vertNum[0] = 0; triInfos[dstTri].vertNum[1] = 2; triInfos[dstTri].vertNum[2] = 3;
          triInfos[dstTri].flags = 0;
          for (int k = 0; k < 3; k++) { triInfos[dstTri].neighbors[k] = -1; triInfos[dstTri].groupId[k] = -1; }
          dstTri++;
        } else {
          // Split 0-1-3 and 1-2-3
          triList[dstTri * 3 + 0] = i0;
          triList[dstTri * 3 + 1] = i1;
          triList[dstTri * 3 + 2] = i3;
          triInfos[dstTri].orgFace = int(f);
          triInfos[dstTri].vertNum[0] = 0; triInfos[dstTri].vertNum[1] = 1; triInfos[dstTri].vertNum[2] = 3;
          triInfos[dstTri].flags = 0;
          for (int k = 0; k < 3; k++) { triInfos[dstTri].neighbors[k] = -1; triInfos[dstTri].groupId[k] = -1; }
          dstTri++;

          triList[dstTri * 3 + 0] = i1;
          triList[dstTri * 3 + 1] = i2;
          triList[dstTri * 3 + 2] = i3;
          triInfos[dstTri].orgFace = int(f);
          triInfos[dstTri].vertNum[0] = 1; triInfos[dstTri].vertNum[1] = 2; triInfos[dstTri].vertNum[2] = 3;
          triInfos[dstTri].flags = 0;
          for (int k = 0; k < 3; k++) { triInfos[dstTri].neighbors[k] = -1; triInfos[dstTri].groupId[k] = -1; }
          dstTri++;
        }
      }
      fvOff += nv;
    }
  }

  // =========================================================================
  // Phase 3: Vertex welding via flat open-addressing hash table
  // =========================================================================
  // Uses power-of-2 table with linear probing for cache-friendly lookups.
  std::vector<int> origTriList = triList; // save original for data access
  int nextWeldId = 0;

  {
    size_t nEntries = triList.size();
    // Table size = next power of 2 >= 2*nEntries (load factor ~0.5)
    size_t tableSize = 1;
    while (tableSize < nEntries * 2) tableSize <<= 1;
    size_t mask = tableSize - 1;

    // Flat table: each slot stores (hash, fvi_representative, weld_id)
    // -1 = empty slot
    struct Slot { size_t hash; int fvi; int weldId; };
    std::vector<Slot> table(tableSize, {0, -1, -1});

    VertexKeyHash hasher;
    for (size_t i = 0; i < nEntries; i++) {
      int fvi = triList[i];
      VertexKey key;
      key.data[0] = positions[fvi][0]; key.data[1] = positions[fvi][1]; key.data[2] = positions[fvi][2];
      key.data[3] = normals[fvi][0];   key.data[4] = normals[fvi][1];   key.data[5] = normals[fvi][2];
      key.data[6] = texcoords[fvi][0]; key.data[7] = texcoords[fvi][1];

      size_t h = hasher(key);
      size_t idx = h & mask;

      // Linear probe
      while (true) {
        if (table[idx].fvi < 0) {
          // Empty slot — insert
          table[idx] = {h, fvi, nextWeldId};
          triList[i] = nextWeldId;
          nextWeldId++;
          break;
        }
        if (table[idx].hash == h) {
          // Check actual equality
          int ofvi = table[idx].fvi;
          if (positions[fvi][0] == positions[ofvi][0] &&
              positions[fvi][1] == positions[ofvi][1] &&
              positions[fvi][2] == positions[ofvi][2] &&
              normals[fvi][0] == normals[ofvi][0] &&
              normals[fvi][1] == normals[ofvi][1] &&
              normals[fvi][2] == normals[ofvi][2] &&
              texcoords[fvi][0] == texcoords[ofvi][0] &&
              texcoords[fvi][1] == texcoords[ofvi][1]) {
            triList[i] = table[idx].weldId;
            break;
          }
        }
        idx = (idx + 1) & mask;
      }
    }
  }

  // =========================================================================
  // Phase 4: Mark degenerate triangles
  // =========================================================================
  int nrDegen = 0;
  for (int t = 0; t < nrTriangles; t++) {
    int i0 = triList[t*3+0], i1 = triList[t*3+1], i2 = triList[t*3+2];
    if (i0 == i1 || i0 == i2 || i1 == i2) {
      triInfos[t].flags |= MARK_DEGENERATE;
      nrDegen++;
    }
  }

  // Separate degenerate triangles: move good ones to front.
  // We need to maintain relative order within good and degenerate sets.
  if (nrDegen > 0) {
    std::vector<int> goodIdx, degenIdx;
    goodIdx.reserve(size_t(nrTriangles - nrDegen));
    degenIdx.reserve(size_t(nrDegen));
    for (int t = 0; t < nrTriangles; t++) {
      if (triInfos[t].flags & MARK_DEGENERATE)
        degenIdx.push_back(t);
      else
        goodIdx.push_back(t);
    }

    std::vector<int> newTriList(triList.size());
    std::vector<int> newOrigTriList(origTriList.size());
    std::vector<TriInfo> newTriInfos(triInfos.size());

    int dst = 0;
    for (int idx : goodIdx) {
      newTriList[dst*3+0] = triList[idx*3+0];
      newTriList[dst*3+1] = triList[idx*3+1];
      newTriList[dst*3+2] = triList[idx*3+2];
      newOrigTriList[dst*3+0] = origTriList[idx*3+0];
      newOrigTriList[dst*3+1] = origTriList[idx*3+1];
      newOrigTriList[dst*3+2] = origTriList[idx*3+2];
      newTriInfos[dst] = triInfos[idx];
      dst++;
    }
    for (int idx : degenIdx) {
      newTriList[dst*3+0] = triList[idx*3+0];
      newTriList[dst*3+1] = triList[idx*3+1];
      newTriList[dst*3+2] = triList[idx*3+2];
      newOrigTriList[dst*3+0] = origTriList[idx*3+0];
      newOrigTriList[dst*3+1] = origTriList[idx*3+1];
      newOrigTriList[dst*3+2] = origTriList[idx*3+2];
      newTriInfos[dst] = triInfos[idx];
      dst++;
    }
    triList = std::move(newTriList);
    origTriList = std::move(newOrigTriList);
    triInfos = std::move(newTriInfos);
  }

  int nrGoodTris = nrTriangles - nrDegen;

  // =========================================================================
  // Phase 5: Compute per-triangle tangent derivatives (threaded)
  // =========================================================================
  tangent_parallel_for(size_t(0), size_t(nrGoodTris), [&](size_t ti) {
    int t = int(ti);
    int fi0 = origTriList[t*3+0], fi1 = origTriList[t*3+1], fi2 = origTriList[t*3+2];

    Vec3 v1{positions[fi0][0], positions[fi0][1], positions[fi0][2]};
    Vec3 v2{positions[fi1][0], positions[fi1][1], positions[fi1][2]};
    Vec3 v3{positions[fi2][0], positions[fi2][1], positions[fi2][2]};

    float t1x = texcoords[fi0][0], t1y = texcoords[fi0][1];
    float t2x = texcoords[fi1][0], t2y = texcoords[fi1][1];
    float t3x = texcoords[fi2][0], t3y = texcoords[fi2][1];

    float t21x = t2x - t1x, t21y = t2y - t1y;
    float t31x = t3x - t1x, t31y = t3y - t1y;
    Vec3 d1 = vsub(v2, v1), d2 = vsub(v3, v1);

    float signedAreax2 = t21x * t31y - t21y * t31x;

    Vec3 vOs = vsub(vscale(t31y, d1), vscale(t21y, d2));
    Vec3 vOt = vadd(vscale(-t31x, d1), vscale(t21x, d2));

    triInfos[t].flags |= (signedAreax2 > 0 ? ORIENT_PRESERVING : 0);
    triInfos[t].flags |= GROUP_WITH_ANY; // assume bad initially

    if (notzero(signedAreax2)) {
      float absArea = std::fabs(signedAreax2);
      float lenOs2 = vlength2(vOs), lenOt2 = vlength2(vOt);
      float fS = (triInfos[t].flags & ORIENT_PRESERVING) ? 1.0f : -1.0f;

      if (lenOs2 > 1e-20f) {
        float invLenOs = fast_math::fast_rsqrt(lenOs2);
        triInfos[t].vOs = vscale(fS * invLenOs, vOs);
        triInfos[t].fMagS = 1.0f / (invLenOs * absArea);
      }
      if (lenOt2 > 1e-20f) {
        float invLenOt = fast_math::fast_rsqrt(lenOt2);
        triInfos[t].vOt = vscale(fS * invLenOt, vOt);
        triInfos[t].fMagT = 1.0f / (invLenOt * absArea);
      }

      if (notzero(triInfos[t].fMagS) && notzero(triInfos[t].fMagT))
        triInfos[t].flags &= ~GROUP_WITH_ANY;
    }
  });

  // Force quad orientation consistency
  for (int t = 0; t < nrGoodTris - 1; t++) {
    if (triInfos[t].orgFace == triInfos[t+1].orgFace) {
      bool degA = (triInfos[t].flags & MARK_DEGENERATE) != 0;
      bool degB = (triInfos[t+1].flags & MARK_DEGENERATE) != 0;
      if (!degA && !degB) {
        bool orA = (triInfos[t].flags & ORIENT_PRESERVING) != 0;
        bool orB = (triInfos[t+1].flags & ORIENT_PRESERVING) != 0;
        if (orA != orB) {
          // Use the triangle with larger tex area, or the first non-GROUP_WITH_ANY
          bool chooseFirst = (triInfos[t+1].flags & GROUP_WITH_ANY) != 0;
          if (!chooseFirst) {
            // Compare tex areas
            int *idx0 = &origTriList[t*3], *idx1 = &origTriList[(t+1)*3];
            auto texArea = [&](int *idx) {
              float u0 = texcoords[idx[0]][0], v0 = texcoords[idx[0]][1];
              float u1 = texcoords[idx[1]][0], v1 = texcoords[idx[1]][1];
              float u2 = texcoords[idx[2]][0], v2 = texcoords[idx[2]][1];
              return std::fabs((u1-u0)*(v2-v0) - (v1-v0)*(u2-u0));
            };
            chooseFirst = texArea(idx0) >= texArea(idx1);
          }
          int src = chooseFirst ? t : t+1;
          int dst = chooseFirst ? t+1 : t;
          triInfos[dst].flags &= ~ORIENT_PRESERVING;
          triInfos[dst].flags |= (triInfos[src].flags & ORIENT_PRESERVING);
        }
      }
      t++; // skip pair
    }
  }

  // =========================================================================
  // Phase 6: Build neighbors via sorted edge array
  // =========================================================================
  {
    // Build flat edge array, sort, then pair up matching edges.
    struct SortEdge {
      int v0, v1;   // welded vertex indices, v0 <= v1
      int triIdx;
      int edgeIdx;
    };
    size_t nEdges = size_t(nrGoodTris) * 3;
    std::vector<SortEdge> edges(nEdges);

    for (int t = 0; t < nrGoodTris; t++) {
      for (int e = 0; e < 3; e++) {
        int wv0 = triList[t*3 + e];
        int wv1 = triList[t*3 + ((e+1)%3)];
        auto &se = edges[size_t(t)*3 + size_t(e)];
        se.v0 = std::min(wv0, wv1);
        se.v1 = std::max(wv0, wv1);
        se.triIdx = t;
        se.edgeIdx = e;
      }
    }

    // Sort by (v0, v1)
    std::sort(edges.begin(), edges.end(), [](const SortEdge &a, const SortEdge &b) {
      if (a.v0 != b.v0) return a.v0 < b.v0;
      return a.v1 < b.v1;
    });

    // Pair up consecutive matching edges
    for (size_t i = 0; i + 1 < nEdges; i++) {
      if (edges[i].v0 == edges[i+1].v0 && edges[i].v1 == edges[i+1].v1) {
        triInfos[edges[i].triIdx].neighbors[edges[i].edgeIdx] = edges[i+1].triIdx;
        triInfos[edges[i+1].triIdx].neighbors[edges[i+1].edgeIdx] = edges[i].triIdx;
        i++; // skip the paired edge
      }
    }
  }

  // =========================================================================
  // Phase 7: Build groups via flood fill (same semantics as MikkTSpace)
  // =========================================================================
  std::vector<Group> groups;
  groups.reserve(size_t(nrGoodTris)); // typically fewer groups than triangles
  std::vector<int> groupTriBuf;
  groupTriBuf.reserve(size_t(nrGoodTris) * 3);

  // Stack-based flood fill (avoid recursion for large meshes)
  std::vector<int> floodStack;

  for (int t = 0; t < nrGoodTris; t++) {
    for (int v = 0; v < 3; v++) {
      if (triInfos[t].groupId[v] >= 0) continue; // already assigned

      int weldVert = triList[t*3+v];
      bool orient = (triInfos[t].flags & ORIENT_PRESERVING) != 0;

      int gid = int(groups.size());
      Group g;
      g.vertexRep = weldVert;
      g.firstTri = int(groupTriBuf.size());
      g.numTris = 0;
      g.orientPreserving = orient;

      // Flood fill from this triangle vertex
      floodStack.clear();
      floodStack.push_back(t);

      while (!floodStack.empty()) {
        int ct = floodStack.back();
        floodStack.pop_back();

        // Find which local vertex matches weldVert
        int ci = -1;
        if (triList[ct*3+0] == weldVert) ci = 0;
        else if (triList[ct*3+1] == weldVert) ci = 1;
        else if (triList[ct*3+2] == weldVert) ci = 2;
        if (ci < 0) continue;

        // Already assigned to this group?
        if (triInfos[ct].groupId[ci] == gid) continue;
        // Assigned to another group?
        if (triInfos[ct].groupId[ci] >= 0) continue;

        // Handle GROUP_WITH_ANY: first group determines orientation
        if (triInfos[ct].flags & GROUP_WITH_ANY) {
          if (triInfos[ct].groupId[0] < 0 &&
              triInfos[ct].groupId[1] < 0 &&
              triInfos[ct].groupId[2] < 0) {
            triInfos[ct].flags &= ~ORIENT_PRESERVING;
            triInfos[ct].flags |= (orient ? ORIENT_PRESERVING : 0);
          }
        }

        // Check orientation match
        bool triOrient = (triInfos[ct].flags & ORIENT_PRESERVING) != 0;
        if (triOrient != orient) continue;

        // Add to group
        triInfos[ct].groupId[ci] = gid;
        groupTriBuf.push_back(ct);
        g.numTris++;

        // Enqueue neighbors that share this welded vertex
        // Left neighbor of edge (ci)
        int nL = triInfos[ct].neighbors[ci];
        if (nL >= 0) floodStack.push_back(nL);
        // Right neighbor of edge (ci-1)
        int nR = triInfos[ct].neighbors[ci > 0 ? ci-1 : 2];
        if (nR >= 0) floodStack.push_back(nR);
      }

      groups.push_back(g);
    }
  }

  // =========================================================================
  // Phase 8: Compute tangent spaces per group
  // (OPTIMIZED: with default 180° threshold, ALL triangles in a group
  //  form one subgroup. No O(n^2) subgroup formation needed.)
  // =========================================================================
  // Allocate output tangent space array (one per facevarying vertex)
  std::vector<TSpace> tspaces(totalFaceVerts);
  for (auto &ts : tspaces) {
    ts.vOs = {1,0,0}; ts.vOt = {0,1,0};
    ts.fMagS = 1; ts.fMagT = 1;
    ts.orient = true; ts.counter = 0;
  }

  // Compute face offsets for mapping triInfo → tspace index
  std::vector<uint32_t> faceOffsets(numFaces);
  {
    uint32_t off = 0;
    for (size_t f = 0; f < numFaces; f++) {
      faceOffsets[f] = off;
      off += faceVertexCounts[f];
    }
  }

  for (size_t gi = 0; gi < groups.size(); gi++) {
    const Group &grp = groups[gi];
    if (grp.numTris == 0) continue;

    // EvalTspace: angle-weighted accumulation for all triangles in group
    Vec3 accOs{0,0,0}, accOt{0,0,0};
    float accMagS = 0, accMagT = 0, accAngle = 0;

    for (int ti = 0; ti < grp.numTris; ti++) {
      int t = groupTriBuf[grp.firstTri + ti];

      if (triInfos[t].flags & GROUP_WITH_ANY) continue;

      // Find which vertex in this triangle matches the group's representative
      int ci = -1;
      if (triList[t*3+0] == grp.vertexRep) ci = 0;
      else if (triList[t*3+1] == grp.vertexRep) ci = 1;
      else if (triList[t*3+2] == grp.vertexRep) ci = 2;
      if (ci < 0) continue;

      int fvi = origTriList[t*3+ci];
      Vec3 n{normals[fvi][0], normals[fvi][1], normals[fvi][2]};

      // Project face tangent onto vertex normal plane
      // Use fast normalize (~fp16 precision, rsqrt-based)
      Vec3 vOs = vsub(triInfos[t].vOs, vscale(vdot(n, triInfos[t].vOs), n));
      Vec3 vOt = vsub(triInfos[t].vOt, vscale(vdot(n, triInfos[t].vOt), n));
      if (vnotzero(vOs)) vOs = vnormalize_fast(vOs);
      if (vnotzero(vOt)) vOt = vnormalize_fast(vOt);

      // Compute angle at this vertex
      int fi_prev = origTriList[t*3 + (ci > 0 ? ci-1 : 2)];
      int fi_next = origTriList[t*3 + (ci < 2 ? ci+1 : 0)];
      Vec3 p0{positions[fi_prev][0], positions[fi_prev][1], positions[fi_prev][2]};
      Vec3 p1{positions[fvi][0], positions[fvi][1], positions[fvi][2]};
      Vec3 p2{positions[fi_next][0], positions[fi_next][1], positions[fi_next][2]};

      Vec3 ev1 = vsub(p0, p1), ev2 = vsub(p2, p1);
      // Project edges onto normal plane
      ev1 = vsub(ev1, vscale(vdot(n, ev1), n));
      ev2 = vsub(ev2, vscale(vdot(n, ev2), n));
      if (vnotzero(ev1)) ev1 = vnormalize_fast(ev1);
      if (vnotzero(ev2)) ev2 = vnormalize_fast(ev2);

      float cosAngle = vdot(ev1, ev2);
      cosAngle = cosAngle < -1.0f ? -1.0f : (cosAngle > 1.0f ? 1.0f : cosAngle);
      float angle = fast_math::fast_acos(cosAngle);

      accOs = vadd(accOs, vscale(angle, vOs));
      accOt = vadd(accOt, vscale(angle, vOt));
      accMagS += angle * triInfos[t].fMagS;
      accMagT += angle * triInfos[t].fMagT;
      accAngle += angle;
    }

    // Normalize
    if (vnotzero(accOs)) accOs = vnormalize(accOs);
    if (vnotzero(accOt)) accOt = vnormalize(accOt);
    if (accAngle > 0) {
      accMagS /= accAngle;
      accMagT /= accAngle;
    }

    TSpace ts;
    ts.vOs = accOs;
    ts.vOt = accOt;
    ts.fMagS = accMagS;
    ts.fMagT = accMagT;
    ts.orient = grp.orientPreserving;
    ts.counter = 1;

    // Write to all face-vertices in this group
    for (int ti = 0; ti < grp.numTris; ti++) {
      int t = groupTriBuf[grp.firstTri + ti];
      int ci = -1;
      if (triList[t*3+0] == grp.vertexRep) ci = 0;
      else if (triList[t*3+1] == grp.vertexRep) ci = 1;
      else if (triList[t*3+2] == grp.vertexRep) ci = 2;
      if (ci < 0) continue;

      int orgFace = triInfos[t].orgFace;
      int vertInFace = triInfos[t].vertNum[ci];
      int tsIdx = int(faceOffsets[orgFace]) + vertInFace;

      if (tspaces[tsIdx].counter == 0) {
        tspaces[tsIdx] = ts;
      } else {
        // Average for quad shared vertex
        TSpace &existing = tspaces[tsIdx];
        existing.vOs = vnormalize(vadd(existing.vOs, ts.vOs));
        existing.vOt = vnormalize(vadd(existing.vOt, ts.vOt));
        existing.fMagS = 0.5f * (existing.fMagS + ts.fMagS);
        existing.fMagT = 0.5f * (existing.fMagT + ts.fMagT);
        existing.counter = 2;
        existing.orient = grp.orientPreserving;
      }
    }
  }

  // =========================================================================
  // Phase 9: Degenerate epilogue — copy tspace from good neighbors
  // =========================================================================
  for (int t = nrGoodTris; t < nrTriangles; t++) {
    // Try to find a good triangle sharing a welded vertex
    for (int v = 0; v < 3; v++) {
      int weldVert = triList[t*3+v];
      int orgFace = triInfos[t].orgFace;
      int vertInFace = triInfos[t].vertNum[v];
      int tsIdx = int(faceOffsets[orgFace]) + vertInFace;

      if (tspaces[tsIdx].counter > 0) continue; // already set

      // Search good triangles for matching welded vertex
      bool found = false;
      for (int gt = 0; gt < nrGoodTris && !found; gt++) {
        for (int gv = 0; gv < 3; gv++) {
          if (triList[gt*3+gv] == weldVert) {
            int gOrgFace = triInfos[gt].orgFace;
            int gVertInFace = triInfos[gt].vertNum[gv];
            int gtsIdx = int(faceOffsets[gOrgFace]) + gVertInFace;
            if (tspaces[gtsIdx].counter > 0) {
              tspaces[tsIdx] = tspaces[gtsIdx];
              found = true;
              break;
            }
          }
        }
      }
    }
  }

  // =========================================================================
  // Phase 10: Output (threaded)
  // =========================================================================
  out_tangents->resize(totalFaceVerts);
  out_binormals->resize(totalFaceVerts);

  tangent_parallel_for(size_t(0), totalFaceVerts, [&](size_t fvIdx) {
    const TSpace &ts = tspaces[fvIdx];
    (*out_tangents)[fvIdx] = {ts.vOs.x, ts.vOs.y, ts.vOs.z};

    const auto &n = normals[fvIdx];
    float sign = ts.orient ? 1.0f : -1.0f;
    (*out_binormals)[fvIdx] = {
      (n[1] * ts.vOs.z - n[2] * ts.vOs.y) * sign,
      (n[2] * ts.vOs.x - n[0] * ts.vOs.z) * sign,
      (n[0] * ts.vOs.y - n[1] * ts.vOs.x) * sign
    };
  });

  return true;
}

/// Convenience wrapper matching ComputeTangentsMikkTSpace signature.
inline bool ComputeTangentsFastMikkTSpace(
    const std::vector<value::float3> &positions,
    const std::vector<value::float3> &normals,
    const std::vector<value::float2> &texcoords,
    const std::vector<uint32_t> &faceVertexCounts,
    std::vector<value::float3> *out_tangents,
    std::vector<value::float3> *out_binormals,
    std::string *err) {

  size_t totalFV = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++)
    totalFV += faceVertexCounts[i];

  return ComputeTangentsFastMikkTSpace(
      positions.data(), normals.data(), texcoords.data(),
      faceVertexCounts.data(), faceVertexCounts.size(), totalFV,
      out_tangents, out_binormals, err);
}

// ===========================================================================
// Hybrid Lengyel + MikkTSpace-quality averaging
//
// Algorithm:
//   1. Lengyel base pass: per-triangle tangent/bitangent accumulation, O(N)
//   2. 8-float welding (pos+norm+uv): same welding as MikkTSpace, O(N)
//   3. Within each weld group: simple average of Lengyel tangents
//      (no acos, no edge sort, no flood fill needed)
//   4. Gram-Schmidt orthogonalization
//
// Key insight: MikkTSpace's quality comes from averaging tangents across
// faces sharing a vertex (same pos+norm+uv). The angle-weighting adds
// marginal improvement. By doing the same welding + simple averaging
// on top of Lengyel's per-triangle tangent, we get near-MikkTSpace
// quality without the expensive O(N log N) edge sort or O(N) acos.
//
// Memory: O(N) — tan1/tan2 + weld hash table + group lists
// Speed: ~2-4x slower than Lengyel, ~5-10x faster than MikkTSpace
// Quality: close to MikkTSpace (missing only angle weighting)
// ===========================================================================

struct HybridStats {
  size_t working_memory_bytes;
  size_t num_weld_groups;
  size_t total_vertices;
};

inline bool ComputeTangentsHybrid(
    const value::float3 *positions,
    const value::float3 *normals,
    const value::float2 *texcoords,
    const uint32_t *faceVertexCounts,
    size_t numFaces,
    size_t totalFaceVerts,
    std::vector<value::float3> *out_tangents,
    std::vector<value::float3> *out_binormals,
    HybridStats *stats,
    std::string *err) {

  if (!out_tangents || !out_binormals) {
    if (err) *err = "Output pointers are nullptr.";
    return false;
  }
  if (totalFaceVerts == 0) {
    if (err) *err = "Empty mesh.";
    return false;
  }

  size_t memTrack = 0;

  // Precompute face offsets for parallel access
  std::vector<uint32_t> earlyFaceOffsets(numFaces);
  {
    uint32_t off = 0;
    for (size_t f = 0; f < numFaces; f++) {
      earlyFaceOffsets[f] = off;
      off += faceVertexCounts[f];
    }
  }

  // =========================================================================
  // Phase 1: Per-face tangent derivatives (threaded)
  // =========================================================================
  // For each face, compute the normalized tangent/bitangent direction
  // (vOs, vOt) exactly as MikkTSpace does: without dividing by UV area,
  // normalized to unit length, with orientation sign applied.
  // Store per face-vertex (all vertices of a face get the same direction).
  std::vector<Vec3> fvOs(totalFaceVerts, {0,0,0});
  std::vector<Vec3> fvOt(totalFaceVerts, {0,0,0});
  memTrack += totalFaceVerts * sizeof(Vec3) * 2;

  // =========================================================================
  // Phase 2: Compute per-face-vertex UV orientation (merged with Phase 1)
  // =========================================================================
  std::vector<int8_t> orientSign(totalFaceVerts, 0);
  memTrack += totalFaceVerts * sizeof(int8_t);

  tangent_parallel_for(size_t(0), numFaces, [&](size_t f) {
    uint32_t nv = faceVertexCounts[f];
    if (nv < 3) return;
    size_t fvOff = earlyFaceOffsets[f];

    // Phase 1: tangent derivatives
    size_t i0 = fvOff, i1 = fvOff + 1, i2 = fvOff + 2;

    Vec3 p0{positions[i0][0], positions[i0][1], positions[i0][2]};
    Vec3 p1{positions[i1][0], positions[i1][1], positions[i1][2]};
    Vec3 p2{positions[i2][0], positions[i2][1], positions[i2][2]};

    float t21x = texcoords[i1][0] - texcoords[i0][0];
    float t21y = texcoords[i1][1] - texcoords[i0][1];
    float t31x = texcoords[i2][0] - texcoords[i0][0];
    float t31y = texcoords[i2][1] - texcoords[i0][1];

    Vec3 d1 = vsub(p1, p0), d2 = vsub(p2, p0);
    float signedAreax2 = t21x * t31y - t21y * t31x;

    Vec3 vOs = vsub(vscale(t31y, d1), vscale(t21y, d2));
    Vec3 vOt = vadd(vscale(-t31x, d1), vscale(t21x, d2));

    float fS = signedAreax2 > 0.0f ? 1.0f : -1.0f;

    float lenOs2 = vlength2(vOs);
    float lenOt2 = vlength2(vOt);
    if (lenOs2 > 1e-20f) vOs = vscale(fS * fast_math::fast_rsqrt(lenOs2), vOs);
    else vOs = {0,0,0};
    if (lenOt2 > 1e-20f) vOt = vscale(fS * fast_math::fast_rsqrt(lenOt2), vOt);
    else vOt = {0,0,0};

    // Phase 2: UV orientation
    int8_t sign = signedAreax2 > 0.0f ? int8_t(1) : int8_t(-1);

    for (uint32_t v = 0; v < nv; v++) {
      fvOs[fvOff + v] = vOs;
      fvOt[fvOff + v] = vOt;
      orientSign[fvOff + v] = sign;
    }
  });

  // =========================================================================
  // Phase 3: Welding (pos3+norm3+uv2+orient) — same grouping as MikkTSpace
  // =========================================================================
  // Welds on all 8 attribute floats PLUS orientation sign.
  // This ensures mirrored UV faces don't get mixed with non-mirrored.
  std::vector<int> weldGroup(totalFaceVerts);
  int numGroups = 0;

  {
    size_t tableSize = 1;
    while (tableSize < totalFaceVerts * 2) tableSize <<= 1;
    size_t mask = tableSize - 1;

    struct Slot { size_t hash; int fvi; int groupId; };
    std::vector<Slot> table(tableSize, {0, -1, -1});
    memTrack += tableSize * sizeof(Slot);

    VertexKeyHash hasher;
    for (size_t i = 0; i < totalFaceVerts; i++) {
      VertexKey key;
      key.data[0] = positions[i][0]; key.data[1] = positions[i][1]; key.data[2] = positions[i][2];
      key.data[3] = normals[i][0];   key.data[4] = normals[i][1];   key.data[5] = normals[i][2];
      key.data[6] = texcoords[i][0]; key.data[7] = texcoords[i][1];

      size_t h = hasher(key);
      // Mix orientation sign into hash for proper separation
      h ^= (orientSign[i] > 0) ? 0x9e3779b97f4a7c15ULL : 0x517cc1b727220a95ULL;
      size_t idx = h & mask;

      while (true) {
        if (table[idx].fvi < 0) {
          table[idx] = {h, int(i), numGroups};
          weldGroup[i] = numGroups;
          numGroups++;
          break;
        }
        if (table[idx].hash == h) {
          int o = table[idx].fvi;
          if (positions[i][0] == positions[o][0] &&
              positions[i][1] == positions[o][1] &&
              positions[i][2] == positions[o][2] &&
              normals[i][0] == normals[o][0] &&
              normals[i][1] == normals[o][1] &&
              normals[i][2] == normals[o][2] &&
              texcoords[i][0] == texcoords[o][0] &&
              texcoords[i][1] == texcoords[o][1] &&
              orientSign[i] == orientSign[o]) {
            weldGroup[i] = table[idx].groupId;
            break;
          }
        }
        idx = (idx + 1) & mask;
      }
    }
  }

  memTrack += totalFaceVerts * sizeof(int);  // weldGroup

  // =========================================================================
  // Phase 3: Build fvi→face mapping + face offsets
  // =========================================================================
  std::vector<uint32_t> faceOffsets(numFaces);
  std::vector<uint32_t> fviToFace(totalFaceVerts);
  {
    uint32_t off = 0;
    for (size_t f = 0; f < numFaces; f++) {
      faceOffsets[f] = off;
      for (uint32_t v = 0; v < faceVertexCounts[f]; v++) {
        fviToFace[off + v] = uint32_t(f);
      }
      off += faceVertexCounts[f];
    }
  }
  memTrack += numFaces * sizeof(uint32_t) + totalFaceVerts * sizeof(uint32_t);

  // =========================================================================
  // Phase 4: Angle-weighted accumulation per weld group
  // =========================================================================
  // Exactly like MikkTSpace Phase 8: project per-face tangent derivative
  // onto vertex normal plane, normalize, then weight by vertex angle.
  // Uses fast_acos. Skips edge sort, neighbor finding, flood fill.
  std::vector<Vec3> grpOs(size_t(numGroups), {0,0,0});
  std::vector<Vec3> grpOt(size_t(numGroups), {0,0,0});
  memTrack += size_t(numGroups) * sizeof(Vec3) * 2;

  for (size_t i = 0; i < totalFaceVerts; i++) {
    int g = weldGroup[i];
    Vec3 n{normals[i][0], normals[i][1], normals[i][2]};

    // Compute face angle at this vertex
    uint32_t face = fviToFace[i];
    uint32_t fStart = faceOffsets[face];
    uint32_t nv = faceVertexCounts[face];
    int localIdx = int(i) - int(fStart);
    int prevFvi = int(fStart) + ((localIdx + int(nv) - 1) % int(nv));
    int nextFvi = int(fStart) + ((localIdx + 1) % int(nv));

    Vec3 pC{positions[i][0], positions[i][1], positions[i][2]};
    Vec3 pP{positions[prevFvi][0], positions[prevFvi][1], positions[prevFvi][2]};
    Vec3 pN{positions[nextFvi][0], positions[nextFvi][1], positions[nextFvi][2]};

    Vec3 ev1 = vsub(pP, pC), ev2 = vsub(pN, pC);
    // Project edges onto normal plane (same as MikkTSpace)
    ev1 = vsub(ev1, vscale(vdot(n, ev1), n));
    ev2 = vsub(ev2, vscale(vdot(n, ev2), n));
    if (vnotzero(ev1)) ev1 = vnormalize_fast(ev1);
    if (vnotzero(ev2)) ev2 = vnormalize_fast(ev2);

    float cosA = vdot(ev1, ev2);
    cosA = cosA < -1.0f ? -1.0f : (cosA > 1.0f ? 1.0f : cosA);
    float angle = fast_math::fast_acos(cosA);

    // Project per-face tangent derivative onto normal plane and normalize
    // (matches MikkTSpace Phase 8 exactly)
    Vec3 vOs = vsub(fvOs[i], vscale(vdot(n, fvOs[i]), n));
    Vec3 vOt = vsub(fvOt[i], vscale(vdot(n, fvOt[i]), n));
    if (vnotzero(vOs)) vOs = vnormalize_fast(vOs);
    if (vnotzero(vOt)) vOt = vnormalize_fast(vOt);

    // Angle-weighted accumulation
    grpOs[size_t(g)] = vadd(grpOs[size_t(g)], vscale(angle, vOs));
    grpOt[size_t(g)] = vadd(grpOt[size_t(g)], vscale(angle, vOt));
  }

  // =========================================================================
  // Phase 5: Output tangent/binormal per face-vertex (threaded)
  // =========================================================================
  out_tangents->resize(totalFaceVerts);
  out_binormals->resize(totalFaceVerts);
  memTrack += totalFaceVerts * sizeof(value::float3) * 2;

  tangent_parallel_for(size_t(0), totalFaceVerts, [&](size_t i) {
    int g = weldGroup[i];
    Vec3 accOs = grpOs[size_t(g)];
    Vec3 accOt = grpOt[size_t(g)];

    if (vnotzero(accOs)) accOs = vnormalize(accOs);
    if (vnotzero(accOt)) accOt = vnormalize(accOt);

    bool orient = (orientSign[i] > 0);
    float sign = orient ? 1.0f : -1.0f;

    (*out_tangents)[i] = {accOs.x, accOs.y, accOs.z};

    const auto &n = normals[i];
    (*out_binormals)[i] = {
      (n[1] * accOs.z - n[2] * accOs.y) * sign,
      (n[2] * accOs.x - n[0] * accOs.z) * sign,
      (n[0] * accOs.y - n[1] * accOs.x) * sign
    };
  });

  if (stats) {
    stats->working_memory_bytes = memTrack;
    stats->num_weld_groups = size_t(numGroups);
    stats->total_vertices = totalFaceVerts;
  }

  return true;
}

/// Convenience wrapper for Hybrid method.
inline bool ComputeTangentsHybrid(
    const std::vector<value::float3> &positions,
    const std::vector<value::float3> &normals,
    const std::vector<value::float2> &texcoords,
    const std::vector<uint32_t> &faceVertexCounts,
    std::vector<value::float3> *out_tangents,
    std::vector<value::float3> *out_binormals,
    HybridStats *stats,
    std::string *err) {

  size_t totalFV = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++)
    totalFV += faceVertexCounts[i];

  return ComputeTangentsHybrid(
      positions.data(), normals.data(), texcoords.data(),
      faceVertexCounts.data(), faceVertexCounts.size(), totalFV,
      out_tangents, out_binormals, stats, err);
}

}  // namespace fast_mikkt
}  // namespace tydra
}  // namespace lightusd
