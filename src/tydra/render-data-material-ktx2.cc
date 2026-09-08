// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Material and texture conversion routines split from render-data.cc
//
#include <cctype>
#include <chrono>
#include <numeric>
#include <set>

#include "common-utils.hh"
#include "common-types.hh"
#include "color-management.hh"
#include "enum-handlers.hh"
#include "../tiny-hashmap.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#if defined(LIGHTUSD_WITH_TEXTOOLS)
// KTX2 reader for the keep-compressed texture path (RenderSceneConverterConfig::
// keep_compressed_textures). Pulls in texcomp.h too.
#include "texpipe.h"
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/zstd.h"  // ZSTD_decompress for supercompressionScheme 2
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#endif
#include "io-util.hh"
#include "linear-algebra.hh"
#include "pprinter.hh"
#include "math-util.inc"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "safe-arithmetic.hh"
#include "usdMtlx.hh"
#include "value-pprint.hh"
#include "logger.hh"
#include "materialx-to-json.hh"
#include "security-policy.hh"

//
#include "common-macros.inc"

//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"
#include "tydra/render-data-material-internal.hh"

namespace lightusd {
namespace tydra {

#if defined(LIGHTUSD_WITH_TEXTOOLS)
namespace {

#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
// KTX2 Zstd (supercompressionScheme 2) decompressor for tp_ktx2_read_zstd.
static size_t KTX2ZstdDecompress(void * /*user*/, uint8_t *dst, size_t dst_cap,
                                 const uint8_t *src, size_t src_size) {
  const size_t r = ZSTD_decompress(dst, dst_cap, src, src_size);
  return ZSTD_isError(r) ? 0u : r;
}
#endif

// Budget-capped allocator for the KTX2 reader: a supercompressed KTX2 inflates
// into a reader-owned buffer sized from the header, so a small crafted file may
// legally declare dimensions whose block payload is multiple GiB. Bound it by
// the same ceiling used for asset reads.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((malloc, alloc_size(2)))
#endif
static void *KTX2BudgetAlloc(void *user, size_t size) {
  const size_t cap = *static_cast<const size_t *>(user);
  if (size == 0 || size > cap) return nullptr;
  return std::malloc(size);
}
static void KTX2BudgetFree(void * /*user*/, void *ptr) { std::free(ptr); }

// Case-insensitive ".ktx2" suffix test.
static bool EndsWithKtx2(const std::string &s) {
  if (s.size() < 5) return false;
  std::string ext = s.substr(s.size() - 5);
  for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".ktx2";
}

// Map a parsed KTX2 codec (or the uni intermediate) to a TextureBlockFormat.
static TextureBlockFormat Ktx2ToBlockFormat(const tp_ktx2_image &k) {
  if (k.is_uni) return TextureBlockFormat::UNI;
  switch (k.codec) {
    case TP_CODEC_BC1: return TextureBlockFormat::BC1;
    case TP_CODEC_BC3: return TextureBlockFormat::BC3;
    case TP_CODEC_BC5: return TextureBlockFormat::BC5;
    case TP_CODEC_BC6H: return TextureBlockFormat::BC6H;
    case TP_CODEC_BC7: return TextureBlockFormat::BC7;
    case TP_CODEC_ETC2_RGB: return TextureBlockFormat::ETC2_RGB;
    case TP_CODEC_ETC2_RGBA: return TextureBlockFormat::ETC2_RGBA;
    case TP_CODEC_EAC_R11: return TextureBlockFormat::EAC_R11;
    case TP_CODEC_EAC_RG11: return TextureBlockFormat::EAC_RG11;
    case TP_CODEC_ASTC: return TextureBlockFormat::ASTC_4x4;
    default: return TextureBlockFormat::None;
  }
}

// Load the level-0 compressed block payload of a `.ktx2` asset without decoding.
// On success fills `texImage` (blockFormat / block dims / width / height /
// channels) and `out_bytes` (the raw blocks), and returns true. Returns false
// (leaving blockFormat == None) so the caller falls back to the normal decode.
bool LoadKTX2CompressedBlocksImpl(const AssetResolutionResolver &resolver,
                                     const value::AssetPath &assetPath,
                                     TextureImage *texImage,
                                     std::vector<uint8_t> *out_bytes,
                                     std::string *warn, std::string *err) {
  const std::string resolved = resolver.resolve(assetPath.GetAssetPath());
  if (resolved.empty()) {
    if (err) (*err) += "keep-compressed: asset did not resolve.\n";
    return false;
  }
  Asset asset;
  if (!resolver.open_asset(resolved, assetPath.GetAssetPath(), &asset, warn, err))
    return false;
  if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
    if (err) (*err) += "keep-compressed: asset exceeds max read bytes.\n";
    return false;
  }
  tp_ktx2_image k;
  // Handles uncompressed (scheme 0) and, where zstd is available, Zstd (scheme
  // 2) KTX2; the reader owns any decompressed buffer, released via
  // tp_ktx2_image_free below (with the same allocator).
  size_t alloc_cap = security_policy::GetMaxAssetReadBytes();
  const tir_allocator kalloc{&alloc_cap, &KTX2BudgetAlloc, &KTX2BudgetFree};
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  const tp_result kr = tp_ktx2_read_zstd(asset.data(), asset.size(), &kalloc,
                                         &KTX2ZstdDecompress, nullptr, &k);
#else
  const tp_result kr = tp_ktx2_read(asset.data(), asset.size(), &k);
#endif
  if (kr != TP_SUCCESS) {
    if (err) (*err) += "keep-compressed: tp_ktx2_read failed.\n";
    return false;
  }
  if (k.num_faces != 1 || k.num_layers > 1) {  // 2D non-array only
    tp_ktx2_image_free(&kalloc, &k);
    return false;
  }
  const TextureBlockFormat bf = Ktx2ToBlockFormat(k);
  if (bf == TextureBlockFormat::None) {
    tp_ktx2_image_free(&kalloc, &k);
    return false;
  }
  // Store every mip level, largest-first (level 0 .. level N-1), tightly packed.
  // A GPU consumer re-derives per-level sizes from the block geometry + level
  // dimensions (each level's byteLength was validated by the reader).
  size_t total = 0;
  for (int l = 0; l < k.num_levels; ++l) total += k.levels[l].size;
  out_bytes->clear();
  out_bytes->reserve(total);
  for (int l = 0; l < k.num_levels; ++l) {
    const tp_ktx2_level &lv = k.levels[l];
    out_bytes->insert(out_bytes->end(), lv.data, lv.data + lv.size);
  }
  tp_ktx2_image_free(&kalloc, &k);  // blocks copied into out_bytes
  texImage->blockFormat = bf;
  texImage->blockWidth = k.block_w;
  texImage->blockHeight = k.block_h;
  texImage->width = int(k.width);
  texImage->height = int(k.height);
  texImage->channels = 4;
  texImage->assetTexelComponentType = ComponentType::UInt8;
  texImage->texelComponentType = ComponentType::UInt8;
  return true;
}

}  // namespace

bool IsKTX2AssetPath(const std::string &path) { return EndsWithKtx2(path); }

bool LoadKTX2CompressedBlocks(const AssetResolutionResolver &resolver,
                              const value::AssetPath &assetPath,
                              TextureImage *texImage,
                              std::vector<uint8_t> *out_bytes,
                              std::string *warn, std::string *err) {
  return LoadKTX2CompressedBlocksImpl(resolver, assetPath, texImage, out_bytes,
                                      warn, err);
}
#endif  // LIGHTUSD_WITH_TEXTOOLS

}  // namespace tydra
}  // namespace lightusd
