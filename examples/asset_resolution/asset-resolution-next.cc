// SPDX-License-Identifier: Apache-2.0
// Next-core counterpart to the legacy asset-resolution example. Virtual
// identifiers are resolved and read through the next AssetResolver API.
#include "next/pcp/layer-registry.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/writer/usda-writer.hh"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  const std::string bora = R"USD(#usda 1.0

def "bora" {
    float myval = 3.1
}
)USD";
  const std::string dora = R"USD(#usda 1.0

def "dora" {
    float myval = 5.1
}
)USD";
  const std::string requested = argc > 1 ? argv[1] : "bora.usda";

  lightusd::next::AssetResolver resolver;
  auto register_text = [&resolver](const std::string& name,
                                   const std::string& text) {
    const auto* begin = reinterpret_cast<const uint8_t*>(text.data());
    resolver.RegisterMemoryAsset(
        name, std::vector<uint8_t>(begin, begin + text.size()));
  };
  register_text("bora.usda", bora);
  register_text("dora.usda", dora);

  const auto resolved = resolver.Resolve(requested);
  if (!resolved.exists) {
    std::cerr << "Failed to resolve asset: " << requested << "\n";
    return 1;
  }
  std::vector<uint8_t> bytes;
  std::string error;
  if (!resolver.ReadAsset(resolved.resolved_path, &bytes, &error)) {
    std::cerr << "Failed to read asset: " << error << "\n";
    return 1;
  }
  std::cout << "Read asset: " << resolved.resolved_path << " ("
            << bytes.size() << " bytes)\n";

  std::string warning;
  std::shared_ptr<lightusd::next::Layer> layer =
      lightusd::next::pcp::LoadLayerFromMemoryOwned(
          resolved.resolved_path,
          std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
          &warning, &error);
  if (!layer) {
    std::cerr << "Failed to parse asset: " << error << "\n";
    return 1;
  }
  if (!warning.empty()) std::cerr << "WARN: " << warning;
  const std::string output = lightusd::next::WriteLayerToString(*layer);
  if (output.empty()) {
    std::cerr << "Failed to print layer as USDA\n";
    return 1;
  }
  std::cout << output;
  return 0;
}
