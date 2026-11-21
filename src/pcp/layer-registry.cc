// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// layer-registry.cc - implementation of the parse-once Layer cache.
//
#include "pcp/layer-registry.hh"

#include "composition.hh"  // CompositeSublayers
#include "io-util.hh"      // io::GetBaseDir
#include "core/security-policy.hh"
#include "lightusd.hh"  // LoadLayerFromMemory

namespace lightusd {
namespace pcp {

LayerRegistry::LayerRegistry()
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
    : _mu(new std::mutex())
#endif
{
}

const Layer *LayerRegistry::GetOrLoad(AssetResolutionResolver &resolver,
                                      const std::string &asset_path,
                                      const std::string &cwp,
                                      std::string *warn, std::string *err) {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  // Serialize the whole resolve+open+parse: this also protects the shared
  // resolver's working-path mutation against concurrent builds.
  std::lock_guard<std::mutex> lock(*_mu);
#endif

  // Honor the per-prim current-working-path for relative resolution.
  // RAII restore so the shared resolver's working-path is put back on every exit
  // path, not just the linear fall-through.
  const std::string old_cwp = resolver.current_working_path();
  struct CwpRestore {
    AssetResolutionResolver &r;
    const std::string &old;
    bool armed{false};
    ~CwpRestore() {
      if (armed && !old.empty()) {
        r.set_current_working_path(old);
      }
    }
  } cwp_restore{resolver, old_cwp};
  if (!cwp.empty()) {
    resolver.set_current_working_path(cwp);
    cwp_restore.armed = true;
  }

  std::string resolved_path = resolver.resolve(asset_path);

  const Layer *result = nullptr;
  if (!resolved_path.empty()) {
    auto it = _by_resolved.find(resolved_path);
    if (it != _by_resolved.end()) {
      // Cache hit -- no re-parse.
      result = it->second.get();
    } else {
      Asset asset;
      if (resolver.open_asset(resolved_path, asset_path, &asset, warn, err)) {
        if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
          if (err) {
            (*err) += "Resolved asset exceeds max read bytes: " + asset_path +
                      "\n";
          }
        } else {
          Layer layer;
          // Pass the RESOLVED (anchored) path so the cached layer's prims are
          // stamped with the resolved directory as their current-working-path,
          // preserving parent-directory context for nested relative arcs.
          if (LoadLayerFromMemory(asset.data(), asset.size(), resolved_path,
                                  &layer, warn, err)) {
            // If the referenced/payload asset aggregates its prims through its
            // OWN subLayers (common in asset-centric scenes like ALab, where an
            // entity file just sublayers department layers), compose them now —
            // otherwise the referenced layer's content would be empty. Mirrors
            // composition::LoadAsset.
            if (!layer.metas().subLayers.empty()) {
              const std::string base_dir = io::GetBaseDir(resolved_path);
              layer.set_asset_resolution_state(base_dir, resolver.search_paths(),
                                               resolver.get_userdata());
              Layer composited;
              SublayersCompositionOptions subopts;
              subopts.allow_parent_relative_paths = true;
              std::string sw, se;
              if (CompositeSublayers(resolver, layer, &composited, &sw, &se,
                                     subopts)) {
                layer = std::move(composited);
              }
              if (warn && !sw.empty()) (*warn) += sw;
            }
            auto sp = std::make_shared<Layer>(std::move(layer));
            result = sp.get();
            _by_resolved.emplace(resolved_path, std::move(sp));
            _parse_count++;
          }
        }
      }
    }
  }

  return result;  // cwp_restore restores the resolver's working-path here.
}

void LayerRegistry::Drop(const std::string &resolved_path) {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  std::lock_guard<std::mutex> lock(*_mu);
#endif
  _by_resolved.erase(resolved_path);
}

void LayerRegistry::Clear() {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  std::lock_guard<std::mutex> lock(*_mu);
#endif
  _by_resolved.clear();
}

size_t LayerRegistry::size() const {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  std::lock_guard<std::mutex> lock(*_mu);
#endif
  return _by_resolved.size();
}

}  // namespace pcp
}  // namespace lightusd
