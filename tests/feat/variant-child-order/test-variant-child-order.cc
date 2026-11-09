// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: OpenUSD inserts variant-SELECTED children BEFORE the prim's local
// children (locals keep authored order). lightusd previously APPENDED new variant
// children, so a referenced asset's `variantSet "LOD"` geometry came out after
// the locally-authored `def Scope "Materials"` instead of before it (matching
// usdcat). CompositeVariant now prepends new variant children and keeps the
// `primChildren` order metadatum in sync.

#include <iostream>
#include <string>

#include "lightusd.hh"
#include "composition.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {
int g_failures = 0;
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::cerr << "FAIL: " << (msg) << "  (line " << __LINE__ << ")\n"; \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

const PrimSpec *findRoot(const Layer &l, const std::string &n) {
  auto it = l.primspecs().find(n);
  return it == l.primspecs().end() ? nullptr : &it->second;
}
}  // namespace

int main() {
  // Local children authored as Local1, Local2; the selected variant adds VarA.
  const std::string usda =
      "#usda 1.0\n"
      "def \"Host\" (\n"
      "    variants = { string v = \"a\" }\n"
      "    prepend variantSets = \"v\"\n"
      ")\n"
      "{\n"
      "    def \"Local1\" {}\n"
      "    def \"Local2\" {}\n"
      "    variantSet \"v\" = {\n"
      "        \"a\" {\n"
      "            def \"VarA\" {}\n"
      "        }\n"
      "    }\n"
      "}\n";

  std::string warn, err;
  Layer layer;
  CHECK(LoadUSDALayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                                usda.size(), "mem.usda", &layer, &warn, &err),
        "parse USDA");

  Layer composited;
  CHECK(CompositeVariant(layer, &composited, &warn, &err),
        "CompositeVariant");

  const PrimSpec *host = findRoot(composited, "Host");
  CHECK(host != nullptr, "Host present");
  if (host) {
    // Children: VarA (variant) FIRST, then locals in authored order.
    const auto &c = host->children();
    CHECK(c.size() == 3, "Host has 3 children");
    if (c.size() == 3) {
      CHECK(c[0].name() == "VarA",
            "variant child VarA is first (prepended before locals)");
      CHECK(c[1].name() == "Local1", "Local1 second");
      CHECK(c[2].name() == "Local2", "Local2 third");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-variant-child-order: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-variant-child-order: " << g_failures << " failure(s)\n";
  return 1;
}
