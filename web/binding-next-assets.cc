// SPDX-License-Identifier: Apache-2.0
#include "binding-next-assets.hh"
#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <limits>
#include <unordered_set>
#include <utility>
#include "sha256.hh"

namespace lightusd::web_next {

std::string GenerateAssetUuid() {
  std::random_device source;
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < bytes.size(); i += 4) {
    const uint32_t value = source();
    for (size_t j = 0; j < 4; ++j)
      bytes[i + j] = static_cast<uint8_t>(value >> (j * 8));
  }
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
  constexpr char hex[] = "0123456789abcdef";
  std::string uuid;
  uuid.reserve(36);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) uuid.push_back('-');
    uuid.push_back(hex[bytes[i] >> 4]);
    uuid.push_back(hex[bytes[i] & 0x0fu]);
  }
  return uuid;
}

std::string NextAssetStore::makeUuid_() { return GenerateAssetUuid(); }

bool NextAssetStore::assetMetadataCost_(size_t identifier_size,
                                        uint64_t* out) {
  constexpr uint64_t kPerAssetBookkeepingBytes = 256;
  if (!out) return false;
  *out = static_cast<uint64_t>(identifier_size) + kPerAssetBookkeepingBytes;
  return true;
}

bool NextAssetStore::aliasMetadataCost_(size_t authored_size,
                                        size_t resolved_size,
                                        uint64_t* out) {
  constexpr uint64_t kPerAliasBookkeepingBytes = 128;
  if (!out) return false;
  *out = static_cast<uint64_t>(authored_size) +
      static_cast<uint64_t>(resolved_size) + kPerAliasBookkeepingBytes;
  return true;
}

int NextAssetStore::registerMemoryAsset(const uint8_t* identifier,
                                        uint32_t identifier_size,
                                        const uint8_t* bytes,
                                        uint32_t byte_size,
                                        std::string* registered_identifier) {
  if (!registered_identifier || (!identifier && identifier_size) ||
      (!bytes && byte_size) || byte_size > (uint32_t{1} << 30)) return -1;
  std::string key;
  if (identifier_size)
    key.assign(reinterpret_cast<const char*>(identifier), identifier_size);
  if (key.empty()) {
    do {
      key = "usd-anon:" + std::to_string(next_anonymous_id_++);
    } while (assets_.find(key) != assets_.end());
  }
  const auto previous = assets_.find(key);
  const size_t old_size = previous == assets_.end() ? 0 : previous->second;
  uint64_t old_metadata = 0, new_metadata = 0;
  if ((previous != assets_.end() && !assetMetadataCost_(key.size(), &old_metadata)) ||
      !assetMetadataCost_(key.size(), &new_metadata) || old_metadata > metadata_bytes_ ||
      metadata_bytes_ - old_metadata > metadata_limit_bytes_ ||
      new_metadata > metadata_limit_bytes_ - (metadata_bytes_ - old_metadata)) return -4;
  // Apply the hard payload budget before any compatibility-cache eviction so
  // a rejected registration cannot evict otherwise valid cache entries.
  if (byte_size > memory_limit_bytes_ -
          std::min<size_t>(memory_bytes_, memory_limit_bytes_)) return -2;
  std::shared_ptr<void> payload_reservation = payload_budget_->Reserve(byte_size);
  if (byte_size && !payload_reservation) return -3;
  if (previous == assets_.end() && cache_max_bytes_ != 0) {
    const uint64_t incoming = static_cast<uint64_t>(key.size()) + byte_size;
    const uint64_t target = incoming >= cache_max_bytes_
        ? 0 : cache_max_bytes_ - incoming;
    while (!assets_.empty() && cacheSizeBytes() > target) {
      const std::string oldest_key = assets_.begin()->first;
      unregisterMemoryAsset(
          reinterpret_cast<const uint8_t*>(oldest_key.data()),
          static_cast<uint32_t>(oldest_key.size()));
    }
  }
  // Reserve the full new payload during replacement because the new vector
  // exists before AssetResolver swaps out its previous shared payload.
  std::vector<uint8_t> payload;
  if (byte_size) payload.assign(bytes, bytes + byte_size);
  borrowed_views_.erase(key);
  auto retained = std::shared_ptr<const std::vector<uint8_t>>(
      new std::vector<uint8_t>(std::move(payload)),
      [reservation = std::move(payload_reservation)](
          const std::vector<uint8_t>* value) {
        delete value;
        (void)reservation;
      });
  std::string actual = resolver_.RegisterMemoryAssetView(key,
                                                          std::move(retained));
  if (actual.empty()) return -1;
  const auto old_uuid = uuids_.find(actual);
  if (old_uuid != uuids_.end()) {
    assets_by_uuid_.erase(old_uuid->second);
    uuids_.erase(old_uuid);
  }
  uuids_[actual] = makeUuid_();
  assets_by_uuid_[uuids_[actual]] = actual;
  hashes_[actual] = lightusd::sha256(reinterpret_cast<const char*>(bytes), byte_size);
  if (old_size > memory_bytes_) return -1;
  memory_bytes_ -= old_size;
  memory_bytes_ += byte_size;
  metadata_bytes_ = metadata_bytes_ - old_metadata + new_metadata;
  assets_[actual] = byte_size;
  *registered_identifier = std::move(actual);
  return 0;
}

int NextAssetStore::setAssetFromRawPointer(const uint8_t* identifier,
                                           uint32_t identifier_size,
                                           const uint8_t* bytes,
                                           uint32_t byte_size) {
  if (!bytes || byte_size == 0 || byte_size > (uint32_t{1} << 30) ||
      (!identifier && identifier_size)) return -1;
  std::string key;
  if (identifier_size)
    key.assign(reinterpret_cast<const char*>(identifier), identifier_size);
  const bool overwritten = !key.empty() && assets_.find(key) != assets_.end();
  std::string registered_identifier;
  const int status = registerMemoryAsset(identifier, identifier_size, bytes,
                                         byte_size, &registered_identifier);
  if (status != 0) return status;
  return overwritten ? 1 : 0;
}

int NextAssetStore::adoptStreamingAsset(const std::string& identifier,
                                        std::vector<uint8_t>&& bytes,
                                        const std::string& uuid,
                                        std::shared_ptr<void> payload_reservation) {
  if (identifier.empty() || uuid.empty() ||
      bytes.size() > (uint32_t{1} << 30)) return -1;
  const auto previous = assets_.find(identifier);
  const size_t old_size = previous == assets_.end() ? 0 : previous->second;
  uint64_t old_metadata = 0, new_metadata = 0;
  if ((previous != assets_.end() &&
       !assetMetadataCost_(identifier.size(), &old_metadata)) ||
      !assetMetadataCost_(identifier.size(), &new_metadata) ||
      old_metadata > metadata_bytes_ ||
      metadata_bytes_ - old_metadata > metadata_limit_bytes_ ||
      new_metadata > metadata_limit_bytes_ - (metadata_bytes_ - old_metadata)) return -4;
  if (bytes.size() > memory_limit_bytes_ -
          std::min<size_t>(memory_bytes_, memory_limit_bytes_)) return -2;
  if (bytes.size() && !payload_reservation) {
    payload_reservation = payload_budget_->Reserve(bytes.size());
    if (!payload_reservation) return -3;
  }
  const auto uuid_owner = assets_by_uuid_.find(uuid);
  if (uuid_owner != assets_by_uuid_.end() && uuid_owner->second != identifier)
    return -1;
  if (previous == assets_.end() && cache_max_bytes_ != 0) {
    const uint64_t incoming = static_cast<uint64_t>(identifier.size()) + bytes.size();
    const uint64_t target = incoming >= cache_max_bytes_
        ? 0 : cache_max_bytes_ - incoming;
    while (!assets_.empty() && cacheSizeBytes() > target) {
      const std::string oldest_key = assets_.begin()->first;
      unregisterMemoryAsset(reinterpret_cast<const uint8_t*>(oldest_key.data()),
                           static_cast<uint32_t>(oldest_key.size()));
    }
  }
  borrowed_views_.erase(identifier);
  auto retained = std::shared_ptr<const std::vector<uint8_t>>(
      new std::vector<uint8_t>(std::move(bytes)),
      [reservation = std::move(payload_reservation)](
          const std::vector<uint8_t>* value) {
        delete value;
        (void)reservation;
      });
  const std::string actual = resolver_.RegisterMemoryAssetView(
      identifier, std::move(retained));
  if (actual != identifier) return -1;
  const auto old_uuid = uuids_.find(identifier);
  if (old_uuid != uuids_.end()) {
    assets_by_uuid_.erase(old_uuid->second);
    uuids_.erase(old_uuid);
  }
  uuids_[identifier] = uuid;
  assets_by_uuid_[uuid] = identifier;
  const auto view = resolver_.GetMemoryAssetView(identifier);
  if (!view) return -1;
  hashes_[identifier] = lightusd::sha256(
      reinterpret_cast<const char*>(view->data()), view->size());
  if (old_size > memory_bytes_) return -1;
  memory_bytes_ -= old_size;
  memory_bytes_ += view->size();
  metadata_bytes_ = metadata_bytes_ - old_metadata + new_metadata;
  assets_[identifier] = view->size();
  return 0;
}

int NextAssetStore::unregisterMemoryAsset(const uint8_t* identifier,
                                          uint32_t size) {
  if (!identifier || !size) return -1;
  const std::string key(reinterpret_cast<const char*>(identifier), size);
  const auto it = assets_.find(key);
  if (it == assets_.end()) return 0;
  uint64_t metadata_cost = 0;
  if (!assetMetadataCost_(key.size(), &metadata_cost) || metadata_cost > metadata_bytes_)
    return -1;
  borrowed_views_.erase(key);
  if (!resolver_.UnregisterMemoryAsset(key)) return 0;
  metadata_bytes_ -= metadata_cost;
  memory_bytes_ -= it->second;
  assets_.erase(it);
  const auto uuid = uuids_.find(key);
  if (uuid != uuids_.end()) {
    assets_by_uuid_.erase(uuid->second);
    uuids_.erase(uuid);
  }
  hashes_.erase(key);
  return 1;
}

std::string NextAssetStore::aliasTarget_(const std::string& identifier) const {
  const auto it = aliases_.find(identifier);
  return it == aliases_.end() ? identifier : it->second;
}

int NextAssetStore::readMemoryAsset(const uint8_t* identifier, uint32_t size,
                                    std::vector<uint8_t>* bytes) const {
  if (!bytes || (!identifier && size)) return -1;
  std::string key;
  if (size) key.assign(reinterpret_cast<const char*>(identifier), size);
  key = aliasTarget_(key);
  std::string error;
  return resolver_.ReadAsset(key, bytes, &error) ? 1 : 0;
}

int NextAssetStore::borrowedAssetView(const uint8_t* identifier, uint32_t size,
                                     const uint8_t** data,
                                     uint32_t* byte_size) {
  if (!data || !byte_size || (!identifier && size)) return -1;
  std::string key;
  if (size) key.assign(reinterpret_cast<const char*>(identifier), size);
  key = aliasTarget_(key);
  std::shared_ptr<const std::vector<uint8_t>> view =
      resolver_.GetMemoryAssetView(key);
  if (!view) return 0;
  if (view->size() > (std::numeric_limits<uint32_t>::max)()) return -1;
  borrowed_views_[key] = std::move(view);
  const auto& retained = borrowed_views_[key];
  *data = retained->data();
  *byte_size = static_cast<uint32_t>(retained->size());
  return 1;
}

int NextAssetStore::setAlias(const uint8_t* authored, uint32_t authored_size,
                             const uint8_t* resolved, uint32_t resolved_size) {
  if (!authored || !authored_size || !resolved || !resolved_size) return -1;
  const std::string authored_key(reinterpret_cast<const char*>(authored), authored_size);
  const auto previous = aliases_.find(authored_key);
  uint64_t old_metadata = 0, new_metadata = 0;
  if ((previous != aliases_.end() && !aliasMetadataCost_(previous->first.size(),
          previous->second.size(), &old_metadata)) ||
      !aliasMetadataCost_(authored_size, resolved_size, &new_metadata) ||
      old_metadata > metadata_bytes_ ||
      metadata_bytes_ - old_metadata > metadata_limit_bytes_ ||
      new_metadata > metadata_limit_bytes_ - (metadata_bytes_ - old_metadata)) return -2;
  aliases_[authored_key] =
      std::string(reinterpret_cast<const char*>(resolved), resolved_size);
  metadata_bytes_ = metadata_bytes_ - old_metadata + new_metadata;
  return 0;
}

int NextAssetStore::setBaseWorkingPath(const uint8_t* path, uint32_t size) {
  if (!path && size) return -1;
  resolver_.SetWorkingDirectory(size
      ? std::string(reinterpret_cast<const char*>(path), size) : std::string());
  return 0;
}

int NextAssetStore::baseWorkingPathCopy(uint8_t* out, uint32_t cap) const {
  const std::string& value = resolver_.GetWorkingDirectory();
  if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
  const int required = static_cast<int>(value.size());
  if (!out || cap < value.size() || value.empty()) return required;
  std::memcpy(out, value.data(), value.size());
  return required;
}

int NextAssetStore::addSearchPath(const uint8_t* path, uint32_t size) {
  if (!path || !size) return -1;
  resolver_.AddSearchPath(std::string(reinterpret_cast<const char*>(path), size));
  return 0;
}

int NextAssetStore::clearSearchPaths() {
  resolver_.ClearSearchPaths();
  return 0;
}

int NextAssetStore::searchPathCount() const {
  const size_t count = resolver_.GetSearchPaths().size();
  return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
      ? static_cast<int>(count) : -1;
}

int NextAssetStore::searchPathCopy(int index, uint8_t* out, uint32_t cap) const {
  const auto& paths = resolver_.GetSearchPaths();
  if (index < 0 || static_cast<size_t>(index) >= paths.size()) return -1;
  const std::string& value = paths[static_cast<size_t>(index)];
  if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
  const int required = static_cast<int>(value.size());
  if (!out || cap < value.size() || value.empty()) return required;
  std::memcpy(out, value.data(), value.size());
  return required;
}

int NextAssetStore::identifierCount() const {
  return assets_.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
      ? -1 : static_cast<int>(assets_.size());
}

int NextAssetStore::identifierCopy(int index, uint8_t* out, uint32_t cap) const {
  if (index < 0 || static_cast<size_t>(index) >= assets_.size()) return -1;
  auto it = assets_.begin();
  std::advance(it, index);
  if (it->first.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return -1;
  const int size = static_cast<int>(it->first.size());
  if (out && cap >= it->first.size() && size)
    std::memcpy(out, it->first.data(), it->first.size());
  return size;
}

int NextAssetStore::assetUuid(const uint8_t* identifier, uint32_t size,
                              uint8_t* out, uint32_t cap) const {
  if (!identifier && size) return -1;
  std::string key;
  if (size) key.assign(reinterpret_cast<const char*>(identifier), size);
  key = aliasTarget_(key);
  const auto it = uuids_.find(key);
  if (it == uuids_.end()) return 0;
  if (it->second.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return -1;
  const int length = static_cast<int>(it->second.size());
  if (out && cap >= it->second.size()) std::memcpy(out, it->second.data(), it->second.size());
  return length;
}

int NextAssetStore::findAssetByUuid(const uint8_t* uuid, uint32_t size,
                                    uint8_t* out, uint32_t cap) const {
  if (!uuid || !size) return -1;
  const std::string key(reinterpret_cast<const char*>(uuid), size);
  const auto found = assets_by_uuid_.find(key);
  if (found == assets_by_uuid_.end()) return 0;
  if (found->second.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return -1;
  const int length = static_cast<int>(found->second.size());
  if (out && cap >= found->second.size()) std::memcpy(out, found->second.data(), found->second.size());
  return length;
}

int NextAssetStore::assetHash(const uint8_t* identifier, uint32_t size,
                              uint8_t* out, uint32_t cap) const {
  if (!identifier && size) return -1;
  std::string key;
  if (size) key.assign(reinterpret_cast<const char*>(identifier), size);
  key = aliasTarget_(key);
  const auto it = hashes_.find(key);
  if (it == hashes_.end()) return 0;
  const int length = static_cast<int>(it->second.size());
  if (out && cap >= it->second.size()) std::memcpy(out, it->second.data(), it->second.size());
  return length;
}

int NextAssetStore::verifyAssetHash(const uint8_t* identifier,
                                   uint32_t identifier_size,
                                   const uint8_t* hash,
                                   uint32_t hash_size) const {
  if ((!identifier && identifier_size) || (!hash && hash_size)) return -1;
  std::string key;
  if (identifier_size)
    key.assign(reinterpret_cast<const char*>(identifier), identifier_size);
  key = aliasTarget_(key);
  const auto it = hashes_.find(key);
  if (it == hashes_.end()) return 0;
  const std::string expected = hash_size
      ? std::string(reinterpret_cast<const char*>(hash), hash_size)
      : std::string();
  return it->second == expected ? 1 : 0;
}

int NextAssetStore::deleteAssetByUuid(const uint8_t* uuid, uint32_t size) {
  if (!uuid || !size) return -1;
  const std::string key(reinterpret_cast<const char*>(uuid), size);
  const auto found = assets_by_uuid_.find(key);
  if (found == assets_by_uuid_.end()) return 0;
  const std::string identifier = found->second;
  return unregisterMemoryAsset(reinterpret_cast<const uint8_t*>(identifier.data()),
                               static_cast<uint32_t>(identifier.size()));
}

int NextAssetStore::setMemoryLimit(uint32_t limit_bytes) {
  if (limit_bytes == 0 || limit_bytes > (uint32_t{1} << 30) ||
      memory_bytes_ > limit_bytes) return -1;
  if (!payload_budget_->SetLimit(limit_bytes)) return -1;
  memory_limit_bytes_ = limit_bytes;
  return 0;
}

int NextAssetStore::setMetadataLimit(uint64_t limit_bytes) {
  if (limit_bytes == 0 || limit_bytes > (uint64_t{1} << 30) ||
      metadata_bytes_ > limit_bytes) return -1;
  metadata_limit_bytes_ = limit_bytes;
  return 0;
}

uint64_t NextAssetStore::cacheSizeBytes() const {
  uint64_t total = 0;
  for (const auto& asset : assets_) {
    const uint64_t entry_size = static_cast<uint64_t>(asset.first.size()) +
        static_cast<uint64_t>(asset.second);
    if (entry_size > (std::numeric_limits<uint64_t>::max)() - total)
      return (std::numeric_limits<uint64_t>::max)();
    total += entry_size;
  }
  return total;
}

int NextAssetStore::clear() {
  for (const auto& asset : assets_) resolver_.UnregisterMemoryAsset(asset.first);
  assets_.clear();
  aliases_.clear();
  borrowed_views_.clear();
  uuids_.clear();
  assets_by_uuid_.clear();
  hashes_.clear();
  memory_bytes_ = 0;
  metadata_bytes_ = 0;
  return 0;
}

int NextAssetStore::copyAssetsTo(
    lightusd::next::AssetResolver* destination,
    size_t* accounted_bytes) const {
  if (!destination || !accounted_bytes) return -1;
  destination->SetConfig(resolver_.GetConfig());
  size_t total = 0;
  std::unordered_set<std::string> registered_keys;
  std::unordered_set<const std::vector<uint8_t>*> payloads;
  auto register_view = [&](const std::string& key,
                           std::shared_ptr<const std::vector<uint8_t>> view) {
    if (!view || key.empty()) return false;
    if (registered_keys.insert(key).second) {
      if (key.size() > (std::numeric_limits<size_t>::max)() - total)
        return false;
      total += key.size();
    }
    if (payloads.insert(view.get()).second) {
      if (view->size() > (std::numeric_limits<size_t>::max)() - total)
        return false;
      total += view->size();
    }
    return destination->RegisterMemoryAssetView(key, std::move(view)) == key;
  };
  for (const auto& asset : assets_) {
    auto view = resolver_.GetMemoryAssetView(asset.first);
    if (!register_view(asset.first, std::move(view))) return -1;
  }
  for (const auto& alias : aliases_) {
    const std::string target = aliasTarget_(alias.first);
    auto view = resolver_.GetMemoryAssetView(target);
    // Unresolved aliases stay unresolved, matching NextAssetStore reads.
    if (!view) continue;
    if (!register_view(alias.first, std::move(view))) return -1;
  }
  *accounted_bytes = total;
  return 0;
}

}  // namespace lightusd::web_next
