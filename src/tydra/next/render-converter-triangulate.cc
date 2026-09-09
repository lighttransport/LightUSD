// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Mesh triangulation and normals

#include "render-converter.hh"
#include "safe-arithmetic.hh"
#include <cstring>
#include "external/mapbox/earcut/earcut.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
namespace {
constexpr size_t kMaxTempAllocBytes = 256u * 1024u * 1024u;
constexpr size_t kMaxTriangulationCornerCount = 150'000'000u;
constexpr uint32_t kEarcutMaxVertices = 16384;
bool WouldOverflowSizeMul(size_t a, size_t b) {
  return a != 0 && b > std::numeric_limits<size_t>::max() / a;
}
size_t SaturatingMul(size_t a, size_t b) {
  return a && b > (std::numeric_limits<size_t>::max() / a)
             ? std::numeric_limits<size_t>::max() : a * b;
}
}  // namespace

bool RenderSceneConverter::TriangulateMesh(RenderMesh* mesh) {
  if (mesh->face_vertex_counts.empty()) return false;

  // Triangulation roughly doubles the index storage (triangulated_indices +
  // triangulated_face_vertex_indices, 4 bytes each per corner) on top of the
  // authored corners. The per-mesh kMaxTempAllocBytes check below bounds ONE
  // mesh; this bounds the scene.
  if (BudgetWouldExceed(
          SaturatingMul(mesh->face_vertex_indices.size(), 2 * sizeof(uint32_t)),
          "triangulation")) {
    AddWarning("Mesh '" + mesh->prim_path +
                        "' not triangulated: memory budget reached");
    return false;
  }

  mesh->triangulated_indices.clear();  // re-entry / failure-path hardening
  mesh->triangulated_face_vertex_indices.clear();

  // Check if already triangulated
  bool all_triangles = true;
  for (size_t i = 0; i < mesh->face_vertex_counts.size(); ++i) {
    if (mesh->face_vertex_counts[i] != 3) {
      all_triangles = false;
      break;
    }
  }

  if (all_triangles && !mesh->left_handed && mesh->hole_faces.empty()) {
    // Just copy indices; corner remap is identity, one triangle per face.
    mesh->face_triangle_offsets.resize(mesh->face_vertex_counts.size() + 1);
    for (size_t f = 0; f <= mesh->face_vertex_counts.size(); ++f) {
      mesh->face_triangle_offsets[f] = static_cast<uint32_t>(f);
    }
    const size_t n = mesh->face_vertex_indices.size();
    if (WouldOverflowSizeMul(n, sizeof(uint32_t)) ||
        (n * sizeof(uint32_t)) > kMaxTempAllocBytes * 4u) {
      AddWarning("Mesh '" + mesh->prim_path +
                          "' triangulated index allocation too large; skipping");
      return false;
    }
    if (!mesh->triangulated_indices.resize(n) ||
        !mesh->triangulated_face_vertex_indices.resize(n)) {
      AddWarning("Out of memory triangulating mesh '" +
                          mesh->prim_path + "'");
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      mesh->triangulated_indices.mutable_at(i) = mesh->face_vertex_indices[i];
      mesh->triangulated_face_vertex_indices.mutable_at(i) = static_cast<uint32_t>(i);
    }
    mesh->is_triangulated = true;
    return true;
  }

  size_t tri_count = 0;
  for (size_t i = 0; i < mesh->face_vertex_counts.size(); ++i) {
    uint32_t nverts = mesh->face_vertex_counts[i];
    if (nverts >= 3) {
      size_t next_tri_count = 0;
      if (!safe::add(tri_count, size_t(nverts - 2), &next_tri_count)) {
        AddWarning("Mesh '" + mesh->prim_path +
                            "' triangle count overflows size_t; skipping");
        return false;
      }
      tri_count = next_tri_count;
    }
  }
  size_t tri_corner_count = 0;
  if (!safe::mul(tri_count, size_t(3), &tri_corner_count)) {
    AddWarning("Mesh '" + mesh->prim_path +
                        "' triangulated corner count overflows size_t; skipping");
    return false;
  }
  if (tri_corner_count >= kMaxTriangulationCornerCount) {
    AddWarning("Mesh '" + mesh->prim_path +
                        "' has too many triangulated corners (" +
                        std::to_string(tri_corner_count) +
                        "); skipping");
    return false;
  }
  if (WouldOverflowSizeMul(tri_corner_count, sizeof(uint32_t)) ||
      (tri_corner_count * sizeof(uint32_t)) > kMaxTempAllocBytes * 4u) {
    AddWarning("Mesh '" + mesh->prim_path +
                        "' triangulated index allocation too large; skipping");
    return false;
  }

  if (!mesh->triangulated_indices.reserve(tri_corner_count) ||
      !mesh->triangulated_face_vertex_indices.reserve(tri_corner_count)) {
    AddWarning("Out of memory triangulating mesh '" +
                        mesh->prim_path + "'");
    return false;
  }
  mesh->face_triangle_offsets.assign(mesh->face_vertex_counts.size() + 1, 0);
  size_t idx_offset = 0;
  // Hoisted out of the per-face loop: constructing the nested vector inside it
  // cost two heap allocations per n-gon (1M allocations for a 500k-n-gon mesh).
  // clear() keeps the ring's capacity across faces.
  using EarcutPoint2 = std::array<double, 2>;
  std::vector<std::vector<EarcutPoint2>> earcut_polygon(1);

  const uint32_t point_count = static_cast<uint32_t>(mesh->point_count());
  size_t oob_faces = 0;

  for (size_t f = 0; f < mesh->face_vertex_counts.size(); ++f) {
    mesh->face_triangle_offsets[f] =
        static_cast<uint32_t>(mesh->triangulated_indices.size() / 3);
    const uint32_t nverts = mesh->face_vertex_counts[f];
    if (idx_offset + nverts > mesh->face_vertex_indices.size()) return false;

    // Bounds-check the face's VERTEX INDICES, not just the corner range.
    // Everything downstream indexes mesh->points by these values -- the quad
    // shorter-diagonal test, the earcut Newell normal and projection -- and
    // they land in triangulated_indices, which ComputeVertexNormals/Tangents
    // later dereference.
    //
    // This is defense-in-depth, NOT a fix for a reachable bug: the ConvertMesh
    // path runs SanitizeMeshTopology first, which already drops out-of-range
    // faces (verified -- disabling this check does not trip ASan on a mesh
    // whose faceVertexIndices point past the points array). What it buys is a
    // LOCAL guarantee: TriangulateMesh is also entered from
    // ConvertGeomPrimitive/ConvertBoundsProxy and from the self-triangulation
    // in ComputeVertexNormals/ComputeVertexTangents, none of which sanitize,
    // so today's safety there rests on those callers happening to build
    // self-consistent topology. Its two sibling functions already validate
    // their own indices; this makes the triangulator consistent with them.
    // An out-of-range face contributes zero triangles, exactly like a hole.
    bool face_in_range = true;
    for (uint32_t i = 0; i < nverts; ++i) {
      if (mesh->face_vertex_indices[idx_offset + i] >= point_count) {
        face_in_range = false;
        break;
      }
    }
    if (!face_in_range) ++oob_faces;

    const bool is_hole = std::binary_search(mesh->hole_faces.begin(),
                                            mesh->hole_faces.end(),
                                            static_cast<uint32_t>(f));
    if (nverts >= 3 && !is_hole && face_in_range) {
      auto emit_triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (mesh->left_handed) std::swap(b, c);
        const uint32_t corners[3] = {a, b, c};
        for (uint32_t corner : corners) {
          mesh->triangulated_indices.push_back(
              mesh->face_vertex_indices[idx_offset + corner]);
          mesh->triangulated_face_vertex_indices.push_back(
              static_cast<uint32_t>(idx_offset + corner));
        }
      };

      if (nverts == 4) {
        // Split the quad along the SHORTER diagonal (legacy parity): better
        // triangle quality, and non-planar / concave-ish quads render
        // correctly. A tie (e.g. a planar rectangle) keeps the classic
        // 0-2 fan split.
        const uint32_t i0 = mesh->face_vertex_indices[idx_offset + 0];
        const uint32_t i1 = mesh->face_vertex_indices[idx_offset + 1];
        const uint32_t i2 = mesh->face_vertex_indices[idx_offset + 2];
        const uint32_t i3 = mesh->face_vertex_indices[idx_offset + 3];
        auto dist_sq = [&](uint32_t a, uint32_t b) -> float {
          const float dx = mesh->points[size_t(a) * 3 + 0] -
                           mesh->points[size_t(b) * 3 + 0];
          const float dy = mesh->points[size_t(a) * 3 + 1] -
                           mesh->points[size_t(b) * 3 + 1];
          const float dz = mesh->points[size_t(a) * 3 + 2] -
                           mesh->points[size_t(b) * 3 + 2];
          return dx * dx + dy * dy + dz * dz;
        };
        if (dist_sq(i1, i3) < dist_sq(i0, i2)) {
          // Diagonal 1-3: triangles (0,1,3) and (1,2,3).
          emit_triangle(0, 1, 3);
          emit_triangle(1, 2, 3);
        } else {
          // Diagonal 0-2: triangles (0,1,2) and (0,2,3).
          emit_triangle(0, 1, 2);
          emit_triangle(0, 2, 3);
        }
        idx_offset += nverts;
        continue;
      }

      bool used_earcut = false;
      if (config_.mesh.triangulation_method ==
              MeshConfig::TriangulationMethod::Earcut &&
          nverts > 4) {
        if (nverts > kEarcutMaxVertices) {
          // Extremely large polygons are safer with fan triangulation in this
          // converter to avoid temporary O(nverts) geometry explosions in
          // earcut allocation paths.
          used_earcut = false;
        } else {
        using Point2 = EarcutPoint2;
        std::vector<std::vector<Point2>>& polygon = earcut_polygon;
        polygon[0].clear();
        polygon[0].reserve(nverts);

        // Newell normal chooses the projection plane with the largest area,
        // keeping concave and non-axis-aligned polygons stable.
        double normal[3] = {0.0, 0.0, 0.0};
        for (uint32_t i = 0; i < nverts; ++i) {
          const uint32_t ia = mesh->face_vertex_indices[idx_offset + i];
          const uint32_t ib =
              mesh->face_vertex_indices[idx_offset + ((i + 1) % nverts)];
          const size_t a = static_cast<size_t>(ia) * 3;
          const size_t b = static_cast<size_t>(ib) * 3;
          normal[0] += (mesh->points[a + 1] - mesh->points[b + 1]) *
                       (mesh->points[a + 2] + mesh->points[b + 2]);
          normal[1] += (mesh->points[a + 2] - mesh->points[b + 2]) *
                       (mesh->points[a] + mesh->points[b]);
          normal[2] += (mesh->points[a] - mesh->points[b]) *
                       (mesh->points[a + 1] + mesh->points[b + 1]);
        }
        int drop_axis = 0;
        if (std::fabs(normal[1]) > std::fabs(normal[drop_axis])) drop_axis = 1;
        if (std::fabs(normal[2]) > std::fabs(normal[drop_axis])) drop_axis = 2;
        for (uint32_t i = 0; i < nverts; ++i) {
          const uint32_t vertex = mesh->face_vertex_indices[idx_offset + i];
          const size_t p = static_cast<size_t>(vertex) * 3;
          if (drop_axis == 0) {
            polygon[0].push_back({mesh->points[p + 1], mesh->points[p + 2]});
          } else if (drop_axis == 1) {
            polygon[0].push_back({mesh->points[p], mesh->points[p + 2]});
          } else {
            polygon[0].push_back({mesh->points[p], mesh->points[p + 1]});
          }
        }
        const std::vector<uint32_t> local =
            mapbox::earcut<uint32_t>(polygon);
        if (!local.empty() && (local.size() % 3) == 0) {
          used_earcut = true;
          // earcut emits every triangle in ONE fixed 2D orientation
          // regardless of the input ring winding. The triangles must follow
          // the AUTHORED ring winding (so their 3D orientation matches the
          // face); compare the projected ring's signed area against the
          // orientation of the first non-degenerate output triangle and flip
          // when they disagree. (The previous unconditional reverse flipped
          // faces whose projection preserved orientation — e.g. a
          // +dominant-axis polygon projected without an axis-order swap.)
          double ring_area2 = 0.0;  // 2x signed area of the projected ring
          const std::vector<Point2>& ring = polygon[0];
          for (size_t i = 0; i < ring.size(); ++i) {
            const Point2& a = ring[i];
            const Point2& b = ring[(i + 1) % ring.size()];
            ring_area2 += a[0] * b[1] - b[0] * a[1];
          }
          // earcut's output indexes the ring it was handed; verify rather
          // than trust, since every value feeds ring[] and emit_triangle().
          bool local_in_range = true;
          for (uint32_t li : local) {
            if (li >= ring.size()) { local_in_range = false; break; }
          }
          if (!local_in_range) {
            AddWarning("Earcut produced out-of-range indices for face " +
                                std::to_string(f) + " of " + mesh->prim_path +
                                "; using triangle fan fallback");
            used_earcut = false;
          }
          double tri_cross = 0.0;
          for (size_t i = 0; local_in_range && i < local.size() && tri_cross == 0.0;
               i += 3) {
            const Point2& a = ring[local[i]];
            const Point2& b = ring[local[i + 1]];
            const Point2& c = ring[local[i + 2]];
            tri_cross = (b[0] - a[0]) * (c[1] - a[1]) -
                        (b[1] - a[1]) * (c[0] - a[0]);
          }
          const bool flip = (ring_area2 != 0.0) && (tri_cross != 0.0) &&
                            ((ring_area2 > 0.0) != (tri_cross > 0.0));
          for (size_t i = 0; local_in_range && i < local.size(); i += 3) {
            if (flip) {
              emit_triangle(local[i], local[i + 2], local[i + 1]);
            } else {
              emit_triangle(local[i], local[i + 1], local[i + 2]);
            }
          }
        } else {
          AddWarning("Earcut failed for face " + std::to_string(f) +
                              " of " + mesh->prim_path +
                              "; using triangle fan fallback");
        }
        }
      }

      if (!used_earcut) {
        for (uint32_t i = 1; i < nverts - 1; ++i) {
          emit_triangle(0, i, i + 1);
        }
      }
    }
    idx_offset += nverts;
  }
  mesh->face_triangle_offsets[mesh->face_vertex_counts.size()] =
      static_cast<uint32_t>(mesh->triangulated_indices.size() / 3);

  if (oob_faces > 0) {
    AddWarning(
        "Mesh '" + mesh->prim_path + "': dropped " +
        std::to_string(oob_faces) +
        " face(s) whose faceVertexIndices are out of range for " +
        std::to_string(point_count) + " points");
  }

  mesh->is_triangulated = true;
  return true;
}

bool RenderSceneConverter::TriangulateFan(
    const uint32_t* face_vertex_counts, size_t face_count,
    const uint32_t* indices, size_t index_count,
    UInt32Chunked* out_indices) {

  // Count triangles
  size_t tri_count = 0;
  size_t required_index_count = 0;
  for (size_t i = 0; i < face_count; ++i) {
    uint32_t nverts = face_vertex_counts[i];
    size_t next_required_index_count = 0;
    if (!safe::add(required_index_count, size_t(nverts),
                   &next_required_index_count)) {
      return false;
    }
    required_index_count = next_required_index_count;
    if (nverts >= 3) {
      size_t next_tri_count = 0;
      if (!safe::add(tri_count, size_t(nverts - 2), &next_tri_count)) {
        return false;
      }
      tri_count = next_tri_count;
    }
  }
  if (required_index_count > index_count) {
    return false;
  }

  size_t tri_corner_count = 0;
  if (!safe::mul(tri_count, size_t(3), &tri_corner_count) ||
      tri_corner_count >= kMaxTriangulationCornerCount ||
      !out_indices->reserve(tri_corner_count)) {
    return false;
  }

  size_t idx_offset = 0;
  for (size_t f = 0; f < face_count; ++f) {
    uint32_t nverts = face_vertex_counts[f];
    if (nverts < 3) {
      idx_offset += nverts;
      continue;
    }

    // Triangle fan: v0, v1, v2; v0, v2, v3; v0, v3, v4; ...
    uint32_t v0 = indices[idx_offset];
    for (uint32_t i = 1; i < nverts - 1; ++i) {
      out_indices->push_back(v0);
      out_indices->push_back(indices[idx_offset + i]);
      out_indices->push_back(indices[idx_offset + i + 1]);
    }

    idx_offset += nverts;
  }

  return true;
}

//
// Normal computation
//

bool RenderSceneConverter::ComputeVertexNormals(RenderMesh* mesh) {
  if (mesh->points.empty() || !mesh->is_triangulated) {
    // Need triangulated mesh for normal computation
    if (!mesh->is_triangulated) {
      TriangulateMesh(mesh);
    }
    if (!mesh->is_triangulated) return false;
  }

  size_t num_points = mesh->point_count();
  size_t num_tris = mesh->triangulated_indices.size() / 3;

  // Initialize normals to zero
  if (!mesh->normals.resize(num_points * 3, 0.0f)) {
    AddWarning("Out of memory computing normals for mesh '" +
                        mesh->prim_path + "'");
    return false;
  }

  // Accumulate face normals at each vertex
  for (size_t t = 0; t < num_tris; ++t) {
    uint32_t i0 = mesh->triangulated_indices[t * 3 + 0];
    uint32_t i1 = mesh->triangulated_indices[t * 3 + 1];
    uint32_t i2 = mesh->triangulated_indices[t * 3 + 2];
    // Bounds-check the triangle, as the tangent path already does. Nothing
    // here re-validated triangulated_indices against the point count, so a
    // mesh whose topology survived sanitization with an out-of-range corner
    // read (and accumulated into) memory past both points and normals.
    if (i0 >= num_points || i1 >= num_points || i2 >= num_points) continue;

    // One chunk resolution per point instead of three. (The normals
    // accumulation below is inherently random-access -- i0/i1/i2 are arbitrary
    // vertex indices -- so it cannot be made chunk-sequential.)
    float p0[3], p1[3], p2[3];
    mesh->points.read3(i0 * 3, p0);
    mesh->points.read3(i1 * 3, p1);
    mesh->points.read3(i2 * 3, p2);

    // Edge vectors
    float e1[3] = {p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]};
    float e2[3] = {p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2]};

    // Cross product
    float n[3] = {
      e1[1]*e2[2] - e1[2]*e2[1],
      e1[2]*e2[0] - e1[0]*e2[2],
      e1[0]*e2[1] - e1[1]*e2[0]
    };

    // Add to each vertex
    mesh->normals.mutable_at(i0*3+0) += n[0]; mesh->normals.mutable_at(i0*3+1) += n[1]; mesh->normals.mutable_at(i0*3+2) += n[2];
    mesh->normals.mutable_at(i1*3+0) += n[0]; mesh->normals.mutable_at(i1*3+1) += n[1]; mesh->normals.mutable_at(i1*3+2) += n[2];
    mesh->normals.mutable_at(i2*3+0) += n[0]; mesh->normals.mutable_at(i2*3+1) += n[1]; mesh->normals.mutable_at(i2*3+2) += n[2];
  }

  // Normalize
  for (size_t v = 0; v < num_points; ++v) {
    float nx = mesh->normals[v*3+0];
    float ny = mesh->normals[v*3+1];
    float nz = mesh->normals[v*3+2];
    float len = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 1e-8f) {
      mesh->normals.mutable_at(v*3+0) = nx / len;
      mesh->normals.mutable_at(v*3+1) = ny / len;
      mesh->normals.mutable_at(v*3+2) = nz / len;
    } else {
      mesh->normals.mutable_at(v*3+0) = 0.0f;
      mesh->normals.mutable_at(v*3+1) = 1.0f;
      mesh->normals.mutable_at(v*3+2) = 0.0f;
    }
  }

  mesh->normals_interp = Interpolation::Vertex;
  return true;
}

//
// Material conversion
//


} } }  // namespace lightusd::tydra::next
