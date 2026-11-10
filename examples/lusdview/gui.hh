// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include "compare_panel.hh"
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lightusd-cpp.hh"
#include "light3d/camera-nav.hh"
#include "frame_packet.hh"
#include "gpu_scene.hh"
#include "light3d/camera.h"  // light3d::Frustum (cull worker)
#include "imgui.h"  // ImGuiTextFilter
#include "load_control.hh"
#include "renderer.hh"
#include "scene_loader.hh"
#include "vchar_control_map.hh"

namespace lightusd {
class Prim;
namespace tydra {
struct Node;
}
}  // namespace lightusd

namespace lusdview {

// The orbit camera is shared with examples/lmetalview
// (examples/common/light3d/camera-nav.hh).
using light3d::CameraConform;
using light3d::CameraProjection;
using light3d::CameraViewPreset;
using light3d::OrbitCamera;

class Gui {
 public:
  void openCompare(int mode = -1) { comparePanel_.open(mode); }
  void setCompareContext(unsigned long long generation, bool threaded) { compareGeneration_=generation; compareThreaded_=threaded; }
  ~Gui();  // joins the per-instance cull worker

  struct LoadStatus {
    bool active{false};
    std::string path;
    long long meshesDone{0};
    long long meshesTotal{0};
    long long payloadsDone{0};
    long long payloadsTotal{0};
    long long texturesDone{0};
    long long texturesTotal{0};
    int stage{0};
    int detailPhase{0};
    float phaseProgress{0.0f};
    float elapsed{0.0f};
  };

  // GPU-side progress shown as a non-modal viewport overlay: raster geometry/
  // texture streaming (progressive upload) and the ray-tracing scene build.
  struct UploadStatus {
    bool active{false};                 // progressive raster upload in flight
    size_t meshesDone{0}, meshesTotal{0};
    size_t texDone{0}, texTotal{0};
    size_t volDone{0}, volTotal{0};
    std::string note;                   // e.g. "Building ray-tracing scene…"
  };

  struct TimelineInfo {
    bool hasAnimation{false};
    double start{0.0};
    double end{0.0};
    double fps{24.0};
    double current{0.0};
    double applied{0.0};
    bool playing{false};
    bool converting{false};
  };

  struct SkinningInfo {
    SkinningMode requested{SkinningMode::Auto};
    SkinningMode effective{SkinningMode::CPU};
    std::string reason;
  };

  enum class DomeSource : int { Off = 0, Authored = 1, File = 2,
                                WhiteFurnace = 3, SunSky = 4 };
  struct DomeLightRequest {
    DomeSource source{DomeSource::Off};
    int authoredIndex{0};
    DrawLightCPU::DomeTextureFormat format{
        DrawLightCPU::DomeTextureFormat::Automatic};
    float intensity{1.0f};
    float rotationDegrees{0.0f};
    std::string filePath;
    bool browse{false};
  };

  void setScene(const LoadedScene* loaded, const DrawScene* draw);
  bool consumeDomeLightRequest(DomeLightRequest* request) {
    if (!domeDirty_ || !request) return false;
    *request = domeRequest_;
    domeDirty_ = false;
    return true;
  }
  void setDomeFilePath(const std::string& path) {
    domeRequest_.filePath = path;
    domeRequest_.source = DomeSource::File;
    domeRequest_.browse = false;
    domeDirty_ = true;
  }
  void setDomeRequestState(const DomeLightRequest& request) {
    domeRequest_ = request;
    domeRequest_.browse = false;
    domeDirty_ = false;
  }
  void setDomeStatus(const std::string& status) { domeStatus_ = status; }
  void setNextStage(const lightusd::api::Stage& stage);
  size_t nextSkeletonOverlayVertexCount() const { return showSkeleton_ ? nextSkeletonLines_.size() : 0; }
  // StageSession owns the authoritative deferred set. The composed next Stage
  // may no longer expose payload metadata for arcs deliberately left unloaded.
  void setDeferredPayloadPaths(std::vector<std::string> paths) {
    deferredPayloadPaths_ = std::move(paths);
  }
  void setLoadStatus(const LoadStatus& s) { loadStatus_ = s; }
  void setUploadStatus(const UploadStatus& s) { upload_ = s; }
  // Re-poses the selection-highlight wireframe (rebuildSubsetHighlight) as
  // playback/scrub advances a skinned selection -- it is otherwise only
  // rebuilt on selection change, so a skinned mesh's highlight would freeze
  // at the pose it was selected at (most visibly the T-pose at frame 0)
  // while the shaded mesh keeps animating underneath.
  void setTimeline(const TimelineInfo& t) {
    const bool poseChanged = t.applied != timeline_.applied;
    timeline_ = t;
    if (poseChanged && selectionNeedsPoseRebuild()) rebuildSubsetHighlight();
  }
  void setSkinning(const SkinningInfo& s) { skinning_ = s; }
  void setCameraLens(const RtCameraLens& lens) {
    cameraLens_ = lens;
    if (lens.apertureRadius > 0.0f) {
      lastDofApertureRadius_ = lens.apertureRadius;
    }
  }
  void setCameraShutter(const RtCameraShutter& shutter) {
    cameraShutter_ = shutter;
  }
  void setRtTemporalAccumulation(bool active, bool reset) {
    rtTemporalPose_ = active;
    rtTemporalReset_ = reset;
  }
  void setCameraClippingPlanes(std::vector<float> planes) {
    cameraClippingPlanes_ = std::move(planes);
  }
  void setPathTraceSettings(const PathTraceSettings& settings) {
    pathTrace_ = settings;
  }
  const PathTraceSettings& pathTraceSettings() const { return pathTrace_; }
  void setBudget(LoadControl* b) { budget_ = b; }
  void setLoadOptions(LoadOptions* options) { loadOptions_ = options; }
  void setShowGrid(bool on) { showGrid_ = on; }
  void setShowSkeleton(bool on) { showSkeleton_ = on; }
  void setVirtualHumanProfile(bool on) { virtualHumanProfile_ = on; }
  void setAutoriggerConfigured(bool on) { autoriggerConfigured_ = on; }
  void setAutoriggerStatus(std::string status, bool running) {
    autoriggerStatus_ = std::move(status); autoriggerRunning_ = running;
  }
  bool consumeAutoriggerRequest() {
    const bool requested = autoriggerRequested_; autoriggerRequested_ = false;
    return requested;
  }
  void setMcpServerState(bool available, bool running,
                         const std::string& hostname, int port,
                         const std::string& status) {
    mcpAvailable_ = available;
    if (!mcpStateInitialized_ || running) {
      mcpPort_ = port;
      if (!mcpHostEditing_) {
        std::snprintf(mcpHostname_.data(), mcpHostname_.size(), "%s",
                      hostname.c_str());
      }
    }
    mcpRunning_ = running;
    mcpStatus_ = status;
    mcpStateInitialized_ = true;
  }
  bool consumeMcpStartRequest(std::string* hostname, int* port) {
    if (!mcpStartRequested_) return false;
    mcpStartRequested_ = false;
    if (hostname) *hostname = mcpHostname_.data();
    if (port) *port = mcpPort_;
    return true;
  }
  bool consumeMcpStopRequest() {
    const bool requested = mcpStopRequested_;
    mcpStopRequested_ = false;
    return requested;
  }
  void setDnaWorkerStatus(std::string status, bool running) {
    dnaWorkerStatus_ = std::move(status); dnaWorkerRunning_ = running;
  }
  bool consumeDnaInspectRequest(std::string* path) {
    if (!dnaInspectRequested_) return false;
    dnaInspectRequested_ = false; if (path) *path = dnaPath_.data(); return true;
  }
  void setFacialControls(std::vector<VcharControl> controls) {
    facialControls_ = std::move(controls);
    for (const VcharControl& control : facialControls_) {
      if (blendWeights_.find(control.blendshape) == blendWeights_.end()) {
        blendWeights_[control.blendshape] = control.defaultValue;
      }
    }
  }
  void setPhysicsDebugLines(std::vector<HelperVertex> lines) {
    physicsDebugLines_ = std::move(lines);
  }
  // Fixed-frame headless captures have no interactive UI. Give the render
  // viewport the full requested extent so saved dock state cannot change the
  // screenshot dimensions.
  void setCaptureViewportOnly(bool on) { captureViewportOnly_ = on; }

  // Flush the current dock/window arrangement to ImGui's configured ini file.
  bool saveImGuiLayout(std::string* err = nullptr);

  void frame(Renderer* renderer, OrbitCamera* camera);
  // Build the viewport render inputs. `packet` null (single-threaded) renders the
  // scene inline via renderer_->renderFrame; non-null (threaded) copies the inputs
  // into the packet and routes GPU side-effects (resize, instance visibility)
  // through postGpu_ for the render thread (renderFrame is NOT called here).
  void renderViewportScene(FramePacket* packet = nullptr);
  // Current viewport panel size in pixels (0 until the first frame lays it out).
  // Used by the interactive HIP/CUDA path to size its GPU trace to the viewport.
  void viewportPixelSize(int* w, int* h) const {
    if (w) *w = viewportW_;
    if (h) *h = viewportH_;
  }
  // Temporarily render an authored stereo eye into a half-width target. The
  // ImGui viewport remains full width; App composites the two eye captures back
  // into that display-sized target before the final present.
  void setFixedRenderSize(int width, int height) { fixedRenderW_ = width; fixedRenderH_ = height; }
  void setStereoEyeViewportWidth(int width) { stereoEyeViewportWidth_ = width; }
  void viewportPixelToScreen(float x, float y, float* sx, float* sy) const {
    if (sx) *sx = x + viewportImageMin_.x;
    if (sy) *sy = y + viewportImageMin_.y;
  }
  // Route a GPU op to the render thread (threaded); runs inline if unset.
  void setRenderObserver(std::function<void()> observer) { renderObserver_ = std::move(observer); }
  void setPostGpu(std::function<void(std::function<void()>)> fn) {
    postGpu_ = std::move(fn);
  }

  bool wantOpen() const { return wantOpen_; }
  // File > Open Recent: the menu populated from setRecentScenes(); when an entry
  // is clicked wantOpenRecent() is set and recentToOpen() holds its path.
  void setRecentScenes(const std::vector<std::string>& v) { recentScenes_ = v; }
  bool wantOpenRecent() const { return wantOpenRecent_; }
  const std::string& recentToOpen() const { return recentToOpen_; }
  bool wantReload() const { return wantReload_; }
  // True if the 3D viewport image was hovered last frame (used by the WebSocket
  // stream server to decide whether a browser drag navigates the camera or is an
  // ImGui widget interaction).
  bool viewportHovered() const { return vpHovered_; }
  bool wantQuit() const { return wantQuit_; }
  bool wantCancelLoad() const { return wantCancelLoad_; }
  bool wantLoadAllPayloads() const { return wantLoadAllPayloads_; }
  std::vector<std::string> takePayloadLoadRequests() {
    return std::move(payloadLoadRequests_);
  }
  bool wantVariantSwitch() const { return wantVariantSwitch_; }
  const std::map<std::string, std::map<std::string, std::string>>&
  variantOverrides() const { return variantOverrides_; }
  bool wantTogglePlay() const { return wantTogglePlay_; }
  bool wantStop() const { return wantStop_; }
  bool wantStepForward() const { return wantStepForward_; }
  bool wantStepBackward() const { return wantStepBackward_; }
  bool hasSeek() const { return hasSeek_; }
  double seekTime() const { return seekTime_; }
  bool loopPlayback() const { return loop_; }
  float playSpeed() const { return speed_; }
  float tessellationQuality() const { return tessQuality_; }
  // Effective global displacement scale (0 when displacement is disabled), for the
  // CUDA ray tracer which bakes displacement into geometry at build time.
  float displacementScale() const {
    return displacementEnabled_ ? displacementScale_ : 0.0f;
  }
  void setBackgroundPreset(bool white) {
    clearColor_[0] = clearColor_[1] = clearColor_[2] = white ? 1.0f : 0.0f;
  }
  const char* backgroundPreset() const {
    if (clearColor_[0] == 0 && clearColor_[1] == 0 && clearColor_[2] == 0) return "black";
    if (clearColor_[0] == 1 && clearColor_[1] == 1 && clearColor_[2] == 1) return "white";
    return "native";
  }
  void setPurposeFlags(bool guide, bool proxy, bool render) {
    showPurposeGuide_ = guide; showPurposeProxy_ = proxy; showPurposeRender_ = render;
  }
  uint32_t purposeVisibleMask() const {
    return (showPurposeDefault_ ? 1u : 0u) |
           (showPurposeRender_ ? 2u : 0u) |
           (showPurposeProxy_ ? 4u : 0u) |
           (showPurposeGuide_ ? 8u : 0u);
  }
  bool showSkeletonOverlay() const { return showSkeleton_; }
  // Manual blendshape weights from the blend-shape editor (Maya-like). Returns
  // the override map when manual mode is active, else nullptr (use animation).
  const std::unordered_map<std::string, float>* blendOverrides() const {
    return blendActive_ ? &blendWeights_ : nullptr;
  }
  // True once since the last call if a blendshape weight (or manual mode) changed
  // -- the app uses this to force a morph re-pose at the same time code.
  bool consumeBlendDirty() {
    const bool d = blendDirty_;
    blendDirty_ = false;
    return d;
  }
  // Programmatically set a manual blendshape weight (e.g. the --blend CLI option
  // for headless posing); enables manual mode and marks the morph dirty.
  void setBlendWeight(const std::string& name, float w) {
    blendWeights_[name] = w;
    blendActive_ = true;
    blendDirty_ = true;
  }
  enum class TransformMode { None, Translate, Rotate, Scale };
  TransformMode transformMode() const { return xformMode_; }
  void setTransformMode(TransformMode m) { xformMode_ = m; }
  void setRenderMode(RenderMode m) { mode_ = m; }
  RenderMode renderMode() const { return mode_; }
  // Advance the 'v'-key wireframe cycle (0 off / 1 wire / 2 wire+shade); returns
  // the new state. Used by the MCP input tool.
  int cycleWireframe() { wireCycle_ = (wireCycle_ + 1) % 3; return wireCycle_; }
  int wireframeMode() const { return wireCycle_; }
  bool wireXray() const { return wireXray_; }
  void setWireXray(bool on) { wireXray_ = on; }
  bool toggleWireXray() { wireXray_ = !wireXray_; return wireXray_; }
  void setCullEnabled(bool on) { cullEnabled_ = on; }
  // Offload per-instance culling to a worker thread (interactive responsiveness).
  // Disabled in headless so screenshots stay synchronous/deterministic.
  void setCullAsync(bool on);
  // Suspend culling while progressive loading mutates DrawScene::meshes.
  void setSceneMutating(bool on);
  // Prepare for a caller-driven mutation/replacement of draw_ (an async load
  // swap or a playback re-evaluation swap). Bumps the scene generation (so any
  // in-flight cull worker aborts at its next mesh boundary), joins the worker
  // (the actual memory-safety guarantee: it must not read geometry that the
  // swap frees), and drops the per-prototype instance grids (their indices
  // point into the outgoing scene's instance data). Must run on the main
  // thread, immediately before the draw_ mutation.
  void prepareSceneSwap();
  bool hasSkinningModeRequest() const { return hasSkinningModeRequest_; }
  SkinningMode requestedSkinningMode() const { return requestedSkinningMode_; }
  bool hasTechniqueRequest() const { return hasTechniqueRequest_; }
  RenderTechnique requestedTechnique() const { return requestedTechnique_; }
  bool hasCudaDeviceRequest() const { return hasCudaDeviceRequest_; }
  bool hasHipDeviceRequest() const { return hasHipDeviceRequest_; }
  int requestedCudaDevice() const { return requestedCudaDevice_; }
  int requestedHipDevice() const { return requestedHipDevice_; }
  bool hasCudaRtBackendRequest() const { return hasCudaRtBackendRequest_; }
  CudaRtBackend requestedCudaRtBackend() const {
    return requestedCudaRtBackend_;
  }
  void setCudaRtBackend(CudaRtBackend backend, bool optixAvailable) {
    cudaRtBackend_ = backend;
    optixAvailable_ = optixAvailable;
  }
  void setComputeDevices(const std::vector<std::string>& cudaDevices,
                         int cudaSelected,
                         const std::vector<std::string>& hipDevices,
                         int hipSelected) {
    cudaDevices_ = cudaDevices;
    hipDevices_ = hipDevices;
    cudaDeviceSelected_ = cudaSelected;
    hipDeviceSelected_ = hipSelected;
  }
  // The "R" keybinding (Gui::handleNavigation): toggle CPU RT on/off, restoring
  // whatever technique was active before. App::run() owns the toggle logic
  // (it tracks previousTechnique_); this is a one-shot request flag, mirroring
  // wantTogglePlay_ below.
  bool wantToggleCpuRt() const { return wantToggleCpuRt_; }
  // App calls this once per frame (before building the View menu) so the menu
  // can show which technique is currently active.
  void setActiveTechnique(RenderTechnique t) { activeTechnique_ = t; }
  // Gui caches the Renderer* handed to frame() and uses it later in the same
  // frame (renderViewportScene, viewport image, caps queries). A runtime
  // backend switch destroys and recreates that renderer BETWEEN those two
  // points, so the switch must hand over the new pointer or the rest of the
  // frame runs on freed memory.
  void setRenderer(Renderer* renderer) { renderer_ = renderer; }
  void setFrameRates(float renderFps, bool threaded) {
    renderFps_ = renderFps;
    threadedFrameRate_ = threaded;
  }
  void setAdaptiveQuality(bool enabled, float renderScale, int tier,
                          float targetFps) {
    adaptiveQuality_ = enabled;
    renderScale_ = std::max(0.25f, std::min(1.0f, renderScale));
    adaptiveTier_ = tier;
    adaptiveTargetFps_ = targetFps;
  }
  bool cameraInteractive() const { return cameraInteractive_; }
  // True once App::poseNextDrawForTracer has written POSED vertices back into
  // draw_ for a CUDA/HIP/CPU RT build. The selection-highlight re-pose must
  // then NOT deform draw_ again (it would double-deform); see
  // rebuildSubsetHighlight. Fed by App each frame from nextTracerPosedTime_.
  void setDrawIsPosed(bool posed) { drawIsPosed_ = posed; }
  // Lazy CUDA/HIP capability cache (App::cudaProbe_/hipProbe_), fed each frame
  // so the menu can gray out an item once a switch attempt has proven the
  // device unavailable. Optimistic (true) until then -- no startup probing.
  void setTechniqueAvailability(bool cudaOk, bool hipOk, bool vulkanOk,
                                bool vulkanRtOk) {
    cudaAvailable_ = cudaOk;
    hipAvailable_ = hipOk;
    vulkanAvailable_ = vulkanOk;
    vulkanRtAvailable_ = vulkanRtOk;
  }
  void clearActions() {
    wantOpen_ = wantReload_ = wantQuit_ = wantCancelLoad_ = false;
    wantOpenRecent_ = false;
    wantLoadAllPayloads_ = false;
    payloadLoadRequests_.clear();
    wantVariantSwitch_ = false;
    variantOverrides_.clear();
    wantTogglePlay_ = wantStop_ = hasSeek_ = false;
    wantStepForward_ = wantStepBackward_ = false;
    hasSkinningModeRequest_ = false;
    hasTechniqueRequest_ = false;
    hasCudaDeviceRequest_ = hasHipDeviceRequest_ = false;
    hasCudaRtBackendRequest_ = false;
    wantToggleCpuRt_ = false;
  }

  // Per-frame render stats (after the last renderViewportScene). Used by the
  // headless path to report frustum-cull effectiveness on large scenes.
  struct RenderStats {
    size_t visibleMeshes{0};
    size_t totalMeshes{0};
    size_t visibleInstances{0};
    size_t totalInstances{0};
    size_t proxyInstances{0};
    size_t drawnTriangles{0};
    size_t drawCalls{0};
  };
  RenderStats renderStats() const {
    return {statVisibleMeshes_, draw_ ? draw_->meshes.size() : 0,
            statVisibleInstances_, statTotalInstances_,
            statProxyInstances_,
            statNonInstTris_ + statInstTris_, statDrawCalls_};
  }

  // One selected prim. A merged static/material batch collapses many prims into
  // one draw, so `sourcePrim` (an index into DrawMeshCPU::sourcePrims) is what
  // makes the selection resolve to the prim under the cursor; -1 means the whole
  // draw is selected.
  struct SelectionItem {
    std::string path;
    int meshIndex{-1};
    int sourcePrim{-1};
  };
  void selectByPath(const std::string& absPath, int meshIndex,
                    int sourcePrim = -1);
  // Multi-selection. `selectionList_` holds every selected prim; the primary
  // (selPath_/selMeshIndex_/selSourcePrim_) is the focused one, used by the
  // inspector, framing and gizmo.
  const std::vector<SelectionItem>& selectionItems() const {
    return selectionList_;
  }
  bool isPathSelected(const std::string& path) const;
  // Replace the whole multi-selection (region picks, hierarchy group). Focuses
  // the first item.
  void setSelectionItems(std::vector<SelectionItem> items, bool recordHistory);
  // Ctrl+click in the hierarchy: add/remove one prim from the multi-selection.
  void toggleSelectionItem(const SelectionItem& item);
  bool focusSelectedPath(const std::string& path);
  // Hierarchy "select group": every renderable prim at or below `path`.
  void selectDescendants(const std::string& path, bool addToSelection);
  // Every prim visible inside the viewport rectangle.
  std::vector<SelectionItem> pickViewportRegionItems(float x0, float y0,
                                                     float x1, float y1) const;
  struct PickReport {
    std::string path;
    std::string kind;
    int meshIndex{-1};
    float x{0.0f};
    float y{0.0f};
    int viewportWidth{0};
    int viewportHeight{0};
    int coveredPixels{0};
    float hitDistance{0.0f};
    // Region picks report every prim inside the marquee; `path`/`meshIndex`
    // remain the focused (first) item for callers that only need one.
    std::vector<SelectionItem> items;
  };
  PickReport pickViewportPixel(float x, float y, bool select);
  PickReport pickViewportRegion(float x0, float y0, float x1, float y1,
                                bool select);
  bool isolateSelected();
  void showAllRenderables();
  void clearSelection();
  void requestVariantSwitch(
      const std::string& primPath,
      const std::map<std::string, std::string>& selections) {
    variantOverrides_[primPath] = selections;
    wantVariantSwitch_ = true;
  }
  const std::string& selectedPath() const { return selPath_; }
  int selectedMeshIndex() const { return selMeshIndex_; }
  struct OpenPbrMaterialEdit {
    int materialId{-1};
    DrawLightRtOpenPBRCPU constants;
    bool makeConstant{false};
  };
  bool takeOpenPbrMaterialEdit(OpenPbrMaterialEdit* edit) {
    if (!hasOpenPbrMaterialEdit_) return false;
    if (edit) *edit = openPbrMaterialEdit_;
    hasOpenPbrMaterialEdit_ = false;
    return true;
  }
  const std::vector<uint8_t>& viewVisibility() const { return viewVisible_; }
  struct TextureResidencyInfo {
    size_t residentBytes{0};
    size_t budgetBytes{0};
    size_t resident{0};
    size_t queued{0};
    size_t total{0};
    bool backgroundRefinement{true};
  };
  void setTextureResidencyInfo(const TextureResidencyInfo& info) {
    textureResidencyInfo_ = info;
  }
  bool wantRefineSelectedTextures() const { return refineSelectedTextures_; }
  bool wantReleaseSelectedTextures() const { return releaseSelectedTextures_; }
  bool backgroundTextureRefinement() const {
    return textureResidencyInfo_.backgroundRefinement;
  }
  void saveCameraBookmark(int slot);
  bool loadCameraBookmark(int slot);
  bool hasCameraBookmark(int slot) const;
  bool canGoSelectionBack() const;
  bool canGoSelectionForward() const;
  bool goSelectionBack();
  bool goSelectionForward();
  // Move the thin-lens focal plane to the last clicked point on the current
  // selection. Hierarchy/region selections fall back to their world bounds.
  bool focusDofOnSelection(float* focusDistance = nullptr);
  float dofFocusDistance() const { return cameraLens_.focusDistance; }
  const RtCameraLens& cameraLens() const { return cameraLens_; }
  const RtCameraShutter& cameraShutter() const { return cameraShutter_; }
  const std::vector<float>& cameraClippingPlanes() const {
    return cameraClippingPlanes_;
  }

 private:
  void applySelection(const std::string& absPath, int meshIndex,
                      bool recordHistory, int sourcePrim = -1);
  void pushSelectionHistory(const std::string& absPath);
  void drawDockspaceAndMenu();
  void buildDefaultLayout(unsigned int dockId);
  void drawHierarchy();
  bool drawNextPrimTree(lightusd_prim prim);
  void drawNextInspector();
  void drawInspector();
  // Maya-like blendshape weight editor for the selected prim (shown when the
  // selection, an ancestor, or a descendant mesh carries blendshape targets).
  void drawBlendShapeEditor();
  void drawVirtualHumanPanel();
  void drawSelectionList();
  void drawCameraPanel();
  void drawDomeLightPanel();
  void drawStats();
  void drawPayloads();
  void drawTimeline();
  void drawMaterialsPanel();
  void drawOpenPbrMaterialPanel();
  void drawCompositionGraph();
  void drawViewport();
  void drawAboutModal();
  void drawMcpServerPanel();
  void drawLoadingModal();
  void drawProgressOverlay();  // non-modal GPU upload / RT-build progress
  void drawStageMeta();
  void drawNavigationOverlay(const ImVec2& imageMin, const ImVec2& imageMax);
  void cycleNavigationHelpMode();
  void drawSelectionBreadcrumbs(const char* idSuffix);
  bool framePath(const std::string& absPath);
  bool selectedFocusPoint(light3d::Vec3* point,
                          bool* fromViewportPick = nullptr) const;
  void handleNavigation();
  void buildHelpers();
  bool meshPurposeVisible(const std::string& purpose) const;
  bool meshVisibleForView(size_t meshIndex) const;
  bool carrierVisibleForView(size_t carrierIndex) const;
  size_t carrierIndexForPath(const std::string& path) const;
  bool pathInSelection(const std::string& path) const;
  bool selectionHasRenderable() const;
  void isolateSelection();
  void setSelectionVisibility(bool visible);
  void buildViewVisibilityMask();
  void setSelectionListSingle(const std::string& absPath, int meshIndex);
  void setSelectionListFromMeshes(std::vector<int> meshIndices);
  void setSelectionListFromPaths(std::vector<std::string> paths);
  void focusSelectionListItem(size_t index);
  void beginRegionSelection(const ImVec2& mouse);
  void updateRegionSelection(const ImVec2& mouse);
  void finishRegionSelection(const ImVec2& imageMin);
  void beginPointSelection(const ImVec2& mouse);
  void updatePointSelection(const ImVec2& mouse);
  void finishPointSelection(const ImVec2& imageMin);
  std::vector<int> regionPickMeshes(const ImVec2& imageMin, int vpW, int vpH) const;
  bool meshIntersectsScreenRect(size_t meshIndex, const ImVec2& rectMin,
                                const ImVec2& rectMax, int vpW, int vpH) const;
  int pickMesh(float px, float py, int vpW, int vpH,
               float* hitDistance = nullptr,
               int* hitSourcePrim = nullptr) const;
  // Single-ray implementation used by the pixel-center + cursor aperture in
  // pickMesh(). Kept separate so both rays share the same mesh logic.
  int pickMeshSingle(
      float px, float py, int vpW, int vpH, float* hitDistance,
      const std::unordered_map<int, std::vector<DrawVertex>>* posedCache,
      int* hitSourcePrim = nullptr) const;
  // Pose skinned/morphed meshes at the current timeline time for exact
  // ray/triangle picking. The raster path deforms on the GPU, so draw_'s
  // `vertices` are the rest pose; picking them directly would select the wrong
  // surface (or miss entirely) on animated characters.
  // Displayed-pose vertices for deformable meshes, cached until the time code or
  // the scene changes. Re-posing every skinned mesh on every pick cost ~0.4 s a
  // click on a scene with thousands of skinned meshes.
  const std::unordered_map<int, std::vector<DrawVertex>>* posedPickVertices()
      const;
  mutable std::unordered_map<int, std::vector<DrawVertex>> posedPickCache_;
  mutable double posedPickCacheTime_{-1.0e30};
  mutable uint64_t posedPickCacheGen_{0};
  std::string pickCarrierPath(float px, float py, int vpW, int vpH,
                              float* hitDistance = nullptr,
                              int* hitMeshIndex = nullptr,
                              int* hitSourcePrim = nullptr) const;
  void selectAdjacentMesh(int step);
  void applyViewPreset(CameraViewPreset preset);
  void homeView();
  void frameSelected();
  void frameAll();
  void unhideAll();

  Renderer* renderer_{nullptr};
  OrbitCamera* cam_{nullptr};
  RtCameraLens cameraLens_;
  RtCameraShutter cameraShutter_;
  bool rtTemporalPose_{false};
  bool rtTemporalReset_{false};
  std::vector<float> cameraClippingPlanes_;
  float lastDofApertureRadius_{0.0f};
  PathTraceSettings pathTrace_;
  const LoadedScene* loaded_{nullptr};
  const DrawScene* draw_{nullptr};
  DomeLightRequest domeRequest_;
  struct DomeInfo {
    std::string label;
    DrawLightCPU::DomeTextureFormat format{
        DrawLightCPU::DomeTextureFormat::Automatic};
  };
  std::vector<DomeInfo> authoredDomes_;
  bool domeDirty_{false};
  bool showDomeLight_{true};
  std::string domeStatus_;

  struct CameraBookmark {
    bool valid{false};
    light3d::Vec3 target{0.0f, 0.0f, 0.0f};
    float yaw{0.0f};
    float pitch{0.0f};
    float distance{1.0f};
    std::string selectedPath;
  };

  lightusd::api::Stage nextStage_;
  std::vector<std::string> nextSkeletonPaths_;
  bool nextSkeletonPathsReady_{false};
  double nextSkeletonTime_{0};
  bool nextSkeletonTimeValid_{false};
  std::vector<HelperVertex> nextSkeletonLines_;
  std::vector<std::string> deferredPayloadPaths_;
  std::string selPath_;
  int selMeshIndex_{-1};
  // When a viewport pick lands inside a merged batch, the index of the source
  // prim (DrawMeshCPU::sourcePrims) that owns the hit triangle. The highlight
  // then outlines just that prim instead of the whole batch. -1 = whole draw.
  int selSourcePrim_{-1};
  int openPbrEditorMaterial_{-1};
  int openPbrEditorSelectionMesh_{-2};
  bool openPbrDockAttempted_{false};
  bool hasOpenPbrMaterialEdit_{false};
  OpenPbrMaterialEdit openPbrMaterialEdit_;
  light3d::Vec3 lastPickedFocusPoint_{0.0f, 0.0f, 0.0f};
  std::string lastPickedFocusPath_;
  bool haveLastPickedFocusPoint_{false};
  // When the selection is a GeomSubset, the triangle vertex indices of its faces
  // (built via the parent mesh's sourceFaceId) so the highlight outlines just the
  // subset; highlightSubsetMesh_ is that parent mesh's draw index.
  std::vector<uint32_t> highlightSubsetIndices_;
  int highlightSubsetMesh_{-1};
  // World-space orange edge lines of the highlighted triangles (whole mesh or
  // subset) for the Vulkan backend's line-pipeline highlight.
  std::vector<HelperVertex> highlightLinesData_;
  // 0 off, 1 selected bounds, 2 depth-tested wireframe, 3 X-ray wireframe.
  int selectionHighlightMode_{2};
  int selectionHighlightLastVisibleMode_{2};
  void rebuildSubsetHighlight();
  // Outline one draw with world-space orange edge lines (whole draw, one batched
  // source prim, or an explicit triangle list). Shared by single and multi
  // selection.
  void appendMeshHighlightLines(
      int meshIndex, const std::vector<uint32_t>* triOverride, int sourcePrim,
      const std::unordered_map<int, std::vector<DrawVertex>>* posedCache);
  // Outline a Points/Curves carrier selection.
  void appendCarrierHighlightLines(const std::string& path);
  // True when a timeline change actually needs the CPU selection-highlight
  // geometry rebuilt (animated selection whose backend does not re-pose it).
  bool selectionNeedsPoseRebuild() const;

  // Blendshape editor state. blendWeights_ is keyed by BlendShape name; when
  // blendActive_ it overrides the SkelAnimation-driven weights in the morph pass.
  std::unordered_map<std::string, float> blendWeights_;
  bool blendActive_{false};
  bool blendDirty_{false};
  std::vector<SelectionItem> selectionList_;
  std::array<CameraBookmark, 3> cameraBookmarks_{};
  std::vector<std::string> selectionHistory_;
  int selectionHistoryIndex_{-1};

  RenderMode mode_{RenderMode::Shaded};
  // Wireframe overlay state cycled by the 'v' key: 0 off / 1 wire-only / 2 wire+shaded.
  int wireCycle_{0};
  // When false (default) scene wireframe is depth tested; when true it draws
  // through surfaces (x-ray). Toggled with X / View > Wireframe X-ray.
  bool wireXray_{false};
  int curveMaxSegments_{8};
  std::array<float, 16> lastCurveView_{};
  bool haveLastCurveView_{false};
  double lastCurveViewChangeTime_{-1.0};
  float renderFps_{0.0f};
  bool threadedFrameRate_{false};
  bool adaptiveQuality_{false};
  bool cameraInteractive_{false};
  float renderScale_{1.0f};
  float adaptiveTargetFps_{30.0f};
  int adaptiveTier_{0};
  // Transform manipulator mode.
  TransformMode xformMode_{TransformMode::None};
  int gizmoAxis_{-1};         // -1 = none, 0=X, 1=Y, 2=Z
  bool gizmoActive_{false};   // currently dragging the gizmo
  ImVec2 gizmoMouseStart_;    // mouse position at drag start
  float gizmoStartPos_[3];    // world position at drag start
  bool dockBuilt_{false};
  bool resetLayoutRequested_{false};

  ImGuiTextFilter hierFilter_;
  ImGuiTextFilter propFilter_;
  ImGuiTextFilter metaFilter_;

  // View menu toggles
  int fixedRenderW_{0}, fixedRenderH_{0};
  bool showGrid_{true};
  bool showAxes_{true};
  bool showSceneBbox_{false};
  bool showSkeleton_{true};
  bool virtualHumanProfile_{false};
  std::vector<VcharControl> facialControls_;
  bool autoriggerConfigured_{false};
  bool autoriggerRequested_{false};
  bool autoriggerRunning_{false};
  std::string autoriggerStatus_;
  std::array<char, 1024> dnaPath_{};
  bool dnaInspectRequested_{false};
  bool dnaWorkerRunning_{false};
  std::string dnaWorkerStatus_;
  bool showLights_{false};
  bool showCameras_{false};
  bool showExtent_{false};
  bool showPrimLabels_{false};
  bool showPurposeDefault_{true};
  bool showPurposeRender_{true};
  // Render and proxy are alternate representations. Keep proxy geometry
  // available for inspection/picking, but do not draw it over render-purpose
  // geometry until the user explicitly enables the Proxy purpose.
  bool showPurposeProxy_{false};
  bool showPurposeGuide_{false};
  enum class NavigationHelpMode { None, Simple, Full };
  NavigationHelpMode navigationHelpMode_{NavigationHelpMode::None};
  bool showAbout_{false};
  bool showMcpServer_{false};
  bool mcpAvailable_{false};
  bool mcpStateInitialized_{false};
  bool mcpRunning_{false};
  bool mcpHostEditing_{false};
  bool mcpStartRequested_{false};
  bool mcpStopRequested_{false};
  std::array<char, 256> mcpHostname_{{'1','2','7','.','0','.','0','.','1','\0'}};
  int mcpPort_{8080};
  std::string mcpStatus_;
  bool cullEnabled_{true};  // per-mesh + per-instance frustum culling (View menu)
  // Per-frame render stats (computed in buildViewVisibilityMask + the per-instance
  // cull pass; rendered by drawStats). "visible" reflects frustum culling.
  size_t statVisibleMeshes_{0};
  size_t statTotalInstances_{0};
  size_t statVisibleInstances_{0};  // owned by cullInstances
  size_t statProxyInstances_{0};    // aggregate/non-instanced box LOD proxies
  size_t statNonInstTris_{0};       // visible non-instanced triangles (per-mesh pass)
  size_t statInstTris_{0};          // visible instanced triangles (cullInstances)
  size_t statDrawCalls_{0};
  // Per-instance frustum culling (A4): compact each instanced prototype's visible
  // instances into the renderer's instance buffer. Re-run only when the view or
  // cull state changes (cullInstances), keyed by lastCull*. The CPU frustum
  // test + compaction runs on a WORKER thread (Stage-1 offload, large instanced
  // scenes); cullInstances() polls the worker, applies its compacted result on the
  // main thread (updateInstanceVisibility is a GPU call), and relaunches on change.
  // Run a GPU op on the render thread when threaded, else inline.
  ComparePanel comparePanel_;
  unsigned long long compareGeneration_{0};
  bool compareThreaded_{false};
  std::function<void()> renderObserver_;
  std::function<void(std::function<void()>)> postGpu_;
  void gpu(std::function<void()> op) {
    if (postGpu_) postGpu_(std::move(op)); else op();
  }
  void cullInstances();
  void cullInstancesSync();  // synchronous compaction + apply (headless / cullAsync_ off)
  void joinCullWorker();     // join + reset; called from setScene + ~Gui
  void cullWorkerMain();     // runs on the worker thread (CPU only, reads snapshots)
  bool cullAsync_{true};
  bool sceneMutating_{false};
  // Monotonic scene identity. The cull worker snapshots it at launch
  // (cullJobGen_) and aborts if it changes mid-run, so a scene swap that races
  // the worker cannot feed it torn mesh data. Incremented by setScene,
  // setSceneMutating, and prepareSceneSwap (all of which also join the worker).
  std::atomic<uint64_t> cullSceneGen_{0};
  uint64_t cullJobGen_{0};   // generation the running worker snapshotted
  bool cullAborted_{false};  // worker saw a generation change and bailed early
  float lastCullVP_[16]{};
  bool lastCullValid_{false};
  bool lastCullEnabled_{false};
  bool lastCullRasterLod_{false};
  bool lastCullWireMode_{false};
  const DrawScene* lastCullDraw_{nullptr};
  // One mesh's compacted visible instances, produced by the worker, applied on main.
  struct CullJobMesh {
    size_t meshIndex{0};
    std::vector<float> xforms;   // 12 floats/visible-instance
    std::vector<float> colors;   // 3 floats/visible-instance (empty when none)
    std::vector<float> opacities;  // 1 float/visible-instance (empty when none)
    uint32_t count{0};
    std::array<uint32_t, 4> lodCounts{{0, 0, 0, 0}};
    bool hasColors{false};
    bool hasOpacities{false};
  };
  // Frustum-test + compact one instanced mesh's visible instances into `out`
  // (shared by the sync + worker paths). cullEnabled=false -> the full set. When
  // `grid` is non-null (a coarse per-prototype instance grid), whole off-screen
  // cells are rejected and fully-inside cells accepted without per-instance tests
  // -- so cull cost scales with the visible cell set, not the total instance count.
  // When lodCam.lodEnabled, instances are also size-classified: sub-pixel ones are
  // dropped and (with proxyOut + lodCam.proxyEnabled) small ones become box proxies
  // appended to proxyOut (accumulated across prototypes by the caller).
  static void compactMeshInstances(const DrawMeshCPU& m, const light3d::Frustum& fr,
                                   bool cullEnabled, const RtLodGrid* grid,
                                   const RtLodCamera& lodCam, CullJobMesh* out,
                                   CullJobMesh* proxyOut);
  // Raster view-dependent LOD (optimization B): drop sub-pixel instances + collapse
  // small ones to shared box proxies, cutting both the uploaded instance count and
  // the rasterised geometry. Off by default (exact parity). proxyEnabled is gated on
  // the renderer supporting the box-proxy draw (GL); else cull-only.
  bool rasterLodEnabled_{false};
  bool dynamicRasterLodEnabled_{false};
  float dynamicRasterLodErrorPx_{0.0f};
  int dynamicRasterLodTier_{0};
  float rasterLodFullPx_{48.0f};
  float rasterLodCullPx_{1.5f};
  CullJobMesh proxyResult_;        // accumulated box proxies (sync path / applied)
  RtLodCamera cullJobLodCam_;      // snapshot for the worker
  CullJobMesh cullJobProxy_;       // worker-accumulated box proxies
  // Box proxies for NON-instanced meshes. Built on the main thread in
  // buildViewVisibilityMask (the instance cull never sees these meshes), and
  // prepended to whichever proxy set cullInstances uploads.
  CullJobMesh nonInstProxy_;
  // Merge nonInstProxy_ with the instance-cull's proxies (consumed) and upload the
  // union as the one shared box-proxy draw. Skips the upload when unchanged, since
  // the non-instanced set is rebuilt every frame while the instance cull is gated.
  void uploadProxies(CullJobMesh* instProxy);
  std::vector<float> lastProxyXforms_, lastProxyColors_;
  bool lastProxyValid_{false};
 public:
  void setRasterLod(bool on, float fullPx, float cullPx) {
    rasterLodEnabled_ = on;
    if (fullPx > 0.f) rasterLodFullPx_ = fullPx;
    if (cullPx >= 0.f) rasterLodCullPx_ = cullPx;
  }
  void setDynamicRasterLod(bool on, float errorPx) {
    if (dynamicRasterLodEnabled_ != on ||
        dynamicRasterLodErrorPx_ != errorPx) {
      lastCullValid_ = false;
    }
    dynamicRasterLodEnabled_ = on;
    dynamicRasterLodErrorPx_ = std::max(0.0f, errorPx);
  }
  void setDynamicRasterLodTier(int tier) { dynamicRasterLodTier_ = tier; }
  void notifyPrototypeLodsChanged() { lastCullValid_ = false; }
 private:
  // Build the LOD camera (thresholds + projection focal length) for this cull.
  RtLodCamera buildRasterLodCam() const;
  // Coarse instance grids (one per mesh; empty/invalid for non-instanced or small
  // prototypes), built once per scene for compactMeshInstances cell rejection.
  // Read-only after build, so the cull worker shares them via cullJobGrids_.
  std::vector<RtLodGrid> instGrids_;
  const DrawScene* instGridsFor_{nullptr};
  void ensureInstanceGrids(uint64_t gen);  // (re)build instGrids_ when draw_ changes
  const std::vector<RtLodGrid>* cullJobGrids_{nullptr};
  std::thread cullThread_;
  std::atomic<bool> cullRunning_{false};
  std::atomic<bool> cullDone_{false};
  std::vector<CullJobMesh> cullJobResult_;     // worker writes, main reads after done
  size_t cullJobVisInstances_{0};
  size_t cullJobInstTris_{0};
  // Worker inputs snapshotted by main before launch (so the worker races nothing).
  light3d::Mat4 cullJobVP_{};
  std::vector<uint8_t> cullJobViewVisible_;
  bool cullJobEnabled_{false};
  const DrawScene* cullJobDraw_{nullptr};
  float tessQuality_{1.0f};
  // UsdPreviewSurface displacement (raster preview). enabled = master toggle;
  // scale = global multiplier; maxTessLevel > 1 enables GPU tessellation for
  // adaptive sub-triangle detail (1 = coarse per-vertex displacement only).
  bool captureViewportOnly_{false};
  bool displacementEnabled_{true};
  float displacementScale_{1.0f};
  int maxTessLevel_{1};
  std::vector<HelperVertex> helperLines_;
  std::vector<HelperVertex> overlayLines_;
  std::vector<HelperVertex> physicsDebugLines_;

  std::vector<uint8_t> meshVisible_;
  std::vector<uint8_t> carrierVisible_;
  std::vector<uint8_t> viewVisible_;
  TextureResidencyInfo textureResidencyInfo_;
  bool refineSelectedTextures_{false};
  bool releaseSelectedTextures_{false};
  bool revealSelectionInHierarchy_{false};

  std::string inspectorCachePath_;

  bool vpHovered_{false};
  int navMode_{0};
  int viewportW_{0};
  int viewportH_{0};
  int stereoEyeViewportWidth_{0};
  ImVec2 viewportImageMin_{0.0f, 0.0f};
  bool regionSelecting_{false};
  bool regionSelectionMoved_{false};
  ImVec2 regionStart_{0.0f, 0.0f};
  ImVec2 regionEnd_{0.0f, 0.0f};
  // Plain LMB drag is a point/surface pick. Region (marquee) selection is
  // reserved for Shift+LMB drag so the common gesture never turns into a
  // rubber band by accident.
  bool pointSelecting_{false};
  bool pointSelectionMoved_{false};
  ImVec2 pointStart_{0.0f, 0.0f};
  ImVec2 pointEnd_{0.0f, 0.0f};
  // Grid/axes half-extent frozen per scene so an animated pose changing the
  // scene bounds during playback does not rescale (wobble) the ground grid.
  float helperHalfExtent_{10.0f};
  bool helperHalfExtentValid_{false};

  bool wantOpen_{false};
  bool wantOpenRecent_{false};
  std::string recentToOpen_;
  std::vector<std::string> recentScenes_;  // newest first (from App config)
  bool wantReload_{false};
  bool wantQuit_{false};
  bool wantCancelLoad_{false};
  bool wantLoadAllPayloads_{false};
  std::vector<std::string> payloadLoadRequests_;
  bool wantVariantSwitch_{false};
  std::map<std::string, std::map<std::string, std::string>> variantOverrides_;

  TimelineInfo timeline_;
  SkinningInfo skinning_;
  bool hasSkinningModeRequest_{false};
  SkinningMode requestedSkinningMode_{SkinningMode::Auto};
  // Runtime backend switch (View > Render Technique submenu + the CPU RT
  // keybinding, both in gui.cc): mirrors hasSkinningModeRequest_ above. App
  // has no Gui back-pointer, so the request lives here; App::run() drains it
  // once per frame via the accessors below, applies it, then clears it in
  // clearActions() alongside the other one-shot UI requests.
  bool hasTechniqueRequest_{false};
  RenderTechnique requestedTechnique_{RenderTechnique::GLRaster};
  RenderTechnique activeTechnique_{RenderTechnique::GLRaster};  // fed back by App each frame
  std::vector<std::string> cudaDevices_;
  std::vector<std::string> hipDevices_;
  int cudaDeviceSelected_{0};
  int hipDeviceSelected_{0};
  int requestedCudaDevice_{0};
  int requestedHipDevice_{0};
  bool hasCudaDeviceRequest_{false};
  bool hasHipDeviceRequest_{false};
  CudaRtBackend cudaRtBackend_{CudaRtBackend::Auto};
  CudaRtBackend requestedCudaRtBackend_{CudaRtBackend::Auto};
  bool hasCudaRtBackendRequest_{false};
  bool optixAvailable_{false};
  bool cudaAvailable_{true};
  bool hipAvailable_{true};
  bool vulkanAvailable_{false};
  bool vulkanRtAvailable_{false};
  bool drawIsPosed_{false};
  bool wantToggleCpuRt_{false};
  bool wantTogglePlay_{false};
  bool wantStop_{false};
  bool wantStepForward_{false};
  bool wantStepBackward_{false};
  bool hasSeek_{false};
  double seekTime_{0.0};
  bool loop_{true};
  float speed_{1.0f};
  float clearColor_[4]{0.12f, 0.12f, 0.13f, 1.0f};

  LoadStatus loadStatus_;
  UploadStatus upload_;
  std::string imguiLayoutStatus_;
  LoadControl* budget_{nullptr};
  LoadOptions* loadOptions_{nullptr};
};

}  // namespace lusdview
