// SPDX-License-Identifier: Apache-2.0
//
// Header-only CPU tracer for 3D Gaussian splats using the Blender Cycles
// kernel in gsplat-kernel.hh (bounding-sphere BVH, closest stochastic hit,
// SH emission). Shared by lusdrender and lusdview (--gsplat-kernel cycles).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gsplat-kernel.hh"

namespace lightusd {
namespace gsplat {

class GSplatTracer {
 public:
  struct Hit {
    float t = 0.f;
    uint32_t index = 0;
  };

  void Clear() { *this = GSplatTracer(); }
  size_t size() const { return splats_.size(); }
  bool empty() const { return splats_.empty(); }
  size_t skipped() const { return skipped_; }
  size_t node_count() const { return nodes_.size(); }
  // World-space splats in BVH order; Radiance() takes indices into this.
  const std::vector<GSplatWorld>& splats() const { return splats_; }
  // Per-splat field index and offset (floats) into sh_data(); per-field
  // inverse linear part (row-major) and SH degree. For GPU encoders.
  uint32_t field_of(size_t i) const { return field_of_[i]; }
  size_t sh_offset(size_t i) const { return sh_of_[i]; }
  const std::vector<float>& sh_data() const { return sh_; }
  size_t field_count() const { return fields_.size(); }
  const float* field_linv(size_t f) const { return fields_[f].linv; }
  int field_degree(size_t f) const { return fields_[f].sh_degree; }
  // World-space bounds of the visible support (center +- bounding radius).
  const float* bounds_lo() const { return lo_; }
  const float* bounds_hi() const { return hi_; }

  // Adds one field. `linear` is the object's 3x3 linear part (row-major,
  // column-vector convention: world = linear * local + translation).
  // positions xyz, orientations (w,x,y,z), scales (linear sigma), opacities
  // [0,1], sh: (degree+1)^2 RGB per splat (nullptr = neutral grey).
  // Returns false when the transform is singular (field skipped).
  bool AddField(const double linear[9], const double translation[3],
                size_t count, const float* positions, const float* orientations,
                const float* scales, const float* opacities, int sh_degree,
                const float* sh) {
    Field field;
    if (!Invert3(linear, field.linv)) {
      skipped_ += count;
      return false;
    }
    float fro = 0.f;
    for (int i = 0; i < 9; ++i) fro += float(linear[i] * linear[i]);
    fro = std::sqrt(fro);
    field.sh_degree = sh ? std::max(0, std::min(3, sh_degree)) : 0;
    const size_t coeffs = size_t(field.sh_degree + 1) * (field.sh_degree + 1) * 3;
    const uint32_t field_index = uint32_t(fields_.size());
    fields_.push_back(field);
    splats_.reserve(splats_.size() + count);
    for (size_t i = 0; i < count; ++i) {
      const float* p = positions + i * 3;
      const Vec3 mu = {
          float(linear[0] * p[0] + linear[1] * p[1] + linear[2] * p[2] +
                translation[0]),
          float(linear[3] * p[0] + linear[4] * p[1] + linear[5] * p[2] +
                translation[1]),
          float(linear[6] * p[0] + linear[7] * p[1] + linear[8] * p[2] +
                translation[2])};
      const float* q = orientations ? orientations + i * 4 : nullptr;
      const float* s = scales + i * 3;
      GSplatWorld g;
      if (!std::isfinite(mu.x) || !std::isfinite(mu.y) || !std::isfinite(mu.z) ||
          !MakeGSplatWorld(mu, q ? Quat{q[0], q[1], q[2], q[3]} : Quat{1, 0, 0, 0},
                           {std::fabs(s[0]), std::fabs(s[1]), std::fabs(s[2])},
                           opacities ? opacities[i] : 1.f, field.linv, fro, &g)) {
        ++skipped_;
        continue;
      }
      splats_.push_back(g);
      field_of_.push_back(field_index);
      sh_of_.push_back(sh_.size());
      if (sh)
        sh_.insert(sh_.end(), sh + i * coeffs, sh + (i + 1) * coeffs);
      else
        sh_.insert(sh_.end(), {0.f, 0.f, 0.f});  // DC 0 -> grey 0.5
      for (int k = 0; k < 3; ++k) {
        const float c = (&mu.x)[k];
        lo_[k] = std::min(lo_[k], c - g.radius);
        hi_[k] = std::max(hi_[k], c + g.radius);
      }
    }
    return true;
  }

  // Median-split BVH over bounding spheres. Call after the last AddField.
  void Build() {
    const size_t n = splats_.size();
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = uint32_t(i);
    nodes_.clear();
    nodes_.reserve(n / 2 + 1);
    struct Item { uint32_t node, begin, end; };
    std::vector<Item> stack;
    nodes_.push_back({});
    stack.push_back({0, 0, uint32_t(n)});
    while (!stack.empty()) {
      const Item it = stack.back();
      stack.pop_back();
      Node& node = nodes_[it.node];
      float clo[3] = {1e30f, 1e30f, 1e30f}, chi[3] = {-1e30f, -1e30f, -1e30f};
      for (int k = 0; k < 3; ++k) {
        node.lo[k] = 1e30f;
        node.hi[k] = -1e30f;
      }
      for (uint32_t i = it.begin; i < it.end; ++i) {
        const GSplatWorld& g = splats_[idx[i]];
        for (int k = 0; k < 3; ++k) {
          const float c = (&g.mu.x)[k];
          node.lo[k] = std::min(node.lo[k], c - g.radius);
          node.hi[k] = std::max(node.hi[k], c + g.radius);
          clo[k] = std::min(clo[k], c);
          chi[k] = std::max(chi[k], c);
        }
      }
      const uint32_t count = it.end - it.begin;
      if (count <= 4) {
        node.first = it.begin;
        node.count = count;
        continue;
      }
      int axis = 0;
      for (int k = 1; k < 3; ++k)
        if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
      const uint32_t mid = it.begin + count / 2;
      std::nth_element(idx.begin() + it.begin, idx.begin() + mid,
                       idx.begin() + it.end, [&](uint32_t a, uint32_t b) {
                         return (&splats_[a].mu.x)[axis] <
                                (&splats_[b].mu.x)[axis];
                       });
      const uint32_t left = uint32_t(nodes_.size());
      node.first = left;
      node.count = 0;
      nodes_.push_back({});
      nodes_.push_back({});
      stack.push_back({left, it.begin, mid});
      stack.push_back({left + 1, mid, it.end});
    }
    std::vector<GSplatWorld> splats(n);
    std::vector<uint32_t> field_of(n);
    std::vector<size_t> sh_of(n);
    for (size_t i = 0; i < n; ++i) {
      splats[i] = splats_[idx[i]];
      field_of[i] = field_of_[idx[i]];
      sh_of[i] = sh_of_[idx[i]];
    }
    splats_.swap(splats);
    field_of_.swap(field_of);
    sh_of_.swap(sh_of);
  }

  // Closest splat accepted by the Cycles stochastic opacity test in
  // [tmin, tmax]. `seed` decorrelates samples (mix pixel and sample index).
  bool Trace(Vec3 ro, Vec3 rd, float tmin, float tmax, uint32_t seed,
             Hit* hit) const {
    if (nodes_.empty()) return false;
    const float o[3] = {ro.x, ro.y, ro.z};
    const float inv[3] = {1.f / rd.x, 1.f / rd.y, 1.f / rd.z};
    float best = tmax;
    bool found = false;
    uint32_t stack[96];
    size_t sp = 0;
    stack[sp++] = 0;
    const float dd = Dot(rd, rd);
    while (sp) {
      const Node& nd = nodes_[stack[--sp]];
      if (!HitBox(nd, o, inv, tmin, best)) continue;
      if (nd.count == 0) {
        if (sp + 2 <= 96) {
          stack[sp++] = nd.first;
          stack[sp++] = nd.first + 1;
        }
        continue;
      }
      for (uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
        const GSplatWorld& g = splats_[i];
        // Bounding-sphere reject (Cycles: point_intersect_test).
        const Vec3 oc = Sub(ro, g.mu);
        const float b = Dot(oc, rd);
        const float c = Dot(oc, oc) - g.radius * g.radius;
        if (b * b - dd * c < 0.f) continue;
        GSplatPeak pk;
        if (!GSplatWorldPeak(g, ro, rd, tmin, best, &pk)) continue;
        if (HashPositionToUnit(pk.x, seed) > pk.alpha) continue;
        best = pk.t;
        hit->t = pk.t;
        hit->index = i;
        found = true;
      }
    }
    return found;
  }

  // Emission of a hit splat seen along world direction rd (Cycles evaluates
  // SH along -wi in object space: L^-1 rd).
  Vec3 Radiance(const Hit& hit, Vec3 rd, GSplatColorSpace cs) const {
    const Field& f = fields_[field_of_[hit.index]];
    return GSplatRadiance(f.sh_degree, &sh_[sh_of_[hit.index]],
                          MulA(f.linv, rd), cs);
  }

 private:
  struct Field {
    float linv[9];
    int sh_degree = 0;
  };
  struct Node {
    float lo[3], hi[3];
    uint32_t first;  // child index (inner) or first splat (leaf)
    uint32_t count;  // 0 = inner node: children first, first + 1
  };

  static bool Invert3(const double m[9], float out[9]) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                       m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    const double id = 1.0 / det;
    out[0] = float((m[4] * m[8] - m[5] * m[7]) * id);
    out[1] = float((m[2] * m[7] - m[1] * m[8]) * id);
    out[2] = float((m[1] * m[5] - m[2] * m[4]) * id);
    out[3] = float((m[5] * m[6] - m[3] * m[8]) * id);
    out[4] = float((m[0] * m[8] - m[2] * m[6]) * id);
    out[5] = float((m[2] * m[3] - m[0] * m[5]) * id);
    out[6] = float((m[3] * m[7] - m[4] * m[6]) * id);
    out[7] = float((m[1] * m[6] - m[0] * m[7]) * id);
    out[8] = float((m[0] * m[4] - m[1] * m[3]) * id);
    return true;
  }
  static bool HitBox(const Node& nd, const float o[3], const float inv[3],
                     float tmin, float tmax) {
    float t0 = tmin, t1 = tmax;
    for (int k = 0; k < 3; ++k) {
      float a = (nd.lo[k] - o[k]) * inv[k], b = (nd.hi[k] - o[k]) * inv[k];
      if (a > b) std::swap(a, b);
      t0 = std::max(t0, a);
      t1 = std::min(t1, b);
    }
    return t0 <= t1;
  }

  std::vector<GSplatWorld> splats_;
  std::vector<uint32_t> field_of_;
  std::vector<size_t> sh_of_;
  std::vector<float> sh_;
  std::vector<Field> fields_;
  std::vector<Node> nodes_;
  float lo_[3] = {1e30f, 1e30f, 1e30f}, hi_[3] = {-1e30f, -1e30f, -1e30f};
  size_t skipped_ = 0;
};

}  // namespace gsplat
}  // namespace lightusd
