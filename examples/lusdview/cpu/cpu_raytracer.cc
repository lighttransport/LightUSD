// SPDX-License-Identifier: Apache-2.0
#include "cpu_raytracer.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "lightrt_c_tri.h"
#include "lightrt_mtlx_bridge.hh"  // kRtMaterialTexSlots
#include "ptex-sample.hh"
#include "shading-normal.hh"

namespace lusdview {

namespace {
inline void TransformPoint(const float m[12], const float p[3], float out[3]);
inline void TransformNormal(const float m[12], const float p[3], float out[3]);

inline float FresnelDielectric(float cosThetaI, float etaI, float etaT) {
  const float ci = std::clamp(std::fabs(cosThetaI), 0.0f, 1.0f);
  const float eta = etaI / std::max(etaT, 1.0e-6f);
  const float sinThetaT2 = eta * eta * std::max(0.0f, 1.0f - ci * ci);
  if (sinThetaT2 >= 1.0f) return 1.0f;
  const float ct = std::sqrt(std::max(0.0f, 1.0f - sinThetaT2));
  const float rs = (etaI * ci - etaT * ct) /
                   std::max(etaI * ci + etaT * ct, 1.0e-6f);
  const float rp = (etaT * ci - etaI * ct) /
                   std::max(etaT * ci + etaI * ct, 1.0e-6f);
  return 0.5f * (rs * rs + rp * rp);
}

// Packed per-material texture parameters (`PackRtMaterialTextureParams`):
// slots 0-5 carry six floats of UV transform each (m00, m01, tx, m10, m11, ty),
// and float 69 bit-packs which slot samples the second UV set.
const float* CpuTextureParams(const HostScene& scene, int material_id) {
  if (material_id < 0) return nullptr;
  const size_t base =
      static_cast<size_t>(material_id) * kRtMaterialTextureParamFloats;
  if (base + kRtMaterialTextureParamFloats > scene.matTexParam.size()) {
    return nullptr;
  }
  return &scene.matTexParam[base];
}

bool CpuSlotUsesSecondUvSet(const float* params, int slot) {
  if (!params) return false;
  return (static_cast<int>(params[69] + 0.5f) & (1 << slot)) != 0;
}

void CpuXformUvAt(const float* params, int offset, float u, float v,
                  float* out) {
  if (!params) {
    out[0] = u;
    out[1] = v;
    return;
  }
  const float* p = params + offset;
  out[0] = p[0] * u + p[1] * v + p[2];
  out[1] = p[3] * u + p[4] * v + p[5];
}

void CpuXformSlotUv(const float* params, int slot, float u, float v,
                    float* out) {
  CpuXformUvAt(params, slot * 6, u, v, out);
}

}

CpuRayTracer::~CpuRayTracer() { freeScene(); }

void CpuRayTracer::freeScene() {
  if (tlas_) {
    lrt_tlas_free(tlas_);
    tlas_ = nullptr;
  }
  for (lrt_tri_scene* blas : blasScenes_) {
    if (blas) lrt_tri_scene_free(blas);
  }
  blasScenes_.clear();
  tlasBlasTris_.clear();
  tlasPrimToGlobal_.clear();
  tlasRootToGroup_.clear();
  tlasValid_ = false;
  if (scene_) {
    lrt_tri_scene_free(scene_);
    scene_ = nullptr;
  }
  cpuTris_.clear();
  cpuNrms_.clear();
  cpuUv_.clear();
  cpuUv1_.clear();
  cpuMat_.clear();
  cpuInstance_.clear();
  cpuSourceTri_.clear();
}

int CpuRayTracer::SrcForInstancePrim(int inst, uint32_t prim) const {
  if (inst < 0 || static_cast<size_t>(inst) >= hs_.instances.size()) return -1;
  const int root = hs_.instances[static_cast<size_t>(inst)].blasRoot;
  if (root < 0 || static_cast<size_t>(root) >= tlasRootToGroup_.size())
    return -1;
  const int g = tlasRootToGroup_[static_cast<size_t>(root)];
  if (g < 0 || static_cast<size_t>(g) >= tlasPrimToGlobal_.size()) return -1;
  const std::vector<int>& map = tlasPrimToGlobal_[static_cast<size_t>(g)];
  return prim < map.size() ? map[prim] : -1;
}

bool CpuRayTracer::Intersect(const lrt_ray& ray, float* t, float* u, float* v,
                             int* inst, int* src) const {
  if (!tlas_) return false;
  lrt_tlas_hit h{};
  if (!lrt_tlas_intersect1(tlas_, &ray, 0xFFFFFFFFu, &h)) return false;
  *t = h.t;
  *u = h.u;
  *v = h.v;
  *inst = static_cast<int>(h.inst_id);
  *src = SrcForInstancePrim(*inst, h.prim_id);
  return *src >= 0;
}

namespace {
// Extract the six clip planes of a column-major projection*view (zero-to-one
// depth) and test instance bounds against them.
struct InstanceFrustum {
  float plane[6][4];
  bool valid = false;

  explicit InstanceFrustum(const float* m) {
    if (!m) return;
    // Row vectors of the column-major matrix.
    const float r0[4] = {m[0], m[4], m[8], m[12]};
    const float r1[4] = {m[1], m[5], m[9], m[13]};
    const float r2[4] = {m[2], m[6], m[10], m[14]};
    const float r3[4] = {m[3], m[7], m[11], m[15]};
    for (int c = 0; c < 4; ++c) {
      plane[0][c] = r3[c] + r0[c];  // left
      plane[1][c] = r3[c] - r0[c];  // right
      plane[2][c] = r3[c] + r1[c];  // bottom
      plane[3][c] = r3[c] - r1[c];  // top
      plane[4][c] = r2[c];          // near (zero-to-one depth)
      plane[5][c] = r3[c] - r2[c];  // far
    }
    valid = true;
  }

  // True when the box is entirely outside at least one plane.
  bool culls(const float bmin[3], const float bmax[3]) const {
    if (!valid) return false;
    for (int p = 0; p < 6; ++p) {
      // Positive vertex: the corner furthest along the plane normal.
      float x = plane[p][0] >= 0.0f ? bmax[0] : bmin[0];
      float y = plane[p][1] >= 0.0f ? bmax[1] : bmin[1];
      float z = plane[p][2] >= 0.0f ? bmax[2] : bmin[2];
      if (plane[p][0] * x + plane[p][1] * y + plane[p][2] * z +
              plane[p][3] <
          0.0f) {
        return true;
      }
    }
    return false;
  }
};

// World-space bounds of one instance, from its BLAS root bounds.
bool InstanceWorldBounds(const HostScene& hs, const Inst& inst, float bmin[3],
                         float bmax[3]) {
  if (inst.blasRoot < 0 || static_cast<size_t>(inst.blasRoot) >= hs.blas.size())
    return false;
  const Node& root = hs.blas[static_cast<size_t>(inst.blasRoot)];
  if (!(root.bmax[0] >= root.bmin[0])) return false;
  bmin[0] = bmin[1] = bmin[2] = 1.0e30f;
  bmax[0] = bmax[1] = bmax[2] = -1.0e30f;
  for (int corner = 0; corner < 8; ++corner) {
    const float c[3] = {(corner & 1) ? root.bmax[0] : root.bmin[0],
                        (corner & 2) ? root.bmax[1] : root.bmin[1],
                        (corner & 4) ? root.bmax[2] : root.bmin[2]};
    float w[3];
    TransformPoint(inst.o2w, c, w);
    for (int i = 0; i < 3; ++i) {
      bmin[i] = std::min(bmin[i], w[i]);
      bmax[i] = std::max(bmax[i], w[i]);
    }
  }
  return true;
}
}  // namespace

bool CpuRayTracer::build(const DrawScene& scene, size_t maxTris, size_t maxInstances,
                         std::string* err, float displacementScale,
                         BuildProgress* progress,
                         uint32_t purposeVisibleMask,
                         const float* viewProj16) {
  freeScene();
  hs_ = HostScene();  // drop the previous flatten before building the next one
  if (!BuildHostScene(scene, maxTris, maxInstances, displacementScale, &hs_, err,
                      progress, /*refitOut=*/nullptr, textureBudgetBytes_, nullptr,
                      purposeVisibleMask, /*nativePointCarriers=*/false)) {
    return false;
  }
  truncated_ = hs_.truncated;
  // One streamed source per packed PTex table entry. The loader's coarse atlas
  // stays as the fallback when the source cannot be opened (USDZ package
  // entries, non-UINT8 data), while the source path provides per-face mips and
  // a bounded on-demand page cache.
  ptex_.clear();
  ptex_.resize(hs_.textures.size());
  ptexAsset_.clear();
  ptexAsset_.resize(hs_.textures.size());
  for (size_t src = 0; src < hs_.sourceToTable.size() &&
                       src < scene.textures.size(); ++src) {
    const int tid = hs_.sourceToTable[src];
    if (tid < 0 || static_cast<size_t>(tid) >= hs_.textures.size() ||
        !hs_.textures[static_cast<size_t>(tid)].isPtex) {
      continue;
    }
    const DrawTextureCPU& dt = scene.textures[src];
    auto sampler = std::make_unique<CpuPtexSampler>(16ull * 1024ull * 1024ull);
    std::string ptexErr;
    bool ok = false;
    if (dt.ptexSourceDataShared && !dt.ptexSourceDataShared->empty()) {
      ok = sampler->OpenMemory(dt.ptexSourceDataShared->data(),
                               dt.ptexSourceDataShared->size(), &ptexErr);
    } else if (!dt.assetIdentifier.empty()) {
      ok = sampler->OpenFile(dt.assetIdentifier, &ptexErr);
    }
    if (ok) {
      ptex_[static_cast<size_t>(tid)] = std::move(sampler);
      ptexAsset_[static_cast<size_t>(tid)] = dt.assetIdentifier;
    }
  }
  if (hs_.triCount == 0 || hs_.instances.empty()) {
    if (err) *err = "no triangles";
    return false;
  }

  // lightrt_c's public triangle builder accepts one world-space triangle
  // stream, while HostScene deliberately stores each prototype once plus a
  // shared BLAS/TLAS. Expand the instances here so primary and shadow rays
  // see the same transforms as the GPU paths. Traverse each instance's BLAS
  // rather than assuming a mesh's triangles occupy one contiguous range: the
  // assembled BLAS is global and leaf order is part of its contract.
  // CPU RT flattens every instance into one triangle stream because lightrt_c's
  // builder takes a single world-space stream. Highly instanced scenes (e.g.
  // isBeach: ~1.9M instances, ~8B effective triangles) cannot be flattened in
  // full without exhausting memory, so cap the expansion to a bounded budget and
  // mark the build truncated instead of throwing std::bad_alloc.
  // ---------------------------------------------------------------------
  // Instance-aware traversal.
  //
  // Every shading attribute the renderer needs is `hs_`-indexed by source
  // triangle: the old flat stream was a per-instance, world-space *copy* of
  // exactly these arrays. Indexing `hs_` directly and resolving the instance
  // per hit removes the expansion entirely, so no instance is ever flattened
  // and nothing is truncated (isBeach: 8.09e9 effective triangles).
  // ---------------------------------------------------------------------
  const InstanceFrustum frustum(viewProj16);
  size_t culledInstances = 0, visibleInstances = 0;
  for (const Inst& inst : hs_.instances) {
    float ibmin[3], ibmax[3];
    if (InstanceWorldBounds(hs_, inst, ibmin, ibmax) &&
        frustum.culls(ibmin, ibmax)) {
      ++culledInstances;
    } else {
      ++visibleInstances;
    }
  }
  culledInstances_ = culledInstances;
  expandedInstances_ = visibleInstances;

  cpuTris_ = hs_.tris;
  cpuNrms_ = hs_.nrms;
  cpuUv_ = hs_.uv;
  cpuUv1_ = hs_.uv1;
  cpuMat_ = hs_.mat;
  cpuTan_ = hs_.tan;
  cpuBin_ = hs_.bin;
  cpuSourceTri_.resize(hs_.triCount);
  for (size_t i = 0; i < cpuSourceTri_.size(); ++i)
    cpuSourceTri_[i] = static_cast<int>(i);
  cpuInstance_.clear();
  triCount_ = hs_.triCount;
  truncated_ = false;

  lrt_tri_build_options opts{};
  opts.quality = LRT_TRI_BUILD_DEFAULT;
  opts.layout = LRT_TRI_LAYOUT_AUTO;
  opts.max_leaf_size = 0;  // default
  opts.num_threads = static_cast<unsigned>(
      std::max(1u, std::thread::hardware_concurrency()));

  // One BLAS per distinct instance blasRoot. Instances that report the same
  // prototype do NOT necessarily share a BLAS subtree (measured on the beach:
  // two instances of prototype 1 referenced subtrees of 7040 and 1664
  // triangles over disjoint source ranges), so the root is the sharing key.
  tlasRootToGroup_.assign(hs_.blas.size(), -1);
  std::vector<int> distinctRoots;
  for (const Inst& in : hs_.instances) {
    if (in.blasRoot < 0 || static_cast<size_t>(in.blasRoot) >= hs_.blas.size())
      continue;
    if (tlasRootToGroup_[static_cast<size_t>(in.blasRoot)] < 0) {
      tlasRootToGroup_[static_cast<size_t>(in.blasRoot)] =
          static_cast<int>(distinctRoots.size());
      distinctRoots.push_back(in.blasRoot);
    }
  }
  blasScenes_.assign(distinctRoots.size(), nullptr);
  tlasBlasTris_.assign(distinctRoots.size(), {});
  tlasPrimToGlobal_.assign(distinctRoots.size(), {});
  for (size_t g = 0; g < distinctRoots.size(); ++g) {
    std::vector<int> stack(1, distinctRoots[g]);
    while (!stack.empty()) {
      const int nodeIndex = stack.back();
      stack.pop_back();
      if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= hs_.blas.size())
        continue;
      const Node& node = hs_.blas[static_cast<size_t>(nodeIndex)];
      if (node.count <= 0) {
        stack.push_back(node.left);
        stack.push_back(node.right);
        continue;
      }
      if (node.left < 0 ||
          node.count > static_cast<int>(hs_.triCount) - node.left)
        continue;
      for (int t = 0; t < node.count; ++t) {
        const size_t src = static_cast<size_t>(node.left + t);
        tlasBlasTris_[g].insert(
            tlasBlasTris_[g].end(),
            hs_.tris.begin() + static_cast<ptrdiff_t>(src * 9),
            hs_.tris.begin() + static_cast<ptrdiff_t>(src * 9 + 9));
        tlasPrimToGlobal_[g].push_back(static_cast<int>(src));
      }
    }
    if (tlasPrimToGlobal_[g].empty()) continue;
    lrt_result berr = LRT_RESULT_OK;
    blasScenes_[g] = lrt_tri_scene_build(
        tlasBlasTris_[g].data(), tlasPrimToGlobal_[g].size(), &opts, &berr);
  }

  std::vector<lrt_instance> tlInstances;
  tlInstances.reserve(hs_.instances.size());
  for (size_t i = 0; i < hs_.instances.size(); ++i) {
    const Inst& in = hs_.instances[i];
    const int root = in.blasRoot;
    if (root < 0 || static_cast<size_t>(root) >= tlasRootToGroup_.size() ||
        tlasRootToGroup_[static_cast<size_t>(root)] < 0)
      continue;
    const int group = tlasRootToGroup_[static_cast<size_t>(root)];
    if (static_cast<size_t>(group) >= blasScenes_.size() ||
        !blasScenes_[static_cast<size_t>(group)])
      continue;
    lrt_instance li{};
    li.blas_id = static_cast<uint32_t>(group);
    std::memcpy(li.obj2world, in.o2w, sizeof(li.obj2world));
    li.instance_id = static_cast<uint32_t>(i);
    li.mask = 0xFFFFFFFFu;
    tlInstances.push_back(li);
  }
  if (!tlInstances.empty()) {
    lrt_result terr = LRT_RESULT_OK;
    tlas_ = lrt_tlas_build(blasScenes_.data(), blasScenes_.size(),
                           tlInstances.data(), tlInstances.size(), &opts,
                           &terr);
  }
  tlasValid_ = tlas_ != nullptr;
  if (!tlasValid_) {
    if (err) *err = "lrt_tlas_build failed";
    return false;
  }
  return true;
}

namespace {

// Barycentric-interpolate one of HostScene's 9-floats/tri vertex arrays
// (tris, nrms, cols) at a hit's (u, v); Moller-Trumbore convention: hit point
// = (1-u-v)*v0 + u*v1 + v*v2.
inline void Interp9(const float* base9, float u, float v, float out[3]) {
  const float w = 1.0f - u - v;
  for (int c = 0; c < 3; ++c) {
    out[c] = w * base9[c] + u * base9[3 + c] + v * base9[6 + c];
  }
}

// World-space vertices of a source triangle placed by one instance. Shading
// attributes are stored object-space and indexed by source triangle; only the
// hit's instance transform turns them into world space.
inline void WorldVerts9(const HostScene& hs, const Inst& in, int src,
                        float out[9]) {
  for (int v = 0; v < 3; ++v)
    TransformPoint(in.o2w, &hs.tris[static_cast<size_t>(src) * 9 + v * 3],
                   out + v * 3);
}

// Interpolate an object-space 3-vector array (normals/tangents/binormals) at a
// hit and rotate it into world space with the instance's normal transform.
inline void WorldVec9(const std::vector<float>& obj, const Inst& in, int src,
                      float bu, float bv, float out[3]) {
  float t[3];
  Interp9(&obj[static_cast<size_t>(src) * 9], bu, bv, t);
  TransformNormal(in.w2o, t, out);
}

// Same convention as Interp9 but for HostScene::uv/uv1 (2 floats/vertex).
inline void Interp6(const float* base6, float u, float v, float out[2]) {
  const float w = 1.0f - u - v;
  for (int c = 0; c < 2; ++c) {
    out[c] = w * base6[c] + u * base6[2 + c] + v * base6[4 + c];
  }
}

inline void Normalize3(float v[3]) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len > 1.0e-8f) {
    v[0] /= len; v[1] /= len; v[2] /= len;
  }
}

inline float PackedIesFactor(const float* lp, const float lightToPoint[3]) {
  if (!lp || std::fabs(lp[52] - 2.0f) > 0.25f) return 1.0f;
  float axis[3] = {lp[8], lp[9], lp[10]};
  Normalize3(axis);
  float d[3] = {lightToPoint[0], lightToPoint[1], lightToPoint[2]};
  Normalize3(d);
  const float c = std::clamp(axis[0] * d[0] + axis[1] * d[1] + axis[2] * d[2],
                             -1.0f, 1.0f);
  const float fy = std::clamp(std::acos(c) * 57.2957795131f / 60.0f,
                              0.0f, 3.0f);
  const int y0 = static_cast<int>(std::floor(fy));
  const int y1 = std::min(y0 + 1, 3);
  const float t = fy - static_cast<float>(y0);
  float axisX[3] = {lp[40], lp[41], lp[42]};
  float axisY[3] = {lp[44], lp[45], lp[46]};
  Normalize3(axisX); Normalize3(axisY);
  float az = std::atan2(d[0] * axisY[0] + d[1] * axisY[1] + d[2] * axisY[2],
                        d[0] * axisX[0] + d[1] * axisX[1] + d[2] * axisX[2]) *
             57.2957795131f;
  if (az < 0.0f) az += 360.0f;
  const float fx = az / 60.0f;
  const int x0 = static_cast<int>(std::floor(fx)) % 6;
  const int x1 = (x0 + 1) % 6;
  const float tx = fx - std::floor(fx);
  const float a0 = lp[56 + y0 * 6 + x0] +
                   (lp[56 + y0 * 6 + x1] - lp[56 + y0 * 6 + x0]) * tx;
  const float a1 = lp[56 + y1 * 6 + x0] +
                   (lp[56 + y1 * 6 + x1] - lp[56 + y1 * 6 + x0]) * tx;
  return a0 + (a1 - a0) * t;
}

inline void TransformPoint(const float m[12], const float p[3], float out[3]) {
  out[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  out[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  out[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

inline void TransformNormal(const float m[12], const float p[3], float out[3]) {
  out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2];
  out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2];
  out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2];
  Normalize3(out);
}

// Ptex atlases store one rectangle per face and a face-rect table in the
// atlas alpha texels (8 RGBA texels per face: x/y/width/height as u16 LE, two
// bytes per texel). Remap an intrinsic face (u,v) into the atlas exactly as
// the shaders' ptexUv() does: clamp inside the inner rectangle so bilinear
// taps cannot bleed into another face.
inline bool PtexRemapUv(const std::vector<HostTextureDesc>& textures,
                        const std::vector<uint8_t>& texels, int texId,
                        int face, float* u, float* v) {
  if (texId < 0 || static_cast<size_t>(texId) >= textures.size()) return false;
  const HostTextureDesc& tex = textures[static_cast<size_t>(texId)];
  if (!tex.isPtex || face < 0 || face >= tex.ptexFaceCount ||
      tex.width <= 0 || tex.height <= 0 || tex.ptexRectTexelOffset < 0) {
    return false;
  }
  const size_t texel_count =
      static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height);
  const size_t first = static_cast<size_t>(tex.ptexRectTexelOffset) +
                       static_cast<size_t>(face) * 8u;
  const size_t base = static_cast<size_t>(tex.offset);
  if (first + 8u > texel_count || base > texels.size()) return false;
  uint16_t values[4] = {0, 0, 0, 0};
  for (size_t byte = 0; byte < 8u; ++byte) {
    const size_t index = base + (first + byte) * 4u + 3u;
    if (index >= texels.size()) return false;
    values[byte / 2u] |= static_cast<uint16_t>(texels[index])
                         << ((byte & 1u) * 8u);
  }
  if (values[2] == 0 || values[3] == 0) return false;
  const float span_u = static_cast<float>(values[2] - 1u);
  const float span_v = static_cast<float>(values[3] - 1u);
  const float pu = std::clamp(*u, 0.0f, 1.0f);
  const float pv = std::clamp(*v, 0.0f, 1.0f);
  const float px = static_cast<float>(values[0]) + pu * span_u;
  const float py = static_cast<float>(values[1]) + (1.0f - pv) * span_v;
  *u = (px + 0.5f) / static_cast<float>(tex.width);
  *v = (py + 0.5f) / static_cast<float>(tex.height);
  return true;
}

// Bilinear-sample an RGBA8 HostTextureDesc at
// its base mip. Deliberately mirrors sampleTexLevel/wrapCoord in
// raytracer_kernel_src.txt (the CUDA/HIP kernel) so CPU RT matches the GPU
// tracers: same wrap-mode encoding (0 and 3 = clamp, 2 = mirror, everything
// else = repeat), same [0,1] -> [0, size-1] mapping with clamp-to-edge on the
// upper neighbour, and NO V flip -- HostScene::uv already carries the
// convention the kernel samples with. RGB texels honor HostTextureDesc::srgb;
// alpha remains an unmodified coverage/value channel.
inline void SampleTextureBilinear(const std::vector<HostTextureDesc>& textures,
                                  const std::vector<uint8_t>& texels, int texId,
                                  float u, float v, float out[3],
                                  int ptex_face = -1,
                                  const CpuPtexSampler* ptex = nullptr,
                                  uint32_t ptex_mip = 0,
                                  const CpuPtexSampler::Footprint* ptex_fp =
                                      nullptr) {
  if (texId < 0 || static_cast<size_t>(texId) >= textures.size()) {
    out[0] = out[1] = out[2] = 1.0f;
    return;
  }
  if (ptex && ptex_face >= 0 && ptex->valid()) {
    float rgb[3];
    const bool srgb = textures[static_cast<size_t>(texId)].srgb != 0;
    const bool ok = ptex_fp
        ? ptex->SampleFootprint(static_cast<uint32_t>(ptex_face), *ptex_fp,
                                u, v, srgb, rgb)
        : ptex->Sample(static_cast<uint32_t>(ptex_face), ptex_mip, u, v,
                       srgb, rgb);
    if (ok) {
      out[0] = rgb[0];
      out[1] = rgb[1];
      out[2] = rgb[2];
      return;
    }
  }
  if (ptex_face >= 0) {
    PtexRemapUv(textures, texels, texId, ptex_face, &u, &v);
  }
  const HostTextureDesc* desc = &textures[static_cast<size_t>(texId)];
  if (desc->isUdim) {
    const int tileU = static_cast<int>(std::floor(u));
    const int tileV = static_cast<int>(std::floor(v));
    const int lut = tileU + tileV * 10;
    if (lut < 0 || lut >= 100 || desc->udimLayer[lut] < 0 ||
        static_cast<size_t>(desc->udimLayer[lut]) >= textures.size()) {
      out[0] = out[1] = out[2] = 1.0f;
      return;
    }
    texId = desc->udimLayer[lut];
    u -= static_cast<float>(tileU);
    v -= static_cast<float>(tileV);
    desc = &textures[static_cast<size_t>(texId)];
  }
  // Ptex atlases are sampled with the remapped face-rect UV above; the atlas
  // itself is an ordinary RGBA image from here on.
  if (desc->width <= 0 || desc->height <= 0) {
    out[0] = out[1] = out[2] = 1.0f;
    return;
  }
  const HostTextureDesc& d = *desc;
  auto srgbToLinear = [](float x) {
    return x <= 0.04045f ? x / 12.92f
                         : std::pow((x + 0.055f) / 1.055f, 2.4f);
  };
  auto wrapCoord = [](float x, int mode) -> float {
    if (mode == 0 || mode == 3) return std::clamp(x, 0.0f, 1.0f);  // clamp
    if (mode == 2) {                                               // mirror
      const float t = std::floor(x);
      const float f = x - t;
      return (static_cast<int>(t) & 1) ? (1.0f - f) : f;
    }
    return x - std::floor(x);  // repeat
  };
  const float su = wrapCoord(u, d.wrapS) * static_cast<float>(d.width - 1);
  const float sv = wrapCoord(v, d.wrapT) * static_cast<float>(d.height - 1);
  const int x0 = static_cast<int>(std::floor(su));
  const int y0 = static_cast<int>(std::floor(sv));
  const int x1 = (x0 + 1 < d.width) ? (x0 + 1) : (d.width - 1);
  const int y1 = (y0 + 1 < d.height) ? (y0 + 1) : (d.height - 1);
  const float tx = su - static_cast<float>(x0);
  const float ty = sv - static_cast<float>(y0);
  auto texel = [&](int x, int y, float rgb[3]) {
    x = std::clamp(x, 0, d.width - 1);
    y = std::clamp(y, 0, d.height - 1);
    const size_t idx = static_cast<size_t>(d.offset) +
                       (static_cast<size_t>(y) * d.width + x) * 4;
    if (idx + 2 >= texels.size()) { rgb[0] = rgb[1] = rgb[2] = 1.0f; return; }
    rgb[0] = texels[idx + 0] / 255.0f;
    rgb[1] = texels[idx + 1] / 255.0f;
    rgb[2] = texels[idx + 2] / 255.0f;
    if (d.srgb) {
      rgb[0] = srgbToLinear(rgb[0]);
      rgb[1] = srgbToLinear(rgb[1]);
      rgb[2] = srgbToLinear(rgb[2]);
    }
  };
  float c00[3], c10[3], c01[3], c11[3];
  texel(x0, y0, c00); texel(x1, y0, c10);
  texel(x0, y1, c01); texel(x1, y1, c11);
  for (int c = 0; c < 3; ++c) {
    const float top = c00[c] * (1 - tx) + c10[c] * tx;
    const float bot = c01[c] * (1 - tx) + c11[c] * tx;
    out[c] = top * (1 - ty) + bot * ty;
  }
}

// Optional PTex page-cache diagnostics for tests: one line per compiled PTex
// source with hit/miss/eviction counts and the mip histogram.
inline void LogCpuPtexStats(
    const std::vector<std::unique_ptr<CpuPtexSampler>>& sources) {
  if (!std::getenv("LIGHTUSD_CPU_PTEX_STATS")) return;
  for (size_t i = 0; i < sources.size(); ++i) {
    const CpuPtexSampler* source = sources[i].get();
    if (!source) continue;
    const auto& st = source->stats();
    std::fprintf(stderr,
                 "[cpu-ptex] tex=%zu hits=%llu misses=%llu evictions=%llu "
                 "resident=%llu peak=%llu samples=%llu aniso=%llu taps=%llu "
                 "mips=",
                 i, static_cast<unsigned long long>(st.hits),
                 static_cast<unsigned long long>(st.misses),
                 static_cast<unsigned long long>(st.evictions),
                 static_cast<unsigned long long>(st.residentBytes),
                 static_cast<unsigned long long>(st.peakResidentBytes),
                 static_cast<unsigned long long>(st.samples),
                 static_cast<unsigned long long>(st.anisotropicSamples),
                 static_cast<unsigned long long>(st.anisotropicTaps));
    for (int m = 0; m < 16; ++m) {
      std::fprintf(stderr, "%s%llu", m ? "," : "",
                   static_cast<unsigned long long>(st.mipSamples[m]));
    }
    std::fprintf(stderr, "\n");
  }
}

// Per-pixel diagnostic of the PTex base-colour sample: an element-scale
// oracle can decode the same (face, mip, u, v) with an independent decoder.
struct CpuPtexProbe {
  uint32_t tex{~0u};
  uint32_t face{~0u};
  int mip{0};
  int srgb{0};
  float u{0.0f}, v{0.0f};
  float rgb[3]{0.0f, 0.0f, 0.0f};
};

inline uint32_t HashColor(uint32_t id) {
  uint32_t x = id * 2654435761u + 1u;
  x ^= x >> 16; x *= 0x85ebca6bu; x ^= x >> 13; x *= 0xc2b2ae35u; x ^= x >> 16;
  return x;
}

// Scalar material lanes (opacity, roughness, masks) need the same UDIM
// resolution and bilinear filtering as RGB graph/image lanes.
inline float SampleTextureBilinearChannel(
    const std::vector<HostTextureDesc>& textures,
    const std::vector<uint8_t>& texels, int texId, float u, float v,
    int channel, int ptex_face = -1, const CpuPtexSampler* ptex = nullptr,
    uint32_t ptex_mip = 0,
    const CpuPtexSampler::Footprint* ptex_fp = nullptr) {
  if (texId < 0 || static_cast<size_t>(texId) >= textures.size()) return 1.0f;
  if (ptex && ptex_face >= 0 && ptex->valid()) {
    float value = 0.0f;
    const bool ok = ptex_fp
        ? ptex->SampleChannelFootprint(static_cast<uint32_t>(ptex_face),
                                       *ptex_fp, u, v, channel, &value)
        : ptex->SampleChannel(static_cast<uint32_t>(ptex_face), ptex_mip, u,
                              v, channel, &value);
    if (ok) {
      return value;
    }
  }
  if (ptex_face >= 0) {
    PtexRemapUv(textures, texels, texId, ptex_face, &u, &v);
  }
  const HostTextureDesc* desc = &textures[static_cast<size_t>(texId)];
  if (desc->isUdim) {
    const int tileU = static_cast<int>(std::floor(u));
    const int tileV = static_cast<int>(std::floor(v));
    const int lut = tileU + tileV * 10;
    if (lut < 0 || lut >= 100 || desc->udimLayer[lut] < 0 ||
        static_cast<size_t>(desc->udimLayer[lut]) >= textures.size()) return 1.0f;
    texId = desc->udimLayer[lut];
    u -= static_cast<float>(tileU);
    v -= static_cast<float>(tileV);
    desc = &textures[static_cast<size_t>(texId)];
  }
  if (desc->width <= 0 || desc->height <= 0) return 1.0f;
  const HostTextureDesc& d = *desc;
  channel = std::clamp(channel, 0, 3);
  auto srgbToLinear = [](float x) {
    return x <= 0.04045f ? x / 12.92f
                         : std::pow((x + 0.055f) / 1.055f, 2.4f);
  };
  auto wrapCoord = [](float x, int mode) -> float {
    if (mode == 0 || mode == 3) return std::clamp(x, 0.0f, 1.0f);
    if (mode == 2) {
      const float t = std::floor(x);
      const float f = x - t;
      return (static_cast<int>(t) & 1) ? (1.0f - f) : f;
    }
    return x - std::floor(x);
  };
  const float su = wrapCoord(u, d.wrapS) * static_cast<float>(d.width - 1);
  const float sv = wrapCoord(v, d.wrapT) * static_cast<float>(d.height - 1);
  const int x0 = static_cast<int>(std::floor(su));
  const int y0 = static_cast<int>(std::floor(sv));
  const int x1 = (x0 + 1 < d.width) ? x0 + 1 : d.width - 1;
  const int y1 = (y0 + 1 < d.height) ? y0 + 1 : d.height - 1;
  const float tx = su - static_cast<float>(x0);
  const float ty = sv - static_cast<float>(y0);
  auto texel = [&](int x, int y) {
    x = std::clamp(x, 0, d.width - 1);
    y = std::clamp(y, 0, d.height - 1);
    const size_t idx = static_cast<size_t>(d.offset) +
                       (static_cast<size_t>(y) * d.width + x) * 4u +
                       static_cast<size_t>(channel);
    if (idx >= texels.size()) return 1.0f;
    const float value = texels[idx] / 255.0f;
    return d.srgb && channel < 3 ? srgbToLinear(value) : value;
  };
  const float top = texel(x0, y0) * (1.0f - tx) + texel(x1, y0) * tx;
  const float bot = texel(x0, y1) * (1.0f - tx) + texel(x1, y1) * tx;
  return top * (1.0f - ty) + bot * ty;
}

// Interpolated, transformed UV for one semantic lane.
bool CpuLaneUv(const HostScene& scene, int material_id, int tri, float bu,
               float bv, bool use_second_set, int xform_offset,
               const std::vector<float>& uv0, const std::vector<float>& uv1,
               float* out) {
  if (static_cast<size_t>(tri) * 6 + 5 >= uv0.size()) return false;
  const bool use_uv1 = use_second_set &&
                       static_cast<size_t>(tri) * 6 + 5 < uv1.size();
  const float* src = (use_uv1 ? uv1 : uv0).data() +
                     static_cast<size_t>(tri) * 6;
  float uv[2];
  Interp6(src, bu, bv, uv);
  CpuXformUvAt(CpuTextureParams(scene, material_id), xform_offset, uv[0],
               uv[1], out);
  return true;
}

bool CpuSampleSemanticScalar(const HostScene& scene, int material_id, int tex,
                             int tri, float bu, float bv,
                             bool use_second_set, int xform_offset,
                             int channel, const std::vector<float>& uv0,
                             const std::vector<float>& uv1, float* out,
                             int ptex_face = -1,
                             const CpuPtexSampler* ptex = nullptr,
                             uint32_t ptex_mip = 0,
                             const CpuPtexSampler::Footprint* ptex_fp =
                                 nullptr) {
  if (tex < 0) return false;
  float uv[2];
  if (!CpuLaneUv(scene, material_id, tri, bu, bv, use_second_set,
                 xform_offset, uv0, uv1, uv)) {
    return false;
  }
  *out = SampleTextureBilinearChannel(scene.textures, scene.texels, tex,
                                      uv[0], uv[1], channel, ptex_face, ptex,
                                      ptex_mip, ptex_fp);
  return true;
}

bool CpuSampleSemanticRgb(const HostScene& scene, int material_id, int tex,
                          int tri, float bu, float bv, bool use_second_set,
                          int xform_offset, const std::vector<float>& uv0,
                          const std::vector<float>& uv1, float out[3],
                          int ptex_face = -1,
                          const CpuPtexSampler* ptex = nullptr,
                          uint32_t ptex_mip = 0,
                          const CpuPtexSampler::Footprint* ptex_fp = nullptr) {
  if (tex < 0) return false;
  float uv[2];
  if (!CpuLaneUv(scene, material_id, tri, bu, bv, use_second_set,
                 xform_offset, uv0, uv1, uv)) {
    return false;
  }
  SampleTextureBilinear(scene.textures, scene.texels, tex, uv[0], uv[1], out,
                        ptex_face, ptex, ptex_mip, ptex_fp);
  return true;
}

uint32_t MtlxRotl32(uint32_t x, int k) {
  return (x << k) | (x >> (32 - k));
}
uint32_t MtlxBjFinal(uint32_t a, uint32_t b, uint32_t c) {
  c ^= b; c -= MtlxRotl32(b,14); a ^= c; a -= MtlxRotl32(c,11);
  b ^= a; b -= MtlxRotl32(a,25); c ^= b; c -= MtlxRotl32(b,16);
  a ^= c; a -= MtlxRotl32(c,4); b ^= a; b -= MtlxRotl32(a,14);
  c ^= b; c -= MtlxRotl32(b,24); return c;
}
void MtlxBjMix(uint32_t& a,uint32_t& b,uint32_t& c){
  a-=c;a^=MtlxRotl32(c,4);c+=b;b-=a;b^=MtlxRotl32(a,6);a+=c;
  c-=b;c^=MtlxRotl32(b,8);b+=a;a-=c;a^=MtlxRotl32(c,16);c+=b;
  b-=a;b^=MtlxRotl32(a,19);a+=c;c-=b;c^=MtlxRotl32(b,4);b+=a;
}
float MtlxCellNoise(int x, int y, int z, bool is3d) {
  const uint32_t seed = 0xdeadbeefu + ((is3d ? 3u : 2u) << 2u) + 13u;
  const uint32_t hash = MtlxBjFinal(seed + static_cast<uint32_t>(x),
      seed + static_cast<uint32_t>(y),
      seed + (is3d ? static_cast<uint32_t>(z) : 0u));
  return static_cast<float>(hash) / static_cast<float>(0xffffffffu);
}
float MtlxCellNoiseChannel(int x, int y, int z, bool is3d, int channel) {
  const uint32_t seed = 0xdeadbeefu + ((is3d ? 3u : 2u) << 2u) + 13u;
  uint32_t hash = MtlxBjFinal(seed + static_cast<uint32_t>(x),
      seed + static_cast<uint32_t>(y),
      seed + (is3d ? static_cast<uint32_t>(z) : 0u));
  if (channel >= 0) hash = (hash >> (channel * 8)) & 0xffu;
  return static_cast<float>(hash) /
      static_cast<float>(channel >= 0 ? 0xffu : 0xffffffffu);
}
std::array<float,3> MtlxCellNoiseVec3(int x,int y,int z,int w,bool hasW){
  uint32_t a=0xdeadbeefu+((hasW?5u:4u)<<2u)+13u,b=a,c=a;
  a+=static_cast<uint32_t>(x);b+=static_cast<uint32_t>(y);
  c+=static_cast<uint32_t>(z);MtlxBjMix(a,b,c);
  if(hasW)a+=static_cast<uint32_t>(w);
  const float scale=1.0f/static_cast<float>(0xffffffffu);
  return hasW?std::array<float,3>{MtlxBjFinal(a,b,c)*scale,
      MtlxBjFinal(a,b+1u,c)*scale,MtlxBjFinal(a,b+2u,c)*scale}:
      std::array<float,3>{MtlxBjFinal(a,b,c)*scale,
      MtlxBjFinal(a+1u,b,c)*scale,MtlxBjFinal(a+2u,b,c)*scale};
}
std::array<float,4> MtlxFlake(const std::array<float,4>* values,int start,int count,
                              int output){
  const float size=std::max(std::fabs(values[start][0]),1.0e-8f);
  const float roughness=values[std::min(start+1,count-1)][0];
  const float coverage=std::clamp(values[std::min(start+2,count-1)][0],0.0f,1.0f);
  const auto position=values[std::min(start+3,count-1)];
  const auto normal=values[std::min(start+4,count-1)];
  const auto tangent=values[std::min(start+5,count-1)];
  const auto bitangent=values[std::min(start+6,count-1)];
  const float xx=coverage*coverage;
  const float probability=(-26.19771808f*xx+26.39663835f*coverage)/
      (85.53857017f*xx*coverage-102.35069432f*xx-101.42634862f*coverage+118.45082288f);
  const float px=position[0]/size,py=position[1]/size,pz=position[2]/size;
  const int bx=static_cast<int>(std::floor(px)),by=static_cast<int>(std::floor(py)),bz=static_cast<int>(std::floor(pz));
  float priority=0;int cx=0,cy=0,cz=0;const float diameter=.86602540378f;
  for(int i=-1;i<=1;i++)for(int j=-1;j<=1;j++)for(int k=-1;k<=1;k++){
    const int x=bx+i,y=by+j,z=bz+k;float qx=px-x-.5f,qy=py-y-.5f,qz=pz-z-.5f;
    if(qx*qx+qy*qy+qz*qz>=diameter*diameter*3)continue;
    if(MtlxCellNoise(x,y,z,true)>probability)continue;
    const float candidate=MtlxCellNoiseVec3(x,y,z,3,true)[0];if(candidate<priority)continue;
    const auto rot=MtlxCellNoiseVec3(x,y,z,0,false);const float theta=6.28318530718f*rot[0],phi=6.28318530718f*rot[1],zz=2*rot[2];
    const float rr=std::sqrt(zz),vx=std::sin(phi)*rr,vy=std::cos(phi)*rr,vz=std::sqrt(2-zz),s=std::sin(theta),c=std::cos(theta),sx=vx*c-vy*s,sy=vx*s+vy*c;
    const float rx=(vx*sx-c)*qx+(vy*sx+s)*qy+vz*sx*qz;
    const float ry=(vx*sy-s)*qx+(vy*sy-c)*qy+vz*sy*qz;
    const float rz=vx*vz*qx+vy*vz*qy+(1-zz)*qz;
    if(std::fabs(rx)<=diameter&&std::fabs(ry)<=diameter&&std::fabs(rz)<=diameter){priority=candidate;cx=x;cy=y;cz=z;}
  }
  if(priority<=0)return output==3?normal:std::array<float,4>{0,0,0,0};
  const auto noise=MtlxCellNoiseVec3(cx,cy,cz,2,true);const float random=noise[2];
  if(output==0)return {std::floor(random*16777215.0f),0,0,0};
  if(output==1)return {random,random,random,random};
  if(output==2)return {priority,priority,priority,priority};
  const float phi=6.28318530718f*noise[0],tanTheta=roughness*roughness*std::sqrt(noise[1])/std::sqrt(std::max(1-noise[1],1.0e-8f));
  const float sinTheta=tanTheta/std::sqrt(1+tanTheta*tanTheta),cosTheta=std::sqrt(std::max(1-sinTheta*sinTheta,0.0f));
  std::array<float,4> out{tangent[0]*std::cos(phi)*sinTheta+bitangent[0]*std::sin(phi)*sinTheta+normal[0]*cosTheta,
      tangent[1]*std::cos(phi)*sinTheta+bitangent[1]*std::sin(phi)*sinTheta+normal[1]*cosTheta,
      tangent[2]*std::cos(phi)*sinTheta+bitangent[2]*std::sin(phi)*sinTheta+normal[2]*cosTheta,0};
  const float len=std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);if(len>1.0e-8f){out[0]/=len;out[1]/=len;out[2]/=len;}return out;
}
std::array<float,4> MtlxWorley(const std::array<float,4>& p, bool is3d,
                               float jitter, int style, int channels) {
  const int ix=static_cast<int>(std::floor(p[0]));
  const int iy=static_cast<int>(std::floor(p[1]));
  const int iz=is3d?static_cast<int>(std::floor(p[2])):0;
  const float local[3]={p[0]-ix,p[1]-iy,is3d?p[2]-iz:0.0f};
  float best[3]={1.0e6f,1.0e6f,1.0e6f};
  float feature[3]={0,0,0};
  for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)
    for(int z=is3d?-1:0;z<=(is3d?1:0);++z){
      const int hx=ix+x,hy=iy+y,hz=iz+z;
      float candidate[3]{0,0,0};
      for(int lane=0;lane<(is3d?3:2);++lane){
        const float rnd=MtlxCellNoiseChannel(hx,hy,hz,is3d,lane);
        candidate[lane]=(lane==0?x:(lane==1?y:z))+(rnd-0.5f)*jitter+0.5f;
      }
      const float dx=candidate[0]-local[0],dy=candidate[1]-local[1];
      const float dz=is3d?candidate[2]-local[2]:0.0f;
      const float d=dx*dx+dy*dy+dz*dz;
      if(d<best[0]){best[2]=best[1];best[1]=best[0];best[0]=d;
        feature[0]=candidate[0];feature[1]=candidate[1];feature[2]=candidate[2];}
      else if(d<best[1]){best[2]=best[1];best[1]=d;}
      else if(d<best[2])best[2]=d;
    }
  std::array<float,4> out{0,0,0,0};
  if(style==1){
    const int fx=static_cast<int>(std::floor(feature[0]-local[0]+p[0]));
    const int fy=static_cast<int>(std::floor(feature[1]-local[1]+p[1]));
    const int fz=is3d?static_cast<int>(std::floor(feature[2]-local[2]+p[2])):0;
    for(int lane=0;lane<channels;++lane)
      out[lane]=MtlxCellNoiseChannel(fx,fy,fz,is3d,channels==1?-1:lane);
  }else for(int lane=0;lane<channels;++lane)out[lane]=std::sqrt(best[lane]);
  return out;
}
float MtlxCircleShape(float x,float y,float r){return x*x+y*y<=r*r?1.0f:0.0f;}
float MtlxCloverShape(float x,float y,float r){const float sx=2*x,sy=2*y,r2=r*r,dx[4]={sx+r,sx-r,sx,sx},dy[4]={sy,sy,sy-r,sy+r};for(int i=0;i<4;i++)if(dx[i]*dx[i]+dy[i]*dy[i]<=r2)return 1;return 0;}
float MtlxHexShape(float x,float y,float r){float px=std::fabs(y),py=std::fabs(x);const float kx=-.866025f,ky=.5f,kz=.57735f;const float projection=std::min(kx*px+ky*py,0.0f);px-=2*projection*kx;py-=2*projection*ky;px-=std::clamp(px,-kz*r,kz*r);py-=r;return std::sqrt(px*px+py*py)*(py<0?-1.0f:1.0f)<=0?1.0f:0.0f;}
float MtlxPattern(float x,float y,float parameter,bool staggered,int kind){
  if(kind==0||kind==1){float row=std::fmod(y,2.0f);if(staggered&&row>1.0f)x+=.5f;float nx=2*std::fmod(x,1.0f)-1,ny=2*std::fmod(y,1.0f)-1;if(kind==0)return (std::fabs(nx)>1-parameter||std::fabs(ny)>1-parameter)?1:0;const float inv=.70710678118f;return (std::fabs(nx-ny)*inv<=parameter||std::fabs(nx+ny)*inv<=parameter)?1:0;}
  auto shape=[&](float sx,float sy,float r){return kind==2?MtlxCircleShape(sx,sy,r):(kind==3?MtlxCloverShape(sx,sy,r):MtlxHexShape(sx,sy,r));};
  if(!staggered)return shape(2*std::fmod(x,1.0f)-1,2*std::fmod(y,1.0f)-1,parameter);
  const float half=kind==3?.5f:.866025f,period=2*half,shift=std::fmod(y,period)>half?.5f:0;
  const float mx=std::fmod(x+shift,1.0f),my=std::fmod(y,half),r=parameter*.5f;
  return std::max(shape(mx,my,r),std::max(shape(1-mx,my,r),shape(mx-.5f,half-my,r)));
}
float MtlxGradient2(uint32_t hash, float x, float y) {
  const uint32_t h = hash & 7u;
  const float u = h < 4u ? x : y;
  const float v = 2.0f * (h < 4u ? y : x);
  return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}
float MtlxPerlin2(float x, float y, int channel) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
  const float u = fx*fx*fx*(fx*(fx*6.0f-15.0f)+10.0f);
  const float v = fy*fy*fy*(fy*(fy*6.0f-15.0f)+10.0f);
  const auto hash = [channel](int px, int py) {
    const uint32_t seed = 0xdeadbeefu + (2u << 2u) + 13u;
    uint32_t h = MtlxBjFinal(seed + static_cast<uint32_t>(px),
                            seed + static_cast<uint32_t>(py), seed);
    if (channel >= 0) h = (h >> (channel * 8)) & 0xffu;
    return h;
  };
  const float a=MtlxGradient2(hash(ix,iy),fx,fy);
  const float b=MtlxGradient2(hash(ix+1,iy),fx-1.0f,fy);
  const float c=MtlxGradient2(hash(ix,iy+1),fx,fy-1.0f);
  const float d=MtlxGradient2(hash(ix+1,iy+1),fx-1.0f,fy-1.0f);
  return 0.6616f*((1.0f-v)*(a+(b-a)*u)+v*(c+(d-c)*u));
}
float MtlxGrad3(uint32_t hash,float x,float y,float z){
  const uint32_t h=hash&15u;const float u=h<8u?x:y;
  const float v=h<4u?y:((h==12u||h==14u)?x:z);
  return ((h&1u)?-u:u)+((h&2u)?-v:v);
}
float MtlxPerlin3(float x,float y,float z,int channel){
  const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y)),iz=static_cast<int>(std::floor(z));
  const float fx=x-ix,fy=y-iy,fz=z-iz;
  const float u=fx*fx*fx*(fx*(fx*6-15)+10),v=fy*fy*fy*(fy*(fy*6-15)+10),w=fz*fz*fz*(fz*(fz*6-15)+10);
  auto grad=[&](int dx,int dy,int dz){uint32_t seed=0xdeadbeefu+(3u<<2u)+13u,h=MtlxBjFinal(seed+static_cast<uint32_t>(ix+dx),seed+static_cast<uint32_t>(iy+dy),seed+static_cast<uint32_t>(iz+dz));if(channel>=0)h=(h>>(channel*8))&255u;return MtlxGrad3(h,fx-dx,fy-dy,fz-dz);};
  const float x00=grad(0,0,0)+u*(grad(1,0,0)-grad(0,0,0));
  const float x10=grad(0,1,0)+u*(grad(1,1,0)-grad(0,1,0));
  const float x01=grad(0,0,1)+u*(grad(1,0,1)-grad(0,0,1));
  const float x11=grad(0,1,1)+u*(grad(1,1,1)-grad(0,1,1));
  return .9820f*((x00+v*(x10-x00))+w*((x01+v*(x11-x01))-(x00+v*(x10-x00))));
}

std::array<float,4> MtlxBlackbody(float kelvin){
  const float temperature=std::clamp(kelvin,800.0f,25000.0f);
  const float t=1000.0f/temperature,t2=t*t,t3=t2*t;
  const float x=temperature<4000.0f?
      -0.2661239f*t3-0.2343580f*t2+0.8776956f*t+0.179910f:
      -3.0258469f*t3+2.1070379f*t2+0.2226347f*t+0.240390f;
  const float x2=x*x,x3=x2*x;
  const float y=temperature<2222.0f?
      -1.1063814f*x3-1.34811020f*x2+2.18555832f*x-0.20219683f:
      (temperature<4000.0f?
       -0.9549476f*x3-1.37418593f*x2+2.09137015f*x-0.16748867f:
       3.0817580f*x3-5.87338670f*x2+3.75112997f*x-0.37001483f);
  if(y<=0.0f)return {1,1,1,1};
  const float X=x/y,Z=(1.0f-x-y)/y;
  return {std::max(3.2406f*X-1.5372f-0.4986f*Z,0.0f),
          std::max(-0.9689f*X+1.8758f+0.0415f*Z,0.0f),
          std::max(0.0557f*X-0.2040f+1.0570f*Z,0.0f),1.0f};
}

std::array<float, 4> EvalCpuMaterialXGraph(
    const HostScene& scene, int materialId, int route, float u, float v,
    float u1, float v1, const float* normal = nullptr,
    const float* position = nullptr, const float* viewDirection = nullptr,
    float time = 0.0f, float frame = 0.0f, int geomPropTri = -1,
    const float* tangent = nullptr, const float* bitangent = nullptr) {
  constexpr int kStride = kRtMaterialGraphFloats;
  std::array<float, 4> zero{0.0f, 0.0f, 0.0f, 0.0f};
  const size_t base = static_cast<size_t>(materialId) * kStride;
  if (materialId < 0 || route < 0 ||
      base + kRtMaterialGraphHeaderFloats > scene.matGraph.size()) return zero;
  const int count = std::clamp(static_cast<int>(scene.matGraph[base]), 0,
                               kRtMaterialGraphMaxNodes);
  const int wanted = static_cast<int>(std::floor(
      scene.matGraph[base + 1 + route] + 0.5f));
  if (count <= 0 || wanted < 0 || wanted >= count) return zero;
  std::array<std::array<float, 4>, kRtMaterialGraphMaxNodes> values{};
  // CompileMaterialXGraphRuntime canonicalizes dependencies before consumers
  // and rejects cycles, so one pass is sufficient. The historical 64-pass
  // fixed-point loop multiplied graph shading cost without changing results.
  for (int i = 0; i < count; ++i) {
      const size_t p = base + kRtMaterialGraphHeaderFloats +
                       static_cast<size_t>(i) * kRtMaterialGraphNodeFloats;
      const int op = static_cast<int>(scene.matGraph[p] + 0.5f);
      const int ia = static_cast<int>(std::floor(scene.matGraph[p + 1] + 0.5f));
      const int ib = static_cast<int>(std::floor(scene.matGraph[p + 2] + 0.5f));
      const int ic = static_cast<int>(std::floor(scene.matGraph[p + 3] + 0.5f));
      const auto fallback = [&](int input) {
        return std::array<float, 4>{
            scene.matGraph[p + 4 + input * 4],
            scene.matGraph[p + 5 + input * 4],
            scene.matGraph[p + 6 + input * 4],
            scene.matGraph[p + 7 + input * 4]};
      };
      const auto a = ia >= 0 && ia < count ? values[ia] : fallback(0);
      const auto b = ib >= 0 && ib < count ? values[ib] : fallback(1);
      const auto c = ic >= 0 && ic < count ? values[ic] : fallback(2);
      const int textureId = static_cast<int>(std::floor(
          scene.matGraph[p + 16] + 0.5f));
      auto& dst = values[i];
      if (op == static_cast<int>(MaterialXGraphOpCPU::Constant)) dst = fallback(0);
      else if (op == static_cast<int>(MaterialXGraphOpCPU::GeomProp)) {
        dst = fallback(0);  // MaterialX default when the mesh has no stream.
        uint32_t wantedHash = 0;
        float encoded = scene.matGraph[p + 17];
        std::memcpy(&wantedHash, &encoded, sizeof(wantedHash));
        const int components = std::clamp(
            static_cast<int>(std::floor(scene.matGraph[p + 18] + 0.5f)), 1, 16);
        const int matrixColumn = std::clamp(
            static_cast<int>(std::floor(scene.matGraph[p + 19] + 0.5f)), 0,
            components == 9 ? 2 : 3);
        const int outputComponents = components == 9 ? 3 :
                                     components == 16 ? 4 : components;
        if (geomPropTri >= 0 &&
            static_cast<size_t>(geomPropTri) < scene.triCount) {
          for (const HostGeomProp& prop : scene.geomProps) {
            if (prop.components != static_cast<uint32_t>(components) ||
                MaterialXGeomPropHash(prop.name) != wantedHash) continue;
            const size_t begin = static_cast<size_t>(geomPropTri) * 3u *
                                 static_cast<size_t>(components);
            if (begin + static_cast<size_t>(components) * 3u > prop.values.size())
              break;
            const float w = 1.0f - u - v;
            const size_t columnBegin = static_cast<size_t>(matrixColumn) *
                                       static_cast<size_t>(outputComponents);
            for (int lane = 0; lane < outputComponents; ++lane)
              dst[lane] = w * prop.values[begin + columnBegin + lane] +
                          u * prop.values[begin + components + columnBegin + lane] +
                          v * prop.values[begin + 2 * components + columnBegin + lane];
            dst[3] = components == 4 ?
                         w * prop.values[begin + 3] +
                         u * prop.values[begin + components + 3] +
                         v * prop.values[begin + 2 * components + 3] : 1.0f;
            break;
          }
        }
      }
      else if (op == static_cast<int>(MaterialXGraphOpCPU::Image) ||
               op == static_cast<int>(MaterialXGraphOpCPU::TiledImage)) {
        float rgb[3];
        float gu = u, gv = v;
        const int uvInput = static_cast<int>(std::floor(
            scene.matGraph[p + 15] + 0.5f));
        if (uvInput == 0) { gu = a[0]; gv = a[1]; }
        else if (uvInput == 1) { gu = b[0]; gv = b[1]; }
        else if (uvInput == 2) { gu = c[0]; gv = c[1]; }
        const float su = gu * scene.matGraph[p + 17] + scene.matGraph[p + 19];
        const float sv = gv * scene.matGraph[p + 18] + scene.matGraph[p + 20];
        SampleTextureBilinear(scene.textures, scene.texels, textureId, su, sv,
                              rgb);
        dst = {rgb[0], rgb[1], rgb[2], 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::NormalMap)) {
        float nx = a[0] * 2.0f - 1.0f, ny = a[1] * 2.0f - 1.0f,
              nz = a[2] * 2.0f - 1.0f;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        dst = length > 1.0e-6f ? std::array<float, 4>{nx / length * 0.5f + 0.5f,
                                                       ny / length * 0.5f + 0.5f,
                                                       nz / length * 0.5f + 0.5f,
                                                       a[3]} : a;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Add)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] + b[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Subtract)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] - b[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Multiply)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] * b[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Divide)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = a[cidx] / std::max(std::fabs(b[cidx]), 1.0e-6f);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Mix)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] + (b[cidx] - a[cidx]) * c[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Clamp)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::clamp(a[cidx], b[cidx], c[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Dot)) {
        const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        dst = {d, d, d, d};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Normalized)) {
        const float length = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        dst = length > 1.0e-6f ? std::array<float, 4>{a[0] / length, a[1] / length,
                                                       a[2] / length, a[3]} : a;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Power)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = std::pow(std::max(a[cidx], 0.0f), b[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Minimum)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::min(a[cidx], b[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Maximum)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::max(a[cidx], b[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Absolute)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::fabs(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::SquareRoot)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::sqrt(std::max(a[cidx], 0.0f));
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Sine)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::sin(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Cosine)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::cos(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Luminance)) {
        const float l = a[0] * 0.2126f + a[1] * 0.7152f + a[2] * 0.0722f;
        dst = {l, l, l, a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Select)) {
        dst = (a[0] >= 0.5f) ? b : c;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Texcoord)) {
        const bool secondSet = c[2] > 0.5f;
        dst = {secondSet ? u1 : u, secondSet ? v1 : v, 0.0f, 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Floor)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::floor(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Ceil)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::ceil(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Fract)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] - std::floor(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Step)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = a[cidx] < b[cidx] ? 0.0f : 1.0f;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Smoothstep)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          const float t = std::clamp((a[cidx] - b[cidx]) /
                                         std::max(c[cidx] - b[cidx], 1.0e-6f),
                                     0.0f, 1.0f);
          dst[cidx] = t * t * (3.0f - 2.0f * t);
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Cross)) {
        dst = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
               a[0] * b[1] - a[1] * b[0], a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Length)) {
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        dst = {l, l, l, l};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Noise2D)) {
        const int channels=std::clamp(static_cast<int>(std::floor(c[3]+.5f)),1,4);
        dst={0,0,0,0};
        if(channels==1)dst[0]=MtlxPerlin2(a[0],a[1],-1)*b[0]+c[0];
        else if(channels==2){dst[0]=MtlxPerlin2(a[0],a[1],-1)*b[0]+c[0];dst[1]=MtlxPerlin2(a[0]+19,a[1]+193,-1)*b[1]+c[1];}
        else{for(int lane=0;lane<3;lane++)dst[lane]=MtlxPerlin2(a[0],a[1],lane)*b[lane]+c[lane];if(channels==4)dst[3]=MtlxPerlin2(a[0]+19,a[1]+193,-1)*b[3]+c[3];}
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Noise3D)) {
        const int channels=std::clamp(static_cast<int>(std::floor(c[3]+.5f)),1,4);
        dst={0,0,0,0};
        if(channels==1)dst[0]=MtlxPerlin3(a[0],a[1],a[2],-1)*b[0]+c[0];
        else if(channels==2){dst[0]=MtlxPerlin3(a[0],a[1],a[2],-1)*b[0]+c[0];dst[1]=MtlxPerlin3(a[0]+19,a[1]+193,a[2]+17,-1)*b[1]+c[1];}
        else{for(int lane=0;lane<3;lane++)dst[lane]=MtlxPerlin3(a[0],a[1],a[2],lane)*b[lane]+c[lane];if(channels==4)dst[3]=MtlxPerlin3(a[0]+19,a[1]+193,a[2]+17,-1)*b[3]+c[3];}
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Tangent)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::tan(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Exponential)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::exp(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Logarithm)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::log(std::max(a[cidx], 1.0e-6f));
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Modulo)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::fmod(a[cidx], std::max(std::fabs(b[cidx]), 1.0e-6f));
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Atan2)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::atan2(a[cidx], b[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Sign)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = a[cidx] > 0.0f ? 1.0f : (a[cidx] < 0.0f ? -1.0f : 0.0f);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Round)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = std::round(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Invert)) {
        for (int cidx = 0; cidx < 4; ++cidx) dst[cidx] = 1.0f - a[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Remap)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          const float t = (a[cidx] - b[cidx]) / std::max(c[cidx] - b[cidx], 1.0e-6f);
          dst[cidx] = t;
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::GeometricNormal)) {
        dst = normal ? std::array<float, 4>{normal[0], normal[1], normal[2], 1.0f}
                     : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::GeometricTangent)) {
        dst = tangent ? std::array<float, 4>{tangent[0], tangent[1], tangent[2], 1.0f}
                      : std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Rotate3D)) {
        const float al = a[0] * 0.0174532925199433f;
        const float alen = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (alen > 1.0e-6f) {
          const float ax = b[0] / alen, ay = b[1] / alen, az = b[2] / alen;
          const float cs = std::cos(al), sn = std::sin(al);
          const float d = ax * c[0] + ay * c[1] + az * c[2];
          dst = {c[0] * cs + (ay * c[2] - az * c[1]) * sn + ax * d * (1.0f - cs),
                 c[1] * cs + (az * c[0] - ax * c[2]) * sn + ay * d * (1.0f - cs),
                 c[2] * cs + (ax * c[1] - ay * c[0]) * sn + az * d * (1.0f - cs), c[3]};
        } else dst = c;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Transform2D)) {
        float tx = a[0] * scene.matGraph[p + 17] + scene.matGraph[p + 19];
        float ty = a[1] * scene.matGraph[p + 18] + scene.matGraph[p + 20];
        const float angle = scene.matGraph[p + 15] * 0.0174532925199433f;
        const float cs = std::cos(angle), sn = std::sin(angle);
        dst = {tx * cs - ty * sn, tx * sn + ty * cs, a[2], a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Combine)) {
        dst = {a[0], b[0], c[0], 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Extract)) {
        const int lane = std::clamp(static_cast<int>(std::floor(b[0] + 0.5f)), 0, 3);
        dst = {a[lane], a[lane], a[lane], a[lane]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Convert)) {
        dst = a;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Position)) {
        dst = position ? std::array<float, 4>{position[0], position[1], position[2], 1.0f}
                       : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::ViewDirection)) {
        if (viewDirection) {
          const float len=std::sqrt(viewDirection[0]*viewDirection[0]+
                                    viewDirection[1]*viewDirection[1]+
                                    viewDirection[2]*viewDirection[2]);
          dst=len>1.0e-8f?std::array<float,4>{viewDirection[0]/len,
              viewDirection[1]/len,viewDirection[2]/len,1.0f}:
              std::array<float,4>{0,0,1,1};
        } else dst={0,0,1,1};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Time)) {
        dst={time,time,time,time};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Frame)) {
        dst={frame,frame,frame,frame};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Blackbody)) {
        dst=MtlxBlackbody(a[0]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::RoughnessAnisotropy)) {
        const float r=std::clamp(a[0]*a[0],1.0e-8f,1.0f);
        if(b[0]>0.0f){const float aspect=std::sqrt(1.0f-std::clamp(b[0],0.0f,0.98f));dst={std::min(r/aspect,1.0f),r*aspect,0,1};}
        else dst={r,r,0,1};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::RoughnessDual)) {
        const float y=a[1]<0.0f?a[0]:a[1];dst={std::clamp(a[0]*a[0],1.0e-8f,1.0f),std::clamp(y*y,1.0e-8f,1.0f),0,1};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::ArtisticIor)) {
        for(int lane=0;lane<3;++lane){const float r=std::clamp(a[lane],0.0f,.99f),rs=std::sqrt(r),nmin=(1-r)/(1+r),nmax=(1+rs)/(1-rs),ior=nmax+(nmin-nmax)*b[lane];const float k2=std::max((((ior+1)*(ior+1)*r)-((ior-1)*(ior-1)))/(1-r),0.0f);dst[lane]=scene.matGraph[p+17]>=.5f?std::sqrt(k2):ior;}dst[3]=1;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::ChiangHairAbsorption)) {
        const float beta=b[0],b2=beta*beta,b3=b2*beta,b4=b3*beta,b5=b4*beta,den=5.969f-.215f*beta+2.532f*b2-10.73f*b3+5.574f*b4+.245f*b5;
        for(int lane=0;lane<3;++lane){const float q=std::log(std::max(a[lane],1.0e-6f))/std::max(std::fabs(den),1.0e-6f);dst[lane]=q*q;}dst[3]=1;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::HsvAdjust)) {
        const float mx = std::max(a[0], std::max(a[1], a[2]));
        const float mn = std::min(a[0], std::min(a[1], a[2]));
        const float chroma = mx - mn;
        float hue = 0.0f;
        if (chroma > 1.0e-6f) {
          if (mx == a[0]) hue = std::fmod((a[1] - a[2]) / chroma, 6.0f);
          else if (mx == a[1]) hue = (a[2] - a[0]) / chroma + 2.0f;
          else hue = (a[0] - a[1]) / chroma + 4.0f;
          hue /= 6.0f;
        }
        hue = hue + b[0] - std::floor(hue + b[0]);
        const float sat = mx > 1.0e-6f ? chroma / mx : 0.0f;
        const float s2 = std::clamp(sat * b[1], 0.0f, 1.0f);
        const float v2 = std::max(mx * b[2], 0.0f);
        const float hp = hue * 6.0f, x = v2 * s2 *
            (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
        const float cc = v2 * s2, m = v2 - cc;
        const int sector = static_cast<int>(std::floor(hp)) % 6;
        const float rgb[6][3] = {{cc,x,0},{x,cc,0},{0,cc,x},
                                 {0,x,cc},{x,0,cc},{cc,0,x}};
        dst = {rgb[sector][0] + m, rgb[sector][1] + m,
               rgb[sector][2] + m, a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::HeightToNormal)) {
        // Ray tracing has no fragment derivatives.  For the common
        // heighttonormal(image) form, evaluate centered texel differences in
        // the source image; procedural inputs retain the defined flat fallback.
        dst = {0.5f, 0.5f, 1.0f, 1.0f};
        int heightSource=ia;
        for(int hop=0;hop<4&&heightSource>=0&&heightSource<count;++hop){
          const size_t q=base+kRtMaterialGraphHeaderFloats+static_cast<size_t>(heightSource)*kRtMaterialGraphNodeFloats;
          const int qop=static_cast<int>(scene.matGraph[q]+0.5f);
          if(qop==static_cast<int>(MaterialXGraphOpCPU::Extract)||qop==static_cast<int>(MaterialXGraphOpCPU::Convert))heightSource=static_cast<int>(std::floor(scene.matGraph[q+1]+0.5f));else break;
        }
        if (heightSource >= 0 && heightSource < count) {
          const size_t source = base + kRtMaterialGraphHeaderFloats +
              static_cast<size_t>(heightSource) * kRtMaterialGraphNodeFloats;
          const int sourceOp=static_cast<int>(scene.matGraph[source]+0.5f);
          const int sourceTex=static_cast<int>(std::floor(scene.matGraph[source+16]+0.5f));
          if ((sourceOp==static_cast<int>(MaterialXGraphOpCPU::Image)||
               sourceOp==static_cast<int>(MaterialXGraphOpCPU::TiledImage))&&
              sourceTex>=0&&static_cast<size_t>(sourceTex)<scene.textures.size()) {
            float gu=ic>=0?c[0]:u,gv=ic>=0?c[1]:v;
            gu=gu*scene.matGraph[source+17]+scene.matGraph[source+19];
            gv=gv*scene.matGraph[source+18]+scene.matGraph[source+20];
            const auto& td=scene.textures[static_cast<size_t>(sourceTex)];
            const float du=1.0f/std::max(td.width,1),dv=1.0f/std::max(td.height,1);
            float xm[3],xp[3],ym[3],yp[3];
            SampleTextureBilinear(scene.textures,scene.texels,sourceTex,gu-du,gv,xm);
            SampleTextureBilinear(scene.textures,scene.texels,sourceTex,gu+du,gv,xp);
            SampleTextureBilinear(scene.textures,scene.texels,sourceTex,gu,gv-dv,ym);
            SampleTextureBilinear(scene.textures,scene.texels,sourceTex,gu,gv+dv,yp);
            const float scale=b[0]*(1.0f/16.0f),dx=(xp[0]-xm[0])*0.5f*scale,
                        dy=(yp[0]-ym[0])*0.5f*scale;
            const float len=std::sqrt(dx*dx+dy*dy+1.0f);
            dst={-dx/len*0.5f+0.5f,-dy/len*0.5f+0.5f,
                 1.0f/len*0.5f+0.5f,1.0f};
          }
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Arcsine)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = std::asin(std::clamp(a[cidx], -1.0f, 1.0f));
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Arccosine)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = std::acos(std::clamp(a[cidx], -1.0f, 1.0f));
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Arctangent)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = std::atan(a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Contrast)) {
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = (a[cidx] - c[cidx]) * b[cidx] + c[cidx];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Screen)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          const float composite =
              1.0f - (1.0f - a[cidx]) * (1.0f - b[cidx]);
          dst[cidx] = b[cidx] + c[cidx] * (composite - b[cidx]);
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Overlay)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          const float composite = b[cidx] >= 0.5f
              ? 1.0f - 2.0f * (1.0f - a[cidx]) * (1.0f - b[cidx])
              : 2.0f * a[cidx] * b[cidx];
          dst[cidx] = b[cidx] + c[cidx] * (composite - b[cidx]);
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Burn)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          dst[cidx] = std::fabs(a[cidx]) < 1.0e-6f
                          ? 0.0f
                          : c[cidx] * (1.0f - (1.0f - b[cidx]) / a[cidx]) +
                                (1.0f - c[cidx]) * b[cidx];
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Dodge)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          dst[cidx] = std::fabs(1.0f - a[cidx]) < 1.0e-6f
                          ? 0.0f
                          : c[cidx] * (b[cidx] / (1.0f - a[cidx])) +
                                (1.0f - c[cidx]) * b[cidx];
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::RampLR) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::RampTB)) {
        const float coord = op == static_cast<int>(MaterialXGraphOpCPU::RampLR)
                                ? (ic >= 0 ? c[0] : u)
                                : (ic >= 0 ? c[1] : v);
        const float t = std::clamp(coord, 0.0f, 1.0f);
        for (int cidx = 0; cidx < 4; ++cidx)
          dst[cidx] = a[cidx] + t * (b[cidx] - a[cidx]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::SplitLR) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::SplitTB)) {
        const auto coordValue = textureId >= 0 && textureId < count
                                    ? values[textureId]
                                    : std::array<float, 4>{u, v, 0.0f, 1.0f};
        const float coord = op == static_cast<int>(MaterialXGraphOpCPU::SplitLR)
                                ? coordValue[0] : coordValue[1];
        dst = coord >= c[0] ? b : a;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Saturate)) {
        const float luminance =
            0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
        for (int cidx = 0; cidx < 3; ++cidx)
          dst[cidx] = luminance + b[cidx] * (a[cidx] - luminance);
        dst[3] = a[3];
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::IfGreater) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::IfGreaterEqual) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::IfEqual)) {
        const auto other = textureId >= 0 && textureId < count
                               ? values[textureId]
                               : std::array<float, 4>{scene.matGraph[p + 17],
                                      scene.matGraph[p + 18], scene.matGraph[p + 19],
                                      scene.matGraph[p + 20]};
        const bool chooseFirst =
            op == static_cast<int>(MaterialXGraphOpCPU::IfGreater)
                ? a[0] > b[0]
                : (op == static_cast<int>(MaterialXGraphOpCPU::IfGreaterEqual)
                       ? a[0] >= b[0] : a[0] == b[0]);
        dst = chooseFirst ? c : other;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::RgbToHsv)) {
        const float mx = std::max(a[0], std::max(a[1], a[2]));
        const float mn = std::min(a[0], std::min(a[1], a[2]));
        const float chroma = mx - mn;
        float hue = 0.0f;
        if (chroma > 1.0e-6f) {
          if (mx == a[0]) hue = std::fmod((a[1] - a[2]) / chroma, 6.0f);
          else if (mx == a[1]) hue = (a[2] - a[0]) / chroma + 2.0f;
          else hue = (a[0] - a[1]) / chroma + 4.0f;
          hue = hue / 6.0f;
          if (hue < 0.0f) hue += 1.0f;
        }
        dst = {hue, mx > 1.0e-6f ? chroma / mx : 0.0f, mx, a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::HsvToRgb)) {
        const float h = (a[0] - std::floor(a[0])) * 6.0f;
        const float chroma = a[2] * a[1];
        const float x = chroma * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
        const int sector = static_cast<int>(std::floor(h)) % 6;
        const float rgb[6][3] = {{chroma,x,0},{x,chroma,0},{0,chroma,x},
                                 {0,x,chroma},{x,0,chroma},{chroma,0,x}};
        const float m = a[2] - chroma;
        dst = {rgb[sector][0] + m, rgb[sector][1] + m,
               rgb[sector][2] + m, a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Rotate2D)) {
        const float angle = b[0] * 0.0174532925199433f;
        const float cs = std::cos(angle), sn = std::sin(angle);
        dst = {a[0] * cs - a[1] * sn, a[0] * sn + a[1] * cs, a[2], a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Distance)) {
        const float x=a[0]-b[0], y=a[1]-b[1], z=a[2]-b[2];
        const float d=std::sqrt(x*x+y*y+z*z); dst={d,d,d,d};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Reflect)) {
        const float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
        dst={a[0]-2*d*b[0],a[1]-2*d*b[1],a[2]-2*d*b[2],a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Refract)) {
        const float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2], eta=c[0];
        const float k=1-eta*eta*(1-d*d);
        dst=k<0?std::array<float,4>{0,0,0,a[3]}:
            std::array<float,4>{eta*a[0]-(eta*d+std::sqrt(k))*b[0],eta*a[1]-(eta*d+std::sqrt(k))*b[1],eta*a[2]-(eta*d+std::sqrt(k))*b[2],a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Premult)) {
        dst={a[0]*a[3],a[1]*a[3],a[2]*a[3],a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Unpremult)) {
        const float q=std::fabs(a[3])>1e-6f?1.0f/a[3]:0.0f;
        dst={a[0]*q,a[1]*q,a[2]*q,a[3]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::MinComponent) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::MaxComponent)) {
        const float q=op==static_cast<int>(MaterialXGraphOpCPU::MinComponent)?
            std::min(a[0],std::min(a[1],a[2])):std::max(a[0],std::max(a[1],a[2]));
        dst={q,q,q,q};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::LogicalAnd) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::LogicalOr) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::LogicalXor)) {
        const bool av=a[0]!=0, bv=b[0]!=0;
        const bool q=op==static_cast<int>(MaterialXGraphOpCPU::LogicalAnd)?av&&bv:
            (op==static_cast<int>(MaterialXGraphOpCPU::LogicalOr)?av||bv:av!=bv);
        dst={q?1.0f:0.0f,q?1.0f:0.0f,q?1.0f:0.0f,q?1.0f:0.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::LogicalNot)) {
        const float q=a[0]==0?1.0f:0.0f; dst={q,q,q,q};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Inside) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Outside)) {
        const float m=op==static_cast<int>(MaterialXGraphOpCPU::Inside)?b[0]:1-b[0];
        for(int lane=0;lane<4;++lane) dst[lane]=a[lane]*m;
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::GeomColor)) {
        dst={1,1,1,1};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Bitangent)) {
        dst = bitangent
                  ? std::array<float, 4>{bitangent[0], bitangent[1],
                                         bitangent[2], 1.0f}
                  : std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::SetAlpha)) {
        dst = {a[0], a[1], a[2], b[0]};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::CellNoise2D) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::CellNoise3D)) {
        const bool is3d = op == static_cast<int>(MaterialXGraphOpCPU::CellNoise3D);
        const float q = MtlxCellNoise(static_cast<int>(std::floor(a[0])),
            static_cast<int>(std::floor(a[1])),
            static_cast<int>(std::floor(a[2])), is3d);
        dst = {q, q, q, q};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Fractal2D)) {
        const auto diminishValue = textureId >= 0 && textureId < count
            ? values[textureId][0] : scene.matGraph[p + 17];
        const int channels = std::clamp(
            static_cast<int>(std::floor(scene.matGraph[p + 20] + 0.5f)), 1, 4);
        const int octaves = std::clamp(static_cast<int>(std::floor(b[0]+0.5f)),0,16);
        float px=a[0], py=a[1], weight=1.0f;
        dst={0,0,0,0};
        for(int octave=0;octave<octaves;++octave){
          if(channels==1) dst[0]+=weight*MtlxPerlin2(px,py,-1);
          else if(channels==2){dst[0]+=weight*MtlxPerlin2(px,py,-1);dst[1]+=weight*MtlxPerlin2(px+19,py+193,-1);}
          else {for(int lane=0;lane<3;++lane)dst[lane]+=weight*MtlxPerlin2(px,py,lane);if(channels==4)dst[3]+=weight*MtlxPerlin2(px+19,py+193,-1);}
          px*=c[0];py*=c[0];weight*=diminishValue;
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::WorleyNoise2D) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::WorleyNoise3D)) {
        const int channels=std::clamp(static_cast<int>(std::floor(c[1]+0.5f)),1,3);
        dst=MtlxWorley(a,op==static_cast<int>(MaterialXGraphOpCPU::WorleyNoise3D),
                       b[0],static_cast<int>(std::floor(c[0]+0.5f)),channels);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Fractal3D)) {
        const float diminishValue=textureId>=0&&textureId<count?values[textureId][0]:scene.matGraph[p+17];
        const int channels=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+20]+.5f)),1,4);
        const int octaves=std::clamp(static_cast<int>(std::floor(b[0]+.5f)),0,16);
        float px=a[0],py=a[1],pz=a[2],weight=1;dst={0,0,0,0};
        for(int octave=0;octave<octaves;octave++){
          if(channels==1)dst[0]+=weight*MtlxPerlin3(px,py,pz,-1);
          else if(channels==2){dst[0]+=weight*MtlxPerlin3(px,py,pz,-1);dst[1]+=weight*MtlxPerlin3(px+19,py+193,pz+17,-1);}
          else{for(int lane=0;lane<3;lane++)dst[lane]+=weight*MtlxPerlin3(px,py,pz,lane);if(channels==4)dst[3]+=weight*MtlxPerlin3(px+19,py+193,pz+17,-1);}
          px*=c[0];py*=c[0];pz*=c[0];weight*=diminishValue;
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Cloverleaf)) {
        const float sx=2*a[0],sy=2*a[1],cx=2*b[0],cy=2*b[1],r=c[0],r2=r*r;
        const float dx[4]={sx+r-cx,sx-r-cx,sx-cx,sx-cx};
        const float dy[4]={sy-cy,sy-cy,sy-r-cy,sy+r-cy};
        float inside=0;for(int k=0;k<4;k++)if(dx[k]*dx[k]+dy[k]*dy[k]<=r2)inside=1;
        dst={inside,inside,inside,inside};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Hexagon)) {
        float px=std::fabs(a[1]-b[1]),py=std::fabs(a[0]-b[0]);
        const float kx=-.866025f,ky=.5f,kz=.57735f,r=c[0];
        const float projection=std::min(kx*px+ky*py,0.0f);
        px-=2*projection*kx;py-=2*projection*ky;
        px-=std::clamp(px,-kz*r,kz*r);py-=r;
        const float signedDistance=std::sqrt(px*px+py*py)*(py<0?-1.0f:1.0f);
        const float inside=signedDistance<=0?1.0f:0.0f;dst={inside,inside,inside,inside};
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Grid) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Crosshatch) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::TiledCircles) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::TiledCloverleafs) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::TiledHexagons)) {
        const float parameter=textureId>=0&&textureId<count?values[textureId][0]:scene.matGraph[p+17];
        const bool staggered=scene.matGraph[p+18]!=0;const int kind=op-static_cast<int>(MaterialXGraphOpCPU::Grid);
        const float q=MtlxPattern(a[0]*b[0]-c[0],a[1]*b[1]-c[1],parameter,staggered,kind);dst={q,q,q,q};
      } else if(op==static_cast<int>(MaterialXGraphOpCPU::RampCoordinate)){
        const int type=static_cast<int>(std::floor(b[0]+.5f));const float x=a[0]-.5f,y=a[1]-.5f;
        const float q=type==1?std::atan2(y,x)/6.28319f+.5f:(type==2?std::sqrt(x*x+y*y)*1.414f:(type==3?2*std::max(std::fabs(x),std::fabs(y)):a[0]));dst={q,q,q,q};
      } else if(op==static_cast<int>(MaterialXGraphOpCPU::Ramp)||
                op==static_cast<int>(MaterialXGraphOpCPU::RampGradient)){
        const int start=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+17]+.5f)),0,count-1);
        const int interpolation=static_cast<int>(std::floor(b[0]+.5f)),num=std::clamp(static_cast<int>(std::floor(c[0]+.5f)),2,10);const float x=a[0];
        auto blend=[&](float lo,float hi,const std::array<float,4>& c0,const std::array<float,4>& c1){float t=std::fabs(hi-lo)>1e-8f?std::clamp((std::clamp(x,lo,hi)-lo)/(hi-lo),0.0f,1.0f):0;if(interpolation==1)t=t*t*(3-2*t);if(interpolation==2)t=x>=hi?1.0f:0.0f;std::array<float,4> q;for(int lane=0;lane<4;lane++)q[lane]=c0[lane]+t*(c1[lane]-c0[lane]);return q;};
        if (op == static_cast<int>(MaterialXGraphOpCPU::Ramp)) {
          dst = values[std::min(start + 1, count - 1)];
          for (int interval = 1; interval <= 9; interval++) {
            if (interval >= num) continue;
            const int intervalBase = start + (interval - 1) * 2;
            const float lo = values[std::min(intervalBase, count - 1)][0];
            const float hi = values[std::min(intervalBase + 2, count - 1)][0];
            const auto q = blend(lo, hi,
                values[std::min(intervalBase + 1, count - 1)],
                values[std::min(intervalBase + 3, count - 1)]);
            if (x > lo) dst = q;
          }
        }
        else{const float lo=values[start][0],hi=values[std::min(start+1,count-1)][0];const int intervalNum=static_cast<int>(values[std::min(start+5,count-1)][0]);dst=values[std::min(start+4,count-1)];if(intervalNum<num&&x>lo)dst=blend(lo,hi,values[std::min(start+2,count-1)],values[std::min(start+3,count-1)]);}
      } else if(op==static_cast<int>(MaterialXGraphOpCPU::Flake)){
        const int start=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+17]+.5f)),0,count-1);
        const int output=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+18]+.5f)),0,3);
        dst=MtlxFlake(values.data(),start,count,output);
      } else if(op>=static_cast<int>(MaterialXGraphOpCPU::MatrixTransform)&&
                op<=static_cast<int>(MaterialXGraphOpCPU::MatrixDeterminant)){
        const int start=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+17]+.5f)),0,count-1);
        const int dim=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+18]+.5f)),3,4);
        const int column=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+19]+.5f)),0,dim-1);
        const int outputComponents=std::clamp(static_cast<int>(std::floor(scene.matGraph[p+20]+.5f)),1,4);
        float matrix[4][4]{};for(int col=0;col<dim;col++)for(int row=0;row<dim;row++)matrix[row][col]=values[std::min(start+col,count-1)][row];
        auto invert=[&](){float aug[4][8]{};for(int row=0;row<dim;row++){for(int col=0;col<dim;col++)aug[row][col]=matrix[row][col];aug[row][dim+row]=1;}for(int col=0;col<dim;col++){int pivot=col;for(int row=col+1;row<dim;row++)if(std::fabs(aug[row][col])>std::fabs(aug[pivot][col]))pivot=row;if(std::fabs(aug[pivot][col])<1e-12f)return false;if(pivot!=col)for(int k=0;k<2*dim;k++)std::swap(aug[col][k],aug[pivot][k]);const float q=aug[col][col];for(int k=0;k<2*dim;k++)aug[col][k]/=q;for(int row=0;row<dim;row++)if(row!=col){const float f=aug[row][col];for(int k=0;k<2*dim;k++)aug[row][k]-=f*aug[col][k];}}for(int row=0;row<dim;row++)for(int col=0;col<dim;col++)matrix[row][col]=aug[row][dim+col];return true;};
        if(op==static_cast<int>(MaterialXGraphOpCPU::MatrixTransform)){dst={0,0,0,0};for(int row=0;row<outputComponents;row++)for(int col=0;col<dim;col++)dst[row]+=matrix[row][col]*(col<outputComponents?a[col]:((col==dim-1&&outputComponents+1==dim)?1.0f:0.0f));}
        else if(op==static_cast<int>(MaterialXGraphOpCPU::MatrixTranspose)){dst={0,0,0,0};for(int row=0;row<dim;row++)dst[row]=matrix[column][row];}
        else if(op==static_cast<int>(MaterialXGraphOpCPU::MatrixInverse)){dst=values[std::min(start+column,count-1)];if(invert())for(int row=0;row<dim;row++)dst[row]=matrix[row][column];}
        else{float determinant=1;for(int col=0;col<dim;col++){int pivot=col;for(int row=col+1;row<dim;row++)if(std::fabs(matrix[row][col])>std::fabs(matrix[pivot][col]))pivot=row;if(std::fabs(matrix[pivot][col])<1e-12f){determinant=0;break;}if(pivot!=col){for(int k=0;k<dim;k++)std::swap(matrix[col][k],matrix[pivot][k]);determinant=-determinant;}const float q=matrix[col][col];determinant*=q;for(int row=col+1;row<dim;row++){const float f=matrix[row][col]/q;for(int k=col+1;k<dim;k++)matrix[row][k]-=f*matrix[col][k];}}dst={determinant,determinant,determinant,determinant};}
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Difference)) {
        for (int lane = 0; lane < 4; ++lane) {
          const float q = std::fabs(a[lane] - b[lane]);
          dst[lane] = b[lane] + c[lane] * (q - b[lane]);
        }
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::In) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Mask) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Out) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Matte) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::Over) ||
                 op == static_cast<int>(MaterialXGraphOpCPU::DisjointOver)) {
        std::array<float, 4> q{};
        if (op == static_cast<int>(MaterialXGraphOpCPU::In)) {
          for (int lane = 0; lane < 4; ++lane) q[lane] = a[lane] * b[3];
        } else if (op == static_cast<int>(MaterialXGraphOpCPU::Mask)) {
          for (int lane = 0; lane < 4; ++lane) q[lane] = b[lane] * a[3];
        } else if (op == static_cast<int>(MaterialXGraphOpCPU::Out)) {
          for (int lane = 0; lane < 4; ++lane) q[lane] = a[lane] * (1.0f - b[3]);
        } else if (op == static_cast<int>(MaterialXGraphOpCPU::DisjointOver)) {
          const float alpha = a[3] + b[3];
          const bool zeroBackground = std::fabs(b[3]) < 1.0e-6f;
          const float scale = alpha <= 1.0f ? 1.0f :
              (zeroBackground ? 0.0f : (1.0f - a[3]) / b[3]);
          for (int lane = 0; lane < 3; ++lane)
            q[lane] = alpha <= 1.0f ? a[lane] + b[lane]
                                    : (zeroBackground ? 0.0f : a[lane] + b[lane] * scale);
          q[3] = std::min(alpha, 1.0f);
        } else {
          const bool matte = op == static_cast<int>(MaterialXGraphOpCPU::Matte);
          for (int lane = 0; lane < 3; ++lane)
            q[lane] = (matte ? a[lane] * a[3] : a[lane]) +
                      b[lane] * (1.0f - a[3]);
          q[3] = a[3] + b[3] * (1.0f - a[3]);
        }
        for (int lane = 0; lane < 4; ++lane)
          dst[lane] = b[lane] + c[lane] * (q[lane] - b[lane]);
      } else if (op == static_cast<int>(MaterialXGraphOpCPU::Swizzle)) {
        for (int cidx = 0; cidx < 4; ++cidx) {
          const int selector = std::clamp(
              static_cast<int>(std::floor(b[cidx] + 0.5f)), 0, 5);
          dst[cidx] = selector < 4 ? a[selector]
                                   : (selector == 5 ? 1.0f : 0.0f);
        }
      } else dst = fallback(0);
  }
  return values[wanted];
}

}  // namespace

bool CpuRayTracer::trace(const float invViewProj[16], const float viewProj[16],
                         const float camPos[3], const float lightDir[3],
                         const float clearColor[3], float exposure, int renderMode,
                         float depthScale, const float /*sceneMin*/[3],
                         const float /*sceneExtent*/[3], int w, int h,
                         std::vector<uint8_t>* rgba, std::string* err,
                         int spp, const RtCameraLens* lens) {
  // Vertical world size of one pixel at unit distance. Projection element 5
  // is cot(fovY/2) for the perspective camera; zero keeps the atlas path.
  const float pixelAngle =
      (viewProj[5] != 0.0f && h > 0)
          ? 2.0f / (std::fabs(viewProj[5]) * static_cast<float>(h))
          : 0.0f;
  const int sampleCount = std::max(1, lens && lens->enabled() ? std::max(spp, 8) : spp);
  if (sampleCount == 1) {
    const bool ok = traceSingle(invViewProj, camPos, lightDir, clearColor,
                                 exposure, renderMode, depthScale, w, h, 0.0f,
                                 0.0f, pixelAngle, lens, rgba, err);
    LogCpuPtexStats(ptex_);
    return ok;
  }
  std::vector<uint32_t> accum(static_cast<size_t>(w) * h * 4, 0);
  std::vector<uint8_t> sample;
  for (int i = 0; i < sampleCount; ++i) {
    const float x = std::fmod((static_cast<float>(i) + 0.5f) /
                                  static_cast<float>(sampleCount) + 0.37f,
                              1.0f) - 0.5f;
    uint32_t bits = static_cast<uint32_t>(i + 1);
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    const float y = std::fmod(static_cast<float>(bits) * 2.3283064365386963e-10f +
                                  0.11f, 1.0f) - 0.5f;
    if (!traceSingle(invViewProj, camPos, lightDir, clearColor, exposure,
                     renderMode, depthScale, w, h, x, y, pixelAngle, lens,
                     &sample, err))
      return false;
    for (size_t p = 0; p < accum.size(); ++p)
      accum[p] += sample[p];
  }
  rgba->resize(accum.size());
  for (size_t p = 0; p < accum.size(); ++p)
    (*rgba)[p] = static_cast<uint8_t>((accum[p] +
                                       static_cast<uint32_t>(sampleCount / 2)) /
                                      static_cast<unsigned>(sampleCount));
  LogCpuPtexStats(ptex_);
  return true;
}

bool CpuRayTracer::traceSingle(const float invViewProj[16],
                               const float camPos[3], const float lightDir[3],
                               const float clearColor[3], float exposure,
                               int renderMode, float depthScale, int w, int h,
                               float sampleX, float sampleY, float pixelAngle,
                               const RtCameraLens* lens,
                               std::vector<uint8_t>* rgba, std::string* err) {
  if (!tlas_ || w < 1 || h < 1) {
    if (err) *err = "no instance TLAS / invalid size";
    return false;
  }
  rgba->assign(static_cast<size_t>(w) * h * 4, 0);

  // Unproject a pixel's NDC coordinate through invViewProj: same formula as
  // Gui::pickMesh's CPU picking ray (gui.cc), so CPU RT's camera exactly
  // matches what the user clicks on. z=0 is the near plane (z01 depth, which
  // App always passes here -- camera_.proj(/*zeroToOneDepth=*/true)).
  auto unproject = [&](float ndcx, float ndcy, float nz, float out[3]) {
    const float* m = invViewProj;  // column-major: m[col*4 + row]
    const float ox = m[0] * ndcx + m[4] * ndcy + m[8] * nz + m[12];
    const float oy = m[1] * ndcx + m[5] * ndcy + m[9] * nz + m[13];
    const float oz = m[2] * ndcx + m[6] * ndcy + m[10] * nz + m[14];
    const float ow = m[3] * ndcx + m[7] * ndcy + m[11] * nz + m[15];
    const float inv = (ow != 0.0f) ? 1.0f / ow : 1.0f;
    out[0] = ox * inv; out[1] = oy * inv; out[2] = oz * inv;
  };

  float centerNear[3], centerFar[3];
  unproject(0.0f, 0.0f, 0.0f, centerNear);
  unproject(0.0f, 0.0f, 1.0f, centerFar);
  float cameraForward[3] = {centerFar[0] - centerNear[0],
                            centerFar[1] - centerNear[1],
                            centerFar[2] - centerNear[2]};
  Normalize3(cameraForward);

  const float lightLen = std::sqrt(lightDir[0] * lightDir[0] +
                                   lightDir[1] * lightDir[1] +
                                   lightDir[2] * lightDir[2]);
  float lightDirN[3] = {0, 1, 0};
  if (lightLen > 1e-8f) {
    lightDirN[0] = lightDir[0] / lightLen;
    lightDirN[1] = lightDir[1] / lightLen;
    lightDirN[2] = lightDir[2] / lightLen;
  }
  const float exposureMul = std::pow(2.0f, exposure);

  const unsigned nThreads = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<int> nextRow{0};
  uint8_t* pixels = rgba->data();
  const char* probe_path = std::getenv("LIGHTUSD_CPU_PTEX_PROBE");
  const bool probe_enabled = probe_path && probe_path[0] &&
                             sampleX == 0.0f && sampleY == 0.0f;
  // Diagnostic A/B for the anisotropic PTex footprint: the probe needs the
  // single-tap addressing contract, and this env switch restores the previous
  // isotropic max-gradient mip + one bilinear tap for comparison.
  const bool force_isotropic_ptex =
      std::getenv("LIGHTUSD_CPU_PTEX_ISOTROPIC") != nullptr;
  const bool use_ptex_footprint = !probe_enabled && !force_isotropic_ptex;
  std::vector<CpuPtexProbe> probes(
      probe_enabled ? static_cast<size_t>(w) * static_cast<size_t>(h)
                    : 0);
  // Ptex face id for a triangle in the expanded CPU stream. The source face id
  // is texture-local, so instance expansion reuses it unchanged.
  auto ptexFaceFor = [&](int t) -> int {
    if (t < 0 || static_cast<size_t>(t) >= cpuSourceTri_.size()) return -1;
    const int src = cpuSourceTri_[static_cast<size_t>(t)];
    if (src < 0 || static_cast<size_t>(src) >= hs_.face.size()) return -1;
    return hs_.face[static_cast<size_t>(src)];
  };

  auto cpuLayerAlpha = [&](int tri, int inst, float bu, float bv) {
    if (tri < 0 || static_cast<size_t>(tri) >= cpuMat_.size()) return 1.0f;
    const int mid = cpuMat_[static_cast<size_t>(tri)];
    if (mid < 0 || static_cast<size_t>(mid) * 6 + 5 >= hs_.matPbr.size())
      return 1.0f;
    float alpha = hs_.matPbr[static_cast<size_t>(mid) * 6 + 5];
    float mode = 0.0f, cutoff = 0.5f;
    if (static_cast<size_t>(mid) * kLightRtOpenPBRFloats + 55 <
        hs_.matLightRt.size()) {
      const float* lrt = &hs_.matLightRt[static_cast<size_t>(mid) *
                                          kLightRtOpenPBRFloats];
      mode = lrt[54];
      cutoff = lrt[55];
      if (lrt[51] > 0.5f) alpha = lrt[39];
    }
    if (static_cast<size_t>(tri) * 12 + 11 < hs_.cols.size()) {
      const float w0 = 1.0f - bu - bv;
      alpha *= hs_.cols[static_cast<size_t>(tri) * 12 + 3] * w0 +
               hs_.cols[static_cast<size_t>(tri) * 12 + 7] * bu +
               hs_.cols[static_cast<size_t>(tri) * 12 + 11] * bv;
    }
    if (inst >= 0 && static_cast<size_t>(inst) < hs_.instances.size())
      alpha *= hs_.instances[static_cast<size_t>(inst)].tint[3];
    int opacityTex = -1;
    if (mid >= 0 && !hs_.matTex.empty() &&
        static_cast<size_t>(mid) * kRtMaterialTexSlots + 5 < hs_.matTex.size() &&
        tri * 6 + 5 < static_cast<int>(cpuUv_.size())) {
      opacityTex = hs_.matTex[static_cast<size_t>(mid) *
                              kRtMaterialTexSlots + 5];
      if (opacityTex >= 0) {
        const float* param = CpuTextureParams(hs_, mid);
        const bool uv_set1 = CpuSlotUsesSecondUvSet(param, 5) &&
                             static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size();
        const float* src_uv = uv_set1
            ? &cpuUv1_[static_cast<size_t>(tri) * 6]
            : &cpuUv_[static_cast<size_t>(tri) * 6];
        float uv[2];
        Interp6(src_uv, bu, bv, uv);
        float slot_uv[2];
        CpuXformSlotUv(param, 5, uv[0], uv[1], slot_uv);
        int channel = 0;
        float scale = 1.0f;
        float bias = 0.0f;
        if (param) {
          channel = static_cast<int>(param[66] + 0.5f);
          scale = param[67];
          bias = param[68];
        }
        alpha *= SampleTextureBilinearChannel(
            hs_.textures, hs_.texels, opacityTex, slot_uv[0], slot_uv[1],
            channel, ptexFaceFor(tri)) * scale + bias;
      }
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float authoredOpacity =
        static_cast<size_t>(mid) * kLightRtOpenPBRFloats + 51 <
                hs_.matLightRt.size() &&
            hs_.matLightRt[static_cast<size_t>(mid) *
                               kLightRtOpenPBRFloats + 51] > 0.5f
            ? hs_.matLightRt[static_cast<size_t>(mid) *
                                 kLightRtOpenPBRFloats + 39]
            : hs_.matPbr[static_cast<size_t>(mid) * 6 + 5];
    // Match Vulkan raster/RT mixed-atlas classification: painted coverage in
    // a Blend atlas is opaque, while an authored fractional material remains
    // true glass. Without this split the CPU reference ghosts the whole prop.
    if (mode > 1.5f && opacityTex >= 0 && authoredOpacity >= 0.999f &&
        alpha >= 0.5f)
      alpha = 1.0f;
    if (mode > 0.5f && mode < 1.5f) alpha = alpha >= cutoff ? 1.0f : 0.0f;
    return alpha;
  };

  auto cpuLayerColor = [&](int tri, int inst, float bu, float bv) {
    float color[3] = {0.6f, 0.6f, 0.6f};
    if (tri < 0 || static_cast<size_t>(tri) >= cpuMat_.size())
      return std::array<float, 3>{color[0], color[1], color[2]};
    const int mid = cpuMat_[static_cast<size_t>(tri)];
    if (mid >= 0 && static_cast<size_t>(mid) * 3 + 2 < hs_.matBase.size()) {
      color[0] = hs_.matBase[static_cast<size_t>(mid) * 3 + 0];
      color[1] = hs_.matBase[static_cast<size_t>(mid) * 3 + 1];
      color[2] = hs_.matBase[static_cast<size_t>(mid) * 3 + 2];
      if (static_cast<size_t>(tri) * 6 + 5 < cpuUv_.size()) {
        float uv[2];
        Interp6(&cpuUv_[static_cast<size_t>(tri) * 6], bu, bv, uv);
        float uv1[2] = {uv[0], uv[1]};
        if (static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size())
          Interp6(&cpuUv1_[static_cast<size_t>(tri) * 6], bu, bv, uv1);
        if (!hs_.matTex.empty() && static_cast<size_t>(mid) * kRtMaterialTexSlots <
                                      hs_.matTex.size()) {
          const int tex = hs_.matTex[static_cast<size_t>(mid) * kRtMaterialTexSlots];
          const float* tp = CpuTextureParams(hs_, mid);
          const bool uv1_used =
              CpuSlotUsesSecondUvSet(tp, 0) &&
              static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size();
          const float* src_uv = uv1_used
              ? &cpuUv1_[static_cast<size_t>(tri) * 6]
              : &cpuUv_[static_cast<size_t>(tri) * 6];
          float lane_uv[2];
          Interp6(src_uv, bu, bv, lane_uv);
          float slot_uv[2];
          CpuXformSlotUv(tp, 0, lane_uv[0], lane_uv[1], slot_uv);
          float sampled[3];
          SampleTextureBilinear(hs_.textures, hs_.texels, tex, slot_uv[0],
                                slot_uv[1], sampled, ptexFaceFor(tri));
          color[0] *= sampled[0]; color[1] *= sampled[1]; color[2] *= sampled[2];
        }
        const size_t graphBase = static_cast<size_t>(mid) * kRtMaterialGraphFloats;
        if (graphBase + kRtMaterialGraphHeaderFloats <= hs_.matGraph.size() &&
            hs_.matGraph[graphBase + 1] >= 0.0f) {
          const auto graph = EvalCpuMaterialXGraph(hs_, mid, 0, uv[0], uv[1],
                                                   uv1[0], uv1[1], nullptr,
                                                   nullptr, nullptr, 0.0f, 0.0f,
                                                   (static_cast<size_t>(tri) < cpuSourceTri_.size())
                                                       ? cpuSourceTri_[static_cast<size_t>(tri)]
                                                       : -1);
          color[0] = graph[0]; color[1] = graph[1]; color[2] = graph[2];
        }
      }
    }
    if (mid >= 0) {
      const size_t lrtBase = static_cast<size_t>(mid) * kLightRtOpenPBRFloats;
      if (lrtBase + 79 < hs_.matLightRt.size()) {
        const float volumeOpacity =
            std::clamp(1.0f - std::exp(-std::max(hs_.matLightRt[lrtBase + 75],
                                                  0.0f) * 0.1f),
                       0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) {
          color[c] *= 1.0f +
                      (hs_.matLightRt[lrtBase + 72 + c] - 1.0f) *
                          volumeOpacity;
          color[c] += hs_.matLightRt[lrtBase + 76 + c] *
                      std::max(hs_.matLightRt[lrtBase + 79], 0.0f) *
                      volumeOpacity;
        }
      }
    }
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float cwb2[9];
    if (static_cast<size_t>(tri) * 9 + 8 < cpuTris_.size()) {
      WorldVerts9(hs_, hs_.instances[static_cast<size_t>(inst)], tri,
                  cwb2);
      const float* tv = cwb2;
      const float e1[3] = {tv[3] - tv[0], tv[4] - tv[1], tv[5] - tv[2]};
      const float e2[3] = {tv[6] - tv[0], tv[7] - tv[1], tv[8] - tv[2]};
      normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
      normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
      normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
      const float nl = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                 normal[2] * normal[2]);
      if (nl > 1.0e-6f) {
        normal[0] /= nl; normal[1] /= nl; normal[2] /= nl;
      }
    }
    const float nl = std::max(0.0f, normal[0] * lightDirN[0] +
                                      normal[1] * lightDirN[1] +
                                      normal[2] * lightDirN[2]);
    // Match the primary RT material response for intermediate layers: retain
    // a compact dielectric/metallic GGX lobe instead of reducing every
    // underlayer to Lambertian color. This keeps the bounded compositor cheap
    // while preserving authored roughness and metalness contrast.
    float metallic = 0.0f;
    float roughness = 0.35f;
    float subsurface = 0.0f;
    float subsurfaceScale = 1.0f;
    float coatWeight = 0.0f;
    float coatRoughness = 0.2f;
    float subsurfaceColor[3] = {color[0], color[1], color[2]};
    float coatColor[3] = {1.0f, 1.0f, 1.0f};
    if (mid >= 0) {
      const size_t lrtBase = static_cast<size_t>(mid) * kLightRtOpenPBRFloats;
      if (lrtBase + 42 < hs_.matLightRt.size()) {
        metallic = std::clamp(hs_.matLightRt[lrtBase + 40], 0.0f, 1.0f);
        roughness = std::clamp(hs_.matLightRt[lrtBase + 42], 0.03f, 1.0f);
        if (lrtBase + 43 < hs_.matLightRt.size()) {
          subsurfaceColor[0] = std::max(0.0f, hs_.matLightRt[lrtBase + 16]);
          subsurfaceColor[1] = std::max(0.0f, hs_.matLightRt[lrtBase + 17]);
          subsurfaceColor[2] = std::max(0.0f, hs_.matLightRt[lrtBase + 18]);
          subsurface = std::clamp(hs_.matLightRt[lrtBase + 19], 0.0f, 1.0f);
          subsurfaceScale = std::max(0.0f, hs_.matLightRt[lrtBase + 23]);
          coatColor[0] = std::max(0.0f, hs_.matLightRt[lrtBase + 24]);
          coatColor[1] = std::max(0.0f, hs_.matLightRt[lrtBase + 25]);
          coatColor[2] = std::max(0.0f, hs_.matLightRt[lrtBase + 26]);
          coatWeight = std::clamp(hs_.matLightRt[lrtBase + 27], 0.0f, 1.0f);
          coatRoughness = std::clamp(hs_.matLightRt[lrtBase + 44], 0.03f, 1.0f);
        }
      }
    }
    float hit[3] = {0.0f, 0.0f, 0.0f};
    float cwb[9];
    if (static_cast<size_t>(tri) * 9 + 8 < cpuTris_.size()) {
      WorldVerts9(hs_, hs_.instances[static_cast<size_t>(inst)], tri,
                  cwb);
      const float* tv = cwb;
      const float w0 = 1.0f - bu - bv;
      for (int c = 0; c < 3; ++c)
        hit[c] = tv[c] * w0 + tv[3 + c] * bu + tv[6 + c] * bv;
    }
    float view[3] = {camPos[0] - hit[0], camPos[1] - hit[1], camPos[2] - hit[2]};
    Normalize3(view);
    float halfVec[3] = {lightDirN[0] + view[0], lightDirN[1] + view[1],
                         lightDirN[2] + view[2]};
    Normalize3(halfVec);
    const float nh = std::max(0.0f, normal[0] * halfVec[0] +
                                      normal[1] * halfVec[1] +
                                      normal[2] * halfVec[2]);
    const float f0 = 0.04f * (1.0f - metallic) + metallic;
    const float spec = std::pow(nh, std::max(1.0f, 2.0f /
                                                 (roughness * roughness) - 2.0f));
    const float lit = 0.15f + 0.85f * nl;
    const float radiusGain = std::clamp(std::sqrt(std::max(subsurfaceScale, 0.0f)) *
                                            2.0f, 0.0f, 1.0f);
    const float diffusion = subsurface * (0.2f + 0.8f * (1.0f - nl) * radiusGain);
    const float coatSpec = coatWeight * 0.04f * nl /
                           std::max(0.08f, coatRoughness * coatRoughness);
    const float scale = lit * (1.0f - metallic) + f0 * spec + coatSpec;
    return std::array<float, 3>{
        color[0] * scale * (1.0f - diffusion) + subsurfaceColor[0] * diffusion + coatColor[0] * coatSpec,
        color[1] * scale * (1.0f - diffusion) + subsurfaceColor[1] * diffusion + coatColor[1] * coatSpec,
        color[2] * scale * (1.0f - diffusion) + subsurfaceColor[2] * diffusion + coatColor[2] * coatSpec};
  };

  auto cpuLayerTransmission = [&](int tri, float travelDistance) {
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
    if (tri < 0 || static_cast<size_t>(tri) >= cpuMat_.size()) return tint;
    const int mid = cpuMat_[static_cast<size_t>(tri)];
    if (mid < 0) return tint;
    const size_t base = static_cast<size_t>(mid) * kLightRtOpenPBRFloats;
    if (base + 11 >= hs_.matLightRt.size()) return tint;
    const float weight = std::clamp(hs_.matLightRt[base + 11], 0.0f, 1.0f);
    const float depth = base + 15 < hs_.matLightRt.size()
                            ? std::max(hs_.matLightRt[base + 15], 0.0f)
                            : 0.0f;
    for (int c = 0; c < 3; ++c) {
      const float color = std::clamp(hs_.matLightRt[base + 8 + c], 0.0f, 1.0f);
      const float scatter = base + 12 + static_cast<size_t>(c) < hs_.matLightRt.size()
                                ? std::max(hs_.matLightRt[base + 12 + c], 0.0f)
                                : 0.0f;
      // OpenPBR transmission_color is the transmittance after `depth` scene
      // units. Apply it only after an exit hit provides the actual distance;
      // depth==0 retains the specification's one-time interface tint.
      const float attenuation = depth > 0.0f
          ? (travelDistance > 0.0f
                 ? std::exp(std::log(std::max(color, 1.0e-6f)) *
                            (travelDistance / depth) -
                            0.25f * scatter * travelDistance / depth)
                 : 1.0f)
          : color;
      tint[c] = 1.0f - weight + weight * attenuation;
    }
    // Match the CUDA/Vulkan OpenPBR transmission dispersion approximation.
    // The compact material lane stores the authored dispersion strength and
    // scale separately; Abbe number increases the effect for low-dispersion
    // media while remaining bounded for malformed input.
    const float dispersion = base + 62 < hs_.matLightRt.size()
                                 ? std::clamp(
                                       hs_.matLightRt[base + 60] *
                                           hs_.matLightRt[base + 62] *
                                           (base + 61 < hs_.matLightRt.size() &&
                                                    hs_.matLightRt[base + 61] > 0.0f
                                                ? 1.0f +
                                                      20.0f / std::max(
                                                          hs_.matLightRt[base + 61],
                                                          1.0f)
                                                : 1.0f),
                                       0.0f, 1.0f)
                                 : 0.0f;
    tint[0] *= 1.0f + 0.35f * dispersion;
    tint[2] *= 1.0f - 0.25f * dispersion;
    return tint;
  };

  auto cpuLayerTransmissionWeight = [&](int tri, int inst, float bu,
                                         float bv) {
    if (tri < 0 || static_cast<size_t>(tri) >= cpuMat_.size()) return 0.0f;
    const int mid = cpuMat_[static_cast<size_t>(tri)];
    if (mid < 0) return 0.0f;
    const size_t base = static_cast<size_t>(mid) * kLightRtOpenPBRFloats;
    if (base + 54 >= hs_.matLightRt.size()) return 0.0f;
    float transmission = std::clamp(hs_.matLightRt[base + 11], 0.0f, 1.0f);
    const float authoredOpacity = hs_.matLightRt[base + 51] > 0.5f
                                      ? hs_.matLightRt[base + 39]
                                      : hs_.matPbr[static_cast<size_t>(mid) * 6 + 5];
    if (hs_.matLightRt[base + 54] > 1.5f &&
        hs_.matLightRt[base + 43] > 1.01f && authoredOpacity < 0.999f)
      transmission =
        std::max(transmission, 1.0f - cpuLayerAlpha(tri, inst, bu, bv));
    return transmission;
  };

  struct CpuRefractResult {
    std::array<float, 3> color{0.0f, 0.0f, 0.0f};
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> dir{0.0f, 0.0f, -1.0f};
    // The compositor needs the first continuation ray independently of the
    // bounded three-interface color approximation.  Reusing the latter's
    // terminal ray skips any interfaces that the underlayer walk should see.
    std::array<float, 3> continuationOrigin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> continuationDir{0.0f, 0.0f, -1.0f};
    bool hasContinuation{false};
  };
  auto cpuLayerRefracted = [&](int tri, int inst, float bu, float bv,
                               const float rayOrigin[3],
                               const float rayDir[3]) {
    CpuRefractResult result;
    std::copy(rayOrigin, rayOrigin + 3, result.origin.begin());
    std::copy(rayDir, rayDir + 3, result.dir.begin());
    std::array<float, 3> throughput{1.0f, 1.0f, 1.0f};
    int currentTri = tri;
    int currentInst = inst;
    float currentBu = bu, currentBv = bv;
    float currentDir[3] = {rayDir[0], rayDir[1], rayDir[2]};
    for (int bounce = 0; bounce < 3; ++bounce) {
      if (currentTri < 0 ||
          static_cast<size_t>(currentTri) * 9 + 8 >= cpuTris_.size())
        return result;
      float rwb[9];
      WorldVerts9(hs_, hs_.instances[static_cast<size_t>(currentInst)],
                  currentTri, rwb);
      const float* tv = rwb;
      const float p0[3] = {tv[0], tv[1], tv[2]};
      const float p1[3] = {tv[3], tv[4], tv[5]};
      const float p2[3] = {tv[6], tv[7], tv[8]};
      float n[3] = {(p1[1] - p0[1]) * (p2[2] - p0[2]) -
                       (p1[2] - p0[2]) * (p2[1] - p0[1]),
                   (p1[2] - p0[2]) * (p2[0] - p0[0]) -
                       (p1[0] - p0[0]) * (p2[2] - p0[2]),
                   (p1[0] - p0[0]) * (p2[1] - p0[1]) -
                       (p1[1] - p0[1]) * (p2[0] - p0[0])};
      const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (nl <= 1.0e-8f) return result;
      for (float& v : n) v /= nl;
      if (static_cast<size_t>(currentTri) * 9 + 8 < cpuNrms_.size()) {
        float smooth[3];
        WorldVec9(hs_.nrms, hs_.instances[static_cast<size_t>(currentInst)],
                  currentTri, currentBu, currentBv, smooth);
        const float smoothLen = std::sqrt(smooth[0] * smooth[0] +
                                          smooth[1] * smooth[1] +
                                          smooth[2] * smooth[2]);
        if (smoothLen > 1.0e-8f) {
          for (float& value : smooth) value /= smoothLen;
          if (smooth[0] * n[0] + smooth[1] * n[1] + smooth[2] * n[2] < 0.0f)
            for (float& value : smooth) value = -value;
          std::copy(smooth, smooth + 3, n);
        }
      }
      const float normalDir = n[0] * currentDir[0] +
                              n[1] * currentDir[1] +
                              n[2] * currentDir[2];
      const bool entering = normalDir < 0.0f;
      if (normalDir > 0.0f) {
        for (float& v : n) v = -v;
      }
      int mid = currentTri < static_cast<int>(cpuMat_.size())
                    ? cpuMat_[static_cast<size_t>(currentTri)]
                    : -1;
      const size_t mb = mid >= 0 ? static_cast<size_t>(mid) *
                                      kLightRtOpenPBRFloats
                                : hs_.matLightRt.size();
      const float ior = mb + 43 < hs_.matLightRt.size()
                            ? std::max(hs_.matLightRt[mb + 43], 1.0f)
                            : 1.0f;
      const bool thinWalled = mb + 71 < hs_.matLightRt.size() &&
                              hs_.matLightRt[mb + 71] >= 0.5f;
      const float eta = thinWalled ? 1.0f : (entering ? 1.0f / ior : ior);
      const float cosi = -(n[0] * currentDir[0] + n[1] * currentDir[1] +
                           n[2] * currentDir[2]);
      const float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
      float refr[3];
      if (thinWalled) {
        for (int c = 0; c < 3; ++c) refr[c] = currentDir[c];
      } else if (k < 0.0f) {
        const float dot = n[0] * currentDir[0] + n[1] * currentDir[1] +
                          n[2] * currentDir[2];
        for (int c = 0; c < 3; ++c) refr[c] = currentDir[c] - 2.0f * dot * n[c];
      } else {
        const float scale = eta * cosi - std::sqrt(k);
        for (int c = 0; c < 3; ++c) refr[c] = eta * currentDir[c] + scale * n[c];
      }
      const float rl = std::sqrt(refr[0] * refr[0] + refr[1] * refr[1] +
                                 refr[2] * refr[2]);
      if (rl <= 1.0e-8f) return result;
      for (float& v : refr) v /= rl;
      float hit[3] = {p0[0] * (1.0f - currentBu - currentBv) +
                          p1[0] * currentBu + p2[0] * currentBv,
                      p0[1] * (1.0f - currentBu - currentBv) +
                          p1[1] * currentBu + p2[1] * currentBv,
                      p0[2] * (1.0f - currentBu - currentBv) +
                          p1[2] * currentBu + p2[2] * currentBv};
      lrt_ray nextRay{};
      for (int c = 0; c < 3; ++c) {
        nextRay.org[c] = hit[c] + refr[c] * 1.0e-4f;
        nextRay.dir[c] = refr[c];
        result.origin[c] = nextRay.org[c];
        result.dir[c] = refr[c];
        if (bounce == 0) {
          result.continuationOrigin[c] = nextRay.org[c];
          result.continuationDir[c] = refr[c];
        }
      }
      if (bounce == 0) result.hasContinuation = true;
      nextRay.tmin = 1.0e-4f;
      nextRay.tmax = 1.0e30f;
      float nt = 0.0f, nu = 0.0f, nv = 0.0f;
      int nInst = -1, nSrc = -1;
      if (!Intersect(nextRay, &nt, &nu, &nv, &nInst, &nSrc)) return result;
      const auto tint = cpuLayerTransmission(currentTri, nt);
      for (int c = 0; c < 3; ++c) throughput[c] *= tint[c];
      currentTri = nSrc;
      currentInst = nInst;
      currentBu = nu;
      currentBv = nv;
      std::copy(refr, refr + 3, currentDir);
      if (cpuLayerTransmissionWeight(currentTri, currentInst, currentBu,
                                     currentBv) <= 1.0e-4f ||
          bounce == 2) {
        const auto color =
            cpuLayerColor(currentTri, currentInst, currentBu, currentBv);
        result.color = {color[0] * throughput[0], color[1] * throughput[1],
                        color[2] * throughput[2]};
        return result;
      }
    }
    return result;
  };

  // Sample authored dome lights for miss rays and glossy reflection. HostScene
  // retains the source lat-long texture and the packed world->environment
  // rotation, so the CPU reference need not fall back to a flat clear color.
  auto cpuEnvironment = [&](const float worldDir[3]) {
    std::array<float, 3> result{0.0f, 0.0f, 0.0f};
    bool found = false;
    for (int li = 0; li < hs_.numLights; ++li) {
      const float* lp = &hs_.lightParams[static_cast<size_t>(li) *
                                          kRtLightParamFloats];
      if (static_cast<int>(lp[0] + 0.5f) !=
              static_cast<int>(DrawLightCPU::Type::Dome) ||
          static_cast<int>(lp[1] + 0.5f) == 0)
        continue;
      float d[3] = {
          lp[40] * worldDir[0] + lp[41] * worldDir[1] + lp[42] * worldDir[2],
          lp[44] * worldDir[0] + lp[45] * worldDir[1] + lp[46] * worldDir[2],
          lp[48] * worldDir[0] + lp[49] * worldDir[1] + lp[50] * worldDir[2]};
      Normalize3(d);
      float radiance[3] = {1.0f, 1.0f, 1.0f};
      const int textureId = static_cast<int>(std::floor(lp[2] + 0.5f));
      if (textureId >= 0) {
        constexpr float kInvTwoPi = 0.15915494309189535f;
        constexpr float kInvPi = 0.31830988618379067f;
        const float u = std::atan2(d[0], -d[2]) * kInvTwoPi + 0.5f;
        const float v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) * kInvPi;
        SampleTextureBilinear(hs_.textures, hs_.texels, textureId, u, v,
                              radiance);
      }
      for (int c = 0; c < 3; ++c)
        result[c] += radiance[c] * std::max(lp[16 + c], 0.0f);
      found = true;
    }
    if (!found)
      result = {clearColor[0], clearColor[1], clearColor[2]};
    return result;
  };

  auto cpuDomeIrradiance = [&](const float worldNormal[3]) {
    std::array<float, 3> result{0.0f, 0.0f, 0.0f};
    for (int li = 0; li < hs_.numLights; ++li) {
      const float* lp = &hs_.lightParams[static_cast<size_t>(li) *
                                          kRtLightParamFloats];
      if (static_cast<int>(lp[0] + 0.5f) !=
              static_cast<int>(DrawLightCPU::Type::Dome) ||
          (static_cast<int>(lp[1] + 0.5f) & (1 << 7)) == 0)
        continue;
      float n[3] = {
          lp[40] * worldNormal[0] + lp[41] * worldNormal[1] + lp[42] * worldNormal[2],
          lp[44] * worldNormal[0] + lp[45] * worldNormal[1] + lp[46] * worldNormal[2],
          lp[48] * worldNormal[0] + lp[49] * worldNormal[1] + lp[50] * worldNormal[2]};
      Normalize3(n);
      const float basis[9] = {
          0.282095f, 0.488603f * n[1], 0.488603f * n[2],
          0.488603f * n[0], 1.092548f * n[0] * n[1],
          1.092548f * n[1] * n[2],
          0.315392f * (3.0f * n[2] * n[2] - 1.0f),
          1.092548f * n[0] * n[2],
          0.546274f * (n[0] * n[0] - n[1] * n[1])};
      for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c)
          result[c] += basis[k] * lp[52 + k * 3 + c] *
                       std::max(lp[16 + c], 0.0f);
    }
    for (float& c : result) c = std::max(c, 0.0f);
    return result;
  };

  auto worker = [&]() {
    int y;
    while ((y = nextRow.fetch_add(1, std::memory_order_relaxed)) < h) {
      const float ndcy = 1.0f - 2.0f *
          ((static_cast<float>(y) + 0.5f + sampleY) / static_cast<float>(h));
      for (int x = 0; x < w; ++x) {
        const float ndcx = 2.0f *
            ((static_cast<float>(x) + 0.5f + sampleX) / static_cast<float>(w)) - 1.0f;
        float nearW[3], farW[3];
        unproject(ndcx, ndcy, 0.0f, nearW);
        unproject(ndcx, ndcy, 1.0f, farW);
        float dir[3] = {farW[0] - nearW[0], farW[1] - nearW[1], farW[2] - nearW[2]};
        const float dlen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dlen > 1e-8f) { dir[0] /= dlen; dir[1] /= dlen; dir[2] /= dlen; }

        float rayOrg[3] = {camPos[0], camPos[1], camPos[2]};
        if (lens && lens->enabled()) {
          const float forwardDot = std::max(
              1.0e-4f, dir[0] * cameraForward[0] +
                             dir[1] * cameraForward[1] +
                             dir[2] * cameraForward[2]);
          const float focusT = lens->focusDistance / forwardDot;
          const float focus[3] = {camPos[0] + dir[0] * focusT,
                                  camPos[1] + dir[1] * focusT,
                                  camPos[2] + dir[2] * focusT};
          float up[3] = {0.0f, 1.0f, 0.0f};
          if (std::fabs(dir[1]) > 0.95f) {
            up[0] = 1.0f;
            up[1] = 0.0f;
          }
          float right[3] = {up[1] * dir[2] - up[2] * dir[1],
                            up[2] * dir[0] - up[0] * dir[2],
                            up[0] * dir[1] - up[1] * dir[0]};
          const float rlen = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                       right[2] * right[2]);
          if (rlen > 1e-8f) {
            for (float& value : right) value /= rlen;
            const float disk = sampleX * sampleX + sampleY * sampleY <= 1.0f
                                   ? lens->apertureRadius
                                   : 0.0f;
            rayOrg[0] += right[0] * sampleX * disk;
            rayOrg[1] += right[1] * sampleX * disk;
            rayOrg[2] += right[2] * sampleX * disk;
            const float trueUp[3] = {dir[1] * right[2] - dir[2] * right[1],
                                     dir[2] * right[0] - dir[0] * right[2],
                                     dir[0] * right[1] - dir[1] * right[0]};
            rayOrg[0] += trueUp[0] * sampleY * disk;
            rayOrg[1] += trueUp[1] * sampleY * disk;
            rayOrg[2] += trueUp[2] * sampleY * disk;
            dir[0] = focus[0] - rayOrg[0];
            dir[1] = focus[1] - rayOrg[1];
            dir[2] = focus[2] - rayOrg[2];
            const float newLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] +
                                           dir[2] * dir[2]);
            if (newLen > 1e-8f)
              for (float& value : dir) value /= newLen;
          }
        }

        lrt_ray ray{};
        ray.org[0] = rayOrg[0]; ray.org[1] = rayOrg[1]; ray.org[2] = rayOrg[2];
        ray.dir[0] = dir[0]; ray.dir[1] = dir[1]; ray.dir[2] = dir[2];
        ray.tmin = 1e-4f;
        ray.tmax = 1e30f;

        uint8_t* px = pixels + (static_cast<size_t>(y) * w + x) * 4;
        lrt_hit hit{};
        int hitInst = -1, hitSrc = -1;
        const bool flatHit = Intersect(ray, &hit.t, &hit.u, &hit.v, &hitInst,
                                       &hitSrc);
        if (flatHit) hit.prim_id = static_cast<uint32_t>(hitSrc);
        if (!flatHit) {
          const auto environment = cpuEnvironment(dir);
          const auto encode = [](float value) {
            value = std::max(value, 0.0f);
            value = std::clamp(
                value * (2.51f * value + 0.03f) /
                    (value * (2.43f * value + 0.59f) + 0.14f),
                0.0f, 1.0f);
            return value <= 0.0031308f
                       ? value * 12.92f
                       : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
          };
          px[0] = static_cast<uint8_t>(encode(environment[0]) * 255.0f);
          px[1] = static_cast<uint8_t>(encode(environment[1]) * 255.0f);
          px[2] = static_cast<uint8_t>(encode(environment[2]) * 255.0f);
          px[3] = 255;
          continue;
        }

        const uint32_t tri = hit.prim_id;
        float nrm[3];
        WorldVec9(hs_.nrms, hs_.instances[static_cast<size_t>(hitInst)],
                  static_cast<int>(tri), hit.u, hit.v, nrm);
        const float nlen = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        if (nlen > 1e-8f) { nrm[0] /= nlen; nrm[1] /= nlen; nrm[2] /= nlen; }

        float outCol[3];
        if (renderMode == 2) {  // Normals
          outCol[0] = nrm[0] * 0.5f + 0.5f;
          outCol[1] = nrm[1] * 0.5f + 0.5f;
          outCol[2] = nrm[2] * 0.5f + 0.5f;
        } else if (renderMode == 3) {  // MaterialId (hashed color)
          const int matId = (tri < cpuMat_.size()) ? cpuMat_[tri] : -1;
          const uint32_t hcol = HashColor(static_cast<uint32_t>(matId + 1));
          outCol[0] = ((hcol >> 16) & 0xFF) / 255.0f;
          outCol[1] = ((hcol >> 8) & 0xFF) / 255.0f;
          outCol[2] = (hcol & 0xFF) / 255.0f;
        } else if (renderMode == 6) {  // Depth
          const float d = std::clamp(hit.t / std::max(1e-3f, depthScale), 0.0f, 1.0f);
          outCol[0] = outCol[1] = outCol[2] = d;
        } else {  // Shaded (default) -- flat Lambertian, one directional light + ambient
          const int matId = (tri < cpuMat_.size()) ? cpuMat_[tri] : -1;
          // Per-hit intrinsic-UV footprint. Triangle UVs are affine, so the
          // world-space UV gradients give |duv/dpixel| at the hit distance; a
          // PTex sampler turns that into a per-face mip level.
          float uvPerPixel = 0.0f;
          float grad_u[3] = {0.0f, 0.0f, 0.0f};
          float grad_v[3] = {0.0f, 0.0f, 0.0f};
          float worldFootprint = 0.0f;
          bool have_grad = false;
          if (matId >= 0 &&
              static_cast<size_t>(tri) * 9 + 8 < cpuTris_.size() &&
              static_cast<size_t>(tri) * 6 + 5 < cpuUv_.size() &&
              pixelAngle > 0.0f) {
            float wv1[9];
            WorldVerts9(hs_, hs_.instances[static_cast<size_t>(hitInst)],
                        static_cast<int>(tri), wv1);
            const float* tv = wv1;
            const float* uv = &cpuUv_[static_cast<size_t>(tri) * 6];
            const float e1[3] = {tv[3] - tv[0], tv[4] - tv[1], tv[5] - tv[2]};
            const float e2[3] = {tv[6] - tv[0], tv[7] - tv[1], tv[8] - tv[2]};
            const float du1 = uv[2] - uv[0], dv1 = uv[3] - uv[1];
            const float du2 = uv[4] - uv[0], dv2 = uv[5] - uv[1];
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) > 1.0e-9f) {
              const float gu[3] = {(dv2 * e1[0] - dv1 * e2[0]) / det,
                                   (dv2 * e1[1] - dv1 * e2[1]) / det,
                                   (dv2 * e1[2] - dv1 * e2[2]) / det};
              const float gv[3] = {(-du2 * e1[0] + du1 * e2[0]) / det,
                                   (-du2 * e1[1] + du1 * e2[1]) / det,
                                   (-du2 * e1[2] + du1 * e2[2]) / det};
              const float lu = std::sqrt(gu[0] * gu[0] + gu[1] * gu[1] +
                                         gu[2] * gu[2]);
              const float lv = std::sqrt(gv[0] * gv[0] + gv[1] * gv[1] +
                                         gv[2] * gv[2]);
              worldFootprint = pixelAngle * std::max(hit.t, 1.0e-4f);
              uvPerPixel = worldFootprint * std::max(lu, lv);
              for (int k = 0; k < 3; ++k) {
                grad_u[k] = gu[k];
                grad_v[k] = gv[k];
              }
              have_grad = true;
            }
          }
          auto ptexAt = [&](int tex) -> const CpuPtexSampler* {
            return (tex >= 0 && static_cast<size_t>(tex) < ptex_.size())
                       ? ptex_[static_cast<size_t>(tex)].get()
                       : nullptr;
          };
          auto ptexMipOf = [&](const CpuPtexSampler* sampler,
                               int face) -> uint32_t {
            return (sampler && face >= 0)
                       ? sampler->ChooseMip(static_cast<uint32_t>(face),
                                            uvPerPixel)
                       : 0u;
          };
          auto ptexFpOf = [&](const CpuPtexSampler* sampler,
                              int face) -> CpuPtexSampler::Footprint {
            CpuPtexSampler::Footprint fp;
            if (sampler && face >= 0 && have_grad)
              fp = sampler->ComputeFootprint(static_cast<uint32_t>(face),
                                             grad_u, grad_v, worldFootprint);
            return fp;
          };
          // One hit frame serves the graph's tangent/bitangent nodes and the
          // material normal map. Only pay for it when either could be used.
          ShadingTangentFrame shading_frame;
          bool have_shading_frame = false;
          bool authored_tangent_frame = false;
          if (matId >= 0 &&
              static_cast<size_t>(tri) * 6 + 5 < cpuUv_.size()) {
            const size_t graph_base =
                static_cast<size_t>(matId) * kRtMaterialGraphFloats;
            const bool has_graph =
                graph_base + kRtMaterialGraphHeaderFloats <= hs_.matGraph.size();
            const bool has_normal_tex =
                static_cast<size_t>(matId) * kRtMaterialTexSlots + 3 <
                    hs_.matTex.size() &&
                hs_.matTex[static_cast<size_t>(matId) * kRtMaterialTexSlots + 3] >= 0;
            if (has_graph || has_normal_tex) {
              // Authored primvars:tangents take precedence when present; the
              // triangle-derived frame remains the fallback (and the shader's
              // behavior for meshes without authored tangents).
              if (static_cast<size_t>(tri) * 9 + 8 < cpuTan_.size() &&
                  static_cast<size_t>(tri) * 9 + 8 < cpuBin_.size()) {
                float at[3], ab[3];
                WorldVec9(hs_.tan, hs_.instances[static_cast<size_t>(hitInst)],
                          static_cast<int>(tri), hit.u, hit.v, at);
                WorldVec9(hs_.bin, hs_.instances[static_cast<size_t>(hitInst)],
                          static_cast<int>(tri), hit.u, hit.v, ab);
                authored_tangent_frame =
                    BuildAuthoredShadingFrame(at, ab, nrm, &shading_frame);
              }
              if (!authored_tangent_frame) {
                float wv2[9];
                WorldVerts9(hs_, hs_.instances[static_cast<size_t>(hitInst)],
                            static_cast<int>(tri), wv2);
                const float* tv = wv2;
                const float* tuv = &cpuUv_[static_cast<size_t>(tri) * 6];
                const float uv0[2] = {tuv[0], tuv[1]};
                const float uv1[2] = {tuv[2], tuv[3]};
                const float uv2[2] = {tuv[4], tuv[5]};
                BuildShadingTangentFrame(tv, tv + 3, tv + 6, uv0, uv1, uv2, nrm,
                                         &shading_frame);
              }
              have_shading_frame = true;
            }
          }
          float base[3] = {0.8f, 0.8f, 0.8f};
          if (matId >= 0 && static_cast<size_t>(matId) * 3 + 3 <= hs_.matBase.size()) {
            base[0] = hs_.matBase[static_cast<size_t>(matId) * 3 + 0];
            base[1] = hs_.matBase[static_cast<size_t>(matId) * 3 + 1];
            base[2] = hs_.matBase[static_cast<size_t>(matId) * 3 + 2];
          }
          // Base-color texture (matTex slot 0), UV set 0. No other semantic
          // slots (metallic/roughness/normal/emissive/opacity) yet -- a first
          // cut, replacing the flat constant fallback with the sampled texel
          // when the material has one.
          if (matId >= 0 && !hs_.matTex.empty() &&
              static_cast<size_t>(matId) * kRtMaterialTexSlots < hs_.matTex.size()) {
            const int baseTex = hs_.matTex[static_cast<size_t>(matId) * kRtMaterialTexSlots + 0];
            if (baseTex >= 0 && tri * 6 + 5 < cpuUv_.size()) {
              const float* tex_params = CpuTextureParams(hs_, matId);
              const bool uv_set1 =
                  CpuSlotUsesSecondUvSet(tex_params, 0) &&
                  static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size();
              const float* src_uv = uv_set1
                  ? &cpuUv1_[static_cast<size_t>(tri) * 6]
                  : &cpuUv_[static_cast<size_t>(tri) * 6];
              float uv[2];
              Interp6(src_uv, hit.u, hit.v, uv);
              float slot_uv[2];
              CpuXformSlotUv(tex_params, 0, uv[0], uv[1], slot_uv);
              float texCol[3];
              const int base_face = ptexFaceFor(static_cast<int>(tri));
              const CpuPtexSampler* base_src = ptexAt(baseTex);
              const uint32_t base_mip = ptexMipOf(base_src, base_face);
              const CpuPtexSampler::Footprint base_fp =
                  ptexFpOf(base_src, base_face);
              SampleTextureBilinear(hs_.textures, hs_.texels, baseTex,
                                    slot_uv[0], slot_uv[1], texCol, base_face,
                                    base_src, base_mip,
                                    use_ptex_footprint ? &base_fp : nullptr);
              if (probe_enabled && base_src && base_face >= 0) {
                CpuPtexProbe& probe =
                    probes[static_cast<size_t>(y) * static_cast<size_t>(w) +
                           static_cast<size_t>(x)];
                probe.tex = static_cast<uint32_t>(baseTex);
                probe.face = static_cast<uint32_t>(base_face);
                probe.mip = static_cast<int>(base_mip);
                probe.srgb = hs_.textures[static_cast<size_t>(baseTex)].srgb;
                probe.u = slot_uv[0];
                probe.v = slot_uv[1];
                probe.rgb[0] = texCol[0];
                probe.rgb[1] = texCol[1];
                probe.rgb[2] = texCol[2];
              }
              base[0] *= texCol[0]; base[1] *= texCol[1]; base[2] *= texCol[2];
            }
          }
          if (matId >= 0 && static_cast<size_t>(matId) * kRtMaterialGraphFloats +
                                  kRtMaterialGraphHeaderFloats <= hs_.matGraph.size()) {
            const size_t graphBase = static_cast<size_t>(matId) * kRtMaterialGraphFloats;
            if (hs_.matGraph[graphBase + 1] >= 0.0f && tri * 6 + 5 < cpuUv_.size()) {
              float uv[2];
              Interp6(&cpuUv_[static_cast<size_t>(tri) * 6], hit.u, hit.v, uv);
              float uv1[2] = {uv[0], uv[1]};
              if (static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size())
                Interp6(&cpuUv1_[static_cast<size_t>(tri) * 6], hit.u, hit.v, uv1);
              const float graphPosition[3]={rayOrg[0]+dir[0]*hit.t,
                                            rayOrg[1]+dir[1]*hit.t,
                                            rayOrg[2]+dir[2]*hit.t};
              const float graphViewDirection[3]={-dir[0],-dir[1],-dir[2]};
              const auto graphBaseColor = EvalCpuMaterialXGraph(hs_, matId, 0,
                                                                  uv[0], uv[1],
                                                                  uv1[0], uv1[1],
                                                                  nrm,graphPosition,
                                                                  graphViewDirection,
                                                                  0.0f, 0.0f,
                                                                  (static_cast<size_t>(tri) < cpuSourceTri_.size())
                                                                      ? cpuSourceTri_[static_cast<size_t>(tri)]
                                                                      : -1,
                                                                  have_shading_frame ? shading_frame.tangent : nullptr,
                                                                  have_shading_frame ? shading_frame.bitangent : nullptr);
              base[0] = graphBaseColor[0];
              base[1] = graphBaseColor[1];
              base[2] = graphBaseColor[2];
            }
          }
          if (hitInst >= 0) {
            const int instanceIndex = hitInst;
            if (instanceIndex >= 0 && static_cast<size_t>(instanceIndex) < hs_.instances.size()) {
              const Inst& inst = hs_.instances[static_cast<size_t>(instanceIndex)];
              base[0] *= inst.tint[0];
              base[1] *= inst.tint[1];
              base[2] *= inst.tint[2];
            }
          }
          const float hitP[3] = {rayOrg[0] + dir[0] * hit.t,
                                 rayOrg[1] + dir[1] * hit.t,
                                 rayOrg[2] + dir[2] * hit.t};
          // Perturb the shading normal from the material's normal map. The
          // Vulkan RT shader already rebuilds a tangent frame from the
          // triangle's UVs; the CPU preview used to shade with the geometric
          // normal, which silently ignored normal maps. The frame is rebuilt
          // from the lane's own UV set/transform when the map is sampled.
          if (have_shading_frame) {
            float tangent_normal[3] = {0.0f, 0.0f, 1.0f};
            float map_uv[2];
            Interp6(&cpuUv_[static_cast<size_t>(tri) * 6], hit.u, hit.v, map_uv);
            bool have_normal = false;
            ShadingTangentFrame normal_frame = shading_frame;
            const size_t graph_base =
                static_cast<size_t>(matId) * kRtMaterialGraphFloats;
            const float graphPosition[3] = {hitP[0], hitP[1], hitP[2]};
            const float graphViewDirection[3] = {-dir[0], -dir[1], -dir[2]};
            if (graph_base + kRtMaterialGraphHeaderFloats <=
                    hs_.matGraph.size() &&
                hs_.matGraph[graph_base + 1 + 5] >= 0.0f) {
              // Route 5 is the MaterialX normal output: a unit tangent-space
              // normal encoded in [0,1].
              const auto gn = EvalCpuMaterialXGraph(
                  hs_, matId, 5, map_uv[0], map_uv[1], map_uv[0], map_uv[1], nrm,
                  graphPosition, graphViewDirection, 0.0f, 0.0f,
                  (static_cast<size_t>(tri) < cpuSourceTri_.size())
                      ? cpuSourceTri_[static_cast<size_t>(tri)]
                      : -1,
                  shading_frame.tangent, shading_frame.bitangent);
              tangent_normal[0] = gn[0] * 2.0f - 1.0f;
              tangent_normal[1] = gn[1] * 2.0f - 1.0f;
              tangent_normal[2] = gn[2] * 2.0f - 1.0f;
              have_normal = true;
            } else if (static_cast<size_t>(matId) * kRtMaterialTexSlots + 3 <
                           hs_.matTex.size() &&
                       hs_.matTex[static_cast<size_t>(matId) *
                                      kRtMaterialTexSlots + 3] >= 0) {
              const int normal_tex =
                  hs_.matTex[static_cast<size_t>(matId) * kRtMaterialTexSlots + 3];
              const float* tex_params = CpuTextureParams(hs_, matId);
              const bool uv_set1 =
                  CpuSlotUsesSecondUvSet(tex_params, 3) &&
                  static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size();
              const float* src_uv = uv_set1
                  ? &cpuUv1_[static_cast<size_t>(tri) * 6]
                  : &cpuUv_[static_cast<size_t>(tri) * 6];
              float uv[2];
              Interp6(src_uv, hit.u, hit.v, uv);
              float slot_uv[2];
              CpuXformSlotUv(tex_params, 3, uv[0], uv[1], slot_uv);
              float rgb[3];
              const int normal_face = ptexFaceFor(static_cast<int>(tri));
              const CpuPtexSampler* normal_src = ptexAt(normal_tex);
              const CpuPtexSampler::Footprint normal_fp =
                  ptexFpOf(normal_src, normal_face);
              SampleTextureBilinear(hs_.textures, hs_.texels, normal_tex,
                                    slot_uv[0], slot_uv[1], rgb, normal_face,
                                    normal_src,
                                    ptexMipOf(normal_src, normal_face),
                                    use_ptex_footprint ? &normal_fp : nullptr);
              tangent_normal[0] = rgb[0] * 2.0f - 1.0f;
              tangent_normal[1] = rgb[1] * 2.0f - 1.0f;
              tangent_normal[2] = rgb[2] * 2.0f - 1.0f;
              have_normal = true;
              // The frame must use the same UV set and transformed
              // coordinates as the normal sample, matching the shaders.
              float frame_uv[6];
              for (int c = 0; c < 3; ++c) {
                float corner[2];
                CpuXformSlotUv(tex_params, 3, src_uv[c * 2],
                               src_uv[c * 2 + 1], corner);
                frame_uv[c * 2] = corner[0];
                frame_uv[c * 2 + 1] = corner[1];
              }
              if (!authored_tangent_frame) {
                float wv3[9];
                WorldVerts9(hs_, hs_.instances[static_cast<size_t>(hitInst)],
                            static_cast<int>(tri), wv3);
                const float* tv = wv3;
                BuildShadingTangentFrame(tv, tv + 3, tv + 6, frame_uv,
                                         frame_uv + 2, frame_uv + 4, nrm,
                                         &normal_frame);
              }
            }
            if (have_normal) {
              float world[3];
              ApplyShadingTangentNormal(normal_frame, tangent_normal, world);
              nrm[0] = world[0];
              nrm[1] = world[1];
              nrm[2] = world[2];
            }
          }
          // OpenPBR applies a separate coat normal (matTex slot 11) with its
          // own UV set/transform and tangent frame. The CPU preview shaded the
          // coat lobe with the surface normal, so an authored coat map was
          // silently ignored. Sample it here; without one the coat lobe uses
          // the surface normal.
          float coatNrm[3] = {nrm[0], nrm[1], nrm[2]};
          if (matId >= 0 &&
              static_cast<size_t>(matId) * kRtMaterialTexSlots + 11 <
                  hs_.matTex.size()) {
            const int coat_tex =
                hs_.matTex[static_cast<size_t>(matId) * kRtMaterialTexSlots +
                           11];
            if (coat_tex >= 0 && static_cast<size_t>(tri) * 6 + 5 <
                                     cpuUv_.size()) {
              const float* tp = CpuTextureParams(hs_, matId);
              const bool uv_set1 = tp != nullptr &&
                                   static_cast<int>(tp[146] + 0.5f) == 1 &&
                                   static_cast<size_t>(tri) * 6 + 5 <
                                       cpuUv1_.size();
              const float* src_uv = uv_set1
                  ? &cpuUv1_[static_cast<size_t>(tri) * 6]
                  : &cpuUv_[static_cast<size_t>(tri) * 6];
              float uv[2];
              Interp6(src_uv, hit.u, hit.v, uv);
              float slot_uv[2];
              CpuXformUvAt(tp, 140, uv[0], uv[1], slot_uv);
              float rgb[3];
              const int coat_face = ptexFaceFor(static_cast<int>(tri));
              const CpuPtexSampler* coat_src = ptexAt(coat_tex);
              const CpuPtexSampler::Footprint coat_fp =
                  ptexFpOf(coat_src, coat_face);
              SampleTextureBilinear(hs_.textures, hs_.texels, coat_tex,
                                    slot_uv[0], slot_uv[1], rgb, coat_face,
                                    coat_src, ptexMipOf(coat_src, coat_face),
                                    use_ptex_footprint ? &coat_fp : nullptr);
              // Packed coat-normal scale/bias (147/151); the loader stores the
              // usual 2/-1 when the texture authors defaults.
              float co_n[3];
              for (int c = 0; c < 3; ++c) {
                const float scale = tp ? tp[147 + c] : 2.0f;
                const float bias = tp ? tp[151 + c] : -1.0f;
                co_n[c] = rgb[c] * scale + bias;
              }
              float frame_uv[6];
              for (int c = 0; c < 3; ++c) {
                float corner[2];
                CpuXformUvAt(tp, 140, src_uv[c * 2], src_uv[c * 2 + 1],
                             corner);
                frame_uv[c * 2] = corner[0];
                frame_uv[c * 2 + 1] = corner[1];
              }
              float wv4[9];
              WorldVerts9(hs_, hs_.instances[static_cast<size_t>(hitInst)],
                          static_cast<int>(tri), wv4);
              const float* tv = wv4;
              ShadingTangentFrame coat_frame;
              BuildShadingTangentFrame(tv, tv + 3, tv + 6, frame_uv,
                                       frame_uv + 2, frame_uv + 4, nrm,
                                       &coat_frame);
              float world[3];
              ApplyShadingTangentNormal(coat_frame, co_n, world);
              coatNrm[0] = world[0];
              coatNrm[1] = world[1];
              coatNrm[2] = world[2];
            }
          }
          float direct = 0.0f;
          float directCoat = 0.0f;
          bool hadDirectLight = false;
          const int lightCount = hs_.numLights;
          for (int li = 0; li < lightCount; ++li) {
            const float* lp = &hs_.lightParams[static_cast<size_t>(li) *
                                                kRtLightParamFloats];
            const int lightType = static_cast<int>(lp[0] + 0.5f);
            if (lightType == static_cast<int>(DrawLightCPU::Type::Dome) ||
                lp[1] <= 0.5f) continue;
            const int instanceIndex = hitInst;
            if (li < 32 && instanceIndex >= 0 &&
                static_cast<size_t>(instanceIndex) < hs_.instances.size() &&
                (hs_.instances[static_cast<size_t>(instanceIndex)].directLightMask &
                 (1u << li)) == 0) continue;
            hadDirectLight = true;
            float lightP[3] = {lp[4], lp[5], lp[6]};
            float emitterN[3] = {0.0f, 0.0f, 0.0f};
            float emitter[3] = {lp[12], lp[13], lp[14]};
            if (lightType == static_cast<int>(DrawLightCPU::Type::Geometry) &&
                lp[52] > 2.75f && lp[52] < 3.25f) {
              const int triOffset = static_cast<int>(lp[53] + 0.5f);
              const int instance = static_cast<int>(lp[55] + 0.5f);
              if (triOffset >= 0 && instance >= 0 &&
                  static_cast<size_t>(triOffset) < hs_.triCount &&
                  static_cast<size_t>(instance) < hs_.instances.size()) {
                const float* tv = &hs_.tris[static_cast<size_t>(triOffset) * 9];
                float center[3] = {(tv[0] + tv[3] + tv[6]) / 3.0f,
                                   (tv[1] + tv[4] + tv[7]) / 3.0f,
                                   (tv[2] + tv[5] + tv[8]) / 3.0f};
                const Inst& inst = hs_.instances[static_cast<size_t>(instance)];
                TransformPoint(inst.o2w, center, lightP);
                float e1[3] = {tv[3] - tv[0], tv[4] - tv[1], tv[5] - tv[2]};
                float e2[3] = {tv[6] - tv[0], tv[7] - tv[1], tv[8] - tv[2]};
                float no[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                               e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0]};
                TransformNormal(inst.w2o, no, emitterN);
                const int mid = (static_cast<size_t>(triOffset) < hs_.mat.size())
                                    ? hs_.mat[static_cast<size_t>(triOffset)] : -1;
                if (mid >= 0 && static_cast<size_t>(mid) * 6 + 5 < hs_.matPbr.size()) {
                  emitter[0] = hs_.matPbr[static_cast<size_t>(mid) * 6 + 2];
                  emitter[1] = hs_.matPbr[static_cast<size_t>(mid) * 6 + 3];
                  emitter[2] = hs_.matPbr[static_cast<size_t>(mid) * 6 + 4];
                }
              }
            }
            float L[3] = {lightP[0] - hitP[0], lightP[1] - hitP[1],
                          lightP[2] - hitP[2]};
            float attenuation = 1.0f;
            float maxShadowT = 1.0e30f;
            if (lightType == static_cast<int>(DrawLightCPU::Type::Distant)) {
              L[0] = -lp[8]; L[1] = -lp[9]; L[2] = -lp[10];
            } else {
              const float d2 = std::max(1.0e-6f, L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
              maxShadowT = std::sqrt(d2);
              attenuation = 1.0f / d2;
            }
            Normalize3(L);
            float shape = 1.0f;
            if (lightType == static_cast<int>(DrawLightCPU::Type::Geometry)) {
              shape *= std::max(0.0f, emitterN[0] * -L[0] +
                                      emitterN[1] * -L[1] + emitterN[2] * -L[2]);
            }
            const float nl = std::max(0.0f, nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2]);
            const float nlCoat = std::max(0.0f, coatNrm[0] * L[0] +
                                                    coatNrm[1] * L[1] +
                                                    coatNrm[2] * L[2]);
            if ((nl <= 0.0f && nlCoat <= 0.0f) || shape <= 0.0f) continue;
            float shadow = 1.0f;
            if ((static_cast<int>(lp[1] + 0.5f) & (1 << 1)) != 0) {
              lrt_ray shadowRay{};
              shadowRay.org[0] = hitP[0] + nrm[0] * 1e-3f;
              shadowRay.org[1] = hitP[1] + nrm[1] * 1e-3f;
              shadowRay.org[2] = hitP[2] + nrm[2] * 1e-3f;
              shadowRay.dir[0] = L[0]; shadowRay.dir[1] = L[1]; shadowRay.dir[2] = L[2];
              shadowRay.tmin = 1e-4f; shadowRay.tmax = maxShadowT;
              bool shadowLinked = true;
              if (li < 32 && instanceIndex >= 0 &&
                  static_cast<size_t>(instanceIndex) < hs_.instances.size()) {
                shadowLinked = (hs_.instances[static_cast<size_t>(instanceIndex)].shadowLightMask &
                                (1u << li)) != 0;
              }
              if (shadowLinked) {
                // Reference-quality colored transmission shadows. The old
                // any-hit query made a 1%-opaque decal or glass globe cast the
                // same black shadow as solid metal.
                float transmittance[3] = {1.0f, 1.0f, 1.0f};
                float cursor = shadowRay.tmin;
                struct ShadowMedium {
                  int instance{-1};
                  int entryTri{-1};
                };
                std::array<ShadowMedium, 8> media{};
                int mediumDepth = 0;
                for (int layer = 0; layer < 12; ++layer) {
                  shadowRay.tmin = cursor;
                  float shadowT = 0.0f, shadowU = 0.0f, shadowV = 0.0f;
                  int shadowInst = -1, shadowSrc = -1;
                  if (!Intersect(shadowRay, &shadowT, &shadowU, &shadowV,
                                 &shadowInst, &shadowSrc)) {
                    // Finite local/area lights can end inside a volume. Apply
                    // the remaining segment; an unmatched directional-light
                    // shell is treated as malformed/open and keeps fallback
                    // interface behavior instead of attenuating over infinity.
                    if (mediumDepth > 0 && maxShadowT < 1.0e20f) {
                      const auto volumeTint = cpuLayerTransmission(
                          media[static_cast<size_t>(mediumDepth)].entryTri,
                          std::max(0.0f, maxShadowT - cursor));
                      for (int c = 0; c < 3; ++c)
                        transmittance[c] *= volumeTint[c];
                    }
                    break;
                  }
                  const int shadowTri = shadowSrc;
                  if (mediumDepth > 0) {
                    const auto volumeTint = cpuLayerTransmission(
                        media[static_cast<size_t>(mediumDepth)].entryTri,
                        std::max(0.0f, shadowT - cursor));
                    for (int c = 0; c < 3; ++c)
                      transmittance[c] *= volumeTint[c];
                  }
                  const float alpha = cpuLayerAlpha(shadowTri, shadowInst,
                                                    shadowU, shadowV);
                  // Zero-depth OpenPBR color is an interface tint. For a
                  // positive depth this is unity; attenuation was applied to
                  // the measured segment above.
                  const auto tint = cpuLayerTransmission(shadowTri, 0.0f);
                  const float transmissionWeight =
                      cpuLayerTransmissionWeight(shadowTri, shadowInst,
                                                 shadowU, shadowV);
                  for (int c = 0; c < 3; ++c) {
                    const float through = transmissionWeight > 0.0f
                                              ? tint[c] * (1.0f - alpha)
                                              : (1.0f - alpha);
                    transmittance[c] *= std::clamp(through, 0.0f, 1.0f);
                  }
                  const int shadowInstance = shadowInst;
                  const int shadowMaterial =
                      static_cast<size_t>(shadowTri) < cpuMat_.size()
                          ? cpuMat_[static_cast<size_t>(shadowTri)] : -1;
                  const size_t shadowMb = shadowMaterial >= 0
                      ? static_cast<size_t>(shadowMaterial) *
                            kLightRtOpenPBRFloats
                      : hs_.matLightRt.size();
                  const bool thinWalled = shadowMb + 71 < hs_.matLightRt.size() &&
                      hs_.matLightRt[shadowMb + 71] >= 0.5f;
                  bool frontFacing = true;
                  float swb[9];
                  const size_t tb = static_cast<size_t>(shadowTri) * 9;
                  if (tb + 8 < cpuTris_.size()) {
                    WorldVerts9(hs_,
                                hs_.instances[static_cast<size_t>(shadowInst)],
                                shadowTri, swb);
                    const float* tv = swb;
                    const float e1[3] = {tv[3] - tv[0], tv[4] - tv[1],
                                         tv[5] - tv[2]};
                    const float e2[3] = {tv[6] - tv[0], tv[7] - tv[1],
                                         tv[8] - tv[2]};
                    const float gn[3] = {
                        e1[1] * e2[2] - e1[2] * e2[1],
                        e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};
                    frontFacing = gn[0] * L[0] + gn[1] * L[1] +
                                      gn[2] * L[2] < 0.0f;
                  }
                  if (transmissionWeight > 1.0e-4f && !thinWalled) {
                    int matched = -1;
                    for (int level = mediumDepth; level > 0; --level) {
                      if (media[static_cast<size_t>(level)].instance ==
                          shadowInstance) {
                        matched = level;
                        break;
                      }
                    }
                    const bool exiting = matched > 0 ||
                                         (!frontFacing && mediumDepth == 0);
                    if (!exiting && mediumDepth < 7) {
                      ++mediumDepth;
                      media[static_cast<size_t>(mediumDepth)] =
                          {shadowInstance, shadowTri};
                    } else if (matched > 0) {
                      mediumDepth = matched - 1;
                    }
                  }
                  if (transmittance[0] + transmittance[1] +
                          transmittance[2] < 0.003f)
                    break;
                  cursor = shadowT + 1.0e-4f;
                  if (cursor >= shadowRay.tmax) break;
                }
                shadow = 0.2126f * transmittance[0] +
                         0.7152f * transmittance[1] +
                         0.0722f * transmittance[2];
              }
            }
            const float lightToPoint[3] = {-L[0], -L[1], -L[2]};
            const float radiance = std::max(0.0f, emitter[0]) * 0.3333333f +
                                   std::max(0.0f, emitter[1]) * 0.3333333f +
                                   std::max(0.0f, emitter[2]) * 0.3333333f;
            const float lightTerm = attenuation * shape * shadow * radiance *
                                    PackedIesFactor(lp, lightToPoint) * lp[29];
            direct += nl * lightTerm;
            directCoat += nlCoat * lightTerm;
          }
          const float diff = hadDirectLight ? direct :
              std::max(0.0f, nrm[0] * lightDirN[0] + nrm[1] * lightDirN[1] +
                             nrm[2] * lightDirN[2]);
          const float diffCoat = hadDirectLight ? directCoat :
              std::max(0.0f, coatNrm[0] * lightDirN[0] +
                             coatNrm[1] * lightDirN[1] +
                             coatNrm[2] * lightDirN[2]);
          // Keep the CPU preview on the same compact OpenPBR material lanes
          // as the CUDA/Vulkan paths.  It remains a single-light preview, but
          // no longer collapses metallic, transmission, coat, and subsurface
          // materials to an unconditional Lambertian response.
          float metallic = 0.0f, roughness = 0.5f, transmission = 0.0f;
          float specWeight = 1.0f, ior = 1.5f, coatWeight = 0.0f;
          float coatRoughness = 0.2f;
          float subsurface = 0.0f, subsurfaceRadius = 1.0f;
          float anisotropy = 0.0f, anisotropyRotation = 0.0f;
          float sheenWeight = 0.0f, sheenRoughness = 0.3f;
          float coatAffectColor = 0.0f, coatAffectRoughness = 0.0f;
          float coatDarkening = 0.0f;
          float thinFilmWeight = 0.0f, thinFilmThickness = 0.0f;
          float thinFilmIor = 1.5f;
          float emission[3] = {0.0f, 0.0f, 0.0f};
          float specColor[3] = {1.0f, 1.0f, 1.0f};
          float sheenColor[3] = {1.0f, 1.0f, 1.0f};
          float coatColor[3] = {1.0f, 1.0f, 1.0f};
          if (matId >= 0) {
            const size_t pb = static_cast<size_t>(matId) * 6;
            if (pb + 5 < hs_.matPbr.size()) {
              metallic = std::clamp(hs_.matPbr[pb + 0], 0.0f, 1.0f);
              roughness = std::clamp(hs_.matPbr[pb + 1], 0.03f, 1.0f);
              emission[0] = hs_.matPbr[pb + 2];
              emission[1] = hs_.matPbr[pb + 3];
              emission[2] = hs_.matPbr[pb + 4];
            }
            const size_t lb = static_cast<size_t>(matId) * kLightRtOpenPBRFloats;
            if (lb + 79 < hs_.matLightRt.size()) {
              const float* lrt = &hs_.matLightRt[lb];
              specColor[0] = std::max(0.0f, lrt[4]);
              specColor[1] = std::max(0.0f, lrt[5]);
              specColor[2] = std::max(0.0f, lrt[6]);
              specWeight = std::clamp(lrt[7], 0.0f, 1.0f);
              sheenColor[0] = std::max(0.0f, lrt[28]);
              sheenColor[1] = std::max(0.0f, lrt[29]);
              sheenColor[2] = std::max(0.0f, lrt[30]);
              sheenWeight = std::clamp(lrt[31], 0.0f, 1.0f);
              transmission = std::clamp(lrt[11], 0.0f, 1.0f);
              subsurface = std::clamp(lrt[19], 0.0f, 1.0f);
              subsurfaceRadius = std::max(
                  0.0f, (0.2126f * lrt[20] + 0.7152f * lrt[21] +
                         0.0722f * lrt[22]) * lrt[23]);
              coatWeight = std::clamp(lrt[27], 0.0f, 1.0f);
              coatColor[0] = std::max(0.0f, lrt[24]);
              coatColor[1] = std::max(0.0f, lrt[25]);
              coatColor[2] = std::max(0.0f, lrt[26]);
              coatRoughness = std::clamp(lrt[44], 0.03f, 1.0f);
              sheenRoughness = std::clamp(lrt[46], 0.0f, 1.0f);
              thinFilmWeight = std::clamp(lrt[48], 0.0f, 1.0f);
              thinFilmThickness = std::max(lrt[49], 0.0f);
              thinFilmIor = std::max(lrt[50], 1.0f);
              ior = std::max(lrt[43], 1.0f);
              anisotropy = std::clamp(lrt[57] + 0.5f * lrt[59], -1.0f, 1.0f);
              anisotropyRotation = lrt[58];
              coatAffectColor = std::clamp(lrt[67], 0.0f, 1.0f);
              coatAffectRoughness = std::clamp(lrt[68], 0.0f, 1.0f);
              coatDarkening = std::clamp(lrt[70], 0.0f, 1.0f);
              emission[0] += lrt[76] * std::max(lrt[79], 0.0f);
              emission[1] += lrt[77] * std::max(lrt[79], 0.0f);
              emission[2] += lrt[78] * std::max(lrt[79], 0.0f);
            }
          }
          // Sample the semantic texture lanes the CPU preview consumes with
          // the same packed UV-set/transform/channel/scale-bias rules as the
          // shaders. The loader neutralizes a lane's constant to 1 whenever a
          // texture is bound, so constant * sample is the right combine.
          float occlusion = 1.0f;
          if (matId >= 0) {
            const float* tp = CpuTextureParams(hs_, matId);
            const size_t tex_base =
                static_cast<size_t>(matId) * kRtMaterialTexSlots;
            auto lane_tex = [&](size_t slot) -> int {
              return tex_base + slot < hs_.matTex.size()
                         ? hs_.matTex[tex_base + slot]
                         : -1;
            };
            auto lane_set1 = [&](int uvset_float, int bit) {
              return tp != nullptr &&
                     ((static_cast<int>(tp[uvset_float] + 0.5f) &
                       (1 << bit)) != 0);
            };
            auto lane_channel = [&](int offset) {
              return tp ? static_cast<int>(tp[offset] + 0.5f) : 0;
            };
            auto lane_scale = [&](int offset, int c) {
              return tp ? tp[offset + c] : 1.0f;
            };
            auto lane_bias = [&](int offset, int c) {
              return tp ? tp[offset + c] : 0.0f;
            };
            const int tri_i = static_cast<int>(tri);
            const int lane_face = ptexFaceFor(tri_i);
            auto lane_src = [&](size_t slot) -> const CpuPtexSampler* {
              return ptexAt(lane_tex(slot));
            };
            auto lane_mip = [&](size_t slot) -> uint32_t {
              const CpuPtexSampler* sampler = lane_src(slot);
              return ptexMipOf(sampler, lane_face);
            };
            CpuPtexSampler::Footprint lane_fps[16];
            auto lane_fp = [&](size_t slot)
                -> const CpuPtexSampler::Footprint* {
              if (!use_ptex_footprint) return nullptr;
              lane_fps[slot] = ptexFpOf(lane_src(slot), lane_face);
              return &lane_fps[slot];
            };
            float v = 0.0f;
            if (CpuSampleSemanticScalar(hs_, matId, lane_tex(1), tri_i, hit.u,
                                        hit.v, lane_set1(69, 1), 6,
                                        lane_channel(60), cpuUv_, cpuUv1_,
                                        &v, lane_face, lane_src(1),
                                        lane_mip(1), lane_fp(1))) {
              metallic = std::clamp(
                  metallic * (v * lane_scale(61, 0) + lane_bias(62, 0)),
                  0.0f, 1.0f);
            }
            if (CpuSampleSemanticScalar(hs_, matId, lane_tex(2), tri_i, hit.u,
                                        hit.v, lane_set1(69, 2), 12,
                                        lane_channel(63), cpuUv_, cpuUv1_,
                                        &v, lane_face, lane_src(2),
                                        lane_mip(2), lane_fp(2))) {
              roughness = std::clamp(
                  roughness * (v * lane_scale(64, 0) + lane_bias(65, 0)),
                  0.03f, 1.0f);
            }
            float rgb[3];
            if (CpuSampleSemanticRgb(hs_, matId, lane_tex(4), tri_i, hit.u,
                                     hit.v, lane_set1(69, 4), 24, cpuUv_,
                                     cpuUv1_, rgb, lane_face, lane_src(4),
                                     lane_mip(4), lane_fp(4))) {
              for (int c = 0; c < 3; ++c) {
                emission[c] = std::max(
                    0.0f, emission[c] *
                              (rgb[c] * lane_scale(52, c) + lane_bias(56, c)));
              }
            }
            if (CpuSampleSemanticRgb(hs_, matId, lane_tex(10), tri_i, hit.u,
                                     hit.v,
                                     tp != nullptr &&
                                         static_cast<int>(tp[130] + 0.5f) == 1,
                                     124, cpuUv_, cpuUv1_, rgb, lane_face,
                                     lane_src(10), lane_mip(10), lane_fp(10))) {
              for (int c = 0; c < 3; ++c) {
                specColor[c] = std::max(
                    0.0f, specColor[c] *
                              (rgb[c] * lane_scale(132, c) + lane_bias(136, c)));
              }
            }
            if (CpuSampleSemanticScalar(hs_, matId, lane_tex(7), tri_i, hit.u,
                                        hit.v, lane_set1(99, 1), 78,
                                        lane_channel(97), cpuUv_, cpuUv1_,
                                        &v, lane_face, lane_src(7),
                                        lane_mip(7), lane_fp(7))) {
              coatWeight = std::clamp(
                  coatWeight * (v * lane_scale(100, 0) + lane_bias(104, 0)),
                  0.0f, 1.0f);
            }
            if (CpuSampleSemanticRgb(hs_, matId, lane_tex(8), tri_i, hit.u,
                                     hit.v, lane_set1(99, 2), 84, cpuUv_,
                                     cpuUv1_, rgb, lane_face, lane_src(8),
                                     lane_mip(8), lane_fp(8))) {
              for (int c = 0; c < 3; ++c) {
                coatColor[c] = std::max(
                    0.0f, coatColor[c] * (rgb[c] * lane_scale(108, c) +
                                          lane_bias(112, c)));
              }
            }
            if (CpuSampleSemanticScalar(hs_, matId, lane_tex(9), tri_i, hit.u,
                                        hit.v, lane_set1(99, 3), 90,
                                        lane_channel(98), cpuUv_, cpuUv1_,
                                        &v, lane_face, lane_src(9),
                                        lane_mip(9), lane_fp(9))) {
              coatRoughness = std::clamp(
                  coatRoughness * (v * lane_scale(116, 0) + lane_bias(120, 0)),
                  0.03f, 1.0f);
            }
            if (CpuSampleSemanticScalar(hs_, matId, lane_tex(6), tri_i, hit.u,
                                        hit.v, lane_set1(99, 0), 72,
                                        lane_channel(96), cpuUv_, cpuUv1_,
                                        &v, lane_face, lane_src(6),
                                        lane_mip(6), lane_fp(6))) {
              occlusion = std::clamp(
                  v * lane_scale(70, 0) + lane_bias(71, 0), 0.0f, 1.0f);
            }
            // MaterialX graph lanes override the constant/texture result,
            // matching the shaders' hasGraphRoute checks.
            const size_t gb =
                static_cast<size_t>(matId) * kRtMaterialGraphFloats;
            if (gb + kRtMaterialGraphHeaderFloats <= hs_.matGraph.size()) {
              float guv[2];
              Interp6(&cpuUv_[static_cast<size_t>(tri) * 6], hit.u, hit.v, guv);
              float guv1[2] = {guv[0], guv[1]};
              if (static_cast<size_t>(tri) * 6 + 5 < cpuUv1_.size()) {
                Interp6(&cpuUv1_[static_cast<size_t>(tri) * 6], hit.u, hit.v,
                        guv1);
              }
              const float graph_position[3] = {hitP[0], hitP[1], hitP[2]};
              const float graph_view[3] = {-dir[0], -dir[1], -dir[2]};
              const int source_tri =
                  static_cast<size_t>(tri) < cpuSourceTri_.size()
                      ? cpuSourceTri_[static_cast<size_t>(tri)]
                      : -1;
              auto graph_route = [&](int route) {
                return EvalCpuMaterialXGraph(
                    hs_, matId, route, guv[0], guv[1], guv1[0], guv1[1], nrm,
                    graph_position, graph_view, 0.0f, 0.0f, source_tri,
                    have_shading_frame ? shading_frame.tangent : nullptr,
                    have_shading_frame ? shading_frame.bitangent : nullptr);
              };
              if (hs_.matGraph[gb + 1 + 1] >= 0.0f) {
                metallic = std::clamp(graph_route(1)[0], 0.0f, 1.0f);
              }
              if (hs_.matGraph[gb + 1 + 2] >= 0.0f) {
                roughness = std::clamp(graph_route(2)[0], 0.03f, 1.0f);
              }
              if (hs_.matGraph[gb + 1 + 4] >= 0.0f) {
                const auto g = graph_route(4);
                emission[0] = std::max(g[0], 0.0f);
                emission[1] = std::max(g[1], 0.0f);
                emission[2] = std::max(g[2], 0.0f);
              }
              // Route 10 is OpenPBR specular_color, route 14 coat_color and
              // route 13 coat_weight. They were previously reachable only
              // through a *texture* lane, so a MaterialX graph driving them
              // changed nothing (measured: coat_color low/high rendered
              // byte-identically). Evaluate them like metallic/roughness.
              if (hs_.matGraph[gb + 1 + 10] >= 0.0f) {
                const auto g = graph_route(10);
                specColor[0] = std::max(g[0], 0.0f);
                specColor[1] = std::max(g[1], 0.0f);
                specColor[2] = std::max(g[2], 0.0f);
              }
              if (hs_.matGraph[gb + 1 + 13] >= 0.0f) {
                coatWeight = std::clamp(graph_route(13)[0], 0.0f, 1.0f);
              }
              if (hs_.matGraph[gb + 1 + 14] >= 0.0f) {
                const auto g = graph_route(14);
                coatColor[0] = std::max(g[0], 0.0f);
                coatColor[1] = std::max(g[1], 0.0f);
                coatColor[2] = std::max(g[2], 0.0f);
              }
            }
          }
          const float viewLen = std::sqrt(
              (camPos[0] - hitP[0]) * (camPos[0] - hitP[0]) +
              (camPos[1] - hitP[1]) * (camPos[1] - hitP[1]) +
              (camPos[2] - hitP[2]) * (camPos[2] - hitP[2]));
          const float nv = std::max(1.0e-4f,
                                    (nrm[0] * (camPos[0] - hitP[0]) +
                                     nrm[1] * (camPos[1] - hitP[1]) +
                                     nrm[2] * (camPos[2] - hitP[2])) /
                                        std::max(viewLen, 1.0e-4f));
          float directionalRoughness = roughness;
          if (std::fabs(anisotropy) > 1.0e-4f &&
              static_cast<size_t>(tri) * 6 + 5 < cpuUv_.size() &&
              static_cast<size_t>(tri) * 9 + 8 < cpuTris_.size()) {
            float wv5[9];
            WorldVerts9(hs_, hs_.instances[static_cast<size_t>(hitInst)],
                        static_cast<int>(tri), wv5);
            const float* tv = wv5;
            const float* uv = &cpuUv_[static_cast<size_t>(tri) * 6];
            const float e1[3] = {tv[3] - tv[0], tv[4] - tv[1], tv[5] - tv[2]};
            const float e2[3] = {tv[6] - tv[0], tv[7] - tv[1], tv[8] - tv[2]};
            const float du1 = uv[2] - uv[0], dv1 = uv[3] - uv[1];
            const float du2 = uv[4] - uv[0], dv2 = uv[5] - uv[1];
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) > 1.0e-8f) {
              float t[3] = {(e1[0] * dv2 - e2[0] * dv1) / det,
                            (e1[1] * dv2 - e2[1] * dv1) / det,
                            (e1[2] * dv2 - e2[2] * dv1) / det};
              const float tn = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
              if (tn > 1.0e-6f) {
                for (float& c : t) c /= tn;
                float b[3] = {nrm[1] * t[2] - nrm[2] * t[1],
                              nrm[2] * t[0] - nrm[0] * t[2],
                              nrm[0] * t[1] - nrm[1] * t[0]};
                const float rot = anisotropyRotation * 0.01745329252f;
                const float cr = std::cos(rot), sr = std::sin(rot);
                const float rt[3] = {t[0] * cr + b[0] * sr,
                                     t[1] * cr + b[1] * sr,
                                     t[2] * cr + b[2] * sr};
                const float rb[3] = {-t[0] * sr + b[0] * cr,
                                     -t[1] * sr + b[1] * cr,
                                     -t[2] * sr + b[2] * cr};
                float halfVector[3] = {lightDirN[0] + (camPos[0] - hitP[0]) / viewLen,
                              lightDirN[1] + (camPos[1] - hitP[1]) / viewLen,
                              lightDirN[2] + (camPos[2] - hitP[2]) / viewLen};
                const float hn = std::sqrt(halfVector[0] * halfVector[0] +
                                           halfVector[1] * halfVector[1] +
                                           halfVector[2] * halfVector[2]);
                if (hn > 1.0e-6f) {
                  for (float& c : halfVector) c /= hn;
                  const float ht = halfVector[0] * rt[0] + halfVector[1] * rt[1] +
                                   halfVector[2] * rt[2];
                  const float hb = halfVector[0] * rb[0] + halfVector[1] * rb[1] +
                                   halfVector[2] * rb[2];
                  directionalRoughness = std::clamp(
                      roughness * (1.0f + anisotropy * (ht * ht - hb * hb)),
                      0.03f, 1.0f);
                }
              }
            }
          }
          const float f0 = std::pow((ior - 1.0f) / (ior + 1.0f), 2.0f);
          const float fresnel = FresnelDielectric(nv, 1.0f, ior);
          if (thinFilmWeight > 0.0f && thinFilmThickness > 0.0f) {
            const float phase = 6.28318530718f * thinFilmIor *
                                thinFilmThickness *
                                (1.0f - std::clamp(nv, 0.0f, 1.0f));
            const float phaseMul[3] = {1.0f, 1.17f, 1.35f};
            const float phaseOff[3] = {0.0f, 2.1f, 4.2f};
            for (int c = 0; c < 3; ++c) {
              const float film = 0.5f + 0.5f *
                  std::cos(phase * phaseMul[c] + phaseOff[c]);
              const float filmF = std::clamp(
                  f0 + (1.0f - f0) * film * 0.65f, 0.0f, 1.0f);
              specColor[c] *= 1.0f - thinFilmWeight +
                              thinFilmWeight * (filmF / std::max(f0, 1.0e-4f));
            }
          }
          // Keep the CPU preview aligned with the packed OpenPBR coat and
          // sheen controls used by the Vulkan/CUDA paths.  These bounded
          // energy adjustments avoid turning either lobe into an additive
          // brightness spike on grazing views.
          const float coatColorMix = std::clamp(coatWeight * coatAffectColor,
                                                0.0f, 1.0f);
          for (int c = 0; c < 3; ++c) {
            base[c] *= 1.0f - 0.2f * std::clamp(coatWeight * coatDarkening,
                                                0.0f, 1.0f);
            base[c] *= 1.0f + (coatColor[c] - 1.0f) * coatColorMix;
          }
          const float coatRoughMix = std::clamp(coatWeight * coatAffectRoughness,
                                                0.0f, 1.0f);
          const float lobeRoughness = roughness * (1.0f - coatRoughMix) +
                                      std::max(0.03f, coatRoughness) * coatRoughMix;
          const float diffuseWeight = (1.0f - metallic) *
                                      (1.0f - transmission) *
                                      (1.0f - fresnel);
          const float radiusGain = std::clamp(std::sqrt(subsurfaceRadius) * 8.0f,
                                               0.0f, 1.0f);
          const float diffusion = subsurface *
              (0.25f + 0.75f * (1.0f - diff) * radiusGain);
          const float specular = specWeight * (fresnel + metallic) *
              diff / std::max(0.08f, directionalRoughness * directionalRoughness * 4.0f);
          const float coat = coatWeight * 0.04f * diffCoat /
                             std::max(0.08f, lobeRoughness * lobeRoughness);
          const float sheen = sheenWeight *
              (0.5f + 0.5f * sheenRoughness) *
              std::pow(1.0f - std::clamp(nv, 0.0f, 1.0f), 2.0f) * diff;
          const float lit = 0.15f * occlusion +
                            0.85f * diff * diffuseWeight + diffusion;
          // The coat lobe's Fresnel F0 is tinted by coat_color, matching
          // raytrace.comp (`coatF0 = coatColor * coatDielectric`). A scalar
          // coat term made a coat_color route or texture invisible here.
          outCol[0] = (base[0] * lit + specColor[0] * specular +
                       sheenColor[0] * sheen + coatColor[0] * coat +
                       emission[0]) * exposureMul;
          outCol[1] = (base[1] * lit + specColor[1] * specular +
                       sheenColor[1] * sheen + coatColor[1] * coat +
                       emission[1]) * exposureMul;
          outCol[2] = (base[2] * lit + specColor[2] * specular +
                       sheenColor[2] * sheen + coatColor[2] * coat +
                       emission[2]) * exposureMul;
          // Dome diffuse irradiance and view-dependent reflection make CPU RT
          // a useful visual reference for the raster and ray-query paths. SH
          // supplies the low-frequency diffuse term; the authored dome texture
          // supplies a sharp reflection, attenuated as roughness increases.
          const auto domeDiffuse = cpuDomeIrradiance(nrm);
          float viewDir[3] = {(camPos[0] - hitP[0]) / std::max(viewLen, 1.0e-4f),
                              (camPos[1] - hitP[1]) / std::max(viewLen, 1.0e-4f),
                              (camPos[2] - hitP[2]) / std::max(viewLen, 1.0e-4f)};
          const float vn = viewDir[0] * nrm[0] + viewDir[1] * nrm[1] +
                           viewDir[2] * nrm[2];
          float reflected[3] = {-viewDir[0] + 2.0f * vn * nrm[0],
                                -viewDir[1] + 2.0f * vn * nrm[1],
                                -viewDir[2] + 2.0f * vn * nrm[2]};
          Normalize3(reflected);
          const auto domeSpecular = cpuEnvironment(reflected);
          const float envSpecWeight = specWeight *
              (fresnel * (1.0f - metallic) + metallic) *
              (1.0f - 0.75f * roughness);
          for (int c = 0; c < 3; ++c) {
            outCol[c] += exposureMul * occlusion *
                (base[c] * diffuseWeight * domeDiffuse[c] * 0.31830988618f +
                 specColor[c] * domeSpecular[c] * envSpecWeight);
          }
          if (renderMode == 0) {
            const int mid = (tri < cpuMat_.size()) ? cpuMat_[tri] : -1;
            float alphaMode = 0.0f;
            if (mid >= 0 && static_cast<size_t>(mid) * kLightRtOpenPBRFloats + 54 <
                               hs_.matLightRt.size())
              alphaMode = hs_.matLightRt[static_cast<size_t>(mid) *
                                         kLightRtOpenPBRFloats + 54];
            const float topAlpha =
                cpuLayerAlpha(static_cast<int>(tri), hitInst, hit.u, hit.v);
            if (alphaMode > 0.5f && alphaMode < 1.5f && topAlpha <= 0.0f) {
              outCol[0] = clearColor[0]; outCol[1] = clearColor[1];
              outCol[2] = clearColor[2];
            } else if (alphaMode > 1.5f && topAlpha < 1.0f) {
              const float topColor[3] = {outCol[0], outCol[1], outCol[2]};
              const auto refracted = cpuLayerRefracted(
                  static_cast<int>(tri), hitInst, hit.u, hit.v, rayOrg, dir);
              const auto topTint =
                  cpuLayerTransmission(static_cast<int>(tri), 0.0f);
              const float topTransmission = cpuLayerTransmissionWeight(
                  static_cast<int>(tri), hitInst, hit.u, hit.v);
              float underAccum[3] = {0.0f, 0.0f, 0.0f};
              float throughput[3] = {1.0f, 1.0f, 1.0f};
              // Continue the underlayer walk from the last refracted interface.
              // This preserves the changed direction through stacked glass while
              // retaining the original camera ray for non-transmissive materials.
              float walkOrigin[3] = {rayOrg[0], rayOrg[1], rayOrg[2]};
              float walkDir[3] = {dir[0], dir[1], dir[2]};
              float cursor = hit.t + 1.0e-4f;
              if (topTransmission > 0.0f) {
                if (refracted.hasContinuation) {
                  std::copy(refracted.continuationOrigin.begin(),
                            refracted.continuationOrigin.end(), walkOrigin);
                  std::copy(refracted.continuationDir.begin(),
                            refracted.continuationDir.end(), walkDir);
                }
                cursor = 1.0e-4f;
              }
              for (int layer = 0; layer < 7 &&
                   (throughput[0] + throughput[1] + throughput[2]) > 0.009f;
                   ++layer) {
                lrt_ray nextRay{};
                nextRay.org[0] = walkOrigin[0]; nextRay.org[1] = walkOrigin[1];
                nextRay.org[2] = walkOrigin[2];
                nextRay.dir[0] = walkDir[0]; nextRay.dir[1] = walkDir[1];
                nextRay.dir[2] = walkDir[2];
                nextRay.tmin = cursor; nextRay.tmax = 1.0e30f;
                float nextT = 0.0f, nextU = 0.0f, nextV = 0.0f;
                int nextInst = -1, nextSrc = -1;
                if (!Intersect(nextRay, &nextT, &nextU, &nextV, &nextInst,
                               &nextSrc))
                  break;
                const int nextTri = nextSrc;
                const float layerAlpha =
                    cpuLayerAlpha(nextTri, nextInst, nextU, nextV);
                const auto layerColor =
                    cpuLayerColor(nextTri, nextInst, nextU, nextV);
                underAccum[0] += layerColor[0] * throughput[0] * layerAlpha;
                underAccum[1] += layerColor[1] * throughput[1] * layerAlpha;
                underAccum[2] += layerColor[2] * throughput[2] * layerAlpha;
                const auto layerTint = cpuLayerTransmission(nextTri, 0.0f);
                for (int c = 0; c < 3; ++c)
                  throughput[c] *= layerTint[c] * (1.0f - layerAlpha);
                // Intermediate transparent interfaces can change the
                // direction of the remaining walk.  Propagate that bounded
                // refraction instead of continuing every layer along the
                // previous ray; opaque/alpha-only layers remain straight.
                if (cpuLayerTransmissionWeight(nextTri, nextInst, nextU,
                                               nextV) > 0.0f) {
                  const auto layerRefracted = cpuLayerRefracted(
                      nextTri, nextInst, nextU, nextV, walkOrigin, walkDir);
                  std::copy(layerRefracted.origin.begin(),
                            layerRefracted.origin.end(), walkOrigin);
                  std::copy(layerRefracted.dir.begin(),
                            layerRefracted.dir.end(), walkDir);
                } else {
                  for (int c = 0; c < 3; ++c)
                    walkOrigin[c] += walkDir[c] * nextT + walkDir[c] * 1.0e-4f;
                }
                cursor = 1.0e-4f;
                if (layerAlpha >= 0.999f) break;
              }
              for (int c = 0; c < 3; ++c) {
                const float straightUnder = underAccum[c] +
                    clearColor[c] * throughput[c];
                const float straightTransmitted = straightUnder * topTint[c];
                const float transmitted = straightTransmitted *
                    (1.0f - topTransmission) +
                    refracted.color[c] * topTransmission;
                outCol[c] = topColor[c] * topAlpha +
                            transmitted * (1.0f - topAlpha);
              }
            }
          }
        }
        // Shading is accumulated in linear light; encode only the final color
        // for the RGBA8 viewport. Previously CPU RT wrote linear values straight
        // to UNORM, making it systematically darker than both Vulkan paths.
        auto linearToSrgb = [](float value) {
          value = std::max(value, 0.0f);
          value = std::clamp(
              value * (2.51f * value + 0.03f) /
                  (value * (2.43f * value + 0.59f) + 0.14f),
              0.0f, 1.0f);
          return value <= 0.0031308f ? value * 12.92f
                                    : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        };
        const bool encodeSrgb = renderMode == 0;
        px[0] = static_cast<uint8_t>(std::clamp(
            (encodeSrgb ? linearToSrgb(outCol[0]) : outCol[0]) * 255.0f,
            0.0f, 255.0f));
        px[1] = static_cast<uint8_t>(std::clamp(
            (encodeSrgb ? linearToSrgb(outCol[1]) : outCol[1]) * 255.0f,
            0.0f, 255.0f));
        px[2] = static_cast<uint8_t>(std::clamp(
            (encodeSrgb ? linearToSrgb(outCol[2]) : outCol[2]) * 255.0f,
            0.0f, 255.0f));
        px[3] = 255;
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(nThreads > 1 ? nThreads - 1 : 0);
  for (unsigned i = 1; i < nThreads; ++i) pool.emplace_back(worker);
  worker();
  for (auto& t : pool) t.join();
  if (probe_enabled) {
    std::FILE* out = std::fopen(probe_path, "w");
    if (out) {
      for (size_t i = 0; i < ptexAsset_.size(); ++i) {
        if (!ptex_[i]) continue;
        std::fprintf(out, "# texture %zu %s\n", i,
                     ptexAsset_[i].empty() ? "<memory>" : ptexAsset_[i].c_str());
      }
      std::fprintf(out, "# x y tex face mip srgb u v r g b\n");
      for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
          const CpuPtexProbe& probe =
              probes[static_cast<size_t>(py) * static_cast<size_t>(w) +
                     static_cast<size_t>(px)];
          if (probe.face == ~0u) continue;
          std::fprintf(out, "%d %d %u %u %d %d %.9g %.9g %.9g %.9g %.9g\n",
                       px, py, probe.tex, probe.face, probe.mip, probe.srgb,
                       probe.u, probe.v, probe.rgb[0], probe.rgb[1],
                       probe.rgb[2]);
        }
      }
      std::fclose(out);
    }
  }  return true;
}

}  // namespace lusdview
