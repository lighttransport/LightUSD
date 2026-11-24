// SPDX-License-Identifier: Apache-2.0
#include "lightusd-render-cpp.hh"

#include <atomic>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace lightusd::api;

int OnProgress(void* userdata, const lightusd_document_progress*) {
  return !*static_cast<bool*>(userdata);
}

void OnPayloadSelected(void* userdata, lightusd_sv) {
  static_cast<std::atomic<unsigned>*>(userdata)->fetch_add(1, std::memory_order_relaxed);
}

struct Previews { Stage authored, spatial; };
int OnPreview(void* userdata, const lightusd_document_preview* event) {
  if (!event || event->phase > 1 || !lightusd_stage_is_read_only(event->stage)) return 0;
  auto* previews = static_cast<Previews*>(userdata);
  lightusd_stage_retain(event->stage);
  Stage retained(event->stage);
  if (event->phase == 0) previews->authored = std::move(retained);
  else previews->spatial = std::move(retained);
  std::cout << "provisional preview phase=" << event->phase << '\n';
  return 1;
}

int OnRenderEvent(void*, const lightusd_render_event* event) {
  if (!event) return 0;
  switch (event->type) {
    case LIGHTUSD_RENDER_EVENT_BEGIN:
      std::cout << "begin " << event->base_revision << " -> "
                << event->revision << " full=" << unsigned(event->full_resync)
                << '\n';
      break;
    case LIGHTUSD_RENDER_EVENT_UPSERT:
      if (event->kind == 2) std::cout << "mesh " << event->resource_id << '\n';
      break;
    case LIGHTUSD_RENDER_EVENT_REMOVE:
      std::cout << "remove " << event->resource_id << '\n';
      break;
    case LIGHTUSD_RENDER_EVENT_END:
    case LIGHTUSD_RENDER_EVENT_ABORT:
      break;
    default: return 0;
  }
  return 1;
}

bool Check(lightusd_status status) {
  if (status == LIGHTUSD_OK) return true;
  std::cerr << LastError() << '\n';
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: interactive_session ROOT.usda\n";
    return 2;
  }
  bool cancel = false;
  lightusd_document_options options;
  InitDocumentOptions(&options);
  options.load_payloads = 0;
  options.progress_callback = OnProgress;
  options.progress_userdata = &cancel;
  Previews previews;
  std::atomic<unsigned> selectedPayloads{0};
  DocumentSession document;
  if (!Check(document.create(&options))) return 1;
  lightusd_document_open_config openConfig;
  lightusd_document_open_config_init(&openConfig);
  openConfig.opinion_batch_size = 64;
  if (!Check(document.configure_open(&openConfig))) return 1;
  if (!Check(document.set_preview_callback(OnPreview, &previews))) return 1;
  if (!Check(document.set_payload_callbacks(nullptr, OnPayloadSelected,
                                             &selectedPayloads))) return 1;

  DocumentSnapshot retained;
  if (!Check(document.open_file(argv[1], &retained))) return 1;
  {
    Stage view;
    if (!Check(DocumentSnapshotStage(retained, &view)) || !view.is_read_only() ||
        !view.prim("/World")) return 1;
    std::cout << "read-only snapshot stage queried without cloning\n";
  }

  RenderSession renderer;
  if (!Check(renderer.create(document))) return 1;
  lightusd_render_event_sink sink;
  InitRenderEventSink(&sink);
  sink.callback = OnRenderEvent;
  if (!Check(renderer.set_event_sink(&sink))) return 1;

  auto apply = [&](const DocumentSnapshot& snapshot) {
    lightusd_document_changes_info changes{};
    changes.struct_size = sizeof(changes);
    if (!Check(DocumentSnapshotChangesCopy(snapshot, &changes))) return false;
    std::vector<lightusd_prim_change> prims(static_cast<size_t>(changes.prim_count));
    std::vector<lightusd_sv> properties(static_cast<size_t>(changes.property_count));
    if (!Check(DocumentSnapshotChangesCopy(snapshot, &changes,
        prims.data(), prims.size(), properties.data(), properties.size()))) return false;
    std::cout << "changes " << changes.base_revision << " -> " << changes.revision
              << " prims=" << changes.prim_count << " properties=" << changes.property_count
              << " metadata=" << bool(changes.flags & 2u) << '\n';
    for (const auto& prim : prims) {
      for (size_t i = 0; i < prim.property_count; ++i) {
        std::cout << "property " << prim.prim_path << '.';
        std::cout.write(prim.properties[i].data,
                        static_cast<std::streamsize>(prim.properties[i].len));
        std::cout << '\n';
      }
    }
    RenderScene scene;
    lightusd_render_update_info info;
    InitRenderUpdateInfo(&info);
    if (!Check(renderer.apply(snapshot, &scene, &info))) return false;
    std::cout << "converted=" << info.converted_resource_count
              << " upserts=" << info.upsert_count << '\n';
    return true;
  };
  if (!apply(retained)) return 1;

  auto edit = [&](auto operation) {
    DocumentSnapshot next;
    if (!Check(operation(&next))) return false;
    return apply(next);
  };
  const lightusd_document_variant_selection selections[] = {
      {"/World", "display", "high"}};
  if (!edit([&](DocumentSnapshot* out) {
        return document.set_variants(selections, 1, out);
      })) return 1;

  const size_t payload_count = document.deferred_payload_count();
  std::vector<std::string> payload_paths;
  payload_paths.reserve(payload_count);
  for (size_t i = 0; i < payload_count; ++i) {
    size_t bytes = 0;
    if (!Check(document.deferred_payload_copy(i, nullptr, 0, &bytes))) return 1;
    std::string path(bytes, '\0');
    if (!Check(document.deferred_payload_copy(
            i, path.data(), path.size(), &bytes))) return 1;
    payload_paths.push_back(std::move(path));
  }
  std::vector<const char*> payload_names;
  payload_names.reserve(payload_paths.size());
  for (const auto& path : payload_paths) payload_names.push_back(path.c_str());
  if (!payload_names.empty() && !edit([&](DocumentSnapshot* out) {
        return document.load_payloads(payload_names.data(), payload_names.size(), out);
      })) return 1;
  for (const auto& path : payload_paths) {
    if (!edit([&](DocumentSnapshot* out) {
          return document.unload_payload(path.c_str(), out);
        })) return 1;
  }
  std::cout << "payload selections="
            << selectedPayloads.load(std::memory_order_relaxed) << '\n';

  for (size_t i = 0; i < document.dependency_count(); ++i) {
    size_t bytes = 0;
    if (!Check(document.dependency_copy(i, nullptr, 0, &bytes))) return 1;
    std::string dependency(bytes, '\0');
    if (!Check(document.dependency_copy(i, dependency.data(), bytes, &bytes))) return 1;
    std::cout << "dependency " << dependency.c_str() << '\n';
  }
  if (!document.is_open() || !document.is_composed() ||
      !Check(document.trim_caches()) || !Check(document.release_cache())) return 1;
  std::cout << "released composition cache; retained snapshot remains readable\n";
  if (!DocumentSnapshotHasPrim(retained, "/World")) return 1;

  size_t root_bytes = 0;
  if (!Check(document.root_identifier_copy(nullptr, 0, &root_bytes))) return 1;
  std::string root(root_bytes, '\0');
  if (!Check(document.root_identifier_copy(
          root.data(), root.size(), &root_bytes))) return 1;
  if (!edit([&](DocumentSnapshot* out) {
        return document.reload_layer(root.c_str(), out);
      })) return 1;

  DocumentSnapshot before;
  if (!Check(document.snapshot(&before))) return 1;
  cancel = true;
  DocumentSnapshot cancelled_snapshot;
  const lightusd_status cancelled = document.rebuild(&cancelled_snapshot);
  if (cancelled != LIGHTUSD_ERR_CANCELLED || cancelled_snapshot) return 1;
  DocumentSnapshot after;
  if (!Check(document.snapshot(&after))) return 1;
  if (DocumentSnapshotRevision(after) != DocumentSnapshotRevision(before))
    return 1;
  if (!DocumentSnapshotHasPrim(retained, "/World")) return 1;
  std::cout << "retained revision="
            << DocumentSnapshotRevision(retained)
            << "; cancellation preserved current revision\n";
  lightusd_document_memory_stats memory{};
  memory.struct_size = sizeof(memory);
  if (!Check(document.memory_stats(&memory))) return 1;
  std::cout << "session bytes=" << memory.estimated_total_bytes
            << " peak=" << memory.peak_estimated_total_bytes << '\n';
  Stage transferred;
  if (document.take_stage(&transferred) != LIGHTUSD_ERR_BUSY || transferred) return 1;
  before.reset();
  after.reset();
  retained.reset();
  renderer.reset();
  // Conversion is complete; compact static defaults before transferring the stage.
  lightusd_geometry_release_stats released{};
  if (!Check(document.release_geometry(nullptr, 1, &released))) return 1;
  std::cout << "released static geometry properties=" << released.property_count << '\n';
  if (!Check(document.take_stage(&transferred)) || !transferred.prim("/World")) return 1;
  std::cout << "transferred stage without cloning\n";
  return 0;
}
