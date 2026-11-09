// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: the USDC reader's PrimSpec (Layer) reconstruction must carry the
// authored specifier (def/over/class). It previously set typeName/name/props/
// metas but NOT specifier(), so a PrimSpec defaulted to `def` — silently
// promoting an authored `over` (e.g. a variant's pure-override child, or a
// scene-level override) to `def` after loading USDC into a Layer (the path used
// by composition/flatten). The Stage reconstruction path already passed the
// specifier, so this only bit the Layer/flatten path.
//
// On Pixar Kitchen_set this manifested as `over "pCylinder389/390/562"` prims
// (color overrides in a shading variant on meshes the selected modeling variant
// does not define) being written as `def` on flatten.
//
// Round-trips a layer with `over` prims through USDC and checks the specifier
// survives.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "usdc-writer.hh"
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

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const std::string usda =
      "#usda 1.0\n"
      "over \"TopOver\"\n"
      "{\n"
      "    custom int x = 1\n"
      "}\n"
      "def \"Host\" (\n"
      "    variants = {\n"
      "        string v = \"a\"\n"
      "    }\n"
      "    prepend variantSets = \"v\"\n"
      ")\n"
      "{\n"
      "    variantSet \"v\" = {\n"
      "        \"a\" {\n"
      "            over \"OverChild\"\n"
      "            {\n"
      "                custom int c = 2\n"
      "            }\n"
      "            def \"DefChild\"\n"
      "            {\n"
      "                custom int d = 3\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "}\n";

  std::string warn, err;

  // Parse USDA -> Layer (the USDA parser preserves `over`).
  Layer ulayer;
  CHECK(LoadUSDALayerFromMemory(
            reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
            "mem.usda", &ulayer, &warn, &err),
        "parse USDA into Layer");

  // Write the Layer to USDC bytes.
  std::vector<uint8_t> usdc;
  CHECK(usdc::SaveAsUSDCToMemory(ulayer, &usdc, &warn, &err),
        "write Layer to USDC");
  CHECK(!usdc.empty(), "USDC output non-empty");

  // Re-read the USDC into a Layer (the path that lost the specifier).
  Layer clayer;
  CHECK(LoadLayerFromMemory(usdc.data(), usdc.size(), "mem.usdc", &clayer,
                            &warn, &err),
        "read USDC into Layer");

  // Top-level `over` must survive the USDC round-trip.
  const PrimSpec *top = findRoot(clayer, "TopOver");
  CHECK(top != nullptr, "TopOver present after USDC round-trip");
  CHECK(top && top->specifier() == Specifier::Over,
        "top-level `over` specifier preserved through USDC (was promoted to def)");

  // The variant's `over` child must survive; its sibling `def` stays `def`.
  const PrimSpec *host = findRoot(clayer, "Host");
  CHECK(host != nullptr, "Host present");
  if (host) {
    auto vit = host->variantSets().find("v");
    CHECK(vit != host->variantSets().end(), "variantSet 'v' present");
    if (vit != host->variantSets().end()) {
      auto sit = vit->second.variantSet.find("a");
      CHECK(sit != vit->second.variantSet.end(), "variant 'a' present");
      if (sit != vit->second.variantSet.end()) {
        const PrimSpec &content = sit->second;
        const PrimSpec *ovc = findChild(content, "OverChild");
        const PrimSpec *dfc = findChild(content, "DefChild");
        CHECK(ovc && ovc->specifier() == Specifier::Over,
              "variant `over` child specifier preserved through USDC");
        CHECK(dfc && dfc->specifier() == Specifier::Def,
              "variant `def` child specifier preserved through USDC");
      }
    }
  }

  if (g_failures == 0) {
    std::cout << "test-usdc-specifier: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-usdc-specifier: " << g_failures << " failure(s)\n";
  return 1;
}
