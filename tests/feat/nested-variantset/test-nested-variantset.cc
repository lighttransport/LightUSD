// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test (classic/eager composition): a SELECTED variant block that
// authors its OWN nested variantSet must keep that nested content through
// layer-mode load AND variant composition.
//
// Two bugs combined to drop the nested content:
//
//  1. The PrimSpec/Layer load path (USDAReader::RegisterPrimSpecHandler ->
//     ToPrimSpecRec, src/usda-reader*.cc) converted each parsed VariantContent
//     into a VariantNode but never copied the variant block's OWN nested
//     variantSets, so `LoadLayerFromFile` silently dropped them. (The Stage
//     path via ConstructVariantPrimTreeRec already recursed.) Fixed by
//     ConvertVariantSetList / BuildVariantSetsRec recursing into the nested sets.
//
//  2. VariantSelectPrimSpec (src/composition-reconstruct.cc) consumed the OUTER
//     variantSets and wholesale-cleared all variant metadata/content, discarding
//     the nested variantSets that the selected block contributed. Fixed by
//     promoting the selected block's nested variantSets (and their selections)
//     and re-establishing them so a subsequent CompositeVariant pass resolves
//     them.
//
// Mirrors Animal Logic ALab: a component's `render_high` geo variant holds a
// `geo_vis` variantSet whose `preview` option supplies a proxy-purpose mesh.
// Without both fixes the proxy geometry vanished (`def Mesh` count 1 vs 2).
//
// Fixture: tests/usda/feat-nested-variantset.usda

#include <iostream>
#include <string>
#include <utility>

#include "lightusd.hh"
#include "composition.hh"
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

bool getPurpose(const PrimSpec &ps, std::string *out) {
  auto it = ps.props().find("purpose");
  if (it == ps.props().end() || !it->second.is_attribute()) return false;
  const Attribute &attr = it->second.get_attribute();
  if (auto pv = attr.get_value<value::token>()) {
    *out = pv.value().str();
    return true;
  }
  return false;
}

// Iteratively resolve variantSets, deferring until refs/payloads settle
// (mirrors the lusdcat driver). This fixture has no refs/payloads.
bool flatten(Layer layer, Layer *out, std::string *warn, std::string *err) {
  Layer cur = std::move(layer);
  const int kMaxIter = 64;
  for (int i = 0; i < kMaxIter; i++) {
    if (!cur.check_unresolved_variant()) break;
    Layer next;
    if (!CompositeVariant(cur, &next, warn, err)) {
      return false;
    }
    cur = std::move(next);
  }
  *out = std::move(cur);
  return true;
}

}  // namespace

int main() {
  Layer layer;
  std::string warn, err;
  CHECK(LoadLayerFromFile("tests/usda/feat-nested-variantset.usda", &layer,
                          &warn, &err),
        "load feat-nested-variantset.usda");

  // The parsed Layer must already carry the nested `geo_vis` variantSet inside
  // the `geo`/`hi` block (bug 1: layer-mode load dropped it).
  {
    const PrimSpec *root = findRoot(layer, "root");
    CHECK(root != nullptr, "parsed /root present");
    if (root) {
      auto vit = root->variantSets().find("geo");
      CHECK(vit != root->variantSets().end(), "parsed /root has `geo` variantSet");
      if (vit != root->variantSets().end()) {
        auto hit = vit->second.variantSet.find("hi");
        CHECK(hit != vit->second.variantSet.end(), "`geo` set has `hi` variant");
        if (hit != vit->second.variantSet.end()) {
          CHECK(hit->second.variantSets().count("geo_vis") == 1,
                "`hi` block retains nested `geo_vis` variantSet after load");
        }
      }
    }
  }

  Layer flat;
  CHECK(flatten(std::move(layer), &flat, &warn, &err),
        "iterative variant flatten (nested variantSet)");

  const PrimSpec *root = findRoot(flat, "root");
  CHECK(root != nullptr, "composited /root present");
  if (root) {
    // Outer variant content: GEO gets render purpose + Mesh M.
    const PrimSpec *geo = findChild(*root, "GEO");
    CHECK(geo != nullptr, "/root/GEO present");
    if (geo) {
      std::string purpose;
      CHECK(getPurpose(*geo, &purpose) && purpose == "render",
            "/root/GEO purpose == render (nested `preview` over applied)");
      CHECK(findChild(*geo, "M") != nullptr, "/root/GEO/M (outer variant) present");
    }

    // Nested variant content: GEO_PROXY (proxy purpose) + its Mesh must survive.
    const PrimSpec *proxy = findChild(*root, "GEO_PROXY");
    CHECK(proxy != nullptr,
          "/root/GEO_PROXY present (nested `geo_vis` variant resolved)");
    if (proxy) {
      std::string purpose;
      CHECK(getPurpose(*proxy, &purpose) && purpose == "proxy",
            "/root/GEO_PROXY purpose == proxy");
      const PrimSpec *mesh = findChild(*proxy, "mesh");
      CHECK(mesh != nullptr, "/root/GEO_PROXY/mesh (nested-variant proxy Mesh) present");
      if (mesh) {
        CHECK(mesh->typeName() == "Mesh", "/root/GEO_PROXY/mesh is a Mesh");
      }
    }

    // All variant arcs fully resolved.
    CHECK(!root->metas().variants.has_value(),
          "variant selection cleared on /root after flatten");
    CHECK(!root->metas().variantSets.has_value(),
          "variantSets cleared on /root after flatten");
  }

  if (g_failures == 0) {
    std::cout << "test-nested-variantset: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-nested-variantset: " << g_failures << " failure(s)\n";
  return 1;
}
