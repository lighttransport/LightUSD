// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// MikkTSpace tangent computation wrapper for LightUSD Tydra.
// Uses the canonical MikkTSpace library by Morten S. Mikkelsen.
// https://github.com/mmikk/MikkTSpace
//
// MikkTSpace license:
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
#pragma once

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// Only value::float2/3/4 (std::array<float, N>) are used here, so declare them
// locally instead of pulling the legacy value-type layer. Redeclaring the
// aliases is legal even where value-types.hh is also included.
#include <array>

namespace lightusd {
namespace value {
using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;
}  // namespace value
}  // namespace lightusd

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

extern "C" {
#include "external/mikktspace/mikktspace.h"
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {

namespace detail {

struct MikkTSpaceUserData {
  // All arrays are facevarying (one entry per face-vertex).
  const value::float3 *positions;
  const value::float3 *normals;
  const value::float2 *texcoords;
  const uint32_t *faceVertexCounts;
  std::vector<uint32_t> faceOffsets;  // cumulative per-face offsets
  size_t numFaces;
  size_t totalFaceVerts;  // sum of faceVertexCounts
  std::vector<value::float4> tangents_out;  // xyz=tangent, w=sign
};

// --- MikkTSpace callbacks ---

inline int mts_getNumFaces(const SMikkTSpaceContext *pContext) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  return static_cast<int>(ud->numFaces);
}

inline int mts_getNumVerticesOfFace(const SMikkTSpaceContext *pContext,
                                    const int iFace) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  return static_cast<int>(ud->faceVertexCounts[iFace]);
}

inline void mts_getPosition(const SMikkTSpaceContext *pContext, float fvPosOut[],
                             const int iFace, const int iVert) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  size_t idx = ud->faceOffsets[size_t(iFace)] + size_t(iVert);
  const auto &p = ud->positions[idx];
  fvPosOut[0] = p[0];
  fvPosOut[1] = p[1];
  fvPosOut[2] = p[2];
}

inline void mts_getNormal(const SMikkTSpaceContext *pContext, float fvNormOut[],
                           const int iFace, const int iVert) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  size_t idx = ud->faceOffsets[size_t(iFace)] + size_t(iVert);
  const auto &n = ud->normals[idx];
  fvNormOut[0] = n[0];
  fvNormOut[1] = n[1];
  fvNormOut[2] = n[2];
}

inline void mts_getTexCoord(const SMikkTSpaceContext *pContext, float fvTexcOut[],
                             const int iFace, const int iVert) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  size_t idx = ud->faceOffsets[size_t(iFace)] + size_t(iVert);
  const auto &uv = ud->texcoords[idx];
  fvTexcOut[0] = uv[0];
  fvTexcOut[1] = uv[1];
}

inline void mts_setTSpaceBasic(const SMikkTSpaceContext *pContext,
                                const float fvTangent[], const float fSign,
                                const int iFace, const int iVert) {
  auto *ud = static_cast<MikkTSpaceUserData *>(pContext->m_pUserData);
  size_t idx = ud->faceOffsets[size_t(iFace)] + size_t(iVert);
  ud->tangents_out[idx] = {fvTangent[0], fvTangent[1], fvTangent[2], fSign};
}

}  // namespace detail

/// Compute tangents and binormals using MikkTSpace.
///
/// All input arrays must be facevarying (one entry per face-vertex).
/// Output tangents and binormals are also facevarying.
///
/// For polygons with N > 4 vertices, the function pre-triangulates using fan
/// triangulation, runs MikkTSpace on the triangulated data, and maps results
/// back to the original facevarying layout by accumulating and normalizing.
///
/// @param[in] positions   Facevarying positions
/// @param[in] normals     Facevarying normals
/// @param[in] texcoords   Facevarying texcoords
/// @param[in] faceVertexCounts  Per-face vertex counts
/// @param[out] out_tangents   Facevarying tangents (vec3)
/// @param[out] out_binormals  Facevarying binormals (vec3)
/// @param[out] err  Error message on failure
/// @return true on success
inline bool ComputeTangentsMikkTSpace(
    const std::vector<value::float3> &positions,
    const std::vector<value::float3> &normals,
    const std::vector<value::float2> &texcoords,
    const std::vector<uint32_t> &faceVertexCounts,
    std::vector<value::float3> *out_tangents,
    std::vector<value::float3> *out_binormals, std::string *err) {
  if (!out_tangents || !out_binormals) {
    if (err) *err = "Output pointers are nullptr.";
    return false;
  }

  if (positions.empty() || normals.empty() || texcoords.empty()) {
    if (err) *err = "Input arrays are empty.";
    return false;
  }

  size_t totalFV = 0;
  bool needsTriangulation = false;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    totalFV += faceVertexCounts[i];
    if (faceVertexCounts[i] > 4) {
      needsTriangulation = true;
    }
    if (faceVertexCounts[i] < 3) {
      if (err) *err = "Face with fewer than 3 vertices found.";
      return false;
    }
  }

  if (positions.size() != totalFV || normals.size() != totalFV ||
      texcoords.size() != totalFV) {
    if (err) {
      *err = "Array size mismatch: positions=" + std::to_string(positions.size()) +
             " normals=" + std::to_string(normals.size()) +
             " texcoords=" + std::to_string(texcoords.size()) +
             " expected=" + std::to_string(totalFV);
    }
    return false;
  }

  if (!needsTriangulation) {
    // All faces are tris or quads — pass directly to MikkTSpace.
    detail::MikkTSpaceUserData ud;
    ud.positions = positions.data();
    ud.normals = normals.data();
    ud.texcoords = texcoords.data();
    ud.faceVertexCounts = faceVertexCounts.data();
    ud.numFaces = faceVertexCounts.size();
    ud.totalFaceVerts = totalFV;
    ud.tangents_out.resize(totalFV, {0.0f, 0.0f, 0.0f, 1.0f});

    // Build face offsets
    ud.faceOffsets.resize(faceVertexCounts.size());
    size_t offset = 0;
    for (size_t i = 0; i < faceVertexCounts.size(); i++) {
      ud.faceOffsets[i] = static_cast<uint32_t>(offset);
      offset += faceVertexCounts[i];
    }

    SMikkTSpaceInterface iface;
    memset(&iface, 0, sizeof(iface));
    iface.m_getNumFaces = detail::mts_getNumFaces;
    iface.m_getNumVerticesOfFace = detail::mts_getNumVerticesOfFace;
    iface.m_getPosition = detail::mts_getPosition;
    iface.m_getNormal = detail::mts_getNormal;
    iface.m_getTexCoord = detail::mts_getTexCoord;
    iface.m_setTSpaceBasic = detail::mts_setTSpaceBasic;

    SMikkTSpaceContext context;
    context.m_pInterface = &iface;
    context.m_pUserData = &ud;

    if (!genTangSpaceDefault(&context)) {
      if (err) *err = "MikkTSpace genTangSpaceDefault failed.";
      return false;
    }

    // Extract tangent (xyz) and compute binormal = cross(normal, tangent) * sign
    out_tangents->resize(totalFV);
    out_binormals->resize(totalFV);
    for (size_t i = 0; i < totalFV; i++) {
      const auto &t = ud.tangents_out[i];
      (*out_tangents)[i] = {t[0], t[1], t[2]};

      const auto &n = normals[i];
      float sign = t[3];
      // binormal = cross(normal, tangent) * sign
      (*out_binormals)[i] = {
          (n[1] * t[2] - n[2] * t[1]) * sign,
          (n[2] * t[0] - n[0] * t[2]) * sign,
          (n[0] * t[1] - n[1] * t[0]) * sign};
    }

    return true;
  }

  // --- Polygon triangulation path (N > 4) ---
  // Pre-triangulate all faces using fan triangulation.

  // Count total triangulated face-vertices.
  size_t triTotalFV = 0;
  size_t triNumFaces = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    uint32_t nv = faceVertexCounts[i];
    size_t numTris = size_t(nv) - 2;
    triNumFaces += numTris;
    triTotalFV += numTris * 3;
  }

  std::vector<value::float3> triPositions(triTotalFV);
  std::vector<value::float3> triNormals(triTotalFV);
  std::vector<value::float2> triTexcoords(triTotalFV);
  std::vector<uint32_t> triFVC(triNumFaces, 3);

  // Map: triIdx -> original facevarying index (for accumulation back)
  std::vector<size_t> triToOrigIdx(triTotalFV);

  size_t srcOffset = 0;
  size_t triVtxIdx = 0;
  for (size_t fi = 0; fi < faceVertexCounts.size(); fi++) {
    uint32_t nv = faceVertexCounts[fi];
    for (uint32_t t = 0; t < nv - 2; t++) {
      size_t i0 = srcOffset;
      size_t i1 = srcOffset + t + 1;
      size_t i2 = srcOffset + t + 2;

      triPositions[triVtxIdx] = positions[i0];
      triNormals[triVtxIdx] = normals[i0];
      triTexcoords[triVtxIdx] = texcoords[i0];
      triToOrigIdx[triVtxIdx] = i0;
      triVtxIdx++;

      triPositions[triVtxIdx] = positions[i1];
      triNormals[triVtxIdx] = normals[i1];
      triTexcoords[triVtxIdx] = texcoords[i1];
      triToOrigIdx[triVtxIdx] = i1;
      triVtxIdx++;

      triPositions[triVtxIdx] = positions[i2];
      triNormals[triVtxIdx] = normals[i2];
      triTexcoords[triVtxIdx] = texcoords[i2];
      triToOrigIdx[triVtxIdx] = i2;
      triVtxIdx++;
    }
    srcOffset += nv;
  }

  // Run MikkTSpace on triangulated data
  detail::MikkTSpaceUserData ud;
  ud.positions = triPositions.data();
  ud.normals = triNormals.data();
  ud.texcoords = triTexcoords.data();
  ud.faceVertexCounts = triFVC.data();
  ud.numFaces = triNumFaces;
  ud.totalFaceVerts = triTotalFV;
  ud.tangents_out.resize(triTotalFV, {0.0f, 0.0f, 0.0f, 1.0f});

  ud.faceOffsets.resize(triNumFaces);
  for (size_t i = 0; i < triNumFaces; i++) {
    ud.faceOffsets[i] = static_cast<uint32_t>(i * 3);
  }

  SMikkTSpaceInterface iface;
  memset(&iface, 0, sizeof(iface));
  iface.m_getNumFaces = detail::mts_getNumFaces;
  iface.m_getNumVerticesOfFace = detail::mts_getNumVerticesOfFace;
  iface.m_getPosition = detail::mts_getPosition;
  iface.m_getNormal = detail::mts_getNormal;
  iface.m_getTexCoord = detail::mts_getTexCoord;
  iface.m_setTSpaceBasic = detail::mts_setTSpaceBasic;

  SMikkTSpaceContext context;
  context.m_pInterface = &iface;
  context.m_pUserData = &ud;

  if (!genTangSpaceDefault(&context)) {
    if (err) *err = "MikkTSpace genTangSpaceDefault failed (triangulated path).";
    return false;
  }

  // Accumulate triangulated results back to original facevarying indices.
  // Multiple triangles from the same polygon may contribute to the same vertex.
  out_tangents->assign(totalFV, {0.0f, 0.0f, 0.0f});
  out_binormals->assign(totalFV, {0.0f, 0.0f, 0.0f});
  std::vector<uint32_t> accumCount(totalFV, 0);

  for (size_t i = 0; i < triTotalFV; i++) {
    size_t origIdx = triToOrigIdx[i];
    const auto &t = ud.tangents_out[i];
    const auto &n = triNormals[i];
    float sign = t[3];

    (*out_tangents)[origIdx][0] += t[0];
    (*out_tangents)[origIdx][1] += t[1];
    (*out_tangents)[origIdx][2] += t[2];

    float bx = (n[1] * t[2] - n[2] * t[1]) * sign;
    float by = (n[2] * t[0] - n[0] * t[2]) * sign;
    float bz = (n[0] * t[1] - n[1] * t[0]) * sign;
    (*out_binormals)[origIdx][0] += bx;
    (*out_binormals)[origIdx][1] += by;
    (*out_binormals)[origIdx][2] += bz;

    accumCount[origIdx]++;
  }

  // Normalize accumulated tangents/binormals
  for (size_t i = 0; i < totalFV; i++) {
    if (accumCount[i] > 0) {
      auto &tv = (*out_tangents)[i];
      float tlen = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
      if (tlen > 1e-8f) {
        tv[0] /= tlen;
        tv[1] /= tlen;
        tv[2] /= tlen;
      }

      auto &bv = (*out_binormals)[i];
      float blen = std::sqrt(bv[0] * bv[0] + bv[1] * bv[1] + bv[2] * bv[2]);
      if (blen > 1e-8f) {
        bv[0] /= blen;
        bv[1] /= blen;
        bv[2] /= blen;
      }
    }
  }

  return true;
}

}  // namespace tydra
}  // namespace lightusd
