// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "usdz/geometry-optimize.hh"
#include "writer/usda-writer.hh"
#include "writer/usdc-writer.hh"

#define CHECK(expr)                                               \
  do {                                                            \
    if (!(expr)) {                                                \
      std::cerr << "Failed line " << __LINE__ << ": " #expr "\n"; \
      return 1;                                                   \
    }                                                             \
  } while (false)
namespace {
std::string Mesh(const std::string& name) {
  return "def Mesh \"" + name + R"USD(" {
    rel material:binding = </Root/Mat>
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    normal3f[] normals = [(0.70710677,0.70710677,0),(0.70710677,0.70710677,0),(0.70710677,0.70710677,0)] (interpolation="vertex")
    texCoord2f[] primvars:st = [(0,0),(1,0),(0,1)] (interpolation="faceVarying")
    int[] primvars:st:indices = [0,1,2]
    float[] primvars:weights = [1,2,3,4,5,6] (interpolation="vertex"; elementSize=2)
    uint2[] primvars:ids = [(1,2),(3,4),(5,6)] (interpolation="vertex")
    int[] cornerIndices = [2]
    float[] cornerSharpnesses = [2]
    int[] creaseIndices = [0,1]
    int[] creaseLengths = [2]
    float[] creaseSharpnesses = [1]
    int[] holeIndices = [0]
    def GeomSubset "Look" {
      uniform token elementType = "face"
      uniform token familyName = "materialBind"
      int[] indices = [0]
      rel material:binding = </Root/OtherMat>
    }
  })USD";
}
}  // namespace
int main() {
  using namespace lightusd;
  std::string source = R"USD(#usda 1.0
    (defaultPrim="Root")
    def Xform "Root" {
      def Material "Mat" {}
      def Material "OtherMat" {}
      def Xform "A" {
        double3 xformOp:scale = (2,1,1)
        uniform token[] xformOpOrder = ["xformOp:scale"]
  )USD" + Mesh("M") + R"USD(}
      def Xform "B" {
        matrix4d xformOp:transform = ((-2,0,0,0),(0,1,0,0),(0,0,1,0),(3,0,0,1))
        uniform token[] xformOpOrder = ["xformOp:transform"]
  )USD" + Mesh("M") + "}}";
  auto loaded = LoadUSDAFromString(source);
  CHECK(loaded.success);
  usdz::MeshMergeOptions options;
  usdz::GeometryOptimizationStats stats;
  std::string warn, error;
  CHECK(usdz::MergeMeshes(&loaded.stage, options, &stats, &warn, &error));
  CHECK(stats.num_meshes_merged == 2 && stats.num_mesh_aggregates == 1);
  const Layer* layer = loaded.stage.GetRootLayer();
  CHECK(!layer->prim_at_path("/Root/A/M") && !layer->prim_at_path("/Root/B/M"));
  const PrimSpec* mesh =
      layer->prim_at_path("/Root/__LightUSD_MeshMerge/Merged_0");
  CHECK(mesh &&
        !mesh->property(
            "subdivisionScheme"));  // USD's default Catmull-Clark is retained.
  CHECK(mesh->property_value("points")->array_size() == 6);
  CHECK(mesh->property_value("faceVertexIndices")->as_int_array()[3] == 5);
  CHECK(mesh->property_value("cornerIndices")->as_int_array()[1] == 5);
  CHECK(mesh->property_value("creaseIndices")->as_int_array()[2] == 3);
  CHECK(mesh->property_value("holeIndices")->as_int_array()[1] == 1);
  CHECK(mesh->property_value("primvars:weights")->array_size() == 12);
  CHECK(mesh->property_meta("primvars:weights")->elementSize == 2);
  CHECK(mesh->property_value("primvars:ids")->type_id() == TypeId::UInt2);
  CHECK(mesh->property_value("primvars:ids")->array_size() == 6);
  CHECK(mesh->property_value("primvars:st:indices")->array_size() == 6);
  const Value& normals = *mesh->property_value("normals");
  const float* n = static_cast<const float*>(normals.raw_data());
  CHECK(std::abs(n[0] - 0.4472136f) < 1e-6f &&
        std::abs(n[1] - 0.8944272f) < 1e-6f);
  CHECK(std::abs(n[9] + 0.4472136f) < 1e-6f &&
        std::abs(n[10] - 0.8944272f) < 1e-6f);
  const float* uv = static_cast<const float*>(
      mesh->property_value("primvars:st")->raw_data());
  CHECK(uv[6] == 0 && uv[7] == 0 && uv[10] == 0 && uv[11] == 1);
  CHECK(mesh->property_value("primvars:st:indices")->as_int_array()[3] == 5);
  CHECK(mesh->child_indices().size() == 2);
  CHECK(layer->prim(mesh->child_indices()[1])
            ->property_value("indices")
            ->as_int_array()[0] == 1);
  // Crate serialization must not revive unlinked source meshes or their
  // subsets.
  std::vector<uint8_t> bytes;
  CHECK(WriteUSDCToMemory(bytes, loaded.stage).success);
  auto binary = LoadUSDCFromMemory(bytes.data(), bytes.size());
  CHECK(binary.success &&
        !binary.stage.GetRootLayer()->prim_at_path("/Root/A/M"));
  CHECK(binary.stage.GetRootLayer()->prim_at_path(mesh->path().str()));
  // Capacity limits keep complete source meshes; no material-fragment
  // duplication.
  auto unchanged = LoadUSDAFromString(source);
  options.max_aggregate_faces = 1;
  CHECK(usdz::MergeMeshes(&unchanged.stage, options, &stats, &warn, &error));
  CHECK(stats.num_meshes_merged == 0 &&
        unchanged.stage.GetRootLayer()->prim_at_path("/Root/A/M"));
  options.max_aggregate_faces = 65536;
  // Failure during output planning is transactional.
  options.memory_budget_bytes = 1000;
  auto bounded = LoadUSDAFromString(source);
  const std::string before = WriteLayerToString(*bounded.stage.GetRootLayer());
  CHECK(!usdz::MergeMeshes(&bounded.stage, options, &stats, &warn, &error));
  CHECK(before == WriteLayerToString(*bounded.stage.GetRootLayer()));
  // Indexed face-varying pools define Catmull-Clark UV continuity. Keep shared
  // indices across neighboring faces instead of duplicating every face corner.
  std::string patch = R"USD(def Mesh "A" {
    point3f[] points=[(0,0,0),(1,0,0),(0,1,0),(1,1,0)]
    int[] faceVertexCounts=[3,3]
    int[] faceVertexIndices=[0,1,2,1,3,2]
    texCoord2f[] primvars:st=[(0,0),(1,0),(0,1),(1,1)] (interpolation="faceVarying")
    int[] primvars:st:indices=[0,1,2,1,3,2]
  })USD";
  std::string second = patch;
  second.replace(second.find("\"A\""), 3, "\"B\"");
  auto patches = LoadUSDAFromString("#usda 1.0\ndef Xform \"Root\" {" + patch +
                                    second + "}");
  CHECK(patches.success);
  options.memory_budget_bytes = size_t(512) << 20;
  CHECK(usdz::MergeMeshes(&patches.stage, options, &stats, &warn, &error));
  const PrimSpec* combined = patches.stage.GetRootLayer()->prim_at_path(
      "/Root/__LightUSD_MeshMerge/Merged_0");
  CHECK(combined && combined->property_value("primvars:st")->array_size() == 8);
  const auto shared =
      combined->property_value("primvars:st:indices")->as_int_array();
  CHECK(shared.size() == 12 && shared[1] == shared[3] &&
        shared[7] == shared[9] && shared[6] == 4);
  std::cout << "PASS: whole-mesh topology, transformed normals/primvars, "
               "subsets, limits and Crate output\n";
}
