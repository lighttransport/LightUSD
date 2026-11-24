// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test (classic/eager composition, cwp anchoring -- the deeply-nested
// sub-assembly core of "Bug 2"): when a prim's reference/payload pulls in an
// asset that ITSELF still carries unresolved arcs, those arcs must stay anchored
// to the pulled-in asset's directory, not the consuming prim's.
//
// Structure (mirrors ALab: a sub-assembly definition `add payload`s a leaf
// component whose subLayers reference `../../../fragment/geo/...`):
//   top.usda          -> T (add payload = @leaf/leaf.usda@)
//   leaf/leaf.usda    -> root (references = @../sibling/geo.usda@)   <- anchored at leaf/
//   sibling/geo.usda  -> G { Mesh M }
//
// Bug: OverridePrimSpec (append payload) / InheritPrimSpecImpl (prepend) left the
// composed prim carrying the CONSUMING prim's working path (top/). The leaf's
// still-unresolved `../sibling/geo.usda` reference then resolved from top/ -- one
// directory too high -- and the geometry vanished. In the real nested ALab chain
// the consuming dir is a sub-assembly's `base/definition`, several levels off.
//
// We assert on the composed prim's working path directly: a geometry assertion
// would not discriminate, because leaf/ stays on the resolver's accumulated
// search paths and `../sibling` resolves via that fallback regardless of the cwp
// clobber. (The real ALab failure escapes the fallback in a deferred pass.) Fix:
// after the arc merge, re-anchor to the source asset's dir when the source still
// has unresolved arcs (ReanchorArcsToSource + the InheritPrimSpecImpl src-has-arcs
// guard). Validated end-to-end: lab_workbench01 asset-not-found 61 -> 0.

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

bool endsWith(const std::string &s, const std::string &suf) {
  return s.size() >= suf.size() &&
         s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// After ONE payload pass on top.usda, /T must hold leaf's unresolved reference
// AND carry leaf's working path (so `../sibling/geo.usda` resolves), not top's.
bool checkAnchor(const std::string &payload_qualifier) {
  Layer layer;
  std::string warn, err;
  if (!LoadLayerFromFile("tests/usda/feat-nested-payload-cwp/top.usda", &layer,
                         &warn, &err)) {
    std::cerr << "FAIL: load top.usda (" << payload_qualifier << "): " << err
              << "\n";
    return false;
  }

  AssetResolutionResolver resolver;
  PayloadCompositionOptions pl_opts;
  pl_opts.allow_parent_relative_paths = true;

  Layer next;
  if (!CompositePayloadInPlace(resolver,
                               std::make_unique<Layer>(std::move(layer)), &next,
                               &warn, &err, pl_opts)) {
    std::cerr << "FAIL: payload pass (" << payload_qualifier << "): " << err
              << "\n";
    return false;
  }

  const PrimSpec *t = findRoot(next, "T");
  CHECK(t != nullptr, payload_qualifier + ": /T present after payload");
  if (!t) return false;

  CHECK(t->metas().references.has_value(),
        payload_qualifier + ": /T carries leaf's unresolved reference");

  const std::string cwp = t->get_current_working_path();
  CHECK(endsWith(cwp, "leaf"),
        payload_qualifier + ": /T working path anchored at leaf/ (got '" + cwp +
            "'), NOT clobbered to the consuming top/ dir");
  return true;
}

}  // namespace

int main() {
  // The fixture uses `add payload` (append -> OverridePrimSpec), as ALab does.
  checkAnchor("add payload");

  if (g_failures == 0) {
    std::cout << "test-nested-payload-cwp: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-nested-payload-cwp: " << g_failures << " failure(s)\n";
  return 1;
}
