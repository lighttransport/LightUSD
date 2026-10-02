// SPDX-License-Identifier: Apache-2.0
#include "checker.hh"
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
  const std::map<std::string, std::string> flags = {
    {"strict", "--strict"}, {"strictParse", "--strict-parse"}, {"composed", "--composed"},
    {"allTimeSamples", "--all-time-samples"}, {"skipVariants", "--skip-variants"},
    {"rootPackageOnly", "--root-package-only"}, {"noAssetChecks", "--no-asset-checks"},
    {"usdcheckerCompat", "--usdchecker-compat"}, {"requireAllGroups", "--require-all-groups"},
    {"arkit", "--arkit"}};
  const std::map<std::string, std::string> lists = {
    {"groups", "--groups"}, {"variantSets", "--variant-sets"},
    {"variants", "--variants"}, {"includeKeywords", "--include-keywords"}};
  std::vector<std::string> args{"lusdchecker", "--json"};
  auto registry = lightusd::next::GetBuiltinValidationRegistry();
  for (const auto& option : *options.object_items()) {
    const auto& key = option.key;
    const auto& value = option.value();
    if (flags.count(key)) {
      if (!value.is_boolean()) return fail(key + " must be a boolean");
      if (value.get_bool()) args.push_back(flags.at(key));
    } else if (lists.count(key)) {
      if (!value.is_array() || value.empty()) return fail(key + " must be a nonempty array of strings");
      std::string joined;
      for (const auto& item : value) {
        if (!item.is_string() || item.get_string().empty() || item.get_string().find('\0') != std::string::npos)
          return fail(key + " must contain nonempty strings");
        if (key == "variants") { args.push_back(lists.at(key)); args.push_back(item.get_string()); }
        else { if (!joined.empty()) joined += ','; joined += item.get_string(); }
      }
      if (key != "variants") { args.push_back(lists.at(key)); args.push_back(joined); }
    } else if (key == "profile") {
      if (!value.is_string() || value.get_string().find('\0') != std::string::npos) return fail("profile must be a string without NUL bytes");
      args.push_back("--profile"); args.push_back(value.get_string());
    } else if (key == "maxSamples" || key == "maxMemoryMB") {
      if (!value.is_number_integer() || value.get_double() <= 0 ||
          value.get_double() > (key == "maxMemoryMB" ? 1024 : 1000000))
        return fail(key + " must be a positive integer within the supported limit");
      args.push_back(key == "maxSamples" ? "--max-samples" : "--max-memory-mb");
      args.push_back(std::to_string(value.get_uint64()));
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
  args.push_back(filename);
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(&arg[0]);
  RunChecker(static_cast<int>(argv.size()), argv.data(), registry, &environment);
  if (options.contains("baseline") && report["executionSuccessful"].get_bool()) {
    std::string error;
    if (!ApplyBaselineReport(&report, &options["baseline"], &error)) return fail(error);
  }
  return options["format"].get_string() == "sarif" ? ToSarif(report) : report;
}
}  // namespace lusdchecker
