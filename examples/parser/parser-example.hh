// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "composition/composition.hh"
#include "load-usd.hh"
#include "pipeline/flatten.hh"
#include "writer/usda-writer.hh"

namespace lightusd_examples {
inline int ParserExampleMain(int argc, char** argv, bool binary) {
  const char* program = binary ? "usdc_parser" : "usda_parser";
  const auto help = [=] {
    std::cout << "Usage: " << program << " [--flatten] [--composition=LIST] input."
              << (binary ? "usdc" : "usda") << '\n'
              << "  --flatten          Resolve composition arcs and export USDA\n"
                 "  --composition=LIST Select subLayers,inherits,variantSets,references,payload,specializes\n"
                 "                     (aliases: l,i,v,r,p,s; requires --flatten)\n"
                 "  -h, --help         Show this help\n";
  };
  lightusd::CompositionOptions options;
  options.preserve_unresolved_arcs = true;
  bool flatten = false, selection = false;
  std::string input;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") { help(); return EXIT_SUCCESS; }
    if (arg == "--flatten") flatten = true;
    else if (arg.compare(0, 14, "--composition=") == 0) {
      selection = true;
      std::array<bool, 6> enabled{};
      const std::string list = arg.substr(14);
      if (list.empty() || list.back() == ',') {
        std::cerr << "Empty composition selection\n"; return EXIT_FAILURE;
      }
      std::istringstream values(list);
      for (std::string value; std::getline(values, value, ',');) {
        constexpr std::array<const char*, 6> names = {
          "subLayers", "inherits", "variantSets", "references", "payload", "specializes"};
        constexpr std::array<const char*, 6> aliases = {"l", "i", "v", "r", "p", "s"};
        size_t index = 0;
        for (; index < names.size(); ++index)
          if (value == names[index] || value == aliases[index]) break;
        if (index == names.size()) {
          std::cerr << "Unknown composition type: " << value << '\n'; return EXIT_FAILURE;
        }
        enabled[index] = true;
      }
      options.resolve_sublayers = enabled[0];
      options.resolve_inherits = enabled[1];
      options.resolve_variants = enabled[2];
      options.resolve_references = enabled[3];
      options.load_payloads = enabled[4];
      options.resolve_specializes = enabled[5];
    } else if (arg.empty() || arg[0] == '-' || !input.empty()) {
      std::cerr << "Unexpected argument: " << arg << '\n'; return EXIT_FAILURE;
    } else input = arg;
  }
  if (input.empty()) { help(); return EXIT_FAILURE; }
  if (selection && !flatten) {
    std::cerr << "--composition requires --flatten\n"; return EXIT_FAILURE;
  }
  lightusd::Stage stage;
  std::string warning, error;
  const bool loaded = binary ? lightusd::LoadUSDC(input, &stage, &warning, &error)
                             : lightusd::LoadUSDA(input, &stage, &warning, &error);
  if (!warning.empty()) std::cerr << warning << '\n';
  if (!loaded) { std::cerr << error << '\n'; return EXIT_FAILURE; }
  if (!flatten) {
    std::cout << lightusd::WriteLayerToString(*stage.GetRootLayer());
    return EXIT_SUCCESS;
  }
  std::error_code ec;
  const std::string anchor = std::filesystem::absolute(input, ec).generic_string();
  if (ec) { std::cerr << ec.message() << '\n'; return EXIT_FAILURE; }
  lightusd::AssetResolver resolver;
  resolver.SetWorkingDirectory(lightusd::AssetResolver::GetDirectory(anchor));
  lightusd::Compositor compositor(&resolver);
  compositor.SetOptions(options);
  compositor.SetLayerLoader(lightusd::pipeline::MakeFileSystemLayerLoader());
  auto result = compositor.Compose(*stage.GetRootLayer(), anchor);
  if (!result || !compositor.GetErrors().empty()) {
    for (const auto& diagnostic : compositor.GetErrors())
      std::cerr << diagnostic.message << " (" << diagnostic.prim_path << ")\n";
    return EXIT_FAILURE;
  }
  std::cout << lightusd::WriteLayerToString(*result);
  return EXIT_SUCCESS;
}
}  // namespace lightusd_examples
