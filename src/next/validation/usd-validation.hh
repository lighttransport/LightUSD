// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
//
// LightUSD Next - AOUSD Core semantic validation.
//
// Port of the legacy validator (src/usd-validation.{hh,cc}) onto the next-core
// layer model (next::Layer / next::PrimSpec). Rule ids, severities and message
// wording mirror the legacy validator so reports are interchangeable between
// the two paths. Checks that depend on legacy-only typed schema reconstruction
// are approximated structurally; each such divergence is noted with a comment
// in usd-validation.cc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../layer/layer.hh"

namespace lightusd {
namespace next {

enum class USDValidationSeverity {
  Error,
  Warning,
};

// Selects which rule groups to run.
//
// `core` rules (default on) mirror the legacy AOUSD Core behavior: layer
// metadata, prim-name/xformOp integrity, composition metadata, CollectionAPI,
// ColorSpace, clips, attribute/relationship metadata.
//
// `geom`, `shade`, `lux`, and `physics` are modeled on OpenUSD `usdchecker`
// and USD schema constraints. They are warning-heavy and composition-
// sensitive, so they are off by default and must be opted into explicitly.
//
// `package` and `crate` select byte-container checks. Layer-only validation
// leaves them unchecked; a caller retaining the original bytes can run and
// merge those checks.
//
// `arkit` is the ARKit / RealityKit USDZ compatibility profile, modeled on
// OpenUSD `usdchecker --arkit` (see doc/openusd-usdz.md). It is a delivery
// profile rather than a defect class: a perfectly valid USD layer can be
// non-ARKit (e.g. Z-up, a BasisCurves prim, a .usdz payload with a .tif
// texture). It is therefore NOT enabled by MakeValidateAllOptions() and must
// be requested explicitly.
struct ValidationOptions {
  bool core{true};
  bool geom{false};
  bool shade{false};
  bool lux{false};
  bool physics{false};
  bool render{false};
  bool package{false};
  bool crate{false};
  bool arkit{false};

  // Not a group: gates the "referenceable asset" presence rule
  // (core.layer.defaultPrim.missing), mirroring OpenUSD usdchecker's
  // --noAssetChecks. The geom-group stage-metadata presence rules
  // (geom.stage.upAxis / geom.stage.metersPerUnit) are NOT gated by this,
  // matching usdchecker (its --noAssetChecks keeps those on).
  bool asset_checks{true};
  // Stage-metadata PRESENCE rules (geom.stage.upAxis / geom.stage.metersPerUnit
  // / core.layer.defaultPrim.missing). usdchecker resolves these against the
  // stage ROOT layer only, so a caller validating a COMPOSED layer (whose metas
  // may have merged sublayer opinions) disables them here and calls
  // ValidateStageMetadataPresence on the authored root layer instead.
  bool stage_presence_checks{true};

  // Human-readable list of enabled groups, e.g. "core, geom, shade".
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

// Convenience option builder for callers that want CLI-compatible behavior.
// Enables every defect-class group (core/geom/shade/lux/physics/package/crate)
// but leaves `arkit` off; ARKit is an opt-in delivery profile.
ValidationOptions MakeValidateAllOptions();

// Stable group-name order for structured reports ("core", "geom", "shade",
// "lux", "physics", "package", "crate", "arkit").
std::vector<std::string> GetValidationGroupNames(
    const ValidationOptions &options);

// Deterministic presentation order for validation issues: errors before
// warnings, layer-scoped issues first, then location/rule id.
std::vector<const USDValidationIssue *> GetOrderedValidationIssues(
    const USDValidationResult &result);

// Validate an uncomposed Layer. The no-options overload runs `core` rules
// only, matching the legacy default.
USDValidationResult ValidateLayerAgainstAOUSDCore(const Layer &layer);
USDValidationResult ValidateLayerAgainstAOUSDCore(
    const Layer &layer, const ValidationOptions &options);

// Validate a USD payload from memory: parses via pcp::LoadLayerFromMemory
// (content-dispatch to the next USDA / USDC / USDZ readers), then runs the
// Layer rules. Returns false only when the input could not be parsed as a
// Layer (parse diagnostics land in *err). Warning-only results still return
// true and have USDValidationResult::ok().
bool ValidateUSDFromMemoryAgainstAOUSDCore(
    const uint8_t *data, size_t size, const std::string &filename,
    const ValidationOptions &options, USDValidationResult *result,
    std::string *warn, std::string *err);

void MergeValidationResults(USDValidationResult *dst,
                            const USDValidationResult &src);

// One row of the rule registry: a stable rule identifier, the group that
// emits it, and a one-line description. Powers `lusdchecker --dump-rules`
// (the counterpart of `usdchecker --dumpRules`).
struct ValidationRuleInfo {
  const char *id;
  const char *group;
  const char *doc;
};

// The full rule registry, ordered by (group, id).
// Borrowed immutable records with process lifetime; count may be null.
const ValidationRuleInfo* GetValidationRuleTable(size_t* count);

// usdchecker compatibility: OpenUSD reports several of these findings as
// ERRORS where the lightusd defaults use warnings (and the presence rules
// default to warnings). Applying this to a result upgrades the mapped rules
// to usdchecker's severities so exit-code behavior matches
// `usdchecker <file>`. Idempotent.
void ApplyUsdcheckerCompatSeverities(USDValidationResult *result);

// Stage-metadata presence rules against a ROOT layer's own metas (see
// ValidationOptions::stage_presence_checks).
void ValidateStageMetadataPresence(const Layer &layer,
                                   const ValidationOptions &options,
                                   USDValidationResult *result);

std::string FormatValidationResult(const USDValidationResult &result);

}  // namespace next
}  // namespace lightusd
