// SPDX-License-Identifier: Apache-2.0
// lusdview - bounded coarse Ptex atlas construction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu_scene.hh"
#include "ptx-loader.hh"

namespace lusdview {

struct PtexAtlasOptions {
  uint32_t maxFaceEdge{512};
  uint32_t maxAtlasEdge{16384};
  uint32_t gutter{2};
  size_t maxDecodedFaceBytes{64ull * 1024ull * 1024ull};
  // Decoded face pages retained while constructing an atlas. Zero disables
  // retention while preserving the same bounded one-page-at-a-time behavior.
  size_t maxDecodedCacheBytes{0};
  size_t maxAtlasBytes{0};  // 0 = derive only from maxAtlasEdge.
  // Optional fixed physical page cache appended after the always-resident
  // fallback faces. The total image, including this cache and the face table,
  // still obeys maxAtlasBytes/maxAtlasEdge.
  size_t maxPhysicalCacheBytes{0};
  // If nonzero, only this many face fallbacks are built at startup. Remaining
  // faces receive one-pixel reserved entries and are filled by page streaming.
  uint32_t initialFaceLimit{0};
  // Test/diagnostic override. Normal builds reserve slots only when the atlas
  // budget forced at least one face below its requested mip.
  bool forcePhysicalCache{false};
  // Number of absolute mip levels packed per face, starting at the static fit
  // mip. 1 preserves the historical single-level atlas. The descriptor path
  // uses >1 for per-face minification.
  uint32_t mipLevels{1};
};

struct PtexPageCacheStats {
  uint64_t hits{0};
  uint64_t misses{0};
  uint64_t evictions{0};
  uint64_t decodedBytes{0};
  uint64_t residentBytes{0};
  uint64_t peakResidentBytes{0};
};

struct PtexPhysicalPageAssignment {
  uint32_t slot{0};
  uint32_t evictedFace{~uint32_t{0}};
  bool hit{false};
};

// Deterministic LRU ownership for a fixed number of GPU atlas slots. Pixel
// upload and face-table updates stay backend-specific; this class guarantees
// that a face has at most one slot and identifies the old face that must be
// redirected to fallback before a slot is overwritten.
class PtexPhysicalPageCache {
 public:
  explicit PtexPhysicalPageCache(uint32_t slotCount);

  bool Request(uint32_t face, PtexPhysicalPageAssignment* assignment);
  void Clear();

  uint32_t slotCount() const { return static_cast<uint32_t>(slots_.size()); }
  uint32_t residentCount() const { return residentCount_; }
  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }
  uint64_t evictions() const { return evictions_; }

 private:
  struct Slot {
    uint32_t face{~uint32_t{0}};
    uint64_t stamp{0};
  };
  std::vector<Slot> slots_;
  std::unordered_map<uint32_t, uint32_t> byFace_;
  uint32_t residentCount_{0};
  uint64_t clock_{0};
  uint64_t hits_{0};
  uint64_t misses_{0};
  uint64_t evictions_{0};
};

// Byte-bounded decoded-face cache. One cache belongs to one Reader/source;
// entries are keyed by (face,mip), promoted on access, and evicted least-
// recently-used. Oversized pages are returned through a transient slot and are
// never retained, so residentBytes never exceeds the configured limit.
class PtexFacePageCache {
 public:
  explicit PtexFacePageCache(size_t maxBytes) : maxBytes_(maxBytes) {}

  const lightusd::ptx::FaceImage* Fetch(
      const lightusd::ptx::Reader& reader, uint32_t face, uint32_t mip,
      size_t maxDecodedFaceBytes, std::string* err);
  void Clear();

  const PtexPageCacheStats& stats() const { return stats_; }
  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    uint64_t key{0};
    lightusd::ptx::FaceImage image;
  };
  using List = std::list<Entry>;

  size_t maxBytes_{0};
  List entries_;
  std::unordered_map<uint64_t, List::iterator> byKey_;
  lightusd::ptx::FaceImage transient_;
  PtexPageCacheStats stats_;
};

struct PtexAtlasBuildStats {
  uint64_t decodedFaces{0};
  uint64_t decodedBytes{0};
  uint64_t atlasBytes{0};
  uint32_t downsampledFaces{0};
  uint32_t rectTexelOffset{0};
  uint32_t physicalCacheOffsetY{0};
  uint32_t physicalCacheSlotEdge{0};
  uint32_t physicalCacheSlots{0};
  uint32_t mipLevels{1};
  PtexPageCacheStats pageCache;
};

// Builds an RGBA8 atlas without stretching rectangular faces. Face rectangles
// are retained in `faceRects` and include only the inner texels; `image`
// includes the surrounding gutters. Ptex's bottom-left data convention is
// converted to lusdview's top-row-first image convention during the copy.
bool BuildPtexAtlas(const lightusd::ptx::Reader& reader,
                    const PtexAtlasOptions& options, bool srgb,
                    light3d::Image* image,
                    std::vector<DrawPtexFaceRectCPU>* faceRects,
                    PtexAtlasBuildStats* stats, std::string* err);

// Decode one face/mip into an independently uploadable RGBA8 rectangle with
// clamp gutters. The returned pixel rows use the same convention as atlas
// level zero, so the image can be passed directly to updateTextureRegion.
bool BuildPtexPage(const lightusd::ptx::Reader& reader, uint32_t face,
                   uint32_t mip, uint32_t gutter, size_t maxDecodedFaceBytes,
                   light3d::Image* page, DrawPtexFaceRectCPU* inner,
                   std::string* err);

// Encode one face-table entry as ten RGBA texels (payload in alpha), matching
// the shader's two-byte little-endian x/y/width/height/baseMip representation.
void EncodePtexFaceRectTexels(const DrawPtexFaceRectCPU& rect,
                              uint8_t texels[10u * 4u]);

}  // namespace lusdview
