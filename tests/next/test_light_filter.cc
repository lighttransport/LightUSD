// SPDX-License-Identifier: Apache-2.0
// Portable light-filter parsing, registry and fold-in behaviour.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tydra/light-filter.hh"
#include "../../src/pcp/cache.hh"
#include "resolver/asset-resolver.hh"
#include "stage/stage.hh"
#include "tydra/render-converter.hh"
#include "tydra/scene-preview.hh"

namespace td = lightusd::tydra::next;
namespace fs = std::filesystem;

namespace {
bool Check(bool ok, const std::string& what) {
  if (!ok) std::cerr << "FAIL: " << what << "\n";
  return ok;
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
}  // namespace

int main(int argc, char** argv) {
  const fs::path folder =
      argc > 1 ? fs::path(argv[1])
               : fs::temp_directory_path() / "light-filter-test";
  fs::create_directories(folder);
  const fs::path fixture = folder / "filters.usda";
  {
    std::ofstream file(fixture);
    file << R"usd(#usda 1.0
 def Scope "World" {
   def Cube "Cube" {
     double size = 1
   }
   def SphereLight "Key" {
     float inputs:intensity = 2
     color3f inputs:color = (1, 1, 1)
     rel light:filters = [</World/Half>, </World/Tint>]
   }
   def LightFilter "Half" {
     uniform token lightusd:filter:kind = "intensity"
     float lightusd:filter:intensity = 0.5
   }
   def LightFilter "Tint" {
     uniform token lightusd:filter:kind = "color"
     color3f lightusd:filter:color = (0.25, 0.5, 1)
   }
   def LightFilter "Mystery" {
     uniform token lightusd:filter:kind = "does-not-exist"
   }
   def SphereLight "Unknown" {
     float inputs:intensity = 1
     rel light:filters = [</World/Mystery>]
   }
   def LightFilter "NoKind" {
   }
   def SphereLight "Missing" {
     rel light:filters = [</World/Absent>]
   }
 }
)usd";
  }

  lightusd::next::AssetResolver resolver;
  lightusd::next::Stage stage;
  lightusd::next::pcp::CompositionOptions compose;
  compose.num_threads = 1;
  std::string warning, error;
  if (!lightusd::next::pcp::ComposeStageFromFile(fixture.string(), resolver,
                                                 &stage, compose, &warning,
                                                 &error)) {
    std::cerr << "compose failed: " << error << "\n";
    return 1;
  }

  td::RegisterBuiltinLightFilters();
  const auto kinds = td::LightFilterKinds();
  if (!Check(std::find(kinds.begin(), kinds.end(), "intensity") != kinds.end() &&
                 std::find(kinds.begin(), kinds.end(), "color") != kinds.end(),
             "builtin kinds registered"))
    return 1;

  td::RenderSceneConverter converter;
  td::RenderLight key;
  const auto key_prim = stage.GetPrimAtPath("/World/Key");
  if (!Check(key_prim && converter.ConvertLight(stage, key_prim, &key),
             "convert filtered light"))
    return 1;
  if (key.filters.size() != 2) {
    std::cerr << "diag: filters=" << key.filters.size()
              << " targets=" << key.filter_targets.size() << "\n";
    for (const auto& target : key.filter_targets)
      std::cerr << "diag: target " << target << "\n";
    for (const auto& filter : key.filters)
      std::cerr << "diag: parsed " << filter.kind << " " << filter.prim_path
                << "\n";
    std::cerr << "diag: warning=" << warning << " error=" << error << "\n";
  }
  if (!Check(key.filters.size() == 2, "two filters parsed")) return 1;
  if (!Check(key.filters[0].kind == "intensity" &&
                 key.filters[1].kind == "color",
             "filter order and kinds") ||
      !Check(Near(key.filters[0].intensity, 0.5f), "intensity parameter") ||
      !Check(Near(key.filters[1].color[1], 0.5f), "color parameter") ||
      !Check(Near(key.filters[1].color[2], 1.0f), "color parameter channel"))
    return 1;
  // Receiver-independent filters fold into the render-ready light.
  if (!Check(Near(key.intensity, 1.0f), "folded intensity") ||
      !Check(Near(key.color.x, 0.25f) && Near(key.color.y, 0.5f) &&
                 Near(key.color.z, 1.0f),
             "folded color"))
    return 1;

  // Unknown kind and missing target warn but never change the light.
  td::RenderLight unknown;
  const auto unknown_prim = stage.GetPrimAtPath("/World/Unknown");
  if (!Check(unknown_prim && converter.ConvertLight(stage, unknown_prim, &unknown),
             "convert unknown-filter light"))
    return 1;
  if (!Check(unknown.filters.empty(), "unknown kind rejected") ||
      !Check(Near(unknown.intensity, 1.0f), "unknown filter leaves intensity") ||
      !Check(Near(unknown.color.x, 1.0f), "unknown filter leaves color"))
    return 1;
  td::RenderSceneConverter warning_converter;
  const auto converted = warning_converter.Convert(stage);
  bool warned_unknown = false;
  for (const auto& message : converted.warnings)
    if (message.find("does-not-exist") != std::string::npos)
      warned_unknown = true;
  if (!Check(warned_unknown, "unknown filter warning")) return 1;

  td::RenderLight missing;
  const auto missing_prim = stage.GetPrimAtPath("/World/Missing");
  if (!Check(missing_prim && converter.ConvertLight(stage, missing_prim, &missing),
             "convert missing-filter light"))
    return 1;
  if (!Check(missing.filters.empty() && Near(missing.intensity, 1.0f),
             "missing target leaves light"))
    return 1;

  // A custom kind can be registered without core changes.
  if (!Check(td::RegisterLightFilter(
                 "gain",
                 [](const lightusd::next::Stage&, const lightusd::next::UsdPrim&,
                    double, td::LightFilter* out, std::string*) {
                   out->intensity = 0.25f;
                   return true;
                 }),
             "register custom kind") ||
      !Check(td::HasLightFilter("gain"), "custom kind visible") ||
      !Check(!td::RegisterLightFilter("gain", td::LightFilterParser{}),
             "duplicate registration rejected"))
    return 1;
  if (!Check(td::UnregisterLightFilter("gain") && !td::HasLightFilter("gain"),
             "unregister custom kind"))
    return 1;

  // Cached previews fold the same filters into the light they keep.
  td::ScenePreviewOptions options;
  options.compute_backend = td::PreviewComputeBackend::Cpu;
  options.kind = td::ScenePreviewKind::Gaussian;
  options.resolution = 8;
  options.cache_directory = (folder / "cache").string();
  const auto result =
      td::ConvertScenePreview(fixture.string(), resolver, options);
  if (!Check(result.success, result.error)) return 1;
  lightusd::next::Stage cached;
  if (!Check(lightusd::next::pcp::ComposeStageFromFile(
                 result.usd_path, resolver, &cached, compose, &warning, &error),
             "compose cached preview"))
    return 1;
  bool found_light = false;
  bool cached_fold = false;
  cached.Traverse([&](const lightusd::next::UsdPrim& prim) {
    if (prim.GetPath().str().rfind("/Key") != prim.GetPath().str().size() - 4)
      return true;
    found_light = true;
    float intensity = 0.0f;
    if (const auto* value = prim.GetPropertyValue("inputs:intensity"))
      if (const float* authored = value->as_float()) intensity = *authored;
    cached_fold = Near(intensity, 1.0f);
    return true;
  });
  if (!Check(found_light, "cached light kept") ||
      !Check(cached_fold, "cached light carries folded intensity"))
    return 1;

  std::cout << "light filters: registry, conversion fold and cache fold passed\n";
  std::error_code ec;
  fs::remove_all(folder, ec);
  return 0;
}
