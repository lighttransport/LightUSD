// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test (classic/eager composition): composing a `payload` onto a prim
// that already carries variantSet CONTENT must NOT drop that content.
//
// Bug: InheritPrimSpecImpl (src/composition.cc) -- used by CompositePayloadRec --
// builds `ps = src` (the payload's content), overlays dst's metadata, properties
// and CHILDREN, and carries over dst-only children, but it never carried over
// dst's variantSet CONTENT (the `variantSet "x" = { ... }` blocks). So `dst =
// std::move(ps)` discarded any variant content the prim already had. When a prim
// has BOTH a payload (e.g. a material binding) AND a variant whose selected
// option supplies geometry via `over "GEO"`, the payload stage deleted the
// `over "GEO"` and the geometry vanished -- variant selection then found nothing.
//
// This is the dominant cause of Animal Logic ALab under-composing on the eager
// path: a component's `/root` gets the surfacing look-binding `payload` composed
// beside the modelling reference's geo variant, and every component dropped to
// zero meshes (workbench: ~28 meshes raw-flatten -> 4 drawn). With the fix the
// raw flatten recovers the full geometry (workbench ~1327 meshes, matching the
// next/pcp engine's 1336).
//
// Fixtures: tests/usda/feat-payload-variant-{main,bind}.usda

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

// Iteratively compose references -> payload -> variants, deferring variant
// resolution until references & payloads settle (mirrors the lusdcat driver and
// the feat-variant-payload-chain helper).
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
      unresolved = true;
      const bool arcs_settled = !cur.check_unresolved_references() &&
                                !cur.check_unresolved_payload();
      if (arcs_settled) {
        Layer next;
        if (!CompositeVariant(cur, &next, warn, err)) {
          return false;
        }
        cur = std::move(next);
      }
    }

    if (!unresolved) break;
  }
  *out = std::move(cur);
  return true;
}

}  // namespace

int main() {
  Layer layer;
  std::string warn, err;
  CHECK(LoadLayerFromFile("tests/usda/feat-payload-variant-main.usda", &layer,
                          &warn, &err),
        "load feat-payload-variant-main.usda");

  AssetResolutionResolver resolver;
  resolver.set_search_paths({"tests/usda"});

  Layer flat;
  CHECK(flatten(resolver, std::move(layer), &flat, &warn, &err),
        "iterative flatten (payload + variant-gated geometry)");

  const PrimSpec *root = findRoot(flat, "root");
  CHECK(root != nullptr, "composited /root present");
  if (root) {
    // The crux: the variant's `over "GEO"` geometry must survive the payload
    // composition.
    const PrimSpec *geo = findChild(*root, "GEO");
    CHECK(geo != nullptr, "/root/GEO present (variant content survived payload)");
    if (geo) {
      const PrimSpec *mesh = findChild(*geo, "M");
      CHECK(mesh != nullptr,
            "/root/GEO/M (variant-gated Mesh) survived payload composition");
      if (mesh) {
        CHECK(mesh->typeName() == "Mesh", "/root/GEO/M is a Mesh");
      }
    }

    // The payload's own (non-geometry) content must also compose in -- proving we
    // MERGE rather than letting either side clobber the other.
    CHECK(findChild(*root, "MATERIAL") != nullptr,
          "/root/MATERIAL present (payload content composed in too)");

    // Variant arcs fully resolved after flatten.
    CHECK(!root->metas().variants.has_value(),
          "variant selection cleared on /root after flatten");
    CHECK(!root->metas().variantSets.has_value(),
          "variantSets cleared on /root after flatten");
  }

  if (g_failures == 0) {
    std::cout << "test-payload-variant-content: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-payload-variant-content: " << g_failures
            << " failure(s)\n";
  return 1;
}
