#include "json-to-usd.hh"

#include "layer.hh"
#include "ascii-parser.hh"
#include "minijson.hh"
#include "security-policy.hh"
#include "str-util.hh"
#include "common-macros.inc"
#include "composition.hh"
#include "usdGeom.hh"
#include "core/model-scope.hh"
#include "usd-to-json.hh"
#if defined(LIGHTUSD_WITH_TYDRA)
#include "tydra/value-to-json.hh"
#endif

namespace {

constexpr int kJSONMaxParseDepth = 256;

}  // namespace

#include <limits>

namespace lightusd {

using json = minijson::Value;

static bool JSONToPrimSpecImpl(const json &, PrimSpec *, std::string *,
                               std::string *);

struct JSONToUSDContext;
namespace detail {
static bool JSONToGeomMesh(const json &, GeomMesh *, JSONToUSDContext *,
                           std::string *, std::string *);
static bool JSONToGeomBasisCurves(const json &, GeomBasisCurves *,
                                  JSONToUSDContext *, std::string *,
                                  std::string *);
}

static bool GetSizeT(const json &value, size_t *out, const char *name,
                     std::string *err) {
  if (!out) {
    if (err) {
      (*err) = std::string(name ? name : "value") + " output pointer is null";
    }
    return false;
  }
  if (value.as_size_t(out)) {
    return true;
  }

  if (value.is_number_unsigned()) {
    if (err) {
      (*err) = std::string(name ? name : "value") + " overflows size_t";
    }
    return false;
  }

  if (err) {
    (*err) = std::string(name ? name : "value") + " must be a non-negative integer";
  }
  return false;
}

namespace {


bool ParsePrimSpecifier(const json &j, Prim *prim, std::string *err) {
  if (!j.contains("specifier") || !j["specifier"].is_string()) return true;
  const std::string specifier = j["specifier"].get_string();
  if (specifier == "def") {
    prim->specifier() = Specifier::Def;
  } else if (specifier == "over") {
    prim->specifier() = Specifier::Over;
  } else if (specifier == "class") {
    prim->specifier() = Specifier::Class;
  } else {
    if (err) *err = "Unknown prim specifier: " + specifier;
    return false;
  }
  return true;
}

bool ParseAxis(const json &j, Axis *axis, std::string *err) {
  const json *value = j.find("axis");
  if (!value) return true;
  if (!value->is_string()) {
    if (err) *err = "Geometry axis must be a string";
    return false;
  }
  const std::string name = value->get_string();
  if (name == "X") *axis = Axis::X;
  else if (name == "Y") *axis = Axis::Y;
  else if (name == "Z") *axis = Axis::Z;
  else {
    if (err) *err = "Unknown geometry axis: " + name;
    return false;
  }
  return true;
}

bool ParseDoubleField(const json &j, const char *name, double *out,
                      std::string *err) {
  const json *value = j.find(name);
  if (!value) return true;
  if (!value->as_double(out)) {
    if (err) *err = std::string(name) + " must be a number";
    return false;
  }
  return true;
}

bool JSONToRuntimePrim(const json &j, const std::string &name, Prim *out,
                       JSONToUSDContext *context, std::string *warn,
                       std::string *err) {
  (void)warn;
  if (!out || !j.is_object()) {
    if (err) *err = "Prim JSON must be an object";
    return false;
  }
  const json *type_value = j.find("typeName");
  if (!type_value || !type_value->is_string()) {
    if (err) *err = "Prim JSON is missing string typeName";
    return false;
  }
  const std::string type_name = type_value->get_string();
  if (type_name == "GeomMesh") {
    GeomMesh mesh;
    mesh.name = name;
    if (!detail::JSONToGeomMesh(j, &mesh, context, warn, err)) return false;
    *out = Prim(name, std::move(mesh));
  } else if (type_name == "GeomBasisCurves") {
    GeomBasisCurves curves;
    curves.name = name;
    if (!detail::JSONToGeomBasisCurves(j, &curves, context, warn, err)) {
      return false;
    }
    *out = Prim(name, std::move(curves));
  } else if (type_name == "Xform") {
    Xform xform;
    xform.name = name;
    *out = Prim(name, std::move(xform));
  } else if (type_name == "GeomSphere") {
    GeomSphere sphere;
    sphere.name = name;
    const json *radius = j.find("radius");
    if (radius) {
      double value = 0.0;
      if (!radius->as_double(&value)) {
        if (err) *err = "GeomSphere radius must be a number";
        return false;
      }
      sphere.radius = value;
    }
    *out = Prim(name, std::move(sphere));
  } else if (type_name == "GeomCube") {
    GeomCube cube;
    cube.name = name;
    const json *size = j.find("size");
    if (size) {
      double value = 0.0;
      if (!size->as_double(&value)) {
        if (err) *err = "GeomCube size must be a number";
        return false;
      }
      cube.size = value;
    }
    *out = Prim(name, std::move(cube));
  } else if (type_name == "GeomCone" || type_name == "GeomCylinder" ||
             type_name == "GeomCapsule" || type_name == "GeomPlane") {
    double first = 0.0, second = 0.0;
    Axis axis = Axis::Z;
    if (!ParseAxis(j, &axis, err)) return false;
    if (type_name == "GeomCone") {
      GeomCone cone;
      cone.name = name;
      if (!ParseDoubleField(j, "height", &first, err) ||
          !ParseDoubleField(j, "radius", &second, err)) return false;
      if (j.contains("height")) cone.height = first;
      if (j.contains("radius")) cone.radius = second;
      if (j.contains("axis")) cone.axis = axis;
      *out = Prim(name, std::move(cone));
    } else if (type_name == "GeomCylinder") {
      GeomCylinder cylinder;
      cylinder.name = name;
      if (!ParseDoubleField(j, "height", &first, err) ||
          !ParseDoubleField(j, "radius", &second, err)) return false;
      if (j.contains("height")) cylinder.height = first;
      if (j.contains("radius")) cylinder.radius = second;
      if (j.contains("axis")) cylinder.axis = axis;
      *out = Prim(name, std::move(cylinder));
    } else if (type_name == "GeomCapsule") {
      GeomCapsule capsule;
      capsule.name = name;
      if (!ParseDoubleField(j, "height", &first, err) ||
          !ParseDoubleField(j, "radius", &second, err)) return false;
      if (j.contains("height")) capsule.height = first;
      if (j.contains("radius")) capsule.radius = second;
      if (j.contains("axis")) capsule.axis = axis;
      *out = Prim(name, std::move(capsule));
    } else {
      GeomPlane plane;
      plane.name = name;
      if (!ParseDoubleField(j, "width", &first, err) ||
          !ParseDoubleField(j, "length", &second, err)) return false;
      if (j.contains("width")) plane.width = first;
      if (j.contains("length")) plane.length = second;
      if (j.contains("axis")) plane.axis = axis;
      *out = Prim(name, std::move(plane));
    }
  } else {
    // Preserve unknown schemas as generic Model prims. The typed runtime
    // carriers above remain preferred for renderable geometry, while Model
    // keeps hierarchy and the authored schema name available for inspection.
    Model model;
    model.name = name;
    model.prim_type_name = type_name;
    PrimSpec generic_spec;
    if (!JSONToPrimSpecImpl(j, &generic_spec, warn, err)) return false;
    model.props = std::move(generic_spec.props());
    if (const json *properties = j.find("properties");
        properties && properties->is_object()) {
      for (const auto &member : *properties->object_items()) {
        if (const json *custom = member.value().find("isCustom");
            custom && custom->is_boolean()) {
          auto prop = model.props.find(member.key);
          if (prop != model.props.end()) prop->second.set_custom(custom->get_bool());
        }
      }
    }
    if (j.contains("specifier") && j["specifier"].is_string()) {
      const std::string specifier = j["specifier"].get_string();
      model.spec = specifier == "class"
                       ? Specifier::Class
                       : specifier == "over" ? Specifier::Over : Specifier::Def;
    }
    *out = Prim(name, std::move(model));
  }
  if (!ParsePrimSpecifier(j, out, err)) return false;

  const json *children = j.find("primChildren");
  if (children) {
    if (!children->is_object()) {
      if (err) *err = "Prim primChildren must be an object";
      return false;
    }
    for (const auto &member : *children->object_items()) {
      Prim child;
      if (!JSONToRuntimePrim(member.value(), member.key, &child, context, warn,
                             err)) {
        return false;
      }
      std::string child_err;
      if (!out->add_child(std::move(child), false, &child_err)) {
        if (err) *err = "Failed to add JSON child `" + member.key + "`: " + child_err;
        return false;
      }
    }
  }
  return true;
}

bool ParseStageJSON(const std::string &json_string, json *out,
                    std::string *err) {
  minijson::Error parse_err;
  minijson::ParseOptions options;
  options.max_depth = kJSONMaxParseDepth;
  if (!minijson::Parse(json_string, out, &parse_err, options)) {
    if (err) *err = "Failed to parse string as JSON: " + parse_err.message;
    return false;
  }
  if (!out->is_object()) {
    if (err) *err = "Stage JSON must be an object";
    return false;
  }
  return true;
}

}  // namespace

///
/// JSON to USD conversion context (stores buffers, views, and accessors for deserialization)
///
struct JSONToUSDContext {
  const JSONToUSDOptions *options{nullptr};
  std::vector<std::vector<uint8_t>> buffers;  // Raw buffer data
  std::vector<JSONBufferView> bufferViews;    // Buffer view information
  std::vector<JSONAccessor> accessors;        // Accessor information

  // Parse buffer data from JSON
  bool ParseBuffers(const json& j, std::string* err = nullptr);
  bool ParseBufferViews(const json& j, std::string* err = nullptr);
  bool ParseAccessors(const json& j, std::string* err = nullptr);

  // Get raw array bytes from an accessor. Explicit typed wrappers avoid
  // template instantiations in this JSON codec.
  bool GetArrayBytesFromAccessor(size_t accessorIndex, size_t element_size,
                                 std::vector<uint8_t>* result,
                                 std::string* err = nullptr);
  bool GetFloatArrayFromAccessor(size_t accessorIndex, std::vector<float>* result,
                                 std::string* err = nullptr);
  bool GetIntArrayFromAccessor(size_t accessorIndex, std::vector<int>* result,
                               std::string* err = nullptr);
};

// Implementation of buffer parsing methods
bool JSONToUSDContext::ParseBuffers(const json& j, std::string* err) {
  if (!j.is_array()) {
    if (err) (*err) = "Buffers must be an array";
    return false;
  }

  buffers.clear();
  buffers.reserve(j.size());

  for (const auto& buffer_obj : j) {
    if (!buffer_obj.is_object() || !buffer_obj.contains("byteLength") || !buffer_obj.contains("uri")) {
      if (err) (*err) = "Invalid buffer object";
      return false;
    }

    size_t byteLength = 0;
    if (!GetSizeT(buffer_obj["byteLength"], &byteLength, "buffer.byteLength", err)) {
      return false;
    }
    if (!buffer_obj["uri"].is_string()) {
      if (err) (*err) = "buffer.uri must be a string";
      return false;
    }
    std::string uri = buffer_obj["uri"].get_string();

    std::vector<uint8_t> buffer_data;

    if (uri.find("data:application/octet-stream;base64,") == 0) {
      // Embedded base64 data
      std::string base64_data = uri.substr(37);  // Skip "data:application/octet-stream;base64,"
      if (base64_data.size() > security_policy::kJSONMaxBase64InputChars) {
        if (err) (*err) = "Embedded base64 buffer is too large";
        return false;
      }
      if (byteLength > security_policy::kJSONMaxDecodedBytes) {
        if (err) (*err) = "Embedded buffer byteLength exceeds limit";
        return false;
      }
      size_t estimated_decoded_size = 0;
      if (!security_policy::EstimateBase64DecodedSize(base64_data, &estimated_decoded_size)) {
        if (err) (*err) = "Invalid base64 buffer encoding";
        return false;
      }
      if (estimated_decoded_size != byteLength) {
        if (err) (*err) = "Buffer size mismatch";
        return false;
      }
      std::string decoded = base64_decode(base64_data);

      if (decoded.size() != byteLength) {
        if (err) (*err) = "Buffer size mismatch";
        return false;
      }

      // Move the bytes across in a single pass: assign() avoids resize()'s
      // redundant zero-fill of the whole buffer before the copy.
      buffer_data.assign(decoded.begin(), decoded.end());
    } else {
      if (!options || !options->resolver) {
        if (err) (*err) = "External buffer URI requires a JSON asset resolver";
        return false;
      }
      if (byteLength > security_policy::kJSONMaxDecodedBytes ||
          (options->max_external_buffer_bytes != 0 &&
           byteLength > options->max_external_buffer_bytes)) {
        if (err) (*err) = "External buffer exceeds configured byte limit";
        return false;
      }

      const std::string resolved = options->resolver->resolve(uri);
      if (resolved.empty()) {
        if (err) (*err) = "Failed to resolve external buffer URI: " + uri;
        return false;
      }
      Asset asset;
      std::string open_warn;
      if (!options->resolver->open_asset(resolved, uri, &asset, &open_warn,
                                         err)) {
        return false;
      }
      if (asset.size() != byteLength) {
        if (err) (*err) = "External buffer size mismatch";
        return false;
      }
      buffer_data.assign(asset.data(), asset.data() + asset.size());
    }

    buffers.push_back(std::move(buffer_data));
  }

  return true;
}

bool JSONToUSDContext::ParseBufferViews(const json& j, std::string* err) {
  if (!j.is_array()) {
    if (err) (*err) = "BufferViews must be an array";
    return false;
  }

  bufferViews.clear();
  bufferViews.reserve(j.size());

  for (const auto& bufferView_obj : j) {
    if (!bufferView_obj.is_object() || !bufferView_obj.contains("buffer") ||
        !bufferView_obj.contains("byteOffset") || !bufferView_obj.contains("byteLength")) {
      if (err) (*err) = "Invalid bufferView object";
      return false;
    }

    JSONBufferView bufferView;
    if (!GetSizeT(bufferView_obj["buffer"], &bufferView.buffer, "bufferView.buffer", err) ||
        !GetSizeT(bufferView_obj["byteOffset"], &bufferView.byteOffset, "bufferView.byteOffset", err) ||
        !GetSizeT(bufferView_obj["byteLength"], &bufferView.byteLength, "bufferView.byteLength", err)) {
      return false;
    }
    bufferView.byteStride = 0;

    if (bufferView_obj.contains("byteStride")) {
      if (!GetSizeT(bufferView_obj["byteStride"], &bufferView.byteStride, "bufferView.byteStride", err)) {
        return false;
      }
    }

    bufferViews.push_back(bufferView);
  }

  return true;
}

bool JSONToUSDContext::ParseAccessors(const json& j, std::string* err) {
  if (!j.is_array()) {
    if (err) (*err) = "Accessors must be an array";
    return false;
  }

  accessors.clear();
  accessors.reserve(j.size());

  for (const auto& accessor_obj : j) {
    if (!accessor_obj.is_object() || !accessor_obj.contains("bufferView") ||
        !accessor_obj.contains("componentType") || !accessor_obj.contains("count") ||
        !accessor_obj.contains("type")) {
      if (err) (*err) = "Invalid accessor object";
      return false;
    }

    JSONAccessor accessor;
    if (!GetSizeT(accessor_obj["bufferView"], &accessor.bufferView, "accessor.bufferView", err)) {
      return false;
    }
    accessor.byteOffset = 0;
    if (!accessor_obj["componentType"].is_string() ||
        !accessor_obj["type"].is_string()) {
      if (err) (*err) = "accessor.componentType and accessor.type must be strings";
      return false;
    }
    accessor.componentType = accessor_obj["componentType"].get_string();
    if (!GetSizeT(accessor_obj["count"], &accessor.count, "accessor.count", err)) {
      return false;
    }
    accessor.type = accessor_obj["type"].get_string();

    if (accessor_obj.contains("byteOffset")) {
      if (!GetSizeT(accessor_obj["byteOffset"], &accessor.byteOffset, "accessor.byteOffset", err)) {
        return false;
      }
    }

    accessors.push_back(accessor);
  }

  return true;
}

bool JSONToUSDContext::GetArrayBytesFromAccessor(size_t accessorIndex,
                                                 size_t elementSize,
                                                 std::vector<uint8_t>* result,
                                                 std::string* err) {
  if (!result) {
    if (err) (*err) = "Result pointer is null";
    return false;
  }

  if (accessorIndex >= accessors.size()) {
    if (err) (*err) = "Accessor index out of range";
    return false;
  }

  const auto& accessor = accessors[accessorIndex];

  if (accessor.bufferView >= bufferViews.size()) {
    if (err) (*err) = "BufferView index out of range";
    return false;
  }

  const auto& bufferView = bufferViews[accessor.bufferView];

  if (bufferView.buffer >= buffers.size()) {
    if (err) (*err) = "Buffer index out of range";
    return false;
  }

  const auto& buffer = buffers[bufferView.buffer];

  if (bufferView.byteOffset > buffer.size()) {
    if (err) (*err) = "BufferView byteOffset out of bounds";
    return false;
  }
  if (accessor.byteOffset > (buffer.size() - bufferView.byteOffset)) {
    if (err) (*err) = "Accessor byteOffset out of bounds";
    return false;
  }
  size_t data_offset = bufferView.byteOffset + accessor.byteOffset;

  auto component_count = [](const std::string& type) -> size_t {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
  };

  const size_t components = component_count(accessor.type);
  if (components == 0 || elementSize == 0) {
    if (err) (*err) = "Unsupported accessor type or element size";
    return false;
  }

  if ((elementSize == sizeof(float) && accessor.componentType != "FLOAT") ||
      (elementSize == sizeof(int) && accessor.componentType != "UNSIGNED_INT" &&
       accessor.componentType != "INT")) {
    if (err) (*err) = "Accessor component type does not match requested array type";
    return false;
  }

  size_t packedElementSize = 0;
  if (!safe::mul(elementSize, components, &packedElementSize)) {
    if (err) (*err) = "Accessor element size overflow";
    return false;
  }

  // The current writer historically stored vector accessors with count equal
  // to the number of scalar components. Newer producers commonly store the
  // logical element count. Accept both forms: a tightly packed view whose
  // length only accommodates count * elementSize is the legacy form.
  size_t packedBytes = 0;
  if (!safe::mul(accessor.count, packedElementSize, &packedBytes)) {
    if (err) (*err) = "Accessor size overflow";
    return false;
  }
  size_t legacyBytes = 0;
  if (!safe::mul(accessor.count, elementSize, &legacyBytes)) {
    if (err) (*err) = "Accessor size overflow";
    return false;
  }

  if (accessor.byteOffset > bufferView.byteLength) {
    if (err) (*err) = "Accessor byteOffset exceeds bufferView";
    return false;
  }
  const size_t viewBytes = bufferView.byteLength - accessor.byteOffset;
  bool legacy_scalarized = (components > 1 && bufferView.byteStride == 0 &&
                            packedBytes > viewBytes && legacyBytes <= viewBytes);
  // In the legacy form count is already the number of scalar values returned
  // to the USD decoder; only the accessor's type is vector-shaped.
  const size_t logicalCount = accessor.count;
  if (legacy_scalarized && (accessor.count % components) != 0) {
    if (err) (*err) = "Legacy vector accessor count is not component aligned";
    return false;
  }

  const size_t elementBytes = legacy_scalarized ? elementSize : packedElementSize;
  size_t requiredBytes = 0;
  if (logicalCount > 0) {
    const size_t stride = bufferView.byteStride == 0 ? elementBytes : bufferView.byteStride;
    if (stride < elementBytes) {
      if (err) (*err) = "Accessor byteStride is smaller than one element";
      return false;
    }
    size_t stepped = 0;
    if (!safe::mul(logicalCount - 1, stride, &stepped) ||
        !safe::add(stepped, elementBytes, &requiredBytes)) {
      if (err) (*err) = "Accessor stride size overflow";
      return false;
    }
  }
  if (requiredBytes > viewBytes || requiredBytes > (buffer.size() - data_offset)) {
    if (err) (*err) = "Buffer access out of bounds";
    return false;
  }

  size_t totalBytes = 0;
  if (!safe::mul(logicalCount, elementBytes, &totalBytes)) {
    if (err) (*err) = "Accessor output size overflow";
    return false;
  }
  result->resize(totalBytes);
  const uint8_t* srcData = buffer.data() + data_offset;
  const size_t stride = bufferView.byteStride == 0 ? elementBytes : bufferView.byteStride;
  if (stride == elementBytes) {
    std::memcpy(result->data(), srcData, totalBytes);
  } else {
    for (size_t i = 0; i < logicalCount; ++i) {
      std::memcpy(result->data() + i * elementBytes,
                  srcData + i * stride, elementBytes);
    }
  }

  return true;
}

bool JSONToUSDContext::GetFloatArrayFromAccessor(size_t accessorIndex,
                                                 std::vector<float>* result,
                                                 std::string* err) {
  std::vector<uint8_t> bytes;
  if (!GetArrayBytesFromAccessor(accessorIndex, sizeof(float), &bytes, err)) {
    return false;
  }
  if (bytes.size() % sizeof(float) != 0) {
    if (err) *err = "Float accessor byte size is not element aligned";
    return false;
  }
  result->resize(bytes.size() / sizeof(float));
  std::memcpy(result->data(), bytes.data(), bytes.size());
  return true;
}

bool JSONToUSDContext::GetIntArrayFromAccessor(size_t accessorIndex,
                                               std::vector<int>* result,
                                               std::string* err) {
  std::vector<uint8_t> bytes;
  if (!GetArrayBytesFromAccessor(accessorIndex, sizeof(int), &bytes, err)) {
    return false;
  }
  if (bytes.size() % sizeof(int) != 0) {
    if (err) *err = "Int accessor byte size is not element aligned";
    return false;
  }
  result->resize(bytes.size() / sizeof(int));
  std::memcpy(result->data(), bytes.data(), bytes.size());
  return true;
}

namespace detail {

// Helper functions for array deserialization from base64
bool DeserializeBytesFromBase64(const std::string& base64_data,
                                size_t element_size,
                                std::vector<uint8_t>* result) {
  if (!result || element_size == 0) {
    return false;
  }

  if (base64_data.empty()) {
    result->clear();
    return true;
  }

  if (base64_data.size() > security_policy::kJSONMaxBase64InputChars) {
    return false;
  }

  size_t estimated_decoded_size = 0;
  if (!security_policy::EstimateBase64DecodedSize(base64_data, &estimated_decoded_size)) {
    return false;
  }
  if (estimated_decoded_size > security_policy::kJSONMaxDecodedBytes) {
    return false;
  }

  std::string decoded = base64_decode(base64_data);
  if (decoded.empty()) {
    return false;
  }

  if (decoded.size() % element_size != 0) {
    return false;
  }

  result->assign(decoded.begin(), decoded.end());

  return true;
}


static bool DeserializeFloatArrayFromBase64(const std::string& base64_data, std::vector<float>* result) {
  std::vector<uint8_t> bytes;
  if (!DeserializeBytesFromBase64(base64_data, sizeof(float), &bytes)) return false;
  result->resize(bytes.size() / sizeof(float));
  std::memcpy(result->data(), bytes.data(), bytes.size());
  return true;
}


static bool JSONToMetadataValue(const json &j, MetaVariable *out,
                                std::string *err) {
  if (!out) {
    if (err) *err = "Metadata value output is null";
    return false;
  }
#if defined(LIGHTUSD_WITH_TYDRA)
  if (j.is_object() && j.contains("type") && j.contains("value") &&
      j["type"].is_string()) {
    auto typed = tydra::MiniJSONToValue(j, err);
    if (!typed) {
      if (err && err->empty()) *err = "Invalid typed metadata value";
      return false;
    }
    out->set_value(std::move(*typed));
    return true;
  }
#endif
  if (j.is_boolean()) {
    out->set_value(value::Value(j.get_bool()));
    return true;
  }
  if (j.is_number_integer()) {
    out->set_value(value::Value(j.get_int64()));
    return true;
  }
  if (j.is_number_unsigned()) {
    out->set_value(value::Value(j.get_uint64()));
    return true;
  }
  if (j.is_number()) {
    out->set_value(value::Value(j.get_double()));
    return true;
  }
  if (j.is_string()) {
    out->set_value(value::Value(j.get_string()));
    return true;
  }
  if (j.is_object()) {
    Dictionary dictionary;
    for (const auto &member : *j.object_items()) {
      MetaVariable value;
      if (!JSONToMetadataValue(member.value(), &value, err)) return false;
      dictionary[member.key] = std::move(value);
    }
    out->set_value(value::Value(std::move(dictionary)));
    return true;
  }
  if (err) *err = "Unsupported JSON metadata value";
  return false;
}

static bool JSONToMetadataDictionary(const json &j, Dictionary *out,
                                     std::string *err) {
  if (!out || !j.is_object()) {
    if (err) *err = "Metadata dictionary must be an object";
    return false;
  }
  out->clear();
  for (const auto &member : *j.object_items()) {
    MetaVariable value;
    if (!JSONToMetadataValue(member.value(), &value, err)) return false;
    (*out)[member.key] = std::move(value);
  }
  return true;
}

// Attribute metadata deserialization
static bool DeserializeAttributeMetadata(const json& metadata_json, AttrMetas* metas, std::string* err = nullptr) {
  if (!metas || !metadata_json.is_object()) {
    return false;
  }

  // Parse interpolation
  if (metadata_json.contains("interpolation") && metadata_json["interpolation"].is_string()) {
    std::string interp_str = metadata_json["interpolation"].get_string();
    if (interp_str == "constant") {
      metas->set_interpolation_enum(Interpolation::Constant);
    } else if (interp_str == "uniform") {
      metas->set_interpolation_enum(Interpolation::Uniform);
    } else if (interp_str == "varying") {
      metas->set_interpolation_enum(Interpolation::Varying);
    } else if (interp_str == "vertex") {
      metas->set_interpolation_enum(Interpolation::Vertex);
    } else if (interp_str == "faceVarying") {
      metas->set_interpolation_enum(Interpolation::FaceVarying);
    }
  }

  // Parse elementSize
  if (metadata_json.contains("elementSize") && metadata_json["elementSize"].is_number_integer()) {
    metas->set_elementSize(static_cast<uint32_t>(metadata_json["elementSize"].get_int()));
  }

  // Parse hidden flag
  if (metadata_json.contains("hidden") && metadata_json["hidden"].is_boolean()) {
    metas->set_hidden(metadata_json["hidden"].get_bool());
  }

  // Parse comment
  if (metadata_json.contains("comment") && metadata_json["comment"].is_string()) {
    value::StringData comment_data;
    comment_data.value = metadata_json["comment"].get_string();
    metas->set_comment(comment_data);
  }

  // Parse displayName
  if (metadata_json.contains("displayName") && metadata_json["displayName"].is_string()) {
    metas->set_displayName(metadata_json["displayName"].get_string());
  }

  if (metadata_json.contains("customData")) {
    Dictionary custom_data;
    if (!JSONToMetadataDictionary(metadata_json["customData"], &custom_data, err)) {
      return false;
    }
    metas->set_customData(custom_data);
  }

  if (metadata_json.contains("sdrMetadata")) {
    Dictionary sdr_metadata;
    if (!JSONToMetadataDictionary(metadata_json["sdrMetadata"], &sdr_metadata, err)) {
      return false;
    }
    metas->set_sdrMetadata(sdr_metadata);
  }

  return true;
}

// Helper function to parse array data from JSON (supports both base64 and accessor modes)
static bool ParseArrayFromJSON(const json& j, JSONToUSDContext* context, std::string* base64_data,
                        size_t* accessor_index, size_t* count, std::string* type, std::string* err) {
  if (!j.is_object()) {
    if (err) {
      (*err) = "Array data must be an object";
    }
    return false;
  }

  if (!j.contains("count") || !j.contains("type")) {
    if (err) {
      (*err) = "Array object must contain 'count' and 'type' fields";
    }
    return false;
  }

  if (!j["count"].is_number_unsigned()) {
    if (err) {
      (*err) = "'count' field must be a positive number";
    }
    return false;
  }

  if (!j["type"].is_string()) {
    if (err) {
      (*err) = "'type' field must be a string";
    }
    return false;
  }

  if (!GetSizeT(j["count"], count, "count", err)) {
    return false;
  }
  *type = j["type"].get_string();

  // Check for base64 mode
  if (j.contains("data")) {
    if (!j["data"].is_string()) {
      if (err) {
        (*err) = "'data' field must be a string";
      }
      return false;
    }
    *base64_data = j["data"].get_string();
    *accessor_index = SIZE_MAX;  // Invalid accessor index indicates base64 mode
    return true;
  }

  // Check for accessor mode
  if (j.contains("accessor")) {
    if (!j["accessor"].is_number_unsigned()) {
      if (err) {
        (*err) = "'accessor' field must be a positive number";
      }
      return false;
    }

    if (!context) {
      if (err) {
        (*err) = "Context required for accessor mode but not provided";
      }
      return false;
    }

    if (!GetSizeT(j["accessor"], accessor_index, "accessor", err)) {
      return false;
    }
    base64_data->clear();  // No base64 data in accessor mode
    return true;
  }

  if (err) {
    (*err) = "Array object must contain either 'data' (base64) or 'accessor' field";
  }
  return false;
}

// Byte-oriented array parser shared by the explicit int/float wrappers below.
static bool ParseAndDeserializeBytes(const json& array_json,
                                     JSONToUSDContext* context,
                                     const std::string& expected_type,
                                     size_t element_size,
                                     std::vector<uint8_t>* bytes,
                                     size_t* element_count,
                                     std::string* err) {
  std::string base64_data, type;
  size_t accessor_index, count;

  if (!ParseArrayFromJSON(array_json, context, &base64_data, &accessor_index, &count, &type, err)) {
    return false;
  }

  if (type != expected_type) {
    if (err) {
      (*err) = "Unexpected array type: " + type + ", expected: " + expected_type;
    }
    return false;
  }

  if (accessor_index == SIZE_MAX) {
    // Base64 mode
    if (!DeserializeBytesFromBase64(base64_data, element_size, bytes)) {
      return false;
    }
  } else {
    // Accessor mode
    if (context) {
      if (!context->GetArrayBytesFromAccessor(accessor_index, element_size,
                                              bytes, err)) {
        return false;
      }
    } else {
      if (err) {
        (*err) = "Context required for accessor mode";
      }
      return false;
    }
  }

  if (bytes->size() / element_size != count) {
    if (err) {
      (*err) = "Array count mismatch";
    }
    return false;
  }

  if (element_count) *element_count = count;
  return true;
}

static bool ParseAndDeserializeFloatArray(const json& array_json,
                                          JSONToUSDContext* context,
                                          const std::string& expected_type,
                                          std::vector<float>* result,
                                          std::string* err) {
  std::vector<uint8_t> bytes;
  size_t count = 0;
  if (!ParseAndDeserializeBytes(array_json, context, expected_type, sizeof(float),
                                &bytes, &count, err)) return false;
  result->resize(count);
  std::memcpy(result->data(), bytes.data(), bytes.size());
  return true;
}

static bool ParseAndDeserializeIntArray(const json& array_json,
                                        JSONToUSDContext* context,
                                        const std::string& expected_type,
                                        std::vector<int>* result,
                                        std::string* err) {
  std::vector<uint8_t> bytes;
  size_t count = 0;
  if (!ParseAndDeserializeBytes(array_json, context, expected_type, sizeof(int),
                                &bytes, &count, err)) return false;
  result->resize(count);
  std::memcpy(result->data(), bytes.data(), bytes.size());
  return true;
}

static bool ParseAndDeserializeFloatArrayWithMetadata(
    const json& array_json, JSONToUSDContext* context,
    const std::string& expected_type, std::vector<float>* result,
    AttrMetas* metas, std::string* err) {
  if (!array_json.is_object()) {
    if (err) (*err) = "Array data must be an object";
    return false;
  }

  // Parse array data (base64 or accessor)
  if (!ParseAndDeserializeFloatArray(array_json, context, expected_type, result,
                                     err)) {
    return false;
  }

  // Parse metadata if present
  if (array_json.contains("metadata") && metas) {
    if (!DeserializeAttributeMetadata(array_json["metadata"], metas, err)) {
      // Continue even if metadata parsing fails (metadata is optional)
      if (err) {
        *err = "Warning: Failed to parse metadata - " + *err;
      }
    }
  }

  return true;
}

static bool ParseAndDeserializeIntArrayWithMetadata(
    const json& array_json, JSONToUSDContext* context,
    const std::string& expected_type, std::vector<int>* result,
    AttrMetas* metas, std::string* err) {
  if (!array_json.is_object()) {
    if (err) (*err) = "Array data must be an object";
    return false;
  }
  if (!ParseAndDeserializeIntArray(array_json, context, expected_type, result,
                                   err)) return false;
  if (array_json.contains("metadata") && metas) {
    if (!DeserializeAttributeMetadata(array_json["metadata"], metas, err) && err) {
      *err = "Warning: Failed to parse metadata - " + *err;
    }
  }
  return true;
}

// Specialized metadata-aware parsing for point3f arrays
static bool ParsePoint3fArrayWithMetadata(const json& array_json, JSONToUSDContext* context,
                                  std::vector<value::point3f>* result, AttrMetas* metas, std::string* err) {
  if (!array_json.is_object()) {
    if (err) (*err) = "Points array must be an object";
    return false;
  }

  std::string base64_data, type;
  size_t accessor_index, count;

  if (!ParseArrayFromJSON(array_json, context, &base64_data, &accessor_index, &count, &type, err)) {
    return false;
  }

  if (type != "point3f[]") {
    if (err) (*err) = "Expected point3f[] type, got: " + type;
    return false;
  }

  if (count > ((std::numeric_limits<size_t>::max)() / 3)) {
    if (err) (*err) = "point3f count overflow";
    return false;
  }

  std::vector<float> float_data;
  bool data_success = false;

  if (accessor_index == SIZE_MAX) {
    // Base64 mode
    data_success = DeserializeFloatArrayFromBase64(base64_data, &float_data);
  } else {
    // Accessor mode
    if (context) {
      data_success = context->GetFloatArrayFromAccessor(accessor_index, &float_data, err);
    }
  }

  if (!data_success || float_data.size() != count * 3) {
    if (err) (*err) = "Failed to parse points data or size mismatch";
    return false;
  }

  result->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    value::point3f pt;
    pt[0] = float_data[i * 3 + 0];
    pt[1] = float_data[i * 3 + 1];
    pt[2] = float_data[i * 3 + 2];
    result->push_back(pt);
  }

  // Parse metadata if present
  if (array_json.contains("metadata") && metas) {
    DeserializeAttributeMetadata(array_json["metadata"], metas, err);
  }

  return true;
}

// Specialized metadata-aware parsing for normal3f arrays
static bool ParseNormal3fArrayWithMetadata(const json& array_json, JSONToUSDContext* context,
                                   std::vector<value::normal3f>* result, AttrMetas* metas, std::string* err) {
  if (!array_json.is_object()) {
    if (err) (*err) = "Normals array must be an object";
    return false;
  }

  std::vector<float> float_data;
  if (!ParseAndDeserializeFloatArray(array_json, context, "normal3f[]", &float_data, err)) {
    return false;
  }

  if (float_data.size() % 3 != 0) {
    if (err) (*err) = "Normal array size must be divisible by 3";
    return false;
  }

  size_t count = float_data.size() / 3;
  result->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    value::normal3f normal;
    normal[0] = float_data[i * 3 + 0];
    normal[1] = float_data[i * 3 + 1];
    normal[2] = float_data[i * 3 + 2];
    result->push_back(normal);
  }

  // Parse metadata if present
  if (array_json.contains("metadata") && metas) {
    DeserializeAttributeMetadata(array_json["metadata"], metas, err);
  }

  return true;
}

// JSON to GeomMesh conversion (with context support)
static bool JSONToGeomMesh(const json& j, GeomMesh* mesh, JSONToUSDContext* context, std::string* warn, std::string* err) {
  (void)warn;

  if (!mesh) {
    if (err) {
      (*err) = "Internal error: mesh is null";
    }
    return false;
  }

  if (!j.is_object()) {
    if (err) {
      (*err) = "JSON must be an object";
    }
    return false;
  }

  // Set name if present
  if (j.contains("name") && j["name"].is_string()) {
    mesh->name = j["name"].get_string();
  }

  // Parse points array with metadata
  if (j.contains("points")) {
    std::vector<value::point3f> points;
    if (ParsePoint3fArrayWithMetadata(j["points"], context, &points, &mesh->points.metas(), err)) {
      Animatable<std::vector<value::point3f>> animatable_points;
      animatable_points.set(points);
      mesh->points.set_value(animatable_points);
    } else {
      return false;
    }
  }

  // Parse face vertex counts array
  if (j.contains("faceVertexCounts")) {
    std::vector<int> int_data;
    if (ParseAndDeserializeIntArrayWithMetadata(j["faceVertexCounts"], context, "int[]", &int_data, &mesh->faceVertexCounts.metas(), err)) {
      Animatable<std::vector<int>> animatable_face_counts;
      animatable_face_counts.set(int_data);
      mesh->faceVertexCounts.set_value(animatable_face_counts);
    } else {
      return false;
    }
  }

  // Parse face vertex indices array
  if (j.contains("faceVertexIndices")) {
    std::vector<int> int_data;
    if (ParseAndDeserializeIntArrayWithMetadata(j["faceVertexIndices"], context, "int[]", &int_data, &mesh->faceVertexIndices.metas(), err)) {
      Animatable<std::vector<int>> animatable_face_indices;
      animatable_face_indices.set(int_data);
      mesh->faceVertexIndices.set_value(animatable_face_indices);
    } else {
      return false;
    }
  }

  // Parse normals array with metadata
  if (j.contains("normals")) {
    std::vector<value::normal3f> normals;
    if (ParseNormal3fArrayWithMetadata(j["normals"], context, &normals, &mesh->normals.metas(), err)) {
      Animatable<std::vector<value::normal3f>> animatable_normals;
      animatable_normals.set(normals);
      mesh->normals.set_value(animatable_normals);
    } else {
      return false;
    }
  }

  // Parse subdivision surface arrays using the helper
  if (j.contains("cornerIndices")) {
    std::vector<int> int_data;
    if (ParseAndDeserializeIntArrayWithMetadata(j["cornerIndices"], context, "int[]", &int_data, &mesh->cornerIndices.metas(), err)) {
      Animatable<std::vector<int>> animatable_corner_indices;
      animatable_corner_indices.set(int_data);
      mesh->cornerIndices.set_value(animatable_corner_indices);
    }
  }

  if (j.contains("cornerSharpnesses")) {
    std::vector<float> float_data;
    if (ParseAndDeserializeFloatArrayWithMetadata(j["cornerSharpnesses"], context, "float[]", &float_data, &mesh->cornerSharpnesses.metas(), err)) {
      Animatable<std::vector<float>> animatable_corner_sharpnesses;
      animatable_corner_sharpnesses.set(float_data);
      mesh->cornerSharpnesses.set_value(animatable_corner_sharpnesses);
    }
  }

  if (j.contains("creaseIndices")) {
    std::vector<int> int_data;
    if (ParseAndDeserializeIntArrayWithMetadata(j["creaseIndices"], context, "int[]", &int_data, &mesh->creaseIndices.metas(), err)) {
      Animatable<std::vector<int>> animatable_crease_indices;
      animatable_crease_indices.set(int_data);
      mesh->creaseIndices.set_value(animatable_crease_indices);
    }
  }

  if (j.contains("creaseLengths")) {
    std::vector<int> int_data;
    if (ParseAndDeserializeIntArrayWithMetadata(j["creaseLengths"], context, "int[]", &int_data, &mesh->creaseLengths.metas(), err)) {
      Animatable<std::vector<int>> animatable_crease_lengths;
      animatable_crease_lengths.set(int_data);
      mesh->creaseLengths.set_value(animatable_crease_lengths);
    }
  }

  if (j.contains("creaseSharpnesses")) {
    std::vector<float> float_data;
    if (ParseAndDeserializeFloatArrayWithMetadata(j["creaseSharpnesses"], context, "float[]", &float_data, &mesh->creaseSharpnesses.metas(), err)) {
      Animatable<std::vector<float>> animatable_crease_sharpnesses;
      animatable_crease_sharpnesses.set(float_data);
      mesh->creaseSharpnesses.set_value(animatable_crease_sharpnesses);
    }
  }

  return true;
}

static bool JSONToGeomBasisCurves(const json &j, GeomBasisCurves *curves,
                                  JSONToUSDContext *context, std::string *warn,
                                  std::string *err) {
  (void)warn;
  if (!curves || !j.is_object()) {
    if (err) *err = "Basis curves JSON must be an object";
    return false;
  }
  if (j.contains("name") && j["name"].is_string()) {
    curves->name = j["name"].get_string();
  }
  if (j.contains("points")) {
    std::vector<value::point3f> data;
    if (!ParsePoint3fArrayWithMetadata(j["points"], context, &data,
                                       &curves->points.metas(), err)) {
      return false;
    }
    Animatable<std::vector<value::point3f>> anim;
    anim.set(data);
    curves->points.set_value(anim);
  }
  if (j.contains("normals")) {
    std::vector<value::normal3f> data;
    if (!ParseNormal3fArrayWithMetadata(j["normals"], context, &data,
                                        &curves->normals.metas(), err)) {
      return false;
    }
    Animatable<std::vector<value::normal3f>> anim;
    anim.set(data);
    curves->normals.set_value(anim);
  }
  if (j.contains("curveVertexCounts")) {
    std::vector<int> data;
    if (!ParseAndDeserializeIntArrayWithMetadata(
            j["curveVertexCounts"], context, "int[]", &data,
            &curves->curveVertexCounts.metas(), err)) {
      return false;
    }
    Animatable<std::vector<int>> anim;
    anim.set(data);
    curves->curveVertexCounts.set_value(anim);
  }
  if (j.contains("widths")) {
    std::vector<float> data;
    if (!ParseAndDeserializeFloatArrayWithMetadata(
            j["widths"], context, "float[]", &data, &curves->widths.metas(),
            err)) {
      return false;
    }
    Animatable<std::vector<float>> anim;
    anim.set(data);
    curves->widths.set_value(anim);
  }
  return true;
}

} // namespace detail


static bool ParsePathValue(const json &value, Path *path, std::string *err) {
  if (!path || !value.is_string()) {
    if (err) *err = "Path value must be a string";
    return false;
  }
  Path parsed(value.get_string(), "");
  if (!parsed.is_valid()) {
    if (err) *err = "Invalid USD path: " + value.get_string();
    return false;
  }
  *path = std::move(parsed);
  return true;
}

static bool ParseAttributeJSON(const json &j, const std::string &name,
                               Property *property, std::string *err) {
  if (!property || !j.is_object()) {
    if (err) *err = "Attribute must be an object";
    return false;
  }
  const std::string type_name = j.contains("typeName") && j["typeName"].is_string()
                                    ? j["typeName"].get_string() : std::string();
  Attribute attr;
  attr.set_name(name);
  attr.set_type_name(type_name);

  if (j.contains("metadata")) {
    if (!detail::DeserializeAttributeMetadata(j["metadata"], &attr.metas(), err)) {
      if (err && err->empty()) *err = "Invalid attribute metadata";
      return false;
    }
  }

  if (j.contains("isConnection") && j["isConnection"].is_boolean() &&
      j["isConnection"].get_bool()) {
    std::vector<Path> paths;
    if (j.contains("connection")) {
      Path path;
      if (!ParsePathValue(j["connection"], &path, err)) return false;
      paths.push_back(std::move(path));
    } else if (j.contains("connections") && j["connections"].is_array()) {
      for (const auto &item : j["connections"]) {
        Path path;
        if (!ParsePathValue(item, &path, err)) return false;
        paths.push_back(std::move(path));
      }
    }
    if (paths.empty()) {
      if (err) *err = "Connection attribute has no connection path";
      return false;
    }
    attr.set_connections(std::move(paths));
  } else if (j.contains("valueType") && j["valueType"].is_string() &&
             j["valueType"].get_string() == "blocked") {
    attr.set_blocked(true);
  } else if (j.contains("hasValue") && j["hasValue"].is_boolean() &&
             j["hasValue"].get_bool() && j.contains("value") &&
             j["value"].is_string()) {
    value::Value parsed;
    std::string parse_error;
    if (type_name.empty() || !ascii::ParseUnregistredValue(
                                 type_name, j["value"].get_string(), &parsed,
                                 &parse_error)) {
      if (err) *err = "Failed to parse attribute `" + name + "`: " + parse_error;
      return false;
    }
    attr.get_var().set_value(std::move(parsed));
  }

  if (j.contains("hasTimeSamples") && j["hasTimeSamples"].is_boolean() &&
      j["hasTimeSamples"].get_bool() && j.contains("timeSamples") &&
      j["timeSamples"].is_array()) {
    for (const auto &sample : j["timeSamples"]) {
      if (!sample.is_object() || !sample.contains("time") ||
          !sample["time"].is_number()) {
        if (err) *err = "Invalid attribute time sample";
        return false;
      }
      const double time = sample["time"].get_double();
      const bool blocked = sample.contains("blocked") &&
                           sample["blocked"].is_boolean() &&
                           sample["blocked"].get_bool();
      if (blocked) {
        value::Value block(value::ValueBlock{});
        attr.get_var().set_timesample(time, block);
      } else if (sample.contains("value") && sample["value"].is_string()) {
        value::Value parsed;
        std::string parse_error;
        if (type_name.empty() || !ascii::ParseUnregistredValue(
                                     type_name, sample["value"].get_string(),
                                     &parsed, &parse_error)) {
          if (err) *err = "Failed to parse time sample for `" + name + "`: " + parse_error;
          return false;
        }
        attr.get_var().set_timesample(time, parsed);
      } else {
        if (err) *err = "Time sample must contain a string value or be blocked";
        return false;
      }
    }
  }

  *property = Property(std::move(attr),
                       j.contains("isCustom") && j["isCustom"].is_boolean() &&
                           j["isCustom"].get_bool());
  return true;
}

static bool ParseRelationshipJSON(const json &j, const std::string &name,
                                  Property *property, std::string *err) {
  if (!property || !j.is_object()) {
    if (err) *err = "Relationship must be an object";
    return false;
  }
  Relationship rel;
  const std::string value_type = j.contains("valueType") && j["valueType"].is_string()
                                     ? j["valueType"].get_string() : std::string();
  if (value_type == "valueBlock" || (j.contains("blocked") && j["blocked"].is_boolean() &&
                                      j["blocked"].get_bool())) {
    rel.set_blocked();
  } else if (j.contains("target")) {
    Path path;
    if (!ParsePathValue(j["target"], &path, err)) return false;
    rel.set(path);
  } else if (j.contains("targets") && j["targets"].is_array()) {
    std::vector<Path> paths;
    for (const auto &item : j["targets"]) {
      Path path;
      if (!ParsePathValue(item, &path, err)) return false;
      paths.push_back(std::move(path));
    }
    rel.set(std::move(paths));
  }
  Property parsed(std::move(rel),
                  j.contains("isCustom") && j["isCustom"].is_boolean() &&
                      j["isCustom"].get_bool());
  parsed.set_listedit_qual(ListEditQual::ResetToExplicit);
  (void)name;
  *property = std::move(parsed);
  return true;
}

static bool ParseListEditQualifier(const json &value, ListEditQual *qual,
                                   std::string *err) {
  if (!qual || !value.is_string()) {
    if (err) *err = "Composition list-op `op` must be a string";
    return false;
  }
  const std::string op = value.get_string();
  if (op.empty() || op == "explicit") *qual = ListEditQual::ResetToExplicit;
  else if (op == "delete") *qual = ListEditQual::Delete;
  else if (op == "add") *qual = ListEditQual::Add;
  else if (op == "prepend") *qual = ListEditQual::Prepend;
  else if (op == "append") *qual = ListEditQual::Append;
  else if (op == "reorder" || op == "order") *qual = ListEditQual::Order;
  else {
    if (err) *err = "Unknown composition list-op: " + op;
    return false;
  }
  return true;
}

static bool ParsePathListOps(const json &value,
                             std::vector<std::pair<ListEditQual, std::vector<Path>>> *out,
                             std::string *err) {
  if (!out || !value.is_array()) {
    if (err) *err = "Composition path list-ops must be an array";
    return false;
  }
  for (const auto &op_json : value) {
    if (!op_json.is_object() || !op_json.contains("op") ||
        !op_json.contains("items") || !op_json["items"].is_array()) {
      if (err) *err = "Invalid composition path list-op";
      return false;
    }
    ListEditQual qual;
    if (!ParseListEditQualifier(op_json["op"], &qual, err)) return false;
    std::vector<Path> paths;
    paths.reserve(op_json["items"].size());
    for (const auto &item : op_json["items"]) {
      Path path;
      if (!ParsePathValue(item, &path, err)) return false;
      paths.push_back(std::move(path));
    }
    out->emplace_back(qual, std::move(paths));
  }
  return true;
}

static bool ParseReferenceListOps(const json &value,
                                  std::vector<std::pair<ListEditQual, std::vector<Reference>>> *out,
                                  std::string *err) {
  if (!out || !value.is_array()) {
    if (err) *err = "Reference list-ops must be an array";
    return false;
  }
  for (const auto &op_json : value) {
    if (!op_json.is_object() || !op_json.contains("op") ||
        !op_json.contains("items") || !op_json["items"].is_array()) {
      if (err) *err = "Invalid reference list-op";
      return false;
    }
    ListEditQual qual;
    if (!ParseListEditQualifier(op_json["op"], &qual, err)) return false;
    std::vector<Reference> refs;
    refs.reserve(op_json["items"].size());
    for (const auto &item : op_json["items"]) {
      if (!item.is_object()) {
        if (err) *err = "Reference item must be an object";
        return false;
      }
      Reference ref;
      if (item.contains("assetPath")) {
        if (!item["assetPath"].is_string()) {
          if (err) *err = "Reference assetPath must be a string";
          return false;
        }
        ref.asset_path = value::AssetPath(item["assetPath"].get_string());
      }
      if (item.contains("primPath")) {
        if (!ParsePathValue(item["primPath"], &ref.prim_path, err)) return false;
      }
      if (item.contains("offset")) {
        if (!item["offset"].as_double(&ref.layerOffset._offset)) {
          if (err) *err = "Reference offset must be a number";
          return false;
        }
      }
      if (item.contains("scale")) {
        if (!item["scale"].as_double(&ref.layerOffset._scale)) {
          if (err) *err = "Reference scale must be a number";
          return false;
        }
      }
      if (item.contains("customData")) {
        if (!detail::JSONToMetadataDictionary(item["customData"],
                                              &ref.customData, err)) {
          return false;
        }
      }
      refs.push_back(std::move(ref));
    }
    out->emplace_back(qual, std::move(refs));
  }
  return true;
}

static bool ParsePayloadListOps(const json &value,
                                std::vector<std::pair<ListEditQual, std::vector<Payload>>> *out,
                                std::string *err) {
  if (!out || !value.is_array()) {
    if (err) *err = "Payload list-ops must be an array";
    return false;
  }
  for (const auto &op_json : value) {
    if (!op_json.is_object() || !op_json.contains("op") ||
        !op_json.contains("items") || !op_json["items"].is_array()) {
      if (err) *err = "Invalid payload list-op";
      return false;
    }
    ListEditQual qual;
    if (!ParseListEditQualifier(op_json["op"], &qual, err)) return false;
    std::vector<Payload> payloads;
    payloads.reserve(op_json["items"].size());
    for (const auto &item : op_json["items"]) {
      if (!item.is_object()) {
        if (err) *err = "Payload item must be an object";
        return false;
      }
      Payload payload;
      if (item.contains("assetPath")) {
        if (!item["assetPath"].is_string()) {
          if (err) *err = "Payload assetPath must be a string";
          return false;
        }
        payload.asset_path = value::AssetPath(item["assetPath"].get_string());
      }
      if (item.contains("primPath") &&
          !ParsePathValue(item["primPath"], &payload.prim_path, err)) return false;
      if (item.contains("offset") &&
          !item["offset"].as_double(&payload.layerOffset._offset)) {
        if (err) *err = "Payload offset must be a number";
        return false;
      }
      if (item.contains("scale") &&
          !item["scale"].as_double(&payload.layerOffset._scale)) {
        if (err) *err = "Payload scale must be a number";
        return false;
      }
      payloads.push_back(std::move(payload));
    }
    out->emplace_back(qual, std::move(payloads));
  }
  return true;
}

static bool JSONToPrimSpecImpl(const json &j, PrimSpec *ps, std::string *warn, std::string *err) {
  (void)warn;
  if (!ps || !j.is_object()) {
    if (err) *err = "PrimSpec must be an object";
    return false;
  }
  if (j.contains("name") && j["name"].is_string()) ps->name() = j["name"].get_string();
  if (j.contains("typeName") && j["typeName"].is_string()) ps->typeName() = j["typeName"].get_string();
  if (j.contains("specifier") && j["specifier"].is_string()) {
    const std::string spec = j["specifier"].get_string();
    if (spec == "def") ps->specifier() = Specifier::Def;
    else if (spec == "over") ps->specifier() = Specifier::Over;
    else if (spec == "class") ps->specifier() = Specifier::Class;
    else {
      if (err) *err = "Unknown PrimSpec specifier: " + spec;
      return false;
    }
  }
  if (j.contains("inherits")) {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> ops;
    if (!ParsePathListOps(j["inherits"], &ops, err)) return false;
    ps->metas().inherits = std::move(ops);
  }
  if (j.contains("specializes")) {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> ops;
    if (!ParsePathListOps(j["specializes"], &ops, err)) return false;
    ps->metas().specializes = std::move(ops);
  }
  if (j.contains("references")) {
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> ops;
    if (!ParseReferenceListOps(j["references"], &ops, err)) return false;
    ps->metas().references = std::move(ops);
  }
  if (j.contains("payloads")) {
    std::vector<std::pair<ListEditQual, std::vector<Payload>>> ops;
    if (!ParsePayloadListOps(j["payloads"], &ops, err)) return false;
    ps->metas().payload = std::move(ops);
  }
  if (j.contains("variants")) {
    if (!j["variants"].is_object()) {
      if (err) *err = "Prim variants must be an object";
      return false;
    }
    VariantSelectionMap selections;
    for (const auto &item : *j["variants"].object_items()) {
      if (!item.value().is_string()) {
        if (err) *err = "Variant selection must be a string";
        return false;
      }
      selections[item.key] = item.value().get_string();
    }
    ps->metas().variants = std::move(selections);
  }
  if (j.contains("properties") && j["properties"].is_object()) {
    for (const auto &member : *j["properties"].object_items()) {
      const json &prop_json = member.value();
      Property property;
      bool ok = false;
      if (prop_json.contains("propertyType") && prop_json["propertyType"].is_string() &&
          (prop_json["propertyType"].get_string() == "relationship" ||
           prop_json["propertyType"].get_string() == "noTargetsRelationship")) {
        ok = ParseRelationshipJSON(prop_json.contains("relationship") ? prop_json["relationship"] : prop_json,
                                   member.key, &property, err);
      } else if (prop_json.contains("attribute")) {
        ok = ParseAttributeJSON(prop_json["attribute"], member.key, &property, err);
      } else if (prop_json.contains("typeName")) {
        ok = ParseAttributeJSON(prop_json, member.key, &property, err);
      }
      if (!ok) return false;
      ps->props()[member.key] = std::move(property);
    }
  }
  if (j.contains("children") && j["children"].is_object()) {
    for (const auto &member : *j["children"].object_items()) {
      PrimSpec child;
      if (!JSONToPrimSpecImpl(member.value(), &child, warn, err)) return false;
      if (child.name().empty()) child.name() = member.key;
      ps->children().push_back(std::move(child));
    }
  }
  return true;
}

bool JSONToPrimSpec(const std::string &j_str, PrimSpec *ps, std::string *warn, std::string *err) {
  json j;
  minijson::Error parse_err;
  minijson::ParseOptions parse_options;
  parse_options.max_depth = kJSONMaxParseDepth;
  if (!minijson::Parse(j_str, &j, &parse_err, parse_options)) {
    if (err) {
      (*err) = "Failed to parse string as JSON: " + parse_err.message + "\n";
    }
    return false;
  }

  return JSONToPrimSpecImpl(j, ps, warn, err);
}

bool JSONToStage(const std::string &json_string, Stage *dst_stage,
                 std::string *warn, std::string *err,
                 const JSONToUSDOptions &options) {
  if (!dst_stage) {
    if (err) *err = "Internal error: stage is null";
    return false;
  }
  json j;
  if (!ParseStageJSON(json_string, &j, err)) return false;

  Stage stage;
  JSONToUSDContext context;
  context.options = &options;
  if (j.contains("buffers") && !context.ParseBuffers(j["buffers"], err)) {
    return false;
  }
  if (j.contains("bufferViews") &&
      !context.ParseBufferViews(j["bufferViews"], err)) {
    return false;
  }
  if (j.contains("accessors") &&
      !context.ParseAccessors(j["accessors"], err)) {
    return false;
  }
  const json *stage_properties = j.find("properties");
  if (stage_properties && stage_properties->is_object()) {
    if (const json *up_axis = stage_properties->find("upAxis")) {
      if (up_axis->is_string()) {
        const std::string axis = up_axis->get_string();
        if (axis == "X") stage.metas().upAxis = Axis::X;
        else if (axis == "Y") stage.metas().upAxis = Axis::Y;
        else if (axis == "Z") stage.metas().upAxis = Axis::Z;
        else {
          if (err) *err = "Unknown stage upAxis value: " + axis;
          return false;
        }
      }
    }
    if (const json *comment = stage_properties->find("comment")) {
      if (comment->is_string()) stage.metas().comment = comment->get_string();
    }
  }

  const json *children = j.find("primChildren");
  if (!children || !children->is_object()) {
    if (err) *err = "Stage JSON is missing object primChildren";
    return false;
  }
  for (const auto &member : *children->object_items()) {
    Prim prim;
    if (!JSONToRuntimePrim(member.value(), member.key, &prim, &context, warn,
                           err)) {
      return false;
    }
    if (!stage.add_root_prim(std::move(prim), false)) {
      if (err) *err = "Failed to add JSON root prim: " + member.key;
      return false;
    }
  }
  *dst_stage = std::move(stage);
  return true;
}

bool JSONToStage(const std::string &json_string, Stage *dst_stage,
                 std::string *warn, std::string *err) {
  return JSONToStage(json_string, dst_stage, warn, err, JSONToUSDOptions());
}

bool JSONToStage(const std::string &json_string, Prim *dst_prim,
                 std::string *warn, std::string *err,
                 const JSONToUSDOptions &options) {
  if (!dst_prim) {
    if (err) *err = "Internal error: prim is null";
    return false;
  }
  json j;
  if (!ParseStageJSON(json_string, &j, err)) return false;
  JSONToUSDContext context;
  context.options = &options;
  if (j.contains("buffers") && !context.ParseBuffers(j["buffers"], err)) {
    return false;
  }
  if (j.contains("bufferViews") &&
      !context.ParseBufferViews(j["bufferViews"], err)) {
    return false;
  }
  if (j.contains("accessors") &&
      !context.ParseAccessors(j["accessors"], err)) {
    return false;
  }
  const json *children = j.find("primChildren");
  if (children && children->is_object() && children->size() == 1) {
    const auto &member = (*children->object_items())[0];
    return JSONToRuntimePrim(member.value(), member.key, dst_prim, &context,
                             warn, err);
  }
  if (j.contains("typeName")) {
    const std::string name = j.contains("name") && j["name"].is_string()
                                 ? j["name"].get_string()
                                 : std::string();
    return JSONToRuntimePrim(j, name, dst_prim, &context, warn, err);
  }
  if (err) *err = "Prim JSON must contain typeName or exactly one primChildren entry";
  return false;
}

bool JSONToStage(const std::string &json_string, Prim *dst_prim,
                 std::string *warn, std::string *err) {
  return JSONToStage(json_string, dst_prim, warn, err, JSONToUSDOptions());
}

bool JSONToLayer(const std::string &j_str, Layer *dst_layer, std::string *warn, std::string *err) {
  if (!dst_layer) {
    if (err) {
      (*err) = "Internal error.";
    }
    return false;
  }

  json j;
  minijson::Error parse_err;
  minijson::ParseOptions parse_options;
  parse_options.max_depth = kJSONMaxParseDepth;
  if (!minijson::Parse(j_str, &j, &parse_err, parse_options)) {
    if (err) {
      (*err) = "Failed to parse string as JSON: " + parse_err.message;
    }
    return false;
  }

  // Create context for buffer parsing if needed
  JSONToUSDContext context;

  // Parse buffer data if present
  if (j.contains("buffers")) {
    if (!context.ParseBuffers(j["buffers"], err)) {
      return false;
    }
  }

  if (j.contains("bufferViews")) {
    if (!context.ParseBufferViews(j["bufferViews"], err)) {
      return false;
    }
  }

  if (j.contains("accessors")) {
    if (!context.ParseAccessors(j["accessors"], err)) {
      return false;
    }
  }

  Layer layer;

  // Set name if present
  if (j.contains("name") && j["name"].is_string()) {
    layer.set_name(j["name"].get_string());
  }

  // Parse layer metadata
  if (j.contains("metas")) {
    json metas = j["metas"];

    if (metas.contains("upAxis") && metas["upAxis"].is_string()) {
      std::string s = metas["upAxis"].get_string();
      if (s == "X") {
        layer.metas().upAxis = lightusd::Axis::X;
      } else if (s == "Y") {
        layer.metas().upAxis = lightusd::Axis::Y;
      } else if (s == "Z") {
        layer.metas().upAxis = lightusd::Axis::Z;
      } else {
        if (err) {
          (*err) = "Unknown upAxis value: " + s;
        }
        return false;
      }
    }

    if (metas.contains("defaultPrim") && metas["defaultPrim"].is_string()) {
      layer.metas().defaultPrim = value::token(metas["defaultPrim"].get_string());
    }

    if (metas.contains("metersPerUnit") && metas["metersPerUnit"].is_number()) {
      layer.metas().metersPerUnit = metas["metersPerUnit"].get_double();
    }

    if (metas.contains("timeCodesPerSecond") && metas["timeCodesPerSecond"].is_number()) {
      layer.metas().timeCodesPerSecond = metas["timeCodesPerSecond"].get_double();
    }

    if (metas.contains("framesPerSecond") && metas["framesPerSecond"].is_number()) {
      layer.metas().framesPerSecond = metas["framesPerSecond"].get_double();
    }

    if (metas.contains("startTimeCode") && metas["startTimeCode"].is_number()) {
      layer.metas().startTimeCode = metas["startTimeCode"].get_double();
    }

    if (metas.contains("endTimeCode") && metas["endTimeCode"].is_number()) {
      layer.metas().endTimeCode = metas["endTimeCode"].get_double();
    }

    if (metas.contains("kilogramsPerUnit") && metas["kilogramsPerUnit"].is_number()) {
      layer.metas().kilogramsPerUnit = metas["kilogramsPerUnit"].get_double();
    }

    if (metas.contains("doc") && metas["doc"].is_string()) {
      layer.metas().doc = metas["doc"].get_string();
    }

    if (metas.contains("comment") && metas["comment"].is_string()) {
      layer.metas().comment = metas["comment"].get_string();
    }

    if (metas.contains("autoPlay") && metas["autoPlay"].is_boolean()) {
      layer.metas().autoPlay = metas["autoPlay"].get_bool();
    }

    if (metas.contains("playbackMode") && metas["playbackMode"].is_string()) {
      std::string mode = metas["playbackMode"].get_string();
      if (mode == "loop") {
        layer.metas().playbackMode = LayerMetas::PlaybackMode::PlaybackModeLoop;
      } else {
        layer.metas().playbackMode = LayerMetas::PlaybackMode::PlaybackModeNone;
      }
    }

    if (metas.contains("primChildren") && metas["primChildren"].is_array()) {
      std::vector<std::string> children_strs;
      for (const auto& child : metas["primChildren"]) {
        if (child.is_string()) {
          children_strs.push_back(child.get_string());
        }
      }
      std::vector<value::token> children_tokens;
      for (const auto& child_str : children_strs) {
        children_tokens.emplace_back(child_str);
      }
      layer.metas().primChildren = children_tokens;
    }

    if (metas.contains("subLayers") && metas["subLayers"].is_array()) {
      for (const auto& subLayer : metas["subLayers"]) {
        if (subLayer.is_object() && subLayer.contains("assetPath") && subLayer["assetPath"].is_string()) {
          SubLayer sub;
          sub.assetPath = value::AssetPath(subLayer["assetPath"].get_string());
          if (subLayer.contains("layerOffset") && subLayer["layerOffset"].is_object()) {
            const json &offset = subLayer["layerOffset"];
            if (offset.contains("offset") && offset["offset"].is_number()) {
              sub.layerOffset._offset = offset["offset"].get_double();
            }
            if (offset.contains("scale") && offset["scale"].is_number()) {
              sub.layerOffset._scale = offset["scale"].get_double();
            }
          }
          layer.metas().subLayers.push_back(sub);
        }
      }
    }

    if (metas.contains("customLayerData")) {
      Dictionary custom_data;
      if (!detail::JSONToMetadataDictionary(metas["customLayerData"],
                                            &custom_data, err)) {
        return false;
      }
      layer.metas().customLayerData = std::move(custom_data);
      layer.metas().customLayerDataAuthored = true;
    }
    if (metas.contains("colorConfiguration") &&
        metas["colorConfiguration"].is_string()) {
      layer.metas().colorConfiguration = value::AssetPath(
          metas["colorConfiguration"].get_string());
    }
    if (metas.contains("colorManagementSystem") &&
        metas["colorManagementSystem"].is_string()) {
      layer.metas().colorManagementSystem = value::token(
          metas["colorManagementSystem"].get_string());
    }
    if (metas.contains("renderSettingsPrimPath") &&
        metas["renderSettingsPrimPath"].is_string()) {
      Path path;
      if (!ParsePathValue(metas["renderSettingsPrimPath"], &path, err)) return false;
      layer.metas().renderSettingsPrimPath = path.full_path_name();
    }
    if (metas.contains("owner") && metas["owner"].is_string()) {
      layer.metas().owner = metas["owner"].get_string();
    }
    if (metas.contains("hasOwnedSubLayers") &&
        metas["hasOwnedSubLayers"].is_boolean()) {
      layer.metas().hasOwnedSubLayers = metas["hasOwnedSubLayers"].get_bool();
    }
    if (metas.contains("expressionVariables")) {
      Dictionary variables;
      if (!detail::JSONToMetadataDictionary(metas["expressionVariables"],
                                            &variables, err)) return false;
      layer.metas().expressionVariables = std::move(variables);
    }
    if (metas.contains("layerRelocates")) {
      if (!metas["layerRelocates"].is_array()) {
        if (err) *err = "layerRelocates must be an array";
        return false;
      }
      for (const auto &item : metas["layerRelocates"]) {
        if (!item.is_object() || !item.contains("source") ||
            !item.contains("target")) {
          if (err) *err = "Invalid layer relocation";
          return false;
        }
        Path source, target;
        if (!ParsePathValue(item["source"], &source, err) ||
            !ParsePathValue(item["target"], &target, err)) return false;
        layer.metas().layerRelocates.emplace_back(std::move(source),
                                                  std::move(target));
      }
    }
    if (metas.contains("unregisteredMetas")) {
      if (!metas["unregisteredMetas"].is_object()) {
        if (err) *err = "unregisteredMetas must be an object";
        return false;
      }
      for (const auto &item : *metas["unregisteredMetas"].object_items()) {
        if (!item.value().is_string()) {
          if (err) *err = "Unregistered layer metadata must be a string";
          return false;
        }
        layer.metas().unregisteredMetas[item.key] = item.value().get_string();
      }
    }
  }

  // Parse primSpecs
  if (j.contains("primSpecs") && j["primSpecs"].is_object()) {
    const auto *prim_specs = j["primSpecs"].object_items();
    for (const auto &member : *prim_specs) {
      if (!member.value().is_object()) {
        if (err) *err = "PrimSpec entry must be an object: " + member.key;
        return false;
      }
      PrimSpec primspec;
      if (!JSONToPrimSpecImpl(member.value(), &primspec, warn, err)) return false;
      if (primspec.name().empty()) primspec.name() = member.key;
      layer.primspecs()[member.key] = std::move(primspec);
    }
  }

  (*dst_layer) = std::move(layer);

  return true;
}

bool JSONToGeomMesh(const std::string &j_str, GeomMesh *mesh, std::string *warn,
                    std::string *err, const JSONToUSDOptions &options) {
  if (!mesh) {
    if (err) {
      (*err) = "Internal error: mesh is null";
    }
    return false;
  }

  json j;
  minijson::Error parse_err;
  minijson::ParseOptions parse_options;
  parse_options.max_depth = kJSONMaxParseDepth;
  if (!minijson::Parse(j_str, &j, &parse_err, parse_options)) {
    if (err) {
      (*err) = "Failed to parse string as JSON: " + parse_err.message;
    }
    return false;
  }

  JSONToUSDContext context;
  context.options = &options;
  if (j.contains("buffers") && !context.ParseBuffers(j["buffers"], err)) {
    return false;
  }
  if (j.contains("bufferViews") &&
      !context.ParseBufferViews(j["bufferViews"], err)) {
    return false;
  }
  if (j.contains("accessors") && !context.ParseAccessors(j["accessors"], err)) {
    return false;
  }
  return detail::JSONToGeomMesh(j, mesh, &context, warn, err);
}

bool JSONToGeomMesh(const std::string &j_str, GeomMesh *mesh,
                    std::string *warn, std::string *err) {
  return JSONToGeomMesh(j_str, mesh, warn, err, JSONToUSDOptions());
}

} // namespace lightusd
