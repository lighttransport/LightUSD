#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-path-expression.h"
#include "core/path-expression.hh"
#include "core/path-expression-eval.hh"
#include "lightusd.hh"
#include "tydra/scene-access.hh"
#include "tydra/render-data.hh"
#include "tydra/attribute-eval.hh"
#include "usdc-writer.hh"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

static bool Matches(const char *expr_text, const char *path,
                    const PathExpressionEvalContext &ctx = {}) {
  auto e = ParsedPathExpression::Parse(expr_text);
  return e.valid() && MatchPath(e, path, ctx);
}

// Structural equality of two parsed expressions (ops + patterns + refs).
static bool SameStructure(const ParsedPathExpression &a,
                          const ParsedPathExpression &b) {
  if (a.ops() != b.ops()) return false;
  if (a.patterns() != b.patterns()) return false;
  if (a.refs().size() != b.refs().size()) return false;
  for (size_t i = 0; i < a.refs().size(); i++) {
    if (a.refs()[i].path != b.refs()[i].path) return false;
    if (a.refs()[i].name != b.refs()[i].name) return false;
  }
  return true;
}

void path_expression_parse_test(void) {
  // Valid expressions parse.
  const char *valid[] = {
      "/World/Geom",
      "/World/Geom/*",
      "//",
      "/World//Foo",
      "/World//C*{model}",
      "~/World/Excluded",
      "/A + /B",
      "/A & /B",
      "/A - /B",
      "/A /B",
      "%/Lib:coll",
      "%_",
      "/root %:ref1 - %:ref2",
      "(/A + /B) & /C",
  };
  for (const char *s : valid) {
    std::string err;
    auto e = ParsedPathExpression::Parse(s, &err);
    TEST_CHECK_(e.valid(), "expected valid parse for '%s' (err=%s)", s,
                err.c_str());
  }

  // Empty string is a valid (empty) expression.
  {
    auto e = ParsedPathExpression::Parse("");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.empty());
  }

  // Malformed expressions fail.
  const char *invalid[] = {
      "(/A",     // unbalanced
      "/A +",    // trailing operator with no operand
  };
  for (const char *s : invalid) {
    auto e = ParsedPathExpression::Parse(s);
    TEST_CHECK_(!e.valid(), "expected parse failure for '%s'", s);
  }
}

void path_expression_limits_test(void) {
  {
    std::string deeply_nested;
    for (int i = 0; i < 300; i++) deeply_nested.push_back('(');
    deeply_nested += "/A";
    for (int i = 0; i < 300; i++) deeply_nested.push_back(')');
    std::string err;
    auto e = ParsedPathExpression::Parse(deeply_nested, &err);
    TEST_CHECK(!e.valid());
    TEST_CHECK(err.find("deep") != std::string::npos);
  }

  {
    std::string many_nodes;
    for (int i = 0; i < 3000; i++) {
      if (i) many_nodes += " ";
      many_nodes += "/A" + std::to_string(i);
    }
    std::string err;
    auto e = ParsedPathExpression::Parse(many_nodes, &err);
    TEST_CHECK(!e.valid());
    TEST_CHECK(err.find("too many nodes") != std::string::npos);
  }

  // Stretch matching uses memoization and remains linear in the table size.
  TEST_CHECK(Matches("/Root//Leaf", "/Root/A/B/C/D/E/F/G/H/I/J/Leaf"));
}

void path_expression_roundtrip_test(void) {
  // Exact canonical text round-trip for clean inputs.
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
  for (auto &t : exact) {
    auto e = ParsedPathExpression::Parse(t.in);
    TEST_CHECK_(e.valid(), "parse failed for '%s'", t.in);
    std::string got = e.GetText();
    TEST_CHECK_(got == t.out, "GetText('%s') = '%s', expected '%s'", t.in,
                got.c_str(), t.out);
  }

  // Round-trip stability: Parse -> GetText -> Parse yields same structure.
  const char *exprs[] = {
      "/root %:ref1 - %:ref2",
      "(/A + /B) & /C",
      "/CollectionTest/Geom//C* //Box",
      "~/World/Excluded + /World/Included",
  };
  for (const char *s : exprs) {
    auto e1 = ParsedPathExpression::Parse(s);
    TEST_CHECK_(e1.valid(), "parse failed for '%s'", s);
    auto e2 = ParsedPathExpression::Parse(e1.GetText());
    TEST_CHECK_(e2.valid(), "reparse failed for '%s' (text='%s')", s,
                e1.GetText().c_str());
    TEST_CHECK_(SameStructure(e1, e2),
                "round-trip changed structure for '%s' (text='%s')", s,
                e1.GetText().c_str());
  }
}

void path_expression_decompose_test(void) {
  // "//" => absolute-root prefix + single stretch.
  {
    auto e = ParsedPathExpression::Parse("//");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.patterns().size() == 1);
    const PathPattern &p = e.patterns()[0];
    TEST_CHECK(p.prefix == "/");
    TEST_CHECK(p.components.size() == 1);
    TEST_CHECK(p.components[0].is_stretch());
  }

  // "/World/Geom//C*{model}" => prefix folded, stretch, wildcard+predicate.
  {
    auto e = ParsedPathExpression::Parse("/World/Geom//C*{model}");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.patterns().size() == 1);
    const PathPattern &p = e.patterns()[0];
    TEST_CHECK(p.prefix == "/World/Geom");
    TEST_CHECK(p.components.size() == 2);
    TEST_CHECK(p.components[0].is_stretch());
    TEST_CHECK(p.components[1].text == "C*");
    TEST_CHECK(p.components[1].predicate == "model");
  }

  // Difference + implied-union + references.
  {
    auto e = ParsedPathExpression::Parse("/root %:ref1 - %:ref2");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.refs().size() == 2);
    TEST_CHECK(e.patterns().size() == 1);
    using Op = ParsedPathExpression::Op;
    // Prefix order: Difference, (ImpliedUnion, Pattern, Ref), Ref
    TEST_CHECK(e.ops().size() == 5);
    TEST_CHECK(e.ops()[0] == Op::Difference);
    TEST_CHECK(e.ops()[1] == Op::ImpliedUnion);
    TEST_CHECK(e.refs()[0].name == "ref1");
    TEST_CHECK(e.refs()[1].name == "ref2");
  }

  // Weaker reference.
  {
    auto e = ParsedPathExpression::Parse("%_");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.refs().size() == 1);
    TEST_CHECK(e.refs()[0].is_weaker());
  }
}

void path_expression_match_test(void) {
  // Prefix + single wildcard component.
  TEST_CHECK(Matches("/World/Geom/*", "/World/Geom/Sphere"));
  TEST_CHECK(!Matches("/World/Geom/*", "/World/Geom"));        // '*' needs a child
  TEST_CHECK(!Matches("/World/Geom/*", "/World/Geom/A/B"));    // single level only
  TEST_CHECK(!Matches("/World/Geom/*", "/Other/Geom/Sphere"));

  // Stretch (`//`) matches arbitrary depth, including direct child.
  TEST_CHECK(Matches("/World//Sphere", "/World/Sphere"));
  TEST_CHECK(Matches("/World//Sphere", "/World/A/B/Sphere"));
  TEST_CHECK(!Matches("/World//Sphere", "/World/A/Cube"));

  // "//" matches everything.
  TEST_CHECK(Matches("//", "/Foo"));
  TEST_CHECK(Matches("//", "/Foo/Bar/Baz"));

  // Glob wildcard within a component.
  TEST_CHECK(Matches("/World/C*", "/World/Cube"));
  TEST_CHECK(Matches("/World/C*", "/World/Cone"));
  TEST_CHECK(!Matches("/World/C*", "/World/Box"));

  // Union / difference / intersection / complement.
  TEST_CHECK(Matches("/World/A + /World/B", "/World/A"));
  TEST_CHECK(Matches("/World/A + /World/B", "/World/B"));
  TEST_CHECK(!Matches("/World/A + /World/B", "/World/C"));

  TEST_CHECK(Matches("/World//* - /World/Excluded", "/World/Foo"));
  TEST_CHECK(!Matches("/World//* - /World/Excluded", "/World/Excluded"));

  TEST_CHECK(Matches("/World//* & /World/Keep", "/World/Keep"));
  TEST_CHECK(!Matches("/World//* & /World/Keep", "/World/Other"));

  TEST_CHECK(!Matches("~/World/Excluded", "/World/Excluded"));
  TEST_CHECK(Matches("~/World/Excluded", "/World/Other"));

  // Property gating: a prim pattern must not match a property path.
  TEST_CHECK(!Matches("/World/Geom/*", "/World/Geom/Sphere.radius"));
}

void path_expression_predicate_ref_test(void) {
  // Predicate: only paths the callback approves match.
  {
    PathExpressionEvalContext ctx;
    ctx.eval_predicate = [](const std::string &pred,
                            const std::string &prim_path) {
      return pred == "mesh" && prim_path == "/World/A/Mesh1";
    };
    TEST_CHECK(Matches("/World//*{mesh}", "/World/A/Mesh1", ctx));
    TEST_CHECK(!Matches("/World//*{mesh}", "/World/A/Curve1", ctx));
  }

  // Expression reference resolution.
  {
    auto sub = ParsedPathExpression::Parse("/World/Lib//*");
    PathExpressionEvalContext ctx;
    ctx.resolve_ref =
        [&sub](const ExpressionReference &ref) -> const ParsedPathExpression * {
      if (ref.name == "lib") return &sub;
      return nullptr;
    };
    TEST_CHECK(Matches("%:lib", "/World/Lib/Thing", ctx));
    TEST_CHECK(!Matches("%:lib", "/World/Other/Thing", ctx));
    // Unresolved reference matches nothing.
    TEST_CHECK(!Matches("%:missing", "/World/Lib/Thing", ctx));
  }
}

void path_expression_collection_membership_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"World\"\n"
      "{\n"
      "    def Mesh \"Cube\" {}\n"
      "    def Mesh \"Cone\" {}\n"
      "    def Mesh \"Box\" {}\n"
      "    def Xform \"Asset\" (kind = \"component\") {}\n"
      "\n"
      "    uniform pathExpression collection:expr:membershipExpression = \"/World/C*\"\n"
      "    uniform pathExpression collection:base:membershipExpression = \"/World/C*\"\n"
      "    uniform pathExpression collection:alias:membershipExpression = \"%:base\"\n"
      "    uniform pathExpression collection:outer:membershipExpression = \"%/World:alias\"\n"
      "    uniform pathExpression collection:models:membershipExpression = \"/World//*{component}\"\n"
      "    rel collection:rel:includes = [</World>]\n"
      "    rel collection:rel:excludes = [</World/Box>]\n"
      "}\n";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      &stage, &warn, &err);
  TEST_CHECK_(ok, "USDA parse failed: %s", err.c_str());
  if (!ok) return;

  auto wret = stage.GetPrimAtPath(Path("/World", ""));
  TEST_CHECK_(bool(wret), "missing /World");
  if (!wret) return;
  const Collection *coll = nullptr;
  TEST_CHECK_(lightusd::legacy::tydra::GetCollection(*wret.value(), &coll) && coll,
              "no collection on /World");
  if (!coll) return;

  // Expression mode: "/World/C*"
  {
    const CollectionInstance *inst = nullptr;
    TEST_CHECK(coll->get_instance("expr", &inst) && inst);
    if (inst) {
      auto q = lightusd::legacy::tydra::BuildCollectionMembershipQuery(stage, *inst, "/World");
      TEST_CHECK(q.mode == lightusd::legacy::tydra::CollectionMembershipQuery::Mode::Expression);
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cube", "")));
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cone", "")));
      TEST_CHECK(!lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Box", "")));
    }
  }

  // Nested expression refs preserve the referenced collection's owner:
  // outer -> absolute alias -> same-owner base.
  {
    const CollectionInstance *inst = nullptr;
    TEST_CHECK(coll->get_instance("outer", &inst) && inst);
    if (inst) {
      auto q = lightusd::legacy::tydra::BuildCollectionMembershipQuery(stage, *inst, "/World");
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cube", "")));
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cone", "")));
      TEST_CHECK(!lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Box", "")));
    }
  }

  // Expression mode with predicate: "/World//*{component}"
  {
    const CollectionInstance *inst = nullptr;
    TEST_CHECK(coll->get_instance("models", &inst) && inst);
    if (inst) {
      auto q = lightusd::legacy::tydra::BuildCollectionMembershipQuery(stage, *inst, "/World");
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Asset", "")));
      TEST_CHECK(!lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cube", "")));
    }
  }

  // Predicate helper directly.
  TEST_CHECK(lightusd::legacy::tydra::EvalPathExpressionPredicate(stage, "component", "/World/Asset"));
  TEST_CHECK(!lightusd::legacy::tydra::EvalPathExpressionPredicate(stage, "component", "/World/Cube"));
  TEST_CHECK(lightusd::legacy::tydra::EvalPathExpressionPredicate(stage, "isa:Mesh", "/World/Cube"));

  // Relationship mode: includes /World, excludes /World/Box.
  {
    const CollectionInstance *inst = nullptr;
    TEST_CHECK(coll->get_instance("rel", &inst) && inst);
    if (inst) {
      auto q = lightusd::legacy::tydra::BuildCollectionMembershipQuery(stage, *inst, "/World");
      TEST_CHECK(q.mode == lightusd::legacy::tydra::CollectionMembershipQuery::Mode::Relationship);
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cube", "")));
      TEST_CHECK(lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Cone", "")));
      TEST_CHECK(!lightusd::legacy::tydra::IsPathIncluded(q, stage, Path("/World/Box", "")));
    }
  }
}

void path_expression_light_linking_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      // Non-sphere light schemas must expose their inherited CollectionAPI;
      // ApplyToCollection historically dispatched SphereLight only.
      "def DistantLight \"Linked\"\n"
      "{\n"
      "    uniform pathExpression collection:lightLink:membershipExpression = \"/World/Geom/Lit*\"\n"
      "}\n"
      "\n"
      "def SphereLight \"All\"\n"
      "{\n"
      "}\n";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      &stage, &warn, &err);
  TEST_CHECK_(ok, "USDA parse failed: %s", err.c_str());
  if (!ok) return;

  // Manually assemble a RenderScene with three meshes and the two lights.
  lightusd::legacy::tydra::RenderScene scene;
  {
    lightusd::legacy::tydra::RenderMesh m;
    m.abs_path = "/World/Geom/LitA";
    scene.meshes.push_back(m);
    m.abs_path = "/World/Geom/LitB";
    scene.meshes.push_back(m);
    m.abs_path = "/World/Geom/Dark";
    scene.meshes.push_back(m);
  }
  {
    lightusd::legacy::tydra::RenderLight l;
    l.abs_path = "/Linked";
    scene.lights.push_back(l);
    l.abs_path = "/All";
    scene.lights.push_back(l);
  }

  size_t resolved = lightusd::legacy::tydra::ResolveLightLinking(stage, &scene);
  TEST_CHECK_(resolved == 1, "expected 1 light resolved, got %zu", resolved);

  // Light "Linked": illuminates LitA(0), LitB(1) but not Dark(2).
  const lightusd::legacy::tydra::RenderLight &linked = scene.lights[0];
  TEST_CHECK(!linked.light_links_all);
  TEST_CHECK(linked.light_link_mesh_indices.size() == 2);
  bool has0 = false, has1 = false, has2 = false;
  for (int idx : linked.light_link_mesh_indices) {
    if (idx == 0) has0 = true;
    if (idx == 1) has1 = true;
    if (idx == 2) has2 = true;
  }
  TEST_CHECK(has0 && has1 && !has2);

  // Light "All": no link collection -> illuminates everything (default).
  const lightusd::legacy::tydra::RenderLight &all = scene.lights[1];
  TEST_CHECK(all.light_links_all);
  TEST_CHECK(all.light_link_mesh_indices.empty());
}

void path_expression_crate_roundtrip_test(void) {
  // A `pathExpression` attribute should survive a USDC (crate-57) round-trip.
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"World\"\n"
      "{\n"
      "    uniform pathExpression primvars:expr = \"/World/Geom//C*\"\n"
      "}\n";

  Stage stage;
  std::string w, e;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      &stage, &w, &e);
  TEST_CHECK_(ok, "USDA parse failed: %s", e.c_str());
  if (!ok) return;

  std::vector<uint8_t> usdc;
  std::string w2, e2;
  bool sret = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &usdc, &w2, &e2);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e2.c_str());
  if (!sret) return;

  // Boot header: ident[8] then version[8]; byte 9 = version minor. A
  // PathExpression value must bump the emitted crate version to >= 0.10.0.
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 10, "crate version minor = %d, expected >= 10", usdc[9]);

  Stage stage2;
  std::string w3, e3;
  bool lret = lightusd::legacy::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w3, &e3);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e3.c_str());
  if (!lret) return;

  auto pr = stage2.GetPrimAtPath(Path("/World", ""));
  TEST_CHECK(bool(pr));
  if (pr && pr.value()) {
    lightusd::legacy::tydra::TerminalAttributeValue tav;
    std::string ee;
    bool ev = lightusd::legacy::tydra::EvaluateAttribute(stage2, *pr.value(), "primvars:expr",
                                       &tav, &ee);
    TEST_CHECK_(ev, "eval failed: %s", ee.c_str());
    const lightusd::legacy::value::PathExpression *pe = tav.as<lightusd::legacy::value::PathExpression>();
    TEST_CHECK(pe != nullptr);
    if (pe) {
      TEST_CHECK_(pe->GetText() == "/World/Geom//C*",
                  "pathExpression mismatch after crate roundtrip: '%s'",
                  pe->GetText().c_str());
    }
  }
}

void path_expression_usda_roundtrip_test(void) {
  // A `pathExpression`-typed attribute should parse and re-emit its text.
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def \"World\"\n"
      "{\n"
      "    custom pathExpression primvars:expr = \"/World/Geom//C*{model}\"\n"
      "}\n";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      &stage, &warn, &err);
  TEST_CHECK_(ok, "USDA parse failed: %s", err.c_str());
  if (!ok) return;

  std::string exported = stage.ExportToString();
  TEST_CHECK_(exported.find("pathExpression") != std::string::npos,
              "exported USDA missing pathExpression type. Output:\n%s",
              exported.c_str());
  TEST_CHECK_(exported.find("/World/Geom//C*{model}") != std::string::npos,
              "exported USDA missing pathExpression value. Output:\n%s",
              exported.c_str());
}

// ---------------------------------------------------------------------------
// Extended grammar / round-trip coverage.
// ---------------------------------------------------------------------------
void path_expression_grammar_extended_test(void) {
  // Round-trip stability (Parse -> GetText -> Parse) over a broad grammar set.
  const char *exprs[] = {
      "/A",
      "/A/B/C",
      "//",
      "/A//",
      "/A//B//C",
      "/World/Geom/*",
      "/World/C?be",
      "/World//*{model}",
      "/World//{kind:component}",
      "/A.attr",
      "/A//B.visibility",
      "~/A",
      "~(~/A)",
      "/A + /B",
      "/A - /B",
      "/A & /B",
      "/A /B",
      "/A + /B + /C",
      "(/A + /B) & /C",
      "/A & (/B - /C)",
      "~(/A + /B)",
      "%:coll",
      "%/Lib:coll",
      "%_",
      "/root %:r1 - %:r2",
      "/W//Mesh* & ~/W//Excluded",
  };
  for (const char *s : exprs) {
    auto e1 = ParsedPathExpression::Parse(s);
    TEST_CHECK_(e1.valid(), "parse failed: '%s' (%s)", s, e1.error().c_str());
    auto e2 = ParsedPathExpression::Parse(e1.GetText());
    TEST_CHECK_(e2.valid(), "reparse failed: '%s' -> '%s'", s,
                e1.GetText().c_str());
    TEST_CHECK_(SameStructure(e1, e2), "round-trip changed structure: '%s'", s);
  }

  // Predicate text is captured verbatim.
  {
    auto e = ParsedPathExpression::Parse("/W//*{kind:component}");
    TEST_CHECK(e.valid());
    TEST_CHECK(e.patterns().size() == 1);
    const PathPattern &p = e.patterns()[0];
    TEST_CHECK(!p.components.empty());
    TEST_CHECK(p.components.back().predicate == "kind:component");
  }

  // Grouped double complement (per OpenUSD grammar, a factor takes at most one
  // `~`; nesting requires parentheses: `~(~/A/B)`).
  {
    auto e = ParsedPathExpression::Parse("~(~/A/B)");
    TEST_CHECK(e.valid());
    using Op = ParsedPathExpression::Op;
    int complements = 0;
    for (auto op : e.ops()) {
      if (op == Op::Complement) complements++;
    }
    TEST_CHECK(complements == 2);
  }

  // Malformed inputs fail cleanly (no crash, valid()==false).
  const char *invalid[] = {
      "(/A",        // unbalanced (
      "/A)",        // unbalanced )
      "/A +",       // dangling binary op
      "& /A",       // leading binary op
      "/A & & /B",  // doubled operator
  };
  for (const char *s : invalid) {
    auto e = ParsedPathExpression::Parse(s);
    TEST_CHECK_(!e.valid(), "expected parse failure: '%s'", s);
  }
}

// ---------------------------------------------------------------------------
// Extended matcher coverage (structural, no predicates).
// ---------------------------------------------------------------------------
void path_expression_matcher_extended_test(void) {
  // '?' single-character wildcard.
  TEST_CHECK(Matches("/W/C?be", "/W/Cube"));
  TEST_CHECK(!Matches("/W/C?be", "/W/Cone"));   // 'one' != '?be'
  TEST_CHECK(!Matches("/W/C?be", "/W/Cube2"));  // one char only

  // Stretch at multiple positions.
  TEST_CHECK(Matches("/A//B//C", "/A/x/B/y/z/C"));
  TEST_CHECK(Matches("/A//B//C", "/A/B/C"));
  TEST_CHECK(!Matches("/A//B//C", "/A/x/C"));  // no B

  // Trailing stretch matches the prefix subtree.
  TEST_CHECK(Matches("/W//", "/W/a/b"));
  TEST_CHECK(Matches("/W//", "/W"));
  TEST_CHECK(!Matches("/W//", "/Other"));

  // Set algebra.
  TEST_CHECK(Matches("(/W/A + /W/B) & /W/A", "/W/A"));
  TEST_CHECK(!Matches("(/W/A + /W/B) & /W/A", "/W/B"));
  TEST_CHECK(Matches("/W//* - (/W/X + /W/Y)", "/W/Z"));
  TEST_CHECK(!Matches("/W//* - (/W/X + /W/Y)", "/W/X"));
  TEST_CHECK(!Matches("/W//* - (/W/X + /W/Y)", "/W/Y"));

  // Complement of a union.
  TEST_CHECK(!Matches("~(/W/A + /W/B)", "/W/A"));
  TEST_CHECK(Matches("~(/W/A + /W/B)", "/W/C"));

  // Property gating: a prim pattern never matches a property path.
  TEST_CHECK(!Matches("/W//*", "/W/A.attr"));
  // A property pattern matches the property and not the bare prim.
  TEST_CHECK(Matches("/W/A.visibility", "/W/A.visibility"));
  TEST_CHECK(!Matches("/W/A.visibility", "/W/A"));
}

// ---------------------------------------------------------------------------
// Predicate library (against a real Stage).
// ---------------------------------------------------------------------------
void path_expression_predicate_library_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"Asset\" (kind = \"component\")\n"
      "{\n"
      "    def Scope \"Grp\" (kind = \"group\")\n"
      "    {\n"
      "    }\n"
      "    def Mesh \"M\" {}\n"
      "    over \"Ov\" {}\n"
      "    class \"Cls\" {}\n"
      "}\n";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      &stage, &warn, &err);
  TEST_CHECK_(ok, "USDA parse failed: %s", err.c_str());
  if (!ok) return;

  using lightusd::legacy::tydra::EvalPathExpressionPredicate;
  auto P = [&](const char *pred, const char *path) {
    return EvalPathExpressionPredicate(stage, pred, path);
  };

  // defined / abstract (Prim::specifier() is populated on both USDA and USDC load)
  TEST_CHECK(P("defined", "/Asset"));
  TEST_CHECK(!P("defined", "/Asset/Ov"));   // over
  TEST_CHECK(!P("defined", "/Asset/Cls"));  // class
  TEST_CHECK(P("abstract", "/Asset/Cls"));
  TEST_CHECK(!P("abstract", "/Asset"));

  // model / kind
  TEST_CHECK(P("model", "/Asset"));     // has kind
  TEST_CHECK(!P("model", "/Asset/M"));  // no kind
  TEST_CHECK(P("component", "/Asset"));
  TEST_CHECK(P("group", "/Asset/Grp"));
  TEST_CHECK(P("kind:component", "/Asset"));
  TEST_CHECK(P("kind:group", "/Asset/Grp"));
  TEST_CHECK(!P("kind:component", "/Asset/Grp"));

  // isa (schema type)
  TEST_CHECK(P("isa:Mesh", "/Asset/M"));
  TEST_CHECK(P("isa:Xform", "/Asset"));
  TEST_CHECK(!P("isa:Mesh", "/Asset"));

  // active (default true)
  TEST_CHECK(P("active", "/Asset/M"));

  // unknown predicate -> conservatively false; missing prim -> false
  TEST_CHECK(!P("bogusPredicate", "/Asset"));
  TEST_CHECK(!P("defined", "/DoesNotExist"));

  // End-to-end: an expression predicate selects only matching prims.
  auto e = ParsedPathExpression::Parse("/Asset//{kind:group}");
  TEST_CHECK(e.valid());
  PathExpressionEvalContext ctx;
  ctx.eval_predicate = [&](const std::string &pred, const std::string &pp) {
    return EvalPathExpressionPredicate(stage, pred, pp);
  };
  TEST_CHECK(MatchPath(e, "/Asset/Grp", ctx));
  TEST_CHECK(!MatchPath(e, "/Asset/M", ctx));
}

// ---------------------------------------------------------------------------
// Cross-tool: read an OpenUSD-authored crate (type 57) and confirm the
// pathExpression value decodes. Fixture generated by `usdcat` (crate 0.10.0).
// ---------------------------------------------------------------------------
namespace {
std::string PEFixture(const std::string &rel) {
  if (const char *root = std::getenv("LIGHTUSD_TEST_FIXTURE_DIR")) {
    if (*root != '\0') {
      const std::string candidate = std::string(root) + "/" + rel;
      std::ifstream f(candidate, std::ios::binary);
      if (f.good()) return candidate;
    }
  }

  // Tests may run from the repo root or from build/.
  const char *prefixes[] = {"", "../", "../../"};
  for (const char *p : prefixes) {
    std::string cand = std::string(p) + rel;
    std::ifstream f(cand, std::ios::binary);
    if (f.good()) return cand;
  }
  return rel;
}
}  // namespace

void path_expression_openusd_crate_read_test(void) {
  const std::string path =
      PEFixture("tests/unit/fixtures/openusd/pathexpr_openusd.usdc");
  std::ifstream probe(path, std::ios::binary);
  if (!probe.good()) {
    TEST_MSG("fixture not found: %s (skipping)", path.c_str());
    return;  // fixture missing in this checkout layout
  }

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDCFromFile(path, &stage, &warn, &err);
  TEST_CHECK_(ok, "LoadUSDCFromFile(%s) failed: %s", path.c_str(), err.c_str());
  if (!ok) return;

  auto pr = stage.GetPrimAtPath(Path("/World", ""));
  TEST_CHECK(bool(pr));
  if (pr && pr.value()) {
    lightusd::legacy::tydra::TerminalAttributeValue tav;
    std::string ee;
    bool ev = lightusd::legacy::tydra::EvaluateAttribute(stage, *pr.value(), "primvars:expr",
                                       &tav, &ee);
    TEST_CHECK_(ev, "eval failed: %s", ee.c_str());
    const lightusd::legacy::value::PathExpression *pe = tav.as<lightusd::legacy::value::PathExpression>();
    TEST_CHECK(pe != nullptr);
    if (pe) {
      TEST_CHECK_(pe->GetText() == "/World/Geom//C*",
                  "OpenUSD-authored pathExpression mismatch: '%s'",
                  pe->GetText().c_str());
    }
  }
}
