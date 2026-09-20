// SPDX-License-Identifier: Apache-2.0
// lusdrender — shared low-level types: memory budget/pool, Vec3/Bounds, the
// per-triangle/material records, the pooled-vector typedefs, and the inline
// vector/matrix math. Leaf header (no other lusdr_* deps).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#if defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "value-types.hh"
#include "tydra/openpbr-params.hh"
#include "tydra/next/mem-budget.hh"
#include "tydra/next/resource-budget.hh"
#include "xform.hh"

namespace lusdr {

using lightusd::value::color3f;
using lightusd::value::float3;
using lightusd::value::matrix4d;

// Memory budget + tracking pool allocator now live in the shared header so
// examples/lusdquicklook can use them too. These aliases keep every lusdr call
// site (MemBudget::Get(), PoolAlloc<T>, MemPool::Get()) unchanged.
using MemBudget = lightusd::tydra::next::MemBudget;
using MemPool = lightusd::tydra::next::MemPool;
template <class T>
using PoolAlloc = lightusd::tydra::next::PoolAlloc<T>;

// Conservative estimate of the extra memory LightRT's BVH build adds per
// triangle (the re-swizzled vertex copy ~36 B + wide-BVH nodes/leaves). Used by
// the pre-build RSS guards; intentionally an over-estimate so the guard trips
// before an OOM rather than after.
static constexpr size_t kBvhBytesPerTri = 80;

// Max anisotropic taps for diffuse texture sampling (GPU-style; 1 = isotropic
// trilinear). Each tap is a trilinear lookup, so this bounds the per-hit cost.
static constexpr int kMaxAniso = 8;

struct Vec3 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
};

struct Bounds {
  Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
          std::numeric_limits<float>::max()};
  Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
          -std::numeric_limits<float>::max()};
  bool valid{false};
};

static constexpr uint32_t kPurposeDefaultBit = 1u << 0u;
static constexpr uint32_t kPurposeRenderBit = 1u << 1u;
static constexpr uint32_t kPurposeProxyBit = 1u << 2u;
static constexpr uint32_t kPurposeGuideBit = 1u << 3u;
static constexpr uint32_t kPurposeDefaultMask =
    kPurposeDefaultBit | kPurposeRenderBit | kPurposeProxyBit;
static constexpr uint32_t kNoOpenPBRMaterial = UINT32_MAX;
static constexpr uint32_t kNoBackfaceMaterial = UINT32_MAX;

// Exactly representable as float; zero is the capture background.
inline uint32_t CapturePrimId(const std::string& path) {
  uint32_t h = 2166136261u;
  for (unsigned char c : path) { h ^= c; h *= 16777619u; }
  return (h % 16777215u) + 1u;
}

struct TriInfo {
  uint32_t capture_prim_id{0};
  Vec3 p0;
  Vec3 p1;
  Vec3 p2;
  Vec3 n;
  // USD doubleSided (default 1 = pre-cull behavior; see FlatTri::double_sided).
  uint8_t double_sided{1};
  Vec3 base_color{0.18f, 0.18f, 0.18f};
  Vec3 emission{0.0f, 0.0f, 0.0f};
  float roughness{0.55f};
  float metallic{0.0f};
  uint32_t purpose_bit{kPurposeDefaultBit};
  int32_t tex_id{-1};  // diffuse texture index, or -1 for a flat base_color
  int32_t normal_tex_id{-1};  // tangent-space normal map, or -1
  int32_t rough_tex_id{-1};   // roughness texture, or -1
  int32_t metal_tex_id{-1};   // metallic texture, or -1
  int32_t emission_tex_id{-1};  // emissive color texture (sRGB), or -1
  int32_t occ_tex_id{-1};       // occlusion (AO) texture, or -1
  float occlusion{1.0f};        // ambient-occlusion scalar
  uint8_t rough_ch{0};          // source channel (0=r,1=g,2=b,3=a)
  uint8_t metal_ch{0};
  uint8_t occ_ch{0};
  float rough_tex_scale{1.0f};
  float rough_tex_bias{0.0f};
  float metal_tex_scale{1.0f};
  float metal_tex_bias{0.0f};
  float occ_tex_scale{1.0f};
  float occ_tex_bias{0.0f};
  float opacity{1.0f};          // displayOpacity / UsdPreviewSurface opacity; <1 = blend
  int32_t opacity_tex_id{-1};   // UsdPreviewSurface inputs:opacity texture, or -1
  uint8_t opacity_ch{0};        // opacity texture source channel (often 'a')
  float opacity_tex_scale{1.0f};
  float opacity_tex_bias{0.0f};
  float opacity_threshold{0.0f}; // inputs:opacityThreshold; >0 = alpha cutout (mask)
  float clearcoat{0.0f};        // inputs:clearcoat weight (2nd specular lobe)
  float clearcoat_roughness{0.01f}; // inputs:clearcoatRoughness
  int32_t clearcoat_tex_id{-1};      // clearcoat-weight texture, or -1
  int32_t clearcoat_rough_tex_id{-1}; // clearcoat-roughness texture, or -1
  uint8_t clearcoat_ch{0};
  uint8_t clearcoat_rough_ch{0};
  float clearcoat_tex_scale{1.0f};
  float clearcoat_tex_bias{0.0f};
  float clearcoat_rough_tex_scale{1.0f};
  float clearcoat_rough_tex_bias{0.0f};
  Vec3 specular_color{0.0f, 0.0f, 0.0f}; // inputs:specularColor (specular workflow F0)
  int32_t specular_tex_id{-1};       // specularColor texture, or -1
  float ior{1.5f};                   // inputs:ior; dielectric F0 = ((ior-1)/(ior+1))^2
  uint8_t use_specular_workflow{0};  // inputs:useSpecularWorkflow
  uint32_t openpbr_id{kNoOpenPBRMaterial};  // optional side-table OpenPBR block
  // This triangle is part of an emissive mesh that is ALSO registered as an
  // analytic mesh light (LightCache::mesh). Its emission is therefore already
  // being delivered by direct lighting, and a BSDF-bounce ray that lands on it
  // must not add it a second time -- see the `indirect` argument of Shade.
  uint8_t area_light{0};
};

// Per-material shading parameters, factored out of the per-triangle record. A
// scene with many meshes sharing few materials (e.g. Island's coral) stores one
// of these per mesh-job instead of replicating ~64 B of material data on every
// triangle. The instanced (TLAS) BLAS arrays store the slim TriStore below + a
// side table of TriMat, cutting per-triangle memory ~2× on geometry-heavy scenes.
struct TriMat {
  uint32_t capture_prim_id{0};
  Vec3 base_color{0.18f, 0.18f, 0.18f};
  Vec3 emission{0.0f, 0.0f, 0.0f};
  float roughness{0.55f};
  float metallic{0.0f};
  int32_t tex_id{-1};
  int32_t normal_tex_id{-1};
  int32_t rough_tex_id{-1};
  int32_t metal_tex_id{-1};
  int32_t emission_tex_id{-1};
  int32_t occ_tex_id{-1};
  float occlusion{1.0f};
  float opacity{1.0f};  // displayOpacity / UsdPreviewSurface opacity; <1 = see-through
  int32_t opacity_tex_id{-1};   // UsdPreviewSurface inputs:opacity texture, or -1
  float opacity_threshold{0.0f}; // inputs:opacityThreshold; >0 = alpha cutout (mask)
  float clearcoat{0.0f};        // inputs:clearcoat weight (2nd specular lobe)
  float clearcoat_roughness{0.01f}; // inputs:clearcoatRoughness
  int32_t clearcoat_tex_id{-1};      // clearcoat-weight texture, or -1
  int32_t clearcoat_rough_tex_id{-1}; // clearcoat-roughness texture, or -1
  uint8_t rough_ch{0};
  uint8_t metal_ch{0};
  uint8_t occ_ch{0};
  uint8_t opacity_ch{0};
  uint8_t clearcoat_ch{0};
  uint8_t clearcoat_rough_ch{0};
  float rough_tex_scale{1.0f};
  float rough_tex_bias{0.0f};
  float metal_tex_scale{1.0f};
  float metal_tex_bias{0.0f};
  float occ_tex_scale{1.0f};
  float occ_tex_bias{0.0f};
  float opacity_tex_scale{1.0f};
  float opacity_tex_bias{0.0f};
  float clearcoat_tex_scale{1.0f};
  float clearcoat_tex_bias{0.0f};
  float clearcoat_rough_tex_scale{1.0f};
  float clearcoat_rough_tex_bias{0.0f};
  Vec3 specular_color{0.0f, 0.0f, 0.0f}; // inputs:specularColor (specular workflow)
  int32_t specular_tex_id{-1};
  float ior{1.5f};
  uint8_t use_specular_workflow{0};
  uint32_t openpbr_id{kNoOpenPBRMaterial};  // optional side-table OpenPBR block
  // Optional index into the SAME material table for a ray hitting the authored
  // back face. Stored once per mesh-job material, not once per triangle.
  uint32_t backface_id{kNoBackfaceMaterial};
  // This triangle is part of an emissive mesh that is ALSO registered as an
  // analytic mesh light (LightCache::mesh). Its emission is therefore already
  // being delivered by direct lighting, and a BSDF-bounce ray that lands on it
  // must not add it a second time -- see the `indirect` argument of Shade.
  uint8_t area_light{0};
};

// Slim per-triangle record for instanced BLAS storage: just a material id into
// the BLAS's TriMat table. Triangle positions are read from the BLAS's vertex
// soup (`Blas::vertices`, which LightRT aliases so it stays resident) at hit
// time, and the geometric normal is recomputed there — so the instanced
// per-triangle record is only 4 bytes (down from a 124 B TriInfo).
struct TriStore {
  uint32_t mat_id{0};
};

// Flat (non-instanced) per-triangle record: world-space geometry + purpose +
// a material id into the flat material table (RenderContext::flat_mats). The
// flat path used to store a full ~176 B TriInfo per triangle (geometry AND a
// copy of the per-mesh material); since meshes share few materials, the material
// is hoisted into the side table and referenced by id, mirroring the instanced
// TriStore/TriMat split. 56 B/triangle vs 176 B. Shade rebuilds the full TriInfo
// at hit via CombineTriMat(flat_mats[mat_id]) + this geometry.
struct FlatTri {
  Vec3 p0;
  Vec3 p1;
  Vec3 p2;
  Vec3 n;  // geometric (winding) normal: the front face points along +n
  uint32_t purpose_bit{kPurposeDefaultBit};
  uint32_t mat_id{0};
  // USD doubleSided (default 1 here = the pre-cull behavior, so any unstamped
  // flat tri keeps rendering from both sides). Stamped from the authored value
  // at every mesh creation site; 0 = single-sided -> back-face culled.
  uint8_t double_sided{1};
};

// Build a full TriInfo from a material record (positions/normal are filled by the
// caller from the vertex soup). Used by ResolveTLASHit for instanced mesh hits.
inline TriInfo CombineTriMat(const TriMat &m) {
  TriInfo t;
  t.base_color = m.base_color;
  t.emission = m.emission;
  t.roughness = m.roughness;
  t.metallic = m.metallic;
  t.tex_id = m.tex_id;
  t.normal_tex_id = m.normal_tex_id;
  t.rough_tex_id = m.rough_tex_id;
  t.metal_tex_id = m.metal_tex_id;
  t.emission_tex_id = m.emission_tex_id;
  t.occ_tex_id = m.occ_tex_id;
  t.occlusion = m.occlusion;
  t.opacity = m.opacity;
  t.opacity_tex_id = m.opacity_tex_id;
  t.opacity_threshold = m.opacity_threshold;
  t.clearcoat = m.clearcoat;
  t.clearcoat_roughness = m.clearcoat_roughness;
  t.clearcoat_tex_id = m.clearcoat_tex_id;
  t.clearcoat_rough_tex_id = m.clearcoat_rough_tex_id;
  t.rough_ch = m.rough_ch;
  t.metal_ch = m.metal_ch;
  t.occ_ch = m.occ_ch;
  t.opacity_ch = m.opacity_ch;
  t.clearcoat_ch = m.clearcoat_ch;
  t.clearcoat_rough_ch = m.clearcoat_rough_ch;
  t.rough_tex_scale = m.rough_tex_scale;
  t.rough_tex_bias = m.rough_tex_bias;
  t.metal_tex_scale = m.metal_tex_scale;
  t.metal_tex_bias = m.metal_tex_bias;
  t.occ_tex_scale = m.occ_tex_scale;
  t.occ_tex_bias = m.occ_tex_bias;
  t.opacity_tex_scale = m.opacity_tex_scale;
  t.opacity_tex_bias = m.opacity_tex_bias;
  t.clearcoat_tex_scale = m.clearcoat_tex_scale;
  t.clearcoat_tex_bias = m.clearcoat_tex_bias;
  t.clearcoat_rough_tex_scale = m.clearcoat_rough_tex_scale;
  t.clearcoat_rough_tex_bias = m.clearcoat_rough_tex_bias;
  t.specular_color = m.specular_color;
  t.specular_tex_id = m.specular_tex_id;
  t.ior = m.ior;
  t.use_specular_workflow = m.use_specular_workflow;
  t.openpbr_id = m.openpbr_id;
  t.area_light = m.area_light;
  t.capture_prim_id = m.capture_prim_id;
  return t;
}

// Inverse of CombineTriMat: pull the shading parameters out of a full TriInfo so
// a curve BLAS can store one TriMat per material instead of replicating ~64 B of
// material on every hair segment (the geometry p0/p1 stay per-segment; p2/n are
// synthetic/recomputed). Mirrors the mesh TriStore + TriMat split.
inline TriMat ExtractTriMat(const TriInfo &t) {
  TriMat m;
  m.capture_prim_id = t.capture_prim_id;
  m.base_color = t.base_color;
  m.emission = t.emission;
  m.roughness = t.roughness;
  m.metallic = t.metallic;
  m.tex_id = t.tex_id;
  m.normal_tex_id = t.normal_tex_id;
  m.rough_tex_id = t.rough_tex_id;
  m.metal_tex_id = t.metal_tex_id;
  m.emission_tex_id = t.emission_tex_id;
  m.occ_tex_id = t.occ_tex_id;
  m.occlusion = t.occlusion;
  m.opacity = t.opacity;
  m.opacity_tex_id = t.opacity_tex_id;
  m.opacity_threshold = t.opacity_threshold;
  m.clearcoat = t.clearcoat;
  m.clearcoat_roughness = t.clearcoat_roughness;
  m.clearcoat_tex_id = t.clearcoat_tex_id;
  m.clearcoat_rough_tex_id = t.clearcoat_rough_tex_id;
  m.rough_ch = t.rough_ch;
  m.metal_ch = t.metal_ch;
  m.occ_ch = t.occ_ch;
  m.opacity_ch = t.opacity_ch;
  m.clearcoat_ch = t.clearcoat_ch;
  m.clearcoat_rough_ch = t.clearcoat_rough_ch;
  m.rough_tex_scale = t.rough_tex_scale;
  m.rough_tex_bias = t.rough_tex_bias;
  m.metal_tex_scale = t.metal_tex_scale;
  m.metal_tex_bias = t.metal_tex_bias;
  m.occ_tex_scale = t.occ_tex_scale;
  m.occ_tex_bias = t.occ_tex_bias;
  m.opacity_tex_scale = t.opacity_tex_scale;
  m.opacity_tex_bias = t.opacity_tex_bias;
  m.clearcoat_tex_scale = t.clearcoat_tex_scale;
  m.clearcoat_tex_bias = t.clearcoat_tex_bias;
  m.clearcoat_rough_tex_scale = t.clearcoat_rough_tex_scale;
  m.clearcoat_rough_tex_bias = t.clearcoat_rough_tex_bias;
  m.specular_color = t.specular_color;
  m.specular_tex_id = t.specular_tex_id;
  m.ior = t.ior;
  m.use_specular_workflow = t.use_specular_workflow;
  m.openpbr_id = t.openpbr_id;
  m.area_light = t.area_light;
  return m;
}

// Split a legacy per-triangle TriInfo array into the (FlatTri geometry+purpose) +
// (TriMat material table) form Shade/RenderImage consume. One material per
// triangle (no dedup) -- the legacy path may carry per-triangle materials -- so
// it is exactly round-trip identical: CombineTriMat(mats[i]) restores tris[i]'s
// material and the geometry is copied verbatim.
inline void SplitTriInfos(const std::vector<TriInfo> &tris,
                          std::vector<FlatTri> *out_tris,
                          std::vector<TriMat> *out_mats) {
  out_tris->resize(tris.size());
  out_mats->resize(tris.size());
  for (size_t i = 0; i < tris.size(); ++i) {
    const TriInfo &t = tris[i];
    FlatTri &ft = (*out_tris)[i];
    ft.p0 = t.p0;
    ft.p1 = t.p1;
    ft.p2 = t.p2;
    ft.n = t.n;
    ft.purpose_bit = t.purpose_bit;
    ft.mat_id = uint32_t(i);
    ft.double_sided = t.double_sided;
    (*out_mats)[i] = ExtractTriMat(t);
  }
}

inline bool SameTriMat(const TriMat &a, const TriMat &b) {
  return a.base_color.x == b.base_color.x && a.base_color.y == b.base_color.y &&
         a.base_color.z == b.base_color.z && a.emission.x == b.emission.x &&
         a.emission.y == b.emission.y && a.emission.z == b.emission.z &&
         a.roughness == b.roughness && a.metallic == b.metallic &&
         a.tex_id == b.tex_id && a.normal_tex_id == b.normal_tex_id &&
         a.rough_tex_id == b.rough_tex_id && a.metal_tex_id == b.metal_tex_id &&
         a.emission_tex_id == b.emission_tex_id && a.occ_tex_id == b.occ_tex_id &&
         a.occlusion == b.occlusion && a.opacity == b.opacity &&
         a.opacity_tex_id == b.opacity_tex_id &&
         a.opacity_threshold == b.opacity_threshold &&
         a.clearcoat == b.clearcoat &&
         a.clearcoat_roughness == b.clearcoat_roughness &&
         a.clearcoat_tex_id == b.clearcoat_tex_id &&
         a.clearcoat_rough_tex_id == b.clearcoat_rough_tex_id &&
         a.rough_ch == b.rough_ch && a.metal_ch == b.metal_ch &&
         a.occ_ch == b.occ_ch && a.opacity_ch == b.opacity_ch &&
         a.clearcoat_ch == b.clearcoat_ch &&
         a.clearcoat_rough_ch == b.clearcoat_rough_ch &&
         a.rough_tex_scale == b.rough_tex_scale &&
         a.rough_tex_bias == b.rough_tex_bias &&
         a.metal_tex_scale == b.metal_tex_scale &&
         a.metal_tex_bias == b.metal_tex_bias &&
         a.occ_tex_scale == b.occ_tex_scale &&
         a.occ_tex_bias == b.occ_tex_bias &&
         a.opacity_tex_scale == b.opacity_tex_scale &&
         a.opacity_tex_bias == b.opacity_tex_bias &&
         a.clearcoat_tex_scale == b.clearcoat_tex_scale &&
         a.clearcoat_tex_bias == b.clearcoat_tex_bias &&
         a.clearcoat_rough_tex_scale == b.clearcoat_rough_tex_scale &&
         a.clearcoat_rough_tex_bias == b.clearcoat_rough_tex_bias &&
         a.specular_color.x == b.specular_color.x &&
         a.specular_color.y == b.specular_color.y &&
         a.specular_color.z == b.specular_color.z &&
         a.specular_tex_id == b.specular_tex_id && a.ior == b.ior &&
         a.use_specular_workflow == b.use_specular_workflow &&
         a.openpbr_id == b.openpbr_id &&
         a.backface_id == b.backface_id && a.area_light == b.area_light;
}

// A scalar texture binding: texture index + source channel (UsdUVTexture
// outputs:r/g/b/a; for ORM packing roughness=g, metallic=b in one texture).
struct ScalarTex {
  int32_t id{-1};
  uint8_t ch{0};
  // UsdUVTexture inputs:scale/inputs:bias for the sampled channel (out =
  // raw*scale + bias). Displacement applies this at mesh-build time; other
  // scalar inputs apply it at hit time.
  float scale{1.0f};
  float bias{0.0f};
};

// Budget-tracked, pooled vectors for the big render buffers (triangle positions,
// TriInfo, UVs). All other vectors keep the default allocator.
using FloatVec = std::vector<float, PoolAlloc<float>>;
// Per-corner displayColor/Opacity stored as RGBA8 (12 B/tri vs 48 B as float):
// the rendered difference is <=1/255 (1 LSB), well below output precision.
using ByteVec = std::vector<uint8_t, PoolAlloc<uint8_t>>;
using TriVec = std::vector<TriInfo, PoolAlloc<TriInfo>>;
// Indexed geometry (Phase 2): unique vertex ids, 3 per triangle, into a BLAS's
// uverts array. Lets the stream hand 1x unique verts + indices to the indexed
// LightRT build instead of a 3x-expanded soup.
using IdxVec = std::vector<uint32_t, PoolAlloc<uint32_t>>;
using TriStoreVec = std::vector<TriStore, PoolAlloc<TriStore>>;

inline float Dot(const Vec3 &a, const Vec3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x};
}

inline Vec3 Add(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 Sub(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 Mul(const Vec3 &a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }

inline Vec3 Mul(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

inline Vec3 Div(const Vec3 &a, float s) {
  if (std::abs(s) <= 1.0e-20f) return Vec3{0.0f, 0.0f, 0.0f};
  return Vec3{a.x / s, a.y / s, a.z / s};
}

inline float Length(const Vec3 &v) { return std::sqrt(std::max(0.0f, Dot(v, v))); }

inline float Luminance(const Vec3 &v) {
  return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}

inline Vec3 Normalize(const Vec3 &v) {
  float len = Length(v);
  if (len <= 1.0e-20f) {
    return Vec3{0.0f, 0.0f, 0.0f};
  }
  return Mul(v, 1.0f / len);
}

// Any two unit vectors perpendicular to `w` and to each other. Gives a shaped
// light a sampling basis when its world matrix carries no usable axes.
inline void OrthonormalBasis(const Vec3 &w, Vec3 *u, Vec3 *v) {
  const Vec3 a = (std::fabs(w.x) > 0.9f) ? Vec3{0.0f, 1.0f, 0.0f}
                                         : Vec3{1.0f, 0.0f, 0.0f};
  *u = Normalize(Cross(a, w));
  *v = Cross(w, *u);
}

inline Vec3 Clamp01(const Vec3 &v) {
  return Vec3{std::max(0.0f, std::min(1.0f, v.x)),
              std::max(0.0f, std::min(1.0f, v.y)),
              std::max(0.0f, std::min(1.0f, v.z))};
}

inline Vec3 Lerp(const Vec3 &a, const Vec3 &b, float t) {
  return Add(Mul(a, 1.0f - t), Mul(b, t));
}

inline Vec3 Reflect(const Vec3 &v, const Vec3 &n) {
  return Sub(v, Mul(n, 2.0f * Dot(v, n)));
}

inline float ClampFloat(float v, float lo, float hi) {
  return std::max(lo, std::min(hi, v));
}

inline Vec3 FromFloat3(const float3 &v) { return Vec3{v[0], v[1], v[2]}; }
inline Vec3 FromPoint3(const lightusd::value::point3f &v) {
  return Vec3{v[0], v[1], v[2]};
}
inline Vec3 FromVector3(const lightusd::value::vector3f &v) {
  return Vec3{v[0], v[1], v[2]};
}

inline Vec3 TransformPoint(const matrix4d &m, const Vec3 &p) {
  double x = m.m[0][0] * double(p.x) + m.m[1][0] * double(p.y) +
             m.m[2][0] * double(p.z) + m.m[3][0];
  double y = m.m[0][1] * double(p.x) + m.m[1][1] * double(p.y) +
             m.m[2][1] * double(p.z) + m.m[3][1];
  double z = m.m[0][2] * double(p.x) + m.m[1][2] * double(p.y) +
             m.m[2][2] * double(p.z) + m.m[3][2];
  double w = m.m[0][3] * double(p.x) + m.m[1][3] * double(p.y) +
             m.m[2][3] * double(p.z) + m.m[3][3];
  if (std::abs(w) > 1.0e-20) {
    x /= w;
    y /= w;
    z /= w;
  }
  return Vec3{float(x), float(y), float(z)};
}

inline Vec3 TransformVector(const matrix4d &m, const Vec3 &v) {
  return Vec3{
      float(m.m[0][0] * double(v.x) + m.m[1][0] * double(v.y) +
            m.m[2][0] * double(v.z)),
      float(m.m[0][1] * double(v.x) + m.m[1][1] * double(v.y) +
            m.m[2][1] * double(v.z)),
      float(m.m[0][2] * double(v.x) + m.m[1][2] * double(v.y) +
            m.m[2][2] * double(v.z))};
}

inline void Expand(Bounds *b, const Vec3 &p) {
  if (!b) return;
  b->lo.x = std::min(b->lo.x, p.x);
  b->lo.y = std::min(b->lo.y, p.y);
  b->lo.z = std::min(b->lo.z, p.z);
  b->hi.x = std::max(b->hi.x, p.x);
  b->hi.y = std::max(b->hi.y, p.y);
  b->hi.z = std::max(b->hi.z, p.z);
  b->valid = true;
}

// 3x4 object->world matrix helpers (compact instance transforms).
inline void Mat4ToObj2World(const matrix4d &m, float out[12]) {
  for (int k = 0; k < 3; ++k) {
    out[k * 4 + 0] = float(m.m[0][k]);
    out[k * 4 + 1] = float(m.m[1][k]);
    out[k * 4 + 2] = float(m.m[2][k]);
    out[k * 4 + 3] = float(m.m[3][k]);
  }
}

// Apply a 3x4 object->world (same layout as Mat4ToObj2World) to a point. Matches
// TransformPoint(matrix4d, p) for affine transforms (instances are affine), in
// float — the same precision LightRT uses for traversal.
inline Vec3 TransformPointO2W(const float o[12], const Vec3 &p) {
  return Vec3{o[0] * p.x + o[1] * p.y + o[2] * p.z + o[3],
              o[4] * p.x + o[5] * p.y + o[6] * p.z + o[7],
              o[8] * p.x + o[9] * p.y + o[10] * p.z + o[11]};
}

// Transform a direction (e.g. a normal) by the 3x3 part of a 3x4 object->world.
// Exact for rigid/uniform-scale instances (the common case); non-uniform scale
// is approximate (true normals need the inverse-transpose) — fine for preview.
inline Vec3 TransformDirO2W(const float o[12], const Vec3 &v) {
  return Vec3{o[0] * v.x + o[1] * v.y + o[2] * v.z,
              o[4] * v.x + o[5] * v.y + o[6] * v.z,
              o[8] * v.x + o[9] * v.y + o[10] * v.z};
}

// Compose two 3x4 object->world transforms (same column-vector layout as
// TransformPointO2W: world = A*p + t). out = outer * inner, i.e. out maps a point
// by applying `inner` first then `outer`: out(p) = outer(inner(p)). Used to flatten
// nested instancing (a prototype that itself contains instancers) into the single
// level a TLAS can express: a leaf's nested-local placement composed with each
// outer placement of its containing prototype.
inline void Compose3x4(const float outer[12], const float inner[12],
                       float out[12]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r * 4 + c] = outer[r * 4 + 0] * inner[0 * 4 + c] +
                       outer[r * 4 + 1] * inner[1 * 4 + c] +
                       outer[r * 4 + 2] * inner[2 * 4 + c];
    }
    out[r * 4 + 3] = outer[r * 4 + 0] * inner[0 * 4 + 3] +
                     outer[r * 4 + 1] * inner[1 * 4 + 3] +
                     outer[r * 4 + 2] * inner[2 * 4 + 3] + outer[r * 4 + 3];
  }
}

// One placement of a BLAS, stored as a compact 3x4 float object->world (48 B vs a
// 128 B matrix4d) — instance arrays dominate footprint on heavily-instanced
// scenes (e.g. Island's 22 M instances). instances[0] is the base at identity.

// Resolve a primary TLAS hit into a world-space TriInfo (positions transformed,
// normal recomputed, diffuse texture sampled). Returns false if the hit indices
// are out of range. `textures` is the shared global texture table.
// CPU port of Storm's ComputeTBNMatrix/PerturbNormal
// (pxr/imaging/hdSt/shaders/surfaceHelpers.glslfx, "Surface Gradient-Based Bump
// Mapping Framework" 2020): the per-fragment screen-space derivatives dFdx/dFdy
// of position and st are replaced by this (planar) triangle's edge and UV deltas,
// which give the same constant gradient across the face. `N` is the geometric
// normal; `Nt` is the tangent-space normal already unpacked to [-1,1]. Returns
// the perturbed world normal, or `N` if the UV parameterization is degenerate.

// Van der Corput radical inverse in `base` — the building block of the Halton
// low-discrepancy sequence. Deterministic in `i`, so multi-sample anti-aliasing
// stays reproducible regardless of thread scheduling.
inline float RadicalInverse(uint32_t i, uint32_t base) {
  float inv = 1.0f / float(base), f = 1.0f, r = 0.0f;
  while (i > 0u) {
    f *= inv;
    r += f * float(i % base);
    i /= base;
  }
  return r;
}

// Sub-pixel sample offset in [0,1)^2 for anti-aliasing sample `s` of `spp`. A
// single sample is pixel-centered (0.5,0.5) so -samples 1 is unchanged; multiple
// samples follow the Halton(2,3) sequence for an even, grid-free distribution
// that actually supersamples every requested sample.
inline void PixelJitter(int s, int spp, float *jx, float *jy) {
  if (spp <= 1) {
    *jx = 0.5f;
    *jy = 0.5f;
  } else {
    *jx = RadicalInverse(uint32_t(s) + 1u, 2u);
    *jy = RadicalInverse(uint32_t(s) + 1u, 3u);
  }
}

}  // namespace lusdr
