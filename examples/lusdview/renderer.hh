// SPDX-License-Identifier: Apache-2.0
// lusdview - abstract Renderer interface (the GL/Vulkan boundary).
//
// Both backends render the 3D scene into an *offscreen* color target and expose
// it as an opaque texture handle. The GUI shows that handle via ImGui::Image in
// the "Viewport" dock window, so the 3D view participates in docking. All
// backend-specific ImGui wiring (NewFrame/Init/RenderDrawData, swap/present)
// lives behind this interface so the app main-loop is backend-agnostic.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "gpu_scene.hh"
#include "path_trace.hh"
#include "scene_validation.hh"
#include "rt_camera.hh"
#include "rt_lod.hh"  // RtLodCamera (view-dependent RT LOD)

struct GLFWwindow;
struct ImDrawData;

namespace lusdview {

// Completion record shared between App's MCP state and a renderer-owned
// asynchronous Vulkan pipeline build. Writers fill the ordinary fields before
// publishing `done` with release ordering; readers acquire-load `done` first.
struct AsyncShaderReloadResult {
  std::atomic<bool> done{false};
  bool ok{false};
  double elapsedMs{0.0};
  std::string error;
};

enum class Backend { GL, Vulkan };
enum class TransparencyMode { Auto, Weighted, Sorted };

// Thread-safe renderer snapshot for runtime transparency requests.
struct TransparencyStatus {
  TransparencyMode requested{TransparencyMode::Auto};
  bool supported{false};
  bool active{false};
  std::string phase{"idle"};
  std::string error;
  double compileMs{0};
  uint64_t attachmentBytes{0};
};

// Existing-slot update prepared by the application. Pointers remain valid for
// the duration of applyMeshSlotUpdatesTransactional(). A backend must validate
// the complete batch before changing any slot and return false without visible
// mutation when it cannot commit the batch.
struct MeshSlotUpdate {
  size_t meshIndex{0};
  const std::vector<DrawVertex>* vertices{nullptr};
  const float* world{nullptr};
  const std::vector<float>* instanceXforms{nullptr};
  const std::vector<float>* instanceColors{nullptr};
  const std::vector<float>* instanceOpacities{nullptr};
};

struct MeshSlotReplacement {
  size_t meshIndex{0};
  const DrawMeshCPU* mesh{nullptr};
};

struct MaterialSlotUpdate {
  size_t materialIndex{0};
  const DrawMaterialCPU* material{nullptr};
};

struct TextureSlotReplacement {
  size_t textureIndex{0};
  const DrawTextureCPU* texture{nullptr};
};

// Return the logical dimensions addressed by a streamed texture-region update
// at mipLevel. Keeping this calculation in the renderer ABI makes the Vulkan
// bounds checks and CPU-side regression tests agree on NPOT dimensions.
inline bool TextureMipDimensions(int width, int height, int mipLevels,
                                 int mipLevel, int* mipWidth,
                                 int* mipHeight) {
  if (width <= 0 || height <= 0 || mipLevels <= 0 || mipLevel < 0 ||
      mipLevel >= mipLevels || !mipWidth || !mipHeight) {
    return false;
  }
  *mipWidth = std::max(1, width >> mipLevel);
  *mipHeight = std::max(1, height >> mipLevel);
  return true;
}

// User-facing render-technique selector for the runtime backend switch (View
// menu + CPU RT keybinding). Decomposes into a window-owning Backend (which
// Renderer subclass owns the GLFW window/GPU context) plus an OverlayKind
// (what actually draws into it): GLRaster/VulkanRaster select the owner with
// no overlay; VulkanRT is Renderer::setRayTracing() inside the owner's normal
// renderFrame(); CudaRT/HipRT/CpuRT trace externally and composite via
// uploadViewportImage() on top of whichever owner (GL or Vulkan) is active.
enum class RenderTechnique {
  GLRaster,
  VulkanRaster,
  VulkanRT,
  CudaRT,
  HipRT,
  CpuRT,
  VulkanPathTrace,
  CudaPathTrace,
  HipPathTrace
};
enum class OverlayKind {
  None,
  VulkanRT,
  CudaRT,
  HipRT,
  CpuRT,
  VulkanPathTrace,
  CudaPathTrace,
  HipPathTrace
};

enum class CudaRtBackend { Auto, SoftwareBvh, Optix };

// GLRaster/VulkanRaster/VulkanRT mandate a specific window owner; CudaRT/
// HipRT/CpuRT are owner-agnostic overlays that run on top of whichever owner
// (GL or Vulkan) is already active. Returns true and sets *owner for the
// former group; returns false (owner left as-is) for the latter.
inline bool TechniqueRequiresOwner(RenderTechnique t, Backend* owner) {
  switch (t) {
    case RenderTechnique::GLRaster:
      if (owner) *owner = Backend::GL;
      return true;
    case RenderTechnique::VulkanRaster:
    case RenderTechnique::VulkanRT:
    case RenderTechnique::VulkanPathTrace:
      if (owner) *owner = Backend::Vulkan;
      return true;
    default:
      return false;
  }
}
inline OverlayKind OverlayForTechnique(RenderTechnique t) {
  switch (t) {
    case RenderTechnique::VulkanRT: return OverlayKind::VulkanRT;
    case RenderTechnique::CudaRT: return OverlayKind::CudaRT;
    case RenderTechnique::HipRT: return OverlayKind::HipRT;
    case RenderTechnique::CpuRT: return OverlayKind::CpuRT;
    case RenderTechnique::VulkanPathTrace: return OverlayKind::VulkanPathTrace;
    case RenderTechnique::CudaPathTrace: return OverlayKind::CudaPathTrace;
    case RenderTechnique::HipPathTrace: return OverlayKind::HipPathTrace;
    default: return OverlayKind::None;
  }
}
inline const char* RenderTechniqueLabel(RenderTechnique t) {
  switch (t) {
    case RenderTechnique::GLRaster: return "GL Raster";
    case RenderTechnique::VulkanRaster: return "Vulkan Raster";
    case RenderTechnique::VulkanRT: return "Vulkan RT";
    case RenderTechnique::CudaRT: return "CUDA RT";
    case RenderTechnique::HipRT: return "HIP RT";
    case RenderTechnique::CpuRT: return "CPU RT";
    case RenderTechnique::VulkanPathTrace: return "Vulkan Path Trace";
    case RenderTechnique::CudaPathTrace: return "CUDA Path Trace";
    case RenderTechnique::HipPathTrace: return "HIP Path Trace";
  }
  return "?";
}

struct RendererDevicePreference {
  // Vulkan device selector. Empty = automatic. Non-empty accepts either a
  // physical-device index ("0", "1", ...) or a case-insensitive substring of
  // the device name / driver name / driver info.
  std::optional<std::string> vulkanDevice;
};

// Shaded/Wireframe + debug AOVs. Normals = the shading normal used by the lit path;
// GeomNormal = the geometric face normal; Uv = texcoord set 0; Depth = camera
// distance normalized by RenderFrameParams::depthScale.
enum class RenderMode : int {
  Shaded = 0,
  Wireframe = 1,
  Normals = 2,       // shading normal
  MaterialId = 3,
  GeomNormal = 4,    // geometric face normal
  Uv = 5,            // texcoord set 0
  Depth = 6,         // camera distance / scene-bbox diagonal
  Albedo = 7,        // unlit base color (material baseColor x displayColor)
  Facing = 8,        // front (green) / back (red) by face orientation
  Roughness = 9,
  Metallic = 10,
  Emissive = 11,
  Opacity = 12,
  Position = 13,     // world position normalized to scene bbox
  Barycentric = 14,
  PrimId = 15,       // triangle id (hashed color)
  MeshId = 16,       // per-mesh/prim id (hashed color)
  Facing2 = 17,      // reserved
  Purpose = 18,      // default/render/proxy/guide
  MissingNormals = 19,  // meshes with no authored normals
  DoubleSided = 20,
  SkinWeights = 21,  // dominant joint (hashed) tinted by weight
  Tangent = 22,
  UvChecker = 23,
  AmbientOcclusion = 24,  // ray-traced (RT backends)
  Curvature = 25,         // screen-space normal variation (raster)
  InstanceId = 26,        // per-instance hashed color (instanced geom)
  BvhHeatmap = 27,        // BVH traversal cost (CUDA software tracer)
  SoftShadow = 28,        // ray-traced soft shadow / sky visibility (RT backends)
  Kind = 29,              // USD model kind (component/group/assembly/subcomponent)
  UdimTile = 30,          // UDIM tile id from UV set 0 (hashed color)
  Uv1 = 31,               // texcoord set 1 (multi-UV; raster)
  BlendInfluence = 32,    // per-vertex blendshape displacement magnitude (raster)
  TexelDensity = 33,      // UV-to-world area ratio (view-independent texel density)
  SourceFaceId = 34,      // original USD face id before triangulation (hashed)
  CoatNormal = 35,        // independently authored coat-layer shading normal
  CoatWeight = 36,        // evaluated coat-layer scalar weight
  CoatColor = 37,         // evaluated coat-layer tint
  CoatRoughness = 38,     // evaluated coat-layer roughness
  SpecularF0 = 39,        // evaluated specular-workflow reflectance
  IorF0 = 40,             // dielectric F0 derived from authored IOR
};
enum class SkinningMode : int { Auto = 0, CPU = 1, GPU = 2 };

// Unlit, vertex-colored line vertex for debug helpers (grid, axes, bboxes).
// Drawn as GL_LINES / VK_PRIMITIVE_TOPOLOGY_LINE_LIST in world space.
struct HelperVertex {
  float pos[3];
  float col[3];
};

struct RendererCaps {
  const char* backend_name{""};
  std::string gpu_name;
  std::string api_info;
  bool usesZeroToOneDepth{false};  // Vulkan clip space Z in [0,1]; GL in [-1,1]
  bool flipViewportV{false};       // GL FBO textures are bottom-up
  // Device class, as the API reports it: "discrete", "integrated", "virtual",
  // "cpu", "other" or "unknown". Tests use this to tell a real GPU from a
  // software rasterizer without pattern-matching driver marketing strings.
  std::string device_type{"unknown"};
  bool supportsRayTracing{false};  // device has the RT extensions (Vulkan only)
  bool supportsGpuSkinning{false};
  bool supportsExtendedGpuSkinning{false};  // texture-backed >4 influences
  bool supportsWeightedOit{false};  // independent blending + FP OIT attachments
  bool usesWeightedOit{false};
  std::string transparencyMode{"sorted"};
  uint64_t oitAttachmentBytes{0};
  uint64_t oitDrawCalls{0};
  uint64_t pipelineBinds{0};
  uint64_t descriptorSetBinds{0};
  std::string rtShaderVariant{"none"};
  double gpuRtMs{0.0};
  double gpuTotalMs{0.0};

  // GPU compressed-texture format support (queried at init). Used to cap-gate
  // the `--texture-compress` mode: a requested format the device can't sample is
  // remapped to a supported one (or uncompressed) before CPU encoding, so e.g.
  // `--texture-compress astc` on a desktop BC-only GPU falls back to BC7.
  bool supportsBC{false};      // S3TC/RGTC/BPTC (BC1/3/5/6H/7) — desktop
  bool supportsASTC{false};    // KHR_texture_compression_astc_ldr — mobile/some
  bool supportsETC2{false};    // ETC2/EAC — GLES3 baseline / mobile
  bool supportsBC5{false};     // RGTC (BC5) — usually with BC
  bool supportsBC6H{false};    // BPTC float (BC6H) — usually with BC7
};

struct RenderFrameParams {
  static constexpr int kMaxClippingPlanes = 8;
  const float* view{nullptr};  // column-major 4x4 (light3d::Mat4 layout)
  const float* proj{nullptr};  // column-major 4x4 (GL: Z[-1,1]; VK: Z[0,1])
  float cameraPos[3]{0, 0, 0};
  float exposure{0.0f};  // photographic exposure in stops (linear multiplier 2^x)
  float materialXTime{0.0f};   // MaterialX time in seconds
  float materialXFrame{0.0f};  // MaterialX frame/timeCode
  RtCameraLens cameraLens;
  RtCameraShutter cameraShutter;
  // A deterministic shutter pose selected by the application for this Vulkan
  // RT frame. While active, camera/pose-driven PV and acceleration-generation
  // changes extend one accumulation instead of resetting it. `reset` starts a
  // new base-view sequence when the timeline/view/shutter contract changes.
  bool rtTemporalPose{false};
  bool rtTemporalReset{false};
  // Authored UsdGeomCamera world-space clipping-plane equations. A fragment is
  // retained when dot(vec4(worldPosition, 1), plane) >= 0. Raster backends
  // support the same portable bounded set; zero disables user clipping.
  float clippingPlanes[kMaxClippingPlanes * 4]{};
  int clippingPlaneCount{0};
  PathTraceSettings pathTrace;
  RenderMode mode{RenderMode::Shaded};
  // Wireframe overlay state, cycled with the 'v' key (GL backend):
  //   0 = off (shaded fill only)
  //   1 = wireframe only (hidden-line: depth-only fill, then polygon edges)
  //   2 = wireframe + shading (shaded fill, then polygon edges on top)
  // Edges are the ORIGINAL polygon edges of the base (pre-tessellation) mesh --
  // triangulation diagonals are dropped via per-triangle source face ids.
  int wireMode{0};
  float clearColor[4]{0.12f, 0.12f, 0.13f, 1.0f};
  float lightDir[3]{0.40160966f, 0.64257544f, 0.48193160f};
  float lightColor[3]{1.0f, 1.0f, 1.0f};
  float depthScale{1.0f};  // Depth AOV: normalize camera distance by this (scene extent)
  float sceneMin[3]{0, 0, 0};     // Position AOV: scene bbox min
  float sceneExtent[3]{1, 1, 1};  // Position AOV: scene bbox size (max-min)
  int highlightMeshIndex{-1};  // draw a wireframe overlay on this mesh (-1 = none)
  // When set, the highlight overlay draws only these triangle vertex indices (a
  // selected GeomSubset of highlightMeshIndex) instead of the whole mesh.
  const uint32_t* highlightIndices{nullptr};
  int highlightIndexCount{0};
  // Selection highlight as world-space orange edge lines (2 verts/segment). The
  // Used for subset selection and backends/modes without a surface-wire pass.
  const HelperVertex* highlightLines{nullptr};
  int highlightLineVertexCount{0};
  // false: selection obeys the scene depth buffer; true: draw through geometry.
  bool highlightXray{false};

  // Debug helper lines (grid / axes / bounding boxes), world space, depth-tested
  // so geometry occludes them.
  const HelperVertex* helperLines{nullptr};
  int helperLineVertexCount{0};  // total vertices (2 per segment)

  // Overlay lines (skeleton bones), world space, drawn on top with depth testing
  // disabled so they stay visible through solid geometry (X-ray).
  const HelperVertex* overlayLines{nullptr};
  int overlayLineVertexCount{0};  // total vertices (2 per segment)

  // Per-mesh visibility mask (index i <-> the i-th appended mesh, same order as
  // highlightMeshIndex). null = all meshes visible. Applies to the raster paths
  // (GL + VK raster). It may include transient view/frustum filtering.
  const uint8_t* meshVisible{nullptr};
  int meshVisibleCount{0};

  // Native carrier visibility mask. Indices are DrawScene::points followed by
  // DrawScene::curves, matching the upload order in uploadScene().
  const uint8_t* carrierVisible{nullptr};
  int carrierVisibleCount{0};

  // Persistent user hide/isolate mask for Vulkan RT. Unlike meshVisible this
  // must not contain frustum culling: off-screen geometry still casts shadows.
  // null or short masks default missing entries to visible.
  const uint8_t* rtMeshVisible{nullptr};
  int rtMeshVisibleCount{0};

  // Visible USD purposes, bit i = PurposeId i (0 default, 1 render, 2 proxy,
  // 3 guide). The VK ray-tracing path leaves meshes of hidden purposes out of
  // the TLAS entirely (a purpose toggle triggers a rebuild). Without this,
  // Caldera's guide breadcrumb planes -- hidden in raster -- engulf the RT
  // camera ("--rt renders near-blank"). Default matches the GUI: render and
  // default visible; proxy and guide hidden.
  uint32_t purposeVisibleMask{0x3u};

  // Surface displacement (UsdPreviewSurface inputs:displacement). When enabled, a
  // material's displacement (constant or height-map red channel) offsets the
  // surface along its normal by displacementScale in the raster vertex/tess shader
  // (coarse, no extra geometry). maxTessLevel > 1 enables GPU tessellation for
  // adaptive sub-triangle detail (0/1 = coarse per-vertex only). Geometric normals
  // are used on displaced surfaces so shading follows the deformed geometry.
  bool displacement{true};
  float displacementScale{1.0f};
  int maxTessLevel{1};
  // Maximum camera-facing ribbon segments emitted per curve strand by raster
  // backends. Zero preserves every tessellated sample. This is independent of
  // loader preview limits: all prims/strands remain selectable while dense
  // basis-curve tessellation is decimated only for drawing.
  int curveMaxSegments{8};
};

// Opaque texture handle for ImGui::Image. GL: a GLuint texture id. Vulkan: a
// VkDescriptorSet. Both fit in 64 bits (ImTextureID is ImU64).
using ViewportTexHandle = uint64_t;

class Renderer {
 public:
  virtual ~Renderer() = default;

  const std::string& sceneUploadError() const { return sceneUploadError_; }

  void clearSceneUploadError() { sceneUploadError_.clear(); }

  // Create device resources. `window` is the GLFW window (Vulkan creates its
  // surface from it; GL assumes its context is already current). A null window
  // requests a windowless/offscreen backend (Vulkan only): no surface, no
  // swapchain — the composited frame is rendered to an offscreen target sized by
  // setHeadlessSize() and retrieved via captureWindow().
  virtual bool init(GLFWwindow* window, std::string* err) = 0;

  // Set the headless composite (full-window) size in pixels. Must be called
  // before init() when `window` is null. No-op for backends that need a window.
  virtual void setHeadlessSize(int /*w*/, int /*h*/) {}

  virtual void setDevicePreference(
      const RendererDevicePreference& /*preference*/) {}
  virtual void setTransparencyMode(TransparencyMode /*mode*/) {}
  virtual TransparencyStatus transparencyStatus() const { return {}; }
  virtual void setRasterQualityHigh(bool /*high*/) {}

  // Resize the headless composite at runtime (recreate the offscreen swap images
  // + framebuffers). Returns false if unsupported / not headless. The caller must
  // also update ImGui's DisplaySize. No-op for backends that need a window.
  virtual bool resizeHeadless(int /*w*/, int /*h*/) { return false; }

  // Wire up the ImGui platform+renderer backends. Call after ImGui::CreateContext().
  virtual bool initImGui(std::string* err) = 0;

  // --- Incremental scene upload (for progressive / lazy loading) ---
  // Reset the GPU scene, set materials, and reserve `textureCount` texture slots
  // (each starts as white until filled by uploadTexture). Materials reference
  // texture slots by index.
  virtual void beginScene(const std::vector<DrawMaterialCPU>& materials,
                          int textureCount) = 0;
  // Grow/update scene resources without clearing meshes already uploaded by a
  // progressive loader. Backends that do not stream may leave this as a no-op.
  virtual void syncSceneResources(
      const std::vector<DrawMaterialCPU>& /*materials*/, int /*textureCount*/) {}
  // Rewrite one existing material's constant data without rebuilding geometry,
  // acceleration structures, or texture tables. Used by the live OpenPBR
  // editor; texture bindings and material indices are unchanged.
  virtual bool updateMaterialConstants(int /*materialId*/,
                                       const DrawMaterialCPU& /*material*/,
                                       std::string* err) {
    if (err) *err = "live material updates are unsupported by this backend";
    return false;
  }
  virtual void setLights(const std::vector<DrawLightCPU>& /*lights*/,
                         size_t /*meshCount*/) {}
  // Append one mesh (uploaded immediately). Rendered from the next frame on.
  virtual void appendMesh(const DrawMeshCPU& mesh) = 0;
  virtual void appendPoints(const DrawPointsCPU& /*points*/) {}
  virtual void appendCurves(const DrawCurvesCPU& /*curves*/) {}
  // Ownership-transfer upload for backends that retain CPU carriers for a
  // later RT build. The default preserves the const-reference behavior used
  // by GL and other non-owning backends.
  virtual void appendPoints(DrawPointsCPU&& points) {
    appendPoints(static_cast<const DrawPointsCPU&>(points));
  }
  virtual void appendCurves(DrawCurvesCPU&& curves) {
    appendCurves(static_cast<const DrawCurvesCPU&>(curves));
  }
  // Progressive surface-first upload. The default preserves existing behavior;
  // GL defers wireframe/source-face buffers until uploadMeshAux.
  virtual void appendMeshSurface(const DrawMeshCPU& mesh) { appendMesh(mesh); }
  virtual void uploadMeshAux(size_t /*meshIndex*/, const DrawMeshCPU& /*mesh*/) {}
  // Append one UsdVol volume (OpenVDB). Default: no-op (backend has no volume
  // support yet; GL implements raymarching, VK/CUDA are placeholders).
  virtual void appendVolume(const DrawVolumeCPU& /*vol*/) {}
  // Fill texture slot `slot`; materials referencing it switch from white to it.
  virtual void uploadTexture(int slot, const DrawTextureCPU& tex) = 0;
  // Return a texture slot to the backend's fallback texture. Implementations
  // must make replacement/destruction safe with respect to submitted frames.
  virtual void evictTexture(int slot) = 0;
  // Approximate live GPU allocation owned by one texture slot. This is the
  // quantity used by the application residency budget (not CPU staging bytes).
  virtual size_t textureResidentBytes(int slot) const = 0;
  // Bound the raw texture table used by native RT backends. Textures beyond
  // the cap remain valid material slots but use the backend's fallback image.
  virtual void setRtTextureBudgetBytes(size_t /*bytes*/) {}
  // Backend-agnostic scene source for host-side BVH builders (rt_scene_build.cc's
  // BuildHostScene(), already used by the CUDA/HIP screenshot tracers). `scene`
  // must outlive the renderer or be re-set/cleared before it doesn't (the App's
  // `draw_` member is stable for the app's lifetime, so callers typically set
  // this once, early, and rely on the renderer re-reading it lazily). Renderers
  // that don't need a host-built BVH (e.g. GL raster, hardware Vulkan RT) can
  // ignore this; the Vulkan compute-BVH fallback path uses it.
  virtual void setHostSceneSource(const DrawScene* /*scene*/) {}
  // Replace an RGBA8 rectangle in an already-uploaded ordinary 2D texture.
  // Used by bounded Ptex page streaming; compressed and array textures reject
  // updates. `rowBytes` permits uploading a sub-rectangle from a larger CPU
  // image without repacking it (zero means tightly packed width*4).
  virtual bool updateTextureRegion(int /*slot*/, int /*x*/, int /*y*/, int /*w*/,
                                   int /*h*/, const uint8_t* /*rgba*/,
                                   size_t /*rowBytes*/ = 0) {
    return false;
  }
  struct TextureRegionUpdate {
    int x{0}, y{0}, width{0}, height{0};
    int mipLevel{0};
    size_t rowBytes{0};
    std::vector<uint8_t> rgba;
    // Optional block-compressed update. `width`/`height` remain logical texel
    // dimensions; the backend derives block-row sizes from the texture format.
    DrawCompressedFormat compressedFormat{DrawCompressedFormat::None};
    std::vector<uint8_t> compressed;
  };
  virtual bool updateTextureRegions(
      int slot, const std::vector<TextureRegionUpdate>& updates) {
    for (const TextureRegionUpdate& update : updates) {
      if (!updateTextureRegion(slot, update.x, update.y, update.width,
                               update.height, update.rgba.data(),
                               update.rowBytes)) {
        return false;
      }
    }
    return true;
  }
  // Update the compact Ptex rectangle table used by compressed Vulkan Ptex
  // images. Backends without a separate table keep their legacy atlas table.
  virtual bool updatePtexFaceRect(int /*slot*/, uint32_t /*face*/,
                                  const DrawPtexFaceRectCPU& /*rect*/) {
    return false;
  }
  virtual void uploadSkinningFrame(const SkinningFrameCPU& /*skin*/) {}
  // Per-instance frustum culling: replace mesh `meshIndex`'s drawn instance set
  // with `count` visible instances (xforms = 12 floats/instance, 3x4 o2w row-major;
  // colors = 3 floats/instance or null to keep the existing per-instance colors;
  // opacities = 1 float/instance or null to keep the existing per-instance opacity).
  // count == instanceCount restores the full set. No-op for non-instanced meshes
  // or backends that flatten instances. Called each frame the view changes.
  virtual void updateInstanceVisibility(size_t /*meshIndex*/,
                                        const float* /*xforms*/,
                                        const float* /*colors*/,
                                        const float* /*opacities*/,
                                        uint32_t /*count*/) {}
  // Install asynchronously generated index-only prototype LODs. LOD0 remains
  // the authored mesh; `levels` contains progressively coarser LOD1+ data.
  virtual void installPrototypeLods(
      size_t /*meshIndex*/,
      const std::vector<DrawPrototypeLodCPU>& /*levels*/) {}
  // The compacted instance arrays are packed by LOD. Backends that do not
  // implement prototype LOD render the complete packed set at LOD0.
  virtual void updateInstanceLodVisibility(
      size_t meshIndex, const float* xforms, const float* colors,
      const float* opacities, const std::array<uint32_t, 4>& lodCounts) {
    const uint32_t count = lodCounts[0] + lodCounts[1] + lodCounts[2] +
                           lodCounts[3];
    updateInstanceVisibility(meshIndex, xforms, colors, opacities, count);
  }
  // Replace mesh `meshIndex`'s vertex buffer in place (same vertex count) — used
  // for per-frame GPU blendshape morph (positions/normals re-derived on the CPU
  // from the rest pose, then GPU-skinned). No-op if unsupported or size differs.
  virtual void updateMeshVertices(int /*meshIndex*/,
                                  const std::vector<DrawVertex>& /*verts*/) {}
  // GPU compute skinning for the RAY-TRACED vertex stream: linear-blend skin the
  // rest pose into the RT vertex buffer on the GPU (then refit the BLAS), given
  // the mesh's composed skinning matrices (`jointCount` matrices of 16 floats,
  // row-major, applied as row-vector p*M; joint attribute ids are absolute and
  // offset by `matrixBase`). `aabbMin/aabbMax` is the caller's conservative
  // posed bound (union of per-joint transformed rest boxes) for LOD/proxy use.
  // Returns false when the backend cannot GPU-skin this mesh (no RT stream, no
  // skin attributes, shader unavailable) — the caller then CPU-skins instead.
  virtual bool updateMeshSkinningGpu(int /*meshIndex*/, const float* /*mats*/,
                                     int /*jointCount*/, int /*matrixBase*/,
                                     const float /*aabbMin*/[3],
                                     const float /*aabbMax*/[3]) {
    return false;
  }
  // Upload mesh `meshIndex`'s per-channel blendshape coefficients (one float per
  // morph channel) for GPU-side morphing in the raster vertex shader. Only the
  // tiny coefficient buffer updates per frame — no vertex re-upload, no GPU stall.
  // No-op on backends that bake morph into geometry (ray tracing) or lack morph.
  virtual void updateMorphWeights(int /*meshIndex*/,
                                  const std::vector<float>& /*coeffs*/) {}
  // Replace mesh `meshIndex`'s world transform (column-major float[16]) — used
  // for per-frame node/xform animation alongside GPU skinning. No-op if
  // unsupported.
  virtual void updateMeshWorld(int /*meshIndex*/, const float /*world*/[16]) {}
  virtual bool applyMeshSlotUpdatesTransactional(
      const std::vector<MeshSlotUpdate>& /*updates*/, std::string* error) {
    if (error) *error = "transactional mesh updates are unsupported";
    return false;
  }
  virtual bool replaceMeshSlotsTransactional(
      const std::vector<MeshSlotReplacement>& /*replacements*/,
      std::string* error) {
    if (error) *error = "transactional mesh replacement is unsupported";
    return false;
  }
  virtual bool applyMaterialUpdatesTransactional(
      const std::vector<MaterialSlotUpdate>& /*updates*/, std::string* error) {
    if (error) *error = "transactional material updates are unsupported";
    return false;
  }
  // Stage every replacement allocation before publishing any texture slot.
  // The first implementation covers ordinary 2D textures; shared UDIM/Ptex
  // lookup resources remain eligible for the full-scene fallback.
  virtual bool replaceTextureSlotsTransactional(
      const std::vector<TextureSlotReplacement>& /*replacements*/,
      std::string* error) {
    if (error) *error = "transactional texture replacement is unsupported";
    return false;
  }
  // Replace an entire mesh (vertices + indices + submeshes) — used by adaptive
  // re-tessellation where the vertex/index count may change. The mesh at
  // `meshIndex` is deleted and re-created with the new data.
  virtual void replaceMesh(int /*meshIndex*/, const DrawMeshCPU& /*mesh*/) {}
  // Number of uploaded meshes. `updateMeshVertices`/`updateMeshWorld` index by
  // DrawScene mesh order, which only matches when this equals the DrawScene
  // mesh count; callers verify before per-index updates.
  virtual int meshCount() const { return 0; }

  // Raster view-dependent LOD box proxies (optimization B). supportsProxyDraw()
  // gates whether the cull emits proxies at all; updateProxyInstances uploads the
  // shared per-frame set (`xforms`: 12 floats/proxy box-fit o2w; `tints`: 3
  // floats/proxy) drawn in one instanced call. Default: unsupported / no-op.
  virtual bool supportsProxyDraw() const { return false; }
  virtual void updateProxyInstances(const float* /*xforms*/, const float* /*tints*/,
                                    uint32_t /*count*/) {}

  // Convenience: upload an entire scene in one call (used by the headless /
  // synchronous path so screenshots are deterministic).
  bool uploadScene(const DrawScene& scene, std::string* err) {
    sceneUploadError_.clear();
    if (!ValidateDrawScene(scene, err)) return false;
    beginScene(scene.materials, static_cast<int>(scene.textures.size()));
    setLights(scene.lights, scene.meshes.size());
    for (size_t i = 0; i < scene.textures.size(); ++i) {
      if (scene.textures[i].deferredDecode) continue;
      uploadTexture(static_cast<int>(i), scene.textures[i]);
    }
    static const bool timeit = std::getenv("LUSDVIEW_TIME_UPLOAD") != nullptr;
    if (timeit) {
      const auto t0 = std::chrono::steady_clock::now();
      auto last = t0;
      for (size_t i = 0; i < scene.meshes.size(); ++i) {
        appendMesh(scene.meshes[i]);
        if (((i + 1) % 5000) == 0 || i + 1 == scene.meshes.size()) {
          const auto now = std::chrono::steady_clock::now();
          const double tot =
              std::chrono::duration<double>(now - t0).count();
          const double dt =
              std::chrono::duration<double>(now - last).count();
          last = now;
          std::fprintf(stderr,
                       "[upload] meshes %zu/%zu  +%.2fs (total %.2fs)\n",
                       i + 1, scene.meshes.size(), dt, tot);
        }
      }
    } else {
      for (const auto& m : scene.meshes) appendMesh(m);
    }
    for (const auto& p : scene.points) appendPoints(p);
    for (const auto& c : scene.curves) appendCurves(c);
    for (const auto& v : scene.volumes) appendVolume(v);
    if (!sceneUploadError_.empty()) {
      if (err) *err = sceneUploadError_;
      return false;
    }
    return true;
  }

  // Resize the offscreen viewport target (dock content size). Clamped to >= 1.
  virtual void resizeViewport(int width, int height) = 0;

  // Per-frame backend ImGui new-frame (ImGui_ImplOpenGL3/Vulkan_NewFrame).
  virtual void newFrame() = 0;

  // Render the 3D scene with `params`. GL renders to its FBO immediately; Vulkan
  // records the parameters and renders during present().
  virtual void renderFrame(const RenderFrameParams& params) = 0;

  // Texture handle of the offscreen color target for ImGui::Image.
  virtual ViewportTexHandle viewportTexture() const = 0;

  // Composite: draw the current ImGui draw data to the window and present/swap.
  // Must be called after ImGui::Render().
  virtual void present() = 0;

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // --- Experimental threaded GL rendering (LUSDVIEW_ENABLE_GL_THREAD) ---
  // Threaded composite: like present() but renders the supplied (deep-copied) ImGui
  // draw data using the framebuffer size queried on the main thread, so it can run
  // on the render thread (glfwGetFramebufferSize is main-thread-only).
  virtual void presentThreaded(ImDrawData* /*drawData*/, int /*fbW*/, int /*fbH*/) {}
  // Threaded init split: the platform half (GLFW callbacks/input) runs on the main
  // thread; the backend half (GL/VK objects) on the render thread which owns the
  // context. Default = combined initImGui (single-thread path). `window` is passed
  // here (not just to init()) because init() runs later on the render thread, so the
  // backend must capture the window handle on the main thread for the GLFW backend.
  virtual bool initImGuiPlatform(GLFWwindow* /*window*/, std::string* /*err*/) { return true; }
  virtual bool initImGuiBackend(std::string* /*err*/) { return true; }
#endif

  // GPU video-memory usage in MB (used, total). Returns false if the backend
  // can't report it. Used by the Stats panel.
  virtual bool gpuMemoryMB(size_t* /*usedMB*/, size_t* /*totalMB*/) const {
    return false;
  }

  // Current offscreen viewport (color target) size in pixels. Default no-op.
  virtual void viewportSize(int* w, int* h) const {
    if (w) *w = 0;
    if (h) *h = 0;
  }

  // Upload an externally-traced RGBA8 image (R8G8B8A8_UNORM, w*h*4, top-down) into
  // the offscreen color target for display this frame. Used by the interactive
  // HIP/CUDA path, which traces on the GPU outside Vulkan: the next present()
  // composites this image instead of running the raster/RT 3D pass. Returns false
  // if unsupported. The flag is consumed by a single present().
  virtual bool uploadViewportImage(const uint8_t* /*rgba*/, int /*w*/, int /*h*/) {
    return false;
  }

  // Read back the offscreen 3D viewport as top-down RGBA8 (headless QA).
  // Returns false if unsupported.
  virtual bool captureViewport(std::vector<uint8_t>* rgba, int* w, int* h) {
    (void)rgba;
    (void)w;
    (void)h;
    return false;
  }
  // Read the unfiltered scene-linear accumulation as top-down RGBA32F. The
  // returned values are averages, not radiance sums.
  virtual bool captureLinearViewport(std::vector<float>* rgba, int* w, int* h) {
    (void)rgba;
    (void)w;
    (void)h;
    return false;
  }

  // Ask the next present() to grab the composited window (back buffer) so it can
  // be retrieved with captureWindow() afterwards. No-op if unsupported.
  virtual void requestWindowCapture() {}

  // Return the window grabbed by the most recent requestWindowCapture()+present(),
  // as top-down RGBA8. Returns false if unsupported / nothing captured.
  virtual bool captureWindow(std::vector<uint8_t>* rgba, int* w, int* h) {
    (void)rgba;
    (void)w;
    (void)h;
    return false;
  }

  virtual const RendererCaps& caps() const = 0;

  // --- Ray tracing (Vulkan only) ---
  // True when the device supports the RT extensions and a ray-tracing technique
  // is available. The GL backend always returns false.
  virtual bool rayTracingAvailable() const { return false; }
  // Whether the ray-tracing technique is currently active.
  virtual bool rayTracingActive() const { return false; }
  // Number of samples represented by the current progressive RT image. Zero
  // for raster/backends without progressive accumulation.
  virtual uint32_t rayTracingAccumulatedSamples() const { return 0; }
  virtual uint32_t rayTracingTlasChunks() const { return 0; }
  virtual double rayTracingInitializationMs() const { return 0.0; }
  virtual uint64_t rayTracingInputInstances() const { return 0; }
  virtual bool rayTracingBuildIncomplete() const { return false; }
  // True when the active/available RT technique is hardware ray-query rather
  // than a software fallback (e.g. Vulkan's compute-BVH path). Meaningless
  // when rayTracingAvailable() is false. Defaults to true: only backends that
  // actually have a non-hardware RT technique need to override this.
  virtual bool rayTracingIsHardware() const { return true; }
  // Transactionally replace the active Vulkan ray-query or compute-BVH
  // pipeline from a SPIR-V module. The old pipeline remains live on
  // validation/creation failure. Intended for persistent-process shader
  // development.
  virtual bool reloadRayTracingShader(const uint32_t* /*words*/,
                                      size_t /*wordCount*/,
                                      std::string* err) {
    if (err) *err = "live Vulkan shader reload is unsupported by this backend";
    return false;
  }
  // Start a transactional replacement build without blocking the caller. The
  // renderer commits a successful candidate from its owning thread and then
  // publishes `result`. Backends without Vulkan RT leave this unsupported.
  virtual bool beginReloadRayTracingShaderAsync(
      std::vector<uint32_t> /*words*/,
      std::shared_ptr<AsyncShaderReloadResult> /*result*/, std::string* err) {
    if (err) *err = "asynchronous live shader reload is unsupported by this backend";
    return false;
  }
  // The Vulkan backend selects a compact graph-free shader for simple scenes.
  // Live compilation mirrors that selection to avoid paying the full MaterialX
  // driver-JIT cost when its ABI-compatible interpreter is not in use.
  virtual bool rayTracingUsesFullShader() const { return true; }
  virtual bool rayTracingUsesProceduralMaterialX() const { return true; }
  virtual void setMaterialXVulkanShaderLimits(size_t /*maxSourceBytes*/,
                                               int /*compileTimeoutSec*/) {}
  // Switch the active technique between rasterization (false) and ray tracing
  // (true). No-op / ignored when ray tracing is unavailable. Both techniques
  // consume the same uploaded scene, so toggling needs no reload.
  virtual void setRayTracing(bool /*enable*/) {}

  // View-dependent RT LOD: supply the camera snapshot used to classify instances
  // as Full / Proxy / Cull when the TLAS is (re)built. `reselect` requests a TLAS
  // rebuild now (call it when the camera has settled). focalPx is filled by the
  // backend from its own viewport height; the caller leaves it 0. No-op unless
  // the ray-tracing backend supports it.
  virtual void setLodCamera(const RtLodCamera& /*cam*/, bool /*reselect*/) {}

  // Tear down ImGui backend + device resources.
  virtual void shutdown() = 0;

 protected:
  void reportSceneUploadError(const std::string& message) {
    if (sceneUploadError_.empty()) sceneUploadError_ = message;
  }

 private:
  std::string sceneUploadError_;
};

std::unique_ptr<Renderer> CreateGLRenderer();
#if defined(HAVE_VULKAN)
std::unique_ptr<Renderer> CreateVulkanRenderer();

// Total DEVICE_LOCAL heap bytes of the GPU we would render on, via a throwaway
// VkInstance so it can be called BEFORE a renderer exists -- the large-scene
// budgets are resolved during argument parsing, long before device creation.
// Returns 0 if Vulkan is unavailable; callers need a fallback. Prefers a
// discrete GPU, matching startup device selection.
uint64_t QueryDeviceLocalVramBytes();
#endif

}  // namespace lusdview
