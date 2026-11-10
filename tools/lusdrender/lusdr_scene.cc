// SPDX-License-Identifier: Apache-2.0
// lusdrender — shared scene utilities: worker counts, purpose filtering,
// stats/bounds merging, direct (generated) primitive and curve tessellation,
// auto-fit camera framing.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lusdr_context.hh"

namespace lusdr {

using lusdr::value::color3f;
using lusdr::value::float3;
using lusdr::value::matrix4d;

template <typename T>
T ReadBorrowedArrayValue(const BorrowedArrayView<T> &view, size_t index) {
  if (view.data) return view.data[index];
  T value{};
  std::memcpy(&value, view.bytes + index * sizeof(T), sizeof(T));
  return value;
}


// Number of worker threads to use for the embarrassingly-parallel mesh-stream
// and render passes. `requested` is the user's -threads value (<=0 means auto).
unsigned WorkerThreadCount(int requested) {
  if (requested > 0) return unsigned(requested);
  unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? hw : 1u;
}






uint32_t PurposeBit(lusdr::Purpose purpose) {
  switch (purpose) {
    case lusdr::Purpose::Render:
      return kPurposeRenderBit;
    case lusdr::Purpose::Proxy:
      return kPurposeProxyBit;
    case lusdr::Purpose::Guide:
      return kPurposeGuideBit;
    case lusdr::Purpose::Default:
    default:
      return kPurposeDefaultBit;
  }
}


bool PurposeVisible(uint32_t purpose_bit, uint32_t purpose_mask) {
  return (purpose_bit & purpose_mask) != 0;
}




// A single mesh-extraction work item produced by the (serial) tree walk and
// consumed by the parallel mesh-stream workers.


void MergeStats(RTPreviewStats *dst, const RTPreviewStats &src) {
  dst->meshes_with_mmap_points += src.meshes_with_mmap_points;
  dst->meshes_with_owned_points += src.meshes_with_owned_points;
  dst->skipped_meshes += src.skipped_meshes;
  dst->triangles += src.triangles;
  dst->copied_point_bytes += src.copied_point_bytes;
  dst->copied_topology_bytes += src.copied_topology_bytes;
  dst->purpose_default_triangles += src.purpose_default_triangles;
  dst->purpose_render_triangles += src.purpose_render_triangles;
  dst->purpose_proxy_triangles += src.purpose_proxy_triangles;
  dst->purpose_guide_triangles += src.purpose_guide_triangles;
}

void MergeBounds(Bounds *dst, const Bounds &src) {
  if (!src.valid) return;
  Expand(dst, src.lo);
  Expand(dst, src.hi);
}









float ApproxScale(const matrix4d &m) {
  Vec3 sx = TransformVector(m, Vec3{1.0f, 0.0f, 0.0f});
  Vec3 sy = TransformVector(m, Vec3{0.0f, 1.0f, 0.0f});
  Vec3 sz = TransformVector(m, Vec3{0.0f, 0.0f, 1.0f});
  return std::max(1.0e-6f, (Length(sx) + Length(sy) + Length(sz)) / 3.0f);
}

void AddNurbsTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                      std::vector<float> *vertices, std::vector<TriInfo> *tris,
                      Bounds *bounds) {
  Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
  TriInfo tri;
  tri.p0 = p0;
  tri.p1 = p1;
  tri.p2 = p2;
  tri.n = n;
  tri.base_color = Vec3{0.42f, 0.42f, 0.48f};
  vertices->insert(vertices->end(), {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z,
                                     p2.x, p2.y, p2.z});
  tris->push_back(tri);
  Expand(bounds, p0);
  Expand(bounds, p1);
  Expand(bounds, p2);
}

void AddDirectTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                       const Vec3 &color, std::vector<float> *vertices,
                       std::vector<TriInfo> *tris, Bounds *bounds) {
  Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
  TriInfo tri;
  tri.p0 = p0;
  tri.p1 = p1;
  tri.p2 = p2;
  tri.n = n;
  tri.base_color = color;
  vertices->insert(vertices->end(), {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z,
                                     p2.x, p2.y, p2.z});
  tris->push_back(tri);
  Expand(bounds, p0);
  Expand(bounds, p1);
  Expand(bounds, p2);
}

void AddDirectCube(double size, const matrix4d &world, std::vector<float> *vertices,
                   std::vector<TriInfo> *tris, Bounds *bounds) {
  float h = float(size * 0.5);
  Vec3 p[8] = {
      TransformPoint(world, Vec3{-h, -h, -h}),
      TransformPoint(world, Vec3{ h, -h, -h}),
      TransformPoint(world, Vec3{ h,  h, -h}),
      TransformPoint(world, Vec3{-h,  h, -h}),
      TransformPoint(world, Vec3{-h, -h,  h}),
      TransformPoint(world, Vec3{ h, -h,  h}),
      TransformPoint(world, Vec3{ h,  h,  h}),
      TransformPoint(world, Vec3{-h,  h,  h}),
  };
  const int f[12][3] = {
      {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
      {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
      {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
  };
  for (const auto &tri : f) {
    AddDirectTriangle(p[tri[0]], p[tri[1]], p[tri[2]],
                      Vec3{0.46f, 0.50f, 0.56f}, vertices, tris, bounds);
  }
}

void AddDirectPlane(double width, double length, lusdr::Axis axis,
                    const matrix4d &world, std::vector<float> *vertices,
                    std::vector<TriInfo> *tris, Bounds *bounds) {
  float hw = float(width * 0.5);
  float hl = float(length * 0.5);
  Vec3 local[4];
  if (axis == lusdr::Axis::X) {
    local[0] = Vec3{0.0f, -hw, -hl};
    local[1] = Vec3{0.0f,  hw, -hl};
    local[2] = Vec3{0.0f,  hw,  hl};
    local[3] = Vec3{0.0f, -hw,  hl};
  } else if (axis == lusdr::Axis::Y) {
    local[0] = Vec3{-hw, 0.0f, -hl};
    local[1] = Vec3{ hw, 0.0f, -hl};
    local[2] = Vec3{ hw, 0.0f,  hl};
    local[3] = Vec3{-hw, 0.0f,  hl};
  } else {
    local[0] = Vec3{-hw, -hl, 0.0f};
    local[1] = Vec3{ hw, -hl, 0.0f};
    local[2] = Vec3{ hw,  hl, 0.0f};
    local[3] = Vec3{-hw,  hl, 0.0f};
  }
  Vec3 p0 = TransformPoint(world, local[0]);
  Vec3 p1 = TransformPoint(world, local[1]);
  Vec3 p2 = TransformPoint(world, local[2]);
  Vec3 p3 = TransformPoint(world, local[3]);
  AddDirectTriangle(p0, p1, p2, Vec3{0.38f, 0.55f, 0.44f}, vertices, tris, bounds);
  AddDirectTriangle(p0, p2, p3, Vec3{0.38f, 0.55f, 0.44f}, vertices, tris, bounds);
}

double BSplineBasis(int i, int degree, double u, const std::vector<double> &knots) {
  if (degree == 0) {
    const bool last = (i + 1 == int(knots.size()) - 1) && (u == knots.back());
    return ((knots[size_t(i)] <= u && u < knots[size_t(i + 1)]) || last) ? 1.0 : 0.0;
  }
  double left = 0.0;
  double denom_l = knots[size_t(i + degree)] - knots[size_t(i)];
  if (std::abs(denom_l) > 1.0e-14) {
    left = (u - knots[size_t(i)]) / denom_l *
           BSplineBasis(i, degree - 1, u, knots);
  }
  double right = 0.0;
  double denom_r = knots[size_t(i + degree + 1)] - knots[size_t(i + 1)];
  if (std::abs(denom_r) > 1.0e-14) {
    right = (knots[size_t(i + degree + 1)] - u) / denom_r *
            BSplineBasis(i + 1, degree - 1, u, knots);
  }
  return left + right;
}

Vec3 EvalNurbsPatchPoint(const std::vector<lusdr::value::point3f> &points,
                         const std::vector<double> &weights, int u_count,
                         int v_count, int u_order, int v_order,
                         const std::vector<double> &u_knots,
                         const std::vector<double> &v_knots, double u, double v) {
  Vec3 sum{0.0f, 0.0f, 0.0f};
  double wsum = 0.0;
  int u_degree = std::max(0, u_order - 1);
  int v_degree = std::max(0, v_order - 1);
  for (int j = 0; j < v_count; j++) {
    double bv = BSplineBasis(j, v_degree, v, v_knots);
    if (bv == 0.0) continue;
    for (int i = 0; i < u_count; i++) {
      double bu = BSplineBasis(i, u_degree, u, u_knots);
      if (bu == 0.0) continue;
      size_t idx = size_t(j) * size_t(u_count) + size_t(i);
      double w = idx < weights.size() ? weights[idx] : 1.0;
      double b = bu * bv * w;
      Vec3 p = FromPoint3(points[idx]);
      sum = Add(sum, Mul(p, float(b)));
      wsum += b;
    }
  }
  if (std::abs(wsum) > 1.0e-20) {
    sum = Mul(sum, float(1.0 / wsum));
  }
  return sum;
}


// Fixed base color for unmaterialed curve geometry (the `next` path doesn't
// resolve curve displayColor yet). One value for every hair segment.

void AppendLinearCurveStrands(const std::vector<lusdr::value::point3f> &points,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds) {
  size_t cursor = 0;
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > points.size()) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t(c));
    for (int i = 0; i < c; i++) {
      const size_t idx = cursor + size_t(i);
      Vec3 p = TransformPoint(world, FromPoint3(points[idx]));
      const float radius =
          0.5f * ((idx < widths.size()) ? widths[idx] : 0.01f);
      curve_points->insert(curve_points->end(), {p.x, p.y, p.z});
      curve_radii->push_back(std::max(1.0e-5f, radius * ApproxScale(world)));
      Expand(bounds, p);
    }
    if (info) {
      for (int i = 0; i + 1 < c; i++) {
        TriInfo ti;
        const size_t point_base = size_t(first->back()) + size_t(i);
        Vec3 p0{(*curve_points)[point_base * 3 + 0],
                (*curve_points)[point_base * 3 + 1],
                (*curve_points)[point_base * 3 + 2]};
        Vec3 p1{(*curve_points)[(point_base + 1) * 3 + 0],
                (*curve_points)[(point_base + 1) * 3 + 1],
                (*curve_points)[(point_base + 1) * 3 + 2]};
        ti.p0 = p0;
        ti.p1 = p1;
        ti.p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}

void AppendLinearCurveStrands(const float *points, size_t point_count,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds) {
  if (!points || point_count == 0) return;
  size_t cursor = 0;
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > point_count) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t(c));
    for (int i = 0; i < c; i++) {
      size_t idx = cursor + size_t(i);
      Vec3 p = TransformPoint(
          world, Vec3{points[idx * 3 + 0], points[idx * 3 + 1],
                      points[idx * 3 + 2]});
      float radius = 0.5f * ((idx < widths.size()) ? widths[idx] : 0.01f);
      curve_points->insert(curve_points->end(), {p.x, p.y, p.z});
      curve_radii->push_back(std::max(1.0e-5f, radius * ApproxScale(world)));
      Expand(bounds, p);
    }
    // Per-segment TriInfo: only for the DirectScene curve path (info != null).
    // The instanced curve BLAS passes info == null and derives its slim per-
    // segment endpoints straight from curve_points (kCurveColor is the material),
    // skipping this 120 B/segment intermediate entirely.
    if (info) {
      for (int i = 0; i + 1 < c; i++) {
        TriInfo ti;
        size_t point_base = size_t(first->back()) + size_t(i);
        Vec3 p0{(*curve_points)[point_base * 3 + 0],
                (*curve_points)[point_base * 3 + 1],
                (*curve_points)[point_base * 3 + 2]};
        Vec3 p1{(*curve_points)[(point_base + 1) * 3 + 0],
                (*curve_points)[(point_base + 1) * 3 + 1],
                (*curve_points)[(point_base + 1) * 3 + 2]};
        ti.p0 = p0;
        ti.p1 = p1;
        ti.p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}

void AppendHermiteCurveStrands(const float *points, const float *tangents,
                               size_t point_count,
                               const std::vector<int> &counts,
                               const std::vector<float> &widths,
                               unsigned tessellation_segments,
                               const matrix4d &world,
                               std::vector<float> *curve_points,
                               std::vector<float> *curve_radii,
                               std::vector<uint32_t> *first,
                               std::vector<uint32_t> *count,
                               std::vector<TriInfo> *info,
                               Bounds *bounds) {
  if (!points || !tangents || point_count == 0 || !curve_points ||
      !curve_radii || !first || !count) {
    return;
  }
  const unsigned samples = std::max(1u, tessellation_segments);
  size_t cursor = 0;
  auto width_at = [&](size_t index) {
    return index < widths.size() ? widths[index] : 0.01f;
  };
  auto append_sample = [&](const float p[3], float width) {
    const Vec3 world_p = TransformPoint(world, Vec3{p[0], p[1], p[2]});
    curve_points->insert(curve_points->end(), {world_p.x, world_p.y, world_p.z});
    curve_radii->push_back(std::max(1.0e-5f,
                                    0.5f * width * ApproxScale(world)));
    Expand(bounds, world_p);
  };
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > point_count) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t((size_t(c) - 1) * samples + 1));
    for (int span = 0; span + 1 < c; ++span) {
      const size_t i0 = cursor + size_t(span);
      const size_t i1 = i0 + 1;
      const float *p0 = points + i0 * 3;
      const float *p1 = points + i1 * 3;
      const float *m0 = tangents + i0 * 3;
      const float *m1 = tangents + i1 * 3;
      for (unsigned sample = 0; sample < samples; ++sample) {
        const float t = float(sample) / float(samples);
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        float p[3];
        for (int k = 0; k < 3; ++k)
          p[k] = h00 * p0[k] + h10 * m0[k] + h01 * p1[k] + h11 * m1[k];
        append_sample(p, width_at(i0) +
                             (width_at(i1) - width_at(i0)) * t);
      }
    }
    const size_t last = cursor + size_t(c) - 1;
    append_sample(points + last * 3, width_at(last));
    if (info) {
      const size_t base = size_t(first->back());
      for (size_t i = 0; i + 1 < size_t(count->back()); ++i) {
        TriInfo ti;
        ti.p0 = Vec3{(*curve_points)[(base + i) * 3 + 0],
                     (*curve_points)[(base + i) * 3 + 1],
                     (*curve_points)[(base + i) * 3 + 2]};
        ti.p1 = Vec3{(*curve_points)[(base + i + 1) * 3 + 0],
                     (*curve_points)[(base + i + 1) * 3 + 1],
                     (*curve_points)[(base + i + 1) * 3 + 2]};
        ti.p2 = Add(ti.p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}









// ===========================================================================
// `next` lazy-loader RT preview backend (default for USDC inputs).
//
// Loads the USDC with the experimental `next` reader (fast, low-memory, lazy
// arrays) and streams triangles using tydra's bit-exact world transforms.
// Produces the byte-identical triangle stream of the former eager path (validated:
// matching per-purpose triangle counts on large scenes). Falls back to the
// ===========================================================================

// Auto-fit camera framing the scene bounds from the default view direction
// for the stage up axis (or -viewDir).
CameraFrame MakeCameraFrame(const Options &opt, const Bounds &bounds,
                            int height, lusdr::Axis up_axis) {
  CameraFrame frame;
  Vec3 center{0.0f, 0.0f, 0.0f};
  float radius = 1.0f;
  if (bounds.valid) {
    center = Mul(Add(bounds.lo, bounds.hi), 0.5f);
    radius = std::max(0.001f, Length(Sub(bounds.hi, bounds.lo)) * 0.5f);
  }
  float aspect = (height > 0) ? float(opt.width) / float(height) : 16.0f / 9.0f;
  frame.yfov = 45.0f * 3.14159265358979323846f / 180.0f;
  float distance = radius / std::tan(frame.yfov * 0.5f);
  if (aspect < 1.0f) {
    distance /= aspect;
  }
  Vec3 up_axis_vec{0.0f, 1.0f, 0.0f};
  Vec3 view_dir{0.0f, 0.15f, 1.8f};
  if (up_axis == lusdr::Axis::Z) {
    up_axis_vec = Vec3{0.0f, 0.0f, 1.0f};
    view_dir = Normalize(Vec3{-0.95f, -1.15f, 0.62f});
  } else if (up_axis == lusdr::Axis::X) {
    up_axis_vec = Vec3{1.0f, 0.0f, 0.0f};
    view_dir = Normalize(Vec3{0.62f, -0.95f, -1.15f});
  }
  if (opt.has_view_dir) {
    view_dir = Normalize(opt.view_dir);
  }
  frame.origin = Add(center, Mul(view_dir, distance * opt.fit_scale));
  frame.forward = Normalize(Sub(center, frame.origin));
  frame.right = Normalize(Cross(frame.forward, up_axis_vec));
  if (Length(frame.right) < 1.0e-6f) {
    frame.right = Vec3{1.0f, 0.0f, 0.0f};
  }
  frame.up = Normalize(Cross(frame.right, frame.forward));
  frame.znear = std::max(1.0e-4f, distance * 0.001f);
  frame.zfar = std::max(1000.0f, distance * 10.0f);
  return frame;
}

matrix4d Mat4FromArray(const double d[16]) {
  matrix4d m;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m.m[i][j] = d[i * 4 + j];
  return m;
}

// Read an array attribute, decoding lazily-stored arrays into a throwaway temp
// (materialized_copy) so the `next` stage's Value stays lazy. This keeps the
// big geometry arrays (points/indices/normals) from being permanently
// materialized into the stage as we stream the scene, bounding peak memory.
// `time` is NaN for the default value, or a frame time for animated (time-
// sampled) arrays (held to the nearest authored sample). Decoding goes through
// materialized_copy so the stage's Value stays lazy.



}  // namespace lusdr
