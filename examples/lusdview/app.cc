// SPDX-License-Identifier: Apache-2.0
#include "app.hh"

#include <glad/glad.h>
//
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>  // sysconf (RSS page size for the post-RT-build free log)
#endif

#include "cascadia_mono.h"  // CascadiaMono_compressed_data / _size
#include "config.hh"
#include "dome_light.hh"
#include "gpu_budget_lod.hh"
#include "lod_stream.hh"
#include "gui_style.hh"
#include "image-writer.hh"
#include "external/stb_image_resize2.h"  // stbir_resize (impl lives in the lib)
#include "external/jsonhpp/nlohmann/json.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "log.hh"
#include "lightrt_mtlx_bridge.hh"
#include "mesh_build.hh"
#include "next_scene_loader.hh"
#include "next/lightusd-next.hh"  // tnext::Stage (per-frame --next morph weights)
#include "scene_validation.hh"
#include "skinning.hh"
#include "texture_residency_policy.hh"
#include "vchar_control_map.hh"
#if defined(HAVE_VULKAN)
#include "vk/vk_renderer.hh"
#endif

#if defined(HAVE_NFD)
#include "nfd.h"
#endif

namespace lusdview {

namespace {
bool AppendPtexFaceTableUpdates(
    const DrawTextureCPU& texture, uint32_t face,
    const DrawPtexFaceRectCPU& rect,
    std::vector<Renderer::TextureRegionUpdate>* updates);

void GlfwErrorCallback(int code, const char* desc) {
  LOGE("glfw error %d: %s", code, desc);
}

size_t EstimateTextureResidentBytes(const DrawTextureCPU& texture) {
  size_t compressedBytes = texture.compressed.data.size();
  for (const DrawCompressedMipCPU& mip : texture.compressed.mips)
    compressedBytes += mip.data.size();
  if (compressedBytes > 0) return compressedBytes;
  size_t bytes = size_t(std::max(texture.image.width, 0)) *
                 size_t(std::max(texture.image.height, 0)) * 4u;
  for (const light3d::Image& mip : texture.mipImages) {
    bytes += size_t(std::max(mip.width, 0)) *
             size_t(std::max(mip.height, 0)) * 4u;
  }
  if (texture.mipImages.empty() && !texture.streamingMutable) bytes += bytes / 3u;
  return bytes;
}

void AppendMaterialTextureSlots(const DrawMaterialCPU& material,
                                size_t textureCount,
                                std::vector<int>* slots) {
  const int ids[] = {
      material.baseColorTex,      material.metallicTex,
      material.roughnessTex,      material.normalTex,
      material.coatNormalTex,     material.emissiveTex,
      material.opacityTex,        material.occlusionTex,
      material.specularColorTex,  material.coatWeightTex,
      material.coatColorTex,      material.coatRoughnessTex,
      material.displacementTex,
  };
  for (int id : ids) {
    if (id >= 0 && static_cast<size_t>(id) < textureCount) slots->push_back(id);
  }
}

void ReleaseOrdinaryTexturePayload(DrawTextureCPU* texture) {
  if (!texture || texture->isUdim || texture->isPtex) return;
  texture->image.data.clear();
  texture->image.data.shrink_to_fit();
  texture->mipImages.clear();
  texture->mipImages.shrink_to_fit();
  texture->compressed.data.clear();
  texture->compressed.data.shrink_to_fit();
  texture->compressed.mips.clear();
  texture->compressed.mips.shrink_to_fit();
}

// True when a texture's pixels can be recovered later by re-decoding its source
// asset (DecodeDeferredDrawTexture). Everything that drops a CPU payload below
// assumes that is always possible, but it is not: DecodeDeferredDrawTextureImpl
// re-opens assetIdentifier as a standalone file, so a texture embedded in a
// USDZ *archive* has nothing to re-open and its pixels are gone for good once
// released. That silently broke the CUDA/HIP/CPU tracers, which build their
// texture table from these same draw_.textures payloads: after the interactive
// loader had streamed and released them, an RT build saw "1 source texture(s),
// 0 descriptor(s)" and traced an untextured (flat grey) scene.
bool TextureCanRedecodeFromSource(const DrawTextureCPU& texture) {
  if (texture.assetIdentifier.empty() || texture.isUdim || texture.isPtex) {
    return false;
  }
  std::error_code ec;
  const bool exists = std::filesystem::exists(texture.assetIdentifier, ec);
  return exists && !ec;
}

}  // anonymous namespace

std::vector<int> App::selectedTextureSlots() const {
  std::vector<int> slots;
  const int meshIndex = gui_.selectedMeshIndex();
  if (meshIndex < 0 ||
      static_cast<size_t>(meshIndex) >= textureSlotsByMesh_.size())
    return slots;
  return textureSlotsByMesh_[static_cast<size_t>(meshIndex)];
}

void App::resetTextureResidency() {
  // A reload joins the scene loader first. At most four independent filesystem
  // decodes can still be finishing here; consuming them keeps future teardown
  // deterministic and prevents results crossing scene generations.
  for (std::future<TextureDecodeResult>& job : textureDecodeJobs_) {
    if (job.valid()) job.wait();
  }
  textureDecodeJobs_.clear();
  textureDecodeRawBytes_ = 0;
  textureDecodeGpuBytes_ = 0;
  texturePeakResidentBytes_ = 0;
  textureCoarseUploads_ = 0;
  textureFullUploads_ = 0;
  textureDecodeFailures_ = 0;
  textureEvictions_ = 0;
  textureResidency_.assign(draw_.textures.size(), TextureResidencySlot{});
  textureSlotsByMesh_.clear();
  textureSlotsByMesh_.resize(draw_.meshes.size());
  textureMarginVisible_.clear();
  textureCameraSignatureValid_ = false;
  for (size_t meshIndex = 0; meshIndex < draw_.meshes.size(); ++meshIndex) {
    std::vector<int>& slots = textureSlotsByMesh_[meshIndex];
    for (const DrawSubmesh& submesh : draw_.meshes[meshIndex].submeshes) {
      if (submesh.materialId < 0 ||
          static_cast<size_t>(submesh.materialId) >= draw_.materials.size())
        continue;
      AppendMaterialTextureSlots(
          draw_.materials[static_cast<size_t>(submesh.materialId)],
          draw_.textures.size(), &slots);
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  }
  for (size_t i = 0; i < draw_.textures.size(); ++i) {
    if (!draw_.textures[i].deferredDecode) {
      textureResidency_[i].state = TextureResidencyState::FullResident;
      textureResidency_[i].residentBytes =
          renderThreadActive_
              ? EstimateTextureResidentBytes(draw_.textures[i])
              : renderer_->textureResidentBytes(static_cast<int>(i));
    }
  }
}

void App::updateTextureResidency() {
  if (!renderer_ || textureResidency_.empty()) return;
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();

  // Publish completed work without blocking the frame.
  for (size_t i = 0; i < textureDecodeJobs_.size();) {
    std::future<TextureDecodeResult>& job = textureDecodeJobs_[i];
    if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++i;
      continue;
    }
    TextureDecodeResult ready = job.get();
    TextureResidencySlot& state = textureResidency_[ready.slot];
    if (ready.generation != state.generation) {
      // The user released the slot while this decode was in flight.
    } else if (!ready.ok) {
      ++textureDecodeFailures_;
      state.state = state.residentBytes > 0
                        ? TextureResidencyState::CoarseResident
                        : TextureResidencyState::Failed;
    } else {
      textureDecodeRawBytes_ += ready.texture.image.data.size();
      textureDecodeGpuBytes_ += EstimateTextureResidentBytes(ready.texture);
      if (ready.full)
        ++textureFullUploads_;
      else
        ++textureCoarseUploads_;
      size_t residentBytes = 0;
      if (renderThreadActive_) {
        residentBytes = EstimateTextureResidentBytes(ready.texture);
        auto upload =
            std::make_shared<DrawTextureCPU>(std::move(ready.texture));
        // Keep identity/dimensions for UI and future source re-decode, while the
        // render-thread closure exclusively owns the large staging payload.
        DrawTextureCPU& retained = draw_.textures[ready.slot];
        retained.image.width = upload->image.width;
        retained.image.height = upload->image.height;
        retained.image.channels = upload->image.channels;
        retained.deferredDecode = false;
        retained.requestedCompressed = upload->requestedCompressed;
        retained.compressed.format = upload->compressed.format;
        retained.compressed.width = upload->compressed.width;
        retained.compressed.height = upload->compressed.height;
        postGpu([this, slot = ready.slot, upload = std::move(upload)] {
          renderer_->uploadTexture(static_cast<int>(slot), *upload);
        });
      } else {
        renderer_->uploadTexture(static_cast<int>(ready.slot), ready.texture);
        residentBytes =
            renderer_->textureResidentBytes(static_cast<int>(ready.slot));
      }
      state.state = ready.full ? TextureResidencyState::FullResident
                               : TextureResidencyState::CoarseResident;
      state.residentBytes = residentBytes;
      state.lastWanted = now;
      if (loadOpts_.timing) {
        LOGI("texture residency: slot %zu %s resident (%.2f MiB)",
             ready.slot, ready.full ? "full" : "coarse",
             double(residentBytes) / (1024.0 * 1024.0));
      }
      if (!renderThreadActive_) {
        draw_.textures[ready.slot] = std::move(ready.texture);
        // The backend has consumed or retained what it needs. Keep only source
        // identity and sampling metadata in the application; eviction/full-res
        // promotion re-decodes the ordinary file instead of pinning a duplicate
        // CPU block/raw payload for every resident texture -- but only when
        // that re-decode is actually possible (see TextureCanRedecodeFromSource).
        if (TextureCanRedecodeFromSource(draw_.textures[ready.slot])) {
          ReleaseOrdinaryTexturePayload(&draw_.textures[ready.slot]);
        }
      }
      loadCtrl_.texturesDone.fetch_add(1);
    }
    textureDecodeJobs_.erase(textureDecodeJobs_.begin() +
                             static_cast<std::ptrdiff_t>(i));
  }

  const std::vector<int> selected = selectedTextureSlots();
  std::vector<uint8_t> selectedMask(textureResidency_.size(), uint8_t{0});
  for (int slot : selected) selectedMask[static_cast<size_t>(slot)] = 1;
  if (gui_.wantRefineSelectedTextures()) {
    for (int slot : selected) textureResidency_[static_cast<size_t>(slot)].forceFull = true;
  }
  if (gui_.wantReleaseSelectedTextures()) {
    for (int slot : selected) {
      TextureResidencySlot& state = textureResidency_[static_cast<size_t>(slot)];
      ++state.generation;
      // Releasing sets deferredDecode so the pixels come back on demand; a
      // texture that cannot be re-decoded (archive-embedded, see
      // TextureCanRedecodeFromSource) would instead be lost for good, so it
      // stays resident.
      if (state.residentBytes > 0 &&
          TextureCanRedecodeFromSource(
              draw_.textures[static_cast<size_t>(slot)])) {
        postGpu([this, slot] { renderer_->evictTexture(slot); });
        ++textureEvictions_;
        DrawTextureCPU& texture = draw_.textures[static_cast<size_t>(slot)];
        ReleaseOrdinaryTexturePayload(&texture);
        texture.deferredDecode = true;
      }
      const uint64_t generation = state.generation;
      state = TextureResidencySlot{};
      state.generation = generation;
    }
  }
  backgroundTextureRefinement_ = gui_.backgroundTextureRefinement();

  // Slot priority: selected, visible frustum, 20%-expanded frustum, background.
  std::vector<TextureDemandPriority> priority(
      textureResidency_.size(),
      backgroundTextureRefinement_ ? TextureDemandPriority::Background
                                   : TextureDemandPriority::None);
  const std::vector<uint8_t>& visible = gui_.viewVisibility();
  const light3d::Vec3 eye = camera_.eye();
  const light3d::Vec3 target = camera_.target();
  const std::array<float, 15> cameraSignature = {
      eye.x,
      eye.y,
      eye.z,
      target.x,
      target.y,
      target.z,
      camera_.fovYDeg(),
      camera_.aspect(),
      camera_.orthographicHeight(),
      camera_.lensShiftX(),
      camera_.lensShiftY(),
      static_cast<float>(camera_.projection() == CameraProjection::Orthographic),
      static_cast<float>(camera_.conform()),
      camera_.nearPlane(),
      camera_.farPlane(),
  };
  if (!textureCameraSignatureValid_ ||
      cameraSignature != textureCameraSignature_ ||
      textureMarginVisible_.size() != draw_.meshes.size()) {
    textureCameraSignature_ = cameraSignature;
    textureCameraSignatureValid_ = true;
    textureMarginVisible_.assign(draw_.meshes.size(), uint8_t{0});
    light3d::Mat4 expandedProj = camera_.proj(false);
    expandedProj.m[0] /= 1.2f;
    expandedProj.m[5] /= 1.2f;
    const light3d::Frustum expanded = light3d::Frustum::fromViewProjection(
        expandedProj * camera_.view());
    for (size_t meshIndex = 0; meshIndex < draw_.meshes.size(); ++meshIndex) {
      const DrawMeshCPU& mesh = draw_.meshes[meshIndex];
      const light3d::Vec3 mn{mesh.aabbMin[0], mesh.aabbMin[1], mesh.aabbMin[2]};
      const light3d::Vec3 mx{mesh.aabbMax[0], mesh.aabbMax[1], mesh.aabbMax[2]};
      textureMarginVisible_[meshIndex] =
          expanded.testAABB(mn, mx) != light3d::CullResult::Outside ? 1u : 0u;
    }
  }
  for (size_t meshIndex = 0; meshIndex < draw_.meshes.size(); ++meshIndex) {
    TextureDemandPriority p = TextureDemandPriority::None;
    if (meshIndex < visible.size() && visible[meshIndex]) {
      p = TextureDemandPriority::Visible;
    } else if (meshIndex < textureMarginVisible_.size() &&
               textureMarginVisible_[meshIndex]) {
      p = TextureDemandPriority::Margin;
    }
    if (p != TextureDemandPriority::None &&
        meshIndex < textureSlotsByMesh_.size()) {
      for (int slot : textureSlotsByMesh_[meshIndex]) {
        priority[static_cast<size_t>(slot)] =
            std::min(priority[static_cast<size_t>(slot)], p);
      }
    }
  }
  for (int slot : selected)
    priority[static_cast<size_t>(slot)] = TextureDemandPriority::Selected;
  for (size_t i = 0; i < priority.size(); ++i) {
    if (priority[i] < TextureDemandPriority::Background)
      textureResidency_[i].lastWanted = now;
  }

  // Launch no more than four independent decoders. Selected slots bypass the
  // coarse edge cap; ordinary visible/background work uses the preview profile.
  std::vector<uint8_t> decodeEligible(textureResidency_.size(), uint8_t{0});
  for (size_t i = 0; i < textureResidency_.size(); ++i) {
    const TextureResidencySlot& state = textureResidency_[i];
    const bool wantsFull = selectedMask[i] || state.forceFull;
    const bool canDecode = state.state == TextureResidencyState::Unloaded ||
                           (wantsFull && state.state ==
                                             TextureResidencyState::CoarseResident);
    decodeEligible[i] = canDecode && priority[i] != TextureDemandPriority::None &&
                                !draw_.textures[i].assetIdentifier.empty()
                            ? 1u
                            : 0u;
  }
  const std::vector<size_t> candidates =
      OrderTextureDecodeCandidates(priority, decodeEligible);
  const uint64_t totalBudget = loadOpts_.textureOptions.textureBudgetMB > 0
                                   ? uint64_t(loadOpts_.textureOptions.textureBudgetMB) *
                                         1024ull * 1024ull
                                   : 0;
  const uint64_t perDecodeBudget =
      totalBudget && !textureResidency_.empty()
          ? std::max<uint64_t>(1ull << 20, totalBudget / textureResidency_.size())
          : 0;
  for (size_t slot : candidates) {
    if (textureDecodeJobs_.size() >= 4) break;
    TextureResidencySlot& state = textureResidency_[slot];
    const bool full = selectedMask[slot] || state.forceFull;
    TextureRuntimeOptions runtime = loadOpts_.textureOptions;
    if (full) runtime.maxTextureSize = 0;
    DrawTextureCPU placeholder = draw_.textures[slot];
    placeholder.deferredDecode = true;
    state.state = TextureResidencyState::Decoding;
    const uint64_t generation = state.generation;
    textureDecodeJobs_.push_back(std::async(
        std::launch::async,
        [slot, full, generation, placeholder = std::move(placeholder), runtime,
         perDecodeBudget]() mutable {
          TextureDecodeResult result;
          result.slot = slot;
          result.full = full;
          result.generation = generation;
          result.ok = DecodeDeferredDrawTexture(placeholder, runtime,
                                                full ? 0 : perDecodeBudget,
                                                &result.texture);
          return result;
        }));
  }

  // Budget eviction: selected full-res gets a 10% reserve capped at 1 GiB.
  size_t residentBytes = 0;
  for (const TextureResidencySlot& state : textureResidency_)
    residentBytes += state.residentBytes;
  const size_t baseBudget = static_cast<size_t>(totalBudget);
  size_t selectedResidentBytes = 0;
  for (size_t i = 0; i < textureResidency_.size(); ++i) {
    if (selectedMask[i]) selectedResidentBytes += textureResidency_[i].residentBytes;
  }
  const size_t reserve = selectedResidentBytes > 0
                             ? std::min<size_t>(baseBudget / 10u, 1ull << 30)
                             : 0;
  const size_t allowed = baseBudget ? baseBudget + reserve : 0;
  while (allowed && residentBytes > allowed) {
    std::vector<TextureEvictionCandidate> evictionCandidates;
    evictionCandidates.reserve(textureResidency_.size());
    for (size_t i = 0; i < textureResidency_.size(); ++i) {
      const TextureResidencySlot& state = textureResidency_[i];
      // Protect textures that cannot be re-decoded from their source
      // (archive-embedded; see TextureCanRedecodeFromSource) the same way an
      // explicitly selected one is: evicting them would drop pixels nothing
      // can bring back.
      const bool protectedSlot =
          selectedMask[i] != 0 ||
          (i < draw_.textures.size() &&
           !TextureCanRedecodeFromSource(draw_.textures[i]));
      evictionCandidates.push_back(TextureEvictionCandidate{
          i, std::chrono::duration<double>(now - state.lastWanted).count(),
          state.residentBytes, protectedSlot,
          state.state == TextureResidencyState::Decoding});
    }
    const size_t victimIndex = ChooseTextureEvictionVictim(
        evictionCandidates.data(), evictionCandidates.size(), 2.0);
    if (victimIndex == evictionCandidates.size()) break;
    const size_t victim = evictionCandidates[victimIndex].slot;
    postGpu([this, victim] {
      renderer_->evictTexture(static_cast<int>(victim));
    });
    ++textureEvictions_;
    residentBytes -= textureResidency_[victim].residentBytes;
    DrawTextureCPU& texture = draw_.textures[victim];
    ReleaseOrdinaryTexturePayload(&texture);
    texture.deferredDecode = true;
    textureResidency_[victim] = TextureResidencySlot{};
  }

  Gui::TextureResidencyInfo info;
  info.residentBytes = residentBytes;
  info.budgetBytes = baseBudget;
  info.total = textureResidency_.size();
  info.queued = textureDecodeJobs_.size();
  info.backgroundRefinement = backgroundTextureRefinement_;
  texturePeakResidentBytes_ =
      std::max<uint64_t>(texturePeakResidentBytes_, residentBytes);
  for (const TextureResidencySlot& state : textureResidency_) {
    if (state.residentBytes > 0) ++info.resident;
  }
  gui_.setTextureResidencyInfo(info);
}

void App::writeRenderReport(const std::string& scenePath, int exitCode) const {
  if (renderReportPath_.empty()) return;
  using json = nlohmann::json;
  json report = json::object();
  report["schema_version"] = 2;
  report["status"] = exitCode == 0 ? (draw_.empty() ? "no_scene" : "ok")
                                    : "error";
  report["exit_code"] = exitCode;
  report["scene"] = scenePath;
  report["profile"] = largeSceneProfile_;
  report["camera"] = cameraName_.empty() ? "auto" : cameraName_;
  report["camera_lens"] = {
      {"depth_of_field", cameraLens_.enabled()},
      {"focus_distance", cameraLens_.focusDistance},
      {"aperture_radius", cameraLens_.apertureRadius}};
  const LoadDiagnostics diagnostics =
      CategorizeLoadWarnings(loaded_.warn, draw_.skipped);
  report["load_diagnostics"] = {
      {"degraded_materials", diagnostics.degraded_material},
      {"missing_textures", diagnostics.missing_texture},
      {"unsupported_mtlx", diagnostics.unsupported_mtlx},
      {"unsupported_lobes", diagnostics.unsupported_lobes},
      {"skipped", diagnostics.skipped},
      {"other", diagnostics.other},
      {"actionable", diagnostics.actionable()}};
  report["backend"] = json::object();
  if (renderer_) {
    const RendererCaps& caps = renderer_->caps();
    report["backend"]["name"] = cudaRt_ ? "CUDA" : (hipRt_ ? "HIP" : caps.backend_name);
    const std::string rtDevice =
        cudaRt_ ? cudaTracer_.deviceName()
                : (hipRt_ ? hipTracer_.deviceName() : std::string());
    report["backend"]["device"] = rtDevice.empty() ? caps.gpu_name : rtDevice;
    report["backend"]["api"] = caps.api_info;
    report["backend"]["device_type"] =
        (cudaRt_ || hipRt_) ? "gpu" : caps.device_type;
    if (cudaRt_) {
      report["backend"]["transport"] =
          cudaTracer_.usesOptixTransport() ? "optix" : "software-bvh";
      report["backend"]["transport_requested"] =
          cudaTracer_.rtBackend() == CudaRtBackend::Optix
              ? "optix"
              : (cudaTracer_.rtBackend() == CudaRtBackend::SoftwareBvh
                     ? "software"
                     : "auto");
      report["backend"]["optix_available"] = cudaTracer_.optixAvailable();
      report["backend"]["optix_status"] = cudaTracer_.optixStatus();
      report["backend"]["optix_abi"] = cudaTracer_.optixAbiVersion();
      report["backend"]["optix_gas_bytes"] = cudaTracer_.optixGasBytes();
      report["backend"]["optix_ias_bytes"] = cudaTracer_.optixIasBytes();
      report["backend"]["optix_acceleration_bytes"] =
          cudaTracer_.optixAccelerationBytes();
      report["backend"]["fallback_reason"] =
          cudaTracer_.optixFallbackReason();
    }
    report["backend"]["weighted_oit"] = caps.supportsWeightedOit;
    report["backend"]["weighted_oit_active"] = caps.usesWeightedOit;
    report["backend"]["transparency"] = caps.transparencyMode;
    report["backend"]["oit_attachment_bytes"] = caps.oitAttachmentBytes;
    report["backend"]["oit_draw_calls"] = caps.oitDrawCalls;
    report["backend"]["pipeline_binds"] = caps.pipelineBinds;
    report["backend"]["descriptor_set_binds"] = caps.descriptorSetBinds;
    report["backend"]["rt_shader_variant"] = caps.rtShaderVariant;
    report["backend"]["gpu_rt_ms"] = caps.gpuRtMs;
    report["backend"]["gpu_total_ms"] = caps.gpuTotalMs;
    report["backend"]["gpu_fps"] =
        caps.gpuTotalMs > 0.0 ? 1000.0 / caps.gpuTotalMs : 0.0;
  }
  uint64_t residentTextureBytes = 0;
  uint64_t residentTextureSlots = 0;
  for (const TextureResidencySlot& slot : textureResidency_) {
    residentTextureBytes += slot.residentBytes;
    if (slot.residentBytes > 0) ++residentTextureSlots;
  }
  report["texture_residency"] = {
      {"slots", textureResidency_.size()},
      {"resident_slots", residentTextureSlots},
      {"resident_bytes", residentTextureBytes},
      {"peak_resident_bytes", texturePeakResidentBytes_},
      {"decoded_raw_bytes", textureDecodeRawBytes_},
      {"uploaded_gpu_bytes", textureDecodeGpuBytes_},
      {"coarse_uploads", textureCoarseUploads_},
      {"full_uploads", textureFullUploads_},
      {"decode_failures", textureDecodeFailures_},
      {"evictions", textureEvictions_},
      {"background_refinement", backgroundTextureRefinement_},
  };
  int width = 0, height = 0;
  if (reportCaptureWidth_ > 0 && reportCaptureHeight_ > 0) {
    width = reportCaptureWidth_;
    height = reportCaptureHeight_;
  } else if (renderer_) {
    renderer_->viewportSize(&width, &height);
  }
  report["resolution"] = {{"width", width}, {"height", height}};

  const Gui::RenderStats renderStats = gui_.renderStats();

  size_t ptexTextures = 0, ptexFaces = 0, ptexAtlasBytes = 0;
  size_t ptexDownsampledFaces = 0, ptexFallbackTextures = 0;
  uint64_t ptexCacheHits = 0, ptexCacheMisses = 0, ptexCacheEvictions = 0;
  uint64_t ptexCachePeakBytes = 0;
  uint64_t ptexPageDecodedBytes = 0;
  uint64_t ptexPhysicalSlots = 0, ptexGpuUploads = 0, ptexGpuHits = 0;
  uint64_t ptexGpuMisses = 0, ptexGpuEvictions = 0;
  uint64_t ptexSourceBytes = 0, ptexUniqueSourceBytes = 0;
  std::unordered_set<const void*> ptexSources;
  size_t ptexRequestedFaces = 0;
  for (const auto& requests : ptexRequestedFaces_)
    ptexRequestedFaces += requests.size();
  const size_t ptexConsideredMeshes = static_cast<size_t>(std::count(
      ptexMeshRequested_.begin(), ptexMeshRequested_.end(), uint8_t{1}));
  const size_t ptexRequestedMeshes = static_cast<size_t>(std::count(
      ptexMeshDemanded_.begin(), ptexMeshDemanded_.end(), uint8_t{1}));
  for (const DrawTextureCPU& texture : draw_.textures) {
    if (!texture.isPtex) continue;
    ++ptexTextures;
    ptexFaces += texture.ptexFaceRects.size();
    ptexAtlasBytes += texture.ptexAtlasBytes > 0
                          ? texture.ptexAtlasBytes
                          : texture.image.data.size();
    ptexDownsampledFaces += texture.ptexDownsampledFaces;
    ptexCacheHits += texture.ptexPageCacheHits;
    ptexCacheMisses += texture.ptexPageCacheMisses;
    ptexCacheEvictions += texture.ptexPageCacheEvictions;
    ptexCachePeakBytes =
        std::max(ptexCachePeakBytes, texture.ptexPageCachePeakBytes);
    ptexPageDecodedBytes += texture.ptexPageDecodedBytes;
    ptexPhysicalSlots += texture.ptexPhysicalCacheSlots;
    const std::vector<uint8_t>& source = texture.PtexSourceData();
    ptexSourceBytes += source.size();
    const void* sourceKey = texture.ptexSourceDataShared
                                ? static_cast<const void*>(
                                      texture.ptexSourceDataShared.get())
                                : static_cast<const void*>(source.data());
    if (ptexSources.insert(sourceKey).second) ptexUniqueSourceBytes += source.size();
    ptexGpuUploads += texture.ptexGpuPageUploads;
    ptexGpuHits += texture.ptexGpuPageHits;
    ptexGpuMisses += texture.ptexGpuPageMisses;
    ptexGpuEvictions += texture.ptexGpuPageEvictions;
    if (texture.ptexFaceRects.empty()) ++ptexFallbackTextures;
  }
  size_t ptexDisplacedMeshes = 0;
  for (const DrawMeshCPU& mesh : draw_.meshes)
    if (mesh.rasterDisplacementBaked) ++ptexDisplacedMeshes;
  size_t ptexBase = 0, ptexNormal = 0, ptexRoughness = 0, ptexMetallic = 0;
  size_t ptexOpacity = 0, ptexEmissive = 0, ptexDisplacement = 0;
  size_t ptexOcclusion = 0, ptexSpecular = 0, ptexCoatWeight = 0;
  size_t ptexCoatColor = 0, ptexCoatRoughness = 0;
  for (const DrawMaterialCPU& material : draw_.materials) {
    ptexBase += material.baseColorSample.isPtex ? 1u : 0u;
    ptexNormal += material.normalSample.isPtex ? 1u : 0u;
    ptexRoughness += material.roughnessSample.isPtex ? 1u : 0u;
    ptexMetallic += material.metallicSample.isPtex ? 1u : 0u;
    ptexOpacity += material.opacitySample.isPtex ? 1u : 0u;
    ptexEmissive += material.emissiveSample.isPtex ? 1u : 0u;
    ptexDisplacement += material.displacementSample.isPtex ? 1u : 0u;
    ptexOcclusion += material.occlusionSample.isPtex ? 1u : 0u;
    ptexSpecular += material.specularColorSample.isPtex ? 1u : 0u;
    ptexCoatWeight += material.coatWeightSample.isPtex ? 1u : 0u;
    ptexCoatColor += material.coatColorSample.isPtex ? 1u : 0u;
    ptexCoatRoughness += material.coatRoughnessSample.isPtex ? 1u : 0u;
  }
  report["scene_stats"] = {
      {"meshes", draw_.meshes.size()},
      {"vertices", draw_.vertexCount},
      {"triangles", draw_.triangleCount},
      {"materials", draw_.materials.size()},
      {"source_materials", draw_.optimization.sourceMaterials},
      {"unique_materials", draw_.optimization.uniqueMaterials},
      {"canonical_materials", draw_.optimization.canonicalMaterials},
      {"deduplicated_materials",
       draw_.optimization.deduplicatedMaterials},
      {"textures", draw_.textures.size()},
      {"ptex_textures", ptexTextures},
      {"ptex_initial_faces", loadOpts_.ptexInitialFaces},
      {"ptex_physical_cache_bytes", loadOpts_.ptexPhysicalCacheBytes},
      {"ptex_faces", ptexFaces},
      {"ptex_atlas_bytes", ptexAtlasBytes},
      {"ptex_source_bytes", ptexSourceBytes},
      {"ptex_unique_source_bytes", ptexUniqueSourceBytes},
      {"ptex_downsampled_faces", ptexDownsampledFaces},
      {"ptex_fallback_textures", ptexFallbackTextures},
      {"ptex_page_cache_hits", ptexCacheHits},
      {"ptex_page_cache_misses", ptexCacheMisses},
      {"ptex_page_cache_evictions", ptexCacheEvictions},
      {"ptex_page_cache_peak_bytes", ptexCachePeakBytes},
      {"ptex_page_decoded_bytes", ptexPageDecodedBytes},
      {"ptex_gpu_physical_slots", ptexPhysicalSlots},
      {"ptex_gpu_page_uploads", ptexGpuUploads},
      {"ptex_gpu_page_hits", ptexGpuHits},
      {"ptex_gpu_page_misses", ptexGpuMisses},
      {"ptex_gpu_page_evictions", ptexGpuEvictions},
      {"ptex_requested_faces", ptexRequestedFaces},
      {"ptex_considered_meshes", ptexConsideredMeshes},
      {"ptex_requested_meshes", ptexRequestedMeshes},
      {"ptex_async_jobs_launched", ptexAsyncJobsLaunched_},
      {"ptex_async_jobs_completed", ptexAsyncJobsCompleted_},
      {"ptex_displaced_meshes", ptexDisplacedMeshes},
      {"ptex_slots",
       {{"base_color", ptexBase},
        {"normal", ptexNormal},
        {"roughness", ptexRoughness},
        {"metallic", ptexMetallic},
        {"opacity", ptexOpacity},
        {"emissive", ptexEmissive},
        {"displacement", ptexDisplacement},
        {"occlusion", ptexOcclusion},
        {"specular_color", ptexSpecular},
        {"coat_weight", ptexCoatWeight},
        {"coat_color", ptexCoatColor},
        {"coat_roughness", ptexCoatRoughness}}}};

  size_t gpuUsed = 0, gpuTotal = 0;
  const bool haveGpuMemory =
      renderer_ && renderer_->gpuMemoryMB(&gpuUsed, &gpuTotal);
  size_t peakRssMiB = 0;
#if defined(__linux__)
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) == 0)
    peakRssMiB = static_cast<size_t>(usage.ru_maxrss) / 1024u;
#endif
  report["memory"] = {
      {"host_peak_rss_mib", peakRssMiB},
      {"gpu_memory_available", haveGpuMemory},
      {"gpu_used_mib", gpuUsed},
      {"gpu_total_mib", gpuTotal}};
  const uint32_t vkAccumulatedSamples =
      renderer_ ? renderer_->rayTracingAccumulatedSamples() : 0u;
  const bool rtBuildIncomplete =
      renderer_ && renderer_->rayTracingBuildIncomplete();
  report["render"] = {
      {"integrator", pathTrace_.enabled ? "path" : "preview"},
      {"quality", PathTraceQualityLabel(pathTrace_.quality)},
      {"target_samples", pathTrace_.targetSamples},
      {"min_samples", pathTrace_.minSamples},
      {"max_depth", pathTrace_.maxDepth},
      {"russian_roulette_depth", pathTrace_.russianRouletteDepth},
      {"variance_threshold", pathTrace_.varianceThreshold},
      {"firefly_clamp", pathTrace_.fireflyClamp},
      {"denoise", PathTraceDenoiseLabel(pathTrace_.denoise)},
      {"motion_segments", pathTrace_.motionSegments},
      {"max_subsurface_events", pathTrace_.maxSubsurfaceEvents},
      {"max_volume_events", pathTrace_.maxVolumeEvents},
      {"seed", pathTrace_.seed},
      {"linear_output", linearOutput_},
      {"samples", vkAccumulatedSamples > 0
                      ? vkAccumulatedSamples
                      : (pathTraceRenderedSamples_ > 0
                             ? pathTraceRenderedSamples_
                             : static_cast<uint32_t>(rtSamples_))},
      {"rt_initialization_ms",
       renderer_ ? renderer_->rayTracingInitializationMs() : 0.0},
      {"tlas_chunks", renderer_ ? renderer_->rayTracingTlasChunks() : 0u},
      {"rt_input_instances",
       renderer_ ? renderer_->rayTracingInputInstances() : 0u},
      {"rt_build_incomplete", rtBuildIncomplete},
      {"upload_budget_ms", uploadBudgetMs_},
      {"raster_lod", rasterLodEnabled_},
      {"rt_lod", rtLodEnabled_},
      {"visible_meshes", renderStats.visibleMeshes},
      {"total_meshes", renderStats.totalMeshes},
      {"visible_instances", renderStats.visibleInstances},
      {"total_instances", renderStats.totalInstances},
      {"lod_proxy_instances", renderStats.proxyInstances},
      {"drawn_triangles", renderStats.drawnTriangles},
      {"draw_calls", renderStats.drawCalls},
      {"checkpoint_every", checkpointEvery_},
      {"checkpoint_count", checkpointCount_},
      {"checkpoint_pattern", checkpointPattern_},
      {"elapsed_seconds",
       std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                     runStart_)
           .count()},
      {"cpu_setup_complete_seconds", streamFullConversionSeconds_},
      {"truncated", draw_.truncated || rtBuildIncomplete}};
  report["limits"] = {
      {"gpu_memory_budget_bytes", gpuMemBudgetBytes_},
      {"max_full_meshes", maxFullMeshes_},
      {"rt_max_instances", rtMaxInstances_},
      {"rt_max_tris", cudaMaxTris_}};
  if (cudaRt_) {
    report["cuda_memory"] = {
        {"used_mib", cudaTracer_.deviceUsedBytes() / (1024u * 1024u)},
        {"total_mib", cudaTracer_.deviceTotalBytes() / (1024u * 1024u)}};
  }
  std::vector<std::string> degradationReasons = draw_.skipped;
  if (draw_.truncated) {
    degradationReasons.push_back(
        "scene truncated or proxied by configured render budgets");
  }
  if (rtBuildIncomplete) {
    degradationReasons.push_back(
        "Vulkan RT acceleration structure build incomplete; no partial TLAS "
        "render was accepted");
  }
  if (ptexDownsampledFaces > 0) {
    degradationReasons.push_back(
        "Ptex faces downsampled to fit atlas edge/residency limits");
  }
  if (ptexFallbackTextures > 0) {
    degradationReasons.push_back(
        "Ptex atlas fallback used for one or more textures");
  }
  report["degradation_reasons"] = std::move(degradationReasons);

  std::error_code ec;
  const std::filesystem::path path(renderReportPath_);
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    LOGW("failed to write render report '%s'", renderReportPath_.c_str());
    return;
  }
  output << report.dump(2) << '\n';
  if (!output) {
    LOGW("failed to finish render report '%s'", renderReportPath_.c_str());
  } else {
    LOGI("wrote render report %s", renderReportPath_.c_str());
  }
}

namespace {

// --next blendshape capability: the next loader emits GPU morph channels rather
// than the RenderScene targets SceneHasBlendShapes() looks for.
bool DrawSceneHasMorphChannels(const DrawScene& draw) {
  for (const DrawMeshCPU& m : draw.meshes)
    if (m.morphChannelCount > 0) return true;
  return false;
}

constexpr int kBaseWindowWidth = 1280;
constexpr int kBaseWindowHeight = 800;
constexpr int kMaxGpuTextureInfluences = 256;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

struct AutoSubdivisionView {
  float fovYDeg{60.0f};
  float aspect{1.3333f};
  float yaw{0.6f};
  float pitch{0.35f};
  float quality{1.0f};
  float camDolly{1.0f};
  int viewportHeight{kBaseWindowHeight};
};

void CopyPreviewLightDir(const DrawScene& draw, float out[3]) {
  if (draw.hasPreviewLight) {
    out[0] = draw.previewLightDir[0];
    out[1] = draw.previewLightDir[1];
    out[2] = draw.previewLightDir[2];
  } else {
    out[0] = 0.40160966f;
    out[1] = 0.64257544f;
    out[2] = 0.48193160f;
  }
}

// Write top-down RGBA8 rows as a binary PPM (RGB).
void WritePPM(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return;
  std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = &rgba[static_cast<size_t>(y) * static_cast<size_t>(w) * 4];
    for (int x = 0; x < w; ++x) {
      std::fputc(row[x * 4 + 0], fp);
      std::fputc(row[x * 4 + 1], fp);
      std::fputc(row[x * 4 + 2], fp);
    }
  }
  std::fclose(fp);
}

std::string LowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext;
}

light3d::Vec3 DirFromAngles(float yaw, float pitch, int upAxis) {
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  const float sy = std::sin(yaw);
  const float cy = std::cos(yaw);
  if (upAxis == 2) {
    return light3d::Vec3{sy * cp, cy * cp, sp};
  }
  return light3d::Vec3{sy * cp, sp, cy * cp};
}

bool ValidAabb(const float mn[3], const float mx[3]) {
  for (int a = 0; a < 3; ++a) {
    if (!std::isfinite(mn[a]) || !std::isfinite(mx[a]) || mx[a] < mn[a]) {
      return false;
    }
  }
  return true;
}

// Pick the least enclosed horizontal side for an initial room/environment
// view. Large, thin meshes at the scene boundary provide a format-agnostic
// enclosure estimate; ordinary objects and closed scenes retain the standard
// isometric view.
bool FindOpenSideYaw(const DrawScene& draw, const float mn[3],
                     const float mx[3], int upAxis, float* yaw) {
  if (!yaw || !ValidAabb(mn, mx)) return false;
  const int axes[2] = {0, upAxis == 2 ? 1 : 2};
  float cover[4] = {0, 0, 0, 0};
  for (const DrawMeshCPU& mesh : draw.meshes) {
    if (mesh.purpose == "guide" || !ValidAabb(mesh.aabbMin, mesh.aabbMax)) continue;
    for (int ai = 0; ai < 2; ++ai) {
      const int axis = axes[ai];
      const int other = axes[1 - ai];
      const float extent = mx[axis] - mn[axis];
      const float cross0 = mx[other] - mn[other];
      const float cross1 = mx[upAxis] - mn[upAxis];
      if (!(extent > 1e-5f && cross0 > 1e-5f && cross1 > 1e-5f)) continue;
      const float thickness = mesh.aabbMax[axis] - mesh.aabbMin[axis];
      const float area = (mesh.aabbMax[other] - mesh.aabbMin[other]) *
                         (mesh.aabbMax[upAxis] - mesh.aabbMin[upAxis]);
      if (thickness > extent * 0.025f || area < cross0 * cross1 * 0.01f) continue;
      const float center = 0.5f * (mesh.aabbMin[axis] + mesh.aabbMax[axis]);
      if (center <= mn[axis] + extent * 0.08f) cover[ai * 2] += area;
      if (center >= mx[axis] - extent * 0.08f) cover[ai * 2 + 1] += area;
    }
  }
  int best = 0;
  int worst = 0;
  for (int i = 1; i < 4; ++i) {
    if (cover[i] < cover[best]) best = i;
    if (cover[i] > cover[worst]) worst = i;
  }
  if (!(cover[worst] > 0.0f) || cover[best] > cover[worst] * 0.5f) return false;
  constexpr float kOpenSidePi = 3.14159265358979323846f;
  const float candidateYaw[4] = {-0.5f * kOpenSidePi, 0.5f * kOpenSidePi,
                                 kOpenSidePi, 0.0f};
  *yaw = candidateYaw[best];
  return true;
}

bool AutoSubdivisionFitBounds(const DrawScene& draw, float fitMin[3],
                              float fitMax[3]) {
  float ngMin[3] = {1e30f, 1e30f, 1e30f};
  float ngMax[3] = {-1e30f, -1e30f, -1e30f};
  bool ngValid = false;
  for (const DrawMeshCPU& m : draw.meshes) {
    if (m.purpose == "guide") continue;
    if (!ValidAabb(m.aabbMin, m.aabbMax)) continue;
    for (int a = 0; a < 3; ++a) {
      ngMin[a] = std::min(ngMin[a], m.aabbMin[a]);
      ngMax[a] = std::max(ngMax[a], m.aabbMax[a]);
    }
    ngValid = true;
  }
  if (ngValid) {
    for (int a = 0; a < 3; ++a) {
      fitMin[a] = ngMin[a];
      fitMax[a] = ngMax[a];
    }
    return true;
  }
  if (draw.hasBounds && ValidAabb(draw.aabbMin, draw.aabbMax)) {
    for (int a = 0; a < 3; ++a) {
      fitMin[a] = draw.aabbMin[a];
      fitMax[a] = draw.aabbMax[a];
    }
    return true;
  }
  return false;
}

int AutoSubdivisionLevel(float projectedRadiusPx, int maxLevel) {
  if (!(projectedRadiusPx > 64.0f)) return 0;
  int level = 1;
  if (projectedRadiusPx > 160.0f) level = 2;
  if (projectedRadiusPx > 360.0f) level = 3;
  if (projectedRadiusPx > 800.0f) level = 4;
  if (projectedRadiusPx > 1600.0f) level = 5;
  if (projectedRadiusPx > 3200.0f) level = 6;
  return std::min(level, std::max(0, maxLevel));
}

bool EstimateAutoSubdivisionLevels(
    const DrawScene& draw, const AutoSubdivisionView& view, int sceneLevel,
    int maxLevel, const std::map<std::string, int>& explicitPrimLevels,
    std::map<std::string, int>* autoPrimLevels) {
  if (!autoPrimLevels) return false;
  autoPrimLevels->clear();
  maxLevel = std::max(0, std::min(maxLevel, 10));
  if (maxLevel <= 0 || draw.meshes.empty()) return false;

  float fitMin[3], fitMax[3];
  if (!AutoSubdivisionFitBounds(draw, fitMin, fitMax)) return false;

  const float cx = 0.5f * (fitMin[0] + fitMax[0]);
  const float cy = 0.5f * (fitMin[1] + fitMax[1]);
  const float cz = 0.5f * (fitMin[2] + fitMax[2]);
  const float dx = fitMax[0] - fitMin[0];
  const float dy = fitMax[1] - fitMin[1];
  const float dz = fitMax[2] - fitMin[2];
  float sceneRadius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(sceneRadius > 1e-4f)) sceneRadius = 1.0f;

  const float fovYDeg = std::max(5.0f, std::min(175.0f, view.fovYDeg));
  const float halfV = 0.5f * fovYDeg * kDeg2Rad;
  const float aspect = std::max(0.05f, view.aspect);
  const float halfH = std::atan(std::tan(halfV) * aspect);
  const float halfMin = std::max(1e-3f, std::min(halfV, halfH));
  float fitDistance = (sceneRadius / std::sin(halfMin)) * 1.1f;
  if (view.camDolly > 0.0f) fitDistance *= view.camDolly;

  const int upAxis = (draw.upAxis == "Z") ? 2 : 1;
  const light3d::Vec3 eye =
      light3d::Vec3{cx, cy, cz} + DirFromAngles(view.yaw, view.pitch, upAxis) *
                                      std::max(fitDistance, 1e-3f);
  const float viewportH =
      static_cast<float>(std::max(1, view.viewportHeight));
  const float pixelScale = viewportH / (2.0f * std::tan(halfV));
  const float quality = std::max(0.25f, view.quality);

  for (const DrawMeshCPU& m : draw.meshes) {
    if (m.absPath.empty() || m.purpose == "guide") continue;
    if (explicitPrimLevels.find(m.absPath) != explicitPrimLevels.end()) continue;
    if (!ValidAabb(m.aabbMin, m.aabbMax)) continue;

    const float mx = 0.5f * (m.aabbMin[0] + m.aabbMax[0]);
    const float my = 0.5f * (m.aabbMin[1] + m.aabbMax[1]);
    const float mz = 0.5f * (m.aabbMin[2] + m.aabbMax[2]);
    const float mdx = m.aabbMax[0] - m.aabbMin[0];
    const float mdy = m.aabbMax[1] - m.aabbMin[1];
    const float mdz = m.aabbMax[2] - m.aabbMin[2];
    const float radius = 0.5f * std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
    if (!(radius > 1e-6f)) continue;
    const light3d::Vec3 center{mx, my, mz};
    const float centerDist = light3d::length(center - eye);
    const float dist = std::max(1e-3f, centerDist - radius);
    const float projectedRadiusPx = (radius * pixelScale / dist) * quality;
    const int level = AutoSubdivisionLevel(projectedRadiusPx, maxLevel);
    if (level > std::max(0, sceneLevel)) {
      (*autoPrimLevels)[m.absPath] = level;
    }
  }
  return !autoPrimLevels->empty();
}

bool LoadUsdMaybeAutoSubdivision(
    const std::string& path, LoadOptions opts, LoadedScene* out, DrawScene* draw,
    bool rtPath, LoadControl* ctrl, const AutoSubdivisionView& view) {
  bool ok = LoadUSD(path, opts, out, draw, rtPath, ctrl);
  if (!ok || !draw || !opts.subdivisionAuto || (ctrl && ctrl->cancel.load())) {
    return ok;
  }

  std::map<std::string, int> autoPrimLevels;
  if (!EstimateAutoSubdivisionLevels(*draw, view, opts.subdivisionLevel,
                                     opts.subdivisionAutoMaxLevel,
                                     opts.subdivisionPrimLevels,
                                     &autoPrimLevels)) {
    return ok;
  }

  LoadOptions refinedOpts = opts;
  refinedOpts.subdivisionAuto = false;
  for (const auto& kv : autoPrimLevels) {
    refinedOpts.subdivisionPrimLevels.emplace(kv.first, kv.second);
  }

  int maxApplied = 0;
  for (const auto& kv : refinedOpts.subdivisionPrimLevels) {
    maxApplied = std::max(maxApplied, kv.second);
  }
  LOGI("auto subdivision: %zu prim override(s), max level %d",
       autoPrimLevels.size(), maxApplied);

  LoadedScene refined;
  DrawScene refinedDraw;
  if (LoadUSD(path, refinedOpts, &refined, &refinedDraw, rtPath, ctrl)) {
    *out = std::move(refined);
    *draw = std::move(refinedDraw);
    return true;
  }

  if (!refined.err.empty()) {
    out->warn += "Auto subdivision retry failed; using base mesh: " +
                 refined.err + "\n";
  }
  return ok;
}

std::string ShellQuote(const std::string& value) {
#if defined(_WIN32)
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"') quoted += '\\';
    quoted += c;
  }
  return quoted + "\"";
#else
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'') quoted += "'\\''";
    else quoted += c;
  }
  return quoted + "'";
#endif
}

int RunCommandWithTimeout(const std::string& command, int timeoutSec,
                          bool* timedOut) {
  if (timedOut) *timedOut = false;
#if defined(_WIN32)
  (void)timeoutSec;
  return std::system(command.c_str());
#else
  const pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    setpgid(0, 0);
    execl("/bin/sh", "sh", "-c", command.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  setpgid(child, child);
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(std::max(1, timeoutSec));
  for (;;) {
    const pid_t done = waitpid(child, &status, WNOHANG);
    if (done == child) break;
    if (done < 0) return -1;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (timedOut) *timedOut = true;
      kill(-child, SIGTERM);
      const auto grace = std::chrono::steady_clock::now() +
                         std::chrono::seconds(1);
      while (std::chrono::steady_clock::now() < grace) {
        if (waitpid(child, &status, WNOHANG) == child) return 124;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      kill(-child, SIGKILL);
      waitpid(child, &status, 0);
      return 124;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

bool CompileLiveVulkanShader(const std::string& source,
                             bool fullMaterialShader,
                             bool useProceduralMaterialShader,
                             bool softwareBvh,
                             bool productionPath,
                             size_t maxSourceBytes,
                             int compileTimeoutSec,
                             std::vector<uint32_t>* spirv,
                             std::string* err) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source, ec) || ec) {
    if (err) *err = "Vulkan shader source does not exist: " + source;
    return false;
  }
  const uintmax_t sourceBytes = std::filesystem::file_size(source, ec);
  if (ec) {
    if (err) *err = "cannot determine Vulkan shader source size: " + source;
    return false;
  }
  if (maxSourceBytes > 0 && sourceBytes > maxSourceBytes) {
    if (err) {
      *err = "MaterialX Vulkan shader source is " +
             std::to_string(sourceBytes) + " bytes; limit is " +
             std::to_string(maxSourceBytes) +
             " bytes (override with --materialx-vk-shader-max-kib)";
    }
    return false;
  }
  const char* compilerEnv = std::getenv("LUSDVIEW_GLSLC");
  const std::string compiler =
      compilerEnv && *compilerEnv ? compilerEnv : "glslc";
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(ec) /
      ("lusdview-live-shader-" + std::to_string(stamp));
  if (ec) {
    if (err) *err = "cannot resolve temporary directory for glslc";
    return false;
  }
  const std::string output = base.string() + ".spv";
  const std::string logPath = base.string() + ".log";
  const std::string include =
      std::filesystem::path(source).parent_path().string();
  std::string variantDefines;
  if (softwareBvh) {
    if (!fullMaterialShader)
      variantDefines += " -DLUSDVIEW_SW_DISABLE_MTLX=1";
    if (productionPath)
      variantDefines += " -DLUSDVIEW_SW_PATH_ONLY=1";
  } else if (!fullMaterialShader) {
    variantDefines = " -DLUSDVIEW_RT_FAST_MATERIAL=1"
                     " -DLUSDVIEW_RT_DISABLE_MTLX=1"
                     " -DLUSDVIEW_RT_DISABLE_DEBUG_RAYS=1";
  } else if (!useProceduralMaterialShader) {
    variantDefines = " -DLUSDVIEW_RT_DISABLE_MTLX_PROCEDURAL=1";
  }
  // Match the interactive startup shader: the production integrator is unused
  // here but can add minutes of driver optimization if left in the module.
  // Keep it for actual path-tracing reloads, including the hardware backend.
  if (!softwareBvh && !productionPath)
    variantDefines += " -DLUSDVIEW_RT_INTERACTIVE_ONLY=1";
  const std::string compilerCommand =
      ShellQuote(compiler) + " --target-env=vulkan1.2 "
      "-fshader-stage=compute" + variantDefines + " -I" +
      ShellQuote(include) + " " +
      ShellQuote(source) + " -o " + ShellQuote(output) + " >" +
      ShellQuote(logPath) + " 2>&1";
  bool timedOut = false;
  const int result = RunCommandWithTimeout(
      compilerCommand, compileTimeoutSec, &timedOut);
  std::string compilerLog;
  {
    std::ifstream log(logPath, std::ios::binary);
    if (log) {
      log.seekg(0, std::ios::end);
      const std::streamoff size = log.tellg();
      if (size > 0 && size < 4 * 1024 * 1024) {
        log.seekg(0, std::ios::beg);
        compilerLog.resize(static_cast<size_t>(size));
        log.read(compilerLog.data(), size);
      }
    }
  }
  std::filesystem::remove(logPath, ec);
  if (result != 0) {
    std::filesystem::remove(output, ec);
    if (err) {
      *err = timedOut
                 ? "MaterialX Vulkan shader compile exceeded " +
                       std::to_string(compileTimeoutSec) + " seconds"
                 : "glslc live compile failed";
      if (!compilerLog.empty()) *err += ":\n" + compilerLog;
    }
    return false;
  }
  std::ifstream binary(output, std::ios::binary);
  if (!binary) {
    if (err) *err = "glslc produced no SPIR-V output";
    return false;
  }
  binary.seekg(0, std::ios::end);
  const std::streamoff bytes = binary.tellg();
  if (bytes < 20 || bytes > 64 * 1024 * 1024 || (bytes & 3) != 0) {
    std::filesystem::remove(output, ec);
    if (err) *err = "glslc produced an invalid SPIR-V size";
    return false;
  }
  binary.seekg(0, std::ios::beg);
  spirv->resize(static_cast<size_t>(bytes) / sizeof(uint32_t));
  const bool read = static_cast<bool>(binary.read(
      reinterpret_cast<char*>(spirv->data()), bytes));
  binary.close();
  std::filesystem::remove(output, ec);
  if (!read || spirv->empty() || (*spirv)[0] != 0x07230203u) {
    spirv->clear();
    if (err) *err = "glslc output is not a SPIR-V module";
    return false;
  }
  return true;
}
}  // anonymous namespace

// In namespace lusdview (declared in app.hh) so the MCP screenshot tool can reuse it.
bool WriteScreenshotImage(const std::string& path,
                          const std::vector<uint8_t>& rgba, int w, int h,
                          std::string* err) {
  if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) *
                                      static_cast<size_t>(h) * 4) {
    if (err) *err = "invalid screenshot buffer";
    return false;
  }

  // .ppm: keep the dependency-free fast path. Everything else goes through the
  // shared encoder, which autodetects png/jpg/jpeg/qoi/bmp/exr from the extension
  // (fpnge/fpng PNG, libjpeg-turbo JPEG, QOI -- whatever the build enabled).
  if (LowerExtension(path) == "ppm") {
    WritePPM(path, rgba, w, h);
    return true;
  }

  lightusd::Image img;
  img.uri = path;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data = rgba;

  lightusd::image::WriteOption opt;  // Autodetect by extension
  auto ret = lightusd::image::WriteImageToFile(path, img, opt);
  if (!ret) {
    if (err) *err = ret.error();
    return false;
  }
  return true;
}

bool WriteLinearExr(const std::string& path, const std::vector<float>& rgba,
                    int w, int h, std::string* err) {
  if (LowerExtension(path) != "exr") {
    if (err) *err = "linear output must use the .exr extension";
    return false;
  }
  const size_t sampleCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
  if (w <= 0 || h <= 0 || rgba.size() < sampleCount) {
    if (err) *err = "invalid linear RGBA buffer";
    return false;
  }
  lightusd::Image img;
  img.uri = path;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.bpp = 32;
  img.format = lightusd::Image::PixelFormat::Float;
  img.data.resize(sampleCount * sizeof(float));
  std::memcpy(img.data.data(), rgba.data(), img.data.size());
  lightusd::image::WriteOption opt;
  opt.format = lightusd::image::WriteImageFormat::EXR;
  opt.half = true;
  auto ret = lightusd::image::WriteImageToFile(path, img, opt);
  if (!ret) {
    if (err) *err = ret.error();
    return false;
  }
  return true;
}

std::vector<float> SuppressPathFireflies(const std::vector<float>& input, int w,
                                         int h, float relativeLimit) {
  if (w <= 0 || h <= 0 || input.size() < static_cast<size_t>(w) * h * 4u)
    return input;
  if (!(relativeLimit > 0.0f) || !std::isfinite(relativeLimit)) return input;
  std::vector<float> output = input;
  const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
  auto luminance = [](const float* p) {
    return p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f;
  };
  auto parallelRows = [&](const auto& fn) {
    unsigned workers = pixelCount >= 128u * 128u
                           ? std::max(1u, std::thread::hardware_concurrency())
                           : 1u;
    workers = std::min(workers, 8u);
    workers = std::min(workers, static_cast<unsigned>(h));
    if (workers <= 1u) {
      fn(0, h);
      return;
    }
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker) {
      const int y0 = h * static_cast<int>(worker) / static_cast<int>(workers);
      const int y1 =
          h * static_cast<int>(worker + 1u) / static_cast<int>(workers);
      threads.emplace_back(fn, y0, y1);
    }
    for (std::thread& thread : threads) thread.join();
  };

  // Clamp only isolated, extreme samples. This is intentionally display-side:
  // the linear EXR remains an unbiased accumulation, while a preview or PNG is
  // not dominated by one unresolved GGX/transmission/subsurface path.
  parallelRows([&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < w; ++x) {
        std::array<float, 9> neighborhood{};
        size_t n = 0;
        for (int ky = -1; ky <= 1; ++ky) {
          const int sy = std::max(0, std::min(h - 1, y + ky));
          for (int kx = -1; kx <= 1; ++kx) {
            const int sx = std::max(0, std::min(w - 1, x + kx));
            const size_t i = (static_cast<size_t>(sy) * w + sx) * 4u;
            neighborhood[n++] = std::max(0.0f, luminance(&input[i]));
          }
        }
        std::sort(neighborhood.begin(), neighborhood.end());
        const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
        const float center = std::max(0.0f, luminance(&input[i]));
        const float median = neighborhood[4];
        std::array<float, 9> deviations{};
        for (size_t sample = 0; sample < neighborhood.size(); ++sample) {
          deviations[sample] = std::fabs(neighborhood[sample] - median);
        }
        std::sort(deviations.begin(), deviations.end());
        // Median absolute deviation distinguishes a real highlight edge (the
        // neighborhood has structure) from an isolated high-energy path (all
        // neighbors agree except the center). A small signal-relative floor
        // avoids overreacting to quantization/numerical noise in flat regions.
        const float robustDeviation =
            deviations[4] + 0.005f + 0.02f * median;
        const float limit = median + relativeLimit * robustDeviation;
        const float scale = center > limit ? limit / center : 1.0f;
        for (int c = 0; c < 3; ++c) {
          const float value = std::isfinite(input[i + static_cast<size_t>(c)])
                                  ? input[i + static_cast<size_t>(c)]
                                  : 0.0f;
          output[i + static_cast<size_t>(c)] =
              std::max(0.0f, value) * scale;
        }
        output[i + 3] = 1.0f;
      }
    }
  });
  return output;
}

std::vector<float> AtrousDenoise(const std::vector<float>& input, int w, int h,
                                 int iterations, float fireflyClamp) {
  if (w <= 0 || h <= 0 || input.size() < static_cast<size_t>(w) * h * 4u)
    return input;
  std::vector<float> src =
      SuppressPathFireflies(input, w, h, fireflyClamp);
  std::vector<float> dst(src.size());
  const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<float> variance(pixelCount, 0.0f);
  const float kernel[5] = {1.0f, 4.0f, 6.0f, 4.0f, 1.0f};
  auto luminance = [](const float* p) {
    return p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f;
  };
  // The denoiser is a batch/export operation and is independent per row. Keep
  // it from becoming the tail of a GPU render at UHD resolutions while
  // avoiding thread startup overhead for thumbnails and unit-test images.
  auto parallelRows = [&](const auto& fn) {
    unsigned workers = pixelCount >= 128u * 128u
                           ? std::max(1u, std::thread::hardware_concurrency())
                           : 1u;
    workers = std::min(workers, 8u);
    workers = std::min(workers, static_cast<unsigned>(h));
    if (workers <= 1u) {
      fn(0, h);
      return;
    }
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker) {
      const int y0 = h * static_cast<int>(worker) / static_cast<int>(workers);
      const int y1 =
          h * static_cast<int>(worker + 1u) / static_cast<int>(workers);
      threads.emplace_back(fn, y0, y1);
    }
    for (std::thread& thread : threads) thread.join();
  };

  // Estimate local signal variance once from the robust input. The variance
  // widens the range kernel in noisy regions but keeps fine chess-piece edges
  // sharp where neighboring samples are already consistent.
  parallelRows([&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < w; ++x) {
        float sum = 0.0f, sum2 = 0.0f;
        for (int ky = -1; ky <= 1; ++ky) {
          const int sy = std::max(0, std::min(h - 1, y + ky));
          for (int kx = -1; kx <= 1; ++kx) {
            const int sx = std::max(0, std::min(w - 1, x + kx));
            const size_t i = (static_cast<size_t>(sy) * w + sx) * 4u;
            const float l = luminance(&src[i]);
            sum += l;
            sum2 += l * l;
          }
        }
        const float mean = sum * (1.0f / 9.0f);
        variance[static_cast<size_t>(y) * w + x] =
            std::max(0.0f, sum2 * (1.0f / 9.0f) - mean * mean);
      }
    }
  });

  for (int pass = 0; pass < iterations; ++pass) {
    const int step = 1 << pass;
    parallelRows([&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t centerIndex = (static_cast<size_t>(y) * w + x) * 4u;
        const float centerLum = luminance(&src[centerIndex]);
        const float sigma = std::sqrt(
            variance[static_cast<size_t>(y) * w + x]);
        const float phiLum = 0.01f + 2.0f * sigma + 0.08f * centerLum;
        float sum[3] = {0.0f, 0.0f, 0.0f};
        float weightSum = 0.0f;
        for (int ky = -2; ky <= 2; ++ky) {
          const int sy = std::max(0, std::min(h - 1, y + ky * step));
          for (int kx = -2; kx <= 2; ++kx) {
            const int sx = std::max(0, std::min(w - 1, x + kx * step));
            const size_t sampleIndex =
                (static_cast<size_t>(sy) * w + sx) * 4u;
            const float dl =
                std::fabs(luminance(&src[sampleIndex]) - centerLum);
            const float dr = src[sampleIndex] - src[centerIndex];
            const float dg = src[sampleIndex + 1] - src[centerIndex + 1];
            const float db = src[sampleIndex + 2] - src[centerIndex + 2];
            const float colorDistance =
                (dr * dr + dg * dg + db * db) /
                (0.01f + centerLum * centerLum);
            const float range =
                std::exp(-dl / phiLum - 0.08f * colorDistance);
            const float weight = kernel[kx + 2] * kernel[ky + 2] * range;
            for (int c = 0; c < 3; ++c)
              sum[c] += src[sampleIndex + static_cast<size_t>(c)] * weight;
            weightSum += weight;
          }
        }
        for (int c = 0; c < 3; ++c)
          dst[centerIndex + static_cast<size_t>(c)] = sum[c] / weightSum;
        dst[centerIndex + 3] = 1.0f;
      }
    }
    });
    src.swap(dst);
  }
  return src;
}

std::vector<uint8_t> ToneMapPathDisplay(const std::vector<float>& linear,
                                        int w, int h, float exposure) {
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u, 255u);
  const float gain = std::exp2(exposure);
  auto aces = [](float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return std::max(0.0f, std::min(1.0f, (x * (a * x + b)) /
                                           (x * (c * x + d) + e)));
  };
  auto srgb = [](float x) {
    return x <= 0.0031308f ? 12.92f * x
                           : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
  };
  for (size_t i = 0; i < rgba.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const float mapped = srgb(aces(std::max(0.0f, linear[i + c]) * gain));
      rgba[i + static_cast<size_t>(c)] = static_cast<uint8_t>(
          std::lround(std::max(0.0f, std::min(1.0f, mapped)) * 255.0f));
    }
  }
  return rgba;
}

bool ShouldDenoise(const PathTraceSettings& settings, uint32_t samples) {
  return settings.denoise == PathTraceDenoise::On ||
         (settings.denoise == PathTraceDenoise::Auto && samples >= 8u);
}

std::vector<float> PreparePathDisplay(const std::vector<float>& linear, int w,
                                      int h,
                                      const PathTraceSettings& settings,
                                      uint32_t samples) {
  if (ShouldDenoise(settings, samples)) {
    return AtrousDenoise(linear, w, h, 5, settings.fireflyClamp);
  }
  // CUDA/HIP interactive previews trace one fresh sample per frame. Avoid an
  // 18-element robust sort per pixel (and worker startup) when there is no
  // accumulated image to clean yet; batch captures and converged displays take
  // the suppression path below.
  if (samples <= 1u) return linear;
  return SuppressPathFireflies(linear, w, h, settings.fireflyClamp);
}

void App::getRequestedWindowSize(int* width, int* height) const {
  if (hasWindowSizeOverride_) {
    *width = windowWidth_;
    *height = windowHeight_;
    return;
  }

  *width = static_cast<int>(static_cast<float>(kBaseWindowWidth) * windowScale_);
  *height = static_cast<int>(static_cast<float>(kBaseWindowHeight) * windowScale_);
}

App::~App() {
#if defined(LUSDVIEW_HAVE_MCP)
  // Stop the MCP transports first so no worker thread calls back into App while
  // its members are being destroyed.
  if (mcp_) mcp_->stop();
#endif
  if (physicsWorldReady_) lightusd::tydra::FreePhysWorld(&physicsWorld_);
  cancelAndJoinLoad();  // must run before members the worker writes into are destroyed
  // Finish any in-flight RT build: std::thread's destructor calls
  // std::terminate() if it is destroyed still joinable (e.g. a headless
  // --frames run that exits before a background HIP/CUDA/CPU RT build
  // finishes), so every *BuildThread_ member must be joined here.
  if (hipBuildThread_.joinable()) hipBuildThread_.join();
  if (cudaBuildThread_.joinable()) cudaBuildThread_.join();
  if (cpuBuildThread_.joinable()) cpuBuildThread_.join();
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  if (renderThreadActive_) {
    joinRenderThread();  // the render thread runs renderer_->shutdown() on its context
  } else
#endif
      if (renderer_) {
    renderer_->shutdown();
  }
  ShutdownTextureGpu();
  if (ImGui::GetCurrentContext()) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename && io.IniFilename[0]) {
      ImGui::SaveIniSettingsToDisk(io.IniFilename);
    }
    ImGui::DestroyContext();
  }
  if (window_) glfwDestroyWindow(window_);
  glfwTerminate();
#if defined(LUSDVIEW_HAVE_MCP)
  for (const std::filesystem::path& path : mcpTempFiles_) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
#endif
}

void App::autoDetectUiScale() {
  if (uiScaleExplicit_ || headless_) return;
  GLFWmonitor* mon = glfwGetPrimaryMonitor();
  if (!mon) return;
  // OS-reported HiDPI content scale (e.g. 2.0 at 200% desktop scaling).
  float xs = 1.0f, ys = 1.0f;
  glfwGetMonitorContentScale(mon, &xs, &ys);
  const GLFWvidmode* mode = glfwGetVideoMode(mon);
  const int mw = mode ? mode->width : 0;
  // Use 2x only on genuinely high-density / large panels: a HiDPI content scale,
  // or a native width of at least 2K (2560). Standard 1080p/1920 stays at 1x, so
  // the font/widgets are not oversized on a normal-DPI display.
  const float scale = (xs >= 1.5f || mw >= 2560) ? 2.0f : 1.0f;
  uiScale_ = scale;
  // A first run on a 4K-class display starts at 2560x1600. A normal 2560-wide
  // monitor still gets crisp 2x UI metrics without nearly filling its workarea.
  windowScale_ = mw >= 3840 ? 2.0f : 1.0f;
  fontSizePx_ = 16.0f * scale;
  LOGI("ui scale: %.0fx, window scale: %.0fx (monitor %dpx wide, content scale %.2f)",
       scale, windowScale_, mw, xs);
}

bool App::initWindow(std::string* err) {
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    *err = "glfwInit failed";
    return false;
  }
  autoDetectUiScale();  // before getRequestedWindowSize (windowScale_ feeds it)

  if (backend_ == Backend::GL) {
    // GLFW window hints are STICKY: the Vulkan branch below sets
    // GLFW_CLIENT_API = GLFW_NO_API, and that persists for every later
    // glfwCreateWindow. Harmless while only one window is ever created, but the
    // runtime GL<->Vulkan technique switch recreates it -- without putting the
    // hint back, a "GL" window came up with no GL context at all, glad loaded
    // nothing, and the first GL call in GLRenderer::init() jumped through a
    // null pointer (SIGSEGV at address 0).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    // Request 4.1 core (the highest macOS supports) so the GPU-tessellation
    // displacement path is available; fall back to 3.3 below if creation fails.
    // 4.1 core is a strict superset of 3.3 core, so all existing shaders run as-is.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
  } else {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  }

  // Restore native geometry before creating the GLFW window. Applying it later
  // from ImGui's settings callback works under X11/Xvfb but is not reliably
  // honored by HiDPI compositors/window managers after the surface and swapchain
  // already exist. ImGui still loads the same file later for docking state.
  if (persistImGuiState_ && !hasWindowSizeOverride_) {
    std::optional<std::filesystem::path> iniPath;
    if (!executablePath_.empty()) {
      const std::filesystem::path localIni =
          executablePath_.parent_path() / "imgui.ini";
      std::error_code ec;
      if (std::filesystem::is_regular_file(localIni, ec) && !ec)
        iniPath = localIni;
    }
    if (!iniPath && !configPath_.empty()) {
      iniPath = configPath_.parent_path() / "imgui.ini";
    } else if (!iniPath) {
      iniPath = DefaultImGuiIniPath();
    }
    if (iniPath) {
      std::ifstream input(*iniPath);
      std::string line;
      bool inMainWindow = false;
      while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '[') {
          inMainWindow = line == "[lusdview][MainWindow]";
          continue;
        }
        if (!inMainWindow) continue;
        int a = 0, b = 0;
        if (std::sscanf(line.c_str(), "Pos=%d,%d", &a, &b) == 2) {
          iniWindowX_ = a;
          iniWindowY_ = b;
          iniWindowPositionValid_ = true;
        } else if (std::sscanf(line.c_str(), "Size=%d,%d", &a, &b) == 2 &&
                   a > 0 && b > 0) {
          iniWindowWidth_ = a;
          iniWindowHeight_ = b;
          iniWindowSizeValid_ = true;
        }
      }
    }
  }

  // Scale the first-run default independently from font/widget sizing, then
  // clamp saved/default dimensions to GLFW's monitor work-area coordinate space.
  int winW = 0;
  int winH = 0;
  if (!hasWindowSizeOverride_ && iniWindowSizeValid_) {
    winW = iniWindowWidth_;
    winH = iniWindowHeight_;
  } else {
    getRequestedWindowSize(&winW, &winH);
  }
  int workX = 0, workY = 0, workW = 0, workH = 0;
  if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
    glfwGetMonitorWorkarea(mon, &workX, &workY, &workW, &workH);
    if (workW > 0 && winW > workW) winW = workW;
    if (workH > 0 && winH > workH) winH = workH;
  }
  const char* windowTitle = virtualHumanProfile_ ? "vchar" : "lusdview";
  window_ = glfwCreateWindow(winW, winH, windowTitle, nullptr, nullptr);
  if (!window_ && backend_ == Backend::GL) {
    // 4.1 unavailable (e.g. 3.3-only hardware): retry at 3.3 core. Tessellation
    // displacement is then unavailable; coarse per-vertex displacement still works.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    window_ = glfwCreateWindow(winW, winH, windowTitle, nullptr, nullptr);
  }
  if (!window_) {
    *err = "glfwCreateWindow failed";
    return false;
  }
  glfwSetWindowUserPointer(window_, this);
  glfwSetDropCallback(window_, [](GLFWwindow* window, int count,
                                 const char** paths) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (!app || count <= 0 || !paths || !paths[0]) return;
    const std::filesystem::path path(paths[0]);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" ||
        ext == ".usdz") {
      app->startLoadAsync(path.string());
    } else if (ext == ".hdr" || ext == ".exr" || ext == ".png" ||
               ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
               ext == ".tif" || ext == ".tiff") {
      app->droppedEnvmapPath_ = path.string();
    } else {
      LOGW("dropped file is neither USD nor a supported environment image: %s",
           path.string().c_str());
    }
  });

  if (!hasWindowSizeOverride_ && iniWindowPositionValid_) {
    int x = iniWindowX_;
    int y = iniWindowY_;
    if (workW > 0) x = std::max(workX, std::min(x, workX + workW - winW));
    if (workH > 0) y = std::max(workY, std::min(y, workY + workH - winH));
    glfwSetWindowPos(window_, x, y);
  }
  int actualW = 0, actualH = 0, framebufferW = 0, framebufferH = 0;
  glfwGetWindowSize(window_, &actualW, &actualH);
  glfwGetFramebufferSize(window_, &framebufferW, &framebufferH);
  LOGI("window size: %dx%d logical, %dx%d framebuffer (%s%s)", actualW,
       actualH, framebufferW, framebufferH,
       hasWindowSizeOverride_ ? "explicit size" :
       (iniWindowSizeValid_ ? "restored from imgui.ini" : "display default"),
       (winW != actualW || winH != actualH) ? ", adjusted by window manager" : "");

  if (backend_ == Backend::GL) {
    glfwMakeContextCurrent(window_);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
#endif
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
      *err = "Failed to load OpenGL via glad";
      return false;
    }
    if (!GLAD_GL_VERSION_3_3) {
      *err = "OpenGL 3.3 not available";
      return false;
    }
    glfwSwapInterval(1);
  }
  return true;
}

bool App::initImGui(std::string* err) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGuiSettingsHandler windowSettings;
  windowSettings.TypeName = "lusdview";
  windowSettings.TypeHash = ImHashStr(windowSettings.TypeName);
  windowSettings.UserData = this;
  windowSettings.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* handler,
                                 const char* name) -> void* {
    if (std::strcmp(name, "MainWindow") != 0) return nullptr;
    App* app = static_cast<App*>(handler->UserData);
    app->iniWindowPositionValid_ = false;
    app->iniWindowSizeValid_ = false;
    return app;
  };
  windowSettings.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*,
                                 void* entry, const char* line) {
    App* app = static_cast<App*>(entry);
    int a = 0, b = 0;
    if (std::sscanf(line, "Pos=%d,%d", &a, &b) == 2) {
      app->iniWindowX_ = a;
      app->iniWindowY_ = b;
      app->iniWindowPositionValid_ = true;
    } else if (std::sscanf(line, "Size=%d,%d", &a, &b) == 2 && a > 0 &&
               b > 0) {
      app->iniWindowWidth_ = a;
      app->iniWindowHeight_ = b;
      app->iniWindowSizeValid_ = true;
    }
  };
  windowSettings.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler) {
    App* app = static_cast<App*>(handler->UserData);
    if (!app->window_ || app->hasWindowSizeOverride_) return;
    int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
      glfwGetMonitorWorkarea(monitor, &workX, &workY, &workWidth, &workHeight);
    }
    if (app->iniWindowSizeValid_) {
      int width = app->iniWindowWidth_;
      int height = app->iniWindowHeight_;
      if (workWidth > 0) width = std::min(width, workWidth);
      if (workHeight > 0) height = std::min(height, workHeight);
      glfwSetWindowSize(app->window_, width, height);
    }
    if (app->iniWindowPositionValid_) {
      int x = app->iniWindowX_;
      int y = app->iniWindowY_;
      int width = 0, height = 0;
      glfwGetWindowSize(app->window_, &width, &height);
      if (workWidth > 0) {
        x = std::max(workX, std::min(x, workX + workWidth - width));
      }
      if (workHeight > 0) {
        y = std::max(workY, std::min(y, workY + workHeight - height));
      }
      glfwSetWindowPos(app->window_, x, y);
    }
  };
  windowSettings.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler,
                                  ImGuiTextBuffer* out) {
    App* app = static_cast<App*>(handler->UserData);
    if (!app->window_) return;
    int x = 0, y = 0, width = 0, height = 0;
    glfwGetWindowPos(app->window_, &x, &y);
    glfwGetWindowSize(app->window_, &width, &height);
    if (width <= 0 || height <= 0) return;
    out->appendf("[lusdview][MainWindow]\nPos=%d,%d\nSize=%d,%d\n\n", x,
                 y, width, height);
  };
  ImGui::AddSettingsHandler(&windowSettings);
  imguiIniPath_.clear();
  std::optional<std::filesystem::path> iniPath;
  // A portable layout beside the executable wins when present. This lets a
  // packaged/local lusdview carry its preferred docking arrangement without
  // replacing the user's normal per-platform layout merely by being launched.
  if (!executablePath_.empty()) {
    const std::filesystem::path localIni =
        executablePath_.parent_path() / "imgui.ini";
    std::error_code localEc;
    if (std::filesystem::is_regular_file(localIni, localEc) && !localEc)
      iniPath = localIni;
  }
  if (!iniPath && !configPath_.empty()) {
    iniPath = configPath_.parent_path() / "imgui.ini";
  } else if (!iniPath) {
    iniPath = DefaultImGuiIniPath();
  }
  // Headless/test runs use synthetic viewport sizes and must never load or
  // overwrite the interactive user's native window/docking geometry.
  if (iniPath && persistImGuiState_) {
    std::error_code ec;
    const std::filesystem::path dir = iniPath->parent_path();
    if (!dir.empty()) {
      std::filesystem::create_directories(dir, ec);
    }
    if (ec) {
      LOGW("could not create ImGui state directory %s: %s", dir.string().c_str(),
           ec.message().c_str());
      io.IniFilename = nullptr;
    } else {
      imguiIniPath_ = iniPath->string();
      io.IniFilename = imguiIniPath_.c_str();
      if (std::filesystem::is_regular_file(*iniPath, ec) && !ec) {
        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
        LOGI("loaded ImGui layout %s", io.IniFilename);
      }
    }
  } else {
    io.IniFilename = nullptr;
  }

  // HiDPI: load Cascadia Mono at a scaled pixel size and scale widget metrics
  // so the UI is readable on 4K panels. Baking the scale into the font size
  // keeps the text crisp (vs. io.FontGlobalScale which just magnifies).
  io.Fonts->AddFontFromMemoryCompressedTTF(CascadiaMono_compressed_data,
                                           CascadiaMono_compressed_size, fontSizePx_);

  // Maya-like dark theme, then scale rounding/padding/spacing for HiDPI.
  StyleMaya();
  ImGui::GetStyle().ScaleAllSizes(uiScale_);

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // Threaded: only the platform half (GLFW callbacks/input) on the main thread; the
  // GL backend half runs on the render thread in renderThreadMain().
  if (renderThreadActive_) return renderer_->initImGuiPlatform(window_, err);
#endif
  return renderer_->initImGui(err);
}

// Release a mesh's CPU geometry after it's been uploaded to the GPU. Used only
// for the static --next preview, where the CPU copy is dead weight (no
// animation re-pose or GPU skinning). Halves resident RAM for large scenes.
// Keeps small metadata (name/purpose/world/aabb/submeshes): visibility and the
// low-memory object-picking fallback both use the retained world-space bounds.
// A mesh the RT path has to be able to re-pose: its rest vertices and skin/morph
// attributes are the ONLY inputs to BuildNextRtDeformedVertices, so freeing them
// would leave the ray tracer stuck on whatever pose the BLAS was built with.
// (The raster path can free them freely -- it deforms in the vertex shader from
// the GPU-side copies.)
static bool MeshIsDeformable(const DrawMeshCPU& m) {
  return !m.jointIdx.empty() || !m.morphDeltaHalf.empty();
}

static void FreeMeshSurfaceCPU(DrawMeshCPU& m) {
  // Preserve enough geometry for pixel-accurate picking before dropping the
  // much wider render vertices. Static meshes never change shape, so a UNORM16
  // position copy is both stable and substantially smaller than DrawVertex.
  if (m.pickPositions.empty() && !m.vertices.empty() && m.indices.size() >= 3) {
    float mn[3] = {m.vertices[0].px, m.vertices[0].py, m.vertices[0].pz};
    float mx[3] = {mn[0], mn[1], mn[2]};
    for (const DrawVertex& v : m.vertices) {
      const float p[3] = {v.px, v.py, v.pz};
      for (int c = 0; c < 3; ++c) {
        mn[c] = std::min(mn[c], p[c]);
        mx[c] = std::max(mx[c], p[c]);
      }
    }
    for (int c = 0; c < 3; ++c) {
      m.pickQuantMin[c] = mn[c];
      m.pickQuantExtent[c] = mx[c] - mn[c];
    }
    m.pickPositions.resize(m.vertices.size() * 3);
    for (size_t i = 0; i < m.vertices.size(); ++i) {
      const float p[3] = {m.vertices[i].px, m.vertices[i].py, m.vertices[i].pz};
      for (int c = 0; c < 3; ++c) {
        const float u = m.pickQuantExtent[c] > 0.0f
                            ? (p[c] - mn[c]) / m.pickQuantExtent[c]
                            : 0.0f;
        m.pickPositions[i * 3 + static_cast<size_t>(c)] =
            static_cast<uint16_t>(std::lround(
                std::max(0.0f, std::min(1.0f, u)) * 65535.0f));
      }
    }
    m.pickIndices = m.indices;
  }
  std::vector<DrawVertex>().swap(m.vertices);
  std::vector<float>().swap(m.vertexColors);
  std::vector<float>().swap(m.vertexAlpha);
  std::vector<float>().swap(m.tangents);
  std::vector<float>().swap(m.binormals);
  for (DrawGeomPropCPU& prop : m.geomProps) std::vector<float>().swap(prop.values);
  std::vector<DrawGeomPropCPU>().swap(m.geomProps);
  std::vector<float>().swap(m.uv1);
  std::vector<float>().swap(m.morphInfluence);
  std::vector<DrawVertex>().swap(m.rtDisplacedVertices);
  std::vector<uint32_t>().swap(m.morphOffsetCount);
  std::vector<uint16_t>().swap(m.morphDeltaHalf);
  std::vector<uint16_t>().swap(m.morphChannelId);
  std::vector<uint32_t>().swap(m.jointIdx);
  std::vector<float>().swap(m.jointWt);
  std::vector<uint32_t>().swap(m.influenceOffsetCount);
  std::vector<float>().swap(m.influenceTexels);
  // The shaded EBO is already resident. When authored perimeter edges exist,
  // deferred wire upload no longer needs the triangulated CPU indices either.
  // Keep them only for the fallback that reconstructs edges from triangles.
  if (!m.wireframeIndices.empty()) std::vector<uint32_t>().swap(m.indices);
  // instanceXforms / instanceColors / instanceOpacities are RETAINED:
  // per-instance frustum culling
  // (gui) re-tests each instance's protoAabb against the frustum and re-uploads
  // the visible subset every frame, so it needs the CPU transforms. This is the
  // CPU-culling memory cost (one CPU + one GPU copy); GPU compute culling would
  // drop the CPU copy -- a documented follow-up.
}

static void FreeMeshAuxCPU(DrawMeshCPU& m) {
  std::vector<uint32_t>().swap(m.indices);
  std::vector<uint32_t>().swap(m.wireframeIndices);
  std::vector<uint32_t>().swap(m.sourceFaceId);
}

static void FreeMeshGeometryCPU(DrawMeshCPU& m) {
  FreeMeshSurfaceCPU(m);
  FreeMeshAuxCPU(m);
}

static void FreeMeshInstanceCPU(DrawMeshCPU& m) {
  std::vector<float>().swap(m.instanceXforms);
  std::vector<float>().swap(m.instanceColors);
  std::vector<float>().swap(m.instanceOpacities);
}

// Aggressive free for the HIP/CUDA RT path: after the build everything lives in
// the GPU BVH and trace() never reads draw_ geometry again, so drop ALL per-mesh
// CPU arrays (including instance transforms, which the RT path -- unlike raster
// culling -- does not need). Keeps only metadata (name/purpose/world/aabb).
static void FreeMeshGeometryCPUForRT(DrawMeshCPU& m) {
  FreeMeshGeometryCPU(m);
  std::vector<float>().swap(m.uv1);
  std::vector<float>().swap(m.morphInfluence);
  std::vector<MorphTargetCPU>().swap(m.morphs);
  std::vector<MorphTargetChannelsCPU>().swap(m.morphTargetChannels);
  std::vector<float>().swap(m.skinnedHelperPoints);
  std::vector<float>().swap(m.instanceXforms);
  std::vector<float>().swap(m.instanceColors);
  std::vector<float>().swap(m.instanceOpacities);
}

static void FreePointGeometryCPUForRT(DrawPointsCPU& p) {
  std::vector<float>().swap(p.points);
  std::vector<float>().swap(p.normals);
  std::vector<float>().swap(p.widths);
  std::vector<float>().swap(p.colors);
  std::vector<float>().swap(p.opacities);
  std::vector<float>().swap(p.ellipseRadii);
  std::vector<float>().swap(p.ellipseNormals);
  std::vector<float>().swap(p.ellipseMajorAxes);
}

static void FreeCurveGeometryCPUForRT(DrawCurvesCPU& c) {
  std::vector<uint32_t>().swap(c.vertexCounts);
  std::vector<float>().swap(c.points);
  std::vector<float>().swap(c.widths);
  std::vector<float>().swap(c.colors);
  std::vector<float>().swap(c.opacities);
}

static void FreeTexturePayloadCPUForRT(DrawTextureCPU& t) {
  std::vector<uint8_t>().swap(t.image.data);
  std::vector<uint8_t>().swap(t.ptexSourceData);
  t.ptexSourceDataShared.reset();
  std::vector<uint8_t>().swap(t.compressed.data);
  for (DrawCompressedMipCPU& mip : t.compressed.mips)
    std::vector<uint8_t>().swap(mip.data);
  for (DrawUdimTileCPU& tile : t.udimTiles) {
    std::vector<uint8_t>().swap(tile.image.data);
    std::vector<uint8_t>().swap(tile.compressed.data);
    for (DrawCompressedMipCPU& mip : tile.compressed.mips)
      std::vector<uint8_t>().swap(mip.data);
    for (light3d::Image& mip : tile.mipImages)
      std::vector<uint8_t>().swap(mip.data);
  }
}

// Zig-zagged deltas make sequential face ids and locally coherent topology
// small, while still covering arbitrary uint32 index order. Size first so the
// byte vector has exactly one allocation; progressive loading already has the
// uncompressed array live, so geometric vector growth would raise peak RSS.
static uint64_t ZigZagDelta(uint32_t value, uint32_t previous) {
  const int64_t delta = static_cast<int64_t>(value) -
                        static_cast<int64_t>(previous);
  return (static_cast<uint64_t>(delta) << 1) ^
         static_cast<uint64_t>(delta >> 63);
}

static size_t VarintSize(uint64_t value) {
  size_t n = 1;
  while (value >= 0x80u) {
    value >>= 7;
    ++n;
  }
  return n;
}

static void EncodeDeltaVarints(const std::vector<uint32_t>& values,
                               std::vector<uint8_t>* encoded) {
  if (!encoded) return;
  size_t byteCount = 0;
  uint32_t previous = 0;
  for (uint32_t value : values) {
    byteCount += VarintSize(ZigZagDelta(value, previous));
    previous = value;
  }
  std::vector<uint8_t> bytes(byteCount);
  size_t dst = 0;
  previous = 0;
  for (uint32_t value : values) {
    uint64_t code = ZigZagDelta(value, previous);
    previous = value;
    while (code >= 0x80u) {
      bytes[dst++] = static_cast<uint8_t>((code & 0x7fu) | 0x80u);
      code >>= 7;
    }
    bytes[dst++] = static_cast<uint8_t>(code);
  }
  *encoded = std::move(bytes);
}

static bool DecodeDeltaVarints(const std::vector<uint8_t>& encoded,
                               size_t valueCount,
                               std::vector<uint32_t>* values) {
  if (!values) return false;
  std::vector<uint32_t> decoded(valueCount);
  size_t src = 0;
  uint32_t previous = 0;
  for (size_t i = 0; i < valueCount; ++i) {
    uint64_t code = 0;
    unsigned shift = 0;
    for (;;) {
      if (src >= encoded.size() || shift > 35) return false;
      const uint8_t byte = encoded[src++];
      code |= static_cast<uint64_t>(byte & 0x7fu) << shift;
      if ((byte & 0x80u) == 0) break;
      shift += 7;
    }
    const int64_t delta = static_cast<int64_t>(code >> 1) ^
                          -static_cast<int64_t>(code & 1u);
    const int64_t value = static_cast<int64_t>(previous) + delta;
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) return false;
    previous = static_cast<uint32_t>(value);
    decoded[i] = previous;
  }
  if (src != encoded.size()) return false;
  *values = std::move(decoded);
  return true;
}

void App::compactDeferredMeshAux(size_t meshIndex) {
  if (meshIndex >= draw_.meshes.size() ||
      meshIndex >= deferredMeshAux_.size())
    return;
  DrawMeshCPU& mesh = draw_.meshes[meshIndex];
  DeferredMeshAux& aux = deferredMeshAux_[meshIndex];
  if (MeshIsDeformable(mesh)) return;

  aux.wireCount = mesh.wireframeIndices.size();
  aux.sourceFaceCount = mesh.sourceFaceId.size();
  deferredAuxRawBytes_ +=
      (aux.wireCount + aux.sourceFaceCount) * sizeof(uint32_t);
  if (aux.wireCount > 0) {
    EncodeDeltaVarints(mesh.wireframeIndices, &aux.wire);
    std::vector<uint32_t>().swap(mesh.wireframeIndices);
  }
  if (aux.sourceFaceCount > 0) {
    EncodeDeltaVarints(mesh.sourceFaceId, &aux.sourceFaces);
    std::vector<uint32_t>().swap(mesh.sourceFaceId);
  }
  deferredAuxCompressedBytes_ += aux.wire.size() + aux.sourceFaces.size();
}

bool App::collectPtexRequests(
    const DrawMeshCPU& mesh, const std::vector<uint32_t>* sourceFaces) {
  const std::vector<uint32_t>& faces = sourceFaces ? *sourceFaces
                                                   : mesh.sourceFaceId;
  if (faces.empty() || draw_.materials.empty()) return false;
  bool referencedPtex = false;
  if (ptexRequestedFaces_.size() < draw_.textures.size()) {
    ptexRequestedFaces_.resize(draw_.textures.size());
    ptexRequestedFaceSets_.resize(draw_.textures.size());
    ptexRequestCursors_.resize(draw_.textures.size());
  }
  for (const DrawSubmesh& submesh : mesh.submeshes) {
    int textureIds[26];
    size_t textureCount = 0;
    auto appendMaterial = [&](int materialId) {
      if (materialId < 0 || static_cast<size_t>(materialId) >= draw_.materials.size())
        return;
      const DrawMaterialCPU& m = draw_.materials[static_cast<size_t>(materialId)];
      const std::pair<int, bool> slots[] = {
          {m.baseColorTex, m.baseColorSample.isPtex},
          {m.metallicTex, m.metallicSample.isPtex},
          {m.roughnessTex, m.roughnessSample.isPtex},
          {m.normalTex, m.normalSample.isPtex},
          {m.coatNormalTex, m.coatNormalSample.isPtex},
          {m.emissiveTex, m.emissiveSample.isPtex},
          {m.opacityTex, m.opacitySample.isPtex},
          {m.occlusionTex, m.occlusionSample.isPtex},
          {m.specularColorTex, m.specularColorSample.isPtex},
          {m.coatWeightTex, m.coatWeightSample.isPtex},
          {m.coatColorTex, m.coatColorSample.isPtex},
          {m.coatRoughnessTex, m.coatRoughnessSample.isPtex},
          {m.displacementTex, m.displacementSample.isPtex}};
      for (const auto& slot : slots) {
        if (!slot.second || slot.first < 0 ||
            static_cast<size_t>(slot.first) >= draw_.textures.size())
          continue;
        bool duplicate = false;
        for (size_t i = 0; i < textureCount; ++i)
          duplicate = duplicate || textureIds[i] == slot.first;
        if (!duplicate) textureIds[textureCount++] = slot.first;
      }
    };
    appendMaterial(submesh.materialId);
    if (submesh.backfaceMaterialId != submesh.materialId)
      appendMaterial(submesh.backfaceMaterialId);
    if (textureCount == 0) continue;
    referencedPtex = true;

    const size_t firstTriangle = static_cast<size_t>(submesh.indexOffset) / 3u;
    const size_t endTriangle = std::min(
        faces.size(),
        (static_cast<size_t>(submesh.indexOffset) + submesh.indexCount) / 3u);
    for (size_t triangle = firstTriangle; triangle < endTriangle; ++triangle) {
      const uint32_t face = faces[triangle];
      for (size_t i = 0; i < textureCount; ++i) {
        const size_t textureId = static_cast<size_t>(textureIds[i]);
        if (ptexRequestedFaceSets_[textureId].insert(face).second) {
          ptexRequestedFaces_[textureId].push_back(face);
          // A texture already passed by the residency walker has new work.
          nextPtexTexture_ = std::min(nextPtexTexture_, textureId);
        }
      }
    }
  }
  return referencedPtex;
}

void App::updatePtexResidency() {
  if (!renderer_ || draw_.textures.empty() || rtOwnsScreenshot_) return;
  const std::vector<uint8_t>& visible = gui_.viewVisibility();
  if (visible.empty()) return;
  if (ptexMeshRequested_.size() < draw_.meshes.size())
    ptexMeshRequested_.resize(draw_.meshes.size(), uint8_t{0});
  if (ptexMeshDemanded_.size() < draw_.meshes.size())
    ptexMeshDemanded_.resize(draw_.meshes.size(), uint8_t{0});

  const size_t resident = std::min(
      draw_.meshes.size(),
      static_cast<size_t>(std::max(0, renderer_->meshCount())));
  for (size_t meshIndex = 0; meshIndex < resident; ++meshIndex) {
    if (ptexMeshRequested_[meshIndex] || meshIndex >= visible.size() ||
        visible[meshIndex] == 0)
      continue;
    const DrawMeshCPU& mesh = draw_.meshes[meshIndex];
    bool referencedPtex = false;
    if (!mesh.sourceFaceId.empty()) {
      referencedPtex = collectPtexRequests(mesh);
    } else if (meshIndex < deferredMeshAux_.size() &&
               deferredMeshAux_[meshIndex].sourceFaceCount > 0) {
      std::vector<uint32_t> decoded;
      if (DecodeDeltaVarints(deferredMeshAux_[meshIndex].sourceFaces,
                             deferredMeshAux_[meshIndex].sourceFaceCount,
                             &decoded)) {
        referencedPtex = collectPtexRequests(mesh, &decoded);
      } else {
        LOGW("could not decode deferred Ptex face ids for mesh %zu", meshIndex);
      }
    }
    ptexMeshDemanded_[meshIndex] = referencedPtex ? uint8_t{1} : uint8_t{0};
    ptexMeshRequested_[meshIndex] = 1;
  }

  // Page decode/upload is independent of mesh and ordinary-texture streaming.
  // Bound it to a small slice so a newly visible Ptex-heavy mesh cannot hitch
  // an otherwise interactive frame.
  const auto start = std::chrono::steady_clock::now();
  constexpr double kPtexFrameBudgetMs = 2.0;
  while (!stepPtexResidency(kPtexFrameBudgetMs)) {
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    if (elapsed >= kPtexFrameBudgetMs) break;
  }
}

bool App::finishPtexDecode(bool wait, bool discard) {
  if (!ptexDecodeActive_) return true;
  if (!wait && ptexDecodeFuture_.wait_for(std::chrono::seconds(0)) !=
                   std::future_status::ready)
    return false;
  PtexDecodeResult decoded = ptexDecodeFuture_.get();
  ptexDecodeActive_ = false;
  ++ptexAsyncJobsCompleted_;
  // A decode launched for a previous scene generation must not publish its page
  // into the current scene's texture array (defense-in-depth on top of the
  // clearPtexDecode() wait at every swap site).
  if (discard || !decoded.ok || decoded.sceneGen != ptexSceneGen_) {
    if (!discard && !decoded.ok && !decoded.error.empty())
      LOGW("Ptex face %u residency decode failed: %s", decoded.face,
           decoded.error.c_str());
    return true;
  }
  if (decoded.texture >= draw_.textures.size() ||
      decoded.texture >= ptexPhysicalCaches_.size() ||
      !ptexPhysicalCaches_[decoded.texture])
    return true;

  DrawTextureCPU& texture = draw_.textures[decoded.texture];
  PtexPhysicalPageAssignment assignment;
  PtexPhysicalPageCache& cache = *ptexPhysicalCaches_[decoded.texture];
  if (!cache.Request(decoded.face, &assignment)) return true;
  texture.ptexGpuPageHits = cache.hits();
  texture.ptexGpuPageMisses = cache.misses();
  texture.ptexGpuPageEvictions = cache.evictions();
  if (assignment.hit) return true;

  const uint32_t slotEdge = texture.ptexPhysicalCacheSlotEdge;
  if (slotEdge == 0) return true;
  const uint32_t slotsPerRow =
      static_cast<uint32_t>(texture.image.width) / slotEdge;
  if (slotsPerRow == 0) return true;
  const uint32_t outerX = (assignment.slot % slotsPerRow) * slotEdge;
  const uint32_t outerY = texture.ptexPhysicalCacheOffsetY +
                          (assignment.slot / slotsPerRow) * slotEdge;
  decoded.rect.x += outerX;
  decoded.rect.y += outerY;
  std::vector<Renderer::TextureRegionUpdate> updates;
  const bool compressedPtex =
      texture.compressed.format != DrawCompressedFormat::None &&
      !texture.compressed.data.empty();
  if (assignment.evictedFace != ~uint32_t{0}) {
    if (compressedPtex) {
      renderer_->updatePtexFaceRect(
          static_cast<int>(decoded.texture), assignment.evictedFace,
          texture.ptexFaceRects[assignment.evictedFace]);
    } else {
      AppendPtexFaceTableUpdates(texture, assignment.evictedFace,
                                 texture.ptexFaceRects[assignment.evictedFace],
                                 &updates);
    }
  }
  Renderer::TextureRegionUpdate pageUpdate;
  pageUpdate.x = static_cast<int>(outerX);
  pageUpdate.y = static_cast<int>(outerY);
  pageUpdate.width = decoded.page.width;
  pageUpdate.height = decoded.page.height;
  if (compressedPtex) {
    DrawTextureCPU pageTexture;
    pageTexture.image = decoded.page;
    pageTexture.srgb = texture.srgb;
    pageTexture.isNormalMap = texture.isNormalMap;
    CompressDrawTexture(loadOpts_.textureOptions, &pageTexture);
    if (pageTexture.compressed.format != texture.compressed.format ||
        pageTexture.compressed.data.empty()) {
      LOGW("Ptex face %u GPU compression failed for residency page", decoded.face);
      return true;
    }
    pageUpdate.compressedFormat = pageTexture.compressed.format;
    pageUpdate.compressed = std::move(pageTexture.compressed.data);
  } else {
    pageUpdate.rowBytes = size_t(decoded.page.width) * 4u;
    pageUpdate.rgba = std::move(decoded.page.data);
  }
  updates.push_back(std::move(pageUpdate));
  if (compressedPtex) {
    if (!renderer_->updatePtexFaceRect(static_cast<int>(decoded.texture),
                                       decoded.face, decoded.rect)) {
      LOGW("Ptex face %u metadata residency update failed", decoded.face);
      return true;
    }
  } else {
    AppendPtexFaceTableUpdates(texture, decoded.face, decoded.rect, &updates);
  }
  if (!renderer_->updateTextureRegions(static_cast<int>(decoded.texture),
                                       updates)) {
    LOGW("Ptex face %u residency upload failed (page=%dx%d, slot=%u, "
         "atlas=%dx%d, cacheY=%u, slotEdge=%u)", decoded.face,
         decoded.page.width, decoded.page.height, assignment.slot,
         texture.image.width, texture.image.height,
         texture.ptexPhysicalCacheOffsetY,
         texture.ptexPhysicalCacheSlotEdge);
    return true;
  }
  ++texture.ptexGpuPageUploads;
  return true;
}

void App::clearPtexDecode() {
  finishPtexDecode(/*wait=*/true, /*discard=*/true);
  // Invalidate any decode that was launched before this clear (there can be none
  // in flight now -- the wait above joined it), and stamp the next scene's
  // launches with a fresh generation.
  ++ptexSceneGen_;
  ptexReaders_.clear();
}

bool App::restoreDeferredMeshAux(size_t meshIndex) {
  if (meshIndex >= deferredMeshAux_.size()) return true;
  DeferredMeshAux& aux = deferredMeshAux_[meshIndex];
  DrawMeshCPU& mesh = draw_.meshes[meshIndex];
  if (aux.wireCount > 0 &&
      !DecodeDeltaVarints(aux.wire, aux.wireCount, &mesh.wireframeIndices)) {
    LOGE("could not decode deferred wire topology for mesh %zu", meshIndex);
    return false;
  }
  if (aux.sourceFaceCount > 0 &&
      !DecodeDeltaVarints(aux.sourceFaces, aux.sourceFaceCount,
                          &mesh.sourceFaceId)) {
    LOGE("could not decode deferred source-face ids for mesh %zu", meshIndex);
    return false;
  }
  std::vector<uint8_t>().swap(aux.wire);
  std::vector<uint8_t>().swap(aux.sourceFaces);
  aux.wireCount = 0;
  aux.sourceFaceCount = 0;
  return true;
}

void App::clearDeferredMeshAux() {
  std::vector<DeferredMeshAux>().swap(deferredMeshAux_);
  deferredAuxRawBytes_ = 0;
  deferredAuxCompressedBytes_ = 0;
}

// Frames the camera must hold still before the RT LOD set is re-selected.
static constexpr int kLodSettleFrames = 4;

void App::updateRtLodCamera() {
  if (!renderer_ || !rtLodEnabled_ || !renderer_->rayTracingActive()) return;

  RtLodCamera cam;
  cam.lodEnabled = true;
  cam.proxyEnabled = true;  // distant prototypes render as shared box proxies
  cam.frustumCull = true;
  cam.fullPx = rtLodFullPx_;
  cam.cullPx = rtLodCullPx_;
  cam.bandFrac = rtLodBandFrac_;
  // GL-convention proj*view so light3d::Frustum extracts correct planes (incl. the
  // near plane, which culls behind-camera instances).
  const light3d::Mat4 vp = camera_.proj(/*zeroToOneDepth=*/false) * camera_.view();
  std::memcpy(cam.viewProj.m, vp.m, sizeof(cam.viewProj.m));
  const light3d::Vec3 eye = camera_.eye();
  const light3d::Vec3 fwd = light3d::normalize(camera_.target() - eye);
  cam.eye = eye;
  cam.forward = fwd;
  cam.nearPlane = camera_.nearPlane();

  // Hysteresis + debounce: track the camera each frame; reselect only once it has
  // held still for a few frames (so micro-jitter never rebuilds the TLAS). The
  // stale LOD set keeps rendering while moving (1 spp motion hides the lag).
  const float dist = std::max(1e-3f, camera_.distance());
  const float eyeMove = light3d::length(eye - lastLodEye_);
  const float align = light3d::dot(fwd, lastLodFwd_);
  const bool moved = !lodHaveLast_ || eyeMove > 0.02f * dist || align < 0.9997f;
  if (moved) {
    lodStillFrames_ = 0;
    lastLodEye_ = eye;
    lastLodFwd_ = fwd;
    lodHaveLast_ = true;
    lodPendingReselect_ = true;
  } else if (lodStillFrames_ < kLodSettleFrames) {
    ++lodStillFrames_;
  }
  // The pixel-size threshold needs a laid-out viewport: on the first windowed
  // frame the dock split is not computed yet and resizeViewport() reports a
  // transient tiny height (e.g. 20px), which would make focalPx so small that
  // every instance reads as sub-pixel and gets culled (a one-frame all-Proxy/Cull
  // blip). Require a sane viewport height before the first arm; the headless path
  // reports its full render height from frame 0, so it still arms immediately.
  int vpw = 0, vph = 0;
  renderer_->viewportSize(&vpw, &vph);
  const bool vpReady = vph >= 64;

  bool reselect = false;
  if (!lodArmedOnce_ && vpReady) {  // first laid-out frame: build the LOD TLAS now
    reselect = true;
    lodArmedOnce_ = true;
  } else if (lodPendingReselect_ && lodStillFrames_ >= kLodSettleFrames) {
    reselect = true;
    lodPendingReselect_ = false;
  }
  if (!reselect) return;  // renderer keeps the last snapshot until the next settle
  postGpu([this, cam]() { renderer_->setLodCamera(cam, /*reselect=*/true); });
}

void App::applyLoaded(bool ok, bool progressive, bool alreadyUploaded) {
  // Threaded GL: the per-mesh progressive upload would free CPU geometry on the
  // main thread before the render thread drains the queued appendMesh ops (a
  // use-after-free). Use the one-shot uploadScene path instead (load stays async);
  // non-threaded GL/Vulkan raster paths use the bounded event stream.
  if (renderThreadActive_) progressive = false;
  if (!alreadyUploaded) {
    clearDeferredMeshAux();
    progressiveActive_ = false;
    nextMesh_ = 0;
    nextAux_ = 0;
    nextTex_ = 0;
    nextVolume_ = 0;
    nextPtexTexture_ = 0;
    ptexPhysicalCaches_.clear();
    ptexReaders_.clear();
    ptexRequestedFaces_.clear();
    ptexRequestedFaceSets_.clear();
    ptexRequestCursors_.clear();
    ptexMeshRequested_.clear();
    ptexMeshDemanded_.clear();
    ptexAsyncJobsLaunched_ = 0;
    ptexAsyncJobsCompleted_ = 0;
  }

  if (ok) {
    // Capture the vertex total now, before the --next path frees per-mesh CPU
    // geometry on upload (otherwise the Stats panel would show 0 vertices).
    if (!alreadyUploaded) {
      ++prototypeLodGeneration_;
      queuedPrototypeLods_.clear();
      size_t vtot = 0;
      for (const DrawMeshCPU& m : draw_.meshes) vtot += m.vertices.size();
      draw_.vertexCount = vtot;
    }
    // Record in the recent-scenes list (interactive only -- headless screenshot
    // runs must not mutate the user's config).
    if (!headless_ && !loaded_.filepath.empty()) addRecentScene(loaded_.filepath);
    ++sceneGen_;  // invalidate the MCP library-tool Stage snapshot
    readAnimationRange();  // start/end/fps; resets playback to paused at start
    if (std::isfinite(loadOpts_.timecode)) {
      animTime_ = loadOpts_.timecode;
      reconvApplied_ = animTime_;
    }
    updateSkinningEffective();
    // The tracer re-pose cache indexes draw_.meshes; this is a different scene.
    nextRestVerts_.clear();
    nextTracerPosedTime_ = std::numeric_limits<double>::quiet_NaN();
    // Robust auto-frame bounds: compute from the full per-mesh set NOW, before
    // the LOD merge collapses 80k meshes into one 42M-instance proxy (whose mass
    // would otherwise swamp the weighting). Cached for the framing step below.
    robustBoundsValid_ = false;
    if (robustFrame_ && useNextLoader_) {
      std::string rrep;
      if (ComputeRobustSceneBounds(&draw_, 0.01f, robustBoundsMin_,
                                   robustBoundsMax_, &rrep)) {
        robustBoundsValid_ = true;
        if (!rrep.empty()) LOGI("%s", rrep.c_str());
      }
    }
    // GPU-budget LOD: bound the full-mesh draw count / VRAM for huge assembled
    // scenes (e.g. Moana island ~84k meshes) by merging the long tail of small
    // meshes into one instanced bbox-proxy soup, so the per-mesh-buffer raster
    // upload doesn't create tens of thousands of buffers and stall for minutes.
    if (!alreadyUploaded && useNextLoader_ &&
        (maxFullMeshes_ > 0 || gpuMemBudgetBytes_ > 0)) {
      std::string rep;
      ApplyGpuBudgetLOD(&draw_, gpuMemBudgetBytes_, maxFullMeshes_, &rep);
      if (!rep.empty()) LOGI("%s", rep.c_str());
    }
    // --next GPU morph: detect instanced prototypes carrying morph channels so
    // the per-frame coefficient upload runs (independent of Tydra GPU skinning).
    hasNextMorph_ = false;
    if (useNextLoader_) {
      for (const DrawMeshCPU& m : draw_.meshes)
        if (m.morphChannelCount > 0) { hasNextMorph_ = true; break; }
    }
    if (skinningEffective_ == SkinningMode::GPU) {
      LOGI("skinning: GPU (%s)", skinningReason_.c_str());
    } else if (skinningRequested_ == SkinningMode::GPU) {
      LOGW("skinning: requested GPU, using CPU (%s)", skinningReason_.c_str());
    } else {
      LOGI("skinning: CPU (%s)", skinningReason_.c_str());
    }
    const std::string& up = loaded_.render.meta.upAxis;
    camera_.setUpAxis((up == "Z" || up == "z") ? 2 : 1);
  }

  if (ok && hipInteractive_) {
    // Windowed --hip: no raster upload (the HIP tracer renders the viewport from
    // draw_ each frame); keep CPU geometry for the tracer build.
    LOGI("loaded %s: %zu mesh(es), %zu tri(s)%s; HIP interactive (no raster upload)",
         loaded_.filepath.c_str(), draw_.meshes.size(), draw_.triangleCount,
         draw_.truncated ? " [truncated]" : "");
  } else if (ok && alreadyUploaded) {
    renderer_->setLights(draw_.lights, draw_.meshes.size());
    LOGI("loaded %s: %zu mesh(es), %zu tri(s)%s; progressive display complete",
         loaded_.filepath.c_str(), draw_.meshes.size(), draw_.triangleCount,
         draw_.truncated ? " [truncated]" : "");
  } else if (ok && progressive) {
    // Reserve materials + texture slots now; stream meshes then textures over
    // the next frames (stepProgressiveUpload) so geometry pops in and the UI
    // stays at frame rate instead of stalling on one big upload.
    renderer_->beginScene(draw_.materials, static_cast<int>(draw_.textures.size()));
    renderer_->setLights(draw_.lights, draw_.meshes.size());
    progressiveActive_ = true;
    LOGI("loaded %s: %zu mesh(es), %zu tri(s)%s; streaming to GPU...",
         loaded_.filepath.c_str(), draw_.meshes.size(), draw_.triangleCount,
         draw_.truncated ? " [truncated]" : "");
  } else {
    // One-shot upload (headless / threaded / failure). draw_ is empty when !ok.
    // Threaded: post upload + the CPU-geometry free together so they run, in order,
    // on the render thread (the free must not precede the upload it feeds).
    const bool freeCpu = ok && useNextLoader_ && !needsMeshCpuGeometryForRT();
    if (ok && rtOwnsScreenshot_) {
      for (const DrawMeshCPU& mesh : draw_.meshes) collectPtexRequests(mesh);
    }
    // When the RT path owns the screenshot the rasterized scene is never drawn,
    // so skip the (potentially huge) raster upload entirely.
    if (!rtOwnsScreenshot_) {
      const bool preserveDeferredAux = freeCpu && !renderThreadActive_;
      if (preserveDeferredAux) deferredMeshAux_.resize(draw_.meshes.size());
      for (size_t i = 0; i < draw_.meshes.size(); ++i)
        startPrototypeLodBuild(i, draw_.meshes[i]);
      postGpu([this, freeCpu, preserveDeferredAux] {
        std::string uerr;
        const bool uploaded = renderer_->uploadScene(draw_, &uerr);
        if (!uploaded) {
          LOGE("scene GPU upload failed for '%s': %s", loaded_.filepath.c_str(),
               uerr.empty() ? "unknown error" : uerr.c_str());
          return;
        }
        // Deformable meshes keep their CPU geometry: RT re-poses from it every
        // frame (and RT can be toggled on at any time).
        if (freeCpu) {
          for (size_t i = 0; i < draw_.meshes.size(); ++i) {
            DrawMeshCPU& mesh = draw_.meshes[i];
            if (MeshIsDeformable(mesh)) continue;
            if (preserveDeferredAux) {
              FreeMeshSurfaceCPU(mesh);
              compactDeferredMeshAux(i);
            } else {
              FreeMeshGeometryCPU(mesh);
            }
          }
        }
      });
      if (std::any_of(draw_.textures.begin(), draw_.textures.end(),
                      [](const DrawTextureCPU& texture) {
                        return texture.deferredDecode;
                      })) {
        resetTextureResidency();
      }
      // Headless/non-threaded one-shot uploads can still refine Ptex pages over
      // subsequent frames. Mark the ordinary scene stages complete and retain
      // only compact native sources after the fallback atlas reaches the GPU.
      if (ok && !renderThreadActive_) {
        bool havePtexPages = false;
        for (DrawTextureCPU& texture : draw_.textures) {
          if (texture.HasPtexSourceData() &&
              texture.ptexPhysicalCacheSlots > 0) {
            havePtexPages = true;
            if (!cudaRt_ && !hipRt_) {
              texture.image.data.clear();
              texture.image.data.shrink_to_fit();
            }
          }
        }
        nextMesh_ = draw_.meshes.size();
        nextAux_ = draw_.meshes.size();
        nextTex_ = draw_.textures.size();
        nextVolume_ = draw_.volumes.size();
        progressiveActive_ = havePtexPages;
      }
    }
    if (ok) {
      LOGI("loaded %s: %zu mesh(es), %zu tri(s), %zu materials, %zu textures%s",
           loaded_.filepath.c_str(),
           draw_.meshes.size(), draw_.triangleCount,
           draw_.materials.size(), draw_.textures.size(),
           draw_.truncated ? " [truncated: render budget]" : "");
    } else {
      LOGE("load failed: %s", loaded_.err.c_str());
    }
  }
  // Structured end-of-load diagnostic summary (renderer-parity work). The
  // converter's warnings and the draw-side skipped list are otherwise only
  // visible in the ImGui panel; surface a greppable, machine-parseable line so
  // headless runs and the usd-assets smoke harness can distinguish a full
  // material fallback (degraded) from a benign missing normal-map texture.
  if (ok) {
    if (std::getenv("LUSDVIEW_DUMP_LIGHT_RECORDS")) {
      auto indexHash = [](const std::vector<int>& values) {
        uint64_t h = 1469598103934665603ull;
        for (int v : values) {
          h ^= static_cast<uint64_t>(static_cast<uint32_t>(v));
          h *= 1099511628211ull;
        }
        return h;
      };
      for (size_t i = 0; i < draw_.lights.size(); ++i) {
        const DrawLightCPU& l = draw_.lights[i];
        std::fprintf(stderr,
          "[lusdview-light] %zu %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g "
          "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g "
          "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g "
          "%d %d %d %d %d %d %d %d %d %d %d %d %llu %llu\n",
          i, static_cast<int>(l.type), l.color[0], l.color[1], l.color[2],
          l.intensity, l.exposure, l.diffuse, l.specular, l.radius, l.width,
          l.height, l.length, l.angle, l.shapingConeAngle,
          l.shapingConeSoftness, l.shapingFocus, l.shapingIesAngleScale,
          l.shadowColor[0], l.shadowColor[1], l.shadowColor[2], l.shadowDistance,
          l.shadowFalloff, l.shadowFalloffGamma, l.position[0], l.position[1],
          l.position[2], l.direction[0], l.direction[1], l.direction[2],
          l.effectiveIntensity, l.normalize ? 1 : 0, l.shadowEnable ? 1 : 0,
          l.hasShaping ? 1 : 0, l.lightLinksAll ? 1 : 0,
          l.shadowLinksAll ? 1 : 0,
          static_cast<int>(l.domeTextureFormat),
          l.textureFile.empty() ? 0 : 1, l.ibl.valid ? 1 : 0,
          l.ibl.specFaceSize, l.ibl.irrFaceSize,
          l.ibl.lutSize, l.ibl.envCubeSize,
          static_cast<unsigned long long>(indexHash(l.lightLinkMeshIndices)),
          static_cast<unsigned long long>(indexHash(l.shadowLinkMeshIndices)));
      }
    }
    if (std::getenv("LUSDVIEW_DUMP_CAMERA_RECORDS")) {
      for (size_t i = 0; i < draw_.cameras.size(); ++i) {
        const DrawCameraCPU& c = draw_.cameras[i];
        double clippingChecksum = 0.0;
        for (size_t p = 0; p < c.clippingPlanes.size(); ++p) {
          clippingChecksum += double(p + 1) * c.clippingPlanes[p];
        }
        std::fprintf(stderr,
          "[lusdview-camera] %zu %s %s %d "
          "%.9g %.9g %.9g %.9g %.9g %.9g "
          "%.9g %.9g %.9g %.9g %.9g %.9g "
          "%.9g %.9g %.9g %.9g %.9g %.9g "
          "%.9g %.9g %.17g %.17g %d %zu %.17g\n",
          i,
          c.absPath.c_str(),
          c.projection == DrawCameraCPU::Projection::Orthographic
              ? "orthographic" : "perspective",
          static_cast<int>(c.projection),
          double(c.focalLength), double(c.horizontalAperture),
          double(c.verticalAperture), double(c.horizontalApertureOffset),
          double(c.verticalApertureOffset), double(c.exposure),
          double(c.zNear), double(c.zFar), double(c.fovYDeg),
          double(c.eye[0]), double(c.eye[1]), double(c.eye[2]),
          double(c.up[0]), double(c.up[1]), double(c.up[2]),
          double(c.forward[0]), double(c.forward[1]), double(c.forward[2]),
          double(c.focusDistance), double(c.fStop), c.shutterOpen,
          c.shutterClose, static_cast<int>(c.stereoRole),
          c.clippingPlanes.size() / 4, clippingChecksum);
      }
    }
    const LoadDiagnostics diag =
        CategorizeLoadWarnings(loaded_.warn, draw_.skipped);
    if (diag.actionable() > 0) {
      LOGW(
          "load summary: degraded_materials=%d missing_textures=%d "
          "unsupported_mtlx=%d unsupported_lobes=%d skipped=%d other=%d",
          diag.degraded_material, diag.missing_texture, diag.unsupported_mtlx,
          diag.unsupported_lobes, diag.skipped, diag.other);
      for (const std::string& ex : diag.examples) {
        LOGW("  - %s", ex.c_str());
      }
    }
  }
  // Pose the GPU frame now that the renderer holds the meshes (the per-mesh
  // morph vertex upload needs them present; the bone texture is global). For the
  // progressive path meshes stream in over later frames — the main loop re-poses
  // once streaming completes (skinFrameTime_ stays NaN until then).
  if (ok && skinningEffective_ == SkinningMode::GPU && !progressiveActive_) {
    updateGpuSkinningFrameIfNeeded();
  }
  // --next deformation: upload the initial bone matrices / blendshape
  // coefficients (the render loop re-poses each frame as animTime_ advances).
  // Skipped while streaming; the loop catches up once meshes land.
  if (ok && useNextLoader_ && (hasNextMorph_ || draw_.boneMatrixCount > 0) &&
      !progressiveActive_) {
    updateNextDeformFrameIfNeeded();
  }
  // Frame the camera AFTER the GPU pose updates draw_ bounds (so an animated
  // load, e.g. --time, frames the posed geometry, matching the CPU bake path).
  bool framedAuthoredCamera = false;
  float framedFocalLength = 50.0f;
  if (ok && draw_.hasBounds) {
    camera_.setSceneBounds(draw_.aabbMin, draw_.aabbMax);
    const int upAxis = (draw_.upAxis == "Z") ? 2 : 1;
    camera_.setUpAxis(upAxis);
    NextCameraPose campose;
    cameraLens_ = RtCameraLens{};
    // Either loader can be framed on a named USD camera. The legacy path reads the
    // camera out of the converted RenderScene (FindLegacyCamera); it used to just
    // warn "need --next" and auto-fit instead, which meant the two loaders could
    // never be pointed at the same camera -- and so could not be compared.
    const bool haveCamera =
        !cameraName_.empty() &&
        (useNextLoader_ && nextStageSnapshot_
             ? FindNextCamera(*nextStageSnapshot_, cameraName_, animTime_,
                              &campose)
             : (loaded_.ok &&
                FindLegacyCamera(loaded_.render, cameraName_, &campose)));
    framedAuthoredCamera = haveCamera;
    if (haveCamera) framedFocalLength = campose.focalLength;
    if (framedAuthoredCamera) {
      // Drive the orbit rig from a scene camera. The auto-fit framing is useless
      // on vast scenes (Caldera's 8 km map frames to a sub-pixel speck); a named
      // USD camera gives a meaningful district view across raster / --rt / --cuda.
      const float* E = campose.eye;
      const float* F = campose.forward;
      const float cx = 0.5f * (draw_.aabbMin[0] + draw_.aabbMax[0]);
      const float cy = 0.5f * (draw_.aabbMin[1] + draw_.aabbMax[1]);
      const float cz = 0.5f * (draw_.aabbMin[2] + draw_.aabbMax[2]);
      float d = std::sqrt((cx - E[0]) * (cx - E[0]) + (cy - E[1]) * (cy - E[1]) +
                          (cz - E[2]) * (cz - E[2]));
      if (!(d > 1e-3f)) d = std::max(1.0f, camera_.distance());
      // eye = target + dirToEye*distance with dirToEye = -forward; invert the
      // OrbitCamera DirFromAngles convention (camera_nav.cc) to recover yaw/pitch.
      const float dir[3] = {-F[0], -F[1], -F[2]};
      float yaw, pitch;
      if (upAxis == 2) {  // +Z up: dirToEye = (sy*cp, cy*cp, sp)
        pitch = std::asin(std::max(-1.0f, std::min(1.0f, dir[2])));
        yaw = std::atan2(dir[0], dir[1]);
      } else {  // +Y up: dirToEye = (sy*cp, sp, cy*cp)
        pitch = std::asin(std::max(-1.0f, std::min(1.0f, dir[1])));
        yaw = std::atan2(dir[0], dir[2]);
      }
      const light3d::Vec3 target{E[0] + F[0] * d, E[1] + F[1] * d,
                                 E[2] + F[2] * d};
      camera_.setFovYDeg(campose.fovYDeg);
      camera_.setProjection(campose.projection);
      camera_.setOrthographicHeight(
          std::max(campose.verticalAperture * 0.1f, 1e-5f));
      const float shiftX = 2.0f * campose.horizontalApertureOffset /
                           std::max(campose.horizontalAperture, 1e-5f);
      const float shiftY = 2.0f * campose.verticalApertureOffset /
                           std::max(campose.verticalAperture, 1e-5f);
      camera_.setLensShift(shiftX, shiftY);
      camera_.setExposure(campose.exposure);
      camera_.setAspectOverride(
          campose.horizontalAperture /
          std::max(campose.verticalAperture, 1e-5f));
      camera_.setAspectOverrideEnabled(true);
      // Use the camera's authored clip range, not the auto-clip derived from the
      // (huge) whole-scene radius -- on Caldera the far-flung guide bounds push
      // the auto near plane out past the nearby district, clipping it away.
      camera_.setAutoClip(false);
      camera_.setClipPlanes(campose.zNear, campose.zFar);
      camera_.setOrbit(target, yaw, pitch, d);
      // USD camera optics are authored in tenths of a scene unit. The physical
      // aperture radius is focalLength / (2 * fStop).
      cameraLens_ = MakeRtCameraLens(
          campose.focalLength, campose.focusDistance, campose.fStop,
          campose.projection == CameraProjection::Perspective);
      LOGI("camera: framing USD camera '%s' (%s, fovY %.1f deg, clip %.2f..%.0f)",
           cameraName_.c_str(),
           campose.projection == CameraProjection::Orthographic ? "orthographic"
                                                                 : "perspective",
           campose.fovYDeg, campose.zNear, campose.zFar);
    } else {
      camera_.setProjection(CameraProjection::Perspective);
      camera_.setLensShift(0.0f, 0.0f);
      camera_.setExposure(0.0f);
      if (!cameraName_.empty()) {
        LOGW("camera '%s' not found (no such Camera prim); auto-fitting",
             cameraName_.c_str());
      }
      // Frame on the visible geometry. Two inflators are excluded so pan/dolly
      // sensitivity (scaled by the framing distance) and the initial fit stay
      // sane: (1) guide breadcrumbs/endpoints, which span the whole map and are
      // hidden, and (2) sparse far-flung outliers (robust mass-trim). The aabb
      // metadata survives the --next CPU-geometry free, so this works post-load.
      float ngMin[3] = {1e30f, 1e30f, 1e30f}, ngMax[3] = {-1e30f, -1e30f, -1e30f};
      bool ngValid = false;
      for (const DrawMeshCPU& m : draw_.meshes) {
        if (m.purpose == "guide") continue;
        bool good = true;
        for (int a = 0; a < 3; ++a) {
          if (!(m.aabbMax[a] >= m.aabbMin[a]) || !std::isfinite(m.aabbMin[a]) ||
              !std::isfinite(m.aabbMax[a])) { good = false; break; }
        }
        if (!good) continue;
        // Instanced prototypes carry world-space bounds in aabbMin/aabbMax (set
        // by BuildDrawInstances / the next loader), so no per-instance expansion
        // is needed here.
        for (int a = 0; a < 3; ++a) {
          ngMin[a] = std::min(ngMin[a], m.aabbMin[a]);
          ngMax[a] = std::max(ngMax[a], m.aabbMax[a]);
        }
        ngValid = true;
      }
      const float* fitMin = ngValid ? ngMin : draw_.aabbMin;
      const float* fitMax = ngValid ? ngMax : draw_.aabbMax;
      if (robustBoundsValid_) {
        fitMin = robustBoundsMin_;
        fitMax = robustBoundsMax_;
      }
      camera_.fitToScene(fitMin, fitMax);
      if (!viewDirExplicit_) {
        float openYaw = 0.0f;
        if (FindOpenSideYaw(draw_, fitMin, fitMax, upAxis, &openYaw)) {
          camera_.setOrbit(camera_.target(), openYaw, 0.2f,
                           camera_.distance() * 0.72f);
          LOGI("camera: initial view uses least-enclosed scene side");
        }
      }
      if (viewDirExplicit_) {
        // OrbitCamera stores target-to-eye, the opposite of --view-dir's
        // conventional eye-to-target direction.
        const float dir[3] = {-viewDir_[0], -viewDir_[1], -viewDir_[2]};
        float yaw, pitch;
        if (upAxis == 2) {
          pitch = std::asin(std::clamp(dir[2], -1.0f, 1.0f));
          yaw = std::atan2(dir[0], dir[1]);
        } else {
          pitch = std::asin(std::clamp(dir[1], -1.0f, 1.0f));
          yaw = std::atan2(dir[0], dir[2]);
        }
        camera_.setOrbit(camera_.target(), yaw, pitch, camera_.distance());
      }
      // --cam-dolly: scale the fitted distance (<1 zooms in past the framing so
      // peripheral geometry leaves the frustum -- exercises culling headlessly).
      if (camDolly_ > 0.0f && camDolly_ != 1.0f) {
        camera_.setOrbit(camera_.target(), camera_.yaw(), camera_.pitch(),
                         camera_.distance() * camDolly_);
      }
    }
  }
  authoredLights_ = draw_.lights;
  runtimeDomeValid_ = false;
  runtimeDomeKey_.clear();
  gui_.setScene(&loaded_, &draw_);
  if (!startupEnvmapPath_.empty()) {
    Gui::DomeLightRequest request;
    request.source = Gui::DomeSource::File;
    request.filePath = startupEnvmapPath_;
    request.intensity = startupEnvmapIntensity_;
    request.rotationDegrees = startupEnvmapRotationDegrees_;
    applyDomeLightRequest(request);
    gui_.setDomeRequestState(request);
  }
  gui_.setFacialControls(ReadVcharControls(loaded_.stage));
  if (lensFocusOverride_ > 0.0f)
    cameraLens_.focusDistance = lensFocusOverride_;
  if (lensFStopOverride_ > 0.0f) {
    if (framedAuthoredCamera) {
      // Authored USD focal length is in tenths of a stage unit.
      cameraLens_.apertureRadius =
          framedFocalLength / (20.0f * lensFStopOverride_);
    } else {
      // An auto-fit camera has no authored USD focal length. Interpret the
      // CLI f-stop as a conventional 50 mm lens and convert metres to the
      // stage's world units. Treating the raw `50` as tenths of a stage unit
      // made a metre-scale scene use a 0.625 m aperture at f/4.
      const double metersPerUnit =
          draw_.metersPerUnit > 0.0 ? draw_.metersPerUnit : 0.01;
      cameraLens_.apertureRadius = MakeAutoRtCameraLens(
          std::max(cameraLens_.focusDistance, camera_.distance()),
          lensFStopOverride_, metersPerUnit).apertureRadius;
    }
  }
  if (pathTrace_.enabled &&
      (lensFocusOverride_ > 0.0f || lensFStopOverride_ > 0.0f)) {
    if (!(cameraLens_.focusDistance > 0.0f))
      cameraLens_.focusDistance = std::max(camera_.distance(), 1.0e-3f);
    if (!(cameraLens_.apertureRadius > 0.0f))
      cameraLens_.apertureRadius = MakeAutoRtCameraLens(
          cameraLens_.focusDistance, 2.8f,
          draw_.metersPerUnit > 0.0 ? draw_.metersPerUnit : 0.01)
                                       .apertureRadius;
  }
  gui_.setCameraLens(cameraLens_);
  gui_.setNextStage(nextStageSnapshot_.get());
  {
    std::vector<std::string> deferred;
    if (nextSession_) {
      for (const lightusd::next::Path& path :
           nextSession_->GetDeferredPayloadPaths()) {
        deferred.push_back(path.str());
      }
    }
    gui_.setDeferredPayloadPaths(std::move(deferred));
  }
  // Apply a one-shot --select (prim path) once the scene + draw meshes exist.
  if (!initialSelect_.empty()) {
    gui_.selectByPath(initialSelect_, -1);
    initialSelect_.clear();
  }
}

void App::drainProgressiveLoad() {
  if (!streamLoadActive_ || !loadStream_) return;
  const bool conversionOnly = quitAfterConvert_;
  const auto sliceStart = std::chrono::steady_clock::now();
  // Automated progressive benchmarks need one representative useful present,
  // then can drain aggressively instead of spending llvmpipe time drawing every
  // intermediate refinement. Interactive runs retain the configured frame slice.
  const double budgetMs =
      (quitAfterFullUpload_ && streamFirstFrameLogged_)
          ? 1000.0
          : std::clamp(uploadBudgetMs_, 1.0, 33.0);
  auto elapsedMs = [&]() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - sliceStart)
        .count();
  };

  renderer_->clearSceneUploadError();
  auto failProgressive = [this](const std::string& error) {
    const std::string message = error.empty() ? "progressive scene upload failed"
                                              : error;
    LOGE("%s", message.c_str());
    if (pendingLoaded_) {
      pendingLoaded_->ok = false;
      pendingLoaded_->err = message;
    }
    streamCompleteSeen_ = true;
    progressiveActive_ = false;
    gui_.setSceneMutating(false);
  };
  auto checkRendererUpload = [&]() {
    if (renderer_->sceneUploadError().empty()) return true;
    failProgressive(renderer_->sceneUploadError());
    return false;
  };

  // Diagnostic wire/source-face buffers are optional. Keep their compact CPU
  // source until a diagnostic mode is requested instead of consuming GPU memory
  // and upload bandwidth during an ordinary shaded load.
  if (!conversionOnly && streamFirstFrameLogged_ && streamAuxEager_) {
    while (nextAux_ < draw_.meshes.size() && elapsedMs() < budgetMs) {
      restoreDeferredMeshAux(nextAux_);
      renderer_->uploadMeshAux(nextAux_, draw_.meshes[nextAux_]);
      if (!renderer_->sceneUploadError().empty()) {
        LOGE("progressive mesh auxiliary upload failed: %s",
             renderer_->sceneUploadError().c_str());
        progressiveActive_ = false;
        return;
      }
      if (useNextLoader_ && !needsMeshCpuGeometryForRT() &&
          !MeshIsDeformable(draw_.meshes[nextAux_])) {
        FreeMeshAuxCPU(draw_.meshes[nextAux_]);
      }
      ++nextAux_;
    }
  }

  ProgressiveSceneEvent event;
  auto retainPointMetadata = [](const DrawPointsCPU& source) {
    DrawPointsCPU metadata;
    metadata.name = source.name;
    metadata.absPath = source.absPath;
    metadata.purpose = source.purpose;
    metadata.gaussian = source.gaussian;
    metadata.colorsInterpolation = source.colorsInterpolation;
    metadata.opacitiesInterpolation = source.opacitiesInterpolation;
    metadata.materialId = source.materialId;
    std::memcpy(metadata.world, source.world, sizeof(metadata.world));
    std::memcpy(metadata.aabbMin, source.aabbMin, sizeof(metadata.aabbMin));
    std::memcpy(metadata.aabbMax, source.aabbMax, sizeof(metadata.aabbMax));
    return metadata;
  };
  auto retainCurveMetadata = [](const DrawCurvesCPU& source) {
    DrawCurvesCPU metadata;
    metadata.name = source.name;
    metadata.absPath = source.absPath;
    metadata.purpose = source.purpose;
    metadata.materialId = source.materialId;
    // Curve picking rasterizes a compact carrier ID/depth buffer from the
    // same preview strands that appendCurves() sends to the renderer.  Keep
    // those sampled positions/counts/widths here: retaining only the AABB
    // makes a visible curve impossible to identify and forces selection to
    // fall through to an enclosing mesh bound.  The next loader has already
    // applied --curve-preview-prims/--curve-preview-strands before this handoff,
    // so this remains bounded by the requested preview budget.
    metadata.points = source.points;
    metadata.vertexCounts = source.vertexCounts;
    metadata.widths = source.widths;
    std::memcpy(metadata.world, source.world, sizeof(metadata.world));
    std::memcpy(metadata.aabbMin, source.aabbMin, sizeof(metadata.aabbMin));
    std::memcpy(metadata.aabbMax, source.aabbMax, sizeof(metadata.aabbMax));
    return metadata;
  };
  while (loadStream_->tryPop(&event)) {
    if (event.type == ProgressiveSceneEvent::Type::PreviewScene) {
      DrawScene preview = std::move(event.scene);
      std::string validationError;
      if (!ValidateDrawScene(preview, &validationError)) {
        failProgressive("progressive preview rejected: " + validationError);
        return;
      }
      renderer_->clearSceneUploadError();
      renderer_->beginScene(preview.materials,
                            static_cast<int>(preview.textures.size()));
      if (!conversionOnly) {
        for (const DrawMeshCPU& mesh : preview.meshes) {
          renderer_->appendMeshSurface(mesh);
          if (!checkRendererUpload()) return;
        }
      }
      draw_ = std::move(preview);
      streamRendererBegun_ = true;
      streamPreviewActive_ = true;
      streamHasUsefulGeometry_ = !draw_.meshes.empty();
      if (!draw_.meshes.empty()) {
        for (int k = 0; k < 3; ++k) {
          streamBoundsMin_[k] = draw_.meshes[0].aabbMin[k];
          streamBoundsMax_[k] = draw_.meshes[0].aabbMax[k];
        }
        camera_.fitToScene(streamBoundsMin_, streamBoundsMax_);
        streamCameraFramed_ = true;
      }
      if (loadOpts_.timing) {
        LOGI("timing: composition preview uploaded %.3f s (%zu boxes)",
             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          runStart_)
                 .count(),
             draw_.meshes.empty() ? 0 : draw_.meshes[0].instanceCount());
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Reset) {
      ++prototypeLodGeneration_;
      queuedPrototypeLods_.clear();
      clearPtexDecode();
      renderer_->clearSceneUploadError();
      renderer_->beginScene({}, 0);
      draw_ = DrawScene{};
      deferredMeshAux_.clear();
      streamRendererBegun_ = false;
      streamPreviewActive_ = false;
      streamHasUsefulGeometry_ = false;
      streamCameraFramed_ = false;
      for (int k = 0; k < 3; ++k) {
        streamBoundsMin_[k] = 1e30f;
        streamBoundsMax_[k] = -1e30f;
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Resources) {
      if (!streamRendererBegun_) {
        renderer_->beginScene(event.materials, event.textureCount);
        streamRendererBegun_ = true;
      } else {
        renderer_->syncSceneResources(event.materials, event.textureCount);
      }
      draw_.materials = std::move(event.materials);
      draw_.textures.resize(static_cast<size_t>(std::max(0, event.textureCount)));
      draw_.upAxis = event.upAxis;
      camera_.setUpAxis((event.upAxis == "Z" || event.upAxis == "z") ? 2 : 1);
    } else if (event.type == ProgressiveSceneEvent::Type::Mesh) {
      if (!streamRendererBegun_) {
        draw_.materials.emplace_back();
        renderer_->beginScene(draw_.materials, 0);
        streamRendererBegun_ = true;
      }
      DrawMeshCPU mesh = std::move(event.mesh);
      std::string validationError;
      if (!ValidateDrawMesh(mesh, draw_.materials.size(), &validationError)) {
        failProgressive("progressive mesh rejected: " + validationError);
        return;
      }
      const size_t vertices = mesh.vertices.size();
      const size_t triangles = mesh.indices.size() / 3;
      const size_t effectiveTriangles =
          triangles * std::max<size_t>(size_t(1), mesh.instanceCount());
      for (int a = 0; a < 3; ++a) {
        streamBoundsMin_[a] = std::min(streamBoundsMin_[a], mesh.aabbMin[a]);
        streamBoundsMax_[a] = std::max(streamBoundsMax_[a], mesh.aabbMax[a]);
      }
      if (!streamCameraFramed_ && mesh.purpose != "guide" && triangles > 0) {
        camera_.fitToScene(streamBoundsMin_, streamBoundsMax_);
        streamCameraFramed_ = true;
      }
      if (!conversionOnly) {
        if (streamAuxEager_)
          renderer_->appendMesh(mesh);
        else
          renderer_->appendMeshSurface(mesh);
        if (!checkRendererUpload()) return;
      }
      streamUploadedVertices_ += vertices;
      streamUploadedTriangles_ += triangles;
      streamUploadedEffectiveTriangles_ += effectiveTriangles;
      draw_.vertexCount = streamUploadedVertices_;
      draw_.triangleCount += triangles;
      if (mesh.purpose != "guide" && effectiveTriangles > 0)
        streamHasUsefulGeometry_ = true;
      draw_.meshes.push_back(std::move(mesh));
      ptexMeshRequested_.push_back(uint8_t{0});
      ptexMeshDemanded_.push_back(uint8_t{0});
      deferredMeshAux_.emplace_back();
      DrawMeshCPU& retained = draw_.meshes.back();
      startPrototypeLodBuild(draw_.meshes.size() - 1, retained);
      if (useNextLoader_ && !needsMeshCpuGeometryForRT() && !MeshIsDeformable(retained)) {
        if (streamAuxEager_)
          FreeMeshGeometryCPU(retained);
        else {
          FreeMeshSurfaceCPU(retained);
          compactDeferredMeshAux(draw_.meshes.size() - 1);
        }
        if (!rasterLodEnabled_ && !dynamicPrototypeLodEnabled_)
          FreeMeshInstanceCPU(retained);
      }
      if (streamAuxEager_) ++nextAux_;
      if (!streamFirstUploadLogged_ && loadOpts_.timing) {
        streamFirstUploadLogged_ = true;
        LOGI("timing: first geometry uploaded %.3f s (%zu vertices, %zu triangles)",
             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          runStart_)
                 .count(),
             vertices, triangles);
      }
      if (elapsedMs() >= budgetMs) break;
    } else if (event.type == ProgressiveSceneEvent::Type::Points) {
      if (!streamRendererBegun_) {
        draw_.materials.emplace_back();
        renderer_->beginScene(draw_.materials, 0);
        streamRendererBegun_ = true;
      }
      DrawPointsCPU points = std::move(event.points);
      std::string validationError;
      if (!ValidateDrawPoints(points, draw_.materials.size(), &validationError)) {
        failProgressive("progressive points rejected: " + validationError);
        return;
      }
      const bool useful = !points.points.empty();
      for (int a = 0; a < 3; ++a) {
        streamBoundsMin_[a] = std::min(streamBoundsMin_[a], points.aabbMin[a]);
        streamBoundsMax_[a] = std::max(streamBoundsMax_[a], points.aabbMax[a]);
      }
      if (!streamCameraFramed_ && points.purpose != "guide" && useful) {
        camera_.fitToScene(streamBoundsMin_, streamBoundsMax_);
        streamCameraFramed_ = true;
      }
      if (useful) {
        const bool usefulPurpose = points.purpose != "guide";
        DrawPointsCPU metadata = retainPointMetadata(points);
        if (!conversionOnly) renderer_->appendPoints(std::move(points));
        if (!conversionOnly && !checkRendererUpload()) return;
        draw_.points.push_back(std::move(metadata));
        if (usefulPurpose) streamHasUsefulGeometry_ = true;
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Curves) {
      if (!streamRendererBegun_) {
        draw_.materials.emplace_back();
        renderer_->beginScene(draw_.materials, 0);
        streamRendererBegun_ = true;
      }
      DrawCurvesCPU curves = std::move(event.curves);
      std::string validationError;
      if (!ValidateDrawCurves(curves, draw_.materials.size(), &validationError)) {
        failProgressive("progressive curves rejected: " + validationError);
        return;
      }
      const bool useful = !curves.points.empty();
      for (int a = 0; a < 3; ++a) {
        streamBoundsMin_[a] = std::min(streamBoundsMin_[a], curves.aabbMin[a]);
        streamBoundsMax_[a] = std::max(streamBoundsMax_[a], curves.aabbMax[a]);
      }
      if (!streamCameraFramed_ && curves.purpose != "guide" && useful) {
        camera_.fitToScene(streamBoundsMin_, streamBoundsMax_);
        streamCameraFramed_ = true;
      }
      if (useful) {
        const bool usefulPurpose = curves.purpose != "guide";
        DrawCurvesCPU metadata = retainCurveMetadata(curves);
        if (!conversionOnly) renderer_->appendCurves(std::move(curves));
        if (!conversionOnly && !checkRendererUpload()) return;
        draw_.curves.push_back(std::move(metadata));
        if (usefulPurpose) streamHasUsefulGeometry_ = true;
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Volume) {
      if (!streamRendererBegun_) {
        draw_.materials.emplace_back();
        renderer_->beginScene(draw_.materials, 0);
        streamRendererBegun_ = true;
      }
      DrawVolumeCPU volume = std::move(event.volume);
      std::string validationError;
      if (!ValidateDrawVolume(volume, &validationError)) {
        failProgressive("progressive volume rejected: " + validationError);
        return;
      }
      const bool useful = !volume.density.empty();
      for (int a = 0; a < 3; ++a) {
        streamBoundsMin_[a] = std::min(streamBoundsMin_[a], volume.aabbMin[a]);
        streamBoundsMax_[a] = std::max(streamBoundsMax_[a], volume.aabbMax[a]);
      }
      if (!streamCameraFramed_ && useful) {
        camera_.fitToScene(streamBoundsMin_, streamBoundsMax_);
        streamCameraFramed_ = true;
      }
      if (useful) {
        DrawVolumeCPU metadata;
        metadata.name = volume.name;
        std::memcpy(metadata.dim, volume.dim, sizeof(metadata.dim));
        std::memcpy(metadata.world, volume.world, sizeof(metadata.world));
        std::memcpy(metadata.aabbMin, volume.aabbMin,
                    sizeof(metadata.aabbMin));
        std::memcpy(metadata.aabbMax, volume.aabbMax,
                    sizeof(metadata.aabbMax));
        metadata.densityScale = volume.densityScale;
        std::memcpy(metadata.albedo, volume.albedo, sizeof(metadata.albedo));
        std::memcpy(metadata.emission, volume.emission,
                    sizeof(metadata.emission));
        metadata.background = volume.background;
        if (!conversionOnly) renderer_->appendVolume(volume);
        if (!conversionOnly && !checkRendererUpload()) return;
        draw_.volumes.push_back(std::move(metadata));
        streamHasUsefulGeometry_ = true;
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Texture) {
      if (event.textureSlot < 0) continue;
      const size_t slot = static_cast<size_t>(event.textureSlot);
      if (draw_.textures.size() <= slot) draw_.textures.resize(slot + 1);
      if (!conversionOnly) {
        // Progressive texture events arrive while the loader and UI are still
        // running.  In the threaded render path the renderer/context is owned
        // by the render thread; calling uploadTexture directly here races that
        // thread and can crash as soon as the first decoded payload is queued.
        // Keep the payload alive in the GPU-op closure, matching the upload
        // marshalling used by the completion and residency paths below.
        if (renderThreadActive_) {
          auto upload = std::make_shared<DrawTextureCPU>(event.texture);
          postGpu([this, slot, upload = std::move(upload)] {
            renderer_->uploadTexture(static_cast<int>(slot), *upload);
          });
        } else {
          renderer_->uploadTexture(event.textureSlot, event.texture);
        }
      }
      // The block payload is the resident source after upload. Keep its asset
      // identity for future refinement, but drop the 4x larger RGBA staging.
      if (!event.texture.compressed.data.empty()) {
        event.texture.image.data.clear();
        event.texture.image.data.shrink_to_fit();
      }
      draw_.textures[slot] = std::move(event.texture);
      // Progressive texture events are already GPU-resident at this point.
      // Keep the asset identity and sampling metadata for residency eviction
      // and re-decode, but do not pin another ordinary RGBA/compressed payload
      // in the interactive scene. Ptex/UDIM/native streaming carriers retain
      // their specialized source state for page updates.
      if (!draw_.textures[slot].isUdim && !draw_.textures[slot].isPtex &&
          !draw_.textures[slot].compressedFinal &&
          TextureCanRedecodeFromSource(draw_.textures[slot])) {
        ReleaseOrdinaryTexturePayload(&draw_.textures[slot]);
      }
      if (loadOpts_.timing &&
          (slot < 4 || ((slot + 1) % 32) == 0)) {
        LOGI("timing: async texture %zu ready %.3f s (%dx%d, compressed=%s)",
             slot,
             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          runStart_).count(),
             draw_.textures[slot].image.width,
             draw_.textures[slot].image.height,
             draw_.textures[slot].compressed.data.empty() ? "no" : "yes");
      }
    } else if (event.type == ProgressiveSceneEvent::Type::Complete) {
      clearPtexDecode();
      std::vector<DrawMeshCPU> streamedMeshes = std::move(draw_.meshes);
      std::vector<DrawPointsCPU> streamedPoints = std::move(draw_.points);
      std::vector<DrawCurvesCPU> streamedCurves = std::move(draw_.curves);
      std::vector<DrawVolumeCPU> streamedVolumes = std::move(draw_.volumes);
      std::vector<DrawTextureCPU> streamedTextures = std::move(draw_.textures);
      DrawScene finalScene = std::move(event.scene);
      if (!conversionOnly) {
        DrawScene finalValidation;
        finalValidation.materials = finalScene.materials;
        finalValidation.textures.resize(finalScene.textures.size());
        finalValidation.points = finalScene.points;
        finalValidation.curves = finalScene.curves;
        finalValidation.volumes = finalScene.volumes;
        std::string validationError;
        if (!ValidateDrawScene(finalValidation, &validationError)) {
          failProgressive("progressive final scene rejected: " + validationError);
          return;
        }
        renderer_->clearSceneUploadError();
        renderer_->syncSceneResources(
            finalScene.materials, static_cast<int>(finalScene.textures.size()));
        for (DrawPointsCPU& points : finalScene.points) {
          DrawPointsCPU metadata = retainPointMetadata(points);
          renderer_->appendPoints(std::move(points));
          if (!checkRendererUpload()) return;
          points = std::move(metadata);
        }
        for (DrawCurvesCPU& curves : finalScene.curves) {
          DrawCurvesCPU metadata = retainCurveMetadata(curves);
          renderer_->appendCurves(std::move(curves));
          if (!checkRendererUpload()) return;
          curves = std::move(metadata);
        }
      }
      draw_ = std::move(finalScene);
      draw_.meshes = std::move(streamedMeshes);
      if (!streamedPoints.empty()) draw_.points = std::move(streamedPoints);
      if (!streamedCurves.empty()) draw_.curves = std::move(streamedCurves);
      if (!streamedVolumes.empty()) draw_.volumes = std::move(streamedVolumes);
      if (draw_.textures.size() < streamedTextures.size())
        draw_.textures.resize(streamedTextures.size());
      for (size_t i = 0; i < streamedTextures.size(); ++i) {
        if (!streamedTextures[i].assetIdentifier.empty() ||
            !streamedTextures[i].image.data.empty() ||
            !streamedTextures[i].compressed.data.empty() ||
            streamedTextures[i].deferredDecode ||
            streamedTextures[i].isPtex || streamedTextures[i].isUdim) {
          draw_.textures[i] = std::move(streamedTextures[i]);
        }
      }
      // Deferred ordinary slots are scheduled from live camera demand. Upload
      // only specialized synchronous payloads here; they remain outside the
      // generic residency manager.
      for (size_t i = 0; !conversionOnly && i < draw_.textures.size(); ++i) {
        if (!draw_.textures[i].deferredDecode &&
            (!draw_.textures[i].image.data.empty() ||
             !draw_.textures[i].compressed.data.empty() ||
             draw_.textures[i].streamingMutable)) {
          if (renderThreadActive_) {
            auto upload = std::make_shared<DrawTextureCPU>(draw_.textures[i]);
            postGpu([this, i, upload = std::move(upload)] {
              renderer_->uploadTexture(static_cast<int>(i), *upload);
            });
          } else {
            renderer_->uploadTexture(static_cast<int>(i), draw_.textures[i]);
          }
        }
      }
      resetTextureResidency();
      draw_.vertexCount = streamUploadedVertices_;
      nextMesh_ = draw_.meshes.size();
      nextTex_ = draw_.textures.size();
      nextVolume_ = 0;
      nextPtexTexture_ = 0;
      ptexPhysicalCaches_.clear();
      ptexReaders_.clear();
      ptexRequestedFaces_.clear();
      ptexRequestedFaceSets_.clear();
      ptexRequestCursors_.clear();
      ptexMeshRequested_.assign(draw_.meshes.size(), uint8_t{0});
      ptexMeshDemanded_.assign(draw_.meshes.size(), uint8_t{0});
      progressiveActive_ = !draw_.meshes.empty() || !draw_.points.empty() ||
                           !draw_.curves.empty() || !draw_.textures.empty() ||
                           !draw_.volumes.empty();
      streamCompleteSeen_ = true;
      gui_.setSceneMutating(false);
      if (!streamFullConversionLogged_ && loadOpts_.timing) {
        streamFullConversionLogged_ = true;
        streamFullConversionSeconds_ =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         runStart_)
                .count();
        LOGI("timing: full scene converted %.3f s",
             streamFullConversionSeconds_);
        if (deferredAuxRawBytes_ > 0) {
          LOGI("memory: deferred mesh aux %.1f MiB compressed from %.1f MiB",
               static_cast<double>(deferredAuxCompressedBytes_) / (1024.0 * 1024.0),
               static_cast<double>(deferredAuxRawBytes_) / (1024.0 * 1024.0));
        }
      }
      if (quitAfterConvert_) quitAfterFullPresent_ = true;
    } else if (event.type == ProgressiveSceneEvent::Type::Failed) {
      streamCompleteSeen_ = true;
      gui_.setSceneMutating(false);
      if (pendingLoaded_) pendingLoaded_->err = std::move(event.error);
    }
  }
}

void App::ensureWireAuxReady() {
  const bool requested = gui_.wireframeMode() != 0 ||
                         gui_.renderMode() == RenderMode::Wireframe ||
                         gui_.renderMode() == RenderMode::SourceFaceId;
  if (!requested || !renderer_) return;

  // From this point onward, newly streamed meshes arrive with their auxiliary
  // buffers already resident. Complete all currently resident meshes in this
  // same frame so entering wire mode never exposes a partially populated edge
  // set that grows in visible batches over subsequent frames.
  streamAuxEager_ = true;
  const size_t resident = std::min(
      draw_.meshes.size(),
      static_cast<size_t>(std::max(0, renderer_->meshCount())));
  while (nextAux_ < resident) {
    restoreDeferredMeshAux(nextAux_);
    renderer_->uploadMeshAux(nextAux_, draw_.meshes[nextAux_]);
    if (useNextLoader_ && !needsMeshCpuGeometryForRT() &&
        !MeshIsDeformable(draw_.meshes[nextAux_])) {
      FreeMeshAuxCPU(draw_.meshes[nextAux_]);
    }
    ++nextAux_;
  }
}

void App::startPrototypeLodBuild(size_t meshIndex, const DrawMeshCPU& mesh) {
  if (!dynamicPrototypeLodEnabled_ || backend_ != Backend::Vulkan) {
    return;
  }
  if (!PrototypeNeedsGeneratedLods(mesh)) {
    if (loadOpts_.timing && mesh.instanceCount() > 0 &&
        mesh.indices.size() / 3 >= 2000) {
      LOGI("prototype LOD skipped: mesh %zu (instances=%zu morphs=%zu joints=%zu)",
           meshIndex, mesh.instanceCount(), mesh.morphs.size(),
           mesh.jointIdx.size());
    }
    return;
  }
  PrototypeLodBuildInput input;
  input.meshIndex = meshIndex;
  input.sceneGeneration = prototypeLodGeneration_;
  input.vertices = mesh.vertices;
  input.indices = mesh.indices;
  input.submeshes = mesh.submeshes;
  queuedPrototypeLods_.push_back(std::move(input));
  if (loadOpts_.timing) {
    LOGI("prototype LOD queued: mesh %zu (%zu unique, %zu effective triangles)",
         meshIndex, mesh.indices.size() / 3,
         (mesh.indices.size() / 3) * mesh.instanceCount());
  }
}

void App::pollPrototypeLodBuilds() {
  // Simplification is deliberately bounded: production scenes often have tens
  // of eligible prototypes, and launching one std::async thread per prototype
  // competes with rendering and texture decode precisely when interactivity is
  // most valuable.
  while (pendingPrototypeLods_.size() < 2 && !queuedPrototypeLods_.empty()) {
    PrototypeLodBuildInput input = std::move(queuedPrototypeLods_.front());
    queuedPrototypeLods_.pop_front();
    pendingPrototypeLods_.push_back({std::async(
        std::launch::async, [input = std::move(input)]() mutable {
          return BuildPrototypeLods(std::move(input));
        })});
  }
  for (size_t i = 0; i < pendingPrototypeLods_.size();) {
    auto& pending = pendingPrototypeLods_[i];
    if (pending.future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++i;
      continue;
    }
    PrototypeLodBuildResult result = pending.future.get();
    pendingPrototypeLods_.erase(pendingPrototypeLods_.begin() +
                                static_cast<std::ptrdiff_t>(i));
    if (result.sceneGeneration != prototypeLodGeneration_ ||
        result.meshIndex >= draw_.meshes.size() || result.levels.empty()) {
      continue;
    }
    draw_.meshes[result.meshIndex].prototypeLods = result.levels;
    std::string ratios;
    for (const DrawPrototypeLodCPU& level : result.levels) {
      if (!ratios.empty()) ratios += ", ";
      ratios += "L" + std::to_string(level.level) + "=" +
                std::to_string(static_cast<int>(level.triangleRatio * 100.0f)) +
                "%";
    }
    LOGI("prototype LOD ready: mesh %zu (%s)", result.meshIndex,
         ratios.c_str());
    gui_.notifyPrototypeLodsChanged();
    postGpu([this, meshIndex = result.meshIndex,
             levels = std::move(result.levels)]() mutable {
      renderer_->installPrototypeLods(meshIndex, levels);
    });
  }
}

void App::stepProgressiveUpload() {
  if (!progressiveActive_) return;
  // Keep uploads on the render/context thread, but use enough of each frame to
  // make progress on multi-million-triangle scenes. The old fixed 4 ms slice
  // could reduce a large scene to one buffer per frame and stretch upload over
  // many seconds. Override for latency-sensitive systems with
  // Configured by --upload-budget-ms; keep the render loop control explicit and
  // reproducible instead of depending on process environment.
  const double uploadBudgetMs = std::clamp(uploadBudgetMs_, 1.0, 33.0);
  const double tailBudgetMs = std::min(33.0, uploadBudgetMs + 4.0);
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsedMs = [&]() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     t0)
        .count();
  };
  // Geometry first so meshes appear, normally ~8ms/frame.
  while (nextMesh_ < draw_.meshes.size()) {
    renderer_->appendMesh(draw_.meshes[nextMesh_]);
    if (!renderer_->sceneUploadError().empty()) {
      LOGE("progressive mesh upload failed: %s",
           renderer_->sceneUploadError().c_str());
      progressiveActive_ = false;
      return;
    }
    startPrototypeLodBuild(nextMesh_, draw_.meshes[nextMesh_]);
    if (useNextLoader_ && !needsMeshCpuGeometryForRT() &&
        !MeshIsDeformable(draw_.meshes[nextMesh_])) {
      FreeMeshGeometryCPU(draw_.meshes[nextMesh_]);
      if (!rasterLodEnabled_ && !dynamicPrototypeLodEnabled_)
        FreeMeshInstanceCPU(draw_.meshes[nextMesh_]);
    }
    ++nextMesh_;
    if (elapsedMs() > uploadBudgetMs) break;
  }
  // Auxiliary source-face and authored-polygon wire buffers are only made
  // resident when their display mode has actually been requested.
  if (streamAuxEager_ && nextMesh_ >= draw_.meshes.size()) {
    while (nextAux_ < draw_.meshes.size()) {
      restoreDeferredMeshAux(nextAux_);
      renderer_->uploadMeshAux(nextAux_, draw_.meshes[nextAux_]);
      if (!renderer_->sceneUploadError().empty()) {
        LOGE("progressive mesh auxiliary upload failed: %s",
             renderer_->sceneUploadError().c_str());
        progressiveActive_ = false;
        return;
      }
      if (useNextLoader_ && !needsMeshCpuGeometryForRT() &&
          !MeshIsDeformable(draw_.meshes[nextAux_])) {
        FreeMeshAuxCPU(draw_.meshes[nextAux_]);
      }
      ++nextAux_;
      if (elapsedMs() > tailBudgetMs) break;
    }
  }
  const bool auxReady = !streamAuxEager_ || nextAux_ >= draw_.meshes.size();
  // Stream ready textures independently of the geometry tail. Large meshes can
  // take several frames to upload; waiting for all of them left already-decoded
  // materials on the gray fallback for the whole interval.
  if (auxReady) {
    while (nextTex_ < draw_.textures.size()) {
      renderer_->uploadTexture(static_cast<int>(nextTex_), draw_.textures[nextTex_]);
      if (!cudaRt_ && !hipRt_ && draw_.textures[nextTex_].isPtex &&
          draw_.textures[nextTex_].HasPtexSourceData()) {
        // Fallback pixels are now device-resident. Keep only compact native
        // Ptex bytes for on-demand page decode.
        draw_.textures[nextTex_].image.data.clear();
        draw_.textures[nextTex_].image.data.shrink_to_fit();
      }
      ++nextTex_;
      if (elapsedMs() > tailBudgetMs) break;
    }
  }
  // UsdVol volumes (OpenVDB) after meshes + textures.
  if (nextMesh_ >= draw_.meshes.size() && auxReady &&
      nextTex_ >= draw_.textures.size()) {
    while (!stepPtexResidency(tailBudgetMs - elapsedMs())) {
      if (elapsedMs() > tailBudgetMs) break;
    }
  }
  const bool ptexReady = nextPtexTexture_ >= draw_.textures.size();
  if (nextMesh_ >= draw_.meshes.size() && auxReady &&
      nextTex_ >= draw_.textures.size() && ptexReady) {
    while (nextVolume_ < draw_.volumes.size()) {
      renderer_->appendVolume(draw_.volumes[nextVolume_]);
      if (!renderer_->sceneUploadError().empty()) {
        LOGE("progressive volume upload failed: %s",
             renderer_->sceneUploadError().c_str());
        progressiveActive_ = false;
        return;
      }
      ++nextVolume_;
      if (elapsedMs() > tailBudgetMs) break;
    }
  }
  if (nextMesh_ >= draw_.meshes.size() && auxReady &&
      nextTex_ >= draw_.textures.size() &&
      ptexReady &&
      nextVolume_ >= draw_.volumes.size()) {
    progressiveActive_ = false;
  }
}

namespace {

bool AppendPtexFaceTableUpdates(
    const DrawTextureCPU& texture, uint32_t face,
    const DrawPtexFaceRectCPU& rect,
    std::vector<Renderer::TextureRegionUpdate>* updates) {
  if (!updates || face >= texture.ptexFaceRects.size() ||
      texture.image.width <= 0) {
    return false;
  }
  uint8_t texels[8u * 4u];
  EncodePtexFaceRectTexels(rect, texels);
  size_t linear = size_t(texture.ptexRectTexelOffset) + size_t(face) * 8u;
  size_t consumed = 0;
  while (consumed < 8u) {
    const int x = static_cast<int>(linear % size_t(texture.image.width));
    const int y = static_cast<int>(linear / size_t(texture.image.width));
    const int count = static_cast<int>(
        std::min<size_t>(8u - consumed, size_t(texture.image.width - x)));
    Renderer::TextureRegionUpdate update;
    update.x = x;
    update.y = y;
    update.width = count;
    update.height = 1;
    update.rowBytes = size_t(count) * 4u;
    update.rgba.assign(texels + consumed * 4u,
                       texels + (consumed + size_t(count)) * 4u);
    updates->push_back(std::move(update));
    linear += static_cast<size_t>(count);
    consumed += static_cast<size_t>(count);
  }
  return true;
}

}  // namespace

bool App::stepPtexResidency(double deadlineMs) {
  if (ptexDecodeActive_) {
    if (!finishPtexDecode(/*wait=*/false, /*discard=*/false)) return false;
    // Limit context-thread work to one completed page upload per call.
    return false;
  }
  if (nextPtexTexture_ >= draw_.textures.size()) return true;
  if (deadlineMs <= 0.0) return false;
  if (ptexPhysicalCaches_.size() != draw_.textures.size()) {
    ptexPhysicalCaches_.resize(draw_.textures.size());
  }
  if (ptexReaders_.size() != draw_.textures.size()) {
    ptexReaders_.resize(draw_.textures.size());
  }
  while (nextPtexTexture_ < draw_.textures.size()) {
    DrawTextureCPU& texture = draw_.textures[nextPtexTexture_];
    if (texture.ptexPhysicalCacheSlots == 0 ||
        !texture.HasPtexSourceData() || texture.ptexFaceRects.empty()) {
      ++nextPtexTexture_;
      continue;
    }
    if (!ptexPhysicalCaches_[nextPtexTexture_]) {
      ptexPhysicalCaches_[nextPtexTexture_] =
          std::make_unique<PtexPhysicalPageCache>(
              texture.ptexPhysicalCacheSlots);
    }
    const bool demandDriven = texture.ptexDemandDriven;
    if (ptexRequestCursors_.size() < draw_.textures.size())
      ptexRequestCursors_.resize(draw_.textures.size());
    const size_t requestedCount =
        nextPtexTexture_ < ptexRequestedFaces_.size()
            ? ptexRequestedFaces_[nextPtexTexture_].size()
            : 0u;
    const size_t residencyFaceCount =
        demandDriven ? requestedCount : texture.ptexFaceRects.size();
    size_t& cursor = ptexRequestCursors_[nextPtexTexture_];
    if (cursor >= residencyFaceCount) {
      ++nextPtexTexture_;
      continue;
    }

    std::string error;
    if (!ptexReaders_[nextPtexTexture_]) {
      auto reader = std::make_shared<lightusd::ptx::Reader>();
      const std::vector<uint8_t>& source = texture.PtexSourceData();
      if (!lightusd::ptx::Reader::OpenMemory(source.data(), source.size(),
                                             reader.get(), &error)) {
        LOGW("Ptex residency source reopen failed: %s", error.c_str());
        ++nextPtexTexture_;
        continue;
      }
      ptexReaders_[nextPtexTexture_] = std::move(reader);
    }
    const std::shared_ptr<lightusd::ptx::Reader> reader =
        ptexReaders_[nextPtexTexture_];
    const uint32_t face = demandDriven
                              ? ptexRequestedFaces_[nextPtexTexture_][cursor++]
                              : static_cast<uint32_t>(cursor++);
    if (face >= texture.ptexFaceRects.size() ||
        face >= reader->info().faceInfo.size()) {
      LOGW("Ptex face request %u is out of range", face);
      continue;
    }
    const lightusd::ptx::FaceInfo& info = reader->info().faceInfo[face];
    uint32_t mip = 0;
    uint32_t width = info.width(), height = info.height();
    while (mip + 1u < reader->info().levels && texture.ptexTileEdge > 0 &&
           std::max(width, height) > texture.ptexTileEdge) {
      ++mip;
      width = std::max(1u, width >> 1u);
      height = std::max(1u, height >> 1u);
    }

    // The permanent fallback already has this quality (or better). Physical
    // slots are reserved only for faces forced below the desired mip by the
    // global atlas budget.
    if (!texture.ptexForceResidency &&
        texture.ptexFaceRects[face].mipLevel <= mip &&
        texture.ptexFaceRects[face].reserved == 0) {
      continue;
    }

    const size_t textureIndex = nextPtexTexture_;
    const uint32_t gutter = texture.ptexGutter;
    const uint64_t sceneGen = ptexSceneGen_;
    ptexDecodeFuture_ = std::async(
        std::launch::async,
        [reader, textureIndex, face, mip, gutter, sceneGen]() {
          PtexDecodeResult result;
          result.texture = textureIndex;
          result.face = face;
          result.sceneGen = sceneGen;
          result.ok = BuildPtexPage(*reader, face, mip, gutter,
                                    64ull * 1024ull * 1024ull, &result.page,
                                    &result.rect, &result.error);
          return result;
        });
    ptexDecodeActive_ = true;
    ++ptexAsyncJobsLaunched_;
    return false;
  }
  return true;
}

void App::loadFileBlocking(const std::string& path) {
  clearPtexDecode();
  loadCtrl_.resetProgress();
  LoadedScene tmp;
  DrawScene drawTmp;
  LoadOptions opts = loadOpts_;
  opts.gpuSkinning = wantsNextGpuSkinning();
  // View-dependent district LOD (--lod-stream): a proxy pre-pass promotes the
  // camera-nearest districts to full and writes a wrapper layer we load instead.
  // Only meaningful on the --next path (the only one that composes huge scenes
  // like Caldera). The original `path` stays as the displayed filename.
  std::string effPath = path;
  if (lodStream_ && useNextLoader_) {
    LodStreamOptions lo;
    lo.camera = cameraName_;
    lo.maxMemGiB = lodMaxMemGiB_;
    lo.maxVramGiB = lodMaxVramGiB_;
    lo.time = animTime_;
    std::string wrapper = PrepareLodStream(path, lo);
    if (!wrapper.empty()) effPath = wrapper;
  }
  if (useNextLoader_) {
    std::shared_ptr<lightusd::next::StageSession> session;
    const bool ok = LoadUSDViaNext(effPath, opts, &drawTmp, &tmp.warn, &tmp.err,
                                   &loadCtrl_, &session);
    tmp.ok = ok;
    tmp.filepath = path;
    tmp.render.meta.upAxis = drawTmp.upAxis;  // drive camera/grid up-axis
    if (session) {
      const lightusd::next::Stage& stage = *session->GetSnapshot().stage;
      const double s = stage.GetStartTimeCode();
      const double e = stage.GetEndTimeCode();
      const double fps = stage.GetTimeCodesPerSecond();
      if (fps > 0.0) tmp.render.meta.timeCodesPerSecond = fps;
      if (e > s) {
        tmp.render.meta.startTimeCode = s;
        tmp.render.meta.endTimeCode = e;
      }
    }
    loaded_ = std::move(tmp);
    draw_ = ok ? std::move(drawTmp) : DrawScene{};
    nextSession_ = ok ? std::move(session) : nullptr;
    nextStageSnapshot_ = nextSession_ ? nextSession_->GetSnapshot().stage
                                      : nullptr;
    applyLoaded(ok, /*progressive=*/false);
    return;
  }
  // Load the REST pose whenever the deform may be re-applied downstream -- which is
  // whenever GPU skinning could win, i.e. Auto as much as an explicit GPU. Gating
  // this on an explicit GPU alone (as it did) left the DEFAULT, Auto, baking the
  // pose into the geometry at load (DeformSkinnedMeshes) and then letting
  // updateSkinningEffective pick GPU anyway -- so the vertex shader, and the RT
  // vertex re-pose, deformed the ALREADY-DEFORMED geometry. Every animated
  // `--time` screenshot on the legacy loader was posed twice. If GPU turns out to
  // be ineligible, the block below re-renders with the CPU bake, so Auto still
  // lands on a correctly posed scene either way.
  const bool gpuRestLoad =
      std::isfinite(loadOpts_.timecode) && wantsGpuSkinningLoad();
  if (gpuRestLoad) opts.timecode = std::numeric_limits<double>::quiet_NaN();
  int autoW = 0, autoH = 0;
  getRequestedWindowSize(&autoW, &autoH);
  const AutoSubdivisionView autoSubdivView{
      camera_.fovYDeg(),
      autoH > 0 ? static_cast<float>(std::max(1, autoW)) /
                      static_cast<float>(autoH)
                : camera_.aspect(),
      camera_.yaw(),
      camera_.pitch(),
      tessQuality_,
      camDolly_,
      std::max(1, autoH)};
  // Streaming convert+build in one pass (also fully populates tmp.render).
  bool ok = LoadUsdMaybeAutoSubdivision(path, opts, &tmp, &drawTmp, rtPath_,
                                        &loadCtrl_, autoSubdivView);
  if (ok && gpuRestLoad) {
    const bool skeletal = SceneHasSkeletalSkinning(tmp.render);
    const bool morph = SceneHasBlendShapes(tmp.render);
    const int maxInfluences = MaxSkinInfluenceCount(tmp.render);
    const bool gpuEligible =
        renderer_ && renderer_->caps().supportsGpuSkinning &&
        (skeletal || morph) &&
        (!skeletal || (drawTmp.boneMatrixCount > 0 &&
                       (maxInfluences <= 4 ||
                        (renderer_->caps().supportsExtendedGpuSkinning &&
                         maxInfluences <= kMaxGpuTextureInfluences))));
    if (!gpuEligible) {
      DrawScene cpuDraw;
      std::string w, e;
      if (RenderSceneAtTime(tmp, loadOpts_.timecode, rtPath_, &cpuDraw, &w, &e,
                            &loadCtrl_)) {
        cpuDraw.materials = drawTmp.materials;
        cpuDraw.textures = drawTmp.textures;
        drawTmp = std::move(cpuDraw);
      } else {
        tmp.warn += w;
        tmp.err = e;
        ok = false;
      }
    }
  }
  loaded_ = std::move(tmp);
  draw_ = ok ? std::move(drawTmp) : DrawScene{};
  applyLoaded(ok, /*progressive=*/false);
}

void App::addRecentScene(const std::string& path) {
  if (path.empty()) return;
  // Normalize to an absolute path so the entry is stable regardless of cwd.
  std::string key = path;
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  if (!ec) key = abs.lexically_normal().string();

  auto& v = recentScenes_;
  v.erase(std::remove(v.begin(), v.end(), key), v.end());
  v.insert(v.begin(), key);
  constexpr size_t kMaxRecent = 12;
  if (v.size() > kMaxRecent) v.resize(kMaxRecent);
  gui_.setRecentScenes(v);

  if (!configPath_.empty()) {
    std::string err;
    if (!SaveRecentScenes(configPath_, v, &err)) {
      LOGW("could not save recent scenes to %s: %s", configPath_.string().c_str(),
           err.c_str());
    }
  }
}

void App::startLoadAsync(const std::string& path) {
  cancelAndJoinLoad();
  loadCtrl_.resetProgress();
  loadingPath_ = path;
  loadStart_ = std::chrono::steady_clock::now();
  loadFinished_.store(false);
  loadActive_ = true;
  pendingLoaded_ = std::make_unique<LoadedScene>();
  pendingDraw_ = std::make_unique<DrawScene>();
  LoadedScene* lp = pendingLoaded_.get();
  DrawScene* dp = pendingDraw_.get();
  // Worker touches only CPU data (no GL/VK), so this is thread-safe. The
  // streaming load convert+builds the DrawScene (dp) in one pass.
  const bool rt = rtPath_;
  LoadOptions opts = loadOpts_;
  opts.gpuSkinning = wantsNextGpuSkinning();
  // Populate GPU compressed-format capabilities so the CPU texture build can
  // cap-gate `--texture-compress` (e.g. astc -> BC7 fallback on a BC-only
  // desktop GPU). renderer_ is initialized before any load is started; caps are
  // copied by value into `opts` here, before the worker thread launches.
  if (renderer_) {
    const RendererCaps& rc = renderer_->caps();
    opts.textureOptions.caps.bc = rc.supportsBC;
    opts.textureOptions.caps.astc = rc.supportsASTC;
    opts.textureOptions.caps.etc2 = rc.supportsETC2;
    opts.textureOptions.caps.bc5 = rc.supportsBC5;
    opts.textureOptions.caps.bc6h = rc.supportsBC6H;
  }
  const double requestedTime = opts.timecode;
  const bool gpuRestLoad = std::isfinite(requestedTime) && wantsGpuSkinningLoad();
  if (gpuRestLoad) {
    opts.timecode = std::numeric_limits<double>::quiet_NaN();
  }
  const bool rendererGpuSkinning =
      renderer_ && renderer_->caps().supportsGpuSkinning;
  const bool rendererExtendedGpuSkinning =
      renderer_ && renderer_->caps().supportsExtendedGpuSkinning;
  const bool useNext = useNextLoader_;
  bool renderThreadOwnsContext = false;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  renderThreadOwnsContext = renderThreadActive_;
#endif
  // Both raster backends consume the same bounded producer/consumer events.
  // Keep RT/CUDA/HIP on their scene-owned paths, but do not make Vulkan raster
  // materialize the complete DrawScene before the first upload.
  const bool progressiveStream = useNext && !rt && !cudaRt_ && !hipRt_ &&
                                 !renderThreadOwnsContext;
  if (progressiveStream) {
    const size_t maxBytes =
        opts.streamBufferBytes ? opts.streamBufferBytes : (size_t(64) << 20);
    loadStream_ = std::make_shared<ProgressiveSceneStream>(maxBytes);
    streamLoadActive_ = true;
    streamRendererBegun_ = false;
    streamPreviewActive_ = false;
    streamCompleteSeen_ = false;
    streamCameraFramed_ = false;
    streamFirstUploadLogged_ = false;
    streamFirstFrameLogged_ = false;
    streamFullConversionLogged_ = false;
    streamFullConversionSeconds_ = 0.0;
    streamFullUploadLogged_ = false;
    streamHasUsefulGeometry_ = false;
    streamAuxEager_ = gui_.wireframeMode() != 0 ||
                      gui_.renderMode() == RenderMode::Wireframe ||
                      gui_.renderMode() == RenderMode::SourceFaceId;
    streamUploadedTriangles_ = 0;
    streamUploadedEffectiveTriangles_ = 0;
    streamUploadedVertices_ = 0;
    nextAux_ = 0;
    clearDeferredMeshAux();
    for (int a = 0; a < 3; ++a) {
      streamBoundsMin_[a] = 1e30f;
      streamBoundsMax_[a] = -1e30f;
    }
    draw_ = DrawScene{};
    gui_.setSceneMutating(true);
  }
  std::shared_ptr<ProgressiveSceneStream> stream = loadStream_;
  int autoW = 0, autoH = 0;
  getRequestedWindowSize(&autoW, &autoH);
  const AutoSubdivisionView autoSubdivView{
      camera_.fovYDeg(),
      autoH > 0 ? static_cast<float>(std::max(1, autoW)) /
                      static_cast<float>(autoH)
                : camera_.aspect(),
      camera_.yaw(),
      camera_.pitch(),
      tessQuality_,
      camDolly_,
      std::max(1, autoH)};
  loadThread_ = std::thread([this, path, opts, lp, dp, rt, useNext, stream,
                             autoSubdivView, requestedTime, gpuRestLoad,
                             rendererGpuSkinning,
                             rendererExtendedGpuSkinning]() {
    if (useNext) {
      lp->ok = LoadUSDViaNext(path, opts, dp, &lp->warn, &lp->err, &loadCtrl_,
                              &pendingNextSession_, stream.get());
      lp->filepath = path;
      // Surface the stage's animation range so --next gets a timeline (the Tydra
      // RenderScene meta is otherwise empty here). readAnimationRange reads these.
      if (pendingNextSession_) {
        const lightusd::next::StageSnapshot snapshot =
            pendingNextSession_->GetSnapshot();
        if (snapshot) {
          lp->render.meta.upAxis = snapshot->GetUpAxis();
          const double s = snapshot->GetStartTimeCode();
          const double e = snapshot->GetEndTimeCode();
          const double fps = snapshot->GetTimeCodesPerSecond();
          if (fps > 0.0) lp->render.meta.timeCodesPerSecond = fps;
          if (e > s) {
            lp->render.meta.startTimeCode = s;
            lp->render.meta.endTimeCode = e;
          }
        }
      }
    } else {
      LoadUsdMaybeAutoSubdivision(path, opts, lp, dp, rt, &loadCtrl_,
                                  autoSubdivView);
      if (lp->ok && gpuRestLoad) {
        const bool skeletal = SceneHasSkeletalSkinning(lp->render);
        const bool morph = SceneHasBlendShapes(lp->render);
        const int maxInfluences = MaxSkinInfluenceCount(lp->render);
        const bool gpuEligible =
            rendererGpuSkinning && (skeletal || morph) &&
            (!skeletal ||
             (dp->boneMatrixCount > 0 &&
              (maxInfluences <= 4 ||
               (rendererExtendedGpuSkinning &&
                maxInfluences <= kMaxGpuTextureInfluences))));
        if (!gpuEligible) {
          DrawScene cpuDraw;
          std::string warning, error;
          if (RenderSceneAtTime(*lp, requestedTime, rt, &cpuDraw, &warning,
                                &error, &loadCtrl_)) {
            cpuDraw.materials = dp->materials;
            cpuDraw.textures = dp->textures;
            *dp = std::move(cpuDraw);
          } else {
            lp->warn += warning;
            lp->err = error;
            lp->ok = false;
          }
        }
      }
    }
    loadFinished_.store(true, std::memory_order_release);
  });
}

void App::startRecomposeAsync(const std::set<std::string>& addPrimPaths) {
  if (useNextLoader_ && nextSession_) {
    if (addPrimPaths.empty() && loadOpts_.variantOverrides.empty()) return;
    cancelAndJoinLoad();
    loadCtrl_.resetProgress();
    loadingPath_ = loaded_.filepath;
    loadStart_ = std::chrono::steady_clock::now();
    loadFinished_.store(false);
    loadActive_ = true;
    pendingLoaded_ = std::make_unique<LoadedScene>();
    pendingDraw_ = std::make_unique<DrawScene>();
    pendingNextSession_ = nextSession_;
    LoadedScene* lp = pendingLoaded_.get();
    DrawScene* dp = pendingDraw_.get();
    LoadOptions opts = loadOpts_;
    opts.gpuSkinning = wantsNextGpuSkinning();
    if (!addPrimPaths.empty()) {
      // Hierarchy/Inspector actions may target an ancestor Xform rather than
      // the exact prim that owns a deferred payload. Expand each request to all
      // currently deferred payloads below it (and accept a selected descendant
      // of a deferred owner). StageSession::LoadPayloads then installs
      // WithDescendants rules, so payloads discovered inside the loaded payload
      // are recursively included as well.
      std::set<std::string> recursive = addPrimPaths;
      auto atOrBelow = [](const std::string& path,
                          const std::string& root) {
        return path == root ||
               (path.size() > root.size() &&
                path.compare(0, root.size(), root) == 0 &&
                path[root.size()] == '/');
      };
      for (const lightusd::next::Path& deferred :
           nextSession_->GetDeferredPayloadPaths()) {
        const std::string path = deferred.str();
        for (const std::string& requested : addPrimPaths) {
          if (atOrBelow(path, requested) || atOrBelow(requested, path)) {
            recursive.insert(path);
            break;
          }
        }
      }
      opts.payloadPolicy = PayloadPolicy::Whitelist;
      opts.payloadWhitelist = std::move(recursive);
    }
    const std::string path = loaded_.filepath;
    loadThread_ = std::thread([this, path, opts, lp, dp]() {
      lp->ok = LoadUSDViaNext(path, opts, dp, &lp->warn, &lp->err, &loadCtrl_,
                              &pendingNextSession_);
      lp->filepath = path;
      lp->render.meta.upAxis = dp->upAxis;
      loadFinished_.store(true, std::memory_order_release);
    });
    return;
  }

  if (!loaded_.comp.composed || !loaded_.comp.rootLayer) return;
  if (addPrimPaths.empty() && loadOpts_.variantOverrides.empty()) return;
  cancelAndJoinLoad();
  loadCtrl_.resetProgress();
  loadingPath_ = loaded_.filepath;
  loadStart_ = std::chrono::steady_clock::now();
  loadFinished_.store(false);
  loadActive_ = true;
  pendingLoaded_ = std::make_unique<LoadedScene>();
  pendingDraw_ = std::make_unique<DrawScene>();
  LoadedScene* lp = pendingLoaded_.get();
  DrawScene* dp = pendingDraw_.get();

  // Snapshot composition state for the worker: the root layer is shared
  // (read-only) and the whitelist is the union of already-loaded payloads and
  // the new requests.
  CompositionInfo prev;
  prev.composed = true;
  prev.rootLayer = loaded_.comp.rootLayer;
  prev.searchPaths = loaded_.comp.searchPaths;
  // Package-internal payloads resolve through this retained archive. Keep the
  // shared backing alive across the worker handoff just like rootLayer.
  prev.usdzAsset = loaded_.comp.usdzAsset;
  LoadOptions opts = loadOpts_;
  opts.gpuSkinning = wantsNextGpuSkinning();
  opts.payloadPolicy = PayloadPolicy::Whitelist;
  opts.payloadWhitelist = loaded_.comp.loadedPayloads;
  opts.payloadWhitelist.insert(addPrimPaths.begin(), addPrimPaths.end());

  const std::string path = loaded_.filepath;
  const bool rt = rtPath_;
  loadThread_ = std::thread([this, path, prev, opts, lp, dp, rt]() {
    RecomposeWithPayloads(path, prev, opts, lp, dp, rt, &loadCtrl_);
    loadFinished_.store(true, std::memory_order_release);
  });
}

void App::finishLoadIfReady() {
  if (!loadActive_) return;
  if (!loadFinished_.load(std::memory_order_acquire)) return;
  if (streamLoadActive_ && !streamCompleteSeen_) return;
  if (loadThread_.joinable()) loadThread_.join();  // sync point: worker fully done
  clearPtexDecode();  // readers point into the outgoing draw_'s source bytes
  loaded_ = std::move(*pendingLoaded_);
  const bool ok = loaded_.ok;
  const bool alreadyUploaded = streamLoadActive_ && ok;
  if (!streamLoadActive_) {
    // The async cull worker may still be iterating the outgoing scene (non-
    // streaming loads do not suspend culling). Join it + invalidate its grids
    // BEFORE the swap frees the old DrawScene, not after (setScene's join in
    // applyLoaded runs too late).
    gui_.prepareSceneSwap();
    draw_ = ok ? std::move(*pendingDraw_) : DrawScene{};
  } else if (!ok) {
    // Do not leave a partially uploaded scene visible after the producer has
    // reported a terminal load failure.
    gui_.prepareSceneSwap();
    draw_ = DrawScene{};
  }
  nextSession_ = ok ? std::move(pendingNextSession_) : nullptr;
  nextStageSnapshot_ = nextSession_ ? nextSession_->GetSnapshot().stage
                                    : nullptr;
  pendingNextSession_.reset();
  pendingLoaded_.reset();
  pendingDraw_.reset();
  loadActive_ = false;
  applyLoaded(ok, /*progressive=*/!alreadyUploaded, alreadyUploaded);
  streamLoadActive_ = false;
  loadStream_.reset();
}

void App::cancelAndJoinLoad() {
  // A pending file load supersedes any in-flight playback re-evaluation (which
  // reads loaded_ on a worker thread): stop it before loaded_ is replaced.
  clearPtexDecode();
  cancelAndJoinReconvert();
  if (loadStream_) loadStream_->cancel();
  if (loadThread_.joinable()) {
    loadCtrl_.cancel.store(true);
    loadThread_.join();
  }
  loadActive_ = false;
  loadFinished_.store(false);
  pendingLoaded_.reset();
  pendingDraw_.reset();
  pendingNextSession_.reset();
  loadStream_.reset();
  streamLoadActive_ = false;
  streamCompleteSeen_ = false;
  gui_.setSceneMutating(false);
}

void App::readAnimationRange() {
  const auto& m = loaded_.render.meta;
  animFps_ = m.timeCodesPerSecond > 0.0 ? m.timeCodesPerSecond : 24.0;
  if (m.startTimeCode.has_value() && m.endTimeCode.has_value() &&
      m.endTimeCode.value() > m.startTimeCode.value()) {
    animStart_ = m.startTimeCode.value();
    animEnd_ = m.endTimeCode.value();
    hasAnimation_ = true;
  } else {
    animStart_ = animEnd_ = 0.0;
    hasAnimation_ = false;
  }
  animTime_ = animStart_;
  reconvApplied_ = animStart_;
  animPlaying_ = false;
  reconvHasRequest_ = false;
  haveLastFrameTime_ = false;
}

const char* App::skinningModeName(SkinningMode mode) const {
  switch (mode) {
    case SkinningMode::GPU: return "gpu";
    case SkinningMode::CPU: return "cpu";
    case SkinningMode::Auto:
    default: return "auto";
  }
}

bool App::wantsGpuSkinningLoad() const {
  return skinningRequested_ == SkinningMode::GPU ||
         skinningRequested_ == SkinningMode::Auto;
}

// Should the --next loader emit GPU skinning attributes instead of baking the
// pose into the geometry at load? (LoadOptions::gpuSkinning; the Tydra path
// always emits them and decides in updateSkinningEffective.)
//
// The Vulkan ray tracer keeps the rest pose too: it cannot run the raster vertex
// shader (its BLAS is built from vertex buffers), but it re-poses those retained
// rest vertices per frame and rebuilds the BLAS -- far cheaper than what it used
// to do, which was re-run the entire converter for every new time code. See
// updateNextDeformFrameIfNeeded.
//
// The CUDA/HIP tracers read draw_ geometry directly rather than owning vertex
// buffers, so they cannot run the raster vertex shader either -- but they can
// take the same re-posed vertices, written back into draw_ before their BVH is
// built (poseNextDrawForTracer). They used to be pinned to the load-time CPU
// bake, which meant a converter re-run for every new time code and, worse, a
// SECOND deform implementation that could (and did) disagree with the shader's.
bool App::wantsNextGpuSkinning() const {
  return useNextLoader_ && wantsGpuSkinningLoad() && renderer_ &&
         renderer_->caps().supportsGpuSkinning;
}

bool App::needsMeshCpuGeometryForRT() const {
  // Once a live technique switch is possible (liveSwitchEnabled_, always true
  // -- see its declaration), any RT technique (CUDA/HIP/CPU) may be requested
  // later at runtime (View menu / the CPU RT keybinding) even when no *Rt_
  // CLI flag was passed at startup, and CpuRayTracer::build() always reads
  // draw_ directly (BuildHostScene has no GPU-resident fallback) -- so this
  // geometry must always survive now, superseding the narrower cudaRt_/
  // hipRt_-only check this function used before the switch feature existed.
  if (liveSwitchEnabled_) return true;
  if (cudaRt_ || hipRt_ || cpuRt_) return true;
  // rtPath_ is set as soon as --rt is requested and some RT technique is
  // available (checked at renderer init, before setRayTracing() has
  // necessarily run yet -- see the two rtPath_ = true; sites) -- so this is
  // valid to check as early as scene load, same as cudaRt_/hipRt_ above.
  return rtPath_ && renderer_ && renderer_->rayTracingAvailable() &&
         !renderer_->rayTracingIsHardware();
}

void App::updateSkinningEffective() {
  skinningEffective_ = SkinningMode::CPU;
  skinningReason_ = "CPU skinning selected";
  skinFrame_ = SkinningFrameCPU{};
  skinFrameTime_ = std::numeric_limits<double>::quiet_NaN();
  lastRtActiveForSkinning_ = renderer_ && renderer_->rayTracingActive();
  if (renderer_) renderer_->uploadSkinningFrame(skinFrame_);

  if (skinningRequested_ == SkinningMode::CPU) return;
  if (!renderer_ || !renderer_->caps().supportsGpuSkinning) {
    skinningReason_ = "GPU skinning unsupported by renderer";
    return;
  }
  const bool rtActive = renderer_->rayTracingActive();
  // --next has no RenderScene: what it can deform is recorded in the DrawScene
  // (a bone-matrix layout for skinning, morph channels for blendshapes).
  const bool skeletal = useNextLoader_
                            ? draw_.boneMatrixCount > 0
                            : SceneHasSkeletalSkinning(loaded_.render);
  const bool morph = useNextLoader_ ? DrawSceneHasMorphChannels(draw_)
                                    : SceneHasBlendShapes(loaded_.render);
  if (!loaded_.ok || (!skeletal && !morph)) {
    skinningReason_ = "scene has no skeletal skinning or blendshapes";
    return;
  }
  // The CUDA/HIP ray tracers read CPU-side draw_ geometry directly, so the raster
  // vertex shader's deform never reaches them: they would trace the rest pose.
  // Under the LEGACY loader the only way to pose them is the CPU bake, which the
  // reconvert path writes into draw_ before the tracer builds its BVH. The next
  // loader instead re-poses the retained rest vertices straight into draw_
  // (poseNextDrawForTracer), so it keeps the GPU deform data -- one deform
  // implementation for every backend, and no converter re-run per time code.
  if ((cudaRt_ || hipRt_) && !useNextLoader_) {
    skinningReason_ = "CPU skinning (legacy loader + CUDA/HIP tracer reads CPU geometry)";
    return;
  }
  // The next loader renormalizes onto the 4 strongest influences per vertex, so
  // it never needs the extended (texture) influence path.
  const int maxInfluences =
      useNextLoader_ ? 0 : MaxSkinInfluenceCount(loaded_.render);
  if (maxInfluences > 4 &&
      (!renderer_->caps().supportsExtendedGpuSkinning ||
       maxInfluences > kMaxGpuTextureInfluences)) {
    skinningReason_ =
        (maxInfluences > kMaxGpuTextureInfluences
             ? "CPU skinning fallback: GPU texture path supports up to " +
                   std::to_string(kMaxGpuTextureInfluences) +
                   " influences, scene has " + std::to_string(maxInfluences)
             : (skinningRequested_ == SkinningMode::GPU
                    ? "CPU skinning fallback: GPU path currently supports 4 influences, scene has " +
                          std::to_string(maxInfluences)
                    : "CPU skinning selected for high influence count (" +
                          std::to_string(maxInfluences) + " > 4)"));
    return;
  }
  if (skeletal && draw_.boneMatrixCount <= 0) {
    skinningReason_ = "draw scene has no GPU bone matrix layout";
    return;
  }
  skinningEffective_ = SkinningMode::GPU;
  if (rtActive) {
    skinningReason_ = skeletal && morph
                          ? "RT skeletal + blendshape skinning"
                          : morph ? "RT blendshape morph" : "RT skeletal skinning";
  } else {
    skinningReason_ = skeletal && morph ? "GPU skeletal + blendshape skinning"
                      : morph           ? "GPU blendshape morph"
                                        : "GPU skeletal skinning";
  }
}

void App::updateGpuSkinningFrameIfNeeded() {
  // --next has no RenderScene for Tydra to pose from; it re-poses from its
  // retained Stage instead (bone matrices + GPU morph coefficients).
  if (useNextLoader_) {
    if (nextStageSnapshot_ && loaded_.ok && !progressiveActive_ &&
        renderer_->meshCount() == static_cast<int>(draw_.meshes.size())) {
      if (UpdateNextAnimatedMeshWorlds(*nextStageSnapshot_, &draw_, animTime_)) {
        std::vector<std::pair<int, std::array<float, 16>>> worldUploads;
        for (size_t i = 0; i < draw_.meshes.size(); ++i) {
          if (!draw_.meshes[i].animatedWorld) continue;
          std::array<float, 16> world;
          std::memcpy(world.data(), draw_.meshes[i].world, sizeof(world));
          worldUploads.push_back({static_cast<int>(i), world});
        }
        postGpu([this, uploads = std::move(worldUploads)]() {
          for (const auto& upload : uploads)
            renderer_->updateMeshWorld(upload.first, upload.second.data());
        });
      }
    }
    updateNextDeformFrameIfNeeded();
    return;
  }
  if (skinningEffective_ != SkinningMode::GPU || !loaded_.ok) return;
  const bool hasMorph = SceneHasBlendShapes(loaded_.render);
  const bool mixed = SceneHasNonSkeletalAnimation(loaded_.render);
  // Per-mesh morph/world updates need the meshes present in the renderer; wait
  // for progressive streaming to finish (the bone texture alone is safe early).
  if (progressiveActive_ && (hasMorph || mixed)) return;
  // Manual blendshape weights (Maya-like editor) force a re-pose even at the same
  // time code, since the weights changed rather than the animation clock.
  const bool blendDirty = hasMorph && gui_.consumeBlendDirty();
  // Per-mesh vertex/world updates index the renderer by DrawScene mesh order;
  // that only holds when the renderer uploaded exactly these meshes. Guard it so
  // a future divergence degrades gracefully (rest pose) instead of posing the
  // wrong mesh.
  const bool idxOk =
      renderer_->meshCount() == static_cast<int>(draw_.meshes.size());
  if (!idxOk && !warnedMeshIndexMismatch_) {
    LOGW("GPU skinning: renderer mesh count (%d) != draw mesh count (%zu); "
         "skipping per-mesh morph/world updates.",
         renderer_->meshCount(), draw_.meshes.size());
    warnedMeshIndexMismatch_ = true;
  }

  // Node/xform animation alongside skinning: re-evaluate the animated mesh world
  // transforms (no geometry re-pack) and push them to the renderer. All renderer
  // uploads here go through postGpu(): on the threaded path the render thread
  // owns the GL context (the main thread has released it), so a direct GL call
  // from this main-thread function would hit no current context / race the
  // render thread. postGpu runs inline when single-threaded (no behavior change).
  // Xform samples under a SkelRoot are not represented as non-skeletal
  // AnimationClip channels. Re-evaluate worlds unconditionally so a skinned
  // mesh with an animated parent is not left at the load-time identity world.
  if (idxOk && UpdateAnimatedMeshWorlds(loaded_.stage, &draw_, animTime_)) {
    std::vector<std::pair<int, std::array<float, 16>>> worldUploads;
    worldUploads.reserve(draw_.meshes.size());
    for (size_t i = 0; i < draw_.meshes.size(); ++i) {
      std::array<float, 16> w;
      std::memcpy(w.data(), draw_.meshes[i].world, sizeof(w));
      worldUploads.push_back({static_cast<int>(i), w});
    }
    postGpu([this, ups = std::move(worldUploads)]() {
      for (const auto& u : ups) renderer_->updateMeshWorld(u.first, u.second.data());
    });
  }
  if (skinFrameTime_ == animTime_ && !blendDirty) return;  // already posed

  if (renderer_->rayTracingActive()) {
    if (!idxOk) return;
    // LUSDVIEW_RT_TIMING: the CPU side of the RT pose (morph + LBS + normal
    // regen + displacement bake), the counterpart of the [vk_rt] AS lines.
    static const bool kRtTiming = std::getenv("LUSDVIEW_RT_TIMING") != nullptr;
    const auto skinT0 = std::chrono::steady_clock::now();

    // OPT-IN (LUSDVIEW_RT_GPU_SKIN=1) GPU compute skinning for the eligible
    // class (pure <=4-influence skeletal, no morph/displacement): composed
    // matrices upload + in-place skin of the RT vertex stream + BLAS refit, no
    // CPU per-vertex work. Off by default for two measured reasons (200k-tri
    // probe, RTX 3070): (1) the RT vertex/joint/weight buffers are HOST-VISIBLE
    // (mapped for the CPU-fallback upload), so the compute pass streams over
    // PCIe and loses to the CPU path (~30 ms vs ~7 ms) — flipping the default
    // needs device-local RT streams + a staging upload path; (2) it skins
    // NORMALS with the weighted joint matrices (the raster deform.glsl
    // convention) while the CPU path regenerates smooth normals on the posed
    // surface, so shading differs on smooth meshes (~3% of pixels beyond a
    // 32/255 delta on the probe). Also: the skeleton overlay's dense
    // point-joint samples fall back to rest-pose display for GPU-skinned
    // meshes (no CPU-skinned helper points). Runs inline (not postGpu): the
    // dispatch's return value gates the CPU fallback, and the RT path never
    // runs on the threaded-GL build.
    static const bool kGpuSkinOptIn =
        std::getenv("LUSDVIEW_RT_GPU_SKIN") != nullptr;
    std::unordered_set<int> gpuHandled;
    if (kGpuSkinOptIn) {
      std::vector<RtGpuSkinUpdate> gpuUpdates;
      std::unordered_set<int> eligible;
      BuildRtGpuSkinUpdates(loaded_.render, &draw_, animTime_, &gpuUpdates,
                            &eligible);
      for (const RtGpuSkinUpdate& u : gpuUpdates) {
        // A refused dispatch (no RT stream / shader unavailable) simply stays
        // out of gpuHandled, so the CPU pass below picks the mesh up.
        if (renderer_->updateMeshSkinningGpu(u.meshIndex, u.mats.data(),
                                             u.jointCount, u.matrixBase,
                                             u.aabbMin, u.aabbMax)) {
          gpuHandled.insert(u.meshIndex);
        }
      }
      if (kRtTiming && !gpuHandled.empty()) {
        std::fprintf(stderr, "[rt-skin] gpu compute skin: %.2f ms (%zu meshes)\n",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - skinT0).count(),
                     gpuHandled.size());
      }
    }

    std::vector<RtSkinnedMeshUpload> uploads;
    if (BuildRtSkinnedMeshVertices(loaded_.stage, loaded_.render, &draw_,
                                   animTime_, gui_.blendOverrides(),
                                   gui_.showSkeletonOverlay(), &uploads,
                                   gpuHandled.empty() ? nullptr : &gpuHandled)) {
      if (kRtTiming) {
        size_t verts = 0;
        for (const RtSkinnedMeshUpload& u : uploads) verts += u.vertices.size();
        std::fprintf(stderr, "[rt-skin] cpu pose: %.2f ms (%zu meshes, %zu verts)\n",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - skinT0).count(),
                     uploads.size(), verts);
      }
      postGpu([this, ups = std::move(uploads)]() {
        const auto upT0 = std::chrono::steady_clock::now();
        for (const RtSkinnedMeshUpload& upload : ups) {
          renderer_->updateMeshVertices(upload.meshIndex, upload.vertices);
        }
        if (kRtTiming) {
          std::fprintf(stderr, "[rt-skin] vbo upload: %.2f ms\n",
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - upT0).count());
        }
      });
    }
    skinFrameTime_ = animTime_;
    return;
  }

  // Bone matrices for GPU skinning. Blendshapes are applied in the vertex shader
  // (GPU morph, via updateMorphWeights below), not by CPU-morphing the VBO.
  if (BuildGpuSkinningFrame(loaded_.render, &draw_, animTime_, &skinFrame_,
                            gui_.showSkeletonOverlay(), &loaded_.stage,
                            gui_.blendOverrides())) {
    postGpu([this, sf = skinFrame_]() { renderer_->uploadSkinningFrame(sf); });
  }
  // GPU blendshape morph: upload only the tiny per-channel coefficient array per
  // morphed mesh (the vertex shader sums coeff*delta). No VBO re-upload, no GPU
  // stall. Meshes whose weights fall to 0 get a zero coefficient array (no morph).
  if (hasMorph && idxOk) {
    std::vector<std::pair<int, std::vector<float>>> coeffs;
    BuildMorphChannelWeights(loaded_.stage, draw_, animTime_,
                             gui_.blendOverrides(), &coeffs);
    postGpu([this, mc = std::move(coeffs)]() {
      for (const auto& c : mc) renderer_->updateMorphWeights(c.first, c.second);
    });
  }
  skinFrameTime_ = animTime_;
}

void App::updateNextDeformFrameIfNeeded() {
  if (!useNextLoader_ || !nextStageSnapshot_ || !loaded_.ok) return;
  if (skinningEffective_ != SkinningMode::GPU) return;
  const bool hasSkin = draw_.boneMatrixCount > 0;
  if (!hasNextMorph_ && !hasSkin) return;
  // Manual blendshape weights (editor) re-pose even at the same time code.
  const bool blendDirty = gui_.consumeBlendDirty();
  if (skinFrameTime_ == animTime_ && !blendDirty) return;  // already posed
  // Per-mesh coefficient upload indexes the renderer by draw-mesh order; only
  // valid once the renderer holds exactly these meshes (post-streaming). Mark
  // "posed" only when we actually uploaded, so a too-early call (meshes not yet
  // streamed on the threaded path) re-tries next frame instead of latching.
  // (The bone texture is scene-wide, so it is safe to upload before then -- but
  // keep both on one clock so a partially-streamed frame is never half-posed.)
  if (renderer_->meshCount() != static_cast<int>(draw_.meshes.size())) return;

  // The load-time scene box is the REST box (the loader world-bakes and uploads
  // rest vertices; the pose only happens downstream), so refresh it for the pose
  // at this time code -- the ground grid is sized from it, `--mode depth` is
  // normalized by it, and the initial auto-fit frames on it. Per-MESH boxes are
  // left alone on purpose: a skinned batch keeps the conservative scene box so a
  // moving rig cannot cull or LOD itself out of the frame.
  {
    float bmin[3], bmax[3];
    if (BuildNextPosedSceneBounds(*nextStageSnapshot_, &draw_, animTime_,
                                  gui_.blendOverrides(), bmin, bmax)) {
      for (int k = 0; k < 3; ++k) {
        draw_.aabbMin[k] = bmin[k];
        draw_.aabbMax[k] = bmax[k];
      }
      draw_.hasBounds = true;
    }
  }

  // Ray tracing traces the vertex buffers themselves, so the raster shader's
  // deform never reaches it: re-pose the retained rest vertices on the CPU and
  // hand them to the renderer, which refills the RT vertex buffer and rebuilds
  // that mesh's BLAS. The alternative -- what this path did before -- was to
  // re-run the whole converter at every time code.
  if (renderer_->rayTracingActive()) {
    static const bool kRtTiming = std::getenv("LUSDVIEW_RT_TIMING") != nullptr;
    const auto skinT0 = std::chrono::steady_clock::now();
    std::vector<RtSkinnedMeshUpload> uploads;
    if (BuildNextRtDeformedVertices(*nextStageSnapshot_, draw_, animTime_,
                                    gui_.blendOverrides(), &uploads)) {
      if (kRtTiming) {
        size_t verts = 0;
        for (const RtSkinnedMeshUpload& u : uploads) verts += u.vertices.size();
        std::fprintf(stderr, "[rt-skin] cpu pose: %.2f ms (%zu meshes, %zu verts)\n",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - skinT0).count(),
                     uploads.size(), verts);
      }
      const auto upT0 = std::chrono::steady_clock::now();
      for (const RtSkinnedMeshUpload& up : uploads) {
        renderer_->updateMeshVertices(up.meshIndex, up.vertices);
      }
      if (kRtTiming) {
        std::fprintf(stderr, "[rt-skin] vbo upload: %.2f ms\n",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - upT0).count());
      }
    }
    skinFrameTime_ = animTime_;
    return;
  }

  // Renderer uploads go through postGpu() so they run on the render thread when
  // it owns the context (threaded path); inline otherwise. See the note in
  // updateGpuSkinningFrameIfNeeded.
  if (hasSkin && BuildNextSkinningFrame(*nextStageSnapshot_, &draw_,
                                        animTime_, &skinFrame_)) {
    postGpu([this, sf = skinFrame_]() { renderer_->uploadSkinningFrame(sf); });
  }
  if (hasNextMorph_) {
    std::vector<std::pair<int, std::vector<float>>> coeffs;
    BuildNextMorphWeights(*nextStageSnapshot_, draw_, animTime_,
                          gui_.blendOverrides(), &coeffs);
    postGpu([this, mc = std::move(coeffs)]() {
      for (const auto& c : mc) renderer_->updateMorphWeights(c.first, c.second);
    });
  }
  skinFrameTime_ = animTime_;
}

bool App::sceneIsNextDeformable() const {
  return useNextLoader_ && nextStageSnapshot_ && loaded_.ok &&
         (hasNextMorph_ || draw_.boneMatrixCount > 0);
}

// Write the pose at `time` into draw_ geometry, for the CUDA/HIP tracers -- which
// build their BVH from draw_ meshes rather than from renderer-owned vertex
// buffers, and so cannot be fed the way Vulkan RT is (updateMeshVertices).
//
// The rest pose is snapshotted on first use and restored before every re-pose, so
// this is idempotent and can run at any time code in any order. Only deformable
// meshes are copied. Returns true when draw_ now holds the pose at `time`.
bool App::ensureRtTexturePayloads() {
  // The CUDA/HIP/CPU tracers build their texture table from draw_.textures'
  // CPU payloads (BuildHostTextureTable), but the RASTER residency streamer
  // deliberately drops those payloads: updateTextureResidency() releases each
  // one right after the GPU upload succeeds, and again whenever it evicts a
  // slot, keeping only the source identity so the pixels can be re-decoded on
  // demand ("eviction/full-res promotion re-decodes the ordinary file instead
  // of pinning a duplicate CPU block"). So any RT build that happens AFTER the
  // viewport has rasterized a few frames -- i.e. every interactive switch to
  // CUDA/HIP/CPU RT, as opposed to starting up with --cuda/--hip/--cpu-rt --
  // found the payloads gone and traced an untextured scene.
  //
  // Re-decode them here, on the main thread, before the tracer reads draw_.
  // This is the same on-demand re-decode the residency streamer itself uses,
  // so it costs a decode (not a pinned duplicate) and keeps the VRAM budget
  // machinery untouched. Vulkan hardware ray query is unaffected: it samples
  // the already-uploaded GPU textures rather than draw_.
  // Raster residency intentionally releases ordinary texture payloads after
  // upload. Re-decoding every Island texture at authored resolution here used
  // over 60 GiB of transient host RAM before RT geometry even started. RT is
  // already a bounded preview path for large scenes, so reconstruct a compact
  // texture set when the scene has hundreds of deferred textures. Ptex keeps
  // its native atlas/source path and is not affected by this fallback cap.
  const bool boundedRtTextures = draw_.textures.size() > 512;
  const int rtTextureEdge = boundedRtTextures ? 512 : 0;
  const size_t rtTextureBytes = boundedRtTextures
                                    ? size_t{2ull * 1024ull * 1024ull}
                                    : 0;
  if (boundedRtTextures) {
    LOGI("RT texture fallback: %zu deferred slots, edge <= %d, per-texture "
         "decode budget %.1f MiB",
         draw_.textures.size(), rtTextureEdge,
         double(rtTextureBytes) / (1024.0 * 1024.0));
  }
  bool decodedAny = false;
  size_t decodedCount = 0;
  uint64_t decodedBytes = 0;
  for (DrawTextureCPU& tex : draw_.textures) {
    if (!tex.deferredDecode && !tex.image.data.empty()) continue;
    if (tex.isUdim || tex.isPtex) continue;  // never released above
    DrawTextureCPU placeholder = tex;
    placeholder.deferredDecode = true;
    DrawTextureCPU decoded;
    // Full resolution (budget 0): the tracer has no LOD/streaming of its own,
    // so a coarse mip here would be the permanent quality of the RT image.
    TextureRuntimeOptions runtime = loadOpts_.textureOptions;
    runtime.maxTextureSize = rtTextureEdge;
    if (DecodeDeferredDrawTexture(placeholder, runtime, rtTextureBytes,
                                  &decoded)) {
      decodedBytes += decoded.image.data.size();
      decodedBytes += decoded.compressed.data.size();
      ++decodedCount;
      tex = std::move(decoded);
      decodedAny = true;
    }
  }
  if (boundedRtTextures)
    LOGI("RT texture fallback complete: decoded %zu slot(s), %.1f MiB payload",
         decodedCount, double(decodedBytes) / (1024.0 * 1024.0));
  return decodedAny;
}

void App::restoreNextDrawRestPose() {
  // poseNextDrawForTracer writes POSED vertices into draw_ and leaves them
  // there, so draw_ stops holding the rest pose the moment any CUDA/HIP/CPU RT
  // build runs. Everything else that re-poses from draw_ (the selection
  // wireframe via BuildNextRtDeformedVertices, bounds, picking) needs the REST
  // pose as input, and the raster path GPU-skins from rest too -- so once no
  // tracer owns draw_ any more, put it back. Without this the wireframe froze
  // at whatever time code the last RT frame had posed: click -> RT -> raster ->
  // playback left the highlight stuck on the old pose while the shaded mesh
  // animated.
  if (nextRestVerts_.empty()) return;
  for (const auto& kv : nextRestVerts_) {
    const size_t i = static_cast<size_t>(kv.first);
    if (i < draw_.meshes.size()) draw_.meshes[i].vertices = kv.second;
  }
  nextTracerPosedTime_ = std::numeric_limits<double>::quiet_NaN();
}

bool App::poseNextDrawForTracer(double time) {
  if (!sceneIsNextDeformable()) return false;
  if (nextRestVerts_.empty()) {
    for (size_t i = 0; i < draw_.meshes.size(); ++i) {
      const DrawMeshCPU& m = draw_.meshes[i];
      if (m.vertices.empty()) continue;
      if (m.jointIdx.empty() && m.morphDeltaHalf.empty()) continue;
      nextRestVerts_[static_cast<int>(i)] = m.vertices;
    }
    if (nextRestVerts_.empty()) return false;
  }
  // BuildNextRtDeformedVertices deforms whatever is in draw_, so it must see the
  // REST pose -- not the pose we left behind at the previous time code.
  for (const auto& kv : nextRestVerts_) {
    const size_t i = static_cast<size_t>(kv.first);
    if (i < draw_.meshes.size()) draw_.meshes[i].vertices = kv.second;
  }
  std::vector<RtSkinnedMeshUpload> uploads;
  if (!BuildNextRtDeformedVertices(*nextStageSnapshot_, draw_, time,
                                   gui_.blendOverrides(), &uploads)) {
    return false;
  }
  for (RtSkinnedMeshUpload& up : uploads) {
    if (up.meshIndex < 0 ||
        static_cast<size_t>(up.meshIndex) >= draw_.meshes.size()) {
      continue;
    }
    draw_.meshes[static_cast<size_t>(up.meshIndex)].vertices =
        std::move(up.vertices);
  }
  nextTracerPosedTime_ = time;
  return true;
}

void App::maybeReconvertForManualBlend() {
  // The GPU path repools live in updateGpuSkinningFrameIfNeeded(); only the
  // ray-traced / CPU-skinned path needs a geometry+BLAS rebuild here.
  if (skinningEffective_ == SkinningMode::GPU) return;
  if (!loaded_.ok || !SceneHasBlendShapes(loaded_.render)) return;
  if (gui_.consumeBlendDirty()) blendReconvNeeded_ = true;
  if (blendReconvNeeded_ && !reconvActive_ && !loadActive_ && !progressiveActive_) {
    blendReconvNeeded_ = false;
    requestReconvert(animTime_);  // starts immediately (no reconvert in flight)
  }
}

void App::advancePlayback(float dtSec) {
  if (!hasAnimation_ || !animPlaying_) return;
  const double span = animEnd_ - animStart_;
  animTime_ += static_cast<double>(dtSec) * animFps_ *
               static_cast<double>(animSpeed_);
  if (animTime_ > animEnd_) {
    if (animLoop_ && span > 0.0) {
      animTime_ = animStart_ + std::fmod(animTime_ - animStart_, span);
    } else {
      animTime_ = animEnd_;
      animPlaying_ = false;
    }
  } else if (animTime_ < animStart_) {
    animTime_ = animStart_;
  }
  requestReconvert(animTime_);
}

void App::requestReconvert(double t) {
  // Skip while a fresh file load is streaming (loaded_/draw_ are in flux); the
  // worker would also race the load writing loaded_.
  if (!loaded_.ok || loadActive_) return;

  // The next loader keeps the render scene resident and applies animation in
  // place (world-transform uploads and, when supported, GPU deformation).
  // Sending its timeline through RenderSceneAtTime is incorrect: that worker
  // consumes the legacy RenderScene representation, which is intentionally
  // empty for a next-loader load.  Apart from doing needless work, swapping
  // that failed result during playback makes the viewport report that there
  // are no renderable meshes.
  if (useNextLoader_) {
    reconvApplied_ = t;
    skinFrameTime_ = std::numeric_limits<double>::quiet_NaN();
    return;
  }

  if (skinningEffective_ == SkinningMode::GPU) {
    reconvApplied_ = t;
    skinFrameTime_ = std::numeric_limits<double>::quiet_NaN();
    return;
  }
  reconvRequested_ = t;
  reconvHasRequest_ = true;
  if (!reconvActive_) startReconvertAsync(t);
}

void App::startReconvertAsync(double t) {
  reconvActive_ = true;
  reconvInFlight_ = t;
  reconvHasRequest_ = false;
  reconvFinished_.store(false);
  reconvOk_.store(false);
  reconvCtrl_.cancel.store(false);
  reconvCtrl_.resetProgress();
  reconvDraw_ = std::make_unique<DrawScene>();
  DrawScene* dp = reconvDraw_.get();
  const bool rt = rtPath_;
  // Snapshot the manual blendshape weights on the main thread (gui_ is only
  // touched here); the worker bakes them into the deformed/BLAS geometry.
  std::unordered_map<std::string, float> ovr;
  if (const auto* o = gui_.blendOverrides()) ovr = *o;
  // Worker reads loaded_ (stage/mmap/filepath) read-only; the main thread keeps
  // loaded_ alive and joins this worker (cancelAndJoinReconvert) before any
  // reload. RenderSceneAtTime skips texture decode and fills only dp->meshes.
  // The rest cache only pays off for repeated same-timecode reconverts (dragging
  // a blendshape weight while paused). During playback the timecode changes every
  // frame, so it would never hit and the per-frame copy-into-cache would be pure
  // overhead -- skip it then.
  RestSceneCache* cache = animPlaying_ ? nullptr : &reconvRestCache_;
  reconvThread_ = std::thread([this, t, dp, rt, cache, ovrMoved = std::move(ovr)]() {
    std::string w, e;
    const bool ok = RenderSceneAtTime(loaded_, t, rt, dp, &w, &e, &reconvCtrl_,
                                      ovrMoved.empty() ? nullptr : &ovrMoved, cache);
    reconvOk_.store(ok, std::memory_order_relaxed);
    reconvFinished_.store(true, std::memory_order_release);
  });
}

void App::finishReconvertIfReady() {
  if (!reconvActive_) return;
  if (!reconvFinished_.load(std::memory_order_acquire)) return;
  if (reconvThread_.joinable()) reconvThread_.join();
  reconvActive_ = false;

  if (reconvOk_.load(std::memory_order_relaxed) && !reconvCtrl_.cancel.load() &&
      reconvDraw_) {
    // Swap in the re-evaluated geometry while keeping the initial load's
    // materials/textures (they don't animate). draw_ is mutated in place so the
    // GUI's pointer (and selection/visibility state) stays valid -- do NOT call
    // setScene here. The cull worker may be mid-iteration over the outgoing
    // meshes (playback does not suspend culling); join + invalidate it first.
    gui_.prepareSceneSwap();
    draw_.meshes = std::move(reconvDraw_->meshes);
    draw_.triangleCount = reconvDraw_->triangleCount;
    draw_.truncated = reconvDraw_->truncated;
    reconvApplied_ = reconvInFlight_;
    std::string uerr;
    if (!renderer_->uploadScene(draw_, &uerr)) {
      LOGE("reconverted scene GPU upload failed: %s",
           uerr.empty() ? "unknown error" : uerr.c_str());
    }  // camera untouched (no refit)
  }
  reconvDraw_.reset();

  // Coalesce: if playback advanced past the time we just computed, start the
  // next re-evaluation toward the latest requested time.
  if (reconvHasRequest_ && reconvRequested_ != reconvApplied_ && !loadActive_) {
    startReconvertAsync(reconvRequested_);
  }
}

void App::cancelAndJoinReconvert() {
  if (reconvThread_.joinable()) {
    reconvCtrl_.cancel.store(true);
    reconvThread_.join();
  }
  reconvActive_ = false;
  reconvFinished_.store(false);
  reconvHasRequest_ = false;
  reconvDraw_.reset();
  // The cached rest scene belongs to the (possibly outgoing) scene; drop it now
  // that the worker is joined. Called on reload + skinning-mode switch, not during
  // interactive blendshape reconverts (those queue), so this doesn't defeat it.
  reconvRestCache_ = RestSceneCache{};
}

void App::openFileDialog() {
#if defined(HAVE_NFD)
  NFD_Init();
  nfdu8char_t* outPath = nullptr;
  nfdu8filteritem_t filters[1] = {{"USD", "usd,usda,usdc,usdz"}};
  nfdresult_t r = NFD_OpenDialogU8(&outPath, filters, 1, nullptr);
  if (r == NFD_OKAY && outPath) {
    std::string p = outPath;
    NFD_FreePathU8(outPath);
    startLoadAsync(p);
  }
  NFD_Quit();
#else
  LOGW("File dialog not available in this build. Pass a USD file on the command line.");
#endif
}

void App::openDomeFileDialog() {
#if defined(HAVE_NFD)
  NFD_Init();
  nfdu8char_t* outPath = nullptr;
  nfdu8filteritem_t filters[1] = {
      {"Environment image", "hdr,exr,png,jpg,jpeg,bmp,tif,tiff"}};
  const nfdresult_t result = NFD_OpenDialogU8(&outPath, filters, 1, nullptr);
  if (result == NFD_OKAY && outPath) {
    gui_.setDomeFilePath(outPath);
    NFD_FreePathU8(outPath);
  }
  NFD_Quit();
#else
  gui_.setDomeStatus(
      "File dialog unavailable in this build; drop an image on the window.");
#endif
}

void App::applyDomeLightRequest(const Gui::DomeLightRequest& request) {
  if (request.browse) {
    openDomeFileDialog();
    return;
  }
  std::vector<DrawLightCPU> lights;
  for (const DrawLightCPU& light : authoredLights_)
    if (light.type != DrawLightCPU::Type::Dome) lights.push_back(light);

  DrawLightCPU selected;
  bool haveDome = false;
  std::string status;
  if (request.source == Gui::DomeSource::Authored) {
    int ordinal = 0;
    for (const DrawLightCPU& light : authoredLights_) {
      if (light.type != DrawLightCPU::Type::Dome) continue;
      if (ordinal++ == request.authoredIndex) {
        selected = light;
        haveDome = true;
        if (request.format != light.domeTextureFormat &&
            !light.textureFile.empty()) {
          std::filesystem::path texturePath(light.textureFile);
          if (texturePath.is_relative() && !loaded_.filepath.empty()) {
            texturePath = std::filesystem::path(loaded_.filepath).parent_path() /
                          texturePath;
          }
          DrawLightCPU remapped;
          std::string remapError;
          if (BuildDomeLightFromFile(texturePath.string(), request.format,
                                    loadOpts_.textureOptions.domeIbl >= 2,
                                    &remapped, &remapError)) {
            selected = light;
            selected.ibl = std::move(remapped.ibl);
            selected.domeTextureFormat = request.format;
          } else {
            status = "Coordinate remap unavailable: " + remapError;
          }
        }
        break;
      }
    }
    if (!haveDome) status = "The selected USD DomeLight is unavailable.";
  } else if (request.source != Gui::DomeSource::Off) {
    std::string key;
    if (request.source == Gui::DomeSource::File)
      key = "file:" + request.filePath + ":" +
            std::to_string(static_cast<int>(request.format));
    else if (request.source == Gui::DomeSource::WhiteFurnace)
      key = "white";
    else
      key = "sunsky";
    if (!runtimeDomeValid_ || key != runtimeDomeKey_) {
      std::string error;
      bool ok = false;
      if (request.source == Gui::DomeSource::File && !request.filePath.empty())
        ok = BuildDomeLightFromFile(request.filePath, request.format,
                                    loadOpts_.textureOptions.domeIbl >= 2,
                                    &runtimeDomeBase_, &error);
      else if (request.source == Gui::DomeSource::WhiteFurnace)
        ok = BuildWhiteFurnaceDome(loadOpts_.textureOptions.domeIbl >= 2,
                                   &runtimeDomeBase_, &error);
      else if (request.source == Gui::DomeSource::SunSky)
        ok = BuildSunSkyDome(loadOpts_.textureOptions.domeIbl >= 2,
                             &runtimeDomeBase_, &error);
      else
        error = "Choose or drop an environment-map image.";
      runtimeDomeValid_ = ok;
      runtimeDomeKey_ = ok ? key : std::string();
      if (ok && request.source == Gui::DomeSource::File) {
        LOGI("runtime dome IBL ready: '%s' (%s quality)",
             request.filePath.c_str(),
             loadOpts_.textureOptions.domeIbl >= 2 ? "high" : "low");
      }
      if (!ok) status = error;
    }
    if (runtimeDomeValid_) {
      selected = runtimeDomeBase_;
      haveDome = true;
    }
  }
  if (haveDome) {
    DrawLightCPU controlled;
    ApplyDomeLightControls(request.intensity, request.rotationDegrees, selected,
                           &controlled);
    lights.push_back(std::move(controlled));
    const std::string active = "Active: " + lights.back().displayName;
    status = status.empty() ? active : active + ". " + status;
  } else if (request.source == Gui::DomeSource::Off) {
    status = "Environment lighting disabled.";
  }
  draw_.lights = std::move(lights);
  std::vector<DrawLightCPU> gpuLights = draw_.lights;
  const size_t meshCount = draw_.meshes.size();
  postGpu([this, lights = std::move(gpuLights), meshCount]() {
    renderer_->setLights(lights, meshCount);
  });
  gui_.setDomeStatus(status);
}

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
// --- Experimental threaded GL rendering: the render thread owns the GL context ---

static std::uint64_t FrameSceneKey(const FramePacket& p) {
  std::uint64_t h = 1469598103934665603ull;
  auto bytes = [&](const void* data, size_t size) {
    const auto* b = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
      h ^= static_cast<std::uint64_t>(b[i]);
      h *= 1099511628211ull;
    }
  };
  bytes(p.view, sizeof(p.view));
  bytes(p.proj, sizeof(p.proj));
  bytes(p.cameraPos, sizeof(p.cameraPos));
  bytes(&p.exposure, sizeof(p.exposure));
  bytes(&p.mode, sizeof(p.mode));
  bytes(p.clearColor, sizeof(p.clearColor));
  bytes(p.lightDir, sizeof(p.lightDir));
  bytes(p.lightColor, sizeof(p.lightColor));
  bytes(&p.depthScale, sizeof(p.depthScale));
  bytes(p.sceneMin, sizeof(p.sceneMin));
  bytes(p.sceneExtent, sizeof(p.sceneExtent));
  bytes(&p.curveMaxSegments, sizeof(p.curveMaxSegments));
  bytes(&p.viewportW, sizeof(p.viewportW));
  bytes(&p.viewportH, sizeof(p.viewportH));
  bytes(&p.purposeVisibleMask, sizeof(p.purposeVisibleMask));
  bytes(&p.highlightXray, sizeof(p.highlightXray));
  if (!p.meshVisible.empty()) bytes(p.meshVisible.data(), p.meshVisible.size());
  if (!p.carrierVisible.empty())
    bytes(p.carrierVisible.data(), p.carrierVisible.size());
  if (!p.highlightIndices.empty())
    bytes(p.highlightIndices.data(), p.highlightIndices.size() * sizeof(uint32_t));
  if (!p.highlightLines.empty())
    bytes(p.highlightLines.data(), p.highlightLines.size() * sizeof(HelperVertex));
  if (!p.helperLines.empty())
    bytes(p.helperLines.data(), p.helperLines.size() * sizeof(HelperVertex));
  if (!p.overlayLines.empty())
    bytes(p.overlayLines.data(), p.overlayLines.size() * sizeof(HelperVertex));
  return h;
}

void App::postGpu(std::function<void()> op) {
  if (!renderThreadActive_) { op(); return; }  // inline on the single-threaded path
  {
    std::lock_guard<std::mutex> lk(gpuOpMutex_);
    gpuOps_.push(std::move(op));
    gpuOpsPending_.store(true, std::memory_order_release);
  }
  // GPU operations are independent of frame packets. Wake the render thread
  // immediately so shader swaps and streaming uploads do not inherit the next
  // packet's latency (or wait behind a temporarily idle UI producer).
  pktCv_.notify_one();
}

void App::drainGpuOps() {
  for (;;) {
    std::function<void()> op;
    {
      std::lock_guard<std::mutex> lk(gpuOpMutex_);
      if (gpuOps_.empty()) {
        gpuOpsPending_.store(false, std::memory_order_release);
        break;
      }
      op = std::move(gpuOps_.front());
      gpuOps_.pop();
    }
    op();
  }
}

bool App::startRenderThread() {
  renderRunning_.store(true);
  renderInitDone_.store(false);
  renderInitOk_.store(false);
  renderThread_ = std::thread(&App::renderThreadMain, this);
  while (!renderInitDone_.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!renderInitOk_.load()) { joinRenderThread(); return false; }
  return true;
}

void App::joinRenderThread() {
  renderRunning_.store(false);
  pktCv_.notify_all();
  pktDoneCv_.notify_all();
  if (renderThread_.joinable()) renderThread_.join();
}

void App::submitFramePacket(std::unique_ptr<FramePacket> pkt, bool blockUntilDone) {
  const std::uint64_t seq = pkt->seq;
  {
    std::lock_guard<std::mutex> lk(pktMutex_);
    if (pendingPacket_ && pendingPacket_->drawData)
      FreeImDrawData(pendingPacket_->drawData);  // latest-wins: drop the stale frame
    pendingPacket_ = std::move(pkt);
  }
  pktCv_.notify_one();
  if (blockUntilDone) {  // capture/deterministic mode
    std::unique_lock<std::mutex> lk(pktMutex_);
    pktDoneCv_.wait(lk, [&] {
      return pktRenderedSeq_ >= seq || !renderRunning_.load();
    });
  }
}

void App::renderThreadMain() {
  // GL: acquire the context (released by the main thread) so all GL objects
  // (FBO/shaders + ImGui GL backend) are created on this thread's current context.
  // Vulkan has no per-thread "current context" — the only rule is that one thread
  // owns the queue submits, which this thread does — so it skips the GLFW calls.
  const bool glBackend = (backend_ == Backend::GL);
  if (glBackend) glfwMakeContextCurrent(window_);
  std::string err;
  bool ok = renderer_->init(window_, &err);
  if (!ok) LOGE("render thread: renderer init: %s", err.c_str());
  else if (!(ok = renderer_->initImGuiBackend(&err)))
    LOGE("render thread: ImGui GL backend init: %s", err.c_str());
  renderInitOk_.store(ok);
  renderInitDone_.store(true, std::memory_order_release);
  if (!ok) { if (glBackend) glfwMakeContextCurrent(nullptr); return; }

  std::uint64_t renderedSceneKey = 0;
  while (renderRunning_.load()) {
    drainGpuOps();  // uploads/resize/instance-visibility, FIFO, before the frame
    std::unique_ptr<FramePacket> pkt;
    {
      std::unique_lock<std::mutex> lk(pktMutex_);
      pktCv_.wait_for(lk, std::chrono::milliseconds(4), [&] {
        return pendingPacket_ != nullptr ||
               gpuOpsPending_.load(std::memory_order_acquire) ||
               !renderRunning_.load();
      });
      if (pendingPacket_) pkt = std::move(pendingPacket_);
    }
    if (!pkt) continue;
    renderer_->newFrame();  // ImGui backend NewFrame (GL: ImGui_ImplOpenGL3, VK: ImGui_ImplVulkan)
    const bool renderedScene = pkt->hasParams && pkt->sceneKey != renderedSceneKey;
    const auto renderStart = std::chrono::steady_clock::now();
    if (renderedScene) {
      RenderFrameParams params = pkt->params();
      renderer_->renderFrame(params);
      renderedSceneKey = pkt->sceneKey;
    }
    if (pkt->wantCapture)  // read the 3D offscreen target (GL FBO / VK offscreen img)
      renderer_->captureViewport(&renderCapture_, &renderCaptureW_, &renderCaptureH_);
    renderer_->presentThreaded(pkt->drawData, pkt->fbW, pkt->fbH);
    if (renderedScene) {
      const double seconds = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - renderStart).count();
      if (seconds > 0.0)
        measuredRenderFps_.store(static_cast<float>(1.0 / seconds),
                                 std::memory_order_relaxed);
    }
    FreeImDrawData(pkt->drawData);
    pkt->drawData = nullptr;
    {
      std::lock_guard<std::mutex> lk(pktMutex_);
      pktRenderedSeq_ = pkt->seq;
    }
    pktDoneCv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lk(pktMutex_);
    if (pendingPacket_ && pendingPacket_->drawData)
      FreeImDrawData(pendingPacket_->drawData);
    pendingPacket_.reset();
  }
  renderer_->shutdown();
  if (glBackend) glfwMakeContextCurrent(nullptr);
}
#endif  // LUSDVIEW_ENABLE_GL_THREAD

bool App::createAndInitRenderer(Backend backend, std::string* err) {
  if (backend == Backend::GL) {
    renderer_ = CreateGLRenderer();
  }
#if defined(HAVE_VULKAN)
  else {
    renderer_ = CreateVulkanRenderer();
  }
#endif
  if (!renderer_) {
    if (err) *err = "no renderer for requested backend";
    return false;
  }
  ++rendererGeneration_;
  renderer_->setDevicePreference(devicePreference_);
  renderer_->setTransparencyMode(transparencyMode_);
  renderer_->setRasterQualityHigh(rasterQualityHigh_);
  renderer_->setMaterialXVulkanShaderLimits(
      materialXVulkanShaderMaxSourceBytes_,
      materialXVulkanCompileTimeoutSec_);
  const size_t rtTextureBudget =
      loadOpts_.textureOptions.textureBudgetMB > 0
          ? static_cast<size_t>(loadOpts_.textureOptions.textureBudgetMB) *
                1024ull * 1024ull
          : (loadOpts_.textureGpuBudgetBytes > 0
                 ? loadOpts_.textureGpuBudgetBytes / 4u
                 : 0u);
  renderer_->setRtTextureBudgetBytes(rtTextureBudget);
  // draw_ is a stable App member (same address for the app's lifetime); the
  // Vulkan compute-BVH RT fallback reads it lazily via BuildHostScene() when
  // it needs to rebuild its software BVH, same data the CUDA/HIP tracers'
  // build(draw_, ...) calls already use elsewhere in this file.
  renderer_->setHostSceneSource(&draw_);
  if (headless_) renderer_->setHeadlessSize(headlessWinW_, headlessWinH_);

  std::string initErr;
  if (renderer_->init(headless_ ? nullptr : window_, &initErr)) {
    backend_ = backend;
    return true;
  }

  if (backend == Backend::Vulkan && allowBackendFallback_ && !headless_) {
    LOGW("Vulkan renderer init failed: %s; falling back to OpenGL.", initErr.c_str());
    renderer_->shutdown();
    renderer_.reset();
    renderer_ = CreateGLRenderer();
    std::string glErr;
    if (!renderer_ || !renderer_->init(window_, &glErr)) {
      if (err) *err = glErr;
      renderer_.reset();
      return false;
    }
    backend_ = Backend::GL;
    return true;
  }

  if (err) *err = initErr;
  renderer_.reset();
  return false;
}

bool App::applyTechniqueSwitch(RenderTechnique t) {
  if (t == activeTechnique_) return true;

  Backend newOwner = backend_;
  const bool ownerMandated = TechniqueRequiresOwner(t, &newOwner);
  const Backend targetOwner = ownerMandated ? newOwner : backend_;
  const OverlayKind targetOverlay = OverlayForTechnique(t);
  const bool targetVulkanRt = targetOverlay == OverlayKind::VulkanRT ||
                              targetOverlay == OverlayKind::VulkanPathTrace;
  const bool targetCudaRt = targetOverlay == OverlayKind::CudaRT ||
                            targetOverlay == OverlayKind::CudaPathTrace;
  const bool targetHipRt = targetOverlay == OverlayKind::HipRT ||
                           targetOverlay == OverlayKind::HipPathTrace;
  const bool targetPath = targetOverlay == OverlayKind::VulkanPathTrace ||
                          targetOverlay == OverlayKind::CudaPathTrace ||
                          targetOverlay == OverlayKind::HipPathTrace;

  if (targetOwner == backend_) {
    // Same window owner: only the overlay changes. Exactly one of
    // {VulkanRT, CudaRT, HipRT, CpuRT} (or none) is live at a time, so
    // entering one first turns off whichever was active.
    if (targetVulkanRt) {
      if (!renderer_ || !renderer_->rayTracingAvailable()) {
        LOGW("Vulkan ray tracing is unavailable; staying on %s.",
             RenderTechniqueLabel(activeTechnique_));
        return false;
      }
    } else if (targetCudaRt && cudaProbe_ == ProbeState::Unavailable) {
      LOGW("CUDA ray tracing is unavailable on this machine; staying on %s.",
           RenderTechniqueLabel(activeTechnique_));
      return false;
    } else if (targetHipRt && hipProbe_ == ProbeState::Unavailable) {
      LOGW("HIP ray tracing is unavailable on this machine; staying on %s.",
           RenderTechniqueLabel(activeTechnique_));
      return false;
    }
    if ((activeOverlay_ == OverlayKind::VulkanRT ||
         activeOverlay_ == OverlayKind::VulkanPathTrace) && !targetVulkanRt) {
      if (renderer_) renderer_->setRayTracing(false);
    }
    hipInteractive_ = targetHipRt;
    cudaInteractive_ = targetCudaRt;
    cpuInteractive_ = (targetOverlay == OverlayKind::CpuRT);
    // Leaving the tracers that pose draw_ in place: hand draw_ back to
    // everyone else in its rest pose (see restoreNextDrawRestPose).
    if (!hipInteractive_ && !cudaInteractive_ && !cpuInteractive_) {
      restoreNextDrawRestPose();
    }
    pathTrace_.enabled = targetPath;
    gui_.setPathTraceSettings(pathTrace_);
    if (targetVulkanRt) renderer_->setRayTracing(true);
    activeOverlay_ = targetOverlay;
    previousTechnique_ = activeTechnique_;
    activeTechnique_ = t;
    gui_.setRenderer(renderer_.get());
    return true;
  }

  // Window-owner change (GL <-> Vulkan): full teardown + rebuild + reupload.
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  if (renderThreadActive_) {
    LOGW("Live backend switching is not supported together with --threaded; ignoring request.");
    return false;
  }
#endif
  if (!renderer_) return false;

  const Backend prevOwner = backend_;
  const RenderTechnique prevTechnique = activeTechnique_;

  // A GLFW window is created for ONE client API: GL needs GLFW_OPENGL_API, and
  // the Vulkan path asks for GLFW_NO_API (no GL context exists at all). So the
  // window cannot simply be handed to the other backend -- GLRenderer::init()
  // on a NO_API window found every GL entry point null (glad had nothing to
  // load) and jumped through a null pointer on the first call. Recreate the
  // window whenever the required client API differs.
  //
  // The ImGui *context* belongs to App (App::initImGui creates it once, with
  // the fonts/style/ini); each renderer owns only the two ImGui BACKENDS
  // (platform + renderer), set up in Renderer::initImGui and torn down by
  // Renderer::shutdown. So the rebuild re-inits the backends and must NOT call
  // App::initImGui again -- that would create a second context.
  const bool haveWindow = !headless_ && window_ != nullptr;
  const int wantApi = (targetOwner == Backend::GL) ? GLFW_OPENGL_API : GLFW_NO_API;
  const bool needWindowRecreate =
      haveWindow && glfwGetWindowAttrib(window_, GLFW_CLIENT_API) != wantApi;

  renderer_->shutdown();  // also tears down this renderer's ImGui backends
  renderer_.reset();

  std::string err;
  auto buildFor = [&](Backend owner, std::string* e) -> bool {
    if (needWindowRecreate) {
      // Preserve the current window geometry across the recreate.
      int w = 0, h = 0;
      glfwGetWindowSize(window_, &w, &h);
      glfwDestroyWindow(window_);
      window_ = nullptr;
      backend_ = owner;  // initWindow picks its client-API hints from backend_
      if (w > 0 && h > 0) setWindowSize(w, h);
      if (!initWindow(e)) return false;
      ++windowGeneration_;
    }
    if (!createAndInitRenderer(owner, e)) return false;
    return renderer_->initImGui(e);
  };

  bool ok = buildFor(targetOwner, &err);
  if (!ok) {
    LOGW("Switching to %s failed: %s; reverting to %s.",
         RenderTechniqueLabel(t), err.c_str(), RenderTechniqueLabel(prevTechnique));
    std::string restoreErr;
    if (!buildFor(prevOwner, &restoreErr)) {
      LOGE("Failed to restore the previous renderer after a failed backend switch: %s",
           restoreErr.c_str());
      gui_.setRenderer(renderer_.get());
      return false;
    }
  }
  if (!renderer_->uploadScene(draw_, &err)) {
    LOGW("Scene re-upload failed after backend switch: %s", err.c_str());
  }

  RenderTechnique resultTechnique;
  OverlayKind resultOverlay;
  if (!ok) {
    resultTechnique = prevTechnique;
    resultOverlay = OverlayForTechnique(prevTechnique);
  } else if (backend_ != targetOwner) {
    // createAndInitRenderer's own Vulkan-init-failure fallback kicked in.
    LOGW("Requested %s unavailable; using GL raster instead.", RenderTechniqueLabel(t));
    resultTechnique = RenderTechnique::GLRaster;
    resultOverlay = OverlayKind::None;
  } else {
    resultTechnique = t;
    resultOverlay = targetOverlay;
  }

  if (resultOverlay == OverlayKind::VulkanRT ||
      resultOverlay == OverlayKind::VulkanPathTrace) {
    if (renderer_->rayTracingAvailable()) {
      renderer_->setRayTracing(true);
    } else {
      LOGW("Vulkan ray tracing unavailable after switch; staying on Vulkan raster.");
      resultOverlay = OverlayKind::None;
      resultTechnique = RenderTechnique::VulkanRaster;
    }
  }
  hipInteractive_ = (resultOverlay == OverlayKind::HipRT ||
                     resultOverlay == OverlayKind::HipPathTrace);
  cudaInteractive_ = (resultOverlay == OverlayKind::CudaRT ||
                      resultOverlay == OverlayKind::CudaPathTrace);
  cpuInteractive_ = (resultOverlay == OverlayKind::CpuRT);
  if (!hipInteractive_ && !cudaInteractive_ && !cpuInteractive_) {
    restoreNextDrawRestPose();
  }
  previousTechnique_ = activeTechnique_;
  activeOverlay_ = resultOverlay;
  activeTechnique_ = resultTechnique;
  pathTrace_.enabled = resultOverlay == OverlayKind::VulkanPathTrace ||
                       resultOverlay == OverlayKind::CudaPathTrace ||
                       resultOverlay == OverlayKind::HipPathTrace;
  gui_.setPathTraceSettings(pathTrace_);
  // renderer_ is a brand-new object after an owner change; Gui still holds the
  // pointer it was given by gui_.frame() earlier THIS frame and would use it
  // again in renderViewportScene() below (use-after-free -- observed as
  // "free(): invalid pointer" inside VulkanRenderer::renderFrame, and as a
  // SIGSEGV on other drivers). Hand over the new one.
  gui_.setRenderer(renderer_.get());
  return ok;
}

bool App::renderHipViewport() {
  // The initial model loads on a worker thread; wait until it has been applied on
  // the main thread (finishLoadIfReady -> applyLoaded) and draw_ holds geometry.
  // Returning true (not false) here keeps hipInteractive_ enabled across the wait.
  if (loadActive_ || draw_.empty()) {
    // Show a clear viewport while loading -- also keeps colorImg_ in a defined,
    // ImGui-sampleable layout (nothing else writes it on the HIP path).
    int vw = 0, vh = 0;
    gui_.viewportPixelSize(&vw, &vh);
    if (vw > 0 && vh > 0) {
      std::vector<uint8_t> clearPx(static_cast<size_t>(vw) * vh * 4);
      for (size_t i = 0; i + 3 < clearPx.size(); i += 4) {
        clearPx[i] = 31; clearPx[i + 1] = 31; clearPx[i + 2] = 33; clearPx[i + 3] = 255;
      }
      renderer_->uploadViewportImage(clearPx.data(), vw, vh);
    }
    return true;
  }

  // Build the HIP scene once. It runs on a BACKGROUND THREAD so the UI stays
  // responsive and the progress overlay updates live (the build is multi-second on
  // big scenes). Present a couple of frames first so the loading modal has closed
  // and the overlay is visible before the worker starts.
  if (!hipInteractiveBuilt_) {
    if (hipBuildAnnounceFrames_ < 2) {
      ++hipBuildAnnounceFrames_;
      return true;
    }
    if (!hipBuildStarted_) {
      std::string cerr;
      if (!hipTracer_.init(&cerr)) {
        LOGW("HIP ray tracing unavailable: %s; viewport stays blank.", cerr.c_str());
        hipInteractive_ = false;  // give up; avoid retrying every frame
        return false;
      }
      hipBuildStarted_ = true;
      hipBuildStart_ = std::chrono::steady_clock::now();
      const float dispScale = gui_.displacementScale();  // read on the main thread
      const uint32_t purposeMask = gui_.purposeVisibleMask();
      poseNextDrawForTracer(animTime_);  // on the main thread, before the worker reads draw_
      ensureRtTexturePayloads();         // ditto: re-decode raster-released texture payloads
      // The worker reads draw_ (stable while building: the re-pose below only runs
      // once the build has completed) and builds + uploads on the device
      // (hipSetDevice runs in build()).
      // A deformable scene re-poses per time code: retain the host arrays so
      // those re-poses REFIT the BVH in place instead of paying a full rebuild.
      // (The builder refuses the refit map when displacement is actually live
      // on some mesh -- a displaced flatten re-samples textures per pose.)
      const bool retainForRefit = sceneIsNextDeformable();
      hipBuildThread_ = std::thread([this, dispScale, retainForRefit, purposeMask] {
        std::string e;
        const bool ok = hipTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e,
                                         dispScale, &hipBuildProgress_,
                                         retainForRefit, purposeMask);
        hipBuildErr_ = e;
        hipBuildOk_.store(ok, std::memory_order_release);
        hipBuildDone_.store(true, std::memory_order_release);
      });
      return true;
    }
    if (!hipBuildDone_.load(std::memory_order_acquire)) {
      return true;  // still building -> overlay shows live progress
    }
    if (hipBuildThread_.joinable()) hipBuildThread_.join();
    if (!hipBuildOk_.load(std::memory_order_acquire)) {
      LOGW("HIP ray tracing build failed: %s", hipBuildErr_.c_str());
      hipInteractive_ = false;
      return false;
    }
    hipInteractiveBuilt_ = true;
    hipPathAccum_.invalidate();
    LOGI("HIP interactive: %zu tris%s on %s", hipTracer_.triangleCount(),
         hipTracer_.truncated() ? " [truncated]" : "", hipTracer_.deviceName());
    LOGI("HIP offscreen presentation: %s compute -> primary %s viewport blit",
         hipTracer_.deviceName(), backend_ == Backend::Vulkan ? "Vulkan" : "OpenGL");
    // A deformable scene keeps its CPU geometry: the timeline re-poses it and
    // rebuilds the BVH below, so draw_ is read again on every new time code.
    if (sceneIsNextDeformable()) return true;
    // Otherwise the scene now lives entirely in the GPU BVH; reclaim the (large)
    // CPU geometry -- the interactive trace never reads draw_ geometry again.
    // Skipped once live technique switching is enabled: switching back to a
    // raster window owner needs draw_'s CPU vertices to re-upload the scene.
    if (liveSwitchEnabled_) return true;
    auto rssMB = [] {
      FILE* f = std::fopen("/proc/self/statm", "r");
      if (!f) return size_t(0);
      long pages = 0, res = 0;
      if (std::fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
      std::fclose(f);
#ifdef _WIN32
      return size_t(0);  // unreachable: /proc/self/statm doesn't exist on Windows
#else
      return size_t((static_cast<long long>(res) * sysconf(_SC_PAGESIZE)) / (1024 * 1024));
#endif
    };
    const size_t before = rssMB();
    for (DrawMeshCPU& m : draw_.meshes) FreeMeshGeometryCPUForRT(m);
    for (DrawPointsCPU& p : draw_.points) FreePointGeometryCPUForRT(p);
    for (DrawCurvesCPU& c : draw_.curves) FreeCurveGeometryCPUForRT(c);
    for (DrawTextureCPU& t : draw_.textures) FreeTexturePayloadCPUForRT(t);
    const size_t after = rssMB();
    if (before > after)
      LOGI("freed CPU geometry after RT build: %zu -> %zu MB host RSS", before, after);
  }

  // Animation. The BVH is built from vertex positions, so a new time code means
  // a re-pose and an acceleration-structure update. When the initial build
  // retained its host arrays (deformable, no displacement), that update is a
  // REFIT: rewrite tris/nrms in leaf order, refit node bounds over the unchanged
  // trees, upload only those four buffers. Otherwise (or if the topology guard
  // trips) it falls back to the full rebuild -- still far cheaper than the
  // whole-converter re-run this path originally needed. Synchronous: the
  // timeline should not run ahead of what is on screen.
  if (sceneIsNextDeformable() && animTime_ != nextTracerPosedTime_) {
    if (poseNextDrawForTracer(animTime_)) {
      std::string e;
      const float dispScale = gui_.displacementScale();
      // LUSDVIEW_NO_BVH_REFIT=1 restores the rebuild-per-pose path (A/B lever:
      // the refit parity gate compares the two).
      static const bool kNoRefit =
          std::getenv("LUSDVIEW_NO_BVH_REFIT") != nullptr;
      if (!kNoRefit && hipTracer_.canRefit() && hipTracer_.refit(draw_, &e)) {
        hipPathAccum_.invalidate();
      } else if (!hipTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e,
                                   dispScale, nullptr,
                                   /*retainForRefit=*/!kNoRefit,
                                   gui_.purposeVisibleMask())) {
        LOGW("HIP re-pose rebuild failed: %s", e.c_str());
      }
    }
  }

  int w = 0, h = 0;
  gui_.viewportPixelSize(&w, &h);
  if (w < 1 || h < 1) return true;  // viewport not laid out yet this frame

  camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
  const light3d::Mat4 pv = camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
  const light3d::Mat4 inv = pv.inverse();
  const light3d::Vec3 eye = camera_.eye();
  const float camPos[3] = {eye.x, eye.y, eye.z};
  float lightDir[3];
  CopyPreviewLightDir(draw_, lightDir);
  const float clear[3] = {0.12f, 0.12f, 0.13f};
  const int rmode = static_cast<int>(gui_.renderMode());
  float depthScale = 1.0f;
  float sceneMin[3] = {0, 0, 0}, sceneExtent[3] = {1, 1, 1};
  if (draw_.hasBounds) {
    const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
    const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
    const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
    depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
    for (int i = 0; i < 3; ++i) {
      sceneMin[i] = draw_.aabbMin[i];
      sceneExtent[i] = std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
    }
  }

  std::vector<uint8_t> rgba;
  std::vector<float> linear;
  std::string cerr;
  auto prepareAccumulation = [&](ExternalPathAccumulation* state,
                                 uint64_t kernelGeneration) {
    bool same = state->valid && state->width == w && state->height == h &&
                state->renderMode == rmode &&
                state->focusDistance == cameraLens_.focusDistance &&
                state->apertureRadius == cameraLens_.apertureRadius &&
                state->sceneTime == static_cast<float>(animTime_) &&
                state->maxDepth == pathTrace_.maxDepth &&
                state->rouletteDepth == pathTrace_.russianRouletteDepth &&
                state->maxSubsurfaceEvents == pathTrace_.maxSubsurfaceEvents &&
                state->maxVolumeEvents == pathTrace_.maxVolumeEvents &&
                state->motionSegments == pathTrace_.motionSegments &&
                state->seed == pathTrace_.seed &&
                state->kernelGeneration == kernelGeneration &&
                std::memcmp(state->viewProj.data(), pv.m, sizeof(pv.m)) == 0;
    if (!same) {
      state->samples = 0;
      ++state->generation;
    }
    std::memcpy(state->viewProj.data(), pv.m, sizeof(pv.m));
    state->focusDistance = cameraLens_.focusDistance;
    state->apertureRadius = cameraLens_.apertureRadius;
    state->sceneTime = static_cast<float>(animTime_);
    state->width = w;
    state->height = h;
    state->renderMode = rmode;
    state->maxDepth = pathTrace_.maxDepth;
    state->rouletteDepth = pathTrace_.russianRouletteDepth;
    state->maxSubsurfaceEvents = pathTrace_.maxSubsurfaceEvents;
    state->maxVolumeEvents = pathTrace_.maxVolumeEvents;
    state->motionSegments = pathTrace_.motionSegments;
    state->seed = pathTrace_.seed;
    state->kernelGeneration = kernelGeneration;
    state->valid = true;
  };
  if (pathTrace_.enabled)
    prepareAccumulation(&hipPathAccum_, hipTracer_.kernelGeneration());
  else
    hipPathAccum_.invalidate();
  if (pathTrace_.enabled && pathTrace_.targetSamples > 0 &&
      hipPathAccum_.samples >= pathTrace_.targetSamples &&
      !hipPathAccum_.lastRgba.empty()) {
    renderer_->uploadViewportImage(hipPathAccum_.lastRgba.data(), w, h);
    return true;
  }
  // Static production views trace on a worker. The GPU stream still owns
  // ordering; this keeps its synchronization and the cross-device readback off
  // the UI thread. Animated scenes retain the synchronous path below so a refit
  // can never race an in-flight traversal.
  if (pathTrace_.enabled && !sceneIsNextDeformable()) {
    if (hipTraceFuture_.valid()) {
      if (hipTraceFuture_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        if (!hipPathAccum_.lastRgba.empty())
          renderer_->uploadViewportImage(hipPathAccum_.lastRgba.data(), w, h);
        return true;
      }
      ExternalTraceResult completed = hipTraceFuture_.get();
      if (!completed.ok) {
        LOGW("HIP asynchronous ray trace failed: %s", completed.error.c_str());
      } else if (completed.generation == hipPathAccum_.generation &&
                 completed.accumulationStart == hipPathAccum_.samples &&
                 completed.width == w && completed.height == h) {
        hipPathAccum_.samples = completed.samples;
        const bool denoiseCheckpoint = completed.samples >= 8u &&
            (((completed.samples & (completed.samples - 1u)) == 0u) ||
             completed.samples % 64u == 0u);
        if (denoiseCheckpoint)
          completed.rgba = ToneMapPathDisplay(
              PreparePathDisplay(completed.linear, w, h, pathTrace_,
                                 completed.samples),
              w, h, completed.exposure);
        hipPathAccum_.lastRgba = std::move(completed.rgba);
      }
    }
    if (pathTrace_.targetSamples == 0 ||
        hipPathAccum_.samples < pathTrace_.targetSamples) {
      std::array<float, 16> invCopy{}, pvCopy{};
      std::array<float, 3> camCopy{}, lightCopy{}, clearCopy{}, minCopy{}, extentCopy{};
      std::copy(inv.m, inv.m + 16, invCopy.begin());
      std::copy(pv.m, pv.m + 16, pvCopy.begin());
      std::copy(camPos, camPos + 3, camCopy.begin());
      std::copy(lightDir, lightDir + 3, lightCopy.begin());
      std::copy(clear, clear + 3, clearCopy.begin());
      std::copy(sceneMin, sceneMin + 3, minCopy.begin());
      std::copy(sceneExtent, sceneExtent + 3, extentCopy.begin());
      const RtCameraLens lensCopy = cameraLens_;
      const PathTraceSettings settingsCopy = pathTrace_;
      const uint64_t generation = hipPathAccum_.generation;
      const uint32_t start = hipPathAccum_.samples;
      const uint32_t nextSample = start + 1u;
      const bool wantLinear = nextSample >= 8u &&
          (((nextSample & (nextSample - 1u)) == 0u) || nextSample % 64u == 0u);
      const float exposure = camera_.exposure();
      const float sceneSeconds = static_cast<float>(
          animFps_ > 0.0 ? animTime_ / animFps_ : 0.0);
      const float sceneFrame = static_cast<float>(animTime_);
      hipTraceFuture_ = std::async(std::launch::async,
          [this, invCopy, pvCopy, camCopy, lightCopy, clearCopy, minCopy,
           extentCopy, lensCopy, settingsCopy, generation, start, exposure,
           sceneSeconds, sceneFrame, rmode, depthScale, w, h,
           wantLinear]() mutable {
            ExternalTraceResult result;
            result.generation = generation;
            result.accumulationStart = start;
            result.width = w; result.height = h; result.exposure = exposure;
            result.ok = hipTracer_.trace(
                invCopy.data(), pvCopy.data(), camCopy.data(), lightCopy.data(),
                clearCopy.data(), exposure, rmode, depthScale, minCopy.data(),
                extentCopy.data(), w, h, &result.rgba, &result.error, 1,
                &lensCopy, &settingsCopy,
                wantLinear ? &result.linear : nullptr, &result.samples,
                sceneSeconds, sceneFrame, start);
            return result;
          });
    }
    if (!hipPathAccum_.lastRgba.empty())
      renderer_->uploadViewportImage(hipPathAccum_.lastRgba.data(), w, h);
    return true;
  }
  uint32_t accumulatedSamples = 0;
  // spp=1: single sample for interactive frame rate (no supersampled AA).
  if (hipTracer_.trace(inv.m, pv.m, camPos, lightDir, clear, camera_.exposure(), rmode, depthScale, sceneMin,
                       sceneExtent, w, h, &rgba, &cerr, /*spp=*/1,
                       &cameraLens_, pathTrace_.enabled ? &pathTrace_ : nullptr,
                       pathTrace_.enabled ? &linear : nullptr,
                       pathTrace_.enabled ? &accumulatedSamples : nullptr,
                       static_cast<float>(animFps_ > 0.0 ? animTime_ / animFps_ : 0.0),
                       static_cast<float>(animTime_),
                       pathTrace_.enabled ? hipPathAccum_.samples : 0u)) {
    if (pathTrace_.enabled) {
      hipPathAccum_.samples = accumulatedSamples;
      rgba = ToneMapPathDisplay(
          PreparePathDisplay(linear, w, h, pathTrace_, accumulatedSamples), w, h,
          camera_.exposure());
      hipPathAccum_.lastRgba = rgba;
    }
    renderer_->uploadViewportImage(rgba.data(), w, h);
  } else {
    LOGW("HIP ray trace failed: %s", cerr.c_str());
  }
  return true;
}

bool App::renderCudaViewport() {
  // Mirrors renderHipViewport() above; see its comments for the overall shape
  // (background build with a progress overlay, then per-frame trace ->
  // uploadViewportImage). Topology-stable deformation refits the software BVH
  // and, when selected, update-capable OptiX GAS plus its small IAS.
  if (loadActive_ || draw_.empty()) {
    int vw = 0, vh = 0;
    gui_.viewportPixelSize(&vw, &vh);
    if (vw > 0 && vh > 0) {
      std::vector<uint8_t> clearPx(static_cast<size_t>(vw) * vh * 4);
      for (size_t i = 0; i + 3 < clearPx.size(); i += 4) {
        clearPx[i] = 31; clearPx[i + 1] = 31; clearPx[i + 2] = 33; clearPx[i + 3] = 255;
      }
      renderer_->uploadViewportImage(clearPx.data(), vw, vh);
    }
    return true;
  }

  if (!cudaInteractiveBuilt_) {
    if (cudaBuildAnnounceFrames_ < 2) {
      ++cudaBuildAnnounceFrames_;
      return true;
    }
    if (!cudaBuildStarted_) {
      std::string cerr;
      if (!cudaTracer_.init(&cerr)) {
        LOGW("CUDA ray tracing unavailable: %s; viewport stays blank.", cerr.c_str());
        cudaInteractive_ = false;  // give up; avoid retrying every frame
        cudaProbe_ = ProbeState::Unavailable;
        return false;
      }
      cudaProbe_ = ProbeState::Available;
      cudaBuildStarted_ = true;
      cudaBuildStart_ = std::chrono::steady_clock::now();
      const float dispScale = gui_.displacementScale();  // read on the main thread
      const uint32_t purposeMask = gui_.purposeVisibleMask();
      poseNextDrawForTracer(animTime_);  // on the main thread, before the worker reads draw_
      ensureRtTexturePayloads();         // ditto: re-decode raster-released texture payloads
      const bool retainForRefit = sceneIsNextDeformable();
      cudaBuildThread_ = std::thread([this, dispScale, purposeMask,
                                      retainForRefit] {
        std::string e;
        const bool ok = cudaTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e,
                                          dispScale, &cudaBuildProgress_,
                                          retainForRefit, purposeMask);
        cudaBuildErr_ = e;
        cudaBuildOk_.store(ok, std::memory_order_release);
        cudaBuildDone_.store(true, std::memory_order_release);
      });
      return true;
    }
    if (!cudaBuildDone_.load(std::memory_order_acquire)) {
      return true;  // still building -> overlay shows live progress
    }
    if (cudaBuildThread_.joinable()) cudaBuildThread_.join();
    if (!cudaBuildOk_.load(std::memory_order_acquire)) {
      LOGW("CUDA ray tracing build failed: %s", cudaBuildErr_.c_str());
      cudaInteractive_ = false;
      return false;
    }
    cudaInteractiveBuilt_ = true;
    cudaPathAccum_.invalidate();
    LOGI("CUDA interactive: %zu tris%s on %s", cudaTracer_.triangleCount(),
         cudaTracer_.truncated() ? " [truncated]" : "", cudaTracer_.deviceName());
    // draw_'s CPU geometry and refit map remain available for topology-stable
    // animation (liveSwitchEnabled_ also requires raster switch-back data).
  }

  if (sceneIsNextDeformable() && animTime_ != nextTracerPosedTime_) {
    if (poseNextDrawForTracer(animTime_)) {
      std::string e;
      const float dispScale = gui_.displacementScale();
      static const bool kNoCudaRefit =
          std::getenv("LUSDVIEW_NO_CUDA_BVH_REFIT") != nullptr;
      if (!kNoCudaRefit && cudaTracer_.canRefit() &&
          cudaTracer_.refit(draw_, &e)) {
        cudaPathAccum_.invalidate();
      } else if (!cudaTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e,
                                    dispScale, nullptr, !kNoCudaRefit,
                                    gui_.purposeVisibleMask())) {
        LOGW("CUDA re-pose rebuild failed: %s", e.c_str());
      }
    }
  }

  int w = 0, h = 0;
  gui_.viewportPixelSize(&w, &h);
  if (w < 1 || h < 1) return true;  // viewport not laid out yet this frame

  camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
  const light3d::Mat4 pv = camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
  const light3d::Mat4 inv = pv.inverse();
  const light3d::Vec3 eye = camera_.eye();
  const float camPos[3] = {eye.x, eye.y, eye.z};
  float lightDir[3];
  CopyPreviewLightDir(draw_, lightDir);
  const float clear[3] = {0.12f, 0.12f, 0.13f};
  const int rmode = static_cast<int>(gui_.renderMode());
  float depthScale = 1.0f;
  float sceneMin[3] = {0, 0, 0}, sceneExtent[3] = {1, 1, 1};
  if (draw_.hasBounds) {
    const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
    const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
    const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
    depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
    for (int i = 0; i < 3; ++i) {
      sceneMin[i] = draw_.aabbMin[i];
      sceneExtent[i] = std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
    }
  }

  std::vector<uint8_t> rgba;
  std::vector<float> linear;
  std::string cerr;
  ExternalPathAccumulation& accum = cudaPathAccum_;
  bool sameAccum = accum.valid && accum.width == w && accum.height == h &&
      accum.renderMode == rmode && accum.focusDistance == cameraLens_.focusDistance &&
      accum.apertureRadius == cameraLens_.apertureRadius &&
      accum.sceneTime == static_cast<float>(animTime_) &&
      accum.maxDepth == pathTrace_.maxDepth &&
      accum.rouletteDepth == pathTrace_.russianRouletteDepth &&
      accum.maxSubsurfaceEvents == pathTrace_.maxSubsurfaceEvents &&
      accum.maxVolumeEvents == pathTrace_.maxVolumeEvents &&
      accum.motionSegments == pathTrace_.motionSegments && accum.seed == pathTrace_.seed &&
      accum.kernelGeneration == cudaTracer_.kernelGeneration() &&
      std::memcmp(accum.viewProj.data(), pv.m, sizeof(pv.m)) == 0;
  if (!pathTrace_.enabled) accum.invalidate();
  else {
    if (!sameAccum) {
      accum.samples = 0;
      ++accum.generation;
    }
    std::memcpy(accum.viewProj.data(), pv.m, sizeof(pv.m));
    accum.focusDistance = cameraLens_.focusDistance;
    accum.apertureRadius = cameraLens_.apertureRadius;
    accum.sceneTime = static_cast<float>(animTime_);
    accum.width = w; accum.height = h; accum.renderMode = rmode;
    accum.maxDepth = pathTrace_.maxDepth;
    accum.rouletteDepth = pathTrace_.russianRouletteDepth;
    accum.maxSubsurfaceEvents = pathTrace_.maxSubsurfaceEvents;
    accum.maxVolumeEvents = pathTrace_.maxVolumeEvents;
    accum.motionSegments = pathTrace_.motionSegments; accum.seed = pathTrace_.seed;
    accum.kernelGeneration = cudaTracer_.kernelGeneration(); accum.valid = true;
    if (pathTrace_.targetSamples > 0 && accum.samples >= pathTrace_.targetSamples &&
        !accum.lastRgba.empty()) {
      renderer_->uploadViewportImage(accum.lastRgba.data(), w, h);
      return true;
    }
  }
  if (pathTrace_.enabled && !sceneIsNextDeformable()) {
    if (cudaTraceFuture_.valid()) {
      if (cudaTraceFuture_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        if (!accum.lastRgba.empty())
          renderer_->uploadViewportImage(accum.lastRgba.data(), w, h);
        return true;
      }
      ExternalTraceResult completed = cudaTraceFuture_.get();
      if (!completed.ok) {
        LOGW("CUDA asynchronous ray trace failed: %s", completed.error.c_str());
      } else if (completed.generation == accum.generation &&
                 completed.accumulationStart == accum.samples &&
                 completed.width == w && completed.height == h) {
        accum.samples = completed.samples;
        const bool denoiseCheckpoint = completed.samples >= 8u &&
            (((completed.samples & (completed.samples - 1u)) == 0u) ||
             completed.samples % 64u == 0u);
        if (denoiseCheckpoint)
          completed.rgba = ToneMapPathDisplay(
              PreparePathDisplay(completed.linear, w, h, pathTrace_,
                                 completed.samples),
              w, h, completed.exposure);
        accum.lastRgba = std::move(completed.rgba);
      }
    }
    if (pathTrace_.targetSamples == 0 || accum.samples < pathTrace_.targetSamples) {
      std::array<float, 16> invCopy{}, pvCopy{};
      std::array<float, 3> camCopy{}, lightCopy{}, clearCopy{}, minCopy{}, extentCopy{};
      std::copy(inv.m, inv.m + 16, invCopy.begin());
      std::copy(pv.m, pv.m + 16, pvCopy.begin());
      std::copy(camPos, camPos + 3, camCopy.begin());
      std::copy(lightDir, lightDir + 3, lightCopy.begin());
      std::copy(clear, clear + 3, clearCopy.begin());
      std::copy(sceneMin, sceneMin + 3, minCopy.begin());
      std::copy(sceneExtent, sceneExtent + 3, extentCopy.begin());
      const RtCameraLens lensCopy = cameraLens_;
      const PathTraceSettings settingsCopy = pathTrace_;
      const uint64_t generation = accum.generation;
      const uint32_t start = accum.samples;
      const uint32_t nextSample = start + 1u;
      const bool wantLinear = nextSample >= 8u &&
          (((nextSample & (nextSample - 1u)) == 0u) || nextSample % 64u == 0u);
      const float exposure = camera_.exposure();
      const float sceneSeconds = static_cast<float>(
          animFps_ > 0.0 ? animTime_ / animFps_ : 0.0);
      const float sceneFrame = static_cast<float>(animTime_);
      cudaTraceFuture_ = std::async(std::launch::async,
          [this, invCopy, pvCopy, camCopy, lightCopy, clearCopy, minCopy,
           extentCopy, lensCopy, settingsCopy, generation, start, exposure,
           sceneSeconds, sceneFrame, rmode, depthScale, w, h,
           wantLinear]() mutable {
            ExternalTraceResult result;
            result.generation = generation;
            result.accumulationStart = start;
            result.width = w; result.height = h; result.exposure = exposure;
            result.ok = cudaTracer_.trace(
                invCopy.data(), pvCopy.data(), camCopy.data(), lightCopy.data(),
                clearCopy.data(), exposure, rmode, depthScale, minCopy.data(),
                extentCopy.data(), w, h, &result.rgba, &result.error, 1,
                &lensCopy, &settingsCopy,
                wantLinear ? &result.linear : nullptr, &result.samples,
                sceneSeconds, sceneFrame, start);
            return result;
          });
    }
    if (!accum.lastRgba.empty())
      renderer_->uploadViewportImage(accum.lastRgba.data(), w, h);
    return true;
  }
  uint32_t accumulatedSamples = 0;
  if (cudaTracer_.trace(inv.m, pv.m, camPos, lightDir, clear, camera_.exposure(), rmode, depthScale, sceneMin,
                        sceneExtent, w, h, &rgba, &cerr, /*spp=*/1,
                        &cameraLens_, pathTrace_.enabled ? &pathTrace_ : nullptr,
                        pathTrace_.enabled ? &linear : nullptr,
                        pathTrace_.enabled ? &accumulatedSamples : nullptr,
                        static_cast<float>(animFps_ > 0.0 ? animTime_ / animFps_ : 0.0),
                        static_cast<float>(animTime_),
                        pathTrace_.enabled ? accum.samples : 0u)) {
    if (pathTrace_.enabled) {
      accum.samples = accumulatedSamples;
      rgba = ToneMapPathDisplay(
          PreparePathDisplay(linear, w, h, pathTrace_, accumulatedSamples), w, h,
          camera_.exposure());
      accum.lastRgba = rgba;
    }
    renderer_->uploadViewportImage(rgba.data(), w, h);
  } else {
    LOGW("CUDA ray trace failed: %s", cerr.c_str());
  }
  return true;
}

bool App::renderCpuViewport() {
  // Mirrors renderCudaViewport() above (CPU tracer has no device to probe, so
  // it never fails past its own build() call -- there is no cpuProbe_ gate).
  if (loadActive_ || draw_.empty()) {
    int vw = 0, vh = 0;
    gui_.viewportPixelSize(&vw, &vh);
    if (vw > 0 && vh > 0) {
      std::vector<uint8_t> clearPx(static_cast<size_t>(vw) * vh * 4);
      for (size_t i = 0; i + 3 < clearPx.size(); i += 4) {
        clearPx[i] = 31; clearPx[i + 1] = 31; clearPx[i + 2] = 33; clearPx[i + 3] = 255;
      }
      renderer_->uploadViewportImage(clearPx.data(), vw, vh);
    }
    return true;
  }

  if (!cpuInteractiveBuilt_) {
    if (cpuBuildAnnounceFrames_ < 2) {
      ++cpuBuildAnnounceFrames_;
      return true;
    }
    if (!cpuBuildStarted_) {
      cpuBuildStarted_ = true;
      const float dispScale = gui_.displacementScale();  // read on the main thread
      const uint32_t purposeMask = gui_.purposeVisibleMask();
      poseNextDrawForTracer(animTime_);  // on the main thread, before the worker reads draw_
      ensureRtTexturePayloads();         // ditto: re-decode raster-released texture payloads
      cpuBuildThread_ = std::thread([this, dispScale, purposeMask] {
        std::string e;
        const bool ok = cpuTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e,
                                         dispScale, &cpuBuildProgress_, purposeMask);
        cpuBuildErr_ = e;
        cpuBuildOk_.store(ok, std::memory_order_release);
        cpuBuildDone_.store(true, std::memory_order_release);
      });
      return true;
    }
    if (!cpuBuildDone_.load(std::memory_order_acquire)) {
      return true;  // still building -> overlay shows live progress
    }
    if (cpuBuildThread_.joinable()) cpuBuildThread_.join();
    if (!cpuBuildOk_.load(std::memory_order_acquire)) {
      LOGW("CPU ray tracing build failed: %s", cpuBuildErr_.c_str());
      cpuInteractive_ = false;
      return false;
    }
    cpuInteractiveBuilt_ = true;
    LOGI("CPU interactive: %zu tris%s on %s", cpuTracer_.triangleCount(),
         cpuTracer_.truncated() ? " [truncated]" : "", cpuTracer_.deviceName());
  }

  if (sceneIsNextDeformable() && animTime_ != nextTracerPosedTime_) {
    if (poseNextDrawForTracer(animTime_)) {
      std::string e;
      const float dispScale = gui_.displacementScale();
      if (!cpuTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &e, dispScale,
                            nullptr, gui_.purposeVisibleMask())) {
        LOGW("CPU re-pose rebuild failed: %s", e.c_str());
      }
    }
  }

  int w = 0, h = 0;
  gui_.viewportPixelSize(&w, &h);
  if (w < 1 || h < 1) return true;  // viewport not laid out yet this frame

  camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
  const light3d::Mat4 pv = camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
  const light3d::Mat4 inv = pv.inverse();
  const light3d::Vec3 eye = camera_.eye();
  const float camPos[3] = {eye.x, eye.y, eye.z};
  float lightDir[3];
  CopyPreviewLightDir(draw_, lightDir);
  const float clear[3] = {0.12f, 0.12f, 0.13f};
  const int rmode = static_cast<int>(gui_.renderMode());
  float depthScale = 1.0f;
  float sceneMin[3] = {0, 0, 0}, sceneExtent[3] = {1, 1, 1};
  if (draw_.hasBounds) {
    const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
    const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
    const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
    depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
    for (int i = 0; i < 3; ++i) {
      sceneMin[i] = draw_.aabbMin[i];
      sceneExtent[i] = std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
    }
  }

  std::vector<uint8_t> rgba;
  std::string cerr;
  if (cpuTracer_.trace(inv.m, pv.m, camPos, lightDir, clear, camera_.exposure(), rmode, depthScale, sceneMin,
                       sceneExtent, w, h, &rgba, &cerr, rtSamples_,
                       &cameraLens_)) {
    renderer_->uploadViewportImage(rgba.data(), w, h);
  } else {
    LOGW("CPU ray trace failed: %s", cerr.c_str());
  }
  return true;
}

void App::markStreamActivity() {
  streamLastActivity_ = std::chrono::steady_clock::now();
  streamHiQSent_ = false;
}

void App::streamEncodeAndPush(std::vector<uint8_t> rgba, int w, int h,
                              bool motion) {
  if (!streamServer_ || w <= 0 || h <= 0) return;

  int tw = w, th = h;
  std::vector<uint8_t> scaled;
  if (motion && std::max(w, h) > streamMotionMaxDim_) {
    // Downscale (sRGB-correct) to a small frame for fast interaction.
    const double s = double(streamMotionMaxDim_) / double(std::max(w, h));
    tw = std::max(1, int(w * s));
    th = std::max(1, int(h * s));
    scaled.resize(size_t(tw) * size_t(th) * 4);
    if (stbir_resize_uint8_srgb(rgba.data(), w, h, 0, scaled.data(), tw, th, 0,
                                STBIR_RGBA)) {
      rgba = std::move(scaled);
    } else {
      tw = w; th = h;  // resize failed: fall back to full size
    }
  }

  lightusd::Image img;
  img.width = tw;
  img.height = th;
  img.channels = 4;
  img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data = std::move(rgba);

  lightusd::image::WriteOption opt;
  if (motion) {
    opt.format = lightusd::image::WriteImageFormat::JPEG;
    opt.jpeg_quality = streamMotionJpegQ_;
  } else if (streamIdleCodec_ == "qoi") {
    opt.format = lightusd::image::WriteImageFormat::QOI;
  } else {  // default refinement: PNG (lossless)
    opt.format = lightusd::image::WriteImageFormat::PNG;
  }
  auto enc = lightusd::image::WriteImageToMemory(img, opt);
  if (enc) streamServer_->pushFrame(enc.value().data(), enc.value().size());
}

// Map a browser KeyboardEvent.key string to an ImGuiKey so ImGui text fields and
// keyboard navigation (arrows, backspace, enter, tab, ...) work over the stream.
static ImGuiKey StreamKeyToImGui(const std::string& k) {
  if (k.size() == 1) {
    const char c = k[0];
    if (c >= 'a' && c <= 'z') return ImGuiKey(ImGuiKey_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return ImGuiKey(ImGuiKey_A + (c - 'A'));
    if (c >= '0' && c <= '9') return ImGuiKey(ImGuiKey_0 + (c - '0'));
    if (c == ' ') return ImGuiKey_Space;
  }
  if (k == "Enter") return ImGuiKey_Enter;
  if (k == "Backspace") return ImGuiKey_Backspace;
  if (k == "Delete") return ImGuiKey_Delete;
  if (k == "Tab") return ImGuiKey_Tab;
  if (k == "Escape") return ImGuiKey_Escape;
  if (k == "ArrowLeft") return ImGuiKey_LeftArrow;
  if (k == "ArrowRight") return ImGuiKey_RightArrow;
  if (k == "ArrowUp") return ImGuiKey_UpArrow;
  if (k == "ArrowDown") return ImGuiKey_DownArrow;
  if (k == "Home") return ImGuiKey_Home;
  if (k == "End") return ImGuiKey_End;
  if (k == "PageUp") return ImGuiKey_PageUp;
  if (k == "PageDown") return ImGuiKey_PageDown;
  return ImGuiKey_None;
}

// Apply one browser input event. Runs on the main thread BEFORE ImGui::NewFrame()
// (drained from the stream server's queue): raw mouse/keyboard events are injected
// into ImGui so its widgets are clickable, and when ImGui isn't capturing the
// mouse the same drags drive the camera (orbit/pan/dolly) -- like the desktop app.
void App::applyNavCommand(const StreamNav& c) {
  ImGuiIO& io = ImGui::GetIO();
  // Any browser input keeps the stream in interactive (low-latency) mode and
  // defers the next lossless refinement.
  markStreamActivity();
  switch (c.type) {
    case StreamNav::MouseMove: {
      io.AddMousePosEvent(c.x, c.y);
      if (streamCamDrag_) {
        const float dx = c.x - streamLastX_, dy = c.y - streamLastY_;
        if (streamDragButton_ == 1 || (streamDragButton_ == 0 && streamDragShift_))
          camera_.pan(dx, dy);                       // middle / shift+left = pan
        else if (streamDragButton_ == 2)
          camera_.dolly((dx - dy) * 0.05f);          // right = dolly
        else
          camera_.orbit(dx, dy);                     // left = orbit
      }
      streamLastX_ = c.x;
      streamLastY_ = c.y;
      break;
    }
    case StreamNav::MouseButton: {
      io.AddMousePosEvent(c.x, c.y);
      // DOM button (0=L,1=M,2=R) -> ImGui button (0=L,1=R,2=M).
      const int imguiBtn = (c.button == 1) ? 2 : (c.button == 2) ? 1 : 0;
      io.AddMouseButtonEvent(imguiBtn, c.down);
      if (c.down) {
        // Latch at press: drive the camera only when the press is over the 3D
        // viewport (not an ImGui panel/widget). The viewport is itself an ImGui
        // window, so WantCaptureMouse is always true there -- use the viewport
        // hover signal instead, mirroring the desktop navigation gate.
        streamCamDrag_ = gui_.viewportHovered();
        streamDragButton_ = c.button;
        streamDragShift_ = c.shift;
      } else {
        streamCamDrag_ = false;
      }
      streamLastX_ = c.x;
      streamLastY_ = c.y;
      break;
    }
    case StreamNav::Wheel:
      io.AddMousePosEvent(c.x, c.y);
      io.AddMouseWheelEvent(0.0f, c.wheel);
      if (gui_.viewportHovered()) camera_.dolly(c.wheel);
      break;
    case StreamNav::Key: {
      const std::string& k = c.str;
      // Keep ImGui's modifier state in sync, then feed the key event (press AND
      // release) so editing/navigation keys work in ImGui widgets.
      io.AddKeyEvent(ImGuiMod_Shift, c.shift);
      io.AddKeyEvent(ImGuiMod_Ctrl, c.ctrl);
      io.AddKeyEvent(ImGuiMod_Alt, c.alt);
      const ImGuiKey ik = StreamKeyToImGui(k);
      // These keys are handled immediately below so browser-panel buttons work
      // even when the streamed viewport is not hovered. Do not also inject them
      // into ImGui or Gui::handleNavigation would apply the action twice.
      const bool directViewKey = !io.WantTextInput && !c.alt && !c.ctrl &&
                                 (k == "v" || k == "w" || k == "s");
      if (ik != ImGuiKey_None && !directViewKey) io.AddKeyEvent(ik, c.down);
      if (!c.down) break;  // the rest reacts to presses only
      // Printable char into a focused ImGui text field; don't fire hotkeys while
      // editing text.
      if (io.WantTextInput) {
        if (k.size() == 1 && static_cast<unsigned char>(k[0]) >= 0x20)
          io.AddInputCharacter(static_cast<unsigned>(k[0]));
        break;
      }
      if (k == "v") {
        gui_.cycleWireframe();
      } else if (k == "w") {
        camera_.moveForward(c.shift ? 3.0f : 1.0f);
      } else if (k == "s") {
        camera_.moveForward(c.shift ? -3.0f : -1.0f);
      } else if (k == "f" || k == "a") {
        if (draw_.hasBounds) camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
      } else if (k == "0") {
        camera_.setPreset(CameraViewPreset::Isometric);
        if (draw_.hasBounds) camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
      } else if (k == "5") {
        camera_.setPreset(CameraViewPreset::Isometric);
      } else if (k == "1") {
        camera_.setPreset(CameraViewPreset::Front);
      } else if (k == "3") {
        camera_.setPreset(CameraViewPreset::Right);
      } else if (k == "7") {
        camera_.setPreset(CameraViewPreset::Top);
      }
      break;
    }
    case StreamNav::Load:
      if (!c.str.empty()) startLoadAsync(c.str);
      break;
    case StreamNav::Codec:
      // Selects the idle-refinement codec; re-send a refined frame in it.
      if (c.str == "png" || c.str == "qoi") {
        streamIdleCodec_ = c.str;
        streamHiQSent_ = false;
      }
      break;
    case StreamNav::Resize:
      // Windowed: resize the real window. Headless: queue a composite resize,
      // applied at the top of the next frame (updates winW/winH + the VK swap).
      if (c.w > 0 && c.h > 0) {
        if (!headless_ && window_) {
          glfwSetWindowSize(window_, c.w, c.h);
        } else if (headless_) {
          streamResizeW_ = c.w;
          streamResizeH_ = c.h;
        }
      }
      break;
  }
}

// Stable, machine-readable capability line. Tests used to infer hardware from
// prose log lines ("Vulkan ray tracing (ray query) enabled.", the GPU name), and
// every time that prose was reworded the tests silently stopped covering
// anything -- reporting missing hardware on a machine that had it. This line is
// versioned and parsed instead. Emit it after renderer init and after any change
// that alters what it reports (ray tracing on/off, backend switch).
//
// Contract for consumers -- do not reword, only extend:
//   caps: v1 backend=<gl|vulkan|unknown> device=<discrete|integrated|virtual|cpu|gpu|other|unknown>
//         rt=<off|hardware|software> rt_available=<0|1> gpu="<name>"
// New keys append before gpu=; the gpu= value is quoted and last because it is
// the only field that can contain spaces.
std::string App::activeLiveShaderBackend() const {
  switch (activeTechnique_) {
    case RenderTechnique::VulkanRT:
    case RenderTechnique::VulkanPathTrace: return "vulkan";
    case RenderTechnique::CudaRT:
    case RenderTechnique::CudaPathTrace: return "cuda";
    case RenderTechnique::HipRT:
    case RenderTechnique::HipPathTrace: return "hip";
    default: return std::string();
  }
}

void App::initializeLiveShaderSources() {
  if (liveVulkanShader_.source.empty()) {
    liveVulkanShader_.source =
        renderer_ && renderer_->rayTracingActive() &&
                !renderer_->rayTracingIsHardware()
            ? LUSDVIEW_DEFAULT_VK_SW_SHADER_SOURCE
            : LUSDVIEW_DEFAULT_VK_SHADER_SOURCE;
  }
  if (liveCudaKernel_.source.empty())
    liveCudaKernel_.source = LUSDVIEW_DEFAULT_GPU_KERNEL_SOURCE;
  if (liveHipKernel_.source.empty())
    liveHipKernel_.source = LUSDVIEW_DEFAULT_GPU_KERNEL_SOURCE;
  if (liveShaderWatchRequested_) {
    const std::string active = activeLiveShaderBackend();
    if (active == "vulkan") liveVulkanShader_.watch = true;
    else if (active == "cuda") liveCudaKernel_.watch = true;
    else if (active == "hip") liveHipKernel_.watch = true;
    else LOGW("live shader watch requested without an active GPU RT technique");
  }
}

bool App::reloadLiveShader(const std::string& requestedBackend,
                           const std::string& sourceOverride,
                           std::string* err) {
  finishLiveShaderReloads();
  const std::string backend = requestedBackend == "active"
                                  ? activeLiveShaderBackend()
                                  : requestedBackend;
  LiveShaderState* state = nullptr;
  if (backend == "vulkan") state = &liveVulkanShader_;
  else if (backend == "cuda") state = &liveCudaKernel_;
  else if (backend == "hip") state = &liveHipKernel_;
  else {
    if (err) *err = "shader reload backend must be active, vulkan, cuda, or hip";
    return false;
  }
  if (!sourceOverride.empty()) state->source = sourceOverride;
  // Keep an explicit MCP source sticky, but follow live technique switches
  // when the state still names one of lusdview's two built-in Vulkan sources.
  if (backend == "vulkan" && sourceOverride.empty() && renderer_ &&
      renderer_->rayTracingActive()) {
    const bool builtInSource =
        state->source == LUSDVIEW_DEFAULT_VK_SHADER_SOURCE ||
        state->source == LUSDVIEW_DEFAULT_VK_SW_SHADER_SOURCE;
    if (builtInSource) {
      state->source = renderer_->rayTracingIsHardware()
                          ? LUSDVIEW_DEFAULT_VK_SHADER_SOURCE
                          : LUSDVIEW_DEFAULT_VK_SW_SHADER_SOURCE;
    }
  }
  if (state->source.empty()) {
    if (err) *err = "no live shader source configured for " + backend;
    return false;
  }
  if (state->pending) {
    if (backend == "vulkan" && state->watch) state->queuedWatchReload = true;
    if (err) *err = backend + " shader reload is still pending";
    return false;
  }
  ++state->attempts;
  state->lastError.clear();
  const auto started = std::chrono::steady_clock::now();
  bool ok = false;
  std::string localError;
  if (backend == "vulkan") {
    if (!renderer_ || backend_ != Backend::Vulkan) {
      localError = "the persistent viewer is not owned by Vulkan";
    } else {
      const bool softwareBvh = !renderer_->rayTracingIsHardware();
      const bool full = renderer_->rayTracingUsesFullShader();
      const bool procedural = renderer_->rayTracingUsesProceduralMaterialX();
      const std::string source = state->source;
      const size_t maxBytes = materialXVulkanShaderMaxSourceBytes_;
      const int timeout = materialXVulkanCompileTimeoutSec_;
      state->pending = std::make_shared<AsyncShaderReloadResult>();
      state->phase = "compiling";
      state->vulkanSourceJob.emplace(std::async(
          std::launch::async,
          [source, full, procedural, softwareBvh,
           productionPath = pathTrace_.enabled, maxBytes,
           timeout]() {
            LiveShaderState::VulkanSourceResult result;
            result.ok = CompileLiveVulkanShader(
                source, full, procedural, softwareBvh, productionPath,
                maxBytes, timeout, &result.spirv, &result.error);
            return result;
          }));
      LOGI("Vulkan live shader reload compiling asynchronously");
      return true;
    }
  } else if (backend == "cuda") {
    if (cudaBuildStarted_ && !cudaBuildDone_.load(std::memory_order_acquire)) {
      localError = "CUDA scene build is in progress; retry after it completes";
    } else if (!cudaTracer_.initialized() && !cudaTracer_.init(&localError)) {
      // init reports the unavailable runtime/compiler/device.
    } else {
      ok = cudaTracer_.reloadKernel(state->source, &localError);
    }
  } else {
    if (hipBuildStarted_ && !hipBuildDone_.load(std::memory_order_acquire)) {
      localError = "HIP scene build is in progress; retry after it completes";
    } else if (!hipTracer_.initialized() && !hipTracer_.init(&localError)) {
      // init reports the unavailable runtime/compiler/device.
    } else {
      ok = hipTracer_.reloadKernel(state->source, &localError);
    }
  }
  state->lastCompileMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  if (ok) {
    ++state->successes;
    std::error_code timestampError;
    state->timestamp =
        std::filesystem::last_write_time(state->source, timestampError);
    state->haveTimestamp = !timestampError;
    LOGI("%s live shader reload succeeded in %.1f ms", backend.c_str(),
         state->lastCompileMs);
    return true;
  }
  state->lastError = localError.empty() ? "unknown live reload error" : localError;
  LOGW("%s live shader reload failed; keeping last good module: %s",
       backend.c_str(), state->lastError.c_str());
  if (err) *err = state->lastError;
  return false;
}

void App::finishLiveShaderReloads() {
  struct PendingState {
    const char* backend;
    LiveShaderState* state;
  } states[] = {{"vulkan", &liveVulkanShader_},
                {"cuda", &liveCudaKernel_},
                {"hip", &liveHipKernel_}};
  for (const PendingState& item : states) {
    LiveShaderState& state = *item.state;
    if (std::strcmp(item.backend, "vulkan") == 0 && state.vulkanSourceJob &&
        state.vulkanSourceJob->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
      LiveShaderState::VulkanSourceResult source = state.vulkanSourceJob->get();
      state.vulkanSourceJob.reset();
      if (!source.ok) {
        state.pending->error = source.error;
        state.pending->done.store(true, std::memory_order_release);
      } else {
        state.phase = "warming";
        const std::shared_ptr<AsyncShaderReloadResult> pending = state.pending;
        postGpu([this, words = std::move(source.spirv), pending]() mutable {
          std::string error;
          if (!renderer_->beginReloadRayTracingShaderAsync(
                  std::move(words), pending, &error)) {
            pending->error = error;
            pending->done.store(true, std::memory_order_release);
          }
        });
      }
    }
    if (!state.pending ||
        !state.pending->done.load(std::memory_order_acquire)) {
      continue;
    }
    const std::shared_ptr<AsyncShaderReloadResult> result = state.pending;
    state.pending.reset();
    state.phase.clear();
    state.lastCompileMs = result->elapsedMs;
    if (result->ok) {
      ++state.successes;
      state.lastError.clear();
      std::error_code timestampError;
      state.timestamp =
          std::filesystem::last_write_time(state.source, timestampError);
      state.haveTimestamp = !timestampError;
      LOGI("%s live shader reload succeeded in %.1f ms", item.backend,
           state.lastCompileMs);
    } else {
      state.lastError = result->error.empty()
                            ? "unknown live reload error"
                            : result->error;
      LOGW("%s live shader reload failed; keeping last good module: %s",
           item.backend, state.lastError.c_str());
    }
    if (std::strcmp(item.backend, "vulkan") == 0 &&
        state.queuedWatchReload) {
      state.queuedWatchReload = false;
      std::string ignored;
      reloadLiveShader("vulkan", std::string(), &ignored);
    }
  }
}

void App::pollLiveShaderReload() {
  finishLiveShaderReloads();
  const auto now = std::chrono::steady_clock::now();
  if (now < liveShaderNextPoll_) return;
  liveShaderNextPoll_ = now + std::chrono::milliseconds(250);
  struct Watched {
    const char* backend;
    LiveShaderState* state;
  } watched[] = {{"vulkan", &liveVulkanShader_},
                 {"cuda", &liveCudaKernel_},
                 {"hip", &liveHipKernel_}};
  for (const Watched& item : watched) {
    LiveShaderState& state = *item.state;
    if (!state.watch || state.source.empty()) continue;
    std::error_code ec;
    const auto timestamp =
        std::filesystem::last_write_time(state.source, ec);
    if (ec) {
      state.lastError = "watch cannot stat source: " + state.source;
      continue;
    }
    if (!state.haveTimestamp) {
      state.timestamp = timestamp;
      state.haveTimestamp = true;
      continue;
    }
    if (timestamp == state.timestamp) continue;
    // Consume the timestamp before compiling. A broken intermediate save keeps
    // the last good module and waits for the editor's next write, avoiding a
    // compile-failure loop every 250 ms.
    state.timestamp = timestamp;
    if (state.pending && std::strcmp(item.backend, "vulkan") == 0) {
      state.queuedWatchReload = true;
      continue;
    }
    std::string reloadError;
    reloadLiveShader(item.backend, std::string(), &reloadError);
  }
}

// Write the current viewport under --mode-sweep. Mirrors the end-of-run capture
// path (including the threaded-GL case, where the render thread owns the
// context and leaves the pixels in renderCapture_).
void App::captureModeSweepFrame(const std::string& modeName) {
  if (modeSweepPattern_.empty()) return;
  std::string path = modeSweepPattern_;
  for (size_t at = path.find("{mode}"); at != std::string::npos;
       at = path.find("{mode}", at)) {
    path.replace(at, 6, modeName);
    at += modeName.size();
  }
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  bool ok = false;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  if (renderThreadActive_) {
    rgba = renderCapture_;
    w = renderCaptureW_;
    h = renderCaptureH_;
    ok = !rgba.empty();
  } else
#endif
  {
    ok = renderer_->captureViewport(&rgba, &w, &h);
  }
  if (!ok || w <= 0 || h <= 0) {
    LOGW("mode-sweep: capture not supported by this backend (%s)",
         modeName.c_str());
    return;
  }
  std::string shotErr;
  if (WriteScreenshotImage(path, rgba, w, h, &shotErr)) {
    LOGI("wrote %s (%dx%d)", path.c_str(), w, h);
  } else {
    LOGW("failed to write %s: %s", path.c_str(), shotErr.c_str());
  }
}

void App::logCapabilities() const {
  if (!renderer_) return;
  const RendererCaps& c = renderer_->caps();
  const char* backend = (backend_ == Backend::Vulkan) ? "vulkan" : "gl";
  const bool rtOn = renderer_->rayTracingActive();
  const char* rt = !rtOn ? "off"
                         : (renderer_->rayTracingIsHardware() ? "hardware"
                                                              : "software");
  const char* cudaTransport = !cudaTracer_.initialized()
                                  ? "uninitialized"
                                  : (cudaTracer_.usesOptixTransport()
                                         ? "optix" : "software-bvh");
  LOGI("caps: v1 backend=%s device=%s rt=%s rt_available=%d cuda_transport=%s "
       "optix_available=%d optix_abi=%d optix_accel_bytes=%zu gpu=\"%s\"",
       backend, c.device_type.empty() ? "unknown" : c.device_type.c_str(), rt,
       renderer_->rayTracingAvailable() ? 1 : 0,
       cudaTransport, cudaTracer_.optixAvailable() ? 1 : 0,
       cudaTracer_.optixAbiVersion(), cudaTracer_.optixAccelerationBytes(),
       c.gpu_name.empty() ? "unknown" : c.gpu_name.c_str());
}

int App::run(const std::string& initialFile, int maxFrames,
             const std::string& screenshot) {
  runStart_ = std::chrono::steady_clock::now();
  checkpointCount_ = 0;
  // Finite-frame invocations are automation/test runs even when they create a
  // real GLFW window. Never let their small synthetic viewport (commonly
  // 64x64 or 96x96) replace the interactive user's saved window geometry.
  persistImGuiState_ = !headless_ && maxFrames < 0;
  auto finishRun = [&](int code) {
    writeRenderReport(initialFile, code);
    return code;
  };
  std::string err;
  // Headless composite size (no monitor to clamp to); used for the windowless
  // ImGui DisplaySize and the offscreen composite image.
  int winW = 0;
  int winH = 0;
  getRequestedWindowSize(&winW, &winH);
  headlessWinW_ = winW;
  headlessWinH_ = winH;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  // Threaded rendering applies to the windowed GL or Vulkan path (experimental;
  // includes Vulkan ray tracing). Headless keeps the inline single-threaded path.
  renderThreadActive_ = threaded_ && !headless_ &&
                        (backend_ == Backend::GL || backend_ == Backend::Vulkan);
#endif
  // A headless --cuda/--hip run writes its own screenshot from the RT trace and
  // returns before the rasterized capture is used, so the raster scene upload +
  // per-frame draw are pure waste (huge on heavily-instanced scenes). Skip them.
  rtOwnsScreenshot_ = (cudaRt_ || hipRt_ || cpuRt_) && headless_ &&
                      !screenshot.empty();
  // Windowed --hip: the HIP tracer drives the viewport per frame (build once,
  // retrace on the orbit camera). Skip the raster scene upload (which would stall
  // on huge instanced scenes like Moana Island) and render single-threaded so the
  // HIP launch + the colorImg_ upload happen on one thread.
  hipInteractive_ = hipRt_ && !headless_;
  // Windowed --cuda: same shape as --hip above, driven by CudaRayTracer.
  cudaInteractive_ = cudaRt_ && !headless_;
  // Windowed --cpu-rt: same shape, driven by CpuRayTracer. Headless CPU RT uses
  // the one-shot screenshot path after the frame loop, like CUDA/HIP.
  cpuInteractive_ = cpuRt_ && !headless_;
  if (cudaInteractive_)
    activeOverlay_ = pathTrace_.enabled ? OverlayKind::CudaPathTrace
                                        : OverlayKind::CudaRT;
  else if (hipInteractive_)
    activeOverlay_ = pathTrace_.enabled ? OverlayKind::HipPathTrace
                                        : OverlayKind::HipRT;
  else if (cpuInteractive_) activeOverlay_ = OverlayKind::CpuRT;
  if (cudaInteractive_ || hipInteractive_ || cpuInteractive_) {
    activeTechnique_ = cudaInteractive_
                           ? (pathTrace_.enabled ? RenderTechnique::CudaPathTrace
                                                 : RenderTechnique::CudaRT)
                       : hipInteractive_
                           ? (pathTrace_.enabled ? RenderTechnique::HipPathTrace
                                                 : RenderTechnique::HipRT)
                                         : RenderTechnique::CpuRT;
    previousTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                    : RenderTechnique::VulkanRaster;
  }
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  if (hipInteractive_ || cudaInteractive_ || cpuInteractive_) renderThreadActive_ = false;
  // Streaming captures the composited window + encodes inline each frame; run
  // single-threaded so the capture/encode happen on the context-owning thread.
  if (streamHttpPort_ > 0) renderThreadActive_ = false;
#endif
  if (checkpointEvery_ > 0) {
    if (maxFrames < 1) {
      LOGE("--checkpoint-every requires --frames N with N >= 1");
      return finishRun(1);
    }
    if (cudaRt_ || hipRt_) {
      LOGE("--checkpoint-every currently supports raster and Vulkan RT; "
           "CUDA/HIP use --rt-samples for a single deterministic output");
      return finishRun(1);
    }
    if (checkpointPattern_.empty()) {
      if (screenshot.empty()) {
        LOGE("--checkpoint-every requires --checkpoint-pattern or --screenshot");
        return finishRun(1);
      }
      const std::filesystem::path shotPath(screenshot);
      const std::string ext = shotPath.has_extension()
                                  ? shotPath.extension().string()
                                  : std::string(".png");
      checkpointPattern_ =
          (shotPath.parent_path() /
           (shotPath.stem().string() + ".checkpoint-{frame}" + ext))
              .string();
    }
    if (checkpointPattern_.find("{frame}") == std::string::npos) {
      LOGE("--checkpoint-pattern must contain {frame}");
      return finishRun(1);
    }
  }

  if (headless_) {
    if (backend_ != Backend::Vulkan) {
      LOGE("--headless requires the Vulkan backend (pass --backend vk)");
      return finishRun(1);
    }
    // A streaming server runs an open-ended loop (no one-shot screenshot bound).
    if (maxFrames < 0 && streamHttpPort_ <= 0)
      maxFrames = 4;  // windowless runs are bounded by frame count
    // GLFW is never initialized in the headless path (no window, no surface).
  } else {
    if (!initWindow(&err)) {
      LOGE("%s", err.c_str());
      return finishRun(1);
    }
    ++windowGeneration_;
  }

  {
    const size_t rtTextureBudget =
        loadOpts_.textureOptions.textureBudgetMB > 0
            ? static_cast<size_t>(loadOpts_.textureOptions.textureBudgetMB) *
                  1024ull * 1024ull
            : (loadOpts_.textureGpuBudgetBytes > 0
                   ? loadOpts_.textureGpuBudgetBytes / 4u
                   : 0u);
    cudaTracer_.setTextureBudgetBytes(rtTextureBudget);
    hipTracer_.setTextureBudgetBytes(rtTextureBudget);
  }

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  if (renderThreadActive_) {
    if (backend_ == Backend::GL) {
      renderer_ = CreateGLRenderer();
    }
#if defined(HAVE_VULKAN)
    else {
      renderer_ = CreateVulkanRenderer();
    }
#endif
    if (!renderer_) {
      LOGE("no renderer for requested backend");
      return finishRun(1);
    }
    ++rendererGeneration_;
    renderer_->setDevicePreference(devicePreference_);
    renderer_->setTransparencyMode(transparencyMode_);
    renderer_->setRasterQualityHigh(rasterQualityHigh_);
    renderer_->setMaterialXVulkanShaderLimits(
        materialXVulkanShaderMaxSourceBytes_,
        materialXVulkanCompileTimeoutSec_);
    // draw_ is a stable App member (same address for the app's lifetime); the
    // Vulkan compute-BVH RT fallback reads it lazily via BuildHostScene() when
    // it needs to rebuild its software BVH, same data the CUDA/HIP tracers'
    // build(draw_, ...) calls already use elsewhere in this file.
    renderer_->setHostSceneSource(&draw_);
    if (headless_) renderer_->setHeadlessSize(headlessWinW_, headlessWinH_);
    // Threaded: ImGui's GLFW platform init runs on the main thread; renderer_->init()
    // (device/FBO/shaders) + the ImGui render backend run on the render thread.
    // GL: release the context here so the render thread can make it current. VK has
    // no per-thread context, so there is nothing to release. startRenderThread blocks
    // until the render thread finishes init.
    if (!initImGui(&err)) {
      LOGE("ImGui init failed: %s", err.c_str());
      return finishRun(1);
    }
    if (backend_ == Backend::GL) glfwMakeContextCurrent(nullptr);
    if (!startRenderThread()) {
      LOGE("render thread init failed: %s", err.c_str());
      return finishRun(1);
    }
    // Vulkan ray tracing: rayTracingAvailable() reads device support set by init()
    // (already finished — startRenderThread joined on it). setRayTracing() only flips
    // flags + marks the TLAS dirty (built lazily in present() on the render thread),
    // but those fields are read there, so post it to run on the render thread.
    if (rtRequested_) {
      if (renderer_->rayTracingAvailable()) {
        rtPath_ = true;
        postGpu([this] { renderer_->setRayTracing(true); });
        LOGI("Vulkan ray tracing enabled (%s).",
             renderer_->rayTracingIsHardware() ? "hardware ray query"
                                               : "compute-BVH fallback");
        logCapabilities();
      } else {
        LOGW("--rt requested but ray tracing is unavailable; using rasterization.");
      }
    }
    // Windowed --cuda/--hip already seeded activeTechnique_/activeOverlay_
    // above (before the renderer existed); don't clobber that here.
    if (!cudaInteractive_ && !hipInteractive_ && !cpuInteractive_) {
      activeTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                    : (rtPath_ ? (pathTrace_.enabled
                                                        ? RenderTechnique::VulkanPathTrace
                                                        : RenderTechnique::VulkanRT)
                                                               : RenderTechnique::VulkanRaster);
      activeOverlay_ = rtPath_ ? (pathTrace_.enabled
                                      ? OverlayKind::VulkanPathTrace
                                      : OverlayKind::VulkanRT)
                              : OverlayKind::None;
      previousTechnique_ = activeTechnique_;
    }
  } else
#endif
  {
    if (!createAndInitRenderer(backend_, &err)) {
      LOGE("renderer init failed: %s", err.c_str());
      return finishRun(1);
    }

    // Activate Vulkan ray tracing if requested and supported; else stay on raster.
    if (rtRequested_) {
      if (renderer_->rayTracingAvailable()) {
        rtPath_ = true;
        renderer_->setRayTracing(true);
        LOGI("Vulkan ray tracing enabled (%s).",
             renderer_->rayTracingIsHardware() ? "hardware ray query"
                                               : "compute-BVH fallback");
        logCapabilities();
      } else {
        LOGW("--rt requested but ray tracing is unavailable (needs the Vulkan "
             "backend + an RT-capable glslang at build time for either the "
             "hardware ray-query or compute-BVH fallback shader); using "
             "rasterization.");
      }
    }
    // Windowed --cuda/--hip already seeded activeTechnique_/activeOverlay_
    // above (before the renderer existed); don't clobber that here.
    if (!cudaInteractive_ && !hipInteractive_ && !cpuInteractive_) {
      activeTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                    : (rtPath_ ? (pathTrace_.enabled
                                                        ? RenderTechnique::VulkanPathTrace
                                                        : RenderTechnique::VulkanRT)
                                                               : RenderTechnique::VulkanRaster);
      activeOverlay_ = rtPath_ ? (pathTrace_.enabled
                                      ? OverlayKind::VulkanPathTrace
                                      : OverlayKind::VulkanRT)
                              : OverlayKind::None;
      previousTechnique_ = activeTechnique_;
    }

    if (!initImGui(&err)) {
      LOGE("ImGui init failed: %s", err.c_str());
      return finishRun(1);
    }
  }

#if defined(HAVE_VULKAN)
  if (backend_ == Backend::Vulkan) {
    vulkanAvailable_ = true;
    vulkanRtAvailable_ = renderer_ && renderer_->rayTracingAvailable();
  } else if (!headless_) {
    const VulkanProbeResult probe =
        ProbeVulkanBackend(window_, devicePreference_);
    vulkanAvailable_ = probe.rasterAvailable;
    vulkanRtAvailable_ = probe.rtAvailable;
    if (!probe.error.empty()) LOGI("Vulkan capability probe: %s", probe.error.c_str());
  }
#endif

  {
    const RendererCaps& rendererCaps = renderer_->caps();
    LOGI("renderer: %s, GPU: %s, API: %s",
         rendererCaps.backend_name ? rendererCaps.backend_name : "unknown",
         rendererCaps.gpu_name.empty() ? "unknown"
                                       : rendererCaps.gpu_name.c_str(),
         rendererCaps.api_info.empty() ? "unknown"
                                       : rendererCaps.api_info.c_str());
  }
  logCapabilities();

  authoredLights_ = draw_.lights;
  runtimeDomeValid_ = false;
  runtimeDomeKey_.clear();
  gui_.setScene(&loaded_, &draw_);
  if (!startupEnvmapPath_.empty()) {
    Gui::DomeLightRequest request;
    request.source = Gui::DomeSource::File;
    request.filePath = startupEnvmapPath_;
    request.intensity = startupEnvmapIntensity_;
    request.rotationDegrees = startupEnvmapRotationDegrees_;
    applyDomeLightRequest(request);
    gui_.setDomeRequestState(request);
  }
  gui_.setNextStage(nextStageSnapshot_.get());
  gui_.setFacialControls(nextStageSnapshot_ ? ReadVcharControls(*nextStageSnapshot_)
                                             : ReadVcharControls(loaded_.stage));
  gui_.setDeferredPayloadPaths({});
  gui_.setBudget(&loadCtrl_);
  gui_.setLoadOptions(&loadOpts_);
  // Viewport-only capture (no docked GUI chrome) applies to any fixed-frame
  // --screenshot run, windowed or headless: a windowed run's docked "Viewport"
  // panel gets whatever fraction of the window ImGui's dock builder assigns it
  // on that frame, which on the first windowed frame(s) can be a transient tiny
  // size before the dock layout settles (see the LOD vpReady guard above for the
  // same underlying issue) -- that shrank --screenshot captures to a few dozen
  // pixels instead of the requested window size. --window-shot deliberately
  // wants the full docked window, so it opts out.
  gui_.setCaptureViewportOnly(maxFrames >= 0 && streamHttpPort_ <= 0 &&
                              windowShot_.empty());
  // Route the GUI's GPU side-effects (viewport resize, instance visibility) to the
  // render thread; runs inline on the single-threaded path.
  gui_.setPostGpu([this](std::function<void()> op) { postGpu(std::move(op)); });
  // Only the truly-interactive run-until-quit loop offloads per-instance culling to
  // a worker (UI responsiveness). Any fixed-frame-count run (headless or windowed
  // --frames/--screenshot) culls synchronously so screenshots stay deterministic.
  gui_.setCullAsync(maxFrames < 0);
  initializeLiveShaderSources();

#if defined(LUSDVIEW_HAVE_MCP)
  // Start the embedded MCP server (tool calls are drained on the main thread).
  if (mcpStdio_ || mcpHttpPort_ > 0) {
    mcp_ = std::make_unique<MCPServer>(this);
    if (mcpHttpPort_ > 0) {
      mcpHttpRunning_ = mcp_->startHttp(mcpHttpHost_, mcpHttpPort_);
      mcpHttpStatus_ = mcpHttpRunning_ ? "MCP HTTP server started"
                                       : "Failed to start MCP HTTP server";
    }
    if (mcpStdio_) mcp_->startStdio();
  }
#else
  if (mcpStdio_ || mcpHttpPort_ > 0) {
    LOGW("MCP requested but not compiled in (build with -DLUSDVIEW_ENABLE_MCP=ON).");
  }
#endif

  // WebSocket image-streaming server (independent of MCP; own port).
  if (streamHttpPort_ > 0) {
    streamServer_ = std::make_unique<StreamServer>();
    if (!streamServer_->start(streamHttpPort_)) streamServer_.reset();
  }

  if (!initialFile.empty()) {
    // Headless (--frames) loads synchronously so screenshots are deterministic;
    // interactive runs load on a worker thread to keep the window responsive.
    if (maxFrames >= 0) {
      loadFileBlocking(initialFile);
    } else {
      startLoadAsync(initialFile);
    }
  }

  int frameCount = 0;
  std::vector<float> pathConvergencePrevious;
  uint32_t pathConvergenceLastSample = 0;
  if (!modeSweep_.empty()) setRenderMode(modeSweep_[0].second);
  bool running = true;
  bool escapeWasDown = false;
  bool haveFirstEscape = false;
  std::chrono::steady_clock::time_point firstEscapeTime;
  while (running) {
    const auto uiFrameStart = std::chrono::steady_clock::now();
    // Apply a browser-requested headless composite resize (queued last frame):
    // recreate the VK swap at the new size and update the size ImGui draws at.
    if (headless_ && streamResizeW_ > 0 && streamResizeH_ > 0) {
      if (renderer_->resizeHeadless(streamResizeW_, streamResizeH_)) {
        winW = streamResizeW_;
        winH = streamResizeH_;
        headlessWinW_ = winW;
        headlessWinH_ = winH;
        markStreamActivity();  // re-render at the new size
      }
      streamResizeW_ = streamResizeH_ = 0;
    }
    if (headless_) {
      // No platform backend: drive ImGui's display size + timestep ourselves.
      ImGuiIO& hio = ImGui::GetIO();
      hio.DisplaySize = ImVec2(static_cast<float>(winW), static_cast<float>(winH));
      hio.DeltaTime = 1.0f / 60.0f;
    } else {
      if (glfwWindowShouldClose(window_)) break;
      glfwPollEvents();
    }

    // Drain loader-produced geometry before testing for completion. The worker
    // may already be done while terminal events are still behind mesh batches.
    drainProgressiveLoad();
    finishLoadIfReady();
    stepProgressiveUpload();
    finishReconvertIfReady();  // swap in re-evaluated animation geometry

    // Advance the playback clock and request a re-evaluation at the new time.
    // Headless renders a fixed --time frame unless --play asked for playback --
    // then the fixed 1/60 step below keeps it just as deterministic.
    if (!headless_ || playRequested_ || animPlaying_) {
      // --play: start once the scene (and its timeline) is in. One-shot, so the
      // user can still pause from the Timeline panel afterwards.
      if (playRequested_ && loaded_.ok && hasAnimation_ && !loadActive_) {
        animPlaying_ = true;
        playRequested_ = false;
      }
      const auto now = std::chrono::steady_clock::now();
      float dt = haveLastFrameTime_
                     ? std::chrono::duration<float>(now - lastFrameTime_).count()
                     : 0.0f;
      lastFrameTime_ = now;
      haveLastFrameTime_ = true;
      if (dt > 0.1f) dt = 0.1f;  // clamp after stalls/load hitches
      // Fixed-frame runs step deterministically (frame N = N/60 s of playback),
      // so a --play --frames --screenshot capture is pixel-comparable across
      // runs and machines -- wall-clock dt would land on a different pose every
      // run.
      if (maxFrames >= 0) dt = 1.0f / 60.0f;
      advancePlayback(dt);
    }
    if (renderer_ && renderer_->rayTracingActive() != lastRtActiveForSkinning_) {
      updateSkinningEffective();
      if (skinningEffective_ != SkinningMode::GPU && hasAnimation_) {
        requestReconvert(animTime_);
      }
    }
    updateGpuSkinningFrameIfNeeded();
    maybeReconvertForManualBlend();

    // Feed the GUI the current playback state (drawn this frame).
    Gui::TimelineInfo tl;
    tl.hasAnimation = hasAnimation_;
    tl.start = animStart_;
    tl.end = animEnd_;
    tl.fps = animFps_;
    tl.current = animTime_;
    tl.applied = (skinningEffective_ == SkinningMode::GPU &&
                  std::isfinite(skinFrameTime_))
                     ? skinFrameTime_
                     : reconvApplied_;
    tl.playing = animPlaying_;
    tl.converting = reconvActive_;
    // Must precede setTimeline(): that triggers rebuildSubsetHighlight(), which
    // needs to know whether draw_ currently holds REST or already-POSED
    // vertices (poseNextDrawForTracer leaves it posed after an RT build).
    gui_.setDrawIsPosed(std::isfinite(nextTracerPosedTime_) &&
                        (activeOverlay_ == OverlayKind::CudaRT ||
                         activeOverlay_ == OverlayKind::CudaPathTrace ||
                         activeOverlay_ == OverlayKind::HipRT ||
                         activeOverlay_ == OverlayKind::HipPathTrace ||
                         activeOverlay_ == OverlayKind::CpuRT));
    gui_.setTimeline(tl);
    Gui::SkinningInfo si;
    si.requested = skinningRequested_;
    si.effective = skinningEffective_;
    si.reason = skinningReason_;
    gui_.setSkinning(si);
    gui_.setActiveTechnique(activeTechnique_);
    if (!computeDevicesEnumerated_) {
      std::string ignored;
      CudaRayTracer::enumerateDevices(&cudaDevices_, &ignored);
      ignored.clear();
      HipRayTracer::enumerateDevices(&hipDevices_, &ignored);
      computeDevicesEnumerated_ = true;
    }
    gui_.setComputeDevices(cudaDevices_, cudaTracer_.deviceIndex(),
                           hipDevices_, hipTracer_.deviceIndex());
    gui_.setCudaRtBackend(cudaTracer_.rtBackend(),
                          cudaTracer_.optixAvailable());
    gui_.setTechniqueAvailability(cudaProbe_ != ProbeState::Unavailable,
                                  hipProbe_ != ProbeState::Unavailable,
                                  vulkanAvailable_, vulkanRtAvailable_);

    // Feed the GUI the current load status for the loading modal.
    Gui::LoadStatus ls;
    ls.active = loadActive_;
    ls.path = loadingPath_;
    ls.meshesDone = loadCtrl_.meshesDone.load();
    ls.meshesTotal = loadCtrl_.meshesTotal.load();
    ls.payloadsDone = loadCtrl_.payloadsDone.load();
    ls.payloadsTotal = loadCtrl_.payloadsTotal.load();
    ls.texturesDone = loadCtrl_.texturesDone.load();
    ls.texturesTotal = loadCtrl_.texturesTotal.load();
    ls.stage = loadCtrl_.stage.load();
    ls.detailPhase = loadCtrl_.detailPhase.load();
    ls.phaseProgress = static_cast<float>(loadCtrl_.phasePermille.load()) / 1000.0f;
    ls.elapsed = loadActive_ ? std::chrono::duration<float>(
                                   std::chrono::steady_clock::now() - loadStart_)
                                   .count()
                             : 0.0f;
    gui_.setLoadStatus(ls);

    // GPU-side progress overlay: raster geometry/texture streaming + RT build.
    // Set the RT-build note BEFORE gui_.frame() builds the overlay, so the
    // announce frame (rendered just before the blocking build) carries it.
    rtBuildNote_.clear();
    if (hipInteractive_ && !hipInteractiveBuilt_ && !loadActive_ &&
        !draw_.empty()) {
      if (hipBuildStarted_) {
        const int ph = hipBuildProgress_.phase.load(std::memory_order_relaxed);
        const size_t d = hipBuildProgress_.done.load(std::memory_order_relaxed);
        const size_t t = hipBuildProgress_.total.load(std::memory_order_relaxed);
        const float el = std::chrono::duration<float>(
                             std::chrono::steady_clock::now() - hipBuildStart_)
                             .count();
        char buf[192];
        if (t > 0)
          std::snprintf(buf, sizeof(buf),
                        "Building ray-tracing scene \xE2\x80\x94 %s %zu/%zu  (%.0fs)",
                        BuildProgress::phaseName(ph), d, t, el);
        else
          std::snprintf(buf, sizeof(buf),
                        "Building ray-tracing scene \xE2\x80\x94 %s  (%.0fs)",
                        BuildProgress::phaseName(ph), el);
        rtBuildNote_ = buf;
      } else {
        rtBuildNote_ = "Building ray-tracing scene\xE2\x80\xA6";
      }
    }
    Gui::UploadStatus us;
    us.active = streamLoadActive_ || progressiveActive_;
    us.meshesDone = streamLoadActive_
                        ? static_cast<size_t>(std::max(0, renderer_->meshCount()))
                        : nextMesh_;
    us.meshesTotal = streamLoadActive_
                         ? static_cast<size_t>(std::max<long long>(
                               us.meshesDone, loadCtrl_.meshesTotal.load()))
                         : draw_.meshes.size();
    us.texDone = nextTex_;
    us.texTotal = draw_.textures.size();
    us.volDone = nextVolume_;
    us.volTotal = draw_.volumes.size();
    us.note = rtBuildNote_;
    gui_.setUploadStatus(us);

    // Apply browser input BEFORE NewFrame so injected mouse/keyboard events reach
    // ImGui this frame (widgets become clickable); camera drags are applied here
    // too. Runs on the main thread, like the MCP drain.
    if (streamServer_) {
      for (const StreamNav& c : streamServer_->takeInput()) applyNavCommand(c);
    }

    // Backend switches requested during the PREVIOUS frame are applied here, in
    // the gap between ImGui frames. Doing it mid-frame (where the request is
    // consumed) tears down the ImGui renderer backend after ImGui::NewFrame()
    // has already run: the draw data would then be built by one backend and
    // submitted to another, whose per-frame device objects were never created
    // and whose ImTextureIDs mean nothing -- observed as a SIGSEGV inside
    // ImGui_ImplOpenGL3_RenderDrawData when switching Vulkan -> GL.
    if (hasPendingTechnique_) {
      hasPendingTechnique_ = false;
      applyTechniqueSwitch(pendingTechnique_);
    }

    // In threaded GL, newFrame() is a GL op that runs on the render thread (just
    // before it draws the packet); the main thread only builds ImGui + the packet.
    if (!renderThreadActive_) renderer_->newFrame();
    if (!headless_) ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    pollPrototypeLodBuilds();

    // Raster view-dependent LOD (--raster-lod): drop sub-pixel instances + box
    // proxies on the raster instanced path. Applied before gui_.frame() (which runs
    // the instance cull). Idempotent; cheap.
    gui_.setRasterLod(rasterLodEnabled_, rasterLodFullPx_, rasterLodCullPx_);

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
    if (renderThreadActive_) {
      renderFps_ = measuredRenderFps_.load(std::memory_order_relaxed);
    }
#endif
    const bool adaptiveActive = adaptiveQuality_ && renderThreadActive_ &&
                                backend_ == Backend::Vulkan && !headless_;
    if (!adaptiveActive || !gui_.cameraInteractive()) {
      adaptiveRenderScale_ = 1.0f;
      adaptiveTier_ = 0;
      adaptiveOverBudgetFrames_ = 0;
      adaptiveUnderBudgetFrames_ = 0;
    } else if (renderFps_ > 0.0f) {
      if (renderFps_ < adaptiveTargetFps_ * 0.90f) {
        ++adaptiveOverBudgetFrames_;
        adaptiveUnderBudgetFrames_ = 0;
      } else if (renderFps_ > adaptiveTargetFps_ * 1.25f) {
        ++adaptiveUnderBudgetFrames_;
        adaptiveOverBudgetFrames_ = 0;
      } else {
        adaptiveOverBudgetFrames_ = adaptiveUnderBudgetFrames_ = 0;
      }
      if (adaptiveOverBudgetFrames_ >= 3) {
        if (adaptiveTier_ == 0) {
          adaptiveTier_ = 1;
          adaptiveRenderScale_ = std::max(adaptiveMinScale_, 0.75f);
        } else {
          adaptiveTier_ = 2;
          adaptiveRenderScale_ = adaptiveMinScale_;
        }
        adaptiveOverBudgetFrames_ = 0;
      } else if (adaptiveUnderBudgetFrames_ >= 30 && adaptiveTier_ > 0) {
        --adaptiveTier_;
        adaptiveRenderScale_ = adaptiveTier_ == 0
            ? 1.0f : std::max(adaptiveMinScale_, 0.75f);
        adaptiveUnderBudgetFrames_ = 0;
      }
    }
    gui_.setAdaptiveQuality(adaptiveActive, adaptiveRenderScale_, adaptiveTier_,
                            adaptiveTargetFps_);
    const float effectiveFps =
        renderThreadActive_ ? renderFps_ : ImGui::GetIO().Framerate;
    if (dynamicPrototypeLodEnabled_ && backend_ == Backend::Vulkan &&
        !headless_ && effectiveFps > 0.0f) {
      const float target = gui_.cameraInteractive() ? 30.0f : 10.0f;
      if (effectiveFps < target * 0.90f) {
        ++dynamicLodSlowFrames_;
        dynamicLodFastFrames_ = 0;
      } else if (effectiveFps > target * 1.20f) {
        ++dynamicLodFastFrames_;
        dynamicLodSlowFrames_ = 0;
      } else {
        dynamicLodSlowFrames_ = dynamicLodFastFrames_ = 0;
      }
      if (dynamicLodSlowFrames_ >= 3 && dynamicPrototypeLodTier_ < 3) {
        ++dynamicPrototypeLodTier_;
        LOGI("dynamic prototype LOD: tier %d (%.1f FPS, target %.1f)",
             dynamicPrototypeLodTier_, effectiveFps, target);
        dynamicLodSlowFrames_ = 0;
      } else if (dynamicLodFastFrames_ >= 30 &&
                 dynamicPrototypeLodTier_ > 0) {
        --dynamicPrototypeLodTier_;
        LOGI("dynamic prototype LOD: tier %d (%.1f FPS, target %.1f)",
             dynamicPrototypeLodTier_, effectiveFps, target);
        dynamicLodFastFrames_ = 0;
      }
    } else {
      dynamicPrototypeLodTier_ = 0;
      dynamicLodSlowFrames_ = dynamicLodFastFrames_ = 0;
    }
    static constexpr float kPrototypeErrorPx[4] = {0.0f, 0.75f, 1.5f,
                                                   3.0f};
    gui_.setDynamicRasterLod(dynamicPrototypeLodEnabled_,
                             kPrototypeErrorPx[dynamicPrototypeLodTier_]);
    gui_.setDynamicRasterLodTier(dynamicPrototypeLodTier_);
    gui_.setFrameRates(effectiveFps,
                       renderThreadActive_);

    gui_.setMcpServerState(
#if defined(LUSDVIEW_HAVE_MCP)
        true,
#else
        false,
#endif
        mcpHttpRunning_, mcpHttpHost_, mcpHttpPort_ > 0 ? mcpHttpPort_ : 8080,
        mcpHttpStatus_);
    gui_.frame(renderer_.get(), &camera_);
    if (!droppedEnvmapPath_.empty()) {
      gui_.setDomeFilePath(droppedEnvmapPath_);
      droppedEnvmapPath_.clear();
    }
    Gui::DomeLightRequest domeRequest;
    if (gui_.consumeDomeLightRequest(&domeRequest)) {
      applyDomeLightRequest(domeRequest);
    }
#if defined(LUSDVIEW_HAVE_MCP)
    std::string requestedMcpHost;
    int requestedMcpPort = 0;
    if (gui_.consumeMcpStartRequest(&requestedMcpHost, &requestedMcpPort)) {
      if (!mcp_) mcp_ = std::make_unique<MCPServer>(this);
      mcpHttpHost_ = requestedMcpHost;
      mcpHttpPort_ = requestedMcpPort;
      mcpHttpRunning_ = mcp_->startHttp(mcpHttpHost_, mcpHttpPort_);
      mcpHttpStatus_ = mcpHttpRunning_ ? "MCP HTTP server started"
                                       : "Failed to bind MCP HTTP server";
    }
    if (gui_.consumeMcpStopRequest() && mcp_ && mcpHttpRunning_) {
      mcp_->stopHttp();
      mcpHttpRunning_ = false;
      mcpHttpStatus_ = "MCP HTTP server stopped";
    }
    if (gui_.consumeAutoriggerRequest() && !autoriggerFuture_.valid() &&
        !autoriggerExecutable_.empty() && !loaded_.filepath.empty()) {
      autoriggerOutput_ = (std::filesystem::temp_directory_path() /
          ("vchar-lightrig-gui-" + std::to_string(sceneGen_) + ".usda")).string();
      const std::string executable = autoriggerExecutable_;
      const std::string asset = loaded_.filepath;
      const std::string output = autoriggerOutput_;
      gui_.setAutoriggerStatus("Fitting landmarks and authoring USD rig", true);
      autoriggerFuture_ = std::async(std::launch::async,
          [executable, asset, output] {
            return vchar::RunAutorigger(executable, asset, output,
                                        std::chrono::milliseconds(30000));
          });
    }
    if (autoriggerFuture_.valid() &&
        autoriggerFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
      const vchar::AutoriggerResult completed = autoriggerFuture_.get();
      if (!completed.ok) {
        gui_.setAutoriggerStatus(completed.timedOut ? "Auto-rigger timed out" :
                                 "Auto-rigger failed: " + completed.error, false);
      } else {
        std::string applyError;
        mcpVirtualHuman("apply_rig_overlay", {{"path", autoriggerOutput_}},
                        applyError);
        gui_.setAutoriggerStatus(applyError.empty() ?
            "Rig overlay applied; review landmarks and facial controls" :
            "Overlay application failed: " + applyError, false);
      }
    }
    std::string dnaPath;
    if (gui_.consumeDnaInspectRequest(&dnaPath) && !dnaWorkerFuture_.valid() &&
        !autoriggerExecutable_.empty()) {
      const std::string executable = autoriggerExecutable_;
      const std::string request = nlohmann::json{{"jsonrpc","2.0"},{"id",2},
          {"method","dna.inspect"},{"params",{{"asset_path",dnaPath}}}}.dump();
      gui_.setDnaWorkerStatus("Reading DNA metadata", true);
      dnaWorkerFuture_ = std::async(std::launch::async, [executable, request] {
        return vchar::RunWorkerRequest(executable, request,
                                       std::chrono::milliseconds(30000));
      });
    }
    if (dnaWorkerFuture_.valid() &&
        dnaWorkerFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
      const vchar::AutoriggerResult completed = dnaWorkerFuture_.get();
      gui_.setDnaWorkerStatus(completed.ok ? "DNA: " + completed.response :
          (completed.timedOut ? "DNA inspection timed out" :
           "DNA inspection failed: " + completed.error), false);
    }
#endif
    // Camera-panel focus edits must also reach the standalone CPU/CUDA/HIP
    // viewport tracers and the render report; Vulkan/GL consume this same lens
    // directly from Gui::renderViewportScene().
    cameraLens_ = gui_.cameraLens();
    pathTrace_ = gui_.pathTraceSettings();
    pathTrace_.sanitize();
    ensureWireAuxReady();

    // View-dependent RT LOD: re-classify the instance set when the camera settles.
    updateRtLodCamera();

    // Actions generated by the UI. Timeline/skinning actions are applied before
    // rendering the viewport texture so they can affect this present.
    const bool reload = gui_.wantReload();
    const bool open = gui_.wantOpen();
    const bool openRecent = gui_.wantOpenRecent();
    const std::string recentPath = gui_.recentToOpen();
    const bool quit = gui_.wantQuit();
    const bool cancelLoad = gui_.wantCancelLoad();
    const bool loadAllPayloads = gui_.wantLoadAllPayloads();
    std::vector<std::string> payloadReqs = gui_.takePayloadLoadRequests();
    const bool variantSwitch = gui_.wantVariantSwitch();
    const auto variantOverrides = gui_.variantOverrides();
    // Timeline actions.
    const bool togglePlay = gui_.wantTogglePlay();
    const bool stopPlay = gui_.wantStop();
    const bool hasSeek = gui_.hasSeek();
    const double seekTime = gui_.seekTime();
    const bool hasSkinningModeRequest = gui_.hasSkinningModeRequest();
    const SkinningMode requestedSkinningMode = gui_.requestedSkinningMode();
    const bool hasTechniqueRequest = gui_.hasTechniqueRequest();
    const RenderTechnique requestedTechnique = gui_.requestedTechnique();
    const bool hasCudaDeviceRequest = gui_.hasCudaDeviceRequest();
    const bool hasHipDeviceRequest = gui_.hasHipDeviceRequest();
    const bool hasCudaRtBackendRequest = gui_.hasCudaRtBackendRequest();
    const CudaRtBackend requestedCudaRtBackend =
        gui_.requestedCudaRtBackend();
    const int requestedCudaDevice = gui_.requestedCudaDevice();
    const int requestedHipDevice = gui_.requestedHipDevice();
    const bool wantToggleCpuRt = gui_.wantToggleCpuRt();
    Gui::OpenPbrMaterialEdit openPbrEdit;
    if (gui_.takeOpenPbrMaterialEdit(&openPbrEdit)) {
      pendingOpenPbrEdit_ = openPbrEdit;
      hasPendingOpenPbrEdit_ = true;
    }
    animLoop_ = gui_.loopPlayback();
    animSpeed_ = gui_.playSpeed();
    tessQuality_ = gui_.tessellationQuality();
    gui_.clearActions();

    if (hasCudaRtBackendRequest &&
        requestedCudaRtBackend != cudaTracer_.rtBackend()) {
      cudaTracer_.setRtBackend(requestedCudaRtBackend);
      cudaPathAccum_.invalidate();
      LOGI("CUDA RT traversal selected dynamically: %s",
           requestedCudaRtBackend == CudaRtBackend::Optix
               ? "OptiX"
               : (requestedCudaRtBackend == CudaRtBackend::SoftwareBvh
                      ? "software BVH"
                      : "auto"));
    }

    const bool tracerBuildActive =
        (hipBuildStarted_ && !hipBuildDone_.load(std::memory_order_acquire)) ||
        (cudaBuildStarted_ && !cudaBuildDone_.load(std::memory_order_acquire)) ||
        (cpuBuildStarted_ && !cpuBuildDone_.load(std::memory_order_acquire)) ||
        (hipTraceFuture_.valid() &&
         hipTraceFuture_.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready) ||
        (cudaTraceFuture_.valid() &&
         cudaTraceFuture_.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready);
    if ((hasCudaDeviceRequest || hasHipDeviceRequest) && tracerBuildActive) {
      LOGW("compute RT device change ignored while a scene build is active");
    } else if (hasCudaDeviceRequest &&
               requestedCudaDevice != cudaTracer_.deviceIndex()) {
      if (cudaBuildThread_.joinable()) cudaBuildThread_.join();
      std::string deviceErr;
      if (cudaTracer_.selectDevice(requestedCudaDevice, &deviceErr)) {
        cudaProbe_ = ProbeState::Available;
        cudaInteractiveBuilt_ = false;
        cudaPathAccum_.invalidate();
        cudaBuildStarted_ = false;
        cudaBuildDone_.store(false, std::memory_order_release);
        cudaBuildOk_.store(false, std::memory_order_release);
        cudaBuildAnnounceFrames_ = 0;
        LOGI("CUDA RT device selected dynamically: %s",
             cudaTracer_.deviceName());
      } else {
        LOGW("CUDA RT device selection failed: %s", deviceErr.c_str());
      }
    } else if (hasHipDeviceRequest &&
               requestedHipDevice != hipTracer_.deviceIndex()) {
      if (hipBuildThread_.joinable()) hipBuildThread_.join();
      std::string deviceErr;
      if (hipTracer_.selectDevice(requestedHipDevice, &deviceErr)) {
        hipProbe_ = ProbeState::Available;
        hipInteractiveBuilt_ = false;
        hipPathAccum_.invalidate();
        hipBuildStarted_ = false;
        hipBuildDone_.store(false, std::memory_order_release);
        hipBuildOk_.store(false, std::memory_order_release);
        hipBuildAnnounceFrames_ = 0;
        LOGI("HIP RT device selected dynamically: %s",
             hipTracer_.deviceName());
      } else {
        LOGW("HIP RT device selection failed: %s", deviceErr.c_str());
      }
    }
    if (hasPendingOpenPbrEdit_ && !tracerBuildActive) {
      if (pendingOpenPbrEdit_.materialId < 0 ||
          static_cast<size_t>(pendingOpenPbrEdit_.materialId) >=
              draw_.materials.size()) {
        hasPendingOpenPbrEdit_ = false;
      } else {
        const int materialId = pendingOpenPbrEdit_.materialId;
        DrawMaterialCPU& material =
            draw_.materials[static_cast<size_t>(materialId)];
        if (pendingOpenPbrEdit_.makeConstant) {
          MakeConstantOpenPBRMaterial(&material);
        }
        ApplyOpenPBRMaterialConstants(&material,
                                      pendingOpenPbrEdit_.constants);
        const DrawMaterialCPU materialCopy = material;
        const uint64_t rendererGeneration = rendererGeneration_;
        postGpu([this, rendererGeneration, materialId, materialCopy] {
          if (!renderer_ || rendererGeneration != rendererGeneration_) return;
          std::string err;
          if (!renderer_->updateMaterialConstants(materialId, materialCopy,
                                                  &err) &&
              !err.empty()) {
            LOGW("live OpenPBR renderer update: %s", err.c_str());
          }
        });
        std::string editErr;
        if (cudaInteractiveBuilt_ &&
            !cudaTracer_.updateMaterialConstants(materialId, material,
                                                 &editErr)) {
          LOGW("live OpenPBR CUDA update: %s", editErr.c_str());
        }
        cudaPathAccum_.invalidate();
        editErr.clear();
        if (hipInteractiveBuilt_ &&
            !hipTracer_.updateMaterialConstants(materialId, material,
                                                &editErr)) {
          LOGW("live OpenPBR HIP update: %s", editErr.c_str());
        }
        hipPathAccum_.invalidate();
        editErr.clear();
        if (cpuInteractiveBuilt_ &&
            !cpuTracer_.updateMaterialConstants(materialId, material,
                                                &editErr)) {
          LOGW("live OpenPBR CPU update: %s", editErr.c_str());
        }
        hasPendingOpenPbrEdit_ = false;
      }
    }

    // Queue only -- see the apply site at the top of the next iteration.
    if (hasTechniqueRequest) {
      pendingTechnique_ = requestedTechnique;
      hasPendingTechnique_ = true;
    }
    if (wantToggleCpuRt) {
      pendingTechnique_ = (activeTechnique_ == RenderTechnique::CpuRT)
                              ? previousTechnique_
                              : RenderTechnique::CpuRT;
      hasPendingTechnique_ = true;
    }

    if (hasSkinningModeRequest) {
      skinningRequested_ = requestedSkinningMode;
      updateSkinningEffective();
      if (skinningEffective_ == SkinningMode::GPU) {
        cancelAndJoinReconvert();
        updateGpuSkinningFrameIfNeeded();
      }
      else if (hasAnimation_) requestReconvert(animTime_);
    }

    // Timeline: play/pause, stop (reset to start), step, and scrub.
    if (hasAnimation_) {
      if (togglePlay) animPlaying_ = !animPlaying_;
      if (stopPlay) {
        animPlaying_ = false;
        animTime_ = animStart_;
        requestReconvert(animTime_);
      }
      const bool stepFwd = gui_.wantStepForward();
      const bool stepBwd = gui_.wantStepBackward();
      if (stepFwd || stepBwd) {
        animPlaying_ = false;
        double frameStep = 1.0 / animFps_;
        if (stepFwd) animTime_ += frameStep;
        if (stepBwd) animTime_ -= frameStep;
        if (animTime_ < animStart_) animTime_ = animStart_;
        if (animTime_ > animEnd_) animTime_ = animEnd_;
        requestReconvert(animTime_);
      }
      if (hasSeek) {
        animTime_ = seekTime;
        if (animTime_ < animStart_) animTime_ = animStart_;
        if (animTime_ > animEnd_) animTime_ = animEnd_;
        requestReconvert(animTime_);
      }
    }
    updateGpuSkinningFrameIfNeeded();
    // Keep the camera's auto-clip scene bounds in step with the CURRENT pose.
    // updateGpuSkinningFrameIfNeeded() (via updateNextDeformFrameIfNeeded's
    // BuildNextPosedSceneBounds) refreshes draw_.aabbMin/aabbMax every frame
    // the --next loader's animation advances, but camera_.setSceneBounds()
    // was previously only called once at load time -- so an animated pose
    // that grows past the load-time (e.g. rest/first-frame) bounding sphere
    // had its sceneRadius_/sceneCenter_ (and therefore the auto near/far
    // planes) frozen at the stale box, clipping the mesh as it moved past it.
    if (draw_.hasBounds) camera_.setSceneBounds(draw_.aabbMin, draw_.aabbMax);

    // Render after same-frame action consumption but before ImGui submit. The
    // viewport window already emitted ImGui::Image with the texture handle; both
    // GL and Vulkan sample the texture contents later during present().
    const bool checkpointDue =
        checkpointEvery_ > 0 && ((frameCount + 1) % checkpointEvery_ == 0);
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
    if (renderThreadActive_) {
      // Build the CPU-side frame packet (camera/params + compacted instance data)
      // on the main thread, deep-copy the ImGui draw lists, and hand it to the
      // render thread. The main loop never touches GL or blocks on the GPU.
      auto pkt = std::make_unique<FramePacket>();
      gui_.renderViewportScene(pkt.get());
      updateTextureResidency();
      glfwGetFramebufferSize(window_, &pkt->fbW, &pkt->fbH);
      pkt->wantCapture = checkpointDue ||
          (maxFrames >= 0 && frameCount == maxFrames - 1 && !screenshot.empty());
      pkt->seq = ++pktSubmitSeq_;
      pkt->sceneKey = FrameSceneKey(*pkt);
      ImGui::Render();
      pkt->drawData = CloneImDrawData(ImGui::GetDrawData());
      // Fixed-frame runs block until the render thread has drawn this packet so
      // the screenshot is deterministic; interactive runs never wait.
      submitFramePacket(std::move(pkt), maxFrames >= 0);
    } else
#endif
    {
      // Skip the raster scene draw (and its instance culling) when the RT path
      // owns the screenshot -- only the cheap ImGui composite needs to run.
      // Windowed --hip/--cuda (or a live switch to HipRT/CudaRT) traces the
      // viewport with the HIP/CUDA path instead of raster.
      if (hipInteractive_) {
        if (!renderHipViewport()) {
          // Device unavailable / build failed: renderHipViewport() already
          // cleared hipInteractive_ -- sync the technique state (menu, probe
          // cache) so this doesn't keep getting offered/retried.
          hipProbe_ = ProbeState::Unavailable;
          activeOverlay_ = OverlayKind::None;
          activeTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                        : RenderTechnique::VulkanRaster;
        }
      } else if (cudaInteractive_) {
        if (!renderCudaViewport()) {
          cudaProbe_ = ProbeState::Unavailable;
          activeOverlay_ = OverlayKind::None;
          activeTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                        : RenderTechnique::VulkanRaster;
        }
      } else if (cpuInteractive_) {
        if (!renderCpuViewport()) {
          activeOverlay_ = OverlayKind::None;
          activeTechnique_ = (backend_ == Backend::GL) ? RenderTechnique::GLRaster
                                                        : RenderTechnique::VulkanRaster;
        }
      } else if (!rtOwnsScreenshot_ &&
                 !(quitAfterFullUpload_ && streamFirstFrameLogged_ &&
                   loadActive_)) {
        gui_.renderViewportScene();
        if (!quitAfterConvert_) {
          updateTextureResidency();
          updatePtexResidency();
        }
      }

      // Grab the composited window on the final frame (--window-shot).
      if (!windowShot_.empty() && maxFrames >= 0 && frameCount == maxFrames - 1) {
        renderer_->requestWindowCapture();
      }
      // Streaming: send a small low-quality JPEG while the view is moving, then a
      // single full-resolution lossless (PNG/QOI) frame once it goes stable. Skip
      // sending entirely once the refined frame is out and nothing has changed.
      bool streamSend = false, streamMotion = false;
      if (streamServer_) {
        const int cc = streamServer_->clientCount();
        if (cc > 0) {
          if (cc > streamPrevClientCount_) markStreamActivity();  // greet new client
          const auto now = std::chrono::steady_clock::now();
          const long idleMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - streamLastActivity_).count();
          streamMotion = progressiveActive_ || animPlaying_ ||
                         idleMs < streamIdleMs_;
          streamSend = streamMotion || !streamHiQSent_;
        }
        streamPrevClientCount_ = cc;
      }
      if (streamSend) renderer_->requestWindowCapture();

      ImGui::Render();
      static const bool timeFrame = std::getenv("LUSDVIEW_TIME_FRAME") != nullptr;
      const auto tp0 = std::chrono::steady_clock::now();
      renderer_->present();

      const bool usefulGeometryThreshold =
          streamUploadedEffectiveTriangles_ >= 100000 ||
          (streamCompleteSeen_ && streamUploadedEffectiveTriangles_ > 0);
      if (!streamFirstFrameLogged_ && streamHasUsefulGeometry_ &&
          usefulGeometryThreshold && streamCameraFramed_) {
        streamFirstFrameLogged_ = true;
        if (loadOpts_.timing) {
          LOGI("timing: first useful frame %.3f s (%zu unique, %zu effective "
               "uploaded triangles, %d draws)",
               std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            runStart_)
                   .count(),
               streamUploadedTriangles_, streamUploadedEffectiveTriangles_,
               renderer_->meshCount());
        }
      }
      if (!streamFullUploadLogged_ && streamCompleteSeen_ && !loadActive_ &&
          !progressiveActive_ && renderer_->meshCount() > 0) {
        streamFullUploadLogged_ = true;
        if (loadOpts_.timing) {
          LOGI("timing: full scene uploaded and presented %.3f s",
               std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            runStart_)
                   .count());
        }
        if (quitAfterFullUpload_) quitAfterFullPresent_ = true;
      }

      if (streamSend) {
        std::vector<uint8_t> rgba;
        int cw = 0, ch = 0;
        if (renderer_->captureWindow(&rgba, &cw, &ch) && cw > 0 && ch > 0) {
          streamEncodeAndPush(std::move(rgba), cw, ch, streamMotion);
          if (!streamMotion) streamHiQSent_ = true;  // refined this idle period
        }
      }
      if (timeFrame) {
        const double pms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - tp0)
                               .count();
        std::fprintf(stderr, "[frame] present(GPU+readback)=%.1fms\n", pms);
      }
    }

    if (checkpointDue) {
      std::vector<uint8_t> rgba;
      int captureW = 0, captureH = 0;
      bool captured = false;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
      if (renderThreadActive_) {
        rgba = renderCapture_;
        captureW = renderCaptureW_;
        captureH = renderCaptureH_;
        captured = !rgba.empty();
      } else
#endif
      {
        captured = renderer_->captureViewport(&rgba, &captureW, &captureH);
      }
      std::string checkpointPath = checkpointPattern_;
      char frameText[16];
      std::snprintf(frameText, sizeof(frameText), "%06d", frameCount + 1);
      checkpointPath.replace(checkpointPath.find("{frame}"), 7, frameText);
      std::error_code checkpointDirError;
      const std::filesystem::path checkpointFile(checkpointPath);
      if (!checkpointFile.parent_path().empty()) {
        std::filesystem::create_directories(checkpointFile.parent_path(),
                                            checkpointDirError);
      }
      std::string checkpointErr;
      if (!checkpointDirError && captured &&
          WriteScreenshotImage(checkpointPath, rgba, captureW,
                                           captureH, &checkpointErr)) {
        ++checkpointCount_;
        LOGI("wrote checkpoint %s (%dx%d, frame %d)",
             checkpointPath.c_str(), captureW, captureH, frameCount + 1);
      } else {
        LOGW("failed to write checkpoint %s: %s", checkpointPath.c_str(),
             checkpointDirError
                 ? checkpointDirError.message().c_str()
                 : (captured ? checkpointErr.c_str() : "capture unavailable"));
      }
    }

    // Deferred actions (after the frame, outside the ImGui frame state).
#if defined(LUSDVIEW_HAVE_MCP)
    if (mcp_) mcp_->drain();  // run queued MCP tool calls on the main thread
#endif
    pollLiveShaderReload();
    if (cancelLoad) loadCtrl_.cancel.store(true);
    if (reload && !loaded_.filepath.empty()) startLoadAsync(loaded_.filepath);
    if (open && !headless_) openFileDialog();
    if (openRecent && !headless_ && !recentPath.empty()) {
      startLoadAsync(recentPath);
    }

    // Lazy payload on-demand load: recompose with the requested payloads added.
    // Skipped while a load is in flight (loaded_ would be the outgoing scene).
    if (!loadActive_ && (loadAllPayloads || !payloadReqs.empty())) {
      std::set<std::string> add(payloadReqs.begin(), payloadReqs.end());
      if (loadAllPayloads) {
        if (useNextLoader_ && nextSession_) {
          for (const lightusd::next::Path& path :
               nextSession_->GetDeferredPayloadPaths()) {
            add.insert(path.str());
          }
        } else {
          for (const auto& d : loaded_.comp.deferred) add.insert(d.primPath);
        }
      }
      startRecomposeAsync(add);
    }

    // Variant switch: recompose with the user's variant selections.
    if (!loadActive_ && variantSwitch &&
        ((useNextLoader_ && nextSession_) ||
         (loaded_.comp.composed && loaded_.comp.rootLayer))) {
      loadOpts_.variantOverrides = variantOverrides;
      startRecomposeAsync(std::set<std::string>());
    }

    if (!headless_) {
      if (quit) glfwSetWindowShouldClose(window_, GLFW_TRUE);
      ImGuiIO& io = ImGui::GetIO();
      const bool ctrlDown =
          glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
          glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
      if (ctrlDown && glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
      }

      const bool escapeDown =
          glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS;
      if (!io.WantTextInput && escapeDown && !escapeWasDown) {
        constexpr auto kDoubleEscapeWindow = std::chrono::milliseconds(600);
        const auto now = std::chrono::steady_clock::now();
        if (haveFirstEscape && now - firstEscapeTime <= kDoubleEscapeWindow) {
          glfwSetWindowShouldClose(window_, GLFW_TRUE);
          haveFirstEscape = false;
        } else {
          firstEscapeTime = now;
          haveFirstEscape = true;
          LOGI("press Escape again to quit (or Ctrl+Q)");
        }
      }
      escapeWasDown = escapeDown;
      if (haveFirstEscape &&
          std::chrono::steady_clock::now() - firstEscapeTime >
              std::chrono::milliseconds(600))
        haveFirstEscape = false;
    }

    if (quitAfterFullPresent_) running = false;

    if (maxFrames >= 0) ++frameCount;
    bool fixedFrameBudgetReached = maxFrames >= 0 && frameCount >= maxFrames;
    if (maxFrames >= 0 && headless_ && pathTrace_.enabled && !cudaRt_ &&
        !hipRt_ && pathTrace_.targetSamples > 0) {
      // The first RT frame may establish the auto-fit camera or finish a TLAS
      // rebuild, both of which correctly invalidate accumulation. A production
      // sample target describes accepted samples, not UI loop iterations, so
      // keep submitting until the stable accumulation reaches the requested
      // count. The guard prevents an unexpected perpetual invalidation from
      // turning a batch render into an infinite process.
      const uint32_t accepted =
          renderer_ ? renderer_->rayTracingAccumulatedSamples() : 0u;
      fixedFrameBudgetReached = accepted >= pathTrace_.targetSamples;
      const uint32_t convergenceInterval =
          std::max(8u, pathTrace_.minSamples / 4u);
      if (!fixedFrameBudgetReached && pathTrace_.varianceThreshold > 0.0f &&
          accepted >= pathTrace_.minSamples &&
          accepted >= pathConvergenceLastSample + convergenceInterval) {
        std::vector<float> current;
        int convergenceW = 0, convergenceH = 0;
        if (renderer_->captureLinearViewport(&current, &convergenceW,
                                             &convergenceH)) {
          const double change =
              PathTraceRelativeChange(current, pathConvergencePrevious);
          pathConvergencePrevious = std::move(current);
          pathConvergenceLastSample = accepted;
          if (change <= static_cast<double>(pathTrace_.varianceThreshold)) {
            LOGI("path trace converged at %u samples (relative RMS %.6f <= %.6f)",
                 accepted, change, pathTrace_.varianceThreshold);
            fixedFrameBudgetReached = true;
          }
        }
      }
      if (!fixedFrameBudgetReached &&
          frameCount >= maxFrames + 64) {
        LOGW("path trace stopped after repeated accumulation invalidation "
             "(%u/%u accepted samples)", accepted,
             pathTrace_.targetSamples);
        fixedFrameBudgetReached = true;
      }
    }
    if (fixedFrameBudgetReached) {
      // --mode-sweep: one load, many AOVs. Capture the mode that just finished
      // its full frame budget, then switch and give the next mode the SAME
      // budget, so every image sees as many frames as it would have in its own
      // process. Only when the list is exhausted does the run end.
      if (!modeSweep_.empty() && modeSweepIndex_ < modeSweep_.size()) {
        captureModeSweepFrame(modeSweep_[modeSweepIndex_].first);
        ++modeSweepIndex_;
        if (modeSweepIndex_ < modeSweep_.size()) {
          setRenderMode(modeSweep_[modeSweepIndex_].second);
          // The iteration that captured the previous mode already advanced the
          // frame the new mode will be drawn into, so it starts at 1, not 0.
          // Starting at 0 gives every mode after the first an extra frame,
          // which is measurable: its image then matches a separate process run
          // with --frames N+1 rather than N.
          frameCount = 1;
          continue;
        }
      }
      running = false;
      if (!headless_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
    // The render thread is latest-wins and never blocks this loop. Without a
    // UI cadence the main thread spins at 1000+ Hz cloning identical ImGui draw
    // data and repeatedly rebuilding cull/visibility packets, stealing CPU and
    // memory bandwidth from curve expansion. Cap packet production to 60 Hz;
    // GLFW polling still happens at the start of every tick.
    if (renderThreadActive_ && maxFrames < 0) {
      constexpr auto kUiFrame = std::chrono::microseconds(16667);
      std::this_thread::sleep_until(uiFrameStart + kUiFrame);
    }
#endif
  }

  // Headless determinism: a manual-blend (or animation) reconvert bakes the
  // deformed geometry on a worker thread; drain it so the screenshot and any
  // ray-traced BLAS (built from draw_ below) see the posed result, not the rest
  // pose. Bounded so a stuck worker can't hang the screenshot.
  if (maxFrames >= 0) {
    for (int guard = 0; guard < 2000; ++guard) {
      finishReconvertIfReady();
      maybeReconvertForManualBlend();
      if (!reconvActive_ && !blendReconvNeeded_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // The progressive CUDA/HIP worker owns its tracer stream and pinned output
  // until the future completes.  Drain it before a deterministic one-shot
  // screenshot reuses the tracer or renderer teardown begins.
  if (cudaTraceFuture_.valid()) cudaTraceFuture_.wait();
  if (hipTraceFuture_.valid()) hipTraceFuture_.wait();

  // Headless: report the last frame's frustum-cull stats (visible vs total) so
  // large-scene culling can be measured without the interactive HUD.
  if (maxFrames >= 0) {
    const Gui::RenderStats rs = gui_.renderStats();
    LOGI("render stats: meshes %zu/%zu visible, instances %zu/%zu visible, "
         "LOD proxies %zu, drawn tris %zu, draw calls %zu",
         rs.visibleMeshes, rs.totalMeshes, rs.visibleInstances,
         rs.totalInstances, rs.proxyInstances, rs.drawnTriangles, rs.drawCalls);
  }

  auto shot = [&](const std::string& path, bool window) {
    if (path.empty()) return;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool ok;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
    if (renderThreadActive_ && !window) {
      // The render thread owns the GL context, so it grabbed the viewport into
      // renderCapture_ when the packet's wantCapture was set (above). Use it
      // instead of calling captureViewport from this (context-less) thread.
      rgba = renderCapture_;
      w = renderCaptureW_;
      h = renderCaptureH_;
      ok = !rgba.empty();
    } else
#endif
    {
      ok = window ? renderer_->captureWindow(&rgba, &w, &h)
                  : renderer_->captureViewport(&rgba, &w, &h);
    }
    if (ok && w > 0 && h > 0) {
      reportCaptureWidth_ = w;
      reportCaptureHeight_ = h;
      std::string shotErr;
      if (WriteScreenshotImage(path, rgba, w, h, &shotErr)) {
        LOGI("wrote %s (%dx%d)", path.c_str(), w, h);
      } else {
        LOGW("failed to write %s: %s", path.c_str(), shotErr.c_str());
      }
    } else {
      LOGW("capture not supported by this backend");
    }
  };
  // CUDA ray tracing: trace the loaded scene on the GPU (CUDA driver API + NVRTC
  // loaded at runtime via cuew) and write it in place of the rasterized capture.
  if (cudaRt_ && !screenshot.empty() && !draw_.empty()) {
    std::string cerr;
    // The tracer builds from draw_ geometry, which the next loader hands over in
    // its REST pose (the deform lives in the GPU skin/morph channels). Pose it.
    poseNextDrawForTracer(animTime_);
    ensureRtTexturePayloads();  // raster frames may have released them
    auto buildCudaScene = [&]() {
      BuildProgress progress;
      std::atomic<bool> monitoring{true};
      const auto started = std::chrono::steady_clock::now();
      std::thread monitor([&]() {
        int seconds = 0;
        while (monitoring.load(std::memory_order_relaxed)) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          if (!monitoring.load(std::memory_order_relaxed) || (++seconds % 5) != 0)
            continue;
          const int phase = progress.phase.load(std::memory_order_relaxed);
          const size_t done = progress.done.load(std::memory_order_relaxed);
          const size_t total = progress.total.load(std::memory_order_relaxed);
          LOGI("CUDA scene build: %s %zu/%zu (%.0f s)",
               BuildProgress::phaseName(phase), done, total,
               std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             started)
                   .count());
        }
      });
      const bool ok = cudaTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_,
                                        &cerr, gui_.displacementScale(),
                                        &progress, false,
                                        gui_.purposeVisibleMask());
      monitoring.store(false, std::memory_order_relaxed);
      monitor.join();
      return ok;
    };
    if (!cudaTracer_.init(&cerr)) {
      LOGW("CUDA ray tracing unavailable: %s", cerr.c_str());
    } else if (!buildCudaScene()) {
      LOGW("CUDA ray tracing build failed: %s", cerr.c_str());
    } else {
      // Use the requested window size for the screenshot. The viewport probe is
      // unreliable on the RT screenshot path: rtOwnsScreenshot_ skips the raster
      // renderViewportScene(), so resizeViewport() never runs and captureViewport()
      // would report the tiny default offscreen size (e.g. 64x20).
      int w = 0, h = 0;
      getRequestedWindowSize(&w, &h);
      if (w <= 0 || h <= 0) {
        std::vector<uint8_t> sizeProbe;
        renderer_->captureViewport(&sizeProbe, &w, &h);
      }
      if (w <= 0 || h <= 0) { w = 1024; h = 768; }
      camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
      const light3d::Mat4 pv = camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
      const light3d::Mat4 inv = pv.inverse();
      const light3d::Vec3 eye = camera_.eye();
      const float camPos[3] = {eye.x, eye.y, eye.z};
      float lightDir[3];
      CopyPreviewLightDir(draw_, lightDir);
      const float clear[3] = {0.12f, 0.12f, 0.13f};
      const int rmode = static_cast<int>(gui_.renderMode());
      float depthScale = 1.0f;
      float sceneMin[3] = {0, 0, 0}, sceneExtent[3] = {1, 1, 1};
      if (draw_.hasBounds) {
        const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
        const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
        const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
        depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
        for (int i = 0; i < 3; ++i) {
          sceneMin[i] = draw_.aabbMin[i];
          sceneExtent[i] = std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
        }
      }
      std::vector<uint8_t> rgba;
      std::vector<float> linear;
      pathTraceRenderedSamples_ = 0;
      if (cudaTracer_.trace(inv.m, pv.m, camPos, lightDir, clear, camera_.exposure(), rmode, depthScale, sceneMin,
                            sceneExtent, w, h, &rgba, &cerr, rtSamples_,
                            &cameraLens_, pathTrace_.enabled ? &pathTrace_ : nullptr,
                            pathTrace_.enabled ? &linear : nullptr,
                            pathTrace_.enabled ? &pathTraceRenderedSamples_ : nullptr,
                            static_cast<float>(animFps_ > 0.0 ? animTime_ / animFps_ : 0.0),
                            static_cast<float>(animTime_))) {
        // CUDA/OptiX initializes after the renderer's startup capability line.
        // Re-emit the stable line now that effective transport and acceleration
        // memory are authoritative for automation consumers.
        logCapabilities();
        reportCaptureWidth_ = w;
        reportCaptureHeight_ = h;
        std::string werr;
        if (pathTrace_.enabled) {
          const std::vector<float> display = PreparePathDisplay(
              linear, w, h, pathTrace_, pathTraceRenderedSamples_);
          rgba = ToneMapPathDisplay(display, w, h, camera_.exposure());
        }
        if (WriteScreenshotImage(screenshot, rgba, w, h, &werr)) {
          LOGI("CUDA RT wrote %s (%dx%d, %zu tris%s, %s)", screenshot.c_str(), w, h,
               cudaTracer_.triangleCount(),
               cudaTracer_.truncated() ? ", truncated" : "", cudaTracer_.deviceName());
        } else {
          LOGW("CUDA RT screenshot write failed: %s", werr.c_str());
        }
        if (!linearOutput_.empty()) {
          if (WriteLinearExr(linearOutput_, linear, w, h, &werr))
            LOGI("CUDA path trace wrote linear EXR %s", linearOutput_.c_str());
          else
            LOGW("CUDA linear EXR write failed: %s", werr.c_str());
        }
      } else {
        LOGW("CUDA ray trace failed: %s", cerr.c_str());
      }
    }
    return finishRun(0);  // CUDA owns the screenshot.
  }

  // HIP/ROCm ray tracing: AMD counterpart of the CUDA path above (HIP runtime +
  // hiprtc loaded at runtime via hipew). Same scene flatten / BVH / kernel.
  if (hipRt_ && !screenshot.empty() && !draw_.empty()) {
    std::string cerr;
    poseNextDrawForTracer(animTime_);  // as CUDA above
    ensureRtTexturePayloads();         // as CUDA above
    // A windowed --frames run already built (and per-pose refit/rebuilt) the
    // interactive scene at this very time code: trace THAT instead of paying a
    // redundant full rebuild. This is also what lets the refit parity gate see
    // refit geometry in the screenshot -- the window capture composites the UI,
    // not the traced pixels. Headless (or a failed interactive build) still
    // builds here as before.
    // Static scenes have no tracer pose timestamp to compare: their already
    // uploaded BVH is valid regardless of the timeline value. Requiring an
    // exact timestamp here rebuilt the whole scene after every windowed
    // --frames capture even though the HIP viewport had just traced it.
    const bool reuseInteractive =
        hipInteractiveBuilt_ &&
        (!sceneIsNextDeformable() || animTime_ == nextTracerPosedTime_);
    if (!hipTracer_.init(&cerr)) {
      LOGW("HIP ray tracing unavailable: %s", cerr.c_str());
    } else if (!reuseInteractive &&
               !hipTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &cerr,
                                 gui_.displacementScale(), nullptr, false,
                                 gui_.purposeVisibleMask())) {
      LOGW("HIP ray tracing build failed: %s", cerr.c_str());
    } else {
      // Use the requested window size for the screenshot. The viewport probe is
      // unreliable on the RT screenshot path: rtOwnsScreenshot_ skips the raster
      // renderViewportScene(), so resizeViewport() never runs and captureViewport()
      // would report the tiny default offscreen size (e.g. 64x20).
      int w = 0, h = 0;
      getRequestedWindowSize(&w, &h);
      if (w <= 0 || h <= 0) {
        std::vector<uint8_t> sizeProbe;
        renderer_->captureViewport(&sizeProbe, &w, &h);
      }
      if (w <= 0 || h <= 0) { w = 1024; h = 768; }
      camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
      const light3d::Mat4 pv = camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
      const light3d::Mat4 inv = pv.inverse();
      const light3d::Vec3 eye = camera_.eye();
      const float camPos[3] = {eye.x, eye.y, eye.z};
      float lightDir[3];
      CopyPreviewLightDir(draw_, lightDir);
      const float clear[3] = {0.12f, 0.12f, 0.13f};
      const int rmode = static_cast<int>(gui_.renderMode());
      float depthScale = 1.0f;
      float sceneMin[3] = {0, 0, 0}, sceneExtent[3] = {1, 1, 1};
      if (draw_.hasBounds) {
        const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
        const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
        const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
        depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
        for (int i = 0; i < 3; ++i) {
          sceneMin[i] = draw_.aabbMin[i];
          sceneExtent[i] = std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
        }
      }
      std::vector<uint8_t> rgba;
      std::vector<float> linear;
      pathTraceRenderedSamples_ = 0;
      if (hipTracer_.trace(inv.m, pv.m, camPos, lightDir, clear, camera_.exposure(), rmode, depthScale, sceneMin,
                           sceneExtent, w, h, &rgba, &cerr, rtSamples_,
                           &cameraLens_, pathTrace_.enabled ? &pathTrace_ : nullptr,
                           pathTrace_.enabled ? &linear : nullptr,
                           pathTrace_.enabled ? &pathTraceRenderedSamples_ : nullptr,
                           static_cast<float>(animFps_ > 0.0 ? animTime_ / animFps_ : 0.0),
                           static_cast<float>(animTime_))) {
        reportCaptureWidth_ = w;
        reportCaptureHeight_ = h;
        std::string werr;
        if (pathTrace_.enabled) {
          const std::vector<float> display = PreparePathDisplay(
              linear, w, h, pathTrace_, pathTraceRenderedSamples_);
          rgba = ToneMapPathDisplay(display, w, h, camera_.exposure());
        }
        if (WriteScreenshotImage(screenshot, rgba, w, h, &werr)) {
          LOGI("HIP RT wrote %s (%dx%d, %zu tris%s, %s)", screenshot.c_str(), w, h,
               hipTracer_.triangleCount(),
               hipTracer_.truncated() ? ", truncated" : "", hipTracer_.deviceName());
        } else {
          LOGW("HIP RT screenshot write failed: %s", werr.c_str());
        }
        if (!linearOutput_.empty()) {
          if (WriteLinearExr(linearOutput_, linear, w, h, &werr))
            LOGI("HIP path trace wrote linear EXR %s", linearOutput_.c_str());
          else
            LOGW("HIP linear EXR write failed: %s", werr.c_str());
        }
      } else {
        LOGW("HIP ray trace failed: %s", cerr.c_str());
      }
    }
    return finishRun(0);  // HIP owns the screenshot.
  }

  // CPU ray tracing owns headless screenshots just like CUDA/HIP. Keeping this
  // out of the frame loop avoids building the BVH repeatedly and makes the CPU
  // compositor available to deterministic, display-free regression tests.
  if (cpuRt_ && headless_ && !screenshot.empty() && !draw_.empty()) {
    std::string cerr;
    poseNextDrawForTracer(animTime_);
    ensureRtTexturePayloads();
    if (!cpuTracer_.build(draw_, cudaMaxTris_, rtMaxInstances_, &cerr,
                          gui_.displacementScale(), nullptr,
                          gui_.purposeVisibleMask())) {
      LOGW("CPU ray tracing build failed: %s", cerr.c_str());
    } else {
      int w = 0, h = 0;
      getRequestedWindowSize(&w, &h);
      if (w <= 0 || h <= 0) { w = 1024; h = 768; }
      camera_.setAspect(static_cast<float>(w) / static_cast<float>(h));
      const light3d::Mat4 pv =
          camera_.proj(/*zeroToOneDepth=*/true) * camera_.view();
      const light3d::Mat4 inv = pv.inverse();
      const light3d::Vec3 eye = camera_.eye();
      const float camPos[3] = {eye.x, eye.y, eye.z};
      float lightDir[3];
      CopyPreviewLightDir(draw_, lightDir);
      const float clear[3] = {0.12f, 0.12f, 0.13f};
      const int rmode = static_cast<int>(gui_.renderMode());
      float depthScale = 1.0f;
      float sceneMin[3] = {0, 0, 0};
      float sceneExtent[3] = {1, 1, 1};
      if (draw_.hasBounds) {
        const float dx = draw_.aabbMax[0] - draw_.aabbMin[0];
        const float dy = draw_.aabbMax[1] - draw_.aabbMin[1];
        const float dz = draw_.aabbMax[2] - draw_.aabbMin[2];
        depthScale = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
        for (int i = 0; i < 3; ++i) {
          sceneMin[i] = draw_.aabbMin[i];
          sceneExtent[i] =
              std::max(1e-4f, draw_.aabbMax[i] - draw_.aabbMin[i]);
        }
      }
      std::vector<uint8_t> rgba;
      if (cpuTracer_.trace(inv.m, pv.m, camPos, lightDir, clear,
                           camera_.exposure(), rmode, depthScale, sceneMin,
                           sceneExtent, w, h, &rgba, &cerr, rtSamples_,
                           &cameraLens_)) {
        reportCaptureWidth_ = w;
        reportCaptureHeight_ = h;
        std::string werr;
        if (WriteScreenshotImage(screenshot, rgba, w, h, &werr)) {
          LOGI("CPU RT wrote %s (%dx%d, %zu tris%s, %s)", screenshot.c_str(),
               w, h, cpuTracer_.triangleCount(),
               cpuTracer_.truncated() ? ", truncated" : "",
               cpuTracer_.deviceName());
        } else {
          LOGW("CPU RT screenshot write failed: %s", werr.c_str());
        }
      } else {
        LOGW("CPU ray trace failed: %s", cerr.c_str());
      }
    }
    return finishRun(0);
  }

  bool wrotePathDisplay = false;
  if (pathTrace_.enabled && renderer_ &&
      (!screenshot.empty() || !linearOutput_.empty())) {
    std::vector<float> linear;
    int w = 0, h = 0;
    std::string linearErr;
    if (renderer_->captureLinearViewport(&linear, &w, &h)) {
      if (!screenshot.empty() && modeSweep_.empty()) {
        const uint32_t samples = renderer_->rayTracingAccumulatedSamples();
        const std::vector<float> display =
            PreparePathDisplay(linear, w, h, pathTrace_, samples);
        const std::vector<uint8_t> rgba =
            ToneMapPathDisplay(display, w, h, camera_.exposure());
        wrotePathDisplay = WriteScreenshotImage(screenshot, rgba, w, h,
                                                &linearErr);
        if (wrotePathDisplay)
          LOGI("wrote path-traced display %s (%dx%d, %u spp)",
               screenshot.c_str(), w, h, samples);
      }
      if (!linearOutput_.empty() &&
          WriteLinearExr(linearOutput_, linear, w, h, &linearErr)) {
        LOGI("Vulkan path trace wrote linear EXR %s (%dx%d)",
             linearOutput_.c_str(), w, h);
      } else if (!linearOutput_.empty()) {
        LOGW("Vulkan linear EXR write failed: %s", linearErr.c_str());
      }
    } else {
      linearErr = "linear accumulation capture unavailable";
      LOGW("Vulkan linear EXR write failed: %s", linearErr.c_str());
    }
  }
  if (modeSweep_.empty() && !wrotePathDisplay)
    shot(screenshot, /*window=*/false);
  shot(windowShot_, /*window=*/true);
  return finishRun(0);
}

}  // namespace lusdview
