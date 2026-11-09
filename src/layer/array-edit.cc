// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Sparse array edit (VtArrayEdit) evaluation. See array-edit.hh.

#include "array-edit.hh"

#include <vector>

#include "parser/lexer.hh"
#include "parser/value-parser.hh"
#include "types/type-id.hh"
#include "writer/value-printer.hh"

namespace lightusd {
namespace next {

namespace {

}  // namespace

// WE printed the input (PrintValue), so the grammar is closed: elements are
// atoms, "quoted strings" (with backslash escapes), @asset@ / @@@asset@@@
// paths, or (tuple, (nested))s -- split on top-level commas.
bool SplitPrintedArrayElements(const std::string& text,
                               std::vector<std::string>* out) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n && (text[i] == ' ' || text[i] == '\n')) ++i;
  if (i >= n || text[i] != '[') return false;
  ++i;
  size_t elem_start = i;
  int paren_depth = 0;
  auto flush = [&](size_t end_pos) {
    size_t b = elem_start, e = end_pos;
    while (b < e && (text[b] == ' ' || text[b] == '\n')) ++b;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\n')) --e;
    if (e > b) out->push_back(text.substr(b, e - b));
  };
  for (; i < n; ++i) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      const char q = c;
      ++i;
      while (i < n && text[i] != q) {
        if (text[i] == '\\' && i + 1 < n) ++i;
        ++i;
      }
    } else if (c == '@') {
      const bool triple = i + 2 < n && text[i + 1] == '@' && text[i + 2] == '@';
      i += triple ? 3 : 1;
      if (triple) {
        while (i + 2 < n &&
               !(text[i] == '@' && text[i + 1] == '@' && text[i + 2] == '@')) {
          ++i;
        }
        i += 2;  // loop ++ lands past the last '@'
      } else {
        while (i < n && text[i] != '@') ++i;
      }
    } else if (c == '(') {
      ++paren_depth;
    } else if (c == ')') {
      --paren_depth;
    } else if (c == ',' && paren_depth == 0) {
      flush(i);
      elem_start = i + 1;
    } else if (c == ']' && paren_depth == 0) {
      flush(i);
      return true;
    }
  }
  return false;
}

namespace {

// Canonical element text of a value-initialized element (what pxr's
// resize/minsize fills with when no fill literal is given): print a
// zero-initialized scalar of the element type through the usda printer.
std::string DefaultElementText(TypeId elem_type) {
  if (elem_type == TypeId::String || elem_type == TypeId::Token) {
    return "\"\"";
  }
  if (elem_type == TypeId::AssetPath) {
    return "@@";
  }
  const size_t sz = GetTypeSize(elem_type);
  if (sz == 0 || sz > 256) return "0";
  std::vector<char> zeros(sz, 0);
  const Value v = Value::MakeFromRaw(elem_type, zeros.data());
  PrintOptions opts;
  opts.compact = true;
  return PrintValue(v, opts);
}

// pxr Vt_ArrayEditOps index rules (_ForEachImpl): negatives count from the
// END of the current result; out-of-range skips the op.
bool NormalizeRefIndex(int64_t* idx, size_t size) {
  if (*idx < 0) *idx += static_cast<int64_t>(size);
  return *idx >= 0 && static_cast<size_t>(*idx) < size;
}

bool NormalizeInsertIndex(int64_t* idx, size_t size) {
  if (*idx == kArrayEditEnd) *idx = static_cast<int64_t>(size);
  if (*idx < 0) *idx += static_cast<int64_t>(size);
  return *idx >= 0 && static_cast<size_t>(*idx) <= size;
}

}  // namespace

std::string BuildArrayEditText(const ArrayEditData& edit) {
  std::string out = "edit [";
  bool first = true;
  for (const ArrayEditOpRec& op : edit.ops) {
    if (!first) out += "; ";
    first = false;
    auto idx = [](int64_t v) { return "[" + std::to_string(v) + "]"; };
    switch (op.kind) {
      case ArrayEditOpRec::WriteLiteral:
        out += "write " + op.literal + " to " + idx(op.a2);
        break;
      case ArrayEditOpRec::WriteRef:
        out += "write " + idx(op.a1) + " to " + idx(op.a2);
        break;
      case ArrayEditOpRec::InsertLiteral:
        // Keep pxr's sugar spellings for the canonical positions.
        if (op.a2 == kArrayEditEnd) {
          out += "append " + op.literal;
        } else if (op.a2 == 0) {
          out += "prepend " + op.literal;
        } else {
          out += "insert " + op.literal + " at " + idx(op.a2);
        }
        break;
      case ArrayEditOpRec::InsertRef:
        if (op.a2 == kArrayEditEnd) {
          out += "append " + idx(op.a1);
        } else if (op.a2 == 0) {
          out += "prepend " + idx(op.a1);
        } else {
          out += "insert " + idx(op.a1) + " at " + idx(op.a2);
        }
        break;
      case ArrayEditOpRec::Erase:
        out += "erase " + idx(op.a1);
        break;
      case ArrayEditOpRec::MinSize:
        out += "minsize " + std::to_string(op.a1);
        if (op.has_fill) out += " fill " + op.literal;
        break;
      case ArrayEditOpRec::SetSize:
        out += "resize " + std::to_string(op.a1);
        if (op.has_fill) out += " fill " + op.literal;
        break;
      case ArrayEditOpRec::MaxSize:
        out += "maxsize " + std::to_string(op.a1);
        break;
    }
  }
  out += "]";
  return out;
}

bool ApplyArrayEdit(const ArrayEditData& edit, const Value* base,
                    TypeId elem_type, Value* out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = "array edit apply: " + m;
    return false;
  };
  if (!out) return fail("null output");

  // Base array -> element texts.
  std::vector<std::string> elems;
  if (base && !base->is_empty() && !base->is_block()) {
    PrintOptions opts;
    opts.compact = true;
    const std::string printed = PrintValue(*base, opts);
    if (!SplitPrintedArrayElements(printed, &elems)) {
      return fail("cannot decompose base array: " + printed);
    }
  }

  for (const ArrayEditOpRec& op : edit.ops) {
    int64_t a1 = op.a1;
    int64_t a2 = op.a2;
    const size_t size = elems.size();
    switch (op.kind) {
      case ArrayEditOpRec::WriteLiteral:
        if (!NormalizeRefIndex(&a2, size)) continue;
        elems[static_cast<size_t>(a2)] = op.literal;
        break;
      case ArrayEditOpRec::WriteRef:
        if (!NormalizeRefIndex(&a1, size) || !NormalizeRefIndex(&a2, size)) {
          continue;
        }
        elems[static_cast<size_t>(a2)] = elems[static_cast<size_t>(a1)];
        break;
      case ArrayEditOpRec::InsertLiteral:
        if (!NormalizeInsertIndex(&a2, size)) continue;
        elems.insert(elems.begin() + static_cast<ptrdiff_t>(a2), op.literal);
        break;
      case ArrayEditOpRec::InsertRef: {
        if (!NormalizeRefIndex(&a1, size) || !NormalizeInsertIndex(&a2, size)) {
          continue;
        }
        const std::string v = elems[static_cast<size_t>(a1)];
        elems.insert(elems.begin() + static_cast<ptrdiff_t>(a2), v);
        break;
      }
      case ArrayEditOpRec::Erase:
        if (!NormalizeRefIndex(&a1, size)) continue;
        elems.erase(elems.begin() + static_cast<ptrdiff_t>(a1));
        break;
      // `a1` on the growth ops is a raw file-controlled int64 (crate op stream
      // / usda `resize N`) reaching std::vector<std::string>::resize(). A
      // ~200-byte input asking for 2.8e14 elements otherwise means bad_alloc
      // (fatal under -fno-exceptions) or an OOM kill.
      case ArrayEditOpRec::MinSize:
        if (a1 < 0) continue;
        if (static_cast<uint64_t>(a1) > kMaxArrayEditElements) {
          return fail("minsize " + std::to_string(a1) + " exceeds the limit");
        }
        if (size < static_cast<size_t>(a1)) {
          elems.resize(static_cast<size_t>(a1),
                       op.has_fill ? op.literal
                                   : DefaultElementText(elem_type));
        }
        break;
      case ArrayEditOpRec::SetSize:
        if (a1 < 0) continue;
        if (static_cast<uint64_t>(a1) > kMaxArrayEditElements) {
          return fail("resize " + std::to_string(a1) + " exceeds the limit");
        }
        elems.resize(static_cast<size_t>(a1),
                     op.has_fill ? op.literal
                                 : DefaultElementText(elem_type));
        break;
      case ArrayEditOpRec::MaxSize:
        if (a1 < 0) continue;
        if (size > static_cast<size_t>(a1)) {
          elems.resize(static_cast<size_t>(a1));
        }
        break;
    }
  }

  // Reassemble and parse as a plain array of the element type.
  std::string text = "[";
  for (size_t i = 0; i < elems.size(); ++i) {
    if (i) text += ", ";
    text += elems[i];
  }
  text += "]";
  Lexer lexer(text.data(), text.size());
  ParseArrayContext ctx;  // no lazy source: fully materialized result
  ParseResult r = ParseArrayValue(lexer, elem_type, ctx);
  if (!r.success) {
    return fail("cannot reparse resolved array `" + text + "`: " + r.error);
  }
  *out = std::move(r.value);
  return true;
}

}  // namespace next
}  // namespace lightusd
