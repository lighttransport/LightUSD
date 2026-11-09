#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include <cmath>

#include "unit-composition-arcs.h"
#include "composition.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "core/path-expression-eval.hh"
#include "layer.hh"
#include "io-util.hh"
#include "lightusd.hh"

#include <fstream>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {

bool WriteTextFileForCompositionTest(const std::string &path,
                                     const std::string &content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs) {
    return false;
  }
  ofs << content;
  return bool(ofs);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. comp_inherits_child_prims_test
// /Base has child "SubChild" with prop "childVal"=42.
// /Derived inherits /Base. After CompositeInherits, Derived should have
// child "SubChild" with childVal.
// ---------------------------------------------------------------------------
void comp_inherits_child_prims_test(void) {
  Layer layer;

  // -- /Base (class) with child SubChild --
  PrimSpec base(Specifier::Class, "Scope", "Base");
  {
    PrimSpec subchild(Specifier::Def, "Scope", "SubChild");
    Attribute attr;
    attr.set_value(42);
    attr.set_type_name("int");
    subchild.props()["childVal"] = Property(attr, /* custom */ false);
    base.children().push_back(subchild);
  }
  layer.add_primspec("Base", base);

  // -- /Derived inherits /Base --
  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  // Verify /Derived exists in result
  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  // Verify /Derived has child named "SubChild"
  const auto &children = it->second.children();
  bool found_subchild = false;
  for (const auto &child : children) {
    if (child.name() == "SubChild") {
      found_subchild = true;
      // Verify childVal property exists and equals 42
      TEST_CHECK(child.props().count("childVal") > 0);
      if (child.props().count("childVal")) {
        const Property &p = child.props().at("childVal");
        TEST_CHECK(p.is_attribute());
        if (p.is_attribute()) {
          auto v = p.get_attribute().get_value<int>();
          TEST_CHECK(v.has_value());
          if (v.has_value()) {
            TEST_CHECK(v.value() == 42);
          }
        }
      }
      break;
    }
  }
  TEST_CHECK(found_subchild);
  if (!found_subchild) {
    TEST_MSG("SubChild not found in Derived after CompositeInherits");
  }
}

// ---------------------------------------------------------------------------
// 2. comp_inherits_multiple_bases_test
// /Derived inherits from [</Base1>, </Base2>]. Both bases' properties appear.
// ---------------------------------------------------------------------------
void comp_inherits_multiple_bases_test(void) {
  Layer layer;

  // -- /Base1 --
  PrimSpec base1(Specifier::Class, "Scope", "Base1");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    base1.props()["fromBase1"] = Property(attr, false);
  }
  layer.add_primspec("Base1", base1);

  // -- /Base2 --
  PrimSpec base2(Specifier::Class, "Scope", "Base2");
  {
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    base2.props()["fromBase2"] = Property(attr, false);
  }
  layer.add_primspec("Base2", base2);

  // -- /Derived inherits both --
  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::ResetToExplicit,
                   {Path("/Base1", ""), Path("/Base2", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);

  // Multiple inheritance is now supported.
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  // Verify /Derived has properties from both bases.
  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("fromBase1") > 0);
    TEST_CHECK(it->second.props().count("fromBase2") > 0);

    // Verify values
    if (it->second.props().count("fromBase1")) {
      const auto &a = it->second.props().at("fromBase1").get_attribute();
      auto v = a.get_value<int>();
      TEST_CHECK(v.has_value() && v.value() == 1);
    }
    if (it->second.props().count("fromBase2")) {
      const auto &a = it->second.props().at("fromBase2").get_attribute();
      auto v = a.get_value<int>();
      TEST_CHECK(v.has_value() && v.value() == 2);
    }
  }
}

// ---------------------------------------------------------------------------
// 3. comp_specializes_child_prims_test
// Same as inherits child propagation but via specializes arc.
// ---------------------------------------------------------------------------
void comp_specializes_child_prims_test(void) {
  Layer layer;

  // -- /Base with child SubChild --
  PrimSpec base(Specifier::Def, "Scope", "Base");
  {
    PrimSpec subchild(Specifier::Def, "Scope", "SubChild");
    Attribute attr;
    attr.set_value(99);
    attr.set_type_name("int");
    subchild.props()["childVal"] = Property(attr, false);
    base.children().push_back(subchild);
  }
  layer.add_primspec("Base", base);

  // -- /Specialized specializes /Base --
  PrimSpec specialized(Specifier::Def, "Scope", "Specialized");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> spec;
    spec.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
    specialized.metas().specializes = spec;
  }
  layer.add_primspec("Specialized", specialized);

  Layer result;
  std::string warn, err;
  bool ok = CompositeSpecializes(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeSpecializes failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Specialized");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  const auto &children = it->second.children();
  bool found_subchild = false;
  for (const auto &child : children) {
    if (child.name() == "SubChild") {
      found_subchild = true;
      TEST_CHECK(child.props().count("childVal") > 0);
      if (child.props().count("childVal")) {
        auto v = child.props().at("childVal").get_attribute().get_value<int>();
        TEST_CHECK(v.has_value());
        if (v.has_value()) {
          TEST_CHECK(v.value() == 99);
        }
      }
      break;
    }
  }
  TEST_CHECK(found_subchild);
  if (!found_subchild) {
    TEST_MSG("SubChild not found in Specialized after CompositeSpecializes");
  }

  // specializes metadata should be cleared
  TEST_CHECK(!it->second.metas().specializes.has_value());
}

// ---------------------------------------------------------------------------
// 4. comp_variant_two_sets_test
// Two variantSets on one PrimSpec; select from each; both applied.
// ---------------------------------------------------------------------------
void comp_variant_two_sets_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");

  // variantSets metadata: color, size
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"color", "size"}});
    root.metas().variantSets = vsets;
  }

  // variant selection: color=red, size=large
  {
    VariantSelectionMap vsmap;
    vsmap["color"] = "red";
    vsmap["size"] = "large";
    root.metas().variants = vsmap;
  }

  // color variantSet
  {
    VariantSetSpec vss;
    vss.name = "color";

    PrimSpec red(Specifier::Def, "", "red");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("red"));
      attr.set_type_name("token");
      red.props()["colorName"] = Property(attr, false);
    }
    vss.variantSet["red"] = red;

    PrimSpec blue(Specifier::Def, "", "blue");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("blue"));
      attr.set_type_name("token");
      blue.props()["colorName"] = Property(attr, false);
    }
    vss.variantSet["blue"] = blue;

    root.variantSets()["color"] = vss;
  }

  // size variantSet
  {
    VariantSetSpec vss;
    vss.name = "size";

    PrimSpec large(Specifier::Def, "", "large");
    {
      Attribute attr;
      attr.set_value(10.0f);
      attr.set_type_name("float");
      large.props()["sizeVal"] = Property(attr, false);
    }
    vss.variantSet["large"] = large;

    PrimSpec small_v(Specifier::Def, "", "small");
    {
      Attribute attr;
      attr.set_value(1.0f);
      attr.set_type_name("float");
      small_v.props()["sizeVal"] = Property(attr, false);
    }
    vss.variantSet["small"] = small_v;

    root.variantSets()["size"] = vss;
  }

  layer.add_primspec("Root", root);

  Layer result;
  std::string warn, err;
  bool ok = CompositeVariant(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeVariant failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  // Verify colorName = "red" from color variant
  TEST_CHECK(it->second.props().count("colorName") > 0);
  if (it->second.props().count("colorName")) {
    auto v = it->second.props().at("colorName").get_attribute().get_value<lightusd::legacy::value::token>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value().str() == "red");
    }
  }

  // Verify sizeVal = 10.0f from size variant
  TEST_CHECK(it->second.props().count("sizeVal") > 0);
  if (it->second.props().count("sizeVal")) {
    auto v = it->second.props().at("sizeVal").get_attribute().get_value<float>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 10.0f);
    }
  }

  // variant metadata should be cleared
  TEST_CHECK(!it->second.metas().variants.has_value());
  TEST_CHECK(!it->second.metas().variantSets.has_value());
  TEST_CHECK(it->second.variantSets().empty());
}

// ---------------------------------------------------------------------------
// 5. comp_variant_no_selection_test
// VariantSet exists but no selection; CompositeVariant should handle
// gracefully without crashing or applying variants.
// ---------------------------------------------------------------------------
void comp_variant_no_selection_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");

  // variantSets metadata
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"myVariant"}});
    root.metas().variantSets = vsets;
  }

  // No variant selection set (root.metas().variants is not set)

  // Set up variantSet content
  {
    VariantSetSpec vss;
    vss.name = "myVariant";

    PrimSpec optA(Specifier::Def, "", "optionA");
    {
      Attribute attr;
      attr.set_value(100);
      attr.set_type_name("int");
      optA.props()["val"] = Property(attr, false);
    }
    vss.variantSet["optionA"] = optA;

    root.variantSets()["myVariant"] = vss;
  }

  // Add a pre-existing property on root to ensure it survives
  {
    Attribute attr;
    attr.set_value(7);
    attr.set_type_name("int");
    root.props()["existing"] = Property(attr, false);
  }

  layer.add_primspec("Root", root);

  Layer result;
  std::string warn, err;
  bool ok = CompositeVariant(layer, &result, &warn, &err);
  // Should succeed (no crash, no error)
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeVariant with no selection failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  // The variant property should NOT be applied (no selection)
  TEST_CHECK(it->second.props().count("val") == 0);

  // existing property should still be there
  TEST_CHECK(it->second.props().count("existing") > 0);
}

// ---------------------------------------------------------------------------
// 6. comp_variant_nested_children_test
// Variant with child PrimSpecs; children appear after selection.
// ---------------------------------------------------------------------------
void comp_variant_nested_children_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");

  // variantSets metadata
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"shapeVariant"}});
    root.metas().variantSets = vsets;
  }

  // selection: shapeVariant=optionA
  {
    VariantSelectionMap vsmap;
    vsmap["shapeVariant"] = "optionA";
    root.metas().variants = vsmap;
  }

  // variantSet with child PrimSpec
  {
    VariantSetSpec vss;
    vss.name = "shapeVariant";

    PrimSpec optA(Specifier::Def, "", "optionA");
    {
      // Nested child inside variant
      PrimSpec nested(Specifier::Def, "Scope", "Nested");
      Attribute attr;
      attr.set_value(42);
      attr.set_type_name("int");
      nested.props()["nestedVal"] = Property(attr, false);
      optA.children().push_back(nested);
    }
    vss.variantSet["optionA"] = optA;

    PrimSpec optB(Specifier::Def, "", "optionB");
    vss.variantSet["optionB"] = optB;

    root.variantSets()["shapeVariant"] = vss;
  }

  layer.add_primspec("Root", root);

  Layer result;
  std::string warn, err;
  bool ok = CompositeVariant(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeVariant failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  // /Root should have child "Nested" from selected variant
  const auto &children = it->second.children();
  bool found_nested = false;
  for (const auto &child : children) {
    if (child.name() == "Nested") {
      found_nested = true;
      TEST_CHECK(child.props().count("nestedVal") > 0);
      if (child.props().count("nestedVal")) {
        auto v = child.props().at("nestedVal").get_attribute().get_value<int>();
        TEST_CHECK(v.has_value());
        if (v.has_value()) {
          TEST_CHECK(v.value() == 42);
        }
      }
      break;
    }
  }
  TEST_CHECK(found_nested);
  if (!found_nested) {
    TEST_MSG("Nested child not found in Root after CompositeVariant");
  }
}

// ---------------------------------------------------------------------------
// comp_reference_suffix_fallback_test
// References authored against another machine's layout (UE USD exports):
// escaping '../' runs and Windows drive-prefixed paths. The resolver suffix
// fallback must rebase them onto the search paths (scene root), with a
// rebase warning; a genuinely-missing asset still composes (skipped) with an
// "Asset not found" warning.
// ---------------------------------------------------------------------------
void comp_reference_suffix_fallback_test(void) {
  const std::string dir = lightusd::io::JoinPath(
      lightusd::io::GetTempDir(), "lightusd_comp_reference_suffix_fallback");
  const std::string assets = lightusd::io::JoinPath(dir, "Assets");
  TEST_CHECK(lightusd::io::CreateDirectories(assets));
  if (!lightusd::io::IsDirectory(assets)) {
    return;
  }

  const std::string leaf_usda = R"(#usda 1.0
(
    defaultPrim = "Leaf"
)

def Xform "Leaf"
{
    custom token marker = "leaf"
}
)";

  TEST_CHECK(WriteTextFileForCompositionTest(
      lightusd::io::JoinPath(assets, "leaf.usda"), leaf_usda));
  TEST_CHECK(WriteTextFileForCompositionTest(
      lightusd::io::JoinPath(assets, "leaf2.usda"), leaf_usda));

  const std::string root_usda = R"(#usda 1.0

def Xform "RefEscape" (
    prepend references = @../../../../../USD_Exports/SampleScene/Assets/leaf.usda@
)
{
}

def Xform "RefDrive" (
    prepend references = @F:/USD_Exports/SampleScene/Assets/leaf2.usda@
)
{
}

def Xform "RefMissing" (
    prepend references = @../../missing_dir/missing.usda@
)
{
}
)";

  const std::string root_path = lightusd::io::JoinPath(dir, "root.usda");
  TEST_CHECK(WriteTextFileForCompositionTest(root_path, root_usda));

  Layer root_layer;
  std::string warn, err;
  TEST_CHECK(LoadLayerFromFile(root_path, &root_layer, &warn, &err));
  if (!err.empty()) {
    TEST_MSG("LoadLayerFromFile error: %s", err.c_str());
  }

  AssetResolutionResolver resolver;
  resolver.set_current_working_path(dir);
  resolver.set_search_paths({dir});

  ReferencesCompositionOptions options;
  options.allow_parent_relative_paths = true;

  Layer composited;
  warn.clear();
  err.clear();
  TEST_CHECK(CompositeReferences(resolver, root_layer, &composited, &warn,
                                 &err, options));
  if (!err.empty()) {
    TEST_MSG("CompositeReferences error: %s", err.c_str());
  }

  // Escaping '../' reference rebased onto <dir>/Assets/leaf.usda.
  auto escape_it = composited.primspecs().find("RefEscape");
  TEST_CHECK(escape_it != composited.primspecs().end());
  if (escape_it != composited.primspecs().end()) {
    TEST_CHECK(escape_it->second.props().count("marker") == 1);
  }

  // Drive-prefixed reference rebased onto <dir>/Assets/leaf2.usda.
  auto drive_it = composited.primspecs().find("RefDrive");
  TEST_CHECK(drive_it != composited.primspecs().end());
  if (drive_it != composited.primspecs().end()) {
    TEST_CHECK(drive_it->second.props().count("marker") == 1);
  }

  // Rebases are surfaced as warnings.
  TEST_CHECK(warn.find("suffix fallback") != std::string::npos);
  if (warn.find("suffix fallback") == std::string::npos) {
    TEST_MSG("warn: %s", warn.c_str());
  }

  // Genuinely-missing asset: composition succeeds, prim left as-authored,
  // not-found warning surfaced.
  auto missing_it = composited.primspecs().find("RefMissing");
  TEST_CHECK(missing_it != composited.primspecs().end());
  if (missing_it != composited.primspecs().end()) {
    TEST_CHECK(missing_it->second.props().count("marker") == 0);
  }
  TEST_CHECK(warn.find("Asset not found") != std::string::npos);
}

// ---------------------------------------------------------------------------
// comp_sublayer_variant_sets_merge_test
// Stronger and weaker sublayers can author different variantSets and selection
// entries on the same prim. The composed prim must keep all set names so all
// selected variants are applied.
// ---------------------------------------------------------------------------
void comp_sublayer_variant_sets_merge_test(void) {
  const std::string dir = lightusd::io::JoinPath(
      lightusd::io::GetTempDir(), "lightusd_comp_sublayer_variant_sets_merge");
  TEST_CHECK(lightusd::io::CreateDirectories(dir));
  if (!lightusd::io::IsDirectory(dir)) {
    return;
  }

  const std::string weak_path = lightusd::io::JoinPath(dir, "weak.usda");
  const std::string root_path = lightusd::io::JoinPath(dir, "root.usda");

  const std::string weak_usda = R"(#usda 1.0

over "Root" (
    variants = {
        string look = "uvgrid"
    }
    prepend variantSets = "look"
)
{
    variantSet "look" = {
        "uvgrid" {
            over "Geom"
            {
                custom token lookMarker = "uvgrid"
            }
        }
    }
}

)";

  const std::string root_usda = R"(#usda 1.0
(
    subLayers = [
        @weak.usda@
    ]
)

def Xform "Root" (
    variants = {
        string shape = "subdiv"
    }
    prepend variantSets = "shape"
)
{
    def Xform "Geom"
    {
    }
    variantSet "shape" = {
        "subdiv" {
            over "Geom"
            {
                custom token shapeMarker = "subdiv"
            }
        }
    }
}
)";

  TEST_CHECK(WriteTextFileForCompositionTest(weak_path, weak_usda));
  TEST_CHECK(WriteTextFileForCompositionTest(root_path, root_usda));

  Layer root_layer;
  std::string warn, err;
  TEST_CHECK(LoadLayerFromFile(root_path, &root_layer, &warn, &err));
  if (!err.empty()) {
    TEST_MSG("LoadLayerFromFile error: %s", err.c_str());
  }

  AssetResolutionResolver resolver;
  Layer sublayer_composited;
  TEST_CHECK(CompositeSublayers(resolver, root_layer, &sublayer_composited,
                                &warn, &err));
  if (!err.empty()) {
    TEST_MSG("CompositeSublayers error: %s", err.c_str());
  }

  auto root_it = sublayer_composited.primspecs().find("Root");
  TEST_CHECK(root_it != sublayer_composited.primspecs().end());
  if (root_it == sublayer_composited.primspecs().end()) {
    return;
  }

  const PrimSpec &root = root_it->second;
  TEST_CHECK(root.metas().variants.has_value());
  TEST_CHECK(root.metas().variantSets.has_value());
  TEST_CHECK(root.metas().variants->count("shape") == 1);
  TEST_CHECK(root.metas().variants->count("look") == 1);

  bool has_shape_set = false;
  bool has_look_set = false;
  if (root.metas().variantSets) {
    for (const auto &op : root.metas().variantSets.value()) {
      for (const auto &name : op.second) {
        has_shape_set = has_shape_set || (name == "shape");
        has_look_set = has_look_set || (name == "look");
      }
    }
  }
  TEST_CHECK(has_shape_set);
  TEST_CHECK(has_look_set);

  Layer variant_composited;
  TEST_CHECK(CompositeVariant(sublayer_composited, &variant_composited, &warn,
                              &err));
  if (!err.empty()) {
    TEST_MSG("CompositeVariant error: %s", err.c_str());
  }

  auto final_root_it = variant_composited.primspecs().find("Root");
  TEST_CHECK(final_root_it != variant_composited.primspecs().end());
  if (final_root_it == variant_composited.primspecs().end()) {
    return;
  }

  auto geom_it = std::find_if(final_root_it->second.children().begin(),
                              final_root_it->second.children().end(),
                              [](const PrimSpec &ps) {
                                return ps.name() == "Geom";
                              });
  TEST_CHECK(geom_it != final_root_it->second.children().end());
  if (geom_it == final_root_it->second.children().end()) {
    return;
  }

  TEST_CHECK(geom_it->props().count("shapeMarker") == 1);
  TEST_CHECK(geom_it->props().count("lookMarker") == 1);

  lightusd::io::RemoveFile(root_path);
  lightusd::io::RemoveFile(weak_path);
  lightusd::io::RemoveAll(dir);
}

void comp_sublayer_path_expression_weaker_test(void) {
  const std::string dir = lightusd::io::JoinPath(
      lightusd::io::GetTempDir(), "lightusd_comp_path_expr_weaker");
  TEST_CHECK(lightusd::io::CreateDirectories(dir));
  if (!lightusd::io::IsDirectory(dir)) return;
  const std::string weak_path = lightusd::io::JoinPath(dir, "weak.usda");
  const std::string root_path = lightusd::io::JoinPath(dir, "root.usda");
  const std::string weak_usda = R"(#usda 1.0
def Xform "World"
{
    uniform pathExpression collection:test:membershipExpression = "/World/A"
}
)";
  const std::string root_usda = R"(#usda 1.0
(
    subLayers = [@weak.usda@]
)
over "World"
{
    uniform pathExpression collection:test:membershipExpression = "%_ + /World/B"
}
)";
  TEST_CHECK(WriteTextFileForCompositionTest(weak_path, weak_usda));
  TEST_CHECK(WriteTextFileForCompositionTest(root_path, root_usda));
  Layer root_layer;
  std::string warn, err;
  TEST_CHECK(LoadLayerFromFile(root_path, &root_layer, &warn, &err));
  AssetResolutionResolver resolver;
  Layer composed;
  TEST_CHECK(CompositeSublayers(resolver, root_layer, &composed, &warn, &err));
  auto world = composed.primspecs().find("World");
  TEST_CHECK(world != composed.primspecs().end());
  if (world == composed.primspecs().end()) return;
  auto prop = world->second.props().find(
      "collection:test:membershipExpression");
  TEST_CHECK(prop != world->second.props().end());
  if (prop == world->second.props().end() || !prop->second.is_attribute()) return;
  auto expr = prop->second.get_attribute().get_value<lightusd::legacy::value::PathExpression>();
  TEST_CHECK(expr.has_value());
  if (!expr) return;
  TEST_CHECK(expr->GetText().find("%_") == std::string::npos);
  const ParsedPathExpression parsed = ParsedPathExpression::Parse(expr->GetText());
  TEST_CHECK(parsed.valid());
  TEST_CHECK(MatchPath(parsed, "/World/A"));
  TEST_CHECK(MatchPath(parsed, "/World/B"));
  TEST_CHECK(!MatchPath(parsed, "/World/C"));
  lightusd::io::RemoveFile(root_path);
  lightusd::io::RemoveFile(weak_path);
  lightusd::io::RemoveAll(dir);
}

// ---------------------------------------------------------------------------
// 7. comp_override_basic_test
// Create a /Base (def) and a /Base (over). Use OverridePrimSpec to merge
// the over into def. Verify the override properties are merged.
// ---------------------------------------------------------------------------
void comp_override_basic_test(void) {
  PrimSpec dst(Specifier::Def, "Scope", "Base");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    dst.props()["origProp"] = Property(attr, false);
  }

  PrimSpec src(Specifier::Over, "Scope", "Base");
  {
    Attribute attr;
    attr.set_value(100);
    attr.set_type_name("int");
    src.props()["overProp"] = Property(attr, false);
  }

  std::string warn, err;
  bool ok = OverridePrimSpec(dst, src, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("OverridePrimSpec failed: %s", err.c_str());
    return;
  }

  // origProp should still exist
  TEST_CHECK(dst.props().count("origProp") > 0);
  if (dst.props().count("origProp")) {
    auto v = dst.props().at("origProp").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 1);
    }
  }

  // overProp should be merged from src
  TEST_CHECK(dst.props().count("overProp") > 0);
  if (dst.props().count("overProp")) {
    auto v = dst.props().at("overProp").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 100);
    }
  }
}

// ---------------------------------------------------------------------------
// 8. comp_override_replace_property_test
// Over replaces existing property value.
// ---------------------------------------------------------------------------
void comp_override_replace_property_test(void) {
  PrimSpec dst(Specifier::Def, "Scope", "Thing");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    dst.props()["val"] = Property(attr, false);
  }

  PrimSpec src(Specifier::Over, "Scope", "Thing");
  {
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    src.props()["val"] = Property(attr, false);
  }

  std::string warn, err;
  bool ok = OverridePrimSpec(dst, src, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("OverridePrimSpec failed: %s", err.c_str());
    return;
  }

  // val should be replaced with 2
  TEST_CHECK(dst.props().count("val") > 0);
  if (dst.props().count("val")) {
    auto v = dst.props().at("val").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 2);
      if (v.value() != 2) {
        TEST_MSG("Expected val=2, got %d", v.value());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 9. comp_override_add_new_property_test
// Over adds a new property that didn't exist in dst.
// ---------------------------------------------------------------------------
void comp_override_add_new_property_test(void) {
  PrimSpec dst(Specifier::Def, "Scope", "Thing");
  {
    Attribute attr;
    attr.set_value(10);
    attr.set_type_name("int");
    dst.props()["existing"] = Property(attr, false);
  }

  PrimSpec src(Specifier::Over, "Scope", "Thing");
  {
    Attribute attr;
    attr.set_value(3.14f);
    attr.set_type_name("float");
    src.props()["newProp"] = Property(attr, false);
  }

  std::string warn, err;
  bool ok = OverridePrimSpec(dst, src, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("OverridePrimSpec failed: %s", err.c_str());
    return;
  }

  // existing should remain
  TEST_CHECK(dst.props().count("existing") > 0);
  if (dst.props().count("existing")) {
    auto v = dst.props().at("existing").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 10);
    }
  }

  // newProp should be added
  TEST_CHECK(dst.props().count("newProp") > 0);
  if (dst.props().count("newProp")) {
    auto v = dst.props().at("newProp").get_attribute().get_value<float>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 3.14f);
    }
  }
}

// ---------------------------------------------------------------------------
// 10. comp_inherit_primspec_api_test
// Direct InheritPrimSpec() call. Create base with props, derived without.
// InheritPrimSpec(derived, base). Verify derived got base's props.
// ---------------------------------------------------------------------------
void comp_inherit_primspec_api_test(void) {
  PrimSpec base(Specifier::Class, "Scope", "Base");
  {
    Attribute attr;
    attr.set_value(77);
    attr.set_type_name("int");
    base.props()["baseVal"] = Property(attr, false);

    Attribute attr2;
    attr2.set_value(2.5f);
    attr2.set_type_name("float");
    base.props()["baseFloat"] = Property(attr2, false);
  }

  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  // derived starts empty

  std::string warn, err;
  bool ok = InheritPrimSpec(derived, base, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("InheritPrimSpec failed: %s", err.c_str());
    return;
  }

  // derived should have both base properties
  TEST_CHECK(derived.props().count("baseVal") > 0);
  TEST_CHECK(derived.props().count("baseFloat") > 0);

  if (derived.props().count("baseVal")) {
    auto v = derived.props().at("baseVal").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 77);
    }
  }

  if (derived.props().count("baseFloat")) {
    auto v = derived.props().at("baseFloat").get_attribute().get_value<float>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 2.5f);
    }
  }
}

// ---------------------------------------------------------------------------
// 11. comp_override_primspec_api_test
// Direct OverridePrimSpec() call. Create def with props, over with different
// props. OverridePrimSpec(def, over). Verify merge.
// ---------------------------------------------------------------------------
void comp_override_primspec_api_test(void) {
  PrimSpec dst(Specifier::Def, "Scope", "Prim");
  {
    Attribute attr;
    attr.set_value(lightusd::legacy::value::token("original"));
    attr.set_type_name("token");
    dst.props()["name"] = Property(attr, false);

    Attribute attr2;
    attr2.set_value(100);
    attr2.set_type_name("int");
    dst.props()["count"] = Property(attr2, false);
  }

  PrimSpec src(Specifier::Over, "Scope", "Prim");
  {
    // Override name
    Attribute attr;
    attr.set_value(lightusd::legacy::value::token("overridden"));
    attr.set_type_name("token");
    src.props()["name"] = Property(attr, false);

    // Add new property
    Attribute attr2;
    attr2.set_value(true);
    attr2.set_type_name("bool");
    src.props()["active"] = Property(attr2, false);
  }

  std::string warn, err;
  bool ok = OverridePrimSpec(dst, src, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("OverridePrimSpec failed: %s", err.c_str());
    return;
  }

  // "name" should be overridden
  TEST_CHECK(dst.props().count("name") > 0);
  if (dst.props().count("name")) {
    auto v = dst.props().at("name").get_attribute().get_value<lightusd::legacy::value::token>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value().str() == "overridden");
    }
  }

  // "count" should be untouched
  TEST_CHECK(dst.props().count("count") > 0);
  if (dst.props().count("count")) {
    auto v = dst.props().at("count").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 100);
    }
  }

  // "active" should be added
  TEST_CHECK(dst.props().count("active") > 0);
  if (dst.props().count("active")) {
    auto v = dst.props().at("active").get_attribute().get_value<bool>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == true);
    }
  }
}

// ---------------------------------------------------------------------------
// 12. comp_has_references_test
// Test HasReferences() with layer that has references and one without.
// ---------------------------------------------------------------------------
void comp_has_references_test(void) {
  // Empty layer should not have references.
  // Use force_check=true since the default cached flag may not reflect the
  // actual state of a freshly-constructed Layer.
  {
    Layer empty_layer;
    TEST_CHECK(!HasReferences(empty_layer, /* force_check */ true));
  }

  // Layer without references
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "Plain");
    layer.add_primspec("Plain", ps);
    TEST_CHECK(!HasReferences(layer, true));
  }

  // Layer with references
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "Ref");

    Reference ref;
    ref.asset_path = lightusd::legacy::value::AssetPath("other.usda");
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> refs;
    refs.push_back({ListEditQual::ResetToExplicit, {ref}});
    ps.metas().references = refs;

    layer.add_primspec("Ref", ps);

    TEST_CHECK(HasReferences(layer, /* force_check */ true));
  }
}

// ---------------------------------------------------------------------------
// 13. comp_has_payload_test
// Test HasPayload() with layer that has payload and one without.
// ---------------------------------------------------------------------------
void comp_has_payload_test(void) {
  // Empty layer should not have payload.
  // Use force_check=true since the default cached flag may not reflect the
  // actual state of a freshly-constructed Layer.
  {
    Layer empty_layer;
    TEST_CHECK(!HasPayload(empty_layer, /* force_check */ true));
  }

  // Layer without payload
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "NoPay");
    layer.add_primspec("NoPay", ps);
    TEST_CHECK(!HasPayload(layer, true));
  }

  // Layer with payload
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "Pay");

    Payload pl;
    pl.asset_path = lightusd::legacy::value::AssetPath("payload.usda");
    std::vector<std::pair<ListEditQual, std::vector<Payload>>> payloads;
    payloads.push_back({ListEditQual::ResetToExplicit, {pl}});
    ps.metas().payload = payloads;

    layer.add_primspec("Pay", ps);

    TEST_CHECK(HasPayload(layer, /* force_check */ true));
  }
}

// ---------------------------------------------------------------------------
// 14. comp_has_over_test
// Create layer with Specifier::Over PrimSpec. HasOver returns true.
// Without returns false.
// ---------------------------------------------------------------------------
void comp_has_over_test(void) {
  // Empty layer
  {
    Layer empty_layer;
    TEST_CHECK(!HasOver(empty_layer));
  }

  // Layer with only def PrimSpec
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "DefPrim");
    layer.add_primspec("DefPrim", ps);
    TEST_CHECK(!HasOver(layer));
  }

  // Layer with over PrimSpec
  {
    Layer layer;
    PrimSpec ps(Specifier::Over, "Scope", "OverPrim");
    layer.add_primspec("OverPrim", ps);
    TEST_CHECK(HasOver(layer));
  }

  // Layer with class PrimSpec (should not count as over)
  {
    Layer layer;
    PrimSpec ps(Specifier::Class, "Scope", "ClassPrim");
    layer.add_primspec("ClassPrim", ps);
    TEST_CHECK(!HasOver(layer));
  }
}

// ---------------------------------------------------------------------------
// 15. comp_extract_variants_from_layer_test
// Set up Layer with variant metadata, call ExtractVariants(Layer, dict, err),
// verify Dictionary contents.
// ---------------------------------------------------------------------------
void comp_extract_variants_from_layer_test(void) {
  Layer layer;

  PrimSpec ps(Specifier::Def, "Scope", "Cube");

  // variantSets
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"colorVariant"}});
    ps.metas().variantSets = vsets;
  }

  // variant selection
  {
    VariantSelectionMap vsmap;
    vsmap["colorVariant"] = "green";
    ps.metas().variants = vsmap;
  }

  // Set up VariantSetSpec
  {
    VariantSetSpec vss;
    vss.name = "colorVariant";

    PrimSpec green(Specifier::Def, "", "green");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("green"));
      attr.set_type_name("token");
      green.props()["color"] = Property(attr, false);
    }
    vss.variantSet["green"] = green;

    ps.variantSets()["colorVariant"] = vss;
  }

  layer.add_primspec("Cube", ps);

  Dictionary dict;
  std::string err;
  bool ok = ExtractVariants(layer, &dict, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("ExtractVariants(Layer) failed: %s", err.c_str());
    return;
  }

  // Dictionary should contain entries (non-empty if variants found)
  // The exact structure depends on implementation but it should not be empty
  // for a layer with variants.
  TEST_CHECK(!dict.empty());
}

// ---------------------------------------------------------------------------
// 16. comp_extract_variants_from_stage_test
// Parse simple USDA with variants via LoadUSDAFromMemory, then call
// ExtractVariants(Stage, dict, err).
// ---------------------------------------------------------------------------
void comp_extract_variants_from_stage_test(void) {
  const std::string usda = R"(#usda 1.0

def Scope "Root" (
    variants = {
        string shapeVariant = "cube"
    }
    prepend variantSets = "shapeVariant"
)
{
    variantSet "shapeVariant" = {
        "cube" {
            int cubeSize = 10
        }
        "sphere" {
            float sphereRadius = 5.0
        }
    }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "", &stage, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());
    return;
  }

  Dictionary dict;
  ok = ExtractVariants(stage, &dict, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("ExtractVariants(Stage) failed: %s", err.c_str());
    return;
  }

  // Dictionary should contain variant info
  TEST_CHECK(!dict.empty());
}

// ---------------------------------------------------------------------------
// 17. comp_layer_to_stage_test
// Build Layer with PrimSpecs, call LayerToStage, verify Stage has prims
// at correct paths.
// ---------------------------------------------------------------------------
void comp_layer_to_stage_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    root.props()["rootVal"] = Property(attr, false);
  }
  layer.add_primspec("Root", root);

  PrimSpec another(Specifier::Def, "Scope", "Another");
  {
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    another.props()["anotherVal"] = Property(attr, false);
  }
  layer.add_primspec("Another", another);

  Stage stage;
  std::string warn, err;
  bool ok = LayerToStage(std::move(layer), &stage, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("LayerToStage failed: %s", err.c_str());
    return;
  }

  // Stage should have root prims
  TEST_CHECK(stage.root_prims().size() >= 2);

  // Check /Root exists
  {
    Path path("/Root", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "Root");
    }
  }

  // Check /Another exists
  {
    Path path("/Another", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "Another");
    }
  }
}

// ---------------------------------------------------------------------------
// 18. comp_list_variant_selections_test
// Set up Layer with variant selections, call ListVariantSelectionMaps,
// verify returned map.
// ---------------------------------------------------------------------------
void comp_list_variant_selections_test(void) {
  Layer layer;

  // /Model with variant selection: { lodVariant: "high" }
  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    VariantSelectionMap vsm;
    vsm["lodVariant"] = "high";
    model.metas().variants = vsm;
  }
  layer.add_primspec("Model", model);

  // /Plain — no variants
  PrimSpec plain(Specifier::Def, "Scope", "Plain");
  layer.add_primspec("Plain", plain);

  VariantSelectorMap m;
  bool ok = ListVariantSelectionMaps(layer, m);
  TEST_CHECK(ok);

  // Should have one entry for /Model
  TEST_CHECK(m.size() == 1);
  if (m.size() == 1) {
    auto it = m.find(Path("/Model", ""));
    TEST_CHECK(it != m.end());
    if (it != m.end()) {
      TEST_CHECK(it->second.vsmap.count("lodVariant") > 0);
      if (it->second.vsmap.count("lodVariant")) {
        TEST_CHECK(it->second.vsmap.at("lodVariant") == "high");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 19. comp_variant_select_primspec_test
// Direct VariantSelectPrimSpec() call. Create PrimSpec with variantSets,
// call with selection map, verify correct variant applied.
// ---------------------------------------------------------------------------
void comp_variant_select_primspec_test(void) {
  PrimSpec src(Specifier::Def, "Scope", "Root");

  // Set up variantSet
  {
    VariantSetSpec vss;
    vss.name = "matVariant";

    PrimSpec metal(Specifier::Def, "", "metal");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("metallic"));
      attr.set_type_name("token");
      metal.props()["material"] = Property(attr, false);
    }
    vss.variantSet["metal"] = metal;

    PrimSpec wood(Specifier::Def, "", "wood");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("wooden"));
      attr.set_type_name("token");
      wood.props()["material"] = Property(attr, false);
    }
    vss.variantSet["wood"] = wood;

    src.variantSets()["matVariant"] = vss;
  }

  // variantSets metadata
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"matVariant"}});
    src.metas().variantSets = vsets;
  }

  // variant selection metadata (required for VariantSelectPrimSpec to enter
  // the composition path)
  {
    VariantSelectionMap vsmap;
    vsmap["matVariant"] = "metal";
    src.metas().variants = vsmap;
  }

  PrimSpec dst(Specifier::Def, "Scope", "Root");

  std::map<std::string, std::string> selection;
  selection["matVariant"] = "metal";

  std::string warn, err;
  bool ok = VariantSelectPrimSpec(dst, src, selection, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("VariantSelectPrimSpec failed: %s", err.c_str());
    return;
  }

  // dst should have material="metallic" from metal variant
  TEST_CHECK(dst.props().count("material") > 0);
  if (dst.props().count("material")) {
    auto v = dst.props().at("material").get_attribute().get_value<lightusd::legacy::value::token>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value().str() == "metallic");
    }
  }
}

// ---------------------------------------------------------------------------
// 20. comp_apply_variant_selector_test
// Set up Layer with variants, call ApplyVariantSelector with
// VariantSelectorMap.
// ---------------------------------------------------------------------------
void comp_apply_variant_selector_test(void) {
  Layer layer;

  // /Model with variantSet "colorVariant" containing "red" and "blue" variants.
  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    // Set up variantSets metadata
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vs_ops;
    vs_ops.push_back({ListEditQual::ResetToExplicit, {"colorVariant"}});
    model.metas().variantSets = vs_ops;

    // Set variant selection to "red"
    VariantSelectionMap vsm;
    vsm["colorVariant"] = "red";
    model.metas().variants = vsm;

    // Add variantSet content
    VariantSetSpec vss;
    vss.name = "colorVariant";

    {
      PrimSpec red(Specifier::Def, "", "red");
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("red_color"));
      attr.set_type_name("token");
      red.props()["color"] = Property(attr, false);
      vss.variantSet["red"] = red;
    }

    {
      PrimSpec blue(Specifier::Def, "", "blue");
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("blue_color"));
      attr.set_type_name("token");
      blue.props()["color"] = Property(attr, false);
      vss.variantSet["blue"] = blue;
    }

    model.variantSets()["colorVariant"] = vss;
  }
  layer.add_primspec("Model", model);

  // Test 1: Apply with the layer's own selection ("red")
  {
    VariantSelectorMap vsmap;
    ListVariantSelectionMaps(layer, vsmap);

    Layer result;
    std::string warn, err;
    bool ok = ApplyVariantSelector(layer, vsmap, &result, &warn, &err);
    TEST_CHECK(ok);
    if (!ok) {
      TEST_MSG("ApplyVariantSelector failed: %s", err.c_str());
    }
    if (ok) {
      auto it = result.primspecs().find("Model");
      TEST_CHECK(it != result.primspecs().end());
      if (it != result.primspecs().end()) {
        TEST_CHECK(it->second.props().count("color") > 0);
        if (it->second.props().count("color")) {
          auto v = it->second.props().at("color").get_attribute().get_value<lightusd::legacy::value::token>();
          TEST_CHECK(v.has_value());
          if (v.has_value()) {
            TEST_CHECK(v.value().str() == "red_color");
          }
        }
      }
    }
  }

  // Test 2: Override selection to "blue"
  {
    VariantSelectorMap vsmap;
    VariantSelector sel;
    sel.selection = "blue";
    sel.vsmap["colorVariant"] = "blue";
    vsmap[Path("/Model", "")] = sel;

    Layer result;
    std::string warn, err;
    bool ok = ApplyVariantSelector(layer, vsmap, &result, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      auto it = result.primspecs().find("Model");
      TEST_CHECK(it != result.primspecs().end());
      if (it != result.primspecs().end()) {
        TEST_CHECK(it->second.props().count("color") > 0);
        if (it->second.props().count("color")) {
          auto v = it->second.props().at("color").get_attribute().get_value<lightusd::legacy::value::token>();
          TEST_CHECK(v.has_value());
          if (v.has_value()) {
            TEST_CHECK(v.value().str() == "blue_color");
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 21. comp_implied_inherits_test
// When /Derived references a prim that has `inherits = [/Base]`, and /Base
// also exists in the referencing layer, /Derived should get implied inherits
// from the local /Base.
// ---------------------------------------------------------------------------
void comp_implied_inherits_test(void) {
  // Build a "referenced" layer: /RefPrim inherits /RefBase
  Layer ref_layer;

  PrimSpec ref_base(Specifier::Class, "Scope", "RefBase");
  {
    Attribute attr;
    attr.set_value(42);
    attr.set_type_name("int");
    ref_base.props()["baseVal"] = Property(attr, false);
  }
  ref_layer.add_primspec("RefBase", ref_base);

  PrimSpec ref_prim(Specifier::Def, "Scope", "RefPrim");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::Prepend, {Path("/RefBase", "")}});
    ref_prim.metas().inherits = inh;

    Attribute attr;
    attr.set_value(100);
    attr.set_type_name("int");
    ref_prim.props()["primVal"] = Property(attr, false);
  }
  ref_layer.add_primspec("RefPrim", ref_prim);

  // Now simulate what happens when we inherit from ref_prim into a local prim.
  // The local layer has /LocalBase (matching /RefBase concept) and /Model.
  Layer local_layer;

  PrimSpec local_base(Specifier::Class, "Scope", "RefBase");
  {
    Attribute attr;
    attr.set_value(999);
    attr.set_type_name("int");
    local_base.props()["localBaseVal"] = Property(attr, false);
  }
  local_layer.add_primspec("RefBase", local_base);

  // /Model gets ref_prim's content via InheritPrimSpec + implied inherits
  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    // Simulate PropagateImpliedArcPaths: ref_prim has inherits = [/RefBase]
    // so model gets inheritPaths = [/RefBase]
    std::vector<std::pair<ListEditQual, std::vector<Path>>> implied;
    implied.push_back({ListEditQual::Prepend, {Path("/RefBase", "")}});
    model.metas().inheritPaths = implied;

    // Also give it some content from the reference
    Attribute attr;
    attr.set_value(100);
    attr.set_type_name("int");
    model.props()["primVal"] = Property(attr, false);
  }
  local_layer.add_primspec("Model", model);

  // Run CompositeInherits — should pick up implied inherits from inheritPaths
  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(local_layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  // /Model should have localBaseVal from the implied inherit
  auto it = result.primspecs().find("Model");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("localBaseVal") > 0);
    TEST_CHECK(it->second.props().count("primVal") > 0);
    // inheritPaths should be consumed
    TEST_CHECK(!it->second.metas().inheritPaths.has_value());
  }
}

// ---------------------------------------------------------------------------
// 22. comp_inherits_cycle_detection_test
// Test that self-inherits and mutual inherits don't crash (harmless no-ops
// in our flattening model). Also verify the visited set works for the
// reference cycle case (tested via the depth limit safety net).
// ---------------------------------------------------------------------------
void comp_inherits_cycle_detection_test(void) {
  // Test 1: Self-inherit (/A inherits /A) is a harmless no-op
  {
    Layer layer;

    PrimSpec a(Specifier::Def, "Scope", "A");
    {
      Attribute attr;
      attr.set_value(42);
      attr.set_type_name("int");
      a.props()["val"] = Property(attr, false);

      std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
      inh.push_back({ListEditQual::Prepend, {Path("/A", "")}});
      a.metas().inherits = inh;
    }
    layer.add_primspec("A", a);

    Layer result;
    std::string warn, err;
    bool ok = CompositeInherits(layer, &result, &warn, &err);
    // Self-inherit is harmless — A gets its own properties (no-op)
    TEST_CHECK(ok);
    if (ok) {
      auto it = result.primspecs().find("A");
      TEST_CHECK(it != result.primspecs().end());
      if (it != result.primspecs().end()) {
        // Property should still be there
        TEST_CHECK(it->second.props().count("val") > 0);
      }
    }
  }

  // Test 2: Mutual inherit (/A inherits /B, /B inherits /A) — doesn't crash
  {
    Layer layer;

    PrimSpec a(Specifier::Def, "Scope", "A");
    {
      Attribute attr;
      attr.set_value(1);
      attr.set_type_name("int");
      a.props()["fromA"] = Property(attr, false);

      std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
      inh.push_back({ListEditQual::Prepend, {Path("/B", "")}});
      a.metas().inherits = inh;
    }
    layer.add_primspec("A", a);

    PrimSpec b(Specifier::Def, "Scope", "B");
    {
      Attribute attr;
      attr.set_value(2);
      attr.set_type_name("int");
      b.props()["fromB"] = Property(attr, false);

      std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
      inh.push_back({ListEditQual::Prepend, {Path("/A", "")}});
      b.metas().inherits = inh;
    }
    layer.add_primspec("B", b);

    Layer result;
    std::string warn, err;
    bool ok = CompositeInherits(layer, &result, &warn, &err);
    // Mutual inherits succeed in our flattening model — each gets the other's props
    TEST_CHECK(ok);
  }
}

// ---------------------------------------------------------------------------
// 23. comp_specializes_cycle_detection_test
// Self-specialize is a harmless no-op in our flattening model.
// ---------------------------------------------------------------------------
void comp_specializes_cycle_detection_test(void) {
  Layer layer;

  PrimSpec a(Specifier::Def, "Scope", "A");
  {
    Attribute attr;
    attr.set_value(42);
    attr.set_type_name("int");
    a.props()["val"] = Property(attr, false);

    std::vector<std::pair<ListEditQual, std::vector<Path>>> sp;
    sp.push_back({ListEditQual::Prepend, {Path("/A", "")}});
    a.metas().specializes = sp;
  }
  layer.add_primspec("A", a);

  Layer result;
  std::string warn, err;
  bool ok = CompositeSpecializes(layer, &result, &warn, &err);
  // Self-specialize is a harmless no-op
  TEST_CHECK(ok);
  if (ok) {
    auto it = result.primspecs().find("A");
    TEST_CHECK(it != result.primspecs().end());
    if (it != result.primspecs().end()) {
      TEST_CHECK(it->second.props().count("val") > 0);
    }
  }
}

// ---------------------------------------------------------------------------
// 24. comp_active_false_filtering_test
// Build a layer, run CompositeAllArcs, verify that prims with active=false
// are removed from the result.
// ---------------------------------------------------------------------------
void comp_active_false_filtering_test(void) {
  Layer layer;

  PrimSpec visible(Specifier::Def, "Scope", "Visible");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    visible.props()["val"] = Property(attr, false);
  }
  layer.add_primspec("Visible", visible);

  PrimSpec inactive(Specifier::Def, "Scope", "Inactive");
  {
    inactive.metas().set_active(false);
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    inactive.props()["val"] = Property(attr, false);
  }
  layer.add_primspec("Inactive", inactive);

  // Also add a child with active=false
  PrimSpec parent(Specifier::Def, "Scope", "Parent");
  {
    PrimSpec child(Specifier::Def, "Scope", "ActiveChild");
    Attribute a1;
    a1.set_value(3);
    a1.set_type_name("int");
    child.props()["val"] = Property(a1, false);
    parent.children().push_back(child);

    PrimSpec dead_child(Specifier::Def, "Scope", "DeadChild");
    dead_child.metas().set_active(false);
    Attribute a2;
    a2.set_value(4);
    a2.set_type_name("int");
    dead_child.props()["val"] = Property(a2, false);
    parent.children().push_back(dead_child);
  }
  layer.add_primspec("Parent", parent);

  AssetResolutionResolver resolver;
  Layer result;
  std::string warn, err;
  bool ok = CompositeAllArcs(resolver, layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeAllArcs failed: %s", err.c_str());
    return;
  }

  // "Visible" should remain
  TEST_CHECK(result.primspecs().count("Visible") > 0);
  // "Inactive" root prim should be removed
  TEST_CHECK(result.primspecs().count("Inactive") == 0);
  // "Parent" should remain with only ActiveChild
  auto pit = result.primspecs().find("Parent");
  TEST_CHECK(pit != result.primspecs().end());
  if (pit != result.primspecs().end()) {
    // Should have 1 child (ActiveChild), not 2
    TEST_CHECK(pit->second.children().size() == 1);
    if (!pit->second.children().empty()) {
      TEST_CHECK(pit->second.children()[0].name() == "ActiveChild");
    }
  }
}

// ---------------------------------------------------------------------------
// 25. comp_multi_target_specializes_test
// /Derived specializes [/Base1, /Base2]. Verify both are applied.
// ---------------------------------------------------------------------------
void comp_multi_target_specializes_test(void) {
  Layer layer;

  PrimSpec base1(Specifier::Class, "Scope", "Base1");
  {
    Attribute attr;
    attr.set_value(10);
    attr.set_type_name("int");
    base1.props()["fromBase1"] = Property(attr, false);
  }
  layer.add_primspec("Base1", base1);

  PrimSpec base2(Specifier::Class, "Scope", "Base2");
  {
    Attribute attr;
    attr.set_value(20);
    attr.set_type_name("int");
    base2.props()["fromBase2"] = Property(attr, false);
  }
  layer.add_primspec("Base2", base2);

  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> sp;
    sp.push_back({ListEditQual::Prepend,
                  {Path("/Base1", ""), Path("/Base2", "")}});
    derived.metas().specializes = sp;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeSpecializes(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeSpecializes failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("fromBase1") > 0);
    TEST_CHECK(it->second.props().count("fromBase2") > 0);
  }
}

// ---------------------------------------------------------------------------
// 26. comp_inherits_append_listop_test
// Verify that append inherits fills in gaps (weaker than prepend).
// /Derived has local prop, prepend inherits from /Strong, append from /Weak.
// ---------------------------------------------------------------------------
void comp_inherits_append_listop_test(void) {
  Layer layer;

  PrimSpec strong(Specifier::Class, "Scope", "Strong");
  {
    Attribute a1, a2;
    a1.set_value(1);
    a1.set_type_name("int");
    strong.props()["shared"] = Property(a1, false);
    a2.set_value(10);
    a2.set_type_name("int");
    strong.props()["strongOnly"] = Property(a2, false);
  }
  layer.add_primspec("Strong", strong);

  PrimSpec weak(Specifier::Class, "Scope", "Weak");
  {
    Attribute a1, a2;
    a1.set_value(2);
    a1.set_type_name("int");
    weak.props()["shared"] = Property(a1, false);
    a2.set_value(20);
    a2.set_type_name("int");
    weak.props()["weakOnly"] = Property(a2, false);
  }
  layer.add_primspec("Weak", weak);

  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    // Prepend from /Strong (stronger among inherits), append from /Weak (weaker)
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::Prepend, {Path("/Strong", "")}});
    inh.push_back({ListEditQual::Append, {Path("/Weak", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // Both unique properties should exist
    TEST_CHECK(it->second.props().count("strongOnly") > 0);
    TEST_CHECK(it->second.props().count("weakOnly") > 0);
    // "shared" should come from Strong (prepend is processed first, fills in)
    TEST_CHECK(it->second.props().count("shared") > 0);
    if (it->second.props().count("shared")) {
      auto v = it->second.props().at("shared").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value());
      // The prepend inherit from /Strong sets "shared" = 1 first.
      // The append inherit from /Weak also has "shared" = 2, but since
      // InheritPrimSpec fills in gaps (doesn't override), the first value wins.
      if (v.has_value()) {
        TEST_CHECK(v.value() == 1);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 27. comp_implied_specializes_test
// When a prim has specializePaths propagated from a reference, and matching
// prims exist in the layer, implied specializes should be applied.
// ---------------------------------------------------------------------------
void comp_implied_specializes_test(void) {
  Layer layer;

  PrimSpec base_class(Specifier::Class, "Scope", "BaseClass");
  {
    Attribute attr;
    attr.set_value(77);
    attr.set_type_name("int");
    base_class.props()["impliedVal"] = Property(attr, false);
  }
  layer.add_primspec("BaseClass", base_class);

  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    // Simulate PropagateImpliedArcPaths: referenced prim had
    // specializes = [/BaseClass], so model gets specializePaths
    std::vector<std::pair<ListEditQual, std::vector<Path>>> implied;
    implied.push_back({ListEditQual::Prepend, {Path("/BaseClass", "")}});
    model.metas().specializePaths = implied;

    Attribute attr;
    attr.set_value(100);
    attr.set_type_name("int");
    model.props()["modelVal"] = Property(attr, false);
  }
  layer.add_primspec("Model", model);

  Layer result;
  std::string warn, err;
  bool ok = CompositeSpecializes(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeSpecializes failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Model");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // Should have impliedVal from the implied specialize
    TEST_CHECK(it->second.props().count("impliedVal") > 0);
    TEST_CHECK(it->second.props().count("modelVal") > 0);
    // specializePaths should be consumed
    TEST_CHECK(!it->second.metas().specializePaths.has_value());
  }
}

// ---------------------------------------------------------------------------
// 28. comp_liverps_integration_test
// End-to-end test: build a layer with inherits + variants + specializes,
// run CompositeAllArcs, verify correct LIVERPS ordering.
// ---------------------------------------------------------------------------
void comp_liverps_integration_test(void) {
  Layer layer;

  // /BaseClass: provides default val=100
  PrimSpec base_class(Specifier::Class, "Scope", "BaseClass");
  {
    Attribute attr;
    attr.set_value(100);
    attr.set_type_name("int");
    base_class.props()["val"] = Property(attr, false);

    Attribute only_base;
    only_base.set_value(42);
    only_base.set_type_name("int");
    base_class.props()["baseOnly"] = Property(only_base, false);
  }
  layer.add_primspec("BaseClass", base_class);

  // /SpecClass: provides specVal=77 (weaker than all)
  PrimSpec spec_class(Specifier::Class, "Scope", "SpecClass");
  {
    Attribute attr;
    attr.set_value(77);
    attr.set_type_name("int");
    spec_class.props()["specOnly"] = Property(attr, false);

    // Also provides val=77 — should be overridden by BaseClass inherit
    Attribute val;
    val.set_value(77);
    val.set_type_name("int");
    spec_class.props()["val"] = Property(val, false);
  }
  layer.add_primspec("SpecClass", spec_class);

  // /Model: inherits /BaseClass, specializes /SpecClass, has local val=1
  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    // Local opinion: val=1 (strongest)
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    model.props()["val"] = Property(attr, false);

    // Inherits: /BaseClass
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::Prepend, {Path("/BaseClass", "")}});
    model.metas().inherits = inh;

    // Specializes: /SpecClass
    std::vector<std::pair<ListEditQual, std::vector<Path>>> sp;
    sp.push_back({ListEditQual::Prepend, {Path("/SpecClass", "")}});
    model.metas().specializes = sp;
  }
  layer.add_primspec("Model", model);

  AssetResolutionResolver resolver;
  Layer result;
  std::string warn, err;
  bool ok = CompositeAllArcs(resolver, layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeAllArcs failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Model");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // val should be 1 (local opinion wins over both I and S)
    TEST_CHECK(it->second.props().count("val") > 0);
    if (it->second.props().count("val")) {
      auto v = it->second.props().at("val").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value() && v.value() == 1);
    }

    // baseOnly should come from inherits (I)
    TEST_CHECK(it->second.props().count("baseOnly") > 0);
    if (it->second.props().count("baseOnly")) {
      auto v = it->second.props().at("baseOnly").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value() && v.value() == 42);
    }

    // specOnly should come from specializes (S) — fills in the gap
    TEST_CHECK(it->second.props().count("specOnly") > 0);
    if (it->second.props().count("specOnly")) {
      auto v = it->second.props().at("specOnly").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value() && v.value() == 77);
    }

    // inherits and specializes metadata should be consumed
    TEST_CHECK(!it->second.metas().inherits.has_value());
    TEST_CHECK(!it->second.metas().specializes.has_value());
  }
}

// ---------------------------------------------------------------------------
// 29. comp_specializes_globally_weaker_test
// Verify that specializes opinions are weaker than inherits opinions.
// /Model inherits /Inherited, specializes /Specialized.
// Both provide the same property — inherits should win.
// ---------------------------------------------------------------------------
void comp_specializes_globally_weaker_test(void) {
  Layer layer;

  PrimSpec inherited(Specifier::Class, "Scope", "Inherited");
  {
    Attribute attr;
    attr.set_value(10);
    attr.set_type_name("int");
    inherited.props()["shared"] = Property(attr, false);
  }
  layer.add_primspec("Inherited", inherited);

  PrimSpec specialized(Specifier::Class, "Scope", "Specialized");
  {
    Attribute attr;
    attr.set_value(20);
    attr.set_type_name("int");
    specialized.props()["shared"] = Property(attr, false);
  }
  layer.add_primspec("Specialized", specialized);

  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::Prepend, {Path("/Inherited", "")}});
    model.metas().inherits = inh;

    std::vector<std::pair<ListEditQual, std::vector<Path>>> sp;
    sp.push_back({ListEditQual::Prepend, {Path("/Specialized", "")}});
    model.metas().specializes = sp;
  }
  layer.add_primspec("Model", model);

  AssetResolutionResolver resolver;
  Layer result;
  std::string warn, err;
  bool ok = CompositeAllArcs(resolver, layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeAllArcs failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Model");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("shared") > 0);
    if (it->second.props().count("shared")) {
      auto v = it->second.props().at("shared").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value());
      if (v.has_value()) {
        // Inherits (I) is stronger than Specializes (S) in LIVERPS.
        // I is applied first (fills in defaults), S applied last.
        // Since "shared" is already present from I, S should not override it.
        TEST_CHECK(v.value() == 10);
        TEST_MSG("Expected 10 (from inherits), got %d", v.value());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 30. comp_payload_load_policy_test
// Verify that the load_policy callback in PayloadCompositionOptions works.
// Set up a layer with payload metadata, use a policy that rejects loading.
// ---------------------------------------------------------------------------
void comp_payload_load_policy_test(void) {
  Layer layer;

  // Create a PrimSpec with a payload pointing to a nonexistent file.
  // With default options, this would fail. With a rejecting load_policy,
  // it should succeed because the payload is skipped.
  PrimSpec model(Specifier::Def, "Scope", "Model");
  {
    Payload pl;
    pl.asset_path = lightusd::legacy::value::AssetPath("nonexistent_heavy_geo.usd");
    pl.prim_path = Path("/Geo", "");

    std::vector<std::pair<ListEditQual, std::vector<Payload>>> payload_ops;
    payload_ops.push_back({ListEditQual::Prepend, {pl}});
    model.metas().payload = payload_ops;
  }
  layer.add_primspec("Model", model);

  // Test 1: Without load_policy, composition should fail or skip
  // (error_when_asset_not_found is false by default, so it should warn+skip)
  {
    AssetResolutionResolver resolver;
    Layer result;
    std::string warn, err;
    PayloadCompositionOptions opts;
    bool ok = CompositePayload(resolver, layer, &result, &warn, &err, opts);
    TEST_CHECK(ok);  // Should succeed (asset not found → skip)
  }

  // Test 2: With load_policy that rejects all payloads
  {
    AssetResolutionResolver resolver;
    Layer result;
    std::string warn, err;
    PayloadCompositionOptions opts;
    opts.load_policy = [](const Path &, const Payload &) { return false; };
    bool ok = CompositePayload(resolver, layer, &result, &warn, &err, opts);
    TEST_CHECK(ok);  // Should succeed — payload skipped by policy
    // The payload metadata should still be cleared after composition
    auto it = result.primspecs().find("Model");
    TEST_CHECK(it != result.primspecs().end());
    if (it != result.primspecs().end()) {
      TEST_CHECK(!it->second.metas().payload.has_value());
    }
  }

  // Test 3: With load_policy that accepts (error_when_asset_not_found=true)
  {
    AssetResolutionResolver resolver;
    Layer result;
    std::string warn, err;
    PayloadCompositionOptions opts;
    opts.error_when_asset_not_found = true;
    opts.load_policy = [](const Path &, const Payload &) { return true; };
    bool ok = CompositePayload(resolver, layer, &result, &warn, &err, opts);
    TEST_CHECK(!ok);  // Should fail — policy accepts but file doesn't exist
  }
}

// ---------------------------------------------------------------------------
// 31. comp_inherits_delete_listop_test
// Verify that `delete` ListEditQual removes paths from the inherits list.
// prepend [/A, /B], delete [/A] → only /B should be inherited.
// ---------------------------------------------------------------------------
void comp_inherits_delete_listop_test(void) {
  Layer layer;

  PrimSpec classA(Specifier::Class, "Scope", "A");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    classA.props()["fromA"] = Property(attr, false);
  }
  layer.add_primspec("A", classA);

  PrimSpec classB(Specifier::Class, "Scope", "B");
  {
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    classB.props()["fromB"] = Property(attr, false);
  }
  layer.add_primspec("B", classB);

  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    // Prepend both A and B
    inh.push_back({ListEditQual::Prepend, {Path("/A", ""), Path("/B", "")}});
    // Delete A
    inh.push_back({ListEditQual::Delete, {Path("/A", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // fromA should NOT be present (deleted from list)
    TEST_CHECK(it->second.props().count("fromA") == 0);
    // fromB should be present
    TEST_CHECK(it->second.props().count("fromB") > 0);
  }
}

// ---------------------------------------------------------------------------
// 32. comp_inherits_order_listop_test
// Verify that `order` ListEditQual reorders inherits targets.
// prepend [/A, /B, /C], order [/C, /A] → /C, /A applied before /B.
// Since all use InheritPrimSpec (first fills in), the order determines
// which properties "win" when there are conflicts.
// ---------------------------------------------------------------------------
void comp_inherits_order_listop_test(void) {
  Layer layer;

  PrimSpec classA(Specifier::Class, "Scope", "A");
  {
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    classA.props()["shared"] = Property(attr, false);
    Attribute only;
    only.set_value(10);
    only.set_type_name("int");
    classA.props()["onlyA"] = Property(only, false);
  }
  layer.add_primspec("A", classA);

  PrimSpec classB(Specifier::Class, "Scope", "B");
  {
    Attribute attr;
    attr.set_value(2);
    attr.set_type_name("int");
    classB.props()["shared"] = Property(attr, false);
    Attribute only;
    only.set_value(20);
    only.set_type_name("int");
    classB.props()["onlyB"] = Property(only, false);
  }
  layer.add_primspec("B", classB);

  PrimSpec classC(Specifier::Class, "Scope", "C");
  {
    Attribute attr;
    attr.set_value(3);
    attr.set_type_name("int");
    classC.props()["shared"] = Property(attr, false);
    Attribute only;
    only.set_value(30);
    only.set_type_name("int");
    classC.props()["onlyC"] = Property(only, false);
  }
  layer.add_primspec("C", classC);

  // /Derived: prepend [/A, /B, /C], then order [/C, /A]
  // Without order: resolved list = [/A, /B, /C] (prepend order)
  // With order: unordered items (/B) first, then ordered items (/C, /A)
  // Resolved list = [/B, /C, /A]
  // InheritPrimSpec processes first-to-last, each filling in gaps.
  // So /B fills in first → shared=2, then /C fills (shared already set),
  // then /A fills (shared already set).
  // "shared" should be 2 (from /B, first to fill in).
  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::Prepend,
                   {Path("/A", ""), Path("/B", ""), Path("/C", "")}});
    inh.push_back({ListEditQual::Order,
                   {Path("/C", ""), Path("/A", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // All unique properties should be present
    TEST_CHECK(it->second.props().count("onlyA") > 0);
    TEST_CHECK(it->second.props().count("onlyB") > 0);
    TEST_CHECK(it->second.props().count("onlyC") > 0);

    // "shared" should be 2 (from /B, which is first in reordered list)
    TEST_CHECK(it->second.props().count("shared") > 0);
    if (it->second.props().count("shared")) {
      auto v = it->second.props().at("shared").get_attribute().get_value<int>();
      TEST_CHECK(v.has_value());
      if (v.has_value()) {
        TEST_CHECK(v.value() == 2);
        TEST_MSG("Expected 2 (from /B, first in reordered list), got %d", v.value());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 33. comp_relocates_simple_rename_test
// Relocate /Root/Child -> /Root/NewChild (same-parent rename).
// ---------------------------------------------------------------------------
void comp_relocates_simple_rename_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");
  {
    PrimSpec child(Specifier::Def, "Scope", "Child");
    Attribute attr;
    attr.set_value(42);
    attr.set_type_name("int");
    child.props()["val"] = Property(attr, false);
    root.children().push_back(child);
  }
  layer.add_primspec("Root", root);
  layer.metas().layerRelocates.push_back(
      {Path("/Root/Child", ""), Path("/Root/NewChild", "")});

  Layer result;
  std::string warn, err;
  bool ok = CompositeRelocates(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeRelocates failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // Should have 1 child named "NewChild", not "Child"
    TEST_CHECK(it->second.children().size() == 1);
    if (!it->second.children().empty()) {
      TEST_CHECK(it->second.children()[0].name() == "NewChild");
      TEST_CHECK(it->second.children()[0].props().count("val") > 0);
    }
  }

  // layerRelocates should be cleared
  TEST_CHECK(result.metas().layerRelocates.empty());
}

// ---------------------------------------------------------------------------
// 34. comp_relocates_root_rename_test
// Relocate /OldName -> /NewName (root-level prim rename).
// ---------------------------------------------------------------------------
void comp_relocates_root_rename_test(void) {
  Layer layer;

  PrimSpec prim(Specifier::Def, "Scope", "OldName");
  {
    Attribute attr;
    attr.set_value(99);
    attr.set_type_name("int");
    prim.props()["val"] = Property(attr, false);
  }
  layer.add_primspec("OldName", prim);
  layer.metas().layerRelocates.push_back(
      {Path("/OldName", ""), Path("/NewName", "")});

  Layer result;
  std::string warn, err;
  bool ok = CompositeRelocates(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeRelocates failed: %s", err.c_str());
    return;
  }

  // "OldName" should be gone, "NewName" should exist
  TEST_CHECK(result.primspecs().count("OldName") == 0);
  TEST_CHECK(result.primspecs().count("NewName") > 0);
  if (result.primspecs().count("NewName")) {
    TEST_CHECK(result.primspecs().at("NewName").props().count("val") > 0);
  }
}

// ---------------------------------------------------------------------------
// 35. comp_relocates_cross_parent_test
// Relocate /Root/A -> /Root/Group/A (move child under new parent).
// ---------------------------------------------------------------------------
void comp_relocates_cross_parent_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");
  {
    PrimSpec childA(Specifier::Def, "Scope", "A");
    Attribute attr;
    attr.set_value(1);
    attr.set_type_name("int");
    childA.props()["val"] = Property(attr, false);
    root.children().push_back(childA);

    // Pre-create "Group" as an over
    PrimSpec group(Specifier::Def, "Scope", "Group");
    root.children().push_back(group);
  }
  layer.add_primspec("Root", root);
  layer.metas().layerRelocates.push_back(
      {Path("/Root/A", ""), Path("/Root/Group/A", "")});

  Layer result;
  std::string warn, err;
  bool ok = CompositeRelocates(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeRelocates failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    // /Root should no longer have direct child "A"
    bool found_direct_A = false;
    for (const auto &child : it->second.children()) {
      if (child.name() == "A") found_direct_A = true;
    }
    TEST_CHECK(!found_direct_A);

    // /Root/Group should have child "A"
    bool found_group = false;
    for (const auto &child : it->second.children()) {
      if (child.name() == "Group") {
        found_group = true;
        bool found_A = false;
        for (const auto &gchild : child.children()) {
          if (gchild.name() == "A") {
            found_A = true;
            TEST_CHECK(gchild.props().count("val") > 0);
          }
        }
        TEST_CHECK(found_A);
      }
    }
    TEST_CHECK(found_group);
  }
}

// ---------------------------------------------------------------------------
// 36. comp_relocates_path_remap_test
// Verify that relationship targets and inherits paths are remapped.
// ---------------------------------------------------------------------------
void comp_relocates_path_remap_test(void) {
  Layer layer;

  PrimSpec root(Specifier::Def, "Scope", "Root");
  {
    PrimSpec child(Specifier::Def, "Scope", "Child");
    // Relationship pointing to /Root/Target
    Relationship rel;
    rel.set(Path("/Root/Target", ""));
    child.props()["rel"] = Property(rel, false);
    root.children().push_back(child);

    PrimSpec target(Specifier::Def, "Scope", "Target");
    root.children().push_back(target);
  }
  layer.add_primspec("Root", root);
  // Relocate /Root/Target -> /Root/NewTarget
  layer.metas().layerRelocates.push_back(
      {Path("/Root/Target", ""), Path("/Root/NewTarget", "")});

  Layer result;
  std::string warn, err;
  bool ok = CompositeRelocates(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeRelocates failed: %s", err.c_str());
    return;
  }

  // Check that /Root/Child's relationship now points to /Root/NewTarget
  auto it = result.primspecs().find("Root");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    for (const auto &child : it->second.children()) {
      if (child.name() == "Child") {
        TEST_CHECK(child.props().count("rel") > 0);
        if (child.props().count("rel")) {
          const auto &p = child.props().at("rel");
          TEST_CHECK(p.is_relationship());
          if (p.is_relationship()) {
            const auto &r = p.get_relationship();
            TEST_CHECK(r.is_path());
            if (r.is_path()) {
              TEST_CHECK(r.targetPath.prim_part() == "/Root/NewTarget");
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Regression: `append references` must NOT override local(direct) opinions.
// LIVRPS: Local > References, regardless of prepend/append qualifier.
// Local `x = 1` + append reference to a prim authoring `x = 2` and `y = 20`
// => composed prim keeps x = 1 and gains y = 20 (pxr behavior).
// ---------------------------------------------------------------------------
void comp_append_reference_local_wins_test(void) {
  Layer layer;

  // -- /base: the referenced prim --
  PrimSpec base(Specifier::Def, "Xform", "base");
  {
    Attribute ax;
    ax.set_value(2);
    ax.set_type_name("int");
    base.props()["x"] = Property(ax, false);

    Attribute ay;
    ay.set_value(20);
    ay.set_type_name("int");
    base.props()["y"] = Property(ay, false);
  }
  layer.add_primspec("base", base);

  // -- /root: local x = 1, append references = </base> (in-layer reference) --
  PrimSpec root(Specifier::Def, "Xform", "root");
  {
    Attribute ax;
    ax.set_value(1);
    ax.set_type_name("int");
    root.props()["x"] = Property(ax, false);

    Reference ref;
    ref.asset_path = lightusd::legacy::value::AssetPath("");  // in-layer reference
    ref.prim_path = Path("/base", "");
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> refs;
    refs.push_back({ListEditQual::Append, {ref}});
    root.metas().references = refs;
  }
  layer.add_primspec("root", root);

  AssetResolutionResolver resolver;
  Layer result;
  std::string warn, err;
  bool ok = CompositeReferences(resolver, layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeReferences failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("root");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  // Local opinion x = 1 must win over referenced x = 2.
  TEST_CHECK(it->second.props().count("x") > 0);
  if (it->second.props().count("x")) {
    auto v = it->second.props().at("x").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 1);
      TEST_MSG("append reference must not override local opinion: expected "
               "x = 1, got %d", v.value());
    }
  }

  // Referenced-only opinion y = 20 must be brought in.
  TEST_CHECK(it->second.props().count("y") > 0);
  if (it->second.props().count("y")) {
    auto v = it->second.props().at("y").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 20);
    }
  }
}

// ---------------------------------------------------------------------------
// Regression: variant opinions must NOT override local(direct) prim opinions.
// LIVRPS: Local > VariantSets.
// Local `x = 1` + selected variant authoring `x = 2` and `fromVariant = 10`
// => composed prim keeps x = 1 and gains fromVariant = 10 (pxr behavior).
// ---------------------------------------------------------------------------
void comp_variant_local_wins_test(void) {
  PrimSpec ps(Specifier::Def, "Xform", "root");

  // Local opinion x = 1
  {
    Attribute ax;
    ax.set_value(1);
    ax.set_type_name("int");
    ps.props()["x"] = Property(ax, false);
  }

  // variantSets = ["shading"], variants = { shading = "red" }
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"shading"}});
    ps.metas().variantSets = vsets;

    VariantSelectionMap vsmap;
    vsmap["shading"] = "red";
    ps.metas().variants = vsmap;
  }

  // variantSet "shading" = { "red" { x = 2, fromVariant = 10 } }
  {
    VariantSetSpec vss;
    vss.name = "shading";

    PrimSpec red(Specifier::Def, "", "red");
    {
      Attribute ax;
      ax.set_value(2);
      ax.set_type_name("int");
      red.props()["x"] = Property(ax, false);

      Attribute av;
      av.set_value(10);
      av.set_type_name("int");
      red.props()["fromVariant"] = Property(av, false);
    }
    vss.variantSet["red"] = red;

    ps.variantSets()["shading"] = vss;
  }

  PrimSpec dst;
  std::map<std::string, std::string> selection;  // empty: use authored
  std::string warn, err;
  bool ok = VariantSelectPrimSpec(dst, ps, selection, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("VariantSelectPrimSpec failed: %s", err.c_str());
    return;
  }

  // Local opinion x = 1 must win over variant x = 2.
  TEST_CHECK(dst.props().count("x") > 0);
  if (dst.props().count("x")) {
    auto v = dst.props().at("x").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 1);
      TEST_MSG("variant must not override local opinion: expected x = 1, "
               "got %d", v.value());
    }
  }

  // Variant-only opinion must be brought in.
  TEST_CHECK(dst.props().count("fromVariant") > 0);
  if (dst.props().count("fromVariant")) {
    auto v = dst.props().at("fromVariant").get_attribute().get_value<int>();
    TEST_CHECK(v.has_value());
    if (v.has_value()) {
      TEST_CHECK(v.value() == 10);
    }
  }
}

// ---------------------------------------------------------------------------
// Regression: local-only child prims must survive reference composition.
// /root references /base; /root also has a local child "localChild" which
// does not exist in /base => the composed /root must keep "localChild".
// ---------------------------------------------------------------------------
void comp_reference_keeps_local_children_test(void) {
  Layer layer;

  PrimSpec base(Specifier::Def, "Xform", "base");
  {
    PrimSpec ref_child(Specifier::Def, "Scope", "refChild");
    base.children().push_back(ref_child);
  }
  layer.add_primspec("base", base);

  PrimSpec root(Specifier::Def, "Xform", "root");
  {
    PrimSpec local_child(Specifier::Def, "Sphere", "localChild");
    {
      Attribute ar;
      ar.set_value(3.0);
      ar.set_type_name("double");
      local_child.props()["radius"] = Property(ar, false);
    }
    root.children().push_back(local_child);

    Reference ref;
    ref.asset_path = lightusd::legacy::value::AssetPath("");  // in-layer reference
    ref.prim_path = Path("/base", "");
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> refs;
    refs.push_back({ListEditQual::Prepend, {ref}});
    root.metas().references = refs;
  }
  layer.add_primspec("root", root);

  AssetResolutionResolver resolver;
  Layer result;
  std::string warn, err;
  bool ok = CompositeReferences(resolver, layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeReferences failed: %s", err.c_str());
    return;
  }

  auto it = result.primspecs().find("root");
  TEST_CHECK(it != result.primspecs().end());
  if (it == result.primspecs().end()) return;

  bool has_local_child = false;
  bool has_ref_child = false;
  for (const auto &child : it->second.children()) {
    if (child.name() == "localChild") {
      has_local_child = true;
    }
    if (child.name() == "refChild") {
      has_ref_child = true;
    }
  }
  TEST_CHECK(has_local_child);
  TEST_MSG("local-only child prim must survive reference composition");
  TEST_CHECK(has_ref_child);
  TEST_MSG("referenced child prim must be composed in");
}

// ---------------------------------------------------------------------------
// Regression: TimeSamples::apply_time_transform must transform sample times
// stored in unified binary storage (LayerOffset composition depends on it).
// ---------------------------------------------------------------------------
void comp_timesamples_time_transform_test(void) {
  // Unified binary storage (add_binary_sample)
  {
    lightusd::legacy::value::TimeSamples ts;
    std::string err;
    TEST_CHECK(ts.add_binary_sample<double>(0.0, 1.5, &err));
    TEST_CHECK(ts.add_binary_sample<double>(10.0, 2.5, &err));

    // t_new = t * 2 + 5
    ts.apply_time_transform(2.0, 5.0);

    const auto &samples = ts.get_samples();
    TEST_CHECK(samples.size() == 2);
    if (samples.size() == 2) {
      TEST_CHECK(std::fabs(samples[0].t - 5.0) < 1e-12);
      TEST_CHECK(std::fabs(samples[1].t - 25.0) < 1e-12);
    }
  }

  // Generic Value storage (add_sample)
  {
    lightusd::legacy::value::TimeSamples ts;
    std::string err;
    TEST_CHECK(ts.add_sample(0.0, lightusd::legacy::value::Value(std::string("a")), &err));
    TEST_CHECK(ts.add_sample(10.0, lightusd::legacy::value::Value(std::string("b")), &err));

    ts.apply_time_transform(2.0, 5.0);

    const auto &samples = ts.get_samples();
    TEST_CHECK(samples.size() == 2);
    if (samples.size() == 2) {
      TEST_CHECK(std::fabs(samples[0].t - 5.0) < 1e-12);
      TEST_CHECK(std::fabs(samples[1].t - 25.0) < 1e-12);
    }
  }
}
