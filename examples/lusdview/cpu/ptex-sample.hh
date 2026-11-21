// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU Ptex per-face mip sampling with a streamed page cache.
//
// The GPU preview keeps a coarse per-face atlas and streams a bounded number of
// full-resolution face pages. The CPU preview previously sampled only the
// atlas base mip. This sampler decodes one (face, mip) page on demand, keeps an
// LRU-bounded resident set, and exposes the raw page so oracles can compare it
// byte-for-byte with an independent decoder (OpenPtex).
//
// All sampling math (adjacency, footprint, bilinear taps) lives in the C11
// core src/ptex/tptex_sample.{h,c}; this class only owns the page cache,
// its mutex and the statistics.
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
#include "external/textools/ptex/include/tptex_sample.h"

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
    Adopt(std::move(reader));
    return true;
  }

  bool OpenMemory(const uint8_t* data, size_t size, std::string* err) {
    auto reader = std::make_unique<lightusd::ptx::Reader>();
    if (!lightusd::ptx::Reader::OpenMemory(data, size, reader.get(), err))
      return false;
    Adopt(std::move(reader));
    return true;
  }

 private:
  // Reopening drops pages cached from the previous file.
  void Adopt(std::unique_ptr<lightusd::ptx::Reader> reader) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    byKey_.clear();
    transient_ = lightusd::ptx::FaceImage();
    stats_.residentBytes = 0;
    reader_ = std::move(reader);
    cFaces_.clear();
    cFaces_.reserve(reader_->info().faceInfo.size());
    for (const auto& f : reader_->info().faceInfo) cFaces_.push_back(ToC(f));
  }

 public:

  bool valid() const { return reader_ != nullptr; }
  // Underlying container reader (for atlas construction); null until opened.
  const lightusd::ptx::Reader* reader() const { return reader_.get(); }
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
    const tptex_face_info fi = ToC(pi.faceInfo[face]);
    return tptex_choose_mip(&fi, pi.levels, uv_per_pixel);
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
    const tptex_face_info fi = ToC(pi.faceInfo[face]);
    tptex_footprint fp;
    tptex_compute_uv_footprint(&fi, pi.levels, gu, gv, world_footprint, &fp);
    out->major = fp.major;
    out->minor = fp.minor;
    out->dir_u = fp.dir_u;
    out->dir_v = fp.dir_v;
    out->mip = fp.mip;
    out->taps = fp.taps;
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
    const tptex_sample_source src = Source();
    if (!tptex_sample_rgb(&src, face, mip, u, v, srgb ? 1 : 0, out_rgb,
                          &crossFaceTaps_))
      return false;
    CountSamples(mip, 1);
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
    const tptex_sample_source src = Source();
    if (!tptex_sample_channel(&src, face, mip, u, v, channel, out,
                              &crossFaceTaps_))
      return false;
    CountSamples(mip, 1);
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
  static tptex_face_info ToC(const lightusd::ptx::FaceInfo& f) {
    tptex_face_info c;
    c.res.ulog2 = static_cast<int8_t>(f.ures);
    c.res.vlog2 = static_cast<int8_t>(f.vres);
    c.adjedges = f.adjEdges;
    c.flags = f.flags;
    for (int e = 0; e < 4; ++e)
      c.adjfaces[e] = f.adjFaces[e] == 0xffffffffu ? -1 : static_cast<int32_t>(f.adjFaces[e]);
    return c;
  }

  static tptex_footprint ToC(const Footprint& fp) {
    tptex_footprint c;
    c.major = fp.major;
    c.minor = fp.minor;
    c.dir_u = fp.dir_u;
    c.dir_v = fp.dir_v;
    c.mip = fp.mip;
    c.taps = fp.taps;
    return c;
  }

  // Page provider for tptex_sample: a view of the cached page, valid until
  // the next fetch (the C core samples each page before fetching another).
  static int FetchPage(void* user, uint32_t face, uint32_t mip,
                       tptex_image* page) {
    const auto* self = static_cast<const CpuPtexSampler*>(user);
    const lightusd::ptx::FaceImage* img = self->FetchLocked(face, mip);
    if (!img) return 0;
    page->width = img->width;
    page->height = img->height;
    page->channels = img->channels;
    page->type = static_cast<tptex_data_type>(img->dataType);
    page->data = const_cast<uint8_t*>(img->data.data());
    page->size = img->data.size();
    return 1;
  }

  tptex_sample_source Source() const {
    tptex_sample_source src;
    src.faces = cFaces_.empty() ? nullptr : cFaces_.data();
    src.nfaces = static_cast<uint32_t>(cFaces_.size());
    src.levels = info().levels;
    src.fetch = &CpuPtexSampler::FetchPage;
    src.user = const_cast<CpuPtexSampler*>(this);
    return src;
  }

  void CountSamples(uint32_t mip, uint32_t n) const {
    stats_.samples += n;
    stats_.mipSamples[std::min<uint32_t>(mip, 15u)] += n;
  }

  bool SampleFootprintLocked(uint32_t face, const Footprint& fp, float u,
                             float v, bool srgb, float out_rgb[3]) const {
    const tptex_sample_source src = Source();
    const tptex_footprint cfp = ToC(fp);
    if (fp.anisotropic()) {
      ++stats_.anisotropicSamples;
      stats_.anisotropicTaps += tptex_footprint_taps(&cfp);
    }
    uint32_t n = 0;
    const int ok = tptex_sample_footprint_rgb(&src, face, &cfp, u, v,
                                              srgb ? 1 : 0, out_rgb,
                                              &crossFaceTaps_, &n);
    CountSamples(fp.mip, n);
    return ok != 0;
  }

  bool SampleChannelFootprintLocked(uint32_t face, const Footprint& fp,
                                    float u, float v, int channel,
                                    float* out) const {
    const tptex_sample_source src = Source();
    const tptex_footprint cfp = ToC(fp);
    if (fp.anisotropic()) {
      ++stats_.anisotropicSamples;
      stats_.anisotropicTaps += tptex_footprint_taps(&cfp);
    }
    uint32_t n = 0;
    const int ok = tptex_sample_footprint_channel(&src, face, &cfp, u, v,
                                                  channel, out,
                                                  &crossFaceTaps_, &n);
    CountSamples(fp.mip, n);
    return ok != 0;
  }

  struct Entry {
    uint64_t key{0};
    lightusd::ptx::FaceImage image;
  };

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
  std::vector<tptex_face_info> cFaces_;
  mutable std::mutex mutex_;
};

}  // namespace lusdview
