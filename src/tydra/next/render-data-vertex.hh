// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-enums.hh"
#include "chunked-array.hh"
#include <string>

namespace lightusd { namespace tydra { namespace next {

//
// Stores raw vertex data with format and interpolation info
//
struct VertexAttribute {
  std::string name;
  VertexFormat format = VertexFormat::Vec3;
  Interpolation interpolation = Interpolation::Vertex;

  // Raw data storage (format-dependent)
  FloatChunked float_data;     // For Float, Vec2, Vec3, Vec4
  Int32Chunked int_data;       // For Int, IVec2, IVec3, IVec4
  UInt32Chunked uint_data;     // For UInt, UVec2, UVec3, UVec4

  // Optional index buffer (for indexed attributes)
  UInt32Chunked indices;

  bool has_indices() const { return !indices.empty(); }

  size_t element_count() const {
    switch (format) {
      case VertexFormat::Float:
        return float_data.size();
      case VertexFormat::Int:
        return int_data.size();
      case VertexFormat::UInt:
        return uint_data.size();
      case VertexFormat::Vec2:
        return float_data.size() / 2;
      case VertexFormat::IVec2:
        return int_data.size() / 2;
      case VertexFormat::UVec2:
        return uint_data.size() / 2;
      case VertexFormat::Vec3:
        return float_data.size() / 3;
      case VertexFormat::IVec3:
        return int_data.size() / 3;
      case VertexFormat::UVec3:
        return uint_data.size() / 3;
      case VertexFormat::Vec4:
        return float_data.size() / 4;
      case VertexFormat::IVec4:
        return int_data.size() / 4;
      case VertexFormat::UVec4:
        return uint_data.size() / 4;
      case VertexFormat::Matrix33:
        return float_data.size() / 9;
      case VertexFormat::Matrix44:
        return float_data.size() / 16;
    }
    return 0;
  }

  size_t memory_usage() const {
    return float_data.memory_usage() + int_data.memory_usage() +
           uint_data.memory_usage() + indices.memory_usage();
  }
};

}}} // namespace lightusd::tydra::next
