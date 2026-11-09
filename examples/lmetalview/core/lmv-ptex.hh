// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - coarse Ptex preview atlas.
//
// A preview only needs a bounded, face-local colour map. This builds a simple
// grid atlas of the first `max_faces` faces at a capped resolution (no mips, no
// page streaming), plus the normalized rectangle of every face so the Metal
// shader can sample it with the face index + face-local (s,t).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lmv {

struct PtexRect {
  // Normalized atlas coordinates of the inner texel area (half-texel inset so
  // clamp-to-edge linear sampling stays inside the face).
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  // Zero-size rects mark faces that were not decoded (budget / malformed).
  bool valid() const { return w > 0.0f && h > 0.0f; }
};

struct PtexAtlas {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
  std::vector<PtexRect> rects;  // one per ptx face
  uint32_t face_count = 0;

  bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<size_t>(width) * height * 4;
  }
};

struct PtexAtlasOptions {
  uint32_t max_face_edge = 64;
  uint32_t max_atlas_edge = 4096;
  uint32_t max_faces = 4096;
  uint64_t max_decoded_bytes = 256ull << 20;
};

// Decode `path` (or `data`) and build an RGBA8 preview atlas.
bool BuildPtexAtlas(const std::string& path, const uint8_t* data, size_t size,
                    const PtexAtlasOptions& options, PtexAtlas* out,
                    std::string* error);

}  // namespace lmv
