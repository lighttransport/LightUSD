// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include "types/value.hh"

#include <string>

namespace lightusd {

/// Policy for backtick-delimited variable expressions in composition asset
/// paths. Disabled preserves the authored path, Evaluate preserves an
/// unresolved expression and reports a warning, and RequireResolved rejects
/// the arc when parsing/evaluation fails.
enum class ExpressionVariablePolicy {
  Disabled,
  Evaluate,
  RequireResolved,
};

struct ExpressionEvaluation {
  std::string value;
  std::string error;
  bool is_expression = false;
  bool success = true;
  /// The expression evaluated successfully to `None` (e.g. an `if` without an
  /// else branch). For an asset path or variant selection this means "no
  /// opinion": the caller should skip the arc / selection without an error.
  bool is_none = false;
};

/// Overlay dictionary entries from `stronger` onto `weaker` and return a new
/// dictionary. Non-dictionary inputs are treated as empty.
Value ComposeExpressionVariables(const Value& weaker, const Value& stronger);

/// Evaluate an SdfVariableExpression (a backtick-delimited expression) that
/// must produce a STRING (or None): the form used by composition asset paths
/// and variant selections. Supports the full expression language:
///   - `${VAR}` variable references (variables whose values are themselves
///     backtick expressions evaluate recursively, with cycle detection)
///   - quoted strings with `${VAR}` interpolation and backslash escapes
///   - int64, `True` / `False`, `None`, and `[...]` list literals
///   - functions: if, and, or, not, eq, neq, lt, leq, gt, geq, contains,
///     at, len, defined
/// Inputs that are not backtick-delimited are returned verbatim with
/// `is_expression = false`.
ExpressionEvaluation EvaluateAssetPathExpression(const std::string& path,
                                                 const Value& variables);

}  // namespace lightusd
