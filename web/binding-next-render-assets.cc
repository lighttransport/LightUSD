// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "binding-next-assets.hh"
#include "core/safe-arithmetic.hh"

namespace lightusd {
namespace web_next {
int RenderStream::provideAssetBytes(const uint8_t* name, uint32_t name_size,
                                    const uint8_t* bytes, uint32_t byte_size) {
    if ((!name && name_size) || (!bytes && byte_size)) {
      error_ = "Invalid asset buffer";
      return -1;
    }
    if (byte_size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return -1;
    }
    std::string raw_name;
    if (name_size) raw_name.assign(reinterpret_cast<const char*>(name), name_size);
    std::string key = NormalizeRenderAssetKey(raw_name);
    size_t used = 0;
    used = providedAssetMemoryBytes();
    if (used == std::numeric_limits<size_t>::max()) {
      error_ = "Provided asset accounting overflow";
      return -1;
    }
    const auto previous = clip_assets_.find(key);
    if (previous != clip_assets_.end()) used -= key.size() + previous->second.size();
    if (key.size() > std::numeric_limits<size_t>::max() - byte_size ||
        used > std::numeric_limits<size_t>::max() - key.size() - byte_size) {
      error_ = "Provided asset accounting overflow";
      return -1;
    }
    const size_t next_used = used + key.size() + byte_size;
    std::shared_ptr<void> reservation;
    if (payload_budget_ && byte_size) {
      reservation = payload_budget_->Reserve(byte_size);
      if (!reservation) {
        error_ = "Shared asset payload budget exceeded";
        return -1;
      }
    }
    if (next_used > maxMemoryLimitBytes()) {
      error_ = "Provided assets exceed configured resident memory limit";
      return -1;
    }
    if (key.size() > std::numeric_limits<size_t>::max() - byte_size ||
        key.size() + byte_size > remainingMemoryLimitBytes()) {
      error_ = "Provided asset replacement exceeds remaining resident memory";
      return -1;
    }
    if (provided_asset_byte_limit_ != 0 && next_used > provided_asset_byte_limit_) {
      error_ = "Provided assets exceed configured byte limit";
      return -1;
    }
    std::string data;
    if (byte_size) data.assign(reinterpret_cast<const char*>(bytes), byte_size);
    clip_assets_[key] = std::move(data);
    invalidateCopyPreparation_();
    if (reservation) clip_asset_budget_reservations_[key] = std::move(reservation);
    else clip_asset_budget_reservations_.erase(key);
    return 0;
}

int RenderStream::importAssetStore(const NextAssetStore& store) {
    lightusd::AssetResolver candidate;
    candidate.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
    size_t imported_bytes = 0;
    if (store.copyAssetsTo(&candidate, &imported_bytes) != 0) {
      error_ = "Unable to import memory asset store";
      return -1;
    }
    const size_t current = providedAssetMemoryBytes();
    if (current < imported_asset_bytes_) {
      error_ = "Imported asset accounting is inconsistent";
      return -1;
    }
    const size_t other_assets = current - imported_asset_bytes_;
    const size_t memory_limit = maxMemoryLimitBytes();
    if (other_assets > memory_limit || imported_bytes > memory_limit - other_assets ||
        (provided_asset_byte_limit_ != 0 &&
         (other_assets > provided_asset_byte_limit_ ||
          imported_bytes > provided_asset_byte_limit_ - other_assets))) {
      error_ = "Imported assets exceed configured memory limit";
      return -1;
    }
    const auto next_budget = store.payloadBudget();
    if (payload_budget_ && next_budget != payload_budget_ &&
        (!streaming_assets_.empty() || !clip_asset_budget_reservations_.empty())) {
      error_ = "Cannot change shared payload budget while local assets are retained";
      return -1;
    }
    if (!payload_budget_ || payload_budget_ != next_budget) {
      std::map<std::string, std::shared_ptr<void>> reservations;
      for (const auto& item : clip_assets_) {
        auto lease = next_budget->Reserve(item.second.size());
        if (!item.second.empty() && !lease) {
          error_ = "Shared asset payload budget exceeded by provided asset";
          return -1;
        }
        reservations.emplace(item.first, std::move(lease));
      }
      std::map<std::string, std::shared_ptr<void>> stream_reservations;
      for (const auto& item : streaming_assets_) {
        auto lease = next_budget->Reserve(item.second.expected_size);
        if (item.second.expected_size && !lease) {
          error_ = "Shared asset payload budget exceeded by active stream";
          return -1;
        }
        stream_reservations.emplace(item.first, std::move(lease));
      }
      // Commit the newly acquired reservations only after every active stream
      // fits the target store's cap.
      clip_asset_budget_reservations_ = std::move(reservations);
      for (auto& item : streaming_assets_)
        item.second.budget_reservation =
            std::move(stream_reservations[item.first]);
    }
    asset_resolver_ = std::move(candidate);
    invalidateCopyPreparation_();
    imported_asset_bytes_ = imported_bytes;
    payload_budget_ = next_budget;
    error_.clear();
    return 0;
}

int RenderStream::beginCachedAsset(NextAssetStore& store,
                                  const uint8_t* identifier,
                                  uint32_t identifier_size) {
    if (!identifier || identifier_size == 0) {
      error_ = "Invalid cached asset identifier";
      return 0;
    }
    const uint8_t* bytes = nullptr;
    uint32_t byte_size = 0;
    const int found = store.borrowedAssetView(
        identifier, identifier_size, &bytes, &byte_size);
    if (found <= 0) {
      error_ = found == 0 ? "Asset not found in cache" :
                            "Unable to read cached asset";
      return 0;
    }
    if (byte_size == 0) {
      error_ = "Cached asset is empty";
      return 0;
    }
    if (byte_size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    if (byte_size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return 0;
    }
    if (importAssetStore(store) != 0) return 0;
    return beginBytes(bytes, byte_size);
}

int RenderStream::clearImportedAssetStore() {
    lightusd::AssetResolver candidate;
    candidate.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
    asset_resolver_ = std::move(candidate);
    imported_asset_bytes_ = 0;
    error_.clear();
    return 0;
}

int RenderStream::startStreamingAsset(const uint8_t* name, uint32_t name_size,
                                     uint32_t expected_size) {
    if (!name || name_size == 0 || expected_size > (uint32_t{1} << 30)) {
      error_ = "Invalid streaming asset request";
      return -1;
    }
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const size_t current = providedAssetMemoryBytes();
    if (current == std::numeric_limits<size_t>::max()) return -1;
    const auto previous = streaming_assets_.find(key);
    size_t used = current;
    if (previous != streaming_assets_.end()) {
      const size_t old_size = key.size() + previous->second.expected_size;
      if (old_size > used) return -1;
      used -= old_size;
    }
    if (key.size() > std::numeric_limits<size_t>::max() - expected_size ||
        used > std::numeric_limits<size_t>::max() - key.size() - expected_size) {
      error_ = "Streaming asset accounting overflow";
      return -1;
    }
    const size_t next = used + key.size() + expected_size;
    const size_t range_bytes = previous == streaming_assets_.end()
        ? sizeof(StreamingAsset::written_ranges) : 0;
    if (key.size() > std::numeric_limits<size_t>::max() - expected_size ||
        key.size() + expected_size >
            std::numeric_limits<size_t>::max() - range_bytes) {
      error_ = "Streaming asset accounting overflow";
      return -1;
    }
    if (next > maxMemoryLimitBytes() ||
        (provided_asset_byte_limit_ && next > provided_asset_byte_limit_) ||
        key.size() + expected_size + range_bytes > remainingMemoryLimitBytes()) {
      error_ = "Streaming asset exceeds configured memory limit";
      return -1;
    }
    StreamingAsset stream;
    if (payload_budget_ && expected_size) {
      stream.budget_reservation = payload_budget_->Reserve(expected_size);
      if (!stream.budget_reservation) {
        error_ = "Shared asset payload budget exceeded";
        return -1;
      }
    }
    stream.expected_size = expected_size;
    stream.uuid = GenerateAssetUuid();
    stream.bytes.resize(expected_size);
    streaming_assets_[key] = std::move(stream);
    return 0;
}

int RenderStream::appendStreamingAsset(const uint8_t* name, uint32_t name_size,
                                      const uint8_t* bytes, uint32_t byte_size) {
    if (!name || !name_size || (!bytes && byte_size)) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    if (byte_size) {
      const int marked = markStreamingRange_(it->second, it->second.cursor, byte_size);
      if (marked != 1) return 0;
      std::memcpy(&it->second.bytes[it->second.cursor], bytes, byte_size);
      it->second.cursor += byte_size;
    }
    return 1;
}

int RenderStream::streamingAssetBytesWritten(const uint8_t* name,
                                             uint32_t name_size) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    return it == streaming_assets_.end() ? -1 : static_cast<int>(it->second.written_size);
}

int RenderStream::streamingAssetExpectedBytes(const uint8_t* name,
                                              uint32_t name_size) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    return it == streaming_assets_.end() ? -1 : static_cast<int>(it->second.expected_size);
}

int RenderStream::streamingAssetUuidCopy(const uint8_t* name,
                                         uint32_t name_size,
                                         uint8_t* out, uint32_t cap) const {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.uuid.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int length = static_cast<int>(it->second.uuid.size());
    if (out && cap >= it->second.uuid.size())
      std::memcpy(out, it->second.uuid.data(), it->second.uuid.size());
    return length;
}

uintptr_t RenderStream::streamingAssetViewPtr(const uint8_t* name,
                                              uint32_t name_size,
                                              uint32_t byte_size) const {
    if (!name || !name_size || !byte_size) return 0;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    return reinterpret_cast<uintptr_t>(
        it->second.bytes.data() + it->second.cursor);
}

uintptr_t RenderStream::streamingAssetViewPtrAt(const uint8_t* name,
                                               uint32_t name_size,
                                               uint32_t offset,
                                               uint32_t byte_size) const {
    if (!name || !name_size || !byte_size) return 0;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size == it->second.expected_size ||
        offset > it->second.expected_size ||
        byte_size > it->second.expected_size - offset) return 0;
    return reinterpret_cast<uintptr_t>(it->second.bytes.data() + offset);
}

int RenderStream::markStreamingRange_(StreamingAsset& stream,
                                      uint32_t offset,
                                      uint32_t byte_size) {
    if (offset > stream.expected_size ||
        byte_size > stream.expected_size - offset) return 0;
    if (!byte_size) return 1;
    uint32_t merged_start = offset;
    uint32_t merged_end = offset + byte_size;
    uint32_t overlap = 0;
    size_t first = 0;
    while (first < stream.written_range_count &&
           stream.written_ranges[first].second < merged_start) ++first;
    size_t last = first;
    while (last < stream.written_range_count &&
           stream.written_ranges[last].first <= merged_end) {
      const auto range = stream.written_ranges[last];
      const uint32_t overlap_start = std::max(offset, range.first);
      const uint32_t overlap_end = std::min(offset + byte_size, range.second);
      if (overlap_end > overlap_start) overlap += overlap_end - overlap_start;
      merged_start = std::min(merged_start, range.first);
      merged_end = std::max(merged_end, range.second);
      ++last;
    }
    if (first == last &&
        stream.written_range_count == StreamingAsset::kMaxWrittenRanges) {
      error_ = "Streaming asset has too many disjoint written ranges";
      return -2;
    }
    const size_t removed = last - first;
    const size_t new_count = stream.written_range_count - removed + 1;
    if (removed == 0) {
      for (size_t i = stream.written_range_count; i > first; --i)
        stream.written_ranges[i] = stream.written_ranges[i - 1];
    } else {
      for (size_t src = last, dst = first + 1;
           src < stream.written_range_count; ++src, ++dst)
        stream.written_ranges[dst] = stream.written_ranges[src];
    }
    stream.written_ranges[first] = {merged_start, merged_end};
    stream.written_range_count = new_count;
    stream.written_size += byte_size - overlap;
    return 1;
}

int RenderStream::markStreamingAssetBytesWritten(const uint8_t* name,
                                                 uint32_t name_size,
                                                 uint32_t byte_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        byte_size > it->second.expected_size - it->second.cursor) return 0;
    const int marked = markStreamingRange_(it->second, it->second.cursor, byte_size);
    if (marked != 1) return marked;
    it->second.cursor += byte_size;
    return 1;
}

int RenderStream::markStreamingAssetRangeWritten(const uint8_t* name,
                                                 uint32_t name_size,
                                                 uint32_t offset,
                                                 uint32_t byte_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end()) return 0;
    return markStreamingRange_(it->second, offset, byte_size);
}

int RenderStream::finalizeStreamingAsset(const uint8_t* name, uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size != it->second.expected_size) return 0;
    clip_assets_[key] = std::move(it->second.bytes);
    invalidateCopyPreparation_();
    clip_asset_budget_reservations_[key] = std::move(it->second.budget_reservation);
    streaming_assets_.erase(it);
    return 1;
}

int RenderStream::finalizeStreamingAssetToStore(const uint8_t* name,
                                                uint32_t name_size,
                                                NextAssetStore& store) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end() ||
        it->second.written_size != it->second.expected_size) return 0;
    if (it->second.expected_size > remainingMemoryLimitBytes()) {
      error_ = "Finalizing the stream requires additional resident memory";
      return -2;
    }
    std::vector<uint8_t> bytes(it->second.bytes.begin(), it->second.bytes.end());
    std::shared_ptr<void> reservation;
    if (payload_budget_ == store.payloadBudget())
      reservation = it->second.budget_reservation;
    const int status = store.adoptStreamingAsset(key, std::move(bytes),
                                                 it->second.uuid,
                                                 std::move(reservation));
    if (status != 0) {
      error_ = status == -2 ? "Asset store memory limit exceeded"
          : status == -4 ? "Asset store metadata limit exceeded"
                         : "Unable to transfer streamed asset";
      return status;
    }
    streaming_assets_.erase(it);
    error_.clear();
    return 1;
}

int RenderStream::cancelStreamingAsset(const uint8_t* name, uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    return streaming_assets_.erase(key) ? 1 : 0;
}

int RenderStream::beginStreamingAssetAsRoot(const uint8_t* name,
                                            uint32_t name_size) {
    if (!name || !name_size) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = streaming_assets_.find(key);
    if (it == streaming_assets_.end()) {
      error_ = "Streaming root asset not found";
      return 0;
    }
    if (it->second.written_size != it->second.expected_size) {
      error_ = "Streaming root asset is incomplete";
      return 0;
    }
    if (it->second.expected_size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    std::string root = std::move(it->second.bytes);
    streaming_assets_.erase(it);
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = root.size();
    return beginOwned(std::move(root)) ? 1 : 0;
}

int RenderStream::setProvidedAssetByteLimit(uint32_t limit) {
    if (limit != 0) {
      const size_t used = providedAssetMemoryBytes();
      if (used > limit) return -1;
    }
    provided_asset_byte_limit_ = limit;
    return 0;
}

int RenderStream::removeAssetBytes(const uint8_t* name, uint32_t name_size) {
    if (!name || name_size == 0) return -1;
    std::string key(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const bool removed = clip_assets_.erase(key) != 0;
    if (removed) invalidateCopyPreparation_();
    clip_asset_budget_reservations_.erase(key);
    return removed ? 1 : 0;
}

int RenderStream::providedAssetCount() const {
    return clip_assets_.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(clip_assets_.size());
}

int RenderStream::providedAssetNameCopy(int asset_id, uint8_t* out,
                                        uint32_t cap) const {
    if (asset_id < 0 || static_cast<size_t>(asset_id) >= clip_assets_.size()) return -1;
    auto it = clip_assets_.begin();
    std::advance(it, asset_id);
    const std::string& name = it->first;
    if (name.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(name.size());
    if (!out || cap < name.size()) return required;
    if (required) std::memcpy(out, name.data(), name.size());
    return required;
}

int RenderStream::providedAssetBytesCopy(const uint8_t* name,
                                         uint32_t name_size, uint8_t* out,
                                         uint32_t cap) const {
    if (!name && name_size) return -1;
    std::string key;
    if (name_size) key.assign(reinterpret_cast<const char*>(name), name_size);
    key = NormalizeRenderAssetKey(key);
    const auto it = clip_assets_.find(key);
    if (it == clip_assets_.end() ||
        it->second.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
      return -1;
    }
    const int required = static_cast<int>(it->second.size());
    if (!out || cap < it->second.size()) return required;
    if (required) std::memcpy(out, it->second.data(), it->second.size());
    return required;
}

size_t RenderStream::providedAssetMemoryBytes() const {
    size_t used = imported_asset_bytes_;
    for (const auto& asset : clip_assets_) {
      if (asset.first.size() > std::numeric_limits<size_t>::max() - asset.second.size() ||
          used > std::numeric_limits<size_t>::max() - asset.first.size() - asset.second.size()) {
        return std::numeric_limits<size_t>::max();
      }
      used += asset.first.size() + asset.second.size();
    }
    for (const auto& asset : streaming_assets_) {
      if (asset.first.size() > std::numeric_limits<size_t>::max() - asset.second.expected_size ||
          used > std::numeric_limits<size_t>::max() - asset.first.size() - asset.second.expected_size) {
        return std::numeric_limits<size_t>::max();
      }
      used += asset.first.size() + asset.second.expected_size;
    }
    return used;
}

size_t RenderStream::streamingRangeBookkeepingBytes_() const {
    const size_t unit = sizeof(StreamingAsset::written_ranges);
    if (streaming_assets_.size() > std::numeric_limits<size_t>::max() / unit)
      return std::numeric_limits<size_t>::max();
    return streaming_assets_.size() * unit;
}

}  // namespace web_next
}  // namespace lightusd
