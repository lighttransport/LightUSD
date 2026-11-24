// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "composition/composition.hh"
#include "layer/asset-anchor.hh"
#include "load-usd.hh"
#include "pipeline/flatten.hh"
#include "writer/usda-writer.hh"

namespace {
constexpr std::array<const char*, 6> names = {
    "subLayers", "inherits", "variantSets", "references", "payload", "specializes"};
constexpr std::array<const char*, 6> aliases = {"l", "i", "v", "r", "p", "s"};

void Help() {
  std::cout << "Usage: progressive_composition [options] input.usda/usdc/usdz\n"
               "  --noflatten          Print the authored root layer\n"
               "  -l, --loadOnly       Validate the root layer without composition\n"
               "  --composition=LIST  Select comma-separated LIVRPS arc types\n"
               "                       subLayers,inherits,variantSets,references,payload,specializes\n"
               "                       (short names: l,i,v,r,p,s; default: all)\n"
               "  --extract-variants  Print authored variant names as JSON\n"
               "  --relative          Print mesh diagnostic paths without leading slash\n"
               "  -h, --help          Show this help\n";
}

std::string Quoted(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else out << c;
  }
  out << '"';
  return out.str();
}

void Variants(const lightusd::Layer& layer) {
  std::cout << "# authored variants\n[";
  bool first = true;
  for (const auto& prim : layer.prims()) {
    for (const auto& set : prim.meta().variantSets()) {
      if (!first) std::cout << ',';
      first = false;
      std::cout << "{\"prim\":" << Quoted(prim.path().str())
                << ",\"set\":" << Quoted(set.name) << ",\"variants\":[";
      for (size_t i = 0; i < set.variants.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << Quoted(set.variants[i].name);
      }
      std::cout << "]}";
    }
  }
  std::cout << "]\n";
}

lightusd::CompositionOptions Options(const std::array<bool, 6>& enabled) {
  lightusd::CompositionOptions options;
  options.resolve_sublayers = enabled[0];
  options.resolve_inherits = enabled[1];
  options.resolve_variants = enabled[2];
  options.resolve_references = enabled[3];
  options.load_payloads = enabled[4];
  options.resolve_specializes = enabled[5];
  options.preserve_unresolved_arcs = true;
  return options;
}
}  // namespace

int main(int argc, char** argv) {
  std::array<bool, 6> selected{{true, true, true, true, true, true}};
  bool flatten = true, load_only = false, relative = false, variants = false;
  std::string input;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "-h" || arg == "--help") { Help(); return EXIT_SUCCESS; }
    if (arg == "--noflatten" || arg == "-f") flatten = false;
    else if (arg == "--loadOnly" || arg == "-l") load_only = true;
    else if (arg == "--relative") relative = true;
    else if (arg == "--extract-variants") variants = true;
    else if (arg.compare(0, 14, "--composition=") == 0) {
      selected.fill(false);
      std::string list = arg.substr(14);
      if (list.empty() || list.back() == ',') { std::cerr << "Empty composition selection\n"; return EXIT_FAILURE; }
      std::istringstream values(list);
      for (std::string value; std::getline(values, value, ',');) {
        size_t index = 0;
        for (; index < names.size(); ++index)
          if (value == names[index] || value == aliases[index]) break;
        if (index == names.size()) { std::cerr << "Unknown composition type: " << value << '\n'; return EXIT_FAILURE; }
        selected[index] = true;
      }
    } else if (arg.empty() || arg[0] == '-' || !input.empty()) {
      std::cerr << "Unexpected argument: " << arg << '\n'; return EXIT_FAILURE;
    } else input = arg;
  }
  if (input.empty()) { Help(); return EXIT_FAILURE; }
  lightusd::Stage stage;
  std::string warn, error;
  if (!lightusd::LoadUSD(input, &stage, &warn, &error)) {
    std::cerr << error << '\n'; return EXIT_FAILURE;
  }
  if (!warn.empty()) std::cerr << warn << '\n';
  if (load_only) return EXIT_SUCCESS;
  const auto& root = *stage.GetRootLayer();
  if (variants) Variants(root);
  if (!flatten) { std::cout << lightusd::WriteLayerToString(root); return EXIT_SUCCESS; }

  // Recompose from the authored root for every cumulative pass. Reusing a
  // partially flattened layer would change strength ordering across arc types.
  std::error_code ec;
  std::string anchor = std::filesystem::absolute(input, ec).generic_string();
  if (ec) { std::cerr << ec.message() << '\n'; return EXIT_FAILURE; }
  const auto& package_anchor = lightusd::AssetAnchorPath(root.source_asset_anchor_id());
  if (lightusd::AssetResolver::IsPackagePath(package_anchor)) anchor = package_anchor;
  lightusd::AssetResolver resolver;
  resolver.SetWorkingDirectory(lightusd::AssetResolver::GetDirectory(anchor));
  std::array<bool, 6> enabled{};
  std::unique_ptr<lightusd::Layer> composed;
  std::cout << "# input\n" << lightusd::WriteLayerToString(root);
  for (size_t pass = 0; pass < selected.size(); ++pass) {
    if (!selected[pass]) continue;
    enabled[pass] = true;
    lightusd::Compositor compositor(&resolver);
    compositor.SetOptions(Options(enabled));
    compositor.SetLayerLoader(lightusd::pipeline::MakeFileSystemLayerLoader());
    composed = compositor.Compose(root, anchor);
    if (!composed || !compositor.GetErrors().empty()) {
      for (const auto& diagnostic : compositor.GetErrors())
        std::cerr << diagnostic.message << " (" << diagnostic.prim_path << ")\n";
      return EXIT_FAILURE;
    }
    std::cout << "# composed " << names[pass] << '\n' << lightusd::WriteLayerToString(*composed);
  }
  if (!composed) return EXIT_FAILURE;
  std::cout << "# final\n" << lightusd::WriteLayerToString(*composed);
  std::vector<std::string> meshes;
  for (const auto& prim : composed->prims())
    if (prim.type_name() == "Mesh" && composed->prim_at_path(prim.path().str()))
      meshes.push_back(prim.path().str());
  std::sort(meshes.begin(), meshes.end());
  for (const auto& path : meshes) std::cout << "Prim : " << (relative ? path.substr(1) : path) << '\n';
  return EXIT_SUCCESS;
}
