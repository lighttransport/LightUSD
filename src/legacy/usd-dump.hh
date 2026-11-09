// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.
//
// USD Layer/Stage inspection utilities (similar to pxrUSD's sdfdump)
//
#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace lightusd {

// Forward declarations
class Layer;
class Stage;

/// Value printing mode for inspection
enum class InspectValueMode {
  NoValue,  ///< Schema only, no values printed
  Snip,     ///< First N items for arrays (configurable)
  Full      ///< All values printed
};

/// Output format for inspection
enum class InspectOutputFormat {
  Yaml,  ///< Human-readable YAML-like tree format
  Json   ///< Machine-readable JSON format
};

/// Options for USD Layer/Stage inspection
struct InspectOptions {
  InspectOutputFormat format{InspectOutputFormat::Yaml};
  InspectValueMode value_mode{InspectValueMode::Snip};
  size_t snip_count{8};  ///< Number of items to show in snip mode

  // Filtering
  std::string prim_path_pattern;  ///< Glob pattern for prim paths (empty = all)
  std::string attr_pattern;       ///< Glob pattern for attributes (empty = all)

  // TimeSamples filtering
  bool has_time_query{false};
  double time_start{0.0};  ///< Single time or range start
  double time_end{0.0};    ///< Range end (if != start, it's a range)

  // Formatting
  uint32_t indent_width{2};  ///< Spaces per indent level

  InspectOptions() = default;
};

///
/// Inspect Layer structure (low-level, before reconstruction).
/// Outputs a YAML-like tree representation of the Layer.
///
/// @param[in] layer Layer to inspect
/// @param[in] opts Inspection options
/// @return String containing the inspection output
///
std::string InspectLayer(const Layer &layer, const InspectOptions &opts = InspectOptions());

///
/// Inspect Stage structure (high-level, reconstructed).
/// Outputs a YAML-like tree representation of the Stage.
///
/// @param[in] stage Stage to inspect
/// @param[in] opts Inspection options
/// @return String containing the inspection output
///
std::string InspectStage(const Stage &stage, const InspectOptions &opts = InspectOptions());

}  // namespace lightusd
