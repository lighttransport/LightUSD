// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - usdLod heuristics (see usd-lod.hh)

#include "usd-lod.hh"

#include <algorithm>
#include <cmath>

#include "eval/attribute-eval.hh"
#include "schema/geom-boundable.hh"
#include "layer/prim-spec.hh"
#include "layer/property-index.hh"
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

}  // namespace lightusd
