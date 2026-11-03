// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.

#include "composition.hh"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <atomic>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include "asset-resolution.hh"
#include "common-macros.inc"
#include "core/schema-registry.hh"
#include "namespace-mapping.hh"
#include "str-util.hh"
#include "tiny-hashmap.hh"
#include "io-util.hh"
#include "pprinter.hh"
#include "prim-pprint.hh"
#include "core/security-policy.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "layer.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdObj.hh"
#include "tydra/asset-import/import.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "usdShade.hh"
#include "usda-reader.hh"

namespace lightusd {

namespace {

void AppendDiagnostic(std::string *dst, const std::string &msg) {
  if (!dst) {
    return;
  }

  (*dst) += msg;
  if (msg.empty() || msg.back() != '\n') {
    (*dst) += '\n';
  }
}

}  // namespace

#define PushError(s) AppendDiagnostic(err, s)

#define PushWarn(s) AppendDiagnostic(warn, s)

namespace {

// Recursion-depth cap for the namespace-recursive composition passes
// (variant / inherits / specializes / deferred-variant). The per-level guards
// previously used 1024*1024 — three orders of magnitude above the native
// stack, so they never fired. USD namespace depth never approaches this;
// 1024 stops a crafted deep PrimSpec tree from overflowing the stack while
// staying well above any real scene.
constexpr uint32_t kMaxCompositionDepth = 1024;

// Guards every find/insert on the shared parsed-layer cache
// (options.layer_cache): the parallel per-subtree composition workers hit it
// concurrently. std::map nodes are address-stable, so borrowed Layer pointers
// stay valid outside the lock.
std::mutex &LayerCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

bool IsVisited(const std::vector<std::set<std::string>> layer_names_stack,
               const std::string &name) {
  for (size_t i = 0; i < layer_names_stack.size(); i++) {
    if (layer_names_stack[i].count(name)) {
      return true;
    }
  }
  return false;
}

std::string GetExtension(const std::string &name) {
  return to_lower(io::GetFileExtension(name));
}

// Below `kPathDedupSetThreshold` combined elements we keep the original
// nested linear scan (cache-friendly, no allocation); above it we switch to a
// hash set so list-edit composition of relationship targets stays O(N) instead
// of O(N^2). `full_path_name()` is the canonical key for a *valid* Path (two
// valid paths are `==` iff their full names match). Invalid paths never compare
// equal under Path::operator== (not even to themselves), so they are never
// treated as duplicates here either — matching the previous behavior exactly.
constexpr size_t kPathDedupSetThreshold = 32;

// Append `extra` onto `base`, skipping entries already present (dedup is against
// the growing `base`, so duplicates within `extra` are also collapsed).
void AppendUniquePaths(std::vector<Path> &base, const std::vector<Path> &extra) {
  if (base.size() + extra.size() <= kPathDedupSetThreshold) {
    for (const auto &p : extra) {
      bool dup = false;
      for (const auto &c : base) { if (c == p) { dup = true; break; } }
      if (!dup) base.push_back(p);
    }
    return;
  }
  std::unordered_set<std::string> seen;
  seen.reserve(base.size() + extra.size());
  for (const auto &c : base) {
    if (c.is_valid()) seen.insert(c.full_path_name());
  }
  for (const auto &p : extra) {
    if (!p.is_valid()) { base.push_back(p); continue; }  // never dedup invalid
    if (seen.insert(p.full_path_name()).second) base.push_back(p);
  }
}

// Remove from `base` every entry that appears in `remove` (Path == semantics).
void RemovePaths(std::vector<Path> &base, const std::vector<Path> &remove) {
  if (base.size() + remove.size() <= kPathDedupSetThreshold) {
    std::vector<Path> filtered;
    filtered.reserve(base.size());
    for (const auto &p : base) {
      bool del = false;
      for (const auto &d : remove) { if (d == p) { del = true; break; } }
      if (!del) filtered.push_back(p);
    }
    base = std::move(filtered);
    return;
  }
  std::unordered_set<std::string> del_set;
  del_set.reserve(remove.size());
  for (const auto &d : remove) {
    if (d.is_valid()) del_set.insert(d.full_path_name());
  }
  std::vector<Path> filtered;
  filtered.reserve(base.size());
  for (const auto &p : base) {
    // Invalid `p` never compares equal, so it is never deleted.
    if (p.is_valid() && del_set.count(p.full_path_name())) continue;
    filtered.push_back(p);
  }
  base = std::move(filtered);
}

void PushChildAndVariantPrimSpecs(PrimSpec &ps, std::vector<PrimSpec *> *stack) {
  if (!stack) {
    return;
  }

  auto &children = ps.children();
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    stack->push_back(&(*it));
  }

  for (auto &variant_set_item : ps.variantSets()) {
    for (auto &variant_item : variant_set_item.second.variantSet) {
      stack->push_back(&variant_item.second);
    }
  }
}

bool IsUSDFileFormat(const std::string &name) {
  std::string ext = GetExtension(name);

  return (ext.compare("usd") == 0) || (ext.compare("usda") == 0) ||
         (ext.compare("usdc") == 0) || (ext.compare("usdz") == 0);
}

#if defined(LIGHTUSD_USE_USDOBJ)
bool IsWavefrontObjFileFormat(const std::string &name) {
  std::string ext = GetExtension(name);

  return ext.compare("obj") == 0;
}
#endif

bool IsMtlxFileFormat(const std::string &name) {
  std::string ext = GetExtension(name);

  return ext.compare("mtlx") == 0;
}

bool IsBuiltinFileFormat(const std::string &name) {
  if (IsUSDFileFormat(name)) {
    return true;
  }

  if (IsMtlxFileFormat(name)) {
    return true;
  }

#if defined(LIGHTUSD_USE_USDOBJ)
  if (IsWavefrontObjFileFormat(name)) {
    return true;
  }
#endif

  return false;
}

[[maybe_unused]] bool ReplaceRootPrimPathRec(
  const Path &srcPrefix,
  const Path &dstPrefix,
  PrimSpec &ps,
  std::string *warn,
  std::string *err) {

  (void)warn;

  DCOUT("srcPrefix: " << srcPrefix);
  DCOUT("dstPrefix: " << dstPrefix);

  constexpr size_t kMaxIter = 1024 * 1024 * 128;

  std::vector<PrimSpec *> stack;
  stack.push_back(&ps);
  size_t iter = 0;

  while (!stack.empty()) {
    if (iter++ > kMaxIter) {
      PUSH_ERROR_AND_RETURN("PrimSpec tree too deep.");
    }

    PrimSpec *current = stack.back();
    stack.pop_back();

    for (auto &prop : current->props()) {

      if (prop.second.is_relationship()) {

        Relationship &rel = prop.second.relationship();

        if (rel.is_path()) {
          if (rel.targetPath.has_prefix(srcPrefix)) {
            rel.targetPath.replace_prefix(srcPrefix, dstPrefix);
          }
        } else if (rel.is_pathvector()) {

          for (auto &path : rel.targetPathVector) {
            if (path.has_prefix(srcPrefix)) {
              path.replace_prefix(srcPrefix, dstPrefix);
            }
          }
        }

      } else if (prop.second.is_attribute_connection()) {

        Attribute &attr = prop.second.attribute();
        for (auto &connPath : attr.connections()) {
          if (connPath.has_prefix(srcPrefix)) {
            connPath.replace_prefix(srcPrefix, dstPrefix);
          }
        }
      }

    }

    PushChildAndVariantPrimSpecs(*current, &stack);
  }

  return true;
}

void ApplyLayerOffsetRec(PrimSpec &ps, const LayerOffset &offset);
void RemapPathsInPrimSpecTree(PrimSpec &ps, const NamespaceMapping &mapping);

bool IsURIAssetPath(const std::string &asset_path) {
  return asset_path.find("://") != std::string::npos;
}

// Opt-in (SetNormalizeAssetPathOnFlatten): lexically collapse `.`/`..` in asset
// paths made absolute on flatten, matching usdcat. Plain global, read during
// composition; set before flatten. The public setter/getter live in the
// lightusd namespace (see below); this global is file-local.
static bool g_normalize_asset_path_on_flatten = false;

std::string ResolveAuthoredAssetPathForFlatten(
    const std::string &asset_path,
    const std::string &current_working_path) {
  if (asset_path.empty() || current_working_path.empty() ||
      IsURIAssetPath(asset_path) || io::IsAbsPath(asset_path)) {
    return asset_path;
  }

  std::string joined = io::JoinPath(current_working_path, asset_path);
  if (g_normalize_asset_path_on_flatten) {
    joined = io::NormalizePath(joined);
  }
  return joined;
}

void ResolveAssetAttributesInPrimSpecTree(
    PrimSpec &ps,
    const std::string &fallback_current_working_path = std::string()) {
  constexpr size_t kMaxIter = 1024 * 1024 * 128;

  std::vector<PrimSpec *> stack;
  stack.push_back(&ps);
  size_t iter = 0;

  while (!stack.empty()) {
    if (iter++ > kMaxIter) {
      return;
    }

    PrimSpec *current = stack.back();
    stack.pop_back();

    std::string cwp = current->get_current_working_path();
    if ((cwp.empty() || cwp == "." || cwp == "./") &&
        !fallback_current_working_path.empty()) {
      cwp = fallback_current_working_path;
    }
    for (auto &prop : current->props()) {
      if (!prop.second.is_attribute()) {
        continue;
      }

      Attribute &attr = prop.second.attribute();
      if (!attr.has_value()) {
        continue;
      }

      nonstd::optional<value::AssetPath> ap =
          attr.get_value<value::AssetPath>();
      if (!ap) {
        continue;
      }

      std::string resolved = ResolveAuthoredAssetPathForFlatten(
          ap.value().GetAssetPath(), cwp);
      if (resolved != ap.value().GetAssetPath() ||
          ap.value().GetResolvedPath().empty()) {
        attr.set_value(value::AssetPath(resolved, resolved));
      }
    }

    PushChildAndVariantPrimSpecs(*current, &stack);
  }
}

Path GetReferencedPrimPath(const Path &authored_path,
                           const PrimSpec &src_ps) {
  if (authored_path.is_valid() && !authored_path.prim_part().empty() &&
      authored_path.prim_part() != "/") {
    return authored_path;
  }
  return Path("/" + src_ps.name(), "");
}

PrimSpec PrepareComposedArcPrimSpec(const PrimSpec &src_ps,
                                    const Path &authored_src_path,
                                    const Path &dst_prim_path,
                                    bool is_internal_arc,
                                    const LayerOffset &layer_offset,
                                    bool resolve_asset_paths) {
  PrimSpec prepared = src_ps;
  const Path src_path = GetReferencedPrimPath(authored_src_path, prepared);
  const NamespaceMapping mapping =
      MakeReferenceMapping(src_path, dst_prim_path, is_internal_arc);
  RemapPathsInPrimSpecTree(prepared, mapping);

  if (layer_offset._offset != 0.0 || layer_offset._scale != 1.0) {
    ApplyLayerOffsetRec(prepared, layer_offset);
  }

  if (resolve_asset_paths) {
    ResolveAssetAttributesInPrimSpecTree(prepared);
  }

  return prepared;
}

// After an `append`-style arc merge (OverridePrimSpec) of a referenced/payloaded
// asset `src` onto `dst`, re-anchor `dst`'s working path to the asset's directory
// IF the asset still carries unresolved reference/payload arcs of its own. Those
// arcs were authored in the asset's layer and must resolve against the asset's
// directory (core USD semantics), but OverridePrimSpec leaves `dst` carrying the
// CONSUMING prim's working path -- which, in a nested assembly chain (a
// sub-assembly definition payloads a leaf component whose subLayers reference
// `../../../fragment/geo/...`), is the sub-assembly's `base/definition` dir, one
// or more levels too high. The prepend path (InheritPrimSpecImpl) already keeps
// src's anchor when src has arcs; this is its append-path counterpart.
static void ReanchorArcsToSource(PrimSpec &dst, const PrimSpec &src) {
  if ((src.metas().references.has_value() || src.metas().payload.has_value()) &&
      !src.get_current_working_path().empty()) {
    dst.set_asset_resolution_state(src.get_current_working_path(),
                                   src.get_asset_search_paths());
  }
}

// Copy assetresolver state to all PrimSpec in the tree.
//
// `only_if_unset`: when true, a PrimSpec that ALREADY carries a working path is
// left untouched (gap-fill). Used right after subLayer composition, where each
// prim has already been anchored to the directory of the layer that authored its
// arcs (component root for main-layer opinions, a subLayer's dir for opinions a
// subLayer contributed via CombinePrimSpecRec's cross-directory anchoring).
// Blindly re-stamping the whole subtree with the reference target's ROOT dir
// would clobber those subLayer anchors, so a `../../../` reference authored in
// e.g. `<comp>/surfacing/` would resolve from `<comp>/` -- one dir too high.
// Prims that genuinely lack an anchor (custom file-format readers that build a
// PrimSpec directly) still get stamped.
bool PropagateAssetResolverState(PrimSpec &ps,
                                 const std::string &cwp,
                                 const std::vector<std::string> &search_paths,
                                 bool only_if_unset = false) {
  constexpr size_t kMaxIter = 1024 * 1024 * 512;

  DCOUT("current_working_path: " << cwp);
  DCOUT("search_paths: " << search_paths);

  std::vector<PrimSpec *> stack;
  stack.push_back(&ps);
  size_t iter = 0;

  while (!stack.empty()) {
    if (iter++ > kMaxIter) {
      return false;
    }

    PrimSpec *current = stack.back();
    stack.pop_back();

    if (!only_if_unset || current->get_current_working_path().empty()) {
      current->set_asset_resolution_state(cwp, search_paths);
    }

    PushChildAndVariantPrimSpecs(*current, &stack);
  }

  return true;
}

// TODO: support loading non-USD asset
bool LoadAsset(AssetResolutionResolver &resolver,
               const std::string &current_working_path,
               const std::vector<std::string> &search_paths,
               const lightusd::HashMap<std::string, FileFormatHandler> &fileformats,
               const value::AssetPath &assetPath, const Path &primPath,
               Layer *dst_layer, const PrimSpec **dst_primspec_root,
               const bool error_when_no_prims_found,
               const bool error_when_asset_not_found,
               const bool error_when_unsupported_fileformat,
               const bool allow_parent_relative_paths, const size_t max_asset_bytes,
               std::string *warn,
               std::string *err,
               std::map<std::string, Layer> *layer_cache = nullptr) {
  if (!dst_layer) {
    PUSH_ERROR_AND_RETURN(
        "[Internal error]. `dst_layer` output arg is nullptr.");
  }

  std::string asset_path = assetPath.GetAssetPath();
  if (!security_policy::ValidateAndNormalizeAssetPath(
          asset_path, &asset_path, allow_parent_relative_paths)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Unsafe asset path in composition: `{}`",
                    assetPath.GetAssetPath()));
  }
  std::string ext = GetExtension(asset_path);

  if (asset_path.empty()) {
    PUSH_ERROR_AND_RETURN(
        "TODO: No assetPath but Prim path(e.g. </xform>) in references.");
  }

  // TODO: Use std::stack to manage AssetResolutionResolver state?
  if (current_working_path.size()) {
    resolver.set_current_working_path(current_working_path);
  }

  if (search_paths.size()) {
    resolver.set_search_paths(search_paths);
  }

  // resolve path
  // TODO: Store resolved path to Reference?
  bool used_suffix_fallback{false};
  std::string resolved_path =
      resolver.resolve(asset_path, &used_suffix_fallback);

  DCOUT("Loading references: " << resolved_path
                               << ", asset_path: " << asset_path);

  // Dedupe per-asset-path warnings: UE-exported scenes repeat the same
  // authored path across thousands of arcs.
  static std::set<std::string> s_warned_asset_paths;
  static std::mutex s_warned_asset_paths_mutex;
  auto warn_once = [&](const std::string &key, const std::string &msg) {
    std::lock_guard<std::mutex> lock(s_warned_asset_paths_mutex);
    if (s_warned_asset_paths.insert(key).second) {
      PUSH_WARN(msg);
    }
  };

  if (used_suffix_fallback) {
    warn_once(asset_path,
              fmt::format("Asset `{}` not found at authored path; rebased to "
                          "`{}` via suffix fallback.",
                          asset_path, resolved_path));
  }

  if (resolved_path.empty()) {
    if (error_when_asset_not_found) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to resolve asset path `{}`", asset_path));
    } else {
      warn_once(asset_path,
                fmt::format("Asset not found: `{}` (current working path `{}`, "
                            "search_paths `{}`)",
                            asset_path, resolver.current_working_path(),
                            resolver.search_paths()));
      (*dst_primspec_root) = nullptr;
      return true;
    }
  }

  resolver.set_search_paths(search_paths);

  // Use resolved asset_path's basedir for current working path.
  // Add resolved asset_path's basedir to search path.
  std::string base_dir = io::GetBaseDir(resolved_path);
  if (base_dir.size()) {
    DCOUT(fmt::format("Add `{}' to asset search path.", base_dir));

    resolver.set_current_working_path(base_dir);

    resolver.add_search_path(base_dir);
  }

  // Parsed-layer cache: keyed by the resolved path plus the resolution
  // context that gets stamped into the loaded prims (the search paths; the
  // working path is derived from the resolved file's own directory and is
  // context-independent). On a hit the Layer COPY shares all heavy attribute
  // payloads via the copy-on-write value storage.
  std::string layer_cache_key;
  bool layer_from_cache = false;
  Layer cached_layer_local;
  // When the caller asks for a primspec root (reference/payload arcs), a
  // cache hit is BORROWED: src_ps points into the cache-resident layer (a
  // std::map entry — stable address for the cache's lifetime, which the
  // caller owns across the whole composition), and no per-arc Layer copy is
  // made at all. The resolution context was stamped into the cached prims
  // once at insert (it is a function of the cache key: resolved dir +
  // search paths), so a hit does not even mutate the entry.
  Layer *borrowed_cache_layer = nullptr;
  if (layer_cache) {
    layer_cache_key = resolved_path;
    for (const auto &sp : resolver.search_paths()) {
      layer_cache_key += '\n';
      layer_cache_key += sp;
    }
    // The cache map is shared across the parallel per-subtree composition
    // workers; guard find/insert (map node addresses stay stable, so the
    // borrowed pointer remains valid outside the lock).
    std::lock_guard<std::mutex> cache_lock(LayerCacheMutex());
    auto it = layer_cache->find(layer_cache_key);
    if (it != layer_cache->end()) {
      if (dst_primspec_root) {
        borrowed_cache_layer = &it->second;  // no copy
      } else {
        cached_layer_local = it->second;  // COW copy
      }
      layer_from_cache = true;
    }
  }

  Asset asset;
  if (!layer_from_cache &&
      !resolver.open_asset(resolved_path, asset_path, &asset, warn, err)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to open asset `{}`.", resolved_path));
  }

  const size_t asset_limit = max_asset_bytes
                                 ? max_asset_bytes
                                 : security_policy::kResolverMaxAssetReadBytes;
  if (asset.size() > asset_limit) {
    // An over-cap asset (e.g. Moana island's 683 MB xgGroundCover.usd vs the
    // 512 MB cap) must not abort the WHOLE composition -- a single oversized
    // sub-asset would otherwise drop the entire scene. Treat it like an
    // unresolved asset: hard-fail only when the caller wants strict resolution,
    // otherwise warn once and skip this arc so the rest of the scene composes.
    if (error_when_asset_not_found) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Resolved asset exceeds max bytes ({} > {}).",
                      asset.size(), asset_limit));
    }
    warn_once(resolved_path,
              fmt::format("Asset `{}` exceeds the max composition read size "
                          "({} > {} bytes); skipping this arc.",
                          asset_path, asset.size(), asset_limit));
    if (dst_primspec_root) {
      (*dst_primspec_root) = nullptr;
    }
    return true;
  }

  DCOUT("Opened resolved asset: " << resolved_path
                                  << ", asset_path: " << asset_path);

  if (!layer_from_cache) {
  if (IsBuiltinFileFormat(asset_path)) {
    if (IsUSDFileFormat(asset_path) || IsMtlxFileFormat(asset_path)
#if defined(LIGHTUSD_USE_USDOBJ)
        || IsWavefrontObjFileFormat(asset_path)
#endif
       ) {
      // ok
    } else {
      // TODO: obj
      if (error_when_unsupported_fileformat) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "TODO: Unknown/unsupported asset file format: {}", asset_path));
      } else {
        PUSH_WARN(fmt::format(
            "TODO: Unknown/unsupported asset file format. Skipped: {}",
            asset_path));
        return true;
      }
    }
  } else {
    if (fileformats.count(ext)) {
      DCOUT("Fileformat handler found for: " + ext);

    } else {
      DCOUT("Unknown/unsupported fileformat: " + ext);
      if (error_when_unsupported_fileformat) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Unknown/unsupported asset file format: {}", asset_path));
      } else {
        PUSH_WARN(fmt::format(
            "Unknown/unsupported asset file format. Skipped: {}", asset_path));
        return true;
      }
    }
  }

  }  // !layer_from_cache (format checks)

  Layer layer = std::move(cached_layer_local);
  std::string _warn;
  std::string _err;

  if (layer_from_cache) {
    // parsed + sublayer-composited copy from the cache; skip the parse.
  } else if (IsUSDFileFormat(asset_path)) {
    // Pass the RESOLVED (anchored) path so the loaded layer's prims are stamped
    // with the resolved directory as their current-working-path. Using the
    // authored (relative) asset_path here would lose the parent-directory
    // context across nested references (e.g. `a/b.usd` referencing `./c.usd`
    // must anchor c.usd to `a/`, not to the process working directory).
    if (!LoadLayerFromMemory(asset.data(), asset.size(), resolved_path, &layer,
                             &_warn, &_err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to open `{}` as Layer: {}", asset_path, _err));
    }
#if defined(LIGHTUSD_USE_USDOBJ)
  } else if (IsWavefrontObjFileFormat(asset_path)) {
    tydra::import_asset::Options obj_options;
    obj_options.max_memory = asset_limit;
    obj_options.max_input = asset_limit;
    obj_options.read = [&resolver, asset_limit](const std::string& name,
        const std::string& anchor, std::vector<uint8_t>* bytes,
        std::string* id, std::string* error) {
      // Resolver state is per composition worker; restore its anchor after
      // each dependent read, including failed resolutions.
      const auto previous = resolver.current_working_path();
      resolver.set_current_working_path(io::GetBaseDir(anchor));
      *id = resolver.resolve(name);
      Asset dependency;
      std::string warning;
      const bool ok = !id->empty() && resolver.open_asset(*id, name, &dependency,
                                                         &warning, error, asset_limit);
      resolver.set_current_working_path(previous);
      if (ok) bytes->assign(dependency.data(), dependency.data()+dependency.size());
      return ok;
    };
    if (!usdObj::ReadObjLayerFromMemory(
            std::string(reinterpret_cast<const char*>(asset.data()), asset.size()),
            resolved_path, &layer, obj_options, &_warn, &_err)) {
      PUSH_ERROR_AND_RETURN("Failed to import OBJ layer: " + _err);
    }
#endif
  } else if (IsMtlxFileFormat(asset_path)) {
    // A MaterialX reference targets the converted `/MaterialX` prim or one of
    // its descendants — e.g. `@mat.mtlx@</MaterialX/Materials>` (common, as in
    // usd-wg OpenChessSet looks). Accept /MaterialX or any prim under it; the
    // sub-prim is extracted by find_primspec_at() below.
    const std::string &mtlx_prim = primPath.prim_part();
    if (mtlx_prim != "/MaterialX" && mtlx_prim.rfind("/MaterialX/", 0) != 0) {
      PUSH_ERROR_AND_RETURN(
          "Prim path for a MaterialX reference must be </MaterialX> or a "
          "descendant, but got: " + mtlx_prim);
    }

    PrimSpec ps;
    if (!LoadMaterialXFromAsset(asset, asset_path, ps, &_warn, &_err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to open mtlx asset `{}`: {}", asset_path, _err));
    }

    ps.name() = "MaterialX";
    layer.primspecs()["MaterialX"] = ps;

  } else {
    if (fileformats.count(ext)) {
      PrimSpec ps;
      const FileFormatHandler &handler = fileformats.at(ext);

      if (!handler.reader(asset, ps, &_warn, &_err, handler.userdata)) {
        PUSH_ERROR_AND_RETURN(fmt::format("Failed to read asset `{}` error: {}",
                                          asset_path, _err));
      }

      if (ps.name().empty()) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "PrimSpec element_name is empty. asset `{}`", asset_path));
      }

      layer.primspecs()[ps.name()] = ps;
      DCOUT("Read asset from custom fileformat handler: " << ext);
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "FileFormat handler not found for asset `{}`", asset_path));
    }
  }

  DCOUT("layer = " << print_layer(layer, 0));

  // If the loaded asset aggregates its prims through its OWN subLayers (e.g. a
  // payload/reference target that is just a `subLayers` list — as in usd-wg
  // OpenChessSet's *_payload.usd / *.usd), compose those subLayers here so the
  // target's prims are present before the "no prims" check below (matching
  // OpenUSD). Sublayers are resolved relative to this asset's directory.
  if (!layer_from_cache && IsUSDFileFormat(asset_path) &&
      !layer.metas().subLayers.empty()) {
    layer.set_asset_resolution_state(
        base_dir.empty() ? resolver.current_working_path() : base_dir,
        resolver.search_paths());

    Layer sublayer_composited;
    SublayersCompositionOptions subopts;
    subopts.allow_parent_relative_paths = allow_parent_relative_paths;
    subopts.error_when_asset_not_found = error_when_asset_not_found;
    subopts.error_when_unsupported_fileformat = error_when_unsupported_fileformat;
    subopts.fileformats = fileformats;
    if (!CompositeSublayers(resolver, layer, &sublayer_composited, warn, err,
                            subopts)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to composite subLayers of `{}`", asset_path));
    }
    layer = std::move(sublayer_composited);
  }

  if (layer_cache && !layer_from_cache) {
    std::lock_guard<std::mutex> cache_lock(LayerCacheMutex());
    if (dst_primspec_root) {
      // Cache the parsed (and sublayer-composited) layer by MOVE and stamp
      // the resolution context into every prim once here — later arcs to
      // the same file borrow the entry directly (see above).
      //
      // Lost-race re-check (parallel subtree workers): if another worker
      // inserted this key while we were parsing, BORROW its entry and drop
      // our parse — overwriting the slot would invalidate PrimSpec pointers
      // the other worker already borrowed into its composition.
      auto race_it = layer_cache->find(layer_cache_key);
      if (race_it != layer_cache->end()) {
        borrowed_cache_layer = &race_it->second;
      } else {
        Layer &slot = (*layer_cache)[layer_cache_key];
        slot = std::move(layer);
        for (auto &ps_item : slot.primspecs()) {
          // only_if_unset: preserve the per-prim cwp that variant-nested
          // prims were stamped with at load (Bug 2 sub-case) — the cache
          // entry must not flatten those to the layer-level context.
          if (!PropagateAssetResolverState(ps_item.second,
                                           resolver.current_working_path(),
                                           resolver.search_paths(),
                                           /* only_if_unset */ true)) {
            PUSH_ERROR_AND_RETURN(
                "Store AssetResolver state to each PrimSpec failed.\n");
          }
        }
        slot.set_asset_resolution_state(resolver.current_working_path(),
                                        resolver.search_paths(),
                                        resolver.get_userdata());
        borrowed_cache_layer = &slot;
      }
    } else {
      // Cache the parsed (and sublayer-composited) layer; later arcs to the
      // same file take a COW copy instead of re-parsing. Keep the first
      // insertion on a lost race (same content either way).
      if (!layer_cache->count(layer_cache_key)) {
        (*layer_cache)[layer_cache_key] = layer;
      }
    }
  }

  // The layer all reads below operate on: the cache-resident one when
  // borrowing, the local otherwise.
  Layer &live_layer = borrowed_cache_layer ? *borrowed_cache_layer : layer;

  if (_warn.size()) {
    if (warn) {
      (*warn) += _warn;
    }
  }

  if (live_layer.primspecs().empty()) {
    if (error_when_no_prims_found) {
      PUSH_ERROR_AND_RETURN(fmt::format("No prims in layer `{}`", asset_path));
    }

    if (dst_primspec_root) {
      (*dst_primspec_root) = nullptr;
    }

    (*dst_layer) = std::move(layer);

    return true;
  }

  const PrimSpec *src_ps{nullptr};

  if (dst_primspec_root) {
    std::string default_prim;
    if (primPath.is_valid()) {
      default_prim = primPath.prim_part();
      DCOUT("primPath = " << default_prim);
    } else {
      // Use `defaultPrim` metadatum
      if (live_layer.metas().defaultPrim.valid()) {
        default_prim = "/" + live_layer.metas().defaultPrim.str();
        DCOUT("layer.meta.defaultPrim = " << default_prim);
      } else {
        // Use the first Prim in the layer.
        default_prim = "/" + live_layer.primspecs().begin()->first;
        DCOUT("layer.primspecs[0].name = " << default_prim);
      }
    }

    std::string find_err;
    if (!live_layer.find_primspec_at(Path(default_prim, ""), &src_ps,
                                     &find_err) ||
        !src_ps) {
      if (primPath.is_valid()) {
        // A reference/payload that targets a SPECIFIC prim which does not exist
        // in the referenced layer contributes no opinions — valid in USD
        // (OpenUSD does not fail). Return a null root so the caller skips this
        // arc instead of aborting the whole composition.
        if (warn) {
          (*warn) += "Referenced prim <" + default_prim + "> not found in `" +
                     asset_path + "`; no opinions (skipped).\n";
        }
        (*dst_primspec_root) = nullptr;
        (*dst_layer) = std::move(layer);
        return true;
      }
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to find PrimSpec `{}` in layer `{}`(resolved path: `{}`)",
          default_prim, asset_path, resolved_path));
    }

    if (!borrowed_cache_layer) {
      // Non-cached load: stamp the resolution context into the returned
      // subtree (cached layers were stamped whole at insert).
      // only_if_unset: variant-nested prims were already stamped with their
      // own cwp at load (Bug 2 sub-case) — never overwrite those.
      if (!PropagateAssetResolverState(*const_cast<PrimSpec *>(src_ps),
                                       resolver.current_working_path(),
                                       resolver.search_paths(),
                                       /* only_if_unset */ true)) {
        PUSH_ERROR_AND_RETURN(
            "Store AssetResolver state to each PrimSpec failed.\n");
      }
    }

    (*dst_primspec_root) = src_ps;
  }

  if (!borrowed_cache_layer) {
    // FIXME: This may be redundant, since asset resolution state is stored in
    // each PrimSpec.
    // TODO: Remove layer-level asset resolution state store?
    //
    // save assetresolution state for nested composition.
    layer.set_asset_resolution_state(resolver.current_working_path(),
                                     resolver.search_paths(),
                                     resolver.get_userdata());
  }

  // When borrowing (src_ps points into the cache), the local `layer` is empty
  // and dst_layer receives it as-is — arc callers that pass a layer_cache use
  // only dst_primspec_root.
  (*dst_layer) = std::move(layer);

  return true;
}

namespace {

std::string ResolveWeakerPathExpressionText(const std::string &stronger,
                                            const std::string &weaker) {
  if (stronger.find("%_") == std::string::npos) return stronger;
  const std::string replacement = weaker.empty()
      ? "(/ & ~/)" : ("(" + weaker + ")");
  std::string out = stronger;
  size_t pos = 0;
  while ((pos = out.find("%_", pos)) != std::string::npos) {
    out.replace(pos, 2, replacement);
    pos += replacement.size();
  }
  return out;
}

void ComposeWeakerPathExpression(Attribute *stronger,
                                 const Attribute &weaker) {
  if (!stronger || stronger->is_blocked() || weaker.is_blocked()) return;
  const auto *strong_expr =
      stronger->get_var().as<value::PathExpression>();
  const auto *weak_expr = weaker.get_var().as<value::PathExpression>();
  if (!strong_expr || !weak_expr) return;
  const std::string resolved = ResolveWeakerPathExpressionText(
      strong_expr->GetText(), weak_expr->GetText());
  if (resolved != strong_expr->GetText()) {
    stronger->get_var().set_value(value::PathExpression(resolved));
  }
}

}  // namespace

bool CombinePrimSpecRec(uint32_t depth, PrimSpec &dst, const PrimSpec &src, std::string *warn,
                      std::string *err) {
  (void)warn;

  if (depth > 4096) {
    PUSH_ERROR_AND_RETURN("PrimSpec tree too deep (max 4096).");
  }

  // Whether dst authored its own reference/payload arcs BEFORE merging the
  // weaker opinion (used for list-op accumulation + cross-directory anchoring).
  const bool dst_had_refs = dst.metas().references.has_value();
  const bool dst_had_payload = dst.metas().payload.has_value();
  const bool dst_had_variant_sets = dst.metas().variantSets.has_value();
  const bool dst_had_arcs = dst_had_refs || dst_had_payload;

  // Combine metadataum (weaker fills in where stronger is not authored)
  dst.metas().update_from(src.metas(), /* override_authored */ false);

  // `references` and `payload` are LIST-OPS: across a sublayer stack each layer
  // may `prepend`/`append` to the same prim and the results ACCUMULATE (USD
  // semantics) -- e.g. a shot whose department layers each prepend a reference
  // to the same root prim. update_from() above only gap-fills the whole value
  // (keeping the stronger opinion when present), so when BOTH layers author the
  // arc we must concatenate the weaker layer's list-op entries after the
  // stronger's. (When dst had none, update_from already copied src's.)
  if (dst_had_refs && src.metas().references.has_value()) {
    auto &dref = dst.metas().references.value();
    for (const auto &e : src.metas().references.value()) dref.push_back(e);
  }
  if (dst_had_payload && src.metas().payload.has_value()) {
    auto &dpl = dst.metas().payload.value();
    for (const auto &e : src.metas().payload.value()) dpl.push_back(e);
  }
  if (dst_had_variant_sets && src.metas().variantSets.has_value()) {
    auto &dvsets = dst.metas().variantSets.value();
    for (const auto &e : src.metas().variantSets.value()) {
      dvsets.push_back(e);
    }
  }

  // Cross-directory anchoring for the reference/payload arcs: if the arcs were
  // introduced by this weaker layer (dst authored none of its own), they are
  // anchored to the weaker layer's directory -- which may sit at a different
  // depth than the stronger accumulator (e.g. a parent layer at .../shot/ vs.
  // its `assembly/` subdirectory sublayer, where a `../../../` relative ref
  // resolves to different places). Adopt the authoring layer's anchor so the
  // arcs resolve correctly after sublayer flattening.
  if (!dst_had_arcs &&
      (src.metas().references.has_value() || src.metas().payload.has_value()) &&
      !src.get_current_working_path().empty()) {
    dst.set_asset_resolution_state(src.get_current_working_path(),
                                   src.get_asset_search_paths());
  }

  // AOUSD Core Spec 12.2.1 (specifier): Composed specifier resolution.
  // If dst (stronger) is `over` but src (weaker) is defining (def/class),
  // the composed specifier becomes `def` -- the prim IS defined because
  // at least one opinion provides a definition.
  if (dst.specifier() == Specifier::Over &&
      (src.specifier() == Specifier::Def ||
       src.specifier() == Specifier::Class)) {
    dst.specifier() = src.specifier();

    // The prim's definition -- and therefore the anchor for its relative
    // reference/payload asset paths -- comes from this weaker DEFINING layer,
    // not from the stronger `over`. A pure `over` (e.g. a root-layer variant
    // selection) carries the root layer's working-path; keeping it would
    // mis-anchor the arcs authored in the defining sublayer. Adopt the defining
    // layer's asset-resolution anchor so those arcs resolve against the correct
    // directory after sublayer flattening (cross-directory anchoring).
    if (!src.get_current_working_path().empty()) {
      dst.set_asset_resolution_state(src.get_current_working_path(),
                                     src.get_asset_search_paths());
    }
  }

  // AOUSD Core Spec 12.2.2 (typeName): Use typeName from defining spec.
  // If dst has no typeName and src is defining (def/class), take src's.
  if (dst.typeName().empty() && !src.typeName().empty()) {
    if (src.specifier() == Specifier::Def ||
        src.specifier() == Specifier::Class) {
      dst.typeName() = src.typeName();
    }
  }

  // Combine properties
  for (const auto &prop : src.props()) {
    // Single lookup; the else-branch only mutates the found entry in place (no
    // insert/erase on dst.props()), so the reference stays valid throughout.
    auto dst_it = dst.props().find(prop.first);
    if (dst_it == dst.props().end()) {
      // add if not existent
      dst.props()[prop.first] = prop.second;
    } else {
      Property &dst_prop = dst_it->second;
      // AOUSD Core Spec 12.2.4 (custom): true if ANY opinion says true
      if (prop.second.has_custom() && !dst_prop.has_custom()) {
        dst_prop.set_custom(true);
      }

      // AOUSD Core Spec 12.4 (relationships): Compose relationship targets
      // using list-op semantics across opinions.
      if (dst_prop.is_relationship() &&
          prop.second.is_relationship()) {
        Relationship &dst_rel = dst_prop.relationship();
        const Relationship &src_rel = prop.second.get_relationship();

        // If weaker has targets and stronger doesn't block them,
        // merge using list-edit semantics
        if (!dst_rel.is_blocked() && src_rel.is_pathvector() &&
            dst_rel.is_pathvector()) {
          ListEditQual src_qual = src_rel.get_listedit_qual();

          if (src_qual == ListEditQual::Prepend) {
            // Prepend weaker targets before stronger
            std::vector<Path> combined = src_rel.targetPathVector;
            AppendUniquePaths(combined, dst_rel.targetPathVector);
            dst_rel.targetPathVector = std::move(combined);
          } else if (src_qual == ListEditQual::Append) {
            // Append weaker targets after stronger
            AppendUniquePaths(dst_rel.targetPathVector, src_rel.targetPathVector);
          } else if (src_qual == ListEditQual::Delete) {
            // Delete weaker targets from stronger
            RemovePaths(dst_rel.targetPathVector, src_rel.targetPathVector);
          }
          // ResetToExplicit: stronger already wins (default behavior)
        }
      }

      // AOUSD Core Spec 6.5 (type agreement): Warn if composed property types
      // disagree. Role types (color3f) agree with their underlying type (float3)
      // but are not equivalent; other mismatches are errors.
      if (dst_prop.is_attribute() &&
          prop.second.is_attribute()) {
        const std::string &dst_type = dst_prop.get_attribute().type_name();
        const std::string &src_type = prop.second.get_attribute().type_name();
        if (!dst_type.empty() && !src_type.empty() && dst_type != src_type) {
          // Check if types agree via role-type relationship
          // (e.g., color3f agrees with float3, point3f with float3, etc.)
          uint32_t dst_underlying = value::GetUnderlyingTypeId(dst_type);
          uint32_t src_underlying = value::GetUnderlyingTypeId(src_type);
          if (dst_underlying != src_underlying) {
            PUSH_WARN(fmt::format(
                "Type mismatch for property `{}`: stronger has `{}`, "
                "weaker has `{}`. Composed value may be incorrect.",
                prop.first, dst_type, src_type));
          }
        }
      }

      // AOUSD Core Spec 12.2.3 (variability): If the stronger opinion did not
      // explicitly author variability, use the weaker opinion's variability.
      // Also consult the schema registry as the weakest fallback.
      if (dst_prop.is_attribute() &&
          prop.second.is_attribute()) {
        Attribute &dst_attr = dst_prop.attribute();
        const Attribute &src_attr = prop.second.get_attribute();

        // If dst (stronger) has default variability (Varying) and src (weaker)
        // has explicit uniform, check schema to determine correct variability.
        if (dst_attr.variability() == Variability::Varying &&
            src_attr.variability() == Variability::Uniform) {
          // Weaker opinion has uniform -- use it (spec says weaker fills)
          dst_attr.variability() = Variability::Uniform;
        } else if (dst_attr.variability() == Variability::Varying &&
                   src_attr.variability() == Variability::Varying) {
          // Neither explicitly set uniform; consult schema registry
          const auto *schema_info = SchemaRegistry::instance().find(
              dst.typeName(), prop.first);
          if (schema_info &&
              schema_info->variability == Variability::Uniform) {
            dst_attr.variability() = Variability::Uniform;
          }
        }
        // SdfPathExpression's `%_` operand means the weaker authored value.
        // Resolve it while both layer opinions are still available; a
        // flattened Stage intentionally has no property stack to consult.
        ComposeWeakerPathExpression(&dst_attr, src_attr);
      }
    }
  }

  // Combine child primspecs.
  //
  // Build a name->index map of dst children once so the per-child lookup is O(1)
  // instead of an O(n*m) std::find_if scan. Indices (not iterators/pointers) are
  // stored because push_back below may reallocate dst.children(); the map is
  // kept in sync as new children are appended. emplace() keeps the first
  // occurrence, matching find_if's first-match semantics for duplicate names.
  std::unordered_map<std::string, size_t> dst_child_index;
  dst_child_index.reserve(dst.children().size());
  for (size_t i = 0; i < dst.children().size(); i++) {
    dst_child_index.emplace(dst.children()[i].name(), i);
  }

  for (auto &child : src.children()) {
    auto it = dst_child_index.find(child.name());

    // if exists, combine properties and children
    if (it != dst_child_index.end()) {
      if (!CombinePrimSpecRec(depth + 1, dst.children()[it->second], child, warn, err)) {
        return false;
      }
    }
    // otherwise add it
    else {
      dst_child_index.emplace(child.name(), dst.children().size());
      dst.children().push_back(child);
    }
  }

  // Combine variantSet CONTENT (the `variantSet "x" = { ... }` blocks).
  // update_from() above merged the `variantSets`/`variants` METADATA (the
  // listop + the selection), but the actual variant content lives in
  // variantSets() and would otherwise be dropped. Without this, a prim that
  // receives its variant selection from one layer and its variant content from
  // another (common: a shot/override selects a variant whose set is defined in
  // a weaker sublayer) loses the content and the variant cannot be resolved.
  for (const auto &src_vs : src.variantSets()) {
    auto dit = dst.variantSets().find(src_vs.first);
    if (dit == dst.variantSets().end()) {
      dst.variantSets()[src_vs.first] = src_vs.second;  // add whole variantSet
    } else {
      // Same variantSet authored in both opinions: merge per-variant content
      // (dst is stronger; weaker fills gaps / recurses on conflicts).
      VariantSetSpec &dst_vs = dit->second;
      for (const auto &v : src_vs.second.variantSet) {
        auto vit = dst_vs.variantSet.find(v.first);
        if (vit == dst_vs.variantSet.end()) {
          dst_vs.variantSet[v.first] = v.second;
        } else if (!CombinePrimSpecRec(depth + 1, vit->second, v.second, warn,
                                       err)) {
          return false;
        }
      }
    }
  }

  return true;
}


// AOUSD Core Spec 10.3.1 / 10.3.2.2: Apply LayerOffset to all timeSamples
// in a PrimSpec tree. The formula per spec is: t_stage = t_layer * scale + offset.
// So when importing layer times, we transform: t_new = t_old * scale + offset.
void ApplyLayerOffsetRec(PrimSpec &ps, const LayerOffset &offset) {
  if (offset._offset == 0.0 && offset._scale == 1.0) {
    return;  // Identity — nothing to do
  }

  for (auto &prop_item : ps.props()) {
    if (prop_item.second.is_attribute()) {
      Attribute &attr = prop_item.second.attribute();
      if (attr.has_timesamples()) {
        // NOTE: Use apply_time_transform() instead of mutating samples()
        // directly: TimeSamples may hold samples in unified binary
        // storage(_times), which samples() does not write back to.
        attr.get_var().ts_raw().apply_time_transform(offset._scale,
                                                     offset._offset);
      }
    }
  }

  for (auto &child : ps.children()) {
    ApplyLayerOffsetRec(child, offset);
  }

  for (auto &variant_set_item : ps.variantSets()) {
    for (auto &variant_item : variant_set_item.second.variantSet) {
      ApplyLayerOffsetRec(variant_item.second, offset);
    }
  }
}

// AOUSD Core Spec 10.3.2.3: Tag PrimSpecs with source layer ID for implied arc tracking.
void TagPrimSpecArcOriginRec(PrimSpec &ps, const std::string &layer_id) {
  ArcOrigin origin;
  origin.source_layer_id = layer_id;
  origin.source_prim_path = Path("/" + ps.name(), "");
  ps.metas().arc_origins.push_back(origin);

  for (auto &child : ps.children()) {
    TagPrimSpecArcOriginRec(child, layer_id);
  }
}

// `accum_offset` is the accumulated LayerOffset from the root layer down to
// `in_layer`(identity for the root layer). Nested sublayer offsets compose:
// offset_total = accum ∘ child (i.e. t_root = child(t) * accum_scale + accum_offset).
bool CompositeSublayersRec(AssetResolutionResolver &resolver,
                           const Layer &in_layer,
                           std::vector<std::set<std::string>> layer_names_stack,
                           Layer *composited_layer, std::string *warn,
                           std::string *err,
                           const SublayersCompositionOptions &options,
                           const LayerOffset &accum_offset = LayerOffset()) {
  if (layer_names_stack.size() > options.max_depth) {
    if (err) {
      (*err) += "subLayer is nested too deeply.";
    }
    return false;
  }

  layer_names_stack.emplace_back(std::set<std::string>());
  std::set<std::string> &curr_layer_names = layer_names_stack.back();

  for (auto const &prim : in_layer.primspecs()) {
    if (composited_layer->has_primspec(prim.first))
    {
      if (!CombinePrimSpecRec(0, composited_layer->primspecs().at(prim.first), prim.second, warn, err)) {
        return false;
      }
    }
    else {
      composited_layer->add_primspec(prim.first, prim.second);
    }
  }

  // AOUSD Core Spec 9.5: Build expression variables map for asset path substitution
  lightusd::HashMap<std::string, std::string> expr_vars;
  if (in_layer.metas().expressionVariables) {
    for (const auto &var : in_layer.metas().expressionVariables.value()) {
      if (auto sv = var.second.get_value<std::string>()) {
        expr_vars[var.first] = sv.value();
      }
    }
  }

  for (const auto &layer : in_layer.metas().subLayers) {
    std::string sublayer_asset_path = layer.assetPath.GetAssetPath();

    // AOUSD Core Spec 9.5: Substitute expression variables in asset paths
    if (!expr_vars.empty()) {
      sublayer_asset_path = SubstituteExpressionVariables(sublayer_asset_path, expr_vars);
    }

    // Do cyclic referencing check.
    // TODO: Use resolved name?
    if (IsVisited(layer_names_stack, sublayer_asset_path)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Circular referenceing detected for subLayer: {} in {}",
                      sublayer_asset_path, in_layer.name()));
    }

    // Each sublayer is resolved relative to THIS layer's directory. A previous
    // iteration's LoadAsset() leaves the resolver's working path pointing at the
    // last-loaded sublayer's dir, so reset it here before the existence check —
    // otherwise the 2nd+ sublayer of a multi-sublayer list resolves against the
    // wrong base (e.g. `../../../_common/axis.usda` fails after a sibling loads).
    resolver.set_current_working_path(in_layer.get_current_working_path());
    resolver.set_search_paths(in_layer.get_asset_search_paths());

    std::string layer_filepath = resolver.resolve(sublayer_asset_path);
    if (layer_filepath.empty()) {
      PUSH_ERROR_AND_RETURN(fmt::format("{} not found in path: {}",
                                        sublayer_asset_path,
                                        resolver.search_paths_str()));
    }

    lightusd::Layer sublayer;
    if (!LoadAsset(resolver, in_layer.get_current_working_path(),
                   in_layer.get_asset_search_paths(), options.fileformats,
                   layer.assetPath, /* not_used */ Path::make_root_path(),
                   &sublayer, /* primspec_root */ nullptr,
                   options.error_when_no_prims_in_sublayer,
                   options.error_when_asset_not_found,
                   options.error_when_unsupported_fileformat,
                   options.allow_parent_relative_paths,
                   options.max_asset_bytes, warn, err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Load asset in subLayer failed: `{}`", layer.assetPath));
    }

    // AOUSD Core Spec 10.3.1: Apply sublayer offset/scale to timeSamples.
    // t_stage = t_layer * scale + offset
    // Nested sublayer offsets accumulate(compose like pcp LayerOffset):
    // total = accum ∘ child => scale = s_accum * s_child,
    //                          offset = o_accum + s_accum * o_child
    LayerOffset total_offset;
    total_offset._scale = accum_offset._scale * layer.layerOffset._scale;
    total_offset._offset =
        accum_offset._offset + accum_offset._scale * layer.layerOffset._offset;

    if (total_offset._offset != 0.0 || total_offset._scale != 1.0) {
      for (auto &ps_item : sublayer.primspecs()) {
        ApplyLayerOffsetRec(ps_item.second, total_offset);
      }
    }

    // AOUSD Core Spec 10.3.2.3: Tag sublayer PrimSpecs with source layer ID
    // for implied inherit/specialize arc propagation.
    for (auto &ps_item : sublayer.primspecs()) {
      TagPrimSpecArcOriginRec(ps_item.second, sublayer_asset_path);
    }

    curr_layer_names.insert(sublayer_asset_path);

    // Recursively load subLayer. Pass down the accumulated offset so that
    // grandchild sublayer offsets compose with this one.
    if (!CompositeSublayersRec(resolver, sublayer, layer_names_stack,
                               composited_layer, warn, err, options,
                               total_offset)) {
      return false;
    }
  }

  layer_names_stack.pop_back();

  return true;
}

}  // namespace

std::vector<std::string> ExtractSublayerAssetPaths(const Layer &layer) {

  std::vector<std::string> paths;

  for (const auto &sublayer : layer.metas().subLayers) {
    std::string sublayer_asset_path = sublayer.assetPath.GetAssetPath();

    paths.push_back(sublayer_asset_path);
  }

  return paths;

}



bool CompositeSublayers(AssetResolutionResolver &resolver,
                        const Layer &in_layer, Layer *composited_layer,
                        std::string *warn, std::string *err,
                        SublayersCompositionOptions options) {
  if (!composited_layer) {
    return false;
  }

  std::vector<std::set<std::string>> layer_names_stack;

  // keep metas from the root layer
  composited_layer->metas() = in_layer.metas();

  DCOUT("Resolve subLayers..");
  if (!CompositeSublayersRec(resolver, in_layer, layer_names_stack,
                             composited_layer, warn, err, options)) {
    PUSH_ERROR_AND_RETURN("Composite subLayers failed.");
  }

  composited_layer->metas().subLayers.clear();

  DCOUT("Composite subLayers ok.");
  return true;
}

namespace {


// Visited set for cycle detection in references/payloads.
// Tracks (asset_path, prim_path) pairs. Grows only with arc nesting depth (typically < 10).
using ArcVisitedSet = std::set<std::pair<std::string, std::string>>;

// AOUSD Core Spec 10.3.2.3 / 10.3.2.4: Propagate implied inherit/specialize
// paths from a referenced/payload PrimSpec to the referencing prim.
// When prim P references prim Q, and Q has `inherits = [/C]`, then P should
// also implicitly inherit from /C if it exists in P's layer stack.
static void PropagateImpliedArcPaths(const PrimSpec &src_ps,
                                     PrimSpec &dst_ps) {
  // Propagate inherits paths
  if (src_ps.metas().inherits) {
    for (const auto &op : src_ps.metas().inherits.value()) {
      if (op.second.empty()) continue;
      if (!dst_ps.metas().inheritPaths) {
        dst_ps.metas().inheritPaths.emplace();
      }
      dst_ps.metas().inheritPaths->push_back(op);
    }
  }

  // Propagate specializes paths
  if (src_ps.metas().specializes) {
    for (const auto &op : src_ps.metas().specializes.value()) {
      if (op.second.empty()) continue;
      if (!dst_ps.metas().specializePaths) {
        dst_ps.metas().specializePaths.emplace();
      }
      dst_ps.metas().specializePaths->push_back(op);
    }
  }
}

// Process ONLY `primspec`'s own reference arcs (no child traversal). Split
// out of CompositeReferencesRec so the parallel per-subtree composition can
// run the shell ancestors' own arcs serially after their descendants'
// subtrees completed in parallel (preserving the post-order semantics).
// The resolution context (cwp/search paths) comes from the primspec itself.
bool CompositeReferencesLocal(AssetResolutionResolver &resolver,
                              const Path &dst_prim_path,
                              const Layer &in_layer,
                              PrimSpec &primspec /* [inout] */,
                              std::string *warn, std::string *err,
                              const ReferencesCompositionOptions &options,
                              ArcVisitedSet &visited) {
  // Use PrimSpec's AssetResolution state.
  std::string cwp = primspec.get_current_working_path();
  std::vector<std::string> search_paths = primspec.get_asset_search_paths();

  if (primspec.metas().references) {
    // IMPORTANT: copy the reference listops before iterating. The loop body
    // merges referenced content INTO `primspec`, which can append to
    // `primspec.metas().references` (implied/nested arcs) and REALLOCATE the
    // underlying vector — iterating it live then reads freed memory (observed as
    // a garbage asset/prim path on the 2nd+ element of a `references = [a, b]`
    // list). Iterate a stable copy instead (mirrors CompositeInheritsRec).
    const auto references_ops = primspec.metas().references.value();

    // Consume these reference arcs up front: clear them now (we iterate the
    // `references_ops` copy below, not the live metadata). This lets any NESTED
    // references the merge carries in from a referenced layer — a
    // reference-of-a-reference, e.g. `top`->`mid`->`leaf` — accumulate on
    // `primspec` and survive for the outer fixed-point loop to resolve. A blanket
    // reset() at the end used to wipe those nested arcs too, so only the FIRST
    // level of references ever resolved (this is what collapsed deeply-nested
    // assemblies like Animal Logic ALab to near-empty).
    primspec.metas().references.reset();

    // Process all listops in order (supports multiple listops per arc)
    // Pre-pass: collect deleted reference targets so we can skip them.
    std::set<std::pair<std::string, std::string>> ref_deleted;
    for (const auto &ref_op : references_ops) {
      if (ref_op.first == ListEditQual::Delete) {
        for (const auto &r : ref_op.second) {
          ref_deleted.insert({r.asset_path.GetAssetPath(), r.prim_path.prim_part()});
        }
      }
    }

    for (const auto &ref_op : references_ops) {
      const ListEditQual &qual = ref_op.first;
      const auto &refecences = ref_op.second;

      if ((qual == ListEditQual::ResetToExplicit) ||
          (qual == ListEditQual::Prepend)) {
        for (const auto &reference : refecences) {
          // Skip if this reference was deleted
          if (ref_deleted.count({reference.asset_path.GetAssetPath(),
                                 reference.prim_path.prim_part()})) {
            continue;
          }
          // Lazy reference: check load_policy callback
          if (options.load_policy &&
              !options.load_policy(dst_prim_path, reference)) {
            DCOUT("Reference skipped by load_policy: "
                  << reference.asset_path.GetAssetPath());
            continue;
          }
          Layer layer;
          const PrimSpec *src_ps{nullptr};

          if (reference.asset_path.GetAssetPath().empty()) {
            if (reference.prim_path.is_absolute_path()) {
              // Internal reference (`references = </some/Prim>`). A target that
              // is not defined in this layer contributes no opinions — valid in
              // USD (OpenUSD does not fail). Skip it (src_ps stays null → the
              // `if (!src_ps) continue;` below) instead of aborting. Use a local
              // error string so a benign miss does not poison the shared `err`.
              std::string find_err;
              if (!in_layer.find_primspec_at(reference.prim_path, &src_ps,
                                             &find_err)) {
                if (warn) {
                  (*warn) += "Internal reference target <" +
                             reference.prim_path.prim_part() +
                             "> not found in this layer; skipped.\n";
                }
                src_ps = nullptr;
              } else if (reference.prim_path.prim_part() ==
                             dst_prim_path.prim_part() ||
                         reference.prim_path.prim_part() ==
                             primspec.name()) {
                // Self internal reference (`</A>` on A): a cycle. The external
                // branch guards cycles via `visited`; the internal branch did
                // not, letting the driver's fixed-point loop churn on it.
                // Reject the direct self-reference.
                if (warn) {
                  (*warn) += "Cyclic internal reference <" +
                             reference.prim_path.prim_part() +
                             "> targets its own prim; skipped.\n";
                }
                src_ps = nullptr;
              }

            } else {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Invalid asset path. assetPath is empty and "
                              "primPath is not absolute path: {}",
                              reference.prim_path.full_path_name()));
            }
          } else {

            DCOUT("reference.prim_path = " << reference.prim_path);
            DCOUT("primspec.cwp = " << cwp);
            DCOUT("primspec.search_paths = " << search_paths);

            // Cycle detection: check if this (asset, prim) pair has already been visited.
            auto visit_key = std::make_pair(
                reference.asset_path.GetAssetPath(),
                reference.prim_path.prim_part());
            if (visited.count(visit_key)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Cycle detected in `references`: asset `{}` prim <{}>",
                  visit_key.first, visit_key.second));
            }
            visited.insert(visit_key);

            if (!LoadAsset(resolver, cwp, search_paths, options.fileformats,
                           reference.asset_path, reference.prim_path, &layer,
                           &src_ps, /* error_when_no_prims_found */ true,
                           options.error_when_asset_not_found,
                           options.error_when_unsupported_fileformat,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, warn, err,
                           options.layer_cache)) {
              visited.erase(visit_key);
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Failed to `references` asset `{}`",
                              reference.asset_path.GetAssetPath()));
            }
            visited.erase(visit_key);
          }

          if (!src_ps) {
            // LoadAsset allowed not-found or unsupported file. so do nothing.
            continue;
          }

          PrimSpec prepared_src = PrepareComposedArcPrimSpec(
              *src_ps, reference.prim_path, dst_prim_path,
              reference.asset_path.GetAssetPath().empty(),
              reference.layerOffset,
              !reference.asset_path.GetAssetPath().empty());

          // AOUSD Core Spec 10.3.2.3/10.3.2.4: Propagate implied inherit/specialize
          // paths from the referenced prim before flattening consumes them.
          PropagateImpliedArcPaths(prepared_src, primspec);

          // AOUSD Core Spec 10.3.2.3: Record arc origin for implied inherit propagation
          if (!reference.asset_path.GetAssetPath().empty()) {
            ArcOrigin origin;
            origin.source_layer_id = reference.asset_path.GetAssetPath();
            origin.source_prim_path = reference.prim_path;
            primspec.metas().arc_origins.push_back(origin);
          }

          // `inherits` op (prepared_src is donated — capture what the
          // Model-type fixup below needs first)
          const std::string prepared_typeName = prepared_src.typeName();
          if (!InheritPrimSpec(primspec, std::move(prepared_src), warn, err)) {
            PUSH_ERROR_AND_RETURN(fmt::format("Failed to reference layer `{}`",
                                              reference.asset_path));
          }

          // Modify Prim type if this PrimSpec is Model type.
          if (primspec.typeName().empty() || primspec.typeName() == "Model") {
            if (prepared_typeName.empty() || prepared_typeName == "Model") {
              // pass
            } else {
              primspec.typeName() = prepared_typeName;
            }
          }

          DCOUT("inherit done: primspec = " << primspec.name());
        }

      } else if (qual == ListEditQual::Delete) {
        // Handled in pre-pass above — deleted refs are filtered from prepend/append.
      } else if (qual == ListEditQual::Order) {
        PushWarn("`order` references list edit: reordering not supported. Skipping.\n");
      } else if (qual == ListEditQual::Invalid) {
        PUSH_ERROR_AND_RETURN("Invalid listedit qualifier for `references`.");
      } else if (qual == ListEditQual::Add ||
                 qual == ListEditQual::Append) {
        // AOUSD Core Spec 6.6.3.10: `add` is deprecated, treat as `append`
        if (qual == ListEditQual::Add) {
          PUSH_WARN("`add` list edit qualifier is deprecated (AOUSD Core Spec 6.6.3.10). Treating as `append`.");
        }
        for (const auto &reference : refecences) {
          // Skip if this reference was deleted
          if (ref_deleted.count({reference.asset_path.GetAssetPath(),
                                 reference.prim_path.prim_part()})) {
            continue;
          }
          // Lazy reference: check load_policy callback
          if (options.load_policy &&
              !options.load_policy(dst_prim_path, reference)) {
            DCOUT("Reference skipped by load_policy: "
                  << reference.asset_path.GetAssetPath());
            continue;
          }
          Layer layer;
          const PrimSpec *src_ps{nullptr};

          if (reference.asset_path.GetAssetPath().empty()) {
            if (reference.prim_path.is_absolute_path()) {
              // Internal reference (`references = </some/Prim>`). A target that
              // is not defined in this layer contributes no opinions — valid in
              // USD (OpenUSD does not fail). Skip it (src_ps stays null → the
              // `if (!src_ps) continue;` below) instead of aborting. Use a local
              // error string so a benign miss does not poison the shared `err`.
              std::string find_err;
              if (!in_layer.find_primspec_at(reference.prim_path, &src_ps,
                                             &find_err)) {
                if (warn) {
                  (*warn) += "Internal reference target <" +
                             reference.prim_path.prim_part() +
                             "> not found in this layer; skipped.\n";
                }
                src_ps = nullptr;
              } else if (reference.prim_path.prim_part() ==
                             dst_prim_path.prim_part() ||
                         reference.prim_path.prim_part() ==
                             primspec.name()) {
                // Self internal reference (`</A>` on A): a cycle. The external
                // branch guards cycles via `visited`; the internal branch did
                // not, letting the driver's fixed-point loop churn on it.
                // Reject the direct self-reference.
                if (warn) {
                  (*warn) += "Cyclic internal reference <" +
                             reference.prim_path.prim_part() +
                             "> targets its own prim; skipped.\n";
                }
                src_ps = nullptr;
              }

            } else {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Invalid asset path. assetPath is empty and "
                              "primPath is not absolute path: {}",
                              reference.prim_path.full_path_name()));
            }
          } else {
            // Cycle detection for append references
            auto visit_key = std::make_pair(
                reference.asset_path.GetAssetPath(),
                reference.prim_path.prim_part());
            if (visited.count(visit_key)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Cycle detected in `references`: asset `{}` prim <{}>",
                  visit_key.first, visit_key.second));
            }
            visited.insert(visit_key);

            if (!LoadAsset(resolver, cwp, search_paths, options.fileformats,
                           reference.asset_path, reference.prim_path, &layer,
                           &src_ps, /* error_when_no_prims */ true,
                           options.error_when_asset_not_found,
                           options.error_when_unsupported_fileformat,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, warn, err,
                           options.layer_cache)) {
              visited.erase(visit_key);
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Failed to `references` asset `{}`",
                              reference.asset_path.GetAssetPath()));
            }
            visited.erase(visit_key);
          }

          if (!src_ps) {
            // LoadAsset allowed not-found or unsupported file. so do nothing.
            continue;
          }

          PrimSpec prepared_src = PrepareComposedArcPrimSpec(
              *src_ps, reference.prim_path, dst_prim_path,
              reference.asset_path.GetAssetPath().empty(),
              reference.layerOffset,
              !reference.asset_path.GetAssetPath().empty());

          // AOUSD Core Spec 10.3.2.3/10.3.2.4: Propagate implied arc paths.
          PropagateImpliedArcPaths(prepared_src, primspec);

          // AOUSD Core Spec 10.3.2.3: Record arc origin for implied inherit propagation
          if (!reference.asset_path.GetAssetPath().empty()) {
            ArcOrigin origin;
            origin.source_layer_id = reference.asset_path.GetAssetPath();
            origin.source_prim_path = reference.prim_path;
            primspec.metas().arc_origins.push_back(origin);
          }

          // LIVRPS: local(direct) opinions are stronger than referenced ones,
          // for both `prepend` and `append` (the qualifier only orders
          // reference arcs relative to each other). Merge the referenced
          // PrimSpec UNDERNEATH the local opinions, same as the prepend path
          // (OverridePrimSpec let the referenced layer clobber local values).
          if (!InheritPrimSpec(primspec, prepared_src, warn, err)) {
            PUSH_ERROR_AND_RETURN(fmt::format("Failed to reference layer `{}`",
                                              reference.asset_path));
          }

          // Keep the referenced asset's own unresolved arcs anchored at the
          // asset's directory (see ReanchorArcsToSource).
          ReanchorArcsToSource(primspec, prepared_src);

          // Modify Prim type if this PrimSpec is Model type.
          if (primspec.typeName().empty() || primspec.typeName() == "Model") {
            if (prepared_src.typeName().empty() ||
                prepared_src.typeName() == "Model") {
              // pass
            } else {
              primspec.typeName() = prepared_src.typeName();
            }
          }
        }
      }
    }
  }

  // NOTE: `primspec.metas().references` was cleared up front (before the loop).
  // It now holds only NESTED references carried in by the merge above, which the
  // outer fixed-point loop must still resolve — so we must NOT reset() it here.

  return true;
}

bool CompositeReferencesRec(uint32_t depth, AssetResolutionResolver &resolver,
                            const std::vector<std::string> &asset_search_paths,
                            const Path &dst_prim_path,
                            const Layer &in_layer,
                            PrimSpec &primspec /* [inout] */, std::string *warn,
                            std::string *err,
                            const ReferencesCompositionOptions &options,
                            ArcVisitedSet &visited) {
  if (depth > options.max_depth) {
    PUSH_ERROR_AND_RETURN("Too deep.");
  }

  // Traverse children first.
  for (auto &child : primspec.children()) {
    const Path parent_prim_path = dst_prim_path.AppendPrim(child.name());
    if (!CompositeReferencesRec(depth + 1, resolver, asset_search_paths,
                                parent_prim_path, in_layer, child, warn, err,
                                options, visited)) {
      return false;
    }
  }

  return CompositeReferencesLocal(resolver, dst_prim_path, in_layer, primspec,
                                  warn, err, options, visited);
}

// Own payload arcs only (no child traversal) — see CompositeReferencesLocal.
bool CompositePayloadLocal(AssetResolutionResolver &resolver,
                           const Path &dst_prim_path,
                           const Layer &in_layer,
                           PrimSpec &primspec /* [inout] */, std::string *warn,
                           std::string *err,
                           const PayloadCompositionOptions &options,
                           ArcVisitedSet &visited) {
  // Use PrimSpec's AssetResolution state.
  std::string cwp = primspec.get_current_working_path();
  std::vector<std::string> search_paths = primspec.get_asset_search_paths();

  if (primspec.metas().payload) {
    // Copy the payload listops before iterating: the loop body merges loaded
    // content INTO `primspec`, which can reallocate `primspec.metas().payload`
    // and invalidate a live iterator (use-after-free). See the matching note in
    // CompositeReferencesRec.
    const auto payload_ops = primspec.metas().payload.value();

    // Pre-pass: collect deleted payload targets so we can skip them.
    std::set<std::pair<std::string, std::string>> pl_deleted;
    for (const auto &payload_op : payload_ops) {
      if (payload_op.first == ListEditQual::Delete) {
        for (const auto &p : payload_op.second) {
          pl_deleted.insert({p.asset_path.GetAssetPath(), p.prim_path.prim_part()});
        }
      }
    }

    for (const auto &payload_op : payload_ops) {
      const ListEditQual &qual = payload_op.first;
      const auto &payloads = payload_op.second;

      if ((qual == ListEditQual::ResetToExplicit) ||
          (qual == ListEditQual::Prepend)) {
        for (const auto &pl : payloads) {
          // Skip if this payload was deleted
          if (pl_deleted.count({pl.asset_path.GetAssetPath(),
                                pl.prim_path.prim_part()})) {
            continue;
          }
          // Lazy payload: check load_policy callback
          if (options.load_policy &&
              !options.load_policy(dst_prim_path, pl)) {
            DCOUT("Payload skipped by load_policy: " << pl.asset_path.GetAssetPath());
            continue;
          }

          std::string asset_path = pl.asset_path.GetAssetPath();
          DCOUT("asset_path = " << asset_path);

          Layer layer;
          const PrimSpec *src_ps{nullptr};

          if (pl.asset_path.GetAssetPath().empty()) {
            if (pl.prim_path.is_absolute_path()) {
              // Inherit-like operation.

              if (!in_layer.find_primspec_at(pl.prim_path, &src_ps, err)) {
                return false;
              }
              if (pl.prim_path.prim_part() == dst_prim_path.prim_part() ||
                  pl.prim_path.prim_part() == primspec.name()) {
                // Self internal payload: a cycle (the external branch guards
                // cycles via `visited`; the internal branch did not). Reject.
                if (warn) {
                  (*warn) += "Cyclic internal payload <" +
                             pl.prim_path.prim_part() +
                             "> targets its own prim; skipped.\n";
                }
                src_ps = nullptr;
              }

            } else {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("primPath is not absolute path: {}",
                              pl.prim_path.full_path_name()));
            }
          } else {

            // Cycle detection for payload
            auto visit_key = std::make_pair(
                pl.asset_path.GetAssetPath(),
                pl.prim_path.prim_part());
            if (visited.count(visit_key)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Cycle detected in `payload`: asset `{}` prim <{}>",
                  visit_key.first, visit_key.second));
            }
            visited.insert(visit_key);

            if (!LoadAsset(resolver, cwp, search_paths, options.fileformats,
                           pl.asset_path, pl.prim_path, &layer, &src_ps,
                           /* error_when_no_prims_found */ true,
                           options.error_when_asset_not_found,
                           options.error_when_unsupported_fileformat,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, warn, err,
                           options.layer_cache)) {
              visited.erase(visit_key);
              PUSH_ERROR_AND_RETURN(fmt::format("Failed to `payload` asset `{}`",
                                                pl.asset_path.GetAssetPath()));
            }
            visited.erase(visit_key);
          }

          if (!src_ps) {
            // LoadAsset allowed not-found or unsupported file. so do nothing.
            continue;
          }

          PrimSpec prepared_src = PrepareComposedArcPrimSpec(
              *src_ps, pl.prim_path, dst_prim_path,
              pl.asset_path.GetAssetPath().empty(), pl.layerOffset,
              !pl.asset_path.GetAssetPath().empty());

          // AOUSD Core Spec 10.3.2.3/10.3.2.4: Propagate implied arc paths.
          PropagateImpliedArcPaths(prepared_src, primspec);

          // `inherits` op (prepared_src is donated — capture what the
          // Model-type fixup below needs first)
          const std::string prepared_typeName = prepared_src.typeName();
          if (!InheritPrimSpec(primspec, std::move(prepared_src), warn, err)) {
            PUSH_ERROR_AND_RETURN(
                fmt::format("Failed to payload layer `{}`", asset_path));
          }

          // Modify Prim type if this PrimSpec is Model type.
          if (primspec.typeName().empty() || primspec.typeName() == "Model") {
            if (prepared_typeName.empty() || prepared_typeName == "Model") {
              // pass
            } else {
              primspec.typeName() = prepared_typeName;
            }
          }

          DCOUT("inherit done: primspec = " << primspec.name());
        }

      } else if (qual == ListEditQual::Delete) {
        // Handled in pre-pass above — deleted payloads are filtered from prepend/append.
      } else if (qual == ListEditQual::Order) {
        PushWarn("`order` payloads list edit: reordering not supported. Skipping.\n");
      } else if (qual == ListEditQual::Invalid) {
        PUSH_ERROR_AND_RETURN("Invalid listedit qualifier for `payload`.");
      } else if (qual == ListEditQual::Add ||
                 qual == ListEditQual::Append) {
        // AOUSD Core Spec 6.6.3.10: `add` is deprecated, treat as `append`
        if (qual == ListEditQual::Add) {
          PUSH_WARN("`add` list edit qualifier is deprecated (AOUSD Core Spec 6.6.3.10). Treating as `append`.");
        }
        for (const auto &pl : payloads) {
          // Skip if this payload was deleted
          if (pl_deleted.count({pl.asset_path.GetAssetPath(),
                                pl.prim_path.prim_part()})) {
            continue;
          }
          // Lazy payload: check load_policy callback
          if (options.load_policy &&
              !options.load_policy(dst_prim_path, pl)) {
            DCOUT("Payload skipped by load_policy: " << pl.asset_path.GetAssetPath());
            continue;
          }

          std::string asset_path = pl.asset_path.GetAssetPath();

          Layer layer;
          const PrimSpec *src_ps{nullptr};

          if (pl.asset_path.GetAssetPath().empty()) {
            if (pl.prim_path.is_absolute_path()) {
              // Inherit-like operation.

              if (!in_layer.find_primspec_at(pl.prim_path, &src_ps, err)) {
                return false;
              }
              if (pl.prim_path.prim_part() == dst_prim_path.prim_part() ||
                  pl.prim_path.prim_part() == primspec.name()) {
                // Self internal payload: a cycle (the external branch guards
                // cycles via `visited`; the internal branch did not). Reject.
                if (warn) {
                  (*warn) += "Cyclic internal payload <" +
                             pl.prim_path.prim_part() +
                             "> targets its own prim; skipped.\n";
                }
                src_ps = nullptr;
              }

            } else {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("primPath is not absolute path: {}",
                              pl.prim_path.full_path_name()));
            }
          } else {

            // Cycle detection for append payload
            auto visit_key = std::make_pair(
                pl.asset_path.GetAssetPath(),
                pl.prim_path.prim_part());
            if (visited.count(visit_key)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Cycle detected in `payload`: asset `{}` prim <{}>",
                  visit_key.first, visit_key.second));
            }
            visited.insert(visit_key);

            if (!LoadAsset(resolver, cwp, search_paths, options.fileformats,
                           pl.asset_path, pl.prim_path, &layer, &src_ps,
                           /* error_when_no_prims_found */ true,
                           options.error_when_asset_not_found,
                           options.error_when_unsupported_fileformat,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, warn, err,
                           options.layer_cache)) {
              visited.erase(visit_key);
              PUSH_ERROR_AND_RETURN(fmt::format("Failed to `payload` asset `{}`",
                                                pl.asset_path.GetAssetPath()));
            }
            visited.erase(visit_key);
          }

          if (!src_ps) {
            // LoadAsset allowed not-found or unsupported file. so do nothing.
            continue;
          }

          PrimSpec prepared_src = PrepareComposedArcPrimSpec(
              *src_ps, pl.prim_path, dst_prim_path,
              pl.asset_path.GetAssetPath().empty(), pl.layerOffset,
              !pl.asset_path.GetAssetPath().empty());

          // AOUSD Core Spec 10.3.2.3/10.3.2.4: Propagate implied arc paths.
          PropagateImpliedArcPaths(prepared_src, primspec);

          // LIVRPS: local(direct) opinions are stronger than payloaded ones,
          // for both `prepend` and `append` (the qualifier only orders
          // payload arcs relative to each other). Merge the payloaded
          // PrimSpec UNDERNEATH the local opinions, same as the prepend path.
          if (!InheritPrimSpec(primspec, prepared_src, warn, err)) {
            PUSH_ERROR_AND_RETURN(
                fmt::format("Failed to payload layer `{}`", asset_path));
          }

          // A payloaded asset's OWN still-unresolved arcs must keep anchoring at
          // the asset's directory (core USD semantics), not the consuming prim's.
          // OverridePrimSpec leaves `primspec` carrying the consuming prim's
          // working path; if the payload target brought in unresolved reference/
          // payload arcs (e.g. a leaf component, payloaded by a sub-assembly
          // definition, whose subLayers reference `../../../fragment/geo/...`),
          // re-anchor to the target's directory so those nested arcs resolve.
          ReanchorArcsToSource(primspec, prepared_src);

          // Modify Prim type if this PrimSpec is Model type.
          if (primspec.typeName().empty() || primspec.typeName() == "Model") {
            if (prepared_src.typeName().empty() ||
                prepared_src.typeName() == "Model") {
              // pass
            } else {
              primspec.typeName() = prepared_src.typeName();
            }
          }
        }
      }
    }
  }

  // Remove `payload`.
  primspec.metas().payload.reset();

  return true;
}

bool CompositePayloadRec(uint32_t depth, AssetResolutionResolver &resolver,
                         const std::vector<std::string> &asset_search_paths,
                         const Path &dst_prim_path,
                         const Layer &in_layer,
                         PrimSpec &primspec /* [inout] */, std::string *warn,
                         std::string *err,
                         const PayloadCompositionOptions &options,
                         ArcVisitedSet &visited) {
  if (depth > options.max_depth) {
    PUSH_ERROR_AND_RETURN("Too deep.");
  }

  // Traverse children first.
  for (auto &child : primspec.children()) {
    const Path parent_prim_path = dst_prim_path.AppendPrim(child.name());
    if (!CompositePayloadRec(depth + 1, resolver, asset_search_paths,
                             parent_prim_path, in_layer, child, warn, err,
                             options, visited)) {
      return false;
    }
  }

  return CompositePayloadLocal(resolver, dst_prim_path, in_layer, primspec,
                               warn, err, options, visited);
}

// Own variant selection only (no child traversal) — see
// CompositeReferencesLocal for the shell/subtree split rationale.
bool CompositeVariantLocal(PrimSpec &primspec /* [inout] */, std::string *warn,
                           std::string *err) {
  PrimSpec dst;
  std::map<std::string, std::string>
      variant_selection;  // empty = use variant settings in PrimSpec.

  if (!VariantSelectPrimSpec(dst, std::move(primspec), variant_selection, warn,
                             err)) {
    return false;
  }

  primspec = std::move(dst);

  return true;
}

bool CompositeVariantRec(uint32_t depth, PrimSpec &primspec /* [inout] */,
                         std::string *warn, std::string *err) {
  if (depth > kMaxCompositionDepth) {
    PUSH_ERROR_AND_RETURN("Too deep.");
  }

  // Traverse children first.
  for (auto &child : primspec.children()) {
    if (!CompositeVariantRec(depth + 1, child, warn, err)) {
      return false;
    }
  }

  return CompositeVariantLocal(primspec, warn, err);
}

// Visited set for cycle detection in inherits/specializes.
// Tracks prim paths within the same layer.
using PathVisitedSet = std::set<std::string>;

// Resolve ListEditQual operations into a final ordered list.
// Implements: resetToExplicit clears + adds, prepend prepends, append appends,
// delete removes matching items, order reorders per the order vector.
//
// Matching is by a caller-supplied string key (`key`), which must satisfy
// key(a) == key(b)  <=>  "a and b are the same item". This replaces the former
// O(M*N) per-item linear scans in Delete/Order with hash-set/map lookups, so
// resolution is O(N) regardless of how many list ops or items are supplied
// (also a guard against pathologically large inherits/specializes lists). The
// produced result is identical to the previous predicate-based implementation.
template <typename T, typename KeyFn>
static std::vector<T> ResolveListOpsT(
    const std::vector<std::pair<ListEditQual, std::vector<T>>> &listops,
    KeyFn key, std::string *warn) {
  std::vector<T> result;

  for (const auto &op : listops) {
    const auto &qual = op.first;
    const auto &items = op.second;

    if (qual == ListEditQual::ResetToExplicit) {
      result.clear();
      result.insert(result.end(), items.begin(), items.end());
    } else if (qual == ListEditQual::Prepend) {
      result.insert(result.begin(), items.begin(), items.end());
    } else if (qual == ListEditQual::Append ||
               qual == ListEditQual::Add) {
      if (qual == ListEditQual::Add && warn) {
        (*warn) += "`add` list edit qualifier is deprecated (AOUSD Core Spec 6.6.3.10). Treating as `append`.\n";
      }
      result.insert(result.end(), items.begin(), items.end());
    } else if (qual == ListEditQual::Delete) {
      // Remove every result element whose key matches any deleted item.
      // Single stable pass; preserves the order of the survivors.
      std::unordered_set<std::string> del_keys;
      del_keys.reserve(items.size());
      for (const auto &del_item : items) del_keys.insert(key(del_item));
      result.erase(
          std::remove_if(result.begin(), result.end(),
                         [&del_keys, &key](const T &x) {
                           return del_keys.count(key(x)) != 0;
                         }),
          result.end());
    } else if (qual == ListEditQual::Order) {
      // Reorder items per the order vector.
      // Items in the order vector are placed in that relative order.
      // Items NOT in the order vector are prepended in their original order.
      // This follows the deprecated SdfListOp reorder semantics.
      if (items.empty() || result.empty()) continue;

      // First result element (in result order) for each key.
      std::unordered_map<std::string, const T *> first_match;
      first_match.reserve(result.size());
      for (const auto &r : result) {
        first_match.emplace(key(r), &r);  // keeps the first occurrence
      }
      // Membership set of the order vector's keys.
      std::unordered_set<std::string> item_keys;
      item_keys.reserve(items.size());
      for (const auto &anchor : items) item_keys.insert(key(anchor));

      std::vector<T> ordered;
      std::vector<T> unordered;

      // Items that appear in the order vector, emitted in order-vector order
      // (each anchor maps to the first matching result element).
      for (const auto &anchor : items) {
        auto it = first_match.find(key(anchor));
        if (it != first_match.end()) {
          ordered.push_back(*it->second);
        }
      }

      // Result elements NOT named by the order vector, in original order.
      for (const auto &r : result) {
        if (item_keys.count(key(r)) == 0) {
          unordered.push_back(r);
        }
      }

      // Reassemble: unordered first, then ordered
      result.clear();
      result.insert(result.end(), unordered.begin(), unordered.end());
      result.insert(result.end(), ordered.begin(), ordered.end());
    }
  }

  return result;
}

// Path version: match by prim_part()
static std::vector<Path> ResolveListOps(
    const std::vector<std::pair<ListEditQual, std::vector<Path>>> &listops,
    std::string *warn) {
  return ResolveListOpsT<Path>(listops,
      [](const Path &p) {
        auto v = p.prim_part();
        return std::string(v.data(), v.size());
      }, warn);
}

// Note: Reference and Payload use a pre-pass approach (collect deleted items
// into a set, then skip them during processing) rather than ResolveListOpsT,
// because they need to preserve the prepend/append distinction for different
// merge semantics (InheritPrimSpec vs OverridePrimSpec).

bool CompositeInheritsRec(uint32_t depth, const Layer &layer,
                          PrimSpec &primspec /* [inout] */, std::string *warn,
                          std::string *err,
                          PathVisitedSet &visited) {
  if (depth > kMaxCompositionDepth) {
    PUSH_ERROR_AND_RETURN("Too deep.");
  }

  // Traverse children first.
  for (auto &child : primspec.children()) {
    if (!CompositeInheritsRec(depth + 1, layer, child, warn, err, visited)) {
      return false;
    }
  }

  if (primspec.metas().inherits) {
    // Resolve all ListEditQual operations (prepend, append, delete, order)
    // into a single ordered list of paths before processing.
    auto inherits_copy = primspec.metas().inherits.value();
    std::vector<Path> resolved = ResolveListOps(inherits_copy, warn);

    // Process the resolved path list — each inherits target fills in defaults.
    for (const auto &inheritPath : resolved) {
      // Cycle detection
      std::string key = inheritPath.prim_part();
      if (visited.count(key)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Cycle detected in `inherits`: prim <{}>", key));
      }
      visited.insert(key);

      const PrimSpec *src_ps{nullptr};

      // Inheriting from a class that has no local opinions (or is not defined in
      // this layer) is valid in USD: the arc contributes nothing. Treat a
      // missing target as a no-op (matches OpenUSD) rather than failing the
      // whole composition. Use a local error string so a benign miss does not
      // poison the shared `err`.
      std::string find_err;
      if (!layer.find_primspec_at(inheritPath, &src_ps, &find_err) || !src_ps) {
        // Not a warning: inheriting a class that is undefined in the composed
        // layer is a standard, intentional USD idiom -- the `</__class__/...>`
        // override hook (e.g. every Animal Logic ALab entity root `prepend
        // inherits = </__class__/<name>>`, where `__class__` is never populated
        // here). OpenUSD/usdcat emit nothing; a warning per such arc just floods
        // the log (550+ on one ALab workbench). Debug-only.
        DCOUT("Inherit target <" << inheritPath.prim_part()
              << "> not found in this layer; no opinions to inherit (skipped).");
        visited.erase(key);
        continue;
      }

      if (!InheritPrimSpec(primspec, *src_ps, warn, err)) {
        visited.erase(key);
        return false;
      }
      visited.erase(key);
    }

    // remove `inherits` metadataum after processing.
    primspec.metas().inherits.reset();

    // Applying the inherit(s) above can copy in child PrimSpecs that themselves
    // carry `inherits` (multi-level classes — a class whose children inherit
    // further classes). Those children are introduced AFTER the children-first
    // traversal at the top of this function, so resolve them now, within THIS
    // pass. Otherwise the prim keeps reporting unresolved inherits and lusdcat's
    // outer loop re-runs CompositeInherits up to kMaxIteration (128) times,
    // re-copying large data every pass — the runaway behind oom.md's
    // teapotScene_animCycle (1644 vs OpenUSD's 24 inherit refs). `visited`
    // (erased above) still guards genuine cycles within each freshly-resolved
    // chain, and once a class is applied its copy's `inherits` is reset, so
    // acyclic multi-level chains terminate.
    for (auto &child : primspec.children()) {
      if (!CompositeInheritsRec(depth + 1, layer, child, warn, err, visited)) {
        return false;
      }
    }
  }

  // AOUSD Core Spec 10.3.2.3: Implied inherits.
  // If this prim has arc_origins (e.g., from references), and the referenced
  // layer's prim had inherits, those inherits should be "implied" here.
  // For single-level case: walk arc_origins and check if the source prim had
  // inherits that were already applied in the referenced layer. The inherit
  // targets in the referenced layer should also be applied as implied inherits
  // in this (upstream) layer stack, if the target classes exist here too.
  //
  // Implementation: For each arc origin, look for class prims (class specifier)
  // in the current layer that match the inherit targets from the referenced layer.
  // This is the single-level implied inherit case.
  // AOUSD Core Spec 10.3.2.3: Implied inherits (multi-level).
  // Process inheritPaths propagated from referenced/payload layers.
  // After applying an implied inherit, check if the inherited prim itself
  // has further inheritPaths that should cascade (multi-level propagation).
  {
    std::set<std::string> visited_inherits;
    constexpr size_t kMaxImpliedDepth = 32;
    size_t implied_depth = 0;

    auto process_inheritPaths = [&](auto &&self) -> bool {
      if (!primspec.metas().inheritPaths || implied_depth >= kMaxImpliedDepth) {
        return true;
      }
      implied_depth++;

      auto ip_copy = primspec.metas().inheritPaths.value();
      primspec.metas().inheritPaths.reset();

      for (const auto &inherit_op : ip_copy) {
        const auto &ip_paths = inherit_op.second;
        for (const auto &inheritPath : ip_paths) {
          std::string key = inheritPath.prim_part();
          if (visited_inherits.count(key)) continue;  // prevent cycles

          const PrimSpec *src_ps{nullptr};
          std::string _err;
          if (layer.find_primspec_at(inheritPath, &src_ps, &_err) && src_ps) {
            visited_inherits.insert(key);
            DCOUT("Applying implied inherit (level " << implied_depth
                  << ") from " << key);

            // If the inherited prim has its own inheritPaths, propagate them
            // to this prim before applying the inherit.
            if (src_ps->metas().inheritPaths) {
              for (const auto &nested_op : src_ps->metas().inheritPaths.value()) {
                if (!primspec.metas().inheritPaths) {
                  primspec.metas().inheritPaths.emplace();
                }
                primspec.metas().inheritPaths->push_back(nested_op);
              }
            }

            if (!InheritPrimSpec(primspec, *src_ps, warn, err)) {
              return false;
            }

            // Recursively process any newly added inheritPaths
            if (!self(self)) {
              return false;
            }
          }
        }
      }
      return true;
    };

    if (!process_inheritPaths(process_inheritPaths)) {
      return false;
    }
  }

  return true;
}

bool ExtractReferencesAssetPathsImpl(const PrimSpec &primspec, std::vector<std::string> &paths) {

  constexpr size_t kMaxIter = 1024 * 1024;

  std::vector<const PrimSpec *> stack;
  stack.push_back(&primspec);
  size_t iter = 0;

  while (!stack.empty()) {
    if (iter++ > kMaxIter) {
      return false;
    }

    const PrimSpec *current = stack.back();
    stack.pop_back();

    if (current->metas().references) {
      // Iterate over all listops (supports multiple listops per arc)
      for (const auto &ref_op : current->metas().references.value()) {
        // TODO: qualifier
        //const ListEditQual &qual = ref_op.first;
        const auto &refecences = ref_op.second;

        for (const auto &reference : refecences) {
          std::string asset_path = reference.asset_path.GetAssetPath();
          if (!asset_path.empty()) {
            paths.push_back(std::move(asset_path));
          }
        }
      }
    }

    // Push children in reverse order to preserve DFS order.
    const auto &children = current->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(&(*it));
    }
  }

  return true;

}

} // namespace


std::vector<std::string> ExtractReferencesAssetPaths(const Layer &layer) {

  std::vector<std::string> paths;

  for (const auto &ps : layer.primspecs()) {
    ExtractReferencesAssetPathsImpl(ps.second, paths);
  }

  return paths;

}


namespace {

// Internal implementation that accepts a shared visited set for cross-arc cycle detection.
#if defined(LIGHTUSD_ENABLE_THREAD)
// ---------------------------------------------------------------------------
// Parallel layer-cache pre-warm.
//
// Reference/payload composition is serial (LIVRPS merge order matters), but
// most of its wall time on asset-heavy scenes is parsing the referenced
// files. When a layer_cache is provided, warm it up front: collect every
// authored (assetPath, resolution context) arc in the layer's primspec
// trees, LoadAsset them on worker threads (per-worker resolver copies and
// per-worker caches, merged into the shared cache after each wave), and
// repeat over the newly loaded layers so nested arcs are warmed too
// (bounded BFS). The subsequent serial composition then runs on cache hits.
//
// Correctness notes:
// - Arcs inside variant statements are NOT collected; they simply stay cache
//   misses and load serially during composition (unchanged behavior). Same
//   for arcs that only appear after variant selection.
// - Warming never fails composition: per-arc errors are discarded — a failed
//   asset is not cached, so composition reaches it serially and produces the
//   same error it always did. Warnings emitted while warming (e.g. the
//   suffix-fallback warn_once, whose process-wide dedup would otherwise
//   swallow them) are merged into `warn` in deterministic arc order.
// - Duplicate cache keys across workers hold identical parsed layers (same
//   file, same context), so merge pick order does not matter.
// - Runs once per cache: a marker entry (key "\x01warmed" — can never
//   collide with a resolved path) suppresses re-warming on the later
//   composition passes of the fixed-point flatten loop; arcs introduced by
//   those later passes load serially into the same cache as before.
struct WarmArc {
  value::AssetPath asset_path;
  Path prim_path;
  std::string cwp;
  std::vector<std::string> search_paths;
};

void CollectWarmArcsRec(const PrimSpec &ps, uint32_t depth,
                        std::vector<WarmArc> *out) {
  if (depth > 1024) {
    return;
  }
  const auto add_arcs = [&](const auto &listops) {
    for (const auto &qual_and_vec : listops) {
      // Deleted arcs are collected too — worst case we parse a file the
      // composition then skips; the parse is shared via the cache anyway.
      for (const auto &arc : qual_and_vec.second) {
        if (arc.asset_path.GetAssetPath().empty()) {
          continue;  // internal arc — no file to load
        }
        WarmArc w;
        w.asset_path = arc.asset_path;
        w.prim_path = arc.prim_path;
        w.cwp = ps.get_current_working_path();
        w.search_paths = ps.get_asset_search_paths();
        out->push_back(std::move(w));
      }
    }
  };
  if (ps.metas().references) {
    add_arcs(ps.metas().references.value());
  }
  if (ps.metas().payload) {
    add_arcs(ps.metas().payload.value());
  }
  for (const auto &child : ps.children()) {
    CollectWarmArcsRec(child, depth + 1, out);
  }
}

void WarmLayerCacheParallel(
    const AssetResolutionResolver &base_resolver, const Layer &in_layer,
    const lightusd::HashMap<std::string, FileFormatHandler> &fileformats,
    bool allow_parent_relative_paths, size_t max_asset_bytes,
    std::map<std::string, Layer> *layer_cache, std::string *warn) {
  if (!layer_cache) {
    return;
  }
  if (std::thread::hardware_concurrency() <= 1) {
    return;
  }
  static const char *kWarmedMarkerKey = "\x01warmed";
  if (layer_cache->count(kWarmedMarkerKey)) {
    return;
  }

  std::vector<WarmArc> wave;
  for (const auto &item : in_layer.primspecs()) {
    CollectWarmArcsRec(item.second, 0, &wave);
  }
  if (wave.size() < 4) {
    return;  // not worth spinning up workers
  }
  (*layer_cache)[kWarmedMarkerKey] = Layer();
  const bool warm_verbose = []() {
    const char *e = ::getenv("LIGHTUSD_FLATTEN_TIMING");
    return e && e[0] == '1';
  }();
  auto warm_t0 = std::chrono::steady_clock::now();

  // (authored asset path + context) tuples already warmed this call — bounds
  // the BFS even with circular references.
  std::set<std::string> done;
  auto tuple_key = [](const WarmArc &w) {
    std::string k = w.asset_path.GetAssetPath();
    k += '\x01';
    k += w.cwp;
    for (const auto &sp : w.search_paths) {
      k += '\x01';
      k += sp;
    }
    return k;
  };

  constexpr uint32_t kMaxWaves = 16;
  for (uint32_t wave_idx = 0; wave_idx < kMaxWaves && !wave.empty();
       wave_idx++) {
    std::vector<WarmArc> todo;
    todo.reserve(wave.size());
    for (auto &w : wave) {
      if (done.insert(tuple_key(w)).second) {
        todo.push_back(std::move(w));
      }
    }
    wave.clear();
    if (todo.empty()) {
      break;
    }

    struct ArcResult {
      std::map<std::string, Layer> local_cache;
      std::string local_warn;
    };
    std::vector<ArcResult> results(todo.size());

    {
      const uint32_t hw = std::thread::hardware_concurrency();
      const uint32_t nworkers =
          static_cast<uint32_t>((std::min)(size_t(hw), todo.size()));
      std::atomic<size_t> next{0};
      auto work = [&]() {
        // Per-worker resolver: LoadAsset mutates resolver state (working
        // path, search paths).
        AssetResolutionResolver res = base_resolver;
        for (;;) {
          const size_t i = next.fetch_add(1);
          if (i >= todo.size()) break;
          const WarmArc &w = todo[i];
          Layer discard_layer;
          const PrimSpec *discard_root = nullptr;
          std::string arc_err;  // discarded — composition re-reports serially
          ArcResult &r = results[i];
          (void)LoadAsset(res, w.cwp, w.search_paths, fileformats,
                          w.asset_path, w.prim_path, &discard_layer,
                          &discard_root,
                          /* error_when_no_prims_found */ false,
                          /* error_when_asset_not_found */ false,
                          /* error_when_unsupported_fileformat */ false,
                          allow_parent_relative_paths, max_asset_bytes,
                          &r.local_warn, &arc_err, &r.local_cache);
        }
      };
      std::vector<std::thread> workers;
      workers.reserve(nworkers);
      for (uint32_t t = 1; t < nworkers; t++) {
        workers.emplace_back(work);
      }
      work();
      for (auto &th : workers) th.join();
    }

    // Merge: warnings in deterministic arc order; layers new to the shared
    // cache feed the next wave (their primspecs carry the stamped resolution
    // context the nested arcs need).
    if (warm_verbose) {
      std::cerr << "[lightusd] layer-cache warm wave " << wave_idx << ": "
                << todo.size() << " arcs, "
                << std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - warm_t0)
                       .count()
                << " s elapsed\n";
    }
    for (auto &r : results) {
      if (warn && !r.local_warn.empty()) {
        (*warn) += r.local_warn;
      }
      for (auto &kv : r.local_cache) {
        auto ins = layer_cache->emplace(kv.first, Layer());
        if (ins.second) {
          ins.first->second = std::move(kv.second);
          for (const auto &pitem : ins.first->second.primspecs()) {
            CollectWarmArcsRec(pitem.second, 0, &wave);
          }
        }
      }
    }
  }
}
#endif  // LIGHTUSD_ENABLE_THREAD

#if defined(LIGHTUSD_ENABLE_THREAD)

// One unit of parallel composition work: a prim subtree composed
// independently of its siblings (all mutations stay inside the subtree).
struct CompSubtreeTask {
  PrimSpec *ps{nullptr};
  Path dst_path;
  uint32_t depth{0};
};

// Split the layer's prim forest into >= `target` independent subtree tasks
// plus the list of shell ancestors (level-order discovery). The composition
// passes are post-order (children before a prim's own arcs), so callers run
// the tasks in parallel first, then the shells' OWN arcs serially in REVERSE
// discovery order (deepest shells first — every descendant is then done).
void CollectCompSubtreeFrontier(Layer &layer, size_t target,
                                std::vector<CompSubtreeTask> *tasks,
                                std::vector<CompSubtreeTask> *shells) {
  std::vector<CompSubtreeTask> cur;
  for (auto &item : layer.primspecs()) {
    cur.push_back({&item.second, Path("/" + item.first, ""), 0});
  }
  for (;;) {
    if (cur.size() + tasks->size() >= target) break;
    bool any_expanded = false;
    std::vector<CompSubtreeTask> next;
    for (auto &node : cur) {
      if (!node.ps->children().empty()) {
        shells->push_back(node);
        any_expanded = true;
        for (auto &child : node.ps->children()) {
          next.push_back({&child, node.dst_path.AppendPrim(child.name()),
                          node.depth + 1});
        }
      } else {
        tasks->push_back(node);  // leaf: only its own arcs remain
      }
    }
    cur = std::move(next);
    if (!any_expanded) break;
  }
  tasks->insert(tasks->end(), cur.begin(), cur.end());
}

// Run `fn(task, warn, err)` over the tasks on `nthreads` workers. Per-task
// warn/err strings are merged in task order (deterministic); the first failed
// task's error (lowest index) wins. Subtrees are disjoint, so `fn` must only
// mutate the task's own subtree plus internally-synchronized shared caches.
template <typename Fn>
bool RunCompSubtreesParallel(std::vector<CompSubtreeTask> &tasks,
                             size_t nthreads, std::string *warn,
                             std::string *err, Fn &&fn) {
  std::vector<std::string> task_warns(tasks.size());
  std::vector<std::string> task_errs(tasks.size());
  std::vector<uint8_t> task_ok(tasks.size(), 1);
  std::atomic<size_t> cursor{0};
  auto worker = [&]() {
    for (;;) {
      const size_t i = cursor.fetch_add(1);
      if (i >= tasks.size()) break;
      task_ok[i] = fn(tasks[i], &task_warns[i], &task_errs[i]) ? 1 : 0;
    }
  };
  std::vector<std::thread> ths;
  const size_t n = (std::min)(nthreads, tasks.size());
  ths.reserve(n);
  for (size_t t = 0; t < n; t++) {
    ths.emplace_back(worker);
  }
  for (auto &th : ths) {
    th.join();
  }
  for (size_t i = 0; i < tasks.size(); i++) {
    if (warn && !task_warns[i].empty()) {
      (*warn) += task_warns[i];
    }
    if (!task_ok[i]) {
      if (err && !task_errs[i].empty()) {
        (*err) += task_errs[i];
      }
      return false;
    }
  }
  return true;
}

// Composition worker-thread budget (matches the crate writer's cap).
size_t CompParallelThreads() {
  const unsigned hw = std::thread::hardware_concurrency();
  return (std::max<size_t>)(
      1, (std::min<size_t>)(static_cast<size_t>(hw ? hw : 1), size_t(16)));
}

#endif  // LIGHTUSD_ENABLE_THREAD

bool CompositeReferencesImpl(AssetResolutionResolver &resolver,
                             const Layer &in_layer, Layer *composited_layer,
                             std::string *warn, std::string *err,
                             const ReferencesCompositionOptions &options,
                             ArcVisitedSet &visited) {
  if (!composited_layer) {
    return false;
  }

  std::vector<std::string> search_paths = in_layer.get_asset_search_paths();

#if defined(LIGHTUSD_ENABLE_THREAD)
  if (options.layer_cache) {
    WarmLayerCacheParallel(resolver, in_layer, options.fileformats,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, options.layer_cache, warn);
  }
#endif

  Layer dst = in_layer;  // deep copy

  for (auto &item : dst.primspecs()) {
    Path primPath("/" + item.first, "");
    if (!CompositeReferencesRec(/* depth */ 0, resolver, search_paths, primPath, in_layer,
                                item.second, warn, err, options, visited)) {
      if (err) { (*err) += "Composite `references` failed.\n"; }
      return false;
    }
  }

  (*composited_layer) = std::move(dst);

  DCOUT("Composite `references` ok.");
  return true;
}

}  // namespace

bool CompositeReferences(AssetResolutionResolver &resolver,
                         const Layer &in_layer, Layer *composited_layer,
                         std::string *warn, std::string *err,
                         ReferencesCompositionOptions options) {
  ArcVisitedSet visited;
  return CompositeReferencesImpl(resolver, in_layer, composited_layer,
                                 warn, err, options, visited);
}

namespace {

// True when any prim in the layer authors an internal arc (a reference or
// payload whose asset path is empty: `references = </some/Prim>`). Internal
// arcs are resolved by looking prims up in the *pristine* input layer, so the
// move-based InPlace composition (which consumes the input) must fall back to
// the copying path when they are present.
bool LayerHasInternalArcs(const Layer &layer, bool check_references,
                          bool check_payload) {
  std::vector<const PrimSpec *> stack;
  for (const auto &item : layer.primspecs()) {
    stack.push_back(&item.second);
  }
  while (!stack.empty()) {
    const PrimSpec *ps = stack.back();
    stack.pop_back();

    if (check_references && ps->metas().references) {
      for (const auto &ref_op : ps->metas().references.value()) {
        for (const auto &ref : ref_op.second) {
          if (ref.asset_path.GetAssetPath().empty()) {
            return true;
          }
        }
      }
    }
    if (check_payload && ps->metas().payload) {
      for (const auto &pl_op : ps->metas().payload.value()) {
        for (const auto &pl : pl_op.second) {
          if (pl.asset_path.GetAssetPath().empty()) {
            return true;
          }
        }
      }
    }

    for (const auto &child : ps->children()) {
      stack.push_back(&child);
    }
    for (const auto &variant_set_item : ps->variantSets()) {
      for (const auto &variant_item : variant_set_item.second.variantSet) {
        stack.push_back(&variant_item.second);
      }
    }
  }
  return false;
}

}  // namespace

bool CompositeReferencesInPlace(AssetResolutionResolver &resolver,
                                std::unique_ptr<Layer> layer,
                                Layer *composited_layer, std::string *warn,
                                std::string *err,
                                ReferencesCompositionOptions options) {
  if (!layer || !composited_layer) {
    return false;
  }

  ArcVisitedSet visited;

#if defined(LIGHTUSD_ENABLE_THREAD)
  if (options.layer_cache) {
    WarmLayerCacheParallel(resolver, *layer, options.fileformats,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, options.layer_cache, warn);
  }
#endif

  // Internal references need pristine-layer lookups: keep the input alive and
  // use the copying path.
  if (LayerHasInternalArcs(*layer, /* references */ true, /* payload */ false)) {
    return CompositeReferencesImpl(resolver, *layer, composited_layer, warn,
                                   err, options, visited);
  }

  // No internal arcs: consume the input instead of deep-copying it, halving
  // the peak of the pass (the copying path holds input + output).
  std::vector<std::string> search_paths = layer->get_asset_search_paths();
  Layer dst = std::move(*layer);
  layer.reset();

  // Parallel per-subtree composition: sibling subtrees only mutate
  // themselves, LoadAsset's shared layer cache is mutex-guarded, each task
  // gets its own resolver copy (LoadAsset re-seeds resolver state from its
  // args on every call) and its own cycle set (the serial chain set is
  // insert/erase-balanced, so it is empty between top-level prims). Internal
  // references would read the tree being mutated — this path is only taken
  // when the scan above found none. `dst` is the internal-lookup layer and is
  // never consulted here for the same reason.
  bool parallel_done = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    const size_t nthreads = CompParallelThreads();
    if (nthreads > 1) {
      std::vector<CompSubtreeTask> tasks;
      std::vector<CompSubtreeTask> shells;
      CollectCompSubtreeFrontier(dst, (std::max<size_t>)(512, 32 * nthreads),
                                 &tasks, &shells);
      if (tasks.size() >= 2) {
        if (!RunCompSubtreesParallel(
                tasks, nthreads, warn, err,
                [&](CompSubtreeTask &task, std::string *twarn,
                    std::string *terr) {
                  AssetResolutionResolver task_resolver = resolver;
                  ArcVisitedSet task_visited;
                  return CompositeReferencesRec(
                      task.depth, task_resolver, search_paths, task.dst_path,
                      dst, *task.ps, twarn, terr, options, task_visited);
                })) {
          if (err) {
            (*err) += "Composite `references` failed.\n";
          }
          return false;
        }
        // Shell ancestors' own arcs, deepest-first (all descendants done).
        for (auto it = shells.rbegin(); it != shells.rend(); ++it) {
          if (!CompositeReferencesLocal(resolver, it->dst_path, dst, *(it->ps),
                                        warn, err, options, visited)) {
            if (err) {
              (*err) += "Composite `references` failed.\n";
            }
            return false;
          }
        }
        parallel_done = true;
      }
    }
  }
#endif

  if (!parallel_done) {
    for (auto &item : dst.primspecs()) {
      Path primPath("/" + item.first, "");
      // `dst` doubles as the internal-lookup layer; never consulted since the
      // scan above found no internal arcs (arcs appended during this pass are
      // processed by the next fixed-point iteration, which rescans).
      if (!CompositeReferencesRec(/* depth */ 0, resolver, search_paths,
                                  primPath, dst, item.second, warn, err,
                                  options, visited)) {
        if (err) {
          (*err) += "Composite `references` failed.\n";
        }
        return false;
      }
    }
  }

  (*composited_layer) = std::move(dst);
  return true;
}

namespace {

bool ExtractPayloadAssetPathsImpl(const PrimSpec &primspec, std::vector<std::string> &paths) {

  constexpr size_t kMaxIter = 1024 * 1024;

  std::vector<const PrimSpec *> stack;
  stack.push_back(&primspec);
  size_t iter = 0;

  while (!stack.empty()) {
    if (iter++ > kMaxIter) {
      return false;
    }

    const PrimSpec *current = stack.back();
    stack.pop_back();

    if (current->metas().payload) {
      // Iterate over all listops (supports multiple listops per arc)
      for (const auto &payload_op : current->metas().payload.value()) {
        // TODO: qualifier
        //const ListEditQual &qual = payload_op.first;
        const auto &payload = payload_op.second;

        for (const auto &pl : payload) {
          std::string asset_path = pl.asset_path.GetAssetPath();
          if (!asset_path.empty()) {
            paths.push_back(std::move(asset_path));
          }
        }
      }
    }

    // Push children in reverse order to preserve DFS order.
    const auto &children = current->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(&(*it));
    }
  }

  return true;

}

} // namespace


std::vector<std::string> ExtractPayloadAssetPaths(const Layer &layer) {

  std::vector<std::string> paths;

  for (const auto &ps : layer.primspecs()) {
    ExtractPayloadAssetPathsImpl(ps.second, paths);
  }

  return paths;

}


namespace {

bool CompositePayloadImpl(AssetResolutionResolver &resolver, const Layer &in_layer,
                          Layer *composited_layer, std::string *warn,
                          std::string *err, const PayloadCompositionOptions &options,
                          ArcVisitedSet &visited) {
  if (!composited_layer) {
    return false;
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  if (options.layer_cache) {
    WarmLayerCacheParallel(resolver, in_layer, options.fileformats,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, options.layer_cache, warn);
  }
#endif

  Layer dst = in_layer;  // deep copy

  for (auto &item : dst.primspecs()) {
    Path primPath("/" + item.first, "");
    if (!CompositePayloadRec(/* depth */ 0, resolver,
                             item.second.get_asset_search_paths(), primPath, in_layer, item.second,
                             warn, err, options, visited)) {
      if (err) { (*err) += "Composite `payload` failed.\n"; }
      return false;
    }
  }

  (*composited_layer) = std::move(dst);

  DCOUT("Composite `payload` ok.");
  return true;
}

}  // namespace

bool CompositePayload(AssetResolutionResolver &resolver, const Layer &in_layer,
                      Layer *composited_layer, std::string *warn,
                      std::string *err, PayloadCompositionOptions options) {
  ArcVisitedSet visited;
  return CompositePayloadImpl(resolver, in_layer, composited_layer,
                              warn, err, options, visited);
}

bool CompositePayloadInPlace(AssetResolutionResolver &resolver,
                             std::unique_ptr<Layer> layer,
                             Layer *composited_layer, std::string *warn,
                             std::string *err,
                             PayloadCompositionOptions options) {
  if (!layer || !composited_layer) {
    return false;
  }

  ArcVisitedSet visited;

#if defined(LIGHTUSD_ENABLE_THREAD)
  if (options.layer_cache) {
    WarmLayerCacheParallel(resolver, *layer, options.fileformats,
                           options.allow_parent_relative_paths,
                           options.max_asset_bytes, options.layer_cache, warn);
  }
#endif

  // Internal payload arcs need pristine-layer lookups: copying path.
  if (LayerHasInternalArcs(*layer, /* references */ false, /* payload */ true)) {
    return CompositePayloadImpl(resolver, *layer, composited_layer, warn, err,
                                options, visited);
  }

  // No internal arcs: consume the input instead of deep-copying it.
  Layer dst = std::move(*layer);
  layer.reset();

  // Parallel per-subtree composition — see CompositeReferencesInPlace for the
  // isolation argument (same LoadAsset cache mutex / per-task resolver +
  // cycle set / no-internal-arcs precondition).
  bool parallel_done = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    const size_t nthreads = CompParallelThreads();
    if (nthreads > 1) {
      std::vector<CompSubtreeTask> tasks;
      std::vector<CompSubtreeTask> shells;
      CollectCompSubtreeFrontier(dst, (std::max<size_t>)(512, 32 * nthreads),
                                 &tasks, &shells);
      if (tasks.size() >= 2) {
        if (!RunCompSubtreesParallel(
                tasks, nthreads, warn, err,
                [&](CompSubtreeTask &task, std::string *twarn,
                    std::string *terr) {
                  AssetResolutionResolver task_resolver = resolver;
                  ArcVisitedSet task_visited;
                  return CompositePayloadRec(
                      task.depth, task_resolver,
                      task.ps->get_asset_search_paths(), task.dst_path, dst,
                      *task.ps, twarn, terr, options, task_visited);
                })) {
          if (err) {
            (*err) += "Composite `payload` failed.\n";
          }
          return false;
        }
        for (auto it = shells.rbegin(); it != shells.rend(); ++it) {
          if (!CompositePayloadLocal(resolver, it->dst_path, dst, *(it->ps),
                                     warn, err, options, visited)) {
            if (err) {
              (*err) += "Composite `payload` failed.\n";
            }
            return false;
          }
        }
        parallel_done = true;
      }
    }
  }
#endif

  if (!parallel_done) {
    for (auto &item : dst.primspecs()) {
      Path primPath("/" + item.first, "");
      if (!CompositePayloadRec(/* depth */ 0, resolver,
                               item.second.get_asset_search_paths(), primPath,
                               dst, item.second, warn, err, options, visited)) {
        if (err) {
          (*err) += "Composite `payload` failed.\n";
        }
        return false;
      }
    }
  }

  (*composited_layer) = std::move(dst);
  return true;
}

namespace {

// Shared body of CompositeVariant/CompositeVariantInPlace: composes an
// already-owned layer (the copying entry point pays the deep copy, the
// in-place one just moves).
bool CompositeVariantOwned(Layer &&owned, Layer *composited_layer,
                           std::string *warn, std::string *err) {
  if (!composited_layer) {
    return false;
  }

  Layer dst = std::move(owned);

  // Parallel per-subtree variant selection: VariantSelectPrimSpec only
  // touches the prim it is applied to, so sibling subtrees are independent
  // (no resolver, no I/O, no cycle set).
  bool parallel_done = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    const size_t nthreads = CompParallelThreads();
    if (nthreads > 1) {
      std::vector<CompSubtreeTask> tasks;
      std::vector<CompSubtreeTask> shells;
      CollectCompSubtreeFrontier(dst, (std::max<size_t>)(512, 32 * nthreads),
                                 &tasks, &shells);
      if (tasks.size() >= 2) {
        if (!RunCompSubtreesParallel(
                tasks, nthreads, warn, err,
                [&](CompSubtreeTask &task, std::string *twarn,
                    std::string *terr) {
                  return CompositeVariantRec(task.depth, *task.ps, twarn,
                                             terr);
                })) {
          PUSH_ERROR_AND_RETURN("Composite `variantSet` failed.");
        }
        for (auto it = shells.rbegin(); it != shells.rend(); ++it) {
          if (!CompositeVariantLocal(*(it->ps), warn, err)) {
            PUSH_ERROR_AND_RETURN("Composite `variantSet` failed.");
          }
        }
        parallel_done = true;
      }
    }
  }
#endif

  if (!parallel_done) {
    for (auto &item : dst.primspecs()) {
      if (!CompositeVariantRec(/* depth */ 0, item.second, warn, err)) {
        PUSH_ERROR_AND_RETURN("Composite `variantSet` failed.");
      }
    }
  }

  (*composited_layer) = std::move(dst);

  DCOUT("Composite `variantSet` ok.");
  return true;
}

}  // namespace

bool CompositeVariant(const Layer &in_layer, Layer *composited_layer,
                      std::string *warn, std::string *err) {
  Layer dst = in_layer;  // deep copy
  return CompositeVariantOwned(std::move(dst), composited_layer, warn, err);
}

bool CompositeVariantInPlace(std::unique_ptr<Layer> layer,
                             Layer *composited_layer, std::string *warn,
                             std::string *err) {
  if (!layer) {
    return false;
  }
  Layer dst = std::move(*layer);
  layer.reset();
  return CompositeVariantOwned(std::move(dst), composited_layer, warn, err);
}

bool CompositeInherits(const Layer &in_layer, Layer *composited_layer,
                       std::string *warn, std::string *err) {
  if (!composited_layer) {
    return false;
  }

  Layer dst = in_layer;  // deep copy

  PathVisitedSet visited;  // Cycle detection set

  for (auto &item : dst.primspecs()) {
    if (!CompositeInheritsRec(/* depth */ 0, dst, item.second, warn, err, visited)) {
      PUSH_ERROR_AND_RETURN("Composite `inherits` failed.");
    }
  }

  (*composited_layer) = std::move(dst);

  DCOUT("Composite `inherits` ok.");
  return true;
}

// Forward declare InheritPrimSpecImpl — reused by Specializes (same semantics)
namespace detail {
static bool InheritPrimSpecImpl(PrimSpec &dst, PrimSpec &src, bool consume_src,
                                std::string *warn, std::string *err);
}  // namespace detail

static bool CompositeSpecializesRec(uint32_t depth, const Layer &layer,
                                    PrimSpec &primspec /* [inout] */,
                                    std::string *warn, std::string *err,
                                    PathVisitedSet &visited) {
  if (depth > kMaxCompositionDepth) {
    PUSH_ERROR_AND_RETURN("Too deep in CompositeSpecializesRec.");
  }

  // Traverse children first.
  for (auto &child : primspec.children()) {
    if (!CompositeSpecializesRec(depth + 1, layer, child, warn, err, visited)) {
      return false;
    }
  }

  if (primspec.metas().specializes) {
    // Resolve all ListEditQual operations into a single ordered path list.
    auto specializes_copy = primspec.metas().specializes.value();
    std::vector<Path> resolved = ResolveListOps(specializes_copy, warn);

    // Process the resolved path list.
    for (const auto &specializePath : resolved) {
      // Cycle detection
      std::string key = specializePath.prim_part();
      if (visited.count(key)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Cycle detected in `specializes`: prim <{}>", key));
      }
      visited.insert(key);

      const PrimSpec *src_ps{nullptr};

      // Specializing a class that has no local opinions (or is not defined in
      // this layer) is valid in USD: the arc contributes nothing. Treat a
      // missing target as a no-op (matches OpenUSD, and mirrors the inherits
      // handling above) rather than failing the whole composition. This is what
      // unblocks referenced assets that specialize an internal class sibling not
      // pulled in by the reference (e.g. Animal Logic ALab entities, whose roots
      // `prepend specializes = </_root_type>`). Use a local error string so a
      // benign miss does not poison the shared `err`.
      std::string find_err;
      if (!layer.find_primspec_at(specializePath, &src_ps, &find_err) || !src_ps) {
        // Not a warning (see the inherits counterpart above): specializing an
        // undefined class is a standard USD idiom -- every ALab entity root
        // `prepend specializes = </_root_type>`, and `_root_type`/`__class__` are
        // intentionally unpopulated here. OpenUSD/usdcat emit nothing. Debug-only.
        DCOUT("Specialize target <" << specializePath.prim_part()
              << "> not found in this layer; no opinions to specialize (skipped).");
        visited.erase(key);
        continue;
      }

      if (!detail::InheritPrimSpecImpl(primspec,
                                       const_cast<PrimSpec &>(*src_ps),
                                       /* consume_src */ false, warn, err)) {
        visited.erase(key);
        return false;
      }
      visited.erase(key);
    }

    // Clear specializes metadata after processing.
    primspec.metas().specializes.reset();
  }

  // AOUSD Core Spec 10.3.2.4: Implied specializes.
  // If this prim has specializePaths propagated from referenced layers,
  // apply those specializes from matching prims in the current layer.
  if (primspec.metas().specializePaths) {
    auto sp_copy = primspec.metas().specializePaths.value();
    for (const auto &sp_op : sp_copy) {
      const auto &paths = sp_op.second;
      for (const auto &spPath : paths) {
        const PrimSpec *src_ps{nullptr};
        std::string _err;
        if (layer.find_primspec_at(spPath, &src_ps, &_err)) {
          if (src_ps) {
            std::string key = spPath.prim_part();
            if (!visited.count(key)) {
              visited.insert(key);
              DCOUT("Applying implied specialize from " << spPath.prim_part());
              if (!detail::InheritPrimSpecImpl(primspec,
                                               const_cast<PrimSpec &>(*src_ps),
                                               /* consume_src */ false, warn,
                                               err)) {
                visited.erase(key);
                return false;
              }
              visited.erase(key);
            }
          }
        }
        // If not found, that's OK -- the class may not exist in this layer stack
      }
    }
    primspec.metas().specializePaths.reset();
  }

  return true;
}

bool CompositeSpecializes(const Layer &in_layer, Layer *composited_layer,
                          std::string *warn, std::string *err) {
  if (!composited_layer) {
    return false;
  }

  Layer dst = in_layer;

  PathVisitedSet visited;  // Cycle detection set

  for (auto &item : dst.primspecs()) {
    if (!CompositeSpecializesRec(/* depth */ 0, dst, item.second, warn, err, visited)) {
      PUSH_ERROR_AND_RETURN("Composite `specializes` failed.");
    }
  }

  (*composited_layer) = std::move(dst);

  DCOUT("Composite `specializes` ok.");
  return true;
}

namespace detail {

static Property ComposeStrongerPropertyOverWeaker(const Property &stronger,
                                                  const Property &weaker) {
  Property composed = stronger;
  if (!composed.is_attribute() || !weaker.is_attribute()) {
    return composed;
  }

  Attribute &composed_attr = composed.attribute();
  const Attribute &weaker_attr = weaker.get_attribute();

  // Attribute metadata composes per field: the stronger opinion wins where it
  // authored a field, the weaker fills the rest. e.g. a variant (or override)
  // that supplies a new VALUE but not metadata must keep the weaker layer's
  // `customData` (Pixar Kitchen_set: a PlateB variant overrides displayColor's
  // value but inherits the asset's `customData = { Maya = { generated = 1 } }`).
  composed_attr.metas().merge_from(weaker_attr.metas(),
                                   /*override_existing=*/false);

  if (!composed_attr.has_connections() && weaker_attr.has_connections()) {
    composed_attr.set_connections(weaker_attr.connections());
  }

  if (!composed_attr.has_value() && !composed_attr.has_timesamples() &&
      !composed_attr.is_blocked() &&
      (weaker_attr.has_value() || weaker_attr.has_timesamples() ||
       weaker_attr.is_blocked())) {
    primvar::PrimVar weaker_var = weaker_attr.get_var();
    composed_attr.set_var(weaker_var);
  }

  ComposeWeakerPathExpression(&composed_attr, weaker_attr);

  return composed;
}

static bool OverridePrimSpecRec(uint32_t depth, PrimSpec &dst,
                                const PrimSpec &src, std::string *warn,
                                std::string *err) {
  (void)warn;

  if (depth > 4096) {
    PUSH_ERROR_AND_RETURN("PrimSpec tree too deep (max 4096).");
  }

  DCOUT("update_from");
  DCOUT(print_prim_metas(src.metas(), 1));
  // Override metadataum
  dst.metas().update_from(src.metas());
  DCOUT("update_from done");

  // AOUSD Core Spec 12.2.1 (specifier): the composed prim is DEFINED if ANY
  // opinion defines it. OverridePrimSpec merges a weaker `src` under a stronger
  // `dst`; when `dst` is a pure `over` and `src` is defining (def/class),
  // promote `dst` to the defining specifier. Without this a variant's
  // `def Mesh "LOD0"` merged under a locally-authored `over "LOD0"` (a deep
  // reference override, seen on UE-exported assets) stays `over` with no typeName --
  // the prim's definition silently vanishes on flatten. CombinePrimSpecRec
  // applies the same rule on the reference/sublayer path; this is its
  // counterpart for the variant-merge / append-reference path. The promotion
  // only ever turns `over` into `def`/`class` (never demotes), so it is correct
  // regardless of which side is the stronger opinion.
  if (dst.specifier() == Specifier::Over &&
      (src.specifier() == Specifier::Def ||
       src.specifier() == Specifier::Class)) {
    dst.specifier() = src.specifier();

    // The definition -- and therefore the anchor for its relative
    // reference/payload asset paths -- comes from this defining `src`. A pure
    // `over` carries the overriding layer's working path; keeping it would
    // mis-anchor arcs authored in the defining layer (e.g. a variant's
    // `../../../Materials/...` reference). Adopt the defining layer's
    // asset-resolution anchor so those arcs resolve correctly.
    if (!src.get_current_working_path().empty()) {
      dst.set_asset_resolution_state(src.get_current_working_path(),
                                     src.get_asset_search_paths());
    }
  }

  // AOUSD Core Spec 12.2.2 (typeName): take the typeName from the defining spec
  // when the stronger `over` has none.
  if (dst.typeName().empty() && !src.typeName().empty() &&
      (src.specifier() == Specifier::Def ||
       src.specifier() == Specifier::Class)) {
    dst.typeName() = src.typeName();
  }

  // Override properties with AOUSD Core Spec 12.2.4 (custom) handling:
  // The `custom` flag is true if ANY opinion in the stack says true.
  for (const auto &prop : src.props()) {
    if (dst.props().count(prop.first)) {
      // AOUSD Core Spec 12.2.3: Preserve uniform variability from weaker opinion
      Variability preserved_variability = Variability::Varying;
      if (dst.props().at(prop.first).is_attribute() && prop.second.is_attribute()) {
        preserved_variability = dst.props().at(prop.first).get_attribute().variability();
      }

      bool dst_custom = dst.props().at(prop.first).has_custom();
      dst.props()[prop.first] = ComposeStrongerPropertyOverWeaker(
          prop.second, dst.props().at(prop.first));
      if (dst_custom && !prop.second.has_custom()) {
        dst.props()[prop.first].set_custom(true);
      }

      // AOUSD Core Spec 12.2.3: If overriding property had uniform variability
      // and the override source is varying, preserve uniform.
      if (dst.props()[prop.first].is_attribute() &&
          preserved_variability == Variability::Uniform &&
          dst.props()[prop.first].attribute().variability() == Variability::Varying) {
        dst.props()[prop.first].attribute().variability() = Variability::Uniform;
      }
    } else {
      dst.props()[prop.first] = prop.second;
    }
  }

  // Override child primspecs.
  for (auto &child : dst.children()) {
    auto src_it = std::find_if(
        src.children().begin(), src.children().end(),
        [&child](const PrimSpec &ps) { return ps.name() == child.name(); });

    if (src_it != src.children().end()) {
      if (!OverridePrimSpecRec(depth + 1, child, (*src_it), warn, err)) {
        return false;
      }
    }
  }

  // Add child not exists in dst.
  for (auto &child : src.children()) {
    auto dst_it = std::find_if(
        dst.children().begin(), dst.children().end(),
        [&child](const PrimSpec &ps) { return ps.name() == child.name(); });

    if (dst_it == dst.children().end()) {
      dst.children().push_back(child);
    }
  }

  return true;
}

//
// TODO: Support nested inherits?
//
// When `consume_src` is true, `src` may be moved from (callers pass a
// discardable prepared arc PrimSpec) — removes one full subtree deep copy
// per composed reference/payload arc.
static bool InheritPrimSpecImpl(PrimSpec &dst, PrimSpec &src, bool consume_src,
                                std::string *warn, std::string *err) {
  DCOUT("inherit begin\n");
  (void)warn;

  DCOUT("src = " << prim::print_primspec(src));

  // Create PrimSpec from `src`,
  // Then override it with `dst`
  PrimSpec ps = consume_src ? PrimSpec(std::move(src)) : PrimSpec(src);
  // NOTE: `src` must not be read below (may be moved from); `ps` holds its
  // content verbatim at this point.

  // Keep PrimSpec name from `dst`
  ps.name() = dst.name();

  // AOUSD Core Spec 12.2.2 (typeName): typeName is determined from the
  // "prim definition" -- only use typeName from defining specs (def/class),
  // not from `over` specs. Stronger defining opinion wins.
  if (!dst.typeName().empty()) {
    // dst has a typeName -- if dst is defining (def/class), it wins
    ps.typeName() = dst.typeName();
  } else if (dst.specifier() == Specifier::Over && !ps.typeName().empty()) {
    // dst is an over with no typeName: inherit from src (the definition)
    // ps.typeName() already has src's typeName from the copy
  }

  // AOUSD Core Spec 12.2.1 (specifier): Composed specifier resolution.
  // - If dst (stronger opinion) is `over`, the result depends on whether
  //   any defining spec exists. For now, keep dst specifier as the
  //   strongest opinion, which matches the simple case.
  // - If dst is `def` or `class`, it takes precedence.
  ps.specifier() = dst.specifier();

  // Override metadataum
  ps.metas().update_from(dst.metas());

  // Override properties with AOUSD Core Spec 12.2.4 (custom) handling:
  // The `custom` flag is true if ANY opinion in the stack says true.
  for (const auto &prop : dst.props()) {
    // Single lookup into ps.props(). The found-branch only mutates the entry in
    // place; the else-branch inserts a *new* key, so the held reference (taken
    // only in the found-branch) is never invalidated.
    auto ps_it = ps.props().find(prop.first);
    if (ps_it != ps.props().end()) {
      Property &ps_prop = ps_it->second;
      // AOUSD Core Spec 12.2.3: Preserve uniform variability from weaker (inherited) opinion
      Variability inherited_variability = Variability::Varying;
      if (ps_prop.is_attribute() && prop.second.is_attribute()) {
        inherited_variability = ps_prop.get_attribute().variability();
      }

      // AOUSD Core Spec 12.2.4: OR the custom flags before replacing
      bool src_custom = ps_prop.has_custom();
      ps_prop = ComposeStrongerPropertyOverWeaker(prop.second, ps_prop);
      if (src_custom && !prop.second.has_custom()) {
        ps_prop.set_custom(true);
      }

      // AOUSD Core Spec 12.2.3: If inherited property had uniform variability
      // and the overriding (stronger) is varying, preserve uniform.
      if (ps_prop.is_attribute() &&
          inherited_variability == Variability::Uniform &&
          ps_prop.attribute().variability() == Variability::Varying) {
        ps_prop.attribute().variability() = Variability::Uniform;
      }
    }
    else {
      // re-add
      ps.props()[prop.first] = prop.second;
    }
  }

  // Overide child primspecs.
  //
  // Build a name->index map of dst children once for O(1) lookup instead of an
  // O(n*m) std::find_if scan. This loop does not resize dst.children(), so
  // indices stay valid throughout. emplace() keeps the first occurrence, matching
  // find_if's first-match semantics for duplicate names.
  std::unordered_map<std::string, size_t> dst_child_index;
  dst_child_index.reserve(dst.children().size());
  for (size_t i = 0; i < dst.children().size(); i++) {
    dst_child_index.emplace(dst.children()[i].name(), i);
  }

  // Names present in `src`(=ps) before we append, so dst-only children can be
  // detected below.
  std::unordered_set<std::string> src_child_names;
  src_child_names.reserve(ps.children().size());
  for (const auto &c : ps.children()) {
    src_child_names.insert(c.name());
  }

  for (auto &child : ps.children()) {
    auto it = dst_child_index.find(child.name());

    if (it != dst_child_index.end()) {
      if (!OverridePrimSpecRec(1, child, dst.children()[it->second], warn, err)) {
        return false;
      }
    }
  }

  // Carry over dst-only children: a stronger (dst) opinion that introduces a
  // child NOT present in `src` must survive composition, exactly as the
  // property loop above re-adds dst-only properties. Without this, a
  // locally-authored child placed alongside a reference/inherit/specialize
  // (e.g. a `def "LOD2"` next to a referenced asset whose LODs come from a
  // variant) is silently dropped. Appended after the src children (which keeps
  // the referenced/base children first, then local additions).
  for (const auto &dchild : dst.children()) {
    if (src_child_names.count(dchild.name()) == 0) {
      ps.children().push_back(dchild);
    }
  }

  // Carry over dst's variantSet CONTENT (the `variantSet "x" = { ... }` blocks).
  // `ps` started as `src` (weaker) and the loops above merged dst's stronger
  // metadata / properties / children, but variant content lives in variantSets()
  // -- NOT in metas() or children() -- so `dst = std::move(ps)` would drop it.
  // This mirrors CombinePrimSpecRec's variantSet merge. Without it, applying a
  // payload (or inherit/specialize) onto a prim that already carries variant
  // content silently loses that content: e.g. ALab components whose `/root`
  // gets a `payload` (the surfacing look-binding) composed beside the modelling
  // reference's geo variant -- the variant's `over "GEO"` geometry vanished and
  // the whole component composed to zero meshes. dst is the stronger opinion.
  for (const auto &dst_vs : dst.variantSets()) {
    auto pit = ps.variantSets().find(dst_vs.first);
    if (pit == ps.variantSets().end()) {
      ps.variantSets()[dst_vs.first] = dst_vs.second;  // dst-only: add whole set
    } else {
      VariantSetSpec &ps_vs = pit->second;
      for (const auto &v : dst_vs.second.variantSet) {
        auto vit = ps_vs.variantSet.find(v.first);
        if (vit == ps_vs.variantSet.end()) {
          ps_vs.variantSet[v.first] = v.second;  // dst-only variant
        } else if (!OverridePrimSpecRec(1, vit->second, v.second, warn, err)) {
          // both authored this variant: dst (stronger) overrides ps (weaker),
          // matching the dst-children override above.
          return false;
        }
      }
    }
  }

  // Preserve dst's asset-resolution anchor. The result prim sits at dst's
  // namespace location, so any relative reference/payload asset paths that dst
  // authored (merged into `ps` by the update_from() above) must keep anchoring
  // at dst's directory -- NOT the base class's. `ps = src` adopted src's working
  // path, and update_from() restores only metadata, not the resolution state, so
  // without this a prim that `inherits`/`specializes` an out-of-directory class
  // (e.g. ALab components whose roots `specializes = </_root_type>`, where
  // _root_type is defined up at the shot level) would re-anchor its OWN, possibly
  // still-unresolved, references to the class's directory and silently lose all
  // of its geometry. Only override when dst contributed arcs (so a prim that
  // purely inherits a class's arcs still anchors them at the class's directory).
  //
  // BUT: when `src` itself still carries UNRESOLVED reference/payload arcs, those
  // arcs were authored in src's layer and must keep anchoring at SRC's directory
  // (the payloaded/referenced asset's own location) -- core USD semantics. This
  // happens for nested assembly chains: a sub-assembly definition `payload`s a
  // leaf component whose subLayers in turn `reference` `../../../fragment/geo/...`.
  // `ps = src` already holds src's anchor; overwriting it with dst's (the
  // consuming prim's, e.g. the sub-assembly's `base/definition` dir) would
  // re-anchor the leaf's geo reference one place too high -> "Asset not found".
  // dst's OWN consuming arc is being removed by the caller, so prefer src's anchor
  // whenever src has surviving arcs.
  const std::string dst_cwp = dst.get_current_working_path();
  const bool src_has_arcs =
      src.metas().references.has_value() || src.metas().payload.has_value();
  if (!dst_cwp.empty() && !src_has_arcs &&
      (dst.metas().references.has_value() || dst.metas().payload.has_value())) {
    ps.set_asset_resolution_state(dst_cwp, dst.get_asset_search_paths());
  }

  DCOUT("move");
  dst = std::move(ps);
  DCOUT("move done");

  return true;
}

}  // namespace detail

bool OverridePrimSpec(PrimSpec &dst, const PrimSpec &src, std::string *warn,
                      std::string *err) {
  // NOTE: composition merges (references/payloads/inherits) legitimately
  // override opinions from `def` or `class` source prims, not only from `over`.
  // Overriding their opinions onto `dst` is valid, so this is a debug note, not
  // an error — emitting an error here poisons `err` and fails the whole arc
  // (e.g. usd-wg OpenChessSet inherits from a `class`).
  if (src.specifier() != Specifier::Over) {
    DCOUT("OverridePrimSpec: src specifier is "
          << to_string(src.specifier()) << " (not `over`); proceeding.");
  }

  return detail::OverridePrimSpecRec(0, dst, src, warn, err);
}

bool InheritPrimSpec(PrimSpec &dst, const PrimSpec &src, std::string *warn,
                     std::string *err) {
  // Const source: the impl never mutates src when consume_src is false.
  return detail::InheritPrimSpecImpl(dst, const_cast<PrimSpec &>(src),
                                     /* consume_src */ false, warn, err);
}

bool InheritPrimSpec(PrimSpec &dst, PrimSpec &&src, std::string *warn,
                     std::string *err) {
  return detail::InheritPrimSpecImpl(dst, src, /* consume_src */ true, warn,
                                     err);
}


bool HasReferences(const Layer &layer, const bool force_check,
                   const ReferencesCompositionOptions options) {
  if (!force_check) {
    return layer.has_unresolved_references();
  }

  return layer.check_unresolved_references(options.max_depth);
}

bool HasPayload(const Layer &layer, const bool force_check,
                const PayloadCompositionOptions options) {
  if (!force_check) {
    return layer.has_unresolved_payload();
  }

  return layer.check_unresolved_payload(options.max_depth);
}

bool HasInherits(const Layer &layer) {
  return layer.check_unresolved_inherits();
}

bool HasVariants(const Layer &layer) {
  return layer.check_unresolved_variant();
}

bool ShouldDeferVariantComposition(const Layer &layer, bool references_enabled,
                                   bool payload_enabled) {
  if (references_enabled && layer.check_unresolved_references()) return true;
  if (payload_enabled && layer.check_unresolved_payload()) return true;
  return false;
}

namespace {

// Resolve one apiSchemas applied-schema list-op into its baked EXPLICIT form,
// then rewrite `authoredOps` to a single ResetToExplicit op (the USDA/USDC
// writers emit that without a `prepend`/`append` qualifier). Operates on the
// authored raw-string ops so unknown schemas resolve uniformly.
//
// Resolution follows SdfListOp semantics, which are keyed by qualifier CATEGORY,
// not the order ops happen to be stored in: a `delete` removes its targets from
// the final set regardless of where it sits relative to the `prepend`/`append`
// that introduced them. (The crate reader emits ops in a fixed order --
// add/append/delete/prepend -- so applying them sequentially would make a
// delete-before-prepend a spurious no-op.) An `explicit` (ResetToExplicit) op is
// absolute: pxr stores a list-op in EITHER explicit mode OR the
// prepend/append/add/delete/order set, never both.
void BakeAppliedSchemaListOp(APISchemas &schemas) {
  if (schemas.explicitlyEmpty) {
    // Authored as `apiSchemas = None`; pxrUSD keeps it verbatim on flatten.
    return;
  }
  if (schemas.authoredOps.empty()) {
    // Resolved (names/listOpQual) view with no authored ops: just force the
    // qualifier explicit so the writer drops any `prepend`/`append`.
    schemas.listOpQual = ListEditQual::ResetToExplicit;
    return;
  }

  using Item = std::pair<std::string, std::string>;  // (schemaName, instance)

  bool has_explicit = false;
  std::vector<Item> explicit_items, prepended, appended;
  std::set<Item> deleted;
  for (const auto &op : schemas.authoredOps) {
    switch (op.first) {
      case ListEditQual::ResetToExplicit:
        has_explicit = true;
        explicit_items = op.second;
        break;
      case ListEditQual::Prepend:
        prepended.insert(prepended.end(), op.second.begin(), op.second.end());
        break;
      case ListEditQual::Append:
      case ListEditQual::Add:
        appended.insert(appended.end(), op.second.begin(), op.second.end());
        break;
      case ListEditQual::Delete:
        deleted.insert(op.second.begin(), op.second.end());
        break;
      default:
        // Order/Invalid: not part of the resolved applied-schema set.
        break;
    }
  }

  // Compose against an EMPTY base: a fully flattened layer has no weaker opinion
  // left for this prim's authored op to edit (the reference/sublayer that
  // contributed it was already consumed into `authoredOps`).
  std::vector<Item> resolved;
  std::set<Item> seen;
  auto add_unique = [&](const Item &it) {
    if (deleted.count(it)) return;
    if (seen.insert(it).second) resolved.push_back(it);
  };
  if (has_explicit) {
    for (const auto &it : explicit_items) add_unique(it);
  } else {
    for (const auto &it : prepended) add_unique(it);
    for (const auto &it : appended) add_unique(it);
  }

  schemas.authoredOps.clear();
  schemas.authoredOps.push_back({ListEditQual::ResetToExplicit, resolved});
  schemas.listOpQual = ListEditQual::ResetToExplicit;
}

void FlattenAppliedSchemasRec(PrimSpec &ps) {
  if (ps.metas().has_apiSchemas()) {
    APISchemas schemas = ps.metas().get_apiSchemas();
    BakeAppliedSchemaListOp(schemas);
    ps.metas().set_apiSchemas(schemas);
  }
  for (auto &child : ps.children()) {
    FlattenAppliedSchemasRec(child);
  }
}

}  // namespace

void FlattenAppliedSchemas(Layer &layer) {
  for (auto &kv : layer.primspecs()) {
    FlattenAppliedSchemasRec(kv.second);
  }
}

void SetNormalizeAssetPathOnFlatten(bool enable) {
  g_normalize_asset_path_on_flatten = enable;
}
bool GetNormalizeAssetPathOnFlatten() {
  return g_normalize_asset_path_on_flatten;
}

bool HasOver(const Layer &layer) { return layer.check_over_primspec(); }

bool HasSpecializes(const Layer &layer) {
  return layer.check_unresolved_specializes();
}

namespace {

// AOUSD Core Spec 10.3.2.5: Collect variant selection opinions from a PrimSpec tree.
// Returns a map of (prim_path -> VariantSelectionMap) for all prims that have
// variant selections authored.
void CollectVariantSelectionOpinionsRec(
    const std::string &path_prefix,
    const PrimSpec &ps,
    std::map<std::string, std::vector<VariantSelectionMap>> &opinions) {
  std::string prim_path = path_prefix + "/" + ps.name();

  if (ps.metas().variants) {
    opinions[prim_path].push_back(ps.metas().variants.value());
  }

  for (const auto &child : ps.children()) {
    CollectVariantSelectionOpinionsRec(prim_path, child, opinions);
  }
}

void CollectVariantSelectionOpinions(
    const Layer &layer,
    std::map<std::string, std::vector<VariantSelectionMap>> &opinions) {
  for (const auto &item : layer.primspecs()) {
    CollectVariantSelectionOpinionsRec("", item.second, opinions);
  }
}

// AOUSD Core Spec 10.3.2.5: Compute final variant selections from collected opinions.
// Strongest opinion (first in vector) wins per variant set name.
VariantSelectionMap ComputeVariantSelections(
    const std::vector<VariantSelectionMap> &opinions) {
  VariantSelectionMap result;

  // Iterate from weakest to strongest so strongest overwrites
  for (auto it = opinions.rbegin(); it != opinions.rend(); ++it) {
    for (const auto &sel : *it) {
      result[sel.first] = sel.second;
    }
  }

  return result;
}

// Apply pre-computed variant selections to a PrimSpec tree.
bool ApplyDeferredVariantSelectionsRec(
    uint32_t depth,
    const std::string &path_prefix,
    PrimSpec &primspec,
    const std::map<std::string, VariantSelectionMap> &resolved_selections,
    std::string *warn, std::string *err) {
  if (depth > kMaxCompositionDepth) {
    if (err) { *err += "Too deep in ApplyDeferredVariantSelectionsRec.\n"; }
    return false;
  }

  std::string prim_path = path_prefix + "/" + primspec.name();

  // Traverse children first.
  for (auto &child : primspec.children()) {
    if (!ApplyDeferredVariantSelectionsRec(depth + 1, prim_path, child,
                                            resolved_selections, warn, err)) {
      return false;
    }
  }

  // Apply variant selection for this prim
  std::map<std::string, std::string> selection;
  auto sel_it = resolved_selections.find(prim_path);
  if (sel_it != resolved_selections.end()) {
    selection = sel_it->second;
  }

  PrimSpec dst;
  if (!VariantSelectPrimSpec(dst, std::move(primspec), selection, warn, err)) {
    return false;
  }

  primspec = std::move(dst);

  // Re-stamp the resolved selection: a deep arc chain may deliver the
  // POPULATED variantSet in a later fixpoint iteration (reference ->
  // payload -> reference, the Kitchen_set pattern). VariantSelectPrimSpec
  // consumes the selection metadata with the (possibly empty) set it just
  // applied; without re-stamping, the strongest selection is lost and the
  // late-arriving set falls back to its own weaker default. Leftover
  // selections are stripped by CompositeAllArcs once no unresolved arcs
  // remain.
  if (!selection.empty()) {
    VariantSelectionMap vsm;
    for (const auto &kv : selection) {
      vsm[kv.first] = kv.second;
    }
    primspec.metas().variants = vsm;
  }
  return true;
}

// AOUSD Core Spec 10.3.2.6: Apply relocates to a PrimSpec tree.
// Relocates rename prims in the namespace according to layerRelocates entries.
// Helper: find a mutable PrimSpec at a given absolute path in a PrimSpec tree.
// Returns nullptr if not found.
PrimSpec *FindMutablePrimSpec(PrimSpec &root, const std::string &root_path,
                              const std::string &target_path) {
  if (root_path == target_path) return &root;

  // Check if target is a descendant of root
  if (target_path.size() <= root_path.size()) return nullptr;
  if (target_path.substr(0, root_path.size()) != root_path) return nullptr;
  if (target_path[root_path.size()] != '/') return nullptr;

  // Walk down the tree
  std::string remaining = target_path.substr(root_path.size() + 1);
  PrimSpec *current = &root;
  while (!remaining.empty()) {
    auto slash = remaining.find('/');
    std::string segment = (slash == std::string::npos)
        ? remaining : remaining.substr(0, slash);

    PrimSpec *found = nullptr;
    for (auto &child : current->children()) {
      if (child.name() == segment) {
        found = &child;
        break;
      }
    }
    if (!found) return nullptr;
    current = found;

    if (slash == std::string::npos) break;
    remaining = remaining.substr(slash + 1);
  }
  return current;
}

// Helper: remove a child PrimSpec by name from a parent's children vector.
// Returns the detached PrimSpec, or nullopt if not found.
nonstd::optional<PrimSpec> DetachChild(PrimSpec &parent, const std::string &child_name) {
  auto &children = parent.children();
  for (auto it = children.begin(); it != children.end(); ++it) {
    if (it->name() == child_name) {
      PrimSpec detached = std::move(*it);
      children.erase(it);
      return detached;
    }
  }
  return nonstd::nullopt;
}

// Helper: get or create a PrimSpec at the given path under a root.
// Creates intermediate `over` prims as needed.
PrimSpec *GetOrCreatePrimSpec(PrimSpec &root, const std::string &root_path,
                              const std::string &target_path) {
  if (root_path == target_path) return &root;
  if (target_path.size() <= root_path.size()) return nullptr;
  if (target_path.substr(0, root_path.size()) != root_path) return nullptr;
  if (target_path[root_path.size()] != '/') return nullptr;

  std::string remaining = target_path.substr(root_path.size() + 1);
  PrimSpec *current = &root;
  while (!remaining.empty()) {
    auto slash = remaining.find('/');
    std::string segment = (slash == std::string::npos)
        ? remaining : remaining.substr(0, slash);

    PrimSpec *found = nullptr;
    for (auto &child : current->children()) {
      if (child.name() == segment) {
        found = &child;
        break;
      }
    }
    if (!found) {
      // Create intermediate over prim
      current->children().emplace_back(Specifier::Over, "", segment);
      found = &current->children().back();
    }
    current = found;

    if (slash == std::string::npos) break;
    remaining = remaining.substr(slash + 1);
  }
  return current;
}

// Helper: get parent path from a full path. "/Root/Child" -> "/Root"
std::string GetParentPath(const std::string &path) {
  auto last_slash = path.rfind('/');
  if (last_slash == std::string::npos || last_slash == 0) return "";
  return path.substr(0, last_slash);
}

// Helper: get element name from a full path. "/Root/Child" -> "Child"
std::string GetElementName(const std::string &path) {
  auto last_slash = path.rfind('/');
  if (last_slash == std::string::npos) return path;
  return path.substr(last_slash + 1);
}

// Remap all path references in a PrimSpec tree using a namespace mapping.
// Handles: relationships, connections, inherits, specializes, references, payloads.
void RemapPathsInPrimSpecTree(PrimSpec &ps, const NamespaceMapping &mapping) {
  std::vector<PrimSpec *> stack;
  stack.push_back(&ps);

  while (!stack.empty()) {
    PrimSpec *current = stack.back();
    stack.pop_back();

    // Remap relationship targets and attribute connections
    for (auto &prop : current->props()) {
      if (prop.second.is_relationship()) {
        Relationship &rel = prop.second.relationship();
        if (rel.is_path()) {
          rel.targetPath = mapping.Apply(rel.targetPath);
        } else if (rel.is_pathvector()) {
          for (auto &p : rel.targetPathVector) {
            p = mapping.Apply(p);
          }
        }
      } else if (prop.second.is_attribute_connection()) {
        Attribute &attr = prop.second.attribute();
        for (auto &conn : attr.connections()) {
          conn = mapping.Apply(conn);
        }
      }
    }

    // Remap composition arc target paths
    if (current->metas().inherits) {
      for (auto &op : current->metas().inherits.value()) {
        for (auto &p : op.second) {
          p = mapping.Apply(p);
        }
      }
    }
    if (current->metas().specializes) {
      for (auto &op : current->metas().specializes.value()) {
        for (auto &p : op.second) {
          p = mapping.Apply(p);
        }
      }
    }
    if (current->metas().references) {
      for (auto &op : current->metas().references.value()) {
        for (auto &ref : op.second) {
          if (ref.asset_path.GetAssetPath().empty() && ref.prim_path.is_valid()) {
            // Internal reference — remap the prim path
            ref.prim_path = mapping.Apply(ref.prim_path);
          }
        }
      }
    }
    if (current->metas().payload) {
      for (auto &op : current->metas().payload.value()) {
        for (auto &pl : op.second) {
          if (pl.asset_path.GetAssetPath().empty() && pl.prim_path.is_valid()) {
            pl.prim_path = mapping.Apply(pl.prim_path);
          }
        }
      }
    }

    PushChildAndVariantPrimSpecs(*current, &stack);
  }
}

}  // namespace

// AOUSD Core Spec 10.3.2.6: Apply layerRelocates to a composed layer.
// Relocates are applied after all other composition arcs.
//
// Implementation:
// 1. Validate relocate entries
// 2. For each relocate (src → tgt): detach the prim at src, reattach at tgt
// 3. Remap all path references throughout the tree
bool CompositeRelocates(const Layer &in_layer, Layer *composited_layer,
                        std::string *warn, std::string * /* err */) {
  if (!composited_layer) {
    return false;
  }

  const auto &relocates = in_layer.metas().layerRelocates;
  if (relocates.empty()) {
    *composited_layer = in_layer;
    return true;
  }

  // Validate
  auto validation_errors = ValidateRelocates(relocates);
  for (const auto &ve : validation_errors) {
    PushWarn("Relocates validation: " + ve + "\n");
  }

  Layer dst = in_layer;

  // Build namespace mapping for path remapping
  NamespaceMapping relocate_mapping;
  for (const auto &entry : relocates) {
    relocate_mapping.entries.push_back(entry);
  }

  // Phase 1: Move prims from source to target locations.
  // Process relocates sorted by path depth (deepest first) to avoid
  // invalidating parent paths when moving children.
  auto sorted_relocates = relocates;
  std::sort(sorted_relocates.begin(), sorted_relocates.end(),
            [](const std::pair<Path, Path> &a, const std::pair<Path, Path> &b) {
              return a.first.prim_part().size() > b.first.prim_part().size();
            });

  for (const auto &entry : sorted_relocates) {
    const std::string &src = entry.first.prim_part();
    const std::string &tgt = entry.second.prim_part();
    std::string src_parent = GetParentPath(src);
    std::string src_name = GetElementName(src);
    std::string tgt_parent = GetParentPath(tgt);
    std::string tgt_name = GetElementName(tgt);

    if (src_parent.empty() || tgt_parent.empty()) {
      // Root-level relocate: handle via primspecs map
      if (src_parent.empty() && tgt_parent.empty()) {
        // Root → Root rename (e.g., /Old → /New)
        auto it = dst.primspecs().find(src_name);
        if (it != dst.primspecs().end()) {
          PrimSpec ps = std::move(it->second);
          ps.name() = tgt_name;
          dst.primspecs().erase(it);
          dst.primspecs()[tgt_name] = std::move(ps);
          DCOUT("Relocate root: " << src_name << " -> " << tgt_name);
        }
      } else if (src_parent.empty()) {
        // Root → nested: detach from root map, attach under target parent
        auto it = dst.primspecs().find(src_name);
        if (it != dst.primspecs().end()) {
          PrimSpec ps = std::move(it->second);
          ps.name() = tgt_name;
          dst.primspecs().erase(it);

          // Find target parent (must be a root prim)
          std::string tgt_root = tgt_parent.substr(1); // strip leading /
          auto slash = tgt_root.find('/');
          std::string tgt_root_name = (slash == std::string::npos) ? tgt_root : tgt_root.substr(0, slash);
          auto tgt_root_it = dst.primspecs().find(tgt_root_name);
          if (tgt_root_it != dst.primspecs().end()) {
            PrimSpec *parent_ps = GetOrCreatePrimSpec(
                tgt_root_it->second, "/" + tgt_root_name, tgt_parent);
            if (parent_ps) {
              parent_ps->children().push_back(std::move(ps));
            }
          }
        }
      }
      continue;
    }

    // Non-root relocate: find source parent, detach child, find target parent, reattach
    // Find root prim for source
    std::string src_root_name = src.size() > 1 ? src.substr(1) : "";
    auto slash = src_root_name.find('/');
    if (slash != std::string::npos) src_root_name = src_root_name.substr(0, slash);

    auto src_root_it = dst.primspecs().find(src_root_name);
    if (src_root_it == dst.primspecs().end()) continue;

    // Find the source parent PrimSpec
    PrimSpec *src_parent_ps = nullptr;
    if (src_parent == "/" + src_root_name) {
      src_parent_ps = &src_root_it->second;
    } else {
      src_parent_ps = FindMutablePrimSpec(
          src_root_it->second, "/" + src_root_name, src_parent);
    }
    if (!src_parent_ps) continue;

    // Detach the child
    auto detached = DetachChild(*src_parent_ps, src_name);
    if (!detached) continue;

    detached->name() = tgt_name;

    // Find/create target parent
    std::string tgt_root_name = tgt.size() > 1 ? tgt.substr(1) : "";
    auto tslash = tgt_root_name.find('/');
    if (tslash != std::string::npos) tgt_root_name = tgt_root_name.substr(0, tslash);

    auto tgt_root_it = dst.primspecs().find(tgt_root_name);
    if (tgt_root_it == dst.primspecs().end()) continue;

    if (tgt_parent == "/" + tgt_root_name) {
      tgt_root_it->second.children().push_back(std::move(*detached));
    } else {
      PrimSpec *tgt_parent_ps = GetOrCreatePrimSpec(
          tgt_root_it->second, "/" + tgt_root_name, tgt_parent);
      if (tgt_parent_ps) {
        tgt_parent_ps->children().push_back(std::move(*detached));
      }
    }

    DCOUT("Relocate: " << src << " -> " << tgt);
  }

  // Phase 2: Remap all path references throughout the tree
  for (auto &item : dst.primspecs()) {
    RemapPathsInPrimSpecTree(item.second, relocate_mapping);
  }

  // Clear layerRelocates after processing
  dst.metas().layerRelocates.clear();

  *composited_layer = std::move(dst);
  DCOUT("Composite `relocates` ok.");
  return true;
}

// ---------------------------------------------------------------------------
// ListVariantSelectionMaps / ApplyVariantSelector
// ---------------------------------------------------------------------------

namespace {

void ListVariantSelectionMapsRec(const std::string &path_prefix,
                                  const PrimSpec &ps,
                                  VariantSelectorMap &m) {
  std::string prim_path = path_prefix + "/" + ps.name();

  if (ps.metas().variants || !ps.variantSets().empty()) {
    VariantSelector sel;
    if (ps.metas().variants) {
      sel.vsmap = ps.metas().variants.value();
      // Use the first variant set's selection as `selection`
      if (!sel.vsmap.empty()) {
        sel.selection = sel.vsmap.begin()->second;
      }
    }
    m[Path(prim_path, "")] = sel;
  }

  for (const auto &child : ps.children()) {
    ListVariantSelectionMapsRec(prim_path, child, m);
  }
}

bool ApplyVariantSelectorRec(uint32_t depth,
                              PrimSpec &primspec,
                              const VariantSelectorMap &vsmap,
                              const std::string &path_prefix,
                              std::string *warn, std::string *err) {
  if (depth > kMaxCompositionDepth) {
    if (err) { (*err) += "Too deep in ApplyVariantSelectorRec.\n"; }
    return false;
  }

  std::string prim_path = path_prefix + "/" + primspec.name();

  // Check if this prim has a variant selector entry
  auto it = vsmap.find(Path(prim_path, ""));
  if (it != vsmap.end()) {
    // Use the vsmap from the selector, or fall back to the PrimSpec's own variants
    const auto &selections = it->second.vsmap.empty()
        ? (primspec.metas().variants ? primspec.metas().variants.value()
                                     : VariantSelectionMap{})
        : it->second.vsmap;

    if (!selections.empty()) {
      PrimSpec dst;
      if (!VariantSelectPrimSpec(dst, std::move(primspec), selections, warn,
                                 err)) {
        return false;
      }
      primspec = std::move(dst);
    }
  }

  // Recurse into children
  for (auto &child : primspec.children()) {
    if (!ApplyVariantSelectorRec(depth + 1, child, vsmap, prim_path, warn, err)) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool ListVariantSelectionMaps(const Layer &layer, VariantSelectorMap &m) {
  m.clear();
  for (const auto &item : layer.primspecs()) {
    ListVariantSelectionMapsRec("", item.second, m);
  }
  return true;
}

bool ApplyVariantSelector(const Layer &layer, const VariantSelectorMap &vsmap,
                          Layer *dst, std::string *warn, std::string *err) {
  if (!dst) {
    if (err) { (*err) += "dst is nullptr.\n"; }
    return false;
  }

  *dst = layer;  // deep copy

  for (auto &item : dst->primspecs()) {
    if (!ApplyVariantSelectorRec(0, item.second, vsmap, "", warn, err)) {
      return false;
    }
  }

  return true;
}

bool ApplyVariantSelector(const Layer &layer, const std::string &variant_name,
                          Layer *dst, std::string *warn, std::string *err) {
  // Build a VariantSelectorMap from the layer's existing variant info,
  // overriding all selections with the given variant_name.
  VariantSelectorMap vsmap;
  ListVariantSelectionMaps(layer, vsmap);

  for (auto &item : vsmap) {
    // Override all variant set selections with the given name
    for (auto &sel : item.second.vsmap) {
      sel.second = variant_name;
    }
    item.second.selection = variant_name;
  }

  return ApplyVariantSelector(layer, vsmap, dst, warn, err);
}

// Remove prims (and their descendants) that have `active = false`.
// This is a post-composition pass — active is a composed metadatum.
static void RemoveInactivePrimsRec(std::vector<PrimSpec> &children) {
  // Iterative DFS over the PrimSpec tree (explicit heap worklist) so deeply
  // nested prim hierarchies cannot overflow the call stack. At each level we
  // erase inactive children, then enqueue the surviving children's child lists.
  // Pointers into a (already-erased, no-longer-mutated) vector stay valid because
  // the tree ownership is stable for the duration of the walk.
  std::vector<std::vector<PrimSpec> *> stack;
  stack.push_back(&children);

  while (!stack.empty()) {
    std::vector<PrimSpec> *level = stack.back();
    stack.pop_back();

    // Erase inactive children at this level first.
    level->erase(
        std::remove_if(level->begin(), level->end(),
                       [](const PrimSpec &ps) {
                         return ps.metas().has_active() &&
                                !ps.metas().get_active();
                       }),
        level->end());

    // Enqueue the surviving children's child lists.
    for (auto &child : *level) {
      stack.push_back(&child.children());
    }
  }
}

// AOUSD Core Spec 10.4: Composite all arcs in LIVERPS order.
// L(ocal/sublayers) is assumed already done before calling this function.
// Order: I(nherits) > V(ariants) > R(eferences) > P(ayloads) > S(pecializes)
//
// AOUSD Core Spec 10.3.2.5: Variant selection is deferred -- selections are
// computed using opinions from ALL arcs (I, R, P, S), not just local.
//
// Specializes (S) is applied last and is globally weaker than all other
// opinions per Spec 10.4.1.
bool CompositeAllArcs(AssetResolutionResolver &resolver, const Layer &layer,
                      Layer *composited_layer, std::string *warn,
                      std::string *err) {
  return CompositeAllArcs(resolver, layer, composited_layer, warn, err,
                          AllArcsCompositionOptions());
}

bool CompositeAllArcs(AssetResolutionResolver &resolver, const Layer &layer,
                      Layer *composited_layer, std::string *warn,
                      std::string *err,
                      const AllArcsCompositionOptions &options) {
  if (!composited_layer) {
    if (err) { *err = "composited_layer is nullptr."; }
    return false;
  }

  // Start with a copy of the input layer
  Layer working = layer;

  // Phase 1: Collect variant selection opinions from local layer (strongest)
  std::map<std::string, std::vector<VariantSelectionMap>> variant_opinions;
  CollectVariantSelectionOpinions(working, variant_opinions);

  // I: Inherits (strongest arc type after Local)
  // Process in-place to avoid deep copy.
  if (HasInherits(working)) {
    PathVisitedSet inh_visited;
    for (auto &item : working.primspecs()) {
      if (!CompositeInheritsRec(0, working, item.second, warn, err, inh_visited)) {
        PushError("Composite `inherits` failed.\n");
        return false;
      }
    }
    // Collect additional variant opinions from inherited content
    CollectVariantSelectionOpinions(working, variant_opinions);
  }

  // Skip V for now -- defer variant evaluation until after R and P

  // Shared visited set for cross-arc cycle detection between R and P phases.
  // This catches cycles where an asset is loaded as both a reference and payload.
  ArcVisitedSet arc_visited;

  // R: References
  if (HasReferences(working)) {
    Layer tmp;
    if (!CompositeReferencesImpl(resolver, working, &tmp, warn, err,
                                 options.references, arc_visited)) {
      return false;
    }
    working = std::move(tmp);
    // Collect variant opinions from referenced content
    CollectVariantSelectionOpinions(working, variant_opinions);
  }

  // P: Payloads
  if (HasPayload(working)) {
    Layer tmp;
    if (!CompositePayloadImpl(resolver, working, &tmp, warn, err,
                              options.payload, arc_visited)) {
      return false;
    }
    working = std::move(tmp);
    // Collect variant opinions from payload content
    CollectVariantSelectionOpinions(working, variant_opinions);
  }

  // Phase 2: Compute variant selections with full opinion stack (strongest wins)
  // and apply them. This implements AOUSD Core Spec 10.3.2.5 deferred evaluation.
  if (HasVariants(working)) {
    // Resolve per-prim selections
    std::map<std::string, VariantSelectionMap> resolved;
    for (const auto &item : variant_opinions) {
      resolved[item.first] = ComputeVariantSelections(item.second);
    }

    // Apply resolved selections
    for (auto &ps_item : working.primspecs()) {
      if (!ApplyDeferredVariantSelectionsRec(
              0, "", ps_item.second, resolved, warn, err)) {
        PushError("Deferred variant evaluation failed.\n");
        return false;
      }
    }
  }

  // S: Specializes (globally weaker per Spec 10.4.1)
  // Applied last so all other opinions take precedence.
  // Process in-place to avoid deep copy.
  if (HasSpecializes(working)) {
    PathVisitedSet sp_visited;
    for (auto &item : working.primspecs()) {
      if (!CompositeSpecializesRec(0, working, item.second, warn, err, sp_visited)) {
        PushError("Composite `specializes` failed.\n");
        return false;
      }
    }
  }

  // Fully resolved (no arcs left anywhere): strip the re-stamped variant
  // selections — they were kept only so later fixpoint iterations could
  // apply them to late-arriving variantSets.
  {
    const bool fully_resolved = !working.check_unresolved_references() &&
                                !working.check_unresolved_payload() &&
                                !working.check_unresolved_inherits() &&
                                !working.check_unresolved_specializes() &&
                                !working.check_unresolved_variant();
    if (fully_resolved) {
      std::function<void(PrimSpec&)> strip = [&](PrimSpec &ps) {
        if (ps.metas().variants && !ps.metas().variantSets &&
            ps.variantSets().empty()) {
          ps.metas().variants.reset();
        }
        for (auto &child : ps.children()) strip(child);
      };
      for (auto &ps_item : working.primspecs()) strip(ps_item.second);
    }
  }

  // Relocates (Spec 10.3.2.6): Applied after all other composition arcs.
  // Relocates rename prims in the composed namespace.
  // CompositeRelocates needs to copy because it modifies prim names/paths,
  // which would invalidate the primspecs map during iteration.
  if (!working.metas().layerRelocates.empty()) {
    Layer tmp;
    if (!CompositeRelocates(working, &tmp, warn, err)) {
      return false;
    }
    working = std::move(tmp);
  }

  // Post-composition: Remove prims with `active = false` and their descendants.
  // Active is a composed metadatum — it must be evaluated after all arcs.
  for (auto &ps_item : working.primspecs()) {
    if (ps_item.second.metas().has_active() &&
        !ps_item.second.metas().get_active()) {
      // Root-level prim is inactive — mark for removal
      // We can't erase during iteration of unordered_map, so handle below.
    }
    RemoveInactivePrimsRec(ps_item.second.children());
  }

  // Remove inactive root-level primspecs.
  {
    auto &ps_map = working.primspecs();
    for (auto it = ps_map.begin(); it != ps_map.end(); ) {
      if (it->second.metas().has_active() &&
          !it->second.metas().get_active()) {
        it = ps_map.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (auto &ps_item : working.primspecs()) {
    ResolveAssetAttributesInPrimSpecTree(
        ps_item.second, working.get_current_working_path());
  }

  *composited_layer = std::move(working);
  return true;
}

}  // namespace lightusd
