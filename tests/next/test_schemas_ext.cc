/// Extended schema tests for the next library.
/// Tests all 11 committed schema modules: Skel, AR, Mtlx, Media, Physics.

#include "next/schema/usd-skel.hh"
#include "next/schema/usd-ar.hh"
#include "next/schema/usd-mtlx.hh"
#include "next/schema/usd-media.hh"
#include "next/schema/usd-vol.hh"
#include "next/schema/usd-geom-camera.hh"
#include "next/schema/usd-semantics.hh"
#include "next/schema/usd-render.hh"
#include "next/schema/physics-scene.hh"
#include "next/schema/physics-api.hh"
#include "next/schema/physics-joint.hh"
#include "next/schema/physics-collision.hh"
#include "next/schema/schema-registry.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/usd-geom-model.hh"
#include "next/prim/path.hh"
#include "next/types/value-view.hh"
#include "next/types/value.hh"
#include "next/types/type-id.hh"
#include "next/layer/layer.hh"
#include "next/stage/stage.hh"
#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/load-usd.hh"
#include "next/writer/usdc-writer.hh"
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

using namespace lightusd::next;

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { printf("  %s ... ", name); test_count++; } while(0)
#define PASS() do { pass_count++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_connected_shader_constant() {
  TEST("connected MaterialX constant shader value");
  const char* text = R"USD(#usda 1.0
def Material "Mat" {
  token outputs:volume.connect = </Mat/Volume.outputs:volume>
  def Shader "Color" {
    uniform token info:id = "ND_constant_color3"
    color3f inputs:value = (0.2, 0.4, 0.8)
    color3f outputs:out
  }
  def Shader "Scale" {
    uniform token info:id = "ND_constant_float"
    float inputs:value = 0.5
    float outputs:out
  }
  def Shader "Multiply" {
    uniform token info:id = "ND_multiply_color3FA"
    color3f inputs:in1.connect = </Mat/Color.outputs:out>
    float inputs:in2.connect = </Mat/Scale.outputs:out>
    color3f outputs:out
  }
  def Shader "Volume" {
    uniform token info:id = "ND_standard_volume_volume"
    color3f inputs:emission_color.connect = </Mat/Multiply.outputs:out>
    token outputs:volume
  }
}
)USD";
  Stage stage;
  std::string warn, err;
  if (!LoadUSDFromMemory(reinterpret_cast<const uint8_t*>(text),
                         std::strlen(text), &stage, &warn, &err)) {
    FAIL(err.c_str());
    return;
  }
  const UsdPrim shader = stage.GetPrimAtPath("/Mat/Volume");
  Value value;
  if (!ResolveShaderPortValue(stage, shader, "inputs:emission_color", &value)) {
    FAIL("connection did not resolve");
    return;
  }
  const float* color = value.as_float3();
  if (!color || color[0] != 0.1f || color[1] != 0.2f || color[2] != 0.4f) {
    FAIL("resolved constant has wrong value");
    return;
  }
  PASS();
}

static void test_generated_supported_schema_fixture() {
  TEST("generated OpenUSD supported-schema USDA/USDC coverage");
  const char* paths[] = {
      "../../tests/usda/generated/openusd-supported-schema-26.08.usda",
      "tests/usda/generated/openusd-supported-schema-26.08.usda",
      "../../../tests/usda/generated/openusd-supported-schema-26.08.usda",
      LIGHTUSD_TEST_REPO_ROOT
          "/tests/usda/generated/openusd-supported-schema-26.08.usda"};
  std::string text;
  for (const char* path : paths) {
    std::ifstream stream(path);
    if (!stream.good()) continue;
    text.assign(std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
    break;
  }
  if (text.empty()) { FAIL("fixture not found"); return; }
  LoadResult loaded = LoadUSDAFromString(text.data(), text.size());
  if (!loaded.success) { FAIL("USDA fixture did not parse"); return; }
  size_t prim_count = 0;
  bool markers_ok = true;
  loaded.stage.Traverse([&](const UsdPrim& prim) {
    ++prim_count;
    const Value* marker = prim.GetPropertyValue("parity:schema");
    markers_ok = markers_ok && marker && marker->as_string() &&
                 !marker->as_string()->empty() &&
                 GetSchemaRegistry().IsKnownSchema(*marker->as_string());
    return true;
  });
  if (prim_count != 92 || !markers_ok) {
    FAIL("fixture schema markers/count mismatch"); return;
  }
  std::vector<uint8_t> crate;
  USDCWriteResult written = WriteUSDCToMemory(crate, loaded.stage);
  if (!written.success) { FAIL("fixture did not write to USDC"); return; }
  USDCLoadResult reread = LoadUSDCFromMemory(crate.data(), crate.size());
  if (!reread.success) { FAIL("fixture USDC did not reload"); return; }
  size_t reread_count = 0;
  bool reread_markers = true;
  reread.stage.Traverse([&](const UsdPrim& prim) {
    ++reread_count;
    const Value* marker = prim.GetPropertyValue("parity:schema");
    reread_markers = reread_markers && marker && marker->as_string();
    return true;
  });
  if (reread_count != 92 || !reread_markers) {
    FAIL("USDC schema coverage roundtrip lost authored data"); return;
  }
  PASS();
}

// ============================================================
// Test stage builders
// ============================================================

static Stage MakeSkelStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("SkelTest");
  auto& layer = sb.GetLayerBuilder();

  layer.begin_prim("Root", "SkelRoot");
  layer.add_property("skel:skeleton", Value::MakeAssetPath("/Root/Skel"));
  layer.add_property("skel:animationSource", Value::MakeAssetPath("/Root/Anim"));
  layer.end_prim();

  layer.begin_prim("Skel", "Skeleton");
  std::vector<std::string> joints = {"Hip", "Hip/Spine", "Hip/Spine/Neck"};
  layer.add_property("primvars:skel:joints", Value::MakeTokenArray(joints));
  layer.add_property("primvars:skel:bindTransforms", Value::MakeFloatArray({1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}));
  layer.add_property("primvars:skel:restTransforms", Value::MakeFloatArray({1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}));
  layer.end_prim();

  layer.begin_prim("Anim", "SkelAnimation");
  layer.add_property("translations", Value::MakeFloat3Array({0,0,0, 0,10,0, 0,20,0}));
  layer.add_property("rotations", Value::MakeFloatArray({1,0,0,0, 1,0,0,0, 1,0,0,0}));
  layer.end_prim();

  layer.begin_prim("Blend", "BlendShape");
  layer.add_property("offsets", Value::MakeFloat3Array({0,0,0, 0.5,0,0}));
  layer.add_property("pointIndices", Value::MakeIntArray({0, 1}));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

static Stage MakeARStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("ARTest");
  auto& layer = sb.GetLayerBuilder();

  layer.begin_prim("Anchor", "ARAnchor");
  layer.add_property("ar:location", Value::MakeFloat3Array({1,2,3}));
  layer.end_prim();

  layer.begin_prim("Image", "ARImage");
  layer.add_property("ar:imageAsset", Value::MakeAssetPath("image.png"));
  layer.add_property("ar:physicalWidth", Value(0.5f));
  layer.add_property("ar:physicalHeight", Value(0.3f));
  layer.end_prim();

  layer.begin_prim("Face", "ARFaceGeometry");
  layer.add_property("ar:blendShapeCoefficients", Value::MakeFloatArray({0, 0.5}));
  layer.end_prim();

  layer.begin_prim("Plane", "ARPlane");
  layer.add_property("ar:semanticType", Value::MakeToken("Horizontal"));
  layer.add_property("ar:extent", Value::MakeFloatArray({1, 2}));
  layer.end_prim();

  layer.begin_prim("Cloud", "ARPointCloud");
  layer.add_property("ar:points", Value::MakeFloat3Array({0,0,0, 1,1,1}));
  layer.end_prim();

  layer.begin_prim("Device", "ARDevice");
  layer.add_property("ar:fieldOfView", Value(60.0f));
  layer.add_property("ar:aspectRatio", Value(1.5f));
  layer.end_prim();

  layer.begin_prim("Scene", "ARScene");
  layer.end_prim();

  // AR API schemas on a regular prim
  layer.begin_prim("WithAnchorAPI", "Xform");
  auto* spec = layer.current();
  spec->meta().apiSchemas().push_back("ARAnchorAPI");
  spec->meta().apiSchemas().push_back("ARImagingAPI");
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

static Stage MakeMtlxStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("MtlxTest");
  auto& layer = sb.GetLayerBuilder();

  layer.begin_prim("Shader", "MaterialXShader");
  layer.add_property("inputs:nodeUri", Value("uri:test"));
  layer.add_property("inputs:nodeDefId", Value("ND_StandardSurface"));
  layer.add_property("outputs:mtlx:output", Value("out"));
  layer.end_prim();

  layer.begin_prim("NodeGraph", "MaterialXNodeGraph");
  layer.add_property("strings:mtlx:docUri", Value("doc:test"));
  layer.end_prim();

  layer.begin_prim("LightNode", "MaterialXLightNode");
  layer.add_property("strings:mtlx:nodeUri", Value("uri:light"));
  layer.add_property("inputs:mtlx:color", Value::MakeColor3f(1,1,1));
  layer.end_prim();

  layer.begin_prim("NodeIO", "MaterialXNodeIO");
  layer.add_property("strings:mtlx:nodeUri", Value("uri:io"));
  layer.add_property("outputs:mtlx:output", Value("out"));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

static Stage MakeMediaStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("MediaTest");
  auto& layer = sb.GetLayerBuilder();

  layer.begin_prim("Audio", "MediaAudio");
  layer.add_property("media:filePath", Value::MakeAssetPath("sound.wav"));
  layer.add_property("media:gain", Value(0.8f));
  layer.add_property("media:loop", Value(true));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

static Stage MakePhysicsStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("PhysicsTest");
  auto& layer = sb.GetLayerBuilder();

  // PhysicsScene
  layer.begin_prim("Scene", "PhysicsScene");
  layer.add_property("physics:gravityDirection", Value::MakeFloat3(0, -1, 0));
  layer.add_property("physics:gravityMagnitude", Value(-9.81f));
  layer.end_prim();

  // Applied API schemas on Xform
  layer.begin_prim("RigidBody", "Xform");
  { auto* s = layer.current(); s->meta().apiSchemas().push_back("PhysicsRigidBodyAPI"); }
  layer.add_property("physics:rigidBodyEnabled", Value(true));
  layer.end_prim();

  layer.begin_prim("Collider", "Xform");
  { auto* s = layer.current(); s->meta().apiSchemas().push_back("PhysicsCollisionAPI"); }
  layer.add_property("physics:collisionEnabled", Value(true));
  layer.end_prim();

  layer.begin_prim("Mat", "Xform");
  { auto* s = layer.current(); s->meta().apiSchemas().push_back("PhysicsMaterialAPI"); }
  layer.add_property("physics:staticFriction", Value(0.5f));
  layer.end_prim();

  layer.begin_prim("Mass", "Xform");
  { auto* s = layer.current(); s->meta().apiSchemas().push_back("PhysicsMassAPI"); }
  layer.add_property("physics:mass", Value(10.0f));
  layer.end_prim();

  // Joints
  layer.begin_prim("Joint", "PhysicsJoint");
  layer.add_property("physics:breakForce", Value(100.0f));
  layer.end_prim();

  layer.begin_prim("Prismatic", "PhysicsPrismaticJoint");
  layer.add_property("physics:lowerLimit", Value(-10.0f));
  layer.end_prim();

  layer.begin_prim("Revolute", "PhysicsRevoluteJoint");
  layer.add_property("physics:lowerLimit", Value(-90.0f));
  layer.end_prim();

  layer.begin_prim("Spherical", "PhysicsSphericalJoint");
  layer.add_property("physics:coneAngleLimit", Value(45.0f));
  layer.end_prim();

  layer.begin_prim("Fixed", "PhysicsFixedJoint");
  layer.end_prim();

  layer.begin_prim("Distance", "PhysicsDistanceJoint");
  layer.add_property("physics:minDistance", Value(0.5f));
  layer.end_prim();

  // CollisionGroup
  layer.begin_prim("ColGroup", "PhysicsCollisionGroup");
  layer.add_property("physics:collisionEnabled", Value(true));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

// ============================================================
// Skel tests
// ============================================================

void test_skel_types() {
  TEST("IsSkelRoot");
  auto stage = MakeSkelStage();
  auto prim = stage.GetPrimAtPath("/Root");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsSkelRoot(prim)) { FAIL("expected SkelRoot"); return; }
  PASS();
}

void test_skel_validate_topology() {
  // topology[i] is the parent index of joint i; -1 marks the root.
  TEST("SkelValidateTopology");
  // Valid: root 0, chain 0 -> 1 -> 2.
  {
    std::vector<int> topology = {-1, 0, 1};
    std::string err;
    if (!SkelValidateTopology(topology, &err)) {
      FAIL(("valid chain rejected: " + err).c_str());
      return;
    }
  }
  // Valid: root 0, branch (1 and 4 -> 0, 2 -> 1, 3 -> 2).
  {
    std::vector<int> topology = {-1, 0, 1, 2, 1};
    std::string err;
    if (!SkelValidateTopology(topology, &err)) {
      FAIL(("valid branch rejected: " + err).c_str());
      return;
    }
  }
  // Rejected: 2-cycle (1's parent 2, 2's parent 1). The old DFS reported
  // this as valid; the 3-color walk must reject it.
  {
    std::vector<int> topology = {-1, 2, 1};
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("2-cycle not detected");
      return;
    }
  }
  // Rejected: self-loop (node 1's parent is itself).
  {
    std::vector<int> topology = {-1, 1};
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("self-loop not detected");
      return;
    }
  }
  // Rejected: two roots.
  {
    std::vector<int> topology = {-1, -1};
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("two roots not rejected");
      return;
    }
  }
  // Rejected: out-of-range parent.
  {
    std::vector<int> topology = {-1, 5};
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("out-of-range parent not rejected");
      return;
    }
  }
  // Rejected: parent values other than -1 are not valid roots.
  {
    std::vector<int> topology = {-1, -2};
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("negative non-root parent not rejected");
      return;
    }
  }
  // Rejected: empty.
  {
    std::vector<int> topology;
    std::string err;
    if (SkelValidateTopology(topology, &err)) {
      FAIL("empty topology not rejected");
      return;
    }
  }
  PASS();
}

void test_skeleton() {
  TEST("IsSkeleton");
  auto stage = MakeSkelStage();
  auto prim = stage.GetPrimAtPath("/Skel");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsSkeleton(prim)) { FAIL("expected Skeleton"); return; }
  SkeletonData data;
  if (!GetSkeletonData(stage, prim, &data)) { FAIL("GetSkeletonData"); return; }
  if (data.bindTransforms.size() != 16) { FAIL("expected 16 bindTransforms"); return; }
  PASS();
}

void test_skel_animation() {
  TEST("IsSkelAnimation");
  auto stage = MakeSkelStage();
  auto prim = stage.GetPrimAtPath("/Anim");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsSkelAnimation(prim)) { FAIL("expected SkelAnimation"); return; }
  SkelAnimationData data;
  if (!GetSkelAnimationData(stage, prim, &data)) { FAIL("GetSkelAnimationData"); return; }
  if (!data.hasTranslations) { FAIL("expected translations"); return; }
  PASS();
}

void test_blend_shape() {
  TEST("IsBlendShape");
  auto stage = MakeSkelStage();
  auto prim = stage.GetPrimAtPath("/Blend");
  if (!prim.IsValid()) { FAIL("prim not found"); return; }
  if (!IsBlendShape(prim)) { FAIL("expected BlendShape"); return; }
  BlendShapeData data;
  if (!GetBlendShapeData(stage, prim, &data)) { FAIL("GetBlendShapeData"); return; }
  if (data.offsets.size() != 6) { FAIL("expected 6 offset values"); return; }
  PASS();
}

// ============================================================
// AR tests
// ============================================================

void test_ar_types() {
  auto stage = MakeARStage();

  {
    TEST("IsARAnchor");
    auto p = stage.GetPrimAtPath("/Anchor");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARAnchor(p)) { FAIL("expected ARAnchor"); return; }
    ARAnchorData d;
    if (!GetARAnchorData(stage, p, &d)) { FAIL("GetARAnchorData"); return; }
    PASS();
  }
  {
    TEST("IsARImage");
    auto p = stage.GetPrimAtPath("/Image");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARImage(p)) { FAIL("expected ARImage"); return; }
    ARImageData d;
    if (!GetARImageData(stage, p, &d)) { FAIL("GetARImageData"); return; }
    if (d.physicalWidth < 0.4f) { FAIL("expected physicalWidth"); return; }
    PASS();
  }
  {
    TEST("IsARFaceGeometry");
    auto p = stage.GetPrimAtPath("/Face");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARFaceGeometry(p)) { FAIL("expected ARFaceGeometry"); return; }
    ARFaceGeometryData d;
    if (!GetARFaceGeometryData(stage, p, &d)) { FAIL("GetARFaceGeometryData"); return; }
    PASS();
  }
  {
    TEST("IsARPlane");
    auto p = stage.GetPrimAtPath("/Plane");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARPlane(p)) { FAIL("expected ARPlane"); return; }
    ARPlaneData d;
    if (!GetARPlaneData(stage, p, &d)) { FAIL("GetARPlaneData"); return; }
    PASS();
  }
  {
    TEST("IsARPointCloud");
    auto p = stage.GetPrimAtPath("/Cloud");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARPointCloud(p)) { FAIL("expected ARPointCloud"); return; }
    ARPointCloudData d;
    if (!GetARPointCloudData(stage, p, &d)) { FAIL("GetARPointCloudData"); return; }
    PASS();
  }
  {
    TEST("IsARDevice");
    auto p = stage.GetPrimAtPath("/Device");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARDevice(p)) { FAIL("expected ARDevice"); return; }
    ARDeviceData d;
    if (!GetARDeviceData(stage, p, &d)) { FAIL("GetARDeviceData"); return; }
    if (d.fieldOfView < 59.0f) { FAIL("expected fieldOfView"); return; }
    PASS();
  }
  {
    TEST("IsARScene");
    auto p = stage.GetPrimAtPath("/Scene");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsARScene(p)) { FAIL("expected ARScene"); return; }
    PASS();
  }
}

void test_ar_api_schemas() {
  auto stage = MakeARStage();

  {
    TEST("HasARAnchorAPI");
    auto p = stage.GetPrimAtPath("/WithAnchorAPI");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasARAnchorAPI(p)) { FAIL("expected ARAnchorAPI"); return; }
    ARAnchorAPIData d;
    if (!GetARAnchorAPIData(p, &d)) { FAIL("GetARAnchorAPIData"); return; }
    PASS();
  }
  {
    TEST("HasARImagingAPI");
    auto p = stage.GetPrimAtPath("/WithAnchorAPI");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasARImagingAPI(p)) { FAIL("expected ARImagingAPI"); return; }
    ARImagingAPIData d;
    if (!GetARImagingAPIData(p, &d)) { FAIL("GetARImagingAPIData"); return; }
    PASS();
  }
}

// ============================================================
// Mtlx tests
// ============================================================

void test_mtlx_types() {
  auto stage = MakeMtlxStage();

  {
    TEST("IsMaterialXShader");
    auto p = stage.GetPrimAtPath("/Shader");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsMaterialXShader(p)) { FAIL("expected MaterialXShader"); return; }
    MaterialXShaderData d;
    if (!GetMaterialXShaderData(stage, p, &d)) { FAIL("GetMaterialXShaderData"); return; }
    if (d.nodeDefId != "ND_StandardSurface") { FAIL("expected nodeDefId"); return; }
    PASS();
  }
  {
    TEST("IsMaterialXNodeGraph");
    auto p = stage.GetPrimAtPath("/NodeGraph");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsMaterialXNodeGraph(p)) { FAIL("expected MaterialXNodeGraph"); return; }
    PASS();
  }
  {
    TEST("IsMaterialXLightNode");
    auto p = stage.GetPrimAtPath("/LightNode");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsMaterialXLightNode(p)) { FAIL("expected MaterialXLightNode"); return; }
    MaterialXLightNodeData d;
    if (!GetMaterialXLightNodeData(stage, p, &d)) { FAIL("GetMaterialXLightNodeData"); return; }
    PASS();
  }
  {
    TEST("IsMaterialXNodeIO");
    auto p = stage.GetPrimAtPath("/NodeIO");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsMaterialXNodeIO(p)) { FAIL("expected MaterialXNodeIO"); return; }
    MaterialXNodeIOData d;
    if (!GetMaterialXNodeIOData(stage, p, &d)) { FAIL("GetMaterialXNodeIOData"); return; }
    PASS();
  }
}

// ============================================================
// Media tests
// ============================================================

void test_media_types() {
  TEST("IsMediaAudio");
  auto stage = MakeMediaStage();
  auto p = stage.GetPrimAtPath("/Audio");
  if (!p.IsValid()) { FAIL("prim not found"); return; }
  if (!IsMediaAudio(p)) { FAIL("expected MediaAudio"); return; }
  MediaAudioData d;
  if (!GetMediaAudioData(stage, p, &d)) { FAIL("GetMediaAudioData"); return; }
  if (d.gain < 0.7f) { FAIL("expected gain"); return; }
  if (!d.loop) { FAIL("expected loop=true"); return; }
  PASS();
}

// ============================================================
// Physics tests
// ============================================================

void test_physics_scene() {
  TEST("IsPhysicsScene");
  auto stage = MakePhysicsStage();
  auto p = stage.GetPrimAtPath("/Scene");
  if (!p.IsValid()) { FAIL("prim not found"); return; }
  if (!IsPhysicsScene(p)) { FAIL("expected PhysicsScene"); return; }
  PhysicsSceneData d;
  if (!GetPhysicsSceneData(stage, p, &d)) { FAIL("GetPhysicsSceneData"); return; }
  if (d.gravityMagnitude > -9.0f) { FAIL("expected gravityMagnitude"); return; }
  PASS();
}

void test_physics_api_schemas() {
  auto stage = MakePhysicsStage();

  {
    TEST("HasPhysicsRigidBodyAPI");
    auto p = stage.GetPrimAtPath("/RigidBody");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasPhysicsRigidBodyAPI(p)) { FAIL("expected RigidBodyAPI"); return; }
    PhysicsRigidBodyData d;
    if (!GetPhysicsRigidBodyData(stage, p, &d)) { FAIL("GetPhysicsRigidBodyData"); return; }
    if (!d.rigidBodyEnabled) { FAIL("expected enabled"); return; }
    PASS();
  }
  {
    TEST("HasPhysicsCollisionAPI");
    auto p = stage.GetPrimAtPath("/Collider");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasPhysicsCollisionAPI(p)) { FAIL("expected CollisionAPI"); return; }
    PhysicsCollisionData d;
    if (!GetPhysicsCollisionData(stage, p, &d)) { FAIL("GetPhysicsCollisionData"); return; }
    if (!d.collisionEnabled) { FAIL("expected enabled"); return; }
    PASS();
  }
  {
    TEST("HasPhysicsMaterialAPI");
    auto p = stage.GetPrimAtPath("/Mat");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasPhysicsMaterialAPI(p)) { FAIL("expected MaterialAPI"); return; }
    PhysicsMaterialData d;
    if (!GetPhysicsMaterialData(stage, p, &d)) { FAIL("GetPhysicsMaterialData"); return; }
    if (d.staticFriction < 0.4f) { FAIL("expected staticFriction"); return; }
    PASS();
  }
  {
    TEST("HasPhysicsMassAPI");
    auto p = stage.GetPrimAtPath("/Mass");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!HasPhysicsMassAPI(p)) { FAIL("expected MassAPI"); return; }
    PhysicsMassData d;
    if (!GetPhysicsMassData(stage, p, &d)) { FAIL("GetPhysicsMassData"); return; }
    if (d.mass < 9.0f) { FAIL("expected mass"); return; }
    PASS();
  }
}

void test_physics_joints() {
  auto stage = MakePhysicsStage();

  {
    TEST("IsPhysicsJoint");
    auto p = stage.GetPrimAtPath("/Joint");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsJoint(p)) { FAIL("expected PhysicsJoint"); return; }
    PhysicsJointData d;
    if (!GetPhysicsJointData(stage, p, &d)) { FAIL("GetPhysicsJointData"); return; }
    PASS();
  }
  {
    TEST("IsPhysicsPrismaticJoint");
    auto p = stage.GetPrimAtPath("/Prismatic");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsPrismaticJoint(p)) { FAIL("expected PrismaticJoint"); return; }
    PhysicsPrismaticJointData d;
    if (!GetPhysicsPrismaticJointData(stage, p, &d)) { FAIL("GetPrismaticJointData"); return; }
    if (d.lowerLimit > -9.0f) { FAIL("expected lowerLimit"); return; }
    PASS();
  }
  {
    TEST("IsPhysicsRevoluteJoint");
    auto p = stage.GetPrimAtPath("/Revolute");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsRevoluteJoint(p)) { FAIL("expected RevoluteJoint"); return; }
    PhysicsRevoluteJointData d;
    if (!GetPhysicsRevoluteJointData(stage, p, &d)) { FAIL("GetRevoluteJointData"); return; }
    PASS();
  }
  {
    TEST("IsPhysicsSphericalJoint");
    auto p = stage.GetPrimAtPath("/Spherical");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsSphericalJoint(p)) { FAIL("expected SphericalJoint"); return; }
    PhysicsSphericalJointData d;
    if (!GetPhysicsSphericalJointData(stage, p, &d)) { FAIL("GetSphericalJointData"); return; }
    PASS();
  }
  {
    TEST("IsPhysicsFixedJoint");
    auto p = stage.GetPrimAtPath("/Fixed");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsFixedJoint(p)) { FAIL("expected FixedJoint"); return; }
    PhysicsFixedJointData d;
    if (!GetPhysicsFixedJointData(stage, p, &d)) { FAIL("GetFixedJointData"); return; }
    PASS();
  }
  {
    TEST("IsPhysicsDistanceJoint");
    auto p = stage.GetPrimAtPath("/Distance");
    if (!p.IsValid()) { FAIL("prim not found"); return; }
    if (!IsPhysicsDistanceJoint(p)) { FAIL("expected DistanceJoint"); return; }
    PhysicsDistanceJointData d;
    if (!GetPhysicsDistanceJointData(stage, p, &d)) { FAIL("GetDistanceJointData"); return; }
    if (d.minDistance < 0.4f) { FAIL("expected minDistance"); return; }
    PASS();
  }
}

void test_physics_collision_group() {
  TEST("IsPhysicsCollisionGroup");
  auto stage = MakePhysicsStage();
  auto p = stage.GetPrimAtPath("/ColGroup");
  if (!p.IsValid()) { FAIL("prim not found"); return; }
  if (!IsPhysicsCollisionGroup(p)) { FAIL("expected CollisionGroup"); return; }
  PhysicsCollisionGroupData d;
  if (!GetPhysicsCollisionGroupData(stage, p, &d)) { FAIL("GetCollisionGroupData"); return; }
  if (!d.collisionEnabled) { FAIL("expected enabled"); return; }
  PASS();
}

// ============================================================
// Non-core OpenUSD domain-schema breadth (product parity):
// UsdVol / UsdRender / UsdGeom Hermite-TetMesh-NurbsPatch registry entries.
// ============================================================

static void test_domain_schema_breadth() {
  TEST("domain schema breadth (UsdVol/UsdRender/geom)");

  StageBuilder sb;
  auto& layer = sb.GetLayerBuilder();
  layer.begin_prim("Patch", "NurbsPatch");
  layer.end_prim();
  layer.begin_prim("Tet", "TetMesh");
  layer.end_prim();
  layer.begin_prim("Hermite", "HermiteCurves");
  layer.end_prim();
  layer.begin_prim("Vdb", "OpenVDBAsset");
  layer.end_prim();
  layer.begin_prim("Vol", "Volume");
  layer.end_prim();
  layer.begin_prim("Rs", "RenderSettings");
  layer.add_property("resolution", Value::MakeInt2(1280, 720));
  layer.add_property("disableMotionBlur", Value(true));
  layer.add_relationship("products", Path("/Rp"));
  layer.end_prim();
  layer.begin_prim("Rp", "RenderProduct");
  layer.add_property("productType", Value::MakeToken("raster"));
  layer.add_relationship("orderedVars", Path("/Rv"));
  layer.end_prim();
  layer.begin_prim("Rv", "RenderVar");
  layer.add_property("sourceName", Value(std::string("depth")));
  layer.end_prim();
  layer.begin_prim("Pass", "RenderPass");
  layer.add_relationship("renderSource", Path("/Rs"));
  layer.add_relationship("collection:renderVisibility:includes", Path("/Keep"));
  layer.add_relationship("collection:renderVisibility:excludes", Path("/Drop"));
  layer.add_property("collection:renderVisibility:includeRoot", Value(false));
  layer.add_relationship("collection:cameraVisibility:includes", Path("/Camera"));
  layer.add_relationship("collection:cameraVisibility:excludes", Path("/HiddenCamera"));
  layer.add_property("collection:cameraVisibility:includeRoot", Value(false));
  layer.end_prim();
  layer.begin_prim("Splat", "ParticleField3DGaussianSplat");
  layer.add_property("positions", Value::MakeFloatCompArray(
      {0, 0, 0, 1, 0, 0}, TypeId::Point3f, 3));
  layer.add_property("scales", Value::MakeFloat3Array({1, 1, 1, 2, 2, 2}));
  layer.end_prim();
  layer.begin_prim("SplatHalf", "ParticleField3DGaussianSplat");
  layer.add_property("positionsh", Value::MakeFloatCompArray(
      {0, 0, 0, 1, 2, 3}, TypeId::Point3h, 3));
  layer.add_property("scalesh", Value::MakeFloatCompArray(
      {1, 1, 1, 2, 2, 2}, TypeId::Half3, 3));
  layer.end_prim();
  layer.begin_prim("Camera", "Camera");
  layer.current()->meta().apiSchemas().push_back("BackPlateAPI:plate");
  layer.add_property("backPlate:plate:image",
                     Value::MakeAssetPath("plate.exr"));
  layer.add_property("backPlate:plate:scale:tweak",
                     Value::MakeFloat2(2.0f, 3.0f));
  layer.end_prim();
  layer.begin_prim("Model", "Xform");
  layer.current()->meta().apiSchemas().push_back("GeomModelAPI");
  layer.add_property("model:cardVisibility", Value::MakeToken("simple"));
  layer.add_property("model:cardTextureXPos",
                     Value::MakeAssetPath("xpos.png"));
  layer.begin_prim("Child", "Xform");
  layer.current()->meta().apiSchemas().push_back("GeomModelAPI");
  layer.end_prim();
  layer.end_prim();
  layer.begin_prim("Semantic", "Xform");
  layer.current()->meta().apiSchemas().push_back("SemanticsLabelsAPI:class");
  layer.add_property("semantics:labels:class",
                     Value::MakeTokenArray({"vehicle", "foreground"}));
  layer.end_prim();
  layer.finalize();
  Stage stage = sb.Build();

  const SchemaRegistry& registry = GetSchemaRegistry();
  auto spec = [&](const char* path) {
    const PrimSpec* ps = stage.GetPrimAtPath(path).GetPrimSpec();
    assert(ps);
    return ps;
  };

  // Fallbacks (direct + inherited through the parents chain).
  const SchemaPropertyDefinition* uform =
      registry.FindProperty(*spec("/Patch"), "uForm");
  assert(uform && uform->has_fallback &&
         *uform->fallback.as_token() == "open");
  const SchemaPropertyDefinition* role =
      registry.FindProperty(*spec("/Vdb"), "vectorDataRoleHint");
  assert(role && role->has_fallback && *role->fallback.as_token() == "None" &&
         "OpenVDBAsset inherits VolumeFieldAsset's vectorDataRoleHint");
  const SchemaPropertyDefinition* res =
      registry.FindProperty(*spec("/Rs"), "resolution");
  assert(res && res->has_fallback &&
         "RenderSettings inherits RenderSettingsBase's resolution");
  const int32_t* res2 = res->fallback.as_int2();
  assert(res2 && res2[0] == 2048 && res2[1] == 1080);
  const SchemaPropertyDefinition* dt =
      registry.FindProperty(*spec("/Rv"), "dataType");
  assert(dt && dt->has_fallback && *dt->fallback.as_token() == "color3f");
  // Volume is a Gprim: Imageable fallbacks resolve through the chain.
  const SchemaPropertyDefinition* vis =
      registry.FindProperty(*spec("/Vol"), "visibility");
  assert(vis && vis->has_fallback &&
         *vis->fallback.as_token() == "inherited");
  // RenderSettings is NOT Imageable: no visibility definition.
  assert(!registry.FindProperty(*spec("/Rs"), "visibility"));

  // Declarations (no fallback, but the property is known).
  assert(registry.FindProperty(*spec("/Tet"), "tetVertexIndices"));
  assert(registry.FindProperty(*spec("/Tet"), "surfaceFaceVertexIndices"));
  const SchemaPropertyDefinition* tangents =
      registry.FindProperty(*spec("/Hermite"), "tangents");
  assert(tangents && tangents->has_fallback &&
         tangents->fallback.is_array() && tangents->fallback.array_size() == 0 &&
         "pxr: vector3f[] tangents = [] (empty-array fallback)");
  // pxr parity: NurbsCurves has no `ids`; Volume has no builtin literally
  // named "field" (field:<name> relationships are dynamic).
  {
    StageBuilder nb;
    auto& nl = nb.GetLayerBuilder();
    nl.begin_prim("NC", "NurbsCurves");
    nl.end_prim();
    nl.finalize();
    Stage nstage = nb.Build();
    assert(!registry.FindProperty(*nstage.GetPrimAtPath("/NC").GetPrimSpec(),
                                  "ids"));
  }
  assert(!registry.FindProperty(*spec("/Vol"), "field"));
  assert(registry.FindProperty(*spec("/Patch"), "trimCurve:knots"));
  assert(registry.FindProperty(*spec("/Vdb"), "filePath"));
  assert(registry.FindProperty(*spec("/Vdb"), "fieldClass"));
  assert(registry.FindProperty(*spec("/Rs"), "products"));

  // OpenUSD 26.08 additions and domain gaps.
  const SchemaPropertyDefinition* projection =
      registry.FindProperty(*spec("/Splat"), "projectionModeHint");
  assert(projection && projection->has_fallback &&
         *projection->fallback.as_token() == "perspective");
  assert(registry.FindProperty(*spec("/Splat"), "positions") &&
         "Gaussian schemas are auto-applied without authored apiSchemas");
  ParticleFieldData particle_field;
  assert(GetParticleFieldData(stage, stage.GetPrimAtPath("/Splat"),
                              &particle_field));
  assert(particle_field.particle_count == 2 &&
         particle_field.positions_property == "positions" &&
         particle_field.scales_property == "scales" &&
         particle_field.kernel == ParticleKernel::GaussianEllipsoid);
  ParticleFieldData half_field;
  const UsdPrim half_prim = stage.GetPrimAtPath("/SplatHalf");
  assert(GetParticleFieldData(stage, half_prim, &half_field));
  assert(half_field.particle_count == 2 && half_field.positions_half &&
         half_field.scales_half && half_field.positions_property == "positionsh");
  const Value* half_positions = half_prim.GetPropertyValue("positionsh");
  ArrayScratch<float> half_scratch;
  ArrayView<float> half_view;
  assert(half_positions &&
         GetFloatArrayView(*half_positions, &half_scratch, &half_view) &&
         half_view.size == 6 && half_view[5] == 3.0f);
  const SchemaPropertyDefinition* render_root = registry.FindProperty(
      *spec("/Pass"), "collection:renderVisibility:includeRoot");
  assert(render_root && render_root->has_fallback &&
         *render_root->fallback.as_bool());
  assert(registry.FindProperty(*spec("/Pass"), "renderSource"));
  const SchemaPropertyDefinition* plate = registry.FindProperty(
      *spec("/Camera"), "backPlate:plate:plateVisibility");
  assert(plate && plate->has_fallback &&
         *plate->fallback.as_token() == "solo");
  const SchemaPropertyDefinition* card =
      registry.FindProperty(*spec("/Model"), "model:cardVisibility");
  assert(card && card->has_fallback &&
         *card->fallback.as_token() == "inherited");
  GeomModelData geom_model;
  const UsdPrim model = stage.GetPrimAtPath("/Model");
  assert(GetGeomModelData(stage, model, &geom_model) &&
         geom_model.card_visibility == "simple" &&
         geom_model.card_geometry == "cross" &&
         geom_model.card_textures[1] == "xpos.png");
  const UsdPrim model_child = stage.GetPrimAtPath("/Model/Child");
  assert(ComputeModelCardVisibility(stage, model_child) == "simple");
  assert(ComputeModelCardFaceMask(stage, model_child, 'Z') ==
         (kAllCardFaces & ~(kCardZNeg | kCardZPos)));
  const SchemaPropertyDefinition* labels = registry.FindProperty(
      *spec("/Semantic"), "semantics:labels:class");
  assert(labels && labels->has_fallback && labels->fallback.is_array() &&
         labels->fallback.array_size() == 0);

  BackPlateData back_plate;
  assert(GetBackPlateData(stage, stage.GetPrimAtPath("/Camera"), "plate",
                          &back_plate));
  assert(back_plate.image == "plate.exr" &&
         back_plate.scale_tweak[0] == 2.0f &&
         back_plate.scale_tweak[1] == 3.0f &&
         back_plate.plate_visibility == "solo");
  assert(!HasBackPlateAPI(stage.GetPrimAtPath("/Camera"), "plate:"));
  std::vector<std::string> semantic_labels;
  assert(GetSemanticsLabels(stage, stage.GetPrimAtPath("/Semantic"), "class",
                            &semantic_labels));
  assert(semantic_labels.size() == 2 && semantic_labels[0] == "vehicle" &&
         semantic_labels[1] == "foreground");
  assert(!HasSemanticsLabelsAPI(stage.GetPrimAtPath("/Semantic"),
                                "class:invalid:"));

  RenderSettingsData settings;
  assert(GetRenderSettingsData(stage, stage.GetPrimAtPath("/Rs"), &settings));
  assert(settings.resolution[0] == 1280 && settings.resolution[1] == 720);
  assert(settings.disable_motion_blur && settings.products.size() == 1 &&
         settings.products[0].str() == "/Rp");
  RenderProductData product;
  assert(GetRenderProductData(stage, stage.GetPrimAtPath("/Rp"), &product));
  assert(product.product_type == "raster" && product.ordered_vars.size() == 1);
  RenderVarData render_var;
  assert(GetRenderVarData(stage, stage.GetPrimAtPath("/Rv"), &render_var));
  assert(render_var.source_name == "depth" && render_var.data_type == "color3f");
  RenderPassData render_pass;
  assert(GetRenderPassData(stage, stage.GetPrimAtPath("/Pass"), &render_pass));
  assert(render_pass.render_source.size() == 1 &&
         render_pass.render_source[0].str() == "/Rs" &&
         render_pass.render_visibility_includes.size() == 1 &&
         render_pass.render_visibility_includes[0].str() == "/Keep" &&
         render_pass.render_visibility_excludes.size() == 1 &&
         render_pass.render_visibility_excludes[0].str() == "/Drop" &&
         !render_pass.render_visibility_include_root &&
         render_pass.camera_visibility_includes.size() == 1 &&
         render_pass.camera_visibility_includes[0].str() == "/Camera" &&
         render_pass.camera_visibility_excludes.size() == 1 &&
         render_pass.camera_visibility_excludes[0].str() == "/HiddenCamera" &&
         !render_pass.camera_visibility_include_root);

  PASS();
}

// ============================================================
// Main
// ============================================================

int main() {
  printf("Extended Schema Tests\n");
  printf("=====================\n\n");

  test_generated_supported_schema_fixture();

  printf("Skel:\n");
  test_skel_types();
  test_skel_validate_topology();
  test_skeleton();
  test_skel_animation();
  test_blend_shape();

  printf("\nAR:\n");
  test_ar_types();
  test_ar_api_schemas();

  printf("\nMaterialX:\n");
  test_mtlx_types();
  test_connected_shader_constant();

  printf("\nMedia:\n");
  test_media_types();

  printf("\nPhysics:\n");
  test_physics_scene();
  test_physics_api_schemas();
  test_physics_joints();
  test_physics_collision_group();

  printf("\nDomain breadth (product parity):\n");
  test_domain_schema_breadth();

  printf("\n%d/%d tests passed\n", pass_count, test_count);
  return pass_count == test_count ? 0 : 1;
}
