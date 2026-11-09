// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test (classic/eager composition, cwp anchoring -- the reference-
// chain core of "Bug 2"): a reference/payload authored in a component's SUBLAYER
// must stay anchored to that subLayer's directory after the component is pulled
// in through an outer reference.
//
// Structure (mirrors ALab entity/<comp> with a `surfacing/` subLayer):
//   top.usda                -> references comp/main.usda
//   comp/main.usda          -> subLayers = [ sub/surf.usda ]
//   comp/sub/surf.usda      -> /root references @../look/look.usda@   (anchored at comp/sub/)
//   comp/look/look.usda     -> Look { Mesh LookMesh }
// `../look/look.usda` resolves correctly ONLY from comp/sub/ (-> comp/look/);
// from the component ROOT comp/ it would escape to <fixture>/look/ and miss.
//
// Bug: after CompositeSublayers correctly anchored /root's reference at comp/sub/
// (CombinePrimSpecRec cross-directory anchoring), LoadAsset's
// PropagateAssetResolverState re-stamped the WHOLE referenced subtree with the
// component ROOT working path, clobbering that subLayer anchor. The subLayer's
// `../look/...` then resolved one directory too high -> "Asset not found" -> the
// look geometry vanished. Fixed by making that propagation gap-fill (only stamp
// prims that still lack a working path), preserving subLayer anchors.
//
// NOTE: this is an end-to-end SMOKE test of the subLayer-reference-through-outer-
// reference composition path. It does not in isolation discriminate the cwp
// clobber, because here the subLayer's directory remains on the resolver's
// accumulated search paths, so `../look` would also resolve via that fallback.
// The real ALab regression (which this fix targets) surfaces only in the deeper
// assembly->component chain, where the geo reference is resolved in a deferred
// pass after the subLayer directory has dropped off the search paths and the
// per-prim working path is the only anchor left. Validated there: lab_workbench01
// asset-not-found 119 -> 61, +520 meshes; single components (tool_screw01,
// decor_bottle_soda02) now byte-match `usdcat --flatten`.

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

// Depth-first search for a descendant PrimSpec of the given typeName.
bool hasDescendantOfType(const PrimSpec &ps, const std::string &typeName) {
  for (const auto &c : ps.children()) {
    if (c.typeName() == typeName) return true;
    if (hasDescendantOfType(c, typeName)) return true;
  }
  return false;
}

bool flatten(AssetResolutionResolver &resolver, Layer layer, Layer *out,
             std::string *warn, std::string *err) {
  // ALab authors parent-relative (`../`) arcs, so enable them (as the lusdcat
  // driver does).
  ReferencesCompositionOptions ref_opts;
  ref_opts.allow_parent_relative_paths = true;
  PayloadCompositionOptions pl_opts;
  pl_opts.allow_parent_relative_paths = true;

  Layer cur = std::move(layer);
  const int kMaxIter = 64;
  for (int i = 0; i < kMaxIter; i++) {
    bool unresolved = false;

    if (cur.check_unresolved_references()) {
      unresolved = true;
      Layer next;
      if (!CompositeReferencesInPlace(
              resolver, std::make_unique<Layer>(std::move(cur)), &next, warn,
              err, ref_opts)) {
        return false;
      }
      cur = std::move(next);
    }

    if (cur.check_unresolved_payload()) {
      unresolved = true;
      Layer next;
      if (!CompositePayloadInPlace(
              resolver, std::make_unique<Layer>(std::move(cur)), &next, warn,
              err, pl_opts)) {
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
  CHECK(LoadLayerFromFile("tests/usda/feat-sublayer-ref-cwp/top.usda", &layer,
                          &warn, &err),
        "load feat-sublayer-ref-cwp/top.usda");

  AssetResolutionResolver resolver;

  Layer flat;
  CHECK(flatten(resolver, std::move(layer), &flat, &warn, &err),
        "iterative flatten (subLayer-anchored reference through outer ref)");

  const PrimSpec *world = findRoot(flat, "world");
  CHECK(world != nullptr, "composited /world present");
  if (world) {
    CHECK(hasDescendantOfType(*world, "Mesh"),
          "/world has the subLayer-referenced LookMesh "
          "(comp/sub/ reference anchor preserved, not clobbered to comp/)");
  }

  if (g_failures == 0) {
    std::cout << "test-sublayer-ref-cwp: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-sublayer-ref-cwp: " << g_failures << " failure(s)\n";
  return 1;
}
