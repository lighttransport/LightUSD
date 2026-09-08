// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Mesh tangent conversion

#include "render-converter.hh"
#include "safe-arithmetic.hh"
#include <cstring>
#include "mem-budget.hh"
#include "tydra/fast-mikktspace.hh"
#include "tydra/mikktspace-tangent.hh"
#include "tydra/tangent-quantize.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

bool RenderSceneConverter::ComputeVertexTangents(RenderMesh* mesh) {
  if (!mesh->is_triangulated) {
    if (!TriangulateMesh(mesh)) return false;
  }
  const size_t np = mesh->point_count();
  if (np == 0) return false;

  const bool vertex_normals =
      mesh->normals_interp == Interpolation::Vertex &&
      mesh->normals.size() == np * 3;
  const bool vertex_uvs =
      mesh->texcoords_0_interp == Interpolation::Vertex &&
      mesh->texcoords_0.size() == np * 2;

  const size_t authored_corner_count = mesh->face_vertex_indices.size();
  const size_t tri_corner_count = mesh->triangulated_indices.size();
  const bool facevarying_normals =
      mesh->normals_interp == Interpolation::FaceVarying &&
      (mesh->normals.size() == authored_corner_count * 3 ||
       mesh->normals.size() == tri_corner_count * 3);
  const bool facevarying_uvs =
      mesh->texcoords_0_interp == Interpolation::FaceVarying &&
      (mesh->texcoords_0.size() == authored_corner_count * 2 ||
       mesh->texcoords_0.size() == tri_corner_count * 2);

  if ((!vertex_normals && !facevarying_normals) ||
      (!vertex_uvs && !facevarying_uvs)) {
    return false;
  }

  // Pre-flight the temporary buffers. The corner-expanded MikkTSpace path holds
  // positions(12) + normals(12) + uvs(8) + tri_counts(~1.3) + tangents(12) +
  // binormals(12) simultaneously across the tangent call = ~58 B per
  // triangulated corner; kMikkBytesPerCorner adds margin for the fv_out(16)
  // phase, which overlaps only partially because the dead inputs are released
  // first (see below). The old estimate of 64 both understated the peak and
  // probed a single block rather than the sum. The Lengyel path is ~40B per
  // point. A failed probe skips tangents for this mesh (they are optional)
  // instead of abort()ing the module under -fno-exceptions.
  constexpr size_t kMikkBytesPerCorner = 80;
  size_t probe_bytes = 0;
  bool probe_overflow = false;
  if (config_.mesh.tangent_method ==
          MeshConfig::TangentComputationMethod::Lengyel &&
      vertex_normals && vertex_uvs) {
    probe_overflow = !safe::mul3(np, size_t(10), sizeof(float), &probe_bytes);
  } else {
    probe_overflow =
        !safe::mul(tri_corner_count, kMikkBytesPerCorner, &probe_bytes);
  }
  // ProbeAlloc only asks "can this ONE block be allocated"; the cumulative
  // guard additionally refuses when the scene as a whole is at the cap.
  if (probe_overflow ||
      !ProbeAlloc(probe_bytes) ||
      BudgetWouldExceed(probe_bytes, "tangent generation")) {
    AddWarning("Out of memory computing tangents for mesh '" +
                        mesh->prim_path + "'; tangents skipped");
    return false;
  }

  std::vector<float> tan(np * 3, 0.0f);
  std::vector<float> bit(np * 3, 0.0f);
  const size_t ntris = mesh->triangulated_indices.size() / 3;
  if (config_.mesh.tangent_method == MeshConfig::TangentComputationMethod::Lengyel &&
      vertex_normals && vertex_uvs) {
  for (size_t t = 0; t < ntris; ++t) {
    const uint32_t i0 = mesh->triangulated_indices[t * 3 + 0];
    const uint32_t i1 = mesh->triangulated_indices[t * 3 + 1];
    const uint32_t i2 = mesh->triangulated_indices[t * 3 + 2];
    if (i0 >= np || i1 >= np || i2 >= np) continue;
    // Copy out, never `&chunked[i]` + offset: an xyz/uv run straddles the
    // chunk boundary (see ChunkedArray::read_n).
    float p0[3], p1[3], p2[3], u0[2], u1[2], u2[2];
    mesh->points.read3(i0 * 3, p0);
    mesh->points.read3(i1 * 3, p1);
    mesh->points.read3(i2 * 3, p2);
    mesh->texcoords_0.read2(i0 * 2, u0);
    mesh->texcoords_0.read2(i1 * 2, u1);
    mesh->texcoords_0.read2(i2 * 2, u2);
    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    const float du1 = u1[0] - u0[0], dv1 = u1[1] - u0[1];
    const float du2 = u2[0] - u0[0], dv2 = u2[1] - u0[1];
    const float det = du1 * dv2 - du2 * dv1;
    const float r = (std::fabs(det) > 1e-12f) ? 1.0f / det : 0.0f;
    const float sdir[3] = {(dv2 * e1[0] - dv1 * e2[0]) * r,
                           (dv2 * e1[1] - dv1 * e2[1]) * r,
                           (dv2 * e1[2] - dv1 * e2[2]) * r};
    const float tdir[3] = {(du1 * e2[0] - du2 * e1[0]) * r,
                           (du1 * e2[1] - du2 * e1[1]) * r,
                           (du1 * e2[2] - du2 * e1[2]) * r};
    for (uint32_t vi : {i0, i1, i2}) {
      tan[vi * 3 + 0] += sdir[0];
      tan[vi * 3 + 1] += sdir[1];
      tan[vi * 3 + 2] += sdir[2];
      bit[vi * 3 + 0] += tdir[0];
      bit[vi * 3 + 1] += tdir[1];
      bit[vi * 3 + 2] += tdir[2];
    }
  }

  std::vector<float> out_tan(np * 4, 0.0f);
  for (size_t v = 0; v < np; ++v) {
    float n[3];
    mesh->normals.read3(v * 3, n);
    const float* tv = &tan[v * 3];
    // Gram-Schmidt orthogonalize t against n.
    const float ndt = n[0] * tv[0] + n[1] * tv[1] + n[2] * tv[2];
    float tx = tv[0] - n[0] * ndt;
    float ty = tv[1] - n[1] * ndt;
    float tz = tv[2] - n[2] * ndt;
    const float len = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (len > 1e-12f) {
      tx /= len; ty /= len; tz /= len;
    } else {
      tx = 1.0f; ty = 0.0f; tz = 0.0f;
    }
    // Handedness: sign of dot(cross(n, t), bitangent).
    const float* bv = &bit[v * 3];
    const float cx = n[1] * tz - n[2] * ty;
    const float cy = n[2] * tx - n[0] * tz;
    const float cz = n[0] * ty - n[1] * tx;
    const float w = (cx * bv[0] + cy * bv[1] + cz * bv[2]) < 0.0f ? -1.0f : 1.0f;
    out_tan[v * 4 + 0] = tx;
    out_tan[v * 4 + 1] = ty;
    out_tan[v * 4 + 2] = tz;
    out_tan[v * 4 + 3] = w;
  }
  mesh->tangents.clear();
  mesh->tangents.append(out_tan.data(), out_tan.size());
  mesh->tangents_interp = Interpolation::Vertex;
  return true;
  }

  std::vector<value::float3> fv_positions(tri_corner_count);
  std::vector<value::float3> fv_normals(tri_corner_count);
  std::vector<value::float2> fv_uvs(tri_corner_count);
  std::vector<uint32_t> tri_counts(ntris, 3);

  const bool tri_corner_remap =
      mesh->triangulated_face_vertex_indices.size() == tri_corner_count;

  for (size_t c = 0; c < tri_corner_count; ++c) {
    const uint32_t point_id = mesh->triangulated_indices[c];
    if (point_id >= np) return false;

    const size_t authored_corner =
        tri_corner_remap ? mesh->triangulated_face_vertex_indices[c] : c;

    const size_t p3 = size_t(point_id) * 3;
    fv_positions[c] = {mesh->points[p3 + 0], mesh->points[p3 + 1],
                       mesh->points[p3 + 2]};

    size_t nidx = 0;
    if (vertex_normals) {
      nidx = size_t(point_id);
    } else if (mesh->normals.size() == tri_corner_count * 3) {
      nidx = c;
    } else {
      if (authored_corner >= authored_corner_count) return false;
      nidx = authored_corner;
    }
    const size_t n3 = nidx * 3;
    if (n3 + 2 >= mesh->normals.size()) return false;
    fv_normals[c] = {mesh->normals[n3 + 0], mesh->normals[n3 + 1],
                     mesh->normals[n3 + 2]};

    size_t uvidx = 0;
    if (vertex_uvs) {
      uvidx = size_t(point_id);
    } else if (mesh->texcoords_0.size() == tri_corner_count * 2) {
      uvidx = c;
    } else {
      if (authored_corner >= authored_corner_count) return false;
      uvidx = authored_corner;
    }
    const size_t uv2 = uvidx * 2;
    if (uv2 + 1 >= mesh->texcoords_0.size()) return false;
    fv_uvs[c] = {mesh->texcoords_0[uv2 + 0], mesh->texcoords_0[uv2 + 1]};
  }

  std::vector<value::float3> fv_tangents;
  std::vector<value::float3> fv_binormals;
  std::string tangent_error;
  bool tangent_ok = false;

  switch (config_.mesh.tangent_method) {
    case MeshConfig::TangentComputationMethod::MikkTSpace:
      tangent_ok = ::lightusd::tydra::ComputeTangentsMikkTSpace(
          fv_positions, fv_normals, fv_uvs, tri_counts, &fv_tangents,
          &fv_binormals, &tangent_error);
      break;
    case MeshConfig::TangentComputationMethod::FastMikkTSpace:
      tangent_ok = ::lightusd::tydra::fast_mikkt::ComputeTangentsFastMikkTSpace(
          fv_positions, fv_normals, fv_uvs, tri_counts, &fv_tangents,
          &fv_binormals, &tangent_error);
      break;
    case MeshConfig::TangentComputationMethod::Hybrid: {
      ::lightusd::tydra::fast_mikkt::HybridStats stats = {};
      tangent_ok = ::lightusd::tydra::fast_mikkt::ComputeTangentsHybrid(
          fv_positions, fv_normals, fv_uvs, tri_counts, &fv_tangents,
          &fv_binormals, &stats, &tangent_error);
      break;
    }
    case MeshConfig::TangentComputationMethod::Lengyel:
      // Face-varying Lengyel is intentionally not duplicated here; Hybrid is
      // the O(n) seam-aware fallback for non-vertex data.
      tangent_ok = ::lightusd::tydra::fast_mikkt::ComputeTangentsHybrid(
          fv_positions, fv_normals, fv_uvs, tri_counts, &fv_tangents,
          &fv_binormals, nullptr, &tangent_error);
      break;
  }

  if (!tangent_ok || fv_tangents.size() != tri_corner_count ||
      fv_binormals.size() != tri_corner_count) {
    if (!tangent_error.empty()) {
      AddWarning("Tangent computation failed for mesh '" +
                          mesh->prim_path + "': " + tangent_error);
    }
    return false;
  }

  // Positions, UVs and the face-size list are dead once the tangent call
  // returns; release them BEFORE allocating fv_out so the two do not stack
  // (24 B/corner, i.e. ~720 MB on a 30M-corner mesh).
  { std::vector<value::float3>().swap(fv_positions); }
  { std::vector<value::float2>().swap(fv_uvs); }
  { std::vector<uint32_t>().swap(tri_counts); }

  std::vector<float> fv_out(tri_corner_count * 4, 0.0f);
  for (size_t i = 0; i < tri_corner_count; ++i) {
    const value::float3& n = fv_normals[i];
    const value::float3& t = fv_tangents[i];
    const value::float3& b = fv_binormals[i];
    const float cx = n[1] * t[2] - n[2] * t[1];
    const float cy = n[2] * t[0] - n[0] * t[2];
    const float cz = n[0] * t[1] - n[1] * t[0];
    const float sign =
        (cx * b[0] + cy * b[1] + cz * b[2]) < 0.0f ? -1.0f : 1.0f;
    fv_out[i * 4 + 0] = t[0];
    fv_out[i * 4 + 1] = t[1];
    fv_out[i * 4 + 2] = t[2];
    fv_out[i * 4 + 3] = sign;
  }

  // Same again: normals/tangents/binormals are consumed by the loop above and
  // must not stay resident alongside vertex_out below (36 B/corner).
  { std::vector<value::float3>().swap(fv_normals); }
  { std::vector<value::float3>().swap(fv_tangents); }
  { std::vector<value::float3>().swap(fv_binormals); }

  if (vertex_normals && vertex_uvs) {
    std::vector<float> vertex_out(np * 4, 0.0f);
    std::vector<uint8_t> seen(np, 0);
    for (size_t c = 0; c < tri_corner_count; ++c) {
      const uint32_t point_id = mesh->triangulated_indices[c];
      if (point_id >= np || seen[point_id]) continue;
      seen[point_id] = 1;
      vertex_out[size_t(point_id) * 4 + 0] = fv_out[c * 4 + 0];
      vertex_out[size_t(point_id) * 4 + 1] = fv_out[c * 4 + 1];
      vertex_out[size_t(point_id) * 4 + 2] = fv_out[c * 4 + 2];
      vertex_out[size_t(point_id) * 4 + 3] = fv_out[c * 4 + 3];
    }
    mesh->tangents.clear();
    mesh->tangents.append(vertex_out.data(), vertex_out.size());
    mesh->tangents_interp = Interpolation::Vertex;
  } else {
    mesh->tangents.clear();
    mesh->tangents.append(fv_out.data(), fv_out.size());
    mesh->tangents_interp = Interpolation::FaceVarying;
  }
  return true;
}


} } }  // namespace lightusd::tydra::next
