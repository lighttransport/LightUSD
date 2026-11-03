// SPDX-License-Identifier: Apache-2.0
#include "value.hh"
#include "type-info.hh"
#include "interpolation.hh"
#include "../crate/lazy-array.hh"
#include <cstring>
#include <limits>
#include <new>
#include <ostream>

namespace lightusd { namespace next {

Value::~Value() { lu_value_destroy(&core_); }
Value::Value(const Value& other) { lu_value_copy(&core_, other.core()); }
Value::Value(Value&& other) noexcept { lu_value_move(&core_, other.mutable_core()); }
Value& Value::operator=(const Value& other) { lu_value_copy(&core_, other.core()); return *this; }
Value& Value::operator=(Value&& other) noexcept { lu_value_move(&core_, other.mutable_core()); return *this; }
Value Value::FromCore(const lu_value* p) { Value v; if (p) lu_value_copy(v.mutable_core(), p); return v; }
ValueView::ValueView(const Value* v) : core_(v ? v->core() : nullptr) {}
ValueView::ValueView(const Value& v) : core_(v.core()) {}
ValueView::operator Value() const { return Value::FromCore(core_); }
StringView::operator std::string() const { return std::string(empty() ? "" : data(), size()); }
static_assert(sizeof(StringView) == sizeof(std::string_view), "compact string views");
static_assert(sizeof(ValueArrayView<const float>) == sizeof(void*) + sizeof(size_t), "compact array views");
std::ostream& operator<<(std::ostream& out, StringView s) { return out.write(s.data(), static_cast<std::streamsize>(s.size())); }

#define SCALAR_CTOR(T, TYPE) Value::Value(T v) { lu_value_set_pod(&core_, LU_TYPE_##TYPE, &v, sizeof(v), nullptr); }
SCALAR_CTOR(bool, BOOL)
SCALAR_CTOR(int32_t, INT)
SCALAR_CTOR(uint32_t, UINT)
SCALAR_CTOR(int64_t, INT64)
SCALAR_CTOR(uint64_t, UINT64)
SCALAR_CTOR(float, FLOAT)
SCALAR_CTOR(double, DOUBLE)
#undef SCALAR_CTOR
Value::Value(const char* s) { const StringView text(s ? s : ""); lu_value_set_string(&core_, LU_TYPE_STRING, text.data(), text.size(), nullptr); }
Value::Value(const std::string& s) { lu_value_set_string(&core_, LU_TYPE_STRING, s.data(), s.size(), nullptr); }
Value::Value(std::string&& s) : Value(static_cast<const std::string&>(s)) {}
Value Value::MakeStringLike(StringView text, TypeId type) { Value v; lu_value_set_string(v.mutable_core(), static_cast<uint16_t>(type), text.data(), text.size(), nullptr); return v; }
Value Value::MakeToken(StringView text) { return MakeStringLike(text, TypeId::Token); }
Value Value::MakeAssetPath(StringView text) { return MakeStringLike(text, TypeId::AssetPath); }
Value Value::MakeDictionary() { Value v; lu_value_make_dict(v.mutable_core(), nullptr); return v; }
Value Value::MakeDictionary(Dict&& d) { return std::move(d.value_); }
Value Value::MakeBlock() { Value v; lu_value_init_block(v.mutable_core()); return v; }
Value Value::MakeFromRaw(TypeId type, const void* data) {
  Value v;
  if (!data) return v;
  if (lu_value_uses_string_storage(static_cast<uint16_t>(type))) return MakeStringLike(static_cast<const char*>(data), type);
  lu_value_set_pod(v.mutable_core(), static_cast<uint16_t>(type), data, GetTypeSize(type), nullptr);
  return v;
}
void Value::adopt_pod(const void* data, size_t size) { lu_value_set_pod(&core_, core_.type, data, size, nullptr); }
void Value::clear() { lu_value_destroy(&core_); }
bool Value::set_string(StringView text) { return lu_value_set_string(&core_, core_.type, text.data(), text.size(), nullptr) != 0; }
void Value::mark_dirty() { core_.flags |= LU_VALUE_MODIFIED; }
bool Value::resize_array(size_t size) { return lu_value_array_resize(&core_, size) != 0; }
void* Value::raw_data() { return materialize() ? lu_value_mutable_data(&core_) : nullptr; }
MutableDictView Value::as_dictionary() { return MutableDictView(is_dictionary() ? &core_ : nullptr); }

Value Value::MakeArrayFromRaw(TypeId type, const void* data, size_t count) {
  Value v; lu_value_set_array(v.mutable_core(), static_cast<uint16_t>(type), data, count, nullptr); return v;
}
Value Value::MakeEmptyArray(TypeId type, size_t count) {
  Value v; lu_value_array_create(v.mutable_core(), static_cast<uint16_t>(type), count, nullptr); return v;
}
Value Value::MakeStrings(TypeId type, size_t count, lu_string_at at, const void* context) {
  Value v; lu_value_set_strings(v.mutable_core(), static_cast<uint16_t>(type), count, at, context, nullptr); return v;
}
Value Value::MakeTokenArray(std::initializer_list<std::string> data) {
  const auto at = [](const void* p, size_t i, size_t* size) { const auto& s = static_cast<const std::string*>(p)[i]; *size = s.size(); return s.data(); };
  return MakeStrings(TypeId::Token, data.size(), at, data.begin());
}
Value Value::MakeBoolArray(std::initializer_list<bool> data) {
  Value v = MakeEmptyArray(TypeId::Bool, data.size());
  if (v.is_array()) {
    auto* out = const_cast<uint8_t*>(static_cast<const uint8_t*>(lu_value_data(v.core())));
    size_t i = 0; for (bool b : data) out[i++] = b ? 1 : 0;
  }
  return v;
}
#define ARRAY_FACTORY(NAME, T, TYPE, COMPS) \
Value Value::Make##NAME##Array(ValueArrayView<const T> data) { \
  return data.size() % COMPS ? Value() : MakeArrayFromRaw(TypeId::TYPE, data.data(), data.size() / COMPS); \
}
ARRAY_FACTORY(Float, float, Float, 1)
ARRAY_FACTORY(Int, int32_t, Int, 1)
ARRAY_FACTORY(Double, double, Double, 1)
ARRAY_FACTORY(Int64, int64_t, Int64, 1)
ARRAY_FACTORY(UInt, uint32_t, UInt, 1)
ARRAY_FACTORY(UInt64, uint64_t, UInt64, 1)
ARRAY_FACTORY(BoolByte, uint8_t, Bool, 1)
ARRAY_FACTORY(Float2, float, Float2, 2)
ARRAY_FACTORY(Float3, float, Float3, 3)
#undef ARRAY_FACTORY
#define COMPONENT_FACTORY(NAME, T, COMPONENT) \
Value Value::Make##NAME##CompArray(ValueArrayView<const T> data, TypeId type, uint32_t components) { \
  if (!components || data.size() % components || components != GetComponentCount(type) || \
      lu_value_array_component(static_cast<uint16_t>(type)) != LU_TYPE_##COMPONENT) return Value(); \
  return MakeArrayFromRaw(type, data.data(), data.size() / components); \
}
COMPONENT_FACTORY(Float, float, FLOAT)
COMPONENT_FACTORY(Double, double, DOUBLE)
COMPONENT_FACTORY(Int, int32_t, INT)
COMPONENT_FACTORY(UInt, uint32_t, UINT)
#undef COMPONENT_FACTORY

Value::DeferredArrayFill::~DeferredArrayFill() { Release(); }
Value::DeferredArrayFill::DeferredArrayFill(DeferredArrayFill&& b) noexcept
    : buffer_(b.buffer_), type_(b.type_), count_(b.count_) { b.buffer_ = nullptr; }
Value::DeferredArrayFill& Value::DeferredArrayFill::operator=(DeferredArrayFill&& b) noexcept {
  if (this != &b) { Release(); buffer_ = b.buffer_; type_ = b.type_; count_ = b.count_; b.buffer_ = nullptr; }
  return *this;
}
void Value::DeferredArrayFill::Release() { lu_buffer_release(buffer_); buffer_ = nullptr; }
Value Value::MakeDeferredArray(TypeId type, ArrayScalarKind kind, uint32_t count, DeferredArrayFill* out) {
  constexpr uint16_t components[] = {LU_TYPE_FLOAT, LU_TYPE_DOUBLE, LU_TYPE_INT, LU_TYPE_UINT, LU_TYPE_INT64, LU_TYPE_UINT64, LU_TYPE_BOOL};
  if (!out || static_cast<unsigned>(kind) >= 7 || lu_value_array_component(static_cast<uint16_t>(type)) != components[static_cast<unsigned>(kind)]) return Value();
  Value v = MakeEmptyArray(type, 0);
  if (!v.is_array()) return v;
  v.core_.size = count;
  out->Release(); out->buffer_ = lu_value_buffer(v.core()); lu_buffer_retain(out->buffer_);
  out->type_ = type; out->count_ = count;
  return v;
}
bool Value::DeferredArrayFill::Complete(Value&& parsed) {
  if (!buffer_ || !parsed.is_array() || parsed.is_lazy() || parsed.is_dirty() || parsed.type_id() != type_ || parsed.array_size() != count_) return false;
  return lu_buffer_publish_deferred(buffer_, lu_value_buffer(parsed.core())) != 0;
}

namespace {
int DecodeLazy(const void* data, uint16_t type, uint32_t count, lu_value* out) {
  const auto& ref = *static_cast<const LazyArrayRef*>(data);
  Value decoded;
  if (!ref.source || !ref.source->MaterializeArray(ref, &decoded) || decoded.is_lazy() || !decoded.is_array() || decoded.array_size() != count) return 0;
  decoded.retag_role(static_cast<TypeId>(type));
  if (decoded.type_id() != static_cast<TypeId>(type)) return 0;
  lu_value_move(out, decoded.mutable_core()); return 1;
}
void DestroyLazy(void* p) { delete static_cast<LazyArrayRef*>(p); }
void* CloneLazy(const void* p) { return new (std::nothrow) LazyArrayRef(*static_cast<const LazyArrayRef*>(p)); }
}
Value Value::MakeLazyArray(const LazyArrayRef& ref) {
  Value v;
  if (!ref.source || !lu_value_is_array_type(static_cast<uint16_t>(ref.value_type)) || ref.element_count > UINT32_MAX) return v;
  auto* context = new (std::nothrow) LazyArrayRef(ref);
  if (!context) return v;
  const lu_lazy_callbacks callbacks{DecodeLazy, DestroyLazy, CloneLazy};
  if (!lu_value_set_lazy(v.mutable_core(), static_cast<uint16_t>(ref.value_type), static_cast<uint32_t>(ref.element_count), context, &callbacks, nullptr)) delete context;
  return v;
}
bool Value::materialize() { return lu_value_materialize(&core_) != 0; }
void Value::retag_role(TypeId type) {
  if (type == type_id() || type == TypeId::Invalid || is_empty() || is_block()) return;
  const auto component = [](TypeId t) {
    if (t == TypeId::UChar) return TypeId::UInt;
    return static_cast<TypeId>(lu_scalar_component(static_cast<uint16_t>(t)));
  };
  const auto a = component(type_id()), b = component(type);
  if (GetComponentCount(type_id()) != GetComponentCount(type)) return;
  if (a != b) {
    if (!((a == TypeId::Int && b == TypeId::UInt) || (a == TypeId::UInt && b == TypeId::Int)) || is_lazy()) return;
  }
  if (!is_array() && GetTypeSize(type_id()) != GetTypeSize(type)) return;
  if (is_lazy()) {
    auto* ref = static_cast<LazyArrayRef*>(lu_value_mutable_lazy_context(&core_));
    if (!ref) return;
    ref->value_type = type;
  }
  core_.type = static_cast<uint16_t>(type);
}

TypeId ValueView::type_id() const { return core_ ? static_cast<TypeId>(core_->type) : TypeId::Invalid; }
bool ValueView::is_empty() const { return !core_ || core_->kind == LU_VALUE_EMPTY; }
bool ValueView::is_block() const { return core_ && core_->kind == LU_VALUE_BLOCK; }
bool ValueView::is_array() const { return core_ && (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY); }
bool ValueView::is_dictionary() const { return core_ && core_->kind == LU_VALUE_DICT; }
bool ValueView::is_lazy() const { return core_ && core_->kind == LU_VALUE_LAZY_ARRAY; }
bool ValueView::is_dirty() const { return core_ && (core_->flags & LU_VALUE_MODIFIED); }
size_t ValueView::array_size() const { return is_array() ? core_->size : 0; }
size_t ValueView::heap_memory_usage() const { return lu_value_heap_bytes(core_); }
size_t ValueView::dynamic_string_memory_usage() const {
  return is_dictionary() || (core_ && lu_value_uses_string_storage(core_->type)) ? heap_memory_usage() : 0;
}
const LazyArrayRef* ValueView::lazy_ref() const { return static_cast<const LazyArrayRef*>(lu_value_lazy_context(core_)); }
bool ValueView::materialize() const { return core_ && lu_value_materialize(const_cast<lu_value*>(core_)); }
Value ValueView::materialized_copy() const { Value v = Value::FromCore(core_); return v.materialize() ? std::move(v) : Value(); }
const void* ValueView::raw_data() const { return materialize() ? lu_value_data(core_) : nullptr; }
const uint8_t* ValueView::raw_bytes(size_t* size) const {
  if (!size) return nullptr;
  *size = 0;
  if (!materialize()) return nullptr;
  *size = lu_value_bytes(core_); return static_cast<const uint8_t*>(lu_value_data(core_));
}
uint64_t ValueView::hash() const { return lu_value_hash(core_); }
StringView ValueView::as_string() const { const lu_view s = core_ && (type_id() == TypeId::String || type_id() == TypeId::PathExpression) ? lu_value_string(core_) : lu_view{nullptr, 0}; return {s.data, s.len}; }
StringView ValueView::as_token() const { const lu_view s = type_id() == TypeId::Token ? lu_value_string(core_) : lu_view{nullptr, 0}; return {s.data, s.len}; }
StringView ValueView::as_asset_path() const { const lu_view s = type_id() == TypeId::AssetPath ? lu_value_string(core_) : lu_view{nullptr, 0}; return {s.data, s.len}; }
DictView ValueView::as_dictionary() const { return DictView(is_dictionary() ? core_ : nullptr); }
StringArrayView ValueView::as_token_array() const {
  if (!is_array() || !lu_value_uses_string_storage(core_->type)) return {};
  return materialize() && core_->kind == LU_VALUE_ARRAY ? StringArrayView(core_) : StringArrayView();
}
#define ARRAY_READER(NAME, T, COMPONENT) \
ValueArrayView<const T> ValueView::as_##NAME##_array() const { \
  if (!is_array() || lu_value_array_component(core_->type) != LU_TYPE_##COMPONENT || !materialize() || core_->kind != LU_VALUE_ARRAY) return {}; \
  return {static_cast<const T*>(lu_value_data(core_)), lu_value_array_scalar_count(core_)}; \
}
ARRAY_READER(float, float, FLOAT)
ARRAY_READER(double, double, DOUBLE)
ARRAY_READER(int, int32_t, INT)
ARRAY_READER(uint, uint32_t, UINT)
ARRAY_READER(int64, int64_t, INT64)
ARRAY_READER(uint64, uint64_t, UINT64)
ARRAY_READER(bool, uint8_t, BOOL)
#undef ARRAY_READER
#define ARRAY_WRITER(NAME, T) \
ValueArrayView<T> Value::as_##NAME##_array() { \
  const auto present = view().as_##NAME##_array(); \
  if (!present) return {}; \
  auto* data = static_cast<T*>(lu_value_mutable_data(&core_)); \
  return data ? ValueArrayView<T>(data, present.size()) : ValueArrayView<T>(); \
}
ARRAY_WRITER(float, float)
ARRAY_WRITER(double, double)
ARRAY_WRITER(int, int32_t)
ARRAY_WRITER(uint, uint32_t)
ARRAY_WRITER(int64, int64_t)
ARRAY_WRITER(uint64, uint64_t)
#undef ARRAY_WRITER
Value LerpValue(const Value& a, const Value& b, double t) {
  if (a.type_id() != b.type_id()) return a;
  if (a.is_array() || TimeInterpolator::IsLinearInterpolatable(a.type_id())) {
    Value r = TimeInterpolator::InterpolateValues(a, b, t);
    return r.is_empty() ? a : r;
  }
  return a;
}
}}  // namespace lightusd::next
