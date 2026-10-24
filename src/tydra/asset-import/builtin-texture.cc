// SPDX-License-Identifier: Apache-2.0
// MJCF builtin textures (mjCTexture::Builtin2D / BuiltinCube) and a small PNG
// encoder.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>

#include "import.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {

uint8_t ToByte(double v) {
  // MuJoCo casts 255 * c; clamp so out-of-range colors stay defined.
  return static_cast<uint8_t>(std::min(255.0, std::max(0.0, 255.0 * v)));
}

const std::array<uint32_t, 256>& CRCTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  return table;
}

struct BitWriter {
  std::vector<uint8_t>* out;
  uint32_t bits = 0;
  int count = 0;
  void Put(uint32_t value, int n) {  // LSB first
    bits |= value << count;
    count += n;
    while (count >= 8) {
      out->push_back(static_cast<uint8_t>(bits));
      bits >>= 8;
      count -= 8;
    }
  }
  void PutHuffman(uint32_t code, int n) {  // Huffman codes go MSB first
    uint32_t reversed = 0;
    for (int i = 0; i < n; ++i) reversed |= ((code >> i) & 1u) << (n - 1 - i);
    Put(reversed, n);
  }
  void Flush() {
    if (count > 0) out->push_back(static_cast<uint8_t>(bits));
    bits = 0;
    count = 0;
  }
};

void PutLiteral(BitWriter& w, uint32_t v) {
  if (v < 144) w.PutHuffman(0x30 + v, 8);
  else if (v < 256) w.PutHuffman(0x190 + (v - 144), 9);
  else if (v < 280) w.PutHuffman(v - 256, 7);
  else w.PutHuffman(0xC0 + (v - 280), 8);
}

void PutMatch(BitWriter& w, uint32_t length, uint32_t distance) {
  static const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                           15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                         17,   25,   33,   49,   65,   97,    129,   193,
                                         257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  int l = 28;
  while (kLengthBase[l] > length) --l;
  PutLiteral(w, 257 + l);
  w.Put(length - kLengthBase[l], kLengthExtra[l]);
  int d = 29;
  while (kDistBase[d] > distance) --d;
  w.PutHuffman(static_cast<uint32_t>(d), 5);
  w.Put(distance - kDistBase[d], kDistExtra[d]);
}

// One fixed-Huffman deflate block. Greedy matches at one pixel and one row
// back cover the flat regions and repeated rows builtin textures consist of.
void Deflate(const std::vector<uint8_t>& raw, size_t pixel, size_t stride,
             std::vector<uint8_t>* out) {
  BitWriter w{out};
  w.Put(1, 1);  // BFINAL
  w.Put(1, 2);  // BTYPE = fixed Huffman
  const size_t n = raw.size();
  size_t i = 0;
  while (i < n) {
    size_t best = 0, best_d = 0;
    for (size_t d : {pixel, stride}) {
      if (d == 0 || d > 32768 || d > i) continue;
      const size_t limit = std::min<size_t>(258, n - i);
      size_t len = 0;
      while (len < limit && raw[i + len] == raw[i + len - d]) ++len;
      if (len > best) {
        best = len;
        best_d = d;
      }
    }
    if (best >= 3) {
      PutMatch(w, static_cast<uint32_t>(best), static_cast<uint32_t>(best_d));
      i += best;
    } else {
      PutLiteral(w, raw[i++]);
    }
  }
  PutLiteral(w, 256);
  w.Flush();
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out->push_back(static_cast<uint8_t>(v >> s));
}

void PutChunk(std::vector<uint8_t>* out, const char* type, const std::vector<uint8_t>& data) {
  PutU32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), data.begin(), data.end());
  uint32_t crc = 0xFFFFFFFFu;
  const auto& table = CRCTable();
  for (size_t i = start; i < out->size(); ++i)
    crc = table[(crc ^ (*out)[i]) & 0xFF] ^ (crc >> 8);
  PutU32(out, crc ^ 0xFFFFFFFFu);
}

}  // namespace

void EncodePNG(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t channels,
               std::vector<uint8_t>* out) {
  const size_t row = size_t(width) * channels;
  std::vector<uint8_t> raw;
  raw.reserve((row + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), pixels + y * row, pixels + (y + 1) * row);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  Deflate(raw, channels, row + 1, &z);
  // Adler-32, reducing once per 5552 bytes (the largest run without
  // uint32 overflow, as in zlib).
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size();) {
    const size_t end = std::min(raw.size(), i + 5552);
    for (; i < end; ++i) {
      a += raw[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  PutU32(&z, (b << 16) | a);
  std::vector<uint8_t> header;
  PutU32(&header, width);
  PutU32(&header, height);
  header.push_back(8);                                   // bit depth
  header.push_back(channels == 4 ? 6 : channels == 1 ? 0 : 2);  // color type
  header.push_back(0);
  header.push_back(0);
  header.push_back(0);
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  out->assign(kSignature, kSignature + 8);
  PutChunk(out, "IHDR", header);
  PutChunk(out, "IDAT", z);
  PutChunk(out, "IEND", {});
}

void GenerateBuiltin2D(const BuiltinTexture& t, std::vector<uint8_t>* rgb) {
  const int w = t.width, h = t.height;
  rgb->assign(size_t(w) * h * 3, 0);
  uint8_t c1[3], c2[3], cm[3];
  for (int j = 0; j < 3; ++j) {
    c1[j] = ToByte(t.rgb1[j]);
    c2[j] = ToByte(t.rgb2[j]);
    cm[j] = ToByte(t.markrgb[j]);
  }
  auto px = [&](int r, int c) { return rgb->data() + 3 * (size_t(r) * w + c); };
  if (t.builtin == "gradient") {
    const double correction = 1.0 / std::sqrt(2.0);
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c) {
        const double x = w > 1 ? 2.0 * c / double(w - 1) - 1 : 0;
        const double y = h > 1 ? 1 - 2.0 * r / double(h - 1) : 0;
        const double pos = 2 * std::sqrt(x * x + y * y) - 1;
        double alpha = 0.5 * (1 + pos / std::sqrt(1 + pos * pos) / correction);
        alpha = std::min(1.0, std::max(0.0, alpha));
        // MuJoCo interpolates rgb2 -> rgb1 (interp(..., rgb2, rgb1, pos)).
        for (int j = 0; j < 3; ++j)
          px(r, c)[j] = ToByte(alpha * t.rgb2[j] + (1 - alpha) * t.rgb1[j]);
      }
  } else if (t.builtin == "checker") {
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        std::memcpy(px(r, c), ((r < h / 2) == (c < w / 2)) ? c1 : c2, 3);
  } else {  // flat
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c) std::memcpy(px(r, c), c1, 3);
  }
  if (t.mark == "edge") {
    for (int r = 0; r < h; ++r) {
      std::memcpy(px(r, 0), cm, 3);
      std::memcpy(px(r, w - 1), cm, 3);
    }
    for (int c = 0; c < w; ++c) {
      std::memcpy(px(0, c), cm, 3);
      std::memcpy(px(h - 1, c), cm, 3);
    }
  } else if (t.mark == "cross") {
    for (int r = 0; r < h; ++r) std::memcpy(px(r, w / 2), cm, 3);
    for (int c = 0; c < w; ++c) std::memcpy(px(h / 2, c), cm, 3);
  } else if (t.mark == "random" && t.random > 0) {
    std::mt19937_64 rng;
    rng.seed(42);
    std::uniform_real_distribution<double> dist(0, 1);
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        if (dist(rng) < t.random) std::memcpy(px(r, c), cm, 3);
  }
}

void GenerateBuiltinCube(const BuiltinTexture& t, std::vector<uint8_t>* faces) {
  const int w = t.width;
  const size_t ww = size_t(w) * w;
  faces->assign(6 * ww * 3, 0);
  uint8_t c1[3], c2[3], cm[3];
  for (int j = 0; j < 3; ++j) {
    c1[j] = ToByte(t.rgb1[j]);
    c2[j] = ToByte(t.rgb2[j]);
    cm[j] = ToByte(t.markrgb[j]);
  }
  auto px = [&](int face, int r, int c) { return faces->data() + 3 * (face * ww + size_t(r) * w + c); };
  // MuJoCo's interp(rgb, rgb1, rgb2, pos): rgb1 at pos = +1.
  auto interp = [&](uint8_t* out, double pos) {
    const double correction = 1.0 / std::sqrt(2.0);
    double alpha = 0.5 * (1 + pos / std::sqrt(1 + pos * pos) / correction);
    alpha = std::min(1.0, std::max(0.0, alpha));
    for (int j = 0; j < 3; ++j) out[j] = ToByte(alpha * t.rgb1[j] + (1 - alpha) * t.rgb2[j]);
  };
  if (t.builtin == "gradient") {
    const double half_pi = 0.5 * 3.14159265358979323846;
    for (int r = 0; r < w; ++r)
      for (int c = 0; c < w; ++c) {
        const double x = w > 1 ? 2.0 * c / double(w - 1) - 1 : 0;
        const double y = w > 1 ? 1 - 2.0 * r / double(w - 1) : 0;
        const double len = std::sqrt(1 + x * x + y * y);
        const double elside = std::asin(y / len) / half_pi;
        const double elup = 1 - std::acos(1.0 / len) / half_pi;
        uint8_t side[3];
        interp(side, elside);
        for (int face : {0, 1, 4, 5}) std::memcpy(px(face, r, c), side, 3);
        interp(px(2, r, c), elup);
        interp(px(3, r, c), -elup);
      }
  } else if (t.builtin == "checker") {
    for (int face = 0; face < 6; ++face) {
      // Front and back swap the colors so the pattern alternates around.
      const uint8_t* a = face < 4 ? c1 : c2;
      const uint8_t* b = face < 4 ? c2 : c1;
      for (int r = 0; r < w; ++r)
        for (int c = 0; c < w; ++c) std::memcpy(px(face, r, c), ((r < w / 2) == (c < w / 2)) ? a : b, 3);
    }
  } else {  // flat: down face takes rgb2
    for (int face = 0; face < 6; ++face)
      for (int r = 0; r < w; ++r)
        for (int c = 0; c < w; ++c) std::memcpy(px(face, r, c), face == 3 ? c2 : c1, 3);
  }
  if (t.mark == "edge") {
    for (int face = 0; face < 6; ++face) {
      for (int r = 0; r < w; ++r) {
        std::memcpy(px(face, r, 0), cm, 3);
        std::memcpy(px(face, r, w - 1), cm, 3);
      }
      for (int c = 0; c < w; ++c) {
        std::memcpy(px(face, 0, c), cm, 3);
        std::memcpy(px(face, w - 1, c), cm, 3);
      }
    }
  } else if (t.mark == "cross") {
    for (int face = 0; face < 6; ++face) {
      for (int r = 0; r < w; ++r) std::memcpy(px(face, r, w / 2), cm, 3);
      for (int c = 0; c < w; ++c) std::memcpy(px(face, w / 2, c), cm, 3);
    }
  } else if (t.mark == "random" && t.random > 0) {
    // One stream over all faces (MuJoCo samples a width x 6*width image).
    std::mt19937_64 rng;
    rng.seed(42);
    std::uniform_real_distribution<double> dist(0, 1);
    for (size_t i = 0; i < 6 * ww; ++i)
      if (dist(rng) < t.random) std::memcpy(faces->data() + 3 * i, cm, 3);
  }
}

void CubeToLatLong(const std::vector<uint8_t>& faces, int face_width, int width, int height,
                   std::vector<uint8_t>* rgb) {
  const double pi = 3.14159265358979323846;
  const int w = face_width;
  rgb->assign(size_t(width) * height * 3, 0);
  // u = (atan2(z, x) + pi/2) / (2 pi), as Storm's dome light lookup.
  std::vector<double> cos_az(width), sin_az(width);
  for (int j = 0; j < width; ++j) {
    const double azimuth = 2 * pi * (j + 0.5) / width - 0.5 * pi;
    cos_az[j] = std::cos(azimuth);
    sin_az[j] = std::sin(azimuth);
  }
  for (int i = 0; i < height; ++i) {
    const double theta = pi * (i + 0.5) / height;  // from +Y
    const double y = std::cos(theta), ring = std::sin(theta);
    for (int j = 0; j < width; ++j) {
      const double x = ring * cos_az[j], z = ring * sin_az[j];
      const double ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
      int face;
      double sc, tc, ma;
      if (ax >= ay && ax >= az) {
        face = x > 0 ? 0 : 1;
        sc = x > 0 ? -z : z;
        tc = -y;
        ma = ax;
      } else if (ay >= az) {
        face = y > 0 ? 2 : 3;
        sc = x;
        tc = y > 0 ? z : -z;
        ma = ay;
      } else {
        face = z > 0 ? 4 : 5;
        sc = z > 0 ? x : -x;
        tc = -y;
        ma = az;
      }
      const int c = std::min(w - 1, std::max(0, int((sc / ma + 1) * 0.5 * w)));
      const int r = std::min(w - 1, std::max(0, int((tc / ma + 1) * 0.5 * w)));
      std::memcpy(rgb->data() + 3 * (size_t(i) * width + j),
                  faces.data() + 3 * (size_t(face) * w * w + size_t(r) * w + c), 3);
    }
  }
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
