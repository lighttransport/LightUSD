// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: opt-in authored-order emission for flattened crate (USDC) layers.
//
// A crate stores prim children lexicographically (path table order); the
// AUTHORED order lives in the per-prim `primChildren` token-vector field. The
// Stage reconstruction path records that field, but the Layer (flatten) path
// dropped it, so a flattened crate came out lexicographically sorted instead of
// in authored order (e.g. usdcat preserves authored order on UE-exported scenes;
// lightusd re-sorted the root/children).
//
// pprint::SetPreserveAuthoredOrder(true) opts in: the USDC Layer reader records
// `primChildren` and the Stage writer emits children in that order (entries in
// the field first, any remainder lexicographically). Default (false) keeps the
// existing lexicographical output byte-for-byte.
//
// This round-trips a layer authored in NON-alphabetical order through USDC and
// checks: (1) default flatten output is lexicographical, (2) opt-in flatten
// output is the authored order.

#include <iostream>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "composition.hh"
#include "usdc-writer.hh"
#include "pprinter.hh"

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

// Index of `name` as a `def "<name>"` line in `s`, or npos.
size_t defPos(const std::string &s, const std::string &name) {
  return s.find("\"" + name + "\"");
}

// Render the USDC bytes to a flattened Stage USDA string under the current
// preserve-authored-order setting.
bool renderFlattened(const std::vector<uint8_t> &usdc, std::string *out) {
  std::string warn, err;
  Layer layer;
  if (!LoadLayerFromMemory(usdc.data(), usdc.size(), "mem.usdc", &layer, &warn,
                           &err)) {
    std::cerr << "load usdc: " << err << "\n";
    return false;
  }
  Stage stage;
  if (!LayerToStage(std::move(layer), &stage, &warn, &err)) {
    std::cerr << "layer->stage: " << err << "\n";
    return false;
  }
  *out = stage.ExportToString();
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // Children AND properties authored in deliberately NON-alphabetical order.
  const std::string usda =
      "#usda 1.0\n"
      "def \"Root\"\n"
      "{\n"
      "    custom int zeta = 1\n"
      "    custom int alpha = 2\n"
      "    custom int mike = 3\n"
      "    def \"Zebra\" {}\n"
      "    def \"Apple\" {}\n"
      "    def \"Mango\" {}\n"
      "}\n";

  std::string warn, err;

  Layer ulayer;
  CHECK(LoadUSDALayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                                usda.size(), "mem.usda", &ulayer, &warn, &err),
        "parse USDA into Layer");

  // Write to USDC: the crate writer records the `primChildren` order field.
  std::vector<uint8_t> usdc;
  CHECK(usdc::SaveAsUSDCToMemory(ulayer, &usdc, &warn, &err),
        "write Layer to USDC");
  CHECK(!usdc.empty(), "USDC non-empty");

  // (1) DEFAULT: lexicographical (Apple < Mango < Zebra).
  pprint::SetPreserveAuthoredOrder(false);
  std::string def_out;
  CHECK(renderFlattened(usdc, &def_out), "render default");
  {
    size_t a = defPos(def_out, "Apple");
    size_t m = defPos(def_out, "Mango");
    size_t z = defPos(def_out, "Zebra");
    CHECK(a != std::string::npos && m != std::string::npos &&
              z != std::string::npos,
          "all children present (default)");
    CHECK(a < m && m < z,
          "default flatten emits children lexicographically (Apple,Mango,Zebra)");
  }

  // (2) OPT-IN: authored order (Zebra, Apple, Mango).
  pprint::SetPreserveAuthoredOrder(true);
  std::string ord_out;
  CHECK(renderFlattened(usdc, &ord_out), "render preserve-order");
  {
    size_t z = defPos(ord_out, "Zebra");
    size_t a = defPos(ord_out, "Apple");
    size_t m = defPos(ord_out, "Mango");
    CHECK(z != std::string::npos && a != std::string::npos &&
              m != std::string::npos,
          "all children present (opt-in)");
    CHECK(z < a && a < m,
          "opt-in flatten emits children in authored order (Zebra,Apple,Mango)");
  }

  // (3) Layer-render path (what `lusdcat --preserve-order` emits): authored
  // CHILD order, but ALPHABETICAL property order -- pxr/usdcat sorts properties
  // even though it preserves child order, so the property order must NOT follow
  // the authored sequence.
  {
    std::string warn2, err2;
    Layer rlayer;
    pprint::SetPreserveAuthoredOrder(true);
    CHECK(LoadLayerFromMemory(usdc.data(), usdc.size(), "mem.usdc", &rlayer,
                              &warn2, &err2),
          "reload usdc for layer render");
    const std::string ltext = to_string(rlayer);
    pprint::SetPreserveAuthoredOrder(false);

    size_t z = ltext.find("\"Zebra\"");
    size_t a = ltext.find("\"Apple\"");
    size_t m = ltext.find("\"Mango\"");
    CHECK(z != std::string::npos && z < a && a < m,
          "layer render: children in authored order (Zebra,Apple,Mango)");

    size_t pa = ltext.find("int alpha");
    size_t pm = ltext.find("int mike");
    size_t pz = ltext.find("int zeta");
    CHECK(pa != std::string::npos && pm != std::string::npos &&
              pz != std::string::npos,
          "layer render: all properties present");
    CHECK(pa < pm && pm < pz,
          "layer render: properties ALPHABETICAL (alpha,mike,zeta), matching "
          "usdcat -- not authored order");
  }

  pprint::SetPreserveAuthoredOrder(false);  // restore global

  if (g_failures == 0) {
    std::cout << "test-preserve-order: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-preserve-order: " << g_failures << " failure(s)\n";
  return 1;
}
