// SPDX-License-Identifier: Apache-2.0
#include "ptex_atlas.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "external/textools/ptex/include/tptex_sample.h"

namespace lusdview {

PtexPhysicalPageCache::PtexPhysicalPageCache(uint32_t slotCount)
    : slots_(slotCount) {}

bool PtexPhysicalPageCache::Request(
    uint32_t face, PtexPhysicalPageAssignment* assignment) {
  if (!assignment || slots_.empty()) return false;
  const auto found = byFace_.find(face);
  if (found != byFace_.end()) {
    Slot& slot = slots_[found->second];
    slot.stamp = ++clock_;
    assignment->slot = found->second;
    assignment->evictedFace = ~uint32_t{0};
    assignment->hit = true;
    ++hits_;
    return true;
  }

  ++misses_;
  uint32_t selected = 0;
  if (residentCount_ < slots_.size()) {
    // Slots are populated in index order, which keeps tests and upload traces
    // reproducible across standard-library implementations.
    selected = residentCount_++;
  } else {
    for (uint32_t i = 1; i < slots_.size(); ++i) {
      if (slots_[i].stamp < slots_[selected].stamp) selected = i;
    }
  }

  Slot& slot = slots_[selected];
  const uint32_t evicted = slot.face;
  if (evicted != ~uint32_t{0}) {
    byFace_.erase(evicted);
    ++evictions_;
  }
  slot.face = face;
  slot.stamp = ++clock_;
  byFace_[face] = selected;
  assignment->slot = selected;
  assignment->evictedFace = evicted;
  assignment->hit = false;
  return true;
}

void PtexPhysicalPageCache::Clear() {
  std::fill(slots_.begin(), slots_.end(), Slot{});
  byFace_.clear();
  residentCount_ = 0;
  clock_ = 0;
  hits_ = 0;
  misses_ = 0;
  evictions_ = 0;
}
namespace {

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1fu;
  uint32_t mantissa = h & 0x3ffu;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exp = 127u - 15u + 1u;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exp;
      }
      mantissa &= 0x3ffu;
      bits = sign | (exp << 23) | (mantissa << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exp + (127u - 15u)) << 23) | (mantissa << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

uint16_t U16(const uint8_t* p) {
  const uint32_t value = uint32_t(p[0]) | (uint32_t(p[1]) << 8u);
  return static_cast<uint16_t>(value);
}

float Component(const lightusd::ptx::FaceImage& face, size_t pixel,
                uint16_t channel) {
  const size_t component = pixel * face.channels + channel;
  switch (face.dataType) {
    case lightusd::ptx::DataType::UInt8:
      return float(face.data[component]) / 255.0f;
    case lightusd::ptx::DataType::UInt16:
      return float(U16(face.data.data() + component * 2)) / 65535.0f;
    case lightusd::ptx::DataType::Half:
      return HalfToFloat(U16(face.data.data() + component * 2));
    case lightusd::ptx::DataType::Float: {
      float value = 0.0f;
      std::memcpy(&value, face.data.data() + component * 4, sizeof(value));
      return value;
    }
  }
  return 0.0f;
}

uint8_t Byte(float value) {
  if (!std::isfinite(value)) value = 0.0f;
  value = std::max(0.0f, std::min(1.0f, value));
  return static_cast<uint8_t>(std::lround(value * 255.0f));
}

light3d::Image ConvertFace(const lightusd::ptx::FaceImage& face) {
  light3d::Image out;
  out.width = static_cast<int>(face.width);
  out.height = static_cast<int>(face.height);
  out.channels = 4;
  const size_t pixels = size_t(face.width) * face.height;
  out.data.resize(pixels * 4);
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    const float r = Component(face, pixel, 0);
    const float g = face.channels > 1 ? Component(face, pixel, 1) : r;
    const float b = face.channels > 2 ? Component(face, pixel, 2) : r;
    const float a = face.channels == 2
                        ? g
                        : (face.channels > 3 ? Component(face, pixel, 3)
                                             : 1.0f);
    uint8_t* dst = out.data.data() + pixel * 4;
    dst[0] = Byte(r);
    dst[1] = Byte(face.channels == 2 ? r : g);
    dst[2] = Byte(face.channels == 2 ? r : b);
    dst[3] = Byte(a);
  }
  return out;
}

light3d::Image ResizeFace(const light3d::Image& src, uint32_t width,
                          uint32_t height) {
  if (src.width == static_cast<int>(width) &&
      src.height == static_cast<int>(height)) {
    return src;
  }
  light3d::Image out;
  out.width = static_cast<int>(width);
  out.height = static_cast<int>(height);
  out.channels = 4;
  out.data.resize(size_t(width) * height * 4u);
  // Area-center nearest sampling is sufficient for the permanent coarse
  // fallback; requested-quality pages always use the native authored mip.
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t sy = std::min<uint32_t>(
        static_cast<uint32_t>(src.height - 1),
        static_cast<uint32_t>((uint64_t(y) * src.height + height / 2u) /
                              height));
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t sx = std::min<uint32_t>(
          static_cast<uint32_t>(src.width - 1),
          static_cast<uint32_t>((uint64_t(x) * src.width + width / 2u) /
                                width));
      std::memcpy(out.data.data() + (size_t(y) * width + x) * 4u,
                  src.data.data() + (size_t(sy) * src.width + sx) * 4u, 4u);
    }
  }
  return out;
}

struct Placement {
  uint32_t face{0};
  uint32_t mip{0};
  uint32_t baseMip{0};
  uint32_t baseWidth{0};
  uint32_t baseHeight{0};
  bool virtualDownsample{false};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t x{0};
  uint32_t y{0};
  uint32_t chainLevel{0};
};

bool Pack(std::vector<Placement>* placements, uint32_t gutter,
          uint32_t maxEdge, uint32_t* width, uint32_t* height) {
  if (!placements || !width || !height || maxEdge == 0) return false;
  std::vector<size_t> order(placements->size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const Placement& pa = (*placements)[a];
    const Placement& pb = (*placements)[b];
    if (pa.height != pb.height) return pa.height > pb.height;
    return pa.width > pb.width;
  });
  uint32_t x = 0, y = 0, rowHeight = 0, usedWidth = 0;
  for (size_t index : order) {
    Placement& p = (*placements)[index];
    const uint64_t outerW = uint64_t(p.width) + uint64_t(gutter) * 2u;
    const uint64_t outerH = uint64_t(p.height) + uint64_t(gutter) * 2u;
    if (outerW > maxEdge || outerH > maxEdge) return false;
    if (uint64_t(x) + outerW > maxEdge) {
      x = 0;
      y += rowHeight;
      rowHeight = 0;
    }
    if (uint64_t(y) + outerH > maxEdge) return false;
    p.x = x + gutter;
    p.y = y + gutter;
    x += static_cast<uint32_t>(outerW);
    rowHeight = std::max(rowHeight, static_cast<uint32_t>(outerH));
    usedWidth = std::max(usedWidth, x);
  }
  *width = std::max(1u, usedWidth);
  *height = std::max(1u, y + rowHeight);
  return true;
}

void CopyPixel(const light3d::Image& src, uint32_t sx, uint32_t sy,
               light3d::Image* dst, uint32_t dx, uint32_t dy) {
  if (!dst || sx >= static_cast<uint32_t>(src.width) ||
      sy >= static_cast<uint32_t>(src.height) ||
      dx >= static_cast<uint32_t>(dst->width) ||
      dy >= static_cast<uint32_t>(dst->height)) return;
  std::memcpy(dst->data.data() + (size_t(dy) * dst->width + dx) * 4,
              src.data.data() + (size_t(sy) * src.width + sx) * 4, 4);
}

void CopyWithClampGutter(const light3d::Image& face, const Placement& p,
                         uint32_t gutter, light3d::Image* atlas) {
  // Ptex rows begin at v=0 (bottom). Atlas images are top-row-first, so reverse
  // source Y while copying the inner rectangle.
  for (uint32_t y = 0; y < p.height; ++y) {
    const uint32_t sy = p.height - 1u - y;
    std::memcpy(atlas->data.data() +
                    (size_t(p.y + y) * atlas->width + p.x) * 4u,
                face.data.data() + (size_t(sy) * face.width) * 4u,
                size_t(p.width) * 4u);
  }
  for (uint32_t d = 1; d <= gutter; ++d) {
    for (uint32_t x = 0; x < p.width; ++x) {
      CopyPixel(*atlas, p.x + x, p.y, atlas, p.x + x, p.y - d);
      CopyPixel(*atlas, p.x + x, p.y + p.height - 1u, atlas, p.x + x,
                p.y + p.height - 1u + d);
    }
    for (uint32_t y = 0; y < p.height; ++y) {
      CopyPixel(*atlas, p.x, p.y + y, atlas, p.x - d, p.y + y);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y + y, atlas,
                p.x + p.width - 1u + d, p.y + y);
    }
  }
  for (uint32_t gy = 0; gy < gutter; ++gy) {
    for (uint32_t gx = 0; gx < gutter; ++gx) {
      CopyPixel(*atlas, p.x, p.y, atlas, p.x - 1u - gx, p.y - 1u - gy);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y, atlas,
                p.x + p.width + gx, p.y - 1u - gy);
      CopyPixel(*atlas, p.x, p.y + p.height - 1u, atlas,
                p.x - 1u - gx, p.y + p.height + gy);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y + p.height - 1u, atlas,
                p.x + p.width + gx, p.y + p.height + gy);
    }
  }
}

// Common Island fast path: most coarse color faces are already RGBA8 at the
// selected mip. Avoid allocating a temporary Image and converting every pixel
// before copying into the atlas. The atlas still uses the same vertically
// flipped convention and clamp gutters as the generic path.
void CopyRgba8FaceWithClampGutter(const lightusd::ptx::FaceImage& face,
                                  const Placement& p, uint32_t gutter,
                                  light3d::Image* atlas) {
  for (uint32_t y = 0; y < p.height; ++y) {
    const uint32_t sy = p.height - 1u - y;
    std::memcpy(atlas->data.data() +
                    (size_t(p.y + y) * atlas->width + p.x) * 4u,
                face.data.data() + (size_t(sy) * face.width) * 4u,
                size_t(p.width) * 4u);
  }
  for (uint32_t d = 1; d <= gutter; ++d) {
    for (uint32_t x = 0; x < p.width; ++x) {
      CopyPixel(*atlas, p.x + x, p.y, atlas, p.x + x, p.y - d);
      CopyPixel(*atlas, p.x + x, p.y + p.height - 1u, atlas,
                p.x + x, p.y + p.height - 1u + d);
    }
    for (uint32_t y = 0; y < p.height; ++y) {
      CopyPixel(*atlas, p.x, p.y + y, atlas, p.x - d, p.y + y);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y + y, atlas,
                p.x + p.width - 1u + d, p.y + y);
    }
  }
  for (uint32_t gy = 0; gy < gutter; ++gy) {
    for (uint32_t gx = 0; gx < gutter; ++gx) {
      CopyPixel(*atlas, p.x, p.y, atlas, p.x - 1u - gx, p.y - 1u - gy);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y, atlas,
                p.x + p.width + gx, p.y - 1u - gy);
      CopyPixel(*atlas, p.x, p.y + p.height - 1u, atlas,
                p.x - 1u - gx, p.y + p.height + gy);
      CopyPixel(*atlas, p.x + p.width - 1u, p.y + p.height - 1u, atlas,
                p.x + p.width + gx, p.y + p.height + gy);
    }
  }
}

// Bilinear RGBA8 sample of a converted face page. ConvertFace keeps Ptex's
// v=0-at-bottom row order, so the atlas copy flips it but this sampler does not.
void SampleRgba8Page(const light3d::Image& page, float u, float v,
                     uint8_t out[4]) {
  const int w = page.width, h = page.height;
  if (w <= 0 || h <= 0 || page.data.size() < size_t(w) * size_t(h) * 4u) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
  const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
  const int x0 = std::clamp(int(std::floor(fx)), 0, w - 1);
  const int y0 = std::clamp(int(std::floor(fy)), 0, h - 1);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
  const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
  for (int c = 0; c < 4; ++c) {
    auto texel = [&](int x, int y) {
      return page.data[(size_t(y) * size_t(w) + size_t(x)) * 4u + size_t(c)];
    };
    const float a = texel(x0, y0) * (1 - tx) + texel(x1, y0) * tx;
    const float b = texel(x0, y1) * (1 - tx) + texel(x1, y1) * tx;
    out[c] = static_cast<uint8_t>(std::lround(a * (1 - ty) + b * ty));
  }
}

// Fill the first gutter texel around each face from its Ptex neighbour. The
// descriptor shader permits its bilinear taps to read one texel outside the
// face rectangle, so this replaces the previous clamp-only gutter with a
// cross-face sample. Faces without adjacency keep the clamp gutter.
void FillCrossFaceGutters(const lightusd::ptx::Reader& reader,
                          const std::vector<Placement>& placements,
                          const PtexAtlasOptions& options,
                          light3d::Image* atlas) {
  if (!atlas || options.gutter == 0) return;
  const lightusd::ptx::Info& info = reader.info();
  if (info.faceInfo.empty()) return;
  std::vector<tptex_face_info> faces(info.faceInfo.size());
  for (size_t i = 0; i < info.faceInfo.size(); ++i) {
    const lightusd::ptx::FaceInfo& fi = info.faceInfo[i];
    faces[i].res.ulog2 = static_cast<int8_t>(fi.ures);
    faces[i].res.vlog2 = static_cast<int8_t>(fi.vres);
    faces[i].adjedges = fi.adjEdges;
    faces[i].flags = fi.flags;
    for (int e = 0; e < 4; ++e)
      faces[i].adjfaces[e] = static_cast<int32_t>(fi.adjFaces[e]);
  }
  const size_t cacheBytes =
      std::max<size_t>(options.maxDecodedCacheBytes, 64ull * 1024ull * 1024ull);
  PtexFacePageCache pages(cacheBytes);
  std::unordered_map<uint64_t, light3d::Image> converted;
  converted.reserve(64);
  auto fill = [&](uint32_t x, uint32_t y, uint32_t face, float u, float v,
                  uint32_t mip) {
    if (x >= uint32_t(atlas->width) || y >= uint32_t(atlas->height)) return;
    uint32_t mapped = face;
    float mu = u, mv = v;
    if (!tptex_resolve_tap(faces.data(), uint32_t(faces.size()), &mapped, &mu,
                           &mv))
      return;
    if (mapped == face) return;
    const uint64_t key = (uint64_t(mapped) << 32u) | uint64_t(mip);
    auto found = converted.find(key);
    if (found == converted.end()) {
      std::string err;
      const lightusd::ptx::FaceImage* page =
          pages.Fetch(reader, mapped, mip, options.maxDecodedFaceBytes, &err);
      if (!page) return;
      if (converted.size() > 128) converted.clear();
      found = converted.emplace(key, ConvertFace(*page)).first;
    }
    uint8_t rgba[4];
    SampleRgba8Page(found->second, mu, mv, rgba);
    std::memcpy(atlas->data.data() +
                    (size_t(y) * size_t(atlas->width) + size_t(x)) * 4u,
                rgba, 4u);
  };
  for (const Placement& p : placements) {
    if (p.virtualDownsample || p.width == 0 || p.height == 0) continue;
    const float w = float(p.width), h = float(p.height);
    // Top and bottom rows (atlas y grows downward; Ptex v grows upward).
    for (uint32_t x = 0; x < p.width; ++x) {
      const float u = (float(x) + 0.5f) / w;
      fill(p.x + x, p.y - 1u, p.face, u, 1.0f + 0.5f / h, p.mip);
      fill(p.x + x, p.y + p.height, p.face, u, -0.5f / h, p.mip);
    }
    // Left and right columns.
    for (uint32_t y = 0; y < p.height; ++y) {
      const float v = 1.0f - (float(y) + 0.5f) / h;
      fill(p.x - 1u, p.y + y, p.face, -0.5f / w, v, p.mip);
      fill(p.x + p.width, p.y + y, p.face, 1.0f + 0.5f / w, v, p.mip);
    }
    // Four corners (two-hop diagonal adjacency).
    fill(p.x - 1u, p.y - 1u, p.face, -0.5f / w, 1.0f + 0.5f / h, p.mip);
    fill(p.x + p.width, p.y - 1u, p.face, 1.0f + 0.5f / w,
         1.0f + 0.5f / h, p.mip);
    fill(p.x - 1u, p.y + p.height, p.face, -0.5f / w, -0.5f / h, p.mip);
    fill(p.x + p.width, p.y + p.height, p.face, 1.0f + 0.5f / w,
         -0.5f / h, p.mip);
  }
}

}  // namespace

const lightusd::ptx::FaceImage* PtexFacePageCache::Fetch(
    const lightusd::ptx::Reader& reader, uint32_t face, uint32_t mip,
    size_t maxDecodedFaceBytes, std::string* err) {
  const uint64_t key = (uint64_t(face) << 32u) | uint64_t(mip);
  const auto found = byKey_.find(key);
  if (found != byKey_.end()) {
    ++stats_.hits;
    entries_.splice(entries_.begin(), entries_, found->second);
    return &entries_.front().image;
  }

  ++stats_.misses;
  lightusd::ptx::FaceImage decoded;
  if (!reader.ReadFace(face, mip, maxDecodedFaceBytes, &decoded, err)) {
    return nullptr;
  }
  stats_.decodedBytes += decoded.data.size();
  const size_t bytes = decoded.data.size();
  if (maxBytes_ == 0 || bytes > maxBytes_) {
    transient_ = std::move(decoded);
    return &transient_;
  }

  while (!entries_.empty() && stats_.residentBytes + bytes > maxBytes_) {
    auto last = std::prev(entries_.end());
    stats_.residentBytes -= last->image.data.size();
    byKey_.erase(last->key);
    entries_.erase(last);
    ++stats_.evictions;
  }
  entries_.push_front(Entry{key, std::move(decoded)});
  byKey_[key] = entries_.begin();
  stats_.residentBytes += bytes;
  stats_.peakResidentBytes =
      std::max(stats_.peakResidentBytes, stats_.residentBytes);
  return &entries_.front().image;
}

void PtexFacePageCache::Clear() {
  entries_.clear();
  byKey_.clear();
  transient_ = lightusd::ptx::FaceImage{};
  stats_.residentBytes = 0;
}

bool BuildPtexPage(const lightusd::ptx::Reader& reader, uint32_t face,
                   uint32_t mip, uint32_t gutter,
                   size_t maxDecodedFaceBytes, light3d::Image* page,
                   DrawPtexFaceRectCPU* inner, std::string* err) {
  if (!page || !inner) {
    if (err) *err = "Ptex page output is null";
    return false;
  }
  PtexFacePageCache decodedCache(0);
  const lightusd::ptx::FaceImage* decoded = decodedCache.Fetch(
      reader, face, mip, maxDecodedFaceBytes, err);
  if (!decoded) return false;
  const uint64_t outerWidth = uint64_t(decoded->width) + gutter * 2u;
  const uint64_t outerHeight = uint64_t(decoded->height) + gutter * 2u;
  if (outerWidth > uint64_t(std::numeric_limits<int>::max()) ||
      outerHeight > uint64_t(std::numeric_limits<int>::max()) ||
      outerWidth * outerHeight >
          uint64_t(std::numeric_limits<size_t>::max()) / 4u) {
    if (err) *err = "Ptex page dimensions overflow";
    return false;
  }
  page->width = static_cast<int>(outerWidth);
  page->height = static_cast<int>(outerHeight);
  page->channels = 4;
  page->data.assign(size_t(outerWidth * outerHeight) * 4u, 0);
  Placement placement;
  placement.face = face;
  placement.mip = mip;
  placement.width = decoded->width;
  placement.height = decoded->height;
  placement.x = gutter;
  placement.y = gutter;
  CopyWithClampGutter(ConvertFace(*decoded), placement, gutter, page);
  inner->x = gutter;
  inner->y = gutter;
  inner->width = decoded->width;
  inner->height = decoded->height;
  inner->mipLevel = static_cast<uint16_t>(mip);
  return true;
}

void EncodePtexFaceRectTexels(const DrawPtexFaceRectCPU& rect,
                              uint8_t texels[10u * 4u]) {
  if (!texels) return;
  std::fill(texels, texels + 10u * 4u, uint8_t{0});
  const uint16_t values[5] = {
      static_cast<uint16_t>(rect.x), static_cast<uint16_t>(rect.y),
      static_cast<uint16_t>(rect.width), static_cast<uint16_t>(rect.height),
      static_cast<uint16_t>(rect.baseMip)};
  for (uint32_t byte = 0; byte < 10; ++byte) {
    texels[byte * 4u + 3u] = static_cast<uint8_t>(
        (values[byte / 2u] >> ((byte & 1u) * 8u)) & 0xffu);
  }
}

bool BuildPtexAtlas(const lightusd::ptx::Reader& reader,
                    const PtexAtlasOptions& options, bool /*srgb*/,
                    light3d::Image* image,
                    std::vector<DrawPtexFaceRectCPU>* faceRects,
                    PtexAtlasBuildStats* stats, std::string* err) {
  if (!image || !faceRects) {
    if (err) *err = "Ptex atlas output is null";
    return false;
  }
  const lightusd::ptx::Info& info = reader.info();
  if (info.meshType != 1 || info.faces == 0 || info.channels == 0 ||
      info.channels > 4) {
    if (err) *err = "Ptex atlas requires a non-empty quad texture with 1-4 channels";
    return false;
  }
  PtexAtlasBuildStats localStats;
  const uint32_t mip_levels = std::max(1u, options.mipLevels);
  std::vector<Placement> placements;
  placements.reserve(size_t(info.faces) * size_t(mip_levels));
  for (uint32_t face = 0; face < info.faces; ++face) {
    Placement p;
    p.face = face;
    uint32_t w = info.faceInfo[face].width();
    uint32_t h = info.faceInfo[face].height();
    while (p.mip + 1u < info.levels && options.maxFaceEdge > 0 &&
           std::max(w, h) > options.maxFaceEdge) {
      ++p.mip;
      w = std::max(1u, w >> 1u);
      h = std::max(1u, h >> 1u);
    }
    p.width = w;
    p.height = h;
    p.baseMip = p.mip;
    p.baseWidth = w;
    p.baseHeight = h;
    if (options.initialFaceLimit > 0 &&
        face >= options.initialFaceLimit) {
      p.width = 1;
      p.height = 1;
      p.virtualDownsample = true;
    }
    p.chainLevel = 0;
    placements.push_back(p);
    if (mip_levels <= 1 || p.virtualDownsample) continue;
    for (uint32_t level = 1; level < mip_levels; ++level) {
      Placement q = p;
      q.chainLevel = level;
      q.mip = p.mip + level;
      if (q.mip >= info.levels) break;
      q.width = std::max(1u, p.width >> level);
      q.height = std::max(1u, p.height >> level);
      // Chain levels are intentional, not budget-driven fallbacks; keep the
      // physical-page-cache test from treating them as downsampled faces.
      q.baseMip = q.mip;
      placements.push_back(q);
    }
  }

  uint32_t atlasWidth = 0, atlasHeight = 0, imageHeight = 0;
  uint32_t rectTexelOffset = 0;
  uint32_t physicalCacheOffsetY = 0;
  uint32_t physicalCacheSlotEdge = 0;
  uint32_t physicalCacheSlots = 0;
  for (;;) {
    if (Pack(&placements, options.gutter, options.maxAtlasEdge, &atlasWidth,
             &atlasHeight)) {
      const uint32_t packedAtlasWidth = atlasWidth;
      const bool needsPhysicalCache = options.forcePhysicalCache || std::any_of(
          placements.begin(), placements.end(),
          [](const Placement& p) {
            return p.mip > p.baseMip || p.virtualDownsample;
          });
      const uint64_t requestedSlotEdge =
          uint64_t(options.maxFaceEdge) + uint64_t(options.gutter) * 2u;
      const uint64_t requestedSlotBytes = requestedSlotEdge *
                                          requestedSlotEdge * 4u;
      physicalCacheSlotEdge =
          needsPhysicalCache && requestedSlotEdge <= options.maxAtlasEdge &&
                  requestedSlotEdge <= std::numeric_limits<uint32_t>::max() &&
                  requestedSlotBytes <= options.maxPhysicalCacheBytes
              ? static_cast<uint32_t>(requestedSlotEdge)
              : 0u;
      if (physicalCacheSlotEdge > 0) {
        atlasWidth = std::max(atlasWidth, physicalCacheSlotEdge);
      }
      const uint64_t rectTexels =
          uint64_t(info.faces) * uint64_t(mip_levels) * 10u;
      uint64_t rectRows =
          (rectTexels + uint64_t(atlasWidth) - 1u) / atlasWidth;
      const uint64_t slotBytes = uint64_t(physicalCacheSlotEdge) *
                                 physicalCacheSlotEdge * 4u;
      const uint64_t desiredSlots = slotBytes > 0
                                        ? options.maxPhysicalCacheBytes / slotBytes
                                        : 0u;
      const uint64_t slotsPerRow = physicalCacheSlotEdge > 0
                                       ? atlasWidth / physicalCacheSlotEdge
                                       : 0u;
      uint64_t selectedSlots = desiredSlots;
      uint64_t cacheRows = 0;
      while (selectedSlots > 0) {
        cacheRows = (selectedSlots + slotsPerRow - 1u) / slotsPerRow;
        const uint64_t candidateHeight =
            uint64_t(atlasHeight) + cacheRows * physicalCacheSlotEdge + rectRows;
        const uint64_t candidateBytes = uint64_t(atlasWidth) * candidateHeight * 4u;
        if (candidateHeight <= options.maxAtlasEdge &&
            (options.maxAtlasBytes == 0 || candidateBytes <= options.maxAtlasBytes)) {
          break;
        }
        --selectedSlots;
      }
      const bool canShrinkFallback = std::any_of(
          placements.begin(), placements.end(), [](const Placement& p) {
            return p.width > 1u || p.height > 1u;
          });
      const bool retryForCache = needsPhysicalCache && desiredSlots > 0u &&
                                 selectedSlots == 0u && canShrinkFallback;
      if (selectedSlots == 0) {
        cacheRows = 0;
        physicalCacheSlotEdge = 0;
        atlasWidth = packedAtlasWidth;
        rectRows = (rectTexels + uint64_t(atlasWidth) - 1u) / atlasWidth;
      }
      // BCn residency updates address 4x4 blocks. Keep the mutable physical
      // cache origin block-aligned even when the packed fallback atlas ends on
      // an odd row (the raw atlas path does not require this padding).
      const uint32_t cacheBaseY =
          cacheRows > 0 ? (atlasHeight + 3u) & ~uint32_t{3u} : atlasHeight;
      const uint64_t totalHeight = uint64_t(cacheBaseY) +
                                   cacheRows * physicalCacheSlotEdge + rectRows;
      const uint64_t bytes = uint64_t(atlasWidth) * totalHeight * 4u;
      if (!retryForCache && totalHeight <= options.maxAtlasEdge &&
          (options.maxAtlasBytes == 0 || bytes <= options.maxAtlasBytes)) {
        imageHeight = static_cast<uint32_t>(totalHeight);
        physicalCacheOffsetY = cacheBaseY;
        physicalCacheSlots = static_cast<uint32_t>(selectedSlots);
        rectTexelOffset = atlasWidth * static_cast<uint32_t>(
            uint64_t(cacheBaseY) + cacheRows * physicalCacheSlotEdge);
        break;
      }
    }
    size_t best = placements.size();
    uint64_t bestArea = 0;
    for (size_t i = 0; i < placements.size(); ++i) {
      const Placement& p = placements[i];
      if (p.width == 1 && p.height == 1) continue;
      const uint64_t area = uint64_t(p.width) * p.height;
      if (area > bestArea) { bestArea = area; best = i; }
    }
    if (best == placements.size()) {
      if (err) *err = "Ptex atlas does not fit configured edge/byte budget";
      return false;
    }
    Placement& p = placements[best];
    if (!p.virtualDownsample && p.mip + 1u < info.levels) {
      ++p.mip;
    } else {
      p.virtualDownsample = true;
    }
    p.width = std::max(1u, p.width >> 1u);
    p.height = std::max(1u, p.height >> 1u);
  }

  localStats.downsampledFaces = static_cast<uint32_t>(std::count_if(
      placements.begin(), placements.end(), [](const Placement& p) {
        return p.chainLevel == 0 &&
               (p.mip > 0 || p.width < p.baseWidth ||
                p.height < p.baseHeight);
      }));

  image->width = static_cast<int>(atlasWidth);
  image->height = static_cast<int>(imageHeight);
  image->channels = 4;
  image->data.assign(size_t(atlasWidth) * imageHeight * 4, 0);
  faceRects->assign(size_t(info.faces) * size_t(mip_levels),
                    DrawPtexFaceRectCPU{});
  PtexFacePageCache pageCache(options.maxDecodedCacheBytes);
  for (const Placement& p : placements) {
    if (options.initialFaceLimit > 0 &&
        p.face >= options.initialFaceLimit) {
      DrawPtexFaceRectCPU& rect =
          (*faceRects)[size_t(p.face) * mip_levels + p.chainLevel];
      rect.x = p.x;
      rect.y = p.y;
      rect.width = 1;
      rect.height = 1;
      rect.mipLevel = static_cast<uint16_t>(p.mip);
      rect.baseMip = static_cast<uint16_t>(p.mip - p.chainLevel);
      rect.reserved = 1u;
      continue;
    }
    std::string readErr;
    const lightusd::ptx::FaceImage* decoded = pageCache.Fetch(
        reader, p.face, p.mip, options.maxDecodedFaceBytes, &readErr);
    if (!decoded) {
      if (err) *err = "Ptex face " + std::to_string(p.face) + ": " + readErr;
      return false;
    }
    if (decoded->width < p.width || decoded->height < p.height) {
      if (err) *err = "Ptex face dimensions are smaller than packed fallback";
      return false;
    }
    localStats.decodedFaces++;
    localStats.decodedBytes += decoded->data.size();
    if (decoded->dataType == lightusd::ptx::DataType::UInt8 &&
        decoded->channels == 4 && decoded->width == p.width &&
        decoded->height == p.height) {
      CopyRgba8FaceWithClampGutter(*decoded, p, options.gutter, image);
    } else {
      const light3d::Image rgba =
          ResizeFace(ConvertFace(*decoded), p.width, p.height);
      CopyWithClampGutter(rgba, p, options.gutter, image);
    }
    DrawPtexFaceRectCPU& rect =
        (*faceRects)[size_t(p.face) * mip_levels + p.chainLevel];
    rect.x = p.x;
    rect.y = p.y;
    rect.width = p.width;
    rect.height = p.height;
    rect.mipLevel = static_cast<uint16_t>(p.mip);
    rect.baseMip = static_cast<uint16_t>(p.mip - p.chainLevel);
    rect.reserved = p.virtualDownsample ? 1u : 0u;
  }
  // Replace the clamp-only first gutter with adjacent-face samples. This is
  // what lets the descriptor shader blend across a Ptex face edge.
  FillCrossFaceGutters(reader, placements, options, image);

  // Store the rectangle table in alpha-only texels after the color atlas.
  // GL/Vulkan sRGB formats transform RGB reads but never alpha, so this compact
  // table can be decoded by every backend from the same sampled image.
  for (uint32_t face = 0; face < info.faces; ++face) {
    for (uint32_t level = 0; level < mip_levels; ++level) {
      const DrawPtexFaceRectCPU& rect =
          (*faceRects)[size_t(face) * mip_levels + level];
      uint8_t texels[10u * 4u];
      EncodePtexFaceRectTexels(rect, texels);
      for (uint32_t byte = 0; byte < 10; ++byte) {
        const uint32_t linear = rectTexelOffset +
                                (face * mip_levels + level) * 10u + byte;
        image->data[size_t(linear) * 4u + 3u] = texels[byte * 4u + 3u];
      }
    }
  }
  localStats.rectTexelOffset = rectTexelOffset;
  localStats.mipLevels = mip_levels;
  localStats.physicalCacheOffsetY = physicalCacheOffsetY;
  localStats.physicalCacheSlotEdge = physicalCacheSlotEdge;
  localStats.physicalCacheSlots = physicalCacheSlots;
  localStats.atlasBytes = image->data.size();
  localStats.pageCache = pageCache.stats();
  if (stats) *stats = localStats;
  return true;
}

}  // namespace lusdview
