// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// path-expression-eval.hh (next-local)
//
// Self-contained matcher for SdfPathExpression, backed by the C11 core
// (src/next/c/path-expr.c). Predicate evaluation and expression-reference
// resolution are injected through PathExpressionEvalContext so this module
// stays decoupled from Stage/Prim.
//
#pragma once

#include <cstring>
#include <functional>
#include <string>

#include "path-expression.hh"

namespace lightusd {
// See path-expression.hh for the inline namespace.
inline namespace next_path_expr {

// Caller-provided hooks. Both are optional; when unset the matcher evaluates
// structurally (predicates pass, references match nothing).
struct PathExpressionEvalContext {
  // Evaluate a predicate (the raw text inside `{...}`, e.g. "model",
  // "kind:component", "isa:Mesh") against the prim at `prim_path`.
  // When unset, predicates are treated as satisfied.
  std::function<bool(const std::string &predicate, const std::string &prim_path)>
      eval_predicate;

  // Resolve an expression reference (`%/Owner:name`, `%:name`, `%_`) to a parsed
  // expression. Return nullptr if unresolved; unresolved refs match nothing.
  // The returned expression must outlive the MatchPath() call.
  std::function<const ParsedPathExpression *(const ExpressionReference &ref)>
      resolve_ref;

  // Guard against reference cycles.
  int max_ref_depth = 32;
};

// --- internal bridge (declared here so the C11 core stays C) ---

namespace detail {

inline int eval_predicate_bridge(void *user, lu_view predicate,
                                 lu_view prim_path) {
  const PathExpressionEvalContext *ctx =
      static_cast<const PathExpressionEvalContext *>(user);
  if (ctx == nullptr || !ctx->eval_predicate) return 1;
  const std::string pred(predicate.data != nullptr ? predicate.data : "",
                         predicate.len);
  const std::string prim(prim_path.data != nullptr ? prim_path.data : "",
                         prim_path.len);
  return ctx->eval_predicate(pred, prim) ? 1 : 0;
}

inline const lu_path_expr *resolve_ref_bridge(void *user, lu_view ref_path,
                                              lu_view ref_name, int is_weaker) {
  const PathExpressionEvalContext *ctx =
      static_cast<const PathExpressionEvalContext *>(user);
  if (ctx == nullptr || !ctx->resolve_ref) return nullptr;
  ExpressionReference ref;
  ref.path.assign(ref_path.data != nullptr ? ref_path.data : "", ref_path.len);
  ref.name.assign(ref_name.data != nullptr ? ref_name.data : "", ref_name.len);
  (void)is_weaker; // derivable from the reference fields
  const ParsedPathExpression *resolved = ctx->resolve_ref(ref);
  return resolved != nullptr ? resolved->impl() : nullptr;
}

} // namespace detail

// Match a full expression (set algebra over patterns and references) against a
// path. Returns false for an invalid or empty expression.
inline bool MatchPath(const ParsedPathExpression &expr, const std::string &path,
                      const PathExpressionEvalContext &ctx = {}) {
  if (!expr.valid() || expr.empty()) return false;

  lu_view view;
  view.data = path.empty() ? nullptr : path.data();
  view.len = path.size();

  lu_path_expr_callbacks cb;
  std::memset(&cb, 0, sizeof(cb));
  cb.max_ref_depth = ctx.max_ref_depth;
  if (ctx.eval_predicate) cb.eval_predicate = &detail::eval_predicate_bridge;
  if (ctx.resolve_ref) cb.resolve_ref = &detail::resolve_ref_bridge;

  return lu_path_expr_match_path(expr.impl(), view, &cb,
                                 const_cast<void *>(static_cast<const void *>(&ctx))) == 1;
}

} // inline namespace next_path_expr
} // namespace lightusd
