// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "next/layer/asset-anchor.hh"
#include "next/resolver/asset-resolver.hh"
#include <cstdint>
#include <string>
namespace lightusd { namespace tydra { namespace next {
namespace {
uint64_t ImageCacheHash(const std::string& resolved,
                        ColorSpace color_space) noexcept {
  // FNV-1a is fast for these short, path-like keys and avoids allocating a
  // second copy of every resolved path just to use an unordered_map.
  uint64_t hash = 1469598103934665603ull;
  hash ^= static_cast<uint8_t>(color_space);
  hash *= 1099511628211ull;
  for (const unsigned char byte : resolved) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}
}  // namespace

std::string RenderSceneConverter::ResolveAssetPath(const std::string& file,
                                                   uint32_t asset_anchor_id) const {
  if (file.empty() || file[0] == '/' ||
      file.find("://") != std::string::npos) {
    return file;
  }

  // A relative asset path anchors to the LAYER THAT AUTHORED IT, not to the
  // stage's root layer. `asset_anchor_id` carries that layer's directory through
  // composition (see asset-anchor.hh); production look layers sit several dirs
  // below the root and reach their textures with `../..`, so anchoring at the
  // root yields a nonexistent path. Fall back to the stage base dir for prims
  // with no anchor (the root layer itself, and USDZ package entries).
  const std::string& anchor =
      ::lightusd::next::AssetAnchorPath(asset_anchor_id);
  const std::string& base = anchor.empty() ? config_.asset_base_dir : anchor;

  // A configured AssetResolver owns resolution (anchor/search paths + suffix
  // fallback). Anchor it at the AUTHORING layer's directory, same as above.
  if (config_.asset_resolver) {
    ::lightusd::next::ResolvedAsset resolved = config_.asset_resolver->Resolve(
        file, base.empty() ? std::string() : base + "/");
    if (resolved.exists && !resolved.resolved_path.empty()) {
      return resolved.resolved_path;
    }
    return file;
  }

  if (base.empty()) return file;

  // Normalize: the join commonly yields `look/binding/../../texture/x.png`, and
  // an un-collapsed path would also defeat the `resolved_path` image dedup below.
  return ::lightusd::next::AssetResolver::NormalizePath(
      ::lightusd::next::AssetResolver::JoinPath(base, file));
}

uint32_t RenderSceneConverter::AssetAnchorOf(const UsdPrim& prim) {
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  return spec ? spec->asset_anchor_id() : 0u;
}

// Dedup key for scene->images. Both image-dedup sites used a linear scan with
// full string compares over every image already in the scene -- O(n^2) with a
// long-path comparison per step (a 5000-image scene cost ~12.5M string
// compares). image_id_by_key_ makes it O(1); FindImageId/RememberImageId are
// the single place that maintains it.
std::string RenderSceneConverter::ImageKey(const std::string& resolved_path,
                                           ColorSpace cs) {
  // '\x1f' = unit separator, never valid in a path.
  return resolved_path + '\x1f' +
         std::to_string(static_cast<int>(cs));
}

int32_t RenderSceneConverter::FindImageId(const RenderScene* scene,
                                          const std::string& resolved_path,
                                          ColorSpace cs) {
  if (!scene) return -1;
  // Rebuild lazily if the caller mutated scene->images behind our back (or
  // this is a fresh scene), so the map can never report a stale index.
  if (image_id_by_key_.size() != scene->images.size()) {
    image_id_by_key_.clear();
    image_id_by_key_.reserve(scene->images.size());
    for (size_t i = 0; i < scene->images.size(); ++i) {
      image_id_by_key_.emplace(
          ImageKey(scene->images[i].resolved_path, scene->images[i].color_space),
          static_cast<int32_t>(i));
    }
  }
  auto it = image_id_by_key_.find(ImageKey(resolved_path, cs));
  return it == image_id_by_key_.end() ? -1 : it->second;
}


void RenderSceneConverter::RememberImageId(const RenderScene* scene,
                                           const std::string& resolved_path,
                                           ColorSpace cs, int32_t id) {
  (void)scene;
  image_id_by_key_.emplace(ImageKey(resolved_path, cs), id);
}

int32_t RenderSceneConverter::ResolveImageId(RenderScene* scene,
                                             const std::string& file,
                                             ColorSpace color_space,
                                             uint32_t asset_anchor_id) {
  if (!scene || file.empty()) return -1;
  const std::string resolved = ResolveAssetPath(file, asset_anchor_id);
  const ColorSpace csp =
      color_space == ColorSpace::Unknown ? ColorSpace::sRGB : color_space;
  const int32_t cached = FindCachedImageId(scene, resolved, csp);
  if (cached >= 0) return cached;
  TextureImage image;
  image.name = file;
  image.resolved_path = resolved;
  image.color_space = csp;
  const int32_t id = static_cast<int32_t>(scene->images.size());
  scene->images.push_back(std::move(image));
  RememberImageId(scene, resolved, csp, id);
  return id;
}

thread_local bool RenderSceneConverter::tl_material_local_scope_ = false;

int32_t RenderSceneConverter::FindCachedImageId(
    RenderScene* scene, const std::string& resolved, ColorSpace color_space) {
  if (!scene) return -1;
  if (tl_material_local_scope_) {
    // Parallel-worker scratch scene: no shared cache to consult or update --
    // just scan the local (small, freshly-empty-per-material) images list.
    for (size_t i = 0; i < scene->images.size(); ++i) {
      if (scene->images[i].resolved_path == resolved &&
          scene->images[i].color_space == color_space) {
        return static_cast<int32_t>(i);
      }
    }
    return -1;
  }
  if (image_cache_scene_ != scene) {
    image_id_cache_.clear();
    image_cache_scene_ = scene;
  }
  const uint64_t hash = ImageCacheHash(resolved, color_space);
  const auto candidates = image_id_cache_.equal_range(hash);
  for (auto it = candidates.first; it != candidates.second; ++it) {
    if (it->second >= 0 && static_cast<size_t>(it->second) < scene->images.size()) {
      const TextureImage& image = scene->images[static_cast<size_t>(it->second)];
      if (image.resolved_path == resolved && image.color_space == color_space) {
        return it->second;
      }
    }
  }

  // A caller may have populated the scene catalog before the converter sees an
  // image. Pay the compatibility scan once, then make subsequent references O(1).
  for (size_t i = 0; i < scene->images.size(); ++i) {
    if (scene->images[i].resolved_path == resolved &&
        scene->images[i].color_space == color_space) {
      const int32_t id = static_cast<int32_t>(i);
      image_id_cache_.emplace(hash, id);
      return id;
    }
  }
  return -1;
}

void RenderSceneConverter::RememberImageId(RenderScene* scene,
                                           const std::string& resolved,
                                           ColorSpace color_space,
                                           int32_t id) {
  if (!scene) return;
  if (tl_material_local_scope_) return;  // no shared cache to update
  if (image_cache_scene_ != scene) {
    image_id_cache_.clear();
    image_cache_scene_ = scene;
  }
  image_id_cache_.emplace(ImageCacheHash(resolved, color_space), id);
}

void RenderSceneConverter::ResetImageIdCache() {
  image_id_cache_.clear();
  image_cache_scene_ = nullptr;
}





}}}  // namespace lightusd::tydra::next
