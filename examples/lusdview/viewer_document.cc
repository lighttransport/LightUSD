// SPDX-License-Identifier: Apache-2.0
#include "viewer_document.hh"
#include <algorithm>

namespace lusdview {
namespace api = lightusd::api;
namespace {
std::vector<lightusd_document_variant_selection> VariantRecords(const ViewerDocument::Variants& selections) {
  std::vector<lightusd_document_variant_selection> result;
  for (const auto& prim : selections)
    for (const auto& set : prim.second)
      result.push_back({prim.first.c_str(), set.first.c_str(), set.second.c_str()});
  return result;
}
}

bool ViewerChanges::Append(const ViewerChanges& incoming) {
  bool continuous = true;
  if (new_revision == base_revision && prims.empty() && !full_resync &&
      !stage_metadata_changed) {
    base_revision = incoming.base_revision;
  } else if (new_revision != incoming.base_revision) {
    continuous = false;
    full_resync = true;
  }
  new_revision = incoming.new_revision;
  full_resync = full_resync || incoming.full_resync;
  stage_metadata_changed = stage_metadata_changed || incoming.stage_metadata_changed;
  for (const auto& change : incoming.prims) {
    auto found = std::find_if(prims.begin(), prims.end(),
        [&](const ViewerPrimChange& existing) { return existing.path == change.path; });
    if (found == prims.end()) {
      prims.push_back(change);
      continue;
    }
    found->flags |= change.flags;
    for (const auto& property : change.properties) {
      if (std::find(found->properties.begin(), found->properties.end(), property) == found->properties.end())
        found->properties.push_back(property);
    }
  }
  return continuous;
}

void ViewerDocument::SetCallbacks(const Options& options) {
  callbacks_ = options;
  if (document_) document_.set_preview_callback(
      options.early_preview_callback || options.preview_callback ? Preview : nullptr, this);
}
void ViewerDocument::ClearTransientCallbacks() {
  callbacks_.progress_callback = {};
  callbacks_.early_preview_callback = {};
  callbacks_.preview_callback = {};
  callbacks_.payload_load_callback = {};
  if (document_) document_.set_preview_callback(nullptr);
}
int ViewerDocument::Progress(void* userdata, const lightusd_document_progress* input) {
  auto& self = *static_cast<ViewerDocument*>(userdata);
  if (!self.callbacks_.progress_callback) return 1;
  return self.callbacks_.progress_callback(*input);
}
int ViewerDocument::Preview(void* userdata, const lightusd_document_preview* input) {
  auto& self = *static_cast<ViewerDocument*>(userdata);
  const auto& callback = input->phase == 0 ? self.callbacks_.early_preview_callback : self.callbacks_.preview_callback;
  if (!callback) return 1;
  return callback(*input);
}
int ViewerDocument::Payload(void* userdata, lightusd_sv path, lightusd_sv asset) {
  auto& options = static_cast<ViewerDocument*>(userdata)->callbacks_;
  return options.payload_policy ? options.payload_policy(
      path, asset) : options.document.load_payloads;
}
void ViewerDocument::Selected(void* userdata, lightusd_sv path) {
  auto& callback = static_cast<ViewerDocument*>(userdata)->callbacks_.payload_load_callback;
  if (callback) callback(path);
}

bool ViewerDocument::OpenFile(const std::string& path, const Options& options) {
  if (IsOpen()) { error_ = "document is already open; use ReloadLayer"; return false; }
  callbacks_ = options;
  lightusd_document_options config = options.document;
  config.progress_callback = Progress;
  config.progress_userdata = this;
  auto status = document_.create(&config);
  lightusd_document_open_config open = options.open;
  const auto records = VariantRecords(options.variants);
  open.variants = records.data();
  open.variant_count = records.size();
  if (status == LIGHTUSD_OK) status = document_.configure_open(&open);
  if (status == LIGHTUSD_OK) status = document_.set_payload_callbacks(Payload, Selected, this);
  if (status == LIGHTUSD_OK) SetCallbacks(options);
  api::DocumentSnapshot snapshot;
  if (status == LIGHTUSD_OK) status = document_.open_file(path.c_str(), &snapshot);
  auto result = Finish(status, snapshot);
  if (result) {
    root_ = path;
    initial_variants_ = options.variants;
    variants_ = initial_variants_;
  }
  return bool(result);
}
ViewerDocument::Edit ViewerDocument::Finish(lightusd_status status, const api::DocumentSnapshot& snapshot) {
  Edit result;
  if (status != LIGHTUSD_OK) { error_ = api::LastError(); return result; }
  lightusd_document_changes_info info{};
  info.struct_size = sizeof(info);
  status = api::DocumentSnapshotChangesCopy(snapshot, &info);
  std::vector<lightusd_prim_change> prims(static_cast<size_t>(info.prim_count));
  std::vector<lightusd_sv> properties(static_cast<size_t>(info.property_count));
  if (status == LIGHTUSD_OK) status = api::DocumentSnapshotChangesCopy(snapshot, &info,
      prims.data(), prims.size(), properties.data(), properties.size());
  if (status != LIGHTUSD_OK) { error_ = api::LastError(); return result; }
  result.changes.base_revision = info.base_revision;
  result.changes.new_revision = info.revision;
  result.changes.full_resync = (info.flags & 1) != 0;
  result.changes.stage_metadata_changed = (info.flags & 2) != 0;
  for (const auto& prim : prims) {
    ViewerPrimChange change;
    change.path = prim.prim_path;
    change.flags = prim.flags;
    for (size_t i = 0; i < prim.property_count; ++i)
      change.properties.emplace_back(prim.properties[i].data, prim.properties[i].len);
    result.changes.prims.push_back(std::move(change));
  }
  api::Stage stage;
  warning_.clear();
  if (api::DocumentSnapshotStage(snapshot, &stage) == LIGHTUSD_OK) {
    api::String text;
    if (stage.take_warnings(&text) == LIGHTUSD_OK) {
      const auto value = api::StringView(text);
      warning_.assign(value.data, value.len);
    }
  }
  error_.clear();
  changes_ = result.changes;
  result.success = true;
  return result;
}
api::DocumentSnapshot ViewerDocument::PublicSnapshot() const {
  api::DocumentSnapshot result;
  document_.snapshot(&result);
  return result;
}
ViewerDocument::Edit ViewerDocument::ReloadLayer(const std::string& path) {
  api::DocumentSnapshot snapshot;
  const auto status = document_.reload_layer(path.c_str(), &snapshot);
  auto result = Finish(status, snapshot);
  if (result && path == root_) variants_ = initial_variants_;
  return result;
}
ViewerDocument::Edit ViewerDocument::SetVariantSelections(const Variants& selections) {
  const auto records = VariantRecords(selections);
  api::DocumentSnapshot snapshot;
  const auto status = document_.set_variants(records.data(), records.size(), &snapshot);
  auto result = Finish(status, snapshot);
  if (result) variants_ = selections;
  return result;
}
ViewerDocument::Edit ViewerDocument::LoadPayloads(const std::vector<std::string>& paths) {
  std::vector<const char*> records;
  for (const auto& path : paths) records.push_back(path.c_str());
  api::DocumentSnapshot snapshot;
  const auto status = document_.load_payloads(records.data(), records.size(), &snapshot);
  return Finish(status, snapshot);
}
std::vector<std::string> ViewerDocument::GetLayerDependencies() const {
  std::vector<std::string> result;
  api::StringList paths;
  if (document_.dependencies(&paths) != LIGHTUSD_OK) return result;
  result.reserve(api::StringListSize(paths));
  for (size_t i = 0; i < api::StringListSize(paths); ++i) {
    const auto path = api::StringListGet(paths, i);
    result.emplace_back(path.data, path.len);
  }
  return result;
}
std::vector<std::string> ViewerDocument::GetDeferredPayloadPaths() const {
  std::vector<std::string> result;
  api::StringList paths;
  if (document_.deferred_payloads(&paths) != LIGHTUSD_OK) return result;
  result.reserve(api::StringListSize(paths));
  for (size_t i = 0; i < api::StringListSize(paths); ++i) {
    const auto path = api::StringListGet(paths, i);
    result.emplace_back(path.data, path.len);
  }
  return result;
}
lightusd_document_memory_stats ViewerDocument::GetMemoryStats() const {
  lightusd_document_memory_stats info{};
  info.struct_size = sizeof(info);
  if (document_.memory_stats(&info) != LIGHTUSD_OK) return {};
  return info;
}
lightusd_geometry_release_stats ViewerDocument::Release(const char* path, size_t minimum) {
  lightusd_geometry_release_stats stats{};
  if (document_.release_geometry(path, minimum, &stats) != LIGHTUSD_OK) return {};
  return stats;
}
lightusd_geometry_release_stats ViewerDocument::ReleaseStaticGeometryArrays(size_t minimum) { return Release(nullptr, minimum); }
lightusd_geometry_release_stats ViewerDocument::ReleaseStaticGeometryArraysForPrim(const std::string& path, size_t minimum) {
  return Release(path.c_str(), minimum);
}
}
