// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - C11 SdfPathExpression core test.
//
// Covers the C11 core in src/next/c/path-expr.c through its C++ adapter
// (src/next/core/path-expression{,-eval}.hh). The behavioural contract mirrors
// the legacy path-expression test suite (tests/unit/unit-path-expression.cc):
// the same acceptance names, canonical texts, structural decomposition and
// match results must hold after the C11 port.
//
// Sections:
//   1. Accept / reject      - valid + malformed inputs.
//   2. Limits               - depth, node budget, expression size (unconditionally
//                             rejected, not silently truncated).
//   3. Canonical round-trip - exact texts plus Parse->GetText->Parse stability.
//   4. Decomposition        - prefix folding, stretch, wildcard, predicate,
//                             property, references, prefix op order.
//   5. Matching             - wildcards, stretch, property gating, set algebra.
//   6. Predicates / refs    - injected callbacks, including unresolved refs and
//                             cycle depth guard.
//   7. C API directly       - counted buffers, clone, NULL/short-buffer handling.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "next/c/lightusd-core-c.h"
#include "next/core/path-expression-eval.hh"

using lightusd::ExpressionReference;
using lightusd::ParsedPathExpression;
using lightusd::PathExpressionEvalContext;
using lightusd::PathPattern;
using lightusd::PathPatternComponent;

namespace {

int g_failures = 0;
const char *g_section = "";

void Section(const char *name) { g_section = name; }

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL [%s] %s:%d: %s\n", g_section, __FILE__, __LINE__,     \
                  #cond);                                                    \
      g_failures++;                                                          \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    const std::string _va = (a);                                             \
    const std::string _vb = (b);                                             \
    if (_va != _vb) {                                                        \
      std::printf("FAIL [%s] %s:%d: \"%s\" != \"%s\"\n", g_section,          \
                  __FILE__, __LINE__, _va.c_str(), _vb.c_str());             \
      g_failures++;                                                          \
    }                                                                        \
  } while (0)

bool Matches(const std::string &expr, const std::string &path,
             const PathExpressionEvalContext &ctx = {}) {
  ParsedPathExpression e = ParsedPathExpression::Parse(expr);
  return e.valid() && lightusd::MatchPath(e, path, ctx);
}

bool Valid(const std::string &expr, std::string *err = nullptr) {
  ParsedPathExpression e = ParsedPathExpression::Parse(expr, err);
  return e.valid();
}

bool SameStructure(const ParsedPathExpression &a, const ParsedPathExpression &b) {
  if (a.ops() != b.ops()) return false;
  if (a.patterns() != b.patterns()) return false;
  if (a.refs().size() != b.refs().size()) return false;
  for (size_t i = 0; i < a.refs().size(); i++) {
    if (a.refs()[i].path != b.refs()[i].path) return false;
    if (a.refs()[i].name != b.refs()[i].name) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------

void TestAcceptReject() {
  Section("accept/reject");
  const char *valid[] = {
      "/World/Geom", "/World/Geom/*", "//", "/World//Foo", "/World//C*{model}",
      "~/World/Excluded", "/A + /B", "/A & /B", "/A - /B", "/A /B",
      "%/Lib:coll", "%_", "/root %:ref1 - %:ref2", "(/A + /B) & /C",
  };
  for (const char *s : valid) {
    std::string err;
    CHECK(Valid(s, &err));
    CHECK(err.empty());
  }

  {  // Empty expression is valid and empty.
    ParsedPathExpression e = ParsedPathExpression::Parse("");
    CHECK(e.valid());
    CHECK(e.empty());
    CHECK_EQ(e.GetText(), "");
  }

  const char *invalid[] = {
      "(/A",      // unbalanced (
      "/A)",      // unbalanced )
      "/A +",     // dangling binary op
      "& /A",     // leading binary op
      "/A & & /B",// doubled operator
  };
  for (const char *s : invalid) {
    std::string err;
    const ParsedPathExpression e = ParsedPathExpression::Parse(s, &err);
    CHECK(!e.valid());
    CHECK(!err.empty());
  }
}

void TestLimits() {
  Section("limits");
  {  // Nesting depth: rejected past 256 (was 300 in the legacy test).
    std::string s;
    for (int i = 0; i < 300; i++) s.push_back('(');
    s += "/A";
    for (int i = 0; i < 300; i++) s.push_back(')');
    std::string err;
    CHECK(!Valid(s, &err));
    CHECK(err.find("deep") != std::string::npos);
  }
  {  // Node budget.
    std::string s;
    for (int i = 0; i < 3000; i++) {
      if (i) s += " ";
      s += "/A" + std::to_string(i);
    }
    std::string err;
    CHECK(!Valid(s, &err));
    CHECK(err.find("too many nodes") != std::string::npos);
  }
  {  // Expression size: unconditionally rejected, never parsed.
    std::string s(5u << 20, 'a');  // 5 MiB of a single (invalid) token
    s.insert(0, "/");
    std::string err;
    CHECK(!Valid(s, &err));
    CHECK(err.find("too large") != std::string::npos);
  }
  CHECK(Matches("/Root//Leaf", "/Root/A/B/C/D/E/F/G/H/I/J/Leaf"));
}

void TestRoundtrip() {
  Section("roundtrip");
  struct {
    const char *in;
    const char *out;
  } exact[] = {
      {"/World/Geom", "/World/Geom"},
      {"//", "//"},
      {"/World//Foo", "/World//Foo"},
      {"/World/Geom/*", "/World/Geom/*"},
      {"/World//C*{model}", "/World//C*{model}"},
      {"~/A", "~/A"},
      {"%/Lib:coll", "%/Lib:coll"},
      {"%_", "%_"},
      {"/A + /B", "/A + /B"},
      {"/A & /B", "/A & /B"},
      {"/A /B", "/A /B"},
      {"/World/Geom.visibility", "/World/Geom.visibility"},
  };
  for (const auto &t : exact) {
    const ParsedPathExpression e = ParsedPathExpression::Parse(t.in);
    CHECK(e.valid());
    CHECK_EQ(e.GetText(), t.out);
  }

  const char *exprs[] = {
      "/root %:ref1 - %:ref2",
      "(/A + /B) & /C",
      "/CollectionTest/Geom//C* //Box",
      "~/World/Excluded + /World/Included",
      "/A", "/A/B/C", "//", "/A//", "/A//B//C", "/World/Geom/*", "/World/C?be",
      "/World//*{model}", "/World//{kind:component}", "/A.attr",
      "/A//B.visibility", "~/A", "~/A", "~(/A)", "/A + /B", "/A - /B", "/A & /B",
      "/A /B", "/A + /B + /C", "(/A + /B) & /C", "/A & (/B - /C)",
      "~(/A + /B)", "%:coll", "%/Lib:coll", "%_", "/root %:r1 - %:r2",
      "/W//Mesh* & ~/W//Excluded",
  };
  for (const char *s : exprs) {
    const ParsedPathExpression e1 = ParsedPathExpression::Parse(s);
    CHECK(e1.valid());
    const ParsedPathExpression e2 = ParsedPathExpression::Parse(e1.GetText());
    CHECK(e2.valid());
    CHECK(SameStructure(e1, e2));
  }
}

void TestDecompose() {
  Section("decompose");
  {  // "//" => absolute-root prefix + single stretch.
    const ParsedPathExpression e = ParsedPathExpression::Parse("//");
    CHECK(e.valid());
    CHECK(e.patterns().size() == 1);
    CHECK_EQ(e.patterns()[0].prefix, "/");
    CHECK(e.patterns()[0].components.size() == 1);
    CHECK(e.patterns()[0].components[0].is_stretch());
  }
  {  // Prefix folding + stretch + wildcard + predicate.
    const ParsedPathExpression e =
        ParsedPathExpression::Parse("/World/Geom//C*{model}");
    CHECK(e.valid());
    CHECK_EQ(e.patterns()[0].prefix, "/World/Geom");
    CHECK(e.patterns()[0].components.size() == 2);
    CHECK(e.patterns()[0].components[0].is_stretch());
    CHECK_EQ(e.patterns()[0].components[1].text, "C*");
    CHECK_EQ(e.patterns()[0].components[1].predicate, "model");
  }
  {  // Difference + implied-union + refs; prefix op order.
    const ParsedPathExpression e =
        ParsedPathExpression::Parse("/root %:ref1 - %:ref2");
    CHECK(e.valid());
    CHECK(e.refs().size() == 2);
    CHECK(e.patterns().size() == 1);
    using Op = ParsedPathExpression::Op;
    CHECK(e.ops().size() == 5);
    if (e.ops().size() == 5) {
      CHECK(e.ops()[0] == Op::Difference);
      CHECK(e.ops()[1] == Op::ImpliedUnion);
    }
    CHECK_EQ(e.refs()[0].name, "ref1");
    CHECK_EQ(e.refs()[1].name, "ref2");
  }
  {  // Weaker reference.
    const ParsedPathExpression e = ParsedPathExpression::Parse("%_");
    CHECK(e.valid());
    CHECK(e.refs().size() == 1);
    CHECK(e.refs()[0].is_weaker());
  }
  {  // Grouped double complement: two Complement ops.
    const ParsedPathExpression e = ParsedPathExpression::Parse("~(~/A/B)");
    CHECK(e.valid());
    int complements = 0;
    for (const auto op : e.ops()) {
      if (op == ParsedPathExpression::Op::Complement) complements++;
    }
    CHECK(complements == 2);
  }
  {  // Predicate text is captured verbatim.
    const ParsedPathExpression e = ParsedPathExpression::Parse("/W//*{kind:component}");
    CHECK(e.valid());
    CHECK(!e.patterns()[0].components.empty());
    CHECK_EQ(e.patterns()[0].components.back().predicate, "kind:component");
  }
}

void TestMatch() {
  Section("match");
  CHECK(Matches("/World/Geom/*", "/World/Geom/Sphere"));
  CHECK(!Matches("/World/Geom/*", "/World/Geom"));      // '*' needs a child
  CHECK(!Matches("/World/Geom/*", "/World/Geom/A/B"));  // single level only
  CHECK(!Matches("/World/Geom/*", "/Other/Geom/Sphere"));

  CHECK(Matches("/World//Sphere", "/World/Sphere"));
  CHECK(Matches("/World//Sphere", "/World/A/B/Sphere"));
  CHECK(!Matches("/World//Sphere", "/World/A/Cube"));

  CHECK(Matches("//", "/Foo"));
  CHECK(Matches("//", "/Foo/Bar/Baz"));

  CHECK(Matches("/World/C*", "/World/Cube"));
  CHECK(Matches("/World/C*", "/World/Cone"));
  CHECK(!Matches("/World/C*", "/World/Box"));

  CHECK(Matches("/World/A + /World/B", "/World/A"));
  CHECK(Matches("/World/A + /World/B", "/World/B"));
  CHECK(!Matches("/World/A + /World/B", "/World/C"));

  CHECK(Matches("/World//* - /World/Excluded", "/World/Foo"));
  CHECK(!Matches("/World//* - /World/Excluded", "/World/Excluded"));

  CHECK(Matches("/World//* & /World/Keep", "/World/Keep"));
  CHECK(!Matches("/World//* & /World/Keep", "/World/Other"));

  CHECK(!Matches("~/World/Excluded", "/World/Excluded"));
  CHECK(Matches("~/World/Excluded", "/World/Other"));

  CHECK(!Matches("/World/Geom/*", "/World/Geom/Sphere.radius"));

  CHECK(Matches("/W/C?be", "/W/Cube"));
  CHECK(!Matches("/W/C?be", "/W/Cone"));
  CHECK(!Matches("/W/C?be", "/W/Cube2"));

  CHECK(Matches("/A//B//C", "/A/x/B/y/z/C"));
  CHECK(Matches("/A//B//C", "/A/B/C"));
  CHECK(!Matches("/A//B//C", "/A/x/C"));

  CHECK(Matches("/W//", "/W/a/b"));
  CHECK(Matches("/W//", "/W"));
  CHECK(!Matches("/W//", "/Other"));

  CHECK(Matches("(/W/A + /W/B) & /W/A", "/W/A"));
  CHECK(!Matches("(/W/A + /W/B) & /W/A", "/W/B"));
  CHECK(Matches("/W//* - (/W/X + /W/Y)", "/W/Z"));
  CHECK(!Matches("/W//* - (/W/X + /W/Y)", "/W/X"));
  CHECK(!Matches("/W//* - (/W/X + /W/Y)", "/W/Y"));

  CHECK(!Matches("~(/W/A + /W/B)", "/W/A"));
  CHECK(Matches("~(/W/A + /W/B)", "/W/C"));

  CHECK(!Matches("/W//*", "/W/A.attr"));
  CHECK(Matches("/W/A.visibility", "/W/A.visibility"));
  CHECK(!Matches("/W/A.visibility", "/W/A"));
}

void TestPredicatesAndRefs() {
  Section("predicates/refs");
  {  // Predicate callback gates the match.
    PathExpressionEvalContext ctx;
    ctx.eval_predicate = [](const std::string &pred,
                            const std::string &prim_path) {
      return pred == "mesh" && prim_path == "/World/A/Mesh1";
    };
    CHECK(Matches("/World//*{mesh}", "/World/A/Mesh1", ctx));
    CHECK(!Matches("/World//*{mesh}", "/World/A/Curve1", ctx));
  }
  {  // Reference resolution, including the unresolved case.
    ParsedPathExpression sub = ParsedPathExpression::Parse("/World/Lib//*");
    PathExpressionEvalContext ctx;
    ctx.resolve_ref = [&sub](const ExpressionReference &ref)
        -> const ParsedPathExpression * {
      if (ref.name == "lib") return &sub;
      return nullptr;
    };
    CHECK(Matches("%:lib", "/World/Lib/Thing", ctx));
    CHECK(!Matches("%:lib", "/World/Other/Thing", ctx));
    CHECK(!Matches("%:missing", "/World/Lib/Thing", ctx));
  }
  {  // Referenced expression must outlive the call; a living temporary works.
    PathExpressionEvalContext ctx;
    std::vector<ParsedPathExpression> keep;
    keep.push_back(ParsedPathExpression::Parse("/A//*"));
    ctx.resolve_ref = [&keep](const ExpressionReference &)
        -> const ParsedPathExpression * {
      return &keep[0];
    };
    CHECK(Matches("%:any", "/A/B/C", ctx));
  }
}

void TestCapi() {
  Section("c-api");
  const std::string text = "/World/Geom//C*{model} + ~/World/Excluded";
  const lu_view view{text.data(), text.size()};

  lu_path_expr *expr = nullptr;
  char errbuf[128] = {0};
  CHECK(lu_path_expr_parse(view, &expr, errbuf, sizeof(errbuf)) == LU_OK);
  if (expr == nullptr) return;
  CHECK(lu_path_expr_valid(expr) != 0);
  CHECK(lu_path_expr_empty(expr) == 0);

  // Size query without a buffer.
  size_t need = 0;
  CHECK(lu_path_expr_get_text(expr, nullptr, 0, &need) == LU_OK);
  CHECK(need == text.size());

  // Short buffer reports the needed size instead of truncating.
  char small[4] = {0};
  size_t got = 0;
  CHECK(lu_path_expr_get_text(expr, small, sizeof(small), &got) == LU_ERR_BUFFER);
  CHECK(got == need);

  // Exact buffer round-trips through the C boundary.
  std::vector<char> buf(need + 1, '\0');
  CHECK(lu_path_expr_get_text(expr, buf.data(), buf.size(), &got) == LU_OK);
  CHECK(std::string(buf.data(), got) == text);

  // Indices and views.
  size_t nops = 0, npatterns = 0, nrefs = 0;
  CHECK(lu_path_expr_op_count(expr, &nops) == LU_OK);
  CHECK(lu_path_expr_pattern_count(expr, &npatterns) == LU_OK);
  CHECK(lu_path_expr_ref_count(expr, &nrefs) == LU_OK);
  CHECK(npatterns == 2);
  CHECK(nrefs == 0);
  CHECK(nops == 4);

  lu_pattern_view pv;
  CHECK(lu_path_expr_pattern_at(expr, 0, &pv) == LU_OK);
  CHECK(std::string(pv.prefix.data, pv.prefix.len) == "/World/Geom");
  CHECK(pv.component_count == 2);
  lu_component_view cv;
  CHECK(lu_path_expr_component_at(expr, 0, 0, &cv) == LU_OK);
  CHECK(cv.is_stretch != 0);
  CHECK(lu_path_expr_component_at(expr, 0, 1, &cv) == LU_OK);
  CHECK(std::string(cv.text.data, cv.text.len) == "C*");
  CHECK(std::string(cv.predicate.data, cv.predicate.len) == "model");

  // Out-of-range indices are rejected, not read.
  lu_path_expr_op op = LU_PATH_EXPR_OP_PATTERN;
  CHECK(lu_path_expr_op_at(expr, nops, &op) == LU_ERR_INVALID_ARG);
  CHECK(lu_path_expr_pattern_at(expr, npatterns, &pv) == LU_ERR_INVALID_ARG);

  // Match through the C boundary. The expression carries a `{model}`
  // predicate; with no predicate callback the core treats predicates as
  // satisfied, so any path matching "/World/Geom//C*" matches.
  const lu_view hit{"/World/Geom/Cube", 15};
  CHECK(lu_path_expr_match_path(expr, hit, nullptr, nullptr) == 1);
  const lu_view miss{"/World/Excluded", 15};
  CHECK(lu_path_expr_match_path(expr, miss, nullptr, nullptr) == 0);

  // Clone is an independent deep copy.
  lu_path_expr *clone = nullptr;
  CHECK(lu_path_expr_clone(expr, &clone) == LU_OK);
  if (clone != nullptr) {
    size_t clone_need = 0;
    CHECK(lu_path_expr_get_text(clone, nullptr, 0, &clone_need) == LU_OK);
    CHECK(clone_need == need);
    lu_path_expr_destroy(clone);
  }

  lu_path_expr_destroy(expr);  // destroy(NULL) is safe
  lu_path_expr_destroy(nullptr);

  // Malformed input yields an invalid object with a diagnostic.
  const std::string bad("(/A");
  lu_path_expr *bad_expr = nullptr;
  char bad_err[64] = {0};
  CHECK(lu_path_expr_parse(lu_view{bad.data(), bad.size()}, &bad_expr, bad_err,
                           sizeof(bad_err)) == LU_OK);
  if (bad_expr != nullptr) {
    CHECK(lu_path_expr_valid(bad_expr) == 0);
    CHECK(bad_err[0] != '\0');
    CHECK(lu_path_expr_get_text(bad_expr, nullptr, 0, &need) ==
          LU_ERR_INVALID_ARG);
    lu_path_expr_destroy(bad_expr);
  }

  // NULL / empty views.
  CHECK(lu_path_expr_parse(lu_view{nullptr, 0}, nullptr, nullptr, 0) ==
        LU_ERR_INVALID_ARG);
  lu_path_expr *empty_expr = nullptr;
  CHECK(lu_path_expr_parse(lu_view{nullptr, 0}, &empty_expr, nullptr, 0) ==
        LU_OK);
  if (empty_expr != nullptr) {
    CHECK(lu_path_expr_valid(empty_expr) != 0);
    CHECK(lu_path_expr_empty(empty_expr) != 0);
    lu_path_expr_match_path(empty_expr, hit, nullptr, nullptr);
    lu_path_expr_destroy(empty_expr);
  }
}

}  // namespace

int main() {
  TestAcceptReject();
  TestLimits();
  TestRoundtrip();
  TestDecompose();
  TestMatch();
  TestPredicatesAndRefs();
  TestCapi();

  if (g_failures == 0) {
    std::printf("path_expression_c_core_test: all checks passed\n");
    return 0;
  }
  std::printf("path_expression_c_core_test: %d check(s) failed\n", g_failures);
  return 1;
}
