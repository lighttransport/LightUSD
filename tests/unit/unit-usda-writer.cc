#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usda-writer.h"
#include "lightusd.hh"
#include "usda-writer.hh"
#include "usdGeom.hh"
#include "stage.hh"

#include <string>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {

static bool parse_usda(const std::string &usda_str, Stage *stage) {
  std::string warn, err;
  return LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda_str.data()), usda_str.size(),
      "test.usda", stage, &warn, &err);
}

}  // namespace

void usda_writer_empty_stage_test(void) {
  Stage stage;
  std::string output = stage.ExportToString();

  // Should start with USDA header
  TEST_CHECK(output.size() > 0);
  TEST_CHECK(output.find("#usda 1.0") != std::string::npos);
  TEST_MSG("Empty stage export should contain '#usda 1.0' header");

  // Should be parseable
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Empty stage export should be re-parseable");
}

void usda_writer_xform_test(void) {
  Stage stage;

  Xform xform;
  xform.name = "MyXform";
  Prim prim("MyXform", xform);
  stage.add_root_prim(std::move(prim));

  std::string output = stage.ExportToString();

  TEST_CHECK(output.find("def Xform") != std::string::npos);
  TEST_MSG("Export should contain 'def Xform'");
  TEST_CHECK(output.find("MyXform") != std::string::npos);
  TEST_MSG("Export should contain prim name 'MyXform'");

  // Re-parse and verify
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Xform USDA should be re-parseable");

  if (ok) {
    TEST_CHECK(reparsed.root_prims().size() == 1);
    if (reparsed.root_prims().size() > 0) {
      TEST_CHECK(reparsed.root_prims()[0].element_name() == "MyXform");
      TEST_CHECK(reparsed.root_prims()[0].is<Xform>());
    }
  }
}

void usda_writer_mesh_test(void) {
  Stage stage;

  GeomMesh mesh;
  mesh.name = "MyMesh";
  mesh.points.set_value(
      std::vector<lightusd::legacy::value::point3f>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
  mesh.faceVertexCounts.set_value(std::vector<int32_t>({3}));
  mesh.faceVertexIndices.set_value(std::vector<int32_t>({0, 1, 2}));

  Prim prim("MyMesh", mesh);
  stage.add_root_prim(std::move(prim));

  std::string output = stage.ExportToString();

  TEST_CHECK(output.find("def Mesh") != std::string::npos);
  TEST_MSG("Export should contain 'def Mesh'");
  TEST_CHECK(output.find("MyMesh") != std::string::npos);
  TEST_MSG("Export should contain mesh name 'MyMesh'");

  // Re-parse and verify
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Mesh USDA should be re-parseable");

  if (ok) {
    TEST_CHECK(reparsed.root_prims().size() == 1);
    if (reparsed.root_prims().size() > 0) {
      TEST_CHECK(reparsed.root_prims()[0].is<GeomMesh>());
      const GeomMesh *m = reparsed.root_prims()[0].as<GeomMesh>();
      TEST_CHECK(m != nullptr);
      if (m) {
        auto pts = m->points.get_value();
        TEST_CHECK(pts.has_value());
      }
    }
  }
}

void usda_writer_material_test(void) {
  Stage stage;

  Material mat;
  mat.name = "MyMat";

  Prim prim("MyMat", mat);
  stage.add_root_prim(std::move(prim));

  std::string output = stage.ExportToString();

  TEST_CHECK(output.find("MyMat") != std::string::npos);
  TEST_MSG("Export should contain material name 'MyMat'");
  TEST_CHECK(output.find("Material") != std::string::npos);
  TEST_MSG("Export should contain 'Material' type");

  // Re-parse and verify
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Material USDA should be re-parseable");

  if (ok) {
    TEST_CHECK(reparsed.root_prims().size() == 1);
    if (reparsed.root_prims().size() > 0) {
      TEST_CHECK(reparsed.root_prims()[0].is<Material>());
    }
  }
}

void usda_writer_metadata_test(void) {
  Stage stage;

  // Set stage metadata
  stage.metas().defaultPrim = lightusd::legacy::value::token("Root");
  stage.metas().upAxis.set_value(Axis::Y);
  stage.metas().metersPerUnit.set_value(0.01);

  std::string output = stage.ExportToString();

  TEST_CHECK(output.find("defaultPrim") != std::string::npos);
  TEST_MSG("Export should contain 'defaultPrim' metadata");
  TEST_CHECK(output.find("upAxis") != std::string::npos);
  TEST_MSG("Export should contain 'upAxis' metadata");
  TEST_CHECK(output.find("metersPerUnit") != std::string::npos);
  TEST_MSG("Export should contain 'metersPerUnit' metadata");

  // Re-parse and verify metadata survives
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  if (ok) {
    TEST_CHECK(reparsed.metas().defaultPrim.str() == "Root");
    TEST_MSG("defaultPrim should survive round-trip");
  }
}

void usda_writer_hierarchy_test(void) {
  Stage stage;

  // Build nested hierarchy: /Parent/Child/GrandChild
  Xform parent_xform;
  parent_xform.name = "Parent";
  Prim parent("Parent", parent_xform);

  Xform child_xform;
  child_xform.name = "Child";
  Prim child("Child", child_xform);

  GeomMesh gc_mesh;
  gc_mesh.name = "GrandChild";
  Prim grandchild("GrandChild", gc_mesh);

  child.add_child(std::move(grandchild));
  parent.add_child(std::move(child));
  stage.add_root_prim(std::move(parent));

  std::string output = stage.ExportToString();

  TEST_CHECK(output.find("Parent") != std::string::npos);
  TEST_CHECK(output.find("Child") != std::string::npos);
  TEST_CHECK(output.find("GrandChild") != std::string::npos);
  TEST_MSG("Export should contain all prim names in hierarchy");

  // Re-parse and verify hierarchy
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Hierarchy USDA should be re-parseable");

  if (ok) {
    TEST_CHECK(reparsed.root_prims().size() == 1);
    if (reparsed.root_prims().size() > 0) {
      const Prim &rp = reparsed.root_prims()[0];
      TEST_CHECK(rp.element_name() == "Parent");
      TEST_CHECK(rp.children().size() == 1);
      if (rp.children().size() > 0) {
        TEST_CHECK(rp.children()[0].element_name() == "Child");
        TEST_CHECK(rp.children()[0].children().size() == 1);
        if (rp.children()[0].children().size() > 0) {
          TEST_CHECK(
              rp.children()[0].children()[0].element_name() == "GrandChild");
        }
      }
    }
  }
}

void usda_writer_special_chars_test(void) {
  Stage stage;

  // Names with underscores and numbers (valid USD identifiers)
  Xform xform1;
  xform1.name = "Prim_With_Underscores";
  Prim prim1("Prim_With_Underscores", xform1);
  stage.add_root_prim(std::move(prim1));

  Xform xform2;
  xform2.name = "Prim123";
  Prim prim2("Prim123", xform2);
  stage.add_root_prim(std::move(prim2));

  Xform xform3;
  xform3.name = "_LeadingUnderscore";
  Prim prim3("_LeadingUnderscore", xform3);
  stage.add_root_prim(std::move(prim3));

  std::string output = stage.ExportToString();

  // Re-parse and verify names survive round-trip
  Stage reparsed;
  bool ok = parse_usda(output, &reparsed);
  TEST_CHECK(ok);
  TEST_MSG("Special chars USDA should be re-parseable");

  if (ok) {
    TEST_CHECK(reparsed.root_prims().size() == 3);

    bool found_underscore = false;
    bool found_numbers = false;
    bool found_leading = false;
    for (const auto &p : reparsed.root_prims()) {
      if (p.element_name() == "Prim_With_Underscores") found_underscore = true;
      if (p.element_name() == "Prim123") found_numbers = true;
      if (p.element_name() == "_LeadingUnderscore") found_leading = true;
    }

    TEST_CHECK(found_underscore);
    TEST_MSG("Prim with underscores should survive round-trip");
    TEST_CHECK(found_numbers);
    TEST_MSG("Prim with numbers should survive round-trip");
    TEST_CHECK(found_leading);
    TEST_MSG("Prim with leading underscore should survive round-trip");
  }
}

void usda_writer_idempotent_test(void) {
  // Start with a known USDA string
  std::string input_usda =
      "#usda 1.0\n"
      "(\n"
      "    defaultPrim = \"Root\"\n"
      ")\n"
      "\n"
      "def Xform \"Root\"\n"
      "{\n"
      "    def Mesh \"MyMesh\"\n"
      "    {\n"
      "    }\n"
      "}\n";

  // Parse -> export
  Stage stage1;
  bool ok1 = parse_usda(input_usda, &stage1);
  TEST_CHECK(ok1);
  TEST_MSG("First parse should succeed");

  if (!ok1) return;

  std::string export1 = stage1.ExportToString();
  TEST_CHECK(!export1.empty());

  // Re-parse -> re-export
  Stage stage2;
  bool ok2 = parse_usda(export1, &stage2);
  TEST_CHECK(ok2);
  TEST_MSG("Second parse should succeed");

  if (!ok2) return;

  std::string export2 = stage2.ExportToString();
  TEST_CHECK(!export2.empty());

  // The two exports should be identical
  TEST_CHECK(export1 == export2);
  TEST_MSG("Idempotent test: second export should match first export");
}

void usda_writer_api_test(void) {
  Stage stage;

  Xform xform;
  xform.name = "TestPrim";
  Prim prim("TestPrim", xform);
  stage.add_root_prim(std::move(prim));

  // ExportToString
  std::string export_str = stage.ExportToString();
  TEST_CHECK(!export_str.empty());

  // usda::ExportToUSDAString
  std::string api_output, warn, err;
  bool ok = usda::ExportToUSDAString(stage, &api_output, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("ExportToUSDAString should succeed");

  // Both should produce valid USDA
  TEST_CHECK(!api_output.empty());
  TEST_CHECK(api_output.find("#usda 1.0") != std::string::npos);
  TEST_MSG("ExportToUSDAString output should contain USDA header");

  // Both should be parseable
  Stage parsed1, parsed2;
  bool ok1 = parse_usda(export_str, &parsed1);
  bool ok2 = parse_usda(api_output, &parsed2);
  TEST_CHECK(ok1);
  TEST_CHECK(ok2);
  TEST_MSG("Both API outputs should be re-parseable");

  // Both should have same prim count
  if (ok1 && ok2) {
    TEST_CHECK(parsed1.root_prims().size() == parsed2.root_prims().size());
  }
}
