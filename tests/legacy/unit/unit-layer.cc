#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-layer.h"
#include "layer.hh"
#include "core/prim-spec.hh"
#include "composition.hh"
#include "lightusd.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void layer_create_empty_test(void) {
  Layer layer;
  TEST_CHECK(layer.primspecs().empty());
  TEST_CHECK(layer.name().empty());
}

void layer_add_primspec_test(void) {
  Layer layer;

  // Add a PrimSpec
  PrimSpec ps(Specifier::Def, "Xform", "Root");
  bool ok = layer.add_primspec("Root", ps);
  TEST_CHECK(ok);
  TEST_CHECK(layer.has_primspec("Root"));
  TEST_CHECK(layer.primspecs().size() == 1);

  // Adding duplicate name should return false
  PrimSpec ps2(Specifier::Def, "Xform", "Root");
  bool dup = layer.add_primspec("Root", ps2);
  TEST_CHECK(!dup);
  TEST_CHECK(layer.primspecs().size() == 1);

  // Adding with different name should succeed
  PrimSpec ps3(Specifier::Def, "Mesh", "Child");
  bool ok2 = layer.add_primspec("Child", ps3);
  TEST_CHECK(ok2);
  TEST_CHECK(layer.primspecs().size() == 2);
  TEST_CHECK(layer.has_primspec("Child"));
}

void layer_emplace_primspec_test(void) {
  Layer layer;

  PrimSpec ps(Specifier::Def, "Xform", "Moved");
  bool ok = layer.emplace_primspec("Moved", std::move(ps));
  TEST_CHECK(ok);
  TEST_CHECK(layer.has_primspec("Moved"));
  TEST_CHECK(layer.primspecs().size() == 1);

  // Verify the PrimSpec data
  auto it = layer.primspecs().find("Moved");
  TEST_CHECK(it != layer.primspecs().end());
  if (it != layer.primspecs().end()) {
    TEST_CHECK(it->second.typeName() == "Xform");
  }
}

void layer_replace_primspec_test(void) {
  Layer layer;

  // Add a PrimSpec first
  PrimSpec ps_orig(Specifier::Def, "Xform", "Target");
  layer.add_primspec("Target", ps_orig);
  TEST_CHECK(layer.has_primspec("Target"));

  // Replace with a different PrimSpec
  PrimSpec ps_new(Specifier::Def, "Mesh", "Target");
  bool ok = layer.replace_primspec("Target", ps_new);
  TEST_CHECK(ok);
  TEST_CHECK(layer.primspecs().size() == 1);

  // Verify the replacement happened
  auto it = layer.primspecs().find("Target");
  TEST_CHECK(it != layer.primspecs().end());
  if (it != layer.primspecs().end()) {
    TEST_CHECK(it->second.typeName() == "Mesh");
  }

  // Replace for non-existent name should return false
  PrimSpec ps_missing(Specifier::Def, "Xform", "Missing");
  bool fail = layer.replace_primspec("NonExistent", ps_missing);
  TEST_CHECK(!fail);
}

void layer_find_primspec_at_test(void) {
  Layer layer;

  // Create root PrimSpec "Root" with child "Child"
  PrimSpec root_ps(Specifier::Def, "Xform", "Root");
  PrimSpec child_ps(Specifier::Def, "Mesh", "Child");
  root_ps.children().push_back(child_ps);

  layer.add_primspec("Root", root_ps);

  // Find the child at "/Root/Child"
  {
    const PrimSpec *found = nullptr;
    std::string err;
    bool ok = layer.find_primspec_at(Path("/Root/Child", ""), &found, &err);
    TEST_CHECK(ok);
    if (ok && found) {
      TEST_CHECK(found->name() == "Child");
      TEST_CHECK(found->typeName() == "Mesh");
    } else {
      TEST_MSG("find_primspec_at failed: %s", err.c_str());
    }
  }

  // Find the root at "/Root"
  {
    const PrimSpec *found = nullptr;
    std::string err;
    bool ok = layer.find_primspec_at(Path("/Root", ""), &found, &err);
    TEST_CHECK(ok);
    if (ok && found) {
      TEST_CHECK(found->name() == "Root");
    }
  }

  // Non-existent path should fail
  {
    const PrimSpec *found = nullptr;
    std::string err;
    bool ok = layer.find_primspec_at(Path("/Root/NonExistent", ""), &found, &err);
    TEST_CHECK(!ok);
  }
}

// Regression: copying a Layer must reset its lazy path->PrimSpec lookup cache.
// The cache stores `const PrimSpec*` into the SOURCE layer's _prim_specs tree;
// before the fix a copy returned the source's pointers (dangling once the
// source is destroyed) and even shared its cache mutex. After the fix the copy
// rebuilds against its own tree.
void layer_copy_resets_lookup_cache_test(void) {
  Layer a;
  a.add_primspec("Foo", PrimSpec(Specifier::Def, "Xform", "Foo"));

  // Populate A's lookup cache: _dirty becomes false and the cache holds a
  // pointer into A's own _prim_specs.
  const PrimSpec *a_hit = nullptr;
  std::string err;
  TEST_CHECK(a.find_primspec_at(Path("/Foo", ""), &a_hit, &err));
  TEST_CHECK(a_hit == &a.primspecs().at("Foo"));

  // Copy-construct: the copy must serve a pointer into ITSELF, not into A.
  {
    Layer b(a);
    const PrimSpec *b_hit = nullptr;
    TEST_CHECK(b.find_primspec_at(Path("/Foo", ""), &b_hit, &err));
    TEST_CHECK_(b_hit == &b.primspecs().at("Foo"),
                "copy ctor: find_primspec_at must return a pointer into the COPY");
    TEST_CHECK_(b_hit != a_hit,
                "copy ctor: must not return the source layer's PrimSpec pointer");
  }

  // Copy-assign (onto a non-empty target): same guarantee.
  {
    Layer c;
    c.add_primspec("Bar", PrimSpec(Specifier::Def, "Xform", "Bar"));
    c = a;  // overwrites c with a's data; the lookup cache must be reset
    const PrimSpec *c_hit = nullptr;
    TEST_CHECK(c.find_primspec_at(Path("/Foo", ""), &c_hit, &err));
    TEST_CHECK_(c_hit == &c.primspecs().at("Foo"),
                "copy assign: find_primspec_at must return a pointer into the COPY");
    TEST_CHECK_(c_hit != a_hit,
                "copy assign: must not return the source layer's PrimSpec pointer");
  }
}

void layer_check_unresolved_refs_test(void) {
  // Layer with references should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "Ref");

    Reference ref;
    ref.asset_path = lightusd::legacy::value::AssetPath("other.usda");
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> refs;
    refs.push_back({ListEditQual::ResetToExplicit, {ref}});
    ps.metas().references = refs;

    layer.add_primspec("Ref", ps);
    bool has_refs = layer.check_unresolved_references();
    TEST_CHECK(has_refs);
  }

  // Layer without references should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "NoRef");
    layer.add_primspec("NoRef", ps);
    bool has_refs = layer.check_unresolved_references();
    TEST_CHECK(!has_refs);
  }

  // Respect the requested traversal depth.
  {
    Layer layer;
    PrimSpec root(Specifier::Def, "Xform", "Root");
    PrimSpec child(Specifier::Def, "Xform", "Child");

    Reference ref;
    ref.asset_path = lightusd::legacy::value::AssetPath("other.usda");
    std::vector<std::pair<ListEditQual, std::vector<Reference>>> refs;
    refs.push_back({ListEditQual::ResetToExplicit, {ref}});
    child.metas().references = refs;
    root.children().push_back(child);

    layer.add_primspec("Root", root);
    TEST_CHECK(!layer.check_unresolved_references(/* max_depth */ 0));
    TEST_CHECK(layer.check_unresolved_references(/* max_depth */ 1));
  }
}

void layer_check_unresolved_payload_test(void) {
  // Layer with payload should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "PL");

    Payload pl;
    pl.asset_path = lightusd::legacy::value::AssetPath("payload.usda");
    std::vector<std::pair<ListEditQual, std::vector<Payload>>> payloads;
    payloads.push_back({ListEditQual::ResetToExplicit, {pl}});
    ps.metas().payload = payloads;

    layer.add_primspec("PL", ps);
    bool has_payload = layer.check_unresolved_payload();
    TEST_CHECK(has_payload);
  }

  // Layer without payload should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "NoPL");
    layer.add_primspec("NoPL", ps);
    bool has_payload = layer.check_unresolved_payload();
    TEST_CHECK(!has_payload);
  }
}

void layer_check_unresolved_inherits_test(void) {
  // Layer with inherits should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "Inh");

    std::vector<std::pair<ListEditQual, std::vector<Path>>> inherits;
    inherits.push_back({ListEditQual::ResetToExplicit, {Path("/BaseClass", "")}});
    ps.metas().inherits = inherits;

    layer.add_primspec("Inh", ps);
    bool has_inherits = layer.check_unresolved_inherits();
    TEST_CHECK(has_inherits);
  }

  // Layer without inherits should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "NoInh");
    layer.add_primspec("NoInh", ps);
    bool has_inherits = layer.check_unresolved_inherits();
    TEST_CHECK(!has_inherits);
  }
}

void layer_check_unresolved_specializes_test(void) {
  // Layer with specializes should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "Spec");

    std::vector<std::pair<ListEditQual, std::vector<Path>>> specializes;
    specializes.push_back({ListEditQual::ResetToExplicit, {Path("/SpecBase", "")}});
    ps.metas().specializes = specializes;

    layer.add_primspec("Spec", ps);
    bool has_spec = layer.check_unresolved_specializes();
    TEST_CHECK(has_spec);
  }

  // Layer without specializes should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "NoSpec");
    layer.add_primspec("NoSpec", ps);
    bool has_spec = layer.check_unresolved_specializes();
    TEST_CHECK(!has_spec);
  }
}

void layer_check_unresolved_variant_test(void) {
  // Layer with variant should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "Var");

    VariantSelectionMap vsmap;
    vsmap["modelingVariant"] = "default";
    ps.metas().variants = vsmap;

    std::vector<std::pair<ListEditQual, std::vector<std::string>>> variantSets;
    variantSets.push_back({ListEditQual::ResetToExplicit, {"modelingVariant"}});
    ps.metas().variantSets = variantSets;

    layer.add_primspec("Var", ps);
    bool has_variant = layer.check_unresolved_variant();
    TEST_CHECK(has_variant);
  }

  // Layer without variant should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "NoVar");
    layer.add_primspec("NoVar", ps);
    bool has_variant = layer.check_unresolved_variant();
    TEST_CHECK(!has_variant);
  }
}

void layer_check_over_primspec_test(void) {
  // Layer with Over specifier should return true
  {
    Layer layer;
    PrimSpec ps(Specifier::Over, "Xform", "OverPrim");
    layer.add_primspec("OverPrim", ps);
    bool has_over = layer.check_over_primspec();
    TEST_CHECK(has_over);
  }

  // Layer without Over specifier should return false
  {
    Layer layer;
    PrimSpec ps(Specifier::Def, "Xform", "DefPrim");
    layer.add_primspec("DefPrim", ps);
    bool has_over = layer.check_over_primspec();
    TEST_CHECK(!has_over);
  }
}

void layer_metas_test(void) {
  Layer layer;

  // Set layer name
  layer.set_name("TestLayer");
  TEST_CHECK(layer.name() == "TestLayer");

  // Set layer metas
  layer.metas().defaultPrim = lightusd::legacy::value::token("Root");
  TEST_CHECK(layer.metas().defaultPrim.str() == "Root");

  // Set upAxis
  layer.metas().upAxis = Axis::Z;
  TEST_CHECK(layer.metas().upAxis.get_value() == Axis::Z);
}

void layer_asset_resolution_state_test(void) {
  Layer layer;

  std::vector<std::string> search_paths = {"path1", "path2"};
  layer.set_asset_resolution_state("cwd", search_paths);

  TEST_CHECK(layer.get_current_working_path() == "cwd");

  std::vector<std::string> retrieved = layer.get_asset_search_paths();
  TEST_CHECK(retrieved.size() == 2);
  if (retrieved.size() == 2) {
    TEST_CHECK(retrieved[0] == "path1");
    TEST_CHECK(retrieved[1] == "path2");
  }
}

void layer_memory_estimation_test(void) {
  Layer layer;

  // Add some data
  PrimSpec ps(Specifier::Def, "Xform", "Root");
  PrimSpec child(Specifier::Def, "Mesh", "Child");
  ps.children().push_back(child);
  layer.add_primspec("Root", ps);

  size_t mem = layer.estimate_memory_usage();
  TEST_CHECK(mem > 0);
  TEST_MSG("Layer memory usage estimate: %zu bytes", mem);
}

// Regression: a moved-from Layer must remain valid and assignable. The internal
// _impl was previously left null by the move ctor / move assignment, so reusing
// a moved-from Layer as a copy-assignment target (as the composition fixed-point
// loop does: `a = std::move(b); Composite(..., &b);`) dereferenced a null _impl
// and crashed. These checks pin the "moved-from stays valid" invariant.
void layer_moved_from_is_valid_test(void) {
  // 1. Move-construct, then assign INTO the moved-from source.
  {
    Layer src;
    src.set_name("src");
    PrimSpec ps(Specifier::Def, "Xform", "Root");
    src.add_primspec("Root", ps);

    Layer dst(std::move(src));
    TEST_CHECK(dst.name() == "src");
    TEST_CHECK(dst.has_primspec("Root"));

    // Assigning into the moved-from `src` must not crash (was a null-deref).
    Layer other;
    other.set_name("other");
    src = other;  // copy-assign into moved-from
    TEST_CHECK(src.name() == "other");
  }

  // 2. Move-assign, then reuse the moved-from source as a copy-assign target.
  {
    Layer a, b;
    b.set_name("b");
    b.add_primspec("P", PrimSpec(Specifier::Def, "Mesh", "P"));

    a = std::move(b);
    TEST_CHECK(a.name() == "b");
    TEST_CHECK(a.has_primspec("P"));

    // `b` is moved-from; copy-assigning into it must be safe.
    Layer c;
    c.set_name("c");
    b = c;
    TEST_CHECK(b.name() == "c");

    // And copy-CONSTRUCTING from a freshly moved-from layer must be safe too.
    Layer d = std::move(a);
    Layer e(a);  // a is moved-from here
    TEST_CHECK(e.name().empty());
    TEST_CHECK(d.name() == "b");
  }
}

// Test that find_primspec_at correctly distinguishes prims with the same leaf name
// but different parent paths. This tests the fix for the cache correctness bug where
// the cache key was path.prim_part() (leaf name only) instead of path.full_path_name().
void layer_find_primspec_at_same_leaf_name_test(void) {
  Layer layer;
  layer.set_name("test_layer");

  // Add root prims with same leaf name under different parents
  layer.add_primspec("Alpha", PrimSpec(Specifier::Def, "Xform", "Alpha"));
  layer.add_primspec("Beta", PrimSpec(Specifier::Def, "Xform", "Beta"));

  // Build hierarchy: /Alpha/Child -> Mesh
  PrimSpec &alpha = layer.primspecs().at("Alpha");
  PrimSpec ps_alpha_child(Specifier::Def, "Mesh", "Child");
  alpha.children().push_back(ps_alpha_child);

  // Build hierarchy: /Beta/Child -> Sphere (same leaf name "Child")
  PrimSpec &beta = layer.primspecs().at("Beta");
  PrimSpec ps_beta_child(Specifier::Def, "Sphere", "Child");
  beta.children().push_back(ps_beta_child);

  // Test that find_primspec_at returns the correct prims
  const PrimSpec *found_alpha_child = nullptr;
  const PrimSpec *found_beta_child = nullptr;
  std::string err;

  bool ok1 = layer.find_primspec_at(Path("/Alpha/Child", ""), &found_alpha_child, &err);
  TEST_CHECK(ok1);
  TEST_CHECK(found_alpha_child != nullptr);
  TEST_CHECK(found_alpha_child->name() == "Child");
  TEST_CHECK(found_alpha_child->typeName() == "Mesh");

  bool ok2 = layer.find_primspec_at(Path("/Beta/Child", ""), &found_beta_child, &err);
  TEST_CHECK(ok2);
  TEST_CHECK(found_beta_child != nullptr);
  TEST_CHECK(found_beta_child->name() == "Child");
  TEST_CHECK(found_beta_child->typeName() == "Sphere");

  // Verify they are different prims
  TEST_CHECK(found_alpha_child != found_beta_child);

  // Now test the cache - look up the same paths again
  const PrimSpec *found_alpha_cached = nullptr;
  const PrimSpec *found_beta_cached = nullptr;

  bool ok1_cached = layer.find_primspec_at(Path("/Alpha/Child", ""), &found_alpha_cached, &err);
  TEST_CHECK(ok1_cached);
  TEST_CHECK(found_alpha_cached == found_alpha_child);  // Same pointer from cache

  bool ok2_cached = layer.find_primspec_at(Path("/Beta/Child", ""), &found_beta_cached, &err);
  TEST_CHECK(ok2_cached);
  TEST_CHECK(found_beta_cached == found_beta_child);  // Same pointer from cache

  // Critical: Verify that looking up /Beta/Child after /Alpha/Child doesn't return /Alpha/Child's prim
  // (This was the bug: cache key was just "Child" so both lookups would return same cached result)
  TEST_CHECK(found_beta_cached->typeName() == "Sphere");
  TEST_CHECK(found_alpha_cached->typeName() == "Mesh");
}

// Test that the cache lookup uses full path for correct O(1) unordered_map lookup
// rather than leaf name which would cause incorrect results when leaf names collide
void layer_find_primspec_at_cache_full_path_test(void) {
  Layer layer;
  layer.set_name("cache_test");

  // Add a root prim with children
  layer.add_primspec("Root", PrimSpec(Specifier::Def, "Xform", "Root"));
  PrimSpec &root = layer.primspecs().at("Root");

  // Add children
  for (int i = 0; i < 10; ++i) {
    std::string child_name = "Child" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Mesh", child_name.c_str());
    root.children().push_back(child);
  }

  std::string err;
  const PrimSpec *found = nullptr;

  // First lookup - should miss cache and populate it
  bool ok1 = layer.find_primspec_at(Path("/Root/Child0", ""), &found, &err);
  TEST_CHECK(ok1);
  TEST_CHECK(found != nullptr);
  TEST_CHECK(found->name() == "Child0");

  // Second lookup - should hit cache (same full path)
  bool ok2 = layer.find_primspec_at(Path("/Root/Child0", ""), &found, &err);
  TEST_CHECK(ok2);
  TEST_CHECK(found != nullptr);
  TEST_CHECK(found->name() == "Child0");

  // Lookup different prim - should also hit cache
  bool ok3 = layer.find_primspec_at(Path("/Root/Child5", ""), &found, &err);
  TEST_CHECK(ok3);
  TEST_CHECK(found != nullptr);
  TEST_CHECK(found->name() == "Child5");

  // Verify a non-existent path returns false
  const PrimSpec *not_found = nullptr;
  bool ok_not_found = layer.find_primspec_at(Path("/Root/NonExistent", ""), &not_found, &err);
  TEST_CHECK(!ok_not_found);
  TEST_CHECK(not_found == nullptr);
}

// Test deeply nested namespaces (99 levels deep) still resolve correctly.
void layer_deep_namespace_depth_test(void) {
  Layer layer;
  layer.set_name("deep_ns_test");

  layer.add_primspec("Root", PrimSpec(Specifier::Def, "Xform", "Root"));
  PrimSpec *current = &layer.primspecs().at("Root");

  for (int i = 0; i < 99; ++i) {
    std::string name = "L" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Xform", name.c_str());
    current->children().push_back(child);
    current = &current->children().back();
  }
  PrimSpec leaf(Specifier::Def, "Mesh", "Leaf");
  current->children().push_back(leaf);

  std::string err;
  const PrimSpec *found = nullptr;

  std::string path = "/Root";
  current = &layer.primspecs().at("Root");
  while (!current->children().empty()) {
    current = &current->children().back();
    path += "/" + current->name();
  }

  bool ok = layer.find_primspec_at(Path(path, ""), &found, &err);
  TEST_CHECK(ok);
  TEST_CHECK(found != nullptr);
  TEST_CHECK(found->name() == "Leaf");
  TEST_CHECK(found->typeName() == "Mesh");
}

// Regression test for children_by_parent hash-map optimization:
// 50 children under one parent must all resolve correctly.
void layer_children_by_parent_test(void) {
  Layer layer;
  layer.set_name("children_map_test");

  layer.add_primspec("Root", PrimSpec(Specifier::Def, "Xform", "Root"));
  PrimSpec &root = layer.primspecs().at("Root");

  for (int i = 0; i < 50; ++i) {
    std::string name = "Child" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Mesh", name.c_str());
    root.children().push_back(child);
  }

  std::string err;
  const PrimSpec *found = nullptr;

  for (int i = 0; i < 50; ++i) {
    std::string path = "/Root/Child" + std::to_string(i);
    bool ok = layer.find_primspec_at(Path(path, ""), &found, &err);
    TEST_CHECK(ok);
    TEST_CHECK(found != nullptr);
    TEST_CHECK(found->name() == ("Child" + std::to_string(i)));
  }
}

// Verify find_primspec_at handles deep trees (depth 5000, ~40KB path).
void layer_max_prim_path_length_test(void) {
  Layer layer;
  layer.set_name("path_length_test");

  layer.add_primspec("Root", PrimSpec(Specifier::Def, "Xform", "Root"));

  PrimSpec *current = &layer.primspecs().at("Root");
  for (int i = 0; i < 5000; ++i) {
    std::string name = "D" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Xform", name.c_str());
    current->children().push_back(child);
    current = &current->children().back();
  }
  PrimSpec leaf(Specifier::Def, "Mesh", "Leaf");
  current->children().push_back(leaf);

  std::string err;
  const PrimSpec *found = nullptr;

  std::string path = "/Root";
  current = &layer.primspecs().at("Root");
  while (!current->children().empty()) {
    current = &current->children().back();
    path += "/" + current->name();
  }

  bool ok = layer.find_primspec_at(Path(path, ""), &found, &err);
  TEST_CHECK(ok);
  TEST_CHECK(found != nullptr);
  TEST_CHECK(found->name() == "Leaf");
}
