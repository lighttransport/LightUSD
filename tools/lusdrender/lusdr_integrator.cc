// SPDX-License-Identifier: Apache-2.0
// lusdrender — the path-tracing integrator: occlusion/visibility, TLAS hit
// resolve, normal/texture sampling, the Shade() recursion, volume raymarch
// compositing, and the multi-threaded RenderImage pixel loop.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "image-writer.hh"
#include "lusdr_context.hh"
#include "tydra/materialx.hh"
#include "../../examples/lusdview/lightrt_mtlx_bridge.hh"

extern "C" {
#include "mtlxrender/bsdf.h"
}

namespace lusdr {

int PurposeAnyHitFilter(void *user, uint32_t prim_id, float, float, float) {
  const PurposeFilter *filter = reinterpret_cast<const PurposeFilter *>(user);
  if (!filter || !filter->tris || filter->first >= filter->tris->size() ||
      size_t(prim_id) >= filter->count ||
      size_t(prim_id) >= filter->tris->size() - filter->first) {
    return 0;
  }
  // Back-face culling is a VISIBILITY property, not an occlusion one: a
  // single-sided surface still blocks light from either side (an opaque
  // occluder, like the raster backends' shadowing), so shadow rays are NOT
  // face-culled -- only the primary/bounce visibility walk is.
  return PurposeVisible((*filter->tris)[filter->first + size_t(prim_id)].purpose_bit,
                        filter->mask)
             ? 1
             : 0;
}

bool IntersectVisibleTriangles(lrt_tri_scene *scene,
                               const std::vector<FlatTri> &tris,
                               const lrt_ray &ray, uint32_t purpose_mask,
                               lrt_hit *hit, size_t tri_first,
                               size_t tri_count) {
  if (!scene || !hit) return false;
  if (tri_first >= tris.size()) return false;
  tri_count = std::min(tri_count, tris.size() - tri_first);
  static constexpr size_t kMaxHits = 64;
  lrt_ray query = ray;
  for (int iter = 0; iter < 8; ++iter) {
    lrt_hit hits[kMaxHits];
    const size_t n = lrt_tri_intersect_n(scene, &query, hits, kMaxHits);
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t prim_id = hits[i].prim_id;
      if (prim_id == LRT_TRI_NO_HIT || size_t(prim_id) >= tri_count) {
        continue;
      }
      const FlatTri &ft = tris[tri_first + size_t(prim_id)];
      if (!PurposeVisible(ft.purpose_bit, purpose_mask)) continue;
      // Back-face cull a single-sided triangle: the ray sees through it to
      // whatever lies behind (USD doubleSided=false, matching the raster
      // backends). The geometric normal points out of the front face, so a ray
      // travelling with it (dot > 0) struck the back. Skip and keep walking the
      // hit list rather than stopping here.
      if (!ft.double_sided) {
        const float d = ft.n.x * query.dir[0] + ft.n.y * query.dir[1] +
                        ft.n.z * query.dir[2];
        if (d > 0.0f) continue;
      }
      *hit = hits[i];
      return true;
    }
    query.tmin = std::nextafter(hits[n - 1].t, query.tmax);
    if (!(query.tmin < query.tmax)) return false;
  }
  return false;
}

bool IntersectVisibleTriangleChunks(
    const std::vector<TriangleSceneChunk> &chunks,
    const std::vector<FlatTri> &tris, const lrt_ray &ray,
    uint32_t purpose_mask, lrt_hit *hit) {
  if (!hit) return false;
  bool found = false;
  float best_t = ray.tmax;
  for (const TriangleSceneChunk &chunk : chunks) {
    if (!chunk.scene || chunk.first >= tris.size() || chunk.count == 0) continue;
    lrt_ray query = ray;
    query.tmax = best_t;
    lrt_hit local;
    if (!IntersectVisibleTriangles(chunk.scene.get(), tris, query,
                                    purpose_mask, &local, chunk.first,
                                    chunk.count))
      continue;
    if (!found || local.t < best_t) {
      best_t = local.t;
      *hit = local;
      hit->prim_id = static_cast<uint32_t>(chunk.first + local.prim_id);
      found = true;
    }
  }
  return found;
}

bool Occluded(lrt_tri_scene *scene, const std::vector<FlatTri> &tris,
              const Vec3 &p, const Vec3 &n, const Vec3 &l, float max_t,
              const DirectScene *direct, uint32_t purpose_mask,
              const std::vector<TriangleSceneChunk> *chunks) {
  // Self-intersection offset must scale with the surface point's magnitude: at
  // large world coordinates a fixed 1e-4 offset is below a float32 ULP (e.g.
  // ULP(40000) ~ 0.005), so `p + n*1e-4 == p` and the shadow ray would start
  // exactly on the surface -> self-shadowing (shadow acne). Use a relative
  // epsilon (~25 ULPs) so the origin clears the surface at any scale.
  const float mag = std::max(std::max(std::fabs(p.x), std::fabs(p.y)),
                             std::fabs(p.z));
  const float eps = std::max(1.0e-4f, mag * 3.0e-6f);
  Vec3 o = Add(p, Mul(n, eps));
  lrt_ray ray;
  ray.org[0] = o.x;
  ray.org[1] = o.y;
  ray.org[2] = o.z;
  ray.tmin = eps;
  ray.dir[0] = l.x;
  ray.dir[1] = l.y;
  ray.dir[2] = l.z;
  ray.tmax = max_t;
  if (scene) {
    PurposeFilter filter{&tris, purpose_mask, 0, tris.size()};
    if (lrt_tri_occluded1_filtered(scene, &ray, PurposeAnyHitFilter, &filter)) {
      return true;
    }
  }
  if (chunks) {
    for (const TriangleSceneChunk &chunk : *chunks) {
      if (!chunk.scene || chunk.first >= tris.size() || chunk.count == 0) continue;
      PurposeFilter filter{&tris, purpose_mask, chunk.first,
                           std::min(chunk.count, tris.size() - chunk.first)};
      if (lrt_tri_occluded1_filtered(chunk.scene.get(), &ray,
                                     PurposeAnyHitFilter, &filter))
        return true;
    }
  }
  if (direct) {
    if (direct->spheres && lrt_tri_occluded1(direct->spheres.get(), &ray)) return true;
    if (direct->has_round_curves()) {
      if (direct->round_curves &&
          lrt_tri_occluded1(direct->round_curves.get(), &ray)) return true;
      for (const CurveSceneChunk &chunk : direct->round_curve_chunks)
        if (lrt_tri_occluded1(chunk.scene.get(), &ray)) return true;
    }
    if (direct->has_flat_curves()) {
      if (direct->flat_curves &&
          lrt_tri_occluded1(direct->flat_curves.get(), &ray)) return true;
      for (const CurveSceneChunk &chunk : direct->flat_curve_chunks)
        if (lrt_tri_occluded1(chunk.scene.get(), &ray)) return true;
    }
    if (direct->points && lrt_tri_occluded1(direct->points.get(), &ray)) return true;
    if (direct->bez_curves &&
        lrt_tri_occluded1(direct->bez_curves.get(), &ray)) return true;
    if (direct->tets && lrt_tri_occluded1(direct->tets.get(), &ray)) return true;
    for (const DirectShape &shape : direct->shapes) {
      DirectHit dh;
      if (IntersectDirectShape(shape, o, l, ray.tmin, ray.tmax, &dh)) return true;
    }
  }
  return false;
}

// Shadow-ray occlusion against a TLAS (two-level/instanced path). The same
// magnitude-scaled self-intersection epsilon as Occluded() is used. No purpose
// filter: purpose-invisible triangles are already culled from the BLAS at build.
bool OccludedTLAS(const lrt_tlas *tlas, const Vec3 &p, const Vec3 &n,
                  const Vec3 &l, float max_t) {
  if (!tlas) return false;
  const float mag = std::max(std::max(std::fabs(p.x), std::fabs(p.y)),
                             std::fabs(p.z));
  const float eps = std::max(1.0e-4f, mag * 3.0e-6f);
  Vec3 o = Add(p, Mul(n, eps));
  lrt_ray ray;
  ray.org[0] = o.x;
  ray.org[1] = o.y;
  ray.org[2] = o.z;
  ray.tmin = eps;
  ray.dir[0] = l.x;
  ray.dir[1] = l.y;
  ray.dir[2] = l.z;
  ray.tmax = max_t;
  return lrt_tlas_occluded1(tlas, &ray, 0xffffffffu) != 0;
}

v3 ToMtlxV3(const Vec3 &v) {
  return v3_make(v.x, v.y, v.z);
}

Vec3 FromMtlxV3(v3 v) {
  return Vec3{v.x, v.y, v.z};
}

OpenPBRParams OpenPBRParamsFromTri(const TriInfo &tri, const Vec3 &normal) {
  OpenPBRParams p{};
  p.base_weight = 1.0f;
  p.base_color = ToMtlxV3(tri.base_color);
  p.diffuse_roughness = 0.0f;
  p.metalness = tri.metallic;
  p.specular_weight = 1.0f;
  p.specular_color = tri.use_specular_workflow
                         ? ToMtlxV3(tri.specular_color)
                         : v3_splat(1.0f);
  p.specular_roughness = tri.roughness;
  p.specular_ior = tri.ior > 0.0f ? tri.ior : 1.5f;
  p.transmission_color = v3_splat(1.0f);
  p.subsurface_color = v3_splat(0.8f);
  p.subsurface_radius = v3_splat(1.0f);
  p.subsurface_scale = 1.0f;
  p.coat_weight = tri.clearcoat;
  p.coat_color = v3_splat(1.0f);
  p.coat_roughness = tri.clearcoat_roughness;
  p.coat_ior = 1.5f;
  p.sheen_color = v3_splat(1.0f);
  p.sheen_roughness = 0.3f;
  p.thin_film_ior = 1.5f;
  p.emission_color = ToMtlxV3(tri.emission);
  p.emission = (tri.emission.x > 0.0f || tri.emission.y > 0.0f ||
                tri.emission.z > 0.0f)
                   ? 1.0f
                   : 0.0f;
  p.normal = ToMtlxV3(normal);
  p.opacity = tri.opacity;
  return p;
}

OpenPBRParams OpenPBRParamsFromLightRt(
    const lightusd::tydra::LightRtOpenPBRParams &src, const Vec3 &normal) {
  OpenPBRParams p{};
  p.base_weight = src.baseWeight;
  p.base_color = v3_make(src.baseColor[0], src.baseColor[1], src.baseColor[2]);
  p.diffuse_roughness = src.diffuseRoughness;
  p.diffuse_model = src.diffuseModel;
  static_assert(lightusd::tydra::kMaxRealtimeDiffuseLobes == MTLX_MAX_DIFFUSE_LOBES,
                "Diffuse lobe limits must match");
  p.diffuse_lobe_count = std::clamp(src.diffuseLobeCount, 0, MTLX_MAX_DIFFUSE_LOBES);
  for (int i = 0; i < p.diffuse_lobe_count; ++i) {
    const auto &lobe = src.diffuseLobes[i];
    p.diffuse_lobes[i].color = v3_make(lobe.color[0], lobe.color[1], lobe.color[2]);
    p.diffuse_lobes[i].weight = lobe.weight;
    p.diffuse_lobes[i].roughness = lobe.roughness;
    p.diffuse_lobes[i].model = lobe.model;
  }
  p.metalness = src.metalness;
  p.specular_weight = src.specularWeight;
  p.specular_color =
      v3_make(src.specularColor[0], src.specularColor[1], src.specularColor[2]);
  p.specular_roughness = src.specularRoughness;
  p.specular_ior = src.specularIor;
  p.transmission = src.transmission;
  p.transmission_color = v3_make(src.transmissionColor[0],
                                 src.transmissionColor[1],
                                 src.transmissionColor[2]);
  p.transmission_depth = src.transmissionDepth;
  p.transmission_scatter = v3_make(src.transmissionScatter[0],
                                   src.transmissionScatter[1],
                                   src.transmissionScatter[2]);
  p.transmission_scatter_anisotropy = src.transmissionScatterAnisotropy;
  p.subsurface = src.subsurface;
  p.subsurface_color =
      v3_make(src.subsurfaceColor[0], src.subsurfaceColor[1],
              src.subsurfaceColor[2]);
  p.subsurface_radius =
      v3_make(src.subsurfaceRadius[0], src.subsurfaceRadius[1],
              src.subsurfaceRadius[2]);
  p.subsurface_scale = src.subsurfaceScale;
  p.coat_weight = src.coatWeight;
  p.coat_color = v3_make(src.coatColor[0], src.coatColor[1], src.coatColor[2]);
  p.coat_roughness = src.coatRoughness;
  p.coat_ior = src.coatIor;
  p.sheen_weight = src.sheenWeight;
  p.sheen_color =
      v3_make(src.sheenColor[0], src.sheenColor[1], src.sheenColor[2]);
  p.sheen_roughness = src.sheenRoughness;
  p.thin_film_weight = src.thinFilmWeight;
  p.thin_film_thickness = src.thinFilmThicknessNm;
  p.thin_film_ior = src.thinFilmIor;
  p.emission = src.emission;
  p.emission_color =
      v3_make(src.emissionColor[0], src.emissionColor[1], src.emissionColor[2]);
  p.normal = ToMtlxV3(normal);
  p.opacity = src.opacity;
  return p;
}

OpenPBRParams OpenPBRParamsForMaterial(
    const TriInfo &tri, const Vec3 &normal,
    const lightusd::tydra::LightRtOpenPBRParams *openpbr) {
  if (openpbr) {
    OpenPBRParams p = OpenPBRParamsFromLightRt(*openpbr, normal);
    // tri.base_color carries the per-hit base-color TEXTURE sample (and any
    // per-corner displayColor); the resolver neutralizes the constant to white
    // when a texture loads, so for untextured materials this equals
    // src.baseColor. The struct's own baseColor is only the constant -- using it
    // alone shaded textured surfaces flat in lightrt-bsdf mode.
    p.base_color = v3_make(tri.base_color.x, tri.base_color.y, tri.base_color.z);
    return p;
  }
  return OpenPBRParamsFromTri(tri, normal);
}

uint32_t FloatBits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  return u;
}

uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

pcg32 MakeBsdfRng(const Vec3 &org, const Vec3 &dir, int depth) {
  uint64_t seed = 0x9e3779b97f4a7c15ULL;
  auto mix = [&](uint32_t v) {
    seed = Mix64(seed ^ uint64_t(v) ^ 0x9e3779b97f4a7c15ULL);
  };
  mix(FloatBits(org.x));
  mix(FloatBits(org.y));
  mix(FloatBits(org.z));
  mix(FloatBits(dir.x));
  mix(FloatBits(dir.y));
  mix(FloatBits(dir.z));
  mix(uint32_t(depth));
  pcg32 rng;
  pcg32_seed(&rng, seed, Mix64(seed ^ 0xd1b54a32d192ed03ULL));
  return rng;
}

bool FiniteColor(const Vec3 &v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 EvalMaterialDirect(const TriInfo &tri, const Vec3 &normal,
                        const Vec3 &wo, const Vec3 &wi,
                        const Vec3 &radiance, const Options &opt,
                        const lightusd::tydra::LightRtOpenPBRParams *openpbr,
                        Vec3 *diffuse_out = nullptr) {
  const float ndotl = std::max(0.0f, Dot(normal, wi));
  if (ndotl <= 0.0f) return Vec3{0.0f, 0.0f, 0.0f};
  if (opt.material_shading != Options::MaterialShading::LightRtBsdf) {
    const Vec3 full = Mul(Mul(tri.base_color, radiance), ndotl);
    if (diffuse_out) *diffuse_out = full;
    return full;
  }
  OpenPBRParams p = OpenPBRParamsForMaterial(tri, normal, openpbr);
  float pdf = 0.0f;
  v3 fd, fs;
  bsdf_eval_lobes(&p, ToMtlxV3(normal), ToMtlxV3(wo), ToMtlxV3(wi), &fd, &fs,
                  &pdf);
  const Vec3 diffuse = Mul(Mul(FromMtlxV3(fd), radiance), ndotl);
  const Vec3 specular = Mul(Mul(FromMtlxV3(fs), radiance), ndotl);
  if (diffuse_out) *diffuse_out = diffuse;
  return Add(diffuse, specular);
}

// ---------------------------------------------------------------------------
// Next-event estimation for SPHERE lights, with power-heuristic MIS.
//
// Every finite light used to be collapsed to a point: irradiance = I / d², with
// an ad-hoc `max(1, area)` fudge for the ones that are obviously not points.
// That is exactly right only in the far field, and increasingly wrong as a
// sphere light gets large or close -- no penumbra, and a light you are standing
// inside of divides by ~0.
//
// A sphere is the one light whose geometry is fully described by what
// PreviewLight already carries (position + radius; no tangent frame needed), so
// it is where area sampling can be added without changing the light plumbing.
// The other kinds keep the analytic path for now.
//
// Radiance vs intensity: PreviewLight::radiance holds an INTENSITY (the punctual
// path divides it by d²). A sphere of radius r emitting uniform radiance L
// presents projected area pi*r², so L = I / (pi*r²) reproduces the same far-field
// irradiance -- i.e. this is energy-preserving against the old look, and only
// differs where the punctual approximation was wrong to begin with.
struct LightSample {
  Vec3 wi{0.0f, 0.0f, 0.0f};
  float dist{0.0f};
  float pdf{0.0f};        // solid angle
  Vec3 radiance{0.0f, 0.0f, 0.0f};
};

void OrthoBasis(const Vec3 &w, Vec3 *u, Vec3 *v) {
  const Vec3 a = (std::fabs(w.x) > 0.9f) ? Vec3{0.0f, 1.0f, 0.0f}
                                         : Vec3{1.0f, 0.0f, 0.0f};
  *u = Normalize(Cross(a, w));
  *v = Cross(w, *u);
}

Vec3 SphereLightRadiance(const PreviewLight &light) {
  // PreviewLight.radiance IS the sphere's emitted radiance: color * intensity *
  // 2^exposure, divided by the surface area when inputs:normalize is authored
  // (UsdLux semantics, applied at collection). The old convention here divided
  // by pi*r^2 unconditionally -- an implicit, always-on normalize that ignored
  // the authored flag and disagreed with lusdview and the other shapes.
  return light.radiance;
}

// Uniform cone sampling of the sphere's visible solid angle.
bool SampleSphereLight(const PreviewLight &light, const Vec3 &p, float u1,
                       float u2, LightSample *out) {
  const Vec3 d = Sub(light.position, p);
  const float dist = Length(d);
  const float r = light.radius;
  // Degenerate, or the shading point is inside the sphere: the cone is not
  // defined. Fall back to the punctual path rather than producing nonsense.
  if (r <= 1.0e-5f || dist <= r + 1.0e-5f) return false;

  const Vec3 w = Mul(d, 1.0f / dist);
  Vec3 su, sv;
  OrthoBasis(w, &su, &sv);

  const float sin_max_sq = (r * r) / (dist * dist);
  const float cos_max = std::sqrt(std::max(0.0f, 1.0f - sin_max_sq));
  const float cos_theta = 1.0f - u1 * (1.0f - cos_max);
  const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
  const float phi = 2.0f * MTLX_PI * u2;

  out->wi = Normalize(Add(Add(Mul(su, std::cos(phi) * sin_theta),
                              Mul(sv, std::sin(phi) * sin_theta)),
                          Mul(w, cos_theta)));
  // Distance to the sphere along wi (for the shadow ray); the cone always hits.
  const float b = Dot(out->wi, d);
  const float disc = std::max(0.0f, b * b - (dist * dist - r * r));
  out->dist = std::max(0.0f, b - std::sqrt(disc));
  const float solid_angle = 2.0f * MTLX_PI * (1.0f - cos_max);
  out->pdf = (solid_angle > 1.0e-8f) ? (1.0f / solid_angle) : 0.0f;
  out->radiance = SphereLightRadiance(light);
  return out->pdf > 0.0f;
}

// The solid-angle pdf SampleSphereLight would have had for direction `wi` --
// zero if the direction misses the sphere. This is the light-side pdf that the
// BSDF-sampled branch needs for its MIS weight.
float SphereLightPdf(const PreviewLight &light, const Vec3 &p, const Vec3 &wi) {
  const Vec3 d = Sub(light.position, p);
  const float dist = Length(d);
  const float r = light.radius;
  if (r <= 1.0e-5f || dist <= r + 1.0e-5f) return 0.0f;
  const float b = Dot(wi, d);
  if (b <= 0.0f) return 0.0f;  // behind us
  if ((dist * dist - b * b) > r * r) return 0.0f;  // ray misses the sphere
  const float cos_max =
      std::sqrt(std::max(0.0f, 1.0f - (r * r) / (dist * dist)));
  const float solid_angle = 2.0f * MTLX_PI * (1.0f - cos_max);
  return (solid_angle > 1.0e-8f) ? (1.0f / solid_angle) : 0.0f;
}

// --- Shaped area lights: Rect / Disk / Cylinder ------------------------------
//
// These were punctual: collapsed to their center, so a 10-unit rect cast exactly
// the same razor-sharp shadow as a bare point. They are now sampled over their
// SURFACE, with the same NEE + power-heuristic MIS the sphere gets.
//
// Radiance: the punctual path computed irradiance as
//   E = radiance * cos_emit * max(1, area) / d^2,
// and the area estimator integrates L * cos_emit * cos_theta / d^2 over the
// surface, which for a distant light is L * area * cos_emit / d^2. So the surface
// radiance L IS PreviewLight::radiance, and the far field is preserved -- except
// for the max(1, area) clamp, which the area estimator drops. A light SMALLER than
// one square unit therefore gets dimmer (correctly: the clamp was inventing energy
// for it), while every light at or above unit area is unchanged at distance.

// Does `wi` from `p` hit the light's surface? Fills the distance and the surface
// normal at the hit, which the pdf and the shadow ray both need.
bool IntersectAreaLight(const PreviewLight &light, const Vec3 &p, const Vec3 &wi,
                        float *out_t, Vec3 *out_n) {
  const Vec3 c = light.position;
  if (light.kind == PreviewLight::Kind::Rect ||
      light.kind == PreviewLight::Kind::Disk) {
    const Vec3 nl = light.normal;
    const float denom = Dot(wi, nl);
    if (std::fabs(denom) < 1.0e-8f) return false;  // parallel to the plane
    const float t = Dot(Sub(c, p), nl) / denom;
    if (t <= 1.0e-5f) return false;
    const Vec3 q = Sub(Add(p, Mul(wi, t)), c);
    const float du = Dot(q, light.axis_u);
    const float dv = Dot(q, light.axis_v);
    if (light.kind == PreviewLight::Kind::Rect) {
      if (std::fabs(du) > 0.5f * light.width ||
          std::fabs(dv) > 0.5f * light.height) {
        return false;
      }
    } else if (du * du + dv * dv > light.radius * light.radius) {
      return false;
    }
    *out_t = t;
    *out_n = nl;
    return true;
  }
  if (light.kind == PreviewLight::Kind::Cylinder) {
    // Lateral surface of a finite cylinder about axis_u, radius r, length L.
    const Vec3 a = light.axis_u;
    const float r = light.radius;
    if (r <= 1.0e-5f) return false;
    const Vec3 oc = Sub(p, c);
    const Vec3 d_perp = Sub(wi, Mul(a, Dot(wi, a)));
    const Vec3 o_perp = Sub(oc, Mul(a, Dot(oc, a)));
    const float A = Dot(d_perp, d_perp);
    if (A < 1.0e-12f) return false;  // travelling along the axis
    const float B = 2.0f * Dot(o_perp, d_perp);
    const float C = Dot(o_perp, o_perp) - r * r;
    const float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f) return false;
    const float sq = std::sqrt(disc);
    for (const float t : {(-B - sq) / (2.0f * A), (-B + sq) / (2.0f * A)}) {
      if (t <= 1.0e-5f) continue;
      const Vec3 q = Sub(Add(p, Mul(wi, t)), c);
      if (std::fabs(Dot(q, a)) > 0.5f * light.length) continue;  // past an end cap
      const Vec3 radial = Sub(q, Mul(a, Dot(q, a)));
      if (Length(radial) < 1.0e-8f) continue;
      *out_t = t;
      *out_n = Normalize(radial);
      return true;
    }
    return false;
  }
  return false;
}

// Uniform-area sampling of the light's surface, converted to a solid-angle pdf
// (pdf_A * d^2 / cos_emit) so it is directly comparable with the BSDF's pdf.
bool SampleAreaLight(const PreviewLight &light, const Vec3 &p, float u1, float u2,
                     LightSample *out) {
  if (!(light.area > 1.0e-8f)) return false;
  Vec3 q, nl;
  if (light.kind == PreviewLight::Kind::Rect) {
    q = Add(light.position,
            Add(Mul(light.axis_u, (u1 - 0.5f) * light.width),
                Mul(light.axis_v, (u2 - 0.5f) * light.height)));
    nl = light.normal;
  } else if (light.kind == PreviewLight::Kind::Disk) {
    const float rr = light.radius * std::sqrt(std::max(0.0f, u1));
    const float phi = 2.0f * MTLX_PI * u2;
    q = Add(light.position,
            Add(Mul(light.axis_u, rr * std::cos(phi)),
                Mul(light.axis_v, rr * std::sin(phi))));
    nl = light.normal;
  } else if (light.kind == PreviewLight::Kind::Mesh) {
    const float root = std::sqrt(std::max(0.0f, u1));
    const Vec3 p0 = Sub(light.position, Mul(Add(light.axis_u, light.axis_v), 1.0f / 3.0f));
    q = Add(p0, Add(Mul(light.axis_u, root * (1.0f - u2)),
                    Mul(light.axis_v, root * u2)));
    nl = light.normal;
  } else if (light.kind == PreviewLight::Kind::Cylinder) {
    Vec3 b1, b2;
    OrthonormalBasis(light.axis_u, &b1, &b2);
    const float phi = 2.0f * MTLX_PI * u2;
    const Vec3 radial = Add(Mul(b1, std::cos(phi)), Mul(b2, std::sin(phi)));
    q = Add(light.position, Add(Mul(light.axis_u, (u1 - 0.5f) * light.length),
                                Mul(radial, light.radius)));
    nl = radial;  // emits radially outward
  } else {
    return false;
  }

  const Vec3 d = Sub(q, p);
  const float dist = Length(d);
  if (dist <= 1.0e-5f) return false;
  out->wi = Mul(d, 1.0f / dist);
  const float cos_emit = Dot(nl, Mul(out->wi, -1.0f));
  if (cos_emit <= 1.0e-6f) return false;  // shading point is behind the emitter
  out->dist = dist;
  out->pdf = (dist * dist) / (cos_emit * light.area);
  out->radiance = light.radiance;
  return out->pdf > 0.0f && std::isfinite(out->pdf);
}

// The solid-angle pdf SampleAreaLight would have had for `wi`; 0 if it misses.
float AreaLightPdf(const PreviewLight &light, const Vec3 &p, const Vec3 &wi) {
  if (!(light.area > 1.0e-8f)) return 0.0f;
  float t = 0.0f;
  Vec3 nl;
  if (!IntersectAreaLight(light, p, wi, &t, &nl)) return 0.0f;
  const float cos_emit = Dot(nl, Mul(wi, -1.0f));
  if (cos_emit <= 1.0e-6f) return 0.0f;
  const float pdf = (t * t) / (cos_emit * light.area);
  return std::isfinite(pdf) ? pdf : 0.0f;
}

// Power heuristic (beta = 2). Both pdfs are in solid angle, so they are directly
// comparable; a delta lobe (pdf_b reported as 1.0 with `specular`) must never
// reach this -- the caller skips MIS entirely there.
float PowerHeuristic(float pdf_a, float pdf_b) {
  const float a = pdf_a * pdf_a;
  const float b = pdf_b * pdf_b;
  const float sum = a + b;
  return (sum > 1.0e-12f) ? (a / sum) : 0.0f;
}

Vec3 EvalMaterialIblDiffuse(
    const TriInfo &tri, const Vec3 &normal, const Vec3 &wo,
    const Vec3 &diffuse_irradiance, const Options &opt,
    const lightusd::tydra::LightRtOpenPBRParams *openpbr) {
  if (opt.material_shading != Options::MaterialShading::LightRtBsdf) {
    return Vec3{0.0f, 0.0f, 0.0f};
  }
  OpenPBRParams p = OpenPBRParamsForMaterial(tri, normal, openpbr);
  float pdf = 0.0f;
  v3 fd, fs;
  // ONLY the diffuse lobe belongs against the irradiance map. Using the full BSDF
  // here (and again against the prefiltered reflection map below) counted both
  // lobes twice over: a white Lambert surface in a uniform furnace came out at
  // 1.32x the radiance of the dome lighting it, where it must come out at 1.0x.
  bsdf_eval_lobes(&p, ToMtlxV3(normal), ToMtlxV3(wo), ToMtlxV3(normal), &fd, &fs,
                  &pdf);
  Vec3 brdf = FromMtlxV3(fd);
  const float scale = MTLX_PI * tri.occlusion;
  return Vec3{brdf.x * diffuse_irradiance.x * scale,
              brdf.y * diffuse_irradiance.y * scale,
              brdf.z * diffuse_irradiance.z * scale};
}

// Environment specular for a prefiltered radiance, via the bounded split-sum
// approximation: spec_env * (F0*A + B), where (A,B) come from the environment
// BRDF LUT. This is used for BOTH shading modes. The old lightrt-bsdf path
// instead evaluated the analytic microfacet BRDF at the exact mirror direction
// and multiplied it by the prefiltered radiance -- but the prefilter already
// integrated the NDF, so at the mirror direction D = 1/(pi*alpha) blew the term
// up without bound as roughness -> 0 (a ~280x-too-bright highlight at
// roughness 0.05). f0 already carries the material's reflectance (OpenPBR
// metalness/specularColor/ior in bsdf mode), so the split-sum stays material-
// correct while remaining bounded.
Vec3 EvalIblSpecularSplitSum(const Vec3 &spec_env, const Vec3 &f0, float brdf_a,
                             float brdf_b) {
  return Mul(spec_env, Add(Mul(f0, brdf_a), Vec3{brdf_b, brdf_b, brdf_b}));
}

bool IntersectDirectScene(const DirectScene *direct, const Vec3 &ray_org,
                          const Vec3 &ray_dir, float tmin, float tmax,
                          DirectHit *best) {
  if (!direct || !best) return false;
  lrt_ray ray;
  ray.org[0] = ray_org.x;
  ray.org[1] = ray_org.y;
  ray.org[2] = ray_org.z;
  ray.tmin = tmin;
  ray.dir[0] = ray_dir.x;
  ray.dir[1] = ray_dir.y;
  ray.dir[2] = ray_dir.z;
  ray.tmax = tmax;
  auto test_scene = [&](lrt_tri_scene *scene, const std::vector<TriInfo> &info,
                        size_t first, size_t count, bool sphere) {
    if (!scene) return;
    lrt_hit h;
    if (!lrt_tri_intersect1(scene, &ray, &h) || h.prim_id == LRT_TRI_NO_HIT ||
        size_t(h.prim_id) >= count || h.t >= best->t) {
      return;
    }
    const TriInfo &ti = info[first + size_t(h.prim_id)];
    Vec3 p = Add(ray_org, Mul(ray_dir, h.t));
    best->t = h.t;
    if (sphere) {
      Vec3 c = ti.p0;
      best->n = Normalize(Sub(p, c));
    } else {
      best->n = Normalize(Cross(Sub(ti.p1, ti.p0), Sub(ti.p2, ti.p0)));
      if (Length(best->n) < 1.0e-6f) {
        best->n = Normalize(Sub(ray_org, p));
      }
    }
    best->base_color = ti.base_color;
    best->emission = ti.emission;
    best->hit = true;
  };
  test_scene(direct->spheres.get(), direct->sphere_info, 0,
             direct->sphere_info.size(), true);
  auto test_curve_chunks = [&](const std::vector<CurveSceneChunk> &chunks) {
    for (const CurveSceneChunk &chunk : chunks)
      test_scene(chunk.scene.get(), chunk.info, 0, chunk.info.size(), false);
  };
  test_scene(direct->round_curves.get(), direct->round_curve_info, 0,
             direct->round_curve_info.size(), false);
  test_curve_chunks(direct->round_curve_chunks);
  test_scene(direct->flat_curves.get(), direct->flat_curve_info, 0,
             direct->flat_curve_info.size(), false);
  test_curve_chunks(direct->flat_curve_chunks);
  test_scene(direct->points.get(), direct->point_info, 0,
             direct->point_info.size(), true);
  auto test_ellipses = [&](const EllipseSceneChunk &chunk) {
    lrt_tri_scene *scene = chunk.scene.get();
    if (!scene) return;
    lrt_hit h;
    if (!lrt_tri_intersect1(scene, &ray, &h) || h.prim_id == LRT_TRI_NO_HIT ||
        size_t(h.prim_id) >= chunk.info.size() || h.t >= best->t) return;
    const TriInfo &ti = chunk.info[size_t(h.prim_id)];
    best->t = h.t;
    best->n = Normalize(ti.p1);
    if (Length(best->n) < 1.0e-6f) best->n = Normalize(Sub(ray_org, Add(ray_org, Mul(ray_dir, h.t))));
    best->base_color = ti.base_color;
    best->emission = ti.emission;
    best->hit = true;
  };
  if (!direct->ellipse_chunks.empty()) {
    for (const EllipseSceneChunk &chunk : direct->ellipse_chunks)
      test_ellipses(chunk);
  }
  test_scene(direct->bez_curves.get(), direct->bez_curve_info, 0,
             direct->bez_curve_info.size(), false);
  if (direct->tets) {
    lrt_hit h;
    if (lrt_tri_intersect1(direct->tets.get(), &ray, &h) &&
        h.prim_id != LRT_TRI_NO_HIT &&
        size_t(h.prim_id) < direct->tet_prims.size() && h.t < best->t) {
      const TetPrim &tet = direct->tet_prims[size_t(h.prim_id)];
      float t = h.t;
      Vec3 n;
      if (IntersectTetPrim(tet, ray_org, ray_dir, tmin, best->t, &t, &n)) {
        best->t = t;
        best->n = n;
        best->base_color = tet.base_color;
        best->emission = tet.emission;
        best->hit = true;
      }
    }
  }
  for (const DirectShape &shape : direct->shapes) {
    DirectHit h;
    if (IntersectDirectShape(shape, ray_org, ray_dir, tmin, best->t, &h) &&
        h.t < best->t) {
      *best = h;
    }
  }
  return best->hit;
}

// A bottom-level acceleration structure (BLAS): one prototype's (or the base
// scene's) geometry in its OWN local space, with the parallel per-triangle
// attributes. Instanced via a TLAS; the prototype geometry is stored once
// regardless of how many times it is placed.

// 3x4 row-major object->world for LightRT (p' = L*p + t), derived from a
// row-vector matrix4d so it matches TransformPoint(m, p) exactly.
Vec3 PerturbNormalStorm(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                        const Vec3 &N, float u0, float v0, float u1, float v1,
                        float u2, float v2, const Vec3 &Nt) {
  Vec3 dP1 = Sub(p1, p0), dP2 = Sub(p2, p0);
  Vec3 sigmaX = Sub(dP1, Mul(N, Dot(dP1, N)));
  Vec3 sigmaY = Sub(dP2, Mul(N, Dot(dP2, N)));
  float flipSign = Dot(dP2, Cross(N, dP1)) < 0.0f ? -1.0f : 1.0f;
  const float du1 = u1 - u0, dv1 = v1 - v0, du2 = u2 - u0, dv2 = v2 - v0;
  const float det = du1 * dv2 - dv1 * du2;
  const float signDet = det < 0.0f ? -1.0f : 1.0f;
  // First column of the inverse st matrix, scaled by sign (not divided by det).
  Vec3 T = Add(Mul(sigmaX, signDet * dv2), Mul(sigmaY, signDet * (-dv1)));
  if (std::fabs(det) <= 0.0f || Length(T) < 1.0e-12f) return N;
  T = Normalize(T);
  Vec3 B = Mul(Cross(N, T), signDet * flipSign);
  Vec3 pert = Add(Add(Mul(T, Nt.x), Mul(B, Nt.y)), Mul(N, Nt.z));
  float l = Length(pert);
  return l > 1.0e-12f ? Mul(pert, 1.0f / l) : N;
}

// Sample a normal map at (u,v,lod) and unpack to a tangent-space normal via the
// texture's scale/bias (UsdUVTexture convention).
Vec3 SampleTangentNormal(const Texture &nm, float u, float v, float lod) {
  Vec3 s = nm.sample(u, v, lod);  // raw [0,1] (normal maps are sourceColorSpace=raw)
  return Vec3{s.x * nm.scale.x + nm.bias.x, s.y * nm.scale.y + nm.bias.y,
              s.z * nm.scale.z + nm.bias.z};
}

inline float ChannelOf(const Vec3 &c, uint8_t ch) {
  return ch == 1 ? c.y : ch == 2 ? c.z : c.x;  // r/g/b (a not sampled -> r)
}

// Map a world-space direction into the DomeLight's local frame before the
// lat-long lookup, so an authored dome rotation orients the environment. A no-op
// (identity passthrough) when the dome carries no non-identity rotation.
inline Vec3 EnvDir(const IblCache &ibl, const Vec3 &d) {
  if (!ibl.rotated) return d;
  return Vec3{Dot(d, ibl.rx), Dot(d, ibl.ry), Dot(d, ibl.rz)};
}

// Sample a scalar (roughness/metallic/opacity) texture's channel into [0,1].
// Reads the channel directly (incl. alpha, ch=3), so an opacity/scalar input
// connected to a UsdUVTexture's outputs:a reads alpha rather than red.
inline float SampleScalarTex(const std::vector<Texture> &textures, int32_t id,
                             uint8_t ch, float u, float v, float lod,
                             float scale = 1.0f, float bias = 0.0f) {
  if (id < 0 || size_t(id) >= textures.size())
    return std::numeric_limits<float>::quiet_NaN();
  return textures[size_t(id)].sample_channel(u, v, lod, int(ch)) * scale + bias;
}

// Anisotropic scalar sample: when a ray-differential footprint is available, the
// channel is read from the same parallelogram-footprint filter the color textures
// use (so a grazing roughness/metallic/occlusion/opacity map is anti-aliased along
// its major axis instead of over-blurred by isotropic trilinear). Falls back to a
// full-res lookup when no footprint (lod 0), matching the prior result there.
inline float SampleScalarTexAniso(const std::vector<Texture> &textures,
                                  int32_t id, uint8_t ch, float u, float v,
                                  bool have_fp, float dudx, float dvdx,
                                  float dudy, float dvdy, int max_aniso,
                                  float scale = 1.0f, float bias = 0.0f) {
  if (id < 0 || size_t(id) >= textures.size())
    return std::numeric_limits<float>::quiet_NaN();
  const Texture &tx = textures[size_t(id)];
  const float raw = have_fp ? tx.sample_channel_aniso(u, v, dudx, dvdx, dudy,
                                                      dvdy, max_aniso, int(ch))
                            : tx.sample_channel(u, v, 0.0f, int(ch));
  return raw * scale + bias;
}

// Anisotropic tangent-space normal sample (footprint-filtered raw RGB, then the
// UsdPreviewSurface scale/bias unpack). Falls back to a full-res lookup with no
// footprint.
Vec3 SampleTangentNormalAniso(const Texture &nm, float u, float v, bool have_fp,
                              float dudx, float dvdx, float dudy, float dvdy,
                              int max_aniso) {
  Vec3 s = have_fp ? nm.sample_aniso(u, v, dudx, dvdx, dudy, dvdy, max_aniso)
                   : nm.sample(u, v, 0.0f);
  return Vec3{s.x * nm.scale.x + nm.bias.x, s.y * nm.scale.y + nm.bias.y,
              s.z * nm.scale.z + nm.bias.z};
}

// Per-pixel ray differential: the camera rays for the +1 pixel neighbors in
// screen x and y (origin + direction each, to cover both pinhole and ortho).

// Barycentric (e1,e2 coords) of point Q on triangle p0,p1,p2 -> interpolated UV.
inline void TriPointUV(const Vec3 &Q, const Vec3 &p0, const Vec3 &e1,
                       const Vec3 &e2, float d00, float d01, float d11,
                       float invden, float u0, float v0, float du1, float dv1,
                       float du2, float dv2, float *u, float *v) {
  Vec3 q = Sub(Q, p0);
  float d20 = Dot(q, e1), d21 = Dot(q, e2);
  float a = (d11 * d20 - d01 * d21) * invden;
  float b = (d00 * d21 - d01 * d20) * invden;
  *u = u0 + a * du1 + b * du2;
  *v = v0 + a * dv1 + b * dv2;
}

// Mip LOD from the UV footprint (OpenGL isotropic formula): the larger of the
// two screen-axis texel spans. Returns 0 (full res) when textures aren't
// minified or differentials are unavailable.
float TextureLod(float dudx, float dvdx, float dudy, float dvdy, int w, int h) {
  float px = dudx * float(w), py = dvdx * float(h);
  float qx = dudy * float(w), qy = dvdy * float(h);
  float rho2 = std::max(px * px + py * py, qx * qx + qy * qy);
  if (!(rho2 > 1.0e-20f)) return 0.0f;
  return 0.5f * std::log2(rho2);
}

// Per-hit UV footprint via ray differentials: intersect the primary + neighbor
// rays with the hit triangle's plane and difference their UVs. Outputs the UV
// gradients per screen pixel. Returns false if the parameterization/plane is
// degenerate (caller then uses lod 0).
bool ComputeUVFootprint(const Vec3 &org, const Vec3 &dir, const RayDiff &rd,
                        const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                        const Vec3 &N, float u0, float v0, float u1, float v1,
                        float u2, float v2, float *dudx, float *dvdx,
                        float *dudy, float *dvdy) {
  if (!rd.valid) return false;
  Vec3 e1 = Sub(p1, p0), e2 = Sub(p2, p0);
  float d00 = Dot(e1, e1), d01 = Dot(e1, e2), d11 = Dot(e2, e2);
  float den = d00 * d11 - d01 * d01;
  if (std::fabs(den) < 1.0e-20f) return false;
  float invden = 1.0f / den;
  float du1 = u1 - u0, dv1 = v1 - v0, du2 = u2 - u0, dv2 = v2 - v0;
  float pd = Dot(p0, N);
  auto hit_uv = [&](const Vec3 &o, const Vec3 &d, float *u, float *v) -> bool {
    float denom = Dot(d, N);
    if (std::fabs(denom) < 1.0e-12f) return false;
    float t = (pd - Dot(o, N)) / denom;
    if (t <= 0.0f) return false;
    TriPointUV(Add(o, Mul(d, t)), p0, e1, e2, d00, d01, d11, invden, u0, v0, du1,
               dv1, du2, dv2, u, v);
    return true;
  };
  float u, v, ux, vx, uy, vy;
  if (!hit_uv(org, dir, &u, &v) || !hit_uv(rd.ox, rd.dx, &ux, &vx) ||
      !hit_uv(rd.oy, rd.dy, &uy, &vy)) {
    return false;
  }
  *dudx = ux - u;
  *dvdx = vx - v;
  *dudy = uy - u;
  *dvdy = vy - v;
  return true;
}

bool MakeMaterialXHitContext(const Vec3 &ray_org, const Vec3 &ray_dir,
                             const RayDiff &rd, const Vec3 &p0,
                             const Vec3 &p1, const Vec3 &p2,
                             const float *uv, float b1, float b2,
                             MaterialXHitContext *out) {
  if (!uv || !out) return false;
  const float b0 = 1.0f - b1 - b2;
  out->has_uv = true;
  out->uv[0] = b0 * uv[0] + b1 * uv[2] + b2 * uv[4];
  out->uv[1] = b0 * uv[1] + b1 * uv[3] + b2 * uv[5];

  const Vec3 e1 = Sub(p1, p0);
  const Vec3 e2 = Sub(p2, p0);
  const float du1 = uv[2] - uv[0];
  const float dv1 = uv[3] - uv[1];
  const float du2 = uv[4] - uv[0];
  const float dv2 = uv[5] - uv[1];
  const float det = du1 * dv2 - du2 * dv1;
  if (std::fabs(det) > 1.0e-12f) {
    const float invdet = 1.0f / det;
    out->dpdu = Mul(Sub(Mul(e1, dv2), Mul(e2, dv1)), invdet);
    out->dpdv = Mul(Sub(Mul(e2, du1), Mul(e1, du2)), invdet);
  }

  const Vec3 geometric_normal = Normalize(Cross(e1, e2));
  float dudx = 0.0f, dvdx = 0.0f, dudy = 0.0f, dvdy = 0.0f;
  if (ComputeUVFootprint(ray_org, ray_dir, rd, p0, p1, p2,
                         geometric_normal, uv[0], uv[1], uv[2], uv[3],
                         uv[4], uv[5], &dudx, &dvdx, &dudy, &dvdy)) {
    out->uv_dx[0] = dudx;
    out->uv_dx[1] = dvdx;
    out->uv_dy[0] = dudy;
    out->uv_dy[1] = dvdy;
    out->has_uv_derivatives = true;
  }
  return true;
}

bool ResolveTLASHit(const lrt_tlas_hit &th, const std::vector<Blas> &blas,
                    const std::vector<InstanceRT> &instances,
                    const std::vector<Texture> *textures, const Vec3 &ray_org,
                    const Vec3 &ray_dir, const RayDiff &rd, TriInfo *out,
                    const lightusd::tydra::LightRtOpenPBRParams **out_openpbr,
                    Vec3* geometric_normal, Vec3* shading_normal,
                    MaterialXHitContext *materialx_hit) {
  if (out_openpbr) *out_openpbr = nullptr;
  if (materialx_hit) *materialx_hit = MaterialXHitContext{};
  if (size_t(th.inst_id) >= instances.size()) return false;
  const InstanceRT &inst = instances[size_t(th.inst_id)];
  if (size_t(inst.blas_id) >= blas.size()) return false;
  const Blas &b = blas[size_t(inst.blas_id)];
  // Curve BLAS: resolve the segment's local TriInfo, transform endpoints to world
  // (no UVs/textures for curves — matches the DirectScene curve path).
  if (b.is_curve) {
    const size_t si = size_t(th.prim_id);
    if (si * 6 + 5 >= b.curve_seg.size() || si >= b.curve_seg_mat.size())
      return false;
    const float *sg = &b.curve_seg[si * 6];
    const Vec3 p0{sg[0], sg[1], sg[2]};
    const Vec3 p1{sg[3], sg[4], sg[5]};
    // p2 is the synthetic normal helper AppendLinearCurveStrands used (p0 + +Y);
    // recomputed here instead of stored, so the hit normal is byte-identical.
    const Vec3 p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
    Vec3 wp0 = TransformPointO2W(inst.o2w, p0);
    Vec3 wp1 = TransformPointO2W(inst.o2w, p1);
    Vec3 wp2 = TransformPointO2W(inst.o2w, p2);
    const uint32_t mid = b.curve_seg_mat[si];
    *out = CombineTriMat(mid < b.mat_table.size() ? b.mat_table[mid] : TriMat{});
    if (out_openpbr && mid < b.mat_table.size()) {
      const TriMat &mat = b.mat_table[mid];
      if (mat.openpbr_id < b.openpbr_table.size()) {
        *out_openpbr = &b.openpbr_table[mat.openpbr_id];
      }
    }
    out->p0 = wp0;
    out->p1 = wp1;
    out->p2 = wp2;
    out->n = Normalize(Cross(Sub(wp1, wp0), Sub(wp2, wp0)));
    if (geometric_normal) *geometric_normal = out->n;
    if (shading_normal) *shading_normal = out->n;
    return true;
  }
  if (size_t(th.prim_id) >= b.tris.size()) return false;
  const TriStore &ts = b.tris[size_t(th.prim_id)];
  TriInfo lt;
  // Per-corner displayColor/displayOpacity (RGBA), barycentrically interpolated.
  // Phase 5: when colors were reordered into leaf-slot order (-coherentColors), the
  // hit indexes by the leaf slot (cache-coherent) instead of prim_id; otherwise
  // the prim_id-ordered array is used. Same 12 bytes either way.
  const uint8_t *cc = nullptr;
  if (!b.tri_colors_slot.empty()) {
    const uint32_t slot = lrt_tri_get_slot(b.scene, th.prim_id);
    if (slot != LRT_TRI_NO_HIT && size_t(slot) * 12 + 11 < b.tri_colors_slot.size())
      cc = &b.tri_colors_slot[size_t(slot) * 12];
  } else if (size_t(th.prim_id) * 12 + 11 < b.tri_colors.size()) {
    cc = &b.tri_colors[size_t(th.prim_id) * 12];
  }
  // Local-space triangle positions: from the vertex soup when present, else
  // recovered from the BVH leaves (the soup was freed post-build to save
  // 36 B/tri). lrt_tri_get_verts is byte-exact with the soup for mesh coords.
  float lv[9];
  if (!b.vertices.empty()) {
    if (size_t(th.prim_id) * 9 + 8 >= b.vertices.size()) return false;
    std::memcpy(lv, &b.vertices[size_t(th.prim_id) * 9], sizeof(lv));
  } else if (!lrt_tri_get_verts(b.scene, th.prim_id, &lv[0], &lv[3], &lv[6])) {
    return false;
  }
  Vec3 wp0 = TransformPointO2W(inst.o2w, Vec3{lv[0], lv[1], lv[2]});
  Vec3 wp1 = TransformPointO2W(inst.o2w, Vec3{lv[3], lv[4], lv[5]});
  Vec3 wp2 = TransformPointO2W(inst.o2w, Vec3{lv[6], lv[7], lv[8]});
  uint32_t mid = ts.mat_id;
  if (size_t(mid) < b.mat_table.size() &&
      Dot(Cross(Sub(wp1, wp0), Sub(wp2, wp0)), ray_dir) > 0.0f) {
    const uint32_t back = b.mat_table[mid].backface_id;
    if (size_t(back) < b.mat_table.size()) mid = back;
  }
  lt = CombineTriMat(size_t(mid) < b.mat_table.size()
                         ? b.mat_table[size_t(mid)]
                         : TriMat{});
  if (out_openpbr && size_t(mid) < b.mat_table.size()) {
    const TriMat &mat = b.mat_table[size_t(mid)];
    if (mat.openpbr_id < b.openpbr_table.size()) {
      *out_openpbr = &b.openpbr_table[mat.openpbr_id];
    }
  }
  if (cc) {
    const float w1 = th.u, w2 = th.v, w0 = 1.0f - w1 - w2;
    const float s = 1.0f / 255.0f;
    lt.base_color = Vec3{(w0 * cc[0] + w1 * cc[4] + w2 * cc[8]) * s,
                         (w0 * cc[1] + w1 * cc[5] + w2 * cc[9]) * s,
                         (w0 * cc[2] + w1 * cc[6] + w2 * cc[10]) * s};
    lt.opacity = (w0 * cc[3] + w1 * cc[7] + w2 * cc[11]) * s;
  }
  *out = lt;
  out->p0 = wp0;
  out->p1 = wp1;
  out->p2 = wp2;
  out->n = Normalize(Cross(Sub(wp1, wp0), Sub(wp2, wp0)));
  if (geometric_normal) *geometric_normal = out->n;
  // Smooth shading: interpolate per-corner authored normals (local) and transform
  // by the instance, falling back to the geometric normal above.
  if (size_t(th.prim_id) * 9 + 8 < b.tri_normals.size()) {
    const float *nn = &b.tri_normals[size_t(th.prim_id) * 9];
    const float w1 = th.u, w2 = th.v, w0 = 1.0f - w1 - w2;
    Vec3 ln{w0 * nn[0] + w1 * nn[3] + w2 * nn[6],
            w0 * nn[1] + w1 * nn[4] + w2 * nn[7],
            w0 * nn[2] + w1 * nn[5] + w2 * nn[8]};
    Vec3 wn = TransformNormalO2W(inst.o2w, ln);
    if (Length(wn) > 1.0e-12f) out->n = Normalize(wn);
  }
  if (shading_normal) *shading_normal = out->n;
  if (materialx_hit && !b.tri_uvs.empty()) {
    const size_t base = size_t(th.prim_id) * 6;
    if (base + 5 < b.tri_uvs.size()) {
      MakeMaterialXHitContext(ray_org, ray_dir, rd, wp0, wp1, wp2,
                              &b.tri_uvs[base], th.u, th.v, materialx_hit);
    }
  }
  const bool has_tex = (lt.tex_id >= 0 || lt.normal_tex_id >= 0 ||
                        lt.rough_tex_id >= 0 || lt.metal_tex_id >= 0 ||
                        lt.emission_tex_id >= 0 || lt.occ_tex_id >= 0 ||
                        lt.opacity_tex_id >= 0 || lt.clearcoat_tex_id >= 0 ||
                        lt.clearcoat_rough_tex_id >= 0 ||
                        lt.specular_tex_id >= 0);
  if (has_tex && textures && !b.tri_uvs.empty()) {
    const size_t base = size_t(th.prim_id) * 6;
    if (base + 5 < b.tri_uvs.size()) {
      const float w1 = th.u, w2 = th.v, w0 = 1.0f - w1 - w2;
      const float *uv = &b.tri_uvs[base];
      float u = w0 * uv[0] + w1 * uv[2] + w2 * uv[4];
      float v = w0 * uv[1] + w1 * uv[3] + w2 * uv[5];
      // Ray-differential UV footprint (shared by all texture samples).
      float dudx = 0, dvdx = 0, dudy = 0, dvdy = 0;
      const bool have_fp =
          ComputeUVFootprint(ray_org, ray_dir, rd, wp0, wp1, wp2, out->n, uv[0],
                             uv[1], uv[2], uv[3], uv[4], uv[5], &dudx, &dvdx,
                             &dudy, &dvdy);
      if (lt.tex_id >= 0 && size_t(lt.tex_id) < textures->size()) {
        const Texture &dt = (*textures)[size_t(lt.tex_id)];
        Vec3 t = have_fp ? dt.sample_aniso(u, v, dudx, dvdx, dudy, dvdy, kMaxAniso)
                         : dt.sample(u, v, 0.0f);
        t = Vec3{t.x * dt.scale.x + dt.bias.x, t.y * dt.scale.y + dt.bias.y,
                 t.z * dt.scale.z + dt.bias.z};
        out->base_color = Vec3{lt.base_color.x * t.x, lt.base_color.y * t.y,
                               lt.base_color.z * t.z};
      }
      if (lt.rough_tex_id >= 0) {
        float r = SampleScalarTexAniso(*textures, lt.rough_tex_id, lt.rough_ch, u,
                                       v, have_fp, dudx, dvdx, dudy, dvdy,
                                       kMaxAniso, lt.rough_tex_scale,
                                       lt.rough_tex_bias);
        if (std::isfinite(r)) out->roughness = ClampFloat(r, 0.0f, 1.0f);
      }
      if (lt.metal_tex_id >= 0) {
        float m = SampleScalarTexAniso(*textures, lt.metal_tex_id, lt.metal_ch, u,
                                       v, have_fp, dudx, dvdx, dudy, dvdy,
                                       kMaxAniso, lt.metal_tex_scale,
                                       lt.metal_tex_bias);
        if (std::isfinite(m)) out->metallic = ClampFloat(m, 0.0f, 1.0f);
      }
      if (lt.emission_tex_id >= 0 &&
          size_t(lt.emission_tex_id) < textures->size()) {
        const Texture &et = (*textures)[size_t(lt.emission_tex_id)];
        Vec3 e = have_fp ? et.sample_aniso(u, v, dudx, dvdx, dudy, dvdy, kMaxAniso)
                         : et.sample(u, v, 0.0f);
        e=et.apply_value_transform(e);
        out->emission = Vec3{lt.emission.x * e.x, lt.emission.y * e.y,
                             lt.emission.z * e.z};
      }
      if (lt.specular_tex_id >= 0 &&
          size_t(lt.specular_tex_id) < textures->size()) {
        const Texture &st = (*textures)[size_t(lt.specular_tex_id)];
        Vec3 s = have_fp ? st.sample_aniso(u, v, dudx, dvdx, dudy, dvdy, kMaxAniso)
                         : st.sample(u, v, 0.0f);
        s=st.apply_value_transform(s);
        out->specular_color = Vec3{lt.specular_color.x * s.x,
                                   lt.specular_color.y * s.y,
                                   lt.specular_color.z * s.z};
      }
      if (lt.occ_tex_id >= 0) {
        float o = SampleScalarTexAniso(*textures, lt.occ_tex_id, lt.occ_ch, u, v,
                                       have_fp, dudx, dvdx, dudy, dvdy,
                                       kMaxAniso, lt.occ_tex_scale,
                                       lt.occ_tex_bias);
        if (std::isfinite(o)) out->occlusion = ClampFloat(o, 0.0f, 1.0f);
      }
      if (lt.opacity_tex_id >= 0) {
        float a = SampleScalarTexAniso(*textures, lt.opacity_tex_id,
                                       lt.opacity_ch, u, v, have_fp, dudx, dvdx,
                                       dudy, dvdy, kMaxAniso,
                                       lt.opacity_tex_scale,
                                       lt.opacity_tex_bias);
        if (std::isfinite(a)) out->opacity *= ClampFloat(a, 0.0f, 1.0f);
      }
      if (lt.clearcoat_tex_id >= 0) {
        float clearcoat_val = SampleScalarTexAniso(*textures, lt.clearcoat_tex_id,
                                         lt.clearcoat_ch, u, v, have_fp, dudx, dvdx,
                                         dudy, dvdy, kMaxAniso,
                                         lt.clearcoat_tex_scale,
                                         lt.clearcoat_tex_bias);
        if (std::isfinite(clearcoat_val)) out->clearcoat = ClampFloat(clearcoat_val, 0.0f, 1.0f);
      }
      if (lt.clearcoat_rough_tex_id >= 0) {
        float cr = SampleScalarTexAniso(*textures, lt.clearcoat_rough_tex_id,
                                        lt.clearcoat_rough_ch, u, v, have_fp,
                                        dudx, dvdx, dudy, dvdy, kMaxAniso,
                                        lt.clearcoat_rough_tex_scale,
                                        lt.clearcoat_rough_tex_bias);
        if (std::isfinite(cr)) out->clearcoat_roughness = ClampFloat(cr, 0.0f, 1.0f);
      }
      if (lt.normal_tex_id >= 0 &&
          size_t(lt.normal_tex_id) < textures->size()) {
        const Texture &nt = (*textures)[size_t(lt.normal_tex_id)];
        Vec3 Nt = SampleTangentNormalAniso(nt, u, v, have_fp, dudx, dvdx, dudy,
                                           dvdy, kMaxAniso);
        out->n = PerturbNormalStorm(wp0, wp1, wp2, out->n, uv[0], uv[1], uv[2],
                                    uv[3], uv[4], uv[5], Nt);
      }
    }
  }
  // inputs:opacityThreshold > 0: alpha cutout. The (textured) opacity becomes a
  // binary mask -- below threshold is fully transparent, at/above fully opaque --
  // rather than translucent blending.
  if (out->opacity_threshold > 0.0f)
    out->opacity = (out->opacity < out->opacity_threshold) ? 0.0f : 1.0f;
  return true;
}

Vec3 Shade(lrt_tri_scene *scene, const DirectScene *direct,
           const std::vector<FlatTri> &tris, const std::vector<TriMat> &mats,
           const LightCache &lights, const IblCache *ibl,
           const CameraFrame &camera,
           const Options &opt, const Vec3 &ray_org, const Vec3 &ray_dir,
           const std::vector<Texture> *textures,
           const std::vector<float> *tri_uvs,
           const std::vector<float> *tri_uvs1,
           const lrt_tlas *tlas,
           const std::vector<Blas> *blas,
           const std::vector<InstanceRT> *instances,
           const RayDiff &rd, int depth,
           const ByteVec *tri_colors,
           const std::vector<float> *tri_normals,
           const std::vector<lightusd::tydra::LightRtOpenPBRParams>
               *openpbr_mats,
           bool indirect,
           const std::vector<TriangleSceneChunk> *triangle_chunks,
           float *primary_hit_t, std::atomic<bool>* diagnostic_exhausted) {
  lrt_ray ray;
  ray.org[0] = ray_org.x;
  ray.org[1] = ray_org.y;
  ray.org[2] = ray_org.z;
  ray.tmin = camera.znear;
  ray.dir[0] = ray_dir.x;
  ray.dir[1] = ray_dir.y;
  ray.dir[2] = ray_dir.z;
  ray.tmax = camera.zfar;
  // Primary triangle hit. Two-level (TLAS/instanced) path resolves the hit into
  // a world-space TriInfo; the flat path indexes the shared tris[] directly.
  bool tri_hit = false;
  float tri_t = camera.zfar;
  TriInfo hit_tri;
  MaterialXHitContext materialx_hit;
  Vec3 hit_geometric_normal{0, 0, 0}, hit_shading_normal{0, 0, 0};
  const lightusd::tydra::LightRtOpenPBRParams *hit_openpbr = nullptr;
  if (tlas) {
    lrt_tlas_hit th;
    if (lrt_tlas_intersect1(tlas, &ray, 0xffffffffu, &th) && blas && instances &&
        ResolveTLASHit(th, *blas, *instances, textures, ray_org, ray_dir, rd,
                       &hit_tri, &hit_openpbr, &hit_geometric_normal,
                       &hit_shading_normal, &materialx_hit)) {
      tri_t = th.t;
      tri_hit = true;
    }
  } else {
    lrt_hit hit;
    const bool visible = triangle_chunks && !triangle_chunks->empty()
                             ? IntersectVisibleTriangleChunks(
                                   *triangle_chunks, tris, ray,
                                   opt.purpose_mask, &hit)
                             : IntersectVisibleTriangles(
                                   scene, tris, ray, opt.purpose_mask, &hit);
    if (visible &&
        hit.prim_id != LRT_TRI_NO_HIT && size_t(hit.prim_id) < tris.size()) {
      // Rebuild the full TriInfo from the slim FlatTri (geometry + purpose) and
      // its material table entry, exactly the record the flat path stored inline
      // before the material was hoisted into flat_mats.
      const FlatTri &ft = tris[size_t(hit.prim_id)];
      uint32_t mid = ft.mat_id;
      if (size_t(mid) < mats.size() && Dot(ft.n, ray_dir) > 0.0f) {
        const uint32_t back = mats[mid].backface_id;
        if (size_t(back) < mats.size()) mid = back;
      }
      if (size_t(mid) < mats.size()) {
        const TriMat &mat = mats[mid];
        hit_tri = CombineTriMat(mat);
        if (openpbr_mats && mat.openpbr_id < openpbr_mats->size()) {
          hit_openpbr = &(*openpbr_mats)[mat.openpbr_id];
        }
      } else {
        hit_tri = TriInfo{};
      }
      hit_tri.p0 = ft.p0;
      hit_tri.p1 = ft.p1;
      hit_tri.p2 = ft.p2;
      hit_tri.n = ft.n;
      hit_geometric_normal = ft.n;
      hit_tri.purpose_bit = ft.purpose_bit;
      // Per-corner displayColor/displayOpacity (RGBA), barycentric-interpolated.
      if (tri_colors && size_t(hit.prim_id) * 12 + 11 < tri_colors->size()) {
        const uint8_t *cc = &(*tri_colors)[size_t(hit.prim_id) * 12];
        const float w1 = hit.u, w2 = hit.v, w0 = 1.0f - w1 - w2;
        const float s = 1.0f / 255.0f;
        hit_tri.base_color = Vec3{(w0 * cc[0] + w1 * cc[4] + w2 * cc[8]) * s,
                                  (w0 * cc[1] + w1 * cc[5] + w2 * cc[9]) * s,
                                  (w0 * cc[2] + w1 * cc[6] + w2 * cc[10]) * s};
        hit_tri.opacity = (w0 * cc[3] + w1 * cc[7] + w2 * cc[11]) * s;
      }
      // Smooth shading: interpolate per-corner authored normals (world-space in
      // the flat path), falling back to the stored geometric normal.
      if (tri_normals && size_t(hit.prim_id) * 9 + 8 < tri_normals->size()) {
        const float *nn = &(*tri_normals)[size_t(hit.prim_id) * 9];
        const float w1 = hit.u, w2 = hit.v, w0 = 1.0f - w1 - w2;
        Vec3 sn{w0 * nn[0] + w1 * nn[3] + w2 * nn[6],
                w0 * nn[1] + w1 * nn[4] + w2 * nn[7],
                w0 * nn[2] + w1 * nn[5] + w2 * nn[8]};
        if (Length(sn) > 1.0e-12f) hit_tri.n = Normalize(sn);
      }
      hit_shading_normal = hit_tri.n;
      if (tri_uvs) {
        const size_t uv_base = size_t(hit.prim_id) * 6;
        if (uv_base + 5 < tri_uvs->size()) {
          MakeMaterialXHitContext(ray_org, ray_dir, rd, hit_tri.p0, hit_tri.p1,
                                  hit_tri.p2, &(*tri_uvs)[uv_base], hit.u,
                                  hit.v, &materialx_hit);
        }
      }
      // Diffuse/normal textures: interpolate per-vertex UV with the barycentric
      // hit weights (Moller-Trumbore: w0=1-u-v for p0, u for p1, v for p2).
      if ((hit_tri.tex_id >= 0 || hit_tri.normal_tex_id >= 0 ||
           hit_tri.rough_tex_id >= 0 || hit_tri.metal_tex_id >= 0 ||
           hit_tri.emission_tex_id >= 0 || hit_tri.occ_tex_id >= 0 ||
           hit_tri.opacity_tex_id >= 0 || hit_tri.clearcoat_tex_id >= 0 ||
           hit_tri.clearcoat_rough_tex_id >= 0) &&
          textures && tri_uvs) {
        const size_t base = size_t(hit.prim_id) * 6;
        if (base + 5 < tri_uvs->size()) {
          const float w1 = hit.u, w2 = hit.v, w0 = 1.0f - w1 - w2;
          // One interpolated UV sample per stream, each with its own ray
          // differential footprint. A lane selects stream 0 or 1 through
          // TriMat::uv_lane_slot, so two textures can read two UV sets.
          struct UvSample {
            float u{0.0f}, v{0.0f};
            float dudx{0.0f}, dvdx{0.0f}, dudy{0.0f}, dvdy{0.0f};
            bool have_fp{false};
            const float *corners{nullptr};
          };
          auto sample_stream = [&](const std::vector<float> *stream) {
            UvSample out;
            if (!stream || base + 5 >= stream->size()) return out;
            const float *uv = &(*stream)[base];
            out.corners = uv;
            out.u = w0 * uv[0] + w1 * uv[2] + w2 * uv[4];
            out.v = w0 * uv[1] + w1 * uv[3] + w2 * uv[5];
            out.have_fp = ComputeUVFootprint(
                ray_org, ray_dir, rd, hit_tri.p0, hit_tri.p1, hit_tri.p2,
                hit_tri.n, uv[0], uv[1], uv[2], uv[3], uv[4], uv[5], &out.dudx,
                &out.dvdx, &out.dudy, &out.dvdy);
            return out;
          };
          const UvSample uv0 = sample_stream(tri_uvs);
          const UvSample uv1 = sample_stream(tri_uvs1);
          auto lane = [&](uint32_t id) -> const UvSample & {
            return (uv1.corners && UvLaneSlot(hit_tri.uv_lane_slot, id) == 1)
                       ? uv1 : uv0;
          };
          {
            const UvSample &s = lane(kUvLaneDiffuse);
            if (hit_tri.tex_id >= 0 && size_t(hit_tri.tex_id) < textures->size()) {
              const Texture &dt = (*textures)[size_t(hit_tri.tex_id)];
              Vec3 t = s.have_fp ? dt.sample_aniso(s.u, s.v, s.dudx, s.dvdx,
                                                   s.dudy, s.dvdy, kMaxAniso)
                                 : dt.sample(s.u, s.v, 0.0f);
              t = Vec3{t.x * dt.scale.x + dt.bias.x,
                       t.y * dt.scale.y + dt.bias.y,
                       t.z * dt.scale.z + dt.bias.z};
              hit_tri.base_color = Vec3{hit_tri.base_color.x * t.x,
                                        hit_tri.base_color.y * t.y,
                                        hit_tri.base_color.z * t.z};
            }
          }
          {
            const UvSample &s = lane(kUvLaneRough);
            if (hit_tri.rough_tex_id >= 0) {
              float r = SampleScalarTexAniso(
                  *textures, hit_tri.rough_tex_id, hit_tri.rough_ch, s.u, s.v,
                  s.have_fp, s.dudx, s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.rough_tex_scale, hit_tri.rough_tex_bias);
              if (std::isfinite(r)) hit_tri.roughness = ClampFloat(r, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneMetal);
            if (hit_tri.metal_tex_id >= 0) {
              float m = SampleScalarTexAniso(
                  *textures, hit_tri.metal_tex_id, hit_tri.metal_ch, s.u, s.v,
                  s.have_fp, s.dudx, s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.metal_tex_scale, hit_tri.metal_tex_bias);
              if (std::isfinite(m)) hit_tri.metallic = ClampFloat(m, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneEmissive);
            if (hit_tri.emission_tex_id >= 0 &&
                size_t(hit_tri.emission_tex_id) < textures->size()) {
              const Texture &et = (*textures)[size_t(hit_tri.emission_tex_id)];
              Vec3 e = s.have_fp ? et.sample_aniso(s.u, s.v, s.dudx, s.dvdx,
                                                   s.dudy, s.dvdy, kMaxAniso)
                                 : et.sample(s.u, s.v, 0.0f);
              e = et.apply_value_transform(e);
              hit_tri.emission = Vec3{hit_tri.emission.x * e.x,
                                      hit_tri.emission.y * e.y,
                                      hit_tri.emission.z * e.z};
            }
          }
          {
            const UvSample &s = lane(kUvLaneSpecular);
            if (hit_tri.specular_tex_id >= 0 &&
                size_t(hit_tri.specular_tex_id) < textures->size()) {
              const Texture &st = (*textures)[size_t(hit_tri.specular_tex_id)];
              Vec3 v = s.have_fp ? st.sample_aniso(s.u, s.v, s.dudx, s.dvdx,
                                                   s.dudy, s.dvdy, kMaxAniso)
                                 : st.sample(s.u, s.v, 0.0f);
              v = st.apply_value_transform(v);
              hit_tri.specular_color = Vec3{hit_tri.specular_color.x * v.x,
                                            hit_tri.specular_color.y * v.y,
                                            hit_tri.specular_color.z * v.z};
            }
          }
          {
            const UvSample &s = lane(kUvLaneOcc);
            if (hit_tri.occ_tex_id >= 0) {
              float o = SampleScalarTexAniso(
                  *textures, hit_tri.occ_tex_id, hit_tri.occ_ch, s.u, s.v,
                  s.have_fp, s.dudx, s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.occ_tex_scale, hit_tri.occ_tex_bias);
              if (std::isfinite(o)) hit_tri.occlusion = ClampFloat(o, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneOpacity);
            if (hit_tri.opacity_tex_id >= 0) {
              float a = SampleScalarTexAniso(
                  *textures, hit_tri.opacity_tex_id, hit_tri.opacity_ch, s.u,
                  s.v, s.have_fp, s.dudx, s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.opacity_tex_scale, hit_tri.opacity_tex_bias);
              if (std::isfinite(a)) hit_tri.opacity *= ClampFloat(a, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneClearcoat);
            if (hit_tri.clearcoat_tex_id >= 0) {
              float cc = SampleScalarTexAniso(
                  *textures, hit_tri.clearcoat_tex_id, hit_tri.clearcoat_ch, s.u,
                  s.v, s.have_fp, s.dudx, s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.clearcoat_tex_scale, hit_tri.clearcoat_tex_bias);
              if (std::isfinite(cc))
                hit_tri.clearcoat = ClampFloat(cc, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneClearcoatRough);
            if (hit_tri.clearcoat_rough_tex_id >= 0) {
              float cr = SampleScalarTexAniso(
                  *textures, hit_tri.clearcoat_rough_tex_id,
                  hit_tri.clearcoat_rough_ch, s.u, s.v, s.have_fp, s.dudx,
                  s.dvdx, s.dudy, s.dvdy, kMaxAniso,
                  hit_tri.clearcoat_rough_tex_scale,
                  hit_tri.clearcoat_rough_tex_bias);
              if (std::isfinite(cr))
                hit_tri.clearcoat_roughness = ClampFloat(cr, 0.0f, 1.0f);
            }
          }
          {
            const UvSample &s = lane(kUvLaneNormal);
            if (hit_tri.normal_tex_id >= 0 &&
                size_t(hit_tri.normal_tex_id) < textures->size()) {
              const Texture &nt = (*textures)[size_t(hit_tri.normal_tex_id)];
              Vec3 Nt = SampleTangentNormalAniso(nt, s.u, s.v, s.have_fp, s.dudx,
                                                 s.dvdx, s.dudy, s.dvdy,
                                                 kMaxAniso);
              hit_tri.n = PerturbNormalStorm(
                  hit_tri.p0, hit_tri.p1, hit_tri.p2, hit_tri.n, s.corners[0],
                  s.corners[1], s.corners[2], s.corners[3], s.corners[4],
                  s.corners[5], Nt);
            }
          }
        }
      }
      // inputs:opacityThreshold > 0: alpha cutout (binary mask, not blending).
      if (hit_tri.opacity_threshold > 0.0f)
        hit_tri.opacity =
            (hit_tri.opacity < hit_tri.opacity_threshold) ? 0.0f : 1.0f;
      tri_t = hit.t;
      tri_hit = true;
    }
  }
  float best_t = tri_hit ? tri_t : camera.zfar;
  DirectHit direct_hit;
  IntersectDirectScene(direct, ray_org, ray_dir, camera.znear, best_t, &direct_hit);
  const bool gathered_directly = indirect && !opt.light_double_count;
  if (!tri_hit && !direct_hit.hit) {
    if (primary_hit_t) *primary_hit_t = camera.zfar;
    if (opt.aov != CaptureAov::Color) return Vec3{0, 0, 0};
    // An escaping BSDF bounce must not bring the ENVIRONMENT back with it: the
    // surface that spawned it already integrated the dome over this very lobe
    // (the split-sum IBL term, or the flat dome term), so returning it here
    // counts the dome twice. The background is a different thing -- a flat
    // backdrop that no IBL term accounts for -- so an indirect ray still returns
    // it, and a reflective surface still picks up the backdrop's tint.
    if (gathered_directly) return opt.bg;
    if (ibl && ibl->valid) {
      return Add(opt.bg, SampleEnv(ibl->env, EnvDir(*ibl, ray_dir)));
    }
    return lights.has_dome ? Add(opt.bg, lights.env_color) : opt.bg;
  }
  TriInfo tri;
  const lightusd::tydra::LightRtOpenPBRParams *tri_openpbr = nullptr;
  float hit_t = best_t;
  if (direct_hit.hit) {
    hit_t = direct_hit.t;
    tri.n = direct_hit.n;
    hit_geometric_normal = hit_shading_normal = direct_hit.n;
    tri.base_color = direct_hit.base_color;
    tri.emission = direct_hit.emission;
  } else {
    tri = hit_tri;
    tri_openpbr = hit_openpbr;
  }
  if (primary_hit_t) *primary_hit_t = hit_t;
  if (!IsMaterialXAov(opt.aov)) {
    if (opt.aov == CaptureAov::Depth) return Vec3{hit_t, hit_t, hit_t};
    if (opt.aov == CaptureAov::WorldNormal) return Normalize(tri.n);
    if (opt.aov == CaptureAov::GeometricNormal)
      return Normalize(hit_geometric_normal);
    if (opt.aov == CaptureAov::ShadingNormal)
      return Normalize(hit_shading_normal);
    if (opt.aov == CaptureAov::PrimId) {
      const float id = float(tri.capture_prim_id);
      return Vec3{id, id, id};
    }
  }
  const Vec3 p = Add(ray_org, Mul(ray_dir, hit_t));
  lightusd::tydra::LightRtOpenPBRParams dynamic_openpbr{};
  if (opt.mtlx_runtime && tri_hit) {
    lusdview::MaterialXSurfaceContext context;
    if (materialx_hit.has_uv) {
      context.uv[0] = materialx_hit.uv[0];
      context.uv[1] = materialx_hit.uv[1];
      context.uv_dx[0] = materialx_hit.uv_dx[0];
      context.uv_dx[1] = materialx_hit.uv_dx[1];
      context.uv_dy[0] = materialx_hit.uv_dy[0];
      context.uv_dy[1] = materialx_hit.uv_dy[1];
      context.has_uv_derivatives = materialx_hit.has_uv_derivatives;
      context.dpdu[0] = materialx_hit.dpdu.x;
      context.dpdu[1] = materialx_hit.dpdu.y;
      context.dpdu[2] = materialx_hit.dpdu.z;
      context.dpdv[0] = materialx_hit.dpdv.x;
      context.dpdv[1] = materialx_hit.dpdv.y;
      context.dpdv[2] = materialx_hit.dpdv.z;
    }
    context.position[0] = p.x;
    context.position[1] = p.y;
    context.position[2] = p.z;
    const Vec3 runtime_normal = Normalize(tri.n);
    context.shading_normal[0] = runtime_normal.x;
    context.shading_normal[1] = runtime_normal.y;
    context.shading_normal[2] = runtime_normal.z;
    context.geometric_normal[0] = hit_geometric_normal.x;
    context.geometric_normal[1] = hit_geometric_normal.y;
    context.geometric_normal[2] = hit_geometric_normal.z;
    const Vec3 runtime_view = Normalize(Mul(ray_dir, -1.0f));
    context.view_direction[0] = runtime_view.x;
    context.view_direction[1] = runtime_view.y;
    context.view_direction[2] = runtime_view.z;
    std::string eval_error;
    if (opt.aov == CaptureAov::MaterialXNode) {
      lusdview::MaterialXNodeValue value;
      if (!lusdview::EvaluateMaterialXNodeRuntimeAt(
              opt.mtlx_runtime.get(), context, opt.mtlx_aov_node.c_str(),
              &value, &eval_error)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
          std::cerr << "MaterialX node AOV evaluation failed: " << eval_error
                    << "\n";
        return Vec3{0.0f, 0.0f, 0.0f};
      }
      return Vec3{value.rgb[0], value.rgb[1], value.rgb[2]};
    }
    if (lusdview::EvaluateMaterialXSurfaceRuntimeAt(
            opt.mtlx_runtime.get(), context, &dynamic_openpbr, &eval_error)) {
      tri_openpbr = &dynamic_openpbr;
      tri.base_color = Vec3{dynamic_openpbr.baseColor[0],
                            dynamic_openpbr.baseColor[1],
                            dynamic_openpbr.baseColor[2]};
      tri.roughness = dynamic_openpbr.diffuseRoughness;
      tri.metallic = dynamic_openpbr.metalness;
      tri.clearcoat = dynamic_openpbr.coatWeight;
      tri.clearcoat_roughness = dynamic_openpbr.coatRoughness;
      tri.ior = dynamic_openpbr.specularIor;
      tri.emission = Vec3{dynamic_openpbr.emissionColor[0] * dynamic_openpbr.emission,
                          dynamic_openpbr.emissionColor[1] * dynamic_openpbr.emission,
                          dynamic_openpbr.emissionColor[2] * dynamic_openpbr.emission};
      tri.opacity = dynamic_openpbr.opacity;
      const Vec3 evaluated_normal{dynamic_openpbr.normal[0],
                                  dynamic_openpbr.normal[1],
                                  dynamic_openpbr.normal[2]};
      if (Length(evaluated_normal) > 1.0e-12f) {
        tri.n = Normalize(evaluated_normal);
        hit_shading_normal = tri.n;
      }
    } else {
      static std::atomic<bool> reported{false};
      if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::cerr << "MaterialX hit evaluation failed: " << eval_error << "\n";
      }
    }
  }
  if (opt.aov == CaptureAov::MaterialXDiffuseWeight ||
      opt.aov == CaptureAov::MaterialXSpecularWeight ||
      opt.aov == CaptureAov::MaterialXCoatWeight ||
      opt.aov == CaptureAov::MaterialXTransmissionWeight) {
    if (!tri_openpbr) return Vec3{0.0f, 0.0f, 0.0f};
    const OpenPBRParams params = OpenPBRParamsForMaterial(tri, tri.n, tri_openpbr);
    const BsdfLobeWeights weights = bsdf_lobe_weights(&params);
    float weight = 0.0f;
    switch (opt.aov) {
      case CaptureAov::MaterialXDiffuseWeight: weight = weights.diffuse; break;
      case CaptureAov::MaterialXSpecularWeight: weight = weights.specular; break;
      case CaptureAov::MaterialXCoatWeight: weight = weights.coat; break;
      case CaptureAov::MaterialXTransmissionWeight:
        weight = weights.transmission;
        break;
      default: break;
    }
    return Vec3{weight, weight, weight};
  }
  // primvars:displayOpacity < 1: see-through. Blend the surface shade with what
  // lies behind it (continuation ray; bounded recursion). Opaque hits (default
  // opacity 1.0) return `col` unchanged, so opaque renders are byte-identical.
  auto apply_opacity = [&](const Vec3 &col) -> Vec3 {
    if (tri.opacity >= 0.999f) return col;
    if (depth >= (opt.diagnostic_emission ? 15 : 4)) {
      if (opt.diagnostic_emission && diagnostic_exhausted)
        diagnostic_exhausted->store(true, std::memory_order_relaxed);
      return col;
    }
    const float a = std::max(0.0f, tri.opacity);
    const float mag = std::max(std::max(std::fabs(p.x), std::fabs(p.y)),
                               std::fabs(p.z));
    const float eps = std::max(1.0e-4f, mag * 3.0e-6f);
    const Vec3 behind_org = Add(p, Mul(ray_dir, eps));
    CameraFrame behind_camera = camera;
    behind_camera.znear = eps;
    behind_camera.zfar = std::max(0.0f, camera.zfar - hit_t - eps);
    const Vec3 behind =
        Shade(scene, direct, tris, mats, lights, ibl, behind_camera, opt, behind_org,
              ray_dir, textures, tri_uvs, tri_uvs1, tlas, blas, instances, rd,
              depth + 1,
              tri_colors, tri_normals, openpbr_mats, indirect, triangle_chunks,
              nullptr, diagnostic_exhausted);
    return Add(Mul(col, a), Mul(behind, 1.0f - a));
  };
  if (opt.diagnostic_emission) {
    Vec3 diagnostic = tri.emission;
    if (opt.diagnostic_material != MaterialDiagnostic::None) {
      const float value = opt.diagnostic_material == MaterialDiagnostic::Roughness
          ? tri.roughness : tri.metallic;
      diagnostic = Vec3{value, value, value};
    }
    // Transparent PreviewSurface retains emission at full weight. Presence
    // attenuates it, while cutout masks both modes identically.
    const Vec3 blended = apply_opacity(diagnostic);
    if (!tri.opacity_presence && tri.opacity_threshold <= 0.0f)
      return Add(blended, Mul(diagnostic, 1.0f - std::clamp(tri.opacity, 0.0f, 1.0f)));
    return blended;
  }
  // Converted particle previews bake surface color rather than illumination.
  // Keep their camera light independent of the original scene's light powers.
  if(opt.scene_preview_albedo && direct_hit.hit) {
    return Mul(tri.base_color,.25f+.75f*std::abs(Dot(Normalize(tri.n),Normalize(ray_dir))));
  }
  // Occlusion against whichever acceleration structure is active.
  auto occluded = [&](const Vec3 &op, const Vec3 &on, const Vec3 &ol,
                      float omax) -> bool {
    return tlas ? OccludedTLAS(tlas, op, on, ol, omax)
                : Occluded(scene, tris, op, on, ol, omax, direct,
                           opt.purpose_mask, triangle_chunks);
  };
  Vec3 n = tri.n;
  if (Dot(n, ray_dir) > 0.0f) {
    n = Mul(n, -1.0f);
  }
  Vec3 view = Normalize(Mul(ray_dir, -1.0f));
  // UsdPreviewSurface opacityMode. "transparent" (the default) reduces only the
  // diffuse/subsurface contribution and alpha-blends that part with what lies
  // behind, while the specular stays at full weight; "presence"/cutout keep the
  // whole-shade coverage blend. Track the diffuse sum separately for that split.
  const bool transparent_opacity =
      !tri.opacity_presence && tri.opacity_threshold <= 0.0f;
  Vec3 c_diffuse{0.0f, 0.0f, 0.0f};
  // Occlusion (AO) modulates the indirect/ambient response, not self-emission.
  // An indirect bounce that lands on an analytic mesh light must not pick up its
  // emission: the shading point it came from already received that light through
  // the direct-lighting term (eval_light over LightCache::mesh).
  const Vec3 emitted = (gathered_directly && tri.area_light)
                           ? Vec3{0.0f, 0.0f, 0.0f}
                           : tri.emission;
  const Vec3 ambient_diffuse =
      Mul(tri.base_color, opt.ambient * tri.occlusion);
  Vec3 c = Add(ambient_diffuse, emitted);
  c_diffuse = Add(c_diffuse, ambient_diffuse);
  if (ibl && ibl->valid) {
    Vec3 diffuse = SampleEnv(ibl->diffuse, EnvDir(*ibl, n));
    float ndotv = std::max(0.0f, Dot(n, view));
    // F0 (normal-incidence reflectance): specular workflow uses specularColor
    // directly; metallic workflow lerps the dielectric F0 (from IOR; ior 1.5 ->
    // 0.04) toward the base color by metalness. The specular workflow ignores
    // metallic, so its diffuse weight isn't dimmed by it.
    Vec3 f0;
    float kd_metal;
    float specular_roughness = tri.roughness;
    if (tri.use_specular_workflow) {
      f0 = tri.specular_color;
      kd_metal = 0.0f;
    } else if (opt.material_shading == Options::MaterialShading::LightRtBsdf &&
               tri_openpbr) {
      // The LightRT BSDF path carries the complete MaterialX/OpenPBR parameters.
      // Keep the split-sum environment specular consistent with that material:
      // the legacy TriInfo values do not include specular_weight, and its
      // roughness is diffuse roughness for MaterialX surfaces.
      const OpenPBRParams p = OpenPBRParamsForMaterial(tri, n, tri_openpbr);
      float dielectric_f0 = (p.specular_ior - 1.0f) /
                            (p.specular_ior + 1.0f);
      dielectric_f0 = dielectric_f0 * dielectric_f0 * p.specular_weight;
      const Vec3 dielectric_f0_color{
          dielectric_f0 * p.specular_color.x,
          dielectric_f0 * p.specular_color.y,
          dielectric_f0 * p.specular_color.z};
      const Vec3 base = Mul(FromMtlxV3(p.base_color), p.base_weight);
      f0 = Lerp(dielectric_f0_color, base, p.metalness);
      kd_metal = 0.0f;
      specular_roughness = p.specular_roughness;
    } else {
      // ior 1.5 (the default) -> exactly 0.04f, matching the prior fixed constant
      // bit-for-bit; only an authored non-default IOR takes the derived path.
      float df0 = 0.04f;
      if (tri.ior != 1.5f) {
        df0 = (tri.ior - 1.0f) / (tri.ior + 1.0f);
        df0 *= df0;
      }
      f0 = Lerp(Vec3{df0, df0, df0}, tri.base_color, tri.metallic);
      kd_metal = tri.metallic;
    }
    Vec3 refl = Reflect(Mul(view, -1.0f), n);
    Vec3 env_refl = EnvDir(*ibl, refl);  // reflection in the dome's frame
    Vec3 spec_env = SampleIblMip(ibl->prefiltered, env_refl,
                                 specular_roughness);
    float brdf_a = 1.0f;
    float brdf_b = 0.0f;
    SampleBrdfLut(*ibl, ndotv, specular_roughness, &brdf_a, &brdf_b);
    // Both modes use the bounded split-sum for environment specular (see
    // EvalIblSpecularSplitSum). f0 already reflects the OpenPBR reflectance in
    // bsdf mode, so this stays material-correct.
    Vec3 spec = EvalIblSpecularSplitSum(spec_env, f0, brdf_a, brdf_b);
    Vec3 kd = Mul(Vec3{1.0f - f0.x, 1.0f - f0.y, 1.0f - f0.z},
                  1.0f - kd_metal);
    Vec3 diff =
        opt.material_shading == Options::MaterialShading::LightRtBsdf
            ? EvalMaterialIblDiffuse(tri, n, view, diffuse, opt, tri_openpbr)
            : Mul(Mul(Mul(tri.base_color, diffuse), kd), tri.occlusion);
    c_diffuse = Add(c_diffuse, diff);
    c = Add(c, Add(diff, spec));
    // Clearcoat: a thin dielectric coat (F0=0.04) reflecting the environment with
    // its own (usually low) roughness, layered on top and weighted by the coat
    // amount. Skipped entirely when clearcoat==0 (byte-identical default).
    if (opt.material_shading != Options::MaterialShading::LightRtBsdf &&
        tri.clearcoat > 0.0f) {
      Vec3 cc_env =
          SampleIblMip(ibl->prefiltered, env_refl, tri.clearcoat_roughness);
      float cc_a = 1.0f, cc_b = 0.0f;
      SampleBrdfLut(*ibl, ndotv, tri.clearcoat_roughness, &cc_a, &cc_b);
      float cc_spec = 0.04f * cc_a + cc_b;
      c = Add(c, Mul(cc_env, cc_spec * tri.clearcoat));
    }
  } else if (lights.has_dome) {
    const Vec3 dome_diffuse = Mul(Mul(tri.base_color, lights.env_color), 0.25f);
    c_diffuse = Add(c_diffuse, dome_diffuse);
    c = Add(c, dome_diffuse);
  }
  // Is this light area-sampled (NEE + MIS) rather than treated as a point? Only
  // in lightrt-bsdf mode -- the legacy shading path has no BSDF pdf to weigh
  // against, so MIS is not even defined there, and its look must not change.
  const bool bsdf_mode =
      (opt.material_shading == Options::MaterialShading::LightRtBsdf);
  auto is_area_sampled = [&](const PreviewLight &light) -> bool {
    if (!opt.light_nee || !bsdf_mode) return false;
    switch (light.kind) {
      case PreviewLight::Kind::Sphere:
        return light.radius > 1.0e-5f &&
               Length(Sub(light.position, p)) > light.radius + 1.0e-5f;
      case PreviewLight::Kind::Rect:
      case PreviewLight::Kind::Disk:
      case PreviewLight::Kind::Cylinder:
        return light.area > 1.0e-8f;
      default:
        return false;  // Point / Distant are genuinely delta lights
    }
  };
  // Light-side quantities for the BSDF-sampled MIS term: the pdf of having chosen
  // `wi` by light sampling, the distance to the surface it hits (for the shadow
  // ray), and the radiance there.
  auto light_pdf_along = [&](const PreviewLight &light, const Vec3 &wi,
                             float *hit_dist, Vec3 *radiance) -> float {
    if (light.kind == PreviewLight::Kind::Sphere) {
      const float pdf = SphereLightPdf(light, p, wi);
      if (pdf <= 0.0f) return 0.0f;
      const Vec3 d = Sub(light.position, p);
      const float b = Dot(wi, d);
      const float disc =
          std::max(0.0f, b * b - (Dot(d, d) - light.radius * light.radius));
      *hit_dist = std::max(0.0f, b - std::sqrt(disc));
      *radiance = SphereLightRadiance(light);
      return pdf;
    }
    float t = 0.0f;
    Vec3 nl;
    if (!IntersectAreaLight(light, p, wi, &t, &nl)) return 0.0f;
    const float cos_emit = Dot(nl, Mul(wi, -1.0f));
    if (cos_emit <= 1.0e-6f) return 0.0f;
    const float pdf = (t * t) / (cos_emit * light.area);
    if (!(pdf > 0.0f) || !std::isfinite(pdf)) return 0.0f;
    *hit_dist = t;
    *radiance = light.radiance;
    return pdf;
  };

  auto sample_bsdf_bounce = [&]() -> Vec3 {
    if (opt.material_shading != Options::MaterialShading::LightRtBsdf ||
        depth >= 2 || tri.opacity < 0.999f) {
      return Vec3{0.0f, 0.0f, 0.0f};
    }
    OpenPBRParams params = OpenPBRParamsForMaterial(tri, n, tri_openpbr);
    pcg32 rng = MakeBsdfRng(ray_org, ray_dir, depth);
    BsdfSample sample{};
    if (!bsdf_sample(&params, ToMtlxV3(n), ToMtlxV3(view), &rng, &sample) ||
        sample.pdf <= 0.0f || !v3_is_finite(sample.throughput)) {
      return Vec3{0.0f, 0.0f, 0.0f};
    }
    Vec3 wi = Normalize(FromMtlxV3(sample.wi));
    Vec3 throughput = FromMtlxV3(sample.throughput);
    if (Length(wi) <= 0.0f || !FiniteColor(throughput)) {
      return Vec3{0.0f, 0.0f, 0.0f};
    }
    const float mag = std::max(std::max(std::fabs(p.x), std::fabs(p.y)),
                               std::fabs(p.z));
    const float eps = std::max(1.0e-4f, mag * 3.0e-6f);
    const Vec3 bounce_org = Add(p, Mul(wi, eps));
    CameraFrame bounce_camera = camera;
    bounce_camera.znear = eps;
    RayDiff bounce_rd;
    bounce_rd.valid = false;
    float bounce_hit_t = bounce_camera.zfar;
    Vec3 bounce = Shade(scene, direct, tris, mats, lights, ibl, bounce_camera,
                        opt, bounce_org, wi, textures, tri_uvs, tri_uvs1, tlas,
                        blas, instances, bounce_rd, depth + 1, tri_colors,
                        tri_normals, openpbr_mats, /*indirect=*/true,
                        triangle_chunks, &bounce_hit_t, diagnostic_exhausted);
    // OpenPBR transmission_depth defines a homogeneous interior medium. Apply
    // Beer-Lambert extinction over the actual continuation-ray distance instead
    // of tinting only at the interface; this makes thick glass absorb more than
    // thin glass and keeps transmission_color physically meaningful.
    if (sample.transmission && sample.crossed &&
        bounce_hit_t < bounce_camera.zfar) {
      const VolumeMedium medium = transmission_medium(&params);
      bounce.x *= std::exp(-medium.sigma_t.x * bounce_hit_t);
      bounce.y *= std::exp(-medium.sigma_t.y * bounce_hit_t);
      bounce.z *= std::exp(-medium.sigma_t.z * bounce_hit_t);
    }
    Vec3 out = FiniteColor(bounce) ? Mul(bounce, throughput)
                                   : Vec3{0.0f, 0.0f, 0.0f};

    // The other half of MIS. Analytic lights are not in the BVH, so the bounce
    // ray cannot hit them: pick them up explicitly along wi and weight by the
    // chance NEE would have found this direction instead. A delta lobe (`specular`)
    // is unreachable by light sampling, so it takes the full contribution -- this
    // is what BsdfSample::specular is for.
    for (const PreviewLight &light : lights.finite) {
      if (!is_area_sampled(light)) continue;
      float hit_dist = 0.0f;
      Vec3 L{0.0f, 0.0f, 0.0f};
      const float pdf_l = light_pdf_along(light, wi, &hit_dist, &L);
      if (pdf_l <= 0.0f) continue;
      if (opt.shadows && light.shadow_enable &&
          occluded(p, n, wi, std::max(0.0f, hit_dist - 1.0e-3f))) {
        continue;
      }
      const float w = sample.specular ? 1.0f : PowerHeuristic(sample.pdf, pdf_l);
      out = Add(out, Vec3{throughput.x * L.x * w, throughput.y * L.y * w,
                          throughput.z * L.z * w});
    }
    return out;
  };
  if (lights.finite.empty() && lights.mesh.empty() && !lights.has_dome &&
      (!ibl || !ibl->valid)) {
    Vec3 l = Normalize(Sub(camera.origin, p));
    if (Dot(n, l) > 0.0f &&
        (!opt.shadows ||
         !occluded(p, n, l,
                   std::max(0.0f, Length(Sub(camera.origin, p)) - 1.0e-3f)))) {
      Vec3 direct_diffuse;
      c = Add(c, EvalMaterialDirect(tri, n, view, l, Vec3{1.0f, 1.0f, 1.0f},
                                    opt, tri_openpbr, &direct_diffuse));
      c_diffuse = Add(c_diffuse, direct_diffuse);
    }
    c = Add(c, sample_bsdf_bounce());
    if (transparent_opacity)
      return Add(Sub(c, c_diffuse), apply_opacity(c_diffuse));
    return apply_opacity(c);
  }
  // NEE: sample a point on the light, weigh the estimate against the chance the
  // BSDF sampler would have found the same direction on its own. The power
  // heuristic is only valid when the BSDF-sampling partner actually runs;
  // sample_bsdf_bounce early-returns at depth >= 2, for non-opaque hits and in
  // the non-bsdf shading modes, and then nothing supplies the pdf_b^2 share --
  // the light was systematically under-counted (up to ~2x too dark where
  // pdf_l ~ pdf_b). Light sampling is the sole estimator there, weight 1.
  const bool bsdf_partner_runs =
      opt.material_shading == Options::MaterialShading::LightRtBsdf &&
      depth < 2 && tri.opacity >= 0.999f;
  auto nee_light = [&](const PreviewLight &light, pcg32 *rng) {
    LightSample ls;
    const float u1 = pcg32_f(rng);
    const float u2 = pcg32_f(rng);
    const bool ok = (light.kind == PreviewLight::Kind::Sphere)
                        ? SampleSphereLight(light, p, u1, u2, &ls)
                        : SampleAreaLight(light, p, u1, u2, &ls);
    if (!ok) return;
    const float ndotl = Dot(n, ls.wi);
    if (ndotl <= 0.0f || ls.pdf <= 0.0f) return;
    if (opt.shadows && light.shadow_enable &&
        occluded(p, n, ls.wi, std::max(0.0f, ls.dist - 1.0e-3f))) {
      return;
    }
    OpenPBRParams params = OpenPBRParamsForMaterial(tri, n, tri_openpbr);
    float pdf_b = 0.0f;
    v3 fd, fs;
    bsdf_eval_lobes(&params, ToMtlxV3(n), ToMtlxV3(view), ToMtlxV3(ls.wi),
                    &fd, &fs, &pdf_b);
    const Vec3 diffuse = FromMtlxV3(fd);
    const Vec3 brdf = Add(diffuse, FromMtlxV3(fs));
    if (!FiniteColor(brdf)) return;
    const float w = bsdf_partner_runs ? PowerHeuristic(ls.pdf, pdf_b) : 1.0f;
    const float k = w * ndotl / ls.pdf;
    c_diffuse = Add(c_diffuse, Vec3{diffuse.x * ls.radiance.x * k,
                                    diffuse.y * ls.radiance.y * k,
                                    diffuse.z * ls.radiance.z * k});
    c = Add(c, Vec3{brdf.x * ls.radiance.x * k, brdf.y * ls.radiance.y * k,
                    brdf.z * ls.radiance.z * k});
  };

  auto eval_light = [&](const PreviewLight &light) {
    if (is_area_sampled(light)) return;  // handled by NEE below
    Vec3 l;
    float max_t = 1.0e30f;
    Vec3 radiance = light.radiance;
    if (light.kind == PreviewLight::Kind::Distant) {
      l = Normalize(Mul(light.direction, -1.0f));
    } else {
      Vec3 d = Sub(light.position, p);
      float dist = Length(d);
      if (dist <= 1.0e-6f) return;
      l = Mul(d, 1.0f / dist);
      max_t = std::max(0.0f, dist - 1.0e-3f);
      if (light.kind == PreviewLight::Kind::Mesh ||
          light.kind == PreviewLight::Kind::Rect ||
          light.kind == PreviewLight::Kind::Disk ||
          light.kind == PreviewLight::Kind::Cylinder) {
        float emit_cos = std::max(0.0f, Dot(light.normal, Mul(l, -1.0f)));
        if (emit_cos <= 0.0f) return;
        radiance = Mul(radiance, emit_cos * std::max(1.0f, light.area));
      } else if (light.kind == PreviewLight::Kind::Sphere &&
                 light.radius > 1.0e-5f) {
        // Small-sphere limit of the area integral: E = L * (projected area
        // pi*r^2) / d^2. This is what NEE converges to for a distant sphere, so
        // the punctual fallback (NEE disabled, non-bsdf modes) agrees with it
        // for both normalize settings. At or below the gate the sphere is a
        // point-light stand-in and keeps the plain I/d^2 (its radiance was
        // never area-divided at collection).
        radiance = Mul(radiance, MTLX_PI * light.radius * light.radius);
      }
      radiance = Mul(radiance, 1.0f / std::max(1.0e-4f, dist * dist));
    }
    if (Dot(n, l) <= 0.0f) return;
    if (opt.shadows && light.shadow_enable && occluded(p, n, l, max_t)) {
      return;
    }
    Vec3 direct_diffuse;
    c = Add(c, EvalMaterialDirect(tri, n, view, l, radiance, opt,
                                  tri_openpbr, &direct_diffuse));
    c_diffuse = Add(c_diffuse, direct_diffuse);
  };
  for (const PreviewLight &light : lights.finite) {
    eval_light(light);
  }
  for (const PreviewLight &light : lights.mesh) {
    eval_light(light);
  }
  // Area-sampled lights. Its own RNG stream: seeding from (org, dir, depth) like
  // the BSDF sampler would correlate the light sample with the BSDF sample and
  // defeat the whole point of combining them.
  {
    pcg32 lrng = MakeBsdfRng(p, n, depth + 0x5eed);
    for (const PreviewLight &light : lights.finite) {
      if (is_area_sampled(light)) nee_light(light, &lrng);
    }
  }
  c = Add(c, sample_bsdf_bounce());
  // primvars:displayOpacity < 1: see-through. Blend the surface shade with what
  // lies behind it (continuation ray), bounded recursion. Opaque hits (the
  // default opacity 1.0) skip this entirely, so opaque renders are unchanged.
  // "transparent" opacityMode blends only the diffuse half and keeps the
  // specular at full weight.
  if (transparent_opacity)
    return Add(Sub(c, c_diffuse), apply_opacity(c_diffuse));
  return apply_opacity(c);
}

uint8_t ToSRGB8(float linear, bool srgb_display_transform) {
  linear = std::max(0.0f, linear);
  const float mapped = srgb_display_transform ? linear : linear / (1.0f + linear);
  float srgb = (mapped <= 0.0031308f)
                   ? (12.92f * mapped)
                   : (1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f);
  int v = int(std::round(std::max(0.0f, std::min(1.0f, srgb)) * 255.0f));
  return uint8_t(std::max(0, std::min(255, v)));
}

void MakeRay(const CameraFrame &camera, float aspect, float sx, float sy,
             Vec3 *org, Vec3 *dir) {
  float px = 2.0f * sx - 1.0f + camera.shift_x;
  float py = 1.0f - 2.0f * sy + camera.shift_y;
  if (camera.ortho) {
    float xmag = camera.xmag;
    float ymag = camera.ymag;
    if (xmag <= 0.0f) xmag = ymag * aspect;
    if (ymag <= 0.0f) ymag = xmag / std::max(1.0e-6f, aspect);
    *org = Add(camera.origin,
               Add(Mul(camera.right, px * xmag * 0.5f),
                   Mul(camera.up, py * ymag * 0.5f)));
    *dir = camera.forward;
    return;
  }
  float tan_y = std::tan(camera.yfov * 0.5f);
  Vec3 d = Add(camera.forward,
               Add(Mul(camera.right, px * aspect * tan_y),
                   Mul(camera.up, py * tan_y)));
  *org = camera.origin;
  *dir = Normalize(d);
}

// ===========================================================================
// UsdVol volume rendering (simple emission/absorption raymarch).
//
// Each RenderVolume's density field is a dense float grid spanning an
// object-space AABB [bmin, bmax]. We transform the primary camera ray into the
// volume's object space, ray-march front-to-back accumulating absorption +
// (albedo) single-scatter + emission, and composite over the surface color.
//
// Limitation ("simple firstly"): no depth sorting against surfaces inside the
// volume (the surface color is treated as the background behind the whole
// volume). Good for isolated volumes; refine later.
// ===========================================================================


// Fast blackbody approximation in linear-ish RGB. VDB fire fields are often
// normalized rather than Kelvin, so map [0,1] to a useful 1000..6500 K range.
static Vec3 VolumeTemperatureColor(float temperature) {
  float kelvin = temperature > 100.0f
                     ? temperature
                     : 1000.0f + 5500.0f * std::max(0.0f, temperature);
  float t = std::max(10.0f, std::min(400.0f, kelvin / 100.0f));
  float r = t <= 66.0f ? 1.0f
                       : 1.2929362f * std::pow(t - 60.0f, -0.13320476f);
  float g = t <= 66.0f
                ? 0.39008158f * std::log(t) - 0.63184144f
                : 1.1298909f * std::pow(t - 60.0f, -0.07551485f);
  float b = t >= 66.0f ? 1.0f
                       : (t <= 19.0f ? 0.0f
                                     : 0.5432068f * std::log(t - 10.0f) -
                                           1.1962541f);
  return Vec3{std::max(0.0f, std::min(1.0f, r)),
              std::max(0.0f, std::min(1.0f, g)),
              std::max(0.0f, std::min(1.0f, b))};
}

// Slab ray/AABB intersection. Returns the [t0,t1] overlap (t in `o`/`d` units).
static bool RayAABBVol(const Vec3 &o, const Vec3 &d, const Vec3 &bmin,
                       const Vec3 &bmax, float *t0, float *t1) {
  float tmin = -1e30f, tmax = 1e30f;
  const float od[3] = {o.x, o.y, o.z};
  const float dd[3] = {d.x, d.y, d.z};
  const float lo[3] = {bmin.x, bmin.y, bmin.z};
  const float hi[3] = {bmax.x, bmax.y, bmax.z};
  for (int a = 0; a < 3; a++) {
    if (std::fabs(dd[a]) < 1e-12f) {
      if (od[a] < lo[a] || od[a] > hi[a]) return false;
    } else {
      float inv = 1.0f / dd[a];
      float ta = (lo[a] - od[a]) * inv;
      float tb = (hi[a] - od[a]) * inv;
      if (ta > tb) std::swap(ta, tb);
      if (ta > tmin) tmin = ta;
      if (tb < tmax) tmax = tb;
      if (tmin > tmax) return false;
    }
  }
  *t0 = tmin;
  *t1 = tmax;
  return true;
}

// --- Level sets (UsdVol fieldClass == "levelSet") -------------------------
// The field holds signed distances in world units (negative inside), so it is
// shaded as an isosurface: sphere-trace to the first zero crossing, take the
// normal from the distance gradient and shade it as a matte surface with the
// scene's punctual lights. Fog fields keep the emission/absorption raymarch.
// Volume surfaces do not receive shadow rays or indirect light (documented
// limit of this preview path), and they replace the background only when they
// are in front of the scene geometry along the primary ray.

static bool TraceLevelSetSurface(const VolumeData &vd, const Vec3 &o,
                                 const Vec3 &d, float t0, float t1,
                                 float *t_hit, size_t *work = nullptr) {
  if (!t_hit) return false;
  const float dir_len = Length(d);
  if (!std::isfinite(dir_len) || dir_len <= 1.0e-9f) return false;
  // t is the world-ray parameter, while the field and voxel size are in
  // object space. Convert all object-space distances to ray parameters.
  const float min_step = std::max(1.0e-6f, vd.voxel_size * 0.25f) / dir_len;
  // Capping the sphere-trace step degrades to a fine march when the stored
  // values are not world-unit distances (a mis-scaled SDF then still finds
  // the crossing instead of stepping over it).
  const float max_step = std::max(min_step, vd.voxel_size * 4.0f / dir_len);
  auto sample_at = [&](float t) {
    if (work) ++*work;
    return SampleVolumeDensity(vd, Add(o, Mul(d, t)));
  };
  float t_prev = t0;
  float v_prev = sample_at(t_prev);
  if (work && *work > 1000000) return false;
  // The first sign change along the ray is the outermost surface, whatever the
  // stored sign convention (narrow-band level sets store a positive value
  // inside the solid's core, so "negative" is not the whole interior).
  const bool entry_negative = v_prev < 0.0f;
  auto other_side = [&](float v) { return (v < 0.0f) != entry_negative; };
  const int max_steps = 4096;
  for (int i = 0; i < max_steps; ++i) {
    const float step = std::max(
        min_step,
        std::min(max_step, std::fabs(v_prev) / dir_len * 0.8f));
    const float t_next = std::min(t_prev + step, t1);
    const float v_next = sample_at(t_next);
    if (work && *work > 1000000) return false;
    if (other_side(v_next)) {
      float lo = t_prev, hi = t_next;
      for (int k = 0; k < 16; ++k) {
        const float mid = 0.5f * (lo + hi);
        const bool crossed = other_side(sample_at(mid));
        if (work && *work > 1000000) return false;
        if (crossed) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      *t_hit = hi;
      return true;
    }
    if (t_next >= t1) return false;
    t_prev = t_next;
    v_prev = v_next;
  }
  return false;
}

// Central-difference gradient of the stored field at an object-space point,
// in object-space units.
static Vec3 LevelSetObjectGradient(const VolumeData &vd, const Vec3 &p) {
  const float h = std::max(1.0e-6f, vd.voxel_size * 0.5f);
  const Vec3 dx{h, 0.0f, 0.0f}, dy{0.0f, h, 0.0f}, dz{0.0f, 0.0f, h};
  const Vec3 g{
      SampleVolumeDensity(vd, Add(p, dx)) - SampleVolumeDensity(vd, Sub(p, dx)),
      SampleVolumeDensity(vd, Add(p, dy)) - SampleVolumeDensity(vd, Sub(p, dy)),
      SampleVolumeDensity(vd, Add(p, dz)) - SampleVolumeDensity(vd, Sub(p, dz))};
  return Mul(g, 1.0f / (2.0f * h));
}

static Vec3 ShadeLevelSetSurface(const VolumeData &vd, const Vec3 &p,
                                 const Vec3 &n, const LightCache &lights,
                                 float ambient) {
  Vec3 c = Mul(vd.albedo, ambient);
  auto add_light = [&](const PreviewLight &light) {
    Vec3 radiance = light.radiance;
    Vec3 l;
    if (light.kind == PreviewLight::Kind::Distant) {
      l = Normalize(Mul(light.direction, -1.0f));
    } else {
      const Vec3 d = Sub(light.position, p);
      const float dist = Length(d);
      if (dist <= 1.0e-6f) return;
      l = Mul(d, 1.0f / dist);
      if (light.kind == PreviewLight::Kind::Mesh ||
          light.kind == PreviewLight::Kind::Rect ||
          light.kind == PreviewLight::Kind::Disk ||
          light.kind == PreviewLight::Kind::Cylinder) {
        const float emit_cos = std::max(0.0f, Dot(light.normal, Mul(l, -1.0f)));
        if (emit_cos <= 0.0f) return;
        radiance = Mul(radiance, emit_cos * std::max(1.0f, light.area));
      } else if (light.kind == PreviewLight::Kind::Sphere &&
                 light.radius > 1.0e-5f) {
        radiance = Mul(radiance, MTLX_PI * light.radius * light.radius);
      }
      radiance = Mul(radiance, 1.0f / std::max(1.0e-4f, dist * dist));
    }
    const float ndotl = Dot(n, l);
    if (ndotl <= 0.0f) return;
    c = Add(c, Vec3{vd.albedo.x * radiance.x * ndotl,
                    vd.albedo.y * radiance.y * ndotl,
                    vd.albedo.z * radiance.z * ndotl});
  };
  for (const PreviewLight &light : lights.finite) add_light(light);
  for (const PreviewLight &light : lights.mesh) add_light(light);
  // The volume material's emission, as in the fog path (already scaled by
  // emission_scale when the volume was built).
  return Add(c, vd.emission);
}

// Composite all volumes along the world-space primary ray over `bg`.
// `scene_hit` is the world-space distance to the primary geometry hit (or the
// far plane); a level-set surface behind it does not replace the geometry.
struct MixedVolumeInterval {
  size_t index;
  Vec3 origin, direction;
  float begin, end, step;
};

static Vec3 CompositeMixedVolumes(
    const std::vector<VolumeData>& vols, const Vec3& worg, const Vec3& wdir,
    Vec3 background, const LightCache& lights, float ambient, float scene_hit,
    const CameraFrame& camera, Options::VolumeLevelSet level_set_shading,
    const std::vector<std::unique_ptr<lightusd::tydra::MtlxVolumeEvaluator>>* evaluators,
    std::atomic<bool>* material_error, const IblCache* ibl, lrt_tri_scene* scene,
    const std::vector<FlatTri>& tris, const DirectScene* direct,
    const lrt_tlas* tlas, const std::vector<TriangleSceneChunk>* chunks,
    const Options& opt) {
  float far = scene_hit;
  Vec3 result = background;
  // Surfaces first: fog in front is integrated over the nearest surface.
  for (const VolumeData& vd : vols) {
    if (!vd.level_set || level_set_shading != Options::VolumeLevelSet::Surface) continue;
    const Vec3 o = TransformPoint(vd.inv_world, worg), d = TransformVector(vd.inv_world, wdir);
    float a, b, hit;
    if (!RayAABBVol(o, d, vd.bmin, vd.bmax, &a, &b) ||
        b <= std::max(a, 0.0f) ||
        !TraceLevelSetSurface(vd, o, d, std::max(a, 0.0f), b, &hit) || hit >= far) continue;
    const Vec3 p = Add(o, Mul(d, hit)), gradient = LevelSetObjectGradient(vd, p);
    Vec3 normal = Length(gradient) > 1.0e-8f ? TransformNormal(vd.inv_world, gradient)
                                               : Normalize(Mul(wdir, -1.0f));
    if (Dot(normal, wdir) > 0.0f) normal = Mul(normal, -1.0f);
    result = ShadeLevelSetSurface(vd, TransformPoint(vd.world, p), normal, lights, ambient);
    far = hit;
  }
  std::vector<MixedVolumeInterval> intervals;
  std::vector<float> boundaries;
  for (size_t i = 0; i < vols.size(); ++i) {
    const VolumeData& vd = vols[i];
    if (vd.level_set && level_set_shading == Options::VolumeLevelSet::Surface) continue;
    const Vec3 o = TransformPoint(vd.inv_world, worg), d = TransformVector(vd.inv_world, wdir);
    float a, b;
    if (!RayAABBVol(o, d, vd.bmin, vd.bmax, &a, &b)) continue;
    a = std::max(a, 0.0f); b = std::min(b, far);
    if (b <= a) continue;
    const float speed = Length(d);
    const Vec3 extent = Sub(vd.bmax, vd.bmin);
    const float sx = vd.dim[0] > 0 ? extent.x / float(vd.dim[0]) : extent.x;
    const float sy = vd.dim[1] > 0 ? extent.y / float(vd.dim[1]) : extent.y;
    const float sz = vd.dim[2] > 0 ? extent.z / float(vd.dim[2]) : extent.z;
    const float voxel = std::min(sx, std::min(sy, sz));
    const float step = voxel > 0 ? voxel * 0.5f / speed : (b - a) / 128.0f;
    if (!(speed > 0) || !std::isfinite(speed) || !std::isfinite(a) ||
        !std::isfinite(b) || !(step > 0) || !std::isfinite(step)) {
      material_error->store(true, std::memory_order_relaxed); return background;
    }
    intervals.push_back({i, o, d, a, b, step});
    boundaries.push_back(a); boundaries.push_back(b);
    if (intervals.size() > 1024) {
      material_error->store(true, std::memory_order_relaxed); return background;
    }
  }
  if (intervals.empty()) return result;
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
  const float world_speed = Length(wdir);
  if (!(world_speed > 0) || !std::isfinite(world_speed)) {
    material_error->store(true, std::memory_order_relaxed); return background;
  }
  Vec3 trans{1, 1, 1}, radiance{0, 0, 0};
  size_t work = 0;
  // Reuse the camera's graph context when evaluating extinction along light rays.
  auto sample_material = [&](size_t index, const Vec3& world, const Vec3& local,
                             lightusd::tydra::MtlxVolumeSample* sample) -> bool {
    const VolumeData& vd = vols[index];
    if (!evaluators || index >= evaluators->size() || !(*evaluators)[index] ||
        !vd.materialx_fields) {
      material_error->store(true, std::memory_order_relaxed); return false;
    }
    const Vec3 extent = Sub(vd.bmax, vd.bmin);
    lightusd::tydra::MtlxVolumeContext context;
    context.position[0] = world.x; context.position[1] = world.y; context.position[2] = world.z;
    context.uv[0] = extent.x > 0 ? (local.x - vd.bmin.x) / extent.x : 0;
    context.uv[1] = extent.y > 0 ? (local.y - vd.bmin.y) / extent.y : 0;
    context.view[0] = -wdir.x / world_speed; context.view[1] = -wdir.y / world_speed;
    context.view[2] = -wdir.z / world_speed;
    context.frame = vd.materialx_frame; context.time = vd.materialx_time;
    context.has_space_transforms = true;
    for (size_t c = 0; c < 4; ++c) for (size_t r = 0; r < 4; ++r) {
      context.object_to_world[c * 4 + r] = float(vd.materialx_world.m[c][r]);
      context.world_to_object[c * 4 + r] = float(vd.materialx_inv_world.m[c][r]);
    }
    const Vec3 axes[3] = {camera.right, camera.up, Mul(camera.forward, -1.0f)};
    for (size_t c = 0; c < 3; ++c) {
      const float axis[3] = {axes[c].x, axes[c].y, axes[c].z};
      for (size_t r = 0; r < 3; ++r) {
        context.view_to_world[c * 4 + r] = axis[r];
        context.world_to_view[r * 4 + c] = axis[r];
      }
      context.world_to_view[12 + c] = -Dot(axes[c], camera.origin);
    }
    context.view_to_world[12] = camera.origin.x; context.view_to_world[13] = camera.origin.y;
    context.view_to_world[14] = camera.origin.z;

    if (!(*evaluators)[index]->EvaluateWithFields(vd.materialx_volume,
            *vd.materialx_fields, context, sample)) {
      if (!material_error->exchange(true, std::memory_order_relaxed))
        std::cerr << "MaterialX volume evaluation: " << (*evaluators)[index]->GetError() << "\n";
      return false;
    }
    return true;
  };
  // Integrate extinction separately in each medium. Products of their
  // transmittances give the same result as summing extinction in overlaps.
  auto light_transmittance = [&](const Vec3& origin, const Vec3& direction,
                                 float max_t, Vec3* transmittance) -> bool {
    *transmittance = Vec3{1, 1, 1};
    for (size_t index = 0; index < vols.size(); ++index) {
      if (++work > 1000000) return false;
      const VolumeData& vd = vols[index];
      const Vec3 local_origin = TransformPoint(vd.inv_world, origin);
      const Vec3 local_direction = TransformVector(vd.inv_world, direction);
      float a, b;
      if (!RayAABBVol(local_origin, local_direction, vd.bmin, vd.bmax, &a, &b)) continue;
      a = std::max(a, 0.0f); b = std::min(b, max_t);
      if (!(b > a)) continue;
      if (vd.level_set && level_set_shading == Options::VolumeLevelSet::Surface) {
        float hit;
        const bool blocked = TraceLevelSetSurface(vd, local_origin, local_direction,
                                                  a, b, &hit, &work);
        if (work > 1000000) return false;
        if (blocked) {
          *transmittance = Vec3{0, 0, 0};
          return true;
        }
        continue;
      }
      const float speed = Length(local_direction);
      const Vec3 extent = Sub(vd.bmax, vd.bmin);
      const float sx = vd.dim[0] > 0 ? extent.x / float(vd.dim[0]) : extent.x;
      const float sy = vd.dim[1] > 0 ? extent.y / float(vd.dim[1]) : extent.y;
      const float sz = vd.dim[2] > 0 ? extent.z / float(vd.dim[2]) : extent.z;
      const float voxel = std::min(sx, std::min(sy, sz));
      if (!(speed > 0) || !std::isfinite(speed) || !std::isfinite(a) || !std::isfinite(b)) return false;
      const float step = std::max(voxel > 0 ? voxel * 0.5f / speed : (b - a) / 128.0f,
                                  (b - a) / 128.0f);
      if (!(step > 0) || !std::isfinite(step)) return false;
      float optical_depth[3] = {};
      for (float t = a; t < b;) {
        if (++work > 1000000) return false;
        const float dt = std::min(step, b - t);
        if (!(dt > 0) || !(t + dt > t)) return false;
        const float midpoint = t + 0.5f * dt;
        const Vec3 local = Add(local_origin, Mul(local_direction, midpoint));
        float extinction[3];
        if (vd.materialx_volume.materialx_binding) {
          lightusd::tydra::MtlxVolumeSample sample;
          if (!sample_material(index, Add(origin, Mul(direction, midpoint)), local, &sample)) return false;
          for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(sample.absorption[c]) || !std::isfinite(sample.scattering[c]) ||
                sample.absorption[c] < 0 || sample.scattering[c] < 0) return false;
            extinction[c] = sample.absorption[c] + sample.scattering[c];
          }
        } else {
          const float raw = SampleVolumeDensity(vd, local);
          if (!std::isfinite(raw) || !std::isfinite(vd.background) ||
              !std::isfinite(vd.density_scale)) return false;
          const float density = vd.level_set ? std::max(0.0f, -raw) * vd.density_scale
                                              : std::max(0.0f, raw - vd.background) * vd.density_scale;
          if (!std::isfinite(density) || density < 0) return false;
          extinction[0] = extinction[1] = extinction[2] = density;
        }
        for (int c = 0; c < 3; ++c) optical_depth[c] += extinction[c] * dt;
        t += dt;
      }
      transmittance->x *= std::exp(-optical_depth[0]);
      transmittance->y *= std::exp(-optical_depth[1]);
      transmittance->z *= std::exp(-optical_depth[2]);
    }
    return true;
  };
  for (size_t interval = 0; interval + 1 < boundaries.size(); ++interval) {
    float t = boundaries[interval];
    const float end = boundaries[interval + 1];
    float step = end - t;
    for (const auto& active : intervals) {
      if (++work > 1000000) {
        material_error->store(true, std::memory_order_relaxed); return background;
      }
      if (active.begin <= t && active.end >= end) step = std::min(step, active.step);
    }
    step = std::max(step, (end - t) / 4096.0f);
    while (t < end) {
      const float dt = std::min(step, end - t);
      if (!(dt > 0) || !(t + dt > t)) {
        material_error->store(true, std::memory_order_relaxed); return background;
      }
      const float midpoint = t + 0.5f * dt;
      float extinction[3] = {}, source[3] = {};
      for (const auto& active : intervals) {
        if (++work > 1000000) {
          material_error->store(true, std::memory_order_relaxed); return background;
        }
        if (active.begin > t || active.end < end) continue;
        const VolumeData& vd = vols[active.index];
        const Vec3 local = Add(active.origin, Mul(active.direction, midpoint));
        if (vd.materialx_volume.materialx_binding) {
          const Vec3 world = Add(worg, Mul(wdir, midpoint));
          lightusd::tydra::MtlxVolumeSample sample;
          if (!sample_material(active.index, world, local, &sample)) return background;
          for (int c = 0; c < 3; ++c) {
            if (sample.absorption[c] < 0 || sample.scattering[c] < 0 || sample.emission[c] < 0) {
              material_error->store(true, std::memory_order_relaxed); return background;
            }
            extinction[c] += sample.absorption[c] + sample.scattering[c];
            source[c] += sample.emission[c];
          }
          for (size_t lobe_index = 0; lobe_index < sample.lobe_count; ++lobe_index) {
            const auto& lobe = sample.lobes[lobe_index];
            Vec3 incident{0, 0, 0};
            if (!std::isfinite(lobe.anisotropy) || std::abs(lobe.anisotropy) >= 1.0f ||
                lobe.scattering[0] < 0 || lobe.scattering[1] < 0 || lobe.scattering[2] < 0) {
              material_error->store(true, std::memory_order_relaxed); return background;
            }
            const float g = lobe.anisotropy;
            auto add_scattered_light = [&](const PreviewLight& light) {
              auto accumulate = [&](const Vec3& l, Vec3 incoming, float max_t) {
                if (++work > 1000000) { material_error->store(true, std::memory_order_relaxed); return; }
                if (opt.shadows && light.shadow_enable &&
                    (tlas ? OccludedTLAS(tlas, world, l, l, max_t)
                          : Occluded(scene, tris, world, l, l, max_t, direct,
                                     opt.purpose_mask, chunks))) return;
                if (opt.shadows && light.shadow_enable) {
                  Vec3 transmittance;
                  if (!light_transmittance(world, l, max_t, &transmittance)) {
                    material_error->store(true, std::memory_order_relaxed); return;
                  }
                  incoming = Vec3{incoming.x * transmittance.x, incoming.y * transmittance.y,
                                  incoming.z * transmittance.z};
                }
                const float cosine = std::clamp(Dot(l, Mul(wdir, 1.0f / world_speed)), -1.0f, 1.0f);
                const float denom = 1.0f + g * g - 2.0f * g * cosine;
                const float phase = (1.0f - g * g) / (4.0f * MTLX_PI * denom * std::sqrt(denom));
                incident = Add(incident, Mul(incoming, phase));
              };
              const bool sphere = light.kind == PreviewLight::Kind::Sphere && light.radius > 1.0e-5f;
              const bool shaped = light.kind == PreviewLight::Kind::Rect ||
                                  light.kind == PreviewLight::Kind::Disk ||
                                  light.kind == PreviewLight::Kind::Cylinder ||
                                  light.kind == PreviewLight::Kind::Mesh;
              if (shaped || (sphere && Length(Sub(light.position, world)) > light.radius + 1.0e-5f)) {
                // Stratified surface or solid-angle quadrature; invalid/back-facing
                // samples have zero contribution and retain their share of the PDF.
                constexpr int kAreaSamples = 16;
                for (int i = 0; i < kAreaSamples; ++i) {
                  if (++work > 1000000) {
                    material_error->store(true, std::memory_order_relaxed); return;
                  }
                  const float u = (float(i / 4) + 0.5f) * 0.25f;
                  const float v = (float(i % 4) + 0.5f) * 0.25f;
                  LightSample ls;
                  const bool valid = sphere ? SampleSphereLight(light, world, u, v, &ls)
                                            : SampleAreaLight(light, world, u, v, &ls);
                  if (valid) accumulate(ls.wi, Mul(ls.radiance, 1.0f / (float(kAreaSamples) * ls.pdf)),
                                        std::max(0.0f, ls.dist - 1.0e-3f));
                  if (material_error->load(std::memory_order_relaxed)) return;
                }
                return;
              }
              Vec3 l, incoming = light.radiance;
              float max_t = 1.0e30f;
              if (light.kind == PreviewLight::Kind::Distant) {
                l = Normalize(Mul(light.direction, -1.0f));
              } else {
                const Vec3 delta = Sub(light.position, world);
                const float dist = Length(delta);
                if (dist <= 1.0e-6f) return;
                l = Mul(delta, 1.0f / dist);
                max_t = std::max(0.0f, dist - 1.0e-3f);
                if (sphere) incoming = Mul(incoming, MTLX_PI * light.radius * light.radius);
                if (!light.constant_attenuation)
                  incoming = Mul(incoming, 1.0f / std::max(1.0e-4f, dist * dist));
              }
              accumulate(l, incoming, max_t);
            };
            if (lobe.scattering[0] > 0 || lobe.scattering[1] > 0 || lobe.scattering[2] > 0) {
              for (const PreviewLight& light : lights.finite) {
                add_scattered_light(light);
                if (material_error->load(std::memory_order_relaxed)) return background;
              }
              for (const PreviewLight& light : lights.mesh) {
                add_scattered_light(light);
                if (material_error->load(std::memory_order_relaxed)) return background;
              }
              // Importance-sample the phase function. Its PDF cancels the phase
              // factor, so averaging radiance gives the spherical integral.
              if ((ibl && ibl->valid) || lights.has_dome) {
                constexpr int kEnvSamples = 16;
                const Vec3 axis = Mul(wdir, 1.0f / world_speed);
                Vec3 u_axis, v_axis;
                OrthoBasis(axis, &u_axis, &v_axis);
                for (int i = 0; i < kEnvSamples; ++i) {
                  if (++work > 1000000) {
                    material_error->store(true, std::memory_order_relaxed); return background;
                  }
                  const float u = (float(i) + 0.5f) / float(kEnvSamples);
                  float cosine = 2.0f * u - 1.0f;
                  if (std::abs(g) > 1.0e-4f) {
                    const float q = (1.0f - g * g) / (1.0f - g + 2.0f * g * u);
                    cosine = std::clamp((1.0f + g * g - q * q) / (2.0f * g), -1.0f, 1.0f);
                  }
                  const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
                  const float phi = float(i) * 2.39996323f;
                  const Vec3 l = Add(Mul(axis, cosine),
                      Mul(Add(Mul(u_axis, std::cos(phi)), Mul(v_axis, std::sin(phi))), sine));
                  Vec3 env = ibl && ibl->valid ? SampleEnv(ibl->env, EnvDir(*ibl, l))
                                               : lights.env_color;
                  if (opt.shadows) {
                    if (tlas ? OccludedTLAS(tlas, world, l, l, 1.0e30f)
                             : Occluded(scene, tris, world, l, l, 1.0e30f, direct,
                                        opt.purpose_mask, chunks)) continue;
                    Vec3 transmittance;
                    if (!light_transmittance(world, l, 1.0e30f, &transmittance)) {
                      material_error->store(true, std::memory_order_relaxed); return background;
                    }
                    env = Vec3{env.x * transmittance.x, env.y * transmittance.y,
                               env.z * transmittance.z};
                  }
                  incident = Add(incident, Mul(env, 1.0f / float(kEnvSamples)));
                }
              }
            }
            if (material_error->load(std::memory_order_relaxed)) return background;
            const float incident_channels[3] = {incident.x, incident.y, incident.z};
            for (int c = 0; c < 3; ++c)
              source[c] += lobe.scattering[c] * incident_channels[c];
          }
        } else {
          const float raw = SampleVolumeDensity(vd, local);
          if (!std::isfinite(raw) || !std::isfinite(vd.background) ||
              !std::isfinite(vd.density_scale)) {
            material_error->store(true, std::memory_order_relaxed); return background;
          }
          const float density = vd.level_set ? std::max(0.0f, -raw) * vd.density_scale
                                              : std::max(0.0f, raw - vd.background) * vd.density_scale;
          Vec3 emit = vd.emission;
          float weight = density;
          if (!vd.emission_field.data.empty()) {
            const float value = SampleVolumeField(vd.emission_field.data, vd.emission_field.dim,
                vd.emission_field.bmin, vd.emission_field.bmax, local) - vd.emission_field.background;
            if (!std::isfinite(value)) {
              material_error->store(true, std::memory_order_relaxed); return background;
            }
            weight = std::max(0.0f, value);
          }
          if (!vd.temperature_field.data.empty()) {
            const float value = SampleVolumeField(vd.temperature_field.data,
                vd.temperature_field.dim, vd.temperature_field.bmin, vd.temperature_field.bmax,
                local) - vd.temperature_field.background;
            if (!std::isfinite(value)) {
              material_error->store(true, std::memory_order_relaxed); return background;
            }
            const float temperature = std::max(0.0f, value);
            const Vec3 tint = VolumeTemperatureColor(temperature);
            emit = (emit.x + emit.y + emit.z) > 0 ? Vec3{emit.x * tint.x, emit.y * tint.y, emit.z * tint.z} : tint;
            weight = std::max(weight, temperature);
          }
          const float albedo[3] = {vd.albedo.x, vd.albedo.y, vd.albedo.z};
          const float emission[3] = {emit.x, emit.y, emit.z};
          for (int c = 0; c < 3; ++c) {
            extinction[c] += density;
            source[c] += density * albedo[c] + emission[c] * weight * vd.emission_scale;
          }
        }
      }
      float* tr[3] = {&trans.x, &trans.y, &trans.z};
      float* light[3] = {&radiance.x, &radiance.y, &radiance.z};
      const float distance = dt * world_speed;
      for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(extinction[c]) || !std::isfinite(source[c]) ||
            extinction[c] < 0 || source[c] < 0) {
          material_error->store(true, std::memory_order_relaxed); return background;
        }
        const float attenuation = std::exp(-extinction[c] * distance);
        const float integral = extinction[c] > 0 ? -std::expm1(-extinction[c] * distance) / extinction[c] : distance;
        *light[c] += *tr[c] * source[c] * integral;
        *tr[c] *= attenuation;
        if (!std::isfinite(*light[c]) || !std::isfinite(*tr[c])) {
          material_error->store(true, std::memory_order_relaxed); return background;
        }
      }
      t += dt;
    }
  }
  return Vec3{radiance.x + trans.x * result.x, radiance.y + trans.y * result.y,
              radiance.z + trans.z * result.z};
}

Vec3 CompositeVolumes(const std::vector<VolumeData> &vols, const Vec3 &worg,
                      const Vec3 &wdir, Vec3 bg, const LightCache &lights,
                      float ambient, float scene_hit,
                      const CameraFrame& camera,
                      Options::VolumeLevelSet level_set_shading,
                      const std::vector<std::unique_ptr<lightusd::tydra::MtlxVolumeEvaluator>>* evaluators,
                      std::atomic<bool>* material_error, const IblCache* ibl,
                      lrt_tri_scene* scene, const std::vector<FlatTri>& tris, const DirectScene* direct,
                      const lrt_tlas* tlas, const std::vector<TriangleSceneChunk>* chunks,
                      const Options& opt) {
  for (const VolumeData& volume : vols)
    if (volume.materialx_volume.materialx_binding)
      return CompositeMixedVolumes(vols, worg, wdir, bg, lights, ambient, scene_hit,
                                   camera, level_set_shading, evaluators, material_error,
                                   ibl, scene, tris, direct, tlas, chunks, opt);
  Vec3 result = bg;
  float nearest_surface = scene_hit;
  for (size_t volume_index = 0; volume_index < vols.size(); ++volume_index) {
    const VolumeData &vd = vols[volume_index];
    // World -> object space.
    Vec3 o = TransformPoint(vd.inv_world, worg);
    Vec3 d = TransformVector(vd.inv_world, wdir);
    float t0, t1;
    if (!RayAABBVol(o, d, vd.bmin, vd.bmax, &t0, &t1)) continue;
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 <= t0) continue;

    if (vd.level_set && level_set_shading == Options::VolumeLevelSet::Surface) {
      float hit = 0.0f;
      if (!TraceLevelSetSurface(vd, o, d, t0, t1, &hit)) continue;
      // o + d * hit is the inverse transform of worg + wdir * hit, so
      // `hit` already uses the same world-ray parameter as scene_hit.
      if (hit >= nearest_surface) continue;  // geometry or a volume is in front
      const Vec3 p_obj = Add(o, Mul(d, hit));
      const Vec3 n_obj = LevelSetObjectGradient(vd, p_obj);
      Vec3 n = Length(n_obj) > 1.0e-8f
                   ? TransformNormal(vd.inv_world, n_obj)
                   : Normalize(Mul(wdir, -1.0f));
      if (Dot(n, wdir) > 0.0f) n = Mul(n, -1.0f);
      result = ShadeLevelSetSurface(vd, TransformPoint(vd.world, p_obj), n,
                                    lights, ambient);
      nearest_surface = hit;
      continue;
    }


    const float ex = vd.bmax.x - vd.bmin.x;
    const float ey = vd.bmax.y - vd.bmin.y;
    const float ez = vd.bmax.z - vd.bmin.z;
    float vsx = (vd.dim[0] > 0) ? ex / float(vd.dim[0]) : ex;
    float vsy = (vd.dim[1] > 0) ? ey / float(vd.dim[1]) : ey;
    float vsz = (vd.dim[2] > 0) ? ez / float(vd.dim[2]) : ez;
    float vmin = std::min(vsx, std::min(vsy, vsz));
    float step = (vmin > 0.0f) ? vmin * 0.5f : (t1 - t0) / 128.0f;
    if (step <= 0.0f) step = (t1 - t0) / 128.0f;
    // Cap iterations to keep this bounded.
    int max_steps = 4096;

    float T = 1.0f;
    Vec3 L{0.0f, 0.0f, 0.0f};
    float t = t0;
    for (int i = 0; i < max_steps && t < t1; i++, t += step) {
      Vec3 p = Add(o, Mul(d, t + 0.5f * step));
      const float raw = SampleVolumeDensity(vd, p);
      // A level set marched as fog: the stored distance becomes a density
      // inside the surface (clamp(-sdf)); other fields keep their density.
      float dens = vd.level_set
                       ? std::max(0.0f, -raw) * vd.density_scale
                       : (raw - vd.background) * vd.density_scale;
      if (dens > 1.0e-5f) {
        float a = 1.0f - std::exp(-dens * step);
        Vec3 emit = vd.emission;
        float emit_weight = dens;
        if (!vd.emission_field.data.empty()) {
          emit_weight = std::max(0.0f,
              SampleVolumeField(vd.emission_field.data, vd.emission_field.dim,
                                vd.emission_field.bmin, vd.emission_field.bmax, p) -
                  vd.emission_field.background);
        }
        if (!vd.temperature_field.data.empty()) {
          const float temperature = std::max(0.0f,
              SampleVolumeField(vd.temperature_field.data,
                                vd.temperature_field.dim,
                                vd.temperature_field.bmin,
                                vd.temperature_field.bmax, p) -
                  vd.temperature_field.background);
          const Vec3 tint = VolumeTemperatureColor(temperature);
          // A temperature field is useful even when no explicit volume
          // material was authored: use it as physically motivated emission.
          emit = (emit.x + emit.y + emit.z) > 0.0f
                     ? Vec3{emit.x * tint.x, emit.y * tint.y, emit.z * tint.z}
                     : tint;
          emit_weight = std::max(emit_weight, temperature);
        }
        Vec3 src = Add(Mul(vd.albedo, a),
                       Mul(emit, emit_weight * vd.emission_scale * step));
        L = Add(L, Mul(src, T));
        T *= (1.0f - a);
        if (T < 0.003f) break;
      }
    }
    // Composite this volume over the accumulated background.
    result = Add(L, Mul(result, T));
  }
  return result;
}

namespace {
bool SampleBackPlate(const BackPlateImage &plate, float u, float v,
                     Vec3 *color, float *alpha, float *depth) {
  const float angle = plate.rotate_degrees * (3.14159265358979323846f / 180.0f);
  const float cs = std::cos(angle), sn = std::sin(angle);
  float x = u - 0.5f - plate.translate_x;
  float y = v - 0.5f - plate.translate_y;
  const float rx = cs * x + sn * y;
  const float ry = -sn * x + cs * y;
  u = rx / std::max(std::fabs(plate.scale_x), 1.0e-8f) + 0.5f;
  v = ry / std::max(std::fabs(plate.scale_y), 1.0e-8f) + 0.5f;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
  const int ix = std::min(plate.width - 1,
                          std::max(0, int(u * float(plate.width))));
  const int iy = std::min(plate.height - 1,
                          std::max(0, int(v * float(plate.height))));
  const size_t index = size_t(iy) * size_t(plate.width) + size_t(ix);
  Vec3 c = plate.color[index];
  auto grade = [](float value, float gain, float lift, float gamma) {
    value = std::max(0.0f, value * gain + lift);
    return std::pow(value, 1.0f / std::max(gamma, 1.0e-6f));
  };
  c.x = grade(c.x, plate.gain.x, plate.lift.x, plate.gamma.x);
  c.y = grade(c.y, plate.gain.y, plate.lift.y, plate.gamma.y);
  c.z = grade(c.z, plate.gain.z, plate.lift.z, plate.gamma.z);
  *color = c;
  *alpha = plate.alpha.empty() ? 1.0f
                               : ClampFloat(plate.alpha[index], 0.0f, 1.0f);
  *depth = plate.depth.empty()
               ? std::numeric_limits<float>::infinity()
               : plate.depth[index] * plate.depth_normalizing_factor +
                     plate.depth_min_offset + plate.depth_camera_space_offset;
  return true;
}
}  // namespace

lightusd::Image RenderImage(lrt_tri_scene *scene, const DirectScene *direct,
                            const std::vector<FlatTri> &tris,
                            const std::vector<TriMat> &mats,
                            const LightCache &lights, const IblCache *ibl,
                            const CameraFrame &camera, const Options &opt,
                            int height,
                            const std::vector<Texture> *textures,
                            const std::vector<float> *tri_uvs,
                            const std::vector<float> *tri_uvs1,
                            const lrt_tlas *tlas,
                            const std::vector<Blas> *blas,
                            const std::vector<InstanceRT> *instances,
                            const ByteVec *tri_colors,
                            const std::vector<float> *tri_normals,
                            const std::vector<VolumeData> *volumes,
                            const std::vector<lightusd::tydra::LightRtOpenPBRParams>
                                *openpbr_mats,
                            const std::vector<TriangleSceneChunk> *triangle_chunks,
                            const std::vector<BackPlateImage> *backplates) {
  lightusd::Image img;
  img.width = opt.width;
  img.height = height;
  img.channels = 4;
  const bool data_aov = opt.aov != CaptureAov::Color;
  img.bpp = data_aov ? 32 : 8;
  img.format = data_aov ? lightusd::Image::PixelFormat::Float : lightusd::Image::PixelFormat::UInt;
  img.data.resize(size_t(img.width) * size_t(img.height) * 4 * (data_aov ? 4 : 1));
  float aspect = float(img.width) / float(img.height);
  // Exactly `samples` anti-aliasing samples, distributed by the Halton(2,3)
  // low-discrepancy sequence (PixelJitter). This supersedes the old
  // ceil(sqrt(N))^2 regular grid, which both rounded the sample count up to a
  // square and aliased against regular geometry/texture patterns; Halton spreads
  // the same N samples evenly while staying fully deterministic in `s`.
  int spp = data_aov ? 1 : std::max(1, opt.samples);
  std::atomic<bool> diagnostic_exhausted{false};
  std::atomic<bool> volume_material_error{false};
  const unsigned nthreads = std::min<unsigned>(WorkerThreadCount(opt.threads),
                                                img.height > 0 ? unsigned(img.height) : 1u);
  size_t bound_volume_count = 0;
  if (volumes) for (const VolumeData& volume : *volumes)
    if (volume.materialx_volume.materialx_binding) ++bound_volume_count;

  // Scanlines are independent and write to disjoint pixel ranges, so render
  // them in parallel. Result is deterministic regardless of thread scheduling.
  auto render_rows = [&](int y_begin, int y_end) {
    std::vector<std::unique_ptr<lightusd::tydra::MtlxVolumeEvaluator>> volume_evaluators;
    if (volumes) {
      volume_evaluators.resize(volumes->size());
      lightusd::tydra::MtlxConverter converter;
      lightusd::tydra::MtlxConvertOptions volume_options;
      const uint64_t total_texture_budget = opt.texture_budget_mb > 0
          ? uint64_t(opt.texture_budget_mb) * 1024u * 1024u : uint64_t(256) * 1024u * 1024u;
      const uint64_t cache_count = uint64_t(std::max(1u, nthreads)) *
                                   std::max<size_t>(1, bound_volume_count);
      volume_options.volume_texture_max_decoded_bytes = static_cast<size_t>(
          std::max<uint64_t>(1, total_texture_budget / cache_count));
      converter.SetOptions(volume_options);
      for (size_t i = 0; i < volumes->size(); ++i) {
        if (!(*volumes)[i].materialx_volume.materialx_binding) continue;
        volume_evaluators[i] = converter.CreateVolumeEvaluator((*volumes)[i].materialx_volume);
        if (!volume_evaluators[i]) {
          if (!volume_material_error.exchange(true, std::memory_order_relaxed))
            std::cerr << "MaterialX volume setup: " << converter.GetError() << "\n";
          return;
        }
      }
    }
    for (int y = y_begin; y < y_end; y++) {
      for (int x = 0; x < img.width; x++) {
        Vec3 color{0.0f, 0.0f, 0.0f};
        for (int s = 0; s < spp; s++) {
          float jx, jy;
          PixelJitter(s, spp, &jx, &jy);
          float fx = (float(x) + jx) / float(img.width);
          float fy = (float(y) + jy) / float(img.height);
          Vec3 org;
          Vec3 dir;
          MakeRay(camera, aspect, fx, fy, &org, &dir);
          // One-pixel ray differentials for texture footprint / mip LOD
          // (origin + dir cover both pinhole and ortho cameras).
          RayDiff rd;
          MakeRay(camera, aspect, fx + 1.0f / float(img.width), fy, &rd.ox,
                  &rd.dx);
          MakeRay(camera, aspect, fx, fy + 1.0f / float(img.height), &rd.oy,
                  &rd.dy);
          rd.valid = true;
          CameraFrame ray_camera=camera;
          if (!camera.ortho) {
            const float cosine=std::max(1.0e-6f,Dot(camera.forward,dir));
            ray_camera.znear/=cosine; ray_camera.zfar/=cosine;
          }
          float primary_hit_t = ray_camera.zfar;
          Vec3 surf = Shade(scene, direct, tris, mats, lights, ibl, ray_camera, opt,
                            org, dir, textures, tri_uvs, tri_uvs1, tlas, blas,
                            instances, rd, 0, tri_colors, tri_normals,
                            openpbr_mats,
                            false, triangle_chunks, &primary_hit_t,
                            opt.diagnostic_emission ? &diagnostic_exhausted : nullptr);
          if (!data_aov && backplates) {
            for (const BackPlateImage &backplate : *backplates) {
              if (!backplate.valid()) continue;
              Vec3 plate_color;
              float plate_alpha = 0.0f;
              float plate_depth = 0.0f;
              if (SampleBackPlate(backplate, fx, fy, &plate_color, &plate_alpha,
                                  &plate_depth)) {
                const bool no_surface = primary_hit_t >= camera.zfar;
                const bool plate_in_front = !backplate.depth.empty() &&
                                            plate_depth > 0.0f &&
                                            plate_depth < primary_hit_t;
                if (no_surface || plate_in_front) {
                  surf = Add(Mul(plate_color, plate_alpha),
                             Mul(surf, 1.0f - plate_alpha));
                }
              }
            }
          }
          if (!data_aov && !opt.diagnostic_emission && volumes && !volumes->empty()) {
            surf = CompositeVolumes(*volumes, org, dir, surf, lights,
                                    opt.ambient, primary_hit_t, ray_camera,
                                    opt.volume_level_set, &volume_evaluators,
                                    &volume_material_error, ibl, scene, tris, direct,
                                    tlas, triangle_chunks, opt);
          }
          color = Add(color, surf);
        }
        color = Mul(color, 1.0f / float(spp));
        size_t ofs = (size_t(y) * size_t(img.width) + size_t(x)) * 4;
        if (data_aov) {
          const float values[4] = {color.x, color.y, color.z, 1.f};
          std::memcpy(img.data.data() + ofs * sizeof(float), values, sizeof(values));
          continue;
        }
        img.data[ofs + 0] = opt.diagnostic_emission
                                ? EncodeDiagnosticSRGB(color.x)
                                : ToSRGB8(color.x, opt.srgb_display_transform);
        img.data[ofs + 1] = opt.diagnostic_emission
                                ? EncodeDiagnosticSRGB(color.y)
                                : ToSRGB8(color.y, opt.srgb_display_transform);
        img.data[ofs + 2] = opt.diagnostic_emission
                                ? EncodeDiagnosticSRGB(color.z)
                                : ToSRGB8(color.z, opt.srgb_display_transform);
        img.data[ofs + 3] = 255;
      }
    }
  };
  if (nthreads <= 1) {
    render_rows(0, img.height);
  } else {
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    const int rows_per = (img.height + int(nthreads) - 1) / int(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) {
      const int y0 = int(t) * rows_per;
      const int y1 = std::min(img.height, y0 + rows_per);
      if (y0 >= y1) break;
      pool.emplace_back(render_rows, y0, y1);
    }
    for (std::thread &th : pool) th.join();
  }
  if (diagnostic_exhausted.load(std::memory_order_relaxed)) {
    std::cerr << "CPU material diagnostic exceeds the maximum 16 transparency layers.\n";
    return {};
  }
  if (volume_material_error.load(std::memory_order_relaxed)) {
    std::cerr << "MaterialX volume evaluation failed, exceeded limits, or produced negative coefficients.\n";
    return {};
  }
  return img;
}

bool LoadProgress(float progress, void *) {
  int percent = int(std::round(ClampFloat(progress, 0.0f, 1.0f) * 100.0f));
  std::cerr << "\rload: " << percent << "%" << std::flush;
  if (progress >= 1.0f) std::cerr << "\n";
  return true;
}

}  // namespace lusdr
