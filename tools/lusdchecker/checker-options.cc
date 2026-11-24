// SPDX-License-Identifier: Apache-2.0
#include "checker-options.hh"
#include <algorithm>
#include <cctype>
namespace lusdchecker {
using lightusd::ValidationOptions;
using Args = CheckerOptions;
static ValidationOptions AllAvailableGroups() { return lightusd::MakeValidateAllOptions(); }
std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> values;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(separator, start);
    values.emplace_back(text.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return values;
}

// Map an OpenUSD usdchecker validator keyword to lightusd group names, so
// `--include-keywords UsdGeomValidators` style invocations work verbatim.
// Returns an empty list for a non-keyword (regular group name).
std::vector<std::string> KeywordToGroups(const std::string& keyword) {
  std::string lower = keyword;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "usdcorevalidators") return {"core"};
  if (lower == "usdgeomvalidators") return {"geom"};
  if (lower == "usdshadevalidators") return {"shade"};
  if (lower == "usdskelvalidators") return {"geom"};   // skel runs under geom
  if (lower == "usdluxvalidators") return {"lux"};
  if (lower == "usdphysicsvalidators") return {"physics"};
  if (lower == "usdutilsvalidators" || lower == "usdzvalidators")
    return {"package", "crate"};
  if (lower == "usdgeomsubset") return {"geom", "shade"};
  return {};
}

bool SetGroups(const std::string& text, ValidationOptions* groups,
               std::string* error) {
  if (!groups) return false;
  *groups = ValidationOptions();
  groups->core = false;
  std::vector<std::string> names;
  for (const std::string& entry : Split(text, ',')) {
    const std::vector<std::string> mapped = KeywordToGroups(entry);
    if (!mapped.empty()) {
      names.insert(names.end(), mapped.begin(), mapped.end());
    } else {
      names.push_back(entry);
    }
  }
  for (const std::string& name : names) {
    bool* flag = nullptr;
    if (name == "core")
      flag = &groups->core;
    else if (name == "geom")
      flag = &groups->geom;
    else if (name == "shade")
      flag = &groups->shade;
    else if (name == "lux")
      flag = &groups->lux;
    else if (name == "physics")
      flag = &groups->physics;
    else if (name == "render")
      flag = &groups->render;
    else if (name == "package")
      flag = &groups->package;
    else if (name == "crate")
      flag = &groups->crate;
    else if (name == "arkit")
      flag = &groups->arkit;
    else {
      if (error) *error = "unknown or empty validation group: '" + name + "'";
      return false;
    }
    *flag = true;
  }
  return true;
}

bool NormalizeOptions(CheckerOptions* args, std::string* error) {
  if (args->input.empty() && !args->dump_rules) {
    if (error) *error = "an input FILE is required";
    return false;
  }
  // The ARKit profile's package-layout rules (arkit.package.fileExtension /
  // .rootLayer) live in the package container check, which only runs when the
  // package group reads the raw bytes. Selecting `arkit` therefore implies
  // `package`, so `-g arkit` on a .usdz enforces the ARKit package rules rather
  // than silently skipping them while still reporting arkit as checked. (The
  // `--arkit` flag already sets this explicitly.)
  // `aousd-core-1.1.0` is current; `aousd-core-1.0.1` stays as a compatible
  // alias (same normative check set) for existing scripts.
  if (args->profile != "default" && args->profile != "strict" &&
      args->profile != "aousd-core-1.1.0" && args->profile != "aousd-core-1.0.1") {
    *error = "unknown validation profile: " + args->profile;
    return false;
  }
  if (args->profile != "default") {
    if (args->group_selection || args->root_package_only || args->skip_variants ||
        !args->variant_sets.empty() || !args->variant_selections.empty() || args->no_asset_checks) {
      *error = "full validation profiles cannot be combined with coverage-reducing options";
      return false;
    }
    const bool arkit = args->groups.arkit;
    args->groups = args->profile == "strict" ? AllAvailableGroups() : ValidationOptions();
    args->groups.package = true;
    args->groups.crate = true;
    args->groups.arkit = arkit;
    args->composed = true;
    args->strict_parse = true;
    args->all_samples = true;
    args->strict = args->profile == "strict";
    args->groups.require_complete = args->strict;
    args->groups.normative_only = args->profile == "aousd-core-1.1.0" ||
                                  args->profile == "aousd-core-1.0.1";
    if (args->groups.normative_only && arkit) {
      *error = "AOUSD Core profile cannot include the ARKit delivery profile";
      return false;
    }
    args->groups.stage_presence_checks = !args->groups.normative_only;
    args->groups.asset_checks = !args->groups.normative_only;
  }
  if (args->groups.arkit) args->groups.package = true;
  // Applied after the loop: --groups/--all/--core-only rebuild the options
  // struct, which would otherwise silently discard an earlier
  // --no-asset-checks depending on flag order.
  args->groups.asset_checks = !args->no_asset_checks && !args->groups.normative_only;
  args->groups.validator_keywords = args->keywords;
  return true;
}
}
