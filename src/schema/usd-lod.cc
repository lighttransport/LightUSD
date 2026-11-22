// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - usdLod heuristics (see usd-lod.hh)

#include "usd-lod.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "eval/attribute-eval.hh"
#include "schema/geom-boundable.hh"
#include "layer/prim-spec.hh"
#include "layer/property-index.hh"
#include "math-trig.hh"
#include "stage/stage.hh"
#include "types/value-view.hh"

namespace lightusd {
namespace {

bool Invert4(const double m[16], double out[16]) {
  double a[4][8] = {};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) { a[r][c] = m[r * 4 + c]; a[r][c + 4] = (r == c); }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
    if (std::abs(a[p][c]) < 1e-300) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
  }
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  return true;
}

double Det4(const double m[16]) {
  double a[4][4];
  for (int i = 0; i < 16; ++i) a[i / 4][i % 4] = m[i];
  double det = 1;
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
    if (a[p][c] == 0) return 0;
    if (p != c) { for (int k = 0; k < 4; ++k) std::swap(a[p][k], a[c][k]); det = -det; }
    det *= a[c][c];
    for (int r = c + 1; r < 4; ++r) {
      const double f = a[r][c] / a[c][c];
      for (int k = c; k < 4; ++k) a[r][k] -= f * a[c][k];
    }
  }
  return det;
}

void Transform(const double m[16], const double p[3], double out[3]) {
  double w = m[15];
  for (int c = 0; c < 3; ++c) out[c] = m[12 + c];
  for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) out[c] += p[r] * m[r * 4 + c]; w += p[r] * m[r * 4 + 3]; }
  if (w != 1.0 && w != 0.0) for (int c = 0; c < 3; ++c) out[c] /= w;
}

std::vector<float> Floats(const EvalResult& r) {
  std::vector<float> out;
  if (!r.success) return out;
  if (auto f = r.value.as_float_array()) out.assign(f->begin(), f->end());
  return out;
}

}  // namespace

bool CreateLodDistanceQuery(const Stage& stage, const UsdPrim& heuristic, double time,
                            LodDistanceQuery* out) {
  if (!out || !heuristic.IsValid()) return false;
  *out = LodDistanceQuery();
  AttributeEval eval(&stage);
  eval.SetTime(time);
  const EvalResult domain = eval.Eval(heuristic, "lod:domain");
  if (domain.success) {
    const StringView t = domain.value.as_token();
    out->lod_domain.assign(t.data(), t.size());
  }
  const EvalResult center = eval.Eval(heuristic, "center");
  if (center.success) {
    if (const double* d = center.value.as_double3()) {
      for (int k = 0; k < 3; ++k) out->center[k] = d[k];
    } else if (const float* f = center.value.as_float3()) {
      for (int k = 0; k < 3; ++k) out->center[k] = f[k];
    }
  }
  if (const std::vector<Path>* targets = heuristic.GetRelationship("boundingVolume")) {
    for (const Path& target : *targets) {
      const UsdPrim prim = stage.GetPrimAtPath(target);
      if (!prim.IsValid()) continue;
      // UsdGeomBoundable::ComputeExtent: the authored extent, else computed.
      double e[6];
      bool ok = false;
      // Only an *authored* extent counts; the schema fallback (e.g. Cube's
      // unit box) must not shadow the computed extent.
      const PrimSpec* spec = prim.GetPrimSpec();
      const PropNameId extent_id = GetPropNameTable().find("extent");
      const bool has_authored = spec && extent_id.is_valid() &&
                                (spec->property(extent_id) || prim.GetTimeSamples(extent_id));
      const EvalResult authored = has_authored ? eval.Eval(prim, "extent") : EvalResult();
      if (has_authored && authored.success) {
        ArrayScratch<float> scratch;
        ArrayView<float> view;
        if (GetFloatArrayView(authored.value, &scratch, &view) && view.size == 6) {
          for (int k = 0; k < 6; ++k) e[k] = view.data[k];
          ok = true;
        }
      }
      if (!ok) ok = ComputeExtent(prim, time, e);
      if (ok) {
        for (int k = 0; k < 3; ++k) { out->extent_min[k] = float(e[k]); out->extent_max[k] = float(e[3 + k]); }
        out->has_extent = true;
        break;
      }
    }
  }
  out->thresholds = Floats(eval.Eval(heuristic, "thresholds"));
  out->blend_thresholds = Floats(eval.Eval(heuristic, "blendThresholds"));
  return true;
}

double ComputeLodDistance(const LodDistanceQuery& q, const double viewpoint[3],
                          const double transform[16]) {
  double target[3] = {q.center[0], q.center[1], q.center[2]};
  const bool empty = !q.has_extent || q.extent_min[0] > q.extent_max[0] ||
                     q.extent_min[1] > q.extent_max[1] || q.extent_min[2] > q.extent_max[2];
  double inv[16];
  if (!empty && std::abs(Det4(transform)) > 1.0e-10 && Invert4(transform, inv)) {
    double local[3];
    Transform(inv, viewpoint, local);
    bool inside = true;
    for (int k = 0; k < 3; ++k)
      inside = inside && local[k] >= q.extent_min[k] && local[k] <= q.extent_max[k];
    if (inside) return 0.0;
    for (int k = 0; k < 3; ++k)
      target[k] = std::clamp(local[k], double(q.extent_min[k]), double(q.extent_max[k]));
  }
  double world[3];
  Transform(transform, target, world);
  const double dx = world[0] - viewpoint[0], dy = world[1] - viewpoint[1],
               dz = world[2] - viewpoint[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double ComputeLodDistance(const LodDistanceQuery& q, const double viewpoint[3],
                          const double transform[16], double prev, double hysteresis) {
  const double distance = ComputeLodDistance(q, viewpoint, transform);
  if (prev < distance - hysteresis) return distance - hysteresis;
  if (prev > distance + hysteresis) return distance + hysteresis;
  return prev;
}

float ComputeLodIndex(const LodDistanceQuery& q, double distance) {
  const auto& t = q.thresholds;
  const auto& b = q.blend_thresholds;
  const int lod = int(std::upper_bound(t.begin(), t.end(), distance) - t.begin());
  if (b.empty()) return float(lod);
  int blend = int(std::upper_bound(b.begin(), b.end(), distance) - b.begin());
  blend = std::max(lod - 1, std::min(lod, blend));
  if (blend == lod || size_t(blend) >= b.size()) return float(lod);
  const float lo = t[size_t(blend)];
  float hi = b[size_t(blend)];
  if (size_t(lod) < t.size()) hi = std::min(hi, t[size_t(lod)]);
  if (lo <= distance && distance < hi) {
    const float fraction = float((distance - lo) / (hi - lo));
    return float(blend) + fraction;
  }
  return float(lod);
}

std::string ComputeLodOverride(const Stage& stage, const UsdPrim& prim, double time,
                               float* index) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  for (UsdPrim p = prim; p.IsValid(); p = p.GetParent()) {
    if (!p.HasAPI("LODOverrideAPI")) continue;
    const PrimSpec* spec = p.GetPrimSpec();
    const PropNameId mode_id = GetPropNameTable().find("lod:override:mode");
    const bool authored = spec && mode_id.is_valid() &&
                          (spec->property(mode_id) || p.GetTimeSamples(mode_id));
    if (!authored) continue;
    const EvalResult mode = eval.Eval(p, "lod:override:mode");
    if (!mode.success) continue;
    const StringView t = mode.value.as_token();
    const std::string m(t.data(), t.size());
    if (m == "inherited") continue;
    if (m == "indexedLOD") {
      const EvalResult idx = eval.Eval(p, "lod:override:index");
      const float* f = idx.success ? idx.value.as_float() : nullptr;
      if (!f) continue;  // unreadable index: treated as inherited
      if (index) *index = *f;
    }
    return m;
  }
  return "noOverride";
}

namespace {

bool ExtentFromAttr(const AttributeEval& eval, const UsdPrim& prim, float mn[3], float mx[3],
                    bool* present) {
  const EvalResult r = eval.Eval(prim, "extent");
  *present = r.success;
  if (!r.success) return false;
  ArrayScratch<float> scratch;
  ArrayView<float> view;
  if (!GetFloatArrayView(r.value, &scratch, &view) || view.size != 6) return false;
  for (int k = 0; k < 3; ++k) { mn[k] = view.data[k]; mx[k] = view.data[3 + k]; }
  return true;
}

double PlaneDistance(const double plane[4], const double p[3]) {
  return plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] - plane[3];
}

void Mul4(const double a[16], const double b[16], double out[16]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = v;
    }
}

}  // namespace

bool CreateLodScreenSizeQuery(const Stage& stage, const UsdPrim& heuristic, double time,
                              LodScreenSizeQuery* out) {
  if (!out || !heuristic.IsValid()) return false;
  *out = LodScreenSizeQuery();
  AttributeEval eval(&stage);
  eval.SetTime(time);
  const EvalResult domain = eval.Eval(heuristic, "lod:domain");
  if (domain.success) {
    const StringView t = domain.value.as_token();
    out->lod_domain.assign(t.data(), t.size());
  }
  bool present = false;
  out->has_extent = ExtentFromAttr(eval, heuristic, out->extent_min, out->extent_max, &present);
  const EvalResult method = eval.Eval(heuristic, "projectionMethod");
  if (method.success) {
    const StringView t = method.value.as_token();
    if (t.size()) out->projection_method.assign(t.data(), t.size());
  }
  if (const std::vector<Path>* targets = heuristic.GetRelationship("boundingVolume")) {
    for (const Path& target : *targets) {
      const UsdPrim prim = stage.GetPrimAtPath(target);
      if (!prim.IsValid()) continue;
      const PrimSpec* spec = prim.GetPrimSpec();
      const PropNameId extent_id = GetPropNameTable().find("extent");
      const bool authored = spec && extent_id.is_valid() &&
                            (spec->property(extent_id) || prim.GetTimeSamples(extent_id));
      double e[6];
      bool ok = false;
      if (authored) {
        float mn[3], mx[3];
        bool p2 = false;
        if (ExtentFromAttr(eval, prim, mn, mx, &p2)) {
          for (int k = 0; k < 3; ++k) { e[k] = mn[k]; e[3 + k] = mx[k]; }
          ok = true;
        }
      }
      if (!ok) ok = ComputeExtent(prim, time, e);
      if (ok) {
        for (int k = 0; k < 3; ++k) { out->extent_min[k] = float(e[k]); out->extent_max[k] = float(e[3 + k]); }
        out->has_extent = true;
        break;
      }
    }
  }
  out->thresholds = Floats(eval.Eval(heuristic, "thresholds"));
  out->blend_thresholds = Floats(eval.Eval(heuristic, "blendThresholds"));
  return true;
}

double ComputeLodScreenSize(const LodScreenSizeQuery& q, const LodFrustum& f,
                            const double transform[16]) {
  const double* mn_f = nullptr;
  (void)mn_f;
  const double mn[3] = {q.extent_min[0], q.extent_min[1], q.extent_min[2]};
  const double mx[3] = {q.extent_max[0], q.extent_max[1], q.extent_max[2]};
  if (!q.has_extent || mn[0] > mx[0] || mn[1] > mx[1] || mn[2] > mx[2]) return 0.0;
  if (q.projection_method == "projectedSphere") {
    double wmin[3], wmax[3], mid[3], wc[3];
    Transform(transform, mn, wmin);
    Transform(transform, mx, wmax);
    double dd = 0;
    for (int k = 0; k < 3; ++k) { dd += (wmax[k] - wmin[k]) * (wmax[k] - wmin[k]); mid[k] = 0.5 * (mn[k] + mx[k]); }
    const double radius_w = std::sqrt(dd) / 2.0;
    Transform(transform, mid, wc);
    for (const auto& plane : f.planes)
      if (PlaneDistance(plane, wc) < -radius_w) return 0.0;
    double radius = radius_w;
    if (f.perspective) {
      double d2 = 0;
      for (int k = 0; k < 3; ++k) d2 += (f.position[k] - wc[k]) * (f.position[k] - wc[k]);
      const double distance = std::sqrt(d2);
      if (distance == 0.0) return std::numeric_limits<double>::max();
      radius = radius_w * f.reference_plane_depth / distance;
    }
    const double area = math::constants<double>::pi() * radius * radius;
    const double window = f.window_size[0] * f.window_size[1];
    if (window == 0.0) return std::numeric_limits<double>::max();
    return area / window;
  }
  // projectedExtent: cull the transformed box (GfFrustum::Intersects(bbox)).
  double corners[8][3];
  for (int i = 0; i < 8; ++i) {
    const double c[3] = {(i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]};
    Transform(transform, c, corners[i]);
  }
  for (const auto& plane : f.planes) {
    bool any = false;
    for (const auto& c : corners) any = any || PlaneDistance(plane, c) >= 0;
    if (!any) return 0.0;
  }
  double vt[16], clip[16];
  Mul4(transform, f.view, vt);
  Mul4(vt, f.projection, clip);
  double h[8][4];
  for (int i = 0; i < 8; ++i) {
    const double c[3] = {(i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]};
    for (int col = 0; col < 4; ++col)
      h[i][col] = c[0] * clip[col] + c[1] * clip[4 + col] + c[2] * clip[8 + col] + clip[12 + col];
  }
  static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                    {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  std::vector<std::array<double, 2>> pts;
  auto project = [&](const double a[4], const double b[4], double t) {
    double p[4];
    for (int k = 0; k < 4; ++k) p[k] = a[k] + (b[k] - a[k]) * t;
    pts.push_back({p[0] / p[3], p[1] / p[3]});
  };
  for (const auto& e : kEdges) {
    const double* p0 = h[e[0]];
    const double* p1 = h[e[1]];
    double t0 = 0, t1 = 1;
    const double d0 = p0[2] + p0[3], d1 = p1[2] + p1[3];
    if (d0 < 0 && d1 < 0) continue;
    if (d0 < 0 || d1 < 0) {
      const double t = d0 / (d0 - d1);
      if (d0 < 0) t0 = std::max(t0, t); else t1 = std::min(t1, t);
    }
    project(p0, p1, t0);
    project(p0, p1, t1);
  }
  std::sort(pts.begin(), pts.end());
  auto close = [](double a, double b) {
    return std::abs(a - b) <= 1e-6;  // GfIsClose(a, b, 1e-6)
  };
  pts.erase(std::unique(pts.begin(), pts.end(), [&](const auto& a, const auto& b) {
              return close(a[0], b[0]) && close(a[1], b[1]);
            }), pts.end());
  auto cross = [](const std::array<double, 2>& a, const std::array<double, 2>& b,
                  const std::array<double, 2>& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  };
  std::vector<std::array<double, 2>> hull(pts.size() + 1);
  int n = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    while (n >= 2 && cross(hull[n - 2], hull[n - 1], pts[i]) <= 0) --n;
    hull[n++] = pts[i];
  }
  const int lower = n + 1;
  for (int j = int(pts.size()) - 2; j >= 0; --j) {
    while (n >= lower && cross(hull[n - 2], hull[n - 1], pts[size_t(j)]) <= 0) --n;
    hull[n++] = pts[size_t(j)];
  }
  double area2 = 0;
  for (int i = 0; i < n - 1; ++i) area2 += hull[i][0] * hull[i + 1][1] - hull[i][1] * hull[i + 1][0];
  return area2 / 8.0;
}

float ComputeLodIndex(const LodScreenSizeQuery& q, double size) {
  const auto& t = q.thresholds;
  const auto& b = q.blend_thresholds;
  auto greater = [](double v, float x) { return v > x; };
  const int lod = int(std::upper_bound(t.begin(), t.end(), size, greater) - t.begin());
  if (b.empty()) return float(lod);
  int blend = int(std::upper_bound(b.begin(), b.end(), size, greater) - b.begin());
  blend = std::max(lod - 1, std::min(lod, blend));
  if (blend == lod || size_t(blend) >= b.size()) return float(lod);
  const float lo = t[size_t(blend)];
  float hi = b[size_t(blend)];
  if (size_t(lod) < t.size()) hi = std::max(hi, t[size_t(lod)]);
  if (lo >= size && size > hi) return float(blend) + float((size - lo) / (hi - lo));
  return float(lod);
}

}  // namespace lightusd
