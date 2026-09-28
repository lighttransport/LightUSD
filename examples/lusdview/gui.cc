// SPDX-License-Identifier: Apache-2.0
#include "gui.hh"
#include "public_stage_queries.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <unistd.h>  // sysconf (RSS page size)
#endif

#include "core/prim.hh"
#include "gizmo_build.hh"
#include "lod_math.hh"  // BoxFitXform (raster LOD box proxies)
#include "light3d/camera.h"  // light3d::Frustum (per-mesh frustum culling)
#include "gui_stringify.hh"
#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*
#include "next_scene_loader.hh"  // BuildNextRtDeformedVertices (selection-highlight re-pose)
#include "pprint-enum.hh"
#include "skinning.hh"
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

namespace lusdview {

namespace tydra = lightusd::tydra;

namespace {

const char* NodeTypeName(tydra::NodeType t) {
  switch (t) {
    case tydra::NodeType::Xform: return "Xform";
    case tydra::NodeType::Mesh: return "Mesh";
    case tydra::NodeType::Camera: return "Camera";
    case tydra::NodeType::SkelRoot: return "SkelRoot";
    case tydra::NodeType::Skeleton: return "Skeleton";
    case tydra::NodeType::PointLight: return "PointLight";
    case tydra::NodeType::DirectionalLight: return "DirectionalLight";
    case tydra::NodeType::EnvmapLight: return "EnvmapLight";
    case tydra::NodeType::RectLight: return "RectLight";
    case tydra::NodeType::DiskLight: return "DiskLight";
    case tydra::NodeType::CylinderLight: return "CylinderLight";
    case tydra::NodeType::GeometryLight: return "GeometryLight";
    default: return "Node";
  }
}

const char* MaterialParamTypeName(DrawMaterialParamType t) {
  switch (t) {
    case DrawMaterialParamType::Float: return "float";
    case DrawMaterialParamType::Vec2: return "vec2";
    case DrawMaterialParamType::Vec3: return "vec3";
    case DrawMaterialParamType::Vec4: return "vec4";
    default: return "value";
  }
}

const char* MaterialParamChannelName(int channel) {
  switch (channel) {
    case 0: return "R";
    case 1: return "G";
    case 2: return "B";
    case 3: return "A";
    default: return "-";
  }
}

bool HasNonIdentityUvXform(const DrawUvXformCPU& uv) {
  return std::fabs(uv.m00 - 1.0f) > 1.0e-6f ||
         std::fabs(uv.m01) > 1.0e-6f ||
         std::fabs(uv.m10) > 1.0e-6f ||
         std::fabs(uv.m11 - 1.0f) > 1.0e-6f ||
         std::fabs(uv.tx) > 1.0e-6f ||
         std::fabs(uv.ty) > 1.0e-6f;
}

// Greyed, word-wrapped hint text (TextDisabled does not wrap and would clip in
// a narrow panel).
void HintWrapped(const char* s) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
  ImGui::TextWrapped("%s", s);
  ImGui::PopStyleColor();
}

// Selectable diagnostic text with a one-click clipboard action. InputTextMultiline
// requires a mutable buffer even in read-only mode, so use std::string::data()
// without permitting ImGui to resize or edit it.
void DiagnosticTextArea(const char* id, const char* copyLabel,
                        const std::string& message) {
  if (ImGui::SmallButton(copyLabel)) {
    ImGui::SetClipboardText(message.c_str());
  }
  ImGui::PushItemWidth(-FLT_MIN);
  const float height = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
  ImGui::InputTextMultiline(id, const_cast<char*>(message.c_str()),
                            message.size() + 1, ImVec2(-FLT_MIN, height),
                            ImGuiInputTextFlags_ReadOnly);
  ImGui::PopItemWidth();
}

bool ViewportRay(const OrbitCamera& camera, bool zeroToOneDepth, float px,
                 float py, int viewportWidth, int viewportHeight,
                 light3d::Vec3* origin, light3d::Vec3* direction) {
  if (!origin || !direction || viewportWidth <= 0 || viewportHeight <= 0) {
    return false;
  }
  const light3d::Mat4 invVP =
      (camera.proj(zeroToOneDepth) * camera.view()).inverse();
  const float ndcx =
      2.0f * (px / static_cast<float>(viewportWidth)) - 1.0f;
  const float ndcy =
      1.0f - 2.0f * (py / static_cast<float>(viewportHeight));
  auto unproject = [&](float nz) {
    const float* m = invVP.m;
    const float x = m[0] * ndcx + m[4] * ndcy + m[8] * nz + m[12];
    const float y = m[1] * ndcx + m[5] * ndcy + m[9] * nz + m[13];
    const float z = m[2] * ndcx + m[6] * ndcy + m[10] * nz + m[14];
    const float w = m[3] * ndcx + m[7] * ndcy + m[11] * nz + m[15];
    const float iw = w != 0.0f ? 1.0f / w : 1.0f;
    return light3d::Vec3{x * iw, y * iw, z * iw};
  };
  const light3d::Vec3 nearPoint =
      unproject(zeroToOneDepth ? 0.0f : -1.0f);
  const light3d::Vec3 farPoint = unproject(1.0f);
  const light3d::Vec3 ray = farPoint - nearPoint;
  if (!std::isfinite(ray.x) || !std::isfinite(ray.y) ||
      !std::isfinite(ray.z) || light3d::dot(ray, ray) <= 1.0e-20f) {
    return false;
  }
  *origin = camera.eye();
  *direction = light3d::normalize(ray);
  return true;
}

std::string LightUsdVersionString() {
  std::string v = std::to_string(lightusd::version_major) + "." +
                  std::to_string(lightusd::version_minor) + "." +
                  std::to_string(lightusd::version_micro);
  if (lightusd::version_rev && lightusd::version_rev[0] != '\0') {
    v += "-";
    v += lightusd::version_rev;
  }
  return v;
}

const char* OperatingSystemString() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#elif defined(__FreeBSD__)
  return "FreeBSD";
#else
  return "unknown OS";
#endif
}

const char* CpuArchString() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown arch";
#endif
}

std::string CpuInfoString() {
  const unsigned threads = std::thread::hardware_concurrency();
  std::string s = CpuArchString();
  if (threads > 0) {
    s += ", ";
    s += std::to_string(threads);
    s += " logical threads";
  }
  return s;
}

const char* CompilerString() {
#if defined(__clang__)
  return "Clang " __clang_version__;
#elif defined(__GNUC__)
  return "GCC " __VERSION__;
#elif defined(_MSC_VER)
  return "MSVC";
#else
  return "unknown compiler";
#endif
}

const lightusd::Prim* FindPrimByPath(const lightusd::Prim& prim,
                                     const std::string& path) {
  if (prim.absolute_path().full_path_name() == path) return &prim;
  for (const auto& c : prim.children()) {
    if (const lightusd::Prim* p = FindPrimByPath(c, path)) return p;
  }
  return nullptr;
}

// --- Hierarchy search predicates (name + type + abs path) ---
bool PrimPasses(ImGuiTextFilter& f, const lightusd::Prim& prim) {
  std::string typeName = prim.prim_type_name();
  if (typeName.empty()) typeName = prim.type_name();
  std::string s = prim.element_name() + " " + typeName + " " +
                  prim.absolute_path().full_path_name();
  return f.PassFilter(s.c_str());
}
bool PrimSubtreePasses(ImGuiTextFilter& f, const lightusd::Prim& prim) {
  if (PrimPasses(f, prim)) return true;
  for (const auto& c : prim.children()) {
    if (PrimSubtreePasses(f, c)) return true;
  }
  return false;
}
bool NodePasses(ImGuiTextFilter& f, const tydra::Node& node) {
  std::string s = node.prim_name + " " + node.abs_path + " " +
                  NodeTypeName(node.nodeType);
  return f.PassFilter(s.c_str());
}
bool NodeSubtreePasses(ImGuiTextFilter& f, const tydra::Node& node) {
  if (NodePasses(f, node)) return true;
  for (const auto& c : node.children) {
    if (NodeSubtreePasses(f, c)) return true;
  }
  return false;
}

bool PathIsSameOrDescendant(const std::string& path, const std::string& ancestor) {
  if (path.empty() || ancestor.empty()) return false;
  if (path == ancestor) return true;
  if (path.size() <= ancestor.size()) return false;
  if (path.compare(0, ancestor.size(), ancestor) != 0) return false;
  return ancestor == "/" || path[ancestor.size()] == '/';
}

std::vector<std::pair<std::string, std::string>> BuildBreadcrumbs(const std::string& path) {
  std::vector<std::pair<std::string, std::string>> out;
  if (path.empty()) return out;
  if (path[0] != '/') {
    out.emplace_back(path, path);
    return out;
  }

  out.emplace_back("/", "/");
  std::string accum;
  size_t start = 1;
  while (start < path.size()) {
    const size_t end = path.find('/', start);
    const size_t len = (end == std::string::npos) ? (path.size() - start) : (end - start);
    if (len > 0) {
      const std::string part = path.substr(start, len);
      accum += "/" + part;
      out.emplace_back(part, accum);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

const char* NavModeLabel(int mode) {
  switch (mode) {
    case 1: return "Orbit";
    case 2: return "Pan";
    case 3: return "Dolly";
    default: return "Ready";
  }
}

const char* SkinningModeLabel(SkinningMode mode) {
  switch (mode) {
    case SkinningMode::CPU: return "CPU";
    case SkinningMode::GPU: return "GPU";
    case SkinningMode::Auto:
    default: return "Auto";
  }
}

}  // namespace

void Gui::setScene(const LoadedScene* loaded, const DrawScene* draw) {
  // The cull worker reads the current DrawScene; join it before draw_ changes so
  // it never touches freed geometry.
  joinCullWorker();
  ++cullSceneGen_;  // invalidate any worker snapshot; drop grids keyed to the old scene
  instGrids_.clear();
  instGridsFor_ = nullptr;
  lastCullValid_ = false;  // force a re-cull for the new scene
  loaded_ = loaded;
  draw_ = draw;
  authoredDomes_.clear();
  if (draw_) {
    for (const DrawLightCPU& light : draw_->lights) {
      if (light.type != DrawLightCPU::Type::Dome) continue;
      DomeInfo info;
      info.label = !light.displayName.empty()
                       ? light.displayName
                       : (!light.name.empty() ? light.name : light.absPath);
      if (info.label.empty()) info.label = "DomeLight";
      info.format = light.domeTextureFormat;
      authoredDomes_.push_back(std::move(info));
    }
  }
  domeRequest_.source = !authoredDomes_.empty() ? DomeSource::Authored
                                                : DomeSource::Off;
  domeRequest_.authoredIndex = 0;
  domeRequest_.format = authoredDomes_.empty()
                            ? DrawLightCPU::DomeTextureFormat::Automatic
                            : authoredDomes_[0].format;
  domeRequest_.intensity = 1.0f;
  domeRequest_.rotationDegrees = 0.0f;
  domeRequest_.browse = false;
  domeStatus_.clear();
  selPrim_ = nullptr;
  selPath_.clear();
  selMeshIndex_ = -1;
  openPbrEditorMaterial_ = -1;
  openPbrEditorSelectionMesh_ = -2;
  hasOpenPbrMaterialEdit_ = false;
  lastPickedFocusPath_.clear();
  haveLastPickedFocusPoint_ = false;
  selectionList_.clear();
  selectionHistory_.clear();
  selectionHistoryIndex_ = -1;
  inspectorCachePrim_ = nullptr;
  inspectorCachePath_.clear();
  inspectorCacheRows_.clear();
  // Reset per-mesh visibility to all-visible for the new scene.
  meshVisible_.assign(draw_ ? draw_->meshes.size() : 0, uint8_t{1});
  carrierVisible_.assign(draw_ ? draw_->points.size() + draw_->curves.size() : 0,
                         uint8_t{1});
  viewVisible_.clear();
  revealSelectionInHierarchy_ = false;
  // Start with nothing selected; the user selects via the viewport or hierarchy.
  // Drop any highlight geometry the previous scene's selection left behind --
  // it is in the OLD scene's world space, and the Vulkan backend would keep
  // drawing those lines over the new scene (see clearSelection).
  rebuildSubsetHighlight();
}

void Gui::frame(Renderer* renderer, OrbitCamera* camera) {
  renderer_ = renderer;
  cam_ = camera;
  if (captureViewportOnly_) {
    drawViewport();
    return;
  }
  drawDockspaceAndMenu();
  drawHierarchy();
  drawInspector();
  if (virtualHumanProfile_) drawVirtualHumanPanel();
  drawSelectionList();
  drawCameraPanel();
  drawDomeLightPanel();
  drawStageMeta();
  drawStats();
  drawPayloads();
  drawMaterialsPanel();
  drawOpenPbrMaterialPanel();
  drawCompositionGraph();
  drawViewport();
  drawTimeline();
  drawMcpServerPanel();
  drawAboutModal();
  drawLoadingModal();
  drawProgressOverlay();
}

void Gui::drawMcpServerPanel() {
  if (!showMcpServer_) return;
  ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("MCP Server", &showMcpServer_)) {
    ImGui::End();
    return;
  }

  if (!mcpAvailable_) {
    ImGui::TextWrapped(
        "MCP support is not available in this build. Reconfigure with "
        "LUSDVIEW_ENABLE_MCP=ON.");
    ImGui::End();
    return;
  }

  ImGui::Text("Status: %s", mcpRunning_ ? "Running" : "Stopped");
  ImGui::BeginDisabled(mcpRunning_);
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputText("Hostname", mcpHostname_.data(), mcpHostname_.size());
  mcpHostEditing_ = ImGui::IsItemActive();
  ImGui::SetNextItemWidth(220.0f);
  // No +/- steppers: at high UI scales they consumed nearly the entire item
  // width and left room for only one digit of the port number.
  ImGui::InputInt("Port", &mcpPort_, 0, 0);
  mcpPort_ = std::max(1, std::min(65535, mcpPort_));
  ImGui::EndDisabled();

  if (mcpRunning_) {
    ImGui::TextWrapped("Endpoint: http://%s:%d/mcp", mcpHostname_.data(),
                       mcpPort_);
    if (ImGui::Button("Stop server")) mcpStopRequested_ = true;
  } else {
    ImGui::BeginDisabled(mcpHostname_[0] == '\0');
    if (ImGui::Button("Start server")) mcpStartRequested_ = true;
    ImGui::EndDisabled();
  }
  if (!mcpStatus_.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", mcpStatus_.c_str());
  }
  ImGui::TextDisabled(
      "The MCP endpoint is unauthenticated. Use 127.0.0.1 unless remote "
      "access is intentional.");
  ImGui::End();
}

void Gui::drawAboutModal() {
  const char* kId = "About lusdview##about";
  if (showAbout_) {
    ImGui::OpenPopup(kId);
    showAbout_ = false;
  }

  const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  constexpr const char* kRepoUrl = "https://github.com/lighttransport/LightUSD";
  const RendererCaps* caps = renderer_ ? &renderer_->caps() : nullptr;
  const std::string lightusdVersion = LightUsdVersionString();
  const std::string cpuInfo = CpuInfoString();

  ImGui::TextUnformatted("lusdview");
  ImGui::Separator();
  ImGui::Text("lusdview version: %s", "development build");
  ImGui::Text("lightusd version: %s", lightusdVersion.c_str());
  ImGui::Text("Backend: %s", caps ? caps->backend_name : "?");
  ImGui::Text("GPU: %s", (caps && !caps->gpu_name.empty()) ? caps->gpu_name.c_str() : "?");
  ImGui::Text("Graphics API: %s",
              (caps && !caps->api_info.empty()) ? caps->api_info.c_str() : "?");
  ImGui::Text("GPU skinning: %s%s",
              (caps && caps->supportsGpuSkinning) ? "yes" : "no",
              (caps && caps->supportsExtendedGpuSkinning) ? " (extended influences)" : "");
  ImGui::Text("Ray tracing: %s%s",
              (caps && caps->supportsRayTracing) ? "available" : "unavailable",
              (renderer_ && renderer_->rayTracingActive()) ? ", active" : "");
  ImGui::Text("Weighted OIT: %s",
              (caps && caps->supportsWeightedOit) ? "available"
                                                  : "sorted fallback");
  ImGui::Separator();
  ImGui::Text("CPU: %s", cpuInfo.c_str());
  ImGui::Text("OS: %s", OperatingSystemString());
  ImGui::Text("Compiler: %s", CompilerString());
  ImGui::Separator();
  ImGui::TextUnformatted("Repository:");
  ImGui::SameLine();
  ImGui::TextUnformatted(kRepoUrl);
  ImGui::SameLine();
  if (ImGui::SmallButton("Copy URL")) {
    ImGui::SetClipboardText(kRepoUrl);
  }
  ImGui::Separator();
  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void Gui::drawLoadingModal() {
  const char* kId = "Loading USD##lusdview";
  if (loadStatus_.active && !ImGui::IsPopupOpen(kId)) {
    ImGui::OpenPopup(kId);
  }
  const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(kId, nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove)) {
    ImGui::TextUnformatted("Loading on a background thread...");
    ImGui::TextDisabled("%s", loadStatus_.path.c_str());
    ImGui::Separator();
    const auto phaseName = [&]() {
      switch (static_cast<LoadDetailPhase>(loadStatus_.detailPhase)) {
        case LoadDetailPhase::Parsing: return "Parsing USD";
        case LoadDetailPhase::Composing: return "Composing stage";
        case LoadDetailPhase::Converting: return "Converting geometry";
        case LoadDetailPhase::ProcessingTextures: return "Processing textures";
        case LoadDetailPhase::Finalizing: return "Finalizing scene";
      }
      return "Loading";
    };
    ImGui::Text("Phase: %s", phaseName());
    if (loadStatus_.texturesTotal > 0) {
      const float textureFrac = static_cast<float>(loadStatus_.texturesDone) /
                                static_cast<float>(loadStatus_.texturesTotal);
      ImGui::ProgressBar(std::clamp(textureFrac, 0.0f, 1.0f), ImVec2(300, 0));
      ImGui::Text("Textures: %lld / %lld", loadStatus_.texturesDone,
                  loadStatus_.texturesTotal);
    }
    if (loadStatus_.meshesTotal > 0) {
      const float meshFrac = static_cast<float>(loadStatus_.meshesDone) /
                             static_cast<float>(loadStatus_.meshesTotal);
      const float frac = loadStatus_.payloadsTotal > 0
                             ? 0.2f + 0.8f * meshFrac
                             : meshFrac;
      ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(300, 0));
      ImGui::Text("Meshes: %lld / %lld", loadStatus_.meshesDone,
                  loadStatus_.meshesTotal);
    } else if (loadStatus_.payloadsTotal > 0) {
      const float payloadFrac = static_cast<float>(loadStatus_.payloadsDone) /
                                static_cast<float>(loadStatus_.payloadsTotal);
      ImGui::ProgressBar(std::clamp(0.2f * payloadFrac, 0.0f, 0.2f),
                         ImVec2(300, 0));
      ImGui::Text("Payloads: %lld / %lld",
                  loadStatus_.payloadsDone, loadStatus_.payloadsTotal);
    } else {
      ImGui::ProgressBar(std::clamp(loadStatus_.phaseProgress, 0.0f, 1.0f),
                         ImVec2(300, 0));
      ImGui::Text("Stage progress: %.0f%%", loadStatus_.phaseProgress * 100.0f);
    }
    ImGui::Text("Elapsed: %.1f s", loadStatus_.elapsed);
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      wantCancelLoad_ = true;
    }
    if (!loadStatus_.active) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void Gui::drawProgressOverlay() {
  // Non-modal: the partial scene stays visible/interactive while geometry streams
  // to the GPU (raster) or the ray-tracing acceleration structure builds.
  const bool show = upload_.active || !upload_.note.empty();
  if (!show) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + 12.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##progress_overlay", nullptr, flags)) {
    auto bar = [](const char* label, size_t done, size_t total) {
      if (total == 0) return;
      const float frac = static_cast<float>(done) / static_cast<float>(total);
      char ov[64];
      std::snprintf(ov, sizeof(ov), "%s %zu / %zu", label, done, total);
      ImGui::ProgressBar(frac, ImVec2(260, 0), ov);
    };
    if (upload_.active) {
      ImGui::TextUnformatted("Streaming scene to GPU\xE2\x80\xA6");
      bar("meshes", upload_.meshesDone, upload_.meshesTotal);
      if (upload_.texTotal > 0)
        bar("textures", upload_.texDone, upload_.texTotal);
      if (upload_.volTotal > 0) bar("volumes", upload_.volDone, upload_.volTotal);
    }
    if (!upload_.note.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.2f, 1.0f), "%s", upload_.note.c_str());
    }
  }
  ImGui::End();
}

void Gui::buildDefaultLayout(unsigned int dockId) {
  ImGui::DockBuilderRemoveNode(dockId);
  ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

  ImGuiID center = dockId;
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
  ImGuiID leftBottom =
      ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.28f, nullptr, &left);
  ImGuiID rightBottom =
      ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.44f, nullptr, &right);
  // Timeline spans the bottom of the viewport column.
  ImGuiID centerBottom =
      ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.14f, nullptr, &center);

  ImGui::DockBuilderDockWindow("Hierarchy", left);
  ImGui::DockBuilderDockWindow("Stats", leftBottom);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Selection", right);
  ImGui::DockBuilderDockWindow("Camera", right);
  ImGui::DockBuilderDockWindow("Dome Light", right);
  ImGui::DockBuilderDockWindow("Stage", rightBottom);
  ImGui::DockBuilderDockWindow("Composition Graph", rightBottom);
  ImGui::DockBuilderDockWindow("Materials", rightBottom);
  ImGui::DockBuilderDockWindow("OpenPBR Material", rightBottom);
  ImGui::DockBuilderDockWindow("Payloads", rightBottom);
  ImGui::DockBuilderDockWindow("Viewport", center);
  ImGui::DockBuilderDockWindow("Timeline", centerBottom);
  ImGui::DockBuilderFinish(dockId);
}

void Gui::drawDockspaceAndMenu() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_MenuBar;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##LusdviewDockHost", nullptr, flags);
  ImGui::PopStyleVar(3);

  const ImGuiID dockId = ImGui::GetID("LusdviewDockspace");
  ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
  if (resetLayoutRequested_) {
    ImGui::ClearIniSettings();
    buildDefaultLayout(dockId);
    ImGui::MarkIniSettingsDirty();
    resetLayoutRequested_ = false;
    dockBuilt_ = true;
    imguiLayoutStatus_ = "UI layout reset to initial layout";
  } else if (!dockBuilt_) {
    bool hasSavedIni = false;
    if (const char* ini = ImGui::GetIO().IniFilename) {
      std::error_code ec;
      hasSavedIni = std::filesystem::is_regular_file(std::filesystem::path(ini), ec);
    }
    if (!hasSavedIni || ImGui::DockBuilderGetNode(dockId) == nullptr) {
      buildDefaultLayout(dockId);
    }
    dockBuilt_ = true;
  }

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Open...", "Ctrl+O")) wantOpen_ = true;
      if (ImGui::BeginMenu("Open Recent", !recentScenes_.empty())) {
        for (const std::string& p : recentScenes_) {
          // Label with the file name; full path in a tooltip (paths are long).
          std::string label = std::filesystem::path(p).filename().string();
          if (label.empty()) label = p;
          if (ImGui::MenuItem(label.c_str())) {
            recentToOpen_ = p;
            wantOpenRecent_ = true;
          }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.c_str());
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Reload", "Ctrl+R", false, loaded_ != nullptr)) wantReload_ = true;
      if (ImGui::MenuItem("Save ImGui Layout", "Ctrl+Shift+S")) {
        std::string err;
        if (saveImGuiLayout(&err)) {
          imguiLayoutStatus_ = "Layout saved";
        } else {
          imguiLayoutStatus_ = "Layout save failed: " + err;
        }
      }
      if (ImGui::BeginMenu("UI Layout")) {
        if (ImGui::MenuItem("Reset to initial layout")) {
          resetLayoutRequested_ = true;
        }
        ImGui::EndMenu();
      }
      if (!imguiLayoutStatus_.empty()) {
        ImGui::TextDisabled("%s", imguiLayoutStatus_.c_str());
      }
      {
        const bool haveDeferred =
            !deferredPayloadPaths_.empty() ||
            (loaded_ && !loaded_->comp.deferred.empty());
        if (ImGui::MenuItem("Load All Payloads", nullptr, false, haveDeferred)) {
          wantLoadAllPayloads_ = true;
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Quit", "Ctrl+Q / Esc Esc")) wantQuit_ = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Shaded", nullptr,
                          mode_ == RenderMode::Shaded && wireCycle_ == 0)) {
        mode_ = RenderMode::Shaded;
        wireCycle_ = 0;
      }
      if (ImGui::MenuItem("Wireframe", "V",
                          mode_ == RenderMode::Wireframe || wireCycle_ == 1)) {
        mode_ = RenderMode::Shaded;
        wireCycle_ = 1;
      }
      if (ImGui::MenuItem("Wireframe on shaded", nullptr, wireCycle_ == 2)) {
        mode_ = RenderMode::Shaded;
        wireCycle_ = 2;
      }
      ImGui::Separator();
      if (ImGui::BeginMenu("Selection display")) {
        static const char* labels[] = {"Off", "Bounds only",
                                       "Wireframe (depth tested)",
                                       "Wireframe (X-ray)"};
        for (int i = 0; i < 4; ++i) {
          if (ImGui::MenuItem(labels[i], i == 0 ? "K" : nullptr,
                              selectionHighlightMode_ == i)) {
            selectionHighlightMode_ = i;
            if (i > 0) selectionHighlightLastVisibleMode_ = i;
          }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Shift+K cycles modes");
        ImGui::EndMenu();
      }
      ImGui::Separator();
      // Debug AOV channels (grouped to keep the View menu tidy as they grow).
      if (ImGui::BeginMenu("Debug AOV")) {
        struct AovItem { const char* label; RenderMode m; };
        static const AovItem kAovs[] = {
            {"Material ID", RenderMode::MaterialId},
            {"Picking buffer", RenderMode::MeshId},
            {"Normals (shading)", RenderMode::Normals},
            {"Normals (coat)", RenderMode::CoatNormal},
            {"Coat weight", RenderMode::CoatWeight},
            {"Coat color", RenderMode::CoatColor},
            {"Coat roughness", RenderMode::CoatRoughness},
            {"Specular F0", RenderMode::SpecularF0},
            {"IOR F0", RenderMode::IorF0},
            {"Normals (geometric)", RenderMode::GeomNormal},
            {"UV", RenderMode::Uv},
            {"UV checker", RenderMode::UvChecker},
            {"UDIM tile", RenderMode::UdimTile},
            {"UV set 1", RenderMode::Uv1},
            {"Blendshape influence", RenderMode::BlendInfluence},
            {"Texel density", RenderMode::TexelDensity},
            {"Source face id", RenderMode::SourceFaceId},
            {"Depth", RenderMode::Depth},
            {"Position", RenderMode::Position},
            {"Albedo (unlit)", RenderMode::Albedo},
            {"Facing", RenderMode::Facing},
            {"Roughness", RenderMode::Roughness},
            {"Metallic", RenderMode::Metallic},
            {"Emissive", RenderMode::Emissive},
            {"Opacity", RenderMode::Opacity},
            {"Purpose", RenderMode::Purpose},
            {"Kind", RenderMode::Kind},
            {"Missing normals", RenderMode::MissingNormals},
            {"Double-sided", RenderMode::DoubleSided},
            {"Skin weights", RenderMode::SkinWeights},
            {"Tangent", RenderMode::Tangent},
            {"Curvature", RenderMode::Curvature},
            {"Instance ID", RenderMode::InstanceId},
            {"Ambient occlusion (RT)", RenderMode::AmbientOcclusion},
            {"Soft shadow (RT)", RenderMode::SoftShadow},
            {"BVH heatmap (CUDA)", RenderMode::BvhHeatmap},
        };
        for (const AovItem& a : kAovs)
          if (ImGui::MenuItem(a.label, nullptr, mode_ == a.m)) mode_ = a.m;
        ImGui::EndMenu();
      }
      ImGui::Separator();
      // Runtime backend switch: GL raster / Vulkan raster / Vulkan RT live-
      // switchable in place; CUDA/HIP RT overlay onto whichever owner is
      // active (CPU RT joins in a later phase). Vulkan Raster/RT availability
      // comes from the startup capability probe and the active renderer;
      // CUDA/HIP gray out after a switch attempt proves the device unavailable.
      if (ImGui::BeginMenu("Render Technique")) {
#if defined(HAVE_VULKAN)
        const bool vulkanCompiled = true;
#else
        const bool vulkanCompiled = false;
#endif
        const bool vkOwner = activeTechnique_ != RenderTechnique::GLRaster;
        const bool rtAvail = vkOwner && renderer_ && renderer_->rayTracingAvailable();
        auto item = [&](RenderTechnique t, bool enabled) {
          if (ImGui::MenuItem(RenderTechniqueLabel(t), nullptr,
                              activeTechnique_ == t, enabled)) {
            hasTechniqueRequest_ = true;
            requestedTechnique_ = t;
          }
        };
        item(RenderTechnique::GLRaster, true);
        item(RenderTechnique::VulkanRaster, vulkanCompiled && vulkanAvailable_);
        item(RenderTechnique::VulkanRT,
             vulkanCompiled && vulkanRtAvailable_ && (vkOwner ? rtAvail : true));
        item(RenderTechnique::CudaRT, cudaAvailable_);
        item(RenderTechnique::HipRT, hipAvailable_);
        item(RenderTechnique::CpuRT, true);  // always available, no device to probe
        ImGui::Separator();
        item(RenderTechnique::VulkanPathTrace,
             vulkanCompiled && vulkanRtAvailable_ && (vkOwner ? rtAvail : true));
        item(RenderTechnique::CudaPathTrace, cudaAvailable_);
        item(RenderTechnique::HipPathTrace, hipAvailable_);
        ImGui::Separator();
        if (ImGui::BeginMenu("CUDA device", !cudaDevices_.empty())) {
          for (size_t i = 0; i < cudaDevices_.size(); ++i) {
            if (ImGui::MenuItem(cudaDevices_[i].c_str(), nullptr,
                                cudaDeviceSelected_ == static_cast<int>(i))) {
              requestedCudaDevice_ = static_cast<int>(i);
              hasCudaDeviceRequest_ = true;
            }
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("CUDA traversal")) {
          auto traversalItem = [&](const char* label, CudaRtBackend backend,
                                   bool enabled = true) {
            if (ImGui::MenuItem(label, nullptr, cudaRtBackend_ == backend,
                                enabled)) {
              requestedCudaRtBackend_ = backend;
              hasCudaRtBackendRequest_ = true;
            }
          };
          traversalItem("Auto", CudaRtBackend::Auto);
          traversalItem("OptiX", CudaRtBackend::Optix, optixAvailable_);
          traversalItem("Software BVH", CudaRtBackend::SoftwareBvh);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("ROCm/HIP device", !hipDevices_.empty())) {
          for (size_t i = 0; i < hipDevices_.size(); ++i) {
            if (ImGui::MenuItem(hipDevices_[i].c_str(), nullptr,
                                hipDeviceSelected_ == static_cast<int>(i))) {
              requestedHipDevice_ = static_cast<int>(i);
              hasHipDeviceRequest_ = true;
            }
          }
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Path Trace Quality")) {
        bool final = pathTrace_.quality == PathTraceQuality::Final;
        if (ImGui::MenuItem("Interactive", nullptr, !final)) {
          const bool enabled = pathTrace_.enabled;
          pathTrace_ = PathTraceSettings::Interactive();
          pathTrace_.enabled = enabled;
        }
        if (ImGui::MenuItem("Final", nullptr, final)) {
          const bool enabled = pathTrace_.enabled;
          pathTrace_ = PathTraceSettings::Final();
          pathTrace_.enabled = enabled;
        }
        int samples = static_cast<int>(pathTrace_.targetSamples);
        int depth = static_cast<int>(pathTrace_.maxDepth);
        int rrDepth = static_cast<int>(pathTrace_.russianRouletteDepth);
        int motion = static_cast<int>(pathTrace_.motionSegments);
        if (ImGui::InputInt("Target samples", &samples))
          pathTrace_.targetSamples = static_cast<uint32_t>(std::max(samples, 0));
        if (ImGui::SliderInt("Max depth", &depth, 1, 64))
          pathTrace_.maxDepth = static_cast<uint32_t>(depth);
        if (ImGui::SliderInt("Roulette depth", &rrDepth, 0, depth))
          pathTrace_.russianRouletteDepth = static_cast<uint32_t>(rrDepth);
        if (ImGui::SliderInt("Motion segments", &motion, 1, 64))
          pathTrace_.motionSegments = static_cast<uint32_t>(motion);
        int denoise = static_cast<int>(pathTrace_.denoise);
        const char* modes[] = {"Off", "Auto", "On"};
        if (ImGui::Combo("Denoise", &denoise, modes, 3))
          pathTrace_.denoise = static_cast<PathTraceDenoise>(denoise);
        if (ImGui::DragFloat("Firefly clamp", &pathTrace_.fireflyClamp, 0.1f,
                             0.0f, 64.0f, "%.1f")) {
          pathTrace_.sanitize();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Robust local outlier threshold; lower is stronger.\n"
              "0 disables; scene-linear EXR output is never clamped.");
        }
        ImGui::TextDisabled("%s profile, seed %u",
                            PathTraceQualityLabel(pathTrace_.quality),
                            pathTrace_.seed);
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Skinning")) {
        auto item = [&](SkinningMode mode, const char* label) {
          if (ImGui::MenuItem(label, nullptr, skinning_.requested == mode)) {
            hasSkinningModeRequest_ = true;
            requestedSkinningMode_ = mode;
          }
        };
        item(SkinningMode::Auto, "Auto");
        item(SkinningMode::CPU, "CPU");
        item(SkinningMode::GPU, "GPU");
        ImGui::Separator();
        ImGui::TextDisabled("Effective: %s", SkinningModeLabel(skinning_.effective));
        if (!skinning_.reason.empty()) ImGui::TextDisabled("%s", skinning_.reason.c_str());
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Curve quality")) {
        ImGui::TextDisabled("Maximum ribbon segments per strand");
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("##curve_segments", &curveMaxSegments_, 2, 128))
          rebuildSubsetHighlight();
        if (ImGui::MenuItem("Full tessellation (slow)", nullptr,
                            curveMaxSegments_ == 0))
          curveMaxSegments_ = 0;
        if (ImGui::MenuItem("Preview (8)", nullptr, curveMaxSegments_ == 8))
          curveMaxSegments_ = 8;
        if (ImGui::MenuItem("Low (16)", nullptr, curveMaxSegments_ == 16))
          curveMaxSegments_ = 16;
        if (ImGui::MenuItem("Medium (32)", nullptr, curveMaxSegments_ == 32))
          curveMaxSegments_ = 32;
        if (ImGui::MenuItem("High (64)", nullptr, curveMaxSegments_ == 64))
          curveMaxSegments_ = 64;
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::BeginMenu("Navigation help")) {
        if (ImGui::MenuItem("Full", "F1",
                            navigationHelpMode_ == NavigationHelpMode::Full))
          navigationHelpMode_ = NavigationHelpMode::Full;
        if (ImGui::MenuItem("Simple", nullptr,
                            navigationHelpMode_ == NavigationHelpMode::Simple))
          navigationHelpMode_ = NavigationHelpMode::Simple;
        if (ImGui::MenuItem("None", nullptr,
                            navigationHelpMode_ == NavigationHelpMode::None))
          navigationHelpMode_ = NavigationHelpMode::None;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Camera")) {
        const bool haveCam = cam_ != nullptr;
        if (ImGui::MenuItem("Home", "0", false, haveCam)) homeView();
        if (ImGui::MenuItem("Isometric", "5", false, haveCam))
          applyViewPreset(CameraViewPreset::Isometric);
        ImGui::Separator();
        if (ImGui::MenuItem("Front", "1", false, haveCam))
          applyViewPreset(CameraViewPreset::Front);
        if (ImGui::MenuItem("Back", "Shift+1", false, haveCam))
          applyViewPreset(CameraViewPreset::Back);
        if (ImGui::MenuItem("Right", "3", false, haveCam))
          applyViewPreset(CameraViewPreset::Right);
        if (ImGui::MenuItem("Left", "Shift+3", false, haveCam))
          applyViewPreset(CameraViewPreset::Left);
        if (ImGui::MenuItem("Top", "7", false, haveCam))
          applyViewPreset(CameraViewPreset::Top);
        if (ImGui::MenuItem("Bottom", "Shift+7", false, haveCam))
          applyViewPreset(CameraViewPreset::Bottom);
        ImGui::Separator();
        if (ImGui::BeginMenu("Bookmarks")) {
          for (int slot = 0; slot < 3; ++slot) {
            const int humanSlot = slot + 1;
            std::string loadLabel = "Recall bookmark " + std::to_string(humanSlot);
            std::string saveLabel = "Save bookmark " + std::to_string(humanSlot);
            std::string loadShortcut = "Ctrl+" + std::to_string(humanSlot);
            std::string saveShortcut = "Ctrl+Shift+" + std::to_string(humanSlot);
            if (ImGui::MenuItem(loadLabel.c_str(), loadShortcut.c_str(), false,
                                haveCam && hasCameraBookmark(slot))) {
              loadCameraBookmark(slot);
            }
            if (ImGui::MenuItem(saveLabel.c_str(), saveShortcut.c_str(), false, haveCam)) {
              saveCameraBookmark(slot);
            }
          }
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
      ImGui::Separator();
      const bool haveBounds = draw_ && draw_->hasBounds;
      const bool haveMeshes = draw_ && !draw_->meshes.empty();
      if (ImGui::MenuItem("Selection back", "Alt+Left", false, canGoSelectionBack())) {
        goSelectionBack();
      }
      if (ImGui::MenuItem("Selection forward", "Alt+Right", false,
                          canGoSelectionForward())) {
        goSelectionForward();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Previous selection", "[", false, haveMeshes)) {
        selectAdjacentMesh(-1);
      }
      if (ImGui::MenuItem("Next selection", "]", false, haveMeshes)) {
        selectAdjacentMesh(1);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Frame selected", "F", false, haveBounds)) frameSelected();
      if (ImGui::MenuItem("Frame all", "A", false, haveBounds)) frameAll();
      ImGui::Separator();
      // Hide family (Maya): also available via H / Ctrl+H / Shift+H / Alt+H.
      const bool haveSel = selectionHasRenderable();
      if (ImGui::MenuItem("Hide selection", "Ctrl+H", false, haveSel)) {
        setSelectionVisibility(false);
      }
      if (ImGui::MenuItem("Show selected only", "Alt+H", false, haveSel)) {
        isolateSelection();
      }
      if (ImGui::MenuItem("Show all", nullptr, false,
                          !meshVisible_.empty() || !carrierVisible_.empty())) {
        unhideAll();
      }
      if (ImGui::MenuItem("Show RenderScene nodes", nullptr, &showRenderNodes_)) {
        if (!selPath_.empty()) revealSelectionInHierarchy_ = true;
      }
      ImGui::Separator();

      ImGui::MenuItem("Grid", nullptr, &showGrid_);
      ImGui::MenuItem("Axes", nullptr, &showAxes_);
      ImGui::MenuItem("Scene bounds", nullptr, &showSceneBbox_);
      bool selectedBounds = selectionHighlightMode_ == 1;
      if (ImGui::MenuItem("Selected bounds", nullptr, &selectedBounds)) {
        selectionHighlightMode_ = selectedBounds ? 1 : 0;
        if (selectedBounds) selectionHighlightLastVisibleMode_ = 1;
      }
      ImGui::MenuItem("Skeleton", nullptr, &showSkeleton_);
      ImGui::MenuItem("Frustum culling", nullptr, &cullEnabled_);
      ImGui::Separator();
      if (ImGui::BeginMenu("Purpose")) {
        ImGui::MenuItem("default", nullptr, &showPurposeDefault_);
        ImGui::MenuItem("render", nullptr, &showPurposeRender_);
        ImGui::MenuItem("proxy", nullptr, &showPurposeProxy_);
        ImGui::MenuItem("guide", nullptr, &showPurposeGuide_);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      ImGui::MenuItem("Lights", nullptr, &showLights_);
      ImGui::MenuItem("Dome Light panel", nullptr, &showDomeLight_);
      ImGui::MenuItem("Cameras", nullptr, &showCameras_);
      ImGui::MenuItem("Extent attribute", nullptr, &showExtent_);
      ImGui::MenuItem("Prim labels", nullptr, &showPrimLabels_);
      ImGui::Separator();
      if (ImGui::BeginMenu("Transform")) {
        bool tNone = xformMode_ == TransformMode::None;
        bool tTrans = xformMode_ == TransformMode::Translate;
        bool tRot = xformMode_ == TransformMode::Rotate;
        bool tScale = xformMode_ == TransformMode::Scale;
        if (ImGui::MenuItem("None", nullptr, tNone)) xformMode_ = TransformMode::None;
        if (ImGui::MenuItem("Translate", "W", tTrans)) xformMode_ = TransformMode::Translate;
        if (ImGui::MenuItem("Rotate", "E", tRot)) xformMode_ = TransformMode::Rotate;
        if (ImGui::MenuItem("Scale", "R", tScale)) xformMode_ = TransformMode::Scale;
        ImGui::EndMenu();
      }
      ImGui::SetNextItemWidth(80.0f);
      ImGui::SliderFloat("Tessellation", &tessQuality_, 0.25f, 4.0f, "%.2f");
      ImGui::Separator();
      ImGui::MenuItem("Displacement", nullptr, &displacementEnabled_);
      ImGui::SetNextItemWidth(80.0f);
      ImGui::SliderFloat("Disp scale", &displacementScale_, 0.0f, 4.0f, "%.2f");
      ImGui::SetNextItemWidth(80.0f);
      ImGui::SliderInt("Max tess", &maxTessLevel_, 1, 16);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
      if (ImGui::MenuItem("MCP Server", nullptr, showMcpServer_)) {
        showMcpServer_ = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      if (ImGui::MenuItem("Cycle navigation help", "F1"))
        cycleNavigationHelpMode();
      ImGui::Separator();
      ImGui::TextDisabled("Viewport");
      ImGui::TextUnformatted("Alt+LMB  Orbit");
      ImGui::TextUnformatted("Alt+MMB / Shift+Alt+LMB  Pan");
      ImGui::TextUnformatted("Alt+RMB / Wheel  Dolly");
      ImGui::TextUnformatted("W / S  Move forward / backward (Shift = fast)");
      ImGui::TextUnformatted("V  Cycle wireframe (off / wire / wire+shade)");
      ImGui::Separator();
      ImGui::TextDisabled("Selection");
      ImGui::TextUnformatted("[ / ]  Previous / Next visible selection");
      ImGui::TextUnformatted("Double-click hierarchy item  Frame subtree");
      ImGui::Separator();
      ImGui::TextDisabled("Camera");
      ImGui::TextUnformatted("0 Home   5 Isometric");
      ImGui::TextUnformatted("1/Shift+1 Front/Back");
      ImGui::TextUnformatted("3/Shift+3 Right/Left");
      ImGui::TextUnformatted("7/Shift+7 Top/Bottom");
      ImGui::TextUnformatted("Ctrl+1..3 Recall bookmark");
      ImGui::TextUnformatted("Ctrl+Shift+1..3 Save bookmark");
      ImGui::Separator();
      ImGui::TextDisabled("Application");
      ImGui::TextUnformatted("Ctrl+Q / Esc Esc  Quit");
      ImGui::Separator();
      if (ImGui::MenuItem("About lusdview")) showAbout_ = true;
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }
  ImGui::End();
}

void Gui::selectByPath(const std::string& absPath, int meshIndex) {
  applySelection(absPath, meshIndex, /*recordHistory=*/true);
  setSelectionListSingle(selPath_, selMeshIndex_);
}

bool Gui::saveImGuiLayout(std::string* err) {
  const char* ini = ImGui::GetIO().IniFilename;
  if (!ini || !*ini) {
    if (err) *err = "ImGui layout persistence is disabled";
    return false;
  }
  const std::filesystem::path path(ini);
  std::error_code ec;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  if (ec) {
    if (err) *err = "could not create " + parent.string() + ": " + ec.message();
    return false;
  }
  ImGui::SaveIniSettingsToDisk(ini);
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    if (err) *err = "ImGui did not write " + path.string();
    return false;
  }
  return true;
}

Gui::PickReport Gui::pickViewportPixel(float x, float y, bool select) {
  PickReport report;
  report.x = x;
  report.y = y;
  report.viewportWidth = viewportW_;
  report.viewportHeight = viewportH_;
  if (!draw_ || !cam_ || !renderer_ || viewportW_ <= 0 || viewportH_ <= 0 ||
      !std::isfinite(x) || !std::isfinite(y) ||
      x < 0.0f || y < 0.0f || x >= static_cast<float>(viewportW_) ||
      y >= static_cast<float>(viewportH_)) {
    return report;
  }
  float hitDistance = 0.0f;
  report.path =
      pickCarrierPath(x, y, viewportW_, viewportH_, &hitDistance);
  if (report.path.empty()) return report;
  report.hitDistance = hitDistance;
  for (size_t i = 0; i < draw_->curves.size(); ++i) {
    if (draw_->curves[i].absPath == report.path) {
      report.kind = "curve";
      break;
    }
  }
  if (report.kind.empty()) {
    for (size_t i = 0; i < draw_->points.size(); ++i) {
      if (draw_->points[i].absPath == report.path) {
        report.kind = "points";
        break;
      }
    }
  }
  if (report.kind.empty()) {
    for (size_t i = 0; i < draw_->meshes.size(); ++i) {
      if (draw_->meshes[i].absPath == report.path) {
        report.kind = "mesh";
        report.meshIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (report.kind.empty()) report.kind = "prim";
  if (select) {
    selectByPath(report.path, report.meshIndex);
    light3d::Vec3 origin, direction;
    if (hitDistance > 0.0f && std::isfinite(hitDistance) &&
        ViewportRay(*cam_, renderer_->caps().usesZeroToOneDepth, x, y,
                    viewportW_, viewportH_, &origin, &direction)) {
      lastPickedFocusPoint_ = origin + direction * hitDistance;
      lastPickedFocusPath_ = report.path;
      haveLastPickedFocusPoint_ = true;
    }
  }
  return report;
}

Gui::PickReport Gui::pickViewportRegion(float x0, float y0, float x1, float y1,
                                        bool select) {
  PickReport report;
  report.viewportWidth = viewportW_;
  report.viewportHeight = viewportH_;
  if (!draw_ || !cam_ || !renderer_ || viewportW_ <= 0 || viewportH_ <= 0)
    return report;
  const int rx0 = std::max(0, std::min(viewportW_ - 1,
      static_cast<int>(std::floor(std::min(x0, x1)))));
  const int ry0 = std::max(0, std::min(viewportH_ - 1,
      static_cast<int>(std::floor(std::min(y0, y1)))));
  const int rx1 = std::max(rx0 + 1, std::min(viewportW_,
      static_cast<int>(std::ceil(std::max(x0, x1)))));
  const int ry1 = std::max(ry0 + 1, std::min(viewportH_,
      static_cast<int>(std::ceil(std::max(y0, y1)))));
  report.x = 0.5f * static_cast<float>(rx0 + rx1);
  report.y = 0.5f * static_cast<float>(ry0 + ry1);
  const int rw = rx1 - rx0, rh = ry1 - ry0;
  const size_t pixels = static_cast<size_t>(rw) * static_cast<size_t>(rh);
  constexpr size_t kMaxRegionPixels = size_t{16} * 1024 * 1024;
  if (pixels > kMaxRegionPixels) return report;

  std::vector<uint32_t> ids(pixels, 0);
  std::vector<float> depths(pixels, 1.0e30f);
  const bool z01 = renderer_->caps().usesZeroToOneDepth;
  const light3d::Mat4 VP = cam_->proj(z01) * cam_->view();
  const light3d::Vec3 eye = cam_->eye();
  const light3d::Mat4 view = cam_->view();
  const light3d::Vec3 cameraRight{view.m[0], view.m[4], view.m[8]};
  auto project = [&](const light3d::Vec3& p, float& sx, float& sy,
                     float& depth) {
    const float* m = VP.m;
    const float cx=m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12];
    const float cy=m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13];
    const float cz=m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14];
    const float cw=m[3]*p.x+m[7]*p.y+m[11]*p.z+m[15];
    if (!std::isfinite(cw) || cw <= 1.0e-7f) return false;
    const float iw=1.0f/cw;
    sx=(cx*iw*0.5f+0.5f)*viewportW_;
    sy=(1.0f-(cy*iw*0.5f+0.5f))*viewportH_;
    depth=cz*iw;
    return std::isfinite(sx) && std::isfinite(sy) && std::isfinite(depth);
  };
  for (size_t ci=0; ci<draw_->curves.size(); ++ci) {
    const DrawCurvesCPU& c=draw_->curves[ci];
    if (!carrierVisibleForView(draw_->points.size()+ci) || c.points.size()<6)
      continue;
    const size_t np=c.points.size()/3;
    const std::vector<uint32_t> counts=c.vertexCounts.empty()
        ? std::vector<uint32_t>{static_cast<uint32_t>(np)} : c.vertexCounts;
    const float sx=std::sqrt(c.world[0]*c.world[0]+c.world[1]*c.world[1]+c.world[2]*c.world[2]);
    const float sy=std::sqrt(c.world[4]*c.world[4]+c.world[5]*c.world[5]+c.world[6]*c.world[6]);
    const float sz=std::sqrt(c.world[8]*c.world[8]+c.world[9]*c.world[9]+c.world[10]*c.world[10]);
    const float scale=std::max(sx,std::max(sy,sz));
    auto point=[&](size_t i) {
      const float x=c.points[i*3], y=c.points[i*3+1], z=c.points[i*3+2];
      return light3d::Vec3{c.world[0]*x+c.world[4]*y+c.world[8]*z+c.world[12],
        c.world[1]*x+c.world[5]*y+c.world[9]*z+c.world[13],
        c.world[2]*x+c.world[6]*y+c.world[10]*z+c.world[14]};
    };
    size_t base=0;
    for (uint32_t count:counts) {
      const size_t end=base>=np?np:base+std::min(np-base,static_cast<size_t>(count));
      const size_t strandPoints=end-base;
      const size_t pickSegments=curveMaxSegments_>0&&strandPoints>1
          ?std::min(strandPoints-1,static_cast<size_t>(curveMaxSegments_))
          :(strandPoints>1?strandPoints-1:0);
      for (size_t sj=0;sj<pickSegments;++sj) {
        const size_t i=base+(sj*(strandPoints-1)+pickSegments/2)/pickSegments;
        const size_t ni=base+((sj+1)*(strandPoints-1)+pickSegments/2)/pickSegments;
        const light3d::Vec3 a=point(i), b=point(ni), mid=(a+b)*0.5f;
        float ax,ay,az,bx,by,bz;
        if (!project(a,ax,ay,az)||!project(b,bx,by,bz)) continue;
        const float w0=c.widths.empty()?1.0f:(c.widths.size()==1?c.widths[0]:c.widths[std::min(i,c.widths.size()-1)]);
        const float w1=c.widths.empty()?1.0f:(c.widths.size()==1?c.widths[0]:c.widths[std::min(ni,c.widths.size()-1)]);
        const float width=0.5f*(w0+w1)*scale;
        light3d::Vec3 side=light3d::cross(b-a,eye-mid);
        side=light3d::dot(side,side)>1.0e-10f?light3d::normalize(side):cameraRight;
        float ex,ey,ez; float radius=2.0f;
        if (project(mid+side*(0.5f*width),ex,ey,ez))
          radius=std::max(2.0f,std::min(64.0f,std::hypot(ex-(ax+bx)*0.5f,ey-(ay+by)*0.5f)));
        const int ix0=std::max(rx0,static_cast<int>(std::floor(std::min(ax,bx)-radius)));
        const int ix1=std::min(rx1-1,static_cast<int>(std::ceil(std::max(ax,bx)+radius)));
        const int iy0=std::max(ry0,static_cast<int>(std::floor(std::min(ay,by)-radius)));
        const int iy1=std::min(ry1-1,static_cast<int>(std::ceil(std::max(ay,by)+radius)));
        const float dx=bx-ax,dy=by-ay,len2=dx*dx+dy*dy;
        for(int y=iy0;y<=iy1;++y) for(int x=ix0;x<=ix1;++x) {
          const float qx=x+0.5f,qy=y+0.5f;
          const float u=len2>1.0e-8f?std::max(0.0f,std::min(1.0f,((qx-ax)*dx+(qy-ay)*dy)/len2)):0.0f;
          const float ddx=ax+u*dx-qx,ddy=ay+u*dy-qy;
          if(ddx*ddx+ddy*ddy>radius*radius) continue;
          const float depth=az*(1.0f-u)+bz*u;
          const size_t pi=static_cast<size_t>(y-ry0)*rw+static_cast<size_t>(x-rx0);
          if(depth<depths[pi]) { depths[pi]=depth; ids[pi]=static_cast<uint32_t>(ci+1); }
        }
      }
      base=end;
    }
  }
  std::vector<int> coverage(draw_->curves.size(),0);
  for(uint32_t id:ids) if(id>0&&id<=coverage.size()) ++coverage[id-1];
  const auto best=std::max_element(coverage.begin(),coverage.end());
  if(best!=coverage.end()&&*best>0) {
    const size_t ci=static_cast<size_t>(best-coverage.begin());
    report.path=draw_->curves[ci].absPath;
    report.kind="curve";
    report.coveredPixels=*best;
    if(select) selectByPath(report.path,-1);
    return report;
  }
  // No curve pixels: use the center pixel's exact mesh triangle picker.
  return pickViewportPixel(report.x,report.y,select);
}

void Gui::pushSelectionHistory(const std::string& absPath) {
  if (absPath.empty()) return;
  if (selectionHistoryIndex_ >= 0 &&
      selectionHistoryIndex_ < static_cast<int>(selectionHistory_.size()) &&
      selectionHistory_[static_cast<size_t>(selectionHistoryIndex_)] == absPath) {
    return;
  }

  if ((selectionHistoryIndex_ + 1) < static_cast<int>(selectionHistory_.size())) {
    selectionHistory_.erase(selectionHistory_.begin() + selectionHistoryIndex_ + 1,
                            selectionHistory_.end());
  }
  selectionHistory_.push_back(absPath);
  selectionHistoryIndex_ = static_cast<int>(selectionHistory_.size()) - 1;
}

void Gui::applySelection(const std::string& absPath, int meshIndex, bool recordHistory) {
  // A hierarchy/history selection has no longer got the precise viewport hit
  // associated with the previous selection. A viewport pick restores it after
  // selectByPath() returns.
  lastPickedFocusPath_.clear();
  haveLastPickedFocusPoint_ = false;
  selPath_ = absPath;
  selMeshIndex_ = meshIndex;
  selPrim_ = nullptr;
  revealSelectionInHierarchy_ = !absPath.empty();
  if (loaded_) {
    for (const auto& root : loaded_->stage.root_prims()) {
      if (const lightusd::Prim* p = FindPrimByPath(root, absPath)) {
        selPrim_ = p;
        break;
      }
    }
    // FindPrimByPath misses prims whose composed absolute_path is unset (e.g.
    // GeomSubset children); fall back to the stage's path lookup.
    if (!selPrim_ && !absPath.empty()) {
      const lightusd::Prim* p = nullptr;
      std::string err;
      if (loaded_->stage.find_prim_at_path(lightusd::Path(absPath, ""), p, &err) && p) {
        selPrim_ = p;
      }
    }
  }
  if (meshIndex < 0 && draw_) {
    for (size_t i = 0; i < draw_->meshes.size(); ++i) {
      if (draw_->meshes[i].absPath == absPath) {
        selMeshIndex_ = static_cast<int>(i);
        break;
      }
    }
  }
  if (recordHistory && !absPath.empty()) pushSelectionHistory(absPath);
  if (inspectorCachePrim_ != selPrim_ || inspectorCachePath_ != selPath_) {
    inspectorCachePrim_ = nullptr;
    inspectorCachePath_.clear();
    inspectorCacheRows_.clear();
  }
  rebuildSubsetHighlight();
}

// Build the selection-highlight geometry: the GL backend draws a polygon-mode
// wireframe (whole mesh via highlightMeshIndex, or a GeomSubset's triangles via
// highlightSubsetIndices_); the Vulkan backend, lacking a wireframe pass, draws
// world-space orange edge lines (highlightLinesData_). A selected GeomSubset
// outlines exactly its faces (mapped through the mesh's sourceFaceId).
void Gui::rebuildSubsetHighlight() {
  highlightSubsetIndices_.clear();
  highlightSubsetMesh_ = -1;
  highlightLinesData_.clear();
  if (!draw_) return;

  // Resolve the highlighted mesh + the triangle vertex-index list to outline.
  int mi = -1;
  const std::vector<uint32_t>* tri = nullptr;  // index list (3 per triangle)

  if (selPrim_) {
    if (const auto* gs = selPrim_->as<lightusd::GeomSubset>()) {
      if (gs->elementType.get_value() == lightusd::GeomSubset::ElementType::Face) {
        std::set<uint32_t> faces;
        if (auto opt = gs->indices.get_value()) {
          std::vector<int32_t> fi;
          if (opt.value().get_scalar(&fi))
            for (int32_t f : fi)
              if (f >= 0) faces.insert(static_cast<uint32_t>(f));
        }
        std::string meshPath = selPath_;
        const size_t slash = meshPath.find_last_of('/');
        if (!faces.empty() && slash != std::string::npos && slash != 0) {
          meshPath.resize(slash);
          for (size_t i = 0; i < draw_->meshes.size(); ++i)
            if (draw_->meshes[i].absPath == meshPath) { mi = static_cast<int>(i); break; }
          if (mi >= 0) {
            const DrawMeshCPU& m = draw_->meshes[static_cast<size_t>(mi)];
            if (m.sourceFaceId.size() == m.indices.size() / 3) {
              for (size_t t = 0; t < m.sourceFaceId.size(); ++t)
                if (faces.count(m.sourceFaceId[t])) {
                  highlightSubsetIndices_.push_back(m.indices[t * 3 + 0]);
                  highlightSubsetIndices_.push_back(m.indices[t * 3 + 1]);
                  highlightSubsetIndices_.push_back(m.indices[t * 3 + 2]);
                }
              if (!highlightSubsetIndices_.empty()) {
                highlightSubsetMesh_ = mi;
                tri = &highlightSubsetIndices_;
              } else {
                mi = -1;
              }
            } else {
              mi = -1;
            }
          }
        }
      }
    }
  }
  // No GeomSubset: a selected mesh highlights all its triangles.
  if (mi < 0 && selMeshIndex_ >= 0 &&
      static_cast<size_t>(selMeshIndex_) < draw_->meshes.size()) {
    mi = selMeshIndex_;
    tri = &draw_->meshes[static_cast<size_t>(mi)].indices;
  }
  if (mi < 0 || !tri) {
    const float orange[3] = {1.0f, 0.55f, 0.1f};
    auto addLine = [&](const float a[3], const float b[3]) {
      HelperVertex va{}, vb{};
      std::copy(a, a + 3, va.pos); std::copy(b, b + 3, vb.pos);
      std::copy(orange, orange + 3, va.col); std::copy(orange, orange + 3, vb.col);
      highlightLinesData_.push_back(va); highlightLinesData_.push_back(vb);
    };
    auto wp = [](const auto& c, size_t i, float p[3]) {
      const float x = c.points[i*3], y = c.points[i*3+1], z = c.points[i*3+2];
      p[0] = c.world[0]*x + c.world[4]*y + c.world[8]*z + c.world[12];
      p[1] = c.world[1]*x + c.world[5]*y + c.world[9]*z + c.world[13];
      p[2] = c.world[2]*x + c.world[6]*y + c.world[10]*z + c.world[14];
    };
    for (const DrawPointsCPU& p : draw_->points) if (p.absPath == selPath_) {
      for (size_t i = 0; i < p.points.size()/3; ++i) {
        float q[3]; wp(p, i, q);
        const float w = p.widths.empty() ? 1.0f : p.widths.size() == 1 ? p.widths[0] : p.widths[std::min(i, p.widths.size()-1)];
        float x[3] = {q[0] + w, q[1], q[2]}, y[3] = {q[0], q[1] + w, q[2]};
        float xm[3] = {q[0] - w, q[1], q[2]}, ym[3] = {q[0], q[1] - w, q[2]};
        addLine(xm, x); addLine(ym, y);
      }
      return;
    }
    for (const DrawCurvesCPU& c : draw_->curves) if (c.absPath == selPath_) {
      const size_t pointCount = c.points.size() / 3;
      const std::vector<uint32_t> counts = c.vertexCounts.empty()
          ? std::vector<uint32_t>{static_cast<uint32_t>(pointCount)}
          : c.vertexCounts;
      size_t base = 0;
      for (uint32_t count : counts) {
        const size_t remaining = pointCount - std::min(base, pointCount);
        const size_t end = base >= pointCount
                               ? pointCount
                               : base + std::min(remaining,
                                                 static_cast<size_t>(count));
        // Each curve is an independent strand. Never add an edge from the end
        // of one strand to the start of the next: on XGen leaf bundles such as
        // xgLeavesI those bogus edges span the whole plant as giant loops.
        const size_t strandPoints = end - base;
        const size_t segments = curveMaxSegments_ > 0 && strandPoints > 1
            ? std::min(strandPoints - 1,
                       static_cast<size_t>(curveMaxSegments_))
            : (strandPoints > 1 ? strandPoints - 1 : 0);
        for (size_t j = 0; j < segments; ++j) {
          const size_t i = base +
              (j * (strandPoints - 1) + segments / 2) / segments;
          const size_t next = base +
              ((j + 1) * (strandPoints - 1) + segments / 2) / segments;
          float a[3], b[3];
          wp(c, i, a);
          wp(c, next, b);
          addLine(a, b);
        }
        base = end;
      }
      return;
    }
    return;
  }

  // World-space orange edge lines (for the Vulkan line-pipeline highlight).
  const DrawMeshCPU& m = draw_->meshes[static_cast<size_t>(mi)];
  const float* W = m.world;  // column-major
  // GL draws this mesh's highlight through the GPU-deformed polygon overlay
  // (gl_renderer.cc's highlightMeshIndex path), so it naturally follows
  // skinning. Vulkan has no wireframe pass and relies entirely on these
  // CPU-built lines (draw_'s m.vertices are always REST pose -- GPU skinning
  // deforms in the vertex shader without writing back to the CPU array), so
  // they must be posed here or a skinned selection shows its frozen rest-pose
  // silhouette over the moving mesh (visible as a mismatched wireframe net).
  std::vector<DrawVertex> skinnedVerts;
  const std::vector<DrawVertex>* srcVerts = &m.vertices;
  // Both re-pose paths below produce OBJECT-space vertices that still need the
  // normal `W *` multiply below -- confirmed empirically against
  // BuildHostScene (rt_scene_build.cc), which needed the identical fix: its
  // mesh flatten read draw_'s (already BuildNextRtDeformedVertices-posed)
  // vertices directly with no world multiply, silently wrong for any mesh
  // with a non-identity world (animatedWorld meshes: gpu_scene.hh's `world`
  // field is a real per-frame transform, not just a load-time bake-in
  // placeholder). Skipping the multiply here equally double-counts or
  // under-counts depending on the mesh, visible as an oversized/offset or
  // wrongly-shaped wireframe net relative to the correctly-posed shaded mesh.
  if (drawIsPosed_) {
    // App::poseNextDrawForTracer already wrote the pose at this time code
    // straight into draw_ for a CUDA/HIP/CPU RT build, so m.vertices IS the
    // posed geometry -- deforming it again below would double-deform it (the
    // "wireframe breaks after an RT render" case: draw_ stays posed after the
    // tracer runs, and every later highlight rebuild compounded another pose
    // on top). Leave srcVerts pointing at m.vertices.
  } else if (nextStage_) {
    // --next loader: there is no tydra RenderScene to re-pose from (see
    // gpu_scene.hh's NextSkelBinding comment) -- re-pose via the retained next
    // Stage instead, the same mechanism the CUDA/HIP/CPU RT tracers already
    // use (BuildNextRtDeformedVertices, next_scene_loader.hh).
    std::vector<RtSkinnedMeshUpload> posed;
    if (BuildNextRtDeformedVertices(nextStage_.get(), *draw_, timeline_.applied,
                                    nullptr, &posed)) {
      for (const RtSkinnedMeshUpload& u : posed) {
        if (u.meshIndex == mi) {
          skinnedVerts = u.vertices;
          srcVerts = &skinnedVerts;
          break;
        }
      }
    }
  } else if (m.skelId >= 0 && m.skinMatrixBase >= 0 && !m.jointIdx.empty() && loaded_) {
    std::unordered_map<int, std::vector<lightusd::value::matrix4d>> skinCache;
    std::vector<lightusd::value::matrix4d> composed;
    if (BuildComposedSkinningMatrices(loaded_->render, m, timeline_.applied,
                                      &skinCache, &composed)) {
      skinnedVerts = m.vertices;
      if (ApplySkinningToVertices(m, composed, &skinnedVerts)) srcVerts = &skinnedVerts;
    }
  }
  auto wpos = [&](uint32_t vi, float o[3]) {
    const DrawVertex& v = (*srcVerts)[vi];
    o[0] = W[0] * v.px + W[4] * v.py + W[8] * v.pz + W[12];
    o[1] = W[1] * v.px + W[5] * v.py + W[9] * v.pz + W[13];
    o[2] = W[2] * v.px + W[6] * v.py + W[10] * v.pz + W[14];
  };
  const float orange[3] = {1.0f, 0.55f, 0.1f};
  highlightLinesData_.reserve(tri->size() * 2);
  const size_t nv = srcVerts->size();
  for (size_t t = 0; t + 2 < tri->size(); t += 3) {
    const uint32_t a = (*tri)[t], b = (*tri)[t + 1], c = (*tri)[t + 2];
    if (a >= nv || b >= nv || c >= nv) continue;
    HelperVertex va{}, vb{}, vc{};
    wpos(a, va.pos); wpos(b, vb.pos); wpos(c, vc.pos);
    for (int k = 0; k < 3; ++k) { va.col[k] = vb.col[k] = vc.col[k] = orange[k]; }
    highlightLinesData_.push_back(va); highlightLinesData_.push_back(vb);
    highlightLinesData_.push_back(vb); highlightLinesData_.push_back(vc);
    highlightLinesData_.push_back(vc); highlightLinesData_.push_back(va);
  }
}

void Gui::clearSelection() {
  selPath_.clear();
  selMeshIndex_ = -1;
  selPrim_ = nullptr;
  lastPickedFocusPath_.clear();
  haveLastPickedFocusPoint_ = false;
  selectionList_.clear();
  revealSelectionInHierarchy_ = false;
  inspectorCachePrim_ = nullptr;
  inspectorCachePath_.clear();
  inspectorCacheRows_.clear();
  // Drop the highlight geometry too, exactly as selecting rebuilds it. The GL
  // backend keys its selection overlay off highlightMeshIndex (cleared to -1
  // above), so it stops drawing on its own -- but the Vulkan backend has no
  // wireframe pass and draws the CPU-built world-space edge lines instead, so
  // without this the last selection's wireframe stayed on screen forever after
  // deselecting.
  rebuildSubsetHighlight();
}

void Gui::setCullAsync(bool on) {
  if (cullAsync_ == on) return;
  joinCullWorker();
  cullAsync_ = on;
  lastCullValid_ = false;
}

void Gui::setSceneMutating(bool on) {
  if (sceneMutating_ == on) return;
  joinCullWorker();
  ++cullSceneGen_;  // cull snapshots (grids) index the outgoing scene
  sceneMutating_ = on;
  lastCullValid_ = false;
  instGrids_.clear();
  instGridsFor_ = nullptr;
}

void Gui::prepareSceneSwap() {
  // Invalidate the worker snapshot first so an in-flight worker aborts at its
  // next mesh boundary instead of reading geometry the caller is about to free;
  // then join it, which is the actual memory-safety barrier.
  ++cullSceneGen_;
  joinCullWorker();
  instGrids_.clear();
  instGridsFor_ = nullptr;
  lastCullValid_ = false;
}

void Gui::setSelectionListSingle(const std::string& absPath, int meshIndex) {
  selectionList_.clear();
  if (!absPath.empty()) selectionList_.push_back({absPath, meshIndex});
}

void Gui::setSelectionListFromMeshes(std::vector<int> meshIndices) {
  selectionList_.clear();
  if (!draw_) return;
  selectionList_.reserve(meshIndices.size());
  for (int meshIndex : meshIndices) {
    if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= draw_->meshes.size()) {
      continue;
    }
    const DrawMeshCPU& mesh = draw_->meshes[static_cast<size_t>(meshIndex)];
    if (mesh.absPath.empty()) continue;
    selectionList_.push_back({mesh.absPath, meshIndex});
  }
  if (!selectionList_.empty()) {
    focusSelectionListItem(0);
  } else {
    clearSelection();
  }
}

void Gui::setSelectionListFromPaths(std::vector<std::string> paths) {
  selectionList_.clear();
  for (const std::string& path : paths) {
    if (!path.empty()) selectionList_.push_back({path, -1});
  }
  if (!selectionList_.empty()) focusSelectionListItem(0);
  else clearSelection();
}

void Gui::focusSelectionListItem(size_t index) {
  if (index >= selectionList_.size()) return;
  applySelection(selectionList_[index].first, selectionList_[index].second,
                 /*recordHistory=*/true);
}

bool Gui::canGoSelectionBack() const {
  return selectionHistoryIndex_ > 0 &&
         selectionHistoryIndex_ <= static_cast<int>(selectionHistory_.size()) - 1;
}

bool Gui::canGoSelectionForward() const {
  return selectionHistoryIndex_ >= 0 &&
         (selectionHistoryIndex_ + 1) < static_cast<int>(selectionHistory_.size());
}

bool Gui::goSelectionBack() {
  if (!canGoSelectionBack()) return false;
  --selectionHistoryIndex_;
  applySelection(selectionHistory_[static_cast<size_t>(selectionHistoryIndex_)], -1,
                 /*recordHistory=*/false);
  setSelectionListSingle(selPath_, selMeshIndex_);
  return true;
}

bool Gui::goSelectionForward() {
  if (!canGoSelectionForward()) return false;
  ++selectionHistoryIndex_;
  applySelection(selectionHistory_[static_cast<size_t>(selectionHistoryIndex_)], -1,
                 /*recordHistory=*/false);
  setSelectionListSingle(selPath_, selMeshIndex_);
  return true;
}

void Gui::saveCameraBookmark(int slot) {
  if (!cam_ || slot < 0 || slot >= static_cast<int>(cameraBookmarks_.size())) return;
  CameraBookmark& bm = cameraBookmarks_[static_cast<size_t>(slot)];
  bm.valid = true;
  bm.target = cam_->target();
  bm.yaw = cam_->yaw();
  bm.pitch = cam_->pitch();
  bm.distance = cam_->distance();
  bm.selectedPath = selPath_;
}

bool Gui::loadCameraBookmark(int slot) {
  if (!cam_ || slot < 0 || slot >= static_cast<int>(cameraBookmarks_.size())) return false;
  const CameraBookmark& bm = cameraBookmarks_[static_cast<size_t>(slot)];
  if (!bm.valid) return false;
  cam_->setOrbit(bm.target, bm.yaw, bm.pitch, bm.distance);
  if (!bm.selectedPath.empty()) {
    selectByPath(bm.selectedPath, -1);
  }
  return true;
}

bool Gui::hasCameraBookmark(int slot) const {
  return slot >= 0 && slot < static_cast<int>(cameraBookmarks_.size()) &&
         cameraBookmarks_[static_cast<size_t>(slot)].valid;
}

void Gui::selectAdjacentMesh(int step) {
  if (!draw_ || draw_->meshes.empty() || step == 0) return;

  const int count = static_cast<int>(draw_->meshes.size());
  int start = selMeshIndex_;
  if (start < 0 || start >= count) start = (step > 0) ? -1 : count;

  for (int i = 0; i < count; ++i) {
    int idx = start + ((i + 1) * step);
    while (idx < 0) idx += count;
    while (idx >= count) idx -= count;
    if (!meshVisibleForView(static_cast<size_t>(idx))) {
      continue;
    }
    selectByPath(draw_->meshes[static_cast<size_t>(idx)].absPath, idx);
    return;
  }
}

void Gui::drawSelectionBreadcrumbs(const char* idSuffix) {
  if (selPath_.empty()) return;

  const auto crumbs = BuildBreadcrumbs(selPath_);
  if (crumbs.empty()) return;

  const float height = ImGui::GetTextLineHeightWithSpacing() * 1.8f;
  if (ImGui::BeginChild(idSuffix, ImVec2(0, height), false,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    for (size_t i = 0; i < crumbs.size(); ++i) {
      if (i > 0) {
        ImGui::SameLine(0, 4.0f);
        ImGui::TextDisabled(">");
        ImGui::SameLine(0, 4.0f);
      }

      const bool selected = crumbs[i].second == selPath_;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
      }
      std::string label = crumbs[i].first + "##" + crumbs[i].second;
      if (ImGui::SmallButton(label.c_str())) {
        selectByPath(crumbs[i].second, -1);
      }
      if (selected) ImGui::PopStyleColor(2);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", crumbs[i].second.c_str());
    }
  }
  ImGui::EndChild();
}

bool Gui::framePath(const std::string& absPath) {
  if (!cam_ || !draw_ || absPath.empty()) return false;

  bool found = false;
  float mn[3] = {0.0f, 0.0f, 0.0f};
  float mx[3] = {0.0f, 0.0f, 0.0f};
  for (const auto& mesh : draw_->meshes) {
    if (!PathIsSameOrDescendant(mesh.absPath, absPath)) continue;
    if (!found) {
      for (int i = 0; i < 3; ++i) {
        mn[i] = mesh.aabbMin[i];
        mx[i] = mesh.aabbMax[i];
      }
      found = true;
    } else {
      for (int i = 0; i < 3; ++i) {
        mn[i] = std::min(mn[i], mesh.aabbMin[i]);
        mx[i] = std::max(mx[i], mesh.aabbMax[i]);
      }
    }
  }

  if (found) cam_->fitToScene(mn, mx);
  return found;
}

bool Gui::drawPrimTree(const lightusd::Prim& prim) {
  // When filtering, hide whole subtrees that contain no match; expand the rest
  // so matches are visible in context.
  const bool filtering = hierFilter_.IsActive();
  if (filtering && !PrimSubtreePasses(hierFilter_, prim)) return false;

  ImGui::PushID(static_cast<const void*>(&prim));
  std::string typeName = prim.prim_type_name();
  if (typeName.empty()) typeName = prim.type_name();
  std::string label = prim.element_name();
  if (label.empty()) label = "<root>";
  if (!typeName.empty()) label += "  [" + typeName + "]";

  std::string subd = SubdivisionSchemeName(prim);
  std::string vis = VisibilityState(prim);
  std::string badges;
  if (!subd.empty() && subd != "none") badges += " [" + subd + "]";
  if (vis == "invisible") badges += " [invisible]";
  // Purpose badge
  {
    auto tryPurpose = [&](auto* gprim) -> std::string {
      if (gprim->purpose.authored()) {
        auto v = gprim->purpose.get_value();
        if (v != lightusd::Purpose::Default) return lightusd::to_string(v);
      }
      return {};
    };
    std::string purpose;
    if (auto* m = prim.as<lightusd::GeomMesh>()) purpose = tryPurpose(m);
    else if (auto* s = prim.as<lightusd::GeomSphere>()) purpose = tryPurpose(s);
    else if (auto* c = prim.as<lightusd::GeomCube>()) purpose = tryPurpose(c);
    else if (auto* y = prim.as<lightusd::GeomCylinder>()) purpose = tryPurpose(y);
    else if (auto* o = prim.as<lightusd::GeomCone>()) purpose = tryPurpose(o);
    else if (auto* p = prim.as<lightusd::GeomCapsule>()) purpose = tryPurpose(p);
    else if (auto* n = prim.as<lightusd::GeomPlane>()) purpose = tryPurpose(n);
    else if (auto* b = prim.as<lightusd::GeomBasisCurves>()) purpose = tryPurpose(b);
    else if (auto* pt = prim.as<lightusd::GeomPoints>()) purpose = tryPurpose(pt);
    if (!purpose.empty()) badges += " [purpose:" + purpose + "]";
  }
  // Material badge
  const std::string absPath = prim.absolute_path().full_path_name();
  if (loaded_ && loaded_->ok &&
      (prim.is<lightusd::GeomMesh>() || prim.is<lightusd::GeomBasisCurves>() ||
       prim.is<lightusd::GeomPoints>())) {
    lightusd::Path matPath;
    const lightusd::Material* matPtr = nullptr;
    std::string matErr;
    if (lightusd::tydra::GetDirectlyBoundMaterial(
            loaded_->stage, prim, "", &matPath, &matPtr, &matErr)) {
      badges += " [mat:" + std::string(matPath.prim_part().c_str()) + "]";
    }
  }
  // Instance / composition arc badges
  if (!prim.IsActive()) badges += " [inactive]";
  if (prim.specifier() == lightusd::Specifier::Over) badges += " [over]";
  else if (prim.specifier() == lightusd::Specifier::Class) badges += " [class]";
  if (prim.IsInstance()) badges += " [instance]";
  if (prim.metas().get_instanceable()) badges += " [instanceable]";
  {
    const auto& m = prim.metas();
    if (m.has_kind()) badges += " [" + lightusd::to_string(m.get_kind()) + "]";
    if (m.references.has_value() && !m.references->empty()) {
      size_t count = 0;
      for (const auto& [qual, refs] : *m.references) count += refs.size();
      if (count > 0) badges += " [ref:" + std::to_string(count) + "]";
    }
    if (m.payload.has_value()) {
      bool isDeferred = false;
      if (loaded_) {
        for (const auto& da : loaded_->comp.deferred) {
          if (da.primPath == absPath) { isDeferred = true; break; }
        }
      }
      badges += isDeferred ? " [payload:defer]" : " [payload:loaded]";
    }
    if (m.inherits.has_value() && !m.inherits->empty()) badges += " [inherits]";
  }
  // Visibility indicator
  {
    bool anyHidden = false, anyVisible = false;
    if (draw_) {
      for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
        if (draw_->meshes[mi].absPath == absPath) {
          if (mi < meshVisible_.size() && !meshVisible_[mi]) anyHidden = true;
          else anyVisible = true;
        }
      }
    }
    if (anyHidden && !anyVisible) badges += " [hidden]";
    else if (anyHidden) badges += " [mixed]";
  }
  label += badges;

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth;
  const bool leaf = prim.children().empty();
  if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
  if (selPrim_ == &prim) flags |= ImGuiTreeNodeFlags_Selected;

  if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  else if (revealSelectionInHierarchy_ && !selPath_.empty() &&
           PathIsSameOrDescendant(selPath_, absPath)) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  }
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", absPath.c_str());
  }
  if (selPrim_ == &prim && revealSelectionInHierarchy_) {
    ImGui::SetScrollHereY(0.35f);
    revealSelectionInHierarchy_ = false;
  }
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    selectByPath(absPath, -1);
    selPrim_ = &prim;
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (!framePath(absPath)) frameSelected();
    }
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(absPath.c_str());
    if (ImGui::MenuItem("Frame selection")) { if (!framePath(absPath)) frameSelected(); }
    // Hide/show
    bool hasDeferred = false;
    if (loaded_) {
      for (const auto& da : loaded_->comp.deferred) {
        if (da.primPath == absPath) { hasDeferred = true; break; }
      }
    }
    if (hasDeferred && ImGui::MenuItem("Load payload")) payloadLoadRequests_.push_back(absPath);
    ImGui::Separator();
    std::vector<size_t> primMeshIndices;
    if (draw_) {
      for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
        if (draw_->meshes[mi].absPath == absPath) primMeshIndices.push_back(mi);
      }
    }
    if (!primMeshIndices.empty()) {
      bool anyHidden = false;
      for (size_t mi : primMeshIndices) {
        if (mi < meshVisible_.size() && !meshVisible_[mi]) { anyHidden = true; break; }
      }
      if (anyHidden) {
        if (ImGui::MenuItem("Show")) {
          for (size_t mi : primMeshIndices) if (mi < meshVisible_.size()) meshVisible_[mi] = 1;
        }
      } else {
        if (ImGui::MenuItem("Hide", "Ctrl+H")) {
          for (size_t mi : primMeshIndices) if (mi < meshVisible_.size()) meshVisible_[mi] = 0;
        }
      }
    }
    ImGui::EndPopup();
  }
  if (open) {
    for (const auto& c : prim.children()) drawPrimTree(c);
    ImGui::TreePop();
  }
  ImGui::PopID();
  return true;
}

bool Gui::drawNodeTree(const tydra::Node& node) {
  const bool filtering = hierFilter_.IsActive();
  if (filtering && !NodeSubtreePasses(hierFilter_, node)) return false;

  ImGui::PushID(static_cast<const void*>(&node));
  std::string label = node.prim_name.empty() ? "<node>" : node.prim_name;
  label += "  {";
  label += NodeTypeName(node.nodeType);
  label += "}";

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth;
  const bool leaf = node.children.empty();
  if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
  if (!selPath_.empty() && selPath_ == node.abs_path) flags |= ImGuiTreeNodeFlags_Selected;

  if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  else if (revealSelectionInHierarchy_ && !selPath_.empty() &&
           PathIsSameOrDescendant(selPath_, node.abs_path)) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  }
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (!selPath_.empty() && selPath_ == node.abs_path && revealSelectionInHierarchy_) {
    ImGui::SetScrollHereY(0.35f);
    revealSelectionInHierarchy_ = false;
  }
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    const int meshIdx = (node.nodeType == tydra::NodeType::Mesh) ? node.id : -1;
    selectByPath(node.abs_path, meshIdx);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (!framePath(node.abs_path)) frameSelected();
    }
  }
  if (open) {
    for (const auto& c : node.children) drawNodeTree(c);
    ImGui::TreePop();
  }
  ImGui::PopID();
  return true;
}

void Gui::rebuildInspectorCache() {
  if (!selPrim_) return;
  if (inspectorCachePrim_ == selPrim_ && inspectorCachePath_ == selPath_) return;

  inspectorCachePrim_ = selPrim_;
  inspectorCachePath_ = selPath_;
  inspectorCacheRows_.clear();
  inspectorCacheError_.clear();
  inspectorCacheType_ = selPrim_->prim_type_name();
  if (inspectorCacheType_.empty()) inspectorCacheType_ = selPrim_->type_name();
  inspectorCacheMeta_ = PrimMetaSummary(*selPrim_);

  std::vector<std::string> names;
  std::string err;
  if (!tydra::GetPropertyNames(*selPrim_, &names, &err)) {
    inspectorCacheError_ = err.empty() ? "No properties." : err;
    return;
  }

  inspectorCacheRows_.reserve(names.size());
  for (const std::string& name : names) {
    InspectorPropRow row;
    row.name = name;
    lightusd::Property prop;
    std::string perr;
    row.gotProperty = tydra::GetProperty(*selPrim_, name, &prop, &perr);
    if (row.gotProperty) {
      row.value = PropertyToString(prop);
      if (prop.is_attribute()) {
        row.typeStr = prop.value_type_name();
        const auto& a = prop.get_attribute();
        if (a.is_connection()) row.typeStr += " (connect)";
        else if (a.has_value() || a.has_timesamples()) row.typeStr += " (authored)";
        else row.typeStr += " (default)";
        row.attrMeta = AttrMetaSummary(a);
        // Color swatch detection
        const std::string& tn = prop.value_type_name();
        if (a.has_value() && (tn == "color3f" || tn == "color4f" || tn == "color3d")) {
          const auto& v = a.get_var().value_raw();
          if (tn == "color3f") {
            if (const auto* c = v.as<lightusd::value::color3f>()) {
              row.hasColor = true;
              row.color[0] = c->r; row.color[1] = c->g;
              row.color[2] = c->b; row.color[3] = 1.0f;
            }
          } else if (tn == "color4f") {
            if (const auto* c = v.as<lightusd::value::color4f>()) {
              row.hasColor = true;
              row.color[0] = c->r; row.color[1] = c->g;
              row.color[2] = c->b; row.color[3] = c->a;
            }
          } else if (tn == "color3d") {
            if (const auto* c = v.as<lightusd::value::color3d>()) {
              row.hasColor = true;
              row.color[0] = static_cast<float>(c->r); row.color[1] = static_cast<float>(c->g);
              row.color[2] = static_cast<float>(c->b); row.color[3] = 1.0f;
            }
          }
        }
      } else if (prop.is_relationship()) {
        row.typeStr = "rel";
      }
    } else {
      row.value = perr.empty() ? "<error>" : perr;
    }
    inspectorCacheRows_.push_back(std::move(row));
  }
}

bool Gui::drawNextPrimTree(lightusd_prim prim) {
  if (!lightusd_prim_is_valid(prim)) return false;
  const std::string path = PublicString(lightusd_prim_path(prim));
  const std::string label = PublicString(lightusd_prim_name(prim)) + "  " + PublicString(lightusd_prim_type_name(prim));
  const bool matches = hierFilter_.PassFilter(label.c_str()) ||
                       hierFilter_.PassFilter(path.c_str());
  bool descendant_matches = false;
  for (size_t i = 0; i < lightusd_prim_child_count(prim) && !descendant_matches; ++i) {
    const lightusd_prim child = lightusd_prim_child(prim, i);
    const std::string child_label =
        PublicString(lightusd_prim_name(child)) + "  " + PublicString(lightusd_prim_type_name(child)) + "  " +
        PublicString(lightusd_prim_path(child));
    descendant_matches = hierFilter_.PassFilter(child_label.c_str());
  }
  if (!matches && !descendant_matches && hierFilter_.IsActive()) return false;

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                             ImGuiTreeNodeFlags_OpenOnArrow;
  if (lightusd_prim_child_count(prim) == 0) flags |= ImGuiTreeNodeFlags_Leaf;
  if (path == selPath_) flags |= ImGuiTreeNodeFlags_Selected;
  if (revealSelectionInHierarchy_ && !selPath_.empty() &&
      selPath_.compare(0, path.size(), path) == 0) {
    ImGui::SetNextItemOpen(true);
  }
  ImGui::PushID(path.c_str());
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    selectByPath(path, -1);
  }
  if (!lightusd_prim_is_active(prim)) {
    ImGui::SameLine();
    ImGui::TextDisabled("inactive");
  } else if (lightusd_prim_has_payload(prim)) {
    ImGui::SameLine();
    ImGui::TextDisabled("payload");
  }
  if (ImGui::BeginPopupContextItem("next_prim_context")) {
    if (lightusd_prim_has_payload(prim) && ImGui::MenuItem("Load payload")) {
      payloadLoadRequests_.push_back(path);
    }
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path.c_str());
    ImGui::EndPopup();
  }
  if (open) {
    for (size_t i = 0; i < lightusd_prim_child_count(prim); ++i) {
      drawNextPrimTree(lightusd_prim_child(prim, i));
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
  return matches || descendant_matches;
}

void Gui::drawNextInspector() {
  if (!nextStage_ || selPath_.empty()) {
    HintWrapped("Select a prim to inspect it.");
    return;
  }
  const lightusd_prim publicPrim = lightusd_stage_prim_at_path(nextStage_.get(), selPath_.c_str());
  if (!lightusd_prim_is_valid(publicPrim)) {
    HintWrapped("The selected prim is not present in the composed stage.");
    return;
  }

  drawSelectionBreadcrumbs("##next-inspector-breadcrumbs");
  ImGui::TextWrapped("%s", selPath_.c_str());
  if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(selPath_.c_str());
  ImGui::TextDisabled("Type: %s", PublicString(lightusd_prim_type_name(publicPrim)).c_str());
  ImGui::Text("Specifier: %s", lightusd_prim_specifier(publicPrim) == 0
                                   ? "def"
                                   : lightusd_prim_specifier(publicPrim) == 1
                                         ? "over"
                                         : "class");
  ImGui::Text("Active: %s", lightusd_prim_is_active(publicPrim) ? "true" : "false");

  // StandardShaderBall authors its example-material choice on the scene root,
  // while the selected material_surface inherits that binding through the
  // shader_ball Xform. PCP's composed snapshot intentionally flattens variant
  // definitions, so the ordinary per-prim Variant sets section below cannot
  // discover this control. Build the material choices from the composed
  // example Material scope instead and author the root override explicitly.
  const std::string shaderBallSuffix = "/shader_ball/material_surface";
  const size_t shaderBallPos = selPath_.find(shaderBallSuffix);
  if (shaderBallPos != std::string::npos &&
      shaderBallPos + shaderBallSuffix.size() == selPath_.size()) {
    const std::string sceneRoot = selPath_.substr(0, shaderBallPos);
    const lightusd_prim materialScope = lightusd_stage_prim_at_path(nextStage_.get(),
        (sceneRoot + "/materials/examples/mtlx").c_str());
    std::vector<std::string> choices;
    if (lightusd_prim_is_valid(materialScope)) {
      for (size_t i = 0; i < lightusd_prim_child_count(materialScope); ++i) {
        const lightusd_prim child = lightusd_prim_child(materialScope, i);
        if (PublicString(lightusd_prim_type_name(child)) == "Material")
          choices.push_back(PublicString(lightusd_prim_name(child)));
      }
      std::sort(choices.begin(), choices.end());
    }
    if (!choices.empty() &&
        ImGui::CollapsingHeader("Inherited material variant",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      std::string selected = "none";
      if (loadOptions_) {
        const auto primIt = loadOptions_->variantOverrides.find(sceneRoot);
        if (primIt != loadOptions_->variantOverrides.end()) {
          const auto setIt = primIt->second.find("example_material");
          if (setIt != primIt->second.end()) {
            constexpr const char* kPrefix = "mtlx_";
            selected = setIt->second.rfind(kPrefix, 0) == 0
                           ? setIt->second.substr(std::strlen(kPrefix))
                           : setIt->second;
          }
        }
      }
      if (ImGui::BeginCombo("OpenPBR example##inherited_material_variant",
                            selected.c_str())) {
        const bool noneSelected = selected == "none";
        if (ImGui::Selectable("none", noneSelected) && !noneSelected) {
          requestVariantSwitch(sceneRoot,
                               {{"example_material", "none"}});
        }
        if (noneSelected) ImGui::SetItemDefaultFocus();
        for (const std::string& choice : choices) {
          const bool current = selected == choice;
          if (ImGui::Selectable(choice.c_str(), current) && !current) {
            requestVariantSwitch(
                sceneRoot, {{"example_material", "mtlx_" + choice}});
          }
          if (current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("Authored on %s; inherited by this surface.",
                          sceneRoot.c_str());
    }
  }

  const size_t setCount = lightusd_prim_variant_set_count(publicPrim);
  if (setCount && ImGui::CollapsingHeader("Variant sets", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (size_t i = 0; i < setCount; ++i) {
      const std::string set = PublicString(lightusd_prim_variant_set_name(publicPrim, i));
      std::string selected = PublicString(lightusd_variant_selection(publicPrim, set.c_str()));
      if (selected.empty()) selected = "(default)";
      const std::string id = set + "##next_variant";
      if (ImGui::BeginCombo(id.c_str(), selected.c_str())) {
        for (size_t j = 0; j < lightusd_variant_count(publicPrim, set.c_str()); ++j) {
          const std::string variant = PublicString(lightusd_variant_name(publicPrim, set.c_str(), j));
          const bool current = variant == selected;
          if (ImGui::Selectable(variant.c_str(), current) && !current) {
            lightusd::api::StringList authored;
            if (lightusd_prim_variant_selection_names(publicPrim, authored.put()) == LIGHTUSD_OK) {
              std::map<std::string, std::string> selections;
              for (size_t k = 0; k < lightusd::api::StringListSize(authored); ++k) {
                const std::string key = PublicString(lightusd::api::StringListGet(authored, k));
                selections[key] = PublicString(lightusd_variant_selection(publicPrim, key.c_str()));
              }
              selections[set] = variant;
              requestVariantSwitch(selPath_, selections);
            }
          }
          if (current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    }
  }

  const size_t references = lightusd_prim_arc_count(publicPrim, LIGHTUSD_ARC_REFERENCE);
  const size_t payloads = lightusd_prim_arc_count(publicPrim, LIGHTUSD_ARC_PAYLOAD);
  const size_t inherits = lightusd_prim_arc_count(publicPrim, LIGHTUSD_ARC_INHERIT);
  const size_t specializes = lightusd_prim_arc_count(publicPrim, LIGHTUSD_ARC_SPECIALIZE);
  if ((references || payloads || inherits || specializes) &&
      ImGui::CollapsingHeader("Composition arcs", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("References: %zu", references);
    ImGui::Text("Payloads: %zu", payloads);
    ImGui::Text("Inherits: %zu", inherits);
    ImGui::Text("Specializes: %zu", specializes);
    if (payloads && ImGui::SmallButton("Load payload")) payloadLoadRequests_.push_back(selPath_);
  }

  const std::string material = PublicString(lightusd_rel_target(publicPrim, "material:binding", 0));
  if (!material.empty() &&
      ImGui::CollapsingHeader("Material binding",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::SmallButton(material.c_str())) selectByPath(material, -1);
  }

  if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginTable("##next_properties", 3,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable)) {
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("Type");
      ImGui::TableSetupColumn("Value");
      ImGui::TableHeadersRow();
      lightusd::api::StringList names;
      if (lightusd_prim_property_names(publicPrim, names.put()) == LIGHTUSD_OK) {
        for (size_t i = 0; i < lightusd::api::StringListSize(names); ++i) {
          const std::string name = PublicString(lightusd::api::StringListGet(names, i));
          lightusd_value_view value{};
          lightusd_sv text{};
          const bool present = lightusd_attr_inspect_default(publicPrim, name.c_str(), &value, &text) == LIGHTUSD_OK;
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(name.c_str());
          ImGui::TableNextColumn();
          const char* type = present ? lightusd_type_name(value.type) : "relationship";
          ImGui::TextUnformatted(type ? type : "value");
          ImGui::TableNextColumn();
          const std::string summary = present ? PublicDefaultSummary(value, text) : "";
          ImGui::TextUnformatted(summary.c_str());
        }
      }
      lightusd::api::StringList relationships;
      if (lightusd_prim_relationship_names(publicPrim, relationships.put()) == LIGHTUSD_OK) {
        for (size_t i = 0; i < lightusd::api::StringListSize(relationships); ++i) {
          const std::string name = PublicString(lightusd::api::StringListGet(relationships, i));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted("relationship");
          ImGui::TableNextColumn();
          ImGui::Text("%zu target(s)", lightusd_rel_target_count(publicPrim, name.c_str()));
        }
      }
      ImGui::EndTable();
    }
  }
}

void Gui::drawHierarchy() {
  ImGui::Begin("Hierarchy");
  if (loaded_ && loaded_->ok) {
    if (!selPath_.empty()) {
      ImGui::TextDisabled("Selected");
      drawSelectionBreadcrumbs("##hierarchy-breadcrumbs");
      ImGui::TextWrapped("%s", selPath_.c_str());
      if (canGoSelectionBack()) {
        if (ImGui::Button("Back")) goSelectionBack();
      } else {
        ImGui::BeginDisabled();
        ImGui::Button("Back");
        ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (canGoSelectionForward()) {
        if (ImGui::Button("Forward")) goSelectionForward();
      } else {
        ImGui::BeginDisabled();
        ImGui::Button("Forward");
        ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (draw_ && !draw_->meshes.empty()) {
        if (ImGui::Button("Prev")) selectAdjacentMesh(-1);
        ImGui::SameLine();
        if (ImGui::Button("Next")) selectAdjacentMesh(1);
        ImGui::SameLine();
      }
      if (ImGui::Button("Frame selection")) {
        if (!framePath(selPath_)) frameSelected();
      }
      ImGui::SameLine();
      if (ImGui::Button("Reveal")) {
        revealSelectionInHierarchy_ = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear")) {
        clearSelection();
      }
      ImGui::Separator();
    }
    hierFilter_.Draw("Search", -1.0f);  // name / type / path
    ImGui::Separator();
    if (showRenderNodes_) {
      for (const auto& n : loaded_->render.nodes) drawNodeTree(n);
    } else if (nextStage_) {
      for (size_t i = 0; i < lightusd_stage_root_prim_count(nextStage_.get()); ++i) {
        drawNextPrimTree(lightusd_stage_root_prim(nextStage_.get(), i));
      }
    } else {
      for (const auto& root : loaded_->stage.root_prims()) drawPrimTree(root);
    }
  } else {
    HintWrapped("No scene loaded.");
    HintWrapped("File > Open... to load a USD file.");
  }
  ImGui::End();
}

void Gui::drawInspector() {
  refineSelectedTextures_ = false;
  releaseSelectedTextures_ = false;
  ImGui::Begin("Inspector");
  if (nextStage_) {
    drawNextInspector();
  } else if (selPrim_) {
    rebuildInspectorCache();
    drawSelectionBreadcrumbs("##inspector-breadcrumbs");
    ImGui::TextWrapped("%s", selPath_.c_str());
    if (ImGui::SmallButton("Copy path")) {
      ImGui::SetClipboardText(selPath_.c_str());
    }
    ImGui::SameLine();
    if (canGoSelectionBack()) {
      if (ImGui::SmallButton("Back")) goSelectionBack();
    } else {
      ImGui::BeginDisabled();
      ImGui::SmallButton("Back");
      ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (canGoSelectionForward()) {
      if (ImGui::SmallButton("Forward")) goSelectionForward();
    } else {
      ImGui::BeginDisabled();
      ImGui::SmallButton("Forward");
      ImGui::EndDisabled();
    }
    ImGui::TextDisabled("Type: %s", inspectorCacheType_.c_str());

    // Blendshape editor (only renders when the selection has blendshape targets).
    drawBlendShapeEditor();

    // Prim metadata (kind/active/hidden/displayName/doc/...).
    if (!inspectorCacheMeta_.empty() &&
        ImGui::CollapsingHeader("Prim metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
      HintWrapped(inspectorCacheMeta_.c_str());
    }

    // GPrim properties (doubleSided, orientation, visibility, purpose)
    {
      std::string gprimSummary = GPrimPropertySummary(*selPrim_);
      if (!gprimSummary.empty() &&
          ImGui::CollapsingHeader("GPrim properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        HintWrapped(gprimSummary.c_str());
      }
    }

    // Variant sets with combo boxes
    {
      const auto& vsMap = selPrim_->variantSets();
      if (!vsMap.empty() &&
          ImGui::CollapsingHeader("Variant sets", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& metas = selPrim_->metas();
        const auto* curSel = metas.variants.has_value() ? &(*metas.variants) : nullptr;
        for (const auto& [setName, vs] : vsMap) {
          if (vs.variantSet.empty()) continue;
          std::string preview = "(default)";
          if (curSel) {
            auto it = curSel->find(setName);
            if (it != curSel->end()) preview = it->second;
          }
          std::string comboLabel = setName + "##variant";
          if (ImGui::BeginCombo(comboLabel.c_str(), preview.c_str())) {
            for (const auto& [vName, var] : vs.variantSet) {
              bool selected = (vName == preview);
              if (ImGui::Selectable(vName.c_str(), selected)) {
                if (!curSel || preview != vName) {
                  std::map<std::string, std::string> overrides;
                  if (curSel) overrides = *curSel;
                  overrides[setName] = vName;
                  requestVariantSwitch(selPath_, overrides);
                }
              }
              if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
        }
      }
    }

    // Composition sources / arcs
    {
      const auto& metas = selPrim_->metas();
      bool hasArcs = false;
      std::string compSummary;
      auto addArc = [&](const char* k, const std::string& v) {
        if (!compSummary.empty()) compSummary += "\n";
        compSummary += k;
        compSummary += ": ";
        compSummary += v;
        hasArcs = true;
      };
      if (metas.references.has_value() && !metas.references->empty()) {
        size_t count = 0;
        for (const auto& [qual, refs] : *metas.references) count += refs.size();
        addArc("references", std::to_string(count) + " arc(s)");
      }
      if (metas.payload.has_value()) {
        addArc("payload", "(authored)");
      }
      if (metas.inherits.has_value() && !metas.inherits->empty()) {
        addArc("inherits", std::to_string(metas.inherits->size()) + " path(s)");
      }
      if (metas.specializes.has_value() && !metas.specializes->empty()) {
        addArc("specializes", std::to_string(metas.specializes->size()) + " path(s)");
      }
      if (hasArcs &&
          ImGui::CollapsingHeader("Composition arcs", ImGuiTreeNodeFlags_DefaultOpen)) {
        HintWrapped(compSummary.c_str());
      }
    }

    // Material binding
    if (loaded_ && loaded_->ok &&
        (selPrim_->is<lightusd::GeomMesh>() || selPrim_->is<lightusd::GeomBasisCurves>() ||
         selPrim_->is<lightusd::GeomPoints>())) {
      lightusd::Path matPath;
      const lightusd::Material* matPtr = nullptr;
      std::string matErr;
      if (lightusd::tydra::GetDirectlyBoundMaterial(
              loaded_->stage, *selPrim_, "", &matPath, &matPtr, &matErr)) {
        const std::string matPathStr = matPath.full_path_name();
        std::string matLabel = "Material: " + matPathStr;
        if (ImGui::CollapsingHeader("Material binding", ImGuiTreeNodeFlags_DefaultOpen)) {
          if (ImGui::SmallButton(matLabel.c_str())) {
            selectByPath(matPathStr, -1);
          }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to navigate");
          (void)matPtr;
        }
      }
    }

    // Shader graph viewer (for Material/Shader prims).
    if (loaded_ && loaded_->ok && selPrim_ && selPrim_->is<lightusd::Material>()) {
      if (ImGui::CollapsingHeader("Shader graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Recursive lambda to trace shader connections.
        std::function<void(const std::string&, int)> traceShader;
        traceShader = [&](const std::string& shaderPath, int depth) {
          if (depth > 4) { ImGui::TextDisabled("  ..."); return; }
          const lightusd::Prim* sprim = nullptr;
          std::function<void(const lightusd::Prim&, const std::string&)> findPrim;
          findPrim = [&](const lightusd::Prim& p, const std::string& target) {
            if (p.absolute_path().full_path_name() == target) { sprim = &p; return; }
            for (const auto& c : p.children()) findPrim(c, target);
          };
          for (const auto& r : loaded_->stage.root_prims()) findPrim(r, shaderPath);
          if (!sprim) { ImGui::TextDisabled("  %s", shaderPath.c_str()); return; }
          ImGui::TextDisabled("  %s  %s", sprim->type_name().c_str(), shaderPath.c_str());
          std::vector<std::string> names;
          std::string perr;
          if (tydra::GetPropertyNames(*sprim, &names, &perr)) {
            for (const auto& n : names) {
              lightusd::Property prop;
              if (tydra::GetProperty(*sprim, n, &prop, &perr) &&
                  prop.is_attribute() && prop.get_attribute().has_connections()) {
                for (const auto& cpath : prop.get_attribute().connections()) {
                  ImGui::TextDisabled("    %s ->", n.c_str());
                  traceShader(cpath.full_path_name(), depth + 1);
                }
              }
            }
          }
        };
        // Trace from Material outputs.
        std::vector<std::string> pnames;
        std::string perr;
        if (tydra::GetPropertyNames(*selPrim_, &pnames, &perr)) {
          for (const auto& n : pnames) {
            lightusd::Property prop;
            if (tydra::GetProperty(*selPrim_, n, &prop, &perr) &&
                prop.is_attribute() && prop.get_attribute().has_connections()) {
              for (const auto& cp : prop.get_attribute().connections()) {
                ImGui::TextDisabled("outputs:%s", n.c_str());
                ImGui::Indent();
                traceShader(cp.full_path_name(), 0);
                ImGui::Unindent();
              }
            }
          }
        }
      }
    }

    ImGui::Separator();
    propFilter_.Draw("Search##props", -1.0f);  // property name / value / type

    if (inspectorCacheError_.empty()) {
      if (ImGui::BeginTable("##props", 3,
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 8.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const InspectorPropRow& row : inspectorCacheRows_) {
          // Filter rows by property name, type or value.
          if (propFilter_.IsActive() && !propFilter_.PassFilter(row.name.c_str()) &&
              !propFilter_.PassFilter(row.typeStr.c_str()) &&
              !propFilter_.PassFilter(row.value.c_str()) &&
              !propFilter_.PassFilter(row.attrMeta.c_str())) {
            continue;
          }
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(row.name.c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(row.typeStr.c_str());
          ImGui::TableSetColumnIndex(2);
          if (row.gotProperty) {
            // Color swatch before value if applicable
            if (row.hasColor) {
              ImVec4 c(row.color[0], row.color[1], row.color[2], row.color[3]);
              ImGui::ColorButton("##swatch", c,
                                 ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                 ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
              ImGui::SameLine();
            }
            if (ImGui::BeginChild(("##val-" + row.name).c_str(),
                                  ImVec2(0, ImGui::GetTextLineHeightWithSpacing()),
                                  false, ImGuiWindowFlags_NoScrollbar)) {
              ImGui::TextUnformatted(row.value.c_str());
              if (ImGui::IsItemHovered() && !row.value.empty()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                  ImGui::SetClipboardText(row.value.c_str());
                }
              }
            }
            ImGui::EndChild();
            if (!row.attrMeta.empty()) HintWrapped(row.attrMeta.c_str());
          } else {
            ImGui::TextDisabled("<error>");
          }
        }
        ImGui::EndTable();
      }
    } else {
      HintWrapped(inspectorCacheError_.c_str());
    }
  } else if (!selPath_.empty()) {
    ImGui::TextWrapped("%s", selPath_.c_str());
    if (ImGui::SmallButton("Copy path")) {
      ImGui::SetClipboardText(selPath_.c_str());
    }
    HintWrapped("(RenderScene node; no matching Stage prim)");
  } else {
    HintWrapped("Select a prim in the Hierarchy.");
  }
  if (textureResidencyInfo_.total > 0) {
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Texture residency",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const double mib = 1024.0 * 1024.0;
      ImGui::Text("%zu / %zu resident", textureResidencyInfo_.resident,
                  textureResidencyInfo_.total);
      ImGui::Text("%.1f / %.1f MiB, %zu queued",
                  double(textureResidencyInfo_.residentBytes) / mib,
                  double(textureResidencyInfo_.budgetBytes) / mib,
                  textureResidencyInfo_.queued);
      if (ImGui::Button("Refine selected")) refineSelectedTextures_ = true;
      ImGui::SameLine();
      if (ImGui::Button("Release selected")) releaseSelectedTextures_ = true;
      ImGui::Checkbox("Background refinement",
                      &textureResidencyInfo_.backgroundRefinement);
    }
  }
  ImGui::End();
}

void Gui::drawBlendShapeEditor() {
  if (!draw_ || (selPath_.empty() && !virtualHumanProfile_)) return;

  // Gather blendshapes (by name, with their in-between weights) from meshes that
  // are the selection itself, an ancestor of it (selecting a BlendShape child),
  // or a descendant (selecting a SkelRoot/Xform above the mesh).
  std::map<std::string, std::vector<float>> shapes;  // name -> inbetween weights
  const std::string selSlash = selPath_ + "/";
  for (const DrawMeshCPU& m : draw_->meshes) {
    if (m.morphTargetChannels.empty()) continue;
    const std::string meshSlash = m.absPath + "/";
    const bool related = virtualHumanProfile_ || (m.absPath == selPath_) ||
                         (m.absPath.rfind(selSlash, 0) == 0) ||
                         (selPath_.rfind(meshSlash, 0) == 0);
    if (!related) continue;
    // morphTargetChannels carries name + ascending usdWeights = [inbetween
    // weights..., 1.0]; the editor wants the in-between weights (drop the
    // trailing 1.0 primary). The heavy per-vertex morphs are freed after load.
    for (const MorphTargetChannelsCPU& tc : m.morphTargetChannels) {
      std::vector<float>& ib = shapes[tc.name];
      if (ib.empty() && tc.usdWeights.size() > 1) {
        for (size_t k = 0; k + 1 < tc.usdWeights.size(); ++k) {
          ib.push_back(tc.usdWeights[k]);
        }
      }
    }
  }
  if (shapes.empty()) return;

  if (!ImGui::CollapsingHeader("Blend Shapes", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  if (ImGui::Checkbox("Manual weights", &blendActive_)) blendDirty_ = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset")) {
    for (auto& kv : blendWeights_) kv.second = 0.0f;
    blendDirty_ = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu shape%s", shapes.size(), shapes.size() == 1 ? "" : "s");
  if (!blendActive_) {
    ImGui::TextDisabled("Enable 'Manual weights' to drive these (overrides anim).");
  }

  ImGui::BeginDisabled(!blendActive_);
  for (auto& kv : shapes) {
    const std::string& name = kv.first;
    float& w = blendWeights_[name];  // default 0 (rest)
    ImGui::PushID(name.c_str());
    // 0..1 slider; ctrl+click to type an overdrive value (the morph eval
    // extrapolates beyond the end shapes, Maya-style).
    if (ImGui::SliderFloat(name.c_str(), &w, 0.0f, 1.0f, "%.3f")) {
      blendDirty_ = true;
    }
    // In-between tick marks on the slider track (amber), so authored intermediate
    // shapes are visible like Maya's target markers.
    if (!kv.second.empty()) {
      const ImVec2 mn = ImGui::GetItemRectMin();
      const ImVec2 mx = ImGui::GetItemRectMax();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (float iw : kv.second) {
        const float c = iw < 0.0f ? 0.0f : (iw > 1.0f ? 1.0f : iw);
        const float x = mn.x + (mx.x - mn.x) * c;
        dl->AddLine(ImVec2(x, mn.y), ImVec2(x, mn.y + 3.0f),
                    IM_COL32(255, 200, 60, 255), 1.5f);
        dl->AddLine(ImVec2(x, mx.y - 3.0f), ImVec2(x, mx.y),
                    IM_COL32(255, 200, 60, 255), 1.5f);
      }
    }
    ImGui::PopID();
  }
  ImGui::EndDisabled();
}

void Gui::drawVirtualHumanPanel() {
  ImGui::Begin("Facial Rig");
  ImGui::TextDisabled("UsdSkel + authored vchar controls");
  ImGui::BeginDisabled(!autoriggerConfigured_ || autoriggerRunning_ || !draw_);
  if (ImGui::Button(autoriggerRunning_ ? "Auto-rigging..." : "Auto-rig facial"))
    autoriggerRequested_ = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!autoriggerConfigured_) ImGui::TextDisabled("start with --autorigger PATH");
  else if (!autoriggerStatus_.empty()) ImGui::TextWrapped("%s", autoriggerStatus_.c_str());
  if (autoriggerRunning_) ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0));
  ImGui::SeparatorText("MetaHuman DNA");
  ImGui::InputText("DNA file", dnaPath_.data(), dnaPath_.size());
  ImGui::SameLine();
  ImGui::BeginDisabled(!autoriggerConfigured_ || dnaWorkerRunning_ || dnaPath_[0] == '\0');
  if (ImGui::Button(dnaWorkerRunning_ ? "Inspecting DNA..." : "Inspect DNA"))
    dnaInspectRequested_ = true;
  ImGui::EndDisabled();
  if (!dnaWorkerStatus_.empty()) ImGui::TextWrapped("%s", dnaWorkerStatus_.c_str());
  if (dnaWorkerRunning_) ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0));
  ImGui::Separator();
  if (facialControls_.empty()) {
    drawBlendShapeEditor();
  } else {
    if (ImGui::Checkbox("Manual controls", &blendActive_)) blendDirty_ = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Defaults")) {
      for (const VcharControl& control : facialControls_) {
        blendWeights_[control.blendshape] = control.defaultValue;
      }
      blendDirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu authored", facialControls_.size());
    ImGui::BeginDisabled(!blendActive_);
    for (const VcharControl& control : facialControls_) {
      float& value = blendWeights_[control.blendshape];
      ImGui::PushID(control.name.c_str());
      if (ImGui::SliderFloat(control.name.c_str(), &value, control.minimum,
                             control.maximum, "%.3f")) {
        blendDirty_ = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Blendshape: %s\nDefault: %.3f",
                          control.blendshape.c_str(), control.defaultValue);
      }
      ImGui::PopID();
    }
    ImGui::EndDisabled();
  }
  ImGui::Separator();
  if (!draw_ || draw_->curves.empty()) {
    ImGui::TextDisabled("No BasisCurves hair/groom geometry");
  } else if (ImGui::CollapsingHeader("Hair diagnostics",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
    size_t strands = 0;
    size_t samples = 0;
    size_t widthSamples = 0;
    float minimumWidth = std::numeric_limits<float>::max();
    float maximumWidth = 0.0f;
    for (const DrawCurvesCPU& curves : draw_->curves) {
      strands += curves.vertexCounts.size();
      samples += curves.points.size() / 3u;
      widthSamples += curves.widths.size();
      for (float width : curves.widths) {
        minimumWidth = std::min(minimumWidth, width);
        maximumWidth = std::max(maximumWidth, width);
      }
    }
    ImGui::Text("Curve prims: %zu", draw_->curves.size());
    ImGui::Text("Strands: %zu", strands);
    ImGui::Text("Tessellated samples: %zu", samples);
    if (widthSamples) {
      ImGui::Text("Width samples: %zu (%.6g .. %.6g)", widthSamples,
                  minimumWidth, maximumWidth);
    } else {
      ImGui::TextDisabled("No authored widths; renderer fallback width is active");
    }
    ImGui::Text("Density: %.2f samples/strand",
                strands ? static_cast<double>(samples) /
                              static_cast<double>(strands)
                        : 0.0);
    ImGui::TextDisabled("Raster: camera-facing ribbons; Vulkan RT: tube proxies");
  }
  ImGui::End();
}

void Gui::drawSelectionList() {
  ImGui::Begin("Selection");
  ImGui::Text("Selected: %zu", selectionList_.size());
  if (!selectionList_.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) clearSelection();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy all")) {
      std::string text;
      for (const auto& item : selectionList_) {
        text += item.first;
        text += '\n';
      }
      ImGui::SetClipboardText(text.c_str());
    }
  }
  ImGui::Separator();

  if (selectionList_.empty()) {
    HintWrapped("Drag a region in the viewport or click a prim/mesh.");
    ImGui::End();
    return;
  }

  if (ImGui::BeginChild("##selection-list", ImVec2(0, 0), false,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(selectionList_.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& item = selectionList_[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const bool focused = item.first == selPath_;
        if (ImGui::Selectable(item.first.c_str(), focused,
                              ImGuiSelectableFlags_SpanAvailWidth)) {
          focusSelectionListItem(static_cast<size_t>(i));
        }
        if (ImGui::BeginPopupContextItem("##selection-item-menu")) {
          if (ImGui::MenuItem("Copy path")) {
            ImGui::SetClipboardText(item.first.c_str());
          }
          if (ImGui::MenuItem("Frame")) {
            focusSelectionListItem(static_cast<size_t>(i));
            if (!framePath(item.first)) frameSelected();
          }
          ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.first.c_str());
        ImGui::PopID();
      }
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

void Gui::drawCameraPanel() {
  ImGui::Begin("Camera");
  if (cam_) {
    const light3d::Vec3 eye = cam_->eye();
    const light3d::Vec3 target = cam_->target();
    ImGui::Text("Eye:    %.3f %.3f %.3f", eye.x, eye.y, eye.z);
    ImGui::Text("Target: %.3f %.3f %.3f", target.x, target.y, target.z);
    ImGui::Text("Yaw/Pitch: %.3f / %.3f", cam_->yaw(), cam_->pitch());
    ImGui::Text("Distance: %.3f", cam_->distance());
    ImGui::Text("Aspect: %.5g", cam_->aspect());
    ImGui::Text("Clip: %.5g / %.5g", cam_->nearPlane(), cam_->farPlane());

    ImGui::Separator();
    ImGui::TextDisabled("Navigation sensitivity (N cycles presets)");
    static const float kNavScales[] = {0.25f, 0.5f, 1.0f, 1.5f};
    for (size_t i = 0; i < 4; ++i) {
      if (i) ImGui::SameLine();
      char label[16];
      std::snprintf(label, sizeof(label), "%gx", kNavScales[i]);
      if (ImGui::RadioButton(label,
                             std::fabs(cam_->navigationScale() -
                                       kNavScales[i]) < 1.0e-4f)) {
        cam_->setNavigationScale(kNavScales[i]);
      }
    }
    float navScale = cam_->navigationScale();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputFloat("Scale", &navScale, 0.05f, 0.25f, "%.3gx")) {
      cam_->setNavigationScale(std::max(0.01f, std::min(10.0f, navScale)));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Views");
    if (ImGui::Button("Home")) homeView();
    ImGui::SameLine();
    if (ImGui::Button("Frame selected")) frameSelected();
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) frameAll();
    if (canGoSelectionBack()) {
      if (ImGui::Button("Selection back")) goSelectionBack();
    } else {
      ImGui::BeginDisabled();
      ImGui::Button("Selection back");
      ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (canGoSelectionForward()) {
      if (ImGui::Button("Selection forward")) goSelectionForward();
    } else {
      ImGui::BeginDisabled();
      ImGui::Button("Selection forward");
      ImGui::EndDisabled();
    }

      if (ImGui::Button("Front")) applyViewPreset(CameraViewPreset::Front);
    ImGui::SameLine();
    if (ImGui::Button("Back")) applyViewPreset(CameraViewPreset::Back);
    ImGui::SameLine();
    if (ImGui::Button("Iso")) applyViewPreset(CameraViewPreset::Isometric);

    if (ImGui::Button("Left")) applyViewPreset(CameraViewPreset::Left);
    ImGui::SameLine();
    if (ImGui::Button("Right")) applyViewPreset(CameraViewPreset::Right);
    ImGui::SameLine();
    if (ImGui::Button("Top")) applyViewPreset(CameraViewPreset::Top);
    ImGui::SameLine();
    if (ImGui::Button("Bottom")) applyViewPreset(CameraViewPreset::Bottom);

    ImGui::Separator();
    ImGui::TextDisabled("Projection");
    float fov = cam_->fovYDeg();
    if (ImGui::SliderFloat("FOV Y", &fov, 5.0f, 175.0f, "%.1f deg")) {
      cam_->setFovYDeg(fov);
    }
    bool aspectOverride = cam_->aspectOverrideEnabled();
    if (ImGui::Checkbox("Aspect override", &aspectOverride)) {
      cam_->setAspectOverrideEnabled(aspectOverride);
    }
    float aspectValue = cam_->aspectOverride();
    if (!aspectOverride) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputFloat("Aspect", &aspectValue, 0.0f, 0.0f, "%.5g")) {
      cam_->setAspectOverride(aspectValue);
    }
    if (!aspectOverride) ImGui::EndDisabled();
    int conform = static_cast<int>(cam_->conform());
    const char* conformNames[] = {"Fit", "Crop", "Horizontal", "Vertical", "None"};
    if (ImGui::Combo("Filmback conform", &conform, conformNames, 5)) {
      cam_->setConform(static_cast<CameraConform>(conform));
    }
    bool autoClip = cam_->autoClip();
    if (ImGui::Checkbox("Auto clipping", &autoClip)) {
      cam_->setAutoClip(autoClip);
    }
    float nearClip = cam_->manualNearPlane();
    float farClip = cam_->manualFarPlane();
    if (autoClip) ImGui::BeginDisabled();
    bool clipChanged = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    clipChanged |= ImGui::InputFloat("Near", &nearClip, 0.0f, 0.0f, "%.5g");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    clipChanged |= ImGui::InputFloat("Far", &farClip, 0.0f, 0.0f, "%.5g");
    if (autoClip) ImGui::EndDisabled();
    if (clipChanged) {
      cam_->setClipPlanes(nearClip, farClip);
    }
    if (ImGui::SmallButton("Reset projection")) {
      cam_->setFovYDeg(60.0f);
      cam_->setAspectOverride(1.0f);
      cam_->setAspectOverrideEnabled(false);
      cam_->setClipPlanes(0.01f, 10000.0f);
      cam_->setAutoClip(true);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Depth of field");
    const double metersPerUnit =
        draw_ && draw_->metersPerUnit > 0.0 ? draw_->metersPerUnit : 0.01;
    auto apertureForFStop = [&](float distance, float fStop) {
      return MakeAutoRtCameraLens(std::max(distance, 1.0e-4f), fStop,
                                  metersPerUnit)
          .apertureRadius;
    };
    bool dofEnabled = cameraLens_.enabled();
    if (ImGui::Checkbox("Enable depth of field", &dofEnabled)) {
      if (dofEnabled) {
        if (!(cameraLens_.focusDistance > 0.0f)) {
          cameraLens_.focusDistance =
              std::max(cam_->distance(), cam_->nearPlane() * 2.0f);
        }
        if (!(lastDofApertureRadius_ > 0.0f)) {
          lastDofApertureRadius_ =
              apertureForFStop(cameraLens_.focusDistance, 4.0f);
        }
        cameraLens_.apertureRadius = lastDofApertureRadius_;
      } else {
        if (cameraLens_.apertureRadius > 0.0f) {
          lastDofApertureRadius_ = cameraLens_.apertureRadius;
        }
        cameraLens_.apertureRadius = 0.0f;
      }
    }

    float focusDistance = cameraLens_.focusDistance;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputFloat("Focus distance", &focusDistance, 0.0f, 0.0f,
                          "%.5g") &&
        std::isfinite(focusDistance) && focusDistance >= 0.0f) {
      cameraLens_.focusDistance = focusDistance;
    }

    float equivalentFStop = cameraLens_.apertureRadius > 0.0f
        ? static_cast<float>(0.025 /
                             (metersPerUnit * cameraLens_.apertureRadius))
        : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputFloat("F-stop (50 mm equiv.)", &equivalentFStop, 0.0f,
                          0.0f, "%.4g") &&
        std::isfinite(equivalentFStop) && equivalentFStop >= 0.0f) {
      cameraLens_.apertureRadius = equivalentFStop > 0.0f
          ? apertureForFStop(cameraLens_.focusDistance, equivalentFStop)
          : 0.0f;
      if (cameraLens_.apertureRadius > 0.0f) {
        lastDofApertureRadius_ = cameraLens_.apertureRadius;
      }
    }

    float apertureRadius = cameraLens_.apertureRadius;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputFloat("Aperture radius", &apertureRadius, 0.0f, 0.0f,
                          "%.6g") &&
        std::isfinite(apertureRadius) && apertureRadius >= 0.0f) {
      cameraLens_.apertureRadius = apertureRadius;
      if (apertureRadius > 0.0f) lastDofApertureRadius_ = apertureRadius;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Thin-lens radius in stage units (metersPerUnit %.6g)",
                        metersPerUnit);
    }
    const bool canScaleAperture = cameraLens_.apertureRadius > 0.0f;
    if (!canScaleAperture) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Aperture /2")) {
      cameraLens_.apertureRadius *= 0.5f;
      lastDofApertureRadius_ = cameraLens_.apertureRadius;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Aperture x2")) {
      cameraLens_.apertureRadius *= 2.0f;
      lastDofApertureRadius_ = cameraLens_.apertureRadius;
    }
    if (!canScaleAperture) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset f/4")) {
      cameraLens_.focusDistance =
          std::max(cam_->distance(), cam_->nearPlane() * 2.0f);
      cameraLens_.apertureRadius =
          apertureForFStop(cameraLens_.focusDistance, 4.0f);
      lastDofApertureRadius_ = cameraLens_.apertureRadius;
    }

    light3d::Vec3 selectedPoint;
    bool fromViewportPick = false;
    const bool haveFocusPoint =
        selectedFocusPoint(&selectedPoint, &fromViewportPick);
    const light3d::Vec3 focusEye = cam_->eye();
    const float selectedDistance = haveFocusPoint
        ? RtFocusDistanceToPoint(focusEye, cam_->target() - focusEye,
                                 selectedPoint)
        : 0.0f;
    const bool canFocus = haveFocusPoint && std::isfinite(selectedDistance) &&
                          selectedDistance >
                              std::max(1.0e-4f, cam_->nearPlane() * 1.001f);
    if (!canFocus) ImGui::BeginDisabled();
    if (ImGui::Button("Focus selected")) focusDofOnSelection();
    if (!canFocus) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (canFocus) {
        ImGui::SetTooltip("Set focus to %.5g (%s)", selectedDistance,
                          fromViewportPick ? "picked surface"
                                           : "selection bounds center");
      } else {
        ImGui::SetTooltip("Pick a visible object or select a renderable prim");
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(!haveFocusPoint
                            ? "no focus target"
                            : (fromViewportPick ? "picked surface"
                                                : "selection bounds center"));
    if (!cameraLens_.enabled()) {
      HintWrapped("Depth of field is inactive. Enable it above or enter a "
                  "positive focus distance and aperture/F-stop.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Bookmarks");
    for (int slot = 0; slot < 3; ++slot) {
      const int humanSlot = slot + 1;
      ImGui::PushID(slot);
      ImGui::Text("Slot %d", humanSlot);
      ImGui::SameLine();
      if (ImGui::Button("Save")) saveCameraBookmark(slot);
      ImGui::SameLine();
      const bool canLoad = hasCameraBookmark(slot);
      if (!canLoad) ImGui::BeginDisabled();
      if (ImGui::Button("Load")) loadCameraBookmark(slot);
      if (!canLoad) ImGui::EndDisabled();
      if (canLoad) {
        const auto& bm = cameraBookmarks_[static_cast<size_t>(slot)];
        if (!bm.selectedPath.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", bm.selectedPath.c_str());
        }
      } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(empty)");
      }
      ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Navigation tuning");
    float orbit = cam_->orbitSensitivity();
    if (ImGui::SliderFloat("Orbit sensitivity", &orbit, 0.1f, 4.0f, "%.2f")) {
      cam_->setOrbitSensitivity(orbit);
    }
    float pan = cam_->panSensitivity();
    if (ImGui::SliderFloat("Pan sensitivity", &pan, 0.1f, 4.0f, "%.2f")) {
      cam_->setPanSensitivity(pan);
    }
    float dolly = cam_->dollySensitivity();
    if (ImGui::SliderFloat("Dolly sensitivity", &dolly, 0.1f, 4.0f, "%.2f")) {
      cam_->setDollySensitivity(dolly);
    }
    bool legacyYaw = !cam_->invertYaw();
    if (ImGui::Checkbox("Legacy yaw direction", &legacyYaw)) {
      cam_->setInvertYaw(!legacyYaw);
    }
    bool invert = cam_->invertDolly();
    if (ImGui::Checkbox("Invert dolly", &invert)) {
      cam_->setInvertDolly(invert);
    }
    if (ImGui::Button("Reset tuning")) {
      cam_->setOrbitSensitivity(1.0f);
      cam_->setPanSensitivity(1.0f);
      cam_->setDollySensitivity(1.0f);
      cam_->setInvertYaw(true);
      cam_->setInvertDolly(false);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Persist via config.json if desired.");
  } else {
    HintWrapped("Camera controls appear here once the viewer is initialized.");
  }
  ImGui::End();
}

void Gui::drawDomeLightPanel() {
  if (!showDomeLight_) return;
  if (!ImGui::Begin("Dome Light", &showDomeLight_)) {
    ImGui::End();
    return;
  }

  const char* sources[] = {"Off", "USD authored", "File", "White furnace",
                           "Sun and sky"};
  int source = static_cast<int>(domeRequest_.source);
  if (ImGui::Combo("Source", &source, sources, 5)) {
    domeRequest_.source = static_cast<DomeSource>(source);
    domeDirty_ = true;
  }

  if (domeRequest_.source == DomeSource::Authored) {
    if (authoredDomes_.empty()) {
      ImGui::TextDisabled("This USD has no authored DomeLight.");
    } else {
      domeRequest_.authoredIndex = std::max(
          0, std::min(domeRequest_.authoredIndex,
                      int(authoredDomes_.size()) - 1));
      if (ImGui::BeginCombo(
              "Authored light",
              authoredDomes_[size_t(domeRequest_.authoredIndex)].label.c_str())) {
        for (size_t i = 0; i < authoredDomes_.size(); ++i) {
          if (ImGui::Selectable(authoredDomes_[i].label.c_str(),
                                int(i) == domeRequest_.authoredIndex)) {
            domeRequest_.authoredIndex = int(i);
            domeRequest_.format = authoredDomes_[i].format;
            domeDirty_ = true;
          }
        }
        ImGui::EndCombo();
      }
    }
  }

  if (domeRequest_.source == DomeSource::File) {
    ImGui::TextWrapped("%s", domeRequest_.filePath.empty()
                                  ? "No environment map selected"
                                  : domeRequest_.filePath.c_str());
    if (ImGui::Button("Load environment map...")) {
      domeRequest_.browse = true;
      domeDirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or drop an image on the window");
  }

  const char* mappings[] = {"Automatic", "Latitude-longitude", "Mirrored ball",
                            "Angular probe"};
  int mapping = static_cast<int>(domeRequest_.format);
  const bool mappingRelevant = domeRequest_.source == DomeSource::Authored ||
                               domeRequest_.source == DomeSource::File;
  ImGui::BeginDisabled(!mappingRelevant);
  if (ImGui::Combo("Coordinates", &mapping, mappings, 4)) {
    domeRequest_.format = static_cast<DrawLightCPU::DomeTextureFormat>(mapping);
    domeDirty_ = true;
  }
  ImGui::EndDisabled();
  if (ImGui::DragFloat("Intensity", &domeRequest_.intensity, 0.02f, 0.0f,
                       1000.0f, "%.3f")) {
    domeRequest_.intensity = std::max(0.0f, domeRequest_.intensity);
    domeDirty_ = true;
  }
  if (ImGui::SliderFloat("Rotation", &domeRequest_.rotationDegrees, -180.0f,
                         180.0f, "%.1f deg")) {
    domeDirty_ = true;
  }
  if (!domeStatus_.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", domeStatus_.c_str());
  }
  ImGui::End();
}

void Gui::drawPayloads() {
  if (!ImGui::Begin("Payloads")) {
    ImGui::End();
    return;
  }
  if (nextStage_) {
    // The overwhelmingly common case has no deferred payloads. Do not traverse
    // the entire composed stage merely to decide whether the empty panel says
    // "No deferred payloads" or "All payloads loaded"; on large scenes that
    // O(prims) walk happened every frame and made the viewer appear hung.
    if (deferredPayloadPaths_.empty()) {
      ImGui::TextDisabled("No deferred payloads.");
      ImGui::End();
      return;
    }
    ImGui::Text("Deferred payloads: %zu", deferredPayloadPaths_.size());
    if (ImGui::Button("Load All") && !loadStatus_.active) {
      wantLoadAllPayloads_ = true;
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##next_payloads", 3,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable)) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Prim");
      ImGui::TableSetupColumn("Asset");
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < deferredPayloadPaths_.size(); ++i) {
        const std::string& primPath = deferredPayloadPaths_[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Load") && !loadStatus_.active) {
          payloadLoadRequests_.push_back(primPath);
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(primPath.c_str());
        ImGui::TableNextColumn();
        const lightusd_prim prim = lightusd_stage_prim_at_path(nextStage_.get(), primPath.c_str());
        const std::string asset = lightusd_prim_has_payload(prim)
            ? PublicString(lightusd_prim_arc_text(prim, LIGHTUSD_ARC_PAYLOAD, 0))
            : "(deferred)";
        ImGui::TextUnformatted(asset.c_str());
      }
      ImGui::EndTable();
    }
    ImGui::End();
    return;
  }
  if (!loaded_ || !loaded_->comp.composed) {
    ImGui::TextDisabled(loaded_ && loaded_->ok
                            ? "Scene has no composition arcs."
                            : "No scene loaded.");
    ImGui::End();
    return;
  }
  const auto& comp = loaded_->comp;
  if (comp.deferred.empty()) {
    ImGui::TextDisabled("All payloads loaded (%zu).", comp.loadedPayloads.size());
    ImGui::End();
    return;
  }

  ImGui::Text("Deferred arcs: %zu", comp.deferred.size());
  if (!comp.loadedPayloads.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu loaded)", comp.loadedPayloads.size());
  }
  const bool busy = loadStatus_.active;
  if (ImGui::Button("Load All") && !busy) {
    wantLoadAllPayloads_ = true;
  }
  ImGui::Separator();

  if (ImGui::BeginTable("##payloads", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Arc", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Prim");
    ImGui::TableSetupColumn("Asset");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < comp.deferred.size(); ++i) {
      const DeferredArc& d = comp.deferred[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("Load") && !busy) {
        payloadLoadRequests_.push_back(d.primPath);
      }
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.arc);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.primPath.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.assetPath.empty() ? "(internal)"
                                                 : d.assetPath.c_str());
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

void Gui::drawOpenPbrMaterialPanel() {
  // Existing users already have a saved ImGui layout with no entry for this
  // newly-added window. Adopt the Materials tab's dock on first appearance so
  // the editor never opens as a large viewport-obscuring floating window.
  if (!openPbrDockAttempted_) {
    ImGuiWindow* materialsWindow = ImGui::FindWindowByName("Materials");
    if (materialsWindow && materialsWindow->DockId != 0) {
      ImGui::SetNextWindowDockID(materialsWindow->DockId, ImGuiCond_Always);
      openPbrDockAttempted_ = true;
    }
  }
  ImGui::Begin("OpenPBR Material");
  if (!draw_ || draw_->materials.empty()) {
    ImGui::TextDisabled("No scene materials.");
    ImGui::End();
    return;
  }

  // Follow a newly selected mesh once, while still allowing the material combo
  // to be changed independently afterward.
  if (openPbrEditorSelectionMesh_ != selMeshIndex_) {
    openPbrEditorSelectionMesh_ = selMeshIndex_;
    if (selMeshIndex_ >= 0 &&
        static_cast<size_t>(selMeshIndex_) < draw_->meshes.size()) {
      for (const DrawSubmesh& submesh :
           draw_->meshes[static_cast<size_t>(selMeshIndex_)].submeshes) {
        if (submesh.materialId >= 0 &&
            static_cast<size_t>(submesh.materialId) < draw_->materials.size() &&
            draw_->materials[static_cast<size_t>(submesh.materialId)]
                .hasOpenPBRSurface) {
          openPbrEditorMaterial_ = submesh.materialId;
          break;
        }
      }
    }
  }
  auto validOpenPbr = [&](int id) {
    return id >= 0 && static_cast<size_t>(id) < draw_->materials.size() &&
           draw_->materials[static_cast<size_t>(id)].hasOpenPBRSurface;
  };
  if (!validOpenPbr(openPbrEditorMaterial_)) {
    openPbrEditorMaterial_ = -1;
    for (size_t i = 0; i < draw_->materials.size(); ++i) {
      if (draw_->materials[i].hasOpenPBRSurface) {
        openPbrEditorMaterial_ = static_cast<int>(i);
        break;
      }
    }
  }
  if (openPbrEditorMaterial_ < 0) {
    ImGui::TextDisabled("This scene has no OpenPBRSurface materials.");
    ImGui::End();
    return;
  }

  const DrawMaterialCPU& mat =
      draw_->materials[static_cast<size_t>(openPbrEditorMaterial_)];
  const char* preview = mat.displayName.empty()
                            ? (mat.name.empty() ? "OpenPBR material"
                                                : mat.name.c_str())
                            : mat.displayName.c_str();
  if (ImGui::BeginCombo("Material", preview)) {
    for (size_t i = 0; i < draw_->materials.size(); ++i) {
      const DrawMaterialCPU& candidate = draw_->materials[i];
      if (!candidate.hasOpenPBRSurface) continue;
      std::string label = candidate.displayName.empty()
                              ? (candidate.name.empty()
                                     ? "Material " + std::to_string(i)
                                     : candidate.name)
                              : candidate.displayName;
      label += "##openpbr_" + std::to_string(i);
      const bool selected = static_cast<int>(i) == openPbrEditorMaterial_;
      if (ImGui::Selectable(label.c_str(), selected))
        openPbrEditorMaterial_ = static_cast<int>(i);
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  const DrawMaterialCPU& editedMat =
      draw_->materials[static_cast<size_t>(openPbrEditorMaterial_)];
  DrawLightRtOpenPBRCPU p = editedMat.lightRtOpenPBR;
  bool changed = false;
  bool makeConstant = false;
  auto connected = [&](std::initializer_list<const char*> names, int route,
                       int directTexture) {
    if (directTexture >= 0) return true;
    for (const DrawMaterialParamCPU& param : editedMat.params) {
      if (param.shader != "OpenPBRSurface" ||
          (param.texture < 0 && param.renderTexture < 0)) {
        continue;
      }
      for (const char* name : names)
        if (param.name == name) return true;
    }
    return route >= 0 && route < MaterialXGraphRuntimeCPU::kOutputCount &&
           editedMat.materialXGraph.valid &&
           editedMat.materialXGraph.output[route] >= 0;
  };
  auto disabledHint = [](bool disabled) {
    if (disabled &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Connected input; edit the MaterialX graph or texture source.");
    }
  };
  auto slider = [&](const char* label, float* value, float lo, float hi,
                    bool disabled) {
    ImGui::BeginDisabled(disabled);
    const bool edited = ImGui::SliderFloat(label, value, lo, hi, "%.3f");
    ImGui::EndDisabled();
    disabledHint(disabled);
    changed |= edited;
  };
  auto drag = [&](const char* label, float* value, float speed, float lo,
                  float hi, bool disabled) {
    ImGui::BeginDisabled(disabled);
    const bool edited = ImGui::DragFloat(label, value, speed, lo, hi, "%.3f");
    ImGui::EndDisabled();
    disabledHint(disabled);
    changed |= edited;
  };
  auto color = [&](const char* label, float value[3], bool disabled) {
    ImGui::BeginDisabled(disabled);
    const bool edited = ImGui::ColorEdit3(label, value,
                                          ImGuiColorEditFlags_Float |
                                              ImGuiColorEditFlags_HDR);
    ImGui::EndDisabled();
    disabledHint(disabled);
    changed |= edited;
  };

  if (!editedMat.openPbrSpecularModel) {
    ImGui::TextWrapped(
        "This material is currently using its textured PreviewSurface "
        "fallback. Switch it to a session-local OpenPBR material to edit "
        "connection-free constants on the selected shape.");
    if (ImGui::Button("Use constant OpenPBR")) {
      makeConstant = true;
      changed = true;
    }
  } else {
    ImGui::TextDisabled("Constant inputs update the active renderer live.");
  }
  if (ImGui::CollapsingHeader("Base", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("Weight##base", &p.baseWeight, 0.0f, 1.0f,
           connected({"base_weight"}, -1, -1));
    color("Color##base", p.baseColor,
          connected({"base_color"}, 0, editedMat.baseColorTex));
    slider("Diffuse roughness", &p.diffuseRoughness, 0.0f, 1.0f,
           connected({"base_diffuse_roughness"}, -1, -1));
    slider("Metalness", &p.metalness, 0.0f, 1.0f,
           connected({"base_metalness"}, 1, editedMat.metallicTex));
  }
  if (ImGui::CollapsingHeader("Specular", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("Weight##specular", &p.specularWeight, 0.0f, 1.0f,
           connected({"specular_weight"}, 9, -1));
    color("Color##specular", p.specularColor,
          connected({"specular_color"}, 10, editedMat.specularColorTex));
    slider("Roughness##specular", &p.specularRoughness, 0.0f, 1.0f,
           connected({"specular_roughness", "base_roughness"}, 2,
                     editedMat.roughnessTex));
    drag("IOR##specular", &p.specularIor, 0.01f, 1.0f, 4.0f,
         connected({"specular_ior"}, 19, -1));
    slider("Anisotropy##specular", &p.specularAnisotropy, -1.0f, 1.0f,
           connected({"specular_anisotropy"}, -1, -1));
    slider("Rotation##specular", &p.specularRotation, 0.0f, 1.0f,
           connected({"specular_rotation"}, -1, -1));
    slider("Roughness anisotropy##specular",
           &p.specularRoughnessAnisotropy, -1.0f, 1.0f,
           connected({"specular_roughness_anisotropy"}, -1, -1));
  }
  if (ImGui::CollapsingHeader("Transmission", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("Weight##transmission", &p.transmission, 0.0f, 1.0f,
           connected({"transmission_weight"}, 11, -1));
    color("Color##transmission", p.transmissionColor,
          connected({"transmission_color"}, 12, -1));
    drag("Depth##transmission", &p.transmissionDepth, 0.01f, 0.0f, 1000.0f,
         connected({"transmission_depth"}, -1, -1));
    color("Scatter", p.transmissionScatter,
          connected({"transmission_scatter"}, -1, -1));
    slider("Scatter anisotropy", &p.transmissionScatterAnisotropy, -1.0f,
           1.0f, connected({"transmission_scatter_anisotropy"}, -1, -1));
    drag("Dispersion", &p.transmissionDispersion, 0.01f, 0.0f, 10.0f,
         connected({"transmission_dispersion"}, -1, -1));
    drag("Abbe number", &p.transmissionDispersionAbbeNumber, 0.1f, 0.0f,
         1000.0f,
         connected({"transmission_dispersion_abbe_number"}, -1, -1));
    drag("Dispersion scale", &p.transmissionDispersionScale, 0.01f, 0.0f,
         100.0f, connected({"transmission_dispersion_scale"}, -1, -1));
  }
  if (ImGui::CollapsingHeader("Subsurface", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("Weight##subsurface", &p.subsurface, 0.0f, 1.0f,
           connected({"subsurface_weight"}, 6, -1));
    color("Color##subsurface", p.subsurfaceColor,
          connected({"subsurface_color"}, 7, -1));
    const bool radiusConnected = connected(
        {"subsurface_radius", "subsurface_radius_scale"}, 8, -1);
    ImGui::BeginDisabled(radiusConnected);
    changed |= ImGui::DragFloat3("Radius", p.subsurfaceRadius, 0.01f, 0.0f,
                                 1000.0f, "%.3f");
    ImGui::EndDisabled();
    disabledHint(radiusConnected);
    drag("Scale##subsurface", &p.subsurfaceScale, 0.01f, 0.0f, 1000.0f,
         connected({"subsurface_scale"}, -1, -1));
    slider("Anisotropy##subsurface", &p.subsurfaceAnisotropy, -1.0f, 1.0f,
           connected({"subsurface_anisotropy"}, -1, -1));
    slider("Scatter anisotropy##subsurface",
           &p.subsurfaceScatterAnisotropy, -1.0f, 1.0f,
           connected({"subsurface_scatter_anisotropy"}, -1, -1));
  }
  if (ImGui::CollapsingHeader("Coat and fuzz")) {
    slider("Weight##coat", &p.coatWeight, 0.0f, 1.0f,
           connected({"coat_weight"}, 13, editedMat.coatWeightTex));
    color("Color##coat", p.coatColor,
          connected({"coat_color"}, 14, editedMat.coatColorTex));
    slider("Roughness##coat", &p.coatRoughness, 0.0f, 1.0f,
           connected({"coat_roughness"}, 15, editedMat.coatRoughnessTex));
    drag("IOR##coat", &p.coatIor, 0.01f, 1.0f, 4.0f,
         connected({"coat_ior"}, -1, -1));
    slider("Anisotropy##coat", &p.coatAnisotropy, -1.0f, 1.0f,
           connected({"coat_anisotropy"}, -1, -1));
    slider("Rotation##coat", &p.coatRotation, 0.0f, 1.0f,
           connected({"coat_rotation"}, -1, -1));
    slider("Affect color", &p.coatAffectColor, 0.0f, 1.0f,
           connected({"coat_affect_color"}, -1, -1));
    slider("Affect roughness", &p.coatAffectRoughness, 0.0f, 1.0f,
           connected({"coat_affect_roughness"}, -1, -1));
    slider("Roughness anisotropy##coat", &p.coatRoughnessAnisotropy, -1.0f,
           1.0f, connected({"coat_roughness_anisotropy"}, -1, -1));
    slider("Darkening", &p.coatDarkening, 0.0f, 1.0f,
           connected({"coat_darkening"}, -1, -1));
    slider("Weight##fuzz", &p.sheenWeight, 0.0f, 1.0f,
           connected({"fuzz_weight", "sheen_weight"}, 16, -1));
    color("Color##fuzz", p.sheenColor,
          connected({"fuzz_color", "sheen_color"}, 17, -1));
    slider("Roughness##fuzz", &p.sheenRoughness, 0.0f, 1.0f,
           connected({"fuzz_roughness", "sheen_roughness"}, 18, -1));
  }
  if (ImGui::CollapsingHeader("Emission, film, and opacity")) {
    drag("Emission luminance", &p.emission, 0.01f, 0.0f, 100000.0f,
         connected({"emission_luminance"}, -1, -1));
    color("Emission color", p.emissionColor,
          connected({"emission_color"}, 4, editedMat.emissiveTex));
    slider("Thin-film weight", &p.thinFilmWeight, 0.0f, 1.0f,
           connected({"thin_film_weight"}, -1, -1));
    drag("Thin-film thickness (nm)", &p.thinFilmThicknessNm, 1.0f, 0.0f,
         5000.0f, connected({"thin_film_thickness"}, -1, -1));
    drag("Thin-film IOR", &p.thinFilmIor, 0.01f, 1.0f, 4.0f,
         connected({"thin_film_ior"}, -1, -1));
    slider("Opacity", &p.opacity, 0.0f, 1.0f,
           connected({"geometry_opacity", "opacity"}, 3,
                     editedMat.opacityTex));
  }

  if (changed) {
    lightusd::tydra::ClampRealtimePbrMaterial(&p);
    openPbrMaterialEdit_.materialId = openPbrEditorMaterial_;
    openPbrMaterialEdit_.constants = p;
    openPbrMaterialEdit_.makeConstant = makeConstant;
    hasOpenPbrMaterialEdit_ = true;
  }
  ImGui::End();
}

void Gui::drawMaterialsPanel() {
  ImGui::Begin("Materials");
  if (draw_ && !draw_->materials.empty()) {
    ImGui::Text("Total: %zu materials", draw_->materials.size());
    ImGui::Text("Textures: %zu", draw_->textures.size());
    ImGui::Separator();
    for (size_t i = 0; i < draw_->materials.size(); ++i) {
      const auto& mat = draw_->materials[i];
      ImGui::PushID(static_cast<int>(i));
      std::string headerLabel = mat.name.empty()
                                    ? ("Material " + std::to_string(i))
                                    : mat.name;
      if (ImGui::CollapsingHeader(headerLabel.c_str(),
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Base color:  %.3f %.3f %.3f",
                    mat.baseColor[0], mat.baseColor[1], mat.baseColor[2]);
        ImGui::Text("Roughness:   %.3f", mat.roughness);
        ImGui::Text("Metallic:    %.3f", mat.metallic);
        ImGui::Text("Emissive:    %.3f %.3f %.3f",
                    mat.emissive[0], mat.emissive[1], mat.emissive[2]);
        ImGui::Text("Alpha:       %.3f", mat.alpha);
        const char* alphaModeStr = "opaque";
        if (mat.alphaMode == static_cast<int>(AlphaMode::Mask)) alphaModeStr = "mask";
        else if (mat.alphaMode == static_cast<int>(AlphaMode::Blend)) alphaModeStr = "blend";
        ImGui::Text("Alpha mode:  %s", alphaModeStr);
        if (mat.alphaMode == static_cast<int>(AlphaMode::Mask)) {
          ImGui::Text("Alpha cutoff: %.3f", mat.alphaCutoff);
        }
        if (mat.hasOpenPBRSurface) ImGui::TextUnformatted("Shader: OpenPBRSurface");
        else if (mat.hasUsdPreviewSurface) ImGui::TextUnformatted("Shader: UsdPreviewSurface");
        if (!mat.params.empty()) {
          ImGui::Text("Shader inputs: %zu", mat.params.size());
        }
        if (mat.hasLightRtOpenPBR) {
          ImGui::Text("LightRT flags: textures=%s normals=%s",
                      mat.lightRtOpenPBR.hasTextureInputs ? "yes" : "no",
                      mat.lightRtOpenPBR.hasNormalInput ? "yes" : "no");
        }
        if (mat.baseColorTex >= 0) ImGui::Text("Base color tex: %d", mat.baseColorTex);
        if (mat.metallicTex >= 0) ImGui::Text("Metallic tex: %d", mat.metallicTex);
        if (mat.roughnessTex >= 0) ImGui::Text("Roughness tex: %d", mat.roughnessTex);
        if (mat.normalTex >= 0) ImGui::Text("Normal tex: %d", mat.normalTex);
        if (mat.coatNormalTex >= 0) ImGui::Text("Coat normal tex: %d", mat.coatNormalTex);
        if (mat.emissiveTex >= 0) ImGui::Text("Emissive tex: %d", mat.emissiveTex);
        if (!mat.params.empty() && ImGui::TreeNode("Shader inputs")) {
          if (ImGui::BeginTable("##shader_inputs", 8,
                                ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Shader");
            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Texture");
            ImGui::TableSetupColumn("Channel");
            ImGui::TableSetupColumn("Scale/Bias");
            ImGui::TableSetupColumn("UV");
            ImGui::TableHeadersRow();
            for (const DrawMaterialParamCPU& param : mat.params) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(param.shader.c_str());
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(param.name.c_str());
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(MaterialParamTypeName(param.type));
              ImGui::TableNextColumn();
              if (param.type == DrawMaterialParamType::Float) {
                ImGui::Text("%.3f", param.value[0]);
              } else if (param.type == DrawMaterialParamType::Vec2) {
                ImGui::Text("%.3f %.3f", param.value[0], param.value[1]);
              } else {
                ImGui::Text("%.3f %.3f %.3f", param.value[0], param.value[1],
                            param.value[2]);
              }
              ImGui::TableNextColumn();
              if (param.texture >= 0) ImGui::Text("%d", param.texture);
              else if (param.renderTexture >= 0) ImGui::Text("render:%d", param.renderTexture);
              else ImGui::TextUnformatted("-");
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(MaterialParamChannelName(param.channel));
              ImGui::TableNextColumn();
              const bool hasScaleBias =
                  std::fabs(param.sample.scale[0] - 1.0f) > 1.0e-6f ||
                  std::fabs(param.sample.scale[1] - 1.0f) > 1.0e-6f ||
                  std::fabs(param.sample.scale[2] - 1.0f) > 1.0e-6f ||
                  std::fabs(param.sample.scale[3] - 1.0f) > 1.0e-6f ||
                  std::fabs(param.sample.bias[0]) > 1.0e-6f ||
                  std::fabs(param.sample.bias[1]) > 1.0e-6f ||
                  std::fabs(param.sample.bias[2]) > 1.0e-6f ||
                  std::fabs(param.sample.bias[3]) > 1.0e-6f;
              if (hasScaleBias) {
                ImGui::Text("%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f",
                            param.sample.scale[0], param.sample.scale[1],
                            param.sample.scale[2], param.sample.scale[3],
                            param.sample.bias[0], param.sample.bias[1],
                            param.sample.bias[2], param.sample.bias[3]);
              } else {
                ImGui::TextUnformatted("-");
              }
              ImGui::TableNextColumn();
              if (HasNonIdentityUvXform(param.sample.uv)) {
                ImGui::Text("%.2f %.2f %.2f / %.2f %.2f %.2f",
                            param.sample.uv.m00, param.sample.uv.m01,
                            param.sample.uv.tx, param.sample.uv.m10,
                            param.sample.uv.m11, param.sample.uv.ty);
              } else {
                ImGui::TextUnformatted("-");
              }
            }
            ImGui::EndTable();
          }
          ImGui::TreePop();
        }
      }
      ImGui::PopID();
    }
    if (!draw_->textures.empty() &&
        ImGui::CollapsingHeader("Texture Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
      const float thumb = 96.0f;
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      const float avail = ImGui::GetContentRegionAvail().x;
      int cols = static_cast<int>((avail + gap) / (thumb + gap));
      if (cols < 1) cols = 1;
      for (size_t ti = 0; ti < draw_->textures.size(); ++ti) {
        if (ti > 0 && (static_cast<int>(ti) % cols) != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(ti));
        ImGui::BeginGroup();
        const DrawTextureCPU& tex = draw_->textures[ti];
        const light3d::Image* img = &tex.image;
        if (tex.isUdim && !tex.udimTiles.empty()) img = &tex.udimTiles[0].image;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + thumb, p.y + thumb),
                          IM_COL32(30, 30, 30, 255));
        if (img && img->width > 0 && img->height > 0 && img->channels >= 4 &&
            !img->data.empty()) {
          const int cells = 32;
          const float cell = thumb / static_cast<float>(cells);
          for (int y = 0; y < cells; ++y) {
            const int sy = (y * img->height) / cells;
            for (int x = 0; x < cells; ++x) {
              const int sx = (x * img->width) / cells;
              const size_t off =
                  (static_cast<size_t>(sy) * static_cast<size_t>(img->width) +
                   static_cast<size_t>(sx)) *
                  4u;
              if (off + 3 >= img->data.size()) continue;
              const ImU32 c = IM_COL32(img->data[off + 0], img->data[off + 1],
                                       img->data[off + 2], img->data[off + 3]);
              dl->AddRectFilled(ImVec2(p.x + static_cast<float>(x) * cell, p.y + static_cast<float>(y) * cell),
                                ImVec2(p.x + static_cast<float>(x + 1) * cell + 0.5f,
                                       p.y + static_cast<float>(y + 1) * cell + 0.5f),
                                c);
            }
          }
        }
        dl->AddRect(p, ImVec2(p.x + thumb, p.y + thumb),
                    IM_COL32(90, 90, 90, 255));
        ImGui::Dummy(ImVec2(thumb, thumb));
        ImGui::Text("#%zu %dx%d%s", ti, img ? img->width : 0, img ? img->height : 0,
                    tex.isUdim ? " UDIM" : "");
        if (tex.isUdim) {
          ImGui::Text("tiles %zu", tex.udimTiles.size());
        }
        if (tex.renderUdimId >= 0) ImGui::Text("udim image set %d", tex.renderUdimId);
        else if (tex.renderImageId >= 0) ImGui::Text("image %d", tex.renderImageId);
        ImGui::Text("wrap %d/%d%s", tex.wrapS, tex.wrapT,
                    tex.srgb ? " sRGB" : " raw");
        if (!tex.assetIdentifier.empty()) {
          std::string label = std::filesystem::path(tex.assetIdentifier).filename().string();
          if (label.empty()) label = tex.assetIdentifier;
          ImGui::TextWrapped("%s", label.c_str());
          if (ImGui::IsItemHovered()) {
            std::string tip = tex.assetIdentifier;
            if (tex.isUdim && !tex.udimTiles.empty()) {
              tip += "\n";
              const size_t n = std::min<size_t>(tex.udimTiles.size(), 8);
              for (size_t ui = 0; ui < n; ++ui) {
                const DrawUdimTileCPU& tile = tex.udimTiles[ui];
                tip += std::to_string(tile.udim) + " image " +
                       std::to_string(tile.renderImageId);
                if (!tile.assetIdentifier.empty()) {
                  tip += " ";
                  tip += tile.assetIdentifier;
                }
                tip += "\n";
              }
              if (tex.udimTiles.size() > n) {
                tip += "...";
              }
            }
            ImGui::SetTooltip("%s", tip.c_str());
          }
        }
        ImGui::EndGroup();
        ImGui::PopID();
      }
    }
  } else {
    ImGui::TextDisabled("No materials loaded.");
  }
  ImGui::End();
}

void Gui::drawCompositionGraph() {
  ImGui::Begin("Composition Graph");
  if (!selPrim_) {
    ImGui::TextDisabled("Select a prim to see its composition arcs.");
    ImGui::End();
    return;
  }
  const auto& m = selPrim_->metas();

  // Collect arcs into a simple data structure.
  struct ArcNode {
    std::string label;
    std::string detail;
    ImU32 color;
  };
  std::vector<ArcNode> arcs;

  auto addArc = [&](const char* type, const std::string& detail, ImU32 color) {
    arcs.push_back({type, detail, color});
  };

  if (m.references.has_value() && !m.references->empty()) {
    for (const auto& [qual, refs] : *m.references) {
      for (const auto& ref : refs) {
        std::string qstr = lightusd::to_string(qual);
        std::string detail = qstr + " " + ref.asset_path.GetAssetPath();
        addArc("reference", detail, IM_COL32(100, 180, 255, 220));
      }
    }
  }
  if (m.payload.has_value() && !m.payload->empty()) {
    for (const auto& [qual, pls] : *m.payload) {
      for (const auto& pl : pls) {
        addArc("payload", pl.asset_path.GetAssetPath(), IM_COL32(255, 180, 80, 220));
      }
    }
  }
  if (m.inherits.has_value() && !m.inherits->empty()) {
    for (const auto& [qual, inh] : *m.inherits) {
      for (const auto& p : inh) {
        addArc("inherits", p.full_path_name(), IM_COL32(180, 255, 120, 220));
      }
    }
  }
  if (m.specializes.has_value() && !m.specializes->empty()) {
    for (const auto& [qual, spec] : *m.specializes) {
      for (const auto& p : spec) {
        addArc("specializes", p.full_path_name(), IM_COL32(220, 140, 255, 220));
      }
    }
  }
  if (m.variantSets.has_value() && !m.variantSets->empty()) {
    for (const auto& [qual, vs] : *m.variantSets) {
      for (const auto& vsName : vs) {
        addArc("variantSet", vsName, IM_COL32(255, 220, 100, 220));
      }
    }
  }

  if (arcs.empty()) {
    ImGui::TextDisabled("No composition arcs on this prim.");
    ImGui::End();
    return;
  }

  // Draw the prim and its arcs as a simple graph.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  ImVec2 canvasSize = ImGui::GetContentRegionAvail();
  if (canvasSize.x < 10 || canvasSize.y < 10) { ImGui::End(); return; }

  const float primBoxW = 200.0f;
  const float primBoxH = 40.0f;
  const float arcBoxW = 220.0f;
  const float arcBoxH = 36.0f;
  const float spacing = 16.0f;
  const float topMargin = 20.0f;

  // Center the prim node at the top.
  float cx = canvasPos.x + canvasSize.x * 0.5f;
  float primX = cx - primBoxW * 0.5f;
  float primY = canvasPos.y + topMargin;

  // Draw prim node.
  ImVec2 primMin(primX, primY);
  ImVec2 primMax(primX + primBoxW, primY + primBoxH);
  dl->AddRectFilled(primMin, primMax, IM_COL32(70, 80, 110, 220), 6.0f);
  dl->AddRect(primMin, primMax, IM_COL32(130, 150, 190, 255), 6.0f);
  dl->AddText(ImVec2(primX + 8, primY + 10), IM_COL32(255, 255, 255, 255),
              selPrim_->element_name().c_str());

  // Layout arcs below in 2 columns.
  int cols = std::min(static_cast<int>(arcs.size()), 2);
  float totalWidth = static_cast<float>(cols) * arcBoxW + static_cast<float>(cols - 1) * spacing;
  float startX = canvasPos.x + std::max(0.0f, (canvasSize.x - totalWidth) * 0.5f);

  for (size_t i = 0; i < arcs.size(); ++i) {
    int col = static_cast<int>(i) % cols;
    int row = static_cast<int>(i) / cols;
    float ax = startX + static_cast<float>(col) * (arcBoxW + spacing);
    float ay = primY + primBoxH + 50.0f + static_cast<float>(row) * (arcBoxH + 10.0f);

    // Connection line from prim bottom to arc top.
    float primMidX = cx;
    float arcMidX = ax + arcBoxW * 0.5f;
    dl->AddLine(ImVec2(primMidX, primY + primBoxH),
                ImVec2(arcMidX, ay), IM_COL32(180, 180, 200, 180), 1.5f);

    // Arc node box.
    ImVec2 arcMin(ax, ay);
    ImVec2 arcMax(ax + arcBoxW, ay + arcBoxH);
    dl->AddRectFilled(arcMin, arcMax, arcs[i].color, 4.0f);
    dl->AddRect(arcMin, arcMax, IM_COL32(200, 200, 220, 180), 4.0f);

    // Arc type label (e.g. "reference").
    dl->AddText(ImVec2(ax + 6, ay + 2), IM_COL32(255, 255, 255, 255),
                arcs[i].label.c_str());
    // Arc detail (e.g. "prepend /path/to/file.usda").
    std::string detail = arcs[i].detail;
    if (detail.size() > 32) { detail.resize(29); detail += "..."; }
    dl->AddText(ImVec2(ax + 6, ay + 18), IM_COL32(220, 220, 240, 200),
                detail.c_str());

    // Click handling: if mouse is over this node, select it.
    if (ImGui::IsMouseHoveringRect(arcMin, arcMax) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      // For refs/payloads with asset paths, try to open them; for others, no-op.
      (void)0;
    }
  }

  ImGui::Dummy(ImVec2(canvasSize.x, primY + primBoxH + 50.0f +
                      static_cast<float>((arcs.size() + cols - 1) / cols) * (arcBoxH + 10.0f) +
                      topMargin));
  ImGui::End();
}

void Gui::drawTimeline() {
  ImGui::Begin("Timeline");
  if (!timeline_.hasAnimation) {
    ImGui::TextDisabled(loaded_ && loaded_->ok
                            ? "No animation (single time code)."
                            : "No scene loaded.");
    ImGui::End();
    return;
  }

  // Transport: Play/Pause + Stop (reset to start).
  if (ImGui::Button(timeline_.playing ? "Pause" : "Play", ImVec2(70, 0))) {
    wantTogglePlay_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop", ImVec2(70, 0))) {
    wantStop_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("|<", ImVec2(40, 0))) { wantStepBackward_ = true; }
  ImGui::SameLine();
  if (ImGui::Button(">|", ImVec2(40, 0))) { wantStepForward_ = true; }
  ImGui::SameLine();
  ImGui::Checkbox("Loop", &loop_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  if (ImGui::InputFloat("Speed", &speed_, 0.0f, 0.0f, "%.2fx")) {
    if (speed_ < 0.01f) speed_ = 0.01f;
    if (speed_ > 100.0f) speed_ = 100.0f;
  }
  if (timeline_.converting) {
    ImGui::SameLine();
    ImGui::TextDisabled("(evaluating...)");
  }

  // Scrubber over [start, end] in time codes.
  float cur = static_cast<float>(timeline_.current);
  const float lo = static_cast<float>(timeline_.start);
  const float hi = static_cast<float>(timeline_.end);
  ImGui::SetNextItemWidth(-1.0f);  // full width
  if (ImGui::SliderFloat("##timeline_scrub", &cur, lo, hi, "frame %.2f")) {
    hasSeek_ = true;
    seekTime_ = static_cast<double>(cur);
  }

  ImGui::Text("%.2f / [%.1f .. %.1f]  @ %.2f tps", timeline_.current,
              timeline_.start, timeline_.end, timeline_.fps);
  ImGui::End();
}

// Process resident set size (RSS) in MB, from /proc (Linux). 0 if unavailable.
static size_t ReadProcessRssMB() {
#ifdef _WIN32
  return 0;  // /proc/self/statm doesn't exist on Windows
#else
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long pages = 0, resident = 0;
  if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
  std::fclose(f);
  const long pageSz = sysconf(_SC_PAGESIZE);
  return static_cast<size_t>((static_cast<long long>(resident) * pageSz) / (1024 * 1024));
#endif
}

// amdgpu VRAM (used,total) in MB via sysfs; fallback when the renderer can't
// report GPU memory (e.g. the Vulkan backend). 0 if unavailable.
static bool ReadAmdgpuVramMB(size_t* usedMB, size_t* totalMB) {
  const char* cards[] = {"/sys/class/drm/card0/device/mem_info_vram_",
                         "/sys/class/drm/card1/device/mem_info_vram_"};
  for (const char* base : cards) {
    auto readVal = [](const std::string& path) -> long long {
      FILE* f = std::fopen(path.c_str(), "r");
      if (!f) return -1;
      long long v = -1;
      if (std::fscanf(f, "%lld", &v) != 1) v = -1;
      std::fclose(f);
      return v;
    };
    const long long tot = readVal(std::string(base) + "total");
    const long long use = readVal(std::string(base) + "used");
    if (tot > 0 && use >= 0) {
      if (totalMB) *totalMB = static_cast<size_t>(tot / (1024 * 1024));
      if (usedMB) *usedMB = static_cast<size_t>(use / (1024 * 1024));
      return true;
    }
  }
  return false;
}

void Gui::drawStats() {
  ImGui::Begin("Stats");
  const float uiFps = ImGui::GetIO().Framerate;
  if (threadedFrameRate_) {
    ImGui::Text("Render FPS: %.1f%s", renderFps_,
                renderFps_ <= 0.0f ? " (warming up)" : "");
    ImGui::Text("UI FPS: %.1f (60 Hz cap)", uiFps);
    if (adaptiveQuality_) {
      const char* tier = adaptiveTier_ == 2 ? "interactive" :
                         (adaptiveTier_ == 1 ? "balanced" : "full");
      ImGui::Text("Adaptive: %s, scale %.2f, target %.0f FPS", tier,
                  renderScale_, adaptiveTargetFps_);
    }
  } else {
    ImGui::Text("FPS: %.1f (%.2f ms)", uiFps,
                uiFps > 0.0f ? 1000.0f / uiFps : 0.0f);
  }
  if (dynamicRasterLodEnabled_) {
    size_t ready = 0;
    size_t levels = 0;
    if (draw_) {
      for (const DrawMeshCPU& mesh : draw_->meshes) {
        if (!mesh.prototypeLods.empty()) ++ready;
        levels += mesh.prototypeLods.size();
      }
    }
    ImGui::Text("Prototype LOD: tier %d, error %.2f px, %zu prototypes/%zu levels",
                dynamicRasterLodTier_, dynamicRasterLodErrorPx_, ready, levels);
  }
  ImGui::Text("Backend: %s", renderer_ ? renderer_->caps().backend_name : "?");
  // CPU RSS + GPU VRAM, refreshed a few times a second (the queries touch /proc
  // and the driver, so they are throttled rather than run every frame).
  static size_t cpuMB = 0, vramUsedMB = 0, vramTotalMB = 0;
  static bool haveVram = false;
  static double lastPoll = -1.0;
  const double now = ImGui::GetTime();
  if (lastPoll < 0.0 || now - lastPoll > 0.5) {
    lastPoll = now;
    cpuMB = ReadProcessRssMB();
    haveVram = renderer_ && renderer_->gpuMemoryMB(&vramUsedMB, &vramTotalMB);
    if (!haveVram) haveVram = ReadAmdgpuVramMB(&vramUsedMB, &vramTotalMB);
  }
  if (cpuMB > 0) ImGui::Text("CPU mem (RSS): %zu MB", cpuMB);
  if (haveVram) {
    ImGui::Text("GPU VRAM: %zu / %zu MB (%.0f%%)", vramUsedMB, vramTotalMB,
                vramTotalMB ? 100.0 * double(vramUsedMB) / double(vramTotalMB) : 0.0);
  }
  ImGui::Text("Skinning: %s requested, %s effective",
              SkinningModeLabel(skinning_.requested),
              SkinningModeLabel(skinning_.effective));
  if (!skinning_.reason.empty()) ImGui::TextDisabled("%s", skinning_.reason.c_str());
  ImGui::Separator();
  if (draw_) {
    ImGui::Text("Meshes: %zu", draw_->meshes.size());
    size_t pointSamples = 0, curveSamples = 0;
    for (const DrawPointsCPU& p : draw_->points) pointSamples += p.points.size() / 3;
    for (const DrawCurvesCPU& c : draw_->curves) curveSamples += c.points.size() / 3;
    ImGui::Text("Points: %zu prims / %zu samples", draw_->points.size(),
                pointSamples);
    ImGui::Text("Curves: %zu prims / %zu tessellated samples",
                draw_->curves.size(), curveSamples);
    // draw_->vertexCount is captured at load (CPU geometry may be freed after
    // upload on the --next path, so summing meshes[].vertices would read 0).
    ImGui::Text("Vertices: %zu", draw_->vertexCount);
    ImGui::Text("Triangles: %zu", draw_->triangleCount);
    // Frustum-cull stats (this frame). "visible" reflects per-mesh + per-instance
    // culling; with culling disabled these equal the totals.
    ImGui::Text("Visible meshes: %zu / %zu", statVisibleMeshes_,
                draw_->meshes.size());
    if (statTotalInstances_ > 0) {
      ImGui::Text("Visible instances: %zu / %zu", statVisibleInstances_,
                  statTotalInstances_);
      if (cullRunning_.load()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.1f, 1.0f), "(culling\xE2\x80\xA6)");
      }
    }
    if (statProxyInstances_ > 0)
      ImGui::Text("LOD proxy instances: %zu", statProxyInstances_);
    ImGui::Text("Drawn triangles: %zu", statNonInstTris_ + statInstTris_);
    ImGui::Text("Draw calls: %zu", statDrawCalls_);
    ImGui::Text("Materials: %zu", draw_->materials.size());
    ImGui::Text("Textures: %zu", draw_->textures.size());
    if (draw_->hasBounds) {
      ImGui::Text("Bounds: [%.2f %.2f %.2f] - [%.2f %.2f %.2f]", draw_->aabbMin[0],
                  draw_->aabbMin[1], draw_->aabbMin[2], draw_->aabbMax[0],
                  draw_->aabbMax[1], draw_->aabbMax[2]);
    }
    if (draw_->truncated) {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.25f, 1.0f),
                         "Scene TRUNCATED to fit the render budget.");
      ImGui::TextDisabled("(raise the budget below, then Reload)");
    }
    if (!draw_->skipped.empty()) {
      ImGui::Separator();
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Skipped (%zu):",
                         draw_->skipped.size());
      for (const std::string& s : draw_->skipped) ImGui::BulletText("%s", s.c_str());
    }
  }
  if (loaded_ && !loaded_->warn.empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Warnings:");
    ImGui::SameLine();
    DiagnosticTextArea("##load-warnings", "Copy warnings", loaded_->warn);
  }
  if (loaded_ && !loaded_->ok && !loaded_->err.empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Load error:");
    ImGui::SameLine();
    DiagnosticTextArea("##load-error", "Copy error", loaded_->err);
  }

  if (budget_) {
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Render budget")) {
      int triM = static_cast<int>(budget_->maxTriangles / 1000000ull);
      if (ImGui::InputInt("Max triangles (M)", &triM)) {
        budget_->maxTriangles = static_cast<size_t>(triM < 1 ? 1 : triM) * 1000000ull;
      }
      int vbMB = static_cast<int>(budget_->maxVertexBytes / (1024ull * 1024ull));
      if (ImGui::InputInt("Max vertex mem (MB)", &vbMB)) {
        budget_->maxVertexBytes =
            static_cast<size_t>(vbMB < 16 ? 16 : vbMB) * 1024ull * 1024ull;
      }
      float t = static_cast<float>(budget_->convertTimeBudgetSec);
      if (ImGui::InputFloat("Convert time budget (s, 0=off)", &t)) {
        budget_->convertTimeBudgetSec = t < 0.0f ? 0.0f : t;
      }
      ImGui::TextDisabled("Applies to the next load (File > Reload).");
    }
  }
  if (loadOptions_) {
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Subdivision surfaces",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      int level = loadOptions_->subdivisionLevel;
      if (ImGui::SliderInt("Pre-tessellation level", &level, 0, 5)) {
        loadOptions_->subdivisionLevel = std::max(0, level);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) wantReload_ = true;
      ImGui::TextDisabled(
          "Catmull-Clark, Loop and bilinear meshes rebuild asynchronously.");
    }
    if (ImGui::CollapsingHeader("Load scheduling")) {
      int composeThreads = static_cast<int>(loadOptions_->compositionThreads);
      if (ImGui::InputInt("Composition threads (0=auto)", &composeThreads)) {
        loadOptions_->compositionThreads =
            static_cast<unsigned>(std::max(0, composeThreads));
      }
      int convertThreads = static_cast<int>(loadOptions_->conversionThreads);
      if (ImGui::InputInt("Conversion threads (0=auto)", &convertThreads)) {
        loadOptions_->conversionThreads =
            static_cast<unsigned>(std::max(0, convertThreads));
      }
      int opinionBatch = static_cast<int>(loadOptions_->compositionOpinionBatch);
      if (ImGui::InputInt("Opinion batch (0=auto)", &opinionBatch)) {
        loadOptions_->compositionOpinionBatch =
            static_cast<size_t>(std::max(0, opinionBatch));
      }
      int instanceChunk = static_cast<int>(loadOptions_->instanceChunkSamples);
      if (ImGui::InputInt("Instance chunk samples (0=auto)", &instanceChunk)) {
        loadOptions_->instanceChunkSamples =
            static_cast<size_t>(std::max(0, instanceChunk));
      }
      int meshChunk = static_cast<int>(loadOptions_->meshConversionChunkPrims);
      if (ImGui::InputInt("Mesh chunk prims (0=auto)", &meshChunk)) {
        loadOptions_->meshConversionChunkPrims =
            static_cast<size_t>(std::max(0, meshChunk));
      }
      int meshChunkMB = static_cast<int>(
          loadOptions_->meshConversionChunkBytes / (1024ull * 1024ull));
      if (ImGui::InputInt("Mesh chunk memory MB (0=auto)", &meshChunkMB)) {
        loadOptions_->meshConversionChunkBytes =
            static_cast<size_t>(std::max(0, meshChunkMB)) * 1024ull * 1024ull;
      }
      int curveMin = static_cast<int>(loadOptions_->curveParallelMinPrims);
      if (ImGui::InputInt("Parallel curves minimum (0=auto)", &curveMin)) {
        loadOptions_->curveParallelMinPrims =
            static_cast<size_t>(std::max(0, curveMin));
      }
      ImGui::TextDisabled("Applies to the next load (File > Reload). CLI values initialize these controls.");
    }
  }
  ImGui::End();
}

void Gui::handleNavigation() {
  if (!cam_) return;
  ImGuiIO& io = ImGui::GetIO();
  const bool alt = io.KeyAlt;

  if (!io.WantTextInput && io.KeyCtrl && io.KeyShift &&
      ImGui::IsKeyPressed(ImGuiKey_S)) {
    std::string err;
    if (saveImGuiLayout(&err)) {
      imguiLayoutStatus_ = "Layout saved";
    } else {
      imguiLayoutStatus_ = "Layout save failed: " + err;
    }
  }

  if (ImGui::IsKeyPressed(ImGuiKey_F1)) cycleNavigationHelpMode();
  if (!io.WantTextInput && timeline_.hasAnimation &&
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    wantTogglePlay_ = true;
  }

  if (navMode_ == 0 && vpHovered_ && alt) {
    // Shift+Alt+LMB pans (an alternative to Alt+MMB for trackpads / keypads with
    // no middle button); plain Alt+LMB orbits. navMode_ latches until release, so
    // the modifier state at press time selects the mode for the whole drag.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) navMode_ = io.KeyShift ? 2 : 1;
    else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) navMode_ = 2;
    else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) navMode_ = 3;
  }
  if (navMode_ != 0) {
    const ImVec2 d = io.MouseDelta;
    if (navMode_ == 1) cam_->orbit(d.x, d.y);
    else if (navMode_ == 2) cam_->pan(d.x, d.y);
    else if (navMode_ == 3) cam_->dolly((d.x - d.y) * 0.05f);
    const bool anyDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!anyDown) navMode_ = 0;
  }

  if (vpHovered_ && io.MouseWheel != 0.0f) cam_->dolly(io.MouseWheel);

  // Maya-style hotkeys (viewport hovered, no text field focused).
  if (vpHovered_ && !io.WantTextInput) {
    // Keep wireframe on V so the left-hand W/S pair remains available for
    // forward/backward movement with a right-handed mouse.
    if (!io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
      wireCycle_ = (wireCycle_ + 1) % 3;
      if (mode_ == RenderMode::Wireframe) mode_ = RenderMode::Shaded;
    }
    if (!io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
      static const float kScales[] = {0.25f, 0.5f, 1.0f, 1.5f};
      const float current = cam_->navigationScale();
      size_t next = 0;
      for (size_t i = 0; i < 4; ++i) {
        if (std::fabs(current - kScales[i]) < 1.0e-4f) {
          next = (i + 1) % 4;
          break;
        }
        if (current < kScales[i]) { next = i; break; }
      }
      cam_->setNavigationScale(kScales[next]);
    }
    // CPU RT: dedicated keybinding, toggles on/off (App restores whichever
    // technique was active before).
    if (!io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
      wantToggleCpuRt_ = true;
    }
    // Walk/fly without changing the orbit relationship: both camera eye and
    // pivot translate, so repeated movement passes cleanly through world origin.
    if (!io.KeyAlt && !io.KeyCtrl) {
      const float step = io.KeyShift ? 3.0f : 1.0f;
      if (ImGui::IsKeyPressed(ImGuiKey_W) ||
          ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        cam_->moveForward(step);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_S) ||
          ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        cam_->moveForward(-step);
      }
    }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) goSelectionBack();
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) goSelectionForward();
    if (ImGui::IsKeyPressed(ImGuiKey_0)) homeView();
    if (ImGui::IsKeyPressed(ImGuiKey_5)) applyViewPreset(CameraViewPreset::Isometric);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) selectAdjacentMesh(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) selectAdjacentMesh(1);
    if (io.KeyCtrl) {
      if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        if (io.KeyShift) saveCameraBookmark(0);
        else loadCameraBookmark(0);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_2)) {
        if (io.KeyShift) saveCameraBookmark(1);
        else loadCameraBookmark(1);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        if (io.KeyShift) saveCameraBookmark(2);
        else loadCameraBookmark(2);
      }
    } else {
      if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        applyViewPreset(io.KeyShift ? CameraViewPreset::Back : CameraViewPreset::Front);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        applyViewPreset(io.KeyShift ? CameraViewPreset::Left : CameraViewPreset::Right);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_7)) {
        applyViewPreset(io.KeyShift ? CameraViewPreset::Bottom : CameraViewPreset::Top);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) frameSelected();  // frame selection
    if (ImGui::IsKeyPressed(ImGuiKey_A)) frameAll();        // frame all

    // Shift+K cycles off -> bounds -> Z-tested wire -> X-ray wire. Plain K is
    // the quick hide/restore toggle and never clears the actual selection.
    if (!io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
      if (io.KeyShift) {
        selectionHighlightMode_ = (selectionHighlightMode_ + 1) % 4;
        if (selectionHighlightMode_ > 0)
          selectionHighlightLastVisibleMode_ = selectionHighlightMode_;
      } else if (selectionHighlightMode_ == 0) {
        selectionHighlightMode_ = selectionHighlightLastVisibleMode_;
      } else {
        selectionHighlightLastVisibleMode_ = selectionHighlightMode_;
        selectionHighlightMode_ = 0;
      }
    }

    // Hide family. Needs a valid selected mesh (except none is required for the
    // plain toggle/show, but those are no-ops without a selection too).
    if (ImGui::IsKeyPressed(ImGuiKey_H)) {
      const int sel = selMeshIndex_;
      const bool haveSel =
          sel >= 0 && static_cast<size_t>(sel) < meshVisible_.size();
      const size_t carrier = carrierIndexForPath(selPath_);
      const bool haveCarrier = carrier != static_cast<size_t>(-1) &&
                               carrier < carrierVisible_.size();
      if (io.KeyAlt) {
        // Alt+H: isolate the selected prim, including renderable descendants.
        isolateSelection();
      } else if (io.KeyCtrl) {
        setSelectionVisibility(false);
      } else if (io.KeyShift) {
        setSelectionVisibility(true);
      } else if (haveSel) {
        // Plain H: toggle the selection's visibility.
        meshVisible_[static_cast<size_t>(sel)] =
            meshVisible_[static_cast<size_t>(sel)] ? 0 : 1;
      } else if (haveCarrier) {
        carrierVisible_[carrier] = carrierVisible_[carrier] ? 0 : 1;
      }
    }
  }
}

void Gui::applyViewPreset(CameraViewPreset preset) {
  if (cam_) cam_->setPreset(preset);
}

void Gui::homeView() {
  if (!cam_) return;
  cam_->setPreset(CameraViewPreset::Isometric);
  if (draw_ && draw_->hasBounds) {
    cam_->fitToScene(draw_->aabbMin, draw_->aabbMax);
  }
}

void Gui::frameSelected() {
  if (!cam_ || !draw_) return;
  const int sel = selMeshIndex_;
  if (sel >= 0 && static_cast<size_t>(sel) < draw_->meshes.size()) {
    const DrawMeshCPU& m = draw_->meshes[static_cast<size_t>(sel)];
    cam_->fitToScene(m.aabbMin, m.aabbMax);
    return;
  }
  for (const DrawPointsCPU& p : draw_->points) {
    if (p.absPath == selPath_) {
      cam_->fitToScene(p.aabbMin, p.aabbMax);
      return;
    }
  }
  for (const DrawCurvesCPU& c : draw_->curves) {
    if (c.absPath == selPath_) {
      cam_->fitToScene(c.aabbMin, c.aabbMax);
      return;
    }
  }
  if (draw_->hasBounds) {
    cam_->fitToScene(draw_->aabbMin, draw_->aabbMax);
  }
}

bool Gui::selectedFocusPoint(light3d::Vec3* point,
                             bool* fromViewportPick) const {
  if (fromViewportPick) *fromViewportPick = false;
  if (!point || !draw_ || selPath_.empty()) return false;
  if (haveLastPickedFocusPoint_ && lastPickedFocusPath_ == selPath_) {
    *point = lastPickedFocusPoint_;
    if (fromViewportPick) *fromViewportPick = true;
    return true;
  }

  bool found = false;
  light3d::Vec3 boundsMin{0.0f, 0.0f, 0.0f};
  light3d::Vec3 boundsMax{0.0f, 0.0f, 0.0f};
  auto includeBounds = [&](const std::string& path, const float mn[3],
                           const float mx[3]) {
    if (!PathIsSameOrDescendant(path, selPath_)) return;
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(mn[i]) || !std::isfinite(mx[i]) || mn[i] > mx[i]) {
        return;
      }
    }
    const light3d::Vec3 lo{mn[0], mn[1], mn[2]};
    const light3d::Vec3 hi{mx[0], mx[1], mx[2]};
    if (!found) {
      boundsMin = lo;
      boundsMax = hi;
      found = true;
      return;
    }
    boundsMin.x = std::min(boundsMin.x, lo.x);
    boundsMin.y = std::min(boundsMin.y, lo.y);
    boundsMin.z = std::min(boundsMin.z, lo.z);
    boundsMax.x = std::max(boundsMax.x, hi.x);
    boundsMax.y = std::max(boundsMax.y, hi.y);
    boundsMax.z = std::max(boundsMax.z, hi.z);
  };
  for (const DrawMeshCPU& mesh : draw_->meshes) {
    includeBounds(mesh.absPath, mesh.aabbMin, mesh.aabbMax);
  }
  for (const DrawPointsCPU& points : draw_->points) {
    includeBounds(points.absPath, points.aabbMin, points.aabbMax);
  }
  for (const DrawCurvesCPU& curves : draw_->curves) {
    includeBounds(curves.absPath, curves.aabbMin, curves.aabbMax);
  }
  if (!found) return false;
  *point = (boundsMin + boundsMax) * 0.5f;
  return true;
}

bool Gui::focusDofOnSelection(float* focusDistance) {
  if (focusDistance) *focusDistance = 0.0f;
  if (!cam_) return false;
  light3d::Vec3 point;
  if (!selectedFocusPoint(&point)) return false;
  const light3d::Vec3 eye = cam_->eye();
  const float distance =
      RtFocusDistanceToPoint(eye, cam_->target() - eye, point);
  const float minimum = std::max(1.0e-4f, cam_->nearPlane() * 1.001f);
  if (!std::isfinite(distance) || distance <= minimum) return false;
  cameraLens_.focusDistance = distance;
  if (focusDistance) *focusDistance = distance;
  return true;
}

void Gui::frameAll() {
  if (cam_ && draw_ && draw_->hasBounds) {
    cam_->fitToScene(draw_->aabbMin, draw_->aabbMax);
  }
}

void Gui::unhideAll() {
  for (auto& v : meshVisible_) v = 1;
  for (auto& v : carrierVisible_) v = 1;
}

bool Gui::isolateSelected() {
  if (!selectionHasRenderable()) return false;
  isolateSelection();
  return true;
}

void Gui::showAllRenderables() { unhideAll(); }

bool Gui::selectionHasRenderable() const {
  if (!draw_ || selPath_.empty()) return false;
  for (const DrawMeshCPU& mesh : draw_->meshes)
    if (PathIsSameOrDescendant(mesh.absPath, selPath_)) return true;
  for (const DrawPointsCPU& points : draw_->points)
    if (PathIsSameOrDescendant(points.absPath, selPath_)) return true;
  for (const DrawCurvesCPU& curves : draw_->curves)
    if (PathIsSameOrDescendant(curves.absPath, selPath_)) return true;
  return false;
}

void Gui::setSelectionVisibility(bool visible) {
  if (!draw_ || selPath_.empty()) return;
  const uint8_t value = visible ? uint8_t{1} : uint8_t{0};
  for (size_t i = 0; i < draw_->meshes.size() && i < meshVisible_.size(); ++i)
    if (PathIsSameOrDescendant(draw_->meshes[i].absPath, selPath_))
      meshVisible_[i] = value;
  size_t carrier = 0;
  for (const DrawPointsCPU& points : draw_->points) {
    if (carrier < carrierVisible_.size() &&
        PathIsSameOrDescendant(points.absPath, selPath_))
      carrierVisible_[carrier] = value;
    ++carrier;
  }
  for (const DrawCurvesCPU& curves : draw_->curves) {
    if (carrier < carrierVisible_.size() &&
        PathIsSameOrDescendant(curves.absPath, selPath_))
      carrierVisible_[carrier] = value;
    ++carrier;
  }
}

void Gui::isolateSelection() {
  if (!selectionHasRenderable()) return;
  for (size_t i = 0; i < meshVisible_.size(); ++i)
    meshVisible_[i] = PathIsSameOrDescendant(draw_->meshes[i].absPath, selPath_)
                          ? uint8_t{1} : uint8_t{0};
  size_t carrier = 0;
  for (const DrawPointsCPU& points : draw_->points) {
    carrierVisible_[carrier++] =
        PathIsSameOrDescendant(points.absPath, selPath_) ? uint8_t{1} : uint8_t{0};
  }
  for (const DrawCurvesCPU& curves : draw_->curves) {
    carrierVisible_[carrier++] =
        PathIsSameOrDescendant(curves.absPath, selPath_) ? uint8_t{1} : uint8_t{0};
  }
}

bool Gui::meshPurposeVisible(const std::string& purpose) const {
  if (purpose == "render") return showPurposeRender_;
  if (purpose == "proxy") return showPurposeProxy_;
  if (purpose == "guide") return showPurposeGuide_;
  return showPurposeDefault_;
}

bool Gui::meshVisibleForView(size_t meshIndex) const {
  if (!draw_ || meshIndex >= draw_->meshes.size()) return false;
  if (meshIndex < meshVisible_.size() && !meshVisible_[meshIndex]) return false;
  if (draw_->meshes[meshIndex].purpose == "proxy" &&
      draw_->meshes[meshIndex].proxyFallback) {
    return true;
  }
  return meshPurposeVisible(draw_->meshes[meshIndex].purpose);
}

size_t Gui::carrierIndexForPath(const std::string& path) const {
  if (!draw_) return static_cast<size_t>(-1);
  for (size_t i = 0; i < draw_->points.size(); ++i)
    if (draw_->points[i].absPath == path) return i;
  for (size_t i = 0; i < draw_->curves.size(); ++i)
    if (draw_->curves[i].absPath == path) return draw_->points.size() + i;
  return static_cast<size_t>(-1);
}

bool Gui::carrierVisibleForView(size_t carrierIndex) const {
  if (!draw_) return false;
  const size_t total = draw_->points.size() + draw_->curves.size();
  if (carrierIndex >= total) return false;
  if (carrierIndex < carrierVisible_.size() && !carrierVisible_[carrierIndex]) return false;
  const std::string& purpose = carrierIndex < draw_->points.size()
      ? draw_->points[carrierIndex].purpose
      : draw_->curves[carrierIndex - draw_->points.size()].purpose;
  return meshPurposeVisible(purpose);
}

void Gui::buildViewVisibilityMask() {
  viewVisible_.clear();
  if (!draw_ || draw_->meshes.empty()) return;
  viewVisible_.resize(draw_->meshes.size(), uint8_t{1});

  // Per-mesh frustum culling. Extract the frustum from the GL-convention P*V
  // (Z in [-1,1]) regardless of backend: light3d's Gribb-Hartmann near-plane
  // formula assumes that range, and the side/far planes are convention-neutral.
  // Instanced prototypes carry a scene-spanning union AABB so they rarely cull
  // here -- per-instance culling (A4) handles those; static-batched non-instanced
  // meshes have tight world AABBs and cull well.
  const bool doCull = cullEnabled_ && cam_;
  light3d::Frustum fr;
  if (doCull) {
    const light3d::Mat4 vp = cam_->proj(/*zeroToOneDepth=*/false) * cam_->view();
    fr = light3d::Frustum::fromViewProjection(vp);
  }
  // Non-instanced raster LOD: needs the same projected-size metric the instance
  // cull uses, and the shared box-proxy draw to collapse into.
  nonInstProxy_.xforms.clear();
  nonInstProxy_.colors.clear();
  nonInstProxy_.opacities.clear();
  nonInstProxy_.count = 0;
  const bool wireActive = wireCycle_ != 0 || mode_ == RenderMode::Wireframe;
  // Wire density is controlled continuously per projected edge in the shader.
  // Hard mesh/instance size culls make whole edge sets pop during dolly, so keep
  // only ordinary frustum culling while a wire mode is active.
  const bool lodOn = (rasterLodEnabled_ || dynamicRasterLodEnabled_) &&
                     !wireActive && cam_ && renderer_;
  RtLodCamera lodCam;
  if (lodOn) lodCam = buildRasterLodCam();
  // Backends without the shared box-proxy draw (VK) still size-cull; they just
  // cannot substitute a box, so a small mesh keeps drawing at full resolution.
  const bool lodProxy = lodOn && lodCam.proxyEnabled;

  // Per-mesh stats here; per-instance stats (visible instances + instanced tris)
  // are owned by cullInstances (dirty-gated, so not recomputed every frame).
  statVisibleMeshes_ = 0;
  statTotalInstances_ = 0;
  statNonInstTris_ = 0;
  statDrawCalls_ = 0;
  for (size_t i = 0; i < draw_->meshes.size(); ++i) {
    const DrawMeshCPU& m = draw_->meshes[i];
    const size_t ninst = m.instanceCount();
    statTotalInstances_ += ninst;
    bool vis = meshVisibleForView(i);
    if (vis && doCull) {
      const light3d::Vec3 mn{m.aabbMin[0], m.aabbMin[1], m.aabbMin[2]};
      const light3d::Vec3 mx{m.aabbMax[0], m.aabbMax[1], m.aabbMax[2]};
      if (fr.testAABB(mn, mx) == light3d::CullResult::Outside) vis = false;
    }
    viewVisible_[i] = vis ? uint8_t{1} : uint8_t{0};
    // Raster LOD for NON-INSTANCED meshes. The instance cull only ever looked at
    // meshes with instanceCount() > 0, so unique geometry got no LOD at all: its
    // only filter was this all-or-nothing frustum test. That is the whole of
    // Island's residual raster cost -- 55 M drawn tris of unique geometry against
    // 63 visible instances -- because the instanced side is already solved.
    //
    // Classify each one exactly like an instance: sub-pixel -> cull, small ->
    // collapse to the shared box proxy, else draw it. A decimated LOD for a big
    // unique mesh is a different (much larger) project; the box is the cheap 80%.
    if (vis && ninst == 0 && lodOn) {
      const float* lo = m.aabbMin;
      const float* hi = m.aabbMax;
      const bool degenerate = !(hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2]);
      if (!degenerate) {
        const float center[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]),
                                 0.5f * (lo[2] + hi[2])};
        const float ext[3] = {0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]),
                              0.5f * (hi[2] - lo[2])};
        const float radius =
            std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
        const float px = ProjectedRadiusPx(center, radius, lodCam);
        if (px < lodCam.cullPx) {
          vis = false;  // sub-pixel
        } else if (lodProxy && px < lodCam.fullPx) {
          // Non-instanced geometry is world-baked, so its AABB is already in world
          // space: the box proxy's object->world is the identity.
          static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
          float bx[12];
          BoxFitXform(kIdentity, lo, hi, bx);
          nonInstProxy_.xforms.insert(nonInstProxy_.xforms.end(), bx, bx + 12);
          const float* tint = m.flatColor;
          nonInstProxy_.colors.insert(nonInstProxy_.colors.end(), tint, tint + 3);
          vis = false;  // drawn as a box instead
        }
      }
      viewVisible_[i] = vis ? uint8_t{1} : uint8_t{0};
    }

    if (!vis) continue;
    ++statVisibleMeshes_;
    if (ninst > 0) {
      ++statDrawCalls_;  // one instanced draw per visible prototype mesh
    } else {
      // Triangle count from submesh metadata (DrawMeshCPU.indices is freed after
      // GPU upload, but submeshes survive).
      for (const DrawSubmesh& s : m.submeshes) statNonInstTris_ += s.indexCount / 3;
      statDrawCalls_ += m.submeshes.size();
    }
  }
  nonInstProxy_.count =
      static_cast<uint32_t>(nonInstProxy_.xforms.size() / 12);
  nonInstProxy_.hasColors = true;
}

Gui::~Gui() { joinCullWorker(); }

void Gui::joinCullWorker() {
  if (cullThread_.joinable()) cullThread_.join();
  cullRunning_.store(false);
  cullDone_.store(false);
  cullAborted_ = false;
}

namespace {
// Below this instance count the flat per-instance cull is already cheap, so the
// grid (build cost + memory) is not worth it. Matches the RT path's threshold.
constexpr std::uint32_t kInstGridMinInstances = 4096;
}  // namespace

// (Re)build instGrids_ -- one coarse spatial grid per instanced prototype -- when
// the scene changes. Read-only afterwards, so the cull worker can share them.
// `gen` is the worker's scene-generation snapshot (0 = synchronous path, no abort
// check); if it changes mid-build the caller is swapping the scene out from under
// us, so abort to avoid indexing freed/replaced instance data.
void Gui::ensureInstanceGrids(uint64_t gen) {
  if (instGridsFor_ == draw_) return;
  instGrids_.clear();
  instGridsFor_ = draw_;
  if (!draw_) return;
  instGrids_.resize(draw_->meshes.size());
  for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
    if (gen != 0 &&
        cullSceneGen_.load(std::memory_order_acquire) != gen) {
      instGrids_.clear();
      instGridsFor_ = nullptr;
      cullAborted_ = true;
      return;
    }
    const DrawMeshCPU& m = draw_->meshes[mi];
    if (m.instanceCount() < kInstGridMinInstances) continue;
    RtLodProto p;
    p.instanceXforms = m.instanceXforms.data();
    p.instanceCount = static_cast<std::uint32_t>(m.instanceCount());
    p.protoAabbMin = m.protoAabbMin;
    p.protoAabbMax = m.protoAabbMax;
    p.meshId = static_cast<std::uint32_t>(mi);
    BuildRtLodGrid(p, kInstGridMinInstances, &instGrids_[mi]);
  }
}

// Frustum-test one instanced mesh; append visible instances' 12-float o2w (+ 3-float
// color) to `out`. cullEnabled=false restores the full set. When lodCam.lodEnabled,
// also size-classify: sub-pixel (<cullPx) instances are dropped, and (when proxyOut
// is non-null + lodCam.proxyEnabled) small (<fullPx) instances become shared box
// proxies -- their box-fit o2w + tint are *appended* to proxyOut (never cleared
// here; the caller accumulates proxies across all prototypes). Static +
// snapshot-only, so it is safe to run on the worker thread.
void Gui::compactMeshInstances(const DrawMeshCPU& m, const light3d::Frustum& fr,
                               bool cullEnabled, const RtLodGrid* grid,
                               const RtLodCamera& lodCam, CullJobMesh* out,
                               CullJobMesh* proxyOut) {
  const size_t ninst = m.instanceCount();
  out->hasColors = m.instanceColors.size() == ninst * 3;
  out->hasOpacities = m.instanceOpacities.size() == ninst;
  out->xforms.clear();
  out->colors.clear();
  out->opacities.clear();
  if (!cullEnabled && !lodCam.prototypeLodEnabled) {
    out->xforms = m.instanceXforms;  // full set (a prior cull may have compacted)
    if (out->hasColors) out->colors = m.instanceColors;
    if (out->hasOpacities) out->opacities = m.instanceOpacities;
    out->count = static_cast<uint32_t>(ninst);
    out->lodCounts = {{out->count, 0, 0, 0}};
    return;
  }
  const float* lo = m.protoAabbMin;
  const float* hi = m.protoAabbMax;
  const bool hc = out->hasColors;
  const bool ho = out->hasOpacities;
  const bool lod = lodCam.lodEnabled;
  // Degenerate (unset) prototype AABB carries no size -> never size-cull/proxy it.
  const bool degenerate = !(hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2]);
  const bool doProxy = proxyOut && lodCam.proxyEnabled && !degenerate;
  std::array<std::vector<float>, 4> lodXforms;
  std::array<std::vector<float>, 4> lodColors;
  std::array<std::vector<float>, 4> lodOpacities;
  // Classify + append instance k. Frustum-tests its world AABB unless `assumeInside`
  // (its whole cell already tested Inside); when LOD is on the AABB is always built
  // (its projected size drives Full / Proxy / Cull).
  auto emit = [&](std::uint32_t k, bool assumeInside) {
    // The coarse grid may index instance data from an earlier scene snapshot
    // (a mesh is replaced wholesale by playback re-evaluation); never read past
    // the mesh's CURRENT instance count.
    if (k >= ninst) return;
    const float* o2w = &m.instanceXforms[k * 12];
    float center[3], radius = 0.0f;
    if (!assumeInside || (lod && !degenerate)) {
      float wmn[3] = {1e30f, 1e30f, 1e30f}, wmx[3] = {-1e30f, -1e30f, -1e30f};
      for (int c = 0; c < 8; ++c) {
        const float px = (c & 1) ? hi[0] : lo[0];
        const float py = (c & 2) ? hi[1] : lo[1];
        const float pz = (c & 4) ? hi[2] : lo[2];
        for (int r = 0; r < 3; ++r) {
          const float w = o2w[r * 4 + 0] * px + o2w[r * 4 + 1] * py +
                          o2w[r * 4 + 2] * pz + o2w[r * 4 + 3];
          wmn[r] = std::min(wmn[r], w);
          wmx[r] = std::max(wmx[r], w);
        }
      }
      if (cullEnabled && !assumeInside &&
          fr.testAABB({wmn[0], wmn[1], wmn[2]}, {wmx[0], wmx[1], wmx[2]}) ==
              light3d::CullResult::Outside)
        return;
      for (int r = 0; r < 3; ++r) center[r] = 0.5f * (wmn[r] + wmx[r]);
      const float dx = wmx[0] - wmn[0], dy = wmx[1] - wmn[1], dz = wmx[2] - wmn[2];
      radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (lod && !degenerate) {
      const float px = ProjectedRadiusPx(center, radius, lodCam);
      if (px < lodCam.cullPx) return;  // sub-pixel: drop entirely
      if (doProxy && px < lodCam.fullPx) {
        float bx[12];
        BoxFitXform(o2w, lo, hi, bx);  // unit box -> this instance's world AABB
        proxyOut->xforms.insert(proxyOut->xforms.end(), bx, bx + 12);
        const float* tint = hc ? &m.instanceColors[k * 3] : m.flatColor;
        proxyOut->colors.insert(proxyOut->colors.end(), tint, tint + 3);
        return;
      }
    }
    uint32_t chosenLod = 0;
    if (lodCam.prototypeLodEnabled && lodCam.prototypeErrorPx > 0.0f &&
        !degenerate) {
      const float radiusPx = ProjectedRadiusPx(center, radius, lodCam);
      // meshoptimizer reports normalized object-space error. Projecting twice
      // the bounding radius converts that to a conservative screen error.
      for (auto it = m.prototypeLods.rbegin(); it != m.prototypeLods.rend(); ++it) {
        if (it->level > 3) continue;
        const float screenError = 2.0f * radiusPx * it->objectError;
        if (screenError <= lodCam.prototypeErrorPx) {
          chosenLod = it->level;
          break;
        }
      }
    }
    auto& xf = lodXforms[chosenLod];
    xf.insert(xf.end(), o2w, o2w + 12);
    if (hc)
      lodColors[chosenLod].insert(lodColors[chosenLod].end(),
                                  &m.instanceColors[k * 3],
                                  &m.instanceColors[k * 3] + 3);
    if (ho) lodOpacities[chosenLod].push_back(m.instanceOpacities[k]);
  };

  if (grid && grid->valid) {
    // Cell-rejection path: skip whole off-screen cells, accept whole inside cells
    // without per-instance tests, and only per-instance test boundary cells. The
    // emitted set is identical to the flat loop (order differs, irrelevant for
    // instancing). Reserve from the non-rejected cells to avoid regrowth.
    std::uint32_t upper = 0;
    for (const RtLodGridCell& cell : grid->cells) {
      if (fr.testAABB({cell.wmn[0], cell.wmn[1], cell.wmn[2]},
                      {cell.wmx[0], cell.wmx[1], cell.wmx[2]}) ==
          light3d::CullResult::Outside)
        continue;
      upper += (lod && !degenerate && IsSubpixelAggregateCell(cell, lodCam))
                   ? 0u
                   : cell.count;
    }
    out->xforms.reserve(static_cast<size_t>(upper) * 12);
    if (hc) out->colors.reserve(static_cast<size_t>(upper) * 3);
    if (ho) out->opacities.reserve(static_cast<size_t>(upper));
    for (const RtLodGridCell& cell : grid->cells) {
      const light3d::CullResult cr =
          fr.testAABB({cell.wmn[0], cell.wmn[1], cell.wmn[2]},
                      {cell.wmx[0], cell.wmx[1], cell.wmx[2]});
      if (cr == light3d::CullResult::Outside) continue;
      if (lod && !degenerate && IsSubpixelAggregateCell(cell, lodCam)) {
        // Individually sub-pixel populations still carry visible aggregate
        // density (vegetation, debris, crowds). Preserve that density with
        // one bounded cell proxy instead of erasing every placement or
        // allocating one proxy per instance.
        if (doProxy) {
          static const float kIdentity[12] = {
              1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
          float bx[12];
          BoxFitXform(kIdentity, cell.wmn, cell.wmx, bx);
          proxyOut->xforms.insert(proxyOut->xforms.end(), bx, bx + 12);
          proxyOut->colors.insert(proxyOut->colors.end(), m.flatColor,
                                  m.flatColor + 3);
        }
        continue;
      }
      const bool inside = (cr == light3d::CullResult::Inside);
      for (std::uint32_t i = cell.begin; i < cell.begin + cell.count; ++i)
        emit(grid->order[i], inside);
    }
  } else {
    for (std::uint32_t k = 0; k < ninst; ++k) emit(k, /*assumeInside=*/false);
  }
  out->lodCounts = {{0, 0, 0, 0}};
  for (size_t level = 0; level < 4; ++level) {
    out->lodCounts[level] =
        static_cast<uint32_t>(lodXforms[level].size() / 12);
    out->xforms.insert(out->xforms.end(), lodXforms[level].begin(),
                       lodXforms[level].end());
    out->colors.insert(out->colors.end(), lodColors[level].begin(),
                       lodColors[level].end());
    out->opacities.insert(out->opacities.end(), lodOpacities[level].begin(),
                          lodOpacities[level].end());
  }
  out->count = static_cast<uint32_t>(out->xforms.size() / 12);
  if (proxyOut) {
    proxyOut->hasColors = true;  // every box proxy carries a tint
    proxyOut->count = static_cast<uint32_t>(proxyOut->xforms.size() / 12);
  }
}

// Build the LOD camera (thresholds + focal length) for the raster instance cull.
RtLodCamera Gui::buildRasterLodCam() const {
  RtLodCamera c;
  const bool wireActive = wireCycle_ != 0 || mode_ == RenderMode::Wireframe;
  c.lodEnabled = (rasterLodEnabled_ || dynamicRasterLodEnabled_) && !wireActive;
  c.prototypeLodEnabled = dynamicRasterLodEnabled_ && !wireActive;
  c.prototypeErrorPx = dynamicRasterLodErrorPx_;
  // Box proxies have no authored wire topology. Wire modes disable size LOD
  // altogether above; the shader's projected-edge fade controls density without
  // hard full/proxy/cull transitions during dolly.
  c.proxyEnabled =
      rasterLodEnabled_ && !wireActive && renderer_ &&
      renderer_->supportsProxyDraw();
  c.frustumCull = true;
  c.fullPx = rasterLodFullPx_;
  c.cullPx = rasterLodCullPx_;
  c.bandFrac = 0.0f;
  const light3d::Mat4 vp = cam_->proj(/*zeroToOneDepth=*/false) * cam_->view();
  std::memcpy(c.viewProj.m, vp.m, sizeof(c.viewProj.m));
  const light3d::Vec3 eye = cam_->eye();
  c.eye = eye;
  c.forward = light3d::normalize(cam_->target() - eye);
  c.nearPlane = cam_->nearPlane();
  // focalPx = (viewportH * 0.5) / tan(fovY/2) = 0.5 * H * proj[1][1]. Matches the
  // RT path's vpH_ * proj_[5] derivation.
  const light3d::Mat4 proj = cam_->proj(/*zeroToOneDepth=*/false);
  c.focalPx = 0.5f * static_cast<float>(viewportH_) *
              (proj.m[5] != 0.0f ? proj.m[5] : 1.0f);
  return c;
}

// Worker thread: compact every visible instanced mesh into cullJobResult_ from the
// main-thread snapshots (cullJob*). No GPU calls, no live Gui/camera reads.
void Gui::cullWorkerMain() {
  const uint64_t gen = cullJobGen_;
  const DrawScene* d = cullJobDraw_;
  // Build the instance grids here (off the main thread) so the one-time build cost
  // on a huge scene -- O(instances) over the mega-prototypes -- never freezes the
  // UI; the first worker run is slower, later runs reap the cell-rejection speedup.
  ensureInstanceGrids(gen);
  if (cullAborted_) { cullDone_.store(true, std::memory_order_release); return; }
  const light3d::Frustum fr = light3d::Frustum::fromViewProjection(cullJobVP_);
  cullJobResult_.clear();
  cullJobProxy_.xforms.clear();
  cullJobProxy_.colors.clear();
  cullJobProxy_.opacities.clear();
  cullJobProxy_.count = 0;
  size_t visInstances = 0, instTris = 0;
  for (size_t mi = 0; mi < d->meshes.size(); ++mi) {
    // Abort at a mesh boundary if the scene was replaced under us (the main
    // thread joins before swapping, so this is defense-in-depth -- it bounds the
    // worker's tail so that join is fast).
    if (cullSceneGen_.load(std::memory_order_acquire) != gen) {
      cullAborted_ = true;
      cullDone_.store(true, std::memory_order_release);
      return;
    }
    const DrawMeshCPU& m = d->meshes[mi];
    if (m.instanceCount() == 0) continue;
    const bool meshVisible =
        mi >= cullJobViewVisible_.size() || cullJobViewVisible_[mi] != 0;
    if (!meshVisible) continue;
    size_t protoTris = 0;
    for (const DrawSubmesh& s : m.submeshes) protoTris += s.indexCount / 3;
    CullJobMesh r;
    r.meshIndex = mi;
    const RtLodGrid* grid =
        (cullJobGrids_ && mi < cullJobGrids_->size()) ? &(*cullJobGrids_)[mi] : nullptr;
    compactMeshInstances(m, fr, cullJobEnabled_, grid, cullJobLodCam_, &r,
                         &cullJobProxy_);
    visInstances += r.count;
    instTris += protoTris * r.lodCounts[0];
    for (const DrawPrototypeLodCPU& level : m.prototypeLods)
      if (level.level < r.lodCounts.size())
        instTris += (level.indices.size() / 3) * r.lodCounts[level.level];
    cullJobResult_.push_back(std::move(r));
  }
  cullJobVisInstances_ = visInstances;
  cullJobInstTris_ = instTris;
  cullDone_.store(true, std::memory_order_release);
}

// Synchronous path (headless / cullAsync_ off): compact + apply inline, exact
// original behavior so screenshots stay deterministic.
void Gui::cullInstancesSync() {
  const light3d::Mat4 vp = cam_->proj(/*zeroToOneDepth=*/false) * cam_->view();
  bool changed = !lastCullValid_ || cullEnabled_ != lastCullEnabled_ ||
                 draw_ != lastCullDraw_ || rasterLodEnabled_ != lastCullRasterLod_ ||
                 (wireCycle_ != 0 || mode_ == RenderMode::Wireframe) !=
                     lastCullWireMode_;
  if (!changed)
    for (int i = 0; i < 16; ++i)
      if (vp.m[i] != lastCullVP_[i]) { changed = true; break; }
  if (!changed) return;
  std::memcpy(lastCullVP_, vp.m, sizeof(lastCullVP_));
  lastCullValid_ = true;
  lastCullEnabled_ = cullEnabled_;
  lastCullDraw_ = draw_;
  lastCullRasterLod_ = rasterLodEnabled_;
  lastCullWireMode_ = wireCycle_ != 0 || mode_ == RenderMode::Wireframe;
  ensureInstanceGrids(/*gen=*/0);  // synchronous: no worker snapshot to guard
  const RtLodCamera lodCam = buildRasterLodCam();
  proxyResult_.xforms.clear();
  proxyResult_.colors.clear();
  proxyResult_.opacities.clear();
  proxyResult_.count = 0;

  const light3d::Frustum fr = light3d::Frustum::fromViewProjection(vp);
  size_t visInstances = 0, instTris = 0;
  CullJobMesh r;
  for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
    const DrawMeshCPU& m = draw_->meshes[mi];
    if (m.instanceCount() == 0) continue;
    const bool meshVisible = mi >= viewVisible_.size() || viewVisible_[mi] != 0;
    if (!meshVisible) continue;
    size_t protoTris = 0;
    for (const DrawSubmesh& s : m.submeshes) protoTris += s.indexCount / 3;
    const RtLodGrid* grid = mi < instGrids_.size() ? &instGrids_[mi] : nullptr;
    compactMeshInstances(m, fr, cullEnabled_, grid, lodCam, &r, &proxyResult_);
    // Route the GPU upload to the render thread when threaded (else inline).
    uint32_t cnt = r.count;
    const auto lodCounts = r.lodCounts;
    bool hc = r.hasColors;
    bool ho = r.hasOpacities;
    std::vector<float> xf = r.xforms, col = r.colors, op = r.opacities;
    gpu([this, miCap = mi, cntCap = cnt, lodCounts, hcCap = hc, hoCap = ho,
         xfCap = std::move(xf), colCap = std::move(col),
         opCap = std::move(op)]() mutable {
      (void)cntCap;
      renderer_->updateInstanceLodVisibility(
          miCap, xfCap.data(), hcCap ? colCap.data() : nullptr,
          hoCap ? opCap.data() : nullptr, lodCounts);
    });
    visInstances += r.count;
    instTris += protoTris * r.lodCounts[0];
    for (const DrawPrototypeLodCPU& level : m.prototypeLods)
      if (level.level < r.lodCounts.size())
        instTris += (level.indices.size() / 3) * r.lodCounts[level.level];
  }
  // Upload the accumulated box proxies (one shared instanced draw). Always called
  // (count 0 when LOD off) so a previous frame's proxies are cleared.
  uploadProxies(&proxyResult_);
  statVisibleInstances_ = visInstances;
  statInstTris_ = instTris;
}

// Union of the non-instanced proxies (rebuilt every frame by
// buildViewVisibilityMask) and the instance cull's proxies (dirty-gated, so they
// arrive only on the frames the cull actually reran). Both feed the single shared
// box-proxy instanced draw, so they have to be uploaded together; the content
// compare keeps the steady state free.
void Gui::uploadProxies(CullJobMesh* instProxy) {
  std::vector<float> xf = nonInstProxy_.xforms;
  std::vector<float> col = nonInstProxy_.colors;
  if (instProxy) {
    xf.insert(xf.end(), instProxy->xforms.begin(), instProxy->xforms.end());
    col.insert(col.end(), instProxy->colors.begin(), instProxy->colors.end());
    instProxy->xforms.clear();
    instProxy->colors.clear();
    instProxy->count = 0;
  }
  statProxyInstances_ = xf.size() / 12;
  if (lastProxyValid_ && xf == lastProxyXforms_ && col == lastProxyColors_) return;
  lastProxyXforms_ = xf;
  lastProxyColors_ = col;
  lastProxyValid_ = true;
  const uint32_t pc = static_cast<uint32_t>(xf.size() / 12);
  gpu([this, pcCap = pc, xfCap = std::move(xf), colCap = std::move(col)]() mutable {
    renderer_->updateProxyInstances(xfCap.data(), colCap.data(), pcCap);
  });
}

void Gui::cullInstances() {
  if (!draw_ || !renderer_ || !cam_) return;
  // Uploaded progressive chunks already contain their full instance set. Draw
  // them as-is until Complete makes the scene immutable and async culling can
  // safely resume.
  if (sceneMutating_) return;
  // Only instanced prototypes carry instanceXforms; nothing to do otherwise --
  // except the box proxies raster LOD substituted for small NON-instanced meshes,
  // which still need their upload (a scene can be entirely non-instanced).
  bool anyInstanced = false;
  for (const auto& m : draw_->meshes) {
    if (m.instanceCount() > 0) { anyInstanced = true; break; }
  }
  if (!anyInstanced) {
    uploadProxies(nullptr);
    if (!cullRunning_.load()) { statVisibleInstances_ = 0; statInstTris_ = 0; }
    return;
  }
  if (!cullAsync_) { cullInstancesSync(); return; }

  // (1) Apply a finished worker result on the main thread (the GPU upload).
  if (cullDone_.load(std::memory_order_acquire)) {
    if (cullThread_.joinable()) cullThread_.join();
    if (cullAborted_) {
      // The scene generation changed while the worker ran (defensive: the App
      // also joins before swapping). Discard the partial compaction -- it may
      // reference the outgoing scene -- and re-cull next frame.
      cullAborted_ = false;
      cullJobResult_.clear();
      cullJobProxy_.xforms.clear();
      cullJobProxy_.colors.clear();
      cullJobProxy_.opacities.clear();
      cullJobProxy_.count = 0;
      cullDone_.store(false);
      cullRunning_.store(false);
      lastCullValid_ = false;
      return;
    }
    for (CullJobMesh& r : cullJobResult_) {
      size_t mi = r.meshIndex;
      uint32_t cnt = r.count;
      const auto lodCounts = r.lodCounts;
      bool hc = r.hasColors;
      bool ho = r.hasOpacities;
      std::vector<float> xf = std::move(r.xforms), col = std::move(r.colors),
                         op = std::move(r.opacities);
      gpu([this, miCap = mi, cntCap = cnt, lodCounts, hcCap = hc, hoCap = ho,
           xfCap = std::move(xf), colCap = std::move(col),
           opCap = std::move(op)]() mutable {
        (void)cntCap;
        renderer_->updateInstanceLodVisibility(
            miCap, xfCap.data(), hcCap ? colCap.data() : nullptr,
            hoCap ? opCap.data() : nullptr, lodCounts);
      });
    }
    // Apply the accumulated box proxies (shared instanced draw).
    uploadProxies(&cullJobProxy_);
    statVisibleInstances_ = cullJobVisInstances_;
    statInstTris_ = cullJobInstTris_;
    cullDone_.store(false);
    cullRunning_.store(false);
  }

  // (2) (Re)launch the worker when the view / cull toggle / scene changed. While a
  // worker runs the renderer keeps the previous visible set, so the UI never blocks.
  const light3d::Mat4 vp = cam_->proj(/*zeroToOneDepth=*/false) * cam_->view();
  bool changed = !lastCullValid_ || cullEnabled_ != lastCullEnabled_ ||
                 draw_ != lastCullDraw_ || rasterLodEnabled_ != lastCullRasterLod_ ||
                 (wireCycle_ != 0 || mode_ == RenderMode::Wireframe) !=
                     lastCullWireMode_;
  if (!changed)
    for (int i = 0; i < 16; ++i)
      if (vp.m[i] != lastCullVP_[i]) { changed = true; break; }
  if (!changed || cullRunning_.load()) return;  // up to date, or worker busy
  lastCullRasterLod_ = rasterLodEnabled_;
  lastCullWireMode_ = wireCycle_ != 0 || mode_ == RenderMode::Wireframe;
  std::memcpy(lastCullVP_, vp.m, sizeof(lastCullVP_));
  lastCullValid_ = true;
  lastCullEnabled_ = cullEnabled_;
  lastCullDraw_ = draw_;
  // Snapshot worker inputs: instanceXforms/protoAabb are static after load,
  // viewVisible_ is copied, so the worker races nothing the main thread mutates.
  // instGrids_ is built by the worker (cullWorkerMain) on its first run and is
  // read-only thereafter, so sharing it by pointer is race-free; building there
  // keeps the one-time O(instances) cost off the main thread.
  cullJobVP_ = vp;
  cullJobViewVisible_ = viewVisible_;
  cullJobEnabled_ = cullEnabled_;
  cullJobDraw_ = draw_;
  cullJobGen_ = cullSceneGen_.load(std::memory_order_relaxed);
  cullAborted_ = false;
  cullJobGrids_ = &instGrids_;
  cullJobLodCam_ = buildRasterLodCam();  // camera read on main, used by the worker
  cullRunning_.store(true);
  cullDone_.store(false);
  cullThread_ = std::thread(&Gui::cullWorkerMain, this);
}

void Gui::cycleNavigationHelpMode() {
  // Default None; the first F1 exposes the complete reference, then the compact
  // drag-time feedback, then returns to an unobstructed viewport.
  if (navigationHelpMode_ == NavigationHelpMode::None)
    navigationHelpMode_ = NavigationHelpMode::Full;
  else if (navigationHelpMode_ == NavigationHelpMode::Full)
    navigationHelpMode_ = NavigationHelpMode::Simple;
  else
    navigationHelpMode_ = NavigationHelpMode::None;
}

void Gui::drawNavigationOverlay(const ImVec2& imageMin, const ImVec2& imageMax) {
  const bool full = navigationHelpMode_ == NavigationHelpMode::Full;
  const bool simple = navigationHelpMode_ == NavigationHelpMode::Simple;
  if (navigationHelpMode_ == NavigationHelpMode::None || (simple && navMode_ == 0))
    return;

  const char* title = "Viewport navigation";
  const std::string modeLine = std::string("Mode: ") + NavModeLabel(navMode_);
  const char* lineOrbit = "Alt+LMB Orbit";
  const char* linePan = "Alt+MMB / Shift+Alt+LMB Pan";
  const char* lineDolly = "Alt+RMB / Wheel Dolly";
  const char* lineWalk = "W / S Move eye + pivot (Up/Down aliases, Shift = fast)";
  const char* lineSelect = "[ / ] Prev/Next selection";
  const char* lineHistory = "Alt+Left / Alt+Right Selection back/forward";
  const char* lineFrame = "F Frame selected   A Frame all   0 Home";
  const char* lineViews = "1/Shift+1 Front/Back   3/Shift+3 Right/Left";
  const char* lineViews2 = "7/Shift+7 Top/Bottom   5 Isometric   F1 Cycle help";
  const char* lineBookmarks = "Ctrl+1..3 Recall   Ctrl+Shift+1..3 Save";

  float maxWidth = ImGui::CalcTextSize(title).x;
  maxWidth = std::max(maxWidth, ImGui::CalcTextSize(modeLine.c_str()).x);
  if (full) {
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineOrbit).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(linePan).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineDolly).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineWalk).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineSelect).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineHistory).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineFrame).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineViews).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineViews2).x);
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lineBookmarks).x);
  }

  const float pad = ImGui::GetStyle().FramePadding.x * 1.2f;
  const float lineH = ImGui::GetTextLineHeightWithSpacing();
  const int lines = full ? 11 : 2;
  const ImVec2 pos(imageMin.x + 12.0f, imageMin.y + 12.0f);
  const ImVec2 boxSize(maxWidth + pad * 2.0f, lineH * static_cast<float>(lines) + pad * 2.0f);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 boxMin = pos;
  const ImVec2 boxMax(std::min(pos.x + boxSize.x, imageMax.x - 8.0f),
                      std::min(pos.y + boxSize.y, imageMax.y - 8.0f));
  dl->AddRectFilled(boxMin, boxMax, IM_COL32(18, 20, 24, 196), 8.0f);
  dl->AddRect(boxMin, boxMax, IM_COL32(110, 120, 135, 220), 8.0f);

  float y = boxMin.y + pad;
  const float x = boxMin.x + pad;
  dl->AddText(ImVec2(x, y), IM_COL32(245, 245, 245, 255), title);
  y += lineH;
  dl->AddText(ImVec2(x, y),
              navMode_ != 0 ? IM_COL32(255, 220, 120, 255) : IM_COL32(180, 200, 220, 255),
              modeLine.c_str());
  y += lineH;
  if (full) {
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineOrbit);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), linePan);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineDolly);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineWalk);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineSelect);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineHistory);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(220, 220, 220, 255), lineFrame);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(200, 215, 255, 255), lineViews);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(200, 215, 255, 255), lineViews2);
    y += lineH;
    dl->AddText(ImVec2(x, y), IM_COL32(190, 225, 205, 255), lineBookmarks);
  }

  // Axis indicator in bottom-right corner
  {
    const float axSize = 60.0f;
    const float axPad = 12.0f;
    const ImVec2 axOrigin(imageMax.x - axPad - axSize, imageMax.y - axPad - axSize);
    if (cam_) {
      const float L = axSize * 0.25f;
      const float oX = axOrigin.x + axSize * 0.5f;
      const float oY = axOrigin.y + axSize * 0.5f;
      const ImU32 colX = IM_COL32(230, 50, 50, 220);
      const ImU32 colY = IM_COL32(50, 220, 60, 220);
      const ImU32 colZ = IM_COL32(80, 130, 255, 220);
      dl->AddLine(ImVec2(oX, oY), ImVec2(oX + L, oY), colX, 2.5f);
      dl->AddText(ImVec2(oX + L + 2, oY - 5), colX, "X");
      dl->AddLine(ImVec2(oX, oY), ImVec2(oX, oY - L), colY, 2.5f);
      dl->AddText(ImVec2(oX - 3, oY - L - 12), colY, "Y");
      dl->AddLine(ImVec2(oX, oY), ImVec2(oX - L * 0.5f, oY + L * 0.5f), colZ, 2.5f);
      dl->AddText(ImVec2(oX - L * 0.5f - 8, oY + L * 0.5f + 2), colZ, "Z");
    }
  }
}

int Gui::pickMesh(float px, float py, int vpW, int vpH,
                  float* hitDistance) const {
  if (hitDistance) *hitDistance = 1e30f;
  if (!cam_ || !renderer_ || !draw_ || draw_->meshes.empty() || vpW <= 0 ||
      vpH <= 0) {
    return -1;
  }

  // Build a world-space ray through the clicked pixel. The displayed image is
  // upright on both backends (GL flips via UVs, VK via a negative-height
  // viewport), so screen-top maps to NDC y = +1 regardless of flipViewportV.
  const bool z01 = renderer_->caps().usesZeroToOneDepth;
  const light3d::Mat4 V = cam_->view();
  const light3d::Mat4 P = cam_->proj(z01);
  const light3d::Mat4 invVP = (P * V).inverse();

  const float ndcx = 2.0f * (px / static_cast<float>(vpW)) - 1.0f;
  const float ndcy = 1.0f - 2.0f * (py / static_cast<float>(vpH));

  auto unproject = [&](float nz) -> light3d::Vec3 {
    const float* m = invVP.m;  // column-major: m[col*4 + row]
    const float ox = m[0] * ndcx + m[4] * ndcy + m[8] * nz + m[12];
    const float oy = m[1] * ndcx + m[5] * ndcy + m[9] * nz + m[13];
    const float oz = m[2] * ndcx + m[6] * ndcy + m[10] * nz + m[14];
    const float ow = m[3] * ndcx + m[7] * ndcy + m[11] * nz + m[15];
    const float inv = (ow != 0.0f) ? 1.0f / ow : 1.0f;
    return {ox * inv, oy * inv, oz * inv};
  };

  const light3d::Vec3 nearW = unproject(z01 ? 0.0f : -1.0f);
  const light3d::Vec3 farW = unproject(1.0f);
  const light3d::Vec3 ro = cam_->eye();
  const light3d::Vec3 rd = light3d::normalize(farW - nearW);

  // Slab test against a world-space AABB. Return the entry distance as well: the
  // next-loader deliberately releases static CPU triangles after GPU upload, so
  // their retained bounds are the low-memory object-picking representation.
  const float invdx = (rd.x != 0.0f) ? 1.0f / rd.x : 1e30f;
  const float invdy = (rd.y != 0.0f) ? 1.0f / rd.y : 1e30f;
  const float invdz = (rd.z != 0.0f) ? 1.0f / rd.z : 1e30f;
  auto hitAabb = [&](const float mn[3], const float mx[3], float tMax,
                     float* tNear) -> bool {
    float t1 = (mn[0] - ro.x) * invdx, t2 = (mx[0] - ro.x) * invdx;
    float tmin = std::min(t1, t2), tmax = std::max(t1, t2);
    t1 = (mn[1] - ro.y) * invdy; t2 = (mx[1] - ro.y) * invdy;
    tmin = std::max(tmin, std::min(t1, t2));
    tmax = std::min(tmax, std::max(t1, t2));
    t1 = (mn[2] - ro.z) * invdz; t2 = (mx[2] - ro.z) * invdz;
    tmin = std::max(tmin, std::min(t1, t2));
    tmax = std::min(tmax, std::max(t1, t2));
    const float entry = std::max(tmin, 0.0f);
    if (tmax < entry || entry >= tMax) return false;
    if (tNear) *tNear = entry;
    return true;
  };

  // Moller-Trumbore ray/triangle in world space.
  auto rayTri = [&](const light3d::Vec3& a, const light3d::Vec3& b,
                    const light3d::Vec3& c, float& tOut) -> bool {
    const light3d::Vec3 e1 = b - a, e2 = c - a;
    const light3d::Vec3 pv = light3d::cross(rd, e2);
    const float det = light3d::dot(e1, pv);
    if (std::fabs(det) < 1e-9f) return false;  // parallel
    const float invDet = 1.0f / det;
    const light3d::Vec3 tv = ro - a;
    const float u = light3d::dot(tv, pv) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const light3d::Vec3 qv = light3d::cross(tv, e1);
    const float vv = light3d::dot(rd, qv) * invDet;
    if (vv < 0.0f || u + vv > 1.0f) return false;
    const float t = light3d::dot(e2, qv) * invDet;
    if (t <= 1e-5f) return false;  // behind the ray origin
    tOut = t;
    return true;
  };

  auto compactPoint = [](const DrawMeshCPU& m, uint32_t i) {
    const size_t base = static_cast<size_t>(i) * 3;
    constexpr float kInvU16 = 1.0f / 65535.0f;
    return light3d::Vec3{
        m.pickQuantMin[0] + m.pickQuantExtent[0] *
                                (float(m.pickPositions[base]) * kInvU16),
        m.pickQuantMin[1] + m.pickQuantExtent[1] *
                                (float(m.pickPositions[base + 1]) * kInvU16),
        m.pickQuantMin[2] + m.pickQuantExtent[2] *
                                (float(m.pickPositions[base + 2]) * kInvU16)};
  };

  int best = -1;
  float bestT = 1e30f;
  int bestBounds = -1;
  float bestBoundsT = 1e30f;
  for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
    // Skip hidden meshes (they aren't drawn, so they shouldn't be pickable).
    if (!meshVisibleForView(mi)) continue;
    const DrawMeshCPU& m = draw_->meshes[mi];
    // These are viewer-generated boxes, not authored USD mesh prims.  Picking
    // their scene-spanning bounds made a click on a tree select the large
    // composition/budget container instead of the actual source mesh.
    if (m.absPath == "/__composition_preview_bounds" ||
        m.name == "__gpu_budget_proxy__") {
      continue;
    }
    const bool gpuDeformed = !m.jointIdx.empty() || !m.morphDeltaHalf.empty();
    const float* pickMin = m.hasPosedPickAabb
                               ? m.posedPickAabbMin
                               : (gpuDeformed ? m.restAabbMin : m.aabbMin);
    const float* pickMax = m.hasPosedPickAabb
                               ? m.posedPickAabbMax
                               : (gpuDeformed ? m.restAabbMax : m.aabbMax);
    bool boundsHit = false;
    if (m.instanceCount() > 0) {
      // aabbMin/Max is the union of all placements and is therefore much too
      // large for picking an instanced prototype. Test each prototype-local
      // box after applying its authored 3x4 object-to-world transform.
      for (size_t ii = 0; ii < m.instanceCount(); ++ii) {
        const float* x = &m.instanceXforms[ii * 12];
        float imn[3] = {1e30f, 1e30f, 1e30f};
        float imx[3] = {-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
          const float lx = (c & 1) ? pickMax[0] : pickMin[0];
          const float ly = (c & 2) ? pickMax[1] : pickMin[1];
          const float lz = (c & 4) ? pickMax[2] : pickMin[2];
          for (int r = 0; r < 3; ++r) {
            const float w = x[r * 4 + 0] * lx + x[r * 4 + 1] * ly +
                            x[r * 4 + 2] * lz + x[r * 4 + 3];
            imn[r] = std::min(imn[r], w);
            imx[r] = std::max(imx[r], w);
          }
        }
        float instanceT = 0.0f;
        if (hitAabb(imn, imx, bestT, &instanceT)) {
          boundsHit = true;
          bool exactInstanceHit = false;
          const bool hasFullTriangles =
              !m.vertices.empty() && m.indices.size() >= 3;
          const bool hasCompactTriangles =
              !m.pickPositions.empty() && m.pickIndices.size() >= 3;
          if (!gpuDeformed && (hasFullTriangles || hasCompactTriangles)) {
            auto instancePoint = [&](const light3d::Vec3& v) {
              return light3d::Vec3{
                  x[0] * v.x + x[1] * v.y + x[2] * v.z + x[3],
                  x[4] * v.x + x[5] * v.y + x[6] * v.z + x[7],
                  x[8] * v.x + x[9] * v.y + x[10] * v.z + x[11]};
            };
            const std::vector<uint32_t>& pi =
                hasFullTriangles ? m.indices : m.pickIndices;
            const size_t pointCount = hasFullTriangles
                                          ? m.vertices.size()
                                          : m.pickPositions.size() / 3;
            auto point = [&](uint32_t i) {
              return hasFullTriangles
                         ? light3d::Vec3{m.vertices[i].px, m.vertices[i].py,
                                         m.vertices[i].pz}
                         : compactPoint(m, i);
            };
            for (size_t ti = 0; ti + 2 < pi.size(); ti += 3) {
              const uint32_t ia = pi[ti + 0], ib = pi[ti + 1], ic = pi[ti + 2];
              if (ia >= pointCount || ib >= pointCount || ic >= pointCount)
                continue;
              float triangleT = 0.0f;
              if (rayTri(instancePoint(point(ia)), instancePoint(point(ib)),
                         instancePoint(point(ic)), triangleT) &&
                  triangleT < bestT) {
                bestT = triangleT;
                best = static_cast<int>(mi);
                exactInstanceHit = true;
              }
            }
          }
          // Retain bounds-only picking for released/deformed prototype data,
          // but never let an exact miss on retained triangles turn the whole
          // instance box into a selectable solid.
          if ((hasFullTriangles || hasCompactTriangles) && !gpuDeformed &&
              !exactInstanceHit) {
            continue;
          }
          if (!exactInstanceHit && instanceT < bestBoundsT) {
            bestBoundsT = instanceT;
            bestBounds = static_cast<int>(mi);
          }
        }
      }
    } else {
      float boundsT = 0.0f;
      boundsHit = hitAabb(pickMin, pickMax, bestT, &boundsT);
      const bool hasExactStatic =
          !gpuDeformed && ((!m.vertices.empty() && m.indices.size() >= 3) ||
                           (!m.pickPositions.empty() &&
                            m.pickIndices.size() >= 3));
      if (boundsHit && !hasExactStatic && boundsT < bestBoundsT) {
        bestBoundsT = boundsT;
        bestBounds = static_cast<int>(mi);
      }
    }
    if (!boundsHit) continue;

    // Static next meshes normally arrive here after FreeMeshSurfaceCPU, so use
    // their compact picking triangles. Only GPU-deformed meshes and legacy
    // carriers without either triangle representation retain the bounds fallback:
    // their CPU vertices are the rest pose, not necessarily the shape on screen.
    const bool hasFullTriangles = !m.vertices.empty() && m.indices.size() >= 3;
    const bool hasCompactTriangles =
        !m.pickPositions.empty() && m.pickIndices.size() >= 3;
    if (m.instanceCount() > 0 || (!hasFullTriangles && !hasCompactTriangles) ||
        gpuDeformed)
      continue;

    light3d::Mat4 W;
    for (int k = 0; k < 16; ++k) W.m[k] = m.world[k];
    const std::vector<uint32_t>& pi =
        hasFullTriangles ? m.indices : m.pickIndices;
    const size_t pointCount =
        hasFullTriangles ? m.vertices.size() : m.pickPositions.size() / 3;
    auto point = [&](uint32_t i) {
      return hasFullTriangles
                 ? light3d::Vec3{m.vertices[i].px, m.vertices[i].py,
                                 m.vertices[i].pz}
                 : compactPoint(m, i);
    };
    for (size_t i = 0; i + 2 < pi.size(); i += 3) {
      if (pi[i] >= pointCount || pi[i + 1] >= pointCount ||
          pi[i + 2] >= pointCount)
        continue;
      const light3d::Vec3 a = light3d::transformPoint(W, point(pi[i]));
      const light3d::Vec3 b = light3d::transformPoint(W, point(pi[i + 1]));
      const light3d::Vec3 c = light3d::transformPoint(W, point(pi[i + 2]));
      float t;
      if (rayTri(a, b, c, t) && t < bestT) {
        bestT = t;
        best = static_cast<int>(mi);
      }
    }
  }
  const int result = best >= 0 ? best : bestBounds;
  // A negative distance explicitly denotes a bounds-only fallback.  It is
  // useful to callers that have a real selection buffer for curves: a broad
  // cached mesh bound must not occlude a curve merely because the camera is
  // currently inside that bound (which makes its entry distance zero).
  if (hitDistance) *hitDistance = best >= 0 ? bestT : -bestBoundsT;
  return result;
}

std::string Gui::pickCarrierPath(float px, float py, int vpW, int vpH,
                                 float* hitDistance) const {
  if (hitDistance) *hitDistance = 0.0f;
  if (!draw_ || !cam_ || !renderer_ || vpW <= 0 || vpH <= 0) return {};
  // This is deliberately renderer-independent.  Both the GL and Vulkan
  // raster backends retain the same tessellated carrier data; only the depth
  // convention used to unproject/project the local ID/depth buffer differs
  // (GL [-1,1], Vulkan [0,1]).
  float meshT = 1.0e30f;
  const int mesh = pickMesh(px, py, vpW, vpH, &meshT);

  const bool z01 = renderer_->caps().usesZeroToOneDepth;
  const light3d::Mat4 invVP =
      (cam_->proj(z01) * cam_->view()).inverse();
  const float ndcx = 2.0f * px / static_cast<float>(vpW) - 1.0f;
  const float ndcy = 1.0f - 2.0f * py / static_cast<float>(vpH);
  auto unproject = [&](float nz) {
    const float* m = invVP.m;
    const float x = m[0] * ndcx + m[4] * ndcy + m[8] * nz + m[12];
    const float y = m[1] * ndcx + m[5] * ndcy + m[9] * nz + m[13];
    const float z = m[2] * ndcx + m[6] * ndcy + m[10] * nz + m[14];
    const float w = m[3] * ndcx + m[7] * ndcy + m[11] * nz + m[15];
    const float inv = w != 0.0f ? 1.0f / w : 1.0f;
    return light3d::Vec3{x * inv, y * inv, z * inv};
  };
  const light3d::Vec3 ro = cam_->eye();
  const light3d::Vec3 rd = light3d::normalize(
      unproject(1.0f) - unproject(z01 ? 0.0f : -1.0f));
  const float inv[3] = {
      rd.x != 0.0f ? 1.0f / rd.x : 1.0e30f,
      rd.y != 0.0f ? 1.0f / rd.y : 1.0e30f,
      rd.z != 0.0f ? 1.0f / rd.z : 1.0e30f};
  auto hit = [&](const float mn[3], const float mx[3], float* tOut) {
    float t0 = -1.0e30f, t1 = 1.0e30f;
    const float o[3] = {ro.x, ro.y, ro.z};
    const float d[3] = {inv[0], inv[1], inv[2]};
    for (int k = 0; k < 3; ++k) {
      float a = (mn[k] - o[k]) * d[k];
      float b = (mx[k] - o[k]) * d[k];
      if (a > b) std::swap(a, b);
      t0 = std::max(t0, a);
      t1 = std::min(t1, b);
    }
    if (t1 < std::max(t0, 0.0f)) return false;
    *tOut = std::max(t0, 0.0f);
    return true;
  };
  // Curves are rendered as camera-facing quads, so testing only their carrier
  // AABB is both too broad (the enclosing trunk/foliage mesh wins) and too
  // shallow (a click can land on a curve behind the AABB entry point).  Test
  // the retained tessellated segments in screen space, then use the segment's
  // world-space point to order it against mesh hits.  This is only performed
  // for a mouse click, not per frame, and remains cheap for the capped Island
  // debug scenes.
  const light3d::Mat4 VP = cam_->proj(z01) * cam_->view();
  auto project = [&](const light3d::Vec3& p, float& sx, float& sy,
                     float& ndcZ) -> bool {
    const float* m = VP.m;
    const float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    const float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    const float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (!std::isfinite(w) || w <= 1.0e-7f || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z)) return false;
    const float iw = 1.0f / w;
    sx = (x * iw * 0.5f + 0.5f) * static_cast<float>(vpW);
    sy = (1.0f - (y * iw * 0.5f + 0.5f)) * static_cast<float>(vpH);
    ndcZ = z * iw;
    return std::isfinite(sx) && std::isfinite(sy) && std::isfinite(ndcZ);
  };
  // Full viewport-sized curve selection buffer.  This is intentionally keyed
  // by raster pixel, rather than by a ray/AABB approximation: the carrier ID
  // and linear ray depth are written for every covered pixel in the same
  // viewport extent that the user sees.  Mesh bounds are not consulted here;
  // the raster mesh picker is only used when this buffer has no curve ID.
  const size_t pixelCount = static_cast<size_t>(vpW) *
                            static_cast<size_t>(vpH);
  // Keep the temporary ID/depth attachment bounded.  A normal 4K viewport is
  // well below this limit; for an unusually large remote/headless viewport,
  // fall through to the bounded local curve picker below instead of trying to
  // reserve hundreds of megabytes (or failing inside vector allocation).
  constexpr size_t kMaxSelectionPixels = size_t{16} * 1024 * 1024;
  if (vpW > 0 && vpH > 0 && pixelCount <= kMaxSelectionPixels) {
    std::vector<uint32_t> curveIds(pixelCount, 0);
    std::vector<float> curveDepth(pixelCount, 1.0e30f);
    const light3d::Mat4 view = cam_->view();
    // Rows of the column-major view matrix are the camera basis vectors.
    const light3d::Vec3 cameraRight{view.m[0], view.m[4], view.m[8]};
    for (size_t ci = 0; ci < draw_->curves.size(); ++ci) {
      const DrawCurvesCPU& c = draw_->curves[ci];
      if (!carrierVisibleForView(draw_->points.size() + ci) ||
          c.points.size() < 6) continue;
      const size_t np = c.points.size() / 3;
      const std::vector<uint32_t> counts = c.vertexCounts.empty()
          ? std::vector<uint32_t>{static_cast<uint32_t>(np)} : c.vertexCounts;
      const float sx = std::sqrt(c.world[0] * c.world[0] + c.world[1] * c.world[1] + c.world[2] * c.world[2]);
      const float sy = std::sqrt(c.world[4] * c.world[4] + c.world[5] * c.world[5] + c.world[6] * c.world[6]);
      const float sz = std::sqrt(c.world[8] * c.world[8] + c.world[9] * c.world[9] + c.world[10] * c.world[10]);
      const float worldScale = std::max(sx, std::max(sy, sz));
      auto point = [&](size_t i) {
        const float x = c.points[i * 3], y = c.points[i * 3 + 1],
                    z = c.points[i * 3 + 2];
        return light3d::Vec3{
            c.world[0] * x + c.world[4] * y + c.world[8] * z + c.world[12],
            c.world[1] * x + c.world[5] * y + c.world[9] * z + c.world[13],
            c.world[2] * x + c.world[6] * y + c.world[10] * z + c.world[14]};
      };
      size_t base = 0;
      for (uint32_t count : counts) {
        const size_t remaining = np - std::min(base, np);
        const size_t end = base >= np ? np :
            base + std::min(remaining, static_cast<size_t>(count));
        const size_t strandPoints = end - base;
        const size_t pickSegments = curveMaxSegments_ > 0 && strandPoints > 1
            ? std::min(strandPoints - 1,
                       static_cast<size_t>(curveMaxSegments_))
            : (strandPoints > 1 ? strandPoints - 1 : 0);
        for (size_t sj = 0; sj < pickSegments; ++sj) {
          const size_t i = base +
              (sj * (strandPoints - 1) + pickSegments / 2) / pickSegments;
          const size_t ni = base +
              ((sj + 1) * (strandPoints - 1) + pickSegments / 2) / pickSegments;
          const light3d::Vec3 a = point(i), b = point(ni);
          float ax, ay, az, bx, by, bz;
          if (!project(a, ax, ay, az) || !project(b, bx, by, bz)) continue;
          const float w0 = c.widths.empty() ? 1.0f :
              (c.widths.size() == 1 ? c.widths[0] : c.widths[std::min(i, c.widths.size() - 1)]);
          const float w1 = c.widths.empty() ? 1.0f :
              (c.widths.size() == 1 ? c.widths[0] : c.widths[std::min(ni, c.widths.size() - 1)]);
          const float width = 0.5f * (w0 + w1) * worldScale;
          if (!std::isfinite(width) || width < 0.0f) continue;
          const light3d::Vec3 mid = (a + b) * 0.5f;
          light3d::Vec3 side = light3d::cross(b - a, ro - mid);
          if (light3d::dot(side, side) > 1.0e-10f)
            side = light3d::normalize(side);
          else
            side = cameraRight;
          const light3d::Vec3 edge = mid + side * (0.5f * width);
          float ex, ey, ez;
          float radius = 2.0f;
          if (project(edge, ex, ey, ez)) {
            radius = std::max(2.0f, std::min(64.0f,
                std::sqrt((ex - (ax + bx) * 0.5f) * (ex - (ax + bx) * 0.5f) +
                          (ey - (ay + by) * 0.5f) * (ey - (ay + by) * 0.5f))));
          }
          const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - radius)));
          const int x1 = std::min(vpW - 1, static_cast<int>(std::ceil(std::max(ax, bx) + radius)));
          const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - radius)));
          const int y1 = std::min(vpH - 1, static_cast<int>(std::ceil(std::max(ay, by) + radius)));
          const float dx = bx - ax, dy = by - ay;
          const float len2 = dx * dx + dy * dy;
          for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
              const float qx = static_cast<float>(x) + 0.5f;
              const float qy = static_cast<float>(y) + 0.5f;
              float u = 0.0f;
              if (len2 > 1.0e-8f)
                u = std::max(0.0f, std::min(1.0f,
                    ((qx - ax) * dx + (qy - ay) * dy) / len2));
              const float rx = ax + u * dx - qx, ry = ay + u * dy - qy;
              if (rx * rx + ry * ry > radius * radius) continue;
              // Depth belongs to this raster pixel, not to the ray through the
              // original click.  Using that one ray for the entire viewport
              // made overlapping curve prims exchange ownership as the click
              // moved, even though their ID coverage was otherwise correct.
              const float depth = az * (1.0f - u) + bz * u;
              if (!std::isfinite(depth)) continue;
              const size_t pi = static_cast<size_t>(y) * static_cast<size_t>(vpW) +
                                static_cast<size_t>(x);
              if (depth < curveDepth[pi]) {
                curveDepth[pi] = depth;
                curveIds[pi] = static_cast<uint32_t>(ci + 1);
              }
            }
          }
        }
        base = end;
      }
    }
    const int centerX = std::max(0, std::min(vpW - 1, static_cast<int>(std::floor(px))));
    const int centerY = std::max(0, std::min(vpH - 1, static_cast<int>(std::floor(py))));
    uint32_t selectedId = 0;
    float selectedDepth = 1.0e30f;
    for (int radius = 0; radius <= 2 && selectedId == 0; ++radius) {
      for (int y = std::max(0, centerY - radius);
           y <= std::min(vpH - 1, centerY + radius); ++y) {
        for (int x = std::max(0, centerX - radius);
             x <= std::min(vpW - 1, centerX + radius); ++x) {
          const size_t pi = static_cast<size_t>(y) * static_cast<size_t>(vpW) +
                            static_cast<size_t>(x);
          if (curveIds[pi] != 0 && curveDepth[pi] < selectedDepth) {
            selectedId = curveIds[pi];
            selectedDepth = curveDepth[pi];
          }
        }
      }
    }
    if (selectedId > 0 && selectedId <= draw_->curves.size()) {
      bool curveVisible = meshT < 0.0f;
      if (meshT >= 0.0f) {
        const light3d::Vec3 meshPoint = ro + rd * meshT;
        float meshX = 0.0f, meshY = 0.0f, meshDepth = 0.0f;
        curveVisible = !project(meshPoint, meshX, meshY, meshDepth) ||
                       selectedDepth <= meshDepth + 1.0e-5f;
      }
      if (curveVisible) {
        if (hitDistance) {
          const float t = light3d::dot(unproject(selectedDepth) - ro, rd);
          *hitDistance = (std::isfinite(t) && t > 0.0f) ? t : 0.0f;
        }
        return draw_->curves[static_cast<size_t>(selectedId - 1)].absPath;
      }
    }
  }
  auto curveHit = [&](const DrawCurvesCPU& c, float& outT) -> bool {
    const size_t np = c.points.size() / 3;
    if (np < 2) return false;
    const std::vector<uint32_t> counts = c.vertexCounts.empty()
        ? std::vector<uint32_t>{static_cast<uint32_t>(np)} : c.vertexCounts;
    auto point = [&](size_t i) {
      const float x = c.points[i * 3], y = c.points[i * 3 + 1],
                  z = c.points[i * 3 + 2];
      return light3d::Vec3{
          c.world[0] * x + c.world[4] * y + c.world[8] * z + c.world[12],
          c.world[1] * x + c.world[5] * y + c.world[9] * z + c.world[13],
          c.world[2] * x + c.world[6] * y + c.world[10] * z + c.world[14]};
    };
    // Local selection ID/depth buffer.  Restricting this to a small window
    // around the cursor gives the same coverage semantics as a GPU ID pass,
    // without allocating a second full-size Vulkan framebuffer for every
    // click.  Each invocation represents one carrier ID; the caller compares
    // the resulting depth between carriers and meshes.
    constexpr int kPickRadius = 10;
    constexpr int kPickWidth = kPickRadius * 2 + 1;
    int idBuffer[kPickWidth * kPickWidth] = {};
    float depthBuffer[kPickWidth * kPickWidth];
    std::fill(std::begin(depthBuffer), std::end(depthBuffer), 1.0e30f);
    const light3d::Mat4 view = cam_->view();
    const light3d::Vec3 cameraRight{view.m[0], view.m[4], view.m[8]};
    const float sx = std::sqrt(c.world[0] * c.world[0] + c.world[1] * c.world[1] + c.world[2] * c.world[2]);
    const float sy = std::sqrt(c.world[4] * c.world[4] + c.world[5] * c.world[5] + c.world[6] * c.world[6]);
    const float sz = std::sqrt(c.world[8] * c.world[8] + c.world[9] * c.world[9] + c.world[10] * c.world[10]);
    const float worldScale = std::max(sx, std::max(sy, sz));
    float best = 1.0e30f;
    size_t base = 0;
    for (uint32_t count : counts) {
      const size_t remaining = np - std::min(base, np);
      const size_t end = base >= np ? np :
          base + std::min(remaining, static_cast<size_t>(count));
      const size_t strandPoints = end - base;
      const size_t pickSegments = curveMaxSegments_ > 0 && strandPoints > 1
          ? std::min(strandPoints - 1,
                     static_cast<size_t>(curveMaxSegments_))
          : (strandPoints > 1 ? strandPoints - 1 : 0);
      for (size_t sj = 0; sj < pickSegments; ++sj) {
        const size_t i = base +
            (sj * (strandPoints - 1) + pickSegments / 2) / pickSegments;
        const size_t ni = base +
            ((sj + 1) * (strandPoints - 1) + pickSegments / 2) / pickSegments;
        const light3d::Vec3 a = point(i), b = point(ni);
        float ax, ay, az, bx, by, bz;
        if (!project(a, ax, ay, az) || !project(b, bx, by, bz)) continue;
        const float dx = bx - ax, dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        const float w0 = c.widths.empty() ? 1.0f :
            (c.widths.size() == 1 ? c.widths[0] : c.widths[std::min(i, c.widths.size() - 1)]);
        const float w1 = c.widths.empty() ? 1.0f :
            (c.widths.size() == 1 ? c.widths[0] : c.widths[std::min(ni, c.widths.size() - 1)]);
        const float width = 0.5f * (w0 + w1) * worldScale;
        if (!std::isfinite(width) || width < 0.0f) continue;
        const light3d::Vec3 mid = (a + b) * 0.5f;
        light3d::Vec3 side = light3d::cross(b - a, ro - mid);
        if (light3d::dot(side, side) > 1.0e-10f)
          side = light3d::normalize(side);
        else
          side = cameraRight;
        const light3d::Vec3 edge = mid + side * (0.5f * width);
        float mx, my, mz;
        float radius = 2.0f;
        if (project(mid, mx, my, mz) && project(edge, mx, my, mz)) {
          // Keep the local buffer bounded even for malformed authored widths.
          radius = std::max(2.0f, std::min(64.0f,
              std::sqrt((mx - (ax + bx) * 0.5f) * (mx - (ax + bx) * 0.5f) +
                        (my - (ay + by) * 0.5f) * (my - (ay + by) * 0.5f))));
        }
        const int x0 = std::max(-kPickRadius, static_cast<int>(std::floor(std::min(ax, bx) - px - radius)));
        const int x1 = std::min(kPickRadius, static_cast<int>(std::ceil(std::max(ax, bx) - px + radius)));
        const int y0 = std::max(-kPickRadius, static_cast<int>(std::floor(std::min(ay, by) - py - radius)));
        const int y1 = std::min(kPickRadius, static_cast<int>(std::ceil(std::max(ay, by) - py + radius)));
        for (int yy = y0; yy <= y1; ++yy) {
          for (int xx = x0; xx <= x1; ++xx) {
            const float qx = px + static_cast<float>(xx) + 0.5f;
            const float qy = py + static_cast<float>(yy) + 0.5f;
            float u = 0.0f;
            if (len2 > 1.0e-8f)
              u = std::max(0.0f, std::min(1.0f,
                  ((qx - ax) * dx + (qy - ay) * dy) / len2));
            const float ex = ax + u * dx - qx, ey = ay + u * dy - qy;
            if (ex * ex + ey * ey > radius * radius) continue;
            const light3d::Vec3 q = a * (1.0f - u) + b * u;
            const float t = light3d::dot(q - ro, rd);
            if (t <= 1.0e-5f) continue;
            const int bi = (yy + kPickRadius) * kPickWidth + xx + kPickRadius;
            if (t < depthBuffer[bi]) {
              depthBuffer[bi] = t;
              idBuffer[bi] = 1;
            }
          }
        }
      }
      base = end;
    }
    // Sample the cursor pixel first; if its center falls in a one-pixel gap,
    // accept the nearest covered pixel in the local selection buffer.
    for (int pass = 0; pass < 2 && best == 1.0e30f; ++pass) {
      for (int yy = -kPickRadius; yy <= kPickRadius; ++yy) {
        for (int xx = -kPickRadius; xx <= kPickRadius; ++xx) {
          if (pass == 0 && (xx != 0 || yy != 0)) continue;
          const int bi = (yy + kPickRadius) * kPickWidth + xx + kPickRadius;
          if (idBuffer[bi] == 0) continue;
          if (pass == 1 && xx * xx + yy * yy > 4) continue;
          best = std::min(best, depthBuffer[bi]);
        }
      }
    }
    if (best == 1.0e30f) return false;
    outT = best;
    return true;
  };
  auto pointHit = [&](const DrawPointsCPU& p, float& outT) -> bool {
    const size_t np = p.points.size() / 3;
    if (np == 0) return false;
    const light3d::Mat4 view = cam_->view();
    const light3d::Vec3 cameraRight{view.m[0], view.m[4], view.m[8]};
    float best = 1.0e30f;
    for (size_t i = 0; i < np; ++i) {
      const float x = p.points[i * 3], y = p.points[i * 3 + 1],
                  z = p.points[i * 3 + 2];
      const light3d::Vec3 center{
          p.world[0] * x + p.world[4] * y + p.world[8] * z + p.world[12],
          p.world[1] * x + p.world[5] * y + p.world[9] * z + p.world[13],
          p.world[2] * x + p.world[6] * y + p.world[10] * z + p.world[14]};
      float sx = 0.0f, sy = 0.0f, sz = 0.0f;
      if (!project(center, sx, sy, sz)) continue;
      float radiusWorld = 0.5f;
      if (p.gaussian && p.ellipseRadii.size() >= (i + 1) * 2) {
        radiusWorld = 0.5f * std::max(p.ellipseRadii[i * 2],
                                      p.ellipseRadii[i * 2 + 1]);
      } else if (!p.widths.empty()) {
        radiusWorld = p.widths.size() == 1
            ? 0.5f * p.widths[0]
            : 0.5f * p.widths[std::min(i, p.widths.size() - 1)];
      }
      if (!std::isfinite(radiusWorld) || radiusWorld < 0.0f) continue;
      float ex = 0.0f, ey = 0.0f, ez = 0.0f;
      const light3d::Vec3 edge = center + cameraRight * radiusWorld;
      float radius = 2.0f;
      if (project(edge, ex, ey, ez)) {
        radius = std::max(2.0f, std::min(64.0f,
            std::sqrt((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy))));
      }
      const float dx = px - sx, dy = py - sy;
      if (dx * dx + dy * dy > radius * radius) continue;
      const float t = light3d::dot(center - ro, rd);
      if (t > 1.0e-5f) best = std::min(best, t);
    }
    if (best == 1.0e30f) return false;
    outT = best;
    return true;
  };
  std::string bestPath;
  float bestT = 1.0e30f;
  auto consider = [&](const std::string& path, const std::string& purpose,
                      const float mn[3], const float mx[3]) {
    if (path.empty() || !meshPurposeVisible(purpose)) return;
    float t = 0.0f;
    if (hit(mn, mx, &t) && t < bestT) {
      bestT = t;
      bestPath = path;
    }
  };
  for (const DrawPointsCPU& p : draw_->points) {
    if (!meshPurposeVisible(p.purpose) || p.absPath.empty()) continue;
    float pointT = 0.0f;
    if (pointHit(p, pointT) && pointT < bestT) {
      bestT = pointT;
      bestPath = p.absPath;
    } else if (p.points.empty()) {
      consider(p.absPath, p.purpose, p.aabbMin, p.aabbMax);
    }
  }
  for (size_t i = 0; i < draw_->curves.size(); ++i) {
    const DrawCurvesCPU& c = draw_->curves[i];
    if (!carrierVisibleForView(draw_->points.size() + i)) continue;
    float segmentT = 0.0f;
    if (curveHit(c, segmentT) && segmentT < bestT) {
      bestT = segmentT;
      bestPath = c.absPath;
    } else if (c.points.size() < 6) {
      consider(c.absPath, c.purpose, c.aabbMin, c.aabbMax);
    }
  }
  // Do not let a broad mesh/container bound hide a visible curve carrier. The
  // curve path is only preferred when its bounds enter the ray before the mesh
  // hit; otherwise normal mesh picking remains authoritative.
  const bool meshHasExactDepth = meshT >= 0.0f;
  if (!bestPath.empty() && (!meshHasExactDepth || bestT < meshT)) {
    if (hitDistance)
      *hitDistance = (std::isfinite(bestT) && bestT > 0.0f) ? bestT : 0.0f;
    return bestPath;
  }
  if (mesh >= 0 && static_cast<size_t>(mesh) < draw_->meshes.size()) {
    if (hitDistance) {
      const float t = std::fabs(meshT);
      *hitDistance = (std::isfinite(t) && t > 0.0f) ? t : 0.0f;
    }
    return draw_->meshes[static_cast<size_t>(mesh)].absPath;
  }
  if (hitDistance)
    *hitDistance = (std::isfinite(bestT) && bestT > 0.0f) ? bestT : 0.0f;
  return bestPath;
}

void Gui::beginRegionSelection(const ImVec2& mouse) {
  regionSelecting_ = true;
  regionSelectionMoved_ = false;
  regionStart_ = mouse;
  regionEnd_ = mouse;
}

void Gui::updateRegionSelection(const ImVec2& mouse) {
  if (!regionSelecting_) return;
  regionEnd_ = mouse;
  const float dx = regionEnd_.x - regionStart_.x;
  const float dy = regionEnd_.y - regionStart_.y;
  if ((dx * dx + dy * dy) > 16.0f) regionSelectionMoved_ = true;
}

bool Gui::meshIntersectsScreenRect(size_t meshIndex, const ImVec2& rectMin,
                                   const ImVec2& rectMax, int vpW, int vpH) const {
  if (!draw_ || !cam_ || !renderer_ || meshIndex >= draw_->meshes.size() ||
      vpW <= 0 || vpH <= 0 || !meshVisibleForView(meshIndex)) {
    return false;
  }

  const DrawMeshCPU& mesh = draw_->meshes[meshIndex];
  const light3d::Mat4 VP =
      cam_->proj(renderer_->caps().usesZeroToOneDepth) * cam_->view();
  const float xs[2] = {mesh.aabbMin[0], mesh.aabbMax[0]};
  const float ys[2] = {mesh.aabbMin[1], mesh.aabbMax[1]};
  const float zs[2] = {mesh.aabbMin[2], mesh.aabbMax[2]};

  ImVec2 bmin(1e30f, 1e30f);
  ImVec2 bmax(-1e30f, -1e30f);
  bool any = false;
  for (int xi = 0; xi < 2; ++xi) {
    for (int yi = 0; yi < 2; ++yi) {
      for (int zi = 0; zi < 2; ++zi) {
        const float x = xs[xi], y = ys[yi], z = zs[zi];
        const float* m = VP.m;
        const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
        const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (cw <= 1e-6f) continue;
        const float invW = 1.0f / cw;
        const float sx = (cx * invW * 0.5f + 0.5f) * static_cast<float>(vpW);
        const float sy = (1.0f - (cy * invW * 0.5f + 0.5f)) * static_cast<float>(vpH);
        bmin.x = std::min(bmin.x, sx);
        bmin.y = std::min(bmin.y, sy);
        bmax.x = std::max(bmax.x, sx);
        bmax.y = std::max(bmax.y, sy);
        any = true;
      }
    }
  }
  if (!any) return false;
  return bmax.x >= rectMin.x && bmin.x <= rectMax.x &&
         bmax.y >= rectMin.y && bmin.y <= rectMax.y;
}

std::vector<int> Gui::regionPickMeshes(const ImVec2& imageMin, int vpW, int vpH) const {
  std::vector<int> hits;
  if (!draw_ || !cam_ || !renderer_ || vpW <= 0 || vpH <= 0) return hits;

  ImVec2 r0(std::min(regionStart_.x, regionEnd_.x) - imageMin.x,
            std::min(regionStart_.y, regionEnd_.y) - imageMin.y);
  ImVec2 r1(std::max(regionStart_.x, regionEnd_.x) - imageMin.x,
            std::max(regionStart_.y, regionEnd_.y) - imageMin.y);
  r0.x = std::max(0.0f, std::min(r0.x, static_cast<float>(vpW)));
  r0.y = std::max(0.0f, std::min(r0.y, static_cast<float>(vpH)));
  r1.x = std::max(0.0f, std::min(r1.x, static_cast<float>(vpW)));
  r1.y = std::max(0.0f, std::min(r1.y, static_cast<float>(vpH)));
  if ((r1.x - r0.x) < 1.0f || (r1.y - r0.y) < 1.0f) return hits;

  for (size_t i = 0; i < draw_->meshes.size(); ++i) {
    if (meshIntersectsScreenRect(i, r0, r1, vpW, vpH)) {
      hits.push_back(static_cast<int>(i));
    }
  }
  return hits;
}

void Gui::finishRegionSelection(const ImVec2& imageMin) {
  if (!regionSelecting_) return;
  const bool wasDrag = regionSelectionMoved_;
  regionSelecting_ = false;
  regionSelectionMoved_ = false;

  if (wasDrag) {
    const float x0 = regionStart_.x - imageMin.x;
    const float y0 = regionStart_.y - imageMin.y;
    const float x1 = regionEnd_.x - imageMin.x;
    const float y1 = regionEnd_.y - imageMin.y;
    const PickReport picked = pickViewportRegion(x0, y0, x1, y1, true);
    if (picked.path.empty()) clearSelection();
    return;
  }

  if (!draw_) return;
  const float px = regionEnd_.x - imageMin.x;
  const float py = regionEnd_.y - imageMin.y;
  const PickReport picked = pickViewportPixel(px, py, true);
  if (picked.path.empty()) clearSelection();
}

void Gui::drawViewport() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_None;
  if (captureViewportOnly_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
  }
  ImGui::Begin("Viewport", nullptr, flags);
  ImGui::PopStyleVar();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int w = static_cast<int>(avail.x);
  const int h = static_cast<int>(avail.y);
  viewportW_ = w;
  viewportH_ = h;

  if (renderer_ && cam_ && w > 0 && h > 0) {
    // resizeViewport is a GL op (re-allocates the offscreen FBO textures); in the
    // threaded path it must run on the render thread that owns the context, not
    // here on the UI thread. gpu() routes it to the op-queue (inline when single
    // threaded). viewportTexture()'s id is stable across resizes, so ImGui::Image
    // can reference it now and the render thread fills it.
    const int rw = std::max(1, static_cast<int>(std::lround(w * renderScale_)));
    const int rh = std::max(1, static_cast<int>(std::lround(h * renderScale_)));
    gpu([this, rw, rh] { renderer_->resizeViewport(rw, rh); });

    const ImTextureID tex = static_cast<ImTextureID>(renderer_->viewportTexture());
    const bool flip = renderer_->caps().flipViewportV;
    const ImVec2 uv0 = flip ? ImVec2(0, 1) : ImVec2(0, 0);
    const ImVec2 uv1 = flip ? ImVec2(1, 0) : ImVec2(1, 1);
    // Only draw the viewport image once the backend has a real texture handle. On the
    // threaded path the offscreen target (and its ImGui descriptor) is created on the
    // render thread; before its first frame viewportTexture() can be 0, and emitting
    // ImGui::Image(0) binds an empty descriptor set that the ImGui fragment shader then
    // samples out-of-bounds -> GPU fault / device loss (VK). Skip it until it's ready.
    if (tex) ImGui::Image(tex, avail, uv0, uv1);
    else ImGui::Dummy(avail);
    const bool imageHovered = ImGui::IsItemHovered();
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    viewportImageMin_ = imageMin;
    bool navButtonHovered = false;
    {
      const float buttonSize = ImGui::GetFrameHeight();
      ImGui::SetCursorScreenPos(ImVec2(imageMax.x - buttonSize - 8.0f,
                                       imageMin.y + 8.0f));
      ImGui::PushID("viewport_nav_help");
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(18, 20, 24, 180));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(42, 48, 58, 220));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(64, 72, 86, 240));
      if (ImGui::Button("?", ImVec2(buttonSize, buttonSize))) {
        cycleNavigationHelpMode();
      }
      navButtonHovered = ImGui::IsItemHovered();
      if (navButtonHovered) {
        const char* mode = navigationHelpMode_ == NavigationHelpMode::Full
                               ? "full"
                               : (navigationHelpMode_ == NavigationHelpMode::Simple
                                      ? "simple" : "none");
        ImGui::SetTooltip("Viewport navigation: %s (F1 cycles)", mode);
      }
      ImGui::PopStyleColor(3);
      ImGui::PopStyleVar();
      ImGui::PopID();
    }
    vpHovered_ = imageHovered && !navButtonHovered;
    handleNavigation();
    drawNavigationOverlay(imageMin, imageMax);

    // Prim path labels overlay
    if (showPrimLabels_ && draw_ && cam_ && renderer_) {
      const bool z01 = renderer_->caps().usesZeroToOneDepth;
      const light3d::Mat4 VP = cam_->proj(z01) * cam_->view();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float vpW_f = static_cast<float>(w);
      const float vpH_f = static_cast<float>(h);
      for (size_t mi = 0; mi < draw_->meshes.size(); ++mi) {
        if (!meshVisibleForView(mi)) continue;
        const DrawMeshCPU& m = draw_->meshes[mi];
        // Project AABB center to screen
        float cx = (m.aabbMin[0] + m.aabbMax[0]) * 0.5f;
        float cy = (m.aabbMin[1] + m.aabbMax[1]) * 0.5f;
        float cz = (m.aabbMin[2] + m.aabbMax[2]) * 0.5f;
        const float* vp = VP.m;
        float px = vp[0] * cx + vp[4] * cy + vp[8] * cz + vp[12];
        float py = vp[1] * cx + vp[5] * cy + vp[9] * cz + vp[13];
        float pw = vp[3] * cx + vp[7] * cy + vp[11] * cz + vp[15];
        if (pw <= 1e-6f) continue;
        float invW = 1.0f / pw;
        float sx = (px * invW * 0.5f + 0.5f) * vpW_f + imageMin.x;
        float sy = (1.0f - (py * invW * 0.5f + 0.5f)) * vpH_f + imageMin.y;
        if (sx < imageMin.x || sx > imageMax.x || sy < imageMin.y || sy > imageMax.y) continue;
        dl->AddText(ImVec2(sx + 4, sy - 8), IM_COL32(255, 255, 200, 200),
                    m.absPath.c_str());
      }
    }

    // Gizmo interaction (translate manipulator).
    if (xformMode_ == TransformMode::Translate && draw_ && /*vpHovered_ &&*/
        selMeshIndex_ >= 0 &&
        static_cast<size_t>(selMeshIndex_) < draw_->meshes.size()) {
      ImGuiIO& io = ImGui::GetIO();
      const bool plainLeft = navMode_ == 0 && !io.KeyAlt;
      const float* W = draw_->meshes[static_cast<size_t>(selMeshIndex_)].world;
      float tx = W[12], ty = W[13], tz = W[14];
      float gizmoLen = 1.0f;
      if (draw_->hasBounds) {
        const light3d::Vec3 smn{draw_->aabbMin[0], draw_->aabbMin[1], draw_->aabbMin[2]};
        const light3d::Vec3 smx{draw_->aabbMax[0], draw_->aabbMax[1], draw_->aabbMax[2]};
        gizmoLen = std::max(light3d::length(smx - smn) * 0.15f, 0.5f);
      }
      const light3d::Mat4 viewM = cam_->view();
      const light3d::Mat4 projM = cam_->proj(renderer_->caps().usesZeroToOneDepth);
      const light3d::Mat4 vp = projM * viewM;
      // Project a point to screen-space NDC.
      auto project = [&](float x, float y, float z) -> ImVec2 {
        float c[4];
        for (int r = 0; r < 4; ++r)
          c[r] = vp.m[0*4+r]*x + vp.m[1*4+r]*y + vp.m[2*4+r]*z + vp.m[3*4+r];
        if (std::abs(c[3]) < 1e-10f) return ImVec2(-1e9,-1e9);
        float nx = c[0]/c[3], ny = c[1]/c[3];
        return ImVec2((nx*0.5f+0.5f)*static_cast<float>(w), (1.0f-(ny*0.5f+0.5f))*static_cast<float>(h));
      };
      ImVec2 origin2D = project(tx, ty, tz);
      // Axis endpoints in screen space.
      struct Axis2D { ImVec2 tip; float r,g,b; int idx; };
      Axis2D axes2D[3] = {
        {project(tx+gizmoLen, ty, tz), 0.9f,0.2f,0.2f, 0},
        {project(tx, ty+gizmoLen, tz), 0.2f,0.9f,0.2f, 1},
        {project(tx, ty, tz+gizmoLen), 0.2f,0.3f,0.9f, 2},
      };
      // For gizmo-active state, compute displacement along axis.
      if (gizmoActive_ && gizmoAxis_ >= 0 && gizmoAxis_ < 3) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
          ImVec2 delta = io.MouseDelta;
          float scale = 0.01f * cam_->distance();
          float dir[3] = {0,0,0}; dir[gizmoAxis_] = 1.0f;
          float d = (delta.x - delta.y) * 0.5f * scale;
          gizmoStartPos_[0] += dir[0] * d;
          gizmoStartPos_[1] += dir[1] * d;
          gizmoStartPos_[2] += dir[2] * d;
          auto& mesh = const_cast<DrawMeshCPU&>(draw_->meshes[static_cast<size_t>(selMeshIndex_)]);
          mesh.world[12] = gizmoStartPos_[0];
          mesh.world[13] = gizmoStartPos_[1];
          mesh.world[14] = gizmoStartPos_[2];
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          gizmoActive_ = false;
          gizmoAxis_ = -1;
        }
      } else if (vpHovered_ && plainLeft && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Screen-space pick: find closest axis to mouse cursor.
        ImVec2 mouse = {io.MousePos.x - imageMin.x, io.MousePos.y - imageMin.y};
        float bestDist = 1e10f;
        int bestAxis = -1;
        for (int i = 0; i < 3; ++i) {
          if (axes2D[i].tip.x < -1e8f) continue;
          // Distance from mouse to line segment (origin2D to tip).
          ImVec2 d = {axes2D[i].tip.x - origin2D.x, axes2D[i].tip.y - origin2D.y};
          float len2 = d.x*d.x + d.y*d.y;
          if (len2 < 1e-6f) continue;
          float t = ((mouse.x - origin2D.x)*d.x + (mouse.y - origin2D.y)*d.y) / len2;
          t = std::max(0.0f, std::min(1.0f, t));
          float px2 = origin2D.x + t*d.x - mouse.x;
          float py2 = origin2D.y + t*d.y - mouse.y;
          float dist2 = px2*px2 + py2*py2;
          if (dist2 < bestDist) { bestDist = dist2; bestAxis = i; }
        }
        if (bestAxis >= 0 && bestDist < 400.0f) {  // 20px threshold squared
          gizmoAxis_ = bestAxis;
          gizmoMouseStart_ = io.MousePos;
          gizmoStartPos_[0] = tx; gizmoStartPos_[1] = ty; gizmoStartPos_[2] = tz;
          gizmoActive_ = true;
        } else {
          beginRegionSelection(io.MousePos);
        }
      } else if (vpHovered_ && !io.MouseDown[0]) {
        // Hover: update gizmoAxis_ for highlighting.
        ImVec2 mouse = {io.MousePos.x - imageMin.x, io.MousePos.y - imageMin.y};
        float bestDist = 400.0f;
        int bestAxis = -1;
        for (int i = 0; i < 3; ++i) {
          if (axes2D[i].tip.x < -1e8f) continue;
          ImVec2 d = {axes2D[i].tip.x - origin2D.x, axes2D[i].tip.y - origin2D.y};
          float len2 = d.x*d.x + d.y*d.y;
          if (len2 < 1e-6f) continue;
          float t = ((mouse.x - origin2D.x)*d.x + (mouse.y - origin2D.y)*d.y) / len2;
          t = std::max(0.0f, std::min(1.0f, t));
          float px2 = origin2D.x + t*d.x - mouse.x;
          float py2 = origin2D.y + t*d.y - mouse.y;
          float dist2 = px2*px2 + py2*py2;
          if (dist2 < bestDist) { bestDist = dist2; bestAxis = i; }
        }
        gizmoAxis_ = bestAxis;
      }
    }

    // Plain left click picks one mesh; plain left drag selects all visible mesh
    // prims whose projected bounds intersect the drag rectangle. Alt+drag stays
    // reserved for camera navigation.
    {
      ImGuiIO& io = ImGui::GetIO();
      const bool plainLeft = navMode_ == 0 && !io.KeyAlt;
      if (vpHovered_ && plainLeft && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          !gizmoActive_) {
        beginRegionSelection(io.MousePos);
      }
      if (regionSelecting_) {
        updateRegionSelection(io.MousePos);
        if (regionSelectionMoved_) {
          const ImVec2 rmin(std::max(imageMin.x, std::min(regionStart_.x, regionEnd_.x)),
                            std::max(imageMin.y, std::min(regionStart_.y, regionEnd_.y)));
          const ImVec2 rmax(std::min(imageMax.x, std::max(regionStart_.x, regionEnd_.x)),
                            std::min(imageMax.y, std::max(regionStart_.y, regionEnd_.y)));
          ImDrawList* dl = ImGui::GetWindowDrawList();
          dl->AddRectFilled(rmin, rmax, IM_COL32(80, 145, 255, 48), 0.0f);
          dl->AddRect(rmin, rmax, IM_COL32(130, 180, 255, 220), 0.0f);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          finishRegionSelection(imageMin);
        }
      }
    }

    if (draw_ && draw_->empty()) {
      // Center the message in the viewport (a fixed pixel offset clips under the
      // tab bar at HiDPI scale).
      const char* msg = (loaded_ && loaded_->ok)
                            ? "No renderable meshes in this scene."
                            : "No scene loaded. Use File > Open...";
      const ImVec2 ts = ImGui::CalcTextSize(msg);
      ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
      ImGui::TextColored(ImVec4(1, 1, 1, 0.7f), "%s", msg);
    }
  } else {
    vpHovered_ = false;
  }
  ImGui::End();
}

void Gui::renderViewportScene(FramePacket* packet) {
  if (!renderer_ || !cam_ || viewportW_ <= 0 || viewportH_ <= 0) return;

  const int vpW = stereoEyeViewportWidth_ > 0
                      ? std::min(viewportW_, stereoEyeViewportWidth_)
                      : viewportW_;
  cam_->setAspect(static_cast<float>(vpW) / static_cast<float>(viewportH_));
  const int vpH = viewportH_;
  const int renderW = std::max(
      1, static_cast<int>(std::lround(vpW * renderScale_)));
  const int renderH = std::max(
      1, static_cast<int>(std::lround(vpH * renderScale_)));
  gpu([this, renderW, renderH] { renderer_->resizeViewport(renderW, renderH); });

  const light3d::Mat4 viewM = cam_->view();
  const light3d::Mat4 projM = cam_->proj(renderer_->caps().usesZeroToOneDepth);
  const light3d::Vec3 eye = cam_->eye();

  RenderFrameParams p;
  p.view = viewM.m;
  p.proj = projM.m;
  p.cameraPos[0] = eye.x;
  p.cameraPos[1] = eye.y;
  p.cameraPos[2] = eye.z;
  p.exposure = cam_->exposure();
  p.materialXFrame = static_cast<float>(timeline_.current);
  p.materialXTime = static_cast<float>(timeline_.fps > 0.0
                                           ? timeline_.current / timeline_.fps
                                           : 0.0);
  p.cameraLens = cameraLens_;
  p.cameraShutter = cameraShutter_;
  p.rtTemporalPose = rtTemporalPose_;
  p.rtTemporalReset = rtTemporalReset_;
  p.clippingPlaneCount = std::min(
      static_cast<int>(cameraClippingPlanes_.size() / 4),
      RenderFrameParams::kMaxClippingPlanes);
  std::copy_n(cameraClippingPlanes_.data(), p.clippingPlaneCount * 4,
              p.clippingPlanes);
  p.pathTrace = pathTrace_;
  p.mode = mode_;
  const bool pickingAov = mode_ == RenderMode::MeshId;
  // A picking-buffer view must contain only pickable, depth-tested carriers.
  // Wireframe and helper/highlight overlays have no selection ID.
  p.wireMode = pickingAov ? 0
                          : (mode_ == RenderMode::Wireframe ? 1 : wireCycle_);
  p.displacement = displacementEnabled_;
  p.displacementScale = displacementScale_;
  p.maxTessLevel = maxTessLevel_;
  bool curveViewChanged = !haveLastCurveView_;
  if (!curveViewChanged) {
    for (size_t i = 0; i < lastCurveView_.size(); ++i) {
      if (std::fabs(lastCurveView_[i] - viewM.m[i]) > 1.0e-6f) {
        curveViewChanged = true;
        break;
      }
    }
  }
  const double curveNow = ImGui::GetTime();
  if (curveViewChanged) {
    std::copy(viewM.m, viewM.m + 16, lastCurveView_.begin());
    haveLastCurveView_ = true;
    lastCurveViewChangeTime_ = curveNow;
  }
  // Fixed-frame/headless captures must render the requested quality.  Their
  // startup camera is technically changing, but there is no interactive UI to
  // protect and reducing the first frames makes screenshots nondeterministic.
  const bool curveInteractive = !captureViewportOnly_ &&
                                lastCurveViewChangeTime_ >= 0.0 &&
                                curveNow - lastCurveViewChangeTime_ < 0.20;
  cameraInteractive_ = curveInteractive;
  p.curveMaxSegments = curveInteractive && curveMaxSegments_ > 0
                           ? std::min(curveMaxSegments_, 2)
                           : curveMaxSegments_;
  // Don't outline a hidden selection.
  const bool selHidden =
      selMeshIndex_ >= 0 &&
      !meshVisibleForView(static_cast<size_t>(selMeshIndex_));
  const bool showHighlight = selectionHighlightMode_ >= 2 && !pickingAov;
  p.highlightMeshIndex = !showHighlight || selHidden ? -1 : selMeshIndex_;
  // A selected GeomSubset highlights just its faces on the parent mesh (GL
  // polygon-mode path).
  if (showHighlight && highlightSubsetMesh_ >= 0 && !highlightSubsetIndices_.empty() &&
      meshVisibleForView(static_cast<size_t>(highlightSubsetMesh_))) {
    p.highlightMeshIndex = highlightSubsetMesh_;
    p.highlightIndices = highlightSubsetIndices_.data();
    p.highlightIndexCount = static_cast<int>(highlightSubsetIndices_.size());
  }
  // Vulkan highlight: world-space edge lines (whole mesh or subset).
  if (showHighlight && !highlightLinesData_.empty()) {
    p.highlightLines = highlightLinesData_.data();
    p.highlightLineVertexCount = static_cast<int>(highlightLinesData_.size());
  }
  p.highlightXray = selectionHighlightMode_ == 3;
  for (int i = 0; i < 4; ++i) p.clearColor[i] = clearColor_[i];
  // Scene bbox for the depth + position AOVs.
  if (draw_ && draw_->hasBounds) {
    float dx = draw_->aabbMax[0] - draw_->aabbMin[0];
    float dy = draw_->aabbMax[1] - draw_->aabbMin[1];
    float dz = draw_->aabbMax[2] - draw_->aabbMin[2];
    p.depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
    for (int i = 0; i < 3; ++i) {
      p.sceneMin[i] = draw_->aabbMin[i];
      p.sceneExtent[i] = std::max(1e-4f, draw_->aabbMax[i] - draw_->aabbMin[i]);
    }
  }
  if (draw_ && draw_->hasPreviewLight) {
    for (int i = 0; i < 3; ++i) {
      p.lightDir[i] = draw_->previewLightDir[i];
      p.lightColor[i] = draw_->previewLightColor[i];
    }
  } else if (cam_) {
    // Match usdview/Storm's default GlfSimpleLight: a camera-positioned
    // headlight. The previous above/right studio offset left view-facing
    // surfaces underexposed even though their display transfer was correct.
    const light3d::Vec3 towardCamera =
        light3d::normalize(cam_->eye() - cam_->target());
    p.lightDir[0] = towardCamera.x;
    p.lightDir[1] = towardCamera.y;
    p.lightDir[2] = towardCamera.z;
    p.lightColor[0] = p.lightColor[1] = p.lightColor[2] = 1.0f;
  }
  // Per-phase frame timing (LUSDVIEW_TIME_FRAME): isolates where a heavy scene
  // spends its frame -- instance cull/upload (CPU + GPU upload) vs renderFrame
  // (GPU draw submission). The GPU rasterisation itself lands largely in present()
  // (timed in app.cc), since GL/VK only flush there.
  static const bool timeFrame = std::getenv("LUSDVIEW_TIME_FRAME") != nullptr;
  using Clock = std::chrono::steady_clock;
  auto t0 = timeFrame ? Clock::now() : Clock::time_point{};
  buildViewVisibilityMask();
  auto t1 = timeFrame ? Clock::now() : Clock::time_point{};
  cullInstances();  // per-instance frustum cull (updates renderer instance buffers)
  auto t2 = timeFrame ? Clock::now() : Clock::time_point{};
  if (!viewVisible_.empty()) {
    p.meshVisible = viewVisible_.data();
    p.meshVisibleCount = static_cast<int>(viewVisible_.size());
  }
  if (!meshVisible_.empty()) {
    p.rtMeshVisible = meshVisible_.data();
    p.rtMeshVisibleCount = static_cast<int>(meshVisible_.size());
  }
  if (!carrierVisible_.empty()) {
    p.carrierVisible = carrierVisible_.data();
    p.carrierVisibleCount = static_cast<int>(carrierVisible_.size());
  }
  // Purpose visibility for the RT TLAS (PurposeId bit order: default, render,
  // proxy, guide). Raster gets the same filtering via viewVisible_.
  p.purposeVisibleMask = (showPurposeDefault_ ? 1u : 0u) |
                         (showPurposeRender_ ? 2u : 0u) |
                         (showPurposeProxy_ ? 4u : 0u) |
                         (showPurposeGuide_ ? 8u : 0u);
  buildHelpers();
  if (!pickingAov) {
    p.helperLines = helperLines_.empty() ? nullptr : helperLines_.data();
    p.helperLineVertexCount = static_cast<int>(helperLines_.size());
    p.overlayLines = overlayLines_.empty() ? nullptr : overlayLines_.data();
    p.overlayLineVertexCount = static_cast<int>(overlayLines_.size());
  }

  if (!packet) {
    renderer_->renderFrame(p);  // single-threaded: render inline
    if (timeFrame) {
      auto t3 = Clock::now();
      auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
      };
      std::fprintf(stderr,
                   "[frame] viewmask=%.1fms cull+upload=%.1fms renderFrame=%.1fms "
                   "(vis inst=%zu, inst tris=%zu)\n",
                   ms(t0, t1), ms(t1, t2), ms(t2, t3), statVisibleInstances_,
                   statInstTris_);
    }
    return;
  }
  // Threaded: copy everything the render thread needs into the owned packet.
  std::memcpy(packet->view, viewM.m, sizeof(packet->view));
  std::memcpy(packet->proj, projM.m, sizeof(packet->proj));
  packet->cameraPos[0] = eye.x; packet->cameraPos[1] = eye.y; packet->cameraPos[2] = eye.z;
  packet->exposure = p.exposure;
  packet->materialXTime = p.materialXTime;
  packet->materialXFrame = p.materialXFrame;
  packet->cameraLens = p.cameraLens;
  packet->cameraShutter = p.cameraShutter;
  packet->rtTemporalPose = p.rtTemporalPose;
  packet->rtTemporalReset = p.rtTemporalReset;
  packet->clippingPlaneCount = p.clippingPlaneCount;
  std::copy(p.clippingPlanes,
            p.clippingPlanes + RenderFrameParams::kMaxClippingPlanes * 4,
            packet->clippingPlanes);
  packet->pathTrace = p.pathTrace;
  packet->mode = p.mode;
  for (int i = 0; i < 4; ++i) packet->clearColor[i] = p.clearColor[i];
  for (int i = 0; i < 3; ++i) {
    packet->lightDir[i] = p.lightDir[i];
    packet->lightColor[i] = p.lightColor[i];
  }
  packet->depthScale = p.depthScale;
  for (int i = 0; i < 3; ++i) { packet->sceneMin[i] = p.sceneMin[i]; packet->sceneExtent[i] = p.sceneExtent[i]; }
  packet->highlightMeshIndex = p.highlightMeshIndex;
  packet->highlightXray = p.highlightXray;
  if (p.highlightIndices)
    packet->highlightIndices.assign(p.highlightIndices, p.highlightIndices + p.highlightIndexCount);
  if (p.highlightLines)
    packet->highlightLines.assign(p.highlightLines, p.highlightLines + p.highlightLineVertexCount);
  if (p.helperLines)
    packet->helperLines.assign(p.helperLines, p.helperLines + p.helperLineVertexCount);
  if (p.overlayLines)
    packet->overlayLines.assign(p.overlayLines, p.overlayLines + p.overlayLineVertexCount);
  if (p.meshVisible)
    packet->meshVisible.assign(p.meshVisible, p.meshVisible + p.meshVisibleCount);
  if (p.carrierVisible)
    packet->carrierVisible.assign(p.carrierVisible,
                                  p.carrierVisible + p.carrierVisibleCount);
  if (p.rtMeshVisible)
    packet->rtMeshVisible.assign(p.rtMeshVisible,
                                 p.rtMeshVisible + p.rtMeshVisibleCount);
  packet->purposeVisibleMask = p.purposeVisibleMask;
  packet->curveMaxSegments = p.curveMaxSegments;
  packet->viewportW = renderW;
  packet->viewportH = renderH;
  packet->hasParams = true;
}

void Gui::buildHelpers() {
  helperLines_.clear();
  overlayLines_.clear();
  helperLines_.insert(helperLines_.end(), physicsDebugLines_.begin(),
                      physicsDebugLines_.end());
  const bool zUp = loaded_ && loaded_->ok && loaded_->render.meta.upAxis == "Z";

  auto addLine = [&](float ax, float ay, float az, float bx, float by, float bz,
                     float r, float g, float b) {
    helperLines_.push_back(HelperVertex{{ax, ay, az}, {r, g, b}});
    helperLines_.push_back(HelperVertex{{bx, by, bz}, {r, g, b}});
  };
  auto addOverlay = [&](float ax, float ay, float az, float bx, float by, float bz,
                        float r, float g, float b) {
    overlayLines_.push_back(HelperVertex{{ax, ay, az}, {r, g, b}});
    overlayLines_.push_back(HelperVertex{{bx, by, bz}, {r, g, b}});
  };
  auto addBox = [&](const float mn[3], const float mx[3], float r, float g, float b) {
    const float xs[2] = {mn[0], mx[0]}, ys[2] = {mn[1], mx[1]}, zs[2] = {mn[2], mx[2]};
    for (int yi = 0; yi < 2; ++yi)
      for (int zi = 0; zi < 2; ++zi)
        addLine(xs[0], ys[yi], zs[zi], xs[1], ys[yi], zs[zi], r, g, b);
    for (int xi = 0; xi < 2; ++xi)
      for (int zi = 0; zi < 2; ++zi)
        addLine(xs[xi], ys[0], zs[zi], xs[xi], ys[1], zs[zi], r, g, b);
    for (int xi = 0; xi < 2; ++xi)
      for (int yi = 0; yi < 2; ++yi)
        addLine(xs[xi], ys[yi], zs[0], xs[xi], ys[yi], zs[1], r, g, b);
  };

  // Grid/axes extent from scene bounds. A 1:1 extent (half == the subject's
  // own footprint) reads as a tight platform ending right at its feet rather
  // than ground extending into the distance -- most visible on a small,
  // human-scale subject (e.g. a ~1.7-unit-tall character got a 4x4 unit
  // grid). Pad with a flat margin (proportionally large for small scenes,
  // a small fraction of a big one) and floor at 5 units so even a tiny scene
  // still shows a reasonable amount of ground.
  float half = 10.0f;
  if (draw_ && draw_->hasBounds) {
    float ex = 0.0f;
    for (int i = 0; i < 3; ++i)
      ex = std::max(ex, draw_->aabbMax[i] - draw_->aabbMin[i]);
    if (ex > 1e-3f) half = std::max(5.0f, std::ceil(ex) + 3.0f);
  }
  // Auto-clip's far plane is derived from the SUBJECT's bounding sphere, which
  // on a small subject is much tighter than this (deliberately padded) helper
  // extent -- tell the camera how far the helpers actually reach so it does not
  // slice the grid off. The grid is a square of half-extent `half` centred on
  // the world origin, so its farthest point is the corner at half*sqrt(2).
  if (cam_) {
    cam_->setHelperRadius((showGrid_ || showAxes_) ? half * 1.4143f : 0.0f);
  }

  if (showGrid_) {
    const int n = 10;
    const float step = half / static_cast<float>(n);
    const float gc = 0.32f;
    for (int i = -n; i <= n; ++i) {
      const float t = static_cast<float>(i) * step;
      if (zUp) {  // ground = XY plane (z = 0)
        addLine(-half, t, 0, half, t, 0, gc, gc, gc);
        addLine(t, -half, 0, t, half, 0, gc, gc, gc);
      } else {  // ground = XZ plane (y = 0)
        addLine(-half, 0, t, half, 0, t, gc, gc, gc);
        addLine(t, 0, -half, t, 0, half, gc, gc, gc);
      }
    }
  }
  if (showAxes_) {
    const float L = half;
    addLine(0, 0, 0, L, 0, 0, 0.90f, 0.20f, 0.20f);  // X red
    addLine(0, 0, 0, 0, L, 0, 0.20f, 0.85f, 0.25f);  // Y green
    addLine(0, 0, 0, 0, 0, L, 0.30f, 0.50f, 1.00f);  // Z blue
  }
  if (showSceneBbox_ && draw_ && draw_->hasBounds) {
    addBox(draw_->aabbMin, draw_->aabbMax, 0.90f, 0.90f, 0.30f);
  }
  if (selectionHighlightMode_ == 1 && draw_ && selMeshIndex_ >= 0 &&
      static_cast<size_t>(selMeshIndex_) < draw_->meshes.size() &&
      meshVisibleForView(static_cast<size_t>(selMeshIndex_))) {
    const auto& m = draw_->meshes[static_cast<size_t>(selMeshIndex_)];
    addBox(m.aabbMin, m.aabbMax, 1.00f, 0.60f, 0.10f);
  }

  // Extent attribute bbox
  if (showExtent_ && loaded_ && selPrim_) {
    lightusd::Property extentProp;
    std::string extentErr;
    if (tydra::GetProperty(*selPrim_, "extent", &extentProp, &extentErr)) {
      if (extentProp.is_attribute()) {
        const auto& attr = extentProp.get_attribute();
        if (attr.has_value()) {
          const auto& v = attr.get_var().value_raw();
          const float* vf = v.as<float>();
          if (vf) {
            // extent is typically vec3f[2] = {min, max}
            float mn[3] = {vf[0], vf[1], vf[2]};
            float mx[3] = {vf[3], vf[4], vf[5]};
            addBox(mn, mx, 0.30f, 1.00f, 0.50f);
          }
        }
      }
    }
  }

  // Light and camera gizmos
  if ((showLights_ || showCameras_) && loaded_ && loaded_->ok) {
    std::unordered_map<int, std::array<float, 16>> lightXforms;
    std::unordered_map<int, std::array<float, 16>> camXforms;
    for (const auto& n : loaded_->render.nodes) {
      CollectLightCameraTransforms(n, lightXforms, camXforms);
    }
    if (showLights_) {
      BuildLightGizmos(loaded_->render, lightXforms, helperLines_);
    }
    if (showCameras_) {
      BuildCameraGizmos(loaded_->render, camXforms, helperLines_);
    }
  }

  // UsdSkel joint hierarchy as bone line segments + a small cross per joint.
  // Draw the same animated joint hierarchy used for skinning, then apply the
  // Skeleton prim transform to match the rendered character placement.
  if (showSkeleton_ && loaded_ && loaded_->ok) {
    std::unordered_map<std::string, light3d::Mat4> nodeWorlds;
    std::function<void(const tydra::Node&)> collectNodeWorlds =
        [&](const tydra::Node& node) {
          light3d::Mat4 world;
          for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
              world.m[r * 4 + c] = static_cast<float>(node.global_matrix.m[r][c]);
          nodeWorlds.emplace(node.abs_path, world);
          for (const tydra::Node& child : node.children) collectNodeWorlds(child);
        };
    for (const tydra::Node& root : loaded_->render.nodes) collectNodeWorlds(root);

    // Joint positions are in skeleton space and are placed by the Skeleton
    // prim itself. They must not inherit a bound mesh's current transform.
    auto skelWorld = [&](size_t si, bool densePointSamples) -> light3d::Mat4 {
      (void)densePointSamples;
      if (si < loaded_->render.skeletons.size()) {
        auto it = nodeWorlds.find(loaded_->render.skeletons[si].abs_path);
        if (it != nodeWorlds.end()) return it->second;
      }
      return light3d::Mat4::identity();
    };

    const auto& skels = loaded_->render.skeletons;
    for (size_t si = 0; si < skels.size(); ++si) {
      const auto& skel = skels[si];
      const size_t nj = skel.num_joints();
      if (nj == 0 || skel.bind_transforms.size() != nj ||
          skel.parent_joint_indices.size() != nj) {
        continue;  // empty or malformed topology; skip
      }
      const bool densePointSamples = nj >= 1024;
      if (densePointSamples && draw_) {
        float sceneDiag = 1.0f;
        if (draw_->hasBounds) {
          const light3d::Vec3 smn{draw_->aabbMin[0], draw_->aabbMin[1], draw_->aabbMin[2]};
          const light3d::Vec3 smx{draw_->aabbMax[0], draw_->aabbMax[1], draw_->aabbMax[2]};
          sceneDiag = std::max(light3d::length(smx - smn), 1.0e-5f);
        }
        const float cs = std::max(sceneDiag * 0.0006f, 1e-5f);
        bool drewDenseSamples = false;
        for (const auto& dm : draw_->meshes) {
          if (dm.skelId != static_cast<int>(si)) continue;
          light3d::Mat4 W;
          for (int k = 0; k < 16; ++k) W.m[k] = dm.world[k];
          const bool hasSkinnedSamples =
              !dm.skinnedHelperPoints.empty() && dm.skinnedHelperPoints.size() % 3 == 0;
          const size_t count = hasSkinnedSamples ? (dm.skinnedHelperPoints.size() / 3)
                                                 : dm.vertices.size();
          for (size_t i = 0; i < count; ++i) {
            light3d::Vec3 local;
            if (hasSkinnedSamples) {
              local = {dm.skinnedHelperPoints[i * 3 + 0],
                       dm.skinnedHelperPoints[i * 3 + 1],
                       dm.skinnedHelperPoints[i * 3 + 2]};
            } else {
              const DrawVertex& v = dm.vertices[i];
              local = {v.px, v.py, v.pz};
            }
            const light3d::Vec3 p = light3d::transformPoint(W, local);
            addOverlay(p.x - cs, p.y, p.z, p.x + cs, p.y, p.z, 0.20f, 0.95f, 0.95f);
            drewDenseSamples = true;
          }
        }
        if (drewDenseSamples) continue;
      }
      const light3d::Mat4 W = skelWorld(si, densePointSamples);
      std::vector<lightusd::value::matrix4d> jointWorlds;
      if (!BuildSkeletonJointWorlds(loaded_->render, static_cast<int>(si),
                                    timeline_.applied, &jointWorlds) ||
          jointWorlds.size() != nj) {
        jointWorlds = skel.bind_transforms;
      }

      // World-space joint positions + this skeleton's own AABB.
      std::vector<light3d::Vec3> jp(nj);
      light3d::Vec3 mn{1e30f, 1e30f, 1e30f};
      light3d::Vec3 mx{-1e30f, -1e30f, -1e30f};
      for (size_t i = 0; i < nj; ++i) {
        jp[i] = light3d::transformPoint(
            W, {static_cast<float>(jointWorlds[i].m[3][0]),
                static_cast<float>(jointWorlds[i].m[3][1]),
                static_cast<float>(jointWorlds[i].m[3][2])});
        mn.x = std::min(mn.x, jp[i].x); mn.y = std::min(mn.y, jp[i].y);
        mn.z = std::min(mn.z, jp[i].z);
        mx.x = std::max(mx.x, jp[i].x); mx.y = std::max(mx.y, jp[i].y);
        mx.z = std::max(mx.z, jp[i].z);
      }
      // Stable marker size: dense point-joint rigs can have animated extents,
      // so derive cross size from the scene bounds to avoid helper wobble.
      float sceneDiag = light3d::length(mx - mn);
      if (draw_ && draw_->hasBounds) {
        const light3d::Vec3 smn{draw_->aabbMin[0], draw_->aabbMin[1], draw_->aabbMin[2]};
        const light3d::Vec3 smx{draw_->aabbMax[0], draw_->aabbMax[1], draw_->aabbMax[2]};
        sceneDiag = light3d::length(smx - smn);
      }
      const float cs = std::max(sceneDiag * (nj >= 1024 ? 0.002f : 0.02f), 1e-5f);

      for (size_t i = 0; i < nj; ++i) {
        const light3d::Vec3& p = jp[i];
        // Small axis cross so leaf/isolated joints are visible too.
        addOverlay(p.x - cs, p.y, p.z, p.x + cs, p.y, p.z, 0.20f, 0.95f, 0.95f);
        addOverlay(p.x, p.y - cs, p.z, p.x, p.y + cs, p.z, 0.20f, 0.95f, 0.95f);
        addOverlay(p.x, p.y, p.z - cs, p.x, p.y, p.z + cs, 0.20f, 0.95f, 0.95f);
        const int par = skel.parent_joint_indices[i];
        if (!densePointSamples && par >= 0 && static_cast<size_t>(par) < nj) {
          const light3d::Vec3& pp = jp[static_cast<size_t>(par)];
          addOverlay(pp.x, pp.y, pp.z, p.x, p.y, p.z, 0.10f, 0.80f, 0.90f);
        }
      }
    }
  }

  // Translation gizmo at the selected mesh world position.
  if (xformMode_ == TransformMode::Translate && draw_ && selMeshIndex_ >= 0 &&
      static_cast<size_t>(selMeshIndex_) < draw_->meshes.size()) {
    const float* W = draw_->meshes[static_cast<size_t>(selMeshIndex_)].world;
    float tx = W[12], ty = W[13], tz = W[14];
    float gizmoLen = 1.0f;
    if (draw_->hasBounds) {
      const light3d::Vec3 smn{draw_->aabbMin[0], draw_->aabbMin[1], draw_->aabbMin[2]};
      const light3d::Vec3 smx{draw_->aabbMax[0], draw_->aabbMax[1], draw_->aabbMax[2]};
      gizmoLen = std::max(light3d::length(smx - smn) * 0.15f, 0.5f);
    }
    const struct { float dx, dy, dz; float r, g, b; } axes[4] = {
      {gizmoLen, 0, 0, 0.9f, 0.2f, 0.2f},
      {0, gizmoLen, 0, 0.2f, 0.9f, 0.2f},
      {0, 0, gizmoLen, 0.2f, 0.3f, 0.9f},
      {-gizmoLen * 0.3f, 0, 0, 0.6f, 0.1f, 0.1f},
    };
    for (int i = 0; i < 4; ++i) {
      float ex = tx + axes[i].dx, ey = ty + axes[i].dy, ez = tz + axes[i].dz;
      float r = axes[i].r, g = axes[i].g, b = axes[i].b;
      if (gizmoActive_ && i == gizmoAxis_) { r = 1.0f; g = 1.0f; b = 0.0f; }
      else if (!gizmoActive_ && i == gizmoAxis_) {
        r = std::min(1.0f, r * 1.5f); g = std::min(1.0f, g * 1.5f); b = std::min(1.0f, b * 1.5f);
      }
      addOverlay(tx, ty, tz, ex, ey, ez, r, g, b);
      if (i < 3) {
        float nx = axes[i].dx / gizmoLen, ny = axes[i].dy / gizmoLen, nz = axes[i].dz / gizmoLen;
        float perpX = 1.0f - std::abs(nx), perpY = 1.0f - std::abs(ny), perpZ = 1.0f - std::abs(nz);
        float plen = std::sqrt(perpX*perpX + perpY*perpY + perpZ*perpZ);
        if (plen > 1e-6f) { perpX /= plen; perpY /= plen; perpZ /= plen; }
        float coneLen = gizmoLen * 0.12f, coneW = gizmoLen * 0.06f;
        for (int j = 0; j < 2; ++j) {
          float sign = (j == 0) ? 1.0f : -1.0f;
          addOverlay(ex, ey, ez,
                     ex - nx * coneLen + perpX * sign * coneW,
                     ey - ny * coneLen + perpY * sign * coneW,
                     ez - nz * coneLen + perpZ * sign * coneW, r, g, b);
        }
      }
    }
  }
}

void Gui::drawStageMeta() {
  ImGui::Begin("Stage");
  if (loaded_ && loaded_->ok) {
    const auto& meta = loaded_->render.meta;
    metaFilter_.Draw("Search##meta", -1.0f);  // key / value
    if (ImGui::BeginTable("##stagemeta", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 9.0f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();
      auto row = [&](const char* k, const std::string& v) {
        if (metaFilter_.IsActive() && !metaFilter_.PassFilter(k) &&
            !metaFilter_.PassFilter(v.c_str())) {
          return;  // filtered out
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(k);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", v.c_str());
      };
      if (nextStage_) {
        PublicStageInfo info;
        if (ReadPublicStageInfo(nextStage_.get(), &info)) {
          row("upAxis", info.upAxis);
          row("metersPerUnit", std::to_string(info.metersPerUnit));
          row("framesPerSecond", std::to_string(info.framesPerSecond));
          row("timeCodesPerSecond", std::to_string(info.timeCodesPerSecond));
          if (info.startTimeCodeAuthored) row("startTimeCode", std::to_string(info.startTimeCode));
          if (info.endTimeCodeAuthored) row("endTimeCode", std::to_string(info.endTimeCode));
          if (!info.defaultPrim.empty()) row("defaultPrim", info.defaultPrim);
          if (!info.comment.empty()) row("comment", info.comment);
          if (!info.documentation.empty()) row("documentation", info.documentation);
        }
        lightusd::api::StringList layers;
        if (lightusd_stage_sublayers(nextStage_.get(), layers.put()) == LIGHTUSD_OK &&
            lightusd::api::StringListSize(layers)) {
          std::string sublayers;
          for (size_t i = 0; i < lightusd::api::StringListSize(layers); ++i) {
            if (!sublayers.empty()) sublayers += "\n";
            sublayers += PublicString(lightusd::api::StringListGet(layers, i));
          }
          row("subLayers", sublayers);
        }
      } else {
        const auto& smeta = loaded_->stage.metas();
        row("upAxis", meta.upAxis);
        row("metersPerUnit", std::to_string(meta.metersPerUnit));
        row("framesPerSecond", std::to_string(meta.framesPerSecond));
        row("timeCodesPerSecond", std::to_string(meta.timeCodesPerSecond));
        if (meta.startTimeCode.has_value())
          row("startTimeCode", std::to_string(*meta.startTimeCode));
        if (meta.endTimeCode.has_value())
          row("endTimeCode", std::to_string(*meta.endTimeCode));
        if (!smeta.defaultPrim.str().empty())
          row("defaultPrim", smeta.defaultPrim.str());
        if (!meta.copyright.empty()) row("copyright", meta.copyright);
        if (!meta.comment.empty()) row("comment", meta.comment);
        if (!smeta.doc.value.empty()) row("documentation", smeta.doc.value);
      }
      ImGui::EndTable();
    }
  } else {
    HintWrapped("No stage loaded.");
  }
  ImGui::End();
}

}  // namespace lusdview
