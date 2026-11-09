#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-composition.h"
#include "composition.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "layer.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// ---------------------------------------------------------------------------
// CompositeInherits: /Base has "myAttr", /Derived inherits /Base.
// After composition, /Derived should contain "myAttr" from /Base.
// ---------------------------------------------------------------------------
void composition_inherits_test(void) {
  // Build a Layer with /Base (class) and /Derived (def, inherits /Base)
  Layer layer;

  // -- /Base --
  PrimSpec base(Specifier::Class, "Scope", "Base");
  {
    Attribute attr;
    attr.set_value(42);
    attr.set_type_name("int");
    Property prop(attr, /* custom */ false);
    base.props()["myAttr"] = prop;
  }
  layer.add_primspec("Base", base);

  // -- /Derived --
  PrimSpec derived(Specifier::Def, "Scope", "Derived");
  {
    // Set inherits = [</Base>]
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::ResetToExplicit,
                   {Path("/Base", "")}});
    derived.metas().inherits = inh;
  }
  layer.add_primspec("Derived", derived);

  // Composite
  Layer result;
  std::string warn, err;
  bool ok = CompositeInherits(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeInherits failed: %s", err.c_str());
    return;
  }

  // Verify /Derived has "myAttr"
  auto it = result.primspecs().find("Derived");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("myAttr") > 0);

    // Verify value is 42
    if (it->second.props().count("myAttr")) {
      const Property &p = it->second.props().at("myAttr");
      TEST_CHECK(p.is_attribute());
      if (p.is_attribute()) {
        const Attribute &a = p.get_attribute();
        auto v = a.get_value<int>();
        TEST_CHECK(v.has_value());
        if (v.has_value()) {
          TEST_CHECK(v.value() == 42);
        }
      }
    }
  }

  // Verify inherits metadata was consumed (reset) on /Derived
  if (it != result.primspecs().end()) {
    TEST_CHECK(!it->second.metas().inherits.has_value());
  }

  // Verify /Derived's own properties still survive if authored
  // (Add a local property on Derived and ensure it overrides)
  {
    Layer layer2;

    PrimSpec base2(Specifier::Class, "Scope", "Base");
    {
      Attribute attr;
      attr.set_value(100);
      attr.set_type_name("int");
      base2.props()["shared"] = Property(attr);
    }
    layer2.add_primspec("Base", base2);

    PrimSpec derived2(Specifier::Def, "Scope", "Derived");
    {
      std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
      inh.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
      derived2.metas().inherits = inh;

      // Local override
      Attribute attr;
      attr.set_value(999);
      attr.set_type_name("int");
      derived2.props()["shared"] = Property(attr);
    }
    layer2.add_primspec("Derived", derived2);

    Layer result2;
    std::string w2, e2;
    TEST_CHECK(CompositeInherits(layer2, &result2, &w2, &e2));

    auto it2 = result2.primspecs().find("Derived");
    TEST_CHECK(it2 != result2.primspecs().end());
    if (it2 != result2.primspecs().end()) {
      TEST_CHECK(it2->second.props().count("shared") > 0);
      if (it2->second.props().count("shared")) {
        const Attribute &a2 = it2->second.props().at("shared").get_attribute();
        auto v2 = a2.get_value<int>();
        TEST_CHECK(v2.has_value());
        if (v2.has_value()) {
          // Local override wins
          TEST_CHECK(v2.value() == 999);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// CompositeSpecializes: Same pattern as inherits but using specializes arc.
// ---------------------------------------------------------------------------
void composition_specializes_test(void) {
  Layer layer;

  // -- /Base --
  PrimSpec base(Specifier::Def, "Scope", "Base");
  {
    Attribute attr;
    attr.set_value(3.14f);
    attr.set_type_name("float");
    base.props()["radius"] = Property(attr);
  }
  layer.add_primspec("Base", base);

  // -- /Specialized --
  PrimSpec specialized(Specifier::Def, "Scope", "Specialized");
  {
    std::vector<std::pair<ListEditQual, std::vector<Path>>> spec;
    spec.push_back({ListEditQual::ResetToExplicit,
                    {Path("/Base", "")}});
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

  // Verify /Specialized has "radius" from /Base
  auto it = result.primspecs().find("Specialized");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("radius") > 0);

    if (it->second.props().count("radius")) {
      const Property &p = it->second.props().at("radius");
      TEST_CHECK(p.is_attribute());
      if (p.is_attribute()) {
        auto v = p.get_attribute().get_value<float>();
        TEST_CHECK(v.has_value());
        if (v.has_value()) {
          TEST_CHECK(v.value() == 3.14f);
        }
      }
    }

    // specializes metadata should be cleared
    TEST_CHECK(!it->second.metas().specializes.has_value());
  }

  // Test local-override semantics: local property wins
  {
    Layer layer2;

    PrimSpec base2(Specifier::Def, "Scope", "Base");
    {
      Attribute attr;
      attr.set_value(1.0f);
      attr.set_type_name("float");
      base2.props()["val"] = Property(attr);
    }
    layer2.add_primspec("Base", base2);

    PrimSpec spec2(Specifier::Def, "Scope", "Spec");
    {
      std::vector<std::pair<ListEditQual, std::vector<Path>>> sv;
      sv.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
      spec2.metas().specializes = sv;

      Attribute attr;
      attr.set_value(2.0f);
      attr.set_type_name("float");
      spec2.props()["val"] = Property(attr);
    }
    layer2.add_primspec("Spec", spec2);

    Layer result2;
    std::string w2, e2;
    TEST_CHECK(CompositeSpecializes(layer2, &result2, &w2, &e2));

    auto it2 = result2.primspecs().find("Spec");
    TEST_CHECK(it2 != result2.primspecs().end());
    if (it2 != result2.primspecs().end()) {
      auto v2 = it2->second.props().at("val").get_attribute().get_value<float>();
      TEST_CHECK(v2.has_value());
      if (v2.has_value()) {
        TEST_CHECK(v2.value() == 2.0f);  // local wins
      }
    }
  }
}

// ---------------------------------------------------------------------------
// CompositeVariant: Create a PrimSpec with variantSets, select a variant,
// verify the selected variant's properties appear in the result.
// ---------------------------------------------------------------------------
void composition_variant_test(void) {
  Layer layer;

  // -- /Cube with variantSet "colorVariant" containing "red" and "green" --
  PrimSpec cube(Specifier::Def, "Scope", "Cube");

  // Set up variantSets metadata
  {
    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"colorVariant"}});
    cube.metas().variantSets = vsets;
  }

  // Set up variants metadata (selection)
  {
    VariantSelectionMap vsmap;
    vsmap["colorVariant"] = "green";
    cube.metas().variants = vsmap;
  }

  // Set up the VariantSetSpec with two variants
  {
    VariantSetSpec vss;
    vss.name = "colorVariant";

    // "red" variant
    PrimSpec red(Specifier::Def, "", "red");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("red"));
      attr.set_type_name("token");
      red.props()["color"] = Property(attr);
    }
    vss.variantSet["red"] = red;

    // "green" variant
    PrimSpec green(Specifier::Def, "", "green");
    {
      Attribute attr;
      attr.set_value(lightusd::legacy::value::token("green"));
      attr.set_type_name("token");
      green.props()["color"] = Property(attr);
    }
    vss.variantSet["green"] = green;

    cube.variantSets()["colorVariant"] = vss;
  }

  layer.add_primspec("Cube", cube);

  Layer result;
  std::string warn, err;
  bool ok = CompositeVariant(layer, &result, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("CompositeVariant failed: %s", err.c_str());
    return;
  }

  // Verify /Cube now has "color" = "green"
  auto it = result.primspecs().find("Cube");
  TEST_CHECK(it != result.primspecs().end());
  if (it != result.primspecs().end()) {
    TEST_CHECK(it->second.props().count("color") > 0);

    if (it->second.props().count("color")) {
      const Property &p = it->second.props().at("color");
      TEST_CHECK(p.is_attribute());
      if (p.is_attribute()) {
        auto v = p.get_attribute().get_value<lightusd::legacy::value::token>();
        TEST_CHECK(v.has_value());
        if (v.has_value()) {
          TEST_CHECK(v.value().str() == "green");
        }
      }
    }

    // variant/variantSets metadata should be cleared
    TEST_CHECK(!it->second.metas().variants.has_value());
    TEST_CHECK(!it->second.metas().variantSets.has_value());
    TEST_CHECK(it->second.variantSets().empty());
  }
}

// ---------------------------------------------------------------------------
// HasInherits: Verify detection of inherits metadata in a Layer.
// ---------------------------------------------------------------------------
void composition_has_inherits_test(void) {
  // Empty layer should not have inherits
  {
    Layer empty_layer;
    TEST_CHECK(!HasInherits(empty_layer));
  }

  // Layer with a PrimSpec that has inherits
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "A");
    std::vector<std::pair<ListEditQual, std::vector<Path>>> inh;
    inh.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
    ps.metas().inherits = inh;
    layer.add_primspec("A", ps);

    TEST_CHECK(HasInherits(layer));
  }

  // Layer with a PrimSpec without inherits
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "B");
    layer.add_primspec("B", ps);

    TEST_CHECK(!HasInherits(layer));
  }
}

// ---------------------------------------------------------------------------
// HasSpecializes: Verify detection of specializes metadata in a Layer.
// ---------------------------------------------------------------------------
void composition_has_specializes_test(void) {
  // Empty layer should not have specializes
  {
    Layer empty_layer;
    TEST_CHECK(!HasSpecializes(empty_layer));
  }

  // Layer with a PrimSpec that has specializes
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "X");
    std::vector<std::pair<ListEditQual, std::vector<Path>>> spec;
    spec.push_back({ListEditQual::ResetToExplicit, {Path("/Base", "")}});
    ps.metas().specializes = spec;
    layer.add_primspec("X", ps);

    TEST_CHECK(HasSpecializes(layer));
  }

  // Layer with a PrimSpec without specializes
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "Y");
    layer.add_primspec("Y", ps);

    TEST_CHECK(!HasSpecializes(layer));
  }
}

// ---------------------------------------------------------------------------
// HasVariants: Verify detection of variant metadata in a Layer.
// ---------------------------------------------------------------------------
void composition_has_variants_test(void) {
  // Empty layer
  {
    Layer empty_layer;
    TEST_CHECK(!HasVariants(empty_layer));
  }

  // Layer with a PrimSpec that has variants
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "V");

    std::vector<std::pair<ListEditQual, std::vector<std::string>>> vsets;
    vsets.push_back({ListEditQual::ResetToExplicit, {"myVariant"}});
    ps.metas().variantSets = vsets;

    VariantSelectionMap vsmap;
    vsmap["myVariant"] = "optionA";
    ps.metas().variants = vsmap;

    layer.add_primspec("V", ps);

    TEST_CHECK(HasVariants(layer));
  }

  // Layer without variants
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Scope", "W");
    layer.add_primspec("W", ps);

    TEST_CHECK(!HasVariants(layer));
  }
}
