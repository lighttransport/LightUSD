// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment, Inc.
//
// Material serialization utilities for WASM/JS bindings

#include "material-serializer.hh"
#include <sstream>
#include <iomanip>

// Suppress warnings for external yyjson header
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#pragma clang diagnostic ignored "-Wreserved-identifier"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif

#include "external/yyjson.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {

namespace {

// Helper to convert vec3/array<float,3> to JSON array
std::string vec3ToJson(const std::array<float, 3>& vec) {
  std::stringstream ss;
  ss << "[" << vec[0] << ", " << vec[1] << ", " << vec[2] << "]";
  return ss.str();
}


// Serialize OpenPBRSurfaceShader to JSON
std::string serializeOpenPBRToJson(const OpenPBRSurfaceShader& shader, const RenderScene* renderScene = nullptr) {
  std::stringstream json;
  json << "{";
  json << "\"type\": \"OpenPBRSurfaceShader\",";

  // Helper function to convert ColorSpace enum to string
  auto colorSpaceToString = [](tydra::ColorSpace cs) -> const char* {
    switch (cs) {
      case tydra::ColorSpace::sRGB: return "sRGB";
      case tydra::ColorSpace::Lin_sRGB: return "lin_sRGB";
      case tydra::ColorSpace::Rec709: return "rec709";
      case tydra::ColorSpace::Lin_Rec709: return "lin_rec709";
      case tydra::ColorSpace::g22_Rec709: return "g22_rec709";
      case tydra::ColorSpace::g18_Rec709: return "g18_rec709";
      case tydra::ColorSpace::sRGB_Texture: return "srgb_texture";
      case tydra::ColorSpace::Raw: return "raw";
      case tydra::ColorSpace::Lin_ACEScg: return "lin_acescg";
      case tydra::ColorSpace::ACES2065_1: return "aces2065_1";
      case tydra::ColorSpace::Lin_Rec2020: return "lin_rec2020";
      case tydra::ColorSpace::OCIO: return "ocio";
      case tydra::ColorSpace::Lin_DisplayP3: return "lin_displayp3";
      case tydra::ColorSpace::sRGB_DisplayP3: return "srgb_displayp3";
      case tydra::ColorSpace::Custom: return "custom";
      case tydra::ColorSpace::Unknown: return "unknown";
    }
    return "unknown"; // fallback for any unhandled case
  };

  // Helper functions to serialize values based on type
  auto serializeValue = [&](const ShaderParam<float>& param) {
    json << param.value;
  };

  auto serializeVec3Value = [&](const ShaderParam<vec3>& param) {
    json << vec3ToJson(param.value);
  };

  // Generic lambda to serialize shader parameters
  auto serializeFloatParam = [&](const std::string& paramName, const ShaderParam<float>& param) {
    json << "\"" << paramName << "\": {";
    json << "\"name\": \"" << paramName << "\",";

    if (param.is_texture() && renderScene) {
      // Texture parameter
      int texId = param.texture_id;
      json << "\"type\": \"texture\",";
      json << "\"textureId\": " << texId;

      // Look up texture image information
      if (texId >= 0 && static_cast<size_t>(texId) < renderScene->textures.size()) {
        const auto& uvTexture = renderScene->textures[static_cast<size_t>(texId)];
        if (uvTexture.texture_image_id >= 0 && static_cast<size_t>(uvTexture.texture_image_id) < renderScene->images.size()) {
          const auto& image = renderScene->images[static_cast<size_t>(uvTexture.texture_image_id)];
          json << ", \"assetIdentifier\": \"" << image.asset_identifier << "\"";

          // Include texture metadata
          json << ", \"texture\": {";
          json << "\"width\": " << image.width << ",";
          json << "\"height\": " << image.height << ",";
          json << "\"channels\": " << image.channels << ",";
          json << "\"colorSpace\": \"" << colorSpaceToString(image.colorSpace) << "\",";
          json << "\"usdColorSpace\": \"" << colorSpaceToString(image.usdColorSpace) << "\",";
          json << "\"decoded\": " << (image.decoded ? "true" : "false");
          json << "}";
        }
      }
    } else {
      // Scalar parameter
      json << "\"type\": \"value\", \"value\": ";
      serializeValue(param);
    }
    json << "}";
  };

  auto serializeVec3Param = [&](const std::string& paramName, const ShaderParam<vec3>& param) {
    json << "\"" << paramName << "\": {";
    json << "\"name\": \"" << paramName << "\",";

    if (param.is_texture() && renderScene) {
      // Texture parameter
      int texId = param.texture_id;
      json << "\"type\": \"texture\",";
      json << "\"textureId\": " << texId;

      // Look up texture image information
      if (texId >= 0 && static_cast<size_t>(texId) < renderScene->textures.size()) {
        const auto& uvTexture = renderScene->textures[static_cast<size_t>(texId)];
        if (uvTexture.texture_image_id >= 0 && static_cast<size_t>(uvTexture.texture_image_id) < renderScene->images.size()) {
          const auto& image = renderScene->images[static_cast<size_t>(uvTexture.texture_image_id)];
          json << ", \"assetIdentifier\": \"" << image.asset_identifier << "\"";

          // Include texture metadata
          json << ", \"texture\": {";
          json << "\"width\": " << image.width << ",";
          json << "\"height\": " << image.height << ",";
          json << "\"channels\": " << image.channels << ",";
          json << "\"colorSpace\": \"" << colorSpaceToString(image.colorSpace) << "\",";
          json << "\"usdColorSpace\": \"" << colorSpaceToString(image.usdColorSpace) << "\",";
          json << "\"decoded\": " << (image.decoded ? "true" : "false");
          json << "}";
        }
      }
    } else {
      // Scalar parameter
      json << "\"type\": \"value\", \"value\": ";
      serializeVec3Value(param);
    }
    json << "}";
  };

  // Base layer
  json << "\"base\": {";
  serializeFloatParam("base_weight", shader.base_weight); json << ",";
  serializeVec3Param("base_color", shader.base_color); json << ",";
  serializeFloatParam("base_roughness", shader.base_roughness); json << ",";
  serializeFloatParam("base_metalness", shader.base_metalness); json << ",";
  serializeFloatParam("base_diffuse_roughness", shader.base_diffuse_roughness);
  json << "},";

  // Specular layer
  json << "\"specular\": {";
  serializeFloatParam("specular_weight", shader.specular_weight); json << ",";
  serializeVec3Param("specular_color", shader.specular_color); json << ",";
  serializeFloatParam("specular_roughness", shader.specular_roughness); json << ",";
  serializeFloatParam("specular_ior", shader.specular_ior); json << ",";
  serializeFloatParam("specular_anisotropy", shader.specular_anisotropy); json << ",";
  serializeFloatParam("specular_rotation", shader.specular_rotation);
  json << "},";

  // Transmission layer
  json << "\"transmission\": {";
  serializeFloatParam("transmission_weight", shader.transmission_weight); json << ",";
  serializeVec3Param("transmission_color", shader.transmission_color); json << ",";
  serializeFloatParam("transmission_depth", shader.transmission_depth); json << ",";
  serializeVec3Param("transmission_scatter", shader.transmission_scatter); json << ",";
  serializeFloatParam("transmission_scatter_anisotropy", shader.transmission_scatter_anisotropy); json << ",";
  serializeFloatParam("transmission_dispersion", shader.transmission_dispersion);
  json << "},";

  // Subsurface layer
  json << "\"subsurface\": {";
  serializeFloatParam("subsurface_weight", shader.subsurface_weight); json << ",";
  serializeVec3Param("subsurface_color", shader.subsurface_color); json << ",";
  serializeFloatParam("subsurface_scale", shader.subsurface_scale); json << ",";
  serializeFloatParam("subsurface_anisotropy", shader.subsurface_anisotropy);
  json << "},";

  // Sheen layer - fabric-like reflection
  json << "\"sheen\": {";
  serializeFloatParam("sheen_weight", shader.sheen_weight); json << ",";
  serializeVec3Param("sheen_color", shader.sheen_color); json << ",";
  serializeFloatParam("sheen_roughness", shader.sheen_roughness);
  json << "},";

  // Fuzz layer - velvet/fabric-like appearance
  json << "\"fuzz\": {";
  serializeFloatParam("fuzz_weight", shader.fuzz_weight); json << ",";
  serializeVec3Param("fuzz_color", shader.fuzz_color); json << ",";
  serializeFloatParam("fuzz_roughness", shader.fuzz_roughness);
  json << "},";

  // Thin film layer - iridescence from thin film interference
  json << "\"thin_film\": {";
  serializeFloatParam("thin_film_weight", shader.thin_film_weight); json << ",";
  serializeFloatParam("thin_film_thickness", shader.thin_film_thickness); json << ",";
  serializeFloatParam("thin_film_ior", shader.thin_film_ior);
  json << "},";

  // Coat layer
  json << "\"coat\": {";
  serializeFloatParam("coat_weight", shader.coat_weight); json << ",";
  serializeVec3Param("coat_color", shader.coat_color); json << ",";
  serializeFloatParam("coat_roughness", shader.coat_roughness); json << ",";
  serializeFloatParam("coat_anisotropy", shader.coat_anisotropy); json << ",";
  serializeFloatParam("coat_rotation", shader.coat_rotation); json << ",";
  serializeFloatParam("coat_ior", shader.coat_ior); json << ",";
  serializeFloatParam("coat_affect_color", shader.coat_affect_color); json << ",";
  serializeFloatParam("coat_affect_roughness", shader.coat_affect_roughness);
  json << "},";

  // Emission
  json << "\"emission\": {";
  serializeFloatParam("emission_luminance", shader.emission_luminance); json << ",";
  serializeVec3Param("emission_color", shader.emission_color);
  json << "},";

  // Geometry
  json << "\"geometry\": {";
  serializeFloatParam("opacity", shader.opacity); json << ",";
  serializeFloatParam("geometry_opacity", shader.opacity); json << ",";  // alias for Three.js alpha mapping
  serializeVec3Param("normal", shader.normal); json << ",";
  serializeVec3Param("tangent", shader.tangent); json << ",";
  // Tangent rotation for anisotropic materials (from ND_rotate3d_vector3 node)
  json << "\"tangent_rotation\": " << shader.tangent_rotation << ",";
  // Normal map scale factor (from ND_normalmap_float node)
  json << "\"normal_map_scale\": " << shader.normal_map_scale << ",";
  // Coat layer normal and tangent
  serializeVec3Param("coat_normal", shader.coat_normal); json << ",";
  serializeVec3Param("coat_tangent", shader.coat_tangent); json << ",";
  json << "\"coat_tangent_rotation\": " << shader.coat_tangent_rotation << ",";
  json << "\"coat_normal_map_scale\": " << shader.coat_normal_map_scale;
  json << "}";

  // Include NodeGraph JSON if available (for shader network reconstruction in JS/WASM)
  if (!shader.nodeGraphJson.empty()) {
    json << ",\"nodeGraph\": " << shader.nodeGraphJson;
  }

  json << "}";
  return json.str();
}

// Serialize PreviewSurfaceShader to JSON
std::string serializePreviewSurfaceToJson(const PreviewSurfaceShader& shader, const RenderScene* renderScene = nullptr) {
  (void)renderScene;  // Reserved for future use
  std::stringstream json;
  json << "{";
  json << "\"type\": \"PreviewSurfaceShader\",";

  json << "\"useSpecularWorkflow\": " << (shader.useSpecularWorkflow ? "true" : "false") << ",";
  json << "\"diffuseColor\": " << vec3ToJson(shader.diffuseColor.value) << ",";
  json << "\"emissiveColor\": " << vec3ToJson(shader.emissiveColor.value) << ",";
  json << "\"specularColor\": " << vec3ToJson(shader.specularColor.value) << ",";
  json << "\"metallic\": " << shader.metallic.value << ",";
  json << "\"roughness\": " << shader.roughness.value << ",";
  json << "\"clearcoat\": " << shader.clearcoat.value << ",";
  json << "\"clearcoatRoughness\": " << shader.clearcoatRoughness.value << ",";
  json << "\"opacity\": " << shader.opacity.value << ",";
  json << "\"opacityThreshold\": " << shader.opacityThreshold.value << ",";
  json << "\"ior\": " << shader.ior.value << ",";
  json << "\"normal\": " << vec3ToJson(shader.normal.value) << ",";
  json << "\"displacement\": " << shader.displacement.value << ",";
  json << "\"occlusion\": " << shader.occlusion.value;

  // Export texture IDs for diffuseColor, emissiveColor, specularColor, metallic, roughness, opacity
  if (shader.diffuseColor.is_texture()) {
    json << ",\"diffuseColorTextureId\": " << shader.diffuseColor.texture_id;
  }
  if (shader.emissiveColor.is_texture()) {
    json << ",\"emissiveColorTextureId\": " << shader.emissiveColor.texture_id;
  }
  if (shader.specularColor.is_texture()) {
    json << ",\"specularColorTextureId\": " << shader.specularColor.texture_id;
  }
  if (shader.metallic.is_texture()) {
    json << ",\"metallicTextureId\": " << shader.metallic.texture_id;
  }
  if (shader.roughness.is_texture()) {
    json << ",\"roughnessTextureId\": " << shader.roughness.texture_id;
  }
  if (shader.opacity.is_texture()) {
    json << ",\"opacityTextureId\": " << shader.opacity.texture_id;
  }

  // Export texture IDs for normal, occlusion, and displacement maps
  if (shader.normal.is_texture()) {
    json << ",\"normalTextureId\": " << shader.normal.texture_id;
  }
  if (shader.occlusion.is_texture()) {
    json << ",\"occlusionTextureId\": " << shader.occlusion.texture_id;
  }
  if (shader.displacement.is_texture()) {
    json << ",\"displacementTextureId\": " << shader.displacement.texture_id;
  }

  json << "}";
  return json.str();
}

// Serialize OpenPBRSurfaceShader to MaterialX XML
std::string serializeOpenPBRToXml(const OpenPBRSurfaceShader& shader) {
  std::stringstream xml;
  xml << "<?xml version=\"1.0\"?>\n";
  xml << "<materialx version=\"1.39\">\n";
  xml << "  <surfacematerial name=\"Material\" type=\"material\">\n";
  xml << "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"OpenPBR_Surface\" />\n";
  xml << "  </surfacematerial>\n";

  xml << "  <open_pbr_surface name=\"OpenPBR_Surface\" type=\"surfaceshader\">\n";

  // Base layer
  xml << "    <input name=\"base_weight\" type=\"float\" value=\"" << shader.base_weight.value << "\" />\n";
  xml << "    <input name=\"base_color\" type=\"color3\" value=\""
      << shader.base_color.value[0] << ", "
      << shader.base_color.value[1] << ", "
      << shader.base_color.value[2] << "\" />\n";
  xml << "    <input name=\"base_roughness\" type=\"float\" value=\"" << shader.base_roughness.value << "\" />\n";
  xml << "    <input name=\"base_metalness\" type=\"float\" value=\"" << shader.base_metalness.value << "\" />\n";

  // Specular layer
  xml << "    <input name=\"specular_weight\" type=\"float\" value=\"" << shader.specular_weight.value << "\" />\n";
  xml << "    <input name=\"specular_color\" type=\"color3\" value=\""
      << shader.specular_color.value[0] << ", "
      << shader.specular_color.value[1] << ", "
      << shader.specular_color.value[2] << "\" />\n";
  xml << "    <input name=\"specular_roughness\" type=\"float\" value=\"" << shader.specular_roughness.value << "\" />\n";
  xml << "    <input name=\"specular_ior\" type=\"float\" value=\"" << shader.specular_ior.value << "\" />\n";

  // Add other significant properties...
  if (shader.coat_weight.value > 0.0f) {
    xml << "    <input name=\"coat_weight\" type=\"float\" value=\"" << shader.coat_weight.value << "\" />\n";
    xml << "    <input name=\"coat_roughness\" type=\"float\" value=\"" << shader.coat_roughness.value << "\" />\n";
  }

  if (shader.transmission_weight.value > 0.0f) {
    xml << "    <input name=\"transmission_weight\" type=\"float\" value=\"" << shader.transmission_weight.value << "\" />\n";
  }

  // Fuzz layer - velvet/fabric-like appearance
  if (shader.fuzz_weight.value > 0.0f) {
    xml << "    <input name=\"fuzz_weight\" type=\"float\" value=\"" << shader.fuzz_weight.value << "\" />\n";
    xml << "    <input name=\"fuzz_color\" type=\"color3\" value=\""
        << shader.fuzz_color.value[0] << ", "
        << shader.fuzz_color.value[1] << ", "
        << shader.fuzz_color.value[2] << "\" />\n";
    xml << "    <input name=\"fuzz_roughness\" type=\"float\" value=\"" << shader.fuzz_roughness.value << "\" />\n";
  }

  // Thin film layer - iridescence from thin film interference
  if (shader.thin_film_weight.value > 0.0f) {
    xml << "    <input name=\"thin_film_weight\" type=\"float\" value=\"" << shader.thin_film_weight.value << "\" />\n";
    xml << "    <input name=\"thin_film_thickness\" type=\"float\" value=\"" << shader.thin_film_thickness.value << "\" />\n";
    xml << "    <input name=\"thin_film_ior\" type=\"float\" value=\"" << shader.thin_film_ior.value << "\" />\n";
  }

  if (shader.emission_luminance.value > 0.0f) {
    xml << "    <input name=\"emission_luminance\" type=\"float\" value=\"" << shader.emission_luminance.value << "\" />\n";
    xml << "    <input name=\"emission_color\" type=\"color3\" value=\""
        << shader.emission_color.value[0] << ", "
        << shader.emission_color.value[1] << ", "
        << shader.emission_color.value[2] << "\" />\n";
  }

  xml << "  </open_pbr_surface>\n";
  xml << "</materialx>\n";

  return xml.str();
}

} // anonymous namespace

nonstd::expected<std::string, std::string> serializeMaterial(
    const RenderMaterial& material,
    SerializationFormat format,
    const RenderScene* renderScene) {

  if (format == SerializationFormat::JSON) {
    std::stringstream json;
    json << "{";
    json << "\"name\": \"" << material.name << "\",";
    json << "\"abs_path\": \"" << material.abs_path << "\",";
    json << "\"display_name\": \"" << material.display_name << "\",";

    // Flags for shader presence
    json << "\"hasUsdPreviewSurface\": " << (material.surfaceShader.has_value() ? "true" : "false") << ",";
    json << "\"hasOpenPBR\": " << (material.openPBRShader.has_value() ? "true" : "false");

    // Add shader data
    if (material.surfaceShader.has_value()) {
      json << ",\"surfaceShader\": " << serializePreviewSurfaceToJson(*material.surfaceShader, renderScene);
    }

    if (material.openPBRShader.has_value()) {
      json << ",\"openPBR\": " << serializeOpenPBRToJson(*material.openPBRShader, renderScene);
    }

    json << "}";
    return json.str();

  } else if (format == SerializationFormat::XML) {
    // For XML, prioritize MaterialX OpenPBR if available
    if (material.openPBRShader.has_value()) {
      return serializeOpenPBRToXml(*material.openPBRShader);
    } else if (material.surfaceShader.has_value()) {
      // Fallback: convert PreviewSurface to basic MaterialX
      std::stringstream xml;
      xml << "<?xml version=\"1.0\"?>\n";
      xml << "<materialx version=\"1.39\">\n";
      xml << "  <!-- UsdPreviewSurface material (OpenPBR not available) -->\n";
      xml << "  <surfacematerial name=\"" << (material.name.empty() ? "Material" : material.name) << "\" type=\"material\">\n";
      xml << "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"PreviewSurface\" />\n";
      xml << "  </surfacematerial>\n";

      const auto& shader = *material.surfaceShader;
      xml << "  <standard_surface name=\"PreviewSurface\" type=\"surfaceshader\">\n";
      xml << "    <input name=\"base_color\" type=\"color3\" value=\""
          << shader.diffuseColor.value[0] << ", "
          << shader.diffuseColor.value[1] << ", "
          << shader.diffuseColor.value[2] << "\" />\n";
      xml << "    <input name=\"metalness\" type=\"float\" value=\"" << shader.metallic.value << "\" />\n";
      xml << "    <input name=\"specular_roughness\" type=\"float\" value=\"" << shader.roughness.value << "\" />\n";
      xml << "  </standard_surface>\n";
      xml << "</materialx>\n";

      return xml.str();
    } else {
      return nonstd::make_unexpected("Material has no surface shader");
    }
  }

  return nonstd::make_unexpected("Unsupported serialization format");
}

nonstd::expected<std::string, std::string> serializeLight(
    const RenderLight& light,
    SerializationFormat format,
    const RenderScene* renderScene) {

  if (format == SerializationFormat::JSON) {
    std::stringstream json;
    json << "{";
    json << "\"name\": \"" << light.name << "\",";
    json << "\"abs_path\": \"" << light.abs_path << "\",";

    // Light type
    json << "\"type\": \"";
    switch (light.type) {
      case RenderLight::Type::Point:
        json << "Point";
        break;
      case RenderLight::Type::Sphere:
        json << "Sphere";
        break;
      case RenderLight::Type::Distant:
        json << "Distant";
        break;
      case RenderLight::Type::Rect:
        json << "Rect";
        break;
      case RenderLight::Type::Disk:
        json << "Disk";
        break;
      case RenderLight::Type::Cylinder:
        json << "Cylinder";
        break;
      case RenderLight::Type::Dome:
        json << "Dome";
        break;
      case RenderLight::Type::Geometry:
        json << "Geometry";
        break;
      case RenderLight::Type::Portal:
        json << "Portal";
        break;
    }
    json << "\",";

    // Common properties
    json << "\"color\": [" << light.color[0] << ", " << light.color[1] << ", " << light.color[2] << "],";
    json << "\"intensity\": " << light.intensity << ",";
    json << "\"exposure\": " << light.exposure << ",";
    json << "\"diffuse\": " << light.diffuse << ",";
    json << "\"specular\": " << light.specular << ",";
    json << "\"normalize\": " << (light.normalize ? "true" : "false") << ",";

    // Color temperature
    json << "\"enableColorTemperature\": " << (light.enableColorTemperature ? "true" : "false") << ",";
    json << "\"colorTemperature\": " << light.colorTemperature << ",";

    // Shadow properties
    json << "\"shadow\": {";
    json << "\"enable\": " << (light.shadowEnable ? "true" : "false") << ",";
    json << "\"color\": [" << light.shadowColor[0] << ", " << light.shadowColor[1] << ", " << light.shadowColor[2] << "],";
    json << "\"distance\": " << light.shadowDistance << ",";
    json << "\"falloff\": " << light.shadowFalloff << ",";
    json << "\"falloffGamma\": " << light.shadowFalloffGamma;
    json << "},";

    // Shaping properties (for Point/Rect lights with shaping)
    json << "\"shaping\": {";
    json << "\"focus\": " << light.shapingFocus << ",";
    json << "\"focusTint\": [" << light.shapingFocusTint[0] << ", " << light.shapingFocusTint[1] << ", " << light.shapingFocusTint[2] << "],";
    json << "\"coneAngle\": " << light.shapingConeAngle << ",";
    json << "\"coneSoftness\": " << light.shapingConeSoftness << ",";
    json << "\"iesFile\": \"" << light.shapingIesFile << "\",";
    json << "\"iesAngleScale\": " << light.shapingIesAngleScale << ",";
    json << "\"iesNormalize\": " << (light.shapingIesNormalize ? "true" : "false");
    json << "},";

    // Type-specific properties
    json << "\"properties\": {";

    switch (light.type) {
      case RenderLight::Type::Point:
      case RenderLight::Type::Sphere:
      case RenderLight::Type::Disk:
        json << "\"radius\": " << light.radius;
        break;

      case RenderLight::Type::Cylinder:
        json << "\"radius\": " << light.radius << ",";
        json << "\"length\": " << light.length;
        break;

      case RenderLight::Type::Rect:
        json << "\"width\": " << light.width << ",";
        json << "\"height\": " << light.height;
        if (!light.textureFile.empty()) {
          json << ",\"textureFile\": \"" << light.textureFile << "\"";
        }
        break;

      case RenderLight::Type::Distant:
        json << "\"angle\": " << light.angle;
        break;

      case RenderLight::Type::Dome:
        json << "\"textureFormat\": \"";
        switch (light.domeTextureFormat) {
          case RenderLight::DomeTextureFormat::Automatic:
            json << "automatic";
            break;
          case RenderLight::DomeTextureFormat::Latlong:
            json << "latlong";
            break;
          case RenderLight::DomeTextureFormat::MirroredBall:
            json << "mirroredBall";
            break;
          case RenderLight::DomeTextureFormat::Angular:
            json << "angular";
            break;
        }
        json << "\",";
        json << "\"guideRadius\": " << light.guideRadius;
        if (!light.textureFile.empty()) {
          json << ",\"textureFile\": \"" << light.textureFile << "\"";
        }
        break;

      case RenderLight::Type::Geometry:
        json << "\"geometryMeshId\": " << light.geometry_mesh_id << ",";
        json << "\"materialSyncMode\": \"" << light.material_sync_mode << "\",";
        json << "\"geometryTargetPath\": \"" << light.geometry_target_path << "\"";

        // Include mesh info if renderScene is provided
        if (renderScene && light.geometry_mesh_id >= 0 &&
            static_cast<size_t>(light.geometry_mesh_id) < renderScene->meshes.size()) {
          const auto& mesh = renderScene->meshes[static_cast<size_t>(light.geometry_mesh_id)];
          json << ",\"meshName\": \"" << mesh.prim_name << "\"";
          json << ",\"meshPath\": \"" << mesh.abs_path << "\"";
        }
        break;

      case RenderLight::Type::Portal:
        // Portal lights don't have special properties
        break;
    }

    json << "}";  // Close properties
    json << "}";  // Close root

    return json.str();

  } else if (format == SerializationFormat::XML) {
    // XML serialization for lights not implemented yet
    return nonstd::make_unexpected("XML serialization for lights not yet implemented");
  }

  return nonstd::make_unexpected("Unsupported serialization format");
}

} // namespace tydra
} // namespace lightusd
