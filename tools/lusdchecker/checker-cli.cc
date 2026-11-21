// SPDX-License-Identifier: Apache-2.0
#include "checker-options.hh"
#include <iostream>
#include <cerrno>
#include <cstdlib>
#include <limits>
namespace {
using namespace lusdchecker;
using lightusd::next::ValidationOptions;
using lightusd::next::GetAOUSDCoreSpecVersionString;
using Args = CheckerOptions;
void PrintUsage(std::ostream& os) {
  os << "Usage: lusdchecker [options] FILE\n"
        "\n"
        "Validate USDA, USDC, USDZ, or MaterialX (.mtlx) against AOUSD Core\n"
        "1.1.0 and LightUSD's structural schema rules. FILE may be '-' for\n"
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
        "      --profile NAME    default, strict, or aousd-core-1.1.0\n"
        "                        (aousd-core-1.0.1: compatible alias)\n"
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
      args->groups = lightusd::next::MakeValidateAllOptions();
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
  return lusdchecker::NormalizeOptions(args, error) ? ParseArgsResult::Run : ParseArgsResult::Error;
}
}
int lusdchecker::RunChecker(int argc, char** argv,
    const lightusd::next::ValidationRegistry& registry, const Environment* environment) {
  CheckerOptions args;
  std::string error;
  const auto parsed = ParseArgs(argc, argv, &args, &error);
  if (parsed == ParseArgsResult::ExitSuccess) return 0;
  if (parsed == ParseArgsResult::Error) {
    // Preserve structured usage reports through the execution frontend.
    args.input.clear();
    args.usage_error = error;
  }
  return RunConfiguredChecker(std::move(args), registry, environment);
}
