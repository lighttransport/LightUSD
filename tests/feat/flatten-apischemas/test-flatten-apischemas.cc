// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: on --flatten, a composed prim's `apiSchemas` applied-schema
// list-op must be baked to EXPLICIT form -- pxrUSD's `usdcat --flatten` emits
// `apiSchemas = [...]`, never the authored `prepend`/`append` qualifier (which
// would wrongly re-prepend if the flattened layer were itself re-referenced).
// lightusd previously carried the authored `prepend` straight through flatten,
// so the whole-scene diff against usdcat showed ~2800 spurious `meta:apiSchemas`
// modifications on UE-exported scenes (every referenced mesh authors
// `prepend apiSchemas = ["MaterialBindingAPI"]`).
//
// lightusd::legacy::FlattenAppliedSchemas(Layer&) performs this finalize. This test
// drives it directly on a parsed Layer and checks the resolved form; it also
// confirms that WITHOUT the call the authored `prepend` is preserved (the
// finalize must be flatten-only, so a plain load+write round-trip is lossless).

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

// Is the apiSchemas list-op in baked, explicit form? (single ResetToExplicit
// authored op + explicit resolved qualifier; the writer drops the qualifier.)
bool isExplicit(const APISchemas &s) {
  if (s.listOpQual != ListEditQual::ResetToExplicit) return false;
  if (s.authoredOps.size() != 1) return false;
  return s.authoredOps.front().first == ListEditQual::ResetToExplicit;
}

bool hasName(const APISchemas &s, const std::string &name) {
  for (const auto &op : s.authoredOps) {
    for (const auto &it : op.second) {
      if (it.first == name) return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const std::string usda =
      "#usda 1.0\n"
      "def \"Base\" (\n"
      "    prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
      ")\n"
      "{\n"
      "    def \"Child\" (\n"
      "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
      "    )\n"
      "    {\n"
      "    }\n"
      "}\n"
      "def \"Combo\" (\n"
      "    prepend apiSchemas = [\"MaterialBindingAPI\", \"GeomModelAPI\"]\n"
      "    append apiSchemas = [\"MotionAPI\"]\n"
      "    delete apiSchemas = [\"GeomModelAPI\"]\n"
      ")\n"
      "{\n"
      "}\n"
      "def \"NoneExplicit\" (\n"
      "    apiSchemas = None\n"
      ")\n"
      "{\n"
      "}\n";

  std::string warn, err;

  Layer layer;
  CHECK(LoadUSDALayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                                usda.size(), "mem.usda", &layer, &warn, &err),
        "parse USDA into Layer");

  // Before finalize: the authored `prepend` qualifier is preserved (a plain
  // load is not a flatten).
  {
    const PrimSpec *base = findRoot(layer, "Base");
    CHECK(base && base->metas().has_apiSchemas(), "Base has apiSchemas");
    if (base && base->metas().has_apiSchemas()) {
      CHECK(!isExplicit(base->metas().get_apiSchemas()),
            "authored `prepend` preserved before flatten finalize");
    }
  }

  // Finalize (the flatten-completion step).
  FlattenAppliedSchemas(layer);

  // Base: prepend -> explicit, same name.
  {
    const PrimSpec *base = findRoot(layer, "Base");
    CHECK(base && base->metas().has_apiSchemas(), "Base apiSchemas present");
    if (base && base->metas().has_apiSchemas()) {
      const APISchemas s = base->metas().get_apiSchemas();
      CHECK(isExplicit(s), "Base apiSchemas baked to explicit on flatten");
      CHECK(hasName(s, "MaterialBindingAPI"), "Base keeps MaterialBindingAPI");
    }
  }

  // Recurses into children.
  {
    const PrimSpec *base = findRoot(layer, "Base");
    const PrimSpec *child = nullptr;
    if (base) {
      for (const auto &c : base->children()) {
        if (c.name() == "Child") child = &c;
      }
    }
    CHECK(child && child->metas().has_apiSchemas(), "Child apiSchemas present");
    if (child && child->metas().has_apiSchemas()) {
      CHECK(isExplicit(child->metas().get_apiSchemas()),
            "child apiSchemas baked to explicit (recursion)");
    }
  }

  // Combo: prepend [MaterialBindingAPI, GeomModelAPI], append [MotionAPI],
  // delete [GeomModelAPI]. The `delete` must remove the prepended GeomModelAPI
  // from the final set even though the crate-style op order stores `delete`
  // before `prepend` -- list-op resolution is by qualifier category, not
  // storage order.
  {
    const PrimSpec *combo = findRoot(layer, "Combo");
    CHECK(combo && combo->metas().has_apiSchemas(), "Combo apiSchemas present");
    if (combo && combo->metas().has_apiSchemas()) {
      const APISchemas s = combo->metas().get_apiSchemas();
      CHECK(isExplicit(s), "Combo apiSchemas baked to explicit");
      CHECK(hasName(s, "MaterialBindingAPI"),
            "Combo keeps prepended MaterialBindingAPI");
      CHECK(hasName(s, "MotionAPI"), "Combo keeps appended MotionAPI");
      CHECK(!hasName(s, "GeomModelAPI"),
            "Combo `delete` removes the prepended GeomModelAPI (category order)");
    }
  }

  // `apiSchemas = None` (explicit empty) is preserved verbatim, not turned into
  // a populated explicit list.
  {
    const PrimSpec *none = findRoot(layer, "NoneExplicit");
    CHECK(none && none->metas().has_apiSchemas(), "NoneExplicit apiSchemas set");
    if (none && none->metas().has_apiSchemas()) {
      CHECK(none->metas().get_apiSchemas().explicitlyEmpty,
            "`apiSchemas = None` preserved through flatten finalize");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-flatten-apischemas: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-flatten-apischemas: " << g_failures << " failure(s)\n";
  return 1;
}
