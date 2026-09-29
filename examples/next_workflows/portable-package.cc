// SPDX-License-Identifier: Apache-2.0
#include "lightusd-cpp.hh"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: portable_asset_package NEW_OUTPUT_DIRECTORY\n";
    return 2;
  }
  // The directory must be new so the example cannot overwrite user assets.
  std::error_code ec;
  const std::filesystem::path dir(argv[1]);
  if (!std::filesystem::create_directory(dir, ec)) return 2;

  const std::string source = "#usda 1.0\n(defaultPrim = \"World\" upAxis = \"Y\" metersPerUnit = 1)\ndef Xform \"World\" { asset info:readme = @readme.txt@ }\n";
  const std::string note = "A package-local dependency.\n";
  lightusd::api::AssetResolver resolver;
  if (lightusd::api::CreateAssetResolver(&resolver) != LIGHTUSD_OK ||
      lightusd::api::RegisterMemoryAsset(
          resolver, "demo:root.usda",
          reinterpret_cast<const uint8_t*>(source.data()), source.size()) !=
          LIGHTUSD_OK ||
      lightusd::api::SetAssetAlias(resolver, "readme.txt",
                                   "demo:readme.txt") != LIGHTUSD_OK ||
      lightusd::api::RegisterMemoryAsset(
          resolver, "demo:readme.txt",
          reinterpret_cast<const uint8_t*>(note.data()), note.size()) !=
          LIGHTUSD_OK) {
    std::cerr << lightusd::api::LastError() << '\n';
    return 1;
  }
  lightusd::api::String inventory;
  bool complete = false;
  if (lightusd::api::DependencyReportJSON(resolver, "demo:root.usda",
                                          &inventory, &complete) !=
      LIGHTUSD_OK) {
    std::cerr << lightusd::api::LastError() << '\n';
    return 1;
  }
  if (!complete) {
    const lightusd_sv text = lightusd::api::StringView(inventory);
    std::cerr << std::string_view(text.data, text.len) << '\n';
    return 1;
  }

  lightusd::api::Stage stage;
  if (stage.load_from_memory(reinterpret_cast<const uint8_t*>(source.data()),
                             source.size()) != LIGHTUSD_OK) {
    std::cerr << lightusd::api::LastError() << '\n';
    return 1;
  }
  size_t errors = 0, warnings = 0;
  if (stage.validate_core(&errors, &warnings) != LIGHTUSD_OK || errors != 0)
    return 1;
  lightusd::api::String crate;
  if (stage.export_usdc(&crate) != LIGHTUSD_OK) return 1;
  const lightusd_sv crate_bytes = lightusd::api::StringView(crate);
  if (crate_bytes.len < 8 || std::memcmp(crate_bytes.data, "PXR-USDC", 8) != 0)
    return 1;

  lightusd::api::String asset;
  if (lightusd::api::ReadResolvedAsset(resolver, "demo:readme.txt", &asset) !=
      LIGHTUSD_OK) return 1;
  const lightusd_sv asset_bytes = lightusd::api::StringView(asset);
  const char* const asset_names[] = {"readme.txt"};
  const uint8_t* const asset_data[] = {
      reinterpret_cast<const uint8_t*>(asset_bytes.data)};
  const size_t asset_sizes[] = {asset_bytes.len};
  const auto original = dir / "asset.usdz";
  const auto moved = dir / "moved.usdz";
  if (stage.save_usdz_with_assets(original.string().c_str(), asset_names,
                                  asset_data, asset_sizes, 1) != LIGHTUSD_OK) {
    std::cerr << lightusd::api::LastError() << '\n';
    return 1;
  }
  std::filesystem::rename(original, moved, ec);
  if (ec || stage.load(moved.string().c_str()) != LIGHTUSD_OK) return 1;

  lightusd::api::AssetResolver file_resolver;
  if (lightusd::api::CreateAssetResolver(&file_resolver) != LIGHTUSD_OK)
    return 1;
  lightusd::api::String relocated;
  if (lightusd::api::DependencyReportJSON(file_resolver,
                                          moved.string().c_str(), &relocated,
                                          &complete) != LIGHTUSD_OK) return 1;
  const lightusd_sv report = lightusd::api::StringView(relocated);
  std::cout << std::string_view(report.data, report.len) << '\n';
  return complete ? 0 : 1;
}
