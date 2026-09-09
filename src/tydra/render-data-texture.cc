// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Default image loading, colorspace inference, and UDIM atlas construction.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common-utils.hh"
#include "common-macros.inc"
#include "image-loader.hh"
#include "image-types.hh"
#include "io-util.hh"
#include "security-policy.hh"
#include "texture-util.hh"
#include "tiny-format.hh"
#include "tydra/render-data.hh"
#include "value-types.hh"
#include "../safe-arithmetic.hh"

namespace lightusd {
namespace tydra {

bool DefaultTextureImageLoaderFunction(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver, TextureImage *texImageOut,
    std::vector<uint8_t> *imageData, void *userdata, std::string *warn,
    std::string *err) {
  if (!texImageOut) {
    if (err) {
      (*err) = "`imageOut` argument is nullptr\n";
    }
    return false;
  }

  if (!imageData) {
    if (err) {
      (*err) = "`imageData` argument is nullptr\n";
    }
    return false;
  }

  // TODO: assetInfo
  (void)assetInfo;
  (void)userdata;
  (void)warn;

  std::string sanitized_path = utils::SanitizeAssetPath(
      assetPath.GetAssetPath(), assetResolver.get_allow_parent_relative_paths());
  if (sanitized_path.empty()) {
    if (err) {
      (*err) += fmt::format("Unsafe asset path: {}\n", assetPath.GetAssetPath());
    }
    return false;
  }

  std::string resolvedPath = assetResolver.resolve(sanitized_path);

  if (resolvedPath.empty()) {
    if (err) {
      (*err) += fmt::format("Failed to resolve asset path: {}\n",
                            assetPath.GetAssetPath());
    }
    return false;
  }

  Asset asset;
  bool ret = assetResolver.open_asset(resolvedPath, sanitized_path,
                                      &asset, warn, err);
  if (!ret) {
    if (err) {
      (*err) += fmt::format("Failed to open asset: {}", resolvedPath);
    }
    return false;
  }

  if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
    if (err) {
      (*err) += fmt::format("Resolved asset exceeds max bytes ({} > {}).",
                            asset.size(), security_policy::GetMaxAssetReadBytes());
    }
    return false;
  }

  DCOUT("Resolved asset path = " << resolvedPath);

  auto result = lightusd::image::LoadImageFromMemory(asset.data(), asset.size(),
                                                     resolvedPath);
  if (!result) {
    if (err) {
      (*err) += "Failed to load image file: " + result.error() + "\n";
    }
    return false;
  }

  TextureImage texImage;

  texImage.asset_identifier = resolvedPath;
  texImage.channels = result.value().image.channels;

  const auto &imgret = result.value();

  if (imgret.image.bpp == 8) {
    // assume uint8
    texImage.assetTexelComponentType = ComponentType::UInt8;
  } else if (imgret.image.bpp == 16) {
    if (imgret.image.format == Image::PixelFormat::UInt) {
      texImage.assetTexelComponentType = ComponentType::UInt16;
    } else if (imgret.image.format == Image::PixelFormat::Int) {
      texImage.assetTexelComponentType = ComponentType::Int16;
    } else if (imgret.image.format == Image::PixelFormat::Float) {
      texImage.assetTexelComponentType = ComponentType::Half;
    } else {
      if (err) {
        (*err) += "Invalid image.pixelformat: " + lightusd::to_string(imgret.image.format) + "\n";
      }
      return false;
    }

  } else if (imgret.image.bpp == 32) {
    if (imgret.image.format == Image::PixelFormat::UInt) {
      texImage.assetTexelComponentType = ComponentType::UInt32;
    } else if (imgret.image.format == Image::PixelFormat::Int) {
      texImage.assetTexelComponentType = ComponentType::Int32;
    } else if (imgret.image.format == Image::PixelFormat::Float) {
      texImage.assetTexelComponentType = ComponentType::Float;
    } else {
      if (err) {
        (*err) += "Invalid image.pixelformat: " + lightusd::to_string(imgret.image.format) + "\n";
      }
      return false;
    }
  } else {
    DCOUT("Unsupported bpp = " << result.value().image.bpp);
    if (err) {
      (*err) += "Unsupported bpp: " +
               std::to_string(result.value().image.bpp) + "\n";
    }
    return false;
  }

  texImage.channels = result.value().image.channels;
  texImage.width = result.value().image.width;
  texImage.height = result.value().image.height;

  // `imageData` receives the decoder output as-is, so the buffer's texel type
  // equals the asset's texel type (HDR/EXR = Float32, 16-bit PNG = UInt16,
  // ...). Without this, float buffers were tagged UInt8 and every consumer
  // read the raw float bytes as 8-bit texels (garbage for HDR envmaps).
  texImage.texelComponentType = texImage.assetTexelComponentType;

  (*texImageOut) = texImage;

  // raw image data
  (*imageData) = result.value().image.data;

  return true;
}

// NOTE: this used to be a ~30-branch `else if` chain on tok.str(). Same
// class of MSVC C1061 ("blocks nested too deeply") risk already hit once in
// crate-writer-values.cc and fixed there -- an else-if chain nests one level
// deeper per link, while a lookup table has no such growth. Every original
// branch was a pure string->enum mapping with no side effects, so a table is
// both safe and a cleaner fit than the standalone-if pattern used elsewhere.
bool InferColorSpace(const value::token &tok, ColorSpace *cty) {
  if (!cty) {
    return false;
  }

  static const std::unordered_map<std::string, ColorSpace> kColorSpaceMap = {
      {"raw", ColorSpace::Raw},
      {"Raw", ColorSpace::Raw},
      {"srgb", ColorSpace::sRGB},
      {"srgb_rec709_scene", ColorSpace::sRGB},
      {"sRGB", ColorSpace::sRGB},
      {"srgb_texture", ColorSpace::sRGB_Texture},  // MaterialX texture colorspace
      {"linear", ColorSpace::Lin_sRGB},  // guess linear_srgb
      {"lin_srgb", ColorSpace::Lin_sRGB},
      {"rec709", ColorSpace::Rec709},
      {"lin_rec709", ColorSpace::Lin_Rec709},  // MaterialX/OpenUSD linear Rec.709
      {"lin_rec709_scene", ColorSpace::Lin_Rec709},
      {"g22_rec709", ColorSpace::g22_Rec709},  // MaterialX/OpenUSD gamma 2.2 Rec.709
      {"g22_rec709_scene", ColorSpace::g22_Rec709},
      {"g18_rec709", ColorSpace::g18_Rec709},  // MaterialX/OpenUSD gamma 1.8 Rec.709
      {"g18_rec709_scene", ColorSpace::g18_Rec709},
      {"lin_rec2020", ColorSpace::Lin_Rec2020},  // Linear Rec.2020
      {"lin_rec2020_scene", ColorSpace::Lin_Rec2020},
      {"acescg", ColorSpace::Lin_ACEScg},  // Alternative ACES CG naming
      {"lin_ap1", ColorSpace::Lin_ACEScg},  // Linear AP1 (same as ACEScg)
      {"lin_ap1_scene", ColorSpace::Lin_ACEScg},
      {"aces2065-1", ColorSpace::ACES2065_1},  // ACES 2065-1
      {"lin_ap0_scene", ColorSpace::ACES2065_1},
      {"ocio", ColorSpace::OCIO},
      {"lin_displayp3", ColorSpace::Lin_DisplayP3},
      {"lin_p3d65_scene", ColorSpace::Lin_DisplayP3},
      {"srgb_displayp3", ColorSpace::sRGB_DisplayP3},
      {"srgb_p3d65_scene", ColorSpace::sRGB_DisplayP3},
      // seen in Apple's USDZ model (or OCIO?)
      {"ACES - ACEScg", ColorSpace::Lin_ACEScg},
      {"Input - Texture - sRGB - Display P3", ColorSpace::sRGB_DisplayP3},
      {"Input - Texture - sRGB - sRGB", ColorSpace::sRGB},
      {"custom", ColorSpace::Custom},
  };

  const auto it = kColorSpaceMap.find(tok.str());
  if (it == kColorSpaceMap.end()) {
    return false;
  }
  (*cty) = it->second;
  return true;
}

namespace {

// Decode a single (resolved) image asset into an 8-bit `Image`.
bool UDIMDecodeImageAsset(const std::string &assetPath,
                          const AssetResolutionResolver &assetResolver,
                          Image *out, std::string *warn, std::string *err) {
  std::vector<uint8_t> direct_data;
  if (io::FileExists(assetPath)) {
    const size_t max_bytes = security_policy::GetMaxAssetReadBytes();
    if (!io::ReadWholeFile(&direct_data, err, assetPath, max_bytes)) {
      if (err) (*err) += fmt::format("Failed to read asset: {}\n", assetPath);
      return false;
    }
    auto result = lightusd::image::LoadImageFromMemory(direct_data.data(),
                                                       direct_data.size(),
                                                       assetPath);
    if (!result) {
      if (err) (*err) += "Failed to load image file: " + result.error() + "\n";
      return false;
    }
    (*out) = result.value().image;
    return true;
  }

  std::string sanitized = utils::SanitizeAssetPath(
      assetPath, assetResolver.get_allow_parent_relative_paths());
  if (sanitized.empty()) {
    if (err) (*err) += fmt::format("Unsafe asset path: {}\n", assetPath);
    return false;
  }

  std::string resolved = assetResolver.resolve(sanitized);
  if (resolved.empty()) {
    if (err) (*err) += fmt::format("Failed to resolve asset path: {}\n", assetPath);
    return false;
  }

  Asset asset;
  if (!assetResolver.open_asset(resolved, sanitized, &asset, warn, err)) {
    if (err) (*err) += fmt::format("Failed to open asset: {}\n", resolved);
    return false;
  }

  if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
    if (err) {
      (*err) += fmt::format("Resolved asset exceeds max bytes ({} > {}).\n",
                            asset.size(),
                            security_policy::GetMaxAssetReadBytes());
    }
    return false;
  }

  auto result =
      lightusd::image::LoadImageFromMemory(asset.data(), asset.size(), resolved);
  if (!result) {
    if (err) (*err) += "Failed to load image file: " + result.error() + "\n";
    return false;
  }

  (*out) = result.value().image;
  return true;
}

// Expand `src` (1-4 channels, 8-bit or fp32) into a 4-channel RGBA8 `Image`.
bool UDIMToRGBA8(const Image &src, Image *dst) {
  if (src.bpp != 8 && src.bpp != 32) return false;
  if (src.channels < 1 || src.channels > 4) return false;

  const size_t npixels = size_t(src.width) * size_t(src.height);
  dst->width = src.width;
  dst->height = src.height;
  dst->channels = 4;
  dst->bpp = 8;
  dst->format = Image::PixelFormat::UInt;
  dst->colorspace = src.colorspace;
  dst->data.assign(npixels * 4, 0);

  const int sc = src.channels;
  for (size_t i = 0; i < npixels; i++) {
    uint8_t *d = dst->data.data() + i * 4;
    if (src.bpp == 32) {
      const float *s = reinterpret_cast<const float *>(src.data.data()) +
                       i * size_t(sc);
      auto q = [](float v) -> uint8_t {
        if (!(v > 0.0f)) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
      };
      if (sc == 1) {
        d[0] = d[1] = d[2] = q(s[0]);
        d[3] = 255;
      } else if (sc == 2) {
        d[0] = d[1] = d[2] = q(s[0]);
        d[3] = q(s[1]);
      } else if (sc == 3) {
        d[0] = q(s[0]); d[1] = q(s[1]); d[2] = q(s[2]);
        d[3] = 255;
      } else {
        d[0] = q(s[0]); d[1] = q(s[1]); d[2] = q(s[2]); d[3] = q(s[3]);
      }
      continue;
    }

    const uint8_t *s = src.data.data() + i * size_t(sc);
    if (sc == 1) {
      d[0] = d[1] = d[2] = s[0];
      d[3] = 255;
    } else if (sc == 2) {  // luminance + alpha
      d[0] = d[1] = d[2] = s[0];
      d[3] = s[1];
    } else if (sc == 3) {
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
      d[3] = 255;
    } else {  // 4
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
  }
  return true;
}

// Largest power-of-two <= n (n >= 1).
uint32_t UDIMFloorPow2(uint32_t n) {
  if (n < 1) return 1;
  uint32_t p = 1;
  while ((p << 1) <= n) p <<= 1;
  return p;
}

}  // namespace

bool ExpandUDIMTiles(const std::string &udimAssetPath,
                     const AssetResolutionResolver &assetResolver,
                     int max_tiles, std::vector<UDIMTile> *tilesOut,
                     std::string *warn, std::string *err) {
  (void)warn;
  if (!tilesOut) {
    if (err) (*err) = "`tilesOut` argument is nullptr\n";
    return false;
  }

  std::string prefix, suffix;
  if (!io::SplitUDIMPath(udimAssetPath, &prefix, &suffix)) {
    if (err) {
      (*err) += fmt::format("Not a UDIM asset path (no <UDIM> token): {}\n",
                            udimAssetPath);
    }
    return false;
  }

  // UDIM ids 1001..1100 (10x10 grid).
  constexpr uint32_t kUDIMStart = 1001;
  constexpr uint32_t kUDIMEnd = 1100;

  int cap = max_tiles;
  if (cap <= 0 || cap > 100) cap = 100;

  tilesOut->clear();
  for (uint32_t id = kUDIMStart; id <= kUDIMEnd; id++) {
    const std::string tilePath = prefix + std::to_string(id) + suffix;
    bool found = io::FileExists(tilePath);
    if (!found) {
      const std::string sanitized = utils::SanitizeAssetPath(
          tilePath, assetResolver.get_allow_parent_relative_paths());
      if (sanitized.empty()) {
        continue;
      }
      const std::string resolved = assetResolver.resolve(sanitized);
      if (resolved.empty()) {
        continue;
      }
      found = true;
    }
    if (!found) {
      continue;
    }

    UDIMTile tile;
    tile.udim_id = id;
    tile.u = (id - kUDIMStart) % 10;
    tile.v = (id - kUDIMStart) / 10;
    tile.asset_path = tilePath;
    tilesOut->push_back(tile);

    if (int(tilesOut->size()) >= cap) {
      break;
    }
  }

  if (tilesOut->empty()) {
    if (err) {
      (*err) += fmt::format("No UDIM tiles resolved for: {}\n", udimAssetPath);
    }
    return false;
  }

  return true;
}

bool BuildUDIMAtlas(const std::vector<UDIMTile> &tiles,
                    const AssetResolutionResolver &assetResolver,
                    int max_atlas_size, bool srgb, UDIMAtlas *atlasOut,
                    std::string *warn, std::string *err) {
  if (!atlasOut) {
    if (err) (*err) = "`atlasOut` argument is nullptr\n";
    return false;
  }
  if (tiles.empty()) {
    if (err) (*err) = "No UDIM tiles to combine\n";
    return false;
  }

  // Grid bounds from present tiles.
  uint32_t min_u = 9, max_u = 0, min_v = 9, max_v = 0;
  for (const auto &t : tiles) {
    min_u = (std::min)(min_u, t.u);
    max_u = (std::max)(max_u, t.u);
    min_v = (std::min)(min_v, t.v);
    max_v = (std::max)(max_v, t.v);
  }
  const uint32_t cols = max_u - min_u + 1;
  const uint32_t rows = max_v - min_v + 1;

  // Per-tile cell size derived from the max atlas longest edge.
  int atlas_cap = max_atlas_size > 0 ? max_atlas_size : 4096;
  uint32_t per_tile_max =
      uint32_t((std::max)(1, atlas_cap / int((std::max)(cols, rows))));
  const uint32_t per_tile = UDIMFloorPow2(per_tile_max);

  const uint32_t atlas_w = per_tile * cols;
  const uint32_t atlas_h = per_tile * rows;

  Image atlas;
  atlas.width = int(atlas_w);
  atlas.height = int(atlas_h);
  atlas.channels = 4;
  atlas.bpp = 8;
  atlas.format = Image::PixelFormat::UInt;
  atlas.data.assign(size_t(atlas_w) * size_t(atlas_h) * 4, 0);  // transparent

  const ResizeFilter filter =
      srgb ? ResizeFilter::SRGB : ResizeFilter::Linear;

  size_t placed = 0;
  for (const auto &t : tiles) {
    Image decoded;
    std::string tile_err;
    if (!UDIMDecodeImageAsset(t.asset_path, assetResolver, &decoded, warn,
                              &tile_err)) {
      if (warn) {
        (*warn) += fmt::format("Skip UDIM tile {} (`{}`): {}", t.udim_id,
                               t.asset_path, tile_err);
      }
      continue;
    }

    Image rgba;
    if (!UDIMToRGBA8(decoded, &rgba)) {
      if (warn) {
        (*warn) += fmt::format("Skip UDIM tile {} (`{}`): unsupported channels\n",
                               t.udim_id, t.asset_path);
      }
      continue;
    }

    Image cell;
    if (int(per_tile) == rgba.width && int(per_tile) == rgba.height) {
      cell = std::move(rgba);
    } else {
      std::string resize_err;
      if (!ResizeImage(rgba, int(per_tile), int(per_tile), &cell, filter,
                       &resize_err)) {
        if (warn) {
          (*warn) += fmt::format("Skip UDIM tile {} (`{}`): resize failed: {}\n",
                                 t.udim_id, t.asset_path, resize_err);
        }
        continue;
      }
    }

    // Cell position. UV v increases upward; image row 0 is the top, so the
    // bottom-most UV row (v == min_v) is placed at the bottom of the atlas.
    const uint32_t cell_col = t.u - min_u;
    const uint32_t cell_row_from_bottom = t.v - min_v;
    const uint32_t dst_x0 = cell_col * per_tile;
    const uint32_t dst_y0 = (rows - 1 - cell_row_from_bottom) * per_tile;

    for (uint32_t y = 0; y < per_tile; y++) {
      const uint8_t *srow = cell.data.data() + size_t(y) * per_tile * 4;
      uint8_t *drow =
          atlas.data.data() + (size_t(dst_y0 + y) * atlas_w + dst_x0) * 4;
      std::memcpy(drow, srow, size_t(per_tile) * 4);
    }
    placed++;
  }

  if (placed == 0) {
    if (err) (*err) += "Failed to place any UDIM tile into the atlas\n";
    return false;
  }

  atlasOut->image = std::move(atlas);
  atlasOut->cols = cols;
  atlasOut->rows = rows;
  atlasOut->min_u = min_u;
  atlasOut->min_v = min_v;
  atlasOut->uv_scale = {1.0f / float(cols), 1.0f / float(rows)};
  atlasOut->uv_offset = {-float(min_u) / float(cols),
                         -float(min_v) / float(rows)};

  return true;
}



}  // namespace tydra
}  // namespace lightusd
