// SPDX-License-Identifier: Apache-2.0
// lusdview — shared host-side scene build (see rt_scene_build.hh).
#include "rt_scene_build.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "displacement_bake.hh"  // SampleTextureRed
#include "lighting_ies.hh"
#include "lightrt_mtlx_bridge.hh"
#include "ptex_atlas.hh"
#include "log.hh"
#include "texture_tools.hh"

namespace lusdview {

namespace {

// BC7 is allowed to quantize the alpha channel, but the raster Ptex path keeps
// rectangle metadata outside the compressed image. Reconstruct the canonical
// metadata texels before putting a compressed Ptex atlas into the raw RT SSBO.
// This keeps Vulkan ray-query/compute RT on the same coarse Ptex fallback atlas
// without requiring the RT shader to decode BC blocks itself.
void RestorePtexRectMetadata(light3d::Image* image,
                             const DrawTextureCPU& tex) {
  if (!image || !tex.isPtex || tex.ptexFaceRects.empty() ||
      image->width <= 0 || image->height <= 0) return;
  const size_t texelCount = static_cast<size_t>(image->width) *
                            static_cast<size_t>(image->height);
  const size_t first = tex.ptexRectTexelOffset;
  const size_t count = tex.ptexFaceRects.size() * 8u;
  if (first > texelCount || count > texelCount - first) return;
  for (uint32_t face = 0;
       face < static_cast<uint32_t>(tex.ptexFaceRects.size()); ++face) {
    uint8_t encoded[8u * 4u];
    EncodePtexFaceRectTexels(tex.ptexFaceRects[face], encoded);
    for (size_t byte = 0; byte < 8u; ++byte) {
      const size_t linear = first + static_cast<size_t>(face) * 8u + byte;
      image->data[linear * 4u + 3u] = encoded[byte * 4u + 3u];
    }
  }
}

// column-major mat4 (DrawMeshCPU.world) -> row-major 3x4 o2w (instance format).
inline void Mat4ToO2W(const float m[16], float o2w[12]) {
  o2w[0] = m[0]; o2w[1] = m[4]; o2w[2] = m[8];  o2w[3] = m[12];
  o2w[4] = m[1]; o2w[5] = m[5]; o2w[6] = m[9];  o2w[7] = m[13];
  o2w[8] = m[2]; o2w[9] = m[6]; o2w[10] = m[10]; o2w[11] = m[14];
}
// Affine inverse of a row-major 3x4 o2w; identity fallback when near-singular.
inline bool Affine3x4Inverse(const float o2w[12], float w2o[12]) {
  const float a = o2w[0], b = o2w[1], c = o2w[2];
  const float d = o2w[4], e = o2w[5], f = o2w[6];
  const float g = o2w[8], h = o2w[9], i = o2w[10];
  const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-20f) {
    for (int k = 0; k < 12; ++k) w2o[k] = 0.0f;
    w2o[0] = w2o[5] = w2o[10] = 1.0f;
    return false;
  }
  const float inv = 1.0f / det;
  const float r00 = A * inv, r01 = (c * h - b * i) * inv, r02 = (b * f - c * e) * inv;
  const float r10 = B * inv, r11 = (a * i - c * g) * inv, r12 = (c * d - a * f) * inv;
  const float r20 = C * inv, r21 = (b * g - a * h) * inv, r22 = (a * e - b * d) * inv;
  const float tx = o2w[3], ty = o2w[7], tz = o2w[11];
  w2o[0] = r00; w2o[1] = r01; w2o[2] = r02; w2o[3] = -(r00 * tx + r01 * ty + r02 * tz);
  w2o[4] = r10; w2o[5] = r11; w2o[6] = r12; w2o[7] = -(r10 * tx + r11 * ty + r12 * tz);
  w2o[8] = r20; w2o[9] = r21; w2o[10] = r22; w2o[11] = -(r20 * tx + r21 * ty + r22 * tz);
  return true;
}
inline void O2WPt(const float o2w[12], float x, float y, float z, float o[3]) {
  o[0] = o2w[0] * x + o2w[1] * y + o2w[2] * z + o2w[3];
  o[1] = o2w[4] * x + o2w[5] * y + o2w[6] * z + o2w[7];
  o[2] = o2w[8] * x + o2w[9] * y + o2w[10] * z + o2w[11];
}
// World AABB = transform the 8 corners of a local AABB by a row-major 3x4 o2w.
inline void O2WAabb(const float o2w[12], const float lo[3], const float hi[3],
                    float wlo[3], float whi[3]) {
  for (int k = 0; k < 3; ++k) { wlo[k] = 1e30f; whi[k] = -1e30f; }
  for (int c = 0; c < 8; ++c) {
    float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1],
                  (c & 4) ? hi[2] : lo[2]};
    float w[3];
    O2WPt(o2w, p[0], p[1], p[2], w);
    for (int k = 0; k < 3; ++k) { wlo[k] = std::min(wlo[k], w[k]); whi[k] = std::max(whi[k], w[k]); }
  }
}

unsigned RtBuildWorkerLimit();

// Run f(i) for i in [0,n) across hardware threads (serial for small n).
template <class F>
void ParallelFor(size_t n, F f) {
  unsigned nt = std::max(1u, std::thread::hardware_concurrency());
  if (n < 64 || nt <= 1) { for (size_t i = 0; i < n; ++i) f(i); return; }
  nt = std::min(nt, RtBuildWorkerLimit());
  nt = static_cast<unsigned>(std::min<size_t>(nt, n));
  const size_t chunk = (n + nt - 1) / nt;
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < nt; ++t) {
    const size_t b = size_t(t) * chunk, e = std::min(n, b + chunk);
    if (b >= e) break;
    ts.emplace_back([&f, b, e] { for (size_t i = b; i < e; ++i) f(i); });
  }
  for (auto& th : ts) th.join();
}

size_t GaussianBvhChunkLimit() {
  constexpr size_t kDefault = size_t(262) * 1024;
  const char* value = std::getenv("LUSDVIEW_GAUSSIAN_CHUNK");
  if (!value || !*value) return kDefault;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || (end && *end != '\0') || parsed == 0)
    return kDefault;
  return std::min<size_t>(static_cast<size_t>(parsed),
                          static_cast<size_t>(std::numeric_limits<int>::max()));
}

unsigned RtBuildWorkerLimit() {
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  constexpr unsigned kDefault = 8;
  const char* value = std::getenv("LUSDVIEW_RT_BUILD_THREADS");
  if (!value || !*value)
    return std::min(hardware, kDefault);
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || (end && *end != '\0') || parsed == 0)
    return std::min(hardware, kDefault);
  return std::min(hardware, static_cast<unsigned>(
      std::min<unsigned long>(parsed, std::numeric_limits<unsigned>::max())));
}

template <typename T>
void AppendMove(std::vector<T>& dst, std::vector<T>& src) {
  dst.insert(dst.end(), std::make_move_iterator(src.begin()),
             std::make_move_iterator(src.end()));
  std::vector<T>().swap(src);
}

// Per-mesh build result (leaf-order geometry + a LOCAL-ref BLAS + placements).
struct MeshBuild {
  bool valid = false;
  std::vector<float> tris, nrms, cols, uv, uv1, infl, domw;
  std::vector<HostGeomProp> geomProps;
  std::vector<uint8_t> geo, emask;
  std::vector<int> mat, backMat, face, domj;
  std::vector<Node> blas;  // local node/leaf refs (rebased during assembly)
  std::vector<int> leafOrder;  // output slot i <- original triangle leafOrder[i]
  float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  std::vector<float> instO2W;   // 12 floats/placement (1 for non-instanced)
  std::vector<float> instTint;  // 4 floats/placement (rgba)
};

// Flatten one mesh into local-space leaf-order triangle SoA + per-prototype BLAS.
// Pure (reads scene/mesh only), so it runs in parallel across meshes.
MeshBuild BuildOneMesh(const DrawScene& scene, const DrawMeshCPU& m,
                       float displacementScale) {
  MeshBuild mb;
  if (m.vertices.empty() || m.indices.empty()) return mb;
  const bool instanced = m.instanceCount() > 0;
  const bool hasVtxCol = m.vertexColors.size() == m.vertices.size() * 3;
  const bool hasVtxAlpha = m.vertexAlpha.size() == m.vertices.size();
  const bool hasUV1 = m.uv1.size() == m.vertices.size() * 2;
  const bool hasInfl = m.morphInfluence.size() == m.vertices.size();
  const bool hasSkin = m.jointIdx.size() == m.vertices.size() * 4 &&
                       m.jointWt.size() == m.vertices.size() * 4;
  const bool hasFace = m.sourceFaceId.size() == m.indices.size() / 3;
  const uint8_t g = static_cast<uint8_t>((m.geometricNormal ? 1 : 0) |
                                         ((PurposeId(m.purpose) & 3) << 1) |
                                         ((m.kindId & 7) << 3));
  auto submeshMat = [&](uint32_t triIdx0) -> const DrawMaterialCPU* {
    for (const DrawSubmesh& s : m.submeshes)
      if (triIdx0 >= s.indexOffset && triIdx0 < s.indexOffset + s.indexCount)
        return (s.materialId >= 0 && size_t(s.materialId) < scene.materials.size())
                   ? &scene.materials[s.materialId] : nullptr;
    return nullptr;
  };
  auto submeshMatId = [&](uint32_t triIdx0) -> int {
    for (const DrawSubmesh& s : m.submeshes)
      if (triIdx0 >= s.indexOffset && triIdx0 < s.indexOffset + s.indexCount)
        return s.materialId;
    return -1;
  };
  auto submeshBackMatId = [&](uint32_t triIdx0) -> int {
    for (const DrawSubmesh& s : m.submeshes)
      if (triIdx0 >= s.indexOffset && triIdx0 < s.indexOffset + s.indexCount)
        return s.backfaceMaterialId;
    return -1;
  };
  auto isDefaultPlaceholder = [&](int materialId) -> bool {
    if (materialId != 0 || scene.materials.empty()) return false;
    const DrawMaterialCPU& mat = scene.materials.front();
    return mat.name.empty() && mat.absPath.empty() &&
           !mat.hasUsdPreviewSurface && !mat.hasOpenPBRSurface &&
           mat.params.empty();
  };

  // Original-polygon edge set (for the wireframe edge mask). Keys are the same
  // vertex-index pairs used by m.indices, so triangle edges can be tested directly.
  const bool hasWire = !m.wireframeIndices.empty();
  std::unordered_set<uint64_t> wireSet;
  auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
    if (a > b) { uint32_t s = a; a = b; b = s; }
    return (static_cast<uint64_t>(a) << 32) | b;
  };
  if (hasWire) {
    wireSet.reserve(m.wireframeIndices.size());
    for (size_t e = 0; e + 1 < m.wireframeIndices.size(); e += 2)
      wireSet.insert(edgeKey(m.wireframeIndices[e], m.wireframeIndices[e + 1]));
  }

  std::vector<float> lt, ln, lc, luv, luv1, linfl, ldomw;
  std::vector<uint8_t> lg, le;
  std::vector<int> lm, lmb, lf, ldomj;
  bool anyBackMaterial = false;
  mb.geomProps.reserve(m.geomProps.size());
  for (const DrawGeomPropCPU& src : m.geomProps) {
    HostGeomProp& dst = mb.geomProps.emplace_back();
    dst.name = src.name;
    dst.components = src.components;
    dst.values.reserve((m.indices.size() / 3) * 3 * dst.components);
  }
  // Most next-loader meshes are world-baked at load time (m.world == identity,
  // the transform already folded into `vertices`), but `animatedWorld` meshes
  // (see gpu_scene.hh) keep a REAL per-frame world matrix on top of a fixed
  // local-space vertex buffer -- the raster GPU vertex shader always applies
  // it, so RT/CUDA/HIP/CPU tracers (which read this flattened copy, not the
  // GPU buffers) must too, or an animated-world mesh renders at its stale
  // local-space position/orientation instead of its actual posed placement.
  const float* W = m.world;
  const bool worldIsIdentity =
      W[0] == 1.0f && W[1] == 0.0f && W[2] == 0.0f && W[3] == 0.0f &&
      W[4] == 0.0f && W[5] == 1.0f && W[6] == 0.0f && W[7] == 0.0f &&
      W[8] == 0.0f && W[9] == 0.0f && W[10] == 1.0f && W[11] == 0.0f &&
      W[12] == 0.0f && W[13] == 0.0f && W[14] == 0.0f && W[15] == 1.0f;
  float worldO2W[12]{};
  float worldW2O[12]{};
  const bool worldNormalInvertible = !worldIsIdentity &&
      (Mat4ToO2W(W, worldO2W), Affine3x4Inverse(worldO2W, worldW2O));
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    float wp[9], wn[9], wc[12], wuv[6], wuv1[6], winfl[3], wdomw[3];
    int domJoint = -1;
    for (int k = 0; k < 3; ++k) {
      const uint32_t vidx = m.indices[t + k];
      const DrawVertex& vtx = m.vertices[vidx];
      if (worldIsIdentity) {
        wp[k * 3 + 0] = vtx.px; wp[k * 3 + 1] = vtx.py; wp[k * 3 + 2] = vtx.pz;
        wn[k * 3 + 0] = vtx.nx; wn[k * 3 + 1] = vtx.ny; wn[k * 3 + 2] = vtx.nz;
      } else {
        wp[k * 3 + 0] = W[0] * vtx.px + W[4] * vtx.py + W[8] * vtx.pz + W[12];
        wp[k * 3 + 1] = W[1] * vtx.px + W[5] * vtx.py + W[9] * vtx.pz + W[13];
        wp[k * 3 + 2] = W[2] * vtx.px + W[6] * vtx.py + W[10] * vtx.pz + W[14];
        if (worldNormalInvertible) {
          // Normals transform by the inverse transpose of the object-to-world
          // linear transform. `worldW2O` is row-major, so its columns provide
          // the inverse-transpose rows used here.
          wn[k * 3 + 0] = worldW2O[0] * vtx.nx + worldW2O[4] * vtx.ny +
                          worldW2O[8] * vtx.nz;
          wn[k * 3 + 1] = worldW2O[1] * vtx.nx + worldW2O[5] * vtx.ny +
                          worldW2O[9] * vtx.nz;
          wn[k * 3 + 2] = worldW2O[2] * vtx.nx + worldW2O[6] * vtx.ny +
                          worldW2O[10] * vtx.nz;
        } else {
          // Preserve the previous linear fallback for singular transforms.
          wn[k * 3 + 0] = W[0] * vtx.nx + W[4] * vtx.ny + W[8] * vtx.nz;
          wn[k * 3 + 1] = W[1] * vtx.nx + W[5] * vtx.ny + W[9] * vtx.nz;
          wn[k * 3 + 2] = W[2] * vtx.nx + W[6] * vtx.ny + W[10] * vtx.nz;
        }
      }
      wuv[k * 2 + 0] = vtx.u; wuv[k * 2 + 1] = vtx.v;
      wuv1[k * 2 + 0] = hasUV1 ? m.uv1[vidx * 2 + 0] : 0.0f;
      wuv1[k * 2 + 1] = hasUV1 ? m.uv1[vidx * 2 + 1] : 0.0f;
      winfl[k] = hasInfl ? m.morphInfluence[vidx] : 0.0f;
      int dj = -1; float dw = 0.0f;
      if (hasSkin)
        for (int b = 0; b < 4; ++b) {
          const float wb = m.jointWt[vidx * 4 + b];
          if (wb > dw) { dw = wb; dj = static_cast<int>(m.jointIdx[vidx * 4 + b]); }
        }
      wdomw[k] = dw;
      if (k == 0) domJoint = dj;
      float dc[3] = {1, 1, 1};
      if (hasVtxCol) {
        const float* c = &m.vertexColors[vidx * 3];
        dc[0] = c[0]; dc[1] = c[1]; dc[2] = c[2];
      }
      // Keep only displayColor here. The hit-selected front/back material base
      // is applied in the kernel from matBase, so changing sides can change the
      // base constant without a second per-triangle color stream.
      wc[k * 4 + 0] = dc[0];
      wc[k * 4 + 1] = dc[1];
      wc[k * 4 + 2] = dc[2];
      wc[k * 4 + 3] = hasVtxAlpha ? m.vertexAlpha[vidx] : 1.0f;
    }
    if (displacementScale != 0.0f) {
      const DrawMaterialCPU* dmat = submeshMat(static_cast<uint32_t>(t));
      if (dmat && dmat->hasDisplacement()) {
        for (int k = 0; k < 3; ++k) {
          float h = dmat->displacementTex >= 0
                        ? SampleTextureRed(
                              scene, dmat->displacementTex, wuv[k * 2],
                              wuv[k * 2 + 1],
                              hasFace ? m.sourceFaceId[t / 3] : UINT32_MAX) *
                                  dmat->displacementTexScale +
                              dmat->displacementTexBias
                        : dmat->displacementConst;
          h *= displacementScale;
          float nx = wn[k * 3], ny = wn[k * 3 + 1], nz = wn[k * 3 + 2];
          const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
          if (nl > 1e-12f) {
            nx /= nl; ny /= nl; nz /= nl;
            wp[k * 3 + 0] += nx * h; wp[k * 3 + 1] += ny * h; wp[k * 3 + 2] += nz * h;
          }
        }
        const float e1[3] = {wp[3] - wp[0], wp[4] - wp[1], wp[5] - wp[2]};
        const float e2[3] = {wp[6] - wp[0], wp[7] - wp[1], wp[8] - wp[2]};
        const float gn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]};
        const float gl = std::sqrt(gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2]);
        if (gl > 1e-12f)
          for (int k = 0; k < 3; ++k) {
            wn[k * 3 + 0] = gn[0] / gl; wn[k * 3 + 1] = gn[1] / gl; wn[k * 3 + 2] = gn[2] / gl;
          }
      }
    }
    lt.insert(lt.end(), wp, wp + 9);
    ln.insert(ln.end(), wn, wn + 9);
    lc.insert(lc.end(), wc, wc + 12);
    luv.insert(luv.end(), wuv, wuv + 6);
    luv1.insert(luv1.end(), wuv1, wuv1 + 6);
    linfl.insert(linfl.end(), winfl, winfl + 3);
    ldomw.insert(ldomw.end(), wdomw, wdomw + 3);
    ldomj.push_back(domJoint);
    for (size_t pi = 0; pi < mb.geomProps.size(); ++pi) {
      const DrawGeomPropCPU& src = m.geomProps[pi];
      for (int k = 0; k < 3; ++k) {
        const size_t valueBegin = size_t(m.indices[t + k]) * src.components;
        if (src.components == 0 ||
            valueBegin + src.components > src.values.size()) {
          mb.geomProps[pi].values.insert(mb.geomProps[pi].values.end(),
                                         src.components, 0.0f);
        } else {
          mb.geomProps[pi].values.insert(
              mb.geomProps[pi].values.end(), src.values.begin() + valueBegin,
              src.values.begin() + valueBegin + src.components);
        }
      }
    }
    lg.push_back(g);
    const int frontMat = submeshMatId(static_cast<uint32_t>(t));
    // Unbound next-loader instanced prototypes carry their flat/instance tint
    // in Inst; suppress that loader's anonymous material-0 placeholder to avoid
    // multiplying by default gray twice. Legacy scenes may use index 0 for a
    // real authored material, identified by its populated material record.
    lm.push_back(instanced &&
                         (frontMat < 0 || isDefaultPlaceholder(frontMat))
                     ? -1
                     : frontMat);
    const int backMat = submeshBackMatId(static_cast<uint32_t>(t));
    lmb.push_back(backMat);
    anyBackMaterial |= backMat >= 0 && backMat != lm.back();
    lf.push_back(hasFace ? static_cast<int>(m.sourceFaceId[t / 3]) : -1);
    // Wireframe edge mask: bit0 edge(v1,v2), bit1 edge(v2,v0), bit2 edge(v0,v1).
    // 7 (all edges) when the mesh has no original-polygon data.
    if (hasWire) {
      const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
      uint8_t em = 0;
      if (wireSet.count(edgeKey(b, c))) em |= 1u;
      if (wireSet.count(edgeKey(c, a))) em |= 2u;
      if (wireSet.count(edgeKey(a, b))) em |= 4u;
      le.push_back(em);
    } else {
      le.push_back(7u);
    }
  }
  const size_t ltc = lt.size() / 9;
  if (ltc == 0) return mb;

  mb.lo[0] = mb.hi[0] = lt[0]; mb.lo[1] = mb.hi[1] = lt[1]; mb.lo[2] = mb.hi[2] = lt[2];
  for (size_t i = 0; i < ltc; ++i)
    for (int v = 0; v < 3; ++v)
      for (int k = 0; k < 3; ++k) {
        float c = lt[i * 9 + v * 3 + k];
        mb.lo[k] = std::min(mb.lo[k], c); mb.hi[k] = std::max(mb.hi[k], c);
      }

  std::vector<float> cent(ltc * 3);
  for (size_t i = 0; i < ltc; ++i) {
    const float* tv = &lt[i * 9];
    for (int k = 0; k < 3; ++k) cent[i * 3 + k] = (tv[k] + tv[3 + k] + tv[6 + k]) / 3.f;
  }
  std::vector<int> bidx(ltc);
  for (size_t i = 0; i < ltc; ++i) bidx[i] = static_cast<int>(i);
  mb.blas.reserve(ltc * 2);
  BuildBvh(mb.blas, bidx, 0, static_cast<int>(ltc), cent, lt);

  // Emit per-tri arrays in BVH leaf order (so the local blas leaf refs are valid).
  mb.tris.reserve(ltc * 9); mb.nrms.reserve(ltc * 9); mb.cols.reserve(ltc * 12);
  mb.uv.reserve(ltc * 6); mb.uv1.reserve(ltc * 6); mb.infl.reserve(ltc * 3);
  mb.domw.reserve(ltc * 3); mb.geo.reserve(ltc); mb.mat.reserve(ltc);
  if (anyBackMaterial) mb.backMat.reserve(ltc);
  mb.face.reserve(ltc); mb.domj.reserve(ltc); mb.emask.reserve(ltc);
  for (size_t i = 0; i < ltc; ++i) {
    int s = bidx[i];
    mb.tris.insert(mb.tris.end(), &lt[s * 9], &lt[s * 9] + 9);
    mb.nrms.insert(mb.nrms.end(), &ln[s * 9], &ln[s * 9] + 9);
    mb.cols.insert(mb.cols.end(), &lc[s * 12], &lc[s * 12] + 12);
    mb.uv.insert(mb.uv.end(), &luv[s * 6], &luv[s * 6] + 6);
    mb.uv1.insert(mb.uv1.end(), &luv1[s * 6], &luv1[s * 6] + 6);
    mb.infl.insert(mb.infl.end(), &linfl[s * 3], &linfl[s * 3] + 3);
    mb.domw.insert(mb.domw.end(), &ldomw[s * 3], &ldomw[s * 3] + 3);
    mb.geo.push_back(lg[s]);
    mb.emask.push_back(le[s]);
    mb.mat.push_back(lm[s]);
    if (anyBackMaterial) mb.backMat.push_back(lmb[s]);
    mb.face.push_back(lf[s]);
    mb.domj.push_back(ldomj[s]);
  }
  for (size_t pi = 0; pi < mb.geomProps.size(); ++pi) {
    std::vector<float> reordered;
    reordered.reserve(mb.geomProps[pi].values.size());
    const uint32_t comps = mb.geomProps[pi].components;
    for (int source : bidx) {
      const size_t begin = size_t(source) * 3 * comps;
      reordered.insert(reordered.end(), mb.geomProps[pi].values.begin() + begin,
                       mb.geomProps[pi].values.begin() + begin + 3 * comps);
    }
    mb.geomProps[pi].values = std::move(reordered);
  }
  mb.leafOrder = std::move(bidx);

  // Placements (instanced: N; non-instanced: one identity-style placement).
  if (instanced) {
    const size_t ninst = m.instanceCount();
    const bool perColor = m.instanceColors.size() == ninst * 3;
    const bool perOpacity = m.instanceOpacities.size() == ninst;
    const bool boundMaterial = std::any_of(
        mb.mat.begin(), mb.mat.end(), [](int materialId) { return materialId >= 0; });
    mb.instO2W.reserve(ninst * 12);
    mb.instTint.reserve(ninst * 4);
    for (size_t k = 0; k < ninst; ++k) {
      mb.instO2W.insert(mb.instO2W.end(), &m.instanceXforms[k * 12], &m.instanceXforms[k * 12] + 12);
      mb.instTint.push_back(perColor ? m.instanceColors[k * 3 + 0]
                                    : (boundMaterial ? 1.0f : m.flatColor[0]));
      mb.instTint.push_back(perColor ? m.instanceColors[k * 3 + 1]
                                    : (boundMaterial ? 1.0f : m.flatColor[1]));
      mb.instTint.push_back(perColor ? m.instanceColors[k * 3 + 2]
                                    : (boundMaterial ? 1.0f : m.flatColor[2]));
      const float opacity = perOpacity ? m.instanceOpacities[k] : m.flatOpacity;
      mb.instTint.push_back(std::max(0.0f, std::min(1.0f, opacity)));
    }
  } else {
    const float o2w[12] = {1.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f};
    mb.instO2W.insert(mb.instO2W.end(), o2w, o2w + 12);
    mb.instTint.push_back(1.0f); mb.instTint.push_back(1.0f);
    mb.instTint.push_back(1.0f); mb.instTint.push_back(1.0f);
  }
  mb.valid = true;
  return mb;
}

}  // namespace

static float RtSrgbToLinear(float value) {
  return value <= 0.04045f ? value / 12.92f
                           : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static void CachePackedDomeTextureProposal(const HostScene& scene, float* p) {
  if (!p) return;
  const int textureId = static_cast<int>(p[2] + 0.5f);
  if (textureId < 0 || static_cast<size_t>(textureId) >= scene.textures.size())
    return;
  const HostTextureDesc& td = scene.textures[static_cast<size_t>(textureId)];
  if (td.width < 1 || td.height < 1 || td.offset < 0) return;
  const size_t required = static_cast<size_t>(td.offset) +
      static_cast<size_t>(td.width) * static_cast<size_t>(td.height) * 4u;
  if (required > scene.texels.size()) return;
  const bool rgbe = (static_cast<int>(p[1] + 0.5f) & (1 << 8)) != 0;
  auto texel = [&](int x, int y, int channel) {
    x = (x % td.width + td.width) % td.width;
    y = std::max(0, std::min(td.height - 1, y));
    const uint8_t* q = &scene.texels[static_cast<size_t>(td.offset) +
        (static_cast<size_t>(y) * td.width + x) * 4u];
    if (rgbe) {
      if (q[3] == 0) return 0.0f;
      return (static_cast<float>(q[channel]) + 0.5f) *
             std::ldexp(1.0f, static_cast<int>(q[3]) - 136);
    }
    float value = static_cast<float>(q[channel]) / 255.0f;
    return td.srgb ? RtSrgbToLinear(value) : value;
  };
  float total = 0.0f;
  float maximum = 0.0f;
  for (int cell = 0; cell < 32; ++cell) {
    const int zCell = cell / 8;
    const int phiCell = cell - zCell * 8;
    const float z = -1.0f + 2.0f * (static_cast<float>(zCell) + 0.5f) / 4.0f;
    const float phi = 6.28318530718f *
                      (static_cast<float>(phiCell) + 0.5f) / 8.0f;
    const float radial = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float dx = radial * std::cos(phi);
    const float dy = radial * std::sin(phi);
    const float dz = z;
    float ex = p[40] * dx + p[41] * dy + p[42] * dz;
    float ey = p[44] * dx + p[45] * dy + p[46] * dz;
    float ez = p[48] * dx + p[49] * dy + p[50] * dz;
    const float len = std::sqrt(ex * ex + ey * ey + ez * ez);
    if (len > 1.0e-12f) { ex /= len; ey /= len; ez /= len; }
    float u = std::atan2(ex, -ez) * 0.15915494309f + 0.5f;
    float v = std::acos(std::max(-1.0f, std::min(1.0f, ey))) *
              0.31830988618f;
    const int x = std::min(td.width - 1,
        std::max(0, static_cast<int>(u * static_cast<float>(td.width))));
    const int y = std::min(td.height - 1,
        std::max(0, static_cast<int>(v * static_cast<float>(td.height))));
    const float r = texel(x, y, 0) * p[16];
    const float g = texel(x, y, 1) * p[17];
    const float b = texel(x, y, 2) * p[18];
    const float weight = std::max(1.0e-6f,
        0.2126f * r + 0.7152f * g + 0.0722f * b);
    total += weight;
    maximum = std::max(maximum, weight);
  }
  p[51] = std::max(maximum, 1.0e-6f);
  p[79] = std::max(total, 1.0e-20f);
}

void PackRtLightParams(const DrawLightCPU& light, int mappedEnvmapTexture,
                       float* p) {
  if (!p) return;
  std::fill(p, p + kRtLightParamFloats, 0.0f);
  int flags = 0;
  if (light.normalize) flags |= 1 << 0;
  if (light.shadowEnable) flags |= 1 << 1;
  if (light.hasShaping) flags |= 1 << 2;
  if (light.hasSpectralEmission) flags |= 1 << 3;
  if (mappedEnvmapTexture >= 0) flags |= 1 << 4;
  if (light.ibl.valid) flags |= 1 << 7;  // baked IBL env cube (VK RT miss)
  if (light.lightLinksAll) flags |= 1 << 5;
  if (light.shadowLinksAll) flags |= 1 << 6;

  p[0] = static_cast<float>(light.type);
  p[1] = static_cast<float>(flags);
  p[2] = static_cast<float>(mappedEnvmapTexture);
  p[3] = static_cast<float>(light.geometryMesh);

  p[4] = light.position[0];
  p[5] = light.position[1];
  p[6] = light.position[2];
  p[7] = light.radius;

  p[8] = light.direction[0];
  p[9] = light.direction[1];
  p[10] = light.direction[2];
  p[11] = light.angle;

  p[12] = light.normalizedColor[0];
  p[13] = light.normalizedColor[1];
  p[14] = light.normalizedColor[2];
  p[15] = light.effectiveIntensity;

  p[16] = light.effectiveColor[0];
  p[17] = light.effectiveColor[1];
  p[18] = light.effectiveColor[2];
  p[19] = light.invArea;

  p[20] = light.width;
  p[21] = light.height;
  p[22] = light.length;
  p[23] = light.area;

  p[24] = light.shapingConeAngle;
  p[25] = light.shapingConeSoftness;
  p[26] = light.shapingFocus;
  p[27] = light.shapingIesAngleScale;

  p[28] = light.shadowEnable ? 1.0f : 0.0f;
  p[29] = light.diffuse;
  p[30] = light.specular;
  p[31] = light.exposure;

  p[32] = light.shadowColor[0];
  p[33] = light.shadowColor[1];
  p[34] = light.shadowColor[2];
  p[35] = light.shadowDistance;

  p[36] = light.shadowFalloff;
  p[37] = light.shadowFalloffGamma;
  p[38] = light.shapingIesNormalize ? 1.0f : 0.0f;
  p[39] = static_cast<float>(light.domeTextureFormat);

  // Rows 10-12: world -> environment rotation (row r = normalized column r of
  // the light transform; for a rigid transform that is R^T). Used by the RT
  // miss shaders to orient dome env sampling; identity when degenerate.
  p[40] = 1.0f; p[45] = 1.0f; p[50] = 1.0f;
  for (int r = 0; r < 3; ++r) {
    const float cx = light.transform[r * 4 + 0];
    const float cy = light.transform[r * 4 + 1];
    const float cz = light.transform[r * 4 + 2];
    const float len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (len > 1e-12f) {
      const float inv = 1.0f / len;
      p[40 + r * 4 + 0] = cx * inv;
      p[40 + r * 4 + 1] = cy * inv;
      p[40 + r * 4 + 2] = cz * inv;
    }
  }

  // Rows 13-19: order-2 SH irradiance (27 floats; zeros when no IBL bake).
  if (light.ibl.valid && light.ibl.shIrradiance.size() >= 27) {
    for (int i = 0; i < 27; ++i) {
      p[52 + i] = light.ibl.shIrradiance[static_cast<size_t>(i)];
    }
    // p[79] caches the normalization and p[51] the maximum cell weight of the
    // CUDA/HIP path tracer's 8x4 environment proposal. Both are invariant for
    // the packed dome; computing them here removes the per-path normalization
    // scan and enables exact low-cost rejection sampling.
    float total = 0.0f;
    float maxWeight = 0.0f;
    for (int cell = 0; cell < 32; ++cell) {
      const int zCell = cell / 8;
      const int phiCell = cell - zCell * 8;
      const float z = -1.0f + 2.0f * (float(zCell) + 0.5f) / 4.0f;
      const float phi = 6.28318530718f * (float(phiCell) + 0.5f) / 8.0f;
      const float radial = std::sqrt(std::max(0.0f, 1.0f - z * z));
      const float dx = radial * std::cos(phi);
      const float dy = radial * std::sin(phi);
      const float dz = z;
      const float ex = p[40] * dx + p[41] * dy + p[42] * dz;
      const float ey = p[44] * dx + p[45] * dy + p[46] * dz;
      const float ez = p[48] * dx + p[49] * dy + p[50] * dz;
      const float b[9] = {
          0.282095f, 0.488603f * ey, 0.488603f * ez, 0.488603f * ex,
          1.092548f * ex * ey, 1.092548f * ey * ez,
          0.315392f * (3.0f * ez * ez - 1.0f),
          1.092548f * ex * ez, 0.546274f * (ex * ex - ey * ey)};
      float rgb[3] = {0.0f, 0.0f, 0.0f};
      for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) rgb[c] += b[k] * p[52 + k * 3 + c];
      for (int c = 0; c < 3; ++c)
        rgb[c] = std::max(rgb[c], 0.0f) * p[16 + c];
      const float weight = std::max(0.2126f * rgb[0] + 0.7152f * rgb[1] +
                                        0.0722f * rgb[2],
                                    1.0e-6f);
      total += weight;
      maxWeight = std::max(maxWeight, weight);
    }
    // The fourth component of the final rotation row is padding. Cache the
    // rejection envelope there so GPU environment sampling need not rescan all
    // 32 SH cells on every bounce.
    p[51] = std::max(maxWeight, 1.0e-6f);
    p[79] = std::max(total, 1.0e-20f);
  } else if (light.iesValid) {
    // IES profiles use the same seven trailing vec4 rows as a compact 6x4
    // angular LUT. Dome lights retain the order-2 SH payload above; ordinary
    // lights never need that payload, so this avoids another descriptor.
    constexpr int kIesVerticalSamples = 4;
    constexpr int kIesHorizontalSamples = 6;
    p[52] = 2.0f;  // payload tag: IES LUT (SH payloads have no tag)
    p[53] = static_cast<float>(kIesVerticalSamples);
    p[54] = static_cast<float>(kIesHorizontalSamples);
    for (int y = 0; y < kIesVerticalSamples; ++y) {
      const float vertical = 180.0f * static_cast<float>(y) /
                             static_cast<float>(kIesVerticalSamples - 1);
      for (int x = 0; x < kIesHorizontalSamples; ++x) {
        const float horizontal = 360.0f * static_cast<float>(x) /
                                 static_cast<float>(kIesHorizontalSamples);
        p[56 + y * kIesHorizontalSamples + x] =
            EvaluateIesProfile(light, vertical, horizontal);
      }
    }
  } else if (light.geometryTriOffset >= 0 && light.geometryTriCount > 0 &&
             light.geometryInstance >= 0) {
    p[52] = 3.0f;  // payload tag: resolved GeometryLight triangle range
    p[53] = static_cast<float>(light.geometryTriOffset);
    p[54] = static_cast<float>(light.geometryTriCount);
    p[55] = static_cast<float>(light.geometryInstance);
  }
}

void PackRtMaterialConstants(const DrawMaterialCPU& material, float* pbr,
                             float* base, float* lightRt) {
  if (pbr) {
    pbr[0] = material.metallic;
    pbr[1] = material.roughness;
    pbr[2] = material.emissive[0];
    pbr[3] = material.emissive[1];
    pbr[4] = material.emissive[2];
    pbr[5] = material.alpha;
  }
  if (base) {
    base[0] = material.baseColor[0];
    base[1] = material.baseColor[1];
    base[2] = material.baseColor[2];
  }
  if (lightRt) PackLightRtOpenPBR(material, lightRt);
}

uint32_t RtLightCollectionMaskForMesh(
    const std::vector<DrawLightCPU>& lights, int meshIndex, bool shadow) {
  uint32_t mask = 0u;
  const size_t count = std::min(lights.size(),
                                static_cast<size_t>(kMaxRtLinkedLights));
  for (size_t i = 0; i < count; ++i) {
    const DrawLightCPU& light = lights[i];
    const bool all = shadow ? light.shadowLinksAll : light.lightLinksAll;
    const std::vector<int>& linked = shadow ? light.shadowLinkMeshIndices
                                            : light.lightLinkMeshIndices;
    if (all || std::find(linked.begin(), linked.end(), meshIndex) != linked.end())
      mask |= uint32_t{1} << static_cast<uint32_t>(i);
  }
  return mask;
}

uint32_t RtLightCollectionMaskForPath(
    const std::vector<DrawLightCPU>& lights, const std::string& carrierPath,
    bool shadow) {
  uint32_t mask = 0u;
  const size_t count = std::min(lights.size(),
                                static_cast<size_t>(kMaxRtLinkedLights));
  auto under = [](const std::string& path, const std::string& root) {
    return path == root ||
           (path.size() > root.size() &&
            path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == '/');
  };
  for (size_t i = 0; i < count; ++i) {
    const DrawLightCPU& light = lights[i];
    const bool all = shadow ? light.shadowLinksAll : light.lightLinksAll;
    const std::vector<std::string>& paths =
        shadow ? light.shadowLinkPaths : light.lightLinkPaths;
    bool linked = all;
    if (!linked) {
      for (const std::string& target : paths) {
        if (under(carrierPath, target)) {
          linked = true;
          break;
        }
      }
    }
    if (linked) mask |= uint32_t{1} << static_cast<uint32_t>(i);
  }
  return mask;
}

void BuildHostTextureTable(const std::vector<DrawTextureCPU>& sourceTextures,
                           const std::vector<DrawMaterialCPU>& materials,
                           HostTextureTable* out,
                           const std::vector<DrawLightCPU>* lights,
                           size_t maxTexelBytes) {
  if (!out) return;
  *out = HostTextureTable{};
  out->matTex.assign(
      std::max<size_t>(materials.size(), 1) * kRtMaterialTexSlots, -1);
  out->matTexParam.assign(std::max<size_t>(materials.size(), 1) *
                              kRtMaterialTextureParamFloats,
                          0.0f);
  out->sourceToTable.assign(sourceTextures.size(), -1);
  out->textures.reserve(sourceTextures.size());

  // Keep the packed RT table usage-driven. Source texture arrays can contain
  // decoded assets which are not connected to a renderable material (or an
  // environment light); retaining those bytes needlessly increases both CPU
  // staging and GPU residency for large scenes.
  std::vector<uint8_t> used(sourceTextures.size(), 0);
  auto markTexture = [&](int textureId) {
    if (textureId >= 0 && static_cast<size_t>(textureId) < used.size())
      used[static_cast<size_t>(textureId)] = 1;
  };
  for (const DrawMaterialCPU& dm : materials) {
    markTexture(dm.baseColorTex);
    markTexture(dm.metallicTex);
    markTexture(dm.roughnessTex);
    markTexture(dm.normalTex);
    markTexture(dm.emissiveTex);
    markTexture(dm.opacityTex);
    markTexture(dm.occlusionTex);
    markTexture(dm.coatWeightTex);
    markTexture(dm.coatColorTex);
    markTexture(dm.coatRoughnessTex);
    markTexture(dm.specularColorTex);
    markTexture(dm.coatNormalTex);
    for (const MaterialXGraphNodeCPU& node : dm.materialXGraph.nodes)
      markTexture(node.textureId);
  }
  if (lights) {
    for (const DrawLightCPU& light : *lights) markTexture(light.envmapTexture);
  }
  size_t skippedUnused = 0;
  size_t skippedBudget = 0;
  bool budgetRejected = false;
  std::unordered_map<uint64_t, std::vector<int>> packedTextureCandidates;
  size_t deduplicated = 0;

  auto decodeImage = [](const light3d::Image& src,
                        const DrawCompressedImageCPU& compressed,
                        light3d::Image* decoded) -> const light3d::Image* {
    if (src.width > 0 && src.height > 0 && !src.data.empty()) return &src;
    if (compressed.format == DrawCompressedFormat::None ||
        compressed.data.empty()) return nullptr;
    DrawCompressedImageCPU unused;
    TextureCompressCaps noGpuFormats;
    if (!TexToolsAdaptCompressed(
            compressed.data.data(), compressed.data.size(), false,
            compressed.format, static_cast<uint32_t>(compressed.width),
            static_cast<uint32_t>(compressed.height), noGpuFormats, &unused,
            decoded))
      return nullptr;
    return decoded;
  };
  auto decodeMips = [&](const std::vector<light3d::Image>& sourceMips,
                        const DrawCompressedImageCPU& compressed,
                        std::vector<light3d::Image>* decoded) ->
      const std::vector<light3d::Image>* {
    if (!sourceMips.empty()) return &sourceMips;
    decoded->clear();
    decoded->reserve(compressed.mips.size());
    for (const DrawCompressedMipCPU& mip : compressed.mips) {
      DrawCompressedImageCPU level;
      level.format = compressed.format;
      level.width = mip.width;
      level.height = mip.height;
      level.data = mip.data;
      light3d::Image levelImage;
      if (!decodeImage(light3d::Image{}, level, &levelImage)) break;
      decoded->push_back(std::move(levelImage));
    }
    return decoded;
  };
  auto hashBytes = [](uint64_t h, const uint8_t* data, size_t size) {
    constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    if (h == 0) h = kFnvOffset;
    for (size_t i = 0; i < size; ++i) {
      h ^= static_cast<uint64_t>(data[i]);
      h *= 1099511628211ull;
    }
    return h;
  };
  auto imageHash = [&](uint64_t h, const light3d::Image& image) {
    h = hashBytes(h, reinterpret_cast<const uint8_t*>(&image.width),
                  sizeof(image.width));
    h = hashBytes(h, reinterpret_cast<const uint8_t*>(&image.height),
                  sizeof(image.height));
    h = hashBytes(h, reinterpret_cast<const uint8_t*>(&image.channels),
                  sizeof(image.channels));
    return hashBytes(h, image.data.data(), image.data.size());
  };
  auto samePackedTexture = [&](int baseId, const light3d::Image& image,
                               const std::vector<light3d::Image>& mips,
                               const DrawTextureCPU& tex) {
    if (baseId < 0 || static_cast<size_t>(baseId) >= out->textures.size())
      return false;
    const HostTextureDesc* desc = &out->textures[static_cast<size_t>(baseId)];
    if (desc->width != image.width || desc->height != image.height ||
        desc->srgb != (tex.srgb ? 1 : 0) || desc->wrapS != tex.wrapS ||
        desc->wrapT != tex.wrapT || desc->mipCount != int(mips.size() + 1))
      return false;
    const light3d::Image* level = &image;
    for (size_t i = 0; i <= mips.size(); ++i) {
      if (desc->width != level->width || desc->height != level->height ||
          desc->offset < 0 ||
          static_cast<size_t>(desc->offset) + level->data.size() >
              out->texels.size() ||
          std::memcmp(out->texels.data() + desc->offset, level->data.data(),
                      level->data.size()) != 0)
        return false;
      if (i == mips.size()) break;
      if (desc->firstMip < 0 ||
          static_cast<size_t>(desc->firstMip) >= out->textures.size())
        return false;
      desc = &out->textures[static_cast<size_t>(desc->firstMip)];
      level = &mips[i];
    }
    return true;
  };
  auto appendImage = [&](const light3d::Image& image,
                         const std::vector<light3d::Image>& sourceMips,
                         const DrawTextureCPU& tex, int sourceSlot) {
    if (image.width <= 0 || image.height <= 0 || image.channels != 4 ||
        image.data.empty()) return -1;
    const int id = static_cast<int>(out->textures.size());
    std::vector<const light3d::Image*> levels{&image};
    int expectedW = image.width;
    int expectedH = image.height;
    for (const light3d::Image& mip : sourceMips) {
      if (tex.isPtex) break;  // face rectangles describe the base atlas only
      expectedW = std::max(1, expectedW / 2);
      expectedH = std::max(1, expectedH / 2);
      if (mip.width != expectedW || mip.height != expectedH ||
          mip.channels != 4 || mip.data.empty()) break;
      levels.push_back(&mip);
    }
    size_t payloadBytes = 0;
    for (const light3d::Image* level : levels) {
      if (level->data.size() > std::numeric_limits<size_t>::max() - payloadBytes)
        return -1;
      payloadBytes += level->data.size();
    }
    if (maxTexelBytes > 0 &&
        (payloadBytes > maxTexelBytes || out->texels.size() >
             maxTexelBytes - payloadBytes)) {
      budgetRejected = true;
      return -1;
    }
    if (out->texels.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        payloadBytes > static_cast<size_t>(std::numeric_limits<int>::max()) -
                           out->texels.size()) {
      budgetRejected = maxTexelBytes > 0;
      return -1;
    }
    for (size_t level = 0; level < levels.size(); ++level) {
      const light3d::Image& src = *levels[level];
      HostTextureDesc td;
      td.offset = static_cast<int>(out->texels.size());
      td.width = src.width;
      td.height = src.height;
      td.wrapS = tex.wrapS;
      td.wrapT = tex.wrapT;
      td.srgb = tex.srgb ? 1 : 0;
      td.isPtex = tex.isPtex ? 1 : 0;
      td.ptexCols = tex.ptexAtlasCols;
      td.ptexRows = tex.ptexAtlasRows;
      td.ptexTileEdge = static_cast<int>(tex.ptexTileEdge);
      td.ptexRectTexelOffset = static_cast<int>(tex.ptexRectTexelOffset);
      td.ptexFaceCount = static_cast<int>(tex.ptexFaceRects.size());
      td.mipCount = static_cast<int>(levels.size() - level);
      td.firstMip = td.mipCount > 1 ? id + static_cast<int>(level) + 1 : -1;
      td.imageSlot = tex.isPtex ? sourceSlot : -1;
      for (int& layer : td.udimLayer) layer = -1;
      out->texels.insert(out->texels.end(), src.data.begin(), src.data.end());
      out->textures.push_back(td);
    }
    return id;
  };
  for (size_t ti = 0; ti < sourceTextures.size(); ++ti) {
    if (!used[ti]) {
      ++skippedUnused;
      continue;
    }
    const DrawTextureCPU& tex = sourceTextures[ti];
    if (tex.isUdim) {
      HostTextureDesc virtualTex;
      virtualTex.wrapS = tex.wrapS;
      virtualTex.wrapT = tex.wrapT;
      virtualTex.srgb = tex.srgb ? 1 : 0;
      virtualTex.isUdim = 1;
      for (int& layer : virtualTex.udimLayer) layer = -1;
      const int virtualId = static_cast<int>(out->textures.size());
      out->textures.push_back(virtualTex);
      for (const DrawUdimTileCPU& tile : tex.udimTiles) {
        light3d::Image decoded;
        std::vector<light3d::Image> decodedMipsStorage;
        const light3d::Image* image =
            decodeImage(tile.image, tile.compressed, &decoded);
        const std::vector<light3d::Image>* mips =
            decodeMips(tile.mipImages, tile.compressed, &decodedMipsStorage);
        const int tileId = image ? appendImage(*image, *mips, tex,
                                               static_cast<int>(ti)) : -1;
        if (budgetRejected) {
          ++skippedBudget;
          budgetRejected = false;
        }
        const int lut = static_cast<int>(tile.udim) - 1001;
        if (tileId >= 0 && lut >= 0 && lut < 100) {
          out->textures[static_cast<size_t>(virtualId)].udimLayer[lut] = tileId;
        }
      }
      out->sourceToTable[ti] = virtualId;
    } else {
      light3d::Image decoded;
      light3d::Image restoredPtex;
      std::vector<light3d::Image> decodedMipsStorage;
      const light3d::Image* sourceImage =
          decodeImage(tex.image, tex.compressed, &decoded);
      const std::vector<light3d::Image>* mips =
          decodeMips(tex.mipImages, tex.compressed, &decodedMipsStorage);
      if (!sourceImage) {
        out->sourceToTable[ti] = -1;
        continue;
      }
      const light3d::Image* image = sourceImage;
      if (tex.isPtex && !tex.compressed.data.empty()) {
        restoredPtex = *sourceImage;
        RestorePtexRectMetadata(&restoredPtex, tex);
        image = &restoredPtex;
      }
      uint64_t hash = imageHash(0, *image);
      for (const light3d::Image& mip : *mips) hash = imageHash(hash, mip);
      hash = hashBytes(hash, reinterpret_cast<const uint8_t*>(&tex.wrapS),
                       sizeof(tex.wrapS));
      hash = hashBytes(hash, reinterpret_cast<const uint8_t*>(&tex.wrapT),
                       sizeof(tex.wrapT));
      hash = hashBytes(hash, reinterpret_cast<const uint8_t*>(&tex.srgb),
                       sizeof(tex.srgb));
      int tableId = -1;
      auto candidates = packedTextureCandidates.find(hash);
      if (candidates != packedTextureCandidates.end()) {
        for (int candidate : candidates->second) {
          if (samePackedTexture(candidate, *image, *mips, tex)) {
            tableId = candidate;
            ++deduplicated;
            break;
          }
        }
      }
      if (tableId < 0) {
        tableId = appendImage(*image, *mips, tex, static_cast<int>(ti));
        if (budgetRejected) {
          ++skippedBudget;
          budgetRejected = false;
        }
        if (tableId >= 0) packedTextureCandidates[hash].push_back(tableId);
      }
      out->sourceToTable[ti] = tableId;
    }
  }
  auto mapTex = [&](int t) -> int {
    return (t >= 0 && static_cast<size_t>(t) < out->sourceToTable.size())
               ? out->sourceToTable[static_cast<size_t>(t)]
               : -1;
  };
  for (size_t i = 0; i < materials.size(); ++i) {
    const DrawMaterialCPU& dm = materials[i];
    out->matTex[i * kRtMaterialTexSlots + 0] = mapTex(dm.baseColorTex);
    out->matTex[i * kRtMaterialTexSlots + 1] = mapTex(dm.metallicTex);
    out->matTex[i * kRtMaterialTexSlots + 2] = mapTex(dm.roughnessTex);
    out->matTex[i * kRtMaterialTexSlots + 3] = mapTex(dm.normalTex);
    out->matTex[i * kRtMaterialTexSlots + 4] = mapTex(dm.emissiveTex);
    out->matTex[i * kRtMaterialTexSlots + 5] = mapTex(dm.opacityTex);
    out->matTex[i * kRtMaterialTexSlots + 6] = mapTex(dm.occlusionTex);
    out->matTex[i * kRtMaterialTexSlots + 7] = mapTex(dm.coatWeightTex);
    out->matTex[i * kRtMaterialTexSlots + 8] = mapTex(dm.coatColorTex);
    out->matTex[i * kRtMaterialTexSlots + 9] = mapTex(dm.coatRoughnessTex);
    out->matTex[i * kRtMaterialTexSlots + 10] = mapTex(dm.specularColorTex);
    out->matTex[i * kRtMaterialTexSlots + 11] = mapTex(dm.coatNormalTex);
    PackRtMaterialTextureParams(
        dm, &out->matTexParam[i * kRtMaterialTextureParamFloats]);
  }
  LOGD("RT texture table: %zu source texture(s), %zu descriptor(s), %.2f MiB "
       "packed, skipped %zu unused / %zu budget-rejected source texture(s), "
       "deduplicated %zu%s",
       sourceTextures.size(), out->textures.size(),
       static_cast<double>(out->texels.size()) / (1024.0 * 1024.0),
       skippedUnused, skippedBudget, deduplicated,
       maxTexelBytes ? (" (budget " +
                        std::to_string(maxTexelBytes / (1024.0 * 1024.0)) +
                        " MiB)").c_str()
                     : "");
}

namespace {

void CarrierWorldPoint(const float m[16], const float* p, float out[3]) {
  out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
  out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
  out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

float CarrierWorldScale(const float m[16]) {
  float scale = 0.0f;
  for (int c = 0; c < 3; ++c) {
    const float x = m[c * 4 + 0], y = m[c * 4 + 1], z = m[c * 4 + 2];
    scale = std::max(scale, std::sqrt(x * x + y * y + z * z));
  }
  return scale;
}

void InitProxyMesh(const std::string& name, const std::string& path,
                   const std::string& purpose, int materialId,
                   DrawMeshCPU* mesh) {
  mesh->name = name;
  mesh->absPath = path;
  mesh->purpose = purpose;
  for (int i = 0; i < 16; ++i) {
    mesh->world[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    mesh->skinGeomBind[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
  mesh->doubleSided = true;
  DrawSubmesh sub;
  sub.materialId = materialId;
  sub.indexOffset = 0;
  mesh->submeshes.push_back(sub);
}

void AddProxyVertex(const float p[3], const float n[3], const float color[3],
                    float opacity, DrawMeshCPU* mesh) {
  DrawVertex v{};
  v.px = p[0]; v.py = p[1]; v.pz = p[2];
  v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
  mesh->vertices.push_back(v);
  mesh->vertexColors.insert(mesh->vertexColors.end(), color, color + 3);
  mesh->vertexAlpha.push_back(opacity);
}

size_t RtProxyTriangleLimit() {
  size_t limit = 262144;
  if (const char *env = std::getenv("LUSDVIEW_RT_PROXY_CHUNK_TRIS")) {
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end != env && parsed > 0) limit = static_cast<size_t>(parsed);
  }
  return limit;
}

}  // namespace

std::vector<DrawMeshCPU> BuildNonMeshRtProxyMeshes(const DrawScene& scene,
                                                  bool compact,
                                                  size_t maxTriangles) {
  std::vector<DrawMeshCPU> proxies;
  proxies.reserve(scene.points.size() + scene.curves.size());
  const size_t proxyTriangleLimit = RtProxyTriangleLimit();
  size_t remainingTriangles = maxTriangles;
  auto reserveTriangles = [&](size_t count) {
    if (maxTriangles == 0) return true;
    if (remainingTriangles < count) return false;
    remainingTriangles -= count;
    return true;
  };
  for (const DrawPointsCPU& src : scene.points) {
    const size_t pointCount = src.points.size() / 3;
    const bool native_gaussian =
        src.gaussian && src.ellipseRadii.size() >= pointCount * 2 &&
        src.ellipseNormals.size() >= pointCount * 3 &&
        src.ellipseMajorAxes.size() >= pointCount * 3;
    const bool analytic_points =
        !src.gaussian && src.normals.size() >= pointCount * 3;
    if (native_gaussian || analytic_points) {
      continue;  // native ellipse carrier is uploaded by CUDA/HIP/Vulkan RT paths
    }
    DrawMeshCPU mesh;
    InitProxyMesh(src.name, src.absPath, src.purpose, src.materialId, &mesh);
    auto flush = [&]() {
      if (mesh.indices.empty()) return;
      mesh.submeshes[0].indexCount = static_cast<uint32_t>(mesh.indices.size());
      proxies.push_back(std::move(mesh));
      mesh = DrawMeshCPU{};
      InitProxyMesh(src.name, src.absPath, src.purpose, src.materialId, &mesh);
    };
    const size_t count = pointCount;
    const float scale = CarrierWorldScale(src.world);
    if (src.gaussian && src.ellipseRadii.size() >= count * 2 &&
        src.ellipseNormals.size() >= count * 3 &&
        src.ellipseMajorAxes.size() >= count * 3) {
      auto xv = [&](const float a[3], float o[3]) {
        o[0] = src.world[0] * a[0] + src.world[4] * a[1] + src.world[8] * a[2];
        o[1] = src.world[1] * a[0] + src.world[5] * a[1] + src.world[9] * a[2];
        o[2] = src.world[2] * a[0] + src.world[6] * a[1] + src.world[10] * a[2];
      };
      const int kSegments = compact ? 4 : 8;
      for (size_t i = 0; i < count; ++i) {
        if (!reserveTriangles(static_cast<size_t>(kSegments) * 2)) {
          flush();
          return proxies;
        }
        if (!mesh.indices.empty() &&
            mesh.indices.size() / 3 + 16 > proxyTriangleLimit)
          flush();
        float center[3];
        CarrierWorldPoint(src.world, &src.points[i * 3], center);
        float major[3], normal[3];
        xv(&src.ellipseMajorAxes[i * 3], major);
        xv(&src.ellipseNormals[i * 3], normal);
        float nl = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                              normal[2] * normal[2]);
        if (nl <= 1.0e-8f) continue;
        for (float& v : normal) v /= nl;
        float ml = std::sqrt(major[0] * major[0] + major[1] * major[1] +
                             major[2] * major[2]);
        if (ml <= 1.0e-8f) continue;
        const float majorLen = ml;
        for (float& v : major) v /= ml;
        float minor[3] = {normal[1] * major[2] - normal[2] * major[1],
                          normal[2] * major[0] - normal[0] * major[2],
                          normal[0] * major[1] - normal[1] * major[0]};
        const float rx = 0.5f * src.ellipseRadii[i * 2] * majorLen;
        const float ry = 0.5f * src.ellipseRadii[i * 2 + 1] *
                         std::sqrt(minor[0] * minor[0] + minor[1] * minor[1] +
                                   minor[2] * minor[2]);
        float color[3] = {1.0f, 1.0f, 1.0f};
        if (src.colors.size() >= 3) {
          const size_t ci = src.colors.size() >= (i + 1) * 3 ? i * 3 : 0;
          color[0] = src.colors[ci]; color[1] = src.colors[ci + 1];
          color[2] = src.colors[ci + 2];
        }
        const float opacity = src.opacities.empty()
                                  ? 1.0f
                                  : src.opacities[src.opacities.size() > i ? i : 0];
        for (int k = 0; k < kSegments; ++k) {
          const float a0 = 6.28318530718f * float(k) / float(kSegments);
          const float a1 = 6.28318530718f * float(k + 1) / float(kSegments);
          auto pointAt = [&](float a, float p[3]) {
            p[0] = center[0] + major[0] * std::cos(a) * rx + minor[0] * std::sin(a) * ry;
            p[1] = center[1] + major[1] * std::cos(a) * rx + minor[1] * std::sin(a) * ry;
            p[2] = center[2] + major[2] * std::cos(a) * rx + minor[2] * std::sin(a) * ry;
          };
          float p1[3], p2[3]; pointAt(a0, p1); pointAt(a1, p2);
          const uint32_t b = static_cast<uint32_t>(mesh.vertices.size());
          AddProxyVertex(center, normal, color, opacity, &mesh);
          AddProxyVertex(p1, normal, color, opacity, &mesh);
          AddProxyVertex(p2, normal, color, opacity, &mesh);
          mesh.indices.insert(mesh.indices.end(), {b, b + 1, b + 2});
          const uint32_t bb = static_cast<uint32_t>(mesh.vertices.size());
          float backN[3] = {-normal[0], -normal[1], -normal[2]};
          AddProxyVertex(center, backN, color, opacity, &mesh);
          AddProxyVertex(p2, backN, color, opacity, &mesh);
          AddProxyVertex(p1, backN, color, opacity, &mesh);
          mesh.indices.insert(mesh.indices.end(), {bb, bb + 1, bb + 2});
        }
      }
      flush();
      continue;
    }
    // A subdivided octahedron (subdivide each of 8 faces into 4) producing
    // 32 triangles per point. Compact mode uses the base octahedron (8
    // triangles), retaining a conservative volume at a much lower host cost.
    static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const uint32_t edges[12][2] = {{0,2},{2,1},{1,3},{3,0},{0,4},{2,4},
                                           {1,4},{3,4},{0,5},{2,5},{1,5},{3,5}};
    for (size_t i = 0; i < count; ++i) {
      if (!reserveTriangles(static_cast<size_t>(compact ? 8 : 32))) {
        flush();
        return proxies;
      }
      if (!mesh.indices.empty() &&
          mesh.indices.size() / 3 + 32 > proxyTriangleLimit)
        flush();
      float center[3];
      CarrierWorldPoint(src.world, &src.points[i * 3], center);
      const float width = (src.widths.empty() ? 1.0f
                           : src.widths.size() == 1 ? src.widths[0]
                           : src.widths[std::min(i, src.widths.size() - 1)]) *
                          scale;
      float color[3] = {1.0f, 1.0f, 1.0f};
      if (src.colors.size() >= 3) {
        const size_t ci = src.colors.size() >= (i + 1) * 3 ? i * 3 : 0;
        color[0] = src.colors[ci]; color[1] = src.colors[ci + 1];
        color[2] = src.colors[ci + 2];
      }
      const float opacity = src.opacities.empty()
                                ? 1.0f
                                : src.opacities[src.opacities.size() > i ? i : 0];
      const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
      // Add the 6 octahedron base vertices.
      for (const auto& dir : dirs) {
        const float p[3] = {center[0] + dir[0] * width * 0.5f,
                            center[1] + dir[1] * width * 0.5f,
                            center[2] + dir[2] * width * 0.5f};
        AddProxyVertex(p, dir, color, opacity, &mesh);
      }
      if (compact) {
        static const uint32_t octaFaces[8][3] = {
            {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
            {0, 5, 2}, {2, 5, 1}, {1, 5, 3}, {3, 5, 0}};
        for (const auto& face : octaFaces) {
          mesh.indices.insert(mesh.indices.end(),
                              {base + face[0], base + face[1], base + face[2]});
        }
        continue;
      }
      // Add the 12 edge-midpoint vertices (normalized for spherical proxy).
      for (const auto& edge : edges) {
        const float* d0 = dirs[edge[0]];
        const float* d1 = dirs[edge[1]];
        float dir[3] = {d0[0] + d1[0], d0[1] + d1[1], d0[2] + d1[2]};
        float len = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        if (len > 1e-8f) { dir[0]/=len; dir[1]/=len; dir[2]/=len; }
        const float p[3] = {center[0] + dir[0] * width * 0.5f,
                            center[1] + dir[1] * width * 0.5f,
                            center[2] + dir[2] * width * 0.5f};
        AddProxyVertex(p, dir, color, opacity, &mesh);
      }
      // 4 triangles per original octahedron face, using subdivide pattern:
      // face (a,b,c) -> (a, ab, ac), (ab, b, bc), (ac, bc, c), (ab, bc, ac)
      // where ab = edge midpoint of a-b, etc. Edge indices match edges[] order:
      //   v0-v2=0, v2-v1=1, v1-v3=2, v3-v0=3, v0-v4=4, v2-v4=5,
      //   v1-v4=6, v3-v4=7, v0-v5=8, v2-v5=9, v1-v5=10, v3-v5=11
      static const int face_edges[8][3] = {
        {0,4,5},  // top: v0 v2 v4
        {1,5,6},  // top: v2 v1 v4
        {2,6,7},  // top: v1 v3 v4
        {3,7,4},  // top: v3 v0 v4
        {8,9,0},  // bot: v0 v5 v2 (edge 0 reversed: v2-v0 = same as v0-v2)
        {9,10,1}, // bot: v2 v5 v1
        {10,11,2},// bot: v1 v5 v3
        {11,8,3}  // bot: v3 v5 v0
      };
      for (const auto& fe : face_edges) {
        const uint32_t a = base + fe[0], b = base + fe[1], c = base + fe[2];
        // (v0, v0v2_mid, v0v4_mid)
        mesh.indices.push_back(a);
        mesh.indices.push_back(base + 6 + fe[0]);
        mesh.indices.push_back(base + 6 + fe[1]);
        // (v0v2_mid, v2, v2v4_mid)
        mesh.indices.push_back(base + 6 + fe[0]);
        mesh.indices.push_back(b);
        mesh.indices.push_back(base + 6 + fe[2]);
        // (v0v4_mid, v2v4_mid, v4)
        mesh.indices.push_back(base + 6 + fe[1]);
        mesh.indices.push_back(base + 6 + fe[2]);
        mesh.indices.push_back(c);
        // (v0v2_mid, v2v4_mid, v0v4_mid)
        mesh.indices.push_back(base + 6 + fe[0]);
        mesh.indices.push_back(base + 6 + fe[2]);
        mesh.indices.push_back(base + 6 + fe[1]);
      }
    }
    flush();
  }

  for (const DrawCurvesCPU& src : scene.curves) {
    DrawMeshCPU mesh;
    InitProxyMesh(src.name, src.absPath, src.purpose, src.materialId, &mesh);
    auto flush = [&]() {
      if (mesh.indices.empty()) return;
      mesh.submeshes[0].indexCount = static_cast<uint32_t>(mesh.indices.size());
      proxies.push_back(std::move(mesh));
      mesh = DrawMeshCPU{};
      InitProxyMesh(src.name, src.absPath, src.purpose, src.materialId, &mesh);
    };
    const size_t pointCount = src.points.size() / 3;
    const float scale = CarrierWorldScale(src.world);
    size_t begin = 0;
    for (uint32_t authoredCount : src.vertexCounts) {
      const size_t end = std::min(pointCount, begin + authoredCount);
      for (size_t i = begin; i + 1 < end; ++i) {
        const int kSegments = compact ? 4 : 8;
        if (!reserveTriangles(static_cast<size_t>(kSegments) * 2)) {
          flush();
          return proxies;
        }
        if (!mesh.indices.empty() &&
            mesh.indices.size() / 3 + 16 > proxyTriangleLimit)
          flush();
        float p0[3], p1[3];
        CarrierWorldPoint(src.world, &src.points[i * 3], p0);
        CarrierWorldPoint(src.world, &src.points[(i + 1) * 3], p1);
        float axis[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float axisLen = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] +
                                        axis[2] * axis[2]);
        if (axisLen <= 1.0e-8f) continue;
        for (float& v : axis) v /= axisLen;
        const float ref[3] = {std::fabs(axis[1]) < 0.9f ? 0.0f : 1.0f,
                              std::fabs(axis[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        float side[3] = {axis[1] * ref[2] - axis[2] * ref[1],
                         axis[2] * ref[0] - axis[0] * ref[2],
                         axis[0] * ref[1] - axis[1] * ref[0]};
        const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] +
                                   side[2] * side[2]);
        for (float& v : side) v /= sl;
        const float up[3] = {side[1] * axis[2] - side[2] * axis[1],
                             side[2] * axis[0] - side[0] * axis[2],
                             side[0] * axis[1] - side[1] * axis[0]};
        const float width = (src.widths.empty() ? 1.0f
            : src.widths.size() == 1 ? src.widths[0]
            : 0.5f * (src.widths[std::min(i, src.widths.size() - 1)] +
                      src.widths[std::min(i + 1, src.widths.size() - 1)])) * scale;
        float color[3] = {1.0f, 1.0f, 1.0f};
        if (src.colors.size() >= pointCount * 3) {
          for (int c = 0; c < 3; ++c) {
            color[c] = 0.5f * (src.colors[i * 3 + c] +
                               src.colors[(i + 1) * 3 + c]);
          }
        }
        float opacity = 1.0f;
        if (!src.opacities.empty()) {
          const float o0 = src.opacities[src.opacities.size() > i ? i : 0];
          const float o1 =
              src.opacities[src.opacities.size() > i + 1 ? i + 1 : 0];
          opacity = 0.5f * (o0 + o1);
        }
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        for (int ring = 0; ring < 2; ++ring) {
          const float* center = ring == 0 ? p0 : p1;
          for (int k = 0; k < kSegments; ++k) {
            const float angle = 6.28318530718f * static_cast<float>(k) /
                                static_cast<float>(kSegments);
            const float normal[3] = {side[0] * std::cos(angle) + up[0] * std::sin(angle),
                                     side[1] * std::cos(angle) + up[1] * std::sin(angle),
                                     side[2] * std::cos(angle) + up[2] * std::sin(angle)};
            const float p[3] = {center[0] + normal[0] * width * 0.5f,
                                center[1] + normal[1] * width * 0.5f,
                                center[2] + normal[2] * width * 0.5f};
            AddProxyVertex(p, normal, color, opacity, &mesh);
          }
        }
        for (uint32_t k = 0; k < static_cast<uint32_t>(kSegments); ++k) {
          const uint32_t next = (k + 1) % static_cast<uint32_t>(kSegments);
          mesh.indices.insert(mesh.indices.end(),
                              {base + k, base + kSegments + k,
                               base + kSegments + next,
                               base + k, base + kSegments + next,
                               base + next});
        }
      }
      begin = end;
    }
    flush();
  }
  return proxies;
}

bool BuildHostScene(const DrawScene& scene, size_t maxTris, size_t maxInstances,
                    float displacementScale, HostScene* out, std::string* err,
                    BuildProgress* progress, RefitMap* refitOut,
                    size_t textureBudgetBytes,
                    const std::vector<uint8_t>* meshVisible,
                    uint32_t purposeVisibleMask) {
  if (refitOut) {
    refitOut->valid = false;
    refitOut->meshes.clear();
    refitOut->instances.clear();
  }
  // Refit rewrites positions/normals only, so it cannot reproduce a DISPLACED
  // flatten (it re-samples textures per pose). displacementScale alone is not
  // the test -- the default GUI scale is 1.0 -- displacement must actually be
  // live on some mesh to disqualify the map.
  bool recordRefit = refitOut != nullptr;
  if (recordRefit && displacementScale != 0.0f) {
    for (const DrawMeshCPU& m : scene.meshes) {
      if (MeshHasDisplacement(scene, m)) { recordRefit = false; break; }
    }
  }
  const size_t cap = maxTris ? maxTris : (size_t(1) << 62);
  const size_t instCap = maxInstances ? maxInstances : ~size_t(0);
  const bool compactCarriers = maxTris > 0 && maxTris <= 64000000;
  const size_t compactPointBudget = compactCarriers ? maxTris / 8 : 0;
  auto setPhase = [&](int p, size_t total) {
    if (progress) { progress->phase = p; progress->done = 0; progress->total = total; }
  };

  using Clock = std::chrono::steady_clock;
  auto t0 = Clock::now();
  auto ms = [&](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
  };

  // Keep Gaussian splats and authored-normal Points as analytic ellipses. The
  // carrier stores local-space radii and axes; bake only the affine placement
  // here so CUDA/HIP RT does not expand disk/oval point clouds into triangles.
  for (const DrawPointsCPU& src : scene.points) {
    const size_t n = src.points.size() / 3;
    const bool gaussian =
        src.gaussian && src.ellipseRadii.size() >= n * 2 &&
        src.ellipseNormals.size() >= n * 3 &&
        src.ellipseMajorAxes.size() >= n * 3;
    const bool analytic_points =
        !src.gaussian &&
        (src.normals.size() >= n * 3 || compactCarriers);
    if (!gaussian && !analytic_points)
      continue;
    for (size_t i = 0; i < n; ++i) {
      if (compactCarriers && out->pointCenters.size() / 3 >= compactPointBudget)
        break;
      const float opacity = src.opacities.empty()
                                ? 1.0f
                                : src.opacities[src.opacities.size() > i ? i : 0];
      // A splat below this threshold contributes less than one percent even
      // at its center. Do not pay BVH/build/upload cost for it. This mirrors
      // the native lusdrender path and only affects analytic RT carriers.
      if (!std::isfinite(opacity) || opacity < 0.01f) continue;
      float c[3]; CarrierWorldPoint(src.world, &src.points[i * 3], c);
      if (!std::isfinite(c[0]) || !std::isfinite(c[1]) ||
          !std::isfinite(c[2]))
        continue;
      float major[3], normal[3];
      float gaussianMinorLength = 0.0f;
      if (gaussian) {
        const float* localMajor = &src.ellipseMajorAxes[i * 3];
        const float* localNormal = &src.ellipseNormals[i * 3];
        const float localMinor[3] = {
            localNormal[1] * localMajor[2] - localNormal[2] * localMajor[1],
            localNormal[2] * localMajor[0] - localNormal[0] * localMajor[2],
            localNormal[0] * localMajor[1] - localNormal[1] * localMajor[0]};
        float minor[3];
        for (int k = 0; k < 3; ++k) {
          major[k] = src.world[0 * 4 + k] * localMajor[0] +
                     src.world[1 * 4 + k] * localMajor[1] +
                     src.world[2 * 4 + k] * localMajor[2];
          minor[k] = src.world[0 * 4 + k] * localMinor[0] +
                     src.world[1 * 4 + k] * localMinor[1] +
                     src.world[2 * 4 + k] * localMinor[2];
        }
        normal[0] = major[1] * minor[2] - major[2] * minor[1];
        normal[1] = major[2] * minor[0] - major[0] * minor[2];
        normal[2] = major[0] * minor[1] - major[1] * minor[0];
        gaussianMinorLength =
            std::sqrt(minor[0] * minor[0] + minor[1] * minor[1] +
                      minor[2] * minor[2]);
      } else {
        if (src.normals.size() >= (i + 1) * 3) {
          for (int k = 0; k < 3; ++k) {
            major[k] = src.world[0 * 4 + k];
            normal[k] = src.world[0 * 4 + k] * src.normals[i * 3 + 0] +
                       src.world[1 * 4 + k] * src.normals[i * 3 + 1] +
                       src.world[2 * 4 + k] * src.normals[i * 3 + 2];
          }
        } else {
          // Legacy point carriers often have no authored normal. Use a stable
          // compact splat orientation rather than expanding them to triangles.
          major[0] = src.world[0]; major[1] = src.world[1]; major[2] = src.world[2];
          normal[0] = src.world[4]; normal[1] = src.world[5]; normal[2] = src.world[6];
        }
      }
      const float nl = std::sqrt(normal[0] * normal[0] +
                                  normal[1] * normal[1] +
                                  normal[2] * normal[2]);
      if (!std::isfinite(nl) || nl <= 1.0e-8f) continue;
      for (float& v : normal) v /= nl;
      if (!gaussian) {
        const float d = major[0] * normal[0] + major[1] * normal[1] +
                        major[2] * normal[2];
        for (int k = 0; k < 3; ++k) major[k] -= d * normal[k];
      }
      const float ml = std::sqrt(major[0] * major[0] +
                                 major[1] * major[1] +
                                 major[2] * major[2]);
      if (!std::isfinite(ml) || !std::isfinite(nl) || ml <= 1.0e-8f ||
          nl <= 1.0e-8f) continue;
      float rx = 0.0f, ry = 0.0f;
      if (gaussian) {
        rx = src.ellipseRadii[i * 2] * ml;
        ry = src.ellipseRadii[i * 2 + 1] * gaussianMinorLength;
      } else {
        const float minor[3] = {
            normal[1] * major[2] - normal[2] * major[1],
            normal[2] * major[0] - normal[0] * major[2],
            normal[0] * major[1] - normal[1] * major[0]};
        const float minor_len = std::sqrt(minor[0] * minor[0] +
                                          minor[1] * minor[1] +
                                          minor[2] * minor[2]);
        const float width = src.widths.empty()
                                ? 1.0f
                                : (src.widths.size() == 1
                                       ? src.widths[0]
                                       : src.widths[i < src.widths.size()
                                                       ? i
                                                       : src.widths.size() - 1]);
        rx = 0.5f * std::fabs(width) * ml;
        ry = 0.5f * std::fabs(width) * minor_len;
      }
      if (!std::isfinite(rx) || !std::isfinite(ry) || rx <= 1.0e-8f ||
          ry <= 1.0e-8f)
        continue;
      out->pointCenters.insert(out->pointCenters.end(), c, c + 3);
      for (float& v : major) v /= ml;
      out->pointMajorAxes.insert(out->pointMajorAxes.end(), major, major + 3);
      out->pointNormals.insert(out->pointNormals.end(), normal, normal + 3);
      out->pointRadii.push_back(rx);
      out->pointRadii.push_back(ry);
      const size_t ci = src.colors.size() >= (i + 1) * 3 ? i * 3 : 0;
      out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci] : 1.0f);
      out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci + 1] : 1.0f);
      out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci + 2] : 1.0f);
      out->pointColors.push_back(opacity);
    }
  }
  // Compact RT mode represents curve samples as analytic splats instead of
  // expanding every segment into a triangle tube. This retains the authored
  // locations and width while keeping host/GPU residency bounded.
  if (compactCarriers && out->pointCenters.size() / 3 < compactPointBudget) {
    for (const DrawCurvesCPU& src : scene.curves) {
      const size_t n = src.points.size() / 3;
      for (size_t i = 0; i < n && out->pointCenters.size() / 3 < compactPointBudget; ++i) {
        float center[3];
        CarrierWorldPoint(src.world, &src.points[i * 3], center);
        // Orient each analytic disk to the local curve tangent. The previous
        // fixed world-axis frame made compact curves look like a row of
        // unrelated dots when the strand turned.
        const size_t prev = i > 0 ? i - 1 : i;
        const size_t next = i + 1 < n ? i + 1 : i;
        float localTangent[3] = {
            src.points[next * 3 + 0] - src.points[prev * 3 + 0],
            src.points[next * 3 + 1] - src.points[prev * 3 + 1],
            src.points[next * 3 + 2] - src.points[prev * 3 + 2]};
        float major[3] = {
            src.world[0] * localTangent[0] + src.world[4] * localTangent[1] +
                src.world[8] * localTangent[2],
            src.world[1] * localTangent[0] + src.world[5] * localTangent[1] +
                src.world[9] * localTangent[2],
            src.world[2] * localTangent[0] + src.world[6] * localTangent[1] +
                src.world[10] * localTangent[2]};
        const float ml = std::sqrt(major[0] * major[0] + major[1] * major[1] +
                                   major[2] * major[2]);
        if (!std::isfinite(ml) || ml <= 1.0e-8f) continue;
        for (float& v : major) v /= ml;
        const float ref[3] = {std::fabs(major[1]) < 0.9f ? 0.0f : 1.0f,
                              std::fabs(major[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        float normal[3] = {major[1] * ref[2] - major[2] * ref[1],
                           major[2] * ref[0] - major[0] * ref[2],
                           major[0] * ref[1] - major[1] * ref[0]};
        const float nl = std::sqrt(normal[0] * normal[0] +
                                   normal[1] * normal[1] + normal[2] * normal[2]);
        if (!std::isfinite(nl) || nl <= 1.0e-8f) continue;
        for (float& v : normal) v /= nl;
        const float width = (src.widths.empty()
                                 ? 1.0f
                                 : src.widths.size() == 1
                                       ? src.widths[0]
                                       : src.widths[std::min(i, src.widths.size() - 1)]) *
                            CarrierWorldScale(src.world);
        if (!std::isfinite(width) || std::fabs(width) <= 1.0e-8f) continue;
        out->pointCenters.insert(out->pointCenters.end(), center, center + 3);
        out->pointMajorAxes.insert(out->pointMajorAxes.end(), major, major + 3);
        out->pointNormals.insert(out->pointNormals.end(), normal, normal + 3);
        out->pointRadii.push_back(0.5f * std::fabs(width));
        out->pointRadii.push_back(0.5f * std::fabs(width));
        const size_t ci = src.colors.size() >= (i + 1) * 3 ? i * 3 : 0;
        out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci] : 1.0f);
        out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci + 1] : 1.0f);
        out->pointColors.push_back(src.colors.size() >= 3 ? src.colors[ci + 2] : 1.0f);
        out->pointColors.push_back(src.opacities.empty()
                                       ? 1.0f
                                       : src.opacities[src.opacities.size() > i ? i : 0]);
      }
    }
    LOGI("RT compact carriers: %zu analytic point/curve sample(s), budget %zu",
         out->pointCenters.size() / 3, compactPointBudget);
  }
  out->pointCount = out->pointCenters.size() / 3;
  if (out->pointCount > 0) {
    const size_t chunkLimit = GaussianBvhChunkLimit();
    for (size_t first = 0; first < out->pointCount; first += chunkLimit) {
      const size_t count = std::min(out->pointCount, first + chunkLimit) - first;
      std::vector<float> centers(count * 3), aabb(count * 6);
      std::vector<int> order(count);
      for (size_t i = 0; i < count; ++i) {
        order[i] = static_cast<int>(i);
        for (int k = 0; k < 3; ++k)
          centers[i * 3 + k] = out->pointCenters[(first + i) * 3 + k];
        const float r = std::max(out->pointRadii[(first + i) * 2],
                                 out->pointRadii[(first + i) * 2 + 1]);
        for (int k = 0; k < 3; ++k) {
          aabb[i * 6 + k] = centers[i * 3 + k] - r;
          aabb[i * 6 + 3 + k] = centers[i * 3 + k] + r;
        }
      }
      const size_t orderFirst = out->pointOrder.size();
      const size_t bvhFirst = out->pointBvh.size();
      std::vector<Node> localBvh =
          BuildTlas(order, static_cast<int>(count), centers, aabb);
      for (int& index : order) index += static_cast<int>(first);
      out->pointOrder.insert(out->pointOrder.end(), order.begin(), order.end());
      // BuildTlas returns a local root and local child indices. Traversal
      // adds bvhFirst once when selecting this chunk, so do not globally
      // offset interior children here. Leaf `left` remains an order index.
      out->pointBvh.insert(out->pointBvh.end(), localBvh.begin(), localBvh.end());
      out->pointChunks.push_back({static_cast<int>(first), static_cast<int>(count),
                                  static_cast<int>(orderFirst),
                                  static_cast<int>(bvhFirst),
                                  static_cast<int>(localBvh.size())});
    }
    LOGI("RT Gaussian point BVH: %zu point(s) in %zu chunk(s), limit %zu",
         out->pointCount, out->pointChunks.size(), chunkLimit);
  }

  // Phase A/B1: build and assemble bounded mesh batches. Keeping only a
  // worker-sized batch alive avoids retaining every intermediate MeshBuild
  // while the rest of a large scene is still being built.
  // In a deliberately reduced RT build, proxying every point/curve carrier
  // before the global cap is counterproductive: large procedural carriers can
  // consume tens of GiB, only to be discarded when the instance/triangle cap
  // is reached. Native analytic points remain supported; compact mode keeps
  // the carrier data in the bounded analytic representation above while
  // preserving meshes as the higher-priority input.
  std::vector<DrawMeshCPU> nonMeshProxies;
  const bool compactNonMeshProxies = maxTris > 0 && maxTris <= 64000000;
  if (!compactNonMeshProxies)
    nonMeshProxies = BuildNonMeshRtProxyMeshes(scene, false, 0);
  if (compactNonMeshProxies) {
    LOGI("RT geometry: compact analytic point/curve carriers enabled "
         "(sample budget=%zu, max-tris=%zu)", compactPointBudget, maxTris);
  }
  std::vector<const DrawMeshCPU*> sourceMeshes;
  std::vector<int> sourceSceneMesh;
  sourceMeshes.reserve(scene.meshes.size() + nonMeshProxies.size());
  sourceSceneMesh.reserve(scene.meshes.size() + nonMeshProxies.size());
  for (size_t i = 0; i < scene.meshes.size(); ++i) {
    const DrawMeshCPU& mesh = scene.meshes[i];
    if (meshVisible && i < meshVisible->size() && !(*meshVisible)[i]) continue;
    const uint32_t purpose = static_cast<uint32_t>(PurposeId(mesh.purpose)) & 3u;
    if (((purposeVisibleMask >> purpose) & 1u) == 0u) continue;
    sourceMeshes.push_back(&mesh);
    sourceSceneMesh.push_back(static_cast<int>(i));
  }
  for (const DrawMeshCPU& mesh : nonMeshProxies) {
    sourceMeshes.push_back(&mesh);
    sourceSceneMesh.push_back(-1);
  }
  out->geomProps.clear();
  for (const DrawMeshCPU* mesh : sourceMeshes) {
    for (const DrawGeomPropCPU& source : mesh->geomProps) {
      auto found = std::find_if(
          out->geomProps.begin(), out->geomProps.end(),
          [&](const HostGeomProp& prop) { return prop.name == source.name; });
      if (found == out->geomProps.end()) {
        HostGeomProp& prop = out->geomProps.emplace_back();
        prop.name = source.name;
        prop.components = source.components;
      }
    }
  }
  std::atomic<size_t> buildDone{0};
  size_t assembleDone = 0;
  auto tickBuild = [&]() {
    if (!progress) return;
    progress->phase = 0;
    progress->done = ++buildDone;
    progress->total = sourceMeshes.size();
  };
  auto tickAssemble = [&]() {
    if (!progress) return;
    progress->phase = 1;
    progress->done = ++assembleDone;
    progress->total = sourceMeshes.size();
  };
  setPhase(0, sourceMeshes.size());  // geometry
  size_t buildBatchMeshes = std::max<size_t>(1, size_t(RtBuildWorkerLimit()) * 2);
  // A reduced RT cap is also the memory contract: do not retain a worker-sized
  // set of large MeshBuild temporaries while assembling the next batch. The
  // environment variable below remains an explicit tuning escape hatch.
  if (compactNonMeshProxies) buildBatchMeshes = 1;
  if (const char *env = std::getenv("LUSDVIEW_RT_BUILD_BATCH_MESHES")) {
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end != env && parsed > 0)
      buildBatchMeshes = static_cast<size_t>(parsed);
  }
  LOGD("RT geometry build: %zu mesh(es), %u worker(s), batch %zu",
       sourceMeshes.size(), RtBuildWorkerLimit(), buildBatchMeshes);

  // Phase B1: assemble geometry in mesh order (offsets/caps identical to the
  // serial build) and collect a flat list of instance sources. The per-instance
  // matrix math (inverse + world AABB) is deferred to a parallel pass (B2) since
  // it dominates on massively-instanced scenes (e.g. Moana Island).
  struct InstSrc {
    float o2w[12];
    float tint[4];
    int blasRoot;
    int proto;          // index into protoBox
    int meshIndex;
  };
  std::vector<InstSrc> isrc;
  std::vector<std::array<float, 6>> protoBox;  // {lo.xyz, hi.xyz} per accepted mesh
  std::vector<size_t> sourceTriOffsets(sourceMeshes.size(), 0);
  std::vector<size_t> sourceTriCounts(sourceMeshes.size(), 0);
  setPhase(1, sourceMeshes.size());  // assemble
  for (size_t first = 0; first < sourceMeshes.size() && !out->truncated;
       first += buildBatchMeshes) {
    const size_t count = std::min(buildBatchMeshes, sourceMeshes.size() - first);
    std::vector<MeshBuild> mbs(count);
    ParallelFor(count, [&](size_t i) {
      mbs[i] = BuildOneMesh(scene, *sourceMeshes[first + i], displacementScale);
      tickBuild();
    });
    for (size_t local = 0; local < count && !out->truncated; ++local) {
      const size_t mbi = first + local;
      MeshBuild& mb = mbs[local];
    tickAssemble();
    if (!mb.valid) continue;
    if (out->tris.size() / 9 >= cap) { out->truncated = true; break; }
    if (isrc.size() >= instCap) { out->truncated = true; break; }
    const size_t triOff = out->tris.size() / 9;
    sourceTriOffsets[mbi] = triOff;
    if (recordRefit && sourceSceneMesh[mbi] >= 0) {
      RefitMeshMap rm;
      rm.sceneMesh = static_cast<size_t>(sourceSceneMesh[mbi]);
      rm.triOffset = triOff;
      rm.leafOrder = std::move(mb.leafOrder);
      refitOut->meshes.push_back(std::move(rm));
    }
    const size_t mbTriCount = mb.mat.size();
    sourceTriCounts[mbi] = mbTriCount;
    const size_t nodeOff = out->blas.size();
    const int blasRoot = static_cast<int>(nodeOff);
    AppendMove(out->tris, mb.tris);
    AppendMove(out->nrms, mb.nrms);
    AppendMove(out->cols, mb.cols);
    AppendMove(out->uv, mb.uv);
    AppendMove(out->uv1, mb.uv1);
    AppendMove(out->infl, mb.infl);
    for (HostGeomProp& dst : out->geomProps) {
      auto found = std::find_if(
          mb.geomProps.begin(), mb.geomProps.end(),
          [&](const HostGeomProp& source) { return source.name == dst.name; });
      const size_t expected = mbTriCount * 3 * dst.components;
      if (found != mb.geomProps.end() &&
          found->components == dst.components &&
          found->values.size() == expected) {
        AppendMove(dst.values, found->values);
      } else {
        dst.values.insert(dst.values.end(), expected, 0.0f);
      }
    }
    AppendMove(out->domw, mb.domw);
    AppendMove(out->geo, mb.geo);
    AppendMove(out->emask, mb.emask);
    AppendMove(out->mat, mb.mat);
    if (!mb.backMat.empty()) {
      if (out->backMat.empty()) out->backMat.assign(triOff, -1);
      AppendMove(out->backMat, mb.backMat);
    } else if (!out->backMat.empty()) {
      out->backMat.insert(out->backMat.end(), mbTriCount, -1);
    }
    AppendMove(out->face, mb.face);
    AppendMove(out->domj, mb.domj);
    for (Node nd : mb.blas) {
      if (nd.count > 0) nd.left += static_cast<int>(triOff);
      else { nd.left += static_cast<int>(nodeOff); nd.right += static_cast<int>(nodeOff); }
      out->blas.push_back(nd);
    }
    std::vector<Node>().swap(mb.blas);
    const int proto = static_cast<int>(protoBox.size());
    protoBox.push_back({mb.lo[0], mb.lo[1], mb.lo[2], mb.hi[0], mb.hi[1], mb.hi[2]});
    out->prototypes.push_back(HostPrototype{triOff, mbTriCount});
    const size_t np = mb.instTint.size() / 4;
    for (size_t k = 0; k < np && isrc.size() < instCap; ++k) {
      InstSrc s{};
      std::memcpy(s.o2w, &mb.instO2W[k * 12], sizeof(s.o2w));
      s.tint[0] = mb.instTint[k * 4 + 0];
      s.tint[1] = mb.instTint[k * 4 + 1];
      s.tint[2] = mb.instTint[k * 4 + 2];
      s.tint[3] = mb.instTint[k * 4 + 3];
      s.blasRoot = blasRoot;
      s.proto = proto;
      s.meshIndex = sourceSceneMesh[mbi];
      isrc.push_back(s);
      if (recordRefit) {
        RefitInstanceMap im;
        if (sourceSceneMesh[mbi] >= 0)
          im.sceneMesh = static_cast<size_t>(sourceSceneMesh[mbi]);
        im.sceneInstance = k;
        im.instanced = sourceSceneMesh[mbi] >= 0 &&
            !scene.meshes[static_cast<size_t>(sourceSceneMesh[mbi])]
                 .instanceXforms.empty();
        refitOut->instances.push_back(im);
      }
    }
    if (np > 0 && isrc.size() >= instCap) out->truncated = true;
    }
  }
  auto tA = Clock::now();

  // Phase B2: per-instance matrix math in parallel (instId == output index, so the
  // result is identical to the serial sequential assignment).
  std::vector<float> instAabb(isrc.size() * 6);
  out->instances.resize(isrc.size());
  out->instancePrototype.resize(isrc.size());
  ParallelFor(isrc.size(), [&](size_t i) {
    const InstSrc& s = isrc[i];
    Inst& I = out->instances[i];
    for (int j = 0; j < 12; ++j) I.o2w[j] = s.o2w[j];
    Affine3x4Inverse(s.o2w, I.w2o);
    I.tint[0] = s.tint[0]; I.tint[1] = s.tint[1];
    I.tint[2] = s.tint[2]; I.tint[3] = s.tint[3];
    I.blasRoot = s.blasRoot;
    I.instId = static_cast<int>(i);
    I.directLightMask = RtLightCollectionMaskForMesh(
        scene.lights, s.meshIndex, false);
    I.shadowLightMask = RtLightCollectionMaskForMesh(
        scene.lights, s.meshIndex, true);
    out->instancePrototype[i] = static_cast<uint32_t>(s.proto);
    const auto& box = protoBox[s.proto];
    float wlo[3], whi[3];
    O2WAabb(s.o2w, box.data(), box.data() + 3, wlo, whi);
    for (int k = 0; k < 3; ++k) { instAabb[i * 6 + k] = wlo[k]; instAabb[i * 6 + 3 + k] = whi[k]; }
  });
  std::vector<int> geometryOriginalInstances(scene.lights.size(), -1);
  for (size_t li = 0; li < scene.lights.size(); ++li) {
    if (scene.lights[li].type != DrawLightCPU::Type::Geometry ||
        scene.lights[li].geometryMesh < 0 ||
        static_cast<size_t>(scene.lights[li].geometryMesh) >= scene.meshes.size()) {
      continue;
    }
    for (size_t ii = 0; ii < isrc.size(); ++ii) {
      if (isrc[ii].meshIndex == scene.lights[li].geometryMesh) {
        geometryOriginalInstances[li] = static_cast<int>(ii);
        break;
      }
    }
  }
  isrc.clear();
  isrc.shrink_to_fit();

  out->triCount = out->tris.size() / 9;
  out->instCount = out->instances.size();
  if ((out->triCount == 0 || out->instCount == 0) && out->pointCount == 0) {
    if (err) *err = "no geometry";
    return false;
  }
  out->blasNodeCount = out->blas.size();

  setPhase(2, out->instCount);  // TLAS
  // TLAS over instance world AABBs (reorders the instance table to leaf order).
  std::vector<float> tcent(out->instCount * 3);
  for (size_t i = 0; i < out->instCount; ++i)
    for (int k = 0; k < 3; ++k)
      tcent[i * 3 + k] = 0.5f * (instAabb[i * 6 + k] + instAabb[i * 6 + 3 + k]);
  std::vector<int> tidx(out->instCount);
  for (size_t i = 0; i < out->instCount; ++i) tidx[i] = static_cast<int>(i);
  out->tlas = BuildTlas(tidx, static_cast<int>(out->instCount), tcent, instAabb);
  out->tlasNodeCount = out->tlas.size();
  std::vector<Inst> ordered(out->instCount);
  for (size_t i = 0; i < out->instCount; ++i) ordered[i] = out->instances[tidx[i]];
  out->instances.swap(ordered);

  std::vector<DrawLightCPU> packedLights = scene.lights;
  for (size_t li = 0; li < packedLights.size(); ++li) {
    DrawLightCPU& light = packedLights[li];
    if (light.type != DrawLightCPU::Type::Geometry || light.geometryMesh < 0 ||
        static_cast<size_t>(light.geometryMesh) >= sourceTriOffsets.size()) {
      continue;
    }
    const size_t mesh = static_cast<size_t>(light.geometryMesh);
    if (sourceTriCounts[mesh] == 0) continue;
    const int original = geometryOriginalInstances[li];
    if (original < 0) continue;
    for (size_t orderedIndex = 0; orderedIndex < tidx.size(); ++orderedIndex) {
      if (tidx[orderedIndex] == original) {
        light.geometryTriOffset = static_cast<int>(sourceTriOffsets[mesh]);
        light.geometryTriCount = static_cast<int>(sourceTriCounts[mesh]);
        light.geometryInstance = static_cast<int>(orderedIndex);
        break;
      }
    }
  }

  // UsdVol volumes.
  for (const DrawVolumeCPU& dv : scene.volumes) {
    const size_t n = size_t(dv.dim[0]) * size_t(dv.dim[1]) * size_t(dv.dim[2]);
    if (n == 0 || dv.density.size() < n) continue;
    float o2w[12], w2o[12];
    Mat4ToO2W(dv.world, o2w);
    if (!Affine3x4Inverse(o2w, w2o)) continue;
    HostVolParam vp{};
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 4; c++) vp.invModel[c * 4 + r] = w2o[r * 4 + c];
    vp.invModel[3] = 0.f; vp.invModel[7] = 0.f; vp.invModel[11] = 0.f; vp.invModel[15] = 1.f;
    for (int a = 0; a < 3; a++) {
      vp.bmin[a] = dv.aabbMin[a]; vp.bmax[a] = dv.aabbMax[a];
      vp.albedo[a] = dv.albedo[a]; vp.emission[a] = dv.emission[a]; vp.dim[a] = dv.dim[a];
    }
    vp.dim[3] = static_cast<int>(out->volDens.size());
    vp.albedo[3] = dv.densityScale;
    vp.emission[3] = dv.background;
    out->volDens.insert(out->volDens.end(), dv.density.begin(), dv.density.begin() + n);
    out->volParams.push_back(vp);
  }
  out->numVols = static_cast<int>(out->volParams.size());

  if (std::getenv("LUSDVIEW_RT_TIMING")) {
    const auto bytes = [](size_t n, size_t element) { return n * element; };
    const size_t geometryBytes =
        bytes(out->tris.size() + out->nrms.size() + out->cols.size() +
                  out->uv.size() + out->uv1.size() + out->infl.size() +
                  out->domw.size(), sizeof(float)) +
        bytes(out->geo.size(), sizeof(uint8_t));
    const size_t bvhBytes =
        bytes(out->blas.size() + out->tlas.size(), sizeof(Node)) +
        bytes(out->instances.size(), sizeof(Inst));
    const size_t pointBytes =
        bytes(out->pointCenters.size() + out->pointMajorAxes.size() +
                  out->pointNormals.size() + out->pointRadii.size() +
                  out->pointColors.size(), sizeof(float)) +
        bytes(out->pointOrder.size(), sizeof(int)) +
        bytes(out->pointBvh.size(), sizeof(Node)) +
        bytes(out->pointChunks.size(), sizeof(PointBvhChunk));
    const size_t materialBytes =
        bytes(out->mat.size() + out->backMat.size() + out->face.size() +
                  out->domj.size() + out->matTex.size(), sizeof(int)) +
        bytes(out->matTexParam.size() + out->matPbr.size() + out->matBase.size() +
                  out->matLightRt.size() + out->lightParams.size(), sizeof(float));
    const size_t textureBytes =
        bytes(out->texels.size(), sizeof(uint8_t)) +
        bytes(out->textures.size(), sizeof(HostTextureDesc));
    const size_t volumeBytes =
        bytes(out->volDens.size(), sizeof(float)) +
        bytes(out->volParams.size(), sizeof(HostVolParam));
    const size_t totalBytes = geometryBytes + bvhBytes + pointBytes +
                              materialBytes + textureBytes + volumeBytes;
    const auto mib = [](size_t n) {
      return static_cast<double>(n) / (1024.0 * 1024.0);
    };
    std::fprintf(stderr,
                 "[rt_scene_build] host residency: geometry %.1f MiB, "
                 "bvh/instances %.1f MiB, analytic points %.1f MiB, "
                 "materials %.1f MiB, textures %.1f MiB, volumes %.1f MiB, "
                 "total %.1f MiB\n",
                 mib(geometryBytes), mib(bvhBytes), mib(pointBytes),
                 mib(materialBytes), mib(textureBytes), mib(volumeBytes),
                 mib(totalBytes));
  }

  // Per-material PBR scalars.
  auto tB = Clock::now();
  if (std::getenv("LUSDVIEW_RT_TIMING"))
    std::fprintf(stderr,
                 "[rt_scene_build] %zu meshes -> %zu tris / %zu instances: "
                 "phaseA(geom) %lld ms, phaseB(assemble+TLAS) %lld ms\n",
                 scene.meshes.size(), out->triCount, out->instCount,
                 (long long)ms(t0, tA), (long long)ms(tA, tB));

  out->numMats = static_cast<int>(scene.materials.size());
  out->matPbr.assign(std::max<size_t>(scene.materials.size(), 1) * 6, 0.0f);
  out->matBase.assign(std::max<size_t>(scene.materials.size(), 1) * 3, 0.6f);
  out->matLightRt.assign(std::max<size_t>(scene.materials.size(), 1) *
                             kLightRtOpenPBRFloats,
                         0.0f);
  out->matGraph.assign(std::max<size_t>(scene.materials.size(), 1) *
                           kRtMaterialGraphFloats,
                       0.0f);
  HostTextureTable textureTable;
  BuildHostTextureTable(scene.textures, scene.materials, &textureTable,
                        &scene.lights, textureBudgetBytes);
  // Runtime domes are baked directly to IBL data and therefore have no entry
  // in DrawScene::textures. Append their compact RGBE latlong here so CUDA/HIP
  // can render HDR misses/reflections without duplicating the float cubemap.
  std::vector<int> packedDomeTextures(packedLights.size(), -1);
  for (size_t li = 0; li < packedLights.size(); ++li) {
    const DrawLightCPU& light = packedLights[li];
    if (light.type != DrawLightCPU::Type::Dome || light.envmapTexture >= 0 ||
        light.ibl.envLatlongWidth <= 0 || light.ibl.envLatlongHeight <= 0 ||
        light.ibl.envLatlongRgbe.empty())
      continue;
    const size_t bytes = light.ibl.envLatlongRgbe.size();
    if (textureBudgetBytes > 0 &&
        (bytes > textureBudgetBytes || textureTable.texels.size() >
             textureBudgetBytes - bytes))
      continue;
    if (textureTable.texels.size() > size_t(std::numeric_limits<int>::max()) ||
        bytes > size_t(std::numeric_limits<int>::max()) - textureTable.texels.size())
      continue;
    HostTextureDesc td;
    td.offset = static_cast<int>(textureTable.texels.size());
    td.width = light.ibl.envLatlongWidth;
    td.height = light.ibl.envLatlongHeight;
    td.wrapS = static_cast<int>(WrapMode::Repeat);
    td.wrapT = static_cast<int>(WrapMode::ClampToEdge);
    td.srgb = 0;
    for (int& layer : td.udimLayer) layer = -1;
    packedDomeTextures[li] = static_cast<int>(textureTable.textures.size());
    textureTable.texels.insert(textureTable.texels.end(),
                               light.ibl.envLatlongRgbe.begin(),
                               light.ibl.envLatlongRgbe.end());
    textureTable.textures.push_back(td);
  }
  out->texels = std::move(textureTable.texels);
  out->textures = std::move(textureTable.textures);
  out->matTex = std::move(textureTable.matTex);
  out->matTexParam = std::move(textureTable.matTexParam);
  out->numTextures = static_cast<int>(out->textures.size());
  auto mapTex = [&](int t) -> int {
    return (t >= 0 &&
            static_cast<size_t>(t) < textureTable.sourceToTable.size())
               ? textureTable.sourceToTable[static_cast<size_t>(t)]
               : -1;
  };
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const DrawMaterialCPU& dm = scene.materials[i];
    PackRtMaterialConstants(dm, &out->matPbr[i * 6], &out->matBase[i * 3],
                            &out->matLightRt[i * kLightRtOpenPBRFloats]);
    PackMaterialXGraphRuntime(
        dm, &out->matGraph[i * kRtMaterialGraphFloats],
        &textureTable.sourceToTable);
  }

  out->numLights = static_cast<int>(packedLights.size());
  out->lightParams.assign(std::max<size_t>(packedLights.size(), 1) *
                              kRtLightParamFloats,
                          0.0f);
  for (size_t i = 0; i < packedLights.size(); ++i) {
    const DrawLightCPU& light = packedLights[i];
    const int mapped = light.envmapTexture >= 0
                           ? mapTex(light.envmapTexture)
                           : packedDomeTextures[i];
    PackRtLightParams(light, mapped,
                      &out->lightParams[i * kRtLightParamFloats]);
    if (packedDomeTextures[i] >= 0)
      out->lightParams[i * kRtLightParamFloats + 1] += float(1 << 8);
    if (light.type == DrawLightCPU::Type::Dome && mapped >= 0)
      CachePackedDomeTextureProposal(
          *out, &out->lightParams[i * kRtLightParamFloats]);
  }
  if (recordRefit) refitOut->valid = true;
  return true;
}

bool UpdateHostMaterialConstants(HostScene* scene, int materialId,
                                 const DrawMaterialCPU& material,
                                 std::string* err) {
  if (!scene || materialId < 0 || materialId >= scene->numMats) {
    if (err) *err = "host material index is out of range";
    return false;
  }
  const size_t i = static_cast<size_t>(materialId);
  if (scene->matPbr.size() < (i + 1) * 6 ||
      scene->matBase.size() < (i + 1) * 3 ||
      scene->matLightRt.size() < (i + 1) * kLightRtOpenPBRFloats) {
    if (err) *err = "host material arrays are incomplete";
    return false;
  }
  PackRtMaterialConstants(material, &scene->matPbr[i * 6],
                          &scene->matBase[i * 3],
                          &scene->matLightRt[i * kLightRtOpenPBRFloats]);
  return true;
}

bool RefitHostScene(const DrawScene& scene, const RefitMap& map, HostScene* hs,
                    std::string* err) {
  if (!hs || !map.valid) {
    if (err) *err = "no refit map (scene was not built with refit retained)";
    return false;
  }
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();

  // Topology guard first, across all meshes, so a mismatch never leaves hs
  // half-rewritten (the caller falls back to a full rebuild on false).
  for (const RefitMeshMap& rm : map.meshes) {
    if (rm.sceneMesh >= scene.meshes.size()) {
      if (err) *err = "refit: mesh list changed";
      return false;
    }
    const DrawMeshCPU& m = scene.meshes[rm.sceneMesh];
    const size_t triCount = rm.leafOrder.size();
    if (m.indices.size() / 3 != triCount || m.vertices.empty() ||
        (rm.triOffset + triCount) * 9 > hs->tris.size()) {
      if (err) *err = "refit: mesh topology changed";
      return false;
    }
  }
  if (map.instances.size() != hs->instances.size()) {
    if (err) *err = "refit: instance list changed";
    return false;
  }

  // 1. Rewrite positions + normals in the recorded leaf order (same values a
  //    fresh flatten would emit for this pose; everything else -- uvs, colors,
  //    materials, masks -- is pose-invariant and stays).
  ParallelFor(map.meshes.size(), [&](size_t i) {
    const RefitMeshMap& rm = map.meshes[i];
    const DrawMeshCPU& m = scene.meshes[rm.sceneMesh];
    float worldO2W[12];
    float worldW2O[12];
    Mat4ToO2W(m.world, worldO2W);
    const bool invertible = Affine3x4Inverse(worldO2W, worldW2O);
    const size_t triCount = rm.leafOrder.size();
    for (size_t o = 0; o < triCount; ++o) {
      const size_t s = static_cast<size_t>(rm.leafOrder[o]);
      float* tp = &hs->tris[(rm.triOffset + o) * 9];
      float* np = &hs->nrms[(rm.triOffset + o) * 9];
      for (int k = 0; k < 3; ++k) {
        const DrawVertex& vtx = m.vertices[m.indices[s * 3 + k]];
        tp[k * 3 + 0] = m.world[0] * vtx.px + m.world[4] * vtx.py +
                        m.world[8] * vtx.pz + m.world[12];
        tp[k * 3 + 1] = m.world[1] * vtx.px + m.world[5] * vtx.py +
                        m.world[9] * vtx.pz + m.world[13];
        tp[k * 3 + 2] = m.world[2] * vtx.px + m.world[6] * vtx.py +
                        m.world[10] * vtx.pz + m.world[14];
        if (invertible) {
          np[k * 3 + 0] = worldW2O[0] * vtx.nx + worldW2O[4] * vtx.ny +
                          worldW2O[8] * vtx.nz;
          np[k * 3 + 1] = worldW2O[1] * vtx.nx + worldW2O[5] * vtx.ny +
                          worldW2O[9] * vtx.nz;
          np[k * 3 + 2] = worldW2O[2] * vtx.nx + worldW2O[6] * vtx.ny +
                          worldW2O[10] * vtx.nz;
        } else {
          np[k * 3 + 0] = m.world[0] * vtx.nx + m.world[4] * vtx.ny +
                          m.world[8] * vtx.nz;
          np[k * 3 + 1] = m.world[1] * vtx.nx + m.world[5] * vtx.ny +
                          m.world[9] * vtx.nz;
          np[k * 3 + 2] = m.world[2] * vtx.nx + m.world[6] * vtx.ny +
                          m.world[10] * vtx.nz;
        }
      }
    }
  });

  // 2. Refit BLAS node bounds over the unchanged tree. BuildBvh appends a node
  //    before recursing into its children, so every child index is greater than
  //    its parent's: one reverse sweep computes children before parents. Leaf
  //    `left` is already rebased to the global tri array.
  for (size_t n = hs->blas.size(); n-- > 0;) {
    Node& nd = hs->blas[n];
    if (nd.count > 0) {
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (int t = 0; t < nd.count; ++t) {
        const float* tv = &hs->tris[(static_cast<size_t>(nd.left) + t) * 9];
        for (int v = 0; v < 3; ++v)
          for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], tv[v * 3 + k]);
            hi[k] = std::max(hi[k], tv[v * 3 + k]);
          }
      }
      for (int k = 0; k < 3; ++k) { nd.bmin[k] = lo[k]; nd.bmax[k] = hi[k]; }
    } else {
      const Node& l = hs->blas[static_cast<size_t>(nd.left)];
      const Node& r = hs->blas[static_cast<size_t>(nd.right)];
      for (int k = 0; k < 3; ++k) {
        nd.bmin[k] = std::min(l.bmin[k], r.bmin[k]);
        nd.bmax[k] = std::max(l.bmax[k], r.bmax[k]);
      }
    }
  }

  // 3. Refresh instance transforms and world AABBs. Animated ordinary meshes
  //    use DrawMeshCPU::world; PointInstancer placements use instanceXforms.
  std::vector<float> instAabb(hs->instances.size() * 6);
  ParallelFor(hs->instances.size(), [&](size_t i) {
    Inst& I = hs->instances[i];
    const RefitInstanceMap& im = map.instances[i];
    if (im.sceneMesh != ~size_t(0) && im.sceneMesh < scene.meshes.size()) {
      const DrawMeshCPU& mesh = scene.meshes[im.sceneMesh];
      if (im.instanced) {
        const size_t begin = im.sceneInstance * 12;
        if (begin + 12 <= mesh.instanceXforms.size())
          std::copy_n(mesh.instanceXforms.data() + begin, 12, I.o2w);
      }
      Affine3x4Inverse(I.o2w, I.w2o);
    }
    const Node& root = hs->blas[static_cast<size_t>(I.blasRoot)];
    float wlo[3], whi[3];
    O2WAabb(I.o2w, root.bmin, root.bmax, wlo, whi);
    for (int k = 0; k < 3; ++k) {
      instAabb[i * 6 + k] = wlo[k];
      instAabb[i * 6 + 3 + k] = whi[k];
    }
  });

  // 4. Refit TLAS node bounds (same reverse-sweep argument; a TLAS leaf's
  //    `left` is its first instance in the reordered instance table).
  for (size_t n = hs->tlas.size(); n-- > 0;) {
    Node& nd = hs->tlas[n];
    if (nd.count > 0) {
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (int t = 0; t < nd.count; ++t) {
        const float* a = &instAabb[(static_cast<size_t>(nd.left) + t) * 6];
        for (int k = 0; k < 3; ++k) {
          lo[k] = std::min(lo[k], a[k]);
          hi[k] = std::max(hi[k], a[3 + k]);
        }
      }
      for (int k = 0; k < 3; ++k) { nd.bmin[k] = lo[k]; nd.bmax[k] = hi[k]; }
    } else {
      const Node& l = hs->tlas[static_cast<size_t>(nd.left)];
      const Node& r = hs->tlas[static_cast<size_t>(nd.right)];
      for (int k = 0; k < 3; ++k) {
        nd.bmin[k] = std::min(l.bmin[k], r.bmin[k]);
        nd.bmax[k] = std::max(l.bmax[k], r.bmax[k]);
      }
    }
  }

  if (std::getenv("LUSDVIEW_RT_TIMING")) {
    std::fprintf(stderr, "[rt_scene_build] refit: %zu meshes, %zu tris: %.2f ms\n",
                 map.meshes.size(), hs->triCount,
                 std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  return true;
}

}  // namespace lusdview
