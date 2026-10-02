#include "next/reader/usdz-reader.hh"
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
//
// lusdchecker - dependency-free AOUSD Core and USD schema validator.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "next/composition/composition.hh"
#include "next/crate/crate-reader.hh"
#include "next/pcp/layer-registry.hh"
#include "next/reader/usdz-reader.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/validation/usd-validation.hh"
#include "report.hh"
#include "checker.hh"
#include "next/layer/asset-anchor.hh"
#include <deque>
#include <set>

namespace {

using lightusd::next::GetAOUSDCoreSpecVersionString;
using lightusd::next::GetOrderedValidationIssues;
using lightusd::next::GetValidationGroupNames;
using lightusd::next::Layer;
using lightusd::next::USDValidationIssue;
using lightusd::next::USDValidationResult;
using lightusd::next::USDValidationSeverity;
using lightusd::next::ValidationOptions;

constexpr int kExitValid = 0;
constexpr int kExitInvalid = 1;
constexpr int kExitError = 2;
constexpr size_t kDefaultMaxMemoryMb = 1024;

struct Args {
  std::string input;
  std::string output = "stdout";
  ValidationOptions groups;
  size_t max_memory_mb = kDefaultMaxMemoryMb;
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

constexpr size_t kVariantValidationLimit = 1000;

ValidationOptions AllAvailableGroups() {
  return lightusd::next::MakeValidateAllOptions();
}

void PrintUsage(std::ostream& os) {
  os << "Usage: lusdchecker [options] FILE\n"
        "\n"
        "Validate USDA, USDC, USDZ, or MaterialX (.mtlx) against AOUSD Core\n"
        "1.0.1 and LightUSD's structural schema rules. FILE may be '-' for\n"
        "stdin.\n"
        "\n"
        "Options:\n"
        "  -g, --groups LIST     Comma-separated rule groups: "
        "core,geom,shade,\n"
        "                        lux,physics,render,package,crate,arkit (default: "
        "all\n"
        "                        defect-class groups; arkit is opt-in)\n"
        "      --core-only       Run AOUSD Core rules only\n"
        "      --all             Run all defect-class rule groups (default)\n"
        "      --arkit           ARKit/RealityKit USDZ profile "
        "(arkit + core,\n"
        "                        geom,shade,package), like usdchecker "
        "--arkit\n"
        "  -t, --strict          Treat validation and parser warnings as "
        "failure\n"
        "      --profile NAME    default, strict, or aousd-core-1.0.1\n"
        "      --all-time-samples Validate coherent values at all sample times\n"
        "      --max-samples N   Maximum sampled times (default: 10000)\n"
        "      --schema-definitions FILE  Load schema JSON manifest (repeatable)\n"
        "      --shader-definitions FILE  Load shader JSON manifest (repeatable)\n"
        "      --strict-parse    Reject non-conforming/unsupported format "
        "data\n"
        "      --composed        Compose external arcs and validate the "
        "flattened stage\n"
        "      --require-all-groups\n"
        "                        Fail if a requested group is inapplicable\n"
        "      --usdchecker-compat\n"
        "                        Report usdchecker-mapped rules at OpenUSD\n"
        "                        usdchecker severities (several warnings\n"
        "                        become errors)\n"
        "      --no-asset-checks Skip referenceable-asset checks, like\n"
        "                        usdchecker --noAssetChecks\n"
        "  -p, --root-package-only\n"
        "                        Do not follow or validate dependencies\n"
        "                        outside the given file/package\n"
        "  -s, --skip-variants   With --composed, validate only the\n"
        "                        authored variant selections instead of\n"
        "                        every combination\n"
        "      --variants LIST   ','-separated set:variant pairs checked\n"
        "                        together; repeat for separate passes\n"
        "                        (implies --composed)\n"
        "      --variant-sets NAME\n"
        "                        Validate every variant of the named set\n"
        "                        (repeatable; implies --composed)\n"
        "      --disable-variant-validation-limit\n"
        "                        Lift the 1000-combination sweep cap\n"
        "  -d, --dump-rules      Dump the rule registry (id, group, doc)\n"
        "  -v, --verbose         Report per-pass progress on stderr\n"
        "      --json            Emit stable machine-readable JSON\n"
        "      --sarif           Emit SARIF 2.1.0\n"
        "      --baseline FILE   Fail only on new findings versus a JSON report\n"
        "  -o, --out FILE        Write report to FILE, stdout, or stderr\n"
        "      --max-memory-mb N Bound input/parser memory (default: 1024)\n"
        "  -h, --help            Show this help\n"
        "      --version         Show validator/spec version\n"
        "      --list-groups     List rule groups and their coverage\n"
        "\n"
        "Exit status: 0 valid, 1 validation failed, 2 usage/I/O/parse error.\n";
}

bool ParseSize(const std::string& text, size_t* value) {
  if (!value || text.empty() || text[0] == '-') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || !end || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *value = static_cast<size_t>(parsed);
  return true;
}

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

enum class ParseArgsResult { Run, ExitSuccess, Error };

ParseArgsResult ParseArgs(int argc, char** argv, Args* args,
                          std::string* error) {
  if (!args) return ParseArgsResult::Error;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto next_value = [&](const char* option, std::string* value) {
      if (i + 1 >= argc) {
        if (error) *error = std::string("missing value for ") + option;
        return false;
      }
      *value = argv[++i];
      return true;
    };
    if (arg == "-h" || arg == "--help") {
      PrintUsage(std::cout);
      return ParseArgsResult::ExitSuccess;
    } else if (arg == "--version") {
      std::cout << "lusdchecker 0.1 (" << GetAOUSDCoreSpecVersionString()
                << ")\n";
      return ParseArgsResult::ExitSuccess;
    } else if (arg == "--list-groups") {
      std::cout
          << "core      AOUSD layer, composition, metadata, and value rules\n"
          << "geom      UsdGeom, UsdSkel, and UsdVol structural rules\n"
          << "shade     UsdShade, preview-surface, and MaterialX rules\n"
          << "lux       UsdLux structural and value rules\n"
          << "physics   UsdPhysics placement, joint, and extension rules\n"
          << "render    UsdRender settings/product/var structural rules\n"
          << "package   USDZ layout, path, portability, and dependency rules\n"
          << "crate     USDC decode and cross-table structural rules\n"
          << "arkit     ARKit/RealityKit USDZ delivery profile (opt-in; not "
             "part of --all)\n";
      return ParseArgsResult::ExitSuccess;
    } else if (arg == "--profile") {
      if (!next_value("--profile", &args->profile)) return ParseArgsResult::Error;
    } else if (arg == "--all-time-samples") {
      args->all_samples = true;
    } else if (arg == "--max-samples") {
      std::string value;
      if (!next_value("--max-samples", &value) || !ParseSize(value, &args->max_samples)) {
        *error = "--max-samples requires a positive integer";
        return ParseArgsResult::Error;
      }
    } else if (arg == "--schema-definitions" || arg == "--shader-definitions") {
      std::string value;
      if (!next_value(arg.c_str(), &value)) return ParseArgsResult::Error;
      (arg == "--schema-definitions" ? args->schema_files : args->shader_files).push_back(value);
    } else if (arg == "--json") {
      args->json = true;
    } else if (arg == "--sarif") {
      args->sarif = true;
    } else if (arg == "--baseline") {
      if (!next_value("--baseline", &args->baseline) || args->baseline.empty())
        return ParseArgsResult::Error;
    } else if (arg == "-t" || arg == "--strict") {
      args->strict = true;
    } else if (arg == "--strict-parse") {
      args->strict_parse = true;
    } else if (arg == "--composed") {
      args->composed = true;
    } else if (arg == "--require-all-groups") {
      args->require_all_groups = true;
    } else if (arg == "--usdchecker-compat") {
      args->usdchecker_compat = true;
    } else if (arg == "--no-asset-checks" || arg == "--noAssetChecks") {
      args->no_asset_checks = true;
    } else if (arg == "-p" || arg == "--root-package-only" ||
               arg == "--rootPackageOnly") {
      args->root_package_only = true;
    } else if (arg == "-s" || arg == "--skip-variants" ||
               arg == "--skipVariants") {
      args->skip_variants = true;
    } else if (arg == "--disable-variant-validation-limit" ||
               arg == "--disableVariantValidationLimit") {
      args->disable_variant_limit = true;
    } else if (arg == "-v" || arg == "--verbose") {
      args->verbose = true;
    } else if (arg == "-d" || arg == "--dump-rules" || arg == "--dumpRules") {
      args->dump_rules = true;
    } else if (arg == "--variants") {
      std::string value;
      if (!next_value(arg.c_str(), &value)) {
        return ParseArgsResult::Error;
      }
      std::vector<std::pair<std::string, std::string>> pairs;
      for (const std::string& entry : Split(value, ',')) {
        const size_t colon = entry.find(':');
        if (colon == std::string::npos || colon == 0 ||
            colon + 1 >= entry.size()) {
          if (error) {
            *error = "--variants entries must be set:variant pairs, got '" +
                     entry + "'";
          }
          return ParseArgsResult::Error;
        }
        pairs.emplace_back(entry.substr(0, colon), entry.substr(colon + 1));
      }
      args->variant_selections.push_back(std::move(pairs));
      args->composed = true;
    } else if (arg == "--variant-sets" || arg == "--variantSets") {
      std::string value;
      if (!next_value(arg.c_str(), &value)) {
        return ParseArgsResult::Error;
      }
      for (const std::string& name : Split(value, ',')) {
        if (!name.empty()) args->variant_sets.push_back(name);
      }
      args->composed = true;
    } else if (arg == "--include-keywords" || arg == "--includeKeywords") {
      args->group_selection = true;
      std::string value;
      if (!next_value(arg.c_str(), &value)) return ParseArgsResult::Error;
      args->keywords = Split(value, ',');
      args->groups = ValidationOptions(); args->groups.core = false;
      std::string groups;
      for (const auto& keyword : args->keywords) {
        if (keyword.empty()) { *error = "empty validator keyword"; return ParseArgsResult::Error; }
        for (const auto& group : KeywordToGroups(keyword)) {
          if (!groups.empty()) groups += ',';
          groups += group;
        }
      }
      if (!groups.empty() && !SetGroups(groups, &args->groups, error)) return ParseArgsResult::Error;
    } else if (arg == "--core-only") {
      args->group_selection = true;
      args->groups = ValidationOptions();
    } else if (arg == "--all") {
      args->groups = AllAvailableGroups();
    } else if (arg == "--arkit") {
      // The ARKit profile: the ARKit-only rules plus the base rule groups that
      // OpenUSD's `usdchecker --arkit` always runs (stage metadata, prim
      // encapsulation, textures, material bindings, package layout).
      args->groups.arkit = true;
      args->groups.core = true;
      args->groups.geom = true;
      args->groups.shade = true;
      args->groups.package = true;
    } else if (arg == "-g" || arg == "--groups") {
      args->group_selection = true;
      std::string value;
      if (!next_value(arg.c_str(), &value) ||
          !SetGroups(value, &args->groups, error)) {
        return ParseArgsResult::Error;
      }
    } else if (arg == "-o" || arg == "--out") {
      if (!next_value(arg.c_str(), &args->output)) {
        return ParseArgsResult::Error;
      }
    } else if (arg == "--max-memory-mb") {
      std::string value;
      if (!next_value(arg.c_str(), &value) ||
          !ParseSize(value, &args->max_memory_mb)) {
        if (error && error->empty()) {
          *error = "--max-memory-mb requires a positive integer";
        }
        return ParseArgsResult::Error;
      }
    } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
      if (error) *error = "unknown option: " + arg;
      return ParseArgsResult::Error;
    } else if (!args->input.empty()) {
      if (error) *error = "only one input file may be specified";
      return ParseArgsResult::Error;
    } else {
      args->input = arg;
    }
  }
  if (args->input.empty() && !args->dump_rules) {
    if (error) *error = "an input FILE is required";
    return ParseArgsResult::Error;
  }
  // The ARKit profile's package-layout rules (arkit.package.fileExtension /
  // .rootLayer) live in the package container check, which only runs when the
  // package group reads the raw bytes. Selecting `arkit` therefore implies
  // `package`, so `-g arkit` on a .usdz enforces the ARKit package rules rather
  // than silently skipping them while still reporting arkit as checked. (The
  // `--arkit` flag already sets this explicitly.)
  if (args->profile != "default" && args->profile != "strict" &&
      args->profile != "aousd-core-1.0.1") {
    *error = "unknown validation profile: " + args->profile;
    return ParseArgsResult::Error;
  }
  if (args->profile != "default") {
    if (args->group_selection || args->root_package_only || args->skip_variants ||
        !args->variant_sets.empty() || !args->variant_selections.empty() || args->no_asset_checks) {
      *error = "full validation profiles cannot be combined with coverage-reducing options";
      return ParseArgsResult::Error;
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
    args->groups.normative_only = args->profile == "aousd-core-1.0.1";
    if (args->groups.normative_only && arkit) {
      *error = "AOUSD Core profile cannot include the ARKit delivery profile";
      return ParseArgsResult::Error;
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
  return ParseArgsResult::Run;
}

std::string JsonEscape(const std::string& text) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '\"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xf]);
          out.push_back(kHex[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

void WriteJsonString(std::ostream& os, const std::string& value) {
  os << '\"' << JsonEscape(value) << '\"';
}

std::string JsonReport(const Args& args, const USDValidationResult& result,
                       const std::string& parser_warnings, bool valid,
                       size_t variant_pass_count, bool variant_limit_hit) {
  std::ostringstream os;
  os << "{\n  \"tool\":\"lusdchecker\",\n  \"spec\":";
  WriteJsonString(os, GetAOUSDCoreSpecVersionString());
  os << ",\n  \"input\":";
  WriteJsonString(os, args.input);
  os << ",\n  \"valid\":" << (valid ? "true" : "false")
     << ",\n  \"strict\":" << (args.strict ? "true" : "false")
     << ",\n  \"strictParse\":" << (args.strict_parse ? "true" : "false")
     << ",\n  \"composed\":" << (args.composed ? "true" : "false")
     << ",\n  \"usdcheckerCompat\":"
     << (args.usdchecker_compat ? "true" : "false")
     << ",\n  \"assetChecks\":" << (args.no_asset_checks ? "false" : "true")
     << ",\n  \"rootPackageOnly\":"
     << (args.root_package_only ? "true" : "false")
     << ",\n  \"variantPasses\":" << variant_pass_count
     << ",\n  \"variantLimitHit\":"
     << (variant_limit_hit ? "true" : "false")
     << ",\n  \"requireAllGroups\":"
     << (args.require_all_groups ? "true" : "false")
     << ",\n  \"requestedGroups\":[";
  const std::vector<std::string> requested =
      GetValidationGroupNames(args.groups);
  for (size_t i = 0; i < requested.size(); ++i) {
    if (i) os << ',';
    WriteJsonString(os, requested[i]);
  }
  os << "],\n  \"checkedGroups\":[";
  const std::vector<std::string> groups =
      GetValidationGroupNames(result.checked_groups);
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i) os << ',';
    WriteJsonString(os, groups[i]);
  }
  os << "],\n  \"skippedGroups\":[";
  size_t skipped_count = 0;
  for (const std::string& name : requested) {
    if (std::find(groups.begin(), groups.end(), name) != groups.end()) continue;
    if (skipped_count++) os << ',';
    WriteJsonString(os, name);
  }
  os << "],\n  \"errorCount\":" << result.error_count()
     << ",\n  \"warningCount\":" << result.warning_count()
     << ",\n  \"parserWarnings\":";
  WriteJsonString(os, parser_warnings);
  os << ",\n  \"issues\":[";
  const auto issues = GetOrderedValidationIssues(result);
  for (size_t i = 0; i < issues.size(); ++i) {
    const USDValidationIssue& issue = *issues[i];
    if (i) os << ',';
    os << "\n    {\"severity\":";
    WriteJsonString(os, issue.severity == USDValidationSeverity::Error
                            ? "error"
                            : "warning");
    os << ",\"ruleId\":";
    WriteJsonString(os, issue.rule_id);
    os << ",\"location\":";
    WriteJsonString(os, issue.location);
    os << ",\"message\":";
    WriteJsonString(os, issue.message);
    os << '}';
  }
  if (!issues.empty()) os << '\n';
  os << "  ]\n}\n";
  return os.str();
}

lightusd::minijson::Value MakeReport(const Args& args,
    const USDValidationResult& result, const std::string& warnings,
    bool valid, size_t passes, bool limit) {
  using Json = lightusd::minijson::Value;
  Json report;
  lightusd::minijson::Parse(JsonReport(args, result, warnings, valid, passes, limit), &report);
  report["reportVersion"] = 2;
  report["profile"] = args.profile;
  report["conformanceScope"] = "implemented AOUSD Core 1.0.1 document constraints";
  report["complete"] = result.complete && !limit;
  report["executionSuccessful"] = true;
  report["allTimeSamples"] = args.all_samples;
  report["maxSamples"] = uint64_t(args.max_samples);
  report["referenceRevision"] = "2095fafafd033fa23386d7ec6d58c7cc33974518";
  bool normative_failure = false;
  const auto ordered = GetOrderedValidationIssues(result);
  for (size_t i = 0; i < ordered.size(); ++i) {
    const auto& issue = *ordered[i];
    auto meta = lightusd::next::GetValidationRuleMetadata(issue.rule_id);
    if (args.groups.registry) for (const auto& rule : args.groups.registry->validators())
      if (rule.id == issue.rule_id) meta = rule.metadata;
    Json& row = report["issues"][i];
    row["category"] = meta.category;
    row["specification"] = meta.specification;
    row["referenceErrors"] = Json::array();
    for (const auto& reference : meta.reference_errors) row["referenceErrors"].push_back(reference);
    row["sourceAsset"] = issue.source_asset.empty() ? args.input : issue.source_asset;
    row["variants"] = issue.variants;
    if (issue.has_time) row["time"] = issue.time;
    normative_failure |= meta.category == "normative" && issue.severity == USDValidationSeverity::Error;
  }
  report["conformance"] = args.profile == "default" ? "notRequested" :
      normative_failure ? "failed" : !report["complete"].get_bool() ? "incomplete" : "passed";
  return report;
}

int ReportError(const Args& args, const std::string& rule, const std::string& message) {
  USDValidationResult result;
  result.checked_groups.core = false;
  result.complete = false;
  result.issues.push_back({USDValidationSeverity::Error, rule, "<input>", message});
  auto report = MakeReport(args, result, "", false, 0, false);
  report["executionSuccessful"] = false;
  report["gatePassed"] = false;
  report["newIssueCount"] = uint64_t(1);
  report["existingIssueCount"] = uint64_t(0);
  if (rule == "parser.error" && args.profile != "default") report["conformance"] = "failed";
  std::ofstream file;
  std::ostream* output = args.output == "stderr" ? &std::cerr : &std::cout;
  if (args.output != "stdout" && args.output != "stderr") {
    file.open(args.output);
    if (!file) { std::cerr << "cannot write report: " << args.output << '\n'; return kExitError; }
    output = &file;
  }
  if (args.json || args.sarif)
    *output << (args.sarif ? lusdchecker::ToSarif(report).dump(2) : report.dump()) << '\n';
  else *output << "lusdchecker: " << rule << ": " << message << '\n';
  return kExitError;
}

bool ReadStdin(size_t limit, std::string* data, std::string* error) {
  if (!data) return false;
  char buffer[64 * 1024];
  while (std::cin) {
    std::cin.read(buffer, sizeof(buffer));
    const std::streamsize count = std::cin.gcount();
    if (count > 0) {
      const size_t n = static_cast<size_t>(count);
      if (data->size() > limit || n > limit - data->size()) {
        if (error) *error = "stdin exceeds --max-memory-mb limit";
        return false;
      }
      data->append(buffer, n);
    }
  }
  if (!std::cin.eof()) {
    if (error) *error = "failed while reading stdin";
    return false;
  }
  return true;
}

bool ReadFile(const std::string& filename, size_t limit, std::string* data,
              std::string* error) {
  if (!data) return false;
  std::ifstream stream(filename, std::ios::binary | std::ios::ate);
  if (!stream) {
    if (error) *error = "cannot open input file '" + filename + "'";
    return false;
  }
  const std::streampos end = stream.tellg();
  if (end < std::streampos(0) || static_cast<uint64_t>(end) > limit) {
    if (error) *error = "input exceeds --max-memory-mb limit";
    return false;
  }
  data->resize(static_cast<size_t>(end));
  stream.seekg(0);
  if (!data->empty() &&
      !stream.read(&(*data)[0], static_cast<std::streamsize>(data->size()))) {
    if (error) *error = "failed while reading input file";
    return false;
  }
  return true;
}

void AddIssue(USDValidationResult* result, USDValidationSeverity severity,
              const std::string& rule, const std::string& location,
              const std::string& message) {
  if (!result) return;
  USDValidationIssue issue;
  issue.severity = severity;
  issue.rule_id = rule;
  issue.location = location;
  issue.message = message;
  result->issues.push_back(std::move(issue));
}

bool ReadU16(const uint8_t* bytes, size_t size, size_t pos, uint16_t* out) {
  if (!out || pos > size || size - pos < 2) return false;
  *out = static_cast<uint16_t>(bytes[pos]) |
         static_cast<uint16_t>(bytes[pos + 1] << 8);
  return true;
}

bool ReadU32(const uint8_t* bytes, size_t size, size_t pos, uint32_t* out) {
  if (!out || pos > size || size - pos < 4) return false;
  *out = static_cast<uint32_t>(bytes[pos]) |
         (static_cast<uint32_t>(bytes[pos + 1]) << 8) |
         (static_cast<uint32_t>(bytes[pos + 2]) << 16) |
         (static_cast<uint32_t>(bytes[pos + 3]) << 24);
  return true;
}

struct PackageEntry {
  std::string name;
  size_t local_header_offset = 0;
  size_t data_offset = 0;
  size_t size = 0;
  size_t uncompressed_size = 0;
  uint32_t crc = 0;
  uint16_t flags = 0;
  uint16_t compression = 0;
};

uint32_t ComputeCRC32(const uint8_t* data, size_t size) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < values.size(); ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? 0xedb88320u ^ (value >> 1) : value >> 1;
      }
      values[i] = value;
    }
    return values;
  }();
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

std::string LowerExtension(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return std::string();
  }
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext;
}

bool IsUnsafePackagePath(const std::string& path) {
  if (path.empty() || path[0] == '/' || path[0] == '\\' ||
      path.find('\\') != std::string::npos) {
    return true;
  }
  for (const std::string& part : Split(path, '/')) {
    if (part.empty() || part == "." || part == "..") return true;
  }
  return false;
}

std::string NormalizePackagePath(const std::string& root_name,
                                 const std::string& asset_path) {
  if (asset_path.empty() || asset_path[0] == '/' ||
      asset_path.find("//") == 0) {
    return std::string();
  }
  std::vector<std::string> parts;
  const size_t slash = root_name.find_last_of('/');
  if (slash != std::string::npos) {
    parts = Split(root_name.substr(0, slash), '/');
  }
  std::string path = asset_path;
  std::replace(path.begin(), path.end(), '\\', '/');
  for (const std::string& part : Split(path, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (parts.empty()) return std::string();
      parts.pop_back();
    } else {
      parts.push_back(part);
    }
  }
  std::string normalized;
  for (const std::string& part : parts) {
    if (!normalized.empty()) normalized += '/';
    normalized += part;
  }
  return normalized;
}

// ARKit (usdchecker --arkit, ARKitFileExtensionChecker): a package may only
// contain USD layers and the portable image formats.
bool IsArkitPackageExtension(const std::string& ext) {
  static const std::unordered_set<std::string> kAllowed = {
      "usd", "usda", "usdc", "usdz", "exr", "jpg", "jpeg", "png"};
  return kAllowed.count(ext) != 0;
}

std::vector<PackageEntry> ParsePackageEntries(const uint8_t* bytes, size_t size,
                                              bool arkit,
                                              USDValidationResult* result) {
  std::vector<PackageEntry> entries;
  std::unordered_set<std::string> names;
  size_t pos = 0;
  while (pos <= size && size - pos >= 4) {
    uint32_t signature = 0;
    if (!ReadU32(bytes, size, pos, &signature) || signature != 0x04034b50u) {
      break;
    }
    if (size - pos < 30) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.structure.header", "<package>",
               "truncated ZIP local-file header");
      break;
    }
    uint16_t flags = 0, compression = 0, name_size = 0, extra_size = 0;
    uint32_t crc = 0, compressed_size = 0, uncompressed_size = 0;
    ReadU16(bytes, size, pos + 6, &flags);
    ReadU16(bytes, size, pos + 8, &compression);
    ReadU32(bytes, size, pos + 14, &crc);
    ReadU32(bytes, size, pos + 18, &compressed_size);
    ReadU32(bytes, size, pos + 22, &uncompressed_size);
    ReadU16(bytes, size, pos + 26, &name_size);
    ReadU16(bytes, size, pos + 28, &extra_size);
    const size_t variable_size = static_cast<size_t>(name_size) + extra_size;
    if (variable_size > size - pos - 30) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.structure.header", "<package>",
               "ZIP entry name or extra field exceeds archive bounds");
      break;
    }
    const size_t data_offset = pos + 30 + variable_size;
    if (static_cast<size_t>(compressed_size) > size - data_offset) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.structure.bounds", "<package>",
               "ZIP entry payload exceeds archive bounds");
      break;
    }
    PackageEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(bytes + pos + 30),
                      name_size);
    entry.local_header_offset = pos;
    entry.data_offset = data_offset;
    entry.size = compressed_size;
    entry.uncompressed_size = uncompressed_size;
    entry.crc = crc;
    entry.flags = flags;
    entry.compression = compression;
    const std::string location = "<package>[" + entry.name + "]";
    if (flags & 0x0001u) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.encryption", location,
               "USDZ entries must not be encrypted");
    }
    if (flags & 0x0008u) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.dataDescriptor", location,
               "USDZ entries must carry sizes in the local-file header");
    }
    if (compression != 0) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.compression", location,
               "USDZ entries must use ZIP store mode (no compression)");
    } else if (compressed_size != uncompressed_size) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.size", location,
               "stored ZIP entry has differing compressed and uncompressed sizes");
    } else if (ComputeCRC32(bytes + data_offset, compressed_size) != crc) {
      AddIssue(result, USDValidationSeverity::Error, "package.entry.crc",
               location, "entry payload does not match its local-header CRC-32");
    }
    if ((data_offset & 63u) != 0u) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.alignment", location,
               "USDZ entry data must begin on a 64-byte boundary");
    }
    if (IsUnsafePackagePath(entry.name)) {
      AddIssue(result, USDValidationSeverity::Error, "package.entry.path",
               location, "package entry name is not a safe relative path");
    }
    if (!names.insert(entry.name).second) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.entry.duplicate", location,
               "package contains a duplicate entry name");
    }
    static const std::unordered_set<std::string> kPortableExtensions = {
        "usd", "usda", "usdc", "usdz", "png", "jpg", "jpeg", "exr",
        "avif", "m4a", "mp3", "wav"};
    const std::string ext = LowerExtension(entry.name);
    if (arkit) {
      // The ARKit set is stricter than the portable set (no avif/audio) and is
      // a hard requirement, so it replaces the portability warning.
      if (!IsArkitPackageExtension(ext)) {
        AddIssue(result, USDValidationSeverity::Error,
                 "arkit.package.fileExtension", location,
                 "package entry `" + entry.name +
                     "` is not an ARKit-supported file type; a USDZ may only "
                     "contain usd/usda/usdc/usdz layers and exr/jpg/jpeg/png "
                     "textures");
      }
    } else if (!ext.empty() && kPortableExtensions.count(ext) == 0) {
      AddIssue(result, USDValidationSeverity::Warning,
               "package.entry.extension", location,
               "entry type is outside the portable USDZ/AR extension set");
    }
    entries.push_back(entry);
    pos = data_offset + compressed_size;
  }
  return entries;
}

void ValidatePackageCentralDirectory(const uint8_t* bytes, size_t size,
                                     const std::vector<PackageEntry>& entries,
                                     USDValidationResult* result) {
  if (size < 22) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.missing", "<package>",
             "ZIP end-of-central-directory record is missing");
    return;
  }
  const size_t lower = size > 65557 ? size - 65557 : 0;
  size_t eocd = std::string::npos;
  for (size_t pos = size - 22;; --pos) {
    uint32_t signature = 0;
    if (ReadU32(bytes, size, pos, &signature) && signature == 0x06054b50u) {
      eocd = pos;
      break;
    }
    if (pos == lower) break;
  }
  if (eocd == std::string::npos) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.missing", "<package>",
             "ZIP end-of-central-directory record is missing");
    return;
  }
  uint16_t disk = 0, central_disk = 0, disk_entries = 0, total_entries = 0;
  uint16_t comment_size = 0;
  uint32_t central_size = 0, central_offset = 0;
  ReadU16(bytes, size, eocd + 4, &disk);
  ReadU16(bytes, size, eocd + 6, &central_disk);
  ReadU16(bytes, size, eocd + 8, &disk_entries);
  ReadU16(bytes, size, eocd + 10, &total_entries);
  ReadU32(bytes, size, eocd + 12, &central_size);
  ReadU32(bytes, size, eocd + 16, &central_offset);
  ReadU16(bytes, size, eocd + 20, &comment_size);
  if (eocd + 22 + comment_size != size) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.trailingData", "<package>",
             "EOCD comment length does not account for the archive tail");
  }
  if (comment_size != 0) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.comment", "<package>",
             "USDZ archives must have an empty ZIP comment");
  }
  if (disk != 0 || central_disk != 0 || disk_entries != total_entries) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.multidisk", "<package>",
             "multi-disk ZIP archives are not valid USDZ packages");
  }
  if (total_entries != entries.size()) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.entryCount", "<package>",
             "central-directory entry count differs from local headers");
  }
  if (central_offset > eocd || central_size > eocd - central_offset ||
      central_offset + central_size != eocd) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.bounds", "<package>",
             "central-directory range is invalid or non-contiguous");
    return;
  }
  size_t pos = central_offset;
  size_t index = 0;
  while (pos < eocd) {
    uint32_t signature = 0;
    if (eocd - pos < 46 || !ReadU32(bytes, size, pos, &signature) ||
        signature != 0x02014b50u) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.centralDirectory.structure", "<package>",
               "invalid central-directory entry header");
      return;
    }
    uint16_t flags = 0, compression = 0, name_size = 0, extra_size = 0;
    uint16_t entry_comment_size = 0;
    uint32_t crc = 0, compressed_size = 0, uncompressed_size = 0;
    uint32_t local_offset = 0;
    ReadU16(bytes, size, pos + 8, &flags);
    ReadU16(bytes, size, pos + 10, &compression);
    ReadU32(bytes, size, pos + 16, &crc);
    ReadU32(bytes, size, pos + 20, &compressed_size);
    ReadU32(bytes, size, pos + 24, &uncompressed_size);
    ReadU16(bytes, size, pos + 28, &name_size);
    ReadU16(bytes, size, pos + 30, &extra_size);
    ReadU16(bytes, size, pos + 32, &entry_comment_size);
    ReadU32(bytes, size, pos + 42, &local_offset);
    const size_t variable = static_cast<size_t>(name_size) + extra_size +
                            entry_comment_size;
    if (variable > eocd - pos - 46) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.centralDirectory.bounds", "<package>",
               "central-directory entry exceeds its declared range");
      return;
    }
    const std::string name(reinterpret_cast<const char*>(bytes + pos + 46),
                           name_size);
    const auto local_it = std::find_if(
        entries.begin(), entries.end(), [&](const PackageEntry& entry) {
          return entry.local_header_offset == local_offset;
        });
    if (local_it == entries.end()) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.centralDirectory.mismatch", "<package>[" + name + "]",
               "central directory references no matching local header");
    } else {
      const PackageEntry& local = *local_it;
      if (name != local.name || flags != local.flags ||
          compression != local.compression || crc != local.crc ||
          compressed_size != local.size ||
          uncompressed_size != local.uncompressed_size ||
          local_offset != local.local_header_offset) {
        AddIssue(result, USDValidationSeverity::Error,
                 "package.centralDirectory.mismatch",
                 "<package>[" + name + "]",
                 "central-directory metadata differs from the local header");
      }
    }
    ++index;
    pos += 46 + variable;
  }
  if (index != total_entries) {
    AddIssue(result, USDValidationSeverity::Error,
             "package.centralDirectory.entryCount", "<package>",
             "parsed central-directory entry count differs from EOCD");
  }
}

void CollectValueDependencies(
    const lightusd::next::Value& value,
    std::unordered_set<std::string>* dependencies) {
  if (!dependencies) return;
  if (const std::string* asset = value.as_asset_path()) {
    if (!asset->empty()) dependencies->insert(*asset);
    return;
  }
  if (value.is_array() &&
      value.type_id() == lightusd::next::TypeId::AssetPath) {
    if (const auto* assets = value.as_token_array()) {
      for (const std::string& asset : *assets) {
        if (!asset.empty()) dependencies->insert(asset);
      }
    }
    return;
  }
  if (const lightusd::next::Dict* dict = value.as_dictionary()) {
    for (const auto& entry : dict->entries()) {
      CollectValueDependencies(entry.second, dependencies);
    }
  }
}

void CollectLayerDependencies(const Layer& layer,
                              std::unordered_set<std::string>* dependencies) {
  if (!dependencies) return;
  for (const std::string& path : layer.meta().subLayers) {
    if (!path.empty()) dependencies->insert(path);
  }
  if (!layer.meta().colorConfiguration.empty()) {
    dependencies->insert(layer.meta().colorConfiguration);
  }
  CollectValueDependencies(layer.meta().customLayerData, dependencies);
  CollectValueDependencies(layer.meta().expressionVariables, dependencies);
  for (const auto& prim : layer.prims()) {
    const auto collect_arcs = [&](const std::vector<std::string>& arcs) {
      for (const std::string& encoded : arcs) {
        const auto arc = lightusd::next::Compositor::ParseReference(encoded);
        if (!arc.asset_path.empty()) dependencies->insert(arc.asset_path);
      }
    };
    collect_arcs(prim.meta().references);
    collect_arcs(prim.meta().payloads);
    CollectValueDependencies(prim.meta().customData(), dependencies);
    CollectValueDependencies(prim.meta().assetInfo(), dependencies);
    CollectValueDependencies(prim.meta().sdrMetadata(), dependencies);
    CollectValueDependencies(prim.meta().clips(), dependencies);
    for (const auto& slot : prim.properties().slots()) {
      const lightusd::next::Value* value = prim.property_value(slot.name_id);
      if (value) CollectValueDependencies(*value, dependencies);
      if (const auto* meta = prim.property_meta(slot.name_id)) {
        CollectValueDependencies(meta->customData, dependencies);
        CollectValueDependencies(meta->assetInfo, dependencies);
        CollectValueDependencies(meta->sdrMetadata, dependencies);
      }
      if (const auto* samples = prim.time_samples(slot.name_id)) {
        for (const auto& sample : *samples) {
          if (const lightusd::next::Value* sample_value =
                  prim.time_sample_value(sample.second)) {
            CollectValueDependencies(*sample_value, dependencies);
          }
        }
      }
    }
  }
}

void ValidatePackage(const uint8_t* bytes, size_t size, const Layer& layer,
                     bool arkit, USDValidationResult* result,
                     std::vector<PackageEntry>* parsed_entries) {
  if (!result) return;
  result->checked_groups.package = true;
  std::vector<PackageEntry> entries =
      ParsePackageEntries(bytes, size, arkit, result);
  if (entries.empty()) {
    AddIssue(result, USDValidationSeverity::Error, "package.structure.empty",
             "<package>", "USDZ archive contains no local-file entries");
    return;
  }
  ValidatePackageCentralDirectory(bytes, size, entries, result);
  const std::string root_ext = LowerExtension(entries.front().name);
  if (root_ext != "usd" && root_ext != "usda" && root_ext != "usdc") {
    AddIssue(result, USDValidationSeverity::Error, "package.root.first",
             "<package>[" + entries.front().name + "]",
             "the first USDZ entry must be the package root USD layer");
  }
  // UsdUtilsCreateNewARKitUsdzPackage forces the root layer to binary crate.
  if (arkit && root_ext != "usdc") {
    AddIssue(result, USDValidationSeverity::Error, "arkit.package.rootLayer",
             "<package>[" + entries.front().name + "]",
             "the ARKit package root layer must be a `.usdc` crate layer, but "
             "is `." + root_ext + "`");
  }
  std::unordered_set<std::string> entry_names;
  for (const auto& entry : entries) entry_names.insert(entry.name);
  std::unordered_set<std::string> dependencies;
  CollectLayerDependencies(layer, &dependencies);
  for (const std::string& authored : dependencies) {
    const std::string normalized =
        NormalizePackagePath(entries.front().name, authored);
    if (normalized.empty()) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.dependency.external", "<package>",
               "dependency is not package-relative: " + authored);
    } else if (entry_names.count(normalized) == 0) {
      AddIssue(result, USDValidationSeverity::Error,
               "package.dependency.missing", "<package>",
               "authored dependency is absent from package: " + normalized);
    }
  }
  if (parsed_entries) *parsed_entries = std::move(entries);
}

void ValidateCrate(const uint8_t* bytes, size_t size,
                   const std::string& location, size_t max_memory,
                   USDValidationResult* result) {
  if (!result) return;
  result->checked_groups.crate = true;
  lightusd::next::CrateReadOptions options;
  options.max_memory = max_memory;
  options.lazy_arrays = true;
  lightusd::next::CrateReader reader(options);
  lightusd::next::CrateReadResult read = reader.Read(bytes, size);
  if (!read.success) {
    for (const auto& error : read.errors) {
      AddIssue(result, USDValidationSeverity::Error, "crate.structure",
               location, "offset " + std::to_string(error.offset) + ": " +
                             error.message);
    }
    if (read.errors.empty()) {
      AddIssue(result, USDValidationSeverity::Error, "crate.structure",
               location, "Crate reader rejected the container");
    }
    return;
  }
  for (const std::string& warning : read.warnings) {
    AddIssue(result, USDValidationSeverity::Warning, "crate.decode.warning",
             location, warning);
  }
  const auto tokens = reader.tokens();
  const auto& paths = reader.paths();
  const auto& fields = reader.fields();
  const auto& specs = reader.specs();
  const auto& fieldsets = reader.fieldset_indices();
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].token_index.value >= tokens.size()) {
      AddIssue(result, USDValidationSeverity::Error,
               "crate.field.tokenIndex", location,
               "field " + std::to_string(i) + " references an invalid token index");
    }
  }
  for (size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].path_index.value >= paths.size()) {
      AddIssue(result, USDValidationSeverity::Error,
               "crate.spec.pathIndex", location,
               "spec " + std::to_string(i) + " references an invalid path index");
    }
    const uint32_t start = specs[i].fieldset_index.value;
    if (start >= fieldsets.size()) {
      AddIssue(result, USDValidationSeverity::Error,
               "crate.spec.fieldsetIndex", location,
               "spec " + std::to_string(i) + " references an invalid fieldset index");
      continue;
    }
    bool terminated = false;
    for (size_t j = start; j < fieldsets.size(); ++j) {
      if (fieldsets[j] == 0xffffffffu) {
        terminated = true;
        break;
      }
      if (fieldsets[j] >= fields.size()) {
        AddIssue(result, USDValidationSeverity::Error,
                 "crate.fieldset.fieldIndex", location,
                 "fieldset references an invalid field index");
        break;
      }
    }
    if (!terminated) {
      AddIssue(result, USDValidationSeverity::Error,
               "crate.fieldset.terminator", location,
               "fieldset has no terminating sentinel");
    }
  }
}

std::string ResolveDependencyPath(const std::string& anchor,
                                  const std::string& asset) {
  namespace fs = std::filesystem;
  fs::path path(asset);
  if (path.is_relative()) path = fs::path(anchor).parent_path() / path;
  return path.lexically_normal().string();
}

bool HasResolvableUdimTile(const std::string& resolved_template) {
  namespace fs = std::filesystem;
  constexpr char kUdimToken[] = "<UDIM>";
  constexpr int kFirstUdimTile = 1001;
  constexpr int kLastUdimTile = 1100;

  const fs::path path(resolved_template);
  const std::string filename = path.filename().string();
  const size_t token_pos = filename.find(kUdimToken);
  if (token_pos == std::string::npos) return false;

  const fs::path directory = path.parent_path().empty()
                                 ? fs::path(".")
                                 : path.parent_path();
  const std::string prefix = filename.substr(0, token_pos);
  const std::string suffix =
      filename.substr(token_pos + std::strlen(kUdimToken));
  for (int tile = kFirstUdimTile; tile <= kLastUdimTile; ++tile) {
    const std::string candidate =
        prefix + std::to_string(tile) + suffix;
    std::error_code ec;
    if (fs::exists(directory / candidate, ec) && !ec) return true;
  }
  return false;
}

std::string MapComposedPath(const std::string& source_path,
                            const std::string& source_prefix,
                            const std::string& target_prefix) {
  if (source_prefix == "/") {
    return target_prefix == "/" ? source_path : target_prefix + source_path;
  }
  if (source_path == source_prefix) return target_prefix;
  if (source_path.size() > source_prefix.size() &&
      source_path.compare(0, source_prefix.size(), source_prefix) == 0 &&
      source_path[source_prefix.size()] == '/') {
    return target_prefix + source_path.substr(source_prefix.size());
  }
  return std::string();
}

std::string PropertyTypeName(const lightusd::next::PrimSpec& prim,
                             const lightusd::next::PropSlot& slot) {
  const std::string name(
      lightusd::next::GetPropNameTable().get(slot.name_id));
  if (const std::string* declared = prim.property_type_name(name)) {
    if (!declared->empty()) return *declared;
  }
  const char* type = lightusd::next::GetTypeName(
      static_cast<lightusd::next::TypeId>(slot.value_type));
  return type ? std::string(type) : std::to_string(slot.value_type);
}

void CompareLayerTypes(const Layer& stronger, const Layer& weaker,
                       const std::string& weaker_prefix,
                       const std::string& stronger_prefix,
                       USDValidationResult* result) {
  for (const auto& weak_prim : weaker.prims()) {
    const std::string mapped = MapComposedPath(
        weak_prim.path().str(), weaker_prefix, stronger_prefix);
    if (mapped.empty()) continue;
    const lightusd::next::PrimSpec* strong_prim =
        stronger.prim_at_path(mapped);
    if (!strong_prim) continue;
    for (const auto& weak_slot : weak_prim.properties().slots()) {
      const lightusd::next::PropSlot* strong_slot =
          strong_prim->property(weak_slot.name_id);
      if (!strong_slot) continue;
      const std::string property(
          lightusd::next::GetPropNameTable().get(weak_slot.name_id));
      const bool weak_rel = weak_slot.is_relationship();
      const bool strong_rel = strong_slot->is_relationship();
      if (weak_rel != strong_rel) {
        AddIssue(result, USDValidationSeverity::Error,
                 "core.composition.propertyKindMismatch",
                 mapped + "." + property,
                 "property is authored as both an attribute and a relationship across composition arcs");
        continue;
      }
      if (weak_rel) continue;
      const std::string weak_type = PropertyTypeName(weak_prim, weak_slot);
      const std::string strong_type = PropertyTypeName(*strong_prim, *strong_slot);
      if (weak_type != strong_type) {
        AddIssue(result, USDValidationSeverity::Error,
                 "core.composition.attributeTypeMismatch",
                 mapped + "." + property,
                 "attribute has conflicting declared types '" + strong_type +
                     "' and '" + weak_type + "' across composition arcs");
      }
    }
  }
}

void AuditLayerTypesRecursive(
    const Layer& layer, const std::string& anchor,
    const lightusd::next::pcp::LayerLoadOptions& options,
    std::unordered_set<std::string>* visited, USDValidationResult* result) {
  if (!visited || !result) return;
  const auto load_and_audit = [&](const std::string& asset,
                                  const std::string& source_prefix,
                                  const std::string& target_prefix) {
    if (asset.empty()) return;
    const std::string resolved = ResolveDependencyPath(anchor, asset);
    std::string warnings, errors;
    std::shared_ptr<Layer> dependency =
        lightusd::next::pcp::LoadLayerFromFile(
            resolved, &warnings, &errors, options);
    if (!dependency) return;
    std::string effective_prefix = source_prefix;
    if (effective_prefix.empty()) {
      if (!dependency->meta().defaultPrim.empty()) {
        effective_prefix = "/" + dependency->meta().defaultPrim;
      } else if (!dependency->root_indices().empty()) {
        const auto* root = dependency->prim(dependency->root_indices().front());
        if (root) effective_prefix = root->path().str();
      }
    }
    if (!effective_prefix.empty()) {
      CompareLayerTypes(layer, *dependency, effective_prefix, target_prefix,
                        result);
    }
    if (visited->insert(resolved).second) {
      AuditLayerTypesRecursive(*dependency, resolved, options, visited, result);
    }
  };

  for (const std::string& sublayer : layer.meta().subLayers) {
    load_and_audit(sublayer, "/", "/");
  }
  for (const auto& prim : layer.prims()) {
    const auto audit_arcs = [&](const std::vector<std::string>& arcs) {
      for (const std::string& encoded : arcs) {
        const auto arc = lightusd::next::Compositor::ParseReference(encoded);
        if (!arc.asset_path.empty()) {
          load_and_audit(arc.asset_path, arc.prim_path, prim.path().str());
        }
      }
    };
    audit_arcs(prim.meta().references);
    audit_arcs(prim.meta().payloads);
  }
}

// ---------------------------------------------------------------------------
// Unresolvable authored dependencies (usdchecker MissingReferenceValidator).
// Filesystem access lives in the tool, not the validation module.
// ---------------------------------------------------------------------------
void ValidateDependencyResolution(const Layer& layer,
                                  const std::string& anchor,
                                  USDValidationResult* result,
                                  bool require_complete = false,
                                  size_t max_memory = size_t(512) << 20) {
  lightusd::next::ResolverConfig resolver_config;
  resolver_config.enable_suffix_fallback = false;
  lightusd::next::AssetResolver resolver(resolver_config);
  const auto package_exists = [&](const lightusd::next::ResolvedAsset& asset) {
    lightusd::next::USDZReadOptions options;
    options.max_archive_size = options.max_entry_size = max_memory;
    lightusd::next::USDZReader archive;
    if (!archive.OpenFile(asset.package_path, options)) return false;
    std::function<bool(const lightusd::next::USDZReader&, const std::string&, size_t)> contains;
    contains = [&](const lightusd::next::USDZReader& zip, const std::string& path, size_t depth) {
      if (depth > 32) return false;
      const size_t bracket = path.find('[');
      const std::string name = path.substr(0, bracket);
      for (size_t i = 0; i < zip.NumEntries(); ++i) if (zip.EntryName(i) == name) {
        if (bracket == std::string::npos) return true;
        if (path.back() != ']') return false;
        lightusd::next::USDZReader inner;
        return inner.Open(zip.EntryData(i), zip.EntrySize(i), options) &&
            contains(inner, path.substr(bracket+1, path.size()-bracket-2), depth+1);
      }
      return false;
    };
    return contains(archive, asset.asset_in_package, 0);
  };
  std::unordered_set<std::string> dependencies, missing;
  CollectLayerDependencies(layer, &dependencies);
  std::vector<std::string> ordered(dependencies.begin(), dependencies.end());
  std::sort(ordered.begin(), ordered.end());
  for (const std::string& authored : ordered) {
    if (authored.find("://") != std::string::npos) {
      if (require_complete) {
        result->complete = false;
        AddIssue(result, USDValidationSeverity::Error, "checker.coverage.resolver", "<layer>",
                 "Dependency requires an unavailable resolver: " + authored);
      }
    }
    const std::string resolved = ResolveDependencyPath(anchor, authored);
    bool exists = false;
    const auto asset = resolver.Resolve(authored, anchor, false);
    if (asset.is_package) exists = asset.exists && package_exists(asset);
    else if (authored.find("<UDIM>") != std::string::npos) exists = HasResolvableUdimTile(resolved);
    else {
      std::error_code ec;
      exists = std::filesystem::is_regular_file(resolved, ec);
    }
    if (!exists) {
      if (require_complete) {
        result->complete = false;
        AddIssue(result, USDValidationSeverity::Error, "checker.coverage.dependencies", "<layer>",
                 "Required dependency is unavailable: " + authored);
      }
      missing.insert(authored);
      AddIssue(result, USDValidationSeverity::Warning,
               "core.dependency.unresolvable", "<layer>",
               "authored dependency `" + authored + "` does not resolve to an existing file (" + resolved + ")");
    }
  }
  // Preserve a useful prim site for the reference normal-texture validator.
  for (const auto& prim : layer.prims()) {
    const auto* id = prim.property_value("info:id");
    if (!id || !id->as_token() || *id->as_token() != "UsdPreviewSurface") continue;
    const auto* connections = prim.connection("inputs:normal");
    if (!connections) continue;
    for (const auto& target : *connections) {
      const auto* texture = layer.prim_at_path(target.str().substr(0, target.str().find('.')));
      if (!texture) continue;
      const auto* file = texture->property_value("inputs:file");
      if (file && file->as_asset_path() && missing.count(*file->as_asset_path()))
        AddIssue(result, USDValidationSeverity::Warning, "shade.normalMap.file",
                 texture->path().str() + ".inputs:file", "Normal texture asset does not resolve");
    }
  }
}

// ---------------------------------------------------------------------------
// Variant sweep (usdchecker parity): enumerate authored variant selections.
// ---------------------------------------------------------------------------
struct VariantChoice {
  std::string key, set_name, selected;
  std::vector<std::string> options;
};

std::string SelectionKey(const std::map<std::string, std::string>& selections) {
  // Length-framed keys cannot collide on delimiters in asset-authored text.
  std::string result;
  for (const auto& selection : selections)
    result += std::to_string(selection.first.size()) + ":" + selection.first +
              std::to_string(selection.second.size()) + ":" + selection.second;
  return result;
}
std::string SelectionDescription(const std::map<std::string, std::string>& selections) {
  std::string result;
  for (const auto& selection : selections) {
    if (!result.empty()) result += ",";
    result += selection.first + "=" + selection.second;
  }
  return result;
}

// Merge `src` into `dst`, dropping issues already present (the same defect
// reported by several variant passes).
void MergeDedupedIssues(USDValidationResult* dst,
                        std::unordered_set<std::string>* seen,
                        const USDValidationResult& src) {
  if (!dst || !seen) return;
  USDValidationResult unique;
  unique.checked_groups = src.checked_groups;
  unique.complete = src.complete;
  for (const USDValidationIssue& issue : src.issues) {
    const std::string key = lightusd::minijson::Value::array({
        issue.severity == USDValidationSeverity::Error ? "error" : "warning",
        issue.rule_id, issue.location, issue.message, issue.source_asset,
        issue.variants, issue.has_time, issue.time}).dump();
    if (seen->insert(key).second) {
      unique.issues.push_back(issue);
    }
  }
  MergeValidationResults(dst, unique);
}

}  // namespace

int lusdchecker::RunChecker(int argc, char** argv,
    const lightusd::next::ValidationRegistry& initial_registry) {
  Args args;
  args.groups = AllAvailableGroups();
  std::string error;
  const ParseArgsResult parsed = ParseArgs(argc, argv, &args, &error);
  if (parsed == ParseArgsResult::ExitSuccess) return kExitValid;
  if (parsed == ParseArgsResult::Error) {
    return ReportError(args, "checker.usage", error);
  }

  if (args.json && args.sarif) {
    std::cerr << "lusdchecker: --json and --sarif are mutually exclusive\n";
    return kExitError;
  }
  if (args.max_memory_mb >
      std::numeric_limits<size_t>::max() / (size_t{1024} * 1024)) {
    return ReportError(args, "checker.usage", "--max-memory-mb is too large");
  }
  const size_t max_memory = args.max_memory_mb * size_t{1024} * 1024;
  lightusd::next::ValidationRegistry registry = initial_registry;
  for (const auto& file : args.schema_files) {
    std::string text;
    if (!ReadFile(file, std::min(max_memory, size_t(16) << 20), &text, &error))
      return ReportError(args, "checker.definitions", error);
    auto loaded = registry.LoadSchemaDefinitions(text);
    if (!loaded) return ReportError(args, "checker.definitions", loaded.error());
  }
  for (const auto& file : args.shader_files) {
    std::string text;
    if (!ReadFile(file, std::min(max_memory, size_t(16) << 20), &text, &error))
      return ReportError(args, "checker.definitions", error);
    auto loaded = registry.LoadShaderDefinitions(text);
    if (!loaded) return ReportError(args, "checker.definitions", loaded.error());
  }
  args.groups.registry = &registry;
  for (const auto& keyword : args.keywords) {
    bool known = !KeywordToGroups(keyword).empty();
    for (const auto& rule : registry.validators())
      known |= std::find(rule.keywords.begin(), rule.keywords.end(), keyword) != rule.keywords.end();
    if (!known) return ReportError(args, "checker.usage", "unknown validator keyword: " + keyword);
  }
  if (args.dump_rules) {
    size_t count = 0;
    const auto* rules = lightusd::next::GetValidationRuleTable(&count);
    for (size_t i = 0; i < count; ++i) {
      const auto metadata = lightusd::next::GetValidationRuleMetadata(rules[i].id);
      std::cout << "[" << rules[i].group << ":" << rules[i].id << "]:\n\tDoc: " << rules[i].doc
                << "\n\tCategory: " << metadata.category << '\n';
    }
    for (const auto& rule : registry.validators())
      std::cout << "[extension:" << rule.id << "]:\n\tDoc: " << rule.description << '\n';
    return kExitValid;
  }

  lightusd::next::pcp::LayerLoadOptions load_options;
  load_options.max_memory = max_memory;
  load_options.strict_aousd_conformance = args.strict_parse;

  std::string parser_warnings;
  std::string parser_errors;
  std::string input_bytes;
  std::shared_ptr<Layer> layer;
  if (args.input == "-") {
    if (!ReadStdin(max_memory, &input_bytes, &error)) {
      return ReportError(args, "checker.io", error);
    }
    layer = lightusd::next::pcp::LoadLayerFromMemory(
        "stdin.usda", reinterpret_cast<const uint8_t*>(input_bytes.data()),
        input_bytes.size(), &parser_warnings, &parser_errors,
        load_options);
  } else if (args.groups.package || args.groups.crate) {
    if (!ReadFile(args.input, max_memory, &input_bytes, &error)) {
      return ReportError(args, "checker.io", error);
    }
    layer = lightusd::next::pcp::LoadLayerFromMemory(
        args.input, reinterpret_cast<const uint8_t*>(input_bytes.data()),
        input_bytes.size(), &parser_warnings, &parser_errors, load_options);
  } else {
    layer = lightusd::next::pcp::LoadLayerFromFile(
        args.input, &parser_warnings, &parser_errors, load_options);
  }
  if (!layer) return ReportError(args, "parser.error", parser_errors.empty() ? "failed to parse input" : parser_errors);

  USDValidationResult result;
  result.checked_groups.core = false;
  std::map<std::string, std::shared_ptr<Layer>> loaded_layers;
  loaded_layers[args.input] = layer;
  size_t cached_bytes = layer->memory_usage();
  bool resource_limit_hit = false;
  auto load_cached = [&](const std::string& path, std::string* load_error) -> std::shared_ptr<Layer> {
    const auto existing = loaded_layers.find(path);
    if (existing != loaded_layers.end()) return existing->second;
    std::string warnings, errors;
    auto loaded = lightusd::next::pcp::LoadLayerFromFile(path, &warnings, &errors, load_options);
    if (!warnings.empty()) parser_warnings += warnings;
    if (!loaded) { if (load_error) *load_error = errors; return {}; }
    const size_t bytes = loaded->memory_usage();
    if (bytes > max_memory - std::min(cached_bytes, max_memory)) {
      resource_limit_hit = true;
      if (load_error) *load_error = "validation layer cache exceeds --max-memory-mb";
      return {};
    }
    cached_bytes += bytes;
    loaded_layers[path] = loaded;
    return loaded;
  };
  std::map<std::string, VariantChoice> discovered;
  auto compose_pass = [&](const std::map<std::string, std::string>& overrides,
      std::vector<lightusd::next::CompositionError>* errors) -> std::unique_ptr<Layer> {
    lightusd::next::ResolverConfig config;
    config.enable_suffix_fallback = !args.strict_parse;
    lightusd::next::AssetResolver resolver(config);
    lightusd::next::Compositor compositor(&resolver);
    lightusd::next::CompositionOptions options;
    options.strict_aousd_conformance = args.strict_parse;
    options.max_layer_memory = max_memory;
    options.variant_overrides = overrides;
    options.variant_observer = [&](const std::string& path,
        const lightusd::next::VariantSetData& set, const std::string& chosen) {
      auto& choice = discovered[path + "{" + set.name + "}"];
      choice.key = path + "{" + set.name + "}";
      choice.set_name = set.name; choice.selected = chosen;
      for (const auto& variant : set.variants)
        if (std::find(choice.options.begin(), choice.options.end(), variant.name) == choice.options.end())
          choice.options.push_back(variant.name);
      std::sort(choice.options.begin(), choice.options.end());
    };
    compositor.SetOptions(options);
    compositor.SetLayerLoader([&](const std::string& path, std::string* error) {
      auto loaded = load_cached(path, error);
      return loaded ? std::make_unique<Layer>(loaded->Clone()) : std::unique_ptr<Layer>();
    });
    auto composed = compositor.Compose(*layer, args.input == "-" ? "" : args.input);
    if (errors) *errors = compositor.GetErrors();
    return composed;
  };
  std::vector<lightusd::next::CompositionError> composition_errors;
  size_t variant_pass_count = 1;
  bool variant_limit_hit = false;
  bool any_pass_composed = false;

  std::unordered_set<std::string> seen_issue_keys;
  auto validate_stage = [&](std::unique_ptr<Layer> composed, const std::string& selections) {
    lightusd::next::Stage stage;
    stage.SetRootLayer(std::move(*composed));
    auto options = args.groups;
    options.stage_presence_checks = false;
    options.run_callbacks = false;
    auto pass_result = lightusd::next::ValidateLayerAgainstAOUSDCore(*stage.GetRootLayer(), options);
    lightusd::next::ValidationContext context;
    context.layer = stage.GetRootLayer(); context.stage = &stage;
    context.options = options; context.source_asset = args.input; context.variants = selections;
    context.limits.max_value_clip_samples = args.max_samples;
    context.limits.max_resident_bytes = max_memory;
    registry.RunCallbacks(lightusd::next::ValidationScope::Stage, context, nullptr, &pass_result);
    for (const auto& prim : stage.GetRootLayer()->prims())
      registry.RunCallbacks(lightusd::next::ValidationScope::Prim, context, &prim, &pass_result);
    for (auto& issue : pass_result.issues) {
      issue.source_asset = args.input; issue.variants = selections;
    }
    MergeDedupedIssues(&result, &seen_issue_keys, pass_result);
    if (args.all_samples) {
      lightusd::next::EvalOptions eval;
      eval.clip_stage_cache = std::make_shared<lightusd::next::ValueClipStageCache>();
      eval.clip_stage_loader = [&](const std::string& path, lightusd::next::Stage* out,
                                    std::string*, std::string* error) {
        lightusd::next::AssetResolver resolver;
        const std::string resolved = resolver.ResolvePath(path, args.input);
        auto loaded = load_cached(resolved.empty() ? path : resolved, error);
        if (!loaded) return false;
        out->SetRootLayer(loaded->Clone());
        return true;
      };
      MergeDedupedIssues(&result, &seen_issue_keys,
          lightusd::next::ValidateStageSamples(stage, context, eval));
    }
  };
  if (!args.composed) {
    result = lightusd::next::ValidateLayerAgainstAOUSDCore(*layer, args.groups);
    if (args.all_samples) validate_stage(std::make_unique<Layer>(layer->Clone()), "");
    else {
      lightusd::next::Stage stage;
      stage.SetRootLayer(layer->Clone());
      lightusd::next::ValidationContext context;
      context.layer = stage.GetRootLayer(); context.stage = &stage;
      context.options = args.groups; context.source_asset = args.input;
      context.limits.max_resident_bytes = max_memory;
      registry.RunCallbacks(lightusd::next::ValidationScope::Stage, context, nullptr, &result);
    }
  } else {
    struct Pass { std::map<std::string, std::string> selections; size_t base; };
    std::vector<std::map<std::string, std::string>> bases;
    if (args.variant_selections.empty()) bases.emplace_back();
    else for (const auto& pairs : args.variant_selections) {
      std::map<std::string, std::string> base;
      for (const auto& pair : pairs) base[pair.first] = pair.second;
      bases.push_back(std::move(base));
    }
    std::deque<Pass> pending;
    std::set<std::pair<size_t, std::string>> scheduled, expanded;
    for (size_t i = 0; i < bases.size(); ++i) {
      pending.push_back({bases[i], i}); scheduled.emplace(i, SelectionKey(bases[i]));
    }
    variant_pass_count = 0;
    const size_t cap = args.disable_variant_limit ? SIZE_MAX : kVariantValidationLimit;
    if (args.groups.stage_presence_checks)
      lightusd::next::ValidateStageMetadataPresence(*layer, args.groups, &result);
    while (!pending.empty()) {
      if (variant_pass_count >= cap) { variant_limit_hit = true; break; }
      Pass pass = std::move(pending.front()); pending.pop_front();
      discovered.clear();
      std::vector<lightusd::next::CompositionError> errors;
      auto composed = compose_pass(pass.selections, &errors);
      composition_errors.insert(composition_errors.end(), errors.begin(), errors.end());
      ++variant_pass_count;
      std::map<std::string, std::string> effective = pass.selections;
      for (const auto& item : discovered)
        if (!item.second.selected.empty()) effective[item.first] = item.second.selected;
      const bool new_selection = expanded.emplace(pass.base, SelectionKey(effective)).second;
      if (args.verbose) std::cerr << "lusdchecker: variant pass " << variant_pass_count << " "
                                 << SelectionDescription(effective) << '\n';
      if (composed && new_selection) {
        any_pass_composed = true;
        validate_stage(std::move(composed), SelectionDescription(effective));
      }
      if (!new_selection || args.skip_variants) continue;
      for (const auto& item : discovered) {
        const auto& choice = item.second;
        if (bases[pass.base].count(choice.set_name) || bases[pass.base].count(choice.key)) continue;
        if (!args.variant_sets.empty() && std::find(args.variant_sets.begin(), args.variant_sets.end(),
            choice.set_name) == args.variant_sets.end()) continue;
        // Explicit selections sweep only explicitly requested additional sets.
        if (!args.variant_selections.empty() && args.variant_sets.empty()) continue;
        for (const auto& variant : choice.options) {
          if (variant == choice.selected) continue;
          auto next = effective; next[choice.key] = variant;
          const auto key = std::make_pair(pass.base, SelectionKey(next));
          if (expanded.count(key) || scheduled.count(key)) continue;
          if (scheduled.size() >= cap) { variant_limit_hit = true; continue; }
          scheduled.insert(key);
          pending.push_back({std::move(next), pass.base});
        }
      }
    }
    // Check authored declarations before flattening can conceal a type mismatch.
    for (const auto& entry : loaded_layers) {
      auto options = args.groups;
      options.geom = options.shade = options.lux = options.physics = options.render = options.arkit = false;
      options.stage_presence_checks = false;
      options.require_complete = false; // incomplete fragments may inherit their type/API
      options.run_callbacks = false;
      auto authored = lightusd::next::ValidateLayerAgainstAOUSDCore(*entry.second, options);
      authored.issues.erase(std::remove_if(authored.issues.begin(), authored.issues.end(),
          [](const USDValidationIssue& issue) {
            const auto category = lightusd::next::GetValidationRuleMetadata(issue.rule_id).category;
            return category != "normative" && issue.rule_id != "core.schema.attributeType" &&
                   issue.rule_id != "core.schema.propertyKind";
          }), authored.issues.end());
      lightusd::next::ValidationContext context;
      context.layer = entry.second.get(); context.options = options; context.source_asset = entry.first;
      registry.RunCallbacks(lightusd::next::ValidationScope::Layer, context, nullptr, &authored);
      for (auto& issue : authored.issues) issue.source_asset = entry.first;
      MergeDedupedIssues(&result, &seen_issue_keys, authored);
    }
  }
  if (resource_limit_hit) {
    result.complete = false;
    AddIssue(&result, USDValidationSeverity::Error, "checker.coverage.memory", "<stage>",
             "Validation exceeded the layer cache memory limit");
  }

  if (args.groups.core && !args.root_package_only && args.input != "-" &&
      LowerExtension(args.input) != "usdz") {
    for (const auto& entry : loaded_layers) {
      if (entry.first.find('[') != std::string::npos || LowerExtension(entry.first) == "usdz") continue;
      USDValidationResult dependencies;
      dependencies.checked_groups.core = false;
      ValidateDependencyResolution(*entry.second, entry.first, &dependencies, args.groups.require_complete, max_memory);
      for (auto& issue : dependencies.issues) issue.source_asset = entry.first;
      MergeDedupedIssues(&result, &seen_issue_keys, dependencies);
    }
  }
  if (args.composed && args.groups.core && !args.root_package_only &&
      args.input != "-") {
    std::unordered_set<std::string> visited;
    visited.insert(std::filesystem::path(args.input).lexically_normal().string());
    AuditLayerTypesRecursive(*layer, args.input, load_options, &visited,
                             &result);
  }
  // Inspect every loaded file container and every stored nested member. Magic
  // bytes also identify crates whose portable filename ends in .usd.
  std::function<void(const uint8_t*, size_t, const std::string&, const Layer*, size_t)> audit_container;
  audit_container = [&](const uint8_t* raw, size_t size, const std::string& source,
                        const Layer* root_layer, size_t depth) {
    if (depth > 32) {
      result.complete = false;
      AddIssue(&result, USDValidationSeverity::Error, "checker.coverage.package", source,
               "Nested package validation exceeds depth limit");
      return;
    }
    const bool package = size >= 4 && raw[0] == 0x50 && raw[1] == 0x4b && raw[2] == 3 && raw[3] == 4;
    const bool crate = size >= 8 && std::memcmp(raw, "PXR-USDC", 8) == 0;
    USDValidationResult local;
    local.checked_groups.core = false;
    if (args.profile != "default" && size >= 5 && std::memcmp(raw, "#usda", 5) == 0) {
      size_t first = 5;
      while (first < size && (raw[first] == ' ' || raw[first] == '\t')) ++first;
      size_t end = first;
      while (end < size && end-first < 64 && raw[end] != ' ' && raw[end] != '\t' && raw[end] != '\r' && raw[end] != '\n') ++end;
      const std::string version(reinterpret_cast<const char*>(raw + first), end-first);
      if (version != "1.0") {
        local.complete = false;
        AddIssue(&local, USDValidationSeverity::Error, "checker.coverage.version", source,
                 "AOUSD Core 1.0.1 covers USDA 1.0; this document declares " + version);
      }
    }
    if (crate && args.groups.crate) ValidateCrate(raw, size, source, max_memory, &local);
    if (package) {
      std::vector<PackageEntry> entries;
      Layer empty;
      if (args.groups.package)
        ValidatePackage(raw, size, root_layer ? *root_layer : empty, args.groups.arkit, &local, &entries);
      else { USDValidationResult ignored; entries = ParsePackageEntries(raw, size, false, &ignored); }
      for (const auto& entry : entries) {
        if (entry.compression != 0 || entry.data_offset > size || entry.size > size - entry.data_offset) continue;
        const uint8_t* member = raw + entry.data_offset;
        const std::string location = source + "[" + entry.name + "]";
        audit_container(member, entry.size, location, nullptr, depth + 1);
      }
    }
    for (auto& issue : local.issues) issue.source_asset = source;
    MergeDedupedIssues(&result, &seen_issue_keys, local);
  };
  if (args.groups.package || args.groups.crate) {
    audit_container(reinterpret_cast<const uint8_t*>(input_bytes.data()), input_bytes.size(), args.input, layer.get(), 0);
    if (!args.root_package_only) for (const auto& entry : loaded_layers) {
      if (entry.first == args.input || entry.first.find('[') != std::string::npos) continue;
      std::string bytes, read_error;
      if (!ReadFile(entry.first, max_memory, &bytes, &read_error)) {
        result.complete = false;
        AddIssue(&result, USDValidationSeverity::Error, "checker.coverage.io", entry.first, read_error);
        continue;
      }
      audit_container(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), entry.first, entry.second.get(), 0);
    }
  }
  for (const auto& composition_error : composition_errors) {
    USDValidationIssue issue;
    issue.severity = USDValidationSeverity::Error;
    issue.rule_id = "core.composition.error";
    issue.location = composition_error.prim_path.empty()
                         ? std::string("/")
                         : composition_error.prim_path;
    issue.message = composition_error.message;
    result.issues.push_back(std::move(issue));
  }
  if (args.composed && !any_pass_composed && composition_errors.empty()) {
    USDValidationIssue issue;
    issue.severity = USDValidationSeverity::Error;
    issue.rule_id = "core.composition.error";
    issue.location = "/";
    issue.message = "composition failed without a detailed diagnostic";
    result.issues.push_back(std::move(issue));
  }
  if (args.require_all_groups) {
    const std::vector<std::string> requested =
        GetValidationGroupNames(args.groups);
    const std::vector<std::string> checked =
        GetValidationGroupNames(result.checked_groups);
    for (const std::string& name : requested) {
      if (std::find(checked.begin(), checked.end(), name) == checked.end()) {
        AddIssue(&result, USDValidationSeverity::Error,
                 "checker.coverage.skipped", "<checker>",
                 "requested validation group was inapplicable: " + name);
      }
    }
  }
  if (args.usdchecker_compat) {
    lightusd::next::ApplyUsdcheckerCompatSeverities(&result);
  }
  const bool warnings_fail =
      args.strict && (result.warning_count() > 0 || !parser_warnings.empty());
  if (variant_limit_hit) {
    result.complete = false;
    AddIssue(&result, USDValidationSeverity::Error, "checker.coverage.variants", "<stage>",
             "Variant validation limit reached before all selections were checked");
  }
  if (args.groups.normative_only) {
    result.issues.erase(std::remove_if(result.issues.begin(), result.issues.end(),
        [](const USDValidationIssue& issue) {
          const auto category = lightusd::next::GetValidationRuleMetadata(issue.rule_id).category;
          return category != "normative" && category != "coverage";
        }), result.issues.end());
  }
  const bool valid = result.ok() && !warnings_fail && result.complete;
  auto report = MakeReport(args, result, parser_warnings, valid,
                           variant_pass_count, variant_limit_hit);
  if (!lusdchecker::ApplyBaseline(&report, args.baseline, &error))
    return ReportError(args, "checker.baseline", error);

  std::ofstream file_output;
  std::ostream* output = &std::cout;
  if (args.output == "stderr") {
    output = &std::cerr;
  } else if (args.output != "stdout") {
    file_output.open(args.output, std::ios::out | std::ios::trunc);
    if (!file_output) {
      std::cerr << "lusdchecker: error: cannot open output file '"
                << args.output << "'\n";
      return kExitError;
    }
    output = &file_output;
  }

  if (args.json || args.sarif) {
    *output << (args.sarif ? lusdchecker::ToSarif(report).dump(2) : report.dump()) << '\n';
  } else {
    *output << "Input: " << args.input << '\n';
    if (!parser_warnings.empty()) {
      *output << "\nParser warnings:\n" << parser_warnings;
      if (parser_warnings.back() != '\n') *output << '\n';
    }
    *output << lightusd::next::FormatValidationResult(result);
    if (warnings_fail && result.ok()) {
      *output << "Strict result: FAILED - warnings are errors\n";
    }
    if (!args.baseline.empty()) {
      *output << "Baseline: " << report["existingIssueCount"].get_uint64()
              << " existing, " << report["newIssueCount"].get_uint64() << " new findings\n"
              << "Baseline gate: " << (report["gatePassed"].get_bool() ? "PASSED" : "FAILED") << '\n';
    }
  }
  if (!*output) {
    std::cerr << "lusdchecker: error: failed while writing report\n";
    return kExitError;
  }
  return report["gatePassed"].get_bool() ? kExitValid : kExitInvalid;
}
