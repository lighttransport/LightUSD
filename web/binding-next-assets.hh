// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "asset-uuid.hh"
#include "next/resolver/asset-resolver.hh"

namespace lightusd::web_next {

class NextAssetStore {
 public:
  int registerMemoryAsset(const uint8_t* identifier, uint32_t identifier_size,
                          const uint8_t* bytes, uint32_t byte_size,
                          std::string* registered_identifier);
  int adoptStreamingAsset(const std::string& identifier,
                          std::vector<uint8_t>&& bytes,
                          const std::string& uuid,
                          std::shared_ptr<void> payload_reservation = {});
  int setAssetFromRawPointer(const uint8_t* identifier,
                             uint32_t identifier_size, const uint8_t* bytes,
                             uint32_t byte_size);
  int unregisterMemoryAsset(const uint8_t* identifier, uint32_t size);
  int readMemoryAsset(const uint8_t* identifier, uint32_t size,
                      std::vector<uint8_t>* bytes) const;
  int borrowedAssetView(const uint8_t* identifier, uint32_t size,
                        const uint8_t** data, uint32_t* byte_size);
  int setAlias(const uint8_t* authored, uint32_t authored_size,
               const uint8_t* resolved, uint32_t resolved_size);
  int identifierCount() const;
  int identifierCopy(int index, uint8_t* out, uint32_t cap) const;
  int setBaseWorkingPath(const uint8_t* path, uint32_t size);
  int baseWorkingPathCopy(uint8_t* out, uint32_t cap) const;
  int addSearchPath(const uint8_t* path, uint32_t size);
  int clearSearchPaths();
  int searchPathCount() const;
  int searchPathCopy(int index, uint8_t* out, uint32_t cap) const;
  void setAllowParentRelativeAssetPaths(bool allow) {
    resolver_.SetAllowParentPaths(allow);
  }
  bool allowParentRelativeAssetPaths() const {
    return resolver_.GetAllowParentPaths();
  }
  int streamingAssetUuid(const uint8_t* identifier, uint32_t size,
                         uint8_t* out, uint32_t cap) const {
    return assetUuid(identifier, size, out, cap);
  }
  int assetUuid(const uint8_t* identifier, uint32_t size,
                uint8_t* out, uint32_t cap) const;
  int findAssetByUuid(const uint8_t* uuid, uint32_t size,
                      uint8_t* out, uint32_t cap) const;
  int assetHash(const uint8_t* identifier, uint32_t size,
                uint8_t* out, uint32_t cap) const;
  int verifyAssetHash(const uint8_t* identifier, uint32_t identifier_size,
                      const uint8_t* hash, uint32_t hash_size) const;
  int deleteAssetByUuid(const uint8_t* uuid, uint32_t size);
  uint64_t cacheSizeBytes() const;
  uint64_t cacheMaxSizeBytes() const { return cache_max_bytes_; }
  void setCacheMaxSizeBytes(uint64_t bytes) { cache_max_bytes_ = bytes; }
  int setMemoryLimit(uint32_t limit_bytes);
  uint32_t memoryLimit() const { return memory_limit_bytes_; }
  uint32_t memoryBytes() const { return static_cast<uint32_t>(memory_bytes_); }
  int setMetadataLimit(uint64_t limit_bytes);
  uint64_t metadataBytes() const { return metadata_bytes_; }
  uint64_t metadataLimit() const { return metadata_limit_bytes_; }
  std::shared_ptr<lightusd::next::AssetPayloadBudget> payloadBudget() const {
    return payload_budget_;
  }
  int clear();
  // Import a snapshot into another resolver without copying payload bytes.
  // Aliases are materialized as additional keys sharing the same immutable data.
  int copyAssetsTo(lightusd::next::AssetResolver* destination,
                   size_t* accounted_bytes) const;

 private:
  std::string aliasTarget_(const std::string& identifier) const;
  static std::string makeUuid_();
  static bool assetMetadataCost_(size_t identifier_size, uint64_t* out);
  static bool aliasMetadataCost_(size_t authored_size, size_t resolved_size,
                                 uint64_t* out);
  lightusd::next::AssetResolver resolver_;
  std::map<std::string, size_t> assets_;
  std::map<std::string, std::string> aliases_;
  std::map<std::string, std::string> uuids_;
  std::map<std::string, std::string> assets_by_uuid_;
  std::map<std::string, std::string> hashes_;
  std::map<std::string,
      std::shared_ptr<const std::vector<uint8_t>>> borrowed_views_;
  size_t memory_bytes_{0};
  uint64_t metadata_bytes_{0};
  uint64_t metadata_limit_bytes_{uint64_t{1} << 26};
  uint32_t memory_limit_bytes_{uint32_t{1} << 29};
  std::shared_ptr<lightusd::next::AssetPayloadBudget> payload_budget_ =
      std::make_shared<lightusd::next::AssetPayloadBudget>(uint32_t{1} << 29);
  uint64_t cache_max_bytes_{0};
  uint64_t next_anonymous_id_{1};
};

}  // namespace lightusd::web_next
