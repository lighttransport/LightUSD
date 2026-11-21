// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 Light Transport Entertainment Inc.

#include "mcp-tools-validate.hh"

#include <string>
#include <vector>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "str-util.hh"  // base64_decode
#include "../lightusd.hh"
#include "../usd-validation.hh"
#include "mcp-context.hh"

namespace lightusd { namespace legacy {
namespace tydra {
namespace mcp {

namespace {

// Map the optional `groups` argument to ValidationOptions. No `groups` -> core
// only. When `groups` is present, start all-off and enable the listed groups;
// "all" matches lusdcat --validate-all.
ValidationOptions ParseGroups(const nlohmann::json &args) {
  ValidationOptions opts;

  if (args.contains("groups") && args["groups"].is_array()) {
    opts.core = false;
    opts.geom = false;
    opts.shade = false;
    opts.lux = false;
    opts.physics = false;
    opts.crate = false;
    for (const auto &g : args["groups"]) {
      if (!g.is_string()) {
        continue;
      }
      const std::string name = g.get<std::string>();
      if (name == "core") {
        opts.core = true;
      } else if (name == "geom") {
        opts.geom = true;
      } else if (name == "shade") {
        opts.shade = true;
      } else if (name == "lux") {
        opts.lux = true;
      } else if (name == "physics") {
        opts.physics = true;
        // NOTE: no "render" group here — this tool drives the LEGACY
        // validator, which has no UsdRender rules; next's validator (web
        // binding, lusdchecker) is where the render group exists.
      } else if (name == "crate") {
        opts.crate = true;
      } else if (name == "all") {
        opts = MakeValidateAllOptions();
      }
    }
    // If nothing recognized, fall back to core so we never run an empty pass.
    if (!opts.core && !opts.geom && !opts.shade && !opts.lux &&
        !opts.physics && !opts.crate) {
      opts.core = true;
    }
  }

  return opts;
}

const char *SeverityString(USDValidationSeverity severity) {
  return severity == USDValidationSeverity::Error ? "error" : "warning";
}

void ResultToJson(const USDValidationResult &validation,
                  nlohmann::json &result) {
  result["ok"] = validation.ok();
  result["error_count"] = validation.error_count();
  result["warning_count"] = validation.warning_count();
  result["spec_version"] = GetAOUSDCoreSpecVersionString();

  // Which rule groups actually ran (a core-only "ok" did not check geom/shade).
  nlohmann::json groups = nlohmann::json::array();
  for (const std::string &name :
       GetValidationGroupNames(validation.checked_groups)) {
    groups.push_back(name);
  }
  result["checked_groups"] = groups;

  nlohmann::json issues = nlohmann::json::array();
  for (const USDValidationIssue *issue : GetOrderedValidationIssues(validation)) {
    nlohmann::json j;
    j["severity"] = SeverityString(issue->severity);
    j["rule_id"] = issue->rule_id;
    j["location"] = issue->location;
    j["message"] = issue->message;
    issues.push_back(j);
  }
  result["issues"] = issues;
}

}  // namespace

bool UsdValidate(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err) {
  const ValidationOptions options = ParseGroups(args);

  Layer layer;
  USDValidationResult validation;
  std::string warn;
  std::string source;

  if (args.contains("data") && args["data"].is_string()) {
    const std::string binary = base64_decode(args["data"].get<std::string>());
    const std::string name = args.value("name", std::string("memory.usd"));
    USDLoadOptions load_options;
    if (!ValidateUSDFromMemoryAgainstAOUSDCore(
            reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
            name, options, load_options, &validation, &warn, &err)) {
      return false;
    }
    source = "data";
  } else if (args.contains("uri") && args["uri"].is_string()) {
#if !defined(__EMSCRIPTEN__)
    const std::string uri = args["uri"].get<std::string>();
    USDLoadOptions load_options;
    if (!ValidateUSDFileAgainstAOUSDCore(uri, options, load_options,
                                         &validation, &warn, &err)) {
      return false;
    }
    source = "uri";
#else
    err = "Validating from a file URI is not supported in this build";
    return false;
#endif
  } else if (args.contains("layer_uuid") && args["layer_uuid"].is_string()) {
    const std::string uuid = args["layer_uuid"].get<std::string>();
    auto it = ctx.layers.find(uuid);
    if (it == ctx.layers.end()) {
      err = "No layer found for layer_uuid: " + uuid;
      return false;
    }
    layer = it->second.layer;
    validation = ValidateLayerAgainstAOUSDCore(layer, options);
    source = "layer_uuid";
  } else {
    // No explicit input: validate the current session stage by serializing it
    // to USDA and re-parsing as an uncomposed Layer.
    if (!ctx.stage || !ctx.stage_loaded) {
      err =
          "No input provided and no stage loaded. Pass `data`, `uri`, "
          "`layer_uuid`, or load a stage first.";
      return false;
    }
    const std::string usda = ctx.stage->ExportToString();
    if (!LoadLayerFromMemory(reinterpret_cast<const uint8_t *>(usda.data()),
                             usda.size(), "session-stage.usda", &layer, &warn,
                             &err)) {
      return false;
    }
    validation = ValidateLayerAgainstAOUSDCore(layer, options);
    source = "stage";
  }

  ResultToJson(validation, result);
  result["source"] = source;
  if (!warn.empty()) {
    result["warn"] = warn;
  }

  return true;
}

}  // namespace mcp
}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd
