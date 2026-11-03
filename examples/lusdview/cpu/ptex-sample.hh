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
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, int(w) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, int(h) - 1);
    const int x1 = std::min<int>(x0 + 1, int(w) - 1);
    const int y1 = std::min<int>(y0 + 1, int(h) - 1);
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
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, int(w) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, int(h) - 1);
    const int x1 = std::min<int>(x0 + 1, int(w) - 1);
    const int y1 = std::min<int>(y0 + 1, int(h) - 1);
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

 private:
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
  mutable std::mutex mutex_;
};

}  // namespace lusdview
