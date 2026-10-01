// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lightusd {
namespace udim {

enum class BakeMode { Off, Grid, Dense };
enum class CrossTilePolicy { Reject, Split };
struct Options {
  BakeMode mode{BakeMode::Off};
  size_t max_tiles{100};
  int max_atlas_size{8192};
  size_t memory_budget_bytes{size_t(512) << 20};
  CrossTilePolicy cross_tile{CrossTilePolicy::Reject};
  int dense_padding{2};
  int subdivision_level{2};
  int max_tile_size{0};  // Existing texture resize cap; 0 preserves resolution.
};
struct Cell {
  uint32_t id;
  int x;
  int y;
};
struct Layout {
  std::vector<Cell> cells;
  int cols{0}, rows{0};
  int min_u{0}, min_v{0};
  int tile_width{0}, tile_height{0}, padding{0};
  int width{0}, height{0};
  int blank_x{0}, blank_y{0};
  BakeMode mode{BakeMode::Off};
  std::array<float, 2> remap(float u, float v, uint32_t face_tile = 0) const;
};
struct Atlas {
  Layout layout;
  std::vector<uint8_t> bytes;
  std::string extension;
};
// Incremental API retains one atlas and one decoded tile. JS streaming callers
// can await each fetch without retaining the other encoded tiles in WASM.
class AtlasBuilder {
 public:
  bool begin(const std::vector<uint32_t>& ids, int width, int height,
             const Options&, bool srgb, std::string* error);
  bool add(uint32_t id, const uint8_t* bytes, size_t size,
           const std::string& uri, std::string* error);
  bool blank(uint32_t id, std::string* error);
  bool finish(const std::string& format, int quality, Atlas*,
              std::string* error);
  const Layout& layout() const { return layout_; }
  size_t inputBudget() const {
    return options_.memory_budget_bytes - pixels_.size() * sizeof(float);
  }

 private:
  Layout layout_;
  Options options_;
  std::vector<float> pixels_;
  std::vector<uint8_t> seen_;
  bool srgb_{false}, floating_{false};
  int bits_{8};
};
using Fetch = std::function<bool(const std::string&, std::vector<uint8_t>*,
                                 std::string*, size_t max_bytes)>;

// Exactly one pattern is allowed. No generic printf formatting is performed.
bool SplitPattern(const std::string&, std::string* prefix, std::string* suffix,
                  std::string* marker = nullptr);
uint32_t TileAt(float u, float v);
bool ValidateOptions(const Options&, std::string* error);
bool MakeLayout(const std::vector<uint32_t>& ids, int tile_width,
                int tile_height, const Options&, Layout*, std::string* error);
bool InspectLayout(const std::string& pattern, const std::vector<uint32_t>& ids,
                   const Fetch&, const Options&, Layout*, std::string*);
bool BakeAtlas(const std::string& pattern, const std::vector<uint32_t>& ids,
               const Fetch&, const Options&, bool srgb,
               const std::string& format, int jpeg_quality, Atlas*,
               std::string* error, const Layout* common = nullptr);

}  // namespace udim
}  // namespace lightusd
