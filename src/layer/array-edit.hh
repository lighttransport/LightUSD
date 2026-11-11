// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - sparse array edit (VtArrayEdit) text building & evaluation.
//
// The next core carries an authored `edit [ <op>; ... ]` value as a
// structured op list (PrimSpec::array_edit) whose literal elements are
// canonical usda ELEMENT TEXT, plus the canonical `edit [...]` one-liner in
// raw_default_source for printing. Evaluation applies the ops to a base
// array by round-tripping through the usda value codec: print the base
// array, edit the element-text vector with pxr's exact index-normalization
// and skip rules (Vt_ArrayEditOps::_ForEachImpl), reparse the result. One
// implementation covers every array element type, and edits stay rare
// enough that the codec round-trip cost is irrelevant.
#pragma once

#include <string>
#include <vector>

#include "prim-spec.hh"
#include "types/value.hh"

namespace lightusd {

/// Upper bound on the element count a growth op (`resize`/`minsize`) may ask
/// for. Matches CrateReadOptions::max_array_elements. The op operand comes
/// straight out of the file, so without this a few bytes of input drive an
/// arbitrarily large std::vector<std::string>::resize().
constexpr uint64_t kMaxArrayEditElements = 1ull << 30;

/// Split a canonical PrintValue'd array "[e0, e1, ...]" into element texts.
/// Only guaranteed for text this codebase printed (closed grammar: atoms,
/// double-quoted strings, @assets@, (tuples)). Returns false on malformed
/// input.
bool SplitPrintedArrayElements(const std::string& text,
                               std::vector<std::string>* out);

/// Rebuild the canonical one-line `edit [op; op; ...]` spelling from an op
/// list (pxr usdcat's format). Used when composition stacks two edits.
std::string BuildArrayEditText(const ArrayEditData& edit);

/// Apply `edit` to `base` (nullptr or empty Value = edit over an empty
/// array, like pxr resolving an edit with no weaker opinion) and return the
/// resolved array through `out`. `elem_type` is the array's ELEMENT type.
/// Out-of-range indices skip their op (pxr semantics); a codec failure
/// returns false with a diagnostic in `err`.
bool ApplyArrayEdit(const ArrayEditData& edit, const Value* base,
                    TypeId elem_type, Value* out, std::string* err);

}  // namespace lightusd
