// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Asset Resolution Implementation

#include "asset-resolver.hh"
#include "../safe-file-size.hh"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstring>
#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#endif

#ifdef _WIN32
#include <direct.h>
#define PATH_SEPARATOR '\\'
#define ALT_PATH_SEPARATOR '/'
#else
#include <unistd.h>
#include <sys/stat.h>
#define PATH_SEPARATOR '/'
#define ALT_PATH_SEPARATOR '\\'
#endif

namespace lightusd {
namespace next {

bool AssetPayloadBudget::SetLimit(uint64_t limit_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (limit_bytes < used_bytes_) return false;
  limit_bytes_ = limit_bytes;
  return true;
}

std::shared_ptr<void> AssetPayloadBudget::Reserve(uint64_t bytes) {
  if (bytes == 0) return {};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > limit_bytes_ - std::min(used_bytes_, limit_bytes_)) return {};
    used_bytes_ += bytes;
  }
  const auto owner = shared_from_this();
  return std::shared_ptr<void>(new uint8_t{0}, [owner, bytes](void* token) {
    delete static_cast<uint8_t*>(token);
    owner->Release(bytes);
  });
}

uint64_t AssetPayloadBudget::UsedBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return used_bytes_;
}

uint64_t AssetPayloadBudget::LimitBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return limit_bytes_;
}

void AssetPayloadBudget::Release(uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  used_bytes_ = bytes > used_bytes_ ? 0 : used_bytes_ - bytes;
}

namespace {

// Global default resolver
std::unique_ptr<AssetResolver> g_default_resolver;

bool FileExistsImpl(const std::string& path) {
#if defined(__EMSCRIPTEN__)
  // wasm builds may have no filesystem at all (-sFILESYSTEM=0), where even a
  // stat syscall aborts. Resolution there goes through the custom resolver
  // (asset-cache backed); plain file paths never exist.
  (void)path;
  return false;
#elif defined(_WIN32)
  std::ifstream f(path);
  return f.good();
#else
  // Follow symlinks: production USD asset trees commonly use them for shared
  // payload and texture roots. The subsequent reader still enforces its size
  // and format limits.
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool HasParentComponent(const std::string& path) {
  size_t begin = 0;
  while (begin <= path.size()) {
    size_t end = path.find_first_of("/\\", begin);
    if (end == std::string::npos) end = path.size();
    if (path.compare(begin, end - begin, "..") == 0) return true;
    if (end == path.size()) break;
    begin = end + 1;
  }
  return false;
}

std::string FindRecursively(const std::string& root,
                            const std::string& asset_path) {
#if defined(__EMSCRIPTEN__)
  (void)root;
  (void)asset_path;
  return {};
#else
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root_path(root);
  if (!fs::is_directory(root_path, ec) || ec) return {};

  const fs::path wanted(asset_path);
  std::vector<std::string> matches;
  fs::recursive_directory_iterator it(
      root_path, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    // Recursive search is a basename lookup. Authored directory suffixes are
    // already handled deterministically by the normal and suffix-fallback
    // paths before reaching this broader search.
    if (it->path().filename() == wanted.filename()) {
      matches.push_back(it->path().lexically_normal().string());
    }
  }
  if (matches.empty()) return {};
  std::sort(matches.begin(), matches.end());
  return matches.front();
#endif
}

std::string NormalizePackageEntryPath(std::string path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      (path.size() >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':')) {
    return {};
  }
  for (char& c : path) {
    if (c == '\\') c = '/';
  }

  std::vector<std::string> parts;
  size_t start = 0;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    std::string part = path.substr(start, end - start);
    if (part == "..") {
      if (parts.empty()) return {};
      parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    start = end + 1;
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += '/';
    out += parts[i];
  }
  return out;
}

std::string GetCurrentWorkingDirectory() {
#if defined(__EMSCRIPTEN__)
  // No filesystem (and the getcwd syscall ABORTS under -sFILESYSTEM=0).
  return ".";
#elif defined(_WIN32)
  char buffer[4096];
  if (_getcwd(buffer, sizeof(buffer))) {
    return std::string(buffer);
  }
#else
  char buffer[4096];
  if (getcwd(buffer, sizeof(buffer))) {
    return std::string(buffer);
  }
#endif
  return ".";
}

}  // namespace

// ============================================================
// AssetResolver
// ============================================================

AssetResolver::AssetResolver() {
  config_.working_directory = GetCurrentWorkingDirectory();
}

AssetResolver::AssetResolver(const ResolverConfig& config) : config_(config) {
  if (config_.working_directory.empty()) {
    config_.working_directory = GetCurrentWorkingDirectory();
  }
}

AssetResolver::AssetResolver(const AssetResolver& other) {
  std::lock_guard<std::mutex> lock(other.registry_mutex_);
  config_ = other.config_;
  custom_resolver_ = other.custom_resolver_;
  asset_reader_ = other.asset_reader_;
  scheme_handlers_ = other.scheme_handlers_;
  memory_assets_ = other.memory_assets_;
  next_anonymous_id_ = other.next_anonymous_id_;
}

AssetResolver& AssetResolver::operator=(const AssetResolver& other) {
  if (this == &other) return *this;
  std::scoped_lock lock(registry_mutex_, other.registry_mutex_);
  config_ = other.config_;
  custom_resolver_ = other.custom_resolver_;
  asset_reader_ = other.asset_reader_;
  scheme_handlers_ = other.scheme_handlers_;
  memory_assets_ = other.memory_assets_;
  next_anonymous_id_ = other.next_anonymous_id_;
  return *this;
}

AssetResolver::AssetResolver(AssetResolver&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.registry_mutex_);
  config_ = std::move(other.config_);
  custom_resolver_ = std::move(other.custom_resolver_);
  asset_reader_ = std::move(other.asset_reader_);
  scheme_handlers_ = std::move(other.scheme_handlers_);
  memory_assets_ = std::move(other.memory_assets_);
  next_anonymous_id_ = other.next_anonymous_id_;
}

AssetResolver& AssetResolver::operator=(AssetResolver&& other) noexcept {
  if (this == &other) return *this;
  std::scoped_lock lock(registry_mutex_, other.registry_mutex_);
  config_ = std::move(other.config_);
  custom_resolver_ = std::move(other.custom_resolver_);
  asset_reader_ = std::move(other.asset_reader_);
  scheme_handlers_ = std::move(other.scheme_handlers_);
  memory_assets_ = std::move(other.memory_assets_);
  next_anonymous_id_ = other.next_anonymous_id_;
  return *this;
}

AssetResolver::~AssetResolver() = default;

void AssetResolver::SetWorkingDirectory(const std::string& dir) {
  config_.working_directory = dir;
}

void AssetResolver::AddSearchPath(const std::string& path) {
  config_.search_paths.push_back(path);
}

void AssetResolver::SetSearchPaths(const std::vector<std::string>& paths) {
  config_.search_paths = paths;
}

void AssetResolver::ClearSearchPaths() {
  config_.search_paths.clear();
}

void AssetResolver::SetCustomResolver(ResolverCallback callback) {
  custom_resolver_ = std::move(callback);
}

void AssetResolver::SetAssetReader(AssetReadCallback reader) {
  asset_reader_ = std::move(reader);
}

bool AssetResolver::HasUserCallbacks() const {
  if (custom_resolver_ || asset_reader_) return true;
  std::lock_guard<std::mutex> lock(registry_mutex_);
  for (const auto& entry : scheme_handlers_) {
    if (entry.second.resolver || entry.second.reader) return true;
  }
  return false;
}

void AssetResolver::RegisterScheme(const std::string& scheme,
                                   ResolverCallback resolver,
                                   AssetReadCallback reader) {
  std::string normalized = scheme;
  if (!normalized.empty() && normalized.back() == ':') normalized.pop_back();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized.empty() ||
      GetIdentifierScheme(normalized + ":asset") != normalized) {
    return;
  }
  std::lock_guard<std::mutex> lock(registry_mutex_);
  scheme_handlers_[normalized] =
      SchemeHandler{std::move(resolver), std::move(reader)};
}

bool AssetResolver::UnregisterScheme(const std::string& scheme) {
  std::string normalized = scheme;
  if (!normalized.empty() && normalized.back() == ':') normalized.pop_back();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return scheme_handlers_.erase(normalized) != 0;
}

bool AssetResolver::HasScheme(const std::string& scheme) const {
  std::string normalized = scheme;
  if (!normalized.empty() && normalized.back() == ':') normalized.pop_back();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return scheme_handlers_.find(normalized) != scheme_handlers_.end();
}

std::string AssetResolver::RegisterMemoryAsset(const std::string& identifier,
                                               std::vector<uint8_t> bytes) {
  return RegisterMemoryAssetView(identifier,
      std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
}

std::string AssetResolver::RegisterMemoryAssetView(
    const std::string& identifier,
    std::shared_ptr<const std::vector<uint8_t>> bytes) {
  if (!bytes) return {};
  std::lock_guard<std::mutex> lock(registry_mutex_);
  std::string key = identifier;
  if (key.empty()) {
    key = "usd-anon:" + std::to_string(next_anonymous_id_++);
  }
  memory_assets_[key] = std::move(bytes);
  return key;
}

bool AssetResolver::UnregisterMemoryAsset(const std::string& identifier) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return memory_assets_.erase(identifier) != 0;
}

void AssetResolver::ClearMemoryAssets() {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  memory_assets_.clear();
}

std::shared_ptr<const std::vector<uint8_t>> AssetResolver::GetMemoryAssetView(
    const std::string& identifier) const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  const auto it = memory_assets_.find(identifier);
  return it == memory_assets_.end() ? nullptr : it->second;
}

bool AssetResolver::ReadAsset(const std::string& resolved_path,
                              std::vector<uint8_t>* out,
                              std::string* err) const {
  if (!out) {
    if (err) *err += "ReadAsset: null output buffer\n";
    return false;
  }
  out->clear();

  std::shared_ptr<const std::vector<uint8_t>> memory;
  AssetReadCallback scheme_reader;
  const std::string scheme = GetIdentifierScheme(resolved_path);
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto memory_it = memory_assets_.find(resolved_path);
    if (memory_it != memory_assets_.end()) memory = memory_it->second;
    const auto scheme_it = scheme_handlers_.find(scheme);
    if (scheme_it != scheme_handlers_.end()) scheme_reader = scheme_it->second.reader;
  }
  if (memory) {
    *out = *memory;
    return true;
  }
  if (scheme_reader) return scheme_reader(resolved_path, out, err);

  if (asset_reader_) {
    return asset_reader_(resolved_path, out, err);
  }

  if (!scheme.empty()) {
    if (err) *err += "ReadAsset: no reader registered for scheme `" + scheme +
                     "`: " + resolved_path + "\n";
    return false;
  }

#if defined(__EMSCRIPTEN__)
  // No filesystem under -sFILESYSTEM=0; bytes must come from a custom reader.
  if (err) {
    *err += "ReadAsset: no asset reader installed (wasm build has no "
            "filesystem): " + resolved_path + "\n";
  }
  return false;
#else
  std::ifstream f(resolved_path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (err) *err += "ReadAsset: failed to open: " + resolved_path + "\n";
    return false;
  }
  size_t size = 0;
  if (!SafeStreamSize(f, 0, &size)) {
    if (err) *err += "ReadAsset: implausible file size: " + resolved_path + "\n";
    return false;
  }
  f.seekg(0, std::ios::beg);
  out->resize(size);
  if (size > 0 &&
      !f.read(reinterpret_cast<char*>(out->data()), size)) {
    if (err) *err += "ReadAsset: short read: " + resolved_path + "\n";
    out->clear();
    return false;
  }
  return true;
#endif
}

ResolvedAsset AssetResolver::Resolve(const std::string& asset_path,
                                      const std::string& anchor_path,
                                      bool allow_suffix_fallback) const {
  return ResolveInternal(asset_path, anchor_path, allow_suffix_fallback);
}

std::string AssetResolver::ResolvePath(const std::string& asset_path,
                                        const std::string& anchor_path,
                                        bool allow_suffix_fallback) const {
  ResolvedAsset resolved =
      Resolve(asset_path, anchor_path, allow_suffix_fallback);
  return resolved.resolved_path;
}

bool AssetResolver::Exists(const std::string& asset_path,
                           const std::string& anchor_path) const {
  ResolvedAsset resolved = Resolve(asset_path, anchor_path);
  return resolved.exists;
}

std::string AssetResolver::CreateIdentifier(const std::string& asset_path,
                                             const std::string& anchor_path) const {
  const ResolvedAsset resolved = Resolve(asset_path, anchor_path);
  if (resolved.exists || IsAbsolutePath(asset_path) ||
      IsPackagePath(asset_path) || !GetIdentifierScheme(asset_path).empty()) {
    return resolved.resolved_path;
  }
  // Identifiers must remain context-dependent even when the asset has not
  // been created yet; otherwise two layers authoring the same relative token
  // collide in caches.
  const std::string base = anchor_path.empty()
                               ? config_.working_directory
                               : GetDirectory(anchor_path);
  return NormalizePath(JoinPath(base, asset_path));
}

ResolvedAsset AssetResolver::ResolveInternal(const std::string& asset_path,
                                              const std::string& anchor_path,
                                              bool allow_suffix_fallback) const {
  ResolvedAsset result;
  result.original_path = asset_path;

  if (asset_path.empty()) {
    return result;
  }

  // Reject `..` escapes before any lookup -- this is a containment check, so it
  // must run ahead of the scheme/memory-asset resolution below.
  if (!config_.allow_parent_paths && HasParentComponent(asset_path)) {
    return result;
  }

  ResolverCallback scheme_resolver;
  bool memory_asset = false;
  const std::string identifier_scheme = GetIdentifierScheme(asset_path);
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    memory_asset = memory_assets_.find(asset_path) != memory_assets_.end();
    const auto it = scheme_handlers_.find(identifier_scheme);
    if (it != scheme_handlers_.end()) scheme_resolver = it->second.resolver;
  }
  if (memory_asset) {
    result.resolved_path = asset_path;
    result.exists = true;
    return result;
  }

  // Check for package path (USDZ)
  if (IsPackagePath(asset_path)) {
    result.is_package = true;
    if (ParsePackagePath(asset_path, &result.package_path, &result.asset_in_package)) {
      result.asset_in_package =
          NormalizePackageEntryPath(result.asset_in_package);
      if (result.asset_in_package.empty()) return result;
      // Resolve the package file itself
      ResolvedAsset pkg = ResolveInternal(result.package_path, anchor_path,
                                          allow_suffix_fallback);
      if (pkg.exists) {
        result.resolved_path = pkg.resolved_path + "[" + result.asset_in_package + "]";
        result.package_path = pkg.resolved_path;
        result.exists = true;  // Assume asset exists in package if package exists
      }
    }
    return result;
  }

  // Explicit schemes never fall through into filesystem path handling.
  if (!identifier_scheme.empty()) {
    std::string scheme_result;
    if (scheme_resolver) {
      scheme_result = scheme_resolver(asset_path, anchor_path);
    } else if (custom_resolver_) {
      // Backward compatibility: the catch-all callback historically emulated
      // URI schemes before explicit registration existed.
      scheme_result = custom_resolver_(asset_path, anchor_path);
    }
    if (!scheme_result.empty()) {
      result.resolved_path = scheme_result;
      result.exists = true;
    } else {
      result.resolved_path = asset_path;
    }
    return result;
  }

  // Try custom resolver first
  if (custom_resolver_) {
    std::string custom_result = custom_resolver_(asset_path, anchor_path);
    if (!custom_result.empty()) {
      result.resolved_path = GetIdentifierScheme(custom_result).empty()
                                 ? NormalizePath(custom_result)
                                 : custom_result;
      // A custom resolver returning a path asserts the asset resolved; its
      // result need not be a filesystem path (e.g. a wasm asset-cache key),
      // so don't second-guess it with a file probe.
      result.exists = true;
      return result;
    }
  }

  // Absolute path
  if (IsAbsolutePath(asset_path)) {
    if (config_.allow_absolute_paths) {
      result.resolved_path = NormalizePath(asset_path);
      result.exists = FileExists(result.resolved_path);
    }
    // A missing absolute path falls through to the suffix fallback below
    // (assets authored with another machine's absolute prefix).
    if (result.exists ||
        !(allow_suffix_fallback && config_.enable_suffix_fallback)) {
      return result;
    }
  }

  // Relative paths authored inside a package layer resolve to another entry in
  // that same package. This lets pkg.usdz[root.usda] reference @geom.usda@
  // without escaping to the filesystem beside pkg.usdz.
  if (!anchor_path.empty() && IsPackagePath(anchor_path)) {
    std::string anchor_package;
    std::string anchor_entry;
    if (ParsePackagePath(anchor_path, &anchor_package, &anchor_entry)) {
      ResolvedAsset pkg;
      if (!config_.allow_absolute_paths && IsAbsolutePath(anchor_package)) {
        // The package path is not newly-authored input: it is the already
        // resolved container portion of `anchor_path`. Reusing that exact
        // container does not grant access to another absolute path.
        pkg.resolved_path = NormalizePath(anchor_package);
        pkg.exists = FileExists(pkg.resolved_path);
      } else {
        pkg = ResolveInternal(anchor_package, "");
      }
      if (pkg.exists) {
        const std::string entry_dir = GetDirectory(anchor_entry);
        std::string entry = (entry_dir == "." || entry_dir.empty())
                                ? asset_path
                                : JoinPath(entry_dir, asset_path);
        entry = NormalizePackageEntryPath(entry);
        if (entry.empty()) return result;
        result.is_package = true;
        result.package_path = pkg.resolved_path;
        result.asset_in_package = entry;
        result.resolved_path = pkg.resolved_path + "[" + entry + "]";
        result.exists = true;  // The package exists; entry validation happens on open.
        return result;
      }
    }
  }

  // Relative path - try relative to anchor first
  if (!anchor_path.empty()) {
    std::string anchor_dir = GetDirectory(anchor_path);
    std::string candidate = JoinPath(anchor_dir, asset_path);
    candidate = NormalizePath(candidate);
    if (FileExists(candidate)) {
      result.resolved_path = candidate;
      result.exists = true;
      return result;
    }
  }

  // Try relative to working directory
  {
    std::string candidate = JoinPath(config_.working_directory, asset_path);
    candidate = NormalizePath(candidate);
    if (FileExists(candidate)) {
      result.resolved_path = candidate;
      result.exists = true;
      return result;
    }
  }

  // Search in search paths
  for (const auto& search_path : config_.search_paths) {
    std::string candidate = JoinPath(search_path, asset_path);
    candidate = NormalizePath(candidate);
    if (FileExists(candidate)) {
      result.resolved_path = candidate;
      result.exists = true;
      return result;
    }
  }

  if (config_.search_recursively) {
    for (const auto& search_path : config_.search_paths) {
      const std::string candidate = FindRecursively(search_path, asset_path);
      if (!candidate.empty()) {
        result.resolved_path = NormalizePath(candidate);
        result.exists = true;
        return result;
      }
    }
  }

  // Suffix fallback: rehome paths authored with absolute / machine-specific
  // prefixes by retrying progressively shorter suffixes (down to the
  // basename) through the same anchor / working-dir / search-path order.
  if (allow_suffix_fallback && config_.enable_suffix_fallback) {
    for (const std::string& suffix : SuffixCandidates(asset_path)) {
      if (suffix == asset_path) continue;
      ResolvedAsset alt =
          ResolveInternal(suffix, anchor_path, /*allow_suffix_fallback=*/false);
      if (alt.exists) {
        alt.original_path = asset_path;
        return alt;
      }
    }
  }

  // Not found - return original path normalized
  result.resolved_path = NormalizePath(asset_path);
  result.exists = false;
  return result;
}

bool AssetResolver::FileExists(const std::string& path) const {
  return FileExistsImpl(path);
}

// ============================================================
// Static path utilities
// ============================================================

bool AssetResolver::IsAbsolutePath(const std::string& path) {
  if (path.empty()) return false;

#ifdef _WIN32
  // Windows: C:\ or \\network or /path
  if (path.length() >= 2 && path[1] == ':') return true;
  if (path.length() >= 2 && path[0] == '\\' && path[1] == '\\') return true;
#endif

  return path[0] == '/' || path[0] == '\\';
}

std::string AssetResolver::GetIdentifierScheme(
    const std::string& identifier) {
  const size_t colon = identifier.find(':');
  if (colon == std::string::npos || colon == 0) return {};
  // Do not classify Windows drive paths as URI schemes on any host platform.
  if (colon == 1 && std::isalpha(static_cast<unsigned char>(identifier[0])) &&
      identifier.size() > 2 &&
      (identifier[2] == '/' || identifier[2] == '\\')) {
    return {};
  }
  if (!std::isalpha(static_cast<unsigned char>(identifier[0]))) return {};
  for (size_t i = 1; i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(identifier[i]);
    if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return {};
  }
  std::string scheme = identifier.substr(0, colon);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return scheme;
}

bool AssetResolver::IsPackagePath(const std::string& path) {
  // Package paths look like: /path/to/file.usdz[asset.usda]
  size_t bracket = path.find('[');
  if (bracket == std::string::npos || bracket == 0) return false;
  const size_t end = path.find(']', bracket);
  return end != std::string::npos && end == path.size() - 1 &&
         end > bracket + 1;
}

bool AssetResolver::ParsePackagePath(const std::string& path,
                                      std::string* package_file,
                                      std::string* asset_in_package) {
  size_t bracket_start = path.find('[');
  if (bracket_start == std::string::npos) return false;

  size_t bracket_end = path.find(']', bracket_start);
  if (bracket_end == std::string::npos || bracket_end + 1 != path.size() ||
      bracket_start == 0 || bracket_end == bracket_start + 1 ||
      !package_file || !asset_in_package) {
    return false;
  }

  *package_file = path.substr(0, bracket_start);
  *asset_in_package = path.substr(bracket_start + 1, bracket_end - bracket_start - 1);
  return true;
}

std::string AssetResolver::JoinPath(const std::string& base, const std::string& path) {
  if (base.empty()) return path;
  if (path.empty()) return base;

  char last = base.back();
  if (last == '/' || last == '\\') {
    return base + path;
  }
  return base + PATH_SEPARATOR + path;
}

std::string AssetResolver::GetDirectory(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return ".";
  return path.substr(0, pos);
}

std::string AssetResolver::GetFilename(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

std::string AssetResolver::NormalizePath(const std::string& path) {
  if (path.empty()) return path;

  std::string result = path;

  // Normalize separators
  for (char& c : result) {
    if (c == ALT_PATH_SEPARATOR) {
      c = PATH_SEPARATOR;
    }
  }

  // Handle . and ..
  std::vector<std::string> parts;
  size_t start = 0;

  // Preserve leading slashes for absolute paths
  size_t prefix_len = 0;
  if (!result.empty() && result[0] == PATH_SEPARATOR) {
    prefix_len = 1;
#ifdef _WIN32
    if (result.length() >= 2 && result[1] == PATH_SEPARATOR) {
      prefix_len = 2;  // UNC path
    }
#endif
  }
#ifdef _WIN32
  // Handle drive letter
  if (result.length() >= 2 && result[1] == ':') {
    prefix_len = 2;
    if (result.length() >= 3 && result[2] == PATH_SEPARATOR) {
      prefix_len = 3;
    }
  }
#endif

  start = prefix_len;

  while (start < result.length()) {
    size_t end = result.find(PATH_SEPARATOR, start);
    if (end == std::string::npos) {
      end = result.length();
    }

    std::string part = result.substr(start, end - start);

    if (part == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (prefix_len == 0) {
        parts.push_back(part);
      }
    } else if (part != "." && !part.empty()) {
      parts.push_back(part);
    }

    start = end + 1;
  }

  // Reconstruct path
  std::string normalized = result.substr(0, prefix_len);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0 || (prefix_len > 0 && normalized.back() != PATH_SEPARATOR)) {
      normalized += PATH_SEPARATOR;
    }
    normalized += parts[i];
  }

  if (normalized.empty()) {
    normalized = ".";
  }

  return normalized;
}

std::vector<std::string> AssetResolver::SuffixCandidates(
    const std::string& asset_path) {
  std::vector<std::string> candidates;
  if (asset_path.empty()) return candidates;

  std::string p = asset_path;
  std::replace(p.begin(), p.end(), '\\', '/');

  // Strip a Windows drive prefix (e.g. "F:")
  if ((p.size() >= 2) &&
      (((p[0] >= 'A') && (p[0] <= 'Z')) || ((p[0] >= 'a') && (p[0] <= 'z'))) &&
      (p[1] == ':')) {
    p = p.substr(2);
  }

  // Strip leading '/', './' and '../' runs.
  size_t pos = 0;
  while (pos < p.size()) {
    if (p[pos] == '/') {
      pos++;
    } else if (p.compare(pos, 2, "./") == 0) {
      pos += 2;
    } else if (p.compare(pos, 3, "../") == 0) {
      pos += 3;
    } else {
      break;
    }
  }
  p = p.substr(pos);

  // Emit progressively shorter suffixes, longest first, down to the basename.
  while (p.size()) {
    if (p != asset_path) {
      candidates.push_back(p);
    }
    size_t slash_loc = p.find('/');
    if (slash_loc == std::string::npos) {
      break;
    }
    p = p.substr(slash_loc + 1);
  }

  return candidates;
}

std::string AssetResolver::MakeRelative(const std::string& path, const std::string& base) {
  std::string norm_path = NormalizePath(path);
  std::string norm_base = NormalizePath(base);

  // Simple implementation - just check if path starts with base
  if (norm_path.find(norm_base) == 0) {
    size_t base_len = norm_base.length();
    if (base_len < norm_path.length()) {
      if (norm_path[base_len] == PATH_SEPARATOR) {
        return norm_path.substr(base_len + 1);
      }
    }
  }

  return path;
}

// ============================================================
// Global resolver
// ============================================================

AssetResolver& GetDefaultResolver() {
  if (!g_default_resolver) {
    g_default_resolver = std::make_unique<AssetResolver>();
  }
  return *g_default_resolver;
}

void SetDefaultResolver(std::unique_ptr<AssetResolver> resolver) {
  g_default_resolver = std::move(resolver);
}

}  // namespace next
}  // namespace lightusd
