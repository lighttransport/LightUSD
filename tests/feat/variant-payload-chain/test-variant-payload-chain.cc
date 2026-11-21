// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test: a variant selection authored on the REFERENCING prim must
// win over the referenced asset's own default selection, even when the
// populated variantSet lives several composition arcs deep.
//
// Bug (Pixar Kitchen_set, Chair.usd): a scene prim references an asset and
// selects a NON-default modelingVariant ("ChairB"); the asset has EMPTY variant
// blocks plus a payload, and the *populated* variantSet (with its own, weaker,
// default selection "ChairA") only appears two arcs deeper:
//
//   main  : def "C1" (references=@asset@, variants={modelingVariant="ChairB"})
//   asset : def "Chair" (payload=@payload@, variants={modelingVariant="ChairA"},
//                        variantSets="modelingVariant") { EMPTY ChairA/ChairB }
//   payload: def "Chair" (references=@geom@)
//   geom  : def "Chair" (variants={modelingVariant="ChairA"}, ...) {
//             variantSet { "ChairA" {Geom which="A"} "ChairB" {Geom which="B"} } }
//
// Root cause: variant resolution was performed EAGERLY each composition
// iteration. At iter 0 the strong local "ChairB" selection was resolved against
// the asset's EMPTY variant blocks (no content) and then CONSUMED -- the
// selection metadata is reset by VariantSelectPrimSpec. When the populated
// variantSet finally arrived from the geom layer (two arcs deeper, carrying its
// own weaker default "ChairA"), the local selection was already gone, so the
// deep default won. The fix DEFERS variant resolution until references &
// payloads have settled (AOUSD Core Spec 10.3.2.5), so the local selection is
// still present when the real variantSet content is composed in. The flatten()
// helper below mirrors the fixed lusdcat driver (src/legacy/examples/lusdcat/main.cc).
//
// (Note: reference/payload composition already keeps the local variant
// SELECTION strongest across the merge -- this test confirms that too.)
//
// Fixtures: tests/usda/feat-variant-chain-{main,asset,payload,geom}.usda

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

#define CHECK(cond, msg)                                                \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "FAIL: " << (msg) << "  (line " << __LINE__ << ")\n"; \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

const PrimSpec *findRoot(const Layer &layer, const std::string &name) {
  auto it = layer.primspecs().find(name);
  return (it == layer.primspecs().end()) ? nullptr : &it->second;
}

const PrimSpec *findChild(const PrimSpec &ps, const std::string &name) {
  for (const auto &c : ps.children()) {
    if (c.name() == name) return &c;
  }
  return nullptr;
}

// Read a string attribute value off a PrimSpec (empty optional if absent).
nonstd::optional<std::string> stringProp(const PrimSpec &ps,
                                         const std::string &name) {
  auto it = ps.props().find(name);
  if (it == ps.props().end() || !it->second.is_attribute()) {
    return nonstd::nullopt;
  }
  return it->second.get_attribute().get_value<std::string>();
}

// Iteratively compose references -> payload -> variants, DEFERRING variant
// resolution until references & payloads have settled. Mirrors the fixed
// lusdcat driver (src/legacy/examples/lusdcat/main.cc) and CompositeAllArcs().
bool flatten(AssetResolutionResolver &resolver, Layer layer, Layer *out,
             std::string *warn, std::string *err) {
  Layer cur = std::move(layer);
  const int kMaxIter = 64;
  for (int i = 0; i < kMaxIter; i++) {
    bool unresolved = false;

    if (cur.check_unresolved_references()) {
      unresolved = true;
      Layer next;
      if (!CompositeReferencesInPlace(
              resolver, std::make_unique<Layer>(std::move(cur)), &next, warn,
              err)) {
        return false;
      }
      cur = std::move(next);
    }

    if (cur.check_unresolved_payload()) {
      unresolved = true;
      Layer next;
      if (!CompositePayloadInPlace(
              resolver, std::make_unique<Layer>(std::move(cur)), &next, warn,
              err)) {
        return false;
      }
      cur = std::move(next);
    }

    if (cur.check_unresolved_variant()) {
      unresolved = true;  // not done yet either way
      const bool arcs_settled = !cur.check_unresolved_references() &&
                                !cur.check_unresolved_payload();
      if (arcs_settled) {
        Layer next;
        if (!CompositeVariant(cur, &next, warn, err)) {
          return false;
        }
        cur = std::move(next);
      }
      // else: defer variant resolution; loop again to settle refs/payloads.
    }

    if (!unresolved) break;
  }
  *out = std::move(cur);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  Layer layer;
  std::string warn, err;
  CHECK(LoadLayerFromFile("tests/usda/feat-variant-chain-main.usda", &layer,
                          &warn, &err),
        "load feat-variant-chain-main.usda");

  AssetResolutionResolver resolver;
  resolver.set_search_paths({"tests/usda"});

  Layer flat;
  CHECK(flatten(resolver, std::move(layer), &flat, &warn, &err),
        "iterative flatten (ref -> payload -> ref -> deferred variant)");

  const PrimSpec *c1 = findRoot(flat, "C1");
  CHECK(c1 != nullptr, "composited /C1 present");
  if (c1) {
    const PrimSpec *geom = findChild(*c1, "Geom");
    CHECK(geom != nullptr,
          "/C1/Geom present (variant content promoted from deep variantSet)");
    if (geom) {
      auto which = stringProp(*geom, "which");
      CHECK(which.has_value(), "/C1/Geom.which authored");
      // The crux: the local "ChairB" selection must win over the asset's and
      // the geom layer's own default "ChairA".
      CHECK(which.has_value() && which.value() == "I_am_ChairB",
            "local variant selection 'ChairB' wins across reference+payload "
            "chain (got '" +
                (which.has_value() ? which.value() : std::string("<none>")) +
                "', expected 'I_am_ChairB')");
    }
    // Variant arcs must be fully resolved (consumed) after flatten.
    CHECK(!c1->metas().variants.has_value(),
          "variant selection cleared on /C1 after flatten");
    CHECK(!c1->metas().variantSets.has_value(),
          "variantSets cleared on /C1 after flatten");
  }

  if (g_failures == 0) {
    std::cout << "test-variant-payload-chain: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-variant-payload-chain: " << g_failures
            << " failure(s)\n";
  return 1;
}
