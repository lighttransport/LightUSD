// SPDX-License-Identifier: Apache-2.0
// Shared converter pixels/encoding; migrated from the former JS binding.
#include "image-convert.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "c/lightusd-core-c.h"
#include "image-loader.hh"
#include "image-writer.hh"
#include "imageio/png-stream.hh"
#include "tydra/resize-image.hh"
namespace lightusd::image {
namespace {
static inline float acesRrtOdtFit(float v) {
  const float a = v * (v + 0.0245786f) - 0.000090537f;
  const float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
  return (b != 0.0f) ? (a / b) : 0.0f;
}
// In-place ACES tonemap of one linear RGB triple. Output is sRGB/Rec.709 linear
// (apply the sRGB OETF afterwards before quantizing to 8-bit).
static inline void acesFittedRGB(float& r, float& g, float& b) {
  // sRGB/Rec.709 linear -> ACEScg (ACESInputMat).
  const float ir = 0.59719f * r + 0.35458f * g + 0.04823f * b;
  const float ig = 0.07600f * r + 0.90834f * g + 0.01566f * b;
  const float ib = 0.02840f * r + 0.13383f * g + 0.83777f * b;
  const float fr = acesRrtOdtFit(ir);
  const float fg = acesRrtOdtFit(ig);
  const float fb = acesRrtOdtFit(ib);
  // ACEScg -> sRGB/Rec.709 linear (ACESOutputMat).
  r = 1.60475f * fr - 0.53108f * fg - 0.07367f * fb;
  g = -0.10208f * fr + 1.10813f * fg - 0.00605f * fb;
  b = -0.00327f * fr - 0.07276f * fg + 1.07602f * fb;
}

// Convert an fp32 (HDR/EXR) image to 8-bit LDR so it can be written as
// PNG/JPEG. USDZ delivers color textures as sRGB-encoded 8-bit, while EXR data
// is scene-linear, so we apply a default ACES filmic tonemap to the color
// channels and then the sRGB OETF; an alpha channel (index 3) is treated as
// linear data. Returns the input unchanged if it is not fp32 float.
// TODO: exposure/EV control and per-texture colorspace (data vs color)
// handling.
static Image floatImageTo8bit(const Image& img) {
  using PF = Image::PixelFormat;
  if (!(img.bpp == 32 && img.format == PF::Float)) return img;
  if (img.width <= 0 || img.height <= 0 || img.channels < 1 ||
      img.channels > 4) {
    return img;
  }
  const size_t npix =
      static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
  const size_t ch = static_cast<size_t>(img.channels);
  if (npix > SIZE_MAX / ch) return img;
  if (img.data.size() < npix * ch * sizeof(float)) return img;

  auto srgb8 = [](float x) -> uint8_t {
    if (!(x > 0.0f)) x = 0.0f;  // also maps NaN -> 0
    if (x > 1.0f) x = 1.0f;
    const float s = (x <= 0.0031308f)
                        ? (12.92f * x)
                        : (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f);
    int v = static_cast<int>(s * 255.0f + 0.5f);
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  };
  auto lin8 = [](float x) -> uint8_t {
    if (!(x > 0.0f)) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    int v = static_cast<int>(x * 255.0f + 0.5f);
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  };

  const float* src = reinterpret_cast<const float*>(img.data.data());
  Image out;
  out.width = img.width;
  out.height = img.height;
  out.channels = img.channels;
  out.bpp = 8;
  out.format = PF::UInt;
  out.colorspace = img.colorspace;
  out.data.resize(npix * ch);
  for (size_t i = 0; i < npix; i++) {
    const float* p = &src[i * ch];
    uint8_t* o = &out.data[i * ch];
    if (ch >= 3) {
      float r = p[0], g = p[1], b = p[2];
      acesFittedRGB(r, g, b);  // tonemap color, then sRGB-encode below.
      o[0] = srgb8(r);
      o[1] = srgb8(g);
      o[2] = srgb8(b);
      if (ch == 4) o[3] = lin8(p[3]);
    } else if (ch == 2) {
      o[0] = srgb8(acesRrtOdtFit(p[0]));  // grayscale tonemap (no matrix)
      o[1] = lin8(p[1]);
    } else {  // ch == 1
      o[0] = srgb8(acesRrtOdtFit(p[0]));
    }
  }
  return out;
}

// The EXR fast path keeps all-half images in fp16 form. Promote that compact
// representation before resize/tone-map code that expects fp32 samples.
static Image halfImageToFloat(const Image& img) {
  using PF = Image::PixelFormat;
  if (!(img.bpp == 16 && img.format == PF::Float) || img.width <= 0 ||
      img.height <= 0 || img.channels < 1 || img.channels > 4) {
    return img;
  }
  const size_t npix =
      static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
  const size_t ch = static_cast<size_t>(img.channels);
  if (npix > SIZE_MAX / ch || img.data.size() < npix * ch * sizeof(uint16_t)) {
    return img;
  }
  Image out;
  out.width = img.width;
  out.height = img.height;
  out.channels = img.channels;
  out.bpp = 32;
  out.format = PF::Float;
  out.colorspace = img.colorspace;
  out.uri = img.uri;
  out.data.resize(npix * ch * sizeof(float));
  const uint16_t* src = reinterpret_cast<const uint16_t*>(img.data.data());
  float* dst = reinterpret_cast<float*>(out.data.data());
  for (size_t i = 0; i < npix * ch; ++i) dst[i] = lu_half_to_float(src[i]);
  return out;
}

bool Reserve(uint64_t count, size_t stride, size_t* remaining) {
  if (stride && count > *remaining / stride) return false;
  *remaining -= count * stride;
  return true;
}
float Sample(const Image& image, size_t index) {
  if (image.bpp == 8) return float(image.data[index]) / 255.0f;
  if (image.bpp == 16) {
    uint16_t value;
    std::memcpy(&value, image.data.data() + index * 2, 2);
    return image.format == Image::PixelFormat::Float ? lu_half_to_float(value)
                                                     : float(value) / 65535.0f;
  }
  float value;
  std::memcpy(&value, image.data.data() + index * 4, 4);
  return value;
}
void Colorspace(Image* image, const std::string& conversion) {
  const bool to_linear =
      conversion == "srgb-to-linear" || conversion == "srgbToLinear";
  const bool to_srgb =
      conversion == "linear-to-srgb" || conversion == "linearToSrgb";
  if (!to_linear && !to_srgb) return;
  const size_t count =
      size_t(image->width) * size_t(image->height) * size_t(image->channels);
  for (size_t i = 0; i < count; ++i) {
    const size_t channel = i % size_t(image->channels);
    if ((image->channels == 4 && channel == 3) ||
        (image->channels == 2 && channel == 1))
      continue;
    const float v = Sample(*image, i);
    const float value =
        to_linear
            ? (v <= 0.04045f ? v / 12.92f
                             : std::pow((v + 0.055f) / 1.055f, 2.4f))
            : (v <= 0.0031308f ? 12.92f * v
                               : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
    if (image->bpp == 8)
      image->data[i] =
          uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
    else if (image->bpp == 16 && image->format == Image::PixelFormat::UInt) {
      const uint16_t u =
          uint16_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535));
      std::memcpy(image->data.data() + i * 2, &u, 2);
    } else if (image->bpp == 16) {
      const uint16_t h = lu_float_to_half(value);
      std::memcpy(image->data.data() + i * 2, &h, 2);
    } else
      std::memcpy(image->data.data() + i * 4, &value, 4);
  }
}
}  // namespace
nonstd::expected<Image, std::string> PrepareImageForEncoding(
    Image image, const std::string& format, size_t budget) {
  if (image.width <= 0 || image.height <= 0 || image.channels < 1 ||
      image.channels > 4)
    return nonstd::make_unexpected(
        std::string("Invalid image dimensions/channels"));
  const size_t count =
      size_t(image.width) * size_t(image.height) * size_t(image.channels);
  if (count > budget / 8 || image.data.size() > budget)
    return nonstd::make_unexpected(
        std::string("Image encoding working memory budget exceeded"));
  if (format != "exr") {
    if (image.format == Image::PixelFormat::Float) {
      image = halfImageToFloat(image);
      image = floatImageTo8bit(image);
    } else if (format == "jpeg" && image.bpp == 16) {
      Image out = image;
      out.bpp = 8;
      out.data.resize(count);
      for (size_t i = 0; i < count; ++i)
        out.data[i] = uint8_t(std::lround(Sample(image, i) * 255));
      image = std::move(out);
    }
  }
  if (format == "jpeg" && (image.channels == 2 || image.channels == 4)) {
    Image out = image;
    out.channels = image.channels - 1;
    out.data.resize(count / size_t(image.channels) * size_t(out.channels));
    for (size_t pixel = 0; pixel < count / size_t(image.channels); ++pixel)
      for (int channel = 0; channel < out.channels; ++channel)
        out.data[pixel * size_t(out.channels) + size_t(channel)] =
            image.data[pixel * size_t(image.channels) + size_t(channel)];
    image = std::move(out);
  }
  return image;
}
nonstd::expected<ConvertResult, std::string> ConvertImage(
    const uint8_t* bytes, size_t size, const ConvertOptions& opts,
    const std::string& name) {
  auto fail = [](const std::string& error)
      -> nonstd::expected<ConvertResult, std::string> {
    return nonstd::make_unexpected(error);
  };
  constexpr int max_dimension = 16384;
  const std::string format = opts.format == "jpg" ? "jpeg" : opts.format;
  if (!bytes || !size || size > opts.memory_budget_bytes ||
      opts.memory_budget_bytes > (size_t(1) << 30) || opts.width < 0 ||
      opts.height < 0 || opts.max_size < 0 || opts.width > max_dimension ||
      opts.height > max_dimension || opts.max_size > max_dimension ||
      opts.jpeg_quality < 1 || opts.jpeg_quality > 100 ||
      (format != "png" && format != "jpeg" && format != "exr"))
    return fail("Invalid image conversion options/input or memory budget");
  if (!opts.colorspace.empty() && opts.colorspace != "srgb-to-linear" &&
      opts.colorspace != "srgbToLinear" &&
      opts.colorspace != "linear-to-srgb" && opts.colorspace != "linearToSrgb")
    return fail("Unknown colorspace conversion");
  if (!opts.resize_colorspace.empty() && opts.resize_colorspace != "auto" &&
      opts.resize_colorspace != "linear" && opts.resize_colorspace != "srgb")
    return fail("Unknown resize colorspace");
  if (opts.png_encoder != "auto" && opts.png_encoder != "fpng" &&
      opts.png_encoder != "fpnge")
    return fail("Unknown PNG encoder");
  auto info = GetImageInfoFromMemoryBounded(bytes, size, name,
                                            opts.memory_budget_bytes - size);
  if (!info) return fail(info.error());
  if (!info->width || !info->height || info->width > 65536 ||
      info->height > 65536 || !info->channels || info->channels > 4)
    return fail("Image dimensions/channels exceed conversion limits");
  int width = opts.width, height = opts.height;
  if (width <= 0 || height <= 0) {
    width = int(info->width);
    height = int(info->height);
    if (opts.max_size > 0 && std::max(width, height) > opts.max_size) {
      const double scale = double(opts.max_size) / std::max(width, height);
      width = std::max(1, int(width * scale + 0.5));
      height = std::max(1, int(height * scale + 0.5));
    }
  }
  ConvertResult result;
  result.width = width;
  result.height = height;
  result.resized = width != int(info->width) || height != int(info->height);
  const bool png = size >= 24 && bytes[0] == 0x89 && bytes[1] == 'P' &&
                   bytes[2] == 'N' && bytes[3] == 'G';
  const bool change_colorspace = !opts.colorspace.empty();
  size_t remaining = opts.memory_budget_bytes - size;
  size_t full_remaining = remaining;
  const size_t component_bytes = png ? (bytes[24] == 16 ? 2 : 1) : 4;
  const bool whole_fits =
      Reserve(uint64_t(info->width) * info->height,
              size_t(info->channels) * component_bytes * 3, &full_remaining) &&
      Reserve(uint64_t(width) * uint64_t(height),
              size_t(info->channels) * std::max<size_t>(component_bytes, 4) * 5,
              &full_remaining);
  if (format == "png" && png &&
      (opts.low_memory || change_colorspace || !whole_fits)) {
    // Reserve the bounded row window, zlib staging, color LUTs, chunk metadata,
    // and both encoded IDAT and finalized PNG buffers. Large compressible PNGs
    // need not materialize a decoded width*height pixel array.
    const uint64_t rows =
        uint64_t(std::max(info->width, uint32_t(width))) * 4 * 16 * 32 +
        (1u << 20);
    if (rows > remaining)
      return fail("PNG row buffers exceed working memory budget");
    const size_t output_limit = (remaining - size_t(rows)) / 17 * 8;
    bool ok = false;
    if (!result.resized && change_colorspace) {
      const bool to_linear = opts.colorspace == "srgb-to-linear" ||
                             opts.colorspace == "srgbToLinear";
      const bool to_srgb = opts.colorspace == "linear-to-srgb" ||
                           opts.colorspace == "linearToSrgb";
      if (!to_linear && !to_srgb) return fail("Unknown colorspace conversion");
      ok = imageio::ConvertColorspacePNG(
          bytes, size,
          to_linear ? imageio::ColorspaceXform::SrgbToLinear
                    : imageio::ColorspaceXform::LinearToSrgb,
          result.data, output_limit);
    } else if (!change_colorspace) {
      ok = result.resized
               ? imageio::ResizePNG(bytes, size, uint32_t(width),
                                    uint32_t(height),
                                    opts.resize_colorspace == "srgb",
                                    result.data, output_limit)
               : imageio::TranscodePNG(bytes, size, result.data, output_limit);
    }
    if (ok) return result;
    if (!whole_fits)
      return fail(
          "PNG streaming conversion failed or exceeds working memory budget");
  }
  // Account for source decode/scratch, target resize, HDR conversion and
  // encoder buffers BEFORE materializing the whole image. PNG bit depth is
  // explicit; other formats conservatively reserve fp32 channels.
  if (!whole_fits)
    return fail("Image conversion working memory budget exceeded");
  Image image;
  std::string error;
  bool half = false;
  if (format == "exr")
    half = DecodeImageEXRHalf(bytes, size, name, &image, &error);
  if (!half) {
    auto decoded = LoadImageFromMemoryBounded(bytes, size, name,
                                              opts.memory_budget_bytes - size);
    if (!decoded) return fail(decoded.error());
    image = std::move(decoded->image);
  }
  if (result.resized) {
    Image resized;
    const auto filter =
        opts.resize_colorspace == "srgb"     ? tydra::ResizeFilter::SRGB
        : opts.resize_colorspace == "linear" ? tydra::ResizeFilter::Linear
                                             : tydra::ResizeFilter::Auto;
    if (!tydra::ResizeImage(image, width, height, &resized, filter, &error))
      return fail(error);
    image = std::move(resized);
  }
  Colorspace(&image, opts.colorspace);
  auto prepared = PrepareImageForEncoding(std::move(image), format,
                                          opts.memory_budget_bytes - size);
  if (!prepared) return fail(prepared.error());
  WriteOption write;
  write.format = format == "png"    ? WriteImageFormat::PNG
                 : format == "jpeg" ? WriteImageFormat::JPEG
                                    : WriteImageFormat::EXR;
  write.png_encoder = opts.png_encoder == "fpng"    ? PngEncoder::Fpng
                      : opts.png_encoder == "fpnge" ? PngEncoder::Fpnge
                                                    : PngEncoder::Auto;
  write.jpeg_quality = opts.jpeg_quality;
  auto encoded = WriteImageToMemory(*prepared, write);
  if (!encoded) return fail(encoded.error());
  if (encoded->size() > opts.memory_budget_bytes - size)
    return fail("Encoded image exceeds working memory budget");
  result.data = std::move(*encoded);
  return result;
}
}  // namespace lightusd::image
