#pragma once

#include <string>

namespace lightusd {
namespace tydra {

// forward decl.
enum class NodeCategory;
enum class NodeType;
enum class ComponentType;
enum class VertexAttributeFormat;
enum class VertexVariability;
enum class ColorSpace;
enum class UVReaderFloatComponentType;

// Forward declare UVTexture
// Note: We cannot forward declare nested types, so these function declarations
// are only available when render-data.hh is included (which defines UVTexture)
struct UVTexture;

// to_string functions for various enum types
std::string to_string(VertexVariability variability);
std::string to_string(NodeCategory category);
std::string to_string(NodeType ntype);
std::string to_string(ComponentType ty);
std::string to_string(VertexAttributeFormat f);
std::string to_string(ColorSpace cs);
std::string to_string(UVReaderFloatComponentType ty);

// These functions require the full UVTexture definition from render-data.hh
// They are implemented in render-data-pprint.cc
// std::string to_string(UVTexture::WrapMode ty);
// std::string to_string(const UVTexture::Channel channel);

} // namespace tydra
} // namespace lightusd
