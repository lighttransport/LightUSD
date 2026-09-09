// SPDX-License-Identifier: Apache 2.0
#if defined(LIGHTUSD_WITH_EXR)
#if defined(LIGHTUSD_EXR_V3)
#include "external/tinyexr/include/exr.h"
#else
#include "external/tinyexr.h"
#endif
#endif

#if defined(LIGHTUSD_WITH_TIFF)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
#define TINY_DNG_WRITER_IMPLEMENTATION
#include "external/tiny_dng_writer.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#ifndef LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/stb_image_write.h"

#if defined(LIGHTUSD_HAVE_FPNG)
#include "external/fpng.h"
#endif

#if defined(LIGHTUSD_HAVE_FPNGE)
#include "external/fpnge/fpnge.h"
#endif

#if defined(LIGHTUSD_HAVE_QOI)
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO  // memory-only; we don't use qoi_read/qoi_write file helpers
#include "external/qoi/qoi.h"
#endif

#if defined(LIGHTUSD_HAVE_TURBOJPEG)
#include "turbojpeg.h"
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdlib>  // free (qoi_encode / turbojpeg buffers)
#include <sstream>

#include "image-writer.hh"
#include "io-util.hh"
#include "safe-arithmetic.hh"
#include "str-util.hh"

#include <climits>
#include <limits>

namespace lightusd {
namespace image {

namespace {

bool DetectFileFormatFromExtension(const std::string &_ext, lightusd::image::WriteImageFormat &format) {

  std::string ext = to_lower(_ext);

  if (ext == "bmp") {
    format = lightusd::image::WriteImageFormat::BMP;
    return true;
  } else if (ext == "png") {
    format = lightusd::image::WriteImageFormat::PNG;
    return true;
  } else if ((ext == "jpg") || (ext == "jpeg")) {
    format = lightusd::image::WriteImageFormat::JPEG;
    return true;
  } else if ((ext == "tiff") || (ext == "tif")) {
    format = lightusd::image::WriteImageFormat::TIFF;
    return true;
  } else if (ext == "dng") {
    format = lightusd::image::WriteImageFormat::DNG;
    return true;
  } else if (ext == "exr") {
    format = lightusd::image::WriteImageFormat::EXR;
    return true;
  } else if (ext == "qoi") {
    format = lightusd::image::WriteImageFormat::QOI;
    return true;
  }

  return false;
}

// stb_image_write callback that appends to a std::vector<uint8_t>.
void AppendToVector(void *context, void *data, int size) {
  if (size <= 0 || data == nullptr) {
    return;
  }
  auto *buf = reinterpret_cast<std::vector<uint8_t> *>(context);
  const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
  buf->insert(buf->end(), p, p + size_t(size));
}

bool ValidDims(const Image &image, std::string *err) {
  if (image.width <= 0 || image.height <= 0 || image.channels <= 0) {
    if (err) {
      (*err) = "Invalid image dimensions/channels.";
    }
    return false;
  }
  if (image.channels > 4) {
    if (err) {
      (*err) = "Only 1-4 channel images are supported.";
    }
    return false;
  }
  // Enforce a maximum dimension to prevent integer overflow in stride calculations.
  constexpr int kMaxDimension = 65536;
  if (image.width > kMaxDimension || image.height > kMaxDimension) {
    if (err) {
      (*err) = "Image dimensions exceed maximum allowed size (65536).";
    }
    return false;
  }
  const int64_t npixels = int64_t(image.width) * int64_t(image.height);
  // bpp must be a positive multiple of 8 (8, 16, 24, 32).
  if (image.bpp > 0 && (image.bpp % 8 != 0)) {
    if (err) {
      (*err) = "Image bits-per-pixel must be a multiple of 8.";
    }
    return false;
  }
  const int64_t bytes_per_channel = (image.bpp > 0) ? (image.bpp / 8) : 1;
  const int64_t expected =
      npixels * int64_t(image.channels) * bytes_per_channel;
  if (int64_t(image.data.size()) < expected) {
    if (err) {
      (*err) = "Image pixel buffer is smaller than width*height*channels*bytesPerChannel.";
    }
    return false;
  }
  return true;
}

#if defined(LIGHTUSD_WITH_TIFF)
// TinyDNGWriter emits a baseline TIFF stream and is also suitable for the
// simple, non-camera DNG payloads used by LightUSD. Image::bpp is bits per
// channel (matching the loader), while TinyDNG expects one value per channel.
bool EncodeTIFF(const Image &image, std::vector<uint8_t> *out,
                std::string *err) {
  if (!out || image.bpp <= 0 || image.bpp > 32 || image.bpp % 8 != 0) {
    if (err) *err = "TIFF: unsupported bits per channel.";
    return false;
  }

  const size_t bytes_per_channel = size_t(image.bpp / 8);
  size_t expected = 0;
  if (!safe::mul(size_t(image.width), size_t(image.height), &expected) ||
      !safe::mul(expected, size_t(image.channels), &expected) ||
      !safe::mul(expected, bytes_per_channel, &expected) ||
      image.data.size() < expected) {
    if (err) *err = "TIFF: image pixel buffer is too small.";
    return false;
  }

  tinydngwriter::DNGImage dng_image;
  dng_image.SetBigEndian(false);
  if (!dng_image.SetImageWidth(static_cast<unsigned int>(image.width)) ||
      !dng_image.SetImageLength(static_cast<unsigned int>(image.height)) ||
      !dng_image.SetRowsPerStrip(static_cast<unsigned int>(image.height)) ||
      !dng_image.SetSamplesPerPixel(static_cast<unsigned short>(image.channels))) {
    if (err) *err = "TIFF: failed to set image dimensions.";
    return false;
  }

  std::vector<unsigned short> bits(size_t(image.channels),
                                   static_cast<unsigned short>(image.bpp));
  if (!dng_image.SetBitsPerSample(static_cast<unsigned int>(bits.size()),
                                  bits.data()) ||
      !dng_image.SetPhotometric(image.channels == 1
                                    ? tinydngwriter::PHOTOMETRIC_BLACK_IS_ZERO
                                    : tinydngwriter::PHOTOMETRIC_RGB) ||
      !dng_image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG) ||
      !dng_image.SetCompression(tinydngwriter::COMPRESSION_NONE) ||
      !dng_image.SetOrientation(tinydngwriter::ORIENTATION_TOPLEFT)) {
    if (err) *err = "TIFF: failed to set image metadata.";
    return false;
  }

  const int sample_format =
      image.format == Image::PixelFormat::Float
          ? tinydngwriter::SAMPLEFORMAT_IEEEFP
          : image.format == Image::PixelFormat::Int
                ? tinydngwriter::SAMPLEFORMAT_INT
                : tinydngwriter::SAMPLEFORMAT_UINT;
  std::vector<unsigned short> formats(size_t(image.channels),
                                      static_cast<unsigned short>(sample_format));
  if (!dng_image.SetSampleFormat(static_cast<unsigned int>(formats.size()),
                                 formats.data()) ||
      !dng_image.SetImageData(image.data.data(), expected)) {
    if (err) *err = "TIFF: failed to encode image data: " + dng_image.Error();
    return false;
  }

  // TinyDNG's public stream methods use the standard eight-byte TIFF header
  // layout: byte order, version 42, and the first IFD offset (8).
  std::ostringstream stream(std::ios::binary);
  const char header[8] = {'I', 'I', 42, 0, 8, 0, 0, 0};
  stream.write(header, sizeof(header));
  if (!dng_image.WriteDataToStream(&stream) ||
      !dng_image.WriteIFDToStream(0, 0, &stream)) {
    if (err) *err = "TIFF: failed to write IFD: " + dng_image.Error();
    return false;
  }
  const char next_ifd[4] = {0, 0, 0, 0};
  stream.write(next_ifd, sizeof(next_ifd));
  const std::string encoded = stream.str();
  out->assign(encoded.begin(), encoded.end());
  return true;
}

bool EncodeTIFFLayers(const std::vector<Image> &images,
                      std::vector<uint8_t> *out, std::string *err) {
  if (!out || images.empty()) {
    if (err) *err = "TIFF: at least one image layer is required.";
    return false;
  }
  std::vector<tinydngwriter::DNGImage> dng_images(images.size());
  for (size_t i = 0; i < images.size(); ++i) {
    const Image &image = images[i];
    std::string validation_error;
    if (!ValidDims(image, &validation_error) || image.bpp <= 0 ||
        image.bpp > 32 || image.bpp % 8 != 0) {
      if (err) *err = "TIFF layer " + std::to_string(i) + ": " +
                      (validation_error.empty()
                           ? "unsupported bits per channel."
                           : validation_error);
      return false;
    }
    const size_t bytes_per_channel = size_t(image.bpp / 8);
    size_t expected = 0;
    if (!safe::mul(size_t(image.width), size_t(image.height), &expected) ||
        !safe::mul(expected, size_t(image.channels), &expected) ||
        !safe::mul(expected, bytes_per_channel, &expected) ||
        image.data.size() < expected) {
      if (err) *err = "TIFF layer " + std::to_string(i) +
                      ": image pixel buffer is too small.";
      return false;
    }
    auto &dng = dng_images[i];
    dng.SetBigEndian(false);
    if (!dng.SetImageWidth(static_cast<unsigned int>(image.width)) ||
        !dng.SetImageLength(static_cast<unsigned int>(image.height)) ||
        !dng.SetRowsPerStrip(static_cast<unsigned int>(image.height)) ||
        !dng.SetSamplesPerPixel(static_cast<unsigned short>(image.channels))) {
      if (err) *err = "TIFF layer " + std::to_string(i) +
                      ": failed to set image dimensions.";
      return false;
    }
    std::vector<unsigned short> bits(size_t(image.channels),
                                     static_cast<unsigned short>(image.bpp));
    const int sample_format =
        image.format == Image::PixelFormat::Float
            ? tinydngwriter::SAMPLEFORMAT_IEEEFP
            : image.format == Image::PixelFormat::Int
                  ? tinydngwriter::SAMPLEFORMAT_INT
                  : tinydngwriter::SAMPLEFORMAT_UINT;
    std::vector<unsigned short> formats(size_t(image.channels),
                                        static_cast<unsigned short>(sample_format));
    if (!dng.SetBitsPerSample(static_cast<unsigned int>(bits.size()), bits.data()) ||
        !dng.SetPhotometric(image.channels == 1
                                ? tinydngwriter::PHOTOMETRIC_BLACK_IS_ZERO
                                : tinydngwriter::PHOTOMETRIC_RGB) ||
        !dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG) ||
        !dng.SetCompression(tinydngwriter::COMPRESSION_NONE) ||
        !dng.SetOrientation(tinydngwriter::ORIENTATION_TOPLEFT) ||
        !dng.SetSampleFormat(static_cast<unsigned int>(formats.size()),
                             formats.data()) ||
        !dng.SetImageData(image.data.data(), expected)) {
      if (err) *err = "TIFF layer " + std::to_string(i) +
                      ": failed to encode image data: " + dng.Error();
      return false;
    }
  }
  size_t data_len = 0;
  std::vector<size_t> data_offsets, strip_offsets;
  data_offsets.reserve(dng_images.size());
  strip_offsets.reserve(dng_images.size());
  for (const auto &dng : dng_images) {
    data_offsets.push_back(data_len);
    if (!safe::add(data_len, dng.GetStripOffset(), &data_len)) {
      if (err) *err = "TIFF: layer data offset overflow.";
      return false;
    }
    strip_offsets.push_back(data_len);
    if (!safe::add(data_len, dng.GetDataSize(), &data_len) ||
        data_len > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
      if (err) *err = "TIFF: multi-layer stream is too large.";
      return false;
    }
  }
  std::ostringstream stream(std::ios::binary);
  const char header[8] = {'I', 'I', 42, 0, 0, 0, 0, 0};
  stream.write(header, sizeof(header));
  const uint32_t first_ifd = static_cast<uint32_t>(8 + data_len);
  stream.seekp(4);
  stream.write(reinterpret_cast<const char *>(&first_ifd), sizeof(first_ifd));
  stream.seekp(8);
  for (const auto &dng : dng_images) {
    if (!dng.WriteDataToStream(&stream)) {
      if (err) *err = "TIFF: failed to write layer data: " + dng.Error();
      return false;
    }
  }
  for (size_t i = 0; i < dng_images.size(); ++i) {
    if (!dng_images[i].WriteIFDToStream(
            static_cast<unsigned int>(data_offsets[i]),
            static_cast<unsigned int>(strip_offsets[i]), &stream)) {
      if (err) *err = "TIFF: failed to write layer IFD: " +
                      dng_images[i].Error();
      return false;
    }
    uint32_t next = i + 1 == dng_images.size()
                        ? 0u
                        : static_cast<uint32_t>(stream.tellp()) + 4u;
    stream.write(reinterpret_cast<const char *>(&next), sizeof(next));
  }
  const std::string encoded = stream.str();
  out->assign(encoded.begin(), encoded.end());
  return true;
}
#endif

#if defined(LIGHTUSD_HAVE_FPNGE)
bool EncodePNG_fpnge(const Image &image, std::vector<uint8_t> *out,
                     std::string *err) {
  const size_t bytes_per_channel = size_t((image.bpp > 0) ? image.bpp / 8 : 1);
  if (bytes_per_channel != 1 && bytes_per_channel != 2) {
    if (err) {
      (*err) = "fpnge: only 8-bit or 16-bit per channel is supported.";
    }
    return false;
  }
  const size_t num_channels = size_t(image.channels);
  if (num_channels < 1 || num_channels > 4) {
    if (err) {
      (*err) = "fpnge: only 1-4 channels are supported.";
    }
    return false;
  }

  const size_t width = size_t(image.width);
  const size_t height = size_t(image.height);
  const uint64_t row_stride_64 = uint64_t(width) * num_channels * bytes_per_channel;
  if (row_stride_64 > SIZE_MAX) {
    if (err) {
      (*err) = "fpnge: row stride too large.";
    }
    return false;
  }
  const size_t row_stride = static_cast<size_t>(row_stride_64);

  struct FPNGEOptions options;
  FPNGEFillOptions(&options, FPNGE_COMPRESS_LEVEL_DEFAULT, FPNGE_CICP_NONE);

  out->resize(
      FPNGEOutputAllocSize(bytes_per_channel, num_channels, width, height));

  const size_t n =
      FPNGEEncode(bytes_per_channel, num_channels, image.data.data(), width,
                  row_stride, height, out->data(), &options);
  if (n == 0) {
    if (err) {
      (*err) = "fpnge: encode failed.";
    }
    return false;
  }
  out->resize(n);
  return true;
}
#endif

#if defined(LIGHTUSD_HAVE_FPNG)
bool EncodePNG_fpng(const Image &image, std::vector<uint8_t> *out,
                    std::string *err) {
  // fpng only supports 8-bit, 3 or 4 channels.
  if (image.bpp != 8) {
    if (err) {
      (*err) = "fpng: only 8-bit per channel is supported.";
    }
    return false;
  }
  if (image.channels != 3 && image.channels != 4) {
    if (err) {
      (*err) = "fpng: only 3(RGB) or 4(RGBA) channels are supported.";
    }
    return false;
  }
  static bool s_fpng_inited = []() {
    fpng::fpng_init();
    return true;
  }();
  (void)s_fpng_inited;

  if (!fpng::fpng_encode_image_to_memory(
          image.data.data(), uint32_t(image.width), uint32_t(image.height),
          uint32_t(image.channels), *out)) {
    if (err) {
      (*err) = "fpng: encode failed.";
    }
    return false;
  }
  return true;
}
#endif  // LIGHTUSD_HAVE_FPNG

bool EncodePNG_stb(const Image &image, std::vector<uint8_t> *out,
                   std::string *err) {
  if (image.bpp != 8) {
    if (err) {
      (*err) = "stb png: only 8-bit per channel is supported.";
    }
    return false;
  }
  const int64_t stride64 = int64_t(image.width) * int64_t(image.channels);
  if (stride64 > static_cast<int64_t>(INT_MAX)) {
    if (err) {
      (*err) = "stb png: image row stride exceeds INT_MAX.";
    }
    return false;
  }
  const int stride = static_cast<int>(stride64);
  out->clear();
  int ret = stbi_write_png_to_func(AppendToVector, out, image.width,
                                   image.height, image.channels,
                                   image.data.data(), stride);
  if (ret == 0) {
    if (err) {
      (*err) = "stb png: encode failed.";
    }
    return false;
  }
  return true;
}

bool EncodePNG(const Image &image, PngEncoder encoder,
               std::vector<uint8_t> *out, std::string *err) {
  // Decide preference order based on the requested encoder.
  // Auto/Fpnge prefer fpnge(when compiled in), then fpng, then stb.
  // Fpng forces fpng(then stb fallback for unsupported channel/bitdepth).
  // When fpnge is not compiled in, a Fpnge request transparently falls
  // back to fpng/stb.
  (void)encoder;
  std::string local_err;

#if defined(LIGHTUSD_HAVE_FPNGE)
  if (encoder == PngEncoder::Auto || encoder == PngEncoder::Fpnge) {
    if (EncodePNG_fpnge(image, out, &local_err)) {
      return true;
    }
  }
#endif

#if defined(LIGHTUSD_HAVE_FPNG)
  if (EncodePNG_fpng(image, out, &local_err)) {
    return true;
  }
#endif

  // Final fallback: stb (handles 1/2-channel 8-bit that fpng rejects).
  if (EncodePNG_stb(image, out, &local_err)) {
    return true;
  }

  if (err) {
    (*err) = "PNG encode failed: " + local_err;
  }
  return false;
}

#if defined(LIGHTUSD_HAVE_QOI)
bool EncodeQOI(const Image &image, std::vector<uint8_t> *out, std::string *err) {
  // QOI is 8-bit, 3(RGB) or 4(RGBA) channels only.
  if (image.bpp != 8) {
    if (err) (*err) = "qoi: only 8-bit per channel is supported.";
    return false;
  }
  if (image.channels != 3 && image.channels != 4) {
    if (err) (*err) = "qoi: only 3(RGB) or 4(RGBA) channels are supported.";
    return false;
  }
  qoi_desc desc;
  desc.width = static_cast<unsigned int>(image.width);
  desc.height = static_cast<unsigned int>(image.height);
  desc.channels = static_cast<unsigned char>(image.channels);
  desc.colorspace = QOI_SRGB;
  int len = 0;
  void *enc = qoi_encode(image.data.data(), &desc, &len);
  if (!enc || len <= 0) {
    if (err) (*err) = "qoi: encode failed.";
    if (enc) std::free(enc);
    return false;
  }
  const uint8_t *p = static_cast<const uint8_t *>(enc);
  out->assign(p, p + static_cast<size_t>(len));
  std::free(enc);
  return true;
}
#endif  // LIGHTUSD_HAVE_QOI

#if defined(LIGHTUSD_HAVE_TURBOJPEG)
bool EncodeJPEG_turbo(const Image &image, int quality, std::vector<uint8_t> *out,
                      std::string *err) {
  // libjpeg-turbo: 8-bit, 1/3/4 channels. RGBA is downsampled to RGB (alpha
  // dropped) since JPEG has no alpha.
  int pf;
  switch (image.channels) {
    case 1: pf = TJPF_GRAY; break;
    case 3: pf = TJPF_RGB; break;
    case 4: pf = TJPF_RGBA; break;
    default:
      if (err) (*err) = "turbojpeg: only 1/3/4 channels supported.";
      return false;
  }
  tjhandle h = tjInitCompress();
  if (!h) {
    if (err) (*err) = "turbojpeg: tjInitCompress failed.";
    return false;
  }
  unsigned char *jpegBuf = nullptr;
  unsigned long jpegSize = 0;
  const int subsamp = (image.channels == 1) ? TJSAMP_GRAY : TJSAMP_420;
  int rc = tjCompress2(h, image.data.data(), image.width, /*pitch=*/0,
                       image.height, pf, &jpegBuf, &jpegSize, subsamp, quality,
                       TJFLAG_FASTDCT);
  if (rc != 0) {
    std::string e = tjGetErrorStr2(h);
    tjDestroy(h);
    if (jpegBuf) tjFree(jpegBuf);
    if (err) (*err) = "turbojpeg: " + e;
    return false;
  }
  out->assign(jpegBuf, jpegBuf + jpegSize);
  tjFree(jpegBuf);
  tjDestroy(h);
  return true;
}
#endif  // LIGHTUSD_HAVE_TURBOJPEG

}  // namespace

nonstd::expected<std::vector<uint8_t>, std::string> WriteImageToMemory(
    const Image &image, const WriteOption option) {
  lightusd::image::WriteImageFormat format = option.format;

  if (format == lightusd::image::WriteImageFormat::Autodetect) {
    // Choose a sensible default from the pixel format:
    // float -> EXR, otherwise PNG.
    if (image.format == Image::PixelFormat::Float || image.bpp > 16) {
      format = lightusd::image::WriteImageFormat::EXR;
    } else {
      format = lightusd::image::WriteImageFormat::PNG;
    }
  }

  std::string err;
  if (!ValidDims(image, &err)) {
    return nonstd::make_unexpected(err);
  }

  std::vector<uint8_t> out;

  switch (format) {
    case lightusd::image::WriteImageFormat::PNG: {
      if (!EncodePNG(image, option.png_encoder, &out, &err)) {
        return nonstd::make_unexpected(err);
      }
      return out;
    }
    case lightusd::image::WriteImageFormat::JPEG: {
      if (image.bpp != 8) {
        return nonstd::make_unexpected("8bit only for JPEG output.");
      }
      int q = option.jpeg_quality;
      if (q < 1) q = 1;
      if (q > 100) q = 100;
      out.clear();
#if defined(LIGHTUSD_HAVE_TURBOJPEG)
      // Fast SIMD JPEG (also handles RGBA by dropping alpha). Fall back to stb on
      // failure (e.g. an unusual channel count).
      if (EncodeJPEG_turbo(image, q, &out, &err)) {
        return out;
      }
      out.clear();
#endif
      int ret = stbi_write_jpg_to_func(AppendToVector, &out, image.width,
                                       image.height, image.channels,
                                       image.data.data(), q);
      if (ret == 0) {
        return nonstd::make_unexpected("JPEG encode failed.");
      }
      return out;
    }
    case lightusd::image::WriteImageFormat::BMP: {
      if (image.bpp != 8) {
        return nonstd::make_unexpected("8bit only for BMP output.");
      }
      out.clear();
      int ret = stbi_write_bmp_to_func(AppendToVector, &out, image.width,
                                       image.height, image.channels,
                                       image.data.data());
      if (ret == 0) {
        return nonstd::make_unexpected("BMP encode failed.");
      }
      return out;
    }
    case lightusd::image::WriteImageFormat::EXR: {
#if defined(LIGHTUSD_WITH_EXR)
      const int comps = image.channels;
      // EXR here supports 1 (Y), 3 (RGB) or 4 (RGBA). 2-channel is rejected: the
      // fp32 SaveEXRToMemory rejects it too, and the fp16 planar setup below only
      // names 1/3/4 channels (a 2-ch path would leave a null channel pointer ->
      // crash in tinyexr).
      if (comps != 1 && comps != 3 && comps != 4) {
        return nonstd::make_unexpected("EXR: channels must be 1, 3 or 4.");
      }
      if (image.width <= 0 || image.height <= 0) {
        return nonstd::make_unexpected("EXR: invalid image dimensions.");
      }
      size_t npix;
      if (!safe::mul3(size_t(image.width), size_t(image.height), size_t(comps),
                     &npix)) {
        return nonstd::make_unexpected(
            "EXR: image dimensions too large (integer overflow).");
      }

#if defined(LIGHTUSD_EXR_V3)
      // --- TinyEXR v3 C encode: de-interleave the interleaved samples into
      // planar channels (HALF for fp16/8-bit input, FLOAT for fp32), then stream
      // them through the mid-level writer. Channels are matched by name, so no
      // A/B/G/R reordering is needed. ---
      // EXR_OK() is an external TinyEXR macro that uses a C-style cast; suppress
      // -Wold-style-cast for the calls below.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
      {
        size_t pix;
        if (!safe::mul(size_t(image.width), size_t(image.height), &pix)) {
          return nonstd::make_unexpected(
              "EXR: image dimensions too large (integer overflow).");
        }
        const char *kNames1[1] = {"Y"};
        const char *kNames3[3] = {"R", "G", "B"};
        const char *kNames4[4] = {"R", "G", "B", "A"};
        const char *const *names =
            (comps == 1) ? kNames1 : (comps == 3) ? kNames3 : kNames4;

        exr_pixel_type ptype;
        std::vector<std::vector<uint16_t>> hplanes;  // HALF channel planes
        std::vector<std::vector<float>> fplanes;     // FLOAT channel planes

        if (image.format == Image::PixelFormat::Float && image.bpp == 16) {
          if (image.data.size() < npix * 2)
            return nonstd::make_unexpected("EXR: fp16 buffer too small.");
          ptype = EXR_PIXEL_HALF;
          const uint16_t *src =
              reinterpret_cast<const uint16_t *>(image.data.data());
          hplanes.assign(size_t(comps), std::vector<uint16_t>(pix));
          for (size_t i = 0; i < pix; i++)
            for (int c = 0; c < comps; c++)
              hplanes[size_t(c)][i] = src[i * size_t(comps) + size_t(c)];
        } else if (image.format == Image::PixelFormat::Float &&
                   image.bpp == 32) {
          size_t float_bytes;
          if (!safe::mul(npix, sizeof(float), &float_bytes))
            return nonstd::make_unexpected(
                "EXR: float buffer size overflow.");
          if (image.data.size() < float_bytes)
            return nonstd::make_unexpected("EXR: float buffer too small.");
          ptype = EXR_PIXEL_FLOAT;
          const float *src = reinterpret_cast<const float *>(image.data.data());
          fplanes.assign(size_t(comps), std::vector<float>(pix));
          for (size_t i = 0; i < pix; i++)
            for (int c = 0; c < comps; c++)
              fplanes[size_t(c)][i] = src[i * size_t(comps) + size_t(c)];
        } else if (image.bpp == 8) {
          if (image.data.size() < npix)
            return nonstd::make_unexpected("EXR: 8-bit buffer too small.");
          ptype = EXR_PIXEL_HALF;  // normalized 8-bit fits exactly in half
          hplanes.assign(size_t(comps), std::vector<uint16_t>(pix));
          for (size_t i = 0; i < pix; i++)
            for (int c = 0; c < comps; c++) {
              float f =
                  float(image.data[i * size_t(comps) + size_t(c)]) / 255.0f;
              uint16_t h;
              exr_float_to_half(&f, &h, 1);
              hplanes[size_t(c)][i] = h;
            }
        } else {
          return nonstd::make_unexpected("EXR: unsupported source bit depth.");
        }

        exr_header hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.part_type = EXR_PART_SCANLINE;
        hdr.compression = (image.width < 16 && image.height < 16)
                              ? EXR_COMPRESSION_NONE
                              : EXR_COMPRESSION_ZIP;
        hdr.line_order = EXR_LINEORDER_INCREASING_Y;
        hdr.data_window.min_x = 0;
        hdr.data_window.min_y = 0;
        hdr.data_window.max_x = image.width - 1;
        hdr.data_window.max_y = image.height - 1;
        hdr.display_window = hdr.data_window;
        hdr.pixel_aspect_ratio = 1.0f;
        hdr.screen_window_width = 1.0f;
        hdr.num_channels = comps;
        std::vector<exr_channel> chans(static_cast<size_t>(comps));
        for (int c = 0; c < comps; c++) {
          std::memset(&chans[size_t(c)], 0, sizeof(exr_channel));
          std::strncpy(chans[size_t(c)].name, names[c], EXR_MAX_NAME - 1);
          chans[size_t(c)].pixel_type = ptype;
          chans[size_t(c)].x_sampling = 1;
          chans[size_t(c)].y_sampling = 1;
        }
        hdr.channels = chans.data();

        exr_writer *w = nullptr;
        if (!EXR_OK(exr_writer_create(/* alloc */ nullptr, &w)))
          return nonstd::make_unexpected("EXR: writer create failed.");
        int32_t part = 0;
        if (!EXR_OK(exr_writer_add_part(w, &hdr, &part))) {
          exr_writer_destroy(w);
          return nonstd::make_unexpected("EXR: add_part failed.");
        }
        for (int c = 0; c < comps; c++) {
          const void *planar =
              (ptype == EXR_PIXEL_HALF)
                  ? static_cast<const void *>(hplanes[size_t(c)].data())
                  : static_cast<const void *>(fplanes[size_t(c)].data());
          if (!EXR_OK(exr_writer_set_channel(w, part, names[c], planar))) {
            exr_writer_destroy(w);
            return nonstd::make_unexpected("EXR: set_channel failed.");
          }
        }
        void *outdata = nullptr;
        size_t outsize = 0;
        exr_result wr = exr_writer_finalize_to_memory(w, &outdata, &outsize);
        exr_writer_destroy(w);
        if (!EXR_OK(wr) || outdata == nullptr || outsize == 0) {
          if (outdata) free(outdata);
          return nonstd::make_unexpected("EXR: encode failed.");
        }
        std::vector<uint8_t> exr_out(static_cast<uint8_t *>(outdata),
                                     static_cast<uint8_t *>(outdata) + outsize);
        free(outdata);
        return exr_out;
      }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#else
      // fp16 (half) input -> encode HALF directly, with NO fp32 widening:
      // split the interleaved half samples into planar half channels and hand
      // them to SaveEXRImageToMemory (the half values are written as-is).
      if (image.format == Image::PixelFormat::Float && image.bpp == 16) {
        if (image.data.size() < npix * 2) {
          return nonstd::make_unexpected("EXR: fp16 buffer too small.");
        }
        const uint16_t *hsrc =
            reinterpret_cast<const uint16_t *>(image.data.data());
        // npix = pix * comps, comps >= 1 (validated above).
        const size_t pix = npix / size_t(comps);
        std::vector<unsigned short> ch[4];
        for (int c = 0; c < comps; c++) ch[c].resize(pix);
        for (size_t i = 0; i < pix; i++)
          for (int c = 0; c < comps; c++)
            ch[c][i] = hsrc[i * size_t(comps) + size_t(c)];

        // EXR channels are written in (A)BGR order (what most viewers expect).
        unsigned char *image_ptr[4] = {nullptr, nullptr, nullptr, nullptr};
        const char *names[4] = {"R", "G", "B", "A"};
        if (comps == 4) {
          image_ptr[0] = reinterpret_cast<unsigned char *>(ch[3].data());
          image_ptr[1] = reinterpret_cast<unsigned char *>(ch[2].data());
          image_ptr[2] = reinterpret_cast<unsigned char *>(ch[1].data());
          image_ptr[3] = reinterpret_cast<unsigned char *>(ch[0].data());
          names[0] = "A"; names[1] = "B"; names[2] = "G"; names[3] = "R";
        } else if (comps == 3) {
          image_ptr[0] = reinterpret_cast<unsigned char *>(ch[2].data());
          image_ptr[1] = reinterpret_cast<unsigned char *>(ch[1].data());
          image_ptr[2] = reinterpret_cast<unsigned char *>(ch[0].data());
          names[0] = "B"; names[1] = "G"; names[2] = "R";
        } else {  // comps == 1
          image_ptr[0] = reinterpret_cast<unsigned char *>(ch[0].data());
          names[0] = "Y";
        }

        EXRHeader header;
        InitEXRHeader(&header);
        header.compression_type = (image.width < 16 && image.height < 16)
                                      ? TINYEXR_COMPRESSIONTYPE_NONE
                                      : TINYEXR_COMPRESSIONTYPE_ZIP;
        EXRImage exr;
        InitEXRImage(&exr);
        exr.num_channels = comps;
        exr.images = image_ptr;
        exr.width = image.width;
        exr.height = image.height;

        header.num_channels = comps;
        header.channels = static_cast<EXRChannelInfo *>(
            malloc(sizeof(EXRChannelInfo) * size_t(comps)));
        header.pixel_types =
            static_cast<int *>(malloc(sizeof(int) * size_t(comps)));
        header.requested_pixel_types =
            static_cast<int *>(malloc(sizeof(int) * size_t(comps)));
        for (int c = 0; c < comps; c++) {
          std::strncpy(header.channels[c].name, names[c], 255);
          header.channels[c].name[strlen(names[c])] = '\0';
          header.pixel_types[c] = TINYEXR_PIXELTYPE_HALF;       // input is half
          header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_HALF;  // store half
        }

        unsigned char *mem = nullptr;
        const char *exr_err = nullptr;
        size_t msize = SaveEXRImageToMemory(&exr, &header, &mem, &exr_err);
        free(header.channels);
        free(header.pixel_types);
        free(header.requested_pixel_types);
        if (msize == 0 || mem == nullptr) {
          std::string e = exr_err ? std::string(exr_err) : "EXR fp16 encode failed.";
          if (exr_err) FreeEXRErrorMessage(exr_err);
          return nonstd::make_unexpected("EXR: " + e);
        }
        std::vector<uint8_t> out(mem, mem + msize);
        free(mem);
        return out;
      }

      // SaveEXRToMemory expects interleaved fp32. Use the source float data
      // directly when it is already fp32, otherwise promote 8-bit integer data
      // (e.g. when transcoding PNG->EXR) to normalized float.
      std::vector<float> fdata;
      const float *fptr = nullptr;
      if (image.format == Image::PixelFormat::Float && image.bpp == 32) {
        size_t float_bytes;
        if (!safe::mul(npix, sizeof(float), &float_bytes))
          return nonstd::make_unexpected(
              "EXR: float buffer size overflow.");
        if (image.data.size() < float_bytes) {
          return nonstd::make_unexpected("EXR: float buffer too small.");
        }
        fptr = reinterpret_cast<const float *>(image.data.data());
      } else if (image.bpp == 8) {
        if (image.data.size() < npix) {
          return nonstd::make_unexpected("EXR: 8-bit buffer too small.");
        }
        fdata.resize(npix);
        for (size_t i = 0; i < npix; i++) {
          fdata[i] = float(image.data[i]) / 255.0f;
        }
        fptr = fdata.data();
      } else {
        return nonstd::make_unexpected("EXR: unsupported source bit depth.");
      }

      unsigned char *buf = nullptr;
      const char *exr_err = nullptr;
      // Save as fp16 — the common, compact EXR texture encoding.
      int ret = SaveEXRToMemory(fptr, image.width, image.height, comps,
                                /* save_as_fp16 */ 1, &buf, &exr_err);
      if (ret <= 0) {
        std::string e = exr_err ? std::string(exr_err) : "EXR encode failed.";
        if (exr_err) FreeEXRErrorMessage(exr_err);
        return nonstd::make_unexpected("EXR: " + e);
      }
      std::vector<uint8_t> exr_out(buf, buf + size_t(ret));
      free(buf);
      return exr_out;
#endif  // LIGHTUSD_EXR_V3
#else
      return nonstd::make_unexpected(
          "EXR output requires building with LIGHTUSD_WITH_EXR.");
#endif
    }
    case lightusd::image::WriteImageFormat::QOI: {
#if defined(LIGHTUSD_HAVE_QOI)
      if (!EncodeQOI(image, &out, &err)) {
        return nonstd::make_unexpected(err);
      }
      return out;
#else
      return nonstd::make_unexpected(
          "QOI output requires building with LIGHTUSD_WITH_QOI.");
#endif
    }
    case lightusd::image::WriteImageFormat::TIFF:
    case lightusd::image::WriteImageFormat::DNG: {
#if defined(LIGHTUSD_WITH_TIFF)
      if (!EncodeTIFF(image, &out, &err)) {
        return nonstd::make_unexpected(err);
      }
      return out;
#else
      return nonstd::make_unexpected(
          "TIFF/DNG output requires building with LIGHTUSD_WITH_TIFF.");
#endif
    }
    case lightusd::image::WriteImageFormat::Autodetect:
      return nonstd::make_unexpected("Internal error in WriteImageToMemory.");
  }
  // Unreachable but needed for compilers that don't recognize exhaustive switches.
  return nonstd::make_unexpected("Internal error in WriteImageToMemory.");
}

nonstd::expected<std::vector<uint8_t>, std::string> WriteImageLayersToMemory(
    const std::vector<Image> &images, const WriteOption option) {
  if (images.empty()) {
    return nonstd::make_unexpected("At least one image layer is required.");
  }
  if (option.format != WriteImageFormat::Autodetect &&
      option.format != WriteImageFormat::TIFF &&
      option.format != WriteImageFormat::DNG) {
    return nonstd::make_unexpected(
        "Multi-layer memory output supports only TIFF/DNG.");
  }
#if defined(LIGHTUSD_WITH_TIFF)
  std::vector<uint8_t> out;
  std::string err;
  if (!EncodeTIFFLayers(images, &out, &err)) {
    return nonstd::make_unexpected(err);
  }
  return out;
#else
  return nonstd::make_unexpected(
      "TIFF/DNG output requires building with LIGHTUSD_WITH_TIFF.");
#endif
}

nonstd::expected<bool, std::string> WriteImageToFile(
    const std::string &filename, const Image &image, WriteOption option) {
  if (option.format == lightusd::image::WriteImageFormat::Autodetect) {
    lightusd::image::WriteImageFormat detected;
    if (DetectFileFormatFromExtension(io::GetFileExtension(filename),
                                      detected)) {
      option.format = detected;
    }
    // If extension is unknown, leave Autodetect and let WriteImageToMemory pick.
  }

  auto ret = WriteImageToMemory(image, option);
  if (!ret) {
    return nonstd::make_unexpected(ret.error());
  }

  std::string err;
  if (!io::WriteWholeFile(filename, ret.value().data(), ret.value().size(),
                          &err)) {
    return nonstd::make_unexpected("Failed to write image file: " + filename +
                                   " : " + err);
  }

  return true;
}

}  // namespace image
}  // namespace lightusd
