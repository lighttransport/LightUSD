// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test (classic/eager composition): a prim authored INSIDE a variant
// block must load with the SAME asset-resolution working path (cwp) as the prim
// that owns the variantSet, so its OWN relative-path payload/reference resolves
// against the correct directory.
//
// Bug (cwp anchoring, the variant-nested sub-case of "Bug 2"): the initial
// PropagateAssetResolverState (src/lightusd.cc) recursed only into ps.children()
// and never into ps.variantSets(), so a prim that exists only inside a variant
// (e.g. ALab's `geo_vis` proxy GEO_PROXY) loaded with an EMPTY cwp. When that
// variant was later selected and its payload composed, CompositePayloadRec read
// the empty cwp and LoadAsset fell back to the resolver's STALE global working
// path -- the directory of whatever asset was loaded last (a sibling
// `render_high/mesh` payload). So GEO_PROXY's `@display_high/mesh/...@` payload
// resolved one directory wrong -> "Asset not found" -> dropped geometry.
//
// Here GEO has a payload `@a/mesh_a.usda@` and the nested variant's GEO_PROXY has
// `@b/mesh_b.usda@`, both anchored at the main layer's dir. After GEO's payload
// loads (leaving the resolver cwp at .../a), GEO_PROXY's payload must still
// resolve to .../b, NOT .../a. We assert by the distinct mesh point data.
//
// Fixture: tests/usda/feat-variant-payload-cwp/{main,a/mesh_a,b/mesh_b}.usda

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

// First X coordinate of `points` -- distinguishes mesh_a (1.0) from mesh_b (2.0).
bool firstPointX(const PrimSpec &ps, float *out) {
  auto it = ps.props().find("points");
  if (it == ps.props().end() || !it->second.is_attribute()) return false;
  const Attribute &attr = it->second.get_attribute();
  if (auto pv = attr.get_value<std::vector<value::point3f>>()) {
    if (pv.value().size() < 2) return false;
    *out = pv.value()[1].x;  // points[1] = (1,0,0) for A, (2,0,0) for B
    return true;
  }
  return false;
}

// Iteratively compose references -> payload -> variants, deferring variant
// resolution until references & payloads settle (mirrors the lusdcat driver).
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
  CHECK(LoadLayerFromFile("tests/usda/feat-variant-payload-cwp/main.usda",
                          &layer, &warn, &err),
        "load feat-variant-payload-cwp/main.usda");

  AssetResolutionResolver resolver;

  Layer flat;
  CHECK(flatten(resolver, std::move(layer), &flat, &warn, &err),
        "iterative flatten (variant-nested payloads)");

  const PrimSpec *root = findRoot(flat, "root");
  CHECK(root != nullptr, "composited /root present");
  if (root) {
    // GEO's own payload (mesh_a) resolves -- baseline.
    const PrimSpec *geo = findChild(*root, "GEO");
    CHECK(geo != nullptr, "/root/GEO present");
    if (geo) {
      float x = 0.0f;
      CHECK(firstPointX(*geo, &x), "/root/GEO has composed points (mesh_a)");
      CHECK(x == 1.0f, "/root/GEO points came from a/mesh_a.usda");
    }

    // The crux: GEO_PROXY (variant-nested) resolves ITS payload to b/, not the
    // stale a/ that GEO's payload left in the resolver.
    const PrimSpec *proxy = findChild(*root, "GEO_PROXY");
    CHECK(proxy != nullptr, "/root/GEO_PROXY present (nested variant resolved)");
    if (proxy) {
      float x = 0.0f;
      CHECK(firstPointX(*proxy, &x),
            "/root/GEO_PROXY has composed points (its payload resolved)");
      CHECK(x == 2.0f,
            "/root/GEO_PROXY points came from b/mesh_b.usda "
            "(correct cwp, not the stale a/ fallback)");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-variant-payload-cwp: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-variant-payload-cwp: " << g_failures << " failure(s)\n";
  return 1;
}
