// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD C API — tydra-next render-scene implementation.

#include "lightusd-render-c.h"

#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "c-internal.hh"
#include "c-session-ref.hh"
#include "next/load-usd.hh"
#include "next/schema/usd-vol.hh"
#include "next/resolver/asset-resolver.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/render-extract.hh"
#include "tydra/next/render-session.hh"
#include "safe-arithmetic.hh"
#include "tydra/next/render-data.hh"
#include "tydra/next/resource-budget.hh"

namespace td = lightusd::tydra::next;
namespace n = lightusd::next;
using lightusd_internal::EmptySV;
using lightusd_internal::Fail;
using lightusd_internal::SV;

struct BufferCacheKey {
  uint8_t domain;
  int32_t id;
  size_t sub;
  uint8_t which;

  constexpr bool operator==(const BufferCacheKey& other) const {
    return domain == other.domain && id == other.id &&
           sub == other.sub && which == other.which;
  }
};

struct BufferCacheKeyHash {
  size_t operator()(const BufferCacheKey& key) const {
    size_t hash = std::hash<size_t>{}(key.sub);
    const auto mix = [&hash](size_t value) {
      hash ^= value + size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
    };
    mix(static_cast<size_t>(key.domain));
    mix(static_cast<size_t>(static_cast<uint32_t>(key.id)));
    mix(static_cast<size_t>(key.which));
    return hash;
  }
};

static_assert(!(BufferCacheKey{1, 0, 0, 0} ==
                BufferCacheKey{1, 0, 65536, 0}),
              "cache keys must retain the full subresource index");

struct lightusd_render_scene {
  td::RenderScene scene;
  std::shared_ptr<const td::RenderScene> shared_scene;
  const td::RenderScene& data() const {
    return shared_scene ? *shared_scene : scene;
  }
  std::vector<std::string> warnings;
  // Flatten-once cache for multi-chunk buffers: key -> contiguous bytes.
  // Guarded by mu (RenderScene itself is immutable after Convert).
  std::unordered_map<BufferCacheKey, std::vector<uint8_t>, BufferCacheKeyHash> flat_cache;
  // Animation times are authored as doubles while values are normalized to
  // float POD payloads. These derived arrays are materialized once and kept
  // alive with the scene, just like flattened chunked buffers.
  std::unordered_map<BufferCacheKey, std::vector<double>, BufferCacheKeyHash>
      animation_times_cache;
  std::unordered_map<BufferCacheKey, std::vector<float>, BufferCacheKeyHash>
      animation_values_cache;
  mutable std::mutex mu;
};

struct lightusd_render_prim_catalog {
  td::RenderExtractResult extracted;
};

struct lightusd_render_float_array {
  td::ValueArrayRead<float> values;
};

static const td::RenderRecordRefs* CatalogList(
    const lightusd_render_prim_catalog* catalog, uint8_t kind) {
  if (!catalog) return nullptr;
  const td::RenderExtractResult& e = catalog->extracted;
  switch (kind) {
    case LIGHTUSD_RENDER_PRIM_MESH: return &e.meshes;
    case LIGHTUSD_RENDER_PRIM_POINTS: return &e.points;
    case LIGHTUSD_RENDER_PRIM_POINT_INSTANCER: return &e.point_instancers;
    case LIGHTUSD_RENDER_PRIM_NATIVE_INSTANCE: return &e.native_instances;
    case LIGHTUSD_RENDER_PRIM_LIGHT: return &e.lights;
    case LIGHTUSD_RENDER_PRIM_CAMERA: return &e.cameras;
    case LIGHTUSD_RENDER_PRIM_MATERIAL: return &e.materials;
    case LIGHTUSD_RENDER_PRIM_VOLUME: return &e.volumes;
    case LIGHTUSD_RENDER_PRIM_CURVE: return &e.curves;
    case LIGHTUSD_RENDER_PRIM_SKELETON: return &e.skeletons;
    default: return nullptr;
  }
}

namespace lightusd_internal {
const td::RenderScene* RenderSceneForExport(
    const lightusd_render_scene* scene) {
  return scene ? &scene->data() : nullptr;
}
const std::vector<std::string>* RenderWarningsForExport(
    const lightusd_render_scene* scene) {
  return scene ? &scene->warnings : nullptr;
}
}  // namespace lightusd_internal

// The converter borrows the resolver. Sessions own it for their whole lifetime;
// one-shot conversion keeps it on the stack until conversion completes.
static const td::ConverterConfig& ConfigureDefaultResolver(
    td::ConverterConfig& config, lightusd::next::AssetResolver& resolver, bool enabled) {
  if (enabled) {
    lightusd::next::ResolverConfig resolver_config;
    resolver_config.working_directory = config.asset_base_dir;
    resolver_config.search_paths.push_back(config.asset_base_dir);
    resolver.SetConfig(resolver_config);
    config.asset_resolver = &resolver;
  }
  return config;
}

struct lightusd_render_session {
  lightusd_render_session(td::ConverterConfig config,
                          std::string source_dir_in, bool default_resolver)
      : source_dir(std::move(source_dir_in)),
        session(ConfigureDefaultResolver(config, resolver, default_resolver)) {}
  std::string source_dir;
  lightusd::next::AssetResolver resolver;
  td::RenderSession session;
  lightusd_render_event_sink event_sink{};
};

struct lightusd_render_prepared_update {
  td::PreparedRenderUpdate prepared;
};

namespace {
namespace safe = lightusd::safe;

BufferCacheKey CacheKey(uint8_t domain, int32_t id, size_t sub,
                        uint8_t which) {
  return {domain, id, sub, which};
}

bool ToSizeLimit(uint64_t value, size_t* out) {
  if (!out || value == 0) return false;
  if (value == LIGHTUSD_LIMIT_UNLIMITED) {
    *out = (std::numeric_limits<size_t>::max)();
    return true;
  }
#if SIZE_MAX < UINT64_MAX
  if (value > static_cast<uint64_t>(SIZE_MAX)) return false;
#endif
  *out = static_cast<size_t>(value);
  return true;
}

lightusd_status ToCStatus(::lightusd::next::OperationStatus status) {
  using Status = ::lightusd::next::OperationStatus;
  switch (status) {
    case Status::Ok: return LIGHTUSD_OK;
    case Status::InvalidArgument: return LIGHTUSD_ERR_INVALID_ARG;
    case Status::Unsupported: return LIGHTUSD_ERR_UNSUPPORTED;
    case Status::ResourceLimit: return LIGHTUSD_ERR_RESOURCE_LIMIT;
    case Status::IntegerOverflow: return LIGHTUSD_ERR_OVERFLOW;
    case Status::AllocationFailure: return LIGHTUSD_ERR_OUT_OF_MEMORY;
    case Status::Busy: return LIGHTUSD_ERR_BUSY;
    case Status::StaleRevision: return LIGHTUSD_ERR_STALE_REVISION;
    case Status::Cancelled: return LIGHTUSD_ERR_CANCELLED;
    case Status::InvalidData:
    case Status::SinkRejected:
      return LIGHTUSD_ERR_INTERNAL;
  }
  return LIGHTUSD_ERR_INTERNAL;
}

lightusd_status BuildConverterConfig(const lightusd_stage* stage,
                                     const lightusd_render_config* cfg,
                                     td::ConverterConfig* config) {
  if (!config) return Fail(LIGHTUSD_ERR_INVALID_ARG, "config is null");
  if (stage) config->asset_base_dir = stage->source_dir;
  if (!cfg) return LIGHTUSD_OK;
  if (cfg->struct_size < sizeof(lightusd_render_config) ||
      cfg->max_threads < 0 || cfg->triangulation_method > 1 || cfg->tangent_method > 3 ||
      cfg->material_binding_purpose > 2 ||
      cfg->discard_geometry > 1 || cfg->disable_animation > 1 ||
      cfg->discard_instance_source_arrays > 1 || cfg->use_default_asset_resolver > 1 ||
      (cfg->max_render_records > static_cast<uint64_t>(INT32_MAX) &&
       cfg->max_render_records != LIGHTUSD_LIMIT_UNLIMITED) ||
      !ToSizeLimit(cfg->max_resident_bytes,
                   &config->limits.max_resident_bytes) ||
      !ToSizeLimit(cfg->max_render_records,
                   &config->limits.max_render_records) ||
      !ToSizeLimit(cfg->max_render_depth,
                   &config->limits.max_namespace_depth) ||
      !ToSizeLimit(cfg->max_value_clip_samples,
                   &config->limits.max_value_clip_samples)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render config");
  }
  if (cfg->max_render_records == LIGHTUSD_LIMIT_UNLIMITED) {
    config->limits.max_render_records = static_cast<size_t>(INT32_MAX);
  }
  config->mesh.triangulation_method = cfg->triangulation_method
      ? td::MeshConfig::TriangulationMethod::Fan : td::MeshConfig::TriangulationMethod::Earcut;
  const td::MeshConfig::TangentComputationMethod tangent_methods[] = {
      td::MeshConfig::TangentComputationMethod::Hybrid, td::MeshConfig::TangentComputationMethod::Lengyel,
      td::MeshConfig::TangentComputationMethod::MikkTSpace, td::MeshConfig::TangentComputationMethod::FastMikkTSpace};
  config->mesh.tangent_method = tangent_methods[cfg->tangent_method];
  config->mesh.retain_geometry = !cfg->discard_geometry;
  config->animation.enabled = !cfg->disable_animation;
  config->point_instancer.retain_source_arrays = !cfg->discard_instance_source_arrays;
  config->mesh.triangulate = cfg->triangulate != 0;
  config->mesh.compute_normals = cfg->compute_normals != 0;
  config->mesh.compute_tangents = cfg->compute_tangents != 0;
  config->mesh.build_vertex_indices = cfg->build_vertex_indices != 0;
  config->material.load_textures = cfg->load_textures != 0;
  config->material.allow_missing_textures = cfg->allow_missing_textures != 0;
  config->material.target_color_space =
      static_cast<td::ColorSpace>(cfg->target_color_space);
  if (cfg->material_binding_purpose == 1)
    config->material.binding_purpose = "preview";
  else if (cfg->material_binding_purpose == 2)
    config->material.binding_purpose = "full";
  config->point_instancer.duplicate_meshes =
      cfg->duplicate_instance_meshes != 0;
  config->time_code = cfg->time_code;
  config->execution.max_threads = cfg->max_threads;
  return LIGHTUSD_OK;
}

class CEventSceneUpdateSink final : public td::SceneUpdateSink {
 public:
  explicit CEventSceneUpdateSink(const lightusd_render_event_sink& sink)
      : sink_(sink) {}
  bool BeginUpdate(uint64_t base, uint64_t revision, bool full) override {
    lightusd_render_event event = {};
    event.struct_size = sizeof(event);
    event.type = LIGHTUSD_RENDER_EVENT_BEGIN;
    event.record_index = -1;
    event.full_resync = full ? 1 : 0;
    event.base_revision = base;
    event.revision = revision;
    return Emit(event);
  }
  bool UpdateCatalog(const td::RenderScene& scene) override {
    catalog_ = &scene;
    return true;
  }
  bool Remove(const td::RemovedRenderResource& removed) override {
    lightusd_render_event event = {};
    event.struct_size = sizeof(event);
    event.type = LIGHTUSD_RENDER_EVENT_REMOVE;
    event.record_index = -1;
    event.kind = static_cast<uint8_t>(removed.kind) + 1;
    event.resource_id = removed.id;
    event.key = SV(removed.key);
    return Emit(event);
  }
#define LIGHTUSD_C_EVENT_UPSERT(method, kind_value, type_name, member, key_member) \
  bool method(td::RenderId id, const type_name& value) override { \
    const int32_t index = RecordIndex(catalog_ ? &catalog_->member : nullptr, value); \
    if (index < 0) return false; \
    lightusd_render_event event = {}; \
    event.struct_size = sizeof(event); \
    event.type = LIGHTUSD_RENDER_EVENT_UPSERT; \
    event.kind = kind_value; \
    event.record_index = index; \
    event.resource_id = id; \
    event.key = SV(value.key_member); \
    return Emit(event); \
  }
  LIGHTUSD_C_EVENT_UPSERT(UpsertNode, 1, td::SceneNode, nodes, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertMesh, 2, td::RenderMesh, meshes, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertPoints, 3, td::RenderPoints, points, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertCurves, 4, td::RenderCurves, curves, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertPointInstancer, 5, td::RenderPointInstancer,
                          point_instancers,
                          prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertMaterial, 6, td::RenderMaterial, materials, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertTexture, 7, td::RenderTexture, textures, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertImage, 8, td::TextureImage, images, resolved_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertLight, 9, td::RenderLight, lights, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertCamera, 10, td::RenderCamera, cameras, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertAnimation, 11, td::AnimationClip, animations, prim_path)
  LIGHTUSD_C_EVENT_UPSERT(UpsertSkeleton, 12, td::Skeleton, skeletons, prim_path)
#undef LIGHTUSD_C_EVENT_UPSERT
  bool EndUpdate() override {
    lightusd_render_event event = {};
    event.struct_size = sizeof(event);
    event.type = LIGHTUSD_RENDER_EVENT_END;
    event.record_index = -1;
    return Emit(event);
  }
  void AbortUpdate() override {
    lightusd_render_event event = {};
    event.struct_size = sizeof(event);
    event.type = LIGHTUSD_RENDER_EVENT_ABORT;
    event.record_index = -1;
    (void)Emit(event);
  }

 private:
  template <typename T>
  static int32_t RecordIndex(const std::vector<T>* values, const T& value) {
    if (!values || values->empty()) return -1;
    const T* begin = values->data();
    const T* end = begin + values->size();
    const T* address = &value;
    if (address < begin || address >= end) return -1;
    const size_t index = static_cast<size_t>(address - begin);
    return index <= static_cast<size_t>(INT32_MAX)
               ? static_cast<int32_t>(index) : -1;
  }
  bool Emit(const lightusd_render_event& event) const {
    return !sink_.callback || sink_.callback(sink_.userdata, &event) != 0;
  }
  lightusd_render_event_sink sink_;
  const td::RenderScene* catalog_ = nullptr;
};

class AcceptSceneUpdateSink final : public td::SceneUpdateSink {
 public:
  bool BeginUpdate(uint64_t, uint64_t, bool) override { return true; }
  bool EndUpdate() override { return true; }
};

void CopySceneSnapshot(const td::RenderSceneSnapshot& snapshot,
                       const std::vector<std::string>& warnings,
                       lightusd_render_scene* out) {
  out->shared_scene = snapshot.scene;
  out->warnings = warnings;
}

lightusd_status FillUpdateInfo(const td::RenderUpdateResult& result,
                               lightusd_render_update_info* info) {
  if (!info) return LIGHTUSD_OK;
  if (info->struct_size < sizeof(lightusd_render_update_info)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid update info");
  }
  const uint32_t size = info->struct_size;
  std::memset(info, 0, sizeof(*info));
  info->struct_size = size;
  info->full_resync = result.full_resync ? 1 : 0;
  info->revision = result.revision;
  info->converted_resource_count = result.converted_resource_count;
  info->converted_scene_bytes = result.converted_scene_bytes;
  info->upsert_count = result.upsert_count;
  info->remove_count = result.remove_count;
  return LIGHTUSD_OK;
}

lightusd_status PrepareRenderSnapshot(
    lightusd_render_session* session,
    const ::lightusd::next::StageSnapshot& snapshot,
    const ::lightusd::next::StageChangeSet& changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info);

lightusd_status ApplyRenderSnapshot(
    lightusd_render_session* session,
    const ::lightusd::next::StageSnapshot& snapshot,
    const ::lightusd::next::StageChangeSet& changes,
    lightusd_render_scene** out, lightusd_render_update_info* info) {
  if (!session || !snapshot || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render snapshot request");
  *out = nullptr;
  if (info && info->struct_size < sizeof(lightusd_render_update_info)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid update info");
  }
  lightusd_render_prepared_update* prepared = nullptr;
  lightusd_status status = PrepareRenderSnapshot(
      session, snapshot, changes, &prepared, nullptr);
  if (status != LIGHTUSD_OK) return status;
  status = lightusd_render_session_commit(session, prepared, out, info);
  if (status != LIGHTUSD_OK) {
    lightusd_render_session_abort(session, prepared);
  }
  return status;
}

lightusd_status ApplyRenderSession(
    lightusd_render_session* session, const lightusd_stage* stage,
    const ::lightusd::next::StageChangeSet& changes,
    lightusd_render_scene** out, lightusd_render_update_info* info) {
  if (!session || !stage || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/stage/out is null");
  }
  if (stage->source_dir != session->source_dir) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "stage source directory differs from render session");
  }
  ::lightusd::next::StageSnapshot snapshot;
  snapshot.revision = session->session.revision() + 1;
  if (stage->snapshot_stage) {
    snapshot.stage = stage->snapshot_stage;
  } else {
    auto* clone = new (std::nothrow) ::lightusd::next::Stage(stage->ReadStage().Clone());
    if (!clone) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "stage clone alloc failed");
    snapshot.stage.reset(clone);
  }
  ::lightusd::next::StageChangeSet applied = changes;
  applied.new_revision = snapshot.revision;
  return ApplyRenderSnapshot(session, snapshot, applied, out, info);
}

lightusd_status PrepareRenderSnapshot(
    lightusd_render_session* session,
    const ::lightusd::next::StageSnapshot& snapshot,
    const ::lightusd::next::StageChangeSet& changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info) {
  if (!session || !snapshot || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render snapshot request");
  *out = nullptr;
  if (info && info->struct_size < sizeof(lightusd_render_update_info)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid update info");
  }
  std::unique_ptr<lightusd_render_prepared_update> candidate(
      new (std::nothrow) lightusd_render_prepared_update());
  if (!candidate) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "prepared alloc failed");
  td::RenderUpdateResult result =
      session->session.Prepare(snapshot, changes, &candidate->prepared);
  if (!result) {
    return Fail(ToCStatus(result.status),
                result.error.empty() ? "render session prepare failed"
                                     : result.error);
  }
  lightusd_status info_status = FillUpdateInfo(result, info);
  if (info_status != LIGHTUSD_OK) return info_status;
  *out = candidate.release();
  return LIGHTUSD_OK;
}

lightusd_status PrepareRenderSession(
    lightusd_render_session* session, const lightusd_stage* stage,
    const ::lightusd::next::StageChangeSet& changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info) {
  if (!session || !stage || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/stage/out is null");
  }
  if (stage->source_dir != session->source_dir) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "stage source directory differs from render session");
  }
  ::lightusd::next::StageSnapshot snapshot;
  snapshot.revision = session->session.revision() + 1;
  if (stage->snapshot_stage) {
    snapshot.stage = stage->snapshot_stage;
  } else {
    auto* clone = new (std::nothrow) ::lightusd::next::Stage(stage->ReadStage().Clone());
    if (!clone) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "stage clone alloc failed");
    snapshot.stage.reset(clone);
  }
  ::lightusd::next::StageChangeSet applied = changes;
  applied.new_revision = snapshot.revision;
  return PrepareRenderSnapshot(session, snapshot, applied, out, info);
}

lightusd_status DecodeRenderChangeSet(
    const lightusd_render_change_set* input,
    ::lightusd::next::StageChangeSet* output) {
  if (!input || !output ||
      input->struct_size < sizeof(lightusd_render_change_set) ||
      (input->prim_count != 0 && !input->prims)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid change set");
  }
  output->base_revision = input->base_revision;
  output->full_resync = input->full_resync != 0;
  output->stage_metadata_changed = input->stage_metadata_changed != 0;
  constexpr uint32_t kKnownFlags = (1u << 11) - 1u;
  if (input->prim_count > output->prims.max_size() ||
      input->prim_count > (std::numeric_limits<size_t>::max)() / sizeof(*input->prims))
    return Fail(LIGHTUSD_ERR_OVERFLOW, "prim change count overflow");
  output->prims.reserve(input->prim_count);
  for (size_t i = 0; i < input->prim_count; ++i) {
    const lightusd_prim_change& source = input->prims[i];
    if (!source.prim_path || (source.flags & ~kKnownFlags) != 0 ||
        (source.property_count != 0 && !source.properties)) {
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid prim change");
    }
    ::lightusd::next::Path path =
        ::lightusd::next::Path::Parse(std::string(source.prim_path));
    if (path.empty() || path.has_property()) {
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid changed prim path");
    }
    ::lightusd::next::PrimChange change;
    change.path = std::move(path);
    change.flags = static_cast<::lightusd::next::StageChangeFlag>(source.flags);
    if (source.property_count > change.properties.max_size() ||
        source.property_count > (std::numeric_limits<size_t>::max)() / sizeof(*source.properties))
      return Fail(LIGHTUSD_ERR_OVERFLOW, "property change count overflow");
    change.properties.reserve(source.property_count);
    for (size_t p = 0; p < source.property_count; ++p) {
      const lightusd_sv property = source.properties[p];
      if (!property.data && property.len != 0) {
        return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid changed property");
      }
      change.properties.emplace_back(property.data ? property.data : "",
                                     property.len);
    }
    output->prims.push_back(std::move(change));
  }
  return LIGHTUSD_OK;
}

// Fill a buffer view from a ChunkedArray: zero-copy when contiguous, else
// flatten once into the scene cache.
template <typename T, size_t ChunkBytes>
lightusd_status ViewFromChunked(lightusd_render_scene* scene,
                            const td::ChunkedArray<T, ChunkBytes>& arr,
                            BufferCacheKey key, uint8_t comp_type,
                            uint8_t components, lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = comp_type;
  out->components = components;
  const size_t n = arr.size();
  if (components == 0) return Fail(LIGHTUSD_ERR_INTERNAL, "zero components");
  out->count = n / components;
  size_t nbytes;
  if (!safe::mul(n, sizeof(T), &nbytes)) {
    return Fail(LIGHTUSD_ERR_OVERFLOW, "buffer size overflow");
  }
  out->nbytes = nbytes;
  if (n == 0) return LIGHTUSD_OK;

  if (arr.is_contiguous()) {
    out->data = arr.chunk_data(0);
    return LIGHTUSD_OK;
  }
  std::lock_guard<std::mutex> lk(scene->mu);
  auto it = scene->flat_cache.find(key);
  if (it == scene->flat_cache.end()) {
    std::vector<uint8_t> flat(nbytes);
    arr.copy_to(reinterpret_cast<T*>(flat.data()));
    it = scene->flat_cache.emplace(key, std::move(flat)).first;
  }
  out->data = it->second.data();
  return LIGHTUSD_OK;
}

// Plain std::vector-backed view (always contiguous, zero-copy).
template <typename T>
lightusd_status ViewFromVector(const std::vector<T>& v, uint8_t comp_type,
                           uint8_t components, lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = comp_type;
  out->components = components;
  out->count = components ? v.size() / components : 0;
  out->data = v.empty() ? nullptr : v.data();
  if (!safe::mul(v.size(), sizeof(T), &out->nbytes)) {
    out->nbytes = 0;
    return Fail(LIGHTUSD_ERR_OVERFLOW, "buffer size overflow");
  }
  return LIGHTUSD_OK;
}

lightusd_status ViewFromMatrixVector(const std::vector<td::Matrix4>& v,
                                 lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = LIGHTUSD_COMP_FLOAT32;
  out->components = 16;
  out->count = v.size();
  out->data = v.empty() ? nullptr : v.data();
  // Matrix4 is alignas(64) but sizeof is exactly 16 floats.
  if (!safe::mul(v.size(), sizeof(td::Matrix4), &out->nbytes)) {
    out->nbytes = 0;
    return Fail(LIGHTUSD_ERR_OVERFLOW, "matrix buffer size overflow");
  }
  return LIGHTUSD_OK;
}

const td::RenderMesh* MeshAt(const lightusd_render_scene* scene, int32_t id) {
  return scene ? scene->data().get_mesh(id) : nullptr;
}

const td::AnimationClip* AnimationAt(const lightusd_render_scene* scene,
                                     int32_t id) {
  if (!scene || id < 0 || static_cast<size_t>(id) >= scene->data().animations.size()) {
    return nullptr;
  }
  return &scene->data().animations[static_cast<size_t>(id)];
}

const td::AnimationChannel* AnimationChannelAt(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t channel_index) {
  const td::AnimationClip* clip = AnimationAt(scene, animation_id);
  if (!clip || channel_index >= clip->channels.size()) return nullptr;
  return &clip->channels[channel_index];
}

size_t AnimationComponentCount(const td::AnimationChannel& channel) {
  switch (channel.target_path) {
    case td::AnimationChannel::TargetPath::Rotation: return 4;
    case td::AnimationChannel::TargetPath::Weights: return 1;
    case td::AnimationChannel::TargetPath::Translation:
    case td::AnimationChannel::TargetPath::Scale: return 3;
    case td::AnimationChannel::TargetPath::CustomProperty: return 4;
  }
  return 4;
}

void CopyM4(float dst[16], const td::Matrix4& m) {
  std::memcpy(dst, m.m, 16 * sizeof(float));
}

void CopyM4(float dst[16], const td::Matrix4d& m) {
  for (size_t i = 0; i < 16; ++i) dst[i] = static_cast<float>(m.m[i]);
}

const td::ShaderParam* FindParamPreview(const td::PreviewSurfaceShader& s,
                                        const std::string& n) {
  if (n == "diffuse_color") return &s.diffuse_color;
  if (n == "emissive_color") return &s.emissive_color;
  if (n == "specular_color") return &s.specular_color;
  if (n == "metallic") return &s.metallic;
  if (n == "roughness") return &s.roughness;
  if (n == "clearcoat") return &s.clearcoat;
  if (n == "clearcoat_roughness") return &s.clearcoat_roughness;
  if (n == "opacity") return &s.opacity;
  if (n == "opacity_threshold") return &s.opacity_threshold;
  if (n == "ior") return &s.ior;
  if (n == "normal") return &s.normal;
  if (n == "displacement") return &s.displacement;
  if (n == "occlusion") return &s.occlusion;
  return nullptr;
}

const td::ShaderParam* FindParamOpenPBR(const td::OpenPBRSurfaceShader& s,
                                        const std::string& n) {
  if (n == "base_weight") return &s.base_weight;
  if (n == "base_color") return &s.base_color;
  if (n == "base_roughness") return &s.base_roughness;
  if (n == "base_metalness") return &s.base_metalness;
  if (n == "specular_weight") return &s.specular_weight;
  if (n == "specular_color") return &s.specular_color;
  if (n == "specular_roughness") return &s.specular_roughness;
  if (n == "specular_ior") return &s.specular_ior;
  if (n == "specular_anisotropy") return &s.specular_anisotropy;
  if (n == "specular_roughness_anisotropy")
    return &s.specular_roughness_anisotropy;
  if (n == "specular_rotation") return &s.specular_rotation;
  if (n == "transmission_weight") return &s.transmission_weight;
  if (n == "transmission_color") return &s.transmission_color;
  if (n == "transmission_depth") return &s.transmission_depth;
  if (n == "transmission_dispersion") return &s.transmission_dispersion;
  if (n == "transmission_dispersion_scale")
    return &s.transmission_dispersion_scale;
  if (n == "subsurface_weight") return &s.subsurface_weight;
  if (n == "subsurface_color") return &s.subsurface_color;
  if (n == "subsurface_radius") return &s.subsurface_radius;
  if (n == "subsurface_scale") return &s.subsurface_scale;
  if (n == "coat_weight") return &s.coat_weight;
  if (n == "coat_color") return &s.coat_color;
  if (n == "coat_roughness") return &s.coat_roughness;
  if (n == "coat_ior") return &s.coat_ior;
  if (n == "coat_anisotropy") return &s.coat_anisotropy;
  if (n == "coat_roughness_anisotropy")
    return &s.coat_roughness_anisotropy;
  if (n == "coat_normal") return &s.coat_normal;
  if (n == "sheen_weight") return &s.sheen_weight;
  if (n == "sheen_color") return &s.sheen_color;
  if (n == "sheen_roughness") return &s.sheen_roughness;
  if (n == "thin_film_weight") return &s.thin_film_weight;
  if (n == "thin_film_thickness") return &s.thin_film_thickness;
  if (n == "thin_film_ior") return &s.thin_film_ior;
  if (n == "emission_luminance") return &s.emission_luminance;
  if (n == "emission_color") return &s.emission_color;
  if (n == "opacity") return &s.opacity;
  if (n == "thin_walled") return &s.thin_walled;
  if (n == "normal") return &s.normal;
  if (n == "displacement") return &s.displacement;
  if (n == "tangent") return &s.tangent;
  return nullptr;
}

}  // namespace

extern "C" {

lightusd_status lightusd_resource_budget_compute(
    uint64_t host_capacity, uint64_t vram_capacity, uint32_t quality,
    lightusd_resource_budget* out) {
  if (!out || quality > 2) return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid resource budget");
  auto budget = td::ComputeResourceBudget(host_capacity, vram_capacity);
  budget.quality = static_cast<td::LargeSceneQuality>(quality);
  lightusd_resource_budget result{};
  result.host_capacity = budget.host_capacity;
  result.host_limit = budget.host_limit;
  result.stage_limit = budget.stage_limit;
  result.cpu_geometry_limit = budget.cpu_geometry_limit;
  result.io_cache_limit = budget.io_cache_limit;
  result.vram_capacity = budget.vram_capacity;
  result.vram_limit = budget.vram_limit;
  result.gpu_geometry_limit = budget.gpu_geometry_limit;
  result.gpu_texture_limit = budget.gpu_texture_limit;
  result.upload_staging_limit = budget.upload_staging_limit;
  result.proxy_geometry_threshold = budget.proxy_geometry_threshold;
  result.quality = quality;
  result.texture_max_edge = td::DeriveTextureBudget(budget).max_edge;
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_texture_fit_parse(const char* text, lightusd_texture_fit* out) {
  if (!text || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid texture fit output/text");
  td::TextureFit fit;
  if (!td::ParseTextureFit(text, &fit)) return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid texture fit policy");
  *out = {fit.absolute_bytes, static_cast<uint32_t>(fit.policy), 0};
  return LIGHTUSD_OK;
}

lightusd_status lightusd_texture_fit_threshold(
    const lightusd_texture_fit* fit, uint64_t vram_capacity, uint64_t* out) {
  if (!fit || !out || fit->policy > LIGHTUSD_TEXTURE_FIT_ABSOLUTE)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid texture fit policy/output");
  *out = td::TextureFitThresholdBytes(
      {static_cast<td::TextureFitPolicy>(fit->policy), fit->absolute_bytes}, vram_capacity);
  return LIGHTUSD_OK;
}

uint64_t lightusd_budget_percent(uint64_t value, uint64_t percent) {
  return td::Percent(value, percent);
}

const char* lightusd_texture_fit_name(uint32_t policy) {
  if (policy > LIGHTUSD_TEXTURE_FIT_ABSOLUTE) return "invalid";
  return td::TextureFitName({static_cast<td::TextureFitPolicy>(policy), 0});
}

uint32_t lightusd_texture_fit_percent(uint32_t policy) {
  if (policy > LIGHTUSD_TEXTURE_FIT_ABSOLUTE) return 0;
  return td::TextureFitPercent({static_cast<td::TextureFitPolicy>(policy), 0});
}


void lightusd_render_config_init(lightusd_render_config* cfg) {
  if (!cfg) return;
  std::memset(cfg, 0, sizeof(*cfg));
  cfg->struct_size = sizeof(*cfg);
  cfg->triangulate = 1;
  cfg->compute_normals = 1;
  cfg->compute_tangents = 0;
  cfg->build_vertex_indices = 1;
  cfg->load_textures = 1;
  cfg->allow_missing_textures = 1;
  cfg->target_color_space = 1; /* linear */
  cfg->duplicate_instance_meshes = 0;
  cfg->time_code = 0.0;
  cfg->max_threads = 0;
  cfg->max_resident_bytes = 1024ull * 1024ull * 1024ull;
  cfg->max_render_records = 1024ull * 1024ull;
  cfg->max_render_depth = 1024;
  cfg->max_value_clip_samples = 10000;
}

lightusd_status lightusd_render_prim_catalog_create(
    const lightusd_stage* stage, double time_code,
    uint8_t stop_at_point_instancers, uint8_t stop_at_native_instances,
    uint8_t collect_other, lightusd_render_prim_catalog** out) {
  if (out) *out = nullptr;
  if (!stage || !out || stop_at_point_instancers > 1 ||
      stop_at_native_instances > 1 || collect_other > 1) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render catalog arguments");
  }
  std::unique_ptr<lightusd_render_prim_catalog> catalog(
      new lightusd_render_prim_catalog());
  td::RenderExtractOptions options;
  options.time_code = time_code;
  options.stop_at_point_instancers = stop_at_point_instancers != 0;
  options.stop_at_native_instances = stop_at_native_instances != 0;
  options.collect_other = collect_other != 0;
  options.collect_records = false;
  options.collect_categories = true;
  if (!td::CollectRenderPrims(stage->ReadStage(), options,
                              &catalog->extracted)) {
    return Fail(LIGHTUSD_ERR_INTERNAL, "render-prim traversal failed");
  }
  *out = catalog.release();
  return LIGHTUSD_OK;
}

void lightusd_render_prim_catalog_destroy(
    lightusd_render_prim_catalog* catalog) {
  delete catalog;
}

size_t lightusd_render_prim_catalog_count(
    const lightusd_render_prim_catalog* catalog, uint8_t kind) {
  const td::RenderRecordRefs* list = CatalogList(catalog, kind);
  return list ? list->size() : 0;
}

lightusd_status lightusd_render_prim_catalog_get(
    const lightusd_render_prim_catalog* catalog, uint8_t kind, size_t index,
    lightusd_render_prim_info* out) {
  if (!catalog || !out || out->struct_size < sizeof(*out))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render catalog record");
  const td::RenderRecordRefs* list = CatalogList(catalog, kind);
  if (!list) return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render prim kind");
  if (index >= list->size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "render catalog index out of range");
  const td::RenderPrimRecord& rec = (*list)[index];
  lightusd_render_prim_info result{};
  result.struct_size = sizeof(result);
  switch (rec.kind) {
    case td::RenderPrimKind::Mesh: result.kind = LIGHTUSD_RENDER_PRIM_MESH; break;
    case td::RenderPrimKind::PointInstancer: result.kind = LIGHTUSD_RENDER_PRIM_POINT_INSTANCER; break;
    case td::RenderPrimKind::NativeInstance: result.kind = LIGHTUSD_RENDER_PRIM_NATIVE_INSTANCE; break;
    case td::RenderPrimKind::Light: result.kind = LIGHTUSD_RENDER_PRIM_LIGHT; break;
    case td::RenderPrimKind::Camera: result.kind = LIGHTUSD_RENDER_PRIM_CAMERA; break;
    case td::RenderPrimKind::Material: result.kind = LIGHTUSD_RENDER_PRIM_MATERIAL; break;
    case td::RenderPrimKind::Volume: result.kind = LIGHTUSD_RENDER_PRIM_VOLUME; break;
    case td::RenderPrimKind::Curve: result.kind = LIGHTUSD_RENDER_PRIM_CURVE; break;
    case td::RenderPrimKind::Skeleton: result.kind = LIGHTUSD_RENDER_PRIM_SKELETON; break;
    default:
      if (kind != LIGHTUSD_RENDER_PRIM_POINTS)
        return Fail(LIGHTUSD_ERR_INVALID_ARG, "record kind does not match category");
      result.kind = LIGHTUSD_RENDER_PRIM_POINTS;
      break;
  }
  result.animated_world = rec.animated_world ? 1 : 0;
  result.path = SV(rec.path);
  result.type_name = SV(rec.type_name);
  result.purpose = SV(rec.purpose);
  result.material_path = SV(rec.material_path);
  result.native_prototype = SV(rec.native_prototype);
  std::memcpy(result.local, rec.local, sizeof(result.local));
  std::memcpy(result.world, rec.world, sizeof(result.world));
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_float_array_create(
    const lightusd_stage* stage, lightusd_prim prim, const char* property,
    double time_code, lightusd_render_float_array** out) {
  if (out) *out = nullptr;
  if (!stage || !property || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid float-array query");
  std::unique_ptr<lightusd_render_float_array> array(
      new lightusd_render_float_array());
  if (!td::ReadFloatArray(lightusd_internal::FromC(prim), property, time_code,
                          &array->values))
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "float-compatible array not found");
  *out = array.release();
  return LIGHTUSD_OK;
}

size_t lightusd_render_float_array_size(
    const lightusd_render_float_array* array) {
  return array ? array->values.size() : 0;
}

lightusd_status lightusd_render_float_array_data(
    const lightusd_render_float_array* array, const float** out_data,
    size_t* out_count) {
  if (!array || !out_data || !out_count)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid float-array view");
  *out_data = array->values.begin();
  *out_count = array->values.size();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_float_array_copy(
    const lightusd_render_float_array* array, size_t first, size_t count,
    float* destination) {
  if (!array || (count != 0 && !destination) ||
      first > array->values.size() || count > array->values.size() - first)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "float-array range is invalid");
  if (count > std::numeric_limits<size_t>::max() / sizeof(float))
    return Fail(LIGHTUSD_ERR_OVERFLOW, "float-array copy size overflows");
  if (count != 0)
    std::memcpy(destination, array->values.begin() + first,
                count * sizeof(float));
  return LIGHTUSD_OK;
}

void lightusd_render_float_array_destroy(lightusd_render_float_array* array) {
  delete array;
}

lightusd_status lightusd_render_query_particle_field(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    lightusd_render_particle_field_info* out) {
  if (!stage || !out || out->struct_size < sizeof(*out) ||
      prim._owner != stage || !lightusd_internal::SpecFromC(prim))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid ParticleField query");
  n::ParticleFieldData field;
  std::string warning;
  if (!n::GetParticleFieldData(stage->ReadStage(),
                               lightusd_internal::FromC(prim), &field,
                               time_code, &warning))
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH, "prim is not a ParticleField");
  lightusd_render_particle_field_info result{};
  result.struct_size = sizeof(result);
  result.spherical_harmonics_degree =
      static_cast<uint32_t>(std::max(0, field.spherical_harmonics_degree));
  result.particle_count = static_cast<uint64_t>(field.particle_count);
  auto copyName = [](const std::string& src, char* dst, size_t capacity) {
    if (src.size() >= capacity) return false;
    if (!src.empty()) std::memcpy(dst, src.data(), src.size());
    dst[src.size()] = '\0';
    return true;
  };
  if (!copyName(field.positions_property, result.positions_property,
                sizeof(result.positions_property)) ||
      !copyName(field.orientations_property, result.orientations_property,
                sizeof(result.orientations_property)) ||
      !copyName(field.scales_property, result.scales_property,
                sizeof(result.scales_property)) ||
      !copyName(field.opacities_property, result.opacities_property,
                sizeof(result.opacities_property)) ||
      !copyName(field.spherical_harmonics_property,
                result.spherical_harmonics_property,
                sizeof(result.spherical_harmonics_property)))
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT,
                "ParticleField property name is too long");
  *out = result;
  return LIGHTUSD_OK;
}

void lightusd_render_light_query_info_init(
    lightusd_render_light_query_info* info) {
  if (!info) return;
  std::memset(info, 0, sizeof(*info));
  info->struct_size = sizeof(*info);
}

lightusd_status lightusd_render_query_light(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    lightusd_render_light_query_info* out) {
  if (!stage || !out || out->struct_size < sizeof(*out) ||
      prim._owner != stage || !lightusd_internal::SpecFromC(prim)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "invalid stage, prim, or light sample output");
  }
  td::ConverterConfig config;
  config.time_code = time_code;
  td::RenderSceneConverter converter(config);
  td::RenderLight light;
  if (!converter.ConvertLight(stage->ReadStage(),
                              lightusd_internal::FromC(prim), &light))
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH, "prim is not a renderable light");

  lightusd_render_light_query_info result{};
  result.struct_size = sizeof(result);
  switch (light.type) {
    case td::LightType::Directional: result.type = LIGHTUSD_RENDER_LIGHT_DIRECTIONAL; break;
    case td::LightType::Spot: result.type = LIGHTUSD_RENDER_LIGHT_SPOT; break;
    case td::LightType::Rect: result.type = LIGHTUSD_RENDER_LIGHT_RECT; break;
    case td::LightType::Disk: result.type = LIGHTUSD_RENDER_LIGHT_DISK; break;
    case td::LightType::Dome: result.type = LIGHTUSD_RENDER_LIGHT_DOME; break;
    case td::LightType::Sphere: result.type = LIGHTUSD_RENDER_LIGHT_SPHERE; break;
    case td::LightType::Cylinder: result.type = LIGHTUSD_RENDER_LIGHT_CYLINDER; break;
    case td::LightType::Geometry: result.type = LIGHTUSD_RENDER_LIGHT_GEOMETRY; break;
    case td::LightType::Point: default: result.type = LIGHTUSD_RENDER_LIGHT_POINT; break;
  }
  result.color[0] = light.color.x;
  result.color[1] = light.color.y;
  result.color[2] = light.color.z;
  result.intensity = light.intensity;
  result.exposure = light.exposure;
  result.diffuse = light.diffuse;
  result.specular = light.specular;
  result.color_temperature = light.color_temperature;
  result.shaping_cone_angle = light.shaping_cone_angle;
  result.shaping_focus = light.shaping_focus;
  result.shaping_focus_tint[0] = light.shaping_focus_tint.x;
  result.shaping_focus_tint[1] = light.shaping_focus_tint.y;
  result.shaping_focus_tint[2] = light.shaping_focus_tint.z;
  result.shaping_cone_softness = light.shaping_cone_softness;
  result.shaping_ies_angle_scale = light.shaping_ies_angle_scale;
  result.shadow_color[0] = light.shadow_color.x;
  result.shadow_color[1] = light.shadow_color.y;
  result.shadow_color[2] = light.shadow_color.z;
  result.shadow_distance = light.shadow_distance;
  result.shadow_falloff = light.shadow_falloff;
  result.shadow_falloff_gamma = light.shadow_falloff_gamma;
  if (light.normalize) result.flags |= 1u << 0;
  if (light.enable_color_temperature) result.flags |= 1u << 1;
  if (light.shaping_ies_normalize) result.flags |= 1u << 2;
  if (light.enable_shadow) result.flags |= 1u << 3;
  switch (light.type) {
    case td::LightType::Directional: result.shape[0] = light.params.distant.angle; break;
    case td::LightType::Spot: result.shape[0] = light.params.spot.angle; break;
    case td::LightType::Rect:
      result.shape[0] = light.params.rect.width;
      result.shape[1] = light.params.rect.height;
      break;
    case td::LightType::Disk: result.shape[0] = light.params.disk.radius; break;
    case td::LightType::Sphere: result.shape[0] = light.params.sphere.radius; break;
    case td::LightType::Cylinder:
      result.shape[0] = light.params.cylinder.radius;
      result.shape[1] = light.params.cylinder.length;
      break;
    default: break;
  }
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_convert_curves(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    uint32_t tessellation_segments, const char* const* asset_search_paths,
    size_t asset_search_path_count, uint8_t allow_parent_paths,
    lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!stage || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim) || allow_parent_paths > 1 ||
      (asset_search_path_count > 0 && !asset_search_paths))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid stage, prim, or output");

  td::ConverterConfig config;
  config.time_code = time_code;
  config.curves.tessellation_segments =
      std::max<uint32_t>(1u, tessellation_segments);
  config.curves.retain_control_points = false;
  config.asset_base_dir = stage->source_dir;
  auto resolver = std::make_shared<n::AssetResolver>();
  n::ResolverConfig resolver_config;
  resolver_config.working_directory = stage->source_dir;
  resolver_config.allow_parent_paths = allow_parent_paths != 0;
  resolver_config.search_paths.reserve(asset_search_path_count);
  for (size_t i = 0; i < asset_search_path_count; ++i) {
    if (!asset_search_paths[i])
      return Fail(LIGHTUSD_ERR_INVALID_ARG,
                  "null curve asset search path");
    resolver_config.search_paths.emplace_back(asset_search_paths[i]);
  }
  resolver->SetConfig(resolver_config);
  config.animation.clip_stage_loader =
      [resolver](const std::string& asset_path, n::Stage* clip_stage,
                 std::string* warning, std::string* error) {
        if (!clip_stage) return false;
        const n::ResolvedAsset asset = resolver->Resolve(asset_path);
        if (!asset.exists) {
          if (error) *error = "asset not found: " + asset_path;
          return false;
        }
        return n::LoadUSDComposed(asset.resolved_path, clip_stage, warning,
                                  error);
      };

  td::RenderSceneConverter converter(config);
  td::RenderCurves curves;
  if (!converter.ConvertCurves(lightusd_internal::FromC(prim), &curves))
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH,
                "prim is not convertible as curves");
  lightusd_render_scene* result =
      new (std::nothrow) lightusd_render_scene();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  result->scene.curves.push_back(std::move(curves));
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_convert_mesh(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* cfg, int32_t subdivision_level,
    lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!stage || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim) || subdivision_level < 0) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid stage, prim, config, or output");
  }
  td::ConverterConfig config;
  const lightusd_status config_status = BuildConverterConfig(stage, cfg, &config);
  if (config_status != LIGHTUSD_OK) return config_status;
  config.mesh.subdivision_level = subdivision_level;
  td::RenderSceneConverter converter(config);
  td::RenderMesh mesh;
  const auto native_prim = lightusd_internal::FromC(prim);
  if (!converter.ConvertRenderableMesh(stage->ReadStage(), native_prim, &mesh)) {
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH,
                "prim is not convertible as a renderable mesh");
  }
  lightusd_render_scene* result =
      new (std::nothrow) lightusd_render_scene();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  result->scene.meshes.push_back(std::move(mesh));
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_estimate_mesh_bytes(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* cfg, int32_t subdivision_level,
    uint64_t* out_bytes) {
  if (!stage || !out_bytes || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim) || subdivision_level < 0) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "invalid stage, prim, or output");
  }
  td::ConverterConfig config;
  const lightusd_status config_status = BuildConverterConfig(stage, cfg, &config);
  if (config_status != LIGHTUSD_OK) return config_status;
  config.mesh.subdivision_level = subdivision_level;
  td::RenderSceneConverter converter(config);
  const td::GeometryInfo info = converter.GetGeometryInfo(
      lightusd_internal::FromC(prim), td::GeometryKind::Mesh);
  *out_bytes = static_cast<uint64_t>(info.estimated_resident_bytes);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_convert_mesh_proxy(
    const lightusd_stage* stage, lightusd_prim prim, uint8_t proxy_kind,
    const float bounds_min[3], const float bounds_max[3],
    lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!stage || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim) || proxy_kind > 1 ||
      (proxy_kind == 1 && (!bounds_min || !bounds_max))) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "invalid stage, prim, proxy kind, bounds, or output");
  }
  td::RenderSceneConverter converter;
  td::RenderMesh mesh;
  const auto native_prim = lightusd_internal::FromC(prim);
  bool converted = false;
  if (proxy_kind == 0) {
    converted = converter.ConvertExtentProxy(native_prim, &mesh);
  } else {
    converted = converter.ConvertBoundsProxy(
        native_prim, td::Float3(bounds_min[0], bounds_min[1], bounds_min[2]),
        td::Float3(bounds_max[0], bounds_max[1], bounds_max[2]), &mesh);
  }
  if (!converted)
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH,
                "prim cannot produce the requested mesh proxy");
  lightusd_render_scene* result =
      new (std::nothrow) lightusd_render_scene();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  result->scene.meshes.push_back(std::move(mesh));
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_convert_instancer(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* cfg, lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!stage || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "invalid stage, prim, or output");
  }
  td::ConverterConfig config;
  const lightusd_status config_status = BuildConverterConfig(stage, cfg, &config);
  if (config_status != LIGHTUSD_OK) return config_status;
  td::RenderSceneConverter converter(config);
  td::RenderPointInstancer instancer;
  if (!converter.ConvertPointInstancer(lightusd_internal::FromC(prim),
                                       &instancer)) {
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH,
                "prim is not convertible as a PointInstancer");
  }
  lightusd_render_scene* result =
      new (std::nothrow) lightusd_render_scene();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  result->scene.point_instancers.push_back(std::move(instancer));
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_convert_material(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* cfg, lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!stage || !out || prim._owner != stage ||
      !lightusd_internal::SpecFromC(prim)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid stage, prim, or output");
  }
  td::ConverterConfig config;
  const lightusd_status config_status = BuildConverterConfig(stage, cfg, &config);
  if (config_status != LIGHTUSD_OK) return config_status;
  td::RenderSceneConverter converter(config);
  td::RenderMaterial material;
  td::RenderScene converted;
  if (!converter.ConvertMaterial(stage->ReadStage(),
                                 lightusd_internal::FromC(prim), &material,
                                 &converted)) {
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH,
                "prim is not convertible as a material");
  }
  lightusd_render_scene* result =
      new (std::nothrow) lightusd_render_scene();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  result->scene = std::move(converted);
  result->scene.materials.push_back(std::move(material));
  *out = result;
  return LIGHTUSD_OK;
}

void lightusd_render_update_info_init(lightusd_render_update_info* info) {
  if (!info) return;
  std::memset(info, 0, sizeof(*info));
  info->struct_size = sizeof(*info);
}

void lightusd_render_change_set_init(lightusd_render_change_set* changes) {
  if (!changes) return;
  std::memset(changes, 0, sizeof(*changes));
  changes->struct_size = sizeof(*changes);
}

void lightusd_render_event_sink_init(lightusd_render_event_sink* sink) {
  if (!sink) return;
  std::memset(sink, 0, sizeof(*sink));
  sink->struct_size = sizeof(*sink);
}

lightusd_status lightusd_render_convert(const lightusd_stage* stage,
                                const lightusd_render_config* cfg,
                                lightusd_render_scene** out) {
  if (!stage || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage/out is null");
  *out = nullptr;

  td::ConverterConfig config;
  lightusd_status config_status = BuildConverterConfig(stage, cfg, &config);
  if (config_status != LIGHTUSD_OK) return config_status;

  lightusd::next::AssetResolver resolver;
  ConfigureDefaultResolver(config, resolver, cfg && cfg->use_default_asset_resolver);
  td::RenderSceneConverter converter(config);
  td::ConvertResult result = converter.Convert(stage->ReadStage());
  if (!result.success) {
    return Fail(ToCStatus(result.status),
                result.error.empty() ? "render conversion failed"
                                     : result.error);
  }
  lightusd_render_scene* scene = new (std::nothrow) lightusd_render_scene();
  if (!scene) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  scene->scene = std::move(result.scene);
  scene->warnings = std::move(result.warnings);
  *out = scene;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_session_create(
    const lightusd_stage* stage, const lightusd_render_config* cfg,
    lightusd_render_session** out) {
  if (!stage || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage/out is null");
  *out = nullptr;
  td::ConverterConfig config;
  lightusd_status status = BuildConverterConfig(stage, cfg, &config);
  if (status != LIGHTUSD_OK) return status;
  lightusd_render_session* session =
      new (std::nothrow) lightusd_render_session(config, stage->source_dir, cfg && cfg->use_default_asset_resolver);
  if (!session) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  *out = session;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_session_create_document(
    const lightusd_document_session* document,
    const lightusd_render_config* cfg, lightusd_render_session** out) {
  if (out) *out = nullptr;
  if (!document || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "document/out is null");
  }
  if (!lightusd_internal::DocumentSessionIsOpen(document)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "document session is not open");
  }
  td::ConverterConfig config;
  lightusd_status status = BuildConverterConfig(nullptr, cfg, &config);
  if (status != LIGHTUSD_OK) return status;
  const std::string source_dir(
      lightusd_internal::DocumentSessionSourceDir(document));
  config.asset_base_dir = source_dir;
  lightusd_render_session* session = new (std::nothrow)
      lightusd_render_session(config, source_dir, cfg && cfg->use_default_asset_resolver);
  if (!session) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  *out = session;
  return LIGHTUSD_OK;
}

void lightusd_render_session_destroy(lightusd_render_session* session) {
  delete session;
}

uint64_t lightusd_render_session_revision(
    const lightusd_render_session* session) {
  return session ? session->session.revision() : 0;
}

uint64_t lightusd_render_session_resource_id(
    const lightusd_render_session* session, uint8_t kind, const char* key) {
  if (!session || !key || kind == 0 || kind > 12) return 0;
  const td::RenderResourceKind resource_kind =
      static_cast<td::RenderResourceKind>(kind - 1);
  return session->session.ResourceId(resource_kind, key);
}

void lightusd_render_session_reset(lightusd_render_session* session) {
  if (session) session->session.Reset();
}

lightusd_status lightusd_render_session_set_event_sink(
    lightusd_render_session* session, const lightusd_render_event_sink* sink) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  if (!sink) {
    session->event_sink = {};
    return LIGHTUSD_OK;
  }
  if (sink->struct_size < sizeof(lightusd_render_event_sink) ||
      !sink->callback) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render event sink");
  }
  session->event_sink = *sink;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_session_update(
    lightusd_render_session* session, const lightusd_stage* stage,
    lightusd_render_scene** out, lightusd_render_update_info* info) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  ::lightusd::next::StageChangeSet changes;
  changes.base_revision = session->session.revision();
  changes.full_resync = true;
  return ApplyRenderSession(session, stage, changes, out, info);
}

lightusd_status lightusd_render_session_apply(
    lightusd_render_session* session, const lightusd_stage* stage,
    const lightusd_render_change_set* changes, lightusd_render_scene** out,
    lightusd_render_update_info* info) {
  ::lightusd::next::StageChangeSet native;
  const lightusd_status status = DecodeRenderChangeSet(changes, &native);
  if (status != LIGHTUSD_OK) return status;
  return ApplyRenderSession(session, stage, native, out, info);
}

lightusd_status lightusd_render_session_apply_document(
    lightusd_render_session* session,
    const lightusd_document_snapshot* snapshot,
    lightusd_render_scene** out, lightusd_render_update_info* info) {
  if (out) *out = nullptr;
  if (!session || !snapshot || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/snapshot/out is null");
  }
  if (lightusd_internal::DocumentSnapshotSourceDir(snapshot) !=
      session->source_dir) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "document source directory differs from render session");
  }
  return ApplyRenderSnapshot(
      session, lightusd_internal::DocumentSnapshotStage(snapshot),
      lightusd_internal::DocumentSnapshotChanges(snapshot), out, info);
}

lightusd_status lightusd_render_session_prepare(
    lightusd_render_session* session, const lightusd_stage* stage,
    const lightusd_render_change_set* changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info) {
  ::lightusd::next::StageChangeSet native;
  const lightusd_status status = DecodeRenderChangeSet(changes, &native);
  if (status != LIGHTUSD_OK) return status;
  return PrepareRenderSession(session, stage, native, out, info);
}

lightusd_status lightusd_render_session_prepare_document(
    lightusd_render_session* session,
    const lightusd_document_snapshot* snapshot,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info) {
  if (out) *out = nullptr;
  if (!session || !snapshot || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/snapshot/out is null");
  }
  if (lightusd_internal::DocumentSnapshotSourceDir(snapshot) !=
      session->source_dir) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                "document source directory differs from render session");
  }
  return PrepareRenderSnapshot(
      session, lightusd_internal::DocumentSnapshotStage(snapshot),
      lightusd_internal::DocumentSnapshotChanges(snapshot), out, info);
}

lightusd_status lightusd_render_session_prepare_document_changes(
    lightusd_render_session* session, const lightusd_document_snapshot* snapshot,
    const lightusd_render_change_set* changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info) {
  if (out) *out = nullptr;
  if (!session || !snapshot || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/snapshot/out is null");
  if (lightusd_internal::DocumentSnapshotSourceDir(snapshot) != session->source_dir)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "document source directory differs from render session");
  ::lightusd::next::StageChangeSet native;
  const auto status = DecodeRenderChangeSet(changes, &native);
  if (status != LIGHTUSD_OK) return status;
  const auto& stage = lightusd_internal::DocumentSnapshotStage(snapshot);
  native.new_revision = stage.revision;
  return PrepareRenderSnapshot(session, stage, native, out, info);
}

lightusd_status lightusd_render_prepared_scene_copy(
    const lightusd_render_prepared_update* prepared,
    lightusd_render_scene** out) {
  if (out) *out = nullptr;
  if (!prepared || !out || !prepared->prepared.scene()) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "prepared/out is invalid");
  }
  lightusd_render_scene* scene = new (std::nothrow) lightusd_render_scene();
  if (!scene) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "scene snapshot alloc failed");
  scene->shared_scene = prepared->prepared.scene_owner();
  if (const auto* warnings = prepared->prepared.warnings()) {
    scene->warnings = *warnings;
  }
  *out = scene;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_session_commit(
    lightusd_render_session* session, lightusd_render_prepared_update* prepared,
    lightusd_render_scene** out, lightusd_render_update_info* info) {
  if (!session || !prepared || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/prepared/out is null");
  }
  *out = nullptr;
  if (info && info->struct_size < sizeof(lightusd_render_update_info)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid update info");
  }
  std::unique_ptr<lightusd_render_scene> scene(
      new (std::nothrow) lightusd_render_scene());
  if (!scene) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "scene snapshot alloc failed");
  CEventSceneUpdateSink event_sink(session->event_sink);
  AcceptSceneUpdateSink accept_sink;
  td::SceneUpdateSink* sink = session->event_sink.callback
                                  ? static_cast<td::SceneUpdateSink*>(&event_sink)
                                  : static_cast<td::SceneUpdateSink*>(&accept_sink);
  td::RenderUpdateResult result =
      session->session.Commit(std::move(prepared->prepared), sink);
  if (!result) {
    // The candidate remains owned by the wrapper after a rejected commit;
    // callers can inspect it, retry when appropriate, or abort it.
    return Fail(ToCStatus(result.status),
                result.error.empty() ? "render session commit failed"
                                     : result.error);
  }
  CopySceneSnapshot(session->session.GetSnapshot(), result.warnings, scene.get());
  lightusd_status info_status = FillUpdateInfo(result, info);
  if (info_status != LIGHTUSD_OK) {
    delete prepared;
    return info_status;
  }
  *out = scene.release();
  delete prepared;
  return LIGHTUSD_OK;
}

void lightusd_render_session_abort(
    lightusd_render_session* session, lightusd_render_prepared_update* prepared) {
  if (!prepared) return;
  if (session) session->session.Abort(&prepared->prepared);
  delete prepared;
}

void lightusd_render_scene_destroy(lightusd_render_scene* scene) { delete scene; }

lightusd_status lightusd_render_scene_warnings(const lightusd_render_scene* scene,
                                       lightusd_strlist** out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  lightusd_strlist* l = new (std::nothrow) lightusd_strlist();
  if (!l) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  l->items = scene->warnings;
  *out = l;
  return LIGHTUSD_OK;
}

size_t lightusd_render_count(const lightusd_render_scene* scene, uint8_t kind) {
  if (!scene) return 0;
  const td::RenderScene& s = scene->data();
  switch (kind) {
    case LIGHTUSD_RENDER_NODE:
      return s.nodes.size();
    case LIGHTUSD_RENDER_MESH:
      return s.meshes.size();
    case LIGHTUSD_RENDER_MATERIAL:
      return s.materials.size();
    case LIGHTUSD_RENDER_TEXTURE:
      return s.textures.size();
    case LIGHTUSD_RENDER_IMAGE:
      return s.images.size();
    case LIGHTUSD_RENDER_LIGHT:
      return s.lights.size();
    case LIGHTUSD_RENDER_CAMERA:
      return s.cameras.size();
    case LIGHTUSD_RENDER_SKELETON:
      return s.skeletons.size();
    case LIGHTUSD_RENDER_ANIMATION:
      return s.animations.size();
    case LIGHTUSD_RENDER_INSTANCER:
      return s.point_instancers.size();
    case LIGHTUSD_RENDER_ROOT_NODE:
      return s.root_nodes.size();
    case LIGHTUSD_RENDER_UNSUPPORTED:
      return s.unsupported_renderables.size();
    case LIGHTUSD_RENDER_POINTS:
      return s.points.size();
    case LIGHTUSD_RENDER_CURVES:
      return s.curves.size();
    case LIGHTUSD_RENDER_POINT_INSTANCE_DRAW:
      return s.point_instance_draws.size();
    default:
      return 0;
  }
}

int32_t lightusd_render_lookup(const lightusd_render_scene* scene, uint8_t kind,
                           const char* prim_path) {
  if (!scene || !prim_path) return -1;
  const td::RenderScene& s = scene->data();
  const std::unordered_map<std::string, int32_t>* map = nullptr;
  switch (kind) {
    case LIGHTUSD_RENDER_NODE:
      map = &s.node_by_path;
      break;
    case LIGHTUSD_RENDER_MESH:
      map = &s.mesh_by_path;
      break;
    case LIGHTUSD_RENDER_MATERIAL:
      map = &s.material_by_path;
      break;
    case LIGHTUSD_RENDER_INSTANCER:
      map = &s.point_instancer_by_path;
      break;
    default: break;
  }
  if (map) {
    auto it = map->find(prim_path);
    return it == map->end() ? -1 : it->second;
  }
  if (kind == LIGHTUSD_RENDER_ROOT_NODE) {
    for (size_t i = 0; i < s.root_nodes.size(); ++i) {
      const int32_t node_id = s.root_nodes[i];
      if (node_id >= 0 && static_cast<size_t>(node_id) < s.nodes.size() &&
          s.nodes[static_cast<size_t>(node_id)].prim_path == prim_path) {
        return static_cast<int32_t>(i);
      }
    }
    return -1;
  }
  auto find_path = [prim_path](const auto& records) -> int32_t {
    for (size_t i = 0; i < records.size(); ++i) {
      if (records[i].prim_path == prim_path) return static_cast<int32_t>(i);
    }
    return -1;
  };
  switch (kind) {
    case LIGHTUSD_RENDER_POINTS: return find_path(s.points);
    case LIGHTUSD_RENDER_CURVES: return find_path(s.curves);
    case LIGHTUSD_RENDER_POINT_INSTANCE_DRAW:
      for (size_t i = 0; i < s.point_instance_draws.size(); ++i) {
        const int32_t instancer = s.point_instance_draws[i].point_instancer_id;
        if (instancer >= 0 &&
            static_cast<size_t>(instancer) < s.point_instancers.size() &&
            s.point_instancers[static_cast<size_t>(instancer)].prim_path == prim_path) {
          return static_cast<int32_t>(i);
        }
      }
      return -1;
    case LIGHTUSD_RENDER_TEXTURE: return find_path(s.textures);
    case LIGHTUSD_RENDER_IMAGE:
      for (size_t i = 0; i < s.images.size(); ++i) {
        if (s.images[i].resolved_path == prim_path) return static_cast<int32_t>(i);
      }
      return -1;
    case LIGHTUSD_RENDER_LIGHT: return find_path(s.lights);
    case LIGHTUSD_RENDER_CAMERA: return find_path(s.cameras);
    case LIGHTUSD_RENDER_SKELETON: return find_path(s.skeletons);
    case LIGHTUSD_RENDER_ANIMATION: return find_path(s.animations);
    case LIGHTUSD_RENDER_UNSUPPORTED: return find_path(s.unsupported_renderables);
    default: return -1;
  }
}

int32_t lightusd_render_root_node(const lightusd_render_scene* scene, size_t index) {
  if (!scene || index >= scene->data().root_nodes.size()) return -1;
  return scene->data().root_nodes[index];
}

lightusd_status lightusd_render_scene_get_info(const lightusd_render_scene* scene,
                                               lightusd_render_scene_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderScene& s = scene->data();
  out->name = SV(s.name);
  out->default_prim = SV(s.default_prim);
  out->render_settings_path = SV(s.render_settings_path);
  out->working_color_space = SV(s.working_color_space);
  out->meters_per_unit = s.meters_per_unit;
  out->up_axis = static_cast<uint8_t>(s.up_axis);
  out->start_time = s.start_time;
  out->end_time = s.end_time;
  out->frames_per_second = s.frames_per_second;
  for (size_t i = 0; i < 9; ++i) out->working_to_display_linear[i] = s.working_to_display_linear[i];
  return LIGHTUSD_OK;
}

size_t lightusd_render_scene_memory_bytes(const lightusd_render_scene* scene) {
  if (!scene) return 0;
  size_t total = scene->data().memory_usage();
  // The public scene owns warning strings and the lazy-copy caches in
  // addition to RenderScene. Include their container capacities so callers
  // can use this value as a real upper-bound signal when deciding whether to
  // retain another snapshot.
  total = safe::saturating_add(
      total, safe::saturating_mul(scene->warnings.capacity(), sizeof(std::string)));
  for (const auto& warning : scene->warnings) {
    total = safe::saturating_add(total, warning.capacity());
  }
  std::lock_guard<std::mutex> lk(scene->mu);
  total = safe::saturating_add(
      total, safe::saturating_mul(scene->flat_cache.bucket_count(), sizeof(void*)));
  total = safe::saturating_add(total,
      safe::saturating_mul(scene->animation_times_cache.bucket_count(), sizeof(void*)));
  total = safe::saturating_add(total,
      safe::saturating_mul(scene->animation_values_cache.bucket_count(), sizeof(void*)));
  for (const auto& entry : scene->flat_cache) {
    total = safe::saturating_add(total, sizeof(entry));
    total = safe::saturating_add(total, entry.second.capacity());
  }
  for (const auto& entry : scene->animation_times_cache) {
    total = safe::saturating_add(total, sizeof(entry));
    total = safe::saturating_add(
        total, safe::saturating_mul(entry.second.capacity(), sizeof(double)));
  }
  for (const auto& entry : scene->animation_values_cache) {
    total = safe::saturating_add(total, sizeof(entry));
    total = safe::saturating_add(
        total, safe::saturating_mul(entry.second.capacity(), sizeof(float)));
  }
  return total;
}

lightusd_status lightusd_render_scene_get_stats(const lightusd_render_scene* scene,
                                                lightusd_render_stats* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderScene::Stats stats = scene->data().get_stats();
  std::memset(out, 0, sizeof(*out));
  out->node_count = static_cast<uint64_t>(stats.node_count);
  out->mesh_count = static_cast<uint64_t>(stats.mesh_count);
  out->points_count = static_cast<uint64_t>(stats.points_count);
  out->point_cloud_point_count = static_cast<uint64_t>(stats.point_cloud_point_count);
  out->curves_count = static_cast<uint64_t>(stats.curves_count);
  out->curve_count = static_cast<uint64_t>(stats.curve_count);
  out->curve_tessellated_point_count =
      static_cast<uint64_t>(stats.curve_tessellated_point_count);
  out->point_instancer_count = static_cast<uint64_t>(stats.point_instancer_count);
  out->point_instance_count = static_cast<uint64_t>(stats.point_instance_count);
  out->visible_point_instance_count =
      static_cast<uint64_t>(stats.visible_point_instance_count);
  out->point_instance_draw_count =
      static_cast<uint64_t>(stats.point_instance_draw_count);
  out->material_count = static_cast<uint64_t>(stats.material_count);
  out->texture_count = static_cast<uint64_t>(stats.texture_count);
  out->image_count = static_cast<uint64_t>(stats.image_count);
  out->light_count = static_cast<uint64_t>(stats.light_count);
  out->camera_count = static_cast<uint64_t>(stats.camera_count);
  out->animation_count = static_cast<uint64_t>(stats.animation_count);
  out->skeleton_count = static_cast<uint64_t>(stats.skeleton_count);
  out->total_vertices = static_cast<uint64_t>(stats.total_vertices);
  out->total_triangles = static_cast<uint64_t>(stats.total_triangles);
  /* Keep the public stats value aligned with the checked memory query, which
   * also accounts for lazily flattened/derived buffer caches. */
  out->memory_bytes = static_cast<uint64_t>(
      lightusd_render_scene_memory_bytes(scene));
  return LIGHTUSD_OK;
}

namespace {
bool IsPhysicsKind(uint8_t kind) {
  return kind <= LIGHTUSD_RENDER_PHYSICS_ARTICULATION_ROOT;
}
}  // namespace

size_t lightusd_render_physics_count(const lightusd_render_scene* scene,
                                     uint8_t kind) {
  if (!scene || !IsPhysicsKind(kind)) return 0;
  const td::PhysicsAnnotations& p = scene->data().physics;
  switch (kind) {
    case LIGHTUSD_RENDER_PHYSICS_SCENE: return p.scenes.size();
    case LIGHTUSD_RENDER_PHYSICS_RIGID_BODY: return p.rigid_bodies.size();
    case LIGHTUSD_RENDER_PHYSICS_COLLIDER: return p.colliders.size();
    case LIGHTUSD_RENDER_PHYSICS_JOINT: return p.joints.size();
    case LIGHTUSD_RENDER_PHYSICS_MATERIAL: return p.materials.size();
    case LIGHTUSD_RENDER_PHYSICS_FILTERED_PAIR: return p.filtered_pairs.size();
    case LIGHTUSD_RENDER_PHYSICS_ARTICULATION_ROOT:
      return p.articulation_roots.size();
    default: return 0;
  }
}

lightusd_status lightusd_render_physics_get_info(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    lightusd_render_physics_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (!IsPhysicsKind(kind)) return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown physics kind");
  if (index >= lightusd_render_physics_count(scene, kind)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "physics record index out of range");
  }
  const td::PhysicsAnnotations& p = scene->data().physics;
  std::memset(out, 0, sizeof(*out));
  out->kind = kind;
  switch (kind) {
    case LIGHTUSD_RENDER_PHYSICS_SCENE: {
      const auto& v = p.scenes[index];
      out->prim_path = SV(v.prim_path);
      out->vector0[0] = v.gravity_direction.x;
      out->vector0[1] = v.gravity_direction.y;
      out->vector0[2] = v.gravity_direction.z;
      out->scalar0 = v.gravity_magnitude;
      break;
    }
    case LIGHTUSD_RENDER_PHYSICS_RIGID_BODY: {
      const auto& v = p.rigid_bodies[index];
      out->prim_path = SV(v.prim_path);
      out->simulation_owner = SV(v.simulation_owner);
      out->vector0[0] = v.velocity.x; out->vector0[1] = v.velocity.y;
      out->vector0[2] = v.velocity.z;
      out->vector1[0] = v.angular_velocity.x; out->vector1[1] = v.angular_velocity.y;
      out->vector1[2] = v.angular_velocity.z;
      out->vector2[0] = v.center_of_mass.x; out->vector2[1] = v.center_of_mass.y;
      out->vector2[2] = v.center_of_mass.z;
      out->scalar0 = v.mass; out->scalar1 = v.density;
      out->flags = (v.rigid_body_enabled ? 1u : 0u) |
                   (v.kinematic_enabled ? 2u : 0u) |
                   (v.starts_asleep ? 4u : 0u) | (v.has_mass ? 8u : 0u);
      break;
    }
    case LIGHTUSD_RENDER_PHYSICS_COLLIDER: {
      const auto& v = p.colliders[index];
      out->prim_path = SV(v.prim_path);
      out->simulation_owner = SV(v.simulation_owner);
      out->approximation = SV(v.approximation);
      out->flags = (v.collision_enabled ? 1u : 0u) |
                   (v.has_mesh_collision ? 2u : 0u);
      break;
    }
    case LIGHTUSD_RENDER_PHYSICS_JOINT: {
      const auto& v = p.joints[index];
      out->prim_path = SV(v.prim_path); out->type_name = SV(v.type_name);
      out->body0 = SV(v.body0); out->body1 = SV(v.body1);
      out->vector0[0] = v.local_pos0.x; out->vector0[1] = v.local_pos0.y;
      out->vector0[2] = v.local_pos0.z;
      out->vector1[0] = v.local_pos1.x; out->vector1[1] = v.local_pos1.y;
      out->vector1[2] = v.local_pos1.z;
      out->vector2[0] = v.axis.x; out->vector2[1] = v.axis.y;
      out->vector2[2] = v.axis.z;
      out->scalar0 = v.lower_limit; out->scalar1 = v.upper_limit;
      out->scalar2 = v.cone_angle0_limit; out->scalar3 = v.cone_angle1_limit;
      out->scalar4 = v.min_distance; out->scalar5 = v.max_distance;
      out->flags = v.collision_enabled ? 1u : 0u;
      break;
    }
    case LIGHTUSD_RENDER_PHYSICS_MATERIAL: {
      const auto& v = p.materials[index];
      out->prim_path = SV(v.prim_path);
      out->scalar0 = v.static_friction; out->scalar1 = v.dynamic_friction;
      out->scalar2 = v.restitution; out->scalar3 = v.density;
      break;
    }
    case LIGHTUSD_RENDER_PHYSICS_FILTERED_PAIR:
      out->prim_path = SV(p.filtered_pairs[index].prim_path);
      break;
    case LIGHTUSD_RENDER_PHYSICS_ARTICULATION_ROOT:
      out->prim_path = SV(p.articulation_roots[index]);
      break;
    default: return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown physics kind");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_physics_extension_property(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    size_t property_index, lightusd_sv* name, lightusd_sv* value) {
  if (!scene || !name || !value)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/output is null");
  if (!IsPhysicsKind(kind)) return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown physics kind");
  const std::vector<td::PhysicsProperty>* properties = nullptr;
  const td::PhysicsAnnotations& p = scene->data().physics;
  if (index >= lightusd_render_physics_count(scene, kind))
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "physics record index out of range");
  switch (kind) {
    case LIGHTUSD_RENDER_PHYSICS_SCENE: properties = &p.scenes[index].extension_properties; break;
    case LIGHTUSD_RENDER_PHYSICS_RIGID_BODY: properties = &p.rigid_bodies[index].extension_properties; break;
    case LIGHTUSD_RENDER_PHYSICS_COLLIDER: properties = &p.colliders[index].extension_properties; break;
    case LIGHTUSD_RENDER_PHYSICS_JOINT: properties = &p.joints[index].extension_properties; break;
    case LIGHTUSD_RENDER_PHYSICS_MATERIAL: properties = &p.materials[index].extension_properties; break;
    default: return Fail(LIGHTUSD_ERR_NOT_FOUND, "physics record has no properties");
  }
  if (property_index >= properties->size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "physics property index out of range");
  *name = SV((*properties)[property_index].name);
  *value = SV((*properties)[property_index].value);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_physics_string_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    uint8_t which, size_t string_index, char* out, size_t cap,
    size_t* required) {
  if (!scene || which != 0 || !IsPhysicsKind(kind))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid physics string request");
  if (kind == LIGHTUSD_RENDER_PHYSICS_FILTERED_PAIR) {
    if (index >= scene->data().physics.filtered_pairs.size())
      return Fail(LIGHTUSD_ERR_NOT_FOUND, "filtered pair index out of range");
    const auto& paths = scene->data().physics.filtered_pairs[index].filtered_pair_paths;
    if (string_index >= paths.size()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "filtered pair path out of range");
    return lightusd_sv_copy(SV(paths[string_index]), out, cap, required);
  }
  if (kind == LIGHTUSD_RENDER_PHYSICS_ARTICULATION_ROOT) {
    if (string_index != 0 || index >= scene->data().physics.articulation_roots.size())
      return Fail(LIGHTUSD_ERR_NOT_FOUND, "articulation root index out of range");
    return lightusd_sv_copy(SV(scene->data().physics.articulation_roots[index]), out, cap, required);
  }
  return Fail(LIGHTUSD_ERR_NOT_FOUND, "physics record has no indexed strings");
}

lightusd_status lightusd_buffer_copy(const lightusd_buffer_view* view,
                                     void* out, size_t cap, size_t* required) {
  if (!view || !required) return LIGHTUSD_ERR_INVALID_ARG;
  *required = view->nbytes;
  if (view->nbytes != 0 && !view->data) return LIGHTUSD_ERR_INVALID_ARG;
  if (view->nbytes == 0) return LIGHTUSD_OK;
  /* A size query is intentionally successful so callers can allocate once. */
  if (!out && cap == 0) return LIGHTUSD_OK;
  if (cap < view->nbytes || !out) {
    return LIGHTUSD_ERR_INVALID_ARG;
  }
  if (view->nbytes != 0) std::memmove(out, view->data, view->nbytes);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_record_get(const lightusd_render_scene* scene,
                                           uint8_t kind, size_t index,
                                           lightusd_render_record* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderScene& s = scene->data();
  if (index >= lightusd_render_count(scene, kind)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "render record index out of range");
  }
  if (index > static_cast<size_t>(INT32_MAX)) {
    return Fail(LIGHTUSD_ERR_OVERFLOW, "render record ID exceeds int32 range");
  }
  std::memset(out, 0, sizeof(*out));
  out->kind = kind;
  out->id = static_cast<int32_t>(index);
  switch (kind) {
    case LIGHTUSD_RENDER_NODE: out->key = SV(s.nodes[index].prim_path); break;
    case LIGHTUSD_RENDER_ROOT_NODE: {
      const int32_t node_id = s.root_nodes[index];
      if (node_id < 0 || static_cast<size_t>(node_id) >= s.nodes.size()) {
        return Fail(LIGHTUSD_ERR_INTERNAL, "root node record is invalid");
      }
      out->id = node_id;
      out->key = SV(s.nodes[static_cast<size_t>(node_id)].prim_path);
      break;
    }
    case LIGHTUSD_RENDER_MESH: out->key = SV(s.meshes[index].prim_path); break;
    case LIGHTUSD_RENDER_MATERIAL: out->key = SV(s.materials[index].prim_path); break;
    case LIGHTUSD_RENDER_TEXTURE: out->key = SV(s.textures[index].prim_path); break;
    case LIGHTUSD_RENDER_IMAGE: out->key = SV(s.images[index].resolved_path); break;
    case LIGHTUSD_RENDER_LIGHT: out->key = SV(s.lights[index].prim_path); break;
    case LIGHTUSD_RENDER_CAMERA: out->key = SV(s.cameras[index].prim_path); break;
    case LIGHTUSD_RENDER_SKELETON: out->key = SV(s.skeletons[index].prim_path); break;
    case LIGHTUSD_RENDER_ANIMATION: out->key = SV(s.animations[index].prim_path); break;
    case LIGHTUSD_RENDER_INSTANCER: out->key = SV(s.point_instancers[index].prim_path); break;
    case LIGHTUSD_RENDER_UNSUPPORTED: out->key = SV(s.unsupported_renderables[index].prim_path); break;
    case LIGHTUSD_RENDER_POINTS: out->key = SV(s.points[index].prim_path); break;
    case LIGHTUSD_RENDER_CURVES: out->key = SV(s.curves[index].prim_path); break;
    case LIGHTUSD_RENDER_POINT_INSTANCE_DRAW: {
      const int32_t instancer = s.point_instance_draws[index].point_instancer_id;
      out->key = (instancer >= 0 &&
                  static_cast<size_t>(instancer) < s.point_instancers.size())
                     ? SV(s.point_instancers[instancer].prim_path)
                     : EmptySV();
      break;
    }
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown render record kind");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_record_key_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index, char* out,
    size_t cap, size_t* required) {
  if (!required) return Fail(LIGHTUSD_ERR_INVALID_ARG, "required is null");
  lightusd_render_record record = {};
  lightusd_status status = lightusd_render_record_get(scene, kind, index, &record);
  if (status != LIGHTUSD_OK) {
    *required = 0;
    return status;
  }
  return lightusd_sv_copy(record.key, out, cap, required);
}

lightusd_status lightusd_render_record_name_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index, char* out,
    size_t cap, size_t* required) {
  if (!required) return Fail(LIGHTUSD_ERR_INVALID_ARG, "required is null");
  *required = 0;
  lightusd_render_record record = {};
  const lightusd_status status = lightusd_render_record_get(scene, kind, index, &record);
  if (status != LIGHTUSD_OK) return status;
  const td::RenderScene& s = scene->data();
  const std::string* name = nullptr;
  switch (kind) {
    case LIGHTUSD_RENDER_NODE: name = &s.nodes[index].name; break;
    case LIGHTUSD_RENDER_MESH: name = &s.meshes[index].name; break;
    case LIGHTUSD_RENDER_MATERIAL: name = &s.materials[index].name; break;
    case LIGHTUSD_RENDER_TEXTURE: name = &s.textures[index].name; break;
    case LIGHTUSD_RENDER_IMAGE: name = &s.images[index].name; break;
    case LIGHTUSD_RENDER_LIGHT: name = &s.lights[index].name; break;
    case LIGHTUSD_RENDER_CAMERA: name = &s.cameras[index].name; break;
    case LIGHTUSD_RENDER_SKELETON: name = &s.skeletons[index].name; break;
    case LIGHTUSD_RENDER_ANIMATION: name = &s.animations[index].name; break;
    case LIGHTUSD_RENDER_INSTANCER: name = &s.point_instancers[index].name; break;
    case LIGHTUSD_RENDER_ROOT_NODE:
      name = &s.nodes[static_cast<size_t>(record.id)].name;
      break;
    case LIGHTUSD_RENDER_POINTS: name = &s.points[index].name; break;
    case LIGHTUSD_RENDER_CURVES: name = &s.curves[index].name; break;
    default: return Fail(LIGHTUSD_ERR_NOT_FOUND, "render record has no name");
  }
  return lightusd_sv_copy(SV(*name), out, cap, required);
}

lightusd_status lightusd_render_instancer_prototype_path_copy(
    const lightusd_render_scene* scene, int32_t instancer_id,
    size_t prototype_index, char* out, size_t cap, size_t* required) {
  if (!scene || !required) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/required is null");
  if (instancer_id < 0 || static_cast<size_t>(instancer_id) >= scene->data().point_instancers.size()) {
    *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "instancer id out of range");
  }
  const auto& paths = scene->data().point_instancers[static_cast<size_t>(instancer_id)].prototype_paths;
  if (prototype_index >= paths.size()) {
    *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "prototype index out of range");
  }
  const std::string& path = paths[prototype_index];
  return lightusd_sv_copy(SV(path), out, cap, required);
}

lightusd_status lightusd_render_node_get_info(const lightusd_render_scene* scene,
                                      int32_t id,
                                      lightusd_render_node_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::SceneNode* node = scene->data().get_node(id);
  if (!node) return Fail(LIGHTUSD_ERR_NOT_FOUND, "node id out of range");
  out->name = SV(node->name);
  out->prim_path = SV(node->prim_path);
  out->type = static_cast<uint8_t>(node->type);
  out->visible = node->visible ? 1 : 0;
  out->data_id = node->data_id;
  out->parent_id = node->parent_id;
  out->child_count = static_cast<uint32_t>(node->children.size());
  CopyM4(out->local_transform, node->local_transform);
  CopyM4(out->world_transform, node->world_transform);
  return LIGHTUSD_OK;
}

size_t lightusd_render_node_children(const lightusd_render_scene* scene, int32_t id,
                                 int32_t* out, size_t cap) {
  if (!scene) return 0;
  const td::SceneNode* node = scene->data().get_node(id);
  if (!node) return 0;
  if (out) {
    const size_t n = node->children.size() < cap ? node->children.size() : cap;
    for (size_t i = 0; i < n; ++i) out[i] = node->children[i];
  }
  return node->children.size();
}

lightusd_status lightusd_render_mesh_get_info(const lightusd_render_scene* scene,
                                      int32_t id,
                                      lightusd_render_mesh_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  std::memset(out, 0, sizeof(*out));
  out->name = SV(m->name);
  out->prim_path = SV(m->prim_path);
  out->point_count = m->point_count();
  out->face_count = m->face_count();
  out->material_id = m->material_id;
  out->is_triangulated = m->is_triangulated ? 1 : 0;
  out->has_normals = m->has_normals() ? 1 : 0;
  out->has_tangents = m->has_tangents() ? 1 : 0;
  out->has_texcoords0 = m->has_texcoords() ? 1 : 0;
  out->has_texcoords1 = !m->texcoords_1.empty() ? 1 : 0;
  out->has_colors = m->has_colors() ? 1 : 0;
  out->has_skin = m->has_skin() ? 1 : 0;
  out->has_bbox = m->has_bbox ? 1 : 0;
  out->normals_interp = static_cast<uint8_t>(m->normals_interp);
  out->texcoords0_interp = static_cast<uint8_t>(m->texcoords_0_interp);
  out->texcoords1_interp = static_cast<uint8_t>(m->texcoords_1_interp);
  out->colors_interp = static_cast<uint8_t>(m->colors_interp);
  out->subset_count = static_cast<uint32_t>(m->material_subsets.size());
  out->primvar_count = static_cast<uint32_t>(m->primvars.size());
  out->blend_shape_count = static_cast<uint32_t>(m->blend_shapes.size());
  out->skeleton_id = m->skin ? m->skin->skeleton_id : -1;
  out->bbox_min[0] = m->bbox_min.x;
  out->bbox_min[1] = m->bbox_min.y;
  out->bbox_min[2] = m->bbox_min.z;
  out->bbox_max[0] = m->bbox_max.x;
  out->bbox_max[1] = m->bbox_max.y;
  out->bbox_max[2] = m->bbox_max.z;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_get_extra_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_mesh_extra_info* out) {
  if (!scene || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  std::memset(out, 0, sizeof(*out));
  out->tangents_interp = static_cast<uint8_t>(m->tangents_interp);
  out->opacities_interp = static_cast<uint8_t>(m->opacities_interp);
  out->double_sided = m->double_sided ? 1 : 0;
  out->texcoords0_name = SV(m->texcoords_0_name);
  out->texcoords1_name = SV(m->texcoords_1_name);
  size_t expected_colors = 1;
  switch (m->colors_interp) {
    case td::Interpolation::Uniform: expected_colors = m->face_count(); break;
    case td::Interpolation::FaceVarying:
      expected_colors = m->face_vertex_indices.size();
      break;
    case td::Interpolation::Vertex:
    case td::Interpolation::Varying: expected_colors = m->point_count(); break;
    case td::Interpolation::Constant: break;
  }
  if (expected_colors && m->colors.size() == expected_colors * 4)
    out->colors_components = 4;
  else if (!m->colors.empty())
    out->colors_components = 3;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_subset(const lightusd_render_scene* scene,
                                    int32_t mesh_id, size_t index,
                                    uint32_t* face_start, uint32_t* face_count,
                                    int32_t* material_id) {
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (index >= m->material_subsets.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "subset index out of range");
  }
  const auto& sub = m->material_subsets[index];
  if (face_start) *face_start = sub.face_start;
  if (face_count) *face_count = sub.face_count;
  if (material_id) *material_id = sub.material_id;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_primvar_info(const lightusd_render_scene* scene,
                                          int32_t mesh_id, size_t index,
                                          lightusd_render_primvar_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (index >= m->primvars.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "primvar index out of range");
  }
  const td::VertexAttribute& pv = m->primvars[index];
  out->name = SV(pv.name);
  out->format = static_cast<uint8_t>(pv.format);
  out->interpolation = static_cast<uint8_t>(pv.interpolation);
  out->has_indices = pv.has_indices() ? 1 : 0;
  out->_pad = 0;
  out->element_count = pv.element_count();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_buffer(lightusd_render_scene* scene, int32_t mesh_id,
                                    uint8_t kind, lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  const BufferCacheKey key = CacheKey(0, mesh_id, 0, kind);
  switch (kind) {
    case LIGHTUSD_MESH_BUF_POINTS:
      return ViewFromChunked(scene, m->points, key, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_MESH_BUF_FACE_COUNTS:
      return ViewFromChunked(scene, m->face_vertex_counts, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_FACE_INDICES:
      return ViewFromChunked(scene, m->face_vertex_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_TRI_INDICES:
      return ViewFromChunked(scene, m->triangulated_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES:
      return ViewFromChunked(scene, m->triangulated_face_vertex_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_SUBDIVISION_FACE_SOURCE:
      return ViewFromVector(m->subdivision_face_source,
                            LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_FACE_TRIANGLE_OFFSETS:
      return ViewFromVector(m->face_triangle_offsets,
                            LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_SANITIZE_FACE_REMAP:
      return ViewFromVector(m->sanitize_face_remap,
                            LIGHTUSD_COMP_INT32, 1, out);
    case LIGHTUSD_MESH_BUF_NORMALS:
      return ViewFromChunked(scene, m->normals, key, LIGHTUSD_COMP_FLOAT32, 3,
                             out);
    case LIGHTUSD_MESH_BUF_TANGENTS:
      return ViewFromChunked(scene, m->tangents, key, LIGHTUSD_COMP_FLOAT32, 4,
                             out);
    case LIGHTUSD_MESH_BUF_TEXCOORDS0:
      return ViewFromChunked(scene, m->texcoords_0, key, LIGHTUSD_COMP_FLOAT32, 2,
                             out);
    case LIGHTUSD_MESH_BUF_TEXCOORDS1:
      return ViewFromChunked(scene, m->texcoords_1, key, LIGHTUSD_COMP_FLOAT32, 2,
                             out);
    case LIGHTUSD_MESH_BUF_COLORS:
    {
      lightusd_render_mesh_extra_info extra{};
      const lightusd_status status =
          lightusd_render_mesh_get_extra_info(scene, mesh_id, &extra);
      if (status != LIGHTUSD_OK) return status;
      return ViewFromChunked(scene, m->colors, key, LIGHTUSD_COMP_FLOAT32,
                             extra.colors_components == 4 ? 4 : 3, out);
    }
    case LIGHTUSD_MESH_BUF_OPACITIES:
      return ViewFromChunked(scene, m->opacities, key, LIGHTUSD_COMP_FLOAT32, 1,
                             out);
    case LIGHTUSD_MESH_BUF_JOINT_INDICES:
      if (!m->skin) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh has no skin");
      return ViewFromChunked(scene, m->skin->joint_indices, key,
                             LIGHTUSD_COMP_UINT16, 4, out);
    case LIGHTUSD_MESH_BUF_JOINT_WEIGHTS:
      if (!m->skin) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh has no skin");
      return ViewFromChunked(scene, m->skin->joint_weights, key,
                             LIGHTUSD_COMP_FLOAT32, 4, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown mesh buffer kind");
  }
}

lightusd_status lightusd_render_points_buffer(lightusd_render_scene* scene,
                                              int32_t points_id, uint8_t kind,
                                              lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (points_id < 0 || static_cast<size_t>(points_id) >= scene->data().points.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "points id out of range");
  const td::RenderPoints& points = scene->data().points[static_cast<size_t>(points_id)];
  switch (kind) {
    case LIGHTUSD_POINTS_BUF_POSITIONS:
      return ViewFromChunked(scene, points.points, CacheKey(12, points_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_POINTS_BUF_WIDTHS:
      return ViewFromChunked(scene, points.widths, CacheKey(12, points_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 1, out);
    case LIGHTUSD_POINTS_BUF_COLORS:
      return ViewFromChunked(scene, points.colors, CacheKey(12, points_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown points buffer kind");
  }
}

lightusd_status lightusd_render_curves_buffer(lightusd_render_scene* scene,
                                               int32_t curves_id, uint8_t kind,
                                               lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (curves_id < 0 || static_cast<size_t>(curves_id) >= scene->data().curves.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "curves id out of range");
  const td::RenderCurves& curves = scene->data().curves[static_cast<size_t>(curves_id)];
  switch (kind) {
    case LIGHTUSD_CURVES_BUF_POINTS:
      return ViewFromChunked(scene, curves.points, CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_CURVES_BUF_TESSELLATED_POINTS:
      return ViewFromChunked(scene, curves.tessellated_points,
                             CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_CURVES_BUF_WIDTHS:
      return ViewFromChunked(scene, curves.widths, CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 1, out);
    case LIGHTUSD_CURVES_BUF_COLORS:
      return ViewFromChunked(scene, curves.colors, CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_CURVES_BUF_OPACITIES:
      return ViewFromChunked(scene, curves.opacities,
                             CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 1, out);
    case LIGHTUSD_CURVES_BUF_VERTEX_COUNTS:
      return ViewFromVector(curves.curve_vertex_counts, LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_CURVES_BUF_TESSELLATED_COUNTS:
      return ViewFromVector(curves.tessellated_vertex_counts, LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_CURVES_BUF_TESSELLATED_WIDTHS:
      return ViewFromChunked(scene, curves.tessellated_widths,
                             CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 1, out);
    case LIGHTUSD_CURVES_BUF_TESSELLATED_COLORS:
      return ViewFromChunked(scene, curves.tessellated_colors,
                             CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_CURVES_BUF_TESSELLATED_OPACITIES:
      return ViewFromChunked(scene, curves.tessellated_opacities,
                             CacheKey(13, curves_id, 0, kind),
                             LIGHTUSD_COMP_FLOAT32, 1, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown curves buffer kind");
  }
}

lightusd_status lightusd_render_curves_get_info(
    const lightusd_render_scene* scene, int32_t curves_id,
    lightusd_render_curves_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (curves_id < 0 || static_cast<size_t>(curves_id) >= scene->data().curves.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "curves id out of range");
  const td::RenderCurves& curves = scene->data().curves[static_cast<size_t>(curves_id)];
  *out = {};
  out->curve_count = curves.curve_count();
  out->control_point_count = curves.control_point_count();
  out->tessellated_point_count = curves.tessellated_point_count();
  out->type = static_cast<uint8_t>(curves.type);
  out->basis = static_cast<uint8_t>(curves.basis);
  out->wrap = static_cast<uint8_t>(curves.wrap);
  out->is_nurbs = curves.is_nurbs ? 1 : 0;
  out->is_hermite = curves.is_hermite ? 1 : 0;
  out->widths_interpolation = static_cast<uint8_t>(curves.widths_interp);
  out->colors_interpolation = static_cast<uint8_t>(curves.colors_interp);
  out->opacities_interpolation = static_cast<uint8_t>(curves.opacities_interp);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_primvar_buffer(lightusd_render_scene* scene,
                                            int32_t mesh_id,
                                            size_t primvar_index,
                                            uint8_t which,
                                            lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (primvar_index >= m->primvars.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "primvar index out of range");
  }
  const td::VertexAttribute& pv = m->primvars[primvar_index];
  const BufferCacheKey key = CacheKey(1, mesh_id, primvar_index, which);
  if (which == 1) {
    return ViewFromChunked(scene, pv.indices, key, LIGHTUSD_COMP_UINT32, 1, out);
  }
  uint8_t comps = 1;
  switch (pv.format) {
    case td::VertexFormat::Vec2:
    case td::VertexFormat::IVec2:
    case td::VertexFormat::UVec2:
      comps = 2;
      break;
    case td::VertexFormat::Vec3:
    case td::VertexFormat::IVec3:
    case td::VertexFormat::UVec3:
      comps = 3;
      break;
    case td::VertexFormat::Vec4:
    case td::VertexFormat::IVec4:
    case td::VertexFormat::UVec4:
      comps = 4;
      break;
    default:
      comps = 1;
      break;
  }
  switch (pv.format) {
    case td::VertexFormat::Int:
    case td::VertexFormat::IVec2:
    case td::VertexFormat::IVec3:
    case td::VertexFormat::IVec4:
      return ViewFromChunked(scene, pv.int_data, key, LIGHTUSD_COMP_INT32, comps,
                             out);
    case td::VertexFormat::UInt:
    case td::VertexFormat::UVec2:
    case td::VertexFormat::UVec3:
    case td::VertexFormat::UVec4:
      return ViewFromChunked(scene, pv.uint_data, key, LIGHTUSD_COMP_UINT32,
                             comps, out);
    default:
      return ViewFromChunked(scene, pv.float_data, key, LIGHTUSD_COMP_FLOAT32,
                             comps, out);
  }
}

lightusd_status lightusd_render_mesh_blendshape(lightusd_render_scene* scene,
                                        int32_t mesh_id, size_t bs_index,
                                        uint8_t which, lightusd_sv* name,
                                        float* weight,
                                        lightusd_buffer_view* out) {
  if (!scene) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (bs_index >= m->blend_shapes.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "blend shape index out of range");
  }
  const auto& bs = m->blend_shapes[bs_index];
  if (name) *name = SV(bs.name);
  if (weight) *weight = bs.weight;
  if (out) {
    const BufferCacheKey key = CacheKey(2, mesh_id, bs_index, which);
    if (which == 1) {
      return ViewFromChunked(scene, bs.normal_offsets, key, LIGHTUSD_COMP_FLOAT32,
                             3, out);
    }
    return ViewFromChunked(scene, bs.point_offsets, key, LIGHTUSD_COMP_FLOAT32, 3,
                           out);
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_get_info(const lightusd_render_scene* scene,
                                          int32_t id,
                                          lightusd_render_material_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  out->name = SV(mat->name);
  out->prim_path = SV(mat->prim_path);
  out->shader_type = static_cast<uint8_t>(mat->shader_type);
  out->double_sided = mat->double_sided ? 1 : 0;
  out->alpha_mode = static_cast<uint8_t>(mat->alpha_mode);
  out->default_fallback = mat->default_fallback ? 1 : 0;
  out->alpha_cutoff = mat->alpha_cutoff;
  out->has_displacement = mat->has_displacement ? 1 : 0;
  out->has_volume = mat->has_volume ? 1 : 0;
  out->use_specular_workflow =
      mat->preview_surface && mat->preview_surface->use_specular_workflow ? 1 : 0;
  out->_pad = 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_mtlx_config(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_materialx_config_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  std::memset(out, 0, sizeof(*out));
  out->authored = mat->mtlx_config.authored ? 1 : 0;
  out->version = SV(mat->mtlx_config.version);
  out->name_space = SV(mat->mtlx_config.name_space);
  out->colorspace = SV(mat->mtlx_config.colorspace);
  out->source_uri = SV(mat->mtlx_config.source_uri);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_terminal_path_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which, char* out,
    size_t cap, size_t* required) {
  if (!scene) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene is null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  const std::string* path = nullptr;
  if (which == 0) {
    path = &mat->displacement_shader_path;
  } else if (which == 1) {
    path = &mat->volume_shader_path;
  } else {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown material terminal kind");
  }
  return lightusd_sv_copy(SV(*path), out, cap, required);
}

size_t lightusd_render_material_diagnostic_count(
    const lightusd_render_scene* scene, int32_t id) {
  const td::RenderMaterial* mat = scene ? scene->data().get_material(id) : nullptr;
  return mat ? mat->diagnostics.size() : 0;
}

lightusd_status lightusd_render_material_diagnostic_get(
    const lightusd_render_scene* scene, int32_t id, size_t index,
    lightusd_render_material_diagnostic* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  if (index >= mat->diagnostics.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "material diagnostic index out of range");
  }
  const td::MaterialDiagnostic& diagnostic = mat->diagnostics[index];
  std::memset(out, 0, sizeof(*out));
  out->kind = static_cast<uint8_t>(diagnostic.kind);
  out->material_path = SV(diagnostic.material_path);
  out->node_path = SV(diagnostic.node_path);
  out->shader_id = SV(diagnostic.shader_id);
  out->message = SV(diagnostic.message);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_nodegraph_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which, char* out,
    size_t cap, size_t* required) {
  if (!scene) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene is null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  const std::string* json = nullptr;
  if (which == 0) {
    if (mat->openpbr) json = &mat->openpbr->nodegraph_json;
  } else if (which == 1) {
    json = &mat->volume_nodegraph_json;
  } else if (which == 2) {
    json = &mat->preview_surface_nodegraph_json;
  } else {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown material nodegraph kind");
  }
  if (!json) {
    if (required) *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "material nodegraph is unavailable");
  }
  return lightusd_sv_copy(SV(*json), out, cap, required);
}

lightusd_status lightusd_render_material_param(const lightusd_render_scene* scene,
                                       int32_t id, const char* param,
                                       int32_t* texture_id, float value[4]) {
  if (!scene || !param) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/param null");
  const td::RenderMaterial* mat = scene->data().get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");

  const td::ShaderParam* p = nullptr;
  const std::string n(param);
  if (mat->shader_type == td::RenderMaterial::ShaderType::PreviewSurface &&
      mat->preview_surface) {
    p = FindParamPreview(*mat->preview_surface, n);
  } else if (mat->shader_type == td::RenderMaterial::ShaderType::OpenPBR &&
             mat->openpbr) {
    p = FindParamOpenPBR(*mat->openpbr, n);
  }
  if (!p) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, std::string("no shader param: ") + n);
  }
  if (texture_id) *texture_id = p->texture_id;
  if (value) {
    value[0] = p->value.x;
    value[1] = p->value.y;
    value[2] = p->value.z;
    value[3] = p->value.w;
  }
  return LIGHTUSD_OK;
}

size_t lightusd_render_material_retained_param_count(
    const lightusd_render_scene* scene, int32_t id) {
  const td::RenderMaterial* material =
      scene ? scene->data().get_material(id) : nullptr;
  return material ? material->retained_params.size() : 0;
}

lightusd_status lightusd_render_material_retained_param_get(
    const lightusd_render_scene* scene, int32_t id, size_t index,
    lightusd_render_material_retained_param* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* material = scene->data().get_material(id);
  if (!material) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material not found");
  if (index >= material->retained_params.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "retained material parameter not found");
  const auto& value = material->retained_params[index];
  std::memset(out, 0, sizeof(*out));
  out->shader = SV(value.shader);
  out->name = SV(value.name);
  out->texture_id = value.value.texture_id;
  out->value[0] = value.value.value.x;
  out->value[1] = value.value.value.y;
  out->value[2] = value.value.value.z;
  out->value[3] = value.value.value.w;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_texture_get_info(const lightusd_render_scene* scene,
                                         int32_t id,
                                         lightusd_render_texture_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().textures.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "texture id out of range");
  }
  const td::RenderTexture& t = scene->data().textures[size_t(id)];
  out->name = SV(t.name);
  out->prim_path = SV(t.prim_path);
  out->asset_path = SV(t.asset_path);
  out->uv_offset[0] = t.offset.x;
  out->uv_offset[1] = t.offset.y;
  out->uv_scale[0] = t.scale.x;
  out->uv_scale[1] = t.scale.y;
  out->uv_rotation = t.rotation;
  out->wrap_s = static_cast<uint8_t>(t.wrap_s);
  out->wrap_t = static_cast<uint8_t>(t.wrap_t);
  out->output_channel = static_cast<uint8_t>(t.output_channel);
  out->_pad = 0;
  out->bias[0] = t.bias.x;
  out->bias[1] = t.bias.y;
  out->bias[2] = t.bias.z;
  out->bias[3] = t.bias.w;
  out->scale[0] = t.scale_value.x;
  out->scale[1] = t.scale_value.y;
  out->scale[2] = t.scale_value.z;
  out->scale[3] = t.scale_value.w;
  out->image_id = t.image_id;
  out->ktx2_hint = SV(t.ktx2_hint);
  out->uv_primvar = SV(t.uv_primvar);
  out->source_color_space = SV(t.source_color_space);
  out->target_color_space = SV(t.target_color_space);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_image_get_info(const lightusd_render_scene* scene,
                                       int32_t id,
                                       lightusd_render_image_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().images.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "image id out of range");
  }
  const td::TextureImage& img = scene->data().images[size_t(id)];
  out->name = SV(img.name);
  out->resolved_path = SV(img.resolved_path);
  out->width = img.width;
  out->height = img.height;
  out->channels = img.channels;
  out->component_type = static_cast<uint8_t>(img.component_type);
  out->color_space = static_cast<uint8_t>(img.color_space);
  out->is_loaded = img.is_loaded() ? 1 : 0;
  out->nbytes = img.data.size();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_image_buffer(lightusd_render_scene* scene, int32_t id,
                                     lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().images.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "image id out of range");
  }
  const td::TextureImage& img = scene->data().images[size_t(id)];
  const BufferCacheKey key = CacheKey(3, id, 0, 0);
  lightusd_status st = ViewFromChunked(scene, img.data, key, LIGHTUSD_COMP_UINT8,
                                   img.channels ? img.channels : 1, out);
  return st;
}

lightusd_status lightusd_render_light_get_info(const lightusd_render_scene* scene,
                                       int32_t id,
                                       lightusd_render_light_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().lights.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "light id out of range");
  }
  const td::RenderLight& l = scene->data().lights[size_t(id)];
  out->name = SV(l.name);
  out->prim_path = SV(l.prim_path);
  out->type = static_cast<uint8_t>(l.type);
  out->normalize = l.normalize ? 1 : 0;
  out->enable_shadow = l.enable_shadow ? 1 : 0;
  out->_pad = 0;
  out->color[0] = l.color.x;
  out->color[1] = l.color.y;
  out->color[2] = l.color.z;
  out->intensity = l.intensity;
  out->exposure = l.exposure;
  CopyM4(out->transform, l.transform);
  out->param0 = 0.0f;
  out->param1 = 0.0f;
  switch (l.type) {
    case td::LightType::Sphere:
      out->param0 = l.params.sphere.radius;
      break;
    case td::LightType::Rect:
      out->param0 = l.params.rect.width;
      out->param1 = l.params.rect.height;
      break;
    case td::LightType::Disk:
      out->param0 = l.params.disk.radius;
      break;
    case td::LightType::Spot:
      out->param0 = l.params.spot.angle;
      break;
    default:
      break;
  }
  out->shaping_ies_file = SV(l.shaping_ies_file);
  return LIGHTUSD_OK;
}

size_t lightusd_render_light_link_count(const lightusd_render_scene* scene,
                                        int32_t id, uint8_t which) {
  if (!scene || id < 0 || static_cast<size_t>(id) >= scene->data().lights.size() ||
      which > 2) return 0;
  const td::RenderLight& l = scene->data().lights[static_cast<size_t>(id)];
  if (which == 0) return l.light_link_targets.size();
  if (which == 1) return l.shadow_link_targets.size();
  return l.filter_targets.size();
}

lightusd_status lightusd_render_light_link_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which,
    size_t index, char* out, size_t cap, size_t* required) {
  if (!scene || which > 2)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid light-link request");
  if (id < 0 || static_cast<size_t>(id) >= scene->data().lights.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "light id out of range");
  const td::RenderLight& l = scene->data().lights[static_cast<size_t>(id)];
  const std::vector<std::string>* links = which == 0 ? &l.light_link_targets
                                      : which == 1 ? &l.shadow_link_targets
                                                   : &l.filter_targets;
  if (index >= links->size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "light-link index out of range");
  return lightusd_sv_copy(SV((*links)[index]), out, cap, required);
}

lightusd_status lightusd_render_camera_get_info(const lightusd_render_scene* scene,
                                        int32_t id,
                                        lightusd_render_camera_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().cameras.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "camera id out of range");
  }
  const td::RenderCamera& c = scene->data().cameras[size_t(id)];
  std::memset(out, 0, sizeof(*out));
  out->name = SV(c.name);
  out->prim_path = SV(c.prim_path);
  out->type = static_cast<uint8_t>(c.type);
  out->focal_length = c.focal_length;
  out->horizontal_aperture = c.horizontal_aperture;
  out->vertical_aperture = c.vertical_aperture;
  out->ortho_width = c.ortho_width;
  out->near_clip = c.near_clip;
  out->far_clip = c.far_clip;
  out->fov_x = c.fov_x();
  out->fov_y = c.fov_y();
  CopyM4(out->transform, c.transform);
  out->focus_distance = c.focus_distance;
  out->fstop = c.fstop;
  out->aspect = c.aspect_ratio();
  out->horizontal_aperture_offset = c.horizontal_aperture_offset;
  out->vertical_aperture_offset = c.vertical_aperture_offset;
  out->exposure = c.exposure;
  out->stereo_role = static_cast<uint8_t>(c.stereo_role);
  out->shutter_open = c.shutter_open;
  out->shutter_close = c.shutter_close;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_get_info(const lightusd_render_scene* scene,
                                          int32_t id,
                                          lightusd_render_skeleton_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->data().skeletons.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  }
  const td::Skeleton& s = scene->data().skeletons[size_t(id)];
  if (s.joints.size() > (std::numeric_limits<uint32_t>::max)())
    return Fail(LIGHTUSD_ERR_OVERFLOW, "skeleton joint count overflow");
  out->name = SV(s.name);
  out->prim_path = SV(s.prim_path);
  out->joint_count = static_cast<uint32_t>(s.joints.size());
  out->root_joint = s.root_joint;
  out->animation_id = s.animation_id;
  out->animation_source_path = SV(s.animation_source_path);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_joint(const lightusd_render_scene* scene,
                                       int32_t skeleton_id, size_t joint_index,
                                       lightusd_render_joint_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (skeleton_id < 0 ||
      size_t(skeleton_id) >= scene->data().skeletons.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  }
  const td::Skeleton& s = scene->data().skeletons[size_t(skeleton_id)];
  if (joint_index >= s.joints.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "joint index out of range");
  }
  const td::SkeletonJoint& j = s.joints[joint_index];
  out->name = SV(j.name);
  out->path = SV(j.path);
  out->parent_id = j.parent_id;
  CopyM4(out->bind_transform, j.bind_transform);
  CopyM4(out->rest_transform, j.rest_transform);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_joint_children_copy(
    const lightusd_render_scene* scene, int32_t skeleton_id,
    size_t joint_index, int32_t* out, size_t cap, size_t* required) {
  if (!scene || !required)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/required is null");
  *required = 0;
  if (skeleton_id < 0 ||
      static_cast<size_t>(skeleton_id) >= scene->data().skeletons.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  const td::Skeleton& skeleton = scene->data().skeletons[static_cast<size_t>(skeleton_id)];
  if (joint_index >= skeleton.joints.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "joint index out of range");
  const auto& children = skeleton.joints[joint_index].children;
  *required = children.size();
  if (children.empty() || (!out && cap == 0)) return LIGHTUSD_OK;
  if (!out || cap < children.size())
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "joint child output is too small");
  std::memcpy(out, children.data(), children.size() * sizeof(int32_t));
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_buffer(
    lightusd_render_scene* scene, int32_t skeleton_id, uint8_t kind,
    lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (kind > LIGHTUSD_SKELETON_BUF_PARENT_IDS)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown skeleton buffer kind");
  if (skeleton_id < 0 || static_cast<size_t>(skeleton_id) >= scene->data().skeletons.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  const td::Skeleton& skeleton = scene->data().skeletons[static_cast<size_t>(skeleton_id)];
  const size_t count = skeleton.joints.size();
  const size_t components = kind == LIGHTUSD_SKELETON_BUF_PARENT_IDS ? 1 : 16;
  const size_t element_size = kind == LIGHTUSD_SKELETON_BUF_PARENT_IDS
                                  ? sizeof(int32_t)
                                  : sizeof(float);
  size_t bytes = 0;
  if (!safe::mul(count, components, &bytes) || !safe::mul(bytes, element_size, &bytes))
    return Fail(LIGHTUSD_ERR_OVERFLOW, "skeleton buffer size overflow");
  const BufferCacheKey key = CacheKey(14, skeleton_id, 0, kind);
  std::lock_guard<std::mutex> lk(scene->mu);
  auto it = scene->flat_cache.find(key);
  if (it == scene->flat_cache.end()) {
    std::vector<uint8_t> flat(bytes);
    if (kind == LIGHTUSD_SKELETON_BUF_PARENT_IDS) {
      auto* dst = reinterpret_cast<int32_t*>(flat.data());
      for (size_t i = 0; i < count; ++i) dst[i] = skeleton.joints[i].parent_id;
    } else {
      auto* dst = reinterpret_cast<float*>(flat.data());
      for (size_t i = 0; i < count; ++i) {
        CopyM4(dst + i * 16, kind == LIGHTUSD_SKELETON_BUF_BIND_TRANSFORMS
                               ? skeleton.joints[i].bind_transform
                               : skeleton.joints[i].rest_transform);
      }
    }
    it = scene->flat_cache.emplace(key, std::move(flat)).first;
  }
  std::memset(out, 0, sizeof(*out));
  out->component_type = kind == LIGHTUSD_SKELETON_BUF_PARENT_IDS
                            ? LIGHTUSD_COMP_INT32
                            : LIGHTUSD_COMP_FLOAT32;
  out->components = static_cast<uint8_t>(components);
  out->count = count;
  out->nbytes = it->second.size();
  out->data = it->second.empty() ? nullptr : it->second.data();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_animation_get_info(
    const lightusd_render_scene* scene, int32_t animation_id,
    lightusd_render_animation_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "out is null");
  const td::AnimationClip* clip = AnimationAt(scene, animation_id);
  if (!clip) return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation id out of range");
  std::memset(out, 0, sizeof(*out));
  out->name = SV(clip->name);
  out->prim_path = SV(clip->prim_path);
  out->start_time = clip->start_time;
  out->end_time = clip->end_time;
  out->channel_count = static_cast<uint32_t>(clip->channels.size());
  out->clip_asset_count = static_cast<uint32_t>(clip->clip_asset_paths.size());
  out->value_clip_baked = clip->value_clip_baked ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_animation_channel_get_info(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t channel_index, lightusd_render_animation_channel_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "out is null");
  const td::AnimationChannel* channel =
      AnimationChannelAt(scene, animation_id, channel_index);
  if (!channel) return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation channel out of range");
  std::memset(out, 0, sizeof(*out));
  out->target_path = static_cast<uint8_t>(channel->target_path);
  out->interpolation = static_cast<uint8_t>(channel->interpolation);
  out->is_skeletal = channel->is_skeletal ? 1 : 0;
  out->target_node = channel->target_node;
  out->target_skeleton = channel->target_skeleton;
  out->keyframe_count = static_cast<uint32_t>(channel->keyframes.size());
  out->joint_order_count = static_cast<uint32_t>(channel->joint_order.size());
  out->blend_shape_order_count =
      static_cast<uint32_t>(channel->blend_shape_order.size());
  out->joint_remap_count = static_cast<uint32_t>(channel->joint_remap.size());
  out->element_count = channel->element_count;
  out->value_stride = channel->value_stride;
  out->target_prim_path = SV(channel->target_prim_path);
  out->property_name = SV(channel->property_name);
  out->target_skeleton_path = SV(channel->target_skeleton_path);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_animation_channel_buffer(
    lightusd_render_scene* scene, int32_t animation_id, size_t channel_index,
    uint8_t kind, lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::AnimationChannel* channel =
      AnimationChannelAt(scene, animation_id, channel_index);
  if (!channel) return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation channel out of range");
  switch (kind) {
    case LIGHTUSD_ANIMATION_BUF_TIMES: {
      const BufferCacheKey key = CacheKey(9, animation_id, channel_index, kind);
      std::lock_guard<std::mutex> lk(scene->mu);
      auto it = scene->animation_times_cache.find(key);
      if (it == scene->animation_times_cache.end()) {
        std::vector<double> times;
        times.reserve(channel->keyframes.size());
        for (const auto& keyframe : channel->keyframes) times.push_back(keyframe.time);
        it = scene->animation_times_cache.emplace(key, std::move(times)).first;
      }
      return ViewFromVector(it->second, LIGHTUSD_COMP_FLOAT64, 1, out);
    }
    case LIGHTUSD_ANIMATION_BUF_VALUES: {
      const BufferCacheKey key = CacheKey(10, animation_id, channel_index, kind);
      std::lock_guard<std::mutex> lk(scene->mu);
      auto it = scene->animation_values_cache.find(key);
      if (it == scene->animation_values_cache.end()) {
        const size_t components = AnimationComponentCount(*channel);
        size_t value_count = 0;
        if (!safe::mul(channel->keyframes.size(), components, &value_count)) {
          return Fail(LIGHTUSD_ERR_OVERFLOW,
                      "animation value buffer size overflow");
        }
        std::vector<float> values;
        values.reserve(value_count);
        for (const auto& keyframe : channel->keyframes) {
          values.push_back(keyframe.value.x);
          if (components >= 3) {
            values.push_back(keyframe.value.y);
            values.push_back(keyframe.value.z);
          }
          if (components >= 4) values.push_back(keyframe.value.w);
        }
        it = scene->animation_values_cache.emplace(key, std::move(values)).first;
      }
      return ViewFromVector(it->second, LIGHTUSD_COMP_FLOAT32,
                            static_cast<uint8_t>(AnimationComponentCount(*channel)), out);
    }
    case LIGHTUSD_ANIMATION_BUF_ARRAY_VALUES:
      return ViewFromVector(channel->array_values, LIGHTUSD_COMP_FLOAT32,
                            static_cast<uint8_t>(channel->value_stride), out);
    case LIGHTUSD_ANIMATION_BUF_JOINT_REMAP:
      return ViewFromVector(channel->joint_remap, LIGHTUSD_COMP_INT32, 1, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown animation buffer kind");
  }
}

lightusd_status lightusd_render_animation_channel_string_copy(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t channel_index, uint8_t which, size_t string_index, char* out,
    size_t cap, size_t* required) {
  const td::AnimationChannel* channel =
      AnimationChannelAt(scene, animation_id, channel_index);
  if (!channel) {
    if (required) *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation channel out of range");
  }
  const std::vector<std::string>* strings =
      which == 0 ? &channel->joint_order
                 : which == 1 ? &channel->blend_shape_order : nullptr;
  if (!strings) return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown animation string kind");
  if (string_index >= strings->size()) {
    if (required) *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation string index out of range");
  }
  return lightusd_sv_copy(SV((*strings)[string_index]), out, cap, required);
}

lightusd_status lightusd_render_animation_clip_asset_copy(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t asset_index, char* out, size_t cap, size_t* required) {
  const td::AnimationClip* clip = AnimationAt(scene, animation_id);
  if (!clip) {
    if (required) *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation id out of range");
  }
  if (asset_index >= clip->clip_asset_paths.size()) {
    if (required) *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "animation asset index out of range");
  }
  return lightusd_sv_copy(SV(clip->clip_asset_paths[asset_index]), out, cap,
                          required);
}

lightusd_status lightusd_render_instancer_get_info(const lightusd_render_scene* scene,
                                           int32_t id,
                                           lightusd_render_instancer_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderPointInstancer* pi = scene->data().get_point_instancer(id);
  if (!pi) return Fail(LIGHTUSD_ERR_NOT_FOUND, "instancer id out of range");
  if (pi->prototype_count() > (std::numeric_limits<uint32_t>::max)())
    return Fail(LIGHTUSD_ERR_OVERFLOW, "instancer prototype count overflow");
  std::memset(out, 0, sizeof(*out));
  out->name = SV(pi->name);
  out->prim_path = SV(pi->prim_path);
  out->validation_error = SV(pi->validation_error);
  out->instance_count = pi->instance_count();
  out->visible_instance_count = pi->visible_instance_count();
  out->draw_start = pi->draw_start;
  out->draw_count = pi->draw_count;
  out->prototype_count = static_cast<uint32_t>(pi->prototype_count());
  out->valid = pi->valid ? 1 : 0;
  out->has_transforms = pi->transforms.empty() ? 0 : 1;
  out->has_orientations = pi->has_orientations() ? 1 : 0;
  out->has_scales = pi->has_scales() ? 1 : 0;
  out->has_velocities = pi->has_velocities() ? 1 : 0;
  out->has_angular_velocities = pi->has_angular_velocities() ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_point_instance_draw_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_point_instance_draw_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || static_cast<size_t>(id) >= scene->data().point_instance_draws.size())
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "point-instance draw id out of range");
  const td::RenderPointInstanceDraw& draw =
      scene->data().point_instance_draws[static_cast<size_t>(id)];
  out->point_instancer_id = draw.point_instancer_id;
  out->instance_index = draw.instance_index;
  out->prototype_index = draw.prototype_index;
  out->mesh_id = draw.mesh_id;
  out->material_id = draw.material_id;
  out->expanded_mesh_id = draw.expanded_mesh_id;
  CopyM4(out->transform, draw.transform);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_instancer_buffer(lightusd_render_scene* scene, int32_t id,
                                         uint8_t kind,
                                         lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderPointInstancer* pi = scene->data().get_point_instancer(id);
  if (!pi) return Fail(LIGHTUSD_ERR_NOT_FOUND, "instancer id out of range");
  switch (kind) {
    case LIGHTUSD_INST_BUF_PROTO_INDICES:
      return ViewFromVector(pi->proto_indices, LIGHTUSD_COMP_INT32, 1, out);
    case LIGHTUSD_INST_BUF_POSITIONS:
      return ViewFromVector(pi->positions, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_ORIENTATIONS:
      return ViewFromVector(pi->orientations, LIGHTUSD_COMP_FLOAT32, 4, out);
    case LIGHTUSD_INST_BUF_SCALES:
      return ViewFromVector(pi->scales, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_TRANSFORMS:
      return ViewFromMatrixVector(pi->transforms, out);
    case LIGHTUSD_INST_BUF_VISIBLE:
      return ViewFromVector(pi->instance_visible, LIGHTUSD_COMP_UINT8, 1, out);
    case LIGHTUSD_INST_BUF_VELOCITIES:
      return ViewFromVector(pi->velocities, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_ANGULAR_VELOCITIES:
      return ViewFromVector(pi->angular_velocities, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_IDS:
      return ViewFromVector(pi->ids, LIGHTUSD_COMP_INT64, 1, out);
    case LIGHTUSD_INST_BUF_INVISIBLE_IDS:
      return ViewFromVector(pi->invisible_ids, LIGHTUSD_COMP_INT64, 1, out);
    case LIGHTUSD_INST_BUF_INACTIVE_IDS:
      return ViewFromVector(pi->inactive_ids, LIGHTUSD_COMP_INT64, 1, out);
    case LIGHTUSD_INST_BUF_COMPACT:
      return ViewFromVector(pi->compact_instances, LIGHTUSD_COMP_UINT8,
                            sizeof(td::RenderPointInstancer::CompactInstance),
                            out);
    case LIGHTUSD_INST_BUF_PROTO_NODE_IDS:
      return ViewFromVector(pi->prototype_node_ids, LIGHTUSD_COMP_INT32, 1, out);
    case LIGHTUSD_INST_BUF_PROTO_MESH_OFFSETS:
      return ViewFromVector(pi->prototype_mesh_offsets, LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_INST_BUF_PROTO_MESH_IDS:
      return ViewFromVector(pi->prototype_mesh_ids, LIGHTUSD_COMP_INT32, 1, out);
    case LIGHTUSD_INST_BUF_PROTO_TRANSFORMS:
      return ViewFromMatrixVector(pi->prototype_mesh_transforms, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown instancer buffer kind");
  }
}

lightusd_status lightusd_render_unsupported_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_unsupported_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 ||
      static_cast<size_t>(id) >= scene->data().unsupported_renderables.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "unsupported renderable id out of range");
  }
  const td::UnsupportedRenderable& rec =
      scene->data().unsupported_renderables[static_cast<size_t>(id)];
  out->prim_path = SV(rec.prim_path);
  out->type_name = SV(rec.type_name);
  out->reason = SV(rec.reason);
  return LIGHTUSD_OK;
}

}  // extern "C"
