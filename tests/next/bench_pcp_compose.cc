// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP composition benchmark + struct memory stats.
//
// Phase-0 instrumentation for doc/refactor-next.md: captures the baseline
// numbers that the later optimization phases (cycle-frame chain, FindSpecs
// memoization, interned keys, CoW values, PrimSpec footprint) are measured
// against.
//
// Usage:
//   bench_pcp_compose sizes [numPrims]      struct sizeofs + per-prim layer cost
//   bench_pcp_compose compose [M] [R]       M prims referencing R shared assets
//   bench_pcp_compose deep [D]              reference chain of depth D
//   bench_pcp_compose                       all of the above (default sizes)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "next/crate/lazy-array.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/pcp/cache.hh"
#include "next/pcp/prim-index.hh"
#include "next/prim/path.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"

using namespace lightusd::next;

static long peak_rss_kb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;  // kilobytes on Linux
}

static long cur_rss_kb() {
  std::ifstream f("/proc/self/statm");
  long total = 0, resident = 0;
  if (f >> total >> resident) return resident * (sysconf(_SC_PAGESIZE) / 1024);
  return 0;
}

class Timer {
 public:
  void start() { start_ = std::chrono::steady_clock::now(); }
  double ms() const {
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// sizes: struct footprints + per-prim fixed cost of a big flat layer.
// ---------------------------------------------------------------------------

static int do_sizes(size_t num_prims) {
  std::printf("=== struct sizes (bytes) ===\n");
  std::printf("Value              %zu\n", sizeof(Value));
  std::printf("PrimSpec           %zu\n", sizeof(PrimSpec));
  std::printf("PrimMetadata       %zu\n", sizeof(PrimMetadata));
  std::printf("PrimVariantSetData     %zu\n", sizeof(PrimVariantSetData));
  std::printf("Layer              %zu\n", sizeof(Layer));
  std::printf("Path               %zu\n", sizeof(Path));
  std::printf("LazyArrayRef       %zu\n", sizeof(LazyArrayRef));
  std::printf("pcp::CompNode      %zu\n", sizeof(pcp::CompNode));
  std::printf("pcp::PrimIndex     %zu\n", sizeof(pcp::PrimIndex));
  std::printf("pcp::LayerStack    %zu\n", sizeof(pcp::LayerStack));

  // Per-prim fixed cost: prims with NO properties / samples / arcs, so the
  // number isolates PrimSpec + Layer bookkeeping overhead (target of the
  // PrimMetadataCold split + lazy TimeSampleBlock work).
  long rss_before = cur_rss_kb();
  Timer t;
  t.start();
  Layer layer;
  {
    LayerBuilder lb(layer);
    lb.begin_prim("Root", "Xform");
    for (size_t i = 0; i < num_prims; ++i) {
      lb.begin_prim("P" + std::to_string(i), "Xform");
      lb.end_prim();
    }
    lb.end_prim();
    lb.finalize();
  }
  double build_ms = t.ms();
  long rss_after = cur_rss_kb();

  Layer::Stats st = layer.stats();
  std::printf("\n=== empty-prim layer (n=%zu) ===\n", num_prims);
  std::printf("build_time         %.1f ms (%.0f prims/sec)\n", build_ms,
              num_prims / build_ms * 1000.0);
  std::printf("self-reported      %zu bytes (%.1f B/prim)\n", st.memory_bytes,
              double(st.memory_bytes) / double(st.prim_count));
  std::printf("rss_delta          %ld KB (%.1f B/prim)\n",
              rss_after - rss_before,
              double(rss_after - rss_before) * 1024.0 / double(num_prims));
  std::printf("peak_rss           %ld KB\n", peak_rss_kb());
  return 0;
}

// ---------------------------------------------------------------------------
// compose: M referencing prims over R shared in-memory assets.
//
// Each asset is a small subtree (Mesh "A" + child + one float3[verts] array),
// preloaded into the cache; /World/R<i> references @asset_<i%R>@</A>. Times
// Cache::Open, per-prim ComputePrimIndex over all root prims, and BuildStage.
// ---------------------------------------------------------------------------

static int do_compose(size_t M, size_t R, size_t verts) {
  std::vector<std::shared_ptr<Layer>> assets;
  std::vector<float> points(verts * 3, 1.0f);
  for (size_t k = 0; k < R; ++k) {
    auto a = std::make_shared<Layer>();
    LayerBuilder ab(*a);
    ab.begin_prim("A", "Mesh");
    ab.add_property("points", Value::MakeFloat3Array(points));
    ab.add_property("tag", Value::MakeFloat3(float(k), 0, 0));
    ab.begin_prim("Inner", "Sphere");
    ab.end_prim();
    ab.end_prim();
    ab.finalize();
    assets.push_back(a);
  }

  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("World", "Xform");
    for (size_t i = 0; i < M; ++i) {
      rb.begin_prim("R" + std::to_string(i), "");
      rb.current()->meta().references.push_back(
          "@asset_" + std::to_string(i % R) + "@</A>");
      rb.end_prim();
    }
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });

  Timer t;
  t.start();
  auto opened = pcp::Cache::Open(resolver, rootL);
  if (!opened) {
    std::fprintf(stderr, "Cache::Open failed: %s\n", opened.error().c_str());
    return 1;
  }
  pcp::Cache cache = std::move(*opened);
  for (size_t k = 0; k < R; ++k) {
    cache.PreloadLayer("asset_" + std::to_string(k), assets[k]);
  }
  double open_ms = t.ms();

  std::string warn, err;
  t.start();
  size_t indexed = 0;
  for (size_t i = 0; i < M; ++i) {
    Path p("/World/R" + std::to_string(i));
    if (cache.ComputePrimIndex(p, &warn, &err)) ++indexed;
  }
  double index_ms = t.ms();

  t.start();
  Stage stage;
  if (!cache.BuildStage(&stage, &warn, &err)) {
    std::fprintf(stderr, "BuildStage failed: %s\n", err.c_str());
    return 1;
  }
  double build_ms = t.ms();

  Stage::Stats st = stage.GetStats();
  std::printf("=== compose M=%zu R=%zu verts=%zu ===\n", M, R, verts);
  std::printf("open               %.1f ms\n", open_ms);
  std::printf("index              %.1f ms (%zu indices, %.1f us/prim)\n",
              index_ms, indexed, index_ms * 1000.0 / double(M));
  std::printf("build_stage        %.1f ms\n", build_ms);
  std::printf("composed_prims     %zu\n", st.prim_count);
  std::printf("stage_memory       %zu KB\n", st.memory_bytes / 1024);
  std::printf("prototype_count    %zu\n", cache.PrototypeCount());
  std::printf("peak_rss           %ld KB\n", peak_rss_kb());
  if (!err.empty()) std::printf("errors:\n%s", err.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// deep: a reference chain of depth D (chain_0/A -> chain_1/A -> ... ->
// chain_{D-1}/A). Baseline for the per-arc copied cycle-set cost: today each
// level copies a std::set<std::string> of every site above it, so expansion is
// O(D^2 * path-length). The Phase-1 frame chain should make this ~linear.
// ---------------------------------------------------------------------------

static int do_deep(size_t D) {
  std::vector<std::shared_ptr<Layer>> chain;
  for (size_t k = 0; k < D; ++k) {
    auto a = std::make_shared<Layer>();
    LayerBuilder ab(*a);
    ab.begin_prim("A", k + 1 == D ? "Mesh" : "");
    ab.add_property("lvl" + std::to_string(k), Value::MakeFloat3(float(k), 0, 0));
    if (k + 1 < D) {
      ab.current()->meta().references.push_back(
          "@chain_" + std::to_string(k + 1) + "@</A>");
    }
    ab.end_prim();
    ab.finalize();
    chain.push_back(a);
  }

  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("Top", "");
    rb.current()->meta().references.push_back("@chain_0@</A>");
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });

  pcp::CompositionOptions opts;
  opts.max_depth = static_cast<uint32_t>(D + 8);
  auto opened = pcp::Cache::Open(resolver, rootL, "", opts);
  if (!opened) {
    std::fprintf(stderr, "Cache::Open failed: %s\n", opened.error().c_str());
    return 1;
  }
  pcp::Cache cache = std::move(*opened);
  for (size_t k = 0; k < D; ++k) {
    cache.PreloadLayer("chain_" + std::to_string(k), chain[k]);
  }

  std::string warn, err;
  Timer t;
  t.start();
  const pcp::PrimIndex *idx = cache.ComputePrimIndex(Path("/Top"), &warn, &err);
  double index_ms = t.ms();
  if (!idx) {
    std::fprintf(stderr, "ComputePrimIndex failed: %s\n", err.c_str());
    return 1;
  }

  t.start();
  Stage stage;
  if (!cache.BuildStage(&stage, &warn, &err)) {
    std::fprintf(stderr, "BuildStage failed: %s\n", err.c_str());
    return 1;
  }
  double build_ms = t.ms();

  UsdPrim top = stage.GetPrimAtPath("/Top");
  std::printf("=== deep D=%zu ===\n", D);
  std::printf("index              %.2f ms (nodes=%u)\n", index_ms,
              unsigned(idx->GetNodeCount()));
  std::printf("build_stage        %.2f ms\n", build_ms);
  std::printf("composed_type      %s\n", top.GetTypeName().c_str());
  std::printf("deepest_prop       %s\n",
              top.GetPropertyValue("lvl" + std::to_string(D - 1)) ? "ok"
                                                                  : "MISSING");
  std::printf("peak_rss           %ld KB\n", peak_rss_kb());
  return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  auto arg = [&](int i, size_t dflt) {
    return argc > i ? size_t(std::atoll(argv[i])) : dflt;
  };

  if (mode == "sizes") return do_sizes(arg(2, 100000));
  if (mode == "compose") return do_compose(arg(2, 20000), arg(3, 64), 256);
  if (mode == "deep") return do_deep(arg(2, 200));

  int rc = do_sizes(100000);
  std::printf("\n");
  rc |= do_compose(20000, 64, 256);
  std::printf("\n");
  rc |= do_deep(200);
  return rc;
}
