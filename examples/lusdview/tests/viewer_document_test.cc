// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "viewer_document.hh"

namespace api = lightusd::api;
static api::Stage PublishedStage(const lusdview::ViewerDocument& document) {
  api::Stage stage;
  assert(api::DocumentSnapshotStage(document.PublicSnapshot(), &stage) == LIGHTUSD_OK);
  return stage;
}
static bool HasPrim(const api::Stage& stage, const char* path) {
  return lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage.get(), path));
}
static void TestChangeAggregation() {
  lusdview::ViewerChanges aggregate;
  lusdview::ViewerChanges first;
  first.base_revision = 2;
  first.new_revision = 3;
  first.prims.push_back({"/Mesh", LIGHTUSD_CHANGE_TOPOLOGY, {"points"}});
  assert(aggregate.Append(first));
  lusdview::ViewerChanges second;
  second.base_revision = 3;
  second.new_revision = 5;
  second.stage_metadata_changed = true;
  second.prims.push_back({"/Mesh", LIGHTUSD_CHANGE_TRANSFORM, {"points", "xformOp:translate"}});
  second.prims.push_back({"/Light", LIGHTUSD_CHANGE_LIGHT, {"inputs:intensity"}});
  assert(aggregate.Append(second));
  first.prims.clear();
  second.prims.clear();
  assert(aggregate.base_revision == 2 && aggregate.new_revision == 5);
  assert(!aggregate.full_resync && aggregate.stage_metadata_changed);
  assert(aggregate.prims.size() == 2);
  assert(aggregate.prims[0].flags == (LIGHTUSD_CHANGE_TOPOLOGY | LIGHTUSD_CHANGE_TRANSFORM));
  assert(aggregate.prims[0].properties.size() == 2);
  assert(aggregate.prims[0].properties[1] == "xformOp:translate");
  second.base_revision = 6;
  second.new_revision = 7;
  assert(!aggregate.Append(second));
  assert(aggregate.full_resync && aggregate.base_revision == 2 && aggregate.new_revision == 7);
}
int main() {
  TestChangeAggregation();
  const auto directory = std::filesystem::temp_directory_path() /
      ("lusdview-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto root = directory / "root.usda";
  {
    std::ofstream file(root);
    file << R"(#usda 1.0
def Xform "Source" { def Mesh "Child" {
 point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 float3[] extent = [(0,0,0),(1,1,0)]
} }
def Xform "Root" (
 prepend payload = </Source>
 variants = { string look = "low" }
 prepend variantSets = "look"
) {
 variantSet "look" = {
  "low" { def Scope "Low" {} }
  "high" { def Scope "High" {} }
 }
}
)";
  }
  api::Stage retained, retainedPreview;
  {
    lusdview::ViewerDocument document;
    lusdview::ViewerDocument::Options options;
    options.document.max_threads = 1;
    options.document.load_payloads = false;
    options.variants["/Root"]["look"] = "high";
    unsigned progress = 0, previews = 0, selected = 0;
    options.progress_callback = [&](const lightusd_document_progress&) { ++progress; return true; };
    options.preview_callback = [&](const lightusd_document_preview& view) {
      assert(view.stage && view.phase == 1);
      lightusd_stage_retain(view.stage);
      retainedPreview = api::Stage(view.stage);
      ++previews;
      return true;
    };
    options.payload_load_callback = [&](lightusd_sv) { ++selected; };
    assert(document.OpenFile(root.string(), options));
    assert(document.IsOpen() && document.IsComposed());
    assert(progress > 0 && previews == 1 && selected == 0);
    retained = PublishedStage(document);
    const auto retainedRevision = api::DocumentSnapshotRevision(document.PublicSnapshot());
    assert(retained && HasPrim(retained, "/Root/High"));
    assert(!HasPrim(retained, "/Root/Child"));
    const auto deferred = document.GetDeferredPayloadPaths();
    assert(deferred == std::vector<std::string>{"/Root"});
    assert(!document.GetLayerDependencies().empty());
    const auto memory = document.GetMemoryStats();
    assert(memory.struct_size == sizeof(lightusd_document_memory_stats));
    assert(memory.composed_stage_bytes > 0);
    auto loaded = document.LoadPayloads({"/Root"});
    assert(loaded && selected > 0);
    assert(document.GetDeferredPayloadPaths().empty());
    assert(deferred == std::vector<std::string>{"/Root"});
    assert(loaded.changes.base_revision == retainedRevision);
    assert(HasPrim(PublishedStage(document), "/Root/Child"));
    const unsigned old_progress = progress, old_previews = previews, old_selected = selected;
    document.ClearTransientCallbacks();
    document.ReleaseCompositionCache();
    lusdview::ViewerDocument::Variants variants;
    variants["/Root"]["look"] = "low";
    auto changed = document.SetVariantSelections(variants);
    assert(changed && document.GetVariantSelections() == variants);
    auto snapshot = PublishedStage(document);
    assert(HasPrim(snapshot, "/Root/Low"));
    assert(HasPrim(snapshot, "/Root/Child"));
    const auto released = document.ReleaseStaticGeometryArraysForPrim("/Root/Child", 1);
    assert(released.property_count > 0);
    lightusd_value_view points{};
    assert(lightusd_attr_inspect_default(lightusd_stage_prim_at_path(snapshot.get(), "/Root/Child"),
        "points", &points, nullptr) == LIGHTUSD_OK);
    assert(points.is_array && points.count == 3);
    auto afterRelease = PublishedStage(document);
    assert(lightusd_attr_inspect_default(lightusd_stage_prim_at_path(afterRelease.get(), "/Root/Child"),
        "points", &points, nullptr) == LIGHTUSD_ERR_NOT_FOUND);
    assert(document.ReleaseStaticGeometryArraysForPrim("/Root/Child", 1).property_count == 0);
    assert(document.ReleaseStaticGeometryArraysForPrim("/Missing", 1).property_count == 0);
    auto reloaded = document.ReloadLayer(root.string());
    assert(reloaded && HasPrim(PublishedStage(document), "/Root/High"));
    assert(document.GetVariantSelections() == options.variants);
    assert(progress == old_progress && previews == old_previews && selected == old_selected);
    assert(HasPrim(retained, "/Root/High"));
    // Cancellation preserves the published revision and yields a useful error.
    options.progress_callback = [](const lightusd_document_progress&) { return false; };
    document.SetCallbacks(options);
    const auto revision = api::DocumentSnapshotRevision(document.PublicSnapshot());
    assert(!document.SetVariantSelections(variants));
    assert(!document.GetError().empty());
    assert(api::DocumentSnapshotRevision(document.PublicSnapshot()) == revision);
    document.ClearTransientCallbacks();
  }
  assert(HasPrim(retained, "/Root/High"));
  assert(retainedPreview && lightusd_stage_root_prim_count(retainedPreview.get()) > 0);
  std::filesystem::remove_all(directory);
}
