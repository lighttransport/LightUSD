// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "checker.hh"
namespace lusdchecker {
struct CheckerOptions {
  const lusdchecker::Environment* environment = nullptr;
  std::string input;
  std::string usage_error;
  std::string output = "stdout";
  lightusd::next::ValidationOptions groups = lightusd::next::MakeValidateAllOptions();
  size_t max_memory_mb = 1024;
  bool json = false;
  bool sarif = false;
  std::string baseline;
  std::string profile = "default";
  bool group_selection = false;
  bool dump_rules = false;
  std::vector<std::string> keywords;
  bool all_samples = false;
  size_t max_samples = 10000;
  std::vector<std::string> schema_files, shader_files;
  bool strict = false;
  bool strict_parse = false;
  bool composed = false;
  bool require_all_groups = false;
  bool verbose = false;
  bool usdchecker_compat = false;
  bool root_package_only = false;
  // Variant sweep (usdchecker parity). With --composed the checker validates
  // every combination of authored variant selections (capped) unless
  // --skip-variants; --variants/--variant-sets restrict the sweep.
  bool skip_variants = false;
  bool disable_variant_limit = false;
  bool no_asset_checks = false;
  std::vector<std::string> variant_sets;  // --variant-sets NAME (repeatable)
  // Each --variants occurrence is one validation pass of set:variant pairs.
  std::vector<std::vector<std::pair<std::string, std::string>>>
      variant_selections;
};
std::vector<std::string> Split(const std::string&, char);
std::vector<std::string> KeywordToGroups(const std::string&);
bool SetGroups(const std::string&, lightusd::next::ValidationOptions*, std::string*);
bool NormalizeOptions(CheckerOptions*, std::string*);
int RunConfiguredChecker(CheckerOptions, const lightusd::next::ValidationRegistry&,
                         const Environment* = nullptr);
}
