// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include "next/resolver/dependency-report.hh"
#include "next/validation/usd-validation.hh"
#include "next/writer/usdz-writer.hh"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  namespace usd = lightusd::next;
  if (argc != 2) { std::cerr << "Usage: portable_asset_package NEW_OUTPUT_DIRECTORY\n"; return 2; }
  // The directory must be new so the example cannot overwrite user assets.
  std::error_code ec;
  const std::filesystem::path dir(argv[1]);
  if (!std::filesystem::create_directory(dir, ec)) return 2;
  const std::string source = "#usda 1.0\n(defaultPrim = \"World\" upAxis = \"Y\" metersPerUnit = 1)\ndef Xform \"World\" { asset info:readme = @readme.txt@ }\n";
  const std::string note = "A package-local dependency.\n";
  usd::AssetResolver resolver;
  resolver.RegisterMemoryAsset("demo:root.usda", std::vector<uint8_t>(source.begin(), source.end()));
  resolver.SetCustomResolver([](const std::string& asset, const std::string&) {
    return asset == "readme.txt" ? "demo:readme.txt" : std::string();
  });
  resolver.RegisterMemoryAsset("demo:readme.txt", std::vector<uint8_t>(note.begin(), note.end()));
  const auto inventory = usd::CollectDependencies("demo:root.usda", resolver);
  if (!inventory.complete) { std::cerr << usd::DependencyReportToJSON(inventory) << '\n'; return 1; }
  usd::Stage stage;
  std::string warn, err;
  if (!usd::LoadUSDFromMemory(reinterpret_cast<const uint8_t*>(source.data()), source.size(), &stage, &warn, &err)) return 1;
  if (!usd::ValidateLayerAgainstAOUSDCore(*stage.GetRootLayer()).ok()) return 1;
  std::vector<uint8_t> crate, package;
  if (!usd::WriteUSDCToMemory(crate, stage).success) return 1;
  std::map<std::string, std::vector<uint8_t>> assets;
  if (!resolver.ReadAsset("demo:readme.txt", &assets["readme.txt"], &err)) return 1;
  if (!usd::WriteUSDZFromUSDCAndAssetsToMemory(package, crate.data(), crate.size(), assets).success) return 1;
  const auto original = dir / "asset.usdz", moved = dir / "moved.usdz";
  {
    std::ofstream out(original, std::ios::binary);
    out.write(reinterpret_cast<const char*>(package.data()), std::streamsize(package.size()));
    if (!out) return 1;
  }
  std::filesystem::rename(original, moved, ec);
  if (ec || !usd::LoadUSD(moved.string(), &stage, &warn, &err)) return 1;
  usd::AssetResolver file_resolver;
  const auto relocated = usd::CollectDependencies(moved.string(), file_resolver);
  std::cout << usd::DependencyReportToJSON(relocated) << '\n';
  return relocated.complete ? 0 : 1;
}
