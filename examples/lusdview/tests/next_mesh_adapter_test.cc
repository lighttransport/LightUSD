// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "lightusd-cpp.hh"
#include "next_mesh_adapter.hh"
#include "prototype_expansion.hh"
#include "tydra/next/render-converter.hh"

namespace tydn = lightusd::tydra::next;

int main() {
  const size_t maximum = std::numeric_limits<size_t>::max();
  assert(lusdview::BoundedInstanceProduct(maximum, 2, 17) == 17);
  assert(lusdview::BoundedInstanceProduct(2, maximum, maximum) == maximum);
  assert(lusdview::BoundedInstanceProduct(3, 4, 20) == 12);
  assert(lusdview::BoundedInstanceProduct(maximum, 0, maximum) == 0);
  assert(lusdview::BoundedInstanceProduct(3, 4, 0) == 0);
  std::vector<std::string> active;
  {
    lusdview::PrototypeExpansionGuard root(active, "/Root");
    assert(root.entered());
    {
      lusdview::PrototypeExpansionGuard child(active, "/Shared");
      lusdview::PrototypeExpansionGuard cycle(active, "/Root");
      assert(child.entered() && !cycle.entered());
      assert(active.size() == 2);
    }
    // A sibling may expand the shared prototype after the first branch exits.
    lusdview::PrototypeExpansionGuard sibling(active, "/Shared");
    assert(sibling.entered());
  }
  assert(active.empty());
  active.resize(lusdview::PrototypeExpansionGuard::kMaxDepth, "/Parent");
  {
    lusdview::PrototypeExpansionGuard overDepth(active, "/Leaf");
    assert(!overDepth.entered());
  }
  assert(active.size() == lusdview::PrototypeExpansionGuard::kMaxDepth);
  const char source[] = R"(#usda 1.0
def Mesh "FaceUVs" {
 point3f[] points = [(10,20,30),(14,20,30),(14,26,30),(10,26,32)]
 int[] faceVertexCounts = [3,3]
 int[] faceVertexIndices = [0,1,2,0,2,3]
 uniform token subdivisionScheme = "none"
 bool doubleSided = true
 texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0.1,0.2),(0.9,0.8),(0,1)] (interpolation = "faceVarying")
 texCoord2f[] primvars:st1 = [(0.2,0.3),(0.4,0.5),(0.6,0.7),(0.8,0.9)] (interpolation = "vertex")
 float[] primvars:temperature = [10,20,30,40] (interpolation = "vertex")
 float3[] primvars:rest = [(1,2,3),(4,5,6),(7,8,9),(10,11,12)] (interpolation = "vertex")
 int[] primvars:tag = [7,9] (interpolation = "vertex")
 int[] primvars:tag:indices = [0,1,1,0]
}
def Mesh "ConstantUVs" {
 point3f[] points = [(-2,-3,-4),(2,-3,-4),(2,3,-4),(-2,3,-4)]
 int[] faceVertexCounts = [4]
 int[] faceVertexIndices = [0,1,2,3]
 uniform token subdivisionScheme = "none"
 texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
 texCoord2f[] primvars:st1 = [(0.75,0.5)] (interpolation = "constant")
}
def Mesh "Sanitized" {
 point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
 int[] faceVertexCounts = [3,3,3]
 int[] faceVertexIndices = [0,1,99,0,1,2,0,2,3]
 uniform token subdivisionScheme = "none"
}
)";
  lightusd_stage* rawStage = nullptr;
  assert(lightusd_stage_load_from_memory(
             reinterpret_cast<const uint8_t*>(source), std::strlen(source),
             nullptr, &rawStage) == LIGHTUSD_OK);
  lightusd::api::Stage stage(rawStage);
  tydn::ConverterConfig config;
  config.material.load_textures = false;
  tydn::RenderMesh face, constant;
  assert(lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/FaceUVs", config, &face));
  assert(lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/ConstantUVs", config, &constant));

  // Nonzero, asymmetric bounds catch a missing copy even when the final static
  // draw batch happens to recompute its own box. Skinned/streamed batches use
  // these provisional bounds before the final union is available.
  assert(face.has_bbox);
  assert(face.bbox_min.x == 10 && face.bbox_min.y == 20 && face.bbox_min.z == 30);
  assert(face.bbox_max.x == 14 && face.bbox_max.y == 26 && face.bbox_max.z == 32);
  assert(face.double_sided && face.point_count() == 4 && face.face_count() == 2);
  assert(face.texcoords_0_name == "st" && face.texcoords_1_name == "st1");
  assert(face.texcoords_0_interp == tydn::Interpolation::FaceVarying);
  assert(face.texcoords_1_interp == tydn::Interpolation::Vertex);
  assert(constant.texcoords_0_interp == tydn::Interpolation::Vertex);
  assert(constant.texcoords_1_interp == tydn::Interpolation::Constant);
  assert(face.texcoords_0.size() == 12 && face.texcoords_1.size() == 8);
  assert(constant.texcoords_1.size() == 2 && constant.texcoords_1[0] == 0.75f);
  assert(face.triangulated_indices.size() == 6);
  assert(face.triangulated_face_vertex_indices.size() == 6);
  assert(face.face_triangle_offsets.size() == 3);
  assert(face.face_triangle_offsets.back() == 2);
  assert(face.primvars.size() == 3);
  for (const auto& pv : face.primvars) {
    if (pv.name == "temperature") {
      assert(pv.format == tydn::VertexFormat::Float);
      assert(pv.float_data.size() == 4 && pv.float_data[3] == 40);
    } else if (pv.name == "rest") {
      assert(pv.format == tydn::VertexFormat::Vec3);
      assert(pv.float_data.size() == 12 && pv.float_data[11] == 12);
    } else {
      assert(pv.name == "tag" && pv.format == tydn::VertexFormat::Int);
      assert(pv.int_data.size() == 2 && pv.int_data[1] == 9);
      assert(pv.indices.size() == 4 && pv.indices[1] == 1);
    }
  }

  tydn::RenderMesh proxy;
  assert(lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/FaceUVs", config, &proxy, 2));
  assert(proxy.has_bbox && proxy.bbox_min.x == -1 && proxy.bbox_max.z == 1);
  assert(proxy.triangulated_indices.size() == 36);

  tydn::RenderMesh sanitized;
  assert(lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/Sanitized", config, &sanitized));
  assert(sanitized.face_count() == 2 && sanitized.triangulated_indices.size() == 6);
  assert(sanitized.sanitize_dropped_faces == 1);
  assert((sanitized.sanitize_face_remap == std::vector<int32_t>{-1, 0, 1}));

  tydn::RenderMesh unchanged;
  unchanged.name = "sentinel";
  assert(!lusdview::ConvertMeshThroughPublicAPI(nullptr, "/FaceUVs", config, &unchanged));
  assert(!lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/Missing", config, &unchanged));
  assert(!lusdview::ConvertMeshThroughPublicAPI(stage.get(), "/FaceUVs", config, nullptr));
  assert(unchanged.name == "sentinel");
  stage = lightusd::api::Stage();
  // The temporary public render records and source stage are both gone.
  assert(face.points[0] == 10 && face.texcoords_1[0] == 0.2f);
  assert(constant.points[0] == -2 && constant.texcoords_1[0] == 0.75f);
}
