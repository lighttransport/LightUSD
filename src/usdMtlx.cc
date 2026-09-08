// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.

#include <sstream>

#include "usdMtlx.hh"
#include "usdShade.hh"
#include "usdLux.hh"  // SphereLight/RectLight (no longer re-exported by lightusd.hh)
#include "safe-arithmetic.hh"

// Use built-in MaterialX parser instead of pugixml
#include "mtlx-usd-adapter.hh"

#if defined(LIGHTUSD_USE_USDMTLX)

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

#define PushWarn(msg) \
  do {                \
    if (warn) {       \
      (*warn) += msg; \
    }                 \
  } while (0);

#define PushError(msg) \
  do {                 \
    if (err) {         \
      (*err) += msg;   \
    }                  \
  } while (0);

namespace lightusd {

// defined in ascii-parser-base-types.cc
namespace ascii {

extern template bool AsciiParser::SepBy1BasicType<float>(
    const char sep, std::vector<float> *ret);

}  // namespace ascii

namespace detail {

template <typename T>
Property MakeProperty(const T &value) {
  Attribute attr(value);
  Property prop(attr, /* custom */ false);

  return prop;
}

bool is_supported_type(const std::string &typeName);

bool is_supported_type(const std::string &typeName) {
  if (typeName.compare("integer") == 0) return true;
  if (typeName.compare("boolean") == 0) return true;
  if (typeName.compare("float") == 0) return true;
  if (typeName.compare("color3") == 0) return true;
  if (typeName.compare("color4") == 0) return true;
  if (typeName.compare("vector2") == 0) return true;
  if (typeName.compare("vector3") == 0) return true;
  if (typeName.compare("vector4") == 0) return true;
  if (typeName.compare("matrix33") == 0) return true;
  if (typeName.compare("matrix44") == 0) return true;
  if (typeName.compare("string") == 0) return true;
  if (typeName.compare("filename") == 0) return true;

  if (typeName.compare("integerarray") == 0) return true;
  if (typeName.compare("floatarray") == 0) return true;
  if (typeName.compare("vector2array") == 0) return true;
  if (typeName.compare("vector3array") == 0) return true;
  if (typeName.compare("vector4array") == 0) return true;
  if (typeName.compare("color3array") == 0) return true;
  if (typeName.compare("color4array") == 0) return true;
  if (typeName.compare("stringarray") == 0) return true;

  // No matrixarray

  // if (typeName.compare("color") == 0) return true;
  // if (typeName.compare("geomname") == 0) return true;
  // if (typeName.compare("geomnamearray") == 0) return true;

  return false;
}

template <typename T>
bool ParseValue(lightusd::ascii::AsciiParser &parser, T &ret, std::string *err);

template <>
bool ParseValue<int>(lightusd::ascii::AsciiParser &parser, int &ret,
                     std::string *err) {
  int val;
  if (!parser.ReadBasicType(&val)) {
    PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse a value of type `{}`",
                                      value::TypeTraits<int>::type_name()));
  }

  ret = val;

  return true;
}

template <>
bool ParseValue<bool>(lightusd::ascii::AsciiParser &parser, bool &ret,
                      std::string *err) {
  bool val;
  if (!parser.ReadBasicType(&val)) {
    PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse a value of type `{}`",
                                      value::TypeTraits<bool>::type_name()));
  }

  ret = val;

  return true;
}

template <>
bool ParseValue<float>(lightusd::ascii::AsciiParser &parser, float &ret,
                       std::string *err) {
  float val;
  if (!parser.ReadBasicType(&val)) {
    PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse a value of type `{}`",
                                      value::TypeTraits<float>::type_name()));
  }

  ret = val;

  return true;
}

template <>
bool ParseValue<std::string>(lightusd::ascii::AsciiParser &parser,
                             std::string &ret, std::string *err) {
  std::string val;
  if (!parser.ReadBasicType(&val)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<std::string>::type_name()));
  }

  ret = val;

  return true;
}

template <>
bool ParseValue<value::float2>(lightusd::ascii::AsciiParser &parser,
                               value::float2 &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::float3>::type_name()));
  }

  if (values.size() != 2) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 2, but got {}",
        value::TypeTraits<value::float2>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];

  return true;
}

template <>
bool ParseValue<value::float3>(lightusd::ascii::AsciiParser &parser,
                               value::float3 &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::float3>::type_name()));
  }

  if (values.size() != 3) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 3, but got {}",
        value::TypeTraits<value::float3>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];
  ret[2] = values[2];

  return true;
}

template <>
bool ParseValue<value::vector3f>(lightusd::ascii::AsciiParser &parser,
                                 value::vector3f &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::vector3f>::type_name()));
  }

  if (values.size() != 3) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 3, but got {}",
        value::TypeTraits<value::vector3f>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];
  ret[2] = values[2];

  return true;
}

template <>
bool ParseValue<value::normal3f>(lightusd::ascii::AsciiParser &parser,
                                 value::normal3f &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::normal3f>::type_name()));
  }

  if (values.size() != 3) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 3, but got {}",
        value::TypeTraits<value::normal3f>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];
  ret[2] = values[2];

  return true;
}


template <>
bool ParseValue<value::color3f>(lightusd::ascii::AsciiParser &parser,
                                 value::color3f &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::color3f>::type_name()));
  }

  if (values.size() != 3) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 3, but got {}",
        value::TypeTraits<value::color3f>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];
  ret[2] = values[2];

  return true;
}

template <>
bool ParseValue<value::float4>(lightusd::ascii::AsciiParser &parser,
                               value::float4 &ret, std::string *err) {
  std::vector<float> values;
  if (!parser.SepBy1BasicType(',', &values)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to parse a value of type `{}`",
                    value::TypeTraits<value::float4>::type_name()));
  }

  if (values.size() != 4) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "type `{}` expects the number of elements is 4, but got {}",
        value::TypeTraits<value::float4>::type_name(), values.size()));
  }

  ret[0] = values[0];
  ret[1] = values[1];
  ret[2] = values[2];
  ret[3] = values[3];

  return true;
}

///
/// For MaterialX XML.
/// Parse string representation of Attribute value.
/// e.g. "0.0, 1.1" for vector2 type
/// NOTE: no parenthesis('(', '[') for vector and array type.
///
/// @param[in] typeName typeName(e.g. "vector2")
/// @param[in] str Ascii representation of value.
/// @param[out] value Ascii representation of value.
/// @param[out] err Parse error message when returning false.
///
///
/// Supported data type: boolean, float, color3, color4, vector2, vector3,
/// vector4, matrix33, matrix44, string, filename, integerarray, floatarray,
/// color3array, color4array, vector2array, vector3array, vector4array,
/// stringarray. Unsupported data type: geomname, geomnamearray
///
bool ParseMaterialXValue(const std::string &typeName, const std::string &str,
                         value::Value *value, std::string *err);

bool ParseMaterialXValue(const std::string &typeName, const std::string &str,
                         value::Value *value, std::string *err) {
  if (!value) {
    PUSH_ERROR_AND_RETURN("value is nullptr");
  }

  if (!is_supported_type(typeName)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Invalid/unsupported type: {}", typeName));
  }

  lightusd::StreamReader sr(reinterpret_cast<const uint8_t *>(str.data()),
                            str.size(), /* swap endian */ false);
  lightusd::ascii::AsciiParser parser(&sr);

  if (typeName.compare("integer") == 0) {
    int val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("boolean") == 0) {
    bool val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("float") == 0) {
    float val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("string") == 0 || typeName.compare("filename") == 0) {
    // Strings are already unquoted from XML attribute parsing
    (*value) = str;
  } else if (typeName.compare("vector2") == 0) {
    value::float2 val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("color3") == 0) {
    value::color3f val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("vector3") == 0) {
    value::float3 val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("color4") == 0) {
    value::float4 val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("vector4") == 0) {
    value::float4 val;
    if (!ParseValue(parser, val, err)) {
      return false;
    }
    (*value) = val;
  } else if (typeName.compare("matrix33") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN("Failed to parse a value of type `matrix33`");
    }
    if (values.size() != 9) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "type `matrix33` expects 9 elements, but got {}", values.size()));
    }
    value::matrix3f val;
    memcpy(&val.m[0][0], values.data(), sizeof(float) * 9);
    (*value) = val;
  } else if (typeName.compare("matrix44") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN("Failed to parse a value of type `matrix44`");
    }
    if (values.size() != 16) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "type `matrix44` expects 16 elements, but got {}", values.size()));
    }
    value::matrix4f val;
    memcpy(&val.m[0][0], values.data(), sizeof(float) * 16);
    (*value) = val;
  } else if (typeName.compare("integerarray") == 0) {
    // Parse comma-separated integers via float parser then convert
    std::vector<float> fvalues;
    if (!parser.SepBy1BasicType(',', &fvalues)) {
      PUSH_ERROR_AND_RETURN("Failed to parse a value of type `integerarray`");
    }
    std::vector<int> values(fvalues.size());
    for (size_t i = 0; i < fvalues.size(); i++) {
      values[i] = int(fvalues[i]);
    }
    (*value) = values;
  } else if (typeName.compare("floatarray") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN("Failed to parse a value of type `floatarray`");
    }
    (*value) = values;
  } else if (typeName.compare("color3array") == 0 ||
             typeName.compare("vector3array") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse a value of type `{}`", typeName));
    }
    if (values.size() % 3 != 0) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "type `{}` expects element count divisible by 3, but got {}", typeName, values.size()));
    }
    if (typeName.compare("color3array") == 0) {
      std::vector<value::color3f> arr(values.size() / 3);
      size_t byte_count;
      if (!safe::mul(values.size(), sizeof(float), &byte_count)) {
        return false;
      }
      memcpy(arr.data(), values.data(), byte_count);
      (*value) = arr;
    } else {
      std::vector<value::float3> arr(values.size() / 3);
      size_t byte_count2;
      if (!safe::mul(values.size(), sizeof(float), &byte_count2)) {
        return false;
      }
      memcpy(arr.data(), values.data(), byte_count2);
      (*value) = arr;
    }
  } else if (typeName.compare("color4array") == 0 ||
             typeName.compare("vector4array") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse a value of type `{}`", typeName));
    }
    if (values.size() % 4 != 0) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "type `{}` expects element count divisible by 4, but got {}", typeName, values.size()));
    }
    if (typeName.compare("color4array") == 0) {
      std::vector<value::color4f> arr(values.size() / 4);
      size_t byte_count;
      if (!safe::mul(values.size(), sizeof(float), &byte_count)) {
        return false;
      }
      memcpy(arr.data(), values.data(), byte_count);
      (*value) = arr;
    } else {
      std::vector<value::float4> arr(values.size() / 4);
      size_t byte_count2;
      if (!safe::mul(values.size(), sizeof(float), &byte_count2)) {
        return false;
      }
      memcpy(arr.data(), values.data(), byte_count2);
      (*value) = arr;
    }
  } else if (typeName.compare("vector2array") == 0) {
    std::vector<float> values;
    if (!parser.SepBy1BasicType(',', &values)) {
      PUSH_ERROR_AND_RETURN("Failed to parse a value of type `vector2array`");
    }
    if (values.size() % 2 != 0) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "type `vector2array` expects element count divisible by 2, but got {}", values.size()));
    }
    std::vector<value::float2> arr(values.size() / 2);
    memcpy(arr.data(), values.data(), sizeof(float) * values.size());
    (*value) = arr;
  } else if (typeName.compare("stringarray") == 0) {
    // MaterialX string arrays are comma-separated. Keep quoted commas inside
    // one element; XML attributes commonly contain either bare identifiers or
    // quoted strings.
    std::vector<std::string> values;
    std::string item;
    char quote = 0;
    for (const char c : str) {
      if (c == '\'' || c == '"') {
        if (quote == 0) quote = c;
        else if (quote == c) quote = 0;
        else item.push_back(c);
      } else if (c == ',' && quote == 0) {
        const size_t begin = item.find_first_not_of(" \t\r\n");
        const size_t end = item.find_last_not_of(" \t\r\n");
        values.push_back(begin == std::string::npos ? std::string() :
                         item.substr(begin, end - begin + 1));
        item.clear();
      } else {
        item.push_back(c);
      }
    }
    const size_t begin = item.find_first_not_of(" \t\r\n");
    const size_t end = item.find_last_not_of(" \t\r\n");
    if (begin != std::string::npos) values.push_back(item.substr(begin, end - begin + 1));
    else if (!str.empty()) values.emplace_back();
    (*value) = std::move(values);
  } else {
    PUSH_ERROR_AND_RETURN("Unsupported type: " + typeName);
  }

  return true;
}

template <typename T>
bool ParseMaterialXValue(const std::string &str, T *value, std::string *err) {
  lightusd::StreamReader sr(reinterpret_cast<const uint8_t *>(str.data()),
                            str.size(), /* swap endian */ false);
  lightusd::ascii::AsciiParser parser(&sr);

  T val{};

  if (!ParseValue(parser, val, err)) {
    return false;
  }

  (*value) = val;
  return true;
}

// Specialization for std::string - MaterialX XML attributes are already unquoted
template <>
bool ParseMaterialXValue<std::string>(const std::string &str, std::string *value, std::string *err) {
  (void)err;
  (*value) = str;
  return true;
}


// ============================================================================
// Node Converters - ACTIVE PATH
// These functions convert MaterialX XML nodes to USD PrimSpec representations.
// Used for importing Blender MaterialX NodeGraphs.
// ============================================================================

// Initialize a PrimSpec as a Shader with the given info:id
static void InitializeShader(PrimSpec &ps, const std::string &info_id) {
  ps.specifier() = Specifier::Def;
  ps.typeName() = kShader;
  ps.props()[kShaderInfoId] = Property(Attribute::Uniform(value::token(info_id)));
}

static bool ConvertPlace2d(const lightusd::mtlx::pugi::xml_node &node, PrimSpec &ps,
                           std::string * /* warn */, std::string *err) {
  // texcoord(vector2). default index=0 uv coordinate
  // pivot(vector2). default (0, 0)
  // scale(vector2). default (1, 1)
  // rotate(float). in degrees, Counter-clockwise
  // offset(vector2)

  // Get node name
  lightusd::mtlx::pugi::xml_attribute name_attr = node.attribute("name");
  if (name_attr) {
    ps.name() = name_attr.as_string();
  }

  if (lightusd::mtlx::pugi::xml_attribute texcoord_attr = node.attribute("texcoord")) {
    // MaterialX texcoord attribute specifies the UV set name (e.g., "UV0", "st")
    // Map to USD's inputs:varname for UsdPrimvarReader
    std::string texcoord_name = texcoord_attr.as_string();
    if (!texcoord_name.empty()) {
      ps.props()["inputs:varname"] = Property(Attribute::Uniform(value::token(texcoord_name)));
    }
  }

  if (lightusd::mtlx::pugi::xml_attribute pivot_attr = node.attribute("pivot")) {
    value::float2 value{};
    if (ParseMaterialXValue(pivot_attr.as_string(), &value, err)) {
      ps.props()["inputs:pivot"] = Property(Attribute::Uniform(value));
    }
  }

  if (lightusd::mtlx::pugi::xml_attribute scale_attr = node.attribute("scale")) {
    value::float2 value{};
    if (!ParseMaterialXValue(scale_attr.as_string(), &value, err)) {
      PUSH_ERROR_AND_RETURN(
          "Failed to parse `scale` attribute of `place2d`.\n");
    }
    ps.props()["inputs:scale"] = Property(Attribute::Uniform(value));
  }

  if (lightusd::mtlx::pugi::xml_attribute rotate_attr = node.attribute("rotate")) {
    float value{};
    if (!ParseMaterialXValue(rotate_attr.as_string(), &value, err)) {
      PUSH_ERROR_AND_RETURN(
          "Failed to parse `rotate` attribute of `place2d`.\n");
    }
    ps.props()["inputs:rotation"] = Property(Attribute::Uniform(value));
  }

  lightusd::mtlx::pugi::xml_attribute offset_attr = node.attribute("offset");
  if (offset_attr) {
    value::float2 value{};
    if (!ParseMaterialXValue(offset_attr.as_string(), &value, err)) {
      PUSH_ERROR_AND_RETURN(
          "Failed to parse `offset` attribute of `place2d`.\n");
    }
    ps.props()["inputs:translation"] = Property(Attribute::Uniform(value));
  }

  InitializeShader(ps, kUsdTransform2d);

  return true;
}

// Convert MaterialX tiledimage node to USD UsdUVTexture
static bool ConvertTiledImage(const lightusd::mtlx::pugi::xml_node &node, PrimSpec &ps,
                              std::string *warn, std::string *err) {
  (void)warn;

  // Get node name and type
  lightusd::mtlx::pugi::xml_attribute name_attr = node.attribute("name");
  if (name_attr) {
    ps.name() = name_attr.as_string();
  }

  // Parse inputs
  for (auto inp : node.children("input")) {
    std::string input_name;
    std::string input_type;
    std::string input_value;

    lightusd::mtlx::pugi::xml_attribute inp_name_attr = inp.attribute("name");
    if (inp_name_attr) {
      input_name = inp_name_attr.as_string();
    }

    lightusd::mtlx::pugi::xml_attribute type_attr = inp.attribute("type");
    if (type_attr) {
      input_type = type_attr.as_string();
    }

    lightusd::mtlx::pugi::xml_attribute value_attr = inp.attribute("value");
    if (value_attr) {
      input_value = value_attr.as_string();
    }

    // Check for per-input colorspace attribute
    lightusd::mtlx::pugi::xml_attribute colorspace_attr = inp.attribute("colorspace");
    std::string input_colorspace;
    if (colorspace_attr) {
      input_colorspace = colorspace_attr.as_string();
    }

    // Map MaterialX inputs to USD inputs
    std::string prop_key;  // Track which property was set for colorspace propagation
    if (input_name == "file" && input_type == "filename") {
      // Convert filename to asset path
      prop_key = "inputs:file";
      ps.props()[prop_key] = Property(Attribute::Uniform(value::AssetPath(input_value)));
    } else if (input_name == "uvtiling" && input_type == "vector2") {
      value::float2 tiling;
      if (ParseMaterialXValue(input_value, &tiling, err)) {
        // Store for potential scale transformation
        ps.props()["inputs:scale"] = Property(Attribute::Uniform(tiling));
      }
    } else if (input_name == "uvoffset" && input_type == "vector2") {
      value::float2 offset;
      if (ParseMaterialXValue(input_value, &offset, err)) {
        ps.props()["inputs:translation"] = Property(Attribute::Uniform(offset));
      }
    } else if (input_name == "default") {
      // Fallback value
      if (input_type == "color3") {
        value::color3f fallback;
        if (ParseMaterialXValue(input_value, &fallback, err)) {
          ps.props()["inputs:fallback"] = Property(Attribute::Uniform(value::color4f{fallback.r, fallback.g, fallback.b, 1.0f}));
        }
      } else if (input_type == "float") {
        float fallback;
        if (ParseMaterialXValue(input_value, &fallback, err)) {
          ps.props()["inputs:fallback"] = Property(Attribute::Uniform(value::color4f{fallback, fallback, fallback, 1.0f}));
        }
      }
    }

    // Propagate per-input colorspace to the property's attribute metadata
    if (!input_colorspace.empty() && !prop_key.empty()) {
      Attribute *attr_ptr = ps.props()[prop_key].get_attribute_or_null();
      if (attr_ptr) {
        attr_ptr->metas().set_colorSpace(input_colorspace);
      }
    }
  }

  InitializeShader(ps, kUsdUVTexture);

  return true;
}

// Convert MaterialX texcoord node to USD UsdPrimvarReader_float2
static bool ConvertTexCoord(const lightusd::mtlx::pugi::xml_node &node, PrimSpec &ps,
                            const MtlxConfig &config,
                            std::string *warn, std::string *err) {
  (void)warn;

  // Get node name
  lightusd::mtlx::pugi::xml_attribute name_attr = node.attribute("name");
  if (name_attr) {
    ps.name() = name_attr.as_string();
  }

  // Parse index attribute (UV set index)
  int uv_index = 0;
  lightusd::mtlx::pugi::xml_attribute index_attr = node.attribute("index");
  if (index_attr) {
    std::string index_str = index_attr.as_string();
    if (!ParseMaterialXValue(index_str, &uv_index, err)) {
      PUSH_ERROR_AND_RETURN("Failed to parse `index` attribute of `texcoord`.\n");
    }
  }

  // Map to USD primvar name convention using MtlxConfig
  // Similar to OpenUSD's USDMTLX_PRIMARY_UV_NAME environment variable
  std::string varname;
  if (uv_index == 0) {
    // Primary UV: use config.primary_uv_name, fallback to "st" if empty
    varname = config.primary_uv_name.empty() ? "st" : config.primary_uv_name;
  } else {
    // Secondary UV: use config.secondary_uv_name_prefix + index, fallback to "st" + index
    std::string prefix = config.secondary_uv_name_prefix.empty() ? "st" : config.secondary_uv_name_prefix;
    varname = prefix + std::to_string(uv_index);
  }
  ps.props()["inputs:varname"] = Property(Attribute::Uniform(varname));

  // Set fallback to (0, 0)
  ps.props()["inputs:fallback"] = Property(Attribute::Uniform(value::float2{0.0f, 0.0f}));

  InitializeShader(ps, kUsdPrimvarReader_float2);

  return true;
}

// ConvertConstant/ConvertMultiply/ConvertAdd/ConvertMix/ConvertNoise are now
// handled by ConvertGenericNode via the dispatch table in ConvertSingleNode.

// ============================================================================
// Generic Node Converters
// These handle the common pattern of parsing inputs and setting info:id
// ============================================================================

// Helper to parse a single input element and store in PrimSpec
static bool ParseInputElement(const lightusd::mtlx::pugi::xml_node &inp, PrimSpec &ps,
                              std::string *err) {
  std::string input_name;
  lightusd::mtlx::pugi::xml_attribute inp_name_attr = inp.attribute("name");
  if (inp_name_attr) {
    input_name = inp_name_attr.as_string();
  }

  std::string prop_name = "inputs:" + input_name;

  // Check for per-input colorspace attribute (MaterialX allows per-input colorspace override)
  lightusd::mtlx::pugi::xml_attribute colorspace_attr = inp.attribute("colorspace");
  std::string input_colorspace;
  if (colorspace_attr) {
    input_colorspace = colorspace_attr.as_string();
  }

  // Check for connection first (nodename or nodegraph)
  lightusd::mtlx::pugi::xml_attribute nodename_attr = inp.attribute("nodename");
  lightusd::mtlx::pugi::xml_attribute nodegraph_attr = inp.attribute("nodegraph");
  lightusd::mtlx::pugi::xml_attribute output_attr = inp.attribute("output");

  if (nodename_attr || nodegraph_attr) {
    // This is a connection - store connection info
    Attribute attr;
    lightusd::mtlx::pugi::xml_attribute type_attr = inp.attribute("type");
    if (type_attr) {
      attr.set_type_name(type_attr.as_string());
    }
    if (nodename_attr) {
      std::string target = std::string(nodename_attr.as_string()) + ".outputs:out";
      attr.set_connections({Path(target, "")});
    } else if (nodegraph_attr) {
      std::string output_name = output_attr ? output_attr.as_string() : "out";
      std::string target = std::string(nodegraph_attr.as_string()) + ".outputs:" + output_name;
      attr.set_connections({Path(target, "")});
    }
    ps.props()[prop_name] = Property(attr);
    if (!input_colorspace.empty()) {
      Attribute *attr_ptr = ps.props()[prop_name].get_attribute_or_null();
      if (attr_ptr) {
        attr_ptr->metas().set_colorSpace(input_colorspace);
      }
    }
    return true;
  }

  // Parse value
  lightusd::mtlx::pugi::xml_attribute value_attr = inp.attribute("value");
  if (!value_attr) {
    return true;  // No value, skip
  }

  std::string input_value = value_attr.as_string();
  lightusd::mtlx::pugi::xml_attribute type_attr = inp.attribute("type");
  std::string type_str = type_attr ? type_attr.as_string() : "float";

  if (type_str == "float") {
    float val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "color3") {
    value::color3f val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "color4") {
    value::Value parsed;
    if (ParseMaterialXValue(type_str, input_value, &parsed, err)) {
      if (auto val = parsed.as<value::color4f>()) {
        ps.props()[prop_name] = Property(Attribute::Uniform(*val));
      }
    }
  } else if (type_str == "vector2") {
    value::float2 val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "vector3") {
    value::float3 val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "vector4") {
    value::float4 val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "matrix33") {
    value::Value parsed;
    if (ParseMaterialXValue(type_str, input_value, &parsed, err)) {
      if (auto val = parsed.as<value::matrix3f>()) {
        ps.props()[prop_name] = Property(Attribute::Uniform(*val));
      }
    }
  } else if (type_str == "matrix44") {
    value::Value parsed;
    if (ParseMaterialXValue(type_str, input_value, &parsed, err)) {
      if (auto val = parsed.as<value::matrix4f>()) {
        ps.props()[prop_name] = Property(Attribute::Uniform(*val));
      }
    }
  } else if (type_str == "integer") {
    int val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "boolean") {
    bool val;
    if (ParseMaterialXValue(input_value, &val, err)) {
      ps.props()[prop_name] = Property(Attribute::Uniform(val));
    }
  } else if (type_str == "string") {
    ps.props()[prop_name] = Property(Attribute::Uniform(input_value));
  } else if (type_str == "filename") {
    ps.props()[prop_name] = Property(Attribute::Uniform(value::AssetPath(input_value)));
  } else if (type_str == "floatarray" || type_str == "integerarray" ||
             type_str == "color3array" || type_str == "color4array" ||
             type_str == "vector2array" || type_str == "vector3array" ||
             type_str == "vector4array" || type_str == "stringarray") {
    auto store_array = [&](auto &&array) {
      primvar::PrimVar var;
      var.set_value(std::forward<decltype(array)>(array));
      Attribute attr;
      attr.set_var(std::move(var));
      attr.variability() = Variability::Uniform;
      ps.props()[prop_name] = Property(std::move(attr));
    };
    value::Value parsed;
    if (ParseMaterialXValue(type_str, input_value, &parsed, err)) {
      if (type_str == "floatarray") {
        if (auto val = parsed.as<std::vector<float>>())
          store_array(TypedArray<float>(val->data(), val->size()));
      } else if (type_str == "integerarray") {
        if (auto val = parsed.as<std::vector<int>>())
          store_array(TypedArray<int>(val->data(), val->size()));
      } else if (type_str == "color3array") {
        if (auto val = parsed.as<std::vector<value::color3f>>())
          store_array(TypedArray<value::color3f>(val->data(), val->size()));
      } else if (type_str == "color4array") {
        if (auto val = parsed.as<std::vector<value::color4f>>())
          store_array(TypedArray<value::color4f>(val->data(), val->size()));
      } else if (type_str == "vector2array") {
        if (auto val = parsed.as<std::vector<value::float2>>())
          store_array(TypedArray<value::float2>(val->data(), val->size()));
      } else if (type_str == "vector3array") {
        if (auto val = parsed.as<std::vector<value::float3>>())
          store_array(TypedArray<value::float3>(val->data(), val->size()));
      } else if (type_str == "vector4array") {
        if (auto val = parsed.as<std::vector<value::float4>>())
          store_array(TypedArray<value::float4>(val->data(), val->size()));
      } else if (type_str == "stringarray") {
        if (auto val = parsed.as<std::vector<std::string>>())
          store_array(*val);
      }
    }
  }

  // Apply per-input colorspace if present
  if (!input_colorspace.empty()) {
    auto it = ps.props().find(prop_name);
    if (it != ps.props().end() && it->second.is_attribute()) {
      Attribute *attr_ptr = it->second.get_attribute_or_null();
      if (attr_ptr) {
        attr_ptr->metas().set_colorSpace(input_colorspace);
      }
    }
  }

  return true;
}

// Generic converter for any MaterialX node - parses all inputs and sets info:id
static bool ConvertGenericNode(const lightusd::mtlx::pugi::xml_node &node, PrimSpec &ps,
                               const std::string &info_id,
                               std::string *warn, std::string *err) {
  (void)warn;

  lightusd::mtlx::pugi::xml_attribute name_attr = node.attribute("name");
  if (name_attr) {
    ps.name() = name_attr.as_string();
  }

  // Parse all inputs
  for (auto inp : node.children("input")) {
    if (!ParseInputElement(inp, ps, err)) {
      return false;
    }
  }

  InitializeShader(ps, info_id);

  return true;
}

// ============================================================================
// Node dispatch table — maps MaterialX node names to info:id generation.
// Most nodes use "ND_<op_name>_<type>" where type comes from the node's
// "type" attribute. A few use a fixed info:id string.
// ============================================================================

// Helper: get the node's "type" attribute or a default
static std::string GetNodeTypeAttr(const lightusd::mtlx::pugi::xml_node &node,
                                   const char *default_type) {
  lightusd::mtlx::pugi::xml_attribute ta = node.attribute("type");
  return ta ? ta.as_string() : default_type;
}

// Helper: find the "type" attribute of the named input child
static std::string GetInputTypeAttr(const lightusd::mtlx::pugi::xml_node &node,
                                    const char *input_name, const char *default_type) {
  for (auto inp : node.children("input")) {
    lightusd::mtlx::pugi::xml_attribute n = inp.attribute("name");
    if (n && std::string(n.as_string()) == input_name) {
      lightusd::mtlx::pugi::xml_attribute t = inp.attribute("type");
      if (t) return t.as_string();
      break;
    }
  }
  return default_type;
}

// Dispatch entry: how to build info:id for a generic node
struct NodeDispatchEntry {
  const char *op_name;       // prefix for "ND_<op_name>_<type>" (nullptr → use fixed_id)
  const char *default_type;  // default type if node has no "type" attribute
  const char *fixed_id;      // if non-null, use this as the full info:id (ignores op_name)
};

// Table of nodes handled by ConvertGenericNode with computed info:id.
// Nodes NOT in this table are handled as special cases (place2d, tiledimage, etc.)
static const std::pair<const char*, NodeDispatchEntry> kNodeDispatchPairs[] = {
  // Arithmetic with custom info:id (preserving existing behavior)
  {"multiply",        {nullptr, nullptr, "MaterialXMultiply"}},
  {"add",             {nullptr, nullptr, "MaterialXMultiply"}},  // same as multiply
  {"subtract",        {nullptr, nullptr, "MaterialXMultiply"}},  // same as multiply
  {"mix",             {nullptr, nullptr, "MaterialXMix"}},
  {"noise2d",         {nullptr, nullptr, "MaterialXNoise"}},
  {"noise3d",         {nullptr, nullptr, "MaterialXNoise"}},
  {"cellnoise2d",     {nullptr, nullptr, "MaterialXNoise"}},
  {"cellnoise3d",     {nullptr, nullptr, "MaterialXNoise"}},
  {"worleynoise2d",   {nullptr, nullptr, "MaterialXNoise"}},
  {"worleynoise3d",   {nullptr, nullptr, "MaterialXNoise"}},
  {"fractal3d",       {nullptr, nullptr, "MaterialXNoise"}},
  // Binary ops: ND_<name>_<type>
  {"divide",       {"divide",       "float", nullptr}},
  {"power",        {"power",        "float", nullptr}},
  {"min",          {"min",          "float", nullptr}},
  {"max",          {"max",          "float", nullptr}},
  {"modulo",       {"modulo",       "float", nullptr}},
  {"atan2",        {"atan2",        "float", nullptr}},
  {"dotproduct",   {"dotproduct",   "float", nullptr}},
  {"crossproduct", {"crossproduct", "float", nullptr}},
  // Unary ops: ND_<name>_<type>
  {"sqrt",       {"sqrt",       "float", nullptr}},
  {"absval",     {"absval",     "float", nullptr}},
  {"sign",       {"sign",       "float", nullptr}},
  {"floor",      {"floor",      "float", nullptr}},
  {"ceil",       {"ceil",       "float", nullptr}},
  {"round",      {"round",      "float", nullptr}},
  {"sin",        {"sin",        "float", nullptr}},
  {"cos",        {"cos",        "float", nullptr}},
  {"tan",        {"tan",        "float", nullptr}},
  {"asin",       {"asin",       "float", nullptr}},
  {"acos",       {"acos",       "float", nullptr}},
  {"atan",       {"atan",       "float", nullptr}},
  {"exp",        {"exp",        "float", nullptr}},
  {"ln",         {"ln",         "float", nullptr}},
  {"log2",       {"log2",       "float", nullptr}},
  {"normalize",  {"normalize",  "float", nullptr}},
  {"magnitude",  {"magnitude",  "float", nullptr}},
  {"luminance",  {"luminance",  "float", nullptr}},
  {"invert",     {"invert",     "float", nullptr}},
  {"saturate",   {"saturate",   "float", nullptr}},
  {"hueshift",   {"hueshift",   "float", nullptr}},
  {"rgbtohsv",   {"rgbtohsv",   "float", nullptr}},
  {"hsvtorgb",   {"hsvtorgb",   "float", nullptr}},
  // Clamp/remap
  {"clamp",      {"clamp",      "float", nullptr}},
  {"remap",      {"remap",      "float", nullptr}},
  {"range",      {"remap",      "float", nullptr}},  // alias
  {"smoothstep", {"smoothstep", "float", nullptr}},
  // Channel ops
  {"combine2",   {"combine2",   "color3", nullptr}},
  {"combine3",   {"combine3",   "color3", nullptr}},
  {"combine4",   {"combine4",   "color3", nullptr}},
  {"swizzle",    {"swizzle",    "color3", nullptr}},
  // Color ops
  {"hsvadjust",  {nullptr, nullptr, "ND_hsvadjust_color3"}},
  // Geometry nodes: ND_<geom_type>_<type>
  {"position",      {"position",      "vector3", nullptr}},
  {"normal",        {"normal",        "vector3", nullptr}},
  {"tangent",       {"tangent",       "vector3", nullptr}},
  {"bitangent",     {"bitangent",     "vector3", nullptr}},
  {"geomcolor",     {"geomcolor",     "vector3", nullptr}},
  {"geompropvalue", {"geompropvalue", "vector3", nullptr}},
  // Rotation
  {"rotate3d",  {nullptr, nullptr, "ND_rotate3d_vector3"}},
  // Conditional ops: ND_<cond>_<type>
  {"ifgreater",   {"ifgreater",   "float", nullptr}},
  {"ifgreatereq", {"ifgreatereq", "float", nullptr}},
  {"ifless",      {"ifless",      "float", nullptr}},
  {"iflesseq",    {"iflesseq",    "float", nullptr}},
  {"ifequal",     {"ifequal",     "float", nullptr}},
  {"switch",      {"switch",      "float", nullptr}},
  // Constant node
  {"constant",    {nullptr, nullptr, "MaterialXConstant"}},
};

static const lightusd::HashMap<std::string, NodeDispatchEntry> &GetNodeDispatchTable() {
  static const lightusd::HashMap<std::string, NodeDispatchEntry> table(
      std::begin(kNodeDispatchPairs), std::end(kNodeDispatchPairs));
  return table;
}

// Helper to convert a single MaterialX node to PrimSpec
// Returns true if successful (including skip case), false on error
// Sets is_skip to true if the node should be skipped (input/output)
static bool ConvertSingleNode(const lightusd::mtlx::pugi::xml_node &node,
                              PrimSpec &ps, bool &is_skip,
                              const MtlxConfig &config,
                              std::string *warn, std::string *err) {
  is_skip = false;
  std::string node_name = node.name();

  // Skip input/output nodes — handled separately
  if (node_name == "input" || node_name == "output") {
    is_skip = true;
    return true;
  }

  // Special-case nodes with custom conversion logic
  if (node_name == "place2d") {
    return ConvertPlace2d(node, ps, warn, err);
  }
  if (node_name == "tiledimage" || node_name == "image") {
    return ConvertTiledImage(node, ps, warn, err);
  }
  if (node_name == "texcoord") {
    return ConvertTexCoord(node, ps, config, warn, err);
  }
  // "extract" needs the input "in" type, not the node type
  if (node_name == "extract") {
    std::string info_id = "ND_extract_" + GetInputTypeAttr(node, "in", "color3");
    return ConvertGenericNode(node, ps, info_id, warn, err);
  }

  // "convert" needs from-type (input "in") and to-type (node type)
  if (node_name == "convert") {
    std::string from = GetInputTypeAttr(node, "in", "color3");
    std::string to = GetNodeTypeAttr(node, "vector3");
    return ConvertGenericNode(node, ps, "ND_convert_" + from + "_" + to, warn, err);
  }

  // Table-driven dispatch for all standard nodes
  const auto &table = GetNodeDispatchTable();
  auto it = table.find(node_name);
  if (it != table.end()) {
    const auto &e = it->second;
    std::string info_id;
    if (e.fixed_id) {
      info_id = e.fixed_id;
    } else {
      info_id = "ND_" + std::string(e.op_name) + "_" + GetNodeTypeAttr(node, e.default_type);
    }
    return ConvertGenericNode(node, ps, info_id, warn, err);
  }

  // Unknown node — use generic conversion with warning
  std::string type_str = GetNodeTypeAttr(node, "float");
  std::string info_id = "ND_" + node_name + "_" + type_str;
  PUSH_WARN(fmt::format("Unknown node type '{}', using generic conversion with info:id='{}'.\n", node_name, info_id));
  return ConvertGenericNode(node, ps, info_id, warn, err);
}

// Iterative version of ConvertNodeGraph using explicit stack
static bool ConvertNodeGraphIterative(const lightusd::mtlx::pugi::xml_node &root_node,
                                      PrimSpec &ps_out,
                                      const MtlxConfig &config,
                                      std::string *warn, std::string *err) {
  constexpr size_t kMaxDepth = 1024 * 1024;

  // Stack entry for iterative processing
  // We need to collect children into a vector since the iterator is temporary
  struct StackEntry {
    lightusd::mtlx::pugi::xml_node xml_node;
    std::vector<lightusd::mtlx::pugi::xml_node> children;
    size_t child_idx;
    PrimSpec ps;
    bool is_skip;

    explicit StackEntry(const lightusd::mtlx::pugi::xml_node &n)
        : xml_node(n), child_idx(0), is_skip(false) {
      // Collect children into vector
      for (auto it = n.begin(); it != n.end(); ++it) {
        children.push_back(*it);
      }
    }
  };

  std::vector<StackEntry> stack;
  stack.reserve(64);

  // Initialize with root node
  stack.emplace_back(root_node);

  // Convert root node
  if (!ConvertSingleNode(root_node, stack.back().ps, stack.back().is_skip, config, warn, err)) {
    return false;
  }

  while (!stack.empty()) {
    if (stack.size() > kMaxDepth) {
      PUSH_ERROR_AND_RETURN("Network too deep.\n");
    }

    StackEntry &curr = stack.back();

    // Check if there are more children to process
    if (curr.child_idx < curr.children.size()) {
      // Get current child and advance index
      lightusd::mtlx::pugi::xml_node child = curr.children[curr.child_idx];
      curr.child_idx++;

      // Push child onto stack
      stack.emplace_back(child);

      // Convert the child node
      if (!ConvertSingleNode(child, stack.back().ps, stack.back().is_skip, config, warn, err)) {
        return false;
      }
    } else {
      // All children processed
      if (stack.size() > 1) {
        // Move completed node to parent's children if not skipped and has name
        PrimSpec completed = std::move(curr.ps);
        bool was_skip = curr.is_skip;
        stack.pop_back();

        if (!was_skip && !completed.name().empty()) {
          stack.back().ps.children().emplace_back(std::move(completed));
        }
      } else {
        // Root node - copy to output
        if (!curr.is_skip) {
          ps_out = std::move(curr.ps);
        }
        stack.pop_back();
      }
    }
  }

  return true;
}

// Process <include filename="..."/> elements by reading and merging included files.
// Replaces each <include .../> with the children of the included document's <materialx> root.
// @param[in] base_dir Base directory for resolving relative include paths.
// @param[in,out] xml_str The XML string to process. Modified in-place.
// @param[out] err Error message.
// @param[in] max_depth Maximum include recursion depth.
// @return true on success.
static bool ProcessIncludes(const std::string &base_dir, std::string &xml_str,
                            std::string *warn, std::string *err,
                            uint32_t max_depth = 8) {
  // Simple iterative approach: find and replace <include filename="..."/> tags
  // We iterate because included files may themselves contain includes.
  for (uint32_t depth = 0; depth < max_depth; depth++) {
    // Find <include
    size_t pos = xml_str.find("<include");
    if (pos == std::string::npos) {
      return true;  // No more includes
    }

    // Find the closing /> or >
    size_t end = xml_str.find("/>", pos);
    size_t end2 = xml_str.find(">", pos);
    if (end == std::string::npos && end2 == std::string::npos) {
      PUSH_ERROR_AND_RETURN("Unterminated <include> element");
    }

    size_t tag_end;
    size_t replace_end;
    if (end != std::string::npos && (end2 == std::string::npos || end <= end2)) {
      tag_end = end;
      replace_end = end + 2; // past "/>"
    } else {
      tag_end = end2;
      replace_end = end2 + 1; // past ">"
    }

    // Extract the tag content to find filename attribute
    std::string tag = xml_str.substr(pos, tag_end - pos);

    // Find filename="..." or filename='...'
    size_t fn_pos = tag.find("filename=");
    if (fn_pos == std::string::npos) {
      PUSH_ERROR_AND_RETURN("<include> element missing 'filename' attribute");
    }

    fn_pos += 9; // past "filename="
    if (fn_pos >= tag.size()) {
      PUSH_ERROR_AND_RETURN("<include> element has empty filename");
    }

    char quote = tag[fn_pos];
    if (quote != '"' && quote != '\'') {
      PUSH_ERROR_AND_RETURN("<include> filename must be quoted");
    }

    size_t fn_start = fn_pos + 1;
    size_t fn_end_q = tag.find(quote, fn_start);
    if (fn_end_q == std::string::npos) {
      PUSH_ERROR_AND_RETURN("<include> filename has unterminated quote");
    }

    std::string include_filename = tag.substr(fn_start, fn_end_q - fn_start);
    if (include_filename.empty()) {
      PUSH_ERROR_AND_RETURN("<include> filename is empty");
    }

    // Resolve include path relative to base directory.
    //
    // SECURITY: `include_filename` is attacker-controlled (it comes verbatim
    // from the <include filename="..."> attribute of an untrusted MaterialX
    // document). Validate it as a contained relative path before touching the
    // filesystem to prevent path traversal / arbitrary file read. This rejects
    // absolute paths ("/etc/passwd"), Windows drive letters ("C:\\..."), and
    // any ".." segment, and normalizes the remainder. Only paths contained
    // under `base_dir` are allowed, mirroring the validation that
    // composition.cc / composition-graph.cc apply to sublayers/references.
    std::string normalized_include;
    if (!security_policy::ValidateAndNormalizeAssetPath(include_filename,
                                                        &normalized_include)) {
      PUSH_ERROR_AND_RETURN(
          "<include> filename is not a safe relative path; absolute paths and "
          "'..' traversal are not allowed: " +
          include_filename);
    }

    std::string include_path = base_dir;
    if (!include_path.empty() && include_path.back() != '/' &&
        include_path.back() != '\\') {
      include_path += '/';
    }
    include_path += normalized_include;

    // Read the included file
    size_t inc_max_bytes = 1024 * 1024 * 16;
    std::vector<uint8_t> inc_data;
    if (!io::ReadWholeFile(&inc_data, err, include_path, inc_max_bytes, nullptr)) {
      PUSH_ERROR_AND_RETURN("Failed to read included file: " + include_path);
    }

    if (inc_data.empty()) {
      PushWarn("Included file is empty: " + include_path + "\n")
      // Just remove the <include> tag
      xml_str.erase(pos, replace_end - pos);
      continue;
    }

    std::string inc_str(reinterpret_cast<const char *>(inc_data.data()), inc_data.size());

    // Extract the content between <materialx ...> and </materialx> from the included file
    size_t mtlx_start = inc_str.find("<materialx");
    if (mtlx_start == std::string::npos) {
      PushWarn("Included file has no <materialx> root: " + include_path + "\n")
      xml_str.erase(pos, replace_end - pos);
      continue;
    }

    // Find the closing > of <materialx ...>
    size_t mtlx_open_end = inc_str.find(">", mtlx_start);
    if (mtlx_open_end == std::string::npos) {
      PUSH_ERROR_AND_RETURN("Unterminated <materialx> in included file: " + include_path);
    }

    // Check for self-closing <materialx/>
    if (inc_str[mtlx_open_end - 1] == '/') {
      // Empty document, just remove the include tag
      xml_str.erase(pos, replace_end - pos);
      continue;
    }

    size_t content_start = mtlx_open_end + 1;

    // Find </materialx>
    size_t mtlx_close = inc_str.find("</materialx>", content_start);
    if (mtlx_close == std::string::npos) {
      PUSH_ERROR_AND_RETURN("Missing </materialx> in included file: " + include_path);
    }

    // Extract the inner content
    std::string inner_content = inc_str.substr(content_start, mtlx_close - content_start);

    // Replace the <include .../> tag with the inner content
    xml_str.replace(pos, replace_end - pos, inner_content);
  }

  // If we exhausted max_depth, warn about possible circular includes
  if (xml_str.find("<include") != std::string::npos) {
    PushWarn("Maximum include depth reached. Possible circular includes.\n")
  }

  return true;
}

}  // namespace detail

static MtlxLookElement ParseLookElement(
    const lightusd::mtlx::pugi::xml_node &node) {
  MtlxLookElement element;
  element.name = node.name();
  for (const auto &attr : node.attributes()) {
    element.attributes[attr.first] = attr.second;
  }
  for (auto child : node) {
    element.children.push_back(ParseLookElement(child));
  }
  return element;
}

bool ReadMaterialXFromString(const std::string &str,
                             const std::string &asset_path, MtlxModel *mtlx,
                             std::string *warn, std::string *err,
                             const MtlxConfig &config) {
#define GET_ATTR_VALUE(__xml, __name, __ty, __var)                        \
  do {                                                                    \
    lightusd::mtlx::pugi::xml_attribute attr = __xml.attribute(__name);                   \
    if (!attr) {                                                          \
      PUSH_ERROR_AND_RETURN(                                              \
          fmt::format("Required XML Attribute `{}` not found.", __name)); \
    }                                                                     \
    __ty v;                                                               \
    if (!detail::ParseMaterialXValue(attr.as_string(), &v, err)) {        \
      return false;                                                       \
    }                                                                     \
    __var = v;                                                            \
  } while (0)

#define GET_SHADER_PARAM(__name, __typeName, __inp_name, __tyname, __ty, \
                         __valuestr, __attr)                             \
  if (__name == __inp_name) {                                            \
    if (__typeName != __tyname) {                                        \
      PUSH_ERROR_AND_RETURN(                                             \
          fmt::format("type `{}` expected for input `{}`, but got `{}`", \
                      __typeName, __inp_name, __tyname));                \
    }                                                                    \
    __ty v;                                                              \
    if (!detail::ParseMaterialXValue(__valuestr, &v, err)) {             \
      return false;                                                      \
    }                                                                    \
    __attr.set_value(v);                                                 \
  } else

  if (!mtlx) {
    PUSH_ERROR_AND_RETURN("mtlx output pointer is nullptr.");
  }

  if (str.empty()) {
    PUSH_ERROR_AND_RETURN("Input MaterialX string is empty.");
  }

  lightusd::mtlx::pugi::xml_document doc;
  lightusd::mtlx::pugi::xml_parse_result result = doc.load_string(str.c_str());
  if (!result) {
    std::string msg(result.description());
    PUSH_ERROR_AND_RETURN("Failed to parse XML: " + msg);
  }

  lightusd::mtlx::pugi::xml_node root = doc.child("materialx");
  if (!root) {
    PUSH_ERROR_AND_RETURN("<materialx> tag not found: " + asset_path);
  }

  // Attributes for a <materialx> element:
  //
  lightusd::mtlx::pugi::xml_attribute ver_attr = root.attribute("version");
  if (!ver_attr) {
    PUSH_ERROR_AND_RETURN("version attribute not found in <materialx>:" +
                          asset_path);
  }

  // parse version string as floating point
  {
    DCOUT("version = " << ver_attr.as_string());
    float ver{0.0};
    if (!detail::ParseMaterialXValue(ver_attr.as_string(), &ver, err)) {
      return false;
    }

    if (ver < 1.38f) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("LightUSD only supports MaterialX version 1.38 or "
                      "greater, but got {}",
                      ver_attr.as_string()));
    }
    mtlx->version = ver_attr.as_string();
  }

  lightusd::mtlx::pugi::xml_attribute cms_attr = root.attribute("cms");
  if (cms_attr) {
    mtlx->cms = cms_attr.as_string();
  }

  lightusd::mtlx::pugi::xml_attribute cmsconfig_attr = root.attribute("cmsconfig");
  if (cmsconfig_attr) {
    mtlx->cmsconfig = cmsconfig_attr.as_string();
  }
  lightusd::mtlx::pugi::xml_attribute colorspace_attr = root.attribute("colorspace");
  if (colorspace_attr) {
    mtlx->color_space = colorspace_attr.as_string();
  }

  lightusd::mtlx::pugi::xml_attribute namespace_attr = root.attribute("namespace");
  if (namespace_attr) {
    mtlx->name_space = namespace_attr.as_string();
  }

  std::map<std::string, PrimSpec> nodegraph_map;

  // NodeGraph
  for (auto ng : root.children("nodegraph")) {
    std::string ng_name;

    // Get nodegraph name
    lightusd::mtlx::pugi::xml_attribute name_attr = ng.attribute("name");
    if (!name_attr) {
      PUSH_WARN("NodeGraph without name attribute. Skipping.\n");
      continue;
    }
    ng_name = name_attr.as_string();

    PrimSpec ng_ps;
    ng_ps.name() = ng_name;
    ng_ps.specifier() = Specifier::Def;
    ng_ps.typeName() = kNodeGraph;

    // Process all child nodes
    for (auto child : ng) {
      std::string child_name = child.name();

      if (child_name == "output") {
        // Handle output declarations
        std::string output_name;
        std::string output_type;
        std::string nodename_ref;
        std::string output_ref = "out";

        lightusd::mtlx::pugi::xml_attribute out_name_attr = child.attribute("name");
        if (out_name_attr) {
          output_name = out_name_attr.as_string();
        }

        lightusd::mtlx::pugi::xml_attribute out_type_attr = child.attribute("type");
        if (out_type_attr) {
          output_type = out_type_attr.as_string();
        }
        lightusd::mtlx::pugi::xml_attribute output_attr = child.attribute("output");
        if (output_attr) {
          output_ref = output_attr.as_string();
        }

        lightusd::mtlx::pugi::xml_attribute nodename_attr = child.attribute("nodename");
        if (nodename_attr) {
          nodename_ref = nodename_attr.as_string();

          // Create connection to the referenced node
          std::string connection_path = nodename_ref + ".outputs:" + output_ref;

          // Store output as a real USD connection, not a string-valued
          // attribute. This preserves the edge when converting the PrimSpec
          // back to MaterialX or through a USD roundtrip.
          std::string prop_name = "outputs:" + output_name;
          Attribute output_attr;
          if (!output_type.empty()) output_attr.set_type_name(output_type);
          output_attr.set_connections({Path(connection_path, "")});
          ng_ps.props()[prop_name] = Property(output_attr);
        }
      } else if (child_name == "input") {
        // Nodegraph inputs use the same value/connection representation as
        // shader-node inputs. Keeping them in the PrimSpec lets the writer
        // emit exposed graph inputs instead of silently dropping them.
        if (!detail::ParseInputElement(child, ng_ps, err)) {
          PUSH_WARN(fmt::format("Failed to parse input on nodegraph '{}'.\n",
                                ng_name));
        }
      } else {
        // Process shader nodes
        PrimSpec child_ps;
        if (detail::ConvertNodeGraphIterative(child, child_ps, config, warn, err)) {
          if (!child_ps.name().empty()) {
            ng_ps.children().emplace_back(std::move(child_ps));
          }
        }
      }
    }

    nodegraph_map[ng_name] = std::move(ng_ps);
  }

  // Store nodegraphs in the model
  mtlx->nodegraphs = std::move(nodegraph_map);

  // standard_surface (Autodesk StandardSurface)
  for (auto sd_surface : root.children("standard_surface")) {
    std::string surface_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(sd_surface, "name", std::string, surface_name);
      GET_ATTR_VALUE(sd_surface, "type", std::string, typeName);

      if (typeName != "surfaceshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`surfaceshader` expected for type of "
                        "standard_surface, but got `{}`",
                        typeName));
      }
    }

    MtlxAutodeskStandardSurface surface;
    for (auto inp : sd_surface.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      std::string nodegraphRef;
      std::string outputRef;
      std::string nodenameRef;

      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);

      // Check for value attribute (direct value)
      lightusd::mtlx::pugi::xml_attribute value_attr = inp.attribute("value");
      if (value_attr) {
        valueStr = value_attr.as_string();
      }

      // Check for connection attributes
      lightusd::mtlx::pugi::xml_attribute nodegraph_attr = inp.attribute("nodegraph");
      if (nodegraph_attr) {
        nodegraphRef = nodegraph_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute output_attr = inp.attribute("output");
      if (output_attr) {
        outputRef = output_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute nodename_attr = inp.attribute("nodename");
      if (nodename_attr) {
        nodenameRef = nodename_attr.as_string();
      }

      // Handle connections vs values
      bool is_connection = !nodegraphRef.empty() || !nodenameRef.empty();

      if (is_connection) {
        // Store connection information
        MtlxShaderConnection conn;
        conn.input_name = name;
        conn.type = typeName;
        conn.nodegraph = nodegraphRef;
        conn.output = outputRef;
        conn.nodename = nodenameRef;
        mtlx->shader_connections[surface_name].push_back(conn);
        continue;  // Skip value parsing for connections
      }

      // Parse standard_surface direct values
      GET_SHADER_PARAM(name, typeName, "base", "float", float, valueStr, surface.base)
      GET_SHADER_PARAM(name, typeName, "base_color", "color3", value::color3f, valueStr, surface.base_color)
      GET_SHADER_PARAM(name, typeName, "diffuse_roughness", "float", float, valueStr, surface.diffuse_roughness)
      GET_SHADER_PARAM(name, typeName, "metalness", "float", float, valueStr, surface.metalness)
      GET_SHADER_PARAM(name, typeName, "specular", "float", float, valueStr, surface.specular)
      GET_SHADER_PARAM(name, typeName, "specular_color", "color3", value::color3f, valueStr, surface.specular_color)
      GET_SHADER_PARAM(name, typeName, "specular_roughness", "float", float, valueStr, surface.specular_roughness)
      GET_SHADER_PARAM(name, typeName, "specular_IOR", "float", float, valueStr, surface.specular_IOR)
      GET_SHADER_PARAM(name, typeName, "specular_anisotropy", "float", float, valueStr, surface.specular_anisotropy)
      GET_SHADER_PARAM(name, typeName, "specular_rotation", "float", float, valueStr, surface.specular_rotation)
      GET_SHADER_PARAM(name, typeName, "transmission", "float", float, valueStr, surface.transmission)
      GET_SHADER_PARAM(name, typeName, "transmission_color", "color3", value::color3f, valueStr, surface.transmission_color)
      GET_SHADER_PARAM(name, typeName, "transmission_depth", "float", float, valueStr, surface.transmission_depth)
      GET_SHADER_PARAM(name, typeName, "transmission_scatter", "color3", value::color3f, valueStr, surface.transmission_scatter)
      GET_SHADER_PARAM(name, typeName, "transmission_scatter_anisotropy", "float", float, valueStr, surface.transmission_scatter_anisotropy)
      GET_SHADER_PARAM(name, typeName, "transmission_dispersion", "float", float, valueStr, surface.transmission_dispersion)
      GET_SHADER_PARAM(name, typeName, "transmission_extra_roughness", "float", float, valueStr, surface.transmission_extra_roughness)
      GET_SHADER_PARAM(name, typeName, "subsurface", "float", float, valueStr, surface.subsurface)
      GET_SHADER_PARAM(name, typeName, "subsurface_color", "color3", value::color3f, valueStr, surface.subsurface_color)
      GET_SHADER_PARAM(name, typeName, "subsurface_radius", "color3", value::color3f, valueStr, surface.subsurface_radius)
      GET_SHADER_PARAM(name, typeName, "subsurface_scale", "float", float, valueStr, surface.subsurface_scale)
      GET_SHADER_PARAM(name, typeName, "subsurface_anisotropy", "float", float, valueStr, surface.subsurface_anisotropy)
      GET_SHADER_PARAM(name, typeName, "sheen", "float", float, valueStr, surface.sheen)
      GET_SHADER_PARAM(name, typeName, "sheen_color", "color3", value::color3f, valueStr, surface.sheen_color)
      GET_SHADER_PARAM(name, typeName, "sheen_roughness", "float", float, valueStr, surface.sheen_roughness)
      GET_SHADER_PARAM(name, typeName, "coat", "float", float, valueStr, surface.coat)
      GET_SHADER_PARAM(name, typeName, "coat_color", "color3", value::color3f, valueStr, surface.coat_color)
      GET_SHADER_PARAM(name, typeName, "coat_roughness", "float", float, valueStr, surface.coat_roughness)
      GET_SHADER_PARAM(name, typeName, "coat_anisotropy", "float", float, valueStr, surface.coat_anisotropy)
      GET_SHADER_PARAM(name, typeName, "coat_rotation", "float", float, valueStr, surface.coat_rotation)
      GET_SHADER_PARAM(name, typeName, "coat_IOR", "float", float, valueStr, surface.coat_IOR)
      GET_SHADER_PARAM(name, typeName, "coat_affect_color", "float", float, valueStr, surface.coat_affect_color)
      GET_SHADER_PARAM(name, typeName, "coat_affect_roughness", "float", float, valueStr, surface.coat_affect_roughness)
      GET_SHADER_PARAM(name, typeName, "thin_film_thickness", "float", float, valueStr, surface.thin_film_thickness)
      GET_SHADER_PARAM(name, typeName, "thin_film_IOR", "float", float, valueStr, surface.thin_film_IOR)
      GET_SHADER_PARAM(name, typeName, "emission", "float", float, valueStr, surface.emission)
      GET_SHADER_PARAM(name, typeName, "emission_color", "color3", value::color3f, valueStr, surface.emission_color)
      GET_SHADER_PARAM(name, typeName, "opacity", "color3", value::color3f, valueStr, surface.opacity)
      GET_SHADER_PARAM(name, typeName, "thin_walled", "boolean", bool, valueStr, surface.thin_walled)
      GET_SHADER_PARAM(name, typeName, "normal", "vector3", value::normal3f, valueStr, surface.normal)
      GET_SHADER_PARAM(name, typeName, "tangent", "vector3", value::vector3f, valueStr, surface.tangent)
      GET_SHADER_PARAM(name, typeName, "coat_normal", "vector3", value::normal3f, valueStr, surface.coat_normal)
      GET_SHADER_PARAM(name, typeName, "displacement", "float", float, valueStr, surface.displacement)
      {
        if (!valueStr.empty()) {
          mtlx->custom_shader_inputs[surface_name].push_back(
              {name, typeName, valueStr});
        } else {
          PUSH_WARN(fmt::format("Unknown/unsupported standard_surface input `{}`", name));
        }
      }
    }

    mtlx->shaders[surface_name] = surface;
    if (mtlx->shader_name.empty()) {
      mtlx->shader_name = kMtlxAutodeskStandardSurface;
      mtlx->shader = surface;  // Set the primary shader value
    }
  }

  // uniform_edf
  for (auto uniform_edf : root.children("uniform_edf")) {
    std::string node_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(uniform_edf, "name", std::string, node_name);
      GET_ATTR_VALUE(uniform_edf, "type", std::string, typeName);

      if (typeName != "EDF") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`EDF` expected for type of uniform_edf, but got `{}`",
                        typeName));
      }
    }

    MtlxUniformEdf edf;
    for (auto inp : uniform_edf.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);
      GET_ATTR_VALUE(inp, "value", std::string, valueStr);

      GET_SHADER_PARAM(name, typeName, "color", "color3", value::color3f,
                       valueStr, edf.color) {
        PUSH_WARN("Unknown/unsupported input " << name);
      }
    }

    mtlx->light_shaders[node_name] = edf;
  }

  // conical_edf
  for (auto conical_edf : root.children("conical_edf")) {
    std::string node_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(conical_edf, "name", std::string, node_name);
      GET_ATTR_VALUE(conical_edf, "type", std::string, typeName);

      if (typeName != "EDF") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`EDF` expected for type of conical_edf, but got `{}`",
                        typeName));
      }
    }

    MtlxConicalEdf edf;
    for (auto inp : conical_edf.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);
      GET_ATTR_VALUE(inp, "value", std::string, valueStr);

      GET_SHADER_PARAM(name, typeName, "color", "color3", value::color3f,
                       valueStr, edf.color)
      GET_SHADER_PARAM(name, typeName, "normal", "vector3", value::normal3f,
                       valueStr, edf.normal)
      GET_SHADER_PARAM(name, typeName, "inner_angle", "float", float, valueStr,
                       edf.inner_angle)
      GET_SHADER_PARAM(name, typeName, "outer_angle", "float", float, valueStr,
                       edf.outer_angle) {
        PUSH_WARN("Unknown/unsupported input " << name);
      }
    }

    mtlx->light_shaders[node_name] = edf;
  }

  // measured_edf
  for (auto measured_edf : root.children("measured_edf")) {
    std::string node_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(measured_edf, "name", std::string, node_name);
      GET_ATTR_VALUE(measured_edf, "type", std::string, typeName);

      if (typeName != "EDF") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`EDF` expected for type of measured_edf, but got `{}`",
                        typeName));
      }
    }

    MtlxMeasuredEdf edf;
    for (auto inp : measured_edf.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);
      GET_ATTR_VALUE(inp, "value", std::string, valueStr);

      GET_SHADER_PARAM(name, typeName, "color", "color3", value::color3f,
                       valueStr, edf.color)
      // file is a filename type
      if (name == "file") {
        if (typeName != "filename") {
          PUSH_ERROR_AND_RETURN(
              fmt::format("type `{}` expected for input `{}`, but got `{}`",
                          "filename", "file", typeName));
        }
        std::string filepath;
        if (!detail::ParseMaterialXValue(valueStr, &filepath, err)) {
          return false;
        }
        edf.file.set_value(value::AssetPath(filepath));
      } else {
        PUSH_WARN("Unknown/unsupported input " << name);
      }
    }

    mtlx->light_shaders[node_name] = edf;
  }

  // light
  for (auto light : root.children("light")) {
    std::string node_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(light, "name", std::string, node_name);
      GET_ATTR_VALUE(light, "type", std::string, typeName);

      if (typeName != "lightshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`lightshader` expected for type of light, but got `{}`",
                        typeName));
      }
    }

    MtlxLight light_shader;
    for (auto inp : light.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      mtlx::pugi::xml_attribute nodename_attr = inp.attribute("nodename");

      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);

      // Handle connections via nodename
      if (nodename_attr) {
        std::string nodename = nodename_attr.as_string();
        if (name == "edf") {
          light_shader.edf.set_value(value::token(nodename));
        } else {
          PUSH_WARN("Unknown/unsupported connection input " << name);
        }
      } else {
        // Handle direct values
        GET_ATTR_VALUE(inp, "value", std::string, valueStr);

        GET_SHADER_PARAM(name, typeName, "intensity", "color3", value::color3f,
                         valueStr, light_shader.intensity)
        GET_SHADER_PARAM(name, typeName, "exposure", "float", float, valueStr,
                         light_shader.exposure) {
          PUSH_WARN("Unknown/unsupported input " << name);
        }
      }
    }

    mtlx->light_shaders[node_name] = light_shader;
  }

  // standard_surface
  for (auto usd_surface : root.children("UsdPreviewSurface")) {
    std::string surface_name;
    {
      std::string typeName;

      GET_ATTR_VALUE(usd_surface, "name", std::string, surface_name);
      GET_ATTR_VALUE(usd_surface, "type", std::string, typeName);

      if (typeName != "surfaceshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`surfaceshader` expected for type of "
                        "UsdPreviewSurface, but got `{}`",
                        typeName));
      }
    }

    MtlxUsdPreviewSurface surface;
    for (auto inp : usd_surface.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      std::string nodegraphRef;
      std::string outputRef;
      std::string nodenameRef;

      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);

      // Check for value attribute (direct value)
      lightusd::mtlx::pugi::xml_attribute value_attr = inp.attribute("value");
      if (value_attr) {
        valueStr = value_attr.as_string();
      }

      // Check for connection attributes
      lightusd::mtlx::pugi::xml_attribute nodegraph_attr = inp.attribute("nodegraph");
      if (nodegraph_attr) {
        nodegraphRef = nodegraph_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute output_attr = inp.attribute("output");
      if (output_attr) {
        outputRef = output_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute nodename_attr = inp.attribute("nodename");
      if (nodename_attr) {
        nodenameRef = nodename_attr.as_string();
      }

      // Handle connections vs values
      bool is_connection = !nodegraphRef.empty() || !nodenameRef.empty();

      if (is_connection) {
        // This is a connection, not a direct value
        // Store connection information
        MtlxShaderConnection conn;
        conn.input_name = name;
        conn.type = typeName;
        conn.nodegraph = nodegraphRef;
        conn.output = outputRef;
        conn.nodename = nodenameRef;
        mtlx->shader_connections[surface_name].push_back(conn);
        continue;  // Skip value parsing for connections
      }

      // Parse direct values
      GET_SHADER_PARAM(name, typeName, "diffuseColor", "color3", value::color3f,
                       valueStr, surface.diffuseColor)
      GET_SHADER_PARAM(name, typeName, "emissiveColor", "color3",
                       value::color3f, valueStr, surface.emissiveColor)
      GET_SHADER_PARAM(name, typeName, "useSpecularWorkflow", "integer", int,
                       valueStr, surface.useSpecularWorkflow)
      GET_SHADER_PARAM(name, typeName, "specularColor", "color3",
                       value::color3f, valueStr, surface.specularColor)
      GET_SHADER_PARAM(name, typeName, "metallic", "float", float, valueStr,
                       surface.metallic)
      GET_SHADER_PARAM(name, typeName, "roughness", "float", float, valueStr,
                       surface.roughness)
      GET_SHADER_PARAM(name, typeName, "clearcoat", "float", float, valueStr,
                       surface.clearcoat)
      GET_SHADER_PARAM(name, typeName, "clearcoatRoughness", "float", float,
                       valueStr, surface.clearcoatRoughness)
      GET_SHADER_PARAM(name, typeName, "opacity", "float", float, valueStr,
                       surface.opacity)
      GET_SHADER_PARAM(name, typeName, "opacityThreshold", "float", float,
                       valueStr, surface.opacityThreshold)
      GET_SHADER_PARAM(name, typeName, "ior", "float", float, valueStr,
                       surface.ior)
      GET_SHADER_PARAM(name, typeName, "normal", "vector3f", value::normal3f,
                       valueStr, surface.normal)
      GET_SHADER_PARAM(name, typeName, "displacement", "float", float, valueStr,
                       surface.displacement)
      GET_SHADER_PARAM(name, typeName, "occlusion", "float", float, valueStr,
                       surface.occlusion) {
        PUSH_WARN("Unknown/unsupported input " << name);
      }
    }

    mtlx->shaders[surface_name] = surface;
    if (mtlx->shader_name.empty()) {
      mtlx->shader_name = kUsdPreviewSurface;
      mtlx->shader = surface;  // Set the primary shader value
    }
  }

  // OpenPBR Surface - check both "OpenPBRSurface" and "open_pbr_surface"
  std::vector<lightusd::mtlx::pugi::xml_node> openpbr_nodes;
  {
    auto nodes1 = root.children("OpenPBRSurface");
    auto nodes2 = root.children("open_pbr_surface");
    openpbr_nodes.insert(openpbr_nodes.end(), nodes1.begin(), nodes1.end());
    openpbr_nodes.insert(openpbr_nodes.end(), nodes2.begin(), nodes2.end());
  }

  for (auto openpbr_surface : openpbr_nodes) {
    std::string surface_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(openpbr_surface, "name", std::string, surface_name);
      GET_ATTR_VALUE(openpbr_surface, "type", std::string, typeName);
      if (typeName != "surfaceshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("`surfaceshader` expected for type of "
                        "OpenPBRSurface, but got `{}`",
                        typeName));
      }
    }

    MtlxOpenPBRSurface surface;
    for (auto inp : openpbr_surface.children("input")) {
      std::string name;
      std::string typeName;
      std::string valueStr;
      std::string nodegraphRef;
      std::string outputRef;
      std::string nodenameRef;

      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);

      // Check for value attribute (direct value)
      lightusd::mtlx::pugi::xml_attribute value_attr = inp.attribute("value");
      if (value_attr) {
        valueStr = value_attr.as_string();
      }

      // Check for connection attributes
      lightusd::mtlx::pugi::xml_attribute nodegraph_attr = inp.attribute("nodegraph");
      if (nodegraph_attr) {
        nodegraphRef = nodegraph_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute output_attr = inp.attribute("output");
      if (output_attr) {
        outputRef = output_attr.as_string();
      }

      lightusd::mtlx::pugi::xml_attribute nodename_attr = inp.attribute("nodename");
      if (nodename_attr) {
        nodenameRef = nodename_attr.as_string();
      }

      // Handle connections vs values
      bool is_connection = !nodegraphRef.empty() || !nodenameRef.empty();

      if (is_connection) {
        // Store connection information
        MtlxShaderConnection conn;
        conn.input_name = name;
        conn.type = typeName;
        conn.nodegraph = nodegraphRef;
        conn.output = outputRef;
        conn.nodename = nodenameRef;
        mtlx->shader_connections[surface_name].push_back(conn);
        continue;  // Skip value parsing for connections
      }

      // Parse OpenPBR direct values
      GET_SHADER_PARAM(name, typeName, "base_weight", "float", float, valueStr, surface.base_weight)
      GET_SHADER_PARAM(name, typeName, "base_color", "color3", value::color3f, valueStr, surface.base_color)
      GET_SHADER_PARAM(name, typeName, "base_roughness", "float", float, valueStr, surface.base_roughness)
      GET_SHADER_PARAM(name, typeName, "base_metalness", "float", float, valueStr, surface.base_metalness)
      GET_SHADER_PARAM(name, typeName, "base_diffuse_roughness", "float", float, valueStr, surface.base_diffuse_roughness)
      GET_SHADER_PARAM(name, typeName, "specular_weight", "float", float, valueStr, surface.specular_weight)
      GET_SHADER_PARAM(name, typeName, "specular_color", "color3", value::color3f, valueStr, surface.specular_color)
      GET_SHADER_PARAM(name, typeName, "specular_roughness", "float", float, valueStr, surface.specular_roughness)
      GET_SHADER_PARAM(name, typeName, "specular_ior", "float", float, valueStr, surface.specular_ior)
      GET_SHADER_PARAM(name, typeName, "specular_ior_level", "float", float, valueStr, surface.specular_ior_level)
      GET_SHADER_PARAM(name, typeName, "specular_anisotropy", "float", float, valueStr, surface.specular_anisotropy)
      GET_SHADER_PARAM(name, typeName, "specular_rotation", "float", float, valueStr, surface.specular_rotation)
      GET_SHADER_PARAM(name, typeName, "specular_roughness_anisotropy", "float", float, valueStr, surface.specular_roughness_anisotropy)
      GET_SHADER_PARAM(name, typeName, "transmission_weight", "float", float, valueStr, surface.transmission_weight)
      GET_SHADER_PARAM(name, typeName, "transmission_color", "color3", value::color3f, valueStr, surface.transmission_color)
      GET_SHADER_PARAM(name, typeName, "transmission_depth", "float", float, valueStr, surface.transmission_depth)
      GET_SHADER_PARAM(name, typeName, "transmission_scatter", "color3", value::color3f, valueStr, surface.transmission_scatter)
      GET_SHADER_PARAM(name, typeName, "transmission_scatter_anisotropy", "float", float, valueStr, surface.transmission_scatter_anisotropy)
      GET_SHADER_PARAM(name, typeName, "transmission_dispersion", "float", float, valueStr, surface.transmission_dispersion)
      GET_SHADER_PARAM(name, typeName, "transmission_dispersion_abbe_number", "float", float, valueStr, surface.transmission_dispersion_abbe_number)
      GET_SHADER_PARAM(name, typeName, "transmission_dispersion_scale", "float", float, valueStr, surface.transmission_dispersion_scale)
      GET_SHADER_PARAM(name, typeName, "subsurface_weight", "float", float, valueStr, surface.subsurface_weight)
      GET_SHADER_PARAM(name, typeName, "subsurface_color", "color3", value::color3f, valueStr, surface.subsurface_color)
      GET_SHADER_PARAM(name, typeName, "subsurface_radius", "float", float, valueStr, surface.subsurface_radius)
      GET_SHADER_PARAM(name, typeName, "subsurface_radius_scale", "color3", value::color3f, valueStr, surface.subsurface_radius_scale)
      GET_SHADER_PARAM(name, typeName, "subsurface_scale", "float", float, valueStr, surface.subsurface_scale)
      GET_SHADER_PARAM(name, typeName, "subsurface_anisotropy", "float", float, valueStr, surface.subsurface_anisotropy)
      GET_SHADER_PARAM(name, typeName, "subsurface_scatter_anisotropy", "float", float, valueStr, surface.subsurface_scatter_anisotropy)
      GET_SHADER_PARAM(name, typeName, "sheen_weight", "float", float, valueStr, surface.sheen_weight)
      GET_SHADER_PARAM(name, typeName, "sheen_color", "color3", value::color3f, valueStr, surface.sheen_color)
      GET_SHADER_PARAM(name, typeName, "sheen_roughness", "float", float, valueStr, surface.sheen_roughness)
      GET_SHADER_PARAM(name, typeName, "coat_weight", "float", float, valueStr, surface.coat_weight)
      GET_SHADER_PARAM(name, typeName, "coat_color", "color3", value::color3f, valueStr, surface.coat_color)
      GET_SHADER_PARAM(name, typeName, "coat_roughness", "float", float, valueStr, surface.coat_roughness)
      GET_SHADER_PARAM(name, typeName, "coat_anisotropy", "float", float, valueStr, surface.coat_anisotropy)
      GET_SHADER_PARAM(name, typeName, "coat_rotation", "float", float, valueStr, surface.coat_rotation)
      GET_SHADER_PARAM(name, typeName, "coat_roughness_anisotropy", "float", float, valueStr, surface.coat_roughness_anisotropy)
      GET_SHADER_PARAM(name, typeName, "coat_ior", "float", float, valueStr, surface.coat_ior)
      GET_SHADER_PARAM(name, typeName, "coat_darkening", "float", float, valueStr, surface.coat_darkening)
      GET_SHADER_PARAM(name, typeName, "coat_affect_color", "float", float, valueStr, surface.coat_affect_color)
      GET_SHADER_PARAM(name, typeName, "coat_affect_roughness", "float", float, valueStr, surface.coat_affect_roughness)
      GET_SHADER_PARAM(name, typeName, "fuzz_weight", "float", float, valueStr, surface.fuzz_weight)
      GET_SHADER_PARAM(name, typeName, "fuzz_color", "color3", value::color3f, valueStr, surface.fuzz_color)
      GET_SHADER_PARAM(name, typeName, "fuzz_roughness", "float", float, valueStr, surface.fuzz_roughness)
      GET_SHADER_PARAM(name, typeName, "thin_film_thickness", "float", float, valueStr, surface.thin_film_thickness)
      GET_SHADER_PARAM(name, typeName, "thin_film_ior", "float", float, valueStr, surface.thin_film_ior)
      GET_SHADER_PARAM(name, typeName, "thin_film_weight", "float", float, valueStr, surface.thin_film_weight)
      GET_SHADER_PARAM(name, typeName, "emission_luminance", "float", float, valueStr, surface.emission_luminance)
      GET_SHADER_PARAM(name, typeName, "emission_color", "color3", value::color3f, valueStr, surface.emission_color)
      GET_SHADER_PARAM(name, typeName, "opacity", "float", float, valueStr, surface.geometry_opacity)
      GET_SHADER_PARAM(name, typeName, "geometry_opacity", "float", float, valueStr, surface.geometry_opacity)
      GET_SHADER_PARAM(name, typeName, "geometry_thin_walled", "boolean", bool, valueStr, surface.geometry_thin_walled)
      GET_SHADER_PARAM(name, typeName, "normal", "vector3", value::normal3f, valueStr, surface.geometry_normal)
      GET_SHADER_PARAM(name, typeName, "tangent", "vector3", value::vector3f, valueStr, surface.geometry_tangent)
      GET_SHADER_PARAM(name, typeName, "coat_normal", "vector3", value::normal3f, valueStr, surface.geometry_coat_normal)
      GET_SHADER_PARAM(name, typeName, "coat_tangent", "vector3", value::vector3f, valueStr, surface.geometry_coat_tangent)
      {
        if (!valueStr.empty()) {
          mtlx->custom_shader_inputs[surface_name].push_back(
              {name, typeName, valueStr});
        } else {
          PUSH_WARN(fmt::format("Unknown/unsupported OpenPBR input `{}`", name));
        }
      }
    }

    mtlx->shaders[surface_name] = surface;
    if (mtlx->shader_name.empty()) {
      mtlx->shader_name = kOpenPBRSurface;
      mtlx->shader = surface;  // Set the primary shader value
    }
  }

  // surfacematerial
  for (auto surfacematerial : root.children("surfacematerial")) {
    std::string material_name;
    {
      std::string typeName;
      GET_ATTR_VALUE(surfacematerial, "name", std::string, material_name);
      GET_ATTR_VALUE(surfacematerial, "type", std::string, typeName);

      if (typeName != "material") {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "`material` expected for type of surfacematerial, but got `{}`",
            typeName));
      }
    }

    std::string typeName;
    std::string nodename;
    for (auto inp : surfacematerial.children("input")) {
      std::string name;
      GET_ATTR_VALUE(inp, "name", std::string, name);
      GET_ATTR_VALUE(inp, "type", std::string, typeName);
      GET_ATTR_VALUE(inp, "nodename", std::string, nodename);

      if (name != "surfaceshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Currently only `surfaceshader` supported for "
                        "`surfacematerial`'s input, but got `{}`",
                        name));
      }

      if (typeName != "surfaceshader") {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Currently only `surfaceshader` supported for "
                        "`surfacematerial` input type, but got `{}`",
                        typeName));
      }
    }

    MtlxMaterial mat;
    mat.name = material_name;
    // `typeName` above is the connected input type (`surfaceshader`), not
    // the MaterialX element type. A surfacematerial element is always a
    // `material`; retaining the input type makes our writer emit invalid XML.
    mat.typeName = "material";
    mat.nodename = nodename;
    mtlx->surface_materials[material_name] = mat;
  }

  // Preserve look assignments generically. MaterialX and renderer extensions
  // add assignment attributes over time, so retaining the element/attribute
  // pairs is more useful than rejecting or silently dropping the look.
  for (auto look : root.children("look")) {
    MtlxLook parsed_look;
    lightusd::mtlx::pugi::xml_attribute name_attr = look.attribute("name");
    if (name_attr) parsed_look.name = name_attr.as_string();
    for (auto child : look) {
      parsed_look.elements.push_back(ParseLookElement(child));
    }
    if (!parsed_look.name.empty()) {
      mtlx->looks[parsed_look.name] = std::move(parsed_look);
    }
  }

#undef GET_ATTR_VALUE

  return true;
}

bool ReadMaterialXFromFile(const AssetResolutionResolver &resolver,
                           const std::string &asset_path, MtlxModel *mtlx,
                           std::string *warn, std::string *err,
                           const MtlxConfig &config) {
  if (!mtlx) {
    PUSH_ERROR_AND_RETURN("mtlx output pointer is nullptr.");
  }

  std::string filepath = resolver.resolve(asset_path);
  if (filepath.empty()) {
    PUSH_ERROR_AND_RETURN("Asset not found: " + asset_path);
  }

  // up to 16MB xml
  size_t max_bytes = 1024 * 1024 * 16;

  std::vector<uint8_t> data;
  if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    PUSH_ERROR_AND_RETURN("Read file failed: " + filepath);
  }

  if (data.empty()) {
    PUSH_ERROR_AND_RETURN("File is empty: " + filepath);
  }

  std::string str(reinterpret_cast<const char *>(data.data()), data.size());

  // Process <include filename="..."/> elements
  // Compute base directory from the resolved file path
  std::string base_dir;
  {
    size_t last_sep = filepath.find_last_of("/\\");
    if (last_sep != std::string::npos) {
      base_dir = filepath.substr(0, last_sep);
    } else {
      base_dir = ".";
    }
  }

  if (!detail::ProcessIncludes(base_dir, str, warn, err)) {
    return false;
  }

  return ReadMaterialXFromString(str, asset_path, mtlx, warn, err, config);
}


bool ToPrimSpec(const MtlxModel &model, PrimSpec &ps, std::string *err) {
  //
  // def "MaterialX" {
  //
  //   def "Materials" {
  //     def Material ... {
  //     }
  //   }
  //   def "Shaders" {
  //   }
  // Validate the model's surface shader is one we recognize. (This top-level
  // info:id is informational; the root PrimSpec is rebuilt below.) Compare
  // against the SAME constants the reader assigns (see shader_name = … above);
  // the standard_surface one is kMtlxAutodeskStandardSurface, not the bare
  // "AutodeskStandardSurface" string.
  if (model.shader_name == kUsdPreviewSurface) {
    ps.props()["info:id"] =
        detail::MakeProperty(value::token(kUsdPreviewSurface));
  } else if (model.shader_name == kMtlxAutodeskStandardSurface) {
    ps.props()["info:id"] =
        detail::MakeProperty(value::token(kMtlxAutodeskStandardSurface));
  } else if (model.shader_name == kOpenPBRSurface) {
    ps.props()["info:id"] =
        detail::MakeProperty(value::token(kOpenPBRSurface));
  } else {
    PUSH_ERROR_AND_RETURN("Unsupported shader_name: " << model.shader_name);
  }

  PrimSpec materials;
  materials.name() = "Materials";
  materials.specifier() = Specifier::Def;

  for (const auto &item : model.surface_materials) {
    PrimSpec material;
    material.specifier() = Specifier::Def;
    material.typeName() = "Material";
    material.name() = item.second.name;

    // Add MaterialXConfigAPI with version from MaterialX
    if (!model.version.empty()) {
      material.props()["config:mtlx:version"] =
          detail::MakeProperty(model.version);
    }

    materials.children().push_back(std::move(material));
  }

  PrimSpec shaders;
  shaders.name() = "Shaders";
  shaders.specifier() = Specifier::Def;

  // Serialize shader values as child PrimSpecs
  for (const auto &shader_item : model.shaders) {
    PrimSpec shader_ps;
    shader_ps.name() = shader_item.first;
    shader_ps.specifier() = Specifier::Def;
    shader_ps.typeName() = kShader;

    // Set info:id based on shader type
    if (shader_item.second.as<MtlxOpenPBRSurface>()) {
      shader_ps.props()[kShaderInfoId] =
          detail::MakeProperty(value::token(kMtlxOpenPBRSurface));
    } else if (shader_item.second.as<MtlxAutodeskStandardSurface>()) {
      shader_ps.props()[kShaderInfoId] =
          detail::MakeProperty(value::token("AutodeskStandardSurface"));
    } else if (shader_item.second.as<MtlxUsdPreviewSurface>()) {
      shader_ps.props()[kShaderInfoId] =
          detail::MakeProperty(value::token(kUsdPreviewSurface));
    }

    shaders.children().push_back(std::move(shader_ps));
  }

  // Add NodeGraphs container
  PrimSpec nodegraphs;
  nodegraphs.name() = "NodeGraphs";
  nodegraphs.specifier() = Specifier::Def;

  // Add all nodegraphs
  for (const auto &ng_item : model.nodegraphs) {
    PrimSpec ng_copy = ng_item.second; // Copy the nodegraph PrimSpec
    nodegraphs.children().push_back(std::move(ng_copy));
  }

  PrimSpec root;
  root.name() = "MaterialX";
  root.specifier() = Specifier::Def;

  root.children().push_back(materials);
  root.children().push_back(shaders);
  if (!model.nodegraphs.empty()) {
    root.children().push_back(nodegraphs);
  }

  ps = std::move(root);

  return true;
}

bool LoadMaterialXFromAsset(const Asset &asset, const std::string &asset_path,
                            PrimSpec &ps /* inout */, std::string *warn,
                            std::string *err) {
  (void)asset_path;
  (void)warn;

  if (!asset.data() || asset.size() < 32) {
    if (err) {
      (*err) += "MaterialX: Asset data is null or too small.\n";
    }
    return false;
  }

  std::string str(reinterpret_cast<const char *>(asset.data()), asset.size());

  MtlxModel mtlx;
  if (!ReadMaterialXFromString(str, asset_path, &mtlx, warn, err)) {
    PUSH_ERROR_AND_RETURN("Failed to read MaterialX.");
  }

  if (!ToPrimSpec(mtlx, ps, err)) {
    PUSH_ERROR_AND_RETURN("Failed to convert MaterialX to USD PrimSpec.");
  }

  return true;
}

///
/// Convert MaterialX Light shader to UsdLux light
///
bool ConvertMtlxLightToUsdLux(const MtlxLight &mtlx_light,
                               const std::map<std::string, value::Value> &light_shaders,
                               value::Value *usd_light,
                               std::string *warn, std::string *err) {
  (void)warn;

  if (!usd_light) {
    PUSH_ERROR_AND_RETURN("usd_light is nullptr");
  }

  // Get the EDF node name from the light shader
  value::token edf_name;
  if (!mtlx_light.edf.get_value(&edf_name)) {
    PUSH_ERROR_AND_RETURN("Light shader has no EDF connection");
  }

  // Find the EDF node in light_shaders
  auto edf_it = light_shaders.find(edf_name.str());
  if (edf_it == light_shaders.end()) {
    PUSH_ERROR_AND_RETURN(fmt::format("EDF node '{}' not found", edf_name.str()));
  }

  const value::Value &edf_value = edf_it->second;

  // Get intensity and exposure from the light shader
  value::color3f intensity{1.0f, 1.0f, 1.0f};
  mtlx_light.intensity.get_value().get_scalar(&intensity);

  float exposure = 0.0f;
  if (mtlx_light.exposure.authored()) {
    if (auto exp_val = mtlx_light.exposure.get_value()) {
      exp_val.value().get_scalar(&exposure);
    }
  }

  // Convert based on EDF type
  if (auto uniform_edf = edf_value.as<MtlxUniformEdf>()) {
    // uniform_edf -> SphereLight (omnidirectional point light)
    SphereLight light;

    value::color3f edf_color{1.0f, 1.0f, 1.0f};
    uniform_edf->color.get_value().get_scalar(&edf_color);

    // Combine EDF color with light intensity
    value::color3f final_color{
      edf_color[0] * intensity[0],
      edf_color[1] * intensity[1],
      edf_color[2] * intensity[2]
    };

    light.color.set_value(final_color);
    light.exposure.set_value(exposure);
    light.intensity.set_value(1.0f); // Already baked into color

    (*usd_light) = light;
    return true;

  } else if (auto conical_edf = edf_value.as<MtlxConicalEdf>()) {
    // conical_edf -> RectLight with ShapingAPI (spot light effect)
    RectLight light;

    value::color3f edf_color{1.0f, 1.0f, 1.0f};
    conical_edf->color.get_value().get_scalar(&edf_color);

    // Combine EDF color with light intensity
    value::color3f final_color{
      edf_color[0] * intensity[0],
      edf_color[1] * intensity[1],
      edf_color[2] * intensity[2]
    };

    light.color.set_value(final_color);
    light.exposure.set_value(exposure);
    light.intensity.set_value(1.0f);

    // Set ShapingAPI attributes for cone control (inherited from BoundableLight)
    float inner_angle = 60.0f;
    conical_edf->inner_angle.get_value().get_scalar(&inner_angle);
    light.shapingConeAngle.set_value(inner_angle);

    if (conical_edf->outer_angle.authored()) {
      float outer_angle = 60.0f;
      if (auto oa_val = conical_edf->outer_angle.get_value()) {
        oa_val.value().get_scalar(&outer_angle);
        // Use softness to represent the difference between inner and outer angles
        float softness = (outer_angle - inner_angle) / inner_angle;
        light.shapingConeSoftness.set_value(std::max(0.0f, softness));
      }
    }

    (*usd_light) = light;
    return true;

  } else if (auto measured_edf = edf_value.as<MtlxMeasuredEdf>()) {
    // measured_edf -> RectLight or SphereLight with IES profile via ShapingAPI
    SphereLight light;

    value::color3f edf_color{1.0f, 1.0f, 1.0f};
    measured_edf->color.get_value().get_scalar(&edf_color);

    // Combine EDF color with light intensity
    value::color3f final_color{
      edf_color[0] * intensity[0],
      edf_color[1] * intensity[1],
      edf_color[2] * intensity[2]
    };

    light.color.set_value(final_color);
    light.exposure.set_value(exposure);
    light.intensity.set_value(1.0f);

    // Set ShapingAPI attributes with IES profile (inherited from BoundableLight)
    if (measured_edf->file.authored()) {
      if (auto file_val = measured_edf->file.get_value()) {
        value::AssetPath ies_file;
        if (file_val.value().get_scalar(&ies_file)) {
          light.shapingIesFile.set_value(ies_file);
          light.shapingIesNormalize.set_value(true);
        }
      }
    }

    (*usd_light) = light;
    return true;

  } else {
    PUSH_ERROR_AND_RETURN(fmt::format("Unknown EDF type for node '{}'", edf_name.str()));
  }

  return false;
}

//} // namespace usdMtlx
}  // namespace lightusd

#else

namespace lightusd {

bool ReadMaterialXFromString(const std::string &str,
                             const std::string &asset_name, MtlxModel *mtlx,
                             std::string *warn, std::string *err,
                             const MtlxConfig &config) {
  (void)str;
  (void)asset_name;
  (void)mtlx;
  (void)warn;
  (void)config;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }
  return false;
}

bool ReadMaterialXFromFile(const AssetResolutionResolver &resolver,
                           const std::string &asset_path, MtlxModel *mtlx,
                           std::string *warn, std::string *err,
                           const MtlxConfig &config) {
  (void)resolver;
  (void)asset_path;
  (void)mtlx;
  (void)warn;
  (void)config;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }
  return false;
}

bool WriteMaterialXToString(const MtlxModel &mtlx, std::string &xml_str,
                            std::string *warn, std::string *err) {
  (void)mtlx;
  (void)xml_str;
  (void)warn;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }
  return false;
}

bool ToPrimSpec(const MtlxModel &model, PrimSpec &ps, std::string *err) {
  (void)model;
  (void)ps;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }
  return false;
}

bool LoadMaterialXFromAsset(const Asset &asset, const std::string &asset_path,
                            PrimSpec &ps /* inout */, std::string *warn,
                            std::string *err) {
  (void)asset;
  (void)asset_path;
  (void)ps;
  (void)warn;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }

  return false;
}

bool ConvertMtlxLightToUsdLux(const MtlxLight &mtlx_light,
                               const std::map<std::string, value::Value> &light_shaders,
                               value::Value *usd_light,
                               std::string *warn, std::string *err) {
  (void)mtlx_light;
  (void)light_shaders;
  (void)usd_light;
  (void)warn;

  if (err) {
    (*err) += "MaterialX support is disabled in this build.\n";
  }
  return false;
}

}  // namespace lightusd

#endif  // LIGHTUSD_USE_USDMTLX
