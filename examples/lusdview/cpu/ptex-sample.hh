// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU Ptex per-face mip sampling with a streamed page cache.
//
// The GPU preview keeps a coarse per-face atlas and streams a bounded number of
// full-resolution face pages. The CPU preview previously sampled only the
// atlas base mip. This sampler decodes one (face, mip) page on demand, keeps an
// LRU-bounded resident set, and exposes the raw page so oracles can compare it
// byte-for-byte with an independent decoder (OpenPtex).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptx-loader.hh"

namespace lusdview {

class CpuPtexSampler {
 public:
  struct Stats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t decodedBytes{0};
    uint64_t residentBytes{0};
    uint64_t peakResidentBytes{0};
    uint64_t transientPages{0};
    uint64_t samples{0};
    uint64_t mipSamples[16]{};
  };

  explicit CpuPtexSampler(size_t maxResidentBytes = 16ull * 1024ull * 1024ull)
      : maxResidentBytes_(maxResidentBytes) {}

  bool OpenFile(const std::string& path, std::string* err) {
    auto reader = std::make_unique<lightusd::ptx::Reader>();
    if (!lightusd::ptx::Reader::OpenFile(path, reader.get(), err)) return false;
    reader_ = std::move(reader);
    return true;
  }

  bool OpenMemory(const uint8_t* data, size_t size, std::string* err) {
    auto reader = std::make_unique<lightusd::ptx::Reader>();
    if (!lightusd::ptx::Reader::OpenMemory(data, size, reader.get(), err))
      return false;
    reader_ = std::move(reader);
    return true;
  }

  bool valid() const { return reader_ != nullptr; }
  const lightusd::ptx::Info& info() const {
    static const lightusd::ptx::Info empty;
    return reader_ ? reader_->info() : empty;
  }

  // Per-face mip from the intrinsic UV footprint of one pixel. `uv_per_pixel`
  // is |du/dpixel| or |dv/dpixel| for the hit, so faces of different base
  // resolutions select their own mip.
  uint32_t ChooseMip(uint32_t face, float uv_per_pixel) const {
    const lightusd::ptx::Info& pi = info();
    if (face >= pi.faces || pi.levels == 0) return 0;
    const uint32_t edge =
        std::max(pi.faceInfo[face].width(), pi.faceInfo[face].height());
    const float texels = std::max(1.0f, uv_per_pixel * float(edge));
    float level = std::floor(std::log2(texels));
    if (!(level >= 0.0f)) level = 0.0f;  // also catches NaN
    const uint32_t max_level = pi.levels - 1u;
    return std::min(max_level, static_cast<uint32_t>(level));
  }

  // Bilinear RGB sample of `face` at `mip`. (u,v) are intrinsic Ptex
  // coordinates with v=0 at the face bottom, matching the raw face layout.
  bool Sample(uint32_t face, uint32_t mip, float u, float v, bool srgb,
              float out_rgb[3]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const lightusd::ptx::FaceImage* page = FetchLocked(face, mip);
    if (!page || page->dataType != lightusd::ptx::DataType::UInt8) return false;
    const uint32_t w = page->width, h = page->height;
    if (!w || !h || page->channels == 0) return false;
    const float su = std::clamp(u, 0.0f, 1.0f);
    const float sv = std::clamp(v, 0.0f, 1.0f);
    const float fx = su * float(w) - 0.5f;
    const float fy = sv * float(h) - 0.5f;
    const int ix0 = static_cast<int>(std::floor(fx));
    const int iy0 = static_cast<int>(std::floor(fy));
    const int ix1 = ix0 + 1;
    const int iy1 = iy0 + 1;
    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
    const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    // A filter tap outside [0,1] follows the Ptex adjacency into the
    // neighbouring face instead of clamping at the seam.
    auto tap_u = [&](int ix) { return (float(ix) + 0.5f) / float(w); };
    auto tap_v = [&](int iy) { return (float(iy) + 0.5f) / float(h); };
    float c00[3], c10[3], c01[3], c11[3];
    TapRgb(face, mip, tap_u(ix0), tap_v(iy0), c00);
    TapRgb(face, mip, tap_u(ix1), tap_v(iy0), c10);
    TapRgb(face, mip, tap_u(ix0), tap_v(iy1), c01);
    TapRgb(face, mip, tap_u(ix1), tap_v(iy1), c11);
    for (int c = 0; c < 3; ++c) {
      const float top = c00[c] * (1.0f - tx) + c10[c] * tx;
      const float bot = c01[c] * (1.0f - tx) + c11[c] * tx;
      float value = top * (1.0f - ty) + bot * ty;
      if (srgb) value = SrgbToLinear(value);
      out_rgb[c] = value;
    }
    ++stats_.samples;
    ++stats_.mipSamples[std::min<uint32_t>(mip, 15u)];
    return true;
  }

  // Single-channel sample for scalar lanes (no sRGB transfer).
  bool SampleChannel(uint32_t face, uint32_t mip, float u, float v,
                     int channel, float* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const lightusd::ptx::FaceImage* page = FetchLocked(face, mip);
    if (!page || page->dataType != lightusd::ptx::DataType::UInt8) return false;
    const uint32_t w = page->width, h = page->height;
    if (!w || !h || page->channels == 0) return false;
    const float su = std::clamp(u, 0.0f, 1.0f);
    const float sv = std::clamp(v, 0.0f, 1.0f);
    const float fx = su * float(w) - 0.5f;
    const float fy = sv * float(h) - 0.5f;
    const int ix0 = static_cast<int>(std::floor(fx));
    const int iy0 = static_cast<int>(std::floor(fy));
    const int ix1 = ix0 + 1;
    const int iy1 = iy0 + 1;
    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
    const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    const int c = std::clamp(channel, 0, int(page->channels) - 1);
    auto tap_u = [&](int ix) { return (float(ix) + 0.5f) / float(w); };
    auto tap_v = [&](int iy) { return (float(iy) + 0.5f) / float(h); };
    float c00 = 0.0f, c10 = 0.0f, c01 = 0.0f, c11 = 0.0f;
    TapChannel(face, mip, tap_u(ix0), tap_v(iy0), c, &c00);
    TapChannel(face, mip, tap_u(ix1), tap_v(iy0), c, &c10);
    TapChannel(face, mip, tap_u(ix0), tap_v(iy1), c, &c01);
    TapChannel(face, mip, tap_u(ix1), tap_v(iy1), c, &c11);
    const float top = c00 * (1.0f - tx) + c10 * tx;
    const float bot = c01 * (1.0f - tx) + c11 * tx;
    *out = top * (1.0f - ty) + bot * ty;
    ++stats_.samples;
    ++stats_.mipSamples[std::min<uint32_t>(mip, 15u)];
    return true;
  }

  // Raw decoded page for byte-exact verification against an independent
  // decoder. The pointer is valid while the cache entry is resident; callers
  // that need it across other fetches must copy it.
  const lightusd::ptx::FaceImage* RawFace(uint32_t face, uint32_t mip,
                                          const uint8_t** raw,
                                          std::string* err) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const lightusd::ptx::FaceImage* page = FetchLocked(face, mip, err);
    if (!page) return nullptr;
    if (raw) *raw = page->data.empty() ? nullptr : page->data.data();
    return page;
  }

  const Stats& stats() const { return stats_; }

  // Number of taps remapped into an adjacent face (diagnostics/oracle).
  uint64_t crossFaceTaps() const { return crossFaceTaps_; }

 private:
  // Map a UV point that left the face across one edge using the Ptex adjacency
  // (same rotation convention as PtexSeparableFilter::applyAcrossEdge). Returns
  // false when there is no neighbour or the face uses subface adjacency, which
  // this preview does not model; the caller then clamps inside the face.
  bool CrossEdge(uint32_t* face, float* u, float* v) const {
    const lightusd::ptx::Info& pi = info();
    if (!face || *face >= pi.faces) return false;
    const lightusd::ptx::FaceInfo& fi = pi.faceInfo[*face];
    if ((fi.flags & 8u) != 0) return false;  // subface adjacency
    int edge = -1;
    if (*u < 0.0f) edge = 3;        // left
    else if (*u > 1.0f) edge = 1;   // right
    else if (*v < 0.0f) edge = 0;   // bottom
    else if (*v > 1.0f) edge = 2;   // top
    else return false;
    const uint32_t adjacent = fi.adjFaces[edge];
    if (adjacent == 0xffffffffu || adjacent >= pi.faces) return false;
    const int adj_edge = (fi.adjEdges >> (2 * edge)) & 3;
    float uu = *u;
    float vv = *v;
    if (edge == 3) uu += 1.0f;
    else if (edge == 1) uu -= 1.0f;
    else if (edge == 0) vv += 1.0f;
    else vv -= 1.0f;
    const int rot = (edge - adj_edge + 2) & 3;
    float ru = uu;
    float rv = vv;
    if (rot == 1) {
      ru = vv;
      rv = 1.0f - uu;
    } else if (rot == 2) {
      ru = 1.0f - uu;
      rv = 1.0f - vv;
    } else if (rot == 3) {
      ru = 1.0f - vv;
      rv = uu;
    }
    *face = adjacent;
    *u = std::clamp(ru, 0.0f, 1.0f);
    *v = std::clamp(rv, 0.0f, 1.0f);
    return true;
  }

  const lightusd::ptx::FaceImage* PageAt(uint32_t face, uint32_t mip) const {
    const uint32_t levels = info().levels;
    return FetchLocked(face, levels ? std::min(mip, levels - 1u) : 0u);
  }

  void SamplePageRgb(const lightusd::ptx::FaceImage* page, float u, float v,
                     float out[3]) const {
    const uint32_t w = page->width, h = page->height;
    if (!w || !h || page->channels == 0) {
      out[0] = out[1] = out[2] = 0.0f;
      return;
    }
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
    const int x0 = std::clamp(int(std::floor(fx)), 0, int(w) - 1);
    const int y0 = std::clamp(int(std::floor(fy)), 0, int(h) - 1);
    const int x1 = std::min(x0 + 1, int(w) - 1);
    const int y1 = std::min(y0 + 1, int(h) - 1);
    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
    const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    float c00[3], c10[3], c01[3], c11[3];
    TexelRgb(page, x0, y0, c00);
    TexelRgb(page, x1, y0, c10);
    TexelRgb(page, x0, y1, c01);
    TexelRgb(page, x1, y1, c11);
    for (int c = 0; c < 3; ++c) {
      const float top = c00[c] * (1.0f - tx) + c10[c] * tx;
      const float bot = c01[c] * (1.0f - tx) + c11[c] * tx;
      out[c] = top * (1.0f - ty) + bot * ty;
    }
  }

  void TapRgb(uint32_t face, uint32_t mip, float u, float v,
              float out[3]) const {
    bool crossed = false;
    for (int hop = 0; hop < 2; ++hop) {
      if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) break;
      if (!CrossEdge(&face, &u, &v)) break;
      crossed = true;
    }
    if (crossed) ++crossFaceTaps_;
    const lightusd::ptx::FaceImage* page = PageAt(face, mip);
    if (!page) {
      out[0] = out[1] = out[2] = 0.0f;
      return;
    }
    SamplePageRgb(page, u, v, out);
  }

  void SamplePageChannel(const lightusd::ptx::FaceImage* page, float u,
                         float v, int channel, float* out) const {
    const uint32_t w = page->width, h = page->height;
    if (!w || !h || page->channels == 0) {
      *out = 0.0f;
      return;
    }
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
    const int x0 = std::clamp(int(std::floor(fx)), 0, int(w) - 1);
    const int y0 = std::clamp(int(std::floor(fy)), 0, int(h) - 1);
    const int x1 = std::min(x0 + 1, int(w) - 1);
    const int y1 = std::min(y0 + 1, int(h) - 1);
    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
    const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    const int c = std::clamp(channel, 0, int(page->channels) - 1);
    auto at = [&](int x, int y) {
      return float(page->data[(size_t(y) * w + size_t(x)) *
                                  page->channels + size_t(c)]) /
             255.0f;
    };
    const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
    const float bot = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
    *out = top * (1.0f - ty) + bot * ty;
  }

  void TapChannel(uint32_t face, uint32_t mip, float u, float v, int channel,
                  float* out) const {
    bool crossed = false;
    for (int hop = 0; hop < 2; ++hop) {
      if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) break;
      if (!CrossEdge(&face, &u, &v)) break;
      crossed = true;
    }
    if (crossed) ++crossFaceTaps_;
    const lightusd::ptx::FaceImage* page = PageAt(face, mip);
    if (!page) {
      *out = 0.0f;
      return;
    }
    SamplePageChannel(page, u, v, channel, out);
  }

  struct Entry {
    uint64_t key{0};
    lightusd::ptx::FaceImage image;
  };

  static float SrgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
  }

  static void TexelRgb(const lightusd::ptx::FaceImage* page, int x, int y,
                       float out[3]) {
    const size_t index = (size_t(y) * page->width + size_t(x)) *
                         page->channels;
    const uint8_t* px = page->data.data() + index;
    const float r = float(px[0]) / 255.0f;
    const float g = page->channels > 1 ? float(px[1]) / 255.0f : r;
    const float b = page->channels > 2 ? float(px[2]) / 255.0f : r;
    out[0] = r;
    out[1] = g;
    out[2] = b;
  }

  const lightusd::ptx::FaceImage* FetchLocked(uint32_t face, uint32_t mip,
                                              std::string* err = nullptr) const {
    if (!reader_ || face >= reader_->info().faces) return nullptr;
    const uint64_t key = (uint64_t(face) << 32) | uint64_t(mip);
    auto it = byKey_.find(key);
    if (it != byKey_.end()) {
      entries_.splice(entries_.begin(), entries_, it->second);
      ++stats_.hits;
      return &it->second->image;
    }
    ++stats_.misses;
    lightusd::ptx::FaceImage decoded;
    if (!reader_->ReadFace(face, mip, maxDecodedFaceBytes_, &decoded, err))
      return nullptr;
    const size_t bytes = decoded.data.size();
    stats_.decodedBytes += bytes;
    if (bytes > maxResidentBytes_ || maxResidentBytes_ == 0) {
      // A single page larger than the budget is returned transiently and is
      // never retained, so residentBytes stays bounded.
      transient_ = std::move(decoded);
      ++stats_.transientPages;
      const uint64_t peak =
          std::max<uint64_t>(stats_.peakResidentBytes, stats_.residentBytes);
      stats_.peakResidentBytes = peak;
      return &transient_;
    }
    entries_.push_front(Entry{key, std::move(decoded)});
    byKey_[key] = entries_.begin();
    stats_.residentBytes += bytes;
    while (stats_.residentBytes > maxResidentBytes_ && entries_.size() > 1) {
      Entry& victim = entries_.back();
      stats_.residentBytes -= victim.image.data.size();
      byKey_.erase(victim.key);
      entries_.pop_back();
      ++stats_.evictions;
    }
    const uint64_t peak =
        std::max<uint64_t>(stats_.peakResidentBytes, stats_.residentBytes);
    stats_.peakResidentBytes = peak;
    return &entries_.front().image;
  }

  size_t maxResidentBytes_{16ull * 1024ull * 1024ull};
  size_t maxDecodedFaceBytes_{64ull * 1024ull * 1024ull};
  std::unique_ptr<lightusd::ptx::Reader> reader_;
  mutable std::list<Entry> entries_;
  mutable std::unordered_map<uint64_t, std::list<Entry>::iterator> byKey_;
  mutable lightusd::ptx::FaceImage transient_;
  mutable Stats stats_;
  mutable uint64_t crossFaceTaps_{0};
  mutable std::mutex mutex_;
};

}  // namespace lusdview
