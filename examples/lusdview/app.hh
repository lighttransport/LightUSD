// SPDX-License-Identifier: Apache-2.0
// lusdview - application: owns the window, renderer, scene and GUI; runs the
// main loop.
#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame_packet.hh"
#include "viewer_document.hh"
#include "lightusd-render-cpp.hh"

#include "camera_nav.hh"
#include "cpu/cpu_raytracer.hh"
#include "cuda/cuda_raytracer.hh"
#include "gpu_scene.hh"
#include "hip/hip_raytracer.hh"
#include "rt_scene_build.hh"  // BuildProgress (threaded RT build)
#include "gui.hh"
#include "load_control.hh"
#include "parametric_tess.hh"
#include "ptex_atlas.hh"
#include "prototype_lod.hh"
#include "renderer.hh"
#include "rt_camera.hh"
#include "tydra/rb-dynamics.hh"
#include "scene_loader.hh"
#include "stream/stream_server.hh"
#include "../vchar/autorigger_client.hh"
#if defined(LUSDVIEW_HAVE_MCP)
#include "mcp/mcp_host.hh"
#include "mcp/mcp_server.hh"
#include "tydra/js-script.hh"    // complete JSEngineState (held in Context)
#include "tydra/mcp-context.hh"  // lightusd::tydra::mcp::Context (library tool bridge)
#endif

struct GLFWwindow;

namespace lightusd { namespace next { class Stage; } }
namespace lusdview {
class ViewerDocument;

// Encode an RGBA8 (top-down) buffer to an image file; format chosen by extension
// (.png/.ppm). Defined in app.cc. Declared here so the MCP screenshot tool can
// reuse it.
bool WriteScreenshotImage(const std::string& path, const std::vector<uint8_t>& rgba,
                          int w, int h, std::string* err);

class App
#if defined(LUSDVIEW_HAVE_MCP)
    : public McpHost
#endif
{
 public:
  explicit App(Backend backend);
  ~App() override;

  // Optional render budget (for scripting/testing). maxTris==0 keeps the default.
  void setLoadBudget(std::size_t maxTris, double convertTimeBudgetSec) {
    if (maxTris) loadCtrl_.maxTriangles = maxTris;
    loadCtrl_.convertTimeBudgetSec = convertTimeBudgetSec;
  }

  // GPU-budget LOD for the realtime raster preview (huge assembled scenes).
  // gpuMemBudgetBytes>0 caps full-mesh VRAM; maxFullMeshes>0 caps the full-mesh
  // (draw) count; overflow meshes merge into one bbox-proxy soup. 0/0 = off.
  void setGpuBudget(std::size_t gpuMemBudgetBytes, std::size_t maxFullMeshes) {
    gpuMemBudgetBytes_ = gpuMemBudgetBytes;
    maxFullMeshes_ = maxFullMeshes;
  }

  // Robust auto-framing: trim horizon-scale outlier meshes from the fit-all bbox
  // (default on). --no-robust-frame disables it to frame the literal scene bbox.
  void setRobustFrame(bool on) { robustFrame_ = on; }

  // View-dependent RT LOD (--rt-lod): classify instances by projected screen size
  // when the camera settles; drop sub-pixel/off-screen instances from the TLAS
  // (and, from P2, render distant ones as box proxies). fullPx/cullPx are the
  // projected-radius thresholds in pixels.
  void setRtLod(bool enabled, float fullPx, float cullPx, float bandFrac = -1.f) {
    rtLodEnabled_ = enabled;
    if (fullPx > 0.f) rtLodFullPx_ = fullPx;
    if (cullPx >= 0.f) rtLodCullPx_ = cullPx;
    if (bandFrac >= 0.f) rtLodBandFrac_ = bandFrac;
  }

  // Raster view-dependent LOD (optimization B): drop sub-pixel instances + render
  // distant ones as box proxies, on the raster instanced path. Applied to gui_ each
  // frame. fullPx/cullPx are projected-radius thresholds in pixels.
  void setRasterLod(bool enabled, float fullPx, float cullPx) {
    rasterLodEnabled_ = enabled;
    if (fullPx > 0.f) rasterLodFullPx_ = fullPx;
    if (cullPx >= 0.f) rasterLodCullPx_ = cullPx;
  }
  void setDynamicPrototypeLod(bool enabled) {
    dynamicPrototypeLodEnabled_ = enabled;
  }

  // HiDPI UI scale (font + widget sizes). Auto-detected from the monitor at
  // startup (1.0 on standard-density displays, 2.0 on HiDPI / >=2K panels) unless
  // explicitly set via --ui-scale / --font-size / --window-scale.
  void setUiScale(float s) {
    if (s > 0.25f) {
      uiScale_ = s;
      fontSizePx_ = 16.0f * s;
      windowScale_ = s;
      uiScaleExplicit_ = true;
    }
  }
  void setFontSize(float px) {
    if (px > 4.0f) {
      fontSizePx_ = px;
      uiScale_ = px / 16.0f;
      uiScaleExplicit_ = true;
    }
  }
  void setWindowScale(float s) {
    if (s > 0.25f) { windowScale_ = s; uiScaleExplicit_ = true; }
  }
  void setWindowSize(int width, int height) {
    if (width > 0 && height > 0) {
      hasWindowSizeOverride_ = true;
      windowWidth_ = width;
      windowHeight_ = height;
    }
  }
  void setDefaultWindowSize(int width, int height) {
    if (width > 0 && height > 0) {
      windowWidth_ = width;
      windowHeight_ = height;
    }
  }
  void clearWindowSizeOverride() { hasWindowSizeOverride_ = false; }
  void setOrbitSensitivity(float s) { camera_.setOrbitSensitivity(s); }
  void setPanSensitivity(float s) { camera_.setPanSensitivity(s); }
  void setDollySensitivity(float s) { camera_.setDollySensitivity(s); }
  void setInvertYaw(bool on) { camera_.setInvertYaw(on); }
  void setInvertDolly(bool on) { camera_.setInvertDolly(on); }

  // USD composition behavior for subsequent loads (set from CLI/config before
  // run(); payload whitelist is managed internally by recompose).
  void setLoadOptions(const LoadOptions& o) { loadOpts_ = o; }
  const LoadOptions& loadOptions() const { return loadOpts_; }
  void setUploadBudgetMs(double ms) { uploadBudgetMs_ = ms; }
  void setQuitAfterFullUpload(bool on) { quitAfterFullUpload_ = on; }
  void setQuitAfterConvert(bool on) {
    quitAfterConvert_ = on;
    // Conversion-only validation must include ordinary texture decode and
    // compression even when an interactive preset enabled deferred decode.
    loadOpts_.forceTextureDecode = on;
  }

  // Use the `next` lazy loader + tydra-next converter (flat-shaded large-scene
  // mesh preview) instead of the default Tydra path. See next_scene_loader.cc.
  void setUseNextLoader(bool on) { useNextLoader_ = on; }
  void setCullEnabled(bool on) { gui_.setCullEnabled(on); }
  void setShowGrid(bool on) { gui_.setShowGrid(on); }
  void setShowSkeleton(bool on) { gui_.setShowSkeleton(on); }
  void setVirtualHumanProfile(bool on) {
    virtualHumanProfile_ = on;
    gui_.setVirtualHumanProfile(on);
  }
  void setAutoriggerExecutable(const std::string& path) {
    autoriggerExecutable_ = path; gui_.setAutoriggerConfigured(!path.empty());
  }
  void setCamDolly(float f) { camDolly_ = f; }
  void setAdaptiveQuality(bool on, float targetFps, float minScale) {
    adaptiveQuality_ = on;
    adaptiveTargetFps_ = std::max(1.0f, targetFps);
    adaptiveMinScale_ = std::max(0.25f, std::min(1.0f, minScale));
  }
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // --threaded: run GL rendering on a dedicated thread so the UI loop never blocks
  // on GPU work (experimental; default off). No-op unless built with the option.
  void setThreaded(bool on) { threaded_ = on; }
#else
  void setThreaded(bool /*on*/) {}
#endif

  // Request the Vulkan ray-tracing technique at startup (honored only when the
  // device supports it; otherwise the viewer stays on rasterization).
  void setRequestRayTracing(bool on) { rtRequested_ = on; }
  void setAllowBackendFallback(bool on) { allowBackendFallback_ = on; }
  void setDevicePreference(const RendererDevicePreference& preference) {
    devicePreference_ = preference;
  }
  void setTransparencyMode(TransparencyMode mode) { transparencyMode_ = mode; }
  void setRasterQualityHigh(bool high) { rasterQualityHigh_ = high; }
  void setStartupEnvmap(const std::string& path, float intensity,
                        float rotationDegrees) {
    startupEnvmapPath_ = path;
    startupEnvmapIntensity_ = intensity;
    startupEnvmapRotationDegrees_ = rotationDegrees;
  }
  void setSkinningMode(SkinningMode mode) { skinningRequested_ = mode; }
  void setPlayAnimation(bool on) { playRequested_ = on; }

  // Write a PPM of the full composited window after the last frame (QA).
  void setWindowShot(const std::string& path) { windowShot_ = path; }

  // Windowless rendering: no GLFW window, surface or swapchain (Vulkan only).
  // Renders frames offscreen for --screenshot / --window-shot. Requires --frames.
  void setHeadless(bool on) { headless_ = on; }
  // --cuda: trace the screenshot with the CUDA BVH ray tracer (cuew runtime).
  void setCudaRt(bool on) { cudaRt_ = on; }
  void setCudaRtBackend(CudaRtBackend backend) {
    cudaTracer_.setRtBackend(backend);
  }
  void setCudaCacheDir(const std::string& path) {
    cudaTracer_.setCacheDirectory(path);
  }
  // --hip: trace the screenshot with the HIP/ROCm BVH ray tracer (hipew runtime).
  void setHipRt(bool on) { hipRt_ = on; }
  // --cpu-rt: trace with the CPU (lightrt_c) ray tracer. Windowed: interactive,
  // same shape as --hip/--cuda. Headless + --screenshot: a one-shot trace (no
  // device-async upload step needed, so it's simpler than CUDA/HIP's path).
  void setCpuRt(bool on) { cpuRt_ = on; }
  // --rt-samples N: supersampled AA for the CUDA/HIP screenshot path (1 = off).
  void setRtSamples(int n) { rtSamples_ = n < 1 ? 1 : n; }
  void setPathTraceSettings(PathTraceSettings settings) {
    settings.sanitize();
    pathTrace_ = settings;
    gui_.setPathTraceSettings(pathTrace_);
  }
  void setLinearOutput(const std::string& path) { linearOutput_ = path; }
  void setLensOverrides(float fStop, float focusDistance) {
    lensFStopOverride_ = fStop;
    lensFocusOverride_ = focusDistance;
  }
  // Write a stable machine-readable summary when run() exits.
  void setRenderReport(const std::string& path) { renderReportPath_ = path; }
  void setCheckpointOutput(int every, const std::string& pattern) {
    checkpointEvery_ = every > 0 ? every : 0;
    checkpointPattern_ = pattern;
  }
  // Name of the resolved large-scene production profile (off/auto/island/etc.).
  // Kept in the report so captures are reproducible without parsing logs.
  void setLargeSceneProfile(const std::string& profile) {
    largeSceneProfile_ = profile;
  }
  // --max-instances N: cap the CUDA/HIP 2-level-BVH instance count (0 = no cap).
  void setRtMaxInstances(size_t n) { rtMaxInstances_ = n; }
  // --max-tris N bounds unique prototype triangles in the CUDA/HIP 2-level RT scene.
  void setRtMaxTris(size_t n) { if (n > 0) cudaMaxTris_ = n; }
  // --lod-stream: view-dependent district LOD pre-pass (needs --next). Promotes
  // the camera-nearest districts to districtLod=full under the memory budgets.
  void setLodStream(bool on) { lodStream_ = on; }
  void setLodMaxMemGiB(double g) { lodMaxMemGiB_ = g; }
  void setLodMaxVramGiB(double g) { lodMaxVramGiB_ = g; }
  // --camera <name>: frame the viewer on a named USD Camera (either loader) instead
  // of auto-fitting the whole scene. Essential for vast scenes (e.g. Scene C).
  void setCameraName(const std::string& n) {
    cameraName_ = n;
    loadOpts_.viewCamera = n;
  }
  void setStereoRequested(bool on) { stereoRequested_ = on; }
  void setRasterMotionEnabled(bool on) { rasterMotionEnabled_ = on; }
  void setCameraConform(CameraConform conform) { camera_.setConform(conform); }
  void setViewDirection(float x, float y, float z) {
    viewDir_[0] = x; viewDir_[1] = y; viewDir_[2] = z;
    viewDirExplicit_ = true;
  }
  // Recently-opened scenes: the config file path to persist to, and the initial
  // list loaded from it. setRecentScenes also seeds the File > Open Recent menu.
  void setConfigPath(const std::filesystem::path& p) { configPath_ = p; }
  void setExecutablePath(const std::filesystem::path& p) { executablePath_ = p; }
  void setRecentScenes(const std::vector<std::string>& v) {
    recentScenes_ = v;
    gui_.setRecentScenes(recentScenes_);
  }
  // Initial render mode (e.g. --wireframe); applies to raster + both RT backends.
  // --mode-sweep: render each mode from ONE load, writing `pattern` with
  // {mode} replaced. Each mode gets the same frame count a separate process
  // would have given it, so the images match the one-process-per-mode path.
  void setModeSweep(
      const std::vector<std::pair<std::string, RenderMode>>& modes,
      const std::string& pattern) {
    modeSweep_ = modes;
    modeSweepPattern_ = pattern;
  }
  void setRenderMode(RenderMode m) { gui_.setRenderMode(m); }
  void setBlendWeight(const std::string& name, float w) {
    gui_.setBlendWeight(name, w);
  }
  void setInitialSelection(const std::string& primPath) {
    initialSelect_ = primPath;
  }

  // Embedded MCP server transports (no-op unless built with LUSDVIEW_ENABLE_MCP).
  void setMcpStdio(bool on) { mcpStdio_ = on; }
  void setMcpHttp(int port) { mcpHttpPort_ = port; }  // 0 = off
  void setLiveShaderWatch(bool on) { liveShaderWatchRequested_ = on; }
  void setMaterialXVulkanShaderLimits(size_t maxSourceBytes,
                                      int compileTimeoutSec) {
    materialXVulkanShaderMaxSourceBytes_ = maxSourceBytes;
    materialXVulkanCompileTimeoutSec_ = compileTimeoutSec;
  }
  void setStreamHttp(int port) { streamHttpPort_ = port; }  // 0 = off
  // Idle-refinement codec for the stream: "png" (default) or "qoi". While the
  // view is moving, frames are sent as small low-quality JPEG; once the view is
  // stable a single full-resolution lossless frame is sent in this codec.
  void setStreamCodec(const std::string& c) {
    streamCodec_ = c;
    if (c == "png" || c == "qoi") streamIdleCodec_ = c;
  }
  // Motion-frame tuning: long-edge resolution cap (px) and JPEG quality (1-100)
  // used while the view is changing (the stable refine is always full-res lossless).
  void setStreamMotionRes(int px) {
    if (px > 0) streamMotionMaxDim_ = px;
  }
  void setStreamMotionQuality(int q) {
    if (q >= 1 && q <= 100) streamMotionJpegQ_ = q;
  }
  // Milliseconds of input quiet before the stream sends the lossless refine frame.
  void setStreamIdleMs(int ms) {
    if (ms >= 0) streamIdleMs_ = ms;
  }
  // Apply one browser navigation command to the camera/render state (main thread).
  void applyNavCommand(const StreamNav& cmd);

#if defined(LUSDVIEW_HAVE_MCP)
  // McpHost tool handlers (defined in mcp/app_mcp.cc; run on the main thread).
  nlohmann::json mcpLoadUsd(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpSceneInfo(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpRenderStats(const nlohmann::json& a,
                                std::string& e) override;
  nlohmann::json mcpGetFocusedPrim(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpSetFocus(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpViewport(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpScreenshot(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpInput(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpMouse(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpPick(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpListPrims(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpLoadPayloads(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpReloadLayer(const nlohmann::json& a,
                                std::string& e) override;
  nlohmann::json mcpTimeline(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpSkinning(const nlohmann::json& a, std::string& e) override;
  nlohmann::json mcpRenderSettings(const nlohmann::json& a,
                                   std::string& e) override;
  nlohmann::json mcpListOpenPbrMaterials(const nlohmann::json& a,
                                         std::string& e) override;
  nlohmann::json mcpOpenPbrMaterial(const nlohmann::json& a,
                                    std::string& e) override;
  nlohmann::json mcpShaderReload(const nlohmann::json& a,
                                 std::string& e) override;
  nlohmann::json mcpVirtualHuman(const std::string& tool,
                                 const nlohmann::json& a,
                                 std::string& e) override;
  nlohmann::json mcpCallLibraryTool(const std::string& name, const nlohmann::json& a,
                                    std::string& e) override;
#endif

  // Initialize window + renderer + ImGui, optionally load `initialFile`, then
  // run the main loop. `maxFrames >= 0` renders that many frames then exits
  // cleanly (useful for headless smoke tests). If `screenshot` is non-empty the
  // 3D viewport is written there as a PPM after the last frame (GL backend).
  // Returns process exit code.
  int run(const std::string& initialFile, int maxFrames = -1,
          const std::string& screenshot = "");

 private:
  bool virtualHumanProfile_{false};
  std::string autoriggerExecutable_;
  std::future<vchar::AutoriggerResult> autoriggerFuture_;
  std::string autoriggerOutput_;
  std::future<vchar::AutoriggerResult> dnaWorkerFuture_;
  TydraPhysWorld physicsWorld_{};
  bool physicsWorldReady_{false};
  std::uint64_t physicsSceneGen_{~std::uint64_t(0)};
  std::unique_ptr<lightusd::Stage> physicsInitialStage_;
  bool initWindow(std::string* err);
  bool initImGui(std::string* err);
  void getRequestedWindowSize(int* width, int* height) const;
  void openFileDialog();
  void openDomeFileDialog();
  void applyDomeLightRequest(const Gui::DomeLightRequest& request);
  void writeRenderReport(const std::string& scenePath, int exitCode) const;

  // Synchronous load (used for headless --frames runs so screenshots are
  // deterministic) and the async path (keeps the UI responsive).
  void loadFileBlocking(const std::string& path);
  void startLoadAsync(const std::string& path);
  void resetTextureResidency();
  void updateTextureResidency();
  std::vector<int> selectedTextureSlots() const;
  // Record a successfully-opened scene at the front of the recent list (dedup,
  // capped), refresh the menu, and persist to the config path.
  void addRecentScene(const std::string& path);
  // Recompose the current scene with additional payloads loaded (lazy payload
  // on-demand load). `addPrimPaths` are deferred-payload prim paths to load on
  // top of those already loaded. No-op if the scene wasn't composed.
  void startRecomposeAsync(const std::set<std::string>& addPrimPaths);
  void startLayerReloadAsync(const std::string& resolvedLayerId);
  void finishLoadIfReady();
  bool tryApplyNextSceneUpdate(
      DrawScene* next, const ViewerChanges& changes);
  void prepareNextRenderTransaction(const DrawScene& next);
  void applyLoaded(bool ok, bool progressive,
                   bool alreadyUploaded = false);  // upload + bind on main thread
  void stepProgressiveUpload();  // stream meshes then textures, budgeted per frame
  bool stepPtexResidency(double deadlineMs);
  struct PtexDecodeResult {
    size_t texture{0};
    uint32_t face{0};
    uint64_t sceneGen{0};
    light3d::Image page;
    DrawPtexFaceRectCPU rect;
    std::string error;
    bool ok{false};
  };
  bool finishPtexDecode(bool wait, bool discard);
  void clearPtexDecode();
  bool collectPtexRequests(
      const DrawMeshCPU& mesh,
      const std::vector<uint32_t>* sourceFaces = nullptr);
  void updatePtexResidency();
  void drainProgressiveLoad();   // consume loader-produced meshes on context thread
  void ensureWireAuxReady();     // make resident wire buffers complete atomically
  // Static shaded previews keep diagnostic topology in a delta-varint stream
  // until wire/source-face display is requested. This avoids retaining another
  // pair of full uint32 arrays for every uploaded mesh.
  struct DeferredMeshAux {
    std::vector<uint8_t> wire;
    std::vector<uint8_t> sourceFaces;
    size_t wireCount{0};
    size_t sourceFaceCount{0};
  };
  void compactDeferredMeshAux(size_t meshIndex);
  bool restoreDeferredMeshAux(size_t meshIndex);
  void clearDeferredMeshAux();
  void cancelAndJoinLoad();

  // --- Animation playback ---
  // Read the time range (start/end/fps) from the freshly loaded scene; resets
  // playback to a paused state at the start time.
  void readAnimationRange();
  void updateSkinningEffective();
  void updateGpuSkinningFrameIfNeeded();
  // --next per-frame GPU morph: upload blendshape coefficients for instanced
  // prototypes from the retained next stage at animTime_. Runs independently of
  // the Tydra-path GPU-skinning gate (which --next does not engage).
  void updateNextDeformFrameIfNeeded();
  // Does the --next scene carry deform data (skeleton bone rows / morph channels)?
  bool sceneIsNextDeformable() const;
  // Write the pose at `time` into draw_ geometry, for the CUDA/HIP tracers: they
  // build their BVH from draw_ meshes, not from renderer-owned vertex buffers, so
  // they cannot be fed the way Vulkan RT is. Restores the retained rest pose
  // first, so it is idempotent across time codes. True when draw_ now holds `time`.
  bool poseNextDrawForTracer(double time);
  // Undo poseNextDrawForTracer: put draw_ back to the retained rest pose and
  // clear nextTracerPosedTime_. Call whenever no draw_-posing tracer
  // (CUDA/HIP/CPU RT) owns draw_ any more, so the rest-pose invariant every
  // other consumer relies on holds again.
  void restoreNextDrawRestPose();
  // Re-decode any draw_ texture whose CPU payload the raster residency
  // streamer released, so the CUDA/HIP/CPU tracers (which build their texture
  // table from draw_) see real pixels instead of empty slots. Main thread;
  // call before handing draw_ to a tracer build. Returns true if it decoded
  // anything.
  bool ensureRtTexturePayloads();
  // Non-GPU (ray-traced / CPU-skinned) path: when manual blendshape weights
  // change, re-bake the deformed geometry + BLAS via an async reconvert.
  void maybeReconvertForManualBlend();
  bool wantsGpuSkinningLoad() const;
  bool wantsNextGpuSkinning() const;
  // True when some RT backend still needs draw_'s per-mesh CPU geometry to
  // (re)build its own BVH from -- CUDA/HIP (cudaRt_/hipRt_, screenshot-only)
  // or Vulkan's compute-BVH fallback (rt_scene_build.cc's BuildHostScene(),
  // called lazily from VulkanRenderer::rebuildSwBvh() whenever the scene
  // changes, unlike hardware ray-query which reads already-uploaded GPU
  // buffers and hardware AS, not CPU arrays). The --next loader's
  // FreeMeshSurfaceCPU/FreeMeshAuxCPU/FreeMeshGeometryCPU calls must all skip
  // affected meshes while this is true, or the next Vulkan sw-BVH rebuild
  // fails with "no geometry".
  bool needsMeshCpuGeometryForRT() const;
  void startPrototypeLodBuild(size_t meshIndex, const DrawMeshCPU& mesh);
  void pollPrototypeLodBuilds();
  const char* skinningModeName(SkinningMode mode) const;
  // Advance the playback clock by `dtSec` and request a re-evaluation at the new
  // time (called once per frame while playing).
  void advancePlayback(float dtSec);
  // Coalesced request to re-evaluate geometry at time code `t`: starts a worker
  // if idle, else records the latest wanted time (applied when the current one
  // finishes). No-op while a file load is streaming.
  void requestReconvert(double t);
  void startReconvertAsync(double t);
  void finishReconvertIfReady();   // swap in completed geometry, preserve camera
  void cancelAndJoinReconvert();   // stop a running reconvert worker
  void registerParametricPrims();  // scan stage for parametric prims for adaptive tess
  struct LiveShaderState {
    struct VulkanSourceResult {
      bool ok{false};
      std::vector<uint32_t> spirv;
      std::string error;
    };
    std::string source;
    bool watch{false};
    bool haveTimestamp{false};
    std::filesystem::file_time_type timestamp{};
    uint64_t attempts{0};
    uint64_t successes{0};
    double lastCompileMs{0.0};
    std::string lastError;
    std::string phase;
    std::shared_ptr<AsyncShaderReloadResult> pending;
    std::optional<std::future<VulkanSourceResult>> vulkanSourceJob;
    bool queuedWatchReload{false};
  };
  void initializeLiveShaderSources();
  void finishLiveShaderReloads();
  void pollLiveShaderReload();
  bool reloadLiveShader(const std::string& backend,
                        const std::string& sourceOverride, std::string* err);
  std::string activeLiveShaderBackend() const;

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // --- Experimental threaded GL rendering ---
  // The render thread owns the GL context and runs every GPU op: it drains the
  // GPU-op queue (uploads), then renders the latest frame packet (scene draw +
  // ImGui composite + swap). The main thread only does events + ImGui UI build,
  // posting GPU ops + frame packets and never blocking on the GPU.
  bool startRenderThread();
  void joinRenderThread();
  void renderThreadMain();
  void postGpu(std::function<void()> op);  // queued when threaded, else inline
  void drainGpuOps();
  void submitFramePacket(std::unique_ptr<FramePacket> pkt, bool blockUntilDone);
#else
  void postGpu(std::function<void()> op) { op(); }  // inline (single-threaded build)
#endif

  Backend backend_;
  bool allowBackendFallback_{false};
  RendererDevicePreference devicePreference_;
  TransparencyMode transparencyMode_{TransparencyMode::Auto};
  bool rasterQualityHigh_{false};
  GLFWwindow* window_{nullptr};
  std::unique_ptr<Renderer> renderer_;

  LoadedScene loaded_;
  DrawScene draw_;
  std::vector<DrawLightCPU> authoredLights_;
  DrawLightCPU runtimeDomeBase_;
  bool runtimeDomeValid_{false};
  std::string runtimeDomeKey_;
  std::string droppedEnvmapPath_;
  std::string startupEnvmapPath_;
  float startupEnvmapIntensity_{1.0f};
  float startupEnvmapRotationDegrees_{0.0f};
  LoadOptions loadOpts_;
  double uploadBudgetMs_{8.0};
  bool useNextLoader_{false};  // --next: next loader + tydra-next flat preview
  // Persistent public-C document owner, with temporary native query views.
  std::shared_ptr<ViewerDocument> nextSession_;
  std::shared_ptr<ViewerDocument> pendingNextSession_;
  ViewerChanges pendingNextChanges_;
  std::shared_ptr<lightusd::api::RenderSession> nextRenderSession_;
  std::shared_ptr<lightusd::api::RenderSession>
      pendingNextRenderSession_;
  std::unique_ptr<lightusd::api::PreparedRenderUpdate>
      pendingNextRenderUpdate_;
  bool pendingNextRenderBootstrap_{false};
  // Immutable published stage used by the render/UI thread while a shared
  // document recomposes on the loader thread.
  lightusd::api::Stage nextStageSnapshot_;
  uint64_t nextStageRevision_{0};
  bool hasNextMorph_{false};   // any --next draw mesh carries GPU morph channels
  float camDolly_{1.0f};       // --cam-dolly: fitted-distance scale (<1 zooms in)
  OrbitCamera camera_;
  Gui gui_;
  AdaptiveTessellator tess_;      // adaptive re-tessellation for parametric prims
  float tessQuality_{1.0f};      // tessellation quality multiplier

  float uiScale_{2.0f};      // widget scale (defaults to font px / 16)
  float fontSizePx_{32.0f};  // font size in pixels
  float windowScale_{2.0f};  // default window size multiplier
  bool uiScaleExplicit_{false};  // user set --ui-scale/--font-size/--window-scale
  // Pick a default UI/window scale from the primary monitor (no-op once explicit
  // or in headless): 1.0 on standard-density / sub-2K displays, 2.0 on HiDPI/>=2K.
  void autoDetectUiScale();
  bool hasWindowSizeOverride_{false};
  int windowWidth_{0};
  int windowHeight_{0};
  std::string windowShot_;
  std::string renderReportPath_;
  int checkpointEvery_{0};
  std::string checkpointPattern_;
  size_t checkpointCount_{0};
  std::string largeSceneProfile_ = "off";
  int reportCaptureWidth_{0};
  int reportCaptureHeight_{0};
  bool headless_{false};
  // Only an unbounded interactive run may read or write imgui.ini. Headless
  // captures and finite --frames runs use synthetic/test window dimensions.
  bool persistImGuiState_{true};
  std::vector<std::pair<std::string, RenderMode>> modeSweep_;  // --mode-sweep
  std::string modeSweepPattern_;
  size_t modeSweepIndex_{0};  // windowless offscreen rendering (Vulkan only)
  // Mirrors run()'s local winW/winH (headless composite size) so
  // createAndInitRenderer() can call setHeadlessSize() without needing them
  // passed as parameters; kept in sync with the streamResizeW_/H_ live-resize
  // handling in the main loop.
  int headlessWinW_{0};
  int headlessWinH_{0};
  bool cudaRt_{false};    // --cuda: CUDA BVH ray-traced screenshot (cuew runtime)
  bool cpuRt_{false};     // --cpu-rt: CPU (lightrt_c) ray tracer
  std::string cameraName_;  // --camera: named USD camera to frame (--next path)
  bool stereoRequested_{false};  // --stereo: resolve a left/right authored pair
  bool rasterMotionEnabled_{true};  // native raster shutter accumulation
  bool stereoLiveComposed_{false};
  bool stereoCaptureComposed_{false};  // native side-by-side viewport capture
  bool viewDirExplicit_{false};
  float viewDir_[3]{0.0f, 0.0f, -1.0f};  // normalized eye-to-target direction
  std::filesystem::path configPath_;        // where to persist recent scenes
  std::filesystem::path executablePath_;    // local imgui.ini lookup anchor
  std::string imguiIniPath_;                // storage backing ImGuiIO::IniFilename
  int iniWindowX_{0};
  int iniWindowY_{0};
  int iniWindowWidth_{0};
  int iniWindowHeight_{0};
  bool iniWindowPositionValid_{false};
  bool iniWindowSizeValid_{false};
  std::vector<std::string> recentScenes_;   // newest first; File > Open Recent
  CudaRayTracer cudaTracer_;
  size_t cudaMaxTris_{32000000};  // unique prototype-triangle cap
  std::size_t gpuMemBudgetBytes_{0};  // --max-gpu-mem: raster full-mesh VRAM cap
  std::size_t maxFullMeshes_{0};      // --max-draw-meshes: raster full-mesh count cap
  bool robustFrame_{true};            // trim outlier meshes from fit-all bbox
  bool robustBoundsValid_{false};     // robust bounds computed (pre-LOD) this load
  float robustBoundsMin_[3]{0, 0, 0};
  float robustBoundsMax_[3]{0, 0, 0};

  // View-dependent RT LOD (--rt-lod): re-classify on camera settle (hysteresis +
  // debounce), then push the camera snapshot to the renderer for a TLAS rebuild.
  void updateRtLodCamera();
  bool rtLodEnabled_{false};
  float rtLodFullPx_{64.0f};
  float rtLodCullPx_{2.0f};
  float rtLodBandFrac_{0.25f};  // stochastic crossfade half-width (fraction of fullPx)
  bool rasterLodEnabled_{false};       // --raster-lod (optimization B)
  // Screen size at/above which an instance keeps its real geometry; below it (down
  // to rasterLodCullPx_) it is drawn as a box proxy. This was 48px, which proxied
  // clearly-visible geometry: on a large scene subtree the vegetation turned into
  // white boxes. The quality cliff sits between 16 and 48 -- 4, 8 and 16 all render
  // that scene identically (21445786 tris, 6.4ms vs 1154ms unculled) while 48 drops
  // to 5655 proxies and visible blocks. 8 keeps a wide margin below the cliff; use
  // --raster-lod-full-px 4 for quality-critical work (prototype subtree is baseline-identical
  // at 4, marginally blockier at 8).
  float rasterLodFullPx_{8.0f};
  float rasterLodCullPx_{1.5f};
  bool dynamicPrototypeLodEnabled_{false};
  struct PendingPrototypeLod {
    std::future<PrototypeLodBuildResult> future;
  };
  std::vector<PendingPrototypeLod> pendingPrototypeLods_;
  std::deque<PrototypeLodBuildInput> queuedPrototypeLods_;
  uint64_t prototypeLodGeneration_{0};
  int dynamicPrototypeLodTier_{0};
  int dynamicLodSlowFrames_{0};
  int dynamicLodFastFrames_{0};
  bool lodHaveLast_{false};
  bool lodArmedOnce_{false};
  bool lodPendingReselect_{false};
  int lodStillFrames_{0};
  light3d::Vec3 lastLodEye_{0, 0, 0};
  light3d::Vec3 lastLodFwd_{0, 0, -1};
  bool hipRt_{false};     // --hip: HIP/ROCm BVH ray-traced screenshot (hipew runtime)
  // True when a headless --cuda/--hip run owns the screenshot: the rasterized
  // upload + per-frame draw are then skipped (the RT path writes the image, the
  // raster capture is never used) -- a big win on huge scenes (Island).
  bool rtOwnsScreenshot_{false};
  // True for a windowed --hip run: the HIP tracer drives the viewport per frame
  // (build once, retrace on the orbit camera, upload into the offscreen color via
  // renderer_->uploadViewportImage). The raster scene upload is skipped (CPU
  // geometry is kept for the tracer), and rendering stays single-threaded.
  bool hipInteractive_{false};
  bool hipInteractiveBuilt_{false};  // HIP scene built lazily on the first frame
  int hipBuildAnnounceFrames_{0};    // frames rendered with the "building" overlay before kicking off the build
  std::string rtBuildNote_;          // RT build status for the progress overlay
  // Background HIP scene build: the build runs on a worker thread so the UI stays
  // responsive and shows live progress instead of freezing on the multi-second build.
  std::thread hipBuildThread_;
  bool hipBuildStarted_{false};
  std::atomic<bool> hipBuildDone_{false};
  std::atomic<bool> hipBuildOk_{false};
  std::string hipBuildErr_;          // written by the worker, read after join
  BuildProgress hipBuildProgress_;   // atomics polled by the overlay
  std::chrono::steady_clock::time_point hipBuildStart_;
  // Trace the HIP viewport for one interactive frame (builds the scene on first
  // call). Returns false if HIP is unavailable / the build failed.
  bool renderHipViewport();

  // True for a windowed --cuda run: mirrors hipInteractive_ (see above), driving
  // the viewport with CudaRayTracer instead. CudaRayTracer has no refit path
  // (unlike HipRayTracer), so a deformable scene's re-pose always rebuilds.
  bool cudaInteractive_{false};
  bool cudaInteractiveBuilt_{false};
  int cudaBuildAnnounceFrames_{0};
  std::thread cudaBuildThread_;
  bool cudaBuildStarted_{false};
  std::atomic<bool> cudaBuildDone_{false};
  std::atomic<bool> cudaBuildOk_{false};
  std::string cudaBuildErr_;
  BuildProgress cudaBuildProgress_;
  std::chrono::steady_clock::time_point cudaBuildStart_;
  // Trace the CUDA viewport for one interactive frame (builds the scene on first
  // call). Returns false if CUDA is unavailable / the build failed.
  bool renderCudaViewport();
  struct ExternalPathAccumulation {
    std::array<float, 16> viewProj{};
    float focusDistance{0.0f};
    float apertureRadius{0.0f};
    float sceneTime{0.0f};
    int width{0};
    int height{0};
    int renderMode{0};
    uint32_t maxDepth{0};
    uint32_t rouletteDepth{0};
    uint32_t maxSubsurfaceEvents{0};
    uint32_t maxVolumeEvents{0};
    uint32_t motionSegments{0};
    double shutterOpen{0.0};
    double shutterClose{0.0};
    uint32_t seed{0};
    uint64_t kernelGeneration{0};
    uint32_t samples{0};
    uint64_t shutterSegmentMask{0};
    uint64_t sceneShutterSegmentMask{0};
    uint64_t generation{0};
    bool valid{false};
    std::vector<uint8_t> lastRgba;
    void invalidate() {
      samples = 0;
      valid = false;
      lastRgba.clear();
      shutterSegmentMask = 0;
      sceneShutterSegmentMask = 0;
      ++generation;
    }
    void noteSceneShutterSegment(uint32_t sample, uint32_t segments,
                                 const RtCameraShutter& shutter) {
      if (!shutter.enabled() || segments == 0) return;
      sceneShutterSegmentMask |= uint64_t{1} << ((sample % segments) % 64u);
    }
    void noteShutterSegment(uint32_t sample, uint32_t segments,
                            const RtCameraShutter& shutter) {
      if (!shutter.enabled() || segments == 0) return;
      shutterSegmentMask |= uint64_t{1} << ((sample % segments) % 64u);
    }
  };
  struct ExternalTraceResult {
    bool ok{false};
    uint64_t generation{0};
    uint32_t accumulationStart{0};
    uint32_t samples{0};
    int width{0};
    int height{0};
    float exposure{0.0f};
    std::vector<uint8_t> rgba;
    std::vector<float> linear;
    std::string error;
  };
  ExternalPathAccumulation cudaPathAccum_;
  ExternalPathAccumulation hipPathAccum_;
  struct RasterMotionAccumulation {
    std::array<float, 16> baseViewProj{};
    std::vector<float> linearSum;
    std::vector<uint8_t> lastRgba;
    double baseTime{0.0};
    double shutterOpen{0.0};
    double shutterClose{0.0};
    uint64_t sceneGeneration{0};
    uint32_t segmentCount{0};
    uint32_t nextSegment{0};
    uint64_t sampledMask{0};
    int width{0};
    int height{0};
    bool valid{false};
    void reset() {
      linearSum.clear();
      lastRgba.clear();
      nextSegment = 0;
      sampledMask = 0;
      valid = false;
    }
  } rasterMotionAccum_;
  struct VulkanRtMotionAccumulation {
    std::array<float, 16> baseViewProj{};
    RtCameraLens baseLens;
    double baseTime{0.0};
    double shutterOpen{0.0};
    double shutterClose{0.0};
    uint64_t sceneGeneration{0};
    uint32_t segmentCount{0};
    uint32_t nextSegment{0};
    uint64_t sampledMask{0};
    uint32_t pathMaxDepth{0};
    uint32_t pathRouletteDepth{0};
    uint32_t pathSeed{0};
    int width{0};
    int height{0};
    bool pathEnabled{false};
    bool valid{false};
    void reset() {
      nextSegment = 0;
      sampledMask = 0;
      valid = false;
    }
  } vulkanRtMotionAccum_;
  std::future<ExternalTraceResult> cudaTraceFuture_;
  std::future<ExternalTraceResult> hipTraceFuture_;
  // Lazy, cached CUDA/HIP device-availability probe: init() compiles NVRTC/
  // hiprtc kernels, so it must not run speculatively at startup or on every
  // menu-open -- only once, on first switch attempt, then cached for the
  // menu's enabled/grayed-out state.
  enum class ProbeState { Unknown, Available, Unavailable };
  ProbeState cudaProbe_{ProbeState::Unknown};
  ProbeState hipProbe_{ProbeState::Unknown};
  bool computeDevicesEnumerated_{false};
  std::vector<std::string> cudaDevices_;
  std::vector<std::string> hipDevices_;

  // CPU RT (the "R" keybinding / --cpu-rt / View > Render Technique > CPU RT).
  // Always available (no device to probe), so unlike cudaInteractive_/
  // hipInteractive_ there is no *Probe_ gate. Same background-build shape as
  // HIP/CUDA (BuildHostScene + a lightrt_c BVH can take a while on big
  // scenes).
  CpuRayTracer cpuTracer_;
  bool cpuInteractive_{false};
  bool cpuInteractiveBuilt_{false};
  int cpuBuildAnnounceFrames_{0};
  std::thread cpuBuildThread_;
  bool cpuBuildStarted_{false};
  std::atomic<bool> cpuBuildDone_{false};
  std::atomic<bool> cpuBuildOk_{false};
  std::string cpuBuildErr_;
  BuildProgress cpuBuildProgress_;
  bool renderCpuViewport();
  bool hasPendingOpenPbrEdit_{false};
  Gui::OpenPbrMaterialEdit pendingOpenPbrEdit_;

  // --- Runtime backend switch (View menu Render Technique submenu / keybinding) ---
  // Ground truth: activeTechnique_ decomposes into backend_ (which Renderer
  // subclass owns the window) + activeOverlay_ (what draws into it this frame;
  // see OverlayKind in renderer.hh). The request/consume handshake lives on
  // Gui (Gui::hasTechniqueRequest()/requestedTechnique()), mirroring
  // hasSkinningModeRequest_/requestedSkinningMode_ -- View menu items and the
  // CPU RT keybinding (both in gui.cc) set it directly since Gui has no App
  // back-pointer; App::run() drains + clears it once per frame (via
  // gui_.clearActions()) and calls applyTechniqueSwitch(). App feeds the
  // current technique back to Gui each frame (gui_.setActiveTechnique()) so
  // the menu can show which one is checked.
  RenderTechnique activeTechnique_{RenderTechnique::GLRaster};
  OverlayKind activeOverlay_{OverlayKind::None};
  RenderTechnique previousTechnique_{RenderTechnique::GLRaster};  // for the CPU-RT toggle keybinding
  // Once a technique switch could happen at runtime, the CPU-geometry-free
  // optimizations for non-deformable scenes (freed after a HIP/CUDA/CPU-RT
  // build to cut RSS) must not run -- switching back to a raster window owner
  // needs draw_'s CPU vertices for uploadScene(). Always true once run() has
  // constructed a renderer (i.e. for the app's whole interactive lifetime);
  // kept as an explicit flag so the tradeoff is visible at each free-site.
  bool liveSwitchEnabled_{true};
  // Construct + initialize a Renderer for `backend` (device preference, RT
  // texture budget, host scene source, headless size, init()). Factored out of
  // the startup path so the live-switch path (applyTechniqueSwitch) can reuse
  // the exact same sequence instead of duplicating it.
  // Emits the versioned `caps: v1 ...` line consumed by the test suite.
  void logCapabilities() const;
  void captureModeSweepFrame(const std::string& modeName);
  bool createAndInitRenderer(Backend backend, std::string* err);
  // Apply a requested technique switch: same-owner overlay changes (Vulkan
  // raster <-> Vulkan RT) just flip renderer_->setRayTracing(); an owner change
  // (GL <-> Vulkan) tears down and recreates renderer_ via
  // createAndInitRenderer() and re-uploads draw_. Never crashes on failure --
  // reverts to the prior technique and logs a warning instead. Returns false
  // (and leaves activeTechnique_ unchanged) on failure.
  bool applyTechniqueSwitch(RenderTechnique t);
  // Backend switch deferred to the frame boundary (applied before the next
  // ImGui::NewFrame); see App::run().
  RenderTechnique pendingTechnique_{RenderTechnique::GLRaster};
  bool hasPendingTechnique_{false};
  // Encode an RGBA8 window grab and broadcast it. `motion`=true sends a small
  // low-quality JPEG (fast, for interaction); false sends a full-resolution
  // lossless frame in streamIdleCodec_ (the stable-state refinement).
  void streamEncodeAndPush(std::vector<uint8_t> rgba, int w, int h, bool motion);
  // Mark the streamed view as changed (resets the idle refinement timer).
  void markStreamActivity();
  HipRayTracer hipTracer_;
  int rtSamples_{1};      // --rt-samples: AA samples for RT screenshot/CPU viewport
  PathTraceSettings pathTrace_;
  uint32_t pathTraceRenderedSamples_{0};
  std::string linearOutput_;
  float lensFStopOverride_{0.0f};
  float lensFocusOverride_{0.0f};
  RtCameraLens cameraLens_;
  RtCameraShutter cameraShutter_;
  bool vulkanAvailable_{false};
  bool vulkanRtAvailable_{false};
  size_t rtMaxInstances_{16000000};  // --max-instances: CUDA/HIP instance cap (0=off)
  bool lodStream_{false}; // --lod-stream: view-dependent district LOD pre-pass
  double lodMaxMemGiB_{0.0};   // --max-mem: host budget for --lod-stream (0=auto)
  double lodMaxVramGiB_{0.0};  // --max-vram: GPU budget for --lod-stream (0=auto)

  // Ray tracing: requested via --rt; rtPath_ is the effective state after the
  // renderer reports capability (drives the RT-friendly conversion config).
  bool rtRequested_{false};
  bool rtPath_{false};

  SkinningMode skinningRequested_{SkinningMode::Auto};
  SkinningMode skinningEffective_{SkinningMode::CPU};
  std::string skinningReason_{"CPU path"};
  SkinningFrameCPU skinFrame_;
  double skinFrameTime_{std::numeric_limits<double>::quiet_NaN()};
  // CUDA/HIP tracer re-pose (poseNextDrawForTracer): the retained rest vertices of
  // the deformable meshes, and the time code draw_ currently holds.
  std::unordered_map<int, std::vector<DrawVertex>> nextRestVerts_;
  double nextTracerPosedTime_{std::numeric_limits<double>::quiet_NaN()};
  bool lastRtActiveForSkinning_{false};
  bool warnedMeshIndexMismatch_{false};

  // Async loading
  enum class TextureResidencyState : uint8_t {
    Unloaded,
    Decoding,
    CoarseResident,
    FullResident,
    Failed,
  };
  struct TextureResidencySlot {
    TextureResidencyState state{TextureResidencyState::Unloaded};
    std::chrono::steady_clock::time_point lastWanted{};
    size_t residentBytes{0};
    bool forceFull{false};
    uint64_t generation{0};
  };
  struct TextureDecodeResult {
    size_t slot{0};
    bool full{false};
    bool ok{false};
    uint64_t generation{0};
    DrawTextureCPU texture;
  };
  std::vector<TextureResidencySlot> textureResidency_;
  // Compact scene-stable adjacency used by the per-frame camera policy. Avoids
  // repeatedly expanding every DrawMaterialCPU texture field for every mesh.
  std::vector<std::vector<int>> textureSlotsByMesh_;
  std::vector<uint8_t> textureMarginVisible_;
  std::array<float, 15> textureCameraSignature_{};
  bool textureCameraSignatureValid_{false};
  std::vector<std::future<TextureDecodeResult>> textureDecodeJobs_;
  bool backgroundTextureRefinement_{true};
  uint64_t textureDecodeRawBytes_{0};
  uint64_t textureDecodeGpuBytes_{0};
  uint64_t texturePeakResidentBytes_{0};
  uint64_t textureCoarseUploads_{0};
  uint64_t textureFullUploads_{0};
  uint64_t textureDecodeFailures_{0};
  uint64_t textureEvictions_{0};

  std::thread loadThread_;
  LoadControl loadCtrl_;
  std::atomic<bool> loadFinished_{false};
  bool loadActive_{false};  // main-thread-only UI flag
  std::unique_ptr<LoadedScene> pendingLoaded_;
  std::unique_ptr<DrawScene> pendingDraw_;
  std::shared_ptr<ProgressiveSceneStream> loadStream_;
  bool streamLoadActive_{false};
  bool streamRendererBegun_{false};
  bool streamPreviewActive_{false};
  bool streamCompleteSeen_{false};
  bool streamCameraFramed_{false};
  bool streamFirstUploadLogged_{false};
  bool streamFirstFrameLogged_{false};
  bool streamFullConversionLogged_{false};
  bool streamFullUploadLogged_{false};
  bool streamHasUsefulGeometry_{false};
  bool streamAuxEager_{false};
  std::vector<DeferredMeshAux> deferredMeshAux_;
  size_t deferredAuxRawBytes_{0};
  size_t deferredAuxCompressedBytes_{0};
  bool quitAfterFullUpload_{false};
  bool quitAfterConvert_{false};
  bool quitAfterFullPresent_{false};
  double streamFullConversionSeconds_{0.0};
  float streamBoundsMin_[3]{1e30f, 1e30f, 1e30f};
  float streamBoundsMax_[3]{-1e30f, -1e30f, -1e30f};
  size_t streamUploadedTriangles_{0};
  size_t streamUploadedEffectiveTriangles_{0};
  size_t streamUploadedVertices_{0};
  std::chrono::steady_clock::time_point runStart_;
  std::string loadingPath_;
  std::chrono::steady_clock::time_point loadStart_;

  // Animation playback (main-thread owned unless noted).
  bool hasAnimation_{false};
  bool animPlaying_{false};
  // --play: start playback once the scene is in (one-shot). Fixed-frame runs
  // (--frames) step a FIXED 1/60 s per frame so the pose at frame N -- and the
  // final screenshot -- is deterministic, not wall-clock dependent.
  bool playRequested_{false};
  bool animLoop_{true};
  float animSpeed_{1.0f};
  double animStart_{0.0};
  double animEnd_{0.0};
  double animFps_{24.0};
  double animTime_{0.0};         // current time code being shown
  std::chrono::steady_clock::time_point lastFrameTime_;
  bool haveLastFrameTime_{false};

  // Coalesced async re-evaluation of geometry at a time code (playback/scrub).
  std::thread reconvThread_;
  LoadControl reconvCtrl_;
  std::atomic<bool> reconvFinished_{false};
  bool reconvActive_{false};       // a reconvert worker is running
  double reconvInFlight_{0.0};     // time code the running worker computes
  double reconvRequested_{0.0};    // latest requested time code
  bool reconvHasRequest_{false};   // a (re)convert is wanted
  bool blendReconvNeeded_{false};  // manual blend weights changed (non-GPU path)
  std::string initialSelect_;      // --select: prim to select once loaded
  double reconvApplied_{0.0};      // time code currently shown
  std::unique_ptr<DrawScene> reconvDraw_;
  std::atomic<bool> reconvOk_{false};
  // Rest-pose scene cache so a same-timecode reconvert (interactive blendshape
  // edit on the RT/CPU path) reuses the conversion instead of re-running it.
  // Touched only by the reconvert worker; cleared on reload (worker joined first).
  RestSceneCache reconvRestCache_;

  // Progressive GPU upload (interactive path): stream meshes then textures.
  bool progressiveActive_{false};
  size_t nextMesh_{0};
  size_t nextAux_{0};
  size_t nextTex_{0};
  size_t nextVolume_{0};  // UsdVol volumes uploaded so far
  size_t nextPtexTexture_{0};
  std::vector<std::unique_ptr<PtexPhysicalPageCache>> ptexPhysicalCaches_;
  // Parsed Ptex metadata is immutable and can be reused for every page. Keeping
  // one reader per texture avoids reparsing large face/level tables for each
  // 2 ms refinement slice.
  std::vector<std::shared_ptr<lightusd::ptx::Reader>> ptexReaders_;
  std::future<PtexDecodeResult> ptexDecodeFuture_;
  bool ptexDecodeActive_{false};
  // Bumped whenever draw_'s textures are replaced/freed (scene swap). A decode
  // that was launched for an earlier generation must not publish its page into
  // the new scene, so the result carries the generation it was launched with and
  // finishPtexDecode drops any mismatch.
  uint64_t ptexSceneGen_{0};
  uint64_t ptexAsyncJobsLaunched_{0};
  uint64_t ptexAsyncJobsCompleted_{0};
  std::vector<std::vector<uint32_t>> ptexRequestedFaces_;
  std::vector<std::unordered_set<uint32_t>> ptexRequestedFaceSets_;
  std::vector<size_t> ptexRequestCursors_;
  // A raster mesh contributes Ptex pages only after its world bounds enter the
  // current camera frustum. This remains false for off-camera meshes, avoiding
  // the old full-scene request burst at admission time.
  std::vector<uint8_t> ptexMeshRequested_;
  std::vector<uint8_t> ptexMeshDemanded_;

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // Experimental threaded rendering. renderThreadActive_ is true only when
  // threaded_ AND windowed GL; headless/non-threaded keep the inline path.
  bool threaded_{false};
  bool renderThreadActive_{false};
  std::thread renderThread_;
  std::atomic<bool> renderRunning_{false};
  std::atomic<bool> renderInitOk_{false};
  std::atomic<bool> renderInitDone_{false};
  std::mutex gpuOpMutex_;
  std::queue<std::function<void()>> gpuOps_;
  std::atomic<bool> gpuOpsPending_{false};
  std::mutex pktMutex_;
  std::condition_variable pktCv_;
  std::unique_ptr<FramePacket> pendingPacket_;
  std::condition_variable pktDoneCv_;
  std::atomic<std::uint64_t> pktRenderedSeq_{0};
  std::uint64_t pktSubmitSeq_{0};
  std::chrono::steady_clock::time_point renderFpsSampleTime_{};
  std::uint64_t renderFpsSampleSeq_{0};
  float renderFps_{0.0f};
  std::atomic<float> measuredRenderFps_{0.0f};
  std::vector<uint8_t> renderCapture_;
  int renderCaptureW_{0};
  int renderCaptureH_{0};
#else
  static constexpr bool renderThreadActive_ = false;
  float renderFps_{0.0f};
#endif
  bool adaptiveQuality_{true};
  float adaptiveTargetFps_{30.0f};
  float adaptiveMinScale_{0.5f};
  float adaptiveRenderScale_{1.0f};
  int adaptiveTier_{0};
  int adaptiveOverBudgetFrames_{0};
  int adaptiveUnderBudgetFrames_{0};

  // MCP server (transports started in run(); commands drained each frame).
  bool mcpStdio_{false};
  int mcpHttpPort_{0};
  std::string mcpHttpHost_{"127.0.0.1"};
  bool mcpHttpRunning_{false};
  std::string mcpHttpStatus_;
  std::uint64_t windowGeneration_{0};
  std::uint64_t rendererGeneration_{0};
  bool liveShaderWatchRequested_{false};
  std::chrono::steady_clock::time_point liveShaderNextPoll_{};
  size_t materialXVulkanShaderMaxSourceBytes_{129u * 1024u};
  int materialXVulkanCompileTimeoutSec_{30};
  LiveShaderState liveVulkanShader_;
  LiveShaderState liveCudaKernel_;
  LiveShaderState liveHipKernel_;

  // WebSocket image-streaming server (browser remote view + navigation).
  int streamHttpPort_{0};
  std::string streamCodec_{"jpeg"};
  std::unique_ptr<StreamServer> streamServer_;
  // Browser input state (raw mouse/keyboard forwarded into ImGui + camera).
  bool streamCamDrag_{false};   // current drag drives the camera (not ImGui)
  int streamDragButton_{0};     // DOM button latched at press
  bool streamDragShift_{false}; // shift held at press (orbit->pan)
  float streamLastX_{0.f}, streamLastY_{0.f};  // last cursor (image space)
  // Adaptive quality: low-res low-q JPEG while moving, one full-res lossless
  // refine (PNG/QOI) once stable.
  std::string streamIdleCodec_{"png"};      // refinement codec (png/qoi)
  std::chrono::steady_clock::time_point streamLastActivity_{};
  bool streamHiQSent_{false};               // refine frame already sent for this idle
  int streamPrevClientCount_{0};
  int streamMotionMaxDim_{1280};            // long-edge cap for motion frames
  int streamMotionJpegQ_{45};               // motion JPEG quality
  int streamIdleMs_{350};                   // ms of no activity = stable
  int streamResizeW_{0}, streamResizeH_{0}; // pending headless resize (0 = none)
  // Bumped on each successful load so the MCP library-tool bridge knows when to
  // re-snapshot the Stage into its Context.
  std::uint64_t sceneGen_{0};
#if defined(LUSDVIEW_HAVE_MCP)
  std::unique_ptr<MCPServer> mcp_;
  std::vector<std::filesystem::path> mcpTempFiles_;
  // Context for the lightusd library tools; its Stage is a lazy snapshot of
  // loaded_.stage, refreshed when sceneGen_ changes.
  lightusd::tydra::mcp::Context mcpCtx_;
  std::uint64_t mcpCtxGen_{~std::uint64_t(0)};
#endif
};

}  // namespace lusdview
