// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDC(CrateFile) format
#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <cstdlib>

#include "core/prim-enums.hh"        // Specifier, Permission, Variability, SpecType, ListEditQual
#include "core/path.hh"              // Path
#include "core/composition-types.hh" // Reference, Payload, LayerOffset
#include "core/meta-variable.hh"     // MetaVariable, CustomDataType, VariantSelectionMap
#include "core/list-op.hh"           // ListOp
#include "value-types.hh"
#include "timesamples.hh"  // value::TimeSamples / TypeTraits<TimeSamples> (value-types.hh no longer pulls timesamples.hh transitively)
#include "typed-array-core.hh"
#include "mmap-array-ref.hh"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "nonstd/expected.hpp"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace crate {

constexpr size_t kMinCompressedArraySize = 16;
constexpr size_t kSectionNameMaxLength = 15;

constexpr char kTokenVector[] = "TokenVector";
constexpr char kStringVector[] = "StringVector";
constexpr char kPathVector[] = "PathVector";

// crate data type
// id must be identitical to <pxrUSD>/pxr/usd/usd/crateDataType.h
enum class CrateDataTypeId {
  CRATE_DATA_TYPE_INVALID = 0,

  CRATE_DATA_TYPE_BOOL = 1,
  CRATE_DATA_TYPE_UCHAR = 2,
  CRATE_DATA_TYPE_INT = 3,
  CRATE_DATA_TYPE_UINT = 4,
  CRATE_DATA_TYPE_INT64 = 5,
  CRATE_DATA_TYPE_UINT64 = 6,

  CRATE_DATA_TYPE_HALF = 7,
  CRATE_DATA_TYPE_FLOAT = 8,
  CRATE_DATA_TYPE_DOUBLE = 9,

  CRATE_DATA_TYPE_STRING = 10,
  CRATE_DATA_TYPE_TOKEN = 11,
  CRATE_DATA_TYPE_ASSET_PATH = 12,

  CRATE_DATA_TYPE_MATRIX2D = 13,
  CRATE_DATA_TYPE_MATRIX3D = 14,
  CRATE_DATA_TYPE_MATRIX4D = 15,

  CRATE_DATA_TYPE_QUATD = 16,
  CRATE_DATA_TYPE_QUATF = 17,
  CRATE_DATA_TYPE_QUATH = 18,

  CRATE_DATA_TYPE_VEC2D = 19,
  CRATE_DATA_TYPE_VEC2F = 20,
  CRATE_DATA_TYPE_VEC2H = 21,
  CRATE_DATA_TYPE_VEC2I = 22,

  CRATE_DATA_TYPE_VEC3D = 23,
  CRATE_DATA_TYPE_VEC3F = 24,
  CRATE_DATA_TYPE_VEC3H = 25,
  CRATE_DATA_TYPE_VEC3I = 26,

  CRATE_DATA_TYPE_VEC4D = 27,
  CRATE_DATA_TYPE_VEC4F = 28,
  CRATE_DATA_TYPE_VEC4H = 29,
  CRATE_DATA_TYPE_VEC4I = 30,

  CRATE_DATA_TYPE_DICTIONARY = 31,
  CRATE_DATA_TYPE_TOKEN_LIST_OP = 32,
  CRATE_DATA_TYPE_STRING_LIST_OP = 33,
  CRATE_DATA_TYPE_PATH_LIST_OP = 34,
  CRATE_DATA_TYPE_REFERENCE_LIST_OP = 35,
  CRATE_DATA_TYPE_INT_LIST_OP = 36,
  CRATE_DATA_TYPE_INT64_LIST_OP = 37,
  CRATE_DATA_TYPE_UINT_LIST_OP = 38,
  CRATE_DATA_TYPE_UINT64_LIST_OP = 39,

  CRATE_DATA_TYPE_PATH_VECTOR = 40,
  CRATE_DATA_TYPE_TOKEN_VECTOR = 41,

  CRATE_DATA_TYPE_SPECIFIER = 42,
  CRATE_DATA_TYPE_PERMISSION = 43,
  CRATE_DATA_TYPE_VARIABILITY = 44,

  CRATE_DATA_TYPE_VARIANT_SELECTION_MAP = 45,
  CRATE_DATA_TYPE_TIME_SAMPLES = 46,
  CRATE_DATA_TYPE_PAYLOAD = 47,
  CRATE_DATA_TYPE_DOUBLE_VECTOR = 48,
  CRATE_DATA_TYPE_LAYER_OFFSET_VECTOR = 49,
  CRATE_DATA_TYPE_STRING_VECTOR = 50,
  CRATE_DATA_TYPE_VALUE_BLOCK = 51,
  CRATE_DATA_TYPE_VALUE = 52, // Contains ValueRep
  CRATE_DATA_TYPE_UNREGISTERED_VALUE = 53, // String or Dict
  CRATE_DATA_TYPE_UNREGISTERED_VALUE_LIST_OP = 54,
  CRATE_DATA_TYPE_PAYLOAD_LIST_OP = 55,
  CRATE_DATA_TYPE_TIME_CODE = 56,

  // Added in newer crate versions. See <pxrUSD>/pxr/usd/sdf/crateDataTypes.h
  CRATE_DATA_TYPE_PATH_EXPRESSION = 57, // SdfPathExpression (crate >= 0.10.0)
  CRATE_DATA_TYPE_RELOCATES = 58, // SdfRelocates (crate >= 0.11.0)
  CRATE_DATA_TYPE_SPLINE = 59, // TsSpline (crate >= 0.12.0)
  CRATE_DATA_TYPE_ANIMATION_BLOCK = 60, // SdfAnimationBlock (no version bump)

  NumDataTypes // terminator
};

class CrateDataType
{
 public:
  CrateDataType() = default;

  CrateDataType(const char *s, CrateDataTypeId did, bool a)
    : name(s), dtype_id(did), supports_array(a) {
  }

  CrateDataType(const CrateDataType &rhs) = default;
  CrateDataType &operator=(const CrateDataType&rhs) = default;

  const char *name{nullptr}; // name of CrateDatatType. Constant symbol. TODO: Use string_view.
  CrateDataTypeId dtype_id{CrateDataTypeId::CRATE_DATA_TYPE_INVALID};
  bool supports_array{false};
};

std::string GetCrateDataTypeRepr(CrateDataType dty); // for debug cout

nonstd::expected<CrateDataType, std::string> GetCrateDataType(int32_t type_id);

/// Cheap "does this crate type id name a known type" query for the per-value
/// hot loops in the reader. Equivalent to GetCrateDataType(type_id).has_value()
/// but does not build the CrateDataType record (name + supports_array), which
/// is the only thing those loops do not need from the type table.
inline bool IsKnownCrateDataTypeId(int32_t type_id) {
  // The eternal map in GetCrateDataType() covers every enumerator, so the id
  // range check is exact: 0 (INVALID) .. NumDataTypes-1.
  return type_id >= 0 &&
         type_id < static_cast<int32_t>(CrateDataTypeId::NumDataTypes);
}
std::string GetCrateDataTypeName(int32_t type_id);
std::string GetCrateDataTypeName(CrateDataTypeId type_id);



// -- from USD ----------------------------------------------------------------

//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.

// Index base class.  Used to index various tables.  Deriving adds some
// type-safety so we don't accidentally use one kind of index with the wrong
// kind of table.
struct Index {
  Index() : value(~0u) {}
  explicit Index(uint32_t v) : value(v) {}
  bool operator==(const Index &other) const { return value == other.value; }
  bool operator!=(const Index &other) const { return !(*this == other); }
  bool operator<(const Index &other) const { return value < other.value; }
  uint32_t value;
};

// Value in file representation.  Consists of a 2 bytes of type information
// (type enum value, array bit, and inlined-value bit) and 6 bytes of data.
// If possible, we attempt to store certain values directly in the local
// data, such as ints, floats, enums, and special-case values of other types
// (zero vectors, identity matrices, etc).  For values that aren't stored
// inline, the 6 data bytes are the offset from the start of the file to the
// value's location.
struct ValueRep {
  friend class CrateFile;

  constexpr ValueRep() : data(0) {}

  explicit constexpr ValueRep(uint64_t d) : data(d) {}

  constexpr ValueRep(int32_t t, bool isInlined, bool isArray, uint64_t payload)
      : data(Combine(t, isInlined, isArray, payload)) {}

  static const uint64_t IsArrayBit_ = 1ull << 63;
  static const uint64_t IsInlinedBit_ = 1ull << 62;
  static const uint64_t IsCompressedBit_ = 1ull << 61;
  // VtArrayEdit flag (crate >= 0.14.0). Independent of IsArrayBit_; the type
  // byte holds the array element type, same as a normal array.
  static const uint64_t IsArrayEditBit_ = 1ull << 60;

  static const uint64_t PayloadMask_ = ((1ull << 48) - 1);

  inline bool IsArray() const { return data & IsArrayBit_; }
  inline void SetIsArray() { data |= IsArrayBit_; }

  inline bool IsInlined() const { return data & IsInlinedBit_; }
  inline void SetIsInlined() { data |= IsInlinedBit_; }

  inline bool IsCompressed() const { return data & IsCompressedBit_; }
  inline void SetIsCompressed() { data |= IsCompressedBit_; }

  inline bool IsArrayEdit() const { return data & IsArrayEditBit_; }
  inline void SetIsArrayEdit() { data |= IsArrayEditBit_; }

  inline int32_t GetType() const {
    return static_cast<int32_t>((data >> 48) & 0xFF);
  }
  inline void SetType(int32_t t) {
    data &= ~(0xFFull << 48);                  // clear type byte in data.
    data |= (static_cast<uint64_t>(t) << 48);  // set it.
  }

  inline uint64_t GetPayload() const { return data & PayloadMask_; }

  inline void SetPayload(uint64_t payload) {
    data &= ~PayloadMask_;  // clear existing payload.
    data |= payload & PayloadMask_;
  }

  inline uint64_t GetData() const { return data; }

  bool operator==(ValueRep other) const { return data == other.data; }
  bool operator!=(ValueRep other) const { return !(*this == other); }

  // Hash function for use with unordered_map
  struct Hash {
    size_t operator()(const ValueRep& v) const {
      return std::hash<uint64_t>{}(v.data);
    }
  };

  std::string GetStringRepr() const {
    std::stringstream ss;
    ss << "ty: " << static_cast<int>(GetType()) << "(" << GetCrateDataTypeName(GetType()) << "), isArray: " << IsArray()
       << ", isInlined: " << IsInlined() << ", isCompressed: " << IsCompressed()
       << ", isArrayEdit: " << IsArrayEdit()
       << ", payload: " << GetPayload();

    return ss.str();
  }

 private:
  static constexpr uint64_t Combine(int32_t t, bool isInlined, bool isArray,
                                     uint64_t payload) {
    return (isArray ? IsArrayBit_ : 0) | (isInlined ? IsInlinedBit_ : 0) |
           (static_cast<uint64_t>(t) << 48) | (payload & PayloadMask_);
  }

  uint64_t data;
};

inline std::string to_string(const ValueRep &rep) {
  return rep.GetStringRepr();
}

struct TokenIndex : Index { using Index::Index; };
struct StringIndex : Index { using Index::Index; };
struct FieldIndex : Index { using Index::Index; };
struct FieldSetIndex : Index { using Index::Index; };
struct PathIndex : Index { using Index::Index; };

// ----------------------------------------------------------------------------


struct Field {
  TokenIndex token_index;
  ValueRep value_rep;
};

//
// Spec describes the relation of a path(i.e. node) and field(e.g. vertex data)
//
struct Spec {
  Index path_index;
  Index fieldset_index;
  SpecType spec_type; // Must be 32bit
};

static_assert(sizeof(Spec) == (4 * 3), "sizeof(Spec) must be 12");

struct Section {
  Section() { memset(this, 0, sizeof(*this)); }
  Section(char const *name, int64_t start, int64_t size);
  char name[kSectionNameMaxLength + 1];
  int64_t start, size;  // byte offset to section info and its data size
};

// For unordered_map

// https://stackoverflow.com/questions/8513911/how-to-create-a-good-hash-combine-with-64-bit-output-inspired-by-boosthash-co
// From CityHash code.

template <class T>
inline void hash_combine_impl32(std::size_t &seed, const T &v)
{
  // Use boost version.
  std::hash<T> hasher;
  seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T>
inline void hash_combine(std::size_t &seed, const T &v) {
#if defined(__wasi__)  // 32bit platform
  hash_combine_impl32(seed, v);
#else
  if (sizeof(std::size_t) == 4) {
    hash_combine_impl32(seed, v);
  } else {
    // Assume 64bit
    std::hash<T> hasher;
    const uint64_t kMul = 0x9ddfea08eb382d69ULL;
    uint64_t a = (hasher(v) ^ seed) * kMul;
    a ^= (a >> 47);
    uint64_t b = (seed ^ a) * kMul;
    b ^= (b >> 47);
    seed = size_t(b * kMul);
  }
#endif
}

struct PathHasher {
  size_t operator()(const Path &path) const {
    size_t seed = std::hash<lightusd::tstring_view>()(path.prim_part());
    hash_combine(seed, std::hash<lightusd::tstring_view>()(path.prop_part()));
    hash_combine(seed, std::hash<bool>()(path.is_valid()));
    // Variant parts: typically empty for spec paths, but mixing them in
    // makes the hasher robust if a variant-bearing path is ever passed.
    // Uses the private _variant_part / _variant_selection_part directly
    // (Path declares these structs as friends) to avoid the per-call
    // std::string allocation in variant_part(). See concern 5 in review.md.
    hash_combine(seed, std::hash<std::string>()(path.variant_part_raw()));
    hash_combine(seed, std::hash<std::string>()(path.variant_selection_raw()));

    return seed;
  }
};

struct PathKeyEqual {
  bool operator()(const Path &lhs, const Path &rhs) const {
    bool ret = lhs.prim_part() == rhs.prim_part();
    ret &= lhs.prop_part() == rhs.prop_part();
    //ret &= lhs.GetLocalPart() == rhs.GetLocalPart();
    ret &= lhs.is_valid() == rhs.is_valid();
    // See concern 5 in review.md. See note in PathHasher about friendship.
    ret &= lhs.variant_part_raw() == rhs.variant_part_raw();
    ret &= lhs.variant_selection_raw() == rhs.variant_selection_raw();

    return ret;
  }
};

struct FieldHasher {
  size_t operator()(const Field &field) const {
    size_t seed = std::hash<uint32_t>()(field.token_index.value);
    hash_combine(seed, std::hash<uint64_t>()(field.value_rep.GetData()));

    return seed;
  }
};

struct FieldKeyEqual {
  bool operator()(const Field &lhs, const Field &rhs) const {
    bool ret = lhs.token_index == rhs.token_index;
    ret &= lhs.value_rep == rhs.value_rep;

    return ret;
  }
};

struct FieldSetHasher {
  size_t operator()(const std::vector<crate::FieldIndex> &fieldset) const {
    if (fieldset.empty()) {
      return 0;
    }

    size_t seed = std::hash<uint32_t>()(fieldset[0].value);
    for (size_t i = 1; i < fieldset.size(); i++) {
      hash_combine(seed, std::hash<uint32_t>()(fieldset[i].value));
    }

    return seed;
  }
};

//
// TOC = list of sections.
//
struct TableOfContents {
  // Section const *GetSection(SectionName) const;
  // int64_t GetMinimumSectionStart() const;
  std::vector<Section> sections;
};

// TODO: Use PrimVar?
class CrateValue {
 public:
  //typedef std::map<std::string, CrateValue> Dictionary;

  //std::string GetTypeName() const;
  //uint32_t GetTypeId() const;

  // Store a value. value_ (a value::Value) accepts any registered USD value type
  // through its converting constructor — scalars, 1D arrays (std::vector<T>),
  // TypedArray/ChunkedTypedArray, ListOp<>, TimeSamples, dict, Payload,
  // VariantSelectionMap, the Specifier/Permission/Variability enums, ... — so this
  // single constrained forwarding template replaces the ~260 type-specific Set()
  // overloads this class used to generate via macros. Those overloads were a large
  // per-TU parse cost (CrateValue is parsed in every crate translation unit).
  // Serialization still dispatches on value_.type_id(), so the set of
  // crate-writable types is unchanged; an unsupported T fails at value::Value
  // construction exactly as before it failed to find an overload.
  template <typename T,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<T>::type, CrateValue>::value>::type>
  void Set(T &&v) {
    value_ = std::forward<T>(v);
    is_unregistered_value_ = false;
  }

  void SetUnregisteredValueString(std::string v) {
    value_ = std::move(v);
    is_unregistered_value_ = true;
  }

  bool IsUnregisteredValue() const {
    return is_unregistered_value_;
  }

  const std::string *GetUnregisteredValueString() const {
    return is_unregistered_value_ ? value_.as<std::string>() : nullptr;
  }

  // Type-safe way to get concrete value.
  template <class T>
  nonstd::optional<T> get_value() const {
    return value_.get_value<T>();
  }

  // Return null when type-mismatch
  template <class T>
  const T *as() const {
    return value_.as<T>();
  }

  std::string type_name() const {
    return value_.type_name();
  }

  uint32_t type_id() const {
    return value_.type_id();
  }

  const value::Value &get_raw() const LIGHTUSD_LIFETIMEBOUND {
    return value_;
  }

  value::Value &get_raw() LIGHTUSD_LIFETIMEBOUND {
    return value_;
  }

  const value::Value *get_raw_ptr() const LIGHTUSD_LIFETIMEBOUND {
    return &value_;
  }

  // mmap zero-copy: set when value was described but not materialized
  bool has_mmap_ref() const { return _has_mmap_ref; }
  const MMapArrayRef &mmap_ref() const LIGHTUSD_LIFETIMEBOUND {
    return _mmap_ref;
  }
  void set_mmap_ref(const MMapArrayRef &ref) {
    _mmap_ref = ref;
    _has_mmap_ref = true;
  }

 private:
  value::Value value_;
  bool is_unregistered_value_{false};
  MMapArrayRef _mmap_ref{};
  bool _has_mmap_ref{false};
};

// In-memory storage for a single "spec" -- prim, property, etc.
using FieldValuePair = std::pair<std::string, crate::CrateValue>;
using FieldValuePairVector = std::vector<FieldValuePair>;

struct StdHashWrapper {
    template <class T>
    inline size_t operator()(const T &val) const {
        return std::hash<T>()(val);
    }
};


} // namespace crate

namespace value {

#include "define-type-trait.inc"

// synonym to `value::dict`
//DEFINE_TYPE_TRAIT(crate::CrateValue::Dictionary, "dict", TYPE_ID_DICT, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

} // namespace value

} // namespace lightusd
