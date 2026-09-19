// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../security-policy.hh"

namespace lightusd {
namespace next {

// Controls assumptions that cannot be expressed by a byte/count ceiling.
// Untrusted is deliberately the default for every public next entry point.
enum class InputPolicy : uint8_t {
  Untrusted = 0,
  Trusted,
};

// Common limits shared by loading, composition, and render conversion.
//
// Zero is invalid rather than an alias for "unlimited".  This avoids the
// security-sensitive ambiguity where a missing value silently disables a
// guard. Call Unlimited() when a trusted application intentionally wants to
// remove these shared ceilings. Format-specific structural guards remain
// independently configurable on their lower-level option structs.
struct ResourceLimits {
  static constexpr size_t kDefaultResidentBytes = size_t(1) << 30;
  static constexpr size_t kDefaultArrayElements = size_t(16) << 20;
  static constexpr size_t kDefaultRenderRecords = size_t(1) << 20;

  size_t max_input_bytes = security_policy::kDefaultInputLimitBytes;
  size_t max_asset_bytes = security_policy::kDefaultAssetLimitBytes;
  size_t max_resident_bytes = kDefaultResidentBytes;
  size_t max_array_elements = kDefaultArrayElements;
  size_t max_archive_entries = security_policy::kDefaultArchiveEntryCount;
  size_t max_render_records = kDefaultRenderRecords;
  size_t max_parse_depth = 256;
  size_t max_composition_depth = 256;
  size_t max_namespace_depth = 1024;
  size_t max_value_clip_samples = 10000;

  bool valid() const {
    return max_input_bytes && max_asset_bytes && max_resident_bytes &&
           max_array_elements && max_archive_entries && max_render_records &&
           max_parse_depth && max_composition_depth && max_namespace_depth &&
           max_value_clip_samples;
  }

  static ResourceLimits Unlimited() {
    ResourceLimits limits;
    const size_t unlimited = (std::numeric_limits<size_t>::max)();
    limits.max_input_bytes = unlimited;
    limits.max_asset_bytes = unlimited;
    limits.max_resident_bytes = unlimited;
    limits.max_array_elements = unlimited;
    limits.max_archive_entries = unlimited;
    limits.max_render_records = unlimited;
    limits.max_parse_depth = unlimited;
    limits.max_composition_depth = unlimited;
    limits.max_namespace_depth = unlimited;
    limits.max_value_clip_samples = unlimited;
    return limits;
  }
};

}  // namespace next
}  // namespace lightusd
