// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// path-expression.hh (next-local)
//
// C++ adapter over the C11 SdfPathExpression core (src/next/c/path-expr.c).
// The next product owns this adapter instead of including the legacy
// core/path-expression.hh, which drags the legacy value-type layer into every
// next translation unit.
//
// The C++ surface mirrors the legacy names and semantics exactly
// (Parse / GetText / valid / empty / ops / patterns / refs, plus the matcher
// declared in path-expression-eval.hh) so consumers only change their include.
//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "../c/lightusd-core-c.h"

namespace lightusd {
// The legacy library (src/core/path-expression.hh) defines different classes
// under the same lightusd:: names. This inline namespace gives the next
// versions distinct mangled names, so a binary linking both libraries gets no
// duplicate (MSVC LNK2005) or silently merged (ELF) definitions.
inline namespace next_path_expr {

// A single component of a path pattern (the portion following the literal
// prefix path). A "stretch" component (`//`) has empty text and no predicate.
struct PathPatternComponent {
  std::string text;      // literal/glob text; may contain '*' or '?'
  std::string predicate; // raw predicate expression (inside `{...}`)

  bool is_stretch() const { return text.empty() && predicate.empty(); }

  bool operator==(const PathPatternComponent &rhs) const {
    return text == rhs.text && predicate == rhs.predicate;
  }
};

// SdfPathPattern: a prefix path followed by components.
struct PathPattern {
  std::string prefix;
  std::vector<PathPatternComponent> components;
  bool match_property = false; // last component targets a property
  std::string property_name;   // property name when match_property

  bool is_absolute() const { return !prefix.empty() && prefix[0] == '/'; }

  std::string GetText() const;

  bool operator==(const PathPattern &rhs) const {
    return prefix == rhs.prefix && components == rhs.components &&
           match_property == rhs.match_property &&
           property_name == rhs.property_name;
  }
};

// An expression reference: `%/Owner:name`, `%:name` (same prim) or `%_`.
struct ExpressionReference {
  std::string path; // owning-prim path; empty for same-prim / weaker
  std::string name; // collection name; "_" denotes the weaker reference

  bool is_weaker() const { return name == "_" && path.empty(); }
};

// Parsed SdfPathExpression.
class ParsedPathExpression {
 public:
  // Operators are stored in prefix order: the operator precedes its operand
  // subtree(s). `Complement` is unary; `ExpressionRef`/`Pattern` are operand
  // markers that consume the next entry from patterns_/refs_.
  enum class Op {
    Complement,     // ~   (unary)
    ImpliedUnion,   //     (whitespace)
    Union,          // +
    Intersection,   // &
    Difference,     // -
    ExpressionRef,  // operand: next reference
    Pattern,        // operand: next pattern
  };

  ParsedPathExpression() = default;

  ParsedPathExpression(const ParsedPathExpression &rhs) { *this = rhs; }

  ParsedPathExpression(ParsedPathExpression &&rhs) noexcept {
    *this = std::move(rhs);
  }

  ParsedPathExpression &operator=(const ParsedPathExpression &rhs);

  ParsedPathExpression &operator=(ParsedPathExpression &&rhs) noexcept;

  ~ParsedPathExpression() { release(); }

  // Parse `text`. On failure returns an invalid expression and (if `err`) sets
  // a diagnostic. An empty string parses to a valid empty expression.
  static ParsedPathExpression Parse(const std::string &text,
                                    std::string *err = nullptr);

  bool valid() const { return valid_; }
  bool empty() const { return ops_.empty(); }
  const std::string &error() const { return parse_error_; }

  // Re-serialize to canonical text.
  std::string GetText() const;

  const std::vector<Op> &ops() const { return ops_; }
  const std::vector<PathPattern> &patterns() const { return patterns_; }
  const std::vector<ExpressionReference> &refs() const { return refs_; }

  // Handle into the C11 core. Borrowed: valid while this object lives and is
  // not assigned to. Used by the matcher and by next-internal glue.
  const lu_path_expr *impl() const { return impl_; }

 private:
  explicit ParsedPathExpression(std::nullptr_t) {}

  void release() noexcept {
    if (impl_ != nullptr) {
      lu_path_expr_destroy(impl_);
      impl_ = nullptr;
    }
    valid_ = false;
    parse_error_.clear();
    ops_.clear();
    patterns_.clear();
    refs_.clear();
  }

  void rebuild_mirrors();

  lu_path_expr *impl_ = nullptr;
  bool valid_ = false;
  std::string parse_error_;
  std::vector<Op> ops_;
  std::vector<PathPattern> patterns_;
  std::vector<ExpressionReference> refs_;
};

// --- inline implementation (header-only: no next-side TU of its own) ---

inline void ParsedPathExpression::rebuild_mirrors() {
  ops_.clear();
  patterns_.clear();
  refs_.clear();
  if (impl_ == nullptr) return;

  size_t count = 0;
  if (lu_path_expr_op_count(impl_, &count) == LU_OK) {
    ops_.reserve(count);
    for (size_t i = 0; i < count; i++) {
      lu_path_expr_op op = LU_PATH_EXPR_OP_PATTERN;
      if (lu_path_expr_op_at(impl_, i, &op) != LU_OK) break;
      ops_.push_back(static_cast<Op>(op));
    }
  }

  if (lu_path_expr_pattern_count(impl_, &count) == LU_OK) {
    patterns_.reserve(count);
    for (size_t i = 0; i < count; i++) {
      lu_pattern_view pv;
      if (lu_path_expr_pattern_at(impl_, i, &pv) != LU_OK) break;
      PathPattern p;
      p.prefix.assign(pv.prefix.data != nullptr ? pv.prefix.data : "",
                      pv.prefix.len);
      p.match_property = pv.match_property != 0;
      p.property_name.assign(
          pv.property_name.data != nullptr ? pv.property_name.data : "",
          pv.property_name.len);
      for (size_t c = 0; c < pv.component_count; c++) {
        lu_component_view cv;
        if (lu_path_expr_component_at(impl_, i, c, &cv) != LU_OK) break;
        PathPatternComponent comp;
        comp.text.assign(cv.text.data != nullptr ? cv.text.data : "",
                         cv.text.len);
        comp.predicate.assign(
            cv.predicate.data != nullptr ? cv.predicate.data : "",
            cv.predicate.len);
        p.components.push_back(std::move(comp));
      }
      patterns_.push_back(std::move(p));
    }
  }

  if (lu_path_expr_ref_count(impl_, &count) == LU_OK) {
    refs_.reserve(count);
    for (size_t i = 0; i < count; i++) {
      lu_ref_view rv;
      if (lu_path_expr_ref_at(impl_, i, &rv) != LU_OK) break;
      ExpressionReference r;
      r.path.assign(rv.path.data != nullptr ? rv.path.data : "", rv.path.len);
      r.name.assign(rv.name.data != nullptr ? rv.name.data : "", rv.name.len);
      refs_.push_back(std::move(r));
    }
  }
}

inline ParsedPathExpression &ParsedPathExpression::operator=(
    const ParsedPathExpression &rhs) {
  if (this == &rhs) return *this;

  lu_path_expr *cloned = nullptr;
  if (rhs.impl_ != nullptr) {
    if (lu_path_expr_clone(rhs.impl_, &cloned) != LU_OK) {
      // Clone failure keeps this object in a well-defined (invalid) state.
      release();
      return *this;
    }
  }

  lu_path_expr *old = impl_;
  impl_ = cloned;
  valid_ = rhs.valid_;
  parse_error_ = rhs.parse_error_;
  ops_ = rhs.ops_;
  patterns_ = rhs.patterns_;
  refs_ = rhs.refs_;
  if (old != nullptr) lu_path_expr_destroy(old);
  return *this;
}

inline ParsedPathExpression &ParsedPathExpression::operator=(
    ParsedPathExpression &&rhs) noexcept {
  if (this == &rhs) return *this;
  release();
  impl_ = rhs.impl_;
  valid_ = rhs.valid_;
  parse_error_ = std::move(rhs.parse_error_);
  ops_ = std::move(rhs.ops_);
  patterns_ = std::move(rhs.patterns_);
  refs_ = std::move(rhs.refs_);
  rhs.impl_ = nullptr;
  rhs.valid_ = false;
  return *this;
}

inline ParsedPathExpression ParsedPathExpression::Parse(const std::string &text,
                                                        std::string *err) {
  ParsedPathExpression out(std::nullptr_t{});

  lu_view view;
  view.data = text.empty() ? nullptr : text.data();
  view.len = text.size();

  lu_path_expr *impl = nullptr;
  char errbuf[256];
  errbuf[0] = '\0';
  if (lu_path_expr_parse(view, &impl, errbuf, sizeof(errbuf)) != LU_OK) {
    out.valid_ = false;
    out.parse_error_ = errbuf[0] != '\0' ? errbuf : "invalid path expression";
    if (err != nullptr) *err = out.parse_error_;
    return out;
  }

  out.impl_ = impl;
  out.valid_ = lu_path_expr_valid(impl) != 0;
  out.rebuild_mirrors();
  if (!out.valid_) {
    out.parse_error_ = errbuf[0] != '\0' ? errbuf : "invalid path expression";
    if (err != nullptr) *err = out.parse_error_;
  }
  return out;
}

inline std::string ParsedPathExpression::GetText() const {
  if (impl_ == nullptr || !valid_) return std::string();
  size_t need = 0;
  if (lu_path_expr_get_text(impl_, nullptr, 0, &need) != LU_OK) {
    return std::string();
  }
  std::string out;
  out.resize(need);
  if (need == 0) return out;
  if (lu_path_expr_get_text(impl_, &out[0], out.size() + 1, nullptr) != LU_OK) {
    return std::string();
  }
  return out;
}

inline std::string PathPattern::GetText() const {
  // Mirrors the C serializer for a standalone pattern.
  std::string result = prefix;
  const bool prefix_is_abs_root = (prefix == "/");
  for (size_t i = 0; i < components.size(); i++) {
    const PathPatternComponent &c = components[i];
    if (c.is_stretch()) {
      result += (i == 0 && prefix_is_abs_root) ? "/" : "//";
      continue;
    }
    if ((i + 1 == components.size()) && match_property) {
      result.push_back('.');
    } else if (!result.empty() && result.back() != '/') {
      result.push_back('/');
    }
    result += c.text;
    if (!c.predicate.empty()) {
      result += "{" + c.predicate + "}";
    }
  }
  return result;
}

} // inline namespace next_path_expr
} // namespace lightusd
