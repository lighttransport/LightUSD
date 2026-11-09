// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: src/next pcp composition (cycles, arcs, variants,
// payloads, instancing) over fuzzer-shaped multi-layer inputs.
//
// Input format: up to 3 USDA pseudo-layers separated by the line "%%%".
// Layer 0 is the root; layers 1..2 are preloaded as "asset_1" / "asset_2", so
// authored arcs like `references = @asset_1@</X>` compose without disk I/O.
// This is the harness that exercises the Phase-1 cycle/recursion hardening.
//
// Build (clang): cmake -S src/next -B build-fuzz -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON
// Seed corpus: tests/fuzzer/next_compose_seeds/.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "layer/layer.hh"
#include "../../src/pcp/cache.hh"
#include "reader/usda-reader.hh"
#include "resolver/asset-resolver.hh"
#include "stage/stage.hh"

using namespace lightusd::next;

static std::shared_ptr<Layer> ParseChunk(const std::string &chunk) {
  LoadOptions opts;
  opts.parse_options.max_depth = 64;
  opts.parse_options.max_file_size = 4u << 20;
  LoadResult r = LoadUSDAFromString(chunk, opts);
  if (!r.success) return nullptr;
  std::unique_ptr<Layer> l = r.stage.ReleaseRootLayer();
  return std::shared_ptr<Layer>(l.release());
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const std::string input(reinterpret_cast<const char *>(data), size);

  std::vector<std::string> chunks;
  size_t pos = 0;
  while (chunks.size() < 3) {
    size_t sep = input.find("\n%%%\n", pos);
    if (sep == std::string::npos) {
      chunks.push_back(input.substr(pos));
      break;
    }
    chunks.push_back(input.substr(pos, sep - pos));
    pos = sep + 5;
  }
  if (chunks.empty()) return 0;

  std::shared_ptr<Layer> root = ParseChunk(chunks[0]);
  if (!root) return 0;

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &p, const std::string &) { return p; });

  pcp::CompositionOptions opts;
  opts.max_depth = 64;
  opts.max_namespace_depth = 128;

  auto opened = pcp::Cache::Open(resolver, root, "", opts);
  if (!opened) return 0;
  pcp::Cache cache = std::move(*opened);
  for (size_t k = 1; k < chunks.size(); ++k) {
    std::shared_ptr<Layer> asset = ParseChunk(chunks[k]);
    if (asset) cache.PreloadLayer("asset_" + std::to_string(k), asset);
  }

  Stage stage;
  std::string warn, err;
  cache.BuildStage(&stage, &warn, &err);

  // Exercise payload + invalidation paths on whatever was deferred.
  for (const Path &p : cache.GetDeferredPayloadPaths()) {
    cache.LoadPayload(p, &warn, &err);
    break;
  }
  return 0;
}
