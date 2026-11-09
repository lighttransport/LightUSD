// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test: a locally-authored child PrimSpec must survive composition
// even when the prim also pulls a reference/inherit/specialize whose base does
// NOT contain that child.
//
// Bug: a large UE-exported scene authored StaticMesh LODs as a `LOD`
// variantSet; a scene actor references the asset AND authors a local prim-level
// `def "LOD2"` (collides with the unselected LOD2 variant). InheritPrimSpecImpl
// re-added dst-only *properties* but dropped dst-only *children*, so the local
// `def "LOD2"` was silently lost on flatten. OpenUSD keeps it.
//
// Fixtures: tests/usda/feat-localchild-{ref,main}.usda

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "lightusd.hh"
#include "composition.hh"
#include "asset-resolution.hh"
#include "core/prim-spec.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::cerr << "FAIL: " << (msg) << "  (line " << __LINE__ << ")\n"; \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

bool hasChild(const PrimSpec &ps, const std::string &name) {
  for (const auto &c : ps.children()) {
    if (c.name() == name) return true;
  }
  return false;
}

const PrimSpec *findRoot(const Layer &layer, const std::string &name) {
  auto it = layer.primspecs().find(name);
  return (it == layer.primspecs().end()) ? nullptr : &it->second;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // ---- A. Direct InheritPrimSpec: a dst-only child must survive ----
  {
    PrimSpec src;
    src.name() = "Geom";
    src.specifier() = Specifier::Def;
    {
      PrimSpec a;
      a.name() = "LODa";
      a.specifier() = Specifier::Def;
      src.children().push_back(a);
    }

    PrimSpec dst;  // stronger (local) opinion
    dst.name() = "Geom";
    dst.specifier() = Specifier::Def;
    {
      PrimSpec b;
      b.name() = "LODb";
      b.specifier() = Specifier::Def;
      dst.children().push_back(b);
    }

    std::string warn, err;
    CHECK(InheritPrimSpec(dst, src, &warn, &err), "InheritPrimSpec succeeded");
    CHECK(hasChild(dst, "LODa"), "base (src) child LODa present");
    CHECK(hasChild(dst, "LODb"),
          "dst-only child LODb survives InheritPrimSpec (was dropped)");
  }

  // ---- B. End-to-end flatten: reference (LODs via variantSet) + local def ----
  {
    Layer layer;
    std::string warn, err;
    CHECK(LoadLayerFromFile("tests/usda/feat-localchild-main.usda", &layer,
                            &warn, &err),
          "load feat-localchild-main.usda");

    AssetResolutionResolver resolver;
    resolver.set_search_paths({"tests/usda"});

    Layer afterRef;
    CHECK(CompositeReferencesInPlace(
              resolver, std::make_unique<Layer>(std::move(layer)), &afterRef,
              &warn, &err),
          "CompositeReferencesInPlace succeeded");

    Layer afterVar;
    CHECK(CompositeVariant(afterRef, &afterVar, &warn, &err),
          "CompositeVariant succeeded");

    const PrimSpec *geom = findRoot(afterVar, "Geom");
    CHECK(geom != nullptr, "composited /Geom present");
    if (geom) {
      CHECK(hasChild(*geom, "LODb"),
            "local prim-level def LODb survives flatten "
            "(large-scene LOD2 drop regression)");
      CHECK(hasChild(*geom, "LODa"),
            "variant-selected LODa present (reference resolved)");
      CHECK(hasChild(*geom, "Materials"),
            "referenced Materials scope present");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-localchild: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-localchild: " << g_failures << " failure(s)\n";
  return 1;
}
