// Support files
//
// - OpenEXR(through TinyEXR). 16bit and 32bit
// - HDR/RGBE (Radiance) through stb_image. float32
// - TIFF/DNG(through TinyDNG). 8bit, packed 10/12/14bit (expanded to 16bit),
//   16bit and 32bit
// - PNG(8bit, 16bit), Jpeg, bmp, tga, ...(through stb_image or wuffs).
//
// - [ ] Use fpng for 8bit PNG when `stb_image` is used
// - [ ] Support LoD tile, multi-channel for TIFF image
//

#include "safe-arithmetic.hh"

#if defined(LIGHTUSD_WITH_NANOIMAGE)
extern "C" {
#include "external/nanoimage/nanoimage.h"
#include "external/nanoimage/nanoimage_jpeg.h"
#include "external/nanoimage/nanoimage_png.h"
#include "external/nanoimage/nanoimage_bmp.h"
#include "external/nanoimage/nanoimage_tga.h"
}
#endif

#if defined(LIGHTUSD_WITH_EXR)
#if defined(LIGHTUSD_EXR_V3)
// Pure-C11 tinyexr v3 C backend (default).
#include "external/tinyexr/include/exr.h"
#else
// Legacy v1 backend (deprecated; LIGHTUSD_USE_TINYEXR_V3=OFF).
#include "external/tinyexr.h"
#endif
#endif

#if defined(LIGHTUSD_WITH_TEXTOOLS)
// KTX2 / GPU-compressed texture reader (pure-C11 texpipe + texcomp). Decodes a
// KTX2 (uni / BC7 / ASTC LDR) to uncompressed RGBA8 at load. See
// src/external/textools/README.lightusd.md.
#include "texpipe.h"  // pulls in texcomp.h; tp_ktx2_read / tp_ktx2_decode_level_rgba8
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/zstd.h"  // ZSTD_decompress for KTX2 supercompressionScheme 2
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#endif

#if defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)

#ifndef LIGHTUSD_NO_WUFFS_IMPLEMENTATION
#define WUFFS_IMPLEMENTATION

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__BMP
//#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__JPEG
//#define WUFFS_CONFIG__MODULE__WBMP
#endif

#else

#if !defined( LIGHTUSD_NO_BUILTIN_IMAGE_LOADER)

// stb_image
#ifndef LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif

#endif

#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif


#if defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)

#include "external/wuffs-unsupported-snapshot.c"

#else

#if !defined( LIGHTUSD_NO_BUILTIN_IMAGE_LOADER)

// fpng, stb_image
#include "external/fpng.h"

// avoid duplicated symbols when lightusd is linked to an app/library whose also use stb_image.
#define STB_IMAGE_STATIC
#include "external/stb_image.h"

#endif

#endif

#if defined(LIGHTUSD_WITH_TIFF)
// stb_image is owned by this translation unit; TinyDNG's implementation lives
// in tiny_dng_loader.cc and must not include a second stb_image copy here.
#ifndef TINY_DNG_LOADER_NO_STB_IMAGE_INCLUDE
#define TINY_DNG_LOADER_NO_STB_IMAGE_INCLUDE
#endif
#include "external/tiny_dng_loader.h"
#endif

#if defined(LIGHTUSD_WITH_LIBTIFF)
#include <tiffio.h>
// tiny_dng_loader exposes these as namespace constants, while tiffio.h
// defines same-named preprocessor macros. Keep the TinyDNG API usable below.
#ifdef SAMPLEFORMAT_UINT
#undef SAMPLEFORMAT_UINT
#undef SAMPLEFORMAT_INT
#undef SAMPLEFORMAT_IEEEFP
#endif
#endif


#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>  // for std::memcpy
#include <cstdint>  // for SIZE_MAX
#include <mutex>

#include "image-loader.hh"
#include "io-util.hh"
#if defined(LIGHTUSD_WITH_EXR) && defined(LIGHTUSD_EXR_V3)
#include "memory-budget.hh"  // bound v3 C EXR decode allocations
#endif

namespace lightusd {
namespace image {

namespace {
std::mutex g_image_loader_mutex;
LoadImageDataFunction g_image_loader = nullptr;
void *g_image_loader_user_data = nullptr;
GetImageInfoFunction g_image_info_loader = nullptr;
void *g_image_info_loader_user_data = nullptr;

void GetImageLoader(LoadImageDataFunction *loader, void **user_data) {
  std::lock_guard<std::mutex> lock(g_image_loader_mutex);
  *loader = g_image_loader;
  *user_data = g_image_loader_user_data;
}

void GetImageInfoLoader(GetImageInfoFunction *loader, void **user_data) {
  std::lock_guard<std::mutex> lock(g_image_loader_mutex);
  *loader = g_image_info_loader;
  *user_data = g_image_info_loader_user_data;
}
}  // namespace

void SetImageLoader(LoadImageDataFunction loader, void *user_data) {
  std::lock_guard<std::mutex> lock(g_image_loader_mutex);
  g_image_loader = loader;
  g_image_loader_user_data = user_data;
}

void SetImageInfoLoader(GetImageInfoFunction loader, void *user_data) {
  std::lock_guard<std::mutex> lock(g_image_loader_mutex);
  g_image_info_loader = loader;
  g_image_info_loader_user_data = user_data;
}

namespace {

// Safety net against image "decompression bombs": a tiny compressed file can
// declare enormous dimensions that pass each decoder's per-axis limit (e.g.
// stb's STBI_MAX_DIMENSIONS = 1<<24) yet expand to a multi-GB buffer. The
// per-decoder size math below already guards integer overflow with safe::mul*,
// but the resulting resize() would still attempt a huge allocation — and under
// -fno-exceptions an allocation failure aborts the process rather than throwing.
// We reject anything above this ceiling with a clean error instead. This is a
// ceiling, not a policy limit: legitimate textures are far smaller (a 16K RGBA8
// image is 1 GiB; fp32 would be 4 GiB).
static constexpr size_t kMaxDecodedImageBytes = size_t(2048) * 1024 * 1024;  // 2 GiB

// Compute max bytes from a memory-limit-in-MB setting using uint64_t to avoid
// overflow on 32-bit platforms. Clamps to SIZE_MAX if the result exceeds it.
inline size_t MaxMemoryBytes(uint64_t limit_mb) {
  constexpr uint64_t kBytesPerMiB = uint64_t(1024) * uint64_t(1024);
#if SIZE_MAX < UINT64_MAX
  constexpr uint64_t kMaxBytes =
      uint64_t((std::numeric_limits<size_t>::max)());
#else
  constexpr uint64_t kMaxBytes =
      (std::numeric_limits<uint64_t>::max)();
#endif
  if (limit_mb > (kMaxBytes / kBytesPerMiB)) {
    return (std::numeric_limits<size_t>::max)();
  }
  return static_cast<size_t>(limit_mb * kBytesPerMiB);
}

#if defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)

bool DecodeImageWUFF(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image, std::string *warn,
                    std::string *err) {

  (void)warn;
  if (!bytes || !image || size == 0) {
    if (err) *err = "WUFFS: empty image input: " + uri + "\n";
    return false;
  }
  wuffs_base__image_decoder *decoder = nullptr;
  if (size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
      bytes[2] == 'N' && bytes[3] == 'G') {
    decoder = wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
  } else if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) {
    decoder = wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
  } else if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
    decoder = wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
  }
  if (!decoder) {
    if (err) *err = "WUFFS: unsupported image signature: " + uri + "\n";
    return false;
  }
  wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader(
      const_cast<uint8_t *>(bytes), size, true);
  wuffs_base__image_config config = wuffs_base__null_image_config();
  wuffs_base__status status = decoder->decode_image_config(&config, &source);
  if (status.is_error() || !config.is_valid()) {
    if (err) *err = std::string("WUFFS: image header decode failed: ") +
                    (status.message() ? status.message() : "invalid header") +
                    " (" + uri + ")\n";
    free(decoder);
    return false;
  }
  source.meta.ri = static_cast<size_t>(config.first_frame_io_position());
  const uint32_t width = config.pixcfg.width();
  const uint32_t height = config.pixcfg.height();
  size_t pixel_bytes = 0;
  if (width == 0 || height == 0 || width > uint32_t(INT_MAX) ||
      height > uint32_t(INT_MAX) ||
      !safe::mul3(size_t(width), size_t(height), size_t(4), &pixel_bytes) ||
      pixel_bytes > kMaxDecodedImageBytes) {
    if (err) *err = "WUFFS: image dimensions exceed decode limits: " + uri +
                    "\n";
    free(decoder);
    return false;
  }
  wuffs_base__pixel_config output_config = wuffs_base__null_pixel_config();
  output_config.set(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL, 0, width,
                    height);
  if (!output_config.is_valid() || output_config.pixbuf_len() != pixel_bytes) {
    if (err) *err = "WUFFS: failed to allocate output pixel format: " + uri +
                    "\n";
    free(decoder);
    return false;
  }
  std::vector<uint8_t> pixels(pixel_bytes);
  wuffs_base__pixel_buffer output = wuffs_base__null_pixel_buffer();
  status = output.set_from_slice(&output_config,
                                 wuffs_base__make_slice_u8(
                                     pixels.data(), pixels.size()));
  if (status.is_error()) {
    if (err) *err = std::string("WUFFS: output buffer setup failed: ") +
                    status.message() + "\n";
    free(decoder);
    return false;
  }
  const wuffs_base__range_ii_u64 work_range = decoder->workbuf_len();
  if (work_range.min_incl > work_range.max_incl ||
      work_range.min_incl > kMaxDecodedImageBytes) {
    if (err) *err = "WUFFS: decoder work buffer exceeds limits: " + uri +
                    "\n";
    free(decoder);
    return false;
  }
  std::vector<uint8_t> work(static_cast<size_t>(work_range.min_incl));
  status = decoder->decode_frame(
      &output, &source, WUFFS_BASE__PIXEL_BLEND__SRC,
      wuffs_base__make_slice_u8(work.data(), work.size()), nullptr);
  if (status.is_error()) {
    if (err) *err = std::string("WUFFS: image decode failed: ") +
                    status.message() + " (" + uri + ")\n";
    free(decoder);
    return false;
  }
  image->width = static_cast<int>(width);
  image->height = static_cast<int>(height);
  image->channels = 4;
  image->bpp = 8;
  image->format = Image::PixelFormat::UInt;
  image->data = std::move(pixels);
  free(decoder);
  return true;

}

bool GetImageInfoWUFF(const uint8_t *bytes, const size_t size,
                    const std::string &uri, uint32_t *width, uint32_t *height, uint32_t *channels, std::string *warn,
                    std::string *err) {
  (void)warn;
  if (!bytes || !width || !height || !channels || size == 0) {
    if (err) *err = "WUFFS: empty image input: " + uri + "\n";
    return false;
  }
  wuffs_base__image_decoder *decoder = nullptr;
  if (size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
      bytes[2] == 'N' && bytes[3] == 'G') {
    decoder = wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
  } else if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) {
    decoder = wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
  } else if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
    decoder = wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
  }
  if (!decoder) {
    if (err) *err = "WUFFS: unsupported image signature: " + uri + "\n";
    return false;
  }
  wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader(
      const_cast<uint8_t *>(bytes), size, true);
  wuffs_base__image_config config = wuffs_base__null_image_config();
  const wuffs_base__status status =
      decoder->decode_image_config(&config, &source);
  if (status.is_error() || !config.is_valid()) {
    if (err) *err = std::string("WUFFS: image header decode failed: ") +
                    (status.message() ? status.message() : "invalid header") +
                    " (" + uri + ")\n";
    free(decoder);
    return false;
  }
  source.meta.ri = static_cast<size_t>(config.first_frame_io_position());
  *width = config.pixcfg.width();
  *height = config.pixcfg.height();
  const uint32_t format = config.pixcfg.pixel_format().repr;
  *channels = (format == WUFFS_BASE__PIXEL_FORMAT__Y ||
               format == WUFFS_BASE__PIXEL_FORMAT__A)
                  ? 1
                  : (format == WUFFS_BASE__PIXEL_FORMAT__YA_NONPREMUL ||
                     format == WUFFS_BASE__PIXEL_FORMAT__YA_PREMUL)
                        ? 2
                        : (format == WUFFS_BASE__PIXEL_FORMAT__RGB ||
                           format == WUFFS_BASE__PIXEL_FORMAT__BGR)
                              ? 3
                              : 4;
  free(decoder);
  return true;
}


#else

#if !defined( LIGHTUSD_NO_BUILTIN_IMAGE_LOADER)

// Decode image(png, jpg, ...) using STB
// 16bit PNG is supported.
bool DecodeImageSTB(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image, std::string *warn,
                    std::string *err) {
  (void)warn;

  int w = 0, h = 0, comp = 0, req_comp = 0;

  unsigned char *data = nullptr;

  // force 32-bit textures for common Vulkan compatibility. It appears that
  // some GPU drivers do not support 24-bit images for Vulkan
  req_comp = 4;
  int bits = 8;

  // stb_image API accepts buffer size as `int`. Reject inputs larger than
  // INT_MAX to avoid silent size truncation.
  if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    if (err) {
      (*err) += "Image data too large (> 2GB) for stb_image decoder: " + uri + "\n";
    }
    return false;
  }

  // It is possible that the image we want to load is a 16bit per channel image
  // We are going to attempt to load it as 16bit per channel, and if it worked,
  // set the image data accodingly. We are casting the returned pointer into
  // unsigned char, because we are representing "bytes". But we are updating
  // the Image metadata to signal that this image uses 2 bytes (16bits) per
  // channel:
  if (stbi_is_16_bit_from_memory(bytes, int(size))) {
    data = reinterpret_cast<unsigned char *>(
        stbi_load_16_from_memory(bytes, int(size), &w, &h, &comp, req_comp));
    if (data) {
      bits = 16;
    }
  }

  // at this point, if data is still NULL, it means that the image wasn't
  // 16bit per channel, we are going to load it as a normal 8bit per channel
  // mage as we used to do:
  // if image cannot be decoded, ignore parsing and keep it by its path
  // don't break in this case
  // FIXME we should only enter this function if the image is embedded. If
  // `uri` references an image file, it should be left as it is. Image loading
  // should not be mandatory (to support other formats)
  if (!data) {
    data = stbi_load_from_memory(bytes, int(size), &w, &h, &comp, req_comp);
  }

  if (!data) {
    // NOTE: you can use `warn` instead of `err`
    if (err) {
      (*err) +=
          "Unknown image format. STB cannot decode image data for image: " +
          uri + "\".\n";
    }
    return false;
  }

  if ((w < 1) || (h < 1)) {
    stbi_image_free(data);
    if (err) {
      (*err) += "Invalid image data for image: " + uri + "\"\n";
    }
    return false;
  }

  image->width = w;
  image->height = h;
  image->channels = req_comp;
  image->bpp = bits;
  image->format = Image::PixelFormat::UInt;
  size_t count;
  if (!safe::mul3(w, h, req_comp, &count)) {
    return false;
  }
  size_t total_size;
  if (!safe::mul(count, size_t(bits / 8), &total_size)) {
    return false;
  }
  if (total_size > kMaxDecodedImageBytes) {
    stbi_image_free(data);
    if (err) {
      (*err) += "Decoded image exceeds the maximum allowed size for: " + uri + "\n";
    }
    return false;
  }
  // assign() copy-constructs directly from the source range, avoiding the
  // redundant zero-fill that resize() would do before the copy overwrites it
  // (meaningful for large decoded textures).
  image->data.assign(data, data + total_size);
  stbi_image_free(data);

  return true;
}

bool GetImageInfoSTB(const uint8_t *bytes, const size_t size,
                    const std::string &uri, uint32_t *width, uint32_t *height, uint32_t *channels, std::string *warn,
                    std::string *err) {
  (void)warn;
  (void)uri;
  (void)err;

  if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }

  int w = 0, h = 0, comp = 0;

  int ret = stbi_info_from_memory(bytes, int(size), &w, &h, &comp);

  if (w < 0) w = 0;
  if (h < 0) h = 0;
  if (comp < 0) comp = 0;

  if (ret == 1) {
    if (width) { (*width) = uint32_t(w); }
    if (height) { (*height) = uint32_t(h); }
    if (channels) { (*channels) = uint32_t(comp); }
    return true;
  }

  return false;
}

// Check if the image is HDR (Radiance RGBE format)
bool IsHDRFromMemory(const uint8_t *bytes, const size_t size) {
  if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return stbi_is_hdr_from_memory(bytes, int(size)) != 0;
}

// Decode HDR (Radiance RGBE) image using stbi_loadf_from_memory
// Returns float32 RGBA data
bool DecodeImageHDR(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image, std::string *warn,
                    std::string *err) {
  (void)warn;

  if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    if (err) {
      (*err) += "HDR image data too large (> 2GB): " + uri + "\n";
    }
    return false;
  }

  int w = 0, h = 0, comp = 0;

  // Request 4 channels (RGBA) for consistency with other loaders
  float *data = stbi_loadf_from_memory(bytes, int(size), &w, &h, &comp, 4);

  if (!data) {
    if (err) {
      (*err) += "Failed to decode HDR image: " + uri + " - " + stbi_failure_reason() + "\n";
    }
    return false;
  }

  if ((w < 1) || (h < 1)) {
    stbi_image_free(data);
    if (err) {
      (*err) += "Invalid HDR image data for: " + uri + "\n";
    }
    return false;
  }

  image->width = w;
  image->height = h;
  image->channels = 4;  // Always RGBA
  image->bpp = 32;      // 32-bit float per channel
  image->format = Image::PixelFormat::Float;

  // Copy float data to image buffer
  size_t dataSize;
  if (!safe::mul3(size_t(w), size_t(h), size_t(4 * sizeof(float)), &dataSize)) {
    stbi_image_free(data);
    if (err) {
      (*err) += "Integer overflow computing HDR image data size for: " + uri + "\n";
    }
    return false;
  }
  if (dataSize > kMaxDecodedImageBytes) {
    stbi_image_free(data);
    if (err) {
      (*err) += "Decoded HDR image exceeds the maximum allowed size for: " + uri + "\n";
    }
    return false;
  }
  // assign() avoids the redundant zero-fill of resize() before the copy.
  {
    const uint8_t *src = reinterpret_cast<const uint8_t *>(data);
    image->data.assign(src, src + dataSize);
  }

  stbi_image_free(data);

  return true;
}

bool GetImageInfoHDR(const uint8_t *bytes, const size_t size,
                    const std::string &uri, uint32_t *width, uint32_t *height, uint32_t *channels, std::string *warn,
                    std::string *err) {
  (void)warn;
  (void)uri;
  (void)err;

  if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }

  int w = 0, h = 0, comp = 0;

  // Use stbi_info to get HDR image dimensions
  int ret = stbi_info_from_memory(bytes, int(size), &w, &h, &comp);

  if (w < 0) w = 0;
  if (h < 0) h = 0;

  if (ret == 1) {
    if (width) { (*width) = uint32_t(w); }
    if (height) { (*height) = uint32_t(h); }
    if (channels) { (*channels) = 4; }  // Always return RGBA for HDR
    return true;
  }

  return false;
}
#endif
#endif

#if defined(LIGHTUSD_WITH_NANOIMAGE)

// Decode image (jpg, png, bmp, tga) using nanoimage.
// nanoimage is a fuzz-tested, memory-safe C image decoder.
bool DecodeImageNanoimage(const uint8_t *bytes, const size_t size,
                          const std::string &uri, Image *image,
                          std::string *warn, std::string *err) {
  (void)warn;

  char errbuf[256];
  errbuf[0] = 0;

  // JPEG: FF D8 FF
  // PNG:  89 50 4E 47
  // BMP:  42 4D
  // TGA:  no reliable magic; try last
  //
  // Try decoders in order of likelihood. Each reports its own error on mismatch.
  ni_image ni_img;
  memset(&ni_img, 0, sizeof(ni_img));
  int ok = 0;

  if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
    ok = ni_load_jpeg_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
  } else if (size >= 4 && bytes[0] == 0x89 && bytes[1] == 0x50 &&
             bytes[2] == 0x4E && bytes[3] == 0x47) {
    ok = ni_load_png_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
  } else if (size >= 2 && bytes[0] == 0x42 && bytes[1] == 0x4D) {
    ok = ni_load_bmp_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
  } else {
    // Fallback: try TGA first (no magic), then jpeg/png/bmp as last resort
    ok = ni_load_tga_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
    if (!ok) {
      ok = ni_load_png_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
    }
    if (!ok) {
      ok = ni_load_jpeg_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
    }
    if (!ok) {
      ok = ni_load_bmp_from_memory(bytes, size, &ni_img, errbuf, sizeof(errbuf));
    }
  }

  if (!ok) {
    if (err) {
      (*err) += "nanoimage cannot decode image data for: " + uri;
      if (errbuf[0]) {
        (*err) += " (" + std::string(errbuf) + ")";
      }
      (*err) += "\n";
    }
    return false;
  }

  if (ni_img.width < 1 || ni_img.height < 1) {
    ni_image_free(&ni_img);
    if (err) {
      (*err) += "Invalid image dimensions for: " + uri + "\n";
    }
    return false;
  }

  image->width = int(ni_img.width);
  image->height = int(ni_img.height);
  image->channels = int(ni_img.channels);
  image->bpp = int(ni_img.bit_depth);
  image->format = Image::PixelFormat::UInt;
  if (ni_img.data_size > kMaxDecodedImageBytes) {
    ni_image_free(&ni_img);
    if (err) {
      (*err) += "Decoded image exceeds the maximum allowed size for: " + uri + "\n";
    }
    return false;
  }
  // assign() avoids the redundant zero-fill of resize() before the copy.
  {
    const uint8_t *src = reinterpret_cast<const uint8_t *>(ni_img.data);
    image->data.assign(src, src + ni_img.data_size);
  }
  ni_image_free(&ni_img);

  return true;
}

bool GetImageInfoNanoimage(const uint8_t *bytes, const size_t size,
                           const std::string &uri, uint32_t *width,
                           uint32_t *height, uint32_t *channels,
                           std::string *warn, std::string *err) {
  // Reuse the decode path for info extraction — nanoimage has no info-only API.
  Image img;
  if (!DecodeImageNanoimage(bytes, size, uri, &img, warn, err)) {
    return false;
  }
  if (width) *width = uint32_t(img.width);
  if (height) *height = uint32_t(img.height);
  if (channels) *channels = uint32_t(img.channels);
  return true;
}

#endif

#if defined(LIGHTUSD_WITH_EXR)

#if defined(LIGHTUSD_EXR_V3)

// Wrapper around tinyexr v3's EXR_OK() macro, which uses an old-style cast
// (`((int)(r) >= 0)`) that trips -Wold-style-cast at every expansion site. The
// macro lives in the external header and must not be edited, so route the check
// through this static_cast-based helper instead.
static inline bool ExrOk(exr_result r) { return static_cast<int>(r) >= 0; }

// exr_allocator backed by a MemoryBudgetManager: bounds the total memory the
// v3 C decoder allocates (reader scratch + planar channel buffers) for a single
// decode, so a crafted EXR fails cleanly (NULL -> EXR_ERROR_OUT_OF_MEMORY)
// rather than OOM-aborting. Each block carries a 16-byte size header (the free
// callback only receives the pointer); the 16-byte offset preserves the
// 16-byte alignment malloc already provides.
struct ExrBudgetAllocator {
  lightusd::MemoryBudgetManager mgr;
  explicit ExrBudgetAllocator(uint64_t budget) : mgr(budget) {}

#if defined(__clang__) || defined(__GNUC__)
  __attribute__((malloc, alloc_size(2)))
#endif
  static void *Alloc(void *user, size_t size) {
    auto *self = static_cast<ExrBudgetAllocator *>(user);
    const size_t kHdr = 16;
    if (size > SIZE_MAX - kHdr) return nullptr;
    const size_t total = size + kHdr;
    if (!self->mgr.Reserve(total)) return nullptr;
    void *base = std::malloc(total);
    if (!base) {
      self->mgr.Release(total);
      return nullptr;
    }
    std::memcpy(base, &total, sizeof(size_t));
    return static_cast<uint8_t *>(base) + kHdr;
  }
  static void Free(void *user, void *ptr) {
    if (!ptr) return;
    auto *self = static_cast<ExrBudgetAllocator *>(user);
    const size_t kHdr = 16;
    void *base = static_cast<uint8_t *>(ptr) - kHdr;
    size_t total = 0;
    std::memcpy(&total, base, sizeof(size_t));
    self->mgr.Release(total);
    std::free(base);
  }
  exr_allocator handle() {
    exr_allocator a;
    a.user = this;
    a.alloc = &ExrBudgetAllocator::Alloc;
    a.free = &ExrBudgetAllocator::Free;
    return a;
  }
};

// Locate the standard color channels (R/G/B/A and luminance Y) by name in a
// v3 exr_part's name-sorted channel list. Returns false if none are present.
static bool ExrChannelNameIs(const char *name, const char *channel) {
  if (!name || !channel) return false;
  const size_t name_len = std::strlen(name);
  const size_t channel_len = std::strlen(channel);
  if (name_len < channel_len) return false;
  if (std::strcmp(name + name_len - channel_len, channel) != 0) return false;
  return name_len == channel_len || name[name_len - channel_len - 1] == '.';
}

static bool ExrFindRGBAY(const exr_part *part, int *idxR, int *idxG, int *idxB,
                         int *idxA, int *idxY) {
  *idxR = *idxG = *idxB = *idxA = *idxY = -1;
  for (int c = 0; c < part->header.num_channels; c++) {
    const char *n = part->header.channels[c].name;
    if (ExrChannelNameIs(n, "R")) *idxR = c;
    else if (ExrChannelNameIs(n, "G")) *idxG = c;
    else if (ExrChannelNameIs(n, "B")) *idxB = c;
    else if (ExrChannelNameIs(n, "A")) *idxA = c;
    else if (ExrChannelNameIs(n, "Y")) *idxY = c;
  }
  return (*idxR >= 0 || *idxG >= 0 || *idxB >= 0 || *idxY >= 0);
}

bool DecodeImageEXR(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image,
                    std::string *err) {
  exr_image img;
  std::memset(&img, 0, sizeof(img));
  ExrBudgetAllocator budget(kMaxDecodedImageBytes);
  exr_allocator alloc = budget.handle();
  exr_result r = exr_load_from_memory(bytes, size, &alloc, &img);
  if (!ExrOk(r)) {
    (*err) += "Failed to load EXR image: " + uri + " (" +
              std::string(exr_result_string(r)) + ")\n";
    return false;
  }
  if (img.num_parts < 1 || img.parts == nullptr) {
    exr_image_free(&img);
    (*err) += "EXR has no image parts: " + uri + "\n";
    return false;
  }
  const exr_part *part = &img.parts[0];
  if (part->is_deep || part->images == nullptr || part->width <= 0 ||
      part->height <= 0) {
    exr_image_free(&img);
    (*err) += "EXR part is deep or empty: " + uri + "\n";
    return false;
  }

  int idxR, idxG, idxB, idxA, idxY;
  if (!ExrFindRGBAY(part, &idxR, &idxG, &idxB, &idxA, &idxY)) {
    exr_image_free(&img);
    (*err) += "EXR has no R/G/B/Y channel: " + uri + "\n";
    return false;
  }

  size_t npix, total_size;
  if (!safe::mul(size_t(part->width), size_t(part->height), &npix) ||
      !safe::mul(npix, size_t(4 * sizeof(float)), &total_size)) {
    exr_image_free(&img);
    return false;
  }
  if (total_size > kMaxDecodedImageBytes) {
    exr_image_free(&img);
    (*err) += "Decoded EXR image exceeds the maximum allowed size for: " + uri + "\n";
    return false;
  }

  // Read channel `idx` element `p` as float, honoring its native pixel type.
  auto getf = [&](int idx, size_t p) -> float {
    if (idx < 0) return 0.0f;
    const void *base = part->images[idx];
    if (!base) return 0.0f;
    switch (part->header.channels[idx].pixel_type) {
      case EXR_PIXEL_HALF: {
        uint16_t h = reinterpret_cast<const uint16_t *>(base)[p];
        float f;
        exr_half_to_float(&h, &f, 1);
        return f;
      }
      case EXR_PIXEL_FLOAT:
        return reinterpret_cast<const float *>(base)[p];
      case EXR_PIXEL_UINT:
        return float(reinterpret_cast<const uint32_t *>(base)[p]);
    }
    return 0.0f;
  };

  image->width = part->width;
  image->height = part->height;
  image->channels = 4;  // RGBA
  image->bpp = 32;      // fp32
  image->format = Image::PixelFormat::Float;
  image->data.resize(total_size);
  float *out = reinterpret_cast<float *>(image->data.data());
  for (size_t p = 0; p < npix; p++) {
    out[p * 4 + 0] = idxR >= 0 ? getf(idxR, p) : getf(idxY, p);
    out[p * 4 + 1] = idxG >= 0 ? getf(idxG, p) : getf(idxY, p);
    out[p * 4 + 2] = idxB >= 0 ? getf(idxB, p) : getf(idxY, p);
    out[p * 4 + 3] = idxA >= 0 ? getf(idxA, p) : 1.0f;
  }

  exr_image_free(&img);
  return true;
}

#else  // legacy v1 backend

static bool ExrChannelNameIs(const char *name, const char *channel) {
  if (!name || !channel) return false;
  const size_t name_len = std::strlen(name);
  const size_t channel_len = std::strlen(channel);
  return name_len >= channel_len &&
         std::strcmp(name + name_len - channel_len, channel) == 0 &&
         (name_len == channel_len ||
          name[name_len - channel_len - 1] == '.');
}

static bool DecodeImageEXRUInt(const uint8_t *bytes, size_t size,
                               const std::string &uri, Image *image,
                               std::string *err) {
  EXRVersion version;
  if (ParseEXRVersionFromMemory(&version, bytes, size) != TINYEXR_SUCCESS ||
      version.multipart || version.tiled || version.non_image) return false;
  EXRHeader header;
  InitEXRHeader(&header);
  const char *exrerr = nullptr;
  if (ParseEXRHeaderFromMemory(&header, &version, bytes, size, &exrerr) !=
      TINYEXR_SUCCESS) {
    if (exrerr) FreeEXRErrorMessage(exrerr);
    FreeEXRHeader(&header);
    return false;
  }
  bool all_uint = header.num_channels > 0;
  for (int c = 0; c < header.num_channels; ++c) {
    if (header.pixel_types[c] != TINYEXR_PIXELTYPE_UINT) {
      all_uint = false;
      break;
    }
    header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_UINT;
  }
  if (!all_uint) {
    FreeEXRHeader(&header);
    return false;
  }
  EXRImage exr;
  InitEXRImage(&exr);
  if (LoadEXRImageFromMemory(&exr, &header, bytes, size, &exrerr) !=
          TINYEXR_SUCCESS ||
      !exr.images || exr.width <= 0 || exr.height <= 0) {
    if (exrerr) FreeEXRErrorMessage(exrerr);
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }
  auto is_channel = [](const char *name, const char *channel) {
    if (!name || !channel) return false;
    const size_t n = std::strlen(name), c = std::strlen(channel);
    return n >= c && std::strcmp(name + n - c, channel) == 0 &&
           (n == c || name[n - c - 1] == '.');
  };
  int idx_r = -1, idx_g = -1, idx_b = -1, idx_a = -1, idx_y = -1;
  for (int c = 0; c < header.num_channels; ++c) {
    const char *name = header.channels[c].name;
    if (is_channel(name, "R")) idx_r = c;
    else if (is_channel(name, "G")) idx_g = c;
    else if (is_channel(name, "B")) idx_b = c;
    else if (is_channel(name, "A")) idx_a = c;
    else if (is_channel(name, "Y")) idx_y = c;
  }
  if (idx_r < 0 && idx_g < 0 && idx_b < 0 && idx_y < 0) {
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }
  size_t pixels = 0, total = 0;
  if (!safe::mul(size_t(exr.width), size_t(exr.height), &pixels) ||
      !safe::mul(pixels, size_t(4 * sizeof(uint32_t)), &total) ||
      total > kMaxDecodedImageBytes) {
    if (err) *err += "Decoded UINT EXR image exceeds limits: " + uri + "\n";
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }
  auto channel = [&](int index) -> const uint32_t * {
    return index >= 0 ? reinterpret_cast<const uint32_t *>(exr.images[index])
                      : nullptr;
  };
  const uint32_t *r = channel(idx_r), *g = channel(idx_g), *b = channel(idx_b),
                 *a = channel(idx_a), *y = channel(idx_y);
  image->width = exr.width;
  image->height = exr.height;
  image->channels = 4;
  image->bpp = 32;
  image->format = Image::PixelFormat::UInt;
  image->data.resize(total);
  uint32_t *out = reinterpret_cast<uint32_t *>(image->data.data());
  for (size_t p = 0; p < pixels; ++p) {
    out[p * 4 + 0] = r ? r[p] : (y ? y[p] : 0u);
    out[p * 4 + 1] = g ? g[p] : (y ? y[p] : 0u);
    out[p * 4 + 2] = b ? b[p] : (y ? y[p] : 0u);
    out[p * 4 + 3] = a ? a[p] : 0xffffffffu;
  }
  FreeEXRImage(&exr);
  FreeEXRHeader(&header);
  return true;
}

bool DecodeImageEXR(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image,
                    std::string *err) {
  std::string native_err;
  if (DecodeImageEXRUInt(bytes, size, uri, image, &native_err)) {
    return true;
  }

  float *rgba = nullptr;
  int width;
  int height;
  const char *exrerr = nullptr;
  // LoadEXRFromMemory always load EXR image as fp32 x RGBA
  int ret = LoadEXRFromMemory(&rgba, &width, &height, bytes, size, &exrerr);

  if (exrerr) {
    (*err) += std::string(exrerr);

    FreeEXRErrorMessage(exrerr);
  }

  // LoadEXRFromMemory returns TINYEXR_SUCCESS (0) on success and a negative
  // error code otherwise, so test against TINYEXR_SUCCESS (the previous `!ret`
  // inverted this and reported failure on every successful load).
  if (ret != TINYEXR_SUCCESS) {
    (*err) += "Failed to load EXR image: " + uri + "\n";
    return false;
  }
  if (rgba == nullptr) {
    (*err) += "EXR decode returned no pixel data: " + uri + "\n";
    return false;
  }

  image->width = width;
  image->height = height;
  image->channels = 4;  // RGBA
  image->bpp = 32;      // fp32
  image->format = Image::PixelFormat::Float;
  size_t count;
  if (!safe::mul3(width, height, size_t(4), &count)) {
    return false;
  }
  size_t total_size;
  if (!safe::mul(count, sizeof(float), &total_size)) {
    return false;
  }
  if (total_size > kMaxDecodedImageBytes) {
    free(rgba);
    (*err) += "Decoded EXR image exceeds the maximum allowed size for: " + uri + "\n";
    return false;
  }
  image->data.resize(total_size);
  memcpy(image->data.data(), rgba, total_size);

  free(rgba);

  return true;
}

#endif  // LIGHTUSD_EXR_V3

#endif

#if defined(LIGHTUSD_WITH_TIFF)

#if defined(LIGHTUSD_WITH_LIBTIFF)
struct TiffMemory {
  const uint8_t *data = nullptr;
  toff_t size = 0;
  toff_t offset = 0;
};

tsize_t TiffRead(thandle_t handle, tdata_t dst, tsize_t count) {
  TiffMemory *m = static_cast<TiffMemory *>(handle);
  if (!m || count <= 0 || m->offset >= m->size) return 0;
  const toff_t available = m->size - m->offset;
  const toff_t n = (std::min)(available, static_cast<toff_t>(count));
  std::memcpy(dst, m->data + m->offset, static_cast<size_t>(n));
  m->offset += n;
  return static_cast<tsize_t>(n);
}

tsize_t TiffWrite(thandle_t, tdata_t, tsize_t) { return 0; }

toff_t TiffSeek(thandle_t handle, toff_t offset, int whence) {
  TiffMemory *m = static_cast<TiffMemory *>(handle);
  if (!m) return static_cast<toff_t>(-1);
  toff_t next = 0;
  if (whence == SEEK_SET) next = offset;
  else if (whence == SEEK_CUR) next = m->offset + offset;
  else if (whence == SEEK_END) next = m->size + offset;
  else return static_cast<toff_t>(-1);
  if (next < 0 || next > m->size) return static_cast<toff_t>(-1);
  m->offset = next;
  return next;
}

int TiffClose(thandle_t) { return 0; }
toff_t TiffSize(thandle_t handle) {
  const TiffMemory *m = static_cast<const TiffMemory *>(handle);
  return m ? m->size : 0;
}
int TiffMap(thandle_t, tdata_t *, toff_t *) { return 0; }
void TiffUnmap(thandle_t, tdata_t, toff_t) {}

TIFF *OpenMemoryTiff(const uint8_t *bytes, size_t size, TiffMemory *memory) {
  if (!bytes || size > static_cast<size_t>((std::numeric_limits<toff_t>::max)())) return nullptr;
  memory->data = bytes;
  memory->size = static_cast<toff_t>(size);
  memory->offset = 0;
  TIFFSetWarningHandler(nullptr);
  return TIFFClientOpen("lightusd-memory", "r", memory, TiffRead, TiffWrite,
                        TiffSeek, TiffClose, TiffSize, TiffMap, TiffUnmap);
}

bool FindLargestTiffDirectory(TIFF *tif, uint16_t *directory,
                              uint32_t *width, uint32_t *height) {
  if (!tif || !directory || !width || !height) return false;
  uint16_t best = 0;
  uint32_t bestWidth = 0, bestHeight = 0;
  for (uint16_t d = 0; d < (std::numeric_limits<uint16_t>::max)(); ++d) {
    if (!TIFFSetDirectory(tif, d)) break;
    uint32_t w = 0, h = 0;
    if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w) &&
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h) &&
        uint64_t(w) * h > uint64_t(bestWidth) * bestHeight) {
      best = d; bestWidth = w; bestHeight = h;
    }
  }
  if (!bestWidth || !bestHeight || !TIFFSetDirectory(tif, best)) return false;
  *directory = best; *width = bestWidth; *height = bestHeight;
  return true;
}

bool DecodeTiledTIFF(const uint8_t *bytes, size_t size, const std::string &uri,
                     Image *image, std::string *err) {
  TiffMemory memory;
  TIFF *tif = OpenMemoryTiff(bytes, size, &memory);
  if (!tif) return false;
  uint16_t directory = 0;
  uint32_t width = 0, height = 0;
  bool ok = FindLargestTiffDirectory(tif, &directory, &width, &height);
  uint16_t bits = 0, samples = 0, planar = PLANARCONFIG_CONTIG;
  uint16_t sampleFormat = SAMPLEFORMAT_UINT;
  uint32_t tileWidth = 0, tileHeight = 0;
  ok = ok && TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits) &&
       TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples) &&
       TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar) &&
       TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat) &&
       TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tileWidth) &&
       TIFFGetField(tif, TIFFTAG_TILELENGTH, &tileHeight) &&
       samples >= 1 && samples <= 4 && tileWidth && tileHeight;
  if (!ok || (bits != 8 && bits != 16 && bits != 32) ||
      (sampleFormat != SAMPLEFORMAT_UINT &&
       sampleFormat != SAMPLEFORMAT_INT &&
       sampleFormat != SAMPLEFORMAT_IEEEFP) ||
      (planar != PLANARCONFIG_CONTIG && planar != PLANARCONFIG_SEPARATE)) {
    TIFFClose(tif);
    return false;
  }
  size_t pixels = 0, total = 0;
  const size_t bytesPerSample = bits / 8;
  ok = safe::mul(static_cast<size_t>(width), height, &pixels) &&
       safe::mul(pixels, samples, &total) &&
       safe::mul(total, bytesPerSample, &total) &&
       total <= kMaxDecodedImageBytes;
  if (!ok) { TIFFClose(tif); return false; }
  const tsize_t tileBytes = TIFFTileSize(tif);
  if (tileBytes <= 0) { TIFFClose(tif); return false; }
  const size_t sourceTileRowBytes =
      size_t(tileWidth) * bytesPerSample *
      (planar == PLANARCONFIG_CONTIG ? samples : 1);
  size_t sourceTileBytes = 0;
  if (sourceTileRowBytes == 0 ||
      !safe::mul(sourceTileRowBytes, size_t(tileHeight), &sourceTileBytes) ||
      static_cast<size_t>(tileBytes) < sourceTileBytes) {
    TIFFClose(tif);
    return false;
  }
  std::vector<uint8_t> tile(static_cast<size_t>(tileBytes));
  image->width = static_cast<int>(width);
  image->height = static_cast<int>(height);
  image->channels = static_cast<int>(samples);
  image->bpp = bits;
  image->format = sampleFormat == SAMPLEFORMAT_UINT
                      ? Image::PixelFormat::UInt
                      : sampleFormat == SAMPLEFORMAT_INT
                            ? Image::PixelFormat::Int
                            : Image::PixelFormat::Float;
  image->data.assign(total, 0);
  // Avoid width + tileWidth - 1 overflow for hostile, near-uint32 TIFF
  // dimensions. The decoded-byte limit above still rejects impractical
  // rasters, but arithmetic must remain defined before that decision is used.
  const uint32_t xTiles = width / tileWidth + (width % tileWidth != 0 ? 1u : 0u);
  const uint32_t yTiles = height / tileHeight + (height % tileHeight != 0 ? 1u : 0u);
  for (uint16_t sample = 0; sample < samples; ++sample) {
    if (planar == PLANARCONFIG_CONTIG && sample != 0) break;
    for (uint32_t ty = 0; ty < yTiles; ++ty) {
      for (uint32_t tx = 0; tx < xTiles; ++tx) {
        const uint32_t x0 = tx * tileWidth, y0 = ty * tileHeight;
        if (TIFFReadTile(tif, tile.data(), x0, y0, 0,
                         planar == PLANARCONFIG_SEPARATE ? sample : 0) < 0) {
          ok = false; break;
        }
        const uint32_t rows = (std::min)(tileHeight, height - y0);
        const uint32_t cols = (std::min)(tileWidth, width - x0);
        for (uint32_t y = 0; y < rows; ++y) {
          for (uint32_t x = 0; x < cols; ++x) {
            const size_t dstSample =
                (size_t(y0 + y) * width + x0 + x) * samples +
                (planar == PLANARCONFIG_SEPARATE ? sample : 0);
            const size_t srcSample = size_t(y) * tileWidth + x;
            const size_t srcOffset =
                (planar == PLANARCONFIG_CONTIG
                     ? srcSample * samples
                     : srcSample) * bytesPerSample;
            if (planar == PLANARCONFIG_SEPARATE) {
              std::memcpy(image->data.data() + dstSample * bytesPerSample,
                          tile.data() + srcOffset, bytesPerSample);
            } else {
              std::memcpy(image->data.data() + dstSample * bytesPerSample,
                          tile.data() + srcOffset,
                          bytesPerSample * samples);
            }
          }
        }
      }
      if (!ok) break;
    }
    if (!ok) break;
  }
  TIFFClose(tif);
  if (!ok) {
    image->data.clear();
    if (err) *err += "Failed to decode tiled TIFF: " + uri + "\n";
  }
  return ok;
}

bool GetTiledTIFFInfo(const uint8_t *bytes, size_t size, uint32_t *width,
                      uint32_t *height, uint32_t *channels) {
  TiffMemory memory;
  TIFF *tif = OpenMemoryTiff(bytes, size, &memory);
  if (!tif) return false;
  uint16_t directory = 0, samples = 0;
  bool ok = FindLargestTiffDirectory(tif, &directory, width, height) &&
            TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFClose(tif);
  if (ok) *channels = samples;
  return ok;
}

// Decode strip/scanline TIFFs through libtiff. TinyDNG intentionally handles
// the common contiguous path, but libtiff is needed for planar images and for
// rows whose stored stride includes padding. TIFFReadScanline normalizes the
// file byte order and compression details before we interleave the result.
bool DecodeScanlineTIFF(const uint8_t *bytes, size_t size,
                        const std::string &uri, Image *image,
                        std::string *err) {
  TiffMemory memory;
  TIFF *tif = OpenMemoryTiff(bytes, size, &memory);
  if (!tif) return false;
  uint16_t directory = 0;
  uint32_t width = 0, height = 0;
  bool ok = FindLargestTiffDirectory(tif, &directory, &width, &height);
  uint16_t bits = 0, samples = 0, planar = PLANARCONFIG_CONTIG;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  ok = ok && !TIFFIsTiled(tif) &&
       TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits) &&
       TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples) &&
       TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar) &&
       TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format) &&
       samples >= 1 && samples <= 4 &&
       (bits == 8 || bits == 16 || bits == 32) &&
       (sample_format == SAMPLEFORMAT_UINT ||
        sample_format == SAMPLEFORMAT_INT ||
        sample_format == SAMPLEFORMAT_IEEEFP) &&
       (planar == PLANARCONFIG_CONTIG || planar == PLANARCONFIG_SEPARATE);
  size_t pixel_count = 0, row_bytes = 0, total = 0;
  const size_t bytes_per_sample = bits / 8;
  ok = ok && safe::mul(static_cast<size_t>(width), samples, &pixel_count) &&
       safe::mul(pixel_count, bytes_per_sample, &row_bytes) &&
       safe::mul(row_bytes, height, &total) && total <= kMaxDecodedImageBytes;
  if (!ok) {
    TIFFClose(tif);
    return false;
  }
  const tsize_t scanline_size = TIFFScanlineSize(tif);
  if (scanline_size <= 0 ||
      static_cast<size_t>(scanline_size) <
          (planar == PLANARCONFIG_CONTIG
               ? row_bytes
               : size_t(width) * bytes_per_sample)) {
    TIFFClose(tif);
    return false;
  }
  std::vector<uint8_t> scanline(static_cast<size_t>(scanline_size));
  image->width = static_cast<int>(width);
  image->height = static_cast<int>(height);
  image->channels = static_cast<int>(samples);
  image->bpp = bits;
  image->format = sample_format == SAMPLEFORMAT_UINT
                      ? Image::PixelFormat::UInt
                      : sample_format == SAMPLEFORMAT_INT
                            ? Image::PixelFormat::Int
                            : Image::PixelFormat::Float;
  image->data.assign(total, 0);
  for (uint32_t y = 0; y < height && ok; ++y) {
    const uint16_t plane_count =
        planar == PLANARCONFIG_SEPARATE ? samples : uint16_t(1);
    for (uint16_t sample = 0; sample < plane_count; ++sample) {
      if (TIFFReadScanline(tif, scanline.data(), y,
                           planar == PLANARCONFIG_SEPARATE ? sample : 0) < 0) {
        ok = false;
        break;
      }
      for (uint32_t x = 0; x < width; ++x) {
        const size_t src = size_t(x) * bytes_per_sample *
                           (planar == PLANARCONFIG_CONTIG ? samples : 1);
        const size_t dst_sample =
            (size_t(y) * width * samples) +
            (size_t(x) * samples) +
            (planar == PLANARCONFIG_SEPARATE ? sample : 0);
        std::memcpy(image->data.data() + dst_sample * bytes_per_sample,
                    scanline.data() + src, bytes_per_sample);
      }
    }
  }
  TIFFClose(tif);
  if (!ok) {
    image->data.clear();
    if (err) *err += "Failed to decode scanline TIFF: " + uri + "\n";
  }
  return ok;
}
#endif

// Expand a contiguous TIFF bit stream whose samples are not byte aligned.
// TinyDNG exposes these samples in their stored packed form. Image uses a
// byte-addressable bpp contract, so normalize them to unsigned/signed 16-bit
// samples while retaining the original numeric range.
static bool ExpandPackedTIFFSamples(const tinydng::DNGImage &source,
                                    std::vector<uint8_t> *expanded,
                                    std::string *err) {
  const int bps = source.bits_per_sample;
  if (bps != 10 && bps != 12 && bps != 14) return true;
  if (source.sample_format != tinydng::SAMPLEFORMAT_UINT &&
      source.sample_format != tinydng::SAMPLEFORMAT_INT) {
    if (err) *err += "Packed TIFF samples must use integer sample format.\n";
    return false;
  }

  size_t sample_count = 0;
  size_t bit_count = 0;
  size_t packed_bytes = 0;
  if (!safe::mul3(size_t(source.width), size_t(source.height),
                  size_t(source.samples_per_pixel), &sample_count) ||
      !safe::mul(sample_count, size_t(bps), &bit_count) ||
      !safe::add(bit_count, size_t(7), &bit_count)) {
    if (err) *err += "Packed TIFF sample dimensions overflow.\n";
    return false;
  }
  packed_bytes = bit_count / 8;
  if (packed_bytes != source.data.size()) {
    if (err) *err += "Packed TIFF data has unsupported row padding or layout.\n";
    return false;
  }

  size_t expanded_bytes = 0;
  if (!safe::mul(sample_count, sizeof(uint16_t), &expanded_bytes)) {
    if (err) *err += "Packed TIFF expanded size overflow.\n";
    return false;
  }
  expanded->resize(expanded_bytes);
  const bool planar = source.planar_configuration == 2;
  if (source.planar_configuration != 1 && !planar) {
    if (err) *err += "Packed TIFF has an invalid planar configuration.\n";
    return false;
  }
  const size_t pixels_per_plane = size_t(source.width) * size_t(source.height);
  for (size_t output_index = 0; output_index < sample_count; ++output_index) {
    // Packed planar TIFF stores one complete sample plane after another,
    // while Image always exposes interleaved pixel/channel samples.
    const size_t source_index =
        planar ? (output_index % size_t(source.samples_per_pixel)) *
                         pixels_per_plane +
                     output_index / size_t(source.samples_per_pixel)
               : output_index;
    const size_t first_bit = source_index * size_t(bps);
    uint32_t sample = 0;
    for (int bit = 0; bit < bps; ++bit) {
      const size_t source_bit = first_bit + size_t(bit);
      sample = (sample << 1) |
               ((source.data[source_bit / 8] >> (7 - source_bit % 8)) & 1u);
    }
    if (source.sample_format == tinydng::SAMPLEFORMAT_INT &&
        (sample & (uint32_t(1) << (bps - 1))) != 0) {
      sample |= ~((uint32_t(1) << bps) - 1u);
    }
    const uint16_t value = static_cast<uint16_t>(sample);
    std::memcpy(expanded->data() + output_index * sizeof(value), &value,
                sizeof(value));
  }
  return true;
}

bool DecodeImageTIFF(const uint8_t *bytes, const size_t size,
                    const std::string &uri, Image *image,
                    std::string *err) {


  std::vector<tinydng::FieldInfo> custom_fields; // no custom fields
  std::vector<tinydng::DNGImage> images;

  std::string warn;
  std::string dngerr;

  bool ret = tinydng::LoadDNGFromMemory(reinterpret_cast<const char *>(bytes), size, custom_fields, &images, &warn, &dngerr);

  if (!dngerr.empty()) {
    (*err) += dngerr;
  }

  if (!ret) {
#if defined(LIGHTUSD_WITH_LIBTIFF)
    if (DecodeScanlineTIFF(bytes, size, uri, image, err)) return true;
    if (DecodeTiledTIFF(bytes, size, uri, image, err)) return true;
#endif
    (*err) += "Failed to load TIFF/DNG image: " + uri + "\n";
    return false;
  }

  if (images.empty()) {
    (*err) += "No images decoded from TIFF/DNG: " + uri + "\n";
    return false;
  }

  // The single-image API selects the largest decoded directory
  // deterministically. LoadImageLayersFromMemory below preserves every
  // directory for callers that need multi-image TIFF/DNG data.
  size_t largest = 0;
  int largest_width = images[0].width;
  for (size_t i = 1; i < images.size(); i++) {
    if (largest_width < images[i].width) {
      largest = i;
      largest_width = images[i].width;
     }
  }

  size_t spp = size_t(images[largest].samples_per_pixel);
  size_t bps = size_t(images[largest].bits_per_sample);

  if (spp > 4) {
    (*err) += "Samples per pixel must be 0 ~ 4, but got " + std::to_string(spp) + " for image: " + uri + "\n";
    return false;
  }

  const bool packed_integer = (bps == 10) || (bps == 12) || (bps == 14);
  if ((bps == 8) || packed_integer || (bps == 16) || (bps == 32)) {
    // ok
  } else {
    (*err) += "Invalid or unsupported bits per sample " + std::to_string(bps) + " for image: " + uri + "\n";
    return false;
  }

  auto sample_format = images[largest].sample_format;
  if (sample_format == tinydng::SAMPLEFORMAT_UINT) {
    image->format = Image::PixelFormat::UInt;
  } else if (sample_format == tinydng::SAMPLEFORMAT_INT) {
    image->format = Image::PixelFormat::Int;
  } else if (sample_format == tinydng::SAMPLEFORMAT_IEEEFP) {
    image->format = Image::PixelFormat::Float;
  } else {
    (*err) += "Invalid Sample format for image: " + uri + "\n";
    return false;
  }

  image->width = images[largest].width;
  image->height = images[largest].height;
  image->channels = int(spp);
  image->bpp = packed_integer ? 16 : int(bps);

  if (packed_integer) {
    std::vector<uint8_t> expanded;
    if (!ExpandPackedTIFFSamples(images[largest], &expanded, err)) {
      (*err) += "Failed to normalize packed TIFF/DNG image: " + uri + "\n";
      return false;
    }
    image->data = std::move(expanded);
  } else {
    image->data.swap(images[largest].data);
  }

  return true;
}

static bool DecodeTIFFImageRecord(const tinydng::DNGImage &source,
                                  const std::string &uri, Image *image,
                                  std::string *err) {
  if (!image) {
    if (err) *err += "TIFF image output is null.\n";
    return false;
  }
  const size_t spp = size_t(source.samples_per_pixel);
  const size_t bps = size_t(source.bits_per_sample);
  if (spp == 0 || spp > 4) {
    if (err) *err += "Samples per pixel must be 1 ~ 4 for image: " + uri + "\n";
    return false;
  }
  const bool packed_integer = bps == 10 || bps == 12 || bps == 14;
  if (bps != 8 && !packed_integer && bps != 16 && bps != 32) {
    if (err) *err += "Invalid or unsupported bits per sample " +
                    std::to_string(bps) + " for image: " + uri + "\n";
    return false;
  }
  if (source.sample_format == tinydng::SAMPLEFORMAT_UINT) {
    image->format = Image::PixelFormat::UInt;
  } else if (source.sample_format == tinydng::SAMPLEFORMAT_INT) {
    image->format = Image::PixelFormat::Int;
  } else if (source.sample_format == tinydng::SAMPLEFORMAT_IEEEFP) {
    image->format = Image::PixelFormat::Float;
  } else {
    if (err) *err += "Invalid sample format for image: " + uri + "\n";
    return false;
  }
  image->width = source.width;
  image->height = source.height;
  image->channels = static_cast<int>(spp);
  image->bpp = packed_integer ? 16 : static_cast<int>(bps);
  if (packed_integer) {
    std::vector<uint8_t> expanded;
    if (!ExpandPackedTIFFSamples(source, &expanded, err)) return false;
    image->data = std::move(expanded);
  } else {
    image->data = source.data;
  }
  return true;
}

#endif

}  // namespace

nonstd::expected<std::vector<ImageResult>, std::string>
LoadImageLayersFromMemory(const uint8_t *addr, size_t sz,
                          const std::string &uri) {
#if defined(LIGHTUSD_WITH_TIFF)
  if (!addr || sz == 0) {
    return nonstd::make_unexpected("TIFF layer input is empty: " + uri + "\n");
  }
  std::vector<tinydng::FieldInfo> custom_fields;
  std::vector<tinydng::DNGImage> images;
  std::string warn;
  std::string dngerr;
  if (!tinydng::LoadDNGFromMemory(reinterpret_cast<const char *>(addr), sz,
                                  custom_fields, &images, &warn, &dngerr) ||
      images.empty()) {
    return nonstd::make_unexpected("Failed to load TIFF/DNG layers: " + uri +
                                   "\n" + dngerr);
  }
  std::vector<ImageResult> result;
  result.reserve(images.size());
  for (const auto &source : images) {
    ImageResult layer;
    layer.warning = warn;
    std::string err;
    if (!DecodeTIFFImageRecord(source, uri, &layer.image, &err)) {
      return nonstd::make_unexpected(err);
    }
    result.push_back(std::move(layer));
  }
  return result;
#else
  (void)addr;
  (void)sz;
  return nonstd::make_unexpected(
      "TIFF support is disabled; cannot load image layers: " + uri + "\n");
#endif
}

#if defined(LIGHTUSD_WITH_EXR)
#if defined(LIGHTUSD_EXR_V3)
bool DecodeImageEXRHalf(const uint8_t *bytes, size_t size,
                        const std::string &uri, Image *image,
                        std::string *err) {
  (void)uri;
  (void)err;
  exr_image img;
  std::memset(&img, 0, sizeof(img));
  ExrBudgetAllocator budget(kMaxDecodedImageBytes);
  exr_allocator alloc = budget.handle();
  if (!ExrOk(exr_load_from_memory(bytes, size, &alloc, &img))) {
    return false;  // not EXR / unparseable -> caller falls back
  }
  // Contract: single-part scanline only; multipart/tiled/deep -> fp32 path.
  if (img.num_parts != 1 || img.parts == nullptr) {
    exr_image_free(&img);
    return false;
  }
  const exr_part *part = &img.parts[0];
  if (part->is_deep || part->header.part_type != EXR_PART_SCANLINE ||
      part->images == nullptr || part->width <= 0 || part->height <= 0) {
    exr_image_free(&img);
    return false;
  }
  // Only take the half path when EVERY channel is stored HALF in the file.
  if (part->header.num_channels <= 0) {
    exr_image_free(&img);
    return false;
  }
  for (int c = 0; c < part->header.num_channels; c++) {
    if (part->header.channels[c].pixel_type != EXR_PIXEL_HALF) {
      exr_image_free(&img);
      return false;  // float/uint channels present -> fp32 path
    }
  }

  int idxR, idxG, idxB, idxA, idxY;
  if (!ExrFindRGBAY(part, &idxR, &idxG, &idxB, &idxA, &idxY)) {
    exr_image_free(&img);
    return false;  // no standard color channel -> fp32 path
  }

  size_t npix, total;
  if (!safe::mul(size_t(part->width), size_t(part->height), &npix) ||
      !safe::mul(npix, size_t(4 * 2), &total)) {  // RGBA * 2 bytes/half
    exr_image_free(&img);
    return false;
  }
  if (total > kMaxDecodedImageBytes) {
    // Oversized: bail so the caller falls back to the fp32 path, which emits a
    // clean over-size error of its own.
    exr_image_free(&img);
    return false;
  }

  image->width = part->width;
  image->height = part->height;
  image->channels = 4;
  image->bpp = 16;
  image->format = Image::PixelFormat::Float;
  image->data.resize(total);
  uint16_t *out = reinterpret_cast<uint16_t *>(image->data.data());

  auto chan = [&](int idx) -> const uint16_t * {
    return idx >= 0 ? reinterpret_cast<const uint16_t *>(part->images[idx])
                    : nullptr;
  };
  const uint16_t *R = chan(idxR), *G = chan(idxG), *B = chan(idxB),
                 *A = chan(idxA), *Y = chan(idxY);
  const uint16_t kOne = 0x3C00;  // half 1.0
  const uint16_t kZero = 0x0000;
  for (size_t p = 0; p < npix; p++) {
    out[p * 4 + 0] = R ? R[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 1] = G ? G[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 2] = B ? B[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 3] = A ? A[p] : kOne;
  }

  exr_image_free(&img);
  return true;
}

#else  // legacy v1 backend

bool DecodeImageEXRHalf(const uint8_t *bytes, size_t size,
                        const std::string &uri, Image *image,
                        std::string *err) {
  (void)uri;
  (void)err;
  EXRVersion version;
  if (ParseEXRVersionFromMemory(&version, bytes, size) != TINYEXR_SUCCESS) {
    return false;  // not EXR / unparseable -> caller falls back
  }
  if (version.multipart || version.tiled || version.non_image) {
    return false;  // unsupported layout -> fp32 path
  }

  EXRHeader header;
  InitEXRHeader(&header);
  const char *exrerr = nullptr;
  if (ParseEXRHeaderFromMemory(&header, &version, bytes, size, &exrerr) !=
      TINYEXR_SUCCESS) {
    if (exrerr) FreeEXRErrorMessage(exrerr);
    // ParseEXRHeader may allocate the header (ConvertHeader) before failing;
    // free it. (LoadEXRImageFromMemory, by contrast, frees its image itself on
    // failure — calling FreeEXRImage there double-frees.)
    FreeEXRHeader(&header);
    return false;
  }

  // Only take the half path when EVERY channel is HALF in the file — tinyexr
  // cannot narrow a FLOAT channel to half on load.
  bool all_half = header.num_channels > 0;
  for (int c = 0; c < header.num_channels; c++) {
    if (header.pixel_types[c] != TINYEXR_PIXELTYPE_HALF) {
      all_half = false;
      break;
    }
    header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_HALF;  // load as-is
  }
  if (!all_half) {
    FreeEXRHeader(&header);
    return false;  // float channels present -> fp32 path
  }

  EXRImage exr;
  InitEXRImage(&exr);
  if (LoadEXRImageFromMemory(&exr, &header, bytes, size, &exrerr) !=
      TINYEXR_SUCCESS) {
    if (exrerr) FreeEXRErrorMessage(exrerr);
    FreeEXRHeader(&header);
    return false;
  }
  if (exr.images == nullptr || exr.width <= 0 || exr.height <= 0) {
    FreeEXRImage(&exr);  // load succeeded -> safe to free here
    FreeEXRHeader(&header);
    return false;
  }

  // Map channel names to RGBA (EXR usually stores (A)BGR order; Y = luminance).
  int idxR = -1, idxG = -1, idxB = -1, idxA = -1, idxY = -1;
  for (int c = 0; c < header.num_channels; c++) {
    const char *n = header.channels[c].name;
    if (ExrChannelNameIs(n, "R")) idxR = c;
    else if (ExrChannelNameIs(n, "G")) idxG = c;
    else if (ExrChannelNameIs(n, "B")) idxB = c;
    else if (ExrChannelNameIs(n, "A")) idxA = c;
    else if (ExrChannelNameIs(n, "Y")) idxY = c;
  }
  // No standard color channel (e.g. custom/layered names) — bail so the caller
  // falls back to the fp32 path instead of emitting a silently-black image.
  if (idxR < 0 && idxG < 0 && idxB < 0 && idxY < 0) {
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }

  size_t npix, total;
  if (!safe::mul(size_t(exr.width), size_t(exr.height), &npix) ||
      !safe::mul(npix, size_t(4 * 2), &total)) {  // RGBA * 2 bytes/half
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }
  if (total > kMaxDecodedImageBytes) {
    // Oversized: bail so the caller falls back to the fp32 path, which emits a
    // clean over-size error of its own.
    FreeEXRImage(&exr);
    FreeEXRHeader(&header);
    return false;
  }

  image->width = exr.width;
  image->height = exr.height;
  image->channels = 4;
  image->bpp = 16;
  image->format = Image::PixelFormat::Float;
  image->data.resize(total);
  uint16_t *out = reinterpret_cast<uint16_t *>(image->data.data());

  auto chan = [&](int idx) -> const uint16_t * {
    return idx >= 0 ? reinterpret_cast<const uint16_t *>(exr.images[idx])
                    : nullptr;
  };
  const uint16_t *R = chan(idxR), *G = chan(idxG), *B = chan(idxB),
                 *A = chan(idxA), *Y = chan(idxY);
  const uint16_t kOne = 0x3C00;  // half 1.0
  const uint16_t kZero = 0x0000;
  for (size_t p = 0; p < npix; p++) {
    out[p * 4 + 0] = R ? R[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 1] = G ? G[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 2] = B ? B[p] : (Y ? Y[p] : kZero);
    out[p * 4 + 3] = A ? A[p] : kOne;
  }

  FreeEXRImage(&exr);
  FreeEXRHeader(&header);
  return true;
}
#endif  // LIGHTUSD_EXR_V3
#else
bool DecodeImageEXRHalf(const uint8_t *, size_t, const std::string &, Image *,
                        std::string *) {
  return false;  // built without EXR support
}
#endif

#if defined(LIGHTUSD_WITH_EXR)
// EXR magic-number detection, backend-agnostic.
static inline bool ExrIsEXR(const uint8_t *addr, size_t sz) {
#if defined(LIGHTUSD_EXR_V3)
  return exr_is_exr_memory(addr, sz) != 0;
#else
  return TINYEXR_SUCCESS == IsEXRFromMemory(addr, sz);
#endif
}
#endif

#if defined(LIGHTUSD_WITH_TEXTOOLS)
// KTX2 container magic (identifier bytes, KTX 2.0 spec).
static inline bool IsKTX2FromMemory(const uint8_t *addr, size_t sz) {
  static const uint8_t id[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  return addr && sz >= 12 && std::memcmp(addr, id, 12) == 0;
}

// Decode level 0 of a KTX2 to uncompressed RGBA8. Handles the tinyexr-native
// transcodable/decodable set (uni / BC7 / ASTC LDR) via the texpipe reader;
// other block formats (BC1/3/5, ETC2/EAC) and HDR (BC6H/ASTC-HDR) are reported
// as an error here (upload the blocks directly instead, or keep them
// compressed). This is the legacy-friendly path: a .ktx2 asset becomes an
// ordinary RGBA8 Image, so every existing consumer (software renderers,
// re-encode to png/jpg for USDZ, web fallback) works unchanged.
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
// KTX2 supercompressionScheme 2 (Zstd) decompressor callback: wraps the
// vendored ZSTD_decompress into the tp_zstd_decompress_fn contract (return
// bytes written, or 0 on error).
static size_t KTX2ZstdDecompress(void * /*user*/, uint8_t *dst, size_t dst_cap,
                                 const uint8_t *src, size_t src_size) {
  const size_t r = ZSTD_decompress(dst, dst_cap, src, src_size);
  return ZSTD_isError(r) ? 0u : r;
}
#endif

// Budget-capped allocator for the KTX2 reader. A supercompressed (Zstd) KTX2
// inflates into a reader-owned buffer sized from the *header*: a small crafted
// file may legally declare dimensions whose block payload is multiple GiB. Cap
// the reader's allocation the same way the decoders here cap decoded images, so
// a hostile asset fails cleanly instead of exhausting memory.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((malloc, alloc_size(2)))
#endif
static void *KTX2BudgetAlloc(void *user, size_t size) {
  const size_t cap = *static_cast<const size_t *>(user);
  if (size == 0 || size > cap) return nullptr;
  return std::malloc(size);
}
static void KTX2BudgetFree(void * /*user*/, void *ptr) { std::free(ptr); }

static bool DecodeImageKTX2(const uint8_t *addr, size_t sz,
                            const std::string &uri, Image *image,
                            std::string *err) {
  tp_ktx2_image kimg;
  // Support uncompressed (scheme 0) and, when zstd is available, Zstd-super-
  // compressed (scheme 2) KTX2; the reader owns any decompressed buffer, freed
  // via tp_ktx2_image_free (with the same allocator).
  size_t alloc_cap = kMaxDecodedImageBytes;
  const tir_allocator kalloc{&alloc_cap, &KTX2BudgetAlloc, &KTX2BudgetFree};
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  tp_result r =
      tp_ktx2_read_zstd(addr, sz, &kalloc, &KTX2ZstdDecompress, nullptr, &kimg);
#else
  tp_result r = tp_ktx2_read(addr, sz, &kimg);
#endif
  if (r != TP_SUCCESS) {
    if (err) {
      (*err) += "KTX2: failed to parse: " + std::string(tp_result_string(r)) +
                "\n";
    }
    return false;
  }
  bool ok = false;
  if (kimg.is_hdr) {
    // HDR block formats (BC6H / ASTC-HDR) have no meaningful RGBA8 form: decode
    // them to float RGBA, mirroring the EXR/HDR decoders in this file.
    const uint32_t w = kimg.levels[0].width;
    const uint32_t h = kimg.levels[0].height;
    size_t nfloats = 0, nbytes = 0;
    if (!safe::mul3(size_t(w), size_t(h), size_t(4), &nfloats) ||
        !safe::mul(nfloats, sizeof(float), &nbytes)) {
      if (err) {
        (*err) += "KTX2: decoded HDR size overflows size_t for: " + uri + "\n";
      }
      tp_ktx2_image_free(&kalloc, &kimg);
      return false;
    }
    if (nbytes > kMaxDecodedImageBytes) {
      if (err) {
        (*err) += "KTX2: decoded HDR image exceeds the maximum allowed size "
                  "for: " + uri + "\n";
      }
      tp_ktx2_image_free(&kalloc, &kimg);
      return false;
    }
    image->data.resize(nbytes);
    // NOTE: out_size is in *bytes* (as on the RGBA8 path), not float count.
    r = tp_ktx2_decode_level_rgbaf(
        &kimg, 0, reinterpret_cast<float *>(image->data.data()), nbytes);
    if (r != TP_SUCCESS) {
      image->data.clear();
      if (err) {
        (*err) += "KTX2: failed to decode HDR level 0: " +
                  std::string(tp_result_string(r)) + "\n";
      }
    } else {
      image->uri = uri;
      image->width = int(w);
      image->height = int(h);
      image->channels = 4;
      image->bpp = 32;
      image->format = Image::PixelFormat::Float;
      image->colorspace = "";  // HDR block formats are linear
      ok = true;
    }
  } else {
    const uint32_t w = kimg.levels[0].width;
    const uint32_t h = kimg.levels[0].height;
    // The KTX2 parser bounds dimensions (TP_KTX2_MAX_DIM), but a spec-legal
    // 65536x65536 still decodes to 16 GiB of RGBA8 -- and w*h*4 wraps a 32-bit
    // size_t. Use the checked multiply and the same decoded-size ceiling the
    // other decoders in this file enforce.
    size_t need = 0;
    if (!safe::mul3(size_t(w), size_t(h), size_t(4), &need)) {
      if (err) {
        (*err) += "KTX2: decoded size overflows size_t for: " + uri + "\n";
      }
      tp_ktx2_image_free(&kalloc, &kimg);
      return false;
    }
    if (need > kMaxDecodedImageBytes) {
      if (err) {
        (*err) += "KTX2: decoded image exceeds the maximum allowed size for: " +
                  uri + "\n";
      }
      tp_ktx2_image_free(&kalloc, &kimg);
      return false;
    }
    image->data.resize(need);
    r = tp_ktx2_decode_level_rgba8(&kimg, 0, image->data.data(), need);
    if (r != TP_SUCCESS) {
      image->data.clear();
      if (err) {
        (*err) += "KTX2: failed to decode level 0: " +
                  std::string(tp_result_string(r)) + "\n";
      }
    } else {
      image->uri = uri;
      image->width = int(w);
      image->height = int(h);
      image->channels = 4;
      image->bpp = 8;
      image->format = Image::PixelFormat::UInt;
      // KTX2 DFD transfer function -> colorspace hint (Auto/sourceColorSpace
      // still applies downstream; empty = let the caller decide).
      image->colorspace = kimg.srgb ? "sRGB" : "";
      ok = true;
    }
  }
  tp_ktx2_image_free(&kalloc, &kimg);  // no-op unless a Zstd buffer was owned
  return ok;
}
#endif  // LIGHTUSD_WITH_TEXTOOLS

nonstd::expected<image::ImageResult, std::string> LoadImageFromMemory(
    const uint8_t *addr, size_t sz, const std::string &uri) {
  image::ImageResult ret;
  // Keep provenance attached regardless of which decoder handles the bytes.
  // This is especially useful for nanoimage/TinyEXR diagnostics and for
  // downstream texture-cache keys when the source came from a USDZ archive.
  ret.image.uri = uri;
  std::string err;

  LoadImageDataFunction user_loader = nullptr;
  void *user_data = nullptr;
  GetImageLoader(&user_loader, &user_data);
  if (user_loader) {
    if (user_loader(&ret, addr, sz, uri, user_data, &ret.warning, &err)) {
      ret.image.uri = uri;
      return std::move(ret);
    }
    // A custom loader may use the failed attempt to recognize its own format;
    // do not leak that diagnostic into an independent built-in attempt.
    ret.warning.clear();
    err.clear();
  }

#if defined(LIGHTUSD_WITH_TEXTOOLS)
  // KTX2 (GPU-compressed / uni). Distinct 12-byte magic, checked first.
  if (IsKTX2FromMemory(addr, sz)) {
    bool ok = DecodeImageKTX2(addr, sz, uri, &ret.image, &err);
    if (!ok) {
      return nonstd::make_unexpected(err);
    }
    return std::move(ret);
  }
#endif

#if defined(LIGHTUSD_WITH_EXR)
  if (ExrIsEXR(addr, sz)) {

    // Preserve the common all-half scanline case. The regular decoder remains
    // the fallback for mixed channel types, tiled/deep images, and multipart
    // files where the public Image contract is normalized to fp32 RGBA.
    std::string half_err;
    if (DecodeImageEXRHalf(addr, sz, uri, &ret.image, &half_err)) {
      return std::move(ret);
    }

    bool ok = DecodeImageEXR(addr, sz, uri, &ret.image, &err);

    if (!ok) {
      return nonstd::make_unexpected(err);
    }

    return std::move(ret);
  }
#endif

#if defined(LIGHTUSD_WITH_TIFF)
  {
    std::string msg;
    if (tinydng::IsDNGFromMemory(reinterpret_cast<const char *>(addr), sz, &msg)) {

      bool ok = DecodeImageTIFF(addr, sz, uri, &ret.image, &err);

      if (!ok) {
        return nonstd::make_unexpected(err);
      }

      return std::move(ret);
    }
  }
#endif

#if defined(LIGHTUSD_WITH_NANOIMAGE)
  // Try nanoimage for common formats (jpg, png, bmp, tga).
  // Fuzz-tested, memory-safe C decoder. Falls through to STB/WUFFS on failure.
  {
    bool ok = DecodeImageNanoimage(addr, sz, uri, &ret.image, &ret.warning, &err);
    if (ok) {
      return std::move(ret);
    }
    // Clear error from nanoimage attempt; STB/WUFFS will provide the final error.
    err.clear();
  }
#endif

  // HDR (Radiance RGBE) detection - must be before generic STB fallback
  // to ensure we decode as float instead of uint8
#if !defined(LIGHTUSD_NO_BUILTIN_IMAGE_LOADER) && !defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)
  if (IsHDRFromMemory(addr, sz)) {
    bool ok = DecodeImageHDR(addr, sz, uri, &ret.image, &ret.warning, &err);

    if (!ok) {
      return nonstd::make_unexpected(err);
    }

    return std::move(ret);
  }
#endif

#if defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)
  bool ok = DecodeImageWUFF(addr, sz, uri, &ret.image, &ret.warning, &err);
#elif !defined(LIGHTUSD_NO_BUILTIN_IMAGE_LOADER)
  bool ok = DecodeImageSTB(addr, sz, uri, &ret.image, &ret.warning, &err);
#else
  (void)addr;
  (void)sz;
  (void)uri;
  bool ok = false;
  err = "Image loading is disabled in this build and no user loader succeeded\n";
#endif
  if (!ok) {
    return nonstd::make_unexpected(err);
  }

  return std::move(ret);
}

nonstd::expected<image::ImageInfoResult, std::string> GetImageInfoFromMemory(
    const uint8_t *addr, size_t sz, const std::string &uri) {
  image::ImageInfoResult ret;
  std::string err;

  GetImageInfoFunction user_loader = nullptr;
  void *user_data = nullptr;
  GetImageInfoLoader(&user_loader, &user_data);
  if (user_loader) {
    if (user_loader(&ret, addr, sz, uri, user_data)) {
      return std::move(ret);
    }
    ret.warning.clear();
  }

#if defined(LIGHTUSD_WITH_EXR)
  if (ExrIsEXR(addr, sz)) {
#if defined(LIGHTUSD_EXR_V3)
    // Parse headers + offset tables only (no pixel decode) for fast info query.
    ExrBudgetAllocator budget(kMaxDecodedImageBytes);
    exr_allocator alloc = budget.handle();
    exr_reader *rd = nullptr;
    if (!ExrOk(exr_reader_open_memory(addr, sz, &alloc, &rd))) {
      return nonstd::make_unexpected("Failed to open EXR for info: " + uri + "\n");
    }
    if (!ExrOk(exr_reader_parse_header(rd)) ||
        exr_reader_num_parts(rd) < 1) {
      exr_reader_close(rd);
      return nonstd::make_unexpected("Failed to parse EXR header: " + uri + "\n");
    }
    const exr_header *h = exr_reader_part_header(rd, 0);
    if (!h) {
      exr_reader_close(rd);
      return nonstd::make_unexpected("EXR has no part header: " + uri + "\n");
    }
    const int64_t w =
        int64_t(h->data_window.max_x) - int64_t(h->data_window.min_x) + 1;
    const int64_t hgt =
        int64_t(h->data_window.max_y) - int64_t(h->data_window.min_y) + 1;
    const int32_t nch = h->num_channels;  // read before closing (h is reader-owned)
    exr_reader_close(rd);
    if (w <= 0 || hgt <= 0) {
      return nonstd::make_unexpected("EXR has an invalid data window: " + uri + "\n");
    }
    ret.width = uint32_t(w);
    ret.height = uint32_t(hgt);
    ret.channels = uint32_t(nch);
    return std::move(ret);
#else
    EXRVersion version;
    const int version_ret = ParseEXRVersionFromMemory(
        &version, reinterpret_cast<const unsigned char *>(addr), sz);
    if (version_ret != TINYEXR_SUCCESS) {
      return nonstd::make_unexpected("Failed to parse EXR version: " + uri +
                                     "\n");
    }
    EXRHeader header;
    InitEXRHeader(&header);
    const char *exrerr = nullptr;
    const int header_ret = ParseEXRHeaderFromMemory(
        &header, &version, reinterpret_cast<const unsigned char *>(addr), sz,
        &exrerr);
    if (header_ret != TINYEXR_SUCCESS) {
      std::string message = "Failed to parse EXR header: " + uri + "\n";
      if (exrerr) {
        message += exrerr;
        message += "\n";
        FreeEXRErrorMessage(exrerr);
      }
      FreeEXRHeader(&header);
      return nonstd::make_unexpected(message);
    }
    const int64_t w = int64_t(header.data_window.max_x) -
                      int64_t(header.data_window.min_x) + 1;
    const int64_t hgt = int64_t(header.data_window.max_y) -
                        int64_t(header.data_window.min_y) + 1;
    const int nch = header.num_channels;
    FreeEXRHeader(&header);
    if (w <= 0 || hgt <= 0 || nch <= 0) {
      return nonstd::make_unexpected("EXR has an invalid image window: " +
                                     uri + "\n");
    }
    ret.width = static_cast<uint32_t>(w);
    ret.height = static_cast<uint32_t>(hgt);
    ret.channels = static_cast<uint32_t>(nch);
    return std::move(ret);
#endif
  }
#endif

#if defined(LIGHTUSD_WITH_TIFF)
  if (tinydng::IsDNGFromMemory(reinterpret_cast<const char *>(addr), sz, &err)) {
    std::vector<tinydng::FieldInfo> custom_fields;
    std::vector<tinydng::DNGImage> images;
    std::string warn;
    std::string dngerr;
    if (!tinydng::LoadDNGMetadataFromMemory(
            reinterpret_cast<const char *>(addr), sz, custom_fields,
            &images, &warn, &dngerr) || images.empty()) {
#if defined(LIGHTUSD_WITH_LIBTIFF)
      if (GetTiledTIFFInfo(addr, sz, &ret.width, &ret.height,
                           &ret.channels)) {
        return std::move(ret);
      }
#endif
      return nonstd::make_unexpected("Failed to load TIFF/DNG header: " + uri +
                                     "\n" + dngerr);
    }
    size_t largest = 0;
    for (size_t i = 1; i < images.size(); ++i) {
      if (images[largest].width < images[i].width) largest = i;
    }
    ret.width = static_cast<uint32_t>(images[largest].width);
    ret.height = static_cast<uint32_t>(images[largest].height);
    ret.channels = static_cast<uint32_t>(images[largest].samples_per_pixel);
    return std::move(ret);

  }
#endif

#if defined(LIGHTUSD_WITH_NANOIMAGE)
  {
    bool ok = GetImageInfoNanoimage(addr, sz, uri, &ret.width, &ret.height,
                                    &ret.channels, &ret.warning, &err);
    if (ok) {
      return std::move(ret);
    }
    err.clear();
  }
#endif

  // HDR (Radiance RGBE) detection
#if !defined(LIGHTUSD_NO_BUILTIN_IMAGE_LOADER) && !defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)
  if (IsHDRFromMemory(addr, sz)) {
    bool ok = GetImageInfoHDR(addr, sz, uri, &ret.width, &ret.height, &ret.channels, &ret.warning, &err);
    if (!ok) {
      return nonstd::make_unexpected(err);
    }
    return std::move(ret);
  }
#endif

#if defined(LIGHTUSD_USE_WUFFS_IMAGE_LOADER)
  bool ok = GetImageInfoWUFF(addr, sz, uri, &ret.width, &ret.height, &ret.channels, &ret.warning, &err);
#elif !defined(LIGHTUSD_NO_BUILTIN_IMAGE_LOADER)
  bool ok = GetImageInfoSTB(addr, sz, uri, &ret.width, &ret.height, &ret.channels, &ret.warning, &err);
#else
  (void)addr;
  (void)sz;
  (void)uri;
  bool ok = false;
  err = "Image info loading is disabled in this build and no user loader succeeded\n";
#endif
  if (!ok) {
    return nonstd::make_unexpected(err);
  }

  return std::move(ret);
}

namespace {

bool ImageMMapSizeFitsSizeT(uint64_t size) {
  // Some compilers diagnose the comparison as tautological even in a
  // discarded `if constexpr` branch. Remove it before parsing on 64-bit
  // targets, while retaining the narrowing guard for 32-bit builds.
#if SIZE_MAX < UINT64_MAX
  return size <= static_cast<uint64_t>(SIZE_MAX);
#else
  (void)size;
  return true;
#endif
}

}  // namespace

nonstd::expected<image::ImageInfoResult, std::string> GetImageInfoFromFile(
    const std::string &filename) {
  io::MMapFileHandle mapped;
  std::string map_err;
  if (io::MMapFile(filename, &mapped, false, &map_err)) {
    if (ImageMMapSizeFitsSizeT(mapped.size)) {
      auto result = GetImageInfoFromMemory(mapped.addr,
                                           static_cast<size_t>(mapped.size),
                                           filename);
      std::string unmap_err;
      io::UnmapFile(mapped, &unmap_err);
      return result;
    }
    std::string unmap_err;
    io::UnmapFile(mapped, &unmap_err);
  }

  // Mapping is the normal path. Keep the existing bounded stream fallback for
  // platforms without mmap support and for unusual filesystems that reject it.
  std::vector<uint8_t> data;
  std::string err;
  if (!io::ReadWholeFile(&data, &err, filename, MaxMemoryBytes(1024 * 1024),
                         nullptr)) {
    return nonstd::make_unexpected("File not found or failed to read : \"" +
                                   filename + "\"\n");
  }
  return GetImageInfoFromMemory(data.data(), data.size(), filename);
}

nonstd::expected<image::ImageResult, std::string> LoadImageFromFile(
    const std::string &filename, const size_t max_memory_limit_in_mb) {

  // Assume filename is already resolved.
  std::string filepath = filename;

  const size_t max_bytes = MaxMemoryBytes(uint64_t(max_memory_limit_in_mb));
  io::MMapFileHandle mapped;
  std::string map_err;
  if (io::MMapFile(filepath, &mapped, false, &map_err)) {
    if (mapped.size > static_cast<uint64_t>(max_bytes)) {
      std::string unmap_err;
      io::UnmapFile(mapped, &unmap_err);
      return nonstd::make_unexpected("Image file exceeds memory limit: \"" +
                                     filepath + "\"\n");
    }
    auto result = LoadImageFromMemory(mapped.addr,
                                      static_cast<size_t>(mapped.size),
                                      filename);
    std::string unmap_err;
    io::UnmapFile(mapped, &unmap_err);
    return result;
  }

  std::vector<uint8_t> data;
  std::string err;
  if (!io::ReadWholeFile(&data, &err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    return nonstd::make_unexpected("File not found or failed to read : \"" + filepath + "\"\n");
  }

  if (data.size() < 4) {
    return nonstd::make_unexpected("File size too short. Looks like this file is not an image file : \"" +
                filepath + "\"\n");
  }

  return LoadImageFromMemory(data.data(), data.size(), filename);
}

}  // namespace image
}  // namespace lightusd
