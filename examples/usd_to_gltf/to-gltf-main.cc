// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include "next/reader/usdz-reader.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/gltf-export.hh"
#include "minijson.hh"
#include <fstream>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
  namespace usd = lightusd::next;
  namespace render = lightusd::tydra::next;
  bool strict = false;
  std::string input, output, report;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--strict") strict = true;
    else if (arg == "--report" && i + 1 < argc) report = argv[++i];
    else if (!arg.empty() && arg[0] != '-' && input.empty()) input = arg;
    else if (!arg.empty() && arg[0] != '-' && output.empty()) output = arg;
    else { std::cerr << "Usage: usd_to_gltf INPUT OUTPUT.glb [--strict] [--report losses.json]\n"; return 2; }
  }
  if (input.empty() || output.empty() || output == input || report == input || report == output) {
    std::cerr << "Supply distinct input, output and optional report paths\n"; return 2;
  }
  usd::StageSession session;
  usd::StageSessionOptions options;
  options.resolver.enable_suffix_fallback = false;
  options.load.limits.max_resident_bytes = size_t(1) << 30;
  const auto opened = session.OpenFile(input, options);
  if (!opened) { std::cerr << opened.error << '\n'; return 1; }
  usd::AssetResolver resolver;
  resolver.SetConfig(options.resolver);
  render::ConverterConfig config;
  config.asset_resolver = &resolver;
  config.asset_base_dir = std::filesystem::path(resolver.Resolve(input).resolved_path).parent_path().string();
  config.material.load_textures = false;
  render::RenderSceneConverter converter(config);
  auto converted = converter.Convert(*session.GetSnapshot());
  if (!converted.success) { std::cerr << converted.error << '\n'; return 1; }
  std::string root_anchor = resolver.Resolve(input).resolved_path;
  if (std::filesystem::path(root_anchor).extension() == ".usdz") {
    usd::USDZReader package;
    usd::USDZReadOptions read;
    read.max_archive_size = options.load.limits.max_resident_bytes;
    if (package.OpenFile(root_anchor, read) && package.FindRootLayer() >= 0)
      root_anchor += "[" + package.EntryName(size_t(package.FindRootLayer())) + "]";
  }
  for (auto& image : converted.scene.images) {
    const auto asset = resolver.Resolve(image.resolved_path, root_anchor);
    if (asset.exists) image.resolved_path = asset.resolved_path;
  }
  render::GltfExportOptions export_options;
  export_options.resolver = &resolver;
  export_options.fail_on_loss = strict;
  auto result = render::ExportGLB(converted.scene, export_options);
  for (const auto& warning : converted.warnings) result.losses.push_back(warning);
  if (strict && !result.losses.empty()) { result.success = false; result.error = "strict export refuses conversion losses"; }
  for (const auto& loss : result.losses) std::cerr << "loss: " << loss << '\n';
  if (!report.empty()) {
    auto losses = lightusd::minijson::Value::array();
    for (const auto& loss : result.losses) losses.push_back(loss);
    lightusd::minijson::Value json{{"schemaVersion", 1}, {"success", result.success}, {"error", result.error}, {"losses", losses}};
    std::ofstream out(report); out << json.dump(2) << '\n'; out.close();
    if (!out) { std::cerr << "Cannot write loss report\n"; return 1; }
  }
  if (!result.success) { std::cerr << result.error << '\n'; return 1; }
  std::ofstream out(output, std::ios::binary);
  out.write(reinterpret_cast<const char*>(result.glb.data()), std::streamsize(result.glb.size()));
  out.close();
  if (!out) { std::cerr << "Cannot write GLB\n"; return 1; }
  return 0;
}
