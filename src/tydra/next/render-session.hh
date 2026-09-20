// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Persistent, revisioned next::Stage -> Tydra render update conversion.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render-converter.hh"
#include "next/operation-status.hh"
#include "next/stage/change-set.hh"

namespace lightusd {
namespace tydra {
namespace next {

using RenderId = uint64_t;
constexpr RenderId kInvalidRenderId = 0;

enum class RenderResourceKind : uint8_t {
  Node = 0,
  Mesh,
  Points,
  Curves,
  PointInstancer,
  Material,
  Texture,
  Image,
  Light,
  Camera,
  Animation,
  Skeleton,
};

struct RemovedRenderResource {
  RenderResourceKind kind = RenderResourceKind::Node;
  RenderId id = kInvalidRenderId;
  std::string key;
};

/// Transactional destination for incremental render resources. A failed call
/// aborts the update; the sink must keep its previously committed revision.
class SceneUpdateSink {
 public:
  virtual ~SceneUpdateSink() = default;
  virtual bool BeginUpdate(uint64_t base_revision, uint64_t new_revision,
                           bool full_resync) = 0;
  virtual bool UpdateCatalog(const RenderScene&) { return true; }
  virtual bool Remove(const RemovedRenderResource&) { return true; }
  virtual bool UpsertNode(RenderId, const SceneNode&) { return true; }
  virtual bool UpsertMesh(RenderId, const RenderMesh&) { return true; }
  virtual bool UpsertPoints(RenderId, const RenderPoints&) { return true; }
  virtual bool UpsertCurves(RenderId, const RenderCurves&) { return true; }
  virtual bool UpsertPointInstancer(RenderId, const RenderPointInstancer&) {
    return true;
  }
  virtual bool UpsertMaterial(RenderId, const RenderMaterial&) { return true; }
  virtual bool UpsertTexture(RenderId, const RenderTexture&) { return true; }
  virtual bool UpsertImage(RenderId, const TextureImage&) { return true; }
  virtual bool UpsertLight(RenderId, const RenderLight&) { return true; }
  virtual bool UpsertCamera(RenderId, const RenderCamera&) { return true; }
  virtual bool UpsertAnimation(RenderId, const AnimationClip&) { return true; }
  virtual bool UpsertSkeleton(RenderId, const Skeleton&) { return true; }
  virtual bool EndUpdate() = 0;
  virtual void AbortUpdate() {}
};

struct RenderUpdateResult {
  bool success = false;
  ::lightusd::next::OperationStatus status =
      ::lightusd::next::OperationStatus::InvalidData;
  bool cancelled = false;
  uint64_t revision = 0;
  size_t upsert_count = 0;
  size_t remove_count = 0;
  // Observability for incremental consumers. converted_resource_count is the
  // number of retained resources produced by conversion (zero for a no-op),
  // while upsert_count is the smaller set emitted to the sink.
  size_t converted_resource_count = 0;
  size_t converted_scene_bytes = 0;
  bool full_resync = false;
  std::string error;
  std::vector<std::string> warnings;

  explicit operator bool() const { return success; }
};

struct RenderSceneSnapshot {
  uint64_t revision = 0;
  std::shared_ptr<const RenderScene> scene;

  explicit operator bool() const { return static_cast<bool>(scene); }
  const RenderScene* operator->() const { return scene.get(); }
  const RenderScene& operator*() const { return *scene; }
};

/// Move-only candidate produced by RenderSession::Prepare(). Conversion and
/// allocation are complete, but neither the sink nor the published session
/// revision has been changed. Destroying or aborting a candidate is a no-op.
class PreparedRenderUpdate {
 public:
  PreparedRenderUpdate();
  ~PreparedRenderUpdate();
  PreparedRenderUpdate(PreparedRenderUpdate&&) noexcept;
  PreparedRenderUpdate& operator=(PreparedRenderUpdate&&) noexcept;
  PreparedRenderUpdate(const PreparedRenderUpdate&) = delete;
  PreparedRenderUpdate& operator=(const PreparedRenderUpdate&) = delete;

  explicit operator bool() const;
  uint64_t base_revision() const;
  uint64_t new_revision() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class RenderSession;
};

/// Owns stable resource IDs and the last committed retained RenderScene.
class RenderSession {
 public:
  explicit RenderSession(const ConverterConfig& config = {});
  ~RenderSession();
  RenderSession(RenderSession&&) noexcept;
  RenderSession& operator=(RenderSession&&) noexcept;
  RenderSession(const RenderSession&) = delete;
  RenderSession& operator=(const RenderSession&) = delete;

  RenderUpdateResult Initialize(
      const ::lightusd::next::StageSnapshot& snapshot,
      SceneUpdateSink* sink);
  RenderUpdateResult Apply(const ::lightusd::next::StageSnapshot& snapshot,
                           const ::lightusd::next::StageChangeSet& changes,
                           SceneUpdateSink* sink);

  /// Convert and allocate a candidate without invoking the sink or publishing
  /// it. Commit may run later on the render thread. A candidate becomes stale
  /// if another update commits first.
  RenderUpdateResult Prepare(
      const ::lightusd::next::StageSnapshot& snapshot,
      const ::lightusd::next::StageChangeSet& changes,
      PreparedRenderUpdate* prepared);
  RenderUpdateResult PrepareInitialize(
      const ::lightusd::next::StageSnapshot& snapshot,
      PreparedRenderUpdate* prepared);
  /// Emit a prepared candidate to the sink and publish it only after EndUpdate
  /// succeeds. Failure calls AbortUpdate and preserves the old revision/scene.
  RenderUpdateResult Commit(PreparedRenderUpdate&& prepared,
                            SceneUpdateSink* sink);
  void Abort(PreparedRenderUpdate* prepared);

  RenderSceneSnapshot GetSnapshot() const;
  uint64_t revision() const;
  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
