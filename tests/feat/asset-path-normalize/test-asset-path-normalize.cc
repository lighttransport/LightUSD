// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression: io::NormalizePath -- lexical `.`/`..` collapse used by the opt-in
// asset-path normalization on flatten (SetNormalizeAssetPathOnFlatten).
//
// On flatten, a referenced asset's RELATIVE asset path (e.g.
// `../../../Materials/x.png`) is anchored to the asset's directory; lightusd's
// JoinPath leaves the `..` in place (`.../Meshes/Foo/../../../Materials/x.png`),
// while usdcat collapses it (`.../Materials/x.png`). The opt-in normalizes the
// joined path to match.

#include <iostream>
#include <string>

#include "io-util.hh"
#include "composition.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {
int g_failures = 0;
void expect(const std::string &got, const std::string &want,
            const std::string &what) {
  if (got != want) {
    std::cerr << "FAIL: " << what << " : got '" << got << "' want '" << want
              << "'\n";
    ++g_failures;
  }
}
}  // namespace

int main() {
  // Real UE-exported case: a deep anchor + `../../../` back to a sibling tree.
  expect(io::NormalizePath(
             "/a/b/Asset/Meshes/MeshGroup/Sub/"
             "../../../Materials/Group/x.png"),
         "/a/b/Asset/Materials/Group/x.png",
         "collapse ../../../ against anchor (absolute)");

  expect(io::NormalizePath("/a/b/c/../d"), "/a/b/d", "single .. absolute");
  expect(io::NormalizePath("/a/b/./c"), "/a/b/c", "collapse . absolute");
  expect(io::NormalizePath("/a/b//c"), "/a/b/c", "collapse empty (//) ");
  expect(io::NormalizePath("/a/../../b"), "/b", ".. at root is dropped (absolute)");
  expect(io::NormalizePath("/a/b/c"), "/a/b/c", "already normalized unchanged");

  // Relative paths keep leading `..` that cannot be collapsed.
  expect(io::NormalizePath("a/b/../c"), "a/c", "relative single ..");
  expect(io::NormalizePath("../a/b"), "../a/b", "relative leading .. preserved");
  expect(io::NormalizePath("../../a"), "../../a", "relative leading .. x2");
  expect(io::NormalizePath("a/../../b"), "../b", "relative climbs above anchor");

  // Edge cases.
  expect(io::NormalizePath(""), "", "empty");
  expect(io::NormalizePath("/"), "/", "root");

  // The opt-in flag toggles cleanly (default OFF).
  if (GetNormalizeAssetPathOnFlatten()) {
    std::cerr << "FAIL: SetNormalizeAssetPathOnFlatten default should be false\n";
    ++g_failures;
  }
  SetNormalizeAssetPathOnFlatten(true);
  if (!GetNormalizeAssetPathOnFlatten()) {
    std::cerr << "FAIL: SetNormalizeAssetPathOnFlatten(true) not observed\n";
    ++g_failures;
  }
  SetNormalizeAssetPathOnFlatten(false);  // restore

  if (g_failures == 0) {
    std::cout << "test-asset-path-normalize: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-asset-path-normalize: " << g_failures << " failure(s)\n";
  return 1;
}
