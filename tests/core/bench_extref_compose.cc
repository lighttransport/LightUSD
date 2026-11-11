// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - external-reference composition benchmark.
//
// Exercises Compositor::Compose over an EXTERNAL reference to a large library
// layer, measuring peak RSS. The library has L prims; most are arc-free Mesh,
// but a few carry an external reference (so the LAYER has composable arcs). A
// root references K of the ARC-FREE library prims.
//
// This isolates the subtree-scoped arc check in ResolveRefArc: an arc-free
// referenced prim grafts the raw library directly instead of forcing the whole
// (heavy) library to be composed/cloned just to pull a few self-contained prims
// from it. With the optimization, composed_prims stays ~K (only the referenced
// prims reach the output); the unreferenced library bulk is never materialized.
//
// Usage:
//   bench_extref_compose [L] [K] [verts]     (defaults: 8000 100 512)

#include "composition/composition.hh"
#include "layer/layer.hh"
#include "types/value.hh"

#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace lightusd;

static long peak_rss_kb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;  // kilobytes on Linux
}

// Library: L prims under /Lib, each an arc-free Mesh with a big points array.
// When `all_arcs` every prim also carries an external reference to @shader@</S>
// (so every prim is arc-bearing but SELF-CONTAINED); otherwise only every 200th
// does (so the LAYER has arcs even though most prims do not).
static std::unique_ptr<Layer> MakeLib(size_t L, size_t verts, bool all_arcs) {
  std::vector<float> points(verts * 3, 1.0f);
  auto lib = std::make_unique<Layer>();
  LayerBuilder lb(*lib);
  lb.begin_prim("Lib", "Xform");
  for (size_t i = 0; i < L; ++i) {
    lb.begin_prim("P" + std::to_string(i), "Mesh");
    lb.add_property("points", Value::MakeFloat3Array(points));
    if (all_arcs || i % 200 == 0) {
      lb.current()->meta().references.push_back("@shader@</S>");
    }
    lb.end_prim();
  }
  lb.end_prim();
  lb.finalize();
  return lib;
}

static std::unique_ptr<Layer> MakeShader() {
  auto shader = std::make_unique<Layer>();
  LayerBuilder sb(*shader);
  sb.begin_prim("S", "Shader");
  sb.add_property("k", Value::MakeFloat3(1, 2, 3));
  sb.end_prim();
  sb.finalize();
  return shader;
}

int main(int argc, char** argv) {
  size_t L = argc > 1 ? std::stoul(argv[1]) : 8000;
  size_t K = argc > 2 ? std::stoul(argv[2]) : 100;
  size_t verts = argc > 3 ? std::stoul(argv[3]) : 512;
  // mode "arcfree" (default): reference arc-free prims  -> exercises the
  //   subtree-arc check (arc-free referenced prim grafts raw).
  // mode "arcref":            reference arc-bearing self-contained prims ->
  //   exercises subtree-scoped composition (compose just the referenced prim's
  //   subtree, not the whole library).
  const std::string mode = argc > 4 ? argv[4] : "arcfree";
  const bool all_arcs = (mode == "arcref");

  auto root = std::make_unique<Layer>();
  {
    LayerBuilder rb(*root);
    rb.begin_prim("World", "Xform");
    for (size_t i = 0; i < K; ++i) {
      // arcfree: odd indices (never %200==0, so arc-free).
      // arcref:  spread across the library; every prim is arc-bearing.
      size_t idx = all_arcs ? (i * 7 + 1) % L : (i * 2 + 1);
      rb.begin_prim("R" + std::to_string(i), "");
      rb.current()->meta().references.push_back(
          "@lib@</Lib/P" + std::to_string(idx) + ">");
      rb.end_prim();
    }
    rb.end_prim();
    rb.finalize();
  }

  Compositor comp;
  comp.SetLayerLoader(
      [&](const std::string& path, std::string*) -> std::unique_ptr<Layer> {
        if (path.find("shader") != std::string::npos) return MakeShader();
        return MakeLib(L, verts, all_arcs);
      });

  auto out = comp.Compose(*root);
  size_t composed = out ? out->prim_count() : 0;
  std::printf(
      "extref mode=%s L=%zu K=%zu verts=%zu  composed_prims=%zu (want ~%zu)  "
      "peak_rss=%ld KB  errors=%zu\n",
      mode.c_str(), L, K, verts, composed, K + 1, peak_rss_kb(),
      comp.GetErrors().size());
  // composed_prims ~ K+1 confirms only the referenced subtrees were
  // materialized — the unreferenced library bulk was never composed/cloned.
  return (out && comp.GetErrors().empty()) ? 0 : 1;
}
