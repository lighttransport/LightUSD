// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <fstream>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "next/crate/crate-writer.hh"
#include "next/layer/asset-anchor.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "scene-preview-internal.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
using json = nlohmann::json;
// Earlier indices could retain instances excluded by inactiveIds metadata.
constexpr int kSourceGeometryVersion = 2;
bool Build::source_save_catalog(usd::Layer& catalog) {
  AppendPreviewExtras(*this, catalog);
  // Iterate concrete records: a Stage traversal can include a pseudo-root
  // which has no corresponding PrimSpec in a catalog with authored extras.
  for (size_t i = 0; i < catalog.prims().size(); ++i) {
    auto* spec = catalog.prim_mutable(static_cast<uint32_t>(i));
    usd::UsdPrim prim(spec, &catalog, static_cast<uint32_t>(i));
    std::map<std::string, std::string> paths;
    const std::string anchor = usd::AssetAnchorPath(spec->asset_anchor_id());
    for (const auto& name : prim.GetPropertyNames()) {
      const auto* value = prim.GetValueAtTime(name, opts.time_code);
      if (!value || !value->as_asset_path()) continue;
      const std::string path = value->as_asset_path();
      if (!path.empty() && !fs::path(path).is_absolute() && !anchor.empty())
        paths[path] = usd::ResolveAssetAnchorPath(path, spec->asset_anchor_id());
    }
    spec->remap_asset_paths(paths);
  }
  usd::CrateWriteOptions options;
  options.num_threads = 1;
  const auto output = usd::CrateWriter(options).WriteLayerToFile(
      (source_directory / "catalog.usdc").string().c_str(), catalog);
  if (!output.success) result.error = output.error;
  return output.success;
}
bool Build::source_save_index() {
  json bounds = json::array();
  for (const auto& item : source_bounds)
    bounds.push_back({item.first, item.second});
  std::ofstream file(source_directory / "geometry.json");
  file << json{{"version", kSourceGeometryVersion},
               {"triangle_bytes", sizeof(Sample) * 3 + sizeof(float) * 6 + 8},
               {"materials", source_material_paths},
               {"bounds", bounds}}
              .dump();
  if (!file) result.error = "cannot write source geometry index";
  return bool(file);
}
bool Build::source_load_index(const fs::path& directory) {
  std::error_code ec;
  if (fs::file_size(directory / "geometry.json", ec) > (uint64_t(64) << 20) ||
      ec) {
    result.error = "invalid source geometry index";
    return false;
  }
  std::ifstream file(directory / "geometry.json");
  auto j = json::parse(file, nullptr, false);
  if (!j.is_object() || !j.contains("version") ||
      !j["version"].is_number_integer() || j["version"] != kSourceGeometryVersion ||
      !j.contains("materials") || !j["materials"].is_array() ||
      !j.contains("bounds") || !j["bounds"].is_array()) {
    result.error = "invalid source index metadata";
    return false;
  }
  for (const auto& material : j["materials"]) {
    if (!material.is_string()) {
      result.error = "invalid indexed material";
      return false;
    }
    source_material_paths.push_back(material.get<std::string>());
  }
  for (const auto& row : j["bounds"]) {
    if (!row.is_array() || row.size() != 2 || !row[0].is_number_unsigned() ||
        !row[1].is_array() || row[1].size() != 6) {
      result.error = "invalid indexed bounds";
      return false;
    }
    std::array<double, 6> bounds;
    for (size_t k = 0; k < 6; ++k) {
      if (!row[1][k].is_number() || !std::isfinite(row[1][k].get<double>())) {
        result.error = "invalid indexed bound";
        return false;
      }
      bounds[k] = row[1][k].get<double>();
    }
    source_bounds.emplace(row[0].get<uint64_t>(), bounds);
  }
  usd::Stage stage;
  usd::AssetResolver resolver;
  usd::pcp::CompositionOptions read;
  read.num_threads = 1;
  if (!usd::pcp::ComposeStageFromFile((directory / "catalog.usdc").string(),
                                      resolver, &stage, read, &result.warning,
                                      &result.error))
    return false;
  usd::pcp::ComposedBatch batch;
  batch.layer = stage.ReleaseRootLayer();
  if (!plan(batch) || !finish_plan()) return false;
  source_directory = directory;
  return check("source-index", source_bounds.size()) &&
         source_replay(0, Identity().data());
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
