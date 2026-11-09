// SPDX-License-Identifier: Apache-2.0
// Cached previews must keep authored photometric profiles portable.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../../src/pcp/cache.hh"
#include "resolver/asset-resolver.hh"
#include "stage/stage.hh"
#include "tydra/scene-preview-photometry.hh"
#include "tydra/scene-preview.hh"

namespace td = lightusd::tydra::next;
namespace fs = std::filesystem;

namespace {
bool Check(bool ok, const std::string& what) {
  if (!ok) std::cerr << "FAIL: " << what << "\n";
  return ok;
}
const char kConstantIes[] =
    "IESNA:LM-63-2002\r\n[TEST] Synthetic constant\r\nTILT=NONE\r\n"
    "1 -1 2 2 1 1 2 0 0 0\r\n.5 1 0\r\n0 180\r\n0\r\n4 4\r\n";
}  // namespace

int main(int argc, char** argv) {
  const fs::path folder =
      argc > 1 ? fs::path(argv[1])
               : fs::temp_directory_path() / "preview-light-assets-test";
  const fs::path source = folder / "source";
  const fs::path cache = folder / "cache";
  fs::create_directories(source);
  {
    std::ofstream file(source / "lamp.ies", std::ios::binary);
    file << kConstantIes;
  }
  const fs::path scene = folder / "scene.usda";
  {
    std::ofstream file(scene);
    file << R"usd(#usda 1.0
 def Cube "Cube" {
   double size = 1
 }
 def SphereLight "Key" {
   float inputs:intensity = 2
   asset inputs:shaping:ies:file = @source/lamp.ies@
 }
)usd";
  }

  lightusd::next::AssetResolver resolver;
  td::ScenePreviewOptions options;
  options.compute_backend = td::PreviewComputeBackend::Cpu;
  options.kind = td::ScenePreviewKind::Gaussian;
  options.resolution = 8;
  options.cache_directory = cache.string();
  const auto result = td::ConvertScenePreview(scene.string(), resolver, options);
  if (!Check(result.success, "preview conversion: " + result.error)) return 1;
  if (!Check(result.warning.find("IES") == std::string::npos,
             "authored IES profile did not warn: " + result.warning))
    return 1;

  lightusd::next::Stage cached;
  lightusd::next::pcp::CompositionOptions compose;
  compose.num_threads = 1;
  std::string warning, error;
  if (!Check(lightusd::next::pcp::ComposeStageFromFile(
                 result.usd_path, resolver, &cached, compose, &warning, &error),
             "compose cache: " + error))
    return 1;

  std::string profile_path;
  cached.Traverse([&](const lightusd::next::UsdPrim& prim) {
    if (prim.GetName() != "Key") return true;
    if (const auto* value = prim.GetPropertyValue("inputs:shaping:ies:file"))
      profile_path = value->as_asset_path();
    return true;
  });
  if (!Check(!profile_path.empty(), "cached light keeps a profile path"))
    return 1;
  if (!Check(!fs::path(profile_path).is_absolute(),
             "cached profile path is relative: " + profile_path))
    return 1;
  if (!Check(profile_path.rfind("light-", 0) == 0 &&
                 profile_path.size() > 5 &&
                 profile_path.compare(profile_path.size() - 4, 4, ".ies") == 0,
             "cached profile has a baked name: " + profile_path))
    return 1;
  const fs::path baked = fs::path(result.usd_path).parent_path() / profile_path;
  if (!Check(fs::exists(baked), "baked profile exists: " + baked.string()))
    return 1;
  td::PreviewIesProfile profile;
  if (!Check(td::LoadPreviewIes(baked.string(), &profile, &error),
             "baked profile parses: " + error))
    return 1;

  // The cache is self-contained: removing the authored asset must not change
  // the composed profile path.
  fs::remove(source / "lamp.ies");
  lightusd::next::Stage reopened;
  if (!Check(lightusd::next::pcp::ComposeStageFromFile(
                 result.usd_path, resolver, &reopened, compose, &warning, &error),
             "cache opens without the source asset: " + error))
    return 1;

  // Link relationships stay scene-relative in the cache so the volume loader
  // can compile light/shadow masks without the original asset tree.
  const fs::path linked_scene = folder / "linked.usda";
  {
    std::ofstream file(linked_scene);
    file << R"usd(#usda 1.0
 def Cube "Cube" {
   double size = 1
 }
 def Xform "Target" {}
 def SphereLight "Key" {
   float inputs:intensity = 1
   rel light:link = [</Target>]
   rel shadow:link = [</Target>]
 }
)usd";
  }
  const fs::path linked_cache = folder / "linked-cache";
  auto linked_options = options;
  linked_options.cache_directory = linked_cache.string();
  const auto linked = td::ConvertScenePreview(linked_scene.string(), resolver,
                                              linked_options);
  if (!Check(linked.success, "linked preview conversion: " + linked.error))
    return 1;
  lightusd::next::Stage linked_stage;
  if (!Check(lightusd::next::pcp::ComposeStageFromFile(
                 linked.usd_path, resolver, &linked_stage, compose, &warning,
                 &error),
             "compose linked cache: " + error))
    return 1;
  bool light_link = false, shadow_link = false;
  linked_stage.Traverse([&](const lightusd::next::UsdPrim& prim) {
    if (prim.GetName() != "Key") return true;
    if (const auto* targets = prim.GetRelationship("light:link"))
      light_link = !targets->empty() && targets->front().str() == "/Target";
    if (const auto* targets = prim.GetRelationship("shadow:link"))
      shadow_link = !targets->empty() && targets->front().str() == "/Target";
    return true;
  });
  if (!Check(light_link && shadow_link,
             "cached light keeps scene-relative link relationships"))
    return 1;

  std::cout << "preview light assets: portable IES sidecar and link "
               "relationships passed\n";
  std::error_code ec;
  fs::remove_all(folder, ec);
  return 0;
}
