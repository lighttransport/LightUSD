// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../types/value.hh"
#include "../types/interpolation.hh"
#include "property-index.hh"

class ValueBlockStorage {
public:
  ValueBlockStorage();
  ~ValueBlockStorage();

  /// Store a value and return its offset
  uint32_t store(const Value& value);
  uint32_t store(Value&& value);
  void reserve(size_t count);

  /// Get value at offset
  const Value* get(uint32_t offset) const;
  Value* get(uint32_t offset);

  /// Number of stored values
  size_t count() const;

  /// Approximate memory usage in bytes
  size_t memory_usage() const;

  /// Clear all storage
  void clear();

  /// Free every stored value's heap payload (each element becomes an empty
  /// Value) while keeping offsets valid, so slots still resolve. AssetPath
  /// values are kept intact: post-write asset collection reads them after the
  /// crate bytes are emitted. Used by the crate writer's consume_values mode.
  void release_payloads();

private:
  std::vector<Value> values_;
};

/// TimeSampleBlock - stores time samples with value deduplication
/// Design goals:
/// - Deduplicate identical array values across time samples
/// - Use hash + equality check for efficient lookup
/// - Store (time, value_offset) pairs per property
class TimeSampleBlock {
public:
  TimeSampleBlock();
  ~TimeSampleBlock();

  /// Add a time sample for a property
  /// Returns the value offset (may be shared if duplicate)
  uint32_t add(PropNameId name_id, double time, Value value);

  /// Add a time sample with explicit deduplication check
  /// If an identical value exists, reuses its offset
  uint32_t add_dedup(PropNameId name_id, double time, Value value);

private:
  /// Keep each property's (time, offset) vector sorted by time (consumers —
  /// interpolation's FindBracket, the crate writer's times block — assume it),
  /// with last-wins upsert for a re-authored time.
  void insert_sample(PropNameId name_id, double time, uint32_t offset);

public:

  /// Get time samples for a property
  /// Returns vector of (time, value_offset) pairs, sorted by time
  const std::vector<std::pair<double, uint32_t>>* get(PropNameId name_id) const;

  /// Get value at offset
  const Value* value(uint32_t offset) const;

  /// Rewrite scalar asset path sample values.
  size_t remap_asset_paths(const std::map<std::string, std::string>& remap);

  /// Check if property has time samples
  bool has(PropNameId name_id) const;

  /// Remove all time samples for a property. Returns true if any existed.
  /// (Stored values remain in the value pool; they may be shared.)
  bool remove(PropNameId name_id);

  /// Remap every sample time t -> offset + scale * t (layer-offset baking).
  void remap_times(double offset, double scale);

  /// Get interpolated value at a given time
  /// @param name_id Property name ID
  /// @param time Time to sample at
  /// @param mode Interpolation mode (default: Linear)
  /// @return SampleResult with interpolated value
  SampleResult interpolate(PropNameId name_id, double time,
                           TimeInterpolation mode = TimeInterpolation::Linear) const;

  /// Get all property IDs with time samples
  std::vector<PropNameId> properties() const;
  /// Number of properties with time samples.
  size_t property_count() const { return samples_.size(); }

  /// Check if empty
  bool empty() const;

  /// Memory usage in bytes
  size_t memory_usage() const;

  /// Clear all storage
  void clear();

  /// Free every sample value's heap payload (each becomes an empty Value)
  /// while keeping the (time, offset) tables valid; also drops the dedup
  /// index. Used by the crate writer's consume_values mode.
  void release_payloads();

  /// Statistics
  struct Stats {
    size_t property_count;     // Properties with time samples
    size_t total_samples;      // Total (time, offset) pairs
    size_t unique_values;      // Unique values stored
    size_t dedup_count;        // Values deduplicated (savings)
    size_t memory_bytes;
  };
  Stats stats() const;

private:
  // Property -> vector of (time, value_offset)
  std::unordered_map<uint32_t, std::vector<std::pair<double, uint32_t>>> samples_;

  // Value storage
  std::vector<Value> values_;

  // Deduplication: hash -> list of (offset, hash) for collision handling
  std::unordered_map<uint64_t, std::vector<uint32_t>> hash_to_offsets_;

  // Stats
  size_t dedup_count_ = 0;

  // Find existing value or store new one
  uint32_t find_or_store(Value value);
};

/// PrimSpec - unified spec/prim representation
/// Design goals:
/// - No separate Prim type needed (PrimSpec IS the prim)
/// - Properties accessed via O(1) indexed lookup
/// - Values stored in contiguous memory
/// - Children stored as indices, not pointers

std::string NormalizeAssetPathForRemap(std::string path);
const std::string* FindAssetPathRemap(
    const std::map<std::string, std::string>& remap,
    const std::string& path);
