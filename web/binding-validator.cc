// SPDX-License-Identifier: Apache-2.0
#include "binding-next-api.h"
#include "../tools/lusdchecker/checker.hh"
#include <emscripten/emscripten.h>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
using Resolver = lightusd::next::AssetResolver;
using Json = lightusd::minijson::Value;
std::unique_ptr<Resolver> assets;
Json metadata;
size_t retained = 0;
constexpr size_t limit = size_t(512) << 20;
uint8_t* Copy(const std::string& text) {
  if (text.size() >= limit) return nullptr;
  auto* out = static_cast<uint8_t*>(std::malloc(text.size() + 1));
  if (out) std::memcpy(out, text.c_str(), text.size() + 1);
  return out;
}
}
extern "C" {
EMSCRIPTEN_KEEPALIVE void* lu_feature_alloc(uint32_t size) { return std::malloc(size); }
EMSCRIPTEN_KEEPALIVE void lu_feature_free(void* data) { std::free(data); }
EMSCRIPTEN_KEEPALIVE void lu_validator_clear() {
  assets.reset(); metadata = Json(); retained = 0;
}
EMSCRIPTEN_KEEPALIVE int lu_validator_begin(const char* data, uint32_t size) {
  lu_validator_clear();
  if ((!data && size) || size > (uint32_t(64) << 20) ||
      !lightusd::minijson::Parse(std::string(data ? data : "", size), &metadata) ||
      !metadata.is_object()) return -1;
  assets.reset(new (std::nothrow) Resolver());
  if (!assets) return -1;
  auto config = assets->GetConfig();
  config.working_directory = metadata["workingDirectory"].get_string();
  if (metadata.contains("allowParentPaths")) config.allow_parent_paths = metadata["allowParentPaths"].get_bool();
  for (const auto& path : metadata["searchPaths"]) config.search_paths.push_back(path.get_string());
  assets->SetConfig(config);
  retained = size;
  return 0;
}
EMSCRIPTEN_KEEPALIVE int lu_validator_asset(const char* name, uint32_t name_size,
                                           const uint8_t* data, uint32_t size) {
  if (!assets || !name || !name_size || (!data && size) ||
      name_size > limit - retained || size > limit - retained - name_size) return -1;
  std::string key(name, name_size);
  if (key.find('\0') != std::string::npos) return -1;
  assets->RegisterMemoryAsset(key, size ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>());
  retained += size + name_size;
  return 0;
}
EMSCRIPTEN_KEEPALIVE uint8_t* lu_validator_check(const uint8_t* data, uint32_t size,
    const char* name, uint32_t name_size, const char* options, uint32_t options_size,
    int checker) {
  if ((!data && size) || (!name && name_size) || (!options && options_size) ||
      size > (uint32_t(1) << 30) || name_size > 65536 || options_size > (uint32_t(16) << 20)) return nullptr;
  if (!checker) return lightusd::web_next::NextValidateJSON(data, size,
      reinterpret_cast<const uint8_t*>(name), name_size,
      reinterpret_cast<const uint8_t*>(options), options_size);
  if (assets && metadata["aliases"].is_object()) {
    for (const auto& alias : *metadata["aliases"].object_items()) {
      auto view = assets->GetMemoryAssetView(alias.value().get_string());
      if (view) assets->RegisterMemoryAssetView(alias.key, std::move(view));
    }
  }
  return Copy(lusdchecker::CheckMemory(data, size, std::string(name ? name : "", name_size),
      std::string(options ? options : "", options_size), assets.get()).dump());
}
}
