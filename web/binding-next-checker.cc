// SPDX-License-Identifier: Apache-2.0
#include "binding-next-api.h"
#include "binding-next-assets.hh"
#include "../tools/lusdchecker/checker.hh"
#include <cstdlib>
#include <cstring>

namespace lightusd::web_next {
uint8_t* NextCheckJSON(const uint8_t* data, uint32_t size,
    const uint8_t* filename, uint32_t filename_size,
    const uint8_t* options, uint32_t options_size, const void* asset_store) {
  if ((!data && size) || (!filename && filename_size) || (!options && options_size) ||
      size > (uint32_t(1) << 30) || filename_size > 65536 || options_size > (uint32_t(16) << 20)) return nullptr;
  next::AssetResolver assets;
  size_t accounted = 0;
  if (asset_store && static_cast<const NextAssetStore*>(asset_store)->copyAssetsTo(&assets, &accounted) != 0)
    return nullptr;
  const std::string name = filename_size ? std::string(reinterpret_cast<const char*>(filename), filename_size) : "";
  const std::string config = options_size ? std::string(reinterpret_cast<const char*>(options), options_size) : "{}";
  const auto report = lusdchecker::CheckMemory(data, size, name, config, &assets);
  const std::string json = report.dump();
  auto* out = static_cast<uint8_t*>(std::malloc(json.size() + 1));
  if (out) std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}
}  // namespace lightusd::web_next
