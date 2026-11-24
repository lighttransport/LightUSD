// SPDX-License-Identifier: Apache-2.0
// Adapt a custom binary format to core layers before composition. The adapter
// and its byte source belong to the embedding application; no DLL plugins or
// legacy file-format objects are needed.
#include "layer/layer.hh"
#include "load-usd.hh"
#include "pcp/cache.hh"
#include "reader/usda-reader.hh"
#include "resolver/asset-resolver.hh"
#include "writer/usda-writer.hh"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

// The .my wire format is one little-endian IEEE-754 binary32 number.
static std::shared_ptr<lightusd::Layer> DecodeMy(
    const std::vector<uint8_t>& bytes, std::string* error) {
  if (bytes.size() != 4) {
    *error = "A .my asset must contain exactly four bytes";
    return {};
  }
  const uint32_t bits = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
      (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
  float value;
  static_assert(sizeof(value) == sizeof(bits), "binary32 required");
  std::memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value)) {
    *error = "A .my asset must contain a finite value";
    return {};
  }
  auto layer = std::make_shared<lightusd::Layer>();
  lightusd::LayerBuilder builder(*layer);
  builder.define_prim("Asset", "Scope");
  builder.add_property("myval", lightusd::Value(value));
  builder.end_prim();
  builder.finalize();
  layer->meta().defaultPrim = "Asset";
  layer->meta().defaultPrim_set = true;
  return layer;
}

static std::vector<uint8_t> EncodeMy(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
}

int main(int argc, char** argv) {
  using namespace lightusd;
  const std::map<std::string, std::vector<uint8_t>> assets = {
      {"bora.my", EncodeMy(3.14f)}, {"dora.my", EncodeMy(6.14f)}};
  AssetResolver resolver;
  for (const auto& asset : assets) resolver.RegisterMemoryAsset(asset.first, asset.second);

  Stage root;
  std::string warning, error;
  if (argc > 1) {
    if (!LoadUSD(argv[1], &root, &warning, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
  } else {
    auto parsed = LoadUSDAFromString(R"USD(#usda 1.0
      def Scope "Root" {
        def "Bora" ( references = @bora.my@</Asset> ) {}
        def "Dora" ( references = @dora.my@</Asset> ) {}
      }
    )USD");
    if (!parsed.success) return 1;
    root = std::move(parsed.stage);
  }
  auto opened = pcp::Cache::Open(resolver, root.ReleaseRootLayer(),
      argc > 1 ? resolver.Resolve(argv[1]).resolved_path : std::string());
  if (!opened) {
    std::cerr << opened.error() << '\n';
    return 1;
  }
  auto cache = std::move(opened.value());
  for (const auto& asset : assets) {
    auto decoded = DecodeMy(asset.second, &error);
    if (!decoded) {
      std::cerr << error << '\n';
      return 1;
    }
    // Preload under the resolver's identifier. Reference composition consumes
    // this core layer and does not attempt to parse the .my bytes as USD.
    const auto resolved = resolver.Resolve(asset.first);
    if (!resolved.exists) return 1;
    cache.PreloadLayer(resolved.resolved_path, std::move(decoded));
  }
  Stage composed;
  if (!cache.BuildStage(&composed, &warning, &error) || !error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  if (argc == 1) {
    for (const auto& item : {std::pair<const char*, float>{"/Root/Bora", 3.14f},
                             {"/Root/Dora", 6.14f}}) {
      const auto prim = composed.GetPrimAtPath(item.first);
      const Value* value = prim.GetPropertyValue("myval");
      if (!value || !value->as_float() || *value->as_float() != item.second) {
        std::cerr << "Custom-format reference did not compose: " << item.first << '\n';
        return 1;
      }
    }
  }
  if (!warning.empty()) std::cerr << warning << '\n';
  std::cout << WriteUSDAToString(composed);
  return 0;
}
