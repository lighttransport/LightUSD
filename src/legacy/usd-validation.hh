// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 Light Transport Entertainment Inc.
//
// AOUSD Core semantic validation helpers.
//
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/layer-types.hh"
#include "layer.hh"

namespace lightusd {

struct USDLoadOptions;

enum class USDValidationSeverity {
  Error,
  Warning,
};

// Selects which rule groups to run.
//
// `core` rules (default on) preserve the historical AOUSD Core behavior:
// layer metadata, prim-name/xformOp integrity, CollectionAPI, ColorSpace.
//
// `geom`, `shade`, `lux`, and `physics` rules are modeled on OpenUSD
// `usdchecker` and USD schema constraints. They are warning-heavy and
// composition-sensitive, so they are off by default and must be opted into
// explicitly.
struct ValidationOptions {
  bool core{true};
  bool geom{false};
  bool shade{false};
  bool lux{false};
  bool physics{false};
  bool crate{false};

  // Human-readable list of enabled groups, e.g.
  // "core, geom, shade, lux, physics".
  std::string group_summary() const;
};

struct USDValidationIssue {
  USDValidationSeverity severity{USDValidationSeverity::Error};
  std::string rule_id;
  std::string location;
  std::string message;
};

struct USDValidationResult {
  std::vector<USDValidationIssue> issues;

  // The rule groups that were actually run to produce this result, so a report
  // can state coverage (a core-only "OK" did not check geom/shade).
  ValidationOptions checked_groups;

  size_t error_count() const;
  size_t warning_count() const;
  bool ok() const;
};

const char *GetAOUSDCoreSpecVersionString();

// Convenience option builders for callers that want CLI-compatible behavior.
ValidationOptions MakeValidateAllOptions();
ValidationOptions MakeContainerValidationOptions(
    const ValidationOptions &options);

// Stable group-name order for structured reports.
std::vector<std::string> GetValidationGroupNames(
    const ValidationOptions &options);

// Deterministic presentation order for validation issues: errors before
// warnings, layer-scoped issues first, then location/rule id.
std::vector<const USDValidationIssue *> GetOrderedValidationIssues(
    const USDValidationResult &result);

// Validate an uncomposed Layer. The no-options overload runs `core` rules only,
// preserving the original behavior.
USDValidationResult ValidateLayerAgainstAOUSDCore(const Layer &layer);
USDValidationResult ValidateLayerAgainstAOUSDCore(const Layer &layer,
                                                  const ValidationOptions &options);

// Validate source-format/container details that are not represented in Layer.
// Currently covers USDC Crate structure for .usdc inputs and .usdc members in
// USDZ packages. Callers should merge this with ValidateLayerAgainstAOUSDCore.
USDValidationResult ValidateUSDContainerAgainstAOUSDCore(
    const uint8_t *addr, size_t length, const std::string &filename,
    const ValidationOptions &options);
USDValidationResult ValidateUSDContainerFileAgainstAOUSDCore(
    const std::string &filename, const ValidationOptions &options,
    std::string *warn, std::string *err,
    size_t max_file_size_in_mb = 16384ull);

// Validate a USD payload or file using the same orchestration as `lusdcat`
// validation: load as Layer, run Layer rules, then merge format/container
// checks. Returns false only when the input could not be parsed as a Layer.
// Warning-only results still return true and have USDValidationResult::ok().
// checked_groups.crate is set only when USDC/USDZ container rules actually ran.
bool ValidateUSDFromMemoryAgainstAOUSDCore(
    const uint8_t *addr, size_t length, const std::string &filename,
    const ValidationOptions &options, const USDLoadOptions &load_options,
    USDValidationResult *result, std::string *warn, std::string *err);
bool ValidateUSDFileAgainstAOUSDCore(
    const std::string &filename, const ValidationOptions &options,
    const USDLoadOptions &load_options, USDValidationResult *result,
    std::string *warn, std::string *err);

void MergeValidationResults(USDValidationResult *dst,
                            const USDValidationResult &src);

std::string FormatValidationResult(const USDValidationResult &result);

}  // namespace lightusd
