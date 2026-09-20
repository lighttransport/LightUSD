#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-tydra-shader.h"

#include <algorithm>
#include <cstring>

#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-converter.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "core/prim.hh"
#include "stage.hh"

using namespace lightusd;

namespace {

bool ContainsString(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// a. ListPrims<GeomMesh> - parse USDA with multiple meshes, verify map
// ---------------------------------------------------------------------------
void tydra_shader_list_prims_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MeshA" {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
  }
  def Mesh "MeshB" {
    point3f[] points = [(0,0,0),(2,0,0),(0,2,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::PathPrimMap<GeomMesh> mesh_map;
  ok = tydra::ListPrims(stage, mesh_map);
  TEST_CHECK(ok);
  TEST_MSG("ListPrims<GeomMesh> should succeed");

  TEST_CHECK(mesh_map.size() == 2);
  TEST_MSG("Expected 2 meshes in map, got %zu", mesh_map.size());

  TEST_CHECK(mesh_map.count("/Root/MeshA") == 1);
  TEST_MSG("Map should contain /Root/MeshA");

  TEST_CHECK(mesh_map.count("/Root/MeshB") == 1);
  TEST_MSG("Map should contain /Root/MeshB");

  // Verify pointers are non-null
  TEST_CHECK(mesh_map["/Root/MeshA"] != nullptr);
  TEST_MSG("/Root/MeshA pointer should be non-null");

  TEST_CHECK(mesh_map["/Root/MeshB"] != nullptr);
  TEST_MSG("/Root/MeshB pointer should be non-null");
}

// ---------------------------------------------------------------------------
// b. ListShaders<UsdPreviewSurface> - parse USDA with material, verify shader
// ---------------------------------------------------------------------------
void tydra_shader_list_shaders_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Scope "Materials" {
    def Material "Mat" {
      token outputs:surface.connect = </Root/Materials/Mat/PBR.outputs:surface>
      def Shader "PBR" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
        float inputs:roughness = 0.3
        token outputs:surface
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::PathShaderMap<UsdPreviewSurface> shader_map;
  ok = tydra::ListShaders(stage, shader_map);
  TEST_CHECK(ok);
  TEST_MSG("ListShaders<UsdPreviewSurface> should succeed");

  TEST_CHECK(shader_map.size() >= 1);
  TEST_MSG("Expected at least 1 shader, got %zu", shader_map.size());

  // The shader should be found at /Root/Materials/Mat/PBR
  TEST_CHECK(shader_map.count("/Root/Materials/Mat/PBR") == 1);
  TEST_MSG("Shader map should contain /Root/Materials/Mat/PBR");

  // Verify both the generic Shader and typed UsdPreviewSurface pointers
  const auto &entry = shader_map["/Root/Materials/Mat/PBR"];
  TEST_CHECK(entry.first != nullptr);
  TEST_MSG("Generic Shader pointer should be non-null");

  TEST_CHECK(entry.second != nullptr);
  TEST_MSG("Typed UsdPreviewSurface pointer should be non-null");
}

// ---------------------------------------------------------------------------
// c. GetBoundMaterial - parse USDA with material binding, verify resolution
// ---------------------------------------------------------------------------
void tydra_shader_get_bound_material_test(void) {
  const char *usda = R"(#usda 1.0
(
  defaultPrim = "Root"
)
def Xform "Root" {
  def Mesh "Box" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </Root/Looks/SimpleMat>
  }
  def Scope "Looks" {
    def Material "SimpleMat" {
      token outputs:surface.connect = </Root/Looks/SimpleMat/PBR.outputs:surface>
      def Shader "PBR" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
        float inputs:roughness = 0.3
        token outputs:surface
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  Path mat_path;
  const Material *material = nullptr;
  err.clear();

  ok = tydra::GetBoundMaterial(
      stage, Path("/Root/Box", ""), "", &mat_path, &material, &err);
  TEST_CHECK(ok);
  TEST_MSG("GetBoundMaterial should succeed: %s", err.c_str());

  TEST_CHECK(material != nullptr);
  TEST_MSG("Material pointer should be non-null");

  // The material path should resolve to /Root/Looks/SimpleMat
  std::string mat_path_str = mat_path.prim_part();
  TEST_CHECK(mat_path_str == "/Root/Looks/SimpleMat");
  TEST_MSG("Material path should be /Root/Looks/SimpleMat, got %s",
           mat_path_str.c_str());
}

void tydra_shader_collection_material_binding_test(void) {
  const char *usda = R"(#usda 1.0
def Scope "Root" (
  prepend apiSchemas = ["CollectionAPI:hero", "MaterialBindingAPI"]
) {
  rel collection:hero:includes = [</Root/Included>]
  rel material:binding:collection:hero = </Looks/CollectionMat>
  def Mesh "Included" {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
  }
}
def Mesh "Excluded" {
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
}
def Scope "Looks" {
  def Material "CollectionMat" {}
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(LoadUSDAFromMemory(reinterpret_cast<const uint8_t *>(usda),
                                std::strlen(usda), "collection.usda", &stage,
                                &warn, &err));
  auto root_result = stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(bool(root_result));
  const Scope *root_scope = root_result ? (*root_result)->as<Scope>() : nullptr;
  TEST_CHECK(root_scope != nullptr);
  if (root_scope) {
    TEST_CHECK(root_scope->materialBindingCollectionMap().count("hero") == 1);
    TEST_CHECK(root_scope->has_instance("hero"));
  }
  Path material_path;
  const Material *material = nullptr;
  TEST_CHECK(tydra::GetBoundMaterial(stage, Path("/Root/Included", ""), "",
                                     &material_path, &material, &err));
  TEST_CHECK(material != nullptr);
  TEST_CHECK(material_path.prim_part() == "/Looks/CollectionMat");

  material = nullptr;
  material_path = Path();
  err.clear();
  TEST_CHECK(!tydra::GetBoundMaterial(stage, Path("/Excluded", ""), "",
                                      &material_path, &material, &err));
  TEST_CHECK(material == nullptr);
}

// ---------------------------------------------------------------------------
// d. Property access - GetPropertyNames and GetAttribute on a Mesh prim
// ---------------------------------------------------------------------------
void tydra_shader_property_access_test(void) {
  const char *usda = R"(#usda 1.0
def Mesh "TestMesh" {
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  // Get the prim at path
  auto prim_result = stage.GetPrimAtPath(Path("/TestMesh", ""));
  TEST_CHECK(prim_result.has_value());
  TEST_MSG("GetPrimAtPath /TestMesh should succeed");

  const Prim *prim_ptr = prim_result.value();
  TEST_CHECK(prim_ptr != nullptr);
  TEST_MSG("Prim pointer should be non-null");

  // GetPropertyNames
  std::vector<std::string> prop_names;
  err.clear();
  ok = tydra::GetPropertyNames(*prim_ptr, &prop_names, &err);
  TEST_CHECK(ok);
  TEST_MSG("GetPropertyNames should succeed: %s", err.c_str());

  TEST_CHECK(!prop_names.empty());
  TEST_MSG("Property names should not be empty");

  TEST_CHECK(ContainsString(prop_names, "points"));
  TEST_MSG("Property names should include 'points'");

  TEST_CHECK(ContainsString(prop_names, "faceVertexCounts"));
  TEST_MSG("Property names should include 'faceVertexCounts'");

  TEST_CHECK(ContainsString(prop_names, "faceVertexIndices"));
  TEST_MSG("Property names should include 'faceVertexIndices'");

  // GetAttribute for "points"
  Attribute attr;
  err.clear();
  ok = tydra::GetAttribute(*prim_ptr, "points", &attr, &err);
  TEST_CHECK(ok);
  TEST_MSG("GetAttribute('points') should succeed: %s", err.c_str());
}

// ---------------------------------------------------------------------------
// e. BuildXformNodeFromStage - parse hierarchy, verify root has children
// ---------------------------------------------------------------------------
void tydra_shader_xform_node_build_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Xform "ChildA" {
    def Mesh "MeshInA" {
      point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
    }
  }
  def Xform "ChildB" {
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::XformNode root;
  ok = tydra::BuildXformNodeFromStage(stage, &root);
  TEST_CHECK(ok);
  TEST_MSG("BuildXformNodeFromStage should succeed");

  // Root node should have children (Root is a child of the pseudo-root)
  TEST_CHECK(!root.children.empty());
  TEST_MSG("Root XformNode should have children, got %zu",
           root.children.size());

  // Find the "Root" xform among children
  bool found_root = false;
  for (const auto &child : root.children) {
    if (child.element_name == "Root") {
      found_root = true;
      // Root should have ChildA and ChildB
      TEST_CHECK(child.children.size() >= 2);
      TEST_MSG("Root xform should have at least 2 children, got %zu",
               child.children.size());
      break;
    }
  }
  TEST_CHECK(found_root);
  TEST_MSG("Should find 'Root' xform node in hierarchy");
}
