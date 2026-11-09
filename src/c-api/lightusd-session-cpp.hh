// SPDX-License-Identifier: Apache-2.0
// C++17 ownership facade for the persistent C document boundary.
#pragma once

#include "lightusd-cpp.hh"
#include "lightusd-session-c.h"

namespace lightusd {
namespace api {

using DocumentSnapshot = Owner<lightusd_document_snapshot,
                               lightusd_document_snapshot_destroy>;

inline void InitDocumentOptions(lightusd_document_options* options) noexcept {
  lightusd_document_options_init(options);
}

inline uint64_t DocumentSnapshotRevision(const DocumentSnapshot& snapshot) noexcept {
  return lightusd_document_snapshot_revision(snapshot.get());
}

inline bool DocumentSnapshotHasPrim(const DocumentSnapshot& snapshot,
                                     const char* prim_path) noexcept {
  return lightusd_document_snapshot_has_prim(snapshot.get(), prim_path) != 0;
}

inline lightusd_status DocumentSnapshotStage(const DocumentSnapshot& snapshot,
                                             Stage* out) {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  lightusd_stage* stage = nullptr;
  const auto status = lightusd_document_snapshot_stage(snapshot.get(), &stage);
  if (status == LIGHTUSD_OK) *out = Stage(stage);
  return status;
}

inline lightusd_status DocumentSnapshotChangesCopy(
    const DocumentSnapshot& snapshot, lightusd_document_changes_info* info,
    lightusd_prim_change* prims = nullptr, size_t prim_capacity = 0,
    lightusd_sv* properties = nullptr, size_t property_capacity = 0) noexcept {
  return lightusd_document_snapshot_changes_copy(snapshot.get(), info,
      prims, prim_capacity, properties, property_capacity);
}

class DocumentSession {
 public:
  lightusd_status configure_open(const lightusd_document_open_config* config) {
    return lightusd_document_session_configure_open(handle_.get(), config);
  }
  lightusd_status set_payload_callbacks(lightusd_document_payload_policy_fn policy,
      lightusd_document_payload_selected_fn selected, void* userdata = nullptr) {
    return lightusd_document_session_set_payload_callbacks(handle_.get(), policy, selected, userdata);
  }
  lightusd_status set_preview_callback(lightusd_document_preview_fn callback, void* userdata = nullptr) {
    return lightusd_document_session_set_preview_callback(handle_.get(), callback, userdata);
  }
  lightusd_status create(const lightusd_document_options* options = nullptr) {
    return lightusd_document_session_create(options, handle_.put());
  }
  lightusd_status take_stage(Stage* out) {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_stage* stage = nullptr;
    const auto status = lightusd_document_session_take_stage(handle_.get(), &stage);
    if (status == LIGHTUSD_OK) *out = Stage(stage);
    return status;
  }
  lightusd_status memory_stats(lightusd_document_memory_stats* out) const {
    return lightusd_document_session_memory_stats(handle_.get(), out);
  }
  lightusd_document_session* get() const noexcept { return handle_.get(); }
  bool is_open() const { return lightusd_document_session_is_open(handle_.get()) != 0; }
  bool is_composed() const { return lightusd_document_session_is_composed(handle_.get()) != 0; }
  lightusd_status trim_caches() { return lightusd_document_session_trim_caches(handle_.get()); }
  lightusd_status release_cache() { return lightusd_document_session_release_cache(handle_.get()); }
  lightusd_status release_geometry(const char* prim_path, uint64_t min_array_elements,
                                   lightusd_geometry_release_stats* out) {
    return lightusd_document_session_release_geometry(handle_.get(), prim_path, min_array_elements, out);
  }
  size_t dependency_count() const { return lightusd_document_session_dependency_count(handle_.get()); }
  lightusd_status dependencies(StringList* out) const {
    return out ? lightusd_document_session_dependencies(handle_.get(), out->put()) : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status deferred_payloads(StringList* out) const {
    return out ? lightusd_document_session_deferred_payloads(handle_.get(), out->put()) : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status dependency_copy(size_t index, char* out, size_t cap, size_t* required) const {
    return lightusd_document_session_dependency_copy(handle_.get(), index, out, cap, required);
  }
  size_t composition_issue_count() const {
    return lightusd_document_session_composition_issue_count(handle_.get());
  }
  lightusd_status composition_issue_copy(
      size_t index, lightusd_document_composition_issue* info,
      char* site = nullptr, size_t site_cap = 0,
      char* message = nullptr, size_t message_cap = 0) const {
    return lightusd_document_session_composition_issue_copy(
        handle_.get(), index, info, site, site_cap, message, message_cap);
  }
  explicit operator bool() const noexcept { return bool(handle_); }
  lightusd_status open_file(const char* filename, DocumentSnapshot* out) {
    return out ? lightusd_document_session_open_file(handle_.get(), filename,
                                                     out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status rebuild(DocumentSnapshot* out) {
    return out ? lightusd_document_session_rebuild(handle_.get(), out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status set_variant(const char* prim_path, const char* set_name,
                              const char* selection, DocumentSnapshot* out) {
    return out ? lightusd_document_session_set_variant(
                     handle_.get(), prim_path, set_name, selection, out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status load_payload(const char* prim_path, DocumentSnapshot* out) {
    return out ? lightusd_document_session_load_payload(handle_.get(), prim_path,
                                                         out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status set_variants(const lightusd_document_variant_selection* selections,
                               size_t count, DocumentSnapshot* out) {
    return out ? lightusd_document_session_set_variants(
                     handle_.get(), selections, count, out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status load_payloads(const char* const* paths, size_t count,
                               DocumentSnapshot* out) {
    return out ? lightusd_document_session_load_payloads(
                     handle_.get(), paths, count, out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status unload_payload(const char* prim_path, DocumentSnapshot* out) {
    return out ? lightusd_document_session_unload_payload(handle_.get(), prim_path,
                                                           out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status reload_layer(const char* layer_id, DocumentSnapshot* out) {
    return out ? lightusd_document_session_reload_layer(handle_.get(), layer_id,
                                                         out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status snapshot(DocumentSnapshot* out) const {
    return out ? lightusd_document_session_snapshot(handle_.get(), out->put())
               : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status root_identifier_copy(char* out, size_t cap,
                                       size_t* required) const {
    return lightusd_document_session_root_identifier_copy(handle_.get(), out,
                                                           cap, required);
  }
  size_t deferred_payload_count() const noexcept {
    return lightusd_document_session_deferred_payload_count(handle_.get());
  }
  lightusd_status deferred_payload_copy(size_t index, char* out, size_t cap,
                                        size_t* required) const {
    return lightusd_document_session_deferred_payload_copy(
        handle_.get(), index, out, cap, required);
  }

 private:
  Owner<lightusd_document_session, lightusd_document_session_destroy> handle_;
};

}  // namespace api
}  // namespace lightusd
