// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace lightusd {
namespace tydra {
namespace common {

//
// Common configuration structures shared between converters
//

/// Memory management configuration
struct MemoryConfig {
  bool lowmem{false};                     ///< Free source data after conversion for low memory usage
  size_t max_memory_limit_mb{0};          ///< Maximum memory limit in MB (0 = no limit)
  bool track_memory_usage{false};         ///< Track and report memory usage
};

/// Asset path configuration
struct AssetConfig {
  bool allow_backslash_in_asset_path{true};    ///< Allow Windows-style backslashes in asset paths
  bool allow_missing_asset{true};              ///< Allow missing asset files
  bool allow_texture_load_failure{true};       ///< Allow texture loading failures
};

/// Vertex attribute processing configuration
struct VertexProcessingConfig {
  bool build_vertex_indices{true};             ///< Build vertex indices when converting to vertex variability
  bool prefer_non_indexed{false};              ///< Prefer non-indexed data when mesh isn't single indexable
  bool triangulate{true};                      ///< Triangulate polygonal faces
  bool validate_geomsubset{true};             ///< Validate GeomSubset data

  float facevarying_to_vertex_eps{1e-6f};     ///< Epsilon for vertex similarity comparison
  uint32_t max_skin_elementSize{1024 * 256};   ///< Maximum skin weights per vertex
};

/// Geometry computation configuration
struct GeometryComputeConfig {
  bool compute_normals{true};                   ///< Compute normals if not present
  bool compute_tangents_and_binormals{true};   ///< Compute tangent frame for normal mapping

  std::string default_texcoords_primvar_name{"st"};      ///< Default texture coordinate primvar name
  std::string default_texcoords1_primvar_name{"st1"};    ///< Default secondary texture coordinate primvar name
  std::string default_tangents_primvar_name{"tangents"}; ///< Default tangents primvar name
  std::string default_binormals_primvar_name{"binormals"}; ///< Default binormals primvar name
};

/// Common conversion options used by both render-data and layer-to-renderscene converters
struct CommonConverterConfig {
  MemoryConfig memory;
  AssetConfig assets;
  VertexProcessingConfig vertex_processing;
  GeometryComputeConfig geometry_compute;

  bool verbose{false};                          ///< Enable verbose logging
  std::string warn_handler_userdata_name;       ///< Name for warning handler user data
  std::string error_handler_userdata_name;      ///< Name for error handler user data
};

//
// Common result structures
//

/// Conversion statistics and metrics
struct ConversionStats {
  size_t bytes_allocated{0};
  size_t bytes_freed{0};
  size_t peak_memory_usage{0};

  size_t meshes_processed{0};
  size_t materials_processed{0};
  size_t nodes_processed{0};
  size_t textures_loaded{0};

  double conversion_time_ms{0.0};
};

/// Common result structure for conversion operations
struct ConversionResult {
  bool success{false};
  std::string error_message;
  std::vector<std::string> warnings;
  ConversionStats stats;
};

//
// Common utility structures
//

/// Simple pair for tracking string-to-ID mappings
template<typename IdType>
struct StringIdPair {
  std::string name;
  IdType id;

  StringIdPair() = default;
  StringIdPair(const std::string& n, IdType i) : name(n), id(i) {}
};

/// Memory tracking helper
class MemoryTracker {
public:
  void track_allocation(size_t bytes) {
    bytes_allocated_ += bytes;
    current_usage_ += bytes;
    peak_usage_ = std::max(peak_usage_, current_usage_);
  }

  void track_deallocation(size_t bytes) {
    bytes_freed_ += bytes;
    current_usage_ = (current_usage_ >= bytes) ? (current_usage_ - bytes) : 0;
  }

  size_t bytes_allocated() const { return bytes_allocated_; }
  size_t bytes_freed() const { return bytes_freed_; }
  size_t current_usage() const { return current_usage_; }
  size_t peak_usage() const { return peak_usage_; }

  void reset() {
    bytes_allocated_ = bytes_freed_ = current_usage_ = peak_usage_ = 0;
  }

private:
  size_t bytes_allocated_{0};
  size_t bytes_freed_{0};
  size_t current_usage_{0};
  size_t peak_usage_{0};
};

}  // namespace common
}  // namespace tydra
}  // namespace lightusd

