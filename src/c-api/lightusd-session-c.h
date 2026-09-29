/* SPDX-License-Identifier: Apache-2.0
 * Persistent document session and retained stage snapshots for C consumers.
 */
#ifndef LIGHTUSD_SESSION_C_H_
#define LIGHTUSD_SESSION_C_H_

#include "lightusd-c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lightusd_document_session lightusd_document_session;
typedef struct lightusd_document_snapshot lightusd_document_snapshot;

/* Serialize calls that mutate or reopen one session with render-session
 * creation from it. Retained snapshots are immutable and may be read after
 * subsequent edits or session destruction. */

typedef enum lightusd_stage_change_flag {
  LIGHTUSD_CHANGE_RESYNC = 1u << 0,
  LIGHTUSD_CHANGE_TOPOLOGY = 1u << 1,
  LIGHTUSD_CHANGE_TRANSFORM = 1u << 2,
  LIGHTUSD_CHANGE_PRIMVAR = 1u << 3,
  LIGHTUSD_CHANGE_MATERIAL = 1u << 4,
  LIGHTUSD_CHANGE_TEXTURE = 1u << 5,
  LIGHTUSD_CHANGE_LIGHT = 1u << 6,
  LIGHTUSD_CHANGE_CAMERA = 1u << 7,
  LIGHTUSD_CHANGE_ANIMATION = 1u << 8,
  LIGHTUSD_CHANGE_VISIBILITY = 1u << 9,
  LIGHTUSD_CHANGE_METADATA = 1u << 10
} lightusd_stage_change_flag;

typedef struct lightusd_prim_change {
  const char* prim_path;
  uint32_t flags; /* bitwise OR of lightusd_stage_change_flag */
  const lightusd_sv* properties;
  size_t property_count;
} lightusd_prim_change;

/* Fixed-width description of the immutable snapshot's complete change set. */
typedef struct lightusd_document_changes_info {
  uint32_t struct_size;
  uint32_t flags; /* bit 0 full resync, bit 1 stage metadata changed */
  uint64_t base_revision;
  uint64_t revision;
  uint64_t prim_count;
  uint64_t property_count;
} lightusd_document_changes_info;

typedef struct lightusd_document_progress {
  uint8_t phase; /* 0=root load, 1=compose, 2=recompose, 3=preview */
  float progress;
  lightusd_sv message; /* borrowed only during the callback */
  size_t estimated_resident_bytes;
} lightusd_document_progress;

/* Return zero to cancel the current operation. Callbacks run synchronously
 * on the operation's calling thread. */
typedef int (*lightusd_document_progress_fn)(
    void* userdata, const lightusd_document_progress* progress);

typedef struct lightusd_document_preview {
  lightusd_stage* stage; /* borrowed read-only handle; retain to keep it */
  uint64_t revision; /* provisional; never a published document revision */
  uint32_t phase; /* 0 authored root, 1 composed spatial subset */
  uint32_t flags; /* bit 0 namespace complete, bit 1 spatial subset, bit 2 authoritative */
} lightusd_document_preview;
/* Synchronous on the loading thread. The spatial callback occurs only when
 * composition runs. Return zero to cancel without publishing.
 * Preview stage handles support ordinary read/export APIs and reject authoring.
 * Retain the stage, not this event pointer, to use it after the callback. */
typedef int (*lightusd_document_preview_fn)(void* userdata, const lightusd_document_preview* preview);

/* Return nonzero to select a payload, zero to defer it (not cancellation).
 * Explicit session load/unload rules take precedence over this policy. */
typedef int (*lightusd_document_payload_policy_fn)(
    void* userdata, lightusd_sv prim_path, lightusd_sv asset_path);
/* Reports selection for loading, before asset loading succeeds or fails.
 * Recomposition can report the same prim more than once. */
typedef void (*lightusd_document_payload_selected_fn)(
    void* userdata, lightusd_sv prim_path);

typedef struct lightusd_document_options {
  uint32_t struct_size;
  uint8_t load_payloads;
  /* Former reserved bytes: zero retains the original defaults. */
  uint8_t skip_composition;
  uint8_t cache_retention; /* 0 full, 1 source layers only (drop transient PCP cache) */
  uint8_t _pad;
  uint64_t max_resident_bytes; /* zero keeps the secure library default */
  int32_t max_threads; /* 0=bounded auto, 1=serial, >1=fixed */
  lightusd_document_progress_fn progress_callback;
  void* progress_userdata;
} lightusd_document_options;

typedef struct lightusd_document_memory_stats {
  uint32_t struct_size;
  uint32_t reserved;
  uint64_t source_layer_bytes;
  uint64_t transient_cache_bytes;
  uint64_t composed_stage_bytes;
  uint64_t estimated_total_bytes;
  uint64_t peak_estimated_total_bytes;
  uint64_t layer_count;
  uint64_t prim_index_count;
  uint64_t composed_prim_count;
} lightusd_document_memory_stats;

/* Initializes neither storage nor session: set out.struct_size before querying.
 * Estimates include the session's retained/cache storage, not caller snapshots. */
LIGHTUSD_API lightusd_status lightusd_document_session_memory_stats(
    const lightusd_document_session* session, lightusd_document_memory_stats* out);
/* Move the published stage into a new C owner, then close the session. No stage
 * clone is made. BUSY preserves the session if any external snapshot retains it;
 * release those snapshots first. An unopened/closed session returns NOT_FOUND.
 * On failure *out is NULL. The returned stage keeps source anchors/warnings and
 * may be edited independently or outlive/reopen the original session. */
LIGHTUSD_API lightusd_status lightusd_document_session_take_stage(
    lightusd_document_session* session, lightusd_stage** out);

LIGHTUSD_API void lightusd_document_options_init(lightusd_document_options* out);
LIGHTUSD_API lightusd_status lightusd_document_session_create(
    const lightusd_document_options* options,
    lightusd_document_session** out);
LIGHTUSD_API void lightusd_document_session_destroy(lightusd_document_session* session);
/* Configure callbacks before opening. Registration/userdata must outlive the
 * session or be replaced/cleared between operations. NULL disables delivery.
 * Callbacks may retain preview stages; they must not destroy the session. */
LIGHTUSD_API lightusd_status lightusd_document_session_set_preview_callback(
    lightusd_document_session* session, lightusd_document_preview_fn callback, void* userdata);
/* Set/clear between operations; takes effect on the next open/recomposition.
 * Clears cached composition decisions without publishing a new revision.
 * Reentrant calls during a document operation return BUSY without replacement.
 * NULL policy restores options.load_payloads for paths without explicit rules.
 * Strings are borrowed only during the callback. Callbacks may run on worker
 * threads and userdata must support concurrent calls. Do not mutate/destroy
 * the session from these callbacks. Keep userdata alive until replaced/cleared. */
LIGHTUSD_API lightusd_status lightusd_document_session_set_payload_callbacks(
    lightusd_document_session* session, lightusd_document_payload_policy_fn policy,
    lightusd_document_payload_selected_fn selected, void* userdata);
/* NULL sessions report false. Composition remains true after cache release. */
LIGHTUSD_API int lightusd_document_session_is_open(const lightusd_document_session* session);
LIGHTUSD_API int lightusd_document_session_is_composed(const lightusd_document_session* session);
/* Owned copies of the complete lists, captured by one native query each.
 * Empty/closed sessions return an empty list; NULL arguments are invalid.
 * Destroy with lightusd_strlist_destroy. Failure clears *out. */
LIGHTUSD_API lightusd_status lightusd_document_session_dependencies(
    const lightusd_document_session* session, lightusd_strlist** out);
LIGHTUSD_API lightusd_status lightusd_document_session_deferred_payloads(
    const lightusd_document_session* session, lightusd_strlist** out);
/* Trim transient composition data, or also release parsed source layers. The
 * published snapshot/revision is unchanged; later edits restore caches lazily.
 * Repeated calls are safe. Closed sessions return NOT_FOUND; on open sessions,
 * reentrant calls from operation callbacks return BUSY. Cache destruction may
 * run asynchronously. */
LIGHTUSD_API lightusd_status lightusd_document_session_trim_caches(lightusd_document_session* session);
LIGHTUSD_API lightusd_status lightusd_document_session_release_cache(lightusd_document_session* session);
typedef struct lightusd_geometry_release_stats {
  uint64_t property_count;
  uint64_t element_count;
  uint64_t estimated_payload_bytes;
  uint64_t stage_bytes_before;
  uint64_t stage_bytes_after;
} lightusd_geometry_release_stats;
/* Lossy release of static geometry defaults already copied by a renderer.
 * NULL prim_path releases all; a path releases one prim. Unknown/invalid paths
 * return INVALID_ARG. Non-composed sessions return UNSUPPORTED, closed sessions
 * NOT_FOUND, and reentrant calls BUSY. min_array_elements may be zero.
 * Retained snapshots keep their data; publication revision is unchanged.
 * Declarations, animation and non-geometry properties remain. Rebuild restores
 * authored arrays. Per-prim calls leave stage byte totals zero to avoid scans.
 * Failure leaves out unchanged. */
LIGHTUSD_API lightusd_status lightusd_document_session_release_geometry(
    lightusd_document_session* session, const char* prim_path,
    uint64_t min_array_elements, lightusd_geometry_release_stats* out);
/* Session dependency paths include the root and resolved composition layers.
 * Copies use lightusd_sv_copy's counted byte convention (no NUL added). Serialize with
 * edits; count/index order is only stable until the next session mutation.
 * Cache release preserves dependency information for reload/asset anchoring. */
LIGHTUSD_API size_t lightusd_document_session_dependency_count(const lightusd_document_session* session);
LIGHTUSD_API lightusd_status lightusd_document_session_dependency_copy(
    const lightusd_document_session* session, size_t index, char* out,
    size_t cap, size_t* required);
/* Typed issues accumulated by the current composed cache. */
typedef enum lightusd_document_composition_issue_code {
  LIGHTUSD_COMPOSITION_ARC_CYCLE = 0,
  LIGHTUSD_COMPOSITION_SUBLAYER_CYCLE = 1,
  LIGHTUSD_COMPOSITION_MAX_DEPTH = 2,
  LIGHTUSD_COMPOSITION_INVALID_ASSET_PATH = 3,
  LIGHTUSD_COMPOSITION_UNRESOLVED_PRIM_PATH = 4,
  LIGHTUSD_COMPOSITION_INDEX_CAPACITY = 5,
  LIGHTUSD_COMPOSITION_INVALID_VARIANT_SELECTION = 6,
  LIGHTUSD_COMPOSITION_INVALID_REFERENCE_OFFSET = 7,
  LIGHTUSD_COMPOSITION_EXPRESSION_VARIABLE = 8
} lightusd_document_composition_issue_code;
/* Query with NULL strings and zero capacities to obtain byte counts; copied
 * strings are counted bytes without a NUL terminator. Serialize queries with
 * edits. Cache release preserves the latest issues. */
typedef struct lightusd_document_composition_issue {
  uint32_t struct_size;
  uint32_t code;
  size_t site_bytes;
  size_t message_bytes;
} lightusd_document_composition_issue;
LIGHTUSD_API size_t lightusd_document_session_composition_issue_count(
    const lightusd_document_session* session);
LIGHTUSD_API lightusd_status lightusd_document_session_composition_issue_copy(
    const lightusd_document_session* session, size_t index,
    lightusd_document_composition_issue* info, char* site, size_t site_cap,
    char* message, size_t message_cap);
LIGHTUSD_API lightusd_status lightusd_document_session_open_file(
    lightusd_document_session* session, const char* filename,
    lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_rebuild(
    lightusd_document_session* session, lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_set_variant(
    lightusd_document_session* session, const char* prim_path,
    const char* set_name, const char* selection,
    lightusd_document_snapshot** out);
typedef struct lightusd_document_variant_selection {
  const char* prim_path;
  const char* set_name;
  const char* selection;
} lightusd_document_variant_selection;

typedef enum lightusd_document_open_flag {
  LIGHTUSD_DOCUMENT_REJECT_PARENT_PATHS = 1u << 0,
  LIGHTUSD_DOCUMENT_COMPOSITION_TIMING = 1u << 1
} lightusd_document_open_flag;

typedef struct lightusd_document_open_config {
  uint32_t struct_size;
  uint32_t flags; /* bitwise OR of lightusd_document_open_flag */
  uint32_t input_policy; /* lightusd_input_policy; default UNTRUSTED */
  uint64_t opinion_batch_size; /* zero selects the native automatic batch size */
  const lightusd_document_variant_selection* variants;
  size_t variant_count;
} lightusd_document_open_config;
LIGHTUSD_API void lightusd_document_open_config_init(lightusd_document_open_config* out);
/* Configure an unopened session (BUSY once open). Strings are copied and need
 * only survive this call. Initial variants apply to the first composition and
 * its composed spatial preview (the early root preview remains authored),
 * avoiding an intermediate composition with authored selections.
 * Untrusted input always rejects absolute/parent asset paths regardless of
 * flags. Trusted input permits compatibility resolution unless restricted.
 * Replaces this configuration atomically on success; invalid fields/duplicates
 * leave the previous configuration intact. Zero config preserves defaults. */
LIGHTUSD_API lightusd_status lightusd_document_session_configure_open(
    lightusd_document_session* session, const lightusd_document_open_config* config);
/* Replace all session variant overrides in one recomposition. A NULL array
 * with zero count clears all overrides (restoring authored selections).
 * Strings are borrowed only for this call. Duplicate prim/set pairs and empty
 * names/selections are rejected before any edit. Failed recomposition preserves
 * publication. As with other edit calls, failure clears *out. */
LIGHTUSD_API lightusd_status lightusd_document_session_set_variants(
    lightusd_document_session* session,
    const lightusd_document_variant_selection* selections, size_t count,
    lightusd_document_snapshot** out);
/* Load the listed payloads, including descendants, in one recomposition.
 * NULL with zero count is an empty batch. Invalid paths reject the entire
 * batch before editing; failed recomposition restores the prior load rules. */
LIGHTUSD_API lightusd_status lightusd_document_session_load_payloads(
    lightusd_document_session* session, const char* const* prim_paths,
    size_t count, lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_load_payload(
    lightusd_document_session* session, const char* prim_path,
    lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_unload_payload(
    lightusd_document_session* session, const char* prim_path,
    lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_reload_layer(
    lightusd_document_session* session, const char* resolved_layer_id,
    lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_snapshot(
    const lightusd_document_session* session, lightusd_document_snapshot** out);
LIGHTUSD_API lightusd_status lightusd_document_session_root_identifier_copy(
    const lightusd_document_session* session, char* out, size_t cap,
    size_t* required);
LIGHTUSD_API size_t lightusd_document_session_deferred_payload_count(
    const lightusd_document_session* session);
LIGHTUSD_API lightusd_status lightusd_document_session_deferred_payload_copy(
    const lightusd_document_session* session, size_t index, char* out,
    size_t cap, size_t* required);

LIGHTUSD_API void lightusd_document_snapshot_destroy(lightusd_document_snapshot* snapshot);
/* Retain the immutable stage without cloning scene data. The returned C stage
 * owns a reference independent of the snapshot/session and supports all stage
 * read, export and render APIs. Authoring returns UNSUPPORTED. Flattening creates
 * an independent editable stage. Release with lightusd_stage_destroy.
 * A retained view also prevents session_take_stage until released. */
LIGHTUSD_API lightusd_status lightusd_document_snapshot_stage(
    const lightusd_document_snapshot* snapshot, lightusd_stage** out);
LIGHTUSD_API uint64_t lightusd_document_snapshot_revision(
    const lightusd_document_snapshot* snapshot);
LIGHTUSD_API int lightusd_document_snapshot_has_prim(
    const lightusd_document_snapshot* snapshot, const char* prim_path);
/* Snapshots own immutable stages and change records. They remain valid after
 * later edits or destruction of the document session. */
LIGHTUSD_API uint64_t lightusd_document_snapshot_base_revision(
    const lightusd_document_snapshot* snapshot);
LIGHTUSD_API int lightusd_document_snapshot_full_resync(
    const lightusd_document_snapshot* snapshot);
LIGHTUSD_API size_t lightusd_document_snapshot_change_count(
    const lightusd_document_snapshot* snapshot);
LIGHTUSD_API lightusd_status lightusd_document_snapshot_change(
    const lightusd_document_snapshot* snapshot, size_t index,
    lightusd_sv* prim_path, uint32_t* flags);
/* Successful queries/copies perform no native allocation. Initialize info.struct_size.
 * Both arrays NULL with zero capacities queries counts/metadata only. Otherwise
 * supply enough caller-owned, non-overlapping storage for both arrays. Insufficient
 * capacity returns INVALID_ARG with required counts in info and leaves both arrays
 * unchanged. Each prim's properties points into the supplied properties array;
 * paths (NUL terminated) and counted property text borrow this immutable snapshot.
 * Keep the snapshot and caller arrays alive while consuming the records. Safe for
 * concurrent reads with separate output arrays. Invalid info leaves all outputs
 * untouched. Null pointers require zero capacity. */
LIGHTUSD_API lightusd_status lightusd_document_snapshot_changes_copy(
    const lightusd_document_snapshot* snapshot, lightusd_document_changes_info* info,
    lightusd_prim_change* prims, size_t prim_capacity,
    lightusd_sv* properties, size_t property_capacity);

#ifdef __cplusplus
}
#endif
#endif
