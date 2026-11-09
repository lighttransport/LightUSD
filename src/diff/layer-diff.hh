// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Layer / PrimSpec diff
//
// Structural + value-level diff over two next::Layer trees, mirroring the
// legacy tydra diff-and-compare semantics (src/tydra/diff-and-compare.hh):
// prim added/removed/modified (with reasons), property added/removed/modified
// (with per-aspect reasons and rendered lhs/rhs values), stage/layer metadata
// diffs, ULP/eps-tolerant float compare (scalars, vectors, matrices, arrays,
// timeSample times/values) and fuzzy asset-path equivalence.
//
// The text and JSON renderers produce the same output shape as the legacy
// tydra::DiffToText / DiffToJSON, so consumers (web/js/usddiff.js, the
// `lusddiff` CLI conventions) work unchanged.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layer/layer.hh"

namespace lightusd {
namespace next {

///
/// Options controlling value-level comparison.
///
/// Floating-point values are compared with a ULP (units-in-the-last-place)
/// tolerance by default. This absorbs the ~1 ULP rounding differences that
/// pxrUSD introduces in quaternion / xform math, which would otherwise show
/// up as spurious diffs. The ULP tolerance is applied element-wise to every
/// float-backed type (scalars, vecN, matrices, quats, and their array forms).
///
struct DiffOptions {
  // Per-component ULP tolerance. 0 == bitwise-exact.
  uint32_t floatUlps = 1;   // float (and half, compared in float space)
  uint64_t doubleUlps = 1;  // double

  // If >= 0, two floating values also count as equal when |a-b| <= absEps.
  // OR'd with the ULP test. Negative (default) => pure ULP.
  double absEps = -1.0;

  // ULP tolerance for the timeSample time axis (double).
  uint64_t timeUlps = 2;

  // Compare metadata at every level (attribute / relationship / prim / layer).
  bool compareMetadata = true;

  // Asset-path suffix/leaf equivalence heuristic (differing directory
  // prefixes do not count as a diff when the leaf/suffix matches).
  bool fuzzyAssetPaths = true;
};

struct PropDiff {
  struct ModifiedProp {
    std::string name;
    std::string lhs;
    std::string rhs;
    // What aspects changed: "value", "type", "variability", "connections",
    // "timeSamples", "custom", "listOp", "kind", "targets", "meta:<field>", ...
    std::vector<std::string> reasons;
  };

  std::vector<std::string> addedProps;
  std::vector<std::string> modifiedProps;
  std::vector<ModifiedProp> modifiedPropDetails;
  std::vector<std::string> deletedProps;
};

// A PrimSpec that was modified, with the list of reasons describing what about
// the prim itself changed: "typeName", "specifier", "meta:<field>", ...
// (property and child changes are reported separately).
struct ModifiedPrimSpec {
  std::string name;
  std::vector<std::string> reasons;
};

struct PrimSpecDiff {
  std::vector<std::string> addedPS;
  std::vector<std::string> modifiedPS;
  std::vector<std::string> deletedPS;
  // Parallel to modifiedPS: per-modified-child reason lists.
  std::vector<ModifiedPrimSpec> modifiedDetails;
};

// Layer (stage) metadata differences (metersPerUnit, upAxis, defaultPrim, ...).
struct LayerMetaDiff {
  // e.g. "~metersPerUnit", "customLayerData:+foo", "~upAxis".
  std::vector<std::string> changedFields;
  std::vector<PropDiff::ModifiedProp> details;

  bool changed() const { return !changedFields.empty(); }
};

void Diff(const Layer &lhs, const Layer &rhs,

          /* key = primspec path */
          std::unordered_map<std::string, PrimSpecDiff> &psDiffs,

          /* key = primspec path */
          std::unordered_map<std::string, PropDiff> &propDiffs,

          const DiffOptions &opts = {},

          /* optional: stage/layer-metadata diff (nullptr to skip reporting) */
          LayerMetaDiff *layerMetaDiff = nullptr);

///
/// Generate text-based diff output similar to 'diff' command.
/// Returns "No differences found.\n" when the layers are equivalent.
///
std::string DiffToText(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name = "left",
                       const std::string &rhs_name = "right",
                       const DiffOptions &opts = {});

///
/// Generate JSON-based diff output:
/// { "comparison": {left, right},
///   "primspec_diffs": { path: {added, deleted, modified, modified_details} },
///   "property_diffs": { path: {added, deleted, modified,
///                              modified_details:[{name,left,right,reasons}]} },
///   "layer_meta_diff": { changed, details:[{name,left,right}] } }
///
std::string DiffToJSON(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name = "left",
                       const std::string &rhs_name = "right",
                       const DiffOptions &opts = {});

///
/// Given two rendered values that may be long and share a long common prefix
/// (e.g. asset paths), return a pair of display strings centered on the first
/// differing offset, with elided common regions marked by an ellipsis. If both
/// fit in `window`, they are returned unchanged.
///
std::pair<std::string, std::string> CenterValuePairForDiff(
    const std::string &lhs, const std::string &rhs, size_t window = 240);

}  // namespace next
}  // namespace lightusd
