// SPDX-License-Identifier: Apache-2.0
// lusdview - vendored texture-tools wrapper implementation. See
// texture_tools.hh; keep all textools includes confined to this TU.
#include "texture_tools.hh"

#if defined(LUSDVIEW_WITH_TEXTOOLS)

#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "envmap.h"
#include "texcomp.h"
#include "texpipe.h"
#include "tir.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace lusdview {

namespace {

int TexToolsThreads() {
  const unsigned hw = std::thread::hardware_concurrency();
  return static_cast<int>(hw > 8 ? 8 : hw);
}

float SrgbToLinear(float c) {
  return (c <= 0.04045f) ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
  if (c <= 0.0f) return 0.0f;
  if (c >= 1.0f) return 1.0f;
  return (c <= 0.0031308f) ? 12.92f * c
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

const float* SrgbDecodeLut() {
  static float lut[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      lut[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
    }
    init = true;
  }
  return lut;
}

bool ValidRGBA8(const light3d::Image& img) {
  return img.width > 0 && img.height > 0 && img.channels == 4 &&
         img.data.size() >= static_cast<size_t>(img.width) *
                                static_cast<size_t>(img.height) * 4u;
}

}  // namespace

bool TexToolsAvailable() { return true; }

bool TexToolsResizeRGBA8(light3d::Image* img, int dstW, int dstH, bool srgb,
                         std::string* err) {
  if (!img || dstW <= 0 || dstH <= 0 || !ValidRGBA8(*img)) {
    if (err) *err = "textools resize: invalid image or size";
    return false;
  }
  if (img->width == dstW && img->height == dstH) return true;

  tir_options opt;
  tir_options_init(&opt);
  opt.alpha = TIR_ALPHA_PREMULTIPLY;
  opt.clamp_min = 0.0f;
  opt.clamp_max = 1.0f;
  opt.num_threads = TexToolsThreads();

  const size_t srcPixels =
      static_cast<size_t>(img->width) * static_cast<size_t>(img->height);
  const size_t dstPixels =
      static_cast<size_t>(dstW) * static_cast<size_t>(dstH);

  tir_result r = TIR_SUCCESS;
  std::vector<uint8_t> dstBytes(dstPixels * 4);
  if (srgb) {
    // tir filters linearly; decode sRGB RGB -> linear floats around the
    // resize (alpha stays linear).
    const float* lut = SrgbDecodeLut();
    std::vector<float> srcF(srcPixels * 4);
    for (size_t i = 0; i < srcPixels; ++i) {
      srcF[i * 4 + 0] = lut[img->data[i * 4 + 0]];
      srcF[i * 4 + 1] = lut[img->data[i * 4 + 1]];
      srcF[i * 4 + 2] = lut[img->data[i * 4 + 2]];
      srcF[i * 4 + 3] = static_cast<float>(img->data[i * 4 + 3]) / 255.0f;
    }
    std::vector<float> dstF(dstPixels * 4);
    tir_image_view src{srcF.data(), img->width, img->height, 4, TIR_F32, 0};
    tir_image_view dst{dstF.data(), dstW, dstH, 4, TIR_F32, 0};
    r = tir_resize(nullptr, &src, &dst, &opt);
    if (TIR_OK(r)) {
      for (size_t i = 0; i < dstPixels; ++i) {
        auto enc = [](float v) -> uint8_t {
          return static_cast<uint8_t>(LinearToSrgb(v) * 255.0f + 0.5f);
        };
        dstBytes[i * 4 + 0] = enc(dstF[i * 4 + 0]);
        dstBytes[i * 4 + 1] = enc(dstF[i * 4 + 1]);
        dstBytes[i * 4 + 2] = enc(dstF[i * 4 + 2]);
        float a = dstF[i * 4 + 3];
        a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        dstBytes[i * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
      }
    }
  } else {
    tir_image_view src{img->data.data(), img->width, img->height, 4, TIR_U8,
                       0};
    tir_image_view dst{dstBytes.data(), dstW, dstH, 4, TIR_U8, 0};
    r = tir_resize(nullptr, &src, &dst, &opt);
  }
  if (!TIR_OK(r)) {
    if (err) *err = std::string("tir_resize: ") + tir_result_string(r);
    return false;
  }
  img->width = dstW;
  img->height = dstH;
  img->channels = 4;
  img->data = std::move(dstBytes);
  return true;
}

bool TexToolsCompress(const light3d::Image& img, bool srgb,
                      DrawCompressedFormat format,
                      DrawCompressedImageCPU* out) {
  if (!out || !ValidRGBA8(img)) return false;
  const uint32_t w = static_cast<uint32_t>(img.width);
  const uint32_t h = static_cast<uint32_t>(img.height);
  const uint8_t* rgba = img.data.data();

  size_t size = 0;
  tc_result r = TC_ERROR_UNSUPPORTED;
  std::vector<uint8_t> blocks;
  switch (format) {
    case DrawCompressedFormat::BC1: {
      tc_bc1_options o;
      tc_bc1_options_init(&o);
      o.srgb = srgb ? 1 : 0;
      size = tc_bc1_compressed_size(w, h);
      blocks.resize(size);
      r = tc_bc1_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::BC3: {
      tc_bc3_options o;
      tc_bc3_options_init(&o);
      o.srgb = srgb ? 1 : 0;
      size = tc_bc3_compressed_size(w, h);
      blocks.resize(size);
      r = tc_bc3_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::BC5: {
      // Two-channel (R,G) — normal maps. Encoder reads R,G of the RGBA8 input.
      tc_bc5_options o;
      tc_bc5_options_init(&o);
      size = tc_bc5_compressed_size(w, h);
      blocks.resize(size);
      r = tc_bc5_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::BC7: {
      tc_bc7_options o;
      tc_bc7_options_init(&o);
      // texcomp 96dabe5 turned FAST into a mode-6-only real-time path
      // (~18x faster, ~9 dB worse on gradients). QUICKBC7 is what FAST used to
      // produce: same quality and speed as lusdview's previous BC7 output.
      o.quality = TC_BC7_QUALITY_QUICKBC7;
      o.srgb = srgb ? 1 : 0;
      o.threads = TexToolsThreads();
      size = tc_bc7_compressed_size(w, h);
      blocks.resize(size);
      r = tc_bc7_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::ETC2_RGB: {
      tc_etc2_options o;
      tc_etc2_options_init(&o);
      o.srgb = srgb ? 1 : 0;
      o.alpha = 0;
      size = tc_etc2_rgb_compressed_size(w, h);
      blocks.resize(size);
      r = tc_etc2_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::ETC2_RGBA: {
      tc_etc2_options o;
      tc_etc2_options_init(&o);
      o.srgb = srgb ? 1 : 0;
      o.alpha = 1;
      size = tc_etc2_rgba_compressed_size(w, h);
      blocks.resize(size);
      r = tc_etc2_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::ASTC_4x4: {
      tc_astc_options o;
      tc_astc_options_init(&o);
      o.block_x = 4;
      o.block_y = 4;
      o.srgb = srgb ? 1 : 0;
      o.threads = TexToolsThreads();
      size = tc_astc_compressed_size(w, h, &o);
      blocks.resize(size);
      r = tc_astc_compress_rgba8(rgba, w, h, size_t(w) * 4u, &o, blocks.data(), size);
      break;
    }
    case DrawCompressedFormat::BC6H:
      // HDR (float RGB) — the RGBA8 compress-on-load path can't feed BC6H.
      // Reserved for the HDR/EXR texture path (follow-up).
      return false;
    case DrawCompressedFormat::None:
      return false;
  }
  if (r != TC_SUCCESS) return false;
  out->format = format;
  out->width = img.width;
  out->height = img.height;
  out->data = std::move(blocks);
  return true;
}

namespace {
bool CapsAllow(DrawCompressedFormat f, const TextureCompressCaps& c) {
  switch (f) {
    case DrawCompressedFormat::BC1:
    case DrawCompressedFormat::BC3:
    case DrawCompressedFormat::BC7: return c.bc;
    case DrawCompressedFormat::BC5: return c.bc5 || c.bc;
    case DrawCompressedFormat::BC6H: return c.bc6h || c.bc;
    case DrawCompressedFormat::ETC2_RGB:
    case DrawCompressedFormat::ETC2_RGBA: return c.etc2;
    case DrawCompressedFormat::ASTC_4x4: return c.astc;
    case DrawCompressedFormat::None: return false;
  }
  return false;
}
bool DecodeToImage(uint32_t w, uint32_t h, light3d::Image* out) {
  out->width = static_cast<int>(w);
  out->height = static_cast<int>(h);
  out->channels = 4;
  out->data.assign(static_cast<size_t>(w) * h * 4u, 0);
  return true;
}
}  // namespace

bool TexToolsAdaptCompressed(const uint8_t* blocks, size_t nbytes, bool srcIsUni,
                             DrawCompressedFormat srcFmt, uint32_t w, uint32_t h,
                             const TextureCompressCaps& caps,
                             DrawCompressedImageCPU* outCompressed,
                             light3d::Image* outRGBA) {
  if (!blocks || !nbytes || !w || !h || !outCompressed || !outRGBA) return false;
  const size_t rowBytes = static_cast<size_t>(w) * 4u;

  auto keepBlocks = [&](DrawCompressedFormat f) {
    outCompressed->format = f;
    outCompressed->width = static_cast<int>(w);
    outCompressed->height = static_cast<int>(h);
    outCompressed->data.assign(blocks, blocks + nbytes);
  };

  if (srcIsUni) {
    // uni is a valid ASTC 4x4 block stream and transcodes cheaply.
    if (caps.astc) { keepBlocks(DrawCompressedFormat::ASTC_4x4); return true; }
    if (caps.bc) {
      const size_t sz = tc_bc7_compressed_size(w, h);
      outCompressed->data.resize(sz);
      if (tc_uni_transcode_bc7(blocks, w, h, outCompressed->data.data(), sz) != TC_SUCCESS)
        return false;
      outCompressed->format = DrawCompressedFormat::BC7;
      outCompressed->width = static_cast<int>(w);
      outCompressed->height = static_cast<int>(h);
      return true;
    }
    if (caps.etc2) {
      const size_t sz = tc_etc2_rgba_compressed_size(w, h);
      outCompressed->data.resize(sz);
      if (tc_uni_transcode_etc2(blocks, w, h, 1, outCompressed->data.data(), sz) != TC_SUCCESS)
        return false;
      outCompressed->format = DrawCompressedFormat::ETC2_RGBA;
      outCompressed->width = static_cast<int>(w);
      outCompressed->height = static_cast<int>(h);
      return true;
    }
    // No compressed format: decode uni -> RGBA8.
    DecodeToImage(w, h, outRGBA);
    return tc_uni_decompress_rgba8(blocks, w, h, rowBytes, outRGBA->data.data(),
                                   outRGBA->data.size()) == TC_SUCCESS;
  }

  // Stored (non-uni) block format: upload as-is if the device supports it.
  if (CapsAllow(srcFmt, caps)) { keepBlocks(srcFmt); return true; }

  // Otherwise decode to RGBA8. texcomp now ships a decoder for every LDR block
  // format, so any LDR payload can fall back to an uncompressed upload.
  DecodeToImage(w, h, outRGBA);
  uint8_t* dst = outRGBA->data.data();
  const size_t dsz = outRGBA->data.size();
  switch (srcFmt) {
    case DrawCompressedFormat::BC7:
      return tc_bc7_decompress_rgba8(blocks, w, h, rowBytes, dst, dsz) == TC_SUCCESS;
    case DrawCompressedFormat::BC1:
      return tc_bc1_decompress_rgba8(blocks, w, h, rowBytes, dst, dsz) == TC_SUCCESS;
    case DrawCompressedFormat::BC3:
      return tc_bc3_decompress_rgba8(blocks, w, h, rowBytes, dst, dsz) == TC_SUCCESS;
    case DrawCompressedFormat::BC5:
      return tc_bc5_decompress_rgba8(blocks, w, h, /*snorm=*/0, rowBytes, dst,
                                     dsz) == TC_SUCCESS;
    case DrawCompressedFormat::ETC2_RGB:
    case DrawCompressedFormat::ETC2_RGBA:
      return tc_etc2_decompress_rgba8(
                 blocks, w, h,
                 /*alpha=*/srcFmt == DrawCompressedFormat::ETC2_RGBA, rowBytes,
                 dst, dsz) == TC_SUCCESS;
    case DrawCompressedFormat::ASTC_4x4:
      return tc_astc_decompress_rgba8(blocks, w, h, 4u, 4u, dst, dsz) == TC_SUCCESS;
    case DrawCompressedFormat::BC6H: {
      // HDR has no RGBA8 form. On a device that can't sample BC6H, decode to
      // float and tone-map (Reinhard) so the texture is still usable.
      std::vector<float> hdr(static_cast<size_t>(w) * h * 4u);
      if (tc_bc6h_decompress_rgbaf(blocks, w, h, /*is_signed=*/0,
                                   static_cast<size_t>(w) * 4u * sizeof(float),
                                   hdr.data(),
                                   hdr.size() * sizeof(float)) != TC_SUCCESS) {
        outRGBA->data.clear();
        return false;
      }
      for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
          const float v = hdr[i * 4 + static_cast<size_t>(c)];
          const float t = v / (1.0f + v);  // Reinhard
          const float s = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
          dst[i * 4 + static_cast<size_t>(c)] =
              static_cast<uint8_t>(s * 255.0f + 0.5f);
        }
        dst[i * 4 + 3] = 255;
      }
      return true;
    }
    case DrawCompressedFormat::None:
    default:
      outRGBA->data.clear();
      return false;
  }
}

bool TexToolsAdaptCompressedLevel(const uint8_t* blocks, size_t nbytes,
                                  bool srcIsUni, DrawCompressedFormat srcFmt,
                                  DrawCompressedFormat targetFmt, uint32_t w,
                                  uint32_t h, std::vector<uint8_t>* out) {
  if (!blocks || !nbytes || !w || !h || !out) return false;
  if (srcIsUni) {
    if (targetFmt == DrawCompressedFormat::ASTC_4x4) {
      out->assign(blocks, blocks + nbytes);  // uni blocks are valid ASTC 4x4
      return true;
    }
    if (targetFmt == DrawCompressedFormat::BC7) {
      const size_t sz = tc_bc7_compressed_size(w, h);
      out->resize(sz);
      return tc_uni_transcode_bc7(blocks, w, h, out->data(), sz) == TC_SUCCESS;
    }
    if (targetFmt == DrawCompressedFormat::ETC2_RGBA) {
      const size_t sz = tc_etc2_rgba_compressed_size(w, h);
      out->resize(sz);
      return tc_uni_transcode_etc2(blocks, w, h, 1, out->data(), sz) == TC_SUCCESS;
    }
    return false;
  }
  // Stored block format: only a direct copy to the same format is supported.
  if (targetFmt == srcFmt) {
    out->assign(blocks, blocks + nbytes);
    return true;
  }
  return false;
}

bool TexToolsBuildMips(const light3d::Image& base, const TexUsage& usage,
                       std::vector<light3d::Image>* outMips) {
  if (!outMips || !ValidRGBA8(base)) return false;
  outMips->clear();
  if (base.width == 1 && base.height == 1) return true;  // no levels below 1x1

  auto tirEdge = [](int wrap) -> tir_edge_mode {
    switch (static_cast<WrapMode>(wrap)) {
      case WrapMode::Repeat: return TIR_EDGE_WRAP;
      case WrapMode::Mirror: return TIR_EDGE_REFLECT;
      case WrapMode::ClampToEdge:
      case WrapMode::ClampToBorder:
      default: return TIR_EDGE_CLAMP;
    }
  };

  const tp_content content =
      usage.normalMap ? TP_CONTENT_NORMAL
                      : (usage.alphaTested ? TP_CONTENT_ALPHA_TESTED
                                           : TP_CONTENT_COLOR);
  tp_options opt;
  tp_options_init(&opt, content, TP_CODEC_BC1);  // codec unused by mip build
  opt.srgb = usage.srgb ? 1 : 0;
  opt.srgb_aware = usage.srgb ? 1 : 0;
  opt.alpha = TIR_ALPHA_PREMULTIPLY;
  opt.alpha_test_threshold = usage.alphaCutoff;
  opt.normal_encoding = TIR_NORMAL_UNORM;
  opt.edge_x = tirEdge(usage.wrapS);
  opt.edge_y = tirEdge(usage.wrapT);
  for (int c = 0; c < 4; ++c) {
    opt.channel_op[c] = static_cast<tp_channel_op>(usage.channelOp[c]);
  }
  opt.threads = TexToolsThreads();

  tir_image_view baseView{const_cast<uint8_t*>(base.data.data()), base.width,
                          base.height, 4, TIR_U8, 0};
  tp_mip_chain chain{};
  const tp_result r = tp_build_mips(nullptr, &baseView, 1, &opt, &chain);
  if (!TP_OK(r)) return false;

  outMips->reserve(static_cast<size_t>(
      chain.num_levels > 1 ? chain.num_levels - 1 : 0));
  for (int l = 1; l < chain.num_levels; ++l) {
    const tp_surface& s = chain.level[l];
    light3d::Image img;
    img.width = s.width;
    img.height = s.height;
    img.channels = 4;
    img.data.resize(static_cast<size_t>(s.width) *
                    static_cast<size_t>(s.height) * 4u);
    auto quant = [](float v) -> uint8_t {
      v = v * 255.0f + 0.5f;
      if (v < 0.0f) v = 0.0f;
      if (v > 255.0f) v = 255.0f;
      return static_cast<uint8_t>(v);
    };
    for (int y = 0; y < s.height; ++y) {
      const float* row = reinterpret_cast<const float*>(
          reinterpret_cast<const uint8_t*>(s.data) +
          static_cast<size_t>(y) * s.stride);
      uint8_t* drow = img.data.data() +
                      static_cast<size_t>(y) * static_cast<size_t>(s.width) * 4u;
      for (int x = 0; x < s.width; ++x) {
        const float* px = row + static_cast<size_t>(x) * s.channels;
        // Normal-map chains are 3-channel; grayscale chains 1-2. Expand to
        // RGBA8 to match the DrawTextureCPU convention.
        const float c0 = px[0];
        const float c1 = s.channels > 1 ? px[1] : c0;
        const float c2 = s.channels > 2 ? px[2] : c0;
        const float c3 = s.channels > 3 ? px[3] : 1.0f;
        drow[x * 4 + 0] = quant(c0);
        drow[x * 4 + 1] = quant(c1);
        drow[x * 4 + 2] = quant(c2);
        drow[x * 4 + 3] = quant(c3);
      }
    }
    outMips->push_back(std::move(img));
  }
  tp_mip_chain_free(nullptr, &chain);
  return true;
}

bool TexToolsProbeToEquirect(const float* rgb, int width, int height,
                             int format, int outW, std::vector<float>* outRgb,
                             int* outH) {
  if (!rgb || !outRgb || !outH || width <= 0 || height <= 0 || outW < 4 ||
      (format != 2 && format != 3)) {
    return false;
  }
  const int W = outW;
  const int H = outW / 2;
  outRgb->assign(static_cast<size_t>(W) * H * 3u, 0.0f);

  auto sample = [&](float u, float v, float* out3) {
    // Bilinear, clamped to the probe image.
    const float fx = u * static_cast<float>(width) - 0.5f;
    const float fy = v * static_cast<float>(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    auto at = [&](int x, int y) -> const float* {
      x = x < 0 ? 0 : (x >= width ? width - 1 : x);
      y = y < 0 ? 0 : (y >= height ? height - 1 : y);
      return rgb + (static_cast<size_t>(y) * width + x) * 3u;
    };
    const float* p00 = at(x0, y0);
    const float* p10 = at(x0 + 1, y0);
    const float* p01 = at(x0, y0 + 1);
    const float* p11 = at(x0 + 1, y0 + 1);
    for (int c = 0; c < 3; ++c) {
      const float a = p00[c] + (p10[c] - p00[c]) * tx;
      const float b = p01[c] + (p11[c] - p01[c]) * tx;
      out3[c] = a + (b - a) * ty;
    }
  };

  const float kPi = 3.14159265358979323846f;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      float dir[3];
      em_uv_to_dir(EM_PROJ_EQUIRECT, 0,
                   (static_cast<float>(x) + 0.5f) / static_cast<float>(W),
                   (static_cast<float>(y) + 0.5f) / static_cast<float>(H), dir);
      float u = 0.5f, v = 0.5f;
      if (format == 2) {
        // Mirrored ball, photographed along -Z: the ball normal reflecting
        // `dir` back to the camera is normalize(dir + (0,0,1)); the normal's
        // xy IS the image position within the unit disc.
        float nx = dir[0], ny = dir[1], nz = dir[2] + 1.0f;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
          u = 0.5f + 0.5f * (nx / len);
          v = 0.5f - 0.5f * (ny / len);
        }
      } else {
        // Angular / light-probe (Debevec): image radius = angle from the
        // forward axis (-Z) / pi, full sphere at the disc edge.
        float z = dir[2];
        z = z < -1.0f ? -1.0f : (z > 1.0f ? 1.0f : z);
        const float theta = std::acos(-z);
        const float lxy = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
        const float r = (lxy > 1e-8f) ? (theta / kPi) : 0.0f;
        if (lxy > 1e-8f) {
          u = 0.5f + 0.5f * r * (dir[0] / lxy);
          v = 0.5f - 0.5f * r * (dir[1] / lxy);
        }
      }
      sample(u, v, outRgb->data() + (static_cast<size_t>(y) * W + x) * 3u);
    }
  }
  *outH = H;
  return true;
}

bool TexToolsBuildDomeIbl(const float* equirectRgb, int width, int height,
                          bool highQuality, DomeIblCPU* out) {
  if (!out || !equirectRgb || width <= 0 || height <= 0) return false;
  *out = DomeIblCPU{};

  // Retain a bounded HDR latlong for the software RT backends. Their compact
  // texture table is byte-addressed, so Radiance RGBE preserves the useful HDR
  // range at the same bandwidth as RGBA8. Downsampling here also makes a dome
  // lookup substantially friendlier to the CUDA texture working set.
  {
    const int dstW = std::min(width, highQuality ? 2048 : 1024);
    const int dstH = std::max(1, int((int64_t(height) * dstW) / width));
    out->envLatlongWidth = dstW;
    out->envLatlongHeight = dstH;
    out->envLatlongRgbe.resize(size_t(dstW) * size_t(dstH) * 4u);
    for (int y = 0; y < dstH; ++y) {
      const int sy = std::min(height - 1, int((int64_t(y) * height) / dstH));
      for (int x = 0; x < dstW; ++x) {
        const int sx = std::min(width - 1, int((int64_t(x) * width) / dstW));
        const float* src = equirectRgb + (size_t(sy) * width + sx) * 3u;
        uint8_t* dst = out->envLatlongRgbe.data() +
                       (size_t(y) * dstW + x) * 4u;
        const float peak = std::max(src[0], std::max(src[1], src[2]));
        if (!(peak > 1.0e-32f) || !std::isfinite(peak)) {
          dst[0] = dst[1] = dst[2] = dst[3] = 0;
          continue;
        }
        int exponent = 0;
        const float mantissa = std::frexp(peak, &exponent);
        const float encode = mantissa * 256.0f / peak;
        dst[0] = static_cast<uint8_t>(std::min(255.0f,
                                               std::max(0.0f, src[0] * encode)));
        dst[1] = static_cast<uint8_t>(std::min(255.0f,
                                               std::max(0.0f, src[1] * encode)));
        dst[2] = static_cast<uint8_t>(std::min(255.0f,
                                               std::max(0.0f, src[2] * encode)));
        dst[3] = static_cast<uint8_t>(std::max(0, std::min(255, exponent + 128)));
      }
    }
  }

  em_image equirect{};
  equirect.proj = EM_PROJ_EQUIRECT;
  equirect.width = width;
  equirect.height = height;
  equirect.channels = 3;
  equirect.faces = 1;
  equirect.data = const_cast<float*>(equirectRgb);

  // Bound the per-sample source cost: prefilter/irradiance sample a modest
  // cube resample of the (possibly multi-K) equirect.
  const int srcCube = highQuality ? 256 : 128;
  em_image cube{};
  if (!EM_OK(em_convert(nullptr, &equirect, EM_PROJ_CUBE, srcCube, &cube))) {
    return false;
  }

  // Keep the full-resolution, un-clamped cube for env backgrounds (RT miss
  // shading samples it directly; clamping would visibly dim bright lights in
  // the backdrop).
  {
    const size_t n =
        static_cast<size_t>(cube.faces) * cube.width * cube.height * 3u;
    out->envCubeSize = cube.width;
    out->envCube.assign(cube.data, cube.data + n);
  }

  // Firefly clamp: tiny ultra-bright sources (night HDRI lanterns, sun disks)
  // alias badly through the modest importance-sample counts below. Clamp the
  // source radiance to a multiple of the mean luminance — standard preview
  // trade (slightly dims pin-point lights, kills speckle noise).
  {
    const size_t n =
        static_cast<size_t>(cube.faces) * cube.width * cube.height;
    double meanLum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const float* t = cube.data + i * 3;
      meanLum += 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
    }
    meanLum /= static_cast<double>(n);
    const float maxLum = static_cast<float>(meanLum) * 64.0f;
    if (maxLum > 0.0f) {
      for (size_t i = 0; i < n; ++i) {
        float* t = cube.data + i * 3;
        const float lum = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
        if (lum > maxLum) {
          const float s = maxLum / lum;
          t[0] *= s;
          t[1] *= s;
          t[2] *= s;
        }
      }
    }
  }

  const int specFace = highQuality ? 64 : 32;
  const int specLevels = highQuality ? 5 : 4;
  const int specSamples = highQuality ? 64 : 32;
  const int irrFace = highQuality ? 32 : 16;
  const int irrSamples = highQuality ? 512 : 256;
  const int lutSize = 64;

  std::vector<em_image> levels(static_cast<size_t>(specLevels));
  for (em_image& l : levels) l = em_image{};
  bool ok = EM_OK(em_prefilter_specular(nullptr, &cube, specFace, specLevels,
                                        specSamples, levels.data()));
  // Irradiance is cosine-sampled; tiny ultra-bright sources (night HDRIs)
  // produce fireflies when sampled from the sharp cube. Box-average the cube
  // down first (energy-preserving, unlike em_convert's point resample), then
  // integrate with a higher sample count.
  em_image irrSrc{};
  em_image irr{};
  if (ok) {
    const int srcFs = cube.width;
    const int dstFs = highQuality ? 32 : 16;
    const int block = srcFs / dstFs;
    ok = EM_OK(em_image_alloc(nullptr, &irrSrc, EM_PROJ_CUBE, dstFs, dstFs, 3));
    if (ok && block >= 1) {
      const float invN = 1.0f / (static_cast<float>(block) * static_cast<float>(block));
      for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < dstFs; ++y) {
          for (int x = 0; x < dstFs; ++x) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < block; ++sy) {
              const float* row =
                  em_image_texel(&cube, f, x * block, y * block + sy);
              for (int sx = 0; sx < block; ++sx) {
                acc[0] += row[sx * 3 + 0];
                acc[1] += row[sx * 3 + 1];
                acc[2] += row[sx * 3 + 2];
              }
            }
            float* t = em_image_texel(&irrSrc, f, x, y);
            t[0] = acc[0] * invN;
            t[1] = acc[1] * invN;
            t[2] = acc[2] * invN;
          }
        }
      }
    }
  }
  if (ok) {
    ok = EM_OK(em_irradiance_cube(nullptr, &irrSrc, irrFace, irrSamples, &irr));
  }
  if (ok) {
    // Order-2 SH projection of the irradiance cube for the RT surface
    // ambient. Projection and shader evaluation use the same fixed 9-term
    // real-SH polynomial basis, so the axis convention cancels out.
    out->shIrradiance.assign(27, 0.0f);
    auto basis9 = [](const float d[3], float* b) {
      const float x = d[0], y = d[1], z = d[2];
      b[0] = 0.282095f;
      b[1] = 0.488603f * y;
      b[2] = 0.488603f * z;
      b[3] = 0.488603f * x;
      b[4] = 1.092548f * x * y;
      b[5] = 1.092548f * y * z;
      b[6] = 0.315392f * (3.0f * z * z - 1.0f);
      b[7] = 1.092548f * x * z;
      b[8] = 0.546274f * (x * x - y * y);
    };
    for (int f = 0; f < 6; ++f) {
      for (int y = 0; y < irr.height; ++y) {
        for (int x = 0; x < irr.width; ++x) {
          float dir[3];
          em_uv_to_dir(EM_PROJ_CUBE, f,
                       (static_cast<float>(x) + 0.5f) / static_cast<float>(irr.width),
                       (static_cast<float>(y) + 0.5f) / static_cast<float>(irr.height), dir);
          const float sa =
              em_texel_solid_angle(EM_PROJ_CUBE, irr.width, irr.height, f, x, y);
          const float* t = em_image_texel(&irr, f, x, y);
          float b[9];
          basis9(dir, b);
          for (int k = 0; k < 9; ++k) {
            const float w = b[k] * sa;
            out->shIrradiance[static_cast<size_t>(k) * 3 + 0] += t[0] * w;
            out->shIrradiance[static_cast<size_t>(k) * 3 + 1] += t[1] * w;
            out->shIrradiance[static_cast<size_t>(k) * 3 + 2] += t[2] * w;
          }
        }
      }
    }
  }
  if (ok) {
    out->specFaceSize = specFace;
    out->specLevels.resize(static_cast<size_t>(specLevels));
    for (int l = 0; l < specLevels; ++l) {
      const em_image& li = levels[static_cast<size_t>(l)];
      const size_t n = static_cast<size_t>(li.faces) * li.width * li.height * 3u;
      out->specLevels[static_cast<size_t>(l)].assign(li.data, li.data + n);
    }
    const size_t irrN = static_cast<size_t>(irr.faces) * irr.width * irr.height * 3u;
    out->irrFaceSize = irrFace;
    out->irradiance.assign(irr.data, irr.data + irrN);
    // BRDF LUT is environment-independent: compute once per process.
    static std::vector<float> s_lut;
    static int s_lutSize = 0;
    if (s_lutSize != lutSize) {
      s_lut.resize(static_cast<size_t>(lutSize) * lutSize * 2u);
      em_brdf_lut(lutSize, 1024, s_lut.data());
      s_lutSize = lutSize;
    }
    out->lutSize = lutSize;
    out->brdfLut = s_lut;
    out->valid = true;
  }
  for (em_image& l : levels) em_image_free(nullptr, &l);
  em_image_free(nullptr, &irr);
  em_image_free(nullptr, &irrSrc);
  em_image_free(nullptr, &cube);
  return out->valid;
}

}  // namespace lusdview

#else  // !LUSDVIEW_WITH_TEXTOOLS

namespace lusdview {

bool TexToolsAvailable() { return false; }

bool TexToolsResizeRGBA8(light3d::Image*, int, int, bool, std::string*) {
  return false;
}

bool TexToolsCompress(const light3d::Image&, bool, DrawCompressedFormat,
                      DrawCompressedImageCPU*) {
  return false;
}

bool TexToolsAdaptCompressed(const uint8_t*, size_t, bool,
                             DrawCompressedFormat, uint32_t, uint32_t,
                             const TextureCompressCaps&,
                             DrawCompressedImageCPU*, light3d::Image*) {
  return false;
}

bool TexToolsAdaptCompressedLevel(const uint8_t*, size_t, bool,
                                  DrawCompressedFormat, DrawCompressedFormat,
                                  uint32_t, uint32_t, std::vector<uint8_t>*) {
  return false;
}

bool TexToolsBuildMips(const light3d::Image&, const TexUsage&,
                       std::vector<light3d::Image>*) {
  return false;
}

bool TexToolsBuildDomeIbl(const float*, int, int, bool, DomeIblCPU*) {
  return false;
}

bool TexToolsProbeToEquirect(const float*, int, int, int, int,
                             std::vector<float>*, int*) {
  return false;
}

}  // namespace lusdview

#endif  // LUSDVIEW_WITH_TEXTOOLS
