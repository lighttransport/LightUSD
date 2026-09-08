// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value class with Small Buffer Optimization
// Replaces linb::any with a type-aware, USD-specific value container

#pragma once

#include "type-id.hh"
#include <cstddef>
#include <string>
#include "../core/string-index.hh"
#include <vector>
#include <utility>

namespace lightusd {
namespace next {

struct LazyArrayRef;  // crate/lazy-array.hh
struct Dict;          // recursive USD dictionary (defined below)

/// Value class - type-erased container for USD values
/// Uses Small Buffer Optimization to avoid heap allocation for small types
/// All USD scalar and vector types fit in the inline buffer
class Value {
public:
  /// Small buffer size - exactly the largest inline USD scalar (matrix4d,
  /// 16 doubles). String/array/dictionary storage uses smaller handles. Keeping
  /// this at 128 instead of 136 avoids alignment rounding Value from 144 to 160
  /// bytes, saving 10% in the dense per-prim value pools.
  static constexpr size_t kSBOSize = 128;

  // ============================================================
  // Constructors and assignment
  // ============================================================

  /// Default constructor - creates empty value
  Value();

  /// Destructor
  ~Value();

  /// Copy constructor
  Value(const Value& other);

  /// Move constructor
  Value(Value&& other) noexcept;

  /// Copy assignment
  Value& operator=(const Value& other);

  /// Move assignment
  Value& operator=(Value&& other) noexcept;

  // ============================================================
  // Type-specific constructors (no templates in public API)
  // ============================================================

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

  // ============================================================
  // Factory functions for compound types
  // ============================================================

  static Value MakeInt2(int32_t x, int32_t y);
  static Value MakeInt3(int32_t x, int32_t y, int32_t z);
  static Value MakeInt4(int32_t x, int32_t y, int32_t z, int32_t w);

  static Value MakeFloat2(float x, float y);
  static Value MakeFloat3(float x, float y, float z);
  static Value MakeFloat4(float x, float y, float z, float w);

  static Value MakeDouble2(double x, double y);
  static Value MakeDouble3(double x, double y, double z);
  static Value MakeDouble4(double x, double y, double z, double w);

  static Value MakeQuatf(float x, float y, float z, float w);
  static Value MakeQuatd(double x, double y, double z, double w);

  static Value MakeMatrix2f(const float* data);   // 4 floats
  static Value MakeMatrix3f(const float* data);   // 9 floats
  static Value MakeMatrix4f(const float* data);   // 16 floats
  static Value MakeMatrix2d(const double* data);  // 4 doubles
  static Value MakeMatrix3d(const double* data);  // 9 doubles
  static Value MakeMatrix4d(const double* data);  // 16 doubles

  static Value MakeToken(const std::string& s);
  static Value MakeToken(std::string&& s);
  static Value MakeAssetPath(const std::string& s);
  static Value MakeAssetPath(std::string&& s);

  // Dictionary (recursive key->Value map; insertion-ordered for round-trip).
  // Stored behind a shared_ptr<Dict> in the SBO with copy-on-write, so copying
  // a dict-valued Value bumps a refcount and never deep-copies until mutated.
  static Value MakeDictionary();
  static Value MakeDictionary(Dict&& d);

  // Semantic type factories (same storage as vectors)
  static Value MakePoint3f(float x, float y, float z);
  static Value MakePoint3d(double x, double y, double z);
  static Value MakeVector3f(float x, float y, float z);
  static Value MakeVector3d(double x, double y, double z);
  static Value MakeNormal3f(float x, float y, float z);
  static Value MakeNormal3d(double x, double y, double z);
  static Value MakeColor3f(float r, float g, float b);
  static Value MakeColor4f(float r, float g, float b, float a);
  static Value MakeTexcoord2f(float u, float v);

  /// Create from raw type ID and data pointer
  static Value MakeFromRaw(TypeId type_id, const void* data);

  // ============================================================
  // Array constructors
  // ============================================================

  /// Create array value from vector of floats
  static Value MakeFloatArray(const std::vector<float>& data);
  static Value MakeFloatArray(std::vector<float>&& data);

  /// Create array value from vector of ints
  static Value MakeIntArray(const std::vector<int32_t>& data);
  static Value MakeIntArray(std::vector<int32_t>&& data);

  /// Create array of float2 from flat data (length must be multiple of 2)
  static Value MakeFloat2Array(const std::vector<float>& data);
  static Value MakeFloat2Array(std::vector<float>&& data);

  /// Create array of float3 from flat data (length must be multiple of 3)
  static Value MakeFloat3Array(const std::vector<float>& data);
  static Value MakeFloat3Array(std::vector<float>&& data);

  /// New array types
  static Value MakeDoubleArray(const std::vector<double>& data);
  static Value MakeDoubleArray(std::vector<double>&& data);
  static Value MakeInt64Array(const std::vector<int64_t>& data);
  static Value MakeInt64Array(std::vector<int64_t>&& data);
  static Value MakeUIntArray(const std::vector<uint32_t>& data);
  static Value MakeUIntArray(std::vector<uint32_t>&& data);
  static Value MakeUInt64Array(const std::vector<uint64_t>& data);
  static Value MakeUInt64Array(std::vector<uint64_t>&& data);
  static Value MakeBoolArray(const std::vector<bool>& data);
  static Value MakeTokenArray(const std::vector<std::string>& data);
  static Value MakeTokenArray(std::vector<std::string>&& data);

  /// Generic flat-buffer array factories for vector/quat/matrix element types
  /// (e.g. Vec4f, Matrix4d, Quatf). `data` is the flat scalar buffer and
  /// `comps_per_elem` the number of scalars per element (4 for Vec4f / Quatf,
  /// 16 for Matrix4d, ...). array_size == data.size() / comps_per_elem.
  static Value MakeFloatCompArray(std::vector<float>&& data, TypeId elem_type,
                                  uint32_t comps_per_elem);
  static Value MakeDoubleCompArray(std::vector<double>&& data, TypeId elem_type,
                                   uint32_t comps_per_elem);
  /// Int-vector element types (Int2/Int3/Int4): flat int32 buffer.
  static Value MakeIntCompArray(std::vector<int32_t>&& data, TypeId elem_type,
                                uint32_t comps_per_elem);
  /// UInt-vector element types (UInt2/UInt3/UInt4): flat uint32 buffer.
  static Value MakeUIntCompArray(std::vector<uint32_t>&& data, TypeId elem_type,
                                 uint32_t comps_per_elem);
  /// String-family arrays with an explicit element type (Token / String /
  /// AssetPath); same storage as MakeTokenArray.
  static Value MakeStringLikeArray(std::vector<std::string>&& data,
                                   TypeId elem_type);
  /// Scalar string-family value with an explicit type (Token / String /
  /// AssetPath / PathExpression).
  static Value MakeStringLike(const std::string& s, TypeId type);

  // ============================================================
  // Type queries
  // ============================================================

  /// Get the type ID
  TypeId type_id() const { return type_id_; }

  /// Re-tag the semantic (role) type without touching storage — e.g.
  /// Float2 -> Texcoord2f after a crate read, where roles exist only in the
  /// declared type name. No-op unless the two types share a storage layout.
  void retag_role(TypeId new_type);

  /// Check if empty (no value stored). A value BLOCK (`= None`) is NOT empty:
  /// it is an authored opinion that blocks weaker values and must round-trip.
  bool is_empty() const { return type_id_ == TypeId::Invalid && !is_block_; }

  /// True if this is an authored value block (`= None`) — a typed opinion that
  /// carries no data but suppresses weaker opinions and emits `= None`.
  bool is_block() const { return is_block_; }

  /// Check if this is an array value
  bool is_array() const { return is_array_; }

  /// Check if this is a dictionary value
  bool is_dictionary() const { return type_id_ == TypeId::Dictionary; }

  /// Get array size (0 if not an array)
  size_t array_size() const;

  /// Clear the value (becomes empty)
  void clear();

  // ============================================================
  // Lazy array references (crate-backed, undecoded)
  // ============================================================

  /// Construct an authored value block (`= None`): no data, no type, but a real
  /// opinion (is_empty()==false) that blocks weaker values and re-emits `= None`.
  /// The attribute's declared type comes from the slot / property_type_name.
  static Value MakeBlock();

  /// Construct a lazy array value that references an undecoded block inside a
  /// retained CrateDataSource. The payload is decoded on first access.
  static Value MakeLazyArray(const LazyArrayRef& ref);

  /// True if this is an array whose payload has not been decoded yet.
  bool is_lazy() const { return is_lazy_; }

  /// True if the value was materialized AND potentially mutated (so write-time
  /// byte pass-through is no longer safe).
  bool is_dirty() const { return dirty_; }

  /// Mark the value as edited (forces re-encode on write).
  void mark_dirty() { dirty_ = true; }

  /// Access the lazy reference (nullptr unless is_lazy()).
  const LazyArrayRef* lazy_ref() const;

  /// Decode a lazy array into concrete storage (idempotent; no-op if not lazy).
  void materialize();

  /// Decode a lazy array into a fresh temporary Value WITHOUT mutating *this
  /// (the source Value stays lazy). For non-lazy values, returns an ordinary
  /// copy-on-write copy (preserves dirty_). On decode failure, returns an empty
  /// Value (prints as "None", matching in-place materialize() failure). Used by
  /// the writer to format a lazy array once and free it immediately, so peak
  /// resident decoded-array memory is bounded to ~one array.
  Value materialized_copy() const;

  // ============================================================
  // Type-safe accessors (return nullptr if wrong type)
  // ============================================================

  const bool* as_bool() const;
  const int32_t* as_int() const;
  const uint32_t* as_uint() const;
  const int64_t* as_int64() const;
  const uint64_t* as_uint64() const;
  const float* as_float() const;
  const double* as_double() const;
  const uint8_t* as_uchar() const;
  const std::string* as_string() const;

  // Mutable accessors
  bool* as_bool();
  int32_t* as_int();
  uint32_t* as_uint();
  int64_t* as_int64();
  uint64_t* as_uint64();
  float* as_float();
  double* as_double();
  uint8_t* as_uchar();
  std::string* as_string();

  // Vector accessors (return pointer to first element)
  const int32_t* as_int2() const;
  const int32_t* as_int3() const;
  const int32_t* as_int4() const;
  const float* as_float2() const;
  const float* as_float3() const;
  const float* as_float4() const;
  const double* as_double2() const;
  const double* as_double3() const;
  const double* as_double4() const;

  /// Converting scalar reads: also widen raw-half SBO scalars (authored
  /// half/half2/half3/half4 and their role types store half-bit lanes that
  /// as_float*() cannot see) and narrow double-backed values. Lanes are
  /// copied in storage order (quats stay real-first like as_float4()).
  /// Return false when the value is not a matching-arity scalar.
  bool to_float(float* out) const;
  bool to_float2(float* out) const;   // out[2]
  bool to_float3(float* out) const;   // out[3]
  bool to_float4(float* out) const;   // out[4]

  // Matrix accessors (return pointer to first element)
  const float* as_matrix2f() const;
  const float* as_matrix3f() const;
  const float* as_matrix4f() const;
  const double* as_matrix2d() const;
  const double* as_matrix3d() const;
  const double* as_matrix4d() const;

  // Token and AssetPath as string
  const std::string* as_token() const;
  const std::string* as_asset_path() const;

  // Dictionary access (nullptr if not a dictionary). The mutable accessor
  // copy-on-write-detaches before returning a writable view.
  const Dict* as_dictionary() const;
  Dict* as_dictionary();

  // Array accessors
  const std::vector<float>* as_float_array() const;
  const std::vector<int32_t>* as_int_array() const;
  const std::vector<double>* as_double_array() const;
  const std::vector<int64_t>* as_int64_array() const;
  const std::vector<uint32_t>* as_uint_array() const;
  const std::vector<uint64_t>* as_uint64_array() const;
  const std::vector<uint8_t>* as_bool_array() const;   // 0/1 values
  const std::vector<std::string>* as_token_array() const;
  std::vector<float>* as_float_array();
  std::vector<int32_t>* as_int_array();
  std::vector<double>* as_double_array();
  std::vector<int64_t>* as_int64_array();
  std::vector<uint32_t>* as_uint_array();
  std::vector<uint64_t>* as_uint64_array();

  // ============================================================
  // Raw data access
  // ============================================================

  /// Get raw pointer to data (use with caution)
  const void* raw_data() const;
  void* raw_data();

  // ============================================================
  // Comparison
  // ============================================================

  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }

  // ============================================================
  // Hashing (for deduplication)
  // ============================================================

  /// Compute hash of the value (for deduplication)
  /// Returns 0 for empty values
  uint64_t hash() const;

  /// Get raw bytes for comparison (for array deduplication)
  /// Returns nullptr and size=0 if not applicable
  const uint8_t* raw_bytes(size_t* out_size) const;

private:
  // Shared implementation for the to_float* converting reads.
  bool ToFloatLanes(int lanes, float* out) const;

  TypeId type_id_ = TypeId::Invalid;
  bool is_array_ = false;
  bool is_lazy_ = false;  // array payload not decoded; storage_ holds LazyArrayRef*
  bool dirty_ = false;    // materialized and possibly mutated (no byte pass-through)
  bool is_block_ = false; // authored `= None` block: no data, but not is_empty()
  uint32_t array_size_ = 0;

  // Storage - either inline or heap-allocated
  alignas(16) char storage_[kSBOSize];

  // Helper functions
  bool uses_heap() const;
  void copy_from(const Value& other);
  void move_from(Value&& other) noexcept;
  void destroy();

  // Decode a lazy value in place if needed (logical const: lazy load).
  void ensure_materialized() const;

  void* data_ptr();
  const void* data_ptr() const;
};

/// Recursive USD dictionary. Entries are kept in insertion order so the writer
/// re-emits them exactly as authored. A value may itself be a Dictionary.
struct Dict {
  const std::vector<std::pair<std::string, Value>>& entries() const { return entries_; }
  Dict();
  ~Dict();
  Dict(const Dict& other);
  Dict(Dict&& other) noexcept;
  Dict& operator=(const Dict& other);
  Dict& operator=(Dict&& other) noexcept;

  const Value* find(const std::string& key) const;
  Value* find(const std::string& key);
  /// Replace in place, or append while preserving insertion order.
  void set(std::string key, Value v);
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
 private:
  friend class Value;  // Iterative destruction moves out nested Values.
  std::vector<std::pair<std::string, Value>> entries_;
  // Keys live only in entries_. Index allocation failure uses a correct linear
  // fallback; it cannot discard an authored value. Keys change only via set().
  detail::StringIndex index_;
};

/// Linearly interpolate between two values for time-sample evaluation.
/// `t` is the fraction in [0,1] from `a` to `b`. Interpolatable numeric scalar,
/// vector, and matrix types are component-wise lerped; non-interpolatable types
/// (int/bool/string/token/asset/quaternion) and type mismatches fall back to
/// held interpolation (returns `a`).
Value LerpValue(const Value& a, const Value& b, double t);

}  // namespace next
}  // namespace lightusd
