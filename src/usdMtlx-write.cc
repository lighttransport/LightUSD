// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.

#include <algorithm>
#include <sstream>

#include "usdMtlx.hh"
#include "usdShade.hh"
#include "usdLux.hh"  // SphereLight/RectLight (no longer re-exported by lightusd.hh)
#include "safe-arithmetic.hh"

// Use built-in MaterialX parser instead of pugixml
#include "mtlx-usd-adapter.hh"

#if defined(LIGHTUSD_USE_USDMTLX)

// ============================================================================
// Configuration flags for MaterialX support
// ============================================================================
// Currently only Blender-style OpenPBR + NodeGraph import is actively used.
// Other shader types (UsdPreviewSurface, StandardSurface) export paths are
// disabled until needed. Enable these flags to re-enable those code paths.
// ============================================================================
#define LIGHTUSD_MTLX_ENABLE_USDPREVIEWSURFACE_EXPORT 1
#define LIGHTUSD_MTLX_ENABLE_STANDARDSURFACE_EXPORT 1

#include "ascii-parser.hh"  // To parse color3f value
#include "common-macros.inc"
#include "io-util.hh"
#include "pprint-enum.hh"
#include "security-policy.hh"
#include "str-util.hh"  // For dragonbox-based dtos()
#include "tiny-format.hh"
#include "value-pprint.hh"

// Use dragonbox-based dtos from str-util.hh for shortest representation
// No need for local dtos() or float_to_xml_string() - dtos() already
// produces the shortest round-trip-correct representation without trailing zeros

#define PushError(msg) \
  do {                 \
    if (err) {         \
      (*err) += msg;   \
    }                  \
  } while (0);

// MaterialX WRITE path (USD -> .mtlx XML), split from usdMtlx.cc to divide back-end
// codegen. Read and write paths share no statics. See usdMtlx.cc for the read path.

namespace lightusd {
namespace detail {

// MaterialX is XML, so USD names, asset paths, and user-authored string
// values must not be interpolated into attributes verbatim.
static std::string EscapeXML(const std::string &value, bool attribute = true) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"':
        if (attribute) escaped += "&quot;";
        else escaped.push_back(c);
        break;
      case '\'':
        if (attribute) escaped += "&apos;";
        else escaped.push_back(c);
        break;
      default: escaped.push_back(c); break;
    }
  }
  return escaped;
}

static void SerializeMaterialXHeader(std::stringstream &ss,
                                     const std::string &version,
                                     const std::string &colorspace,
                                     const std::string &cms,
                                     const std::string &cmsconfig,
                                     const std::string &name_space) {
  ss << "<?xml version=\"1.0\"?>\n";
  ss << "<materialx version=\""
     << EscapeXML(version.empty() ? "1.38" : version) << "\" colorspace=\""
     << EscapeXML(colorspace.empty() ? "lin_rec709" : colorspace) << "\"";
  if (!cms.empty()) ss << " cms=\"" << EscapeXML(cms) << "\"";
  if (!cmsconfig.empty()) ss << " cmsconfig=\"" << EscapeXML(cmsconfig) << "\"";
  if (!name_space.empty()) ss << " namespace=\"" << EscapeXML(name_space) << "\"";
  ss << ">\n";
}

template <typename T>
std::string to_xml_string(const T &val);

// Forward declaration
static bool SerializeNodeGraphs(const std::map<std::string, PrimSpec> &nodegraphs,
                                std::stringstream &ss, std::string *warn, std::string *err);

template <>
std::string to_xml_string(const float &val) {
  return dtos(val);
}

template <>
std::string to_xml_string(const int &val) {
  return std::to_string(val);
}

template <>
std::string to_xml_string(const bool &val) {
  return val ? "true" : "false";
}

template <>
std::string to_xml_string(const value::color3f &val) {
  return dtos(val.r) + ", " + dtos(val.g) + ", " + dtos(val.b);
}

template <>
std::string to_xml_string(const value::normal3f &val) {
  return dtos(val.x) + ", " + dtos(val.y) + ", " + dtos(val.z);
}

template <>
std::string to_xml_string(const value::vector3f &val) {
  return dtos(val.x) + ", " + dtos(val.y) + ", " + dtos(val.z);
}

template <typename T>
bool SerializeTypedInput(const char *name, const char *type_name,
                         const TypedAttribute<Animatable<T>> &attr,
                         std::string &xml, std::string *err) {
  if (!attr.authored()) {
    xml.clear();
    return true;
  }

  if (attr.has_connections() && !attr.connections().empty()) {
    const std::string path = attr.connections()[0].full_path_name();
    const size_t dot = path.find('.');
    const std::string node = dot == std::string::npos ? path : path.substr(0, dot);
    const std::string output_marker = ".outputs:";
    const std::string output =
        (dot != std::string::npos && path.compare(dot, output_marker.size(), output_marker) == 0)
            ? path.substr(dot + output_marker.size())
            : std::string();
    xml = std::string("<input name=\"") + EscapeXML(name) +
          "\" type=\"" + EscapeXML(type_name) + "\" nodename=\"" +
          EscapeXML(node) + (output.empty() ? "" : "\" output=\"" + EscapeXML(output)) +
          "\" />\n";
    return true;
  }

  const auto value = attr.get_value();
  T scalar;
  if (!value || !value.value().get_scalar(&scalar)) {
    if (err) {
      *err += fmt::format("Failed to get the value of `{}`", name);
    }
    return false;
  }

  xml = std::string("<input name=\"") + EscapeXML(name) +
        "\" type=\"" + EscapeXML(type_name) + "\" value=\"" +
        EscapeXML(to_xml_string(scalar)) + "\" />\n";
  return true;
}

static void EmitConnectionInput(std::stringstream &ss, const char *name,
                                const char *type_name, const Path &path) {
  const std::string full_path = path.full_path_name();
  const size_t dot = full_path.find('.');
  const std::string node = dot == std::string::npos
                               ? full_path
                               : full_path.substr(0, dot);
  const std::string output_marker = ".outputs:";
  const std::string output =
      (dot != std::string::npos && full_path.compare(dot, output_marker.size(), output_marker) == 0)
          ? full_path.substr(dot + output_marker.size())
          : std::string();
  ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(name)
     << "\" type=\"" << EscapeXML(type_name) << "\" nodename=\""
     << EscapeXML(node) << (output.empty() ? "" : "\" output=\"" + EscapeXML(output))
     << "\" />\n";
}

static void SerializeSurfaceMaterials(
    const std::map<std::string, MtlxMaterial> &materials,
    const std::string &default_name, const std::string &default_node,
    std::stringstream &ss) {
  if (materials.empty()) {
    ss << pprint::Indent(1) << "<surfacematerial name=\""
       << EscapeXML(default_name) << "\" type=\"material\">\n";
    ss << pprint::Indent(2)
       << "<input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\""
       << EscapeXML(default_node) << "\" />\n";
    ss << pprint::Indent(1) << "</surfacematerial>\n";
    return;
  }

  for (const auto &item : materials) {
    const MtlxMaterial &material = item.second;
    const std::string material_name = material.name.empty() ? item.first : material.name;
    const std::string node_name = material.nodename.empty() ? default_node : material.nodename;
    ss << pprint::Indent(1) << "<surfacematerial name=\""
       << EscapeXML(material_name) << "\" type=\""
       << EscapeXML(material.typeName.empty() ? "material" : material.typeName)
       << "\">\n";
    ss << pprint::Indent(2)
       << "<input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\""
       << EscapeXML(node_name) << "\" />\n";
    ss << pprint::Indent(1) << "</surfacematerial>\n";
  }
}

static void SerializeLooks(const std::map<std::string, MtlxLook> &looks,
                           std::stringstream &ss) {
  const auto serialize_element = [&](const auto &self,
                                     const MtlxLookElement &element,
                                     int indent) -> void {
    ss << pprint::Indent(static_cast<uint32_t>(indent)) << "<"
       << EscapeXML(element.name);
    for (const auto &attribute : element.attributes) {
      ss << " " << EscapeXML(attribute.first) << "=\""
         << EscapeXML(attribute.second) << "\"";
    }
    if (element.children.empty()) {
      ss << " />\n";
      return;
    }
    ss << ">\n";
    for (const auto &child : element.children) self(self, child, indent + 1);
    ss << pprint::Indent(static_cast<uint32_t>(indent)) << "</"
       << EscapeXML(element.name) << ">\n";
  };
  for (const auto &item : looks) {
    const MtlxLook &look = item.second;
    const std::string name = look.name.empty() ? item.first : look.name;
    ss << pprint::Indent(1) << "<look name=\"" << EscapeXML(name) << "\">\n";
    for (const auto &element : look.elements) {
      serialize_element(serialize_element, element, 2);
    }
    ss << pprint::Indent(1) << "</look>\n";
  }
}

template <typename T>
static void SerializeMtlxLightInput(
    const char *name, const char *type,
    const TypedAttribute<Animatable<T>> &attr, std::stringstream &ss) {
  if (!attr.authored()) return;
  const auto value = attr.get_value();
  T scalar;
  if (!value || !value.value().get_scalar(&scalar)) return;
  ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(name)
     << "\" type=\"" << EscapeXML(type) << "\" value=\""
     << EscapeXML(to_xml_string(scalar)) << "\" />\n";
}

template <typename T>
static void SerializeMtlxLightInput(
    const char *name, const char *type,
    const TypedAttributeWithFallback<Animatable<T>> &attr,
    std::stringstream &ss) {
  if (!attr.authored()) return;
  const auto &value = attr.get_value();
  T scalar;
  if (!value.get_scalar(&scalar)) return;
  ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(name)
     << "\" type=\"" << EscapeXML(type) << "\" value=\""
     << EscapeXML(to_xml_string(scalar)) << "\" />\n";
}

static void SerializeMtlxLightShaders(
    const std::map<std::string, value::Value> &light_shaders,
    std::stringstream &ss) {
  for (const auto &item : light_shaders) {
    const std::string name = item.first;
    if (const auto *edf = item.second.as<MtlxUniformEdf>()) {
      ss << pprint::Indent(1) << "<uniform_edf name=\"" << EscapeXML(name)
         << "\" type=\"EDF\">\n";
      SerializeMtlxLightInput("color", "color3", edf->color, ss);
      ss << pprint::Indent(1) << "</uniform_edf>\n";
    } else if (const auto *conical_edf = item.second.as<MtlxConicalEdf>()) {
      ss << pprint::Indent(1) << "<conical_edf name=\"" << EscapeXML(name)
         << "\" type=\"EDF\">\n";
      SerializeMtlxLightInput("color", "color3", conical_edf->color, ss);
      SerializeMtlxLightInput("normal", "vector3", conical_edf->normal, ss);
      SerializeMtlxLightInput("inner_angle", "float", conical_edf->inner_angle, ss);
      SerializeMtlxLightInput("outer_angle", "float", conical_edf->outer_angle, ss);
      ss << pprint::Indent(1) << "</conical_edf>\n";
    } else if (const auto *measured_edf = item.second.as<MtlxMeasuredEdf>()) {
      ss << pprint::Indent(1) << "<measured_edf name=\"" << EscapeXML(name)
         << "\" type=\"EDF\">\n";
      SerializeMtlxLightInput("color", "color3", measured_edf->color, ss);
      if (measured_edf->file.authored()) {
        const auto file = measured_edf->file.get_value();
        value::AssetPath path;
        if (file && file.value().get_scalar(&path)) {
          ss << pprint::Indent(2) << "<input name=\"file\" type=\"filename\" value=\""
             << EscapeXML(path.GetAssetPath()) << "\" />\n";
        }
      }
      ss << pprint::Indent(1) << "</measured_edf>\n";
    } else if (const auto *light = item.second.as<MtlxLight>()) {
      ss << pprint::Indent(1) << "<light name=\"" << EscapeXML(name)
         << "\" type=\"lightshader\">\n";
      if (light->edf.authored()) {
        value::token edf_name;
        if (light->edf.get_value(&edf_name)) {
          ss << pprint::Indent(2) << "<input name=\"edf\" type=\"EDF\" nodename=\""
             << EscapeXML(edf_name.str()) << "\" />\n";
        }
      }
      SerializeMtlxLightInput("intensity", "color3", light->intensity, ss);
      SerializeMtlxLightInput("exposure", "float", light->exposure, ss);
      ss << pprint::Indent(1) << "</light>\n";
    }
  }
}

template <typename T>
bool SerializeAttribute(const std::string &attr_name,
                        const TypedAttributeWithFallback<Animatable<T>> &attr,
                        std::string &value_str, std::string *err) {
  std::stringstream value_ss;

  // A fallback value is a schema default, not authored MaterialX content.
  // Do not materialize every default on export; this keeps output compact and
  // preserves the distinction between authored and implicit values.
  if (!attr.authored() && !attr.is_connection() && !attr.is_blocked()) {
    value_str.clear();
    return true;
  }

  if (attr.is_connection()) {
    // Connection-only inputs are emitted by EMIT_ATTRIBUTE, which preserves
    // the connected node path as a MaterialX nodename. Keep this helper
    // tolerant for callers that intentionally omit connection-only values.
    value_str.clear();
    return true;
  } else if (attr.is_blocked()) {
    // do nothing
    value_str = "";
    return true;
  } else {
    const Animatable<T> &animatable_value = attr.get_value();
    if (animatable_value.has_default()) {
      T value;
      if (animatable_value.get_scalar(&value)) {
        value_ss << "\"" << EscapeXML(to_xml_string(value)) << "\"";
      } else {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to get the value at default time of `{}`", attr_name));
      }
    } else {
      // no time-varying(timesamples) attribute in MaterialX.

      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to get the value of `{}`", attr_name));
    }
  }

  value_str = value_ss.str();
  return true;
}

#if LIGHTUSD_MTLX_ENABLE_USDPREVIEWSURFACE_EXPORT
static void EmitUnconsumedShaderConnections(
    const std::vector<MtlxShaderConnection> &connections,
    const std::vector<std::string> &consumed, std::stringstream &ss) {
  for (const auto &conn : connections) {
    if (conn.input_name.empty() ||
        std::find(consumed.begin(), consumed.end(), conn.input_name) !=
            consumed.end()) {
      continue;
    }
    const std::string type = conn.type.empty() ? "color3" : conn.type;
    ss << pprint::Indent(2) << "<input name=\""
       << detail::EscapeXML(conn.input_name) << "\" type=\""
       << detail::EscapeXML(type) << "\"";
    if (!conn.nodegraph.empty()) {
      ss << " nodegraph=\"" << detail::EscapeXML(conn.nodegraph) << "\"";
      if (!conn.output.empty()) {
        ss << " output=\"" << detail::EscapeXML(conn.output) << "\"";
      }
    } else if (!conn.nodename.empty()) {
      ss << " nodename=\"" << detail::EscapeXML(conn.nodename) << "\"";
    }
    ss << " />\n";
  }
}

static bool WriteMaterialXToString(const MtlxUsdPreviewSurface &shader,
                                   const std::string &shader_name,
                                   const std::vector<MtlxShaderConnection> &connections,
                                   const std::map<std::string, PrimSpec> &nodegraphs,
                                   const std::map<std::string, MtlxMaterial> &materials,
                                   const std::map<std::string, MtlxLook> &looks,
                                   const std::string &colorspace,
                                   const std::string &version,
                                   const std::string &cms,
                                   const std::string &cmsconfig,
                                   const std::string &name_space,
                                   const std::map<std::string, value::Value> &light_shaders,
                                   std::string &xml_str, std::string *warn,
                                   std::string *err) {
  (void)warn;

  std::stringstream ss;

  std::string node_name = shader_name.empty() ? "SR_default" : shader_name;
  // Use provided colorspace or default to lin_rec709
  std::string cs = colorspace.empty() ? "lin_rec709" : colorspace;

  SerializeMaterialXHeader(ss, version, cs, cms, cmsconfig, name_space);

  SerializeMtlxLightShaders(light_shaders, ss);

  // Serialize nodegraphs first
  if (!nodegraphs.empty()) {
    SerializeNodeGraphs(nodegraphs, ss, warn, err);
  }
  SerializeLooks(looks, ss);

  ss << pprint::Indent(1) << "<UsdPreviewSurface name=\"" << EscapeXML(node_name)
     << "\" type=\"surfaceshader\">\n";

  // Helper to check if an input has a connection
  std::vector<std::string> consumed_connections;
  auto has_connection = [&connections, &consumed_connections](const std::string &input_name) -> const MtlxShaderConnection* {
    for (const auto &conn : connections) {
      if (conn.input_name == input_name) {
        consumed_connections.push_back(input_name);
        return &conn;
      }
    }
    return nullptr;
  };

#define EMIT_ATTRIBUTE(__name, __tyname, __attr)                            \
  {                                                                         \
    const MtlxShaderConnection *conn = has_connection(__name);              \
    if (conn) {                                                             \
      /* Emit connection */                                                 \
      ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
         << EscapeXML(__tyname) << "\"";                                               \
      if (!conn->nodegraph.empty()) {                                       \
        ss << " nodegraph=\"" << EscapeXML(conn->nodegraph) << "\"";                   \
        if (!conn->output.empty()) {                                        \
          ss << " output=\"" << EscapeXML(conn->output) << "\"";                       \
        }                                                                   \
      } else if (!conn->nodename.empty()) {                                 \
        ss << " nodename=\"" << EscapeXML(conn->nodename) << "\"";                     \
      }                                                                     \
      ss << " />\n";                                                        \
    } else if (__attr.is_connection() && !__attr.connections().empty()) {   \
      EmitConnectionInput(ss, __name, __tyname, __attr.connections()[0]);   \
    } else {                                                                \
      /* Emit value */                                                      \
      std::string value_str;                                                \
      if (!SerializeAttribute(__name, __attr, value_str, err)) {            \
        return false;                                                       \
      }                                                                     \
      if (value_str.size()) {                                               \
        ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
           << EscapeXML(__tyname) << "\" value=" << value_str << " />\n";              \
      }                                                                     \
    }                                                                       \
  }

  EMIT_ATTRIBUTE("diffuseColor", "color3", shader.diffuseColor)
  EMIT_ATTRIBUTE("emissiveColor", "color3", shader.emissiveColor)
  EMIT_ATTRIBUTE("useSpecularWorkflow", "integer", shader.useSpecularWorkflow)
  EMIT_ATTRIBUTE("specularColor", "color3", shader.specularColor)
  EMIT_ATTRIBUTE("metallic", "float", shader.metallic)
  EMIT_ATTRIBUTE("roughness", "float", shader.roughness)
  EMIT_ATTRIBUTE("clearcoat", "float", shader.clearcoat)
  EMIT_ATTRIBUTE("clearcoatRoughness", "float", shader.clearcoatRoughness)
  EMIT_ATTRIBUTE("opacity", "float", shader.opacity)
  EMIT_ATTRIBUTE("opacityThreshold", "float", shader.opacityThreshold)
  EMIT_ATTRIBUTE("ior", "float", shader.ior)
  EMIT_ATTRIBUTE("normal", "vector3", shader.normal)
  EMIT_ATTRIBUTE("displacement", "float", shader.displacement)
  EMIT_ATTRIBUTE("occlusion", "float", shader.occlusion)

  EmitUnconsumedShaderConnections(connections, consumed_connections, ss);

  ss << pprint::Indent(1) << "</UsdPreviewSurface>\n";

  SerializeSurfaceMaterials(materials, "USD_Default", node_name, ss);

  ss << "</materialx>\n";

  xml_str = ss.str();

  return true;
}
#endif // LIGHTUSD_MTLX_ENABLE_USDPREVIEWSURFACE_EXPORT

#if LIGHTUSD_MTLX_ENABLE_STANDARDSURFACE_EXPORT
static bool WriteMaterialXToString(const MtlxAutodeskStandardSurface &shader,
                                   const std::string &shader_name,
                                   const std::vector<MtlxShaderConnection> &connections,
                                   const std::map<std::string, PrimSpec> &nodegraphs,
                                   const std::map<std::string, MtlxMaterial> &materials,
                                   const std::map<std::string, MtlxLook> &looks,
                                   const std::string &colorspace,
                                   const std::string &version,
                                   const std::string &cms,
                                   const std::string &cmsconfig,
                                   const std::string &name_space,
                                   const std::map<std::string, value::Value> &light_shaders,
                                   std::string &xml_str, std::string *warn,
                                   std::string *err) {
  (void)warn;

  std::stringstream ss;

  std::string node_name = shader_name.empty() ? "SR_default" : shader_name;
  // Use provided colorspace or default to lin_rec709
  std::string cs = colorspace.empty() ? "lin_rec709" : colorspace;

  SerializeMaterialXHeader(ss, version, cs, cms, cmsconfig, name_space);

  SerializeMtlxLightShaders(light_shaders, ss);

  // Serialize nodegraphs first
  if (!nodegraphs.empty()) {
    SerializeNodeGraphs(nodegraphs, ss, warn, err);
  }
  SerializeLooks(looks, ss);

  ss << pprint::Indent(1) << "<standard_surface name=\"" << EscapeXML(node_name)
     << "\" type=\"surfaceshader\">\n";

  // Helper to check if an input has a connection
  std::vector<std::string> consumed_connections;
  auto has_connection = [&connections, &consumed_connections](const std::string &input_name) -> const MtlxShaderConnection* {
    for (const auto &conn : connections) {
      if (conn.input_name == input_name) {
        consumed_connections.push_back(input_name);
        return &conn;
      }
    }
    return nullptr;
  };

#define EMIT_ATTRIBUTE(__name, __tyname, __attr)                            \
  {                                                                         \
    const MtlxShaderConnection *conn = has_connection(__name);              \
    if (conn) {                                                             \
      /* Emit connection */                                                 \
      ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
         << EscapeXML(__tyname) << "\"";                                               \
      if (!conn->nodegraph.empty()) {                                       \
        ss << " nodegraph=\"" << EscapeXML(conn->nodegraph) << "\"";                   \
        if (!conn->output.empty()) {                                        \
          ss << " output=\"" << EscapeXML(conn->output) << "\"";                       \
        }                                                                   \
      } else if (!conn->nodename.empty()) {                                 \
        ss << " nodename=\"" << EscapeXML(conn->nodename) << "\"";                     \
      }                                                                     \
      ss << " />\n";                                                        \
    } else if (__attr.is_connection() && !__attr.connections().empty()) {   \
      EmitConnectionInput(ss, __name, __tyname, __attr.connections()[0]);   \
    } else {                                                                \
      /* Emit value */                                                      \
      std::string value_str;                                                \
      if (!SerializeAttribute(__name, __attr, value_str, err)) {            \
        return false;                                                       \
      }                                                                     \
      if (value_str.size()) {                                               \
        ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
           << EscapeXML(__tyname) << "\" value=" << value_str << " />\n";              \
      }                                                                     \
    }                                                                       \
  }

  // Base properties
  EMIT_ATTRIBUTE("base", "float", shader.base)
  EMIT_ATTRIBUTE("base_color", "color3", shader.base_color)
  EMIT_ATTRIBUTE("diffuse_roughness", "float", shader.diffuse_roughness)
  EMIT_ATTRIBUTE("metalness", "float", shader.metalness)

  // Specular properties
  EMIT_ATTRIBUTE("specular", "float", shader.specular)
  EMIT_ATTRIBUTE("specular_color", "color3", shader.specular_color)
  EMIT_ATTRIBUTE("specular_roughness", "float", shader.specular_roughness)
  EMIT_ATTRIBUTE("specular_IOR", "float", shader.specular_IOR)
  EMIT_ATTRIBUTE("specular_anisotropy", "float", shader.specular_anisotropy)
  EMIT_ATTRIBUTE("specular_rotation", "float", shader.specular_rotation)

  // Transmission properties
  EMIT_ATTRIBUTE("transmission", "float", shader.transmission)
  EMIT_ATTRIBUTE("transmission_color", "color3", shader.transmission_color)
  EMIT_ATTRIBUTE("transmission_depth", "float", shader.transmission_depth)
  EMIT_ATTRIBUTE("transmission_scatter", "color3", shader.transmission_scatter)
  EMIT_ATTRIBUTE("transmission_scatter_anisotropy", "float", shader.transmission_scatter_anisotropy)
  EMIT_ATTRIBUTE("transmission_dispersion", "float", shader.transmission_dispersion)
  EMIT_ATTRIBUTE("transmission_extra_roughness", "float", shader.transmission_extra_roughness)

  // Subsurface properties
  EMIT_ATTRIBUTE("subsurface", "float", shader.subsurface)
  EMIT_ATTRIBUTE("subsurface_color", "color3", shader.subsurface_color)
  // Autodesk standard_surface defines subsurface_radius as color3 (matching the
  // MtlxAutodeskStandardSurface color3f field and the reader); emitting "float"
  // here produced a type/value mismatch that failed re-parse.
  EMIT_ATTRIBUTE("subsurface_radius", "color3", shader.subsurface_radius)
  EMIT_ATTRIBUTE("subsurface_scale", "float", shader.subsurface_scale)
  EMIT_ATTRIBUTE("subsurface_anisotropy", "float", shader.subsurface_anisotropy)

  // Sheen properties
  EMIT_ATTRIBUTE("sheen", "float", shader.sheen)
  EMIT_ATTRIBUTE("sheen_color", "color3", shader.sheen_color)
  EMIT_ATTRIBUTE("sheen_roughness", "float", shader.sheen_roughness)

  // Coat properties
  EMIT_ATTRIBUTE("coat", "float", shader.coat)
  EMIT_ATTRIBUTE("coat_color", "color3", shader.coat_color)
  EMIT_ATTRIBUTE("coat_roughness", "float", shader.coat_roughness)
  EMIT_ATTRIBUTE("coat_anisotropy", "float", shader.coat_anisotropy)
  EMIT_ATTRIBUTE("coat_rotation", "float", shader.coat_rotation)
  EMIT_ATTRIBUTE("coat_IOR", "float", shader.coat_IOR)
  EMIT_ATTRIBUTE("coat_affect_color", "float", shader.coat_affect_color)
  EMIT_ATTRIBUTE("coat_affect_roughness", "float", shader.coat_affect_roughness)

  // Thin film properties
  EMIT_ATTRIBUTE("thin_film_thickness", "float", shader.thin_film_thickness)
  EMIT_ATTRIBUTE("thin_film_IOR", "float", shader.thin_film_IOR)

  // Emission properties
  EMIT_ATTRIBUTE("emission", "float", shader.emission)
  EMIT_ATTRIBUTE("emission_color", "color3", shader.emission_color)

  // Opacity
  EMIT_ATTRIBUTE("opacity", "color3", shader.opacity)

  // Thin walled
  EMIT_ATTRIBUTE("thin_walled", "boolean", shader.thin_walled)

  // Renderer extension shared with USD-authored standard_surface graphs.
  EMIT_ATTRIBUTE("displacement", "float", shader.displacement)

  std::string typed_input;
  if (!SerializeTypedInput("normal", "vector3", shader.normal, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;
  if (!SerializeTypedInput("tangent", "vector3", shader.tangent, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;
  if (!SerializeTypedInput("coat_normal", "vector3", shader.coat_normal, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;

#undef EMIT_ATTRIBUTE

  EmitUnconsumedShaderConnections(connections, consumed_connections, ss);

  ss << pprint::Indent(1) << "</standard_surface>\n";

  SerializeSurfaceMaterials(materials, "StandardSurface_Material", node_name, ss);

  ss << "</materialx>\n";

  xml_str = ss.str();

  return true;
}
#endif // LIGHTUSD_MTLX_ENABLE_STANDARDSURFACE_EXPORT

// ============================================================================
// OpenPBR Surface Export - ACTIVE PATH
// This is the primary export path used for Blender MaterialX exports.
// ============================================================================
static bool WriteMaterialXToString(const MtlxOpenPBRSurface &shader,
                                   const std::string &shader_name,
                                   const std::vector<MtlxShaderConnection> &connections,
                                   const std::map<std::string, PrimSpec> &nodegraphs,
                                   const std::map<std::string, MtlxMaterial> &materials,
                                   const std::map<std::string, MtlxLook> &looks,
                                   const std::string &colorspace,
                                   const std::string &version,
                                   const std::string &cms,
                                   const std::string &cmsconfig,
                                   const std::string &name_space,
                                   const std::map<std::string, value::Value> &light_shaders,
                                   std::string &xml_str, std::string *warn,
                                   std::string *err) {
  (void)warn;

  std::stringstream ss;

  std::string node_name = shader_name.empty() ? "SR_default" : shader_name;
  // Use provided colorspace or default to lin_rec709
  std::string cs = colorspace.empty() ? "lin_rec709" : colorspace;

  SerializeMaterialXHeader(ss, version, cs, cms, cmsconfig, name_space);

  SerializeMtlxLightShaders(light_shaders, ss);

  // Serialize nodegraphs first
  if (!nodegraphs.empty()) {
    SerializeNodeGraphs(nodegraphs, ss, warn, err);
  }
  SerializeLooks(looks, ss);

  ss << pprint::Indent(1) << "<open_pbr_surface name=\"" << EscapeXML(node_name)
     << "\" type=\"surfaceshader\">\n";

  // Helper to check if an input has a connection
  std::vector<std::string> consumed_connections;
  auto has_connection = [&connections, &consumed_connections](const std::string &input_name) -> const MtlxShaderConnection* {
    for (const auto &conn : connections) {
      if (conn.input_name == input_name) {
        consumed_connections.push_back(input_name);
        return &conn;
      }
    }
    return nullptr;
  };

#define EMIT_ATTRIBUTE(__name, __tyname, __attr)                            \
  {                                                                         \
    const MtlxShaderConnection *conn = has_connection(__name);              \
    if (conn) {                                                             \
      /* Emit connection */                                                 \
      ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
         << EscapeXML(__tyname) << "\"";                                               \
      if (!conn->nodegraph.empty()) {                                       \
        ss << " nodegraph=\"" << EscapeXML(conn->nodegraph) << "\"";                   \
        if (!conn->output.empty()) {                                        \
          ss << " output=\"" << EscapeXML(conn->output) << "\"";                       \
        }                                                                   \
      } else if (!conn->nodename.empty()) {                                 \
        ss << " nodename=\"" << EscapeXML(conn->nodename) << "\"";                     \
      }                                                                     \
      ss << " />\n";                                                        \
    } else if (__attr.is_connection() && !__attr.connections().empty()) {   \
      EmitConnectionInput(ss, __name, __tyname, __attr.connections()[0]);   \
    } else {                                                                \
      /* Emit value */                                                      \
      std::string value_str;                                                \
      if (!SerializeAttribute(__name, __attr, value_str, err)) {            \
        return false;                                                       \
      }                                                                     \
      if (value_str.size()) {                                               \
        ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(__name) << "\" type=\"" \
           << EscapeXML(__tyname) << "\" value=" << value_str << " />\n";              \
      }                                                                     \
    }                                                                       \
  }

  // Base properties
  EMIT_ATTRIBUTE("base_weight", "float", shader.base_weight)
  EMIT_ATTRIBUTE("base_color", "color3", shader.base_color)
  EMIT_ATTRIBUTE("base_roughness", "float", shader.base_roughness)
  EMIT_ATTRIBUTE("base_metalness", "float", shader.base_metalness)
  EMIT_ATTRIBUTE("base_diffuse_roughness", "float", shader.base_diffuse_roughness)

  // Specular properties
  EMIT_ATTRIBUTE("specular_weight", "float", shader.specular_weight)
  EMIT_ATTRIBUTE("specular_color", "color3", shader.specular_color)
  EMIT_ATTRIBUTE("specular_roughness", "float", shader.specular_roughness)
  EMIT_ATTRIBUTE("specular_ior", "float", shader.specular_ior)
  EMIT_ATTRIBUTE("specular_ior_level", "float", shader.specular_ior_level)
  EMIT_ATTRIBUTE("specular_anisotropy", "float", shader.specular_anisotropy)
  EMIT_ATTRIBUTE("specular_rotation", "float", shader.specular_rotation)
  EMIT_ATTRIBUTE("specular_roughness_anisotropy", "float", shader.specular_roughness_anisotropy)

  // Transmission properties
  EMIT_ATTRIBUTE("transmission_weight", "float", shader.transmission_weight)
  EMIT_ATTRIBUTE("transmission_color", "color3", shader.transmission_color)
  EMIT_ATTRIBUTE("transmission_depth", "float", shader.transmission_depth)
  EMIT_ATTRIBUTE("transmission_scatter", "color3", shader.transmission_scatter)
  EMIT_ATTRIBUTE("transmission_scatter_anisotropy", "float", shader.transmission_scatter_anisotropy)
  EMIT_ATTRIBUTE("transmission_dispersion", "float", shader.transmission_dispersion)
  EMIT_ATTRIBUTE("transmission_dispersion_abbe_number", "float", shader.transmission_dispersion_abbe_number)
  EMIT_ATTRIBUTE("transmission_dispersion_scale", "float", shader.transmission_dispersion_scale)

  // Subsurface properties
  EMIT_ATTRIBUTE("subsurface_weight", "float", shader.subsurface_weight)
  EMIT_ATTRIBUTE("subsurface_color", "color3", shader.subsurface_color)
  EMIT_ATTRIBUTE("subsurface_radius", "float", shader.subsurface_radius)
  EMIT_ATTRIBUTE("subsurface_radius_scale", "color3", shader.subsurface_radius_scale)
  EMIT_ATTRIBUTE("subsurface_scale", "float", shader.subsurface_scale)
  EMIT_ATTRIBUTE("subsurface_anisotropy", "float", shader.subsurface_anisotropy)
  EMIT_ATTRIBUTE("subsurface_scatter_anisotropy", "float", shader.subsurface_scatter_anisotropy)

  // Sheen properties
  EMIT_ATTRIBUTE("sheen_weight", "float", shader.sheen_weight)
  EMIT_ATTRIBUTE("sheen_color", "color3", shader.sheen_color)
  EMIT_ATTRIBUTE("sheen_roughness", "float", shader.sheen_roughness)

  // Coat properties
  EMIT_ATTRIBUTE("coat_weight", "float", shader.coat_weight)
  EMIT_ATTRIBUTE("coat_color", "color3", shader.coat_color)
  EMIT_ATTRIBUTE("coat_roughness", "float", shader.coat_roughness)
  EMIT_ATTRIBUTE("coat_anisotropy", "float", shader.coat_anisotropy)
  EMIT_ATTRIBUTE("coat_rotation", "float", shader.coat_rotation)
  EMIT_ATTRIBUTE("coat_roughness_anisotropy", "float", shader.coat_roughness_anisotropy)
  EMIT_ATTRIBUTE("coat_ior", "float", shader.coat_ior)
  EMIT_ATTRIBUTE("coat_darkening", "float", shader.coat_darkening)
  EMIT_ATTRIBUTE("coat_affect_color", "float", shader.coat_affect_color)
  EMIT_ATTRIBUTE("coat_affect_roughness", "float", shader.coat_affect_roughness)

  // Fuzz properties
  EMIT_ATTRIBUTE("fuzz_weight", "float", shader.fuzz_weight)
  EMIT_ATTRIBUTE("fuzz_color", "color3", shader.fuzz_color)
  EMIT_ATTRIBUTE("fuzz_roughness", "float", shader.fuzz_roughness)

  // Thin film properties
  EMIT_ATTRIBUTE("thin_film_thickness", "float", shader.thin_film_thickness)
  EMIT_ATTRIBUTE("thin_film_ior", "float", shader.thin_film_ior)
  EMIT_ATTRIBUTE("thin_film_weight", "float", shader.thin_film_weight)

  // Emission properties
  EMIT_ATTRIBUTE("emission_luminance", "float", shader.emission_luminance)
  EMIT_ATTRIBUTE("emission_color", "color3", shader.emission_color)

  // Geometry properties
  EMIT_ATTRIBUTE("geometry_opacity", "float", shader.geometry_opacity)
  EMIT_ATTRIBUTE("geometry_thin_walled", "boolean", shader.geometry_thin_walled)

  std::string typed_input;
  if (!SerializeTypedInput("normal", "vector3", shader.geometry_normal, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;
  if (!SerializeTypedInput("tangent", "vector3", shader.geometry_tangent, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;
  if (!SerializeTypedInput("coat_normal", "vector3", shader.geometry_coat_normal, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;
  if (!SerializeTypedInput("coat_tangent", "vector3", shader.geometry_coat_tangent, typed_input, err)) {
    return false;
  }
  if (!typed_input.empty()) ss << pprint::Indent(2) << typed_input;

#undef EMIT_ATTRIBUTE

  EmitUnconsumedShaderConnections(connections, consumed_connections, ss);

  ss << pprint::Indent(1) << "</open_pbr_surface>\n";

  SerializeSurfaceMaterials(materials, "OpenPBR_Material", node_name, ss);

  ss << "</materialx>\n";

  xml_str = ss.str();

  return true;
}

// Helper to extract MaterialX node category from info:id (e.g., "ND_multiply_color3" -> "multiply")
static std::string ExtractNodeCategory(const std::string &info_id) {
  // info:id format: "ND_<category>_<type>" or just "<category>"
  if (info_id.substr(0, 3) == "ND_") {
    std::string rest = info_id.substr(3);
    size_t underscore = rest.rfind('_');
    if (underscore != std::string::npos) {
      rest = rest.substr(0, underscore);
    }
    return rest;
  }
  return info_id;
}

// Helper to extract MaterialX type from info:id (e.g., "ND_multiply_color3" -> "color3")
static std::string ExtractNodeType(const std::string &info_id) {
  size_t underscore = info_id.rfind('_');
  if (underscore != std::string::npos) {
    return info_id.substr(underscore + 1);
  }
  return "float";
}

// Helper to convert USD type to MaterialX type string
static std::string ToMtlxTypeString(const value::Value &val) {
  if (val.type_name() == "float[]") return "floatarray";
  if (val.type_name() == "int[]") return "integerarray";
  if (val.type_name() == "color3f[]") return "color3array";
  if (val.type_name() == "color4f[]") return "color4array";
  if (val.type_name() == "float2[]") return "vector2array";
  if (val.type_name() == "float3[]") return "vector3array";
  if (val.type_name() == "float4[]") return "vector4array";
  if (val.type_name() == "string[]") return "stringarray";
  if (val.type_id() == value::TYPE_ID_FLOAT) return "float";
  if (val.type_id() == value::TYPE_ID_INT32) return "integer";
  if (val.type_id() == value::TYPE_ID_BOOL) return "boolean";
  if (val.type_id() == value::TYPE_ID_STRING) return "string";
  if (val.type_id() == value::TYPE_ID_FLOAT2) return "vector2";
  if (val.type_id() == value::TYPE_ID_FLOAT3) return "vector3";
  if (val.type_id() == value::TYPE_ID_FLOAT4) return "vector4";
  if (val.type_id() == value::TYPE_ID_COLOR3F) return "color3";
  if (val.type_id() == value::TYPE_ID_COLOR4F) return "color4";
  if (val.type_id() == value::TYPE_ID_MATRIX3F) return "matrix33";
  if (val.type_id() == value::TYPE_ID_MATRIX4F) return "matrix44";
  if (val.type_id() == value::TYPE_ID_NORMAL3F) return "vector3";
  if (val.type_id() == value::TYPE_ID_ASSET_PATH) return "filename";
  if (val.type_id() == value::TYPE_ID_TOKEN) return "string";
  return "float";
}

// Helper to convert USD value to MaterialX value string
static std::string ToMtlxValueString(const value::Value &val) {
  if (auto a = val.as<std::vector<std::string>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      if (v.find(',') != std::string::npos || v.find(' ') != std::string::npos) {
        result += "\"" + v + "\"";
      } else {
        result += v;
      }
    }
    return result;
  }
  if (auto a = val.as<TypedArray<float>>()) {
    std::string result;
    for (float v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<int>>()) {
    std::string result;
    for (int v : *a) {
      if (!result.empty()) result += ", ";
      result += std::to_string(v);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<value::color3f>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v.r) + ", " + dtos(v.g) + ", " + dtos(v.b);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<value::color4f>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v.r) + ", " + dtos(v.g) + ", " + dtos(v.b) + ", " + dtos(v.a);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<value::float2>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v[0]) + ", " + dtos(v[1]);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<value::float3>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v[0]) + ", " + dtos(v[1]) + ", " + dtos(v[2]);
    }
    return result;
  }
  if (auto a = val.as<TypedArray<value::float4>>()) {
    std::string result;
    for (const auto &v : *a) {
      if (!result.empty()) result += ", ";
      result += dtos(v[0]) + ", " + dtos(v[1]) + ", " + dtos(v[2]) + ", " + dtos(v[3]);
    }
    return result;
  }
  if (auto f = val.as<float>()) {
    return dtos(*f);
  }
  if (auto i = val.as<int>()) {
    return std::to_string(*i);
  }
  if (auto b = val.as<bool>()) {
    return *b ? "true" : "false";
  }
  if (auto s = val.as<std::string>()) {
    return *s;
  }
  if (auto v2 = val.as<value::float2>()) {
    return dtos((*v2)[0]) + ", " + dtos((*v2)[1]);
  }
  if (auto v3 = val.as<value::float3>()) {
    return dtos((*v3)[0]) + ", " + dtos((*v3)[1]) + ", " + dtos((*v3)[2]);
  }
  if (auto v4 = val.as<value::float4>()) {
    return dtos((*v4)[0]) + ", " + dtos((*v4)[1]) + ", " + dtos((*v4)[2]) + ", " + dtos((*v4)[3]);
  }
  if (auto c3 = val.as<value::color3f>()) {
    return dtos(c3->r) + ", " + dtos(c3->g) + ", " + dtos(c3->b);
  }
  if (auto c4 = val.as<value::color4f>()) {
    return dtos(c4->r) + ", " + dtos(c4->g) + ", " + dtos(c4->b) + ", " + dtos(c4->a);
  }
  if (auto n3 = val.as<value::normal3f>()) {
    return dtos(n3->x) + ", " + dtos(n3->y) + ", " + dtos(n3->z);
  }
  if (auto ap = val.as<value::AssetPath>()) {
    return ap->GetAssetPath();
  }
  if (auto m = val.as<value::matrix3f>()) {
    std::string result;
    for (size_t i = 0; i < 3; ++i) {
      for (size_t j = 0; j < 3; ++j) {
        if (!result.empty()) result += ", ";
        result += dtos(m->m[i][j]);
      }
    }
    return result;
  }
  if (auto m = val.as<value::matrix4f>()) {
    std::string result;
    for (size_t i = 0; i < 4; ++i) {
      for (size_t j = 0; j < 4; ++j) {
        if (!result.empty()) result += ", ";
        result += dtos(m->m[i][j]);
      }
    }
    return result;
  }
  if (auto t = val.as<value::token>()) {
    return t->str();
  }
  return "";
}

// Helper function to serialize nodegraphs to MaterialX XML
static std::string ToMtlxAttributeType(const Attribute &attr) {
  if (attr.get_var().has_value() && !attr.get_var().has_timesamples()) {
    const std::string value_type = ToMtlxTypeString(attr.get_var().value_raw());
    if (!value_type.empty()) return value_type;
  }

  const std::string type_name = attr.type_name();
  if (type_name == "color3f") return "color3";
  if (type_name == "float2") return "vector2";
  if (type_name == "float3") return "vector3";
  if (type_name == "float4") return "vector4";
  if (type_name == "bool") return "boolean";
  return type_name.empty() ? "float" : type_name;
}

static void SerializeNodeGraphInputs(const PrimSpec &ng_ps,
                                     std::stringstream &ss) {
  for (const auto &prop_item : ng_ps.props()) {
    const std::string &prop_name = prop_item.first;
    if (prop_name.find("inputs:") != 0 || !prop_item.second.is_attribute()) {
      continue;
    }

    const Attribute &attr = prop_item.second.get_attribute();
    const std::string input_name = prop_name.substr(7);
    if (attr.has_connections() && !attr.connections().empty()) {
      const std::string full_path = attr.connections()[0].full_path_name();
      const size_t dot = full_path.find('.');
      const std::string node = dot == std::string::npos
                                   ? full_path
                                   : full_path.substr(0, dot);
      std::string output;
      const std::string marker = ".outputs:";
      if (dot != std::string::npos &&
          full_path.compare(dot, marker.size(), marker) == 0) {
        output = full_path.substr(dot + marker.size());
      }
      ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(input_name)
         << "\" type=\"" << EscapeXML(ToMtlxAttributeType(attr))
         << "\" nodename=\"" << EscapeXML(node) << "\"";
      if (!output.empty()) {
        ss << " output=\"" << EscapeXML(output) << "\"";
      }
      ss << " />\n";
      continue;
    }

    if (!attr.get_var().has_value() || attr.get_var().has_timesamples()) {
      continue;
    }
    const value::Value value = attr.get_var().value_raw();
    const std::string value_str = ToMtlxValueString(value);
    if (value_str.empty()) continue;
    ss << pprint::Indent(2) << "<input name=\"" << EscapeXML(input_name)
       << "\" type=\"" << EscapeXML(ToMtlxTypeString(value))
       << "\" value=\"" << EscapeXML(value_str) << "\" />\n";
  }
}

static bool SerializeNodeGraphs(const std::map<std::string, PrimSpec> &nodegraphs,
                                std::stringstream &ss, std::string *warn, std::string *err) {
  (void)warn;
  (void)err;

  for (const auto &ng_item : nodegraphs) {
    const std::string &ng_name = ng_item.first;
    const PrimSpec &ng_ps = ng_item.second;

    ss << pprint::Indent(1) << "<nodegraph name=\"" << EscapeXML(ng_name)
       << "\">\n";

    SerializeNodeGraphInputs(ng_ps, ss);

    // Serialize child nodes
    for (const auto &child_ps : ng_ps.children()) {
      std::string node_name = child_ps.name();

      // Get info:id to determine node category
      std::string info_id;
      auto info_it = child_ps.props().find(kShaderInfoId);
      if (info_it != child_ps.props().end() && info_it->second.is_attribute()) {
        const Attribute &attr = info_it->second.get_attribute();
        if (auto tok = attr.get_value<value::token>()) {
          info_id = tok->str();
        }
      }

      if (info_id.empty()) {
        // Skip nodes without info:id
        continue;
      }

      std::string category = ExtractNodeCategory(info_id);
      std::string node_type = ExtractNodeType(info_id);

      ss << pprint::Indent(2) << "<" << EscapeXML(category)
         << " name=\"" << EscapeXML(node_name) << "\" type=\""
         << EscapeXML(node_type) << "\">\n";

      // Serialize inputs
      for (const auto &prop_item : child_ps.props()) {
        const std::string &prop_name = prop_item.first;

        // Skip non-input properties
        if (prop_name.find("inputs:") != 0) continue;

        std::string input_name = prop_name.substr(7); // Remove "inputs:" prefix

        if (prop_item.second.is_attribute()) {
          const Attribute &attr = prop_item.second.get_attribute();

          // Check if it's a connection
          if (attr.has_connections() && !attr.connections().empty()) {
            // Extract nodename from connection path
            const Path &conn_path = attr.connections()[0];
            std::string full_path = conn_path.full_path_name();

            // Parse connection: "nodename.outputs:out" or just "nodename"
            size_t dot_pos = full_path.find('.');
            std::string nodename_ref = (dot_pos != std::string::npos) ?
                                       full_path.substr(0, dot_pos) : full_path;
            const std::string output_marker = ".outputs:";
            const std::string output_ref =
                (dot_pos != std::string::npos &&
                 full_path.compare(dot_pos, output_marker.size(), output_marker) == 0)
                    ? full_path.substr(dot_pos + output_marker.size())
                    : std::string();

            // Determine type from the connected node's output or use default
            ss << pprint::Indent(3) << "<input name=\""
               << EscapeXML(input_name) << "\" type=\""
               << EscapeXML(ToMtlxAttributeType(attr)) << "\" nodename=\""
               << EscapeXML(nodename_ref) << "\"";
            if (!output_ref.empty()) {
              ss << " output=\"" << EscapeXML(output_ref) << "\"";
            }
            ss << " />\n";
          } else {
            // It's a value
            value::Value val;
            if (attr.get_var().has_value() && !attr.get_var().has_timesamples()) {
              val = attr.get_var().value_raw();
            }

            if (val.type_id() != value::TYPE_ID_NULL) {
              std::string type_str = ToMtlxTypeString(val);
              std::string value_str = ToMtlxValueString(val);

              if (!value_str.empty()) {
                ss << pprint::Indent(3) << "<input name=\""
                   << EscapeXML(input_name) << "\" type=\""
                   << EscapeXML(type_str) << "\" value=\""
                   << EscapeXML(value_str) << "\" />\n";
              }
            }
          }
        }
      }

      ss << pprint::Indent(2) << "</" << category << ">\n";
    }

    // Serialize outputs
    for (const auto &prop_item : ng_ps.props()) {
      const std::string &prop_name = prop_item.first;

      // Check if this is an output property
      if (prop_name.find("outputs:") != 0) continue;

      std::string output_name = prop_name.substr(8); // Remove "outputs:" prefix

      if (prop_item.second.is_attribute()) {
        const Attribute &attr = prop_item.second.get_attribute();

        if (attr.has_connections() && !attr.connections().empty()) {
          const Path &conn_path = attr.connections()[0];
          std::string full_path = conn_path.full_path_name();

          // Parse connection path to extract nodename
          size_t dot_pos = full_path.find('.');
          std::string nodename_ref = (dot_pos != std::string::npos) ?
                                     full_path.substr(0, dot_pos) : full_path;
          const std::string output_marker = ".outputs:";
          const std::string output_ref =
              (dot_pos != std::string::npos &&
               full_path.compare(dot_pos, output_marker.size(), output_marker) == 0)
                  ? full_path.substr(dot_pos + output_marker.size())
                  : std::string();

          // Try to determine type from the connected node
          std::string output_type = ToMtlxAttributeType(attr);
          if (attr.type_name().empty()) {
            output_type = "color3";  // Default when the source omitted type.
            for (const auto &child_ps : ng_ps.children()) {
              if (child_ps.name() == nodename_ref) {
                auto info_it = child_ps.props().find(kShaderInfoId);
                if (info_it != child_ps.props().end() && info_it->second.is_attribute()) {
                  if (auto tok = info_it->second.get_attribute().get_value<value::token>()) {
                    output_type = ExtractNodeType(tok->str());
                  }
                }
                break;
              }
            }
          }

          ss << pprint::Indent(2) << "<output name=\""
             << EscapeXML(output_name) << "\" type=\""
             << EscapeXML(output_type) << "\" nodename=\""
             << EscapeXML(nodename_ref) << "\"";
          if (!output_ref.empty() && output_ref != "out") {
            ss << " output=\"" << EscapeXML(output_ref) << "\"";
          }
          ss << " />\n";
        }
      }
    }

    ss << pprint::Indent(1) << "</nodegraph>\n";
  }

  return true;
}

}  // namespace detail

static void AppendCustomShaderInputs(
    const std::string &shader_name, const std::string &closing_tag,
    const std::map<std::string, std::vector<MtlxCustomShaderInput>> &inputs,
    std::string *xml) {
  if (!xml) return;
  auto it = inputs.find(shader_name);
  if (it == inputs.end() || it->second.empty()) return;
  const size_t close = xml->rfind(closing_tag);
  if (close == std::string::npos) return;
  std::stringstream extra;
  for (const auto &input : it->second) {
    if (input.name.empty() || input.type.empty()) continue;
    extra << pprint::Indent(2) << "<input name=\""
          << detail::EscapeXML(input.name) << "\" type=\""
          << detail::EscapeXML(input.type) << "\" value=\""
          << detail::EscapeXML(input.value) << "\" />\n";
  }
  xml->insert(close, extra.str());
}

bool WriteMaterialXToString(const MtlxModel &mtlx, std::string &xml_str,
                            std::string *warn, std::string *err) {
  // Find shader name - use the first shader in the shaders map if available
  // Priority: shader key from shaders map > mtlx.shader_name
  std::string shader_name;
  if (!mtlx.shaders.empty()) {
    shader_name = mtlx.shaders.begin()->first;
  } else {
    shader_name = mtlx.shader_name;
  }

  // Get connections for this shader
  std::vector<MtlxShaderConnection> connections;
  auto it = mtlx.shader_connections.find(shader_name);
  if (it != mtlx.shader_connections.end()) {
    connections = it->second;
  }

  // OpenPBR is the primary active path (used by Blender exports)
  if (auto openpbr = mtlx.shader.as<MtlxOpenPBRSurface>()) {
    if (!detail::WriteMaterialXToString(*openpbr, shader_name, connections, mtlx.nodegraphs, mtlx.surface_materials, mtlx.looks, mtlx.color_space, mtlx.version, mtlx.cms, mtlx.cmsconfig, mtlx.name_space, mtlx.light_shaders, xml_str, warn, err)) return false;
    AppendCustomShaderInputs(shader_name, "</open_pbr_surface>", mtlx.custom_shader_inputs, &xml_str);
    return true;
  }

#if LIGHTUSD_MTLX_ENABLE_USDPREVIEWSURFACE_EXPORT
  if (auto usdps = mtlx.shader.as<MtlxUsdPreviewSurface>()) {
    if (!detail::WriteMaterialXToString(*usdps, shader_name, connections, mtlx.nodegraphs, mtlx.surface_materials, mtlx.looks, mtlx.color_space, mtlx.version, mtlx.cms, mtlx.cmsconfig, mtlx.name_space, mtlx.light_shaders, xml_str, warn, err)) return false;
    AppendCustomShaderInputs(shader_name, "</UsdPreviewSurface>", mtlx.custom_shader_inputs, &xml_str);
    return true;
  }
#endif

#if LIGHTUSD_MTLX_ENABLE_STANDARDSURFACE_EXPORT
  if (auto adskss = mtlx.shader.as<MtlxAutodeskStandardSurface>()) {
    if (!detail::WriteMaterialXToString(*adskss, shader_name, connections, mtlx.nodegraphs, mtlx.surface_materials, mtlx.looks, mtlx.color_space, mtlx.version, mtlx.cms, mtlx.cmsconfig, mtlx.name_space, mtlx.light_shaders, xml_str, warn, err)) return false;
    AppendCustomShaderInputs(shader_name, "</standard_surface>", mtlx.custom_shader_inputs, &xml_str);
    return true;
  }
#endif

  // Fallback error for unsupported shader types
  PUSH_ERROR_AND_RETURN("Unknown/unsupported shader type: " << mtlx.shader_name);

  return false;
}

}  // namespace lightusd

#endif  // LIGHTUSD_USE_USDMTLX
