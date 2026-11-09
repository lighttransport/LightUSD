// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "core/prim-spec.hh"
#include "core/layer-types.hh"
#include "../tiny-hashmap.hh"

namespace lightusd {
namespace tydra {

///
/// Options controlling value-level comparison.
///
/// Floating-point values are compared with a ULP (units-in-the-last-place)
/// tolerance by default. This absorbs the ~1 ULP rounding differences that
/// pxrUSD introduces in quaternion / xform (matrix4d, quatf) math, which would
/// otherwise show up as spurious diffs. The ULP tolerance is applied
/// element-wise to every float-backed type (scalars, vecN, matrices, quats,
/// and their array forms).
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

  // Keep the existing asset-path suffix/leaf equivalence heuristic.
  bool fuzzyAssetPaths = true;
};

struct PropDiff
{
  struct ModifiedProp {
    std::string name;
    std::string lhs;
    std::string rhs;
    // What aspects changed: "value", "type", "variability", "connections",
    // "timeSamples", "custom", "listOp", "meta:<field>", ...
    std::vector<std::string> reasons;
  };

  std::vector<std::string> addedProps;
  std::vector<std::string> modifiedProps;
  std::vector<ModifiedProp> modifiedPropDetails;
  std::vector<std::string> deletedProps;
};

// A PrimSpec that was modified, with the list of reasons describing what about
// the prim itself changed: "typeName", "specifier", "propCount", "childCount",
// "meta:<field>", ... (property and child changes are reported separately).
struct ModifiedPrimSpec
{
  std::string name;
  std::vector<std::string> reasons;
};

struct PrimSpecDiff
{
  std::vector<std::string> addedPS;
  std::vector<std::string> modifiedPS;
  std::vector<std::string> deletedPS;
  // Parallel to modifiedPS: per-modified-child reason lists.
  std::vector<ModifiedPrimSpec> modifiedDetails;
};

// Layer (stage) metadata differences (metersPerUnit, upAxis, defaultPrim, ...).
struct LayerMetaDiff
{
  // e.g. "~metersPerUnit", "+customLayerData:foo", "-upAxis".
  std::vector<std::string> changedFields;
  std::vector<PropDiff::ModifiedProp> details;

  bool changed() const { return !changedFields.empty(); }
};

///
/// Instance-flatten canonicalization (in place).
///
/// Flattened scenes emit root-level prototype prims (usdcat
/// `/Flattened_Prototype_N`) whose numbering is non-deterministic / differs
/// tool-to-tool, so a path-based diff reports the SAME content under different
/// names as spurious added/deleted/modified. This rewrites each root-level
/// prototype (a top-level prim that is the target of an intra-layer reference)
/// to a CONTENT-derived canonical name `/__Proto_<hash>` and retargets every
/// reference accordingly. Run on BOTH layers before Diff() so prototypes match
/// by content regardless of numbering. Returns the number of prototypes
/// canonicalized.
///
size_t CanonicalizeInstances(Layer &layer);

///
/// Low-memory / fast diff pre-pass (in place).
///
/// Replaces every attribute default value that is an ARRAY of more than
/// `threshold` elements with a 64-bit content fingerprint (the array's full
/// `pprint_value` hashed), freeing the array payload while preserving the
/// attribute's declared type. The subsequent Diff() then compares fingerprints
/// (one u64) instead of materialized element-wise arrays — cutting peak RSS
/// (~half, when each input is stripped before the other is loaded) and time.
///
/// Trade-off: large arrays are compared EXACTLY by fingerprint, so the ULP /
/// eps tolerance no longer applies to them (it still applies to scalar and
/// small-vector numeric attributes — matrices, quats, colors — which are NOT
/// stripped). Run on a layer that is only consumed by Diff() and then freed.
/// Returns the number of arrays stripped.
///
size_t StripLargeArrays(Layer &layer, size_t threshold = 16);

void Diff(const Layer &lhs, const Layer &rhs,

  /* key = primspec path */
  lightusd::HashMap<std::string, PrimSpecDiff> &psDiffs,

  /* key = primspec path */
  lightusd::HashMap<std::string, PropDiff> &propDiffs,

  const DiffOptions &opts = {},

  /* optional: stage/layer-metadata diff (nullptr to skip reporting) */
  LayerMetaDiff *layerMetaDiff = nullptr);

///
/// Generate text-based diff output similar to 'diff' command
///
std::string DiffToText(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name = "left",
                       const std::string &rhs_name = "right",
                       const DiffOptions &opts = {});

///
/// Generate JSON-based diff output
///
std::string DiffToJSON(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name = "left",
                       const std::string &rhs_name = "right",
                       const DiffOptions &opts = {});

///
/// Given two rendered values that may be long and share a long common prefix
/// (e.g. asset paths), return a pair of display strings centered on the first
/// differing offset, with elided common regions marked by an ellipsis. Makes
/// the actual difference visible instead of truncating both to an identical
/// prefix. If both fit in `window`, they are returned unchanged.
///
std::pair<std::string, std::string> CenterValuePairForDiff(
    const std::string &lhs, const std::string &rhs, size_t window = 240);

} // namespace tydra
} // namespace lightusd
