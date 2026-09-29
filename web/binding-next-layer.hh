// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "c-api/lightusd-c.h"

namespace lightusd::web_next {

class NextAssetStore;

class NextLayerDocument {
 public:
  NextLayerDocument() = default;
  ~NextLayerDocument();
  NextLayerDocument(const NextLayerDocument&) = delete;
  NextLayerDocument& operator=(const NextLayerDocument&) = delete;

  int32_t load(const uint8_t* bytes, uint32_t size,
               const std::function<bool(const char*, size_t, size_t)>& progress = {});
  int32_t loadJSON(const uint8_t* bytes, uint32_t size);
  int32_t definePrim(const uint8_t* path, uint32_t path_size,
                     const uint8_t* type, uint32_t type_size);
  int32_t removePrim(const uint8_t* path, uint32_t path_size);
  int32_t removeAttribute(const uint8_t* path, uint32_t path_size,
                          const uint8_t* name, uint32_t name_size);
  int32_t setStringAttribute(const uint8_t* path, uint32_t path_size,
                             const uint8_t* name, uint32_t name_size,
                             const uint8_t* value, uint32_t value_size);
  int32_t setNumberAttribute(const uint8_t* path, uint32_t path_size,
                             const uint8_t* name, uint32_t name_size,
                             double value);
  int32_t setAttributeMetadata(const uint8_t* path, uint32_t path_size,
                               const uint8_t* name, uint32_t name_size,
                               const uint8_t* key, uint32_t key_size,
                               uint8_t kind, const uint8_t* payload,
                               uint32_t payload_size, double number,
                               int32_t integer);
  int32_t getAttributeMetadata(const uint8_t* path, uint32_t path_size,
                               const uint8_t* name, uint32_t name_size,
                               const uint8_t* key, uint32_t key_size,
                               uint8_t* kind, uint8_t* out, uint32_t cap);
  int32_t setStageMetadata(const uint8_t* key, uint32_t key_size,
                           uint8_t kind, const uint8_t* text,
                           uint32_t text_size, double number);
  int32_t setPrimMetadata(const uint8_t* path, uint32_t path_size,
                          const uint8_t* key, uint32_t key_size,
                          uint8_t kind, const uint8_t* payload,
                          uint32_t payload_size, uint32_t count);
  int32_t getPrimMetadata(const uint8_t* path, uint32_t path_size,
                          const uint8_t* key, uint32_t key_size,
                          uint8_t* kind, uint32_t* count, uint8_t* out,
                          uint32_t cap);
  int32_t primMetadataIsAuthored(const uint8_t* path, uint32_t path_size,
                                 const uint8_t* key, uint32_t key_size);
  int32_t getStageMetadataNumber(const uint8_t* key, uint32_t key_size,
                                 double* out);
  int32_t getStageMetadataString(const uint8_t* key, uint32_t key_size,
                                 uint8_t* out, uint32_t cap);
  int32_t stageMetadataIsAuthored(const uint8_t* key, uint32_t key_size);
  int32_t setRelationshipTargets(const uint8_t* path, uint32_t path_size,
                                 const uint8_t* name, uint32_t name_size,
                                 const uint8_t* packed_targets,
                                 uint32_t packed_size, uint32_t count);
  int32_t relationshipTargetCount(const uint8_t* path, uint32_t path_size,
                                  const uint8_t* name, uint32_t name_size);
  int32_t relationshipTargetCopy(const uint8_t* path, uint32_t path_size,
                                 const uint8_t* name, uint32_t name_size,
                                 uint32_t index, uint8_t* out,
                                 uint32_t cap);
  int32_t removeRelationship(const uint8_t* path, uint32_t path_size,
                             const uint8_t* name, uint32_t name_size);
  int32_t setAttribute(const uint8_t* path, uint32_t path_size,
                       const uint8_t* name, uint32_t name_size,
                       const uint8_t* type, uint32_t type_size,
                       uint8_t is_array, const uint8_t* data,
                       uint32_t data_size, uint32_t count);
  int32_t primCount() const;
  int32_t loaded() const { return stage_ != nullptr; }
  int32_t errorSize() const;
  int32_t errorCopy(uint8_t* out, uint32_t cap) const;
  int32_t exportUsdaSize();
  uintptr_t exportUsdaData() const;
  int32_t exportUsdcSize();
  uintptr_t exportUsdcData() const;
  int32_t exportUsdz(NextAssetStore* assets, uint8_t root_format);
  int32_t exportUsdzSize() const;
  uintptr_t exportUsdzData() const;
  int32_t mhProfileJSONSize();
  int32_t mhProfileJSONCopy(uint8_t* out, uint32_t cap) const;
  int32_t shadingGraphJSONSize();
  int32_t shadingGraphJSONCopy(uint8_t* out, uint32_t cap) const;
  int32_t exportJSONSize();
  uintptr_t exportJSONData() const;
  void end();

 private:
  int32_t fail_(const char* message);
  int32_t failJSON_(const char* message);
  bool ensureLoaded_();
  void clearExport_();

  lightusd_stage* stage_ = nullptr;
  std::string error_;
  lightusd_string* export_text_ = nullptr;
  lightusd_string* export_crate_ = nullptr;
  std::vector<uint8_t> export_usdz_;
  std::string mh_profile_json_;
  std::string shading_graph_json_;
  std::string layer_json_;
};

}  // namespace lightusd::web_next
