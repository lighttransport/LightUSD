// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.
#pragma once

#include <string>
#include <vector>
#include "nonstd/expected.hpp"
#include "../value-types.hh"

namespace lightusd {
// Forward declarations
template<typename T> struct Animatable;
}  // namespace lightusd

namespace lightusd {
namespace tydra {
namespace utils {

//
// Common error handling macro
//
#define TYDRA_PUSH_ERROR(err_ptr, msg) \
  if (err_ptr) {                       \
    (*(err_ptr)) += msg;               \
  }

//
// Data conversion utilities for vertex attributes
//

/// Convert vertex attribute with Uniform variability to facevarying variability
template <typename T>
nonstd::expected<std::vector<T>, std::string> UniformToFaceVarying(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexCounts);

/// Convert vertex attribute with Uniform variability to vertex variability
template <typename T>
nonstd::expected<std::vector<T>, std::string> UniformToVertex(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices);

/// Convert vertex attribute from vertex variability to facevarying variability
template <typename T>
nonstd::expected<std::vector<T>, std::string> VertexToFaceVarying(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexIndices);

/// Convert constant attribute to facevarying variability
template <typename T>
nonstd::expected<std::vector<T>, std::string> ConstantToFaceVarying(
    const std::vector<T> &src,
    const std::vector<uint32_t> &faceVertexCounts);

/// Convert constant attribute to vertex variability
nonstd::expected<std::vector<uint8_t>, std::string> ConstantToVertex(
    const std::vector<uint8_t> &src,
    uint32_t elementSize,
    size_t numVertices);

/// Convert constant attribute to facevarying for byte data
nonstd::expected<std::vector<uint8_t>, std::string> ConstantToFaceVarying(
    const std::vector<uint8_t> &src,
    uint32_t elementSize,
    const std::vector<uint32_t> &faceVertexCounts);

//
// Vertex similarity and optimization utilities
//

/// Try to convert facevarying data to vertex data by checking vertex similarity
/// Returns true if conversion was successful
template <typename T>
bool TryConvertFacevaryingToVertex(
    const std::vector<T> &facevarying_data,
    const std::vector<uint32_t> &face_vertex_indices,
    std::vector<T> *vertex_data,
    std::vector<uint32_t> *vertex_indices,
    float epsilon = 0.0f);

//
// Memory management utilities
//

/// Move vector data and clear source to save memory
template<typename T>
void MoveAndClearVector(std::vector<T>& src, std::vector<T>& dst) {
  dst = std::move(src);
  src.clear();
  src.shrink_to_fit();
}

//
// String and conversion utilities
//

/// Convert UVTexture channel enum to string
std::string ChannelToString(int channel_value);

/// Normalizes `path` to forward slashes and collapses `.` / `..` segments
/// against preceding components. Returns an empty string if the path
/// attempts to escape the root (more `..` than prior segments).
/// Normalize + guard an asset path against path traversal.
///
/// Collapses `.` and `<seg>/..` segments. A `..` that would escape the
/// anchoring root (no real preceding segment to pop) is rejected (returns
/// empty) unless `allow_parent_refs` is true, in which case the leading `..`
/// is preserved for the asset resolver to rebase. Callers typically pass
/// `assetResolver.get_allow_parent_relative_paths()`.
std::string SanitizeAssetPath(const std::string& path,
                              bool allow_parent_refs = false);

}  // namespace utils
}  // namespace tydra
}  // namespace lightusd

