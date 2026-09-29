// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Asset Resolution
//
// Resolves asset paths for references, payloads, and sublayers

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace lightusd {
namespace next {

/// Thread-safe retained-payload budget shared by asset stores and consumers.
/// Reservations live as long as the last owner of the corresponding bytes.
class AssetPayloadBudget : public std::enable_shared_from_this<AssetPayloadBudget> {
 public:
  explicit AssetPayloadBudget(uint64_t limit_bytes) : limit_bytes_(limit_bytes) {}
  bool SetLimit(uint64_t limit_bytes);
  std::shared_ptr<void> Reserve(uint64_t bytes);
  uint64_t UsedBytes() const;
  uint64_t LimitBytes() const;

 private:
  void Release(uint64_t bytes);
  mutable std::mutex mutex_;
  uint64_t limit_bytes_{0};
  uint64_t used_bytes_{0};
};

/// Resolved asset information
struct ResolvedAsset {
  std::string resolved_path;    // Full resolved path
  std::string original_path;    // Original path before resolution
  bool exists = false;          // Whether the asset exists
  bool is_package = false;      // Whether this is inside a package (USDZ)
  std::string package_path;     // If in package, the package file path
  std::string asset_in_package; // If in package, path within package
};

/// Asset resolver configuration
struct ResolverConfig {
  std::vector<std::string> search_paths;  // Directories to search
  std::string working_directory;           // Base directory for relative paths
  bool allow_absolute_paths = true;        // Allow absolute path resolution
  // USD asset paths are anchored to the layer that authored them, and valid
  // layer stacks commonly use ".." to reach sibling asset directories.
  // Hardened applications can disable this when loading inside a strict
  // asset-root sandbox.
  bool allow_parent_paths = true;
  bool search_recursively = false;         // Search subdirectories
  // When the literal path does not resolve, retry progressively shorter
  // path suffixes (SuffixCandidates) against anchor/working-dir/search
  // paths — rehomes assets authored with absolute or machine-specific
  // prefixes (matches legacy AssetResolutionResolver's default-on
  // suffix fallback).
  bool enable_suffix_fallback = true;
};

/// Custom resolver callback type
/// Returns resolved path, or empty string if not resolved
using ResolverCallback = std::function<std::string(const std::string& asset_path,
                                                    const std::string& anchor_path)>;

/// Byte-read callback: fill `out` with the contents of `resolved_path` (a path
/// previously produced by resolution — possibly a non-filesystem key such as a
/// wasm asset-cache entry). Return false when the asset cannot be read;
/// `err` (may be null) receives a reason.
using AssetReadCallback = std::function<bool(const std::string& resolved_path,
                                             std::vector<uint8_t>* out,
                                             std::string* err)>;

/// AssetResolver - resolves asset paths
class AssetResolver {
public:
  AssetResolver();
  explicit AssetResolver(const ResolverConfig& config);
  AssetResolver(const AssetResolver& other);
  AssetResolver& operator=(const AssetResolver& other);
  AssetResolver(AssetResolver&& other) noexcept;
  AssetResolver& operator=(AssetResolver&& other) noexcept;
  ~AssetResolver();

  // ============================================================
  // Configuration
  // ============================================================

  /// Set working directory (base for relative paths)
  void SetWorkingDirectory(const std::string& dir);
  const std::string& GetWorkingDirectory() const { return config_.working_directory; }

  /// Add a search path
  void AddSearchPath(const std::string& path);

  /// Set all search paths
  void SetSearchPaths(const std::vector<std::string>& paths);
  const std::vector<std::string>& GetSearchPaths() const { return config_.search_paths; }
  void SetAllowParentPaths(bool allow) { config_.allow_parent_paths = allow; }
  bool GetAllowParentPaths() const { return config_.allow_parent_paths; }

  /// Clear all search paths
  void ClearSearchPaths();

  /// Set custom resolver callback (called before default resolution)
  void SetCustomResolver(ResolverCallback callback);

  /// Set byte-read callback (consulted before direct filesystem reads).
  /// Together with SetCustomResolver this makes the resolver a full
  /// filesystem abstraction: resolve to a key, then read the key's bytes.
  void SetAssetReader(AssetReadCallback reader);
  bool HasAssetReader() const { return static_cast<bool>(asset_reader_); }

  /// True when resolution or reads may enter application-provided code.
  bool HasUserCallbacks() const;

  /// Register an explicit URI/IRI-style scheme handler (for example `https`,
  /// `studio`, or `usd-anon`). Scheme names are ASCII case-insensitive and are
  /// stored lowercase. The resolver callback receives the complete identifier.
  void RegisterScheme(const std::string& scheme, ResolverCallback resolver,
                      AssetReadCallback reader = {});
  bool UnregisterScheme(const std::string& scheme);
  bool HasScheme(const std::string& scheme) const;

  /// Register bytes under an identifier. An empty identifier creates a unique
  /// `usd-anon:` identifier. Registered bytes resolve and read without touching
  /// the filesystem and are safe for concurrent reads.
  std::string RegisterMemoryAsset(const std::string& identifier,
                                  std::vector<uint8_t> bytes);
  /// Register an immutable shared payload without copying its bytes.
  std::string RegisterMemoryAssetView(
      const std::string& identifier,
      std::shared_ptr<const std::vector<uint8_t>> bytes);
  bool UnregisterMemoryAsset(const std::string& identifier);
  void ClearMemoryAssets();
  /// Return an owning read view for registered memory assets without copying.
  /// The shared view remains valid after replacement or unregister.
  std::shared_ptr<const std::vector<uint8_t>> GetMemoryAssetView(
      const std::string& identifier) const;

  /// Get configuration
  const ResolverConfig& GetConfig() const { return config_; }
  void SetConfig(const ResolverConfig& config) { config_ = config; }

  // ============================================================
  // Resolution
  // ============================================================

  /// Resolve an asset path
  /// @param asset_path The path to resolve (may be relative or absolute)
  /// @param anchor_path Optional anchor path (e.g., the referencing file)
  /// @return Resolved asset information
  ResolvedAsset Resolve(const std::string& asset_path,
                        const std::string& anchor_path = "",
                        bool allow_suffix_fallback = true) const;

  /// Quick resolution - just get the path
  std::string ResolvePath(const std::string& asset_path,
                          const std::string& anchor_path = "",
                          bool allow_suffix_fallback = true) const;

  /// Check if an asset exists
  bool Exists(const std::string& asset_path,
              const std::string& anchor_path = "") const;

  /// Read the bytes of an already-resolved asset path: the custom reader is
  /// consulted first, then a direct filesystem read (unavailable in wasm
  /// builds with -sFILESYSTEM=0). Returns false with `err` set on failure.
  bool ReadAsset(const std::string& resolved_path, std::vector<uint8_t>* out,
                 std::string* err = nullptr) const;

  /// Create an identifier for an asset (for caching/comparison)
  std::string CreateIdentifier(const std::string& asset_path,
                               const std::string& anchor_path = "") const;

  // ============================================================
  // Path utilities
  // ============================================================

  /// Check if path is absolute
  static bool IsAbsolutePath(const std::string& path);

  /// Return the normalized scheme prefix, or empty for filesystem paths.
  /// Windows drive letters are not classified as schemes.
  static std::string GetIdentifierScheme(const std::string& identifier);

  /// Check if path is a package reference (contains [])
  static bool IsPackagePath(const std::string& path);

  /// Parse package path into (package_file, asset_in_package)
  static bool ParsePackagePath(const std::string& path,
                               std::string* package_file,
                               std::string* asset_in_package);

  /// Join two paths
  static std::string JoinPath(const std::string& base, const std::string& path);

  /// Get directory part of path
  static std::string GetDirectory(const std::string& path);

  /// Get filename part of path
  static std::string GetFilename(const std::string& path);

  /// Normalize path (resolve . and .., normalize separators)
  static std::string NormalizePath(const std::string& path);

  /// Make path relative to base
  static std::string MakeRelative(const std::string& path, const std::string& base);

  /// Fallback candidates for an asset path that failed literal resolution:
  /// strip the un-anchorable prefix (drive letter, leading '/', './', '../'
  /// runs), then drop leading directory components one at a time (longest
  /// suffix first, down to the basename). The input itself is not included.
  /// Used to rebase arcs authored against another machine's layout (e.g.
  /// UnrealEngine USD exports) onto the local scene root.
  static std::vector<std::string> SuffixCandidates(const std::string& asset_path);

private:
  ResolverConfig config_;
  ResolverCallback custom_resolver_;
  AssetReadCallback asset_reader_;
  struct SchemeHandler {
    ResolverCallback resolver;
    AssetReadCallback reader;
  };
  mutable std::mutex registry_mutex_;
  std::unordered_map<std::string, SchemeHandler> scheme_handlers_;
  std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
      memory_assets_;
  uint64_t next_anonymous_id_ = 1;

  // Internal resolution
  ResolvedAsset ResolveInternal(const std::string& asset_path,
                                const std::string& anchor_path,
                                bool allow_suffix_fallback = true) const;
  bool FileExists(const std::string& path) const;
};

/// Global default resolver
AssetResolver& GetDefaultResolver();

/// Set the global default resolver
void SetDefaultResolver(std::unique_ptr<AssetResolver> resolver);

}  // namespace next
}  // namespace lightusd
