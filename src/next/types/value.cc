// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value class implementation

#include "value.hh"
#include "array-storage.hh"
#include "type-info.hh"
#include "interpolation.hh"
#include "../crate/lazy-array.hh"
#include "../crate/crate-data-source.hh"

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>

namespace lightusd {
namespace next {

namespace {

// ============================================================
// Type storage helpers
// ============================================================

// String-like types stored in SBO using std::string
struct StringStorage {
  std::string value;
};

using detail::ArrayHandle;
inline ArrayHandle* ArraySlot(char* s) {
  return reinterpret_cast<ArrayHandle*>(s);
}
inline const ArrayHandle* ArraySlot(const char* s) {
  return reinterpret_cast<const ArrayHandle*>(s);
}
inline void DetachArray(char* s) { ArraySlot(s)->detach(); }

// Dictionary values are held by a shared_ptr<Dict> in the SBO slot — the same
// copy-on-write model as arrays (cheap copy during composition; detach on first
// mutation). A nested dict is itself just another shared_ptr handle, so the
// recursive structure never threatens the SBO size.
using DictHandle = std::shared_ptr<Dict>;
inline DictHandle* DictSlot(char* s) {
  return reinterpret_cast<DictHandle*>(s);
}
inline const DictHandle* DictSlot(const char* s) {
  return reinterpret_cast<const DictHandle*>(s);
}
inline void DetachDict(char* s) {
  DictHandle& h = *DictSlot(s);
  if (h.use_count() > 1) h = std::make_shared<Dict>(*h);
}

// Check if type uses string storage
bool UsesStringStorage(TypeId id) {
  return id == TypeId::String || id == TypeId::Token ||
         id == TypeId::AssetPath || id == TypeId::PathExpression;
}

// Array element types stored as flat float components:
// scalar floats plus all float vector/quat/matrix/color types.
bool IsFloatBackedArray(TypeId id) {
  switch (id) {
    case TypeId::Float:
    case TypeId::Float2:
    case TypeId::Float3:
    case TypeId::Float4:
    case TypeId::Point3f:
    case TypeId::Vector3f:
    case TypeId::Normal3f:
    case TypeId::Color3f:
    case TypeId::Color4f:
    case TypeId::Texcoord2f:
    case TypeId::Texcoord3f:
    case TypeId::Quatf:
    case TypeId::Matrix2f:
    case TypeId::Matrix3f:
    case TypeId::Matrix4f:
    // Half element types materialize into a float buffer (no 16-bit storage).
    case TypeId::Half:
    case TypeId::Half2:
    case TypeId::Half3:
    case TypeId::Half4:
    case TypeId::Quath:
    case TypeId::Point3h:
    case TypeId::Vector3h:
    case TypeId::Normal3h:
    case TypeId::Color3h:
    case TypeId::Color4h:
    case TypeId::Texcoord2h:
    case TypeId::Texcoord3h:
      return true;
    default:
      return false;
  }
}

// Double-backed array types.
// Int-vector element types share the flat int32 array storage with Int.
bool IsIntBackedArray(TypeId id) {
  switch (id) {
    case TypeId::Int:
    case TypeId::Int2:
    case TypeId::Int3:
    case TypeId::Int4:
      return true;
    default:
      return false;
  }
}

// UInt-vector element types share the flat uint32 array storage with UInt.
// uchar[] is widened into the same storage (bit-exact; the crate writer
// narrows back to tightly-packed uint8).
bool IsUIntBackedArray(TypeId id) {
  switch (id) {
    case TypeId::UInt:
    case TypeId::UInt2:
    case TypeId::UInt3:
    case TypeId::UInt4:
    case TypeId::UChar:
      return true;
    default:
      return false;
  }
}

bool IsDoubleBackedArray(TypeId id) {
  switch (id) {
    case TypeId::Double:
    case TypeId::TimeCode:
    case TypeId::Double2:
    case TypeId::Double3:
    case TypeId::Double4:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
    case TypeId::Color4d:
    case TypeId::Texcoord2d:
    case TypeId::Texcoord3d:
    case TypeId::Frame4d:
    case TypeId::Quatd:
    case TypeId::Matrix2d:
    case TypeId::Matrix3d:
    case TypeId::Matrix4d:
      return true;
    default:
      return false;
  }
}

bool FitsValueArraySize(size_t count) {
  return count <=
         static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
}

bool IsValidComponentArray(size_t scalar_count, TypeId elem_type,
                           uint32_t comps_per_elem, bool storage_matches) {
  if (!storage_matches || comps_per_elem == 0 ||
      GetComponentCount(elem_type) != comps_per_elem ||
      scalar_count % comps_per_elem != 0) {
    return false;
  }
  return FitsValueArraySize(scalar_count / comps_per_elem);
}

size_t ComponentArraySize(size_t scalar_count, size_t components) {
  if (components == 0 || scalar_count % components != 0) return 0;
  return scalar_count / components;
}

bool IsValueArrayType(TypeId id) {
  return IsFloatBackedArray(id) || IsDoubleBackedArray(id) ||
         IsIntBackedArray(id) || IsUIntBackedArray(id) ||
         id == TypeId::Int64 || id == TypeId::UInt64 || id == TypeId::Bool ||
         UsesStringStorage(id);
}

bool FinalizeLazyDecode(const LazyArrayRef& ref, Value* decoded) {
  if (!decoded || decoded->is_lazy() || !decoded->is_array()) return false;
  if (decoded->array_size() != ref.element_count) return false;
  decoded->retag_role(ref.value_type);
  return decoded->type_id() == ref.value_type;
}

}  // anonymous namespace

// ============================================================
// Constructors and destructor
// ============================================================

Value::Value() = default;

Value::~Value() {
  destroy();
}

Value::Value(const Value& other) {
  copy_from(other);
}

Value::Value(Value&& other) noexcept {
  move_from(std::move(other));
}

Value& Value::operator=(const Value& other) {
  if (this != &other) {
    destroy();
    copy_from(other);
  }
  return *this;
}

Value& Value::operator=(Value&& other) noexcept {
  if (this != &other) {
    destroy();
    move_from(std::move(other));
  }
  return *this;
}

// ============================================================
// Type-specific constructors
// ============================================================

Value::Value(bool v) : type_id_(TypeId::Bool) {
  std::memcpy(storage_, &v, sizeof(bool));
}

Value::Value(int32_t v) : type_id_(TypeId::Int) {
  std::memcpy(storage_, &v, sizeof(int32_t));
}

Value::Value(uint32_t v) : type_id_(TypeId::UInt) {
  std::memcpy(storage_, &v, sizeof(uint32_t));
}

Value::Value(int64_t v) : type_id_(TypeId::Int64) {
  std::memcpy(storage_, &v, sizeof(int64_t));
}

Value::Value(uint64_t v) : type_id_(TypeId::UInt64) {
  std::memcpy(storage_, &v, sizeof(uint64_t));
}

Value::Value(float v) : type_id_(TypeId::Float) {
  std::memcpy(storage_, &v, sizeof(float));
}

Value::Value(double v) : type_id_(TypeId::Double) {
  std::memcpy(storage_, &v, sizeof(double));
}

Value::Value(const char* v) : type_id_(TypeId::String) {
  new (storage_) StringStorage{std::string(v ? v : "")};
}

Value::Value(const std::string& v) : type_id_(TypeId::String) {
  new (storage_) StringStorage{v};
}

Value::Value(std::string&& v) : type_id_(TypeId::String) {
  new (storage_) StringStorage{std::move(v)};
}

// ============================================================
// Factory functions
// ============================================================

Value Value::MakeInt2(int32_t x, int32_t y) {
  Value v;
  v.type_id_ = TypeId::Int2;
  int32_t data[2] = {x, y};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeInt3(int32_t x, int32_t y, int32_t z) {
  Value v;
  v.type_id_ = TypeId::Int3;
  int32_t data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeInt4(int32_t x, int32_t y, int32_t z, int32_t w) {
  Value v;
  v.type_id_ = TypeId::Int4;
  int32_t data[4] = {x, y, z, w};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeFloat2(float x, float y) {
  Value v;
  v.type_id_ = TypeId::Float2;
  float data[2] = {x, y};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeFloat3(float x, float y, float z) {
  Value v;
  v.type_id_ = TypeId::Float3;
  float data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeFloat4(float x, float y, float z, float w) {
  Value v;
  v.type_id_ = TypeId::Float4;
  float data[4] = {x, y, z, w};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeDouble2(double x, double y) {
  Value v;
  v.type_id_ = TypeId::Double2;
  double data[2] = {x, y};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeDouble3(double x, double y, double z) {
  Value v;
  v.type_id_ = TypeId::Double3;
  double data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeDouble4(double x, double y, double z, double w) {
  Value v;
  v.type_id_ = TypeId::Double4;
  double data[4] = {x, y, z, w};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeQuatf(float x, float y, float z, float w) {
  Value v;
  v.type_id_ = TypeId::Quatf;
  float data[4] = {x, y, z, w};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeQuatd(double x, double y, double z, double w) {
  Value v;
  v.type_id_ = TypeId::Quatd;
  double data[4] = {x, y, z, w};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeMatrix2f(const float* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix2f;
  std::memcpy(v.storage_, data, 4 * sizeof(float));
  return v;
}

Value Value::MakeMatrix3f(const float* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix3f;
  std::memcpy(v.storage_, data, 9 * sizeof(float));
  return v;
}

Value Value::MakeMatrix4f(const float* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix4f;
  std::memcpy(v.storage_, data, 16 * sizeof(float));
  return v;
}

Value Value::MakeMatrix2d(const double* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix2d;
  std::memcpy(v.storage_, data, 4 * sizeof(double));
  return v;
}

Value Value::MakeMatrix3d(const double* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix3d;
  std::memcpy(v.storage_, data, 9 * sizeof(double));
  return v;
}

Value Value::MakeMatrix4d(const double* data) {
  Value v;
  if (!data) return v;
  v.type_id_ = TypeId::Matrix4d;
  std::memcpy(v.storage_, data, 16 * sizeof(double));
  return v;
}

Value Value::MakeStringLike(const std::string& s, TypeId type) {
  Value v;
  if (!UsesStringStorage(type)) return v;
  v.type_id_ = type;
  new (v.storage_) StringStorage{s};
  return v;
}

Value Value::MakeToken(const std::string& s) {
  Value v;
  v.type_id_ = TypeId::Token;
  new (v.storage_) StringStorage{s};
  return v;
}

Value Value::MakeToken(std::string&& s) {
  Value v;
  v.type_id_ = TypeId::Token;
  new (v.storage_) StringStorage{std::move(s)};
  return v;
}

Value Value::MakeAssetPath(const std::string& s) {
  Value v;
  v.type_id_ = TypeId::AssetPath;
  new (v.storage_) StringStorage{s};
  return v;
}

Value Value::MakeAssetPath(std::string&& s) {
  Value v;
  v.type_id_ = TypeId::AssetPath;
  new (v.storage_) StringStorage{std::move(s)};
  return v;
}

Value Value::MakeDictionary() {
  Value v;
  v.type_id_ = TypeId::Dictionary;
  new (v.storage_) DictHandle(std::make_shared<Dict>());
  return v;
}

Value Value::MakeDictionary(Dict&& d) {
  Value v;
  v.type_id_ = TypeId::Dictionary;
  new (v.storage_) DictHandle(std::make_shared<Dict>(std::move(d)));
  return v;
}

Value Value::MakePoint3f(float x, float y, float z) {
  Value v;
  v.type_id_ = TypeId::Point3f;
  float data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakePoint3d(double x, double y, double z) {
  Value v;
  v.type_id_ = TypeId::Point3d;
  double data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeVector3f(float x, float y, float z) {
  Value v;
  v.type_id_ = TypeId::Vector3f;
  float data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeVector3d(double x, double y, double z) {
  Value v;
  v.type_id_ = TypeId::Vector3d;
  double data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeNormal3f(float x, float y, float z) {
  Value v;
  v.type_id_ = TypeId::Normal3f;
  float data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeNormal3d(double x, double y, double z) {
  Value v;
  v.type_id_ = TypeId::Normal3d;
  double data[3] = {x, y, z};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeColor3f(float r, float g, float b) {
  Value v;
  v.type_id_ = TypeId::Color3f;
  float data[3] = {r, g, b};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeColor4f(float r, float g, float b, float a) {
  Value v;
  v.type_id_ = TypeId::Color4f;
  float data[4] = {r, g, b, a};
  std::memcpy(v.storage_, data, sizeof(data));
  return v;
}

Value Value::MakeTexcoord2f(float u, float v_coord) {
  Value val;
  val.type_id_ = TypeId::Texcoord2f;
  float data[2] = {u, v_coord};
  std::memcpy(val.storage_, data, sizeof(data));
  return val;
}

Value Value::MakeFromRaw(TypeId type_id, const void* data) {
  Value v;
  if (!data || type_id == TypeId::Invalid || type_id == TypeId::Dictionary) {
    return v;
  }

  if (UsesStringStorage(type_id)) {
    v.type_id_ = type_id;
    new (v.storage_) StringStorage{std::string(static_cast<const char*>(data))};
  } else {
    const size_t size = GetTypeSize(type_id);
    if (size == 0 || size > kSBOSize) return v;
    v.type_id_ = type_id;
    std::memcpy(v.storage_, data, size);
  }
  return v;
}

// ============================================================
// Array factory functions
// ============================================================

Value Value::MakeFloatArray(const std::vector<float>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v;
  v.type_id_ = TypeId::Float;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data);
  return v;
}

Value Value::MakeFloatArray(std::vector<float>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v;
  v.type_id_ = TypeId::Float;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeIntArray(const std::vector<int32_t>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v;
  v.type_id_ = TypeId::Int;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data);
  return v;
}

Value Value::MakeIntArray(std::vector<int32_t>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v;
  v.type_id_ = TypeId::Int;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeFloat2Array(const std::vector<float>& data) {
  if (!IsValidComponentArray(data.size(), TypeId::Float2, 2, true)) {
    return Value();
  }
  Value v;
  v.type_id_ = TypeId::Float2;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size() / 2);
  new (v.storage_) ArrayHandle(data);
  return v;
}

Value Value::MakeFloat2Array(std::vector<float>&& data) {
  if (!IsValidComponentArray(data.size(), TypeId::Float2, 2, true)) {
    return Value();
  }
  Value v;
  v.type_id_ = TypeId::Float2;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size() / 2);
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeFloat3Array(const std::vector<float>& data) {
  if (!IsValidComponentArray(data.size(), TypeId::Float3, 3, true)) {
    return Value();
  }
  Value v;
  v.type_id_ = TypeId::Float3;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size() / 3);
  new (v.storage_) ArrayHandle(data);
  return v;
}

Value Value::MakeFloat3Array(std::vector<float>&& data) {
  if (!IsValidComponentArray(data.size(), TypeId::Float3, 3, true)) {
    return Value();
  }
  Value v;
  v.type_id_ = TypeId::Float3;
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size() / 3);
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

// New array types
Value Value::MakeDoubleArray(const std::vector<double>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Double; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data); return v;
}
Value Value::MakeDoubleArray(std::vector<double>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Double; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data)); return v;
}
Value Value::MakeInt64Array(const std::vector<int64_t>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Int64; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data); return v;
}
Value Value::MakeInt64Array(std::vector<int64_t>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Int64; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data)); return v;
}
Value Value::MakeUIntArray(const std::vector<uint32_t>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::UInt; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data); return v;
}
Value Value::MakeUIntArray(std::vector<uint32_t>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::UInt; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data)); return v;
}
Value Value::MakeUInt64Array(const std::vector<uint64_t>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::UInt64; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data); return v;
}
Value Value::MakeUInt64Array(std::vector<uint64_t>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::UInt64; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data)); return v;
}
Value Value::MakeBoolArray(const std::vector<bool>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Bool; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  std::vector<uint8_t> tmp(data.size());
  for (size_t i = 0; i < data.size(); i++) tmp[i] = data[i] ? 1 : 0;
  new (v.storage_) ArrayHandle(std::move(tmp)); return v;
}
Value Value::MakeTokenArray(const std::vector<std::string>& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Token; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(data); return v;
}
Value Value::MakeTokenArray(std::vector<std::string>&& data) {
  if (!FitsValueArraySize(data.size())) return Value();
  Value v; v.type_id_ = TypeId::Token; v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data)); return v;
}

Value Value::MakeFloatCompArray(std::vector<float>&& data, TypeId elem_type,
                                uint32_t comps_per_elem) {
  if (!IsValidComponentArray(data.size(), elem_type, comps_per_elem,
                             IsFloatBackedArray(elem_type))) {
    return Value();
  }
  Value v;
  v.type_id_ = elem_type;
  v.is_array_ = true;
  v.array_size_ = comps_per_elem
                      ? static_cast<uint32_t>(data.size() / comps_per_elem)
                      : 0;
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeDoubleCompArray(std::vector<double>&& data, TypeId elem_type,
                                 uint32_t comps_per_elem) {
  if (!IsValidComponentArray(data.size(), elem_type, comps_per_elem,
                             IsDoubleBackedArray(elem_type))) {
    return Value();
  }
  Value v;
  v.type_id_ = elem_type;
  v.is_array_ = true;
  v.array_size_ = comps_per_elem
                      ? static_cast<uint32_t>(data.size() / comps_per_elem)
                      : 0;
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeUIntCompArray(std::vector<uint32_t>&& data, TypeId elem_type,
                               uint32_t comps_per_elem) {
  if (!IsValidComponentArray(data.size(), elem_type, comps_per_elem,
                             IsUIntBackedArray(elem_type))) {
    return Value();
  }
  Value v;
  v.type_id_ = elem_type;
  v.is_array_ = true;
  v.array_size_ = comps_per_elem
                      ? static_cast<uint32_t>(data.size() / comps_per_elem)
                      : 0;
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeStringLikeArray(std::vector<std::string>&& data,
                                 TypeId elem_type) {
  if (!UsesStringStorage(elem_type) || !FitsValueArraySize(data.size())) {
    return Value();
  }
  Value v;
  v.type_id_ = elem_type;  // Token, String or AssetPath
  v.is_array_ = true;
  v.array_size_ = static_cast<uint32_t>(data.size());
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

Value Value::MakeIntCompArray(std::vector<int32_t>&& data, TypeId elem_type,
                              uint32_t comps_per_elem) {
  if (!IsValidComponentArray(data.size(), elem_type, comps_per_elem,
                             IsIntBackedArray(elem_type))) {
    return Value();
  }
  Value v;
  v.type_id_ = elem_type;
  v.is_array_ = true;
  v.array_size_ = comps_per_elem
                      ? static_cast<uint32_t>(data.size() / comps_per_elem)
                      : 0;
  new (v.storage_) ArrayHandle(std::move(data));
  return v;
}

// ============================================================
// Queries and accessors
// ============================================================

size_t Value::array_size() const {
  if (!is_array_) return 0;
  if (is_lazy_) return array_size_;
  const size_t size = ArraySlot(storage_)->size();
  return UsesStringStorage(type_id_) ? size
      : ComponentArraySize(size, GetComponentCount(type_id_));
}

void Value::retag_role(TypeId new_type) {
  if (new_type == type_id_ || new_type == TypeId::Invalid ||
      type_id_ == TypeId::Invalid || is_block_) {
    return;
  }
  // Only role re-tags: same scalar component type and component count, so the
  // buffer/SBO layout is identical (Float3 -> point3f, Half4 -> color4h, ...).
  auto comp = [](TypeId t) {
    TypeId c = GetComponentType(t);
    return c == TypeId::Invalid ? t : c;
  };
  // Signed<->unsigned 32-bit int lanes: the crate encodes uint2/3/4 as the
  // Vec*i twin, so the reader must be able to restore uintN from the
  // declared type name. Without this, `uint3 v = (...,4294967295)` read
  // back Int3-typed and printed a negative lane that fails re-parse. Array
  // storage retains distinct signed/unsigned vector objects, so the
  // buffer is REBUILT (bit-exact int32<->uint32) rather than reinterpreted.
  const TypeId oldc = comp(type_id_);
  const TypeId newc = comp(new_type);
  if (oldc != newc) {
    const bool int_uint_pair =
        (oldc == TypeId::Int && newc == TypeId::UInt) ||
        (oldc == TypeId::UInt && newc == TypeId::Int);
    if (!int_uint_pair) return;
    if (GetComponentCount(type_id_) != GetComponentCount(new_type)) return;
    if (is_lazy_) return;  // would require changing the backing storage class
    if (is_array_) {
      ensure_materialized();
      ArrayHandle& h = *ArraySlot(storage_);
      if (oldc == TypeId::Int) {
        auto* st = static_cast<std::vector<int32_t>*>(h.vector_object());
        std::vector<uint32_t> u(st->begin(), st->end());
        h = ArrayHandle(std::move(u));
      } else {
        auto* st = static_cast<std::vector<uint32_t>*>(h.vector_object());
        std::vector<int32_t> i(st->begin(), st->end());
        h = ArrayHandle(std::move(i));
      }
    } else if (GetTypeSize(type_id_) != GetTypeSize(new_type)) {
      return;  // SBO scalar/vector: same byte width required (it is, 4/lane)
    }
    type_id_ = new_type;
    return;
  }
  if (GetComponentCount(type_id_) != GetComponentCount(new_type)) return;
  if (is_lazy_) {
    // Role aliases share the same scalar type, arity, and on-disk bytes. Keep
    // the payload lazy while ensuring materialization preserves the role too.
    LazyArrayRef* ref;
    std::memcpy(&ref, storage_, sizeof(ref));
    if (!ref) return;
    type_id_ = new_type;
    ref->value_type = new_type;
    return;
  }
  if (!is_array_ && GetTypeSize(type_id_) != GetTypeSize(new_type)) return;
  type_id_ = new_type;
}

void Value::clear() {
  destroy();
  type_id_ = TypeId::Invalid;
  is_array_ = false;
  array_size_ = 0;
}

bool Value::uses_heap() const {
  return is_array_ || type_id_ == TypeId::Dictionary;
}

void Value::destroy() {
  if (is_lazy_) {
    LazyArrayRef* ptr;
    std::memcpy(&ptr, storage_, sizeof(ptr));
    delete ptr;  // drops the shared_ptr<CrateDataSource> reference
    type_id_ = TypeId::Invalid;
    is_array_ = false;
    is_lazy_ = false;
    dirty_ = false;
    array_size_ = 0;
    return;
  }

  if (type_id_ == TypeId::Invalid) {
    is_block_ = false;  // a value block holds no heap; just clear the marker.
    return;
  }

  if (is_array_) {
    // Drop the shared array reference (frees the buffer iff this was the last
    // owner — the copy-on-write release).
    ArraySlot(storage_)->~ArrayHandle();
  } else if (type_id_ == TypeId::Dictionary) {
    // Releasing the last handle through shared_ptr's normal destructor makes
    // Dict -> Value -> Dict teardown recurse once per nesting level. Move
    // uniquely-owned child handles out before destroying each Dict so even
    // API-constructed deep dictionaries use an explicit heap work stack.
    DictHandle root = std::move(*DictSlot(storage_));
    DictSlot(storage_)->~DictHandle();
    std::vector<DictHandle> pending;
    pending.push_back(std::move(root));
    while (!pending.empty()) {
      DictHandle current = std::move(pending.back());
      pending.pop_back();
      if (!current || current.use_count() != 1) {
        // Another Value still owns it. Dropping this reference cannot destroy
        // the Dict, and the eventual final owner will perform this traversal.
        continue;
      }

      for (auto &entry : current->entries_) {
        Value &child = entry.second;
        if (child.type_id_ != TypeId::Dictionary || child.is_array_ ||
            child.is_lazy_) {
          continue;
        }
        DictHandle nested = std::move(*DictSlot(child.storage_));
        DictSlot(child.storage_)->~DictHandle();
        child.type_id_ = TypeId::Invalid;
        child.is_array_ = false;
        child.is_lazy_ = false;
        child.dirty_ = false;
        child.is_block_ = false;
        child.array_size_ = 0;
        pending.push_back(std::move(nested));
      }
      current.reset();
    }
  } else if (UsesStringStorage(type_id_)) {
    reinterpret_cast<StringStorage*>(storage_)->~StringStorage();
  }

  type_id_ = TypeId::Invalid;
  is_array_ = false;
  dirty_ = false;
  array_size_ = 0;
}

void Value::copy_from(const Value& other) {
  type_id_ = other.type_id_;
  is_array_ = other.is_array_;
  is_lazy_ = other.is_lazy_;
  dirty_ = other.dirty_;
  is_block_ = other.is_block_;
  array_size_ = other.array_size_;

  if (other.is_lazy_) {
    LazyArrayRef* other_ptr;
    std::memcpy(&other_ptr, other.storage_, sizeof(other_ptr));
    auto* new_ptr = new LazyArrayRef(*other_ptr);  // shared_ptr refcount++
    std::memcpy(storage_, &new_ptr, sizeof(new_ptr));
    return;
  }

  if (other.is_array_) {
    // Copy-on-write: share the same buffer, just bump the refcount. The
    // element data is NOT copied; a later mutable access detaches.
    new (storage_) ArrayHandle(*ArraySlot(other.storage_));
  } else if (other.type_id_ == TypeId::Dictionary) {
    new (storage_) DictHandle(*DictSlot(other.storage_));  // refcount++ (CoW)
  } else if (UsesStringStorage(other.type_id_)) {
    new (storage_) StringStorage{reinterpret_cast<const StringStorage*>(other.storage_)->value};
  } else {
    std::memcpy(storage_, other.storage_, kSBOSize);
  }
}

void Value::move_from(Value&& other) noexcept {
  type_id_ = other.type_id_;
  is_array_ = other.is_array_;
  is_lazy_ = other.is_lazy_;
  dirty_ = other.dirty_;
  is_block_ = other.is_block_;
  array_size_ = other.array_size_;

  if (other.is_array_ && !other.is_lazy_) {
    // Move the shared_ptr handle, then destroy the moved-from (now-empty) slot.
    new (storage_) ArrayHandle(std::move(*ArraySlot(other.storage_)));
    ArraySlot(other.storage_)->~ArrayHandle();
  } else if (other.is_lazy_) {
    // Steal the raw LazyArrayRef* (lazy arrays are not shared_ptr-backed).
    std::memcpy(storage_, other.storage_, sizeof(void*));
  } else if (other.type_id_ == TypeId::Dictionary) {
    new (storage_) DictHandle(std::move(*DictSlot(other.storage_)));
    DictSlot(other.storage_)->~DictHandle();
  } else if (UsesStringStorage(other.type_id_)) {
    new (storage_) StringStorage{std::move(reinterpret_cast<StringStorage*>(other.storage_)->value)};
    reinterpret_cast<StringStorage*>(other.storage_)->~StringStorage();
  } else {
    std::memcpy(storage_, other.storage_, kSBOSize);
  }

  other.type_id_ = TypeId::Invalid;
  other.is_array_ = false;
  other.is_lazy_ = false;
  other.dirty_ = false;
  other.is_block_ = false;
  other.array_size_ = 0;
}

// ============================================================
// Lazy array references
// ============================================================

Value Value::MakeBlock() {
  Value v;
  v.is_block_ = true;  // type_id_ stays Invalid, is_array_/is_lazy_ stay false:
  return v;            // no heap, destroy()/copy share the trivial-scalar path.
}

Value Value::MakeLazyArray(const LazyArrayRef& ref) {
  Value v;
  if (!ref.source || !IsValueArrayType(ref.value_type) ||
      ref.element_count >
      static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
    return v;
  }
  v.type_id_ = ref.value_type;
  v.is_array_ = true;
  v.is_lazy_ = true;
  v.dirty_ = false;
  v.array_size_ = static_cast<uint32_t>(ref.element_count);
  auto* ptr = new LazyArrayRef(ref);
  std::memcpy(v.storage_, &ptr, sizeof(ptr));
  return v;
}

const LazyArrayRef* Value::lazy_ref() const {
  if (!is_lazy_) return nullptr;
  LazyArrayRef* ptr;
  std::memcpy(&ptr, storage_, sizeof(ptr));
  return ptr;
}

void Value::materialize() {
  if (!is_lazy_) return;

  LazyArrayRef* ptr;
  std::memcpy(&ptr, storage_, sizeof(ptr));

  Value decoded;
  bool ok = (ptr && ptr->source) ? ptr->source->MaterializeArray(*ptr, &decoded) : false;
  if (ok) ok = FinalizeLazyDecode(*ptr, &decoded);

  delete ptr;  // releases the shared_ptr<CrateDataSource>

  // Reset to empty WITHOUT calling destroy() (storage_ now dangles) then adopt
  // the decoded value. If decode failed the value becomes empty.
  is_lazy_ = false;
  type_id_ = TypeId::Invalid;
  is_array_ = false;
  dirty_ = false;
  array_size_ = 0;
  if (ok) {
    move_from(std::move(decoded));
  }
}

void Value::ensure_materialized() const {
  if (is_lazy_) {
    const_cast<Value*>(this)->materialize();
  }
}

Value Value::materialized_copy() const {
  if (is_lazy_) {
    const LazyArrayRef* ref = lazy_ref();
    Value decoded;
    if (ref && ref->source && ref->source->MaterializeArray(*ref, &decoded) &&
        FinalizeLazyDecode(*ref, &decoded)) {
      return decoded;  // NRVO / move: steals the decoded array handle.
    }
    return Value();  // decode failed -> empty (prints "None")
  }
  // Non-lazy: ordinary CoW copy (array refcount bump; preserves dirty_).
  return *this;
}

void* Value::data_ptr() {
  ensure_materialized();
  if (is_array_) {
    dirty_ = true;
    DetachArray(storage_);
    return ArraySlot(storage_)->data();
  }
  if (type_id_ == TypeId::Dictionary) {
    DetachDict(storage_);
    return DictSlot(storage_)->get();
  }
  if (UsesStringStorage(type_id_)) {
    return reinterpret_cast<StringStorage*>(storage_)->value.data();
  }
  if (type_id_ == TypeId::Invalid) return nullptr;
  return storage_;
}

const void* Value::data_ptr() const {
  ensure_materialized();
  if (is_array_) return ArraySlot(storage_)->data();
  if (type_id_ == TypeId::Dictionary) {
    return DictSlot(storage_)->get();
  }
  if (UsesStringStorage(type_id_)) {
    return reinterpret_cast<const StringStorage*>(storage_)->value.data();
  }
  if (type_id_ == TypeId::Invalid) return nullptr;
  return storage_;
}

const void* Value::raw_data() const {
  return data_ptr();
}

void* Value::raw_data() {
  return data_ptr();
}

// ============================================================
// Type-safe accessors
// ============================================================

#define DEFINE_SCALAR_ACCESSOR(type, type_id_val, ret_type) \
  const ret_type* Value::as_##type() const { \
    if (type_id_ != TypeId::type_id_val || is_array_) return nullptr; \
    return reinterpret_cast<const ret_type*>(storage_); \
  } \
  ret_type* Value::as_##type() { \
    if (type_id_ != TypeId::type_id_val || is_array_) return nullptr; \
    return reinterpret_cast<ret_type*>(storage_); \
  }

DEFINE_SCALAR_ACCESSOR(bool, Bool, bool)
DEFINE_SCALAR_ACCESSOR(int, Int, int32_t)
DEFINE_SCALAR_ACCESSOR(uint, UInt, uint32_t)
DEFINE_SCALAR_ACCESSOR(int64, Int64, int64_t)
DEFINE_SCALAR_ACCESSOR(uint64, UInt64, uint64_t)
DEFINE_SCALAR_ACCESSOR(float, Float, float)
DEFINE_SCALAR_ACCESSOR(uchar, UChar, uint8_t)

#undef DEFINE_SCALAR_ACCESSOR

// double additionally accepts TimeCode: same 8-byte storage, and scalar
// timecode values now keep their type identity through the crate reader.
const double* Value::as_double() const {
  if ((type_id_ != TypeId::Double && type_id_ != TypeId::TimeCode) ||
      is_array_) {
    return nullptr;
  }
  return reinterpret_cast<const double*>(storage_);
}
double* Value::as_double() {
  if ((type_id_ != TypeId::Double && type_id_ != TypeId::TimeCode) ||
      is_array_) {
    return nullptr;
  }
  return reinterpret_cast<double*>(storage_);
}

const std::string* Value::as_string() const {
  // PathExpression shares the string storage and reads as a string.
  if ((type_id_ != TypeId::String && type_id_ != TypeId::PathExpression) ||
      is_array_) {
    return nullptr;
  }
  return &reinterpret_cast<const StringStorage*>(storage_)->value;
}

std::string* Value::as_string() {
  if (type_id_ != TypeId::String || is_array_) return nullptr;
  return &reinterpret_cast<StringStorage*>(storage_)->value;
}

const std::string* Value::as_token() const {
  if (type_id_ != TypeId::Token || is_array_) return nullptr;
  return &reinterpret_cast<const StringStorage*>(storage_)->value;
}

const std::string* Value::as_asset_path() const {
  if (type_id_ != TypeId::AssetPath || is_array_) return nullptr;
  return &reinterpret_cast<const StringStorage*>(storage_)->value;
}

const Dict* Value::as_dictionary() const {
  if (type_id_ != TypeId::Dictionary) return nullptr;
  return DictSlot(storage_)->get();
}

Dict* Value::as_dictionary() {
  if (type_id_ != TypeId::Dictionary) return nullptr;
  DetachDict(storage_);
  return DictSlot(storage_)->get();
}

// Vector accessors
const int32_t* Value::as_int2() const {
  if (type_id_ != TypeId::Int2 || is_array_) return nullptr;
  return reinterpret_cast<const int32_t*>(storage_);
}

const int32_t* Value::as_int3() const {
  if (type_id_ != TypeId::Int3 || is_array_) return nullptr;
  return reinterpret_cast<const int32_t*>(storage_);
}

const int32_t* Value::as_int4() const {
  if (type_id_ != TypeId::Int4 || is_array_) return nullptr;
  return reinterpret_cast<const int32_t*>(storage_);
}

const float* Value::as_float2() const {
  if ((type_id_ != TypeId::Float2 && type_id_ != TypeId::Texcoord2f) ||
      is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const float* Value::as_float3() const {
  if (type_id_ != TypeId::Float3 && type_id_ != TypeId::Point3f &&
      type_id_ != TypeId::Vector3f && type_id_ != TypeId::Normal3f &&
      type_id_ != TypeId::Color3f && type_id_ != TypeId::Texcoord3f)
    return nullptr;
  if (is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const float* Value::as_float4() const {
  if (type_id_ != TypeId::Float4 && type_id_ != TypeId::Quatf &&
      type_id_ != TypeId::Color4f) return nullptr;
  if (is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const double* Value::as_double2() const {
  if ((type_id_ != TypeId::Double2 && type_id_ != TypeId::Texcoord2d) ||
      is_array_) return nullptr;
  return reinterpret_cast<const double*>(storage_);
}

const double* Value::as_double3() const {
  if (type_id_ != TypeId::Double3 && type_id_ != TypeId::Point3d &&
      type_id_ != TypeId::Vector3d && type_id_ != TypeId::Normal3d &&
      type_id_ != TypeId::Color3d && type_id_ != TypeId::Texcoord3d)
    return nullptr;
  if (is_array_) return nullptr;
  return reinterpret_cast<const double*>(storage_);
}

// ============================================================
// Converting scalar reads (to_float*): unlike the as_* accessors, these
// widen raw-half SBO scalars (authored half/half3/... store half-bit lanes,
// not floats) and narrow double-backed values. Lanes are copied in storage
// order — quat conventions are the caller's concern, same as as_float4().
// ============================================================

namespace {

// Same conversion as crate-format.hh HalfToFloat (kept local: the types
// layer must not depend on the crate reader).
inline float HalfBitsToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;  // +/- 0
    } else {
      // Subnormal half -> normalized float
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        exponent--;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1Fu) {
    bits = sign | 0x7F800000u | (mantissa << 13);  // inf / nan
  } else {
    bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline int HalfLaneCount(TypeId id) {
  switch (id) {
    case TypeId::Half: return 1;
    case TypeId::Half2:
    case TypeId::Texcoord2h: return 2;
    case TypeId::Half3:
    case TypeId::Point3h:
    case TypeId::Vector3h:
    case TypeId::Normal3h:
    case TypeId::Color3h:
    case TypeId::Texcoord3h: return 3;
    case TypeId::Half4:
    case TypeId::Quath:
    case TypeId::Color4h: return 4;
    default: return 0;
  }
}

}  // namespace

bool Value::ToFloatLanes(int lanes, float* out) const {
  if (!out || is_array_) return false;
  if (HalfLaneCount(type_id_) == lanes) {
    const uint16_t* bits = reinterpret_cast<const uint16_t*>(storage_);
    for (int i = 0; i < lanes; ++i) out[i] = HalfBitsToFloat(bits[i]);
    return true;
  }
  switch (lanes) {
    case 1:
      if (const float* f = as_float()) { out[0] = *f; return true; }
      if (const double* d = as_double()) { out[0] = static_cast<float>(*d); return true; }
      return false;
    case 2:
      if (const float* f = as_float2()) { out[0] = f[0]; out[1] = f[1]; return true; }
      if (const double* d = as_double2()) {
        for (int i = 0; i < 2; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    case 3:
      if (const float* f = as_float3()) {
        for (int i = 0; i < 3; ++i) out[i] = f[i];
        return true;
      }
      if (const double* d = as_double3()) {
        for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    case 4:
      if (const float* f = as_float4()) {
        for (int i = 0; i < 4; ++i) out[i] = f[i];
        return true;
      }
      if (const double* d = as_double4()) {
        for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool Value::to_float(float* out) const { return ToFloatLanes(1, out); }
bool Value::to_float2(float* out) const { return ToFloatLanes(2, out); }
bool Value::to_float3(float* out) const { return ToFloatLanes(3, out); }
bool Value::to_float4(float* out) const { return ToFloatLanes(4, out); }

const double* Value::as_double4() const {
  if (type_id_ != TypeId::Double4 && type_id_ != TypeId::Quatd &&
      type_id_ != TypeId::Color4d) return nullptr;
  if (is_array_) return nullptr;
  return reinterpret_cast<const double*>(storage_);
}

// Matrix accessors
const float* Value::as_matrix2f() const {
  if (type_id_ != TypeId::Matrix2f || is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const float* Value::as_matrix3f() const {
  if (type_id_ != TypeId::Matrix3f || is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const float* Value::as_matrix4f() const {
  if (type_id_ != TypeId::Matrix4f || is_array_) return nullptr;
  return reinterpret_cast<const float*>(storage_);
}

const double* Value::as_matrix2d() const {
  if (type_id_ != TypeId::Matrix2d || is_array_) return nullptr;
  return reinterpret_cast<const double*>(storage_);
}

const double* Value::as_matrix3d() const {
  if (type_id_ != TypeId::Matrix3d || is_array_) return nullptr;
  return reinterpret_cast<const double*>(storage_);
}

const double* Value::as_matrix4d() const {
  // frame4d is a matrix4d role type (same double[16] storage).
  if ((type_id_ != TypeId::Matrix4d && type_id_ != TypeId::Frame4d) ||
      is_array_) {
    return nullptr;
  }
  return reinterpret_cast<const double*>(storage_);
}

// Array accessors
const std::vector<float>* Value::as_float_array() const {
  ensure_materialized();
  if (!is_array_ || !IsFloatBackedArray(type_id_)) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<float>*>(ptr);
}

std::vector<float>* Value::as_float_array() {
  ensure_materialized();
  if (!is_array_ || !IsFloatBackedArray(type_id_)) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<float>*>(ptr);
}

const std::vector<int32_t>* Value::as_int_array() const {
  ensure_materialized();
  if (!is_array_ || !IsIntBackedArray(type_id_)) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<int32_t>*>(ptr);
}

std::vector<int32_t>* Value::as_int_array() {
  ensure_materialized();
  if (!is_array_ || !IsIntBackedArray(type_id_)) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<int32_t>*>(ptr);
}

const std::vector<double>* Value::as_double_array() const {
  ensure_materialized();
  if (!is_array_ || !IsDoubleBackedArray(type_id_)) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<double>*>(ptr);
}
std::vector<double>* Value::as_double_array() {
  ensure_materialized();
  if (!is_array_ || !IsDoubleBackedArray(type_id_)) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<double>*>(ptr);
}
const std::vector<int64_t>* Value::as_int64_array() const {
  ensure_materialized();
  if (type_id_ != TypeId::Int64 || !is_array_) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<int64_t>*>(ptr);
}
std::vector<int64_t>* Value::as_int64_array() {
  ensure_materialized();
  if (type_id_ != TypeId::Int64 || !is_array_) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<int64_t>*>(ptr);
}
const std::vector<uint32_t>* Value::as_uint_array() const {
  ensure_materialized();
  if (!is_array_ || !IsUIntBackedArray(type_id_)) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<uint32_t>*>(ptr);
}
std::vector<uint32_t>* Value::as_uint_array() {
  ensure_materialized();
  if (!is_array_ || !IsUIntBackedArray(type_id_)) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<uint32_t>*>(ptr);
}
const std::vector<uint64_t>* Value::as_uint64_array() const {
  ensure_materialized();
  if (type_id_ != TypeId::UInt64 || !is_array_) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<uint64_t>*>(ptr);
}
std::vector<uint64_t>* Value::as_uint64_array() {
  ensure_materialized();
  if (type_id_ != TypeId::UInt64 || !is_array_) return nullptr;
  dirty_ = true;
  DetachArray(storage_);
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<uint64_t>*>(ptr);
}
const std::vector<uint8_t>* Value::as_bool_array() const {
  ensure_materialized();
  if (type_id_ != TypeId::Bool || !is_array_) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<uint8_t>*>(ptr);
}
const std::vector<std::string>* Value::as_token_array() const {
  ensure_materialized();
  // Token / String / AssetPath / PathExpression arrays share the
  // string-vector storage.
  if (!is_array_ || !UsesStringStorage(type_id_)) return nullptr;
  void* ptr = ArraySlot(storage_)->vector_object();
  return static_cast<std::vector<std::string>*>(ptr);
}

// ============================================================
// Comparison
// ============================================================

bool Value::operator==(const Value& other) const {
  struct ValuePair {
    const Value* lhs{nullptr};
    const Value* rhs{nullptr};
  };
  struct DictPair {
    const Dict* lhs{nullptr};
    const Dict* rhs{nullptr};
    bool operator==(const DictPair& other_pair) const {
      return lhs == other_pair.lhs && rhs == other_pair.rhs;
    }
  };
  struct DictPairHash {
    size_t operator()(const DictPair& pair) const {
      const size_t lhs_hash = std::hash<const Dict*>()(pair.lhs);
      const size_t rhs_hash = std::hash<const Dict*>()(pair.rhs);
      return lhs_hash ^ (rhs_hash + size_t(0x9e3779b9U) +
                         (lhs_hash << 6U) + (lhs_hash >> 2U));
    }
  };

  std::vector<ValuePair> pending;
  std::unordered_set<DictPair, DictPairHash> compared_dicts;
  pending.push_back(ValuePair{this, &other});

  while (!pending.empty()) {
    const ValuePair pair = pending.back();
    pending.pop_back();
    const Value& lhs = *pair.lhs;
    const Value& rhs = *pair.rhs;
    lhs.ensure_materialized();
    rhs.ensure_materialized();
    if (lhs.type_id_ != rhs.type_id_) return false;
    if (lhs.is_array_ != rhs.is_array_) return false;
    // A value block differs from both declared-only and empty values.
    if (lhs.is_block_ != rhs.is_block_) return false;
    if (lhs.is_array_ && lhs.array_size() != rhs.array_size()) return false;
    if (lhs.type_id_ == TypeId::Invalid) continue;

    if (lhs.is_array_) {
      bool equal = false;
      if (IsFloatBackedArray(lhs.type_id_)) {
        equal = *lhs.as_float_array() == *rhs.as_float_array();
      } else if (IsDoubleBackedArray(lhs.type_id_)) {
        equal = *lhs.as_double_array() == *rhs.as_double_array();
      } else if (lhs.type_id_ == TypeId::Int) {
        equal = *lhs.as_int_array() == *rhs.as_int_array();
      } else if (lhs.type_id_ == TypeId::Int64) {
        equal = *lhs.as_int64_array() == *rhs.as_int64_array();
      } else if (IsUIntBackedArray(lhs.type_id_)) {
        equal = *lhs.as_uint_array() == *rhs.as_uint_array();
      } else if (lhs.type_id_ == TypeId::UInt64) {
        equal = *lhs.as_uint64_array() == *rhs.as_uint64_array();
      } else if (lhs.type_id_ == TypeId::Bool) {
        equal = *lhs.as_bool_array() == *rhs.as_bool_array();
      } else if (UsesStringStorage(lhs.type_id_)) {
        equal = *lhs.as_token_array() == *rhs.as_token_array();
      } else if (IsIntBackedArray(lhs.type_id_)) {
        equal = *lhs.as_int_array() == *rhs.as_int_array();
      }
      if (!equal) return false;
      continue;
    }

    if (lhs.type_id_ == TypeId::Dictionary) {
      const Dict* a = lhs.as_dictionary();
      const Dict* b = rhs.as_dictionary();
      if (!a || !b) {
        if (a != b) return false;
        continue;
      }
      if (a == b) continue;
      if (!compared_dicts.insert(DictPair{a, b}).second) continue;
      if (a->entries().size() != b->entries().size()) return false;
      for (size_t i = a->entries().size(); i > 0; --i) {
        const auto& lhs_entry = a->entries()[i - 1];
        const auto& rhs_entry = b->entries()[i - 1];
        if (lhs_entry.first != rhs_entry.first) return false;
        pending.push_back(
            ValuePair{&lhs_entry.second, &rhs_entry.second});
      }
      continue;
    }

    if (UsesStringStorage(lhs.type_id_)) {
      if (reinterpret_cast<const StringStorage*>(lhs.storage_)->value !=
          reinterpret_cast<const StringStorage*>(rhs.storage_)->value) {
        return false;
      }
      continue;
    }

    const size_t size = GetTypeSize(lhs.type_id_);
    if (size == 0) return false;
    if (std::memcmp(lhs.storage_, rhs.storage_, size) != 0) {
      return false;
    }
  }

  return true;
}

// ============================================================
// Hashing
// ============================================================

namespace {

// FNV-1a hash for arbitrary byte sequences
inline uint64_t fnv1a_hash(const uint8_t* data, size_t len) {
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;

  uint64_t hash = FNV_OFFSET;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= FNV_PRIME;
  }
  return hash;
}

}  // namespace

uint64_t Value::hash() const {
  ensure_materialized();
  if (type_id_ == TypeId::Invalid) return 0;

  // Include type in hash
  uint64_t h = static_cast<uint64_t>(type_id_) | (is_array_ ? 0x100 : 0);

  if (is_array_) {
    // Hash array contents
    if (IsFloatBackedArray(type_id_)) {
      const auto* arr = as_float_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(float));
      }
    } else if (IsDoubleBackedArray(type_id_)) {
      const auto* arr = as_double_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(double));
      }
    } else if (IsIntBackedArray(type_id_)) {
      const auto* arr = as_int_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(int32_t));
      }
    } else if (type_id_ == TypeId::Int64) {
      const auto* arr = as_int64_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(int64_t));
      }
    } else if (IsUIntBackedArray(type_id_)) {
      const auto* arr = as_uint_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(uint32_t));
      }
    } else if (type_id_ == TypeId::UInt64) {
      const auto* arr = as_uint64_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(uint64_t));
      }
    } else if (type_id_ == TypeId::Bool) {
      const auto* arr = as_bool_array();
      if (arr && !arr->empty()) {
        h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(arr->data()),
                        arr->size() * sizeof(uint8_t));
      }
    } else if (UsesStringStorage(type_id_)) {
      const auto* arr = as_token_array();
      if (arr) {
        for (const std::string& s : *arr) {
          h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(s.data()),
                          s.size());
          h *= 1099511628211ULL;
        }
      }
    }
    return h;
  }

  // Hash dictionary entries in post-order (order-sensitive, matching
  // operator==) without consuming one C++ stack frame per nesting level.
  if (type_id_ == TypeId::Dictionary) {
    const Dict* root = as_dictionary();
    if (!root) return h;

    struct HashFrame {
      const Dict* dict{nullptr};
      size_t next_entry{0};
      uint64_t hash{static_cast<uint64_t>(TypeId::Dictionary)};
    };
    std::vector<HashFrame> pending;
    std::unordered_set<const Dict*> active;
    std::unordered_map<const Dict*, uint64_t> completed;
    pending.push_back(HashFrame{root, 0,
                                static_cast<uint64_t>(TypeId::Dictionary)});
    active.insert(root);

    while (!pending.empty()) {
      HashFrame& frame = pending.back();
      if (frame.next_entry == frame.dict->entries().size()) {
        const uint64_t child_hash = frame.hash;
        completed[frame.dict] = child_hash;
        active.erase(frame.dict);
        pending.pop_back();
        if (pending.empty()) return child_hash;
        pending.back().hash ^= child_hash;
        pending.back().hash *= 1099511628211ULL;
        continue;
      }

      const auto& kv = frame.dict->entries()[frame.next_entry++];
      frame.hash ^=
          fnv1a_hash(reinterpret_cast<const uint8_t*>(kv.first.data()),
                     kv.first.size());
      frame.hash *= 1099511628211ULL;

      const Dict* nested =
          kv.second.is_dictionary() ? kv.second.as_dictionary() : nullptr;
      if (!nested) {
        frame.hash ^= kv.second.hash();
        frame.hash *= 1099511628211ULL;
        continue;
      }

      const auto complete_it = completed.find(nested);
      if (complete_it != completed.end()) {
        frame.hash ^= complete_it->second;
        frame.hash *= 1099511628211ULL;
      } else if (active.find(nested) != active.end()) {
        // Mutable dictionary access can construct shared_ptr cycles that
        // cannot appear in parsed USD. Fold a stable dictionary marker rather
        // than looping forever on such API-created graphs.
        frame.hash ^= static_cast<uint64_t>(TypeId::Dictionary);
        frame.hash *= 1099511628211ULL;
      } else {
        active.insert(nested);
        pending.push_back(HashFrame{
            nested, 0, static_cast<uint64_t>(TypeId::Dictionary)});
      }
    }
    return h;  // unreachable, retained for defensive compiler flow analysis
  }

  // Hash string types
  if (UsesStringStorage(type_id_)) {
    const auto& s = reinterpret_cast<const StringStorage*>(storage_)->value;
    if (!s.empty()) {
      h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    return h;
  }

  // Hash scalar/vector types from storage
  size_t size = GetTypeSize(type_id_);
  if (size > 0) {
    h ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(storage_), size);
  }

  return h;
}

const uint8_t* Value::raw_bytes(size_t* out_size) const {
  if (!out_size) return nullptr;
  *out_size = 0;

  ensure_materialized();
  if (type_id_ == TypeId::Invalid) return nullptr;

  if (is_array_) {
    if (IsFloatBackedArray(type_id_)) {
      const auto* arr = as_float_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(float);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (IsDoubleBackedArray(type_id_)) {
      const auto* arr = as_double_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(double);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (IsIntBackedArray(type_id_)) {
      const auto* arr = as_int_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(int32_t);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (type_id_ == TypeId::Int64) {
      const auto* arr = as_int64_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(int64_t);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (IsUIntBackedArray(type_id_)) {
      const auto* arr = as_uint_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(uint32_t);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (type_id_ == TypeId::UInt64) {
      const auto* arr = as_uint64_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(uint64_t);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    } else if (type_id_ == TypeId::Bool) {
      const auto* arr = as_bool_array();
      if (arr && !arr->empty()) {
        *out_size = arr->size() * sizeof(uint8_t);
        return reinterpret_cast<const uint8_t*>(arr->data());
      }
    }
    return nullptr;
  }

  if (UsesStringStorage(type_id_)) {
    const auto& s = reinterpret_cast<const StringStorage*>(storage_)->value;
    *out_size = s.size();
    return reinterpret_cast<const uint8_t*>(s.data());
  }

  size_t size = GetTypeSize(type_id_);
  if (size > 0) {
    *out_size = size;
    return reinterpret_cast<const uint8_t*>(storage_);
  }

  return nullptr;
}

Value LerpValue(const Value& a, const Value& b, double t) {
  if (a.type_id() != b.type_id()) return a;  // held on type mismatch

  // Delegate every interpolatable type to the full interpolator (per-lane
  // lerp for float/double/half-backed values, slerp for quats). The previous
  // hand-rolled scalar switch here silently HELD Half/Half2/3/4, the
  // semantic half aliases, Quath, and Matrix2f/3f/2d/3d, so timeSamples of
  // those types snapped to the earlier key when queried between samples.
  if (a.is_array() || TimeInterpolator::IsLinearInterpolatable(a.type_id())) {
    Value r = TimeInterpolator::InterpolateValues(a, b, t);
    return (r.type_id() == TypeId::Invalid) ? a : r;
  }
  return a;  // non-interpolatable (int/bool/string/token/...) -> held
}

}  // namespace next
}  // namespace lightusd
