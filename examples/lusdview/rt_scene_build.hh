// SPDX-License-Identifier: Apache-2.0
// lusdview — shared host-side scene build for the CUDA/HIP screenshot tracers.
// Flattens the DrawScene into world/local-space triangle SoA + per-prototype
// BLAS + a TLAS over instances, ready to upload to the device. The per-mesh
// geometry build (flatten + per-prototype BLAS) is parallelized across meshes;
// the result is byte-identical to a serial build. Both tracers call this and
// then just upload the arrays (the only per-backend difference).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu_scene.hh"  // DrawScene
#include "rt_bvh.hh"     // Node

namespace lusdview {

// 20 vec4 per light: 10 rows of packed DrawLightCPU params, 3 rows holding
// the world->environment rotation (rows of R^T; dome env sampling on miss),
// and 7 rows carrying the order-2 SH irradiance (27 floats, coeff-major RGB;
// the RT surface ambient term).
constexpr int kRtLightParamFloats = 80;
constexpr int kMaxRtLinkedLights = 32;

// Pack a DrawLightCPU into the vec4-friendly RT light layout. `mappedEnvmapTexture`
// is backend-specific: HostScene maps to its compact RT texture table, while
// raster/RT backends may pass their own texture slot id.
void PackRtLightParams(const DrawLightCPU& light, int mappedEnvmapTexture,
                       float* dst);
void PackRtMaterialConstants(const DrawMaterialCPU& material, float* pbr,
                             float* base, float* lightRt);
uint32_t RtLightCollectionMaskForMesh(const std::vector<DrawLightCPU>& lights,
                                      int meshIndex, bool shadow);

// Resolve the same bounded stage-order collection mask for a native point or
// curve carrier, which has an authored USD path but no mesh index.
uint32_t RtLightCollectionMaskForPath(const std::vector<DrawLightCPU>& lights,
                                      const std::string& carrierPath,
                                      bool shadow);

// Live progress for a (possibly background-threaded) scene build. Polled by the
// UI to show a responsive progress overlay during the multi-second build.
struct BuildProgress {
  std::atomic<int> phase{0};        // 0 geometry, 1 assemble, 2 TLAS, 3 upload, 4 done
  std::atomic<size_t> done{0};      // items completed in the current phase
  std::atomic<size_t> total{0};     // items in the current phase (0 = indeterminate)
  static const char* phaseName(int p) {
    switch (p) {
      case 0: return "building geometry";
      case 1: return "assembling scene";
      case 2: return "building TLAS";
      case 3: return "uploading to GPU";
      default: return "finalizing";
    }
  }
};

// Per-instance record (must match `Inst` in the trace kernel: all 4-byte fields).
struct Inst {
  float w2o[12];   // world->object (affine inverse of o2w)
  float o2w[12];   // object->world (row-major 3x4)
  float tint[4];   // per-instance color/opacity
  int blasRoot;    // global node index of this instance's BLAS root
  int instId;      // stable instance id (instance-id AOV)
  uint32_t directLightMask;
  uint32_t shadowLightMask;
};

// Stable prototype geometry span for hardware RT acceleration builders.
// `triOffset`/`triCount` address HostScene's triangle-parallel arrays; instances
// refer to this table through HostScene::instancePrototype without changing the
// packed software-kernel Inst ABI.
struct HostPrototype {
  size_t triOffset{0};
  size_t triCount{0};
};

// Per-volume params (must match `VolParam` in the trace kernel).
struct HostVolParam {
  float invModel[16];
  float bmin[4];
  float bmax[4];
  int dim[4];  // .xyz dims, .w = float offset into volDens
  float albedo[4];
  float emission[4];
};

// Compact RT texture descriptor. offset indexes HostScene::texels as RGBA8 bytes.
struct HostTextureDesc {
  int offset{0};
  int width{0};
  int height{0};
  int wrapS{0};
  int wrapT{0};
  int srgb{0};
  int isUdim{0};
  int isPtex{0};
  int ptexCols{0};
  int ptexRows{0};
  int ptexTileEdge{0};
  int ptexRectTexelOffset{0};
  int ptexFaceCount{0};
  int mipCount{1};       // levels including this descriptor's base level
  int firstMip{-1};      // descriptor id of level 1; consecutive thereafter
  int imageSlot{-1};     // source VkImage slot for direct compressed Ptex sampling
  int udimLayer[100]{};  // texture descriptor ids for tiles 1001..1100
};

struct HostTextureTable {
  std::vector<uint8_t> texels;
  std::vector<HostTextureDesc> textures;
  std::vector<int> matTex;
  std::vector<float> matTexParam;
  std::vector<int> sourceToTable;
};

// Generic float/vector primvars expanded to triangle-corner order. Values are
// components floats per triangle vertex, so a value at hit barycentrics can be
// reconstructed without retaining DrawMeshCPU objects.
struct HostGeomProp {
  std::string name;
  uint32_t components{0};
  std::vector<float> values;
};

// Device-neutral lookup table for the CUDA/HIP graph interpreter. Values are
// stored as triangle-corner triples, so valueOffset is a float offset into the
// packed value stream and each triangle consumes 3 * components entries.
struct HostGeomPropDesc {
  uint32_t hash{0};
  uint32_t components{0};
  uint32_t valueOffset{0};
};

// A bounded analytic-Gaussian BVH range. Point attributes remain in one
// compact global stream; each range owns a local BVH root and order span so
// backends can build/traverse large fields incrementally without one monolithic
// point-BVH working set.
struct PointBvhChunk {
  int first{0};
  int count{0};
  int orderFirst{0};
  int bvhFirst{0};
  int bvhCount{0};
};

// Build the backend-neutral mipmapped texture table used by CUDA/HIP and
// Vulkan ray query. Handles decoded images, compressed-only inputs, sparse
// UDIM tiles, semantic material slots, UV transforms, and channel metadata.
void BuildHostTextureTable(const std::vector<DrawTextureCPU>& sourceTextures,
                           const std::vector<DrawMaterialCPU>& materials,
                           HostTextureTable* out,
                           const std::vector<DrawLightCPU>* lights = nullptr,
                           size_t maxTexelBytes = 0);

// Build camera-independent solid approximations for Points and Curves. RT
// backends consume these; raster backends retain the original carriers and
// generate camera-facing billboards/ribbons at draw time.
// Build triangle proxies for point/curve carriers. Compact mode keeps the
// carrier coverage while using fewer tessellation segments for bounded RT
// previews; full mode preserves the higher-quality legacy proxy.
std::vector<DrawMeshCPU> BuildNonMeshRtProxyMeshes(const DrawScene& scene,
                                                  bool compact = false,
                                                  size_t maxTriangles = 0);

// Fully-built host scene, device-upload ready. Arrays mirror the kernel inputs.
struct HostScene {
  // Native Gaussian ellipse carriers: centers, major axes, normals, radii and
  // RGBA display color are parallel by point. These bypass triangle proxies.
  std::vector<float> pointCenters, pointMajorAxes, pointNormals;
  std::vector<float> pointRadii, pointColors;
  std::vector<int> pointOrder;
  std::vector<Node> pointBvh;
  std::vector<PointBvhChunk> pointChunks;
  size_t pointCount{0};
  // cols is RGBA per triangle vertex: displayColor.rgb + displayOpacity.
  std::vector<float> tris, nrms, cols, uv, uv1, infl, domw;
  std::vector<HostGeomProp> geomProps;
  std::vector<uint8_t> geo;
  // Per-triangle wireframe edge mask (bit0: edge v1v2, bit1: edge v2v0, bit2: edge
  // v0v1 is an original polygon edge). Lets the RT wireframe draw quad/ngon edges
  // and skip triangulation diagonals.
  std::vector<uint8_t> emask;
  std::vector<int> mat, face, domj;
  // Optional back-face material id per triangle. Empty when the scene has no
  // distinct back binding; entries < 0 fall back to mat. Keeping this sparse at
  // scene level avoids another 4 B/triangle on ordinary large scenes.
  std::vector<int> backMat;
  std::vector<Node> blas;       // BLAS nodes, rebased to the global arrays
  std::vector<Node> tlas;       // TLAS nodes (root at 0)
  std::vector<Inst> instances;  // leaf-order (matches the TLAS)
  std::vector<HostPrototype> prototypes;
  std::vector<uint32_t> instancePrototype;
  std::vector<float> matPbr;
  std::vector<float> matBase;  // 3 floats/material; base color constant
  // 80 floats/material: vec4-friendly LightRT/OpenPBR constant fallback.
  // See lightrt_mtlx_bridge.hh PackLightRtOpenPBR.
  std::vector<float> matLightRt;
  // Packed MaterialX graph IR, kRtMaterialGraphFloats per material.
  std::vector<float> matGraph;
  // kRtMaterialTexSlots semantic slots/material. Packed inputs may map several
  // semantics to one texture; see lightrt_mtlx_bridge.hh for the stable order.
  std::vector<int> matTex;
  // UV affine rows, scale/bias vectors and scalar channel selectors. See
  // lightrt_mtlx_bridge.hh PackRtMaterialTextureParams.
  std::vector<float> matTexParam;
  int numMats = 0;
  std::vector<uint8_t> texels;
  std::vector<HostTextureDesc> textures;
  int numTextures = 0;
  // kRtLightParamFloats/light: type/flags/texture ids, transform basis,
  // derived radiance, shape size, shaping, shadow, dome metadata, IES LUTs,
  // and resolved GeometryLight triangle ranges.
  std::vector<float> lightParams;
  int numLights = 0;
  std::vector<float> volDens;
  std::vector<HostVolParam> volParams;
  int numVols = 0;
  size_t triCount = 0, instCount = 0, blasNodeCount = 0, tlasNodeCount = 0;
  bool truncated = false;
};

bool UpdateHostMaterialConstants(HostScene* scene, int materialId,
                                 const DrawMaterialCPU& material,
                                 std::string* err);

// Refit support: enough of the build's mapping to re-pose an existing
// HostScene in place when only VERTEX DATA changed (skin/morph re-pose; same
// meshes, same topology, same worlds). Recorded by BuildHostScene on request.
struct RefitMeshMap {
  size_t sceneMesh{0};        // index into DrawScene.meshes
  size_t triOffset{0};        // this mesh's block in HostScene.tris (tri index)
  std::vector<int> leafOrder; // output slot i holds original triangle leafOrder[i]
};
struct RefitInstanceMap {
  size_t sceneMesh{~size_t(0)};
  size_t sceneInstance{0};
  bool instanced{false};
};
struct RefitMap {
  bool valid{false};
  std::vector<RefitMeshMap> meshes;
  std::vector<RefitInstanceMap> instances;
};

// Build `out` from `scene`. `maxTris` caps unique prototype triangles, `maxInstances`
// caps the instance count (0 = unlimited). `displacementScale` bakes coarse
// UsdPreviewSurface displacement into the traced geometry (0 = none). Returns
// false (with *err) only when the scene has no triangles/instances.
// `refitOut` (optional) records the tri permutation per mesh so RefitHostScene
// can later re-pose `out` without a rebuild; only recorded when
// displacementScale == 0 (a displaced flatten cannot be refit -- it re-samples
// textures per pose).
bool BuildHostScene(const DrawScene& scene, size_t maxTris, size_t maxInstances,
                    float displacementScale, HostScene* out, std::string* err,
                    BuildProgress* progress = nullptr, RefitMap* refitOut = nullptr,
                    size_t textureBudgetBytes = 0,
                    const std::vector<uint8_t>* meshVisible = nullptr,
                    uint32_t purposeVisibleMask = 0x3u);

// Re-pose `hs` in place from `scene`'s CURRENT vertex data: rewrite tris/nrms
// in the recorded leaf order, then refit every BLAS/TLAS node bound over the
// UNCHANGED tree topology (children are appended after their parent by the
// builders, so one reverse-index sweep computes children before parents).
// Instance transforms are refreshed from the current DrawScene before instance
// AABBs are re-derived, so the same map supports object-transform animation as
// well as vertex deformation.
// Returns false (with *err) when the map is invalid or topology changed;
// the caller should fall back to a full build.
bool RefitHostScene(const DrawScene& scene, const RefitMap& map, HostScene* hs,
                    std::string* err);

}  // namespace lusdview
