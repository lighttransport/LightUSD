// SPDX-License-Identifier: Apache-2.0
// lusdrender — the `next` lazy-loader pipeline: streamed geometry/material/volume
// /curve collection, instancing split, parallel triangle streaming + BVH build,
// next light/IBL setup, camera resolve, and the RunRTPreviewNext driver.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "minijson.hh"
#include "image-loader.hh"
#include "image-writer.hh"
#include "next/layer/asset-anchor.hh"   // AssetAnchorPath
#include "next/eval/value-clip.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/usd-shade.hh"  // GetInheritedBoundMaterialPath
#include "next/schema/usd-render.hh"
#include "next/schema/usd-geom-camera.hh"
#include "next/schema/usd-skel.hh"
#include "next/types/value-view.hh"
#include "tsd/tinysubdiv.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/next/openpbr-params-converter.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/render-extract.hh"
#include "tydra/openpbr-params.hh"
#include "tydra/texture-util.hh"
#include "usdVol.hh"
#include "lusdr_context.hh"
#include "lusdr_rt_lod.hh"

namespace lusdr {

bool ReadFloatArrayViewLazy(const lightusd::next::UsdPrim &prim,
                            const char *name, double time,
                            lightusd::tydra::next::ValueArrayRead<float> *out) {
  return lightusd::tydra::next::ReadFloatArray(prim, name, time, out);
}

bool AllowGaussianSHDecode(const lightusd::next::UsdPrim &prim) {
  const lightusd::next::Value *value = prim.GetPropertyValue(
      "radiance:sphericalHarmonicsCoefficients");
  if (!value || !value->is_array()) return true;
  constexpr size_t kMaxDecodedShBytes = size_t(128) * 1024 * 1024;
  const bool oversized = value->array_size() >
                         kMaxDecodedShBytes / sizeof(float);
  return !oversized || !value->is_lazy() ||
         lightusd::next::CanBorrowLazyFlat(*value);
}

bool ReadIntArrayViewLazy(const lightusd::next::UsdPrim &prim,
                          const char *name, double time,
                          lightusd::tydra::next::ValueArrayRead<int32_t> *out) {
  return lightusd::tydra::next::ReadIntArray(prim, name, time, out);
}

std::vector<float> ReadFloatArrayLazy(const lightusd::next::UsdPrim &prim,
                                      const char *name, double time) {
  return lightusd::tydra::next::ReadFloatArrayCopy(prim, name, time);
}

std::vector<int32_t> ReadIntArrayLazy(const lightusd::next::UsdPrim &prim,
                                      const char *name, double time) {
  return lightusd::tydra::next::ReadIntArrayCopy(prim, name, time);
}

std::vector<int64_t> ReadInt64ArrayLazy(const lightusd::next::UsdPrim &prim,
                                        const char *name, double time) {
  return lightusd::tydra::next::ReadInt64ArrayCopy(prim, name, time);
}

bool PointInstanceHidden(size_t index, size_t instance_count,
                         const std::vector<int64_t> &ids,
                         const std::unordered_set<int64_t> &hidden) {
  if (hidden.empty()) return false;
  if (ids.size() == instance_count) return hidden.count(ids[index]) != 0;
  if (index > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  return hidden.count(static_cast<int64_t>(index)) != 0;
}

std::vector<std::string> ReadTokenArrayNext(
    const lightusd::next::UsdPrim &prim, const char *name) {
  const lightusd::next::Value *v = prim.GetPropertyValue(name);
  if (!v) return {};
  if (const std::vector<std::string> *arr = v->as_token_array()) return *arr;
  if (const std::string *tok = v->as_token()) return {*tok};
  if (const std::string *str = v->as_string()) return {*str};
  return {};
}

std::unordered_map<std::string, float> GatherBlendShapeWeightsNext(
    const lightusd::next::Stage &stage, double time) {
  std::unordered_map<std::string, float> weights;
  stage.Traverse([&](const lightusd::next::UsdPrim &prim) {
    if (!lightusd::next::IsSkelAnimation(prim)) return true;
    const std::vector<std::string> names =
        ReadTokenArrayNext(prim, "blendShapes");
    if (names.empty()) return true;
    lightusd::next::AttributeEval eval(&stage);
    eval.SetTime(time);
    lightusd::next::EvalResult result = eval.Eval(prim, "blendShapeWeights");
    if (!result.success || !result.value.is_array()) return true;
    const std::vector<float> *arr = result.value.as_float_array();
    if (!arr) return true;
    const size_t n = std::min(names.size(), arr->size());
    for (size_t i = 0; i < n; ++i) weights[names[i]] = (*arr)[i];
    return true;
  });
  return weights;
}

std::vector<Vec3> ComputeBlendShapeOffsetsNext(
    const lightusd::next::Stage *stage, const lightusd::next::UsdPrim &mesh,
    const std::unordered_map<std::string, float> &weights, size_t npts) {
  if (!stage || npts == 0) return {};
  const std::vector<lightusd::next::Path> *targets =
      mesh.GetRelationship("skel:blendShapeTargets");
  if (!targets || targets->empty()) return {};

  const std::vector<std::string> names =
      ReadTokenArrayNext(mesh, "skel:blendShapes");
  if (weights.empty()) return {};

  std::vector<Vec3> offsets(npts, Vec3{0.0f, 0.0f, 0.0f});
  bool any = false;
  for (size_t ti = 0; ti < targets->size(); ++ti) {
    lightusd::next::UsdPrim shape_prim = stage->GetPrimAtPath((*targets)[ti]);
    if (!shape_prim.IsValid()) continue;
    const std::string shape_name =
        (ti < names.size() && !names[ti].empty()) ? names[ti] : shape_prim.GetName();
    auto wit = weights.find(shape_name);
    if (wit == weights.end() || wit->second == 0.0f) continue;

    lightusd::next::BlendShapeData data;
    if (!lightusd::next::GetBlendShapeData(*stage, shape_prim, &data)) continue;
    const size_t n = data.offsets.size() / 3;
    if (data.hasPointIndices && !data.pointIndices.empty()) {
      const size_t m = std::min(n, data.pointIndices.size());
      for (size_t i = 0; i < m; ++i) {
        const int32_t pi = data.pointIndices[i];
        if (pi < 0 || static_cast<size_t>(pi) >= npts) continue;
        offsets[static_cast<size_t>(pi)] =
            Add(offsets[static_cast<size_t>(pi)],
                Mul(Vec3{data.offsets[i * 3 + 0], data.offsets[i * 3 + 1],
                         data.offsets[i * 3 + 2]},
                    wit->second));
        any = true;
      }
    } else {
      const size_t m = std::min(n, npts);
      for (size_t i = 0; i < m; ++i) {
        offsets[i] = Add(offsets[i],
                         Mul(Vec3{data.offsets[i * 3 + 0],
                                  data.offsets[i * 3 + 1],
                                  data.offsets[i * 3 + 2]},
                             wit->second));
        any = true;
      }
    }
  }
  return any ? offsets : std::vector<Vec3>();
}

float ReadCamFloatNext(const lightusd::next::UsdPrim &prim, const char *name,
                       float fallback);

// Templated on the output buffer type so the flat path can stream into plain
// std::vector (ctx.tris) while the instanced path streams into the budget-tracked
// Blas FloatVec/TriVec (so the big instanced geometry is capped/pooled).
template <class FVec, class TVec>
void AddRTPreviewMeshNext(const lightusd::next::UsdPrim &prim,
                          const lightusd::next::Stage *stage,
                          const std::unordered_map<std::string, float> *blend_weights,
                          const matrix4d &world, lightusd::Purpose purpose,
                          uint32_t purpose_mask, double time,
                          const Vec3 &base_color, int32_t tex_id,
                          int32_t normal_tex_id, float roughness, float metallic,
                          const ScalarTex &rough_tex, const ScalarTex &metal_tex,
                          const Vec3 &emission, int32_t emission_tex_id,
                          float occlusion, const ScalarTex &occ_tex,
                          const ScalarTex &opacity_tex, float opacity_threshold,
                          float clearcoat, float clearcoat_roughness,
                          const ScalarTex &clearcoat_tex,
                          const ScalarTex &clearcoat_rough_tex,
                          const Vec3 &specular_color, int32_t specular_tex_id,
                          float ior, uint8_t use_specular_workflow,
                          const UvXform &uv_xform, bool want_uvs,
                          FVec *vertices, TVec *tris, FVec *tri_uvs,
                          Bounds *bounds, RTPreviewStats *stats,
                          const std::string &preferred_uv = std::string(),
                          // Face whitelist by AUTHORED face id (GeomSubset job
                          // split); null/empty = emit every face.
                          const std::vector<char> *face_mask = nullptr,
                          bool purpose_cull = false,
                          TriMat *out_job_mat = nullptr, float opacity = 1.0f,
                          bool want_colors = false, ByteVec *tri_colors = nullptr,
                          bool want_normals = false, FVec *tri_normals = nullptr,
                          // Indexed geometry (Phase 2b): when out_uverts != null,
                          // emit 1x unique verts + 3 vertex indices/tri (offset by
                          // *io_vbase) instead of writing the de-indexed soup.
                          FVec *out_uverts = nullptr, IdxVec *out_indices = nullptr,
                          uint32_t *io_vbase = nullptr,
                          // Coarse displacement (UsdPreviewSurface inputs:displacement):
                          // each unique vertex is offset along its smooth normal by the
                          // sampled height (const + optional channel-aware texture from
                          // tex_pool). disp_scale == 0 disables it.
                          float displacement = 0.0f,
                          int32_t displacement_tex_id = -1,
                          uint8_t displacement_ch = 0,
                          const std::vector<Texture> *tex_pool = nullptr,
                          float disp_scale = 0.0f,
                          float displacement_tex_scale = 1.0f,
                          float displacement_tex_bias = 0.0f) {
  // MeshLightAPI: this mesh is an AREA LIGHT, not just emissive geometry. The next
  // loader used to ignore that entirely -- LightCache::mesh was only ever built by
  // the legacy flatten -- so an emissive mesh lit nothing and was seen only by
  // whatever a BSDF bounce happened to hit. Read the LightAPI inputs off the prim
  // (the material carries no emission for these) and mark the material;
  // CollectMeshLightsNext turns the marked triangles into analytic lights.
  Vec3 mesh_emission = emission;
  uint8_t mesh_area_light = 0;
  {
    bool mesh_light = false;
    for (const std::string &api : prim.GetMeta().apiSchemas()) {
      if (api == "MeshLightAPI") { mesh_light = true; break; }
    }
    if (mesh_light) {
      const float li = ReadCamFloatNext(prim, "inputs:intensity", 1.0f);
      const float lx = ReadCamFloatNext(prim, "inputs:exposure", 0.0f);
      Vec3 lc{1.0f, 1.0f, 1.0f};
      if (const lightusd::next::Value *v = prim.GetPropertyValue("inputs:color"))
        if (const float *f = v->as_float3()) lc = Vec3{f[0], f[1], f[2]};
      // Same convention as the legacy CollectAllGeometry path (MeshLightEmission):
      // the light color is TINTED by the material -- its emissive color if it has
      // one, else its base color -- rather than emitting raw intensity.
      Vec3 light_color = Mul(lc, li * std::pow(2.0f, lx));
      // inputs:normalize: the light's POWER is held fixed as its area changes, so
      // the radiance is divided by the emitting area. Without this a normalized
      // mesh light scales with its own size -- exactly backwards. The area has to
      // be the WORLD area (a scaled mesh light emits over the scaled surface), and
      // it is measured here rather than in CollectMeshLightsNext because that runs
      // on the flat triangle list, where one material may be shared by several
      // meshes and their areas would be pooled. Re-reads the topology, which is
      // why it is gated on MeshLightAPI. Blendshape offsets are not applied: a
      // morphing light would otherwise change brightness every frame.
      bool normalize = false;
      if (const lightusd::next::Value *v =
              prim.GetPropertyValue("inputs:normalize"))
        if (const bool *b = v->as_bool()) normalize = *b;
      if (normalize) {
        const std::vector<float> npts_f = ReadFloatArrayLazy(prim, "points", time);
        const std::vector<int32_t> ncounts =
            ReadIntArrayLazy(prim, "faceVertexCounts", time);
        const std::vector<int32_t> nidx =
            ReadIntArrayLazy(prim, "faceVertexIndices", time);
        const size_t np = npts_f.size() / 3;
        auto wp = [&](int32_t i) {
          return TransformPoint(world, Vec3{npts_f[3 * size_t(i) + 0],
                                            npts_f[3 * size_t(i) + 1],
                                            npts_f[3 * size_t(i) + 2]});
        };
        float total = 0.0f;
        size_t cur = 0;
        for (int32_t c : ncounts) {
          if (c < 3 || cur + size_t(c) > nidx.size()) {
            cur += size_t(std::max<int32_t>(0, c));
            continue;
          }
          for (int32_t k = 1; k + 1 < c; k++) {
            const int32_t i0 = nidx[cur + 0], i1 = nidx[cur + size_t(k)],
                          i2 = nidx[cur + size_t(k + 1)];
            if (i0 < 0 || i1 < 0 || i2 < 0 || size_t(i0) >= np ||
                size_t(i1) >= np || size_t(i2) >= np) {
              continue;
            }
            total += TriangleArea(wp(i0), wp(i1), wp(i2));
          }
          cur += size_t(c);
        }
        if (total > 1.0e-8f) light_color = Mul(light_color, 1.0f / total);
      }
      const Vec3 tint = (Luminance(emission) > 1.0e-6f) ? emission : base_color;
      mesh_emission = Mul(light_color, tint);
      mesh_area_light = 1;
    }
  }

  // When the output is the slim TriStore (instanced BLAS), the per-mesh material
  // is emitted once into out_job_mat and each triangle stores only its mat_id.
  if (out_job_mat) {
    out_job_mat->base_color = base_color;
    out_job_mat->emission = mesh_emission;
    out_job_mat->area_light = mesh_area_light;
    out_job_mat->roughness = roughness;
    out_job_mat->metallic = metallic;
    out_job_mat->tex_id = tex_id;
    out_job_mat->normal_tex_id = normal_tex_id;
    out_job_mat->rough_tex_id = rough_tex.id;
    out_job_mat->metal_tex_id = metal_tex.id;
    out_job_mat->emission_tex_id = emission_tex_id;
    out_job_mat->occ_tex_id = occ_tex.id;
    out_job_mat->occlusion = occlusion;
    out_job_mat->opacity = opacity;
    out_job_mat->opacity_tex_id = opacity_tex.id;
    out_job_mat->opacity_threshold = opacity_threshold;
    out_job_mat->clearcoat = clearcoat;
    out_job_mat->clearcoat_roughness = clearcoat_roughness;
    out_job_mat->clearcoat_tex_id = clearcoat_tex.id;
    out_job_mat->clearcoat_rough_tex_id = clearcoat_rough_tex.id;
    out_job_mat->rough_ch = rough_tex.ch;
    out_job_mat->metal_ch = metal_tex.ch;
    out_job_mat->occ_ch = occ_tex.ch;
    out_job_mat->opacity_ch = opacity_tex.ch;
    out_job_mat->clearcoat_ch = clearcoat_tex.ch;
    out_job_mat->clearcoat_rough_ch = clearcoat_rough_tex.ch;
    out_job_mat->rough_tex_scale = rough_tex.scale;
    out_job_mat->rough_tex_bias = rough_tex.bias;
    out_job_mat->metal_tex_scale = metal_tex.scale;
    out_job_mat->metal_tex_bias = metal_tex.bias;
    out_job_mat->occ_tex_scale = occ_tex.scale;
    out_job_mat->occ_tex_bias = occ_tex.bias;
    out_job_mat->opacity_tex_scale = opacity_tex.scale;
    out_job_mat->opacity_tex_bias = opacity_tex.bias;
    out_job_mat->clearcoat_tex_scale = clearcoat_tex.scale;
    out_job_mat->clearcoat_tex_bias = clearcoat_tex.bias;
    out_job_mat->clearcoat_rough_tex_scale = clearcoat_rough_tex.scale;
    out_job_mat->clearcoat_rough_tex_bias = clearcoat_rough_tex.bias;
    out_job_mat->specular_color = specular_color;
    out_job_mat->specular_tex_id = specular_tex_id;
    out_job_mat->ior = ior;
    out_job_mat->use_specular_workflow = use_specular_workflow;
  }
  // Read core geometry without permanently materializing it into the stage.
  // Uncompressed USDC arrays are borrowed from the retained crate buffer; other
  // encodings decode into function-local scratch and are freed after this mesh.
  lightusd::tydra::next::ValueArrayRead<float> points;
  lightusd::tydra::next::ValueArrayRead<int32_t> counts;
  lightusd::tydra::next::ValueArrayRead<int32_t> indices;
  ReadFloatArrayViewLazy(prim, "points", time, &points);
  ReadIntArrayViewLazy(prim, "faceVertexCounts", time, &counts);
  ReadIntArrayViewLazy(prim, "faceVertexIndices", time, &indices);
  if (points.empty() || counts.empty() || indices.empty()) {
    stats->skipped_meshes++;
    return;
  }
  const size_t npts = points.size() / 3;
  // Transform each unique vertex ONCE up front. The triangle-fan loop below would
  // otherwise re-transform a shared/fan-center vertex for every incident corner
  // (~6x on a typical triangle mesh) via the double-precision matrix. These are
  // the same world-space floats the loop produced before, just deduplicated, so
  // the emitted soup / normals are byte-identical. (Phase 2b hands this array
  // straight to the indexed build instead of re-expanding it into a soup.)
  std::vector<Vec3> wpts(npts);
  const std::vector<Vec3> blend_offsets =
      blend_weights ? ComputeBlendShapeOffsetsNext(stage, prim, *blend_weights,
                                                   npts)
                    : std::vector<Vec3>();
  for (size_t i = 0; i < npts; i++) {
    Vec3 p{points[3 * i], points[3 * i + 1], points[3 * i + 2]};
    if (!blend_offsets.empty()) p = Add(p, blend_offsets[i]);
    wpts[i] = TransformPoint(world, p);
  }

  // UV (primvars:st) for the diffuse texture. Stored flat [u0,v0,u1,v1,...].
  // Interpolation is inferred from sizes: per-point ("vertex"/"varying") when
  // the UV count matches the point count, otherwise per-face-vertex
  // ("faceVarying"). primvars:st:indices indirection is honored when present.
  std::vector<float> st;
  std::vector<int32_t> st_indices;
  bool st_facevarying = false;
  bool have_st = false;
  if ((want_uvs || displacement_tex_id >= 0) &&
      (tex_id >= 0 || normal_tex_id >= 0 || rough_tex.id >= 0 ||
       metal_tex.id >= 0 || emission_tex_id >= 0 || occ_tex.id >= 0 ||
       opacity_tex.id >= 0 || clearcoat_tex.id >= 0 ||
       clearcoat_rough_tex.id >= 0 || specular_tex_id >= 0 ||
       displacement_tex_id >= 0)) {
    // Pick the UV set. If the bound base-color texture names one (its
    // UsdPrimvarReader varname), use that so a texture reading a secondary set
    // (e.g. `uvSet1`) is sampled with it, matching lusdview. Otherwise fall back
    // to the exporter preference list tydra-next's converter uses.
    std::string st_name;
    if (!preferred_uv.empty()) {
      st = ReadFloatArrayLazy(prim, ("primvars:" + preferred_uv).c_str(), time);
      if (!st.empty()) st_name = preferred_uv;
    }
    if (st.empty()) {
      for (const std::string &name :
           lightusd::tydra::next::MeshConfig{}.uv_primvar_names) {
        st = ReadFloatArrayLazy(prim, ("primvars:" + name).c_str(), time);
        if (!st.empty()) {
          st_name = name;
          break;
        }
      }
    }
    if (!st.empty()) {
      st_indices = ReadIntArrayLazy(
          prim, ("primvars:" + st_name + ":indices").c_str(), time);
      const size_t uv_count =
          st_indices.empty() ? (st.size() / 2) : st_indices.size();
      // Per-point if it matches the points; per-face-vertex if it matches the
      // face-vertex stream (== faceVertexIndices length).
      if (uv_count == npts) {
        st_facevarying = false;
        have_st = true;
      } else if (uv_count == indices.size()) {
        st_facevarying = true;
        have_st = true;
      }
    }
  }
  // Fetch the UV for face-vertex slot `fv` (position in the face-vertex stream)
  // whose underlying point index is `pi`. Returns {u,v}.
  auto uv_at = [&](size_t fv, int32_t pi) -> std::pair<float, float> {
    if (!have_st) return {0.0f, 0.0f};
    size_t s = st_facevarying ? fv : size_t(pi);
    if (!st_indices.empty()) {
      if (s >= st_indices.size()) return {0.0f, 0.0f};
      int32_t idx = st_indices[s];
      if (idx < 0) return {0.0f, 0.0f};
      s = size_t(idx);
    }
    if (s * 2 + 1 >= st.size()) return {0.0f, 0.0f};
    return {st[s * 2 + 0], st[s * 2 + 1]};
  };

  // Coarse displacement: offset each unique vertex along its smooth (area-weighted)
  // vertex normal by the sampled displacement scalar. Watertight (a shared vertex
  // moves exactly once) and memory-free (no new geometry). Geometric normals are
  // recomputed per-triangle from the displaced positions in the fan loop below, so
  // authored normals are intentionally dropped for displaced meshes (see below).
  const bool do_displace =
      disp_scale != 0.0f && (displacement_tex_id >= 0 || displacement != 0.0f);
  if (do_displace) {
    std::vector<Vec3> vn(npts, Vec3{0.0f, 0.0f, 0.0f});
    size_t cur = 0;
    for (int32_t c : counts) {
      if (c >= 3 && cur + size_t(c) <= indices.size()) {
        for (int32_t k = 1; k + 1 < c; k++) {
          int32_t a = indices[cur + 0], b = indices[cur + size_t(k)],
                  e = indices[cur + size_t(k + 1)];
          if (a < 0 || b < 0 || e < 0 || size_t(a) >= npts ||
              size_t(b) >= npts || size_t(e) >= npts)
            continue;
          // Area-weighted (un-normalized cross product) face normal accumulation.
          Vec3 fn = Cross(Sub(wpts[size_t(b)], wpts[size_t(a)]),
                          Sub(wpts[size_t(e)], wpts[size_t(a)]));
          vn[size_t(a)] = Add(vn[size_t(a)], fn);
          vn[size_t(b)] = Add(vn[size_t(b)], fn);
          vn[size_t(e)] = Add(vn[size_t(e)], fn);
        }
      }
      cur += size_t(std::max<int32_t>(0, c));
    }
    // First face-vertex slot referencing each point, for faceVarying UV lookup.
    std::vector<int64_t> first_fv;
    const bool need_fv = displacement_tex_id >= 0 && st_facevarying;
    if (need_fv) {
      first_fv.assign(npts, -1);
      cur = 0;
      for (int32_t c : counts) {
        if (c >= 0 && cur + size_t(c) <= indices.size())
          for (int32_t j = 0; j < c; j++) {
            int32_t pi = indices[cur + size_t(j)];
            if (pi >= 0 && size_t(pi) < npts && first_fv[size_t(pi)] < 0)
              first_fv[size_t(pi)] = int64_t(cur + size_t(j));
          }
        cur += size_t(std::max<int32_t>(0, c));
      }
    }
    const Texture *dtex =
        (displacement_tex_id >= 0 && tex_pool &&
         size_t(displacement_tex_id) < tex_pool->size())
            ? &(*tex_pool)[size_t(displacement_tex_id)]
            : nullptr;
    // Choose a displacement mip from the actual mesh/UV sampling density.
    // Sampling level zero at sparse vertices aliases high-frequency height maps
    // into arbitrary spikes. The longest incident edge footprint is a bounded,
    // topology-aware estimate of the texel area represented by each vertex;
    // dense meshes naturally retain level zero while coarse meshes low-pass.
    std::vector<float> displacement_lod(npts, 0.0f);
    const bool displacement_has_mips = dtex &&
        (!dtex->mips.empty() ||
         std::any_of(dtex->udim_tiles.begin(), dtex->udim_tiles.end(),
                     [](const Texture::UdimTile &tile) {
                       return !tile.mips.empty();
                     }));
    if (displacement_has_mips && have_st) {
      int texture_width = dtex->width;
      int texture_height = dtex->height;
      for (const Texture::UdimTile &tile : dtex->udim_tiles) {
        texture_width = std::max(texture_width, tile.width);
        texture_height = std::max(texture_height, tile.height);
      }
      const float tex_w = float(std::max(1, texture_width));
      const float tex_h = float(std::max(1, texture_height));
      cur = 0;
      for (int32_t c : counts) {
        if (c >= 2 && cur + size_t(c) <= indices.size()) {
          for (int32_t j = 0; j < c; ++j) {
            const size_t a_fv = cur + size_t(j);
            const size_t b_fv = cur + size_t((j + 1) % c);
            const int32_t a = indices[a_fv], b = indices[b_fv];
            if (a < 0 || b < 0 || size_t(a) >= npts || size_t(b) >= npts)
              continue;
            std::pair<float, float> auv = uv_at(a_fv, a);
            std::pair<float, float> buv = uv_at(b_fv, b);
            uv_xform.apply(&auv.first, &auv.second);
            uv_xform.apply(&buv.first, &buv.second);
            // Use authored (unwrapped) UV deltas. A triangle spanning exactly
            // one repeat period genuinely covers a full tile; folding the
            // delta to its shortest wrapped distance would incorrectly select
            // level zero for that common mapping.
            const float du = (buv.first - auv.first) * tex_w;
            const float dv = (buv.second - auv.second) * tex_h;
            const float lod = std::max(0.0f, std::log2(std::max(
                std::sqrt(du * du + dv * dv), 1.0f)));
            displacement_lod[size_t(a)] =
                std::max(displacement_lod[size_t(a)], lod);
            displacement_lod[size_t(b)] =
                std::max(displacement_lod[size_t(b)], lod);
          }
        }
        cur += size_t(std::max<int32_t>(0, c));
      }
    }
    for (size_t i = 0; i < npts; i++) {
      if (Length(vn[i]) < 1.0e-12f) continue;
      Vec3 n = Normalize(vn[i]);
      float d = displacement;
      if (dtex) {
        std::pair<float, float> uv =
            (need_fv && first_fv[i] >= 0)
                ? uv_at(size_t(first_fv[i]), int32_t(i))
                : uv_at(0, int32_t(i));
        uv_xform.apply(&uv.first, &uv.second);
        d = dtex->sample_channel(uv.first, uv.second, displacement_lod[i],
                                 displacement_ch) *
                displacement_tex_scale +
            displacement_tex_bias;
      }
      wpts[i] = Add(wpts[i], Mul(n, d * disp_scale));
    }
  }

  // Per-corner displayColor/displayOpacity (RGBA), when the scene has any
  // non-constant display primvar. Interpolation is inferred from array size:
  // vertex (== npoints), faceVarying (== face-vertex count), uniform (== nfaces);
  // a 1-element array (or absent) falls back to the constant base_color/opacity.
  std::vector<float> dcol, dopac;
  int dc_mode = 0, do_mode = 0;  // 0=const, 1=vertex, 2=faceVarying, 3=uniform
  if (want_colors) {
    dcol = ReadFloatArrayLazy(prim, "primvars:displayColor", time);
    const size_t nc = dcol.size() / 3;
    if (nc == npts) dc_mode = 1;
    else if (nc == indices.size()) dc_mode = 2;
    else if (nc == counts.size()) dc_mode = 3;
    dopac = ReadFloatArrayLazy(prim, "primvars:displayOpacity", time);
    const size_t no = dopac.size();
    if (no == npts) do_mode = 1;
    else if (no == indices.size()) do_mode = 2;
    else if (no == counts.size()) do_mode = 3;
  }
  // RGBA for face-vertex slot `fv` (point index `pi`, face `face`).
  auto col_at = [&](size_t fv, int32_t pi, size_t face, float out[4]) {
    out[0] = base_color.x; out[1] = base_color.y; out[2] = base_color.z;
    out[3] = opacity;
    if (dc_mode) {
      size_t ci = dc_mode == 1 ? size_t(pi) : dc_mode == 2 ? fv : face;
      if (ci * 3 + 2 < dcol.size()) {
        out[0] = dcol[ci * 3 + 0]; out[1] = dcol[ci * 3 + 1];
        out[2] = dcol[ci * 3 + 2];
      }
    }
    if (do_mode) {
      size_t oi = do_mode == 1 ? size_t(pi) : do_mode == 2 ? fv : face;
      if (oi < dopac.size()) out[3] = std::min(1.0f, std::max(0.0f, dopac[oi]));
    }
  };

  // Authored normals for smooth shading (`-smooth`). Stored per corner in the
  // job's frame (world for flat, prototype-local for instanced — `world` is
  // identity there), transformed at hit. Falls back to the geometric normal when
  // absent. Interpolation inferred from size (vertex / faceVarying).
  std::vector<float> nrm;
  int nrm_mode = 0;  // 0=none, 1=vertex, 2=faceVarying
  // Displaced meshes shade with the geometric normal of the deformed surface, so
  // authored (pre-displacement) normals are ignored -- they no longer match.
  if (want_normals && !do_displace) {
    nrm = ReadFloatArrayLazy(prim, "normals", time);
    if (nrm.empty()) nrm = ReadFloatArrayLazy(prim, "primvars:normals", time);
    const size_t nn = nrm.size() / 3;
    if (nn == npts) nrm_mode = 1;
    else if (nn == indices.size()) nrm_mode = 2;
  }
  // World-space normal for face-vertex slot `fv` (point index `pi`); `geom` is the
  // face's geometric normal (already in the job frame) used as the fallback.
  auto norm_at = [&](size_t fv, int32_t pi, const Vec3 &geom) -> Vec3 {
    if (!nrm_mode) return geom;
    size_t ni = nrm_mode == 1 ? size_t(pi) : fv;
    if (ni * 3 + 2 >= nrm.size()) return geom;
    Vec3 ln{nrm[ni * 3 + 0], nrm[ni * 3 + 1], nrm[ni * 3 + 2]};
    Vec3 wn = TransformVector(world, ln);  // job-frame (world for flat path)
    float len = Length(wn);
    return len > 1.0e-12f ? Mul(wn, 1.0f / len) : geom;
  };

  // Reserve from the exact triangle-fan estimate. StreamMeshJobs gives each mesh
  // its OWN thread-local buffers (one mesh's worth), so a single up-front reserve
  // replaces the per-triangle geometric reallocations (the TriInfo realloc churn
  // perf flagged). (This is safe ONLY because the buffers are per-job now; the
  // old shared-buffer design would have reallocated multi-GB on every mesh.)
  size_t tri_estimate = 0;
  for (int32_t c : counts) {
    if (c >= 3) tri_estimate += size_t(c - 2);
  }
  if (tri_estimate) {
    if (out_uverts) {
      out_indices->reserve(out_indices->size() + tri_estimate * 3);
    } else {
      vertices->reserve(vertices->size() + tri_estimate * 9);
    }
    tris->reserve(tris->size() + tri_estimate);
    if (want_uvs) tri_uvs->reserve(tri_uvs->size() + tri_estimate * 6);
    if (want_colors) tri_colors->reserve(tri_colors->size() + tri_estimate * 12);
    if (want_normals) tri_normals->reserve(tri_normals->size() + tri_estimate * 9);
  }
  // Indexed path: append this mesh's unique world-space vertices once; triangle
  // indices below are offset by the BLAS-local base. The soup path leaves these
  // untouched. All npts are appended (index space == point ids) even if some are
  // unreferenced -- simpler base arithmetic, negligible waste.
  const uint32_t vbase = (out_uverts && io_vbase) ? *io_vbase : 0u;
  if (out_uverts) {
    out_uverts->reserve(out_uverts->size() + npts * 3);
    for (size_t i = 0; i < npts; i++) {
      out_uverts->push_back(wpts[i].x);
      out_uverts->push_back(wpts[i].y);
      out_uverts->push_back(wpts[i].z);
    }
    if (io_vbase) *io_vbase += uint32_t(npts);
  }

  // Material + purpose are constant across a mesh's triangles, so resolve them
  // ONCE here instead of rebuilding a full TriInfo per triangle. The slim TLAS
  // path (TriStore) stores the material once in mat_table and needs none of these
  // per-tri; the flat path copies this template and overwrites only the positions.
  const uint32_t purpose_bit = PurposeBit(purpose);
  const bool visible_for_fit = PurposeVisible(purpose_bit, purpose_mask);
  // USD doubleSided (schema default false = single-sided -> back-face cull).
  bool double_sided = false;
  if (const lightusd::next::Value *v = prim.GetPropertyValue("doubleSided"))
    if (const bool *b = v->as_bool()) double_sided = *b;
  const uint8_t ds_flag = double_sided ? 1 : 0;

  TriInfo tmpl;
  tmpl.double_sided = ds_flag;
  tmpl.base_color = base_color;
  tmpl.tex_id = tex_id;
  tmpl.normal_tex_id = normal_tex_id;
  tmpl.roughness = roughness;
  tmpl.metallic = metallic;
  tmpl.rough_tex_id = rough_tex.id;
  tmpl.rough_ch = rough_tex.ch;
  tmpl.rough_tex_scale = rough_tex.scale;
  tmpl.rough_tex_bias = rough_tex.bias;
  tmpl.metal_tex_id = metal_tex.id;
  tmpl.metal_ch = metal_tex.ch;
  tmpl.metal_tex_scale = metal_tex.scale;
  tmpl.metal_tex_bias = metal_tex.bias;
  tmpl.emission = mesh_emission;
  tmpl.emission_tex_id = emission_tex_id;
  tmpl.area_light = mesh_area_light;
  tmpl.occlusion = occlusion;
  tmpl.occ_tex_id = occ_tex.id;
  tmpl.occ_ch = occ_tex.ch;
  tmpl.occ_tex_scale = occ_tex.scale;
  tmpl.occ_tex_bias = occ_tex.bias;
  tmpl.opacity = opacity;
  tmpl.opacity_tex_id = opacity_tex.id;
  tmpl.opacity_ch = opacity_tex.ch;
  tmpl.opacity_tex_scale = opacity_tex.scale;
  tmpl.opacity_tex_bias = opacity_tex.bias;
  tmpl.opacity_threshold = opacity_threshold;
  tmpl.clearcoat = clearcoat;
  tmpl.clearcoat_roughness = clearcoat_roughness;
  tmpl.clearcoat_tex_id = clearcoat_tex.id;
  tmpl.clearcoat_ch = clearcoat_tex.ch;
  tmpl.clearcoat_tex_scale = clearcoat_tex.scale;
  tmpl.clearcoat_tex_bias = clearcoat_tex.bias;
  tmpl.clearcoat_rough_tex_id = clearcoat_rough_tex.id;
  tmpl.clearcoat_rough_ch = clearcoat_rough_tex.ch;
  tmpl.clearcoat_rough_tex_scale = clearcoat_rough_tex.scale;
  tmpl.clearcoat_rough_tex_bias = clearcoat_rough_tex.bias;
  tmpl.specular_color = specular_color;
  tmpl.specular_tex_id = specular_tex_id;
  tmpl.ior = ior;
  tmpl.use_specular_workflow = use_specular_workflow;
  tmpl.purpose_bit = purpose_bit;

  const bool have_mask = face_mask && !face_mask->empty();
  size_t cursor = 0;
  size_t face_idx = 0;
  for (int32_t c : counts) {
    const size_t face = face_idx++;
    if (c < 3 || cursor + size_t(c) > indices.size()) {
      cursor += size_t(std::max<int32_t>(0, c));
      continue;
    }
    // GeomSubset job split: this job emits only its own faces.
    if (have_mask && (face >= face_mask->size() || !(*face_mask)[face])) {
      cursor += size_t(c);
      continue;
    }
    for (int32_t k = 1; k + 1 < c; k++) {
      int32_t i0 = indices[cursor + 0];
      int32_t i1 = indices[cursor + size_t(k)];
      int32_t i2 = indices[cursor + size_t(k + 1)];
      if (i0 < 0 || i1 < 0 || i2 < 0 || size_t(i0) >= npts ||
          size_t(i1) >= npts || size_t(i2) >= npts) {
        continue;
      }
      const Vec3 &p0 = wpts[size_t(i0)];
      const Vec3 &p1 = wpts[size_t(i1)];
      const Vec3 &p2 = wpts[size_t(i2)];
      Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
      if (Length(n) < 1.0e-12f) continue;

      if (purpose_bit == kPurposeRenderBit) {
        stats->purpose_render_triangles++;
      } else if (purpose_bit == kPurposeProxyBit) {
        stats->purpose_proxy_triangles++;
      } else if (purpose_bit == kPurposeGuideBit) {
        stats->purpose_guide_triangles++;
      } else {
        stats->purpose_default_triangles++;
      }
      // TLAS mode culls purpose-invisible triangles at build time (closest-hit
      // can't filter per-prim like the flat multi-hit path does).
      if (purpose_cull && !visible_for_fit) continue;
      if (out_indices) {
        out_indices->push_back(vbase + uint32_t(i0));
        out_indices->push_back(vbase + uint32_t(i1));
        out_indices->push_back(vbase + uint32_t(i2));
      } else {
        vertices->insert(vertices->end(),
                         {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z});
      }
      if constexpr (std::is_same<typename TVec::value_type, TriStore>::value) {
        // Slim store: only mat_id (positions are in `vertices` above; the global
        // mat_id is assigned by StreamMeshJobs when it concatenates jobs).
        TriStore ts;
        ts.mat_id = 0;
        tris->push_back(ts);
      } else if constexpr (std::is_same<typename TVec::value_type,
                                        FlatTri>::value) {
        // Flat slim store: per-tri geometry + purpose + a mat_id into the flat
        // material table (assigned at concat by StreamMeshJobs, like TriStore).
        FlatTri ft;
        ft.p0 = p0;
        ft.p1 = p1;
        ft.p2 = p2;
        ft.n = n;
        ft.purpose_bit = purpose_bit;
        ft.mat_id = 0;
        ft.double_sided = ds_flag;
        tris->push_back(ft);
      } else {
        TriInfo tri = tmpl;  // per-mesh material; per-tri geometry below
        tri.p0 = p0;
        tri.p1 = p1;
        tri.p2 = p2;
        tri.n = n;
        tris->push_back(tri);
      }
      if (want_uvs) {
        // Keep tri_uvs parallel to tris (6 floats/tri). uv0=vert0, uv1=vert(k),
        // uv2=vert(k+1) in fan order, matching i0/i1/i2 above.
        auto uv0 = uv_at(cursor + 0, i0);
        auto uv1 = uv_at(cursor + size_t(k), i1);
        auto uv2 = uv_at(cursor + size_t(k + 1), i2);
        // Bake the UsdTransform2d (if any) into the stored UVs.
        uv_xform.apply(&uv0.first, &uv0.second);
        uv_xform.apply(&uv1.first, &uv1.second);
        uv_xform.apply(&uv2.first, &uv2.second);
        tri_uvs->insert(tri_uvs->end(), {uv0.first, uv0.second, uv1.first,
                                         uv1.second, uv2.first, uv2.second});
      }
      if (want_colors) {
        // Per-corner RGBA8 parallel to tris (12 bytes/tri), fan order matching
        // i0/i1/i2. Constant meshes replicate base_color/opacity at all corners.
        float c0[4], c1[4], c2[4];
        col_at(cursor + 0, i0, face, c0);
        col_at(cursor + size_t(k), i1, face, c1);
        col_at(cursor + size_t(k + 1), i2, face, c2);
        auto q = [](float x) -> uint8_t {
          x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
          return uint8_t(int(x * 255.0f + 0.5f));
        };
        tri_colors->insert(tri_colors->end(),
                           {q(c0[0]), q(c0[1]), q(c0[2]), q(c0[3]), q(c1[0]),
                            q(c1[1]), q(c1[2]), q(c1[3]), q(c2[0]), q(c2[1]),
                            q(c2[2]), q(c2[3])});
      }
      if (want_normals) {
        // Per-corner normals (9 floats/tri), fan order matching i0/i1/i2.
        Vec3 n0 = norm_at(cursor + 0, i0, n);
        Vec3 n1 = norm_at(cursor + size_t(k), i1, n);
        Vec3 n2 = norm_at(cursor + size_t(k + 1), i2, n);
        tri_normals->insert(tri_normals->end(),
                            {n0.x, n0.y, n0.z, n1.x, n1.y, n1.z, n2.x, n2.y,
                             n2.z});
      }
      stats->triangles++;
      if (visible_for_fit) {
        Expand(bounds, p0);
        Expand(bounds, p1);
        Expand(bounds, p2);
      }
    }
    cursor += size_t(c);
  }
}


// ---------------------------------------------------------------------------
// Material/texture resolution for the `next` render path.
//
// Resolves a Mesh's bound material (material:binding -> Material ->
// outputs:surface -> UsdPreviewSurface, including MaterialX's
// ND_UsdPreviewSurface_surfaceshader) into a flat diffuse base color and/or a
// diffuse (base color) texture sampled through inputs:diffuseColor ->
// UsdUVTexture(inputs:file). Done serially before the parallel triangle stream.
// ---------------------------------------------------------------------------

// Directory portion of a path (without trailing slash), or "" if none.
std::string DirName(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return "";
  return path.substr(0, slash);
}

// Loaded RGB(A) textures + a key->index cache so each (file, wrap, colorspace)
// loads once. `usdz`, when set, is searched first so textures packed inside a
// .usdz archive resolve without touching the filesystem.

bool IsUdimPattern(const std::string &asset) {
  return asset.find("<UDIM>") != std::string::npos;
}

// Lazily build the shared decoder: asset resolution (filesystem / .usdz entry),
// 8-bit normalization, and the -texMaxSize / -texBudgetMb shrink all live in
// tydra::next::TextureDecoder now, shared with lusdview. `force_rgba` stays off:
// the CPU integrator samples the source channel count, so a synthetic alpha
// channel would be a third more memory for nothing.
lightusd::tydra::next::TextureDecoder &DecoderFor(TextureCache &tc) {
  if (!tc.decoder) {
    lightusd::tydra::next::TextureDecodeOptions opts;
    opts.base_dir = tc.base_dir;
    opts.usdz = tc.usdz;
    opts.force_rgba = false;
    if (tc.options) {
      opts.max_edge = tc.options->texture_max_size > 0
                          ? uint32_t(tc.options->texture_max_size)
                          : 0u;
      opts.budget_bytes = tc.options->texture_budget_mb > 0
                              ? uint64_t(tc.options->texture_budget_mb) *
                                    1024ull * 1024ull
                              : 0ull;
    }
    tc.decoder =
        std::make_shared<lightusd::tydra::next::TextureDecoder>(std::move(opts));
  }
  return *tc.decoder;
}

// Anchor a RAW authored asset path to the layer that authored it. The hand-rolled
// resolver below reads `inputs:file` straight off the shader prim, so it gets the
// authored string -- which in a look layer nested below the root is relative to
// THAT layer (`../../texture/foo.png`) and does not resolve against the scene
// file. Prims carry their authoring layer's directory through composition; see
// next/layer/asset-anchor.hh. Prims with no anchor (root layer, USDZ entries)
// return the path untouched, preserving the previous behavior.
std::string AnchorAssetNext(const lightusd::next::UsdPrim &prim,
                            const std::string &path) {
  if (path.empty() || lightusd::next::AssetResolver::IsAbsolutePath(path) ||
      path.find("://") != std::string::npos) {
    return path;
  }
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  const uint32_t id = spec ? spec->asset_anchor_id() : 0u;
  const std::string &dir = lightusd::next::AssetAnchorPath(id);
  if (dir.empty()) return path;
  return lightusd::next::AssetResolver::NormalizePath(
      lightusd::next::AssetResolver::JoinPath(dir, path));
}

int32_t LoadTextureCached(TextureCache &tc, const std::string &asset_path,
                          WrapMode ws, WrapMode wt, bool srgb,
                          const Vec3 &scale = Vec3{1.0f, 1.0f, 1.0f},
                          const Vec3 &bias = Vec3{0.0f, 0.0f, 0.0f},
                          const lightusd::color::ColorTransform *color_transform =
                              nullptr) {
  // Key on ALL scale/bias channels: keying only .x collided two materials
  // sharing a file with equal red-scale but different green/blue scale, so the
  // second silently reused the first's tint.
  std::string key =
      asset_path + "|" + std::to_string(int(ws)) + "," +
      std::to_string(int(wt)) + (srgb ? "|s" : "|r") + "|" +
      std::to_string(scale.x) + "," + std::to_string(scale.y) + "," +
      std::to_string(scale.z) + "|" + std::to_string(bias.x) + "," +
      std::to_string(bias.y) + "," + std::to_string(bias.z) + "|" +
      (color_transform ? color_transform->source.name : std::string()) + ">" +
      (color_transform ? color_transform->destination.name : std::string());
  if (color_transform) {
    const auto append_float_bits = [&](float value) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      key += "," + std::to_string(bits);
    };
    key += "|" +
           std::to_string(static_cast<int>(color_transform->source.kind)) +
           "," + (color_transform->bypass ? "1" : "0");
    append_float_bits(color_transform->source.gamma);
    append_float_bits(color_transform->source.linear_bias);
    for (float coefficient : color_transform->matrix) {
      append_float_bits(coefficient);
    }
  }
  auto it = tc.by_key.find(key);
  if (it != tc.by_key.end()) return it->second;

  // Decode + shrink through the shared decoder, then wrap it in lusdrender's
  // Texture (sampler state + mip chain).
  auto make_texture = [&](const std::string &asset, const std::string &label,
                          Texture *out) -> bool {
    if (!out) return false;
    lightusd::tydra::next::DecodedImage img;
    if (!DecoderFor(tc).Decode(asset, srgb, &img)) return false;
    if (tc.options &&
        tc.options->texture_compress == Options::TextureCompress::BCn) {
      std::cerr << "WARN: -texCompress bc requested; " << label
                << " is currently kept as resized 8-bit texels in lusdrender\n";
    }
    Texture t;
    t.width = int(img.width);
    t.height = int(img.height);
    t.channels = int(img.channels);
    if (img.hdr) {
      // lusdrender's CPU sampler and mip chain store 8-bit texels. The shared
      // decoder retains EXR/HDR inputs as linear float RGB, so moving `pixels`
      // here used to leave a non-zero-sized Texture with a null mip source and
      // crash in build_mips(). Preserve the renderer's existing storage model
      // by quantizing the decoded source-space values; the authored color
      // transform is still applied later by Texture::sample_data().
      const size_t texel_count = size_t(t.width) * size_t(t.height);
      t.channels = 3;
      t.pixels.resize(texel_count * 3u);
      for (size_t i = 0; i < texel_count * 3u; ++i) {
        const float value = i < img.float_pixels.size() &&
                                    std::isfinite(img.float_pixels[i])
                                ? img.float_pixels[i]
                                : 0.0f;
        t.pixels[i] = static_cast<uint8_t>(
            std::lround(std::max(0.0f, std::min(value, 1.0f)) * 255.0f));
      }
    } else {
      t.pixels = std::move(img.pixels);
    }
    t.wrap_s = ws;
    t.wrap_t = wt;
    t.srgb = srgb;
    if (color_transform) t.color_transform = *color_transform;
    t.scale = scale;
    t.bias = bias;
    const std::shared_ptr<lightusd::next::TextureBudgetState> budget_state =
        img.budget_lease ? img.budget_lease->state : nullptr;
    t.budget_leases.push_back(std::move(img.budget_lease));
    size_t mip_bytes = 0;
    int mip_w = t.width;
    int mip_h = t.height;
    while (mip_w > 1 || mip_h > 1) {
      mip_w = std::max(1, mip_w / 2);
      mip_h = std::max(1, mip_h / 2);
      const size_t level_bytes = size_t(mip_w) * size_t(mip_h) *
                                 size_t(std::max(0, t.channels));
      if (level_bytes > (std::numeric_limits<size_t>::max)() - mip_bytes) {
        mip_bytes = (std::numeric_limits<size_t>::max)();
        break;
      }
      mip_bytes += level_bytes;
    }
    bool build_mips = true;
    if (budget_state && mip_bytes != 0) {
      build_mips = budget_state->try_add(static_cast<uint64_t>(mip_bytes));
      if (build_mips) {
        t.budget_leases.push_back(std::make_shared<
            lightusd::next::TextureBudgetLease>(budget_state, mip_bytes));
      } else if (tc.texture_mip_fallbacks) {
        ++(*tc.texture_mip_fallbacks);
      }
    }
    if (build_mips) t.build_mips();
    tc.decoded_bytes = size_t(DecoderFor(tc).decoded_bytes());
    *out = std::move(t);
    return true;
  };

  auto adopt = [&](const std::string &asset, const std::string &label) -> int32_t {
    Texture t;
    if (!make_texture(asset, label, &t)) return -1;
    int32_t id = int32_t(tc.textures->size());
    tc.textures->push_back(std::move(t));
    return id;
  };

  int32_t id = -1;
  if (IsUdimPattern(asset_path)) {
    Texture udim;
    udim.is_udim = true;
    udim.wrap_s = ws;
    udim.wrap_t = wt;
    udim.srgb = srgb;
    if (color_transform) udim.color_transform = *color_transform;
    udim.scale = scale;
    udim.bias = bias;
    for (int tile_id = 1001; tile_id <= 1100; ++tile_id) {
      const std::string tile_path =
          lightusd::tydra::next::ReplaceUdimToken(asset_path, tile_id);
      Texture tile_tex;
      if (!make_texture(tile_path, tile_path, &tile_tex)) continue;
      Texture::UdimTile tile;
      tile.udim = tile_id;
      tile.width = tile_tex.width;
      tile.height = tile_tex.height;
      tile.channels = tile_tex.channels;
      tile.pixels = std::move(tile_tex.pixels);
      tile.mips = std::move(tile_tex.mips);
      udim.budget_leases.insert(
          udim.budget_leases.end(),
          std::make_move_iterator(tile_tex.budget_leases.begin()),
          std::make_move_iterator(tile_tex.budget_leases.end()));
      udim.udim_tiles.push_back(std::move(tile));
    }
    if (!udim.udim_tiles.empty()) {
      id = int32_t(tc.textures->size());
      tc.textures->push_back(std::move(udim));
    }
  } else {
    id = adopt(asset_path, asset_path);
  }
  if (id < 0) {
    if (tc.missing_textures) (*tc.missing_textures)++;
    std::cerr << "WARN: failed to load texture: " << asset_path << "\n";
  }
  tc.by_key[key] = id;
  return id;
}

size_t TextureResidentBytes(const Texture &texture) {
  size_t bytes = texture.pixels.capacity();
  for (const Texture::Mip &mip : texture.mips) bytes += mip.data.capacity();
  for (const Texture::UdimTile &tile : texture.udim_tiles) {
    bytes += tile.pixels.capacity();
    for (const Texture::Mip &mip : tile.mips)
      bytes += mip.data.capacity();
  }
  return bytes;
}

void UpdateTextureStats(const std::vector<Texture> &textures,
                        RTPreviewStats *stats) {
  if (!stats) return;
  stats->texture_count = textures.size();
  uint64_t bytes = 0;
  for (const Texture &texture : textures) {
    const uint64_t current = static_cast<uint64_t>(TextureResidentBytes(texture));
    if (current > (std::numeric_limits<uint64_t>::max)() - bytes) {
      bytes = (std::numeric_limits<uint64_t>::max)();
      break;
    }
    bytes += current;
  }
  stats->texture_resident_bytes = bytes;
}

// Follow a connection on `prim` (e.g. "outputs:surface",
// "inputs:diffuseColor") to its target prim, or an invalid prim if unconnected.
lightusd::next::UsdPrim ConnectedPrimNext(const lightusd::next::Stage &stage,
                                          const lightusd::next::UsdPrim &prim,
                                          const std::string &prop) {
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  if (!spec) return lightusd::next::UsdPrim();
  const std::vector<lightusd::next::Path> *c = spec->connection(prop);
  if (!c || c->empty()) return lightusd::next::UsdPrim();
  return stage.GetPrimAtPath((*c)[0].prim_path());
}

bool HasConnectionNext(const lightusd::next::UsdPrim &prim,
                       const std::string &prop) {
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  if (!spec) return false;
  const std::vector<lightusd::next::Path> *c = spec->connection(prop);
  return c && !c->empty();
}

WrapMode ParseWrapMode(const std::string &s) {
  if (s == "clamp") return WrapMode::Clamp;
  if (s == "mirror") return WrapMode::Mirror;
  if (s == "black") return WrapMode::Black;
  return WrapMode::Repeat;  // "repeat"/"useMetadata"/default
}

bool ReadVec3Value(const lightusd::next::Value &value, Vec3 *out) {
  if (!out) return false;
  if (const float *f = value.as_float3()) {
    *out = Vec3{f[0], f[1], f[2]};
    return true;
  }
  if (const float *f = value.as_float4()) {
    *out = Vec3{f[0], f[1], f[2]};
    return true;
  }
  if (const float *f = value.as_float()) {
    *out = Vec3{*f, *f, *f};
    return true;
  }
  return false;
}

// Resolve a scalar PBR input (inputs:roughness / inputs:metallic) that connects
// to a UsdUVTexture outputs:{r,g,b,a} (e.g. ORM packing). Loads the raw texture
// and records the source channel.
void ResolveScalarTextureNext(const lightusd::next::Stage &stage,
                              const lightusd::next::UsdPrim &surf,
                              const std::string &input, TextureCache &tc,
                              ScalarTex *out) {
  const lightusd::next::PrimSpec *spec = surf.GetPrimSpec();
  if (!spec) return;
  const std::vector<lightusd::next::Path> *c = spec->connection(input);
  if (!c || c->empty()) return;
  const lightusd::next::Path &target = (*c)[0];
  lightusd::next::UsdPrim tex = stage.GetPrimAtPath(target.prim_path());
  if (!tex.IsValid()) return;
  const lightusd::next::Value *fv = tex.GetPropertyValue("inputs:file");
  if (!fv) return;
  const std::string *ap = fv->as_asset_path();
  if (!ap) ap = fv->as_string();
  if (!ap || ap->empty()) return;
  WrapMode ws = WrapMode::Repeat, wt = WrapMode::Repeat;
  if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:wrapS"))
    if (const std::string *t = v->as_token()) ws = ParseWrapMode(*t);
  if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:wrapT"))
    if (const std::string *t = v->as_token()) wt = ParseWrapMode(*t);
  int32_t id = LoadTextureCached(tc, AnchorAssetNext(tex, *ap), ws, wt, /*srgb=*/false);
  if (id < 0) return;
  out->id = id;
  const std::string prop = target.property_name();  // e.g. "outputs:g"
  if (!prop.empty()) {
    switch (prop.back()) {
      case 'g': out->ch = 1; break;
      case 'b': out->ch = 2; break;
      case 'a': out->ch = 3; break;
      default: out->ch = 0; break;  // r / rgb
    }
  }
  // UsdUVTexture inputs:scale / inputs:bias for the sampled channel (float4 or
  // scalar). out = raw*scale + bias. Stored for all scalar inputs; the caller
  // applies it only for displacement.
  if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:scale")) {
    if (const float *f = v->as_float4()) out->scale = f[std::min<int>(out->ch, 3)];
    else if (const float *s = v->as_float()) out->scale = *s;
  }
  if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:bias")) {
    if (const float *f = v->as_float4()) out->bias = f[std::min<int>(out->ch, 3)];
    else if (const float *s = v->as_float()) out->bias = *s;
  }
}

// If a UsdUVTexture's inputs:st chain runs through a UsdTransform2d, read its
// rotation (deg, CCW) / scale / translation. Otherwise returns identity.
UvXform ResolveUvXform(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &uvtex) {
  UvXform x;
  lightusd::next::UsdPrim st = ConnectedPrimNext(stage, uvtex, "inputs:st");
  if (!st.IsValid()) return x;
  const lightusd::next::Value *idv = st.GetPropertyValue("info:id");
  const std::string *id = idv ? idv->as_token() : nullptr;
  if (!id || *id != "UsdTransform2d") return x;
  float rot = 0.0f, sx = 1.0f, sy = 1.0f, tx = 0.0f, ty = 0.0f;
  if (const lightusd::next::Value *v = st.GetPropertyValue("inputs:rotation"))
    if (const float *f = v->as_float()) rot = *f;
  if (const lightusd::next::Value *v = st.GetPropertyValue("inputs:scale"))
    if (const float *f = v->as_float2()) { sx = f[0]; sy = f[1]; }
  if (const lightusd::next::Value *v = st.GetPropertyValue("inputs:translation"))
    if (const float *f = v->as_float2()) { tx = f[0]; ty = f[1]; }
  if (rot == 0.0f && sx == 1.0f && sy == 1.0f && tx == 0.0f && ty == 0.0f) {
    return x;  // identity
  }
  float rad = rot * 3.14159265358979f / 180.0f;
  x.rc = std::cos(rad);
  x.rs = std::sin(rad);
  x.sx = sx;
  x.sy = sy;
  x.tx = tx;
  x.ty = ty;
  x.identity = false;
  return x;
}

void ResolveMeshMaterialNext(const lightusd::next::Stage &stage,
                             const lightusd::next::UsdPrim &mesh,
                             TextureCache &tc, Vec3 *base_color, int32_t *tex_id,
                             float *roughness, float *metallic,
                             int32_t *normal_tex_id, UvXform *uv_xform,
                             ScalarTex *rough_tex, ScalarTex *metal_tex,
                             Vec3 *emission, int32_t *emission_tex_id,
                             float *occlusion, ScalarTex *occ_tex,
                             float *opacity, ScalarTex *opacity_tex,
                             float *opacity_threshold, float *clearcoat,
                             float *clearcoat_roughness, ScalarTex *clearcoat_tex,
                             ScalarTex *clearcoat_rough_tex, Vec3 *specular_color,
                             int32_t *specular_tex_id, float *ior,
                             uint8_t *use_specular_workflow,
                             bool *vertex_color, float *displacement,
                             ScalarTex *displacement_tex) {
  // Geometry display primvars: the unmaterialed base color/opacity (e.g. ALab
  // geom-only meshes). The first value seeds the constant base; a >1-element
  // array is per-vertex/faceVarying/uniform and sets *vertex_color so the stream
  // stores per-corner colors (see AddRTPreviewMeshNext). A bound material below
  // overrides the constant color.
  constexpr double kDefaultTime = std::numeric_limits<double>::quiet_NaN();
  lightusd::tydra::next::ValueArrayRead<float> dc;
  if (lightusd::tydra::next::ReadFloatArray(
          mesh, "primvars:displayColor", kDefaultTime, &dc)) {
    if (dc.size() >= 3) *base_color = Vec3{dc[0], dc[1], dc[2]};
    if (vertex_color && dc.size() > 3) *vertex_color = true;
  }
  if (opacity) {
    lightusd::tydra::next::ValueArrayRead<float> od;
    if (lightusd::tydra::next::ReadFloatArray(
            mesh, "primvars:displayOpacity", kDefaultTime, &od)) {
      if (!od.empty()) *opacity = std::min(1.0f, std::max(0.0f, od[0]));
      if (vertex_color && od.size() > 1) *vertex_color = true;
    }
  }
  const std::string bindPath =
      lightusd::next::GetInheritedBoundMaterialPath(stage, mesh.GetPath().str());
  if (bindPath.empty()) return;
  lightusd::next::UsdPrim mat = stage.GetPrimAtPath(bindPath);
  if (!mat.IsValid()) return;
  lightusd::next::UsdPrim surf = ConnectedPrimNext(stage, mat, "outputs:surface");
  if (!surf.IsValid()) {
    surf = ConnectedPrimNext(stage, mat, "outputs:mtlx:surface");
  }
  if (!surf.IsValid()) return;
  if (base_color) {
    *base_color = Vec3{0.18f, 0.18f, 0.18f};  // UsdPreviewSurface default.
  }
  if (roughness) *roughness = 0.5f;  // UsdPreviewSurface shader default.

  // Scalar PBR params (UsdPreviewSurface inputs:roughness / inputs:metallic).
  if (const lightusd::next::Value *r = surf.GetPropertyValue("inputs:roughness")) {
    if (const float *f = r->as_float()) *roughness = std::min(1.0f, std::max(0.0f, *f));
  }
  if (const lightusd::next::Value *m = surf.GetPropertyValue("inputs:metallic")) {
    if (const float *f = m->as_float()) *metallic = std::min(1.0f, std::max(0.0f, *f));
  }
  // Roughness/metallic textures (channel-aware; ORM packing).
  if (rough_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:roughness", tc, rough_tex);
  if (metal_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:metallic", tc, metal_tex);

  // Occlusion (AO) scalar + optional texture.
  if (const lightusd::next::Value *o = surf.GetPropertyValue("inputs:occlusion"))
    if (const float *f = o->as_float())
      if (occlusion) *occlusion = std::min(1.0f, std::max(0.0f, *f));
  if (occ_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:occlusion", tc, occ_tex);

  // Displacement: UsdPreviewSurface inputs:displacement offsets the surface along
  // its normal (scene units). A scalar constant and/or a channel-aware height
  // texture (raw, like roughness/opacity). Applied per-vertex at mesh-build time
  // (coarse displacement) -- see AddRTPreviewMeshNext.
  if (const lightusd::next::Value *d =
          surf.GetPropertyValue("inputs:displacement"))
    if (const float *f = d->as_float())
      if (displacement) *displacement = *f;
  if (displacement_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:displacement", tc,
                             displacement_tex);

  // Transparency: UsdPreviewSurface inputs:opacity (scalar const, multiplied into
  // any displayOpacity) + optional channel-aware texture (commonly the diffuse
  // map's alpha, outputs:a). inputs:opacityThreshold > 0 turns it into an alpha
  // cutout (mask) instead of translucent blending.
  const bool opacity_connected = HasConnectionNext(surf, "inputs:opacity");
  if (const lightusd::next::Value *o = surf.GetPropertyValue("inputs:opacity"))
    if (const float *f = o->as_float())
      if (opacity && !opacity_connected)
        *opacity *= std::min(1.0f, std::max(0.0f, *f));
  if (opacity_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:opacity", tc, opacity_tex);
  if (const lightusd::next::Value *ot =
          surf.GetPropertyValue("inputs:opacityThreshold"))
    if (const float *f = ot->as_float())
      if (opacity_threshold) *opacity_threshold = std::max(0.0f, *f);

  // Clearcoat: a second specular lobe (weight + roughness), each a scalar const
  // plus optional channel-aware texture. Only contributes under IBL (like base
  // roughness/metallic).
  if (const lightusd::next::Value *c = surf.GetPropertyValue("inputs:clearcoat"))
    if (const float *f = c->as_float())
      if (clearcoat) *clearcoat = std::min(1.0f, std::max(0.0f, *f));
  if (const lightusd::next::Value *cr =
          surf.GetPropertyValue("inputs:clearcoatRoughness"))
    if (const float *f = cr->as_float())
      if (clearcoat_roughness)
        *clearcoat_roughness = std::min(1.0f, std::max(0.0f, *f));
  if (clearcoat_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:clearcoat", tc, clearcoat_tex);
  if (clearcoat_rough_tex)
    ResolveScalarTextureNext(stage, surf, "inputs:clearcoatRoughness", tc,
                             clearcoat_rough_tex);

  // Specular workflow: inputs:useSpecularWorkflow swaps the metallic F0 derivation
  // for an explicit inputs:specularColor (constant + optional color texture).
  // inputs:ior sets the dielectric F0 in the (default) metallic workflow; ior 1.5
  // reproduces the fixed 0.04 used before, so the default is byte-identical.
  if (const lightusd::next::Value *uw =
          surf.GetPropertyValue("inputs:useSpecularWorkflow")) {
    if (const int32_t *i = uw->as_int())
      if (use_specular_workflow) *use_specular_workflow = (*i != 0) ? 1 : 0;
  }
  if (const lightusd::next::Value *iv = surf.GetPropertyValue("inputs:ior"))
    if (const float *f = iv->as_float())
      if (ior && *f > 0.0f) *ior = *f;
  if (const lightusd::next::Value *sc =
          surf.GetPropertyValue("inputs:specularColor"))
    if (const float *f = sc->as_float3())
      if (specular_color) *specular_color = Vec3{f[0], f[1], f[2]};
  if (specular_tex_id) {
    lightusd::next::UsdPrim stex =
        ConnectedPrimNext(stage, surf, "inputs:specularColor");
    if (stex.IsValid()) {
      if (const lightusd::next::Value *fv =
              stex.GetPropertyValue("inputs:file")) {
        const std::string *ap = fv->as_asset_path();
        if (!ap) ap = fv->as_string();
        if (ap && !ap->empty()) {
          WrapMode ws = WrapMode::Repeat, wt = WrapMode::Repeat;
          if (const lightusd::next::Value *v = stex.GetPropertyValue("inputs:wrapS"))
            if (const std::string *t = v->as_token()) ws = ParseWrapMode(*t);
          if (const lightusd::next::Value *v = stex.GetPropertyValue("inputs:wrapT"))
            if (const std::string *t = v->as_token()) wt = ParseWrapMode(*t);
          bool srgb = true;
          if (const lightusd::next::Value *v =
                  stex.GetPropertyValue("inputs:sourceColorSpace"))
            if (const std::string *t = v->as_token()) srgb = (*t != "raw");
          int32_t id = LoadTextureCached(tc, AnchorAssetNext(stex, *ap), ws, wt, srgb);
          if (id >= 0) {
            *specular_tex_id = id;
            if (specular_color) *specular_color = Vec3{1.0f, 1.0f, 1.0f};
          }
        }
      }
    }
  }

  // Emissive color: constant + optional UsdUVTexture (sRGB color).
  if (const lightusd::next::Value *e =
          surf.GetPropertyValue("inputs:emissiveColor"))
    if (const float *f = e->as_float3())
      if (emission) *emission = Vec3{f[0], f[1], f[2]};
  if (emission_tex_id) {
    lightusd::next::UsdPrim etex =
        ConnectedPrimNext(stage, surf, "inputs:emissiveColor");
    if (etex.IsValid()) {
      if (const lightusd::next::Value *fv =
              etex.GetPropertyValue("inputs:file")) {
        const std::string *ap = fv->as_asset_path();
        if (!ap) ap = fv->as_string();
        if (ap && !ap->empty()) {
          WrapMode ws = WrapMode::Repeat, wt = WrapMode::Repeat;
          if (const lightusd::next::Value *v = etex.GetPropertyValue("inputs:wrapS"))
            if (const std::string *t = v->as_token()) ws = ParseWrapMode(*t);
          if (const lightusd::next::Value *v = etex.GetPropertyValue("inputs:wrapT"))
            if (const std::string *t = v->as_token()) wt = ParseWrapMode(*t);
          int32_t id = LoadTextureCached(tc, AnchorAssetNext(etex, *ap), ws, wt, /*srgb=*/true);
          if (id >= 0) {
            *emission_tex_id = id;
            if (emission) *emission = Vec3{1.0f, 1.0f, 1.0f};  // texture is the tint
          }
        }
      }
    }
  }

  // Constant diffuse color (also the texture's fallback tint).
  if (const lightusd::next::Value *diffuseColorVal =
          surf.GetPropertyValue("inputs:diffuseColor")) {
    if (const float *f = diffuseColorVal->as_float3()) {
      *base_color = Vec3{f[0], f[1], f[2]};
    }
  }
  // Diffuse texture: inputs:diffuseColor -> UsdUVTexture(inputs:file), honoring
  // its wrapS/wrapT and sourceColorSpace (sRGB by default for color).
  lightusd::next::UsdPrim tex =
      ConnectedPrimNext(stage, surf, "inputs:diffuseColor");
  if (tex.IsValid()) {
    if (const lightusd::next::Value *fv = tex.GetPropertyValue("inputs:file")) {
      const std::string *ap = fv->as_asset_path();
      if (!ap) ap = fv->as_string();
      if (ap && !ap->empty()) {
        WrapMode ws = WrapMode::Repeat, wt = WrapMode::Repeat;
        if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:wrapS"))
          if (const std::string *t = v->as_token()) ws = ParseWrapMode(*t);
        if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:wrapT"))
          if (const std::string *t = v->as_token()) wt = ParseWrapMode(*t);
        bool srgb = true;
        if (const lightusd::next::Value *v =
                tex.GetPropertyValue("inputs:sourceColorSpace")) {
          if (const std::string *t = v->as_token()) srgb = (*t != "raw");
        }
        // inputs:scale/bias tint the sampled color (default identity).
        Vec3 sc{1.0f, 1.0f, 1.0f}, bi{0.0f, 0.0f, 0.0f};
        if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:scale")) {
          ReadVec3Value(*v, &sc);
        }
        if (const lightusd::next::Value *v = tex.GetPropertyValue("inputs:bias")) {
          ReadVec3Value(*v, &bi);
        }
        int32_t id = LoadTextureCached(tc, AnchorAssetNext(tex, *ap), ws, wt, srgb, sc, bi);
        if (id >= 0) {
          *tex_id = id;
          // A textured surface tints by the texture, not the (often unauthored)
          // diffuseColor constant. Use white so sampling shows true texels.
          *base_color = Vec3{1.0f, 1.0f, 1.0f};
          if (uv_xform) *uv_xform = ResolveUvXform(stage, tex);
        }
      }
    }
  }

  // Tangent-space normal map: inputs:normal -> UsdUVTexture(inputs:file). Always
  // raw (non-sRGB); scale/bias default to the UsdPreviewSurface convention
  // (2,-1) that unpacks a [0,1] texel to a [-1,1] tangent-space normal.
  lightusd::next::UsdPrim ntex = ConnectedPrimNext(stage, surf, "inputs:normal");
  if (ntex.IsValid()) {
    if (const lightusd::next::Value *fv = ntex.GetPropertyValue("inputs:file")) {
      const std::string *ap = fv->as_asset_path();
      if (!ap) ap = fv->as_string();
      if (ap && !ap->empty()) {
        WrapMode ws = WrapMode::Repeat, wt = WrapMode::Repeat;
        if (const lightusd::next::Value *v = ntex.GetPropertyValue("inputs:wrapS"))
          if (const std::string *t = v->as_token()) ws = ParseWrapMode(*t);
        if (const lightusd::next::Value *v = ntex.GetPropertyValue("inputs:wrapT"))
          if (const std::string *t = v->as_token()) wt = ParseWrapMode(*t);
        Vec3 scale{2.0f, 2.0f, 2.0f}, bias{-1.0f, -1.0f, -1.0f};
        if (const lightusd::next::Value *v = ntex.GetPropertyValue("inputs:scale")) {
          ReadVec3Value(*v, &scale);
        }
        if (const lightusd::next::Value *v = ntex.GetPropertyValue("inputs:bias")) {
          ReadVec3Value(*v, &bias);
        }
        int32_t id =
            LoadTextureCached(tc, AnchorAssetNext(ntex, *ap), ws, wt, /*srgb=*/false, scale, bias);
        if (id >= 0) {
          *normal_tex_id = id;
          if (uv_xform && uv_xform->identity)
            *uv_xform = ResolveUvXform(stage, ntex);
        }
      }
    }
  }
}

void ApplyDisplayPrimvarsNext(const lightusd::next::UsdPrim &mesh,
                              MeshJobNext *job) {
  if (!job) return;
  constexpr double kDefaultTime = std::numeric_limits<double>::quiet_NaN();
  lightusd::tydra::next::ValueArrayRead<float> dc;
  if (lightusd::tydra::next::ReadFloatArray(
          mesh, "primvars:displayColor", kDefaultTime, &dc)) {
    if (dc.size() >= 3) job->base_color = Vec3{dc[0], dc[1], dc[2]};
    if (dc.size() > 3) job->vertex_color = true;
  }
  lightusd::tydra::next::ValueArrayRead<float> od;
  if (lightusd::tydra::next::ReadFloatArray(
          mesh, "primvars:displayOpacity", kDefaultTime, &od)) {
    if (!od.empty()) job->opacity = std::min(1.0f, std::max(0.0f, od[0]));
    if (od.size() > 1) job->vertex_color = true;
  }
}

void AssignResolvedToJob(const ResolvedMat &r, MeshJobNext *job) {
  if (!job) return;
  job->base_color = r.base_color;
  job->tex_id = r.tex_id;
  job->roughness = r.roughness;
  job->metallic = r.metallic;
  job->normal_tex_id = r.normal_tex_id;
  job->coat_normal_tex_id = r.coat_normal_tex_id;
  job->uv_xform = r.uv_xform;
  job->rough_tex = r.rough_tex;
  job->metal_tex = r.metal_tex;
  job->emission = r.emission;
  job->emission_tex_id = r.emission_tex_id;
  job->occlusion = r.occlusion;
  job->occ_tex = r.occ_tex;
  job->opacity = r.opacity;
  job->opacity_tex = r.opacity_tex;
  job->opacity_threshold = r.opacity_threshold;
  job->clearcoat = r.clearcoat;
  job->clearcoat_roughness = r.clearcoat_roughness;
  job->clearcoat_tex = r.clearcoat_tex;
  job->clearcoat_rough_tex = r.clearcoat_rough_tex;
  job->specular_color = r.specular_color;
  job->specular_tex_id = r.specular_tex_id;
  job->ior = r.ior;
  job->use_specular_workflow = r.use_specular_workflow;
  job->vertex_color = r.vertex_color;
  job->displacement = r.displacement;
  job->displacement_tex = r.displacement_tex;
  job->has_openpbr = r.has_openpbr;
  job->openpbr = r.openpbr;
  job->materialx_graph_json = r.materialx_graph_json;
}

ResolvedMat CaptureResolvedFromJob(const MeshJobNext &job) {
  ResolvedMat r;
  r.base_color = job.base_color;
  r.tex_id = job.tex_id;
  r.roughness = job.roughness;
  r.metallic = job.metallic;
  r.normal_tex_id = job.normal_tex_id;
  r.coat_normal_tex_id = job.coat_normal_tex_id;
  r.uv_xform = job.uv_xform;
  r.rough_tex = job.rough_tex;
  r.metal_tex = job.metal_tex;
  r.emission = job.emission;
  r.emission_tex_id = job.emission_tex_id;
  r.occlusion = job.occlusion;
  r.occ_tex = job.occ_tex;
  r.opacity = job.opacity;
  r.opacity_tex = job.opacity_tex;
  r.opacity_threshold = job.opacity_threshold;
  r.clearcoat = job.clearcoat;
  r.clearcoat_roughness = job.clearcoat_roughness;
  r.clearcoat_tex = job.clearcoat_tex;
  r.clearcoat_rough_tex = job.clearcoat_rough_tex;
  r.specular_color = job.specular_color;
  r.specular_tex_id = job.specular_tex_id;
  r.ior = job.ior;
  r.use_specular_workflow = job.use_specular_workflow;
  r.vertex_color = job.vertex_color;
  r.displacement = job.displacement;
  r.displacement_tex = job.displacement_tex;
  r.has_openpbr = job.has_openpbr;
  r.openpbr = job.openpbr;
  r.materialx_graph_json = job.materialx_graph_json;
  return r;
}

TriMat TriMatFromResolved(const ResolvedMat &r) {
  TriMat m;
  m.base_color = r.base_color;
  m.emission = r.emission;
  m.roughness = r.roughness;
  m.metallic = r.metallic;
  m.tex_id = r.tex_id;
  m.normal_tex_id = r.normal_tex_id;
  m.rough_tex_id = r.rough_tex.id;
  m.metal_tex_id = r.metal_tex.id;
  m.emission_tex_id = r.emission_tex_id;
  m.occ_tex_id = r.occ_tex.id;
  m.occlusion = r.occlusion;
  m.opacity = r.opacity;
  m.opacity_tex_id = r.opacity_tex.id;
  m.opacity_threshold = r.opacity_threshold;
  m.clearcoat = r.clearcoat;
  m.clearcoat_roughness = r.clearcoat_roughness;
  m.clearcoat_tex_id = r.clearcoat_tex.id;
  m.clearcoat_rough_tex_id = r.clearcoat_rough_tex.id;
  m.rough_ch = r.rough_tex.ch;
  m.metal_ch = r.metal_tex.ch;
  m.occ_ch = r.occ_tex.ch;
  m.opacity_ch = r.opacity_tex.ch;
  m.clearcoat_ch = r.clearcoat_tex.ch;
  m.clearcoat_rough_ch = r.clearcoat_rough_tex.ch;
  m.rough_tex_scale = r.rough_tex.scale;
  m.rough_tex_bias = r.rough_tex.bias;
  m.metal_tex_scale = r.metal_tex.scale;
  m.metal_tex_bias = r.metal_tex.bias;
  m.occ_tex_scale = r.occ_tex.scale;
  m.occ_tex_bias = r.occ_tex.bias;
  m.opacity_tex_scale = r.opacity_tex.scale;
  m.opacity_tex_bias = r.opacity_tex.bias;
  m.clearcoat_tex_scale = r.clearcoat_tex.scale;
  m.clearcoat_tex_bias = r.clearcoat_tex.bias;
  m.clearcoat_rough_tex_scale = r.clearcoat_rough_tex.scale;
  m.clearcoat_rough_tex_bias = r.clearcoat_rough_tex.bias;
  m.specular_color = r.specular_color;
  m.specular_tex_id = r.specular_tex_id;
  m.ior = r.ior;
  m.use_specular_workflow = r.use_specular_workflow;
  return m;
}

WrapMode ToTusdrWrap(lightusd::tydra::next::WrapMode w) {
  using NextWrap = lightusd::tydra::next::WrapMode;
  switch (w) {
    case NextWrap::Clamp: return WrapMode::Clamp;
    case NextWrap::Mirror: return WrapMode::Mirror;
    case NextWrap::Black: return WrapMode::Black;
    case NextWrap::Repeat:
    default: return WrapMode::Repeat;
  }
}

uint8_t ToScalarChannel(lightusd::tydra::next::RenderTexture::Channel ch) {
  using Channel = lightusd::tydra::next::RenderTexture::Channel;
  switch (ch) {
    case Channel::G: return 1;
    case Channel::B: return 2;
    case Channel::A: return 3;
    case Channel::R:
    case Channel::RGB:
    case Channel::RGBA:
    default: return 0;
  }
}

bool LoadRenderTexture(const lightusd::tydra::next::RenderScene &scene,
                       int32_t texture_id, TextureCache &tc, bool srgb,
                       const Vec3 *fallback_scale, const Vec3 *fallback_bias,
                       int32_t *out) {
  if (!out || texture_id < 0 ||
      size_t(texture_id) >= scene.textures.size()) {
    return false;
  }
  const lightusd::tydra::next::RenderTexture &tex =
      scene.textures[size_t(texture_id)];
  // Prefer the RESOLVED image path: `asset_path` is the raw authored string,
  // which in a nested look layer is relative to THAT layer, not to the scene
  // file. `resolved_path` carries the authoring layer's anchor (asset-anchor.hh).
  std::string asset;
  if (tex.image_id >= 0 && size_t(tex.image_id) < scene.images.size()) {
    asset = scene.images[size_t(tex.image_id)].resolved_path;
  }
  if (asset.empty()) asset = tex.asset_path;
  if (asset.empty()) return false;
  Vec3 scale{tex.scale_value.x, tex.scale_value.y, tex.scale_value.z};
  Vec3 bias{tex.bias.x, tex.bias.y, tex.bias.z};
  if (fallback_scale && fallback_bias &&
      scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f &&
      bias.x == 0.0f && bias.y == 0.0f && bias.z == 0.0f) {
    scale = *fallback_scale;
    bias = *fallback_bias;
  }
  // The caller's per-slot default ("color slots sRGB, data slots raw") is only
  // the fallback for "auto": an AUTHORED sourceColorSpace / colorSpace asset
  // metadata overrides it. Without this, a base-color map authored "raw"
  // (linear) was sRGB-decoded a second time, crushing the midtones -- and
  // disagreeing with the legacy resolver, which honors the attribute.
  bool effective_srgb = srgb;
  std::string source_space = tex.source_color_space;
  if (source_space.empty() || source_space == "auto") {
    source_space = srgb ? "srgb_rec709_scene" : "raw";
  }
  lightusd::color::ColorSpaceDesc source_desc;
  lightusd::color::ColorSpaceDesc display_desc;
  lightusd::color::ColorTransform color_transform;
  const lightusd::color::ColorTransform *color_transform_ptr = nullptr;
  if (tex.color_transform_valid &&
      lightusd::color::GetBuiltinColorSpace("lin_rec709_scene", &display_desc)) {
    source_desc.name = source_space;
    source_desc.gamma = tex.source_gamma;
    source_desc.linear_bias = tex.source_linear_bias;
    source_desc.kind = tex.source_color_is_data
                           ? lightusd::color::ColorSpaceKind::Data
                           : lightusd::color::ColorSpaceKind::Color;
    color_transform.source = source_desc;
    color_transform.destination = display_desc;
    color_transform.bypass = tex.color_transform_bypass;
    std::copy(tex.source_to_display_linear,
              tex.source_to_display_linear + 9, color_transform.matrix);
    color_transform_ptr = &color_transform;
    effective_srgb = !tex.source_color_is_data &&
                     std::fabs(tex.source_gamma - 2.4f) < 1.0e-5f &&
                     std::fabs(tex.source_linear_bias - 0.055f) < 1.0e-5f;
  } else if (lightusd::color::GetBuiltinColorSpace(source_space, &source_desc) &&
      lightusd::color::GetBuiltinColorSpace("lin_rec709_scene", &display_desc) &&
      lightusd::color::BuildColorTransform(source_desc, display_desc,
                                           &color_transform)) {
    color_transform_ptr = &color_transform;
    // Keep the sRGB hint for linear-light texture resizing. Sampling uses the
    // full transform below (and therefore does not double-decode); linear/data
    // spaces must not fall through to the compatibility sRGB decoder.
    effective_srgb =
        lightusd::color::CanonicalizeToken(source_space).rfind("srgb_", 0) == 0;
  } else if (source_space == "raw" || source_space == "Raw" ||
             source_space == "linear") {
    effective_srgb = false;
  } else if (source_space == "sRGB" || source_space == "srgb") {
    effective_srgb = true;
  }
  const int32_t id =
      LoadTextureCached(tc, asset, ToTusdrWrap(tex.wrap_s),
                        ToTusdrWrap(tex.wrap_t), effective_srgb, scale, bias,
                        color_transform_ptr);
  if (id < 0) return false;
  *out = id;
  return true;
}

bool LoadRenderScalarTexture(const lightusd::tydra::next::RenderScene &scene,
                             const lightusd::tydra::next::ShaderParam &param,
                             TextureCache &tc, bool srgb, ScalarTex *out) {
  if (!out || param.texture_id < 0 ||
      size_t(param.texture_id) >= scene.textures.size()) {
    return false;
  }
  const lightusd::tydra::next::RenderTexture &tex =
      scene.textures[size_t(param.texture_id)];
  const uint8_t ch = ToScalarChannel(tex.output_channel);
  int32_t id = -1;
  if (!LoadRenderTexture(scene, param.texture_id, tc, srgb, nullptr, nullptr, &id)) {
    return false;
  }
  const float scale_values[4] = {tex.scale_value.x, tex.scale_value.y,
                                 tex.scale_value.z, tex.scale_value.w};
  const float bias_values[4] = {tex.bias.x, tex.bias.y, tex.bias.z, tex.bias.w};
  out->id = id;
  out->ch = ch;
  out->scale = scale_values[std::min<int>(ch, 3)];
  out->bias = bias_values[std::min<int>(ch, 3)];
  return true;
}

void ApplyRenderTextureUvXform(const lightusd::next::Stage &stage,
                               const lightusd::tydra::next::RenderScene &scene,
                               int32_t texture_id, bool only_if_identity,
                               UvXform *out) {
  if (!out || texture_id < 0 || size_t(texture_id) >= scene.textures.size()) {
    return;
  }
  if (only_if_identity && !out->identity) return;
  const std::string &prim_path = scene.textures[size_t(texture_id)].prim_path;
  if (prim_path.empty()) return;
  lightusd::next::UsdPrim tex = stage.GetPrimAtPath(prim_path);
  if (!tex.IsValid()) return;
  *out = ResolveUvXform(stage, tex);
}

void ApplyLightRtOpenPBRParamsToJob(
    const lightusd::tydra::LightRtOpenPBRParams &p,
    MeshJobNext *job) {
  if (!job) return;
  job->base_color = Vec3{p.baseColor[0], p.baseColor[1], p.baseColor[2]};
  job->roughness = std::min(1.0f, std::max(0.0f, p.specularRoughness));
  job->metallic = std::min(1.0f, std::max(0.0f, p.metalness));
  job->emission = Vec3{p.emissionColor[0] * p.emission,
                       p.emissionColor[1] * p.emission,
                       p.emissionColor[2] * p.emission};
  job->opacity *= std::min(1.0f, std::max(0.0f, p.opacity));
  job->clearcoat = std::min(1.0f, std::max(0.0f, p.coatWeight));
  job->clearcoat_roughness = std::max(0.0f, p.coatRoughness);
  job->specular_color = Vec3{p.specularColor[0], p.specularColor[1],
                             p.specularColor[2]};
  job->ior = p.specularIor > 0.0f ? p.specularIor : 1.5f;
}

bool ResolveMeshMaterialTydraNext(const lightusd::next::Stage &stage,
                                  const lightusd::next::UsdPrim &mesh,
                                  TextureCache &tc, MeshJobNext *job,
                                  std::string *err, bool *degraded,
                                  const std::string &binding_override = {}) {
  if (!job) return false;
  if (degraded) *degraded = false;
  MeshJobNext resolved = *job;
  ApplyDisplayPrimvarsNext(mesh, &resolved);

  const std::string bindPath = binding_override.empty()
                                   ? lightusd::next::GetInheritedBoundMaterialPath(
                                         stage, mesh.GetPath().str())
                                   : binding_override;
  if (bindPath.empty()) {
    *job = resolved;
    return true;
  }
  lightusd::next::UsdPrim mat = stage.GetPrimAtPath(bindPath);
  if (!mat.IsValid()) {
    *job = resolved;
    return true;
  }

  lightusd::tydra::next::ConverterConfig config;
  config.material.load_textures = false;
  config.material.allow_missing_textures = true;
  if (tc.options) config.time_code = tc.options->timecode;
  lightusd::tydra::next::RenderSceneConverter converter(config);
  lightusd::tydra::next::RenderScene scratch;
  lightusd::tydra::next::RenderMaterial rm;
  if (!converter.ConvertMaterial(stage, mat, &rm, &scratch)) {
    if (err) *err = converter.GetLastError();
    return false;
  }
  // An unknown surface implementation is still a usable per-material degraded
  // PreviewSurface: the shared converter preserves conventional authored PBR
  // constants/textures around the unsupported node. Consume it here instead of
  // discarding those values and switching to the hand-rolled legacy resolver.
  if (degraded) *degraded = rm.default_fallback;
  for (const auto &diagnostic : rm.diagnostics) {
    using Kind = lightusd::tydra::next::MaterialDiagnosticKind;
    if (diagnostic.kind == Kind::UnsupportedMaterialXNode &&
        tc.unsupported_mtlx) {
      ++(*tc.unsupported_mtlx);
    }
    if (tc.material_diagnostic_examples &&
        tc.material_diagnostic_examples->size() < 8) {
      std::string example = diagnostic.material_path;
      if (!diagnostic.node_path.empty()) example += " node=" + diagnostic.node_path;
      if (!diagnostic.shader_id.empty()) example += " id=" + diagnostic.shader_id;
      example += " " + diagnostic.message;
      tc.material_diagnostic_examples->push_back(std::move(example));
    }
  }

  using NextMat = lightusd::tydra::next::RenderMaterial;
  if (rm.shader_type == NextMat::ShaderType::PreviewSurface &&
      rm.preview_surface) {
    const auto &s = *rm.preview_surface;
    lightusd::tydra::LightRtOpenPBRParams p;
    if (lightusd::tydra::next::BuildLightRtOpenPBRParams(rm, &p)) {
      ApplyLightRtOpenPBRParamsToJob(p, &resolved);
      resolved.has_openpbr = true;
      resolved.openpbr = p;
    }
    resolved.occlusion = std::min(1.0f, std::max(0.0f, s.occlusion.value.x));
    resolved.opacity_threshold = std::max(0.0f, s.opacity_threshold.value.x);
    resolved.use_specular_workflow = s.use_specular_workflow ? 1 : 0;
    resolved.displacement = s.displacement.value.x;
    if (LoadRenderTexture(scratch, s.diffuse_color.texture_id, tc, true,
                          nullptr, nullptr, &resolved.tex_id)) {
      resolved.base_color = Vec3{1.0f, 1.0f, 1.0f};
      ApplyRenderTextureUvXform(stage, scratch, s.diffuse_color.texture_id,
                                false, &resolved.uv_xform);
      // Which UV set the base-color texture reads (its UsdPrimvarReader
      // varname). Drives the mesh's `st` selection so a texture bound to a
      // secondary set (`uvSet1`) samples that set, not the primary.
      const int32_t did = s.diffuse_color.texture_id;
      if (did >= 0 && size_t(did) < scratch.textures.size()) {
        resolved.uv_primvar = scratch.textures[size_t(did)].uv_primvar;
      }
    }
    LoadRenderTexture(scratch, s.emissive_color.texture_id, tc, true,
                      nullptr, nullptr,
                      &resolved.emission_tex_id);
    const Vec3 normal_scale{2.0f, 2.0f, 2.0f};
    const Vec3 normal_bias{-1.0f, -1.0f, -1.0f};
    LoadRenderTexture(scratch, s.normal.texture_id, tc, false,
                      &normal_scale, &normal_bias,
                      &resolved.normal_tex_id);
    ApplyRenderTextureUvXform(stage, scratch, s.normal.texture_id,
                              true, &resolved.uv_xform);
    LoadRenderTexture(scratch, s.specular_color.texture_id, tc, true,
                      nullptr, nullptr,
                      &resolved.specular_tex_id);
    LoadRenderScalarTexture(scratch, s.roughness, tc, false, &resolved.rough_tex);
    LoadRenderScalarTexture(scratch, s.metallic, tc, false, &resolved.metal_tex);
    LoadRenderScalarTexture(scratch, s.occlusion, tc, false, &resolved.occ_tex);
    LoadRenderScalarTexture(scratch, s.opacity, tc, false, &resolved.opacity_tex);
    LoadRenderScalarTexture(scratch, s.clearcoat, tc, false,
                            &resolved.clearcoat_tex);
    LoadRenderScalarTexture(scratch, s.clearcoat_roughness, tc, false,
                            &resolved.clearcoat_rough_tex);
    LoadRenderScalarTexture(scratch, s.displacement, tc, false,
                            &resolved.displacement_tex);
  } else if (rm.shader_type == NextMat::ShaderType::OpenPBR && rm.openpbr) {
    const auto &s = *rm.openpbr;
    resolved.materialx_graph_json = s.nodegraph_json;
    lightusd::tydra::LightRtOpenPBRParams p;
    if (lightusd::tydra::next::BuildLightRtOpenPBRParams(rm, &p)) {
      ApplyLightRtOpenPBRParamsToJob(p, &resolved);
      resolved.has_openpbr = true;
      resolved.openpbr = p;
    }
    if (LoadRenderTexture(scratch, s.base_color.texture_id, tc, true,
                          nullptr, nullptr,
                          &resolved.tex_id)) {
      resolved.base_color = Vec3{1.0f, 1.0f, 1.0f};
      ApplyRenderTextureUvXform(stage, scratch, s.base_color.texture_id,
                                false, &resolved.uv_xform);
    }
    LoadRenderTexture(scratch, s.emission_color.texture_id, tc, true,
                      nullptr, nullptr,
                      &resolved.emission_tex_id);
    const Vec3 normal_scale{2.0f, 2.0f, 2.0f};
    const Vec3 normal_bias{-1.0f, -1.0f, -1.0f};
    LoadRenderTexture(scratch, s.normal.texture_id, tc, false,
                      &normal_scale, &normal_bias,
                      &resolved.normal_tex_id);
    LoadRenderTexture(scratch, s.coat_normal.texture_id, tc, false,
                      &normal_scale, &normal_bias,
                      &resolved.coat_normal_tex_id);
    ApplyRenderTextureUvXform(stage, scratch, s.normal.texture_id,
                              true, &resolved.uv_xform);
    LoadRenderScalarTexture(scratch, s.base_roughness, tc, false,
                            &resolved.rough_tex);
    LoadRenderScalarTexture(scratch, s.base_metalness, tc, false,
                            &resolved.metal_tex);
    LoadRenderScalarTexture(scratch, s.opacity, tc, false,
                            &resolved.opacity_tex);
    LoadRenderScalarTexture(scratch, s.coat_weight, tc, false,
                            &resolved.clearcoat_tex);
    LoadRenderScalarTexture(scratch, s.coat_roughness, tc, false,
                            &resolved.clearcoat_rough_tex);
  }

  const float *m = scratch.working_to_display_linear;
  const auto to_display = [m](const Vec3 &v) {
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
  };
  resolved.base_color = to_display(resolved.base_color);
  resolved.emission = to_display(resolved.emission);
  resolved.specular_color = to_display(resolved.specular_color);

  *job = resolved;
  return true;
}

bool MatNear(float a, float b, float eps = 1.0e-4f) {
  return std::fabs(a - b) <= eps;
}

bool MatNear(const Vec3 &a, const Vec3 &b, float eps = 1.0e-4f) {
  return MatNear(a.x, b.x, eps) && MatNear(a.y, b.y, eps) &&
         MatNear(a.z, b.z, eps);
}

bool TextureNear(int32_t a, int32_t b, const TextureCache &tc) {
  if (a == b) return true;
  if (a < 0 || b < 0 || !tc.textures ||
      size_t(a) >= tc.textures->size() ||
      size_t(b) >= tc.textures->size()) {
    return false;
  }
  const Texture &ta = (*tc.textures)[size_t(a)];
  const Texture &tb = (*tc.textures)[size_t(b)];
  if (ta.width != tb.width || ta.height != tb.height ||
      ta.channels != tb.channels || ta.is_udim != tb.is_udim ||
      ta.wrap_s != tb.wrap_s || ta.wrap_t != tb.wrap_t ||
      ta.srgb != tb.srgb ||
      ta.color_transform.source.name != tb.color_transform.source.name ||
      ta.color_transform.destination.name !=
          tb.color_transform.destination.name ||
      ta.color_transform.source.gamma != tb.color_transform.source.gamma ||
      ta.color_transform.source.linear_bias !=
          tb.color_transform.source.linear_bias ||
      ta.color_transform.source.kind != tb.color_transform.source.kind ||
      ta.color_transform.bypass != tb.color_transform.bypass ||
      !MatNear(ta.scale, tb.scale) ||
      !MatNear(ta.bias, tb.bias)) {
    return false;
  }
  for (size_t i = 0; i < 9; ++i) {
    if (std::fabs(ta.color_transform.matrix[i] -
                  tb.color_transform.matrix[i]) > 1.0e-6f) {
      return false;
    }
  }
  if (ta.is_udim) {
    if (ta.udim_tiles.size() != tb.udim_tiles.size()) return false;
    for (size_t i = 0; i < ta.udim_tiles.size(); ++i) {
      const Texture::UdimTile &ua = ta.udim_tiles[i];
      const Texture::UdimTile &ub = tb.udim_tiles[i];
      if (ua.udim != ub.udim || ua.width != ub.width ||
          ua.height != ub.height || ua.channels != ub.channels ||
          ua.pixels != ub.pixels) {
        return false;
      }
    }
    return true;
  }
  return ta.pixels == tb.pixels;
}

bool ScalarTextureNear(const ScalarTex &a, const ScalarTex &b,
                       const TextureCache &tc) {
  return a.ch == b.ch && MatNear(a.scale, b.scale) &&
         MatNear(a.bias, b.bias) && TextureNear(a.id, b.id, tc);
}

void ReportMaterialResolverDiff(const std::string &key,
                                const MeshJobNext &legacy,
                                const MeshJobNext &tydra,
                                const TextureCache &tc) {
  std::vector<std::string> diffs;
  if (legacy.tex_id < 0 && tydra.tex_id < 0 &&
      !MatNear(legacy.base_color, tydra.base_color)) {
    diffs.push_back("baseColor");
  }
  if (legacy.rough_tex.id < 0 && tydra.rough_tex.id < 0 &&
      !MatNear(legacy.roughness, tydra.roughness)) {
    diffs.push_back("roughness");
  }
  if (legacy.metal_tex.id < 0 && tydra.metal_tex.id < 0 &&
      !MatNear(legacy.metallic, tydra.metallic)) {
    diffs.push_back("metallic");
  }
  if (legacy.opacity_tex.id < 0 && tydra.opacity_tex.id < 0 &&
      !MatNear(legacy.opacity, tydra.opacity)) {
    diffs.push_back("opacity");
  }
  if (!MatNear(legacy.opacity_threshold, tydra.opacity_threshold)) {
    diffs.push_back("opacityThreshold");
  }
  if (legacy.emission_tex_id < 0 && tydra.emission_tex_id < 0 &&
      !MatNear(legacy.emission, tydra.emission)) {
    diffs.push_back("emission");
  }
  if (legacy.clearcoat_tex.id < 0 && tydra.clearcoat_tex.id < 0 &&
      !MatNear(legacy.clearcoat, tydra.clearcoat)) {
    diffs.push_back("clearcoat");
  }
  if (legacy.clearcoat_rough_tex.id < 0 && tydra.clearcoat_rough_tex.id < 0 &&
      !MatNear(legacy.clearcoat_roughness, tydra.clearcoat_roughness)) {
    diffs.push_back("clearcoatRoughness");
  }
  if ((legacy.use_specular_workflow || tydra.use_specular_workflow ||
       legacy.specular_tex_id >= 0 || tydra.specular_tex_id >= 0) &&
      !MatNear(legacy.specular_color, tydra.specular_color)) {
    diffs.push_back("specularColor");
  }
  if (!MatNear(legacy.ior, tydra.ior)) diffs.push_back("ior");
  if (!TextureNear(legacy.tex_id, tydra.tex_id, tc)) diffs.push_back("baseTexture");
  if (!TextureNear(legacy.normal_tex_id, tydra.normal_tex_id, tc)) {
    diffs.push_back("normalTexture");
  }
  if (!ScalarTextureNear(legacy.rough_tex, tydra.rough_tex, tc)) {
    diffs.push_back("roughTexture");
  }
  if (!ScalarTextureNear(legacy.metal_tex, tydra.metal_tex, tc)) {
    diffs.push_back("metalTexture");
  }
  if (!ScalarTextureNear(legacy.opacity_tex, tydra.opacity_tex, tc)) {
    diffs.push_back("opacityTexture");
  }
  if (legacy.uv_xform.identity != tydra.uv_xform.identity) {
    diffs.push_back("uvTransform");
  }
  if (diffs.empty()) return;
  std::cerr << "materialResolver compare: " << (key.empty() ? "<unbound>" : key)
            << " differs:";
  for (const std::string &d : diffs) std::cerr << " " << d;
  std::cerr << "\n";
}

// The full resolved-material result for one bound material (everything
// ResolveMeshMaterialNext writes). Memoized by bound-material path so a scene
// with many meshes sharing few materials (e.g. Island's 605k coral meshes over a
// handful of coral materials) resolves each material — and its shader-graph walk
// + texture lookups — exactly once instead of per mesh.

// Resolve a mesh job's material with per-material memoization. The result is a
// pure function of the bound material (+ shared texture cache), so a cache hit
// skips the shader-graph walk entirely. Unbound meshes keep MeshJobNext defaults.
void ResolveMeshMaterialCached(
    const lightusd::next::Stage &stage, const lightusd::next::UsdPrim &mesh_in,
    TextureCache &tc, std::unordered_map<std::string, ResolvedMat> &cache,
    MeshJobNext *job) {
  // A GeomSubset job resolves its binding from the SUBSET prim: the inheritance
  // walk finds the subset's own material:binding first, then falls back up the
  // ancestry (mesh, Xform, ...) -- exact UsdShade semantics for face subsets.
  const lightusd::next::UsdPrim &mesh =
      job->bind_prim.IsValid() ? job->bind_prim : mesh_in;
  const MeshJobNext input_job = *job;
  job->back_material.reset();
  const std::string key =
      lightusd::next::GetInheritedBoundMaterialPath(stage, mesh.GetPath().str());
  bool front_cached = false;
  if (!key.empty()) {
    auto it = cache.find(key);
    if (it != cache.end()) {
      AssignResolvedToJob(it->second, job);
      front_cached = true;
    }
  }

  auto resolve_legacy = [&]() {
    ResolveMeshMaterialNext(stage, mesh, tc, &job->base_color, &job->tex_id,
                            &job->roughness, &job->metallic, &job->normal_tex_id,
                            &job->uv_xform, &job->rough_tex, &job->metal_tex,
                            &job->emission, &job->emission_tex_id, &job->occlusion,
                            &job->occ_tex, &job->opacity, &job->opacity_tex,
                            &job->opacity_threshold, &job->clearcoat,
                            &job->clearcoat_roughness, &job->clearcoat_tex,
                            &job->clearcoat_rough_tex, &job->specular_color,
                            &job->specular_tex_id, &job->ior,
                            &job->use_specular_workflow, &job->vertex_color,
                            &job->displacement, &job->displacement_tex);
  };

  const Options::MaterialResolver mode =
      tc.options ? tc.options->material_resolver
                 : Options::MaterialResolver::Legacy;
  if (!front_cached && mode == Options::MaterialResolver::TydraNext) {
    std::string err;
    bool degraded = false;
    if (!ResolveMeshMaterialTydraNext(stage, mesh, tc, job, &err, &degraded)) {
      if (tc.degraded_materials) (*tc.degraded_materials)++;
      if (!err.empty()) {
        std::cerr << "materialResolver tydra-next failed for "
                  << (key.empty() ? "<unbound>" : key) << ": " << err
                  << "; using legacy resolver.\n";
      }
      resolve_legacy();
    } else if (degraded && tc.degraded_materials) {
      (*tc.degraded_materials)++;
    }
  } else if (!front_cached && mode == Options::MaterialResolver::Compare) {
    resolve_legacy();
    MeshJobNext legacy = *job;
    MeshJobNext tydra = input_job;
    std::string err;
    bool degraded = false;
    if (ResolveMeshMaterialTydraNext(stage, mesh, tc, &tydra, &err,
                                     &degraded)) {
      ReportMaterialResolverDiff(key, legacy, tydra, tc);
    } else if (!err.empty()) {
      std::cerr << "materialResolver compare: "
                << (key.empty() ? "<unbound>" : key)
                << " tydra-next failed: " << err << "\n";
    }
    *job = legacy;
  } else if (!front_cached) {
    resolve_legacy();
  }

  if (!front_cached && !key.empty()) {
    cache.emplace(key, CaptureResolvedFromJob(*job));
  }

  // Back-face purpose is exact: if absent, the integrator uses the front
  // material. Resolve it through the shared converter even when the selected
  // front resolver is legacy, because that converter is what preserves a
  // degraded UsdPreviewSurface for unsupported shader implementations.
  const std::string back_key =
      lightusd::next::GetInheritedBoundMaterialPathForPurpose(
          stage, mesh.GetPath().str(), "back");
  if (back_key.empty()) return;

  auto back_it = cache.find(back_key);
  if (back_it != cache.end()) {
    job->back_material = std::make_shared<ResolvedMat>(back_it->second);
    return;
  }
  MeshJobNext back_job = input_job;
  back_job.back_material.reset();
  std::string back_err;
  bool back_degraded = false;
  if (!ResolveMeshMaterialTydraNext(stage, mesh, tc, &back_job, &back_err,
                                    &back_degraded, back_key)) {
    if (!back_err.empty()) {
      std::cerr << "materialResolver back-face failed for " << back_key
                << ": " << back_err << "; using front material.\n";
    }
    return;
  }
  if (back_degraded && tc.degraded_materials) (*tc.degraded_materials)++;
  ResolvedMat back = CaptureResolvedFromJob(back_job);
  cache.emplace(back_key, back);
  job->back_material = std::make_shared<ResolvedMat>(std::move(back));
}

// Split each job whose mesh has material-bound face GeomSubsets into one job
// per bound subset (face mask + the subset as binding source) plus a remainder
// job for unclaimed faces. The streaming concat is one-material-per-job, so
// this is what makes per-face material bindings render: without it every
// triangle of the mesh took the single whole-mesh material (or the default
// gray). Meshes without bound face subsets pass through untouched.
void ExpandGeomSubsetJobsNext(const lightusd::next::Stage &stage, double time,
                              std::vector<MeshJobNext> *jobs) {
  (void)stage;
  if (!jobs) return;
  std::vector<MeshJobNext> out;
  out.reserve(jobs->size());
  for (MeshJobNext &job : *jobs) {
    // Already split (instanced prototypes may be expanded more than once).
    if (job.bind_prim.IsValid() || !job.subset_faces.empty()) {
      out.push_back(std::move(job));
      continue;
    }
    struct Sub {
      lightusd::next::UsdPrim prim;
      std::vector<int32_t> faces;
    };
    std::vector<Sub> subs;
    for (const lightusd::next::UsdPrim &c : job.prim.GetChildren()) {
      if (c.GetTypeName() != "GeomSubset") continue;
      // elementType defaults to "face"; other element types don't split faces.
      bool is_face = true;
      if (const lightusd::next::Value *et = c.GetPropertyValue("elementType"))
        if (const std::string *t = et->as_token())
          is_face = t->empty() || *t == "face";
      if (!is_face) continue;
      // Material subsets are familyName "materialBind" (accept unauthored).
      if (const lightusd::next::Value *fn = c.GetPropertyValue("familyName"))
        if (const std::string *t = fn->as_token())
          if (!t->empty() && *t != "materialBind") continue;
      // Only subsets that bind a material split faces; an unbound subset keeps
      // falling back to the whole-mesh material (no ancestor walk here -- that
      // is the mesh's own binding, i.e. the remainder job).
      if (lightusd::next::GetBoundMaterialPath(c).empty()) continue;
      std::vector<int32_t> faces = ReadIntArrayLazy(c, "indices", time);
      if (faces.empty()) continue;
      subs.push_back({c, std::move(faces)});
    }
    if (subs.empty()) {
      out.push_back(std::move(job));
      continue;
    }
    std::vector<int32_t> counts_probe =
        ReadIntArrayLazy(job.prim, "faceVertexCounts", time);
    const size_t nfaces = counts_probe.size();
    if (nfaces == 0) {
      out.push_back(std::move(job));
      continue;
    }
    // First claim wins on overlap (a materialBind family should be
    // non-overlapping per spec; be deterministic if it isn't).
    std::vector<char> claimed(nfaces, 0);
    for (Sub &s : subs) {
      std::vector<char> mask(nfaces, 0);
      size_t n = 0;
      for (int32_t f : s.faces) {
        if (f >= 0 && size_t(f) < nfaces && !claimed[size_t(f)]) {
          mask[size_t(f)] = 1;
          claimed[size_t(f)] = 1;
          ++n;
        }
      }
      if (n == 0) continue;
      MeshJobNext sj = job;  // copy world/purpose/prim; material resolved later
      sj.subset_faces = std::move(mask);
      sj.bind_prim = s.prim;
      out.push_back(std::move(sj));
    }
    // Remainder: faces no bound subset claimed keep the mesh's own binding.
    size_t unclaimed = 0;
    for (char cl : claimed)
      if (!cl) ++unclaimed;
    if (unclaimed > 0) {
      MeshJobNext rj = std::move(job);
      rj.subset_faces.resize(nfaces);
      for (size_t f = 0; f < nfaces; ++f) rj.subset_faces[f] = claimed[f] ? 0 : 1;
      out.push_back(std::move(rj));
    }
  }
  *jobs = std::move(out);
}

// True when `path` is one of the mask paths or a descendant of one. An empty
// mask matches everything.
bool PathMatchesMask(const std::string &path,
                     const std::vector<std::string> &mask) {
  if (mask.empty()) return true;
  bool has_include = false;
  for (const std::string &entry : mask) {
    if (entry.empty() || entry[0] != '!') continue;
    const std::string excluded = entry.substr(1);
    if (path == excluded ||
        (path.size() > excluded.size() &&
         path.compare(0, excluded.size(), excluded) == 0 &&
         path[excluded.size()] == '/')) return false;
  }
  for (const std::string &m : mask) {
    if (m.empty() || m[0] == '!') continue;
    has_include = true;
    if (path == m) return true;
    if (path.size() > m.size() && path.compare(0, m.size(), m) == 0 &&
        path[m.size()] == '/') {
      return true;
    }
  }
  return !has_include;
}

// True if any of the prim's authored xform ops are time-sampled (so its local
// transform varies with time).
bool PrimHasAnimatedXform(const lightusd::next::UsdPrim &prim) {
  const lightusd::next::Value *orderv = prim.GetPropertyValue("xformOpOrder");
  const std::vector<std::string> *order =
      orderv ? orderv->as_token_array() : nullptr;
  if (!order) return false;
  for (const std::string &raw : *order) {
    std::string op = raw;
    if (op.rfind("!invert!", 0) == 0) op = op.substr(8);
    if (op == "!resetXformStack!") continue;
    if (prim.HasTimeSamples(op)) return true;
  }
  return false;
}

// True if a renderable prim's authored per-frame data is time-sampled. Points/
// topology affect the BVH directly; authored normals affect the parallel
// smooth-shading buffer, which is rebuilt alongside geometry when `-smooth` is
// active.
bool GeometryPrimHasAnimatedData(const lightusd::next::UsdPrim &prim) {
  const std::string &type = prim.GetTypeName();
  if (type == "Mesh") {
    const std::vector<lightusd::next::Path> *blend_targets =
        prim.GetRelationship("skel:blendShapeTargets");
    return prim.HasTimeSamples("points") ||
           prim.HasTimeSamples("faceVertexIndices") ||
           prim.HasTimeSamples("faceVertexCounts") ||
           prim.HasTimeSamples("normals") ||
           prim.HasTimeSamples("primvars:normals") ||
           (blend_targets && !blend_targets->empty());
  }
  if (type == "BasisCurves" || type == "HermiteCurves" ||
      type == "NurbsCurves" || type == "Points") {
    return prim.HasTimeSamples("points") || prim.HasTimeSamples("widths") ||
           prim.HasTimeSamples("normals") ||
           prim.HasTimeSamples("curveVertexCounts") ||
           prim.HasTimeSamples("velocities") ||
           prim.HasTimeSamples("accelerations");
  }
  if (type == "ParticleField3DGaussianSplat") {
    return prim.HasTimeSamples("positions") ||
           prim.HasTimeSamples("scales") ||
           prim.HasTimeSamples("orientations") ||
           prim.HasTimeSamples("opacities") ||
           prim.HasTimeSamples("sh");
  }
  if (type == "PointInstancer" || type == "UsdGeomPointInstancer") {
    return prim.HasTimeSamples("positions") ||
           prim.HasTimeSamples("orientations") ||
           prim.HasTimeSamples("scales") ||
           prim.HasTimeSamples("protoIndices") ||
           prim.HasTimeSamples("invisibleIds");
  }
  return false;
}

bool IsRenderableGeometryPrim(const lightusd::next::UsdPrim &prim) {
  const std::string &type = prim.GetTypeName();
  return type == "Mesh" || type == "BasisCurves" ||
         type == "HermiteCurves" || type == "NurbsCurves" || type == "Points" ||
         type == "ParticleField3DGaussianSplat" ||
         type == "PointInstancer" || type == "UsdGeomPointInstancer";
}

bool RenderablePrimHasAnimatedGeom(const lightusd::next::UsdPrim &prim) {
  return GeometryPrimHasAnimatedData(prim);
}

// True if the subtree contains rendered (masked) geometry whose world-space
// data varies with time: authored geometry or some xform op on the path (this
// prim or an ancestor) is animated. Cameras and non-rendered prims are ignored,
// so camera-only animation does not flag the BVH as dynamic.
bool SubtreeGeometryAnimated(const lightusd::next::UsdPrim &prim,
                             const std::vector<std::string> &mask,
                             bool ancestor_xform_animated) {
  const bool xform_anim =
      ancestor_xform_animated || PrimHasAnimatedXform(prim);
  if (IsRenderableGeometryPrim(prim) &&
      PathMatchesMask(prim.GetPath().str(), mask)) {
    if (xform_anim || RenderablePrimHasAnimatedGeom(prim)) return true;
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    if (SubtreeGeometryAnimated(child, mask, xform_anim)) return true;
  }
  return false;
}

bool SceneGeometryAnimated(const lightusd::next::Stage &stage,
                           const std::vector<std::string> &mask) {
  for (const lightusd::next::UsdPrim &root : stage.GetRootPrims()) {
    if (SubtreeGeometryAnimated(root, mask, false)) return true;
  }
  return false;
}

// Serial walk: resolve parent-dependent world matrices (at `time`) + purpose and
// flatten Mesh prims into jobs. `mask` (if non-empty) restricts emission to
// meshes at/under those prim paths (usdrecord --mask); the full tree is still
// walked so transform chains remain correct.
// Emit one placement. Two-level streaming (`sink` set) hands (prim, world,
// purpose) straight to the caller's grouping sink; the flat path (`jobs` set)
// appends a world-space MeshJobNext. Exactly one of jobs/sink is non-null.
static inline void EmitPlacementNext(std::vector<MeshJobNext> *jobs,
                                     const RtInstanceSink *sink, size_t *emitted,
                                     const lightusd::next::UsdPrim &prim,
                                     const matrix4d &world,
                                     lightusd::Purpose purpose) {
  if (sink) {
    (*sink)(prim, world, purpose);
    if (emitted) ++*emitted;
    return;
  }
  MeshJobNext job;
  job.prim = prim;
  job.world = world;
  job.purpose = purpose;
  jobs->push_back(std::move(job));
}
// Count of placements emitted so far (for the max_jobs budget), from whichever
// sink is active.
static inline size_t EmittedCountNext(const std::vector<MeshJobNext> *jobs,
                                      const size_t *emitted) {
  return emitted ? *emitted : jobs->size();
}

// Forward decls: instance -> world-space MeshJobNext expansion for the GPU flatten
// path (mutually recursive with CollectPreviewImplNext for nesting). `jobs`/`sink`
// select flat-vector vs streaming emit (see EmitPlacementNext).
static void ExpandPointInstancerJobsNext(
    const lightusd::next::Stage &stage,
    const lightusd::next::UsdPrim &instancer, const matrix4d &instancer_world,
    lightusd::Purpose purpose, double time,
    const std::vector<std::string> &mask, std::vector<MeshJobNext> *jobs,
    const RtInstanceSink *sink, size_t *emitted, size_t max_jobs);
// Native (scenegraph instanceable) instance: place the prototype's geometry at the
// instance proxy's world transform.
static void ExpandNativeInstanceJobsNext(const lightusd::next::Stage &stage,
                                         const std::string &proto_path,
                                         const matrix4d &world,
                                         lightusd::Purpose purpose, double time,
                                         std::vector<MeshJobNext> *jobs,
                                         const RtInstanceSink *sink,
                                         size_t *emitted);
// Collect a prototype's mesh jobs in prototype-LOCAL space (root at identity),
// expanding any nested instancers. Shared by both expanders above.
static void CollectExpandedProtoJobsNext(const lightusd::next::Stage &stage,
                                         const lightusd::next::UsdPrim &proto,
                                         lightusd::Purpose purpose, double time,
                                         std::vector<MeshJobNext> *out);
// Shared body behind the public vector collector and the streaming placement
// collector.
static void CollectPreviewImplNext(const lightusd::next::Stage &stage,
                                   const lightusd::next::UsdPrim &prim,
                                   const matrix4d &parent_world,
                                   lightusd::Purpose inherited_purpose, double time,
                                   const std::vector<std::string> &mask,
                                   std::vector<MeshJobNext> *jobs,
                                   const RtInstanceSink *sink, size_t *emitted,
                                   bool expand_instancers, size_t max_jobs);

static void CollectPreviewImplNext(const lightusd::next::Stage &stage,
                                   const lightusd::next::UsdPrim &prim,
                                   const matrix4d &parent_world,
                                   lightusd::Purpose inherited_purpose, double time,
                                   const std::vector<std::string> &mask,
                                   std::vector<MeshJobNext> *jobs,
                                   const RtInstanceSink *sink, size_t *emitted,
                                   bool expand_instancers, size_t max_jobs) {
  if (!prim.IsActive()) return;  // inactive prim + its subtree are pruned
  // visibility="invisible" prunes the prim and its subtree, exactly like the
  // legacy path (BuildLegacyPurposeVisibility) and per UsdGeomImageable. It
  // used to be ignored here, so invisible prims rendered.
  if (const lightusd::next::Value *vv = prim.GetPropertyValue("visibility")) {
    if (const std::string *t = vv->as_token()) {
      if (*t == "invisible") return;
    }
  }
  if (max_jobs && EmittedCountNext(jobs, emitted) >= max_jobs)
    return;  // instance budget reached
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  lightusd::Purpose purpose = inherited_purpose;
  if (const lightusd::next::Value *pv = prim.GetPropertyValue("purpose")) {
    if (const std::string *t = pv->as_token()) {
      if (*t == "render") {
        purpose = lightusd::Purpose::Render;
      } else if (*t == "proxy") {
        purpose = lightusd::Purpose::Proxy;
      } else if (*t == "guide") {
        purpose = lightusd::Purpose::Guide;
      }
      // "default"/unknown: keep the inherited purpose.
    }
  }

  // Nested instancing: a prototype subtree may itself contain a PointInstancer or a
  // scenegraph (instanceable) instance. That geometry is NOT baked into this
  // prototype's base BLAS -- CollectProtoMeshNesting records those placements
  // separately and the TLAS flattens them (one level, composed per outer
  // placement). So do not descend here (mirror CollectSceneSplit). No-op for the
  // common leaf prototype (plain meshes), so non-nested scenes are byte-identical.
  if (prim.GetTypeName() == "PointInstancer") {
    // GPU flatten path: expand the instancer into world-space placements (no GPU
    // TLAS). Two-level callers leave expand_instancers=false and stop here.
    if (expand_instancers)
      ExpandPointInstancerJobsNext(stage, prim, world, purpose, time, mask, jobs,
                                   sink, emitted, max_jobs);
    return;
  }
  {
    const lightusd::next::PrimSpec *ispec = prim.GetPrimSpec();
    if (ispec && !ispec->meta().instance_prototype().empty()) {
      // Scenegraph (instanceable) native instance proxy: its children come from
      // the prototype. GPU flatten path expands it; two-level callers stop here.
      if (expand_instancers &&
          (!max_jobs || EmittedCountNext(jobs, emitted) < max_jobs) &&
          PathMatchesMask(prim.GetPath().str(), mask))
        ExpandNativeInstanceJobsNext(stage, ispec->meta().instance_prototype(),
                                     world, purpose, time, jobs, sink, emitted);
      return;
    }
  }
  if (prim.GetTypeName() == "Mesh" &&
      PathMatchesMask(prim.GetPath().str(), mask)) {
    EmitPlacementNext(jobs, sink, emitted, prim, world, purpose);
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    if (max_jobs && EmittedCountNext(jobs, emitted) >= max_jobs) break;
    CollectPreviewImplNext(stage, child, world, purpose, time, mask, jobs, sink,
                           emitted, expand_instancers, max_jobs);
  }
}

void CollectRTPreviewMeshesNext(const lightusd::next::Stage &stage,
                                const lightusd::next::UsdPrim &prim,
                                const matrix4d &parent_world,
                                lightusd::Purpose inherited_purpose, double time,
                                const std::vector<std::string> &mask,
                                std::vector<MeshJobNext> *jobs,
                                bool expand_instancers, size_t max_jobs) {
  CollectPreviewImplNext(stage, prim, parent_world, inherited_purpose, time, mask,
                         jobs, /*sink=*/nullptr, /*emitted=*/nullptr,
                         expand_instancers, max_jobs);
}

size_t CollectRTInstancePlacementsNext(const lightusd::next::Stage &stage,
                                       const lightusd::next::UsdPrim &prim,
                                       const matrix4d &parent_world,
                                       lightusd::Purpose inherited_purpose,
                                       double time,
                                       const std::vector<std::string> &mask,
                                       const RtInstanceSink &sink,
                                       size_t max_placements) {
  size_t emitted = 0;
  CollectPreviewImplNext(stage, prim, parent_world, inherited_purpose, time, mask,
                         /*jobs=*/nullptr, &sink, &emitted,
                         /*expand_instancers=*/true, max_placements);
  return emitted;
}

// Expand a UsdGeomPointInstancer into world-space MeshJobNext placements for the
// GPU flatten path. Mirrors CollectPointInstancer's instance iteration (same
// prototype resolution by child name -> stage path, same InstanceTRS *
// instancer_world transform, same invisibleIds/inactiveIds skip), but instead of
// reserving a shared BLAS + emitting an InstanceRT it bakes each prototype's
// mesh jobs to world space per instance. Each prototype's local mesh jobs are
// collected once (with nested instancers expanded recursively) and re-placed per
// instance.
static void ExpandPointInstancerJobsNext(
    const lightusd::next::Stage &stage,
    const lightusd::next::UsdPrim &instancer, const matrix4d &instancer_world,
    lightusd::Purpose purpose, double time,
    const std::vector<std::string> &mask, std::vector<MeshJobNext> *jobs,
    const RtInstanceSink *sink, size_t *emitted, size_t max_jobs) {
  if (!PathMatchesMask(instancer.GetPath().str(), mask)) return;
  const std::vector<lightusd::next::Path> *targets =
      instancer.GetRelationship("prototypes");
  if (!targets || targets->empty()) return;

  // Resolve each prototype target to a live prim (leaf name among the instancer's
  // children first, then an absolute stage lookup -- robust to re-rooting).
  std::unordered_map<std::string, lightusd::next::UsdPrim> children_by_name;
  for (const lightusd::next::UsdPrim &c : instancer.GetChildren())
    children_by_name.emplace(c.GetName(), c);

  // Per-prototype local mesh jobs (root at identity), collected lazily on first
  // use and reused across instances. Nested instancers under a prototype expand
  // recursively, so the GPU path renders nested scatters.
  std::vector<std::vector<MeshJobNext>> proto_jobs(targets->size());
  std::vector<bool> proto_done(targets->size(), false);
  std::vector<lightusd::next::UsdPrim> proto_prim(targets->size());
  for (size_t pi = 0; pi < targets->size(); ++pi) {
    const lightusd::next::Path &tp = (*targets)[pi];
    auto cit = children_by_name.find(tp.name());
    proto_prim[pi] = (cit != children_by_name.end())
                         ? cit->second
                         : stage.GetPrimAtPath(tp.str());
  }
  auto get_proto_jobs = [&](size_t pi) -> std::vector<MeshJobNext> & {
    if (!proto_done[pi]) {
      proto_done[pi] = true;
      CollectExpandedProtoJobsNext(stage, proto_prim[pi], purpose, time,
                                   &proto_jobs[pi]);
    }
    return proto_jobs[pi];
  };

  // Per-instance arrays (positions drives the count; the rest default).
  const std::vector<float> positions =
      ReadFloatArrayLazy(instancer, "positions", time);
  if (positions.empty()) return;
  const size_t n = positions.size() / 3;
  const std::vector<int32_t> proto_indices =
      ReadIntArrayLazy(instancer, "protoIndices", time);
  const std::vector<float> orientations =
      ReadFloatArrayLazy(instancer, "orientations", time);
  const std::vector<float> scales = ReadFloatArrayLazy(instancer, "scales", time);
  const std::vector<int64_t> invisible =
      ReadInt64ArrayLazy(instancer, "invisibleIds", time);
  const std::vector<int64_t> inactive =
      ReadInt64ArrayLazy(instancer, "inactiveIds", time);
  const std::vector<int64_t> ids = ReadInt64ArrayLazy(instancer, "ids", time);
  std::unordered_set<int64_t> hidden_set(invisible.begin(), invisible.end());
  hidden_set.insert(inactive.begin(), inactive.end());

  static const float kIdentQuat[4] = {1.0f, 0.0f, 0.0f, 0.0f};  // real-first
  static const float kUnitScale[3] = {1.0f, 1.0f, 1.0f};
  for (size_t i = 0; i < n; ++i) {
    if (max_jobs && EmittedCountNext(jobs, emitted) >= max_jobs)
      break;  // instance budget reached
    if (PointInstanceHidden(i, n, ids, hidden_set)) continue;
    const int32_t pidx = (i < proto_indices.size()) ? proto_indices[i] : 0;
    if (pidx < 0 || size_t(pidx) >= targets->size()) continue;
    const std::vector<MeshJobNext> &pjobs = get_proto_jobs(size_t(pidx));
    if (pjobs.empty()) continue;
    const float *q = (orientations.size() >= (i + 1) * 4) ? &orientations[i * 4]
                                                          : kIdentQuat;
    const float *s =
        (scales.size() >= (i + 1) * 3) ? &scales[i * 3] : kUnitScale;
    const matrix4d inst_world =
        InstanceTRS(&positions[i * 3], q, s) * instancer_world;
    for (const MeshJobNext &pj : pjobs)
      EmitPlacementNext(jobs, sink, emitted, pj.prim, pj.world * inst_world,
                        pj.purpose);
  }
}

// Collect a prototype's mesh jobs in prototype-LOCAL space (the holder/prototype
// root at identity; the instance transform is applied by the caller). Mirrors
// CollectProtoJobs but expands nested instancers (expand_instancers=true) so the
// flatten path renders instancers nested inside a prototype.
static void CollectExpandedProtoJobsNext(const lightusd::next::Stage &stage,
                                         const lightusd::next::UsdPrim &proto,
                                         lightusd::Purpose purpose, double time,
                                         std::vector<MeshJobNext> *out) {
  if (!proto.IsValid()) return;
  static const std::vector<std::string> kNoMask;
  // A prototype root may itself be a Mesh (a PointInstancer/native prototype can
  // point straight at one); collect it at identity too.
  if (proto.GetTypeName() == "Mesh") {
    MeshJobNext j;
    j.prim = proto;
    j.world = matrix4d::identity();
    j.purpose = purpose;
    out->push_back(std::move(j));
  }
  for (const lightusd::next::UsdPrim &child : proto.GetChildren())
    CollectRTPreviewMeshesNext(stage, child, matrix4d::identity(), purpose, time,
                               kNoMask, out, /*expand_instancers=*/true);
}

// Expand a scenegraph (instanceable) native instance into world-space MeshJobNext
// placements: collect the prototype's geometry once (prototype-local) and bake it
// at the instance proxy's world transform. The prototype path comes from the
// instance proxy's instance_prototype meta; with the `next` loader it resolves to
// a real placed prim (the prototype "holder"), whose own subtree is the geometry.
// This mirrors the CPU two-level CollectSceneSplit native-instance branch (which
// records an InstanceRT at `world` referencing the deduped prototype BLAS), but
// flattens to world space instead.
static void ExpandNativeInstanceJobsNext(const lightusd::next::Stage &stage,
                                         const std::string &proto_path,
                                         const matrix4d &world,
                                         lightusd::Purpose purpose, double time,
                                         std::vector<MeshJobNext> *jobs,
                                         const RtInstanceSink *sink,
                                         size_t *emitted) {
  lightusd::next::UsdPrim proto = stage.GetPrimAtPath(proto_path);
  if (!proto.IsValid()) return;
  std::vector<MeshJobNext> proto_jobs;
  CollectExpandedProtoJobsNext(stage, proto, purpose, time, &proto_jobs);
  for (const MeshJobNext &pj : proto_jobs)
    EmitPlacementNext(jobs, sink, emitted, pj.prim, pj.world * world, pj.purpose);
}

// `next`-path UsdVol volumes: serial walk resolving world matrices; for each
// Volume prim, follow `field:*` -> field-asset prim -> filePath, load the .vdb
// (relative to `baseDir`), and build a VolumeData for raymarching.
static bool FitNextVolumeDensity(VolumeData *volume, size_t max_bytes) {
  if (!volume || max_bytes < sizeof(float) || volume->density.empty() ||
      volume->dim[0] <= 0 || volume->dim[1] <= 0 || volume->dim[2] <= 0)
    return false;
  const size_t source_voxels = volume->density.size();
  const size_t max_voxels = max_bytes / sizeof(float);
  if (source_voxels <= max_voxels) return false;
  int dims[3] = {volume->dim[0], volume->dim[1], volume->dim[2]};
  const double scale = std::cbrt(static_cast<double>(max_voxels) /
                                 static_cast<double>(source_voxels));
  for (int a = 0; a < 3; ++a)
    dims[a] = std::max(1, static_cast<int>(std::floor(dims[a] * scale)));
  auto voxel_count = [&]() -> size_t {
    const size_t x = static_cast<size_t>(dims[0]);
    const size_t y = static_cast<size_t>(dims[1]);
    const size_t z = static_cast<size_t>(dims[2]);
    if (x > (std::numeric_limits<size_t>::max)() / y) return 0;
    const size_t xy = x * y;
    if (xy > (std::numeric_limits<size_t>::max)() / z) return 0;
    return xy * z;
  };
  while (voxel_count() > max_voxels) {
    int largest = 0;
    if (dims[1] > dims[largest]) largest = 1;
    if (dims[2] > dims[largest]) largest = 2;
    if (dims[largest] <= 1) break;
    --dims[largest];
  }
  const size_t target_voxels = voxel_count();
  if (target_voxels == 0 || target_voxels >= source_voxels) return false;
  std::vector<float> reduced(target_voxels);
  const size_t sx = static_cast<size_t>(volume->dim[0]);
  const size_t sy = static_cast<size_t>(volume->dim[1]);
  const size_t sz = static_cast<size_t>(volume->dim[2]);
  for (int z = 0; z < dims[2]; ++z) {
    const size_t oz = std::min(
        sz - 1, (static_cast<size_t>(z) * sz) /
                   static_cast<size_t>(dims[2]));
    for (int y = 0; y < dims[1]; ++y) {
      const size_t oy = std::min(
          sy - 1, (static_cast<size_t>(y) * sy) /
                   static_cast<size_t>(dims[1]));
      for (int x = 0; x < dims[0]; ++x) {
        const size_t ox = std::min(
            sx - 1, (static_cast<size_t>(x) * sx) /
                   static_cast<size_t>(dims[0]));
        const size_t dst = (static_cast<size_t>(z) *
                            static_cast<size_t>(dims[1]) +
                            static_cast<size_t>(y)) *
                               static_cast<size_t>(dims[0]) +
                           static_cast<size_t>(x);
        reduced[dst] = volume->density[(oz * sy + oy) * sx + ox];
      }
    }
  }
  volume->density.swap(reduced);
  volume->dim[0] = dims[0];
  volume->dim[1] = dims[1];
  volume->dim[2] = dims[2];
  return true;
}

void ResolveVolumeMaterialNext(const lightusd::next::Stage &stage,
                               const lightusd::next::UsdPrim &volume,
                               VolumeData *out) {
  if (!out) return;
  const std::string material_path =
      lightusd::next::GetInheritedBoundMaterialPath(stage,
                                                     volume.GetPath().str());
  if (material_path.empty()) return;
  const lightusd::next::UsdPrim material = stage.GetPrimAtPath(material_path);
  if (!material) return;
  const std::string shader_path =
      lightusd::next::GetVolumeShader(stage, material);
  if (shader_path.empty()) return;
  const lightusd::next::UsdPrim shader = stage.GetPrimAtPath(shader_path);
  if (!shader) return;

  auto scalar = [&](const char *name, float *dst) {
    lightusd::next::Value value;
    if (!lightusd::next::ResolveShaderPortValue(stage, shader, name, &value))
      return;
    if (const float *f = value.as_float()) *dst = *f;
    else if (const double *d = value.as_double()) *dst = float(*d);
  };
  auto color = [&](const char *name, Vec3 *dst) {
    lightusd::next::Value value;
    if (!lightusd::next::ResolveShaderPortValue(stage, shader, name, &value))
      return;
    if (const float *f = value.as_float3()) *dst = Vec3{f[0], f[1], f[2]};
  };

  // MaterialX standard_volume/OpenPBR spellings, plus the widespread
  // UsdVolumeShader aliases used by DCC exporters.
  scalar("inputs:density", &out->density_scale);
  color("inputs:scattering_color", &out->albedo);
  color("inputs:scatter_color", &out->albedo);
  color("inputs:emission_color", &out->emission);
  color("inputs:emissionColor", &out->emission);
  scalar("inputs:emission", &out->emission_scale);
  scalar("inputs:emission_intensity", &out->emission_scale);
  scalar("inputs:emissionIntensity", &out->emission_scale);
  out->density_scale = std::max(0.0f, out->density_scale);
  out->emission_scale = std::max(0.0f, out->emission_scale);
}

void CollectVolumesNext(const lightusd::next::Stage &stage,
                        const lightusd::next::UsdPrim &prim,
                        const matrix4d &parent_world, double time,
                        const std::string &baseDir,
                        std::vector<VolumeData> *out,
                        size_t max_density_bytes, size_t *density_bytes_used) {
  if (!prim.IsActive()) return;
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  if (prim.GetTypeName() == "Volume") {
    bool has_density_relationship = false;
    for (const std::string &name : prim.GetRelationshipNames()) {
      if (name == "field:density") has_density_relationship = true;
    }
    for (const std::string &relName : prim.GetRelationshipNames()) {
      if (relName.rfind("field:", 0) != 0) continue;
      // One UsdVol prim is one participating medium, not one medium per field.
      // Decode its density relationship once; auxiliary grids in the same VDB
      // are attached below. Preserve the old first-field fallback when density
      // was not authored.
      if (has_density_relationship && relName != "field:density") continue;
      const std::vector<lightusd::next::Path> *targets =
          prim.GetRelationship(relName);
      if (!targets || targets->empty()) continue;
      lightusd::next::UsdPrim field = stage.GetPrimAtPath((*targets)[0]);
      if (!field) continue;

      const lightusd::next::Value *fp = field.GetPropertyValue("filePath");
      const std::string *ap = fp ? fp->as_asset_path() : nullptr;
      if (!ap || ap->empty()) continue;
      std::string fieldName = relName.substr(std::strlen("field:"));
      if (const lightusd::next::Value *fn = field.GetPropertyValue("fieldName")) {
        if (const std::string *tk = fn->as_token()) fieldName = *tk;
      }
      std::string vpath = *ap;
      if (!vpath.empty() &&
          !lightusd::next::AssetResolver::IsAbsolutePath(vpath) &&
          !baseDir.empty()) {
        vpath = baseDir + "/" + vpath;
      }
      std::vector<lightusd::usdVol::VDBGrid> grids;
      std::string vw, ve;
      if (!lightusd::usdVol::ReadVDBFromFile(vpath, &grids, &vw, &ve) ||
          grids.empty()) {
        continue;
      }
      lightusd::usdVol::VDBGrid *g = nullptr;
      for (auto &gg : grids)
        if (gg.name == fieldName) { g = &gg; break; }
      if (!g) g = &grids[0];
      if (g->data.empty() || g->dim[0] <= 0 || g->dim[1] <= 0 || g->dim[2] <= 0)
        continue;

      VolumeData vd;
      vd.dim[0] = g->dim[0];
      vd.dim[1] = g->dim[1];
      vd.dim[2] = g->dim[2];
      for (const auto &aux : grids) {
        VolumeScalarField *dst = nullptr;
        if (aux.name == "temperature") {
          dst = &vd.temperature_field;
        } else if (aux.name == "emission" || aux.name == "flame" ||
                   aux.name == "heat") {
          dst = &vd.emission_field;
        }
        if (dst) {
          dst->data = aux.data;
          for (int a = 0; a < 3; ++a) dst->dim[a] = aux.dim[a];
          dst->bmin = Vec3{
              float(aux.origin[0]) * float(aux.voxel_size[0]) +
                  float(aux.world_translation[0]),
              float(aux.origin[1]) * float(aux.voxel_size[1]) +
                  float(aux.world_translation[1]),
              float(aux.origin[2]) * float(aux.voxel_size[2]) +
                  float(aux.world_translation[2])};
          dst->bmax = Vec3{
              float(aux.origin[0] + aux.dim[0]) * float(aux.voxel_size[0]) +
                  float(aux.world_translation[0]),
              float(aux.origin[1] + aux.dim[1]) * float(aux.voxel_size[1]) +
                  float(aux.world_translation[1]),
              float(aux.origin[2] + aux.dim[2]) * float(aux.voxel_size[2]) +
                  float(aux.world_translation[2])};
          dst->background = aux.background;
        }
      }
      vd.density = std::move(g->data);
      float lo[3], hi[3];
      for (int a = 0; a < 3; a++) {
        lo[a] = float(g->origin[a]) * float(g->voxel_size[a]) +
                float(g->world_translation[a]);
        hi[a] = float(g->origin[a] + g->dim[a]) * float(g->voxel_size[a]) +
                float(g->world_translation[a]);
      }
      vd.bmin = Vec3{lo[0], lo[1], lo[2]};
      vd.bmax = Vec3{hi[0], hi[1], hi[2]};
      matrix4d invw;
      if (!lightusd::inverse(world, invw, 1.0e-12)) invw = matrix4d::identity();
      vd.inv_world = invw;
      vd.background = g->background;
      ResolveVolumeMaterialNext(stage, prim, &vd);
      if (max_density_bytes > 0 && density_bytes_used) {
        const size_t used = *density_bytes_used;
        const size_t remaining = used < max_density_bytes
                                     ? max_density_bytes - used
                                     : 0;
        if (remaining < sizeof(float)) {
          std::cerr << "WARN: skipping volume '" << prim.GetPath().str()
                    << "': density budget exhausted\n";
          continue;
        }
        const int source_dim[3] = {vd.dim[0], vd.dim[1], vd.dim[2]};
        const size_t source_bytes = vd.density.size() * sizeof(float);
        if (FitNextVolumeDensity(&vd, remaining)) {
          std::cerr << "WARN: volume '" << prim.GetPath().str()
                    << "' downsampled from " << source_dim[0] << "x"
                    << source_dim[1] << "x" << source_dim[2] << " ("
                    << source_bytes / (1024 * 1024) << " MiB) to "
                    << vd.dim[0] << "x" << vd.dim[1] << "x" << vd.dim[2]
                    << " for the density budget\n";
        }
        *density_bytes_used += vd.density.size() * sizeof(float);
      }
      out->push_back(std::move(vd));
    }
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    CollectVolumesNext(stage, child, world, time, baseDir, out,
                       max_density_bytes, density_bytes_used);
  }
}

// Read a UsdGeomCamera attribute (float, with double fallback).
float ReadCamFloatNext(const lightusd::next::UsdPrim &prim, const char *name,
                       float fallback) {
  if (const lightusd::next::Value *v = prim.GetPropertyValue(name)) {
    if (const float *f = v->as_float()) return *f;
    if (const double *d = v->as_double()) return float(*d);
  }
  return fallback;
}

// Find a (named) UsdGeomCamera in the next stage and build a CameraFrame plus
// its aperture aspect (horizontal/vertical). An empty query matches the first
// camera. Mirrors CameraFrameFromGeomCamera but on the next stage with
// bit-exact world transforms.
bool FindNextCameraFrameRecursive(const lightusd::next::Stage &stage,
                                  const lightusd::next::UsdPrim &prim,
                                  const matrix4d &parent_world,
                                  const std::string &query, double time,
                                  CameraFrame *frame, float *aspect) {
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  if (prim.GetTypeName() == "Camera") {
    const std::string path = prim.GetPath().str();
    const std::string name = prim.GetName();
    const bool match =
        query.empty() || name == query || path == query ||
        (path.size() > query.size() &&
         path.compare(path.size() - query.size(), query.size(), query) == 0 &&
         path[path.size() - query.size() - 1] == '/');
    if (match) {
      const float focal = ReadCamFloatNext(prim, "focalLength", 50.0f);
      const float vap = ReadCamFloatNext(prim, "verticalAperture", 15.2908f);
      const float hap = ReadCamFloatNext(prim, "horizontalAperture", 20.955f);
      float znear = 0.1f, zfar = 1.0e6f;
      if (const lightusd::next::Value *v = prim.GetPropertyValue("clippingRange")) {
        if (const float *f = v->as_float2()) { znear = f[0]; zfar = f[1]; }
      }
      std::string proj = "perspective";
      if (const lightusd::next::Value *v = prim.GetPropertyValue("projection")) {
        if (const std::string *t = v->as_token()) proj = *t;
      }
      frame->origin = Vec3{float(world.m[3][0]), float(world.m[3][1]),
                           float(world.m[3][2])};
      frame->right = Normalize(TransformVector(world, Vec3{1.0f, 0.0f, 0.0f}));
      frame->up = Normalize(TransformVector(world, Vec3{0.0f, 1.0f, 0.0f}));
      frame->forward = Normalize(TransformVector(world, Vec3{0.0f, 0.0f, -1.0f}));
      frame->yfov = 2.0f * std::atan(0.5f * vap / std::max(1.0e-6f, focal));
      frame->xmag = hap;
      frame->ymag = vap;
      frame->znear = std::max(1.0e-5f, znear);
      frame->zfar = std::max(frame->znear, zfar);
      frame->ortho = (proj == "orthographic");
      if (aspect) *aspect = hap / std::max(1.0e-6f, vap);
      return true;
    }
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    if (FindNextCameraFrameRecursive(stage, child, world, query, time, frame,
                                     aspect)) {
      return true;
    }
  }
  return false;
}

bool FindNextCameraFrame(const lightusd::next::Stage &stage,
                         const std::string &query, double time,
                         CameraFrame *frame, float *aspect) {
  for (const lightusd::next::UsdPrim &root : stage.GetRootPrims()) {
    if (FindNextCameraFrameRecursive(stage, root, matrix4d::identity(), query,
                                     time, frame, aspect)) {
      return true;
    }
  }
  return false;
}

// OpenUSD usdrecord-style auto framing: replicates
// UsdAppUtilsFrameRecorder's default-GfCamera framing (focal 50mm, aperture
// 20.955 x 15.2908 -> horizontal FOV ~23.66 deg, aspect ~1.37). Positions a
// perspective camera on the depth axis so the bbox fits the horizontal FOV,
// front-on for Y-up and rotated for Z-up. Writes the aperture-derived image
// height (width / aspect) to `out_height`.
CameraFrame MakeUsdRecordCamera(const Bounds &bounds, lightusd::Axis up_axis,
                                int width, int *out_height) {
  constexpr float kPi = 3.14159265358979323846f;
  const float focal = 50.0f, hap = 20.955f, vap = 15.2908f;
  const float aspect = hap / vap;  // ~1.370
  const float half_hfov = std::atan(0.5f * hap / focal);
  if (out_height) {
    *out_height = std::max(1, int(std::lround(float(width) / aspect)));
  }

  Vec3 center{0.0f, 0.0f, 0.0f};
  Vec3 dim{2.0f, 2.0f, 2.0f};
  if (bounds.valid) {
    center = Mul(Add(bounds.lo, bounds.hi), 0.5f);
    dim = Sub(bounds.hi, bounds.lo);
  }

  CameraFrame frame;
  frame.yfov = 2.0f * std::atan(0.5f * vap / focal);
  frame.ortho = false;

  // Plane corner (half-extents in the focal plane) and the depth half-extent.
  float plane_x, plane_y, depth;
  Vec3 pos_dir, up_vec, fwd;
  if (up_axis == lightusd::Axis::Z) {
    plane_x = dim.x * 0.5f; plane_y = dim.z * 0.5f; depth = dim.y * 0.5f;
    pos_dir = Vec3{0.0f, -1.0f, 0.0f};  // back up along -Y
    fwd = Vec3{0.0f, 1.0f, 0.0f};
    up_vec = Vec3{0.0f, 0.0f, 1.0f};
  } else if (up_axis == lightusd::Axis::X) {
    plane_x = dim.y * 0.5f; plane_y = dim.z * 0.5f; depth = dim.x * 0.5f;
    pos_dir = Vec3{-1.0f, 0.0f, 0.0f};
    fwd = Vec3{1.0f, 0.0f, 0.0f};
    up_vec = Vec3{0.0f, 0.0f, 1.0f};
  } else {  // Y-up
    plane_x = dim.x * 0.5f; plane_y = dim.y * 0.5f; depth = dim.z * 0.5f;
    pos_dir = Vec3{0.0f, 0.0f, 1.0f};  // back up along +Z (look down -Z)
    fwd = Vec3{0.0f, 0.0f, -1.0f};
    up_vec = Vec3{0.0f, 1.0f, 0.0f};
  }

  // FLAT scene (a single quad, a ground plane, a card): one extent is ~zero. The
  // fixed axis-aligned view above then looks ALONG the plane -- exactly edge-on --
  // so the render comes out empty. The common case is a Z-up quad lying in the XY
  // plane, framed from -Y. Look down the degenerate axis instead so the scene is
  // seen face-on; the two remaining extents become the focal-plane half-extents.
  const float ext[3] = {dim.x, dim.y, dim.z};
  const float max_ext = std::max(ext[0], std::max(ext[1], ext[2]));
  int flat = -1;
  if (max_ext > 0.0f) {
    for (int i = 0; i < 3; i++) {
      if (ext[i] <= 1.0e-4f * max_ext) {
        flat = i;
        break;
      }
    }
  }
  // The axis the default view above already looks down.
  const int view_axis = (up_axis == lightusd::Axis::Z)   ? 1
                        : (up_axis == lightusd::Axis::X) ? 0
                                                         : 2;
  if (flat >= 0 && flat != view_axis) {
    auto unit = [](int a) {
      return Vec3{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f,
                  a == 2 ? 1.0f : 0.0f};
    };
    const int up_idx = (up_axis == lightusd::Axis::Z)   ? 2
                       : (up_axis == lightusd::Axis::X) ? 0
                                                        : 1;
    pos_dir = unit(flat);          // stand off on the + side of the plane...
    fwd = Mul(unit(flat), -1.0f);  // ...and look back down at it
    // The scene's up axis lies IN the plane, unless it IS the flat axis (a card
    // standing edge-up); then any in-plane axis serves as the roll reference.
    up_vec = (up_idx == flat) ? unit((flat + 1) % 3) : unit(up_idx);

    plane_x = ext[(flat + 1) % 3] * 0.5f;
    plane_y = ext[(flat + 2) % 3] * 0.5f;
    depth = ext[flat] * 0.5f;
  }

  const float plane_radius =
      std::sqrt(plane_x * plane_x + plane_y * plane_y);
  float distance = plane_radius / std::max(1.0e-6f, std::tan(half_hfov)) + depth;
  (void)kPi;

  frame.origin = Add(center, Mul(pos_dir, distance));
  frame.forward = Normalize(fwd);
  frame.right = Normalize(Cross(frame.forward, up_vec));
  if (Length(frame.right) < 1.0e-6f) frame.right = Vec3{1.0f, 0.0f, 0.0f};
  frame.up = Normalize(Cross(frame.right, frame.forward));
  const float diag = Length(dim);
  frame.znear = std::max(1.0e-4f, distance - diag);
  frame.zfar = distance + diag * 2.0f;
  return frame;
}

// Persistent render context: the loaded next stage, extracted geometry, and the
// built BVH are kept alive so the camera/render parameters can be changed and
// the scene re-rendered repeatedly without re-parsing or rebuilding the BVH
// (memory-persistent rendering, e.g. animation with a moving camera).

// Resolve the camera (named / autoframe / auto-fit) into ctx.camera and the
// image height into ctx.height, from the current ctx.opt + ctx.bounds.
void ResolveCameraNext(RenderContext &ctx) {
  const Options &opt = ctx.opt;
  RenderScene empty_render_scene;
  int height = opt.height;
  if (!opt.camera.empty()) {
    float cam_aspect = 16.0f / 9.0f;
    if (FindNextCameraFrame(ctx.stage, opt.camera, ctx.frame_time, &ctx.camera,
                            &cam_aspect)) {
      if (height <= 0) {
        {
          double dh = double(ctx.width) / double(cam_aspect);
          if (!std::isfinite(dh)) dh = 540.0;
          height = std::max(1, int(std::lround(std::min(32768.0, dh))));
        }
      }
    } else {
      std::cerr << "WARN: camera not found: " << opt.camera
                << ". Using auto-fit.\n";
      if (height <= 0) height = 540;
      Options auto_opt = opt;
      auto_opt.camera.clear();
      auto_opt.width = ctx.width;
      ctx.camera = MakeCameraFrame(empty_render_scene, auto_opt, ctx.bounds,
                                   height, ctx.up_axis);
    }
  } else if (opt.autoframe) {
    ctx.camera = MakeUsdRecordCamera(ctx.bounds, ctx.up_axis, ctx.width, &height);
  } else {
    if (height <= 0) height = 540;
    Options auto_opt = opt;
    auto_opt.camera.clear();
    auto_opt.width = ctx.width;
    ctx.camera = MakeCameraFrame(empty_render_scene, auto_opt, ctx.bounds,
                                 height, ctx.up_axis);
  }
  ctx.height = height;
}

void ResolveBackPlateNext(RenderContext &ctx) {
  ctx.backplates.clear();
  if (ctx.opt.camera.empty()) return;
  lightusd::next::UsdPrim camera = ctx.stage.GetPrimAtPath(ctx.opt.camera);
  if (!camera.IsValid()) {
    std::vector<lightusd::next::UsdPrim> stack = ctx.stage.GetRootPrims();
    while (!stack.empty()) {
      const lightusd::next::UsdPrim prim = stack.back();
      stack.pop_back();
      if (prim.GetTypeName() == "Camera" && prim.GetName() == ctx.opt.camera) {
        camera = prim;
        break;
      }
      for (const lightusd::next::UsdPrim &child : prim.GetChildren())
        stack.push_back(child);
    }
  }
  if (!camera.IsValid()) return;
  constexpr const char *prefix = "BackPlateAPI:";
  for (const std::string &schema : camera.GetMeta().apiSchemas()) {
    if (schema.rfind(prefix, 0) != 0) continue;
    lightusd::next::BackPlateData data;
    if (!lightusd::next::GetBackPlateData(
            ctx.stage, camera, schema.substr(std::strlen(prefix)), &data,
            ctx.frame_time) || data.image.empty() ||
        data.plate_visibility == "invisible") continue;
    auto anchored = [&](const std::string &path) {
      if (path.empty() || path[0] == '/') return path;
      const std::string base = DirName(ctx.opt.input);
      return base.empty() ? path : base + "/" + path;
    };
    BackPlateImage plate;
    if (!LoadBackPlateImage(anchored(data.image), anchored(data.alpha_image),
                            anchored(data.depth_image), &plate)) continue;
    plate.scale_x = data.scale_tweak[0];
    plate.scale_y = data.scale_tweak[1];
    plate.rotate_degrees = data.rotate_xyz_tweak[2];
    plate.translate_x = data.translate_tweak[0];
    plate.translate_y = data.translate_tweak[1];
    plate.gain = {data.luma_gain[0], data.luma_gain[1], data.luma_gain[2]};
    plate.lift = {data.luma_lift[0], data.luma_lift[1], data.luma_lift[2]};
    plate.gamma = {data.luma_gamma[0], data.luma_gamma[1], data.luma_gamma[2]};
    plate.depth_min_offset = data.depth_min_offset;
    plate.depth_normalizing_factor = data.depth_normalizing_factor;
    plate.depth_camera_space_offset = data.depth_camera_space_offset;
    ctx.backplates.push_back(std::move(plate));
  }
}

// Prototype BLAS to build: the holder prim's path + the inherited purpose
// context it was instanced under (part of the dedup key, so instances under a
// guide ancestor get a separate, purpose-culled BLAS).

// A curve prim (BasisCurves / HermiteCurves / NurbsCurves) to ray-trace as hair strands
// in the next path. `world` is the world transform; the linear-strand geometry is
// built into the RenderContext's DirectScene (shared by the flat and TLAS render
// paths) — see BuildNextCurves.

bool IsCurvePrimNext(const lightusd::next::UsdPrim &prim) {
  const std::string &t = prim.GetTypeName();
  return t == "BasisCurves" || t == "HermiteCurves" || t == "NurbsCurves";
}

unsigned HermiteTessellationSegmentsNext() {
  unsigned segments = 8;
  if (const char *s = std::getenv("LUSDR_HERMITE_SEGMENTS")) {
    char *end = nullptr;
    const unsigned long n = std::strtoul(s, &end, 10);
    if (end != s && *end == '\0' && n > 0 && n <= 1024)
      segments = static_cast<unsigned>(n);
  }
  return segments;
}

struct CurvePointViewNext {
  lightusd::tydra::next::ValueArrayRead<float> view;
  // Value clips are materialized here because their temporary Value/ArrayScratch
  // cannot outlive the resolver call. Authored lazy arrays remain borrowed.
  std::vector<float> clipped;
};

bool ReadCurvePointViewNext(
    const lightusd::next::UsdPrim &prim, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader,
    CurvePointViewNext *out) {
  if (!out) return false;
  *out = CurvePointViewNext{};
  if (ReadFloatArrayViewLazy(prim, "points", time, &out->view) &&
      !out->view.empty()) {
    return out->view.size() >= 3;
  }
  if (clip_loader) {
    for (lightusd::next::UsdPrim owner = prim; owner.IsValid();
         owner = owner.GetParent()) {
      const lightusd::next::PrimSpec *spec = owner.GetPrimSpec();
      if (!spec || !spec->meta().clips().is_dictionary()) continue;
      std::vector<lightusd::next::ValueClipSet> sets;
      if (!lightusd::next::ParseValueClipSets(owner, &sets, nullptr)) break;
      for (auto &set : sets) set.prim_path = prim.GetPath().str();
      lightusd::next::Value clipped;
      if (!lightusd::next::ResolveValueClipFromSets(
              sets, prim, "points", time, clip_loader, &clipped)) {
        break;
      }
      lightusd::next::ArrayScratch<float> scratch;
      lightusd::next::ArrayView<float> view;
      if (!lightusd::next::GetFloatArrayView(clipped, &scratch, &view)) break;
      out->clipped.assign(view.begin(), view.end());
      out->view.view.data = out->clipped.data();
      out->view.view.size = out->clipped.size();
      return out->view.size() >= 3;
    }
  }
  return false;
}

// Read a curve prim's `points` into a point3f vector for legacy callers that
// need an owning representation. The main chunked path consumes the view above
// directly and avoids this conversion copy.
std::vector<lightusd::value::point3f> ReadCurvePointsNext(
    const lightusd::next::UsdPrim &prim, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader) {
  CurvePointViewNext source;
  if (!ReadCurvePointViewNext(prim, time, clip_loader, &source)) return {};
  const float *pf = source.view.begin();
  const size_t pn = source.view.size();
  std::vector<lightusd::value::point3f> pts(pn / 3);
  for (size_t i = 0; i < pts.size(); ++i)
    pts[i] = {pf[3 * i + 0], pf[3 * i + 1], pf[3 * i + 2]};
  return pts;
}

// Build the collected curve jobs into the RenderContext's DirectScene as LightRT
// hair-strand scenes (round by default; flat/ribbon when the prim authors
// `normals`), reusing AppendLinearCurveStrands + the same intersectors the
// legacy direct path uses. Curve hits/occlusion are then traced by RenderImage's
// existing DirectScene path regardless of use_tlas. Returns false only on a
// LightRT build failure.
bool BuildNextCurves(RenderContext &ctx, const std::vector<CurveJobNext> &jobs,
                     double time, bool include_flat) {
  if (jobs.empty()) return true;
  lrt_tri_build_options build_opts;
  std::memset(&build_opts, 0, sizeof(build_opts));
  build_opts.quality = ctx.opt.quality;
  build_opts.layout = LRT_TRI_LAYOUT_AUTO;
  build_opts.num_threads = WorkerThreadCount(ctx.opt.threads);
  lrt_result lrt_err = LRT_RESULT_OK;
  auto make_strands = [](const std::vector<float> &pts,
                         const std::vector<float> &radii,
                         const std::vector<uint32_t> &first,
                         const std::vector<uint32_t> &count) {
    lrt_hair_strands s;
    std::memset(&s, 0, sizeof(s));
    s.points = pts.data();
    s.radius = radii.data();
    s.strand_first = first.data();
    s.strand_count = count.data();
    s.nstrands = first.size();
    s.npoints = radii.size();
    return s;
  };
  size_t chunk_segments = size_t(262144);
  if (const char *s = std::getenv("LUSDR_CURVE_CHUNK")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && n > 0) chunk_segments = static_cast<size_t>(n);
  }
  auto build_curve_chunks = [&](const std::vector<float> &pts,
                                const std::vector<float> &radii,
                                const std::vector<uint32_t> &strand_first,
                                const std::vector<uint32_t> &strand_count,
                                std::vector<TriInfo> *info,
                                size_t *info_offset,
                                std::vector<CurveSceneChunk> *chunks,
                                bool flat, const char *label) -> bool {
    for (size_t s = 0; s < strand_first.size();) {
      size_t segs = 0;
      const size_t group_start = s;
      while (s < strand_first.size()) {
        const size_t add = strand_count[s] > 1 ? strand_count[s] - 1 : 0;
        if (segs != 0 && segs + add > chunk_segments) break;
        segs += add;
        ++s;
      }
      if (segs == 0) {
        continue;
      }
      const uint32_t point0 = strand_first[group_start];
      const uint32_t point1 = strand_first[s - 1] + strand_count[s - 1];
      std::vector<float> sub_points(
          pts.begin() + size_t(point0) * 3,
          pts.begin() + size_t(point1) * 3);
      std::vector<float> sub_radii(
          radii.begin() + point0, radii.begin() + point1);
      std::vector<uint32_t> sub_first;
      std::vector<uint32_t> sub_count;
      sub_first.reserve(s - group_start);
      sub_count.reserve(s - group_start);
      for (size_t k = group_start; k < s; ++k) {
        sub_first.push_back(strand_first[k] - point0);
        sub_count.push_back(strand_count[k]);
      }
      lrt_hair_strands hs =
          make_strands(sub_points, sub_radii, sub_first, sub_count);
      CurveSceneChunk chunk;
      chunk.first = chunks->empty() ? 0 :
          chunks->back().first + chunks->back().count;
      chunk.count = segs;
      if (!info || !info_offset || *info_offset > info->size() ||
          info->size() - *info_offset < segs) {
        std::cerr << "Failed to map " << label << " curve metadata at chunk ["
                  << chunk.first << ", " << (chunk.first + segs) << "].\n";
        return false;
      }
      chunk.info.reserve(segs);
      for (size_t i = 0; i < segs; ++i)
        chunk.info.push_back(std::move((*info)[*info_offset + i]));
      *info_offset += segs;
      chunk.scene.reset(flat ? lrt_flatcurve_scene_build(&hs, &build_opts, &lrt_err)
                             : lrt_roundcurve_scene_build(&hs, &build_opts, &lrt_err));
      if (!chunk.scene) {
        std::cerr << "Failed to build LightRT " << label << " curve chunk ["
                  << chunk.first << ", " << (chunk.first + chunk.count)
                  << "] (err=" << int(lrt_err)
                  << "). Try LUSDR_CURVE_CHUNK.\n";
        return false;
      }
      chunks->push_back(std::move(chunk));
    }
    return true;
  };
  ctx.direct.round_curve_chunks.clear();
  ctx.direct.flat_curve_chunks.clear();
  ctx.direct.round_curve_info.clear();
  ctx.direct.flat_curve_info.clear();
  // Process one authored curve prim at a time. The previous implementation
  // accumulated every transformed point/radius in scene-wide vectors before
  // splitting them, which defeated the chunk limit for scenes with many large
  // curve prims. Each local carrier is released after its LightRT chunks are
  // built, so peak memory is bounded by one source prim plus one chunk.
  for (const CurveJobNext &job : jobs) {
    CurvePointViewNext point_source;
    if (!ReadCurvePointViewNext(job.prim, time, ctx.clip_stage_loader,
                                &point_source)) {
      ++ctx.stats.skipped_curves;
      ++ctx.stats.invalid_curve_data;
      continue;
    }
    std::vector<int32_t> counts32 =
        ReadIntArrayLazy(job.prim, "curveVertexCounts", time);
    if (counts32.empty()) {
      ++ctx.stats.skipped_curves;
      ++ctx.stats.invalid_curve_data;
      continue;
    }
    size_t total_points = 0;
    bool counts_valid = true;
    for (int32_t value : counts32) {
      if (value < 2 || total_points >
                           std::numeric_limits<size_t>::max() -
                               static_cast<size_t>(value)) {
        counts_valid = false;
        break;
      }
      total_points += static_cast<size_t>(value);
    }
    if (!counts_valid || total_points != point_source.view.size() / 3) {
      ++ctx.stats.skipped_curves;
      ++ctx.stats.invalid_curve_data;
      continue;
    }
    std::vector<int> counts(counts32.begin(), counts32.end());
    std::vector<float> widths = ReadFloatArrayLazy(job.prim, "widths", time);
    const bool flat = job.prim.GetPropertyValue("normals") != nullptr;
    if (flat && !include_flat) continue;
    std::vector<float> curve_points, curve_radii;
    std::vector<uint32_t> curve_first, curve_count;
    std::vector<TriInfo> curve_info;
    size_t curve_info_offset = 0;
    const bool hermite = job.prim.GetTypeName() == "HermiteCurves";
    lightusd::tydra::next::ValueArrayRead<float> tangents;
    const bool have_tangents =
        hermite && ReadFloatArrayViewLazy(job.prim, "tangents", time, &tangents);
    if (have_tangents && tangents.size() == point_source.view.size()) {
      AppendHermiteCurveStrands(
          point_source.view.begin(), tangents.begin(),
          point_source.view.size() / 3, counts, widths,
          HermiteTessellationSegmentsNext(), job.world, &curve_points,
          &curve_radii, &curve_first, &curve_count, &curve_info, &ctx.bounds);
    } else {
      if (hermite && have_tangents && !tangents.empty())
        std::cerr << "WARN: HermiteCurves '" << job.prim.GetPath().str()
                  << "' tangents do not match points; using control polygon.\n";
      AppendLinearCurveStrands(point_source.view.begin(),
                               point_source.view.size() / 3, counts, widths,
                               job.world, &curve_points, &curve_radii,
                               &curve_first, &curve_count, &curve_info,
                               &ctx.bounds);
    }
    if (curve_first.empty()) {
      ++ctx.stats.skipped_curves;
      continue;
    }
    if (!build_curve_chunks(
            curve_points, curve_radii, curve_first, curve_count,
            &curve_info, &curve_info_offset,
            flat ? &ctx.direct.flat_curve_chunks
                 : &ctx.direct.round_curve_chunks,
            flat, flat ? "flat" : "round"))
      return false;
  }
  if (ctx.opt.stats) {
    std::cerr << "native curves: round " << ctx.direct.round_curve_chunks.size()
              << " chunk(s), flat " << ctx.direct.flat_curve_chunks.size()
              << " chunk(s), segment limit " << chunk_segments << "\n";
    if (ctx.stats.skipped_curves != 0)
      std::cerr << "native curves skipped: " << ctx.stats.skipped_curves
                << " (invalid data: " << ctx.stats.invalid_curve_data << ")\n";
  }
  return true;
}

bool BuildNextFlatCurveMeshes(
    const std::vector<CurveJobNext> &jobs, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader,
    const CameraFrame &camera, std::vector<RTPreviewStats::MeshGeometry> *geos,
    std::vector<Vec3> *base_colors) {
  if (!geos || !base_colors) return false;
  size_t kChunkTriangles = size_t(262144);
  if (const char *s = std::getenv("LUSDR_GPU_TRIANGLE_CHUNK")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && *end == '\0' && n > 0)
      kChunkTriangles = static_cast<size_t>(n);
  }
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  positions.reserve(kChunkTriangles * 9);
  indices.reserve(kChunkTriangles * 3);
  size_t triangles = 0;
  auto flush = [&]() {
    if (triangles == 0) return;
    RTPreviewStats::MeshGeometry geo;
    geo.positions = std::move(positions);
    geo.indices = std::move(indices);
    geos->push_back(std::move(geo));
    base_colors->push_back(kCurveColor);
    positions.clear();
    indices.clear();
    positions.reserve(kChunkTriangles * 9);
    indices.reserve(kChunkTriangles * 3);
    triangles = 0;
  };
  auto append_vertex = [&](const Vec3 &p, uint32_t *index) {
    *index = static_cast<uint32_t>(positions.size() / 3);
    positions.insert(positions.end(), {p.x, p.y, p.z});
  };
  for (const CurveJobNext &job : jobs) {
    if (job.prim.GetPropertyValue("normals") == nullptr) continue;
    CurvePointViewNext point_source;
    if (!ReadCurvePointViewNext(job.prim, time, clip_loader, &point_source))
      continue;
    const std::vector<int32_t> counts32 =
        ReadIntArrayLazy(job.prim, "curveVertexCounts", time);
    if (counts32.empty()) continue;
    size_t total_points = 0;
    bool valid = true;
    for (int32_t count : counts32) {
      if (count < 2 || total_points >
          std::numeric_limits<size_t>::max() - static_cast<size_t>(count)) {
        valid = false;
        break;
      }
      total_points += static_cast<size_t>(count);
    }
    if (!valid || total_points != point_source.view.size() / 3) continue;
    const std::vector<int> counts(counts32.begin(), counts32.end());
    const std::vector<float> widths =
        ReadFloatArrayLazy(job.prim, "widths", time);
    std::vector<float> curve_points, curve_radii;
    std::vector<uint32_t> strand_first, strand_count;
    if (job.prim.GetTypeName() == "HermiteCurves") {
      lightusd::tydra::next::ValueArrayRead<float> tangents;
      const bool have_tangents = ReadFloatArrayViewLazy(
          job.prim, "tangents", time, &tangents);
      if (have_tangents && tangents.size() == point_source.view.size()) {
        AppendHermiteCurveStrands(
            point_source.view.begin(), tangents.begin(),
            point_source.view.size() / 3, counts, widths,
            HermiteTessellationSegmentsNext(), job.world, &curve_points,
            &curve_radii, &strand_first, &strand_count, nullptr, nullptr);
      } else {
        AppendLinearCurveStrands(
            point_source.view.begin(), point_source.view.size() / 3, counts,
            widths, job.world, &curve_points, &curve_radii, &strand_first,
            &strand_count, nullptr, nullptr);
      }
    } else {
      AppendLinearCurveStrands(
          point_source.view.begin(), point_source.view.size() / 3, counts,
          widths, job.world, &curve_points, &curve_radii, &strand_first,
          &strand_count, nullptr, nullptr);
    }
    for (size_t strand = 0; strand < strand_first.size(); ++strand) {
      const size_t first = strand_first[strand];
      const size_t count = strand_count[strand];
      for (size_t i = 0; i + 1 < count; ++i) {
        const Vec3 p0{curve_points[(first + i) * 3 + 0],
                      curve_points[(first + i) * 3 + 1],
                      curve_points[(first + i) * 3 + 2]};
        const Vec3 p1{curve_points[(first + i + 1) * 3 + 0],
                      curve_points[(first + i + 1) * 3 + 1],
                      curve_points[(first + i + 1) * 3 + 2]};
        const Vec3 axis = Sub(p1, p0);
        const float axis_len = Length(axis);
        if (!std::isfinite(axis_len) || axis_len <= 1.0e-8f) continue;
        const Vec3 tangent = Mul(axis, 1.0f / axis_len);
        Vec3 side = Cross(tangent, camera.forward);
        if (Length(side) <= 1.0e-6f) side = Cross(tangent, camera.up);
        const float side_len = Length(side);
        if (!std::isfinite(side_len) || side_len <= 1.0e-6f) continue;
        side = Mul(side, 1.0f / side_len);
        const float r0 = curve_radii[first + i];
        const float r1 = curve_radii[first + i + 1];
        if (!std::isfinite(r0) || !std::isfinite(r1) || r0 <= 0.0f ||
            r1 <= 0.0f) continue;
        if (triangles + 2 > kChunkTriangles) flush();
        const Vec3 a = Sub(p0, Mul(side, r0));
        const Vec3 b = Add(p0, Mul(side, r0));
        const Vec3 c = Add(p1, Mul(side, r1));
        const Vec3 d = Sub(p1, Mul(side, r1));
        uint32_t ia, ib, ic, id;
        append_vertex(a, &ia); append_vertex(b, &ib);
        append_vertex(c, &ic); append_vertex(d, &id);
        indices.insert(indices.end(), {ia, ib, ic, ia, ic, id});
        triangles += 2;
      }
    }
  }
  flush();
  return true;
}

bool BuildNextFlatCurveBounds(
    const std::vector<CurveJobNext> &jobs, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader, Bounds *bounds) {
  if (!bounds) return false;
  for (const CurveJobNext &job : jobs) {
    if (job.prim.GetPropertyValue("normals") == nullptr) continue;
    CurvePointViewNext point_source;
    if (!ReadCurvePointViewNext(job.prim, time, clip_loader, &point_source))
      continue;
    for (size_t i = 0; i + 2 < point_source.view.size(); i += 3) {
      const Vec3 p = TransformPoint(
          job.world, Vec3{point_source.view[i], point_source.view[i + 1],
                          point_source.view[i + 2]});
      if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
        Expand(bounds, p);
    }
  }
  return true;
}

// Gaussian splats are not Mesh prims and must not be forced through the
// triangle stream. Build one native LightRT ellipse scene from the composed
// point field instead. The DC SH coefficient is used as the direct primitive's
// albedo; opacity is folded into it until the integrator grows true
// front-to-back splat compositing.
bool BuildNextGaussianEllipses(const lightusd::next::Stage &stage,
                               RenderContext &ctx, double time,
                               bool defer_gpu_bvh) {
  std::vector<float> centers;
  std::vector<float> radii;
  std::vector<float> normals;
  std::vector<float> major_axes;
  std::vector<TriInfo> chunk_info;
  size_t budget = 0;
  if (const char *s = std::getenv("LUSDR_GAUSSIAN_MAX")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && n > 0) budget = static_cast<size_t>(n);
  }
  size_t chunk_size = size_t(262144);
  if (const char *s = std::getenv("LUSDR_GAUSSIAN_CHUNK")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && n > 0) chunk_size = static_cast<size_t>(n);
  }
  lrt_tri_build_options opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.quality = ctx.opt.quality;
  opts.layout = LRT_TRI_LAYOUT_AUTO;
  opts.num_threads = WorkerThreadCount(ctx.opt.threads);
  ctx.direct.ellipse_chunks.clear();
  bool build_ok = true;
  size_t total_count = 0;
  auto flush_chunk = [&]() -> bool {
    const size_t count = centers.size() / 3;
    if (count == 0) return true;
    EllipseSceneChunk chunk;
    chunk.first = total_count;
    chunk.count = count;
    chunk.info = std::move(chunk_info);
    if (defer_gpu_bvh) {
      chunk.centers = std::move(centers);
      chunk.radii = std::move(radii);
      chunk.normals = std::move(normals);
      chunk.major_axes = std::move(major_axes);
    } else {
      lrt_result e = LRT_RESULT_OK;
      chunk.scene.reset(lrt_ellipse_scene_build_oriented(
          centers.data(), radii.data(), normals.data(), major_axes.data(), count,
          &opts, &e));
      if (!chunk.scene) {
        std::cerr << "Failed to build LightRT Gaussian ellipse chunk ["
                  << chunk.first << ", " << (chunk.first + count)
                  << "] (err=" << int(e)
                  << "). Try -maxMem, -mask, or LUSDR_GAUSSIAN_CHUNK.\n";
        return false;
      }
    }
    ctx.direct.ellipse_chunks.push_back(std::move(chunk));
    centers.clear();
    radii.clear();
    normals.clear();
    major_axes.clear();
    chunk_info.clear();
    total_count += count;
    return true;
  };
  size_t count_seen = 0;
  auto visit = [&](const auto &self, const lightusd::next::UsdPrim &prim,
                   const matrix4d &parent) -> void {
    if (!prim.IsActive() || !build_ok) return;
    double dmat[16];
    lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
    const matrix4d local = Mat4FromArray(dmat);
    const matrix4d world = local * parent;
    if (prim.GetTypeName() == "ParticleField3DGaussianSplat") {
      lightusd::next::ParticleFieldData field;
      std::string field_warning;
      if (!lightusd::next::GetParticleFieldData(
              stage, prim, &field, time, &field_warning)) {
        build_ok = false;
        return;
      }
      if (!field_warning.empty()) std::cerr << field_warning << "\n";
      // Keep uncompressed USDC arrays borrowed from the retained crate buffer
      // (and decode compressed/lazy values only into the ValueArrayRead's
      // bounded scratch). The previous Copy helper held five complete source
      // arrays live while the final ellipse arrays were being built.
      lightusd::tydra::next::ValueArrayRead<float> p;
      lightusd::tydra::next::ValueArrayRead<float> s;
      lightusd::tydra::next::ValueArrayRead<float> qv;
      lightusd::tydra::next::ValueArrayRead<float> op;
      lightusd::tydra::next::ValueArrayRead<float> sh;
      const bool have_p = !field.positions_property.empty() &&
          ReadFloatArrayViewLazy(prim, field.positions_property.c_str(), time,
                                 &p);
      const bool have_s = !field.scales_property.empty() &&
          ReadFloatArrayViewLazy(prim, field.scales_property.c_str(), time, &s);
      const bool have_q = !field.orientations_property.empty() &&
          ReadFloatArrayViewLazy(prim, field.orientations_property.c_str(), time,
                                 &qv);
      const bool have_op = !field.opacities_property.empty() &&
          ReadFloatArrayViewLazy(prim, field.opacities_property.c_str(), time,
                                 &op);
      const bool allow_sh = AllowGaussianSHDecode(prim);
      const bool have_sh = allow_sh &&
          !field.spherical_harmonics_property.empty() && ReadFloatArrayViewLazy(
          prim, field.spherical_harmonics_property.c_str(), time, &sh);
      if (!allow_sh)
        std::cerr << "Gaussian splat: skipping oversized compressed SH payload at "
                  << prim.GetPath().str() << "; using fallback color.\n";
      if (!have_p || !have_s || p.size() < 3 || s.size() < 3) {
        for (const lightusd::next::UsdPrim &child : prim.GetChildren())
          self(self, child, world);
        return;
      }
      const size_t n = std::min(p.size() / 3, s.size() / 3);
      const size_t sh_stride = (have_sh && n) ? (sh.size() / 3) / n : 0;
      const float *pp = p.begin();
      const float *ss = s.begin();
      const float *qq = have_q ? qv.begin() : nullptr;
      const float *oo = have_op ? op.begin() : nullptr;
      const float *hh = have_sh ? sh.begin() : nullptr;
      for (size_t i = 0; i < n && (budget == 0 || total_count + chunk_info.size() < budget); ++i) {
        ++count_seen;
        const float opacity = (oo && op.size() > 1 && i < op.size())
                                  ? oo[i]
                                  : (oo && op.size() == 1 ? oo[0] : 1.0f);
        const float sx = std::fabs(ss[i * 3]);
        const float sy = std::fabs(ss[i * 3 + 1]);
        const float sz = std::fabs(ss[i * 3 + 2]);
        if (!std::isfinite(opacity) || opacity < 0.01f ||
            !std::isfinite(sx) || !std::isfinite(sy) ||
            !std::isfinite(sz) || sx <= 1.0e-8f || sy <= 1.0e-8f ||
            !std::isfinite(pp[i * 3]) || !std::isfinite(pp[i * 3 + 1]) ||
            !std::isfinite(pp[i * 3 + 2]))
          continue;
        const Vec3 c = TransformPoint(world, Vec3{pp[i * 3], pp[i * 3 + 1],
                                                   pp[i * 3 + 2]});
        lightusd::value::quatf q;
        q.real = 1.0f;
        q.imag[0] = q.imag[1] = q.imag[2] = 0.0f;
        const size_t orientation_index = qv.size() == 4 ? 0 : i * 4;
        if (qq && orientation_index + 3 < qv.size()) {
          q.real = qq[orientation_index]; q.imag[0] = qq[orientation_index + 1];
          q.imag[1] = qq[orientation_index + 2]; q.imag[2] = qq[orientation_index + 3];
        }
        const lightusd::value::matrix3d r = lightusd::to_matrix3x3(q);
        const lightusd::value::matrix4d r4 = lightusd::to_matrix(
            r, lightusd::value::double3{0.0, 0.0, 0.0});
        const Vec3 nx = TransformVector(world, TransformVector(r4, Vec3{1, 0, 0}));
        const Vec3 ny = TransformVector(world, TransformVector(r4, Vec3{0, 1, 0}));
        const Vec3 nz = Normalize(TransformVector(world, TransformVector(r4, Vec3{0, 0, 1})));
        const float rx = std::max(1.0e-6f, 2.0f * sx * Length(nx));
        const float ry = std::max(1.0e-6f, 2.0f * sy * Length(ny));
        centers.insert(centers.end(), {c.x, c.y, c.z});
        radii.insert(radii.end(), {rx, ry});
        normals.insert(normals.end(), {nz.x, nz.y, nz.z});
        const Vec3 major = Normalize(nx);
        major_axes.insert(major_axes.end(), {major.x, major.y, major.z});
        TriInfo ti;
        ti.p0 = c;
        ti.p1 = nz;
        ti.base_color = Vec3{0.72f, 0.72f, 0.72f};
        if (hh && sh_stride >= 1 && i * sh_stride * 3 + 2 < sh.size()) {
          const size_t j = i * sh_stride * 3;
          ti.base_color = Vec3{
              std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * hh[j])),
              std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * hh[j + 1])),
              std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * hh[j + 2]))};
        }
        ti.base_color = Mul(ti.base_color, opacity);
        chunk_info.push_back(ti);
        Expand(&ctx.bounds, Vec3{c.x - std::max(rx, ry), c.y - std::max(rx, ry), c.z - std::max(rx, ry)});
        Expand(&ctx.bounds, Vec3{c.x + std::max(rx, ry), c.y + std::max(rx, ry), c.z + std::max(rx, ry)});
        if (centers.size() / 3 >= chunk_size) {
          build_ok = flush_chunk();
          if (!build_ok) break;
        }
      }
    }
    for (const lightusd::next::UsdPrim &child : prim.GetChildren())
      self(self, child, world);
  };
  for (const lightusd::next::UsdPrim &root : stage.GetRootPrims())
    visit(visit, root, matrix4d::identity());
  if (!build_ok) return false;
  if (!flush_chunk()) return false;
  if (total_count == 0) return true;
  std::cerr << "native Gaussian ellipses: " << total_count
            << " in " << ctx.direct.ellipse_chunks.size() << " chunk(s)";
  if (budget != 0) std::cerr << " / " << count_seen << " budgeted";
  std::cerr << "\n";
  return true;
}

bool BuildDeferredGaussianChunk(const Options &opt, EllipseSceneChunk *chunk) {
  if (!chunk || chunk->scene || chunk->count == 0 ||
      chunk->centers.size() != chunk->count * 3u ||
      chunk->radii.size() != chunk->count * 2u ||
      chunk->normals.size() != chunk->count * 3u ||
      chunk->major_axes.size() != chunk->count * 3u) {
    if (chunk && chunk->scene) return true;
    std::cerr << "Invalid deferred Gaussian ellipse chunk";
    if (chunk) {
      std::cerr << " [" << chunk->first << ", "
                << (chunk->first + chunk->count) << "]";
    }
    std::cerr << ".\n";
    return false;
  }
  lrt_tri_build_options opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.quality = opt.quality;
  opts.layout = LRT_TRI_LAYOUT_AUTO;
  opts.num_threads = WorkerThreadCount(opt.threads);
  lrt_result e = LRT_RESULT_OK;
  chunk->scene.reset(lrt_ellipse_scene_build_oriented(
      chunk->centers.data(), chunk->radii.data(), chunk->normals.data(),
      chunk->major_axes.data(), chunk->count, &opts, &e));
  if (!chunk->scene) {
    std::cerr << "Failed to build LightRT deferred Gaussian ellipse chunk ["
              << chunk->first << ", " << (chunk->first + chunk->count)
              << "] (err=" << int(e)
              << "). Try -maxMem, -mask, or LUSDR_GAUSSIAN_CHUNK.\n";
    return false;
  }
  std::vector<float>().swap(chunk->centers);
  std::vector<float>().swap(chunk->radii);
  std::vector<float>().swap(chunk->normals);
  std::vector<float>().swap(chunk->major_axes);
  return true;
}

// Build a UsdGeomPointInstancer per-instance object->world matrix in the
// row-vector convention (p' = p * M): scale, then orient, then translate, all in
// the instancer's local space (USD's instance transform order). `quat_wxyz` is the
// orientation as the NEXT stage stores it: REAL-FIRST (w, x, y, z). Crate is
// imaginary-first on disk and the reader swizzles on load (crate-reader-unpack.cc),
// so reading these four floats as (x,y,z,w) turns a 30-degree Z rotation into a
// 150-degree X rotation -- it flips the instance upside down. `scale3`/`pos` are
// per-axis scale and translation.
matrix4d InstanceTRS(const float *pos, const float *quat_wxyz,
                     const float *scale3) {
  lightusd::value::quatf q;
  q.real = quat_wxyz[0];
  q.imag[0] = quat_wxyz[1];
  q.imag[1] = quat_wxyz[2];
  q.imag[2] = quat_wxyz[3];
  // 3x3 rotation in the same convention as the rest of the xform stack.
  lightusd::value::matrix3d rot = lightusd::to_matrix3x3(q);
  // p * S * R with S diagonal scales row i of R by scale[i].
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) rot.m[i][j] *= double(scale3[i]);
  lightusd::value::double3 t{double(pos[0]), double(pos[1]), double(pos[2])};
  return lightusd::to_matrix(rot, t);  // translation into row 3
}

// Recursively collect curve prims under `prim`, accumulating world transforms in
// the row-vector convention. Used both at scene level and to gather a
// PointInstancer prototype's curves (relative to the prototype root).
void CollectCurvesNextRec(const lightusd::next::UsdPrim &prim,
                          const matrix4d &parent_world,
                          lightusd::Purpose inherited_purpose, double time,
                          std::vector<CurveJobNext> *out) {
  if (!prim.IsActive()) return;  // inactive prim + its subtree are pruned
  // visibility="invisible" prunes the prim and its subtree (parity with the
  // legacy path and UsdGeomImageable).
  if (const lightusd::next::Value *vv = prim.GetPropertyValue("visibility")) {
    if (const std::string *t = vv->as_token()) {
      if (*t == "invisible") return;
    }
  }
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);
  lightusd::Purpose purpose = inherited_purpose;
  if (const lightusd::next::Value *pv = prim.GetPropertyValue("purpose")) {
    if (const std::string *t = pv->as_token()) {
      if (*t == "render") purpose = lightusd::Purpose::Render;
      else if (*t == "proxy") purpose = lightusd::Purpose::Proxy;
      else if (*t == "guide") purpose = lightusd::Purpose::Guide;
    }
  }
  // Nested instancers under a prototype are NOT baked into its curve BLAS -- their
  // (curve) instancing is flattened separately, like the mesh path (else the nested
  // instancer's scatter collapses to a single curve copy). No-op for plain curve
  // prototypes.
  if (prim.GetTypeName() == "PointInstancer") return;
  {
    const lightusd::next::PrimSpec *s = prim.GetPrimSpec();
    if (s && !s->meta().instance_prototype().empty()) return;
  }
  if (IsCurvePrimNext(prim)) {
    CurveJobNext cj;
    cj.prim = prim;
    cj.world = world;
    cj.purpose = purpose;
    out->push_back(std::move(cj));
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren())
    CollectCurvesNextRec(child, world, purpose, time, out);
}

// Collect a PointInstancer prototype's curves with transforms relative to the
// prototype root (root at identity, replaced by the instance transform).
void CollectProtoCurves(const lightusd::next::Stage &stage,
                        const std::string &proto_path,
                        lightusd::Purpose start_purpose, double time,
                        std::vector<CurveJobNext> *out) {
  lightusd::next::UsdPrim proto = stage.GetPrimAtPath(proto_path);
  if (!proto.IsValid()) return;
  if (IsCurvePrimNext(proto)) {
    CurveJobNext cj;
    cj.prim = proto;
    cj.world = matrix4d::identity();
    cj.purpose = start_purpose;
    out->push_back(std::move(cj));
  }
  for (const lightusd::next::UsdPrim &child : proto.GetChildren())
    CollectCurvesNextRec(child, matrix4d::identity(), start_purpose, time, out);
}

// Reserve (deduped by path+purpose) a curve BLAS for a prototype's curves, shared
// by PointInstancer and native-instance placements. Returns its index in
// curve_inst->protos, or -1 if curve_inst is null or the prototype has no curves.
int32_t ReserveCurveProto(const lightusd::next::Stage &stage,
                          const std::string &proto_path,
                          lightusd::Purpose purpose, double time,
                          CurveProtoCollect *curve_inst) {
  if (!curve_inst) return -1;
  const std::string key =
      proto_path + "\x1f" + std::to_string(PurposeBit(purpose));
  auto it = curve_inst->ids.find(key);
  if (it != curve_inst->ids.end()) return int32_t(it->second);
  std::vector<CurveJobNext> probe;
  CollectProtoCurves(stage, proto_path, purpose, time, &probe);
  if (probe.empty()) return -1;
  const uint32_t idx = uint32_t(curve_inst->protos.size());
  curve_inst->ids[key] = idx;
  curve_inst->protos.push_back({proto_path, purpose, 0});
  return int32_t(idx);
}

// Expand a UsdGeomPointInstancer into TLAS placements: each prototype becomes a
// deduped BLAS (shared with the native-instance pool) and every visible instance
// becomes an InstanceRT placing that BLAS at scale*orient*translate composed with
// the instancer's world transform. Prototype paths come from the `prototypes`
// relationship; they are normally descendants of the instancer, so we resolve
// each target by leaf name among the instancer's children first (robust to
// whether composition re-rooted the authored target paths) and fall back to an
// absolute stage lookup. `invisibleIds`/`inactiveIds` are skipped. The
// instancer's children are the prototypes, so the caller must NOT descend into
// it. Curve prototypes are deduped into a curve BLAS and instanced through the
// same TLAS as meshes.
void CollectPointInstancer(const lightusd::next::Stage &stage,
                           const lightusd::next::UsdPrim &instancer,
                           const matrix4d &instancer_world,
                           lightusd::Purpose purpose, double time,
                           const std::vector<std::string> &mask,
                           std::vector<InstanceRT> *instances,
                           std::unordered_map<std::string, uint32_t> *proto_ids,
                           std::vector<ProtoBuildReq> *protos,
                           CurveProtoCollect *curve_inst, RTPreviewStats *stats,
                           // When set, curve placements go here (in `instancer_world`
                           // space) instead of curve_inst->instances -- used to
                           // capture a NESTED instancer's curve placements per
                           // prototype for later flattening. Curve prototypes are
                           // still deduped into curve_inst.
                           std::vector<CurveInstanceRT> *curve_out) {
  if (!PathMatchesMask(instancer.GetPath().str(), mask)) return;
  const std::vector<lightusd::next::Path> *targets =
      instancer.GetRelationship("prototypes");
  if (!targets || targets->empty()) return;

  // Resolve each prototype target to a live stage prim and reserve its mesh BLAS
  // id (deduped by path + purpose, matching the native-instance path) and, if the
  // prototype has curves, a curve BLAS id (also deduped) — both stored once and
  // instanced via the TLAS rather than baked per instance.
  std::unordered_map<std::string, lightusd::next::UsdPrim> children_by_name;
  for (const lightusd::next::UsdPrim &c : instancer.GetChildren())
    children_by_name.emplace(c.GetName(), c);
  std::vector<int32_t> proto_blas(targets->size(), -1);
  std::vector<int32_t> proto_curve(targets->size(), -1);  // CurveProtoCollect idx
  for (size_t pi = 0; pi < targets->size(); ++pi) {
    const lightusd::next::Path &tp = (*targets)[pi];
    lightusd::next::UsdPrim proto;
    auto cit = children_by_name.find(tp.name());
    if (cit != children_by_name.end()) proto = cit->second;
    else proto = stage.GetPrimAtPath(tp.str());
    if (!proto.IsValid()) continue;
    const std::string proto_path = proto.GetPath().str();
    const std::string key =
        proto_path + "\x1f" + std::to_string(PurposeBit(purpose));
    auto it = proto_ids->find(key);
    if (it == proto_ids->end()) {
      const uint32_t blas_id = uint32_t(protos->size()) + 1;  // blas[0] = base
      (*proto_ids)[key] = blas_id;
      protos->push_back({proto_path, purpose, blas_id});
      proto_blas[pi] = int32_t(blas_id);
    } else {
      proto_blas[pi] = int32_t(it->second);
    }
    proto_curve[pi] =
        ReserveCurveProto(stage, proto_path, purpose, time, curve_inst);
  }

  // Per-instance arrays. `positions` drives the instance count; the rest default
  // (identity orientation, unit scale, proto 0) when absent or shorter.
  const std::vector<float> positions =
      ReadFloatArrayLazy(instancer, "positions", time);
  if (positions.empty()) return;
  const size_t n = positions.size() / 3;
  const std::vector<int32_t> proto_indices =
      ReadIntArrayLazy(instancer, "protoIndices", time);
  const std::vector<float> orientations =
      ReadFloatArrayLazy(instancer, "orientations", time);
  const std::vector<float> scales = ReadFloatArrayLazy(instancer, "scales", time);
  const std::vector<int64_t> invisible =
      ReadInt64ArrayLazy(instancer, "invisibleIds", time);
  const std::vector<int64_t> inactive =
      ReadInt64ArrayLazy(instancer, "inactiveIds", time);
  const std::vector<int64_t> ids = ReadInt64ArrayLazy(instancer, "ids", time);
  std::unordered_set<int64_t> hidden_set(invisible.begin(), invisible.end());
  hidden_set.insert(inactive.begin(), inactive.end());

  static const float kIdentQuat[4] = {1.0f, 0.0f, 0.0f, 0.0f};  // real-first
  static const float kUnitScale[3] = {1.0f, 1.0f, 1.0f};
  size_t emitted = 0;
  for (size_t i = 0; i < n; ++i) {
    if (PointInstanceHidden(i, n, ids, hidden_set)) continue;
    const int32_t pidx = (i < proto_indices.size()) ? proto_indices[i] : 0;
    if (pidx < 0 || size_t(pidx) >= proto_blas.size()) continue;
    const int32_t blas_id = proto_blas[size_t(pidx)];
    const int32_t curve_idx = proto_curve[size_t(pidx)];
    if (blas_id < 0 && curve_idx < 0) continue;
    const float *q = (orientations.size() >= (i + 1) * 4) ? &orientations[i * 4]
                                                          : kIdentQuat;
    const float *s =
        (scales.size() >= (i + 1) * 3) ? &scales[i * 3] : kUnitScale;
    const matrix4d inst_world =
        InstanceTRS(&positions[i * 3], q, s) * instancer_world;
    float o2w[12];
    Mat4ToObj2World(inst_world, o2w);
    if (blas_id >= 0) {
      InstanceRT inst;
      inst.blas_id = uint32_t(blas_id);
      std::memcpy(inst.o2w, o2w, sizeof(o2w));
      instances->push_back(inst);
    }
    if (curve_idx >= 0 && curve_inst) {
      CurveInstanceRT ci;
      ci.curve_proto_idx = uint32_t(curve_idx);
      std::memcpy(ci.o2w, o2w, sizeof(o2w));
      (curve_out ? *curve_out : curve_inst->instances).push_back(ci);
    }
    emitted++;
  }
  if (stats) {
    stats->point_instancers++;
    stats->point_instances += emitted;
  }
}

// Walk the composed stage, splitting it into (a) base mesh jobs — geometry not
// under any native instance, emitted in world space — and (b) instance
// placements that reference a per-prototype BLAS. Native instances (prims with
// instance_prototype set) and UsdGeomPointInstancer prims are NOT descended
// into; instead each placement is recorded as an InstanceRT and its prototype
// (deduped by path+purpose) is queued for a BLAS build. This keeps each
// prototype's geometry stored once. Honors `mask`.
void CollectSceneSplit(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &prim,
                       const matrix4d &parent_world,
                       lightusd::Purpose inherited_purpose, double time,
                       const std::vector<std::string> &mask,
                       std::vector<MeshJobNext> *base_jobs,
                       std::vector<InstanceRT> *instances,
                       std::unordered_map<std::string, uint32_t> *proto_ids,
                       std::vector<ProtoBuildReq> *protos,
                       std::vector<CurveJobNext> *curve_jobs,
                       CurveProtoCollect *curve_inst, RTPreviewStats *stats,
                       const std::unordered_set<std::string> *proto_holders) {
  if (!prim.IsActive()) return;  // inactive prim + its subtree are pruned
  // visibility="invisible" prunes the prim and its subtree (parity with the
  // legacy path and UsdGeomImageable).
  if (const lightusd::next::Value *vv = prim.GetPropertyValue("visibility")) {
    if (const std::string *t = vv->as_token()) {
      if (*t == "invisible") return;
    }
  }
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  lightusd::Purpose purpose = inherited_purpose;
  if (const lightusd::next::Value *pv = prim.GetPropertyValue("purpose")) {
    if (const std::string *t = pv->as_token()) {
      if (*t == "render") purpose = lightusd::Purpose::Render;
      else if (*t == "proxy") purpose = lightusd::Purpose::Proxy;
      else if (*t == "guide") purpose = lightusd::Purpose::Guide;
    }
  }

  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  std::string proto_path = spec ? spec->meta().instance_prototype() : std::string();
  // A prototype HOLDER (the target of some instance_prototype) is itself a placed
  // instanceable prim -- Pixar renders every instanceable sibling, including the one
  // composition picked as the prototype source. So emit it as an instance of its OWN
  // geometry (keyed by its own path, the same BLAS its siblings reference) rather
  // than skipping it. Its subtree is the prototype geometry (collected once via
  // CollectProtoJobs), so do not descend. (Without this, one of N instanceable
  // siblings -- e.g. one of isIronwoodA1's two trees -- silently vanished.)
  if (proto_path.empty() && proto_holders &&
      proto_holders->count(prim.GetPath().str())) {
    proto_path = prim.GetPath().str();
  }
  if (!proto_path.empty()) {
    // Native instance (or self-instancing holder): record placement + queue its
    // prototype. Do not descend (the instance proxy's children come from the
    // prototype).
    if (PathMatchesMask(prim.GetPath().str(), mask)) {
      const std::string key =
          proto_path + "\x1f" + std::to_string(PurposeBit(purpose));
      auto it = proto_ids->find(key);
      uint32_t blas_id;
      if (it == proto_ids->end()) {
        blas_id = uint32_t(protos->size()) + 1;  // blas[0] is the base scene
        (*proto_ids)[key] = blas_id;
        protos->push_back({proto_path, purpose, blas_id});
      } else {
        blas_id = it->second;
      }
      float o2w[12];
      Mat4ToObj2World(world, o2w);
      InstanceRT inst;
      inst.blas_id = blas_id;
      std::memcpy(inst.o2w, o2w, sizeof(o2w));
      instances->push_back(inst);
      // Curves under the prototype: place them per native instance via a deduped
      // curve BLAS (the prototype's own copy is collected once as base geometry).
      const int32_t curve_idx =
          ReserveCurveProto(stage, proto_path, purpose, time, curve_inst);
      if (curve_idx >= 0 && curve_inst) {
        CurveInstanceRT ci;
        ci.curve_proto_idx = uint32_t(curve_idx);
        std::memcpy(ci.o2w, o2w, sizeof(o2w));
        curve_inst->instances.push_back(ci);
      }
    }
    return;
  }

  // UsdGeomPointInstancer: expand into TLAS placements (one BLAS per prototype,
  // shared with the native-instance pool) plus instanced curves. Its children are
  // the prototypes, so do not descend (that would emit each prototype once,
  // un-instanced).
  if (prim.GetTypeName() == "PointInstancer") {
    CollectPointInstancer(stage, prim, world, purpose, time, mask, instances,
                          proto_ids, protos, curve_inst, stats);
    return;
  }

  if (prim.GetTypeName() == "Mesh" &&
      PathMatchesMask(prim.GetPath().str(), mask)) {
    MeshJobNext job;
    job.prim = prim;
    job.world = world;
    job.purpose = purpose;
    base_jobs->push_back(std::move(job));
  } else if (curve_jobs && IsCurvePrimNext(prim) &&
             PathMatchesMask(prim.GetPath().str(), mask)) {
    CurveJobNext cj;
    cj.prim = prim;
    cj.world = world;
    cj.purpose = purpose;
    curve_jobs->push_back(std::move(cj));
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    CollectSceneSplit(stage, child, world, purpose, time, mask, base_jobs,
                      instances, proto_ids, protos, curve_jobs, curve_inst,
                      stats, proto_holders);
  }
}

// Recurse a prototype subtree collecting its NESTED instance placements (nested
// PointInstancer expansions + scenegraph-instanceable instances), recorded in
// prototype-LOCAL space. Each placement references a deduped leaf-prototype BLAS
// queued into the shared proto_ids/protos pool, so deeper nesting is collected on
// later iterations of the proto loop and composed by the TLAS expansion. Mesh-only:
// nested instanced curves are routed to a throwaway collector (their geometry still
// renders once via the per-prototype curve BLAS). Mirrors the instance branches of
// CollectSceneSplit; base meshes are left to CollectProtoJobs.
void CollectProtoMeshNestingRec(
    const lightusd::next::Stage &stage, const lightusd::next::UsdPrim &prim,
    const matrix4d &parent_world, lightusd::Purpose inherited_purpose, double time,
    const std::vector<std::string> &mask, std::vector<InstanceRT> *nested,
    std::vector<CurveInstanceRT> *nested_curves,
    std::unordered_map<std::string, uint32_t> *proto_ids,
    std::vector<ProtoBuildReq> *protos, CurveProtoCollect *curve_inst,
    RTPreviewStats *stats,
    const std::unordered_set<std::string> *proto_holders) {
  if (!prim.IsActive()) return;  // inactive prim + its subtree are pruned
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  lightusd::Purpose purpose = inherited_purpose;
  if (const lightusd::next::Value *pv = prim.GetPropertyValue("purpose")) {
    if (const std::string *t = pv->as_token()) {
      if (*t == "render") purpose = lightusd::Purpose::Render;
      else if (*t == "proxy") purpose = lightusd::Purpose::Proxy;
      else if (*t == "guide") purpose = lightusd::Purpose::Guide;
    }
  }

  // Nested scenegraph instance: record a placement + queue its prototype.
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  const std::string proto_path =
      spec ? spec->meta().instance_prototype() : std::string();
  if (!proto_path.empty()) {
    if (PathMatchesMask(prim.GetPath().str(), mask)) {
      const std::string key =
          proto_path + "\x1f" + std::to_string(PurposeBit(purpose));
      auto it = proto_ids->find(key);
      uint32_t blas_id;
      if (it == proto_ids->end()) {
        blas_id = uint32_t(protos->size()) + 1;
        (*proto_ids)[key] = blas_id;
        protos->push_back({proto_path, purpose, blas_id});
      } else {
        blas_id = it->second;
      }
      InstanceRT inst;
      inst.blas_id = blas_id;
      Mat4ToObj2World(world, inst.o2w);
      nested->push_back(inst);
      // Curves under the nested native instance's prototype, placed (proto-local)
      // via a deduped curve BLAS -- flattened with the outer placements later.
      const int32_t curve_idx =
          ReserveCurveProto(stage, proto_path, purpose, time, curve_inst);
      if (curve_idx >= 0 && nested_curves) {
        CurveInstanceRT ci;
        ci.curve_proto_idx = uint32_t(curve_idx);
        std::memcpy(ci.o2w, inst.o2w, sizeof(inst.o2w));
        nested_curves->push_back(ci);
      }
    }
    return;
  }

  // Nested PointInstancer: reuse the top-level expander, directing its mesh
  // placements into `nested` and curve placements into `nested_curves` (both
  // prototype-local); mesh + curve protos dedup into the shared pools.
  if (prim.GetTypeName() == "PointInstancer") {
    CollectPointInstancer(stage, prim, world, purpose, time, mask, nested,
                          proto_ids, protos, curve_inst, stats, nested_curves);
    return;
  }

  for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
    if (proto_holders && proto_holders->count(child.GetPath().str())) continue;
    CollectProtoMeshNestingRec(stage, child, world, purpose, time, mask, nested,
                               nested_curves, proto_ids, protos, curve_inst, stats,
                               proto_holders);
  }
}

// Entry: root the nested-instance walk at the prototype's CHILDREN with an identity
// world (the prototype root's own transform is replaced by each outer placement,
// matching CollectProtoJobs). Appends prototype-local placements to `nested`.
void CollectProtoMeshNesting(
    const lightusd::next::Stage &stage, const std::string &proto_path,
    lightusd::Purpose purpose, double time, const std::vector<std::string> &mask,
    std::vector<InstanceRT> *nested, std::vector<CurveInstanceRT> *nested_curves,
    std::unordered_map<std::string, uint32_t> *proto_ids,
    std::vector<ProtoBuildReq> *protos, CurveProtoCollect *curve_inst,
    RTPreviewStats *stats,
    const std::unordered_set<std::string> *proto_holders) {
  lightusd::next::UsdPrim proto = stage.GetPrimAtPath(proto_path);
  if (!proto.IsValid()) return;
  for (const lightusd::next::UsdPrim &child : proto.GetChildren()) {
    if (proto_holders && proto_holders->count(child.GetPath().str())) continue;
    CollectProtoMeshNestingRec(stage, child, matrix4d::identity(), purpose, time,
                               mask, nested, nested_curves, proto_ids, protos,
                               curve_inst, stats, proto_holders);
  }
}

// Pre-pass: gather the paths of native-instance prototype holders (the targets
// of instance_prototype()). Walks the same prims CollectSceneSplit collects as
// base geometry -- it stops at instance proxies and PointInstancers, so its cost
// is one base-graph traversal (no instance multiplicity), not the expanded set.
void CollectPrototypePaths(const lightusd::next::UsdPrim &prim,
                           std::unordered_set<std::string> *out) {
  if (!prim.IsActive()) return;  // inactive prim + its subtree are pruned
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  if (spec && !spec->meta().instance_prototype().empty()) {
    out->insert(spec->meta().instance_prototype());
    return;  // proxy children come from the prototype; don't descend
  }
  if (prim.GetTypeName() == "PointInstancer") return;
  for (const lightusd::next::UsdPrim &child : prim.GetChildren())
    CollectPrototypePaths(child, out);
}

// Collect a prototype's mesh jobs in prototype-LOCAL space (the holder prim at
// identity): traverse the holder's children with parent_world = identity, where
// GetChildren() transparently expands any nested instances inline (bounded —
// built once per unique prototype). The instance's world transform is applied
// later by the TLAS.
void CollectProtoJobs(const lightusd::next::Stage &stage,
                      const std::string &proto_path,
                      lightusd::Purpose start_purpose, double time,
                      std::vector<MeshJobNext> *jobs) {
  lightusd::next::UsdPrim proto = stage.GetPrimAtPath(proto_path);
  if (!proto.IsValid()) return;
  static const std::vector<std::string> kNoMask;
  // A prototype root is placed at identity (its own transform is replaced by the
  // instance transform), but if the prototype prim IS a Mesh (a PointInstancer
  // prototype can point straight at a Mesh) it must still be collected.
  if (proto.GetTypeName() == "Mesh") {
    MeshJobNext job;
    job.prim = proto;
    job.world = matrix4d::identity();
    job.purpose = start_purpose;
    jobs->push_back(std::move(job));
  }
  for (const lightusd::next::UsdPrim &child : proto.GetChildren()) {
    CollectRTPreviewMeshesNext(stage, child, matrix4d::identity(), start_purpose,
                               time, kNoMask, jobs);
  }
}

// Build one curve prototype into a curve BLAS (round hair) in prototype-LOCAL
// space, with one TriInfo per segment for hit resolution. The instance transform
// is applied later by the TLAS. Instanced curves are treated as round (the common
// XGen case; per-instance flat/ribbon curves are not separated). Returns false
// only on a LightRT build failure.
bool BuildCurveBlasUnbounded(const lightusd::next::Stage &stage,
                    const std::string &proto_path, lightusd::Purpose purpose,
                    double time, const lrt_tri_build_options &build_opts,
                    const lightusd::next::ValueClipStageLoader &clip_loader,
                    Blas *out, Bounds *local,
                    // Sub-BLAS split: a large curve prototype is split into
                    // several disjoint sub-BLAS so their (serial) LBVH collapses
                    // run CONCURRENTLY. The first sub-BLAS fills `out`/`local`;
                    // any extras are appended here (the caller places each as a
                    // TLAS instance at the prototype's transform). Null => never
                    // split (single BLAS, byte-identical to the old path).
                    std::vector<Blas> *extra_blas = nullptr,
                    std::vector<Bounds> *extra_bounds = nullptr) {
  std::vector<CurveJobNext> curves;
  CollectProtoCurves(stage, proto_path, purpose, time, &curves);
  if (curves.empty()) return true;
  std::vector<float> pts, radii;
  std::vector<uint32_t> first, count;
  // Curve endpoints (curve_seg) are derived directly from the transformed points
  // below, so pass info == null: AppendLinearCurveStrands skips the redundant
  // 120 B/segment TriInfo intermediate (its only payload here is p0/p1 -- already
  // in pts -- and the constant kCurveColor). On a 3 M-segment prototype that
  // removes ~360 MB of build + slim work that was the dominant serial cost.
  // The FULL prototype bounds (over all points). Every sub-BLAS reports this same
  // bounds so the TLAS / autoframe is byte-identical to the unsplit path: the
  // world bounds is the transformed-AABB hull, and a union of partial AABB hulls
  // is tighter than the full AABB's hull under a rotated transform (would reframe
  // the camera). Conservative per sub-BLAS but correct (geometry is within).
  Bounds proto_bounds;
  for (const CurveJobNext &job : curves) {
    CurvePointViewNext point_source;
    if (!ReadCurvePointViewNext(job.prim, time, clip_loader, &point_source))
      continue;
    std::vector<int32_t> c32 =
        ReadIntArrayLazy(job.prim, "curveVertexCounts", time);
    if (c32.empty()) continue;
    std::vector<int> c(c32.begin(), c32.end());
    std::vector<float> w = ReadFloatArrayLazy(job.prim, "widths", time);
    if (job.prim.GetTypeName() == "HermiteCurves") {
      lightusd::tydra::next::ValueArrayRead<float> tangents;
      const bool have_tangents = ReadFloatArrayViewLazy(
          job.prim, "tangents", time, &tangents);
      if (have_tangents && tangents.size() == point_source.view.size()) {
        AppendHermiteCurveStrands(
            point_source.view.begin(), tangents.begin(),
            point_source.view.size() / 3, c, w,
            HermiteTessellationSegmentsNext(), job.world, &pts, &radii,
            &first, &count, /*info=*/nullptr, &proto_bounds);
      } else {
        AppendLinearCurveStrands(point_source.view.begin(),
                                 point_source.view.size() / 3, c, w, job.world,
                                 &pts, &radii, &first, &count, /*info=*/nullptr,
                                 &proto_bounds);
      }
    } else {
      AppendLinearCurveStrands(point_source.view.begin(),
                               point_source.view.size() / 3, c, w, job.world,
                               &pts, &radii, &first, &count, /*info=*/nullptr,
                               &proto_bounds);
    }
  }
  if (first.empty()) return true;
  const TriMat kCurveMat = ExtractTriMat([] {
    TriInfo t;
    t.base_color = kCurveColor;
    return t;
  }());

  // Partition strands into groups of ~kCurveSplitSegs segments so each group's
  // BLAS build/collapse is a separate, concurrent job. Small prototypes (the
  // common case) stay a single BLAS -> byte-identical to the unsplit path.
  size_t total_segs = 0;
  for (uint32_t c : count) total_segs += size_t(c) - 1u;
  const size_t kCurveSplitSegs = size_t(1) << 20;  // ~1M segments/sub-BLAS
  std::vector<std::array<size_t, 2>> groups;  // {s0, s1}
  if (!extra_blas || total_segs <= kCurveSplitSegs) {
    groups.push_back({0, first.size()});
  } else {
    const size_t nsub = (total_segs + kCurveSplitSegs - 1) / kCurveSplitSegs;
    const size_t per = (total_segs + nsub - 1) / nsub;
    size_t s0 = 0, acc = 0;
    for (size_t s = 0; s < first.size(); s++) {
      acc += size_t(count[s]) - 1u;
      if (acc >= per && s + 1 < first.size()) {
        groups.push_back({s0, s + 1});
        s0 = s + 1;
        acc = 0;
      }
    }
    groups.push_back({s0, first.size()});
  }
  if (extra_blas && groups.size() > 1) {
    extra_blas->resize(groups.size() - 1);
    extra_bounds->resize(groups.size() - 1);
  }

  // Phase A (serial): for each group, derive its per-segment endpoints
  // (curve_seg) straight from the shared transformed points and cut its rebased
  // strand offsets. One material (kCurveMat) covers every segment. Points are NOT
  // copied: each sub-scene reads the shared pts/radii at a base offset.
  struct SubGeom {
    std::vector<uint32_t> sf, sc;  // rebased strand offsets/counts
    uint32_t pbase, npts;
    Blas *dst;
  };
  std::vector<SubGeom> subs(groups.size());
  for (size_t g = 0; g < groups.size(); ++g) {
    const size_t s0 = groups[g][0], s1 = groups[g][1];
    Blas *dst = g == 0 ? out : &(*extra_blas)[g - 1];
    *(g == 0 ? local : &(*extra_bounds)[g - 1]) = proto_bounds;  // full bounds
    SubGeom &sg = subs[g];
    sg.dst = dst;
    sg.pbase = first[s0];
    sg.npts = first[s1 - 1] + count[s1 - 1] - sg.pbase;
    sg.sf.resize(s1 - s0);
    sg.sc.resize(s1 - s0);
    size_t nseg_sub = 0;
    for (size_t s = s0; s < s1; s++) {
      sg.sf[s - s0] = first[s] - sg.pbase;
      sg.sc[s - s0] = count[s];
      nseg_sub += size_t(count[s]) - 1u;
    }
    dst->mat_table.push_back(kCurveMat);  // index 0 for every segment
    dst->curve_seg.reserve(nseg_sub * 6);
    dst->curve_seg_mat.assign(nseg_sub, 0u);
    for (size_t s = s0; s < s1; s++) {
      const size_t pf = first[s];  // global point base (pts is concatenated)
      for (uint32_t i = 0; i + 1u < count[s]; i++) {
        const float *a = &pts[(pf + i) * 3];
        const float *b = &pts[(pf + i + 1) * 3];
        dst->curve_seg.push_back(a[0]);
        dst->curve_seg.push_back(a[1]);
        dst->curve_seg.push_back(a[2]);
        dst->curve_seg.push_back(b[0]);
        dst->curve_seg.push_back(b[1]);
        dst->curve_seg.push_back(b[2]);
      }
    }
  }

  // Phase B (parallel): build each sub-BLAS's round-hair scene (its serial LBVH
  // collapse runs concurrently with the others). Reads the shared pts/radii.
  std::atomic<bool> ok{true};
  std::atomic<size_t> gcur{0};
  auto gw = [&]() {
    for (;;) {
      const size_t g = gcur.fetch_add(1, std::memory_order_relaxed);
      if (g >= subs.size()) break;
      SubGeom &sg = subs[g];
      lrt_hair_strands hs;
      std::memset(&hs, 0, sizeof(hs));
      hs.points = pts.data() + size_t(sg.pbase) * 3;
      hs.radius = radii.data() + sg.pbase;
      hs.strand_first = sg.sf.data();
      hs.strand_count = sg.sc.data();
      hs.nstrands = sg.sf.size();
      hs.npoints = sg.npts;
      lrt_result e = LRT_RESULT_OK;
      sg.dst->scene = lrt_roundcurve_scene_build(&hs, &build_opts, &e);
      sg.dst->is_curve = true;
      if (!sg.dst->scene) ok.store(false, std::memory_order_relaxed);
    }
  };
  const unsigned gt = std::min<unsigned>(
      WorkerThreadCount(int(build_opts.num_threads)), unsigned(subs.size()));
  if (gt <= 1) {
    gw();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(gt);
    for (unsigned t = 0; t < gt; ++t) pool.emplace_back(gw);
    for (std::thread &th : pool) th.join();
  }
  if (!ok.load()) {
    std::cerr << "Failed to build curve BLAS.\n";
    return false;
  }
  return true;
}

// Memory-bounded variant used by the render path.  BuildCurveBlasUnbounded is
// retained temporarily as a reference for the split/hit-index layout; this
// implementation avoids retaining the complete transformed prototype while
// LightRT builds its sub-BLASes.
bool BuildCurveBlas(const lightusd::next::Stage &stage,
                    const std::string &proto_path, lightusd::Purpose purpose,
                    double time, const lrt_tri_build_options &build_opts,
                    const lightusd::next::ValueClipStageLoader &clip_loader,
                    Blas *out, Bounds *local,
                    std::vector<Blas> *extra_blas = nullptr,
                    std::vector<Bounds> *extra_bounds = nullptr) {
  std::vector<CurveJobNext> curves;
  CollectProtoCurves(stage, proto_path, purpose, time, &curves);
  if (curves.empty()) return true;

  const TriMat curve_mat = ExtractTriMat([] {
    TriInfo t;
    t.base_color = kCurveColor;
    return t;
  }());
  size_t split_segments = SIZE_MAX;
  if (extra_blas) {
    // Keep the larger historical prototype default, but let the same knob used
    // by direct curves tighten prototype BLAS residency for constrained hosts.
    split_segments = size_t(1) << 20;
    if (const char *s = std::getenv("LUSDR_CURVE_CHUNK")) {
      char *end = nullptr;
      const unsigned long long n = std::strtoull(s, &end, 10);
      if (end != s && n > 0) split_segments = static_cast<size_t>(n);
    }
  }
  std::vector<float> batch_points, batch_radii;
  std::vector<uint32_t> batch_first, batch_count;
  size_t batch_segments = 0;
  Bounds prototype_bounds;
  bool have_blas = false;

  auto flush = [&]() -> bool {
    if (batch_first.empty()) return true;
    Blas *dst = out;
    if (have_blas) {
      if (!extra_blas || !extra_bounds) return false;
      extra_blas->emplace_back();
      extra_bounds->emplace_back();
      dst = &extra_blas->back();
    }
    dst->mat_table.push_back(curve_mat);
    dst->curve_seg.reserve(batch_segments * 6);
    dst->curve_seg_mat.assign(batch_segments, 0u);
    for (size_t s = 0; s < batch_first.size(); ++s) {
      const size_t first = batch_first[s];
      for (uint32_t i = 0; i + 1u < batch_count[s]; ++i) {
        const float *p0 = &batch_points[(first + i) * 3];
        const float *p1 = &batch_points[(first + i + 1u) * 3];
        dst->curve_seg.insert(dst->curve_seg.end(),
                              {p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]});
      }
    }
    lrt_hair_strands hair;
    std::memset(&hair, 0, sizeof(hair));
    hair.points = batch_points.data();
    hair.radius = batch_radii.data();
    hair.strand_first = batch_first.data();
    hair.strand_count = batch_count.data();
    hair.nstrands = batch_first.size();
    hair.npoints = batch_points.size() / 3;
    lrt_result result = LRT_RESULT_OK;
    dst->scene = lrt_roundcurve_scene_build(&hair, &build_opts, &result);
    dst->is_curve = true;
    if (!dst->scene) return false;
    have_blas = true;
    batch_points.clear();
    batch_radii.clear();
    batch_first.clear();
    batch_count.clear();
    batch_segments = 0;
    return true;
  };

  for (const CurveJobNext &job : curves) {
    CurvePointViewNext point_source;
    if (!ReadCurvePointViewNext(job.prim, time, clip_loader, &point_source))
      continue;
    const std::vector<int32_t> c32 =
        ReadIntArrayLazy(job.prim, "curveVertexCounts", time);
    if (c32.empty()) continue;
    const std::vector<int> counts(c32.begin(), c32.end());
    const std::vector<float> widths =
        ReadFloatArrayLazy(job.prim, "widths", time);
    // Materialize at most this source prim plus one bounded batch.  This is
    // important for clip-backed arrays: the view can be borrowed, while the
    // temporary vectors below are released before the next prim is decoded.
    std::vector<float> prim_points, prim_radii;
    std::vector<uint32_t> prim_first, prim_count;
    if (job.prim.GetTypeName() == "HermiteCurves") {
      lightusd::tydra::next::ValueArrayRead<float> tangents;
      const bool have_tangents = ReadFloatArrayViewLazy(
          job.prim, "tangents", time, &tangents);
      if (have_tangents && tangents.size() == point_source.view.size()) {
        AppendHermiteCurveStrands(
            point_source.view.begin(), tangents.begin(),
            point_source.view.size() / 3, counts, widths,
            HermiteTessellationSegmentsNext(), job.world, &prim_points,
            &prim_radii, &prim_first, &prim_count, /*info=*/nullptr,
            &prototype_bounds);
      } else {
        AppendLinearCurveStrands(
            point_source.view.begin(), point_source.view.size() / 3, counts,
            widths, job.world, &prim_points, &prim_radii, &prim_first,
            &prim_count, /*info=*/nullptr, &prototype_bounds);
      }
    } else {
      AppendLinearCurveStrands(
          point_source.view.begin(), point_source.view.size() / 3, counts,
          widths, job.world, &prim_points, &prim_radii, &prim_first,
          &prim_count, /*info=*/nullptr, &prototype_bounds);
    }
    for (size_t s = 0; s < prim_first.size(); ++s) {
      const size_t segments = size_t(prim_count[s]) - 1u;
      if (!batch_first.empty() && batch_segments + segments > split_segments &&
          !flush()) {
        std::cerr << "Failed to build curve BLAS.\n";
        return false;
      }
      const uint32_t base = uint32_t(batch_points.size() / 3);
      const size_t pbegin = size_t(prim_first[s]) * 3;
      const size_t pcount = size_t(prim_count[s]) * 3;
      batch_points.insert(batch_points.end(), prim_points.begin() + pbegin,
                         prim_points.begin() + pbegin + pcount);
      const size_t rbegin = prim_first[s];
      batch_radii.insert(batch_radii.end(), prim_radii.begin() + rbegin,
                         prim_radii.begin() + rbegin + prim_count[s]);
      batch_first.push_back(base);
      batch_count.push_back(prim_count[s]);
      batch_segments += segments;
    }
  }
  if (!flush()) {
    std::cerr << "Failed to build curve BLAS.\n";
    return false;
  }
  if (!have_blas) return true;
  *local = prototype_bounds;
  if (extra_bounds) {
    for (Bounds &bound : *extra_bounds) bound = prototype_bounds;
  }
  return true;
}

// Upper bound on the triangles a mesh job emits: the fan-triangulation count
// (sum of max(0, c-2) over faceVertexCounts). Invalid/degenerate/purpose-culled
// triangles only reduce the actual count, so this never under-reserves -- used to
// reserve the stream output up front so the chunked append never reallocates.
inline size_t EstimateTrisForJob(const lightusd::next::UsdPrim &prim,
                                 double time) {
  const std::vector<int32_t> counts =
      ReadIntArrayLazy(prim, "faceVertexCounts", time);
  size_t est = 0;
  for (int32_t c : counts)
    if (c >= 3) est += size_t(c - 2);
  return est;
}

// Stream a list of (material-resolved) mesh jobs into packed triangle buffers +
// a bounds, in parallel, appending in job order (deterministic). Geometry is
// emitted in each job's `world` space. `purpose_cull` drops purpose-invisible
// triangles at build time (TLAS path).
// Returns false if a memory-cap allocation failure (std::bad_alloc from
// PoolAlloc) interrupted streaming — the caller then aborts the render cleanly
// instead of letting the process get OOM-killed.
template <class FVec, class TVec>
bool StreamMeshJobs(const std::vector<MeshJobNext> &jobs, uint32_t purpose_mask,
                    const lightusd::next::Stage *stage, double time,
                    bool want_uvs, bool purpose_cull, int threads,
                    FVec *out_vertices, TVec *out_tris, FVec *out_tri_uvs,
                    Bounds *out_bounds, RTPreviewStats *out_stats,
                    std::vector<TriMat> *out_mat_table = nullptr,
                    std::vector<lightusd::tydra::LightRtOpenPBRParams>
                        *out_openpbr_table = nullptr,
                    bool want_colors = false, ByteVec *out_tri_colors = nullptr,
                    bool want_normals = false, FVec *out_tri_normals = nullptr,
                    // Indexed geometry (Phase 2b): when both non-null, emit unique
                    // verts + 3 indices/tri here instead of the de-indexed soup
                    // in out_vertices.
                    FVec *out_uverts = nullptr, IdxVec *out_indices = nullptr,
                    // Coarse displacement: per-vertex offset along the smooth normal
                    // by the resolved displacement (texture from tex_pool). disp_scale
                    // == 0 disables it (then renders are byte-identical to before).
                    const std::vector<Texture> *tex_pool = nullptr,
                    float disp_scale = 0.0f) {
  const bool indexed = (out_uverts && out_indices);
  struct R {
    FVec v;
    TVec t;
    FVec uv;
    ByteVec col;  // per-corner RGBA8 (12 bytes/tri) when want_colors
    FVec nrm;  // per-corner normals (9 floats/tri) when want_normals
    FVec uvv;  // unique verts (3 floats each) when indexed
    IdxVec idx;  // job-local vertex indices (3/tri) when indexed
    Bounds b;
    RTPreviewStats s;
    TriMat mat;  // this job's single material (slim TriStore path only)
    TriMat back_mat;
    bool has_back{false};
    bool has_openpbr{false};
    lightusd::tydra::LightRtOpenPBRParams openpbr;
  };
  const unsigned nthreads =
      std::min<unsigned>(WorkerThreadCount(threads),
                         jobs.empty() ? 1u : unsigned(jobs.size()));

  // Reserve the outputs from a triangle upper bound (so the appends never
  // reallocate), then stream in CHUNKS: only one chunk's per-job buffers are held
  // at a time and appended (in job order) into the reserved outputs before the
  // next chunk runs. This keeps the FULL per-job set and the concatenated copy
  // from ever coexisting -- the transient that drove the streaming-phase peak RSS
  // on big multi-mesh scenes (isCoral's base). Byte-identical to the old
  // all-jobs-then-concat: same job-order append, same content.
  size_t est_tris = 0;
  for (const MeshJobNext &job : jobs) est_tris += EstimateTrisForJob(job.prim, time);
  try {
    // Test the pointers directly (not the `indexed` bool) so the compiler's
    // -Wnonnull analysis can prove the dereferenced output is non-null -- it does
    // not propagate `indexed == (out_uverts && out_indices)` to these sites.
    if (out_uverts && out_indices)
      out_indices->reserve(out_indices->size() + est_tris * 3);
    else if (out_vertices)
      out_vertices->reserve(out_vertices->size() + est_tris * 9);
    out_tris->reserve(out_tris->size() + est_tris);
    if (want_uvs) out_tri_uvs->reserve(out_tri_uvs->size() + est_tris * 6);
    if (want_colors && out_tri_colors)
      out_tri_colors->reserve(out_tri_colors->size() + est_tris * 12);
    if (want_normals && out_tri_normals)
      out_tri_normals->reserve(out_tri_normals->size() + est_tris * 9);
  } catch (const std::bad_alloc &) {
    return false;
  }

  std::vector<R> results(jobs.size());
  std::atomic<bool> oom{false};
  const size_t njobs = jobs.size();
  const size_t chunk = std::max<size_t>(size_t(nthreads) * 2u, 1u);
  const std::unordered_map<std::string, float> blend_weights =
      stage ? GatherBlendShapeWeightsNext(*stage, time)
            : std::unordered_map<std::string, float>();
  try {
    for (size_t cstart = 0;
         cstart < njobs && !oom.load(std::memory_order_relaxed);
         cstart += chunk) {
      const size_t cend = std::min(cstart + chunk, njobs);
      std::atomic<size_t> cursor{cstart};
      // A bad_alloc must be caught INSIDE each worker thread (an exception
      // escaping a std::thread calls std::terminate); it signals the cap was hit.
      auto worker = [&]() {
        try {
          for (;;) {
            const size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= cend || oom.load(std::memory_order_relaxed)) break;
            const MeshJobNext &job = jobs[i];
            R &r = results[i];
            uint32_t jvb = 0;  // job-local vertex base (indices rebased at concat)
            AddRTPreviewMeshNext(
                job.prim, stage, &blend_weights, job.world, job.purpose,
                purpose_mask, time,
                job.base_color, job.tex_id, job.normal_tex_id, job.roughness,
                job.metallic, job.rough_tex, job.metal_tex, job.emission,
                job.emission_tex_id, job.occlusion, job.occ_tex,
                job.opacity_tex, job.opacity_threshold, job.clearcoat,
                job.clearcoat_roughness, job.clearcoat_tex,
                job.clearcoat_rough_tex, job.specular_color,
                job.specular_tex_id, job.ior, job.use_specular_workflow,
                job.uv_xform,
                want_uvs, &r.v, &r.t, &r.uv, &r.b, &r.s, job.uv_primvar,
                job.subset_faces.empty() ? nullptr : &job.subset_faces,
                purpose_cull, &r.mat,
                job.opacity, want_colors, &r.col, want_normals, &r.nrm,
                indexed ? &r.uvv : nullptr, indexed ? &r.idx : nullptr,
                indexed ? &jvb : nullptr, job.displacement,
                job.displacement_tex.id, job.displacement_tex.ch, tex_pool,
                disp_scale, job.displacement_tex.scale,
                job.displacement_tex.bias);
            r.mat.capture_prim_id = CapturePrimId(job.prim.GetPath().str());
            r.has_openpbr = job.has_openpbr;
            r.openpbr = job.openpbr;
            if (job.back_material) {
              r.back_mat = TriMatFromResolved(*job.back_material);
              r.back_mat.capture_prim_id = r.mat.capture_prim_id;
              r.has_back = true;
              if constexpr (std::is_same<typename TVec::value_type,
                                         FlatTri>::value) {
                // An explicit back-face binding makes that side visible even
                // when doubleSided was otherwise false.
                for (FlatTri &ft : r.t) ft.double_sided = 1;
              }
            }
          }
        } catch (const std::bad_alloc &) {
          oom.store(true, std::memory_order_relaxed);
        }
      };
      const unsigned cn =
          std::min<unsigned>(nthreads, unsigned(cend - cstart));
      if (cn <= 1) {
        worker();
      } else {
        std::vector<std::thread> pool;
        pool.reserve(cn);
        for (unsigned t = 0; t < cn; ++t) pool.emplace_back(worker);
        for (std::thread &th : pool) th.join();
      }
      if (oom.load(std::memory_order_relaxed)) break;
      // Append this chunk in job order into the (reserved) outputs; free as we go.
      for (size_t i = cstart; i < cend; ++i) {
        R &r = results[i];
        // Direct pointer test (== indexed) so -Wnonnull can prove the appends below
        // dereference non-null outputs.
        if (out_uverts && out_indices) {
          // Rebase this job's local vertex indices by the BLAS-global vertex
          // count, then append its unique verts. Byte-identical triangle set to
          // the soup path (same vertices, same per-tri order).
          const uint32_t base = uint32_t(out_uverts->size() / 3);
          out_uverts->insert(out_uverts->end(), r.uvv.begin(), r.uvv.end());
          for (uint32_t id : r.idx) out_indices->push_back(base + id);
        } else if (out_vertices) {
          out_vertices->insert(out_vertices->end(), r.v.begin(), r.v.end());
        }
        // Slim store: assign each of this job's triangles a global material id and
        // append the job's material to the shared table (one entry per job). Both
        // the instanced (TriStore) and flat (FlatTri) slim records carry a mat_id.
        if constexpr (std::is_same<typename TVec::value_type, TriStore>::value ||
                      std::is_same<typename TVec::value_type, FlatTri>::value) {
          if (out_mat_table) {
            if (out_openpbr_table && r.has_openpbr) {
              r.mat.openpbr_id = uint32_t(out_openpbr_table->size());
              out_openpbr_table->push_back(r.openpbr);
            }
            const uint32_t mid = uint32_t(out_mat_table->size());
            if (r.has_back) {
              r.mat.backface_id = mid + 1;
              if (out_openpbr_table && jobs[i].back_material->has_openpbr) {
                r.back_mat.openpbr_id = uint32_t(out_openpbr_table->size());
                out_openpbr_table->push_back(jobs[i].back_material->openpbr);
              }
            }
            out_mat_table->push_back(r.mat);
            if (r.has_back) out_mat_table->push_back(r.back_mat);
            for (auto &ts : r.t) ts.mat_id = mid;
          }
        }
        out_tris->insert(out_tris->end(), r.t.begin(), r.t.end());
        if (want_uvs)
          out_tri_uvs->insert(out_tri_uvs->end(), r.uv.begin(), r.uv.end());
        if (want_colors && out_tri_colors)
          out_tri_colors->insert(out_tri_colors->end(), r.col.begin(),
                                 r.col.end());
        if (want_normals && out_tri_normals)
          out_tri_normals->insert(out_tri_normals->end(), r.nrm.begin(),
                                  r.nrm.end());
        MergeBounds(out_bounds, r.b);
        MergeStats(out_stats, r.s);
        FVec().swap(r.v);
        TVec().swap(r.t);
        FVec().swap(r.uv);
        ByteVec().swap(r.col);
        FVec().swap(r.nrm);
        FVec().swap(r.uvv);
        IdxVec().swap(r.idx);
      }
    }
  } catch (const std::bad_alloc &) {
    return false;
  }
  if (oom.load(std::memory_order_relaxed)) return false;
  return true;
}

// Expand `g` by a local AABB transformed by a 3x4 object->world (8 corners).
void ExpandBoundsByTransformedO2W(Bounds *g, const Bounds &local,
                                  const float o2w[12]) {
  if (!local.valid) return;
  for (int c = 0; c < 8; ++c) {
    Vec3 corner{(c & 1) ? local.hi.x : local.lo.x,
                (c & 2) ? local.hi.y : local.lo.y,
                (c & 4) ? local.hi.z : local.lo.z};
    Expand(g, TransformPointO2W(o2w, corner));
  }
}

// (Re)stream triangles at `time` and (re)build the BVH. Safe to call repeatedly
// (e.g. once per animation frame): frees the previous BVH and clears the
// previous geometry first. Honors ctx.opt.mask. Geometry/transforms are
// evaluated at `time` (NaN = default value). When the composed scene has native
// instances, builds a two-level BVH (per-prototype BLAS + TLAS) so instanced
// geometry is stored once; otherwise builds a single flat scene (byte-identical
// to the historical path).
bool ExtractAndBuildBVH(RenderContext &ctx, double time) {
  const Options &opt = ctx.opt;
  ctx.frame_time = time;
  if (ctx.tlas) {
    lrt_tlas_free(ctx.tlas);
    ctx.tlas = nullptr;
  }
  if (ctx.scene) {
    lrt_tri_scene_free(ctx.scene);
    ctx.scene = nullptr;
  }
  ctx.vertices.clear();
  ctx.tris.clear();
  ctx.flat_mats.clear();
  ctx.flat_openpbr_mats.clear();
  ctx.textures.clear();
  ctx.tri_uvs.clear();
  ctx.tri_colors.clear();
  ctx.tri_normals.clear();
  ctx.blas.clear();
  ctx.instances.clear();
  ctx.use_tlas = false;
  ctx.bounds = Bounds();
  ctx.stats = RTPreviewStats();
  ctx.direct.ellipses.reset();
  ctx.direct.ellipse_chunks.clear();
  ctx.triangle_chunks.clear();
  ctx.direct.round_curve_chunks.clear();
  ctx.direct.flat_curve_chunks.clear();
  const bool want_openpbr =
      opt.material_shading == Options::MaterialShading::LightRtBsdf;

  const auto stream_t0 = std::chrono::steady_clock::now();
  std::vector<MeshJobNext> base_jobs;
  std::vector<InstanceRT> instances;
  std::unordered_map<std::string, uint32_t> proto_ids;
  std::vector<ProtoBuildReq> protos;
  std::vector<CurveJobNext> curve_jobs;
  CurveProtoCollect curve_inst;
  // Gather native-instance prototype holders up front so the base-geometry
  // traversal can skip them (they are rendered via their instance proxies).
  std::unordered_set<std::string> proto_holders;
  lightusd::tydra::next::CollectPrototypePaths(ctx.stage, &proto_holders);
  for (const lightusd::next::UsdPrim &root : ctx.stage.GetRootPrims()) {
    CollectSceneSplit(ctx.stage, root, matrix4d::identity(),
                      lightusd::Purpose::Default, time, opt.mask, &base_jobs,
                      &instances, &proto_ids, &protos, &curve_jobs, &curve_inst,
                      &ctx.stats, &proto_holders);
  }
  if (!BuildNextGaussianEllipses(ctx.stage, ctx, time)) return false;
  // Curves (BasisCurves/HermiteCurves/NurbsCurves, plus any baked from curve-prototype
  // instancers) build into ctx.direct as LightRT hair scenes; RenderImage traces
  // them via the DirectScene path in both the flat and TLAS render modes.
  if (!BuildNextCurves(ctx, curve_jobs, time)) return false;
  ctx.stats.curve_strands = curve_jobs.size();
  // Curve extraction has copied the render-ready data into ctx.direct. The
  // authored prim handles/world transforms are no longer needed by this build
  // phase; releasing them before mesh material/BLAS work avoids carrying one
  // scene-wide job table alongside the acceleration data.
  std::vector<CurveJobNext>().swap(curve_jobs);

  lrt_tri_build_options build_opts;
  std::memset(&build_opts, 0, sizeof(build_opts));
  build_opts.quality = opt.quality;
  build_opts.layout = LRT_TRI_LAYOUT_AUTO;
  build_opts.max_leaf_size = 0;
  build_opts.num_threads = WorkerThreadCount(opt.threads);

  // Embedded textures: if the input is a .usdz package, open it so material
  // resolution can pull packed textures from the archive (else nullptr -> the
  // texture cache falls back to the filesystem).
  lightusd::next::USDZReader usdz_archive;
  const lightusd::next::USDZReader *usdz_ptr = nullptr;
  {
    const std::string &in = opt.input;
    if (in.size() >= 5 && in.compare(in.size() - 5, 5, ".usdz") == 0 &&
        usdz_archive.OpenFile(in)) {
      usdz_ptr = &usdz_archive;
    }
  }

  // -------------------------------------------------------------------------
  // Flat path: no native instances and no instanced curve prototypes. Identical
  // to the historical single-scene build (preserves byte-for-byte renders of
  // self-contained scenes).
  // -------------------------------------------------------------------------
  if (instances.empty() && curve_inst.instances.empty()) {
    ctx.stats.meshes = base_jobs.size();
    {
      TextureCache tc;
      tc.textures = &ctx.textures;
      tc.base_dir = DirName(opt.input);
      tc.usdz = usdz_ptr;
      tc.options = &opt;
      tc.degraded_materials = &ctx.stats.degraded_materials;
      tc.unsupported_mtlx = &ctx.stats.unsupported_mtlx;
      tc.material_diagnostic_examples = &ctx.stats.material_diagnostic_examples;
      tc.missing_textures = &ctx.stats.missing_textures;
      tc.texture_mip_fallbacks = &ctx.stats.texture_mip_fallbacks;
      std::unordered_map<std::string, ResolvedMat> mat_cache;
      // Per-face GeomSubset materials: split subset-bound meshes into one job
      // per subset BEFORE resolution, so each job resolves its own material.
      ExpandGeomSubsetJobsNext(ctx.stage, time, &base_jobs);
      ctx.stats.meshes = base_jobs.size();
      for (MeshJobNext &job : base_jobs) {
        ResolveMeshMaterialCached(ctx.stage, job.prim, tc, mat_cache, &job);
      }
    }
    UpdateTextureStats(ctx.textures, &ctx.stats);
    const bool want_uvs = !ctx.textures.empty();
    bool want_colors = false;
    for (const MeshJobNext &j : base_jobs)
      if (j.vertex_color) { want_colors = true; break; }
    const float disp_scale = opt.displace ? opt.displace_scale : 0.0f;
    if (!StreamMeshJobs(base_jobs, opt.purpose_mask, &ctx.stage, time, want_uvs,
                        /*purpose_cull=*/false, opt.threads, &ctx.vertices,
                        &ctx.tris, &ctx.tri_uvs, &ctx.bounds, &ctx.stats,
                        /*out_mat_table=*/&ctx.flat_mats,
                        want_openpbr ? &ctx.flat_openpbr_mats : nullptr,
                        want_colors,
                        &ctx.tri_colors, opt.smooth, &ctx.tri_normals,
                        /*out_uverts=*/static_cast<std::vector<float> *>(nullptr),
                        /*out_indices=*/static_cast<IdxVec *>(nullptr),
                        &ctx.textures, disp_scale)) {
      std::cerr << "Aborting: triangle stream exceeded memory cap "
                << MemBudget::GiB(MemBudget::Get().Cap())
                << ".\n  Raise -maxMem, restrict with -mask, or lower -complexity.\n";
      return false;
    }
    const auto stream_t1 = std::chrono::steady_clock::now();
    ctx.stream_seconds =
        std::chrono::duration<double>(stream_t1 - stream_t0).count();
    ctx.stats.build_seconds = ctx.stream_seconds;
    ctx.stats.packed_triangle_bytes =
        uint64_t(ctx.vertices.size()) * sizeof(float);
    const bool have_curves = ctx.direct.has_round_curves() ||
                             ctx.direct.has_flat_curves() ||
                             ctx.direct.bez_curves || ctx.direct.has_ellipses();
    if (ctx.tris.empty()) {
      if (have_curves) return true;  // curves-only scene: traced via DirectScene
      std::cerr << "RT preview (next) found no renderable Mesh triangles.\n";
      return false;
    }
    // LightRT builds its BVH outside our allocator: it copies the triangle
    // vertices into its own layout (~36 B/tri) plus nodes (~kBvhBytesPerTri/tri).
    // Guard the process RSS against the cap before committing to the build.
    std::string why;
    if (MemBudget::Get().WouldExceed(ctx.tris.size() * kBvhBytesPerTri, &why)) {
      std::cerr << "Aborting BVH build: " << why
                << ".\n  Raise -maxMem, restrict with -mask, or lower -complexity.\n";
      return false;
    }
    size_t chunk_limit = size_t(262144);
    if (const char *s = std::getenv("LUSDR_TRIANGLE_CHUNK")) {
      char *end = nullptr;
      const unsigned long long n = std::strtoull(s, &end, 10);
      if (end != s && n > 0) chunk_limit = static_cast<size_t>(n);
    }
    const auto bvh_t0 = std::chrono::steady_clock::now();
    for (size_t first = 0; first < ctx.tris.size(); first += chunk_limit) {
      const size_t count = std::min(chunk_limit, ctx.tris.size() - first);
      lrt_result lrt_err = LRT_RESULT_OK;
      TriangleSceneChunk chunk;
      chunk.first = first;
      chunk.count = count;
      chunk.scene.reset(lrt_tri_scene_build(
          ctx.vertices.data() + first * 9, count, &build_opts, &lrt_err));
      if (!chunk.scene) {
        std::cerr << "Failed to build LightRT triangle chunk [" << first << ", "
                  << (first + count) << "] (err=" << int(lrt_err)
                  << "). Try LUSDR_TRIANGLE_CHUNK.\n";
        return false;
      }
      ctx.triangle_chunks.push_back(std::move(chunk));
    }
    const auto bvh_t1 = std::chrono::steady_clock::now();
    ctx.bvh_seconds = std::chrono::duration<double>(bvh_t1 - bvh_t0).count();
    if (ctx.triangle_chunks.size() == 1) {
      // Preserve the established single-scene path and its diagnostics for
      // ordinary-sized inputs.
      ctx.scene = ctx.triangle_chunks.front().scene.release();
      ctx.triangle_chunks.clear();
    } else if (ctx.opt.stats) {
      std::cerr << "native triangle BVHs: " << ctx.triangle_chunks.size()
                << " chunk(s), triangle limit " << chunk_limit << "\n";
    }
    return true;
  }

  // -------------------------------------------------------------------------
  // Two-level (instanced) path: base BLAS + one BLAS per unique prototype,
  // placed by a TLAS. Geometry is stored once per prototype.
  // -------------------------------------------------------------------------
  ctx.use_tlas = true;
  // proto_jobs[i] = prototype i's base meshes; proto_nested[i] = its nested-instance
  // placements (prototype-local, referencing other prototype BLASes). `protos` GROWS
  // here as nested prototypes are queued, so iterate by index and extend the
  // parallel arrays. Copy path/purpose to locals first: collecting nesting can
  // reallocate `protos`, dangling a `protos[i]` reference mid-call.
  std::vector<std::vector<MeshJobNext>> proto_jobs;
  std::vector<std::vector<InstanceRT>> proto_nested;
  std::vector<std::vector<CurveInstanceRT>> proto_nested_curves;
  for (size_t i = 0; i < protos.size(); ++i) {
    const std::string ppath = protos[i].path;
    const lightusd::Purpose ppurpose = protos[i].purpose;
    std::vector<MeshJobNext> jobs;
    std::vector<InstanceRT> nested;
    std::vector<CurveInstanceRT> nested_curves;
    CollectProtoJobs(ctx.stage, ppath, ppurpose, time, &jobs);
    CollectProtoMeshNesting(ctx.stage, ppath, ppurpose, time, opt.mask, &nested,
                            &nested_curves, &proto_ids, &protos, &curve_inst,
                            &ctx.stats, &proto_holders);
    if (proto_jobs.size() < protos.size()) proto_jobs.resize(protos.size());
    if (proto_nested.size() < protos.size()) proto_nested.resize(protos.size());
    if (proto_nested_curves.size() < protos.size())
      proto_nested_curves.resize(protos.size());
    proto_jobs[i] = std::move(jobs);
    proto_nested[i] = std::move(nested);
    proto_nested_curves[i] = std::move(nested_curves);
  }
  // Material resolution over base + every prototype's meshes (shared cache).
  {
    TextureCache tc;
    tc.textures = &ctx.textures;
    tc.base_dir = DirName(opt.input);
    tc.usdz = usdz_ptr;
    tc.options = &opt;
    tc.degraded_materials = &ctx.stats.degraded_materials;
    tc.unsupported_mtlx = &ctx.stats.unsupported_mtlx;
    tc.material_diagnostic_examples = &ctx.stats.material_diagnostic_examples;
    tc.missing_textures = &ctx.stats.missing_textures;
    tc.texture_mip_fallbacks = &ctx.stats.texture_mip_fallbacks;
    std::unordered_map<std::string, ResolvedMat> mat_cache;
    // Per-face GeomSubset materials: split subset-bound meshes (base and
    // prototype alike) into one job per subset before resolution.
    ExpandGeomSubsetJobsNext(ctx.stage, time, &base_jobs);
    for (MeshJobNext &job : base_jobs) {
      ResolveMeshMaterialCached(ctx.stage, job.prim, tc, mat_cache, &job);
    }
    for (std::vector<MeshJobNext> &pj : proto_jobs) {
      ExpandGeomSubsetJobsNext(ctx.stage, time, &pj);
      for (MeshJobNext &job : pj) {
        ResolveMeshMaterialCached(ctx.stage, job.prim, tc, mat_cache, &job);
      }
    }
  }
  UpdateTextureStats(ctx.textures, &ctx.stats);
  const bool want_uvs = !ctx.textures.empty();
  // displayColor/Opacity is stored per-corner (48 B/tri) only for BLAS that
  // actually carry a *varying* (per-vertex/faceVarying/uniform) primvar. A BLAS
  // whose meshes are all constant-color needs no per-tri storage: the shader
  // falls back to the material base_color (which holds the constant displayColor),
  // so this is byte-identical while skipping the dominant Island footprint
  // (isCoral: ~800 MB of per-corner color across prototypes that don't vary).
  auto jobs_have_color = [](const std::vector<MeshJobNext> &jobs) {
    for (const MeshJobNext &j : jobs)
      if (j.vertex_color) return true;
    return false;
  };

  // Partition the (world-space, non-instanced) base geometry into ~2M-triangle
  // groups, each built as its OWN BLAS and placed as a TLAS instance at identity.
  // One 17M-tri base build allocates a huge BVH-scratch arena (2*ntris bnodes)
  // that dominates peak RSS; per-group builds keep that arena small (freed between
  // groups). Byte-identical: same world-space triangles, same closest hits (the
  // groups partition by mesh, so no triangle's coincidences are split).
  const size_t kBaseGroupTris = size_t(1) << 20;  // ~2M tris/group
  std::vector<std::vector<size_t>> base_group_idx;
  {
    std::vector<size_t> cur;
    size_t cur_tris = 0;
    for (size_t i = 0; i < base_jobs.size(); ++i) {
      const size_t e = EstimateTrisForJob(base_jobs[i].prim, time);
      if (!cur.empty() && cur_tris + e > kBaseGroupTris) {
        base_group_idx.push_back(std::move(cur));
        cur.clear();
        cur_tris = 0;
      }
      cur_tris += e;
      cur.push_back(i);
    }
    if (!cur.empty()) base_group_idx.push_back(std::move(cur));
    if (base_group_idx.empty()) base_group_idx.emplace_back();  // keep blas[0]
  }
  const size_t n_base_groups = base_group_idx.size();
  const size_t base_job_count = base_jobs.size();

  // blas layout: [0] base group 0, [1..P] mesh protos, [P+1..P+C] curve protos,
  // [P+C+1..] base groups 1..n-1 (appended so proto/curve ids stay stable).
  const size_t curve_base = 1 + protos.size();
  const size_t n_curve_protos = curve_inst.protos.size();
  ctx.blas.clear();
  ctx.blas.resize(curve_base + n_curve_protos + (n_base_groups - 1));
  std::vector<Bounds> local_bounds(ctx.blas.size());
  auto base_blas_id = [&](size_t g) -> uint32_t {
    return g == 0 ? 0u : uint32_t(curve_base + n_curve_protos + (g - 1));
  };

  // Stream base groups CONCURRENTLY, each into its own disjoint BLAS with internal
  // threads=1: isCoral's base is a few huge meshes, so the per-mesh threading
  // inside one StreamMeshJobs leaves most cores cold; running the groups across a
  // pool instead uses them. Byte-identical -- outputs are disjoint and per-group
  // stats are summed afterward (order-independent).
  std::vector<RTPreviewStats> gstats(n_base_groups);
  std::atomic<bool> gstream_ok{true};
  std::atomic<size_t> gcur{0};
  auto gworker = [&]() {
    for (;;) {
      const size_t g = gcur.fetch_add(1, std::memory_order_relaxed);
      if (g >= n_base_groups || !gstream_ok.load(std::memory_order_relaxed)) break;
      const uint32_t b = base_blas_id(g);
      std::vector<MeshJobNext> gjobs;
      gjobs.reserve(base_group_idx[g].size());
      for (size_t ji : base_group_idx[g])
        gjobs.push_back(std::move(base_jobs[ji]));
      if (!StreamMeshJobs(
              gjobs, opt.purpose_mask, &ctx.stage, time, want_uvs,
              /*purpose_cull=*/true,
              /*threads=*/1, &ctx.blas[b].vertices, &ctx.blas[b].tris,
              &ctx.blas[b].tri_uvs, &local_bounds[b], &gstats[g],
              &ctx.blas[b].mat_table,
              want_openpbr ? &ctx.blas[b].openpbr_table : nullptr,
              jobs_have_color(gjobs),
              &ctx.blas[b].tri_colors, opt.smooth, &ctx.blas[b].tri_normals,
              /*indexed:*/ &ctx.blas[b].uverts, &ctx.blas[b].indices,
              &ctx.textures, opt.displace ? opt.displace_scale : 0.0f))
        gstream_ok.store(false, std::memory_order_relaxed);
    }
  };
  const unsigned gthreads = std::min<unsigned>(
      WorkerThreadCount(opt.threads), unsigned(n_base_groups ? n_base_groups : 1));
  if (gthreads <= 1) {
    gworker();
  } else {
    std::vector<std::thread> gpool;
    gpool.reserve(gthreads);
    for (unsigned t = 0; t < gthreads; ++t) gpool.emplace_back(gworker);
    for (std::thread &th : gpool) th.join();
  }
  for (const RTPreviewStats &gs : gstats) MergeStats(&ctx.stats, gs);
  // Every base job has now been consumed by exactly one group. In particular,
  // release GeomSubset face masks here; those can be one byte per authored face
  // and otherwise overlap the subsequent prototype material/BLAS phase.
  std::vector<MeshJobNext>().swap(base_jobs);
  std::vector<std::vector<size_t>>().swap(base_group_idx);
  bool stream_ok = gstream_ok.load();
  // Each mesh prototype (local space) -> blas[blas_id].
  for (size_t i = 0; stream_ok && i < protos.size(); ++i) {
    const uint32_t b = protos[i].blas_id;
    RTPreviewStats discard;
    stream_ok = StreamMeshJobs(proto_jobs[i], opt.purpose_mask, &ctx.stage,
                               time, want_uvs, /*purpose_cull=*/true, opt.threads,
                               &ctx.blas[b].vertices, &ctx.blas[b].tris,
                               &ctx.blas[b].tri_uvs, &local_bounds[b], &discard,
                               &ctx.blas[b].mat_table,
                               want_openpbr ? &ctx.blas[b].openpbr_table
                                            : nullptr,
                               jobs_have_color(proto_jobs[i]),
                               &ctx.blas[b].tri_colors, opt.smooth,
                               &ctx.blas[b].tri_normals,
                               /*out_uverts=*/static_cast<FloatVec *>(nullptr),
                               /*out_indices=*/static_cast<IdxVec *>(nullptr),
                               &ctx.textures,
                               opt.displace ? opt.displace_scale : 0.0f);
    // The prototype's geometry and material data now live in ctx.blas[b].
    // Do not retain the authored job records while the remaining prototypes
    // build; this is significant for subset-heavy composed scenes.
    std::vector<MeshJobNext>().swap(proto_jobs[i]);
  }
  if (!stream_ok) {
    std::cerr << "Aborting: triangle stream exceeded memory cap "
              << MemBudget::GiB(MemBudget::Get().Cap())
              << ".\n  Raise -maxMem, restrict with -mask, or lower -complexity.\n";
    return false;
  }
  // Each curve prototype -> blas[curve_base + i] (its first sub-BLAS); a large
  // prototype is split into extra sub-BLAS appended at the end of ctx.blas so
  // their LBVH collapses build concurrently. curve_proto_blas[i] lists every
  // sub-BLAS id for prototype i, used below to place one TLAS instance per
  // sub-BLAS at the prototype's transform.
  std::vector<std::vector<uint32_t>> curve_proto_blas(curve_inst.protos.size());
  for (size_t i = 0; i < curve_inst.protos.size(); ++i) {
    const size_t b = curve_base + i;
    std::vector<Blas> extra;
    std::vector<Bounds> extra_b;
    if (!BuildCurveBlas(ctx.stage, curve_inst.protos[i].path,
                        curve_inst.protos[i].purpose, time, build_opts,
                        ctx.clip_stage_loader,
                        &ctx.blas[b], &local_bounds[b], &extra, &extra_b)) {
      return false;
    }
    curve_proto_blas[i].push_back(uint32_t(b));
    for (size_t j = 0; j < extra.size(); ++j) {
      curve_proto_blas[i].push_back(uint32_t(ctx.blas.size()));
      ctx.blas.push_back(std::move(extra[j]));
      local_bounds.push_back(extra_b[j]);
    }
  }
  const auto stream_t1 = std::chrono::steady_clock::now();
  ctx.stream_seconds =
      std::chrono::duration<double>(stream_t1 - stream_t0).count();

  // Guard the BLAS builds (LightRT allocates outside our pool): the build adds
  // ~kBvhBytesPerTri per UNIQUE triangle (prototypes stored once).
  {
    size_t unique_tris = 0;
    for (const Blas &b : ctx.blas) unique_tris += b.tris.size();
    std::string why;
    if (MemBudget::Get().WouldExceed(unique_tris * kBvhBytesPerTri, &why)) {
      std::cerr << "Aborting BVH build: " << why
                << ".\n  Raise -maxMem, restrict with -mask, or lower -complexity.\n";
      return false;
    }
  }

  // Build the BLAS with a size-split strategy: LARGE prototypes are built one at
  // a time but each with full intra-build threading (LightRT only parallelizes a
  // single build at >=4096 tris), while the many SMALL prototypes are built in
  // one batch parallelized ACROSS scenes (each single-threaded). Using the batch
  // for everything would force big BLAS single-threaded and regress heavily
  // instanced scenes; the serial loop alone leaves the small-BLAS fleet building
  // one-at-a-time.
  // Drop a BLAS's vertex soup (9 floats/tri) as soon as its BVH is built --
  // ResolveTLASHit recovers a hit triangle's object-space vertices from the BVH
  // leaves (lrt_tri_get_verts, byte-exact). Freeing each soup at build time
  // (rather than all at the end) keeps them from accumulating, so the build-phase
  // peak holds at most one large soup + the built BVHs instead of every soup at
  // once (isCoral ~600 MB off peak). Curve BLAS / unrecoverable leaves keep theirs.
  std::atomic<uint64_t> freed_soup_bytes{0};
  auto drop_soup = [&](size_t b) {
    if (ctx.blas[b].scene && !ctx.blas[b].is_curve &&
        lrt_tri_scene_has_verts(ctx.blas[b].scene)) {
      if (!ctx.blas[b].vertices.empty()) {
        freed_soup_bytes.fetch_add(
            uint64_t(ctx.blas[b].vertices.size()) * sizeof(float),
            std::memory_order_relaxed);
        FloatVec().swap(ctx.blas[b].vertices);
      }
      // Indexed BLAS: the leaf holds the de-indexed verts, so the unique-vertex
      // array + indices are no longer needed (lrt_tri_get_verts recovers hits).
      if (!ctx.blas[b].indices.empty()) {
        freed_soup_bytes.fetch_add(
            uint64_t(ctx.blas[b].uverts.size()) * sizeof(float) +
                uint64_t(ctx.blas[b].indices.size()) * sizeof(uint32_t),
            std::memory_order_relaxed);
        FloatVec().swap(ctx.blas[b].uverts);
        IdxVec().swap(ctx.blas[b].indices);
      }
    }
  };
  // Build a BLAS from whichever geometry form it streamed: indexed (uverts +
  // indices, the base groups) or de-indexed soup (everything else). The leaf is
  // identical either way (lrt_tri_scene_build_indexed gathers through indices).
  auto build_blas = [](Blas &bl, const lrt_tri_build_options *o,
                       lrt_result *e) -> lrt_tri_scene * {
    if (!bl.indices.empty())
      return lrt_tri_scene_build_indexed(bl.uverts.data(), bl.uverts.size() / 3,
                                         bl.indices.data(), bl.tris.size(), o, e);
    return lrt_tri_scene_build(bl.vertices.data(), bl.tris.size(), o, e);
  };
  const auto bvh_t0 = std::chrono::steady_clock::now();
  {
    const size_t nb = ctx.blas.size();
    const size_t kLargeTris = 32768;  // above this, intra-build threading wins
    // Pass 1: large BLAS. Build with BOUNDED concurrency -- kBuildPar builds run
    // at once, each with a few INTRA-build threads (kBuildPar * intra_t cores), so
    // the morton/radix/tree steps (tri_parallel_for) use the cores that a
    // single-threaded build leaves idle (the bvh phase otherwise ran ~3/32 cores).
    // kBuildPar bounds the coexisting build scratch to hold peak under Embree.
    // Byte-identical: each BLAS builds independently from its own geometry.
    std::vector<size_t> large;
    for (size_t b = 0; b < nb; ++b)
      if (ctx.blas[b].tris.size() >= kLargeTris) large.push_back(b);
    lrt_tri_build_options sbuild = build_opts;
    const unsigned kBuildPar = std::min<unsigned>(
        WorkerThreadCount(opt.threads), large.empty() ? 1u : 3u);  // cap for peak
    // Give each concurrent build a few intra-build threads (verified sweet spot
    // ~4: k=3 x 4 = 12 cores, bvh 1.7->1.5s, peak still ~180MB under Embree;
    // higher T tightens peak for little gain). Scales down on smaller machines.
    sbuild.num_threads = std::max(
        1u, std::min(4u, WorkerThreadCount(opt.threads) / kBuildPar));
    std::atomic<size_t> lcur{0};
    std::atomic<bool> lfail{false};
    auto lworker = [&]() {
      for (;;) {
        const size_t i = lcur.fetch_add(1, std::memory_order_relaxed);
        if (i >= large.size() || lfail.load(std::memory_order_relaxed)) break;
        const size_t b = large[i];
        lrt_result e = LRT_RESULT_OK;
        ctx.blas[b].scene = build_blas(ctx.blas[b], &sbuild, &e);
        if (!ctx.blas[b].scene) {
          lfail.store(true, std::memory_order_relaxed);
          break;
        }
        drop_soup(b);
      }
    };
    if (kBuildPar <= 1) {
      lworker();
    } else {
      std::vector<std::thread> lpool;
      lpool.reserve(kBuildPar);
      for (unsigned t = 0; t < kBuildPar; ++t) lpool.emplace_back(lworker);
      for (std::thread &th : lpool) th.join();
    }
    if (lfail.load()) {
      std::cerr << "Failed to build BLAS.\n";
      return false;
    }
    // Pass 2: small BLAS, batched across workers (bntris==0 skips large/empty).
    std::vector<const float *> bverts(nb, nullptr);
    std::vector<size_t> bntris(nb, 0);
    std::vector<lrt_tri_scene *> bscenes(nb, nullptr);
    std::vector<lrt_result> berrs(nb, LRT_RESULT_OK);
    for (size_t b = 0; b < nb; ++b) {
      const size_t nt = ctx.blas[b].tris.size();
      if (nt > 0 && nt < kLargeTris) {
        if (!ctx.blas[b].indices.empty()) {
          // Small indexed BLAS (not expected -- base groups are large -- but kept
          // correct): the soup batch can't consume indexed input, so build it now.
          lrt_result e = LRT_RESULT_OK;
          ctx.blas[b].scene = build_blas(ctx.blas[b], &build_opts, &e);
          if (!ctx.blas[b].scene) {
            std::cerr << "Failed to build BLAS (err=" << int(e) << ").\n";
            return false;
          }
          drop_soup(b);
        } else {
          bverts[b] = ctx.blas[b].vertices.data();
          bntris[b] = nt;
        }
      }
    }
    lrt_tri_scene_build_batch(bverts.data(), bntris.data(), nb, &build_opts,
                              bscenes.data(), berrs.data());
    for (size_t b = 0; b < nb; ++b) {
      if (bntris[b] > 0) {
        ctx.blas[b].scene = bscenes[b];
        if (!bscenes[b]) {
          std::cerr << "Failed to build BLAS (err=" << int(berrs[b]) << ").\n";
          return false;
        }
        drop_soup(b);
      }
    }
  }

  // Phase 5 (opt-in via LIGHTUSD_COHCOLOR): reorder each BLAS's per-corner colors
  // from prim_id order into BVH leaf-slot order, so hits within a leaf read
  // adjacent color records instead of scattered ones (the prim_id->slot map is
  // already touched by lrt_tri_get_verts at every hit). Per-BLAS scatter +
  // immediate free of the old array keeps the transient to one BLAS's colors.
  // Off by default: it adds a build-time scatter pass that isn't worth it for the
  // tiny primary-only preview render, but helps render-heavy (hi-res) use.
  // Byte-identical: same 12 bytes, relocated and read back through the same slot.
  if (std::getenv("LIGHTUSD_COHCOLOR")) {
    const size_t nb = ctx.blas.size();
    std::atomic<size_t> ccur{0};
    auto cworker = [&]() {
      for (;;) {
        const size_t b = ccur.fetch_add(1, std::memory_order_relaxed);
        if (b >= nb) break;
        Blas &bl = ctx.blas[b];
        if (bl.tri_colors.empty() || !bl.scene) continue;
        const uint32_t ns = lrt_tri_slot_count(bl.scene);
        if (!ns) continue;
        ByteVec cs(size_t(ns) * 12, 0);
        const size_t nt = bl.tris.size();
        for (size_t p = 0; p < nt; p++) {
          const uint32_t slot = lrt_tri_get_slot(bl.scene, uint32_t(p));
          if (slot != LRT_TRI_NO_HIT && size_t(slot) * 12 + 11 < cs.size())
            std::memcpy(&cs[size_t(slot) * 12], &bl.tri_colors[p * 12], 12);
        }
        bl.tri_colors_slot = std::move(cs);
        ByteVec().swap(bl.tri_colors);
      }
    };
    const unsigned cn = std::min<unsigned>(WorkerThreadCount(opt.threads),
                                           nb ? unsigned(nb) : 1u);
    if (cn <= 1) {
      cworker();
    } else {
      std::vector<std::thread> cp;
      cp.reserve(cn);
      for (unsigned t = 0; t < cn; ++t) cp.emplace_back(cworker);
      for (std::thread &th : cp) th.join();
    }
  }

  // Flatten nested instancing into the single level a TLAS expresses. A prototype
  // whose subtree contains instancers contributed nested placements (in that
  // prototype's local space, `proto_nested[blas-1]`). Each top-level placement of
  // such a prototype must ALSO place that prototype's nested geometry, composed with
  // the outer transform -- recursively, to any depth. Geometry stays deduped (each
  // leaf BLAS is stored once); only the 48 B/placement instance list grows. No-op
  // (byte-identical) when nothing nests.
  {
    const size_t nb = ctx.blas.size();
    bool any_nested = false;
    for (const std::vector<InstanceRT> &v : proto_nested)
      if (!v.empty()) { any_nested = true; break; }
    for (const std::vector<CurveInstanceRT> &v : proto_nested_curves)
      if (!v.empty()) { any_nested = true; break; }
    if (any_nested) {
      // Per-blas flattened nested placements (mesh + curve), in blas-local space.
      // flat[b]/flatC[b] = ALL geometry reachable through nested instancing under b.
      std::vector<std::vector<InstanceRT>> flat(nb);
      std::vector<std::vector<CurveInstanceRT>> flatC(nb);
      std::vector<uint8_t> visit(nb, 0);  // 0=unvisited 1=in-progress 2=done
      std::function<void(uint32_t)> build = [&](uint32_t b) {
        if (b >= nb || visit[b]) return;  // done -> cached; in-progress -> cycle
        visit[b] = 1;
        std::vector<InstanceRT> outM;
        std::vector<CurveInstanceRT> outC;
        const size_t pi = size_t(b) - 1;  // proto index for blas b (base=0: none)
        if (b >= 1) {
          if (pi < proto_nested_curves.size())  // curves directly nested in b
            for (const CurveInstanceRT &c : proto_nested_curves[pi]) outC.push_back(c);
          if (pi < proto_nested.size()) {
            for (const InstanceRT &d : proto_nested[pi]) {  // child placed at d.o2w
              const uint32_t cb = d.blas_id;
              if (cb < nb && ctx.blas[cb].scene) outM.push_back(d);  // child's base
              build(cb);
              if (cb < nb) {
                for (const InstanceRT &g : flat[cb]) {  // child's nested meshes
                  InstanceRT e;
                  e.blas_id = g.blas_id;
                  Compose3x4(d.o2w, g.o2w, e.o2w);  // apply g (cb-local) then d
                  outM.push_back(e);
                }
                for (const CurveInstanceRT &g : flatC[cb]) {  // child's nested curves
                  CurveInstanceRT e;
                  e.curve_proto_idx = g.curve_proto_idx;
                  Compose3x4(d.o2w, g.o2w, e.o2w);
                  outC.push_back(e);
                }
              }
            }
          }
        }
        flat[b] = std::move(outM);
        flatC[b] = std::move(outC);
        visit[b] = 2;
      };

      std::vector<InstanceRT> expanded;
      expanded.reserve(instances.size());
      size_t addedM = 0;
      // Guard the expanded instance arrays against the process memory cap before
      // materializing them (build() is memoized, so this pre-pass is cheap and the
      // expansion loop below reuses the cached flat[]/flatC[]). Nested instancing can
      // multiply the placement count (outer x nested), so a pathological scene could
      // blow the budget on 48 B/placement alone.
      {
        size_t projM = instances.size(), projC = curve_inst.instances.size();
        for (const InstanceRT &it : instances) {
          build(it.blas_id);
          if (it.blas_id < nb) {
            projM += flat[it.blas_id].size();
            projC += flatC[it.blas_id].size();
          }
        }
        std::string why;
        if (MemBudget::Get().WouldExceed(
                projM * sizeof(InstanceRT) + projC * sizeof(CurveInstanceRT),
                &why)) {
          std::cerr << "Aborting nested-instance expansion: " << why
                    << ".\n  Raise -maxMem or restrict with -mask.\n";
          return false;
        }
      }
      for (const InstanceRT &it : instances) {
        expanded.push_back(it);  // the prototype's own base at its outer transform
        build(it.blas_id);
        if (it.blas_id < nb) {
          for (const InstanceRT &g : flat[it.blas_id]) {
            InstanceRT e;
            e.blas_id = g.blas_id;
            Compose3x4(it.o2w, g.o2w, e.o2w);  // leaf-local -> world: apply g then it
            expanded.push_back(e);
            ++addedM;
          }
          for (const CurveInstanceRT &g : flatC[it.blas_id]) {
            CurveInstanceRT e;
            e.curve_proto_idx = g.curve_proto_idx;
            Compose3x4(it.o2w, g.o2w, e.o2w);
            curve_inst.instances.push_back(e);  // nested curve placed in world space
          }
        }
      }
      if (addedM) instances = std::move(expanded);
      ctx.stats.nested_instances = addedM;
      ctx.stats.curve_instances = curve_inst.instances.size();
    }
  }

  // Per-instance RT LOD (-rtLod): append a shared unit-cube [0,1]^3 box BLAS that
  // Proxy instances reference (box-fit onto each prototype's object AABB). Built
  // once; grey default material (TriMat defaults). Skipped unless -rtLod.
  uint32_t rt_lod_box_blas = UINT32_MAX;
  if (opt.rt_lod && opt.rt_lod_proxy) {
    static const float kC[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int kI[36] = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6,
                               0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6,
                               0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5};
    Blas box;
    box.vertices.reserve(36 * 3);
    for (int i = 0; i < 36; ++i)
      for (int k = 0; k < 3; ++k) box.vertices.push_back(kC[kI[i]][k]);
    box.tris.resize(12);  // TriStore{mat_id=0}; geometry lives in the lrt scene
    box.mat_table.resize(1);  // one default-grey TriMat
    lrt_result be = LRT_RESULT_OK;
    box.scene = lrt_tri_scene_build(box.vertices.data(), 12, &build_opts, &be);
    if (box.scene) {
      rt_lod_box_blas = uint32_t(ctx.blas.size());
      ctx.blas.push_back(std::move(box));
      Bounds bb; bb.lo = Vec3{0, 0, 0}; bb.hi = Vec3{1, 1, 1}; bb.valid = true;
      local_bounds.push_back(bb);
    } else {
      std::cerr << "rtLod: failed to build proxy box BLAS (err=" << int(be)
                << "); proxies disabled.\n";
    }
  }

  // LightRT tolerates NULL entries in the BLAS array (empty prototypes — e.g.
  // fully purpose-culled) as long as no instance references them, so the BLAS
  // index used by both the TLAS and shade-time ResolveTLASHit is just the
  // ctx.blas index — no compaction/remap needed.
  std::vector<lrt_tri_scene *> blas_ptrs(ctx.blas.size(), nullptr);
  for (size_t b = 0; b < ctx.blas.size(); ++b) blas_ptrs[b] = ctx.blas[b].scene;

  // Placements: instance 0 is the base scene at identity, then each native
  // instance whose prototype BLAS is non-empty, then each instanced curve
  // prototype placement. instance_id indexes ctx.instances (resolved back to
  // blas_id + transform at shade time).
  //
  // For Island-scale scenes this is tens of millions of instances. The per-
  // instance fill (two 3x4 copies + an 8-corner bounds expand) is independent,
  // so it runs in parallel: a validity mask is filled in parallel, an exclusive
  // scan assigns each kept instance its output slot (preserving the serial
  // base->instances->curves order, hence identical instance_id), then the
  // InstanceRT/lrt_instance arrays are scattered in parallel with per-thread
  // bounds and triangle-count reductions. Output is byte-identical to the serial
  // fill (indexed writes + min/max + integer sums are all order-invariant).
  static const float kIdentO2W[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  std::vector<lrt_instance> lrt_insts;
  const size_t n_inst_src = instances.size();
  // Expand curve placements: one TLAS instance per sub-BLAS of each curve
  // instance's prototype (split prototypes have several), all at the instance's
  // transform. (curve_inst.instances persists, so o2w pointers stay valid.)
  std::vector<std::pair<uint32_t, const float *>> curve_placements;
  for (const CurveInstanceRT &ci : curve_inst.instances)
    for (uint32_t bid : curve_proto_blas[ci.curve_proto_idx])
      curve_placements.push_back({bid, ci.o2w});
  const size_t n_curve_src = curve_placements.size();
  const size_t n_src = n_base_groups + n_inst_src + n_curve_src;
  // Resolve a source index [0, n_src) to its (blas_id, o2w) without materializing
  // a unified array: [0, G) = base groups (at identity), [G, G+nI) = native
  // instances, the remainder = instanced curve sub-BLAS placements.
  auto src_at = [&](size_t i, uint32_t *blas_id, const float **o2w) {
    if (i < n_base_groups) {
      *blas_id = base_blas_id(i);
      *o2w = kIdentO2W;
      return;
    }
    i -= n_base_groups;
    if (i < n_inst_src) {
      *blas_id = instances[i].blas_id;
      *o2w = instances[i].o2w;
      return;
    }
    i -= n_inst_src;
    *blas_id = curve_placements[i].first;
    *o2w = curve_placements[i].second;
  };
  auto blas_ok = [&](uint32_t blas_id) {
    return blas_id < ctx.blas.size() && ctx.blas[blas_id].scene;
  };
  const unsigned ai_threads = std::min<unsigned>(
      WorkerThreadCount(opt.threads), n_src ? unsigned(n_src) : 1u);
  auto run_range = [&](const std::function<void(unsigned, size_t, size_t)> &fn) {
    if (ai_threads <= 1) {
      fn(0u, 0, n_src);
      return;
    }
    std::vector<std::thread> pool;
    pool.reserve(ai_threads);
    for (unsigned t = 0; t < ai_threads; ++t) {
      const size_t b = (n_src * t) / ai_threads;
      const size_t e = (n_src * (t + 1)) / ai_threads;
      pool.emplace_back([&, t, b, e]() { fn(t, b, e); });
    }
    for (std::thread &th : pool) th.join();
  };

  std::vector<uint8_t> valid(n_src);
  run_range([&](unsigned, size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      uint32_t bid;
      const float *o;
      src_at(i, &bid, &o);
      valid[i] = blas_ok(bid) ? 1u : 0u;
    }
  });

  // -rtLod: per-instance view-dependent LOD. Classify each mesh-instance placement
  // (NOT the base non-instanced scene or curve placements) Full/Proxy/Cull from the
  // resolved camera. Auto-fit needs the full-scene bounds BEFORE culling, so
  // accumulate them here and resolve the camera mid-build (idempotent: the
  // post-build ResolveCameraNext re-runs once volumes are folded in). Cull ->
  // valid[i]=0 (drops it from the TLAS via the existing scan/scatter).
  std::vector<uint8_t> rt_lod_level;  // empty unless -rtLod
  if (opt.rt_lod) {
    const size_t inst_lo = n_base_groups;
    const size_t inst_hi = n_base_groups + n_inst_src;
    std::vector<Bounds> pb(ai_threads ? ai_threads : 1);
    run_range([&](unsigned tid, size_t b, size_t e) {
      Bounds lb;
      for (size_t i = b; i < e; ++i) {
        if (!valid[i]) continue;
        uint32_t bid; const float *o; src_at(i, &bid, &o);
        ExpandBoundsByTransformedO2W(&lb, local_bounds[bid], o);
      }
      pb[tid] = lb;
    });
    for (const Bounds &lb : pb)
      if (lb.valid) { Expand(&ctx.bounds, lb.lo); Expand(&ctx.bounds, lb.hi); }
    ResolveCameraNext(ctx);  // fills ctx.camera + ctx.height from ctx.bounds/opt
    const RtLodView view = MakeRtLodView(ctx.camera, ctx.height);
    RtLodConfig cfg;
    cfg.enabled = true;
    cfg.proxy = opt.rt_lod_proxy && rt_lod_box_blas != UINT32_MAX;
    cfg.frustum_cull = opt.rt_lod_frustum_cull;
    cfg.full_px = opt.rt_lod_full_px;
    cfg.cull_px = opt.rt_lod_cull_px;
    rt_lod_level.assign(n_src, uint8_t(RtLod::Full));
    std::atomic<uint32_t> n_full{0}, n_proxy{0}, n_cull{0};
    run_range([&](unsigned, size_t b, size_t e) {
      uint32_t lf = 0, lp = 0, lc = 0;
      for (size_t i = b; i < e; ++i) {
        if (!valid[i] || i < inst_lo || i >= inst_hi) continue;  // mesh instances only
        uint32_t bid; const float *o; src_at(i, &bid, &o);
        const RtLod lvl = ClassifyInstance(view, cfg, o, local_bounds[bid]);
        rt_lod_level[i] = uint8_t(lvl);
        if (lvl == RtLod::Cull) { valid[i] = 0; ++lc; }
        else if (lvl == RtLod::Proxy) ++lp;
        else ++lf;
      }
      n_full += lf; n_proxy += lp; n_cull += lc;
    });
    if (opt.stats)
      std::cerr << "rt-lod: full=" << n_full.load() << " proxy=" << n_proxy.load()
                << " culled=" << n_cull.load() << " (of " << n_inst_src
                << " mesh instances)\n";
  }

  // Exclusive scan -> output slot per kept instance (cheap, ~tens of ms at 22M).
  std::vector<uint32_t> slot(n_src);
  uint32_t kept = 0;
  for (size_t i = 0; i < n_src; ++i) {
    slot[i] = kept;
    kept += valid[i];
  }
  ctx.instances.resize(kept);
  lrt_insts.resize(kept);

  std::vector<Bounds> tls_bounds(ai_threads ? ai_threads : 1);
  std::vector<uint64_t> tls_tris(ai_threads ? ai_threads : 1, 0);
  run_range([&](unsigned tid, size_t b, size_t e) {
    Bounds lb;
    uint64_t lt = 0;
    for (size_t i = b; i < e; ++i) {
      if (!valid[i]) continue;
      uint32_t bid;
      const float *o;
      src_at(i, &bid, &o);
      // -rtLod Proxy: trace the shared box BLAS box-fit onto the prototype's object
      // AABB (its world AABB is identical, so bounds/stats are unaffected).
      float proxy_o2w[12];
      if (!rt_lod_level.empty() && rt_lod_level[i] == uint8_t(RtLod::Proxy) &&
          rt_lod_box_blas != UINT32_MAX) {
        BoxFitO2W(o, local_bounds[bid].lo, local_bounds[bid].hi, proxy_o2w);
        o = proxy_o2w;
        bid = rt_lod_box_blas;
      }
      const uint32_t id = slot[i];
      InstanceRT &inst = ctx.instances[id];
      inst.blas_id = bid;
      std::memcpy(inst.o2w, o, sizeof(inst.o2w));
      lrt_instance &li = lrt_insts[id];
      std::memset(&li, 0, sizeof(li));
      li.blas_id = bid;
      std::memcpy(li.obj2world, o, sizeof(li.obj2world));
      li.instance_id = id;
      li.mask = 0xffffffffu;
      ExpandBoundsByTransformedO2W(&lb, local_bounds[bid], o);
      lt += uint64_t(ctx.blas[bid].tris.size());
    }
    tls_bounds[tid] = lb;
    tls_tris[tid] = lt;
  });
  for (const Bounds &lb : tls_bounds) {
    if (lb.valid) {
      Expand(&ctx.bounds, lb.lo);
      Expand(&ctx.bounds, lb.hi);
    }
  }
  ctx.stats.triangles = 0;
  for (uint64_t v : tls_tris) ctx.stats.triangles += v;
  std::vector<uint8_t>().swap(valid);
  std::vector<uint32_t>().swap(slot);
  ctx.stats.curve_instances = curve_inst.instances.size();
  // The collection-side instance lists are now fully copied into ctx.instances +
  // lrt_insts; free them before lrt_tlas_build allocates the (peak) TLAS nodes.
  std::vector<InstanceRT>().swap(instances);
  std::vector<CurveInstanceRT>().swap(curve_inst.instances);

  if (lrt_insts.empty()) {
    std::cerr << "RT preview (next) found no renderable Mesh triangles.\n";
    return false;
  }
  ctx.stats.meshes = base_job_count + instances.size();

  lrt_result terr = LRT_RESULT_OK;
  ctx.tlas = lrt_tlas_build(blas_ptrs.data(), blas_ptrs.size(),
                            lrt_insts.data(), lrt_insts.size(), &build_opts,
                            &terr);
  const auto bvh_t1 = std::chrono::steady_clock::now();
  if (!ctx.tlas) {
    std::cerr << "Failed to build LightRT TLAS (err=" << int(terr) << ").\n";
    return false;
  }
  ctx.bvh_seconds = std::chrono::duration<double>(bvh_t1 - bvh_t0).count();
  ctx.stats.build_seconds = ctx.stream_seconds;
  uint64_t blas_bytes = freed_soup_bytes.load();  // soup dropped post-build (above)
  for (const Blas &b : ctx.blas) blas_bytes += uint64_t(b.vertices.size()) * sizeof(float);
  ctx.stats.packed_triangle_bytes = blas_bytes;
  return true;
}

// Collect finite UsdLux lights (Rect/Sphere/Disk/Cylinder/Distant) from the next
// stage into the LightCache, mirroring the legacy CollectLights/AddFiniteLight.
// DomeLights are handled separately as IBL (BuildNextIbl). Radiance is
// color * intensity * 2^exposure; position/direction come from the world xform
// (UsdLux lights emit along local -Z).
// Emissive MeshLightAPI triangles -> analytic mesh lights, so an area-light mesh
// actually LIGHTS the scene instead of merely glowing. The flatten marked their
// material (TriMat::area_light); this pass is where they become lights, which also
// makes the integrator suppress their emission on an indirect bounce -- otherwise
// they would be counted twice.
//
// One light per triangle, as the legacy flatten does, and every shading point
// loops over all of them: a large emissive mesh is therefore capped, brightest
// first, rather than quietly making the render O(emissive triangles) per sample.
void CollectMeshLightsNext(RenderContext &ctx) {
  ctx.lights.mesh.clear();
  ctx.lights.mesh_cdf.clear();

  // Flat (non-instanced) triangles. On the two-level path this list holds only the
  // base geometry and is routinely EMPTY -- and returning early on that, as this
  // used to, skipped the instanced emitters below along with it.
  const size_t nflat = ctx.flat_mats.empty() ? 0 : ctx.tris.size();
  for (size_t i = 0; i < nflat; ++i) {
    const FlatTri &ft = ctx.tris[i];
    if (ft.mat_id >= ctx.flat_mats.size()) continue;
    const TriMat &m = ctx.flat_mats[ft.mat_id];
    if (!m.area_light || Luminance(m.emission) <= 1.0e-6f) continue;
    const float area = TriangleArea(ft.p0, ft.p1, ft.p2);
    if (!(area > 1.0e-10f)) continue;
    PreviewLight ml;
    ml.kind = PreviewLight::Kind::Mesh;
    ml.position = Mul(Add(Add(ft.p0, ft.p1), ft.p2), 1.0f / 3.0f);
    ml.normal = ft.n;                  // the OUTWARD normal of the emitting face
    ml.direction = Mul(ft.n, -1.0f);
    ml.radiance = m.emission;
    ml.area = area;
    ml.power = std::max(0.0f, Luminance(ml.radiance) * area);
    ml.tri_id = int(i);
    ctx.lights.mesh.push_back(ml);
  }

  // INSTANCED emissive meshes (the TLAS path). The loop above walks the flat
  // triangle list, which an instanced prototype is deliberately not in -- its
  // geometry is stored once in a BLAS and placed by InstanceRT. So an
  // `instanceable` mesh light registered nothing at all and lit nothing, however
  // bright it was. Each PLACEMENT is its own light (that is what instancing
  // means), so every emissive prototype triangle is emitted once per instance,
  // transformed to world. The prototype's vertex soup is freed after its BVH is
  // built, so the positions come back out of the leaves (lrt_tri_get_verts).
  for (const InstanceRT &inst : ctx.instances) {
    if (inst.blas_id >= ctx.blas.size()) continue;
    const Blas &b = ctx.blas[inst.blas_id];
    if (b.is_curve || !b.scene || !lrt_tri_scene_has_verts(b.scene)) continue;
    for (size_t t = 0; t < b.tris.size(); ++t) {
      const uint32_t mid = b.tris[t].mat_id;
      if (mid >= b.mat_table.size()) continue;
      const TriMat &m = b.mat_table[mid];
      if (!m.area_light || Luminance(m.emission) <= 1.0e-6f) continue;
      float v0[3], v1[3], v2[3];
      if (!lrt_tri_get_verts(b.scene, uint32_t(t), v0, v1, v2)) continue;
      const Vec3 p0 = TransformPointO2W(inst.o2w, Vec3{v0[0], v0[1], v0[2]});
      const Vec3 p1 = TransformPointO2W(inst.o2w, Vec3{v1[0], v1[1], v1[2]});
      const Vec3 p2 = TransformPointO2W(inst.o2w, Vec3{v2[0], v2[1], v2[2]});
      const float area = TriangleArea(p0, p1, p2);
      if (!(area > 1.0e-10f)) continue;
      const Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
      PreviewLight ml;
      ml.kind = PreviewLight::Kind::Mesh;
      ml.position = Mul(Add(Add(p0, p1), p2), 1.0f / 3.0f);
      ml.normal = n;
      ml.direction = Mul(n, -1.0f);
      ml.radiance = m.emission;
      ml.area = area;
      ml.power = std::max(0.0f, Luminance(ml.radiance) * area);
      ml.tri_id = -1;  // not in the flat list
      ctx.lights.mesh.push_back(ml);
    }
  }

  constexpr size_t kMaxMeshLights = 1024;
  if (ctx.lights.mesh.size() > kMaxMeshLights) {
    const size_t dropped = ctx.lights.mesh.size() - kMaxMeshLights;
    std::partial_sort(ctx.lights.mesh.begin(),
                      ctx.lights.mesh.begin() + kMaxMeshLights,
                      ctx.lights.mesh.end(),
                      [](const PreviewLight &a, const PreviewLight &b) {
                        return a.power > b.power;
                      });
    ctx.lights.mesh.resize(kMaxMeshLights);
    std::cerr << "WARN: " << (dropped + kMaxMeshLights)
              << " emissive mesh-light triangles; keeping the " << kMaxMeshLights
              << " brightest (dropped " << dropped
              << "). Direct lighting from the rest is lost.\n";
  }
  AppendPowerCdf(&ctx.lights.mesh, &ctx.lights.mesh_cdf);
}

void CollectLightsNext(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &prim,
                       const matrix4d &parent_world, double time,
                       LightCache *cache) {
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);

  const std::string &t = prim.GetTypeName();
  PreviewLight::Kind kind = PreviewLight::Kind::Dome;
  bool is_light = true;
  if (t == "RectLight") kind = PreviewLight::Kind::Rect;
  else if (t == "SphereLight") kind = PreviewLight::Kind::Sphere;
  else if (t == "DiskLight") kind = PreviewLight::Kind::Disk;
  else if (t == "CylinderLight") kind = PreviewLight::Kind::Cylinder;
  else if (t == "DistantLight") kind = PreviewLight::Kind::Distant;
  else is_light = false;

  if (is_light) {
    const float intensity = ReadCamFloatNext(prim, "inputs:intensity", 1.0f);
    const float exposure = ReadCamFloatNext(prim, "inputs:exposure", 0.0f);
    Vec3 color{1.0f, 1.0f, 1.0f};
    if (const lightusd::next::Value *v = prim.GetPropertyValue("inputs:color"))
      if (const float *f = v->as_float3()) color = Vec3{f[0], f[1], f[2]};
    const float scale = intensity * std::pow(2.0f, exposure);
    PreviewLight dst;
    dst.kind = kind;
    dst.position = Vec3{float(world.m[3][0]), float(world.m[3][1]),
                        float(world.m[3][2])};
    Vec3 dir = TransformVector(world, Vec3{0.0f, 0.0f, -1.0f});  // -Z forward
    dst.direction = Length(dir) > 1.0e-6f ? Normalize(dir) : Vec3{0, -1, 0};
    // The OUTWARD normal of the emitting face, which for a UsdLux light IS its
    // emission direction (rect/disk/cylinder emit along local -Z / radially).
    // This used to be stored negated, while eval_light's emission-cone test and
    // the mesh lights (PreviewLight::Kind::Mesh, normal = the triangle's outward
    // normal) both read it as the emitting face -- so a rect light pointed AT a
    // surface lit nothing at all, and only lit what was behind it.
    dst.normal = dst.direction;
    dst.radiance = Mul(color, scale);
    if (const lightusd::next::Value *v =
            prim.GetPropertyValue("inputs:shadow:enable")) {
      if (const bool *b = v->as_bool()) dst.shadow_enable = *b;
    }
    const float radius = ReadCamFloatNext(prim, "inputs:radius", 0.5f);
    const float width = ReadCamFloatNext(prim, "inputs:width", 1.0f);
    const float height = ReadCamFloatNext(prim, "inputs:height", 1.0f);
    const float length = ReadCamFloatNext(prim, "inputs:length", 1.0f);
    constexpr float kPi = 3.14159265358979323846f;
    dst.radius = radius;
    dst.width = width;
    dst.height = height;
    dst.length = length;
    // Local axes in world space, so a shaped light can be sampled over its
    // surface: rect/disk live in the local XY plane, a cylinder runs along +X.
    {
      const Vec3 ax = TransformVector(world, Vec3{1.0f, 0.0f, 0.0f});
      const Vec3 ay = TransformVector(world, Vec3{0.0f, 1.0f, 0.0f});
      if (Length(ax) > 1.0e-8f && Length(ay) > 1.0e-8f) {
        dst.axis_u = Normalize(ax);
        dst.axis_v = Normalize(ay);
      } else {
        OrthonormalBasis(dst.normal, &dst.axis_u, &dst.axis_v);
      }
    }
    if (kind == PreviewLight::Kind::Rect) dst.area = width * height;
    else if (kind == PreviewLight::Kind::Sphere) dst.area = 4 * kPi * radius * radius;
    else if (kind == PreviewLight::Kind::Disk) dst.area = kPi * radius * radius;
    else if (kind == PreviewLight::Kind::Cylinder) dst.area = 2 * kPi * radius * length;
    // UsdLux inputs:normalize: hold the light's POWER fixed as its size
    // changes, by dividing the radiance by the shape's full surface area --
    // the same convention as AddFiniteLight on the RenderScene path, lusdview
    // and the mesh lights. A sphere at or below the punctual gate (1e-5) keeps
    // the undivided intensity: it is shaded as a point light (I/d^2), where
    // the division would blow up as r -> 0.
    bool normalize = false;
    if (const lightusd::next::Value *v =
            prim.GetPropertyValue("inputs:normalize"))
      if (const bool *b = v->as_bool()) normalize = *b;
    if (normalize && dst.area > 1.0e-8f &&
        (kind != PreviewLight::Kind::Sphere || radius > 1.0e-5f)) {
      dst.radiance = Mul(dst.radiance, 1.0f / dst.area);
    }
    dst.power = std::max(0.0f, Luminance(dst.radiance) * std::max(1.0f, dst.area));
    cache->finite.push_back(std::move(dst));
  }
  for (const lightusd::next::UsdPrim &c : prim.GetChildren())
    CollectLightsNext(stage, c, world, time, cache);
}

// Load the scene via next, then stream + build the BVH at the initial time.
// First UsdLuxDomeLight in the composed stage (depth-first), or an invalid prim.
// Accumulates the world transform along the way so the caller can read the dome's
// world-space orientation (out_world is the found dome's local-to-world matrix).
lightusd::next::UsdPrim FindDomeLightRec(const lightusd::next::UsdPrim &prim,
                                         const matrix4d &parent_world,
                                         double time, matrix4d *out_world) {
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const bool reset = lightusd::tydra::next::HasResetXformStack(prim);
  const matrix4d world = reset ? local : (local * parent_world);
  if (prim.GetTypeName() == "DomeLight") {
    if (out_world) *out_world = world;
    return prim;
  }
  for (const lightusd::next::UsdPrim &c : prim.GetChildren()) {
    matrix4d cw;
    lightusd::next::UsdPrim r = FindDomeLightRec(c, world, time, &cw);
    if (r.IsValid()) {
      if (out_world) *out_world = cw;
      return r;
    }
  }
  return lightusd::next::UsdPrim();
}

// Build the IBL cache from the --env override or a DomeLight; returns false (and
// leaves ibl invalid) if there is no env, so the renderer falls back to the
// headlight.
bool BuildNextIbl(const lightusd::next::Stage &stage, const Options &opt,
                  const std::string &base_dir, double time, IblCache *ibl) {
  std::string env_path = opt.env_file;
  Vec3 scale{1.0f, 1.0f, 1.0f};
  bool rotated = false;
  bool have_dome = false;  // a DomeLight prim was found (may be textureless)
  int probe_format = 0;    // texture:format: 2 mirroredBall / 3 angular
  Vec3 rx{1.0f, 0.0f, 0.0f}, ry{0.0f, 1.0f, 0.0f}, rz{0.0f, 0.0f, 1.0f};
  if (env_path.empty()) {
    for (const lightusd::next::UsdPrim &root : stage.GetRootPrims()) {
      matrix4d dome_world = matrix4d::identity();
      lightusd::next::UsdPrim dome =
          FindDomeLightRec(root, matrix4d::identity(), time, &dome_world);
      if (!dome.IsValid()) continue;
      have_dome = true;
      if (const lightusd::next::Value *v =
              dome.GetPropertyValue("inputs:texture:file")) {
        const std::string *ap = v->as_asset_path();
        if (!ap) ap = v->as_string();
        if (ap) env_path = *ap;
      }
      if (const lightusd::next::Value *v =
              dome.GetPropertyValue("inputs:texture:format")) {
        const std::string *t = v->as_token();
        if (!t) t = v->as_string();
        if (t) {
          if (*t == "mirroredBall") probe_format = 2;
          else if (*t == "angular") probe_format = 3;
        }
      }
      float intensity = 1.0f;
      if (const lightusd::next::Value *v = dome.GetPropertyValue("inputs:intensity"))
        if (const float *f = v->as_float()) intensity = *f;
      float exposure = 0.0f;
      if (const lightusd::next::Value *v = dome.GetPropertyValue("inputs:exposure"))
        if (const float *f = v->as_float()) exposure = *f;
      Vec3 color{1.0f, 1.0f, 1.0f};
      if (const lightusd::next::Value *v = dome.GetPropertyValue("inputs:color"))
        if (const float *f = v->as_float3()) color = Vec3{f[0], f[1], f[2]};
      const float e = intensity * std::exp2(exposure);
      scale = Vec3{color.x * e, color.y * e, color.z * e};
      // Dome orientation: the dome's local axes in world space (rows of the world
      // rotation, normalized to drop any scale). A world direction is mapped into
      // the dome frame by projecting onto them. Only flagged when meaningfully
      // non-identity, so untransformed domes stay byte-identical.
      Vec3 ax{float(dome_world.m[0][0]), float(dome_world.m[0][1]),
              float(dome_world.m[0][2])};
      Vec3 ay{float(dome_world.m[1][0]), float(dome_world.m[1][1]),
              float(dome_world.m[1][2])};
      Vec3 az{float(dome_world.m[2][0]), float(dome_world.m[2][1]),
              float(dome_world.m[2][2])};
      float la = Length(ax), lb = Length(ay), lc = Length(az);
      if (la > 1.0e-8f && lb > 1.0e-8f && lc > 1.0e-8f) {
        ax = Mul(ax, 1.0f / la);
        ay = Mul(ay, 1.0f / lb);
        az = Mul(az, 1.0f / lc);
        float dev = std::fabs(ax.x - 1.0f) + std::fabs(ax.y) + std::fabs(ax.z) +
                    std::fabs(ay.x) + std::fabs(ay.y - 1.0f) + std::fabs(ay.z) +
                    std::fabs(az.x) + std::fabs(az.y) + std::fabs(az.z - 1.0f);
        if (dev > 1.0e-6f) {
          rotated = true;
          rx = ax;
          ry = ay;
          rz = az;
        }
      }
      break;
    }
  }
  EnvImage env;
  if (env_path.empty()) {
    // A DomeLight with no texture:file is a uniform emitter of color*intensity
    // (times exposure). Synthesize a constant environment so it still lights the
    // scene and fills the background, instead of dropping the light entirely
    // (which rendered a textureless dome black on this path). No dome at all ->
    // no IBL (headlight fallback), unchanged.
    if (!have_dome) return false;
    env.width = 2;
    env.height = 1;
    env.pixels.assign(size_t(env.width) * size_t(env.height), scale);
  } else {
    std::string path = env_path;
    if (!lightusd::next::AssetResolver::IsAbsolutePath(path) &&
        !base_dir.empty()) {
      path = base_dir + "/" + path;
    }
    if (!LoadEnvImageFromFile(path, scale, &env)) return false;
    env = RemapProbeToLatlong(std::move(env), probe_format);
  }
  if (!BuildIblFromEnv(std::move(env), ibl)) return false;
  ibl->rotated = rotated;
  ibl->rx = rx;
  ibl->ry = ry;
  ibl->rz = rz;
  return true;
}

bool PayloadPathWithin(const std::string &path, const std::string &ancestor) {
  if (path == ancestor) return true;
  if (ancestor.empty() || ancestor == "/") return !path.empty() && path[0] == '/';
  return path.size() > ancestor.size() &&
         path.compare(0, ancestor.size(), ancestor) == 0 &&
         path[ancestor.size()] == '/';
}

bool PayloadIntersectsMask(const std::string &payload_path,
                           const std::vector<std::string> &mask) {
  for (const std::string &mask_path : mask) {
    if (mask_path.empty() || mask_path[0] == '!') continue;
    if (PayloadPathWithin(payload_path, mask_path) ||
        PayloadPathWithin(mask_path, payload_path)) {
      return true;
    }
  }
  return false;
}

bool LoadNextStageBudgeted(const Options &opt, lightusd::next::Stage *stage,
                           std::string *warn, std::string *err,
                           lightusd::next::ValueClipStageLoader *clip_loader) {
  if (!stage) {
    if (err) *err = "null Stage output";
    return false;
  }
  lightusd::next::StageSessionOptions session_options;
  // The local-file CLI supports ordinary USD layer-relative references,
  // including sibling directories. Keep its finite load/memory limits while
  // opting into compatibility resolution; API sessions remain untrusted by
  // default.
  session_options.load.input_policy = lightusd::next::InputPolicy::Trusted;
  session_options.compose = true;
  session_options.composition.load_payloads = true;
  session_options.composition.variant_overrides = opt.variant_overrides;
  // A render mask is also a composition boundary. Loading every payload and
  // filtering its geometry afterwards defeats the memory purpose of -mask on
  // payload-heavy scenes. Keep payloads whose authored prim path intersects a
  // requested subtree; selected payloads may still contain nested payloads.
  if (!opt.mask.empty()) {
    const std::vector<std::string> payload_mask = opt.mask;
    session_options.composition.load_payloads = false;
    session_options.composition.payload_policy =
        [payload_mask](const lightusd::next::Path &prim_path,
                       const std::string &) {
          return PayloadIntersectsMask(prim_path.str(), payload_mask);
        };
  }
  session_options.load.limits.max_resident_bytes =
      MemBudget::Get().Cap() * 55 / 100;
  session_options.cache_retention = lightusd::next::CacheRetention::LayersOnly;
  lightusd::next::StageSession session;
  if (!session.OpenFile(opt.input, session_options)) {
    if (warn) *warn = session.GetWarning();
    if (err) *err = session.GetError();
    return false;
  }
  if (!opt.mask.empty()) {
    const std::vector<lightusd::next::Path> deferred =
        session.GetDeferredPayloadPaths();
    if (!deferred.empty()) {
      std::cerr << "next: " << deferred.size()
                << " payload(s) deferred outside -mask\n";
    }
  }
  if (clip_loader) {
    const std::vector<std::string> dependencies = session.GetLayerDependencies();
    const lightusd::next::ResolverConfig resolver_config = session_options.resolver;
    const std::string input = opt.input;
    *clip_loader = [resolver_config, dependencies, input](
        const std::string &asset, lightusd::next::Stage *clip_stage,
        std::string *clip_warn, std::string *clip_err) {
      if (!clip_stage) return false;
      auto base_dir = [](const std::string &file) {
        const size_t slash = file.find_last_of("/\\");
        return slash == std::string::npos ? std::string(".")
                                          : file.substr(0, slash);
      };
      lightusd::next::AssetResolver resolver(resolver_config);
      std::vector<std::string> candidates;
      candidates.push_back(resolver.ResolvePath(
          asset, base_dir(input), resolver_config.enable_suffix_fallback));
      for (const std::string &dependency : dependencies) {
        candidates.push_back(resolver.ResolvePath(
            asset, base_dir(dependency),
            resolver_config.enable_suffix_fallback));
      }
      std::string resolved;
      for (const std::string &candidate : candidates) {
        if (!candidate.empty() && resolver.Exists(candidate)) {
          resolved = candidate;
          break;
        }
      }
      if (resolved.empty()) {
        if (clip_err) *clip_err = "asset not found: " + asset;
        return false;
      }
      lightusd::next::StageSessionOptions clip_options;
      clip_options.load.input_policy = lightusd::next::InputPolicy::Trusted;
      clip_options.resolver = resolver_config;
      clip_options.composition.load_payloads = true;
      lightusd::next::StageSession clip_session;
      if (!clip_session.OpenFile(resolved, clip_options)) {
        if (clip_warn) *clip_warn = clip_session.GetWarning();
        if (clip_err) *clip_err = clip_session.GetError();
        return false;
      }
      auto taken = clip_session.CloseAndTakeStage();
      if (!taken) {
        if (clip_err) *clip_err = "clip stage is still retained";
        return false;
      }
      *clip_stage = std::move(*taken);
      return true;
    };
  }
  auto taken = session.CloseAndTakeStage();
  if (!taken) {
    if (err) *err = "stage is still retained";
    return false;
  }
  *stage = std::move(*taken);
  if (warn) *warn = session.GetWarning();
  if (err) err->clear();
  return true;
}

bool HasAuthoredProperty(const lightusd::next::UsdPrim &prim,
                         const char *name) {
  const lightusd::next::PrimSpec *spec = prim.GetPrimSpec();
  return spec && spec->property(name);
}

uint32_t RenderPurposeMask(const std::vector<std::string> &purposes) {
  uint32_t mask = 0;
  for (const std::string &purpose : purposes) {
    if (purpose == "default") mask |= kPurposeDefaultBit;
    else if (purpose == "render") mask |= kPurposeRenderBit;
    else if (purpose == "proxy") mask |= kPurposeProxyBit;
    else if (purpose == "guide") mask |= kPurposeGuideBit;
  }
  return mask;
}

void AppendCollectionMask(const std::vector<lightusd::next::Path> &includes,
                          const std::vector<lightusd::next::Path> &excludes,
                          bool include_root, std::vector<std::string> *mask) {
  if (!mask) return;
  if (!include_root) {
    for (const auto &path : includes) mask->push_back(path.prim_path().str());
  }
  for (const auto &path : excludes) {
    mask->push_back("!" + path.prim_path().str());
  }
}

bool CaptureSource(const std::string& source, CaptureAov* aov) {
  if (source.empty() || source == "Ci" || source == "color") *aov = CaptureAov::Color;
  else if (source == "depth" || source == "z") *aov = CaptureAov::Depth;
  else if (source == "worldNormal" || source == "N") *aov = CaptureAov::WorldNormal;
  else if (source == "primId") *aov = CaptureAov::PrimId;
  else return false;
  return true;
}

bool ApplyUsdRenderOptions(const lightusd::next::Stage &stage,
                           Options *opt) {
  if (!opt) return false;
  using namespace lightusd::next;

  UsdPrim pass;
  RenderPassData pass_data;
  if (!opt->render_pass.empty()) {
    pass = stage.GetPrimAtPath(opt->render_pass);
    if (!GetRenderPassData(stage, pass, &pass_data, opt->timecode)) {
      std::cerr << "Invalid RenderPass path: " << opt->render_pass << "\n";
      return false;
    }
    if (!pass_data.command.is_empty()) {
      std::cerr << "WARN: RenderPass command is preserved but never executed\n";
    }
    AppendCollectionMask(pass_data.render_visibility_includes,
                         pass_data.render_visibility_excludes,
                         pass_data.render_visibility_include_root, &opt->mask);
  }

  std::string settings_path = opt->render_settings;
  std::string product_path = opt->render_product;
  if (!pass_data.render_source.empty()) {
    const UsdPrim source = stage.GetPrimAtPath(pass_data.render_source[0]);
    if (IsRenderSettings(source) && settings_path.empty())
      settings_path = source.GetPath().str();
    else if (IsRenderProduct(source) && product_path.empty())
      product_path = source.GetPath().str();
  }
  if (settings_path.empty() && stage.GetMeta().renderSettingsPrimPath_set)
    settings_path = stage.GetMeta().renderSettingsPrimPath;

  UsdPrim settings_prim = settings_path.empty()
                              ? UsdPrim()
                              : stage.GetPrimAtPath(settings_path);
  RenderSettingsData settings;
  if (!settings_path.empty() &&
      !GetRenderSettingsData(stage, settings_prim, &settings, opt->timecode)) {
    std::cerr << "Invalid RenderSettings path: " << settings_path << "\n";
    return false;
  }
  if (settings_prim) {
    if (!opt->width_explicit && HasAuthoredProperty(settings_prim, "resolution"))
      opt->width = std::max(1, settings.resolution[0]);
    if (!opt->height_explicit && HasAuthoredProperty(settings_prim, "resolution"))
      opt->height = std::max(1, settings.resolution[1]);
    if (!opt->camera_explicit && !settings.camera.empty())
      opt->camera = settings.camera[0].prim_path().str();
    if (!opt->purpose_explicit) {
      const uint32_t mask = RenderPurposeMask(settings.included_purposes);
      if (mask) opt->purpose_mask = mask;
    }
    if (product_path.empty() && !settings.products.empty())
      product_path = settings.products[0].prim_path().str();
  }

  if (!product_path.empty()) {
    const UsdPrim product_prim = stage.GetPrimAtPath(product_path);
    RenderProductData product;
    if (!GetRenderProductData(stage, product_prim, &product, opt->timecode)) {
      std::cerr << "Invalid RenderProduct path: " << product_path << "\n";
      return false;
    }
    if (!opt->width_explicit && HasAuthoredProperty(product_prim, "resolution"))
      opt->width = std::max(1, product.resolution[0]);
    if (!opt->height_explicit && HasAuthoredProperty(product_prim, "resolution"))
      opt->height = std::max(1, product.resolution[1]);
    if (!opt->camera_explicit && !product.camera.empty())
      opt->camera = product.camera[0].prim_path().str();
    for (const Path &var_path : product.ordered_vars) {
      RenderVarData var;
      const UsdPrim var_prim = stage.GetPrimAtPath(var_path.prim_path());
      if (!GetRenderVarData(stage, var_prim, &var, opt->timecode)) continue;
      if (!opt->aov_explicit && product.ordered_vars.size() == 1) {
        if (var.source_type != "raw" || !CaptureSource(var.source_name, &opt->aov)) {
          std::cerr << "Unsupported RenderVar source: " << var.source_name << "\n";
          return false;
        }
      } else if (!opt->aov_explicit && !opt->all_render_products && product.ordered_vars.size() > 1) {
        std::cerr << "Multiple RenderVars require --all-products or --aov\n";
        return false;
      }
    }
  }
  return true;
}

bool BuildRenderContext(const Options &opt, RenderContext &ctx) {
  ctx.opt = opt;
  ctx.width = opt.width > 0 ? opt.width : 960;

  const auto load_t0 = std::chrono::steady_clock::now();
  std::string warn, err;
  // LoadUSDComposed resolves references/payloads/sublayers in place (anchored to
  // the input dir), so lusdrender consumes raw reference-composed scenes (e.g.
  // Caldera prefab stubs) directly — no external usdcat --flatten step. Self-
  // contained / pre-flattened inputs skip composition (identical to LoadUSD).
  if (!LoadNextStageBudgeted(opt, &ctx.stage, &warn, &err,
                             &ctx.clip_stage_loader)) {
    if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";
    std::cerr << "Failed to load USD (next): " << err << "\n";
    return false;
  }
  if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";
  const auto load_t1 = std::chrono::steady_clock::now();
  ctx.load_seconds = std::chrono::duration<double>(load_t1 - load_t0).count();

  if (!ApplyUsdRenderOptions(ctx.stage, &ctx.opt)) return false;
  ctx.width = ctx.opt.width > 0 ? ctx.opt.width : 960;

  // The composed stage is the memory baseline; everything our pool allocator
  // tracks (triangle buffers) must fit in cap - base. Abort now if compose alone
  // already blew the cap.
  std::string why;
  if (MemBudget::Get().WouldExceed(0, &why)) {
    std::cerr << "Aborting after load: " << why
              << ".\n  Raise -maxMem, restrict with -mask, or pre-flatten.\n";
    return false;
  }
  MemBudget::Get().SnapshotBase();

  ctx.up_axis = GetUpAxis(ctx.stage.GetUpAxis());
  ctx.geometry_animated = SceneGeometryAnimated(ctx.stage, ctx.opt.mask);

  // Initial time: default value unless -timecode was given. -defaultTime forces
  // the default (NaN) explicitly.
  const double init_time = ctx.opt.default_time
                               ? std::numeric_limits<double>::quiet_NaN()
                               : ctx.opt.timecode;
  if (!ExtractAndBuildBVH(ctx, init_time)) return false;

  // UsdVol volumes (OpenVDB) -> dense grids for raymarching. Extend bounds with
  // each volume's world AABB BEFORE resolving the camera, so a volume-only scene
  // (or one whose volume sits away from the origin, e.g. an explosion sim) is
  // framed and rendered instead of leaving the auto-camera looking at an empty
  // origin.
  ctx.volumes.clear();
  const size_t volume_budget_bytes = std::max<size_t>(
      size_t(64) << 20,
      std::min<size_t>(size_t(512) << 20,
                       MemBudget::Get().Cap() / 16));
  size_t volume_density_bytes = 0;
  for (const lightusd::next::UsdPrim &root : ctx.stage.GetRootPrims())
    CollectVolumesNext(ctx.stage, root, matrix4d::identity(), init_time,
                       DirName(ctx.opt.input), &ctx.volumes, volume_budget_bytes,
                       &volume_density_bytes);
  ExpandBoundsByVolume(ctx.volumes, &ctx.bounds);
  if (!ctx.volumes.empty()) {
    std::cerr << "rt volumes: " << ctx.volumes.size() << " (density "
              << volume_density_bytes / (1024 * 1024) << "/"
              << volume_budget_bytes / (1024 * 1024) << " MiB)\n";
  }

  ResolveCameraNext(ctx);
  ResolveBackPlateNext(ctx);

  // Image-based lighting: an explicit --env override wins, else the first
  // UsdLuxDomeLight's texture (scaled by intensity*color). Enables the glossy
  // BRDF (roughness/metallic) + env background; absent -> camera headlight.
  BuildNextIbl(ctx.stage, ctx.opt, DirName(ctx.opt.input), init_time, &ctx.ibl);
  if (ctx.opt.stats && ctx.ibl.valid) {
    std::cerr << "ibl: " << ctx.ibl.env.width << "x" << ctx.ibl.env.height
              << " (" << (ctx.opt.env_file.empty() ? "DomeLight" : "--env") << ")\n";
  }
  // Finite UsdLux lights (Rect/Sphere/Disk/Cylinder/Distant) -> ctx.lights, so the
  // shading path lights interiors that the dome can't reach (e.g. ALab's shot rig).
  ctx.lights.finite.clear();
  for (const lightusd::next::UsdPrim &root : ctx.stage.GetRootPrims())
    CollectLightsNext(ctx.stage, root, matrix4d::identity(), init_time,
                      &ctx.lights);
  AppendPowerCdf(&ctx.lights.finite, &ctx.lights.finite_cdf);
  if (ctx.opt.stats && !ctx.lights.finite.empty())
    std::cerr << "rt finite lights: " << ctx.lights.finite.size() << "\n";

  CollectMeshLightsNext(ctx);
  if (opt.stats && !ctx.lights.mesh.empty())
    std::cerr << "rt mesh light triangles: " << ctx.lights.mesh.size() << "\n";

  // Extraction/BVH construction and mesh-light collection have consumed all
  // static authored geometry needed by the renderer. Releasing those source
  // arrays avoids retaining a second full copy beside the traced scene. Keep
  // animated scenes intact: later -frames extraction must still read defaults
  // and time samples from the Stage.
  if (!ctx.geometry_animated) {
    const lightusd::next::Stage::StaticGeometryReleaseStats released =
        ctx.stage.ReleaseStaticGeometryArrays();
    ctx.released_static_geometry_bytes = released.estimated_payload_bytes;
    if (released.property_count != 0) {
      std::cerr << "next: released " << released.property_count
                << " static geometry arrays ("
                << (double(released.estimated_payload_bytes) /
                    (1024.0 * 1024.0))
                << " MiB) after extraction\n";
    }
  }

  return true;
}

// A prim ID names the source mesh, including a shared prototype. Instance
// placements deliberately share that source ID. Reject collisions/unsupported
// hit kinds instead of silently labelling them as background.
static bool WriteCapturePrimMap(const RenderContext& ctx, const std::string& path) {
  using Json = lightusd::minijson::Value;
  std::map<uint32_t, std::string> names;
  bool valid = true;
  ctx.stage.Traverse([&](const lightusd::next::UsdPrim& prim) {
    const auto name = prim.GetPath().str();
    const uint32_t id = CapturePrimId(name);
    auto found = names.emplace(id, name);
    if (!found.second && found.first->second != name) valid = false;
    return true;
  });
  auto check = [&](const TriMat& mat) {
    if (!mat.capture_prim_id || !names.count(mat.capture_prim_id)) valid = false;
  };
  for (const auto& mat : ctx.flat_mats) check(mat);
  for (const auto& b : ctx.blas) for (const auto& mat : b.mat_table) check(mat);
  if (ctx.stats.curve_strands || !ctx.volumes.empty()) valid = false;
  if (!valid) {
    std::cerr << "primId capture requires named source meshes without ID collisions (curves/volumes are unsupported)\n";
    return false;
  }
  Json entries = Json::array();
  for (const auto& item : names) entries.push_back(Json{{"id", item.first}, {"primPath", item.second}});
  Json report{{"schemaVersion", 1}, {"background", 0}, {"semantics", "sourcePrim"}, {"prims", entries}};
  std::ofstream out(path + ".ids.json"); out << report.dump(2) << '\n'; out.close();
  return bool(out);
}

// Render the current camera/parameters of `ctx` and write to `path`.
// Reuses the persistent BVH (no rebuild). Returns the trace time in seconds
// (or a negative value on write failure).
double RenderFrameTo(RenderContext &ctx, const std::string &path) {
  ctx.opt.width = ctx.width;
  std::string budget_error;
  const uint64_t image_bytes = uint64_t(std::max(0, ctx.width)) * uint64_t(std::max(0, ctx.height)) * 16;
  if (ctx.width <= 0 || ctx.width > 32768 || ctx.height <= 0 || ctx.height > 32768 ||
      image_bytes > std::numeric_limits<size_t>::max() ||
      MemBudget::Get().WouldExceed(size_t(image_bytes), &budget_error)) {
    std::cerr << "Capture image dimensions exceed limits: " << budget_error << "\n"; return -1;
  }
  if (ctx.opt.aov == CaptureAov::PrimId && !WriteCapturePrimMap(ctx, path)) return -1;
  const auto t0 = std::chrono::steady_clock::now();
  lightusd::Image img = RenderImage(
      ctx.scene, &ctx.direct, ctx.tris, ctx.flat_mats, ctx.lights,
      ctx.ibl.valid ? &ctx.ibl : nullptr, ctx.camera, ctx.opt, ctx.height,
      ctx.textures.empty() ? nullptr : &ctx.textures,
      ctx.tri_uvs.empty() ? nullptr : &ctx.tri_uvs,
      ctx.use_tlas ? ctx.tlas : nullptr,
      ctx.use_tlas ? &ctx.blas : nullptr,
      ctx.use_tlas ? &ctx.instances : nullptr,
      ctx.tri_colors.empty() ? nullptr : &ctx.tri_colors,
      ctx.tri_normals.empty() ? nullptr : &ctx.tri_normals,
      ctx.volumes.empty() ? nullptr : &ctx.volumes,
      ctx.flat_openpbr_mats.empty() ? nullptr : &ctx.flat_openpbr_mats,
      ctx.triangle_chunks.empty() ? nullptr : &ctx.triangle_chunks,
      ctx.backplates.empty() ? nullptr : &ctx.backplates);
  const auto t1 = std::chrono::steady_clock::now();
  if (ctx.opt.aov != CaptureAov::Color) {
    if (path.size() < 4 || path.substr(path.size() - 4) != ".pfm") {
      std::cerr << "Data AOV output must use .pfm (raw float32, no color transform)\n";
      return -1;
    }
    std::ofstream out(path, std::ios::binary);
    out << "PF\n" << img.width << " " << img.height << "\n-1.0\n";
    for (int y = img.height - 1; y >= 0; --y) for (int x = 0; x < img.width; ++x) {
      const size_t offset = (size_t(y) * size_t(img.width) + size_t(x)) * 16;
      for (unsigned c = 0; c < 3; ++c) {
        uint32_t bits; std::memcpy(&bits, img.data.data() + offset + c * 4, 4);
        char bytes[4]; for (unsigned b = 0; b < 4; ++b) bytes[b] = char(bits >> (8 * b));
        out.write(bytes, 4);
      }
    }
    out.close();
    if (!out) { std::cerr << "Failed to write PFM\n"; return -1; }
    return std::chrono::duration<double>(t1 - t0).count();
  }
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(path, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return -1.0;
  }
  return std::chrono::duration<double>(t1 - t0).count();
}

void PrintRTStats(const RenderContext &ctx) {
  std::cerr << "rt preview: 1\n";
  std::cerr << "rt loader: next\n";
  if (ctx.released_static_geometry_bytes != 0)
    std::cerr << "rt released static geometry: "
              << (double(ctx.released_static_geometry_bytes) /
                  (1024.0 * 1024.0))
              << " MiB\n";
  std::cerr << "rt meshes: " << ctx.stats.meshes << "\n";
  std::cerr << "rt skipped meshes: " << ctx.stats.skipped_meshes << "\n";
  std::cerr << "rt missing textures: " << ctx.stats.missing_textures << "\n";
  std::cerr << "rt textures: " << ctx.stats.texture_count
            << " (resident "
            << (double(ctx.stats.texture_resident_bytes) /
                (1024.0 * 1024.0))
            << " MiB)\n";
  std::cerr << "rt texture mip fallbacks: " << ctx.stats.texture_mip_fallbacks
            << "\n";
  size_t backface_materials = 0;
  for (const TriMat &m : ctx.flat_mats)
    backface_materials += m.backface_id < ctx.flat_mats.size();
  for (const Blas &b : ctx.blas)
    for (const TriMat &m : b.mat_table)
      backface_materials += m.backface_id < b.mat_table.size();
  std::cerr << "rt backface materials: " << backface_materials << "\n";
  std::cerr << "rt purpose default triangles: "
            << ctx.stats.purpose_default_triangles << "\n";
  std::cerr << "rt purpose render triangles: "
            << ctx.stats.purpose_render_triangles << "\n";
  std::cerr << "rt purpose proxy triangles: "
            << ctx.stats.purpose_proxy_triangles << "\n";
  std::cerr << "rt purpose guide triangles: "
            << ctx.stats.purpose_guide_triangles << "\n";
  if (ctx.stats.curve_strands > 0)
    std::cerr << "rt curve strands: " << ctx.stats.curve_strands << "\n";
  if (ctx.stats.curve_instances > 0)
    std::cerr << "rt curve instances: " << ctx.stats.curve_instances << "\n";
  if (ctx.stats.skipped_curves > 0)
    std::cerr << "rt skipped curves: " << ctx.stats.skipped_curves
              << " (invalid data: " << ctx.stats.invalid_curve_data << ")\n";
  size_t round_curve_segments = 0, flat_curve_segments = 0;
  size_t round_curve_chunks = ctx.direct.round_curve_chunks.size();
  size_t flat_curve_chunks = ctx.direct.flat_curve_chunks.size();
  for (const CurveSceneChunk &chunk : ctx.direct.round_curve_chunks)
    round_curve_segments += chunk.info.size();
  for (const CurveSceneChunk &chunk : ctx.direct.flat_curve_chunks)
    flat_curve_segments += chunk.info.size();
  if (round_curve_chunks != 0 || flat_curve_chunks != 0) {
    const size_t metadata_bytes =
        (round_curve_segments + flat_curve_segments) * sizeof(TriInfo);
    std::cerr << "rt native curve chunks: round " << round_curve_chunks
              << " (" << round_curve_segments << " segments), flat "
              << flat_curve_chunks << " (" << flat_curve_segments
              << " segments), metadata "
              << double(metadata_bytes) / (1024.0 * 1024.0) << " MiB\n";
  }
  size_t native_gaussian_count = 0;
  for (const EllipseSceneChunk &chunk : ctx.direct.ellipse_chunks)
    native_gaussian_count += chunk.info.size();
  if (native_gaussian_count != 0)
    std::cerr << "rt native Gaussian ellipses: " << native_gaussian_count
              << " in " << ctx.direct.ellipse_chunks.size() << " chunk(s)\n";
  std::cerr << "load seconds: " << ctx.load_seconds << "\n";
  std::cerr << "rt triangle stream seconds: " << ctx.stream_seconds << "\n";
  std::cerr << "rt bvh build seconds: " << ctx.bvh_seconds << "\n";
  std::cerr << "memory cap: " << MemBudget::GiB(MemBudget::Get().Cap()) << "\n";
  std::cerr << "tracked buffer peak: "
            << MemBudget::GiB(MemBudget::Get().PeakTracked()) << "\n";
  std::cerr << "process RSS: " << MemBudget::GiB(MemBudget::ProcessRSS()) << "\n";
  // Keep the public -stats fields aligned with the schema-aware renderer. The
  // next path owns the same IblCache representation, so these are exact cache
  // sizes rather than estimates.
  std::cerr << "domelight: "
            << (ctx.ibl.valid && ctx.opt.env_file.empty() ? 1 : 0) << "\n";
  std::cerr << "ibl envmap: " << (ctx.ibl.valid ? 1 : 0) << "\n";
  std::cerr << "ibl diffuse size: "
            << (ctx.ibl.diffuse.width * ctx.ibl.diffuse.height) << "\n";
  std::cerr << "ibl prefilter levels: " << ctx.ibl.prefiltered.size() << "\n";
  std::cerr << "ibl brdf lut size: "
            << (ctx.ibl.brdf_size * ctx.ibl.brdf_size) << "\n";
  // ctx.stats.triangles is the (instance-expanded) renderable triangle count in
  // both paths; ctx.tris is empty in the two-level (TLAS) path.
  std::cerr << "triangles: " << ctx.stats.triangles << "\n";
  if (ctx.use_tlas) {
    size_t unique_tris = 0;
    for (const Blas &b : ctx.blas) unique_tris += b.tris.size();
    std::cerr << "rt instancing: tlas\n";
    std::cerr << "rt blas count: " << ctx.blas.size() << "\n";
    std::cerr << "rt instances: " << ctx.instances.size() << "\n";
    std::cerr << "rt point instancers: " << ctx.stats.point_instancers << "\n";
    std::cerr << "rt point instances: " << ctx.stats.point_instances << "\n";
    std::cerr << "rt nested instances: " << ctx.stats.nested_instances << "\n";
    std::cerr << "rt unique triangles: " << unique_tris << "\n";
  } else {
    if (ctx.scene) {
      lrt_tri_stats st;
      std::memset(&st, 0, sizeof(st));
      lrt_tri_scene_stats(ctx.scene, &st);
      std::cerr << "lightrt: " << lrt_tri_kernel_name(ctx.scene) << "\n";
      std::cerr << "bvh nodes: " << st.node_count << ", leaves: " << st.leaf_count
                << ", memory: " << st.memory_bytes << " bytes\n";
    } else {
      size_t nodes = 0, leaves = 0, memory = 0;
      const char *kernel = nullptr;
      for (const TriangleSceneChunk &chunk : ctx.triangle_chunks) {
        lrt_tri_stats st;
        std::memset(&st, 0, sizeof(st));
        lrt_tri_scene_stats(chunk.scene.get(), &st);
        nodes += st.node_count;
        leaves += st.leaf_count;
        memory += st.memory_bytes;
        if (!kernel) kernel = lrt_tri_kernel_name(chunk.scene.get());
      }
      std::cerr << "lightrt: " << (kernel ? kernel : "none") << "\n";
      std::cerr << "bvh chunks: " << ctx.triangle_chunks.size()
                << ", nodes: " << nodes << ", leaves: " << leaves
                << ", memory: " << memory << " bytes\n";
    }
  }
}

// Parse an OpenUSD usdrecord FRAMESPEC list into time codes. Each comma-
// separated spec is "t", "start:end", or "start:end x stride" (stride defaults
// to 1, sign inferred from start/end). Examples: "1", "1:10", "1:10x2",
// "10:1", "1:5,8,12:20x4".
// Upper bound on the number of frames a single -frames spec may enumerate.
// Guards against OOM / non-terminating loops from a huge range or a tiny stride.
static constexpr size_t kMaxFrameSpecFrames = 100000;

bool ParseFrameSpec(const std::string &spec, std::vector<double> *times) {
  std::string s = spec;
  for (char &c : s) {
    if (c == ',') c = ' ';
  }
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) {
    double start = 0, end = 0, stride = 1;
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) {
      try {
        start = end = std::stod(tok);
      } catch (...) {
        return false;
      }
    } else {
      std::string a = tok.substr(0, colon);
      std::string rest = tok.substr(colon + 1);
      const size_t xpos = rest.find('x');
      std::string b = (xpos == std::string::npos) ? rest : rest.substr(0, xpos);
      try {
        start = std::stod(a);
        end = std::stod(b);
        if (xpos != std::string::npos) stride = std::stod(rest.substr(xpos + 1));
      } catch (...) {
        return false;
      }
    }
    if (stride == 0) stride = 1;
    stride = std::fabs(stride);
    // Reject non-finite bounds/stride, and a stride so small relative to the
    // range that `t += stride` cannot progress (would spin forever) or the range
    // is so large it would enumerate an unbounded number of frames (OOM). A
    // per-token count computed up front avoids both.
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(stride)) {
      return false;
    }
    const double span = std::fabs(end - start);
    // stride must move the cursor by at least ~1 ULP at the range magnitude.
    const double min_stride =
        std::max(std::fabs(start), std::fabs(end)) *
        std::numeric_limits<double>::epsilon();
    if (stride <= min_stride) return false;
    const double steps = std::floor(span / stride + 1e-9);
    // +1 for the inclusive endpoint. Cap the total across all tokens.
    if (steps + 1.0 > double(kMaxFrameSpecFrames) ||
        times->size() + size_t(steps) + 1 > kMaxFrameSpecFrames) {
      return false;
    }
    const long n = long(steps);
    const double dir = (start <= end) ? 1.0 : -1.0;
    for (long i = 0; i <= n; ++i) times->push_back(start + dir * stride * double(i));
  }
  return !times->empty();
}

// Substitute a frame number into an output path. Runs of '#' are replaced by the
// zero-padded frame number (width = number of '#'). If there is no '#', the
// frame number is inserted before the extension (.NNNN).
std::string SubstituteFrame(const std::string &path, long frame) {
  const size_t hpos = path.find('#');
  if (hpos != std::string::npos) {
    size_t hend = hpos;
    while (hend < path.size() && path[hend] == '#') ++hend;
    const int width = int(hend - hpos);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*ld", width, frame);
    return path.substr(0, hpos) + buf + path.substr(hend);
  }
  const size_t dot = path.find_last_of('.');
  char buf[32];
  std::snprintf(buf, sizeof(buf), ".%04ld", frame);
  if (dot == std::string::npos) return path + buf;
  return path.substr(0, dot) + buf + path.substr(dot);
}

// Structured, greppable end-of-load diagnostic summary, mirroring lusdview's
// `load summary:` line so both tools feed the usd-assets smoke harness the same
// way. Printed unconditionally (independent of -stats) when there is something
// actionable to report. degraded_materials tracks unsupported surfaces rendered through the shared
// resolver's per-material degraded PreviewSurface; missing_textures / skipped
// are real counts.
static void PrintLoadSummaryNext(const RenderContext &ctx) {
  const size_t degraded = ctx.stats.degraded_materials;
  const size_t unsupported_mtlx = ctx.stats.unsupported_mtlx;
  const size_t missing = ctx.stats.missing_textures;
  const size_t skipped = ctx.stats.skipped_meshes;
  if (degraded + missing + unsupported_mtlx + skipped == 0) return;
  std::cerr << "load summary: degraded_materials=" << degraded
            << " missing_textures=" << missing
            << " unsupported_mtlx=" << unsupported_mtlx
            << " skipped=" << skipped << " other=0\n";
  for (const std::string &example : ctx.stats.material_diagnostic_examples) {
    std::cerr << "material diagnostic: " << example << "\n";
  }
}

static std::string CaptureSuffix(const std::string& path) {
  std::string out;
  // Encode separators too: /A_B and /A/B must not collide.
  const char* hex = "0123456789ABCDEF";
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += char(c);
    else { out += '_'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

static double RenderCaptureProducts(RenderContext& ctx, const Options& original, const std::string& output) {
  if (!original.all_render_products) return RenderFrameTo(ctx, output);
  using namespace lightusd::next;
  std::string settings_path = original.render_settings;
  if (settings_path.empty() && ctx.stage.GetMeta().renderSettingsPrimPath_set)
    settings_path = ctx.stage.GetMeta().renderSettingsPrimPath;
  RenderSettingsData settings;
  if (!GetRenderSettingsData(ctx.stage, ctx.stage.GetPrimAtPath(settings_path), &settings, ctx.frame_time) || settings.products.empty()) {
    std::cerr << "--all-products requires RenderSettings with products\n"; return -1;
  }
  double total = 0;
  std::set<std::string> output_paths;
  const size_t dot = output.find_last_of('.');
  const size_t slash = output.find_last_of("/\\");
  const std::string prefix = dot == std::string::npos || (slash != std::string::npos && dot < slash)
      ? output : output.substr(0, dot);
  for (const auto& path : settings.products) {
    RenderProductData product;
    if (!GetRenderProductData(ctx.stage, ctx.stage.GetPrimAtPath(path), &product, ctx.frame_time)) return -1;
    if (product.product_type != "raster") { std::cerr << "Only raster products are supported\n"; return -1; }
    Options options = original;
    options.render_product = path.str();
    options.timecode = ctx.frame_time;
    if (!ApplyUsdRenderOptions(ctx.stage, &options)) return -1;
    std::vector<Path> vars = product.ordered_vars;
    if (vars.empty()) vars.emplace_back();
    for (const auto& var_path : vars) {
      CaptureAov aov = CaptureAov::Color;
      if (!var_path.empty()) {
        RenderVarData var;
        if (!GetRenderVarData(ctx.stage, ctx.stage.GetPrimAtPath(var_path), &var, ctx.frame_time) ||
            var.source_type != "raw" || !CaptureSource(var.source_name, &aov)) {
          std::cerr << "Unsupported RenderVar: " << var_path.str() << "\n"; return -1;
        }
      }
      options.aov = aov;
      ctx.opt = options; ctx.width = options.width;
      ResolveCameraNext(ctx); ResolveBackPlateNext(ctx);
      const std::string file = prefix + "." + CaptureSuffix(path.str()) + "." + CaptureSuffix(var_path.str()) +
          (aov == CaptureAov::Color ? ".png" : ".pfm");
      if (!output_paths.insert(file).second) { std::cerr << "Duplicate product output path\n"; return -1; }
      const double seconds = RenderFrameTo(ctx, file);
      if (seconds < 0) return -1;
      total += seconds;
      std::cerr << "product " << path.str() << " var " << var_path.str() << " -> " << file << "\n";
    }
  }
  return total;
}

int RunRTPreviewNext(const Options &opt) {
  if (opt.all_render_products && (!opt.render_product.empty() || opt.aov_explicit || opt.rt_lod || opt.lod_stream)) {
    std::cerr << "--all-products cannot combine a single product/AOV or view-dependent LOD override\n";
    return EXIT_FAILURE;
  }
  RenderContext ctx;
  if (!BuildRenderContext(opt, ctx)) return EXIT_FAILURE;
  PrintLoadSummaryNext(ctx);
  if (opt.stats) PrintRTStats(ctx);

  // Animation: -frames renders one image per time code, re-evaluating geometry,
  // transforms and any animated camera at that time. The scene is parsed once;
  // each frame re-streams + rebuilds the BVH (geometry may deform).
  if (!opt.frames.empty()) {
    std::vector<double> times;
    if (!ParseFrameSpec(opt.frames, &times)) {
      std::cerr << "Invalid -frames FRAMESPEC: " << opt.frames << "\n";
      return EXIT_FAILURE;
    }
    if (opt.output.empty()) {
      std::cerr << "-frames requires an output path (use # for the frame "
                   "number, e.g. frame.####.png).\n";
      return EXIT_FAILURE;
    }
    // If the rendered geometry is static across time (only the camera and/or
    // nothing animates), the BVH built in BuildRenderContext is valid for every
    // frame -- reuse it and only re-resolve the camera per frame. Otherwise
    // re-stream + rebuild the BVH at each time.
    const bool geom_animated = SceneGeometryAnimated(ctx.stage, opt.mask);
    if (opt.stats) {
      std::cerr << "rt frames: " << times.size()
                << ", geometry animated: " << (geom_animated ? 1 : 0)
                << " (BVH " << (geom_animated ? "rebuilt per frame" : "reused")
                << ")\n";
    }
    for (double t : times) {
      if (geom_animated) {
        if (!ExtractAndBuildBVH(ctx, t)) return EXIT_FAILURE;
      } else {
        ctx.frame_time = t;  // static geometry: keep BVH, animate camera only
      }
      ResolveCameraNext(ctx);
      ResolveBackPlateNext(ctx);
      const std::string out = SubstituteFrame(opt.output, std::lround(t));
      const double secs = RenderCaptureProducts(ctx, opt, out);
      if (secs < 0.0) return EXIT_FAILURE;
      std::cerr << "frame " << t << " -> " << out << "  (" << secs << "s)\n";
    }
    return EXIT_SUCCESS;
  }

  const double secs = RenderCaptureProducts(ctx, opt, opt.output);
  if (secs < 0.0) return EXIT_FAILURE;
  if (opt.stats) std::cerr << "render seconds: " << secs << "\n";
  return EXIT_SUCCESS;
}

}  // namespace lusdr
