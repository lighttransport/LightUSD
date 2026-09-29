// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "render-session.hh"

#include <algorithm>
#include <map>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <utility>

#include "next/stage/stage.hh"
#include "render-converter.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace {

using IdMap = std::unordered_map<std::string, RenderId>;
class RenderOperationScope;
thread_local const RenderOperationScope* active_render_operation = nullptr;

class RenderOperationScope {
 public:
  explicit RenderOperationScope(const void* session)
      : session_(session), previous_(active_render_operation) {
    active_render_operation = this;
  }
  ~RenderOperationScope() { active_render_operation = previous_; }

  bool Contains(const void* session) const {
    for (const RenderOperationScope* scope = this; scope;
         scope = scope->previous_) {
      if (scope->session_ == session) return true;
    }
    return false;
  }

 private:
  const void* session_;
  const RenderOperationScope* previous_;
};

bool RenderOperationActive(const void* session) {
  return active_render_operation && active_render_operation->Contains(session);
}

bool PathRelated(const std::string& key, const std::string& changed) {
  if (changed.empty() || changed == "/") return true;
  auto under = [](const std::string& value, const std::string& root) {
    return value == root ||
           (value.size() > root.size() &&
            value.compare(0, root.size(), root) == 0 &&
            value[root.size()] == '/');
  };
  return under(key, changed) || under(changed, key);
}

std::string KeyOrIndex(const std::string& key, size_t index,
                       const char* prefix) {
  if (!key.empty()) return key;
  return std::string(prefix) + std::to_string(index);
}

bool KindAffected(RenderResourceKind kind,
                  ::lightusd::next::StageChangeFlag flags) {
  using Flag = ::lightusd::next::StageChangeFlag;
  if (::lightusd::next::HasStageChange(flags, Flag::Resync)) return true;
  switch (kind) {
    case RenderResourceKind::Node:
      return ::lightusd::next::HasStageChange(flags, Flag::Transform) ||
             ::lightusd::next::HasStageChange(flags, Flag::Visibility) ||
             ::lightusd::next::HasStageChange(flags, Flag::Metadata);
    case RenderResourceKind::Mesh:
    case RenderResourceKind::Points:
    case RenderResourceKind::Curves:
    case RenderResourceKind::PointInstancer:
      return ::lightusd::next::HasStageChange(flags, Flag::Topology) ||
             ::lightusd::next::HasStageChange(flags, Flag::Primvar) ||
             ::lightusd::next::HasStageChange(flags, Flag::Transform) ||
             ::lightusd::next::HasStageChange(flags, Flag::Animation) ||
             ::lightusd::next::HasStageChange(flags, Flag::Visibility);
    case RenderResourceKind::Material:
      return ::lightusd::next::HasStageChange(flags, Flag::Material);
    case RenderResourceKind::Texture:
    case RenderResourceKind::Image:
      return ::lightusd::next::HasStageChange(flags, Flag::Texture) ||
             ::lightusd::next::HasStageChange(flags, Flag::Material);
    case RenderResourceKind::Light:
      return ::lightusd::next::HasStageChange(flags, Flag::Light) ||
             ::lightusd::next::HasStageChange(flags, Flag::Transform) ||
             ::lightusd::next::HasStageChange(flags, Flag::Animation);
    case RenderResourceKind::Camera:
      return ::lightusd::next::HasStageChange(flags, Flag::Camera) ||
             ::lightusd::next::HasStageChange(flags, Flag::Transform) ||
             ::lightusd::next::HasStageChange(flags, Flag::Animation);
    case RenderResourceKind::Animation:
    case RenderResourceKind::Skeleton:
      return ::lightusd::next::HasStageChange(flags, Flag::Animation) ||
             ::lightusd::next::HasStageChange(flags, Flag::Topology);
  }
  return true;
}

}  // namespace

struct PreparedRenderUpdate::Impl {
  std::shared_ptr<const void> owner;
  uint64_t base_revision = 0;
  uint64_t new_revision = 0;
  ::lightusd::next::StageChangeSet changes;
  std::shared_ptr<RenderScene> scene;
  std::map<RenderResourceKind, IdMap> ids;
  RenderId next_id = 1;
  RenderUpdateResult result;
};

namespace {
class AcceptSceneUpdateSink final : public SceneUpdateSink {
 public:
  bool BeginUpdate(uint64_t, uint64_t, bool) override { return true; }
  bool EndUpdate() override { return true; }
};
}  // namespace

struct RenderSession::Impl {
  explicit Impl(const ConverterConfig& config)
      : converter(config), max_resident_bytes(config.limits.max_resident_bytes),
        transaction_owner(new int(0)) {}

  mutable std::recursive_mutex operation_mu;
  mutable std::mutex publication_mu;
  RenderSceneConverter converter;
  size_t max_resident_bytes;
  std::shared_ptr<int> transaction_owner;
  std::shared_ptr<RenderScene> scene;
  uint64_t revision = 0;
  RenderId next_id = 1;
  std::map<RenderResourceKind, IdMap> ids;

  bool IsAffected(RenderResourceKind kind, const std::string& key,
                  const ::lightusd::next::StageChangeSet& changes) const {
    if (revision == 0 || changes.full_resync ||
        changes.base_revision != revision) {
      return true;
    }
    for (const auto& change : changes.prims) {
      if (KindAffected(kind, change.flags) &&
          PathRelated(key, change.path.str())) {
        return true;
      }
    }
    return changes.stage_metadata_changed && kind == RenderResourceKind::Node;
  }

  bool HasRenderEffect(const ::lightusd::next::StageChangeSet& changes) const {
    if (changes.full_resync) return true;
    if (changes.stage_metadata_changed) return true;
    for (const auto& change : changes.prims) {
      if (::lightusd::next::HasStageChange(
              change.flags, ::lightusd::next::StageChangeFlag::Resync)) {
        return true;
      }
      for (const auto& kind_map : ids) {
        for (const auto& resource : kind_map.second) {
          if (KindAffected(kind_map.first, change.flags) &&
              PathRelated(resource.first, change.path.str())) {
            return true;
          }
        }
      }
    }
    return false;
  }

  bool CanPatchMeshes(const ::lightusd::next::StageChangeSet& changes,
                      std::vector<size_t>* affected) const {
    using Flag = ::lightusd::next::StageChangeFlag;
    if (!scene || revision == 0 || changes.full_resync ||
        changes.stage_metadata_changed || changes.prims.empty() || !affected) {
      return false;
    }
    constexpr uint32_t allowed = static_cast<uint32_t>(Flag::Topology) |
                                 static_cast<uint32_t>(Flag::Primvar);
    std::set<size_t> unique;
    for (const auto& change : changes.prims) {
      const uint32_t flags = static_cast<uint32_t>(change.flags);
      if (flags == 0 || (flags & ~allowed) != 0) return false;
      const std::string path = change.path.str();
      bool found = false;
      for (size_t i = 0; i < scene->meshes.size(); ++i) {
        if (scene->meshes[i].prim_path == path) {
          // GeomSubset ranges live in topology space and require the full
          // material-binding finalization pass after a topology edit. Skinned
          // and morphed meshes likewise require skeleton/channel finalization.
          if (!scene->meshes[i].material_subsets.empty() ||
              scene->meshes[i].has_skin() ||
              scene->meshes[i].has_blend_shapes()) {
            return false;
          }
          unique.insert(i);
          found = true;
          break;
        }
      }
      // A topology edit that adds/removes a renderable needs the full catalog
      // extraction path so hierarchy and resource maps are rebuilt together.
      if (!found) return false;
    }
    affected->assign(unique.begin(), unique.end());
    return !affected->empty();
  }

  RenderUpdateResult ApplyNoOp(
      const ::lightusd::next::StageSnapshot& snapshot, SceneUpdateSink* sink,
      bool publish, std::shared_ptr<RenderScene>* prepared_scene,
      std::map<RenderResourceKind, IdMap>* prepared_ids,
      RenderId* prepared_next_id) {
    RenderUpdateResult out;
    out.revision = revision;
    out.full_resync = false;
    if (!sink->BeginUpdate(revision, snapshot.revision, false) ||
        !scene || !sink->UpdateCatalog(*scene) || !sink->EndUpdate()) {
      out.error = "RenderSession: sink rejected no-op update";
      out.status = ::lightusd::next::OperationStatus::SinkRejected;
      sink->AbortUpdate();
      return out;
    }
    if (publish) {
      std::lock_guard<std::mutex> lock(publication_mu);
      revision = snapshot.revision;
      out.revision = revision;
    } else {
      if (prepared_scene) *prepared_scene = scene;
      if (prepared_ids) *prepared_ids = ids;
      if (prepared_next_id) *prepared_next_id = next_id;
      out.revision = snapshot.revision;
    }
    out.success = true;
    out.status = ::lightusd::next::OperationStatus::Ok;
    return out;
  }

  RenderId IdFor(RenderResourceKind kind, const std::string& key,
                 IdMap* next_keys, RenderId* candidate_next_id,
                 bool* candidate_exhausted) const {
    const auto kind_it = ids.find(kind);
    const IdMap* current = kind_it == ids.end() ? nullptr : &kind_it->second;
    const auto found = current ? current->find(key) : IdMap::const_iterator{};
    RenderId id = current && found != current->end() ? found->second
                                                     : kInvalidRenderId;
    if (!current || found == current->end()) {
      if (*candidate_next_id == (std::numeric_limits<RenderId>::max)()) {
        *candidate_exhausted = true;
        return kInvalidRenderId;
      }
      id = (*candidate_next_id)++;
    }
    (*next_keys)[key] = id;
    return id;
  }

  template <typename T, typename KeyFn, typename EmitFn>
  bool EmitVector(RenderResourceKind kind, const std::vector<T>& values,
                  const ::lightusd::next::StageChangeSet& changes,
                  KeyFn key_fn, EmitFn emit,
                  IdMap* next_keys, size_t* upserts,
                  RenderId* candidate_next_id,
                  bool* candidate_exhausted) {
    std::unordered_map<std::string, size_t> occurrences;
    for (size_t i = 0; i < values.size(); ++i) {
      std::string key = key_fn(values[i], i);
      const size_t occurrence = occurrences[key]++;
      if (occurrence != 0) {
        key += "#" + std::to_string(occurrence);
      }
      const RenderId id = IdFor(kind, key, next_keys, candidate_next_id,
                                candidate_exhausted);
      if (id == kInvalidRenderId) return false;
      if (IsAffected(kind, key, changes)) {
        if (!emit(id, values[i])) return false;
        ++*upserts;
      }
    }
    return true;
  }

  RenderUpdateResult Apply(const ::lightusd::next::StageSnapshot& snapshot,
                           const ::lightusd::next::StageChangeSet& requested,
                           SceneUpdateSink* sink, bool publish = true,
                           std::shared_ptr<RenderScene>* prepared_scene = nullptr,
                           std::map<RenderResourceKind, IdMap>* prepared_ids = nullptr,
                           RenderId* prepared_next_id = nullptr) {
    RenderUpdateResult out;
    out.revision = revision;
    if (!snapshot || !sink) {
      out.error = !snapshot ? "RenderSession: invalid stage snapshot"
                            : "RenderSession: null update sink";
      out.status = ::lightusd::next::OperationStatus::InvalidArgument;
      return out;
    }
    if (revision != 0 && snapshot.revision <= revision) {
      out.error = "RenderSession: snapshot revision is not newer";
      out.status = ::lightusd::next::OperationStatus::StaleRevision;
      return out;
    }

    ::lightusd::next::StageChangeSet changes = requested;
    if (revision == 0 || changes.base_revision != revision ||
        changes.new_revision != snapshot.revision) {
      changes.base_revision = revision;
      changes.new_revision = snapshot.revision;
      changes.full_resync = true;
    }
    out.full_resync = changes.full_resync;

    // A transaction may advance the immutable Stage revision without changing
    // any render-affecting data (for example an edit batch that resolves to the
    // already-authored value). Preserve the sink's revision protocol without
    // paying for a complete Stage -> RenderScene conversion.
    if (revision != 0 && (changes.empty() || !HasRenderEffect(changes))) {
      return ApplyNoOp(snapshot, sink, publish, prepared_scene, prepared_ids,
                       prepared_next_id);
    }

    std::vector<size_t> patched_meshes;
    const bool mesh_patch = CanPatchMeshes(changes, &patched_meshes);
    std::shared_ptr<RenderScene> committed;
    if (mesh_patch) {
      committed.reset(new (std::nothrow) RenderScene(*scene));
      if (committed) {
        for (size_t index : patched_meshes) {
          const std::string path = committed->meshes[index].prim_path;
          const ::lightusd::next::UsdPrim prim =
              snapshot.stage->GetPrimAtPath(path);
          RenderMesh replacement;
          if (!converter.ConvertRenderableMesh(*snapshot.stage, prim,
                                               &replacement)) {
            out.error = converter.GetLastError();
            out.status = ::lightusd::next::OperationStatus::InvalidData;
            return out;
          }
          // Geometry-only changes retain the already resolved material table.
          replacement.material_id = committed->meshes[index].material_id;
          replacement.material_subsets =
              committed->meshes[index].material_subsets;
          out.converted_scene_bytes += replacement.memory_usage();
          committed->meshes[index] = std::move(replacement);
        }
        out.converted_resource_count = patched_meshes.size();
      }
    } else {
      ConvertResult converted = converter.Convert(*snapshot.stage);
      out.warnings = converted.warnings;
      if (!converted.success) {
        out.error = converted.error;
        out.status = converted.status;
        return out;
      }
      committed.reset(
          new (std::nothrow) RenderScene(std::move(converted.scene)));
    }
    if (!committed) {
      out.error = "RenderSession: unable to prepare render scene";
      out.status = ::lightusd::next::OperationStatus::AllocationFailure;
      return out;
    }
    RenderScene& next = *committed;
    const size_t retained_bytes = next.memory_usage();
    if (retained_bytes > max_resident_bytes) {
      out.error = "RenderSession: candidate scene exceeds resident memory limit";
      out.status = ::lightusd::next::OperationStatus::ResourceLimit;
      return out;
    }
    if (!mesh_patch) {
      out.converted_resource_count =
          next.images.size() + next.textures.size() + next.materials.size() +
          next.meshes.size() + next.points.size() + next.curves.size() +
          next.point_instancers.size() + next.skeletons.size() +
          next.animations.size() + next.lights.size() + next.cameras.size() +
          next.nodes.size();
      out.converted_scene_bytes = retained_bytes;
    }
    if (!sink->BeginUpdate(revision, snapshot.revision, changes.full_resync) ||
        !sink->UpdateCatalog(next)) {
      out.error = "RenderSession: sink rejected update start/catalog";
      out.status = ::lightusd::next::OperationStatus::SinkRejected;
      sink->AbortUpdate();
      return out;
    }

    std::map<RenderResourceKind, IdMap> next_ids;
    RenderId candidate_next_id = next_id;
    bool candidate_exhausted = false;
    bool ok = true;
#define EMIT_VECTOR(KIND, MEMBER, KEY, METHOD)                                  \
    ok = ok && EmitVector(RenderResourceKind::KIND, next.MEMBER, changes,       \
      [](const auto& value, size_t index) {                                     \
        return KeyOrIndex(value.KEY, index, #KIND ":");                        \
      },                                                                        \
      [&](RenderId id, const auto& value) { return sink->METHOD(id, value); },   \
      &next_ids[RenderResourceKind::KIND], &out.upsert_count,                   \
      &candidate_next_id, &candidate_exhausted)

    EMIT_VECTOR(Image, images, resolved_path, UpsertImage);
    EMIT_VECTOR(Texture, textures, prim_path, UpsertTexture);
    EMIT_VECTOR(Material, materials, prim_path, UpsertMaterial);
    EMIT_VECTOR(Mesh, meshes, prim_path, UpsertMesh);
    EMIT_VECTOR(Points, points, prim_path, UpsertPoints);
    EMIT_VECTOR(Curves, curves, prim_path, UpsertCurves);
    EMIT_VECTOR(PointInstancer, point_instancers, prim_path,
                UpsertPointInstancer);
    EMIT_VECTOR(Skeleton, skeletons, prim_path, UpsertSkeleton);
    EMIT_VECTOR(Animation, animations, prim_path, UpsertAnimation);
    EMIT_VECTOR(Light, lights, prim_path, UpsertLight);
    EMIT_VECTOR(Camera, cameras, prim_path, UpsertCamera);
    EMIT_VECTOR(Node, nodes, prim_path, UpsertNode);
#undef EMIT_VECTOR

    if (ok) {
      for (const auto& kind_map : ids) {
        const IdMap& retained = next_ids[kind_map.first];
        for (const auto& old : kind_map.second) {
          if (retained.count(old.first) != 0) continue;
          RemovedRenderResource removed;
          removed.kind = kind_map.first;
          removed.id = old.second;
          removed.key = old.first;
          if (!sink->Remove(removed)) {
            ok = false;
            break;
          }
          ++out.remove_count;
        }
        if (!ok) break;
      }
    }
    if (!ok || !sink->EndUpdate()) {
      out.error = candidate_exhausted
          ? "RenderSession: stable resource ID space exhausted"
          : "RenderSession: sink rejected resource update";
      out.status = candidate_exhausted
          ? ::lightusd::next::OperationStatus::ResourceLimit
          : ::lightusd::next::OperationStatus::SinkRejected;
      sink->AbortUpdate();
      return out;
    }

    if (publish) {
      std::lock_guard<std::mutex> lock(publication_mu);
      scene = std::move(committed);
      ids = std::move(next_ids);
      next_id = candidate_next_id;
      revision = snapshot.revision;
      out.revision = revision;
    } else {
      if (prepared_scene) *prepared_scene = std::move(committed);
      if (prepared_ids) *prepared_ids = std::move(next_ids);
      if (prepared_next_id) *prepared_next_id = candidate_next_id;
      out.revision = snapshot.revision;
    }
    out.success = true;
    out.status = ::lightusd::next::OperationStatus::Ok;
    return out;
  }
};

PreparedRenderUpdate::PreparedRenderUpdate() = default;
PreparedRenderUpdate::~PreparedRenderUpdate() = default;
PreparedRenderUpdate::PreparedRenderUpdate(PreparedRenderUpdate&&) noexcept =
    default;
PreparedRenderUpdate& PreparedRenderUpdate::operator=(
    PreparedRenderUpdate&&) noexcept = default;
PreparedRenderUpdate::operator bool() const {
  return impl_ && impl_->scene;
}
uint64_t PreparedRenderUpdate::base_revision() const {
  return impl_ ? impl_->base_revision : 0;
}
uint64_t PreparedRenderUpdate::new_revision() const {
  return impl_ ? impl_->new_revision : 0;
}
const RenderScene* PreparedRenderUpdate::scene() const {
  return impl_ ? impl_->scene.get() : nullptr;
}
std::shared_ptr<const RenderScene> PreparedRenderUpdate::scene_owner() const {
  return impl_ ? impl_->scene : nullptr;
}
const std::vector<std::string>* PreparedRenderUpdate::warnings() const {
  return impl_ ? &impl_->result.warnings : nullptr;
}

RenderSession::RenderSession() : RenderSession(ConverterConfig{}) {}
RenderSession::RenderSession(const ConverterConfig& config)
    : impl_(new Impl(config)) {}
RenderSession::~RenderSession() = default;
RenderSession::RenderSession(RenderSession&&) noexcept = default;
RenderSession& RenderSession::operator=(RenderSession&&) noexcept = default;

RenderUpdateResult RenderSession::Initialize(
    const ::lightusd::next::StageSnapshot& snapshot, SceneUpdateSink* sink) {
  PreparedRenderUpdate prepared;
  RenderUpdateResult result = PrepareInitialize(snapshot, &prepared);
  return result ? Commit(std::move(prepared), sink) : result;
}

RenderUpdateResult RenderSession::Apply(
    const ::lightusd::next::StageSnapshot& snapshot,
    const ::lightusd::next::StageChangeSet& changes, SceneUpdateSink* sink) {
  PreparedRenderUpdate prepared;
  RenderUpdateResult result = Prepare(snapshot, changes, &prepared);
  return result ? Commit(std::move(prepared), sink) : result;
}

RenderUpdateResult RenderSession::PrepareInitialize(
    const ::lightusd::next::StageSnapshot& snapshot,
    PreparedRenderUpdate* prepared) {
  ::lightusd::next::StageChangeSet full;
  full.new_revision = snapshot.revision;
  full.full_resync = true;
  return Prepare(snapshot, full, prepared);
}

RenderUpdateResult RenderSession::Prepare(
    const ::lightusd::next::StageSnapshot& snapshot,
    const ::lightusd::next::StageChangeSet& requested,
    PreparedRenderUpdate* prepared) {
  if (!prepared) {
    RenderUpdateResult result;
    result.status = ::lightusd::next::OperationStatus::InvalidArgument;
    result.error = "RenderSession: null prepared-update destination";
    result.revision = revision();
    return result;
  }
  prepared->impl_.reset();
  if (RenderOperationActive(impl_.get())) {
    RenderUpdateResult result;
    result.status = ::lightusd::next::OperationStatus::Busy;
    result.error = "render session update is already in progress";
    result.revision = GetSnapshot().revision;
    return result;
  }
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  RenderOperationScope operation_scope(impl_.get());
  ::lightusd::next::StageChangeSet changes = requested;
  if (impl_->revision == 0 || changes.base_revision != impl_->revision ||
      changes.new_revision != snapshot.revision) {
    changes.base_revision = impl_->revision;
    changes.new_revision = snapshot.revision;
    changes.full_resync = true;
  }
  std::unique_ptr<PreparedRenderUpdate::Impl> candidate(
      new (std::nothrow) PreparedRenderUpdate::Impl());
  if (!candidate) {
    RenderUpdateResult result;
    result.status = ::lightusd::next::OperationStatus::AllocationFailure;
    result.error = "RenderSession: unable to allocate prepared update";
    result.revision = impl_->revision;
    return result;
  }
  AcceptSceneUpdateSink accept;
  RenderUpdateResult result = impl_->Apply(
      snapshot, changes, &accept, false, &candidate->scene, &candidate->ids,
      &candidate->next_id);
  if (!result) return result;
  candidate->owner = impl_->transaction_owner;
  candidate->base_revision = changes.base_revision;
  candidate->new_revision = snapshot.revision;
  candidate->changes = std::move(changes);
  candidate->result = result;
  prepared->impl_ = std::move(candidate);
  return result;
}

RenderUpdateResult RenderSession::Commit(PreparedRenderUpdate&& prepared,
                                         SceneUpdateSink* sink) {
  RenderUpdateResult invalid;
  invalid.status = ::lightusd::next::OperationStatus::InvalidArgument;
  invalid.revision = revision();
  if (!prepared.impl_ || !sink ||
      prepared.impl_->owner.get() != impl_->transaction_owner.get()) {
    invalid.error = !sink ? "RenderSession: null update sink"
                          : "RenderSession: invalid prepared update";
    return invalid;
  }
  if (RenderOperationActive(impl_.get())) {
    invalid.status = ::lightusd::next::OperationStatus::Busy;
    invalid.error = "render session update is already in progress";
    return invalid;
  }
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  RenderOperationScope operation_scope(impl_.get());
  PreparedRenderUpdate::Impl& candidate = *prepared.impl_;
  RenderUpdateResult out = candidate.result;
  if (candidate.base_revision != impl_->revision ||
      candidate.new_revision <= impl_->revision) {
    out.success = false;
    out.status = ::lightusd::next::OperationStatus::StaleRevision;
    out.error = "RenderSession: prepared update base revision is stale";
    out.revision = impl_->revision;
    return out;
  }
  if (!sink->BeginUpdate(candidate.base_revision, candidate.new_revision,
                         candidate.changes.full_resync) ||
      !sink->UpdateCatalog(*candidate.scene)) {
    out.success = false;
    out.status = ::lightusd::next::OperationStatus::SinkRejected;
    out.error = "RenderSession: sink rejected prepared update start/catalog";
    out.revision = impl_->revision;
    sink->AbortUpdate();
    return out;
  }

  size_t upserts = 0;
  bool ok = true;
  auto emit_vector = [&](RenderResourceKind kind, const auto& values,
                         auto key_fn, auto emit) {
    std::unordered_map<std::string, size_t> occurrences;
    const auto ids_it = candidate.ids.find(kind);
    if (ids_it == candidate.ids.end() && !values.empty()) return false;
    for (size_t i = 0; i < values.size(); ++i) {
      std::string key = key_fn(values[i], i);
      const size_t occurrence = occurrences[key]++;
      if (occurrence != 0) key += "#" + std::to_string(occurrence);
      const auto found = ids_it->second.find(key);
      if (found == ids_it->second.end()) return false;
      if (impl_->IsAffected(kind, key, candidate.changes)) {
        if (!emit(found->second, values[i])) return false;
        ++upserts;
      }
    }
    return true;
  };
#define EMIT_PREPARED(KIND, MEMBER, KEY, METHOD)                              \
  ok = ok && emit_vector(RenderResourceKind::KIND, candidate.scene->MEMBER,   \
    [](const auto& value, size_t index) {                                     \
      return KeyOrIndex(value.KEY, index, #KIND ":");                       \
    },                                                                         \
    [&](RenderId id, const auto& value) { return sink->METHOD(id, value); })
  EMIT_PREPARED(Image, images, resolved_path, UpsertImage);
  EMIT_PREPARED(Texture, textures, prim_path, UpsertTexture);
  EMIT_PREPARED(Material, materials, prim_path, UpsertMaterial);
  EMIT_PREPARED(Mesh, meshes, prim_path, UpsertMesh);
  EMIT_PREPARED(Points, points, prim_path, UpsertPoints);
  EMIT_PREPARED(Curves, curves, prim_path, UpsertCurves);
  EMIT_PREPARED(PointInstancer, point_instancers, prim_path,
                UpsertPointInstancer);
  EMIT_PREPARED(Skeleton, skeletons, prim_path, UpsertSkeleton);
  EMIT_PREPARED(Animation, animations, prim_path, UpsertAnimation);
  EMIT_PREPARED(Light, lights, prim_path, UpsertLight);
  EMIT_PREPARED(Camera, cameras, prim_path, UpsertCamera);
  EMIT_PREPARED(Node, nodes, prim_path, UpsertNode);
#undef EMIT_PREPARED

  size_t removes = 0;
  if (ok) {
    for (const auto& kind_map : impl_->ids) {
      const auto retained_it = candidate.ids.find(kind_map.first);
      const IdMap empty;
      const IdMap& retained = retained_it == candidate.ids.end()
                                  ? empty
                                  : retained_it->second;
      for (const auto& old : kind_map.second) {
        if (retained.count(old.first) != 0) continue;
        if (!sink->Remove({kind_map.first, old.second, old.first})) {
          ok = false;
          break;
        }
        ++removes;
      }
      if (!ok) break;
    }
  }
  if (!ok || !sink->EndUpdate()) {
    out.success = false;
    out.status = ::lightusd::next::OperationStatus::SinkRejected;
    out.error = "RenderSession: sink rejected prepared resource update";
    out.revision = impl_->revision;
    sink->AbortUpdate();
    return out;
  }
  {
    std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
    impl_->scene = std::move(candidate.scene);
    impl_->ids = std::move(candidate.ids);
    impl_->next_id = candidate.next_id;
    impl_->revision = candidate.new_revision;
  }
  out.success = true;
  out.status = ::lightusd::next::OperationStatus::Ok;
  out.revision = candidate.new_revision;
  out.upsert_count = upserts;
  out.remove_count = removes;
  prepared.impl_.reset();
  return out;
}

void RenderSession::Abort(PreparedRenderUpdate* prepared) {
  if (prepared) prepared->impl_.reset();
}

RenderSceneSnapshot RenderSession::GetSnapshot() const {
  RenderSceneSnapshot snapshot;
  std::lock_guard<std::mutex> lock(impl_->publication_mu);
  snapshot.revision = impl_->revision;
  snapshot.scene = impl_->scene;
  return snapshot;
}
uint64_t RenderSession::revision() const { return GetSnapshot().revision; }
RenderId RenderSession::ResourceId(RenderResourceKind kind,
                                   const std::string& key) const {
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  const auto kind_it = impl_->ids.find(kind);
  if (kind_it == impl_->ids.end()) return kInvalidRenderId;
  const auto resource_it = kind_it->second.find(key);
  return resource_it == kind_it->second.end() ? kInvalidRenderId
                                              : resource_it->second;
}
void RenderSession::Reset() {
  if (RenderOperationActive(impl_.get())) return;
  std::lock_guard<std::recursive_mutex> operation_lock(impl_->operation_mu);
  RenderOperationScope operation_scope(impl_.get());
  std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
  impl_->scene.reset();
  impl_->revision = 0;
  impl_->next_id = 1;
  impl_->ids.clear();
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
