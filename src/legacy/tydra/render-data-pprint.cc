// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//

#include <algorithm>
#include <cmath>

#include "tydra/render-data-pprint.hh"
#include "tydra/render-data.hh"

namespace lightusd {

namespace tydra {

std::string to_string(const UVTexture::Channel channel) {
  if (channel == UVTexture::Channel::RGB) {
    return "rgb";
  } else if (channel == UVTexture::Channel::R) {
    return "r";
  } else if (channel == UVTexture::Channel::G) {
    return "g";
  } else if (channel == UVTexture::Channel::B) {
    return "b";
  } else if (channel == UVTexture::Channel::A) {
    return "a";
  }

  return "[[InternalError. Invalid UVTexture::Channel]]";
}

std::string to_string(ColorSpace cty) {
  std::string s;
  switch (cty) {
    case ColorSpace::sRGB: {
      s = "srgb";
      break;
    }
    case ColorSpace::Lin_sRGB: {
      s = "lin_srgb";
      break;
    }
    case ColorSpace::Raw: {
      s = "raw";
      break;
    }
    case ColorSpace::Rec709: {
      s = "rec709";
      break;
    }
    case ColorSpace::Lin_Rec709: {
      s = "lin_rec709";
      break;
    }
    case ColorSpace::g22_Rec709: {
      s = "g22_rec709";
      break;
    }
    case ColorSpace::g18_Rec709: {
      s = "g18_rec709";
      break;
    }
    case ColorSpace::sRGB_Texture: {
      s = "srgb_texture";
      break;
    }
    case ColorSpace::OCIO: {
      s = "ocio";
      break;
    }
    case ColorSpace::Lin_ACEScg: {
      s = "lin_acescg";
      break;
    }
    case ColorSpace::ACES2065_1: {
      s = "aces2065-1";
      break;
    }
    case ColorSpace::Lin_Rec2020: {
      s = "lin_rec2020";
      break;
    }
    case ColorSpace::Lin_DisplayP3: {
      s = "lin_displayp3";
      break;
    }
    case ColorSpace::sRGB_DisplayP3: {
      s = "srgb_displayp3";
      break;
    }
    case ColorSpace::Custom: {
      s = "custom";
      break;
    }
    case ColorSpace::Unknown: {
      s = "unknown";
      break;
    }
  }

  return s;
}

std::string to_string(NodeCategory category) {
  switch (category) {
    case NodeCategory::Group:    return "group";
    case NodeCategory::Geom:     return "geom";
    case NodeCategory::Light:    return "light";
    case NodeCategory::Camera:   return "camera";
    case NodeCategory::Material: return "material";
    case NodeCategory::Skeleton: return "skeleton";
  }
  return "???";
}

std::string to_string(NodeType ntype) {
  if (ntype == NodeType::Xform) {
    return "xform";
  } else if (ntype == NodeType::Mesh) {
    return "mesh";
  } else if (ntype == NodeType::Camera) {
    return "camera";
  } else if (ntype == NodeType::PointLight) {
    return "pointLight";
  } else if (ntype == NodeType::DirectionalLight) {
    return "directionalLight";
  } else if (ntype == NodeType::SkelRoot) {
    return "skelroot";
  } else if (ntype == NodeType::Skeleton) {
    return "skeleton";
  } else if (ntype == NodeType::EnvmapLight) {
    return "envmapLight";
  } else if (ntype == NodeType::RectLight) {
    return "rectLight";
  } else if (ntype == NodeType::DiskLight) {
    return "diskLight";
  } else if (ntype == NodeType::CylinderLight) {
    return "cylinderLight";
  } else if (ntype == NodeType::GeometryLight) {
    return "geometryLight";
  }
  return "???";
}

std::string to_string(ComponentType cty) {
  std::string s;
  switch (cty) {
    case ComponentType::UInt8: {
      s = "uint8";
      break;
    }
    case ComponentType::Int8: {
      s = "int8";
      break;
    }
    case ComponentType::UInt16: {
      s = "uint16";
      break;
    }
    case ComponentType::Int16: {
      s = "int16";
      break;
    }
    case ComponentType::UInt32: {
      s = "uint32";
      break;
    }
    case ComponentType::Int32: {
      s = "int32";
      break;
    }
    case ComponentType::Half: {
      s = "half";
      break;
    }
    case ComponentType::Float: {
      s = "float";
      break;
    }
    case ComponentType::Double: {
      s = "double";
      break;
    }
  }

  return s;
}

std::string to_string(UVTexture::WrapMode mode) {
  std::string s;
  switch (mode) {
    case UVTexture::WrapMode::REPEAT: {
      s = "repeat";
      break;
    }
    case UVTexture::WrapMode::CLAMP_TO_BORDER: {
      s = "clamp_to_border";
      break;
    }
    case UVTexture::WrapMode::CLAMP_TO_EDGE: {
      s = "clamp_to_edge";
      break;
    }
    case UVTexture::WrapMode::MIRROR: {
      s = "mirror";
      break;
    }
  }

  return s;
}

std::string to_string(VertexVariability v) {
  std::string s;

  switch (v) {
    case VertexVariability::Constant: {
      s = "constant";
      break;
    }
    case VertexVariability::Uniform: {
      s = "uniform";
      break;
    }
    case VertexVariability::Varying: {
      s = "varying";
      break;
    }
    case VertexVariability::Vertex: {
      s = "vertex";
      break;
    }
    case VertexVariability::FaceVarying: {
      s = "facevarying";
      break;
    }
    case VertexVariability::Indexed: {
      s = "indexed";
      break;
    }
  }

  return s;
}

std::string to_string(VertexAttributeFormat f) {
  std::string s;

  switch (f) {
    case VertexAttributeFormat::Bool: {
      s = "bool";
      break;
    }
    case VertexAttributeFormat::Char: {
      s = "int8";
      break;
    }
    case VertexAttributeFormat::Char2: {
      s = "int8x2";
      break;
    }
    case VertexAttributeFormat::Char3: {
      s = "int8x3";
      break;
    }
    case VertexAttributeFormat::Char4: {
      s = "int8x4";
      break;
    }
    case VertexAttributeFormat::Byte: {
      s = "uint8";
      break;
    }
    case VertexAttributeFormat::Byte2: {
      s = "uint8x2";
      break;
    }
    case VertexAttributeFormat::Byte3: {
      s = "uint8x3";
      break;
    }
    case VertexAttributeFormat::Byte4: {
      s = "uint8x4";
      break;
    }
    case VertexAttributeFormat::Short: {
      s = "int16";
      break;
    }
    case VertexAttributeFormat::Short2: {
      s = "int16x2";
      break;
    }
    case VertexAttributeFormat::Short3: {
      s = "int16x2";
      break;
    }
    case VertexAttributeFormat::Short4: {
      s = "int16x2";
      break;
    }
    case VertexAttributeFormat::Ushort: {
      s = "uint16";
      break;
    }
    case VertexAttributeFormat::Ushort2: {
      s = "uint16x2";
      break;
    }
    case VertexAttributeFormat::Ushort3: {
      s = "uint16x2";
      break;
    }
    case VertexAttributeFormat::Ushort4: {
      s = "uint16x2";
      break;
    }
    case VertexAttributeFormat::Half: {
      s = "half";
      break;
    }
    case VertexAttributeFormat::Half2: {
      s = "half2";
      break;
    }
    case VertexAttributeFormat::Half3: {
      s = "half3";
      break;
    }
    case VertexAttributeFormat::Half4: {
      s = "half4";
      break;
    }
    case VertexAttributeFormat::Float: {
      s = "float";
      break;
    }
    case VertexAttributeFormat::Vec2: {
      s = "float2";
      break;
    }
    case VertexAttributeFormat::Vec3: {
      s = "float3";
      break;
    }
    case VertexAttributeFormat::Vec4: {
      s = "float4";
      break;
    }
    case VertexAttributeFormat::Int: {
      s = "int";
      break;
    }
    case VertexAttributeFormat::Ivec2: {
      s = "int2";
      break;
    }
    case VertexAttributeFormat::Ivec3: {
      s = "int3";
      break;
    }
    case VertexAttributeFormat::Ivec4: {
      s = "int4";
      break;
    }
    case VertexAttributeFormat::Uint: {
      s = "uint";
      break;
    }
    case VertexAttributeFormat::Uvec2: {
      s = "uint2";
      break;
    }
    case VertexAttributeFormat::Uvec3: {
      s = "uint3";
      break;
    }
    case VertexAttributeFormat::Uvec4: {
      s = "uint4";
      break;
    }
    case VertexAttributeFormat::Double: {
      s = "double";
      break;
    }
    case VertexAttributeFormat::Dvec2: {
      s = "double2";
      break;
    }
    case VertexAttributeFormat::Dvec3: {
      s = "double3";
      break;
    }
    case VertexAttributeFormat::Dvec4: {
      s = "double4";
      break;
    }
    case VertexAttributeFormat::Mat2: {
      s = "mat2";
      break;
    }
    case VertexAttributeFormat::Mat3: {
      s = "mat3";
      break;
    }
    case VertexAttributeFormat::Mat4: {
      s = "mat4";
      break;
    }
    case VertexAttributeFormat::Dmat2: {
      s = "dmat2";
      break;
    }
    case VertexAttributeFormat::Dmat3: {
      s = "dmat3";
      break;
    }
    case VertexAttributeFormat::Dmat4: {
      s = "dmat4";
      break;
    }
  }

  return s;
}

std::string to_string(UVReaderFloatComponentType ty) {
  std::string s;
  switch (ty) {
    case UVReaderFloatComponentType::COMPONENT_FLOAT: {
      s = "float";
      break;
    }
    case UVReaderFloatComponentType::COMPONENT_FLOAT2: {
      s = "float2";
      break;
    }
    case UVReaderFloatComponentType::COMPONENT_FLOAT3: {
      s = "float3";
      break;
    }
    case UVReaderFloatComponentType::COMPONENT_FLOAT4: {
      s = "float4";
      break;
    }
  }
  return s;
}

// ============================================================================
// LTE SpectralAPI String Conversion Implementations
// ============================================================================

std::string to_string(SpectralInterpolation interp) {
  switch (interp) {
    case SpectralInterpolation::Linear:
      return "linear";
    case SpectralInterpolation::Held:
      return "held";
    case SpectralInterpolation::Cubic:
      return "cubic";
    case SpectralInterpolation::Sellmeier:
      return "sellmeier";
  }
  return "linear";
}

std::string to_string(IlluminantPreset preset) {
  switch (preset) {
    case IlluminantPreset::None:
      return "none";
    case IlluminantPreset::A:
      return "a";
    case IlluminantPreset::D50:
      return "d50";
    case IlluminantPreset::D65:
      return "d65";
    case IlluminantPreset::E:
      return "e";
    case IlluminantPreset::F1:
      return "f1";
    case IlluminantPreset::F2:
      return "f2";
    case IlluminantPreset::F7:
      return "f7";
    case IlluminantPreset::F11:
      return "f11";
  }
  return "none";
}

std::string to_string(WavelengthUnit unit) {
  switch (unit) {
    case WavelengthUnit::Nanometers:
      return "nanometers";
    case WavelengthUnit::Micrometers:
      return "micrometers";
  }
  return "nanometers";
}

// ============================================================================
// LTE SpectralAPI Evaluate Implementations
// ============================================================================

float SpectralData::evaluate(float wavelength) const {
  if (samples.empty()) {
    return 0.0f;
  }

  // Convert wavelength to internal unit (nanometers)
  float wl = to_nanometers(wavelength);

  // Binary search for the interval containing wavelength
  if (wl <= samples[0][0]) {
    return samples[0][1];
  }
  if (wl >= samples.back()[0]) {
    return samples.back()[1];
  }

  // Find the interval
  size_t i = 0;
  for (; i < samples.size() - 1; ++i) {
    if (wl < samples[i + 1][0]) {
      break;
    }
  }

  float wl0 = samples[i][0];
  float wl1 = samples[i + 1][0];
  float v0 = samples[i][1];
  float v1 = samples[i + 1][1];

  switch (interpolation) {
    case SpectralInterpolation::Held:
      return v0;

    case SpectralInterpolation::Linear: {
      float t = (wl - wl0) / (wl1 - wl0);
      return v0 + t * (v1 - v0);
    }

    case SpectralInterpolation::Cubic: {
      // Simple cubic interpolation (Catmull-Rom style)
      // For proper cubic, we'd need more samples
      float t = (wl - wl0) / (wl1 - wl0);
      float t2 = t * t;
      float t3 = t2 * t;
      // Hermite basis (simplified, assumes tangent = 0 at endpoints)
      float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
      float h01 = -2.0f * t3 + 3.0f * t2;
      return h00 * v0 + h01 * v1;
    }

    case SpectralInterpolation::Sellmeier:
      // Sellmeier not applicable to generic spectral data
      return v0;
  }

  // Fallback (should not reach here)
  return v0;
}

float SpectralIOR::evaluate(float wavelength_nm) const {
  // Sellmeier equation
  if (interpolation == SpectralInterpolation::Sellmeier) {
    // Convert nm to um for Sellmeier equation
    float lambda_um = wavelength_nm / 1000.0f;
    float lambda2 = lambda_um * lambda_um;

    float n2 = 1.0f;
    n2 += (sellmeier_B1 * lambda2) / (lambda2 - sellmeier_C1);
    n2 += (sellmeier_B2 * lambda2) / (lambda2 - sellmeier_C2);
    n2 += (sellmeier_B3 * lambda2) / (lambda2 - sellmeier_C3);

    return std::sqrt(std::max(1.0f, n2));
  }

  // Use standard interpolation for sample-based IOR
  if (samples.empty()) {
    return 1.5f;  // Default IOR
  }

  float wl = wavelength_nm;
  if (unit == WavelengthUnit::Micrometers) {
    wl = wavelength_nm / 1000.0f;
  }

  if (wl <= samples[0][0]) {
    return samples[0][1];
  }
  if (wl >= samples.back()[0]) {
    return samples.back()[1];
  }

  // Find interval and interpolate
  size_t i = 0;
  for (; i < samples.size() - 1; ++i) {
    if (wl < samples[i + 1][0]) {
      break;
    }
  }

  float wl0 = samples[i][0];
  float wl1 = samples[i + 1][0];
  float v0 = samples[i][1];
  float v1 = samples[i + 1][1];

  switch (interpolation) {
    case SpectralInterpolation::Held:
      return v0;

    case SpectralInterpolation::Linear: {
      float t = (wl - wl0) / (wl1 - wl0);
      return v0 + t * (v1 - v0);
    }

    case SpectralInterpolation::Cubic: {
      float t = (wl - wl0) / (wl1 - wl0);
      float t2 = t * t;
      float t3 = t2 * t;
      float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
      float h01 = -2.0f * t3 + 3.0f * t2;
      return h00 * v0 + h01 * v1;
    }

    case SpectralInterpolation::Sellmeier:
      // Already handled above
      return 1.5f;
  }

  // Fallback (should not reach here)
  return 1.5f;
}

float SpectralEmission::evaluate(float wavelength_nm) const {
  // If using a preset, return a placeholder (actual SPD data should be
  // loaded from built-in tables in a real implementation)
  if (preset != IlluminantPreset::None && samples.empty()) {
    // Placeholder: return normalized value at D65 peak
    // Real implementation should use CIE standard illuminant tables
    return 1.0f;
  }

  if (samples.empty()) {
    return 0.0f;
  }

  float wl = wavelength_nm;
  if (unit == WavelengthUnit::Micrometers) {
    wl = wavelength_nm / 1000.0f;
  }

  if (wl <= samples[0][0]) {
    return samples[0][1];
  }
  if (wl >= samples.back()[0]) {
    return samples.back()[1];
  }

  // Find interval
  size_t i = 0;
  for (; i < samples.size() - 1; ++i) {
    if (wl < samples[i + 1][0]) {
      break;
    }
  }

  float wl0 = samples[i][0];
  float wl1 = samples[i + 1][0];
  float v0 = samples[i][1];
  float v1 = samples[i + 1][1];

  switch (interpolation) {
    case SpectralInterpolation::Held:
      return v0;

    case SpectralInterpolation::Linear: {
      float t = (wl - wl0) / (wl1 - wl0);
      return v0 + t * (v1 - v0);
    }

    case SpectralInterpolation::Cubic: {
      float t = (wl - wl0) / (wl1 - wl0);
      float t2 = t * t;
      float t3 = t2 * t;
      float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
      float h01 = -2.0f * t3 + 3.0f * t2;
      return h00 * v0 + h01 * v1;
    }

    case SpectralInterpolation::Sellmeier:
      // Not applicable to emission
      return v0;
  }

  // Fallback (should not reach here)
  return v0;
}

}  // namespace tydra
}  // namespace lightusd
