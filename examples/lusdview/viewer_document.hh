// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lightusd-session-cpp.hh"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace lusdview {
// Owned strings back the borrowed C records constructed for render preparation.
struct ViewerPrimChange {
  std::string path;
  uint32_t flags = 0; // lightusd_stage_change_flag bits
  std::vector<std::string> properties;
};
struct ViewerChanges {
  uint64_t base_revision = 0, new_revision = 0;
  bool full_resync = false, stage_metadata_changed = false;
  std::vector<ViewerPrimChange> prims;
  // Aggregate consecutive edits; a revision gap requires full resync.
  bool Append(const ViewerChanges& incoming);
};
// Viewer document owner using only public C handles, events and configuration.
class ViewerDocument {
 public:
  ViewerDocument() = default;
  ViewerDocument(const ViewerDocument&) = delete;
  ViewerDocument& operator=(const ViewerDocument&) = delete;
  using Variants = std::map<std::string, std::map<std::string, std::string>>;
  struct Options {
    // Callback event data is borrowed for the call; retain preview.stage to keep it.
    // The adapter supplies document progress callbacks and open variant pointers.
    lightusd_document_options document;
    lightusd_document_open_config open;
    Variants variants;
    std::function<bool(const lightusd_document_progress&)> progress_callback;
    std::function<bool(const lightusd_document_preview&)> early_preview_callback, preview_callback;
    std::function<bool(lightusd_sv, lightusd_sv)> payload_policy;
    std::function<void(lightusd_sv)> payload_load_callback;
    Options() {
      lightusd_document_options_init(&document);
      lightusd_document_open_config_init(&open);
    }
  };
  struct Edit {
    bool success = false;
    ViewerChanges changes;
    explicit operator bool() const { return success; }
  };
  bool OpenFile(const std::string&, const Options&);
  void SetCallbacks(const Options&);
  void ClearTransientCallbacks();
  bool IsOpen() const { return document_.is_open(); }
  bool IsComposed() const { return document_.is_composed(); }
  lightusd::api::DocumentSnapshot PublicSnapshot() const;
  const ViewerChanges& GetLastChangeSet() const { return changes_; }
  const Variants& GetVariantSelections() const { return variants_; }
  const std::string& GetError() const { return error_; }
  const std::string& GetWarning() const { return warning_; }
  Edit ReloadLayer(const std::string&);
  Edit SetVariantSelections(const Variants&);
  Edit LoadPayloads(const std::vector<std::string>&);
  std::vector<std::string> GetLayerDependencies() const;
  std::vector<std::string> GetDeferredPayloadPaths() const;
  lightusd_document_memory_stats GetMemoryStats() const;
  void ReleaseCompositionCache() { document_.release_cache(); }
  lightusd_geometry_release_stats ReleaseStaticGeometryArrays(size_t minimum = 256);
  lightusd_geometry_release_stats ReleaseStaticGeometryArraysForPrim(
      const std::string& path, size_t minimum = 256);
 private:
  Edit Finish(lightusd_status, const lightusd::api::DocumentSnapshot&);
  lightusd_geometry_release_stats Release(const char*, size_t);
  static int Progress(void*, const lightusd_document_progress*);
  static int Preview(void*, const lightusd_document_preview*);
  static int Payload(void*, lightusd_sv, lightusd_sv);
  static void Selected(void*, lightusd_sv);
  lightusd::api::DocumentSession document_;
  Options callbacks_;
  Variants variants_, initial_variants_;
  ViewerChanges changes_;
  std::string root_, error_, warning_;
};
}
