// SPDX-License-Identifier: MIT
// Fuzzer for lightusd::legacy::HashMap collision-heavy operations.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "tiny-hashmap.hh"

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

  std::string bytes(size_t max_len) {
    const size_t wanted = bounded(max_len);
    const size_t remain = size - offset;
    const size_t n = std::min(wanted, remain);
    std::string s(reinterpret_cast<const char *>(data + offset), n);
    offset += n;
    return s.empty() ? std::string("empty") : s;
  }
};

struct AlwaysZeroHash {
  size_t operator()(const std::string &) const { return 0; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0 || size > 4096) return 0;

  Reader r{data, size, 0};
  lightusd::legacy::HashMap<std::string, int, AlwaysZeroHash> map;
  map.reserve(1u + r.bounded(16));

  const size_t ops = 1u + r.bounded(160);
  for (size_t i = 0; i < ops; i++) {
    const uint8_t op = r.u8() % 8u;
    std::string key = r.bytes(32);

    switch (op) {
      case 0:
        (void)map.emplace(key, int(i));
        break;
      case 1:
        (void)map.insert_or_assign(key, int(i * 3u));
        break;
      case 2:
        (void)map.erase(key);
        break;
      case 3:
        (void)map.find(key);
        break;
      case 4:
        (void)map.contains(key);
        break;
      case 5:
        map[key] = int(i);
        break;
      case 6:
        map.clear();
        break;
      default:
        map.reserve(1u + r.bounded(256));
        break;
    }
  }

  return 0;
}
