// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Test for Stage functionality

#include "next/layer/layer.hh"
#include <iostream>
#include <cassert>
#include <cmath>
#include <algorithm>

#include "next/stage/stage.hh"
#include "next/eval/attribute-eval.hh"

using namespace lightusd::next;

void test_stage_builder() {
  std::cout << "Testing StageBuilder..." << std::endl;

  StageBuilder builder;
  builder.SetDefaultPrim("World");
  builder.SetUpAxis("Z");
  builder.SetMetersPerUnit(1.0);
  builder.SetTimeCodesPerSecond(30.0);
  builder.SetStartTimeCode(1.0);
  builder.SetEndTimeCode(100.0);

  LayerBuilder& lb = builder.GetLayerBuilder();

  // Build hierarchy:
  // /World (Xform)
  //   /Geometry (Scope)
  //     /Mesh (Mesh)
  //   /Materials (Scope)
  //     /DefaultMaterial (Material)

  lb.begin_prim("World", "Xform");
    lb.begin_prim("Geometry", "Scope");
      lb.begin_prim("Mesh", "Mesh");
      lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{
        0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0
      }));
      lb.add_property("faceVertexCounts", Value::MakeIntArray(std::vector<int32_t>{4}));
      lb.add_property("faceVertexIndices", Value::MakeIntArray(std::vector<int32_t>{0, 1, 2, 3}));
      lb.add_relationship("material:binding", Path("/World/Materials/DefaultMaterial"));
      lb.end_prim();
    lb.end_prim();

    lb.begin_prim("Materials", "Scope");
      lb.begin_prim("DefaultMaterial", "Material");
      lb.end_prim();
    lb.end_prim();
  lb.end_prim();

  Stage stage = builder.Build();

  // Verify metadata
  assert(stage.GetMeta().defaultPrim == "World" && "defaultPrim should be World");
  assert(stage.GetUpAxis() == "Z" && "upAxis should be Z");
  assert(stage.GetMetersPerUnit() == 1.0 && "metersPerUnit should be 1.0");
  assert(stage.GetTimeCodesPerSecond() == 30.0 && "fps should be 30");
  assert(stage.GetStartTimeCode() == 1.0 && "startTimeCode should be 1");
  assert(stage.GetEndTimeCode() == 100.0 && "endTimeCode should be 100");

  std::cout << "  StageBuilder: PASSED" << std::endl;
}

void test_stage_prim_access() {
  std::cout << "Testing Stage prim access..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("Root", "Xform");
    lb.begin_prim("Child1", "Mesh");
    lb.end_prim();
    lb.begin_prim("Child2", "Mesh");
    lb.end_prim();
  lb.end_prim();

  Stage stage = builder.Build();

  // Get prim count
  assert(stage.GetPrimCount() == 3 && "should have 3 prims");

  // Get prim at path
  UsdPrim root = stage.GetPrimAtPath("/Root");
  assert(root.IsValid() && "should find /Root");
  assert(root.GetName() == "Root" && "name should be Root");
  assert(root.GetTypeName() == "Xform" && "type should be Xform");

  UsdPrim child1 = stage.GetPrimAtPath("/Root/Child1");
  assert(child1.IsValid() && "should find /Root/Child1");
  assert(child1.GetTypeName() == "Mesh" && "Child1 should be Mesh");

  // Check if path exists
  assert(stage.HasPrimAtPath("/Root") && "should have /Root");
  assert(stage.HasPrimAtPath("/Root/Child2") && "should have /Root/Child2");
  assert(!stage.HasPrimAtPath("/NonExistent") && "should not have /NonExistent");

  // Get root prims
  auto roots = stage.GetRootPrims();
  assert(roots.size() == 1 && "should have 1 root prim");
  assert(roots[0].GetName() == "Root" && "root should be Root");

  std::cout << "  Stage prim access: PASSED" << std::endl;
}

void test_usd_prim() {
  std::cout << "Testing UsdPrim..." << std::endl;

  StageBuilder builder;
  builder.SetDefaultPrim("World");
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("World", "Xform");
    lb.begin_prim("Mesh", "Mesh");
    lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
    lb.add_property("doubleSided", Value(true));
    lb.set_active(true);
    lb.end_prim();
  lb.end_prim();

  Stage stage = builder.Build();

  // Test default prim
  UsdPrim defaultPrim = stage.GetDefaultPrim();
  assert(defaultPrim.IsValid() && "should have default prim");
  assert(defaultPrim.GetName() == "World" && "default prim should be World");

  // Test prim properties
  UsdPrim mesh = stage.GetPrimAtPath("/World/Mesh");
  assert(mesh.IsValid() && "should find Mesh");
  assert(mesh.IsDefined() && "Mesh should be defined");
  assert(mesh.IsActive() && "Mesh should be active");

  // Test HasProperty
  assert(mesh.HasProperty("points") && "should have points");
  assert(mesh.HasProperty("doubleSided") && "should have doubleSided");
  assert(!mesh.HasProperty("nonExistent") && "should not have nonExistent");

  // Test GetPropertyValue
  const Value* points = mesh.GetPropertyValue("points");
  assert(points != nullptr && "should get points value");
  assert(points->is_array() && "points should be array");

  const Value* ds = mesh.GetPropertyValue("doubleSided");
  assert(ds != nullptr && "should get doubleSided value");
  assert(ds->as_bool() != nullptr && "doubleSided should be bool");
  assert(*ds->as_bool() == true && "doubleSided should be true");

  // Test GetPropertyNames
  auto names = mesh.GetPropertyNames();
  assert(std::find(names.begin(), names.end(), "points") != names.end());
  assert(std::find(names.begin(), names.end(), "doubleSided") != names.end());
  // AOUSD stage population includes schema-defined properties even when no
  // authored spec exists for them.
  assert(std::find(names.begin(), names.end(), "orientation") != names.end());
  assert(std::find(names.begin(), names.end(), "visibility") != names.end());

  // Test hierarchy
  UsdPrim world = mesh.GetParent();
  assert(world.IsValid() && "mesh should have parent");
  assert(world.GetName() == "World" && "parent should be World");

  auto children = world.GetChildren();
  assert(children.size() == 1 && "World should have 1 child");
  assert(children[0].GetName() == "Mesh" && "child should be Mesh");

  UsdPrim child = world.GetChild("Mesh");
  assert(child.IsValid() && "should find Mesh child");

  std::cout << "  UsdPrim: PASSED" << std::endl;
}

void test_stage_traversal() {
  std::cout << "Testing Stage traversal..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  // Create a deeper hierarchy
  lb.begin_prim("A", "Xform");
    lb.begin_prim("B", "Xform");
      lb.begin_prim("C", "Mesh");
      lb.end_prim();
      lb.begin_prim("D", "Mesh");
      lb.end_prim();
    lb.end_prim();
    lb.begin_prim("E", "Xform");
      lb.begin_prim("F", "Mesh");
      lb.end_prim();
    lb.end_prim();
  lb.end_prim();
  lb.begin_prim("G", "Xform");
  lb.end_prim();

  Stage stage = builder.Build();

  // Count prims via traversal
  int count = 0;
  stage.Traverse([&](const UsdPrim& prim) {
    count++;
    return true;
  });
  assert(count == 7 && "should traverse 7 prims");

  // Collect prim names in order
  std::vector<std::string> names;
  stage.Traverse([&](const UsdPrim& prim) {
    names.push_back(prim.GetName());
    return true;
  });
  assert(names.size() == 7 && "should have 7 names");
  assert(names[0] == "A" && "first should be A");
  assert(names[1] == "B" && "second should be B");
  assert(names[2] == "C" && "third should be C");

  // Test early termination
  int visited = 0;
  stage.Traverse([&](const UsdPrim& prim) {
    visited++;
    return visited < 3;  // Stop after 3
  });
  assert(visited == 3 && "should stop after 3");

  // Test GetPrimsOfType
  auto meshes = stage.GetPrimsOfType("Mesh");
  assert(meshes.size() == 3 && "should have 3 meshes");

  auto xforms = stage.GetPrimsOfType("Xform");
  assert(xforms.size() == 4 && "should have 4 xforms");

  std::cout << "  Stage traversal: PASSED" << std::endl;
}

void test_stage_relationships() {
  std::cout << "Testing Stage relationships..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("Mesh", "Mesh");
  lb.add_relationship("material:binding", Path("/Materials/MyMat"));
  lb.end_prim();

  lb.begin_prim("Materials", "Scope");
    lb.begin_prim("MyMat", "Material");
    lb.end_prim();
  lb.end_prim();

  Stage stage = builder.Build();

  UsdPrim mesh = stage.GetPrimAtPath("/Mesh");
  assert(mesh.IsValid() && "should find mesh");

  const std::vector<Path>* targets = mesh.GetRelationship("material:binding");
  assert(targets != nullptr && "should have material:binding");
  assert(targets->size() == 1 && "should have 1 target");
  assert((*targets)[0].str() == "/Materials/MyMat" && "target should match");

  // Resolve relationship target
  UsdPrim material = stage.GetPrimAtPath((*targets)[0]);
  assert(material.IsValid() && "should resolve material");
  assert(material.GetTypeName() == "Material" && "should be Material type");

  std::cout << "  Stage relationships: PASSED" << std::endl;
}

void test_stage_stats() {
  std::cout << "Testing Stage stats..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  for (int i = 0; i < 100; ++i) {
    lb.begin_prim("Prim" + std::to_string(i), "Mesh");
    lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{0, 0, 0}));
    lb.end_prim();
  }

  Stage stage = builder.Build();

  Stage::Stats stats = stage.GetStats();
  assert(stats.prim_count == 100 && "should have 100 prims");
  assert(stats.layer_count == 1 && "should have 1 layer");
  assert(stats.total_properties == 100 && "should have 100 properties");
  assert(stats.memory_bytes > 0 && "should use memory");

  std::cout << "  Stats: prim_count=" << stats.prim_count
            << ", properties=" << stats.total_properties
            << ", memory=" << stats.memory_bytes << " bytes" << std::endl;
  std::cout << "  Stage stats: PASSED" << std::endl;
}

void test_attribute_eval() {
  std::cout << "Testing AttributeEval..." << std::endl;

  StageBuilder builder;
  builder.SetTimeCodesPerSecond(24.0);
  builder.SetStartTimeCode(1.0);
  builder.SetEndTimeCode(48.0);
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("World", "Xform");

  // Mesh with static properties
  lb.begin_prim("StaticMesh", "Mesh");
  lb.add_property("doubleSided", Value(true));
  lb.add_property("radius", Value(1.5f));
  lb.add_property("extent", Value::MakeFloat3(-1.0f, -1.0f, -1.0f));
  lb.end_prim();

  // Mesh with animated visibility
  lb.begin_prim("AnimatedMesh", "Mesh");
  // Add time samples for visibility (simulating animation)
  // Note: This would typically be done via lower-level API
  lb.end_prim();

  lb.end_prim();

  Stage stage = builder.Build();
  AttributeEval eval(&stage);

  // Test basic scalar evaluation
  UsdPrim staticMesh = stage.GetPrimAtPath("/World/StaticMesh");
  assert(staticMesh.IsValid() && "should find StaticMesh");

  // Test EvalBool
  auto ds = eval.EvalBool(staticMesh, "doubleSided");
  assert(ds.has_value() && "should get doubleSided");
  assert(*ds == true && "doubleSided should be true");

  // Test EvalFloat
  auto radius = eval.EvalFloat(staticMesh, "radius");
  assert(radius.has_value() && "should get radius");
  assert(std::abs(*radius - 1.5f) < 0.001f && "radius should be 1.5");

  // Test EvalFloat3
  float extent[3];
  bool got_extent = eval.EvalFloat3(staticMesh, "extent", extent);
  assert(got_extent && "should get extent");
  assert(std::abs(extent[0] - (-1.0f)) < 0.001f && "extent[0] should be -1");

  // Test EvalOr with fallback
  float missing = eval.EvalOr(staticMesh, "nonExistent", 99.0f);
  assert(std::abs(missing - 99.0f) < 0.001f && "should use fallback");

  // Test Eval with result metadata
  EvalResult result = eval.Eval(staticMesh, "radius");
  assert(result.success && "should succeed");
  assert(result.from_default && "should be from default");
  assert(!result.from_time_sample && "should not be from time sample");
  assert(!result.interpolated && "should not be interpolated");

  // Test non-existent attribute
  auto nonexistent = eval.EvalFloat(staticMesh, "nonExistent");
  assert(!nonexistent.has_value() && "should not find nonExistent");

  std::cout << "  AttributeEval: PASSED" << std::endl;
}

void test_attribute_eval_convenience() {
  std::cout << "Testing convenience functions..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("Mesh", "Mesh");
  lb.add_property("focalLength", Value(50.0f));
  lb.add_property("translate", Value::MakeDouble3(1.0, 2.0, 3.0));
  lb.end_prim();

  Stage stage = builder.Build();
  UsdPrim mesh = stage.GetPrimAtPath("/Mesh");

  // Test convenience functions
  float fl;
  bool got_fl = GetFloat(stage, mesh, "focalLength", &fl);
  assert(got_fl && "should get focalLength");
  assert(std::abs(fl - 50.0f) < 0.001f && "focalLength should be 50");

  double translate[3];
  bool got_trans = GetDouble3(stage, mesh, "translate", translate);
  assert(got_trans && "should get translate");
  assert(std::abs(translate[0] - 1.0) < 0.001 && "translate[0] should be 1");
  assert(std::abs(translate[1] - 2.0) < 0.001 && "translate[1] should be 2");
  assert(std::abs(translate[2] - 3.0) < 0.001 && "translate[2] should be 3");

  std::cout << "  Convenience functions: PASSED" << std::endl;
}

void test_stage_move() {
  std::cout << "Testing Stage move semantics..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();
  lb.begin_prim("Test", "Mesh");
  lb.end_prim();

  Stage stage1 = builder.Build();
  assert(stage1.GetPrimCount() == 1 && "stage1 should have 1 prim");

  // Move construct
  Stage stage2 = std::move(stage1);
  assert(stage2.GetPrimCount() == 1 && "stage2 should have 1 prim after move");

  // Move assign
  Stage stage3;
  stage3 = std::move(stage2);
  assert(stage3.GetPrimCount() == 1 && "stage3 should have 1 prim after move");

  std::cout << "  Stage move semantics: PASSED" << std::endl;
}

// Regression: USD DefaultTime never consults time samples. The intentionally
// named compatibility helper retains earliest-sample behavior for consumers
// that need the former convenience semantics.
void test_default_time_timesample_fallback() {
  std::cout << "Testing explicit default-time semantics..." << std::endl;

  StageBuilder builder;
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("Anim", "Xform");
  // timeSamples only, multiple samples (earliest wins at default time).
  lb.add_time_sample("xformOp:translate", 1.0,
                     Value::MakeDouble3(10.0, 20.0, 30.0));
  lb.add_time_sample("xformOp:translate", 2.0,
                     Value::MakeDouble3(11.0, 21.0, 31.0));
  // timeSamples only, single sample (the Blender-export shape).
  lb.add_time_sample("xformOp:rotateXYZ", 0.0,
                     Value::MakeFloat3(-88.25f, 0.0f, 0.0f));
  // Authored default AND timeSamples: the default must win at default time.
  lb.add_property("xformOp:scale", Value::MakeFloat3(2.0f, 2.0f, 2.0f));
  lb.add_time_sample("xformOp:scale", 1.0, Value::MakeFloat3(5.0f, 5.0f, 5.0f));
  lb.end_prim();

  Stage stage = builder.Build();
  UsdPrim prim = stage.GetPrimAtPath("/Anim");
  assert(prim.IsValid());

  const Value* t = prim.GetPropertyValue("xformOp:translate");
  assert(!t && "DefaultTime must not consult time samples");
  t = prim.GetPropertyValueOrEarliestTimeSample("xformOp:translate");
  assert(t && "compatibility helper should resolve the earliest sample");
  const double* td = t->as_double3();
  assert(td && td[0] == 10.0 && td[1] == 20.0 && td[2] == 30.0 &&
         "should be the earliest sample");

  const Value* r = prim.GetPropertyValue("xformOp:rotateXYZ");
  assert(!r && "single time sample is not a DefaultTime value");
  r = prim.GetPropertyValueOrEarliestTimeSample("xformOp:rotateXYZ");
  assert(r && "compatibility helper should resolve a single sample");
  const float* rf = r->as_float3();
  assert(rf && rf[0] == -88.25f && rf[1] == 0.0f && rf[2] == 0.0f);

  const Value* s = prim.GetPropertyValue("xformOp:scale");
  assert(s);
  const float* sf = s->as_float3();
  assert(sf && sf[0] == 2.0f && "authored default must win over samples");

  const Value* missing = prim.GetPropertyValue("xformOp:orient");
  assert(!missing && "absent property still resolves to null");

  std::cout << "  Explicit default-time semantics: PASSED" << std::endl;
}

// Regression: every PropNameId overload must be safe when handed an invalid
// id (e.g. GetPropNameTable().find("unknown") -> invalid). The interned-id
// lookup pass (geom-mesh/geom-xform/schema/stage accessors) relies on this.
void test_propnameid_invalid_id() {
  std::cout << "Testing PropNameId invalid-id safety..." << std::endl;

  StageBuilder builder;
  builder.SetDefaultPrim("World");
  LayerBuilder& lb = builder.GetLayerBuilder();

  lb.begin_prim("World", "Xform");
    lb.begin_prim("Mesh", "Mesh");
    lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
    lb.add_time_sample("points", 0.0, Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
    lb.add_time_sample("points", 1.0, Value::MakeFloat3Array(std::vector<float>{1, 1, 1, 2, 1, 1, 1, 2, 1}));
    lb.end_prim();
  lb.end_prim();

  Stage stage = builder.Build();
  UsdPrim mesh = stage.GetPrimAtPath("/World/Mesh");
  assert(mesh.IsValid());

  const PropNameId invalid = GetPropNameTable().find("doesNotExist");
  assert(!invalid.is_valid() && "fixture id must be invalid");

  // Known-good property id (must exist after the add_property above).
  const PropNameId points = GetPropNameTable().find("points");
  assert(points.is_valid() && "points must be interned by the builder");

  // String overloads keep working.
  assert(mesh.HasProperty("points"));
  assert(mesh.HasTimeSamples("points"));
  assert(!mesh.GetTimeSampleTimes("points").empty());

  // Invalid id: every PropNameId overload returns the safe empty value.
  assert(!mesh.HasProperty(invalid));
  assert(mesh.GetPropertyValue(invalid) == nullptr);
  assert(mesh.GetPropertyValueOrEarliestTimeSample(invalid) == nullptr);
  assert(mesh.EarliestTimeSampleValue(invalid) == nullptr);
  assert(!mesh.HasTimeSamples(invalid));
  assert(mesh.GetTimeSampleTimes(invalid).empty());
  assert(mesh.GetTimeSamples(invalid) == nullptr);
  assert(mesh.GetValueAtTime(invalid, 0.0) == nullptr);
  assert(mesh.GetInterpolatedValue(invalid, 0.0).is_empty());

  // Valid id still resolves after the invalid-id probes.
  assert(mesh.HasProperty(points));
  assert(mesh.GetPropertyValue(points) != nullptr);
  assert(mesh.HasTimeSamples(points));
  assert(mesh.GetTimeSamples(points) != nullptr);
  assert(mesh.GetValueAtTime(points, 0.5) != nullptr);
  assert(!mesh.GetInterpolatedValue(points, 0.5).is_empty());

  std::cout << "  PropNameId invalid-id safety: PASSED" << std::endl;
}

// Regression: the stage-level animation scans rewritten to iterate the root
// layer's flat prim array (no per-property traversal). HasTimeSamples must be
// true iff any prim has authored time samples; HasValueClips must be false
// for a plain stage and true for a stage whose prims carry `clips` metadata.
void test_stage_animation_scans() {
  std::cout << "Testing Stage::HasTimeSamples / HasValueClips..." << std::endl;

  // Static stage: neither.
  {
    StageBuilder builder;
    builder.SetDefaultPrim("World");
    LayerBuilder& lb = builder.GetLayerBuilder();
    lb.begin_prim("World", "Xform");
      lb.begin_prim("Mesh", "Mesh");
      lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
      lb.end_prim();
    lb.end_prim();
    Stage stage = builder.Build();
    assert(!stage.HasTimeSamples() && "static stage must have no time samples");
    assert(!stage.HasValueClips() && "static stage must have no value clips");
  }

  // Stage with authored time samples.
  {
    StageBuilder builder;
    builder.SetDefaultPrim("World");
    LayerBuilder& lb = builder.GetLayerBuilder();
    lb.begin_prim("World", "Xform");
      lb.begin_prim("Mesh", "Mesh");
      lb.add_property("points", Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
      lb.add_time_sample("points", 0.0, Value::MakeFloat3Array(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}));
      lb.add_time_sample("points", 1.0, Value::MakeFloat3Array(std::vector<float>{1, 1, 1, 2, 1, 1, 1, 2, 1}));
      lb.end_prim();
    lb.end_prim();
    Stage stage = builder.Build();
    assert(stage.HasTimeSamples() && "time-sampled stage must be detected");
    assert(!stage.HasValueClips() && "no clips metadata on this stage");
  }

  std::cout << "  Stage animation scans: PASSED" << std::endl;
}

// Regression: every name the schema/tydra accessors intern() at render time
// must be pre-registered by PropNameTable::register_common_names(). Run FIRST,
// before any fixture interning, so find() validity == registration
// completeness. A render-phase intern() MISS on the frozen table unfreezes it
// (property-index.cc), silently disabling the lock-free render path and
// racing concurrent lock-free readers.
void test_name_table_registration() {
  std::cout << "Testing name-table registration completeness..." << std::endl;

  // Names interned by the kId* accessors in geom-mesh.cc, geom-xform.cc,
  // geom-point-instancer.cc, stage.cc and tydra/next/render-converter.cc.
  static const char* kAccessorNames[] = {
      "accelerations",        "angularVelocities",  "clippingRange",
      "cornerIndices",        "cornerSharpnesses",  "creaseIndices",
      "creaseLengths",        "creaseSharpnesses",  "curveVertexCounts",
      "doubleSided",          "extent",             "faceVertexCounts",
      "faceVertexIndices",    "focalLength",        "focusDistance",
      "fStop",                "holeIndices",        "horizontalAperture",
      "ids",                  "inactiveIds",        "indices",
      "inputs:angle",         "inputs:color",       "inputs:colorTemperature",
      "inputs:diffuse",       "inputs:enableColorTemperature",
      "inputs:enableShadows", "inputs:exposure",    "inputs:height",
      "inputs:intensity",     "inputs:length",      "inputs:normalize",
      "inputs:radius",        "inputs:shadow:color",
      "inputs:shadow:distance", "inputs:shadow:enable",
      "inputs:shadow:falloff",  "inputs:shadow:falloffGamma",
      "inputs:shaping:cone:angle", "inputs:shaping:cone:softness",
      "inputs:shaping:focus", "inputs:shaping:focusTint",
      "inputs:shaping:ies:angleScale", "inputs:shaping:ies:file",
      "inputs:shaping:ies:normalize",  "inputs:specular",
      "inputs:texture:file",  "inputs:texture:format", "inputs:width",
      "invisibleIds",         "normals",            "orientation",
      "orientations",         "outputs:out",        "positions",
      "primvars:displayColor", "primvars:displayOpacity",
      "primvars:st",          "primvars:st:indices", "primvars:uv",
      "primvars:uv:indices",  "projection",         "protoIndices",
      "scales",               "shutter:close",      "shutter:open",
      "tetVertexIndices",     "velocities",         "verticalAperture",
      "visibility",           "widths",             "xformOp:orient",
      "xformOp:rotateX",      "xformOp:rotateXYZ",  "xformOp:rotateY",
      "xformOp:rotateZ",      "xformOp:scale",      "xformOp:transform",
      "xformOp:translate",    "xformOpOrder",
  };

  PropNameTable& table = GetPropNameTable();
  table.freeze();
#if defined(LIGHTUSD_ENABLE_THREAD)
  assert(table.is_frozen());
#endif
  for (const char* name : kAccessorNames) {
    const PropNameId id = table.find(name);
    if (!id.is_valid()) {
      std::cerr << "  UNREGISTERED accessor name: " << name << std::endl;
    }
    assert(id.is_valid() && "accessor name must be pre-registered");
  }
  table.unfreeze();

  std::cout << "  Name-table registration completeness: PASSED" << std::endl;
}

int main() {
  std::cout << "=== Stage Tests ===" << std::endl;

  test_name_table_registration();
  test_stage_builder();
  test_stage_prim_access();
  test_usd_prim();
  test_stage_traversal();
  test_stage_relationships();
  test_stage_stats();
  test_attribute_eval();
  test_attribute_eval_convenience();
  test_stage_move();
  test_default_time_timesample_fallback();
  test_propnameid_invalid_id();
  test_stage_animation_scans();

  std::cout << "\n=== All Stage tests PASSED ===" << std::endl;
  return 0;
}
