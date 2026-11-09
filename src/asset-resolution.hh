// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Light Transport Entertainment, Inc.

///
/// @file asset-resolution.hh
/// @brief Asset resolution system for USD files and resources
///
/// Provides abstraction for loading assets from various sources (filesystem,
/// memory, URLs, custom storage, etc.). Similar to ArAsset system in pxrUSD.
///
/// Key classes:
/// - Asset: Abstract asset container with data buffer
/// - AssetResolutionResolver: Resolver for finding and loading assets
/// - FileSystemHandler: Interface for custom file systems
///
/// The system allows USD to load assets from:
/// - Local filesystem
/// - Memory buffers
/// - Network URLs
/// - Custom storage backends (databases, cloud storage, etc.)
///
/// Note: To avoid confusion with AR (Augmented Reality), we don't use
/// abbreviations like `ar`, `Ar` and `AR`.
///
/// Reference: https://graphics.pixar.com/usd/release/api/ar_page_front.html
///
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "value-types.hh"

namespace lightusd {

///
/// Abstract class for asset(e.g. file, memory, uri, ...)
/// Similar to ArAsset in pxrUSD.
///
class Asset {
 public:
  size_t size() const { return buf_.size(); }

  const uint8_t *data() const LIGHTUSD_LIFETIMEBOUND { return buf_.data(); }

  uint8_t *data() LIGHTUSD_LIFETIMEBOUND { return buf_.data(); }

  void resize(size_t sz) { buf_.resize(sz); }

  void shrink_to_fit() { buf_.shrink_to_fit(); }

  void set_data(std::vector<uint8_t> &&rhs) {
    buf_ = std::move(rhs);
  }

  void set_data(const std::vector<uint8_t> &rhs) {
    buf_ = rhs;
  }

  // Move the underlying byte buffer out, leaving this Asset empty. Use when the
  // Asset is about to be discarded and its bytes can be reused without a copy.
  std::vector<uint8_t> release_buffer() { return std::move(buf_); }

  void set_name(const std::string &name) {
    name_ = name;
  }

  void set_resolved_name(const std::string &name) {
    resolved_name_ = name;
  }

  const std::string &name() const LIGHTUSD_LIFETIMEBOUND {
    return name_;
  }

  const std::string &resolved_name() const LIGHTUSD_LIFETIMEBOUND {
    return resolved_name_;
  }

  void set_version(const std::string &version) {
    version_ = version;
  }

  const std::string &version() const LIGHTUSD_LIFETIMEBOUND {
    return version_;
  }

 private:
  std::string version_; // optional.
  std::string name_;
  std::string resolved_name_;
  std::vector<uint8_t> buf_;
};


struct ResolverAssetInfo {
  std::string version;
  std::string assetName;

  value::Value resolverInfo;
};

///
/// For easier language bindings(e.g. for C), we use simple callback function
/// approach.
///

// Resolve asset.
//
// @param[in] asset_name Asset name or filepath
// @param[in] search_paths Search paths.
// @param[out] resolved_asset_name Resolved asset name.
// @param[out] err Error message.
// @param[inout] userdata Userdata.
//
// @return 0 upon success. -1 = asset cannot be resolved(not found). other negative value = error
typedef int (*FSResolveAsset)(const char *asset_name, const std::vector<std::string> &search_paths, std::string *resolved_asset_name,
                          std::string *err, void *userdata);

// @param[in] resolved_asset_name Resolved Asset name or filepath
// @param[out] nbytes Bytes of this asset.
// @param[out] err Error message.
// @param[inout] userdata Userdata.
//
// @return 0 upon success. negative value = error
typedef int (*FSSizeAsset)(const char *resolved_asset_name, uint64_t *nbytes,
                          std::string *err, void *userdata);

// @param[in] resolved_asset_name Resolved Asset name or filepath
// @param[in] req_nbytes Required bytes for output buffer.
// @param[out] out_buf Output buffer. Memory should be allocated before calling this functione(`req_nbytes` or more)
// @param[out] nbytes Read bytes. 0 <= nbytes <= `req_nbytes`
// @param[out] err Error message.
// @param[inout] userdata Userdata.
//
// @return 0 upon success. negative value = error
typedef int (*FSReadAsset)(const char *resolved_asset_name, uint64_t req_nbytes, uint8_t *out_buf,
                          uint64_t *nbytes, std::string *err, void *userdata);

// @param[in] asset_name Asset name or filepath(could be empty)
// @param[in] resolved_asset_name Resolved Asset name or filepath
// @param[in] buffer Data.
// @param[in] nbytes Data bytes.
// @param[out] err Error message.
// @param[inout] userdata Userdata.
//
// @return 0 upon success. negative value = error
typedef int (*FSWriteAsset)(const char *asset_name, const char *resolved_asset_name, const uint8_t *buffer,
                           const uint64_t nbytes, std::string *err, void *userdata);

struct AssetResolutionHandler {
  FSResolveAsset resolve_fun{nullptr};
  FSSizeAsset size_fun{nullptr};
  FSReadAsset read_fun{nullptr};
  FSWriteAsset write_fun{nullptr};
  void *userdata{nullptr};
};


class AssetResolutionResolver {
 public:
  AssetResolutionResolver() = default;
  ~AssetResolutionResolver() {}

  AssetResolutionResolver(const AssetResolutionResolver &rhs) {
    if (this != &rhs) {
      //_resolve_path_handler = rhs._resolve_path_handler;
      _asset_resolution_handlers = rhs._asset_resolution_handlers;
      _userdata = rhs._userdata;
      _search_paths = rhs._search_paths;
      _current_working_path = rhs._current_working_path;
      _max_asset_bytes_in_mb = rhs._max_asset_bytes_in_mb;
      _max_file_descriptors = rhs._max_file_descriptors;
      _enable_suffix_fallback = rhs._enable_suffix_fallback;
      _allow_parent_relative_paths = rhs._allow_parent_relative_paths;
      _cached_resolved_paths.clear();
    }
  }

  AssetResolutionResolver &operator=(const AssetResolutionResolver &rhs) {
    if (this != &rhs) {
      // _resolve_path_handler = rhs._resolve_path_handler;
      _asset_resolution_handlers = rhs._asset_resolution_handlers;
      _userdata = rhs._userdata;
      _search_paths = rhs._search_paths;
      _current_working_path = rhs._current_working_path;
      _max_asset_bytes_in_mb = rhs._max_asset_bytes_in_mb;
      _max_file_descriptors = rhs._max_file_descriptors;
      _enable_suffix_fallback = rhs._enable_suffix_fallback;
      _allow_parent_relative_paths = rhs._allow_parent_relative_paths;
      _cached_resolved_paths.clear();
    }
    return (*this);
  }

  AssetResolutionResolver &operator=(AssetResolutionResolver &&rhs) noexcept {
    if (this != &rhs) {
      //_resolve_path_handler = rhs._resolve_path_handler;
      _asset_resolution_handlers = rhs._asset_resolution_handlers;
      _userdata = rhs._userdata;
      _search_paths = std::move(rhs._search_paths);
      _current_working_path = std::move(rhs._current_working_path);
      _max_asset_bytes_in_mb = rhs._max_asset_bytes_in_mb;
      _max_file_descriptors = rhs._max_file_descriptors;
      _enable_suffix_fallback = rhs._enable_suffix_fallback;
      _allow_parent_relative_paths = rhs._allow_parent_relative_paths;
      _cached_resolved_paths.clear();
    }
    return (*this);
  }

  // LightUSD does not provide global search paths at the moment.
  // static void SetDefaultSearchPath(const std::vector<std::string> &p);

  void set_search_paths(const std::vector<std::string> &paths) {
    // TODO: Validate input paths.
    _search_paths = paths;
    clear_resolution_cache();
  }

  void add_search_path(const std::string &path) {
    _search_paths.push_back(path);
    clear_resolution_cache();
  }

  //
  // Asset is first seeked from the current working path(directory) when the Asset's path is a relative path.
  //
  void set_current_working_path(const std::string &cwp) {
    _current_working_path = cwp;
    clear_resolution_cache();
  }

  const std::string &current_working_path() const LIGHTUSD_LIFETIMEBOUND {
    return _current_working_path;
  }

  const std::vector<std::string> &search_paths() const LIGHTUSD_LIFETIMEBOUND {
    return _search_paths;
  }

  std::string search_paths_str() const;

  ///
  /// Register user defined AssetResolution handler per file extension.
  /// Default = use built-in file handler(FILE/ifstream)
  /// This handler is used in resolve(), find() and open_asset()
  ///
  void register_asset_resolution_handler(const std::string &ext_name, AssetResolutionHandler handler) {
    if (ext_name.empty()) {
      return;
    }
    _asset_resolution_handlers[ext_name] = handler;
    clear_resolution_cache();
  }

  void register_wildcard_asset_resolution_handler(AssetResolutionHandler handler) {
    _asset_resolution_handlers["*"] = handler;
    clear_resolution_cache();
  }

  bool unregister_asset_resolution_handler(const std::string &ext_name) {
    if (_asset_resolution_handlers.count(ext_name)) {
      _asset_resolution_handlers.erase(ext_name);
      clear_resolution_cache();
      return true;
    }
    return false;
  }

  bool unregister_wildcard_asset_resolution_handler() {
    if (_asset_resolution_handlers.count("*")) {
      _asset_resolution_handlers.erase("*");
      clear_resolution_cache();
      return true;
    }
    return false;
  }


  bool has_asset_resolution_handler(const std::string &ext_name) {
    if (_asset_resolution_handlers.count(ext_name)) {
      return true;
    }
    return false;
  }

  bool has_wildcard_asset_resolution_handler() {
    if (_asset_resolution_handlers.count("*")) {
      return true;
    }
    return false;
  }



  ///
  /// Check if input asset exists(do asset resolution inside the function).
  ///
  /// @param[in] assetPath Asset path string(e.g. "bora.png",
  /// "/mnt/c/sphere.usd")
  ///
  bool find(const std::string &assetPath) const;

  ///
  /// Resolve asset path and returns resolved path as string.
  /// Returns empty string when the asset does not exit.
  ///
  std::string resolve(const std::string &assetPath) const;

  ///
  /// resolve() variant which reports whether the result was found through the
  /// suffix fallback (see `set_enable_suffix_fallback`) instead of the
  /// literally-authored path. `used_suffix_fallback` may be nullptr.
  ///
  std::string resolve(const std::string &assetPath,
                      bool *used_suffix_fallback) const;

  ///
  /// When enabled(default), an asset path which fails literal resolution is
  /// retried with its un-anchorable prefix(leading `../` runs, Windows drive)
  /// stripped, dropping leading directory components one at a time(longest
  /// suffix first) until a candidate resolves against the search paths.
  /// This rebases composition arcs authored against another machine's
  /// directory layout(e.g. UnrealEngine USD exports) onto the local scene
  /// root. Only kicks in when the literal path resolves to nothing.
  ///
  void set_enable_suffix_fallback(bool enable) {
    _enable_suffix_fallback = enable;
    clear_resolution_cache();
  }

  bool get_enable_suffix_fallback() const { return _enable_suffix_fallback; }

  ///
  /// Allow parent-relative(`..`) asset paths?
  ///
  /// When disabled(default), asset-path sanitizers(e.g.
  /// tydra::utils::SanitizeAssetPath) reject any path whose `..` segments
  /// escape the anchoring root, as a path-traversal guard. When enabled, a
  /// leading `..` that cannot be lexically collapsed is preserved so the
  /// resolver can rebase it against its base dir / search paths. This mirrors
  /// the composition-layer `allow_parent_relative_paths` option, so downstream
  /// texture/light asset loading honors the same policy the caller chose for
  /// composition (e.g. usd-wg/assets `../_common/*` references).
  ///
  void set_allow_parent_relative_paths(bool allow) {
    _allow_parent_relative_paths = allow;
  }

  bool get_allow_parent_relative_paths() const {
    return _allow_parent_relative_paths;
  }

 private:
  // resolve() without the suffix fallback(literally-authored path only).
  std::string resolve_literal(const std::string &assetPath) const;
  void clear_resolution_cache() const { _cached_resolved_paths.clear(); }

 public:

  ///
  /// Open asset from the resolved Path.
  ///
  /// @param[in] resolvedPath Resolved path(through `resolve()`)
  /// @param[in] assetPath Asset path(could be empty)
  /// @param[out] asset Asset.
  /// @param[out] warn Warning.
  /// @param[out] err Error message.
  ///
  /// @return true upon success.
  ///
  bool open_asset(const std::string &resolvedPath, const std::string &assetPath,
                  Asset *asset, std::string *warn, std::string *err) const;

  // Additional per-call cap, enforced before allocation for built-in and sized custom handlers.
  bool open_asset(const std::string& resolvedPath, const std::string& assetPath,
                  Asset*, std::string* warn, std::string* err, size_t max_bytes) const;

  void set_userdata(void *userdata) { _userdata = userdata; }
  void *get_userdata() { return _userdata; }
  const void *get_userdata() const { return _userdata; }

  void set_max_asset_bytes_in_mb(size_t megabytes) {
    if (megabytes > 0) {
      _max_asset_bytes_in_mb = megabytes;
    }
  }

  size_t get_max_asset_bytes_in_mb() const {
    return _max_asset_bytes_in_mb;
  }

  /// Maximum number of concurrently open file descriptors / asset handles
  /// allowed through this resolver. The built-in filesystem loader opens one
  /// descriptor per `open_asset()` call and closes it before returning; custom
  /// handlers are counted as one logical handle per call. Set to 0 to reject
  /// all opens (useful for tests).
  void set_max_file_descriptors(uint32_t max_file_descriptors) {
    _max_file_descriptors = max_file_descriptors;
  }

  uint32_t get_max_file_descriptors() const {
    return _max_file_descriptors;
  }

  uint32_t current_open_file_descriptors() const {
    return _open_file_descriptors.load();
  }

 private:
  //ResolvePathHandler _resolve_path_handler{nullptr};
  void *_userdata{nullptr};
  std::string _current_working_path{"./"};
  std::vector<std::string> _search_paths;
  mutable size_t _max_asset_bytes_in_mb{1024*1024}; // default 1 TB
  uint32_t _max_file_descriptors{1024};
  bool _enable_suffix_fallback{true};
  bool _allow_parent_relative_paths{false};
  mutable std::atomic<uint32_t> _open_file_descriptors{0};

  std::map<std::string, AssetResolutionHandler> _asset_resolution_handlers;

  mutable std::map<std::string, std::string> _cached_resolved_paths;
};

// forward decl
class PrimSpec;

//
// Fileformat plugin(callback) interface.
// For fileformat which is used in `subLayers`, `reference` or `payload`.
//
// LightUSD uses C++ callback interface for security.
// (On the contrary, pxrUSD uses `plugInfo.json` + dll).
//
// Texture image/Shader file(e.g. glsl) is not handled in this API.
// (Plese refer T.B.D. for texture/shader)
//
// TODO: Move to another header file?

// Check if given data is a expectected file format
//
// @param[in] asset Asset data.
// @param[out] warn Warning message
// @param[out] err Error message(when the fuction returns false)
// @param[inout] user_data Userdata. can be nullptr.
// @return true when the given data is expected file format.
typedef bool (*FileFormatCheckFunction)(const Asset &asset, std::string *warn, std::string *err, void *user_data);


// Read content of data into PrimSpec(metadatum, properties, primChildren/variantChildren).
//
// TODO: Use `Layer` instead of `PrimSpec`?
//
// @param[in] asset Asset data
// @param[inout] ps PrimSpec which references/payload this asset.
// @param[out] warn Warning message
// @param[out] err Error message(when the fuction returns false)
// @param[inout] user_data Userdata. can be nullptr.
//
// @return true when reading data succeeds.
//
typedef bool (*FileFormatReadFunction)(const Asset &asset, PrimSpec &ps/* inout */, std::string *warn, std::string *err, void *user_data);

// Write corresponding content of PrimSpec to a binary data
//
// @param[in] ps PrimSpec which refers this asset.
// @param[out] out_asset Output asset data.
// @param[out] warn Warning message
// @param[out] err Error message(when the fuction returns false)
// @param[inout] user_data Userdata. can be nullptr.
// @return true upon data write success.
typedef bool (*FileFormatWriteFunction)(const PrimSpec &ps, Asset *out_data, std::string *warn, std::string *err, void *user_data);

struct FileFormatHandler
{
  std::string extension; // fileformat extension.
  std::string description; // Description of this fileformat. can be empty.

  FileFormatCheckFunction checker{nullptr};
  FileFormatReadFunction reader{nullptr};
  FileFormatWriteFunction writer{nullptr};
  void *userdata{nullptr};
};


}  // namespace lightusd
