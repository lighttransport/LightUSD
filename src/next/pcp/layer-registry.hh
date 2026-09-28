// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP LayerRegistry
//
// Parse-once, resolved-path-keyed cache of layers. A referenced/sublayer file
// is parsed exactly once and shared (shared_ptr) across every prim index that
// uses it. Standalone, C++14.

#pragma once

#include "../layer/layer.hh"
#include "../parser/ascii-parser.hh"
#include "../resolver/asset-resolver.hh"
#include "../../security-policy.hh"
#include "../../safe-arithmetic.hh"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <future>
#include <mutex>
#endif

namespace lightusd {
namespace next {
namespace pcp {

struct LayerLoadOptions {
  bool strict_aousd_conformance = false;
  /// Keep uncompressed USDC arrays lazy and backed by the crate data source.
  bool usdc_lazy_arrays = true;
  /// mmap file-backed USDC layers when supported. LazyArrayRef retains shared
  /// ownership of each source through composition and Stage reconstruction.
  bool usdc_use_mmap = true;
  /// Maximum file/input bytes for each loaded external layer (0 = no limit).
  size_t max_memory = security_policy::kDefaultInputLimitBytes;
  /// Maximum elements in one USDC value array.
  size_t max_array_elements = size_t(16) * 1024 * 1024;
  /// Maximum number of entries accepted in an external USDZ package.
  size_t max_archive_entries = security_policy::kDefaultArchiveEntryCount;

  /// USDA parser options applied to each external USDA layer.
  ParseOptions usda_parse_options = {};

  /// Optional parser progress callback, also forwarded to crate loads.
  std::function<bool(const char*, size_t, size_t)> progress_callback;

  /// USDA parser worker-thread hint (0 = auto/default, 1 = serial, >1 = fixed).
  int parse_num_threads = 0;
};

class LayerRegistry {
 public:
#if defined(LIGHTUSD_ENABLE_THREAD)
  LayerRegistry() : mu_(new std::mutex()) {}
#else
  LayerRegistry() = default;
#endif

  // Move-only (owns shared layers).
  LayerRegistry(LayerRegistry &&) = default;
  LayerRegistry &operator=(LayerRegistry &&) = default;
  LayerRegistry(const LayerRegistry &) = delete;
  LayerRegistry &operator=(const LayerRegistry &) = delete;

  /// Resolve `asset_path` against `anchor` (the referencing file's directory),
  /// parse once, and return shared ownership. Returns nullptr on failure.
  std::shared_ptr<Layer> GetOrLoad(AssetResolver &resolver,
                                   const std::string &asset_path,
                                   const std::string &anchor,
                                   std::string *warn, std::string *err,
                                   const LayerLoadOptions &options = {});

  /// Pre-register an in-memory layer under a resolved identifier (so a reference
  /// resolving to that id composes it without touching disk). Does not count as
  /// a parse. Useful for embedding already-loaded layers.
  void Preload(const std::string &resolved_id, std::shared_ptr<Layer> layer) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(*mu_);
#endif
    by_resolved_[resolved_id] = std::move(layer);
  }

  /// Drop a single cached layer by resolved path. Caller must have already
  /// invalidated every PrimIndex that referenced it.
  void Drop(const std::string &resolved_path);

  void Clear() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(*mu_);
#endif
    by_resolved_.clear();
  }
  size_t size() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(*mu_);
#endif
    return by_resolved_.size();
  }
  size_t parse_count() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(*mu_);
#endif
    return parse_count_;
  }

  size_t memory_usage() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(*mu_);
#endif
    size_t bytes = 0;
    for (const auto &entry : by_resolved_) {
      bytes = safe::saturating_add(bytes, entry.first.capacity());
      if (entry.second) {
        bytes = safe::saturating_add(bytes, entry.second->memory_usage());
      }
    }
    return bytes;
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<Layer>> by_resolved_;
  size_t parse_count_ = 0;
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Heap-allocated so the registry stays movable. Guards by_resolved_/parse_count_
  // and in_flight_.
  std::unique_ptr<std::mutex> mu_;
  struct LoadOutcome {
    std::shared_ptr<Layer> layer;
    std::string warn;
    std::string err;
  };
  // Resolved path -> the in-progress parse for it. A first requester publishes a
  // future here and parses OUTSIDE the lock; concurrent requesters for the same
  // path wait on the future (parse-once) while requesters for OTHER paths parse
  // in parallel (the lock no longer spans the parse).
  std::unordered_map<std::string, std::shared_future<LoadOutcome>> in_flight_;
#endif
};

/// Load a layer from a resolved file/package path, dispatching by extension to
/// the next USDA / USDC / USDZ / MaterialX readers. Returns nullptr on failure.
/// `options.parse_num_threads` forwards to ParseOptions::num_threads for the
/// USDA large-array parallel parse; `options.max_memory` caps USDA file size
/// and USDC crate input/allocation checks.
std::shared_ptr<Layer> LoadLayerFromFile(const std::string &resolved_path,
                                         std::string *warn, std::string *err,
                                         const LayerLoadOptions &options);

/// Back-compat overload for callers that only need USDA parse parallelism.
std::shared_ptr<Layer> LoadLayerFromFile(const std::string &resolved_path,
                                         std::string *warn, std::string *err,
                                         int parse_num_threads = 0);

/// Load a layer from an in-memory buffer, dispatching by content: the
/// "PXR-USDC" magic selects the crate reader, a ZIP local-file header selects
/// the USDZ reader (a package-path `key` like "pkg.usdz[entry.usdc]" selects
/// that entry; otherwise the first .usdc/.usda entry), anything else parses as
/// USDA text. `key` is used for diagnostics and package-entry selection only.
/// Returns nullptr on failure.
/// Synthesize a skeletal /MaterialX layer from MaterialX XML bytes (see
/// layer-registry.cc for the produced prim shape). Used when a composition
/// arc references a .mtlx document.
std::shared_ptr<Layer> LoadLayerFromMtlxMemory(const std::string &key,
                                               const uint8_t *data,
                                               size_t size, std::string *warn,
                                               std::string *err);

/// Content sniff: true when the buffer looks like a MaterialX XML document.
bool LooksLikeMtlxXML(const uint8_t *data, size_t size);

std::shared_ptr<Layer> LoadLayerFromMemory(const std::string &key,
                                           const uint8_t *data, size_t size,
                                           std::string *warn, std::string *err,
                                           const LayerLoadOptions &options = {});

/// Owned-buffer variant: adopts `data` by move so USDC avoids a second copy
/// and USDA lazy arrays can retain the source text.
std::shared_ptr<Layer> LoadLayerFromMemoryOwned(
    const std::string &key, std::string &&data, std::string *warn,
    std::string *err, const LayerLoadOptions &options = {});

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
