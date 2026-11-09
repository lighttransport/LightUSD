// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test: the `</__class__/...>` override-hook idiom. A prim may
// `inherits`/`specializes` a class that is intentionally NOT defined in the
// composed layer (every Animal Logic ALab entity root does:
//   prepend inherits    = </__class__/<name>>
//   prepend specializes = </_root_type>
// where neither `__class__` nor `_root_type` is ever populated). USD treats such
// an arc as a no-op (it contributes no opinions); OpenUSD/usdcat emit nothing.
//
// We previously emitted "Inherit/Specialize target <...> not found in this layer"
// warnings -- 550+ on a single ALab workbench -- pure noise. This verifies both
// arcs compose to a clean no-op AND produce no warning, while the prim's own
// content (Mesh M) survives untouched.
//
// Fixture: tests/usda/feat-class-override-hook.usda

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

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  Layer layer;
  std::string warn, err;
  CHECK(LoadLayerFromFile("tests/usda/feat-class-override-hook.usda", &layer,
                          &warn, &err),
        "load feat-class-override-hook.usda");

  // Compose specializes then inherits (both target undefined classes).
  Layer spec_composited;
  warn.clear();
  CHECK(CompositeSpecializes(layer, &spec_composited, &warn, &err),
        "CompositeSpecializes succeeds (undefined `_root_type` is a no-op)");
  CHECK(!contains(warn, "Specialize target"),
        "no `Specialize target ... not found` warning (got: '" + warn + "')");

  Layer inh_composited;
  warn.clear();
  CHECK(CompositeInherits(spec_composited, &inh_composited, &warn, &err),
        "CompositeInherits succeeds (undefined `__class__` is a no-op)");
  CHECK(!contains(warn, "Inherit target"),
        "no `Inherit target ... not found` warning (got: '" + warn + "')");

  // The prim's own content must be untouched by the no-op arcs.
  const PrimSpec *root = findRoot(inh_composited, "root");
  CHECK(root != nullptr, "/root present after compose");
  if (root) {
    const PrimSpec *mesh = findChild(*root, "M");
    CHECK(mesh != nullptr, "/root/M survived the no-op inherit/specialize");
    if (mesh) {
      CHECK(mesh->typeName() == "Mesh", "/root/M is a Mesh");
    }
  }

  if (g_failures == 0) {
    std::cout << "test-class-override-hook: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-class-override-hook: " << g_failures << " failure(s)\n";
  return 1;
}
