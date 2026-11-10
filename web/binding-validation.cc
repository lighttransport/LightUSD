// SPDX-License-Identifier: Apache-2.0
#include "binding-core-api.h"
#include "validation/usd-validation.hh"
#include "crate/crate-reader.hh"
#include "load-usd.hh"
#include "core/minijson.hh"
#include <cstdlib>
#include <cstring>
namespace lightusd { namespace web_core {
namespace tn = lightusd;
namespace {

tn::ValidationOptions ParseValidationOptionsJSONForWeb(
    const std::string& options_json) {
  tn::ValidationOptions opts;
  if (options_json.empty()) return opts;

  lightusd::minijson::Value args;
  lightusd::minijson::ParseOptions parse_options;
  // Match the previous options parser: the last duplicate member wins.
  parse_options.reject_duplicate_keys = false;
  if (!lightusd::minijson::Parse(options_json, &args, nullptr, parse_options) ||
      !args.is_object() || !args.contains("groups") ||
      !args["groups"].is_array()) {
    return opts;
  }

  opts.core = false;
  opts.geom = false;
  opts.shade = false;
  opts.lux = false;
  opts.physics = false;
  opts.crate = false;
  for (const auto& group : args["groups"]) {
    if (!group.is_string()) continue;
    const std::string name = group.get_string();
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
    } else if (name == "render") {
      opts.render = true;
    } else if (name == "crate") {
      opts.crate = true;
    } else if (name == "all") {
      opts = tn::MakeValidateAllOptions();
    }
  }
  if (!opts.core && !opts.geom && !opts.shade && !opts.lux && !opts.physics &&
      !opts.render && !opts.crate) {
    opts.core = true;
  }
  return opts;
}

lightusd::minijson::Value ValidationResultToJSON(const tn::USDValidationResult& v) {
  lightusd::minijson::Value result;
  result["parse_ok"] = true;
  result["ok"] = v.ok();
  result["error_count"] = v.error_count();
  result["warning_count"] = v.warning_count();
  result["spec_version"] = tn::GetAOUSDCoreSpecVersionString();
  {
    lightusd::minijson::Value groups = lightusd::minijson::Value::array();
    for (const std::string& name :
         tn::GetValidationGroupNames(v.checked_groups)) {
      groups.push_back(name);
    }
    result["checked_groups"] = groups;
  }
  lightusd::minijson::Value issues = lightusd::minijson::Value::array();
  for (const tn::USDValidationIssue* issue :
       tn::GetOrderedValidationIssues(v)) {
    lightusd::minijson::Value item;
    item["severity"] =
        issue->severity == tn::USDValidationSeverity::Error ? "error"
                                                            : "warning";
    item["rule_id"] = issue->rule_id;
    item["location"] = issue->location;
    item["message"] = issue->message;
    issues.push_back(item);
  }
  result["issues"] = issues;
  return result;
}

}  // namespace

// validateFromBinary(bytes, filename, optionsJson) -> JSON string, matching
// the legacy LightUSDLoaderNative.validateFromBinary contract consumed by
// web/js/validation.js. Runs AOUSD-core validation over ::lightusd::Layer.
static std::string validateFromBytes(const uint8_t* data, size_t size,
                                      const std::string& filename,
                                      const std::string& options_json) {
  tn::ValidationOptions options =
      ParseValidationOptionsJSONForWeb(options_json);
  // This module-level adapter preserves the legacy loader contract. The next
  // validator keeps its stricter authored-stage-presence rules enabled for
  // native callers; the legacy WASM validator never emitted these warnings.
  options.stage_presence_checks = false;

  lightusd::minijson::Value result;
  tn::USDValidationResult validation;
  std::string warn, err;
  const bool is_usdc = tn::IsUSDCData(data, size);
  bool loaded = false;
  if (is_usdc) {
    tn::CrateReadOptions crate_options;
    tn::CrateReader reader(crate_options);
    tn::CrateReadResult crate = reader.Read(data, size);
    for (const std::string& item : crate.warnings) {
      if (!warn.empty() && warn.back() != '\n') warn.push_back('\n');
      warn += item;
      if (warn.empty() || warn.back() != '\n') warn.push_back('\n');
    }
    if (crate.success) {
      const tn::Layer* root = crate.stage.GetRootLayer();
      if (root) {
        validation = tn::ValidateLayerAgainstAOUSDCore(*root, options);
        // A successful bounded CrateReader pass has checked the binary crate
        // structure; the semantic rules above validate its decoded root layer.
        validation.checked_groups.crate = options.core || options.crate;
        loaded = true;
      }
    }
  }
  if (!loaded) {
    warn.clear();
    err.clear();
    loaded = tn::ValidateUSDFromMemoryAgainstAOUSDCore(
        data, size, filename, options, &validation, &warn, &err);
  }
  // The legacy WASM CrateReader reports that threaded reading is unavailable
  // for every USDC load. Keep that observable loader warning in the
  // compatibility adapter; it does not imply the crate validation group ran.
  if (is_usdc) {
    if (!warn.empty() && warn.back() != '\n') warn.push_back('\n');
    warn += "Threading is disabled for this build.\n";
  }
  if (!loaded) {
    result["parse_ok"] = false;
    result["ok"] = false;
    result["error"] = err.empty() ? std::string("Validation failed") : err;
    if (!warn.empty()) result["warn"] = warn;
    return result.dump();
  }

  result = ValidationResultToJSON(validation);
  if (!warn.empty()) result["warn"] = warn;
  return result.dump();
}

uint8_t* NextValidateJSON(const uint8_t* data, uint32_t size,
                          const uint8_t* filename, uint32_t filename_size,
                          const uint8_t* options, uint32_t options_size) {
  if (size > (uint32_t{1} << 30) || (size && !data) ||
      (filename_size && !filename) || (options_size && !options)) return nullptr;
  const std::string name = filename_size
      ? std::string(reinterpret_cast<const char*>(filename), filename_size)
      : std::string();
  const std::string config = options_size
      ? std::string(reinterpret_cast<const char*>(options), options_size)
      : std::string();
  const std::string json = validateFromBytes(data, size, name, config);
  uint8_t* result = static_cast<uint8_t*>(std::malloc(json.size() + 1));
  if (!result) return nullptr;
  std::memcpy(result, json.c_str(), json.size() + 1);
  return result;
}

} }
