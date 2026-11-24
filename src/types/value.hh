// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "type-id.hh"
#include "value-views.hh"

namespace lightusd {
class Value;
class Dict;
class DictView;
class MutableDictView;
struct LazyArrayRef;

// Read-only, non-owning view into a C11 value. Invalidated by owner mutation.
class ValueView {
 public:
  ValueView() = default;
  ValueView(std::nullptr_t) {}
  ValueView(const Value*);
  ValueView(const Value&);
  bool operator==(std::nullptr_t) const { return !core_; }
  bool operator!=(std::nullptr_t) const { return core_ != nullptr; }
  explicit ValueView(const lu_value* p) : core_(p) {}
  explicit operator bool() const { return core_ != nullptr; }
  const ValueView* operator->() const { return this; }
  const ValueView& operator*() const { return *this; }
  operator Value() const;
  const lu_value* core() const { return core_; }
  TypeId type_id() const;
  bool is_empty() const;
  bool is_block() const;
  bool is_array() const;
  bool is_dictionary() const;
  bool is_lazy() const;
  bool is_dirty() const;
  size_t array_size() const;
  size_t dynamic_string_memory_usage() const;
  size_t heap_memory_usage() const;
  const LazyArrayRef* lazy_ref() const;
  Value materialized_copy() const;
  const void* raw_data() const;
  const uint8_t* raw_bytes(size_t*) const;
  uint64_t hash() const;
  StringView as_string() const;
  StringView as_token() const;
  StringView as_asset_path() const;
  DictView as_dictionary() const;
  StringArrayView as_token_array() const;
  const bool* as_bool() const;
  const int32_t* as_int() const;
  const uint32_t* as_uint() const;
  const int64_t* as_int64() const;
  const uint64_t* as_uint64() const;
  const float* as_float() const;
  const uint8_t* as_uchar() const;
  const double* as_double() const;
  const int32_t* as_int2() const;
  const int32_t* as_int3() const;
  const int32_t* as_int4() const;
  const float* as_float2() const;
  const float* as_float3() const;
  const float* as_float4() const;
  const double* as_double2() const;
  const double* as_double3() const;
  const double* as_double4() const;
  const float* as_matrix2f() const;
  const float* as_matrix3f() const;
  const float* as_matrix4f() const;
  const double* as_matrix2d() const;
  const double* as_matrix3d() const;
  const double* as_matrix4d() const;
  ValueArrayView<const float> as_float_array() const;
  ValueArrayView<const double> as_double_array() const;
  ValueArrayView<const int32_t> as_int_array() const;
  ValueArrayView<const uint32_t> as_uint_array() const;
  ValueArrayView<const int64_t> as_int64_array() const;
  ValueArrayView<const uint64_t> as_uint64_array() const;
  ValueArrayView<const uint8_t> as_bool_array() const;
  bool to_float(float*) const;
  bool to_float2(float*) const;
  bool to_float3(float*) const;
  bool to_float4(float*) const;
 private:
  bool ToFloatLanes(int, float*) const;
  bool materialize() const;
  const lu_value* core_ = nullptr;
};
class Value {
 public:
  static constexpr size_t kSBOSize = LU_VALUE_INLINE_CAP;
  Value() = default;
  ~Value();
  Value(const Value&);
  Value(Value&&) noexcept;
  Value& operator=(const Value&);
  Value& operator=(Value&&) noexcept;
  explicit Value(bool v);
  explicit Value(int32_t v);
  explicit Value(uint32_t v);
  explicit Value(int64_t v);
  explicit Value(uint64_t v);
  explicit Value(float v);
  explicit Value(double v);
  explicit Value(const char* v);
  explicit Value(const std::string& v);
  explicit Value(std::string&& v);
  static Value MakePoint3f(float x, float y, float z);
  static Value MakePoint3d(double x, double y, double z);
  static Value MakeVector3f(float x, float y, float z);
  static Value MakeVector3d(double x, double y, double z);
  static Value MakeNormal3f(float x, float y, float z);
  static Value MakeNormal3d(double x, double y, double z);
  static Value MakeColor3f(float r, float g, float b);
  static Value MakeColor4f(float r, float g, float b, float a);
  static Value MakeTexcoord2f(float u, float v_coord);
  static Value MakeInt2(int32_t x, int32_t y);
  static Value MakeInt3(int32_t x, int32_t y, int32_t z);
  static Value MakeInt4(int32_t x, int32_t y, int32_t z, int32_t w);
  static Value MakeFloat2(float x, float y);
  static Value MakeFloat3(float x, float y, float z);
  static Value MakeFloat4(float x, float y, float z, float w);
  static Value MakeDouble2(double x, double y);
  static Value MakeDouble3(double x, double y, double z);
  static Value MakeDouble4(double x, double y, double z, double w);
  // Canonical USD order: scalar first (w, x, y, z), including POD storage.
  static Value MakeQuatf(float w, float x, float y, float z);
  static Value MakeQuatd(double w, double x, double y, double z);
  static Value MakeMatrix2f(const float* data);
  static Value MakeMatrix3f(const float* data);
  static Value MakeMatrix4f(const float* data);
  static Value MakeMatrix2d(const double* data);
  static Value MakeMatrix3d(const double* data);
  static Value MakeMatrix4d(const double* data);
  static Value MakeFromRaw(TypeId, const void*);
  static Value FromCore(const lu_value*);
  static Value MakeToken(StringView);
  static Value MakeAssetPath(StringView);
  static Value MakeStringLike(StringView, TypeId);
  static Value MakeDictionary();
  static Value MakeDictionary(Dict&&);
  static Value MakeBlock();
  static Value MakeLazyArray(const LazyArrayRef&);
  static Value MakeArrayFromRaw(TypeId, const void*, size_t elements);
  static Value MakeEmptyArray(TypeId, size_t elements);
  static Value MakeStrings(TypeId, size_t, lu_string_at, const void*);
  template<class R> static Value MakeStringLikeArray(const R& data, TypeId type) {
    return MakeStrings(type, data.size(), [](const void* p, size_t i, size_t* n) {
      const auto& s = static_cast<const R*>(p)->data()[i];
      *n = s.size(); return s.data();
    }, &data);
  }
  template<class R> static Value MakeTokenArray(const R& data) { return MakeStringLikeArray(data, TypeId::Token); }
  static Value MakeTokenArray(std::initializer_list<std::string> data);
  template<class R> static Value MakeBoolArray(const R& data) {
    Value v = MakeEmptyArray(TypeId::Bool, data.size());
    if (!v.is_array()) return v;
    auto* p = const_cast<uint8_t*>(static_cast<const uint8_t*>(lu_value_data(&v.core_)));
    size_t i = 0; for (bool b : data) p[i++] = b ? 1 : 0;
    return v;
  }
  static Value MakeBoolArray(std::initializer_list<bool> data);
  static Value MakeFloatArray(ValueArrayView<const float> data);
  static Value MakeFloatArray(std::initializer_list<float> data) { return MakeFloatArray(ValueArrayView<const float>(data.begin(), data.size())); }
  static Value MakeIntArray(ValueArrayView<const int32_t> data);
  static Value MakeIntArray(std::initializer_list<int32_t> data) { return MakeIntArray(ValueArrayView<const int32_t>(data.begin(), data.size())); }
  static Value MakeDoubleArray(ValueArrayView<const double> data);
  static Value MakeDoubleArray(std::initializer_list<double> data) { return MakeDoubleArray(ValueArrayView<const double>(data.begin(), data.size())); }
  static Value MakeInt64Array(ValueArrayView<const int64_t> data);
  static Value MakeInt64Array(std::initializer_list<int64_t> data) { return MakeInt64Array(ValueArrayView<const int64_t>(data.begin(), data.size())); }
  static Value MakeUIntArray(ValueArrayView<const uint32_t> data);
  static Value MakeUIntArray(std::initializer_list<uint32_t> data) { return MakeUIntArray(ValueArrayView<const uint32_t>(data.begin(), data.size())); }
  static Value MakeUInt64Array(ValueArrayView<const uint64_t> data);
  static Value MakeUInt64Array(std::initializer_list<uint64_t> data) { return MakeUInt64Array(ValueArrayView<const uint64_t>(data.begin(), data.size())); }
  static Value MakeBoolByteArray(ValueArrayView<const uint8_t> data);
  static Value MakeBoolByteArray(std::initializer_list<uint8_t> data) { return MakeBoolByteArray(ValueArrayView<const uint8_t>(data.begin(), data.size())); }
  static Value MakeFloat2Array(ValueArrayView<const float> data);
  static Value MakeFloat2Array(std::initializer_list<float> data) { return MakeFloat2Array(ValueArrayView<const float>(data.begin(), data.size())); }
  static Value MakeFloat3Array(ValueArrayView<const float> data);
  static Value MakeFloat3Array(std::initializer_list<float> data) { return MakeFloat3Array(ValueArrayView<const float>(data.begin(), data.size())); }
  static Value MakeFloatCompArray(ValueArrayView<const float>, TypeId, uint32_t);
  static Value MakeFloatCompArray(std::initializer_list<float> v, TypeId type, uint32_t components) { return MakeFloatCompArray(ValueArrayView<const float>(v.begin(), v.size()), type, components); }
  static Value MakeDoubleCompArray(ValueArrayView<const double>, TypeId, uint32_t);
  static Value MakeDoubleCompArray(std::initializer_list<double> v, TypeId type, uint32_t components) { return MakeDoubleCompArray(ValueArrayView<const double>(v.begin(), v.size()), type, components); }
  static Value MakeIntCompArray(ValueArrayView<const int32_t>, TypeId, uint32_t);
  static Value MakeIntCompArray(std::initializer_list<int32_t> v, TypeId type, uint32_t components) { return MakeIntCompArray(ValueArrayView<const int32_t>(v.begin(), v.size()), type, components); }
  static Value MakeUIntCompArray(ValueArrayView<const uint32_t>, TypeId, uint32_t);
  static Value MakeUIntCompArray(std::initializer_list<uint32_t> v, TypeId type, uint32_t components) { return MakeUIntCompArray(ValueArrayView<const uint32_t>(v.begin(), v.size()), type, components); }
  enum class ArrayScalarKind : uint8_t { Float, Double, Int32, UInt32, Int64, UInt64, Bool };
  class DeferredArrayFill {
   public:
    DeferredArrayFill() = default;
    ~DeferredArrayFill();
    DeferredArrayFill(DeferredArrayFill&&) noexcept;
    DeferredArrayFill& operator=(DeferredArrayFill&&) noexcept;
    DeferredArrayFill(const DeferredArrayFill&) = delete;
    DeferredArrayFill& operator=(const DeferredArrayFill&) = delete;
    bool Complete(Value&&);
    void Release();
   private:
    friend class Value;
    lu_buffer* buffer_ = nullptr;
    TypeId type_ = TypeId::Invalid;
    uint32_t count_ = 0;
  };
  static Value MakeDeferredArray(TypeId, ArrayScalarKind, uint32_t, DeferredArrayFill*);
  ValueView view() const { return ValueView(&core_); }
  const lu_value* core() const { return &core_; }
  lu_value* mutable_core() { return &core_; }
  void clear();
  void retag_role(TypeId);
  void mark_dirty();
  bool materialize();
  void adopt_pod(const void*, size_t);
  bool set_string(StringView);
  bool resize_array(size_t);
  bool set_string_at(size_t i, StringView s) { return lu_value_string_array_set(&core_, i, s.data(), s.size()) != 0; }
  void* raw_data();
  MutableDictView as_dictionary();
  StringView as_string() { return view().as_string(); }
  ValueArrayView<float> as_float_array();
  ValueArrayView<double> as_double_array();
  ValueArrayView<int32_t> as_int_array();
  ValueArrayView<uint32_t> as_uint_array();
  ValueArrayView<int64_t> as_int64_array();
  ValueArrayView<uint64_t> as_uint64_array();
  TypeId type_id() const { return view().type_id(); }
  bool is_empty() const { return view().is_empty(); }
  bool is_block() const { return view().is_block(); }
  bool is_array() const { return view().is_array(); }
  bool is_dictionary() const { return view().is_dictionary(); }
  bool is_lazy() const { return view().is_lazy(); }
  bool is_dirty() const { return view().is_dirty(); }
  size_t array_size() const { return view().array_size(); }
  size_t dynamic_string_memory_usage() const { return view().dynamic_string_memory_usage(); }
  size_t heap_memory_usage() const { return view().heap_memory_usage(); }
  const LazyArrayRef* lazy_ref() const { return view().lazy_ref(); }
  Value materialized_copy() const { return view().materialized_copy(); }
  const void* raw_data() const { return view().raw_data(); }
  const uint8_t* raw_bytes(size_t* size) const { return view().raw_bytes(size); }
  uint64_t hash() const { return view().hash(); }
  StringView as_string() const { return view().as_string(); }
  StringView as_token() const { return view().as_token(); }
  StringView as_asset_path() const { return view().as_asset_path(); }
  DictView as_dictionary() const;
  StringArrayView as_token_array() const { return view().as_token_array(); }
  const bool* as_bool() const { return view().as_bool(); }
  const int32_t* as_int() const { return view().as_int(); }
  const uint32_t* as_uint() const { return view().as_uint(); }
  const int64_t* as_int64() const { return view().as_int64(); }
  const uint64_t* as_uint64() const { return view().as_uint64(); }
  const float* as_float() const { return view().as_float(); }
  const uint8_t* as_uchar() const { return view().as_uchar(); }
  const double* as_double() const { return view().as_double(); }
  const int32_t* as_int2() const { return view().as_int2(); }
  const int32_t* as_int3() const { return view().as_int3(); }
  const int32_t* as_int4() const { return view().as_int4(); }
  const float* as_float2() const { return view().as_float2(); }
  const float* as_float3() const { return view().as_float3(); }
  const float* as_float4() const { return view().as_float4(); }
  const double* as_double2() const { return view().as_double2(); }
  const double* as_double3() const { return view().as_double3(); }
  const double* as_double4() const { return view().as_double4(); }
  const float* as_matrix2f() const { return view().as_matrix2f(); }
  const float* as_matrix3f() const { return view().as_matrix3f(); }
  const float* as_matrix4f() const { return view().as_matrix4f(); }
  const double* as_matrix2d() const { return view().as_matrix2d(); }
  const double* as_matrix3d() const { return view().as_matrix3d(); }
  const double* as_matrix4d() const { return view().as_matrix4d(); }
  ValueArrayView<const float> as_float_array() const { return view().as_float_array(); }
  ValueArrayView<const double> as_double_array() const { return view().as_double_array(); }
  ValueArrayView<const int32_t> as_int_array() const { return view().as_int_array(); }
  ValueArrayView<const uint32_t> as_uint_array() const { return view().as_uint_array(); }
  ValueArrayView<const int64_t> as_int64_array() const { return view().as_int64_array(); }
  ValueArrayView<const uint64_t> as_uint64_array() const { return view().as_uint64_array(); }
  ValueArrayView<const uint8_t> as_bool_array() const { return view().as_bool_array(); }
  bool to_float(float* out) const { return view().to_float(out); }
  bool to_float2(float* out) const { return view().to_float2(out); }
  bool to_float3(float* out) const { return view().to_float3(out); }
  bool to_float4(float* out) const { return view().to_float4(out); }
  bool* as_bool() { return view().as_bool() ? static_cast<bool*>(lu_value_mutable_data(&core_)) : nullptr; }
  int32_t* as_int() { return view().as_int() ? static_cast<int32_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  uint32_t* as_uint() { return view().as_uint() ? static_cast<uint32_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  int64_t* as_int64() { return view().as_int64() ? static_cast<int64_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  uint64_t* as_uint64() { return view().as_uint64() ? static_cast<uint64_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_float() { return view().as_float() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  uint8_t* as_uchar() { return view().as_uchar() ? static_cast<uint8_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_double() { return view().as_double() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  int32_t* as_int2() { return view().as_int2() ? static_cast<int32_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  int32_t* as_int3() { return view().as_int3() ? static_cast<int32_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  int32_t* as_int4() { return view().as_int4() ? static_cast<int32_t*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_float2() { return view().as_float2() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_float3() { return view().as_float3() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_float4() { return view().as_float4() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_double2() { return view().as_double2() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_double3() { return view().as_double3() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_double4() { return view().as_double4() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_matrix2f() { return view().as_matrix2f() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_matrix3f() { return view().as_matrix3f() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  float* as_matrix4f() { return view().as_matrix4f() ? static_cast<float*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_matrix2d() { return view().as_matrix2d() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_matrix3d() { return view().as_matrix3d() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  double* as_matrix4d() { return view().as_matrix4d() ? static_cast<double*>(lu_value_mutable_data(&core_)) : nullptr; }
  bool operator==(const Value& b) const { return lu_value_equal(&core_, &b.core_) != 0; }
  bool operator!=(const Value& b) const { return !(*this == b); }
 private:
  lu_value core_{};
};
struct DictEntryView { StringView first; ValueView second; };
class DictEntries {
 public:
  explicit DictEntries(const lu_value* p) : core_(p) {}
  size_t size() const { return lu_value_dict_size(core_); }
  bool empty() const { return !size(); }
  DictEntryView operator[](size_t i) const {
    const lu_view k = lu_value_dict_key(core_, i);
    return {{k.data, k.len}, ValueView(lu_value_dict_value(core_, i))};
  }
  struct Iterator {
    const lu_value* core; size_t index;
    DictEntryView operator*() const { return DictEntries(core)[index]; }
    Iterator& operator++() { ++index; return *this; }
    bool operator!=(const Iterator& b) const { return index != b.index; }
  };
  Iterator begin() const { return {core_, 0}; }
  Iterator end() const { return {core_, size()}; }
 private:
  const lu_value* core_;
};
class DictView {
 public:
  DictView() = default;
  DictView(std::nullptr_t) {}
  explicit DictView(const lu_value* p) : core_(p) {}
  explicit operator bool() const { return core_ && core_->kind == LU_VALUE_DICT; }
  const DictView* operator->() const { return this; }
  const DictView& operator*() const { return *this; }
  const lu_value* core() const { return core_; }
  size_t size() const { return lu_value_dict_size(core_); }
  bool empty() const { return !size(); }
  bool typed_json_wrapper() const { return lu_value_dict_typed(core_) != 0; }
  size_t dynamic_string_memory_usage() const { return lu_value_heap_bytes(core_); }
  ValueView find(StringView key) const { return ValueView(lu_value_dict_find(core_, key.data(), key.size())); }
  DictEntries entries() const { return DictEntries(core_); }
 protected:
  const lu_value* core_ = nullptr;
};
class MutableDictView : public DictView {
 public:
  MutableDictView() = default;
  explicit MutableDictView(lu_value* p) : DictView(p), mutable_(p) {}
  MutableDictView* operator->() { return this; }
  const MutableDictView* operator->() const { return this; }
  bool set(StringView key, ValueView value) const {
    return lu_value_dict_set(mutable_, key.data(), key.size(), value.core()) != 0;
  }
  bool set(StringView key, const Value& value) const { return set(key, value.view()); }
  bool set_typed_json_wrapper(bool flag) const { return lu_value_dict_set_typed(mutable_, flag) != 0; }
 private:
  lu_value* mutable_ = nullptr;
};
class Dict {
 public:
  Dict();
  ~Dict() = default;
  Dict(const Dict&) = default;
  Dict(Dict&&) noexcept = default;
  Dict& operator=(const Dict&) = default;
  Dict& operator=(Dict&&) noexcept = default;
  operator DictView() const { return value_.as_dictionary(); }
  operator MutableDictView() { return value_.as_dictionary(); }
  DictEntries entries() const { return value_.as_dictionary().entries(); }
  ValueView find(StringView key) const { return value_.as_dictionary().find(key); }
  bool set(StringView key, Value value) {
    if (!value_.is_dictionary()) value_ = Value::MakeDictionary();
    return value_.as_dictionary().set(key, value);
  }
  size_t size() const { return value_.as_dictionary().size(); }
  bool empty() const { return !size(); }
  bool typed_json_wrapper() const { return value_.as_dictionary().typed_json_wrapper(); }
  bool set_typed_json_wrapper(bool flag) { return value_.as_dictionary().set_typed_json_wrapper(flag); }
  size_t dynamic_string_memory_usage() const { return value_.heap_memory_usage(); }
 private:
  friend class Value;
  Value value_;
};
inline DictView Value::as_dictionary() const { return view().as_dictionary(); }
Value LerpValue(const Value&, const Value&, double);
}  // namespace lightusd
