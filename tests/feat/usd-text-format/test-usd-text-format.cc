// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: opt-in OpenUSD-compatible USDA text layout (SetUSDTextFormat).
//
// usdcat lays out a PrimSpec differently from lightusd's default:
//   - the prim-metadata opening paren sits on the `def` line: `def M "n" (`
//     (default: the `(` goes on its own line);
//   - `apiSchemas` is emitted FIRST in the metadata block (default: later);
//   - a blank line separates sibling prims and the property block from the first
//     child (default: an indented blank line before all but the last child).
//
// Verifies the PrimSpec (Layer) writer reproduces these under the opt-in and
// keeps the default layout byte-stable when off.

#include <iostream>
#include <string>

#include "lightusd.hh"
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
}  // namespace

int main() {
  const std::string usda =
      "#usda 1.0\n"
      "(\n"
      "    metersPerUnit = 0.01\n"
      "    doc = \"hi\"\n"
      "    defaultPrim = \"Root\"\n"
      ")\n"
      "def Xform \"Root\"\n"
      "{\n"
      "    def Mesh \"A\" (\n"
      "        assetInfo = {\n"
      "            string name = \"A\"\n"
      "        }\n"
      "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
      "    )\n"
      "    {\n"
      "        int x = 1\n"
      "    }\n"
      "    def Mesh \"B\"\n"
      "    {\n"
      "        int y = 2\n"
      "    }\n"
      "}\n";

  std::string warn, err;
  Layer layer;
  CHECK(LoadUSDALayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                                usda.size(), "mem.usda", &layer, &warn, &err),
        "parse USDA");

  // (1) Default layout: paren on its own line; stage metadata in historical
  // (non-alphabetical) order (metersPerUnit before defaultPrim).
  pprint::SetUSDTextFormat(false);
  const std::string def_out = to_string(layer);
  CHECK(def_out.find("def Mesh \"A\" (") == std::string::npos,
        "default: paren NOT on the def line");
  CHECK(def_out.find("metersPerUnit") < def_out.find("defaultPrim"),
        "default: stage metadata keeps historical order");

  // (2) Opt-in usdcat layout.
  pprint::SetUSDTextFormat(true);
  const std::string usd_out = to_string(layer);
  pprint::SetUSDTextFormat(false);  // restore

  CHECK(usd_out.find("def Mesh \"A\" (") != std::string::npos,
        "opt-in: metadata paren on the `def` line");

  // apiSchemas emitted before assetInfo inside A's metadata block.
  size_t api = usd_out.find("apiSchemas");
  size_t ainfo = usd_out.find("assetInfo");
  CHECK(api != std::string::npos && ainfo != std::string::npos && api < ainfo,
        "opt-in: apiSchemas emitted before assetInfo (usdcat field order)");

  // Stage metadata sorted alphabetically by field token (doc -> "documentation"):
  // defaultPrim < documentation < metersPerUnit.
  size_t s_default = usd_out.find("defaultPrim");
  size_t s_doc = usd_out.find("doc = ");
  size_t s_mpu = usd_out.find("metersPerUnit");
  CHECK(s_default < s_doc && s_doc < s_mpu,
        "opt-in: stage metadata alphabetical (defaultPrim, doc, metersPerUnit)");

  // A blank line (\n\n) separates sibling prims A and B. Find B's def and check
  // the two chars before its indentation are a blank line.
  size_t bpos = usd_out.find("def Mesh \"B\"");
  CHECK(bpos != std::string::npos, "B present");
  if (bpos != std::string::npos) {
    // Walk back over B's indentation to the preceding newline; expect a blank
    // line (two consecutive newlines) before the indent run.
    size_t p = bpos;
    while (p > 0 && usd_out[p - 1] == ' ') --p;       // skip indent
    CHECK(p >= 2 && usd_out[p - 1] == '\n' && usd_out[p - 2] == '\n',
          "opt-in: blank line between sibling prims A and B");
  }

  if (g_failures == 0) {
    std::cout << "test-usd-text-format: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-usd-text-format: " << g_failures << " failure(s)\n";
  return 1;
}
