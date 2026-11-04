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
    uint64_t anisotropicSamples{0};
    uint64_t anisotropicTaps{0};
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

  // Anisotropic UV footprint for one pixel: the image of the isotropic
  // world-space pixel disk under the hit triangle's world->UV gradients
  // (`gu` = du/dP, `gv` = dv/dP). The singular values of that 2x3 matrix give
  // the semi-major/semi-minor UV extents and the major direction, so a
  // stretched mapping is filtered along its long axis instead of being
  // over-blurred by the maximum gradient.
  struct Footprint {
    float major{0.0f};    // largest UV extent per pixel
    float minor{0.0f};    // smallest UV extent per pixel
    float dir_u{1.0f};    // unit major direction in (u,v)
    float dir_v{0.0f};
    uint32_t mip{0};      // selected from the minor axis
    uint32_t taps{1};     // clamp(ceil(major/minor), 1, 8)
    bool anisotropic() const { return taps > 1u; }
  };

  static void ComputeUvFootprint(uint32_t face,
                                 const lightusd::ptx::Info& pi,
                                 const float gu[3], const float gv[3],
                                 float world_footprint, Footprint* out) {
    if (!out) return;
    *out = Footprint{};
    if (face >= pi.faces || pi.levels == 0) return;
    const float a = gu[0] * gu[0] + gu[1] * gu[1] + gu[2] * gu[2];
    const float b = gu[0] * gv[0] + gu[1] * gv[1] + gu[2] * gv[2];
    const float c = gv[0] * gv[0] + gv[1] * gv[1] + gv[2] * gv[2];
    const float trace = a + c;
    const float disc = std::sqrt(std::max((a - c) * (a - c) + 4.0f * b * b, 0.0f));
    float lmax = 0.5f * (trace + disc);
    float lmin = 0.5f * (trace - disc);
    if (!(lmax >= 0.0f)) lmax = 0.0f;
    if (!(lmin >= 0.0f)) lmin = 0.0f;
    const float w = std::max(world_footprint, 0.0f);
    out->major = w * std::sqrt(lmax);
    out->minor = w * std::sqrt(lmin);
    if (std::fabs(b) > 1.0e-12f) {
      out->dir_u = b;
      out->dir_v = lmax - a;
    } else if (a >= c) {
      out->dir_u = 1.0f;
      out->dir_v = 0.0f;
    } else {
      out->dir_u = 0.0f;
      out->dir_v = 1.0f;
    }
    const float dir_len = std::sqrt(out->dir_u * out->dir_u +
                                    out->dir_v * out->dir_v);
    if (dir_len > 1.0e-12f) {
      out->dir_u /= dir_len;
      out->dir_v /= dir_len;
    } else {
      out->dir_u = 1.0f;
      out->dir_v = 0.0f;
    }
    const uint32_t edge = std::max(pi.faceInfo[face].width(),
                                   pi.faceInfo[face].height());
    const float texels = std::max(1.0f, out->minor * float(edge));
    float level = std::floor(std::log2(texels));
    if (!(level >= 0.0f)) level = 0.0f;
    out->mip = std::min(pi.levels - 1u, static_cast<uint32_t>(level));
    const float ratio = out->minor > 1.0e-9f ? out->major / out->minor : 1.0f;
    float taps = std::ceil(ratio);
    if (!(taps >= 1.0f)) taps = 1.0f;
    out->taps = static_cast<uint32_t>(std::min(8.0f, taps));
  }

  Footprint ComputeFootprint(uint32_t face, const float gu[3],
                             const float gv[3],
                             float world_footprint) const {
    Footprint fp;
    ComputeUvFootprint(face, info(), gu, gv, world_footprint, &fp);
    return fp;
  }

  // Bilinear RGB sample of `face` at `mip`. (u,v) are intrinsic Ptex
  // coordinates with v=0 at the face bottom, matching the raw face layout.
  bool Sample(uint32_t face, uint32_t mip, float u, float v, bool srgb,
              float out_rgb[3]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SampleLocked(face, mip, u, v, srgb, out_rgb);
  }

  // Anisotropic footprint sample: multiple bilinear taps at the minor-axis mip
  // spaced one texel apart along the major direction, tent-weighted.
  bool SampleFootprint(uint32_t face, const Footprint& fp, float u, float v,
                       bool srgb, float out_rgb[3]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SampleFootprintLocked(face, fp, u, v, srgb, out_rgb);
  }

 private:
  bool SampleLocked(uint32_t face, uint32_t mip, float u, float v, bool srgb,
                    float out_rgb[3]) const {
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

 public:
  // Single-channel sample for scalar lanes (no sRGB transfer).
  bool SampleChannel(uint32_t face, uint32_t mip, float u, float v,
                     int channel, float* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SampleChannelLocked(face, mip, u, v, channel, out);
  }

  bool SampleChannelFootprint(uint32_t face, const Footprint& fp, float u,
                              float v, int channel, float* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SampleChannelFootprintLocked(face, fp, u, v, channel, out);
  }

 private:
  bool SampleChannelLocked(uint32_t face, uint32_t mip, float u, float v,
                           int channel, float* out) const {
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

 public:
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

  bool SampleFootprintLocked(uint32_t face, const Footprint& fp, float u,
                             float v, bool srgb, float out_rgb[3]) const {
    if (!fp.anisotropic()) {
      return SampleLocked(face, fp.mip, u, v, srgb, out_rgb);
    }
    const uint32_t n = std::min<uint32_t>(8u, std::max<uint32_t>(1u, fp.taps));
    ++stats_.anisotropicSamples;
    stats_.anisotropicTaps += n;
    const float step = std::max(fp.minor, 1.0e-6f);
    const float center = 0.5f * float(n - 1u);
    float sum[3] = {0.0f, 0.0f, 0.0f};
    float weight_sum = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
      const float offset = (float(i) - center) * step;
      const float weight = float(n) * 0.5f -
                           std::fabs(float(i) - center);
      float tap[3] = {0.0f, 0.0f, 0.0f};
      if (!SampleLocked(face, fp.mip, u + offset * fp.dir_u,
                        v + offset * fp.dir_v, false, tap))
        continue;
      sum[0] += tap[0] * weight;
      sum[1] += tap[1] * weight;
      sum[2] += tap[2] * weight;
      weight_sum += weight;
    }
    const float inv = weight_sum > 0.0f ? 1.0f / weight_sum : 0.0f;
    for (int c = 0; c < 3; ++c) {
      float value = sum[c] * inv;
      if (srgb) value = SrgbToLinear(value);
      out_rgb[c] = value;
    }
    return true;
  }

  bool SampleChannelFootprintLocked(uint32_t face, const Footprint& fp,
                                    float u, float v, int channel,
                                    float* out) const {
    if (!fp.anisotropic()) {
      return SampleChannelLocked(face, fp.mip, u, v, channel, out);
    }
    const uint32_t n = std::min<uint32_t>(8u, std::max<uint32_t>(1u, fp.taps));
    ++stats_.anisotropicSamples;
    stats_.anisotropicTaps += n;
    const float step = std::max(fp.minor, 1.0e-6f);
    const float center = 0.5f * float(n - 1u);
    float sum = 0.0f;
    float weight_sum = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
      const float offset = (float(i) - center) * step;
      const float weight = float(n) * 0.5f -
                           std::fabs(float(i) - center);
      float tap = 0.0f;
      SampleChannelLocked(face, fp.mip, u + offset * fp.dir_u,
                          v + offset * fp.dir_v, channel, &tap);
      sum += tap * weight;
      weight_sum += weight;
    }
    *out = weight_sum > 0.0f ? sum / weight_sum : 0.0f;
    return true;
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
