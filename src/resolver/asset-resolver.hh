// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Asset Resolution
//
// Resolves asset paths for references, payloads, and sublayers

#pragma once

#include "core/host-services.hh"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace lightusd {
namespace next {

class AssetResolver;

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
  /// Final identifier after following alias registrations. Equals
  /// `resolved_path` when no alias applies. Use this for byte caches and
  /// reads; keep `resolved_path` for authored output and diagnostics.
  std::string canonical_path;
  std::string original_path;    // Original path before resolution
  bool exists = false;          // Whether the asset exists
  bool is_package = false;      // Whether this is inside a package (USDZ)
  std::string package_path;     // If in package, the package file path
  std::string asset_in_package; // If in package, path within package
  // File identity captured during resolution (by the same existence probe
  // that resolved the path); ReadAsset(ResolvedAsset) uses it to reject a
  // check/use replacement of the path.
  bool has_file_identity = false;
  uint64_t file_device = 0;
  uint64_t file_inode = 0;
};

/// Resolver context (static, API-driven; no DLL plugins).
///
/// For the built-in filesystem resolver, `context_string` is a platform-
/// separated list of search paths (`;` on Windows, `:` elsewhere), matching
/// OpenUSD's ArDefaultResolver context. Programmatic callers may fill
/// `search_paths` directly. A non-default context replaces the resolver's
/// configured search paths for the duration of the binding; anchor-relative
/// and working-directory resolution still run first.
struct ResolverContext {
  std::string context_string;
  std::vector<std::string> search_paths;
  /// Optional per-scheme context strings passed to context-aware scheme
  /// resolvers (lowercase scheme key).
  std::map<std::string, std::string> scheme_contexts;
  bool is_default = true;

  void SetSchemeContext(const std::string& scheme,
                        const std::string& context);
  std::string GetSchemeContext(const std::string& scheme) const;
  /// Borrowed pointer into scheme_contexts, or null when absent.
  const std::string* FindSchemeContext(const std::string& scheme) const;
};

/// Asset metadata.
struct AssetInfo {
  std::string resolved_path;
  std::string asset_name;
  std::string repository_path;
  bool exists = false;
};

/// Asset modification timestamp.
struct AssetTimestamp {
  bool valid = false;
  uint64_t seconds = 0;
  uint64_t nanoseconds = 0;
};

/// Scoped resolution cache handle.
class ResolverScopedCache {
 public:
  ResolverScopedCache(AssetResolver* resolver);
  ~ResolverScopedCache();
  ResolverScopedCache(const ResolverScopedCache&) = delete;
  ResolverScopedCache& operator=(const ResolverScopedCache&) = delete;

 private:
  AssetResolver* resolver_ = nullptr;
  void* data_ = nullptr;
};
struct ResolverConfig {
  std::vector<std::string> search_paths;  // Directories to search
  /// Persistent resolver context. Scoped BindContext() bindings override it on
  /// the binding thread; composition/worker threads see this value.
  ResolverContext context;
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
  // Deny all native filesystem fallback, probes and timestamps. Registered
  // memory assets and application readers remain available.
  bool allow_filesystem = true;
  // Optional working-directory lookup when working_directory is empty.
  // Without a callback, the lexical base is "."; constructors do no OS I/O.
  std::shared_ptr<const HostServices> host_services;
};

/// Custom resolver callback type
/// Returns resolved path, or empty string if not resolved
using ResolverCallback = std::function<std::string(const std::string& asset_path,
                                                    const std::string& anchor_path)>;

/// Context-aware scheme resolver. `context` is the active scheme context
/// string (empty when none is bound).
using ContextResolverCallback = std::function<std::string(
    const std::string& asset_path, const std::string& anchor_path,
    const std::string& context)>;

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

  /// Normalize a URI scheme name ("Studio:" -> "studio"). Scheme registries
  /// and context keys use the same normalized form.
  static std::string NormalizeSchemeName(const std::string& scheme);
  /// Normalize a package file extension (".PKG" -> "pkg").
  static std::string NormalizePackageExtension(const std::string& extension);
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
  // Consumers must not bypass this resolver with direct file reads.
  bool RequiresResolverReads() const {
    return !NativeFilesystemEnabled() ||
        memory_asset_count_.load(std::memory_order_relaxed) != 0 || HasUserCallbacks();
  }

  /// Register an explicit URI/IRI-style scheme handler (for example `https`,
  /// `studio`, or `usd-anon`). Scheme names are ASCII case-insensitive and are
  /// stored lowercase. The resolver callback receives the complete identifier.
  void RegisterScheme(const std::string& scheme, ResolverCallback resolver,
                      AssetReadCallback reader = {});
  void RegisterScheme(const std::string& scheme,
                      ContextResolverCallback resolver,
                      AssetReadCallback reader = {});
  bool UnregisterScheme(const std::string& scheme);
  bool HasScheme(const std::string& scheme) const;

  /// Register bytes under an identifier. An empty identifier creates a unique
  /// `usd-anon:` identifier. Registered bytes resolve and read without touching
  /// the filesystem and are safe for concurrent reads.
  bool HasMemoryAsset(const std::string& identifier) const;
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

  /// Register an alias identifier that resolves/reads as `target`.
  /// The alias keeps its own identity in `resolved_path` (authored output is
  /// preserved) while `canonical_path` points at the target, so byte caches
  /// can retain one payload for every alias. Rejects empty names, self
  /// aliases, and cycles.
  bool SetAlias(const std::string& alias, const std::string& target);
  bool UnsetAlias(const std::string& alias);
  void ClearAliases();
  /// Follow alias registrations to the final identifier. Returns the input
  /// unchanged when it is not an alias.
  std::string CanonicalPath(const std::string& identifier) const;

  /// Whether the built-in filesystem backend is permitted for this instance.
  bool NativeFilesystemEnabled() const {
#if defined(LIGHTUSD_NO_NATIVE_RESOLVER_IO) || defined(LIGHTUSD_NO_NATIVE_IO)
    return false;
#else
    return config_.allow_filesystem;
#endif
  }

  /// Get configuration
  const ResolverConfig& GetConfig() const { return config_; }
  void SetConfig(const ResolverConfig& config) {
    config_ = config;
    NormalizeConfigContext();
  }

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

  /// True when the resolved file still has the device/inode captured by
  /// Resolve(). Non-file resolutions (scheme/memory/package) are always true.
  bool FileIdentityUnchanged(const ResolvedAsset& resolved) const;

  /// Read the bytes of an already-resolved asset path: the custom reader is
  /// consulted first, then a direct filesystem read (unavailable in wasm
  /// builds with -sFILESYSTEM=0). Returns false with `err` set on failure.
  bool ReadAsset(const std::string& resolved_path, std::vector<uint8_t>* out,
                 std::string* err = nullptr) const;
  /// Read a resolved file, verifying that the opened descriptor still matches
  /// the identity captured by Resolve().
  bool ReadAsset(const ResolvedAsset& resolved_path,
                 std::vector<uint8_t>* out,
                 std::string* err = nullptr) const;

  /// Create an identifier for an asset (for caching/comparison)
  std::string CreateIdentifier(const std::string& asset_path,
                               const std::string& anchor_path = "") const;

  /// Resolve a path for creating a new asset.
  ///
  /// Unlike Resolve(), the file need not exist. Relative paths are anchored to
  /// the referring layer directory (or the working directory when there is no
  /// anchor), and suffix/search-path fallback is not applied. Package anchors
  /// are supported: a new entry resolves inside the same package.
  ResolvedAsset ResolveForNewAsset(const std::string& asset_path,
                                  const std::string& anchor_path = "") const;

  /// Context operations.
  ///
  /// Contexts are per-thread bindings. BindContext() installs an override and
  /// returns an opaque binding token through `binding_data`; UnbindContext()
  /// restores the previous binding. `binding_data` is owned by the caller and
  /// is only valid on the thread that created it. GetCurrentContext() returns
  /// the active binding, else ResolverConfig::context.
  ResolverContext CreateDefaultContext() const;
  ResolverContext CreateContextFromString(const std::string& context_str) const;
  void BindContext(const ResolverContext& context, void** binding_data = nullptr);
  /// Unbind a context. Returns false when no binding was removed (already
  /// unbound, or the binding lives on another thread).
  bool UnbindContext(const ResolverContext& context,
                     void** binding_data = nullptr);
  ResolverContext GetCurrentContext() const;
  bool IsContextDependentPath(const std::string& asset_path) const;

  /// Asset info / timestamp. Both are package-aware: package entries report
  /// the entry basename/repository and timestamp the containing package file.
  AssetInfo GetAssetInfo(const std::string& asset_path,
                         const ResolvedAsset& resolved_path) const;
  AssetTimestamp GetModificationTimestamp(const std::string& asset_path,
                                          const ResolvedAsset& resolved_path) const;

  /// Read asset bytes into a string. Equivalent to ReadAsset() but avoids the
  /// intermediate byte buffer, so a large asset is copied once.
  bool ReadAssetToString(const std::string& resolved_path, std::string* out,
                          std::string* err = nullptr) const;

  /// Open asset bytes
  std::shared_ptr<std::vector<uint8_t>> OpenAsset(
      const ResolvedAsset& resolved_path) const;

  /// Scoped resolution cache.
  ///
  /// BeginCacheScope() installs a thread-local memo for this resolver;
  /// EndCacheScope() removes it. Entries are keyed by
  /// (asset path, anchor, suffix-fallback flag, effective context), so a
  /// context change inside a scope cannot reuse a stale result. The caller
  /// opts into stable resolution for the scope's lifetime.
  void BeginCacheScope(void** cache_scope_data);
  void EndCacheScope(void** cache_scope_data);

  /// Package resolver registration.
  ///
  /// The callback is consulted for package identifiers `pkg.ext[entry]` whose
  /// package file uses `extension` (case-insensitive; a leading dot is
  /// optional). It receives the package file path (authored first, then the
  /// resolved path when it differs) and the normalized in-package entry. On
  /// true with a non-empty `resolved`, the identifier resolves to that value;
  /// on false, normal package resolution runs. Registrations are per-resolver.
  using PackageResolverCallback = std::function<bool(const std::string& package_path,
                                                     const std::string& asset_path,
                                                     std::string* resolved)>;
  void RegisterPackageResolver(const std::string& extension,
                              PackageResolverCallback callback);
  bool UnregisterPackageResolver(const std::string& extension);

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
  static uint64_t NextIdentity();
  uint64_t identity_ = NextIdentity();
  ResolverConfig config_;
  ResolverCallback custom_resolver_;
  AssetReadCallback asset_reader_;
  struct SchemeHandler {
    ResolverCallback resolver;
    ContextResolverCallback context_resolver;
    AssetReadCallback reader;
  };
  mutable std::mutex registry_mutex_;
  std::unordered_map<std::string, SchemeHandler> scheme_handlers_;
  std::unordered_map<std::string, PackageResolverCallback> package_resolvers_;
  std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
      memory_assets_;
  std::unordered_map<std::string, std::string> aliases_;
  // Mirror the registry sizes so hot resolution paths can skip the registry
  // lock entirely when no callback or memory asset is registered.
  std::atomic<size_t> memory_asset_count_{0};
  std::atomic<size_t> scheme_handler_count_{0};
  std::atomic<size_t> package_handler_count_{0};
  std::atomic<size_t> alias_count_{0};
  uint64_t next_anonymous_id_ = 1;

  void NormalizeConfigContext();
  const std::vector<std::string>& EffectiveSearchPaths() const;
  std::string CurrentSchemeContext(const std::string& scheme) const;
  PackageResolverCallback FindPackageResolver(
      const std::string& package_file) const;
  /// Give a registered package resolver a second chance with the resolved
  /// container path so virtual package formats can observe the real container.
  /// Returns true when the callback answered with an identifier.
  static bool ApplyPackageResolverToResolved(
      const PackageResolverCallback& callback,
      const std::string& resolved_package, const std::string& package_file,
      const std::string& entry, bool exists, ResolvedAsset* result);

  // Internal resolution
  ResolvedAsset ResolveInternal(const std::string& asset_path,
                                const std::string& anchor_path,
                                bool allow_suffix_fallback = true) const;
  /// Existence probe that also reports the file's device/inode identity when
  /// the platform provides it, so resolution can capture the identity without
  /// a second stat.
  bool FileExists(const std::string& path, uint64_t* device = nullptr,
                  uint64_t* inode = nullptr) const;
};

/// Global default resolver
AssetResolver& GetDefaultResolver();

/// Set the global default resolver
void SetDefaultResolver(std::unique_ptr<AssetResolver> resolver);

}  // namespace next
}  // namespace lightusd
