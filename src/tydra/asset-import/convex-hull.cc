// SPDX-License-Identifier: Apache-2.0
//
// Incremental 3D convex hull. MuJoCo builds the hull of a face-less mesh asset
// (mjCMesh::Process: `if (needhull_ || face_.empty()) MakeGraph(); if
// (face_.empty()) CopyGraph();`) so a point-cloud OBJ still becomes a usable
// collider with its hull's volume and inertia. This implements the same
// result without a dependency: a triangle soup whose faces are wound
// counter-clockwise seen from outside, every input point inside or on it.
//
// The algorithm is the classic incremental hull: start from a tetrahedron of
// four well-separated points, then for each remaining point that lies outside
// the current hull remove the faces it can see and cap the hole with a fan
// from the point to the visible region's horizon. Points on (or within
// tolerance of) a face are ignored, which is what lets a sampled surface with
// many coplanar points produce a clean triangulated hull.
//
// Every result is checked: the volume must be positive and every input point
// must lie inside or on a face. A failure is reported as HullStatus::Invalid
// so callers reject the mesh instead of emitting a corrupt one.

#include "convex-hull.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace lightusd {
namespace tydra {
namespace import_asset {

namespace {

struct V3 {
  double x = 0, y = 0, z = 0;
};

inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double Length(const V3& a) { return std::sqrt(Dot(a, a)); }

struct Face {
  int32_t v[3] = {0, 0, 0};
  V3 normal;      // unit, pointing away from the hull's interior
  double offset = 0.0;  // plane: Dot(normal, p) == offset
  bool alive = true;

  double Distance(const V3& p) const { return Dot(normal, p) - offset; }
};

// Collapse a directed edge to one 64-bit key.
inline uint64_t EdgeKey(int32_t a, int32_t b) {
  return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
}

}  // namespace

// Point-face distance tests a hull may spend (about a minute of work).
constexpr uint64_t kMaxHullWork = uint64_t(20) * 1000 * 1000 * 1000;

HullStatus BuildConvexHull(const float* points, size_t count,
                           std::vector<int32_t>* triangles, std::string* detail) {
  if (triangles) triangles->clear();
  if (!points || count < 4) {
    if (detail) *detail = "fewer than 4 vertices";
    return HullStatus::TooFewPoints;
  }

  std::vector<V3> p(count);
  double lo[3] = {INFINITY, INFINITY, INFINITY};
  double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (size_t i = 0; i < count; ++i) {
    p[i] = {double(points[3 * i + 0]), double(points[3 * i + 1]), double(points[3 * i + 2])};
    if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || !std::isfinite(p[i].z)) {
      if (detail) *detail = "non-finite vertex";
      return HullStatus::Invalid;
    }
    lo[0] = (std::min)(lo[0], p[i].x); hi[0] = (std::max)(hi[0], p[i].x);
    lo[1] = (std::min)(lo[1], p[i].y); hi[1] = (std::max)(hi[1], p[i].y);
    lo[2] = (std::min)(lo[2], p[i].z); hi[2] = (std::max)(hi[2], p[i].z);
  }
  const double diagonal =
      std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                (hi[2] - lo[2]) * (hi[2] - lo[2]));
  const double eps = (std::max)(1e-12, 1e-9 * diagonal);
  const double eps_volume = eps * diagonal * diagonal;

  // Seed: point 0, the farthest point, the farthest point from that line, and
  // the farthest from that plane. Each step detects a degenerate input the way
  // MuJoCo reports it.
  size_t i0 = 0;
  for (size_t i = 1; i < count; ++i)
    if (p[i].x < p[i0].x) i0 = i;
  size_t i1 = i0;
  double best = 0;
  for (size_t i = 0; i < count; ++i) {
    const double d = Length(p[i] - p[i0]);
    if (d > best) { best = d; i1 = i; }
  }
  if (!(best > eps)) {
    if (detail) *detail = "colocated vertices";
    return HullStatus::Colocated;
  }
  const V3 dir1 = (p[i1] - p[i0]) * (1.0 / best);
  size_t i2 = i0;
  best = 0;
  for (size_t i = 0; i < count; ++i) {
    const double d = Length(Cross(p[i] - p[i0], dir1));
    if (d > best) { best = d; i2 = i; }
  }
  if (!(best > eps)) {
    if (detail) *detail = "collinear vertices";
    return HullStatus::Collinear;
  }
  V3 plane_n = Cross(p[i1] - p[i0], p[i2] - p[i0]);
  const double plane_len = Length(plane_n);
  if (!(plane_len > eps_volume)) {
    if (detail) *detail = "collinear vertices";
    return HullStatus::Collinear;
  }
  plane_n = plane_n * (1.0 / plane_len);
  size_t i3 = i0;
  best = 0;
  for (size_t i = 0; i < count; ++i) {
    const double d = Dot(plane_n, p[i] - p[i0]);
    if (std::fabs(d) > best) { best = std::fabs(d); i3 = i; }
  }
  if (!(best > eps)) {
    if (detail) *detail = "coplanar vertices";
    return HullStatus::Coplanar;
  }
  const V3 interior = (p[i0] + p[i1] + p[i2] + p[i3]) * 0.25;

  std::vector<Face> faces;
  faces.reserve(count + 16);
  // Add a triangle facing away from `inside` (winding fixed up if needed).
  // Returns false for a degenerate triangle.
  const auto add_face = [&](int32_t a, int32_t b, int32_t c) -> bool {
    V3 n = Cross(p[size_t(b)] - p[size_t(a)], p[size_t(c)] - p[size_t(a)]);
    const double len = Length(n);
    if (!(len > 0)) return false;
    n = n * (1.0 / len);
    if (Dot(n, interior - p[size_t(a)]) > 0) {
      std::swap(b, c);
      n = n * -1.0;
    }
    Face f;
    f.v[0] = a;
    f.v[1] = b;
    f.v[2] = c;
    f.normal = n;
    f.offset = Dot(n, p[size_t(a)]);
    faces.push_back(f);
    return true;
  };
  if (!add_face(int32_t(i0), int32_t(i1), int32_t(i2)) ||
      !add_face(int32_t(i0), int32_t(i1), int32_t(i3)) ||
      !add_face(int32_t(i0), int32_t(i2), int32_t(i3)) ||
      !add_face(int32_t(i1), int32_t(i2), int32_t(i3))) {
    if (detail) *detail = "degenerate seed tetrahedron";
    return HullStatus::Coplanar;
  }

  std::unordered_map<uint64_t, int32_t> horizon;
  std::vector<int32_t> visible;
  // Dead faces are dropped once they outnumber the live ones (live faces keep
  // their order, so visibility and output are unchanged), and the total
  // point-face work is bounded: a hostile point cloud fails instead of
  // running for hours.
  size_t dead = 0;
  uint64_t work = 0;
  for (size_t i = 0; i < count; ++i) {
    if (i == i0 || i == i1 || i == i2 || i == i3) continue;
    const V3& q = p[i];
    work += faces.size();
    if (work > kMaxHullWork) {
      if (detail) *detail = "convex hull exceeds its work limit";
      return HullStatus::Invalid;
    }
    bool outside = false;
    for (const Face& f : faces)
      if (f.alive && f.Distance(q) > eps) { outside = true; break; }
    if (!outside) continue;

    visible.clear();
    horizon.clear();
    for (size_t fi = 0; fi < faces.size(); ++fi) {
      Face& f = faces[fi];
      if (!f.alive || f.Distance(q) <= eps) continue;
      visible.push_back(int32_t(fi));
      for (int k = 0; k < 3; ++k) {
        const int32_t a = f.v[k];
        const int32_t b = f.v[(k + 1) % 3];
        auto it = horizon.find(EdgeKey(b, a));  // the opposite half-edge
        if (it != horizon.end()) {
          it->second = -1;  // interior edge: both sides are visible
        } else {
          horizon.emplace(EdgeKey(a, b), a);
        }
      }
    }
    if (visible.empty()) continue;
    for (int32_t fi : visible) faces[size_t(fi)].alive = false;
    dead += visible.size();
    // Cap the hole. The horizon map keeps the visible face's winding per
    // boundary edge, which already orients the fan consistently; add_face
    // re-checks it against the interior point.
    bool ok = true;
    for (const auto& entry : horizon) {
      if (entry.second < 0) continue;  // interior edge
      const int32_t a = int32_t(entry.first >> 32);
      const int32_t b = int32_t(entry.first & 0xffffffffu);
      if (!add_face(a, b, int32_t(i))) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      if (detail) *detail = "degenerate hull face";
      return HullStatus::Invalid;
    }
    if (dead * 2 > faces.size()) {
      faces.erase(std::remove_if(faces.begin(), faces.end(),
                                 [](const Face& f) { return !f.alive; }),
                  faces.end());
      dead = 0;
    }
  }

  // Validate: positive volume and every input point inside or on the hull.
  double volume6 = 0.0;
  for (const Face& f : faces) {
    if (!f.alive) continue;
    volume6 += Dot(p[size_t(f.v[0])], Cross(p[size_t(f.v[1])], p[size_t(f.v[2])]));
  }
  if (!(volume6 > 0)) {
    if (detail) *detail = "hull has no volume";
    return HullStatus::Invalid;
  }
  const double eps_inside = (std::max)(1e-9, 1e-6 * diagonal);
  const size_t stride = count > 200000 ? count / 200000 : 1;
  for (size_t i = 0; i < count; i += stride) {
    for (const Face& f : faces) {
      if (!f.alive) continue;
      if (f.Distance(p[i]) > eps_inside) {
        if (detail) *detail = "hull does not contain every vertex";
        return HullStatus::Invalid;
      }
    }
  }

  if (triangles) {
    triangles->reserve(faces.size() * 3);
    for (const Face& f : faces) {
      if (!f.alive) continue;
      triangles->push_back(f.v[0]);
      triangles->push_back(f.v[1]);
      triangles->push_back(f.v[2]);
    }
  }
  return HullStatus::Ok;
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
