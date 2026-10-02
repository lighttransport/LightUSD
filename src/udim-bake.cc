// SPDX-License-Identifier: Apache-2.0
#include "udim-bake.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "image-loader.hh"
#include "image-writer.hh"
#include "safe-arithmetic.hh"

namespace lightusd {
namespace udim {
namespace {
bool Fail(std::string* error, const std::string& message) {
  if (error) *error = "UDIM bake: " + message;
  return false;
}
bool Fits(size_t a, size_t b, size_t limit) {
  return a <= limit && b <= limit - a;
}
float HalfToFloat(uint16_t h) {
  uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 31u, mantissa = h & 1023u;
  uint32_t bits;
  if (!exp) {
    if (!mantissa)
      bits = sign;
    else {
      int e = -14;
      while (!(mantissa & 1024u)) {
        mantissa <<= 1;
        --e;
      }
      bits = sign | (uint32_t(e + 127) << 23) | ((mantissa & 1023u) << 13);
    }
  } else
    bits = sign | ((exp == 31u ? 255u : exp + 112u) << 23) | (mantissa << 13);
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}
float Channel(const Image& image, int x, int y, int c) {
  if (c == 3 && image.channels != 2 && image.channels != 4) return 1.0f;
  int channel = c;
  if (image.channels < 3) channel = c == 3 ? 1 : 0;
  const size_t sample =
      (size_t(y) * size_t(image.width) + size_t(x)) * size_t(image.channels) +
      size_t(channel);
  const uint8_t* p = image.data.data() + sample * size_t(image.bpp / 8);
  if (image.format == Image::PixelFormat::Float) {
    if (image.bpp == 16) {
      uint16_t h;
      std::memcpy(&h, p, 2);
      return HalfToFloat(h);
    }
    float f;
    std::memcpy(&f, p, 4);
    return f;
  }
  if (image.bpp == 16) {
    uint16_t n;
    std::memcpy(&n, p, 2);
    return float(n) / 65535.0f;
  }
  return float(*p) / 255.0f;
}
float Linear(float x) {
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
float Encoded(float x) {
  return x <= 0.0031308f ? 12.92f * x
                         : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
}  // namespace

bool SplitPattern(const std::string& path, std::string* prefix,
                  std::string* suffix, std::string* marker) {
  size_t pos = std::string::npos;
  std::string found;
  for (const char* tag : {"<UDIM>", "%04d", "%(UDIM)d"}) {
    size_t p = path.find(tag);
    if (p == std::string::npos) continue;
    if (pos != std::string::npos || path.find(tag, p + 1) != std::string::npos)
      return false;
    pos = p;
    found = tag;
  }
  if (pos == std::string::npos) return false;
  if (prefix) *prefix = path.substr(0, pos);
  if (suffix) *suffix = path.substr(pos + found.size());
  if (marker) *marker = found;
  return true;
}
uint32_t TileAt(float u, float v) {
  if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u >= 10 || v < 0 ||
      v >= 900)
    return 0;
  const uint32_t id =
      1001u + uint32_t(std::floor(u)) + 10u * uint32_t(std::floor(v));
  return id <= 9999u ? id : 0;
}
bool ValidateOptions(const Options& o, std::string* error) {
  if (o.mode != BakeMode::Off && o.mode != BakeMode::Grid &&
      o.mode != BakeMode::Dense)
    return Fail(error, "invalid layout mode");
  if (!o.max_tiles || o.max_tiles > 8999)
    return Fail(error, "max tiles must be 1..8999");
  if (o.max_atlas_size < 1 || o.max_atlas_size > 32768)
    return Fail(error, "atlas edge must be 1..32768");
  if (o.max_tile_size < 0)
    return Fail(error, "tile resize cap must be nonnegative");
  if (!o.memory_budget_bytes)
    return Fail(error, "memory budget must be positive");
  if (o.dense_padding < 0 || o.dense_padding > 1024)
    return Fail(error, "dense padding must be 0..1024");
  if (o.subdivision_level < 1 || o.subdivision_level > 8)
    return Fail(error, "subdivision level must be 1..8");
  if (o.cross_tile != CrossTilePolicy::Reject &&
      o.cross_tile != CrossTilePolicy::Split)
    return Fail(error, "invalid crossing-face policy");
  return true;
}
bool MakeLayout(const std::vector<uint32_t>& ids, int w, int h,
                const Options& o, Layout* output, std::string* error) {
  if (!output || !ValidateOptions(o, error)) return false;
  if (o.mode == BakeMode::Off) return Fail(error, "baking is disabled");
  if (ids.empty()) return Fail(error, "no tiles resolved");
  if (ids.size() > o.memory_budget_bytes / 64)
    return Fail(error, "layout metadata exceeds memory limit");
  if (ids.size() > o.max_tiles)
    return Fail(error, "tile count exceeds configured limit");
  if (w < 1 || h < 1 || w > o.max_atlas_size || h > o.max_atlas_size)
    return Fail(error, "source tile exceeds atlas size limit");
  Layout l;
  l.mode = o.mode;
  l.tile_width = w;
  l.tile_height = h;
  l.padding = o.mode == BakeMode::Dense ? o.dense_padding : 0;
  int min_u = 9, min_v = 899, max_u = 0, max_v = 0;
  uint32_t previous = 0;
  for (uint32_t id : ids) {
    if (id < 1001 || id > 9999 || id <= previous)
      return Fail(error, "tile IDs must be sorted and unique in 1001..9999");
    previous = id;
    int u = int((id - 1001) % 10), v = int((id - 1001) / 10);
    min_u = std::min(min_u, u);
    max_u = std::max(max_u, u);
    min_v = std::min(min_v, v);
    max_v = std::max(max_v, v);
  }
  l.min_u = min_u;
  l.min_v = min_v;
  if (o.mode == BakeMode::Grid) {
    l.cols = max_u - min_u + 1;
    l.rows = max_v - min_v + 1;
    for (uint32_t id : ids)
      l.cells.push_back(
          {id, int((id - 1001) % 10) - min_u, int((id - 1001) / 10) - min_v});
  } else {
    l.cols = int(std::ceil(std::sqrt(double(ids.size() + 1))));
    l.rows = int((ids.size() + 1 + size_t(l.cols) - 1) / size_t(l.cols));
    for (size_t i = 0; i < ids.size(); ++i)
      l.cells.push_back(
          {ids[i], int(i % size_t(l.cols)), int(i / size_t(l.cols))});
    l.blank_x = int(ids.size() % size_t(l.cols));
    l.blank_y = int(ids.size() / size_t(l.cols));
  }
  const int64_t width = int64_t(l.cols) * (w + 2 * l.padding);
  const int64_t height = int64_t(l.rows) * (h + 2 * l.padding);
  if (width > o.max_atlas_size || height > o.max_atlas_size)
    return Fail(error,
                "stitched atlas exceeds size limit; request resizing or raise "
                "the limit");
  l.width = int(width);
  l.height = int(height);
  size_t bytes;
  if (!safe::mul(size_t(l.width), size_t(l.height), &bytes) ||
      !safe::mul(bytes, size_t(16), &bytes) || bytes > o.memory_budget_bytes)
    return Fail(error, "atlas exceeds working-memory limit");
  *output = std::move(l);
  return true;
}
std::array<float, 2> Layout::remap(float u, float v, uint32_t face_tile) const {
  if (mode == BakeMode::Grid)
    return {
        {(u - float(min_u)) / float(cols), (v - float(min_v)) / float(rows)}};
  const uint32_t id = face_tile ? face_tile : TileAt(u, v);
  int x = blank_x, y = blank_y;
  auto it =
      std::lower_bound(cells.begin(), cells.end(), id,
                       [](const Cell& c, uint32_t n) { return c.id < n; });
  if (it != cells.end() && it->id == id) {
    x = it->x;
    y = it->y;
  }
  const float origin_u = id ? float((id - 1001) % 10) : std::floor(u);
  const float origin_v = id ? float((id - 1001) / 10) : std::floor(v);
  return {{(float(x * (tile_width + 2 * padding) + padding) +
            (u - origin_u) * float(tile_width)) /
               float(width),
           (float(y * (tile_height + 2 * padding) + padding) +
            (v - origin_v) * float(tile_height)) /
               float(height)}};
}

bool AtlasBuilder::begin(const std::vector<uint32_t>& ids, int width,
                         int height, const Options& options, bool srgb,
                         std::string* error) {
  if (!MakeLayout(ids, width, height, options, &layout_, error)) return false;
  options_ = options;
  srgb_ = srgb;
  bits_ = 8;
  floating_ = false;
  pixels_.assign(size_t(layout_.width) * size_t(layout_.height) * 4, 0);
  seen_.assign(ids.size(), 0);
  return true;
}
bool AtlasBuilder::add(uint32_t id, const uint8_t* bytes, size_t size,
                       const std::string& path, std::string* error) {
  if (!bytes || !size || pixels_.empty())
    return Fail(error, "invalid tile or inactive builder");
  auto found =
      std::lower_bound(layout_.cells.begin(), layout_.cells.end(), id,
                       [](const Cell& c, uint32_t n) { return c.id < n; });
  if (found == layout_.cells.end() || found->id != id)
    return Fail(error, "tile absent from layout");
  const size_t index = size_t(found - layout_.cells.begin());
  if (seen_[index]) return Fail(error, "duplicate tile input");
  const Cell& cell = *found;
  const Layout& l = layout_;
  const Options& o = options_;
  const int max_w = l.tile_width, max_h = l.tile_height;
  const size_t atlas_bytes = pixels_.size() * sizeof(float);
  if (!Fits(atlas_bytes, size, o.memory_budget_bytes))
    return Fail(error, "encoded tile exceeds working-memory limit: " + path);
  const size_t decode_limit = o.memory_budget_bytes - atlas_bytes - size;
  auto info =
      image::GetImageInfoFromMemoryBounded(bytes, size, path, decode_limit);
  size_t decode_bytes;
  if (!info ||
      !safe::mul(size_t(info->width), size_t(info->height), &decode_bytes) ||
      !safe::mul(decode_bytes, size_t(64), &decode_bytes) ||
      !Fits(atlas_bytes, size, o.memory_budget_bytes) ||
      !Fits(atlas_bytes + size, decode_bytes, o.memory_budget_bytes))
    return Fail(error, "tile decode exceeds working-memory limit: " + path);
  auto decoded =
      image::LoadImageFromMemoryBounded(bytes, size, path, decode_limit);
  if (!decoded)
    return Fail(error, "cannot decode " + path + ": " + decoded.error());
  const Image& im = decoded->image;
  size_t expected;
  if (im.width <= 0 || im.height <= 0 || uint32_t(im.width) != info->width ||
      uint32_t(im.height) != info->height || im.channels < 1 ||
      im.channels > 4 || (im.bpp != 8 && im.bpp != 16 && im.bpp != 32) ||
      (im.format != Image::PixelFormat::UInt &&
       im.format != Image::PixelFormat::Float) ||
      (im.format == Image::PixelFormat::UInt && im.bpp == 32) ||
      (im.format == Image::PixelFormat::Float && im.bpp == 8) ||
      !safe::mul(size_t(im.width), size_t(im.height), &expected) ||
      !safe::mul(expected, size_t(im.channels * (im.bpp / 8)), &expected) ||
      im.data.size() != expected)
    return Fail(error,
                "unsupported or inconsistent decoded pixel format: " + path);
  floating_ = floating_ || im.format == Image::PixelFormat::Float;
  bits_ = std::max(bits_, im.bpp);
  for (int py = -l.padding; py < max_h + l.padding; ++py) {
    const float sy = (float(std::max(0, std::min(max_h - 1, py))) + 0.5f) *
                         float(im.height) / float(max_h) -
                     0.5f;
    const int y0 = std::max(0, std::min(im.height - 1, int(std::floor(sy))));
    const int y1 =
        std::max(0, std::min(im.height - 1, int(std::floor(sy)) + 1));
    const float fy = sy - std::floor(sy);
    for (int px = -l.padding; px < max_w + l.padding; ++px) {
      const float sx = (float(std::max(0, std::min(max_w - 1, px))) + 0.5f) *
                           float(im.width) / float(max_w) -
                       0.5f;
      const int x0 = std::max(0, std::min(im.width - 1, int(std::floor(sx))));
      const int x1 =
          std::max(0, std::min(im.width - 1, int(std::floor(sx)) + 1));
      const float fx = sx - std::floor(sx);
      const int dx = cell.x * (max_w + 2 * l.padding) + l.padding + px;
      // Image rows run top-down; UDIM v and atlas cell y run bottom-up.
      const int dy =
          l.height - 1 -
          (cell.y * (max_h + 2 * l.padding) + l.padding + (max_h - 1 - py));
      if (im.width == max_w && im.height == max_h) {
        const int source_x = std::max(0, std::min(max_w - 1, px));
        const int source_y = std::max(0, std::min(max_h - 1, py));
        for (int c = 0; c < 4; ++c) {
          const float value = Channel(im, source_x, source_y, c);
          if (!std::isfinite(value))
            return Fail(error, "non-finite image sample: " + path);
          pixels_[(size_t(dy) * size_t(l.width) + size_t(dx)) * 4 + size_t(c)] =
              value;
        }
        continue;
      }
      for (int c = 0; c < 4; ++c) {
        float a = Channel(im, x0, y0, c), b = Channel(im, x1, y0, c);
        float d = Channel(im, x0, y1, c), e = Channel(im, x1, y1, c);
        const bool linearize =
            srgb_ && c < 3 && im.format != Image::PixelFormat::Float;
        if (linearize) {
          a = Linear(a);
          b = Linear(b);
          d = Linear(d);
          e = Linear(e);
        }
        const float aa = Channel(im, x0, y0, 3), ab = Channel(im, x1, y0, 3);
        const float ad = Channel(im, x0, y1, 3), ae = Channel(im, x1, y1, 3);
        if (c < 3) {
          a *= aa;
          b *= ab;
          d *= ad;
          e *= ae;
        }
        float value = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
        if (c < 3) {
          const float alpha =
              (aa + (ab - aa) * fx) * (1 - fy) + (ad + (ae - ad) * fx) * fy;
          value = alpha > 0 ? value / alpha : 0;
        }
        if (linearize) value = Encoded(value);
        if (!std::isfinite(value))
          return Fail(error, "non-finite image sample: " + path);
        pixels_[(size_t(dy) * size_t(l.width) + size_t(dx)) * 4 + size_t(c)] =
            value;
      }
    }
  }
  seen_[index] = 1;
  return true;
}
bool AtlasBuilder::blank(uint32_t id, std::string* error) {
  const auto found =
      std::lower_bound(layout_.cells.begin(), layout_.cells.end(), id,
                       [](const Cell& c, uint32_t n) { return c.id < n; });
  if (found == layout_.cells.end() || found->id != id || pixels_.empty())
    return Fail(error, "blank tile absent from layout");
  const size_t index = size_t(found - layout_.cells.begin());
  if (seen_[index]) return Fail(error, "duplicate blank tile");
  seen_[index] = 1;
  return true;
}
bool AtlasBuilder::thumbnail(int max_edge, std::vector<uint8_t>* output,
                             std::string* error) const {
  if (!output || max_edge < 1 || max_edge > 256 || pixels_.empty() ||
      std::find(seen_.begin(), seen_.end(), 0) != seen_.end())
    return Fail(error, "invalid or incomplete thumbnail input");
  const float scale = std::min(1.0f, float(max_edge) /
                                       float(std::max(layout_.width, layout_.height)));
  Image image;
  image.width = std::max(1, int(std::floor(float(layout_.width) * scale)));
  image.height = std::max(1, int(std::floor(float(layout_.height) * scale)));
  image.channels = 4;
  image.bpp = 8;
  image.format = Image::PixelFormat::UInt;
  const size_t bytes = size_t(image.width) * size_t(image.height) * 4;
  const size_t scratch = bytes * 3 + size_t(image.height) * 64 + 65536;
  if (!Fits(pixels_.size() * sizeof(float), scratch, options_.memory_budget_bytes))
    return Fail(error, "thumbnail exceeds working-memory limit");
  image.data.resize(bytes);
  for (int y = 0; y < image.height; ++y) {
    const int sy = std::min(layout_.height - 1,
                           int((float(y) + 0.5f) * float(layout_.height) / float(image.height)));
    for (int x = 0; x < image.width; ++x) {
      const int sx = std::min(layout_.width - 1,
                             int((float(x) + 0.5f) * float(layout_.width) / float(image.width)));
      for (int c = 0; c < 4; ++c) {
        float value = pixels_[(size_t(sy) * size_t(layout_.width) + size_t(sx)) * 4 + size_t(c)];
        if (!std::isfinite(value)) return Fail(error, "non-finite thumbnail sample");
        value = std::max(0.0f, value);
        if (floating_ && c < 3) value = Encoded(value / (1.0f + value));
        image.data[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + size_t(c)] =
            uint8_t(std::lround(std::min(1.0f, value) * 255));
      }
    }
  }
  image::WriteOption encoding;
  encoding.format = image::WriteImageFormat::PNG;
  auto encoded = image::WriteImageToMemory(image, encoding);
  if (!encoded) return Fail(error, "cannot encode thumbnail: " + encoded.error());
  if (!Fits(pixels_.size() * sizeof(float), encoded->size(), options_.memory_budget_bytes))
    return Fail(error, "encoded thumbnail exceeds working-memory limit");
  *output = std::move(encoded.value());
  return true;
}
bool AtlasBuilder::finish(const std::string& format, int quality, Atlas* output,
                          std::string* error, size_t retained_bytes) {
  if (!output || pixels_.empty() ||
      std::find(seen_.begin(), seen_.end(), 0) != seen_.end())
    return Fail(error, "atlas is incomplete");
  if (quality < 1 || quality > 100 ||
      (format != "keep" && format != "png" && format != "jpg" &&
       format != "jpeg" && format != "exr"))
    return Fail(error, "invalid output encoding options");
  Atlas result;
  result.layout = layout_;
  const Layout& l = layout_;
  const Options& o = options_;
  const size_t count = pixels_.size() / 4;
  const size_t atlas_bytes = pixels_.size() * sizeof(float);
  Image image;
  image.width = l.width;
  image.height = l.height;
  image.channels = 4;
  image::WriteOption encoding;
  encoding.jpeg_quality = quality;
  if (format == "jpeg" || format == "jpg") {
    if (floating_ || bits_ > 8)
      return Fail(error, "JPEG would discard source precision");
    encoding.format = image::WriteImageFormat::JPEG;
    result.extension = "jpg";
    image.channels = 3;
  } else if (floating_ || format == "exr") {
    if (format == "png")
      return Fail(error, "PNG cannot preserve floating-point tiles");
    encoding.format = image::WriteImageFormat::EXR;
    result.extension = "exr";
  } else {
    encoding.format = image::WriteImageFormat::PNG;
    result.extension = "png";
  }
  image.bpp = result.extension == "exr" ? 32 : bits_;
  image.format = result.extension == "exr" ? Image::PixelFormat::Float
                                           : Image::PixelFormat::UInt;
  size_t final_bytes = count * size_t(image.channels) * size_t(image.bpp / 8);
  // Include encoded output and encoder scratch in the conservative preflight.
  size_t scratch;
  if (!safe::mul(final_bytes, size_t(3), &scratch) ||
      !safe::add(scratch, size_t(l.height) * 64 + 65536, &scratch) ||
      !Fits(atlas_bytes, retained_bytes, o.memory_budget_bytes) ||
      !Fits(atlas_bytes + retained_bytes, scratch, o.memory_budget_bytes))
    return Fail(error, "image encoding exceeds working-memory limit");
  image.data.resize(final_bytes);
  for (size_t i = 0; i < count; ++i)
    for (int c = 0; c < image.channels; ++c) {
      const float f = pixels_[i * 4 + size_t(c)];
      uint8_t* dest =
          image.data.data() +
          (i * size_t(image.channels) + size_t(c)) * size_t(image.bpp / 8);
      if (image.format == Image::PixelFormat::Float)
        std::memcpy(dest, &f, 4);
      else if (image.bpp == 16) {
        uint16_t n =
            uint16_t(std::lround(std::max(0.0f, std::min(1.0f, f)) * 65535));
        std::memcpy(dest, &n, 2);
      } else
        *dest = uint8_t(std::lround(std::max(0.0f, std::min(1.0f, f)) * 255));
    }
  auto encoded = image::WriteImageToMemory(image, encoding);
  if (!encoded) return Fail(error, "cannot encode atlas: " + encoded.error());
  result.bytes = std::move(encoded.value());
  std::vector<float>().swap(pixels_);
  *output = std::move(result);
  return true;
}
bool InspectLayout(const std::string& pattern, const std::vector<uint32_t>& ids,
                   const Fetch& fetch, const Options& o, Layout* output,
                   std::string* error) {
  if (!output || !fetch || !ValidateOptions(o, error)) return false;
  if (ids.empty() || ids.size() > o.max_tiles)
    return Fail(error, "tile count is empty or exceeds configured limit");
  std::string pre, post;
  if (!SplitPattern(pattern, &pre, &post))
    return Fail(error, "invalid pattern: " + pattern);
  int max_w = 0, max_h = 0;
  for (uint32_t id : ids) {
    std::vector<uint8_t> bytes;
    const std::string path = pre + std::to_string(id) + post;
    if (!fetch(path, &bytes, error, o.memory_budget_bytes)) return false;
    if (bytes.size() > o.memory_budget_bytes)
      return Fail(error, "encoded tile exceeds memory limit: " + path);
    auto info = image::GetImageInfoFromMemoryBounded(
        bytes.data(), bytes.size(), path, o.memory_budget_bytes - bytes.size());
    if (!info || !info->width || !info->height || info->width > 32768 ||
        info->height > 32768)
      return Fail(error, "invalid or oversized tile header: " + path);
    int width = int(info->width), height = int(info->height);
    if (o.max_tile_size > 0 && std::max(width, height) > o.max_tile_size) {
      const double scale =
          double(o.max_tile_size) / double(std::max(width, height));
      width = std::max(1, int(std::lround(double(width) * scale)));
      height = std::max(1, int(std::lround(double(height) * scale)));
    }
    max_w = std::max(max_w, width);
    max_h = std::max(max_h, height);
  }
  return MakeLayout(ids, max_w, max_h, o, output, error);
}
bool BakeAtlas(const std::string& pattern, const std::vector<uint32_t>& ids,
               const Fetch& fetch, const Options& o, bool srgb,
               const std::string& format, int quality, Atlas* output,
               std::string* error, const Layout* common) {
  Layout layout;
  if (common)
    layout = *common;
  else if (!InspectLayout(pattern, ids, fetch, o, &layout, error))
    return false;
  std::string pre, post;
  if (!SplitPattern(pattern, &pre, &post))
    return Fail(error, "invalid UDIM pattern");
  std::vector<uint32_t> layout_ids;
  for (const auto& cell : layout.cells) layout_ids.push_back(cell.id);
  AtlasBuilder builder;
  if (!builder.begin(layout_ids, layout.tile_width, layout.tile_height, o, srgb,
                     error))
    return false;
  for (uint32_t id : ids) {
    const std::string path = pre + std::to_string(id) + post;
    std::vector<uint8_t> bytes;
    if (!fetch(path, &bytes, error, builder.inputBudget()) ||
        !builder.add(id, bytes.data(), bytes.size(), path, error))
      return false;
  }
  for (uint32_t id : layout_ids)
    if (!std::binary_search(ids.begin(), ids.end(), id) &&
        !builder.blank(id, error))
      return false;
  return builder.finish(format, quality, output, error);
}
}  // namespace udim
}  // namespace lightusd
