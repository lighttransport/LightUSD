// SPDX-License-Identifier: Apache-2.0

#include "lmv-ptex.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ptx-loader.hh"

namespace lmv {
namespace {

size_t BytesPerComponent(lightusd::ptx::DataType type) {
  switch (type) {
    case lightusd::ptx::DataType::UInt8: return 1;
    case lightusd::ptx::DataType::UInt16:
    case lightusd::ptx::DataType::Half: return 2;
    case lightusd::ptx::DataType::Float: return 4;
  }
  return 1;
}

uint32_t NextPow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa ? std::nanf("") : INFINITY;
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400u),
                       static_cast<int>(exponent) - 25);
  }
  return sign ? -value : value;
}

uint8_t ToByte(float v) {
  if (!std::isfinite(v)) v = 0.0f;
  v = std::max(0.0f, std::min(1.0f, v));
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

float Component(const uint8_t* data, uint16_t channels, uint32_t x,
                uint32_t y, uint32_t width, uint32_t channel,
                lightusd::ptx::DataType type) {
  const size_t index = (static_cast<size_t>(y) * width + x) * channels + channel;
  const uint8_t* p = data + index * BytesPerComponent(type);
  switch (type) {
    case lightusd::ptx::DataType::UInt8:
      return static_cast<float>(*p) / 255.0f;
    case lightusd::ptx::DataType::UInt16: {
      uint16_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<float>(v) / 65535.0f;
    }
    case lightusd::ptx::DataType::Half: {
      uint16_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return HalfToFloat(v);
    }
    case lightusd::ptx::DataType::Float: {
      float v = 0.0f;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
  }
  return 0.0f;
}

}  // namespace

bool BuildPtexAtlas(const std::string& path, const uint8_t* data, size_t size,
                    const PtexAtlasOptions& options, PtexAtlas* out,
                    std::string* error) {
  if (!out) return false;
  *out = PtexAtlas{};
  lightusd::ptx::Reader reader;
  std::string err;
  const bool opened =
      data ? lightusd::ptx::Reader::OpenMemory(data, size, &reader, &err)
           : lightusd::ptx::Reader::OpenFile(path, &reader, &err);
  if (!opened) {
    if (error) *error = err.empty() ? "cannot open Ptex file" : err;
    return false;
  }
  const lightusd::ptx::Info& info = reader.info();
  if (info.faces == 0 || info.channels == 0) {
    if (error) *error = "empty Ptex file";
    return false;
  }

  const uint32_t face_count =
      std::min<uint32_t>(info.faces, options.max_faces);
  const uint32_t tile = std::max<uint32_t>(4, NextPow2(options.max_face_edge));
  const uint32_t columns = std::max<uint32_t>(
      1, std::min<uint32_t>(face_count, std::min<uint32_t>(
                                           64, options.max_atlas_edge / tile)));
  uint32_t rows = (face_count + columns - 1) / columns;
  const uint32_t max_rows =
      std::max<uint32_t>(1, options.max_atlas_edge / tile);
  if (rows > max_rows) {
    rows = max_rows;
    if (error) *error = "Ptex atlas face budget truncated";
  }
  const uint32_t placed_faces = std::min<uint32_t>(face_count, columns * rows);
  const uint32_t width = columns * tile;
  const uint32_t height = rows * tile;

  out->width = width;
  out->height = height;
  out->face_count = info.faces;
  out->rgba.assign(static_cast<size_t>(width) * height * 4, 0);
  out->rects.assign(info.faces, PtexRect{});

  uint64_t decoded_bytes = 0;
  const size_t bpc = BytesPerComponent(info.dataType);
  for (uint32_t face = 0; face < placed_faces; ++face) {
    const lightusd::ptx::FaceInfo& face_info = info.faceInfo[face];
    // Pick the smallest mip whose longest edge fits max_face_edge.
    uint32_t level = 0;
    uint32_t fw = std::max(1u, face_info.width());
    uint32_t fh = std::max(1u, face_info.height());
    while (level + 1 < info.levels &&
           std::max(fw, fh) > options.max_face_edge) {
      fw = std::max(1u, fw >> 1);
      fh = std::max(1u, fh >> 1);
      ++level;
    }
    const size_t needed =
        static_cast<size_t>(fw) * fh * info.channels * bpc;
    if (decoded_bytes + needed > options.max_decoded_bytes) break;
    lightusd::ptx::FaceImage image;
    if (!reader.ReadFace(face, level, needed, &image, &err) || image.width == 0 ||
        image.height == 0) {
      continue;
    }
    decoded_bytes += image.data.size();

    const uint32_t col = face % columns;
    const uint32_t row = face / columns;
    const uint32_t origin_x = col * tile;
    const uint32_t origin_y = row * tile;
    const uint32_t w = std::min(image.width, tile);
    const uint32_t h = std::min(image.height, tile);
    const uint32_t alpha_channel =
        info.alphaChannel < image.channels ? info.alphaChannel : image.channels;

    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t* dst =
            out->rgba.data() + (static_cast<size_t>(origin_y + y) * width +
                                origin_x + x) *
                                   4;
        for (int c = 0; c < 3; ++c) {
          const uint32_t channel = std::min<uint32_t>(c, image.channels - 1);
          dst[c] = ToByte(Component(image.data.data(), image.channels, x, y,
                                    image.width, channel, image.dataType));
        }
        dst[3] = alpha_channel < image.channels
                     ? ToByte(Component(image.data.data(), image.channels, x, y,
                                        image.width, alpha_channel,
                                        image.dataType))
                     : 255;
      }
    }
    // 1-texel gutter (replicate the border) so linear sampling at the face edge
    // cannot bleed into an adjacent face.
    for (uint32_t y = 0; y < h; ++y) {
      const size_t row_base =
          static_cast<size_t>(origin_y + y) * width + origin_x;
      if (origin_x > 0) {
        std::memcpy(out->rgba.data() + (row_base - 1) * 4,
                    out->rgba.data() + row_base * 4, 4);
      }
      if (origin_x + w < width) {
        std::memcpy(out->rgba.data() + (row_base + w) * 4,
                    out->rgba.data() + (row_base + w - 1) * 4, 4);
      }
    }
    PtexRect rect;
    rect.x = static_cast<float>(origin_x) / static_cast<float>(width);
    rect.w = static_cast<float>(w > 1 ? w - 1 : 1) / static_cast<float>(width);
    rect.y = static_cast<float>(origin_y) / static_cast<float>(height);
    rect.h = static_cast<float>(h > 1 ? h - 1 : 1) / static_cast<float>(height);
    out->rects[face] = rect;
  }
  (void)path;
  return out->valid();
}

}  // namespace lmv
