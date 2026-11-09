// SPDX-License-Identifier: Apache-2.0
#include "preview_cache.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include "external/jsonhpp/nlohmann/json.hpp"

namespace lusdview {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr int kFormatVersion = 1;

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value && *value ? std::string(value) : std::string();
}

uint64_t HashString(uint64_t hash, const std::string& value) {
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

std::string Hex(uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string Canonical(const std::string& path) {
  std::error_code ec;
  fs::path result = fs::weakly_canonical(fs::path(path), ec);
  if (ec) result = fs::absolute(fs::path(path), ec);
  return ec ? path : result.string();
}

std::string StatPath(const std::string& identifier) {
  if (identifier.find("://") != std::string::npos) return {};
  const size_t package = identifier.find('[');
  return Canonical(identifier.substr(0, package));
}

bool FileMetadata(const std::string& identifier, uintmax_t* size,
                  int64_t* mtime) {
  const std::string path = StatPath(identifier);
  if (path.empty()) return false;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) return false;
  const uintmax_t file_size = fs::file_size(path, ec);
  if (ec) return false;
  const fs::file_time_type write_time = fs::last_write_time(path, ec);
  if (ec) return false;
  *size = file_size;
  *mtime = static_cast<int64_t>(write_time.time_since_epoch().count());
  return true;
}

fs::path BasePath(const PreviewCacheOptions& options, const std::string& key) {
  return fs::path(options.directory.empty() ? DefaultPreviewCacheDirectory()
                                            : options.directory) /
         key;
}

void Evict(const PreviewCacheOptions& options) {
  if (options.maxBytes == 0) return;
  const fs::path directory = options.directory.empty()
                                 ? DefaultPreviewCacheDirectory()
                                 : options.directory;
  std::error_code ec;
  if (!fs::is_directory(directory, ec)) return;
  struct Entry {
    fs::path path;
    uintmax_t size;
    fs::file_time_type time;
  };
  std::vector<Entry> entries;
  uintmax_t total = 0;
  for (const fs::directory_entry& item : fs::directory_iterator(directory, ec)) {
    if (ec || item.path().extension() != ".usdc") continue;
    const uintmax_t size = item.file_size(ec);
    if (ec) { ec.clear(); continue; }
    entries.push_back({item.path(), size, item.last_write_time(ec)});
    if (ec) { ec.clear(); entries.back().time = fs::file_time_type::min(); }
    total += size;
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.time < b.time;
  });
  for (const Entry& entry : entries) {
    if (total <= options.maxBytes) break;
    fs::remove(entry.path, ec);
    fs::remove(entry.path.parent_path() /
                   (entry.path.stem().string() + ".json"), ec);
    total -= std::min<uintmax_t>(total, entry.size);
  }
}

}  // namespace

std::string DefaultPreviewCacheDirectory() {
#if defined(_WIN32)
  std::string base = Env("LOCALAPPDATA");
  if (base.empty()) base = Env("USERPROFILE");
  return base.empty() ? std::string() : (fs::path(base) / "lusdview" / "previews").string();
#elif defined(__APPLE__)
  const std::string home = Env("HOME");
  return home.empty() ? std::string()
                      : (fs::path(home) / "Library" / "Caches" / "lusdview" /
                         "previews").string();
#else
  std::string base = Env("XDG_CACHE_HOME");
  if (base.empty()) {
    const std::string home = Env("HOME");
    if (home.empty()) return {};
    base = (fs::path(home) / ".cache").string();
  }
  return (fs::path(base) / "lusdview" / "previews").string();
#endif
}

std::string PreviewCacheFingerprint(const std::string& root,
                                    const std::string& compositionOptions) {
  uint64_t hash = UINT64_C(14695981039346656037);
  hash = HashString(hash, "lusdview-preview-v1\n");
  hash = HashString(hash, Canonical(root));
  hash = HashString(hash, "\n" + compositionOptions);
  return Hex(hash);
}

PreviewCacheLookup LoadPreviewCache(const PreviewCacheOptions& options,
                                    const std::string& root,
                                    const std::string& fingerprint) {
  PreviewCacheLookup result;
  result.key = PreviewCacheFingerprint(root, fingerprint);
  if (options.mode == PreviewCacheMode::Off) { result.reason = "disabled"; return result; }
  if (options.mode == PreviewCacheMode::Refresh) { result.reason = "refresh requested"; return result; }
  const fs::path base = BasePath(options, result.key);
  const fs::path manifest_path = base.string() + ".json";
  const fs::path stage_path = base.string() + ".usdc";
  std::ifstream input(manifest_path);
  if (!input) { result.reason = "manifest missing"; return result; }
  json manifest = json::parse(input, nullptr, false);
  if (manifest.is_discarded() || !manifest.is_object()) {
    result.reason = "manifest malformed"; return result;
  }
  if (!manifest.contains("format") || !manifest["format"].is_number_integer() ||
      !manifest.contains("root") || !manifest["root"].is_string() ||
      !manifest.contains("fingerprint") ||
      !manifest["fingerprint"].is_string()) {
    result.reason = "manifest malformed"; return result;
  }
  if (manifest.value("format", 0) != kFormatVersion ||
      manifest.value("root", std::string()) != Canonical(root) ||
      manifest.value("fingerprint", std::string()) != fingerprint) {
    result.reason = "manifest identity mismatch"; return result;
  }
  const auto dependencies = manifest.find("dependencies");
  if (dependencies == manifest.end() || !dependencies->is_array()) {
    result.reason = "dependency manifest missing"; return result;
  }
  for (const json& dependency : *dependencies) {
    if (!dependency.is_object()) { result.reason = "dependency malformed"; return result; }
    if (!dependency.contains("id") || !dependency["id"].is_string() ||
        !dependency.contains("size") ||
        !dependency["size"].is_number_unsigned() ||
        !dependency.contains("mtime") ||
        !dependency["mtime"].is_number_integer()) {
      result.reason = "dependency malformed"; return result;
    }
    const std::string id = dependency.value("id", std::string());
    uintmax_t size = 0;
    int64_t mtime = 0;
    if (!FileMetadata(id, &size, &mtime)) {
      result.reason = "dependency unavailable: " + id; return result;
    }
    if (size != dependency.value("size", uintmax_t(0)) ||
        mtime != dependency.value("mtime", int64_t(0))) {
      result.reason = "dependency changed: " + id; return result;
    }
  }
  lightusd_load_options load_options;
  lightusd_load_options_init(&load_options);
  load_options.format = LIGHTUSD_FORMAT_USDC;
  if (result.stage.load(stage_path.string().c_str(), &load_options) != LIGHTUSD_OK) {
    result.reason = std::string("preview USDC invalid: ") + lightusd::api::LastError(); return result;
  }
  result.hit = true;
  result.reason = "hit";
  std::error_code ec;
  fs::last_write_time(stage_path, fs::file_time_type::clock::now(), ec);
  return result;
}

bool StorePreviewCache(const PreviewCacheOptions& options,
                       const std::string& root,
                       const std::string& fingerprint,
                       const lightusd::api::Stage& preview,
                       const std::vector<std::string>& dependencies,
                       std::string* reason) {
  if (options.mode == PreviewCacheMode::Off) return false;
  const std::string directory = options.directory.empty()
                                    ? DefaultPreviewCacheDirectory()
                                    : options.directory;
  if (directory.empty()) { if (reason) *reason = "cache directory unavailable"; return false; }
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) { if (reason) *reason = "cannot create cache directory"; return false; }
  const std::string key = PreviewCacheFingerprint(root, fingerprint);
  const fs::path base = fs::path(directory) / key;
  const fs::path stage_path = base.string() + ".usdc";
  const fs::path manifest_path = base.string() + ".json";
  const fs::path stage_tmp = base.string() + ".usdc.tmp";
  const fs::path manifest_tmp = base.string() + ".json.tmp";

  json manifest;
  manifest["format"] = kFormatVersion;
  manifest["root"] = Canonical(root);
  manifest["fingerprint"] = fingerprint;
  manifest["dependencies"] = json::array();
  for (const std::string& id : dependencies) {
    uintmax_t size = 0;
    int64_t mtime = 0;
    if (!FileMetadata(id, &size, &mtime)) {
      if (reason) *reason = "dependency is not cacheable: " + id;
      return false;
    }
    manifest["dependencies"].push_back({{"id", id}, {"size", size}, {"mtime", mtime}});
  }
  lightusd_save_options save_options;
  lightusd_save_options_init(&save_options);
  save_options.format = LIGHTUSD_FORMAT_USDC;
  if (preview.save(stage_tmp.string().c_str(), &save_options) != LIGHTUSD_OK) {
    if (reason) *reason = std::string("preview write failed: ") + lightusd::api::LastError();
    return false;
  }
  {
    std::ofstream output(manifest_tmp, std::ios::binary | std::ios::trunc);
    if (!output) { fs::remove(stage_tmp, ec); if (reason) *reason = "manifest write failed"; return false; }
    output << manifest.dump(2) << '\n';
    if (!output) { fs::remove(stage_tmp, ec); fs::remove(manifest_tmp, ec); if (reason) *reason = "manifest write failed"; return false; }
  }
  fs::remove(stage_path, ec);
  ec.clear();
  fs::rename(stage_tmp, stage_path, ec);
  if (ec) { fs::remove(stage_tmp, ec); fs::remove(manifest_tmp, ec); if (reason) *reason = "preview commit failed"; return false; }
  fs::remove(manifest_path, ec);
  ec.clear();
  fs::rename(manifest_tmp, manifest_path, ec);
  if (ec) { fs::remove(manifest_tmp, ec); fs::remove(stage_path, ec); if (reason) *reason = "manifest commit failed"; return false; }
  Evict(options);
  if (reason) *reason = "stored";
  return true;
}

}  // namespace lusdview
