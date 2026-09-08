#define TEST_NO_MAIN
#include "acutest.h"
#include "unit-tiff.h"

#include "image-loader.hh"
#include "image-writer.hh"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v));
  out->push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void Put64(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(v >> (i * 8)));
}
std::vector<uint8_t> MakeRGBTiff(bool bigtiff) {
  constexpr uint16_t kEntries = 12;
  const size_t header = bigtiff ? 16u : 8u;
  const size_t entryBytes = bigtiff ? 20u : 12u;
  const size_t countBytes = bigtiff ? 8u : 2u;
  const size_t nextBytes = bigtiff ? 8u : 4u;
  const size_t ifd = header + countBytes;
  const size_t bitsOffset = ifd + kEntries * entryBytes + nextBytes;
  const size_t pixelsOffset = bitsOffset + 6u;
  std::vector<uint8_t> out;
  out.reserve(pixelsOffset + 12u);
  Put16(&out, 0x4949);
  Put16(&out, bigtiff ? 43u : 42u);
  if (bigtiff) {
    Put16(&out, 8);  // BigTIFF offset size.
    Put16(&out, 0);  // Reserved.
    Put64(&out, header);
  } else {
    Put32(&out, static_cast<uint32_t>(header));
  }
  if (bigtiff) Put64(&out, kEntries); else Put16(&out, kEntries);

  auto entry = [&](uint16_t tag, uint16_t type, uint64_t count,
                   uint64_t value, bool external = false) {
    Put16(&out, tag);
    Put16(&out, type);
    if (bigtiff) Put64(&out, count); else Put32(&out, static_cast<uint32_t>(count));
    if (external) {
      if (bigtiff) Put64(&out, value); else Put32(&out, static_cast<uint32_t>(value));
    } else {
      const size_t slot = bigtiff ? 8u : 4u;
      for (size_t i = 0; i < slot; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
  };
  entry(256, 3, 1, 2);                         // ImageWidth.
  entry(257, 3, 1, 2);                         // ImageLength.
  entry(258, 3, 3, bitsOffset, true);          // BitsPerSample.
  entry(259, 3, 1, 1);                         // Compression: none.
  entry(262, 3, 1, 2);                         // Photometric: RGB.
  entry(273, bigtiff ? 16 : 4, 1, pixelsOffset);  // StripOffsets.
  entry(277, 3, 1, 3);                         // SamplesPerPixel.
  entry(278, 4, 1, 2);                         // RowsPerStrip.
  entry(279, bigtiff ? 16 : 4, 1, 12);          // StripByteCounts.
  entry(284, 3, 1, 1);                         // PlanarConfiguration.
  entry(339, 3, 1, 1);                         // SampleFormat: uint.
  entry(274, 3, 1, 1);                         // Orientation.
  if (bigtiff) Put64(&out, 0); else Put32(&out, 0);
  Put16(&out, 8); Put16(&out, 8); Put16(&out, 8);
  const uint8_t pixels[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  out.insert(out.end(), pixels, pixels + sizeof(pixels));
  return out;
}

std::vector<uint8_t> MakePlanarRGBTiff() {
  constexpr uint16_t entries = 12;
  const uint32_t ifd = 8;
  const uint32_t bits_offset = ifd + 2 + entries * 12 + 4;
  const uint32_t strip_offsets_offset = bits_offset + 6;
  const uint32_t strip_counts_offset = strip_offsets_offset + 12;
  const uint32_t pixels_offset = strip_counts_offset + 12;
  std::vector<uint8_t> out;
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, ifd);
  Put16(&out, entries);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, 2);                         // width
  entry(257, 4, 1, 2);                         // height
  entry(258, 3, 3, bits_offset);                // bits per sample
  entry(259, 3, 1, 1);                         // uncompressed
  entry(262, 3, 1, 2);                         // RGB
  entry(273, 4, 3, strip_offsets_offset);       // planar strip offsets
  entry(277, 3, 1, 3);                         // samples per pixel
  entry(278, 4, 1, 2);                         // rows per strip
  entry(279, 4, 3, strip_counts_offset);        // planar strip sizes
  entry(284, 3, 1, 2);                         // separate planar data
  entry(339, 3, 1, 1);                         // unsigned integer
  entry(274, 3, 1, 1);                         // orientation
  Put32(&out, 0);
  Put16(&out, 8); Put16(&out, 8); Put16(&out, 8);
  Put32(&out, pixels_offset);
  Put32(&out, pixels_offset + 4);
  Put32(&out, pixels_offset + 8);
  Put32(&out, 4); Put32(&out, 4); Put32(&out, 4);
  // R, G, and B planes, each with two rows and two pixels. libtiff exposes
  // each normalized scanline independently of the file's strip layout.
  const uint8_t pixels[] = {255, 0, 0, 0, 0, 255, 0, 0,
                            0, 0, 255, 0};
  out.insert(out.end(), pixels, pixels + sizeof(pixels));
  return out;
}

std::vector<uint8_t> MakeTiledGrayTiff(bool packbits = false) {
  // A 4x4, 8-bit grayscale image with four 2x2 tiles.  The tile values are
  // deliberately distinct so both tile ordering and edge addressing are
  // observable in the decoded raster.
  constexpr uint16_t entries = 12;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t offsets_offset = ifd + 2 + entries * 12 + 4;
  constexpr uint32_t counts_offset = offsets_offset + 4 * 4;
  constexpr uint32_t pixels_offset = counts_offset + 4 * 4;
  const uint32_t encoded_tile_bytes = packbits ? 5u : 4u;
  std::vector<uint8_t> out;
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, ifd);
  Put16(&out, entries);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, 4);                  // ImageWidth.
  entry(257, 4, 1, 4);                  // ImageLength.
  entry(258, 3, 1, 8);                  // BitsPerSample.
  entry(259, 3, 1, packbits ? 32773u : 1u); // None or TIFF PackBits.
  entry(262, 3, 1, 1);                  // Photometric: grayscale.
  entry(277, 3, 1, 1);                  // SamplesPerPixel.
  entry(284, 3, 1, 1);                  // PlanarConfiguration: chunky.
  entry(322, 4, 1, 2);                  // TileWidth.
  entry(323, 4, 1, 2);                  // TileLength.
  entry(324, 4, 4, offsets_offset);     // TileOffsets.
  entry(325, 4, 4, counts_offset);      // TileByteCounts.
  entry(339, 3, 1, 1);                  // SampleFormat: unsigned integer.
  Put32(&out, 0);                       // No next IFD.
  for (uint32_t tile = 0; tile < 4; ++tile) {
    Put32(&out, pixels_offset + tile * encoded_tile_bytes);
  }
  for (int tile = 0; tile < 4; ++tile) Put32(&out, encoded_tile_bytes);
  const uint8_t pixels[] = {
      1, 2, 3, 4,  5, 6, 7, 8,
      9, 10, 11, 12, 13, 14, 15, 16};
  if (!packbits) {
    out.insert(out.end(), pixels, pixels + sizeof(pixels));
  } else {
    for (int tile = 0; tile < 4; ++tile) {
      out.push_back(3);  // Four literal bytes follow.
      out.insert(out.end(), pixels + tile * 4, pixels + tile * 4 + 4);
    }
  }
  return out;
}

std::vector<uint8_t> MakeStripPackBitsGrayTiff() {
  constexpr uint16_t entries = 11;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t pixels_offset = ifd + 2 + entries * 12 + 4;
  std::vector<uint8_t> out;
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, ifd);
  Put16(&out, entries);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, 4);                  // ImageWidth.
  entry(257, 4, 1, 2);                  // ImageLength.
  entry(258, 3, 1, 8);                  // BitsPerSample.
  entry(259, 3, 1, 32773);              // PackBits.
  entry(262, 3, 1, 1);                  // Grayscale.
  entry(273, 4, 1, pixels_offset);      // StripOffsets.
  entry(277, 3, 1, 1);                  // SamplesPerPixel.
  entry(278, 4, 1, 2);                  // RowsPerStrip.
  entry(279, 4, 1, 9);                  // StripByteCounts.
  entry(284, 3, 1, 1);                  // Chunky.
  entry(339, 3, 1, 1);                  // Unsigned integer.
  Put32(&out, 0);
  out.push_back(7);  // Eight literal bytes follow.
  const uint8_t pixels[] = {1, 2, 3, 4, 5, 6, 7, 8};
  out.insert(out.end(), pixels, pixels + sizeof(pixels));
  return out;
}

std::vector<uint8_t> MakePlanarStripPackBitsRGBTiff() {
  constexpr uint16_t entries = 12;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t bits_offset = ifd + 2 + entries * 12 + 4;
  constexpr uint32_t offsets_offset = bits_offset + 2;
  constexpr uint32_t counts_offset = offsets_offset + 12;
  constexpr uint32_t pixels_offset = counts_offset + 12;
  std::vector<uint8_t> out;
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, ifd);
  Put16(&out, entries);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, 2);                  // ImageWidth.
  entry(257, 4, 1, 2);                  // ImageLength.
  entry(258, 3, 1, 8);                  // BitsPerSample.
  entry(259, 3, 1, 32773);              // PackBits.
  entry(262, 3, 1, 2);                  // RGB.
  entry(273, 4, 3, offsets_offset);     // Planar strip offsets.
  entry(277, 3, 1, 3);                  // SamplesPerPixel.
  entry(278, 4, 1, 2);                  // RowsPerStrip.
  entry(279, 4, 3, counts_offset);      // Planar strip byte counts.
  entry(284, 3, 1, 2);                  // Separate planes.
  entry(339, 3, 1, 1);                  // Unsigned integer.
  entry(274, 3, 1, 1);                  // Orientation.
  Put32(&out, 0);
  Put16(&out, 8);
  for (uint32_t plane = 0; plane < 3; ++plane) {
    Put32(&out, pixels_offset + plane * 5);
  }
  Put32(&out, 5); Put32(&out, 5); Put32(&out, 5);
  const uint8_t planes[3][4] = {{255, 0, 255, 0},
                                {0, 255, 0, 255},
                                {0, 0, 255, 255}};
  for (const auto &plane : planes) {
    out.push_back(3);  // Four literal bytes.
    out.insert(out.end(), plane, plane + 4);
  }
  return out;
}

std::vector<uint8_t> MakePackedGrayTiff(int bits) {
  const uint32_t width = bits == 12 ? 2u : 4u;
  const uint32_t height = 1u;
  const uint16_t values[] = {0u, 1u, 8192u, 16383u};
  const uint32_t value_count = width * height;
  const size_t packed_size = (size_t(value_count) * size_t(bits) + 7u) / 8u;
  const uint16_t entry_count = 11;
  const size_t ifd = 8u;
  const size_t pixels_offset = ifd + 2u + size_t(entry_count) * 12u + 4u;

  std::vector<uint8_t> out;
  out.reserve(pixels_offset + packed_size);
  Put16(&out, 0x4949);  // Little endian.
  Put16(&out, 42);
  Put32(&out, static_cast<uint32_t>(ifd));
  Put16(&out, entry_count);

  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, width);                         // ImageWidth.
  entry(257, 4, 1, height);                        // ImageLength.
  entry(258, 3, 1, static_cast<uint32_t>(bits));   // BitsPerSample.
  entry(259, 3, 1, 1);                             // Compression: none.
  entry(262, 3, 1, 1);                             // Photometric: grayscale.
  entry(273, 4, 1, static_cast<uint32_t>(pixels_offset));
  entry(277, 3, 1, 1);                             // SamplesPerPixel.
  entry(278, 4, 1, height);                        // RowsPerStrip.
  entry(279, 4, 1, static_cast<uint32_t>(packed_size));
  entry(284, 3, 1, 1);                             // PlanarConfiguration.
  entry(339, 3, 1, 1);                             // SampleFormat: uint.
  Put32(&out, 0);                                  // No next IFD.

  out.insert(out.end(), packed_size, uint8_t(0));
  for (uint32_t i = 0; i < value_count; ++i) {
    const uint32_t value = values[i];
    for (int bit = 0; bit < bits; ++bit) {
      const size_t stream_bit = size_t(i) * size_t(bits) + size_t(bit);
      if ((value >> (bits - 1 - bit)) & 1u) {
        out[pixels_offset + stream_bit / 8u] |=
            static_cast<uint8_t>(1u << (7u - stream_bit % 8u));
      }
    }
  }
  return out;
}

std::vector<uint8_t> MakePackedPlanarRGBTiff() {
  constexpr int bits = 12;
  constexpr uint32_t width = 2;
  constexpr uint32_t height = 1;
  constexpr uint16_t entry_count = 11;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t bits_offset = ifd + 2 + entry_count * 12 + 4;
  constexpr uint32_t offsets_offset = bits_offset + 6;
  constexpr uint32_t counts_offset = offsets_offset + 12;
  constexpr uint32_t pixels_offset = counts_offset + 12;
  constexpr size_t plane_bytes = 3;

  std::vector<uint8_t> out;
  out.reserve(pixels_offset + 3 * plane_bytes);
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, ifd);
  Put16(&out, entry_count);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    Put16(&out, tag);
    Put16(&out, type);
    Put32(&out, count);
    Put32(&out, value);
  };
  entry(256, 4, 1, width);
  entry(257, 4, 1, height);
  entry(258, 3, 3, bits_offset);
  entry(259, 3, 1, 1);
  entry(262, 3, 1, 2);
  entry(273, 4, 3, offsets_offset);
  entry(277, 3, 1, 3);
  entry(278, 4, 1, height);
  entry(279, 4, 3, counts_offset);
  entry(284, 3, 1, 2);
  entry(339, 3, 1, 1);
  Put32(&out, 0);
  Put16(&out, bits);
  Put16(&out, bits);
  Put16(&out, bits);
  for (uint32_t plane = 0; plane < 3; ++plane) {
    Put32(&out, pixels_offset + plane * plane_bytes);
  }
  for (int plane = 0; plane < 3; ++plane) Put32(&out, plane_bytes);
  out.insert(out.end(), 3 * plane_bytes, uint8_t(0));

  const uint16_t values[3][2] = {{0, 4095}, {100, 200}, {300, 400}};
  for (size_t plane = 0; plane < 3; ++plane) {
    for (size_t sample = 0; sample < 2; ++sample) {
      const uint16_t v = values[plane][sample];
      for (int bit = 0; bit < bits; ++bit) {
        const size_t stream_bit = sample * bits + size_t(bit);
        if ((v >> (bits - 1 - bit)) & 1u) {
          out[pixels_offset + plane * plane_bytes + stream_bit / 8u] |=
              static_cast<uint8_t>(1u << (7u - stream_bit % 8u));
        }
      }
    }
  }
  return out;
}

std::vector<uint8_t> MakeMultiDirectoryGrayTiff() {
  constexpr uint16_t entries = 11;
  constexpr uint32_t first_ifd = 8;
  constexpr uint32_t second_ifd = first_ifd + 2 + entries * 12 + 4;
  constexpr uint32_t pixels = second_ifd + 2 + entries * 12 + 4;
  std::vector<uint8_t> out;
  Put16(&out, 0x4949);
  Put16(&out, 42);
  Put32(&out, first_ifd);

  auto append_ifd = [&](uint32_t next, uint32_t pixel_offset) {
    Put16(&out, entries);
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                     uint32_t inline_value) {
      Put16(&out, tag);
      Put16(&out, type);
      Put32(&out, count);
      Put32(&out, inline_value);
    };
    entry(256, 3, 1, 1);             // width
    entry(257, 3, 1, 1);             // height
    entry(258, 3, 1, 8);             // bits per sample
    entry(259, 3, 1, 1);             // uncompressed
    entry(262, 3, 1, 1);             // grayscale
    entry(273, 4, 1, pixel_offset);  // strip offset
    entry(277, 3, 1, 1);             // samples per pixel
    entry(278, 3, 1, 1);             // rows per strip
    entry(279, 4, 1, 1);             // strip byte count
    entry(284, 3, 1, 1);             // chunky
    entry(339, 3, 1, 1);             // unsigned integer
    Put32(&out, next);
  };
  append_ifd(second_ifd, pixels);
  append_ifd(0, pixels + 1);
  out.push_back(0x11);
  out.push_back(0x22);
  return out;
}

void CheckRGBTiff(const std::vector<uint8_t>& bytes) {
#if defined(LIGHTUSD_WITH_TIFF)
  auto info = lightusd::image::GetImageInfoFromMemory(bytes.data(), bytes.size(), "fixture.tif");
  TEST_CHECK(info.has_value());
  if (!info) return;
  TEST_CHECK(info->width == 2 && info->height == 2 && info->channels == 3);
  auto image = lightusd::image::LoadImageFromMemory(bytes.data(), bytes.size(), "fixture.tif");
  TEST_CHECK(image.has_value());
  if (!image) return;
  TEST_CHECK(image->image.width == 2 && image->image.height == 2);
  TEST_CHECK(image->image.channels == 3 || image->image.channels == 4);
#else
  (void)bytes;
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; TIFF fixture skipped");
#endif
}

}  // namespace

void tinydng_classic_tiff_test(void) { CheckRGBTiff(MakeRGBTiff(false)); }
void tinydng_layers_api_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  const auto bytes = MakeMultiDirectoryGrayTiff();
  auto layers = lightusd::image::LoadImageLayersFromMemory(
      bytes.data(), bytes.size(), "layers.tif");
  TEST_CHECK(layers.has_value());
  if (layers) {
    TEST_CHECK(layers->size() == 2);
    if (layers->size() == 2) {
      TEST_CHECK((*layers)[0].image.width == 1);
      TEST_CHECK((*layers)[1].image.width == 1);
      TEST_CHECK((*layers)[0].image.data.size() == 1);
      TEST_CHECK((*layers)[1].image.data.size() == 1);
      if ((*layers)[0].image.data.size() == 1 &&
          (*layers)[1].image.data.size() == 1) {
        TEST_CHECK((*layers)[0].image.data[0] == 0x11);
        TEST_CHECK((*layers)[1].image.data[0] == 0x22);
      }
    }
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; TIFF layer API skipped");
#endif
}

void tinydng_bigtiff_test(void) {
  const auto bytes = MakeRGBTiff(true);
  CheckRGBTiff(bytes);
#if defined(LIGHTUSD_WITH_TIFF)
  // Move only the StripOffsets metadata above 4 GiB. Metadata queries must
  // retain the 64-bit value without attempting to decode the unavailable
  // pixel payload.
  auto highOffset = bytes;
  const size_t ifd = 16u + 8u;
  const size_t stripOffsetEntry = ifd + 5u * 20u;
  const size_t value = stripOffsetEntry + 12u;
  const uint64_t high = 0x100000000ull;
  for (int i = 0; i < 8; ++i) highOffset[value + i] = static_cast<uint8_t>(high >> (i * 8));
  auto highInfo = lightusd::image::GetImageInfoFromMemory(
      highOffset.data(), highOffset.size(), "high-offset-bigtiff.tif");
  TEST_CHECK(highInfo.has_value());
  if (highInfo) TEST_CHECK(highInfo->width == 2 && highInfo->height == 2);

  const std::string path = "/tmp/lightusd-unit-bigtiff.tif";
  {
    std::ofstream file(path, std::ios::binary);
    TEST_CHECK(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  auto info = lightusd::image::GetImageInfoFromFile(path);
  TEST_CHECK(info.has_value());
  if (info) TEST_CHECK(info->width == 2 && info->height == 2 && info->channels == 3);
  std::remove(path.c_str());
#endif
}

void tinydng_writer_roundtrip_test(void) {
  lightusd::Image image;
  image.width = 2;
  image.height = 2;
  image.channels = 3;
  image.bpp = 8;
  image.format = lightusd::Image::PixelFormat::UInt;
  image.data = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};

  lightusd::image::WriteOption option;
  option.format = lightusd::image::WriteImageFormat::TIFF;
  auto encoded = lightusd::image::WriteImageToMemory(image, option);
  if (encoded) {
    CheckRGBTiff(encoded.value());
  } else {
    // A build with TIFF support disabled should fail explicitly rather than
    // silently selecting another encoder.
    TEST_CHECK(encoded.error().find("LIGHTUSD_WITH_TIFF") != std::string::npos);
  }
}

void tinydng_writer_bitdepth_roundtrip_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  for (const auto format : {lightusd::Image::PixelFormat::UInt,
                            lightusd::Image::PixelFormat::Float}) {
    lightusd::Image image;
    image.width = 2;
    image.height = 1;
    image.channels = 1;
    image.bpp = format == lightusd::Image::PixelFormat::Float ? 32 : 16;
    image.format = format;
    if (format == lightusd::Image::PixelFormat::Float) {
      const float pixels[] = {0.25f, 1.5f};
      const auto *bytes = reinterpret_cast<const uint8_t *>(pixels);
      image.data.assign(bytes, bytes + sizeof(pixels));
    } else {
      const uint16_t pixels[] = {0, 65535};
      const auto *bytes = reinterpret_cast<const uint8_t *>(pixels);
      image.data.assign(bytes, bytes + sizeof(pixels));
    }

    lightusd::image::WriteOption option;
    option.format = lightusd::image::WriteImageFormat::TIFF;
    auto encoded = lightusd::image::WriteImageToMemory(image, option);
    TEST_CHECK(encoded.has_value());
    if (!encoded) continue;
    auto decoded = lightusd::image::LoadImageFromMemory(
        encoded->data(), encoded->size(), "bitdepth.tif");
    TEST_CHECK(decoded.has_value());
    if (decoded) {
      TEST_CHECK(decoded->image.width == 2 && decoded->image.height == 1);
      TEST_CHECK(decoded->image.channels == 1);
      TEST_CHECK(decoded->image.bpp == image.bpp);
      TEST_CHECK(decoded->image.format == format);
      TEST_CHECK(decoded->image.data.size() >= image.data.size());
    }
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; writer bit-depth test skipped");
#endif
}

void tinydng_packed_integer_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  for (const int bits : {10, 12, 14}) {
    const auto bytes = MakePackedGrayTiff(bits);
    auto decoded = lightusd::image::LoadImageFromMemory(
        bytes.data(), bytes.size(), "packed-gray.tif");
    TEST_CHECK(decoded.has_value());
    if (!decoded) continue;
    const uint32_t width = bits == 12 ? 2u : 4u;
    TEST_CHECK(decoded->image.width == int(width));
    TEST_CHECK(decoded->image.height == 1);
    TEST_CHECK(decoded->image.channels == 1);
    TEST_CHECK(decoded->image.bpp == 16);
    TEST_CHECK(decoded->image.format == lightusd::Image::PixelFormat::UInt);
    TEST_CHECK(decoded->image.data.size() == size_t(width) * sizeof(uint16_t));
    const uint16_t expected[] = {0u, 1u, 8192u, 16383u};
    for (uint32_t i = 0; i < width; ++i) {
      uint16_t actual = 0;
      std::memcpy(&actual, decoded->image.data.data() + i * sizeof(actual),
                  sizeof(actual));
      TEST_CHECK(actual == expected[i]);
    }
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; packed TIFF test skipped");
#endif
}

void tinydng_packed_planar_integer_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  const auto bytes = MakePackedPlanarRGBTiff();
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "packed-planar-rgb.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  TEST_CHECK(decoded->image.width == 2 && decoded->image.height == 1);
  TEST_CHECK(decoded->image.channels == 3);
  TEST_CHECK(decoded->image.bpp == 16);
  const uint16_t expected[] = {0, 100, 300, 4095, 200, 400};
  TEST_CHECK(decoded->image.data.size() == sizeof(expected));
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    uint16_t actual = 0;
    if (decoded->image.data.size() >= (i + 1) * sizeof(actual)) {
      std::memcpy(&actual, decoded->image.data.data() + i * sizeof(actual),
                  sizeof(actual));
    }
    TEST_CHECK(actual == expected[i]);
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; packed planar TIFF test skipped");
#endif
}

void tiff_planar_scanline_test(void) {
#if defined(LIGHTUSD_WITH_TIFF) && defined(LIGHTUSD_WITH_LIBTIFF)
  const auto bytes = MakePlanarRGBTiff();
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "planar.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  TEST_CHECK(decoded->image.width == 2 && decoded->image.height == 2);
  TEST_CHECK(decoded->image.channels == 3);
  TEST_CHECK(decoded->image.bpp == 8);
  TEST_CHECK(decoded->image.data.size() == 12);
  if (decoded->image.data.size() == 12) {
    TEST_CHECK(decoded->image.data[0] == 255 && decoded->image.data[1] == 0 &&
               decoded->image.data[2] == 0);
    TEST_CHECK(decoded->image.data[3] == 0 && decoded->image.data[4] == 255 &&
               decoded->image.data[5] == 0);
  }
#else
  TEST_MSG("TIFF/libtiff support is disabled; planar scanline test skipped");
#endif
}

void tiff_tiled_test(void) {
#if defined(LIGHTUSD_WITH_TIFF) && defined(LIGHTUSD_WITH_LIBTIFF)
  const auto bytes = MakeTiledGrayTiff();
  auto info = lightusd::image::GetImageInfoFromMemory(
      bytes.data(), bytes.size(), "tiled.tif");
  TEST_CHECK(info.has_value());
  if (!info) return;
  TEST_CHECK(info->width == 4 && info->height == 4 && info->channels == 1);
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "tiled.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  TEST_CHECK(decoded->image.width == 4 && decoded->image.height == 4);
  TEST_CHECK(decoded->image.channels == 1 && decoded->image.bpp == 8);
  const uint8_t expected[] = {
      1, 2, 5, 6, 3, 4, 7, 8,
      9, 10, 13, 14, 11, 12, 15, 16};
  TEST_CHECK(decoded->image.data.size() == sizeof(expected));
  if (decoded->image.data.size() == sizeof(expected)) {
    TEST_CHECK(std::memcmp(decoded->image.data.data(), expected,
                           sizeof(expected)) == 0);
  }
#else
  TEST_MSG("TIFF/libtiff support is disabled; tiled TIFF test skipped");
#endif
}

void tinydng_tiled_packbits_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  const auto bytes = MakeTiledGrayTiff(true);
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "tiled-packbits.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  const uint8_t expected[] = {
      1, 2, 5, 6, 3, 4, 7, 8,
      9, 10, 13, 14, 11, 12, 15, 16};
  TEST_CHECK(decoded->image.data.size() == sizeof(expected));
  if (decoded->image.data.size() == sizeof(expected)) {
    TEST_CHECK(std::memcmp(decoded->image.data.data(), expected,
                           sizeof(expected)) == 0);
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; tiled PackBits test skipped");
#endif
}

void tinydng_strip_packbits_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  const auto bytes = MakeStripPackBitsGrayTiff();
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "strip-packbits.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  const uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_CHECK(decoded->image.width == 4 && decoded->image.height == 2);
  TEST_CHECK(decoded->image.data.size() == sizeof(expected));
  if (decoded->image.data.size() == sizeof(expected)) {
    TEST_CHECK(std::memcmp(decoded->image.data.data(), expected,
                           sizeof(expected)) == 0);
  }
  auto truncated = bytes;
  // StripByteCounts is the ninth entry (zero-based) in this fixture. Reduce
  // its inline value so the literal run is truncated before decoding.
  const size_t strip_count_value = 8u + 2u + 8u * 12u + 8u;
  truncated[strip_count_value + 0] = 2;
  truncated[strip_count_value + 1] = 0;
  truncated[strip_count_value + 2] = 0;
  truncated[strip_count_value + 3] = 0;
  auto rejected = lightusd::image::LoadImageFromMemory(
      truncated.data(), truncated.size(), "truncated-packbits.tif");
  TEST_CHECK(!rejected.has_value());
#else
  TEST_MSG("TIFF support is disabled; strip PackBits test skipped");
#endif
}

void tinydng_planar_strip_packbits_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  const auto bytes = MakePlanarStripPackBitsRGBTiff();
  auto decoded = lightusd::image::LoadImageFromMemory(
      bytes.data(), bytes.size(), "planar-strip-packbits.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  const uint8_t expected[] = {255, 0, 0, 0, 255, 0,
                              255, 0, 255, 0, 255, 255};
  TEST_CHECK(decoded->image.width == 2 && decoded->image.height == 2);
  TEST_CHECK(decoded->image.channels == 3);
  TEST_CHECK(decoded->image.data.size() == sizeof(expected));
  if (decoded->image.data.size() == sizeof(expected)) {
    TEST_CHECK(std::memcmp(decoded->image.data.data(), expected,
                           sizeof(expected)) == 0);
  }
#else
  TEST_MSG("TIFF support is disabled; planar PackBits test skipped");
#endif
}

void tinydng_multi_layer_writer_test(void) {
#if defined(LIGHTUSD_WITH_TIFF)
  std::vector<lightusd::Image> layers(2);
  layers[0].width = layers[1].width = 1;
  layers[0].height = layers[1].height = 1;
  layers[0].channels = layers[1].channels = 1;
  layers[0].bpp = layers[1].bpp = 8;
  layers[0].format = layers[1].format = lightusd::Image::PixelFormat::UInt;
  layers[0].data = {0x11};
  layers[1].data = {0x22};
  lightusd::image::WriteOption option;
  option.format = lightusd::image::WriteImageFormat::TIFF;
  auto encoded = lightusd::image::WriteImageLayersToMemory(layers, option);
  TEST_CHECK(encoded.has_value());
  if (!encoded) return;
  auto decoded = lightusd::image::LoadImageLayersFromMemory(
      encoded->data(), encoded->size(), "layers.tif");
  TEST_CHECK(decoded.has_value());
  if (!decoded) return;
  TEST_CHECK(decoded->size() == 2);
  if (decoded->size() == 2) {
    TEST_CHECK(decoded->at(0).image.data.size() == 1);
    TEST_CHECK(decoded->at(1).image.data.size() == 1);
    TEST_CHECK(decoded->at(0).image.data[0] == 0x11);
    TEST_CHECK(decoded->at(1).image.data[0] == 0x22);
  }
#else
  TEST_MSG("LIGHTUSD_WITH_TIFF is disabled; multi-layer writer test skipped");
#endif
}

namespace {
bool TestImageLoader(lightusd::image::ImageResult *result,
                     const uint8_t *, size_t, const std::string &name, void *,
                     std::string *warn, std::string *) {
  if (name != "callback.test") return false;
  result->image.width = 1;
  result->image.height = 1;
  result->image.channels = 4;
  result->image.bpp = 8;
  result->image.format = lightusd::Image::PixelFormat::UInt;
  result->image.data = {1, 2, 3, 255};
  if (warn) *warn = "callback decoder used";
  return true;
}

bool TestImageInfoLoader(lightusd::image::ImageInfoResult *result,
                         const uint8_t *, size_t, const std::string &name,
                         void *) {
  if (name != "callback.test") return false;
  result->width = 1;
  result->height = 1;
  result->channels = 4;
  return true;
}
}  // namespace

void image_loader_callback_test(void) {
  lightusd::image::SetImageLoader(TestImageLoader);
  lightusd::image::SetImageInfoLoader(TestImageInfoLoader);
  const uint8_t bytes[] = {0, 1, 2};
  auto image = lightusd::image::LoadImageFromMemory(bytes, sizeof(bytes),
                                                     "callback.test");
  TEST_CHECK(image.has_value());
  if (image) {
    TEST_CHECK(image->image.width == 1);
    TEST_CHECK(image->image.data.size() == 4);
    TEST_CHECK(image->warning == "callback decoder used");
  }
  auto info = lightusd::image::GetImageInfoFromMemory(bytes, sizeof(bytes),
                                                       "callback.test");
  TEST_CHECK(info.has_value());
  if (info) TEST_CHECK(info->width == 1 && info->height == 1 && info->channels == 4);
  lightusd::image::SetImageLoader(nullptr);
  lightusd::image::SetImageInfoLoader(nullptr);
}
