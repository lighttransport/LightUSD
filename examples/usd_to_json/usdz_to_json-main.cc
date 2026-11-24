// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "core/minijson.hh"
#include "load-usd.hh"
#include "reader/usdz-reader.hh"
#include "writer/layer-json.hh"
#include "json-assets.hh"
#include "runtime-json.hh"

namespace {
void Usage(const char* program) {
  std::cout << "Usage: " << program << " [--layer-json] <input.usdz> [output_prefix]\n"
               "Writes <prefix>_usd.json (runtime scene; --layer-json for authored layer) and <prefix>_assets.json\n"
               "(archive entries with filename, byte size and base64 data).\n";
}
}  // namespace
int main(int argc, char** argv) {
  const char* program = argv[0];
  bool authored = false;
  if (argc > 1 && std::string(argv[1]) == "--layer-json") { authored = true; --argc; ++argv; }
  if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    Usage(program); return 0;
  }
  if (argc < 2 || argc > 3) { Usage(program); return 1; }
  const std::filesystem::path input(argv[1]);
  std::string extension = input.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".usdz") { std::cerr << "Expected a USDZ file\n"; return 1; }
  const std::string prefix = argc == 3 ? argv[2] : input.stem().string();
  lightusd::USDZReader archive;
  if (!archive.OpenFile(input.string())) { std::cerr << archive.Error() << '\n'; return 1; }
  const int root_index = archive.FindRootLayer();
  if (root_index < 0) { std::cerr << "USDZ has no valid root layer\n"; return 1; }
  const size_t root = static_cast<size_t>(root_index);
  lightusd::Stage stage;
  std::string warning, error;
  if (!lightusd::LoadUSDFromMemory(archive.EntryData(root), archive.EntrySize(root),
                                  &stage, &warning, &error)) {
    std::cerr << error << '\n'; return 1;
  }
  if (!warning.empty()) std::cerr << warning << '\n';
  auto scene = lightusd::WriteLayerToJSON(*stage.GetRootLayer());
  if (!scene) { std::cerr << scene.error() << '\n'; return 1; }
  if (!authored) scene = lightusd_examples::RuntimeSceneJSON(*stage.GetRootLayer(), scene.value());
  if (!scene) { std::cerr << scene.error() << '\n'; return 1; }
  std::ofstream usd(prefix + "_usd.json", std::ios::binary);
  std::ofstream assets(prefix + "_assets.json", std::ios::binary);
  if (!usd || !assets) { std::cerr << "Unable to create output JSON files\n"; return 1; }
  usd << scene.value();
  usd.close();
  assets << "{\n";
  bool first = true;
  size_t count = 0;
  for (size_t i = 0; i < archive.NumEntries(); ++i) {
    const auto& name = archive.EntryName(i);
    if (!name.empty() && name.back() == '/') continue;
    if (!first) assets << ",\n";
    first = false;
    const auto quoted = lightusd::minijson::Value(name).dump();
    assets << quoted << ":{\"filename\":" << quoted << ",\"size\":"
           << archive.EntrySize(i) << ",\"data\":\"";
    lightusd_examples::Base64(assets, archive.EntryData(i), archive.EntrySize(i));
    assets << "\"}";
    ++count;
  }
  assets << "\n}\n";
  assets.close();
  if (!usd || !assets) { std::cerr << "Unable to write output JSON files\n"; return 1; }
  std::cout << "Main USD file: " << archive.EntryName(root) << '\n'
            << "Total assets: " << count << '\n'
            << "Wrote " << prefix << "_usd.json and " << prefix << "_assets.json\n";
  return 0;
}
