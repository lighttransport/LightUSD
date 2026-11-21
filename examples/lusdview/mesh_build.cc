// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <thread>
#include <memory>

#include "mesh_build.hh"

#include "displacement_bake.hh"
#include "lighting_eval.hh"
#include "lighting_ies.hh"
#include "lightrt_mtlx_bridge.hh"
#include "texture_tools.hh"
#if defined(LUSDVIEW_TEXTURE_GPU)
#include "texture_gpu.hh"
#endif

#if defined(LUSDVIEW_WITH_TEXTOOLS)
extern "C" {
#include "texpipe.h"  // tp_ktx2_read[_zstd] for the kept-compressed passthrough
}
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
#include "external/zstd.h"  // ZSTD_decompress (KTX2 supercompressionScheme 2)
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "light3d/math.h"
#include "log.hh"
#include "skinning.hh"  // InbetweenSamples, CollectBlendShapeInbetweens
#include "tydra/render-data-shaders.hh"
#include "tydra/texture-util.hh"
#include "lightusd-value-math.hh"

namespace lusdview {

namespace tydra = lightusd::tydra;
using lvmath::value::matrix4d;

namespace {

constexpr int kInfluenceTexWidth = 1024;































size_t TextureBytes(const light3d::Image& img) {
  return static_cast<size_t>(std::max(img.width, 0)) *
         static_cast<size_t>(std::max(img.height, 0)) *
         static_cast<size_t>(std::max(img.channels, 0));
}

size_t CompressedTextureBytes(const DrawCompressedImageCPU& compressed) {
  size_t total = compressed.data.size();
  for (const DrawCompressedMipCPU& mip : compressed.mips)
    total += mip.data.size();
  return total;
}

size_t TexturePayloadBytes(const light3d::Image& image,
                           const std::vector<light3d::Image>& mips,
                           const DrawCompressedImageCPU& compressed) {
  size_t total = TextureBytes(image) + CompressedTextureBytes(compressed);
  for (const light3d::Image& mip : mips) total += TextureBytes(mip);
  return total;
}

size_t TextureBytes(const DrawTextureCPU& tex) {
  if (tex.isUdim && !tex.udimTiles.empty()) {
    size_t total = 0;
    for (const DrawUdimTileCPU& tile : tex.udimTiles) {
      total += TexturePayloadBytes(tile.image, tile.mipImages,
                                   tile.compressed);
    }
    return total + TexturePayloadBytes(tex.image, tex.mipImages,
                                       tex.compressed);
  }
  return TexturePayloadBytes(tex.image, tex.mipImages, tex.compressed);
}

bool ResizeDrawImage(light3d::Image* img, int dstW, int dstH, bool srgb,
                     std::string* err) {
  if (!img || dstW <= 0 || dstH <= 0 || img->width <= 0 || img->height <= 0) {
    return false;
  }
  if (img->width == dstW && img->height == dstH) return true;
  // Prefer the vendored tir resizer (sRGB-aware, premultiplied-alpha,
  // higher-quality filters); fall back to the Tydra stb-based resize when
  // built without textools or on failure.
  if (TexToolsAvailable() && TexToolsResizeRGBA8(img, dstW, dstH, srgb, err)) {
    return true;
  }
  lightusd::Image src;
  src.width = img->width;
  src.height = img->height;
  src.channels = img->channels;
  src.bpp = 8;
  src.format = lightusd::Image::PixelFormat::UInt;
  src.data = img->data;
  lightusd::Image dst;
  const auto filter =
      srgb ? tydra::ResizeFilter::SRGB : tydra::ResizeFilter::Linear;
  if (!tydra::ResizeImage(src, dstW, dstH, &dst, filter, err)) {
    return false;
  }
  img->width = dst.width;
  img->height = dst.height;
  img->channels = dst.channels;
  img->data = std::move(dst.data);
  return true;
}

void InitUdimLookup(DrawTextureCPU* tex) {
  tex->udimLayer.fill(-1);
  for (size_t i = 0; i < tex->udimTiles.size(); ++i) {
    const uint32_t udim = tex->udimTiles[i].udim;
    if (udim >= 1001 && udim <= 1100) {
      tex->udimLayer[udim - 1001] = static_cast<int>(i);
    }
  }
}

void NormalizeUdimTiles(DrawTextureCPU* tex, bool srgb, DrawScene* out) {
  if (!tex || !tex->isUdim || tex->udimTiles.empty()) return;
  int w = 0;
  int h = 0;
  for (const DrawUdimTileCPU& tile : tex->udimTiles) {
    w = std::max(w, tile.image.width);
    h = std::max(h, tile.image.height);
  }
  if (w <= 0 || h <= 0) return;
  // Resize every tile to the common (w,h); DROP any that fail -- the renderer
  // uploads udimTiles as a 2D array requiring all layers to be exactly
  // udimTileWidth/Height, so a leftover wrong-sized tile rendered as a
  // black/garbage layer instead of the intended "missing" sentinel. Mirrors the
  // --core loader (LoadCoreUdimTexture); the dropped tile's UDIM id stays
  // unmapped in the LUT rebuilt by InitUdimLookup below.
  std::vector<DrawUdimTileCPU> sized;
  sized.reserve(tex->udimTiles.size());
  for (DrawUdimTileCPU& tile : tex->udimTiles) {
    if (tile.image.width != w || tile.image.height != h) {
      std::string err;
      if (!ResizeDrawImage(&tile.image, w, h, srgb, &err)) {
        if (out) out->skipped.push_back("UDIM tile resize failed (dropped): " + err);
        continue;
      }
    }
    sized.push_back(std::move(tile));
  }
  tex->udimTiles = std::move(sized);
  if (tex->udimTiles.empty()) return;
  tex->udimTileWidth = w;
  tex->udimTileHeight = h;
  tex->image = tex->udimTiles.front().image;  // representative fallback.
  InitUdimLookup(tex);
}

#if !defined(LUSDVIEW_WITH_TEXTOOLS)
// Minimal fallback BC1/BC3 encoders, used only when lusdview is built without
// the vendored texcomp library.
uint16_t PackRGB565(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((uint16_t(r) >> 3) << 11) | ((uint16_t(g) >> 2) << 5) |
                  (uint16_t(b) >> 3));
}

void UnpackRGB565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  const uint8_t r5 = uint8_t((c >> 11) & 31);
  const uint8_t g6 = uint8_t((c >> 5) & 63);
  const uint8_t b5 = uint8_t(c & 31);
  *r = uint8_t((r5 << 3) | (r5 >> 2));
  *g = uint8_t((g6 << 2) | (g6 >> 4));
  *b = uint8_t((b5 << 3) | (b5 >> 2));
}

void EncodeBC1Block(const uint8_t rgba[16][4], uint8_t* out) {
  uint8_t minv[3] = {255, 255, 255};
  uint8_t maxv[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    for (int c = 0; c < 3; ++c) {
      minv[c] = std::min(minv[c], rgba[i][c]);
      maxv[c] = std::max(maxv[c], rgba[i][c]);
    }
  }
  uint16_t c0 = PackRGB565(maxv[0], maxv[1], maxv[2]);
  uint16_t c1 = PackRGB565(minv[0], minv[1], minv[2]);
  if (c0 < c1) std::swap(c0, c1);
  uint8_t pal[4][3]{};
  UnpackRGB565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
  UnpackRGB565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
  for (int c = 0; c < 3; ++c) {
    pal[2][c] = uint8_t((2 * int(pal[0][c]) + int(pal[1][c])) / 3);
    pal[3][c] = uint8_t((int(pal[0][c]) + 2 * int(pal[1][c])) / 3);
  }
  uint32_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    int bestErr = std::numeric_limits<int>::max();
    for (int p = 0; p < 4; ++p) {
      int err = 0;
      for (int c = 0; c < 3; ++c) {
        const int d = int(rgba[i][c]) - int(pal[p][c]);
        err += d * d;
      }
      if (err < bestErr) {
        bestErr = err;
        best = p;
      }
    }
    bits |= uint32_t(best) << (2 * i);
  }
  out[0] = uint8_t(c0 & 255);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1 & 255);
  out[3] = uint8_t(c1 >> 8);
  std::memcpy(out + 4, &bits, 4);
}

void EncodeBC3AlphaBlock(const uint8_t rgba[16][4], uint8_t* out) {
  uint8_t a0 = 0;
  uint8_t a1 = 255;
  for (int i = 0; i < 16; ++i) {
    a0 = std::max(a0, rgba[i][3]);
    a1 = std::min(a1, rgba[i][3]);
  }
  out[0] = a0;
  out[1] = a1;
  uint8_t pal[8]{};
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i <= 6; ++i) {
      pal[i + 1] = uint8_t(((7 - i) * int(a0) + i * int(a1)) / 7);
    }
  } else {
    for (int i = 1; i <= 4; ++i) {
      pal[i + 1] = uint8_t(((5 - i) * int(a0) + i * int(a1)) / 5);
    }
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    int bestErr = std::numeric_limits<int>::max();
    for (int p = 0; p < 8; ++p) {
      const int d = int(rgba[i][3]) - int(pal[p]);
      const int err = d * d;
      if (err < bestErr) {
        bestErr = err;
        best = p;
      }
    }
    bits |= uint64_t(best) << (3 * i);
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t((bits >> (8 * i)) & 255);
}
#endif  // !LUSDVIEW_WITH_TEXTOOLS

// Resolve a requested compression mode to a concrete block format the device
// can actually sample, given its capabilities. `opaque` only affects the BCn
// (BC1 vs BC3) auto choice; ASTC/ETC2/BC7 carry alpha regardless. Falls back
// gracefully (e.g. Astc on a BC-only desktop GPU -> BC7) and returns None when
// nothing is available so the caller keeps the texture uncompressed.
#if defined(LUSDVIEW_WITH_TEXTOOLS) || defined(LUSDVIEW_TEXTURE_GPU)
DrawCompressedFormat ChooseCompressedFormat(TextureCompressionMode mode,
                                            const TextureCompressCaps& caps,
                                            bool opaque, bool normal_map) {
  // A tangent-space normal map's X/Y are independent, so a BC1/BC3 endpoint
  // color line cross-contaminates them (visibly wrong lighting). Force a
  // full-channel format instead. BC5 would be ideal (two independent channels)
  // but the raster shaders don't reconstruct Z from a two-channel sample yet, so
  // BC7 (all four channels, high quality) is the safe choice; ASTC/ETC2 are also
  // full-channel. Never BC1/BC3 for a normal map.
  const DrawCompressedFormat bcn =
      normal_map ? DrawCompressedFormat::BC7
                 : (opaque ? DrawCompressedFormat::BC1 : DrawCompressedFormat::BC3);
  switch (mode) {
    case TextureCompressionMode::Off:
      return DrawCompressedFormat::None;
    case TextureCompressionMode::BCn:
      if (caps.bc) return bcn;
      if (caps.astc) return DrawCompressedFormat::ASTC_4x4;
      if (caps.etc2) return DrawCompressedFormat::ETC2_RGBA;
      return DrawCompressedFormat::None;
    case TextureCompressionMode::BC7:
      if (caps.bc) return DrawCompressedFormat::BC7;
      if (caps.astc) return DrawCompressedFormat::ASTC_4x4;
      if (caps.etc2) return DrawCompressedFormat::ETC2_RGBA;
      return DrawCompressedFormat::None;
    case TextureCompressionMode::Astc:
      if (caps.astc) return DrawCompressedFormat::ASTC_4x4;
      if (caps.bc) return DrawCompressedFormat::BC7;
      if (caps.etc2) return DrawCompressedFormat::ETC2_RGBA;
      return DrawCompressedFormat::None;
    case TextureCompressionMode::Etc2:
      if (caps.etc2) return DrawCompressedFormat::ETC2_RGBA;
      if (caps.bc) return DrawCompressedFormat::BC7;
      if (caps.astc) return DrawCompressedFormat::ASTC_4x4;
      return DrawCompressedFormat::None;
    case TextureCompressionMode::Auto:
      // Prefer the highest-quality format available on the platform.
      if (caps.bc) return DrawCompressedFormat::BC7;
      if (caps.astc) return DrawCompressedFormat::ASTC_4x4;
      if (caps.etc2) return DrawCompressedFormat::ETC2_RGBA;
      return DrawCompressedFormat::None;
  }
  return DrawCompressedFormat::None;
}
#endif

bool EncodeBCn(const light3d::Image& img, bool srgb,
               TextureCompressionMode mode, const TextureCompressCaps& caps,
               DrawCompressedImageCPU* out, bool normal_map,
               const bool* opaque_override = nullptr) {
  if (!out || img.width <= 0 || img.height <= 0 || img.channels != 4 ||
      img.data.empty()) {
    return false;
  }
  bool opaque = true;
  if (opaque_override) {
    // UDIM: one opacity decision for the whole tile set (see CompressTexture) --
    // per-tile choices mixed BC1 (8 B) and BC3 (16 B) blocks, which the 2D-array
    // upload cannot combine, silently falling back to uncompressed.
    opaque = *opaque_override;
  } else {
    for (size_t i = 3; i < img.data.size(); i += 4) {
      if (img.data[i] < 250) {
        opaque = false;
        break;
      }
    }
  }
#if defined(LUSDVIEW_WITH_TEXTOOLS)
  const DrawCompressedFormat format =
      ChooseCompressedFormat(mode, caps, opaque, normal_map);
  if (format == DrawCompressedFormat::None) return false;
  return TexToolsCompress(img, srgb, format, out);
#else
  (void)srgb;
  (void)mode;
  (void)caps;  // ASTC/ETC2/BC7 need the vendored texcomp encoder.
  // Without textools the only encoders are BC1/BC3, both of which corrupt a
  // normal map's X/Y. Leave normal maps uncompressed here.
  if (normal_map) return false;
  out->format = opaque ? DrawCompressedFormat::BC1 : DrawCompressedFormat::BC3;
  out->width = img.width;
  out->height = img.height;
  const int bw = (img.width + 3) / 4;
  const int bh = (img.height + 3) / 4;
  const int blockBytes = opaque ? 8 : 16;
  out->data.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh) *
                       static_cast<size_t>(blockBytes),
                   0);
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      uint8_t block[16][4]{};
      for (int y = 0; y < 4; ++y) {
        const int sy = std::min(by * 4 + y, img.height - 1);
        for (int x = 0; x < 4; ++x) {
          const int sx = std::min(bx * 4 + x, img.width - 1);
          const uint8_t* src =
              img.data.data() + (static_cast<size_t>(sy) * img.width + sx) * 4;
          std::memcpy(block[y * 4 + x], src, 4);
        }
      }
      uint8_t* dst = out->data.data() +
                     (static_cast<size_t>(by) * bw + bx) * blockBytes;
      if (opaque) {
        EncodeBC1Block(block, dst);
      } else {
        EncodeBC3AlphaBlock(block, dst);
        EncodeBC1Block(block, dst + 8);
      }
    }
  }
  return true;
#endif  // LUSDVIEW_WITH_TEXTOOLS
}

void CompressTexture(DrawTextureCPU* tex, TextureCompressionMode mode,
                     const TextureCompressCaps& caps, bool normal_map) {
  if (!tex) return;
  if (tex->isUdim) {
    // One opacity decision for the whole tile set, so every layer gets the SAME
    // block format (the 2D-array upload needs uniform format + layer size).
    bool set_opaque = true;
    for (const DrawUdimTileCPU& tile : tex->udimTiles) {
      const auto& d = tile.image.data;
      for (size_t i = 3; i < d.size(); i += 4) {
        if (d[i] < 250) { set_opaque = false; break; }
      }
      if (!set_opaque) break;
    }
    for (DrawUdimTileCPU& tile : tex->udimTiles) {
      EncodeBCn(tile.image, tex->srgb, mode, caps, &tile.compressed, normal_map,
                &set_opaque);
    }
    EncodeBCn(tex->image, tex->srgb, mode, caps, &tex->compressed, normal_map,
              &set_opaque);
    return;
  }
  EncodeBCn(tex->image, tex->srgb, mode, caps, &tex->compressed, normal_map);
}

}  // namespace  (the texture post-passes below are shared with the --core loader)

// GPU block-compression pass over already-decoded DrawScene textures. Split out
// of ApplyTextureRuntimeOptions so the `--core` loader — which does its own size
// cap / byte budget inside its texture decoder and must not re-run those passes —
// can still honor `--texture-compress`. Declared in mesh_build.hh, so these three
// live outside the anonymous namespace (external linkage).
void ClassifyTextureUsage(DrawScene* out);  // defined below (near FinalizeDrawTextures)

#if defined(LUSDVIEW_TEXTURE_GPU)
namespace {
std::unique_ptr<lusdview_texture_bench::Processor> gTextureGpu;
std::string gTextureGpuError;
double gTextureGpuMs = 0.0;
size_t gTextureGpuImages = 0;

bool EnsureGpu(const TextureRuntimeOptions& opt) {
  if (opt.gpuBackend == TextureGpuBackend::Off) return false;
  if (gTextureGpu) return true;
  lusdview_texture_bench::Backend backend = lusdview_texture_bench::Backend::Vulkan;
  if (opt.gpuBackend == TextureGpuBackend::CUDA)
    backend = lusdview_texture_bench::Backend::CUDA;
  else if (opt.gpuBackend == TextureGpuBackend::HIP)
    backend = lusdview_texture_bench::Backend::HIP;
  gTextureGpu = lusdview_texture_bench::CreateProcessor(
      backend, false, opt.gpuDevice, &gTextureGpuError);
  if (!gTextureGpu) {
    std::fprintf(stderr, "[lusdview] GPU texture processing unavailable: %s\n",
                 gTextureGpuError.c_str());
    return false;
  }
  std::fprintf(stderr, "[lusdview] GPU texture processing: %s %s\n",
               gTextureGpu->device().backend.c_str(),
               gTextureGpu->device().name.c_str());
  return true;
}

bool GpuCompressImage(const TextureRuntimeOptions& opt, const light3d::Image& image,
                     bool srgb, bool normalMap, DrawCompressedImageCPU* out) {
  if (!EnsureGpu(opt) || !out || image.width <= 0 || image.height <= 0 ||
      image.channels != 4 || image.data.empty()) return false;
  bool opaque = true;
  for (size_t i = 3; i < image.data.size(); i += 4) {
    if (image.data[i] < 250) { opaque = false; break; }
  }
  const DrawCompressedFormat chosen = ChooseCompressedFormat(
      opt.compression, opt.caps, opaque, normalMap);
  lusdview_texture_bench::CompressionFormat format;
  switch (chosen) {
    case DrawCompressedFormat::BC1: format = lusdview_texture_bench::CompressionFormat::BC1; break;
    case DrawCompressedFormat::BC3: format = lusdview_texture_bench::CompressionFormat::BC3; break;
    case DrawCompressedFormat::BC5: format = lusdview_texture_bench::CompressionFormat::BC5; break;
    case DrawCompressedFormat::ASTC_4x4: format = lusdview_texture_bench::CompressionFormat::ASTC; break;
    case DrawCompressedFormat::BC7:
    default: format = lusdview_texture_bench::CompressionFormat::BC7; break;
  }
  lusdview_texture_bench::TextureRequest request;
  request.rgba = image.data.data();
  request.rgbaBytes = image.data.size();
  request.width = request.dstWidth = static_cast<uint32_t>(image.width);
  request.height = request.dstHeight = static_cast<uint32_t>(image.height);
  request.srgb = srgb;
  request.format = format;
  request.resize = false;
  request.compress = true;
  lusdview_texture_bench::TextureResult result;
  if (!gTextureGpu->process(request, &result, &gTextureGpuError)) {
    std::fprintf(stderr, "[lusdview] GPU texture processing failed: %s\n",
                 gTextureGpuError.c_str());
    return false;
  }
  gTextureGpuMs += result.timing.totalMs;
  ++gTextureGpuImages;
  out->format = chosen;
  out->width = static_cast<int>(result.width);
  out->height = static_cast<int>(result.height);
  out->data = std::move(result.compressed);
  return !out->data.empty();
}

bool GpuCompressHDRData(const TextureRuntimeOptions& opt, const float* rgb,
                        size_t rgbCount, int width, int height,
                        DrawCompressedImageCPU* out) {
  if (!out || !EnsureGpu(opt) || !rgb || rgbCount == 0 || width <= 0 ||
      height <= 0 || (!opt.caps.bc6h && !opt.caps.bc) ||
      (opt.compression != TextureCompressionMode::Auto &&
       opt.compression != TextureCompressionMode::BCn))
    return false;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (rgbCount < pixels * 3u) return false;
  lusdview_texture_bench::TextureRequest request;
  // HIP's legacy processor validates the common RGBA pointer before entering
  // its BC6H branch. BC6H consumes rgbf exclusively; this non-null alias keeps
  // the request compatible without allocating or copying an LDR buffer.
  request.rgba = reinterpret_cast<const uint8_t*>(rgb);
  request.rgbaBytes = rgbCount * sizeof(float);
  request.rgbf = rgb;
  request.rgbfBytes = rgbCount * sizeof(float);
  request.width = request.dstWidth = static_cast<uint32_t>(width);
  request.height = request.dstHeight = static_cast<uint32_t>(height);
  request.srgb = false;
  request.format = lusdview_texture_bench::CompressionFormat::BC6H;
  request.resize = false;
  request.compress = true;
  lusdview_texture_bench::TextureResult result;
  if (!gTextureGpu->process(request, &result, &gTextureGpuError)) {
    std::fprintf(stderr, "[lusdview] HDR GPU texture processing failed: %s\n",
                 gTextureGpuError.c_str());
    return false;
  }
  gTextureGpuMs += result.timing.totalMs;
  ++gTextureGpuImages;
  out->format = DrawCompressedFormat::BC6H;
  out->width = static_cast<int>(result.width);
  out->height = static_cast<int>(result.height);
  out->data = std::move(result.compressed);
  return !out->data.empty();
}

bool GpuCompressHDR(const TextureRuntimeOptions& opt, const DrawTextureCPU& texture,
                    DrawCompressedImageCPU* out) {
  return texture.isHDR &&
         GpuCompressHDRData(opt, texture.hdrRGB.data(), texture.hdrRGB.size(),
                            texture.image.width, texture.image.height, out);
}

bool GpuResizeImage(const TextureRuntimeOptions& opt, light3d::Image* image,
                    int width, int height) {
  if (!image || !EnsureGpu(opt) || image->channels != 4 ||
      image->data.empty() || width <= 0 || height <= 0) return false;
  lusdview_texture_bench::TextureRequest request;
  request.rgba = image->data.data();
  request.rgbaBytes = image->data.size();
  request.width = static_cast<uint32_t>(image->width);
  request.height = static_cast<uint32_t>(image->height);
  request.dstWidth = static_cast<uint32_t>(width);
  request.dstHeight = static_cast<uint32_t>(height);
  request.srgb = true;
  request.resize = true;
  request.compress = false;
  request.downloadResized = true;
  lusdview_texture_bench::TextureResult result;
  if (!gTextureGpu->process(request, &result, &gTextureGpuError) ||
      result.resizedRGBA.empty()) return false;
  gTextureGpuMs += result.timing.totalMs;
  ++gTextureGpuImages;
  image->width = width;
  image->height = height;
  image->channels = 4;
  image->data = std::move(result.resizedRGBA);
  return true;
}

bool GpuCompressUdim(const TextureRuntimeOptions& opt, DrawTextureCPU* texture) {
  if (!texture || !texture->isUdim || texture->udimTiles.empty()) return false;
  DrawCompressedFormat expected = DrawCompressedFormat::None;
  std::vector<DrawCompressedImageCPU> encoded;
  encoded.reserve(texture->udimTiles.size());
  for (const DrawUdimTileCPU& tile : texture->udimTiles) {
    DrawCompressedImageCPU c;
    const bool ok = tile.isHDR
                       ? GpuCompressHDRData(opt, tile.hdrRGB.data(),
                                            tile.hdrRGB.size(), tile.image.width,
                                            tile.image.height, &c)
                       : GpuCompressImage(opt, tile.image, texture->srgb,
                                           texture->isNormalMap, &c);
    if (!ok) return false;
    if (expected == DrawCompressedFormat::None) expected = c.format;
    if (c.format != expected) return false;
    encoded.push_back(std::move(c));
  }
  DrawCompressedImageCPU baseEncoded;
  if (!texture->image.data.empty() || texture->isHDR) {
    const bool ok = texture->isHDR
                        ? GpuCompressHDR(opt, *texture, &baseEncoded)
                        : GpuCompressImage(opt, texture->image, texture->srgb,
                                            texture->isNormalMap, &baseEncoded);
    if (!ok ||
        baseEncoded.format != expected) return false;
  }
  for (size_t i = 0; i < encoded.size(); ++i)
    texture->udimTiles[i].compressed = std::move(encoded[i]);
  if (!baseEncoded.data.empty()) texture->compressed = std::move(baseEncoded);
  return true;
}
}  // namespace
#endif

void ShutdownTextureGpu() {
#if defined(LUSDVIEW_TEXTURE_GPU)
  gTextureGpu.reset();
  gTextureGpuError.clear();
  gTextureGpuImages = 0;
  gTextureGpuMs = 0.0;
#endif
}

void CompressDrawTexture(const TextureRuntimeOptions& opt,
                         DrawTextureCPU* texture) {
  if (!texture || opt.compression == TextureCompressionMode::Off ||
      texture->compressedFinal ||
      (texture->isPtex && opt.gpuBackend != TextureGpuBackend::Vulkan)) {
    return;
  }
  texture->requestedCompressed = true;
#if defined(LUSDVIEW_TEXTURE_GPU)
  if ((texture->isUdim && GpuCompressUdim(opt, texture)) ||
      (texture->isHDR && GpuCompressHDR(opt, *texture, &texture->compressed)) ||
      (!texture->isUdim && GpuCompressImage(opt, texture->image, texture->srgb,
                                             texture->isNormalMap,
                                             &texture->compressed))) {
    return;
  }
#endif
  CompressTexture(texture, opt.compression, opt.caps, texture->isNormalMap);
}

void ApplyTextureCompression(const TextureRuntimeOptions& opt, DrawScene* out) {
  if (!out || opt.compression == TextureCompressionMode::Off) return;
  // Classify usage first so a normal map is not compressed onto a BC1/BC3 color
  // line (which corrupts the independent X/Y components); it goes to a
  // full-channel format (BC7/ASTC/ETC2) instead. FinalizeDrawTextures re-runs
  // this (idempotent).
  ClassifyTextureUsage(out);
  // Block compression dominates a texture-heavy load: Scene A texture set spent
  // 362 s of a 395 s load here, encoding 507 textures (2028 MB -> 507 MB BC7)
  // one at a time. EncodeBCn writes only into its own texture's buffer and
  // keeps no shared or static state, so the encode is embarrassingly parallel.
  // Texture sizes vary by orders of magnitude, so hand out indices atomically
  // rather than splitting the range statically -- a static split leaves workers
  // that drew the 4K pages running long after the rest have finished.
  const size_t texCount = out->textures.size();
  auto encodeOne = [&opt](DrawTextureCPU& tex) {
    if (tex.compressedFinal) return;  // kept-compressed KTX2 -- already final
    if (tex.isPtex && opt.gpuBackend != TextureGpuBackend::Vulkan) return;
    tex.requestedCompressed = true;
#if defined(LUSDVIEW_TEXTURE_GPU)
    if ((tex.isUdim && GpuCompressUdim(opt, &tex)) ||
        (tex.isHDR && GpuCompressHDR(opt, tex, &tex.compressed)) ||
        (!tex.isUdim && GpuCompressImage(opt, tex.image, tex.srgb,
                                          tex.isNormalMap, &tex.compressed))) {
      return;
    }
#endif
    CompressTexture(&tex, opt.compression, opt.caps, tex.isNormalMap);
  };
  const unsigned hw = std::thread::hardware_concurrency();
  // The Vulkan processor owns one command buffer and descriptor set. Keep the
  // GPU path serialized; CPU encoding remains fully parallel when disabled.
  const size_t workers = opt.gpuBackend != TextureGpuBackend::Off
                             ? 1u
                             : std::min<size_t>(hw ? hw : 1u, texCount);
  if (workers <= 1) {
    for (DrawTextureCPU& tex : out->textures) encodeOne(tex);
  } else {
    std::atomic<size_t> nextTex{0};
    std::vector<std::thread> ts;
    ts.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
      ts.emplace_back([&]() {
        for (;;) {
          const size_t i = nextTex.fetch_add(1, std::memory_order_relaxed);
          if (i >= texCount) break;
          encodeOne(out->textures[i]);
        }
      });
    }
    for (std::thread& t : ts) t.join();
  }
  size_t n = 0, raw = 0, comp = 0;
  for (const DrawTextureCPU& tex : out->textures) {
    if (tex.compressedFinal) continue;
    if (!tex.compressed.data.empty()) {
      ++n;
      raw += tex.image.data.size();
      comp += tex.compressed.data.size();
    }
  }
  if (n) {
    std::fprintf(stderr,
                 "[lusdview] texture-compress: %zu texture(s) -> fmt %d, "
                 "%.1f MB -> %.1f MB (%.1fx)\n",
                 n, static_cast<int>(out->textures.empty()
                                         ? DrawCompressedFormat::None
                                         : out->textures[0].compressed.format),
                 double(raw) / (1024.0 * 1024.0),
                 double(comp) / (1024.0 * 1024.0),
                 comp ? double(raw) / double(comp) : 0.0);
  }
#if defined(LUSDVIEW_TEXTURE_GPU)
  if (gTextureGpuImages > 0) {
    std::fprintf(stderr, "[lusdview] GPU texture timing: %zu operation(s), "
                 "%.3f ms total, %.3f ms average\n",
                 gTextureGpuImages, gTextureGpuMs,
                 gTextureGpuMs / static_cast<double>(gTextureGpuImages));
    gTextureGpuImages = 0;
    gTextureGpuMs = 0.0;
  }
#endif
}

void ApplyTextureRuntimeOptions(const TextureRuntimeOptions& opt, DrawScene* out) {
  if (!out) return;
  if (opt.maxTextureSize > 0) {
    for (DrawTextureCPU& tex : out->textures) {
      auto resizeOne = [&](light3d::Image* img) {
        const int longest = std::max(img->width, img->height);
        if (longest <= opt.maxTextureSize || longest <= 0) return;
        const double scale = static_cast<double>(opt.maxTextureSize) /
                             static_cast<double>(longest);
        const int nw =
            std::max(1, static_cast<int>(std::floor(img->width * scale)));
        const int nh =
            std::max(1, static_cast<int>(std::floor(img->height * scale)));
        std::string err;
#if defined(LUSDVIEW_TEXTURE_GPU)
        if (GpuResizeImage(opt, img, nw, nh)) return;
#endif
        if (!ResizeDrawImage(img, nw, nh, tex.srgb, &err)) {
          out->skipped.push_back("texture resize failed: " + err);
        }
      };
      if (tex.isUdim) {
        for (DrawUdimTileCPU& tile : tex.udimTiles) resizeOne(&tile.image);
        NormalizeUdimTiles(&tex, tex.srgb, out);
      } else {
        resizeOne(&tex.image);
      }
    }
  }

  if (opt.textureBudgetMB > 0) {
    size_t total = 0;
    for (const DrawTextureCPU& tex : out->textures) total += TextureBytes(tex);
    const size_t budget =
        static_cast<size_t>(opt.textureBudgetMB) * 1024ull * 1024ull;
    if (budget > 0 && total > budget) {
      const double scale =
          std::sqrt(static_cast<double>(budget) / static_cast<double>(total));
      for (DrawTextureCPU& tex : out->textures) {
        auto resizeOne = [&](light3d::Image* img) {
          const int nw =
              std::max(1, static_cast<int>(std::floor(img->width * scale)));
          const int nh =
              std::max(1, static_cast<int>(std::floor(img->height * scale)));
          std::string err;
#if defined(LUSDVIEW_TEXTURE_GPU)
          if (GpuResizeImage(opt, img, nw, nh)) return;
#endif
          if (!ResizeDrawImage(img, nw, nh, tex.srgb, &err)) {
            out->skipped.push_back("texture budget resize failed: " + err);
          }
        };
        if (tex.isUdim) {
          for (DrawUdimTileCPU& tile : tex.udimTiles) resizeOne(&tile.image);
          NormalizeUdimTiles(&tex, tex.srgb, out);
        } else {
          resizeOne(&tex.image);
        }
      }
    }
  }
  // NOTE: texture compression is intentionally NOT applied here. It runs after
  // BuildDrawMaterials (in the callers), so ClassifyTextureUsage can see which
  // textures are normal maps and keep them off a BC1 color line.
}

// Classify texture usage from the built materials, then (with --texture-mips)
// build content-aware CPU mip chains and per-level compressed payloads. Must
// run after BuildDrawMaterials on every load path (one-shot and streaming) so
// both produce identical DrawScenes; texture usage (normal map / ORM packing /
// alpha-tested) is only known once materials exist.
// Tag each texture with how its bound materials use it (normal map, packed-ORM
// roughness channel, alpha-tested base color). Idempotent. Must run BEFORE
// ApplyTextureCompression so the block-format choice can see the usage -- a
// normal map must not be squeezed onto a BC1 color line.
void ClassifyTextureUsage(DrawScene* out) {
  if (!out) return;
  auto texAt = [&](int idx) -> DrawTextureCPU* {
    if (idx < 0 || static_cast<size_t>(idx) >= out->textures.size()) {
      return nullptr;
    }
    return &out->textures[static_cast<size_t>(idx)];
  };
  for (DrawMaterialCPU& m : out->materials) {
    // Semantic intent is authoritative for GPU view format. Some legacy UDIM
    // readers retain the file decoder's default sRGB tag even for scalar
    // MaterialX image nodes; sampling those arrays through an SRGB view darkens
    // weight/roughness/opacity data. Ordinary and UDIM paths must classify the
    // same way once the material connection is known.
    auto markRaw = [&](int idx) {
      if (DrawTextureCPU* t = texAt(idx)) t->srgb = false;
    };
    markRaw(m.metallicTex);
    markRaw(m.roughnessTex);
    markRaw(m.normalTex);
    markRaw(m.coatNormalTex);
    markRaw(m.opacityTex);
    markRaw(m.occlusionTex);
    markRaw(m.coatWeightTex);
    markRaw(m.coatRoughnessTex);
    markRaw(m.displacementTex);
    if (DrawTextureCPU* t = texAt(m.coatNormalTex)) {
      m.coatNormalSample.isUdim = t->isUdim;
    }
    if (DrawTextureCPU* t = texAt(m.displacementTex)) {
      m.displacementSample.isUdim = t->isUdim;
    }
    if (DrawTextureCPU* t = texAt(m.normalTex)) t->isNormalMap = true;
    if (DrawTextureCPU* t = texAt(m.coatNormalTex)) t->isNormalMap = true;
    if (DrawTextureCPU* t = texAt(m.roughnessTex)) {
      // Packed ORM map: the roughness channel minifies variance-aware (reduces
      // specular aliasing); other channels keep the filtered average.
      if (m.roughnessChannel >= 0 && m.roughnessChannel < 4) {
        t->channelOp[m.roughnessChannel] = 2;  // TP_CH_ROUGHNESS
      }
    }
    if (m.alphaMode == static_cast<int>(AlphaMode::Mask)) {
      if (DrawTextureCPU* t = texAt(m.baseColorTex)) {
        t->isAlphaTested = true;
        t->alphaCutoff = m.alphaCutoff;
      }
    }
  }
}

void FinalizeDrawTextures(const TextureRuntimeOptions& opt, DrawScene* out) {
  if (!out) return;
  ClassifyTextureUsage(out);
  if (!opt.generateMips || !TexToolsAvailable()) return;
  // Build the content-aware chain for one RGBA8 base image + its compressed
  // per-level payloads (same block format as the base level).
  auto buildOne = [&](const light3d::Image& base, const TexUsage& usage,
                      bool srgb, std::vector<light3d::Image>* mips,
                      DrawCompressedImageCPU* compressed,
                      bool requestedCompressed) -> bool {
    mips->clear();
    if (!TexToolsBuildMips(base, usage, mips)) return false;
    if (requestedCompressed && compressed &&
        compressed->format != DrawCompressedFormat::None) {
      compressed->mips.clear();
      compressed->mips.reserve(mips->size());
      bool ok = true;
      for (const light3d::Image& mip : *mips) {
        DrawCompressedImageCPU c;
#if defined(LUSDVIEW_TEXTURE_GPU)
        bool gpuOK = false;
        if (opt.gpuBackend != TextureGpuBackend::Off) {
          gpuOK = GpuCompressImage(opt, mip, srgb, usage.normalMap, &c) &&
                  c.format == compressed->format;
        }
#else
        bool gpuOK = false;
#endif
        if (!gpuOK && !TexToolsCompress(mip, srgb, compressed->format, &c)) {
          ok = false;
          break;
        }
        DrawCompressedMipCPU lvl;
        lvl.width = c.width;
        lvl.height = c.height;
        lvl.data = std::move(c.data);
        compressed->mips.push_back(std::move(lvl));
      }
      if (!ok) {
        compressed->mips.clear();  // base-only upload fallback
      } else {
        // The uploaders read compressed.mips on this path and never touch the
        // RGBA chain; keeping it retained ~21 MB of dead CPU RAM per 4K texture
        // for the DrawScene's lifetime. The format was cap-gated, so no
        // uncompressed fallback needs these levels.
        mips->clear();
        mips->shrink_to_fit();
      }
    }
    return true;
  };
  auto buildHdrOne = [&](DrawTextureCPU& tex) -> bool {
    if (!tex.isHDR || tex.hdrRGB.empty() || !tex.requestedCompressed ||
        tex.compressed.format != DrawCompressedFormat::BC6H) return false;
    int width = tex.image.width;
    int height = tex.image.height;
    std::vector<float> current = tex.hdrRGB;
    tex.compressed.mips.clear();
    while (width > 1 || height > 1) {
      const int nextWidth = std::max(1, width / 2);
      const int nextHeight = std::max(1, height / 2);
      lightusd::Image src;
      src.width = width;
      src.height = height;
      src.channels = 3;
      src.bpp = 32;
      src.format = lightusd::Image::PixelFormat::Float;
      src.data.resize(current.size() * sizeof(float));
      std::memcpy(src.data.data(), current.data(), src.data.size());
      lightusd::Image dst;
      std::string err;
      if (!tydra::ResizeImage(src, nextWidth, nextHeight, &dst,
                              tydra::ResizeFilter::Linear, &err) ||
          dst.data.size() != static_cast<size_t>(nextWidth) * nextHeight *
                                  3u * sizeof(float)) {
        tex.compressed.mips.clear();
        return false;
      }
      std::vector<float> next(dst.data.size() / sizeof(float));
      std::memcpy(next.data(), dst.data.data(), dst.data.size());
#if defined(LUSDVIEW_TEXTURE_GPU)
      DrawCompressedImageCPU level;
      if (!GpuCompressHDRData(opt, next.data(), next.size(), nextWidth,
                              nextHeight, &level) ||
          level.format != DrawCompressedFormat::BC6H) {
        tex.compressed.mips.clear();
        return false;
      }
      DrawCompressedMipCPU mip;
      mip.width = level.width;
      mip.height = level.height;
      mip.data = std::move(level.data);
      tex.compressed.mips.push_back(std::move(mip));
      current = std::move(next);
      width = nextWidth;
      height = nextHeight;
#else
      tex.compressed.mips.clear();
      return false;
#endif
    }
    return true;
  };
  for (DrawTextureCPU& tex : out->textures) {
    if (tex.isPtex || tex.deferredDecode) continue;
    // Kept-compressed KTX2 passthrough: the compressed payload is final and
    // `image` is empty, so there is nothing to build a mip chain from (the KTX2
    // level 0 is uploaded directly; multi-level KTX2 mips are a follow-up).
    if (tex.compressedFinal) continue;
    if (buildHdrOne(tex)) continue;
    TexUsage usage;
    usage.srgb = tex.srgb;
    usage.normalMap = tex.isNormalMap;
    usage.alphaTested = tex.isAlphaTested;
    usage.alphaCutoff = tex.alphaCutoff;
    usage.wrapS = tex.wrapS;
    usage.wrapT = tex.wrapT;
    for (int c = 0; c < 4; ++c) usage.channelOp[c] = tex.channelOp[c];
    if (tex.isUdim) {
      // Per-tile chains (tile dims were equalized by NormalizeUdimTiles, so
      // every tile carries the same level count for the GL/VK array upload).
      bool ok = true;
      for (DrawUdimTileCPU& tile : tex.udimTiles) {
        if (!buildOne(tile.image, usage, tex.srgb, &tile.mipImages,
                      &tile.compressed, tex.requestedCompressed)) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        for (DrawUdimTileCPU& tile : tex.udimTiles) {
          tile.mipImages.clear();
          tile.compressed.mips.clear();
        }
        out->skipped.push_back("UDIM mip build failed: " +
                               tex.assetIdentifier);
      }
    }
    if (!buildOne(tex.image, usage, tex.srgb, &tex.mipImages, &tex.compressed,
                  tex.requestedCompressed)) {
      out->skipped.push_back("texture mip build failed: " +
                             tex.assetIdentifier);
      continue;
    }
  }
}

#if defined(LUSDVIEW_WITH_TEXTOOLS)
namespace {

#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
size_t Ktx2ZstdDecompress(void* /*user*/, uint8_t* dst, size_t dst_cap,
                          const uint8_t* src, size_t src_size) {
  const size_t r = ZSTD_decompress(dst, dst_cap, src, src_size);
  return ZSTD_isError(r) ? 0u : r;
}
#endif

// Map a KTX2 block codec to the draw format (uni is handled via `isUni`).
DrawCompressedFormat Ktx2CodecToDraw(tp_codec c) {
  switch (c) {
    case TP_CODEC_BC1: return DrawCompressedFormat::BC1;
    case TP_CODEC_BC3: return DrawCompressedFormat::BC3;
    case TP_CODEC_BC5: return DrawCompressedFormat::BC5;
    case TP_CODEC_BC6H: return DrawCompressedFormat::BC6H;
    case TP_CODEC_BC7: return DrawCompressedFormat::BC7;
    case TP_CODEC_ETC2_RGB: return DrawCompressedFormat::ETC2_RGB;
    case TP_CODEC_ETC2_RGBA: return DrawCompressedFormat::ETC2_RGBA;
    case TP_CODEC_ASTC: return DrawCompressedFormat::ASTC_4x4;
    default: return DrawCompressedFormat::None;  // EAC / ASTC-HDR
  }
}

}  // namespace

bool BuildKeptCompressedFromKtx2(const uint8_t* data, size_t size,
                                 const TextureRuntimeOptions& opt,
                                 DrawTextureCPU* tex) {
  if (!data || !size || !tex) return false;
  // A size cap / byte budget needs decoded texels; fall back to the normal path.
  if (opt.maxTextureSize > 0 || opt.textureBudgetMB > 0) return false;

  tp_ktx2_image k;
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  if (!TP_OK(tp_ktx2_read_zstd(data, size, nullptr, &Ktx2ZstdDecompress, nullptr,
                               &k))) {
    return false;
  }
#else
  if (!TP_OK(tp_ktx2_read(data, size, &k))) return false;
#endif

  bool ok = false;
  const bool isUni = k.is_uni != 0;
  const DrawCompressedFormat srcFmt =
      isUni ? DrawCompressedFormat::ASTC_4x4 : Ktx2CodecToDraw(k.codec);
  if (k.num_faces == 1 && k.num_layers <= 1 && k.num_levels >= 1 &&
      (isUni || srcFmt != DrawCompressedFormat::None)) {
    const uint32_t w = k.levels[0].width;
    const uint32_t h = k.levels[0].height;
    DrawCompressedImageCPU comp;
    light3d::Image rgba;
    if (TexToolsAdaptCompressed(k.levels[0].data, k.levels[0].size, isUni, srcFmt,
                                w, h, opt.caps, &comp, &rgba)) {
      if (!comp.data.empty()) {
        // Carry the KTX2's precomputed mip chain to the same target format.
        for (int l = 1; l < k.num_levels; ++l) {
          std::vector<uint8_t> mip;
          if (!TexToolsAdaptCompressedLevel(k.levels[l].data, k.levels[l].size,
                                            isUni, srcFmt, comp.format,
                                            k.levels[l].width,
                                            k.levels[l].height, &mip)) {
            comp.mips.clear();
            break;
          }
          DrawCompressedMipCPU m;
          m.width = static_cast<int>(k.levels[l].width);
          m.height = static_cast<int>(k.levels[l].height);
          m.data = std::move(mip);
          comp.mips.push_back(std::move(m));
        }
        std::fprintf(stderr,
                     "[lusdview] kept-compressed KTX2: %s -> draw fmt %d, %ux%u, "
                     "%zu levels, %zu base bytes (no re-encode)\n",
                     isUni ? "uni" : "block", static_cast<int>(comp.format), w, h,
                     1 + comp.mips.size(), comp.data.size());
        tex->compressed = std::move(comp);
        tex->requestedCompressed = true;
        tex->compressedFinal = true;
        // Metadata only; the blocks (+ mips) are uploaded directly.
        tex->image.width = static_cast<int>(w);
        tex->image.height = static_cast<int>(h);
        tex->image.channels = 4;
        tex->image.data.clear();
      } else {
        tex->image = std::move(rgba);  // uncompressed fallback
      }
      ok = true;
    }
  }
  tp_ktx2_image_free(nullptr, &k);
  return ok;
}
#endif  // LUSDVIEW_WITH_TEXTOOLS

namespace {  // (resume the file-local helpers)

bool Normalize3(float v[3]) {
  const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (len2 <= 1.0e-12f) return false;
  const float inv = 1.0f / std::sqrt(len2);
  v[0] *= inv;
  v[1] *= inv;
  v[2] *= inv;
  return true;
}

// (definition moved below the anonymous namespace so the `core` loader can
// derive the preview key light from its own lights too; see UpdatePreviewLight)



}  // namespace

// Material diagnostics compatibility hook. Public because both scene loaders
// call it while assembling their otherwise independent DrawScene instances.
void DiagnoseUnsupportedRealtimeLobes(const DrawMaterialCPU& material,
                                      DrawScene* draw) {
  (void)material;
  (void)draw;
}

LoadDiagnostics CategorizeLoadWarnings(
    const std::string& warn_blob, const std::vector<std::string>& skipped) {
  LoadDiagnostics d;
  auto contains = [](const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
  };
  size_t start = 0;
  while (start <= warn_blob.size()) {
    size_t nl = warn_blob.find('\n', start);
    const std::string line = warn_blob.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    start = (nl == std::string::npos) ? warn_blob.size() + 1 : nl + 1;
    // Trim trailing whitespace/CR; skip blank lines.
    size_t end = line.find_last_not_of(" \t\r");
    if (end == std::string::npos) continue;
    const std::string l = line.substr(0, end + 1);

    // Order matters: a full fallback ("using default material") is the most
    // severe and is checked first; unsupported-mtlx before the generic
    // missing-texture bucket since its message also mentions resolution.
    int* bucket = nullptr;
    if (contains(l, "using default material") ||
        contains(l, "using degraded material") ||
        contains(l, "Material conversion failed")) {
      bucket = &d.degraded_material;
    } else if (contains(l, "Unsupported node type") ||
               contains(l, "unsupported MaterialX node")) {
      bucket = &d.unsupported_mtlx;
    } else if (contains(l, "unsupported real-time lobes:")) {
      bucket = &d.unsupported_lobes;
    } else if (contains(l, "failed to load texture") ||
               contains(l, "Failed to load texture") ||
               contains(l, "Failed to load image") ||
               contains(l, "could not be resolved to a UsdUVTexture") ||
               contains(l, "could not be resolved to a texture") ||
               contains(l, "Failed to find MaterialX texture") ||
               contains(l, "Failed to convert MaterialX texture")) {
      bucket = &d.missing_texture;
    } else {
      bucket = &d.other;
    }
    (*bucket)++;
    if (d.examples.size() < 6 && bucket != &d.other) d.examples.push_back(l);
  }

  for (const std::string& s : skipped) {
    if (contains(s, "using default material") ||
        contains(s, "using degraded material") ||
        contains(s, "Material conversion failed")) {
      ++d.degraded_material;
    } else if (contains(s, "unsupported MaterialX node")) {
      ++d.unsupported_mtlx;
    } else if (contains(s, "unsupported real-time lobes:")) {
      ++d.unsupported_lobes;
    } else {
      ++d.skipped;
    }
    if (d.examples.size() < 6) d.examples.push_back(s);
  }
  return d;
}

void AddLightDiagnostics(const DrawScene& draw, LoadDiagnostics* diagnostics) {
  if (!diagnostics) return;
  for (const DrawLightCPU& light : draw.lights) {
    if (light.type == DrawLightCPU::Type::Geometry) {
      ++diagnostics->geometry_lights;
      if (light.geometryMesh >= 0) ++diagnostics->emissive_mesh_lights;
    } else if (light.type == DrawLightCPU::Type::Portal) {
      ++diagnostics->portal_lights;
    }
    if (light.iesValid || !light.shapingIesFile.empty())
      ++diagnostics->ies_profiles;
  }
}

void UpdatePreviewLight(DrawScene* draw) {
  if (!draw) return;
  float fallback[3]{0.5f, 0.8f, 0.6f};
  Normalize3(fallback);
  for (int i = 0; i < 3; ++i) {
    draw->previewLightDir[i] = fallback[i];
    draw->previewLightColor[i] = 1.0f;
  }
  draw->hasPreviewLight = false;

  const DrawLightCPU* chosen = nullptr;
  for (const DrawLightCPU& light : draw->lights) {
    if (light.type == DrawLightCPU::Type::Distant) {
      chosen = &light;
      break;
    }
    if (!chosen && light.type != DrawLightCPU::Type::Dome &&
        light.type != DrawLightCPU::Type::Portal) {
      chosen = &light;
    }
  }
  if (!chosen) return;

  float dir[3]{fallback[0], fallback[1], fallback[2]};
  if (chosen->type == DrawLightCPU::Type::Distant) {
    // DrawLightCPU::direction is the light emission direction; preview shaders
    // use L as the vector from the shaded point toward the light.
    dir[0] = -chosen->direction[0];
    dir[1] = -chosen->direction[1];
    dir[2] = -chosen->direction[2];
  } else {
    float center[3]{0.0f, 0.0f, 0.0f};
    if (draw->hasBounds) {
      for (int i = 0; i < 3; ++i) {
        center[i] = 0.5f * (draw->aabbMin[i] + draw->aabbMax[i]);
      }
    }
    for (int i = 0; i < 3; ++i) {
      dir[i] = chosen->position[i] - center[i];
    }
  }
  if (!Normalize3(dir)) {
    dir[0] = fallback[0];
    dir[1] = fallback[1];
    dir[2] = fallback[2];
  }

  for (int i = 0; i < 3; ++i) {
    draw->previewLightDir[i] = dir[i];
    draw->previewLightColor[i] = chosen->normalizedColor[i];
  }
  draw->hasPreviewLight = true;
}



struct F3 {
  float x{0}, y{0}, z{0};
};

F3 Add(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
F3 Sub(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
F3 Mul(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 Cross(F3 a, F3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
float Length(F3 a) { return std::sqrt(Dot(a, a)); }
F3 Normalize(F3 a, F3 fallback = {0, 1, 0}) {
  const float l = Length(a);
  return l > 1.0e-12f ? Mul(a, 1.0f / l) : fallback;
}

void AppendGeneratedMaterial(DrawScene* out, const float color[3],
                             const std::string& name, int* materialId) {
  if (!out || !materialId) return;
  DrawMaterialCPU mat;
  mat.name = name;
  mat.baseColor[0] = color[0];
  mat.baseColor[1] = color[1];
  mat.baseColor[2] = color[2];
  mat.roughness = 0.75f;
  *materialId = static_cast<int>(out->materials.size());
  out->materials.push_back(std::move(mat));
}

void FinalizeGeneratedMesh(DrawMeshCPU* dm) {
  if (!dm || dm->vertices.empty()) return;
  dm->sourceFaceId.resize(dm->indices.size() / 3);
  for (size_t i = 0; i < dm->sourceFaceId.size(); ++i) {
    dm->sourceFaceId[i] = static_cast<uint32_t>(i);
  }
  dm->wireframeIndices.clear();
  for (size_t i = 0; i + 2 < dm->indices.size(); i += 3) {
    const uint32_t a = dm->indices[i + 0];
    const uint32_t b = dm->indices[i + 1];
    const uint32_t c = dm->indices[i + 2];
    dm->wireframeIndices.push_back(a);
    dm->wireframeIndices.push_back(b);
    dm->wireframeIndices.push_back(b);
    dm->wireframeIndices.push_back(c);
    dm->wireframeIndices.push_back(c);
    dm->wireframeIndices.push_back(a);
  }
}

void AddQuad(DrawMeshCPU* dm, const DrawVertex& a, const DrawVertex& b,
             const DrawVertex& c, const DrawVertex& d) {
  const uint32_t base = static_cast<uint32_t>(dm->vertices.size());
  dm->vertices.push_back(a);
  dm->vertices.push_back(b);
  dm->vertices.push_back(c);
  dm->vertices.push_back(d);
  dm->indices.insert(dm->indices.end(),
                     {base + 0, base + 1, base + 2, base + 0, base + 2,
                      base + 3});
}

float CurveWidthAt(const std::vector<float>& widths, size_t pointIndex,
                   size_t curveIndex, size_t pointCount) {
  if (widths.empty()) return 0.04f;
  if (widths.size() == 1) return widths[0];
  if (pointIndex < widths.size()) return widths[pointIndex];
  if (curveIndex < widths.size()) return widths[curveIndex];
  if (pointCount > 0 && widths.size() >= pointCount) {
    return widths[std::min(pointIndex, widths.size() - 1)];
  }
  return widths.back();
}


double BsplineBasis(int i, int degree, double u, const std::vector<double>& knots) {
  if (degree == 0) {
    const bool last = !knots.empty() && (i + 1 < static_cast<int>(knots.size())) &&
                      std::abs(u - knots.back()) < 1.0e-12 &&
                      std::abs(knots[i + 1] - knots.back()) < 1.0e-12 &&
                      knots[i] < knots[i + 1];
    return (knots[i] <= u && u < knots[i + 1]) || last ? 1.0 : 0.0;
  }
  double v = 0.0;
  const double d0 = knots[i + degree] - knots[i];
  if (std::abs(d0) > 1.0e-12) {
    v += (u - knots[i]) / d0 * BsplineBasis(i, degree - 1, u, knots);
  }
  const double d1 = knots[i + degree + 1] - knots[i + 1];
  if (std::abs(d1) > 1.0e-12) {
    v += (knots[i + degree + 1] - u) / d1 *
         BsplineBasis(i + 1, degree - 1, u, knots);
  }
  return v;
}











}  // namespace lusdview
