// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: composing a variant whose selected block DEFINES a prim that the
// host also carries a directly-authored local `over` for. Two related bugs, both
// observed on a UE-exported scene (a referenced mesh's geometry lives inside a
// `variantSet "LOD"` and the root layer deep-overrides the composed material
// path):
//
//  1) Specifier promotion. The variant's `def Mesh "LOD0"` must merge UNDER the
//     host's local `over "LOD0"` and promote it to `def Mesh` (AOUSD 12.2.1/2:
//     a prim is defined if ANY opinion defines it). Previously the local `over`
//     swallowed the variant definition, leaving `over "LOD0"` with no typeName
//     on flatten -- the geometry silently vanished.
//
//  2) LIVRPS strength (Local > Variant). The host's directly-authored local
//     attribute opinion must WIN over the variant's opinion for the same
//     attribute. Previously the variant-merge applied the variant as the
//     STRONGER opinion, so a deep local override (sourceAsset = "WORN") was
//     overwritten by the variant's value ("CLEAN").
//
// Drives the composition library's CompositeVariant() directly (no file I/O).

#include <iostream>
#include <string>

#include "composition.hh"
#include "lightusd.hh"

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

  // Host has a local `over "LOD0"` (sourceAsset = WORN) AND a variantSet whose
  // selected block "a" defines `def Mesh "LOD0"` (sourceAsset = CLEAN).
  const std::string usda =
      "#usda 1.0\n"
      "def Xform \"Host\" (\n"
      "    variants = {\n"
      "        string v = \"a\"\n"
      "    }\n"
      "    prepend variantSets = \"v\"\n"
      ")\n"
      "{\n"
      "    over \"LOD0\"\n"
      "    {\n"
      "        uniform asset info:src = @WORN@\n"
      "    }\n"
      "    variantSet \"v\" = {\n"
      "        \"a\" {\n"
      "            def Mesh \"LOD0\"\n"
      "            {\n"
      "                uniform asset info:src = @CLEAN@\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "}\n";

  std::string warn, err;

  Layer layer;
  CHECK(LoadUSDALayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                                usda.size(), "mem.usda", &layer, &warn, &err),
        "parse USDA into Layer");

  Layer composited;
  CHECK(CompositeVariant(layer, &composited, &warn, &err),
        "CompositeVariant succeeds");

  const PrimSpec *host = findRoot(composited, "Host");
  CHECK(host != nullptr, "Host present after variant composition");
  if (!host) {
    std::cerr << "test-variant-local-override: " << g_failures
              << " failure(s)\n";
    return 1;
  }

  const PrimSpec *lod0 = findChild(*host, "LOD0");
  CHECK(lod0 != nullptr, "LOD0 present");
  if (lod0) {
    // (1) The variant's `def Mesh` promotes the local `over` to a defining spec
    // with the variant's typeName.
    CHECK(lod0->specifier() == Specifier::Def,
          "LOD0 specifier promoted over->def by the variant definition");
    CHECK(lod0->typeName() == "Mesh",
          "LOD0 typeName taken from the variant's `def Mesh`");

    // (2) The host's local `info:src` (WORN) wins over the variant's (CLEAN).
    auto pit = lod0->props().find("info:src");
    CHECK(pit != lod0->props().end(), "LOD0 has info:src");
    if (pit != lod0->props().end() && pit->second.is_attribute()) {
      const Attribute &attr = pit->second.get_attribute();
      std::string got;
      if (auto pv = attr.get_value<lightusd::legacy::value::AssetPath>()) {
        got = pv.value().GetAssetPath();
      }
      CHECK(got == "WORN",
            "LOD0 info:src = local override (WORN) wins over variant (CLEAN); "
            "got '" + got + "'");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-variant-local-override: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-variant-local-override: " << g_failures << " failure(s)\n";
  return 1;
}
