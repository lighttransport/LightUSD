// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// SdfPathExpression matcher. See path-expression-eval.hh.

#include "core/path-expression-eval.hh"

#include <cstdint>
#include <vector>

namespace lightusd {

namespace {

constexpr size_t kMaxMatchElems = 4096;
constexpr size_t kMaxMatchSegments = 4096;
constexpr size_t kMaxEvalOps = 4096;

bool GlobMatch(const std::string &pattern, const std::string &text) {
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t retry = 0;
  while (t < text.size()) {
    if (p < pattern.size() &&
        (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      retry = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++retry;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

// A path decomposed for matching.
struct PathParts {
  bool absolute = false;
  std::vector<std::string> segments;  // prim element names, top-down
  bool has_property = false;
  std::string property;
};

PathParts SplitPath(const std::string &path) {
  PathParts pp;
  if (path.empty()) return pp;

  std::string prim = path;

  // Split off a trailing `.property` (the property delimiter is the '.' in the
  // final '/'-segment). Leading relative tokens ("." / "..") are not relevant
  // for the absolute stage paths collections operate on.
  auto last_slash = path.rfind('/');
  std::string tail =
      (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);
  auto dot = tail.find('.');
  if (dot != std::string::npos && dot > 0) {
    pp.has_property = true;
    pp.property = tail.substr(dot + 1);
    size_t cut = (last_slash == std::string::npos) ? dot : last_slash + 1 + dot;
    prim = path.substr(0, cut);
  }

  pp.absolute = !prim.empty() && prim[0] == '/';
  size_t i = pp.absolute ? 1 : 0;
  std::string cur;
  for (; i < prim.size(); i++) {
    if (prim[i] == '/') {
      if (!cur.empty()) pp.segments.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(prim[i]);
    }
  }
  if (!cur.empty()) pp.segments.push_back(cur);
  return pp;
}

// Split a literal prefix path string (e.g. "/World/Geom") into segments.
std::vector<std::string> PrefixSegments(const std::string &prefix) {
  std::vector<std::string> segs;
  std::string cur;
  for (size_t i = (!prefix.empty() && prefix[0] == '/') ? 1 : 0;
       i < prefix.size(); i++) {
    if (prefix[i] == '/') {
      if (!cur.empty()) segs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(prefix[i]);
    }
  }
  if (!cur.empty()) segs.push_back(cur);
  return segs;
}

// A unified match element: literal prefix segment or a pattern component.
struct MatchElem {
  bool stretch = false;
  std::string text;       // glob text ('*'/'?'); exact for prefix segments
  std::string predicate;  // raw predicate, empty if none
};

// Recursive segment matcher with stretch (`//`) support.
//   elems[ei..] must match segments[si..].
// `segPrimPath(k)` yields the absolute prim path of segments[0..k] for predicates.
bool MatchElems(const std::vector<MatchElem> &elems, size_t ei,
                const std::vector<std::string> &segments, size_t si,
                const std::vector<std::string> &segPrimPaths,
                const PathExpressionEvalContext &ctx,
                std::vector<int8_t> *memo) {
  const size_t width = segments.size() + 1;
  int8_t &cached = (*memo)[ei * width + si];
  if (cached != -1) {
    return cached != 0;
  }
  auto finish = [&](bool v) {
    cached = v ? 1 : 0;
    return v;
  };

  if (ei == elems.size()) {
    return finish(si == segments.size());
  }
  const MatchElem &e = elems[ei];
  if (e.stretch) {
    // Match zero or more segments.
    for (size_t k = si; k <= segments.size(); k++) {
      if (MatchElems(elems, ei + 1, segments, k, segPrimPaths, ctx, memo)) {
        return finish(true);
      }
    }
    return finish(false);
  }
  if (si >= segments.size()) {
    return finish(false);
  }
  // An empty component text (a predicate-only element such as `{kind:group}`)
  // matches any single element name; the predicate below does the filtering.
  if (!e.text.empty() && !GlobMatch(e.text, segments[si])) {
    return finish(false);
  }
  if (!e.predicate.empty() && ctx.eval_predicate) {
    if (!ctx.eval_predicate(e.predicate, segPrimPaths[si])) {
      return finish(false);
    }
  }
  return finish(MatchElems(elems, ei + 1, segments, si + 1, segPrimPaths, ctx,
                           memo));
}

}  // namespace

bool MatchPattern(const PathPattern &pattern, const std::string &path,
                  const PathExpressionEvalContext &ctx) {
  PathParts pp = SplitPath(path);

  // Build the unified element sequence: literal prefix segments, then the
  // (prim-)components. The trailing property component is matched separately.
  std::vector<MatchElem> elems;
  for (const std::string &s : PrefixSegments(pattern.prefix)) {
    elems.push_back(MatchElem{false, s, ""});
  }

  const size_t ncomp = pattern.components.size();
  const size_t prim_comp_end =
      pattern.match_property && ncomp > 0 ? ncomp - 1 : ncomp;
  for (size_t i = 0; i < prim_comp_end; i++) {
    const PathPatternComponent &c = pattern.components[i];
    elems.push_back(MatchElem{c.is_stretch(), c.text, c.predicate});
  }
  if (elems.size() > kMaxMatchElems || pp.segments.size() > kMaxMatchSegments) {
    return false;
  }

  // Property gating.
  if (pattern.match_property) {
    if (!pp.has_property) return false;
    if (!GlobMatch(pattern.property_name, pp.property)) return false;
  } else {
    // A prim pattern does not match a property path.
    if (pp.has_property) return false;
  }

  // Absolute-ness must agree (collections use absolute paths).
  if (pattern.is_absolute() != pp.absolute) {
    return false;
  }

  // Precompute prim paths for predicate evaluation.
  std::vector<std::string> segPrimPaths(pp.segments.size());
  {
    std::string acc;
    for (size_t i = 0; i < pp.segments.size(); i++) {
      acc += "/" + pp.segments[i];
      segPrimPaths[i] = acc;
    }
  }

  const size_t memo_size = (elems.size() + 1) * (pp.segments.size() + 1);
  std::vector<int8_t> memo(memo_size, -1);
  return MatchElems(elems, 0, pp.segments, 0, segPrimPaths, ctx, &memo);
}

namespace {

using Op = ParsedPathExpression::Op;

// Stack-machine evaluator over the prefix-ordered op stream. Both operands of a
// binary op are always evaluated (no short-circuit) so the stream stays in sync.
struct Evaluator {
  const ParsedPathExpression &expr;
  const std::string &path;
  const PathExpressionEvalContext &ctx;
  int depth;
  size_t oi = 0, pi = 0, ri = 0;

  bool Eval() {
    if (oi >= expr.ops().size()) return false;
    Op op = expr.ops()[oi++];
    switch (op) {
      case Op::Pattern:
        return MatchPattern(expr.patterns()[pi++], path, ctx);
      case Op::ExpressionRef: {
        const ExpressionReference &ref = expr.refs()[ri++];
        if (!ctx.resolve_ref || depth <= 0) return false;
        const ParsedPathExpression *sub = ctx.resolve_ref(ref);
        if (!sub) return false;
        Evaluator e{*sub, path, ctx, depth - 1};
        return e.Eval();
      }
      case Op::Complement:
        return !Eval();
      case Op::Union:
      case Op::ImpliedUnion: {
        bool a = Eval();
        bool b = Eval();
        return a || b;
      }
      case Op::Intersection: {
        bool a = Eval();
        bool b = Eval();
        return a && b;
      }
      case Op::Difference: {
        bool a = Eval();
        bool b = Eval();
        return a && !b;
      }
    }
    return false;
  }
};

}  // namespace

bool MatchPath(const ParsedPathExpression &expr, const std::string &path,
               const PathExpressionEvalContext &ctx) {
  if (!expr.valid() || expr.empty()) {
    return false;
  }
  if (expr.ops().size() > kMaxEvalOps) {
    return false;
  }
  Evaluator e{expr, path, ctx, ctx.max_ref_depth};
  return e.Eval();
}

}  // namespace lightusd
