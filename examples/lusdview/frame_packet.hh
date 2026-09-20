// SPDX-License-Identifier: Apache-2.0
// A self-contained snapshot of one frame's render inputs, handed from the main/UI
// thread to the render thread (experimental threaded GL rendering, gated behind
// LUSDVIEW_ENABLE_GL_THREAD). Everything is owned by the packet (deep copies), so
// the main thread may proceed to the next frame while the render thread renders
// this one. See app.cc render thread + gui.cc packet build.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gpu_scene.hh"  // HelperVertex
#include "renderer.hh"   // RenderMode, RenderFrameParams

struct ImDrawData;

namespace lusdview {

struct FramePacket {
  float view[16]{};
  float proj[16]{};
  float cameraPos[3]{0, 0, 0};
  float exposure{0.0f};
  float materialXTime{0.0f};
  float materialXFrame{0.0f};
  RtCameraLens cameraLens;
  RtCameraShutter cameraShutter;
  bool rtTemporalPose{false};
  bool rtTemporalReset{false};
  float clippingPlanes[RenderFrameParams::kMaxClippingPlanes * 4]{};
  int clippingPlaneCount{0};
  PathTraceSettings pathTrace;
  RenderMode mode{RenderMode::Shaded};
  float clearColor[4]{0.12f, 0.12f, 0.13f, 1.0f};
  float lightDir[3]{0.40160966f, 0.64257544f, 0.48193160f};
  float lightColor[3]{1.0f, 1.0f, 1.0f};
  float depthScale{1.0f};
  float sceneMin[3]{0, 0, 0};
  float sceneExtent[3]{1, 1, 1};
  int highlightMeshIndex{-1};
  std::vector<uint32_t> highlightIndices;
  std::vector<HelperVertex> highlightLines;
  bool highlightXray{false};
  std::vector<HelperVertex> helperLines;
  std::vector<HelperVertex> overlayLines;
  std::vector<uint8_t> meshVisible;
  std::vector<uint8_t> carrierVisible;
  std::vector<uint8_t> rtMeshVisible;
  uint32_t purposeVisibleMask{0x3u};
  int curveMaxSegments{8};
  int viewportW{0};
  int viewportH{0};
  int fbW{0};
  int fbH{0};
  bool hasParams{false};       // false on the first frames before a scene/camera
  ImDrawData* drawData{nullptr};  // deep-cloned ImGui output; freed by the render thread
  bool wantCapture{false};        // render thread reads back the frame after present
  std::uint64_t seq{0};
  std::uint64_t sceneKey{0};  // render-affecting state; unchanged => reuse viewport

  // Reconstruct a RenderFrameParams whose pointers reference this packet's buffers.
  RenderFrameParams params() const {
    RenderFrameParams p;
    p.view = view;
    p.proj = proj;
    p.cameraPos[0] = cameraPos[0];
    p.cameraPos[1] = cameraPos[1];
    p.cameraPos[2] = cameraPos[2];
    p.exposure = exposure;
    p.materialXTime = materialXTime;
    p.materialXFrame = materialXFrame;
    p.cameraLens = cameraLens;
    p.cameraShutter = cameraShutter;
    p.rtTemporalPose = rtTemporalPose;
    p.rtTemporalReset = rtTemporalReset;
    p.clippingPlaneCount = clippingPlaneCount;
    std::copy(clippingPlanes,
              clippingPlanes + RenderFrameParams::kMaxClippingPlanes * 4,
              p.clippingPlanes);
    p.pathTrace = pathTrace;
    p.mode = mode;
    for (int i = 0; i < 4; ++i) p.clearColor[i] = clearColor[i];
    for (int i = 0; i < 3; ++i) {
      p.lightDir[i] = lightDir[i];
      p.lightColor[i] = lightColor[i];
    }
    p.depthScale = depthScale;
    for (int i = 0; i < 3; ++i) { p.sceneMin[i] = sceneMin[i]; p.sceneExtent[i] = sceneExtent[i]; }
    p.highlightMeshIndex = highlightMeshIndex;
    p.highlightIndices = highlightIndices.empty() ? nullptr : highlightIndices.data();
    p.highlightIndexCount = static_cast<int>(highlightIndices.size());
    p.highlightLines = highlightLines.empty() ? nullptr : highlightLines.data();
    p.highlightLineVertexCount = static_cast<int>(highlightLines.size());
    p.highlightXray = highlightXray;
    p.helperLines = helperLines.empty() ? nullptr : helperLines.data();
    p.helperLineVertexCount = static_cast<int>(helperLines.size());
    p.overlayLines = overlayLines.empty() ? nullptr : overlayLines.data();
    p.overlayLineVertexCount = static_cast<int>(overlayLines.size());
    p.meshVisible = meshVisible.empty() ? nullptr : meshVisible.data();
    p.meshVisibleCount = static_cast<int>(meshVisible.size());
    p.carrierVisible = carrierVisible.empty() ? nullptr : carrierVisible.data();
    p.carrierVisibleCount = static_cast<int>(carrierVisible.size());
    p.rtMeshVisible = rtMeshVisible.empty() ? nullptr : rtMeshVisible.data();
    p.rtMeshVisibleCount = static_cast<int>(rtMeshVisible.size());
    p.purposeVisibleMask = purposeVisibleMask;
    p.curveMaxSegments = curveMaxSegments;
    return p;
  }
};

// Deep-copy ImGui draw data (its internal buffers are reused next NewFrame, so the
// render thread needs an owned copy). Free with FreeImDrawData on the render thread
// after ImGui_ImplOpenGL3_RenderDrawData. Returns nullptr for a null/empty source.
ImDrawData* CloneImDrawData(const ImDrawData* src);
void FreeImDrawData(ImDrawData* dd);

}  // namespace lusdview
