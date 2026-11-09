// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
#endif
#include "threejs-exporter.hh"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <sstream>
#include <iomanip>
#include <cmath>
#include "str-util.hh"  // For dragonbox-based dtos()
#include "common-macros.inc"  // kMaxDefaultTraversalLimit (recursion depth guard)

namespace lightusd {
namespace tydra {

// Parameter mapping tables
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
const std::map<std::string, std::string>& MaterialParameterMapping::openpbr_to_physical() {
  static const std::map<std::string, std::string> mapping = {
    {"base_color", "color"},
    {"base_metalness", "metalness"},
    {"base_roughness", "roughness"},
    {"emission_color", "emissive"},
    {"emission_luminance", "emissiveIntensity"},
    {"opacity", "opacity"},
    {"coat_weight", "clearcoat"},
    {"coat_roughness", "clearcoatRoughness"},
    {"sheen_weight", "sheen"},
    {"sheen_color", "sheenColor"},
    {"sheen_roughness", "sheenRoughness"},
    {"specular_ior", "ior"},
    {"transmission_weight", "transmission"},
    {"base_weight", "opacity"}  // base_weight affects overall opacity
  };
  return mapping;
}

const std::map<std::string, std::string>& MaterialParameterMapping::openpbr_to_nodes() {
  static const std::map<std::string, std::string> mapping = {
    {"base_color", "base_color"},
    {"base_metalness", "metallic"},
    {"base_roughness", "roughness"},
    {"specular_weight", "specular"},
    {"specular_color", "specular_color"},
    {"specular_roughness", "specular_roughness"},
    {"specular_ior", "ior"},
    {"coat_weight", "coat"},
    {"coat_color", "coat_color"},
    {"coat_roughness", "coat_roughness"},
    {"emission_luminance", "emission"},
    {"emission_color", "emission_color"},
    {"normal", "normalMap"},
    {"tangent", "tangentMap"}
  };
  return mapping;
}

const std::map<std::string, std::string>& MaterialParameterMapping::preview_to_physical() {
  static const std::map<std::string, std::string> mapping = {
    {"diffuseColor", "color"},
    {"metallic", "metalness"},
    {"roughness", "roughness"},
    {"emissiveColor", "emissive"},
    {"opacity", "opacity"},
    {"clearcoat", "clearcoat"},
    {"clearcoatRoughness", "clearcoatRoughness"},
    {"ior", "ior"},
    {"specularColor", "specular"}
  };
  return mapping;
}

const std::map<std::string, std::string>& MaterialParameterMapping::colorspace_map() {
  static const std::map<std::string, std::string> mapping = {
    {"sRGB", "srgb"},
    {"lin_rec709", "linear-rec709"},
    {"lin_sRGB", "linear-srgb"},
    {"ACEScg", "acescg"},
    {"raw", "raw"}
  };
  return mapping;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// Helper function to convert vec3 to JSON array
static json vec3ToJson(const vec3& v) {
  return json::array({v[0], v[1], v[2]});
}

// Helper function to set a parameter in JSON with optional grouping
// For flattened: "base_color" stays as inputs["base_color"]
// For grouped: "base_color" becomes inputs["base"]["color"]
static void setJsonParameter(json& inputs, const std::string& param_name, const json& value, bool use_grouped) {
  if (!use_grouped) {
    // Flattened format: base_color
    inputs[param_name] = value;
  } else {
    // Grouped format: base.color
    size_t underscore_pos = param_name.find('_');
    if (underscore_pos != std::string::npos) {
      std::string group = param_name.substr(0, underscore_pos);
      std::string property = param_name.substr(underscore_pos + 1);

      // Create group object if it doesn't exist
      if (!inputs.contains(group)) {
        inputs[group] = json::object();
      }
      inputs[group][property] = value;
    } else {
      // No underscore, keep as-is
      inputs[param_name] = value;
    }
  }
}

// Helper function to convert vec2 to JSON array
// Static function that may not be used currently
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
static json vec2ToJson(const vec2& v) {
  return json::array({v[0], v[1]});
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// ThreeJSMaterialExporter implementation

bool ThreeJSMaterialExporter::ExportScene(const RenderScene& scene,
                                         const ExportOptions& options,
                                         json& output) {
  output = json::object();
  output["metadata"] = {
    {"version", "1.0"},
    {"generator", "LightUSD/Tydra ThreeJS Exporter"},
    {"type", "Scene"}
  };

  // Export materials
  json materials = json::array();
  for (const auto& mat : scene.materials) {
    json mat_json;
    if (ExportMaterial(mat, options, mat_json)) {
      materials.push_back(mat_json);
    }
  }
  output["materials"] = materials;

  // Export textures
  json textures = json::array();
  for (const auto& tex : scene.textures) {
    json tex_json = ConvertTextureReference(tex.handle, options);
    tex_json["name"] = tex.prim_name;
    textures.push_back(tex_json);
  }
  output["textures"] = textures;

  // Export meshes (geometry)
  json geometries = json::array();
  for (const auto& mesh : scene.meshes) {
    json geom = {
      {"name", mesh.prim_name},
      {"type", "BufferGeometry"},
      {"uuid", mesh.handle}
    };

    // Add vertex data
    json attributes = json::object();

    // Positions
    if (!mesh.points.empty()) {
      json positions = json::array();
      for (const auto& p : mesh.points) {
        positions.push_back(p[0]);
        positions.push_back(p[1]);
        positions.push_back(p[2]);
      }
      attributes["position"] = {
        {"array", positions},
        {"itemSize", 3},
        {"type", "Float32Array"}
      };
    }

    // Normals
    if (!mesh.normals.data.empty()) {
      json normals = json::array();
      // Convert normals based on variability
      if (mesh.normals.is_vertex()) {
        const vec3* normal_data = reinterpret_cast<const vec3*>(mesh.normals.get_data().data());
        for (size_t i = 0; i < mesh.normals.vertex_count(); ++i) {
          normals.push_back(normal_data[i][0]);
          normals.push_back(normal_data[i][1]);
          normals.push_back(normal_data[i][2]);
        }
      }
      attributes["normal"] = {
        {"array", normals},
        {"itemSize", 3},
        {"type", "Float32Array"}
      };
    }

    // UVs
    if (!mesh.texcoords.empty()) {
      auto it = mesh.texcoords.find(0);
      if (it != mesh.texcoords.end() && !it->second.data.empty()) {
        json uvs = json::array();
        const auto& texcoord = it->second;
        if (texcoord.is_vertex()) {
          const vec2* uv_data = reinterpret_cast<const vec2*>(texcoord.get_data().data());
          for (size_t i = 0; i < texcoord.vertex_count(); ++i) {
            uvs.push_back(uv_data[i][0]);
            uvs.push_back(uv_data[i][1]);
          }
        }
        attributes["uv"] = {
          {"array", uvs},
          {"itemSize", 2},
          {"type", "Float32Array"}
        };
      }
    }

    geom["attributes"] = attributes;

    // Add indices if available
    auto indices = mesh.faceVertexIndices();
    if (!indices.empty()) {
      json index_array = json::array();
      for (auto idx : indices) {
        index_array.push_back(idx);
      }
      geom["index"] = {
        {"array", index_array},
        {"type", indices.size() > 65535 ? "Uint32Array" : "Uint16Array"}
      };
    }

    geometries.push_back(geom);
  }
  output["geometries"] = geometries;

  return true;
}

bool ThreeJSMaterialExporter::ExportMaterial(const RenderMaterial& material,
                                            const ExportOptions& options,
                                            json& output) {
  output = json::object();

  // Basic material info
  output["name"] = material.name;
  output["uuid"] = std::to_string(material.handle);

  // Determine which material representation to use
  bool has_openpbr = material.hasOpenPBR();
  bool has_preview = material.hasUsdPreviewSurface();

  if (has_openpbr && options.use_webgpu) {
    // Export as WebGPU node material
    output["type"] = "NodeMaterial";
    output["nodes"] = ConvertOpenPBRToNodeMaterial(material.openPBRShader.value(), options);
  } else if (has_openpbr && options.generate_fallback) {
    // Export as WebGL MeshPhysicalMaterial
    output["type"] = "MeshPhysicalMaterial";
    json params = ConvertOpenPBRToPhysicalMaterial(material.openPBRShader.value(), options);
    for (auto it = params.items().begin(); it != params.items().end(); ++it) {
      output[it.key()] = it.value();
    }
  } else if (has_preview) {
    // Export UsdPreviewSurface as standard material
    if (options.use_webgpu) {
      output["type"] = "NodeMaterial";
      output["nodes"] = ConvertPreviewSurfaceToNodeMaterial(material.surfaceShader.value());
    } else {
      output["type"] = "MeshStandardMaterial";
      json params = ConvertPreviewSurfaceToStandardMaterial(material.surfaceShader.value());
      for (auto it = params.items().begin(); it != params.items().end(); ++it) {
        output[it.key()] = it.value();
      }
    }
  }

  // Add metadata about dual materials if both exist
  if (material.hasBothMaterials()) {
    output["metadata"] = {
      {"hasOpenPBR", true},
      {"hasUsdPreviewSurface", true},
      {"preferredShader", has_openpbr ? "OpenPBR" : "UsdPreviewSurface"}
    };
  }

  return true;
}

json ThreeJSMaterialExporter::ConvertOpenPBRToNodeMaterial(const OpenPBRSurfaceShader& shader, const ExportOptions& options) {
  json nodes = json::object();

  // Create OpenPBR surface node
  json surface_node = {
    {"type", "OpenPBRSurfaceNode"},
    {"uuid", "surface_" + std::to_string(shader.handle)},
    {"inputs", json::object()}
  };

  bool use_grouped = options.use_grouped_parameters;

  // Map all OpenPBR parameters
  auto add_param = [&](const std::string& name, const auto& param) {
    json value;
    if (param.is_texture()) {
      value = {
        {"type", "texture"},
        {"textureId", param.texture_id}
      };
    } else {
      value = param.value;
    }
    setJsonParameter(surface_node["inputs"], name, value, use_grouped);
  };

  // Base layer
  add_param("base_weight", shader.base_weight);
  setJsonParameter(surface_node["inputs"], "base_color", vec3ToJson(shader.base_color.value), use_grouped);
  add_param("base_roughness", shader.base_roughness);
  add_param("base_metalness", shader.base_metalness);

  // Specular layer
  add_param("specular_weight", shader.specular_weight);
  setJsonParameter(surface_node["inputs"], "specular_color", vec3ToJson(shader.specular_color.value), use_grouped);
  add_param("specular_roughness", shader.specular_roughness);
  add_param("specular_ior", shader.specular_ior);
  add_param("specular_ior_level", shader.specular_ior_level);
  add_param("specular_anisotropy", shader.specular_anisotropy);
  add_param("specular_rotation", shader.specular_rotation);

  // Transmission
  add_param("transmission_weight", shader.transmission_weight);
  setJsonParameter(surface_node["inputs"], "transmission_color", vec3ToJson(shader.transmission_color.value), use_grouped);
  add_param("transmission_depth", shader.transmission_depth);
  setJsonParameter(surface_node["inputs"], "transmission_scatter", vec3ToJson(shader.transmission_scatter.value), use_grouped);
  add_param("transmission_scatter_anisotropy", shader.transmission_scatter_anisotropy);
  add_param("transmission_dispersion", shader.transmission_dispersion);

  // Subsurface
  add_param("subsurface_weight", shader.subsurface_weight);
  setJsonParameter(surface_node["inputs"], "subsurface_color", vec3ToJson(shader.subsurface_color.value), use_grouped);
  add_param("subsurface_radius", shader.subsurface_radius);
  setJsonParameter(surface_node["inputs"], "subsurface_radius_scale", vec3ToJson(shader.subsurface_radius_scale.value), use_grouped);
  add_param("subsurface_scale", shader.subsurface_scale);
  add_param("subsurface_anisotropy", shader.subsurface_anisotropy);

  // Sheen
  add_param("sheen_weight", shader.sheen_weight);
  setJsonParameter(surface_node["inputs"], "sheen_color", vec3ToJson(shader.sheen_color.value), use_grouped);
  add_param("sheen_roughness", shader.sheen_roughness);

  // Coat
  add_param("coat_weight", shader.coat_weight);
  setJsonParameter(surface_node["inputs"], "coat_color", vec3ToJson(shader.coat_color.value), use_grouped);
  add_param("coat_roughness", shader.coat_roughness);
  add_param("coat_anisotropy", shader.coat_anisotropy);
  add_param("coat_rotation", shader.coat_rotation);
  add_param("coat_ior", shader.coat_ior);
  add_param("coat_affect_color", shader.coat_affect_color);
  add_param("coat_affect_roughness", shader.coat_affect_roughness);

  // Emission
  add_param("emission_luminance", shader.emission_luminance);
  setJsonParameter(surface_node["inputs"], "emission_color", vec3ToJson(shader.emission_color.value), use_grouped);

  // Geometry
  add_param("opacity", shader.opacity);
  setJsonParameter(surface_node["inputs"], "normal", vec3ToJson(shader.normal.value), use_grouped);
  setJsonParameter(surface_node["inputs"], "tangent", vec3ToJson(shader.tangent.value), use_grouped);

  nodes["surface"] = surface_node;

  // Create output node
  nodes["output"] = {
    {"type", "OutputNode"},
    {"uuid", "output_" + std::to_string(shader.handle)},
    {"inputs", {
      {"surface", {
        {"connection", "surface"},
        {"output", "surface"}
      }}
    }}
  };

  return nodes;
}

json ThreeJSMaterialExporter::ConvertOpenPBRToPhysicalMaterial(const OpenPBRSurfaceShader& shader, const ExportOptions& options) {
  (void)options; // Options reserved for future use (e.g., grouped userData)
  json params = json::object();

  // Map OpenPBR to MeshPhysicalMaterial parameters
  params["color"] = vec3ToJson(shader.base_color.value);
  params["metalness"] = shader.base_metalness.value;
  params["roughness"] = shader.base_roughness.value;

  // Emission
  if (shader.emission_luminance.value > 0.0f) {
    params["emissive"] = vec3ToJson(shader.emission_color.value);
    params["emissiveIntensity"] = shader.emission_luminance.value;
  }

  // Opacity
  params["opacity"] = shader.opacity.value;
  if (shader.opacity.value < 1.0f) {
    params["transparent"] = true;
  }

  // Clearcoat (coat layer)
  if (shader.coat_weight.value > 0.0f) {
    params["clearcoat"] = shader.coat_weight.value;
    params["clearcoatRoughness"] = shader.coat_roughness.value;
  }

  // Sheen
  if (shader.sheen_weight.value > 0.0f) {
    params["sheen"] = shader.sheen_weight.value;
    params["sheenColor"] = vec3ToJson(shader.sheen_color.value);
    params["sheenRoughness"] = shader.sheen_roughness.value;
  }

  // IOR
  params["ior"] = shader.specular_ior.value;

  // Transmission
  if (shader.transmission_weight.value > 0.0f) {
    params["transmission"] = shader.transmission_weight.value;
    params["transmissionColor"] = vec3ToJson(shader.transmission_color.value);

    // Three.js doesn't have direct equivalents for all transmission parameters
    // Store them in userData for custom shaders
    params["userData"] = {
      {"transmission_depth", shader.transmission_depth.value},
      {"transmission_scatter", vec3ToJson(shader.transmission_scatter.value)},
      {"transmission_scatter_anisotropy", shader.transmission_scatter_anisotropy.value},
      {"transmission_dispersion", shader.transmission_dispersion.value}
    };
  }

  // Subsurface (store in userData as Three.js doesn't have direct support)
  if (shader.subsurface_weight.value > 0.0f) {
    if (!params.contains("userData")) {
      params["userData"] = json::object();
    }
    params["userData"]["subsurface"] = {
      {"weight", shader.subsurface_weight.value},
      {"color", vec3ToJson(shader.subsurface_color.value)},
      {"radius", shader.subsurface_radius.value},
      {"radius_scale", vec3ToJson(shader.subsurface_radius_scale.value)},
      {"scale", shader.subsurface_scale.value},
      {"anisotropy", shader.subsurface_anisotropy.value}
    };
  }

  // Specular (additional parameters in userData)
  if (!params.contains("userData")) {
    params["userData"] = json::object();
  }
  params["userData"]["specular"] = {
    {"weight", shader.specular_weight.value},
    {"color", vec3ToJson(shader.specular_color.value)},
    {"roughness", shader.specular_roughness.value},
    {"anisotropy", shader.specular_anisotropy.value},
    {"rotation", shader.specular_rotation.value}
  };

  // Handle texture references
  auto handle_texture = [&](const std::string& param_name, const auto& shader_param, const std::string& three_name) {
    (void)param_name; // Unused for now, may be used for debugging
    if (shader_param.is_texture()) {
      params[three_name + "Map"] = {
        {"type", "Texture"},
        {"uuid", std::to_string(shader_param.texture_id)}
      };
    }
  };

  handle_texture("base_color", shader.base_color, "map");
  handle_texture("base_metalness", shader.base_metalness, "metalnessMap");
  handle_texture("base_roughness", shader.base_roughness, "roughnessMap");
  handle_texture("normal", shader.normal, "normalMap");
  handle_texture("emission_color", shader.emission_color, "emissiveMap");
  handle_texture("opacity", shader.opacity, "alphaMap");

  return params;
}

json ThreeJSMaterialExporter::ConvertPreviewSurfaceToStandardMaterial(const PreviewSurfaceShader& shader) {
  json params = json::object();

  // Basic parameters
  params["color"] = vec3ToJson(shader.diffuseColor.value);
  params["emissive"] = vec3ToJson(shader.emissiveColor.value);
  params["opacity"] = shader.opacity.value;

  if (shader.opacity.value < 1.0f || shader.opacityThreshold.value > 0.0f) {
    params["transparent"] = true;
    if (shader.opacityThreshold.value > 0.0f) {
      params["alphaTest"] = shader.opacityThreshold.value;
    }
  }

  // Workflow detection
  if (shader.useSpecularWorkflow) {
    // Specular workflow - use MeshPhongMaterial properties
    params["specular"] = vec3ToJson(shader.specularColor.value);
  } else {
    // Metalness workflow - standard PBR
    params["metalness"] = shader.metallic.value;
  }

  params["roughness"] = shader.roughness.value;

  // Clearcoat
  if (shader.clearcoat.value > 0.0f) {
    params["clearcoat"] = shader.clearcoat.value;
    params["clearcoatRoughness"] = shader.clearcoatRoughness.value;
  }

  // IOR
  params["ior"] = shader.ior.value;

  // Handle texture connections
  auto handle_texture = [&](const std::string& param_name, const auto& shader_param, const std::string& three_name) {
    (void)param_name; // Unused for now, may be used for debugging
    if (shader_param.is_texture()) {
      params[three_name] = {
        {"type", "Texture"},
        {"uuid", std::to_string(shader_param.texture_id)}
      };
    }
  };

  handle_texture("diffuseColor", shader.diffuseColor, "map");
  handle_texture("metallic", shader.metallic, "metalnessMap");
  handle_texture("roughness", shader.roughness, "roughnessMap");
  handle_texture("normal", shader.normal, "normalMap");
  handle_texture("emissiveColor", shader.emissiveColor, "emissiveMap");
  handle_texture("opacity", shader.opacity, "alphaMap");
  handle_texture("occlusion", shader.occlusion, "aoMap");

  // Displacement (store in userData as Three.js handles it differently)
  if (shader.displacement.value != 0.0f || shader.displacement.is_texture()) {
    json displacement_map = json();
    if (shader.displacement.is_texture()) {
      displacement_map = std::to_string(shader.displacement.texture_id);
    }
    params["userData"] = json::object();
    params["userData"]["displacement"] = shader.displacement.value;
    params["userData"]["displacementMap"] = displacement_map;
  }

  return params;
}

json ThreeJSMaterialExporter::ConvertPreviewSurfaceToNodeMaterial(const PreviewSurfaceShader& shader) {
  json nodes = json::object();

  // Create UsdPreviewSurface node
  json surface_node = {
    {"type", "UsdPreviewSurfaceNode"},
    {"uuid", "surface_" + std::to_string(shader.handle)},
    {"inputs", json::object()}
  };

  // Map parameters
  surface_node["inputs"]["diffuseColor"] = vec3ToJson(shader.diffuseColor.value);
  surface_node["inputs"]["emissiveColor"] = vec3ToJson(shader.emissiveColor.value);
  surface_node["inputs"]["useSpecularWorkflow"] = shader.useSpecularWorkflow;
  surface_node["inputs"]["specularColor"] = vec3ToJson(shader.specularColor.value);
  surface_node["inputs"]["metallic"] = shader.metallic.value;
  surface_node["inputs"]["clearcoat"] = shader.clearcoat.value;
  surface_node["inputs"]["clearcoatRoughness"] = shader.clearcoatRoughness.value;
  surface_node["inputs"]["roughness"] = shader.roughness.value;
  surface_node["inputs"]["opacity"] = shader.opacity.value;
  surface_node["inputs"]["opacityThreshold"] = shader.opacityThreshold.value;
  surface_node["inputs"]["ior"] = shader.ior.value;
  surface_node["inputs"]["normal"] = vec3ToJson(shader.normal.value);
  surface_node["inputs"]["displacement"] = shader.displacement.value;
  surface_node["inputs"]["occlusion"] = shader.occlusion.value;

  nodes["surface"] = surface_node;

  // Create output node
  nodes["output"] = {
    {"type", "OutputNode"},
    {"uuid", "output_" + std::to_string(shader.handle)},
    {"inputs", {
      {"surface", {
        {"connection", "surface"},
        {"output", "surface"}
      }}
    }}
  };

  return nodes;
}

bool ThreeJSMaterialExporter::ExportMaterialX(const RenderMaterial& material,
                                              std::string& mtlx_output) {
  std::stringstream ss;

  ss << "<?xml version=\"1.0\"?>\n";
  ss << "<materialx version=\"1.38\">\n";

  if (material.hasOpenPBR()) {
    const auto& shader = material.openPBRShader.value();

    ss << "  <open_pbr_surface name=\"" << material.name << "_surface\" type=\"surfaceshader\">\n";

    // Helper lambda to export float parameters
    auto export_float = [&ss](const std::string& name, const ShaderParam<float>& param) {
      if (!param.is_texture()) {
        ss << "    <input name=\"" << name << "\" type=\"float\" value=\""
           << dtos(param.value) << "\" />\n";
      } else {
        ss << "    <input name=\"" << name << "\" type=\"float\" nodename=\""
           << name << "_texture\" />\n";
      }
    };

    // Helper lambda to export color3 parameters
    auto export_color3 = [&ss](const std::string& name, const ShaderParam<vec3>& param) {
      if (!param.is_texture()) {
        ss << "    <input name=\"" << name << "\" type=\"color3\" value=\""
           << dtos(param.value[0]) << ", "
           << dtos(param.value[1]) << ", "
           << dtos(param.value[2]) << "\" />\n";
      } else {
        ss << "    <input name=\"" << name << "\" type=\"color3\" nodename=\""
           << name << "_texture\" />\n";
      }
    };

    // Helper lambda to export vector3 parameters
    auto export_vector3 = [&ss](const std::string& name, const ShaderParam<vec3>& param) {
      if (!param.is_texture()) {
        ss << "    <input name=\"" << name << "\" type=\"vector3\" value=\""
           << dtos(param.value[0]) << ", "
           << dtos(param.value[1]) << ", "
           << dtos(param.value[2]) << "\" />\n";
      } else {
        ss << "    <input name=\"" << name << "\" type=\"vector3\" nodename=\""
           << name << "_texture\" />\n";
      }
    };

    // Base layer parameters
    export_float("base_weight", shader.base_weight);
    export_color3("base_color", shader.base_color);
    export_float("base_roughness", shader.base_roughness);
    export_float("base_metalness", shader.base_metalness);

    // Specular layer parameters
    export_float("specular_weight", shader.specular_weight);
    export_color3("specular_color", shader.specular_color);
    export_float("specular_roughness", shader.specular_roughness);
    export_float("specular_ior", shader.specular_ior);
    export_float("specular_ior_level", shader.specular_ior_level);
    export_float("specular_anisotropy", shader.specular_anisotropy);
    export_float("specular_rotation", shader.specular_rotation);

    // Transmission parameters
    export_float("transmission_weight", shader.transmission_weight);
    export_color3("transmission_color", shader.transmission_color);
    export_float("transmission_depth", shader.transmission_depth);
    export_color3("transmission_scatter", shader.transmission_scatter);
    export_float("transmission_scatter_anisotropy", shader.transmission_scatter_anisotropy);
    export_float("transmission_dispersion", shader.transmission_dispersion);

    // Subsurface parameters
    export_float("subsurface_weight", shader.subsurface_weight);
    export_color3("subsurface_color", shader.subsurface_color);
    export_float("subsurface_radius", shader.subsurface_radius);
    export_color3("subsurface_radius_scale", shader.subsurface_radius_scale);
    export_float("subsurface_scale", shader.subsurface_scale);
    export_float("subsurface_anisotropy", shader.subsurface_anisotropy);

    // Sheen parameters
    export_float("sheen_weight", shader.sheen_weight);
    export_color3("sheen_color", shader.sheen_color);
    export_float("sheen_roughness", shader.sheen_roughness);

    // Coat layer parameters
    export_float("coat_weight", shader.coat_weight);
    export_color3("coat_color", shader.coat_color);
    export_float("coat_roughness", shader.coat_roughness);
    export_float("coat_anisotropy", shader.coat_anisotropy);
    export_float("coat_rotation", shader.coat_rotation);
    export_float("coat_ior", shader.coat_ior);
    export_float("coat_affect_color", shader.coat_affect_color);
    export_float("coat_affect_roughness", shader.coat_affect_roughness);

    // Emission parameters
    export_float("emission_luminance", shader.emission_luminance);
    export_color3("emission_color", shader.emission_color);

    // Geometry modifiers
    export_float("opacity", shader.opacity);
    export_vector3("geometry_normal", shader.normal);
    export_vector3("geometry_tangent", shader.tangent);

    ss << "  </open_pbr_surface>\n\n";

    // Export texture nodes if any parameters use textures
    auto export_texture_node = [&ss](const std::string& name, const ShaderParam<float>& param, const std::string& type) {
      if (param.is_texture()) {
        ss << "  <image name=\"" << name << "_texture\" type=\"" << type << "\">\n";
        ss << "    <input name=\"file\" type=\"filename\" value=\"texture_"
           << param.texture_id << "\" />\n";
        ss << "  </image>\n\n";
      }
    };

    auto export_texture_node_vec3 = [&ss](const std::string& name, const ShaderParam<vec3>& param, const std::string& type) {
      if (param.is_texture()) {
        ss << "  <image name=\"" << name << "_texture\" type=\"" << type << "\">\n";
        ss << "    <input name=\"file\" type=\"filename\" value=\"texture_"
           << param.texture_id << "\" />\n";
        ss << "  </image>\n\n";
      }
    };

    // Export all texture nodes
    export_texture_node("base_weight", shader.base_weight, "float");
    export_texture_node_vec3("base_color", shader.base_color, "color3");
    export_texture_node("base_roughness", shader.base_roughness, "float");
    export_texture_node("base_metalness", shader.base_metalness, "float");

    export_texture_node("specular_weight", shader.specular_weight, "float");
    export_texture_node_vec3("specular_color", shader.specular_color, "color3");
    export_texture_node("specular_roughness", shader.specular_roughness, "float");
    export_texture_node("specular_ior", shader.specular_ior, "float");
    export_texture_node("specular_ior_level", shader.specular_ior_level, "float");
    export_texture_node("specular_anisotropy", shader.specular_anisotropy, "float");
    export_texture_node("specular_rotation", shader.specular_rotation, "float");

    export_texture_node("transmission_weight", shader.transmission_weight, "float");
    export_texture_node_vec3("transmission_color", shader.transmission_color, "color3");
    export_texture_node("transmission_depth", shader.transmission_depth, "float");
    export_texture_node_vec3("transmission_scatter", shader.transmission_scatter, "color3");
    export_texture_node("transmission_scatter_anisotropy", shader.transmission_scatter_anisotropy, "float");
    export_texture_node("transmission_dispersion", shader.transmission_dispersion, "float");

    export_texture_node("subsurface_weight", shader.subsurface_weight, "float");
    export_texture_node_vec3("subsurface_color", shader.subsurface_color, "color3");
    export_texture_node("subsurface_radius", shader.subsurface_radius, "float");
    export_texture_node_vec3("subsurface_radius_scale", shader.subsurface_radius_scale, "color3");
    export_texture_node("subsurface_scale", shader.subsurface_scale, "float");
    export_texture_node("subsurface_anisotropy", shader.subsurface_anisotropy, "float");

    export_texture_node("sheen_weight", shader.sheen_weight, "float");
    export_texture_node_vec3("sheen_color", shader.sheen_color, "color3");
    export_texture_node("sheen_roughness", shader.sheen_roughness, "float");

    export_texture_node("coat_weight", shader.coat_weight, "float");
    export_texture_node_vec3("coat_color", shader.coat_color, "color3");
    export_texture_node("coat_roughness", shader.coat_roughness, "float");
    export_texture_node("coat_anisotropy", shader.coat_anisotropy, "float");
    export_texture_node("coat_rotation", shader.coat_rotation, "float");
    export_texture_node("coat_ior", shader.coat_ior, "float");
    export_texture_node("coat_affect_color", shader.coat_affect_color, "float");
    export_texture_node("coat_affect_roughness", shader.coat_affect_roughness, "float");

    export_texture_node("emission_luminance", shader.emission_luminance, "float");
    export_texture_node_vec3("emission_color", shader.emission_color, "color3");

    export_texture_node("opacity", shader.opacity, "float");
    export_texture_node_vec3("geometry_normal", shader.normal, "vector3");
    export_texture_node_vec3("geometry_tangent", shader.tangent, "vector3");

    // Create surface material
    ss << "  <surfacematerial name=\"" << material.name << "\" type=\"material\">\n";
    ss << "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\""
       << material.name << "_surface\" />\n";
    ss << "  </surfacematerial>\n";
  } else if (material.hasUsdPreviewSurface()) {
    // Export UsdPreviewSurface as standard_surface for MaterialX compatibility
    const auto& shader = material.surfaceShader.value();

    ss << "  <standard_surface name=\"" << material.name << "_surface\" type=\"surfaceshader\">\n";

    // Map UsdPreviewSurface to standard_surface parameters
    ss << "    <input name=\"base_color\" type=\"color3\" value=\""
       << dtos(shader.diffuseColor.value[0]) << ", "
       << dtos(shader.diffuseColor.value[1]) << ", "
       << dtos(shader.diffuseColor.value[2]) << "\" />\n";

    ss << "    <input name=\"metalness\" type=\"float\" value=\""
       << dtos(shader.metallic.value) << "\" />\n";

    ss << "    <input name=\"specular_roughness\" type=\"float\" value=\""
       << dtos(shader.roughness.value) << "\" />\n";

    ss << "    <input name=\"emission_color\" type=\"color3\" value=\""
       << dtos(shader.emissiveColor.value[0]) << ", "
       << dtos(shader.emissiveColor.value[1]) << ", "
       << dtos(shader.emissiveColor.value[2]) << "\" />\n";

    ss << "    <input name=\"opacity\" type=\"float\" value=\""
       << dtos(shader.opacity.value) << "\" />\n";

    if (shader.clearcoat.value > 0.0f) {
      ss << "    <input name=\"coat\" type=\"float\" value=\""
         << dtos(shader.clearcoat.value) << "\" />\n";
      ss << "    <input name=\"coat_roughness\" type=\"float\" value=\""
         << dtos(shader.clearcoatRoughness.value) << "\" />\n";
    }

    ss << "    <input name=\"specular_IOR\" type=\"float\" value=\""
       << dtos(shader.ior.value) << "\" />\n";

    ss << "  </standard_surface>\n\n";

    // Create surface material
    ss << "  <surfacematerial name=\"" << material.name << "\" type=\"material\">\n";
    ss << "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\""
       << material.name << "_surface\" />\n";
    ss << "  </surfacematerial>\n";
  } else {
    _err = "Material has neither OpenPBR nor UsdPreviewSurface shader";
    return false;
  }

  ss << "</materialx>\n";

  mtlx_output = ss.str();
  return true;
}

json ThreeJSMaterialExporter::ConvertTextureReference(uint64_t texture_id,
                                                      const ExportOptions& options) {
  // Check cache
  if (_texture_cache.find(texture_id) != _texture_cache.end()) {
    return _texture_cache[texture_id];
  }

  json tex_json = {
    {"uuid", std::to_string(texture_id)},
    {"type", "Texture"}
  };

  if (!options.texture_path.empty()) {
    tex_json["url"] = options.texture_path + "/texture_" + std::to_string(texture_id) + ".png";
  }

  // Default wrapping and filtering
  tex_json["wrapS"] = "RepeatWrapping";
  tex_json["wrapT"] = "RepeatWrapping";
  tex_json["magFilter"] = "LinearFilter";
  tex_json["minFilter"] = "LinearMipmapLinearFilter";

  _texture_cache[texture_id] = tex_json;
  return tex_json;
}

// ThreeJSSceneExporter implementation

bool ThreeJSSceneExporter::ExportScene(const RenderScene& scene,
                                       const SceneExportOptions& options,
                                       json& output) {
  output = json::object();

  // Export materials using material exporter
  json materials;
  _material_exporter.ExportScene(scene, options.material_options, materials);
  output["materials"] = materials["materials"];
  output["textures"] = materials["textures"];
  output["geometries"] = materials["geometries"];

  // Build node hierarchy
  json object3d = json::object();
  object3d["type"] = "Scene";
  object3d["children"] = json::array();

  // Convert nodes - add all root level nodes
  for (const auto& node : scene.nodes) {
    object3d["children"].push_back(ConvertNode(node, scene));
  }

  // Add cameras if requested
  if (options.export_cameras) {
    for (const auto& camera : scene.cameras) {
      object3d["children"].push_back(ConvertCamera(camera));
    }
  }

  // Add lights if requested
  if (options.export_lights) {
    for (const auto& light : scene.lights) {
      object3d["children"].push_back(ConvertLight(light));
    }
  }

  output["object"] = object3d;

  // Add animations if requested
  if (options.export_animations && !scene.animations.empty()) {
    json animations = json::array();
    for (const auto& anim : scene.animations) {
      animations.push_back(ConvertAnimation(anim));
    }
    output["animations"] = animations;
  }

  // Add metadata
  output["metadata"] = {
    {"version", 2},
    {"type", "Object"},
    {"generator", "LightUSD ThreeJS Exporter"},
    {"sourceFile", scene.usd_filename}
  };

  return true;
}

json ThreeJSSceneExporter::ConvertNode(const Node& node, const RenderScene& scene, uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    // Bail on pathologically deep node trees rather than overflow the stack.
    return json::object();
  }
  json obj = json::object();

  obj["name"] = node.prim_name;
  obj["uuid"] = std::to_string(node.handle);

  // Determine node type
  switch (node.nodeType) {
    case NodeType::Mesh:
      obj["type"] = "Mesh";
      // Use node.id to directly index mesh data (O(1) instead of O(N) scan)
      if (node.id >= 0 && static_cast<size_t>(node.id) < scene.meshes.size()) {
        const auto& mesh = scene.meshes[static_cast<size_t>(node.id)];
        obj["geometry"] = std::to_string(mesh.handle);
        if (mesh.material_id != -1 && static_cast<size_t>(mesh.material_id) < scene.materials.size()) {
          obj["material"] = std::to_string(scene.materials[static_cast<size_t>(mesh.material_id)].handle);
        }
      }
      break;
    case NodeType::Xform:
      obj["type"] = "Group";
      break;
    case NodeType::Volume:
      // three.js has no native VDB volume; emit a Group placeholder.
      obj["type"] = "Group";
      break;
    case NodeType::Camera:
      obj["type"] = "Camera";
      break;
    case NodeType::SkelRoot:
      obj["type"] = "SkelRoot";
      break;
    case NodeType::Skeleton:
      obj["type"] = "Skeleton";
      break;
    case NodeType::PointLight:
      obj["type"] = "PointLight";
      break;
    case NodeType::DirectionalLight:
      obj["type"] = "DirectionalLight";
      break;
    case NodeType::EnvmapLight:
      obj["type"] = "EnvironmentLight";
      break;
    case NodeType::RectLight:
      obj["type"] = "RectAreaLight";
      break;
    case NodeType::DiskLight:
      obj["type"] = "DiskLight";
      break;
    case NodeType::CylinderLight:
      obj["type"] = "CylinderLight";
      break;
    case NodeType::GeometryLight:
      obj["type"] = "GeometryLight";
      break;
  }

  // Transform matrix
  if (!is_identity(node.local_matrix)) {
    json matrix = json::array();
    for (int i = 0; i < 16; ++i) {
      matrix.push_back(node.local_matrix.m[i]);
    }
    obj["matrix"] = matrix;
  }

  // Add children
  json children = json::array();
  for (size_t i = 0; i < node.children.size(); ++i) {
    const Node& child_node = node.children[i];
    children.push_back(ConvertNode(child_node, scene, depth + 1));
  }
  if (!children.empty()) {
    obj["children"] = children;
  }

  return obj;
}

json ThreeJSSceneExporter::ConvertCamera(const RenderCamera& camera) {
  float fov = 2.0f * std::atan(0.5f * camera.verticalAperture / camera.focalLength);
  json cam = {
    {"type", "PerspectiveCamera"},
    {"name", camera.name},
    {"fov", fov},
    {"aspect", 1.0f / camera.verticalAspectRatio},
    {"near", camera.znear},
    {"far", camera.zfar}
  };
  return cam;
}

json ThreeJSSceneExporter::ConvertLight(const RenderLight& light) {
  json light_json = json::object();

  light_json["name"] = light.name;
  light_json["uuid"] = light.abs_path;

  // Common light properties
  light_json["color"] = vec3ToJson(vec3{light.color[0], light.color[1], light.color[2]});

  // Calculate effective intensity from intensity and exposure
  // exposure is in stops: intensity_final = intensity * 2^exposure
  float effective_intensity = light.intensity * std::pow(2.0f, light.exposure);
  light_json["intensity"] = effective_intensity;

  // Map RenderLight types to Three.js light types
  switch (light.type) {
    case RenderLight::Type::Point:
    case RenderLight::Type::Sphere:
      light_json["type"] = "PointLight";
      // Three.js PointLight doesn't have a physical radius, but we can store it in userData
      if (light.radius > 0.0f) {
        light_json["userData"] = {{"radius", light.radius}};
      }
      // Distance for attenuation (0 means no limit)
      light_json["distance"] = 0.0f;
      // Power=1 for inverse square falloff (physically correct)
      light_json["decay"] = 2.0f;
      break;

    case RenderLight::Type::Distant:
      light_json["type"] = "DirectionalLight";
      // Directional lights have uniform intensity, no attenuation
      // Store angle for disk shape if specified
      if (light.angle > 0.0f) {
        if (!light_json.contains("userData")) {
          light_json["userData"] = json::object();
        }
        light_json["userData"]["angle"] = light.angle;
      }
      break;

    case RenderLight::Type::Rect:
      // Three.js has RectAreaLight (requires RectAreaLightUniformsLib)
      light_json["type"] = "RectAreaLight";
      light_json["width"] = light.width;
      light_json["height"] = light.height;
      // RectAreaLight doesn't support distance/decay in Three.js r140+
      // Store texture file if present
      if (!light.textureFile.empty()) {
        if (!light_json.contains("userData")) {
          light_json["userData"] = json::object();
        }
        light_json["userData"]["textureFile"] = light.textureFile;
      }
      break;

    case RenderLight::Type::Disk:
      // Three.js doesn't have a disk light, use PointLight as approximation
      light_json["type"] = "PointLight";
      light_json["distance"] = 0.0f;
      light_json["decay"] = 2.0f;
      // Store disk shape parameters in userData
      light_json["userData"] = {
        {"shape", "disk"},
        {"radius", light.radius}
      };
      break;

    case RenderLight::Type::Cylinder:
      // Cylinder lights with shaping are like spotlights
      if (light.shapingConeAngle > 0.0f) {
        light_json["type"] = "SpotLight";
        // Convert cone angle (full angle) to Three.js angle (half angle in radians)
        light_json["angle"] = light.shapingConeAngle * 0.5f * (3.14159265359f / 180.0f);
        // Penumbra (0-1) represents the percent of the spotlight cone attenuated due to penumbra
        light_json["penumbra"] = light.shapingConeSoftness;
        light_json["distance"] = 0.0f;
        light_json["decay"] = 2.0f;

        // Store cylinder dimensions in userData
        light_json["userData"] = {
          {"shape", "cylinder"},
          {"radius", light.radius},
          {"length", light.length}
        };

        // Store IES profile if present
        if (!light.shapingIesFile.empty()) {
          light_json["userData"]["iesFile"] = light.shapingIesFile;
        }
      } else {
        // Without shaping, treat as point light with cylindrical metadata
        light_json["type"] = "PointLight";
        light_json["distance"] = 0.0f;
        light_json["decay"] = 2.0f;
        light_json["userData"] = {
          {"shape", "cylinder"},
          {"radius", light.radius},
          {"length", light.length}
        };
      }
      break;

    case RenderLight::Type::Dome:
      // Dome lights are environment maps, closest is HemisphereLight or PMREMGenerator
      light_json["type"] = "HemisphereLight";
      // HemisphereLight has groundColor (opposite hemisphere)
      // For dome lights, we just use black for ground
      light_json["groundColor"] = json::array({0.0f, 0.0f, 0.0f});

      // Store dome-specific parameters in userData
      if (!light.textureFile.empty()) {
        if (!light_json.contains("userData")) {
          light_json["userData"] = json::object();
        }
        light_json["userData"]["textureFile"] = light.textureFile;

        // Store texture format (latlong, mirroredBall, angular)
        std::string format_str;
        switch (light.domeTextureFormat) {
          case RenderLight::DomeTextureFormat::Automatic:
            format_str = "automatic";
            break;
          case RenderLight::DomeTextureFormat::Latlong:
            format_str = "latlong";
            break;
          case RenderLight::DomeTextureFormat::MirroredBall:
            format_str = "mirroredBall";
            break;
          case RenderLight::DomeTextureFormat::Angular:
            format_str = "angular";
            break;
        }
        light_json["userData"]["textureFormat"] = format_str;
      }
      break;

    case RenderLight::Type::Geometry:
      // Geometry lights are meshes with emissive materials
      light_json["type"] = "MeshEmissive";
      light_json["geometry_mesh_id"] = light.geometry_mesh_id;

      if (!light.material_sync_mode.empty()) {
        if (!light_json.contains("userData")) {
          light_json["userData"] = json::object();
        }
        light_json["userData"]["material_sync_mode"] = light.material_sync_mode;
      }
      break;

    case RenderLight::Type::Portal:
      // Portal lights are special lights for portals
      light_json["type"] = "PortalLight";
      break;
  }

  // Shadow properties (if shadow is enabled)
  if (light.shadowEnable) {
    json shadow = json::object();
    shadow["enabled"] = true;
    shadow["color"] = vec3ToJson(vec3{light.shadowColor[0], light.shadowColor[1], light.shadowColor[2]});

    // Shadow distance and falloff parameters
    if (light.shadowDistance > 0.0f) {
      shadow["distance"] = light.shadowDistance;
      shadow["falloff"] = light.shadowFalloff;
      shadow["falloffGamma"] = light.shadowFalloffGamma;
    }

    light_json["shadow"] = shadow;
  }

  // Store additional UsdLux properties that don't map directly to Three.js
  if (!light_json.contains("userData")) {
    light_json["userData"] = json::object();
  }

  // Diffuse and specular contribution multipliers
  light_json["userData"]["diffuse"] = light.diffuse;
  light_json["userData"]["specular"] = light.specular;
  light_json["userData"]["normalize"] = light.normalize;

  // Color temperature
  if (light.enableColorTemperature) {
    light_json["userData"]["colorTemperature"] = light.colorTemperature;
  }

  // Shaping properties for lights that support them (but aren't spotlights)
  if (light.type != RenderLight::Type::Cylinder &&
      (light.shapingConeAngle > 0.0f || light.shapingFocus != 0.0f || !light.shapingIesFile.empty())) {
    light_json["userData"]["shaping"] = {
      {"focus", light.shapingFocus},
      {"coneAngle", light.shapingConeAngle},
      {"coneSoftness", light.shapingConeSoftness}
    };
    if (!light.shapingIesFile.empty()) {
      light_json["userData"]["shaping"]["iesFile"] = light.shapingIesFile;
    }
  }

  return light_json;
}

json ThreeJSSceneExporter::ConvertAnimation(const AnimationClip& anim) {
  json anim_json = {
    {"name", anim.prim_name},
    {"duration", anim.duration},
    {"tracks", json::array()}
  };

  // Convert animation channels to Three.js tracks
  for (const auto& channel : anim.channels) {
    // Get the sampler for this channel
    if (channel.sampler < 0 || channel.sampler >= static_cast<int32_t>(anim.samplers.size())) {
      continue; // Invalid sampler index
    }

    const auto& sampler = anim.samplers[static_cast<size_t>(channel.sampler)];
    if (sampler.empty()) {
      continue;
    }

    // Build target name based on target type
    std::string target_name;
    if (channel.target_type == ChannelTargetType::SceneNode) {
      target_name = "node_" + std::to_string(channel.target_node);
    } else {
      target_name = "skel_" + std::to_string(channel.skeleton_id) +
                    "_joint_" + std::to_string(channel.joint_id);
    }

    // Create track based on animation path
    json track;
    if (channel.path == AnimationPath::Translation) {
      track = {
        {"name", target_name + ".position"},
        {"type", "vector3"},
        {"times", sampler.times},
        {"values", sampler.values}
      };
    } else if (channel.path == AnimationPath::Rotation) {
      track = {
        {"name", target_name + ".quaternion"},
        {"type", "quaternion"},
        {"times", sampler.times},
        {"values", sampler.values}
      };
    } else if (channel.path == AnimationPath::Scale) {
      track = {
        {"name", target_name + ".scale"},
        {"type", "vector3"},
        {"times", sampler.times},
        {"values", sampler.values}
      };
    } else if (channel.path == AnimationPath::Weights) {
      track = {
        {"name", target_name + ".morphTargetInfluences"},
        {"type", "number"},
        {"times", sampler.times},
        {"values", sampler.values}
      };
    } else if (channel.path == AnimationPath::CustomProperty) {
      std::string value_type = "number";
      if (!sampler.times.empty() && !sampler.values.empty() &&
          (sampler.values.size() % sampler.times.size() == 0u)) {
        const size_t comp_count = sampler.values.size() / sampler.times.size();
        if (comp_count == 2) {
          value_type = "vector2";
        } else if (comp_count == 3) {
          value_type = "vector3";
        } else if (comp_count == 4) {
          value_type = "vector4";
        }
      }

      const std::string property_name =
          channel.property_name.empty() ? "value" : channel.property_name;
      track = {
        {"name", target_name + "." + property_name},
        {"type", value_type},
        {"times", sampler.times},
        {"values", sampler.values}
      };
    } else {
      continue; // Unknown path type
    }

    if (!channel.target_prim_path.empty() ||
        !channel.blendshape_target_names.empty()) {
      track["userData"] = json::object();
      if (!channel.target_prim_path.empty()) {
        track["userData"]["target_prim_path"] = channel.target_prim_path;
      }
      if (!channel.blendshape_target_names.empty()) {
        track["userData"]["blendshape_target_names"] =
            channel.blendshape_target_names;
      }
    }

    anim_json["tracks"].push_back(track);
  }

  return anim_json;
}

bool ThreeJSSceneExporter::ExportGLTF(const RenderScene& scene,
                                      const SceneExportOptions& options,
                                      json& gltf_output) {
  (void)scene;
  (void)options;
  (void)gltf_output;
  // TODO: Implement full glTF 2.0 export with MaterialX extensions
  _err = "glTF export not yet implemented";
  return false;
}

} // namespace tydra
} // namespace lightusd
