// LightUSD side of the benchmark harness.
//
// Usage: bench_lightusd <input.usd[a|c|z]> [--iters N]
// Output: CSV lines `op,status,iters,median_ms,min_ms,max_ms`

#include <cstdio>
#include <string>

#include "bench_common.hh"

#include "lightusd.hh"
#include "composition.hh"
#include "io-util.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <input.usd> [--iters N]\n", argv[0]);
    return 1;
  }
  const std::string input = argv[1];
  const int iters = bench::parse_iters(argc, argv);

  std::string warn, err;

  // Parse: file -> Stage
  lightusd::legacy::Stage stage;
  bench::run("parse", iters, [&]() {
    stage = lightusd::legacy::Stage();
    warn.clear();
    err.clear();
    return lightusd::legacy::LoadUSDFromFile(input, &stage, &warn, &err);
  });
  if (!err.empty()) {
    std::fprintf(stderr, "parse error: %s\n", err.c_str());
    return 1;
  }

  // Write USDA: Stage -> ASCII file
  const std::string out_usda = "/tmp/bench_lightusd_out.usda";
  bench::run("write_usda", iters, [&]() {
    warn.clear();
    err.clear();
    return lightusd::legacy::usda::SaveAsUSDA(out_usda, stage, &warn, &err);
  });

  // Write USDC: Stage -> Crate binary (in-memory)
  bench::run("write_usdc", iters, [&]() {
    warn.clear();
    err.clear();
    std::vector<uint8_t> data;
    return lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &data, &warn, &err);
  });

  // Composite: load as layer, apply composition arcs, build Stage
  bench::run("composite", iters, [&]() {
    warn.clear();
    err.clear();
    lightusd::legacy::Layer root_layer;
    if (!lightusd::legacy::LoadLayerFromFile(input, &root_layer, &warn, &err)) {
      return false;
    }
    lightusd::legacy::AssetResolutionResolver resolver;
    resolver.set_current_working_path(
        lightusd::io::GetBaseDir(input).empty()
            ? "."
            : lightusd::io::GetBaseDir(input));
    resolver.set_search_paths({resolver.current_working_path()});

    lightusd::legacy::Layer sublayered, referenced, payloaded;
    if (!lightusd::legacy::CompositeSublayers(resolver, root_layer, &sublayered, &warn,
                                      &err)) {
      return false;
    }
    if (!lightusd::legacy::CompositeReferences(resolver, sublayered, &referenced,
                                       &warn, &err)) {
      return false;
    }
    if (!lightusd::legacy::CompositePayload(resolver, referenced, &payloaded, &warn,
                                    &err)) {
      return false;
    }
    lightusd::legacy::Stage composited;
    return lightusd::legacy::LayerToStage(std::move(payloaded), &composited, &warn,
                                  &err);
  });
  if (!err.empty()) {
    std::fprintf(stderr, "composite error: %s\n", err.c_str());
  }

  return 0;
}
