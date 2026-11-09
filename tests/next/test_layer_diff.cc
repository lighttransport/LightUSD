// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Layer / PrimSpec diff tests for src/next/diff/layer-diff.{hh,cc}:
// identical layers, value changes, ULP/eps tolerance, prim add/remove,
// metadata gating (compareMetadata), array + timeSample diffs, fuzzy asset
// paths, and the text/JSON renderer output shape.

#include "next/diff/layer-diff.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/reader/usda-reader.hh"
#include "next/types/value.hh"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace lightusd::next;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << "\n"; ++g_fail; }      \
    else { std::cout << "  ok: " << msg << "\n"; }                          \
  } while (0)

using PsDiffs = std::unordered_map<std::string, PrimSpecDiff>;
using PropDiffs = std::unordered_map<std::string, PropDiff>;

static bool has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static bool hasReason(const PropDiffs& pd, const std::string& path,
                      const std::string& prop, const std::string& reason) {
  auto it = pd.find(path);
  if (it == pd.end()) return false;
  for (const auto& m : it->second.modifiedPropDetails) {
    if (m.name == prop && has(m.reasons, reason)) return true;
  }
  return false;
}

static bool hasPrimReason(const PsDiffs& diffs, const std::string& path,
                          const std::string& reason) {
  const size_t slash = path.rfind('/');
  const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
  const std::string name = path.substr(slash + 1);
  const auto it = diffs.find(parent);
  if (it == diffs.end()) return false;
  for (const auto& detail : it->second.modifiedDetails) {
    if (detail.name == name && has(detail.reasons, reason)) return true;
  }
  return false;
}

// Build a tiny scene through the Layer authoring API. `mutate` tweaks the
// second copy.
struct SceneOpts {
  double radius = 1.0;
  float fval = 1.0f;
  std::vector<float> points = {1.0f, 2.0f, 3.0f};
  std::string assetPath = "./textures/wood.png";
  std::string kind = "component";
  std::string ballType = "Sphere";
  bool withExtraChild = false;
  bool withExtraRoot = false;
  double sampleValue10 = 5.0;
  double sampleTime1 = 10.0;
  size_t sampleCount = 2;
  double metersPerUnit = 0.01;
};

static Layer BuildScene(const SceneOpts& o) {
  Layer layer;

  uint32_t root = layer.define_prim_at_path("/Root", "Xform");
  {
    PrimSpec* p = layer.prim(root);
    p->meta().kind() = o.kind;
  }

  uint32_t ball = layer.define_prim_at_path("/Root/Ball", o.ballType);
  {
    PrimSpec* p = layer.prim(ball);
    p->add_property("radius", Value(o.radius));
    p->add_property("fval", Value(o.fval));
    p->add_property("points", Value::MakeFloatArray(o.points));
    p->add_property("tex", Value::MakeAssetPath(o.assetPath));

    // Time-sampled attribute "anim" (declared slot + samples).
    PropNameId anim = GetPropNameTable().intern("anim");
    p->add_property_slot(anim, TypeId::Double, PropSlot::kFlagTimeSampled);
    p->add_time_sample(anim, o.sampleTime1, Value(o.sampleValue10));
    if (o.sampleCount > 1) {
      p->add_time_sample(anim, 20.0, Value(7.0));
    }

    p->add_relationship("material:binding", Path("/Root/Mat"));
  }

  if (o.withExtraChild) {
    layer.define_prim_at_path("/Root/Extra", "Scope");
  }
  if (o.withExtraRoot) {
    layer.define_prim_at_path("/Other", "Xform");
  }

  layer.meta().metersPerUnit = o.metersPerUnit;
  layer.finalize();
  return layer;
}

static void RunDiff(const Layer& a, const Layer& b, PsDiffs& ps, PropDiffs& pp,
                    const DiffOptions& opts = {},
                    LayerMetaDiff* lm = nullptr) {
  ps.clear();
  pp.clear();
  Diff(a, b, ps, pp, opts, lm);
}

// ---------------------------------------------------------------------------

static void test_identical_api_layers() {
  std::cout << "[identical layers (API-built)]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  LayerMetaDiff lm;
  RunDiff(a, b, ps, pp, {}, &lm);
  CHECK(ps.empty(), "no primspec diffs");
  CHECK(pp.empty(), "no property diffs");
  CHECK(!lm.changed(), "no layer metadata diffs");
  CHECK(DiffToText(a, b) == "No differences found.\n",
        "DiffToText reports no differences");
}

static void test_identical_parsed_layers() {
  std::cout << "[identical layers (parsed USDA)]\n";
  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Z"
)

def Xform "World" (
    kind = "assembly"
)
{
    def Mesh "Quad"
    {
        float3[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        double anim.timeSamples = { 1: 0.5, 2: 1.5 }
    }
}
)";
  LoadResult ra = LoadUSDAFromString(usda);
  LoadResult rb = LoadUSDAFromString(usda);
  CHECK(ra.success && rb.success, "USDA parses");
  if (!ra.success || !rb.success) return;
  const Layer* la = ra.stage.GetRootLayer();
  const Layer* lb = rb.stage.GetRootLayer();
  CHECK(la && lb, "root layers present");
  if (!la || !lb) return;

  PsDiffs ps;
  PropDiffs pp;
  LayerMetaDiff lm;
  RunDiff(*la, *lb, ps, pp, {}, &lm);
  CHECK(ps.empty() && pp.empty() && !lm.changed(),
        "parsed layers self-compare clean");
}

static void test_value_change() {
  std::cout << "[value change]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.radius = 2.0;
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(pp.count("/Root/Ball") == 1, "property diff at /Root/Ball");
  CHECK(hasReason(pp, "/Root/Ball", "radius", "value"),
        "radius modified with reason 'value'");
  // Matching legacy tydra semantics: a pure property change is reported under
  // the prim's own path only; it does NOT mark ancestor PrimSpecs modified.
  CHECK(ps.empty(), "pure property change adds no PrimSpec diffs");

  const std::string text = DiffToText(a, b, "a.usda", "b.usda");
  CHECK(text.find("--- a.usda") != std::string::npos, "text has --- header");
  CHECK(text.find("~ /Root/Ball.radius (Property modified: value)") !=
            std::string::npos,
        "text lists modified property with reason");
}

static void test_ulp_tolerance() {
  std::cout << "[ulps/eps tolerance]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.fval = std::nextafter(1.0f, 2.0f);  // exactly 1 float ULP away
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);  // default floatUlps = 1
  CHECK(pp.empty(), "1-ULP float difference absorbed by default tolerance");

  DiffOptions exact;
  exact.floatUlps = 0;
  exact.doubleUlps = 0;
  RunDiff(a, b, ps, pp, exact);
  CHECK(hasReason(pp, "/Root/Ball", "fval", "value"),
        "1-ULP float difference reported with ulps=0");

  // Absolute epsilon: a large-ULP but small-absolute difference.
  SceneOpts o2;
  Layer c = BuildScene(o2);
  o2.radius = 1.0 + 1e-7;  // many double ULPs
  Layer d = BuildScene(o2);
  RunDiff(c, d, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "radius", "value"),
        "1e-7 double delta is a diff under pure ULP");
  DiffOptions eps;
  eps.absEps = 1e-6;
  RunDiff(c, d, ps, pp, eps);
  CHECK(pp.empty(), "1e-7 double delta absorbed by absEps=1e-6");
}

static void test_prim_add_remove() {
  std::cout << "[prim add/remove]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.withExtraChild = true;
  o.withExtraRoot = true;
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(ps.count("/") == 1 && has(ps["/"].addedPS, "Other"),
        "added root prim reported under /");
  CHECK(ps.count("/Root") == 1 && has(ps["/Root"].addedPS, "Extra"),
        "added child prim reported under /Root");

  // Reverse direction: deleted.
  RunDiff(b, a, ps, pp);
  CHECK(ps.count("/") == 1 && has(ps["/"].deletedPS, "Other"),
        "removed root prim reported under /");
  CHECK(ps.count("/Root") == 1 && has(ps["/Root"].deletedPS, "Extra"),
        "removed child prim reported under /Root");
}

static void test_type_change() {
  std::cout << "[prim type change]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.ballType = "Cube";
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  bool found = false;
  if (ps.count("/Root")) {
    for (const auto& m : ps["/Root"].modifiedDetails) {
      if (m.name == "Ball" && has(m.reasons, "typeName")) found = true;
    }
  }
  CHECK(found, "typeName change reported in modifiedDetails reasons");
}

static void test_metadata_gating() {
  std::cout << "[metadata diff gated by compareMetadata]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.kind = "group";
  o.metersPerUnit = 1.0;
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  LayerMetaDiff lm;
  RunDiff(a, b, ps, pp, {}, &lm);
  bool kindFound = false;
  if (ps.count("/")) {
    for (const auto& m : ps["/"].modifiedDetails) {
      if (m.name == "Root" && has(m.reasons, "meta:kind")) kindFound = true;
    }
  }
  CHECK(kindFound, "kind change reported as meta:kind");
  CHECK(lm.changed() && has(lm.changedFields, "~metersPerUnit"),
        "metersPerUnit change reported in layer meta diff");

  DiffOptions noMeta;
  noMeta.compareMetadata = false;
  RunDiff(a, b, ps, pp, noMeta, &lm);
  CHECK(ps.empty() && pp.empty(), "no diffs with compareMetadata=false");
  CHECK(!lm.changed(), "layer meta diff suppressed with compareMetadata=false");
}

static void test_authored_empty_metadata_diff() {
  std::cout << "[authored-empty metadata diff]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  Layer b = BuildScene(o);
  b.meta().defaultPrim_set = true;
  b.meta().rootPrimOrder_set = true;
  PrimSpec* root = b.prim_at_path_mutable("/Root");
  CHECK(root != nullptr, "root prim available for authored-state mutation");
  if (!root) return;
  root->meta().setPrimOrderAuthored();
  root->meta().setPropertyOrderAuthored();
  StringListOps& variants = root->meta().variantSetNameEdits();
  variants.authored = true;
  variants.is_explicit = true;
  root->meta().setApiSchemasAuthored();
  StringListOps& api_schemas = root->meta().apiSchemaEdits();
  api_schemas.authored = true;
  api_schemas.is_explicit = true;

  PsDiffs ps;
  PropDiffs pp;
  LayerMetaDiff lm;
  RunDiff(a, b, ps, pp, {}, &lm);
  CHECK(has(lm.changedFields, "~defaultPrim"),
        "empty authored defaultPrim differs from unauthored");
  CHECK(has(lm.changedFields, "~primOrder"),
        "empty authored root primOrder differs from unauthored");
  bool primOrder = false;
  bool propertyOrder = false;
  bool variantSetNames = false;
  bool apiSchemas = false;
  if (ps.count("/")) {
    for (const auto& modified : ps["/"].modifiedDetails) {
      if (modified.name != "Root") continue;
      primOrder = has(modified.reasons, "meta:primOrder");
      propertyOrder = has(modified.reasons, "meta:propertyOrder");
      variantSetNames = has(modified.reasons, "meta:variantSetNames");
      apiSchemas = has(modified.reasons, "meta:apiSchemasListOp");
    }
  }
  CHECK(primOrder && propertyOrder && variantSetNames && apiSchemas,
        "prim authored-empty order/list-op states are diff-visible");
}

static void test_array_diff() {
  std::cout << "[array diff]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.points = {1.0f, 2.0f, 4.0f};
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "points", "value"),
        "array element change reported with reason 'value'");

  // Array size change.
  o.points = {1.0f, 2.0f};
  Layer c = BuildScene(o);
  RunDiff(a, c, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "points", "value"),
        "array size change reported with reason 'value'");

  // 1-ULP perturbed array element is within default tolerance.
  SceneOpts o2;
  Layer d = BuildScene(o2);
  o2.points[1] = std::nextafter(2.0f, 3.0f);
  Layer e = BuildScene(o2);
  RunDiff(d, e, ps, pp);
  CHECK(pp.empty(), "1-ULP array element difference absorbed");
}

static void test_timesample_diff() {
  std::cout << "[timeSample diff]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.sampleValue10 = 6.0;
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "anim", "timeSamples"),
        "sample value change reported with reason 'timeSamples'");

  // Sample count change.
  SceneOpts o2;
  Layer c = BuildScene(o2);
  o2.sampleCount = 1;
  Layer d = BuildScene(o2);
  RunDiff(c, d, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "anim", "timeSamples"),
        "sample count change reported with reason 'timeSamples'");

  // Time axis: a shifted time is a diff...
  SceneOpts o3;
  Layer e = BuildScene(o3);
  o3.sampleTime1 = 10.5;
  Layer f = BuildScene(o3);
  RunDiff(e, f, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "anim", "timeSamples"),
        "sample time change reported with reason 'timeSamples'");
  // ...but a time within timeUlps is not.
  SceneOpts o4;
  Layer g = BuildScene(o4);
  o4.sampleTime1 = std::nextafter(10.0, 11.0);  // 1 double ULP (timeUlps=2)
  Layer h = BuildScene(o4);
  RunDiff(g, h, ps, pp);
  CHECK(pp.empty(), "1-ULP sample-time difference absorbed by timeUlps");
}

static void test_fuzzy_asset_paths() {
  std::cout << "[fuzzy asset paths]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.assetPath = "wood.png";  // leaf of "./textures/wood.png"
  Layer b = BuildScene(o);

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(pp.empty(), "leaf-equivalent asset paths equal with fuzzyAssetPaths");

  DiffOptions strict;
  strict.fuzzyAssetPaths = false;
  RunDiff(a, b, ps, pp, strict);
  CHECK(hasReason(pp, "/Root/Ball", "tex", "value"),
        "asset path prefix difference reported with fuzzyAssetPaths=false");

  // Different leaf is always a diff.
  o.assetPath = "stone.png";
  Layer c = BuildScene(o);
  RunDiff(a, c, ps, pp);
  CHECK(hasReason(pp, "/Root/Ball", "tex", "value"),
        "different asset leaf reported even with fuzzyAssetPaths");
}

static void test_property_add_remove_and_relationship() {
  std::cout << "[property add/remove + relationship]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  Layer b = BuildScene(o);
  {
    PrimSpec* p = b.prim_at_path_mutable("/Root/Ball");
    p->add_property("newProp", Value(int32_t(42)));
    p->set_relationship_targets("material:binding", {Path("/Root/OtherMat")});
  }

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(a, b, ps, pp);
  CHECK(pp.count("/Root/Ball") == 1 &&
            has(pp["/Root/Ball"].addedProps, "newProp"),
        "added property reported");
  CHECK(hasReason(pp, "/Root/Ball", "material:binding", "targets"),
        "relationship target change reported with reason 'targets'");

  RunDiff(b, a, ps, pp);
  CHECK(pp.count("/Root/Ball") == 1 &&
            has(pp["/Root/Ball"].deletedProps, "newProp"),
        "removed property reported");
}

static void test_extension_field_diff() {
  std::cout << "[typed extension fields]\n";
  SceneOpts opts;
  Layer a = BuildScene(opts);
  Layer b = BuildScene(opts);
  b.meta().unknownFields.push_back(
      DecodedExtensionField{"layerExt", Value(int32_t(1)), false, {}});
  PrimSpec* ball = b.prim_at_path_mutable("/Root/Ball");
  ball->meta().unknownFields().push_back(
      DecodedExtensionField{"primExt", Value(std::string("raw")), true,
                          "\"raw\""});
  ball->ensure_property_meta("radius").unknownFields.push_back(
      DecodedExtensionField{"propExt", Value(true), false, {}});

  PsDiffs ps;
  PropDiffs pp;
  LayerMetaDiff lm;
  RunDiff(a, b, ps, pp, {}, &lm);
  CHECK(has(lm.changedFields, "~extensionFields"),
        "layer extension-field difference reported");
  CHECK(hasPrimReason(ps, "/Root/Ball", "meta:extensionFields"),
        "prim extension-field difference reported");
  CHECK(hasReason(pp, "/Root/Ball", "radius", "meta:extensionFields"),
        "property extension-field difference reported");
}

static void test_json_output() {
  std::cout << "[JSON renderer]\n";
  SceneOpts o;
  Layer a = BuildScene(o);
  o.radius = 2.0;
  o.withExtraRoot = true;
  o.metersPerUnit = 1.0;
  Layer b = BuildScene(o);

  const std::string json = DiffToJSON(a, b, "left.usda", "right.usda");
  CHECK(json.find("\"comparison\"") != std::string::npos, "has comparison");
  CHECK(json.find("\"left\": \"left.usda\"") != std::string::npos,
        "has left name");
  CHECK(json.find("\"primspec_diffs\"") != std::string::npos,
        "has primspec_diffs");
  CHECK(json.find("\"property_diffs\"") != std::string::npos,
        "has property_diffs");
  CHECK(json.find("\"layer_meta_diff\"") != std::string::npos,
        "has layer_meta_diff");
  CHECK(json.find("\"modified_details\"") != std::string::npos,
        "has modified_details");
  CHECK(json.find("\"reasons\":[\"value\"]") != std::string::npos,
        "property modified_details carries reasons");
  CHECK(json.find("~metersPerUnit") != std::string::npos,
        "layer meta change listed");
  CHECK(json.find("\"Other\"") != std::string::npos,
        "added root prim listed");

  // Identical layers still emit the full JSON shape.
  SceneOpts o2;
  Layer c = BuildScene(o2);
  Layer d = BuildScene(o2);
  const std::string emptyJson = DiffToJSON(c, d);
  CHECK(emptyJson.find("\"primspec_diffs\"") != std::string::npos &&
            emptyJson.find("\"layer_meta_diff\"") != std::string::npos,
        "empty diff JSON keeps the shape");
}

// Deep per-field variant differentials: variant-INNER edits (properties,
// unknown metadata) must surface as granular reasons instead of comparing
// only set/option names.
static void test_variant_deep_diff() {
  std::cout << "[variant deep diff]\n";
  auto make_layer = [](const char* tag, const char* value) {
    std::string usda =
        "#usda 1.0\n"
        "def Xform \"P\" (prepend variantSets = \"look\") {\n"
        "    variantSet \"look\" = {\n"
        "        \"red\" (\n"
        "            customPipelineTag = \"";
    usda += tag;
    usda +=
        "\"\n"
        "        ) {\n"
        "            int c = ";
    usda += value;
    usda +=
        "\n"
        "        }\n"
        "    }\n"
        "}\n";
    return LoadUSDAFromString(usda);
  };
  LoadResult base = make_layer("hero", "1");
  LoadResult same = make_layer("hero", "1");
  LoadResult prop_changed = make_layer("hero", "5");
  LoadResult meta_changed = make_layer("bg", "1");
  CHECK(base.success && same.success && prop_changed.success &&
            meta_changed.success,
        "variant layers parse");

  const auto reason_contains = [](const PsDiffs& ps, const char* needle) {
    for (const auto& entry : ps) {
      for (const ModifiedPrimSpec& m : entry.second.modifiedDetails) {
        for (const std::string& r : m.reasons) {
          if (r.find(needle) != std::string::npos) return true;
        }
      }
    }
    return false;
  };

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(*base.stage.GetRootLayer(), *same.stage.GetRootLayer(), ps, pp);
  CHECK(ps.empty() && pp.empty(), "identical variants self-compare clean");

  RunDiff(*base.stage.GetRootLayer(), *prop_changed.stage.GetRootLayer(), ps,
          pp);
  CHECK(reason_contains(ps, "variantSets:look/red:properties"),
        "variant-inner property edit produces a granular differential");

  RunDiff(*base.stage.GetRootLayer(), *meta_changed.stage.GetRootLayer(), ps,
          pp);
  CHECK(reason_contains(ps, "variantSets:look/red:unknownMeta"),
        "variant-option unknown-metadata edit produces a differential");
}

// Deleted/blocked-value and dictionary type-conflict differentials.
static void test_blocked_and_type_conflict_diff() {
  std::cout << "[blocked value + dict type-conflict diff]\n";
  LoadResult with_value = LoadUSDAFromString(
      "#usda 1.0\ndef Xform \"P\" { float v = 1.5 }\n");
  LoadResult blocked = LoadUSDAFromString(
      "#usda 1.0\ndef Xform \"P\" { float v = None }\n");
  CHECK(with_value.success && blocked.success, "layers parse");

  PsDiffs ps;
  PropDiffs pp;
  RunDiff(*with_value.stage.GetRootLayer(), *blocked.stage.GetRootLayer(), ps,
          pp);
  bool saw_blocked = false;
  for (const auto& entry : pp) {
    for (const auto& m : entry.second.modifiedPropDetails) {
      for (const std::string& r : m.reasons) {
        if (r == "blocked") saw_blocked = true;
      }
    }
  }
  CHECK(saw_blocked, "value -> block transition reports `blocked`");

  LoadResult dict_nested = LoadUSDAFromString(
      "#usda 1.0\n"
      "def Xform \"P\" (customData = { dictionary k = { int a = 1 } }) {}\n");
  LoadResult dict_scalar = LoadUSDAFromString(
      "#usda 1.0\n"
      "def Xform \"P\" (customData = { int k = 3 }) {}\n");
  CHECK(dict_nested.success && dict_scalar.success, "dict layers parse");
  RunDiff(*dict_nested.stage.GetRootLayer(), *dict_scalar.stage.GetRootLayer(),
          ps, pp);
  bool saw_conflict = false;
  for (const auto& entry : ps) {
    for (const ModifiedPrimSpec& m : entry.second.modifiedDetails) {
      for (const std::string& r : m.reasons) {
        if (r.find("customData(type-conflict)") != std::string::npos) {
          saw_conflict = true;
        }
      }
    }
  }
  CHECK(saw_conflict,
        "dict-vs-scalar key reports a type-conflict differential");
}

int main() {
  test_identical_api_layers();
  test_identical_parsed_layers();
  test_value_change();
  test_ulp_tolerance();
  test_prim_add_remove();
  test_type_change();
  test_metadata_gating();
  test_authored_empty_metadata_diff();
  test_array_diff();
  test_timesample_diff();
  test_fuzzy_asset_paths();
  test_property_add_remove_and_relationship();
  test_extension_field_diff();
  test_variant_deep_diff();
  test_blocked_and_type_conflict_diff();
  test_json_output();

  if (g_fail) {
    std::cerr << g_fail << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "All layer-diff tests passed\n";
  return 0;
}
