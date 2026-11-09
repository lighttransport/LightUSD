// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - shared texture decoding for the next path.
//
// tydra-next's converter records texture *metadata* (RenderTexture: asset path,
// wrap, value scale/bias, channel) but never decodes pixels -- applications
// supply their own decode so the converter stays filesystem- and
// archive-agnostic. Both lusdview and lusdrender need the same thing, and had
// grown two copies of it; this is the one implementation.
//
// The size cap and the byte budget are applied AT DECODE TIME, not as a
// post-pass over an already-decoded set: on a large scene the post-pass peak
// (every texture resident at full resolution before anything shrinks) is the
// spike we are trying to avoid.
//
// Scope note: simple box/sRGB-aware resize only, via tydra::ResizeImage. Mip
// chains, block compression, and content-aware filtering deliberately live
// elsewhere.

#pragma once

#include <cstdint>
#include <atomic>
#include <list>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lightusd {
namespace next {

struct TextureBudgetState {
  explicit TextureBudgetState(uint64_t limit_bytes) : limit(limit_bytes) {}

  bool try_add(uint64_t bytes) {
    // Read the limit ONCE per attempt: it is retunable at runtime (see
    // set_limit), so re-reading it inside the CAS loop could compare against
    // two different limits within one decision.
    const uint64_t cap = limit.load(std::memory_order_relaxed);
    if (cap == 0) {  // 0 == unbounded (track residency only)
      uint64_t old = resident.load(std::memory_order_relaxed);
      for (;;) {
        if (bytes > (std::numeric_limits<uint64_t>::max)() - old) return false;
        if (resident.compare_exchange_weak(old, old + bytes,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
          return true;
        }
      }
    }
    uint64_t old = resident.load(std::memory_order_relaxed);
    for (;;) {
      if (old > cap || bytes > cap - old) return false;
      if (resident.compare_exchange_weak(old, old + bytes,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        return true;
      }
    }
  }

  void release(uint64_t bytes) {
    resident.fetch_sub(bytes, std::memory_order_relaxed);
  }

  // Retune the cap while decoding is in flight. Leases already taken stay
  // valid and simply count against the new cap. NOTE: 0 means UNBOUNDED, not
  // "nothing fits" -- callers narrowing the cap must floor it above zero.
  void set_limit(uint64_t bytes) {
    limit.store(bytes, std::memory_order_relaxed);
  }
  uint64_t get_limit() const { return limit.load(std::memory_order_relaxed); }

  std::atomic<uint64_t> limit{0};
  std::atomic<uint64_t> resident{0};
};

struct TextureBudgetLease {
  TextureBudgetLease(std::shared_ptr<TextureBudgetState> budget_state,
                     uint64_t resident_bytes)
      : state(std::move(budget_state)), bytes(resident_bytes) {}

  std::shared_ptr<TextureBudgetState> state;
  uint64_t bytes = 0;
  ~TextureBudgetLease() {
    if (state && bytes) state->release(bytes);
  }
};
class USDZReader;
}  // namespace next

namespace tydra {
namespace next {

/// Decoded 8-bit texture pixels. `channels` is 4 unless the decoder was told to
/// keep the source channel count (see TextureDecodeOptions::force_rgba).
struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 4;
  std::vector<uint8_t> pixels;  // width * height * channels
  // HDR images are retained as linear RGB float texels for GPU BC6H upload.
  // LDR callers continue to use pixels above.
  bool hdr = false;
  std::vector<float> float_pixels;  // width * height * 3
  std::shared_ptr<::lightusd::next::TextureBudgetLease> budget_lease;

  bool empty() const { return hdr ? float_pixels.empty() : pixels.empty(); }
  size_t byte_size() const {
    return hdr ? float_pixels.size() * sizeof(float) : pixels.size();
  }
};

struct TextureDecodeOptions {
  /// Directory the source USD resolves relative asset paths against.
  std::string base_dir;
  /// Optional .usdz archive to look the asset up in before the filesystem.
  const ::lightusd::next::USDZReader* usdz = nullptr;
  /// Longest-edge cap in texels. 0 = keep the source resolution.
  uint32_t max_edge = 0;
  /// Cap on resident decoded bytes owned by this decoder and its returned
  /// images. Images may be downscaled to fit; 0 = unbounded.
  uint64_t budget_bytes = 0;
  /// Conservative encoded source-image byte ceiling. 0 = no ceiling.
  uint64_t max_source_bytes = 0;
  /// Resolution floor, in texels, for budget-driven shrinking. When non-zero
  /// the byte budget becomes SOFT: an image that still will not fit after being
  /// shrunk to this floor is admitted anyway (over-subscribing the budget)
  /// rather than failing to decode. Without it, a scene that crosses the budget
  /// mid-decode loses textures entirely -- a visible correctness regression --
  /// where a floor degrades it to "blurrier". 0 = hard budget (decode fails).
  uint32_t min_edge = 0;
  /// Expand every image to 4 channels. Consumers whose GPU texture format is
  /// RGBA8 want this; a CPU sampler that honours the source channel count
  /// should turn it off rather than pay for a synthetic alpha channel.
  bool force_rgba = true;
};

/// Resolves an asset path (filesystem or .usdz entry), decodes it to RGBA8, and
/// shrinks it to fit the configured size cap and resident byte budget.
class TextureDecoder {
 public:
  explicit TextureDecoder(TextureDecodeOptions options)
      : options_(std::move(options)),
        budget_state_(std::make_shared<::lightusd::next::TextureBudgetState>(
            options_.budget_bytes)) {}

  /// Decode `asset` into `out`. `srgb` selects the resize filter's color space
  /// (sRGB data is filtered in linear light). Returns false when the asset
  /// cannot be resolved or decoded; `out` is left untouched.
  bool Decode(const std::string& asset, bool srgb, DecodedImage* out);

  // Decode one Ptex face/mip lazily to RGBA8. This never expands the complete
  // Ptex file; the caller controls the destination budget. Non-Ptex assets
  // return false.
  bool DecodePtexFace(const std::string& asset, uint32_t face,
                      uint32_t level, bool srgb, DecodedImage* out);

  /// Read `asset`'s raw bytes, resolving it exactly like Decode does (a .usdz
  /// entry when one is set, else `base_dir`-relative on disk). For assets a
  /// consumer must interpret itself rather than have decoded to pixels -- e.g. a
  /// GPU-compressed `.ktx2`, whose blocks are uploaded as-is. Returns false when
  /// the asset cannot be resolved or read.
  bool ReadAssetBytes(const std::string& asset, std::vector<uint8_t>* out) const;

  /// Current resident decoded bytes, including retained Ptex cache entries.
  uint64_t decoded_bytes() const {
    return budget_state_->resident.load(std::memory_order_relaxed);
  }
  /// How many images were shrunk by the size cap or the byte budget.
  uint64_t downscaled_count() const { return downscaled_; }

  /// Retune the resident byte budget mid-decode. Used when a better estimate of
  /// the non-texture footprint (e.g. geometry) becomes known after decoding has
  /// already started. 0 = unbounded, so narrow with care.
  void SetBudgetBytes(uint64_t bytes) { budget_state_->set_limit(bytes); }
  uint64_t budget_bytes() const { return budget_state_->get_limit(); }

  const TextureDecodeOptions& options() const { return options_; }

 private:
  struct PtexCacheEntry {
    DecodedImage image;
    std::list<std::string>::iterator lru;
  };
  TextureDecodeOptions options_;
  std::shared_ptr<::lightusd::next::TextureBudgetState> budget_state_;
  uint64_t downscaled_ = 0;
  std::unordered_map<std::string, PtexCacheEntry> ptex_cache_;
  std::list<std::string> ptex_lru_;
  uint64_t ptex_cache_bytes_ = 0;

  /// basename -> .usdz entry indices, in entry order. Built lazily on the
  /// first usdz-backed lookup. Every tier of the entry/asset match (exact,
  /// directory-boundary suffix, basename) implies equal basenames, so
  /// restricting the scan to one bucket is exactly equivalent to walking all
  /// entries -- but turns loading T textures out of an E-entry archive from
  /// O(T*E) comparisons (with up to three string allocations per candidate)
  /// into O(T) bucket lookups.
  mutable std::unordered_map<std::string, std::vector<size_t>> usdz_by_base_;
  mutable bool usdz_index_built_ = false;
  const std::vector<size_t>* UsdzCandidates(const std::string& asset) const;
  bool AttachBudgetLease(DecodedImage* image, bool srgb);
};

/// Substitute the literal `<UDIM>` token in an asset path with a tile id.
/// tydra-next carries the token through verbatim, so UDIM expansion is the
/// application's job.
std::string ReplaceUdimToken(const std::string& asset, int udim);

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
