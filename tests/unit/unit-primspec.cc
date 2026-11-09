#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-primspec.h"
#include "core/prim-spec.hh"
#include "core/prim.hh"
#include "value-types.hh"
#include "layer.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void primspec_create_test(void) {
  // Default constructor
  {
    PrimSpec ps;
    TEST_CHECK(ps.specifier() == Specifier::Def);
    TEST_CHECK(ps.name().empty());
    TEST_CHECK(ps.typeName().empty());
  }

  // Create with Def specifier
  {
    PrimSpec ps(Specifier::Def, "MyPrim");
    TEST_CHECK(ps.specifier() == Specifier::Def);
    TEST_CHECK(ps.name() == "MyPrim");
    TEST_MSG("Expected name 'MyPrim', got '%s'", ps.name().c_str());
  }

  // Create with Over specifier
  {
    PrimSpec ps(Specifier::Over, "OverPrim");
    TEST_CHECK(ps.specifier() == Specifier::Over);
    TEST_CHECK(ps.name() == "OverPrim");
    TEST_MSG("Expected name 'OverPrim', got '%s'", ps.name().c_str());
  }

  // Create with Class specifier
  {
    PrimSpec ps(Specifier::Class, "ClassPrim");
    TEST_CHECK(ps.specifier() == Specifier::Class);
    TEST_CHECK(ps.name() == "ClassPrim");
  }

  // Create with specifier, typeName, and name
  {
    PrimSpec ps(Specifier::Def, "Xform", "MyXform");
    TEST_CHECK(ps.specifier() == Specifier::Def);
    TEST_CHECK(ps.typeName() == "Xform");
    TEST_CHECK(ps.name() == "MyXform");
    TEST_MSG("Expected typeName 'Xform', got '%s'", ps.typeName().c_str());
  }

  // Create with Mesh typeName
  {
    PrimSpec ps(Specifier::Def, "Mesh", "MyMesh");
    TEST_CHECK(ps.typeName() == "Mesh");
    TEST_CHECK(ps.name() == "MyMesh");
  }
}

void primspec_specifiers_test(void) {
  // Verify Def, Over, Class are distinct values
  TEST_CHECK(Specifier::Def != Specifier::Over);
  TEST_CHECK(Specifier::Def != Specifier::Class);
  TEST_CHECK(Specifier::Over != Specifier::Class);
  TEST_CHECK(Specifier::Def != Specifier::Invalid);
  TEST_CHECK(Specifier::Over != Specifier::Invalid);
  TEST_CHECK(Specifier::Class != Specifier::Invalid);

  // Verify specifier can be changed
  {
    PrimSpec ps(Specifier::Def, "TestPrim");
    TEST_CHECK(ps.specifier() == Specifier::Def);

    ps.specifier() = Specifier::Over;
    TEST_CHECK(ps.specifier() == Specifier::Over);
    TEST_MSG("Expected Over after assignment");

    ps.specifier() = Specifier::Class;
    TEST_CHECK(ps.specifier() == Specifier::Class);
    TEST_MSG("Expected Class after assignment");
  }
}

void primspec_properties_test(void) {
  PrimSpec ps(Specifier::Def, "Xform", "TestPrim");

  // Initially no properties
  TEST_CHECK(ps.props().empty());

  // Add an Attribute property with a float value
  {
    Attribute attr(3.14f);
    Property prop(attr);
    ps.props()["myFloat"] = prop;
  }

  TEST_CHECK(ps.props().size() == 1);
  TEST_CHECK(ps.props().count("myFloat") == 1);
  TEST_MSG("Expected 1 property 'myFloat'");

  // Verify the attribute value can be retrieved
  {
    const auto &prop = ps.props().at("myFloat");
    TEST_CHECK(prop.is_attribute());
    if (prop.is_attribute()) {
      const Attribute &attr = prop.get_attribute();
      TEST_CHECK(attr.has_value());
      float val = 0.0f;
      bool ok = attr.get_value(&val);
      TEST_CHECK(ok);
      TEST_CHECK(val > 3.13f && val < 3.15f);
      TEST_MSG("Expected float ~3.14, got %f", val);
    }
  }

  // Add another property
  {
    Attribute attr2(42);
    Property prop2(attr2);
    ps.props()["myInt"] = prop2;
  }

  TEST_CHECK(ps.props().size() == 2);
  TEST_CHECK(ps.props().count("myInt") == 1);

  // Add a string property
  {
    Attribute attr3(std::string("hello"));
    Property prop3(attr3);
    ps.props()["myString"] = prop3;
  }

  TEST_CHECK(ps.props().size() == 3);
}

void primspec_children_test(void) {
  PrimSpec parent(Specifier::Def, "Xform", "Parent");

  // Initially no children
  TEST_CHECK(parent.children().empty());

  // Add first child
  {
    PrimSpec child1(Specifier::Def, "Mesh", "Child1");
    parent.children().push_back(child1);
  }

  TEST_CHECK(parent.children().size() == 1);
  TEST_CHECK(parent.children()[0].name() == "Child1");
  TEST_MSG("Expected child name 'Child1', got '%s'",
           parent.children()[0].name().c_str());

  // Add second child
  {
    PrimSpec child2(Specifier::Def, "Xform", "Child2");
    parent.children().push_back(child2);
  }

  TEST_CHECK(parent.children().size() == 2);
  TEST_CHECK(parent.children()[1].name() == "Child2");

  // Add grandchild to first child
  {
    PrimSpec grandchild(Specifier::Def, "Mesh", "GrandChild");
    parent.children()[0].children().push_back(grandchild);
  }

  TEST_CHECK(parent.children()[0].children().size() == 1);
  TEST_CHECK(parent.children()[0].children()[0].name() == "GrandChild");

  // Verify typeName of children
  TEST_CHECK(parent.children()[0].typeName() == "Mesh");
  TEST_CHECK(parent.children()[1].typeName() == "Xform");
}

void primspec_variant_selection_test(void) {
  PrimSpec ps(Specifier::Def, "Xform", "VarPrim");

  // Set up variants in metas: key = variantSet target name, value = default selection
  VariantSelectionMap vsmap;
  vsmap["shadingVariant"] = "red";
  vsmap["modelVariant"] = "highRes";
  ps.metas().variants = vsmap;

  // Also set up variantSets
  {
    VariantSetSpec vss;
    vss.name = "shadingVariant";
    PrimSpec red_ps(Specifier::Def, "Xform", "red");
    PrimSpec blue_ps(Specifier::Def, "Xform", "blue");
    vss.variantSet["red"] = red_ps;
    vss.variantSet["blue"] = blue_ps;
    ps.variantSets()["shadingVariant"] = vss;
  }

  // Test get_variant_selection_map
  {
    VariantSelectionMap result = ps.get_variant_selection_map();
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result.count("shadingVariant") == 1);
    TEST_CHECK(result["shadingVariant"] == "red");
    TEST_CHECK(result["modelVariant"] == "highRes");
  }

  // Test current_variant_selection (before explicit select)
  {
    std::string selected;
    bool ok = ps.current_variant_selection("shadingVariant", &selected);
    TEST_CHECK(ok);
    TEST_CHECK(selected == "red");
    TEST_MSG("Expected default selection 'red', got '%s'", selected.c_str());
  }

  // Test select_variant
  {
    bool ok = ps.select_variant("shadingVariant", "blue");
    TEST_CHECK(ok);
    TEST_MSG("select_variant should succeed for existing variant set");

    std::string selected;
    bool ok2 = ps.current_variant_selection("shadingVariant", &selected);
    TEST_CHECK(ok2);
    TEST_CHECK(selected == "blue");
    TEST_MSG("Expected selection 'blue' after select, got '%s'",
             selected.c_str());
  }

  // Test select_variant with nonexistent target
  {
    bool ok = ps.select_variant("nonExistent", "value");
    TEST_CHECK(!ok);
    TEST_MSG("select_variant should fail for nonexistent target");
  }

  // Test current_variant_selection with null pointer
  {
    bool ok = ps.current_variant_selection("shadingVariant", nullptr);
    TEST_CHECK(!ok);
  }

  // Test current_variant_selection with nonexistent target
  {
    std::string selected;
    bool ok = ps.current_variant_selection("nonExistent", &selected);
    TEST_CHECK(!ok);
  }
}

void primspec_metas_test(void) {
  PrimSpec ps(Specifier::Def, "Xform", "MetaPrim");

  // Test inherits
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    std::vector<Path> paths;
    paths.push_back(Path("/_class_MyBase", ""));
    inh.push_back(std::make_pair(ListEditQual::Prepend, paths));
    ps.metas().inherits = inh;

    TEST_CHECK(ps.metas().inherits.has_value());
    TEST_CHECK(ps.metas().inherits.value().size() == 1);
    TEST_CHECK(ps.metas().inherits.value()[0].second.size() == 1);
    TEST_MSG("inherits metadata should be set");
  }

  // Test specializes
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> spec;
    std::vector<Path> paths;
    paths.push_back(Path("/SpecBase", ""));
    spec.push_back(std::make_pair(ListEditQual::Prepend, paths));
    ps.metas().specializes = spec;

    TEST_CHECK(ps.metas().specializes.has_value());
    TEST_CHECK(ps.metas().specializes.value().size() == 1);
    TEST_MSG("specializes metadata should be set");
  }

  // Test variants
  {
    VariantSelectionMap vsmap;
    vsmap["look"] = "default";
    ps.metas().variants = vsmap;

    TEST_CHECK(ps.metas().variants.has_value());
    TEST_CHECK(ps.metas().variants.value().size() == 1);
    TEST_CHECK(ps.metas().variants.value().at("look") == "default");
    TEST_MSG("variants metadata should be set");
  }

  // Test variantSets
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    std::vector<std::string> names;
    names.push_back("look");
    vsets.push_back(std::make_pair(ListEditQual::ResetToExplicit, names));
    ps.metas().variantSets = vsets;

    TEST_CHECK(ps.metas().variantSets.has_value());
    TEST_CHECK(ps.metas().variantSets.value().size() == 1);
    TEST_MSG("variantSets metadata should be set");
  }

  // Test kind
  {
    ps.metas().set_kind(Kind::Component);
    TEST_CHECK(ps.metas().has_kind());
    TEST_CHECK(ps.metas().get_kind_enum() == Kind::Component);
    TEST_MSG("kind metadata should be set to Component");
  }

  // Test authored checks
  {
    TEST_CHECK(ps.metas().authored());
    TEST_MSG("PrimSpec should report having authored metadata");
  }
}

// Test PrimMeta lazy initialization (unique_ptr storage)
void primspec_metas_lazy_init_test(void) {
  // Default-constructed PrimSpec should have empty (unallocated) metas
  {
    PrimSpec ps;
    // const access should return a static empty instance, not allocate
    const PrimMeta &m = ps.metas();
    TEST_CHECK(!m.authored());
    TEST_MSG("Default PrimSpec metas should be empty");
  }

  // Non-const access should allocate on first call
  {
    PrimSpec ps;
    PrimMeta &m = ps.metas();
    m.set_displayName("Test");
    TEST_CHECK(ps.metas().has_displayName());
    TEST_CHECK(ps.metas().get_displayName() == "Test");
    TEST_MSG("Metas should persist after lazy init");
  }

  // Copy should deep-copy metas
  {
    PrimSpec ps1(Specifier::Def, "Xform", "CopyTest");
    ps1.metas().set_displayName("Original");
    ps1.metas().set_kind(Kind::Component);

    PrimSpec ps2 = ps1;
    TEST_CHECK(ps2.metas().has_displayName());
    TEST_CHECK(ps2.metas().get_displayName() == "Original");
    TEST_CHECK(ps2.metas().get_kind_enum() == Kind::Component);

    // Mutating ps2 should not affect ps1
    ps2.metas().set_displayName("Modified");
    TEST_CHECK(ps1.metas().get_displayName() == "Original");
    TEST_CHECK(ps2.metas().get_displayName() == "Modified");
    TEST_MSG("Deep copy should be independent");
  }

  // Move should transfer ownership
  {
    PrimSpec ps1(Specifier::Def, "Xform", "MoveTest");
    ps1.metas().set_displayName("Moveable");

    PrimSpec ps2 = std::move(ps1);
    TEST_CHECK(ps2.metas().has_displayName());
    TEST_CHECK(ps2.metas().get_displayName() == "Moveable");
    TEST_MSG("Move should transfer metas");
  }

  // Multiple PrimSpecs should have independent metas
  {
    PrimSpec ps1(Specifier::Def, "Xform", "A");
    PrimSpec ps2(Specifier::Def, "Xform", "B");
    ps1.metas().set_displayName("A");
    ps2.metas().set_displayName("B");
    TEST_CHECK(ps1.metas().get_displayName() == "A");
    TEST_CHECK(ps2.metas().get_displayName() == "B");
    TEST_MSG("Multiple PrimSpecs should have independent metas");
  }
}

// primChildren and propertyNames accessors.
void primspec_primchildren_propertynames_test(void) {
  using namespace lightusd;
  using namespace lightusd::legacy;  // NOLINT

  PrimSpec ps(Specifier::Def, "Scope", "TestScope");

  // Initially empty.
  TEST_CHECK(ps.primChildren().empty());
  TEST_CHECK(ps.propertyNames().empty());
}

// set_current_working_path / get_current_working_path.
void primspec_working_path_test(void) {
  using namespace lightusd;
  using namespace lightusd::legacy;  // NOLINT

  PrimSpec ps(Specifier::Def, "Xform", "PathTest");

  TEST_CHECK(ps.get_current_working_path().empty());

  ps.set_current_working_path("/some/path");
  TEST_CHECK(ps.get_current_working_path() == "/some/path");

  ps.set_asset_resolution_state("/cwp", {"/search1", "/search2"});
  TEST_CHECK(ps.get_current_working_path() == "/cwp");
  TEST_CHECK(ps.get_asset_search_paths().size() == 2);
  TEST_CHECK(ps.get_asset_search_paths()[0] == "/search1");
  TEST_CHECK(ps.get_asset_search_paths()[1] == "/search2");
}
