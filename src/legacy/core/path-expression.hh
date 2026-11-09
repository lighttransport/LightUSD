// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// path-expression.hh
//
// Parser + serializer for SdfPathExpression.
//
// `value::PathExpression` (in value-types.hh) only stores the canonical text.
// This module parses that text into a matchable representation and serializes
// it back, mirroring OpenUSD's `SdfPathExpression`/`SdfPathPattern` model:
//
//   - An expression is set-algebra over operands (path patterns and references
//     to other collections' expressions), combined with the operators
//     complement (`~`), implied-union (whitespace), union (`+`),
//     intersection (`&`) and difference (`-`).
//   - Operators are stored in prefix order in `ops_`; patterns and refs are
//     stored left-to-right in `patterns_`/`refs_` and consumed in walk order.
//   - A pattern is a (possibly absolute) prefix path followed by components,
//     each of which may contain `*`/`?` wildcards and an embedded predicate
//     `{...}`; a "stretch" component (`//`) matches arbitrary hierarchy depth.
//
// The matcher + predicate library that evaluates a parsed expression against a
// prim live in path-expression-eval.{hh,cc}.
//
#pragma once

#include <string>
#include <vector>

#include "value-types.hh"  // value::PathExpression

namespace lightusd {

// A single component of a path pattern (the portion following the literal
// prefix path). A "stretch" component (`//`) has empty text and no predicate.
struct PathPatternComponent {
  std::string text;       // literal/glob text; may contain '*' or '?'
  std::string predicate;  // raw predicate expression (inside `{...}`); empty if none

  bool is_stretch() const { return text.empty() && predicate.empty(); }

  bool operator==(const PathPatternComponent &rhs) const {
    return text == rhs.text && predicate == rhs.predicate;
  }
};

// SdfPathPattern: a prefix path followed by components.
//
// `prefix` is the canonical path string of the literal leading portion before
// the first wildcard / predicate / stretch (matching OpenUSD's
// `_prefix.GetAsString()`): e.g. "/World/Geom" (absolute), "World" (relative),
// "." (reflexive relative), or "" (empty). The trailing components carry any
// wildcards, predicates and stretch (`//`) markers, plus an optional property.
//
// e.g. "/World/Geom//C*{model}" =>
//   prefix="/World/Geom",
//   components=[ <stretch>, {text="C*", predicate="model"} ]
struct PathPattern {
  std::string prefix;
  std::vector<PathPatternComponent> components;
  bool match_property = false;  // last component targets a property
  std::string property_name;    // property name when match_property

  bool is_absolute() const { return !prefix.empty() && prefix[0] == '/'; }

  std::string GetText() const;

  bool operator==(const PathPattern &rhs) const {
    return prefix == rhs.prefix && components == rhs.components &&
           match_property == rhs.match_property &&
           property_name == rhs.property_name;
  }
};

// An expression reference: `%/Owner:name`, `%:name` (same prim) or `%_`
// (the weaker reference used in collection composition).
struct ExpressionReference {
  std::string path;  // owning-prim path; empty for same-prim / weaker
  std::string name;  // collection name; "_" denotes the weaker reference

  bool is_weaker() const { return name == "_" && path.empty(); }

  std::string GetText() const;
};

// Parsed SdfPathExpression.
class ParsedPathExpression {
 public:
  // Operators are stored in prefix order: the operator precedes its operand
  // subtree(s). `Complement` is unary; `Pattern`/`ExpressionRef` are operand
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

 private:
  std::vector<Op> ops_;
  std::vector<PathPattern> patterns_;
  std::vector<ExpressionReference> refs_;
  bool valid_ = false;
  std::string parse_error_;
};

// Convenience overload operating on a value::PathExpression's text.
ParsedPathExpression ParsePathExpression(const value::PathExpression &expr,
                                         std::string *err = nullptr);

}  // namespace lightusd
