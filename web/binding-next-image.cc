// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "../src/external/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "binding-next-api.h"
#if defined(LIGHTUSD_NEXT_WASM_WITH_EXR)
#include "../src/image-writer.hh"
#include "../src/image-types.hh"
#include <string>
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <emscripten/emscripten.h>

namespace {
constexpr uint32_t kMaxInputBytes = 256u * 1024u * 1024u;
constexpr uint32_t kMaxOutputBytes = 512u * 1024u * 1024u;
#if defined(LIGHTUSD_NEXT_WASM_WITH_EXR)
// WASM execution is single-threaded. The JS wrapper consumes and releases this
// one-shot result synchronously before another image call can run.
std::vector<uint8_t> pending_exr;
#endif

struct OutputSink {
  uint8_t* output;
  size_t capacity;
  size_t size;
  bool overflow;
  bool too_large;
};

void AppendOutput(void* context, void* data, int size) {
  auto* sink = static_cast<OutputSink*>(context);
  if (!sink || !data || size <= 0 || sink->overflow) return;
  const size_t chunk = static_cast<size_t>(size);
  if (chunk > (std::numeric_limits<size_t>::max)() - sink->size ||
      chunk > kMaxOutputBytes - std::min<size_t>(sink->size, kMaxOutputBytes)) {
    sink->too_large = true;
    return;
  }
  if (sink->output && chunk <= sink->capacity - std::min(sink->size, sink->capacity)) {
    std::memcpy(sink->output + sink->size, data, chunk);
  } else if (sink->output) {
    sink->overflow = true;
    return;
  }
  sink->size += chunk;
}

void WriteLE16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLE32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t TiffEncodedSize(uint32_t pixel_bytes, int channels) {
  const uint32_t entries = channels == 2 || channels == 4 ? 12u : 11u;
  return 8u + pixel_bytes + 2u + entries * 12u + 4u +
         static_cast<uint32_t>(channels) * 2u;
}

void WriteTiffEntry(uint8_t* out, uint16_t tag, uint16_t type,
                    uint32_t count, uint32_t value) {
  WriteLE16(out, tag);
  WriteLE16(out + 2, type);
  WriteLE32(out + 4, count);
  if (type == 3 && count == 1) {
    WriteLE16(out + 8, static_cast<uint16_t>(value));
    WriteLE16(out + 10, 0);
  } else if (type == 3 && count == 2) {
    WriteLE16(out + 8, 8);
    WriteLE16(out + 10, 8);
  } else {
    WriteLE32(out + 8, value);
  }
}

bool WriteBaselineTiff(const uint8_t* pixels, uint32_t pixel_bytes,
                       int32_t width, int32_t height, int32_t channels,
                       uint8_t* out, uint32_t cap) {
  const uint32_t required = TiffEncodedSize(pixel_bytes, channels);
  if (!out || cap < required) return false;
  std::memset(out, 0, required);
  out[0] = 'I'; out[1] = 'I';
  WriteLE16(out + 2, 42);
  WriteLE32(out + 4, 8u + pixel_bytes);
  std::memcpy(out + 8, pixels, pixel_bytes);
  uint8_t* ifd = out + 8u + pixel_bytes;
  const uint32_t entries = channels == 2 || channels == 4 ? 12u : 11u;
  WriteLE16(ifd, static_cast<uint16_t>(entries));
  uint8_t* item = ifd + 2;
  const uint32_t bits_offset = 8u + pixel_bytes + 2u + entries * 12u + 4u;
  WriteTiffEntry(item, 256, 4, 1, static_cast<uint32_t>(width)); item += 12;
  WriteTiffEntry(item, 257, 4, 1, static_cast<uint32_t>(height)); item += 12;
  WriteTiffEntry(item, 258, 3, static_cast<uint32_t>(channels),
                 channels <= 2 ? 8u : bits_offset); item += 12;
  WriteTiffEntry(item, 259, 3, 1, 1); item += 12;
  WriteTiffEntry(item, 262, 3, 1, channels <= 2 ? 1 : 2); item += 12;
  WriteTiffEntry(item, 273, 4, 1, 8); item += 12;
  WriteTiffEntry(item, 274, 3, 1, 1); item += 12;
  WriteTiffEntry(item, 277, 3, 1, static_cast<uint32_t>(channels)); item += 12;
  WriteTiffEntry(item, 278, 4, 1, static_cast<uint32_t>(height)); item += 12;
  WriteTiffEntry(item, 279, 4, 1, pixel_bytes); item += 12;
  WriteTiffEntry(item, 284, 3, 1, 1); item += 12;
  if (channels == 2 || channels == 4) {
    WriteTiffEntry(item, 338, 3, 1, 2);
    item += 12;
  }
  WriteLE32(item, 0);
  uint8_t* bits = out + bits_offset;
  for (int32_t i = 0; i < channels; ++i) WriteLE16(bits + i * 2, 8);
  return true;
}
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_encode_image(
    const uint8_t* pixels, uint32_t pixel_size, int32_t width, int32_t height,
    int32_t channels, const uint8_t* format, uint32_t format_size,
    uint8_t* out, uint32_t cap) {
  if ((!pixels && pixel_size) || (!format && format_size) || (!out && cap)) {
    return -1;
  }
  constexpr int32_t kMaxDimension = 65536;
  if (width <= 0 || height <= 0 || channels < 1 || channels > 4 ||
      width > kMaxDimension || height > kMaxDimension) {
    return 2;
  }
  const uint64_t required = static_cast<uint64_t>(width) *
      static_cast<uint64_t>(height) * static_cast<uint64_t>(channels);
  if (required > kMaxInputBytes || pixel_size < required) return -1;
  const bool png = format_size == 3 && std::memcmp(format, "png", 3) == 0;
  const bool bmp = format_size == 3 && std::memcmp(format, "bmp", 3) == 0;
  const bool tiff = format_size == 4 && std::memcmp(format, "tiff", 4) == 0;
  const bool dng = format_size == 3 && std::memcmp(format, "dng", 3) == 0;
  const bool exr = format_size == 3 && std::memcmp(format, "exr", 3) == 0;
  if (!png && !bmp && !tiff && !dng && !exr) return -2;

  if (exr) {
#if defined(LIGHTUSD_NEXT_WASM_WITH_EXR)
    if (channels != 1 && channels != 3 && channels != 4) return -1;
    const uint64_t estimate = static_cast<uint64_t>(width) *
        static_cast<uint64_t>(height) * static_cast<uint64_t>(channels) * 2u +
        static_cast<uint64_t>(height) * 64u + 65536u;
    if (estimate > kMaxOutputBytes ||
        estimate > static_cast<uint64_t>((std::numeric_limits<int32_t>::max)())) {
      return -3;
    }
    if (!out && cap == 0) {
      lightusd::Image image;
      image.width = width;
      image.height = height;
      image.channels = channels;
      image.bpp = 8;
      image.format = lightusd::Image::PixelFormat::UInt;
      image.data.assign(pixels, pixels + required);
      lightusd::image::WriteOption option;
      option.format = lightusd::image::WriteImageFormat::EXR;
      auto result = lightusd::image::WriteImageToMemory(image, option);
      if (!result) return -1;
      if (result->size() > kMaxOutputBytes ||
          result->size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
        return -3;
      }
      pending_exr = std::move(result.value());
      return static_cast<int32_t>(pending_exr.size());
    }
    if (!out || cap > kMaxOutputBytes) return -1;
    if (!pending_exr.empty()) {
      if (pending_exr.size() > cap) return -1;
      std::memcpy(out, pending_exr.data(), pending_exr.size());
      const int32_t written = static_cast<int32_t>(pending_exr.size());
      std::vector<uint8_t>().swap(pending_exr);
      return written;
    }
    lightusd::Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.bpp = 8;
    image.format = lightusd::Image::PixelFormat::UInt;
    image.data.assign(pixels, pixels + required);
    lightusd::image::WriteOption option;
    option.format = lightusd::image::WriteImageFormat::EXR;
    auto result = lightusd::image::WriteImageToMemory(image, option);
    if (!result) return -1;
    if (result->size() > cap) return -1;
    std::memcpy(out, result->data(), result->size());
    return static_cast<int32_t>(result->size());
#else
    return -2;
#endif
  }

  if (tiff || dng) {
    const uint32_t output_size = TiffEncodedSize(
        static_cast<uint32_t>(required), channels);
    if (output_size > kMaxOutputBytes ||
        output_size > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)())) {
      return -3;
    }
    if (!out && cap == 0) return static_cast<int32_t>(output_size);
    if (!out || cap < output_size) return -1;
    return WriteBaselineTiff(pixels, static_cast<uint32_t>(required), width,
                             height, channels, out, cap)
        ? static_cast<int32_t>(output_size) : -1;
  }

  OutputSink sink{out, cap, 0, false, false};
  const int stride = width * channels;
  const int success = png
      ? stbi_write_png_to_func(AppendOutput, &sink, width, height, channels,
                               pixels, stride)
      : stbi_write_bmp_to_func(AppendOutput, &sink, width, height, channels,
                               pixels);
  if (!success) return 0;
  if (sink.too_large || sink.size > kMaxOutputBytes ||
      sink.size > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    return -3;
  }
  if (sink.overflow || (out && cap < sink.size)) return -1;
  return static_cast<int32_t>(sink.size);
}

#if defined(LIGHTUSD_NEXT_WASM_WITH_EXR)
extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t
lightusd_next_encoded_image_data(void) {
  return pending_exr.empty()
      ? 0
      : reinterpret_cast<uintptr_t>(pending_exr.data());
}

extern "C" EMSCRIPTEN_KEEPALIVE void
lightusd_next_encoded_image_release(void) {
  std::vector<uint8_t>().swap(pending_exr);
}
#endif
