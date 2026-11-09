// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// path-expression-eval.hh
//
// Self-contained matcher for SdfPathExpression. Given a parsed expression and a
// path string, decide whether the path matches. The engine depends only on
// path-expression.hh, str-util.hh (the freestanding GlobMatch -- no std::regex),
// and the standard library. Predicate evaluation and expression-reference
// resolution are injected via callbacks so this module stays decoupled from
// Stage/Prim; callers (e.g. Tydra collection resolution) supply them.
//
#pragma once

#include <functional>
#include <string>

#include "core/path-expression.hh"

namespace lightusd {

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
  std::function<const ParsedPathExpression *(const ExpressionReference &ref)>
      resolve_ref;

  // Guard against reference cycles.
  int max_ref_depth = 32;
};

// Match a single pattern against an (absolute) path string. `path` may include a
// trailing `.property`. `*`/`?` wildcards are matched with lightusd's freestanding
// GlobMatch; `//` matches arbitrary hierarchy depth; predicates use ctx.
bool MatchPattern(const PathPattern &pattern, const std::string &path,
                  const PathExpressionEvalContext &ctx = {});

// Match a full expression (set-algebra over patterns and references) against a path.
bool MatchPath(const ParsedPathExpression &expr, const std::string &path,
               const PathExpressionEvalContext &ctx = {});

}  // namespace lightusd
