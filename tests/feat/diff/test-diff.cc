// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Feature test for the tydra value-level diff API (src/tydra/diff-and-compare):
//   - ULP-tolerant float compare (matrix4d / quatf), configurable via DiffOptions
//   - metadata diffs + reasons (prim kind, value, type, relationship target)
//   - CenterValuePairForDiff (diff-aware value display)
//
// Self-contained: builds the two layers from in-memory USDA strings.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

#include "lightusd.hh"
#include "tydra/diff-and-compare.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL: " << (msg) << "  [" << #cond << "] (line "      \
                << __LINE__ << ")\n";                                     \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

bool LoadMem(const std::string &usda, Layer *layer) {
  std::string warn, err;
  bool ok = LoadUSDALayerFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      layer, &warn, &err);
  if (!ok) std::cerr << "load error: " << err << "\n";
  return ok;
}

bool HasDiff(const Layer &a, const Layer &b, const DiffOptions &opts) {
  lightusd::legacy::HashMap<std::string, PrimSpecDiff> ps;
  lightusd::legacy::HashMap<std::string, PropDiff> pp;
  LayerMetaDiff lm;
  Diff(a, b, ps, pp, opts, &lm);
  return !ps.empty() || !pp.empty() || lm.changed();
}

bool TextContains(const Layer &a, const Layer &b, const DiffOptions &opts,
                  const std::string &needle) {
  std::string t = DiffToText(a, b, "a", "b", opts);
  return t.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // ---- 1. ULP-tolerant matrix4d / quatf compare ----
  {
    char dbuf[64], fbuf[64];
    std::snprintf(dbuf, sizeof(dbuf), "%.17g", std::nextafter(1.0, 2.0));
    std::snprintf(fbuf, sizeof(fbuf), "%.17g",
                  double(std::nextafterf(1.0f, 2.0f)));

    auto scene = [](const char *m00, const char *qr) {
      std::string s = "#usda 1.0\n";
      s += "def Xform \"X\"\n{\n";
      s += "    custom matrix4d xf = ( (";
      s += m00;
      s += ", 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )\n";
      s += "    custom quatf q = (";
      s += qr;
      s += ", 0, 0, 0)\n";
      s += "}\n";
      return s;
    };

    Layer a, b;
    CHECK(LoadMem(scene("1", "1"), &a), "load ulp A");
    CHECK(LoadMem(scene(dbuf, fbuf), &b), "load ulp B (1 ULP perturbed)");

    DiffOptions def;  // default: 1 ULP
    CHECK(!HasDiff(a, b, def), "1-ULP matrix/quat must be equal at default ulps");

    DiffOptions exact;
    exact.floatUlps = 0;
    exact.doubleUlps = 0;
    CHECK(HasDiff(a, b, exact), "1-ULP matrix/quat must differ at ulps=0");
    CHECK(TextContains(a, b, exact, "xf"), "ulps=0 diff names xf");
  }

  // ---- 2. real value change is always reported ----
  {
    Layer a, b;
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { custom float v = 1.0 }\n", &a),
          "load val A");
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { custom float v = 2.0 }\n", &b),
          "load val B");
    DiffOptions def;
    CHECK(HasDiff(a, b, def), "value change must be reported");
    CHECK(TextContains(a, b, def, "(Property modified: value)"),
          "value change reason is 'value'");
  }

  // ---- 3. type change ----
  {
    Layer a, b;
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { custom float v = 1.0 }\n", &a),
          "load type A");
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { custom double v = 1.0 }\n", &b),
          "load type B");
    DiffOptions def;
    CHECK(TextContains(a, b, def, "type"), "float->double reports 'type'");
  }

  // ---- 4. relationship target change ----
  {
    Layer a, b;
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { rel r = </A> }\n", &a), "load rel A");
    CHECK(LoadMem("#usda 1.0\ndef \"X\" { rel r = </B> }\n", &b), "load rel B");
    DiffOptions def;
    CHECK(HasDiff(a, b, def), "relationship target change reported");
    CHECK(TextContains(a, b, def, "target"), "rel change reason mentions target");
  }

  // ---- 5. prim metadata (kind) ----
  {
    Layer a, b;
    CHECK(LoadMem("#usda 1.0\ndef \"X\" ( kind = \"component\" ) {}\n", &a),
          "load kind A");
    CHECK(LoadMem("#usda 1.0\ndef \"X\" ( kind = \"group\" ) {}\n", &b),
          "load kind B");
    DiffOptions def;
    CHECK(HasDiff(a, b, def), "kind change reported");
    CHECK(TextContains(a, b, def, "meta:kind"), "kind change reason is meta:kind");

    DiffOptions nometa;
    nometa.compareMetadata = false;
    CHECK(!HasDiff(a, b, nometa), "kind change suppressed with compareMetadata=false");
  }

  // ---- 6. CenterValuePairForDiff: show the differing region ----
  {
    std::string pfx(300, 'a');
    std::string lhs = pfx + "LEFT" + std::string(50, 'z');
    std::string rhs = pfx + "RIGHT" + std::string(50, 'z');
    auto pr = CenterValuePairForDiff(lhs, rhs);
    CHECK(pr.first.find("LEFT") != std::string::npos,
          "centered lhs shows the differing region");
    CHECK(pr.second.find("RIGHT") != std::string::npos,
          "centered rhs shows the differing region");
    CHECK(pr.first.size() < lhs.size(), "centered lhs is elided/shorter");
    // Identical short strings are returned unchanged.
    auto same = CenterValuePairForDiff("hello", "hello");
    CHECK(same.first == "hello" && same.second == "hello",
          "short equal values returned unchanged");
  }

  if (g_failures == 0) {
    std::cout << "test-diff: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-diff: " << g_failures << " failure(s)\n";
  return 1;
}
