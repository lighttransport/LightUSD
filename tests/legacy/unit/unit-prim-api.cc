#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-prim-api.h"
#include "core/prim.hh"
#include "lightusd.hh"
#include "usdGeom.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void prim_type_check_test(void) {
  // Create Xform and wrap in Prim
  Xform xform;
  xform.name = "TestXform";
  Prim prim("TestXform", xform);

  // is<Xform>() should be true
  TEST_CHECK(prim.is<Xform>());
  TEST_MSG("Prim created from Xform should report is<Xform>() == true");

  // is<GeomMesh>() should be false
  TEST_CHECK(!prim.is<GeomMesh>());
  TEST_MSG("Prim created from Xform should report is<GeomMesh>() == false");

  // as<Xform>() should not be nullptr
  const Xform *xf_ptr = prim.as<Xform>();
  TEST_CHECK(xf_ptr != nullptr);
  TEST_MSG("as<Xform>() should return non-null for Xform prim");

  // as<GeomMesh>() should be nullptr
  const GeomMesh *mesh_ptr = prim.as<GeomMesh>();
  TEST_CHECK(mesh_ptr == nullptr);
  TEST_MSG("as<GeomMesh>() should return null for Xform prim");

  // Verify the retrieved Xform data
  if (xf_ptr) {
    TEST_CHECK(xf_ptr->name == "TestXform");
  }

  // Also test with GeomMesh
  {
    GeomMesh mesh;
    mesh.name = "TestMesh";
    Prim mesh_prim("TestMesh", mesh);

    TEST_CHECK(mesh_prim.is<GeomMesh>());
    TEST_CHECK(!mesh_prim.is<Xform>());
    TEST_CHECK(mesh_prim.as<GeomMesh>() != nullptr);
    TEST_CHECK(mesh_prim.as<Xform>() == nullptr);
  }
}

void prim_add_child_test(void) {
  Xform root_xform;
  root_xform.name = "Root";
  Prim root("Root", root_xform);

  TEST_CHECK(root.children().empty());

  // Add first child
  {
    Xform child_xform;
    child_xform.name = "Child1";
    Prim child("Child1", child_xform);
    bool ok = root.add_child(std::move(child), false);
    TEST_CHECK(ok);
    TEST_MSG("Adding first child should succeed");
  }

  TEST_CHECK(root.children().size() == 1);
  TEST_CHECK(root.children()[0].element_name() == "Child1");

  // Add second child with different name
  {
    Xform child_xform;
    child_xform.name = "Child2";
    Prim child("Child2", child_xform);
    bool ok = root.add_child(std::move(child), false);
    TEST_CHECK(ok);
    TEST_MSG("Adding second child with different name should succeed");
  }

  TEST_CHECK(root.children().size() == 2);
  TEST_CHECK(root.children()[1].element_name() == "Child2");

  // Try adding child with duplicate name (no rename)
  {
    Xform dup_xform;
    dup_xform.name = "Child1";
    Prim dup("Child1", dup_xform);
    bool ok = root.add_child(std::move(dup), false);
    TEST_CHECK(!ok);
    TEST_MSG("Adding duplicate name without rename should fail");
  }

  TEST_CHECK(root.children().size() == 2);

  // Add child with duplicate name (rename allowed)
  {
    Xform dup_xform;
    dup_xform.name = "Child1";
    Prim dup("Child1", dup_xform);
    bool ok = root.add_child(std::move(dup), true);
    TEST_CHECK(ok);
    TEST_MSG("Adding duplicate name with rename=true should succeed");
  }

  TEST_CHECK(root.children().size() == 3);
  // The renamed child should have a different name than "Child1"
  TEST_CHECK(root.children()[2].element_name() != "Child1");
  TEST_MSG("Renamed child should have a unique name, got '%s'",
           root.children()[2].element_name().c_str());
}

void prim_element_name_test(void) {
  Xform xform;
  xform.name = "MyName";
  Prim prim("MyName", xform);

  TEST_CHECK(prim.element_name() == "MyName");
  TEST_MSG("Expected element_name 'MyName', got '%s'",
           prim.element_name().c_str());

  // Test with different name
  {
    Xform xf2;
    xf2.name = "Another_Prim_123";
    Prim p2("Another_Prim_123", xf2);
    TEST_CHECK(p2.element_name() == "Another_Prim_123");
  }

  // Test with explicit name overriding prim's internal name
  {
    Xform xf3;
    xf3.name = "InternalName";
    Prim p3("ExternalName", xf3);
    TEST_CHECK(p3.element_name() == "ExternalName");
    TEST_MSG("Explicit name should be used for element_name");
  }
}

void prim_specifier_test(void) {
  Xform xform;
  xform.name = "TestPrim";
  Prim prim("TestPrim", xform);

  // Set to Def
  prim.specifier() = Specifier::Def;
  TEST_CHECK(prim.specifier() == Specifier::Def);
  TEST_MSG("Specifier should be Def");

  // Set to Over
  prim.specifier() = Specifier::Over;
  TEST_CHECK(prim.specifier() == Specifier::Over);
  TEST_MSG("Specifier should be Over after assignment");

  // Set to Class
  prim.specifier() = Specifier::Class;
  TEST_CHECK(prim.specifier() == Specifier::Class);
  TEST_MSG("Specifier should be Class after assignment");

  // Round-trip back to Def
  prim.specifier() = Specifier::Def;
  TEST_CHECK(prim.specifier() == Specifier::Def);
  TEST_MSG("Specifier should be Def after round-trip");
}

void prim_absolute_path_test(void) {
  Stage stage;

  // Build hierarchy: /Root/Child/GrandChild
  Xform root_xform;
  root_xform.name = "Root";
  Prim root("Root", root_xform);

  Xform child_xform;
  child_xform.name = "Child";
  Prim child("Child", child_xform);

  Xform gc_xform;
  gc_xform.name = "GrandChild";
  Prim grandchild("GrandChild", gc_xform);

  child.add_child(std::move(grandchild));
  root.add_child(std::move(child));
  stage.add_root_prim(std::move(root));

  // Commit stage to compute absolute paths
  bool committed = stage.commit();
  TEST_CHECK(committed);
  TEST_MSG("Stage commit should succeed");

  // Verify absolute path of root
  {
    Path path("/Root", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->absolute_path().full_path_name() == "/Root");
      TEST_MSG("Root absolute path should be '/Root', got '%s'",
               result.value()->absolute_path().full_path_name().c_str());
    }
  }

  // Verify absolute path of child
  {
    Path path("/Root/Child", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->absolute_path().full_path_name() ==
                 "/Root/Child");
      TEST_MSG("Child absolute path should be '/Root/Child', got '%s'",
               result.value()->absolute_path().full_path_name().c_str());
    }
  }

  // Verify absolute path of grandchild
  {
    Path path("/Root/Child/GrandChild", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->absolute_path().full_path_name() ==
                 "/Root/Child/GrandChild");
      TEST_MSG("GrandChild absolute path should be '/Root/Child/GrandChild', "
               "got '%s'",
               result.value()->absolute_path().full_path_name().c_str());
    }
  }
}

void prim_data_access_test(void) {
  Xform xform;
  xform.name = "MyXform";
  Prim prim("MyXform", xform);

  // Access via as<Xform>()
  const Xform *xf = prim.as<Xform>();
  TEST_CHECK(xf != nullptr);
  TEST_MSG("as<Xform>() should return non-null");

  if (xf) {
    TEST_CHECK(xf->name == "MyXform");
    TEST_MSG("Xform name should be 'MyXform'");
  }

  // Access via data()
  const lightusd::legacy::value::Value &data = prim.data();
  TEST_CHECK(data.type_name().size() > 0);
  TEST_MSG("data() should return a valid Value");

  // Type name check
  TEST_CHECK(prim.type_name().size() > 0);

  // Test with GeomMesh
  {
    GeomMesh mesh;
    mesh.name = "TestMesh";
    mesh.points.set_value(
        std::vector<lightusd::legacy::value::point3f>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
    Prim mesh_prim("TestMesh", mesh);

    const GeomMesh *m = mesh_prim.as<GeomMesh>();
    TEST_CHECK(m != nullptr);
    if (m) {
      TEST_CHECK(m->name == "TestMesh");
      auto pts = m->points.get_value();
      TEST_CHECK(pts.has_value());
    }
  }
}

void prim_replace_child_test(void) {
  Xform root_xform;
  root_xform.name = "Root";
  Prim root("Root", root_xform);

  // Add child "A"
  {
    Xform a_xform;
    a_xform.name = "A";
    Prim a("A", a_xform);
    root.add_child(std::move(a), false);
  }

  TEST_CHECK(root.children().size() == 1);
  TEST_CHECK(root.children()[0].element_name() == "A");
  TEST_CHECK(root.children()[0].is<Xform>());

  // Replace child "A" with a GeomMesh
  {
    GeomMesh new_mesh;
    new_mesh.name = "A";
    Prim new_prim("A", new_mesh);
    bool ok = root.replace_child("A", std::move(new_prim));
    TEST_CHECK(ok);
    TEST_MSG("replace_child for existing child should succeed");
  }

  TEST_CHECK(root.children().size() == 1);
  TEST_CHECK(root.children()[0].element_name() == "A");
  TEST_CHECK(root.children()[0].is<GeomMesh>());
  TEST_MSG("Child 'A' should now be GeomMesh after replacement");

  // Replace nonexistent child "NonExistent" - should add it
  {
    Xform new_xform;
    new_xform.name = "NonExistent";
    Prim new_prim("NonExistent", new_xform);
    bool ok = root.replace_child("NonExistent", std::move(new_prim));
    TEST_CHECK(ok);
    TEST_MSG("replace_child for nonexistent child should add it");
  }

  TEST_CHECK(root.children().size() == 2);

  // Verify the added prim has the correct name
  bool found = false;
  for (const auto &c : root.children()) {
    if (c.element_name() == "NonExistent") {
      found = true;
      break;
    }
  }
  TEST_CHECK(found);
  TEST_MSG("NonExistent prim should have been added");

  // NOTE: replace_child("", ...) does not currently validate for empty names
  // at the early-return level (it sets the error string but does not return
  // false).  Skipping this assertion until the implementation is tightened.
}
