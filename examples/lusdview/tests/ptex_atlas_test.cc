// SPDX-License-Identifier: Apache-2.0
#include "cpu/ptex-sample.hh"
#include "displacement_bake.hh"
#include "ptex_atlas.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "external/miniz.h"

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

void TestPhysicalPageCache() {
  lusdview::PtexPhysicalPageCache cache(2);
  lusdview::PtexPhysicalPageAssignment a;
  CHECK(cache.Request(10, &a) && !a.hit && a.slot == 0 &&
        a.evictedFace == ~uint32_t{0});
  CHECK(cache.Request(20, &a) && !a.hit && a.slot == 1);
  CHECK(cache.Request(10, &a) && a.hit && a.slot == 0);
  CHECK(cache.Request(30, &a) && !a.hit && a.slot == 1 &&
        a.evictedFace == 20);
  CHECK(cache.residentCount() == 2);
  CHECK(cache.hits() == 1 && cache.misses() == 3 && cache.evictions() == 1);
  cache.Clear();
  CHECK(cache.residentCount() == 0 && cache.hits() == 0 &&
        cache.misses() == 0 && cache.evictions() == 0);

  lusdview::PtexPhysicalPageCache empty(0);
  CHECK(!empty.Request(1, &a));
  CHECK(!cache.Request(1, nullptr));
}

void Put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v));
  out->push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>* out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Put64(std::vector<uint8_t>* out, uint64_t v) {
  Put32(out, static_cast<uint32_t>(v));
  Put32(out, static_cast<uint32_t>(v >> 32));
}
std::vector<uint8_t> Deflate(const std::vector<uint8_t>& source) {
  mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(source.size()));
  std::vector<uint8_t> result(static_cast<size_t>(capacity));
  CHECK(mz_compress(result.data(), &capacity, source.data(),
                    static_cast<mz_ulong>(source.size())) == MZ_OK);
  result.resize(static_cast<size_t>(capacity));
  return result;
}

void PutSample(std::vector<uint8_t>* out, uint32_t type, float value) {
  if (type == 0) {
    out->push_back(static_cast<uint8_t>(std::lround(value * 255.0f)));
  } else if (type == 1) {
    Put16(out, static_cast<uint16_t>(std::lround(value * 65535.0f)));
  } else if (type == 2) {
    Put16(out, value < 0.5f ? uint16_t{0x3400} : uint16_t{0x3a00});
  } else {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Put32(out, bits);
  }
}

std::vector<uint8_t> SyntheticPtex(uint32_t dataType, uint16_t channels = 1,
                                   bool verticalGradient = false,
                                   uint32_t encoding = 1,
                                   bool constantFace0 = false,
                                   bool constantTiles = false,
                                   bool malformedTileTable = false,
                                   uint32_t faceCount = 2) {
  std::vector<uint8_t> faceInfo;
  for (uint32_t face = 0; face < faceCount; ++face) {
    faceInfo.push_back(face == 0 ? 2 : 1);  // 4x2, then 2x4
    faceInfo.push_back(face == 0 ? 1 : 2);
    faceInfo.push_back(0);
    faceInfo.push_back(face == 0 && constantFace0 ? 1 : 0);
    for (int edge = 0; edge < 4; ++edge) Put32(&faceInfo, 0xffffffffu);
  }
  const std::vector<uint8_t> faceInfoZ = Deflate(faceInfo);
  std::vector<uint8_t> constants;
  for (uint32_t face = 0; face < faceCount; ++face)
    for (uint16_t channel = 0; channel < channels; ++channel)
      PutSample(&constants, dataType, face == 0 ? 0.25f : 0.75f);
  const std::vector<uint8_t> constantsZ = Deflate(constants);

  std::vector<std::vector<uint8_t>> blocks;
  std::vector<uint32_t> headerSizes;
  uint64_t levelDataSize = 0;
  for (uint32_t level = 0; level < 2; ++level) {
    std::vector<uint8_t> headers, payload;
    const uint32_t firstFace = level > 0 && constantFace0 ? 1u : 0u;
    const uint32_t levelFaces = faceCount - firstFace;
    for (uint32_t face = firstFace; face < faceCount; ++face) {
      if (constantFace0 && face == 0) {
        Put32(&headers, 0);
        continue;
      }
      const uint32_t width = std::max(1u, (face == 0 ? 4u : 2u) >> level);
      const uint32_t height = std::max(1u, (face == 0 ? 2u : 4u) >> level);
      std::vector<uint8_t> planar;
      for (uint16_t channel = 0; channel < channels; ++channel) {
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
          const uint32_t row = pixel / width;
          const float faceValue = verticalGradient
                                      ? (row == 0 ? 0.125f : 0.875f)
                                      : (face == 0 ? 0.25f : 0.75f);
          const float values[4] = {faceValue, 0.5f,
                                   0.625f, 0.875f};
          PutSample(&planar, dataType, values[channel]);
        }
      }
      if (encoding == 2 && dataType == 0) {
        const size_t pixels = size_t(width) * height;
        for (uint16_t channel = 0; channel < channels; ++channel) {
          uint8_t* plane = planar.data() + size_t(channel) * pixels;
          for (size_t pixel = pixels; pixel-- > 1;)
            plane[pixel] = uint8_t(plane[pixel] - plane[pixel - 1]);
        }
      }
      const std::vector<uint8_t> compressed = Deflate(planar);
      if (encoding == 3) {
        std::vector<uint8_t> tileHeader;
        std::vector<uint8_t> tilePayload;
        if (constantTiles) {
          for (uint16_t channel = 0; channel < channels; ++channel)
            PutSample(&tilePayload, dataType,
                      channel == 0 ? (face == 0 ? 0.25f : 0.75f) : 0.5f);
          Put32(&tileHeader, static_cast<uint32_t>(tilePayload.size()));
        } else {
          tilePayload = compressed;
          Put32(&tileHeader,
                (1u << 30) | static_cast<uint32_t>(tilePayload.size()));
        }
        const std::vector<uint8_t> tileHeaderZ = Deflate(tileHeader);
        std::vector<uint8_t> tiled;
        uint8_t logW = 0, logH = 0;
        while ((1u << logW) < width) ++logW;
        while ((1u << logH) < height) ++logH;
        tiled.push_back(logW);
        tiled.push_back(logH);
        Put32(&tiled, static_cast<uint32_t>(tileHeaderZ.size()) +
                          (malformedTileTable ? 1024u : 0u));
        tiled.insert(tiled.end(), tileHeaderZ.begin(), tileHeaderZ.end());
        tiled.insert(tiled.end(), tilePayload.begin(), tilePayload.end());
        Put32(&headers,
              (3u << 30) | static_cast<uint32_t>(tiled.size()));
        payload.insert(payload.end(), tiled.begin(), tiled.end());
      } else {
        Put32(&headers, (encoding << 30) |
                            static_cast<uint32_t>(compressed.size()));
        payload.insert(payload.end(), compressed.begin(), compressed.end());
      }
    }
    const std::vector<uint8_t> headersZ = Deflate(headers);
    headerSizes.push_back(static_cast<uint32_t>(headersZ.size()));
    blocks.push_back(headersZ);
    blocks.back().insert(blocks.back().end(), payload.begin(), payload.end());
    levelDataSize += blocks.back().size();
    (void)levelFaces;
  }
  std::vector<uint8_t> levelInfo;
  for (size_t level = 0; level < blocks.size(); ++level) {
    Put64(&levelInfo, blocks[level].size());
    Put32(&levelInfo, headerSizes[level]);
    Put32(&levelInfo,
          static_cast<uint32_t>(level == 0
                                    ? faceCount
                                    : (constantFace0 ? faceCount - 1u
                                                     : faceCount)));
  }

  std::vector<uint8_t> out;
  out.push_back('P'); out.push_back('t'); out.push_back('e'); out.push_back('x');
  Put32(&out, 1); Put32(&out, 1); Put32(&out, dataType);
  Put32(&out, 0xffffffffu); Put16(&out, channels); Put16(&out, 2);
  Put32(&out, faceCount);
  Put32(&out, 0); Put32(&out, faceInfoZ.size()); Put32(&out, constantsZ.size());
  Put32(&out, levelInfo.size()); Put32(&out, 0); Put64(&out, levelDataSize);
  Put32(&out, 0); Put32(&out, 0);
  out.insert(out.end(), faceInfoZ.begin(), faceInfoZ.end());
  out.insert(out.end(), constantsZ.begin(), constantsZ.end());
  out.insert(out.end(), levelInfo.begin(), levelInfo.end());
  for (const auto& block : blocks) out.insert(out.end(), block.begin(), block.end());
  return out;
}

void RunType(uint32_t type, uint16_t channels = 1) {
  const std::vector<uint8_t> bytes = SyntheticPtex(type, channels);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxFaceEdge = 4;
  options.maxAtlasEdge = 64;
  options.gutter = 1;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  lusdview::PtexAtlasBuildStats stats;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 &stats, &error));
  CHECK(stats.pageCache.misses == 2);
  CHECK(stats.pageCache.peakResidentBytes <= options.maxDecodedCacheBytes);
  CHECK(rects.size() == 2);
  if (rects.size() != 2) return;
  CHECK(rects[0].width == 4 && rects[0].height == 2);
  CHECK(rects[1].width == 2 && rects[1].height == 4);
  CHECK(stats.rectTexelOffset + rects.size() * 8u <=
        static_cast<size_t>(atlas.width) * atlas.height);
  for (uint32_t face = 0; face < rects.size(); ++face) {
    const size_t base = stats.rectTexelOffset + face * 8u;
    auto value = [&](size_t component) {
      const uint32_t lo = atlas.data[(base + component * 2u) * 4u + 3u];
      const uint32_t hi = atlas.data[(base + component * 2u + 1u) * 4u + 3u];
      return lo | (hi << 8u);
    };
    CHECK(value(0) == rects[face].x && value(1) == rects[face].y);
    CHECK(value(2) == rects[face].width && value(3) == rects[face].height);
    const auto pixel = [&](uint32_t x, uint32_t y, uint32_t channel) {
      return atlas.data[(size_t(y) * atlas.width + x) * 4u + channel];
    };
    // One-texel clamp gutters reproduce edge texels, including corners.
    CHECK(pixel(rects[face].x - 1u, rects[face].y, 0) ==
          pixel(rects[face].x, rects[face].y, 0));
    CHECK(pixel(rects[face].x, rects[face].y - 1u, 0) ==
          pixel(rects[face].x, rects[face].y, 0));
    CHECK(pixel(rects[face].x - 1u, rects[face].y - 1u, 0) ==
          pixel(rects[face].x, rects[face].y, 0));
  }
  lusdview::DrawScene scene;
  lusdview::DrawTextureCPU texture;
  texture.image = atlas;
  texture.isPtex = true;
  texture.ptexFaceRects = rects;
  scene.textures.push_back(std::move(texture));
  CHECK(std::fabs(lusdview::SampleTextureRed(scene, 0, 0.37f, 0.61f, 0) -
                  0.25f) < 0.02f);
  CHECK(std::fabs(lusdview::SampleTextureRed(scene, 0, 0.37f, 0.61f, 1) -
                  0.75f) < 0.02f);

  // Demand-driven startup atlases reserve later faces for physical-page
  // streaming instead of decoding every face eagerly.
  if (rects.size() >= 2) {
    lusdview::PtexAtlasOptions lazy = options;
    lazy.initialFaceLimit = 1;
    lazy.maxPhysicalCacheBytes = 64u * 64u * 4u;
    light3d::Image lazyAtlas;
    std::vector<lusdview::DrawPtexFaceRectCPU> lazyRects;
    lusdview::PtexAtlasBuildStats lazyStats;
    CHECK(lusdview::BuildPtexAtlas(reader, lazy, false, &lazyAtlas,
                                   &lazyRects, &lazyStats, &error));
    CHECK(lazyRects.size() == rects.size());
    CHECK(lazyRects[0].reserved == 0);
    CHECK(lazyRects[1].reserved == 1);
    CHECK(lazyStats.decodedFaces < stats.decodedFaces);
  }

  const auto& r0 = rects[0];
  const size_t rgba = (size_t(r0.y) * atlas.width + r0.x) * 4u;
  CHECK(std::abs(int(atlas.data[rgba + 1]) - (channels > 2 ? 128 : 64)) <= 1);
  CHECK(std::abs(int(atlas.data[rgba + 2]) - (channels > 2 ? 159 : 64)) <= 1);
  const int expectedAlpha = channels == 2 ? 128 : (channels == 4 ? 223 : 255);
  CHECK(std::abs(int(atlas.data[rgba + 3]) - expectedAlpha) <= 1);

  if (type == 0 && channels == 1) {
    lusdview::PtexFacePageCache cache(8);
    const auto* face0 = cache.Fetch(reader, 0, 0, 64, &error);
    CHECK(face0 && face0->data.size() == 8);
    CHECK(cache.stats().misses == 1 && cache.stats().hits == 0);
    CHECK(cache.stats().residentBytes == 8);
    const auto* face1 = cache.Fetch(reader, 1, 0, 64, &error);
    CHECK(face1 && face1->data.size() == 8);
    CHECK(cache.stats().evictions == 1 && cache.size() == 1);
    CHECK(cache.Fetch(reader, 1, 0, 64, &error) != nullptr);
    CHECK(cache.stats().hits == 1);
    CHECK(cache.Fetch(reader, 0, 0, 64, &error) != nullptr);
    CHECK(cache.stats().misses == 3 && cache.stats().evictions == 2);
    CHECK(cache.stats().residentBytes <= 8);
    cache.Clear();
    CHECK(cache.size() == 0 && cache.stats().residentBytes == 0);

    lusdview::PtexFacePageCache transientCache(4);
    CHECK(transientCache.Fetch(reader, 0, 0, 64, &error) != nullptr);
    CHECK(transientCache.size() == 0 &&
          transientCache.stats().residentBytes == 0);
    CHECK(transientCache.Fetch(reader, 0, 0, 64, &error) != nullptr);
    CHECK(transientCache.stats().misses == 2 &&
          transientCache.stats().hits == 0);
    CHECK(transientCache.Fetch(reader, 0, 0, 4, &error) == nullptr);

    lusdview::PtexAtlasOptions tight = options;
    tight.maxAtlasBytes = 220;
    light3d::Image tightAtlas;
    std::vector<lusdview::DrawPtexFaceRectCPU> tightRects;
    lusdview::PtexAtlasBuildStats tightStats;
    CHECK(lusdview::BuildPtexAtlas(reader, tight, false, &tightAtlas,
                                   &tightRects, &tightStats, &error));
    CHECK(tightAtlas.data.size() <= tight.maxAtlasBytes);
    CHECK(tightStats.downsampledFaces > 0);
  }
  if (type == 0 && channels == 4) {
    light3d::Image srgbAtlas;
    std::vector<lusdview::DrawPtexFaceRectCPU> srgbRects;
    CHECK(lusdview::BuildPtexAtlas(reader, options, true, &srgbAtlas,
                                   &srgbRects, nullptr, &error));
    // Atlas construction preserves source samples. Color-space conversion is a
    // GPU format choice; alpha and the embedded rectangle table stay linear.
    CHECK(srgbAtlas.data == atlas.data);
  }
}

void RunOrientation() {
  const std::vector<uint8_t> bytes = SyntheticPtex(0, 1, true);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxAtlasEdge = 64;
  options.gutter = 1;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 nullptr, &error));
  CHECK(rects.size() == 2);
  if (rects.empty()) return;
  const auto red = [&](uint32_t x, uint32_t y) {
    return atlas.data[(size_t(y) * atlas.width + x) * 4u];
  };
  // Ptex row zero is the bottom row; atlas images are top-row-first.
  CHECK(std::abs(int(red(rects[0].x, rects[0].y)) - 223) <= 1);
  CHECK(std::abs(int(red(rects[0].x, rects[0].y + 1u)) - 32) <= 1);
}

void RunEncoding(uint32_t encoding) {
  const std::vector<uint8_t> bytes =
      SyntheticPtex(0, 1, true, encoding, false);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxAtlasEdge = 64;
  options.gutter = 1;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 nullptr, &error));
  CHECK(rects.size() == 2);
  if (rects.empty()) return;
  const auto red = [&](uint32_t x, uint32_t y) {
    return atlas.data[(size_t(y) * atlas.width + x) * 4u];
  };
  CHECK(std::abs(int(red(rects[0].x, rects[0].y)) - 223) <= 1);
  CHECK(std::abs(int(red(rects[0].x, rects[0].y + 1u)) - 32) <= 1);
}

void RunConstantFace() {
  const std::vector<uint8_t> bytes = SyntheticPtex(0, 1, false, 1, true);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxAtlasEdge = 64;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 nullptr, &error));
  lusdview::DrawScene scene;
  lusdview::DrawTextureCPU texture;
  texture.image = atlas;
  texture.isPtex = true;
  texture.ptexFaceRects = rects;
  scene.textures.push_back(std::move(texture));
  CHECK(std::fabs(lusdview::SampleTextureRed(scene, 0, 0.5f, 0.5f, 0) -
                  0.25f) < 0.02f);
  CHECK(std::fabs(lusdview::SampleTextureRed(scene, 0, 0.5f, 0.5f, 1) -
                  0.75f) < 0.02f);
}

void RunConstantTiles() {
  const std::vector<uint8_t> bytes =
      SyntheticPtex(0, 3, false, 3, false, true, false);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxAtlasEdge = 64;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 nullptr, &error));
  CHECK(rects.size() == 2);
  if (rects.empty()) return;
  const size_t p = (size_t(rects[0].y) * atlas.width + rects[0].x) * 4u;
  CHECK(std::abs(int(atlas.data[p]) - 64) <= 1);
  CHECK(std::abs(int(atlas.data[p + 1]) - 128) <= 1);
}

void RunMalformedTileTable() {
  const std::vector<uint8_t> bytes =
      SyntheticPtex(0, 1, false, 3, false, false, true);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxAtlasEdge = 64;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  CHECK(!lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                  nullptr, &error));
  CHECK(error.find("tile") != std::string::npos);
}

void RunPhysicalCacheReservation() {
  const std::vector<uint8_t> bytes = SyntheticPtex(0, 1, false, 1);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxFaceEdge = 8;
  options.maxAtlasEdge = 64;
  options.maxAtlasBytes = 64u * 64u * 4u;
  const uint32_t slotEdge = options.maxFaceEdge + options.gutter * 2u;
  options.maxPhysicalCacheBytes = size_t(slotEdge) * slotEdge * 4u * 2u;
  options.forcePhysicalCache = true;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  lusdview::PtexAtlasBuildStats stats;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 &stats, &error));
  CHECK(stats.physicalCacheSlotEdge == slotEdge);
  CHECK(stats.physicalCacheSlots == 2);
  CHECK(stats.physicalCacheOffsetY > 0);
  CHECK((stats.physicalCacheOffsetY & 3u) == 0u);
  CHECK(stats.rectTexelOffset >=
        uint32_t(atlas.width) *
            (stats.physicalCacheOffsetY + stats.physicalCacheSlotEdge));
  CHECK(atlas.data.size() <= options.maxAtlasBytes);
  light3d::Image page;
  lusdview::DrawPtexFaceRectCPU inner;
  CHECK(lusdview::BuildPtexPage(reader, 0, 0, options.gutter,
                                1024u * 1024u, &page, &inner, &error));
  CHECK(page.width == int(inner.width + options.gutter * 2u));
  CHECK(page.height == int(inner.height + options.gutter * 2u));
  lusdview::DrawPtexFaceRectCPU encoded;
  encoded.x = 0x1234u;
  encoded.y = 0x5678u;
  encoded.width = 0x9abcu;
  encoded.height = 0xdef0u;
  uint8_t texels[8u * 4u];
  lusdview::EncodePtexFaceRectTexels(encoded, texels);
  const uint8_t expected[8] = {0x34u, 0x12u, 0x78u, 0x56u,
                               0xbcu, 0x9au, 0xf0u, 0xdeu};
  for (size_t i = 0; i < 8; ++i) {
    CHECK(texels[i * 4u + 0u] == 0u);
    CHECK(texels[i * 4u + 1u] == 0u);
    CHECK(texels[i * 4u + 2u] == 0u);
    CHECK(texels[i * 4u + 3u] == expected[i]);
  }
}

void RunPhysicalCacheCannotStarveFallback() {
  const std::vector<uint8_t> bytes = SyntheticPtex(0, 1, false, 1);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions baseline;
  baseline.maxFaceEdge = 8;
  baseline.maxAtlasEdge = 64;
  light3d::Image plain;
  std::vector<lusdview::DrawPtexFaceRectCPU> plainRects;
  CHECK(lusdview::BuildPtexAtlas(reader, baseline, false, &plain, &plainRects,
                                 nullptr, &error));

  lusdview::PtexAtlasOptions constrained = baseline;
  constrained.maxAtlasBytes = plain.data.size();
  constrained.maxPhysicalCacheBytes = 1024u * 1024u;
  constrained.forcePhysicalCache = true;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  lusdview::PtexAtlasBuildStats stats;
  CHECK(lusdview::BuildPtexAtlas(reader, constrained, false, &atlas, &rects,
                                 &stats, &error));
  CHECK(stats.physicalCacheSlots == 0);
  CHECK(rects.size() == plainRects.size());
  CHECK(atlas.data.size() <= constrained.maxAtlasBytes);
}

void RunVirtualFallbackDownsample() {
  const std::vector<uint8_t> bytes = SyntheticPtex(0, 1, false, 1);
  lightusd::ptx::Reader reader;
  std::string error;
  CHECK(lightusd::ptx::Reader::OpenMemory(bytes.data(), bytes.size(), &reader,
                                          &error));
  lusdview::PtexAtlasOptions options;
  options.maxFaceEdge = 4;
  options.maxAtlasEdge = 64;
  options.gutter = 0;
  options.maxAtlasBytes = 80;
  light3d::Image atlas;
  std::vector<lusdview::DrawPtexFaceRectCPU> rects;
  lusdview::PtexAtlasBuildStats stats;
  CHECK(lusdview::BuildPtexAtlas(reader, options, false, &atlas, &rects,
                                 &stats, &error));
  CHECK(rects.size() == 2);
  CHECK(rects[0].reserved == 1 || rects[1].reserved == 1);
  CHECK(atlas.data.size() <= options.maxAtlasBytes);
}
// The CPU preview samples per-face mips from the source with a streamed page
// cache. The reference texels were produced by OpenPtex 2.4.3 (see
// tests/usdc/ptex-mip/manifest.json); this check is byte-exact.
void RunCpuPtexMipSampler() {
  const char* env = std::getenv("LIGHTUSD_PTEX_TEST_ASSET");
  const std::string asset =
      env ? env
          : "/mnt/nvme02/data/island/textures/isMountainA/Color/mountain_geo.ptx";
  if (!std::ifstream(asset).good()) {
    std::printf("SKIP: pinned Island PTex asset not found (%s)\n",
                asset.c_str());
    return;
  }
#ifndef LIGHTUSD_PTEX_MIP_FIXTURE_DIR
#define LIGHTUSD_PTEX_MIP_FIXTURE_DIR "tests/usdc/ptex-mip"
#endif
  const std::string fixture = LIGHTUSD_PTEX_MIP_FIXTURE_DIR;
  auto read_fixture = [&](const char* name, std::vector<uint8_t>* out) {
    std::ifstream in(fixture + "/" + name, std::ios::binary);
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return in.good() || in.eof();
  };

  lusdview::CpuPtexSampler sampler(4ull * 1024ull * 1024ull);
  std::string error;
  if (!sampler.OpenFile(asset, &error)) {
    std::printf("FAIL: CPU PTex sampler open: %s\n", error.c_str());
    ++failures;
    return;
  }
  const auto& info = sampler.info();
  CHECK(info.levels >= 3 && info.faces > 0);

  struct Case { uint32_t mip; uint32_t w; uint32_t h; const char* file; };
  const Case cases[] = {{1, 16, 8, "mountain-face0-mip1.rgba"},
                        {2, 8, 4, "mountain-face0-mip2.rgba"}};
  for (const Case& c : cases) {
    std::vector<uint8_t> reference;
    CHECK(read_fixture(c.file, &reference));
    const uint8_t* raw = nullptr;
    const auto* page = sampler.RawFace(0, c.mip, &raw, &error);
    CHECK(page != nullptr);
    if (!page || !raw) continue;
    CHECK(page->width == c.w && page->height == c.h);
    CHECK(page->channels == 3);
    CHECK(reference.size() == size_t(c.w) * c.h * 3);
    CHECK(reference.size() == page->data.size());
    CHECK(std::memcmp(reference.data(), raw, reference.size()) == 0);

    // Sampling at the texel centers returns exactly the non-base mip texel.
    for (uint32_t y = 0; y < c.h && y < 2; ++y) {
      for (uint32_t x = 0; x < c.w && x < 2; ++x) {
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        CHECK(sampler.Sample(0, c.mip, (x + 0.5f) / float(c.w),
                             (y + 0.5f) / float(c.h), false, rgb));
        const size_t base = (size_t(y) * c.w + x) * 3;
        CHECK(std::lround(rgb[0] * 255.0f) == reference[base + 0]);
        CHECK(std::lround(rgb[1] * 255.0f) == reference[base + 1]);
        CHECK(std::lround(rgb[2] * 255.0f) == reference[base + 2]);
      }
    }
  }

  // Mip selection is per face resolution. Face 0 is 32 texels wide and the
  // file has four levels, so an 8-texel footprint clamps to mip 3.
  CHECK(sampler.ChooseMip(0, 0.0f) == 0);
  CHECK(sampler.ChooseMip(0, 1.0f / 32.0f) == 0);
  CHECK(sampler.ChooseMip(0, 2.0f / 32.0f) == 1);
  CHECK(sampler.ChooseMip(0, 4.0f / 32.0f) == 2);
  CHECK(sampler.ChooseMip(0, 8.0f / 32.0f) == 3);

  // Streamed residency: a 4 KiB budget holds two 32x16x3 pages (1536 B each),
  // so four fetches evict and the resident/peak footprint stays bounded.
  lusdview::CpuPtexSampler small(4ull * 1024ull);
  CHECK(small.OpenFile(asset, &error));
  for (uint32_t face = 0; face < 4 && face < info.faces; ++face) {
    const uint8_t* raw = nullptr;
    CHECK(small.RawFace(face, 0, &raw, &error) != nullptr);
  }
  const auto& st = small.stats();
  CHECK(st.evictions > 0);
  CHECK(st.residentBytes <= 4ull * 1024ull);
  CHECK(st.peakResidentBytes <= 4ull * 1024ull);
  std::printf("  CPU Ptex mip sampler: PASSED (face 0 mips 1-2 vs OpenPtex, "
              "cache evictions=%llu, resident=%llu)\n",
              static_cast<unsigned long long>(st.evictions),
              static_cast<unsigned long long>(st.residentBytes));
}

void RunCpuPtexAnisotropicSampler() {
  // Two-level synthetic Ptex (face 0 = 4x2) with a row-varying value so a
  // wrong mip or tap placement changes the result.
  const std::vector<uint8_t> bytes =
      SyntheticPtex(0, 1, /*verticalGradient=*/true, 1);
  lusdview::CpuPtexSampler sampler(1ull * 1024ull * 1024ull);
  std::string error;
  CHECK(sampler.OpenMemory(bytes.data(), bytes.size(), &error));
  CHECK(sampler.info().levels == 2 && sampler.info().faces == 2);

  // Isotropic footprint: one texel in both directions selects mip 0 and a
  // single bilinear tap, identical to the plain Sample path.
  const float iso_gu[3] = {1.0f, 0.0f, 0.0f};
  const float iso_gv[3] = {0.0f, 1.0f, 0.0f};
  const lusdview::CpuPtexSampler::Footprint iso =
      sampler.ComputeFootprint(0, iso_gu, iso_gv, 0.25f);
  CHECK(iso.taps == 1);
  CHECK(!iso.anisotropic());
  CHECK(iso.mip == sampler.ChooseMip(0, 0.25f));
  float plain[3], filtered[3];
  CHECK(sampler.Sample(0, iso.mip, 0.375f, 0.25f, false, plain));
  CHECK(sampler.SampleFootprint(0, iso, 0.375f, 0.25f, false, filtered));
  for (int c = 0; c < 3; ++c) CHECK(std::fabs(plain[c] - filtered[c]) < 1e-6f);

  // Anisotropic footprint: 4 texels along u, 1 along v. The mip must come from
  // the minor (v) axis and the taps run along u; the old isotropic rule used
  // max(|gu|,|gv|) and picked the coarser mip.
  const float aniso_gu[3] = {4.0f, 0.0f, 0.0f};
  const float aniso_gv[3] = {0.0f, 1.0f, 0.0f};
  const lusdview::CpuPtexSampler::Footprint fp =
      sampler.ComputeFootprint(0, aniso_gu, aniso_gv, 0.25f);
  CHECK(fp.taps == 4);
  CHECK(fp.anisotropic());
  CHECK(std::fabs(fp.major - 1.0f) < 1e-6f);
  CHECK(std::fabs(fp.minor - 0.25f) < 1e-6f);
  CHECK(fp.mip == sampler.ChooseMip(0, fp.minor));
  CHECK(fp.mip != sampler.ChooseMip(0, fp.major));
  CHECK(std::fabs(fp.dir_u) > 0.99f && std::fabs(fp.dir_v) < 1e-6f);

  // The filter output must equal the tent-weighted tap average computed with
  // the plain sampler at the selected mip.
  const float u = 0.3f, v = 0.25f;
  float expected = 0.0f, weight_sum = 0.0f;
  const float center = 0.5f * float(fp.taps - 1u);
  for (uint32_t i = 0; i < fp.taps; ++i) {
    const float offset = (float(i) - center) * fp.minor;
    const float weight = float(fp.taps) * 0.5f - std::fabs(float(i) - center);
    float tap[3] = {0.0f, 0.0f, 0.0f};
    CHECK(sampler.Sample(0, fp.mip, u + offset * fp.dir_u,
                         v + offset * fp.dir_v, false, tap));
    expected += tap[0] * weight;
    weight_sum += weight;
  }
  expected /= weight_sum;
  CHECK(sampler.SampleFootprint(0, fp, u, v, false, filtered));
  CHECK(std::fabs(filtered[0] - expected) < 2e-6f);

  float channel = 0.0f;
  CHECK(sampler.SampleChannelFootprint(0, fp, u, v, 0, &channel));
  CHECK(std::fabs(channel - expected) < 2e-6f);

  // Clamp: a very stretched footprint never exceeds eight taps.
  const float stretch_gu[3] = {64.0f, 0.0f, 0.0f};
  const lusdview::CpuPtexSampler::Footprint clamped =
      sampler.ComputeFootprint(0, stretch_gu, aniso_gv, 0.25f);
  CHECK(clamped.taps == 8);
  CHECK(clamped.mip == sampler.ChooseMip(0, clamped.minor));

  std::printf("  CPU Ptex anisotropic footprint: PASSED "
              "(mip from minor, %u taps, tent-weighted)\n", fp.taps);
}

}  // namespace

int main(int argc, char** argv) {
  // The camera-demand integration test needs a deterministic native Ptex asset
  // but must not commit a binary fixture. Reuse this writer so CTest generates
  // the two-face source in its temporary directory.
  if (argc == 3 && std::string(argv[1]) == "--write-synthetic") {
    const std::vector<uint8_t> bytes =
        SyntheticPtex(0, 3, false, 1, false, false, false, 32);
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? 0 : 1;
  }
  TestPhysicalPageCache();
  for (uint32_t type = 0; type < 4; ++type) RunType(type);
  RunType(0, 2);
  RunType(0, 3);
  RunType(0, 4);
  RunOrientation();
  RunEncoding(2);
  RunEncoding(3);
  RunConstantFace();
  RunConstantTiles();
  RunMalformedTileTable();
  RunPhysicalCacheReservation();
  RunPhysicalCacheCannotStarveFallback();
  RunVirtualFallbackDownsample();
  RunCpuPtexMipSampler();
  RunCpuPtexAnisotropicSampler();
  if (failures) return 1;
  std::printf("OK: rectangular UInt8/UInt16/Half/Float Ptex atlases\n");
  return 0;
}
