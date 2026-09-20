// SPDX-License-Identifier: Apache-2.0
// lusdview - `next` loader -> tydra-next RenderScene -> DrawScene adapter, plus
// PointInstancer extraction into GPU-instanced draws.

#include "next_scene_loader.hh"
#include "lighting_eval.hh"
#include "lighting_ies.hh"
#include "scene_optimize.hh"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "log.hh"

// `next` + tydra-next (built on demand; see CMakeLists.txt).
#include "next/lightusd-next.hh"
#include "next/eval/attribute-eval.hh" // time/connection-aware light inputs
#include "next/reader/usdz-reader.hh"  // USDZReader (embedded --next textures)
#include "next/schema/usd-shade.hh"    // GetInheritedBoundMaterialPath
#include "next/schema/usd-skel.hh"     // GetSkeletonData / GetSkelAnimationData
#include "next/schema/geom-xform.hh"   // HasAnimatedTransform
#include "next/types/value-view.hh"    // CanBorrowLazyFlat
#include "tydra/scene-access.hh"       // SkinPointsLBS / ConcatJointTransforms
#include "tydra/next/render-converter.hh"
#include "tydra/next/render-extract.hh"
#include "tydra/next/openpbr-params-converter.hh"
#include "tydra/next/texture-cache.hh"  // shared decode + size cap + byte budget
#include "ptx-loader.hh"                  // lazy Ptex metadata validation
#include "ptex_atlas.hh"                  // bounded rectangular face atlas
#include "tydra/next/scene-access.hh"  // ComputeWorldTransform
#include "value-types.hh"              // value::matrix4d / quatf / double3
#include "xform.hh"                    // to_matrix3x3 / to_matrix / inverse
#include "io-util.hh"                  // io::GetBaseDir / SplitUDIMPath
#include "tydra/texture-util.hh"       // tydra::ResizeImage (UDIM tile normalize)
#include "image-loader.hh"             // DomeLight envmap decode (IBL bake)
#include "mesh_build.hh"               // UpdatePreviewLight
#include "preview_cache.hh"
#include "displacement_bake.hh"        // Ptex-aware coarse geometry bake
#include "lightrt_mtlx_bridge.hh"      // BakeLightRtOpenPBR (next material bake)
#include "texture_tools.hh"            // TexToolsBuildDomeIbl / ProbeToEquirect
#include "usdVol.hh"                   // OpenVDB (.vdb) loader

#include <chrono>

namespace lusdview {

namespace tydn = ::lightusd::tydra::next;
namespace tnext = ::lightusd::next;
using matrix4d = ::lightusd::value::matrix4d;

namespace {
class PreviewCacheWriters {
 public:
  // Bound concurrent cache writes. Each worker keeps a full StageSnapshot alive
  // while serializing, so an unbounded queue would accumulate snapshots (and
  // threads) across many loads. When the cap is hit the OLDEST writer is joined
  // first -- it always completes since the work is a plain StorePreviewCache
  // call -- keeping both thread count and retained snapshot memory bounded.
  static constexpr size_t kMaxOutstanding = 4;

  ~PreviewCacheWriters() {
    JoinAll();
  }

  void Start(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (writers_.size() >= kMaxOutstanding) {
      std::thread& oldest = writers_.front();
      if (oldest.joinable()) oldest.join();
      writers_.erase(writers_.begin());
    }
    writers_.emplace_back(std::move(work));
  }

  void JoinAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::thread& writer : writers_) {
      if (writer.joinable()) writer.join();
    }
    writers_.clear();
  }

 private:
  std::mutex mutex_;
  std::vector<std::thread> writers_;
};

void StartPreviewCacheWriter(std::function<void()> work) {
  static PreviewCacheWriters writers;
  writers.Start(std::move(work));
}

size_t ProcessRssMiB() {
#if defined(__linux__)
  FILE* file = std::fopen("/proc/self/statm", "r");
  if (!file) return 0;
  long pages = 0, resident = 0;
  if (std::fscanf(file, "%ld %ld", &pages, &resident) != 2) resident = 0;
  std::fclose(file);
  return static_cast<size_t>(resident) *
         static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / (1024u * 1024u);
#else
  return 0;
#endif
}

size_t ProcessPeakRssMiB() {
#if defined(__linux__)
  struct rusage usage {};
  return ::getrusage(RUSAGE_SELF, &usage) == 0
             ? static_cast<size_t>(usage.ru_maxrss) / 1024u
             : 0;
#else
  return 0;
#endif
}

void LogProcessMemory(const char* phase) {
  LOGI("memory: %s process RSS %zu MiB (peak %zu MiB)", phase,
       ProcessRssMiB(), ProcessPeakRssMiB());
}

size_t ProgressiveMeshBytes(const DrawMeshCPU& m) {
  return m.vertices.size() * sizeof(DrawVertex) +
         m.indices.size() * sizeof(uint32_t) +
         m.vertexColors.size() * sizeof(float) +
         m.vertexAlpha.size() * sizeof(float) + m.uv1.size() * sizeof(float) +
         m.wireframeIndices.size() * sizeof(uint32_t) +
         m.wireframeVertices.size() * sizeof(DrawVertex) +
         m.sourceFaceId.size() * sizeof(uint32_t) +
         m.jointIdx.size() * sizeof(uint32_t) + m.jointWt.size() * sizeof(float) +
         m.influenceOffsetCount.size() * sizeof(uint32_t) +
         m.influenceTexels.size() * sizeof(float) +
         m.morphOffsetCount.size() * sizeof(uint32_t) +
         m.morphDeltaHalf.size() * sizeof(uint16_t) +
         m.morphChannelId.size() * sizeof(uint16_t) +
         m.instanceXforms.size() * sizeof(float) +
         m.instanceColors.size() * sizeof(float) +
         m.instanceOpacities.size() * sizeof(float);
}

size_t ProgressivePointsBytes(const DrawPointsCPU& p) {
  return p.points.size() * sizeof(float) + p.normals.size() * sizeof(float) +
         p.widths.size() * sizeof(float) + p.colors.size() * sizeof(float) +
         p.opacities.size() * sizeof(float) +
         p.ellipseRadii.size() * sizeof(float) +
         p.ellipseNormals.size() * sizeof(float) +
         p.ellipseMajorAxes.size() * sizeof(float);
}

size_t ProgressiveCurvesBytes(const DrawCurvesCPU& c) {
  return c.vertexCounts.size() * sizeof(uint32_t) +
         c.points.size() * sizeof(float) + c.widths.size() * sizeof(float) +
         c.colors.size() * sizeof(float) + c.opacities.size() * sizeof(float);
}

size_t ProgressiveVolumeBytes(const DrawVolumeCPU& v) {
  return (v.density.size() + v.emissionField.size() +
          v.temperatureField.size()) * sizeof(float);
}

size_t ProgressiveTextureBytes(const DrawTextureCPU& t) {
  size_t bytes = t.image.data.size();
  for (const light3d::Image& mip : t.mipImages) bytes += mip.data.size();
  bytes += t.compressed.data.size();
  for (const DrawCompressedMipCPU& mip : t.compressed.mips)
    bytes += mip.data.size();
  for (const DrawUdimTileCPU& tile : t.udimTiles) {
    bytes += tile.image.data.size();
    bytes += tile.compressed.data.size();
    for (const DrawCompressedMipCPU& mip : tile.compressed.mips)
      bytes += mip.data.size();
    for (const light3d::Image& mip : tile.mipImages) bytes += mip.data.size();
  }
  bytes += t.PtexSourceData().size();
  return bytes;
}
}  // namespace

ProgressiveSceneStream::ProgressiveSceneStream(size_t maxBytes)
    : maxBytes_(maxBytes ? maxBytes : (size_t(64) << 20)) {}

ProgressiveSceneStream::~ProgressiveSceneStream() { cancel(); }

bool ProgressiveSceneStream::pushPreview(DrawScene&& scene) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::PreviewScene;
  queued.event.scene = std::move(scene);
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushReset() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Reset;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushResources(
    const std::vector<DrawMaterialCPU>& materials, int textureCount,
    const std::string& upAxis) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Resources;
  queued.event.materials = materials;
  queued.event.textureCount = textureCount;
  queued.event.upAxis = upAxis;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushMesh(
    DrawMeshCPU&& mesh, const std::atomic<bool>* externallyCancelled) {
  const size_t bytes = ProgressiveMeshBytes(mesh);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!cancelled_ &&
         !(queuedBytes_ == 0 || queuedBytes_ + bytes <= maxBytes_)) {
    if (externallyCancelled && externallyCancelled->load()) return false;
    space_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (cancelled_ || (externallyCancelled && externallyCancelled->load())) {
    return false;
  }
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Mesh;
  queued.event.mesh = std::move(mesh);
  queued.bytes = bytes;
  queuedBytes_ += bytes;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushPoints(
    DrawPointsCPU&& points, const std::atomic<bool>* externallyCancelled) {
  const size_t bytes = ProgressivePointsBytes(points);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!cancelled_ &&
         !(queuedBytes_ == 0 || queuedBytes_ + bytes <= maxBytes_)) {
    if (externallyCancelled && externallyCancelled->load()) return false;
    space_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (cancelled_ || (externallyCancelled && externallyCancelled->load()))
    return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Points;
  queued.event.points = std::move(points);
  queued.bytes = bytes;
  queuedBytes_ += bytes;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushCurves(
    DrawCurvesCPU&& curves, const std::atomic<bool>* externallyCancelled) {
  const size_t bytes = ProgressiveCurvesBytes(curves);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!cancelled_ &&
         !(queuedBytes_ == 0 || queuedBytes_ + bytes <= maxBytes_)) {
    if (externallyCancelled && externallyCancelled->load()) return false;
    space_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (cancelled_ || (externallyCancelled && externallyCancelled->load()))
    return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Curves;
  queued.event.curves = std::move(curves);
  queued.bytes = bytes;
  queuedBytes_ += bytes;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushVolume(
    DrawVolumeCPU&& volume, const std::atomic<bool>* externallyCancelled) {
  const size_t bytes = ProgressiveVolumeBytes(volume);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!cancelled_ &&
         !(queuedBytes_ == 0 || queuedBytes_ + bytes <= maxBytes_)) {
    if (externallyCancelled && externallyCancelled->load()) return false;
    space_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (cancelled_ || (externallyCancelled && externallyCancelled->load()))
    return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Volume;
  queued.event.volume = std::move(volume);
  queued.bytes = bytes;
  queuedBytes_ += bytes;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

bool ProgressiveSceneStream::pushTexture(
    int slot, DrawTextureCPU&& texture,
    const std::atomic<bool>* externallyCancelled) {
  const size_t bytes = ProgressiveTextureBytes(texture);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!cancelled_ &&
         !(queuedBytes_ == 0 || queuedBytes_ + bytes <= maxBytes_)) {
    if (externallyCancelled && externallyCancelled->load()) return false;
    space_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (cancelled_ || (externallyCancelled && externallyCancelled->load()))
    return false;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Texture;
  queued.event.textureSlot = slot;
  queued.event.texture = std::move(texture);
  queued.bytes = bytes;
  queue_.push_back(std::move(queued));
  ready_.notify_one();
  return true;
}

void ProgressiveSceneStream::pushComplete(DrawScene&& scene) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Complete;
  queued.event.scene = std::move(scene);
  queue_.push_back(std::move(queued));
  ready_.notify_one();
}

void ProgressiveSceneStream::pushFailed(std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return;
  QueuedEvent queued;
  queued.event.type = ProgressiveSceneEvent::Type::Failed;
  queued.event.error = std::move(error);
  queue_.push_back(std::move(queued));
  ready_.notify_one();
}

bool ProgressiveSceneStream::tryPop(ProgressiveSceneEvent* event) {
  if (!event) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return false;
  QueuedEvent queued = std::move(queue_.front());
  queue_.pop_front();
  queuedBytes_ -= std::min(queuedBytes_, queued.bytes);
  *event = std::move(queued.event);
  space_.notify_all();
  return true;
}

void ProgressiveSceneStream::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = true;
  queue_.clear();
  queuedBytes_ = 0;
  ready_.notify_all();
  space_.notify_all();
}

bool ProgressiveSceneStream::cancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

size_t ProgressiveSceneStream::queuedBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queuedBytes_;
}

static void ResolveNextSurfaceVolumeMaterial(const tnext::Stage& stage,
                                             const tnext::UsdPrim& material,
                                             DrawMaterialCPU* out);
static void ResolveNextDisplacementMaterial(const tnext::Stage& stage,
                                            const tnext::UsdPrim& material,
                                            DrawMaterialCPU* out);

namespace {

// Pack a lightusd row-major matrix4d (m[row][col], row-vector p*M) into a 3x4
// object-to-world (12 floats): row k holds the coefficients of output component
// k, i.e. worldP.k = dot(vec4(p,1), o2w_row_k). Matches lusdrender Mat4ToObj2World
// and the instanced vertex shader's aRow0/1/2.
inline void Mat4dToO2W(const matrix4d& m, float out[12]) {
  for (int k = 0; k < 3; ++k) {
    out[k * 4 + 0] = static_cast<float>(m.m[0][k]);
    out[k * 4 + 1] = static_cast<float>(m.m[1][k]);
    out[k * 4 + 2] = static_cast<float>(m.m[2][k]);
    out[k * 4 + 3] = static_cast<float>(m.m[3][k]);
  }
}

inline matrix4d Mat4dFromArray(const double d[16]) {
  matrix4d m;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m.m[i][j] = d[i * 4 + j];
  return m;
}

inline void RowMatrixToColumnMajor(const double src[16], float dst[16]) {
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      dst[col * 4 + row] = static_cast<float>(src[row * 4 + col]);
}

inline void TransformRowPoint(const double m[16], float x, float y, float z,
                              float out[3]) {
  out[0] = static_cast<float>(x * m[0] + y * m[4] + z * m[8] + m[12]);
  out[1] = static_cast<float>(x * m[1] + y * m[5] + z * m[9] + m[13]);
  out[2] = static_cast<float>(x * m[2] + y * m[6] + z * m[10] + m[14]);
}

// Row-major matrix multiply matching lightusd value::Mult: (a*b).m[j][i] =
// sum_k a.m[j][k]*b.m[k][i]. Row-vector convention: p*(a*b) applies `a` first.
inline matrix4d Mul4(const matrix4d& a, const matrix4d& b) {
  matrix4d r;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      double v = 0.0;
      for (int k = 0; k < 4; ++k) v += a.m[j][k] * b.m[k][i];
      r.m[j][i] = v;
    }
  return r;
}

struct PreviewBound {
  matrix4d boxToWorld{matrix4d::identity()};
  float worldMin[3]{0.0f, 0.0f, 0.0f};
  float worldMax[3]{0.0f, 0.0f, 0.0f};
  float color[3]{0.62f, 0.68f, 0.76f};
  size_t sourceOrder{0};
  bool cameraFacing{true};
  float cameraDistance{0.0f};
};

bool PreviewExtent(const tnext::UsdPrim& prim, float mn[3], float mx[3]) {
  const tnext::Value* value = prim.GetPropertyValue("extent");
  if (!value) value = prim.GetPropertyValue("extentsHint");
  if (!value) return false;
  tnext::Value materialized;
  if (value->is_lazy()) {
    materialized = value->materialized_copy();
    value = &materialized;
  }
  if (const std::vector<float>* f = value->as_float_array()) {
    if (f->size() < 6) return false;
    for (int k = 0; k < 3; ++k) {
      mn[k] = (*f)[k];
      mx[k] = (*f)[k + 3];
    }
    return true;
  }
  if (const std::vector<double>* d = value->as_double_array()) {
    if (d->size() < 6) return false;
    for (int k = 0; k < 3; ++k) {
      mn[k] = static_cast<float>((*d)[k]);
      mx[k] = static_cast<float>((*d)[k + 3]);
    }
    return true;
  }
  return false;
}

void CollectPreviewBounds(const tnext::Stage& stage, const tnext::UsdPrim& prim,
                          double time, size_t maxBounds,
                          std::vector<PreviewBound>* bounds) {
  if (!prim.IsActive() || !bounds || bounds->size() >= maxBounds) return;
  const tnext::Value* visibility = prim.GetPropertyValue("visibility");
  if (visibility && visibility->as_token() &&
      *visibility->as_token() == "invisible") return;

  float mn[3], mx[3];
  if (PreviewExtent(prim, mn, mx)) {
    double world16[16];
    if (tydn::ComputeWorldTransform(stage, prim, world16, time)) {
      matrix4d local = matrix4d::identity();
      for (int k = 0; k < 3; ++k) {
        local.m[k][k] = static_cast<double>(mx[k] - mn[k]);
        local.m[3][k] = static_cast<double>(0.5f * (mn[k] + mx[k]));
      }
      PreviewBound bound;
      bound.boxToWorld = Mul4(local, Mat4dFromArray(world16));
      bound.sourceOrder = bounds->size();
      for (int k = 0; k < 3; ++k) {
        bound.worldMin[k] = std::numeric_limits<float>::max();
        bound.worldMax[k] = -std::numeric_limits<float>::max();
      }
      for (int corner = 0; corner < 8; ++corner) {
        const double x = (corner & 1) ? 0.5 : -0.5;
        const double y = (corner & 2) ? 0.5 : -0.5;
        const double z = (corner & 4) ? 0.5 : -0.5;
        for (int k = 0; k < 3; ++k) {
          const float p = static_cast<float>(
              x * bound.boxToWorld.m[0][k] +
              y * bound.boxToWorld.m[1][k] +
              z * bound.boxToWorld.m[2][k] + bound.boxToWorld.m[3][k]);
          bound.worldMin[k] = std::min(bound.worldMin[k], p);
          bound.worldMax[k] = std::max(bound.worldMax[k], p);
        }
      }
      if (const tnext::Value* color =
              prim.GetPropertyValue("primvars:displayColor")) {
        if (const std::vector<float>* c = color->as_float_array()) {
          if (c->size() >= 3) {
            for (int k = 0; k < 3; ++k) bound.color[k] = (*c)[k];
          }
        }
      }
      bounds->push_back(std::move(bound));
    }
  }
  for (const tnext::UsdPrim& child : prim.GetChildren()) {
    if (bounds->size() >= maxBounds) break;
    CollectPreviewBounds(stage, child, time, maxBounds, bounds);
  }
}

DrawScene BuildCheckpointPreview(const tnext::Stage& stage, double time,
                                 size_t maxBoxes,
                                 const std::string& cameraName) {
  DrawScene draw;
  draw.upAxis = (stage.GetUpAxis() == "Z" || stage.GetUpAxis() == "z") ? "Z" : "Y";
  draw.metersPerUnit = stage.GetMetersPerUnit() > 0.0
                           ? stage.GetMetersPerUnit()
                           : 0.01;
  std::vector<PreviewBound> bounds;
  for (const tnext::UsdPrim& root : stage.GetRootPrims()) {
    if (bounds.size() >= maxBoxes) break;
    CollectPreviewBounds(stage, root, time, maxBoxes, &bounds);
  }

  NextCameraPose camera;
  if (FindNextCamera(stage, cameraName, time, &camera)) {
    for (PreviewBound& bound : bounds) {
      float delta[3];
      float distance2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float center = 0.5f * (bound.worldMin[k] + bound.worldMax[k]);
        delta[k] = center - camera.eye[k];
        distance2 += delta[k] * delta[k];
      }
      bound.cameraDistance = std::sqrt(distance2);
      bound.cameraFacing = delta[0] * camera.forward[0] +
                               delta[1] * camera.forward[1] +
                               delta[2] * camera.forward[2] >
                           0.0f;
    }
    std::stable_sort(bounds.begin(), bounds.end(),
                     [](const PreviewBound& a, const PreviewBound& b) {
                       if (a.cameraFacing != b.cameraFacing) return a.cameraFacing;
                       if (a.cameraDistance != b.cameraDistance)
                         return a.cameraDistance < b.cameraDistance;
                       return a.sourceOrder < b.sourceOrder;
                     });
  }
  if (bounds.size() > maxBoxes) bounds.resize(maxBoxes);
  if (bounds.empty()) return draw;

  DrawMeshCPU mesh;
  mesh.name = "__composition_preview_bounds";
  mesh.absPath = "/__composition_preview_bounds";
  mesh.purpose = "proxy";
  mesh.geometricNormal = true;
  static const float corners[8][3] = {
      {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
      {0.5f, 0.5f, -0.5f},   {-0.5f, 0.5f, -0.5f},
      {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},
      {0.5f, 0.5f, 0.5f},    {-0.5f, 0.5f, 0.5f}};
  for (const auto& p : corners) {
    mesh.vertices.push_back({p[0], p[1], p[2], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
  }
  static const uint32_t indices[] = {
      0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
      3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
  mesh.indices.assign(indices, indices + sizeof(indices) / sizeof(indices[0]));
  mesh.submeshes.push_back({0, static_cast<uint32_t>(mesh.indices.size()), -1});
  for (int i = 0; i < 16; ++i) mesh.world[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  for (const PreviewBound& bound : bounds) {
    float o2w[12];
    Mat4dToO2W(bound.boxToWorld, o2w);
    mesh.instanceXforms.insert(mesh.instanceXforms.end(), o2w, o2w + 12);
    mesh.instanceColors.insert(mesh.instanceColors.end(), bound.color,
                               bound.color + 3);
    for (int k = 0; k < 3; ++k) {
      if (mesh.instanceCount() == 1) {
        mesh.aabbMin[k] = bound.worldMin[k];
        mesh.aabbMax[k] = bound.worldMax[k];
      } else {
        mesh.aabbMin[k] = std::min(mesh.aabbMin[k], bound.worldMin[k]);
        mesh.aabbMax[k] = std::max(mesh.aabbMax[k], bound.worldMax[k]);
      }
      mesh.protoAabbMin[k] = -0.5f;
      mesh.protoAabbMax[k] = 0.5f;
    }
  }
  draw.vertexCount = mesh.vertices.size();
  draw.triangleCount = mesh.indices.size() / 3;
  draw.meshes.push_back(std::move(mesh));
  return draw;
}

// A checkpoint preview is only a spatial placeholder while authoritative
// composition/conversion continues. Keep its density configurable so preview
// quality does not impose a fixed startup cost on every workload.
constexpr size_t kDefaultCheckpointPreviewMaxBoxes = 1024;

std::string PreviewFingerprint(const LoadOptions& options) {
  std::ostringstream out;
  out << "composition=" << options.composition
      << ";payload=" << static_cast<int>(options.payloadPolicy)
      << ";deferReferences=" << options.deferReferences
      << ";parentPaths=" << options.allowParentRelativePaths
      << ";viewCamera=" << options.viewCamera
      << ";time=" << std::setprecision(17) << options.timecode;
  for (const std::string& path : options.payloadWhitelist) {
    out << ";payloadPath=" << path;
  }
  for (const auto& prim : options.variantOverrides) {
    for (const auto& selection : prim.second) {
      out << ";variant=" << prim.first << ':' << selection.first << '='
          << selection.second;
    }
  }
  return out.str();
}

// Per-instance local transform from position + orientation quaternion + scale,
// matching lusdrender's InstanceTRS (p * S * R, translation in row 3).
// `q_wxyz` is REAL-FIRST (w, x, y, z), which is how the next stage stores a quat:
// crate is imaginary-first on disk and the reader swizzles on load
// (crate-reader-unpack.cc). Reading these four floats as (x,y,z,w) turns a
// 30-degree Z rotation into a 150-degree X rotation -- it flips the instance
// upside down, which is exactly what a PointInstancer of an oriented prototype did.
inline matrix4d InstanceTRS(const float* pos, const float* q_wxyz,
                            const float* s3) {
  ::lightusd::value::quatf q;
  q.real = q_wxyz[0];
  q.imag[0] = q_wxyz[1];
  q.imag[1] = q_wxyz[2];
  q.imag[2] = q_wxyz[3];
  ::lightusd::value::matrix3d rot = ::lightusd::to_matrix3x3(q);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) rot.m[i][j] *= static_cast<double>(s3[i]);
  ::lightusd::value::double3 t{static_cast<double>(pos[0]),
                               static_cast<double>(pos[1]),
                               static_cast<double>(pos[2])};
  return ::lightusd::to_matrix(rot, t);
}

// Lazy array readers: try the time sample then the default opinion; materialize a
// lazy (mmap-backed) value. Mirror lusdrender's ReadFloatArrayLazy.
std::vector<float> ReadFloats(const tnext::UsdPrim& p, const char* name, double t) {
  return tydn::ReadFloatArrayCopy(p, name, t);
}
std::vector<int32_t> ReadInts(const tnext::UsdPrim& p, const char* name, double t) {
  return tydn::ReadIntArrayCopy(p, name, t);
}

void BuildAuthoredControlCage(const tnext::UsdPrim& prim, double time,
                              DrawMeshCPU* mesh) {
  if (!mesh) return;
  const std::vector<float> points = ReadFloats(prim, "points", time);
  const std::vector<int32_t> counts = ReadInts(prim, "faceVertexCounts", time);
  const std::vector<int32_t> indices = ReadInts(prim, "faceVertexIndices", time);
  if (points.size() % 3u || points.empty() || counts.empty() || indices.empty())
    return;
  mesh->wireframeVertices.assign(points.size() / 3u, DrawVertex{});
  for (size_t i = 0; i < mesh->wireframeVertices.size(); ++i) {
    mesh->wireframeVertices[i].px = points[i * 3u];
    mesh->wireframeVertices[i].py = points[i * 3u + 1u];
    mesh->wireframeVertices[i].pz = points[i * 3u + 2u];
  }
  mesh->wireframeIndices.clear();
  std::unordered_set<uint64_t> seen;
  size_t offset = 0;
  for (int32_t count : counts) {
    if (count < 2 || offset + size_t(count) > indices.size()) {
      mesh->wireframeIndices.clear();
      mesh->wireframeVertices.clear();
      return;
    }
    for (int32_t k = 0; k < count; ++k) {
      const int32_t ia = indices[offset + size_t(k)];
      const int32_t ib = indices[offset + size_t((k + 1) % count)];
      if (ia < 0 || ib < 0 || ia == ib ||
          size_t(ia) >= mesh->wireframeVertices.size() ||
          size_t(ib) >= mesh->wireframeVertices.size()) continue;
      uint32_t a = uint32_t(ia), b = uint32_t(ib);
      const uint64_t key = a < b ? (uint64_t(a) << 32u) | b
                                 : (uint64_t(b) << 32u) | a;
      if (seen.insert(key).second) {
        mesh->wireframeIndices.push_back(a);
        mesh->wireframeIndices.push_back(b);
      }
    }
    offset += size_t(count);
  }
}

bool PointInstanceHidden(size_t index, size_t instance_count,
                         const tydn::ValueArrayRead<int64_t>& ids,
                         const std::unordered_set<int64_t>& hidden) {
  if (hidden.empty()) return false;
  if (ids.size() == instance_count) return hidden.count(ids[index]) != 0;
  if (index > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  return hidden.count(static_cast<int64_t>(index)) != 0;
}

// Linearly-interpolated float-array read across time samples (the next stage's
// GetValueAtTime/GetInterpolatedValue snap to the nearest sample for arrays).
// Brackets `t` between the two surrounding samples and lerps element-wise; falls
// back to the plain read (default opinion / single sample / no samples).
std::vector<float> ReadFloatsLerp(const tnext::UsdPrim& p, const char* name,
                                  double t) {
  const std::vector<double> times = p.GetTimeSampleTimes(name);
  if (times.size() < 2) return ReadFloats(p, name, t);
  if (t <= times.front()) return ReadFloats(p, name, times.front());
  if (t >= times.back()) return ReadFloats(p, name, times.back());
  size_t hi = 0;
  while (hi < times.size() && times[hi] < t) ++hi;
  const double t0 = times[hi - 1], t1 = times[hi];
  const std::vector<float> a = ReadFloats(p, name, t0);
  const std::vector<float> b = ReadFloats(p, name, t1);
  if (a.size() != b.size() || t1 <= t0) return a;
  const float f = static_cast<float>((t - t0) / (t1 - t0));
  std::vector<float> out(a.size());
  for (size_t i = 0; i < a.size(); ++i) out[i] = a[i] + f * (b[i] - a[i]);
  return out;
}
std::vector<std::string> ReadTokens(const tnext::UsdPrim& p, const char* name,
                                    double t) {
  auto pull = [](const tnext::Value* v) -> std::vector<std::string> {
    if (!v) return {};
    if (v->is_lazy()) {
      tnext::Value tmp = v->materialized_copy();
      if (const auto* a = tmp.as_token_array()) return *a;
      return {};
    }
    if (const auto* a = v->as_token_array()) return *a;
    return {};
  };
  std::vector<std::string> r = pull(p.GetValueAtTime(name, t));
  if (r.empty()) r = pull(p.GetPropertyValue(name));
  return r;
}

// One tydra-next float vertex attribute, sampled per triangulated corner in
// whatever interpolation it was authored with.
struct NextAttr {
  const tydn::FloatChunked* data{nullptr};
  const tydn::Int32Chunked* int_data{nullptr};
  const tydn::UInt32Chunked* uint_data{nullptr};
  const tydn::UInt32Chunked* indices{nullptr};  // indexed primvars; may be null
  tydn::Interpolation interp{tydn::Interpolation::Vertex};
  uint32_t comps{0};

  explicit operator bool() const {
    return comps > 0 &&
           ((data && !data->empty()) || (int_data && !int_data->empty()) ||
            (uint_data && !uint_data->empty()));
  }

  // Element index for a corner, given its point id, its authored face-vertex
  // (corner) index, and its face id. SIZE_MAX when out of range.
  size_t element(uint32_t pointId, uint32_t cornerId, uint32_t faceId) const {
    size_t e = 0;
    switch (interp) {
      case tydn::Interpolation::Constant: e = 0; break;
      case tydn::Interpolation::Uniform: e = faceId; break;
      case tydn::Interpolation::FaceVarying: e = cornerId; break;
      case tydn::Interpolation::Vertex:
      case tydn::Interpolation::Varying:
      default: e = pointId; break;
    }
    if (indices && !indices->empty()) {
      if (e >= indices->size()) return SIZE_MAX;
      e = (*indices)[e];
    }
    const size_t size = data ? data->size()
                             : int_data ? int_data->size() : uint_data->size();
    if ((e + 1) * comps > size) return SIZE_MAX;
    return e;
  }

  // Read up to `n` components into `out` (zero-filled on a miss).
  void read(uint32_t pointId, uint32_t cornerId, uint32_t faceId, uint32_t n,
            float* out) const {
    for (uint32_t c = 0; c < n; ++c) out[c] = 0.0f;
    if (!*this) return;
    const size_t e = element(pointId, cornerId, faceId);
    if (e == SIZE_MAX) return;
    for (uint32_t c = 0; c < std::min(n, comps); ++c) {
      const size_t offset = e * comps + c;
      out[c] = data       ? (*data)[offset]
               : int_data ? static_cast<float>((*int_data)[offset])
                          : static_cast<float>((*uint_data)[offset]);
    }
  }
};

NextAttr MakeNextAttr(const tydn::FloatChunked& data, tydn::Interpolation interp,
                      uint32_t comps) {
  NextAttr a;
  if (!data.empty() && comps > 0) {
    a.data = &data;
    a.interp = interp;
    a.comps = comps;
  }
  return a;
}

// Find a generic primvar by name and expose it as a NextAttr.
NextAttr FindNextPrimvar(const tydn::RenderMesh& m, const char* name,
                         uint32_t comps) {
  for (const tydn::VertexAttribute& pv : m.primvars) {
    if (pv.name != name) continue;
    NextAttr a;
    a.indices = pv.has_indices() ? &pv.indices : nullptr;
    a.interp = pv.interpolation;
    switch (pv.format) {
      case tydn::VertexFormat::Float:
      case tydn::VertexFormat::Vec2:
      case tydn::VertexFormat::Vec3:
      case tydn::VertexFormat::Vec4:
        if (pv.float_data.empty()) continue;
        a.data = &pv.float_data;
        break;
      case tydn::VertexFormat::Matrix33:
      case tydn::VertexFormat::Matrix44:
        if (pv.float_data.empty()) continue;
        a.data = &pv.float_data;
        break;
      case tydn::VertexFormat::Int:
      case tydn::VertexFormat::IVec2:
      case tydn::VertexFormat::IVec3:
      case tydn::VertexFormat::IVec4:
        if (pv.int_data.empty()) continue;
        a.int_data = &pv.int_data;
        break;
      case tydn::VertexFormat::UInt:
      case tydn::VertexFormat::UVec2:
      case tydn::VertexFormat::UVec3:
      case tydn::VertexFormat::UVec4:
        if (pv.uint_data.empty()) continue;
        a.uint_data = &pv.uint_data;
        break;
    }
    a.comps = comps;
    return a;
  }
  return NextAttr{};
}

// Build interleaved DrawVertex geometry (mesh-LOCAL space) + indices from a
// tydra-next RenderMesh. Returns false if there is no renderable geometry.
//
// tydra-next keeps every primvar in its AUTHORED interpolation (constant /
// uniform / vertex / varying / faceVarying) and hands us
// `triangulated_face_vertex_indices` to index the faceVarying ones against the
// triangulated topology. Production USD overwhelmingly authors faceVarying `st`
// and `normals`, so we resolve all five interpolations here and WELD the
// corners: a point is split into multiple DrawVertex entries only where its
// attributes actually differ (a UV seam or a hard edge). Naive per-corner
// expansion would multiply a quad mesh's vertex count ~4x, which is exactly the
// VRAM we are trying not to spend.
//
// `vertexToPoint` receives the source point id per emitted vertex, since the
// weld breaks the old vertex-i == point-i invariant that the skinning,
// blendshape, and wireframe passes relied on.
bool FillFlatGeometry(const tydn::RenderMesh& m, DrawMeshCPU* dm,
                      std::vector<uint32_t>* vertexToPoint) {
  const size_t np = m.point_count();
  const size_t ncorners = m.triangulated_indices.size();
  if (np == 0 || ncorners < 3) return false;

  // Authored doubleSided reaches the renderers (GL back-face-culls
  // single-sided meshes; VK matches via dynamic cull mode). Without this every
  // --next mesh defaulted to single-sided regardless of what was authored.
  dm->doubleSided = m.double_sided;

  // faceVarying lookups need the corner remap; without it, treat faceVarying
  // attributes as absent rather than reading garbage.
  const bool haveCornerMap =
      m.triangulated_face_vertex_indices.size() == ncorners;

  const NextAttr nrm = MakeNextAttr(m.normals, m.normals_interp, 3);
  const NextAttr uv0 = MakeNextAttr(m.texcoords_0, m.texcoords_0_interp, 2);
  const NextAttr uv1 = MakeNextAttr(m.texcoords_1, m.texcoords_1_interp, 2);
  // displayColor is color3f, but tydra-next also accepts a 4-component authoring
  // (rgba); the 4th component is folded into the alpha channel below. Component
  // count follows from the element count its interpolation implies.
  auto expectedElems = [&](tydn::Interpolation interp) -> size_t {
    switch (interp) {
      case tydn::Interpolation::Constant: return 1;
      case tydn::Interpolation::Uniform: return m.face_count();
      case tydn::Interpolation::FaceVarying: return m.face_vertex_indices.size();
      default: return np;
    }
  };
  uint32_t colorComps = 3;
  if (!m.colors.empty()) {
    const size_t elems = expectedElems(m.colors_interp);
    if (elems > 0 && m.colors.size() == elems * 4) colorComps = 4;
  }
  const NextAttr col = MakeNextAttr(m.colors, m.colors_interp, colorComps);
  // displayOpacity has a dedicated tydra-next channel. Older converter builds
  // left it in the generic primvar bag, so retain that as a compatibility
  // fallback instead of silently dropping authored vertex alpha.
  const NextAttr builtinOpacity =
      MakeNextAttr(m.opacities, m.opacities_interp, 1);
  const NextAttr opacity = builtinOpacity
                               ? builtinOpacity
                               : FindNextPrimvar(m, "displayOpacity", 1);
  // Tangents are computed only for normal-mapped meshes (see the tangent-aware
  // converter in LoadUSDViaNext); xyzw with w = handedness.
  const NextAttr tan = MakeNextAttr(m.tangents, m.tangents_interp, 4);

  struct GenericGeomProp {
    std::string name;
    NextAttr attr;
    uint32_t components{0};
  };
  std::vector<GenericGeomProp> genericProps;
  for (const tydn::VertexAttribute& pv : m.primvars) {
    uint32_t components = 0;
    switch (pv.format) {
      case tydn::VertexFormat::Float: components = 1; break;
      case tydn::VertexFormat::Vec2: components = 2; break;
      case tydn::VertexFormat::Vec3: components = 3; break;
      case tydn::VertexFormat::Vec4: components = 4; break;
      case tydn::VertexFormat::Int:
      case tydn::VertexFormat::UInt:
        components = 1;
        break;
      case tydn::VertexFormat::IVec2:
      case tydn::VertexFormat::UVec2:
        components = 2;
        break;
      case tydn::VertexFormat::IVec3:
      case tydn::VertexFormat::UVec3:
        components = 3;
        break;
      case tydn::VertexFormat::IVec4:
      case tydn::VertexFormat::UVec4:
        components = 4;
        break;
      case tydn::VertexFormat::Matrix33: components = 9; break;
      case tydn::VertexFormat::Matrix44: components = 16; break;
      default: break;  // matrix primvars do not fit the packed vec4 ABI
    }
    const bool hasData = !pv.float_data.empty() || !pv.int_data.empty() ||
                         !pv.uint_data.empty();
    if (components == 0 || !hasData ||
        pv.name == "displayColor" || pv.name == "displayOpacity" ||
        pv.name == "normals" || pv.name == "tangents" ||
        pv.name == "binormals") {
      continue;
    }
    GenericGeomProp prop;
    prop.name = pv.name;
    prop.components = components;
    prop.attr.indices = pv.has_indices() ? &pv.indices : nullptr;
    prop.attr.interp = pv.interpolation;
    prop.attr.comps = components;
    switch (pv.format) {
      case tydn::VertexFormat::Float:
      case tydn::VertexFormat::Vec2:
      case tydn::VertexFormat::Vec3:
      case tydn::VertexFormat::Vec4:
        prop.attr.data = &pv.float_data;
        break;
      case tydn::VertexFormat::Int:
      case tydn::VertexFormat::IVec2:
      case tydn::VertexFormat::IVec3:
      case tydn::VertexFormat::IVec4:
        prop.attr.int_data = &pv.int_data;
        break;
      case tydn::VertexFormat::UInt:
      case tydn::VertexFormat::UVec2:
      case tydn::VertexFormat::UVec3:
      case tydn::VertexFormat::UVec4:
        prop.attr.uint_data = &pv.uint_data;
        break;
      case tydn::VertexFormat::Matrix33:
      case tydn::VertexFormat::Matrix44:
        prop.attr.data = &pv.float_data;
        break;
    }
    genericProps.push_back(std::move(prop));
  }

  auto usesFaceVarying = [&](const NextAttr& a) {
    return a && a.interp == tydn::Interpolation::FaceVarying;
  };
  if (!haveCornerMap &&
      (usesFaceVarying(nrm) || usesFaceVarying(uv0) || usesFaceVarying(uv1) ||
       usesFaceVarying(col) || usesFaceVarying(opacity) ||
       usesFaceVarying(tan) || std::any_of(genericProps.begin(), genericProps.end(),
                                           [&](const GenericGeomProp& p) {
                                             return usesFaceVarying(p.attr);
                                           }))) {
    return false;  // triangulation did not produce a usable corner remap
  }

  // Authored normals (in any interpolation) -> smooth shading; otherwise shade
  // geometrically in the shader (screen-derivative normal), which reads
  // correctly on hard surfaces instead of being smeared by averaged normals.
  dm->geometricNormal = !static_cast<bool>(nrm);

  // Uniform (per-face) attributes need a corner -> face lookup. Only pay for it
  // when something is actually authored that way.
  auto usesUniform = [&](const NextAttr& a) {
    return a && a.interp == tydn::Interpolation::Uniform;
  };
  std::vector<uint32_t> cornerToFace;
  if (usesUniform(nrm) || usesUniform(uv0) || usesUniform(uv1) ||
      usesUniform(col) || usesUniform(opacity) || usesUniform(tan) ||
      std::any_of(genericProps.begin(), genericProps.end(),
                  [&](const GenericGeomProp& p) { return usesUniform(p.attr); })) {
    const size_t nfaces = m.face_vertex_counts.size();
    size_t authoredCorners = 0;
    for (size_t f = 0; f < nfaces; ++f) authoredCorners += m.face_vertex_counts[f];
    cornerToFace.resize(authoredCorners);
    size_t off = 0;
    for (size_t f = 0; f < nfaces; ++f) {
      const uint32_t c = m.face_vertex_counts[f];
      for (uint32_t k = 0; k < c && off < authoredCorners; ++k, ++off) {
        cornerToFace[off] = static_cast<uint32_t>(f);
      }
    }
  }

  const bool wantColors = static_cast<bool>(col);
  const bool wantAlpha =
      static_cast<bool>(opacity) || (wantColors && colorComps == 4);
  const bool wantUv1 = static_cast<bool>(uv1);
  const bool wantTangents = static_cast<bool>(tan);

  dm->name = m.name;
  dm->absPath = m.prim_path;

  // Weld: per point, a short chain of already-emitted variants. Almost every
  // point has one; seams add a second. This is much cheaper in both time and
  // peak memory than hashing a full attribute tuple per corner.
  std::vector<int32_t> firstVariant(np, -1);
  std::vector<int32_t> nextVariant;
  std::vector<uint32_t>& v2p = *vertexToPoint;
  v2p.clear();

  nextVariant.reserve(np);
  v2p.reserve(np);
  dm->vertices.reserve(np);
  dm->indices.resize(ncorners);
  dm->sourceFaceId.resize(ncorners / 3, 0);
  dm->geomProps.clear();
  dm->geomProps.reserve(genericProps.size());
  for (const GenericGeomProp& prop : genericProps) {
    DrawGeomPropCPU& dst = dm->geomProps.emplace_back();
    dst.name = prop.name;
    dst.components = prop.components;
    dst.values.reserve(np * prop.components);
  }

  auto sameFloats = [](const float* a, const float* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      if (a[i] != b[i]) return false;
    }
    return true;
  };

  for (size_t c = 0; c < ncorners; ++c) {
    const uint32_t pid = m.triangulated_indices[c];
    if (pid >= np) {  // sanitized upstream, but never index out of bounds
      dm->indices[c] = 0;
      continue;
    }
    const uint32_t cornerId =
        haveCornerMap ? m.triangulated_face_vertex_indices[c] : pid;
    const uint32_t faceId = (cornerToFace.empty() || cornerId >= cornerToFace.size())
                                ? 0u
                                : cornerToFace[cornerId];
    const uint32_t sourceFaceId =
        faceId < m.subdivision_face_source.size()
            ? m.subdivision_face_source[faceId]
            : faceId;

    float n[3], t0[2], t1[2], rgb[4], a = 1.0f, tg[4];
    std::vector<std::vector<float>> geomValues;
    geomValues.reserve(genericProps.size());
    for (const GenericGeomProp& prop : genericProps) {
      geomValues.emplace_back(prop.components, 0.0f);
    }
    nrm.read(pid, cornerId, faceId, 3, n);
    uv0.read(pid, cornerId, faceId, 2, t0);
    uv1.read(pid, cornerId, faceId, 2, t1);
    if (wantColors) {
      col.read(pid, cornerId, faceId, colorComps, rgb);
    } else {
      rgb[0] = rgb[1] = rgb[2] = 1.0f;
      rgb[3] = 1.0f;
    }
    if (colorComps == 4 && wantColors) a = rgb[3];
    if (opacity) {
      float o[1];
      opacity.read(pid, cornerId, faceId, 1, o);
      a = o[0];
    }
    if (wantTangents) tan.read(pid, cornerId, faceId, 4, tg);
    for (size_t pi = 0; pi < genericProps.size(); ++pi) {
      genericProps[pi].attr.read(pid, cornerId, faceId,
                                 genericProps[pi].components,
                                 geomValues[pi].data());
    }

    // Flip V: USD `st` has v=0 at the image bottom, but decoded images are
    // top-row-first and uploaded so v=0 samples the top, so invert here (same as
    // the legacy path in mesh_build.cc).
    const float u0 = t0[0], v0 = 1.0f - t0[1];
    const float u1 = t1[0], v1 = 1.0f - t1[1];

    int32_t found = -1;
    for (int32_t vi = firstVariant[pid]; vi >= 0; vi = nextVariant[size_t(vi)]) {
      const DrawVertex& cand = dm->vertices[size_t(vi)];
      if (cand.nx != n[0] || cand.ny != n[1] || cand.nz != n[2]) continue;
      if (cand.u != u0 || cand.v != v0) continue;
      if (wantUv1 && (dm->uv1[size_t(vi) * 2 + 0] != u1 ||
                      dm->uv1[size_t(vi) * 2 + 1] != v1)) {
        continue;
      }
      if (wantColors &&
          !sameFloats(&dm->vertexColors[size_t(vi) * 3], rgb, 3)) {
        continue;
      }
      if (wantAlpha && dm->vertexAlpha[size_t(vi)] != a) continue;
      if (wantTangents &&
          !sameFloats(&dm->tangents[size_t(vi) * 3], tg, 3)) {
        continue;
      }
      bool geomPropSame = true;
      for (size_t pi = 0; pi < genericProps.size(); ++pi) {
        const DrawGeomPropCPU& stored = dm->geomProps[pi];
        if (!sameFloats(&stored.values[size_t(vi) * stored.components],
                        geomValues[pi].data(), stored.components)) {
          geomPropSame = false;
          break;
        }
      }
      if (!geomPropSame) continue;
      found = vi;
      break;
    }

    if (found < 0) {
      found = static_cast<int32_t>(dm->vertices.size());
      DrawVertex v;
      v.px = m.points[3 * pid + 0];
      v.py = m.points[3 * pid + 1];
      v.pz = m.points[3 * pid + 2];
      v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
      v.u = u0; v.v = v0;
      dm->vertices.push_back(v);
      if (wantUv1) { dm->uv1.push_back(u1); dm->uv1.push_back(v1); }
      if (wantColors) {
        dm->vertexColors.push_back(rgb[0]);
        dm->vertexColors.push_back(rgb[1]);
        dm->vertexColors.push_back(rgb[2]);
      }
      if (wantAlpha) dm->vertexAlpha.push_back(a);
      if (wantTangents) {
        dm->tangents.push_back(tg[0]);
        dm->tangents.push_back(tg[1]);
        dm->tangents.push_back(tg[2]);
        // Binormal from the handedness sign, so the shader gets a full basis.
        const float w = tg[3] < 0.0f ? -1.0f : 1.0f;
        dm->binormals.push_back((n[1] * tg[2] - n[2] * tg[1]) * w);
        dm->binormals.push_back((n[2] * tg[0] - n[0] * tg[2]) * w);
        dm->binormals.push_back((n[0] * tg[1] - n[1] * tg[0]) * w);
      }
      for (size_t pi = 0; pi < genericProps.size(); ++pi) {
        for (uint32_t cidx = 0; cidx < genericProps[pi].components; ++cidx)
          dm->geomProps[pi].values.push_back(geomValues[pi][cidx]);
      }
      nextVariant.push_back(firstVariant[pid]);
      v2p.push_back(pid);
      firstVariant[pid] = found;
    }
    dm->indices[c] = static_cast<uint32_t>(found);
    // Preserve the authored face for every generated triangle. Consumers can
    // then discard triangulation diagonals even if the explicit perimeter EBO
    // is unavailable after material batching or backend conversion.
    if ((c % 3) == 0 && (c / 3) < dm->sourceFaceId.size()) {
      dm->sourceFaceId[c / 3] = sourceFaceId;
    }
  }

  // TriangulateMesh records the generated-triangle range of every authored
  // face. Prefer that compact mapping; cornerToFace only exists when a uniform
  // primvar required it above.
  if (m.face_triangle_offsets.size() == m.face_count() + 1) {
    for (size_t f = 0; f < m.face_count(); ++f) {
      const size_t begin = std::min<size_t>(m.face_triangle_offsets[f],
                                            dm->sourceFaceId.size());
      const size_t end = std::min<size_t>(m.face_triangle_offsets[f + 1],
                                          dm->sourceFaceId.size());
      for (size_t t = begin; t < end; ++t) {
        dm->sourceFaceId[t] =
            f < m.subdivision_face_source.size()
                ? m.subdivision_face_source[f]
                : static_cast<uint32_t>(f);
      }
    }
  }

  if (dm->vertices.empty()) return false;
  dm->submeshes.push_back(
      DrawSubmesh{0, static_cast<uint32_t>(dm->indices.size()), 0});

  // Original-polygon wireframe edges: the perimeter of each USD face, from the
  // pre-triangulation topology. This shows quads/ngons (not triangulation
  // diagonals) and is correct even for double-sided meshes (whose triangulation
  // doubles the tri count, defeating any per-triangle scheme). Point ids map to
  // their first emitted variant -- every variant of a point shares its position,
  // so any of them draws the same edge.
  {
    const std::vector<uint32_t> fvc = m.face_vertex_counts.flatten();
    const std::vector<uint32_t> fvi = m.face_vertex_indices.flatten();
    // After CPU subdivision, face_vertex_counts describes the refined mesh.
    // subdivision_face_source retains the authored face for every refined
    // triangle; discard edges shared by triangles from the same authored face
    // so the control cage remains coarse.
    bool refinedTopology = false;
    if (m.subdivision_face_source.size() == m.face_count() &&
        !m.subdivision_face_source.empty()) {
      std::unordered_set<uint32_t> sourceFaces;
      sourceFaces.insert(m.subdivision_face_source.begin(),
                         m.subdivision_face_source.end());
      refinedTopology = sourceFaces.size() < m.subdivision_face_source.size();
    }
    if (refinedTopology && dm->sourceFaceId.size() == dm->indices.size() / 3) {
      struct EdgeUse { uint32_t face{0}; uint32_t count{0}; bool boundary{false}; };
      std::unordered_map<uint64_t, EdgeUse> uses;
      uses.reserve(dm->indices.size());
      auto edgeKey = [](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32u) | uint64_t(b);
      };
      for (size_t t = 0; t < dm->sourceFaceId.size(); ++t) {
        const uint32_t v[3] = {dm->indices[t * 3u], dm->indices[t * 3u + 1u],
                               dm->indices[t * 3u + 2u]};
        for (int e = 0; e < 3; ++e) {
          EdgeUse& use = uses[edgeKey(v[e], v[(e + 1) % 3])];
          if (use.count == 0) use.face = dm->sourceFaceId[t];
          else if (use.face != dm->sourceFaceId[t]) use.boundary = true;
          ++use.count;
        }
      }
      for (const auto& entry : uses) {
        const EdgeUse& use = entry.second;
        if (use.count != 1 && !use.boundary) continue;
        dm->wireframeIndices.push_back(uint32_t(entry.first >> 32u));
        dm->wireframeIndices.push_back(uint32_t(entry.first));
      }
    } else if (!fvc.empty() && !fvi.empty()) {
      std::unordered_set<uint64_t> seen;
      seen.reserve(fvi.size());
      std::vector<uint32_t>& wire = dm->wireframeIndices;
      wire.reserve(fvi.size() * 2);
      size_t off = 0;
      bool ok = true;
      for (uint32_t c : fvc) {
        if (off + c > fvi.size()) { ok = false; break; }
        for (uint32_t k = 0; k < c; ++k) {
          const uint32_t pa = fvi[off + k];
          const uint32_t pb = fvi[off + (k + 1u) % c];
          if (pa == pb || pa >= np || pb >= np) continue;
          const int32_t va = firstVariant[pa], vb = firstVariant[pb];
          if (va < 0 || vb < 0) continue;
          const uint32_t a = static_cast<uint32_t>(va);
          const uint32_t b = static_cast<uint32_t>(vb);
          const uint64_t key =
              a < b ? (uint64_t(a) << 32 | b) : (uint64_t(b) << 32 | a);
          if (seen.insert(key).second) { wire.push_back(a); wire.push_back(b); }
        }
        off += c;
      }
      if (!ok) wire.clear();
    }
  }
  return true;
}

bool MaterialUsesPtex(const DrawScene& draw, int materialId) {
  if (materialId < 0 || static_cast<size_t>(materialId) >= draw.materials.size())
    return false;
  const DrawMaterialCPU& m = draw.materials[static_cast<size_t>(materialId)];
  return m.baseColorSample.isPtex || m.metallicSample.isPtex ||
         m.roughnessSample.isPtex || m.normalSample.isPtex ||
         m.emissiveSample.isPtex || m.opacitySample.isPtex ||
         m.occlusionSample.isPtex || m.specularColorSample.isPtex ||
         m.coatWeightSample.isPtex || m.coatColorSample.isPtex ||
         m.coatRoughnessSample.isPtex || m.displacementSample.isPtex;
}

// Expand a Ptex mesh to independent triangle corners and attach intrinsic quad
// coordinates. Ptex face ids are texture-local and therefore deliberately do
// not receive the source-face AOV's batching offset.
bool ExpandPtexCorners(const tydn::RenderMesh& source, DrawMeshCPU* mesh) {
  if (!mesh || mesh->indices.empty() ||
      source.triangulated_face_vertex_indices.size() != mesh->indices.size() ||
      mesh->sourceFaceId.size() != mesh->indices.size() / 3) {
    return false;
  }
  const std::vector<uint32_t> counts = source.face_vertex_counts.flatten();
  size_t authoredCorners = 0;
  for (uint32_t count : counts) authoredCorners += count;
  std::vector<uint8_t> cornerOrdinal(authoredCorners, 0);
  std::vector<uint32_t> cornerFace(authoredCorners, 0);
  size_t corner = 0;
  for (size_t face = 0; face < counts.size(); ++face) {
    if (counts[face] != 4) return false;
    for (uint32_t ordinal = 0; ordinal < counts[face]; ++ordinal, ++corner) {
      cornerOrdinal[corner] = static_cast<uint8_t>(ordinal);
      cornerFace[corner] = static_cast<uint32_t>(face);
    }
  }
  if (corner != authoredCorners) return false;

  const size_t oldVertexCount = mesh->vertices.size();
  const bool hasColor = mesh->vertexColors.size() == oldVertexCount * 3;
  const bool hasAlpha = mesh->vertexAlpha.size() == oldVertexCount;
  const bool hasTangent = mesh->tangents.size() == oldVertexCount * 3;
  const bool hasBinormal = mesh->binormals.size() == oldVertexCount * 3;
  const bool hasUv1 = mesh->uv1.size() == oldVertexCount * 2;
  const bool hasInfluence = mesh->morphInfluence.size() == oldVertexCount;
  const bool hasJoint = mesh->jointIdx.size() == oldVertexCount * 4 &&
                        mesh->jointWt.size() == oldVertexCount * 4;
  const bool hasExtended =
      mesh->influenceOffsetCount.size() == oldVertexCount * 2;
  const bool hasMorph = mesh->morphOffsetCount.size() == oldVertexCount * 2;

  DrawMeshCPU expanded = *mesh;
  expanded.vertices.clear();
  expanded.indices.clear();
  expanded.vertexColors.clear();
  expanded.vertexAlpha.clear();
  expanded.tangents.clear();
  expanded.binormals.clear();
  expanded.uv1.clear();
  for (DrawGeomPropCPU& prop : expanded.geomProps) prop.values.clear();
  expanded.morphInfluence.clear();
  expanded.jointIdx.clear();
  expanded.jointWt.clear();
  expanded.influenceOffsetCount.clear();
  expanded.morphOffsetCount.clear();
  expanded.wireframeIndices.clear();
  expanded.rtDisplacedVertices.clear();
  const size_t corners = mesh->indices.size();
  expanded.vertices.reserve(corners);
  expanded.indices.reserve(corners);

  const float intrinsic[4][2] = {
      {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
  std::vector<uint32_t> newToOld;
  newToOld.reserve(corners);
  for (size_t i = 0; i < corners; ++i) {
    const uint32_t old = mesh->indices[i];
    if (old >= oldVertexCount) return false;
    const uint32_t authored = source.triangulated_face_vertex_indices[i];
    if (authored >= cornerOrdinal.size()) return false;
    const uint8_t ordinal = cornerOrdinal[authored];
    const uint32_t face = cornerFace[authored];
    if (i / 3 >= mesh->sourceFaceId.size() ||
        face != mesh->sourceFaceId[i / 3]) {
      return false;
    }
    expanded.indices.push_back(static_cast<uint32_t>(i));
    expanded.vertices.push_back(mesh->vertices[old]);
    // Ptex materials use intrinsic face coordinates, not an authored primvar.
    // Isolated Ptex batches therefore carry these in the ordinary UV0 lane so
    // every existing raster and RT interpolation path sees the same values.
    expanded.vertices.back().u = intrinsic[ordinal][0];
    expanded.vertices.back().v = intrinsic[ordinal][1];
    newToOld.push_back(old);
    if (hasColor) {
      for (size_t k = 0; k < 3; ++k)
        expanded.vertexColors.push_back(mesh->vertexColors[size_t(old) * 3 + k]);
    }
    if (hasAlpha) expanded.vertexAlpha.push_back(mesh->vertexAlpha[old]);
    if (hasTangent) {
      for (size_t k = 0; k < 3; ++k)
        expanded.tangents.push_back(mesh->tangents[size_t(old) * 3 + k]);
    }
    if (hasBinormal) {
      for (size_t k = 0; k < 3; ++k)
        expanded.binormals.push_back(mesh->binormals[size_t(old) * 3 + k]);
    }
    if (hasUv1) {
      expanded.uv1.push_back(mesh->uv1[size_t(old) * 2]);
      expanded.uv1.push_back(mesh->uv1[size_t(old) * 2 + 1]);
    }
    for (size_t pi = 0; pi < expanded.geomProps.size(); ++pi) {
      const DrawGeomPropCPU& source = mesh->geomProps[pi];
      if (source.components == 0 ||
          (size_t(old) + 1) * source.components > source.values.size()) {
        expanded.geomProps[pi].values.resize(
            expanded.geomProps[pi].values.size() + source.components, 0.0f);
        continue;
      }
      const size_t begin = size_t(old) * source.components;
      expanded.geomProps[pi].values.insert(
          expanded.geomProps[pi].values.end(), source.values.begin() + begin,
          source.values.begin() + begin + source.components);
    }
    if (hasInfluence)
      expanded.morphInfluence.push_back(mesh->morphInfluence[old]);
    if (hasJoint) {
      for (size_t k = 0; k < 4; ++k) {
        expanded.jointIdx.push_back(mesh->jointIdx[size_t(old) * 4 + k]);
        expanded.jointWt.push_back(mesh->jointWt[size_t(old) * 4 + k]);
      }
    }
    if (hasExtended) {
      expanded.influenceOffsetCount.push_back(
          mesh->influenceOffsetCount[size_t(old) * 2]);
      expanded.influenceOffsetCount.push_back(
          mesh->influenceOffsetCount[size_t(old) * 2 + 1]);
    }
    if (hasMorph) {
      expanded.morphOffsetCount.push_back(
          mesh->morphOffsetCount[size_t(old) * 2]);
      expanded.morphOffsetCount.push_back(
          mesh->morphOffsetCount[size_t(old) * 2 + 1]);
    }
  }

  // Remap the optional CPU morph targets to the expanded vertex order.
  for (MorphTargetCPU& target : expanded.morphs) {
    std::unordered_map<uint32_t, size_t> oldEntry;
    for (size_t i = 0; i < target.vtx.size(); ++i) oldEntry[target.vtx[i]] = i;
    MorphTargetCPU remapped;
    remapped.name = target.name;
    remapped.inbetweens.resize(target.inbetweens.size());
    for (size_t s = 0; s < target.inbetweens.size(); ++s) {
      remapped.inbetweens[s].weight = target.inbetweens[s].weight;
    }
    for (size_t vertex = 0; vertex < newToOld.size(); ++vertex) {
      const auto it = oldEntry.find(newToOld[vertex]);
      if (it == oldEntry.end()) continue;
      const size_t entry = it->second;
      remapped.vtx.push_back(static_cast<uint32_t>(vertex));
      for (size_t k = 0; k < 3; ++k)
        remapped.dpos.push_back(target.dpos[entry * 3 + k]);
      for (size_t s = 0; s < target.inbetweens.size(); ++s) {
        for (size_t k = 0; k < 3; ++k) {
          remapped.inbetweens[s].dpos.push_back(
              target.inbetweens[s].dpos[entry * 3 + k]);
        }
      }
    }
    target = std::move(remapped);
  }
  *mesh = std::move(expanded);
  return true;
}

void TransformDrawVertices(const double world[16], DrawMeshCPU* mesh) {
  float m[16];
  for (int k = 0; k < 16; ++k) m[k] = static_cast<float>(world[k]);
  for (DrawVertex& v : mesh->vertices) {
    float wp[3], wn[3];
    for (int c = 0; c < 3; ++c) {
      wp[c] = v.px * m[c] + v.py * m[4 + c] + v.pz * m[8 + c] + m[12 + c];
      wn[c] = v.nx * m[c] + v.ny * m[4 + c] + v.nz * m[8 + c];
    }
    v.px = wp[0]; v.py = wp[1]; v.pz = wp[2];
    const float nl =
        std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
    if (nl > 1e-12f) {
      v.nx = wn[0] / nl; v.ny = wn[1] / nl; v.nz = wn[2] / nl;
    } else {
      v.nx = v.ny = v.nz = 0.0f;
    }
  }
}

struct Bounds {
  bool has = false;
  float mn[3]{0, 0, 0}, mx[3]{0, 0, 0};
  void add(const float p[3]) {
    if (!has) { for (int k = 0; k < 3; ++k) mn[k] = mx[k] = p[k]; has = true; }
    else {
      for (int k = 0; k < 3; ++k) {
        mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]);
      }
    }
  }
};

// Resolve a prim's inherited USD `purpose` (default/render/proxy/guide): the
// nearest authored, non-"default" purpose walking self->ancestors, else
// "default". Matches mesh_build.cc ResolveInheritedPurpose for the next stage.
std::string ResolveNextPurpose(const tnext::UsdPrim& source) {
  // Walk UsdPrim parents rather than rebuilding string paths. Instance-proxy
  // parents carry the prototype/instance remapping needed to inherit purpose
  // from the authored instance hierarchy.
  for (tnext::UsdPrim prim = source; prim.IsValid(); prim = prim.GetParent()) {
    // Inspect only the authored local opinion. GetPropertyValue also exposes
    // the schema fallback "default", which must not hide an ancestor purpose.
    const tnext::PrimSpec* spec = prim.GetPrimSpec();
    const tnext::Value* v = spec ? spec->property_value("purpose") : nullptr;
    if (v) {
      if (const std::string* t = v->as_token()) {
        if (*t == "render" || *t == "proxy" || *t == "guide") return *t;
        if (*t == "default") return "default";
      }
    }
  }
  return "default";
}

std::string ResolveNextPurpose(const tnext::Stage& stage,
                               const std::string& abs) {
  return ResolveNextPurpose(stage.GetPrimAtPath(abs));
}

// See mesh_build.cc ResolveModelRoot. Purpose alternatives belong to one model,
// not to an arbitrary common ancestor such as /World.
std::string ResolveNextModelRoot(const tnext::UsdPrim& source) {
  for (tnext::UsdPrim prim = source; prim.IsValid(); prim = prim.GetParent()) {
    const std::string& kind = prim.GetMeta().kind();
    if (!kind.empty() && kind != "group") return prim.GetPath().str();
  }
  return {};
}

// Unreal renders many thin architectural/foliage assets two-sided but its USD
// exporter does not always author the corresponding Mesh doubleSided opinion.
// Apply that compatibility fallback only inside an Unreal assetInfo hierarchy;
// an explicit USD opinion, including false, always wins.
bool NeedsUnrealDoubleSidedFallback(const tnext::UsdPrim& meshPrim) {
  if (!meshPrim.IsValid()) return false;
  if (const tnext::PrimSpec* spec = meshPrim.GetPrimSpec()) {
    if (spec->property_value("doubleSided")) return false;
  }
  for (tnext::UsdPrim prim = meshPrim; prim.IsValid(); prim = prim.GetParent()) {
    const tnext::Dict* dict = prim.GetMeta().assetInfo().as_dictionary();
    const tnext::Value* unreal = dict ? dict->find("unreal") : nullptr;
    if (unreal && unreal->as_dictionary()) return true;
  }
  return false;
}

// Resolve the SkelAnimation that drives a mesh's blendshapes, returning a
// blendShape-name -> weight map. The next converter emits no skel/morph data, so
// we read straight from the stage: prefer a `skel:animationSource` relationship
// (walking the mesh's ancestors, which is where SkelRoot/Skeleton authors it),
// else fall back to scanning the enclosing SkelRoot subtree for a SkelAnimation
// prim. Empty map => everything stays at rest. `time` picks the time sample.
std::unordered_map<std::string, float> ResolveBlendWeights(
    const tnext::Stage& stage, const tnext::UsdPrim& meshPrim, double time) {
  std::unordered_map<std::string, float> out;

  // Find the SkelAnimation prim.
  tnext::UsdPrim anim;
  tnext::UsdPrim skelRoot;
  for (tnext::UsdPrim a = meshPrim; a.IsValid(); a = a.GetParent()) {
    if (const std::vector<tnext::Path>* src =
            a.GetRelationship("skel:animationSource")) {
      if (!src->empty()) {
        tnext::UsdPrim cand = stage.GetPrimAtPath((*src)[0]);
        if (cand.IsValid() && cand.GetTypeName() == "SkelAnimation") {
          anim = cand;
          break;
        }
      }
    }
    if (a.GetTypeName() == "SkelRoot") skelRoot = a;
    if (a.GetPath().str() == "/") break;
  }
  // Fallback: first SkelAnimation under the enclosing SkelRoot.
  if (!anim.IsValid() && skelRoot.IsValid()) {
    std::function<tnext::UsdPrim(const tnext::UsdPrim&)> find =
        [&](const tnext::UsdPrim& p) -> tnext::UsdPrim {
      if (p.GetTypeName() == "SkelAnimation") return p;
      for (const tnext::UsdPrim& c : p.GetChildren()) {
        tnext::UsdPrim r = find(c);
        if (r.IsValid()) return r;
      }
      return tnext::UsdPrim();
    };
    anim = find(skelRoot);
  }
  if (!anim.IsValid()) return out;

  const std::vector<std::string> names = ReadTokens(anim, "blendShapes", time);
  // Linearly-interpolated weights so morph animates smoothly between time
  // samples (static scenes fall back to the default opinion).
  const std::vector<float> weights =
      ReadFloatsLerp(anim, "blendShapeWeights", time);
  for (size_t i = 0; i < names.size() && i < weights.size(); ++i)
    out[names[i]] = weights[i];
  return out;
}

// In-between samples of a `--next` BlendShape prim, read from its `inbetweens:*`
// attributes (vector3f[] offsets parallel to the prim's pointIndices, plus a
// `weight` attr-meta). Returned sorted ascending by weight. Mirrors
// ReadInbetweensFromPrim in skinning.cc for the next stage.
std::vector<std::pair<float, std::vector<float>>> ReadInbetweens(
    const tnext::UsdPrim& bs, double time) {
  std::vector<std::pair<float, std::vector<float>>> out;
  const tnext::PrimSpec* spec = bs.GetPrimSpec();
  if (!spec) return out;
  for (const std::string& name : bs.GetPropertyNames()) {
    if (name.rfind("inbetweens:", 0) != 0) continue;  // namespace prefix
    const tnext::PropMeta* pm = spec->property_meta(name);
    if (!pm || !(pm->authored & tnext::PropMeta::kWeight)) continue;
    std::vector<float> offs = ReadFloats(bs, name.c_str(), time);
    if (offs.empty()) continue;
    out.emplace_back(static_cast<float>(pm->weight), std::move(offs));
  }
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

// Bracket a target weight `w` within the implied sample table {0, ibWeights...,
// 1} (ibWeights ascending). Returns table indices [lo, hi] (0 == implicit rest,
// last == primary) + lerp parameter t (extrapolates outside [0,1]). Identical to
// FindMorphBracket in skinning.cc -- keeps the static bake bit-for-bit with the
// GPU-morph coeff eval. With no in-betweens it degrades to {lo:0, hi:1, t:w},
// i.e. a plain linear primary scale.
struct MorphBracket { int lo; int hi; float t; };
MorphBracket FindMorphBracket(const std::vector<float>& ibWeights, float w) {
  const int N = static_cast<int>(ibWeights.size());
  auto wAt = [&](int i) -> float {
    return i == 0 ? 0.0f : (i == N + 1 ? 1.0f : ibWeights[i - 1]);
  };
  int hi = 1;
  while (hi < N + 1 && w > wAt(hi)) ++hi;
  const int lo = hi - 1;
  const float denom = wAt(hi) - wAt(lo);
  const float t = denom > 1e-12f ? (w - wAt(lo)) / denom : 0.0f;
  return {lo, hi, t};
}

// Bake blendshape (morph) targets into the prototype's local vertex positions
// once at load. The `--next` instanced path has no GPU morph, so resolving the
// morph into geometry here makes the existing flat instanced GL/VK path render N
// instances of the morphed prototype with no shader/attribute changes. The morph
// is per-prototype (shared by all instances), and `--next` is a static preview,
// so load-time weights suffice. `dm->vertices` is point-indexed (vertex i == point
// i), matching the BlendShape `pointIndices` which index authored points.
//
// USD in-between samples are interpolated (piecewise-lerp via FindMorphBracket,
// matching the GPU-morph coeff eval); without in-betweens this is a plain linear
// primary scale. Limitation (acceptable for a static preview): load-time weights
// only -- no animated morph. Authored smooth normals are recomputed below.
void RecomputeSmoothNormalsNext(DrawMeshCPU* dm);

void BakeBlendShapes(const tnext::Stage& stage, const tnext::UsdPrim& meshPrim,
                     double time, DrawMeshCPU* dm,
                     const std::vector<uint32_t>& vertexToPoint,
                     size_t numPoints) {
  const std::vector<std::string> shapeNames =
      ReadTokens(meshPrim, "skel:blendShapes", time);
  const std::vector<tnext::Path>* targets =
      meshPrim.GetRelationship("skel:blendShapeTargets");
  if (shapeNames.empty() || !targets || targets->empty()) return;

  const std::unordered_map<std::string, float> weights =
      ResolveBlendWeights(stage, meshPrim, time);
  if (weights.empty()) return;

  const size_t nv = dm->vertices.size();
  if (nv == 0 || numPoints == 0) return;
  if (!vertexToPoint.empty() && vertexToPoint.size() != nv) return;
  // Offsets are accumulated per authored POINT (that is what BlendShape
  // `pointIndices` index), then scattered onto the welded vertices.
  const size_t np = numPoints;
  std::vector<float> delta(3 * np, 0.0f);
  bool any = false;

  const size_t n = std::min(shapeNames.size(), targets->size());
  for (size_t i = 0; i < n; ++i) {
    auto it = weights.find(shapeNames[i]);
    if (it == weights.end() || std::fabs(it->second) < 1e-8f) continue;
    const float w = it->second;
    tnext::UsdPrim bs = stage.GetPrimAtPath((*targets)[i]);
    if (!bs.IsValid()) continue;
    const std::vector<float> primary = ReadFloats(bs, "offsets", time);
    const std::vector<int32_t> pointIndices = ReadInts(bs, "pointIndices", time);
    const size_t m = primary.size() / 3;
    if (m == 0) continue;

    // Sample table = [in-betweens (ascending)..., primary]; FindMorphBracket
    // gives the two table entries (rest index 0 contributes nothing) + lerp t.
    const std::vector<std::pair<float, std::vector<float>>> ib =
        ReadInbetweens(bs, time);
    std::vector<float> ibW;
    ibW.reserve(ib.size());
    for (const auto& s : ib) ibW.push_back(s.first);
    const MorphBracket br = FindMorphBracket(ibW, w);
    // Table index k in [1..N+1] -> array entry k-1 (in-between k-1, or primary).
    auto sampleAt = [&](int tableIdx) -> const std::vector<float>* {
      const int a = tableIdx - 1;
      if (a < 0) return nullptr;
      return (a < int(ib.size())) ? &ib[size_t(a)].second : &primary;
    };
    const std::vector<float>* sLo = br.lo >= 1 ? sampleAt(br.lo) : nullptr;
    const std::vector<float>* sHi = br.hi >= 1 ? sampleAt(br.hi) : nullptr;
    const float wLo = 1.0f - br.t, wHi = br.t;

    for (size_t k = 0; k < m; ++k) {
      // Absent pointIndices => offsets are per-point for all points (USD rule).
      const int64_t pidx =
          pointIndices.empty()
              ? int64_t(k)
              : (k < pointIndices.size() ? int64_t(pointIndices[k]) : -1);
      if (pidx < 0 || size_t(pidx) >= np) continue;
      float d[3] = {0, 0, 0};
      if (sLo && 3 * k + 2 < sLo->size())
        for (int c = 0; c < 3; ++c) d[c] += wLo * (*sLo)[3 * k + c];
      if (sHi && 3 * k + 2 < sHi->size())
        for (int c = 0; c < 3; ++c) d[c] += wHi * (*sHi)[3 * k + c];
      delta[3 * pidx + 0] += d[0];
      delta[3 * pidx + 1] += d[1];
      delta[3 * pidx + 2] += d[2];
      any = true;
    }
  }
  if (!any) return;
  for (size_t i = 0; i < nv; ++i) {
    const size_t p = vertexToPoint.empty() ? i : size_t(vertexToPoint[i]);
    if (p >= np) continue;
    dm->vertices[i].px += delta[3 * p + 0];
    dm->vertices[i].py += delta[3 * p + 1];
    dm->vertices[i].pz += delta[3 * p + 2];
  }

  // Recompute smooth vertex normals from the baked positions so authored-normal
  // (smooth-shaded) meshes don't keep stale rest normals. Geometric-shaded meshes
  // re-derive normals in the shader and ignore the attribute, so skip them.
  RecomputeSmoothNormalsNext(dm);
}

// Recompute area-weighted smooth vertex normals from positions (mirrors the
// BakeBlendShapes tail). Geometric-shaded meshes re-derive normals in the
// shader, so skip them.
void RecomputeSmoothNormalsNext(DrawMeshCPU* dm) {
  if (dm->geometricNormal) return;
  const size_t np = dm->vertices.size();
  for (size_t i = 0; i < np; ++i)
    dm->vertices[i].nx = dm->vertices[i].ny = dm->vertices[i].nz = 0.0f;
  for (size_t t = 0; t + 2 < dm->indices.size(); t += 3) {
    const uint32_t a = dm->indices[t], b = dm->indices[t + 1], c = dm->indices[t + 2];
    if (a >= np || b >= np || c >= np) continue;
    const DrawVertex& va = dm->vertices[a];
    const DrawVertex& vb = dm->vertices[b];
    const DrawVertex& vc = dm->vertices[c];
    const float e1x = vb.px - va.px, e1y = vb.py - va.py, e1z = vb.pz - va.pz;
    const float e2x = vc.px - va.px, e2y = vc.py - va.py, e2z = vc.pz - va.pz;
    const float fnx = e1y * e2z - e1z * e2y, fny = e1z * e2x - e1x * e2z,
                fnz = e1x * e2y - e1y * e2x;
    for (uint32_t v : {a, b, c}) {
      dm->vertices[v].nx += fnx; dm->vertices[v].ny += fny; dm->vertices[v].nz += fnz;
    }
  }
  for (size_t i = 0; i < np; ++i) {
    DrawVertex& v = dm->vertices[i];
    const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
    if (len > 1e-12f) { const float inv = 1.0f / len; v.nx *= inv; v.ny *= inv; v.nz *= inv; }
  }
}

tnext::UsdPrim FindSkeletonInSubtree(const tnext::UsdPrim& root) {
  if (lightusd::next::IsSkeleton(root)) return root;
  for (const tnext::UsdPrim& c : root.GetChildren()) {
    tnext::UsdPrim r = FindSkeletonInSubtree(c);
    if (r.IsValid()) return r;
  }
  return tnext::UsdPrim();
}

tnext::UsdPrim FindAnimationInSubtree(const tnext::UsdPrim& root) {
  if (lightusd::next::IsSkelAnimation(root)) return root;
  for (const tnext::UsdPrim& c : root.GetChildren()) {
    tnext::UsdPrim r = FindAnimationInSubtree(c);
    if (r.IsValid()) return r;
  }
  return tnext::UsdPrim();
}

// Find the Skeleton bound to a skinned mesh: explicit skel:skeleton rel, else
// the Skeleton under the enclosing SkelRoot ancestor.
tnext::UsdPrim FindBoundSkeletonNext(const tnext::Stage& stage,
                                     const tnext::UsdPrim& meshPrim) {
  if (const std::vector<tnext::Path>* rel = meshPrim.GetRelationship("skel:skeleton")) {
    if (!rel->empty()) {
      tnext::UsdPrim s = stage.GetPrimAtPath((*rel)[0]);
      if (s.IsValid() && lightusd::next::IsSkeleton(s)) return s;
    }
  }
  tnext::UsdPrim p = meshPrim.GetParent();
  while (p.IsValid()) {
    if (p.GetTypeName() == "SkelRoot") {
      if (const std::vector<tnext::Path>* rel = p.GetRelationship("skel:skeleton")) {
        if (!rel->empty()) {
          tnext::UsdPrim s = stage.GetPrimAtPath((*rel)[0]);
          if (s.IsValid() && lightusd::next::IsSkeleton(s)) return s;
        }
      }
      tnext::UsdPrim found = FindSkeletonInSubtree(p);
      if (found.IsValid()) return found;
    }
    p = p.GetParent();
  }
  return tnext::UsdPrim();
}

// Find the SkelAnimation driving a skeleton: its animationSource, else a
// skel:animationSource rel on the mesh's ancestors.
tnext::UsdPrim FindSkelAnimationNext(const tnext::Stage& stage,
                                     const tnext::UsdPrim& meshPrim,
                                     const lightusd::next::SkeletonData& skel) {
  if (skel.hasAnimationSource && !skel.animationSource.empty()) {
    tnext::UsdPrim a = stage.GetPrimAtPath(skel.animationSource);
    if (a.IsValid() && lightusd::next::IsSkelAnimation(a)) return a;
  }
  tnext::UsdPrim p = meshPrim;
  while (p.IsValid()) {
    if (const std::vector<tnext::Path>* rel = p.GetRelationship("skel:animationSource")) {
      if (!rel->empty()) {
        tnext::UsdPrim a = stage.GetPrimAtPath((*rel)[0]);
        if (a.IsValid() && lightusd::next::IsSkelAnimation(a)) return a;
      }
    }
    p = p.GetParent();
  }
  // Some USDA/USDC relationship paths are not exposed by the lightweight
  // next-stage relationship view after composition, even though the bound
  // animation is present under the enclosing SkelRoot. The UsdSkel binding
  // model permits that placement, so retain a deterministic subtree fallback.
  p = meshPrim.GetParent();
  while (p.IsValid()) {
    if (p.GetTypeName() == "SkelRoot") {
      tnext::UsdPrim found = FindAnimationInSubtree(p);
      if (found.IsValid()) return found;
    }
    p = p.GetParent();
  }
  return tnext::UsdPrim();
}

// Joint-local transform from TRS (row-vector; matches skinning.cc MakeLocal).
matrix4d SkinMakeLocal(const float t[3], const ::lightusd::value::quatf& r,
                       const float s[3]) {
  matrix4d m = ::lightusd::to_matrix(r);
  m.m[0][0] *= s[0]; m.m[0][1] *= s[0]; m.m[0][2] *= s[0];
  m.m[1][0] *= s[1]; m.m[1][1] *= s[1]; m.m[1][2] *= s[1];
  m.m[2][0] *= s[2]; m.m[2][1] *= s[2]; m.m[2][2] *= s[2];
  m.m[3][0] = t[0]; m.m[3][1] = t[1]; m.m[3][2] = t[2];
  return m;
}

// A mesh's POSE-INDEPENDENT skin binding: the bound skeleton/animation, the
// geomBindTransform, and the per-VERTEX influences. Resolved once at load and
// consumed either by the CPU bake (BakeSkinning) or by the GPU path, which keeps
// the influences as vertex attributes and re-poses the skeleton every frame
// (SetupGpuSkinNext / BuildNextSkinningFrame).
struct NextSkinBinding {
  std::string skelPath;
  std::string animPath;  // "" = no animation (rest pose)
  size_t numJoints = 0;
  int numInfl = 0;
  matrix4d geomBind = matrix4d::identity();
  std::vector<int> vidx;    // nv * numInfl, in SKELETON joint order
  std::vector<float> vwgt;  // nv * numInfl
};

constexpr int kNextInfluenceTexWidth = 1024;

// false = not skinned, or the skin data is missing/inconsistent (callers then
// leave the mesh in its rest pose).
bool ResolveNextSkinBinding(const tnext::Stage& stage,
                            const tnext::UsdPrim& meshPrim, double time,
                            size_t nv,
                            const std::vector<uint32_t>& vertexToPoint,
                            size_t numPoints, NextSkinBinding* out) {
  if (!out || nv == 0) return false;
  std::vector<int32_t> ji = ReadInts(meshPrim, "primvars:skel:jointIndices", time);
  std::vector<float> jw = ReadFloats(meshPrim, "primvars:skel:jointWeights", time);
  if (ji.empty() || ji.size() != jw.size()) return false;
  // skel:jointIndices/Weights are authored per POINT; the weld may have split
  // points into several vertices, so influences are gathered through
  // `vertexToPoint` below rather than read at the vertex index.
  if (numPoints == 0 || ji.size() % numPoints != 0) return false;
  const int numInfl = static_cast<int>(ji.size() / numPoints);
  if (numInfl <= 0) return false;
  if (!vertexToPoint.empty() && vertexToPoint.size() != nv) return false;

  tnext::UsdPrim skelPrim = FindBoundSkeletonNext(stage, meshPrim);
  if (!skelPrim.IsValid()) return false;
  lightusd::next::SkeletonData skel;
  if (!lightusd::next::GetSkeletonData(stage, skelPrim, &skel)) return false;
  const size_t nj = skel.joints.size();
  if (nj == 0) return false;

  // Remap mesh-authored joint order into skeleton order (when authored).
  std::vector<int> idx(ji.begin(), ji.end());
  // UsdSkel stores the mesh-local joint order in `skel:joints` (the
  // `primvars:` namespace is used for jointIndices/jointWeights).  Some
  // exporters have emitted the names under the latter spelling, so retain a
  // fallback for those files, but prefer the standard property.  Without
  // this remap, a mesh whose joint order differs from Skeleton.joints assigns
  // unrelated bones to parts such as the trunk and feet.
  std::vector<std::string> meshJoints = ReadTokens(meshPrim, "skel:joints", time);
  if (meshJoints.empty()) {
    meshJoints = ReadTokens(meshPrim, "primvars:skel:joints", time);
  }
  if (!meshJoints.empty()) {
    std::unordered_map<std::string, int> skelIdx;
    for (size_t j = 0; j < nj; ++j) skelIdx[skel.joints[j]] = static_cast<int>(j);
    std::vector<int> remap(meshJoints.size(), -1);
    for (size_t i = 0; i < meshJoints.size(); ++i) {
      auto it = skelIdx.find(meshJoints[i]);
      if (it != skelIdx.end()) remap[i] = it->second;
    }
    for (int& v : idx) {
      v = (v >= 0 && v < static_cast<int>(remap.size())) ? remap[v] : -1;
      if (v < 0) return false;  // unresolved joint -> leave rest pose (safe)
    }
  }
  for (int& v : idx)
    if (v < 0 || v >= static_cast<int>(nj)) v = 0;  // clamp stray indices

  // geomBindTransform (single matrix4d; identity when absent).
  matrix4d geomBind = matrix4d::identity();
  if (const tnext::Value* gv =
          meshPrim.GetPropertyValue("primvars:skel:geomBindTransform")) {
    tnext::Value tmp;
    const tnext::Value* v = gv;
    if (gv->is_lazy()) { tmp = gv->materialized_copy(); v = &tmp; }
    if (const double* d = v->as_matrix4d()) geomBind = Mat4dFromArray(d);
  }

  // Gather the per-point influences onto the (possibly welded) vertex array, so
  // every variant of a split point is skinned by that point's weights.
  std::vector<int> vidx(nv * size_t(numInfl));
  std::vector<float> vwgt(nv * size_t(numInfl));
  for (size_t i = 0; i < nv; ++i) {
    const size_t p = vertexToPoint.empty() ? i : size_t(vertexToPoint[i]);
    if (p >= numPoints) return false;
    for (int k = 0; k < numInfl; ++k) {
      vidx[i * size_t(numInfl) + size_t(k)] = idx[p * size_t(numInfl) + size_t(k)];
      vwgt[i * size_t(numInfl) + size_t(k)] = jw[p * size_t(numInfl) + size_t(k)];
    }
  }

  tnext::UsdPrim animPrim = FindSkelAnimationNext(stage, meshPrim, skel);
  out->skelPath = skelPrim.GetPath().str();
  out->animPath = animPrim.IsValid() ? animPrim.GetPath().str() : std::string();
  out->numJoints = nj;
  out->numInfl = numInfl;
  out->geomBind = geomBind;
  out->vidx = std::move(vidx);
  out->vwgt = std::move(vwgt);
  return true;
}

// Pose a skeleton at `time`: skinMat[j] carries a bind-space point (i.e. one the
// geomBindTransform has already been applied to) into the posed skeleton space.
// Row-vector convention, matching tydra::SkinPointsLBS.
bool PoseNextSkeleton(const tnext::Stage& stage, const std::string& skelPath,
                      const std::string& animPath, double time,
                      std::vector<matrix4d>* skinMat) {
  if (!skinMat) return false;
  tnext::UsdPrim skelPrim = stage.GetPrimAtPath(skelPath);
  if (!skelPrim.IsValid()) return false;
  lightusd::next::SkeletonData skel;
  if (!lightusd::next::GetSkeletonData(stage, skelPrim, &skel)) return false;
  const size_t nj = skel.joints.size();
  if (nj == 0) return false;

  std::vector<int> topo;
  std::string terr;
  if (!lightusd::next::BuildSkelTopology(skel.joints, topo, &terr) ||
      topo.size() != nj) {
    return false;
  }

  // Dense point-joint rigs (often thousands of joints, one per sampled point)
  // use animated translations as deformation samples. Their authored rotation
  // and scale channels are exporter bookkeeping, not bone rotations; applying
  // them to the rest local transforms produces the characteristic exploded
  // mesh seen in AnimFinal_LowRes. Keep this in sync with the legacy/Tydra
  // IsPointJointSkeleton heuristic.
  bool translationOnly = false;
  if (nj >= 512) {
    size_t roots = 0;
    size_t rootChildren = 0;
    for (int parent : topo) {
      if (parent < 0) {
        ++roots;
      } else if (parent == 0) {
        ++rootChildren;
      }
    }
    translationOnly = nj >= 1024 ||
                      (nj > 0 && rootChildren + 1 == nj) ||
                      (roots == 1 && rootChildren + 1 == nj);
  }

  const bool haveRest = skel.restTransforms.size() == nj * 16;
  const bool haveBind = skel.bindTransforms.size() == nj * 16;
  std::vector<matrix4d> restLocal(nj), bindWorld(nj);
  for (size_t j = 0; j < nj; ++j) {
    restLocal[j] = haveRest ? Mat4dFromArray(&skel.restTransforms[j * 16])
                            : matrix4d::identity();
    bindWorld[j] = haveBind ? Mat4dFromArray(&skel.bindTransforms[j * 16])
                            : matrix4d::identity();
  }

  // Animated local transforms: default each joint's TRS from its rest local (so
  // a partial animation keeps rest offsets), override with the SkelAnimation.
  std::vector<matrix4d> local = restLocal;
  tnext::UsdPrim animPrim =
      animPath.empty() ? tnext::UsdPrim() : stage.GetPrimAtPath(animPath);
  if (animPrim.IsValid()) {
    lightusd::next::SkelAnimationData anim;
    if (lightusd::next::GetSkelAnimationData(stage, animPrim, &anim, time) &&
        !anim.joints.empty()) {

      std::unordered_map<std::string, int> skelIdx;
      for (size_t j = 0; j < nj; ++j) skelIdx[skel.joints[j]] = static_cast<int>(j);
      for (size_t a = 0; a < anim.joints.size(); ++a) {
        auto it = skelIdx.find(anim.joints[a]);
        if (it == skelIdx.end()) continue;
        const int j = it->second;
        float t3[3] = {0, 0, 0}, s3[3] = {1, 1, 1};
        ::lightusd::value::quatf q;
        q.imag[0] = q.imag[1] = q.imag[2] = 0.0f; q.real = 1.0f;
        ::lightusd::value::double3 dt, ds;
        ::lightusd::value::quatd dq;
        if (::lightusd::decompose(restLocal[j], &dt, &dq, &ds)) {
          t3[0] = float(dt[0]); t3[1] = float(dt[1]); t3[2] = float(dt[2]);
          s3[0] = float(ds[0]); s3[1] = float(ds[1]); s3[2] = float(ds[2]);
          q.imag[0] = float(dq.imag[0]); q.imag[1] = float(dq.imag[1]);
          q.imag[2] = float(dq.imag[2]); q.real = float(dq.real);
        }
        // UsdSkelAnimation translations are joint-local components.
        if (anim.hasTranslations &&
            (a + 1) * 3 <= anim.translations.size()) {
          t3[0] = anim.translations[a * 3 + 0];
          t3[1] = anim.translations[a * 3 + 1];
          t3[2] = anim.translations[a * 3 + 2];
        }
        if (!translationOnly && anim.hasRotations &&
            (a + 1) * 4 <= anim.rotations.size()) {
          // SkelAnimationData exposes quaternions in the next API's canonical
          // real-first order: (w, x, y, z).
          q.real = anim.rotations[a * 4 + 0];
          q.imag[0] = anim.rotations[a * 4 + 1];
          q.imag[1] = anim.rotations[a * 4 + 2];
          q.imag[2] = anim.rotations[a * 4 + 3];
          const float qlen = std::sqrt(
              q.real * q.real + q.imag[0] * q.imag[0] +
              q.imag[1] * q.imag[1] + q.imag[2] * q.imag[2]);
          if (qlen < 1.0e-12f) {
            q.imag[0] = q.imag[1] = q.imag[2] = 0.0f;
            q.real = 1.0f;
          } else {
            q.imag[0] /= qlen;
            q.imag[1] /= qlen;
            q.imag[2] /= qlen;
            q.real /= qlen;
          }
        }
        if (!translationOnly && anim.hasScales &&
            (a + 1) * 3 <= anim.scales.size()) {
          s3[0] = anim.scales[a * 3 + 0];
          s3[1] = anim.scales[a * 3 + 1];
          s3[2] = anim.scales[a * 3 + 2];
        }
        local[j] = SkinMakeLocal(t3, q, s3);
      }
    }
  }

  std::vector<matrix4d> world;
  if (!lightusd::tydra::ConcatJointTransforms(topo, local, &world) ||
      world.size() != nj) {
    return false;
  }
  // Synthesize the bind pose from the rest world transform when bind is absent.
  if (!haveBind) {
    std::vector<matrix4d> restWorld;
    if (lightusd::tydra::ConcatJointTransforms(topo, restLocal, &restWorld) &&
        restWorld.size() == nj) {
      bindWorld = std::move(restWorld);
    }
  } else if (translationOnly) {
    // Dense point-joint rigs use rest-world transforms as their effective bind
    // pose. The authored bindTransforms in these exports are not the bind
    // space used by the point samples; matching the Tydra path here prevents
    // the inverse-bind step from magnifying the mesh.
    std::vector<matrix4d> restWorld;
    if (lightusd::tydra::ConcatJointTransforms(topo, restLocal, &restWorld) &&
        restWorld.size() == nj) {
      bindWorld = std::move(restWorld);
    }
  }
  skinMat->assign(nj, matrix4d::identity());
  for (size_t j = 0; j < nj; ++j)
    (*skinMat)[j] = ::lightusd::inverse(bindWorld[j]) * world[j];
  return true;
}

// Load-time skeletal skinning bake: pose the bound skeleton at `time` and LBS-
// deform dm->vertices (rest, point-indexed) in place, then recompute normals.
// The CPU-skinning path (and the CPU ray tracers, which read this geometry).
// No-op -- leaves the rest pose -- on any missing/mismatched skin/skeleton data.
// Returns true when the mesh was actually skinned (so the caller knows its
// vertices are ONE pose of an animated rig, not static geometry).
bool BakeSkinning(const tnext::Stage& stage, const tnext::UsdPrim& meshPrim,
                  double time, DrawMeshCPU* dm,
                  const std::vector<uint32_t>& vertexToPoint,
                  size_t numPoints) {
  if (!dm || dm->vertices.empty()) return false;
  const size_t nv = dm->vertices.size();
  NextSkinBinding bind;
  if (!ResolveNextSkinBinding(stage, meshPrim, time, nv, vertexToPoint,
                              numPoints, &bind)) {
    return false;
  }
  std::vector<matrix4d> skinMat;
  if (!PoseNextSkeleton(stage, bind.skelPath, bind.animPath, time, &skinMat)) {
    return false;
  }

  // Bake back into the mesh's current local space. UsdSkel skinning produces
  // skeleton-space points; the Skeleton prim and mesh may have different world
  // transforms (Elephant's vibrator is a minimal example).
  double meshWorldData[16], skeletonWorldData[16];
  const tnext::UsdPrim skelPrim = stage.GetPrimAtPath(bind.skelPath);
  if (!tydn::ComputeWorldTransform(stage, meshPrim, meshWorldData, time) ||
      !skelPrim.IsValid() ||
      !tydn::ComputeWorldTransform(stage, skelPrim, skeletonWorldData, time)) {
    return false;
  }
  const matrix4d meshWorld = Mat4dFromArray(meshWorldData);
  const matrix4d skeletonWorld = Mat4dFromArray(skeletonWorldData);
  const matrix4d skeletonToMesh =
      skeletonWorld * ::lightusd::inverse(meshWorld) * bind.geomBind;
  std::vector<matrix4d> meshLocalSkin(skinMat.size());
  for (size_t j = 0; j < skinMat.size(); ++j)
    meshLocalSkin[j] = skinMat[j] * skeletonToMesh;

  std::vector<::lightusd::value::point3f> rest(nv), skinned;
  for (size_t i = 0; i < nv; ++i) {
    rest[i].x = dm->vertices[i].px;
    rest[i].y = dm->vertices[i].py;
    rest[i].z = dm->vertices[i].pz;
  }
  std::string lerr;
  if (!lightusd::tydra::SkinPointsLBS(rest, bind.geomBind, meshLocalSkin, bind.vidx,
                                      bind.vwgt, bind.numInfl, &skinned, &lerr) ||
      skinned.size() != nv) {
    return false;
  }
  // Skin the NORMALS with the same blended matrix the GPU vertex shader uses,
  // rather than regenerating a smooth normal field from the posed positions:
  // the two disagree wherever the pose bends the surface, and the CPU and GPU
  // skinning paths must render the same image (lusdview-skinning-screenshot-diff).
  const matrix4d invGeomBind = ::lightusd::inverse(bind.geomBind);
  std::vector<matrix4d> composed(meshLocalSkin.size());
  for (size_t j = 0; j < meshLocalSkin.size(); ++j)
    composed[j] = bind.geomBind * meshLocalSkin[j] * invGeomBind;

  for (size_t i = 0; i < nv; ++i) {
    dm->vertices[i].px = skinned[i].x;
    dm->vertices[i].py = skinned[i].y;
    dm->vertices[i].pz = skinned[i].z;

    const float n[3] = {dm->vertices[i].nx, dm->vertices[i].ny,
                        dm->vertices[i].nz};
    double acc[3] = {0.0, 0.0, 0.0};
    double wsum = 0.0;
    for (int k = 0; k < bind.numInfl; ++k) {
      const float w = bind.vwgt[i * size_t(bind.numInfl) + size_t(k)];
      if (!(w > 0.0f)) continue;
      const int j = bind.vidx[i * size_t(bind.numInfl) + size_t(k)];
      if (j < 0 || j >= static_cast<int>(composed.size())) continue;
      const matrix4d& m = composed[size_t(j)];
      for (int c = 0; c < 3; ++c) {  // row-vector, rotation part only
        acc[c] += double(w) * (double(n[0]) * m.m[0][c] +
                               double(n[1]) * m.m[1][c] +
                               double(n[2]) * m.m[2][c]);
      }
      wsum += double(w);
    }
    if (wsum <= 0.0) continue;
    const double len =
        std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    if (len <= 1e-12) continue;
    dm->vertices[i].nx = static_cast<float>(acc[0] / len);
    dm->vertices[i].ny = static_cast<float>(acc[1] / len);
    dm->vertices[i].nz = static_cast<float>(acc[2] / len);
  }
  return true;
}

// GPU skinning alternative to BakeSkinning: keep the mesh in its REST pose and
// emit per-vertex joint attributes + a bone-matrix block, so the vertex shader
// poses it every frame. `worldM` is the mesh world transform the caller is about
// to bake into the vertices (row-vector, row-major); it is folded into the bone
// matrices instead of the attributes -- see BuildNextSkinningFrame.
//
// The GPU attribute path carries the four strongest influences for the fast
// common case and also retains the complete variable-length influence list in
// DrawMeshCPU's influence texture stream. The latter is required for rigs such
// as AnimFinal_LowRes, where a vertex may have dozens of meaningful weights.
// Returns false when the mesh is not skinned (caller leaves it alone).
bool SetupGpuSkinNext(const tnext::Stage& stage, const tnext::UsdPrim& meshPrim,
                      double time, DrawMeshCPU* dm,
                      const std::vector<uint32_t>& vertexToPoint,
                      size_t numPoints, const double worldM[16],
                      const double renderWorldM[16],
                      const double outputSpaceWorldM[16],
                      DrawScene* draw) {
  if (!dm || dm->vertices.empty() || !draw) return false;
  const size_t nv = dm->vertices.size();
  NextSkinBinding bind;
  if (!ResolveNextSkinBinding(stage, meshPrim, time, nv, vertexToPoint,
                              numPoints, &bind)) {
    return false;
  }
  // Keep conventional skeletons on the same GPU path as dense point-joint
  // rigs. PoseNextSkeleton selects the appropriate interpretation of the
  // animation (local TRS for ordinary rigs, translation samples for dense
  // point-joint exports); rejecting small rigs here made the next loader bake
  // them on the CPU and, more importantly, left boneMatrixCount at zero so
  // the viewer incorrectly reported that the scene was unskinned.
  const int nj = static_cast<int>(bind.numJoints);
  if (nj <= 0) return false;
  // Guard the bone-texture row space (int rows, absolute indices).
  if (draw->boneMatrixCount > std::numeric_limits<int>::max() - nj) return false;

  const int base = draw->boneMatrixCount;
  const int ni = bind.numInfl;
  dm->jointIdx.assign(nv * 4, 0u);
  dm->jointWt.assign(nv * 4, 0.0f);
  dm->influenceOffsetCount.assign(nv * 2, 0u);
  dm->influenceTexels.clear();
  dm->maxInfluencesPerVertex = 0;
  for (size_t v = 0; v < nv; ++v) {
    // Top-4 influences by weight.
    std::array<std::pair<float, int>, 4> top{};  // (weight, joint)
    for (auto& t : top) t = {0.0f, 0};
    for (int k = 0; k < ni; ++k) {
      const float w = bind.vwgt[v * size_t(ni) + size_t(k)];
      if (!std::isfinite(w) || !(w > 0.0f)) continue;
      const int j = bind.vidx[v * size_t(ni) + size_t(k)];
      // Insertion sort into the 4-slot top list.
      for (int s = 0; s < 4; ++s) {
        if (w > top[size_t(s)].first) {
          for (int t = 3; t > s; --t) top[size_t(t)] = top[size_t(t - 1)];
          top[size_t(s)] = {w, j};
          break;
        }
      }
    }
    float sum = 0.0f;
    for (const auto& t : top) sum += t.first;
    for (int s = 0; s < 4; ++s) {
      dm->jointIdx[v * 4 + size_t(s)] =
          static_cast<uint32_t>(base + top[size_t(s)].second);
      dm->jointWt[v * 4 + size_t(s)] =
          sum > 0.0f ? top[size_t(s)].first / sum : 0.0f;
    }

    // Keep every authored influence, remapped to this mesh's absolute bone
    // rows. Normalize once here so raster, CPU fallback, and RT all consume
    // exactly the same weights. The first four attributes above remain a
    // compact fallback for backends/shaders that cannot use the stream.
    const uint32_t offset =
        static_cast<uint32_t>(dm->influenceTexels.size() / 4);
    double fullSum = 0.0;
    for (int k = 0; k < ni; ++k) {
      const float w = bind.vwgt[v * size_t(ni) + size_t(k)];
      const int j = bind.vidx[v * size_t(ni) + size_t(k)];
      if (!(w > 0.0f) || !std::isfinite(w) || j < 0) continue;
      dm->influenceTexels.push_back(static_cast<float>(base + j));
      dm->influenceTexels.push_back(w);
      dm->influenceTexels.push_back(0.0f);
      dm->influenceTexels.push_back(0.0f);
      fullSum += static_cast<double>(w);
    }
    uint32_t count = static_cast<uint32_t>(dm->influenceTexels.size() / 4) - offset;
    if (fullSum > 0.0) {
      const float inv = static_cast<float>(1.0 / fullSum);
      for (uint32_t k = 0; k < count; ++k)
        dm->influenceTexels[(static_cast<size_t>(offset + k) * 4) + 1] *= inv;
    } else {
      count = 0;
    }
    dm->influenceOffsetCount[v * 2 + 0] = offset;
    dm->influenceOffsetCount[v * 2 + 1] = count;
    dm->maxInfluencesPerVertex =
        std::max(dm->maxInfluencesPerVertex, static_cast<int>(count));
  }
  if (!dm->influenceTexels.empty()) {
    const size_t texels = dm->influenceTexels.size() / 4;
    dm->influenceTexWidth = kNextInfluenceTexWidth;
    dm->influenceTexHeight = static_cast<int>(
        (texels + static_cast<size_t>(kNextInfluenceTexWidth) - 1) /
        static_cast<size_t>(kNextInfluenceTexWidth));
    dm->influenceTexels.resize(
        static_cast<size_t>(dm->influenceTexWidth) *
            static_cast<size_t>(dm->influenceTexHeight) * 4,
        0.0f);
  }

  DrawScene::NextSkelBinding nb;
  nb.skelPath = bind.skelPath;
  nb.animPath = bind.animPath;
  nb.meshPath = meshPrim.GetPath().str();
  nb.numJoints = nj;
  nb.matrixBase = base;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) nb.geomBind[r * 4 + c] = bind.geomBind.m[r][c];
  for (int k = 0; k < 16; ++k) nb.world[k] = worldM[k];
  for (int k = 0; k < 16; ++k) nb.renderWorld[k] = renderWorldM[k];
  const tnext::UsdPrim skelPrim = stage.GetPrimAtPath(bind.skelPath);
  double skeletonStageWorld[16];
  if (!skelPrim.IsValid() || !tydn::ComputeWorldTransform(
          stage, skelPrim, skeletonStageWorld, time)) {
    const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                              0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(nb.skeletonWorld, ident, sizeof(ident));
  } else {
    const matrix4d skeletonStage = Mat4dFromArray(skeletonStageWorld);
    const matrix4d outputStage = Mat4dFromArray(outputSpaceWorldM);
    const matrix4d skeletonOutput =
        skeletonStage * ::lightusd::inverse(outputStage);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        nb.skeletonWorld[r * 4 + c] = skeletonOutput.m[r][c];
  }
  draw->nextSkels.push_back(std::move(nb));
  draw->boneMatrixCount = base + nj;
  return true;
}

// Build GPU-morph CSR channels for a prototype mesh, so the instanced raster
// shader morphs per-frame from a tiny per-channel coefficient buffer instead of
// the morph being baked into geometry. Mirrors BuildMorphChannels in
// mesh_build.cc, reading directly from the next stage. A channel = one delta
// stream (an in-between sample or the primary); per target the channels are
// [in-betweens ascending..., primary] with usdWeights [ibWeights..., 1.0],
// matching EvalMorphChannelCoeffs' bracket eval. No-op (no channels) when the
// mesh has no resolvable blendshape targets.
//
// BlendShape `pointIndices` index authored POINTS, and FillFlatGeometry's weld
// can back one point with several vertices (UV seams / hard edges), so each
// delta entry fans out to every vertex of its point.
void BuildMorphChannelsNext(const tnext::Stage& stage,
                            const tnext::UsdPrim& meshPrim, double time,
                            DrawMeshCPU* dm,
                            const std::vector<uint32_t>& vertexToPoint,
                            size_t numPoints) {
  const std::vector<std::string> shapeNames =
      ReadTokens(meshPrim, "skel:blendShapes", time);
  const std::vector<tnext::Path>* targets =
      meshPrim.GetRelationship("skel:blendShapeTargets");
  if (shapeNames.empty() || !targets || targets->empty()) return;

  const size_t nv = dm->vertices.size();
  const size_t np = numPoints;
  if (nv == 0 || np == 0) return;
  if (!vertexToPoint.empty() && vertexToPoint.size() != nv) return;

  // point -> its welded vertices, as a CSR (counting sort over vertexToPoint).
  std::vector<uint32_t> pvOffset(np + 1, 0u);
  std::vector<uint32_t> pvVerts(nv, 0u);
  {
    for (size_t i = 0; i < nv; ++i) {
      const size_t p = vertexToPoint.empty() ? i : size_t(vertexToPoint[i]);
      if (p < np) pvOffset[p + 1]++;
    }
    for (size_t p = 0; p < np; ++p) pvOffset[p + 1] += pvOffset[p];
    std::vector<uint32_t> cur(pvOffset.begin(), pvOffset.end() - 1);
    for (size_t i = 0; i < nv; ++i) {
      const size_t p = vertexToPoint.empty() ? i : size_t(vertexToPoint[i]);
      if (p < np) pvVerts[cur[p]++] = static_cast<uint32_t>(i);
    }
  }

  // One delta stream per channel: its offsets (3/entry) + the point indices the
  // entries map to (empty => identity 0..M-1). Streams own their data so the
  // sparse target reads can be freed before the CSR scatter.
  struct Chan {
    int id;
    std::vector<float> offsets;       // 3 * M
    const std::vector<int32_t>* pidx; // M (points into `pidxStore`)
  };
  std::vector<Chan> chans;
  std::vector<std::vector<int32_t>> pidxStore;  // stable addresses for Chan::pidx
  pidxStore.reserve(targets->size());
  int nextChannel = 0;
  dm->morphTargetChannels.clear();

  const size_t n = std::min(shapeNames.size(), targets->size());
  for (size_t i = 0; i < n; ++i) {
    tnext::UsdPrim bs = stage.GetPrimAtPath((*targets)[i]);
    if (!bs.IsValid()) continue;
    std::vector<float> primary = ReadFloats(bs, "offsets", time);
    if (primary.size() < 3) continue;
    pidxStore.push_back(ReadInts(bs, "pointIndices", time));
    const std::vector<int32_t>* pidx = &pidxStore.back();
    std::vector<std::pair<float, std::vector<float>>> ib =
        ReadInbetweens(bs, time);

    MorphTargetChannelsCPU tc;
    tc.name = shapeNames[i];
    for (auto& s : ib) {  // in-betweens ascending
      const int ch = nextChannel++;
      tc.usdWeights.push_back(s.first);
      tc.channelIds.push_back(ch);
      chans.push_back({ch, std::move(s.second), pidx});
    }
    const int chPrimary = nextChannel++;  // primary == weight 1.0
    tc.usdWeights.push_back(1.0f);
    tc.channelIds.push_back(chPrimary);
    chans.push_back({chPrimary, std::move(primary), pidx});
    dm->morphTargetChannels.push_back(std::move(tc));
  }
  if (chans.empty()) return;
  dm->morphChannelCount = nextChannel;

  // The entry's target POINT (or -1 to skip); `fanout` visits every welded
  // vertex of that point.
  auto ptOf = [np](const Chan& c, size_t e) -> int64_t {
    const int64_t p = c.pidx->empty() ? int64_t(e) : int64_t((*c.pidx)[e]);
    return (p >= 0 && size_t(p) < np && e * 3 + 2 < c.offsets.size()) ? p : -1;
  };
  auto fanout = [&](int64_t p, const std::function<void(uint32_t)>& fn) {
    for (uint32_t k = pvOffset[size_t(p)]; k < pvOffset[size_t(p) + 1]; ++k) {
      fn(pvVerts[k]);
    }
  };

  // Pass 1: count entries per vertex. M = offsets/3 (== pidx size when present).
  std::vector<uint32_t> count(nv, 0u);
  for (const Chan& c : chans) {
    const size_t m = c.offsets.size() / 3;
    for (size_t e = 0; e < m; ++e) {
      const int64_t p = ptOf(c, e);
      if (p < 0) continue;
      fanout(p, [&](uint32_t v) { count[v]++; });
    }
  }
  // Prefix-sum into morphOffsetCount (offset,count per vertex).
  dm->morphOffsetCount.assign(nv * 2, 0u);
  uint64_t total = 0;
  for (size_t v = 0; v < nv; ++v) {
    dm->morphOffsetCount[v * 2 + 0] = static_cast<uint32_t>(total);
    dm->morphOffsetCount[v * 2 + 1] = count[v];
    total += count[v];
  }
  // Pass 2: scatter [channelId, dx, dy, dz] halfs + the uint16 channelId side
  // buffer (the shader's active-channel skip pre-check).
  auto h = [](float f) { return lightusd::value::float_to_half_full(f).value; };
  dm->morphDeltaHalf.assign(total * 4, 0);
  dm->morphChannelId.assign(total, 0);
  std::vector<uint32_t> cursor(nv, 0u);
  for (const Chan& c : chans) {
    const uint16_t chHalf = h(static_cast<float>(c.id));
    const uint16_t chId = static_cast<uint16_t>(c.id);
    const size_t m = c.offsets.size() / 3;
    for (size_t e = 0; e < m; ++e) {
      const int64_t p = ptOf(c, e);
      if (p < 0) continue;
      fanout(p, [&](uint32_t v) {
        const uint64_t slot = dm->morphOffsetCount[size_t(v) * 2 + 0] + cursor[v]++;
        uint16_t* o = &dm->morphDeltaHalf[slot * 4];
        o[0] = chHalf;
        o[1] = h(c.offsets[e * 3 + 0]);
        o[2] = h(c.offsets[e * 3 + 1]);
        o[3] = h(c.offsets[e * 3 + 2]);
        dm->morphChannelId[slot] = chId;
      });
    }
  }

  // Max per-axis morph displacement, to pad protoAabb for per-instance culling.
  // Conservative: per point, sum each axis's positive and negative deltas across
  // ALL channels (worst case = every channel at full coefficient), then take the
  // largest absolute swing. Safe superset (over-pads, never culls a visible
  // morphed instance); small overdrive (weight > 1) is not bounded.
  std::vector<float> sumPos(np * 3, 0.0f), sumNeg(np * 3, 0.0f);
  for (const Chan& c : chans) {
    const size_t m = c.offsets.size() / 3;
    for (size_t e = 0; e < m; ++e) {
      const int64_t p = ptOf(c, e);
      if (p < 0) continue;
      for (int a = 0; a < 3; ++a) {
        const float d = c.offsets[e * 3 + a];
        (d >= 0.0f ? sumPos : sumNeg)[size_t(p) * 3 + a] += d;
      }
    }
  }
  for (size_t p = 0; p < np; ++p)
    for (int a = 0; a < 3; ++a)
      dm->morphExtent[a] = std::max(
          dm->morphExtent[a],
          std::max(sumPos[p * 3 + a], -sumNeg[p * 3 + a]));
}

// Build a prototype mesh's local geometry (+ flat displayColor) from the
// converter, and its mesh-local -> proto-root-local transform `mesh_rel`. Shared
// by the PointInstancer and native-instance passes. Returns false if the mesh has
// no converter geometry.
bool BuildProtoMesh(const tnext::Stage& stage, tydn::RenderSceneConverter& conv,
                    const tnext::UsdPrim& mp, const matrix4d& inv_protoroot,
                    double time, DrawMeshCPU* dm, matrix4d* mesh_rel,
                    std::vector<uint32_t>* out_vertexToPoint,
                    size_t* out_numPoints, tydn::RenderMesh* outRenderMesh) {
  // Convert just this mesh on demand (streaming) -- avoids holding the whole
  // RenderScene in RAM.
  tydn::RenderMesh rm;
  if (!conv.ConvertMesh(stage, mp, &rm)) return false;
  if (NeedsUnrealDoubleSidedFallback(mp)) rm.double_sided = true;
  std::vector<uint32_t> vertexToPoint;
  if (!FillFlatGeometry(rm, dm, &vertexToPoint)) return false;
  BuildAuthoredControlCage(mp, time, dm);
  const size_t numPoints = rm.point_count();
  // Skinning is resolved by the CALLER (it alone knows the instance count, which
  // decides GPU-skin vs static bake), so hand the weld map back out.
  if (out_numPoints) *out_numPoints = numPoints;
  // Blendshapes on the prototype. Default: build GPU-morph channels so the
  // instanced raster shader morphs per-frame (animated weights). Opt-out
  // (LUSDVIEW_NEXT_MORPH_BAKE=1): bake the morph into geometry at load -- a
  // static, lower-overhead path (no per-frame GPU morph, no morph buffers) for
  // huge static scenes. Mutually exclusive so morph is never applied twice. Both
  // no-op for non-blendshaped meshes.
  static const bool kBakeMorph = [] {
    const char* e = std::getenv("LUSDVIEW_NEXT_MORPH_BAKE");
    return e && e[0] == '1';
  }();
  if (kBakeMorph) {
    // The baked vertices already contain the sampled morph. Keep their tight
    // bounds: morphExtent is only for rest geometry that the GPU will deform
    // later. Padding baked vertices double-counts the displacement and makes
    // CPU/GPU scene bounds, depth normalization, and the ground grid diverge.
    BakeBlendShapes(stage, mp, time, dm, vertexToPoint, numPoints);
  } else {
    BuildMorphChannelsNext(stage, mp, time, dm, vertexToPoint, numPoints);
  }
  dm->purpose = ResolveNextPurpose(stage, mp.GetPath().str());
  // Prototype displayColor is carried PER-VERTEX (FillFlatGeometry filled
  // dm->vertexColors -- uploaded to GL attrib 10 for instanced draws, shared by all
  // instances). Keep the per-instance constant neutral (white) so it doesn't tint
  // the per-vertex color; the instanced shader multiplies the two.
  if (!dm->vertexColors.empty()) {
    dm->flatColor[0] = dm->flatColor[1] = dm->flatColor[2] = 1.0f;
  }
  double mw16[16];
  tydn::ComputeWorldTransform(stage, mp, mw16, time);
  *mesh_rel = Mul4(Mat4dFromArray(mw16), inv_protoroot);
  if (out_vertexToPoint) *out_vertexToPoint = std::move(vertexToPoint);
  if (outRenderMesh) *outRenderMesh = std::move(rm);
  return true;
}

// Split a prototype subtree into its DIRECT mesh prims and its NESTED instancers
// (PointInstancer / scenegraph instanceable), WITHOUT descending into the latter.
// The prototype root itself is collected only if it is a Mesh. Mirrors the
// instancer skips in the static-batching gather + lusdrender CollectProtoMeshNesting.
void SplitProtoSubtree(const tnext::UsdPrim& root,
                       std::vector<tnext::UsdPrim>* meshes,
                       std::vector<tnext::UsdPrim>* instancers) {
  std::function<void(const tnext::UsdPrim&, bool)> rec =
      [&](const tnext::UsdPrim& p, bool isRoot) {
        if (!isRoot) {
          if (p.GetTypeName() == "PointInstancer") {
            instancers->push_back(p);
            return;
          }
          const auto* s = p.GetPrimSpec();
          if (s && !s->meta().instance_prototype().empty()) {
            instancers->push_back(p);
            return;
          }
        }
        if (p.GetTypeName() == "Mesh") meshes->push_back(p);
        for (const tnext::UsdPrim& c : p.GetChildren()) rec(c, false);
      };
  rec(root, true);
}

// Emit GPU-instanced DrawMeshCPU for a prototype subtree placed at the given world
// transforms `placements`. Direct (non-instancer) meshes become one DrawMeshCPU
// each (instanceXforms = mesh_rel * each placement). NESTED instancers are
// flattened: their per-instance transforms (relative to this prototype root) are
// composed with each outer placement and the inner prototype is emitted
// recursively, so a TLAS-less GL preview still shows nested instancing -- geometry
// stays deduped (shared VBO), only the per-instance matrix list grows. Routing the
// top-level PointInstancer/native passes through this is byte-identical when nothing
// nests (same mesh order, same per-placement loop). `placementColors` and
// `placementOpacities`, when set, carry three and one float per placement.
//
// SKINNED prototypes stay INSTANCED (an earlier design de-instanced them, one
// DrawMeshCPU per placement; the comment here outlived it, along with a
// kMaxSkinnedProtoInstances cap that no longer exists). Under `gpuSkinning` the
// prototype emits skin attributes plus ONE bone block whose rows carry geomBind but
// an IDENTITY world: the bones are prototype-local, and the instanced vertex shader
// applies each instance's o2w AFTER skinning. All placements share that block, which
// is sound because USD instancing requires identical composed contents -- so they
// necessarily share a skeleton and an animation. Without `gpuSkinning` the static
// pose at `time` is baked into the prototype's geometry instead.
//
// The corollary for anything reading these meshes back: a skinned/morphed prototype's
// vertices (rest OR posed) are prototype-LOCAL, and mean nothing until they go through
// instanceXforms. BuildNextPosedSceneBounds learned that the hard way.
void EmitInstancedProto(const tnext::Stage& stage,
                        tydn::RenderSceneConverter& conv,
                        const tnext::UsdPrim& protoRoot,
                        const std::vector<matrix4d>& placements,
                        const std::vector<float>* placementColors,
                        const std::vector<float>* placementOpacities, double time,
                        bool gpuSkinning,
                        DrawScene* draw, Bounds* bounds, long long* instTotal,
                        long long* effectiveTris, size_t instBudget,
                        std::unordered_set<std::string>* consumed,
                        // Resolve a bound-material path to a DrawScene material
                        // index (the loader's cached resolveMaterialPath).
                        // Null = keep material 0 (default gray).
                        const std::function<int(const std::string&)>* resolveMat =
                            nullptr) {
  if (placements.empty()) return;
  double pr16[16];
  tydn::ComputeWorldTransform(stage, protoRoot, pr16, time);
  const matrix4d inv_proto = ::lightusd::inverse(Mat4dFromArray(pr16));

  std::vector<tnext::UsdPrim> directMeshes, nestedInstancers;
  SplitProtoSubtree(protoRoot, &directMeshes, &nestedInstancers);

  const bool haveColors =
      placementColors && placementColors->size() == placements.size() * 3;
  const bool haveOpacities =
      placementOpacities && placementOpacities->size() == placements.size();

  for (const tnext::UsdPrim& mp : directMeshes) {
    if (consumed) consumed->insert(mp.GetPath().str());
    DrawMeshCPU dm;
    matrix4d mesh_rel;
    std::vector<uint32_t> vertexToPoint;
    size_t numPoints = 0;
    tydn::RenderMesh renderMesh;
    if (!BuildProtoMesh(stage, conv, mp, inv_proto, time, &dm, &mesh_rel,
                        &vertexToPoint, &numPoints, &renderMesh)) {
      continue;
    }

    // Skeletal skinning on the prototype, which stays INSTANCED either way. GPU:
    // emit skin attributes + a bone block with an IDENTITY world -- the bones are
    // prototype-local and the instanced vertex shader applies each instance's o2w
    // AFTER skinning, so all instances share the one block. (Sound because USD
    // instancing requires identical composed contents: one skeleton, one pose.)
    // CPU: bake the static pose at `time` into the prototype's geometry. Both
    // no-op for unskinned prototypes.
    bool gpuSkinned = false;
    if (gpuSkinning) {
      double identW[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      gpuSkinned = SetupGpuSkinNext(stage, mp, time, &dm, vertexToPoint,
                                    numPoints, identW, identW, pr16, draw);
    }
    if (!gpuSkinned) BakeSkinning(stage, mp, time, &dm, vertexToPoint, numPoints);

    // Resolve the prototype mesh's bound material. FillFlatGeometry emits the
    // submesh with materialId 0 (default gray); without this every instanced
    // prototype ignored its material in the RT path and material-driven AOVs
    // (the flat instanced raster shader shades per-vertex color regardless).
    if (resolveMat) {
      const std::string bind =
          tnext::GetInheritedBoundMaterialPath(stage, mp.GetPath().str());
      if (!bind.empty()) {
        const int protoMat = (*resolveMat)(bind);
        if (protoMat > 0) {
          for (DrawSubmesh& sub : dm.submeshes) sub.materialId = protoMat;
          if (MaterialUsesPtex(*draw, protoMat) &&
              !ExpandPtexCorners(renderMesh, &dm)) {
            LOGW("Ptex material on '%s' requires unsupported non-quad or "
                 "mismatched topology; using texture fallback",
                 mp.GetPath().str().c_str());
          }
        }
      }
    }

    // Prototype-LOCAL bbox over the (untransformed) vertices, for per-instance
    // frustum culling + CUDA instance world-AABBs (each instance transforms it).
    if (!dm.vertices.empty()) {
      float lo[3] = {dm.vertices[0].px, dm.vertices[0].py, dm.vertices[0].pz};
      float hi[3] = {lo[0], lo[1], lo[2]};
      for (const DrawVertex& v : dm.vertices) {
        lo[0] = std::min(lo[0], v.px); hi[0] = std::max(hi[0], v.px);
        lo[1] = std::min(lo[1], v.py); hi[1] = std::max(hi[1], v.py);
        lo[2] = std::min(lo[2], v.pz); hi[2] = std::max(hi[2], v.pz);
      }
      // Pad by the GPU morph's max displacement so a morphed instance is not
      // wrongly frustum-culled (the rest box would miss the displaced geometry).
      for (int k = 0; k < 3; ++k) {
        dm.protoAabbMin[k] = lo[k] - dm.morphExtent[k];
        dm.protoAabbMax[k] = hi[k] + dm.morphExtent[k];
      }
    }
    dm.instanceXforms.reserve(placements.size() * 12);
    if (haveColors) dm.instanceColors.reserve(placements.size() * 3);
    if (haveOpacities) dm.instanceOpacities.reserve(placements.size());
    for (size_t k = 0; k < placements.size(); ++k) {
      if (static_cast<size_t>(*instTotal) + dm.instanceXforms.size() / 12 >=
          instBudget)
        break;
      const matrix4d fin = Mul4(mesh_rel, placements[k]);
      float o2w[12];
      Mat4dToO2W(fin, o2w);
      dm.instanceXforms.insert(dm.instanceXforms.end(), o2w, o2w + 12);
      if (haveColors) {
        dm.instanceColors.push_back((*placementColors)[k * 3 + 0]);
        dm.instanceColors.push_back((*placementColors)[k * 3 + 1]);
        dm.instanceColors.push_back((*placementColors)[k * 3 + 2]);
      }
      if (haveOpacities) {
        dm.instanceOpacities.push_back((*placementOpacities)[k]);
      }
      // Exact affine AABB transform via center/extents. This is equivalent to
      // transforming all eight corners, but uses one matrix multiply plus the
      // absolute linear matrix. Instance-heavy scenes execute this many times.
      double center[3], extent[3];
      for (int a = 0; a < 3; ++a) {
        center[a] = 0.5 * (double(dm.protoAabbMin[a]) +
                           double(dm.protoAabbMax[a]));
        extent[a] = 0.5 * (double(dm.protoAabbMax[a]) -
                           double(dm.protoAabbMin[a]));
      }
      float worldMin[3], worldMax[3];
      for (int a = 0; a < 3; ++a) {
        const double wc = center[0] * fin.m[0][a] +
                          center[1] * fin.m[1][a] +
                          center[2] * fin.m[2][a] + fin.m[3][a];
        const double we = extent[0] * std::fabs(fin.m[0][a]) +
                          extent[1] * std::fabs(fin.m[1][a]) +
                          extent[2] * std::fabs(fin.m[2][a]);
        worldMin[a] = static_cast<float>(wc - we);
        worldMax[a] = static_cast<float>(wc + we);
      }
      bounds->add(worldMin);
      bounds->add(worldMax);
    }
    if (dm.instanceXforms.empty()) continue;
    const size_t ninst = dm.instanceXforms.size() / 12;
    *instTotal += static_cast<long long>(ninst);
    *effectiveTris += (dm.indices.size() / 3) * ninst;
    for (int k = 0; k < 3; ++k) {
      dm.aabbMin[k] = bounds->mn[k];
      dm.aabbMax[k] = bounds->mx[k];
    }
    std::memset(dm.world, 0, sizeof(dm.world));
    dm.world[0] = dm.world[5] = dm.world[10] = dm.world[15] = 1.0f;
    draw->triangleCount += dm.indices.size() / 3;
    draw->meshes.push_back(std::move(dm));
  }

  // Nested instancers: compose each per-instance transform (relative to protoRoot)
  // with every outer placement, then recurse on the inner prototype.
  static const float kIdentQuat[4] = {1, 0, 0, 0};  // real-first (w,x,y,z)
  static const float kUnitScale[3] = {1, 1, 1};
  for (const tnext::UsdPrim& ni : nestedInstancers) {
    if (static_cast<size_t>(*instTotal) >= instBudget) break;
    if (ni.GetTypeName() == "PointInstancer") {
      double iw16[16];
      tydn::ComputeWorldTransform(stage, ni, iw16, time);
      const matrix4d ni_rel = Mul4(Mat4dFromArray(iw16), inv_proto);
      tydn::ValueArrayRead<float> positions;
      tydn::ReadFloatArray(ni, "positions", time, &positions);
      const size_t n = positions.size() / 3;
      tydn::ValueArrayRead<int32_t> protoIdx;
      tydn::ReadIntArray(ni, "protoIndices", time, &protoIdx);
      tydn::ValueArrayRead<float> orients;
      tydn::ReadFloatArray(ni, "orientations", time, &orients);
      tydn::ValueArrayRead<float> scales;
      tydn::ReadFloatArray(ni, "scales", time, &scales);
      tydn::ValueArrayRead<int64_t> invis;
      tydn::ReadInt64Array(ni, "invisibleIds", time, &invis);
      tydn::ValueArrayRead<int64_t> inactive;
      tydn::ReadInt64Array(ni, "inactiveIds", time, &inactive);
      tydn::ValueArrayRead<int64_t> ids;
      tydn::ReadInt64Array(ni, "ids", time, &ids);
      std::unordered_set<int64_t> hiddenSet(invis.begin(), invis.end());
      hiddenSet.insert(inactive.begin(), inactive.end());
      const std::vector<tnext::Path>* iprotos = ni.GetRelationship("prototypes");
      if (!iprotos) continue;
      std::vector<std::vector<uint32_t>> byProto(iprotos->size());
      for (size_t i = 0; i < n; ++i) {
        if (PointInstanceHidden(i, n, ids, hiddenSet)) continue;
        const int pix = (i < protoIdx.size()) ? protoIdx[i] : 0;
        if (pix >= 0 && pix < int(iprotos->size())) byProto[pix].push_back(uint32_t(i));
      }
      for (size_t pix = 0; pix < iprotos->size(); ++pix) {
        if (byProto[pix].empty()) continue;
        tnext::UsdPrim innerRoot = stage.GetPrimAtPath((*iprotos)[pix]);
        if (!innerRoot.IsValid()) continue;
        std::vector<matrix4d> innerPl;
        innerPl.reserve(byProto[pix].size() * placements.size());
        bool capped = false;
        for (const matrix4d& P : placements) {
          const matrix4d eff = Mul4(ni_rel, P);  // instancer effective world
          for (uint32_t j : byProto[pix]) {
            if (static_cast<size_t>(*instTotal) + innerPl.size() >= instBudget) {
              capped = true;
              break;
            }
            const float* q =
                (orients.size() >= (j + 1) * 4) ? &orients[j * 4] : kIdentQuat;
            const float* s =
                (scales.size() >= (j + 1) * 3) ? &scales[j * 3] : kUnitScale;
            innerPl.push_back(Mul4(InstanceTRS(&positions[j * 3], q, s), eff));
          }
          if (capped) break;
        }
        EmitInstancedProto(stage, conv, innerRoot, innerPl, nullptr, nullptr, time,
                           gpuSkinning, draw, bounds, instTotal, effectiveTris,
                           instBudget, consumed, resolveMat);
      }
    } else {
      const auto* s = ni.GetPrimSpec();
      if (!s) continue;
      const std::string ipath = s->meta().instance_prototype();
      if (ipath.empty()) continue;
      double w16[16];
      tydn::ComputeWorldTransform(stage, ni, w16, time);
      const matrix4d m_rel = Mul4(Mat4dFromArray(w16), inv_proto);
      // The native instance's children are proxies of its prototype; consume them.
      std::vector<tnext::UsdPrim> proxies;
      tydn::GatherMeshPrims(ni, &proxies);
      if (consumed)
        for (const tnext::UsdPrim& m : proxies) consumed->insert(m.GetPath().str());
      tnext::UsdPrim innerRoot = stage.GetPrimAtPath(ipath);
      if (!innerRoot.IsValid()) continue;
      std::vector<matrix4d> innerPl;
      innerPl.reserve(placements.size());
      for (const matrix4d& P : placements) innerPl.push_back(Mul4(m_rel, P));
      EmitInstancedProto(stage, conv, innerRoot, innerPl, nullptr, nullptr, time,
                         gpuSkinning, draw, bounds, instTotal, effectiveTris,
                         instBudget, consumed, resolveMat);
    }
  }
}

// Read a scalar float camera attribute, or `fallback` when absent/non-float.
float ReadCamFloatN(const tnext::UsdPrim& prim, const char* name, float fallback) {
  if (const tnext::Value* v = prim.GetPropertyValue(name)) {
    if (const float* f = v->as_float()) return *f;
  }
  return fallback;
}

double ReadCamDoubleN(const tnext::UsdPrim& prim, const char* name,
                      double fallback) {
  if (const tnext::Value* v = prim.GetPropertyValue(name)) {
    if (const double* d = v->as_double()) return *d;
    if (const float* f = v->as_float()) return *f;
  }
  return fallback;
}

DrawCameraCPU::StereoRole ReadStereoRoleN(const tnext::UsdPrim& prim) {
  if (const tnext::Value* v = prim.GetPropertyValue("stereoRole")) {
    if (const std::string* token = v->as_token()) {
      if (*token == "left") return DrawCameraCPU::StereoRole::Left;
      if (*token == "right") return DrawCameraCPU::StereoRole::Right;
    }
  }
  return DrawCameraCPU::StereoRole::Mono;
}

std::vector<float> ReadClippingPlanesN(const tnext::UsdPrim& prim) {
  std::vector<float> out;
  const tnext::Value* v = prim.GetPropertyValue("clippingPlanes");
  if (!v) return out;
  const std::vector<float>* planes = v->as_float_array();
  if (!planes) return out;
  out.assign(planes->begin(), planes->end());
  return out;
}

bool FindNextCameraRec(const tnext::Stage& stage, const tnext::UsdPrim& prim,
                       const std::string& name, double time,
                       NextCameraPose* out) {
  if (prim.GetTypeName() == "Camera") {
    const std::string path = prim.GetPath().str();
    const std::string pname = prim.GetName();
    // Match by exact name, exact path, or a "/<name>" path suffix.
    const bool match =
        name.empty() || pname == name || path == name ||
        (path.size() > name.size() &&
         path.compare(path.size() - name.size(), name.size(), name) == 0 &&
         path[path.size() - name.size() - 1] == '/');
    if (match) {
      double mw[16];
      if (tydn::ComputeWorldTransform(stage, prim, mw, time)) {
        const matrix4d m = Mat4dFromArray(mw);
        // Row-major (p*M): translation in row 3, local axes in rows 0..2. USD
        // cameras look down local -Z with local +Y up (see Mat4dToO2W above).
        float up[3] = {float(m.m[1][0]), float(m.m[1][1]), float(m.m[1][2])};
        float fwd[3] = {-float(m.m[2][0]), -float(m.m[2][1]), -float(m.m[2][2])};
        auto norm3 = [](float v[3]) {
          float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
        };
        norm3(up);
        norm3(fwd);
        out->eye[0] = float(m.m[3][0]);
        out->eye[1] = float(m.m[3][1]);
        out->eye[2] = float(m.m[3][2]);
        for (int k = 0; k < 3; ++k) {
          out->up[k] = up[k];
          out->forward[k] = fwd[k];
        }
        const float focal = ReadCamFloatN(prim, "focalLength", 50.0f);
        out->focalLength = focal;
        out->horizontalAperture =
            ReadCamFloatN(prim, "horizontalAperture", 20.955f);
        out->verticalAperture =
            ReadCamFloatN(prim, "verticalAperture", 15.2908f);
        out->horizontalApertureOffset =
            ReadCamFloatN(prim, "horizontalApertureOffset", 0.0f);
        out->verticalApertureOffset =
            ReadCamFloatN(prim, "verticalApertureOffset", 0.0f);
        out->exposure = ReadCamFloatN(prim, "exposure", 0.0f);
        out->focusDistance = ReadCamFloatN(prim, "focusDistance", 0.0f);
        out->fStop = ReadCamFloatN(prim, "fStop", 0.0f);
        out->shutterOpen = ReadCamDoubleN(prim, "shutter:open", 0.0);
        out->shutterClose = ReadCamDoubleN(prim, "shutter:close", 0.0);
        out->stereoRole = ReadStereoRoleN(prim);
        out->clippingPlanes = ReadClippingPlanesN(prim);
        if (const tnext::Value* v = prim.GetPropertyValue("projection")) {
          if (const std::string* token = v->as_token()) {
            out->projection = (*token == "orthographic")
                                  ? CameraProjection::Orthographic
                                  : CameraProjection::Perspective;
          }
        }
        const float vap = out->verticalAperture;
        out->fovYDeg = 2.0f *
                       std::atan(0.5f * vap / std::max(1.0e-6f, focal)) *
                       (180.0f / 3.14159265358979323846f);
        if (const tnext::Value* v = prim.GetPropertyValue("clippingRange")) {
          if (const float* f = v->as_float2()) {
            out->zNear = std::max(1.0e-4f, f[0]);
            out->zFar = std::max(out->zNear + 1.0e-3f, f[1]);
          }
        }
      }
      return true;
    }
  }
  for (const tnext::UsdPrim& child : prim.GetChildren()) {
    if (FindNextCameraRec(stage, child, name, time, out)) return true;
  }
  return false;
}

// --- Phase 2 --next texture loading -----------------------------------------
// The tydra-next converter records texture *metadata* (RenderTexture: asset
// path, wrap, value scale/bias, channel) into a scratch RenderScene even with
// load_textures=false, but never decodes pixels. Decoding is ours, and it runs
// through tydra::next::TextureDecoder -- the same decoder lusdrender uses, so
// the size cap and byte budget are applied identically and AT DECODE TIME (a
// large scene never has to hold every texture at full resolution first).

struct NextTexCache {
  std::unordered_map<std::string, int> byKey;  // key -> draw->textures index (-1 miss)
  std::unique_ptr<tydn::TextureDecoder> decoder;
  // Texture runtime options (keepCompressed + device caps) for the kept-
  // compressed KTX2 passthrough. Null = plain decode.
  const TextureRuntimeOptions* opt = nullptr;
  // Ptex atlases bypass TextureDecoder's decoded-byte accounting, so keep a
  // cumulative cap here. This is deliberately a residency cap, not merely a
  // per-file cap: production scenes may bind thousands of independent .ptx
  // files. Files beyond the cap retain a representative-face fallback.
  size_t ptexAtlasBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
  // Prevent the first large Ptex file from starving every later binding. The
  // streaming atlas can trade face resolution for space, so reserve a fair
  // one-eighth share (with a 1 MiB minimum) for later Ptex textures. The
  // cumulative cap still governs the aggregate; this ceiling mainly gives
  // shelf packing enough headroom for differently-shaped face sets.
  size_t ptexAtlasPerTextureBytes = 256ull * 1024ull * 1024ull;
  size_t ptexAtlasBytes = 0;
  std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
      ptexSourceByAsset;
  uint32_t ptexInitialFaces = 0;
  size_t ptexPhysicalCacheBytes = 32ull * 1024ull * 1024ull;
  double ptexBuildSeconds = 0.0;
  size_t ptexBuildCount = 0;
  bool ptexBudgetWarned = false;
  bool deferOrdinary = false;
  LoadControl* progress = nullptr;
};

// ".ktx2" suffix (case-insensitive).
bool EndsWithKtx2(const std::string& s) {
  if (s.size() < 5) return false;
  std::string e = s.substr(s.size() - 5);
  for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
  return e == ".ktx2";
}

bool EndsWithPtx(const std::string& s) {
  if (s.size() < 4) return false;
  return s[s.size() - 4] == '.' &&
         std::tolower(static_cast<unsigned char>(s[s.size() - 3])) == 'p' &&
         std::tolower(static_cast<unsigned char>(s[s.size() - 2])) == 't' &&
         std::tolower(static_cast<unsigned char>(s[s.size() - 1])) == 'x';
}

#if defined(LUSDVIEW_WITH_TEXTOOLS)
// Resolve `rel` (a companion named relative to the same layer as the texture)
// against an already-resolved sibling asset path.
std::string ResolveSiblingAsset(const std::string& resolved,
                                const std::string& rel) {
  if (rel.empty()) return std::string();
  if (lightusd::io::IsAbsPath(rel)) return rel;
  const size_t p = resolved.find_last_of('/');
  if (p == std::string::npos) return rel;
  return resolved.substr(0, p + 1) + rel;
}
#endif  // LUSDVIEW_WITH_TEXTOOLS

void TonemapHDRToRGBA8(const std::vector<float>& rgb, uint32_t width,
                       uint32_t height, light3d::Image* out) {
  if (!out || width == 0 || height == 0 || rgb.size() <
      static_cast<size_t>(width) * height * 3u) return;
  out->width = static_cast<int>(width);
  out->height = static_cast<int>(height);
  out->channels = 4;
  out->data.resize(static_cast<size_t>(width) * height * 4u);
  auto encode = [](float v) -> uint8_t {
    if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
    const float mapped = v / (1.0f + v);
    const float srgb = std::pow(std::min(1.0f, mapped), 1.0f / 2.2f);
    return static_cast<uint8_t>(std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
  };
  for (size_t i = 0, p = 0; i < static_cast<size_t>(width) * height;
       ++i, p += 3u) {
    out->data[i * 4u + 0] = encode(rgb[p + 0]);
    out->data[i * 4u + 1] = encode(rgb[p + 1]);
    out->data[i * 4u + 2] = encode(rgb[p + 2]);
    out->data[i * 4u + 3] = 255;
  }
}

// Decode an asset into an RGBA8 light3d::Image through the shared decoder.
bool DecodeNextImage(NextTexCache& tc, const std::string& asset,
                     bool srgb, light3d::Image* out,
                     std::vector<float>* hdrRGB = nullptr) {
  if (!tc.decoder || asset.empty()) return false;
  tydn::DecodedImage img;
  if (!tc.decoder->Decode(asset, srgb, &img)) return false;
  if (img.hdr) {
    if (hdrRGB) *hdrRGB = img.float_pixels;
    TonemapHDRToRGBA8(img.float_pixels, img.width, img.height, out);
    return !out->data.empty();
  }
  out->width = static_cast<int>(img.width);
  out->height = static_cast<int>(img.height);
  out->channels = 4;
  out->data = std::move(img.pixels);
  return true;
}

bool DecodeDeferredDrawTextureImpl(const DrawTextureCPU& placeholder,
                                   const TextureRuntimeOptions& runtime,
                                   uint64_t budgetBytes,
                                   DrawTextureCPU* decoded) {
  if (!decoded || placeholder.assetIdentifier.empty() ||
      !placeholder.deferredDecode || placeholder.isUdim || placeholder.isPtex ||
      EndsWithKtx2(placeholder.assetIdentifier)) {
    return false;
  }
  tydn::TextureDecodeOptions options;
  options.max_edge = runtime.maxTextureSize > 0
                         ? static_cast<uint32_t>(runtime.maxTextureSize)
                         : 0u;
  options.budget_bytes = budgetBytes;
  tydn::TextureDecoder decoder(options);
  tydn::DecodedImage image;
  if (!decoder.Decode(placeholder.assetIdentifier, placeholder.srgb, &image))
    return false;

  *decoded = placeholder;
  decoded->image.width = static_cast<int>(image.width);
  decoded->image.height = static_cast<int>(image.height);
  decoded->image.channels = 4;
  if (image.hdr) {
    decoded->hdrRGB = std::move(image.float_pixels);
    decoded->isHDR = !decoded->hdrRGB.empty();
    TonemapHDRToRGBA8(decoded->hdrRGB, image.width, image.height,
                      &decoded->image);
  } else {
    decoded->image.data = std::move(image.pixels);
    decoded->hdrRGB.clear();
    decoded->isHDR = false;
  }
  decoded->deferredDecode = false;
  CompressDrawTexture(runtime, decoded);
  return !decoded->image.data.empty() || !decoded->compressed.data.empty();
}

// Ptex remains a native face source, but a representative page is useful to
// fixed-function backends that do not yet expose a page-table sampler. It is
// deliberately only a fallback; the metadata above is retained for the
// face-local renderer and the page cache supplies the real lookup path.
bool DecodeNextPtexFallback(NextTexCache& tc, const std::string& asset,
                            bool srgb, light3d::Image* out) {
  if (!tc.decoder) return false;
  tydn::DecodedImage img;
  if (!tc.decoder->DecodePtexFace(asset, 0, 0, srgb, &img)) return false;
  out->width = static_cast<int>(img.width);
  out->height = static_cast<int>(img.height);
  out->channels = 4;
  out->data = std::move(img.pixels);
  return true;
}

bool NextResizeImage(light3d::Image* img, int w, int h, bool srgb);

int NextWrapToDraw(tydn::WrapMode w) {
  switch (w) {
    case tydn::WrapMode::Clamp: return static_cast<int>(WrapMode::ClampToEdge);
    case tydn::WrapMode::Mirror: return static_cast<int>(WrapMode::Mirror);
    case tydn::WrapMode::Black: return static_cast<int>(WrapMode::ClampToBorder);
    case tydn::WrapMode::Repeat:
    default: return static_cast<int>(WrapMode::Repeat);
  }
}

int NextScalarChannel(tydn::RenderTexture::Channel c) {
  switch (c) {
    case tydn::RenderTexture::Channel::G: return 1;
    case tydn::RenderTexture::Channel::B: return 2;
    case tydn::RenderTexture::Channel::A: return 3;
    default: return 0;  // R / RGB / RGBA
  }
}

// Resize an RGBA8 light3d::Image to (w,h) via tydra::ResizeImage. Mirrors
// mesh_build's ResizeDrawImage (minus the vendored textools fast path, which is
// file-local there) so --next UDIM tiles can be normalized to a common size.
bool NextResizeImage(light3d::Image* img, int w, int h, bool srgb) {
  if (!img || w <= 0 || h <= 0 || img->width <= 0 || img->height <= 0) return false;
  if (img->width == w && img->height == h) return true;
  lightusd::Image src;
  src.width = img->width;
  src.height = img->height;
  src.channels = img->channels;
  src.bpp = 8;
  src.format = lightusd::Image::PixelFormat::UInt;
  src.data = img->data;
  lightusd::Image dst;
  const auto filter = srgb ? lightusd::tydra::ResizeFilter::SRGB
                           : lightusd::tydra::ResizeFilter::Linear;
  std::string err;
  if (!lightusd::tydra::ResizeImage(src, w, h, &dst, filter, &err)) return false;
  img->width = dst.width;
  img->height = dst.height;
  img->channels = dst.channels;
  img->data = std::move(dst.data);
  return true;
}

bool NextResizeHDR(std::vector<float>* rgb, int width, int height, int w,
                   int h) {
  if (!rgb || width <= 0 || height <= 0 || w <= 0 || h <= 0 ||
      rgb->size() < static_cast<size_t>(width) * height * 3u) return false;
  if (width == w && height == h) return true;
  lightusd::Image src;
  src.width = width;
  src.height = height;
  src.channels = 3;
  src.bpp = 32;
  src.format = lightusd::Image::PixelFormat::Float;
  src.data.resize(rgb->size() * sizeof(float));
  std::memcpy(src.data.data(), rgb->data(), src.data.size());
  lightusd::Image dst;
  std::string err;
  if (!lightusd::tydra::ResizeImage(src, w, h, &dst,
                                    lightusd::tydra::ResizeFilter::Linear,
                                    &err)) return false;
  if (dst.data.size() != static_cast<size_t>(w) * h * 3u * sizeof(float))
    return false;
  rgb->resize(dst.data.size() / sizeof(float));
  std::memcpy(rgb->data(), dst.data.data(), dst.data.size());
  return true;
}

// Enumerate + decode UDIM tiles for a `<UDIM>`-tagged asset path into a UDIM
// DrawTextureCPU. tydra-next carries the literal `<UDIM>` token through verbatim
// (no udim handling in the converter), so we expand it ourselves: probe ids
// 1001..1100, decode each existing tile (base_dir or .usdz), normalize to a
// common size, and build the udimLayer[100] LUT the sampler2DArray path reads.
// Returns the DrawScene texture index or -1 if no tile decoded. Mirrors
// mesh_build's BuildDrawTextures UDIM branch / NormalizeUdimTiles / InitUdimLookup.
int LoadNextUdimTexture(NextTexCache& tc, DrawScene* draw,
                        const tydn::RenderTexture& rt, const std::string& asset,
                        bool srgb) {
  std::string pre, post;
  if (!lightusd::io::SplitUDIMPath(asset, &pre, &post)) return -1;

  DrawTextureCPU dt;
  for (uint32_t id = 1001; id <= 1100; ++id) {
    const std::string tilePath = pre + std::to_string(id) + post;
    DrawUdimTileCPU tile;
    if (!DecodeNextImage(tc, tilePath, srgb, &tile.image, &tile.hdrRGB))
      continue;  // absent tile
    tile.isHDR = !tile.hdrRGB.empty();
    tile.udim = id;
    tile.u = (id - 1001u) % 10u;
    tile.v = (id - 1001u) / 10u;
    tile.assetIdentifier = tilePath;
    dt.udimTiles.push_back(std::move(tile));
  }
  if (dt.udimTiles.empty()) return -1;

  dt.isUdim = true;
  dt.assetIdentifier = asset;
  dt.srgb = srgb;
  dt.wrapS = NextWrapToDraw(rt.wrap_s);
  dt.wrapT = NextWrapToDraw(rt.wrap_t);

  // Normalize all tiles to the max width/height, then LUT: udim-1001 -> layer.
  int w = 0, h = 0;
  for (const DrawUdimTileCPU& t : dt.udimTiles) {
    w = std::max(w, t.image.width);
    h = std::max(h, t.image.height);
  }
  if (w <= 0 || h <= 0) return -1;
  // Resize every tile to the common (w,h); DROP any that fail — the renderer
  // uploads udimTiles as a sampler2DArray requiring all layers to be exactly
  // udimTileWidth/Height, so a leftover wrong-sized tile would render as a
  // white/garbage layer instead of the intended "missing" (magenta) sentinel.
  {
    std::vector<DrawUdimTileCPU> sized;
    sized.reserve(dt.udimTiles.size());
    for (DrawUdimTileCPU& t : dt.udimTiles) {
      const int oldWidth = t.image.width;
      const int oldHeight = t.image.height;
      if ((oldWidth != w || oldHeight != h) &&
          (!NextResizeImage(&t.image, w, h, srgb) ||
           (t.isHDR && !NextResizeHDR(&t.hdrRGB, oldWidth, oldHeight, w, h)))) {
        continue;  // drop; its UDIM id stays unmapped (-1) in the LUT
      }
      sized.push_back(std::move(t));
    }
    dt.udimTiles = std::move(sized);
  }
  if (dt.udimTiles.empty()) return -1;
  dt.udimTileWidth = w;
  dt.udimTileHeight = h;
  dt.image = dt.udimTiles.front().image;  // representative fallback
  dt.isHDR = dt.udimTiles.front().isHDR;
  if (dt.isHDR) dt.hdrRGB = dt.udimTiles.front().hdrRGB;
  dt.udimLayer.fill(-1);
  for (size_t i = 0; i < dt.udimTiles.size(); ++i) {
    const uint32_t u = dt.udimTiles[i].udim;
    if (u >= 1001 && u <= 1100) dt.udimLayer[u - 1001] = static_cast<int>(i);
  }
  const int idx = static_cast<int>(draw->textures.size());
  draw->textures.push_back(std::move(dt));
  return idx;
}

// Decode + register the texture referenced by scratch.textures[texId]. Deduped
// by (asset, srgb, wrap). Returns the DrawScene texture index or -1.
int LoadNextTexture(NextTexCache& tc, DrawScene* draw,
                    const tydn::RenderScene& scratch, int32_t texId, bool srgb) {
  if (texId < 0 || static_cast<size_t>(texId) >= scratch.textures.size()) return -1;
  const tydn::RenderTexture& rt = scratch.textures[static_cast<size_t>(texId)];
  // Prefer the image's RESOLVED path. `RenderTexture::asset_path` is the raw
  // authored string, and for a look layer nested below the root that is relative
  // to THAT layer (`../../texture/foo.png`) -- it does not resolve against the
  // scene file. `resolved_path` has been anchored to the authoring layer by the
  // converter (see next/layer/asset-anchor.hh). For root-layer and USDZ-internal
  // assets the two are identical, so this only ever adds the anchor.
  std::string asset;
  if (rt.image_id >= 0 &&
      static_cast<size_t>(rt.image_id) < scratch.images.size()) {
    asset = scratch.images[static_cast<size_t>(rt.image_id)].resolved_path;
  }
  if (asset.empty()) asset = rt.asset_path;
  if (asset.empty()) return -1;

  const std::string key = asset + (srgb ? "|s" : "|l") + "|" +
      std::to_string(static_cast<int>(rt.wrap_s)) + "," +
      std::to_string(static_cast<int>(rt.wrap_t));
  auto it = tc.byKey.find(key);
  if (it != tc.byKey.end()) return it->second;
  if (tc.progress) tc.progress->texturesTotal.fetch_add(1);

  // Interactive large-scene material discovery must not read texture pixels.
  // Reserve the stable slot now; a bounded post-geometry worker stage fills it.
  // Keep archive, UDIM, Ptex, and kept-compressed KTX paths synchronous until
  // their shared readers have explicit concurrent ownership.
  if (tc.deferOrdinary && !lightusd::io::IsUDIMPath(asset) &&
      !EndsWithPtx(asset) && !EndsWithKtx2(asset) && rt.ktx2_hint.empty()) {
    DrawTextureCPU dt;
    dt.assetIdentifier =
        lightusd::io::IsAbsPath(asset) || !tc.decoder
            ? asset
            : lightusd::io::JoinPath(tc.decoder->options().base_dir, asset);
    dt.srgb = srgb;
    dt.wrapS = NextWrapToDraw(rt.wrap_s);
    dt.wrapT = NextWrapToDraw(rt.wrap_t);
    dt.deferredDecode = true;
    const int idx = static_cast<int>(draw->textures.size());
    draw->textures.push_back(std::move(dt));
    tc.byKey[key] = idx;
    return idx;
  }

  // UDIM: tydra-next carries the literal `<UDIM>` token through, so expand +
  // decode tiles ourselves into a sampler2DArray-backed UDIM texture.
  if (lightusd::io::IsUDIMPath(asset)) {
    const int uidx = LoadNextUdimTexture(tc, draw, rt, asset, srgb);
    tc.byKey[key] = uidx;
    if (tc.progress) tc.progress->texturesDone.fetch_add(1);
    return uidx;
  }

  DrawTextureCPU dt;
  bool built = false;
  if (EndsWithPtx(asset) && tc.decoder) {
    dt.isPtex = true;
    dt.assetIdentifier = asset;
    const size_t remaining =
        tc.ptexAtlasBytes < tc.ptexAtlasBudgetBytes
            ? tc.ptexAtlasBudgetBytes - tc.ptexAtlasBytes
            : 0;
    if (remaining == 0) {
      DecodeNextPtexFallback(tc, asset, srgb, &dt.image);
      if (!tc.ptexBudgetWarned) {
        draw->skipped.push_back(
            "Ptex atlas residency budget exhausted; using representative "
            "face fallbacks for subsequent textures");
        tc.ptexBudgetWarned = true;
      }
      built = true;
    }
    std::vector<uint8_t> bytes;
    std::shared_ptr<const std::vector<uint8_t>> sharedBytes;
    if (!built) {
      auto sourceIt = tc.ptexSourceByAsset.find(asset);
      if (sourceIt != tc.ptexSourceByAsset.end()) {
        sharedBytes = sourceIt->second;
      } else if (tc.decoder->ReadAssetBytes(asset, &bytes)) {
        sharedBytes = std::make_shared<const std::vector<uint8_t>>(
            std::move(bytes));
        tc.ptexSourceByAsset.emplace(asset, sharedBytes);
      }
    }
    ::lightusd::ptx::Reader ptx;
    std::string ptxErr;
    if (!built && sharedBytes &&
        ::lightusd::ptx::Reader::OpenMemory(sharedBytes->data(),
                                             sharedBytes->size(), &ptx,
                                            &ptxErr)) {
      const auto ptexBuildBegin = std::chrono::steady_clock::now();
      const ::lightusd::ptx::Info& pi = ptx.info();
      dt.ptexFaces = pi.faces;
      dt.ptexLevels = pi.levels;
      dt.ptexChannels = pi.channels;
      for (const ::lightusd::ptx::FaceInfo& fi : pi.faceInfo) {
        dt.ptexMaxFaceEdge = std::max(dt.ptexMaxFaceEdge,
                                      std::max(fi.width(), fi.height()));
      }
      PtexAtlasOptions atlasOptions;
      atlasOptions.maxFaceEdge = std::min(dt.ptexMaxFaceEdge, 512u);
      if (tc.opt && tc.opt->maxTextureSize > 0) {
        atlasOptions.maxFaceEdge =
            std::min(atlasOptions.maxFaceEdge,
                     static_cast<uint32_t>(tc.opt->maxTextureSize));
      }
      atlasOptions.maxAtlasBytes =
          std::min(tc.ptexAtlasPerTextureBytes, remaining);
      atlasOptions.maxPhysicalCacheBytes =
          std::min<size_t>(tc.ptexPhysicalCacheBytes,
                           atlasOptions.maxAtlasBytes / 4u);
      atlasOptions.forcePhysicalCache =
          std::getenv("LUSDVIEW_PTEX_FORCE_RESIDENCY") != nullptr;
      atlasOptions.initialFaceLimit = tc.ptexInitialFaces;
      PtexAtlasBuildStats atlasStats;
      if (!BuildPtexAtlas(ptx, atlasOptions, srgb, &dt.image,
                          &dt.ptexFaceRects, &atlasStats, &ptxErr)) {
        DecodeNextPtexFallback(tc, asset, srgb, &dt.image);
      } else {
        tc.ptexAtlasBytes += dt.image.data.size();
        dt.ptexAtlasBytes = dt.image.data.size();
        dt.ptexDownsampledFaces = atlasStats.downsampledFaces;
        dt.ptexPageCacheHits = atlasStats.pageCache.hits;
        dt.ptexPageCacheMisses = atlasStats.pageCache.misses;
        dt.ptexPageCacheEvictions = atlasStats.pageCache.evictions;
        dt.ptexPageCachePeakBytes = atlasStats.pageCache.peakResidentBytes;
        dt.ptexPageDecodedBytes = atlasStats.pageCache.decodedBytes;
        dt.ptexGutter = atlasOptions.gutter;
        dt.ptexTileEdge = atlasOptions.maxFaceEdge;
        dt.ptexRectTexelOffset = atlasStats.rectTexelOffset;
        dt.ptexPhysicalCacheOffsetY = atlasStats.physicalCacheOffsetY;
        dt.ptexPhysicalCacheSlotEdge = atlasStats.physicalCacheSlotEdge;
        dt.ptexPhysicalCacheSlots = atlasStats.physicalCacheSlots;
        if (dt.ptexPhysicalCacheSlots > 0) {
          dt.ptexSourceDataShared = sharedBytes;
          dt.streamingMutable = true;
          dt.ptexForceResidency = atlasOptions.forcePhysicalCache;
          dt.ptexDemandDriven = atlasOptions.initialFaceLimit > 0;
        }
      }
      tc.ptexBuildSeconds +=
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        ptexBuildBegin)
              .count();
      ++tc.ptexBuildCount;
      built = true;
    }
  }
#if defined(LUSDVIEW_WITH_TEXTOOLS)
  // Kept-compressed KTX2 passthrough. The compressed companion is named by the
  // `inputs:file` customData `ktx2` hint (RenderTexture::ktx2_hint), or the
  // asset itself may already be a .ktx2. Upload/transcode its GPU blocks instead
  // of decoding + re-encoding.
  if (tc.opt && tc.opt->keepCompressed && tc.decoder) {
    std::string ktxAsset;
    if (EndsWithKtx2(asset)) {
      ktxAsset = asset;
    } else if (!rt.ktx2_hint.empty()) {
      ktxAsset = ResolveSiblingAsset(asset, rt.ktx2_hint);
    }
    if (!ktxAsset.empty()) {
      std::vector<uint8_t> bytes;
      if (tc.decoder->ReadAssetBytes(ktxAsset, &bytes) &&
          BuildKeptCompressedFromKtx2(bytes.data(), bytes.size(), *tc.opt, &dt)) {
        dt.assetIdentifier = ktxAsset;
        built = true;
      }
    }
  }
#endif
  if (!built && !DecodeNextImage(tc, asset, srgb, &dt.image, &dt.hdrRGB)) {
    tc.byKey[key] = -1;  // negative-cache the miss
    if (tc.progress) tc.progress->texturesDone.fetch_add(1);
    return -1;
  }
  if (!built) dt.assetIdentifier = asset;
  dt.isHDR = !dt.hdrRGB.empty();
  dt.srgb = srgb;
  dt.wrapS = NextWrapToDraw(rt.wrap_s);
  dt.wrapT = NextWrapToDraw(rt.wrap_t);
  const int idx = static_cast<int>(draw->textures.size());
  draw->textures.push_back(std::move(dt));
  tc.byKey[key] = idx;
  if (tc.progress) tc.progress->texturesDone.fetch_add(1);
  return idx;
}

// Which UV set a texture samples. RenderTexture::uv_primvar carries the name the
// texture's UsdPrimvarReader asked for; the mesh reports the names it actually
// extracted into slots 0 and 1. Anything that is not the secondary set -- the
// usual case, and any unresolvable name -- falls back to slot 0, which is what
// the renderer did unconditionally before.
int ResolveUvSet(const tydn::RenderTexture& rt, const std::string& uv0Name,
                 const std::string& uv1Name) {
  if (uv1Name.empty() || rt.uv_primvar.empty()) return 0;
  if (rt.uv_primvar == uv1Name && uv1Name != uv0Name) return 1;
  return 0;
}

// Fill a DrawTexSampleCPU's UV affine + value scale/bias from a RenderTexture.
void FillNextSample(const tydn::RenderTexture& rt, DrawTexSampleCPU* smp,
                    const std::string& uv0Name = std::string(),
                    const std::string& uv1Name = std::string()) {
  smp->uvSet = ResolveUvSet(rt, uv0Name, uv1Name);
  const float c = std::cos(rt.rotation), s = std::sin(rt.rotation);
  smp->uv.m00 = c * rt.scale.x; smp->uv.m01 = -s * rt.scale.y;
  smp->uv.m10 = s * rt.scale.x; smp->uv.m11 =  c * rt.scale.y;
  smp->uv.tx = rt.offset.x; smp->uv.ty = rt.offset.y;
  smp->scale[0] = rt.scale_value.x; smp->scale[1] = rt.scale_value.y;
  smp->scale[2] = rt.scale_value.z; smp->scale[3] = rt.scale_value.w;
  smp->bias[0] = rt.bias.x; smp->bias[1] = rt.bias.y;
  smp->bias[2] = rt.bias.z; smp->bias[3] = rt.bias.w;
  // Wrap and source color space used to be dropped here and recovered per
  // backend from the shared DrawTextureCPU. That is wrong when two slots sample
  // one image with different intent, so carry them per-slot.
  auto wrap = [](tydn::WrapMode w) {
    switch (w) {
      case tydn::WrapMode::Repeat: return WrapMode::Repeat;
      case tydn::WrapMode::Mirror: return WrapMode::Mirror;
      case tydn::WrapMode::Black: return WrapMode::ClampToBorder;
      case tydn::WrapMode::Clamp: default: return WrapMode::ClampToEdge;
    }
  };
  smp->wrapS = wrap(rt.wrap_s);
  smp->wrapT = wrap(rt.wrap_t);
  const std::string& cs = rt.source_color_space;
  smp->colorSpace = (cs == "sRGB" || cs == "srgb")
                        ? DrawColorSpace::sRGB
                        : ((cs == "raw" || cs == "Raw" || cs == "linear")
                               ? DrawColorSpace::Raw
                               : DrawColorSpace::Auto);
  // -1 means "use the whole value". NextScalarChannel() collapses RGB/RGBA to 0,
  // which would wrongly label a color slot as single-channel R, so only record a
  // channel for genuinely scalar outputs.
  switch (rt.output_channel) {
    case tydn::RenderTexture::Channel::R: smp->channel = 0; break;
    case tydn::RenderTexture::Channel::G: smp->channel = 1; break;
    case tydn::RenderTexture::Channel::B: smp->channel = 2; break;
    case tydn::RenderTexture::Channel::A: smp->channel = 3; break;
    default: smp->channel = -1; break;
  }
}

// Convert a bound material prim into a DrawMaterialCPU appended to `draw`, and
// return its index (>=1). Bakes PBR constants and independent base-color,
// metallic, roughness, emissive, normal, and opacity texture semantics.
// Reuses lusdview's own BakeLightRtOpenPBR so the --next path shades materials
// through the same path the legacy loader uses. Returns -1 if the prim has no
// usable surface shader (caller then keeps the default gray material, index 0).
int BuildNextMaterial(const tnext::Stage& stage, tydn::RenderSceneConverter& conv,
                      const tnext::UsdPrim& matPrim, DrawScene* draw,
                      NextTexCache& texCache, const std::string& uv0Name,
                      const std::string& uv1Name) {
  tydn::RenderScene scratch;  // texture/image metadata (pixels decoded by us)
  tydn::RenderMaterial rm;
  if (!conv.ConvertMaterial(stage, matPrim, &rm, &scratch)) return -1;

  auto setRGB = [](float* dst, const tydn::Float4& v, float w) {
    dst[0] = v.x * w; dst[1] = v.y * w; dst[2] = v.z * w;
  };
  // Load a color texture into a slot; on success neutralise the baked constant
  // (so the texture isn't double-tinted) and fill the UV/scale sample.
  auto colorSlot = [&](const tydn::ShaderParam& sp, bool srgb, int* texField,
                       DrawTexSampleCPU* smp, float* neutralize3) {
    if (sp.texture_id < 0) return;
    int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, srgb);
    if (t < 0) return;
    *texField = t;
    FillNextSample(scratch.textures[static_cast<size_t>(sp.texture_id)], smp,
                   uv0Name, uv1Name);
    if (neutralize3) { neutralize3[0] = neutralize3[1] = neutralize3[2] = 1.0f; }
  };

  auto channelScaleBias = [](const tydn::RenderTexture& rt, int ch,
                             float* scale, float* bias) {
    const float sc[4] = {rt.scale_value.x, rt.scale_value.y,
                         rt.scale_value.z, rt.scale_value.w};
    const float bi[4] = {rt.bias.x, rt.bias.y, rt.bias.z, rt.bias.w};
    const int c = (ch >= 0 && ch < 4) ? ch : 0;
    *scale = sc[c];
    *bias = bi[c];
  };

  DrawMaterialCPU dm;
  dm.name = rm.name;
  dm.absPath = rm.prim_path;
  dm.displayName = rm.name;
  dm.hasDisplacementOutput = rm.has_displacement;
  dm.hasVolumeOutput = rm.has_volume;
  dm.displacementShaderPath = rm.displacement_shader_path;
  dm.volumeShaderPath = rm.volume_shader_path;
  dm.volumeMaterialXNodeGraphJson = rm.volume_nodegraph_json;
  ResolveNextDisplacementMaterial(stage, matPrim, &dm);
  if (rm.has_volume)
    ResolveNextSurfaceVolumeMaterial(stage, matPrim, &dm);
  bool reportedDegradedMaterial = false;
  for (const tydn::MaterialDiagnostic& diagnostic : rm.diagnostics) {
    if (diagnostic.kind == tydn::MaterialDiagnosticKind::DegradedMaterial) {
      std::string message = "material '" + diagnostic.material_path +
                            "': using degraded material";
      if (!diagnostic.node_path.empty())
        message += " from node '" + diagnostic.node_path + "'";
      if (!diagnostic.shader_id.empty())
        message += " (" + diagnostic.shader_id + ")";
      if (!diagnostic.message.empty()) message += ": " + diagnostic.message;
      draw->skipped.push_back(std::move(message));
      reportedDegradedMaterial = true;
    } else if (diagnostic.kind ==
               tydn::MaterialDiagnosticKind::UnsupportedMaterialXNode) {
      std::string message =
          "unsupported MaterialX node '" + diagnostic.node_path +
          "' in material '" + diagnostic.material_path + "'";
      if (!diagnostic.shader_id.empty())
        message += " (" + diagnostic.shader_id + ")";
      draw->skipped.push_back(std::move(message));
    }
  }
  if (rm.default_fallback && !reportedDegradedMaterial) {
    draw->skipped.push_back("material '" + rm.prim_path +
                            "': using degraded material");
  }

  bool jpegOpacityCoverage = false;
  auto looksLikeBinaryCoverage = [](const light3d::Image& image,
                                    int channel) -> bool {
    if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
        image.data.empty()) {
      return false;
    }
    const int c = std::max(0, std::min(channel, image.channels - 1));
    const size_t pixels = static_cast<size_t>(image.width) *
                          static_cast<size_t>(image.height);
    // Inspect at most ~64K regularly spaced texels. JPEG ringing around a hard
    // black/white cutout stays near the endpoints; a lens/transmission map has
    // a meaningful population of intermediate values and must remain Blend.
    const size_t step = std::max<size_t>(1, pixels / 65536u);
    size_t samples = 0;
    size_t middle = 0;
    size_t low = 0;
    size_t high = 0;
    for (size_t p = 0; p < pixels; p += step) {
      const uint8_t v = image.data[p * static_cast<size_t>(image.channels) +
                                   static_cast<size_t>(c)];
      ++samples;
      if (v <= 32) ++low;
      else if (v >= 223) ++high;
      else ++middle;
    }
    return samples > 0 && low > 0 && high > 0 &&
           middle * 50u <= samples;  // <=2% non-binary texels
  };
  auto loadOpacity = [&](const tydn::ShaderParam& sp, int baseTextureId) {
    if (sp.texture_id < 0 ||
        static_cast<size_t>(sp.texture_id) >= scratch.textures.size()) return;
    const tydn::RenderTexture& rt =
        scratch.textures[static_cast<size_t>(sp.texture_id)];
    const size_t dot = rt.asset_path.find_last_of('.');
    if (dot != std::string::npos) {
      std::string ext = rt.asset_path.substr(dot);
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      jpegOpacityCoverage = ext == ".jpg" || ext == ".jpeg";
    }
    const int channel = NextScalarChannel(rt.output_channel);
    // The material shader already multiplies base-color alpha. A common USD
    // graph connects the SAME UsdUVTexture outputs:rgb to diffuseColor and
    // outputs:a to opacity; binding it again would square the alpha.
    if (sp.texture_id == baseTextureId && channel == 3) return;
    int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, false);
    if (t < 0) return;
    dm.opacityTex = t;
    dm.opacityChannel = channel;
    if (jpegOpacityCoverage) {
      jpegOpacityCoverage =
          static_cast<size_t>(t) < draw->textures.size() &&
          looksLikeBinaryCoverage(draw->textures[static_cast<size_t>(t)].image,
                                  channel);
    }
    FillNextSample(rt, &dm.opacitySample, uv0Name, uv1Name);
    channelScaleBias(rt, channel, &dm.opacityTexScale, &dm.opacityTexBias);
  };

  auto loadOcclusion = [&](const tydn::ShaderParam& sp) {
    if (sp.texture_id < 0 ||
        static_cast<size_t>(sp.texture_id) >= scratch.textures.size()) return;
    const tydn::RenderTexture& rt =
        scratch.textures[static_cast<size_t>(sp.texture_id)];
    const int channel = NextScalarChannel(rt.output_channel);
    const int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, false);
    if (t < 0) return;
    dm.occlusionTex = t;
    dm.occlusionChannel = channel;
    FillNextSample(rt, &dm.occlusionSample, uv0Name, uv1Name);
    channelScaleBias(rt, channel, &dm.occlusionTexScale,
                     &dm.occlusionTexBias);
  };

  // Generic scalar slot (coat weight/roughness, specular-workflow-adjacent
  // scalars). These were constant-only before, so an authored map collapsed
  // silently to its fallback constant.
  auto scalarSlot = [&](const tydn::ShaderParam& sp, int* texField,
                        DrawTexSampleCPU* smp, float* neutralize1) {
    if (sp.texture_id < 0 ||
        static_cast<size_t>(sp.texture_id) >= scratch.textures.size()) return;
    const int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, false);
    if (t < 0) return;
    *texField = t;
    FillNextSample(scratch.textures[static_cast<size_t>(sp.texture_id)], smp,
                   uv0Name, uv1Name);
    smp->tex = t;
    if (smp->channel < 0) smp->channel = 0;  // scalar slots read one channel
    if (neutralize1) *neutralize1 = 1.0f;
  };

  // Displacement as a full sample, so it can use UV set 1 and per-channel
  // scale/bias like every other slot. displacementUv/Tex{Scale,Bias} are kept in
  // sync for the existing displacement code paths.
  auto displacementSlot = [&](const tydn::ShaderParam& sp) {
    dm.displacementConst = sp.value.x;
    if (sp.texture_id < 0 ||
        static_cast<size_t>(sp.texture_id) >= scratch.textures.size()) return;
    const tydn::RenderTexture& dt =
        scratch.textures[static_cast<size_t>(sp.texture_id)];
    const int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, false);
    if (t < 0) return;
    dm.displacementTex = t;
    FillNextSample(dt, &dm.displacementSample, uv0Name, uv1Name);
    dm.displacementSample.tex = t;
    dm.displacementSample.isUdim = draw->textures[static_cast<size_t>(t)].isUdim;
    if (dm.displacementSample.channel < 0) dm.displacementSample.channel = 0;
    dm.displacementUv = dm.displacementSample.uv;
    dm.displacementTexScale = dt.scale_value.x;
    dm.displacementTexBias = dt.bias.x;
  };

  // Load a normal-map slot (linear; default [0,1]->[-1,1] remap if unauthored).
  auto loadNormal = [&](const tydn::ShaderParam& sp, int* texField,
                        DrawTexSampleCPU* sample) {
    if (sp.texture_id < 0) return;
    int t = LoadNextTexture(texCache, draw, scratch, sp.texture_id, false);
    if (t < 0) return;
    *texField = t;
    const tydn::RenderTexture& rt = scratch.textures[static_cast<size_t>(sp.texture_id)];
    FillNextSample(rt, sample, uv0Name, uv1Name);
    const bool defScale = rt.scale_value.x == 1.0f && rt.scale_value.y == 1.0f &&
                          rt.scale_value.z == 1.0f;
    const bool defBias = rt.bias.x == 0.0f && rt.bias.y == 0.0f && rt.bias.z == 0.0f;
    if (defScale && defBias) {
      sample->scale[0] = sample->scale[1] = sample->scale[2] = 2.0f;
      sample->bias[0] = sample->bias[1] = sample->bias[2] = -1.0f;
    }
  };
  // Metallic and roughness are independent slots. Packed ORM inputs naturally
  // alias the same DrawTextureCPU while retaining their channel and UV metadata.
  auto loadMetalRough = [&](const tydn::ShaderParam& metallic,
                            const tydn::ShaderParam& roughness) {
    // Per-channel value scale/bias for a scalar texture: the sampled channel's
    // component of the texture's inputs:scale / inputs:bias. Dropping these
    // (they used to stay 1/0) mis-scaled any roughness/metallic map authored
    // with a non-identity scale -- lusdrender applies them, so the two tools
    // disagreed on the same asset.
    if (roughness.texture_id >= 0) {
      int t = LoadNextTexture(texCache, draw, scratch, roughness.texture_id, false);
      if (t >= 0) {
        dm.roughnessTex = t;
        dm.roughness = 1.0f;
        const tydn::RenderTexture& rt =
            scratch.textures[static_cast<size_t>(roughness.texture_id)];
        dm.roughnessChannel = NextScalarChannel(rt.output_channel);
        channelScaleBias(rt, dm.roughnessChannel, &dm.roughnessTexScale,
                         &dm.roughnessTexBias);
        FillNextSample(rt, &dm.roughnessSample, uv0Name, uv1Name);
      }
    }
    if (metallic.texture_id >= 0) {
      int t = LoadNextTexture(texCache, draw, scratch, metallic.texture_id, false);
      if (t >= 0) {
        const tydn::RenderTexture& rt =
            scratch.textures[static_cast<size_t>(metallic.texture_id)];
        dm.metallicTex = t;
        FillNextSample(rt, &dm.metallicSample, uv0Name, uv1Name);
        dm.metallic = 1.0f;
        dm.metallicChannel = NextScalarChannel(rt.output_channel);
        channelScaleBias(rt, dm.metallicChannel, &dm.metallicTexScale,
                         &dm.metallicTexBias);
      }
    }
  };

  // A material can author BOTH a UsdPreviewSurface and an OpenPBR/mtlx shader
  // (DCC exports, MaterialX-with-fallback); ConvertMaterial fills both but sets
  // shader_type to the last child (often OpenPBR). Pick the shader that resolved
  // the most textures; on a tie retain the converter's authoritative shader
  // type. This matters for constant-only OpenPBR materials: selecting a
  // synthesized PreviewSurface fallback would silently replace authored
  // OpenPBR-only lanes such as specular_ior. Falls back cleanly for
  // single-shader materials.
  auto texCount = [](std::initializer_list<int> ids) {
    int n = 0; for (int i : ids) if (i >= 0) ++n; return n;
  };
  int pvTex = -1, opTex = -1;
  if (rm.preview_surface) {
    const tydn::PreviewSurfaceShader& s = *rm.preview_surface;
    pvTex = texCount({s.diffuse_color.texture_id, s.normal.texture_id,
                      s.emissive_color.texture_id, s.metallic.texture_id,
                      s.roughness.texture_id, s.opacity.texture_id,
                      s.occlusion.texture_id, s.specular_color.texture_id,
                      s.clearcoat.texture_id,
                      s.clearcoat_roughness.texture_id,
                      s.displacement.texture_id});
  }
  if (rm.openpbr) {
    const tydn::OpenPBRSurfaceShader& s = *rm.openpbr;
    opTex = texCount({s.base_color.texture_id, s.normal.texture_id,
                      s.emission_color.texture_id, s.base_metalness.texture_id,
                      s.base_roughness.texture_id, s.opacity.texture_id,
                      s.displacement.texture_id, s.coat_normal.texture_id,
                      s.coat_weight.texture_id, s.coat_color.texture_id,
                      s.coat_roughness.texture_id,
                      s.specular_color.texture_id,
                      s.specular_roughness.texture_id});
  }
  const bool preferPreviewOnTie =
      rm.shader_type != tydn::RenderMaterial::ShaderType::OpenPBR;
  const bool usePreview =
      rm.preview_surface &&
      (!rm.openpbr || pvTex > opTex || (pvTex == opTex && preferPreviewOnTie));

  if (usePreview) {
    const tydn::PreviewSurfaceShader& s = *rm.preview_surface;
    dm.hasUsdPreviewSurface = true;
    setRGB(dm.baseColor, s.diffuse_color.value, 1.0f);
    dm.metallic = s.metallic.value.x;
    dm.roughness = s.roughness.value.x;
    setRGB(dm.emissive, s.emissive_color.value, 1.0f);
    dm.alpha = s.opacity.value.x;
    // Specular workflow + IOR (T12): lusdrender honors both; lusdview used to
    // fall back to the metallic workflow at a fixed dielectric IOR 1.5.
    dm.useSpecularWorkflow = s.use_specular_workflow;
    setRGB(dm.specularColor, s.specular_color.value, 1.0f);
    dm.ior = s.ior.value.x;
    colorSlot(s.diffuse_color, true, &dm.baseColorTex, &dm.baseColorSample, dm.baseColor);
    colorSlot(s.emissive_color, true, &dm.emissiveTex, &dm.emissiveSample, dm.emissive);
    loadOpacity(s.opacity, s.diffuse_color.texture_id);
    loadOcclusion(s.occlusion);
    loadNormal(s.normal, &dm.normalTex, &dm.normalSample);
    loadMetalRough(s.metallic, s.roughness);
    // Specular-workflow F0 map; only consulted when useSpecularWorkflow is set.
    colorSlot(s.specular_color, true, &dm.specularColorTex,
              &dm.specularColorSample, dm.specularColor);
    // PreviewSurface clearcoat -> coat lobe. These had no texture slot at all,
    // so an authored clearcoat map silently rendered as its constant.
    scalarSlot(s.clearcoat, &dm.coatWeightTex, &dm.coatWeightSample,
               &dm.coatWeight);
    scalarSlot(s.clearcoat_roughness, &dm.coatRoughnessTex,
               &dm.coatRoughnessSample, &dm.coatRoughness);
    // Preserve PreviewSurface displacement through the same texture/constant
    // slot path used by OpenPBR so raster and ray-traced backends agree.
    displacementSlot(s.displacement);
  } else if (rm.openpbr) {
    const tydn::OpenPBRSurfaceShader& s = *rm.openpbr;
    dm.hasOpenPBRSurface = true;
    dm.openPbrSpecularModel = true;
    dm.materialXNodeGraphJson = s.nodegraph_json;
    setRGB(dm.baseColor, s.base_color.value, s.base_weight.value.x);
    dm.metallic = s.base_metalness.value.x;
    dm.roughness = s.specular_roughness.value.x;
    setRGB(dm.emissive, s.emission_color.value, s.emission_luminance.value.x);
    dm.alpha = s.opacity.value.x;
    // OpenPBR is a metalness workflow; honor its dielectric IOR for F0 (T12).
    dm.ior = s.specular_ior.value.x;
    colorSlot(s.base_color, true, &dm.baseColorTex, &dm.baseColorSample, dm.baseColor);
    colorSlot(s.emission_color, true, &dm.emissiveTex, &dm.emissiveSample, dm.emissive);
    loadOpacity(s.opacity, s.base_color.texture_id);
    loadNormal(s.normal, &dm.normalTex, &dm.normalSample);
    loadNormal(s.coat_normal, &dm.coatNormalTex, &dm.coatNormalSample);
    // Converted MaterialX standard_surface graphs carry their microfacet map
    // in specular_roughness; native OpenPBR assets generally use
    // base_roughness. Both feed the single real-time roughness lane.
    const tydn::ShaderParam& realtimeRoughness =
        (s.base_roughness.texture_id >= 0) ? s.base_roughness
                                          : s.specular_roughness;
    loadMetalRough(s.base_metalness, realtimeRoughness);
    // Coat lobe maps (constant-only before this).
    scalarSlot(s.coat_weight, &dm.coatWeightTex, &dm.coatWeightSample,
               &dm.coatWeight);
    colorSlot(s.coat_color, true, &dm.coatColorTex, &dm.coatColorSample,
              dm.coatColor);
    scalarSlot(s.coat_roughness, &dm.coatRoughnessTex, &dm.coatRoughnessSample,
               &dm.coatRoughness);
    colorSlot(s.specular_color, true, &dm.specularColorTex,
              &dm.specularColorSample, dm.specularColor);
    displacementSlot(s.displacement);
  } else {
    return -1;  // no PreviewSurface/OpenPBR -- fall back to default material
  }

  // Keep a composed OpenPBR alternative discoverable by the live editor even
  // when the richer PreviewSurface fallback wins initial rendering. This is
  // used by StandardShaderBall's neutral shell: the user can explicitly turn
  // that dual-authored material into a connection-free OpenPBR preview without
  // making the sparse OpenPBR branch degrade the scene at load time.
  if (rm.openpbr) dm.hasOpenPBRSurface = true;

  // tydra-next has no dedicated coat_normal input, so the coat lobe reuses the
  // surface normal map. mesh_build.cc (legacy) already does this; without it the
  // --next path was the only loader leaving coatNormalTex unset.
  if (dm.coatNormalTex < 0 && dm.normalTex >= 0) {
    dm.coatNormalTex = dm.normalTex;
    dm.coatNormalSample = dm.normalSample;
  }

  // Make every descriptor self-contained: mirror the per-slot texture id into
  // its sample so consumers can read one struct instead of pairing a sample
  // with the right parallel `*Tex` field.
  auto syncSampleTex = [](DrawTexSampleCPU* smp, int tex) { smp->tex = tex; };
  syncSampleTex(&dm.baseColorSample, dm.baseColorTex);
  syncSampleTex(&dm.metallicSample, dm.metallicTex);
  syncSampleTex(&dm.roughnessSample, dm.roughnessTex);
  syncSampleTex(&dm.normalSample, dm.normalTex);
  syncSampleTex(&dm.coatNormalSample, dm.coatNormalTex);
  syncSampleTex(&dm.emissiveSample, dm.emissiveTex);
  syncSampleTex(&dm.opacitySample, dm.opacityTex);
  syncSampleTex(&dm.occlusionSample, dm.occlusionTex);
  syncSampleTex(&dm.specularColorSample, dm.specularColorTex);
  syncSampleTex(&dm.coatWeightSample, dm.coatWeightTex);
  syncSampleTex(&dm.coatColorSample, dm.coatColorTex);
  syncSampleTex(&dm.coatRoughnessSample, dm.coatRoughnessTex);
  syncSampleTex(&dm.displacementSample, dm.displacementTex);
  auto syncPtex = [&](DrawTexSampleCPU* sample, int tex) {
    if (!sample || tex < 0 || static_cast<size_t>(tex) >= draw->textures.size())
      return;
    const DrawTextureCPU& texture = draw->textures[static_cast<size_t>(tex)];
    sample->isPtex = texture.isPtex;
    sample->ptexAtlasCols = texture.ptexAtlasCols;
    sample->ptexAtlasRows = texture.ptexAtlasRows;
    sample->ptexTileEdge = texture.ptexTileEdge;
    sample->ptexRectTexelOffset = texture.ptexRectTexelOffset;
    sample->ptexFaceCount =
        static_cast<uint32_t>(texture.ptexFaceRects.size());
  };
  syncPtex(&dm.baseColorSample, dm.baseColorTex);
  syncPtex(&dm.metallicSample, dm.metallicTex);
  syncPtex(&dm.roughnessSample, dm.roughnessTex);
  syncPtex(&dm.normalSample, dm.normalTex);
  syncPtex(&dm.emissiveSample, dm.emissiveTex);
  syncPtex(&dm.opacitySample, dm.opacityTex);
  syncPtex(&dm.occlusionSample, dm.occlusionTex);
  syncPtex(&dm.specularColorSample, dm.specularColorTex);
  syncPtex(&dm.coatWeightSample, dm.coatWeightTex);
  syncPtex(&dm.coatColorSample, dm.coatColorTex);
  syncPtex(&dm.coatRoughnessSample, dm.coatRoughnessTex);
  syncPtex(&dm.displacementSample, dm.displacementTex);
  // Scalar slots carry their channel on the sample too.
  if (dm.opacitySample.channel < 0) dm.opacitySample.channel = dm.opacityChannel;
  if (dm.occlusionSample.channel < 0)
    dm.occlusionSample.channel = dm.occlusionChannel;
  if (dm.metallicSample.channel < 0) dm.metallicSample.channel = dm.metallicChannel;
  if (dm.roughnessSample.channel < 0)
    dm.roughnessSample.channel = dm.roughnessChannel;

  // AlphaMode enums line up 1:1 (Opaque=0, Mask=1, Blend=2).
  dm.alphaMode = static_cast<int>(rm.alpha_mode);
  dm.alphaCutoff = rm.alpha_cutoff;
  if (jpegOpacityCoverage &&
      dm.alphaMode == static_cast<int>(AlphaMode::Blend)) {
    dm.alphaMode = static_cast<int>(AlphaMode::Mask);
    dm.alphaCutoff = 0.5f;
    dm.alphaMaskHeuristic = true;
  }

  BakeRealtimePbrMaterial(&dm);  // derive shared PBR constants from the adapter

  // BakeLightRtOpenPBR's generic path consumes DrawMaterialParamCPU, while the
  // default next loader owns the already-typed RenderMaterial above. Rebuild
  // the shared block directly from that authoritative carrier so coat and the
  // other OpenPBR constants do not fall back to defaults merely because this
  // adapter intentionally avoids duplicating the neutral parameter vector.
  const tydn::RenderMaterial::ShaderType originalShaderType = rm.shader_type;
  rm.shader_type = usePreview
                       ? tydn::RenderMaterial::ShaderType::PreviewSurface
                       : tydn::RenderMaterial::ShaderType::OpenPBR;
  lightusd::tydra::RealtimePbrMaterial pbr;
  if (tydn::BuildRealtimePbrMaterial(rm, &pbr)) {
    // BuildRealtimePbrMaterial preserves authored constants from the typed
    // next-core material. Once a live texture slot is resolved those constants
    // are multiplicative fallbacks, so make them neutral before publishing the
    // canonical RT block. Without this, Vulkan ray query/CUDA use e.g.
    // metallic=0 or emission=(0,0,0) from the USD fallback and erase the
    // sampled texture even though raster uses the correct DrawMaterialCPU lane.
    if (dm.baseColorTex >= 0) {
      pbr.baseColor[0] = pbr.baseColor[1] = pbr.baseColor[2] = 1.0f;
    }
    if (dm.metallicTex >= 0) pbr.metalness = 1.0f;
    if (dm.roughnessTex >= 0) pbr.specularRoughness = 1.0f;
    if (dm.emissiveTex >= 0) {
      pbr.emissionColor[0] = pbr.emissionColor[1] = pbr.emissionColor[2] =
          1.0f;
      pbr.emission = 1.0f;
    }
    if (dm.opacityTex >= 0) pbr.opacity = 1.0f;
    if (dm.specularColorTex >= 0) {
      pbr.specularColor[0] = pbr.specularColor[1] = pbr.specularColor[2] =
          1.0f;
    }
    if (dm.coatWeightTex >= 0) pbr.coatWeight = 1.0f;
    if (dm.coatColorTex >= 0) {
      pbr.coatColor[0] = pbr.coatColor[1] = pbr.coatColor[2] = 1.0f;
    }
    if (dm.coatRoughnessTex >= 0) pbr.coatRoughness = 1.0f;
    dm.lightRtOpenPBR = pbr;
    dm.hasLightRtOpenPBR = true;
    // BakeRealtimePbrMaterial above serves the legacy DrawMaterialParamCPU
    // adapter. The next loader intentionally avoids that lossy parameter-vector
    // reconstruction, so its empty parameter list would otherwise bake the
    // OpenPBR defaults (0.8 gray) back over the typed next-core constants. Keep
    // the DrawMaterialCPU fallback in lockstep with the canonical Tydra record.
    if (dm.baseColorTex < 0) {
      dm.baseColor[0] = pbr.baseColor[0];
      dm.baseColor[1] = pbr.baseColor[1];
      dm.baseColor[2] = pbr.baseColor[2];
    }
    if (dm.metallicTex < 0) dm.metallic = pbr.metalness;
    if (dm.roughnessTex < 0) dm.roughness = pbr.specularRoughness;
    dm.ior = pbr.specularIor;
    if (dm.specularColorTex < 0) {
      dm.specularColor[0] = pbr.specularColor[0];
      dm.specularColor[1] = pbr.specularColor[1];
      dm.specularColor[2] = pbr.specularColor[2];
    }
    if (dm.emissiveTex < 0) {
      dm.emissive[0] = pbr.emissionColor[0] * pbr.emission;
      dm.emissive[1] = pbr.emissionColor[1] * pbr.emission;
      dm.emissive[2] = pbr.emissionColor[2] * pbr.emission;
    }
    dm.alpha = pbr.opacity;
    dm.coatWeight = pbr.coatWeight;
    dm.coatColor[0] = pbr.coatColor[0];
    dm.coatColor[1] = pbr.coatColor[1];
    dm.coatColor[2] = pbr.coatColor[2];
    dm.coatRoughness = pbr.coatRoughness;
    dm.coatIor = pbr.coatIor;
    // This rebuild restores the coat CONSTANTS, which would undo the
    // neutralization the coat texture slots applied above and double-tint the
    // sampled value. Re-neutralize any slot that resolved to a texture.
    if (dm.coatWeightTex >= 0) dm.coatWeight = 1.0f;
    if (dm.coatRoughnessTex >= 0) dm.coatRoughness = 1.0f;
    if (dm.coatColorTex >= 0)
      dm.coatColor[0] = dm.coatColor[1] = dm.coatColor[2] = 1.0f;
  }
  // Keep the neutral parameter carrier complete even when the real-time
  // evaluator degrades an unsupported lobe. A RenderTexture id alone is not
  // enough for viewer-side consumers: map it to DrawScene, and preserve its
  // selected channel plus UV/value sampling descriptor.
  auto retainParam = [&](const std::string& shader, const std::string& name,
                         DrawMaterialParamType type,
                         const tydn::ShaderParam& value) {
    DrawMaterialParamCPU param;
    param.shader = shader;
    param.name = name;
    param.type = type;
    param.value[0] = value.value.x;
    param.value[1] = value.value.y;
    param.value[2] = value.value.z;
    param.value[3] = value.value.w;
    param.renderTexture = value.texture_id;
    if (value.texture_id >= 0 &&
        static_cast<size_t>(value.texture_id) < scratch.textures.size()) {
      const tydn::RenderTexture& rt =
          scratch.textures[static_cast<size_t>(value.texture_id)];
      const bool srgb = rt.source_color_space == "sRGB" ||
                        rt.source_color_space == "srgb";
      param.texture =
          LoadNextTexture(texCache, draw, scratch, value.texture_id, srgb);
      FillNextSample(rt, &param.sample, uv0Name, uv1Name);
      param.sample.tex = param.texture;
      param.channel = param.sample.channel;
      if (type == DrawMaterialParamType::Float && param.channel < 0) {
        param.channel = NextScalarChannel(rt.output_channel);
        param.sample.channel = param.channel;
      }
    }
    dm.params.push_back(std::move(param));
  };
  if (!usePreview && rm.openpbr) {
    auto retainDiagnosticScalar = [&](const char* name,
                                      const tydn::ShaderParam& value) {
      retainParam("OpenPBRSurface", name, DrawMaterialParamType::Float, value);
    };
    const tydn::OpenPBRSurfaceShader& s = *rm.openpbr;
    retainDiagnosticScalar("specular_anisotropy", s.specular_anisotropy);
    retainDiagnosticScalar("specular_roughness_anisotropy",
                           s.specular_roughness_anisotropy);
    retainDiagnosticScalar("coat_anisotropy", s.coat_anisotropy);
    retainDiagnosticScalar("coat_roughness_anisotropy",
                           s.coat_roughness_anisotropy);
    retainDiagnosticScalar("transmission_dispersion",
                           s.transmission_dispersion);
    retainDiagnosticScalar("transmission_dispersion_scale",
                           s.transmission_dispersion_scale);
  }
  for (const tydn::RetainedMaterialParam& retained : rm.retained_params) {
    retainParam(retained.shader, retained.name, DrawMaterialParamType::Vec4,
                retained.value);
  }
  rm.shader_type = originalShaderType;
  if (usePreview) {
    dm.occlusion = rm.preview_surface->occlusion.value.x;
  }

  BakeMaterialXGraphTextures(&dm, draw);
  DiagnoseUnsupportedRealtimeLobes(dm, draw);
  if (std::getenv("LUSDVIEW_DEBUG_MATERIALS")) {
    LOGI("next material '%s': shader=%s alpha=%.4f mode=%d opacityTex=%d "
         "ior=%.4f transmission=%.4f baseTex=%d",
         rm.prim_path.c_str(), usePreview ? "preview" : "openpbr", dm.alpha,
         dm.alphaMode, dm.opacityTex, dm.ior,
         dm.hasLightRtOpenPBR ? dm.lightRtOpenPBR.transmission : 0.0f,
         dm.baseColorTex);
  }
  draw->materials.push_back(std::move(dm));
  return static_cast<int>(draw->materials.size() - 1);
}

}  // namespace

bool DecodeDeferredDrawTexture(const DrawTextureCPU& placeholder,
                               const TextureRuntimeOptions& runtime,
                               uint64_t budgetBytes,
                               DrawTextureCPU* decoded) {
  return DecodeDeferredDrawTextureImpl(placeholder, runtime, budgetBytes,
                                       decoded);
}

bool UpdateNextAnimatedMeshWorlds(const tnext::Stage& stage, DrawScene* draw,
                                  double time) {
  if (!draw) return false;
  bool changed = false;
  for (DrawMeshCPU& mesh : draw->meshes) {
    if (!mesh.animatedWorld || mesh.absPath.empty()) continue;
    const tnext::UsdPrim prim = stage.GetPrimAtPath(mesh.absPath);
    if (!prim.IsValid()) continue;
    double world[16];
    if (!tydn::ComputeWorldTransform(stage, prim, world, time)) continue;
    float next[16];
    for (int i = 0; i < 16; ++i) next[i] = static_cast<float>(world[i]);
    const bool matrixChanged = std::memcmp(mesh.world, next, sizeof(next)) != 0;
    if (matrixChanged) std::memcpy(mesh.world, next, sizeof(next));
    // Keep CPU-side culling/picking bounds aligned with the animated matrix.
    const float xs[2] = {mesh.restAabbMin[0], mesh.restAabbMax[0]};
    const float ys[2] = {mesh.restAabbMin[1], mesh.restAabbMax[1]};
    const float zs[2] = {mesh.restAabbMin[2], mesh.restAabbMax[2]};
    for (int k = 0; k < 3; ++k) {
      mesh.aabbMin[k] = std::numeric_limits<float>::max();
      mesh.aabbMax[k] = -std::numeric_limits<float>::max();
    }
    for (float x : xs) for (float y : ys) for (float z : zs) {
      const float p[3] = {x, y, z};
      for (int c = 0; c < 3; ++c) {
        const float v = p[0] * next[c] + p[1] * next[4 + c] +
                        p[2] * next[8 + c] + next[12 + c];
        mesh.aabbMin[c] = std::min(mesh.aabbMin[c], v);
        mesh.aabbMax[c] = std::max(mesh.aabbMax[c], v);
      }
    }
    changed = changed || matrixChanged;
  }
  return changed;
}

bool FindNextCamera(const tnext::Stage& stage, const std::string& name,
                    double time, NextCameraPose* out) {
  for (const tnext::UsdPrim& root : stage.GetRootPrims()) {
    if (FindNextCameraRec(stage, root, name, time, out)) return true;
  }
  return false;
}

static DrawCameraCPU MakeDrawCameraFromNext(
    const lightusd::value::matrix4d& worldMatrix,
    float focalLength, float horizontalAperture, float verticalAperture,
    float horizontalApertureOffset, float verticalApertureOffset,
    float exposure, int projection,
    float zNear, float zFar);

static float BackPlateDepthAt(const light3d::Image* image, float u, float v,
                              const tnext::BackPlateData& plate,
                              float fallback) {
  if (!image || image->width <= 0 || image->height <= 0 ||
      image->channels <= 0 || image->data.empty()) return fallback;
  const int x = std::max(0, std::min(image->width - 1,
      static_cast<int>(u * static_cast<float>(image->width - 1) + 0.5f)));
  const int y = std::max(0, std::min(image->height - 1,
      static_cast<int>((1.0f - v) * static_cast<float>(image->height - 1) + 0.5f)));
  const size_t off = (static_cast<size_t>(y) * image->width + x) *
                     static_cast<size_t>(image->channels);
  if (off >= image->data.size()) return fallback;
  const float z = static_cast<float>(image->data[off]) / 255.0f;
  const float depth = z * plate.depth_normalizing_factor +
                      plate.depth_min_offset + plate.depth_camera_space_offset;
  return std::isfinite(depth) && depth > 0.0f ? depth : fallback;
}

// BackPlateAPI is camera-bound rather than scene geometry. Represent it as a
// camera-space, depth-tested textured grid in the shared DrawScene so GL and
// Vulkan rasterizers consume precisely the same multiple-instance stack.
static void AddNextBackPlates(const tnext::Stage& stage,
                              const tnext::UsdPrim& prim,
                              const DrawCameraCPU& camera,
                              NextTexCache* textures, DrawScene* draw,
                              double time) {
  if (!textures || !draw) return;
  constexpr const char* prefix = "BackPlateAPI:";
  for (const std::string& schema : prim.GetMeta().apiSchemas()) {
    if (schema.rfind(prefix, 0) != 0) continue;
    tnext::BackPlateData plate;
    if (!tnext::GetBackPlateData(stage, prim, schema.substr(std::strlen(prefix)),
                                 &plate, time) || plate.image.empty() ||
        plate.plate_visibility == "invisible") continue;

    light3d::Image color;
    if (!DecodeNextImage(*textures, plate.image, true, &color)) continue;
    light3d::Image alpha;
    if (!plate.alpha_image.empty() &&
        DecodeNextImage(*textures, plate.alpha_image, false, &alpha) &&
        alpha.width == color.width && alpha.height == color.height) {
      const size_t pixels = static_cast<size_t>(color.width) * color.height;
      for (size_t i = 0; i < pixels; ++i) color.data[i * 4 + 3] = alpha.data[i * 4];
    }
    const size_t pixels = static_cast<size_t>(color.width) * color.height;
    for (size_t i = 0; i < pixels; ++i) {
      for (int c = 0; c < 3; ++c) {
        float x = static_cast<float>(color.data[i * 4 + c]) / 255.0f;
        x = x * plate.luma_gain[c] + plate.luma_lift[c];
        const float gamma = std::max(1.0e-6f, plate.luma_gamma[c]);
        x = std::pow(std::max(0.0f, x), 1.0f / gamma);
        color.data[i * 4 + c] = static_cast<uint8_t>(
            std::lround(std::max(0.0f, std::min(1.0f, x)) * 255.0f));
      }
    }
    DrawTextureCPU texture;
    texture.assetIdentifier = plate.image;
    texture.image = std::move(color);
    texture.srgb = true;
    texture.wrapS = static_cast<int>(WrapMode::ClampToEdge);
    texture.wrapT = static_cast<int>(WrapMode::ClampToEdge);
    const int textureId = static_cast<int>(draw->textures.size());
    draw->textures.push_back(std::move(texture));

    DrawMaterialCPU material;
    material.name = "BackPlateAPI:" + schema.substr(std::strlen(prefix));
    material.absPath = prim.GetPath().str() + "." + material.name;
    material.hasUsdPreviewSurface = true;
    material.baseColor[0] = material.baseColor[1] = material.baseColor[2] = 1.0f;
    material.roughness = 1.0f;
    // Raster BackPlates must participate in the ordinary depth test. The GL
    // renderer intentionally disables depth for general Blend materials, so use
    // a near-zero alpha mask here: transparent plate texels are discarded while
    // visible texels retain correct plate/geometry depth ordering on both GL/VK.
    material.alphaMode = static_cast<int>(AlphaMode::Mask);
    material.alphaCutoff = 1.0f / 255.0f;
    material.baseColorTex = textureId;
    material.baseColorSample.tex = textureId;
    material.baseColorSample.wrapS = WrapMode::ClampToEdge;
    material.baseColorSample.wrapT = WrapMode::ClampToEdge;
    material.baseColorSample.colorSpace = DrawColorSpace::sRGB;
    const int materialId = static_cast<int>(draw->materials.size());
    draw->materials.push_back(std::move(material));

    light3d::Image depth;
    const bool hasDepth = !plate.depth_image.empty() &&
                          DecodeNextImage(*textures, plate.depth_image, false,
                                          &depth);
    constexpr int cells = 16;
    const int side = hasDepth ? cells + 1 : 2;
    DrawMeshCPU mesh;
    mesh.name = material.name;
    mesh.absPath = prim.GetPath().str() + "/__" + material.name;
    mesh.doubleSided = true;
    for (int i = 0; i < 16; ++i) mesh.world[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    float right[3] = {
        camera.forward[1] * camera.up[2] - camera.forward[2] * camera.up[1],
        camera.forward[2] * camera.up[0] - camera.forward[0] * camera.up[2],
        camera.forward[0] * camera.up[1] - camera.forward[1] * camera.up[0]};
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                               right[2] * right[2]);
    if (rl > 1.0e-8f) for (float& x : right) x /= rl;
    const float fallbackDepth = std::max(camera.zNear * 2.0f,
                                         camera.zFar * 0.98f);
    const float aspect = camera.verticalAperture > 1.0e-6f
                             ? camera.horizontalAperture / camera.verticalAperture
                             : 1.5f;
    const float rot = plate.rotate_xyz_tweak[2] * 0.01745329251994329577f;
    const float cs = std::cos(rot), sn = std::sin(rot);
    for (int y = 0; y < side; ++y) {
      for (int x = 0; x < side; ++x) {
        const float u = static_cast<float>(x) / static_cast<float>(side - 1);
        const float v = static_cast<float>(y) / static_cast<float>(side - 1);
        const float sx = (u * 2.0f - 1.0f) * plate.scale_tweak[0];
        const float sy = (v * 2.0f - 1.0f) * plate.scale_tweak[1];
        const float rx = cs * sx - sn * sy + plate.translate_tweak[0];
        const float ry = sn * sx + cs * sy + plate.translate_tweak[1];
        const float distance = std::max(
            camera.zNear * 1.001f,
            BackPlateDepthAt(hasDepth ? &depth : nullptr, u, v, plate,
                             fallbackDepth));
        const float halfH = camera.projection == DrawCameraCPU::Projection::Perspective
                                ? distance * std::tan(camera.fovYDeg *
                                                     0.008726646259971648f)
                                : camera.verticalAperture * 0.5f;
        const float halfW = halfH * aspect;
        DrawVertex vertex{};
        float* position = &vertex.px;
        float* normal = &vertex.nx;
        for (int c = 0; c < 3; ++c) {
          position[c] = camera.eye[c] + camera.forward[c] * distance +
                        right[c] * rx * halfW + camera.up[c] * ry * halfH;
          normal[c] = -camera.forward[c];
        }
        vertex.u = u;
        vertex.v = 1.0f - v;
        mesh.vertices.push_back(vertex);
      }
    }
    for (int y = 0; y + 1 < side; ++y) for (int x = 0; x + 1 < side; ++x) {
      const uint32_t a = static_cast<uint32_t>(y * side + x);
      const uint32_t b = a + 1;
      const uint32_t c = a + static_cast<uint32_t>(side);
      const uint32_t d = c + 1;
      mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
    }
    mesh.submeshes.push_back(
        DrawSubmesh{0, static_cast<uint32_t>(mesh.indices.size()), materialId,
                    materialId});
    draw->meshes.push_back(std::move(mesh));
  }
}

static void GatherNextCamerasRec(const tnext::Stage& stage,
                                  const tnext::UsdPrim& prim, double time,
                                  const std::string& selectedCamera,
                                  NextTexCache* textures, DrawScene* draw,
                                  std::vector<DrawCameraCPU>* out) {
  if (prim.GetTypeName() == "Camera") {
    double mw[16];
    matrix4d worldMatrix = matrix4d::identity();
    if (tydn::ComputeWorldTransform(stage, prim, mw, time)) {
      worldMatrix = Mat4dFromArray(mw);
    }

    const float focal = ReadCamFloatN(prim, "focalLength", 50.0f);
    const float ha = ReadCamFloatN(prim, "horizontalAperture", 20.955f);
    const float va = ReadCamFloatN(prim, "verticalAperture", 15.2908f);
    const float hao = ReadCamFloatN(prim, "horizontalApertureOffset", 0.0f);
    const float vao = ReadCamFloatN(prim, "verticalApertureOffset", 0.0f);
    const float expo = ReadCamFloatN(prim, "exposure", 0.0f);

    int proj = 0;
    if (const tnext::Value* v = prim.GetPropertyValue("projection")) {
      if (const std::string* token = v->as_token()) {
        proj = (*token == "orthographic") ? 1 : 0;
      }
    }

    float zn = 0.1f, zf = 10000.0f;
    if (const tnext::Value* v = prim.GetPropertyValue("clippingRange")) {
      if (const float* f = v->as_float2()) {
        zn = f[0]; zf = f[1];
      }
    }

    DrawCameraCPU dc = MakeDrawCameraFromNext(
        worldMatrix, focal, ha, va, hao, vao, expo, proj, zn, zf);
    dc.focusDistance = ReadCamFloatN(prim, "focusDistance", 0.0f);
    dc.fStop = ReadCamFloatN(prim, "fStop", 0.0f);
    dc.shutterOpen = ReadCamDoubleN(prim, "shutter:open", 0.0);
    dc.shutterClose = ReadCamDoubleN(prim, "shutter:close", 0.0);
    dc.stereoRole = ReadStereoRoleN(prim);
    dc.clippingPlanes = ReadClippingPlanesN(prim);
    dc.name = prim.GetName();
    dc.absPath = prim.GetPath().str();
    dc.displayName = dc.name;
    const bool selected = !selectedCamera.empty() &&
        (selectedCamera == dc.name || selectedCamera == dc.absPath ||
         (dc.absPath.size() > selectedCamera.size() &&
          dc.absPath.compare(dc.absPath.size() - selectedCamera.size(),
                             selectedCamera.size(), selectedCamera) == 0 &&
          dc.absPath[dc.absPath.size() - selectedCamera.size() - 1] == '/'));
    if (selected) AddNextBackPlates(stage, prim, dc, textures, draw, time);
    out->push_back(std::move(dc));
    // Camera prims are leaf nodes (no meaningful children to iterate).
    return;
  }
  for (const tnext::UsdPrim& child : prim.GetChildren()) {
    GatherNextCamerasRec(stage, child, time, selectedCamera, textures, draw, out);
  }
}

static DrawCameraCPU MakeDrawCameraFromNext(
    const lightusd::value::matrix4d& worldMatrix,
    float focalLength, float horizontalAperture, float verticalAperture,
    float horizontalApertureOffset, float verticalApertureOffset,
    float exposure, int projection,
    float zNear, float zFar) {
  DrawCameraCPU dc;
  float up[3] = {float(worldMatrix.m[1][0]), float(worldMatrix.m[1][1]),
                 float(worldMatrix.m[1][2])};
  float fwd[3] = {-float(worldMatrix.m[2][0]), -float(worldMatrix.m[2][1]),
                  -float(worldMatrix.m[2][2])};
  auto norm3 = [](float v[3]) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-12f) { v[0]/=l; v[1]/=l; v[2]/=l; }
  };
  norm3(up); norm3(fwd);
  for (int k = 0; k < 3; ++k) {
    dc.eye[k] = float(worldMatrix.m[3][k]);
    dc.up[k] = up[k];
    dc.forward[k] = fwd[k];
  }
  dc.focalLength = focalLength;
  dc.horizontalAperture = horizontalAperture;
  dc.verticalAperture = verticalAperture;
  dc.horizontalApertureOffset = horizontalApertureOffset;
  dc.verticalApertureOffset = verticalApertureOffset;
  dc.exposure = exposure;
  dc.projection = projection ? DrawCameraCPU::Projection::Orthographic
                             : DrawCameraCPU::Projection::Perspective;
  dc.zNear = std::max(1.0e-4f, zNear);
  dc.zFar = std::max(dc.zNear + 1.0e-3f, zFar);
  dc.fovYDeg = 2.0f * std::atan(0.5f * verticalAperture /
                  std::max(1.0e-6f, focalLength)) *
                  (180.0f / 3.14159265358979323846f);
  return dc;
}

void GatherNextCameras(const tnext::Stage& stage, double time,
                       const std::string& selectedCamera,
                       NextTexCache* textures, DrawScene* draw,
                       std::vector<DrawCameraCPU>* out) {
  if (!out) return;
  for (const tnext::UsdPrim& root : stage.GetRootPrims()) {
    GatherNextCamerasRec(stage, root, time, selectedCamera, textures, draw, out);
  }
}

void GatherNextCameras(const tnext::Stage& stage, double time,
                       std::vector<DrawCameraCPU>* out) {
  GatherNextCameras(stage, time, std::string(), nullptr, nullptr, out);
}

// The legacy loader has no next Stage, only the converted RenderScene -- so
// `--camera` was silently unavailable there. The camera's world matrix is already
// baked into its Node (Tydra composes the hierarchy), and its lens lives in the
// parallel RenderCamera the node's id indexes.
bool FindLegacyCameraRec(const lightusd::tydra::RenderScene& scene,
                         const lightusd::tydra::Node& node,
                         const std::string& name, NextCameraPose* out) {
  // scene.nodes is the ROOTS of a tree, not a flat list -- a camera is almost
  // always nested under an Xform (Blender writes /root/Camera/Camera), so a
  // top-level-only scan finds nothing.
  if (node.nodeType == lightusd::tydra::NodeType::Camera) {
    // Match by exact name, exact path, or a "/<name>" path suffix -- the same
    // three ways FindNextCameraRec matches, so one --camera argument means the
    // same thing to both loaders.
    const std::string& path = node.abs_path;
    const bool match =
        name.empty() || node.prim_name == name || path == name ||
        (path.size() > name.size() &&
         path.compare(path.size() - name.size(), name.size(), name) == 0 &&
         path[path.size() - name.size() - 1] == '/');
    if (!match) {
      for (const lightusd::tydra::Node& c : node.children)
        if (FindLegacyCameraRec(scene, c, name, out)) return true;
      return false;
    }

    // Row-major (p*M): translation in row 3, local axes in rows 0..2. USD cameras
    // look down local -Z with local +Y up.
    const lightusd::value::matrix4d& m = node.global_matrix;
    float up[3] = {float(m.m[1][0]), float(m.m[1][1]), float(m.m[1][2])};
    float fwd[3] = {-float(m.m[2][0]), -float(m.m[2][1]), -float(m.m[2][2])};
    auto norm3 = [](float v[3]) {
      const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    };
    norm3(up);
    norm3(fwd);
    for (int k = 0; k < 3; ++k) {
      out->eye[k] = float(m.m[3][k]);
      out->up[k] = up[k];
      out->forward[k] = fwd[k];
    }
    out->fovYDeg = 60.0f;
    if (node.id >= 0 && size_t(node.id) < scene.cameras.size()) {
      const lightusd::tydra::RenderCamera& cam = scene.cameras[size_t(node.id)];
      out->fovYDeg = 2.0f *
                     std::atan(0.5f * cam.verticalAperture /
                               std::max(1.0e-6f, cam.focalLength)) *
                     (180.0f / 3.14159265358979323846f);
      out->zNear = std::max(1.0e-4f, cam.znear);
      out->zFar = std::max(out->zNear + 1.0e-3f, cam.zfar);
      out->projection =
          cam.projection == lightusd::GeomCamera::Projection::Orthographic
              ? CameraProjection::Orthographic
              : CameraProjection::Perspective;
      out->horizontalAperture = cam.horizontalAperture;
      out->focalLength = cam.focalLength;
      out->verticalAperture = cam.verticalAperture;
      out->horizontalApertureOffset = cam.horizontalApertureOffset;
      out->verticalApertureOffset = cam.verticalApertureOffset;
      out->exposure = cam.exposure;
      out->focusDistance = cam.focusDistance;
      out->fStop = cam.fStop;
      out->shutterOpen = cam.shutterOpen;
      out->shutterClose = cam.shutterClose;
      switch (cam.stereoRole) {
        case lightusd::GeomCamera::StereoRole::Left:
          out->stereoRole = DrawCameraCPU::StereoRole::Left;
          break;
        case lightusd::GeomCamera::StereoRole::Right:
          out->stereoRole = DrawCameraCPU::StereoRole::Right;
          break;
        default:
          out->stereoRole = DrawCameraCPU::StereoRole::Mono;
          break;
      }
      out->clippingPlanes.clear();
      out->clippingPlanes.reserve(cam.clippingPlanes.size() * 4);
      for (const lightusd::value::float4& plane : cam.clippingPlanes) {
        for (size_t i = 0; i < 4; ++i) out->clippingPlanes.push_back(plane[i]);
      }
    }
    return true;
  }
  for (const lightusd::tydra::Node& c : node.children)
    if (FindLegacyCameraRec(scene, c, name, out)) return true;
  return false;
}

bool FindLegacyCamera(const lightusd::tydra::RenderScene& scene,
                      const std::string& name, NextCameraPose* out) {
  if (!out) return false;
  for (const lightusd::tydra::Node& root : scene.nodes)
    if (FindLegacyCameraRec(scene, root, name, out)) return true;
  return false;
}

// The scene's absolute bone rows at `time`, indexed exactly as DrawMeshCPU::
// jointIdx indexes them. Shared by the raster bone-texture upload
// (BuildNextSkinningFrame) and the RT vertex re-pose (BuildNextRtDeformedVertices),
// which must agree row-for-row or the two backends pose differently.
namespace {

bool FindLegacyCameraAtTimeRec(const lightusd::tydra::XformNode& node,
                               const std::string& name, double time,
                               NextCameraPose* out) {
  const lightusd::GeomCamera* camera =
      node.prim ? node.prim->as<lightusd::GeomCamera>() : nullptr;
  if (camera) {
    const std::string path = node.absolute_path.full_path_name();
    const bool match =
        name.empty() || node.element_name == name || path == name ||
        (path.size() > name.size() &&
         path.compare(path.size() - name.size(), name.size(), name) == 0 &&
         path[path.size() - name.size() - 1] == '/');
    if (match) {
      const lightusd::value::matrix4d& matrix = node.get_world_matrix();
      float up[3] = {float(matrix.m[1][0]), float(matrix.m[1][1]),
                     float(matrix.m[1][2])};
      float forward[3] = {-float(matrix.m[2][0]), -float(matrix.m[2][1]),
                          -float(matrix.m[2][2])};
      auto normalize = [](float v[3]) {
        const float length =
            std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 1.0e-12f) {
          v[0] /= length;
          v[1] /= length;
          v[2] /= length;
        }
      };
      normalize(up);
      normalize(forward);
      for (int k = 0; k < 3; ++k) {
        out->eye[k] = float(matrix.m[3][k]);
        out->up[k] = up[k];
        out->forward[k] = forward[k];
      }

      float focalLength = 50.0f;
      float horizontalAperture = 20.965f;
      float verticalAperture = 15.2908f;
      float horizontalApertureOffset = 0.0f;
      float verticalApertureOffset = 0.0f;
      float exposure = 0.0f;
      float focusDistance = 5.0f;
      float fStop = 0.0f;
      lightusd::value::float2 clippingRange({0.1f, 1000000.0f});
      lightusd::GeomCamera::Projection projection =
          lightusd::GeomCamera::Projection::Perspective;
      double shutterOpen = 0.0;
      double shutterClose = 0.0;
      camera->focalLength.get_value().get(time, &focalLength);
      camera->horizontalAperture.get_value().get(time, &horizontalAperture);
      camera->verticalAperture.get_value().get(time, &verticalAperture);
      camera->horizontalApertureOffset.get_value().get(
          time, &horizontalApertureOffset);
      camera->verticalApertureOffset.get_value().get(
          time, &verticalApertureOffset);
      camera->exposure.get_value().get(time, &exposure);
      camera->focusDistance.get_value().get(time, &focusDistance);
      camera->fStop.get_value().get(time, &fStop);
      camera->clippingRange.get_value().get(time, &clippingRange);
      camera->projection.get_value().get(time, &projection);
      camera->shutterOpen.get_value().get(time, &shutterOpen);
      camera->shutterClose.get_value().get(time, &shutterClose);

      out->focalLength = focalLength;
      out->horizontalAperture = horizontalAperture;
      out->verticalAperture = verticalAperture;
      out->horizontalApertureOffset = horizontalApertureOffset;
      out->verticalApertureOffset = verticalApertureOffset;
      out->exposure = exposure;
      out->focusDistance = focusDistance;
      out->fStop = fStop;
      out->projection =
          projection == lightusd::GeomCamera::Projection::Orthographic
              ? CameraProjection::Orthographic
              : CameraProjection::Perspective;
      out->zNear = std::max(1.0e-4f, clippingRange[0]);
      out->zFar = std::max(out->zNear + 1.0e-3f, clippingRange[1]);
      out->fovYDeg =
          2.0f * std::atan(0.5f * verticalAperture /
                           std::max(1.0e-6f, focalLength)) *
          (180.0f / 3.14159265358979323846f);
      out->shutterOpen = shutterOpen;
      out->shutterClose = shutterClose;
      switch (camera->stereoRole.get_value()) {
        case lightusd::GeomCamera::StereoRole::Left:
          out->stereoRole = DrawCameraCPU::StereoRole::Left;
          break;
        case lightusd::GeomCamera::StereoRole::Right:
          out->stereoRole = DrawCameraCPU::StereoRole::Right;
          break;
        default:
          out->stereoRole = DrawCameraCPU::StereoRole::Mono;
          break;
      }
      out->clippingPlanes.clear();
      std::vector<lightusd::value::float4> clippingPlanes;
      const auto clippingValue = camera->clippingPlanes.get_value();
      if (clippingValue && clippingValue->get(time, &clippingPlanes)) {
        out->clippingPlanes.reserve(clippingPlanes.size() * 4);
        for (const lightusd::value::float4& plane : clippingPlanes) {
          for (size_t i = 0; i < 4; ++i) {
            out->clippingPlanes.push_back(plane[i]);
          }
        }
      }
      return true;
    }
  }
  for (const lightusd::tydra::XformNode& child : node.children) {
    if (FindLegacyCameraAtTimeRec(child, name, time, out)) return true;
  }
  return false;
}

}  // namespace

bool FindLegacyCameraAtTime(const lightusd::Stage& stage,
                            const std::string& name, double time,
                            NextCameraPose* out) {
  if (!out) return false;
  lightusd::tydra::XformNode root;
  if (!lightusd::tydra::BuildXformNodeFromStage(stage, &root, time)) {
    return false;
  }
  for (const lightusd::tydra::XformNode& child : root.children) {
    if (FindLegacyCameraAtTimeRec(child, name, time, out)) return true;
  }
  return false;
}

static bool ComputeNextBoneRows(const tnext::Stage& stage, const DrawScene& draw,
                                double time, std::vector<matrix4d>* out) {
  if (!out || draw.boneMatrixCount <= 0 || draw.nextSkels.empty()) return false;

  // One row per (skinned source mesh, joint), addressed absolutely by
  // DrawMeshCPU::jointIdx. Identity is the safe default: it renders the vertex
  // at its world-baked REST position, so a skeleton that stops resolving degrades
  // to the rest pose instead of collapsing the mesh to the origin.
  const size_t rows = static_cast<size_t>(draw.boneMatrixCount);
  std::vector<matrix4d> bones(rows, matrix4d::identity());
  // Skeletons are shared between meshes far more often than not; pose each once.
  std::map<std::pair<std::string, std::string>, std::vector<matrix4d>> posed;

  for (const DrawScene::NextSkelBinding& nb : draw.nextSkels) {
    const auto key = std::make_pair(nb.skelPath, nb.animPath);
    auto it = posed.find(key);
    if (it == posed.end()) {
      std::vector<matrix4d> sm;
      if (!PoseNextSkeleton(stage, nb.skelPath, nb.animPath, time, &sm)) continue;
      it = posed.emplace(key, std::move(sm)).first;
    }
    const std::vector<matrix4d>& sm = it->second;
    const matrix4d G = Mat4dFromArray(nb.geomBind);
    const matrix4d W = Mat4dFromArray(nb.world);
    const matrix4d R = Mat4dFromArray(nb.renderWorld);
    const matrix4d S = Mat4dFromArray(nb.skeletonWorld);
    const matrix4d invW = ::lightusd::inverse(W);
    const matrix4d invR = ::lightusd::inverse(R);
    const size_t nj =
        std::min(sm.size(), static_cast<size_t>(std::max(0, nb.numJoints)));
    for (size_t j = 0; j < nj; ++j) {
      const size_t row = static_cast<size_t>(nb.matrixBase) + j;
      if (row >= rows) break;
      // Vertices are world-baked by W. Undo that bake, enter skeleton bind
      // space through G, pose the joint, then place through the Skeleton prim.
      bones[row] = invW * G * sm[j] * S * invR;
    }
  }

  *out = std::move(bones);
  return true;
}

bool BuildNextSkinningFrame(const tnext::Stage& stage, DrawScene* draw,
                            double time, SkinningFrameCPU* frame) {
  if (!draw || !frame) return false;
  std::vector<matrix4d> bones;
  if (!ComputeNextBoneRows(stage, *draw, time, &bones)) return false;
  const size_t rows = bones.size();

  // Pack straight from `bones` rather than walking draw->meshes (as the Tydra
  // path does): the next loader frees each mesh's CPU geometry after GPU upload,
  // so the per-vertex skin attributes are no longer resident here -- only the GL
  // buffers hold them.
  frame->matrixCount = draw->boneMatrixCount;
  frame->rgba32f.assign(rows * 16, 0.0f);
  frame->enabled = true;
  for (size_t row = 0; row < rows; ++row) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        frame->rgba32f[row * 16 + static_cast<size_t>(r) * 4 +
                       static_cast<size_t>(c)] =
            static_cast<float>(bones[row].m[r][c]);
      }
    }
  }
  return true;
}

bool BuildNextPosedSceneBounds(
    const tnext::Stage& stage, DrawScene* draw, double time,
    const std::unordered_map<std::string, float>* blendOverride,
    float outMin[3], float outMax[3]) {
  // The box is taken from the POSED VERTICES, and from the same deform the ray
  // tracer uploads -- not from a cheaper conservative bound. It has to be the
  // tight box: the Tydra/CPU-bake path derives its box from posed vertices too,
  // and the scene box drives the ground grid, the depth normalization and the
  // auto-fit, so a looser box here would make the two paths render the same
  // geometry differently. (A bone-box union -- 8 rest corners through each bone
  // -- was tried first and is ~10% loose on a 60-degree bend, which was visible.)
  std::vector<RtSkinnedMeshUpload> posed;
  if (!draw ||
      !BuildNextRtDeformedVertices(stage, *draw, time, blendOverride, &posed)) {
    return false;
  }
  std::unordered_map<int, const std::vector<DrawVertex>*> posedByMesh;
  for (const RtSkinnedMeshUpload& up : posed)
    posedByMesh[up.meshIndex] = &up.vertices;

  bool has = false;
  float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
  auto grow = [&](const float p[3]) {
    for (int k = 0; k < 3; ++k) {
      if (!has) { mn[k] = mx[k] = p[k]; continue; }
      mn[k] = std::min(mn[k], p[k]);
      mx[k] = std::max(mx[k], p[k]);
    }
    has = true;
  };

  // Fallback for a deformable mesh whose CPU geometry WAS freed (so it has no
  // posed vertices here): push the 8 corners of its rest box through each bone it
  // references. A posed vertex is a convex combination of itself under its bones,
  // so the union of those boxes contains it -- conservative, but it keeps such a
  // mesh inside the scene box instead of dropping it out of the framing.
  std::vector<matrix4d> bones;
  bool bonesReady = false, bonesOk = false;

  for (DrawMeshCPU& m : draw->meshes) m.hasPosedPickAabb = false;
  for (size_t mi = 0; mi < draw->meshes.size(); ++mi) {
    DrawMeshCPU& m = draw->meshes[mi];
    auto pit = posedByMesh.find(static_cast<int>(mi));
    if (pit != posedByMesh.end()) {
      const size_t ninst = m.instanceCount();
      if (ninst > 0) {
        // An instanced prototype's posed vertices are prototype-LOCAL (that is the
        // whole point of the instanced path: the placement lives in the instance
        // matrix, not in the vertices). Each placement gets its own box.
        for (size_t k = 0; k < ninst; ++k) {
          const float* X = &m.instanceXforms[k * 12];  // 3 rows of (x,y,z,tx)
          for (const DrawVertex& v : *pit->second) {
            const float p[3] = {
                X[0] * v.px + X[1] * v.py + X[2] * v.pz + X[3],
                X[4] * v.px + X[5] * v.py + X[6] * v.pz + X[7],
                X[8] * v.px + X[9] * v.py + X[10] * v.pz + X[11]};
            grow(p);
          }
        }
      } else {
        size_t posedVertex = 0;
        for (const DrawVertex& v : *pit->second) {
          float p[3];
          if (m.animatedWorld) {
            const float wp[3] = {
                v.px * m.world[0] + v.py * m.world[4] + v.pz * m.world[8] + m.world[12],
                v.px * m.world[1] + v.py * m.world[5] + v.pz * m.world[9] + m.world[13],
                v.px * m.world[2] + v.py * m.world[6] + v.pz * m.world[10] + m.world[14]};
            std::copy(wp, wp + 3, p);
          } else {
            p[0] = v.px; p[1] = v.py; p[2] = v.pz;
          }
          for (int k = 0; k < 3; ++k) {
            if (posedVertex == 0)
              m.posedPickAabbMin[k] = m.posedPickAabbMax[k] = p[k];
            else {
              m.posedPickAabbMin[k] = std::min(m.posedPickAabbMin[k], p[k]);
              m.posedPickAabbMax[k] = std::max(m.posedPickAabbMax[k], p[k]);
            }
          }
          ++posedVertex;
          grow(p);
        }
        m.hasPosedPickAabb = posedVertex != 0;
      }
      continue;
    }
    if (m.boneLo >= 0 && m.boneHi >= m.boneLo && m.vertices.empty()) {
      if (!bonesReady) {
        bonesOk = ComputeNextBoneRows(stage, *draw, time, &bones);
        bonesReady = true;
      }
      const float rlo[3] = {m.restAabbMin[0] - m.morphExtent[0],
                            m.restAabbMin[1] - m.morphExtent[1],
                            m.restAabbMin[2] - m.morphExtent[2]};
      const float rhi[3] = {m.restAabbMax[0] + m.morphExtent[0],
                            m.restAabbMax[1] + m.morphExtent[1],
                            m.restAabbMax[2] + m.morphExtent[2]};
      if (bonesOk && rlo[0] <= rhi[0]) {
        for (int row = m.boneLo; row <= m.boneHi; ++row) {
          if (static_cast<size_t>(row) >= bones.size()) break;
          const matrix4d& B = bones[static_cast<size_t>(row)];
          for (int c = 0; c < 8; ++c) {
            const double p[3] = {(c & 1) ? rhi[0] : rlo[0],
                                 (c & 2) ? rhi[1] : rlo[1],
                                 (c & 4) ? rhi[2] : rlo[2]};
            float q[3];  // row-vector p*B, as the vertex shader applies it
            for (int k = 0; k < 3; ++k) {
              q[k] = static_cast<float>(p[0] * B.m[0][k] + p[1] * B.m[1][k] +
                                        p[2] * B.m[2][k] + B.m[3][k]);
            }
            grow(q);
          }
        }
        continue;
      }
    }
    // Everything else -- static meshes, instanced prototypes -- already carries a
    // correct world box (morphExtent included).
    if (m.aabbMax[0] >= m.aabbMin[0] && std::isfinite(m.aabbMin[0]) &&
        std::isfinite(m.aabbMax[0])) {
      grow(m.aabbMin);
      grow(m.aabbMax);
    }
  }
  if (!has) return false;
  for (int k = 0; k < 3; ++k) {
    outMin[k] = mn[k];
    outMax[k] = mx[k];
  }
  return true;
}

void BuildNextMorphWeights(
    const tnext::Stage& stage, const DrawScene& draw, double time,
    const std::unordered_map<std::string, float>* blendOverride,
    std::vector<std::pair<int, std::vector<float>>>* out) {
  out->clear();
  for (size_t mi = 0; mi < draw.meshes.size(); ++mi) {
    const DrawMeshCPU& dm = draw.meshes[mi];
    if (dm.morphChannelCount <= 0 || dm.morphTargetChannels.empty()) continue;

    // Animated weights from the mesh's bound SkelAnimation, then manual overrides.
    std::unordered_map<std::string, float> weights =
        ResolveBlendWeights(stage, stage.GetPrimAtPath(dm.absPath), time);
    if (blendOverride)
      for (const auto& kv : *blendOverride) weights[kv.first] = kv.second;

    // Per-channel coefficients (== EvalMorphChannelCoeffs): each target's weight
    // brackets two channels (rest index 0 contributes nothing) with (1-t) / t.
    std::vector<float> coeff(static_cast<size_t>(dm.morphChannelCount), 0.0f);
    for (const MorphTargetChannelsCPU& tc : dm.morphTargetChannels) {
      if (tc.usdWeights.empty()) continue;
      auto it = weights.find(tc.name);
      if (it == weights.end() || it->second == 0.0f) continue;
      const std::vector<float> ibW(tc.usdWeights.begin(), tc.usdWeights.end() - 1);
      const MorphBracket br = FindMorphBracket(ibW, it->second);
      if (br.lo >= 1 && size_t(br.lo - 1) < tc.channelIds.size())
        coeff[tc.channelIds[br.lo - 1]] += (1.0f - br.t);
      if (br.hi >= 1 && size_t(br.hi - 1) < tc.channelIds.size())
        coeff[tc.channelIds[br.hi - 1]] += br.t;
    }
    out->emplace_back(static_cast<int>(mi), std::move(coeff));
  }
}

// IEEE binary16 -> float32 (EXR half envmaps).
static float NextHalfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t man = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while (!(man & 0x400u)) {
        man <<= 1;
        --exp;
      }
      man &= 0x3FFu;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

bool BuildNextRtDeformedVertices(
    const tnext::Stage& stage, const DrawScene& draw, double time,
    const std::unordered_map<std::string, float>* blendOverride,
    std::vector<RtSkinnedMeshUpload>* out) {
  if (!out) return false;
  out->clear();

  const bool kPoseTiming = std::getenv("LUSDVIEW_RT_POSE_TIMING") != nullptr;
  auto tick = []() { return std::chrono::steady_clock::now(); };
  auto msf = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  auto t0 = tick();
  std::vector<matrix4d> bones;
  const bool hasSkin = ComputeNextBoneRows(stage, draw, time, &bones);
  auto t1 = tick();

  // Morph coefficients per morphed mesh -- the same evaluation the raster vertex
  // shader is fed, so RT and raster morph identically.
  std::vector<std::pair<int, std::vector<float>>> morphCoeffs;
  BuildNextMorphWeights(stage, draw, time, blendOverride, &morphCoeffs);
  std::unordered_map<int, const std::vector<float>*> coeffByMesh;
  for (const auto& mc : morphCoeffs) coeffByMesh[mc.first] = &mc.second;
  auto t2 = tick();
  if (kPoseTiming)
    std::fprintf(stderr, "[rt-pose] bone-rows %.2f morph-weights %.2f ms\n",
                 msf(t0, t1), msf(t1, t2));

  for (size_t mi = 0; mi < draw.meshes.size(); ++mi) {
    const DrawMeshCPU& m = draw.meshes[mi];
    const size_t nv = m.vertices.size();
    if (nv == 0) continue;  // CPU geometry freed: nothing to re-pose from
    const bool skinned = hasSkin && m.jointIdx.size() == nv * 4 &&
                         m.jointWt.size() == nv * 4;
    const bool extendedSkinned =
        skinned && m.influenceOffsetCount.size() == nv * 2 &&
        !m.influenceTexels.empty() && m.influenceTexels.size() % 4 == 0 &&
        m.maxInfluencesPerVertex > 4;
    auto ci = coeffByMesh.find(static_cast<int>(mi));
    const bool morphed = ci != coeffByMesh.end() &&
                         m.morphOffsetCount.size() == nv * 2 &&
                         !m.morphDeltaHalf.empty();
    if (!skinned && !morphed) continue;

    auto tA = tick();
    std::vector<DrawVertex> verts = m.vertices;  // rest pose
    auto tB = tick();
    const size_t entries = m.morphDeltaHalf.size() / 4;
    // Per-vertex independent (verts[v] is the only write), so range-split
    // threading is bit-identical to the serial loop.
    DeformParallelFor(nv, 16384, [&](size_t vBegin, size_t vEnd) {
    for (size_t v = vBegin; v < vEnd; ++v) {
      float p[3] = {verts[v].px, verts[v].py, verts[v].pz};
      const float rest_n[3] = {verts[v].nx, verts[v].ny, verts[v].nz};
      float n[3] = {rest_n[0], rest_n[1], rest_n[2]};

      // Blendshape morph, before skinning (deform.glsl's order). Each entry packs
      // 4 halfs: (channelId, dx, dy, dz).
      if (morphed) {
        const std::vector<float>& coeff = *ci->second;
        const size_t base = m.morphOffsetCount[v * 2 + 0];
        const size_t count = m.morphOffsetCount[v * 2 + 1];
        for (size_t k = 0; k < count && base + k < entries; ++k) {
          const uint16_t* e = &m.morphDeltaHalf[(base + k) * 4];
          const size_t chan = static_cast<size_t>(NextHalfToFloat(e[0]) + 0.5f);
          if (chan >= coeff.size()) continue;
          const float c = coeff[chan];
          if (std::fabs(c) < 1e-6f) continue;
          p[0] += c * NextHalfToFloat(e[1]);
          p[1] += c * NextHalfToFloat(e[2]);
          p[2] += c * NextHalfToFloat(e[3]);
        }
      }

      // Linear-blend skinning. The bone rows are absolute and already carry both
      // the mesh's geomBind and its world transform (the next loader world-bakes
      // its vertices), so this is the same row-vector p*M the vertex shader
      // applies -- the two must not drift apart.
      if (skinned) {
        double acc[3] = {0, 0, 0}, accn[3] = {0, 0, 0}, wsum = 0.0;
        if (extendedSkinned) {
          const uint32_t offset = m.influenceOffsetCount[v * 2 + 0];
          const uint32_t count = m.influenceOffsetCount[v * 2 + 1];
          const size_t texelCount = m.influenceTexels.size() / 4;
          for (uint32_t k = 0; k < count; ++k) {
            const size_t texel = static_cast<size_t>(offset) + k;
            if (texel >= texelCount) break;
            const size_t base = texel * 4;
            const float w = m.influenceTexels[base + 1];
            if (!(w > 0.0f)) continue;
            const uint32_t j = static_cast<uint32_t>(std::max(
                0.0f, m.influenceTexels[base] + 0.5f));
            if (j >= bones.size()) continue;
            const matrix4d& B = bones[j];
            for (int c = 0; c < 3; ++c) {
              acc[c] += double(w) * (double(p[0]) * B.m[0][c] +
                                     double(p[1]) * B.m[1][c] +
                                     double(p[2]) * B.m[2][c] + B.m[3][c]);
              accn[c] += double(w) * (double(rest_n[0]) * B.m[0][c] +
                                      double(rest_n[1]) * B.m[1][c] +
                                      double(rest_n[2]) * B.m[2][c]);
            }
            wsum += double(w);
          }
        } else {
          for (int k = 0; k < 4; ++k) {
            const float w = m.jointWt[v * 4 + static_cast<size_t>(k)];
            if (!(w > 0.0f)) continue;
            const uint32_t j = m.jointIdx[v * 4 + static_cast<size_t>(k)];
            if (j >= bones.size()) continue;
            const matrix4d& B = bones[j];
            for (int c = 0; c < 3; ++c) {
              acc[c] += double(w) * (double(p[0]) * B.m[0][c] +
                                     double(p[1]) * B.m[1][c] +
                                     double(p[2]) * B.m[2][c] + B.m[3][c]);
              accn[c] += double(w) * (double(rest_n[0]) * B.m[0][c] +
                                      double(rest_n[1]) * B.m[1][c] +
                                      double(rest_n[2]) * B.m[2][c]);
            }
            wsum += double(w);
          }
        }
        if (wsum > 0.0) {
          for (int c = 0; c < 3; ++c) p[c] = static_cast<float>(acc[c] / wsum);
          const double len = std::sqrt(accn[0] * accn[0] + accn[1] * accn[1] +
                                       accn[2] * accn[2]);
          if (len > 1e-12) {
            for (int c = 0; c < 3; ++c) n[c] = static_cast<float>(accn[c] / len);
          }
        }
      }

      verts[v].px = p[0]; verts[v].py = p[1]; verts[v].pz = p[2];
      verts[v].nx = n[0]; verts[v].ny = n[1]; verts[v].nz = n[2];
    }
    });
    if (kPoseTiming)
      std::fprintf(stderr, "[rt-pose] mesh %zu: copy %.2f deform %.2f ms\n",
                   mi, msf(tA, tB), msf(tB, tick()));

    RtSkinnedMeshUpload up;
    up.meshIndex = static_cast<int>(mi);
    up.vertices = std::move(verts);
    out->push_back(std::move(up));
  }
  return !out->empty();
}

// DomeLight support for the `next` path: walk the stage for DomeLight prims,
// decode the envmap to float RGB (8-bit treated as linear, matching the tydra
// dome loader), and bake the split-sum IBL so raster ambient / instanced
// ambient / RT miss backgrounds light up on the large-scene path too.
void BuildNextLights(const tnext::Stage& stage, tydn::RenderSceneConverter& conv,
                     const std::string& usdPath,
                     double time, const TextureRuntimeOptions& texOpts,
                     DrawScene* draw) {
  const std::string baseDir = lightusd::io::GetBaseDir(usdPath);
  tnext::AttributeEval lightEval(&stage);
  lightEval.SetTime(time);

  auto fillConverted = [&](const tnext::UsdPrim& p, DrawLightCPU* dst) {
    tydn::RenderLight src;
    if (!dst || !conv.ConvertLight(p, &src)) return false;
    dst->name = src.name;
    dst->absPath = src.prim_path;
    switch (src.type) {
      case tydn::LightType::Directional: dst->type = DrawLightCPU::Type::Distant; break;
      case tydn::LightType::Rect: dst->type = DrawLightCPU::Type::Rect; break;
      case tydn::LightType::Disk: dst->type = DrawLightCPU::Type::Disk; break;
      case tydn::LightType::Dome: dst->type = DrawLightCPU::Type::Dome; break;
      case tydn::LightType::Sphere: dst->type = DrawLightCPU::Type::Sphere; break;
      case tydn::LightType::Cylinder: dst->type = DrawLightCPU::Type::Cylinder; break;
      case tydn::LightType::Geometry: dst->type = DrawLightCPU::Type::Geometry; break;
      case tydn::LightType::Spot: dst->type = DrawLightCPU::Type::Sphere; break;
      case tydn::LightType::Point: default: dst->type = DrawLightCPU::Type::Point; break;
    }
    if (p.GetTypeName() == "PortalLight") dst->type = DrawLightCPU::Type::Portal;
    if (dst->type == DrawLightCPU::Type::Geometry) {
      const std::vector<tnext::Path>* targets =
          p.GetRelationship("inputs:geometry");
      if (!targets) targets = p.GetRelationship("geometry");
      if (targets && !targets->empty()) {
        dst->geometryTargetPath = targets->front().str();
      }
    }
    dst->color[0] = src.color.x; dst->color[1] = src.color.y;
    dst->color[2] = src.color.z;
    dst->intensity = src.intensity; dst->exposure = src.exposure;
    dst->normalize = src.normalize; dst->diffuse = src.diffuse;
    dst->specular = src.specular;
    dst->shapingConeAngle = src.shaping_cone_angle;
    dst->enableColorTemperature = src.enable_color_temperature;
    dst->colorTemperature = src.color_temperature;
    dst->shapingFocus = src.shaping_focus;
    dst->shapingFocusTint[0] = src.shaping_focus_tint.x;
    dst->shapingFocusTint[1] = src.shaping_focus_tint.y;
    dst->shapingFocusTint[2] = src.shaping_focus_tint.z;
    dst->shapingConeSoftness = src.shaping_cone_softness;
    dst->shapingIesFile = src.shaping_ies_file;
    dst->shapingIesAngleScale = src.shaping_ies_angle_scale;
    dst->shapingIesNormalize = src.shaping_ies_normalize;
    dst->shadowEnable = src.enable_shadow;
    dst->shadowColor[0] = src.shadow_color.x;
    dst->shadowColor[1] = src.shadow_color.y;
    dst->shadowColor[2] = src.shadow_color.z;
    dst->shadowDistance = src.shadow_distance;
    dst->shadowFalloff = src.shadow_falloff;
    dst->shadowFalloffGamma = src.shadow_falloff_gamma;
    switch (src.type) {
      case tydn::LightType::Directional: dst->angle = src.params.distant.angle; break;
      case tydn::LightType::Rect:
        dst->width = src.params.rect.width; dst->height = src.params.rect.height; break;
      case tydn::LightType::Disk: dst->radius = src.params.disk.radius; break;
      case tydn::LightType::Sphere: dst->radius = src.params.sphere.radius; break;
      case tydn::LightType::Spot:
        dst->shapingConeAngle = src.params.spot.angle * 57.2957795131f; break;
      case tydn::LightType::Cylinder:
        dst->radius = src.params.cylinder.radius;
        dst->length = src.params.cylinder.length; break;
      default: break;
    }
    auto resolveLinks = [&](const char* instanceName,
                            const std::vector<std::string>& directTargets,
                            bool* all, std::vector<int>* indices) {
      const std::string base = std::string("collection:") + instanceName + ":";
      if (p.HasProperty(base + "membershipExpression")) {
        *all = true;  // path-expression evaluation is intentionally unsupported
        return;
      }
      const std::vector<tnext::Path>* includes =
          p.GetRelationship(base + "includes");
      const std::vector<tnext::Path>* excludes =
          p.GetRelationship(base + "excludes");
      std::vector<std::string>* carrierPaths =
          std::string(instanceName) == "lightLink" ? &dst->lightLinkPaths
                                                     : &dst->shadowLinkPaths;
      for (const std::string& target : directTargets) carrierPaths->push_back(target);
      if (includes) {
        for (const tnext::Path& target : *includes)
          carrierPaths->push_back(target.str());
      }
      const bool authoredCollection = includes || excludes;
      if (!authoredCollection && directTargets.empty()) { *all = true; return; }
      bool includeRoot = false;
      if (const tnext::Value* value = p.GetPropertyValue(base + "includeRoot")) {
        if (const bool* authored = value->as_bool()) includeRoot = *authored;
      }
      std::string expansionRule = "expandPrims";
      if (const tnext::Value* value =
              p.GetPropertyValue(base + "expansionRule")) {
        if (const std::string* token = value->as_token()) expansionRule = *token;
      }
      const bool explicitOnly = expansionRule == "explicitOnly";
      auto under = [](const std::string& path, const std::string& root) {
        return path == root ||
               (path.size() > root.size() &&
                path.compare(0, root.size(), root) == 0 &&
                path[root.size()] == '/');
      };
      *all = false;
      for (size_t i = 0; i < draw->meshes.size(); ++i) {
        const std::string& path = draw->meshes[i].absPath;
        if (std::getenv("LUSDVIEW_DEBUG_LIGHTS"))
          std::fprintf(stderr, "[light-links] %s mesh %zu path='%s'\n",
                       instanceName, i, path.c_str());
        bool excluded = false;
        if (excludes) {
          for (const tnext::Path& target : *excludes) {
            if (under(path, target.str())) { excluded = true; break; }
          }
        }
        if (excluded) continue;
        bool included = includeRoot && under(path, p.GetPath().str());
        if (!included && includes) {
          for (const tnext::Path& target : *includes) {
            included = explicitOnly ? path == target.str()
                                    : under(path, target.str());
            if (included) break;
          }
        }
        if (!authoredCollection && !included) {
          for (const std::string& target : directTargets) {
            if (under(path, target)) { included = true; break; }
          }
        }
        if (included) indices->push_back(static_cast<int>(i));
      }
    };
    resolveLinks("lightLink", src.light_link_targets, &dst->lightLinksAll,
                 &dst->lightLinkMeshIndices);
    resolveLinks("shadowLink", src.shadow_link_targets, &dst->shadowLinksAll,
                 &dst->shadowLinkMeshIndices);
    return true;
  };

  auto bakeDerived = [](DrawLightCPU* light) {
    ApplyDerivedLightParams(light);
    if (!light->shapingIesFile.empty()) {
      std::string error;
      if (!LoadIesProfile(light->shapingIesFile, light, &error)) {
        std::fprintf(stderr, "[lusdview][warn] light '%s': IES profile unavailable (%s)\n",
                     light->absPath.c_str(), error.c_str());
      }
    }
  };

  std::function<void(const tnext::UsdPrim&)> rec = [&](const tnext::UsdPrim& p) {
    if (p.GetTypeName() == "DomeLight" || p.GetTypeName() == "DomeLight_1") {
      DrawLightCPU light;
      if (!fillConverted(p, &light)) return;

      double w16[16];
      if (tydn::ComputeWorldTransform(stage, p, w16, time)) {
        for (int i = 0; i < 16; ++i) {
          light.transform[i] = static_cast<float>(w16[i]);
        }
        light.position[0] = static_cast<float>(w16[12]);
        light.position[1] = static_cast<float>(w16[13]);
        light.position[2] = static_cast<float>(w16[14]);
        light.direction[0] = -static_cast<float>(w16[8]);
        light.direction[1] = -static_cast<float>(w16[9]);
        light.direction[2] = -static_cast<float>(w16[10]);
      } else {
        for (int i = 0; i < 16; ++i) light.transform[i] = (i % 5 == 0) ? 1.f : 0.f;
      }

      light.domeTextureFormat = DrawLightCPU::DomeTextureFormat::Automatic;
      if (const auto format = lightEval.EvalToken(p, "inputs:texture:format")) {
        if (*format == "latlong")
          light.domeTextureFormat = DrawLightCPU::DomeTextureFormat::Latlong;
        else if (*format == "mirroredBall")
          light.domeTextureFormat = DrawLightCPU::DomeTextureFormat::MirroredBall;
        else if (*format == "angular")
          light.domeTextureFormat = DrawLightCPU::DomeTextureFormat::Angular;
      }

      const auto textureFile = lightEval.EvalAssetPath(p, "inputs:texture:file");
      if (textureFile && !textureFile->empty()) {
        light.textureFile = *textureFile;
        std::string tpath = *textureFile;
        if (!tpath.empty() && !lightusd::io::IsAbsPath(tpath) && !baseDir.empty()) {
          tpath = baseDir + "/" + tpath;
        }
        if (texOpts.domeIbl > 0 && TexToolsAvailable()) {
          const auto t0 = std::chrono::steady_clock::now();
          std::vector<float> rgb;
          int ew = 0, eh = 0;
          auto res = lightusd::image::LoadImageFromFile(tpath);
          if (res) {
            const lightusd::Image& img = res.value().image;
            const int ch = img.channels;
            if (img.width > 0 && img.height > 0 && ch >= 1) {
              // Do not expand a very large HDR source into a same-resolution
              // float-RGB staging buffer. The image loader has already decoded
              // the source bytes, but this resampling keeps the additional
              // conversion allocation bounded by the texture policy (and a
              // conservative preview cap when no policy was supplied).
              const int envMaxEdge = texOpts.maxTextureSize > 0
                                         ? texOpts.maxTextureSize
                                         : 4096;
              const float envScale =
                  std::min(1.0f, static_cast<float>(envMaxEdge) /
                                     static_cast<float>(std::max(img.width,
                                                                  img.height)));
              const int sampleW = std::max(
                  1, static_cast<int>(std::floor(img.width * envScale)));
              const int sampleH = std::max(
                  1, static_cast<int>(std::floor(img.height * envScale)));
              const size_t srcPixels = static_cast<size_t>(img.width) *
                                       static_cast<size_t>(img.height);
              const size_t bytesPerChannel = img.bpp == 32 ? 4u :
                                             img.bpp == 16 ? 2u :
                                             img.bpp == 8 ? 1u : 0u;
              const bool supported = bytesPerChannel != 0 &&
                  srcPixels <= (std::numeric_limits<size_t>::max)() /
                                  (static_cast<size_t>(ch) * bytesPerChannel) &&
                  img.data.size() >= srcPixels * static_cast<size_t>(ch) *
                                      bytesPerChannel;
              rgb.resize(static_cast<size_t>(sampleW) *
                         static_cast<size_t>(sampleH) * 3u);
              auto readChannel = [&](int x, int y, int c) -> float {
                const size_t index =
                    (static_cast<size_t>(y) * static_cast<size_t>(img.width) +
                     static_cast<size_t>(x)) * static_cast<size_t>(ch) +
                    static_cast<size_t>(std::min(c, ch - 1));
                if (img.bpp == 32) {
                  const float* px =
                      reinterpret_cast<const float*>(img.data.data());
                  return px[index];
                }
                if (img.bpp == 16) {
                  const uint16_t* px =
                      reinterpret_cast<const uint16_t*>(img.data.data());
                  return NextHalfToFloat(px[index]);
                }
                return static_cast<float>(img.data[index]) / 255.0f;
              };
              bool decoded = supported;
              if (decoded) {
                for (int y = 0; y < sampleH; ++y) {
                  const int sy = std::min(
                      img.height - 1,
                      static_cast<int>((static_cast<int64_t>(y) * img.height) /
                                       sampleH));
                  for (int x = 0; x < sampleW; ++x) {
                    const int sx = std::min(
                        img.width - 1,
                        static_cast<int>((static_cast<int64_t>(x) * img.width) /
                                         sampleW));
                    const size_t dst =
                        (static_cast<size_t>(y) * static_cast<size_t>(sampleW) +
                         static_cast<size_t>(x)) * 3u;
                    const float c0 = readChannel(sx, sy, 0);
                    rgb[dst + 0] = c0;
                    rgb[dst + 1] = ch > 1 ? readChannel(sx, sy, 1) : c0;
                    rgb[dst + 2] = ch > 2 ? readChannel(sx, sy, 2) : c0;
                  }
                }
              }
              if (sampleW != img.width || sampleH != img.height) {
                fprintf(stderr,
                        "[lusdview] dome envmap downsampled (next): %dx%d -> %dx%d\n",
                        img.width, img.height, sampleW, sampleH);
              }
              // Release the decoder's large source allocation before the IBL
              // baker creates its cube/prefilter staging buffers.
              res.value().image.data.clear();
              res.value().image.data.shrink_to_fit();
              ew = sampleW;
              eh = sampleH;
              if (decoded) {
                if (light.domeTextureFormat ==
                        DrawLightCPU::DomeTextureFormat::MirroredBall ||
                    light.domeTextureFormat ==
                        DrawLightCPU::DomeTextureFormat::Angular) {
                  std::vector<float> eq;
                  int eqH = 0;
                  const int eqW = std::min(2048, std::max(256, 2 * ew));
                  if (TexToolsProbeToEquirect(
                          rgb.data(), ew, eh,
                          static_cast<int>(light.domeTextureFormat), eqW, &eq,
                          &eqH)) {
                    rgb = std::move(eq);
                    ew = eqW;
                    eh = eqH;
                  } else {
                    decoded = false;
                  }
                }
              }
              if (decoded &&
                  TexToolsBuildDomeIbl(rgb.data(), ew, eh, texOpts.domeIbl >= 2,
                                       &light.ibl)) {
                const double ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
                fprintf(stderr,
                        "[lusdview] dome IBL bake (next) '%s': %dx%d -> spec %d/irr %d in %.0f ms\n",
                        light.name.c_str(), ew, eh, light.ibl.specFaceSize,
                        light.ibl.irrFaceSize, ms);
              }
            }
          } else {
            fprintf(stderr, "[lusdview] dome envmap load failed (next): %s\n",
                    tpath.c_str());
          }
        }
        }
        bakeDerived(&light);
        draw->lights.push_back(std::move(light));
    } else {
      // Non-dome lights: enough for the raster preview key-light derivation
      // (UpdatePreviewLight uses a Distant light's direction, else a finite
      // light's position). Type name -> DrawLightCPU::Type.
      const std::string ty = p.GetTypeName();
      DrawLightCPU::Type lt = DrawLightCPU::Type::Point;
      bool isLight = true;
      if (ty == "DistantLight" || ty == "DistantLight_1")
        lt = DrawLightCPU::Type::Distant;
      else if (ty == "SphereLight")
        lt = DrawLightCPU::Type::Sphere;
      else if (ty == "RectLight")
        lt = DrawLightCPU::Type::Rect;
      else if (ty == "DiskLight")
        lt = DrawLightCPU::Type::Disk;
      else if (ty == "CylinderLight")
        lt = DrawLightCPU::Type::Cylinder;
      else if (ty == "GeometryLight")
        lt = DrawLightCPU::Type::Geometry;
      else if (ty == "PortalLight")
        lt = DrawLightCPU::Type::Portal;
      else
        isLight = false;

      if (isLight) {
        DrawLightCPU light;
        if (!fillConverted(p, &light)) return;
        light.type = lt;

        double w16[16];
        const bool haveXf = tydn::ComputeWorldTransform(stage, p, w16, time);
        if (haveXf) {
          for (int i = 0; i < 16; ++i) {
            light.transform[i] = static_cast<float>(w16[i]);
          }
          // Row 3 = translation (position); light faces local -Z, so the
          // emission direction is -(row 2). Matches the tydra RenderLight
          // derivation (render-data.cc).
          light.position[0] = static_cast<float>(w16[12]);
          light.position[1] = static_cast<float>(w16[13]);
          light.position[2] = static_cast<float>(w16[14]);
          light.direction[0] = -static_cast<float>(w16[8]);
          light.direction[1] = -static_cast<float>(w16[9]);
          light.direction[2] = -static_cast<float>(w16[10]);
        }

        bakeDerived(&light);
        if (light.type == DrawLightCPU::Type::Geometry &&
            !light.geometryTargetPath.empty()) {
          for (size_t meshIndex = 0; meshIndex < draw->meshes.size(); ++meshIndex) {
            if (draw->meshes[meshIndex].absPath == light.geometryTargetPath) {
              light.geometryMesh = static_cast<int>(meshIndex);
              break;
            }
          }
        }
        if (light.type == DrawLightCPU::Type::Geometry) {
          if (light.geometryMesh < 0) {
            draw->skipped.push_back(
                "GeometryLight '" + light.absPath +
                "': emissive-mesh target could not be resolved");
          }
        }
        draw->lights.push_back(std::move(light));
      }
    }
    for (const tnext::UsdPrim& child : p.GetChildren()) rec(child);
  };
  for (const tnext::UsdPrim& root : stage.GetRootPrims()) rec(root);
}

// UsdVol volumes for the `next` path: walk the stage, find Volume prims,
// resolve each `field:*` relationship to its field-asset prim, load the .vdb
// (relative to the USD file dir), and emit a DrawVolumeCPU. Extends `bounds`
// with the volume world-AABB so the camera frames it.
bool FitNextVolumeDensity(DrawVolumeCPU* volume, size_t maxBytes) {
  if (!volume || maxBytes < sizeof(float) || volume->density.empty() ||
      volume->dim[0] <= 0 || volume->dim[1] <= 0 || volume->dim[2] <= 0)
    return false;
  const size_t sourceVoxels = volume->density.size();
  const size_t maxVoxels = maxBytes / sizeof(float);
  if (sourceVoxels <= maxVoxels) return false;

  int dims[3] = {volume->dim[0], volume->dim[1], volume->dim[2]};
  const double scale = std::cbrt(static_cast<double>(maxVoxels) /
                                 static_cast<double>(sourceVoxels));
  for (int a = 0; a < 3; ++a)
    dims[a] = std::max(1, static_cast<int>(std::floor(dims[a] * scale)));
  auto voxelCount = [&]() -> size_t {
    const size_t x = static_cast<size_t>(dims[0]);
    const size_t y = static_cast<size_t>(dims[1]);
    const size_t z = static_cast<size_t>(dims[2]);
    if (x > (std::numeric_limits<size_t>::max)() / y) return 0;
    const size_t xy = x * y;
    if (xy > (std::numeric_limits<size_t>::max)() / z) return 0;
    return xy * z;
  };
  while (voxelCount() > maxVoxels) {
    int largest = 0;
    if (dims[1] > dims[largest]) largest = 1;
    if (dims[2] > dims[largest]) largest = 2;
    if (dims[largest] <= 1) break;
    --dims[largest];
  }
  const size_t targetVoxels = voxelCount();
  if (targetVoxels == 0 || targetVoxels >= sourceVoxels) return false;

  const size_t sx = static_cast<size_t>(volume->dim[0]);
  const size_t sy = static_cast<size_t>(volume->dim[1]);
  const size_t sz = static_cast<size_t>(volume->dim[2]);
  auto reduceField = [&](std::vector<float>* field) {
    if (!field || field->size() != sourceVoxels) return;
    std::vector<float> reduced(targetVoxels);
    for (int z = 0; z < dims[2]; ++z) {
    const size_t oz = std::min(
        sz - 1, (static_cast<size_t>(z) * sz) /
                   static_cast<size_t>(dims[2]));
    for (int y = 0; y < dims[1]; ++y) {
      const size_t oy = std::min(
          sy - 1, (static_cast<size_t>(y) * sy) /
                   static_cast<size_t>(dims[1]));
      for (int x = 0; x < dims[0]; ++x) {
        const size_t ox = std::min(
            sx - 1, (static_cast<size_t>(x) * sx) /
                   static_cast<size_t>(dims[0]));
        const size_t dst = (static_cast<size_t>(z) *
                            static_cast<size_t>(dims[1]) +
                            static_cast<size_t>(y)) *
                               static_cast<size_t>(dims[0]) +
                           static_cast<size_t>(x);
        reduced[dst] = (*field)[(oz * sy + oy) * sx + ox];
      }
    }
    }
    field->swap(reduced);
  };
  reduceField(&volume->density);
  reduceField(&volume->emissionField);
  reduceField(&volume->temperatureField);
  volume->dim[0] = dims[0];
  volume->dim[1] = dims[1];
  volume->dim[2] = dims[2];
  return true;
}

// Resample an auxiliary VDB grid onto the density lattice in object space.
// This preserves independently authored OpenVDB transforms while keeping the
// GPU carrier compact (all 3D textures share density dimensions/bounds).
std::vector<float> ResampleNextVolumeField(
    const lightusd::usdVol::VDBGrid& src,
    const lightusd::usdVol::VDBGrid& density) {
  const size_t n = size_t(density.dim[0]) * size_t(density.dim[1]) *
                   size_t(density.dim[2]);
  std::vector<float> out(n, src.background);
  if (src.data.empty()) return out;
  for (int z = 0; z < density.dim[2]; ++z) {
    for (int y = 0; y < density.dim[1]; ++y) {
      for (int x = 0; x < density.dim[0]; ++x) {
        const int dc[3] = {x, y, z};
        int sc[3];
        for (int a = 0; a < 3; ++a) {
          const double world =
              double(density.origin[a] + dc[a]) * density.voxel_size[a] +
              density.world_translation[a];
          sc[a] = int(std::floor((world - src.world_translation[a]) /
                                 src.voxel_size[a] + 0.5)) - src.origin[a];
          sc[a] = std::max(0, std::min(src.dim[a] - 1, sc[a]));
        }
        out[size_t(x) + size_t(density.dim[0]) *
                            (size_t(y) + size_t(density.dim[1]) * size_t(z))] =
            src.data[size_t(sc[0]) + size_t(src.dim[0]) *
                       (size_t(sc[1]) + size_t(src.dim[1]) * size_t(sc[2]))];
      }
    }
  }
  return out;
}

void ResolveNextVolumeMaterial(const tnext::Stage& stage,
                               const tnext::UsdPrim& volume,
                               DrawVolumeCPU* out) {
  if (!out) return;
  const std::string materialPath =
      tnext::GetInheritedBoundMaterialPath(stage, volume.GetPath().str());
  if (materialPath.empty()) return;
  const tnext::UsdPrim material = stage.GetPrimAtPath(materialPath);
  if (!material) return;
  const std::string shaderPath = tnext::GetVolumeShader(stage, material);
  if (shaderPath.empty()) return;
  const tnext::UsdPrim shader = stage.GetPrimAtPath(shaderPath);
  if (!shader) return;
  auto scalar = [&](const char* name, float* dst) {
    tnext::Value value;
    if (!tnext::ResolveShaderPortValue(stage, shader, name, &value)) return;
    if (const float* f = value.as_float()) *dst = *f;
    else if (const double* d = value.as_double()) *dst = float(*d);
  };
  auto color = [&](const char* name, float dst[3]) {
    tnext::Value value;
    if (!tnext::ResolveShaderPortValue(stage, shader, name, &value)) return;
    if (const float* f = value.as_float3()) {
      dst[0] = f[0]; dst[1] = f[1]; dst[2] = f[2];
    }
  };
  float emissionScale = 1.0f;
  scalar("inputs:density", &out->densityScale);
  color("inputs:scattering_color", out->albedo);
  color("inputs:scatter_color", out->albedo);
  color("inputs:emission_color", out->emission);
  color("inputs:emissionColor", out->emission);
  scalar("inputs:emission", &emissionScale);
  scalar("inputs:emission_intensity", &emissionScale);
  scalar("inputs:emissionIntensity", &emissionScale);
  out->densityScale = std::max(0.0f, out->densityScale);
  emissionScale = std::max(0.0f, emissionScale);
  for (float& channel : out->emission) channel *= emissionScale;
}

static void ResolveNextSurfaceVolumeMaterial(const tnext::Stage& stage,
                                             const tnext::UsdPrim& material,
                                             DrawMaterialCPU* out) {
  if (!out) return;
  const std::string shaderPath = tnext::GetVolumeShader(stage, material);
  if (shaderPath.empty()) return;
  const tnext::UsdPrim shader = stage.GetPrimAtPath(shaderPath);
  if (!shader) return;
  auto scalar = [&](const char* name, float* dst) {
    tnext::Value value;
    if (!tnext::ResolveShaderPortValue(stage, shader, name, &value)) return;
    if (const float* f = value.as_float()) *dst = *f;
    else if (const double* d = value.as_double()) *dst = float(*d);
  };
  auto color = [&](const char* name, float dst[3]) {
    tnext::Value value;
    if (!tnext::ResolveShaderPortValue(stage, shader, name, &value)) return;
    if (const float* f = value.as_float3()) {
      dst[0] = f[0]; dst[1] = f[1]; dst[2] = f[2];
    }
  };
  scalar("inputs:density", &out->volumeDensity);
  color("inputs:scattering_color", out->volumeAlbedo);
  color("inputs:scatter_color", out->volumeAlbedo);
  color("inputs:emission_color", out->volumeEmission);
  color("inputs:emissionColor", out->volumeEmission);
  scalar("inputs:emission", &out->volumeEmissionScale);
  scalar("inputs:emission_intensity", &out->volumeEmissionScale);
  scalar("inputs:emissionIntensity", &out->volumeEmissionScale);
  out->volumeDensity = std::max(0.0f, out->volumeDensity);
  out->volumeEmissionScale = std::max(0.0f, out->volumeEmissionScale);
}

// MaterialX permits displacement to be authored as a separate material
// terminal rather than as a surface input. Preserve its scalar fallback in
// the same geometry lane used by PreviewSurface/OpenPBR displacement instead
// of recording the terminal path and silently ignoring the value.
static void ResolveNextDisplacementMaterial(const tnext::Stage& stage,
                                            const tnext::UsdPrim& material,
                                            DrawMaterialCPU* out) {
  if (!out || !out->hasDisplacementOutput || out->displacementTex >= 0 ||
      out->displacementConst != 0.0f)
    return;
  const std::string shaderPath = tnext::GetDisplacementShader(stage, material);
  if (shaderPath.empty()) return;
  const tnext::UsdPrim shader = stage.GetPrimAtPath(shaderPath);
  if (!shader) return;
  static const char* kInputs[] = {"inputs:displacement", "inputs:height",
                                  "inputs:dispScalar", "inputs:value"};
  for (const char* input : kInputs) {
    tnext::Value value;
    if (!tnext::ResolveShaderPortValue(stage, shader, input, &value)) continue;
    if (const float* f = value.as_float()) {
      out->displacementConst = *f;
      return;
    }
    if (const double* d = value.as_double()) {
      out->displacementConst = static_cast<float>(*d);
      return;
    }
    if (const int32_t* i = value.as_int()) {
      out->displacementConst = static_cast<float>(*i);
      return;
    }
  }
}

bool BuildNextVolumes(
    const tnext::Stage& stage, const std::string& usdPath, double time,
    DrawScene* draw, Bounds* bounds,
    const std::function<bool(DrawVolumeCPU&&)>* publish = nullptr,
    size_t densityBudgetBytes = 0, size_t* densityBytesUsed = nullptr) {
  const std::string baseDir = lightusd::io::GetBaseDir(usdPath);

  std::function<bool(const tnext::UsdPrim&)> rec =
      [&](const tnext::UsdPrim& p) {
    if (p.GetTypeName() == "Volume") {
      double w16[16];
      tydn::ComputeWorldTransform(stage, p, w16, time);

      bool hasDensityRelationship = false;
      for (const std::string& name : p.GetRelationshipNames()) {
        if (name == "field:density") hasDensityRelationship = true;
      }

      for (const std::string& relName : p.GetRelationshipNames()) {
        if (relName.rfind("field:", 0) != 0) continue;
        if (hasDensityRelationship && relName != "field:density") continue;
        const std::vector<tnext::Path>* targets = p.GetRelationship(relName);
        if (!targets || targets->empty()) continue;
        tnext::UsdPrim field = stage.GetPrimAtPath((*targets)[0]);
        if (!field) continue;

        const tnext::Value* fp = field.GetPropertyValue("filePath");
        const std::string* ap = fp ? fp->as_asset_path() : nullptr;
        if (!ap || ap->empty()) continue;
        std::string fieldName = relName.substr(std::strlen("field:"));
        if (const tnext::Value* fn = field.GetPropertyValue("fieldName")) {
          if (const std::string* tk = fn->as_token()) fieldName = *tk;
        }

        // Resolve the asset path relative to the USD file directory.
        std::string vpath = *ap;
        if (!vpath.empty() && !lightusd::io::IsAbsPath(vpath) && !baseDir.empty()) {
          vpath = baseDir + "/" + vpath;
        }
        std::vector<lightusd::usdVol::VDBGrid> grids;
        std::string vw, ve;
        if (!lightusd::usdVol::ReadVDBFromFile(vpath, &grids, &vw, &ve) || grids.empty()) {
          const std::string reason = !ve.empty() ? ve : (!vw.empty() ? vw :
              "no supported voxel grids");
          draw->skipped.push_back("Volume '" + p.GetPath().str() + "': " + reason);
          LOGW("next: Volume '%s' failed to load '%s': %s",
               p.GetPath().str().c_str(), vpath.c_str(), reason.c_str());
          continue;
        }
        lightusd::usdVol::VDBGrid* g = nullptr;
        for (auto& gg : grids)
          if (gg.name == fieldName) { g = &gg; break; }
        if (!g) g = &grids[0];
        if (g->data.empty() || g->dim[0] <= 0 || g->dim[1] <= 0 || g->dim[2] <= 0)
          continue;

        DrawVolumeCPU dv;
        dv.name = p.GetName();
        for (int k = 0; k < 16; ++k) dv.world[k] = static_cast<float>(w16[k]);
        // Transfer ownership out of the temporary VDB grid instead of copying
        // a dense field while the decoded archive remains alive.
        for (const auto& aux : grids) {
          if (aux.name == "temperature") {
            dv.temperatureField = ResampleNextVolumeField(aux, *g);
          } else if (aux.name == "emission" || aux.name == "flame" ||
                     aux.name == "heat") {
            dv.emissionField = ResampleNextVolumeField(aux, *g);
          }
        }
        dv.density = std::move(g->data);
        for (int a = 0; a < 3; ++a) {
          dv.dim[a] = g->dim[a];
          dv.aabbMin[a] = float(g->origin[a]) * float(g->voxel_size[a]) +
                          float(g->world_translation[a]);
          dv.aabbMax[a] = float(g->origin[a] + g->dim[a]) * float(g->voxel_size[a]) +
                          float(g->world_translation[a]);
        }
        dv.background = g->background;
        ResolveNextVolumeMaterial(stage, p, &dv);

        if (densityBudgetBytes > 0 && densityBytesUsed) {
          const size_t used = *densityBytesUsed;
          const size_t remaining = used < densityBudgetBytes
                                       ? densityBudgetBytes - used
                                       : 0;
          const size_t sourceBytes = dv.density.size() * sizeof(float);
          if (remaining < sizeof(float)) {
            if (draw) {
              draw->skipped.push_back(
                  "Volume '" + p.GetPath().str() +
                  "': density budget exhausted; grid skipped");
            }
            continue;
          }
          const int sourceDim[3] = {dv.dim[0], dv.dim[1], dv.dim[2]};
          const bool reduced = FitNextVolumeDensity(&dv, remaining);
          if (reduced) {
            LOGW("Volume '%s': density reduced from %dx%dx%d (%.1f MiB) "
                 "to %dx%dx%d (%.1f MiB) by the volume budget",
                 p.GetPath().str().c_str(), sourceDim[0], sourceDim[1],
                 sourceDim[2], double(sourceBytes) / (1024.0 * 1024.0),
                 dv.dim[0], dv.dim[1], dv.dim[2],
                 double(dv.density.size() * sizeof(float)) /
                     (1024.0 * 1024.0));
            if (draw) {
              draw->skipped.push_back(
                  "Volume '" + p.GetPath().str() +
                  "': density downsampled to fit the memory budget");
            }
          }
          *densityBytesUsed += dv.density.size() * sizeof(float);
        }

        // Extend scene bounds with the volume world-AABB (8 corners). World
        // matrix is stored row-major-USD in w16 (p' = p * M), matching meshes.
        for (int corner = 0; corner < 8; ++corner) {
          float lp[3] = {(corner & 1) ? dv.aabbMax[0] : dv.aabbMin[0],
                         (corner & 2) ? dv.aabbMax[1] : dv.aabbMin[1],
                         (corner & 4) ? dv.aabbMax[2] : dv.aabbMin[2]};
          float wp[3];
          for (int c = 0; c < 3; ++c)
            wp[c] = lp[0] * float(w16[0 * 4 + c]) + lp[1] * float(w16[1 * 4 + c]) +
                    lp[2] * float(w16[2 * 4 + c]) + float(w16[3 * 4 + c]);
          bounds->add(wp);
        }
        if (publish && *publish) {
          if (!(*publish)(std::move(dv))) return false;
        } else {
          draw->volumes.push_back(std::move(dv));
        }
      }
    }
    for (const tnext::UsdPrim& c : p.GetChildren()) {
      if (!rec(c)) return false;
    }
    return true;
  };
  for (const tnext::UsdPrim& r : stage.GetRootPrims()) {
    if (!rec(r)) return false;
  }
  return true;
}

bool LoadUSDViaNext(const std::string& path, const LoadOptions& opts,
                    DrawScene* draw, std::string* warn, std::string* err,
                    LoadControl* ctrl,
                    std::shared_ptr<tnext::StageSession>* out_session,
                    tnext::StageChangeSet* out_changes,
                    ProgressiveSceneStream* stream,
                    const std::string& reload_layer_id) {
  const auto loadBegin = std::chrono::steady_clock::now();
  const bool timing = opts.timing;
  const size_t previewMaxBoxes = opts.previewMaxBoxes != 0
      ? opts.previewMaxBoxes
      : kDefaultCheckpointPreviewMaxBoxes;
  // --- 1. Open a persistent next document. Parsed dependency layers remain in
  // the PCP cache for payload and variant edits instead of being reparsed. ---
  auto session = (out_session && *out_session)
                     ? *out_session
                     : std::make_shared<tnext::StageSession>();
  const bool sessionWasOpen = session->IsOpen();
  tnext::StageChangeSet aggregateChanges;
  if (sessionWasOpen) {
    const uint64_t revision = session->GetSnapshot().revision;
    aggregateChanges.base_revision = revision;
    aggregateChanges.new_revision = revision;
  }
  const std::string previewFingerprint = PreviewFingerprint(opts);
  bool previewPublished = false;
  bool earlyPreviewPublished = false;
  bool previewCacheHit = false;
  tnext::StageSnapshot generatedPreview;
  if (!sessionWasOpen && stream && opts.progressivePreview &&
      opts.previewCache.mode != PreviewCacheMode::Off) {
    const auto cacheBegin = std::chrono::steady_clock::now();
    PreviewCacheLookup cached =
        LoadPreviewCache(opts.previewCache, path, previewFingerprint);
    if (cached.hit) {
      const double previewTime = std::isfinite(opts.timecode) ? opts.timecode : 0.0;
      DrawScene proxy = BuildCheckpointPreview(
          cached.stage, previewTime, previewMaxBoxes,
                                               opts.viewCamera);
      if (!proxy.meshes.empty()) {
        previewPublished = stream->pushPreview(std::move(proxy));
        previewCacheHit = previewPublished;
      }
    }
    if (timing) {
      LOGI("preview cache: %s in %.3f s", cached.reason.c_str(),
           std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        cacheBegin)
               .count());
    }
  }
  tnext::StageSessionOptions session_options;
  session_options.compose = opts.composition;
  if (opts.maxMemoryBytes > 0) {
    session_options.load.limits.max_resident_bytes = opts.maxMemoryBytes;
  }
  // The viewer build enables next's thread-safe PCP paths. Large payload scenes
  // have tens of thousands of independent prim opinion records, so fill those
  // concurrently instead of leaving CompositionOptions at its serial default.
  const unsigned compositionThreads = opts.compositionThreads
      ? opts.compositionThreads
      : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  session_options.execution.max_threads =
      static_cast<int>(std::min(64u, compositionThreads));
  session_options.composition.opinion_batch_size =
      opts.compositionOpinionBatch;
  session_options.composition.enable_timing = timing;
  if (opts.maxMemoryBytes > 0) {
    session_options.cache_retention = tnext::CacheRetention::LayersOnly;
  }
  session_options.resolver.allow_parent_paths = opts.allowParentRelativePaths;
  session_options.composition.variant_overrides_by_path = opts.variantOverrides;
  if (stream && opts.progressivePreview && !sessionWasOpen) {
    session_options.early_preview_callback =
        [stream, &earlyPreviewPublished, &opts, previewMaxBoxes](
            const tnext::StagePreview& preview) {
          if (!preview.snapshot || stream->cancelled()) return false;
          const double previewTime = std::isfinite(opts.timecode)
                                         ? opts.timecode
                                         : 0.0;
          DrawScene proxy = BuildCheckpointPreview(
              *preview.snapshot, previewTime, previewMaxBoxes,
              opts.viewCamera);
          if (proxy.meshes.empty()) {
            if (opts.timing) {
              LOGI("next timing: early root preview had no bounds");
            }
            return true;
          }
          earlyPreviewPublished = stream->pushPreview(std::move(proxy));
          if (opts.timing) {
            LOGI("next timing: early root preview published (%s)",
                 earlyPreviewPublished ? "ready" : "cancelled");
          }
          return earlyPreviewPublished;
        };
  }
  if (ctrl) {
    session_options.composition.payload_load_callback =
        [ctrl](const tnext::Path&) {
          const long long total = ctrl->payloadsTotal.load();
          long long done = ctrl->payloadsDone.load();
          while (done < total &&
                 !ctrl->payloadsDone.compare_exchange_weak(done, done + 1)) {
          }
        };
  }
  if (opts.payloadPolicy == PayloadPolicy::DeferAll) {
    session_options.composition.load_payloads = false;
  } else if (opts.payloadPolicy == PayloadPolicy::Whitelist) {
    session_options.composition.load_payloads = false;
    const std::set<std::string> whitelist = opts.payloadWhitelist;
    session_options.composition.payload_policy =
        [whitelist](const tnext::Path& prim_path, const std::string&) {
          return whitelist.count(prim_path.str()) != 0;
        };
  }
  if (ctrl) {
    session_options.progress_callback =
        [ctrl](const tnext::ProgressEvent& event) {
          ctrl->detailPhase.store(static_cast<int>(
              event.phase == tnext::ProgressPhase::RootLoad
                  ? LoadDetailPhase::Parsing
                  : LoadDetailPhase::Composing));
          ctrl->stage.store(static_cast<int>(event.phase));
          ctrl->phasePermille.store(static_cast<int>(
              std::clamp(event.progress, 0.0f, 1.0f) * 1000.0f));
          return !ctrl->cancel.load();
        };
  }
  if (stream && opts.progressivePreview && !previewCacheHit) {
    session_options.preview_callback =
        [stream, &previewPublished, &generatedPreview,
         &opts, previewMaxBoxes](const tnext::StagePreview& preview) {
          if (!preview.snapshot || stream->cancelled()) return false;
          generatedPreview = preview.snapshot;
          const double previewTime = std::isfinite(opts.timecode)
                                         ? opts.timecode
                                         : 0.0;
          DrawScene proxy = BuildCheckpointPreview(
              *preview.snapshot, previewTime, previewMaxBoxes,
              opts.viewCamera);
          if (proxy.meshes.empty()) return true;
          previewPublished = stream->pushPreview(std::move(proxy));
          return previewPublished;
        };
  }
  bool opened = session->IsOpen();
  if (opened) {
    if (!reload_layer_id.empty()) {
      const tnext::StageEditResult edit = session->ReloadLayer(reload_layer_id);
      opened = static_cast<bool>(edit);
      if (opened) tnext::AppendStageChangeSet(edit.changes, &aggregateChanges);
    }
    if (opened && session->GetVariantSelections() != opts.variantOverrides) {
      const tnext::StageEditResult edit =
          session->SetVariantSelections(opts.variantOverrides);
      opened = static_cast<bool>(edit);
      if (opened) tnext::AppendStageChangeSet(edit.changes, &aggregateChanges);
    }
    if (opened && opts.payloadPolicy == PayloadPolicy::Whitelist) {
      std::vector<tnext::Path> payload_paths;
      payload_paths.reserve(opts.payloadWhitelist.size());
      for (const std::string& payload_path : opts.payloadWhitelist) {
        payload_paths.emplace_back(payload_path);
      }
      if (ctrl) {
        ctrl->payloadsTotal.store(static_cast<long long>(payload_paths.size()));
        ctrl->payloadsDone.store(0);
      }
      const tnext::StageEditResult edit = session->LoadPayloads(payload_paths);
      opened = static_cast<bool>(edit);
      if (opened) tnext::AppendStageChangeSet(edit.changes, &aggregateChanges);
      if (opened && ctrl)
        ctrl->payloadsDone.store(static_cast<long long>(payload_paths.size()));
    }
  } else {
    opened = session->OpenFile(path, session_options);
    if (opened) tnext::AppendStageChangeSet(session->GetLastChangeSet(),
                                            &aggregateChanges);
  }
  if (!opened) {
    if (err) *err = "next: compose failed: " + session->GetError();
    if (stream) stream->pushFailed(err ? *err : "next: compose failed");
    return false;
  }
  if (out_changes) *out_changes = aggregateChanges;
  const auto composedAt = std::chrono::steady_clock::now();
  const std::vector<std::string> previewDependencies =
      generatedPreview ? session->GetLayerDependencies()
                       : std::vector<std::string>{};
  if (previewPublished && stream && !stream->pushReset()) return false;
  if (generatedPreview && !previewCacheHit) {
    // Cache publication is independent of authoritative conversion. Keep the
    // snapshot alive in a background writer so a large USDC serialization
    // cannot delay the first real mesh. Writers join during orderly shutdown;
    // StorePreviewCache publishes its manifest last if the process is killed.
    const PreviewCacheOptions cacheOptions = opts.previewCache;
    const std::string cachePath = path;
    const std::string cacheFingerprint = previewFingerprint;
    const tnext::StageSnapshot cachePreview = generatedPreview;
    StartPreviewCacheWriter(
        [cacheOptions, cachePath, cacheFingerprint, cachePreview,
         previewDependencies, timing]() {
      const auto cacheBegin = std::chrono::steady_clock::now();
      std::string cacheReason;
      const bool stored = StorePreviewCache(
          cacheOptions, cachePath, cacheFingerprint, *cachePreview,
          previewDependencies, &cacheReason);
      if (timing) {
        LOGI("preview cache: %s%s in %.3f s",
             stored ? "stored" : "not stored: ",
             stored ? "" : cacheReason.c_str(),
             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          cacheBegin)
                 .count());
      }
        });
    generatedPreview = {};
  }
  if (timing) {
    LOGI("next timing: compose %.3f s",
         std::chrono::duration<double>(composedAt - loadBegin).count());
    const tnext::StageSessionMemoryStats mem = session->GetMemoryStats();
    LOGI("next memory: layers %.1f MiB, transient cache %.1f MiB, "
         "composed stage %.1f MiB (estimated total %.1f MiB, peak %.1f MiB)",
         static_cast<double>(mem.source_layer_bytes) / (1024.0 * 1024.0),
         static_cast<double>(mem.transient_cache_bytes) / (1024.0 * 1024.0),
         static_cast<double>(mem.composed_stage_bytes) / (1024.0 * 1024.0),
         static_cast<double>(mem.estimated_total_bytes) / (1024.0 * 1024.0),
         static_cast<double>(mem.peak_estimated_total_bytes) /
             (1024.0 * 1024.0));
    LogProcessMemory("after compose");
  }
  if (out_session) *out_session = session;
  if (warn && !session->GetWarning().empty()) *warn = session->GetWarning();
  const tnext::StageSnapshot stage_snapshot = session->GetSnapshot();
  if (!stage_snapshot) {
    if (err) *err = "next session did not publish a stage";
    return false;
  }
  const tnext::Stage& stage = *stage_snapshot;
  if (ctrl) ctrl->detailPhase.store(static_cast<int>(LoadDetailPhase::Converting));
  const std::vector<tnext::Path> deferredPayloads =
      session->GetDeferredPayloadPaths();
  if (!deferredPayloads.empty()) {
    std::string deferredSummary;
    const size_t shown = std::min<size_t>(deferredPayloads.size(), 8);
    for (size_t i = 0; i < shown; ++i) {
      if (!deferredSummary.empty()) deferredSummary += ", ";
      deferredSummary += deferredPayloads[i].str();
    }
    LOGI("next: %zu payloads deferred%s%s", deferredPayloads.size(),
         deferredSummary.empty() ? "" : ": ", deferredSummary.c_str());
  }
  const double time = std::isnan(opts.timecode) ? 0.0 : opts.timecode;

  // Dependency/deferred state is now captured outside the PCP cache. Retire
  // parsed layers before PointInstancer placements and renderer carriers begin
  // to overlap the composed stage. Later payload/variant edits rebuild lazily.
  if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
    session->ReleaseCompositionCache();
    if (timing) {
      const tnext::StageSessionMemoryStats mem = session->GetMemoryStats();
      LOGI("next memory: released composition cache; retained stage %.1f MiB "
           "(estimated total %.1f MiB)",
           static_cast<double>(mem.composed_stage_bytes) / (1024.0 * 1024.0),
           static_cast<double>(mem.estimated_total_bytes) / (1024.0 * 1024.0));
    }
  }

  // --- 2. A per-mesh converter (NOT a full-scene Convert). We triangulate each
  //        mesh on demand (ConvertMesh) as we walk the stage and free it right
  //        after baking it into a batch, so the whole RenderScene's geometry
  //        (~half the load peak) is never resident at once. ---
  tydn::ConverterConfig cfg;
  cfg.material.binding_purpose = opts.materialPurpose;
  cfg.mesh.subdivision_level = std::max(0, opts.subdivisionLevel);
  cfg.mesh.subdivision_prim_levels.insert(opts.subdivisionPrimLevels.begin(),
                                          opts.subdivisionPrimLevels.end());
  cfg.mesh.triangulate = true;
  // Authored USD meshes may contain concave n-gons. A triangle fan can emit
  // overlapping or degenerate triangles for them, so use the converter's
  // robust projected earcut path.
  cfg.mesh.triangulation_method = tydn::MeshConfig::TriangulationMethod::Earcut;
  cfg.mesh.compute_normals = true;
  cfg.mesh.build_vertex_indices = true;
  cfg.curves.tessellation_segments =
      std::max<uint32_t>(1u, opts.curveTessellationSegments);
  // The viewer consumes tessellated polylines and immediately copies them
  // into bounded DrawCurvesCPU carriers; retaining the authored control-point
  // stream here would add another full curve-array allocation.
  cfg.curves.retain_control_points = false;
  // Keep the converter from decoding texture pixels: it records RenderTexture
  // metadata (asset path, wrap, scale/bias, channel) regardless, and we decode
  // the pixels ourselves in LoadNextTexture (base dir / .usdz aware).
  cfg.material.load_textures = false;
  cfg.material.allow_missing_textures = true;
  cfg.time_code = time;
  const std::vector<std::string> layerDependencies =
      session->GetLayerDependencies();
  cfg.animation.clip_stage_loader =
      [resolverConfig = session_options.resolver, path, layerDependencies](
          const std::string& assetPath, tnext::Stage* clipStage,
          std::string* clipWarn, std::string* clipErr) {
        if (!clipStage) return false;
        tnext::AssetResolver resolver(resolverConfig);
        std::vector<std::string> candidates;
        candidates.push_back(resolver.ResolvePath(
            assetPath, lightusd::io::GetBaseDir(path)));
        for (const std::string& dependency : layerDependencies) {
          candidates.push_back(resolver.ResolvePath(
              assetPath, lightusd::io::GetBaseDir(dependency)));
        }
        std::string resolved;
        for (const std::string& candidate : candidates) {
          if (!candidate.empty() && resolver.Exists(candidate)) {
            resolved = candidate;
            break;
          }
        }
        if (resolved.empty()) {
          if (clipErr) *clipErr = "asset not found: " + assetPath;
          return false;
        }
        tnext::StageSession clipSession;
        tnext::StageSessionOptions clipOptions;
        clipOptions.resolver = resolverConfig;
        clipOptions.composition.load_payloads = true;
        if (!clipSession.OpenFile(resolved, clipOptions)) {
          if (clipErr) *clipErr = clipSession.GetError();
          return false;
        }
        auto taken = clipSession.CloseAndTakeStage();
        if (!taken) {
          if (clipErr) *clipErr = "clip stage is still retained";
          return false;
        }
        *clipStage = std::move(*taken);
        (void)clipWarn;
        return true;
      };
  tydn::RenderSceneConverter conv(cfg);
  draw->upAxis = (stage.GetUpAxis() == "Z" || stage.GetUpAxis() == "z") ? "Z" : "Y";
  draw->metersPerUnit = stage.GetMetersPerUnit() > 0.0
                            ? stage.GetMetersPerUnit()
                            : 0.01;

  draw->meshes.clear();
  draw->points.clear();
  draw->curves.clear();
  draw->materials.clear();
  draw->textures.clear();
  draw->skipped.clear();
  draw->triangleCount = 0;
  draw->truncated = false;
  draw->materials.emplace_back();  // default gray material (index 0)

  Bounds bounds;
  const std::size_t triCap =
      ctrl ? ctrl->maxTriangles : std::numeric_limits<std::size_t>::max();

  // Texture cache for material building: resolve texture assets against the
  // source directory and, for a .usdz package, its embedded entries. The size
  // cap and byte budget are applied while decoding, so a large scene never
  // materializes every texture at full resolution.
  NextTexCache texCache;
  texCache.progress = ctrl;
  texCache.opt = &opts.textureOptions;  // kept-compressed KTX2 passthrough
  texCache.ptexInitialFaces = opts.ptexInitialFaces;
  if (opts.ptexPhysicalCacheBytes > 0)
    texCache.ptexPhysicalCacheBytes = opts.ptexPhysicalCacheBytes;
  if (opts.textureOptions.textureBudgetMB > 0) {
    const size_t textureBudgetBytes =
        size_t(opts.textureOptions.textureBudgetMB) * size_t{1024} *
        size_t{1024};
    // Ptex is only one consumer of the texture budget. Previously this
    // assignment gave it the whole budget, allowing thousands of fallback
    // atlases to retain 8+ GiB in a scene whose regular textures and GPU
    // tables still needed the same pool. Keep the long-standing 2 GiB Ptex
    // safety cap even when a larger global texture budget is configured.
    texCache.ptexAtlasBudgetBytes = std::min(
        textureBudgetBytes, size_t{2ull * 1024ull * 1024ull * 1024ull});
  }
  texCache.ptexAtlasPerTextureBytes = std::min(
      texCache.ptexAtlasBudgetBytes,
      std::max<size_t>(1024ull * 1024ull,
                       texCache.ptexAtlasBudgetBytes / 8u));
  tydn::TextureDecodeOptions texOpts;
  texOpts.base_dir = lightusd::io::GetBaseDir(path);
  texOpts.max_edge = opts.textureOptions.maxTextureSize > 0
                         ? uint32_t(opts.textureOptions.maxTextureSize)
                         : 0u;
  texOpts.budget_bytes =
      opts.textureOptions.textureBudgetMB > 0
          ? uint64_t(opts.textureOptions.textureBudgetMB) * 1024ull * 1024ull
          : 0ull;
  // Under a --texture-fit threshold the edge cap is off and the byte budget is
  // the only limiter, so a scene that crosses it mid-decode would otherwise
  // fail individual decodes -- i.e. lose textures. A resolution floor makes the
  // budget soft: over-subscribe rather than drop, so the failure mode is
  // "blurrier", never "missing".
  texOpts.min_edge = opts.textureFitThresholdBytes > 0 ? 64u : 0u;
  lightusd::next::USDZReader usdzArchive;
  if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".usdz") == 0 &&
      usdzArchive.OpenFile(path)) {
    texOpts.usdz = &usdzArchive;
  }
  texCache.decoder = std::make_unique<tydn::TextureDecoder>(texOpts);
  texCache.deferOrdinary = opts.asyncTextureDecode &&
                           !opts.forceTextureDecode && !texOpts.usdz;

  // Resolve a material prim path to a DrawScene material index (cached by path).
  // Unbound / unconvertible -> 0 (default gray material).
  draw->optimization.sourceMaterials = draw->materials.size();
  draw->optimization.uniqueMaterials = draw->materials.size();
  std::unordered_map<std::string, int> matIndexByPath;
  std::unordered_map<uint64_t, std::vector<int>> matIndexByContent;
  std::vector<int> materialCanonicalIds;
  auto canonicalMaterialId = [&](int materialId) -> int {
    if (materialId < 0) return -1;
    while (materialCanonicalIds.size() < draw->materials.size()) {
      const int logical = static_cast<int>(materialCanonicalIds.size());
      const DrawMaterialCPU& material =
          draw->materials[static_cast<size_t>(logical)];
      const uint64_t hash = DrawMaterialRenderHash(material);
      int representative = logical;
      auto& candidates = matIndexByContent[hash];
      for (int candidate : candidates) {
        if (DrawMaterialsRenderEquivalent(
                material, draw->materials[static_cast<size_t>(candidate)])) {
          representative = candidate;
          break;
        }
      }
      if (representative == logical) candidates.push_back(logical);
      materialCanonicalIds.push_back(representative);
    }
    return static_cast<size_t>(materialId) < materialCanonicalIds.size()
               ? materialCanonicalIds[static_cast<size_t>(materialId)]
               : materialId;
  };
  // Texture UV routing is mesh-dependent: a shared USD material can request
  // `perfuv`, while different bound meshes may extract that primvar into different
  // DrawVertex slots. Cache the converted GPU material by both its USD path and
  // the mesh UV layout. Caching by path alone made later meshes reuse the first
  // mesh's routing; opacity maps then sampled unrelated coordinates (most visibly
  // on transparent lenses).
  auto resolveMaterialPath = [&](const std::string& mpath,
                                 const std::string& uv0Name,
                                 const std::string& uv1Name) -> int {
    if (mpath.empty()) return 0;
    const std::string cacheKey = mpath + '\x1f' + uv0Name + '\x1f' + uv1Name;
    auto it = matIndexByPath.find(cacheKey);
    if (it != matIndexByPath.end()) return it->second;
    tnext::UsdPrim matPrim = stage.GetPrimAtPath(mpath);
    int idx = matPrim.IsValid() ? BuildNextMaterial(stage, conv, matPrim, draw,
                                                    texCache, uv0Name, uv1Name)
                                : -1;
    if (idx > 0 && static_cast<size_t>(idx) + 1 == draw->materials.size()) {
      ++draw->optimization.sourceMaterials;
      (void)canonicalMaterialId(idx);
    }
    draw->optimization.uniqueMaterials = draw->materials.size();
    if (idx < 0) idx = 0;
    matIndexByPath[cacheKey] = idx;
    return idx;
  };
  // Prototype-material wrapper for EmitInstancedProto (no per-mesh UV-set names
  // for a shared prototype; secondary-UV routing falls back to set 0).
  const std::function<int(const std::string&)> resolveProtoMat =
      [&](const std::string& mpath) -> int {
        return resolveMaterialPath(mpath, std::string(), std::string());
      };

  size_t streamedMeshCount = 0;
  size_t streamedGuideCount = 0;
  size_t streamedProxyCount = 0;
  size_t streamedRenderCount = 0;
  size_t streamedMaterialCount = 0;
  size_t streamedTextureCount = 0;
  size_t streamedTexturePayloadCount = 0;
  size_t streamedPointCount = 0;
  size_t streamedCurveCount = 0;
  size_t streamedPointSamples = 0;
  size_t streamedCurveSamples = 0;
  size_t streamedGaussianChunks = 0;
  size_t streamedGaussianSamples = 0;
  size_t streamedVolumeCount = 0;
  Bounds streamedCarrierBounds;
  Bounds streamedVolumeBounds;
  Bounds streamedTightBounds;
  bool streamOk = true;
  bool loggedFirstProduced = false;
  // Uploading one mesh at a time invalidates the renderer's shared MDI build.
  // Large instanced scenes can therefore rebuild multi-gigabyte buffers once per
  // prototype. Keep the producer-side scene responsive, but publish in bounded
  // mesh batches so the renderer rebuilds MDI substantially less often. The
  // final flush below makes the batching transparent to the completed scene.
  const size_t progressiveMeshBatch = stream ? 512u : 1u;
  auto publishAvailableMeshes = [&](bool force = false) {
    if (!stream || draw->meshes.empty() || !streamOk) return;
    if (!force && draw->meshes.size() < progressiveMeshBatch) return;
    if (streamedMaterialCount != draw->materials.size() ||
        streamedTextureCount != draw->textures.size()) {
      streamOk = stream->pushResources(
          draw->materials, static_cast<int>(draw->textures.size()), draw->upAxis);
      streamedMaterialCount = draw->materials.size();
      streamedTextureCount = draw->textures.size();
      if (!streamOk) return;
    }
    std::vector<DrawMeshCPU> ready;
    ready.swap(draw->meshes);
    for (DrawMeshCPU& dm : ready) {
      if (!loggedFirstProduced && timing) {
        loggedFirstProduced = true;
        LOGI("next timing: first geometry produced %.3f s",
             std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          loadBegin)
                 .count());
      }
      if (dm.purpose == "guide") ++streamedGuideCount;
      else if (dm.purpose == "proxy") ++streamedProxyCount;
      else if (dm.purpose == "render") ++streamedRenderCount;
      if (std::isfinite(dm.aabbMin[0]) && std::isfinite(dm.aabbMax[0]) &&
          dm.aabbMax[0] >= dm.aabbMin[0]) {
        streamedTightBounds.add(dm.aabbMin);
        streamedTightBounds.add(dm.aabbMax);
      }
      ++streamedMeshCount;
      if (!stream->pushMesh(std::move(dm), ctrl ? &ctrl->cancel : nullptr)) {
        streamOk = false;
        break;
      }
    }
  };
  auto publishAvailableNonMeshes = [&]() {
    if (!stream || !streamOk) return;
    if (streamedMaterialCount != draw->materials.size() ||
        streamedTextureCount != draw->textures.size()) {
      streamOk = stream->pushResources(
          draw->materials, static_cast<int>(draw->textures.size()), draw->upAxis);
      streamedMaterialCount = draw->materials.size();
      streamedTextureCount = draw->textures.size();
      if (!streamOk) return;
    }
    std::vector<DrawPointsCPU> points;
    points.swap(draw->points);
    for (DrawPointsCPU& point : points) {
      if (std::isfinite(point.aabbMin[0]) &&
          std::isfinite(point.aabbMax[0])) {
        streamedCarrierBounds.add(point.aabbMin);
        streamedCarrierBounds.add(point.aabbMax);
      }
      ++streamedPointCount;
      streamedPointSamples += point.points.size() / 3;
      if (point.purpose == "guide") ++streamedGuideCount;
      else if (point.purpose == "proxy") ++streamedProxyCount;
      else if (point.purpose == "render") ++streamedRenderCount;
      if (point.gaussian) {
        ++streamedGaussianChunks;
        streamedGaussianSamples += point.points.size() / 3;
      }
      if (!stream->pushPoints(std::move(point), ctrl ? &ctrl->cancel : nullptr)) {
        streamOk = false;
        return;
      }
    }
    std::vector<DrawCurvesCPU> curves;
    curves.swap(draw->curves);
    for (DrawCurvesCPU& curve : curves) {
      if (std::isfinite(curve.aabbMin[0]) &&
          std::isfinite(curve.aabbMax[0])) {
        streamedCarrierBounds.add(curve.aabbMin);
        streamedCarrierBounds.add(curve.aabbMax);
      }
      ++streamedCurveCount;
      streamedCurveSamples += curve.points.size() / 3;
      if (curve.purpose == "guide") ++streamedGuideCount;
      else if (curve.purpose == "proxy") ++streamedProxyCount;
      else if (curve.purpose == "render") ++streamedRenderCount;
      if (!stream->pushCurves(std::move(curve), ctrl ? &ctrl->cancel : nullptr)) {
        streamOk = false;
        return;
      }
    }
  };
  auto publishAvailableTextures = [&]() {
    if (!stream || !streamOk) return;
    if (streamedMaterialCount != draw->materials.size() ||
        streamedTextureCount != draw->textures.size()) {
      streamOk = stream->pushResources(
          draw->materials, static_cast<int>(draw->textures.size()), draw->upAxis);
      streamedMaterialCount = draw->materials.size();
      streamedTextureCount = draw->textures.size();
      if (!streamOk) return;
    }
    for (size_t i = 0; i < draw->textures.size(); ++i) {
      DrawTextureCPU& texture = draw->textures[i];
      const bool hasPayload =
          !texture.image.data.empty() || !texture.mipImages.empty() ||
          !texture.compressed.data.empty() || !texture.compressed.mips.empty() ||
          !texture.udimTiles.empty() || texture.HasPtexSourceData() ||
          texture.streamingMutable;
      if (texture.deferredDecode || !hasPayload) continue;
      if (!stream->pushTexture(static_cast<int>(i), std::move(texture),
                               ctrl ? &ctrl->cancel : nullptr)) {
        streamOk = false;
        return;
      }
      ++streamedTexturePayloadCount;
    }
  };
  std::function<bool(DrawVolumeCPU&&)> publishVolume;
  if (stream) {
    publishVolume = [&](DrawVolumeCPU&& volume) {
      if (std::isfinite(volume.aabbMin[0]) &&
          std::isfinite(volume.aabbMax[0])) {
        streamedVolumeBounds.add(volume.aabbMin);
        streamedVolumeBounds.add(volume.aabbMax);
      }
      ++streamedVolumeCount;
      return stream->pushVolume(std::move(volume),
                                ctrl ? &ctrl->cancel : nullptr);
    };
  }

  // --- 3a. PointInstancer pass: emit one GPU-instanced DrawMeshCPU per prototype
  //         mesh. Prototype geometry lives at the converter's authored location,
  //         so we re-express it relative to the prototype root and bake each
  //         instance's placement into a per-instance model matrix. ---
  std::unordered_set<std::string> consumed;  // proto mesh paths (skip in 3b)
  long long instTotal = 0;
  long long effectiveTris = 0;
  // Optional cap on total emitted instances (VRAM budget / headless software-GL
  // testing). Each instance matrix is 64 B, so e.g. 50M ~= 3.2 GB.
  size_t instBudget = std::numeric_limits<size_t>::max();
  if (opts.gpuGeometryBudgetBytes > 0) {
    instBudget = opts.gpuGeometryBudgetBytes / sizeof(matrix4d);
  }
  if (const char* mc = std::getenv("LUSDVIEW_NEXT_MAX_INSTANCES")) {
    instBudget = std::min(
        instBudget, static_cast<size_t>(std::strtoull(mc, nullptr, 10)));
  }

  std::function<void(const tnext::UsdPrim&)> walk = [&](const tnext::UsdPrim& p) {
    if (!p.IsActive()) return;
    if (p.GetTypeName() == "PointInstancer") {
      double iw16[16];
      tydn::ComputeWorldTransform(stage, p, iw16, time);
      const matrix4d instancer_world = Mat4dFromArray(iw16);

      tydn::ValueArrayRead<float> positions;
      tydn::ReadFloatArray(p, "positions", time, &positions);
      const size_t n = positions.size() / 3;
      tydn::ValueArrayRead<int32_t> protoIdx;
      tydn::ReadIntArray(p, "protoIndices", time, &protoIdx);
      tydn::ValueArrayRead<float> orients;
      tydn::ReadFloatArray(p, "orientations", time, &orients);
      tydn::ValueArrayRead<float> scales;
      tydn::ReadFloatArray(p, "scales", time, &scales);
      tydn::ValueArrayRead<int64_t> invis;
      tydn::ReadInt64Array(p, "invisibleIds", time, &invis);
      tydn::ValueArrayRead<int64_t> inactive;
      tydn::ReadInt64Array(p, "inactiveIds", time, &inactive);
      tydn::ValueArrayRead<int64_t> ids;
      tydn::ReadInt64Array(p, "ids", time, &ids);
      std::unordered_set<int64_t> hiddenSet(invis.begin(), invis.end());
      hiddenSet.insert(inactive.begin(), inactive.end());
      // Optional per-instance displayColor on the instancer (rgb/instance).
      tydn::ValueArrayRead<float> instCol;
      tydn::ReadFloatArray(p, "primvars:displayColor", time, &instCol);
      const bool perInstColor = (instCol.size() == 3 * n && n > 0);
      tydn::ValueArrayRead<float> instOpacity;
      tydn::ReadFloatArray(p, "primvars:displayOpacity", time, &instOpacity);
      const bool perInstOpacity = (instOpacity.size() == n && n > 0);

      const std::vector<tnext::Path>* protos = p.GetRelationship("prototypes");
      if (protos) {
        static const float kIdentQuat[4] = {1, 0, 0, 0};  // real-first (w,x,y,z)
        static const float kUnitScale[3] = {1, 1, 1};
        if (stream) {
          // Publish one bounded preview chunk instead of waiting for every
          // placement of a massive instancer. Remaining placements stay
          // consolidated per prototype, avoiding hundreds of intermediate
          // draws while still exposing first geometry early.
          // Prototype conversion and draw publication have fixed per-chunk
          // costs. Small chunks can repeat them hundreds of times. One
          // million placements bounds temporary doubles to 128 MiB while
          // reducing those costs by ~16x; the final packed carrier is 48 MiB.
          const size_t progressiveInstanceChunk = opts.instanceChunkSamples != 0
              ? opts.instanceChunkSamples
              : std::max<size_t>(
                    1, opts.streamBufferBytes /
                           ((12u + 3u) * sizeof(float)));
          std::vector<tnext::UsdPrim> protoRoots(protos->size());
          std::vector<std::vector<matrix4d>> placementChunks(protos->size());
          std::vector<std::vector<float>> colorChunks;
          if (perInstColor) colorChunks.resize(protos->size());
          std::vector<std::vector<float>> opacityChunks;
          if (perInstOpacity) opacityChunks.resize(protos->size());
          for (size_t pi = 0; pi < protos->size(); ++pi) {
            protoRoots[pi] = stage.GetPrimAtPath((*protos)[pi]);
            if (!protoRoots[pi].IsValid()) continue;
            std::vector<tnext::UsdPrim> protoMeshes;
            tydn::GatherMeshPrims(protoRoots[pi], &protoMeshes);
            for (const tnext::UsdPrim& mp : protoMeshes)
              consumed.insert(mp.GetPath().str());
            placementChunks[pi].reserve(progressiveInstanceChunk);
            if (perInstColor)
              colorChunks[pi].reserve(progressiveInstanceChunk * 3u);
            if (perInstOpacity)
              opacityChunks[pi].reserve(progressiveInstanceChunk);
          }
          auto flushPlacementChunk = [&](size_t pi) {
            if (placementChunks[pi].empty() || !protoRoots[pi].IsValid()) return;
            EmitInstancedProto(
                stage, conv, protoRoots[pi], placementChunks[pi],
                perInstColor ? &colorChunks[pi] : nullptr,
                perInstOpacity ? &opacityChunks[pi] : nullptr, time,
                opts.gpuSkinning, draw, &bounds, &instTotal, &effectiveTris,
                instBudget, &consumed, &resolveProtoMat);
            placementChunks[pi].clear();
            if (perInstColor) colorChunks[pi].clear();
            if (perInstOpacity) opacityChunks[pi].clear();
            publishAvailableMeshes();
          };
          size_t pendingInstances = 0;
          bool previewPublished = false;
          for (size_t i = 0; i < n && streamOk; ++i) {
            if (static_cast<size_t>(instTotal) + pendingInstances >= instBudget)
              break;
            if (PointInstanceHidden(i, n, ids, hiddenSet)) continue;
            const int protoIndex = (i < protoIdx.size()) ? protoIdx[i] : 0;
            if (protoIndex < 0 || protoIndex >= int(protos->size())) continue;
            const size_t pi = static_cast<size_t>(protoIndex);
            if (!protoRoots[pi].IsValid()) continue;
            const float* q =
                (orients.size() >= (i + 1) * 4) ? &orients[i * 4] : kIdentQuat;
            const float* s =
                (scales.size() >= (i + 1) * 3) ? &scales[i * 3] : kUnitScale;
            placementChunks[pi].push_back(
                Mul4(InstanceTRS(&positions[i * 3], q, s), instancer_world));
            if (perInstColor) {
              colorChunks[pi].push_back(instCol[i * 3 + 0]);
              colorChunks[pi].push_back(instCol[i * 3 + 1]);
              colorChunks[pi].push_back(instCol[i * 3 + 2]);
            }
            if (perInstOpacity) opacityChunks[pi].push_back(instOpacity[i]);
            ++pendingInstances;
            if (!previewPublished &&
                placementChunks[pi].size() >= progressiveInstanceChunk) {
              pendingInstances -= placementChunks[pi].size();
              flushPlacementChunk(pi);
              previewPublished = true;
            }
          }
          for (size_t pi = 0; pi < protos->size() && streamOk; ++pi) {
            pendingInstances -= placementChunks[pi].size();
            flushPlacementChunk(pi);
          }
          if (!streamOk) return;
        } else {
        // Generate placements directly into prototype buckets. The small count
        // pass below reserves exact capacities; unlike the previous path it
        // does not retain one uint32 index for every visible instance before
        // revisiting them. At large-scene scale this removes a substantial allocation
        // and its associated random bucket writes.
        std::vector<std::vector<matrix4d>> placementsByProto(protos->size());
        std::vector<std::vector<float>> colorsByProto;
        if (perInstColor) colorsByProto.resize(protos->size());
        std::vector<std::vector<float>> opacitiesByProto;
        if (perInstOpacity) opacitiesByProto.resize(protos->size());
        const size_t remainingInstances =
            static_cast<size_t>(instTotal) < instBudget
                ? instBudget - static_cast<size_t>(instTotal)
                : 0u;
        std::vector<size_t> placementCounts(protos->size(), 0u);
        size_t countedInstances = 0;
        for (size_t i = 0; i < n && countedInstances < remainingInstances; ++i) {
          if (PointInstanceHidden(i, n, ids, hiddenSet)) continue;
          const int pi = (i < protoIdx.size()) ? protoIdx[i] : 0;
          if (pi < 0 || pi >= int(protos->size())) continue;
          ++placementCounts[static_cast<size_t>(pi)];
          ++countedInstances;
        }
        for (size_t pi = 0; pi < protos->size(); ++pi) {
          placementsByProto[pi].reserve(placementCounts[pi]);
          if (perInstColor) colorsByProto[pi].reserve(placementCounts[pi] * 3u);
          if (perInstOpacity) opacitiesByProto[pi].reserve(placementCounts[pi]);
        }
        size_t bucketedInstances = 0;
        for (size_t i = 0; i < n && bucketedInstances < remainingInstances; ++i) {
          if (PointInstanceHidden(i, n, ids, hiddenSet)) continue;
          const int pi = (i < protoIdx.size()) ? protoIdx[i] : 0;
          if (pi < 0 || pi >= int(protos->size())) continue;
          const float* q =
              (orients.size() >= (i + 1) * 4) ? &orients[i * 4] : kIdentQuat;
          const float* s =
              (scales.size() >= (i + 1) * 3) ? &scales[i * 3] : kUnitScale;
          placementsByProto[static_cast<size_t>(pi)].push_back(
              Mul4(InstanceTRS(&positions[i * 3], q, s), instancer_world));
          if (perInstColor) {
            std::vector<float>& colors = colorsByProto[static_cast<size_t>(pi)];
            colors.push_back(instCol[i * 3 + 0]);
            colors.push_back(instCol[i * 3 + 1]);
            colors.push_back(instCol[i * 3 + 2]);
          }
          if (perInstOpacity) {
            opacitiesByProto[static_cast<size_t>(pi)].push_back(instOpacity[i]);
          }
          ++bucketedInstances;
        }

        for (size_t pi = 0; pi < protos->size(); ++pi) {
          tnext::UsdPrim protoRoot = stage.GetPrimAtPath((*protos)[pi]);
          if (!protoRoot.IsValid()) continue;
          // Consume ALL of this prototype's mesh paths (including nested-instancer
          // ones) so the static-batching pass never draws them as base geometry --
          // prototypes can live outside the instancer subtree.
          std::vector<tnext::UsdPrim> protoMeshes;
          tydn::GatherMeshPrims(protoRoot, &protoMeshes);
          for (const tnext::UsdPrim& mp : protoMeshes)
            consumed.insert(mp.GetPath().str());
          if (placementsByProto[pi].empty()) continue;
          // One world placement (+ optional per-instance color) per visible
          // instance; EmitInstancedProto bakes mesh_rel*placement and recurses into
          // any nested instancers under the prototype.
          EmitInstancedProto(stage, conv, protoRoot, placementsByProto[pi],
                             perInstColor ? &colorsByProto[pi] : nullptr,
                             perInstOpacity ? &opacitiesByProto[pi] : nullptr, time,
                             opts.gpuSkinning, draw, &bounds, &instTotal,
                             &effectiveTris, instBudget, &consumed,
                             &resolveProtoMat);
          publishAvailableMeshes();
          if (!streamOk) return;
        }
        }
      }
      // Placements and prototype carriers now own everything needed to render
      // this static instancer. Drop its reconstructable source arrays before
      // processing the next instancer so positions/orientations/scales do not
      // overlap all packed instance buffers at peak residency.
      if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
        session->ReleaseStaticGeometryArraysForPrim(p);
      }
      return;  // do not descend into a PointInstancer's prototypes as geometry
    }
    for (const tnext::UsdPrim& c : p.GetChildren()) walk(c);
  };
  for (const tnext::UsdPrim& r : stage.GetRootPrims()) walk(r);
  publishAvailableMeshes();
  if (!streamOk) {
    if (err) *err = "next: progressive load cancelled";
    return false;
  }
  const auto pointInstancesAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: point-instancer extraction %.3f s",
         std::chrono::duration<double>(pointInstancesAt - composedAt).count());
  if (timing) LogProcessMemory("after point instancers");

  // One inherited-state traversal supplies both ordinary meshes and native
  // instance roots. Previously the native-instance pass independently walked
  // the full 100k+-prim stage, then CollectRenderPrims walked it again.
  tydn::RenderExtractOptions extractOpts;
  extractOpts.time_code = time;
  extractOpts.stop_at_point_instancers = true;
  extractOpts.stop_at_native_instances = true;
  extractOpts.collect_other = true;  // includes UsdGeomPoints in records
  // Points have their own list, and curves have a dedicated list below. Do not
  // retain a second traversal-order record for every prim: on st_main this
  // duplicate was hundreds of thousands of records and added measurable
  // allocation/cache pressure before mesh setup could begin.
  extractOpts.collect_records = false;
  tydn::RenderExtractResult extracted;
  tydn::CollectRenderPrims(stage, extractOpts, &extracted);
  const auto renderTraversalAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: render-stage traversal %.3f s",
         std::chrono::duration<double>(renderTraversalAt - pointInstancesAt)
             .count());

  auto copyChunked = [](const tydn::FloatChunked& src,
                        std::vector<float>* dst) {
    dst->resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) (*dst)[i] = src[i];
  };
  auto addCarrierBounds = [&](const double world[16],
                              const std::vector<float>& points,
                              const std::vector<float>& widths,
                              float outMin[3], float outMax[3]) {
    for (int k = 0; k < 3; ++k) {
      outMin[k] = std::numeric_limits<float>::infinity();
      outMax[k] = -std::numeric_limits<float>::infinity();
    }
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
      float p[3];
      TransformRowPoint(world, points[i], points[i + 1], points[i + 2], p);
      for (int k = 0; k < 3; ++k) {
        outMin[k] = std::min(outMin[k], p[k]);
        outMax[k] = std::max(outMax[k], p[k]);
      }
      bounds.add(p);
    }
    // USD widths are diameters. Expand centerline bounds by the largest
    // world-space radius so auto-frame, depth AOV normalization, and culling do
    // not clip large Points/Curves whose centers have a tiny or zero extent.
    float maxWidth = widths.empty() ? 1.0f : 0.0f;
    for (float width : widths) maxWidth = std::max(maxWidth, std::fabs(width));
    double worldScale = 0.0;
    for (int basis = 0; basis < 3; ++basis) {
      const double x = world[basis * 4 + 0];
      const double y = world[basis * 4 + 1];
      const double z = world[basis * 4 + 2];
      worldScale = std::max(worldScale, std::sqrt(x * x + y * y + z * z));
    }
    const float radius = 0.5f * maxWidth * static_cast<float>(worldScale);
    if (std::isfinite(radius) && radius > 0.0f &&
        std::isfinite(outMin[0]) && std::isfinite(outMax[0])) {
      for (int k = 0; k < 3; ++k) {
        outMin[k] -= radius;
        outMax[k] += radius;
      }
      bounds.add(outMin);
      bounds.add(outMax);
    }
  };

  // Preserve next-core Points without re-reading schema attributes. Rendering
  // backends receive the converter's evaluated widths/colors at `time` plus the
  // inherited world transform and material binding.
  for (const tydn::RenderPrimRecord& rec : extracted.points) {
    const bool gaussian = rec.type_name == "ParticleField3DGaussianSplat";
    if (rec.type_name != "Points" && !gaussian) continue;

    // Gaussian fields are commonly millions of records. Read their numeric
    // arrays as borrowed/lazy views and emit bounded DrawPointsCPU chunks
    // directly. The old ConvertPoints path first copied positions/scales into
    // RenderPoints and then copied them again into the carrier, creating a
    // large transient peak and one monolithic record.
    if (gaussian) {
      tnext::ParticleFieldData field;
      std::string fieldWarning;
      if (!tnext::GetParticleFieldData(stage, rec.prim, &field, time,
                                       &fieldWarning)) {
        draw->skipped.push_back("GaussianSplat '" + rec.path +
                                "': invalid ParticleField schema data");
        continue;
      }
      if (!fieldWarning.empty())
        LOGW("GaussianSplat '%s': %s", rec.path.c_str(), fieldWarning.c_str());
      tydn::ValueArrayRead<float> positions;
      tydn::ValueArrayRead<float> scales;
      if (field.positions_property.empty() || field.scales_property.empty() ||
          !tydn::ReadFloatArray(rec.prim, field.positions_property.c_str(), time,
                                &positions) ||
          !tydn::ReadFloatArray(rec.prim, field.scales_property.c_str(), time,
                                &scales) ||
          positions.size() < 3 || scales.size() < 3) {
        draw->skipped.push_back("GaussianSplat '" + rec.path +
                                "': missing/invalid positions or scales");
        continue;
      }
      tydn::ValueArrayRead<float> orientations;
      tydn::ValueArrayRead<float> opacities;
      tydn::ValueArrayRead<float> sh;
      const bool haveOrientations = !field.orientations_property.empty() &&
          tydn::ReadFloatArray(rec.prim, field.orientations_property.c_str(), time,
                               &orientations);
      const bool haveOpacities = !field.opacities_property.empty() &&
          tydn::ReadFloatArray(rec.prim, field.opacities_property.c_str(), time,
                               &opacities);
      // Only the first three (DC RGB) coefficients are used by the preview.
      // A compressed crate-backed SH array otherwise forces a full decode just
      // to obtain those three values per splat, creating a large transient
      // allocation on top of the positions/scales carriers.  Keep SH for
      // directly borrowable/non-lazy values; use the neutral fallback color
      // when decoding the optional high-order payload would exceed the loader's
      // memory budget.
      bool allowSh = true;
      if (!field.spherical_harmonics_property.empty()) {
        const tnext::Value* shValue = rec.prim.GetPropertyValue(
            field.spherical_harmonics_property);
        if (shValue) {
          constexpr size_t kMaxDecodedShBytes = size_t(128) * 1024 * 1024;
          const size_t shElements = shValue->array_size();
          const bool oversized =
              shElements > kMaxDecodedShBytes / sizeof(float);
          allowSh = !oversized || !shValue->is_lazy() ||
                    tnext::CanBorrowLazyFlat(*shValue);
        }
      }
      const bool haveSh = allowSh &&
          !field.spherical_harmonics_property.empty() &&
          tydn::ReadFloatArray(rec.prim,
                               field.spherical_harmonics_property.c_str(), time,
                               &sh);
      if (!allowSh) {
        LOGI("GaussianSplat '%s': skipping compressed SH decode; using DC fallback",
             rec.path.c_str());
      }
      const size_t n = std::min(positions.size() / 3, scales.size() / 3);
      const size_t chunkSize = opts.pointChunkSamples != 0
                                   ? opts.pointChunkSamples
                                   : size_t(64) * 1024;
      const size_t shStride = (haveSh && sh.size() >= n * 3) ? sh.size() / n : 0;
      const bool haveQ = haveOrientations && orientations.size() >= n * 4;
      double world[16];
      if (!tydn::ComputeWorldTransform(stage, rec.prim, world, time)) {
        std::memcpy(world, rec.world, sizeof(world));
      }
      size_t emitted = 0;
      for (size_t first = 0; first < n; first += chunkSize) {
        const size_t last = std::min(n, first + chunkSize);
        DrawPointsCPU dp;
        dp.name = rec.prim.GetName();
        dp.absPath = rec.path;
        dp.purpose = rec.purpose;
        dp.materialId = resolveMaterialPath(rec.material_path, std::string(),
                                            std::string());
        dp.gaussian = true;
        dp.colorsInterpolation = 1;   // vertex/per-point
        dp.opacitiesInterpolation = 1;
        dp.points.reserve((last - first) * 3);
        dp.widths.reserve(last - first);
        dp.colors.reserve((last - first) * 3);
        dp.opacities.reserve(last - first);
        dp.ellipseRadii.reserve((last - first) * 2);
        dp.ellipseNormals.reserve((last - first) * 3);
        dp.ellipseMajorAxes.reserve((last - first) * 3);
        for (size_t i = first; i < last; ++i) {
          const float opacity =
              (haveOpacities && opacities.size() > 1 && i < opacities.size())
                  ? opacities.begin()[i]
                  : (haveOpacities && opacities.size() == 1
                         ? opacities.begin()[0]
                         : 1.0f);
          const float sx = std::fabs(scales.begin()[i * 3]);
          const float sy = std::fabs(scales.begin()[i * 3 + 1]);
          const float sz = std::fabs(scales.begin()[i * 3 + 2]);
          const float px = positions.begin()[i * 3];
          const float py = positions.begin()[i * 3 + 1];
          const float pz = positions.begin()[i * 3 + 2];
          if (!std::isfinite(opacity) || opacity < 0.01f ||
              !std::isfinite(sx) || !std::isfinite(sy) ||
              !std::isfinite(sz) || sx <= 1.0e-8f || sy <= 1.0e-8f ||
              !std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz))
            continue;
          lightusd::value::quatf q;
          q.real = haveQ ? orientations.begin()[i * 4] : 1.0f;
          q.imag[0] = haveQ ? orientations.begin()[i * 4 + 1] : 0.0f;
          q.imag[1] = haveQ ? orientations.begin()[i * 4 + 2] : 0.0f;
          q.imag[2] = haveQ ? orientations.begin()[i * 4 + 3] : 0.0f;
          const auto r = lightusd::to_matrix3x3(q);
          dp.points.insert(dp.points.end(), {px, py, pz});
          dp.widths.push_back(2.0f * std::max(sx, std::max(sy, sz)));
          dp.ellipseRadii.insert(dp.ellipseRadii.end(), {2.0f * sx, 2.0f * sy});
          dp.ellipseMajorAxes.insert(
              dp.ellipseMajorAxes.end(),
              {float(r.m[0][0]), float(r.m[0][1]), float(r.m[0][2])});
          dp.ellipseNormals.insert(
              dp.ellipseNormals.end(),
              {float(r.m[2][0]), float(r.m[2][1]), float(r.m[2][2])});
          if (shStride != 0 && i * shStride + 2 < sh.size()) {
            const float* c = sh.begin() + i * shStride;
            dp.colors.insert(dp.colors.end(), {
                std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * c[0])),
                std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * c[1])),
                std::max(0.0f, std::min(1.0f, 0.5f + 0.2820948f * c[2]))});
          } else {
            dp.colors.insert(dp.colors.end(), {0.72f, 0.72f, 0.72f});
          }
          dp.opacities.push_back(opacity);
        }
        if (dp.points.empty()) continue;
        RowMatrixToColumnMajor(world, dp.world);
        addCarrierBounds(rec.world, dp.points, dp.widths, dp.aabbMin, dp.aabbMax);
        emitted += dp.points.size() / 3;
        draw->points.push_back(std::move(dp));
        publishAvailableNonMeshes();
        if (!streamOk) break;
      }
      if (emitted == 0)
        draw->skipped.push_back("GaussianSplat '" + rec.path +
                                "': no finite visible samples");
      continue;
    }

    // Ordinary Points use the same lazy-array path as Gaussian splats. Going
    // through RenderPoints here would materialize the complete prim and then
    // copy it again into DrawPointsCPU, which is a significant transient peak
    // for particle fields that are not Gaussian schemas.
    tydn::ValueArrayRead<float> points;
    if (!tydn::ReadFloatArray(rec.prim, "points", time, &points) ||
        points.empty() || (points.size() % 3) != 0) {
      draw->skipped.push_back("Points '" + rec.path +
                             "': missing/invalid points data");
      continue;
    }
    const size_t n = points.size() / 3;
    tydn::ValueArrayRead<float> normals;
    const bool haveNormals =
        tydn::ReadFloatArray(rec.prim, "normals", time, &normals) &&
        normals.size() == n * 3;
    if (!haveNormals && !normals.empty()) {
      draw->skipped.push_back("Points '" + rec.path +
                             "': ignoring mismatched normals");
    }
    tydn::ValueArrayRead<float> widths;
    const bool haveWidths =
        tydn::ReadFloatArray(rec.prim, "widths", time, &widths) &&
        (widths.size() == 1 || widths.size() == n);
    if (!haveWidths && !widths.empty()) {
      draw->skipped.push_back("Points '" + rec.path +
                             "': ignoring mismatched widths");
    }

    auto readPointAttribute = [&](const char* name, size_t components,
                                  tydn::ValueArrayRead<float>* value,
                                  int* interpolation) -> bool {
      if (!tydn::ReadFloatArray(rec.prim, name, time, value) ||
          value->empty() || value->size() % components != 0) {
        return false;
      }
      const size_t elems = value->size() / components;
      std::string interpTok;
      if (const tnext::PrimSpec* spec = rec.prim.GetPrimSpec()) {
        if (const tnext::PropMeta* pm = spec->property_meta(name)) {
          if (pm->authored & tnext::PropMeta::kInterpolation)
            interpTok = pm->interpolation;
        }
      }
      const bool constant = interpTok == "constant" ||
                            (interpTok.empty() && elems == 1);
      if (constant && elems == 1) {
        *interpolation = 0;
        return true;
      }
      if (!constant && elems == n) {
        *interpolation = 1;
        return true;
      }
      return false;
    };

    tydn::ValueArrayRead<float> colors, opacities;
    int colorsInterpolation = 0;
    int opacitiesInterpolation = 0;
    const bool haveColors = readPointAttribute(
        "primvars:displayColor", 3, &colors, &colorsInterpolation);
    const bool haveOpacities = readPointAttribute(
        "primvars:displayOpacity", 1, &opacities, &opacitiesInterpolation);
    if (!haveColors && !colors.empty())
      draw->skipped.push_back("Points '" + rec.path +
                             "': ignoring mismatched displayColor");
    if (!haveOpacities && !opacities.empty())
      draw->skipped.push_back("Points '" + rec.path +
                             "': ignoring mismatched displayOpacity");

    const size_t chunkSize = opts.pointChunkSamples != 0
                                 ? opts.pointChunkSamples
                                 : size_t(64) * 1024;
    const int materialId = resolveMaterialPath(
        rec.material_path, std::string(), std::string());
    size_t emitted = 0;
    for (size_t first = 0; first < n; first += chunkSize) {
      const size_t last = std::min(n, first + chunkSize);
      DrawPointsCPU dp;
      dp.name = rec.prim.GetName();
      dp.absPath = rec.path;
      dp.purpose = rec.purpose;
      dp.materialId = materialId;
      dp.colorsInterpolation = colorsInterpolation;
      dp.opacitiesInterpolation = opacitiesInterpolation;
      dp.points.insert(dp.points.end(), points.begin() + first * 3,
                       points.begin() + last * 3);
      if (haveNormals)
        dp.normals.insert(dp.normals.end(), normals.begin() + first * 3,
                          normals.begin() + last * 3);
      if (haveWidths) {
        if (widths.size() == 1)
          dp.widths.push_back(widths.begin()[0]);
        else
          dp.widths.insert(dp.widths.end(), widths.begin() + first,
                           widths.begin() + last);
      }
      if (haveColors) {
        if (colorsInterpolation == 0)
          dp.colors.insert(dp.colors.end(), colors.begin(), colors.begin() + 3);
        else
          dp.colors.insert(dp.colors.end(), colors.begin() + first * 3,
                           colors.begin() + last * 3);
      }
      if (haveOpacities) {
        if (opacitiesInterpolation == 0)
          dp.opacities.push_back(opacities.begin()[0]);
        else
          dp.opacities.insert(dp.opacities.end(), opacities.begin() + first,
                              opacities.begin() + last);
      }
      RowMatrixToColumnMajor(rec.world, dp.world);
      addCarrierBounds(rec.world, dp.points, dp.widths, dp.aabbMin,
                       dp.aabbMax);
      emitted += last - first;
      draw->points.push_back(std::move(dp));
      publishAvailableNonMeshes();
      if (!streamOk) break;
    }
    if (emitted == 0)
      draw->skipped.push_back("Points '" + rec.path + "': no samples");
  }

  // RenderCurves already contains tessellated centerlines for Basis, NURBS and
  // Hermite schemas. Retain those samples and their interpolated widths/colors;
  // linear/control data is used only when tessellation produced no samples.
  size_t curvePrimsConverted = 0;
  size_t curvePrimsDeferred = 0;
  size_t curveStrandsRetained = 0;
  size_t curveStrandsDeferred = 0;
  const unsigned carrierThreads = opts.conversionThreads
      ? opts.conversionThreads
      : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  const size_t curveParallelMinPrims = opts.curveParallelMinPrims != 0
      ? opts.curveParallelMinPrims : std::max<size_t>(2, carrierThreads);
  const bool parallelCurves = carrierThreads > 1 &&
      extracted.curves.size() >= curveParallelMinPrims && opts.maxCurvePrims == 0 &&
      opts.maxCurveStrands == 0;
  std::vector<std::unique_ptr<tydn::RenderCurves>> convertedCurves(
      extracted.curves.size());
  std::vector<std::string> curveErrors(extracted.curves.size());
  if (parallelCurves) {
    std::atomic<size_t> nextCurve{0};
    std::vector<std::thread> workers;
    workers.reserve(carrierThreads);
    for (unsigned t = 0; t < carrierThreads; ++t) {
      workers.emplace_back([&, t]() {
        (void)t;
        tydn::RenderSceneConverter workerConv(cfg);
        for (;;) {
          const size_t i = nextCurve.fetch_add(1);
          if (i >= extracted.curves.size()) break;
          const tydn::RenderPrimRecord& rec = extracted.curves[i];
          bool hasClipOwner = false;
          for (tnext::UsdPrim owner = rec.prim; owner.IsValid();
               owner = owner.GetParent()) {
            if (owner.GetPrimSpec() &&
                owner.GetPrimSpec()->meta().clips().is_dictionary()) {
              hasClipOwner = true;
              break;
            }
          }
          if (!rec.prim.HasAuthoredProperty("points") && !hasClipOwner) continue;
          auto result = std::make_unique<tydn::RenderCurves>();
          if (workerConv.ConvertCurves(rec.prim, result.get())) {
            convertedCurves[i] = std::move(result);
          } else {
            curveErrors[i] = workerConv.GetLastError();
          }
        }
      });
    }
    for (std::thread& worker : workers) worker.join();
  }
  for (size_t curveIndex = 0; curveIndex < extracted.curves.size();
       ++curveIndex) {
    const tydn::RenderPrimRecord& rec = extracted.curves[curveIndex];
    if (opts.maxCurvePrims > 0 &&
        curvePrimsConverted >= opts.maxCurvePrims) {
      ++curvePrimsDeferred;
      continue;
    }
    if (opts.maxCurveStrands > 0 &&
        curveStrandsRetained >= opts.maxCurveStrands) {
      ++curvePrimsDeferred;
      continue;
    }
    tydn::RenderCurves rc;
    // Some procedural exports leave a render-prim placeholder with authored
    // curveVertexCounts but no authored points. The points property reported
    // by the schema is only a fallback declaration; without points or a clip
    // owner there is no drawable geometry and no conversion error to report.
    bool hasClipOwner = false;
    for (tnext::UsdPrim owner = rec.prim; owner.IsValid();
         owner = owner.GetParent()) {
      if (owner.GetPrimSpec() &&
          owner.GetPrimSpec()->meta().clips().is_dictionary()) {
        hasClipOwner = true;
        break;
      }
    }
    if (!rec.prim.HasAuthoredProperty("points") && !hasClipOwner) continue;
    bool convertedCurve = false;
    if (parallelCurves) {
      if (convertedCurves[curveIndex]) {
        rc = std::move(*convertedCurves[curveIndex]);
        convertedCurves[curveIndex].reset();
        convertedCurve = true;
      }
    } else {
      convertedCurve = conv.ConvertCurves(rec.prim, &rc);
    }
    if (!convertedCurve) {
      std::string reason = parallelCurves ? curveErrors[curveIndex]
                                          : conv.GetLastError();
      draw->skipped.push_back("Curves '" + rec.path + "': conversion failed" +
                              (reason.empty() ? std::string()
                                              : ": " + reason));
      continue;
    }
    DrawCurvesCPU dc;
    dc.name = rc.name;
    dc.absPath = rec.path;
    dc.purpose = rec.purpose;
    dc.materialId = resolveMaterialPath(rec.material_path, std::string(),
                                        std::string());
    if (!rc.tessellated_points.empty()) {
      dc.vertexCounts = std::move(rc.tessellated_vertex_counts);
      copyChunked(rc.tessellated_points, &dc.points);
      copyChunked(rc.tessellated_widths, &dc.widths);
      if (dc.widths.empty() && !rc.widths.empty()) {
        copyChunked(rc.widths, &dc.widths);
      }
      copyChunked(rc.tessellated_colors, &dc.colors);
      copyChunked(rc.tessellated_opacities, &dc.opacities);
      if (dc.opacities.empty() && !rc.opacities.empty()) {
        copyChunked(rc.opacities, &dc.opacities);
      }
    } else {
      dc.vertexCounts = std::move(rc.curve_vertex_counts);
      copyChunked(rc.points, &dc.points);
      copyChunked(rc.widths, &dc.widths);
      copyChunked(rc.colors, &dc.colors);
      copyChunked(rc.opacities, &dc.opacities);
    }
    // DrawCurvesCPU is the contiguous compatibility carrier consumed by the
    // existing GL/Vulkan/RT paths. Release the converter's chunk-backed source
    // immediately after copying so strand limiting or later carriers do not
    // retain both representations longer than necessary.
    rc = tydn::RenderCurves{};
    if (dc.points.empty()) {
      draw->skipped.push_back("Curves '" + rec.path + "': empty centerline");
      continue;
    }
    ++curvePrimsConverted;
    if (opts.maxCurveStrands > 0 && !dc.vertexCounts.empty()) {
      const size_t remaining = opts.maxCurveStrands - curveStrandsRetained;
      if (dc.vertexCounts.size() > remaining) {
        const std::vector<uint32_t> sourceCounts = dc.vertexCounts;
        const size_t sourcePointCount = dc.points.size() / 3;
        const size_t stride =
            std::max<size_t>(1, (sourceCounts.size() + remaining - 1) /
                                    std::max<size_t>(remaining, 1));
        std::vector<size_t> offsets(sourceCounts.size() + 1, 0);
        for (size_t i = 0; i < sourceCounts.size(); ++i)
          offsets[i + 1] = offsets[i] + sourceCounts[i];
        std::vector<size_t> selected;
        for (size_t i = 0; i < sourceCounts.size() && selected.size() < remaining;
             i += stride) {
          selected.push_back(i);
        }
        auto sampleAttribute = [&](const std::vector<float>& source, size_t comps,
                                   std::vector<float>* sampled) {
          if (source.empty()) return;
          if (source.size() == comps) {
            *sampled = source;
            return;
          }
          const bool perPoint = source.size() == sourcePointCount * comps;
          const bool perCurve = source.size() == sourceCounts.size() * comps;
          if (!perPoint && !perCurve) return;
          for (size_t curve : selected) {
            const size_t begin = perPoint ? offsets[curve] : curve;
            const size_t count = perPoint ? sourceCounts[curve] : 1;
            sampled->insert(sampled->end(), source.begin() + begin * comps,
                            source.begin() + (begin + count) * comps);
          }
        };
        DrawCurvesCPU sampled;
        sampled.name = dc.name;
        sampled.absPath = dc.absPath;
        sampled.purpose = dc.purpose;
        sampled.materialId = dc.materialId;
        std::memcpy(sampled.world, dc.world, sizeof(sampled.world));
        for (size_t curve : selected) {
          sampled.vertexCounts.push_back(sourceCounts[curve]);
          sampled.points.insert(sampled.points.end(),
                                dc.points.begin() + offsets[curve] * 3,
                                dc.points.begin() + offsets[curve + 1] * 3);
        }
        sampleAttribute(dc.widths, 1, &sampled.widths);
        sampleAttribute(dc.colors, 3, &sampled.colors);
        sampleAttribute(dc.opacities, 1, &sampled.opacities);
        curveStrandsDeferred += sourceCounts.size() - selected.size();
        dc = std::move(sampled);
      }
    }
    curveStrandsRetained += dc.vertexCounts.size();
    RowMatrixToColumnMajor(rec.world, dc.world);
    addCarrierBounds(rec.world, dc.points, dc.widths, dc.aabbMin, dc.aabbMax);
    draw->curves.push_back(std::move(dc));
    publishAvailableNonMeshes();
    if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
      session->ReleaseStaticGeometryArraysForPrim(rec.prim);
    }
    if (!streamOk) break;
  }
  if (curvePrimsDeferred > 0 || curveStrandsDeferred > 0) {
    draw->truncated = true;
    draw->skipped.push_back(
        "procedural preview deferred " + std::to_string(curvePrimsDeferred) +
        " Curves prim(s) and " + std::to_string(curveStrandsDeferred) +
        " strand(s); request full quality to refine");
    if (timing) {
      LOGI("next: procedural preview retained %zu Curves prim(s) / %zu "
           "strand(s), deferred %zu prim(s) / %zu strand(s)",
           curvePrimsConverted, curveStrandsRetained, curvePrimsDeferred,
           curveStrandsDeferred);
    }
  }
  const auto carrierGeometryAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: points/curves conversion %.3f s",
         std::chrono::duration<double>(carrierGeometryAt - renderTraversalAt)
             .count());

  // --- 3a-native. Scenegraph (instanceable) instances: prims that share an
  //     instance_prototype are flattened by the converter (one mesh set per
  //     instance). Group them and GPU-instance the prototype's geometry instead;
  //     the prototype prim itself still renders via 3b. ---
  {
    std::unordered_map<std::string, std::vector<matrix4d>> nativeGroups;
    std::vector<std::string> nativeOrder;
    for (const tydn::RenderPrimRecord& rec : extracted.native_instances) {
      const tnext::UsdPrim& p = rec.prim;
      const auto* s = p.GetPrimSpec();
      if (s && !s->meta().instance_prototype().empty()) {
        const std::string& prototype = s->meta().instance_prototype();
        auto inserted = nativeGroups.emplace(
            prototype, std::vector<matrix4d>());
        if (inserted.second) nativeOrder.push_back(prototype);
        inserted.first->second.push_back(Mat4dFromArray(rec.world));
        // CollectRenderPrims stops at native-instance roots, so their proxy
        // descendants are already absent from extracted.meshes. Walking every
        // instance subtree merely to add paths that can never be consumed made
        // heavily-instanced scenes traverse the shared prototype thousands of
        // times.
      }
    }

    if (stream) {
      std::stable_sort(nativeOrder.begin(), nativeOrder.end(),
                       [&](const std::string& a, const std::string& b) {
                         return nativeGroups.at(a).size() >
                                nativeGroups.at(b).size();
                       });
    }
    for (const std::string& prototypePath : nativeOrder) {
      const std::vector<matrix4d>& groupPlacements =
          nativeGroups.at(prototypePath);
      tnext::UsdPrim protoRoot = stage.GetPrimAtPath(prototypePath);
      if (!protoRoot.IsValid()) continue;
      // The prototype of a native-instance group is ITSELF one of the authored
      // instanceable prims (the pcp cache designates the first sibling and points
      // the others at it), so it needs its own placement here -- without this, one
      // instance (all of them, for a 2-instance group) silently vanished.
      //
      // Consume the PROTOTYPE's mesh paths too. An instance's children resolve to
      // the instance's own paths (pcp remaps prototype_root -> instance_root), so
      // the loop above consumed those, not these. EmitInstancedProto draws the
      // prototype's geometry at EVERY placement below, including the prototype
      // prim's own, so leaving its paths unconsumed makes the static-batching pass
      // in 3b draw that same geometry a second time as a standalone mesh.
      std::vector<tnext::UsdPrim> pms;
      tydn::GatherMeshPrims(protoRoot, &pms);
      for (const tnext::UsdPrim& m : pms) consumed.insert(m.GetPath().str());

      std::vector<matrix4d> placements;
      placements.reserve(groupPlacements.size() + 1);
      double pw16[16];
      tydn::ComputeWorldTransform(stage, protoRoot, pw16, time);
      placements.push_back(Mat4dFromArray(pw16));
      placements.insert(placements.end(), groupPlacements.begin(),
                        groupPlacements.end());
      // GPU-instance the prototype's geometry at each placement; EmitInstancedProto
      // recurses into any nested instancers under the prototype.
      EmitInstancedProto(stage, conv, protoRoot, placements,
                         /*placementColors=*/nullptr,
                         /*placementOpacities=*/nullptr, time, opts.gpuSkinning, draw,
                         &bounds, &instTotal, &effectiveTris, instBudget,
                         /*consumed=*/nullptr, &resolveProtoMat);
      publishAvailableMeshes();
      if (!streamOk) break;
    }
  }
  publishAvailableMeshes();
  if (!streamOk) {
    if (err) *err = "next: progressive load cancelled";
    return false;
  }
  const auto nativeInstancesAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: native-instance extraction %.3f s",
         std::chrono::duration<double>(nativeInstancesAt - carrierGeometryAt)
             .count());
  if (timing) LogProcessMemory("after native instances");

  tnext::Stage::StaticGeometryReleaseStats incrementalReleased;
  size_t stageBytesBeforeRelease = 0;
  if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
    stageBytesBeforeRelease = session->GetMemoryStats().composed_stage_bytes;
    // These prototype meshes were fully converted into instanced DrawMeshCPU
    // objects above and are explicitly excluded from the ordinary mesh pass.
    // Retire their stage arrays now instead of carrying them through all static
    // conversion. Duplicate/proxy paths are harmless: a second release is a no-op.
    for (const std::string& consumedPath : consumed) {
      tnext::UsdPrim prim = stage.GetPrimAtPath(consumedPath);
      if (!prim.IsValid()) continue;
      const tnext::Stage::StaticGeometryReleaseStats one =
          session->ReleaseStaticGeometryArraysForPrim(prim);
      incrementalReleased.property_count += one.property_count;
      incrementalReleased.element_count += one.element_count;
      incrementalReleased.estimated_payload_bytes +=
          one.estimated_payload_bytes;
    }
  }

  // --- 3b. Non-instanced meshes: STATIC BATCHING. Each mesh's vertices are baked
  //         to world space and merged into a few big buffers keyed by (purpose,
  //         geometric-normal), so a 33k-mesh scene draws in a handful of calls
  //         (one VAO/VBO/EBO per batch) instead of 33k -- far less draw-call + GL
  //         object overhead. Purpose stays per-batch so the GUI toggles still
  //         work; per-mesh pick/hide is not a goal of the flat large-scene path.
  // Light-link collections are the exception: fragments from two source prims
  // with different membership cannot share one raster draw. Detect that rare
  // case up front and add a per-source id to the batch key below. Scenes without
  // authored links retain the large-scene batching behavior unchanged.
  bool hasAuthoredLightLinks = false;
  std::function<void(const tnext::UsdPrim&)> findLightLinks =
      [&](const tnext::UsdPrim& prim) {
        if (prim.HasProperty("collection:lightLink:includes") ||
            prim.HasProperty("collection:lightLink:excludes") ||
            prim.HasProperty("collection:lightLink:membershipExpression")) {
          hasAuthoredLightLinks = true;
          return;
        }
        for (const tnext::UsdPrim& child : prim.GetChildren()) {
          if (!hasAuthoredLightLinks) findLightLinks(child);
        }
      };
  for (const tnext::UsdPrim& root : stage.GetRootPrims()) {
    if (!hasAuthoredLightLinks) findLightLinks(root);
  }
  struct Batch {
    DrawMeshCPU dm;
    // Avoid rescanning every previously appended triangle to allocate a
    // mesh-local face-id range. Large flattened scenes otherwise turn this
    // bookkeeping into quadratic work.
    uint32_t nextFaceId = 0;
    bool anyColor = false;
    bool anyAlpha = false;
    // Same deal for the SECONDARY UV set: a batch gains a uv1 buffer the first
    // time a mesh that has one joins it. uv0 rides inside DrawVertex, but uv1 is
    // a parallel array, so it has to be appended by hand or the batched draw ends
    // up with an EMPTY uv1 -- and every texture routed to the second UV set then
    // samples a constant instead of the crop it asked for.
    bool anyUv1 = false;
    // A batch gains skin attributes the first time a SKINNED mesh joins it; the
    // vertices already in it (and every unskinned mesh that joins later) get
    // zero weights, which the vertex shader passes through unskinned. Joint
    // indices are absolute bone-texture rows, so one batch can draw vertices
    // posed by several different skeletons.
    bool anySkin = false;
    // True when a SKINNED mesh joined this batch under CPU skinning: its vertices
    // are one baked pose, so they do not bound the rig over the animation (the GPU
    // path signals the same thing through anySkin).
    bool anyCpuSkin = false;
    bool anyExtendedSkin = false;
    // A morphed mesh gets a batch to ITSELF (the key carries a unique id), so the
    // batch's channel ids and its bound SkelAnimation are unambiguous.
    bool anyMorph = false;
    bool animatedWorld = false;
    int matId = 0;
    int backMatId = -1;
  };
  auto appendLogicalSubmesh = [](Batch* batch, uint32_t indexOffset,
                                 uint32_t indexCount, int materialId,
                                 int backMaterialId) {
    if (!batch || indexCount == 0) return;
    if (!batch->dm.submeshes.empty()) {
      DrawSubmesh& tail = batch->dm.submeshes.back();
      if (tail.materialId == materialId &&
          tail.backfaceMaterialId == backMaterialId &&
          tail.indexOffset + tail.indexCount == indexOffset) {
        tail.indexCount += indexCount;
        return;
      }
    }
    batch->dm.submeshes.push_back(
        DrawSubmesh{indexOffset, indexCount, materialId, backMaterialId});
  };
  // key = (purpose, geometricNormal, proxyFallback, doubleSided, front/back
  // material, morphId)
  // -> current
  // batch. Keying by material keeps per-material draws distinct so each batch can
  // reference its own DrawMaterialCPU instead of the single default gray material.
  // doubleSided is per-DrawMeshCPU (it drives back-face culling), so single- and
  // double-sided meshes must not merge. morphId is 0 for ordinary meshes and
  // unique per BLENDSHAPED mesh: morph channel ids and the bound SkelAnimation
  // are per-mesh, so two morphed meshes must not share a batch.
  using BatchKey =
      std::tuple<std::string, bool, bool, bool, int, int, int, int, int>;
  struct BatchKeyHash {
    size_t operator()(const BatchKey& key) const {
      size_t h = std::hash<std::string>{}(std::get<0>(key));
      auto mix = [&h](size_t v) {
        h ^= v + static_cast<size_t>(0x9e3779b9) + (h << 6) + (h >> 2);
      };
      mix(std::hash<bool>{}(std::get<1>(key)));
      mix(std::hash<bool>{}(std::get<2>(key)));
      mix(std::hash<bool>{}(std::get<3>(key)));
      mix(std::hash<int>{}(std::get<4>(key)));
      mix(std::hash<int>{}(std::get<5>(key)));
      mix(std::hash<int>{}(std::get<6>(key)));
      mix(std::hash<int>{}(std::get<7>(key)));
      mix(std::hash<int>{}(std::get<8>(key)));
      return h;
    }
  };
  std::unordered_map<BatchKey, Batch, BatchKeyHash> open;
  std::vector<BatchKey> batchOrder;
  auto getBatch = [&](BatchKey key) -> Batch& {
    auto result = open.emplace(std::move(key), Batch{});
    if (result.second) batchOrder.push_back(result.first->first);
    return result.first->second;
  };
  int nextMorphBatchId = 0;
  int nextLightLinkBatchId = 0;
  int nextAnimatedWorldBatchId = 0;

  // Full UsdShade binding semantics: the purpose fallback chain
  // (material:binding:preview -> material:binding -> material:binding:full) AND
  // inheritance from ancestors. Production scenes may bind purpose-scoped on
  // an ancestor Xform and never author a plain `material:binding` on the Mesh —
  // reading only the Mesh's own `material:binding` dropped every material (and
  // so every texture) on those scenes.
  // A clone of material `base` with its alpha modulated, made once per distinct
  // (material, opacity) pair. Lets a mesh's `displayOpacity` render through the
  // existing material alpha without mutating a material other meshes share.
  std::map<std::pair<int, int>, int> matAlphaVariants;
  int displayColorFallbackMaterial = -1;
  size_t varyingOpacityMeshes = 0;
  // Back-purpose bindings are usually authored on an ancestor. Cache the
  // inherited result per parent path so sibling meshes do not each walk the
  // entire stage ancestry during serial batching.
  std::unordered_map<std::string, std::string> backMaterialByParent;
  auto cachedBackMaterialPath = [&](const tnext::UsdPrim& prim) -> std::string {
    const std::vector<tnext::Path>* local =
        prim.GetRelationship("material:binding:back");
    if (local && !local->empty()) {
      return tnext::GetInheritedBoundMaterialPathForPurpose(
          stage, prim.GetPath().str(), "back");
    }
    const tnext::UsdPrim parent = prim.GetParent();
    if (!parent.IsValid()) return {};
    const std::string parentPath = parent.GetPath().str();
    auto it = backMaterialByParent.find(parentPath);
    if (it != backMaterialByParent.end()) return it->second;
    const std::string value =
        tnext::GetInheritedBoundMaterialPathForPurpose(stage, parentPath,
                                                       "back");
    backMaterialByParent.emplace(parentPath, value);
    return value;
  };
  auto displayColorMaterial = [&]() -> int {
    if (displayColorFallbackMaterial >= 0) {
      return displayColorFallbackMaterial;
    }
    DrawMaterialCPU pass = draw->materials[0];
    pass.name = "displayColorFallback";
    pass.displayName = pass.name;
    pass.baseColor[0] = pass.baseColor[1] = pass.baseColor[2] = 1.0f;
    // Storm/usdview gives unbound displayColor geometry a visible dielectric
    // highlight. The generic default material's 0.5 roughness makes large DCC
    // displayColor scenes look uniformly matte, even though authored Preview
    // Surface materials already retain their own roughness/specular controls.
    // Apply a moderately glossy neutral fallback only to this unbound path.
    pass.metallic = 0.0f;
    pass.roughness = 0.32f;
    pass.ior = 1.5f;
    pass.useSpecularWorkflow = false;
    pass.specularColor[0] = pass.specularColor[1] = pass.specularColor[2] = 1.0f;
    // This is a neutral multiplier, not an authored surface shader. Leaving
    // hasLightRtOpenPBR false makes every backend use the compact white
    // material times displayColor; marking it as a full OpenPBR block caused
    // the raster shaders to take the synthetic evaluator path and turn an
    // otherwise unlit displayColor mesh black.
    pass.hasLightRtOpenPBR = false;
    displayColorFallbackMaterial = static_cast<int>(draw->materials.size());
    draw->materials.push_back(std::move(pass));
    return displayColorFallbackMaterial;
  };
  auto materialWithAlpha = [&](int base, float alpha) -> int {
    if (base < 0 || static_cast<size_t>(base) >= draw->materials.size()) return base;
    // Quantize so near-identical opacities share one variant.
    const int key = static_cast<int>(std::lround(alpha * 1000.0f));
    auto it = matAlphaVariants.find({base, key});
    if (it != matAlphaVariants.end()) return it->second;
    DrawMaterialCPU variant = draw->materials[static_cast<size_t>(base)];
    // displayOpacity modulates the material opacity; it does not replace an
    // authored UsdPreviewSurface/OpenPBR opacity constant.
    variant.alpha = std::max(0.0f, std::min(1.0f, variant.alpha * alpha));
    if (variant.hasLightRtOpenPBR) {
      // CUDA/HIP consume the baked LightRT block in preference to the compact
      // raster fallback, so keep both representations of opacity in sync.
      variant.lightRtOpenPBR.opacity = std::max(
          0.0f, std::min(1.0f, variant.lightRtOpenPBR.opacity * alpha));
    }
    // Authored opacityThreshold remains a mask. A JPEG coverage mask is only
    // a precision heuristic, however: once displayOpacity makes the surface
    // fractional (including varying opacity), it must enter the Blend pass.
    if (variant.alphaMode == static_cast<int>(AlphaMode::Opaque) ||
        variant.alphaMaskHeuristic) {
      variant.alphaMode = static_cast<int>(AlphaMode::Blend);
      variant.alphaMaskHeuristic = false;
    }
    const int idx = static_cast<int>(draw->materials.size());
    draw->materials.push_back(std::move(variant));
    matAlphaVariants[{base, key}] = idx;
    return idx;
  };
  std::unordered_map<int, int> opaqueCoverageVariants;
  auto materialWithoutOpacityMap = [&](int base) -> int {
    auto found = opaqueCoverageVariants.find(base);
    if (found != opaqueCoverageVariants.end()) return found->second;
    DrawMaterialCPU variant = draw->materials[static_cast<size_t>(base)];
    variant.opacityTex = -1;
    variant.opacitySample.tex = -1;
    variant.alpha = 1.0f;
    variant.alphaMode = static_cast<int>(AlphaMode::Opaque);
    variant.alphaMaskHeuristic = false;
    if (variant.hasLightRtOpenPBR) variant.lightRtOpenPBR.opacity = 1.0f;
    const int idx = static_cast<int>(draw->materials.size());
    draw->materials.push_back(std::move(variant));
    opaqueCoverageVariants[base] = idx;
    return idx;
  };

  // GeomSubset per-face materials: when a mesh has `face` GeomSubset children
  // bound to materials, produce a per-triangle material id (else leave *triMat
  // empty -> the caller uses the whole-mesh material). tydra-next's Convert()
  // never fills material_subsets, so we read the GeomSubsets off the stage and
  // reconstruct the triangle->face mapping from the original face vertex counts
  // (fan/earcut both emit c-2 triangles per face, in face order).
  using MaterialPair = std::pair<int, int>;
  auto buildTriMaterials = [&](const tnext::UsdPrim& mp, const tydn::RenderMesh& m,
                               size_t numTris,
                               const std::vector<uint32_t>& sourceFaceId,
                               MaterialPair wholeMat,
                               std::vector<MaterialPair>* triMat) {
    triMat->clear();
    struct Sub { std::vector<int32_t> faces; MaterialPair mat; };
    std::vector<Sub> subs;
    for (const tnext::UsdPrim& c : mp.GetChildren()) {
      if (c.GetTypeName() != "GeomSubset") continue;
      bool isFace = true;  // elementType defaults to "face"
      if (const tnext::Value* et = c.GetPropertyValue("elementType"))
        if (const std::string* t = et->as_token())
          isFace = t->empty() || *t == "face";
      if (!isFace) continue;
      // Resolve through the subset's ancestry so an absent or invalid subset
      // purpose falls back to the whole-mesh material.
      const std::string bind =
          tnext::GetInheritedBoundMaterialPathForPurpose(
              stage, c.GetPath().str(), opts.materialPurpose);
      const std::string backBind = cachedBackMaterialPath(c);
      if (bind.empty() && backBind.empty()) continue;
      std::vector<int32_t> faces = ReadInts(c, "indices", time);
      if (faces.empty()) continue;
      subs.push_back(
          {std::move(faces),
           {bind.empty() ? wholeMat.first
                         : resolveMaterialPath(bind, m.texcoords_0_name,
                                               m.texcoords_1_name),
            backBind.empty() ? wholeMat.second
                             : resolveMaterialPath(backBind, m.texcoords_0_name,
                                                   m.texcoords_1_name)}});
    }
    if (subs.empty()) return;

    const std::vector<uint32_t> fvc = m.face_vertex_counts.flatten();
    std::vector<int> triFace;
    triFace.reserve(numTris);
    // Prefer retained provenance even when the tessellator rewrote the mesh's
    // faceVertexCounts into one triangle per refined face. Merely comparing
    // triangle counts is insufficient there: both arrays have the same length,
    // but authored GeomSubset indices still refer to the four coarse faces.
    if (sourceFaceId.size() == numTris) {
      for (uint32_t face : sourceFaceId) {
        if (face >= fvc.size()) return;
        triFace.push_back(static_cast<int>(face));
      }
    } else {
      for (size_t f = 0; f < fvc.size(); ++f)
        for (uint32_t k = 2; k < fvc[f]; ++k)
          triFace.push_back(static_cast<int>(f));
      if (triFace.size() != numTris) return;
    }

    std::vector<MaterialPair> faceMat(fvc.size(), wholeMat);
    // Authored subset indices use the ORIGINAL face numbering. When
    // SanitizeMeshTopology dropped faces, fvc/faceMat are in the COMPACTED
    // numbering, so route each authored index through sanitize_face_remap
    // (-1 = the face was dropped) -- the core converter does the same
    // (render-converter.cc); applying authored indices directly shifted the
    // bindings onto the wrong faces.
    const bool remap = m.sanitize_dropped_faces > 0 &&
                       !m.sanitize_face_remap.empty();
    for (const Sub& s : subs) {
      for (int32_t f : s.faces) {
        if (f < 0) continue;
        int32_t cf = f;
        if (remap) {
          if (static_cast<size_t>(f) >= m.sanitize_face_remap.size()) continue;
          cf = m.sanitize_face_remap[static_cast<size_t>(f)];
          if (cf < 0) continue;  // face was dropped by sanitize
        }
        if (static_cast<size_t>(cf) < faceMat.size()) faceMat[cf] = s.mat;
      }
    }

    std::vector<MaterialPair> tm(numTris);
    bool split = false;
    for (size_t t = 0; t < numTris; ++t) {
      tm[t] = faceMat[triFace[t]];
      if (tm[t] != wholeMat) split = true;
    }
    if (split) *triMat = std::move(tm);  // uniform -> leave empty
  };
  const size_t bytesPerBatchVertex = sizeof(DrawVertex) + 3 * sizeof(uint32_t);
  const size_t stagingVertexCap =
      opts.uploadStagingBytes > 0
          ? std::max<size_t>(size_t(64) << 10,
                             opts.uploadStagingBytes / bytesPerBatchVertex)
          : (size_t(8) << 20);
  const size_t kBatchVtxCap =
      std::min<size_t>(stream ? (size_t(512) << 10) : (size_t(8) << 20),
                       stagingVertexCap);

  auto flushBatch = [&](Batch& b) {
    if (b.dm.vertices.empty()) return;
    if (!b.anyColor) b.dm.vertexColors.clear();
    if (!b.anyAlpha) b.dm.vertexAlpha.clear();
    if (!b.anyUv1) b.dm.uv1.clear();
    if (b.anySkin && b.dm.jointIdx.size() == b.dm.vertices.size() * 4) {
      // Bone rows are absolute and geomBind/world are already folded into them
      // (BuildNextSkinningFrame), so the batch needs no per-mesh bind matrix and
      // no row offset. skelId only has to be valid for the skinning frame to
      // pick this mesh up; the next path indexes DrawScene::nextSkels, not
      // RenderScene::skeletons.
      b.dm.skelId = 0;
      b.dm.skinMatrixBase = 0;
      std::memset(b.dm.skinGeomBind, 0, sizeof(b.dm.skinGeomBind));
      b.dm.skinGeomBind[0] = b.dm.skinGeomBind[5] = b.dm.skinGeomBind[10] =
          b.dm.skinGeomBind[15] = 1.0f;
      if (b.anyExtendedSkin &&
          b.dm.influenceOffsetCount.size() == b.dm.vertices.size() * 2 &&
          !b.dm.influenceTexels.empty()) {
        const size_t texels = b.dm.influenceTexels.size() / 4;
        b.dm.influenceTexWidth = kNextInfluenceTexWidth;
        b.dm.influenceTexHeight = static_cast<int>(
            (texels + static_cast<size_t>(kNextInfluenceTexWidth) - 1) /
            static_cast<size_t>(kNextInfluenceTexWidth));
        b.dm.influenceTexels.resize(
            static_cast<size_t>(b.dm.influenceTexWidth) *
                static_cast<size_t>(b.dm.influenceTexHeight) * 4,
            0.0f);
      } else {
        b.dm.influenceOffsetCount.clear();
        b.dm.influenceTexels.clear();
        b.dm.influenceTexWidth = b.dm.influenceTexHeight = 0;
        b.dm.maxInfluencesPerVertex = 0;
      }
    } else {
      b.dm.jointIdx.clear();
      b.dm.jointWt.clear();
      b.dm.influenceOffsetCount.clear();
      b.dm.influenceTexels.clear();
      b.dm.influenceTexWidth = b.dm.influenceTexHeight = 0;
      b.dm.maxInfluencesPerVertex = 0;
    }
    // This batch's OWN world bounds, over its (already world-baked) vertices.
    // Copying the running scene-bounds accumulator here instead -- as this used to
    // -- gives every static mesh the scene-spanning box, which makes both the
    // per-mesh frustum cull and raster LOD no-ops: nothing is ever outside the
    // frustum or small on screen.
    //
    // Skinned batches keep the conservative scene box (either skinning mode): the
    // vertices here are a single pose -- rest for GPU skinning, one sampled time
    // for CPU skinning -- so they do not bound the mesh over the animation, and a
    // tight box would pop it out of view as the rig moves.
    float lo[3] = {b.dm.vertices[0].px, b.dm.vertices[0].py,
                   b.dm.vertices[0].pz};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (const DrawVertex& v : b.dm.vertices) {
      lo[0] = std::min(lo[0], v.px); hi[0] = std::max(hi[0], v.px);
      lo[1] = std::min(lo[1], v.py); hi[1] = std::max(hi[1], v.py);
      lo[2] = std::min(lo[2], v.pz); hi[2] = std::max(hi[2], v.pz);
    }
    // The rest box and the bone range are kept even for a skinned batch, whose
    // aabbMin/Max is the scene box: BuildNextPosedSceneBounds re-derives the
    // scene's box at a new time code from these.
    for (int k = 0; k < 3; ++k) {
      b.dm.restAabbMin[k] = lo[k];
      b.dm.restAabbMax[k] = hi[k];
    }
    if (!b.dm.jointIdx.empty()) {
      uint32_t jlo = std::numeric_limits<uint32_t>::max(), jhi = 0;
      for (uint32_t j : b.dm.jointIdx) { jlo = std::min(jlo, j); jhi = std::max(jhi, j); }
      if (jlo <= jhi) {
        b.dm.boneLo = static_cast<int>(jlo);
        b.dm.boneHi = static_cast<int>(jhi);
      }
    }
    if ((b.anySkin || b.anyCpuSkin) && bounds.has) {
      for (int k = 0; k < 3; ++k) {
        b.dm.aabbMin[k] = bounds.mn[k]; b.dm.aabbMax[k] = bounds.mx[k];
      }
    } else {
      for (int k = 0; k < 3; ++k) {
        b.dm.aabbMin[k] = lo[k] - b.dm.morphExtent[k];
        b.dm.aabbMax[k] = hi[k] + b.dm.morphExtent[k];
      }
      if (b.animatedWorld) {
        const float xs[2] = {lo[0], hi[0]};
        const float ys[2] = {lo[1], hi[1]};
        const float zs[2] = {lo[2], hi[2]};
        for (int k = 0; k < 3; ++k) {
          b.dm.aabbMin[k] = std::numeric_limits<float>::max();
          b.dm.aabbMax[k] = -std::numeric_limits<float>::max();
        }
        for (float x : xs) for (float y : ys) for (float z : zs) {
          const float p[3] = {x, y, z};
          for (int c = 0; c < 3; ++c) {
            const float v = p[0] * b.dm.world[c] +
                            p[1] * b.dm.world[4 + c] +
                            p[2] * b.dm.world[8 + c] + b.dm.world[12 + c];
            b.dm.aabbMin[c] = std::min(b.dm.aabbMin[c], v);
            b.dm.aabbMax[c] = std::max(b.dm.aabbMax[c], v);
          }
        }
      }
    }
    if (b.dm.submeshes.empty()) {
      b.dm.submeshes.push_back(
          DrawSubmesh{0, static_cast<uint32_t>(b.dm.indices.size()), b.matId,
                      b.backMatId});
    }
    if (!b.animatedWorld) {
      std::memset(b.dm.world, 0, sizeof(b.dm.world));
      b.dm.world[0] = b.dm.world[5] = b.dm.world[10] = b.dm.world[15] = 1.0f;
    }
    draw->triangleCount += b.dm.indices.size() / 3;
    draw->meshes.push_back(std::move(b.dm));
    b = Batch();
    publishAvailableMeshes();
  };

  // Most large static exports contain many meshes with no material binding,
  // subsets, deformation, or optional vertex streams. Keep their exact
  // topology and stage order, but avoid re-entering the feature bookkeeping
  // below for every mesh. This is deliberately a structural predicate rather
  // than a scene/profile special case; any authored feature falls back to the
  // full path.
  auto appendPlainStatic = [&](const std::string& purpose, bool proxyFallback,
                               const DrawMeshCPU& source,
                               const tydn::RenderMesh& sourceMesh,
                               const std::string& path) {
    Batch& b = getBatch({purpose, source.geometricNormal, proxyFallback,
                         sourceMesh.double_sided, 0, -1, 0, 0, 0});
    if (!b.dm.vertices.empty() &&
        b.dm.vertices.size() + source.vertices.size() > kBatchVtxCap) {
      flushBatch(b);
    }
    b.dm.purpose = purpose;
    b.dm.proxyFallback = proxyFallback;
    b.dm.geometricNormal = source.geometricNormal;
    b.dm.doubleSided = sourceMesh.double_sided;
    if (b.dm.absPath.empty()) {
      b.dm.absPath = source.absPath.empty() ? path : source.absPath;
      b.dm.name = source.name;
    }
    const uint32_t vbase = static_cast<uint32_t>(b.dm.vertices.size());
    const size_t required = b.dm.vertices.size() + source.vertices.size();
    if (required > b.dm.vertices.capacity()) {
      b.dm.vertices.reserve(std::max(required, b.dm.vertices.capacity() * 2));
    }
    b.dm.vertices.insert(b.dm.vertices.end(), source.vertices.begin(),
                         source.vertices.end());
    b.dm.indices.reserve(b.dm.indices.size() + source.indices.size());
    for (uint32_t index : source.indices) b.dm.indices.push_back(vbase + index);
    b.dm.sourceFaceId.insert(b.dm.sourceFaceId.end(), source.sourceFaceId.begin(),
                             source.sourceFaceId.end());
    if (!source.wireframeIndices.empty()) {
      b.dm.wireframeIndices.reserve(b.dm.wireframeIndices.size() +
                                    source.wireframeIndices.size());
      for (uint32_t index : source.wireframeIndices)
        b.dm.wireframeIndices.push_back(vbase + index);
    }
    b.nextFaceId += static_cast<uint32_t>(sourceMesh.face_count());
  };

  // Retain only fields used by the flat batching pass. RenderPrimRecord also
  // carries the local matrix, type/native-prototype strings and classification;
  // keeping those for hundreds of thousands of district meshes wastes a large
  // amount of memory throughout conversion.
  struct PendingMeshPrim {
    tnext::UsdPrim prim;
    std::string path;
    std::string purpose;
    std::string materialPath;
    double world[16];
    bool animatedWorld{false};
    bool deferredProxy{false};
    bool proxyFallback{false};
    float viewPriority{-1.0f};
  };
  std::vector<PendingMeshPrim> meshPrims;
  {
    meshPrims.reserve(extracted.meshes.size());
    for (tydn::RenderPrimRecord& rec : extracted.meshes) {
      if (!consumed.count(rec.path)) {
        PendingMeshPrim pending;
        pending.prim = std::move(rec.prim);
        pending.path = std::move(rec.path);
        pending.purpose = std::move(rec.purpose);
        pending.materialPath =
            tnext::GetInheritedBoundMaterialPathForPurpose(
                stage, pending.path, opts.materialPurpose);
        pending.animatedWorld = rec.animated_world;
        std::memcpy(pending.world, rec.world, sizeof(pending.world));
        meshPrims.push_back(std::move(pending));
      }
    }
    // Deferred payloads have no composed descendants yet. Emit one bounded
    // marker per payload root so the first frame communicates that content is
    // intentionally unloaded (and so an otherwise payload-only stage remains
    // renderable until the user requests materialization).
    for (const tnext::Path& deferred : deferredPayloads) {
      const std::string deferredPath = deferred.str();
      if (consumed.count(deferredPath)) continue;
      tnext::UsdPrim prim = stage.GetPrimAtPath(deferred);
      if (!prim.IsValid()) continue;
      PendingMeshPrim pending;
      pending.prim = prim;
      pending.path = deferredPath;
      pending.purpose = "proxy";
      pending.deferredProxy = true;
      tydn::ComputeWorldTransform(stage, prim, pending.world, time);
      meshPrims.push_back(std::move(pending));
    }
    std::unordered_set<std::string> renderModels;
    for (const PendingMeshPrim& pending : meshPrims) {
      if (pending.purpose != "render") continue;
      const std::string model = ResolveNextModelRoot(pending.prim);
      if (!model.empty()) renderModels.insert(model);
    }
    for (PendingMeshPrim& pending : meshPrims) {
      if (pending.purpose != "proxy") continue;
      pending.proxyFallback = renderModels.count(
          ResolveNextModelRoot(pending.prim)) == 0;
    }
    // Native-instance and extraction-only records are no longer needed. Drop
    // their backing vectors before geometry conversion starts competing for the
    // process RSS peak.
    extracted = tydn::RenderExtractResult();
    if (ctrl) {
      ctrl->meshesTotal.store(static_cast<long long>(meshPrims.size()));
      ctrl->meshesDone.store(0);
    }
  }

  // Namespace order is unrelated to what the startup camera can see. Rank
  // authored extents before geometry inspection/conversion so bounded mesh and
  // VRAM budgets are spent on the useful frame first. Items without extents
  // retain stable relative order behind visible bounded items.
  if (!opts.viewCamera.empty() && meshPrims.size() > 1) {
    NextCameraPose viewCamera;
    if (FindNextCamera(stage, opts.viewCamera, time, &viewCamera)) {
      size_t rankedCount = 0;
      for (PendingMeshPrim& pending : meshPrims) {
        float localMin[3], localMax[3];
        tnext::UsdPrim boundPrim = pending.prim;
        double boundWorld[16];
        std::memcpy(boundWorld, pending.world, sizeof(boundWorld));
        if (!PreviewExtent(boundPrim, localMin, localMax)) {
          std::string ancestorPath = pending.path;
          bool foundAncestor = false;
          while (ancestorPath.size() > 1) {
            const size_t slash = ancestorPath.find_last_of('/');
            ancestorPath = (slash == std::string::npos || slash == 0)
                               ? "/"
                               : ancestorPath.substr(0, slash);
            boundPrim = stage.GetPrimAtPath(ancestorPath);
            if (boundPrim.IsValid() &&
                PreviewExtent(boundPrim, localMin, localMax)) {
              tydn::ComputeWorldTransform(stage, boundPrim, boundWorld, time);
              foundAncestor = true;
              break;
            }
          }
          if (!foundAncestor) continue;
        }
        float worldMin[3] = {std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
        float worldMax[3] = {-std::numeric_limits<float>::max(),
                             -std::numeric_limits<float>::max(),
                             -std::numeric_limits<float>::max()};
        for (int corner = 0; corner < 8; ++corner) {
          float p[3];
          TransformRowPoint(boundWorld,
                            (corner & 1) ? localMax[0] : localMin[0],
                            (corner & 2) ? localMax[1] : localMin[1],
                            (corner & 4) ? localMax[2] : localMin[2], p);
          for (int k = 0; k < 3; ++k) {
            worldMin[k] = std::min(worldMin[k], p[k]);
            worldMax[k] = std::max(worldMax[k], p[k]);
          }
        }
        float toCenter[3];
        float radius2 = 0.0f;
        float distance2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
          const float center = 0.5f * (worldMin[k] + worldMax[k]);
          const float half = 0.5f * (worldMax[k] - worldMin[k]);
          toCenter[k] = center - viewCamera.eye[k];
          radius2 += half * half;
          distance2 += toCenter[k] * toCenter[k];
        }
        const float distance = std::sqrt(std::max(distance2, 1.0e-12f));
        const float depth = toCenter[0] * viewCamera.forward[0] +
                            toCenter[1] * viewCamera.forward[1] +
                            toCenter[2] * viewCamera.forward[2];
        const float radius = std::sqrt(radius2);
        const float projected = radius / std::max(distance - radius, 1.0f);
        const float alignment = depth / distance;
        // Use a conservative diagonal cone (16:9 around the authored vertical
        // FOV), expanded by the bound's angular radius. Exact per-frame frustum
        // culling still happens in Gui after upload; this is only admission
        // order and therefore deliberately favors false positives.
        constexpr float kPi = 3.14159265358979323846f;
        const float halfY = std::clamp(viewCamera.fovYDeg, 1.0f, 179.0f) *
                            (kPi / 360.0f);
        const float halfDiag = std::atan(std::tan(halfY) * 2.04f);
        const float angularRadius =
            std::asin(std::min(1.0f, radius / distance));
        const bool inFront = depth + radius > 0.0f;
        const bool inView = inFront &&
            alignment >= std::cos(std::min(kPi, halfDiag + angularRadius));
        pending.viewPriority = (inView ? 2.0e6f : inFront ? 1.0e6f : 0.0f) +
                               projected;
        ++rankedCount;
      }
      std::stable_sort(meshPrims.begin(), meshPrims.end(),
                       [](const PendingMeshPrim& a,
                          const PendingMeshPrim& b) {
                         return a.viewPriority > b.viewPriority;
                       });
      if (timing) {
        LOGI("next: camera-prioritized %zu/%zu mesh extents for '%s'",
             rankedCount, meshPrims.size(), opts.viewCamera.c_str());
      }
    } else if (timing) {
      LOGW("next: view-priority camera '%s' was not found",
           opts.viewCamera.c_str());
    }
  }
  const auto collectMeshesAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: render-prim collection %.3f s",
         std::chrono::duration<double>(collectMeshesAt - nativeInstancesAt).count());
  const auto prototypesAt = std::chrono::steady_clock::now();
  if (timing)
    LOGI("next timing: post-extraction setup %.3f s",
         std::chrono::duration<double>(prototypesAt - collectMeshesAt).count());

  bool capped = false;
  long long totalTris = 0;
  size_t admittedGeometryBytes = 0;
  size_t budgetSkippedMeshCount = 0;
  size_t lazySkippedMeshCount = 0;
  size_t convertedSourceMeshCount = 0;
  // Weld effectiveness: emitted vertices vs authored points. ~1.0 means the
  // faceVarying split cost nothing; a ratio near the corners-per-point count
  // means the weld is not catching (see FillFlatGeometry).
  size_t weldedVertices = 0;
  size_t sourcePoints = 0;

  // Multiple instance-proxy records may address the same underlying PrimSpec.
  // Release its authored geometry only after the final record has produced its
  // self-contained conversion result; until then a parallel worker may still
  // be reading the shared spec.
  std::vector<uint32_t> pendingPrimUses;
  size_t trackedPrimUses = 0;
  if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
    const tnext::Layer* rootLayer = stage.GetRootLayer();
    if (rootLayer) pendingPrimUses.resize(rootLayer->prim_count(), 0u);
    for (const PendingMeshPrim& pending : meshPrims) {
      const size_t index = pending.prim.GetIndex();
      if (pending.prim.GetLayer() == rootLayer &&
          index < pendingPrimUses.size()) {
        ++pendingPrimUses[index];
        ++trackedPrimUses;
      }
    }
  }
  auto releasePendingPrim = [&](tnext::UsdPrim* prim) {
    if (!prim || !prim->IsValid() || trackedPrimUses == 0) return;
    if (prim->GetLayer() != stage.GetRootLayer()) return;
    const size_t index = prim->GetIndex();
    if (index >= pendingPrimUses.size() || pendingPrimUses[index] == 0) return;
    --trackedPrimUses;
    if (--pendingPrimUses[index] == 0) {
      const tnext::Stage::StaticGeometryReleaseStats one =
          session->ReleaseStaticGeometryArraysForPrim(*prim);
      incrementalReleased.property_count += one.property_count;
      incrementalReleased.element_count += one.element_count;
      incrementalReleased.estimated_payload_bytes +=
          one.estimated_payload_bytes;
    }
  };

  // Convert independent meshes concurrently, then consume them in stable stage
  // order below for deterministic material batching. Keep this bounded: each
  // worker owns one converter and at most one in-flight RenderMesh. Aggregate
  // estimates are used for diagnostics and per-wave admission; they must not
  // disable parallel conversion for a scene larger than the GPU budget.
  // Retaining GeometryInfo would duplicate the path and type strings for every
  // pending mesh.
  unsigned convertThreads = opts.conversionThreads
      ? opts.conversionThreads
      // Large instanced scenes spend most of conversion in independent mesh
      // extraction. Allow more workers by default on large hosts while
      // retaining a hard cap so peak temporary geometry remains bounded.
      : std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
  convertThreads = std::clamp(convertThreads, 1u, 64u);
  convertThreads = std::min<unsigned>(
      convertThreads, static_cast<unsigned>(meshPrims.size()));
  const auto geometryEstimateBegin = std::chrono::steady_clock::now();
  std::vector<size_t> geometryBytes(meshPrims.size());
  size_t estimatedGeometryBytes = 0;
  bool estimateOverflow = false;
  auto estimateGeometry = [&](size_t i, tydn::RenderSceneConverter* estimator) {
    geometryBytes[i] = estimator->GetGeometryInfo(
                           meshPrims[i].prim, tydn::GeometryKind::Mesh)
                           .estimated_resident_bytes;
  };
#if defined(LIGHTUSD_ENABLE_THREAD)
  // GeometryInfo is a read-only preflight. Run it alongside independent
  // workers; on broad composed scenes this removes a second serial walk over
  // 70k-850k mesh records before the first conversion wave can start.
  if (convertThreads > 1 && meshPrims.size() >= 1024) {
    std::atomic<size_t> nextEstimate{0};
    std::vector<std::thread> estimateWorkers;
    estimateWorkers.reserve(convertThreads);
    for (unsigned worker = 0; worker < convertThreads; ++worker) {
      estimateWorkers.emplace_back([&, worker]() {
        (void)worker;
        tydn::RenderSceneConverter estimator(cfg);
        for (;;) {
          const size_t i = nextEstimate.fetch_add(1);
          if (i >= meshPrims.size()) break;
          estimateGeometry(i, &estimator);
        }
      });
    }
    for (std::thread& worker : estimateWorkers) worker.join();
  } else
#endif
  {
    for (size_t i = 0; i < meshPrims.size(); ++i) {
      estimateGeometry(i, &conv);
    }
  }
  for (size_t i = 0; i < meshPrims.size(); ++i) {
    if (estimatedGeometryBytes > std::numeric_limits<size_t>::max() -
                                    geometryBytes[i]) {
      // Keep estimating every mesh so the per-mesh budget checks below remain
      // valid. The aggregate is diagnostic only and must not disable bounded
      // conversion for a scene whose total estimate is larger than size_t.
      estimateOverflow = true;
      estimatedGeometryBytes = std::numeric_limits<size_t>::max();
    } else if (!estimateOverflow) {
      estimatedGeometryBytes += geometryBytes[i];
    }
  }
  if (timing) {
    LOGI("next timing: geometry estimates %.3f s (%zu meshes, %.1f MiB%s)",
         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       geometryEstimateBegin)
             .count(),
         geometryBytes.size(),
         static_cast<double>(estimatedGeometryBytes) / (1024.0 * 1024.0),
         estimateOverflow ? ", aggregate overflow" : "");
  }
  // Stage B of the --texture-fit budget. The decoder was built (far above) with
  // the full threshold because the geometry footprint was not yet known; now it
  // is, and geometry shares the device with textures, so tighten the live
  // budget to what is left. Leases already taken stay valid and simply count
  // against the smaller cap. Mesh materials -- nearly all textures in a mesh
  // scene -- resolve after this point, so this lands before the bulk of decode.
  if (opts.textureFitThresholdBytes > 0 && !opts.textureBudgetExplicit &&
      texCache.decoder &&
      opts.textureFitThresholdBytes !=
          (std::numeric_limits<size_t>::max)()) {
    const size_t threshold = opts.textureFitThresholdBytes;
    size_t remaining = estimatedGeometryBytes >= threshold
                           ? 0
                           : threshold - estimatedGeometryBytes;
    // NOTE: 0 means UNBOUNDED to the budget state, not "nothing fits" -- a
    // literal zero here would invert the intent and disable the limiter
    // entirely. Floor it well above zero instead.
    const size_t kFloor = 64ull * 1024ull * 1024ull;
    if (remaining < kFloor) remaining = kFloor;
    texCache.decoder->SetBudgetBytes(uint64_t(remaining));
    if (timing) {
      LOGI("next timing: texture-fit budget %.1f MiB -> %.1f MiB after %.1f MiB "
           "geometry",
           double(threshold) / (1024.0 * 1024.0),
           double(remaining) / (1024.0 * 1024.0),
           double(estimatedGeometryBytes) / (1024.0 * 1024.0));
    }
  }
  // Deferred payloads and an aggregate estimate above the GPU budget are both
  // normal for large scenes. Conversion is still safe in parallel because
  // work is bounded by the per-wave byte limit and all results are consumed in
  // stable stage order below. The old global gates serialized the entire mesh
  // set in precisely the cases where streaming mattered most.
  const bool parallelConvert = convertThreads > 1 && meshPrims.size() >= 16;
  struct ConvertedMesh {
    tydn::RenderMesh mesh;
    DrawMeshCPU draw;
    std::vector<uint32_t> vertexToPoint;
    bool worldBaked{false};
  };
  std::vector<std::unique_ptr<ConvertedMesh>> converted(meshPrims.size());
  // Persistent workers retain the low-memory 64 MiB wave without paying thread
  // creation/join costs hundreds of times.
  const size_t convertChunk = opts.meshConversionChunkPrims != 0
      ? opts.meshConversionChunkPrims
      // Keep every worker busy for several claims while bounding retained
      // ConvertedMesh records independently of unreliable source estimates.
      // This scales with requested hardware parallelism rather than a scene.
      : static_cast<size_t>(convertThreads) * static_cast<size_t>(16);
  const size_t convertChunkBytes = opts.meshConversionChunkBytes != 0
      ? opts.meshConversionChunkBytes
      : (opts.streamBufferBytes != 0
             ? opts.streamBufferBytes
             : std::numeric_limits<size_t>::max());
  double parallelConvertSeconds = 0.0;
  size_t nextConvertEnd = 0;
  std::mutex convertMutex;
  std::condition_variable convertWork;
  std::condition_variable convertDone;
  std::atomic<size_t> convertNext{0};
  std::atomic<size_t> convertEnd{0};
  size_t convertGeneration = 0;
  unsigned convertFinished = 0;
  bool convertStop = false;
  std::vector<std::thread> convertWorkers;
  if (parallelConvert) {
    convertWorkers.reserve(convertThreads);
    for (unsigned worker = 0; worker < convertThreads; ++worker) {
      convertWorkers.emplace_back([&, worker]() {
        (void)worker;
        tydn::ConverterConfig workerCfg = cfg;
        workerCfg.progress_callback = nullptr;
        workerCfg.cancel_callback = [ctrl]() {
          return ctrl && ctrl->cancel.load();
        };
        tydn::RenderSceneConverter workerConv(workerCfg);
        size_t seenGeneration = 0;
        for (;;) {
          {
            std::unique_lock<std::mutex> lock(convertMutex);
            convertWork.wait(lock, [&]() {
              return convertStop || convertGeneration != seenGeneration;
            });
            if (convertStop) return;
            seenGeneration = convertGeneration;
          }
          for (;;) {
            if (ctrl && ctrl->cancel.load()) break;
            const size_t i = convertNext.fetch_add(1);
            if (i >= convertEnd.load()) break;
            // GeometryInfo is the converter's authoritative lightweight
            // topology preflight.  A zero estimate cannot produce a
            // renderable mesh, so avoid reparsing its properties and running
            // triangulation just to discard the empty result. Deferred payload
            // roots are marker proxies and intentionally bypass this test.
            if (!meshPrims[i].deferredProxy && geometryBytes[i] == 0) continue;
            auto result = std::make_unique<ConvertedMesh>();
            bool convertedMesh = false;
            if (meshPrims[i].deferredProxy) {
              convertedMesh = workerConv.ConvertExtentProxy(
                  meshPrims[i].prim, &result->mesh);
              if (!convertedMesh) {
                convertedMesh = workerConv.ConvertBoundsProxy(
                    meshPrims[i].prim, tydn::Float3(-1.0f, -1.0f, -1.0f),
                    tydn::Float3(1.0f, 1.0f, 1.0f), &result->mesh);
              }
            } else {
              convertedMesh = workerConv.ConvertRenderableMesh(
                  stage, meshPrims[i].prim, &result->mesh);
            }
            if (convertedMesh &&
                NeedsUnrealDoubleSidedFallback(meshPrims[i].prim)) {
              result->mesh.double_sided = true;
            }
            if (convertedMesh &&
                FillFlatGeometry(result->mesh, &result->draw,
                                 &result->vertexToPoint)) {
              if (!result->mesh.has_skin() &&
                  !result->mesh.has_blend_shapes() &&
                  !meshPrims[i].animatedWorld) {
                TransformDrawVertices(meshPrims[i].world, &result->draw);
                result->worldBaked = true;
              }
              converted[i] = std::move(result);
            }
          }
          {
            std::lock_guard<std::mutex> lock(convertMutex);
            if (++convertFinished == convertThreads) convertDone.notify_one();
          }
        }
      });
    }
  }
  auto convertRange = [&](size_t begin, size_t end) {
    const auto rangeStart = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(convertMutex);
      convertNext.store(begin);
      convertEnd.store(end);
      convertFinished = 0;
      ++convertGeneration;
    }
    convertWork.notify_all();
    {
      std::unique_lock<std::mutex> lock(convertMutex);
      convertDone.wait(lock, [&]() { return convertFinished == convertThreads; });
    }
    parallelConvertSeconds += std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - rangeStart)
                                  .count();
  };

  for (size_t meshIndex = 0; meshIndex < meshPrims.size(); ++meshIndex) {
    if (ctrl && ctrl->cancel.load()) break;
    if (capped) break;
    if (opts.maxMeshConversions > 0 &&
        convertedSourceMeshCount >= opts.maxMeshConversions) {
      lazySkippedMeshCount = meshPrims.size() - meshIndex;
      draw->truncated = true;
      for (size_t pendingIndex = meshIndex;
           pendingIndex < meshPrims.size(); ++pendingIndex) {
        converted[pendingIndex].reset();
        releasePendingPrim(&meshPrims[pendingIndex].prim);
      }
      break;
    }
    if (ctrl) ctrl->meshesDone.fetch_add(1);

    PendingMeshPrim& pending = meshPrims[meshIndex];
    // Retain proxy-purpose geometry for hierarchy/MCP selection and for the
    // Purpose > Proxy visibility toggle. The GUI hides proxy purpose by
    // default, so it does not z-fight with the paired render representation.

    const size_t geometryBytesForMesh = geometryBytes[meshIndex];
    const bool overGeometryBudget = !pending.deferredProxy &&
        opts.gpuGeometryBudgetBytes > 0 &&
        (admittedGeometryBytes >= opts.gpuGeometryBudgetBytes ||
         geometryBytesForMesh >
             opts.gpuGeometryBudgetBytes - admittedGeometryBytes);
    if (overGeometryBudget) {
      // A resource budget is a truncation policy, not authored proxy geometry.
      // Replacing every rejected mesh with its extent turns detailed scenes
      // into thousands of misleading cubes and can itself consume substantial
      // GPU memory. Skip meshes rejected solely by the geometry budget.
      ++budgetSkippedMeshCount;
      draw->truncated = true;
      // A preceding parallel wave can have converted this mesh before actual
      // admitted bytes caught up with the estimate. Do not retain that rejected
      // topology until the end of a hundreds-of-thousands-mesh pass.
      converted[meshIndex].reset();
      releasePendingPrim(&pending.prim);
      pending = PendingMeshPrim();
      continue;
    }

    if (parallelConvert && meshIndex >= nextConvertEnd) {
      size_t end = meshIndex;
      size_t bytes = 0;
      size_t budgetRemaining =
          opts.gpuGeometryBudgetBytes > 0
              ? opts.gpuGeometryBudgetBytes - admittedGeometryBytes
              : std::numeric_limits<size_t>::max();
      while (end < meshPrims.size() && end - meshIndex < convertChunk) {
        const size_t add = geometryBytes[end];
        if (end > meshIndex && add > convertChunkBytes -
                                             std::min(bytes, convertChunkBytes))
          break;
        // Do not launch work whose conservative estimate already exceeds the
        // remaining geometry budget. Actual flattened sizes are checked again
        // while consuming the bounded wave below.
        if (add > budgetRemaining) break;
        bytes += add;
        budgetRemaining -= add;
        ++end;
      }
      // The current mesh passed the same remaining-budget test above, so the
      // range always contains at least that one item.
      nextConvertEnd = end;
      convertRange(meshIndex, nextConvertEnd);
    }
    // Move the record out so its strings and prim handle are released at the end
    // of this iteration instead of all surviving until the full scene finishes.
    PendingMeshPrim meshRecord = std::move(pending);
    const tnext::UsdPrim& mp = meshRecord.prim;
    tydn::RenderMesh m;
    DrawMeshCPU loc;
    std::vector<uint32_t> vertexToPoint;
    bool worldBaked = false;
    if (parallelConvert) {
      if (!converted[meshIndex]) {
        releasePendingPrim(&meshRecord.prim);
        continue;
      }
      m = std::move(converted[meshIndex]->mesh);
      loc = std::move(converted[meshIndex]->draw);
      vertexToPoint = std::move(converted[meshIndex]->vertexToPoint);
      worldBaked = converted[meshIndex]->worldBaked;
      converted[meshIndex].reset();
    } else {
      bool convertedMesh = false;
      if (meshRecord.deferredProxy) {
        convertedMesh = conv.ConvertExtentProxy(mp, &m);
        if (!convertedMesh) {
          convertedMesh = conv.ConvertBoundsProxy(
              mp, tydn::Float3(-1.0f, -1.0f, -1.0f),
              tydn::Float3(1.0f, 1.0f, 1.0f), &m);
        }
        if (convertedMesh) draw->truncated = true;
      } else {
        convertedMesh = conv.ConvertRenderableMesh(stage, mp, &m);
      }
      if (convertedMesh && NeedsUnrealDoubleSidedFallback(mp)) {
        m.double_sided = true;
      }
      if (!convertedMesh || !FillFlatGeometry(m, &loc, &vertexToPoint)) {
        releasePendingPrim(&meshRecord.prim);
        continue;
      }
    }
    if (loc.vertices.empty()) {
      releasePendingPrim(&meshRecord.prim);
      continue;
    }
    BuildAuthoredControlCage(mp, time, &loc);
    const std::string backMaterialPath = cachedBackMaterialPath(mp);
    bool hasSubsetMaterialBindings = false;
    const size_t childCount = mp.GetChildCount();
    for (size_t childIndex = 0; childIndex < childCount; ++childIndex) {
      const tnext::UsdPrim child = mp.GetChildAt(childIndex);
      if (!child.IsValid() || child.GetTypeName() != "GeomSubset") continue;
      const char* bindingNames[] = {
          "material:binding:preview", "material:binding",
          "material:binding:full", "material:binding:back"};
      for (const char* name : bindingNames) {
        const std::vector<tnext::Path>* targets = child.GetRelationship(name);
        if (targets && !targets->empty()) {
          hasSubsetMaterialBindings = true;
          break;
        }
      }
      if (hasSubsetMaterialBindings) break;
    }
    const bool plainStatic =
        worldBaked && !m.has_skin() && !m.has_blend_shapes() &&
        meshRecord.materialPath.empty() && backMaterialPath.empty() &&
        !hasSubsetMaterialBindings && !hasAuthoredLightLinks &&
        loc.vertexColors.empty() && loc.vertexAlpha.empty() && loc.uv1.empty() &&
        loc.tangents.empty() && loc.binormals.empty() &&
        loc.jointIdx.empty() && loc.jointWt.empty() &&
        loc.influenceOffsetCount.empty() && loc.influenceTexels.empty() &&
        loc.morphOffsetCount.empty() && loc.morphDeltaHalf.empty() &&
        loc.instanceCount() == 0;
    if (plainStatic) {
      ++convertedSourceMeshCount;
      weldedVertices += loc.vertices.size();
      sourcePoints += m.point_count();
      admittedGeometryBytes +=
          loc.vertices.size() * sizeof(DrawVertex) +
          loc.indices.size() * sizeof(uint32_t);
      appendPlainStatic(meshRecord.purpose, meshRecord.proxyFallback, loc, m,
                        meshRecord.path);
      const tydn::Float3& lo = m.bbox_min;
      const tydn::Float3& hi = m.bbox_max;
      float mf[16];
      for (int k = 0; k < 16; ++k)
        mf[k] = static_cast<float>(meshRecord.world[k]);
      for (int corner = 0; corner < 8; ++corner) {
        const float lp[3] = {(corner & 1) ? hi.x : lo.x,
                             (corner & 2) ? hi.y : lo.y,
                             (corner & 4) ? hi.z : lo.z};
        float wp[3];
        for (int c = 0; c < 3; ++c)
          wp[c] = lp[0] * mf[c] + lp[1] * mf[4 + c] +
                  lp[2] * mf[8 + c] + mf[12 + c];
        bounds.add(wp);
      }
      releasePendingPrim(&meshRecord.prim);
      continue;
    }
    ++convertedSourceMeshCount;
    weldedVertices += loc.vertices.size();
    sourcePoints += m.point_count();
    admittedGeometryBytes +=
        loc.vertices.size() * sizeof(DrawVertex) +
        loc.indices.size() * sizeof(uint32_t) +
        loc.vertexColors.size() * sizeof(float) +
        loc.vertexAlpha.size() * sizeof(float) +
        loc.uv1.size() * sizeof(float) +
        loc.tangents.size() * sizeof(float) +
        loc.binormals.size() * sizeof(float);
    const std::string& purpose = meshRecord.purpose;
    int wholeMat = resolveMaterialPath(meshRecord.materialPath,
                                       m.texcoords_0_name,
                                       m.texcoords_1_name);
    int wholeBackMat = backMaterialPath.empty()
                           ? -1
                           : resolveMaterialPath(backMaterialPath,
                                                 m.texcoords_0_name,
                                                 m.texcoords_1_name);
    // Thin preview coverage surfaces (cloth, leaves, hair cards) need both
    // sides at folds and open boundaries. Exporters commonly leave the USD
    // default doubleSided=false even though a grayscale cutout supplies the
    // apparent boundary; back-face culling then removes sleeve/cloth patches
    // as the camera moves. Restrict the fallback to texture-backed masks so
    // ordinary closed opaque meshes retain authored culling.
    if (wholeMat >= 0 && static_cast<size_t>(wholeMat) < draw->materials.size()) {
      const DrawMaterialCPU& material =
          draw->materials[static_cast<size_t>(wholeMat)];
      if (material.alphaMode == static_cast<int>(AlphaMode::Mask) &&
          material.opacityTex >= 0) {
        m.double_sided = true;
        loc.doubleSided = true;
      }
    }
    double mw16[16];
    std::memcpy(mw16, meshRecord.world, sizeof(mw16));
    // Blendshapes, BEFORE skinning: a blendshape deforms the bind-space points and
    // the skeleton then poses the RESULT (deform.glsl morphs, then runs LBS on the
    // morphed position, and the instanced prototype path bakes in that order too).
    // Baking the skin first and the morph second -- as this used to -- adds the
    // bind-space offsets to already-posed points, so a mesh that is blendshaped AND
    // skinned at the same time code lands somewhere neither path intended.
    //
    // This ran ONLY on the instanced-prototype path, so an ordinary (non-instanced)
    // blendshaped mesh never morphed at all under --next -- it rendered its rest
    // shape at every time code. Same choice as there: bake the pose in
    // (LUSDVIEW_NEXT_MORPH_BAKE=1) or build GPU-morph channels the shader applies
    // per frame. CPU skinning forces the bake: the shader's morph is applied to
    // whatever position it is handed, which for a CPU-skinned mesh is the POSED
    // one -- the wrong order again, and not fixable in the shader (the delta would
    // have to be carried into pose space per frame). Baking both keeps them ordered.
    // A CPU-skinning run re-converts on every time change anyway, so the baked morph
    // still animates.
    static const bool kBakeMorphStatic = [] {
      const char* e = std::getenv("LUSDVIEW_NEXT_MORPH_BAKE");
      return e && e[0] == '1';
    }();
    if (m.has_blend_shapes()) {
      if (kBakeMorphStatic || !opts.gpuSkinning) {
        // As in BuildProtoMesh, these vertices are already at the sampled
        // morph pose. Do not add the live-GPU morphExtent padding again.
        BakeBlendShapes(stage, mp, time, &loc, vertexToPoint, m.point_count());
      } else {
        BuildMorphChannelsNext(stage, mp, time, &loc, vertexToPoint,
                               m.point_count());
      }
    }
    const bool animatedWorld = meshRecord.animatedWorld;
    // Skeletal skinning, before the vertices are world-baked into the batch.
    // GPU: keep the (morphed) bind pose and emit per-vertex joint attributes (the
    // shader poses every frame). CPU: bake the static pose at `time` into the
    // geometry. Both no-op for unskinned meshes.
    bool cpuSkinned = false;
    if (m.has_skin()) {
      if (opts.gpuSkinning) {
        double skinWorld[16];
        double renderWorld[16];
        const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, 1, 0, 0, 0, 0, 1};
        if (animatedWorld) {
          std::memcpy(skinWorld, ident, sizeof(skinWorld));
          std::memcpy(renderWorld, mw16, sizeof(renderWorld));
        } else {
          std::memcpy(skinWorld, mw16, sizeof(skinWorld));
          std::memcpy(renderWorld, ident, sizeof(renderWorld));
        }
        SetupGpuSkinNext(stage, mp, time, &loc, vertexToPoint, m.point_count(),
                         skinWorld, renderWorld, ident, draw);
      } else {
        cpuSkinned =
            BakeSkinning(stage, mp, time, &loc, vertexToPoint, m.point_count());
      }
    }
    // A morphed mesh must not share a batch with anything else: its channel ids
    // and its bound animation are its own. 0 = poolable with other static meshes.
    const int morphBatchId =
        (loc.morphChannelCount > 0) ? ++nextMorphBatchId : 0;
    const int lightLinkBatchId =
        hasAuthoredLightLinks ? ++nextLightLinkBatchId : 0;
    const int animatedWorldBatchId =
        animatedWorld ? ++nextAnimatedWorldBatchId : 0;
    float Mf[16];
    for (int k = 0; k < 16; ++k) Mf[k] = static_cast<float>(mw16[k]);
    const float* M = Mf;  // row-major, p*M (same as the converter's node xform)

    // World-transform vertices in place (positions + normals), so both the
    // single- and multi-material append paths just copy the vertex. Static
    // meshes take this path in parallel with conversion/welding above.
    if (!worldBaked && !animatedWorld) TransformDrawVertices(mw16, &loc);
    // The morph deltas are MESH-LOCAL (BlendShape offsets), but the vertices are
    // now world-baked and every consumer -- the vertex shader (deform.glsl) and
    // BuildNextRtDeformedVertices alike -- adds the delta straight onto the
    // position it is handed. So the deltas have to be rotated/scaled into the same
    // space, or a blendshaped mesh under a rotated or scaled parent morphs along
    // the wrong axes. Skinning already does the equivalent (its bone rows are
    // inverse(bakedWorld) * G * skin * skeletonWorld * inverse(renderWorld));
    // the instanced prototype path needs none of this,
    // because its vertices stay mesh-local and each instance applies its own o2w
    // after the deform.
    const bool linIdentity =
        M[0] == 1.0f && M[1] == 0.0f && M[2] == 0.0f && M[4] == 0.0f &&
        M[5] == 1.0f && M[6] == 0.0f && M[8] == 0.0f && M[9] == 0.0f &&
        M[10] == 1.0f;
    if (loc.morphChannelCount > 0 && !linIdentity) {
      auto toHalf = [](float f) {
        return lightusd::value::float_to_half_full(f).value;
      };
      for (size_t e = 0; e + 3 < loc.morphDeltaHalf.size(); e += 4) {
        const float d[3] = {NextHalfToFloat(loc.morphDeltaHalf[e + 1]),
                            NextHalfToFloat(loc.morphDeltaHalf[e + 2]),
                            NextHalfToFloat(loc.morphDeltaHalf[e + 3])};
        for (int c = 0; c < 3; ++c) {  // a direction: no translation row
          loc.morphDeltaHalf[e + 1 + size_t(c)] =
              toHalf(d[0] * M[0 * 4 + c] + d[1] * M[1 * 4 + c] +
                     d[2] * M[2 * 4 + c]);
        }
      }
      // The extent pads a world-space box, so carry it through |M| (each world
      // axis takes the worst case over the local axes that feed it).
      const float le[3] = {loc.morphExtent[0], loc.morphExtent[1],
                           loc.morphExtent[2]};
      for (int a = 0; a < 3; ++a) {
        loc.morphExtent[a] = le[0] * std::fabs(M[0 * 4 + a]) +
                             le[1] * std::fabs(M[1 * 4 + a]) +
                             le[2] * std::fabs(M[2 * 4 + a]);
      }
    }
    // With no bound material, displayColor is the authored surface-color
    // fallback. Use a white material multiplier only for those meshes; changing
    // the shared gray default would also brighten every uncolored/unbound mesh.
    if (wholeMat == 0 && !loc.vertexColors.empty()) {
      wholeMat = displayColorMaterial();
    }

    // USD `primvars:displayOpacity`. When it does not actually vary, fold it
    // into an alpha-adjusted MATERIAL VARIANT (a clone of the bound material,
    // made once per distinct opacity). That renders correctly through the
    // existing material alpha, and folding it into the shared material in place
    // would be wrong when two meshes with different opacities share a material.
    // A genuinely per-vertex opacity keeps its buffer for the raster and RT
    // backends. Clamp before classifying so authoring such as [1, 2] is correctly
    // treated as fully opaque rather than needlessly entering the blend path.
    if (!loc.vertexAlpha.empty()) {
      for (float& a : loc.vertexAlpha)
        a = std::max(0.0f, std::min(1.0f, a));
      float lo = loc.vertexAlpha[0], hi = loc.vertexAlpha[0];
      for (float a : loc.vertexAlpha) { lo = std::min(lo, a); hi = std::max(hi, a); }
      if (hi - lo <= 1e-6f) {
        if (lo < 1.0f - 1e-6f) {
          wholeMat = materialWithAlpha(wholeMat, lo);
          if (wholeBackMat >= 0)
            wholeBackMat = materialWithAlpha(wholeBackMat, lo);
        }
        loc.vertexAlpha.clear();
        loc.vertexAlpha.shrink_to_fit();
      } else {
        ++varyingOpacityMeshes;
        // Clone instead of mutating a shared material. The factor is one: this
        // variant exists to move an otherwise-opaque material into the blend
        // pass; the actual modulation remains per vertex.
        wholeMat = materialWithAlpha(wholeMat, 1.0f);
        if (wholeBackMat >= 0)
          wholeBackMat = materialWithAlpha(wholeBackMat, 1.0f);
      }
    }

    // Preview-baked assets often bind one JPEG opacity atlas to an entire model.
    // Keep genuinely translucent UV islands in Blend, but promote a mesh whose
    // vertices and triangle interiors all sample the white JPEG plateau to an
    // opaque material variant. Otherwise compression noise (254 instead of 255)
    // puts solid parts such as frames and buckles in the transparent pass.
    if (wholeMat > 0 && static_cast<size_t>(wholeMat) < draw->materials.size()) {
      const DrawMaterialCPU& dm = draw->materials[static_cast<size_t>(wholeMat)];
      if (dm.alphaMode == static_cast<int>(AlphaMode::Blend) && dm.alpha >= 0.999f &&
          dm.opacityTex >= 0 &&
          static_cast<size_t>(dm.opacityTex) < draw->textures.size() &&
          dm.opacitySample.uvSet == 0) {
        const DrawTextureCPU& tex =
            draw->textures[static_cast<size_t>(dm.opacityTex)];
        std::string asset = tex.assetIdentifier;
        std::transform(asset.begin(), asset.end(), asset.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        const bool jpeg = asset.size() >= 4 &&
                          (asset.compare(asset.size() - 4, 4, ".jpg") == 0 ||
                           (asset.size() >= 5 &&
                            asset.compare(asset.size() - 5, 5, ".jpeg") == 0));
        const light3d::Image& image = tex.image;
        auto opaqueAt = [&](float u, float v) {
          u -= std::floor(u);
          v -= std::floor(v);
          const int x = std::min(image.width - 1, int(u * image.width));
          const int y = std::min(image.height - 1, int(v * image.height));
          const int channel = std::max(0, std::min(dm.opacityChannel,
                                                   image.channels - 1));
          const float value =
              image.data[(static_cast<size_t>(y) * image.width + size_t(x)) *
                             image.channels + size_t(channel)] /
                  255.0f * dm.opacityTexScale +
              dm.opacityTexBias;
          return value >= 250.0f / 255.0f;
        };
        bool opaqueCoverage = jpeg && image.width > 0 && image.height > 0 &&
                              image.channels > 0 && !image.data.empty();
        for (const DrawVertex& v : loc.vertices)
          opaqueCoverage = opaqueCoverage && opaqueAt(v.u, v.v);
        for (size_t i = 0; opaqueCoverage && i + 2 < loc.indices.size(); i += 3) {
          const DrawVertex& a = loc.vertices[loc.indices[i]];
          const DrawVertex& b = loc.vertices[loc.indices[i + 1]];
          const DrawVertex& c = loc.vertices[loc.indices[i + 2]];
          opaqueCoverage = opaqueAt((a.u + b.u + c.u) / 3.0f,
                                    (a.v + b.v + c.v) / 3.0f);
        }
        if (opaqueCoverage) wholeMat = materialWithoutOpacityMap(wholeMat);
      }
    }

    // Per-triangle materials from face GeomSubsets (empty => uniform wholeMat).
    std::vector<MaterialPair> triMat;
    // Most meshes have no children. Avoid materializing a temporary child
    // vector and scanning it through the subset reconstruction path for those
    // meshes; the child-count query is backed by the composed prim index and
    // does not allocate. GeomSubset meshes still take the exact existing path.
    if (hasSubsetMaterialBindings) {
      buildTriMaterials(mp, m, loc.indices.size() / 3, loc.sourceFaceId,
                        {wholeMat, wholeBackMat}, &triMat);
    }
    if (!loc.vertexAlpha.empty()) {
      for (MaterialPair& mid : triMat) {
        mid.first = materialWithAlpha(mid.first, 1.0f);
        if (mid.second >= 0)
          mid.second = materialWithAlpha(mid.second, 1.0f);
      }
    }
    bool needsPtex = MaterialUsesPtex(*draw, wholeMat) ||
                     MaterialUsesPtex(*draw, wholeBackMat);
    for (const MaterialPair& mid : triMat) {
      needsPtex = needsPtex || MaterialUsesPtex(*draw, mid.first) ||
                  MaterialUsesPtex(*draw, mid.second);
    }
    if (needsPtex && !ExpandPtexCorners(m, &loc)) {
      LOGW("Ptex material on '%s' requires unsupported non-quad or mismatched "
           "topology; using texture fallback",
           mp.GetPath().str().c_str());
    }

    const bool hasC = !loc.vertexColors.empty();
    const bool hasAlpha = loc.vertexAlpha.size() == loc.vertices.size();
    const bool hasUv1 = loc.uv1.size() == loc.vertices.size() * 2;
    const bool hasGeomProps = !loc.geomProps.empty();
    const bool hasSkin = loc.jointIdx.size() == loc.vertices.size() * 4 &&
                         loc.jointWt.size() == loc.vertices.size() * 4;
    const bool hasExtendedSkin =
        hasSkin && loc.influenceOffsetCount.size() == loc.vertices.size() * 2 &&
        !loc.influenceTexels.empty() && loc.influenceTexels.size() % 4 == 0;
    // Give `b` skin attribute arrays sized to the vertices it already holds
    // (zero-weight = unskinned), so the two arrays stay parallel to b.dm.vertices.
    auto openSkin = [&](Batch& b) {
      if (cpuSkinned) b.anyCpuSkin = true;
      if (hasSkin && !b.anySkin) {
        b.dm.jointIdx.assign(b.dm.vertices.size() * 4, 0u);
        b.dm.jointWt.assign(b.dm.vertices.size() * 4, 0.0f);
        b.anySkin = true;
      }
      if (hasExtendedSkin && !b.anyExtendedSkin) {
        b.dm.influenceOffsetCount.assign(b.dm.vertices.size() * 2, 0u);
        b.anyExtendedSkin = true;
      }
    };
    // Append vertex `i`'s influences (or zeros when this mesh is unskinned but
    // the batch is already carrying skin attributes).
    auto pushSkin = [&](Batch& b, size_t i) {
      if (!b.anySkin) return;
      for (size_t k = 0; k < 4; ++k) {
        b.dm.jointIdx.push_back(hasSkin ? loc.jointIdx[i * 4 + k] : 0u);
        b.dm.jointWt.push_back(hasSkin ? loc.jointWt[i * 4 + k] : 0.0f);
      }
      if (b.anyExtendedSkin) {
        if (hasExtendedSkin) {
          const uint32_t base = static_cast<uint32_t>(b.dm.influenceTexels.size() / 4);
          const uint32_t src = loc.influenceOffsetCount[i * 2 + 0];
          const uint32_t count = loc.influenceOffsetCount[i * 2 + 1];
          for (uint32_t k = 0; k < count; ++k) {
            const size_t e = static_cast<size_t>(src + k) * 4;
            if (e + 3 >= loc.influenceTexels.size()) break;
            b.dm.influenceTexels.insert(b.dm.influenceTexels.end(),
                                        loc.influenceTexels.begin() + e,
                                        loc.influenceTexels.begin() + e + 4);
          }
          const uint32_t actual =
              static_cast<uint32_t>(b.dm.influenceTexels.size() / 4) - base;
          b.dm.influenceOffsetCount.push_back(base);
          b.dm.influenceOffsetCount.push_back(actual);
          b.dm.maxInfluencesPerVertex =
              std::max(b.dm.maxInfluencesPerVertex, static_cast<int>(actual));
        } else {
          b.dm.influenceOffsetCount.push_back(0u);
          b.dm.influenceOffsetCount.push_back(0u);
        }
      }
    };
    // GPU morph. The channel metadata is per MESH, and a morphed mesh owns its
    // batch (see the key), so the batch simply inherits it; the per-vertex delta
    // lists are re-indexed as vertices are appended, since a GeomSubset split
    // routes different vertices to different batches.
    const bool hasMorph = loc.morphChannelCount > 0 &&
                          loc.morphOffsetCount.size() == loc.vertices.size() * 2;
    auto openMorph = [&](Batch& b) {
      if (!hasMorph || b.anyMorph) return;
      b.anyMorph = true;
      b.dm.morphChannelCount = loc.morphChannelCount;
      b.dm.morphTargetChannels = loc.morphTargetChannels;
      b.dm.absPath = loc.absPath;  // BuildNextMorphWeights resolves weights by path
      for (int k = 0; k < 3; ++k) b.dm.morphExtent[k] = loc.morphExtent[k];
      // Vertices already in the batch (there are none for a fresh morph batch, but
      // a flush can leave the slot reused) carry an empty delta list.
      b.dm.morphOffsetCount.assign(b.dm.vertices.size() * 2, 0u);
    };
    auto pushMorph = [&](Batch& b, size_t i) {
      if (!b.anyMorph) return;
      const uint32_t base =
          static_cast<uint32_t>(b.dm.morphDeltaHalf.size() / 4);
      const uint32_t src = loc.morphOffsetCount[i * 2 + 0];
      const uint32_t cnt = loc.morphOffsetCount[i * 2 + 1];
      for (uint32_t k = 0; k < cnt; ++k) {
        const size_t e = size_t(src + k);
        if (e * 4 + 3 >= loc.morphDeltaHalf.size()) break;
        for (int c = 0; c < 4; ++c)
          b.dm.morphDeltaHalf.push_back(loc.morphDeltaHalf[e * 4 + size_t(c)]);
        b.dm.morphChannelId.push_back(e < loc.morphChannelId.size()
                                          ? loc.morphChannelId[e]
                                          : uint16_t{0});
      }
      b.dm.morphOffsetCount.push_back(base);
      b.dm.morphOffsetCount.push_back(cnt);
    };

    if (triMat.empty()) {
      // --- Single-material fast path: append the whole mesh to one batch. ---
      // A Ptex file's face ids are local to one source mesh. Isolate that mesh
      // from ordinary static batching so face ids are never rebased into a
      // different texture's namespace.
      const int batchIsolationId =
          morphBatchId != 0
              ? morphBatchId
              : ((needsPtex || hasSkin || hasGeomProps) ? ++nextMorphBatchId : 0);
      Batch& b = getBatch({purpose, loc.geometricNormal,
                           meshRecord.proxyFallback, m.double_sided,
                           canonicalMaterialId(wholeMat),
                           canonicalMaterialId(wholeBackMat), batchIsolationId,
                           lightLinkBatchId, animatedWorldBatchId});
      if (!b.dm.vertices.empty() &&
          b.dm.vertices.size() + loc.vertices.size() > kBatchVtxCap) {
        flushBatch(b);  // resets b in the map slot
      }
      b.matId = wholeMat;
      b.backMatId = wholeBackMat;
      if (hasGeomProps && b.dm.vertices.empty())
        b.dm.geomProps = loc.geomProps;
      b.dm.purpose = purpose;
      b.dm.proxyFallback = meshRecord.proxyFallback;
      b.dm.geometricNormal = loc.geometricNormal;
      b.dm.doubleSided = m.double_sided;
      // Static/material batching can merge several source meshes into one draw,
      // but the draw still needs a representative prim for viewport selection.
      // Previously only animated/light-linked/morphed batches retained a path,
      // making ordinary and skinned next-loader draws impossible to select.
      if (b.dm.absPath.empty()) {
        b.dm.absPath = loc.absPath;
        b.dm.name = loc.name;
      }
      if (animatedWorld && b.dm.vertices.empty()) {
        b.animatedWorld = true;
        b.dm.animatedWorld = true;
        b.dm.absPath = loc.absPath;
        for (int k = 0; k < 16; ++k)
          b.dm.world[k] = static_cast<float>(mw16[k]);
      }
      if (hasAuthoredLightLinks) b.dm.absPath = loc.absPath;
      if (loc.instanceCount() > 0) {
        for (int k = 0; k < 3; ++k) {
          b.dm.protoAabbMin[k] = loc.protoAabbMin[k];
          b.dm.protoAabbMax[k] = loc.protoAabbMax[k];
        }
      }
      // Allocate the batch color buffer only once a mesh actually contributes a
      // color: back-fill white for the vertices already in the batch. No-color
      // batches (e.g. the hotel) then never allocate a 12 B/vertex white buffer.
      if (hasC && !b.anyColor) {
        b.dm.vertexColors.assign(b.dm.vertices.size() * 3, 1.0f);
        b.anyColor = true;
      }
      if (hasAlpha && !b.anyAlpha) {
        b.dm.vertexAlpha.assign(b.dm.vertices.size(), 1.0f);
        b.anyAlpha = true;
      }
      if (hasUv1 && !b.anyUv1) {
        b.dm.uv1.assign(b.dm.vertices.size() * 2, 0.0f);
        b.anyUv1 = true;
      }
      openSkin(b);
      openMorph(b);
      // NOTE: rely on the vectors' amortized (doubling) growth -- an exact
      // reserve(size()+n) per mesh would reallocate the whole batch (O(N^2)).
      const uint32_t vbase = static_cast<uint32_t>(b.dm.vertices.size());
      const bool plainStatic = !b.anySkin && !b.anyMorph && !b.anyColor &&
                               !b.anyAlpha && !b.anyUv1 && !hasSkin &&
                               !hasMorph && !hasC && !hasAlpha && !hasUv1;
      if (plainStatic) {
        const size_t required = b.dm.vertices.size() + loc.vertices.size();
        if (required > b.dm.vertices.capacity()) {
          b.dm.vertices.reserve(
              std::max(required, b.dm.vertices.capacity() * 2));
        }
        b.dm.vertices.insert(b.dm.vertices.end(),
                             std::make_move_iterator(loc.vertices.begin()),
                             std::make_move_iterator(loc.vertices.end()));
      } else {
        for (size_t i = 0; i < loc.vertices.size(); ++i) {
          b.dm.vertices.push_back(loc.vertices[i]);
          pushSkin(b, i);
          pushMorph(b, i);
          if (b.anyColor) {
            if (hasC) {
              b.dm.vertexColors.push_back(loc.vertexColors[3 * i + 0]);
              b.dm.vertexColors.push_back(loc.vertexColors[3 * i + 1]);
              b.dm.vertexColors.push_back(loc.vertexColors[3 * i + 2]);
            } else {
              b.dm.vertexColors.push_back(1.0f);
              b.dm.vertexColors.push_back(1.0f);
              b.dm.vertexColors.push_back(1.0f);
            }
          }
          if (b.anyAlpha)
            b.dm.vertexAlpha.push_back(hasAlpha ? loc.vertexAlpha[i] : 1.0f);
          if (b.anyUv1) {
            b.dm.uv1.push_back(hasUv1 ? loc.uv1[2 * i + 0] : 0.0f);
            b.dm.uv1.push_back(hasUv1 ? loc.uv1[2 * i + 1] : 0.0f);
          }
        }
      }
      const uint32_t logicalIndexOffset =
          static_cast<uint32_t>(b.dm.indices.size());
      for (uint32_t idx : loc.indices) b.dm.indices.push_back(vbase + idx);
      appendLogicalSubmesh(
          &b, logicalIndexOffset,
          static_cast<uint32_t>(b.dm.indices.size()) - logicalIndexOffset,
          wholeMat, wholeBackMat);
      const uint32_t faceBase = needsPtex ? 0u : b.nextFaceId;
      for (uint32_t face : loc.sourceFaceId)
        b.dm.sourceFaceId.push_back(faceBase + face);
      if (!needsPtex)
        b.nextFaceId += static_cast<uint32_t>(m.face_count());
      for (uint32_t widx : loc.wireframeIndices)
        b.dm.wireframeIndices.push_back(vbase + widx);
    } else {
      // --- Multi-material (GeomSubset) path: route each triangle to its
      //     material's batch, appending only the vertices that batch references
      //     (compacted per group so batches don't carry unused vertices). ---
      std::set<MaterialPair> groups(triMat.begin(), triMat.end());
      const size_t numTris = loc.indices.size() / 3;
      bool firstGroup = true;
      for (const MaterialPair& gm : groups) {
        const int batchIsolationId =
            morphBatchId != 0
                ? morphBatchId
                : ((needsPtex || hasSkin || hasGeomProps) ? ++nextMorphBatchId : 0);
        Batch& b = getBatch({purpose, loc.geometricNormal,
                             meshRecord.proxyFallback, m.double_sided,
                             canonicalMaterialId(gm.first),
                             canonicalMaterialId(gm.second), batchIsolationId,
                             lightLinkBatchId, animatedWorldBatchId});
        if (!b.dm.vertices.empty() &&
            b.dm.vertices.size() + loc.vertices.size() > kBatchVtxCap) {
          flushBatch(b);
        }
        b.matId = gm.first;
        b.backMatId = gm.second;
        b.dm.purpose = purpose;
        b.dm.proxyFallback = meshRecord.proxyFallback;
        b.dm.geometricNormal = loc.geometricNormal;
        b.dm.doubleSided = m.double_sided;
        if (b.dm.absPath.empty()) {
          b.dm.absPath = loc.absPath;
          b.dm.name = loc.name;
        }
        if (animatedWorld && b.dm.vertices.empty()) {
          b.animatedWorld = true;
          b.dm.animatedWorld = true;
          b.dm.absPath = loc.absPath;
          for (int k = 0; k < 16; ++k)
            b.dm.world[k] = static_cast<float>(mw16[k]);
        }
        if (hasAuthoredLightLinks) b.dm.absPath = loc.absPath;
        if (hasC && !b.anyColor) {
          b.dm.vertexColors.assign(b.dm.vertices.size() * 3, 1.0f);
          b.anyColor = true;
        }
        if (hasAlpha && !b.anyAlpha) {
          b.dm.vertexAlpha.assign(b.dm.vertices.size(), 1.0f);
          b.anyAlpha = true;
        }
        if (hasUv1 && !b.anyUv1) {
          b.dm.uv1.assign(b.dm.vertices.size() * 2, 0.0f);
          b.anyUv1 = true;
        }
        if (hasGeomProps && b.dm.geomProps.empty()) {
          b.dm.geomProps = loc.geomProps;
          for (DrawGeomPropCPU& prop : b.dm.geomProps) prop.values.clear();
        }
        openSkin(b);
        openMorph(b);
        const uint32_t faceBase = needsPtex ? 0u : b.nextFaceId;
        std::vector<int> remap(loc.vertices.size(), -1);
        auto vtx = [&](uint32_t vi) -> uint32_t {
          if (remap[vi] < 0) {
            remap[vi] = static_cast<int>(b.dm.vertices.size());
            b.dm.vertices.push_back(loc.vertices[vi]);
            pushSkin(b, vi);
            pushMorph(b, vi);
            if (b.anyColor) {
              if (hasC) {
                b.dm.vertexColors.push_back(loc.vertexColors[3 * vi + 0]);
                b.dm.vertexColors.push_back(loc.vertexColors[3 * vi + 1]);
                b.dm.vertexColors.push_back(loc.vertexColors[3 * vi + 2]);
              } else {
                b.dm.vertexColors.push_back(1.0f);
                b.dm.vertexColors.push_back(1.0f);
                b.dm.vertexColors.push_back(1.0f);
              }
            }
            if (b.anyAlpha)
              b.dm.vertexAlpha.push_back(hasAlpha ? loc.vertexAlpha[vi] : 1.0f);
            if (b.anyUv1) {
              b.dm.uv1.push_back(hasUv1 ? loc.uv1[2 * vi + 0] : 0.0f);
              b.dm.uv1.push_back(hasUv1 ? loc.uv1[2 * vi + 1] : 0.0f);
            }
            for (size_t pi = 0; pi < b.dm.geomProps.size(); ++pi) {
              const DrawGeomPropCPU& source = loc.geomProps[pi];
              const size_t begin = size_t(vi) * source.components;
              if (source.components == 0 ||
                  begin + source.components > source.values.size()) {
                b.dm.geomProps[pi].values.resize(
                    b.dm.geomProps[pi].values.size() + source.components, 0.0f);
                continue;
              }
              b.dm.geomProps[pi].values.insert(
                  b.dm.geomProps[pi].values.end(),
                  source.values.begin() + begin,
                  source.values.begin() + begin + source.components);
            }
          }
          return static_cast<uint32_t>(remap[vi]);
        };
        const uint32_t logicalIndexOffset =
            static_cast<uint32_t>(b.dm.indices.size());
        for (size_t t = 0; t < numTris; ++t) {
          if (triMat[t] != gm) continue;
          b.dm.indices.push_back(vtx(loc.indices[3 * t + 0]));
          b.dm.indices.push_back(vtx(loc.indices[3 * t + 1]));
          b.dm.indices.push_back(vtx(loc.indices[3 * t + 2]));
          b.dm.sourceFaceId.push_back(
              faceBase + (t < loc.sourceFaceId.size()
                              ? loc.sourceFaceId[t]
                              : static_cast<uint32_t>(t)));
        }
        appendLogicalSubmesh(
            &b, logicalIndexOffset,
            static_cast<uint32_t>(b.dm.indices.size()) - logicalIndexOffset,
            gm.first, gm.second);
        // Attach the mesh's wireframe once (to the first group's batch, mapped
        // through vtx so its vertices exist there) -- avoids cross-batch dupes.
        if (firstGroup) {
          for (uint32_t widx : loc.wireframeIndices)
            b.dm.wireframeIndices.push_back(vtx(widx));
          firstGroup = false;
        }
        if (!needsPtex)
          b.nextFaceId += static_cast<uint32_t>(m.face_count());
      }
    }

    // Provisional scene bounds, from the 8 local-bbox corners pushed through the
    // world matrix. Loose once the mesh is rotated (the axis-aligned hull of a
    // rotated box grows), and it is REPLACED below by the union of the batches'
    // own vertex boxes -- but a skinned batch reads `bounds` inside flushBatch (it
    // deliberately carries the whole-scene box), so it has to exist by then.
    const tydn::Float3& lo = m.bbox_min;
    const tydn::Float3& hi = m.bbox_max;
    for (int corner = 0; corner < 8; ++corner) {
      float lp[3] = {(corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                     (corner & 4) ? hi.z : lo.z};
      float wp[3];
      for (int c = 0; c < 3; ++c)
        wp[c] = lp[0] * M[0 * 4 + c] + lp[1] * M[1 * 4 + c] +
                lp[2] * M[2 * 4 + c] + M[3 * 4 + c];
      bounds.add(wp);
    }
    totalTris += static_cast<long long>(loc.indices.size() / 3);
    if (static_cast<std::size_t>(totalTris) > triCap) {
      draw->truncated = true;
      capped = true;
    }
    releasePendingPrim(&meshRecord.prim);
  }
  if (parallelConvert) {
    {
      std::lock_guard<std::mutex> lock(convertMutex);
      convertStop = true;
    }
    convertWork.notify_all();
    for (std::thread& worker : convertWorkers) worker.join();
  }
  // Return the large pending-record/result arrays before final batch flush and
  // renderer hand-off. Large scenes otherwise carry these alongside all emitted
  // geometry through the peak.
  std::vector<PendingMeshPrim>().swap(meshPrims);
  std::vector<size_t>().swap(geometryBytes);
  std::vector<std::unique_ptr<ConvertedMesh>>().swap(converted);
  std::vector<uint32_t>().swap(pendingPrimUses);
  // Every source mesh has now been converted and no worker can dereference its
  // geometry properties. Evict reconstructable defaults before the final open
  // batches are flushed so the composed arrays overlap with less renderer-bound
  // geometry. Volumes/lights/materials below use other prim types/properties.
  if (opts.maxMemoryBytes > 0 && session->IsComposed()) {
    tnext::Stage::StaticGeometryReleaseStats released =
        session->ReleaseStaticGeometryArrays();
    released.property_count += incrementalReleased.property_count;
    released.element_count += incrementalReleased.element_count;
    released.estimated_payload_bytes +=
        incrementalReleased.estimated_payload_bytes;
    if (stageBytesBeforeRelease > 0) {
      released.stage_bytes_before = stageBytesBeforeRelease;
    }
    if (timing && released.property_count > 0) {
      const size_t actual = released.stage_bytes_before >= released.stage_bytes_after
                                ? released.stage_bytes_before -
                                      released.stage_bytes_after
                                : 0;
      LOGI("next memory: released %zu static geometry arrays (%.1f MiB "
           "resident, %.1f MiB estimated payload); retained stage %.1f MiB",
           released.property_count,
           static_cast<double>(actual) / (1024.0 * 1024.0),
           static_cast<double>(released.estimated_payload_bytes) /
               (1024.0 * 1024.0),
           static_cast<double>(released.stage_bytes_after) /
               (1024.0 * 1024.0));
    }
  }
  if (parallelConvert) {
    LOGI("next: converted %zu meshes with %u workers in %.2f s "
         "(bounded chunks of %zu)",
         convertedSourceMeshCount, convertThreads, parallelConvertSeconds,
         convertChunk);
  }
  const auto meshesAt = std::chrono::steady_clock::now();
  if (timing) {
    LOGI("next timing: mesh convert/flatten/batch %.3f s",
         std::chrono::duration<double>(meshesAt - prototypesAt).count());
    LogProcessMemory("after mesh conversion");
  }

  if (budgetSkippedMeshCount > 0 && warn) {
    if (!warn->empty()) *warn += "\n";
    *warn += "next: skipped " + std::to_string(budgetSkippedMeshCount) +
             " mesh(es) exceeding the GPU geometry budget";
  }
  if (lazySkippedMeshCount > 0 && warn) {
    if (!warn->empty()) *warn += "\n";
    *warn += "next: deferred conversion of " +
             std::to_string(lazySkippedMeshCount) +
             " mesh(es) beyond the large-scene preview limit";
  }
  for (const BatchKey& key : batchOrder) flushBatch(open.find(key)->second);
  if (!streamOk) {
    if (err) *err = "next: progressive load cancelled";
    return false;
  }

  // Re-derive the scene box from the batches' own (tight, vertex-derived) boxes,
  // discarding the provisional corner-transformed one. The corner box is a strict
  // superset under rotation, and the scene box is not cosmetic: the ground grid is
  // sized from it, `--mode depth` is normalized by it, and the auto-fit frames on
  // it -- so a loose box here made the next loader draw the same geometry
  // differently from the Tydra path, which takes its box from vertices.
  // restAabb is the batch's tight box in every skinning mode (aabbMin/Max is not:
  // a skinned batch keeps the conservative scene box there on purpose); meshes
  // with no restAabb -- instanced prototypes -- carry a correct world box in aabb.
  {
    Bounds tight = streamedTightBounds;
    for (const DrawMeshCPU& dm : draw->meshes) {
      const bool haveRest = dm.restAabbMax[0] >= dm.restAabbMin[0] &&
                            (dm.restAabbMax[0] != dm.restAabbMin[0] ||
                             dm.restAabbMax[1] != dm.restAabbMin[1] ||
                             dm.restAabbMax[2] != dm.restAabbMin[2]);
      const float* blo = haveRest ? dm.restAabbMin : dm.aabbMin;
      const float* bhi = haveRest ? dm.restAabbMax : dm.aabbMax;
      if (!(bhi[0] >= blo[0]) || !std::isfinite(blo[0]) || !std::isfinite(bhi[0]))
        continue;
      const float pad[3] = {haveRest ? dm.morphExtent[0] : 0.0f,
                            haveRest ? dm.morphExtent[1] : 0.0f,
                            haveRest ? dm.morphExtent[2] : 0.0f};
      const float p0[3] = {blo[0] - pad[0], blo[1] - pad[1], blo[2] - pad[2]};
      const float p1[3] = {bhi[0] + pad[0], bhi[1] + pad[1], bhi[2] + pad[2]};
      tight.add(p0);
      tight.add(p1);
    }
    for (const DrawPointsCPU& points : draw->points) {
      if (std::isfinite(points.aabbMin[0]) &&
          std::isfinite(points.aabbMax[0])) {
        tight.add(points.aabbMin);
        tight.add(points.aabbMax);
      }
    }
    for (const DrawCurvesCPU& curves : draw->curves) {
      if (std::isfinite(curves.aabbMin[0]) &&
          std::isfinite(curves.aabbMax[0])) {
        tight.add(curves.aabbMin);
        tight.add(curves.aabbMax);
      }
    }
    if (streamedCarrierBounds.has) {
      tight.add(streamedCarrierBounds.mn);
      tight.add(streamedCarrierBounds.mx);
    }
    if (streamedVolumeBounds.has) {
      tight.add(streamedVolumeBounds.mn);
      tight.add(streamedVolumeBounds.mx);
    }
    if (tight.has) bounds = tight;
  }

  // UsdVol volumes (OpenVDB): emit DrawVolumeCPU + extend bounds.
  const size_t derivedVolumeBudget =
      opts.volumeMemoryBudgetBytes > 0
          ? opts.volumeMemoryBudgetBytes
          : (opts.gpuGeometryBudgetBytes > 0
                 ? std::max<size_t>(size_t(64) << 20,
                                    std::min<size_t>(size_t(512) << 20,
                                                     opts.gpuGeometryBudgetBytes / 8))
                 : size_t(512) << 20);
  size_t volumeDensityBytes = 0;
  if (!BuildNextVolumes(stage, path, time, draw, &bounds,
                        stream ? &publishVolume : nullptr,
                        derivedVolumeBudget, &volumeDensityBytes)) {
    if (err) *err = "next: progressive volume load cancelled";
    if (stream) streamOk = false;
    return false;
  }
  if (volumeDensityBytes > 0 && timing) {
    LOGI("next: UsdVol density resident %.1f MiB (budget %.1f MiB)",
         double(volumeDensityBytes) / (1024.0 * 1024.0),
         double(derivedVolumeBudget) / (1024.0 * 1024.0));
  }
  BuildNextLights(stage, conv, path, time, opts.textureOptions, draw);

  if (bounds.has) {
    for (int k = 0; k < 3; ++k) {
      draw->aabbMin[k] = bounds.mn[k]; draw->aabbMax[k] = bounds.mx[k];
    }
    draw->hasBounds = true;
  }

  // Derive the raster preview key light from the lights BuildNextLights added
  // (after bounds, so finite-light directions use the scene center).
  UpdatePreviewLight(draw);

  // Purpose breakdown (so the GUI's purpose toggles have something to hide).
  size_t nGuide = streamedGuideCount, nProxy = streamedProxyCount,
         nRender = streamedRenderCount;
  for (const DrawMeshCPU& dm : draw->meshes) {
    if (dm.purpose == "guide") ++nGuide;
    else if (dm.purpose == "proxy") ++nProxy;
    else if (dm.purpose == "render") ++nRender;
  }
  for (const DrawPointsCPU& dp : draw->points) {
    if (dp.purpose == "guide") ++nGuide;
    else if (dp.purpose == "proxy") ++nProxy;
    else if (dp.purpose == "render") ++nRender;
  }
  for (const DrawCurvesCPU& dc : draw->curves) {
    if (dc.purpose == "guide") ++nGuide;
    else if (dc.purpose == "proxy") ++nProxy;
    else if (dc.purpose == "render") ++nRender;
  }
  // Gather camera records for loader-equivalence testing (must run before the
  // early-exit checks below, since the stage may still be valid).
  GatherNextCameras(stage, time, opts.viewCamera, &texCache, draw,
                    &draw->cameras);

  LOGI("next: '%s' -> %zu draws (%zu guide, %zu proxy, %zu render), %lld instances, "
       "%zu unique tris (%lld effective), %zu materials, %zu textures, "
       "%zu cameras, instXform VRAM ~%.2f GB, up=%s%s",
       path.c_str(), streamedMeshCount + draw->meshes.size() +
                         streamedPointCount + streamedCurveCount,
       nGuide, nProxy,
       nRender, instTotal,
       draw->triangleCount, effectiveTris, draw->materials.size(),
       draw->textures.size(), draw->cameras.size(),
       double(instTotal) * 48.0 / 1e9,
       draw->upAxis.c_str(), draw->truncated ? " (truncated)" : "");
  if (!draw->points.empty() || !draw->curves.empty() ||
      streamedPointCount > 0 || streamedCurveCount > 0) {
    size_t pointSamples = 0, curveSamples = 0, opacityPrims = 0;
    size_t gaussianChunks = streamedGaussianChunks;
    size_t gaussianSamples = streamedGaussianSamples;
    size_t constantWidthCurves = 0;
    for (const DrawPointsCPU& p : draw->points) {
      pointSamples += p.points.size() / 3;
      opacityPrims += p.opacities.empty() ? 0 : 1;
      if (p.gaussian) {
        ++gaussianChunks;
        gaussianSamples += p.points.size() / 3;
      }
    }
    for (const DrawCurvesCPU& c : draw->curves) {
      curveSamples += c.points.size() / 3;
      opacityPrims += c.opacities.empty() ? 0 : 1;
      constantWidthCurves += c.widths.size() == 1 ? 1 : 0;
    }
    LOGI("next: retained %zu Points prim(s) / %zu samples and %zu Curves "
         "prim(s) / %zu tessellated samples",
         streamedPointCount + draw->points.size(),
         streamedPointSamples + pointSamples,
         streamedCurveCount + draw->curves.size(),
         streamedCurveSamples + curveSamples);
    if (opacityPrims > 0) {
      LOGI("next: retained displayOpacity for %zu non-mesh prim(s)",
           opacityPrims);
    }
    if (constantWidthCurves > 0) {
      LOGI("next: retained authored constant width for %zu Curves prim(s)",
           constantWidthCurves);
    }
    if (gaussianChunks > 0) {
      LOGI("next: Gaussian carriers %zu chunk(s), %zu visible samples",
           gaussianChunks, gaussianSamples);
    }
  }
  if (sourcePoints > 0) {
    LOGI("next: weld %zu vertices from %zu points (%.2fx)", weldedVertices,
         sourcePoints,
         double(weldedVertices) / double(sourcePoints));
  }
  if (varyingOpacityMeshes > 0)
    LOGI("next: %zu mesh(es) use varying per-vertex displayOpacity",
         varyingOpacityMeshes);
  // GPU block compression + content-aware mip chains. The size cap / byte budget
  // are already applied inside the --next texture decoder above, so only the
  // compression pass runs here; without this `--texture-compress` would be inert
  // on the (default) --next path, which builds its textures itself instead of
  // going through mesh_build's BuildDrawTextures.
  if (ctrl) {
    ctrl->detailPhase.store(static_cast<int>(LoadDetailPhase::ProcessingTextures));
    ctrl->texturesTotal.store(static_cast<long long>(draw->textures.size()));
  }
  const auto textureFinalizeBegin = std::chrono::steady_clock::now();
  size_t decodedTextureBytes = 0;
  for (const DrawTextureCPU& tex : draw->textures) {
    auto imageBytes = [](const light3d::Image& image) -> size_t {
      if (!image.data.empty()) return image.data.size();
      if (image.width <= 0 || image.height <= 0 || image.channels <= 0)
        return 0;
      return static_cast<size_t>(image.width) *
             static_cast<size_t>(image.height) *
             static_cast<size_t>(image.channels);
    };
    if (tex.isUdim && !tex.udimTiles.empty()) {
      for (const DrawUdimTileCPU& tile : tex.udimTiles)
        decodedTextureBytes += imageBytes(tile.image);
    } else {
      decodedTextureBytes += imageBytes(tex.image);
    }
  }
  // --texture-fit decides whether the scene is left alone. CPU block
  // compression is the single most expensive stage of a texture-heavy load
  // (ALab alab_set01: 362 s of a 395 s load, 507 textures 2028 MB -> 507 MB,
  // still 156 s once threaded), so it is only worth paying when the scene would
  // not otherwise fit. Geometry and textures share the device, so both go on
  // the scales.
  const size_t vramCapacityBytes = opts.textureGpuBudgetBytes;
  const size_t fitThreshold = opts.textureFitThresholdBytes;
  const size_t residentEstimate =
      (estimatedGeometryBytes > (std::numeric_limits<size_t>::max)() -
                                    decodedTextureBytes)
          ? (std::numeric_limits<size_t>::max)()
          : estimatedGeometryBytes + decodedTextureBytes;
  // fitThreshold: 0 == "always process", SIZE_MAX == "never process".
  const bool sceneFitsInVram = opts.optimizeTextureUpload && fitThreshold > 0 &&
                               residentEstimate <= fitThreshold;
  // The mip decision deliberately does NOT follow --texture-fit. It keeps the
  // narrow 25%-of-VRAM comfort budget, because skipping mips makes every later
  // frame sample minified textures at full resolution and thrashes the texture
  // cache -- widening this once took the texture-semantic AOV suite from 52 s to
  // over 300 s. textureComfortBytes is derived independently of the policy for
  // exactly this reason; do not "simplify" it to textureBudgetMB, which now
  // carries the (much larger) policy threshold.
  const size_t comfortBytes =
      opts.textureComfortBytes > 0
          ? opts.textureComfortBytes
          : (opts.textureOptions.textureBudgetMB > 0
                 ? static_cast<size_t>(opts.textureOptions.textureBudgetMB) *
                       1024ull * 1024ull
                 : opts.textureGpuBudgetBytes);
  const bool alwaysProcess =
      opts.textureFit.policy ==
      lightusd::tydra::next::TextureFitPolicy::Always;
  const bool texturesFitComfortably = opts.optimizeTextureUpload &&
                                      !alwaysProcess && comfortBytes > 0 &&
                                      decodedTextureBytes <= comfortBytes / 2u;
  const bool skipCompression =
      (texturesFitComfortably || sceneFitsInVram) &&
      !opts.textureCompressionExplicit &&
      opts.textureOptions.compression == TextureCompressionMode::Auto;
  const bool skipMips = texturesFitComfortably && !opts.textureMipsExplicit;
  TextureRuntimeOptions processingOptions = opts.textureOptions;
  if (skipCompression) processingOptions.compression = TextureCompressionMode::Off;
  if (skipMips) processingOptions.generateMips = false;
  if (skipCompression || skipMips) ClassifyTextureUsage(draw);
  {
    // Printed unconditionally: the decision was previously invisible unless
    // something was skipped, which made it undiagnosable from a log.
    const uint32_t pct =
        lightusd::tydra::next::TextureFitPercent(opts.textureFit);
    char fitLabel[64];
    if (pct > 0) {
      std::snprintf(fitLabel, sizeof(fitLabel), "%s (%u%% of VRAM)",
                    lightusd::tydra::next::TextureFitName(opts.textureFit), pct);
    } else {
      std::snprintf(fitLabel, sizeof(fitLabel), "%s",
                    lightusd::tydra::next::TextureFitName(opts.textureFit));
    }
    char thresholdBuf[64];
    if (fitThreshold == (std::numeric_limits<size_t>::max)()) {
      std::snprintf(thresholdBuf, sizeof(thresholdBuf), "unbounded");
    } else {
      std::snprintf(thresholdBuf, sizeof(thresholdBuf), "%.1f MiB",
                    double(fitThreshold) / (1024.0 * 1024.0));
    }
    LOGI("next: texture-fit=%s %zu textures, %.1f MiB decoded + %.1f MiB "
         "geometry = %.1f MiB vs %s threshold (VRAM %.1f MiB, comfort %.1f MiB)"
         "; %s compression, %s mips; decoder max_edge=%u, %llu downscaled%s",
         fitLabel,
         draw->textures.size(),
         double(decodedTextureBytes) / (1024.0 * 1024.0),
         double(estimatedGeometryBytes) / (1024.0 * 1024.0),
         double(residentEstimate) / (1024.0 * 1024.0), thresholdBuf,
         double(vramCapacityBytes) / (1024.0 * 1024.0),
         double(comfortBytes) / (1024.0 * 1024.0),
         skipCompression ? "skip" : "keep", skipMips ? "skip" : "keep",
         texCache.decoder ? texCache.decoder->options().max_edge : 0u,
         static_cast<unsigned long long>(
             texCache.decoder ? texCache.decoder->downscaled_count() : 0ull),
         texCache.deferOrdinary ? " [deferred decode: totals not yet final]"
                                : "");
  }
  if (texCache.deferOrdinary) {
    // Classify normal/alpha/packed-map usage while material slot indices are
    // intact. The application owns decoding from here: it can prioritize the
    // current frustum/selection and evict under the live GPU residency budget.
    FinalizeDrawTextures(processingOptions, draw);
  } else {
    ApplyTextureCompression(processingOptions, draw);
    FinalizeDrawTextures(processingOptions, draw);
  }
  if (ctrl) {
    ctrl->texturesTotal.store(static_cast<long long>(draw->textures.size()));
    ctrl->detailPhase.store(static_cast<int>(LoadDetailPhase::Finalizing));
  }
  if (timing) {
    LOGI("next timing: texture processing %.3f s (%zu texture(s), %lld/%lld completed)",
         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       textureFinalizeBegin).count(),
         draw->textures.size(),
         ctrl ? ctrl->texturesDone.load() :
                static_cast<long long>(draw->textures.size()),
         ctrl ? ctrl->texturesTotal.load() :
                static_cast<long long>(draw->textures.size()));
  }
  BakeRTDisplacement(draw);
  CanonicalizeDrawMaterials(draw);
  publishAvailableTextures();
  if (!streamOk) {
    if (err) *err = "next: progressive load cancelled";
    return false;
  }

  if (texCache.decoder && !draw->textures.empty()) {
    const tydn::TextureDecoder& dec = *texCache.decoder;
    // TextureDecoder::decoded_bytes() is transient decode residency. Its lease
    // is released when DecodeNextImage moves the pixels into DrawTextureCPU,
    // so it normally reaches zero here even though the scene still owns all of
    // those pixels. Report the retained payload measured above instead. Also
    // query the live budget: SetBudgetBytes() may have reduced it after the
    // geometry estimate, while options().budget_bytes remains the initial cap.
    LOGI("next: textures %zu, retained decoded %.1f MB (cap %u px, budget %.0f MB, "
         "%llu downscaled)",
         draw->textures.size(),
         double(decodedTextureBytes) / (1024.0 * 1024.0),
         dec.options().max_edge,
         double(dec.budget_bytes()) / (1024.0 * 1024.0),
         static_cast<unsigned long long>(dec.downscaled_count()));
  }
  if (streamedTexturePayloadCount > 0) {
    LOGI("next: progressively queued %zu decoded texture payload(s) under the "
         "shared stream budget",
         streamedTexturePayloadCount);
  }

  // A stage whose only renderable content lives below a deferred payload is a
  // valid initial composition. Keep its session alive so MCP/UI payload loading
  // can recompose it on demand; only reject truly empty, non-deferred stages.
  if (streamedMeshCount + draw->meshes.size() == 0 &&
      streamedPointCount == 0 && streamedCurveCount == 0 &&
      streamedVolumeCount == 0 && draw->points.empty() &&
      draw->curves.empty() && draw->volumes.empty() &&
      deferredPayloads.empty()) {
    if (err) *err = "next: no renderable geometry produced";
    if (stream)
      stream->pushFailed(err ? *err : "next: no renderable geometry produced");
    return false;
  }
  if (timing) {
    LOGI("next timing: ptex atlas build %.3f s (%zu file(s))",
         texCache.ptexBuildSeconds, texCache.ptexBuildCount);
    LOGI("next timing: finalize %.3f s, total %.3f s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      meshesAt).count(),
         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      loadBegin).count());
    LogProcessMemory("after finalize");
  }
  if (stream) {
    publishAvailableMeshes(/*force=*/true);
    publishAvailableNonMeshes();
    if (!streamOk) {
      if (err) *err = "next: progressive load cancelled";
      return false;
    }
    if (streamedMaterialCount != draw->materials.size() ||
        streamedTextureCount != draw->textures.size()) {
      streamOk = stream->pushResources(
          draw->materials, static_cast<int>(draw->textures.size()), draw->upAxis);
      if (!streamOk) {
        if (err) *err = "next: progressive load cancelled";
        return false;
      }
    }
    stream->pushComplete(std::move(*draw));
  }
  return true;
}

}  // namespace lusdview
