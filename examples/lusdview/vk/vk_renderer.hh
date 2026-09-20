// SPDX-License-Identifier: Apache-2.0
// lusdview - Vulkan backend. Renders the scene into an offscreen VkImage which
// is shown by the GUI via ImGui::Image (ImGui_ImplVulkan_AddTexture). ImGui
// itself renders into the swapchain.
//
// Raster path: vertex/index buffers, material textures, and GPU skinning data
// descriptors. Optional ray-query path is built when Vulkan RT support exists.
#pragma once

// Vulkan entry points are resolved at runtime via volk (cuew-style): we never
// link the Vulkan loader. volk.h pulls in <vulkan/vulkan.h> with
// VK_NO_PROTOTYPES and declares the vk* symbols as function pointers that
// volkInitialize()/volkLoadInstance()/volkLoadDevice() populate.
#include "volk.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu_scene.hh"
#include "raster_lighting.hh"
#include "renderer.hh"

namespace lusdview {

struct VulkanProbeResult {
  bool rasterAvailable{false};
  bool rtAvailable{false};
  std::string error;
};

VulkanProbeResult ProbeVulkanBackend(GLFWwindow* window,
                                     const RendererDevicePreference& preference);

class VulkanRenderer final : public Renderer {
 public:
  VulkanRenderer() = default;
  ~VulkanRenderer() override;

  void setDevicePreference(const RendererDevicePreference& preference) override {
    devicePreference_ = preference;
  }
  void setTransparencyMode(TransparencyMode mode) override;
  TransparencyStatus transparencyStatus() const override;
  void setRasterQualityHigh(bool high) override { rasterQualityHigh_ = high; }
  bool init(GLFWwindow* window, std::string* err) override;
  void setHeadlessSize(int w, int h) override {
    if (w > 0) headlessW_ = w;
    if (h > 0) headlessH_ = h;
    // Headless never calls resizeViewport(), so seed the viewport extent here too:
    // the offscreen target IS the headless size, and consumers like the RT-LOD
    // focalPx (0.5 * vpH_ * proj_[5]) would otherwise see vpH_ == 0 -> focalPx 0
    // -> every instance projects to 0px and is size-culled.
    if (w > 0) vpW_ = w;
    if (h > 0) vpH_ = h;
  }
  bool resizeHeadless(int w, int h) override;
  bool initImGui(std::string* err) override;
  void beginScene(const std::vector<DrawMaterialCPU>& materials, int textureCount) override;
  // Progressive/streaming loads call this as more materials and textures arrive
  // after the initial beginScene. Without it (the base class default is a
  // no-op, which this backend used to inherit) the texture-slot arrays stayed
  // at their first-event size, every later uploadTexture() for a higher slot
  // was dropped, and interactively loaded scenes rendered untextured.
  void syncSceneResources(const std::vector<DrawMaterialCPU>& materials,
                          int textureCount) override;
  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err) override;
  void setLights(const std::vector<DrawLightCPU>& lights,
                 size_t meshCount) override;
  void appendMesh(const DrawMeshCPU& mesh) override;
  void appendPoints(const DrawPointsCPU& points) override;
  void appendCurves(const DrawCurvesCPU& curves) override;
  void appendPoints(DrawPointsCPU&& points) override;
  void appendCurves(DrawCurvesCPU&& curves) override;
  void appendVolume(const DrawVolumeCPU& vol) override;
  void uploadTexture(int slot, const DrawTextureCPU& tex) override;
  void evictTexture(int slot) override;
  size_t textureResidentBytes(int slot) const override;
  void setRtTextureBudgetBytes(size_t bytes) override {
    rtTextureBudgetBytes_ = bytes;
    rtTextureTableDirty_ = true;
  }
  // See renderer.hh: source scene for rebuildSwBvh()'s BuildHostScene() call.
  void setHostSceneSource(const DrawScene* scene) override {
    hostSceneSource_ = scene;
  }
  bool updateTextureRegion(int slot, int x, int y, int w, int h,
                           const uint8_t* rgba,
                           size_t rowBytes = 0) override;
  bool updateTextureRegions(
      int slot, const std::vector<TextureRegionUpdate>& updates) override;
  bool updatePtexFaceRect(int slot, uint32_t face,
                          const DrawPtexFaceRectCPU& rect) override;
  void uploadSkinningFrame(const SkinningFrameCPU& skin) override;
  void uploadMeshAux(size_t meshIndex, const DrawMeshCPU& mesh) override;
  void updateMeshVertices(int meshIndex,
                          const std::vector<DrawVertex>& verts) override;
  bool updateMeshSkinningGpu(int meshIndex, const float* mats, int jointCount,
                             int matrixBase, const float aabbMin[3],
                             const float aabbMax[3]) override;
  void updateMorphWeights(int meshIndex,
                          const std::vector<float>& coeffs) override;
  void updateInstanceVisibility(size_t meshIndex, const float* xforms,
                                const float* colors, const float* opacities,
                                uint32_t count) override;
  void installPrototypeLods(
      size_t meshIndex,
      const std::vector<DrawPrototypeLodCPU>& levels) override;
  void updateInstanceLodVisibility(
      size_t meshIndex, const float* xforms, const float* colors,
      const float* opacities,
      const std::array<uint32_t, 4>& lodCounts) override;
  // Raster LOD box proxies (optimization B): the VK raster path collapses distant
  // instances to a shared unit cube (box-fit per instance) instead of dropping them.
  bool supportsProxyDraw() const override { return true; }
  void updateProxyInstances(const float* xforms, const float* tints,
                            uint32_t count) override;
  void updateMeshWorld(int meshIndex, const float world[16]) override;
  bool applyMeshSlotUpdatesTransactional(
      const std::vector<MeshSlotUpdate>& updates, std::string* error) override;
  bool replaceMeshSlotsTransactional(
      const std::vector<MeshSlotReplacement>& replacements,
      std::string* error) override;
  bool applyMaterialUpdatesTransactional(
      const std::vector<MaterialSlotUpdate>& updates,
      std::string* error) override;
  bool replaceTextureSlotsTransactional(
      const std::vector<TextureSlotReplacement>& replacements,
      std::string* error) override;
  int meshCount() const override { return static_cast<int>(meshes_.size()); }
  void resizeViewport(int width, int height) override;
  void newFrame() override;
  void renderFrame(const RenderFrameParams& params) override;
  ViewportTexHandle viewportTexture() const override;
  void present() override;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  void presentThreaded(ImDrawData* drawData, int fbW, int fbH) override;
  bool initImGuiPlatform(GLFWwindow* window, std::string* err) override;
  bool initImGuiBackend(std::string* err) override;
#endif
  void viewportSize(int* w, int* h) const override {
    if (w) *w = vpW_;
    if (h) *h = vpH_;
  }
  bool uploadViewportImage(const uint8_t* rgba, int w, int h) override;
  bool captureViewport(std::vector<uint8_t>* rgba, int* w, int* h) override;
  bool captureLinearViewport(std::vector<float>* rgba, int* w, int* h) override;
  bool captureWindow(std::vector<uint8_t>* rgba, int* w, int* h) override;  // headless composite
  const RendererCaps& caps() const override { return caps_; }
  bool rayTracingAvailable() const override { return rtSupported_; }
  bool rayTracingActive() const override { return rtActive_; }
  // True when the active/available RT technique is hardware ray-query rather
  // than the compute-BVH fallback. Diagnostics/UI only; rendering code should
  // branch on rtTechnique_ directly.
  bool rayTracingIsHardware() const override {
    return rtTechnique_ == RtTechnique::kHardware;
  }
  bool reloadRayTracingShader(const uint32_t* words, size_t wordCount,
                              std::string* err) override;
  bool beginReloadRayTracingShaderAsync(
      std::vector<uint32_t> words,
      std::shared_ptr<AsyncShaderReloadResult> result,
      std::string* err) override;
  bool rayTracingUsesFullShader() const override { return rtUsesFullShader_; }
  bool rayTracingUsesProceduralMaterialX() const {
    return rtUsesProceduralMaterialX_;
  }
  void setMaterialXVulkanShaderLimits(size_t maxSourceBytes,
                                      int compileTimeoutSec) override {
    materialXShaderMaxSourceBytes_ = maxSourceBytes;
    materialXShaderCompileTimeoutSec_ = compileTimeoutSec;
  }
  uint32_t rayTracingAccumulatedSamples() const override {
    if (!rtActive_) return 0u;
    const bool ready = rtTechnique_ == RtTechnique::kHardware
                           ? tlas_ != VK_NULL_HANDLE
                           : (rtTechnique_ == RtTechnique::kComputeBvh &&
                              swTlasNodeCount_ > 0u && swInstCount_ > 0u);
    return ready ? rtAccumFrame_ + 1u : 0u;
  }
  uint32_t rayTracingTlasChunks() const override { return rtTlasChunkCount_; }
  double rayTracingInitializationMs() const override { return rtInitMs_; }
  uint64_t rayTracingInputInstances() const override {
    return rtTlasInputInstances_;
  }
  bool rayTracingBuildIncomplete() const override { return rtBuildIncomplete_; }
  void setRayTracing(bool enable) override;
  void setLodCamera(const RtLodCamera& cam, bool reselect) override;
  void shutdown() override;

 private:
  // Keep the CPU from queueing a second frame ahead of input. The swapchain can
  // still own multiple images, but lusdview records/submits at most one frame at
  // a time for lower interactive latency.
  static constexpr int kFramesInFlight = 1;

  struct VkMeshGPU {
    VkBuffer vbo{VK_NULL_HANDLE};
    VkDeviceMemory vboMem{VK_NULL_HANDLE};
    // Optional ray-tracing vertex buffer: the BLAS + ray-query hit read this
    // (via vboAddr) for displacement-baked or dynamically-skinned RT geometry,
    // while the raster path keeps using `vbo` as rest geometry.
    VkBuffer vboDisp{VK_NULL_HANDLE};
    VkDeviceMemory vboDispMem{VK_NULL_HANDLE};
    VkBuffer jointVbo{VK_NULL_HANDLE};
    VkDeviceMemory jointVboMem{VK_NULL_HANDLE};
    VkBuffer weightVbo{VK_NULL_HANDLE};
    VkDeviceMemory weightVboMem{VK_NULL_HANDLE};
    VkBuffer influenceVbo{VK_NULL_HANDLE};
    VkDeviceMemory influenceVboMem{VK_NULL_HANDLE};
    VkBuffer uv1Vbo{VK_NULL_HANDLE};         // 2nd texcoord set (binding 4, vec2/vtx)
    VkDeviceMemory uv1VboMem{VK_NULL_HANDLE};
    VkBuffer morphInflVbo{VK_NULL_HANDLE};   // blendshape influence (binding 5, float/vtx)
    VkDeviceMemory morphInflVboMem{VK_NULL_HANDLE};
    VkBuffer influenceDataBuf{VK_NULL_HANDLE};
    VkDeviceMemory influenceDataMem{VK_NULL_HANDLE};
    VkDescriptorSet influenceDesc{VK_NULL_HANDLE};
    // GPU blendshape morph (raster): per-vertex (offset,count) attribute (binding 6,
    // loc 8) plus delta/coefficient/channel SSBOs in deform set 1. hasMorph gates
    // it; non-morph meshes bind dummy buffers.
    VkBuffer morphOffsetVbo{VK_NULL_HANDLE};     // binding 6: uvec2 (offset,count)
    VkDeviceMemory morphOffsetVboMem{VK_NULL_HANDLE};
    VkBuffer morphDeltaBuf{VK_NULL_HANDLE};       // set 1/binding 2 SSBO
    VkDeviceMemory morphDeltaMem{VK_NULL_HANDLE};
    VkDescriptorSet morphDeltaDesc{VK_NULL_HANDLE};
    VkBuffer morphCoeffBuf{VK_NULL_HANDLE};       // set 1/binding 3 SSBO
    VkDeviceMemory morphCoeffMem{VK_NULL_HANDLE};
    VkDescriptorSet morphCoeffDesc{VK_NULL_HANDLE};
    void* morphCoeffMapped{nullptr};
    VkBuffer morphChanBuf{VK_NULL_HANDLE};         // set 1/binding 4 SSBO
    VkDeviceMemory morphChanMem{VK_NULL_HANDLE};
    VkDescriptorSet morphChanDesc{VK_NULL_HANDLE};
    int morphChannelCount{0};
    bool hasMorph{false};
    VkBuffer ebo{VK_NULL_HANDLE};
    VkDeviceMemory eboMem{VK_NULL_HANDLE};
    VkBuffer wireEbo{VK_NULL_HANDLE};
    VkDeviceMemory wireEboMem{VK_NULL_HANDLE};
    VkBuffer wireVbo{VK_NULL_HANDLE};
    VkDeviceMemory wireVboMem{VK_NULL_HANDLE};
    uint32_t wireIndexCount{0};
    std::vector<DrawSubmesh> submeshes;
    float world[16];
    float localCentroid[3]{0, 0, 0};  // mesh-space bbox center (translucency sort)
    float instanceWorldCentroid[3]{0, 0, 0};
    // Ray tracing (built when RT is supported): a BLAS over this mesh's
    // triangles plus device addresses + counts for the shader.
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkBuffer blasBuf{VK_NULL_HANDLE};
    VkDeviceMemory blasMem{VK_NULL_HANDLE};
    VkDeviceAddress blasAddr{0};
    // Dynamic (per-frame deformed) BLAS: updateMeshVertices marks the mesh, its
    // BLAS is then built with ALLOW_UPDATE (and skips compaction -- a compacted
    // AS cannot be refit), and later poses REFIT it in place (MODE_UPDATE)
    // instead of destroy + full rebuild. The refit scratch is persistent: same
    // size every frame, so allocate once and keep it until the BLAS dies.
    bool blasDynamic{false};        // sticky: describes the mesh, not the AS
    bool blasRefitPending{false};   // vertices changed since last build/refit
    VkBuffer blasScratchBuf{VK_NULL_HANDLE};
    VkDeviceMemory blasScratchMem{VK_NULL_HANDLE};
    VkDeviceSize blasUpdateScratchSize{0};
    // Opt-in GPU compute skinning (skin.comp, LUSDVIEW_RT_GPU_SKIN=1):
    // persistently-mapped per-mesh SSBO of composed skinning matrices
    // (16 floats each), re-filled per frame by updateMeshSkinningGpu.
    VkBuffer skinMatBuf{VK_NULL_HANDLE};
    VkDeviceMemory skinMatMem{VK_NULL_HANDLE};
    VkDeviceAddress skinMatAddr{0};
    void* skinMatMapped{nullptr};
    uint32_t skinMatCapacity{0};  // capacity in matrices
    VkDeviceAddress vboAddr{0};
    VkDeviceAddress eboAddr{0};
    uint32_t vertexCount{0};
    uint32_t indexCount{0};
    int matId{-1};       // material id of the mesh's first submesh (RT shading)
    float normalMat[9];  // inverse-transpose of world 3x3 (object->world normals)
    bool skinned{false};
    bool extendedSkinned{false};
    // RT instancing + displayColor + authored-normal flag (mirrors the GL path).
    VkBuffer vtxColorBuf{VK_NULL_HANDLE};   // displayColor+displayOpacity (vec4[])
    VkDeviceMemory vtxColorMem{VK_NULL_HANDLE};
    VkDeviceAddress vtxColorAddr{0};
    // Retained descriptor for RT/compatibility; raster binds vtxColorBuf as an
    // ordinary vec4 vertex stream to avoid descriptor-dependent color loss.
    VkDescriptorSet vtxColorDesc{VK_NULL_HANDLE};
    VkDeviceAddress uv1Addr{0};              // uv1 SSBO address (RT multi-UV AOV)
    VkDeviceAddress inflAddr{0};             // influence SSBO address (RT influence AOV)
    VkBuffer faceBuf{VK_NULL_HANDLE};        // per-triangle source face id (uint[])
    VkDeviceMemory faceMem{VK_NULL_HANDLE};
    VkDeviceAddress faceAddr{0};             // RT source-face-id AOV
    // Optional per-triangle material ids for GeomSubset meshes. Single-material
    // meshes keep this null and use matId directly in the ray-query shader.
    VkBuffer triMatBuf{VK_NULL_HANDLE};
    VkDeviceMemory triMatMem{VK_NULL_HANDLE};
    VkDeviceAddress triMatAddr{0};
    VkDeviceAddress jointAddr{0};            // RT skin-weights AOV (joint ids)
    VkDeviceAddress weightAddr{0};           // RT skin-weights AOV (weights)
    VkBuffer geomPropDescBuf{VK_NULL_HANDLE};
    VkDeviceMemory geomPropDescMem{VK_NULL_HANDLE};
    VkDeviceAddress geomPropDescAddr{0};
    VkBuffer geomPropValueBuf{VK_NULL_HANDLE};
    VkDeviceMemory geomPropValueMem{VK_NULL_HANDLE};
    VkDeviceAddress geomPropValueAddr{0};
    uint32_t geomPropCount{0};
    // Optional compact primitive-range table for RT material lookup. Allocated
    // only for multi-material or distinct-back-material meshes.
    VkBuffer rtSubmeshBuf{VK_NULL_HANDLE};
    VkDeviceMemory rtSubmeshMem{VK_NULL_HANDLE};
    VkDeviceAddress rtSubmeshAddr{0};
    uint32_t rtSubmeshCount{0};
    VkDescriptorSet faceDesc{VK_NULL_HANDLE}; // raster source-face-id (set 3); else dummy
    // Consolidated raster set 1: bones, extended influences, morph buffers,
    // displayColor and source-face ids. Replaces seven separately-bound sets.
    VkDescriptorSet deformDesc{VK_NULL_HANDLE};
    bool geometricNormal{false};            // no authored normals -> geometric face normal
    bool doubleSided{false};                // double-sided AOV flag
    bool hasVertexOpacity{false};           // preserve two-sided varying-opacity folds
    int purposeId{0};                       // purpose AOV: 0=default/1=render/2=proxy/3=guide
    int kindId{0};                          // kind AOV: 0=none/1=component/2=group/3=assembly/4=subcomponent
    float flatColor[3]{0.8f, 0.8f, 0.8f};   // per-draw constant tint (instanced path)
    float flatOpacity{1.0f};
    // Prototype object-space AABB (for RT view-dependent LOD: per-instance
    // projected size + the box-fit proxy transform). Captured from DrawMeshCPU.
    float protoAabbMin[3]{0, 0, 0};
    float protoAabbMax[3]{0, 0, 0};
    // GPU instancing: one TLAS instance per 3x4 o2w in instanceXforms (12 floats
    // each); instanceColors is 3 floats/instance (empty -> use flatColor), and
    // instanceOpacities is 1 float/instance (empty -> use flatOpacity). Held on
    // the CPU and consumed in rebuildTlas (the TLAS instance array is the GPU copy).
    std::vector<float> instanceXforms;
    std::vector<float> instanceColors;
    std::vector<float> instanceOpacities;
    bool hasTranslucentInstances{false};
    // Raster GPU instancing (flat --next path): per-instance 3x4 o2w (instVbo,
    // 48B/inst, host-visible so per-instance culling can re-map the visible subset)
    // + per-instance RGBA color/opacity (instColorBuf) + per-vertex prototype color
    // (instVtxColorBuf, white when absent). drawInstanceCount is the count actually
    // drawn (== instanceCount unless per-instance culling shrank it this frame).
    VkBuffer instVbo{VK_NULL_HANDLE};
    VkDeviceMemory instVboMem{VK_NULL_HANDLE};
    // Persistent host mapping when instVbo/instColorBuf are pool-suballocated (their
    // *Mem is VK_NULL_HANDLE then): per-instance culling writes the compacted subset
    // through these instead of vkMapMemory. Null for the legacy per-buffer path.
    void* instVboMapped{nullptr};
    void* instColorMapped{nullptr};
    VkBuffer instColorBuf{VK_NULL_HANDLE};
    VkDeviceMemory instColorMem{VK_NULL_HANDLE};
    VkBuffer instVtxColorBuf{VK_NULL_HANDLE};
    VkDeviceMemory instVtxColorMem{VK_NULL_HANDLE};
    uint32_t instanceCount{0};
    uint32_t drawInstanceCount{0};
    uint32_t opaqueInstanceCount{0};
    uint32_t translucentInstanceCount{0};
    struct PrototypeLodGPU {
      VkBuffer ebo{VK_NULL_HANDLE};
      VkDeviceMemory eboMem{VK_NULL_HANDLE};
      std::vector<DrawSubmesh> submeshes;
      float objectError{0.0f};
      float triangleRatio{1.0f};
      uint32_t level{1};
    };
    std::vector<PrototypeLodGPU> prototypeLods;
    std::array<uint32_t, 4> lodInstanceCount{{0, 0, 0, 0}};
    std::array<uint32_t, 4> lodInstanceOffset{{0, 0, 0, 0}};
    // Multi-draw-indirect (MDI) placement. When mdiEligible (device supports MDI
    // and this prototype is non-morph), the instance-rate buffers above point into
    // the renderer's ONE global instance buffer rather than a per-mesh allocation:
    // instVbo/instColorBuf are the shared handles and instVboMapped/instColorMapped
    // address this mesh's slice, so the existing cull writes land in the global
    // buffer unchanged. mdiInstFirst is the mesh's base instance index (the indirect
    // command's firstInstance); mdiVertBase/mdiIdxBase are its base into the shared
    // geometry/index buffers (vertexOffset/firstIndex). Morph prototypes stay on the
    // per-mesh path (MDI can't switch morph descriptor sets per draw).
    bool mdiEligible{false};
    uint32_t mdiInstFirst{0};
    uint32_t mdiVertBase{0};
    uint32_t mdiIdxBase{0};
    // Coarse per-prototype instance grid for RT LOD cell rejection (P5). Built
    // lazily on the first LOD-enabled rebuild; instance transforms are static.
    RtLodGrid lodGrid;
    bool lodGridTried{false};
  };

  // setup helpers
  bool createInstance(std::string* err);
  bool createSurface(std::string* err);
  bool pickPhysicalDevice(std::string* err);
  bool createDevice(std::string* err);
  bool createSwapchain(std::string* err);
  bool createSwapchainViews(std::string* err);
  // Windowless substitute for the swapchain: kFramesInFlight color images we own
  // (color-attachment + transfer-src) that the ImGui composite pass renders into
  // and captureWindow() reads back.
  bool createHeadlessSwapchain(std::string* err);
  bool createSwapchainRenderPass(std::string* err);
  bool createSwapchainFramebuffers(std::string* err);
  bool createCommands(std::string* err);
  bool createSync(std::string* err);
  // (Re)create the per-swapchain-image sync (renderFinished_ semaphores + clear
  // imagesInFlight_). Destroys any existing ones first; call after the swapchain (and
  // its image count) is (re)created. Requires the device to be idle.
  bool createPerImageSync(std::string* err);
  bool createOffscreenRenderPass(std::string* err);
  bool createOitRenderPass(std::string* err);
  bool createOitCompositePipeline(std::string* err, bool compile = true);
  bool createRasterDofPipeline(std::string* err);
  void recordRasterDof(VkCommandBuffer cb);
  bool beginOitPromotion(std::string* err);
  void serviceOitPromotion();
  bool createOverlayLoadPass(std::string* err);  // LOAD pass for overlays over RT
  struct NonMeshChunkUpload {
    VkBuffer buf{VK_NULL_HANDLE};
    VkDeviceMemory mem{VK_NULL_HANDLE};
  };
  // Upload + draw one HelperVertex line set with the given pipeline.
  void drawLineSet(VkCommandBuffer cb, const std::vector<HelperVertex>& copy,
                   VkBuffer* buf, VkDeviceMemory* mem, VkDeviceSize* cap,
                   VkPipeline pipeline, const float vp[16],
                   std::vector<NonMeshChunkUpload>* chunkUploads = nullptr);
  bool createPipeline(std::string* err);
  void createTessPipeline();  // best-effort GPU-tessellation pipeline (non-fatal)
  bool createInstPipeline(std::string* err);
  bool createLinePipeline(std::string* err);
  bool createNativeCarrierPipeline(std::string* err);
  bool rebuildNativeCarrierBuffer(int curveSegments);
  uint64_t drawNativeCarriers(VkCommandBuffer cb, VkPipeline pipeline,
                              int transparencyClass = -1);
  void drawNativeCarrierShadows(VkCommandBuffer cb, bool point, int face);
  void destroyNativeCarrierResources();
  bool createSampler(std::string* err);
  bool createDescriptorInfra(std::string* err);
  VkDescriptorSet allocDeformDescriptor(const VkMeshGPU& mesh);
  void refreshDeformDescriptors();
  void updateMaterialDescriptor(size_t materialId);
  void refreshMaterialDescriptors();
  bool createWhiteTexture(std::string* err);

  // --- Ray tracing (ray query) ---
  void detectRtSupport();              // sets rtSupported_ + loads RT entrypoints
  bool createRtResources(std::string* err);  // descriptor layout/pool + pipeline
  void selectFullRtShaderIfNeeded(
      const std::vector<DrawMaterialCPU>& materials);
  void destroyRt();

  // --- Ray tracing (compute-BVH fallback, no hardware ray query needed) ---
  // Falls through from detectRtSupport() when hardware RT is vetoed. Sets
  // rtTechnique_ = kComputeBvh (and rtSupported_ = true) only when the
  // compute-BVH shader variant was actually compiled and embedded
  // (LUSDVIEW_HAVE_SWRT_SHADER) -- otherwise a no-op, so --rt keeps its
  // current rasterization fallback until that shader exists.
  void detectSwRtSupport();
  // Flattens the current scene via rt_scene_build.cc's BuildHostScene() and
  // uploads the triangle SoA + BVH (BLAS/TLAS) + instance arrays as SSBOs.
  // Pure host-side + buffer upload, no shader dependency -- always compiled,
  // but only reachable when rtTechnique_ == kComputeBvh selects this path.
  void rebuildSwBvh();
  // Builds the SAME rtMatBuf_/rtMatLightRtBuf_/rtLightBuf_/rtTex*Buf_ SSBOs
  // rebuildTlas() builds for the hardware technique (same source CPU state:
  // matColor_/matLightRt_/lightParams_/rtTexturesCpu_/rtMaterialsCpu_/
  // rtLightsCpu_), so the shared shading GLSL sees identically-laid-out data
  // regardless of technique. Deliberately a separate function rather than a
  // shared helper factored out of rebuildTlas(): that function is proven,
  // hardware-RT-only code this session cannot runtime-verify on hardware
  // without RT support, so it stays untouched to avoid an unverifiable
  // regression risk.
  void rebuildSwMaterialLightSSBOs();
  bool createSwRtResources(std::string* err);  // sw-BVH descriptor layout/pool + pipeline
  void traceRtBvh(VkCommandBuffer cb);          // dispatch + copy into colorImg_
  void destroySwRt();
  VkDeviceAddress bufferDeviceAddress(VkBuffer buf) const;
  bool createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer* buf, VkDeviceMemory* mem);  // device-local + addr
  // Device-local buffer initialized from `data` via a one-shot staging copy (no
  // device address). For static geometry that the GPU reads every frame -- keeps
  // it in VRAM instead of host-visible memory fetched over PCIe per draw.
  bool createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                               const void* data, VkBuffer* buf, VkDeviceMemory* mem);
  void destroyBlas(VkMeshGPU& m);
  void destroyMeshResources(VkMeshGPU& mesh);
  void finishMeshVertexUpdate(VkMeshGPU& mesh,
                              const std::vector<DrawVertex>& vertices);
  bool meshHasAlphaMask(const VkMeshGPU& m) const;
  bool meshHasBoundMaterial(const VkMeshGPU& m) const;
  void buildBlas(VkMeshGPU& m);
  void refitBlas(VkMeshGPU& m);         // MODE_UPDATE in-place rebuild (dynamic BLAS)
  void buildBoxBlas();                  // shared unit-cube BLAS for LOD box proxies
  void initBoxProxyRaster();            // static box geometry for raster LOD proxies
  void drawBoxProxies(VkCommandBuffer cb);  // upload + instanced draw of box proxies
  void rebuildTlas();                  // (re)build TLAS + MeshDesc/Material SSBOs
  void rebuildRtTextureTable();        // refresh bindings 11..14, keep AS intact
  void createRtImage();                // storage image sized to the viewport
  void traceRt(VkCommandBuffer cb);    // dispatch + copy into colorImg_

  void destroySwapchain();
  bool recreateSwapchain();
  // Shared body of present()/presentThreaded(): records + submits the frame using
  // the supplied ImGui draw data. present() passes ImGui::GetDrawData(); the threaded
  // path passes the packet's deep-copied draw data (+ the main-thread framebuffer
  // size) so it can run on the render thread.
  void presentImpl(ImDrawData* drawData, int fbW, int fbH);
  // ImGui Vulkan render-backend init (device objects); shared by initImGui() and the
  // threaded initImGuiBackend(). Needs init()'s device/queue/render pass.
  bool initVulkanImGuiBackend(std::string* err);
  void destroyOffscreen();
  void destroyScene();
  // Shared by beginScene and syncSceneResources: (re)pack the per-material CPU
  // tables and (re)allocate + refresh the material descriptor sets.
  void updateMaterialTables(const std::vector<DrawMaterialCPU>& materials);
  void applyPendingMaterialConstants();
  bool ensureRasterMaterialCapacity(size_t materialCount);
  // Grow the per-texture-slot arrays to `textureCount`, leaving already
  // populated slots untouched (std::vector::resize only fills NEW entries).
  void growTextureSlots(int textureCount);

  uint32_t findMemoryTypeOrInvalid(uint32_t typeBits,
                                  VkMemoryPropertyFlags props) const;
  // Latched once a device-local host-visible allocation fails (small BAR full),
  // so later buffers go straight to plain host memory. See createHostBuffer.
  bool devLocalHostExhausted_{false};
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
  // Create a host-visible buffer initialised with `data`. When `poolable` is true the
  // backing memory is sub-allocated from a few large shared blocks (set *mem =
  // VK_NULL_HANDLE) instead of its own vkAllocateMemory -- the big-assembled-scene win
  // (the per-buffer alloc count, and thus the driver's O(n^2) allocation cost,
  // collapses). Only pass poolable for write-once static buffers, or buffers re-written
  // through `mappedOut` (a persistent host pointer to the sub-allocation); never for
  // buffers whose *mem is later vkMapMemory'd/updated. Pool blocks are freed in
  // destroyScene() after every per-buffer vkDestroyBuffer.
  bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                        VkBuffer* buf, VkDeviceMemory* mem, bool deviceAddress = false,
                        bool poolable = false, void** mappedOut = nullptr);
  // One large persistently-mapped host-visible block the above sub-allocates from.
  struct HostMemBlock {
    VkDeviceMemory mem{VK_NULL_HANDLE};
    VkDeviceSize size{0};
    VkDeviceSize used{0};
    void* mapped{nullptr};
    uint32_t memoryTypeIndex{0};
    bool deviceAddress{false};
  };
  std::vector<HostMemBlock> hostBlocks_;
  // Sub-allocate `size` (honouring `align`) from a block matching memoryTypeIndex +
  // deviceAddress; grows a new block when none fits. Returns the block memory + offset
  // + persistent mapped address. Returns false on allocation failure.
  bool poolSubAlloc(VkDeviceSize size, VkDeviceSize align, uint32_t memoryTypeIndex,
                    bool deviceAddress, VkDeviceMemory* outMem, VkDeviceSize* outOffset,
                    void** outMapped);

  // ---- Device-local pool (static mesh geometry) ----
  // Static mesh buffers are never re-mapped after upload, so they can live in true
  // DEVICE_LOCAL memory rather than depending on a host-visible VRAM heap (ReBAR).
  // Same block sub-allocation as hostBlocks_ -- one vkAllocateMemory per static
  // mesh buffer would reproduce the allocation storm that pooling exists to avoid
  // (Moana island: 83801 prototypes) -- but with no mapping, since the data is
  // staged in through a copy instead.
  std::vector<HostMemBlock> deviceBlocks_;  // .mapped stays null
  uint64_t devLocalBufCount_{0};   // reported by LUSDVIEW_TIME_GPU
  VkDeviceSize devLocalBufBytes_{0};
  bool deviceSubAlloc(VkDeviceSize size, VkDeviceSize align, uint32_t memoryTypeIndex,
                      bool deviceAddress, VkDeviceMemory* outMem,
                      VkDeviceSize* outOffset);
  bool createDeviceLocalPooledBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                     const void* data, VkBuffer* buf,
                                     VkDeviceMemory* mem, bool deviceAddress);
  void freeDevicePool();
  // Static-buffer size floor for the device-local pool; hardware dependent, so
  // probed once on first use. See the definition for the measurements.
  VkDeviceSize deviceLocalFloorBytes();
  VkDeviceSize deviceLocalFloorBytes_{0};  // 0 = not yet probed
  // Staging copies for the above, batched: endOneShot does a full vkQueueWaitIdle,
  // so one submit per buffer would stall the streaming upload path once per mesh.
  // Flushed on a byte threshold, before any BLAS build, and before each frame.
  struct PendingBufferUpload {
    VkBuffer staging{VK_NULL_HANDLE};
    VkDeviceMemory stagingMem{VK_NULL_HANDLE};
    VkBuffer dst{VK_NULL_HANDLE};
    VkDeviceSize size{0};
  };
  std::vector<PendingBufferUpload> pendingUploads_;
  VkDeviceSize pendingUploadBytes_{0};
  void flushPendingUploads();
  void freeHostPool();  // unmap + free every block (after buffers are destroyed)
  bool createTextureImage(const light3d::Image& img, VkImage* outImg,
                          VkDeviceMemory* outMem, VkImageView* outView,
                          const std::vector<light3d::Image>* mips = nullptr,
                          bool srgb = false);
  bool createCompressedTextureImage(const DrawCompressedImageCPU& img, bool srgb,
                                    VkImage* outImg, VkDeviceMemory* outMem,
                                    VkImageView* outView);
  bool createCompressedUdimTextureArrayImage(const DrawTextureCPU& tex,
                                             bool srgb, VkImage* outImg,
                                             VkDeviceMemory* outMem,
                                             VkImageView* outView);
  bool createUdimTextureArrayImage(const DrawTextureCPU& tex, VkImage* outImg,
                                   VkDeviceMemory* outMem,
                                   VkImageView* outView);
  bool createUdimLookupImage(const DrawTextureCPU& tex, VkImage* outImg,
                             VkDeviceMemory* outMem, VkImageView* outView);
  bool createUdimLookupAtlas(int rows);
  bool updateUdimLookupAtlasRow(int row, const DrawTextureCPU& tex);
  // DomeLight split-sum IBL (sets 21-23; 1x1 black cube/2D fallbacks).
  // `levels` are face-major float RGB cube levels (DomeIblCPU layout).
  bool createIblCubeImage(const std::vector<std::vector<float>>& levels,
                          int faceSize, VkImage* outImg, VkDeviceMemory* outMem,
                          VkImageView* outView);
  bool createIblLutImage(const std::vector<float>& rg, int size, VkImage* outImg,
                         VkDeviceMemory* outMem, VkImageView* outView);
  void destroyIblImages();
  bool createRgba32fTextureImage(int width, int height, const float* data,
                                 VkImage* outImg, VkDeviceMemory* outMem,
                                 VkImageView* outView);
  bool uploadRgba32fTextureImage(VkImage image, int width, int height,
                                 const float* data);
  VkDescriptorSet allocTexDescriptor(VkImageView view);
  VkDescriptorSet allocSkinDescriptor(VkBuffer buffer, VkDeviceSize size);
  VkDescriptorSet allocInfluenceDescriptor(VkBuffer buffer, VkDeviceSize size);
  VkCommandBuffer beginOneShot();
  void endOneShot(VkCommandBuffer cb);
  VkShaderModule createShader(const void* code, size_t bytes);
  void rebuildPtexRectBuffer();

  GLFWwindow* window_{nullptr};
  // Window framebuffer size carried in from the main thread by presentThreaded()
  // (glfwGetFramebufferSize is main-thread-only). When >0, recreateSwapchain()/the
  // createSwapchain currentExtent fallback use it instead of a GLFW query, so
  // swapchain recreation needs no GLFW call on the render thread. 0 on the
  // single-threaded path (then glfwGetFramebufferSize is used as before).
  int winFbW_{0}, winFbH_{0};
  bool headless_{false};       // windowless: no surface/swapchain, render offscreen
  int headlessW_{1280}, headlessH_{800};  // composite size when headless
  uint32_t lastSwapIndex_{0};  // last composite image written (for captureWindow)
  RendererCaps caps_{};
  bool imguiInited_{false};

  VkInstance instance_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  VkPhysicalDevice phys_{VK_NULL_HANDLE};
  RendererDevicePreference devicePreference_;
  VkDevice device_{VK_NULL_HANDLE};
  uint32_t queueFamily_{0};
  VkQueue queue_{VK_NULL_HANDLE};

  // Swapchain (for ImGui)
  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  VkFormat swapFormat_{VK_FORMAT_UNDEFINED};
  VkExtent2D swapExtent_{};
  std::vector<VkImage> swapImages_;
  std::vector<VkDeviceMemory> swapMem_;  // headless only: backing for swapImages_
  std::vector<VkImageView> swapViews_;
  std::vector<VkFramebuffer> swapFramebuffers_;
  VkRenderPass swapRenderPass_{VK_NULL_HANDLE};

  // Offscreen target (3D scene)
  VkRenderPass offscreenPass_{VK_NULL_HANDLE};
  VkRenderPass oitPass_{VK_NULL_HANDLE};
  VkRenderPass oitOverlayPass_{VK_NULL_HANDLE};
  VkRenderPass shadowPass_{VK_NULL_HANDLE};
  VkRenderPass overlayLoadPass_{VK_NULL_HANDLE};  // draw overlays over the RT image
  VkImage colorImg_{VK_NULL_HANDLE};
  VkDeviceMemory colorMem_{VK_NULL_HANDLE};
  VkImageView colorView_{VK_NULL_HANDLE};
  VkImage depthImg_{VK_NULL_HANDLE};
  VkDeviceMemory depthMem_{VK_NULL_HANDLE};
  VkImageView depthView_{VK_NULL_HANDLE};
  VkImage oitAccumImg_{VK_NULL_HANDLE};
  VkDeviceMemory oitAccumMem_{VK_NULL_HANDLE};
  VkImageView oitAccumView_{VK_NULL_HANDLE};
  VkImage oitRevealImg_{VK_NULL_HANDLE};
  VkDeviceMemory oitRevealMem_{VK_NULL_HANDLE};
  VkImageView oitRevealView_{VK_NULL_HANDLE};
  VkImage shadowColorImg_{VK_NULL_HANDLE}, shadowDepthImg_{VK_NULL_HANDLE};
  VkDeviceMemory shadowColorMem_{VK_NULL_HANDLE}, shadowDepthMem_{VK_NULL_HANDLE};
  VkImageView shadowColorView_{VK_NULL_HANDLE}, shadowDepthView_{VK_NULL_HANDLE};
  VkFramebuffer shadowFb_{VK_NULL_HANDLE};
  RasterShadowCamera shadowCamera_;
  VkImage pointShadowDepthImg_{VK_NULL_HANDLE};
  VkDeviceMemory pointShadowDepthMem_{VK_NULL_HANDLE};
  VkImageView pointShadowDepthView_{VK_NULL_HANDLE};
  std::array<VkImageView, 6> pointShadowFaceViews_{};
  std::array<VkFramebuffer, 6> pointShadowFbs_{};
  RasterPointShadowCameras pointShadowCameras_;
  VkFramebuffer offscreenFb_{VK_NULL_HANDLE};
  VkFramebuffer oitFb_{VK_NULL_HANDLE};
  VkDescriptorSetLayout oitCompositeSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool oitCompositePool_{VK_NULL_HANDLE};
  VkDescriptorSet oitCompositeSet_{VK_NULL_HANDLE};
  VkPipelineLayout oitCompositeLayout_{VK_NULL_HANDLE};
  VkPipeline oitCompositePipeline_{VK_NULL_HANDLE};
  VkDescriptorSetLayout dofSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool dofPool_{VK_NULL_HANDLE};
  VkDescriptorSet dofSet_{VK_NULL_HANDLE};
  VkPipelineLayout dofLayout_{VK_NULL_HANDLE};
  VkPipeline dofPipeline_{VK_NULL_HANDLE};
  VkImage dofImg_{VK_NULL_HANDLE};
  VkDeviceMemory dofMem_{VK_NULL_HANDLE};
  VkImageView dofView_{VK_NULL_HANDLE};
  bool dofImageInitialized_{false};
  VkSampler sampler_{VK_NULL_HANDLE};
  // Material samplers are keyed by DrawTextureCPU wrapS/wrapT (4x4). Scene
  // helpers keep using `sampler_`; material descriptors select from this cache.
  std::array<VkSampler, 16> materialSamplers_{};
  VkDescriptorSet offscreenTexId_{VK_NULL_HANDLE};
  int vpW_{0}, vpH_{0};
  VkFormat colorFormat_{VK_FORMAT_R8G8B8A8_UNORM};
  VkFormat depthFormat_{VK_FORMAT_D32_SFLOAT};

  // Pipeline
  VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkPipeline environmentPipeline_{VK_NULL_HANDLE};
  VkPipeline wirePipeline_{VK_NULL_HANDLE};
  VkPipeline wireOccludedPipeline_{VK_NULL_HANDLE};
  VkPipeline shadowPipeline_{VK_NULL_HANDLE};
  // GPU tessellation displacement pipeline (shares pipelineLayout_; PATCH_LIST
  // topology + tesc/tese). Created only when the device supports the
  // tessellationShader feature; otherwise displaced meshes stay coarse.
  bool tessSupported_{false};
  bool samplerAnisotropySupported_{false};
  bool weightedOitSupported_{false};
  TransparencyMode transparencyMode_{TransparencyMode::Auto};
  bool rasterQualityHigh_{false};
  float maxSamplerAnisotropy_{1.0f};
  // VK_EXT_extended_dynamic_state: per-draw cull mode, so single-sided meshes
  // back-face-cull like the GL backend. Optional; false = no culling (legacy).
  bool dynCullSupported_{false};
  PFN_vkCmdSetCullModeEXT vkCmdSetCullMode_{nullptr};
  // multiDrawIndirect + drawIndirectFirstInstance + shaderDrawParameters all
  // present -> the instanced raster pass can draw via vkCmdDrawIndexedIndirect
  // batches (per-draw meshId/flags come from a gl_DrawIDARB-indexed SSBO).
  bool mdiSupported_{false};
  // Translucent (Blend-material) variant of the main mesh pipeline: premultiplied
  // "over" blend + depth-write-off. Drawn back-to-front after the opaque pass.
  VkPipeline translucentPipeline_{VK_NULL_HANDLE};
  VkPipeline oitMeshPipeline_{VK_NULL_HANDLE};
  VkPipeline tessPipeline_{VK_NULL_HANDLE};
  VkShaderStageFlags pushStages_{VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT};
  VkPipelineLayout instPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline instPipeline_{VK_NULL_HANDLE};
  VkPipeline instTranslucentPipeline_{VK_NULL_HANDLE};
  VkPipeline oitInstPipeline_{VK_NULL_HANDLE};
  VkPipeline instShadowPipeline_{VK_NULL_HANDLE};

  // Unlit line pipeline for debug helpers (grid/axes/bbox). Per-frame host
  // buffers (grow on demand) so a frame never writes a buffer still in flight.
  VkPipelineLayout lineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout lineDepthSetLayout_{VK_NULL_HANDLE};
  VkPipelineLayout lineDepthLayout_{VK_NULL_HANDLE};
  VkDescriptorPool lineDepthPool_{VK_NULL_HANDLE};
  VkDescriptorSet lineDepthSet_{VK_NULL_HANDLE};
  VkPipeline linePipeline_{VK_NULL_HANDLE};
  VkPipeline linePipelineNoDepth_{VK_NULL_HANDLE};  // X-ray overlay (skeleton)
  VkPipeline lineDepthPipeline_{VK_NULL_HANDLE};
  // Camera-facing native Points/Curves raster carrier. The persistent Vulkan
  // path stores one compact instance per point/selected curve segment and lets
  // the vertex shader expand it; the old CPU triangle expansion remains as a
  // fallback for allocation/pipeline failure and full-tessellation requests.
  VkPipeline nonMeshPipeline_{VK_NULL_HANDLE};
  VkBuffer nonMeshBuf_[kFramesInFlight]{};
  VkDeviceMemory nonMeshMem_[kFramesInFlight]{};
  VkDeviceSize nonMeshCap_[kFramesInFlight]{};
  std::vector<NonMeshChunkUpload> nonMeshChunkUploads_[kFramesInFlight];
  std::vector<HelperVertex> nonMeshCopy_;
  std::vector<DrawPointsCPU> nativePoints_;
  std::vector<DrawCurvesCPU> nativeCurves_;
  struct NativeCarrierInstanceGPU {
    float p0[3];
    float p1[3];
    float prev[3];
    float next[3];
    float widths[2];
    float color0[4];
    float color1[4];
  };
  struct NativeCarrierRange {
    uint32_t first{0};
    uint32_t count{0};
    int kind{0};
    int materialId{-1};
    int carrierId{0};
    int purpose{0};
    uint32_t lightMask{0xffffffffu};
    uint32_t shadowMask{0xffffffffu};
    bool translucent{false};
  };
  VkPipeline nativeCarrierPipeline_{VK_NULL_HANDLE};
  VkPipeline oitNativeCarrierPipeline_{VK_NULL_HANDLE};
  VkPipeline nativeCarrierShadowPipeline_{VK_NULL_HANDLE};
  VkBuffer nativeCarrierBuf_{VK_NULL_HANDLE};
  VkDeviceMemory nativeCarrierMem_{VK_NULL_HANDLE};
  std::vector<NativeCarrierRange> nativeCarrierRanges_;
  int nativeCarrierSegments_{-1};
  int nativeCarrierRequestedSegments_{-1};
  bool nativeCarrierBuildFailed_{false};
  bool nativeCarrierCpuForced_{false};
  uint32_t nativeCarrierPurposeMask_{0xffffffffu};
  RenderMode nativeCarrierMode_{RenderMode::Shaded};
  std::vector<uint8_t> nativeCarrierVisible_;

  // --- UsdVol volume raymarch (proxy-box, 3D density texture) ---
  struct VkVolumeGPU {
    VkImage img{VK_NULL_HANDLE};
    VkDeviceMemory mem{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    VkBuffer ubo{VK_NULL_HANDLE};
    VkDeviceMemory uboMem{VK_NULL_HANDLE};
    void* uboMapped{nullptr};
    VkDescriptorSet set{VK_NULL_HANDLE};
    float model[16];
    float invModel[16];
    float bmin[3], bmax[3];
    float albedo[3], densityScale;
    float emission[3], background;
    bool hasEmissionField{false};
    bool hasTemperatureField{false};
  };
  bool createVolumePipeline(std::string* err);
  // Record the volume raymarch draws (UBO update + proxy boxes) with `pipe`.
  void recordVolumePass(VkCommandBuffer cb, VkPipeline pipe);
  VkPipelineLayout volumeLayout_{VK_NULL_HANDLE};
  VkPipeline volumePipeline_{VK_NULL_HANDLE};
  VkPipeline oitVolumePipeline_{VK_NULL_HANDLE};

  // Immutable copies of the fixed-function state, captured by startup builders.
  // Pointers in Vulkan create-info structs are rebound to these owned arrays
  // when compiling; no pointers to startup stack data escape.
  struct OitRecipe {
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkDynamicState> dynamic;
    const uint32_t* vertex{nullptr};
    size_t vertexBytes{0};
    const uint32_t* fragment{nullptr};
    size_t fragmentBytes{0};
  };
  std::array<OitRecipe, 4> oitRecipes_;
  void captureOitRecipe(size_t slot, const VkGraphicsPipelineCreateInfo& info,
                        const uint32_t* vertex, size_t vertexBytes,
                        const uint32_t* fragment, size_t fragmentBytes);
  static VkPipeline compileOitRecipe(VkDevice device, VkRenderPass pass,
                                    VkPipelineCache cache, const OitRecipe& recipe);
  bool createOitAttachments();
  uint32_t requiredOitMask() const;
  void publishOitStatus();
  struct AsyncOitPromotion {
    std::atomic<bool> done{false};
    uint32_t mask{0};
    bool ok{false};
    double elapsedMs{0};
    std::array<VkPipeline, 5> pipelines{};
    VkPipeline composite{VK_NULL_HANDLE};
    std::string error;
  };
  std::shared_ptr<AsyncOitPromotion> oitPromotion_;
  std::future<void> oitPromotionJob_;
  // Background RT reload and OIT promotion share the compiler budget.
  std::mutex pipelineCompileMutex_;
  // Bits: graph-free mesh, full mesh, instancing, native carriers, volumes.
  uint32_t oitReadyMask_{0};
  mutable bool oitRequirementsDirty_{true};
  mutable uint32_t oitRequiredMask_{0};
  uint32_t oitFailedMask_{0};
  VkPipeline oitSimpleMeshPipeline_{VK_NULL_HANDLE};
  TransparencyMode requestedTransparency_{TransparencyMode::Auto};
  bool oitEffective_{false};
  std::string oitError_;
  double oitCompileMs_{0};
  mutable std::mutex oitStatusMutex_;
  TransparencyStatus oitStatus_;

  VkPipeline volumePipelineNoDepth_{VK_NULL_HANDLE};  // RT overlay (no depth)
  VkDescriptorSetLayout volumeSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool volumePool_{VK_NULL_HANDLE};
  VkBuffer volumeCubeBuf_{VK_NULL_HANDLE};      // 36-vertex proxy cube
  VkDeviceMemory volumeCubeMem_{VK_NULL_HANDLE};
  std::vector<VkVolumeGPU> volumes_;
  VkBuffer helperBuf_[kFramesInFlight]{};
  VkDeviceMemory helperMem_[kFramesInFlight]{};
  VkDeviceSize helperCap_[kFramesInFlight]{};
  std::vector<HelperVertex> helperCopy_;  // copied in renderFrame, drawn in present
  // Overlay (skeleton X-ray) per-frame line buffers + copy.
  VkBuffer overlayBuf_[kFramesInFlight]{};
  VkDeviceMemory overlayMem_[kFramesInFlight]{};
  VkDeviceSize overlayCap_[kFramesInFlight]{};
  std::vector<HelperVertex> overlayCopy_;
  // Selection-highlight per-frame edge-line buffers + copy (VK wireframe overlay).
  VkBuffer highlightLineBuf_[kFramesInFlight]{};
  VkDeviceMemory highlightLineMem_[kFramesInFlight]{};
  VkDeviceSize highlightLineCap_[kFramesInFlight]{};
  std::vector<HelperVertex> highlightLineCopy_;
  int highlightMeshIndex_{-1};
  bool highlightHasSubset_{false};
  bool highlightXray_{false};
  std::vector<uint8_t> meshVisible_;  // per-mesh visibility mask (raster), copied in renderFrame
  // Reused each frame; avoids allocating the alpha-pass mesh order vector in
  // the render loop. Camera-dependent sorted fallback only permutes it.
  std::vector<size_t> rasterMeshOrder_;
  std::vector<uint8_t> rtMeshVisible_;  // persistent user hide/isolate mask for TLAS

  // Textures (base color). One combined-image-sampler descriptor per texture,
  // plus a default 1x1 white texture for untextured submeshes.
  VkDescriptorSetLayout texSetLayout_{VK_NULL_HANDLE};
  // Consolidated raster layouts: set 0 holds all material/IBL samplers; set 1
  // holds all per-mesh deformation/AOV buffers. Together with frame + material
  // parameter sets this keeps both mesh pipelines at four bound sets.
  VkDescriptorSetLayout materialSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool materialPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> materialSets_;
  // Raster resources are canonicalized independently of RT materials. Logical
  // ids remain stable in DrawScene and RT tables; raster draws translate here.
  std::vector<int> rasterLogicalToCanonical_;
  std::vector<int> rasterCanonicalRepresentatives_;
  std::unordered_map<std::string, VkDescriptorSet> materialSetCache_;
  VkDescriptorSetLayout deformSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool deformPool_{VK_NULL_HANDLE};
  VkDescriptorSet dummyDeformDesc_{VK_NULL_HANDLE};
  VkDescriptorSetLayout skinSetLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout influenceSetLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout faceSetLayout_{VK_NULL_HANDLE};
  // Raster set 2: global frame/displacement params UBO, read in the
  // vertex + tessellation stages so the UI's displacement-scale and max-tess
  // sliders are live on Vulkan (the push constants are full). One persistently
  // mapped host buffer, written each frame (volume-UBO convention).
  VkDescriptorSetLayout dispParamsSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool dispParamsPool_{VK_NULL_HANDLE};
  VkDescriptorSet dispParamsSet_{VK_NULL_HANDLE};
  VkBuffer dispParamsUbo_{VK_NULL_HANDLE};
  VkDeviceMemory dispParamsUboMem_{VK_NULL_HANDLE};
  void* dispParamsMapped_{nullptr};
  // Raster set 3: per-material texture scale/bias (indexed
  // by pc.matId in the vertex + tess-eval stages). This grows to the validated
  // scene material count; a fixed cap used to let ids >= 4096 read past the SSBO.
  VkDescriptorSetLayout dispMatSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool dispMatPool_{VK_NULL_HANDLE};
  VkDescriptorSet dispMatSet_{VK_NULL_HANDLE};
  VkBuffer dispMatSsbo_{VK_NULL_HANDLE};
  VkDeviceMemory dispMatSsboMem_{VK_NULL_HANDLE};
  void* dispMatMapped_{nullptr};
  size_t dispMatCapacity_{0};
  // Backend-neutral OpenPBR constants for the raster fragment path. The packet
  // is indexed by the same canonical material id as dispMatSsbo_ and remains
  // persistently mapped so live material edits do not rebuild descriptors.
  VkBuffer rasterMatPbrBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rasterMatPbrMem_{VK_NULL_HANDLE};
  void* rasterMatPbrMapped_{nullptr};
  // Set 3 of the instanced pipeline: per-draw metadata (meshId + flag bits), one
  // entry per mesh plus a trailing slot for the shared box proxy. The fragment
  // shader indexes it by (baseDraw + gl_DrawIDARB), so a multi-draw-indirect batch
  // needs no per-draw push. Contents are static per mesh -> (re)built by
  // ensureDrawMeta() only when the mesh count changes. Host-visible SSBO.
  VkDescriptorSetLayout drawMetaSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool drawMetaPool_{VK_NULL_HANDLE};
  VkDescriptorSet drawMetaSet_{VK_NULL_HANDLE};
  VkBuffer drawMetaBuf_{VK_NULL_HANDLE};
  VkDeviceMemory drawMetaBufMem_{VK_NULL_HANDLE};
  uint32_t drawMetaCount_{0};    // slots currently populated (meshes + 1 box slot)
  uint32_t drawMetaCap_{0};      // slots the buffer can hold
  uint32_t boxMetaSlot_{0};      // DrawMeta slot the box-proxy draw pushes as baseDraw
  uint32_t mdiMeshMetaBase_{0};  // per-mesh DrawMeta region base for the fallback loop
  void ensureDrawMeta();         // rebuild drawMetaBuf_ when meshes_ changes (non-MDI)
  // (Re)create drawMetaBuf_ to hold `meta` and repoint drawMetaSet_ at it. Shared by
  // ensureDrawMeta (per-mesh layout) and buildInstMdi (per-command layout).
  // Per-draw metadata (set 3), shared by mesh_inst.vert/.frag. `jointAddr` /
  // `weightAddr` are the device addresses of this mesh's per-vertex skin arrays
  // (0 = unskinned): the instanced vertex shader fetches them by gl_VertexIndex
  // rather than through vertex-input state, so the merged multi-draw path needs
  // no extra bindings -- its draws simply carry 0 (skinned prototypes are kept
  // out of MDI, whose gl_VertexIndex would index the MERGED buffer).
  struct DrawMetaCPU {
    int32_t ids[4];
    uint64_t jointAddr{0};
    uint64_t weightAddr{0};
  };
  static_assert(sizeof(int32_t) * 4 + sizeof(uint64_t) * 2 == 32, "DrawMeta 32B");
  void writeDrawMeta(const std::vector<DrawMetaCPU>& meta);
  void refreshMdiDrawMeta();

  // ---- Multi-draw-indirect instanced path (large-scene --next) ----
  // All MDI-eligible (non-morph) instanced prototypes are drawn as a handful of
  // vkCmdDrawIndexedIndirect batches over shared buffers instead of ~83k per-mesh
  // bind+draw calls. Geometry (positions/normals, per-vertex color, morph-zero,
  // indices) is concatenated once; per-instance o2w + RGBA color live in ONE global
  // buffer whose slices the meshes' instVboMapped point into (so cull fills it).
  // The indirect command list is patched each frame with per-mesh drawInstanceCount.
  VkBuffer mdiVbo_{VK_NULL_HANDLE};        VkDeviceMemory mdiVboMem_{VK_NULL_HANDLE};
  VkBuffer mdiVtxColBuf_{VK_NULL_HANDLE};  VkDeviceMemory mdiVtxColMem_{VK_NULL_HANDLE};
  VkBuffer mdiMorphBuf_{VK_NULL_HANDLE};   VkDeviceMemory mdiMorphMem_{VK_NULL_HANDLE};
  VkBuffer mdiEbo_{VK_NULL_HANDLE};        VkDeviceMemory mdiEboMem_{VK_NULL_HANDLE};
  VkBuffer mdiInstBuf_{VK_NULL_HANDLE};    VkDeviceMemory mdiInstMem_{VK_NULL_HANDLE};
  void* mdiInstMapped_{nullptr};
  VkBuffer mdiInstColBuf_{VK_NULL_HANDLE}; VkDeviceMemory mdiInstColMem_{VK_NULL_HANDLE};
  void* mdiInstColMapped_{nullptr};
  VkBuffer mdiIndirectBuf_{VK_NULL_HANDLE}; VkDeviceMemory mdiIndirectMem_{VK_NULL_HANDLE};
  void* mdiIndirectMapped_{nullptr};
  // Optional DEVICE-LOCAL mirror of the per-instance o2w + RGBA color buffers (island's
  // MDI frame time is instance-fetch bound: ~2 GB of host-visible transforms are
  // refetched over PCIe every frame). Cull writes still land in the host-visible
  // buffers; each rewritten mesh slice is queued here and vkCmdCopyBuffer'd into
  // the mirror at the top of the next frame, so a settled camera uploads nothing.
  // Enabled when the VK_EXT_memory_budget headroom check passes (the mirror costs
  // VRAM -- keep it OFF for tight-VRAM configs); LUSDVIEW_MDI_DEVLOCAL=0/1 forces.
  VkBuffer mdiInstDevBuf_{VK_NULL_HANDLE};    VkDeviceMemory mdiInstDevMem_{VK_NULL_HANDLE};
  VkBuffer mdiInstColDevBuf_{VK_NULL_HANDLE}; VkDeviceMemory mdiInstColDevMem_{VK_NULL_HANDLE};
  bool mdiInstDevLocal_{false};
  std::vector<VkBufferCopy> mdiInstPendingXf_, mdiInstPendingCol_;
  // One indirect command per (eligible mesh, submesh); meshIndex ties it back to the
  // prototype for the per-frame instanceCount patch + visibility gate.
  struct MdiCmd {
    uint32_t meshIndex;
    bool disabled;
    VkDrawIndexedIndirectCommand cmd;
  };
  std::vector<MdiCmd> mdiCmds_;
  // CPU staging accumulated during appendMesh, uploaded + freed by buildInstMdi().
  std::vector<float> mdiInstXfStage_;      // 12 floats / instance (o2w rows)
  std::vector<float> mdiInstColStage_;     // 4 floats / instance (rgba)
  std::vector<float> mdiVtxStage_;         // DrawVertex floats (positions+normals+...)
  std::vector<float> mdiVtxColStage_;      // 3 floats / vertex (per-vertex proto color)
  std::vector<uint32_t> mdiMorphStage_;    // 2 uint / vertex (morph offset,count = 0)
  std::vector<uint32_t> mdiIdxStage_;      // concatenated local indices
  uint32_t mdiInstTotal_{0};               // running instance count during staging
  uint32_t mdiVertTotal_{0};               // running vertex count during staging
  uint32_t mdiDrawCount_{0};               // number of indirect commands
  bool mdiBuilt_{false};                   // buffers built for the current scene
  bool mdiActive_{false};                  // any eligible mesh -> use the indirect draw
  // Runtime mesh replacements must not extend the immutable shared MDI buffers.
  // Stage them with owned per-mesh buffers and draw them through the fallback loop.
  bool forceDedicatedInstanceUpload_{false};
  void buildInstMdi();                     // upload staging -> shared buffers (once)
  void patchMdiIndirect();                 // refresh per-frame instanceCount + meta
  void destroyMdiBuffers();                // free shared buffers (reload/shutdown)
  // Sets 7 & 8: per-mesh GPU blendshape morph (delta SSBO + per-frame coeff SSBO).
  // Both are "readonly SSBO, vertex stage, binding 0", so they share one layout.
  // Non-morph meshes bind shared 1-element dummy descriptors (the shader statically
  // references the SSBOs, so they must always be bound).
  VkDescriptorSetLayout morphSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool morphPool_{VK_NULL_HANDLE};
  VkBuffer dummyMorphBuf_{VK_NULL_HANDLE};
  VkDeviceMemory dummyMorphMem_{VK_NULL_HANDLE};
  VkDescriptorSet dummyMorphDesc_{VK_NULL_HANDLE};
  VkDescriptorSet allocMorphDescriptor(VkBuffer buffer, VkDeviceSize size);
  VkDescriptorPool texPool_{VK_NULL_HANDLE};
  VkDescriptorPool skinPool_{VK_NULL_HANDLE};
  VkDescriptorPool influencePool_{VK_NULL_HANDLE};
  VkDescriptorPool facePool_{VK_NULL_HANDLE};
  // Shared 1-element dummy face buffer + descriptor, bound when a mesh has no
  // source-face data (or on pool overflow).
  VkBuffer dummyFaceBuf_{VK_NULL_HANDLE};
  VkDeviceMemory dummyFaceMem_{VK_NULL_HANDLE};
  VkDescriptorSet dummyFaceDesc_{VK_NULL_HANDLE};
  VkDescriptorSet allocFaceDescriptor(VkBuffer buffer, VkDeviceSize size);
  // DomeLight split-sum IBL resources (scene-lifetime; setLights owns).
  VkImage iblIrrImg_{VK_NULL_HANDLE};
  VkDeviceMemory iblIrrMem_{VK_NULL_HANDLE};
  VkImageView iblIrrView_{VK_NULL_HANDLE};
  VkImage iblSpecImg_{VK_NULL_HANDLE};
  VkDeviceMemory iblSpecMem_{VK_NULL_HANDLE};
  VkImageView iblSpecView_{VK_NULL_HANDLE};
  VkImage iblLutImg_{VK_NULL_HANDLE};
  VkDeviceMemory iblLutMem_{VK_NULL_HANDLE};
  VkImageView iblLutView_{VK_NULL_HANDLE};
  // Full-resolution env cube (RT miss background; unclamped radiance).
  VkImage iblEnvImg_{VK_NULL_HANDLE};
  VkDeviceMemory iblEnvMem_{VK_NULL_HANDLE};
  VkImageView iblEnvView_{VK_NULL_HANDLE};
  VkDescriptorSet iblIrrDesc_{VK_NULL_HANDLE};
  VkDescriptorSet iblSpecDesc_{VK_NULL_HANDLE};
  VkDescriptorSet iblLutDesc_{VK_NULL_HANDLE};
  // 1x1 black cube fallback (init-lifetime, like whiteImg_).
  VkImage blackCubeImg_{VK_NULL_HANDLE};
  VkDeviceMemory blackCubeMem_{VK_NULL_HANDLE};
  VkImageView blackCubeView_{VK_NULL_HANDLE};
  VkDescriptorSet blackCubeDesc_{VK_NULL_HANDLE};
  bool iblActive_{false};
  int iblLods_{0};
  float iblColor_[3]{1.0f, 1.0f, 1.0f};
  float iblRotation_[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  VkImage whiteImg_{VK_NULL_HANDLE};
  VkDeviceMemory whiteMem_{VK_NULL_HANDLE};
  VkImageView whiteView_{VK_NULL_HANDLE};
  VkDescriptorSet whiteDesc_{VK_NULL_HANDLE};
  // Black 1x1 (red=0) bound to material binding 16 when a submesh has no
  // displacement. The vertex shader always samples it, so black means no
  // displacement (no push-constant lane needed).
  VkImage blackImg_{VK_NULL_HANDLE};
  VkDeviceMemory blackMem_{VK_NULL_HANDLE};
  VkImageView blackView_{VK_NULL_HANDLE};
  VkDescriptorSet blackDesc_{VK_NULL_HANDLE};
  VkImage dummyArrayImg_{VK_NULL_HANDLE};
  VkDeviceMemory dummyArrayMem_{VK_NULL_HANDLE};
  VkImageView dummyArrayView_{VK_NULL_HANDLE};
  VkDescriptorSet dummyArrayDesc_{VK_NULL_HANDLE};
  VkImage dummyLutImg_{VK_NULL_HANDLE};
  VkDeviceMemory dummyLutMem_{VK_NULL_HANDLE};
  VkImageView dummyLutView_{VK_NULL_HANDLE};
  VkDescriptorSet dummyLutDesc_{VK_NULL_HANDLE};
  std::vector<VkImage> texImgs_;
  std::vector<VkDeviceMemory> texMems_;
  std::vector<VkImageView> texViews_;
  std::vector<VkDescriptorSet> texDescs_;
  std::vector<VkDescriptorSet> texUdimArrayDescs_;
  std::vector<VkImageView> texSlotViews_;
  std::vector<VkImage> texSlotImgs_;
  std::vector<VkDeviceMemory> texSlotMems_;
  std::vector<size_t> texSlotBytes_;
  std::vector<int> texSlotWidths_;
  std::vector<int> texSlotHeights_;
  std::vector<int> texSlotMipLevels_;
  std::vector<uint8_t> texRegionUpdatable_;
  std::vector<DrawCompressedFormat> texCompressedFormats_;
  std::vector<VkImageView> texUdimArrayViews_;
  std::vector<VkImage> texUdimArrayImgs_;
  std::vector<VkDeviceMemory> texUdimArrayMems_;
  std::vector<uint8_t> texIsUdim_;
  VkBuffer ptexRectBuf_{VK_NULL_HANDLE};
  VkDeviceMemory ptexRectMem_{VK_NULL_HANDLE};
  std::vector<uint32_t> ptexRectOffsets_;
  size_t rtTextureBudgetBytes_{0};
  VkImage udimLutAtlasImg_{VK_NULL_HANDLE};
  VkDeviceMemory udimLutAtlasMem_{VK_NULL_HANDLE};
  VkImageView udimLutAtlasView_{VK_NULL_HANDLE};
  VkDescriptorSet udimLutAtlasDesc_{VK_NULL_HANDLE};
  int udimLutAtlasRows_{0};
  std::vector<int> matBaseTex_;  // per material: DrawScene texture index or -1
  std::vector<int> matMetallicTex_;    // per material: DrawScene texture index or -1
  std::vector<int> matRoughnessTex_;
  std::vector<int> matNormalTex_;      // per material: DrawScene texture index or -1
  std::vector<int> matEmissiveTex_;    // per material: DrawScene texture index or -1
  std::vector<int> matOpacityTex_;     // scalar opacity texture index or -1
  std::vector<int> matOcclusionTex_;   // ambient-occlusion texture index or -1
  std::vector<int> matSpecularColorTex_;   // inputs:specularColor tex or -1
  std::vector<int> matCoatWeightTex_;      // coat weight (scalar) tex or -1
  std::vector<int> matCoatColorTex_;       // coat color (rgb) tex or -1
  std::vector<int> matCoatRoughnessTex_;   // coat roughness (scalar) tex or -1
  std::vector<int> matDispTex_;  // per material: displacement texture index or -1
  std::vector<float> matDispConst_;  // per material: constant displacement amount

  VkBuffer boneBuf_{VK_NULL_HANDLE};
  VkDeviceMemory boneMem_{VK_NULL_HANDLE};
  void* boneMapped_{nullptr};
  VkDescriptorSet boneDesc_{VK_NULL_HANDLE};
  VkDeviceSize boneBufSize_{0};

  // Commands & sync
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  VkCommandBuffer cmd_[kFramesInFlight]{};
  VkSemaphore imageAvailable_[kFramesInFlight]{};  // acquire->render, per frame-in-flight
  VkFence inFlight_[kFramesInFlight]{};            // render-done fence, per frame-in-flight

  // LUSDVIEW_TIME_GPU: per-pass GPU timing. Four timestamps per frame -- command
  // buffer start, main offscreen pass start, main pass end, frame end -- read back
  // one frame later (after the in-flight fence, so the results are ready without
  // an extra stall). Off unless the env var is set; no cost otherwise.
  VkQueryPool gpuQueryPool_{VK_NULL_HANDLE};
  double gpuTimestampPeriodNs_{0.0};
  bool gpuQueriesArmed_{false};   // this frame recorded a full set of 4
  bool gpuQueriesPending_{false}; // the previous submit has results to read
  // Present-wait semaphore, ONE PER SWAPCHAIN IMAGE (not per frame): vkQueuePresentKHR
  // has no fence, so a present's wait semaphore must not be reused until that image is
  // acquired again -- reusing a per-frame semaphore across images corrupts the queue
  // (the threaded VK-RT black). imagesInFlight_ tracks which frame's fence last rendered
  // each swapchain image, so we wait before reusing the image. Both are sized to the
  // swapchain image count and (re)built with the swapchain.
  std::vector<VkSemaphore> renderFinished_;
  std::vector<VkFence> imagesInFlight_;
  uint32_t frame_{0};

  // Scene
  std::vector<VkMeshGPU> meshes_;
  std::vector<float> matColor_;    // 3 vec4 per material: preview subset
  std::vector<float> matLightRt_;  // 14 vec4 per material: LightRT/OpenPBR block
  std::vector<float> matGraph_;    // fixed-size MaterialX graph runtime blocks
  std::vector<float> rasterMatGraph_; // local image slots for raster graph evaluation
  std::vector<float> rasterMatPbr_; // canonical 20-vec4 OpenPBR raster packet
  std::vector<float> lightParams_;  // packed DrawLightCPU params
  std::vector<uint32_t> rtDirectLightMasks_;
  std::vector<uint32_t> rtShadowLightMasks_;
  RasterLightSet rasterLights_;
  // CPU copies retained for the backend-neutral RT texture-table build. Vulkan
  // raster uploads images immediately, while ray query consumes decoded RGBA8
  // texels and semantic material slots from storage buffers.
  std::vector<DrawMaterialCPU> rtMaterialsCpu_;
  std::vector<int> pendingMaterialConstants_;
  std::vector<DrawTextureCPU> rtTexturesCpu_;
  std::vector<DrawLightCPU> rtLightsCpu_;

  // Last frame parameters (copied; caller's pointers are transient)
  bool hasParams_{false};
  float view_[16];
  float proj_[16];
  float cameraPos_[3]{0, 0, 0};
  float exposure_{0.0f};
  float materialXTime_{0.0f};
  float materialXFrame_{0.0f};
  std::array<float, RenderFrameParams::kMaxClippingPlanes * 4>
      clippingPlanes_{};
  int clippingPlaneCount_{0};
  RtCameraLens cameraLens_;
  RtCameraShutter cameraShutter_;
  PathTraceSettings pathTrace_;
  float lightDir_[3]{0.40160966f, 0.64257544f, 0.48193160f};
  float lightColor_[3]{1.0f, 1.0f, 1.0f};
  float clear_[4]{0.12f, 0.12f, 0.13f, 1.0f};

  // --- Ray tracing (ray query / compute-BVH) state ---
  // Which trace technique backs `rtSupported_`/`rtActive_`. Hardware ray-query
  // is preferred when the device+shader support it; compute-BVH (a software
  // BVH walk over rt_scene_build.cc's HostScene, see detectSwRtSupport())
  // is the fallback so --rt still ray traces on GPUs without RT hardware
  // instead of silently dropping to rasterization. Code that specifically
  // builds hardware acceleration structures (buildBlas/refitBlas/rebuildTlas)
  // must gate on `rtTechnique_ == RtTechnique::kHardware`, not just
  // `rtSupported_`, since the latter is now true for either technique.
  enum class RtTechnique { kNone, kHardware, kComputeBvh };
  RtTechnique rtTechnique_{RtTechnique::kNone};
  struct AsyncRtReload {
    std::future<void> worker;
    std::mutex mutex;
    bool active{false};
    bool ready{false};
    bool ok{false};
    RtTechnique technique{RtTechnique::kNone};
    bool pathTrace{false};
    VkPipeline pipeline{VK_NULL_HANDLE};
    std::string error;
    std::shared_ptr<AsyncShaderReloadResult> result;
  };
  AsyncRtReload asyncRtReload_;
  void serviceAsyncRtReload();
  void joinAsyncRtReload();
  // Hardware ray-query shader variants are embedded and may be older than the
  // canonical light payload. Scenes containing features not representable by
  // that variant are routed to the verified compute-BVH implementation.
  bool rtHardwareCapable_{false};
  bool rtSupported_{false};   // device + shader available (either technique)
  // Buffer device address is also used by the raster instanced skinning shader.
  // It is a separate capability from ray queries: older GPUs may support the
  // former while (correctly) reporting no RT feature set.
  bool bufferDeviceAddressSupported_{false};
  bool rtActive_{false};      // RT technique currently selected
  uint32_t rtMaxInstanceCount_{0};  // VK acceleration-structure limit
  int rtMode_{0};             // RenderMode (wireframe/matId/AOV) for RT + raster
  int wireMode_{0};           // 0 shaded, 1 wire only, 2 wire over shaded
  float depthScale_{1.0f};    // depth-AOV normalizer (scene extent)
  bool displacement_{true};   // apply UsdPreviewSurface displacement (coarse)
  float displacementScale_{1.0f};  // global displacement multiplier
  int maxTessLevel_{1};       // >1 selects the GPU-tessellation pipeline
  float sceneMin_[3]{0, 0, 0};      // position-AOV scene bbox
  float sceneExtent_[3]{1, 1, 1};
  bool tlasDirty_{true};      // TLAS / SSBOs need rebuild
  bool rtTextureTableDirty_{false};  // texture SSBOs changed; no AS rebuild needed
  std::string techniqueLabel_{"Vulkan"};  // caps_.backend_name points here
  uint32_t scratchAlign_{256};
  // Visible USD purposes for the RT TLAS (bit i = PurposeId i; default: guide
  // hidden, matching the raster/GUI default). rebuildTlas skips hidden-purpose
  // meshes so e.g. Caldera's guide planes never enter (or pay for) the AS.
  uint32_t rtPurposeMask_{0xBu};

  // VK_EXT_memory_budget: live device-local VRAM usage/budget (0 = unsupported).
  bool memBudgetSupported_{false};
  bool memoryBudget(uint64_t* usedBytes, uint64_t* budgetBytes) const;

  PFN_vkGetBufferDeviceAddressKHR pfnGetBufferDeviceAddress_{nullptr};
  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetASBuildSizes_{nullptr};
  PFN_vkCreateAccelerationStructureKHR pfnCreateAS_{nullptr};
  PFN_vkDestroyAccelerationStructureKHR pfnDestroyAS_{nullptr};
  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAS_{nullptr};
  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetASDeviceAddress_{nullptr};
  // BLAS compaction: build with ALLOW_COMPACTION, query the compacted size, then
  // copy into a right-sized AS. A BLAS is typically ~half its build-time size.
  PFN_vkCmdWriteAccelerationStructuresPropertiesKHR pfnCmdWriteASProps_{nullptr};
  PFN_vkCmdCopyAccelerationStructureKHR pfnCmdCopyAS_{nullptr};
  bool blasCompact_{true};  // LUSDVIEW_BLAS_COMPACT=0 opts out
  // Resident compacted BLAS bytes for this pose, and what they would have cost
  // uncompacted (reported in the [vk_rt] log; also what the test asserts on).
  uint64_t blasBytes_{0};
  uint64_t blasBytesUncompacted_{0};
  size_t blasUniqueBuilt_{0};  // prototypes actually built this pose (post-dedup)

  // Build the Full-LOD BLAS set as compacted waves (one build submit + one copy
  // submit per wave), instead of one build + one queue stall per prototype.
  void buildBlasWave(const std::vector<uint32_t>& meshIds);
  // Drop the BLAS of prototypes this pose does not render at Full. Without this
  // the BLAS set only ever grows as the camera visits new regions.
  void evictBlasNotIn(const std::vector<uint32_t>& keepMeshIds);
  void destroyTlasChunks();

  struct ExtraTlasChunk {
    VkAccelerationStructureKHR as{VK_NULL_HANDLE};
    VkBuffer asBuffer{VK_NULL_HANDLE};
    VkDeviceMemory asMemory{VK_NULL_HANDLE};
    VkBuffer instanceBuffer{VK_NULL_HANDLE};
    VkDeviceMemory instanceMemory{VK_NULL_HANDLE};
  };

  VkAccelerationStructureKHR tlas_{VK_NULL_HANDLE};
  VkBuffer tlasBuf_{VK_NULL_HANDLE};
  VkDeviceMemory tlasMem_{VK_NULL_HANDLE};
  VkBuffer instBuf_{VK_NULL_HANDLE};       // VkAccelerationStructureInstanceKHR[]
  VkDeviceMemory instMem_{VK_NULL_HANDLE};
  VkDeviceSize instCap_{0};
  std::vector<ExtraTlasChunk> extraTlasChunks_;
  uint32_t rtTlasChunkCount_{0};
  uint32_t rtTlasChunkStride_{0};
  uint64_t rtTlasInputInstances_{0};
  bool rtBuildIncomplete_{false};
  VkBuffer meshDescBuf_{VK_NULL_HANDLE};    // per-mesh {addrs, matId, normalMat}
  VkDeviceMemory meshDescMem_{VK_NULL_HANDLE};
  VkDeviceSize meshDescCap_{0};
  VkBuffer rtMatBuf_{VK_NULL_HANDLE};       // vec4 baseColor[]
  VkDeviceMemory rtMatMem_{VK_NULL_HANDLE};
  VkBuffer rtMatLightRtBuf_{VK_NULL_HANDLE};  // LightRT/OpenPBR material block
  VkDeviceMemory rtMatLightRtMem_{VK_NULL_HANDLE};
  VkBuffer rtLightBuf_{VK_NULL_HANDLE};        // packed DrawLightCPU params
  VkDeviceMemory rtLightMem_{VK_NULL_HANDLE};
  VkBuffer rtTexelBuf_{VK_NULL_HANDLE};         // packed RGBA8 texels (uint[])
  VkDeviceMemory rtTexelMem_{VK_NULL_HANDLE};
  VkBuffer rtTexDescBuf_{VK_NULL_HANDLE};       // HostTextureDesc[]
  VkDeviceMemory rtTexDescMem_{VK_NULL_HANDLE};
  VkBuffer rtMatTexBuf_{VK_NULL_HANDLE};        // six texture ids/material
  VkDeviceMemory rtMatTexMem_{VK_NULL_HANDLE};
  VkBuffer rtMatTexParamBuf_{VK_NULL_HANDLE};   // 80 floats/material
  VkDeviceMemory rtMatTexParamMem_{VK_NULL_HANDLE};
  VkBuffer rtMatGraphBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rtMatGraphMem_{VK_NULL_HANDLE};
  VkBuffer rasterMatGraphBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rasterMatGraphMem_{VK_NULL_HANDLE};
  void* rasterMatGraphMapped_{nullptr};
  size_t rasterMatGraphCapacity_{0};
  VkDeviceSize rtMatCap_{0};
  VkBuffer instInfoBuf_{VK_NULL_HANDLE};    // per-TLAS-instance {meshId, tint} (binding 4)
  VkDeviceMemory instInfoMem_{VK_NULL_HANDLE};
  VkBuffer rtPointBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rtPointMem_{VK_NULL_HANDLE};
  VkBuffer rtPointNodeBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rtPointNodeMem_{VK_NULL_HANDLE};
  VkBuffer rtPointOrderBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rtPointOrderMem_{VK_NULL_HANDLE};
  VkBuffer rtPointChunkBuf_{VK_NULL_HANDLE};
  VkDeviceMemory rtPointChunkMem_{VK_NULL_HANDLE};
  uint32_t rtPointCount_{0};
  uint32_t rtPointNodeCount_{0};
  uint32_t rtPointChunkCount_{0};
  // True when analytic Gaussian point buffers do not fit the current Vulkan
  // residency budget; raster camera-facing splat quads remain available.
  bool gaussianRtDisabled_{false};

  VkImage rtImage_{VK_NULL_HANDLE};
  VkDeviceMemory rtImageMem_{VK_NULL_HANDLE};
  VkImageView rtImageView_{VK_NULL_HANDLE};
  VkImage rtPrimaryDepthImage_{VK_NULL_HANDLE};
  VkDeviceMemory rtPrimaryDepthMem_{VK_NULL_HANDLE};
  VkImageView rtPrimaryDepthView_{VK_NULL_HANDLE};

  // Interactive HIP/CUDA path: an externally-traced image staged into colorImg_ by
  // uploadViewportImage(). When externalColorValid_ is set, the next presentImpl()
  // skips the raster/RT 3D pass (colorImg_ already holds the frame) and clears it.
  VkBuffer extStaging_{VK_NULL_HANDLE};
  VkDeviceMemory extStagingMem_{VK_NULL_HANDLE};
  VkDeviceSize extStagingCap_{0};
  bool externalColorValid_{false};

  // Progressive accumulation: an rgba32f radiance buffer the trace adds into each
  // frame while the view is static (sub-pixel jittered -> anti-aliased, and the
  // stochastic AOVs like AO / soft-shadow converge). Reset to sample 0 whenever
  // the camera, render mode, viewport, or geometry changes.
  VkImage accumImage_{VK_NULL_HANDLE};
  VkDeviceMemory accumImageMem_{VK_NULL_HANDLE};
  VkImageView accumImageView_{VK_NULL_HANDLE};
  uint32_t rtAccumFrame_{0};        // current accumulated sample index (0 = reset)
  float lastRtPV_[16]{};            // proj*view of the last traced frame
  int lastRtMode_{-1};              // render mode of the last traced frame
  uint64_t rtAccumGen_{0};          // bumped on geometry / viewport invalidation
  uint64_t lastRtAccumGen_{~0ull};  // generation of the last traced frame
  bool rtAccumEnabled_{true};       // master toggle for progressive accumulation
  bool rtTemporalPose_{false};      // shutter pose belongs to one base-view sequence
  bool rtTemporalReset_{false};     // first pose after base sequence changed
  double rtInitMs_{0.0};            // lazy ray-query pipeline creation time

  // View-dependent LOD: camera snapshot used by rebuildTlas to classify instances.
  // Default (lodEnabled=false) reproduces the all-Full, no-cull TLAS exactly.
  RtLodCamera lodCam_;
  // Shared 12-triangle unit-cube BLAS instanced (box-fit per prototype AABB) for
  // every Proxy-LOD instance, so the long tail of distant prototypes needs no
  // full BLAS. Built once (buildBoxBlas). boxMesh_ holds only vbo/ebo/blas.
  VkMeshGPU boxMesh_;

  // Raster LOD box proxies (optimization B, VK raster path). Static unit-cube
  // geometry drawn with instPipeline_; per-instance box-fit o2w (binding 1) + rgba tint
  // (binding 2) are uploaded each frame from boxProxyXforms_/boxProxyTints_ (filled
  // by updateProxyInstances on the cull/upload path). bindings 3/4 carry constant
  // white per-vertex color + zero morph so the box reuses the prototype pipeline.
  VkBuffer boxRasterVbo_{VK_NULL_HANDLE};        // 8x DrawVertex (binding 0)
  VkDeviceMemory boxRasterVboMem_{VK_NULL_HANDLE};
  VkBuffer boxRasterEbo_{VK_NULL_HANDLE};        // 36 indices
  VkDeviceMemory boxRasterEboMem_{VK_NULL_HANDLE};
  VkBuffer boxRasterVtxColBuf_{VK_NULL_HANDLE};  // 8x vec3(1,1,1) (binding 3)
  VkDeviceMemory boxRasterVtxColMem_{VK_NULL_HANDLE};
  VkBuffer boxRasterMorphBuf_{VK_NULL_HANDLE};   // 8x uvec2(0,0) (binding 4)
  VkDeviceMemory boxRasterMorphMem_{VK_NULL_HANDLE};
  VkBuffer boxRasterInstBuf_{VK_NULL_HANDLE};    // per-instance o2w (binding 1)
  VkDeviceMemory boxRasterInstMem_{VK_NULL_HANDLE};
  VkDeviceSize boxRasterInstCap_{0};             // allocated bytes
  VkBuffer boxRasterTintBuf_{VK_NULL_HANDLE};    // per-instance rgba tint (binding 2)
  VkDeviceMemory boxRasterTintMem_{VK_NULL_HANDLE};
  VkDeviceSize boxRasterTintCap_{0};             // allocated bytes
  uint32_t boxRasterCount_{0};                   // proxies to draw this frame
  std::vector<float> boxProxyXforms_;            // 12 floats/proxy (CPU staging)
  std::vector<float> boxProxyTints_;             // 4 floats/proxy rgba (CPU staging)

  VkDescriptorSetLayout rtSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool rtPool_{VK_NULL_HANDLE};
  VkDescriptorSet rtSet_{VK_NULL_HANDLE};
  VkPipelineLayout rtPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline rtPipeline_{VK_NULL_HANDLE};
  bool rtUsesFullShader_{true};
  bool rtUsesProceduralMaterialX_{false};
  size_t materialXShaderMaxSourceBytes_{129u * 1024u};
  int materialXShaderCompileTimeoutSec_{30};

  // --- Compute-BVH fallback (rt_scene_build.cc HostScene, uploaded as SSBOs) ---
  // Separate pipeline/descriptor-set-layout from the hardware-RT ones above:
  // binding 0 is a fundamentally different descriptor type (storage buffers,
  // not an acceleration structure), so the two techniques don't share layouts,
  // mirroring the existing pipeline_/instPipeline_ "two draw paths" precedent.
  VkBuffer swTriBuf_{VK_NULL_HANDLE};    // HostScene::tris (9 floats/tri)
  VkDeviceMemory swTriMem_{VK_NULL_HANDLE};
  VkBuffer swNrmBuf_{VK_NULL_HANDLE};    // HostScene::nrms (9 floats/tri)
  VkDeviceMemory swNrmMem_{VK_NULL_HANDLE};
  VkBuffer swColBuf_{VK_NULL_HANDLE};    // HostScene::cols (RGBA/vertex)
  VkDeviceMemory swColMem_{VK_NULL_HANDLE};
  VkBuffer swUvBuf_{VK_NULL_HANDLE};     // HostScene::uv
  VkDeviceMemory swUvMem_{VK_NULL_HANDLE};
  VkBuffer swUv1Buf_{VK_NULL_HANDLE};    // HostScene::uv1
  VkDeviceMemory swUv1Mem_{VK_NULL_HANDLE};
  VkBuffer swMatBuf_{VK_NULL_HANDLE};    // HostScene::mat (material id/tri)
  VkDeviceMemory swMatMem_{VK_NULL_HANDLE};
  VkBuffer swBackMatBuf_{VK_NULL_HANDLE}; // HostScene::backMat (back material/tri)
  VkDeviceMemory swBackMatMem_{VK_NULL_HANDLE};
  VkBuffer swFaceBuf_{VK_NULL_HANDLE};   // HostScene::face (source face/tri)
  VkDeviceMemory swFaceMem_{VK_NULL_HANDLE};
  VkBuffer swGeomPropDescBuf_{VK_NULL_HANDLE};
  VkDeviceMemory swGeomPropDescMem_{VK_NULL_HANDLE};
  VkBuffer swGeomPropValueBuf_{VK_NULL_HANDLE};
  VkDeviceMemory swGeomPropValueMem_{VK_NULL_HANDLE};
  uint32_t swGeomPropCount_{0};
  VkBuffer swBlasBuf_{VK_NULL_HANDLE};   // HostScene::blas (Node[])
  VkDeviceMemory swBlasMem_{VK_NULL_HANDLE};
  VkBuffer swTlasBuf_{VK_NULL_HANDLE};   // HostScene::tlas (Node[])
  VkDeviceMemory swTlasMem_{VK_NULL_HANDLE};
  VkBuffer swInstBuf_{VK_NULL_HANDLE};   // HostScene::instances (Inst[])
  VkDeviceMemory swInstMem_{VK_NULL_HANDLE};
  uint32_t swTriCount_{0};
  uint32_t swBlasNodeCount_{0};
  uint32_t swTlasNodeCount_{0};
  uint32_t swInstCount_{0};

  const DrawScene* hostSceneSource_{nullptr};  // see setHostSceneSource()

  VkDescriptorSetLayout swRtSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool swRtPool_{VK_NULL_HANDLE};
  VkDescriptorSet swRtSet_{VK_NULL_HANDLE};
  VkPipelineLayout swRtPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline swRtPipeline_{VK_NULL_HANDLE};
  VkPipeline swRtPathPipeline_{VK_NULL_HANDLE};
  // VK_EXT_pipeline_creation_cache_control lets large optional pipelines
  // report VK_PIPELINE_COMPILE_REQUIRED instead of blocking in the driver.
  bool pipelineCompileRequiredSupported_{false};
  // Pipelines replaced by live reload are retired after the next frame fence.
  // This avoids waiting on a fence that newFrame() has reset but not submitted.
  std::vector<VkPipeline> retiredRtPipelines_;

  // Opt-in GPU compute skinning of the RT vertex stream (skin.comp):
  // descriptor-less (push constants carry buffer device addresses). Created
  // lazily on the first updateMeshSkinningGpu(); a failed creation latches
  // skinPipelineTried_ so the caller permanently falls back to CPU skinning.
  bool ensureSkinPipeline();
  VkPipelineLayout skinPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline skinPipeline_{VK_NULL_HANDLE};
  bool skinPipelineTried_{false};
};

}  // namespace lusdview
