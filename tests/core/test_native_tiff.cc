// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <iostream>
#include <vector>

#include "external/miniz.h"
#include "image-loader.hh"
#include "image-writer.hh"
#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      return 1;                              \
    }                                        \
  } while (0)
static void Put16(std::vector<uint8_t>& b, size_t at, uint16_t n) {
  b[at] = uint8_t(n);
  b[at + 1] = uint8_t(n >> 8);
}
static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t n) {
  for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(n >> (i * 8));
}
static std::vector<uint8_t> TIFF(unsigned bits, bool floating, bool zip,
                                 const std::vector<uint8_t>& data) {
  std::vector<uint8_t> pixels = data;
  if (zip) {
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(data.size()));
    pixels.resize(size);
    if (mz_compress(pixels.data(), &size, data.data(),
                    static_cast<mz_ulong>(data.size())) != MZ_OK)
      return {};
    pixels.resize(size);
  }
  constexpr size_t entries = 11, bitsAt = 8 + 2 + entries * 12 + 4,
                   formatAt = bitsAt + 6, pixelsAt = formatAt + 6;
  std::vector<uint8_t> out(pixelsAt + pixels.size(), 0);
  out[0] = 'I';
  out[1] = 'I';
  Put16(out, 2, 42);
  Put32(out, 4, 8);
  Put16(out, 8, entries);
  size_t index = 0;
  const auto tag = [&](uint16_t id, uint16_t type, uint32_t count,
                       uint32_t value) {
    const size_t offset = 10 + index++ * 12;
    Put16(out, offset, id);
    Put16(out, offset + 2, type);
    Put32(out, offset + 4, count);
    Put32(out, offset + 8, value);
  };
  tag(256, 4, 1, 2);
  tag(257, 4, 1, 1);
  tag(258, 3, 3, bitsAt);
  tag(259, 3, 1, zip ? 8 : 1);
  tag(262, 3, 1, 2);
  tag(273, 4, 1, pixelsAt);
  tag(277, 3, 1, 3);
  tag(278, 4, 1, 1);
  tag(279, 4, 1, static_cast<uint32_t>(pixels.size()));
  tag(284, 3, 1, 1);
  tag(339, 3, 3, formatAt);
  for (int i = 0; i < 3; ++i) {
    Put16(out, bitsAt + size_t(i) * 2, uint16_t(bits));
    Put16(out, formatAt + size_t(i) * 2, floating ? 3 : 1);
  }
  std::memcpy(out.data() + pixelsAt, pixels.data(), pixels.size());
  return out;
}
int main() {
  for (bool zip : {false, true})
    for (unsigned bits : {8u, 16u, 32u}) {
      std::vector<uint8_t> pixels(6 * (bits / 8));
      if (bits == 8)
        pixels = {255, 0, 0, 0, 128, 255};
      else if (bits == 16) {
        for (size_t i = 0; i < 6; ++i)
          Put16(pixels, i * 2, i == 0 ? 65535 : i == 4 ? 32768 : 0);
      } else {
        const float values[] = {2.0f, 0.5f, 0, 0, 1, 4};
        std::memcpy(pixels.data(), values, sizeof(values));
      }
      const auto bytes = TIFF(bits, bits == 32, zip, pixels);
      CHECK(!bytes.empty());
      auto info = lightusd::image::GetImageInfoFromMemory(
          bytes.data(), bytes.size(), "fixture.tiff");
      if (!info) std::cerr << info.error() << "\n";
      CHECK(info);
      CHECK(info->width == 2 && info->height == 1 && info->channels == 3);
      auto image = lightusd::image::LoadImageFromMemoryBounded(
          bytes.data(), bytes.size(), "fixture.tiff", 1024 * 1024);
      if (!image) std::cerr << image.error() << "\n";
      CHECK(image);
      CHECK(image->image.bpp == int(bits));
      CHECK(image->image.channels == 3);
      CHECK(image->image.data == pixels);
      CHECK(!lightusd::image::LoadImageFromMemoryBounded(
          bytes.data(), bytes.size(), "fixture.tiff", 4));
      auto huge = bytes;
      Put32(huge, 18, 1u << 20);
      Put32(huge, 30, 1u << 20);
      CHECK(!lightusd::image::LoadImageFromMemoryBounded(
          huge.data(), huge.size(), "oversized.tiff", 1024 * 1024));
    }
  lightusd::Image rgb;
  rgb.width = 2;
  rgb.height = 1;
  rgb.channels = 3;
  rgb.bpp = 8;
  rgb.data = {255, 0, 0, 0, 128, 255};
  lightusd::image::WriteOption option;
  option.format = lightusd::image::WriteImageFormat::JPEG;
  const auto jpeg = lightusd::image::WriteImageToMemory(rgb, option);
  CHECK(jpeg);
  auto jpegTIFF = TIFF(8, false, false, *jpeg);
  Put16(jpegTIFF, 10 + 3 * 12 + 8, 7);
  auto decodedJPEG = lightusd::image::LoadImageFromMemoryBounded(
      jpegTIFF.data(), jpegTIFF.size(), "jpeg.tiff", 1024 * 1024);
  if (!decodedJPEG) std::cerr << decodedJPEG.error() << "\n";
  CHECK(decodedJPEG);
  CHECK(decodedJPEG->image.data.size() == 6);
  auto jpegBomb = *jpeg;
  bool patched = false;
  for (size_t i = 0; i + 9 < jpegBomb.size(); ++i)
    if (jpegBomb[i] == 0xff &&
        (jpegBomb[i + 1] == 0xc0 || jpegBomb[i + 1] == 0xc2)) {
      jpegBomb[i + 5] = jpegBomb[i + 6] = jpegBomb[i + 7] = jpegBomb[i + 8] =
          255;
      patched = true;
      break;
    }
  CHECK(patched);
  jpegTIFF = TIFF(8, false, false, jpegBomb);
  Put16(jpegTIFF, 10 + 3 * 12 + 8, 7);
  auto rejected = lightusd::image::LoadImageFromMemoryBounded(
      jpegTIFF.data(), jpegTIFF.size(), "jpeg-bomb.tiff", 1024 * 1024);
  CHECK(!rejected);
  CHECK(rejected.error().find("budget") != std::string::npos);
  std::cout << "PASS core native TIFF: raw/deflate RGB8, RGB16, float HDR, "
               "metadata and decode limits\n";
}
