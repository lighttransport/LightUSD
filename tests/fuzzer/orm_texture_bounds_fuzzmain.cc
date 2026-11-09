// SPDX-License-Identifier: MIT
// Fuzzer for occlusion/roughness/metallic texture assembly bounds handling.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/safe-arithmetic.hh"
#include "tydra/texture-util.hh"

namespace {

struct Reader {
  const uint8_t *data{nullptr};
  size_t size{0};
  size_t offset{0};

  uint8_t u8() {
    if (offset >= size) return 0;
    return data[offset++];
  }

  size_t bounded(size_t max_value) {
    if (max_value == 0) return 0;
    return size_t(u8()) % (max_value + 1u);
  }
};

std::vector<uint8_t> ImageData(Reader &r, size_t w, size_t h, size_t channels,
                               bool present) {
  if (!present) return {};
  size_t total = 0;
  if (channels == 0 || channels > 4 ||
      !lightusd::safe::mul3(w, h, channels, &total) || total > 512) {
    total = r.bounded(64);
  }

  std::vector<uint8_t> data(total);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = r.u8();
  }
  return data;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 4096) return 0;

  Reader r{data, size, 0};
  const float occlusion = float(r.u8()) / 255.0f;
  const float roughness = float(r.u8()) / 255.0f;
  const float metallic = float(r.u8()) / 255.0f;

  size_t dims[3][3] = {};
  std::vector<uint8_t> images[3];
  size_t channel_indices[3] = {};
  for (size_t i = 0; i < 3; i++) {
    const bool present = (r.u8() & 1u) != 0;
    dims[i][0] = present ? (1u + r.bounded(15)) : 0u;
    dims[i][1] = present ? (1u + r.bounded(15)) : 0u;
    dims[i][2] = present ? r.bounded(6) : 0u;
    channel_indices[i] = r.bounded(6);
    images[i] = ImageData(r, dims[i][0], dims[i][1], dims[i][2], present);
  }

  std::vector<uint8_t> dst;
  size_t dst_w = 0;
  size_t dst_h = 0;
  (void)lightusd::tydra::BuildOcclusionRoughnessMetallicTexture(
      occlusion, roughness, metallic,
      images[0], dims[0][0], dims[0][1], dims[0][2], channel_indices[0],
      images[1], dims[1][0], dims[1][1], dims[1][2], channel_indices[1],
      images[2], dims[2][0], dims[2][1], dims[2][2], channel_indices[2],
      dst, dst_w, dst_h);

  return 0;
}
