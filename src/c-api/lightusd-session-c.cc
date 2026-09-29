// SPDX-License-Identifier: Apache-2.0
#include "c-session-internal.hh"
#include "c-session-ref.hh"
#include "c-stage-bridge.hh"

#include <limits>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "c-internal.hh"

namespace {
using lightusd_internal::Fail;
namespace n = lightusd::next;

lightusd_status ToStatus(n::OperationStatus status) {
  switch (status) {
    case n::OperationStatus::Ok: return LIGHTUSD_OK;
    case n::OperationStatus::InvalidArgument: return LIGHTUSD_ERR_INVALID_ARG;
    case n::OperationStatus::Unsupported: return LIGHTUSD_ERR_UNSUPPORTED;
    case n::OperationStatus::ResourceLimit: return LIGHTUSD_ERR_RESOURCE_LIMIT;
    case n::OperationStatus::IntegerOverflow: return LIGHTUSD_ERR_OVERFLOW;
    case n::OperationStatus::AllocationFailure: return LIGHTUSD_ERR_OUT_OF_MEMORY;
    case n::OperationStatus::Cancelled: return LIGHTUSD_ERR_CANCELLED;
    case n::OperationStatus::Busy: return LIGHTUSD_ERR_BUSY;
    case n::OperationStatus::StaleRevision: return LIGHTUSD_ERR_STALE_REVISION;
    case n::OperationStatus::InvalidData:
    case n::OperationStatus::SinkRejected: return LIGHTUSD_ERR_COMPOSITION;
  }
  return LIGHTUSD_ERR_INTERNAL;
}

lightusd_status PublishResult(n::StageOperationResult result,
                              const std::string& source_dir,
                              const std::string& source_filename,
                              lightusd_document_snapshot** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "snapshot out is null");
  *out = nullptr;
  if (!result) {
    return Fail(ToStatus(result.status),
                result.error.empty() ? "document operation failed"
                                     : result.error);
  }
  if (!result.snapshot) {
    return Fail(LIGHTUSD_ERR_INTERNAL, "document operation has no snapshot");
  }
  auto snapshot = std::unique_ptr<lightusd_document_snapshot>(
      new (std::nothrow) lightusd_document_snapshot());
  if (!snapshot) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "snapshot alloc failed");
  snapshot->snapshot = std::move(result.snapshot);
  snapshot->changes = std::move(result.changes);
  snapshot->source_dir = source_dir;
  snapshot->source_filename = source_filename;
  snapshot->warnings = std::move(result.warning);
  *out = snapshot.release();
  return LIGHTUSD_OK;
}

bool ParsePrimPath(const char* text, n::Path* out) {
  if (!text || !out) return false;
  n::Path path = n::Path::Parse(text);
  if (path.empty() || !path.is_absolute() || path.is_root() ||
      path.has_property()) return false;
  *out = std::move(path);
  return true;
}

lightusd_status ParseVariants(const lightusd_document_variant_selection* selections,
    size_t count, n::pcp::CompositionOptions::VariantSelectionMap* overrides) {
  if (!selections && count) return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid variant batch");
  if (count > (std::numeric_limits<size_t>::max)() / sizeof(*selections))
    return Fail(LIGHTUSD_ERR_OVERFLOW, "variant batch size overflow");
  for (size_t i = 0; i < count; ++i) {
    const auto& entry = selections[i];
    n::Path path;
    if (!ParsePrimPath(entry.prim_path, &path) || !entry.set_name ||
        !*entry.set_name || !entry.selection || !*entry.selection)
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid variant batch entry");
    if (!(*overrides)[path.str()].emplace(entry.set_name, entry.selection).second)
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "duplicate variant batch entry");
  }
  return LIGHTUSD_OK;
}

n::StageSessionOptions::PreviewCallback PreviewCallback(
    lightusd_document_session* session, const std::string& filename,
    const std::string& directory, uint32_t phase) {
  return [session, filename, directory, phase](const n::StagePreview& input) {
    if (!session->preview_callback) return true;
    auto* stage = new (std::nothrow) lightusd_stage();
    if (!stage) {
      session->preview_status = LIGHTUSD_ERR_OUT_OF_MEMORY;
      return false;
    }
    stage->snapshot_stage = input.snapshot.stage;
    stage->source_dir = directory;
    stage->source_filename = filename;
    const lightusd_document_preview event = {
        stage, input.snapshot.revision, phase,
        (input.namespace_complete ? 1u : 0u) |
        (input.spatial_subset ? 2u : 0u) | (input.authoritative ? 4u : 0u)};
    const bool keep_loading = session->preview_callback(session->preview_userdata, &event) != 0;
    lightusd_stage_destroy(stage);
    return keep_loading;
  };
}

}  // namespace

namespace lightusd_internal {
const n::Stage* BorrowNativeStage(const lightusd_stage* stage) {
  return stage ? &stage->ReadStage() : nullptr;
}
bool SetNativeRootLayer(lightusd_stage* stage, n::Layer&& layer) {
  if (!stage) return false;
  stage->stage.SetRootLayer(std::move(layer));
  return true;
}
bool DocumentSessionIsOpen(const lightusd_document_session* session) {
  return session && session->session.IsOpen();
}
std::string_view DocumentSessionSourceDir(
    const lightusd_document_session* session) {
  return session ? std::string_view(session->source_dir) : std::string_view();
}
std::string_view DocumentSnapshotSourceDir(
    const lightusd_document_snapshot* snapshot) {
  return snapshot ? std::string_view(snapshot->source_dir) : std::string_view();
}
const lightusd::next::StageSnapshot& DocumentSnapshotStage(
    const lightusd_document_snapshot* snapshot) {
  return snapshot->snapshot;
}
const lightusd::next::StageChangeSet& DocumentSnapshotChanges(
    const lightusd_document_snapshot* snapshot) {
  return snapshot->changes;
}
}  // namespace lightusd_internal

extern "C" {

lightusd_status lightusd_document_snapshot_stage(
    const lightusd_document_snapshot* snapshot, lightusd_stage** out) {
  if (out) *out = nullptr;
  if (!snapshot || !out || !snapshot->snapshot)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "snapshot/out is null");
  auto stage = std::unique_ptr<lightusd_stage>(new (std::nothrow) lightusd_stage());
  if (!stage) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "snapshot stage alloc failed");
  stage->snapshot_stage = snapshot->snapshot.stage;
  stage->source_dir = snapshot->source_dir;
  stage->source_filename = snapshot->source_filename;
  stage->warnings = snapshot->warnings;
  *out = stage.release();
  return LIGHTUSD_OK;
}

void lightusd_document_options_init(lightusd_document_options* out) {
  if (!out) return;
  *out = {};
  out->struct_size = sizeof(*out);
  out->load_payloads = 1;
}

lightusd_status lightusd_document_session_create(
    const lightusd_document_options* options,
    lightusd_document_session** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session out is null");
  *out = nullptr;
  if (options && (options->struct_size < sizeof(*options) ||
                  options->max_threads < 0 || options->load_payloads > 1 ||
                  options->skip_composition > 1 || options->cache_retention > 1 ||
                  (options->max_resident_bytes != 0 &&
                   options->max_resident_bytes >
                       static_cast<uint64_t>((std::numeric_limits<size_t>::max)())))) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid document options");
  }
  auto session = std::unique_ptr<lightusd_document_session>(
      new (std::nothrow) lightusd_document_session());
  if (!session) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "session alloc failed");
  if (options) {
    session->options.composition.load_payloads = options->load_payloads != 0;
    session->options.compose = !options->skip_composition;
    session->options.cache_retention = options->cache_retention
        ? n::CacheRetention::LayersOnly : n::CacheRetention::Full;
    session->options.execution.max_threads = options->max_threads;
    if (options->max_resident_bytes != 0) {
      session->options.load.limits.max_resident_bytes =
          static_cast<size_t>(options->max_resident_bytes);
    }
    if (options->progress_callback) {
      const auto callback = options->progress_callback;
      void* const userdata = options->progress_userdata;
      session->options.progress_callback = [callback, userdata](
          const n::ProgressEvent& event) {
        const lightusd_document_progress progress = {
            static_cast<uint8_t>(event.phase), event.progress,
            {event.message.data(), event.message.size()},
            event.estimated_resident_bytes};
        return callback(userdata, &progress) != 0;
      };
    }
  }
  // Keep adapters installed so callback replacement also affects options
  // retained by the native session for rebuilds and root reloads.
  auto* owner = session.get();
  session->options.composition.payload_policy =
      [owner](const n::Path& path, const std::string& asset) {
        return owner->payload_policy
            ? owner->payload_policy(owner->payload_userdata,
                {path.str().data(), path.str().size()},
                {asset.data(), asset.size()}) != 0
            : owner->options.composition.load_payloads;
      };
  session->options.composition.payload_load_callback =
      [owner](const n::Path& path) {
        if (owner->payload_selected)
          owner->payload_selected(owner->payload_userdata,
              {path.str().data(), path.str().size()});
      };
  *out = session.release();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_memory_stats(
    const lightusd_document_session* session, lightusd_document_memory_stats* out) {
  if (!session || !out || out->struct_size < sizeof(*out))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid document memory stats output");
  const auto stats = session->session.GetMemoryStats();
  *out = {};
  out->struct_size = sizeof(*out);
  out->source_layer_bytes = stats.source_layer_bytes;
  out->transient_cache_bytes = stats.transient_cache_bytes;
  out->composed_stage_bytes = stats.composed_stage_bytes;
  out->estimated_total_bytes = stats.estimated_total_bytes;
  out->peak_estimated_total_bytes = stats.peak_estimated_total_bytes;
  out->layer_count = stats.layer_count;
  out->prim_index_count = stats.prim_index_count;
  out->composed_prim_count = stats.composed_prim_count;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_take_stage(
    lightusd_document_session* session, lightusd_stage** out) {
  if (out) *out = nullptr;
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  if (!session->session.IsOpen()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  // Prepare the C owner and its strings before committing the native move.
  auto stage = std::unique_ptr<lightusd_stage>(new (std::nothrow) lightusd_stage());
  if (!stage) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "stage alloc failed");
  stage->source_dir = session->source_dir;
  stage->source_filename = session->session.GetRootIdentifier();
  stage->warnings = session->session.GetWarning();
  auto moved = session->session.CloseAndTakeStage();
  if (!moved) return Fail(ToStatus(moved.error()), "stage is retained by a snapshot or operation");
  stage->stage = std::move(*moved);
  *out = stage.release();
  return LIGHTUSD_OK;
}

void lightusd_document_session_destroy(lightusd_document_session* session) {
  delete session;
}

lightusd_status lightusd_document_session_set_preview_callback(
    lightusd_document_session* session, lightusd_document_preview_fn callback, void* userdata) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  session->preview_callback = callback;
  session->preview_userdata = userdata;
  return LIGHTUSD_OK;
}

int lightusd_document_session_is_open(const lightusd_document_session* session) {
  return session && session->session.IsOpen();
}

lightusd_status lightusd_document_session_set_payload_callbacks(
    lightusd_document_session* session, lightusd_document_payload_policy_fn policy,
    lightusd_document_payload_selected_fn selected, void* userdata) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  // Cached composition opinions encode earlier policy decisions. Drop them
  // before replacing callbacks, while retaining parsed layers and load rules.
  const auto status = ToStatus(session->session.TrimCaches());
  if (status != LIGHTUSD_OK) return Fail(status, "cannot change payload callbacks during an operation");
  session->payload_policy = policy;
  session->payload_selected = selected;
  session->payload_userdata = userdata;
  return LIGHTUSD_OK;
}

int lightusd_document_session_is_composed(const lightusd_document_session* session) {
  return session && session->session.IsComposed();
}

lightusd_status lightusd_document_session_dependencies(
    const lightusd_document_session* session, lightusd_strlist** out) {
  if (out) *out = nullptr;
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  auto* result = new (std::nothrow) lightusd_strlist();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "dependency list alloc failed");
  result->items = session->session.GetLayerDependencies();
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_deferred_payloads(
    const lightusd_document_session* session, lightusd_strlist** out) {
  if (out) *out = nullptr;
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  auto* result = new (std::nothrow) lightusd_strlist();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "payload list alloc failed");
  const auto paths = session->session.GetDeferredPayloadPaths();
  result->items.reserve(paths.size());
  for (const auto& path : paths) result->items.push_back(path.str());
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_trim_caches(lightusd_document_session* session) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  if (!session->session.IsOpen()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  const auto status = ToStatus(session->session.TrimCaches());
  return status == LIGHTUSD_OK ? status : Fail(status, "cannot trim cache during an operation");
}

lightusd_status lightusd_document_session_release_cache(lightusd_document_session* session) {
  if (!session) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session is null");
  if (!session->session.IsOpen()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  const auto status = ToStatus(session->session.ReleaseCompositionCache());
  return status == LIGHTUSD_OK ? status : Fail(status, "cannot release cache during an operation");
}

size_t lightusd_document_session_dependency_count(const lightusd_document_session* session) {
  return session ? session->session.GetLayerDependencies().size() : 0;
}

lightusd_status lightusd_document_session_release_geometry(
    lightusd_document_session* session, const char* prim_path,
    uint64_t min_array_elements, lightusd_geometry_release_stats* out) {
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  if (min_array_elements > (std::numeric_limits<size_t>::max)())
    return Fail(LIGHTUSD_ERR_OVERFLOW, "geometry threshold exceeds address space");
  n::Path path;
  if (prim_path && !ParsePrimPath(prim_path, &path))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid geometry prim path");
  if (!session->session.IsOpen()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  n::Stage::StaticGeometryReleaseStats stats;
  const auto status = ToStatus(session->session.ReleaseStaticGeometryArraysByPath(
      prim_path ? &path : nullptr, static_cast<size_t>(min_array_elements), &stats));
  if (status != LIGHTUSD_OK) return Fail(status, "cannot release geometry from this session/path");
  *out = {stats.property_count, stats.element_count, stats.estimated_payload_bytes,
          stats.stage_bytes_before, stats.stage_bytes_after};
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_dependency_copy(
    const lightusd_document_session* session, size_t index, char* out,
    size_t cap, size_t* required) {
  if (!session || !required) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/required is null");
  const auto paths = session->session.GetLayerDependencies();
  if (index >= paths.size()) {
    *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "dependency index out of range");
  }
  return lightusd_sv_copy({paths[index].data(), paths[index].size()}, out, cap, required);
}

size_t lightusd_document_session_composition_issue_count(
    const lightusd_document_session* session) {
  return session ? session->session.GetCompositionIssueCount() : 0;
}

lightusd_status lightusd_document_session_composition_issue_copy(
    const lightusd_document_session* session, size_t index,
    lightusd_document_composition_issue* info, char* site, size_t site_cap,
    char* message, size_t message_cap) {
  if (!session || !info || info->struct_size < sizeof(*info))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid composition issue query");
  n::pcp::Cache::CompositionIssue issue;
  if (!session->session.GetCompositionIssue(index, &issue))
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "composition issue index out of range");
  if ((site && site_cap < issue.site.size()) ||
      (message && message_cap < issue.message.size()) ||
      (!site && site_cap != 0) || (!message && message_cap != 0))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "composition issue buffer too small");
  info->code = static_cast<uint32_t>(issue.code);
  info->site_bytes = issue.site.size();
  info->message_bytes = issue.message.size();
  if (site && !issue.site.empty())
    std::memcpy(site, issue.site.data(), issue.site.size());
  if (message && !issue.message.empty())
    std::memcpy(message, issue.message.data(), issue.message.size());
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_open_file(
    lightusd_document_session* session, const char* filename,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !filename || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/filename/out is null");
  }
  const std::string path(filename);
  const size_t slash = path.find_last_of("/\\");
  const std::string directory = slash == std::string::npos ? "" : path.substr(0, slash);
  auto options = session->options;
  if (session->preview_callback) {
    options.early_preview_callback = PreviewCallback(session, path, directory, 0);
    options.preview_callback = PreviewCallback(session, path, directory, 1);
  }
  session->preview_status = LIGHTUSD_OK;
  auto result = session->session.OpenFile(filename, options);
  if (!result && session->preview_status != LIGHTUSD_OK)
    return Fail(session->preview_status, "preview stage allocation failed");
  if (result) {
    session->source_dir = directory;
  }
  return PublishResult(std::move(result), session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_rebuild(
    lightusd_document_session* session, lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  return PublishResult(session->session.Rebuild(), session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_set_variant(
    lightusd_document_session* session, const char* prim_path,
    const char* set_name, const char* selection,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  n::Path path;
  if (!session || !set_name || !selection || !out ||
      !ParsePrimPath(prim_path, &path)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid variant edit");
  }
  return PublishResult(
      session->session.SetVariantSelection(path, set_name, selection),
      session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_load_payload(
    lightusd_document_session* session, const char* prim_path,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  n::Path path;
  if (!session || !out || !ParsePrimPath(prim_path, &path)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid payload path");
  }
  return PublishResult(session->session.LoadPayload(path), session->source_dir, session->session.GetRootIdentifier(), out);
}

void lightusd_document_open_config_init(lightusd_document_open_config* out) {
  if (!out) return;
  *out = {};
  out->struct_size = sizeof(*out);
}

lightusd_status lightusd_document_session_configure_open(
    lightusd_document_session* session, const lightusd_document_open_config* config) {
  if (!session || !config || config->struct_size < sizeof(*config) ||
      config->input_policy > LIGHTUSD_INPUT_TRUSTED ||
      (config->flags & ~uint32_t{LIGHTUSD_DOCUMENT_REJECT_PARENT_PATHS | LIGHTUSD_DOCUMENT_COMPOSITION_TIMING}))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid document open configuration");
  if (session->session.IsOpen())
    return Fail(LIGHTUSD_ERR_BUSY, "configure before opening the document");
  // Also reject reentrancy while an unopened session is being opened.
  const auto status = ToStatus(session->session.TrimCaches());
  if (status != LIGHTUSD_OK) return Fail(status, "document operation in progress");
  if (config->opinion_batch_size > (std::numeric_limits<size_t>::max)())
    return Fail(LIGHTUSD_ERR_OVERFLOW, "opinion batch exceeds address space");
  n::pcp::CompositionOptions::VariantSelectionMap overrides;
  const auto parsed = ParseVariants(config->variants, config->variant_count, &overrides);
  if (parsed != LIGHTUSD_OK) return parsed;
  session->options.load.input_policy = config->input_policy == LIGHTUSD_INPUT_TRUSTED
      ? n::InputPolicy::Trusted : n::InputPolicy::Untrusted;
  session->options.resolver.allow_parent_paths = !(config->flags & LIGHTUSD_DOCUMENT_REJECT_PARENT_PATHS);
  session->options.composition.enable_timing = (config->flags & LIGHTUSD_DOCUMENT_COMPOSITION_TIMING) != 0;
  session->options.composition.opinion_batch_size = static_cast<size_t>(config->opinion_batch_size);
  session->options.composition.variant_overrides_by_path = std::move(overrides);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_set_variants(
    lightusd_document_session* session,
    const lightusd_document_variant_selection* selections, size_t count,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !out || (!selections && count))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid variant batch");
  n::pcp::CompositionOptions::VariantSelectionMap overrides;
  const auto status = ParseVariants(selections, count, &overrides);
  if (status != LIGHTUSD_OK) return status;
  return PublishResult(session->session.SetVariantSelections(overrides),
                       session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_load_payloads(
    lightusd_document_session* session, const char* const* prim_paths,
    size_t count, lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !out || (!prim_paths && count))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid payload batch");
  std::vector<n::Path> paths;
  if (count > paths.max_size() ||
      count > (std::numeric_limits<size_t>::max)() / sizeof(*prim_paths))
    return Fail(LIGHTUSD_ERR_OVERFLOW, "payload batch size overflow");
  // Validate incrementally without reserving storage based on an unchecked count.
  for (size_t i = 0; i < count; ++i) {
    n::Path path;
    if (!ParsePrimPath(prim_paths[i], &path))
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid payload batch path");
    paths.push_back(std::move(path));
  }
  return PublishResult(session->session.LoadPayloads(paths), session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_unload_payload(
    lightusd_document_session* session, const char* prim_path,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  n::Path path;
  if (!session || !out || !ParsePrimPath(prim_path, &path)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid payload path");
  }
  return PublishResult(session->session.UnloadPayload(path), session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_reload_layer(
    lightusd_document_session* session, const char* resolved_layer_id,
    lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !resolved_layer_id || !out) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/layer/out is null");
  }
  session->preview_status = LIGHTUSD_OK;
  auto result = session->session.ReloadLayer(resolved_layer_id);
  if (!result && session->preview_status != LIGHTUSD_OK)
    return Fail(session->preview_status, "preview stage allocation failed");
  return PublishResult(std::move(result),
                       session->source_dir, session->session.GetRootIdentifier(), out);
}

lightusd_status lightusd_document_session_snapshot(
    const lightusd_document_session* session, lightusd_document_snapshot** out) {
  if (out) *out = nullptr;
  if (!session || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/out is null");
  if (!session->session.IsOpen()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  auto snapshot = std::unique_ptr<lightusd_document_snapshot>(
      new (std::nothrow) lightusd_document_snapshot());
  if (!snapshot) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "snapshot alloc failed");
  snapshot->snapshot = session->session.GetSnapshot();
  if (!snapshot->snapshot) return Fail(LIGHTUSD_ERR_NOT_FOUND, "session is not open");
  snapshot->changes = session->session.GetLastChangeSet();
  snapshot->source_dir = session->source_dir;
  snapshot->source_filename = session->session.GetRootIdentifier();
  snapshot->warnings = session->session.GetWarning();
  *out = snapshot.release();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_session_root_identifier_copy(
    const lightusd_document_session* session, char* out, size_t cap,
    size_t* required) {
  if (!session || !required) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/required is null");
  }
  const std::string root = session->session.GetRootIdentifier();
  return lightusd_sv_copy({root.data(), root.size()}, out, cap, required);
}

size_t lightusd_document_session_deferred_payload_count(
    const lightusd_document_session* session) {
  return session ? session->session.GetDeferredPayloadPaths().size() : 0;
}

lightusd_status lightusd_document_session_deferred_payload_copy(
    const lightusd_document_session* session, size_t index, char* out,
    size_t cap, size_t* required) {
  if (!session || !required) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "session/required is null");
  }
  const auto paths = session->session.GetDeferredPayloadPaths();
  if (index >= paths.size()) {
    *required = 0;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "deferred payload index out of range");
  }
  const std::string& path = paths[index].str();
  return lightusd_sv_copy({path.data(), path.size()}, out, cap, required);
}

void lightusd_document_snapshot_destroy(lightusd_document_snapshot* snapshot) {
  delete snapshot;
}

uint64_t lightusd_document_snapshot_revision(
    const lightusd_document_snapshot* snapshot) {
  return snapshot ? snapshot->snapshot.revision : 0;
}

int lightusd_document_snapshot_has_prim(
    const lightusd_document_snapshot* snapshot, const char* prim_path) {
  n::Path path;
  return snapshot && snapshot->snapshot &&
                 ParsePrimPath(prim_path, &path) &&
                 snapshot->snapshot->GetPrimAtPath(path).IsValid();
}

uint64_t lightusd_document_snapshot_base_revision(
    const lightusd_document_snapshot* snapshot) {
  return snapshot ? snapshot->changes.base_revision : 0;
}

int lightusd_document_snapshot_full_resync(
    const lightusd_document_snapshot* snapshot) {
  return snapshot && snapshot->changes.full_resync;
}

size_t lightusd_document_snapshot_change_count(
    const lightusd_document_snapshot* snapshot) {
  return snapshot ? snapshot->changes.prims.size() : 0;
}

lightusd_status lightusd_document_snapshot_change(
    const lightusd_document_snapshot* snapshot, size_t index,
    lightusd_sv* prim_path, uint32_t* flags) {
  if (!snapshot || !prim_path || !flags) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "snapshot/path/flags is null");
  }
  if (index >= snapshot->changes.prims.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "change index out of range");
  }
  const auto& change = snapshot->changes.prims[index];
  *prim_path = {change.path.c_str(), change.path.str().size()};
  *flags = static_cast<uint32_t>(change.flags);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_document_snapshot_changes_copy(
    const lightusd_document_snapshot* snapshot, lightusd_document_changes_info* info,
    lightusd_prim_change* prims, size_t prim_capacity,
    lightusd_sv* properties, size_t property_capacity) {
  if (!snapshot || !info || info->struct_size < sizeof(*info) ||
      (!prims && prim_capacity) || (!properties && property_capacity)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid change record output");
  }
  size_t property_count = 0;
  for (const auto& change : snapshot->changes.prims) {
    if (change.properties.size() > std::numeric_limits<size_t>::max() - property_count)
      return Fail(LIGHTUSD_ERR_OVERFLOW, "change property count overflow");
    property_count += change.properties.size();
  }
  *info = {};
  info->struct_size = sizeof(*info);
  info->flags = (snapshot->changes.full_resync ? 1u : 0u) |
                (snapshot->changes.stage_metadata_changed ? 2u : 0u);
  info->base_revision = snapshot->changes.base_revision;
  info->revision = snapshot->snapshot.revision;
  info->prim_count = snapshot->changes.prims.size();
  info->property_count = property_count;
  if (!prims && !properties) return LIGHTUSD_OK;
  if (prim_capacity < snapshot->changes.prims.size() || property_capacity < property_count)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "change record capacity is too small");
  size_t property_offset = 0;
  for (size_t i = 0; i < snapshot->changes.prims.size(); ++i) {
    const auto& change = snapshot->changes.prims[i];
    prims[i] = {change.path.c_str(), static_cast<uint32_t>(change.flags),
                change.properties.empty() ? nullptr : properties + property_offset,
                change.properties.size()};
    for (const auto& property : change.properties)
      properties[property_offset++] = {property.data(), property.size()};
  }
  return LIGHTUSD_OK;
}

}  // extern "C"
