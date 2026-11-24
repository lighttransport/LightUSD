// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.

#include "common-utils.hh"
#include <cstring>
#include <algorithm>
#include <limits>
#include "str-util.hh"
#include "tiny-format.hh"
#include "core/safe-arithmetic.hh"

namespace lightusd { namespace legacy {
namespace tydra {
namespace utils {

//
// Data conversion utilities implementations
//

template <typename T>
nonstd::expected<std::vector<T>, std::string> UniformToFaceVarying(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexCounts) {
  std::vector<T> dst;

  if (inputs.size() != faceVertexCounts.size()) {
    return nonstd::make_unexpected(
        fmt::format("The number of inputs {} must be the same with "
                    "faceVertexCounts.size() {}",
                    inputs.size(), faceVertexCounts.size()));
  }

  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    size_t cnt = faceVertexCounts[i];

    // Repeat cnt times
    for (size_t j = 0; j < cnt; j++) {
      dst.push_back(inputs[i]);
    }
  }

  return std::move(dst);
}

template <typename T>
nonstd::expected<std::vector<T>, std::string> UniformToVertex(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices) {

  if (inputs.size() != faceVertexCounts.size()) {
    return nonstd::make_unexpected(
        "inputs.size must be equal to faceVertexCounts.size");
  }

  // First pass: find maximum vertex index
  uint32_t maxVertexIndex = 0;
  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    maxVertexIndex = (std::max)(maxVertexIndex, faceVertexIndices[i]);
  }

  size_t numVertices = size_t(maxVertexIndex) + 1;
  std::vector<T> dst(numVertices);

  // Second pass: assign uniform values to vertices
  size_t faceVertexIndexOffset = 0;
  for (size_t faceId = 0; faceId < faceVertexCounts.size(); faceId++) {
    uint32_t faceVertexCount = faceVertexCounts[faceId];

    for (uint32_t v = 0; v < faceVertexCount; v++) {
      size_t faceVertexIndexId = faceVertexIndexOffset + v;
      if (faceVertexIndexId >= faceVertexIndices.size()) {
        return nonstd::make_unexpected("Invalid face vertex index access");
      }

      uint32_t vertexIndex = faceVertexIndices[faceVertexIndexId];
      if (vertexIndex >= numVertices) {
        return nonstd::make_unexpected("Vertex index out of bounds");
      }

      dst[vertexIndex] = inputs[faceId];
    }

    faceVertexIndexOffset += faceVertexCount;
  }

  return std::move(dst);
}

template <typename T>
nonstd::expected<std::vector<T>, std::string> VertexToFaceVarying(
    const std::vector<T> &inputs,
    const std::vector<uint32_t> &faceVertexIndices) {

  std::vector<T> dst;
  dst.reserve(faceVertexIndices.size());

  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    uint32_t vertexIndex = faceVertexIndices[i];

    if (vertexIndex >= inputs.size()) {
      return nonstd::make_unexpected(
          fmt::format("Vertex index {} is out of bounds (input size: {})",
                      vertexIndex, inputs.size()));
    }

    dst.push_back(inputs[vertexIndex]);
  }

  return std::move(dst);
}

template <typename T>
nonstd::expected<std::vector<T>, std::string> ConstantToFaceVarying(
    const std::vector<T> &src,
    const std::vector<uint32_t> &faceVertexCounts) {

  if (src.empty()) {
    return nonstd::make_unexpected("Source data is empty");
  }

  std::vector<T> dst;

  // Calculate total face vertices
  size_t totalFaceVertices = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    totalFaceVertices += faceVertexCounts[i];
  }

  dst.reserve(totalFaceVertices);

  // Replicate constant value for each face vertex
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    uint32_t count = faceVertexCounts[i];
    for (uint32_t j = 0; j < count; j++) {
      dst.push_back(src[0]); // Use first element as constant
    }
  }

  return std::move(dst);
}

nonstd::expected<std::vector<uint8_t>, std::string> ConstantToVertex(
    const std::vector<uint8_t> &src,
    uint32_t elementSize,
    size_t numVertices) {

  if (src.empty()) {
    return nonstd::make_unexpected("Source data is empty");
  }

  if (src.size() < elementSize) {
    return nonstd::make_unexpected(
        fmt::format("Source size {} is less than element size {}",
                    src.size(), elementSize));
  }

  size_t dstBytes;
  if (!safe::mul(numVertices, size_t(elementSize), &dstBytes)) {
    return nonstd::make_unexpected(
        fmt::format("Integer overflow: {} * {} exceeds size_t range",
                    numVertices, elementSize));
  }

  std::vector<uint8_t> dst(dstBytes);

  for (size_t i = 0; i < numVertices; i++) {
    std::memcpy(dst.data() + i * elementSize, src.data(), elementSize);
  }

  return std::move(dst);
}

nonstd::expected<std::vector<uint8_t>, std::string> ConstantToFaceVarying(
    const std::vector<uint8_t> &src,
    uint32_t elementSize,
    const std::vector<uint32_t> &faceVertexCounts) {

  if (src.empty()) {
    return nonstd::make_unexpected("Source data is empty");
  }

  if (src.size() < elementSize) {
    return nonstd::make_unexpected(
        fmt::format("Source size {} is less than element size {}",
                    src.size(), elementSize));
  }

  // Calculate total face vertices
  size_t totalFaceVertices = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    totalFaceVertices += faceVertexCounts[i];
  }

  size_t dstBytes;
  if (!safe::mul(totalFaceVertices, size_t(elementSize), &dstBytes)) {
    return nonstd::make_unexpected(
        fmt::format("Integer overflow: totalFaceVertices {} * elementSize {}",
                    totalFaceVertices, elementSize));
  }

  std::vector<uint8_t> dst(dstBytes);

  for (size_t i = 0; i < totalFaceVertices; i++) {
    std::memcpy(dst.data() + i * elementSize, src.data(), elementSize);
  }

  return std::move(dst);
}

//
// Template specializations for common types
//
template nonstd::expected<std::vector<float>, std::string> UniformToFaceVarying(
    const std::vector<float>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float2>, std::string> UniformToFaceVarying(
    const std::vector<value::float2>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float3>, std::string> UniformToFaceVarying(
    const std::vector<value::float3>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<float>, std::string> UniformToVertex(
    const std::vector<float>&, const std::vector<uint32_t>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float2>, std::string> UniformToVertex(
    const std::vector<value::float2>&, const std::vector<uint32_t>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float3>, std::string> UniformToVertex(
    const std::vector<value::float3>&, const std::vector<uint32_t>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<float>, std::string> VertexToFaceVarying(
    const std::vector<float>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float2>, std::string> VertexToFaceVarying(
    const std::vector<value::float2>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float3>, std::string> VertexToFaceVarying(
    const std::vector<value::float3>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<float>, std::string> ConstantToFaceVarying(
    const std::vector<float>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float2>, std::string> ConstantToFaceVarying(
    const std::vector<value::float2>&, const std::vector<uint32_t>&);

template nonstd::expected<std::vector<value::float3>, std::string> ConstantToFaceVarying(
    const std::vector<value::float3>&, const std::vector<uint32_t>&);

//
// String and conversion utilities
//

std::string ChannelToString(int channel_value) {
  // This should map to UVTexture::Channel enum values
  switch (channel_value) {
    case 0: return "rgb";   // RGB
    case 1: return "r";     // R
    case 2: return "g";     // G
    case 3: return "b";     // B
    case 4: return "a";     // A
    default: return "[[InternalError. Invalid Channel]]";
  }
}

std::string SanitizeAssetPath(const std::string& path, bool allow_parent_refs) {
  if (path.empty()) {
    return {};
  }

  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');

  // Remember the leading slash(es): the split/rejoin below works on path
  // SEGMENTS and would otherwise turn an absolute "/a/b" into a relative "a/b"
  // (and a UNC "//host/share" into "host/share"), which then resolves against
  // the wrong base — or not at all.
  std::string root;
  if (normalized.size() >= 2 && normalized[0] == '/' && normalized[1] == '/') {
    root = "//";  // UNC
  } else if (!normalized.empty() && normalized[0] == '/') {
    root = "/";
  }

  std::vector<std::string> parts;
  parts.reserve(16);

  size_t begin = 0;
  while (begin <= normalized.size()) {
    size_t end = normalized.find('/', begin);
    std::string part = normalized.substr(begin, end - begin);

    if (!part.empty() && part != ".") {
      if (part == "..") {
        // Collapse "<seg>/.." lexically. A ".." that cannot pop a real
        // preceding segment escapes the anchoring root: reject it as a
        // path-traversal guard, unless the caller opted into parent-relative
        // paths, in which case preserve the leading ".." so the resolver
        // rebases it against its base dir / search paths (mirrors
        // security_policy::ValidateAndNormalizeAssetPath).
        if (!parts.empty() && parts.back() != "..") {
          parts.pop_back();
        } else if (allow_parent_refs) {
          parts.push_back("..");
        } else {
          return {};
        }
      } else {
        parts.push_back(std::move(part));
      }
    }

    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }

  std::string result = root;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) {
      result.push_back('/');
    }
    result += parts[i];
  }
  return result;
}

}  // namespace utils
}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd
