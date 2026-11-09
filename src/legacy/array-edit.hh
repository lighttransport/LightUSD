// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// array-edit.hh
//
// value::ArrayEdit -- lightusd' type-erased representation of OpenUSD's
// VtArrayEdit<T> (Crate value type, ValueRep "array edit" bit, crate >= 0.14.0).
//
// An array edit is a sparse list of edit operations (write/insert/erase/resize/
// ...) applied over a weaker array opinion, rather than a full array value. In
// Crate it is encoded as a ValueRep with the IsArrayEdit bit set and the array
// element type in the type byte; the payload references (valuesRep, indexesRep,
// isDense) where:
//   - valuesRep   : a packed VtArray<T> of literal element values, and
//   - indexesRep  : a packed VtInt64Array -- the op instruction stream `_ins`.
//
// lightusd preserves both faithfully so array edits round-trip through Crate.
// The op stream uses OpenUSD's Vt_ArrayEditOps encoding: each instruction is a
// packed int64 word [count:56 | op:8] followed by `arity(op) * count` int64
// operands (literal indices, ref/dst indices, or sizes).
//
#pragma once

#include <cstdint>
#include <vector>

#include "value-types.hh"

namespace lightusd {
namespace value {

// OpenUSD Vt_ArrayEditOps::Op (pxr/base/vt/arrayEditOps.h). Values are part of
// the on-disk encoding -- do not renumber.
enum class ArrayEditOp : uint8_t {
  WriteLiteral = 0,   // write <literal> to [index]
  WriteRef = 1,       // write [index1] to [index2]
  InsertLiteral = 2,  // insert <literal> at [index]  (prepend/append at 0/End)
  InsertRef = 3,      // insert [index1] at [index2]
  EraseRef = 4,       // erase [index]
  MinSize = 5,        // minsize <size>
  MinSizeFill = 6,    // minsize <size> <literal>
  SetSize = 7,        // resize <size>
  SetSizeFill = 8,    // resize <size> <literal>
  MaxSize = 9,        // maxsize <size>
};

// EndIndex sentinel (insert at end == append). Mirrors
// Vt_ArrayEditOps::EndIndex == std::numeric_limits<int64_t>::min().
constexpr int64_t kArrayEditEndIndex = (std::numeric_limits<int64_t>::min)();

// Number of operands following an op's packed word, per repetition.
inline int ArrayEditOpArity(ArrayEditOp op) {
  switch (op) {
    case ArrayEditOp::WriteLiteral:
    case ArrayEditOp::WriteRef:
    case ArrayEditOp::InsertLiteral:
    case ArrayEditOp::InsertRef:
    case ArrayEditOp::MinSizeFill:
    case ArrayEditOp::SetSizeFill:
      return 2;
    default:
      return 1;
  }
}

// Pack/unpack the [count:56 | op:8] instruction word (low 56 bits = count,
// high 8 bits = op), matching Vt_ArrayEditOps::OpAndCount.
inline int64_t ArrayEditPackOpWord(ArrayEditOp op, int64_t count) {
  return (static_cast<int64_t>(static_cast<uint64_t>(op) & 0xffull) << 56) |
         (count & 0x00FFFFFFFFFFFFFFll);
}
inline ArrayEditOp ArrayEditWordOp(int64_t word) {
  return static_cast<ArrayEditOp>(
      static_cast<uint8_t>((static_cast<uint64_t>(word) >> 56) & 0xffull));
}
inline int64_t ArrayEditWordCount(int64_t word) {
  return static_cast<int64_t>(static_cast<uint64_t>(word) &
                              0x00FFFFFFFFFFFFFFull);
}

// Type-erased VtArrayEdit. `literals` holds the typed VtArray<T> of literal
// element values (a value::Value wrapping std::vector<T>); `ops` is the int64
// instruction stream. `element_type_id` is the Crate element type id (see
// crate::CrateDataTypeId) so a literal-less / ref-only edit still knows its
// element type. An empty `ops` stream is the identity edit.
struct ArrayEdit {
  int32_t element_type_id{0};  // crate::CrateDataTypeId of the element type
  Value literals;              // std::vector<T> of literal element values
  std::vector<int64_t> ops;    // Vt_ArrayEditOps `_ins` instruction stream

  bool is_identity() const { return ops.empty(); }

  bool operator==(const ArrayEdit &rhs) const {
    return element_type_id == rhs.element_type_id && ops == rhs.ops;
  }
};

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(ArrayEdit, "ArrayEdit", TYPE_ID_ARRAY_EDIT, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value
}  // namespace lightusd
