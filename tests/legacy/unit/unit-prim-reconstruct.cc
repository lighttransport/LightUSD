// SPDX-License-Identifier: Apache 2.0
// Prim reconstruction unit tests: parse USDA via LoadUSDAFromMemory, then
// verify that the Stage contains correctly-typed Prim objects with the
// expected properties.  This exercises the full pipeline:
//   ASCII parser -> PrimSpec -> prim-reconstruct -> Stage

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-prim-reconstruct.h"
#include "lightusd.hh"
#include "core/prim.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdSkel.hh"
#include "xform.hh"
#include "math-util.inc"

#include <cstring>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// ---------------------------------------------------------------------------
// Helper: parse a USDA string into a Stage
// ---------------------------------------------------------------------------
namespace {

static bool parse_usda(const char *usda, Stage *stage, std::string *warn,
                       std::string *err) {
  return LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda), "test.usda",
      stage, warn, err);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// 1. Xform reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_xform_test(void) {
  // Single translate xformOp
  {
    const char *usda = R"(#usda 1.0

def Xform "root" {
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";
    Stage stage;
    std::string warn, err;
    bool ok = parse_usda(usda, &stage, &warn, &err);
    if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
    TEST_CHECK(ok);

    auto result = stage.GetPrimAtPath(Path("/root", ""));
    TEST_CHECK(bool(result));
    if (!result) return;

    const Prim *prim = *result;
    TEST_CHECK(prim != nullptr);
    TEST_CHECK(prim->is<Xform>());

    const auto *xform = prim->as<Xform>();
    TEST_CHECK(xform != nullptr);
    if (!xform) return;

    TEST_CHECK(xform->xformOps.size() == 1);
    if (xform->xformOps.size() == 1) {
      TEST_CHECK(xform->xformOps[0].op_type == XformOp::OpType::Translate);
    }

    // Evaluate the transform matrix
    bool resetStack = false;
    auto mat = xform->GetLocalMatrix(lightusd::legacy::value::TimeCode::Default(),
                                     lightusd::legacy::value::TimeSampleInterpolationType::Linear,
                                     &resetStack);
    TEST_CHECK(bool(mat));
    if (mat) {
      // Translation should be (1, 2, 3) -> column 3 of the matrix
      // row-major: m[3][0], m[3][1], m[3][2]
      lightusd::legacy::value::matrix4d m = mat.value();
      TEST_CHECK(math::is_close(m.m[3][0], 1.0));
      TEST_CHECK(math::is_close(m.m[3][1], 2.0));
      TEST_CHECK(math::is_close(m.m[3][2], 3.0));
    }
  }

  // Multiple xformOps: translate + rotateXYZ + scale
  {
    const char *usda = R"(#usda 1.0

def Xform "multi" {
    double3 xformOp:translate = (10, 0, 0)
    double3 xformOp:scale = (2, 2, 2)
    float3 xformOp:rotateXYZ = (0, 90, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]
}
)";
    Stage stage;
    std::string warn, err;
    bool ok = parse_usda(usda, &stage, &warn, &err);
    if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
    TEST_CHECK(ok);

    auto result = stage.GetPrimAtPath(Path("/multi", ""));
    TEST_CHECK(bool(result));
    if (!result) return;

    const Xform *xform = (*result)->as<Xform>();
    TEST_CHECK(xform != nullptr);
    if (!xform) return;

    TEST_CHECK(xform->xformOps.size() == 3);
  }
}

// ---------------------------------------------------------------------------
// 2. GeomMesh reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_mesh_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "cube" {
    point3f[] points = [
        (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
        (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)
    ]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [
        0, 1, 2, 3,
        4, 7, 6, 5,
        0, 4, 5, 1,
        1, 5, 6, 2,
        2, 6, 7, 3,
        3, 7, 4, 0
    ]
    normal3f[] normals = [
        (0, 0, -1), (0, 0, -1), (0, 0, -1), (0, 0, -1),
        (0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1),
        (0, -1, 0), (0, -1, 0), (0, -1, 0), (0, -1, 0),
        (1, 0, 0), (1, 0, 0), (1, 0, 0), (1, 0, 0),
        (0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0),
        (-1, 0, 0), (-1, 0, 0), (-1, 0, 0), (-1, 0, 0)
    ]
    uniform token subdivisionScheme = "none"
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/cube", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<GeomMesh>());

  const GeomMesh *mesh = prim->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  // Check points
  auto pts = mesh->get_points();
  TEST_CHECK(pts.size() == 8);
  if (pts.size() == 8) {
    TEST_CHECK(math::is_close(pts[0].x, -1.0f));
    TEST_CHECK(math::is_close(pts[0].y, -1.0f));
    TEST_CHECK(math::is_close(pts[0].z, -1.0f));
    TEST_CHECK(math::is_close(pts[6].x, 1.0f));
    TEST_CHECK(math::is_close(pts[6].y, 1.0f));
    TEST_CHECK(math::is_close(pts[6].z, 1.0f));
  }

  // Check face vertex counts (6 quads)
  auto fvc = mesh->get_faceVertexCounts();
  TEST_CHECK(fvc.size() == 6);
  for (size_t i = 0; i < fvc.size(); i++) {
    TEST_CHECK(fvc[i] == 4);
  }

  // Check face vertex indices (6 * 4 = 24 indices)
  auto fvi = mesh->get_faceVertexIndices();
  TEST_CHECK(fvi.size() == 24);

  // Check normals
  auto norms = mesh->get_normals();
  TEST_CHECK(norms.size() == 24);

  // Check subdivision scheme
  TEST_CHECK(mesh->subdivisionScheme.get_value() ==
             GeomMesh::SubdivisionScheme::SubdivisionSchemeNone);
}

// ---------------------------------------------------------------------------
// 3. Material + Shader (UsdPreviewSurface) reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_material_shader_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat" {
    token outputs:surface.connect = </mat/surf.outputs:surface>

    def Shader "surf" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
        float inputs:metallic = 1.0
        float inputs:roughness = 0.3
        token outputs:surface
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // Verify Material prim
  auto mat_result = stage.GetPrimAtPath(Path("/mat", ""));
  TEST_CHECK(bool(mat_result));
  if (!mat_result) return;

  const Prim *mat_prim = *mat_result;
  TEST_CHECK(mat_prim->is<Material>());

  const Material *material = mat_prim->as<Material>();
  TEST_CHECK(material != nullptr);
  if (!material) return;

  // Material should have a surface output connection
  TEST_CHECK(material->surface.authored());
  if (material->surface.authored()) {
    auto paths = material->surface.get_connections();
    TEST_CHECK(paths.size() == 1);
  }

  // Verify Shader child prim
  TEST_CHECK(mat_prim->children().size() >= 1);
  if (mat_prim->children().empty()) return;

  const Prim &shader_prim = mat_prim->children()[0];
  TEST_CHECK(shader_prim.is<Shader>());

  const Shader *shader = shader_prim.as<Shader>();
  TEST_CHECK(shader != nullptr);
  if (!shader) return;

  TEST_CHECK(shader->info_id == "UsdPreviewSurface");

  // The concrete shader node is UsdPreviewSurface
  const auto *pbs = shader->value.as<UsdPreviewSurface>();
  TEST_CHECK(pbs != nullptr);
  if (!pbs) return;

  // Check diffuseColor
  {
    lightusd::legacy::value::color3f dc;
    bool got = pbs->diffuseColor.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &dc);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(dc[0], 0.8f));
      TEST_CHECK(math::is_close(dc[1], 0.2f));
      TEST_CHECK(math::is_close(dc[2], 0.1f));
    }
  }

  // Check metallic
  {
    float m;
    bool got = pbs->metallic.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &m);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(m, 1.0f));
    }
  }

  // Check roughness
  {
    float r;
    bool got = pbs->roughness.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &r);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(r, 0.3f));
    }
  }
}

// ---------------------------------------------------------------------------
// 4. SphereLight reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_sphere_light_test(void) {
  const char *usda = R"(#usda 1.0

def SphereLight "light1" {
    float inputs:intensity = 500
    color3f inputs:color = (1, 0.9, 0.8)
    float inputs:radius = 2.5
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/light1", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<SphereLight>());

  const SphereLight *light = prim->as<SphereLight>();
  TEST_CHECK(light != nullptr);
  if (!light) return;

  // Check intensity
  {
    float val;
    bool got = light->intensity.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &val);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(val, 500.0f));
    }
  }

  // Check color
  {
    lightusd::legacy::value::color3f col;
    bool got = light->color.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &col);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(col[0], 1.0f));
      TEST_CHECK(math::is_close(col[1], 0.9f));
      TEST_CHECK(math::is_close(col[2], 0.8f));
    }
  }

  // Check radius
  {
    float val;
    bool got = light->radius.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &val);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(val, 2.5f));
    }
  }
}

// ---------------------------------------------------------------------------
// 5. Skeleton reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_skeleton_test(void) {
  const char *usda = R"(#usda 1.0

def Skeleton "skel" {
    uniform token[] joints = ["Root", "Root/Spine", "Root/Spine/Head"]
    uniform matrix4d[] bindTransforms = [
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) ),
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 2, 0, 1) )
    ]
    uniform matrix4d[] restTransforms = [
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) ),
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) )
    ]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/skel", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<Skeleton>());

  const Skeleton *skel = prim->as<Skeleton>();
  TEST_CHECK(skel != nullptr);
  if (!skel) return;

  // Check joints
  TEST_CHECK(skel->joints.has_value());
  if (skel->joints.has_value()) {
    std::vector<lightusd::legacy::value::token> j;
    skel->joints.get_value(&j);
    TEST_CHECK(j.size() == 3);
    if (j.size() == 3) {
      TEST_CHECK(j[0].str() == "Root");
      TEST_CHECK(j[1].str() == "Root/Spine");
      TEST_CHECK(j[2].str() == "Root/Spine/Head");
    }
  }

  // Check bindTransforms
  TEST_CHECK(skel->bindTransforms.has_value());
  if (skel->bindTransforms.has_value()) {
    std::vector<lightusd::legacy::value::matrix4d> bt;
    skel->bindTransforms.get_value(&bt);
    TEST_CHECK(bt.size() == 3);
    if (bt.size() == 3) {
      // Second bind transform should have translation (0, 1, 0)
      TEST_CHECK(math::is_close(bt[1].m[3][1], 1.0));
      // Third bind transform should have translation (0, 2, 0)
      TEST_CHECK(math::is_close(bt[2].m[3][1], 2.0));
    }
  }

  // Check restTransforms
  TEST_CHECK(skel->restTransforms.has_value());
  if (skel->restTransforms.has_value()) {
    std::vector<lightusd::legacy::value::matrix4d> rt;
    skel->restTransforms.get_value(&rt);
    TEST_CHECK(rt.size() == 3);
  }
}

// ---------------------------------------------------------------------------
// 6. Camera reconstruction
// ---------------------------------------------------------------------------
void prim_reconstruct_camera_test(void) {
  const char *usda = R"(#usda 1.0

def Camera "cam" {
    float focalLength = 50
    float horizontalAperture = 36
    float verticalAperture = 24
    token projection = "perspective"
    float2 clippingRange = (0.1, 10000)
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/cam", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<GeomCamera>());

  const GeomCamera *cam = prim->as<GeomCamera>();
  TEST_CHECK(cam != nullptr);
  if (!cam) return;

  // Check focalLength
  {
    float val;
    bool got = cam->focalLength.get_value().get(
        lightusd::legacy::value::TimeCode::Default(), &val);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(math::is_close(val, 50.0f));
    }
  }

  // Check projection (use get_scalar since enum types don't have lerp)
  {
    GeomCamera::Projection proj;
    bool got = cam->projection.get_value().get_scalar(&proj);
    TEST_CHECK(got);
    if (got) {
      TEST_CHECK(proj == GeomCamera::Projection::Perspective);
    }
  }
}

// ---------------------------------------------------------------------------
// 7. Nested hierarchy: Xform with Mesh child
// ---------------------------------------------------------------------------
void prim_reconstruct_nested_hierarchy_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "world" {
    double3 xformOp:translate = (0, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Mesh "plane" {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
    }

    def Xform "group" {
        def SphereLight "light" {
            float inputs:intensity = 100
        }
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // Verify root Xform
  {
    auto result = stage.GetPrimAtPath(Path("/world", ""));
    TEST_CHECK(bool(result));
    if (!result) return;
    TEST_CHECK((*result)->is<Xform>());
    // Should have 2 children: plane and group
    TEST_CHECK((*result)->children().size() == 2);
  }

  // Verify nested Mesh
  {
    auto result = stage.GetPrimAtPath(Path("/world/plane", ""));
    TEST_CHECK(bool(result));
    if (!result) return;
    TEST_CHECK((*result)->is<GeomMesh>());

    const GeomMesh *mesh = (*result)->as<GeomMesh>();
    TEST_CHECK(mesh != nullptr);
    if (mesh) {
      auto pts = mesh->get_points();
      TEST_CHECK(pts.size() == 4);
    }
  }

  // Verify deeply nested SphereLight
  {
    auto result = stage.GetPrimAtPath(Path("/world/group/light", ""));
    TEST_CHECK(bool(result));
    if (!result) return;
    TEST_CHECK((*result)->is<SphereLight>());

    const SphereLight *light = (*result)->as<SphereLight>();
    TEST_CHECK(light != nullptr);
    if (light) {
      float val;
      bool got = light->intensity.get_value().get(
          lightusd::legacy::value::TimeCode::Default(), &val);
      TEST_CHECK(got);
      if (got) {
        TEST_CHECK(math::is_close(val, 100.0f));
      }
    }
  }
}
