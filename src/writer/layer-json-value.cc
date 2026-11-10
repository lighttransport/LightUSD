// SPDX-License-Identifier: Apache-2.0
#include "writer/layer-json-internal.hh"
#include "types/value.hh"
#include <cmath>
namespace tn = lightusd;
namespace lightusd { namespace layer_json_detail {
namespace {
minijson::Value JSONNumber(double value) {
  // nlohmann encoded non-finite USD numeric values as JSON null. Keep that
  // web contract while the shared MiniJSON serializer remains strict.
  return std::isfinite(value) ? minijson::Value(value) : minijson::Value();
}
}
lightusd::minijson::Value ValueJSON(tn::ValueView value) {
  if (const bool* v = value.as_bool()) return *v;
  if (const int32_t* v = value.as_int()) return *v;
  if (const uint32_t* v = value.as_uint()) return *v;
  if (const int64_t* v = value.as_int64()) return *v;
  if (const uint64_t* v = value.as_uint64()) return *v;
  if (const uint8_t* v = value.as_uchar()) return *v;
  if (const float* v = value.as_float()) return JSONNumber(*v);
  if (const double* v = value.as_double()) return JSONNumber(*v);
  if (auto v = value.as_string()) return std::string(*v);
  if (auto v = value.as_token()) return std::string(*v);
  if (auto v = value.as_asset_path()) return std::string(*v);
  if (auto dictionary = value.as_dictionary()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::object();
    for (const auto& entry : dictionary->entries())
      out[entry.first] = ValueJSON(entry.second);
    return out;
  }
  if (auto v = value.as_bool_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (uint8_t element : *v) out.push_back(element != 0);
    return out;
  }
  if (auto v = value.as_float_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(JSONNumber(element));
    return out;
  }
  if (auto v = value.as_double_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(JSONNumber(element));
    return out;
  }
  if (auto v = value.as_int_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (auto v = value.as_int64_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (auto v = value.as_uint_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (auto v = value.as_uint64_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (auto v = value.as_token_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(std::string(element));
    return out;
  }

  auto float_array = [](const float* ptr, size_t count) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    for (size_t i = 0; i < count; ++i) out.push_back(JSONNumber(ptr[i]));
    return out;
  };
  auto double_array = [](const double* ptr, size_t count) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    for (size_t i = 0; i < count; ++i) out.push_back(JSONNumber(ptr[i]));
    return out;
  };
  switch (value.type_id()) {
    case tn::TypeId::Float2:
    case tn::TypeId::Texcoord2f:
      return float_array(static_cast<const float*>(value.raw_data()), 2);
    case tn::TypeId::Float3:
    case tn::TypeId::Point3f:
    case tn::TypeId::Vector3f:
    case tn::TypeId::Normal3f:
    case tn::TypeId::Color3f:
      return float_array(static_cast<const float*>(value.raw_data()), 3);
    case tn::TypeId::Float4:
    case tn::TypeId::Color4f:
    case tn::TypeId::Quatf:
      return float_array(static_cast<const float*>(value.raw_data()), 4);
    case tn::TypeId::Double2:
    case tn::TypeId::Texcoord2d:
      return double_array(static_cast<const double*>(value.raw_data()), 2);
    case tn::TypeId::Double3:
    case tn::TypeId::Point3d:
    case tn::TypeId::Vector3d:
    case tn::TypeId::Normal3d:
    case tn::TypeId::Color3d:
      return double_array(static_cast<const double*>(value.raw_data()), 3);
    case tn::TypeId::Double4:
    case tn::TypeId::Color4d:
    case tn::TypeId::Quatd:
      return double_array(static_cast<const double*>(value.raw_data()), 4);
    case tn::TypeId::Matrix4f:
      return float_array(value.as_matrix4f(), 16);
    case tn::TypeId::Matrix4d:
    case tn::TypeId::Frame4d:  // matrix4d role
      return double_array(value.as_matrix4d(), 16);
    default:
      break;
  }
  return nullptr;
}

} }
