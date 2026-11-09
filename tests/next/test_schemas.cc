/// Schema tests for the next library.
/// Tests detection functions (IsMesh, IsCamera, etc.) and data getters.

#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-point-instancer.hh"
#include "next/schema/geom-xform.hh"
#include "next/schema/color-space.hh"
#include "next/schema/usd-geom-camera.hh"
#include "next/schema/usd-lux.hh"
#include "next/schema/usd-shade.hh"
#include "next/layer/layer.hh"
#include "next/reader/usda-reader.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>
#include <cmath>

using namespace lightusd::next;

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { printf("  %s ... ", name); test_count++; } while(0)
#define PASS() do { pass_count++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static Stage MakeTestStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("Test");
  sb.SetUpAxis("Y");

  auto& layer = sb.GetLayerBuilder();

  // Xform
  layer.begin_prim("Test", "Xform");
  layer.add_property("visibility", Value::MakeToken("inherited"));
  layer.end_prim();

  // Mesh
  layer.begin_prim("TestMesh", "Mesh");
  std::vector<float> pts = {-1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
                            -1,-1, 1, 1,-1, 1, 1,1, 1, -1,1, 1};
  layer.add_property("points", Value::MakeFloat3Array(pts));
  layer.add_property("faceVertexCounts", Value::MakeIntArray({4,4,4,4,4,4}));
  layer.end_prim();

  // PointInstancer
  layer.begin_prim("Inst", "PointInstancer");
  layer.add_relationship("prototypes", Path("/TestMesh"));
  layer.add_property("protoIndices", Value::MakeIntArray({0, 0}));
  layer.add_property("positions", Value::MakeFloat3Array({0, 0, 0, 2, 0, 0}));
  layer.add_property("orientations",
                     Value::MakeFloatCompArray({1, 0, 0, 0, 1, 0, 0, 0},
                                               TypeId::Quatf, 4));
  layer.add_property("scales", Value::MakeFloat3Array({1, 1, 1, 2, 2, 2}));
  layer.add_property("velocities", Value::MakeFloat3Array({0, 0, 1, 0, 0, 2}));
  layer.add_property("angularVelocities",
                     Value::MakeFloat3Array({0, 1, 0, 0, 2, 0}));
  layer.add_property("ids", Value::MakeInt64Array({100, 101}));
  layer.add_property("invisibleIds", Value::MakeInt64Array({101}));
  layer.end_prim();

  // Camera
  layer.begin_prim("TestCam", "Camera");
  layer.add_property("focalLength", Value(50.0));
  layer.end_prim();

  // SphereLight
  layer.begin_prim("TestLight", "SphereLight");
  layer.add_property("intensity", Value(100.0f));
  layer.end_prim();

  // Material
  layer.begin_prim("TestMat", "Material");
  layer.end_prim();

  // Shader
  layer.begin_prim("TestShader", "Shader");
  layer.add_property("info:id", Value::MakeToken("UsdPreviewSurface"));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

void test_mesh_schema() {
  TEST("IsMesh");
  auto stage = MakeTestStage();
  auto prim = stage.GetPrimAtPath("/TestMesh");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsMesh(prim)) { FAIL("expected Mesh"); return; }
  UsdGeomMesh mesh(prim);
  assert(mesh.IsValid());
  assert(mesh.GetFaceCount() == 6);
  assert(mesh.GetPointCount() == 8);
  PASS();
}

// Regression: schema accessors on a prim missing the queried arrays must
// return empty/safe values (the interned PropNameId lookup pass uses
// GetPropNameTable().find() for some names, which yields an invalid id when
// the property was never registered; every accessor must no-op on that).
void test_mesh_schema_missing_arrays() {
  TEST("Mesh getters on missing arrays");
  {
    StageBuilder sb;
    sb.SetDefaultPrim("Test");
    auto& layer = sb.GetLayerBuilder();

    // A Mesh with topology but no points/normals/UVs/extent/subdivisionScheme.
    layer.begin_prim("BareMesh", "Mesh");
    layer.add_property("faceVertexCounts", Value::MakeIntArray({4, 4}));
    layer.add_property("faceVertexIndices", Value::MakeIntArray({0, 1, 2, 3, 4, 5, 6, 7}));
    layer.end_prim();

    layer.finalize();
    Stage stage = sb.Build();
    auto prim = stage.GetPrimAtPath("/BareMesh");
    if (!prim.IsValid()) { FAIL("prim not found"); return; }

    UsdGeomMesh mesh(prim);
    assert(mesh.IsValid());
    assert(mesh.GetFaceCount() == 2 && "faceVertexCounts must still resolve");
    assert(mesh.GetFaceVertexIndices().size() == 8);
    assert(mesh.GetPointCount() == 0 && "missing points -> 0");
    assert(mesh.GetPoints().empty());
    std::vector<float> xv, yv, zv;
    assert(!mesh.GetPoints(xv, yv, zv) && "missing points -> false");
    assert(mesh.GetNormals().empty());
    assert(!mesh.HasNormals());
    float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    assert(!mesh.GetExtent(mn, mx) && "missing extent -> false");
    assert(mesh.GetUVs().empty() && "missing primvars:st/uv -> empty");
    assert(!mesh.HasUVs());
    assert(mesh.GetUVIndices().empty());
    assert(mesh.GetSubdivisionScheme() == "catmullClark" &&
           "missing subdivisionScheme -> schema fallback");
    assert(mesh.GetPointsAtTimecode(0.0).empty());
    assert(!mesh.HasAnimatedPoints());
    assert(mesh.GetPointsTimeSamples().empty());
  }

  // A Mesh with an authored default point value still resolves through the
  // interned-id path.
  {
    StageBuilder sb;
    sb.SetDefaultPrim("Test");
    auto& layer = sb.GetLayerBuilder();
    layer.begin_prim("PointMesh", "Mesh");
    std::vector<float> pts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    layer.add_property("points", Value::MakeFloat3Array(pts));
    layer.end_prim();
    layer.finalize();
    Stage stage2 = sb.Build();
    auto p2 = stage2.GetPrimAtPath("/PointMesh");
    if (!p2.IsValid()) { FAIL("PointMesh not found"); return; }
    UsdGeomMesh mesh2(p2);
    assert(mesh2.GetPointCount() == 3 && "authored points must resolve");
    assert(mesh2.GetPoints().size() == 9);
  }

  PASS();
}

void test_xform_schema() {
  TEST("IsXform");
  auto stage = MakeTestStage();
  auto prim = stage.GetPrimAtPath("/Test");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsXform(prim)) { FAIL("expected Xform"); return; }
  PASS();
}

void test_point_instancer_schema() {
  TEST("IsPointInstancer");
  auto stage = MakeTestStage();
  auto prim = stage.GetPrimAtPath("/Inst");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsPointInstancer(prim)) { FAIL("expected PointInstancer"); return; }
  UsdGeomPointInstancer pi(prim);
  assert(pi.IsValid());
  assert(pi.GetPrototypes().size() == 1);
  assert(pi.GetPrototypes()[0].str() == "/TestMesh");
  assert(pi.GetInstanceCount() == 2);
  assert(pi.GetProtoIndices() == std::vector<int32_t>({0, 0}));
  assert(pi.GetPositions() == std::vector<float>({0, 0, 0, 2, 0, 0}));
  assert(pi.GetScales() == std::vector<float>({1, 1, 1, 2, 2, 2}));
  assert(pi.GetVelocities() == std::vector<float>({0, 0, 1, 0, 0, 2}));
  assert(pi.GetAngularVelocities() ==
         std::vector<float>({0, 1, 0, 0, 2, 0}));
  assert(pi.GetIds() == std::vector<int64_t>({100, 101}));
  assert(pi.GetInvisibleIds() == std::vector<int64_t>({101}));
  std::string reason;
  assert(pi.HasValidInstanceArrays(0.0, &reason));
  assert(reason.empty());
  std::vector<PointInstancerTransform> transforms = pi.ComputeInstanceTransforms();
  assert(transforms.size() == 2);
  assert(transforms[0].matrix[12] == 0.0);
  assert(transforms[1].matrix[0] == 2.0);
  assert(transforms[1].matrix[12] == 2.0);

  // ComputeMaskAtTime: instance 0 (id 100) visible, instance 1 (id 101) is in
  // invisibleIds. Use explicit FAIL (assert is compiled out under NDEBUG).
  std::vector<bool> mask = pi.ComputeMaskAtTime();
  if (mask.size() != 2) { FAIL("mask size"); return; }
  if (!mask[0] || mask[1]) { FAIL("invisibleId not masked"); return; }

  auto all = GetAllPointInstancers(stage);
  assert(all.size() == 1);

  StageBuilder bad_sb;
  auto& bad_layer = bad_sb.GetLayerBuilder();
  bad_layer.begin_prim("Proto", "Mesh");
  bad_layer.end_prim();
  bad_layer.begin_prim("BadInst", "PointInstancer");
  bad_layer.add_relationship("prototypes", Path("/Proto"));
  bad_layer.add_property("protoIndices", Value::MakeIntArray({0, 0}));
  bad_layer.add_property("positions", Value::MakeFloat3Array({0, 0, 0, 1, 0, 0}));
  bad_layer.add_property("velocities", Value::MakeFloat3Array({0, 0, 1}));
  bad_layer.end_prim();
  bad_layer.finalize();
  Stage bad_stage = bad_sb.Build();
  UsdGeomPointInstancer bad_pi(bad_stage.GetPrimAtPath("/BadInst"));
  assert(!bad_pi.HasValidInstanceArrays(0.0, &reason));
  assert(reason == "velocities size does not match protoIndices");
  PASS();
}

// Regression: PointInstancer getters previously read time samples HELD
// (GetValueAtTime only), so animated positions/scales snapped to the
// earlier key. They must linearly interpolate between samples like the
// tydra render-extract path.
void test_point_instancer_interpolation() {
  TEST("PointInstancerInterpolation");
  StageBuilder sb;
  auto& layer = sb.GetLayerBuilder();
  layer.begin_prim("Proto", "Mesh");
  layer.end_prim();
  layer.begin_prim("Anim", "PointInstancer");
  layer.add_relationship("prototypes", Path("/Proto"));
  layer.add_property("protoIndices", Value::MakeIntArray({0}));
  layer.add_time_sample("positions", 0.0,
                        Value::MakeFloat3Array({0, 0, 0}));
  layer.add_time_sample("positions", 10.0,
                        Value::MakeFloat3Array({4, 0, 0}));
  layer.add_time_sample("scales", 0.0, Value::MakeFloat3Array({1, 1, 1}));
  layer.add_time_sample("scales", 10.0, Value::MakeFloat3Array({3, 3, 3}));
  layer.end_prim();
  layer.finalize();
  Stage stage = sb.Build();
  UsdGeomPointInstancer pi(stage.GetPrimAtPath("/Anim"));
  if (!pi.IsValid()) { FAIL("prim not found"); return; }

  // Exact sample times.
  if (pi.GetPositions(0.0) != std::vector<float>({0, 0, 0})) {
    FAIL("positions at t=0");
    return;
  }
  if (pi.GetPositions(10.0) != std::vector<float>({4, 0, 0})) {
    FAIL("positions at t=10");
    return;
  }
  // Midpoint must interpolate, not snap to the earlier key.
  if (pi.GetPositions(5.0) != std::vector<float>({2, 0, 0})) {
    FAIL("positions at t=5 not interpolated");
    return;
  }
  if (pi.GetScales(5.0) != std::vector<float>({2, 2, 2})) {
    FAIL("scales at t=5 not interpolated");
    return;
  }
  // Interpolated transforms flow through ComputeInstanceTransforms too.
  std::vector<PointInstancerTransform> xf = pi.ComputeInstanceTransforms(5.0);
  if (xf.size() != 1 || xf[0].matrix[12] != 2.0 || xf[0].matrix[0] != 2.0) {
    FAIL("interpolated instance transform");
    return;
  }
  PASS();
}

// Regression: GetPointsAtTimecode / GetTranslationAtTimecode (formerly
// *AtTime) sampled held-only; they must interpolate between samples.
void test_mesh_xform_timecode_interpolation() {
  TEST("MeshXformTimecodeInterpolation");
  StageBuilder sb;
  auto& layer = sb.GetLayerBuilder();
  layer.begin_prim("AnimMesh", "Mesh");
  layer.add_time_sample("points", 0.0, Value::MakeFloat3Array({0, 0, 0}));
  layer.add_time_sample("points", 10.0, Value::MakeFloat3Array({4, 2, 0}));
  layer.end_prim();
  layer.begin_prim("AnimXf", "Xform");
  layer.add_property("xformOpOrder",
                     Value::MakeTokenArray({"xformOp:translate"}));
  layer.add_time_sample("xformOp:translate", 0.0,
                        Value::MakeFloat3(0, 0, 0));
  layer.add_time_sample("xformOp:translate", 10.0,
                        Value::MakeFloat3(4, 0, 8));
  layer.end_prim();
  layer.finalize();
  Stage stage = sb.Build();

  UsdGeomMesh mesh(stage.GetPrimAtPath("/AnimMesh"));
  if (!mesh.IsValid()) { FAIL("mesh prim not found"); return; }
  if (mesh.GetPointsAtTimecode(0.0) != std::vector<float>({0, 0, 0})) {
    FAIL("points at t=0");
    return;
  }
  if (mesh.GetPointsAtTimecode(5.0) != std::vector<float>({2, 1, 0})) {
    FAIL("points at t=5 not interpolated");
    return;
  }
  if (mesh.GetPointsAtTimecode(20.0) != std::vector<float>({4, 2, 0})) {
    FAIL("points past last sample not held");
    return;
  }

  UsdGeomXform xf(stage.GetPrimAtPath("/AnimXf"));
  if (!xf.IsValid()) { FAIL("xform prim not found"); return; }
  float x = -1, y = -1, z = -1;
  if (!xf.GetTranslationAtTimecode(5.0, &x, &y, &z)) {
    FAIL("translation read failed");
    return;
  }
  if (x != 2.0f || y != 0.0f || z != 4.0f) {
    FAIL("translation at t=5 not interpolated");
    return;
  }
  PASS();
}

void test_camera_schema() {
  TEST("IsCamera");
  auto stage = MakeTestStage();
  auto prim = stage.GetPrimAtPath("/TestCam");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsCamera(prim)) { FAIL("expected Camera"); return; }
  CameraData data;
  if (!GetCameraData(stage, prim, &data)) { FAIL("GetCameraData"); return; }
  PASS();
}

void test_light_schema() {
  TEST("IsLight");
  auto stage = MakeTestStage();
  auto prim = stage.GetPrimAtPath("/TestLight");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  auto lt = GetLightType(prim);
  if (lt != LightType::SphereLight) { FAIL("expected SphereLight"); return; }
  LightData data;
  if (!GetLightData(stage, prim, &data)) { FAIL("GetLightData"); return; }

  StageBuilder distant_builder;
  LayerBuilder& layer = distant_builder.GetLayerBuilder();
  layer.begin_prim("Sun", "DistantLight");
  layer.end_prim();
  layer.begin_prim("AuthoredSun", "DistantLight");
  layer.add_property("inputs:intensity", Value(12.0f));
  layer.end_prim();
  Stage distant_stage = distant_builder.Build();
  const UsdPrim sun = distant_stage.GetPrimAtPath("/Sun");
  DistantLightData sun_data;
  if (!GetDistantLightData(distant_stage, sun, &sun_data) ||
      std::abs(sun_data.intensity - 50000.0f) > 0.01f ||
      std::abs(GetLightIntensity(distant_stage, sun) - 50000.0f) > 0.01f) {
    FAIL("DistantLight schema intensity fallback"); return;
  }
  const UsdPrim authored = distant_stage.GetPrimAtPath("/AuthoredSun");
  if (std::abs(GetLightIntensity(distant_stage, authored) - 12.0f) > 0.01f) {
    FAIL("authored DistantLight intensity override"); return;
  }
  PASS();
}

void test_shade_schema() {
  TEST("IsMaterial");
  auto stage = MakeTestStage();
  auto mat = stage.GetPrimAtPath("/TestMat");
  if (!mat.IsValid()) { FAIL("mat not found"); return; }
  if (!IsMaterial(mat)) { FAIL("expected Material"); return; }

  auto sh = stage.GetPrimAtPath("/TestShader");
  if (!sh.IsValid()) { FAIL("shader not found"); return; }
  if (!IsShader(sh)) { FAIL("expected Shader"); return; }
  auto id = GetShaderId(sh);
  if (id != "UsdPreviewSurface") { FAIL("expected UsdPreviewSurface"); return; }
  PASS();
}

void test_material_binding_resolution() {
  TEST("material binding fallback/inheritance");
  StageBuilder sb;
  LayerBuilder& layer = sb.GetLayerBuilder();

  layer.begin_prim("Root", "Xform");
  layer.add_relationship("material:binding", Path("/ParentMat"));
  layer.add_relationship("material:binding:back", Path("/ParentBackMat"));
  {
    PropMeta& pm =
        layer.current()->ensure_property_meta("material:binding");
    pm.authored |= PropMeta::kBindMaterialAs;
    pm.bindMaterialAs = "strongerThanDescendants";
  }
  {
    PropMeta& pm =
        layer.current()->ensure_property_meta("material:binding:back");
    pm.authored |= PropMeta::kBindMaterialAs;
    pm.bindMaterialAs = "strongerThanDescendants";
  }
  layer.begin_prim("Mesh", "Mesh");
  layer.add_relationship("material:binding:preview", Path("/MissingMat"));
  layer.add_relationship("material:binding", Path("/LeafMat"));
  layer.add_relationship("material:binding:back", Path("/LeafBackMat"));
  layer.end_prim();
  layer.end_prim();

  layer.begin_prim("LooseMesh", "Mesh");
  layer.add_relationship("material:binding:preview", Path("/MissingMat"));
  layer.add_relationship("material:binding", Path("/LeafMat"));
  layer.add_relationship("material:binding:full", Path("/FullMat"));
  layer.add_relationship("material:binding:back", Path("/MissingBackMat"));
  layer.end_prim();
  layer.begin_prim("ParentMat", "Material");
  layer.end_prim();
  layer.begin_prim("LeafMat", "Material");
  layer.end_prim();
  layer.begin_prim("FullMat", "Material");
  layer.end_prim();
  layer.begin_prim("ParentBackMat", "Material");
  layer.end_prim();
  layer.begin_prim("LeafBackMat", "Material");
  layer.end_prim();
  layer.finalize();
  Stage stage = sb.Build();

  const UsdPrim mesh = stage.GetPrimAtPath("/Root/Mesh");
  const UsdPrim direct = GetBoundMaterial(stage, mesh);
  if (!direct.IsValid() || direct.GetPath().str() != "/LeafMat") {
    FAIL("dangling preview binding did not fall through");
    return;
  }
  if (GetInheritedBoundMaterialPath(stage, "/Root/Mesh") != "/ParentMat") {
    FAIL("strong ancestor binding did not override leaf binding");
    return;
  }
  if (GetInheritedBoundMaterialPath(stage, "/LooseMesh") != "/LeafMat") {
    FAIL("valid same-prim fallback binding was not selected");
    return;
  }
  if (GetInheritedBoundMaterialPathForPurpose(stage, "/LooseMesh", "full") !=
      "/FullMat") {
    FAIL("full-purpose binding did not outrank all-purpose/preview bindings");
    return;
  }
  if (GetInheritedBoundMaterialPathForPurpose(stage, "/Root/Mesh", "back") !=
      "/ParentBackMat") {
    FAIL("strong ancestor back-purpose binding did not override leaf binding");
    return;
  }
  if (!GetInheritedBoundMaterialPathForPurpose(stage, "/LooseMesh", "back")
           .empty()) {
    FAIL("missing back-purpose binding did not remain empty");
    return;
  }
  PASS();
}

void test_prim_children() {
  TEST("GetChildren");
  auto stage = MakeTestStage();
  auto root_prims = stage.GetRootPrims();
  // Should have at least 6 root prims
  if (root_prims.size() < 6) {
    FAIL("expected >= 6 root prims");
    return;
  }
  PASS();
}

void test_color_transform_numeric() {
  TEST("ColorTransformNumeric");
  using lightusd::color::BuildColorTransform;
  using lightusd::color::ColorSpaceDesc;
  using lightusd::color::ColorTransform;
  using lightusd::color::GetBuiltinColorSpace;

  ColorSpaceDesc srgb, linear, ap0, raw;
  if (!GetBuiltinColorSpace("sRGB", &srgb) ||
      !GetBuiltinColorSpace("lin_srgb", &linear) ||
      !GetBuiltinColorSpace("lin_ap0_scene", &ap0) ||
      !GetBuiltinColorSpace("raw", &raw)) {
    FAIL("builtin or alias lookup"); return;
  }
  if (srgb.name != "srgb_rec709_scene" ||
      linear.name != "lin_rec709_scene" ||
      !lightusd::color::IsLinear(linear) ||
      !lightusd::color::IsData(raw)) {
    FAIL("builtin classification"); return;
  }

  ColorTransform decode;
  if (!BuildColorTransform(srgb, linear, &decode)) {
    FAIL("sRGB decode transform"); return;
  }
  float samples[6] = {0.04045f, 0.5f, -0.5f, 1.0f, 0.0f, 0.25f};
  lightusd::color::TransformRGBSpan(decode, samples, 2);
  const float decoded[6] = {0.0031308f, 0.21404114f, -0.21404114f,
                            1.0f, 0.0f, 0.05087609f};
  for (int i = 0; i < 6; ++i) {
    if (std::abs(samples[i] - decoded[i]) > 2.0e-5f) {
      FAIL("sRGB decode numeric reference"); return;
    }
  }

  ColorTransform encode;
  if (!BuildColorTransform(linear, srgb, &encode)) {
    FAIL("sRGB encode transform"); return;
  }
  float rgba[8] = {samples[0], samples[1], samples[2], 0.125f,
                   samples[3], samples[4], samples[5], 0.75f};
  lightusd::color::TransformRGBASpan(encode, rgba, 2);
  const float roundtrip[8] = {0.04045f, 0.5f, -0.5f, 0.125f,
                              1.0f, 0.0f, 0.25f, 0.75f};
  for (int i = 0; i < 8; ++i) {
    if (std::abs(rgba[i] - roundtrip[i]) > 2.0e-5f) {
      FAIL("sRGB roundtrip or alpha preservation"); return;
    }
  }

  ColorTransform gamut;
  if (!BuildColorTransform(ap0, linear, &gamut)) {
    FAIL("AP0 gamut transform"); return;
  }
  const float expected_matrix[9] = {
      2.52168619f, -1.13413099f, -0.38755520f,
     -0.27647991f,  1.37271909f, -0.09623917f,
     -0.01537806f, -0.15297534f,  1.16835340f};
  for (int i = 0; i < 9; ++i) {
    if (std::abs(gamut.matrix[i] - expected_matrix[i]) > 3.0e-5f) {
      FAIL("AP0 to Rec.709 matrix reference"); return;
    }
  }
  float macbeth_dark_skin[3] = {0.11877f, 0.08709f, 0.05895f};
  lightusd::color::TransformRGB(gamut, macbeth_dark_skin);
  const float expected_patch[3] = {0.17788282f, 0.08103929f, 0.05372536f};
  for (int i = 0; i < 3; ++i) {
    if (std::abs(macbeth_dark_skin[i] - expected_patch[i]) > 3.0e-5f) {
      FAIL("Macbeth AP0 patch reference"); return;
    }
  }

  ColorTransform bypass;
  if (!BuildColorTransform(raw, linear, &bypass) || !bypass.bypass) {
    FAIL("raw bypass transform"); return;
  }
  float data[3] = {-2.0f, 0.5f, 4.0f};
  lightusd::color::TransformRGB(bypass, data);
  if (data[0] != -2.0f || data[1] != 0.5f || data[2] != 4.0f) {
    FAIL("raw values changed"); return;
  }

  const float degenerate[2] = {0.0f, 0.0f};
  const float white[2] = {0.3127f, 0.3290f};
  ColorSpaceDesc invalid;
  if (lightusd::color::MakeColorSpaceFromChromaticities(
          "invalid", degenerate, degenerate, degenerate, white, 1.0f, 0.0f,
          &invalid)) {
    FAIL("degenerate primaries accepted"); return;
  }
  PASS();
}

void test_color_management() {
  TEST("ColorManagement");
  const std::string usda = R"USD(#usda 1.0
(
    renderSettingsPrimPath = "/World/Render/settings"
)
def Scope "World" (
    prepend apiSchemas = ["ColorSpaceAPI", "ColorSpaceDefinitionAPI:studio",
        "ColorSpaceDefinitionAPI:studio_ap0",
        "ColorSpaceDefinitionAPI:dupA", "ColorSpaceDefinitionAPI:dupB"]
)
{
    uniform token colorSpace:name = "srgb_rec709_scene"
    uniform token colorSpaceDefinition:studio:name = "studio_linear"
    float2 colorSpaceDefinition:studio:redChroma = (0.64, 0.33)
    float2 colorSpaceDefinition:studio:greenChroma = (0.30, 0.60)
    float2 colorSpaceDefinition:studio:blueChroma = (0.15, 0.06)
    float2 colorSpaceDefinition:studio:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio:gamma = 1
    float colorSpaceDefinition:studio:linearBias = 0
    uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
    float2 colorSpaceDefinition:studio_ap0:redChroma = (0.7348552434, 0.2642253252)
    float2 colorSpaceDefinition:studio_ap0:greenChroma = (-0.0061709125, 1.0113149590)
    float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.0159675593, -0.0642355031)
    float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio_ap0:gamma = 1
    float colorSpaceDefinition:studio_ap0:linearBias = 0
    uniform token colorSpaceDefinition:dupA:name = "ambiguous"
    float2 colorSpaceDefinition:dupA:redChroma = (0.64, 0.33)
    float2 colorSpaceDefinition:dupA:greenChroma = (0.30, 0.60)
    float2 colorSpaceDefinition:dupA:blueChroma = (0.15, 0.06)
    float2 colorSpaceDefinition:dupA:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:dupA:gamma = 1
    float colorSpaceDefinition:dupA:linearBias = 0
    uniform token colorSpaceDefinition:dupB:name = "ambiguous"
    float2 colorSpaceDefinition:dupB:redChroma = (0.64, 0.33)
    float2 colorSpaceDefinition:dupB:greenChroma = (0.30, 0.60)
    float2 colorSpaceDefinition:dupB:blueChroma = (0.15, 0.06)
    float2 colorSpaceDefinition:dupB:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:dupB:gamma = 1
    float colorSpaceDefinition:dupB:linearBias = 0
    def Shader "Shader"
    {
        color3f inputs:baseColor = (0.5, 0.25, 0.75) (
            colorSpace = "lin_ap1_scene"
        )
    }
    def Scope "Render"
    {
        def RenderSettings "settings"
        {
            uniform token renderingColorSpace = "studio_linear"
        }
        def RenderSettings "override"
        {
            uniform token renderingColorSpace = "lin_rec2020_scene"
        }
        def RenderSettings "nonlinear"
        {
            uniform token renderingColorSpace = "srgb_rec709_scene"
        }
    }
}
def Scope "Legacy" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI"]
)
{
    uniform token name = "legacy_linear"
    float2 redChroma = (0.64, 0.33)
    float2 greenChroma = (0.30, 0.60)
    float2 blueChroma = (0.15, 0.06)
    float2 whitePoint = (0.3127, 0.3290)
    float gamma = 1
    float linearBias = 0
    def Shader "Shader" {}
}
)USD";
  LoadResult loaded = LoadUSDAFromString(usda);
  if (!loaded.success) { FAIL("parse"); return; }

  const UsdPrim shader = loaded.stage.GetPrimAtPath("/World/Shader");
  std::string source;
  bool authored = false;
  if (!color_management::ComputeColorSpaceName(
          shader, "inputs:baseColor", &source, &authored) ||
      !authored || source != "lin_ap1_scene") {
    FAIL("property colorSpace precedence"); return;
  }
  if (!color_management::ComputeColorSpaceName(shader, "missing", &source,
                                                &authored) ||
      !authored || source != "srgb_rec709_scene") {
    FAIL("inherited ColorSpaceAPI"); return;
  }

  color_management::RenderingColorConfig config;
  std::string warning;
  if (!color_management::ResolveRenderingColorConfig(
          loaded.stage, std::string(), &config, &warning) ||
      config.working_space != "studio_linear" || config.used_fallback ||
      !warning.empty()) {
    FAIL("RenderSettings working space"); return;
  }

  warning.clear();
  if (!color_management::ResolveRenderingColorConfig(
          loaded.stage, "/World/Render/override", &config, &warning) ||
      !config.used_override || config.used_fallback ||
      config.working_space != "lin_rec2020_scene" || !warning.empty()) {
    FAIL("RenderSettings override precedence"); return;
  }
  warning.clear();
  if (!color_management::ResolveRenderingColorConfig(
          loaded.stage, "/World/Render/nonlinear", &config, &warning) ||
      !config.used_override || !config.used_fallback ||
      config.working_space != "lin_rec709_scene" || warning.empty()) {
    FAIL("nonlinear working-space fallback"); return;
  }
  warning.clear();
  if (!color_management::ResolveRenderingColorConfig(
          loaded.stage, "/missing", &config, &warning) ||
      !config.used_fallback || config.working_space != "lin_rec709_scene" ||
      warning.empty()) {
    FAIL("invalid RenderSettings fallback"); return;
  }

  const UsdPrim legacy = loaded.stage.GetPrimAtPath("/Legacy/Shader");
  color_management::ColorSpaceDesc legacy_definition;
  std::string definition_error;
  if (!color_management::ResolveColorSpaceDefinition(
          legacy, "legacy_linear", &legacy_definition, &definition_error) ||
      !lightusd::color::IsLinear(legacy_definition) ||
      !definition_error.empty()) {
    FAIL("legacy ColorSpaceDefinitionAPI"); return;
  }

  if (!color_management::ComputeColorSpaceName(
          loaded.stage.GetPrimAtPath("/Legacy"), "missing", &source,
          &authored) || authored || source != "lin_rec709_scene") {
    FAIL("default color space"); return;
  }

  color_management::ColorTransform transform;
  std::string error;
  if (!color_management::BuildColorTransform(
          shader, "srgb_rec709_scene", "lin_rec709_scene", &transform,
          &error)) {
    FAIL("sRGB transform build"); return;
  }
  float linear[3] = {0.5f, 0.5f, 0.5f};
  ::lightusd::color::TransformRGB(transform, linear);
  if (std::abs(linear[0] - 0.214041f) > 1.0e-4f ||
      std::abs(linear[1] - linear[0]) > 1.0e-5f ||
      std::abs(linear[2] - linear[0]) > 1.0e-5f) {
    FAIL("sRGB numeric transform"); return;
  }

  color_management::ColorTransform custom_gamut;
  error.clear();
  if (!color_management::BuildColorTransform(
          shader, "studio_ap0", "lin_rec709_scene", &custom_gamut, &error)) {
    FAIL("custom wide-gamut transform build"); return;
  }
  float custom_patch[3] = {0.11877f, 0.08709f, 0.05895f};
  ::lightusd::color::TransformRGB(custom_gamut, custom_patch);
  const float expected_patch[3] = {0.17788282f, 0.08103929f, 0.05372536f};
  for (int i = 0; i < 3; ++i) {
    if (std::abs(custom_patch[i] - expected_patch[i]) > 4.0e-5f) {
      FAIL("custom AP0 definition numeric reference"); return;
    }
  }

  color_management::ColorSpaceDesc rejected;
  error.clear();
  if (color_management::ResolveColorSpaceDefinition(
          shader, "ambiguous", &rejected, &error) ||
      error.find("Ambiguous ColorSpaceDefinitionAPI") == std::string::npos) {
    FAIL("ambiguous custom definition diagnostic"); return;
  }
  error.clear();
  if (color_management::ResolveColorSpaceDefinition(
          shader, "missing_custom_space", &rejected, &error) ||
      error.find("Unknown color space") == std::string::npos) {
    FAIL("unknown color-space diagnostic"); return;
  }
  PASS();
}

int main() {
  printf("Schema Tests\n");
  printf("============\n\n");

  test_mesh_schema();
  test_mesh_schema_missing_arrays();
  test_point_instancer_schema();
  test_point_instancer_interpolation();
  test_mesh_xform_timecode_interpolation();
  test_xform_schema();
  test_camera_schema();
  test_light_schema();
  test_shade_schema();
  test_material_binding_resolution();
  test_prim_children();
  test_color_transform_numeric();
  test_color_management();

  printf("\n%d/%d tests passed\n", pass_count, test_count);
  return pass_count == test_count ? 0 : 1;
}
