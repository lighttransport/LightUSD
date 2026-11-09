// SPDX-License-Identifier: Apache-2.0
#include "checker-options.hh"
#include "report.hh"
#include "next/reader/usdz-reader.hh"
#include <cstring>
#include <limits>
#include <map>
#include <memory>

namespace lusdchecker {
namespace {
using Json = lightusd::minijson::Value;
using Resolver = lightusd::next::AssetResolver;

Json Failure(const std::string& input, const std::string& profile,
             const std::string& message) {
  return Json{{"tool", "lusdchecker"}, {"reportVersion", 2}, {"input", input},
      {"profile", profile}, {"valid", false}, {"complete", false},
      {"executionSuccessful", false}, {"gatePassed", false},
      {"conformance", "incomplete"}, {"errorCount", 1}, {"warningCount", 0},
      {"issues", Json::array({Json{{"severity", "error"},
          {"ruleId", "checker.usage"}, {"category", "coverage"},
          {"location", "<checker>"}, {"sourceAsset", input}, {"message", message}}})}};
}

// Extract nested package members from registered bytes only. Bound both the
// archive and the member before allocating; USDZ entries must be stored.
bool ReadMemory(const Resolver& assets, const std::string& key, size_t limit,
                std::string* out, std::string* error) {
  std::string outer, entry;
  auto view = assets.GetMemoryAssetView(key);
  const bool packaged = !view && Resolver::ParsePackagePath(key, &outer, &entry);
  if (packaged) view = assets.GetMemoryAssetView(outer);
  if (!view) { *error = "Asset was not supplied: " + key; return false; }
  if (view->size() > limit) { *error = "Asset exceeds memory limit: " + key; return false; }
  const uint8_t* data = view->data();
  size_t size = view->size();
  std::vector<std::unique_ptr<lightusd::next::USDZReader>> archives;
  if (packaged) {
    for (size_t depth = 0; ; ++depth) {
      if (depth >= 32) { *error = "Package nesting exceeds 32 levels"; return false; }
      auto zip = std::make_unique<lightusd::next::USDZReader>();
      lightusd::next::USDZReadOptions options;
      options.max_archive_size = options.max_entry_size = limit;
      if (!zip->Open(data, size, options)) { *error = zip->Error(); return false; }
      std::string name, nested;
      const bool more = Resolver::ParsePackagePath(entry, &name, &nested);
      if (!more) name = entry;
      bool found = false;
      for (size_t i = 0; i < zip->NumEntries(); ++i) if (zip->EntryName(i) == name) {
        data = zip->EntryData(i); size = zip->EntrySize(i); found = true; break;
      }
      if (!found) { *error = "Package member was not supplied: " + key; return false; }
      archives.push_back(std::move(zip));
      if (!more) break;
      entry = nested;
    }
  }
  if (size) out->assign(reinterpret_cast<const char*>(data), size);
  else out->clear();
  return true;
}
}  // namespace

Json CheckMemory(const uint8_t* data, size_t size, const std::string& filename,
                 const std::string& options_json, Resolver* supplied) {
  Json options;
  lightusd::minijson::Error parse_error;
  const auto fail = [&](const std::string& message) {
    auto report = Failure(filename, options["profile"].get_string(), message);
    return options["format"].get_string() == "sarif" ? ToSarif(report) : report;
  };
  if (options_json.size() > (size_t(16) << 20) ||
      !lightusd::minijson::Parse(options_json, &options, &parse_error) || !options.is_object())
    return fail("options must be a JSON object (maximum 16 MiB)");
  if ((!data && size) || size > (size_t(1) << 30)) return fail("input exceeds 1 GiB or is null");
  if (filename.size() > 65536) return fail("filename exceeds 64 KiB");
  if (filename.empty() || filename == "-" || filename.front() == '-' ||
      filename.find('\0') != std::string::npos)
    return fail("filename must be a nonempty asset identifier, not a CLI option or stdin");
  CheckerOptions args;
  args.input = filename;
  args.json = true;
  auto registry = lightusd::next::GetBuiltinValidationRegistry();
  static const struct { const char* name; bool CheckerOptions::*member; } flags[] = {
    {"strict", &CheckerOptions::strict}, {"strictParse", &CheckerOptions::strict_parse},
    {"composed", &CheckerOptions::composed}, {"allTimeSamples", &CheckerOptions::all_samples},
    {"skipVariants", &CheckerOptions::skip_variants}, {"rootPackageOnly", &CheckerOptions::root_package_only},
    {"noAssetChecks", &CheckerOptions::no_asset_checks}, {"usdcheckerCompat", &CheckerOptions::usdchecker_compat},
    {"requireAllGroups", &CheckerOptions::require_all_groups}};
  for (const auto& option : *options.object_items()) {
    const auto& key = option.key;
    const auto& value = option.value();
    bool flag = false;
    for (const auto& entry : flags) if (key == entry.name) {
      if (!value.is_boolean()) return fail(key + " must be a boolean");
      args.*(entry.member) = value.get_bool(); flag = true; break;
    }
    if (flag) continue;
    if (key == "arkit") {
      if (!value.is_boolean()) return fail(key + " must be a boolean");
      if (value.get_bool()) {
        args.groups.arkit = args.groups.core = args.groups.geom = true;
        args.groups.shade = args.groups.package = true;
      }
    } else if (key == "groups" || key == "variantSets" || key == "variants" || key == "includeKeywords") {
      if (!value.is_array() || value.empty()) return fail(key + " must be a nonempty array of strings");
      std::string joined;
      for (const auto& item : value) {
        if (!item.is_string() || item.get_string().empty() || item.get_string().find('\0') != std::string::npos)
          return fail(key + " must contain nonempty strings");
        if (key == "variants") {
          std::vector<std::pair<std::string, std::string>> pairs;
          for (const auto& entry : Split(item.get_string(), ',')) {
            const size_t colon = entry.find(':');
            if (colon == std::string::npos || !colon || colon + 1 >= entry.size())
              return fail("--variants entries must be set:variant pairs, got '" + entry + "'");
            pairs.emplace_back(entry.substr(0, colon), entry.substr(colon + 1));
          }
          args.variant_selections.push_back(std::move(pairs)); args.composed = true;
        } else { if (!joined.empty()) joined += ','; joined += item.get_string(); }
      }
      std::string error;
      if (key == "groups") {
        args.group_selection = true;
        if (!SetGroups(joined, &args.groups, &error)) return fail(error);
      } else if (key == "variantSets") {
        for (const auto& name : Split(joined, ',')) if (!name.empty()) args.variant_sets.push_back(name);
        args.composed = true;
      } else if (key == "includeKeywords") {
        args.group_selection = true; args.keywords = Split(joined, ',');
        args.groups = lightusd::next::ValidationOptions(); args.groups.core = false;
        std::string groups;
        for (const auto& keyword : args.keywords) {
          if (keyword.empty()) return fail("empty validator keyword");
          for (const auto& group : KeywordToGroups(keyword)) {
            if (!groups.empty()) groups += ',';
            groups += group;
          }
        }
        if (!groups.empty() && !SetGroups(groups, &args.groups, &error)) return fail(error);
      }
    } else if (key == "profile") {
      if (!value.is_string() || value.get_string().find('\0') != std::string::npos) return fail("profile must be a string without NUL bytes");
      args.profile = value.get_string();
    } else if (key == "maxSamples" || key == "maxMemoryMB") {
      if (!value.is_number_integer() || value.get_double() <= 0 ||
          value.get_double() > (key == "maxMemoryMB" ? 1024 : 1000000))
        return fail(key + " must be a positive integer within the supported limit");
      (key == "maxMemoryMB" ? args.max_memory_mb : args.max_samples) = size_t(value.get_uint64());
    } else if (key == "schemaDefinitions" || key == "shaderDefinitions") {
      if (!value.is_array()) return fail(key + " must be an array of JSON manifests");
      for (const auto& manifest : value) {
        if (!manifest.is_object()) return fail(key + " must contain JSON objects");
        auto loaded = key == "schemaDefinitions" ? registry.LoadSchemaDefinitions(manifest.dump())
                                                 : registry.LoadShaderDefinitions(manifest.dump());
        if (!loaded) return fail(loaded.error());
      }
    } else if (key == "baseline") {
      if (!value.is_object()) return fail("baseline must be a lusdchecker JSON report");
    } else if (key == "format") {
      if (!value.is_string() || (value.get_string() != "json" && value.get_string() != "sarif"))
        return fail("format must be json or sarif");
    } else return fail("unknown checker option: " + key);
  }
  std::string option_error;
  if (!NormalizeOptions(&args, &option_error)) args.usage_error = option_error;
  const size_t limit = size_t(options.contains("maxMemoryMB") ? options["maxMemoryMB"].get_uint64() : 1024) << 20;
  if (size > limit) return fail("input exceeds maxMemoryMB");
  // Snapshot the caller's registered assets. Never mutate the caller's store.
  Resolver assets = supplied ? *supplied : Resolver();
  assets.RegisterMemoryAsset(filename, size ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>());
  assets.SetCustomResolver([&](const std::string& path, const std::string& anchor) {
    const std::string candidate = Resolver::NormalizePath(Resolver::JoinPath(Resolver::GetDirectory(anchor), path));
    if (assets.GetMemoryAssetView(candidate)) return candidate;
    const std::string normalized = Resolver::NormalizePath(path);
    return assets.GetMemoryAssetView(normalized) ? normalized : std::string();
  });
  Json report;
  Environment environment;
  environment.resolver = &assets;
  environment.report = &report;
  environment.read = [&](const std::string& key, size_t cap, std::string* out, std::string* error) {
    return ReadMemory(assets, key, cap, out, error);
  };
  RunConfiguredChecker(std::move(args), registry, &environment);
  if (options.contains("baseline") && report["executionSuccessful"].get_bool()) {
    std::string error;
    if (!ApplyBaselineReport(&report, &options["baseline"], &error)) return fail(error);
  }
  return options["format"].get_string() == "sarif" ? ToSarif(report) : report;
}
}  // namespace lusdchecker
