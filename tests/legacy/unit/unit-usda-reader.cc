// SPDX-License-Identifier: Apache 2.0
// USDA reader unit tests: parse USDA via LoadUSDAFromMemory, then verify
// that the Stage contains correctly-typed values.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usda-reader.h"
#include "lightusd.hh"
#include "core/prim.hh"
#include "value-types.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdSkel.hh"
#include "usdPhysics.hh"
#include "core/collection-api.hh"
#include "core/model-scope.hh"
#include "math-util.inc"

#include <cstring>
#include <sstream>

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

// ===========================================================================
// Basic Types (3)
// ===========================================================================

void usda_reader_scalar_int_float_double_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom int myInt = 42
    custom float myFloat = 3.14
    custom double myDouble = 2.718281828
    custom bool myBool = true
    custom half myHalf = 1.5
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // int
  {
    auto it = props.find("myInt");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      TEST_CHECK(it->second.is_attribute());
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<int>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value() == 42); }
    }
  }

  // float
  {
    auto it = props.find("myFloat");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<float>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(math::is_close(v.value(), 3.14f)); }
    }
  }

  // double
  {
    auto it = props.find("myDouble");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<double>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(math::is_close(v.value(), 2.718281828)); }
    }
  }

  // bool
  {
    auto it = props.find("myBool");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<bool>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value() == true); }
    }
  }

  // half
  {
    auto it = props.find("myHalf");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::half>();
      TEST_CHECK(v.has_value());
    }
  }
}

void usda_reader_scalar_string_token_path_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom string myString = "hello world"
    custom token myToken = "myTokenValue"
    custom asset myAsset = @./texture.png@
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // string
  {
    auto it = props.find("myString");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::string>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value() == "hello world"); }
    }
  }

  // token
  {
    auto it = props.find("myToken");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::token>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value().str() == "myTokenValue"); }
    }
  }

  // asset
  {
    auto it = props.find("myAsset");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::AssetPath>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value().GetAssetPath() == "./texture.png"); }
    }
  }
}

void usda_reader_vector_matrix_types_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom float2 myFloat2 = (1.0, 2.0)
    custom float3 myFloat3 = (1.0, 2.0, 3.0)
    custom double3 myDouble3 = (10.0, 20.0, 30.0)
    custom color3f myColor = (0.5, 0.6, 0.7)
    custom quatf myQuat = (1.0, 0.0, 0.0, 0.0)
    custom matrix4d myMatrix = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // float2
  {
    auto it = props.find("myFloat2");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::float2>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(math::is_close(v.value()[0], 1.0f));
        TEST_CHECK(math::is_close(v.value()[1], 2.0f));
      }
    }
  }

  // float3
  {
    auto it = props.find("myFloat3");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::float3>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(math::is_close(v.value()[0], 1.0f));
        TEST_CHECK(math::is_close(v.value()[1], 2.0f));
        TEST_CHECK(math::is_close(v.value()[2], 3.0f));
      }
    }
  }

  // double3
  {
    auto it = props.find("myDouble3");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::double3>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(math::is_close(v.value()[0], 10.0));
        TEST_CHECK(math::is_close(v.value()[1], 20.0));
        TEST_CHECK(math::is_close(v.value()[2], 30.0));
      }
    }
  }

  // color3f
  {
    auto it = props.find("myColor");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::color3f>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(math::is_close(v.value()[0], 0.5f));
        TEST_CHECK(math::is_close(v.value()[1], 0.6f));
        TEST_CHECK(math::is_close(v.value()[2], 0.7f));
      }
    }
  }

  // quatf
  {
    auto it = props.find("myQuat");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::quatf>();
      TEST_CHECK(v.has_value());
    }
  }

  // matrix4d
  {
    auto it = props.find("myMatrix");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<lightusd::legacy::value::matrix4d>();
      TEST_CHECK(v.has_value());
      if (v) {
        // Identity matrix: m[0][0] = 1, m[1][1] = 1, etc.
        TEST_CHECK(math::is_close(v.value().m[0][0], 1.0));
        TEST_CHECK(math::is_close(v.value().m[1][1], 1.0));
        TEST_CHECK(math::is_close(v.value().m[2][2], 1.0));
        TEST_CHECK(math::is_close(v.value().m[3][3], 1.0));
        TEST_CHECK(math::is_close(v.value().m[0][1], 0.0));
      }
    }
  }
}

// ===========================================================================
// Arrays (3)
// ===========================================================================

void usda_reader_array_int_float_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom int[] myInts = [1, 2, 3, 4, 5]
    custom float[] myFloats = [1.1, 2.2, 3.3]
    custom double[] myDoubles = [10.5, 20.5]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // int[]
  {
    auto it = props.find("myInts");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<int>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 5);
        TEST_CHECK(v.value()[0] == 1);
        TEST_CHECK(v.value()[4] == 5);
      }
    }
  }

  // float[]
  {
    auto it = props.find("myFloats");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<float>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 3);
        TEST_CHECK(math::is_close(v.value()[0], 1.1f));
      }
    }
  }

  // double[]
  {
    auto it = props.find("myDoubles");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<double>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 2);
        TEST_CHECK(math::is_close(v.value()[0], 10.5));
      }
    }
  }
}

void usda_reader_array_string_token_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom string[] myStrings = ["hello", "world"]
    custom token[] myTokens = ["tok1", "tok2", "tok3"]
    custom int[] emptyArray = []
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // string[]
  {
    auto it = props.find("myStrings");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<std::string>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 2);
        TEST_CHECK(v.value()[0] == "hello");
        TEST_CHECK(v.value()[1] == "world");
      }
    }
  }

  // token[]
  {
    auto it = props.find("myTokens");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<lightusd::legacy::value::token>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 3);
        TEST_CHECK(v.value()[0].str() == "tok1");
      }
    }
  }

  // empty array
  {
    auto it = props.find("emptyArray");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<int>>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().size() == 0);
      }
    }
  }
}

void usda_reader_array_vector_types_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "test" {
    point3f[] points = [(1, 2, 3), (4, 5, 6)]
    normal3f[] normals = [(0, 0, 1), (0, 1, 0)]
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const GeomMesh *mesh = (*result)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  // point3f[]
  auto pts = mesh->get_points();
  TEST_CHECK(pts.size() == 2);
  if (pts.size() == 2) {
    TEST_CHECK(math::is_close(pts[0].x, 1.0f));
    TEST_CHECK(math::is_close(pts[1].x, 4.0f));
  }

  // normal3f[]
  auto norms = mesh->get_normals();
  TEST_CHECK(norms.size() == 2);
  if (norms.size() == 2) {
    TEST_CHECK(math::is_close(norms[0].z, 1.0f));
    TEST_CHECK(math::is_close(norms[1].y, 1.0f));
  }
}

// ===========================================================================
// TimeSamples (4)
// ===========================================================================

void usda_reader_timesamples_scalar_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "test" {
    double3 xformOp:translate.timeSamples = {
        1: (0, 0, 0),
        2: (1, 2, 3),
        3: (10, 20, 30),
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Xform *xform = (*result)->as<Xform>();
  TEST_CHECK(xform != nullptr);
  if (!xform) return;

  TEST_CHECK(xform->xformOps.size() == 1);
  if (xform->xformOps.empty()) return;

  // The xformOp should have timeSamples
  const auto &op = xform->xformOps[0];
  TEST_CHECK(op.is_timesamples());
}

void usda_reader_timesamples_array_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "test" {
    point3f[] points.timeSamples = {
        0: [(0, 0, 0), (1, 0, 0), (1, 1, 0)],
        1: [(0, 0, 1), (1, 0, 1), (1, 1, 1)],
    }
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const GeomMesh *mesh = (*result)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  // points should have timeSamples, not a default value
  auto pts_val = mesh->points.get_value();
  TEST_CHECK(pts_val.has_value());
  if (pts_val) { TEST_CHECK(pts_val.value().is_timesamples()); }
}

void usda_reader_timesamples_blocked_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom float myVal.timeSamples = {
        1: 10.0,
        2: None,
        3: 30.0,
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;
  auto it = props.find("myVal");
  TEST_CHECK(it != props.end());
  if (it != props.end()) {
    const auto &attr = it->second.get_attribute();
    TEST_CHECK(attr.has_timesamples());
  }
}

void usda_reader_timesamples_token_enum_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "test" {
    token visibility.timeSamples = {
        0: "inherited",
        10: "invisible",
        20: "inherited",
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Xform *xform = (*result)->as<Xform>();
  TEST_CHECK(xform != nullptr);
  if (!xform) return;

  TEST_CHECK(xform->visibility.get_value().is_timesamples());
}

// ===========================================================================
// Connections & Relationships (2)
// ===========================================================================

void usda_reader_attribute_connection_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat" {
    token outputs:surface.connect = </mat/surf.outputs:surface>

    def Shader "surf" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
        token outputs:surface
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/mat", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Material *mat = (*result)->as<Material>();
  TEST_CHECK(mat != nullptr);
  if (!mat) return;

  // Material should have surface output connection
  TEST_CHECK(mat->surface.authored());
  if (mat->surface.authored()) {
    auto paths = mat->surface.get_connections();
    TEST_CHECK(paths.size() == 1);
  }

  // Check shader child
  TEST_CHECK((*result)->children().size() >= 1);
  if ((*result)->children().empty()) return;

  const Shader *shader = (*result)->children()[0].as<Shader>();
  TEST_CHECK(shader != nullptr);
  if (!shader) return;
  TEST_CHECK(shader->info_id == "UsdPreviewSurface");
}

void usda_reader_relationship_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh" {
    rel material:binding = </mat>
    point3f[] points = [(0, 0, 0)]
    int[] faceVertexCounts = [1]
    int[] faceVertexIndices = [0]
}

def Material "mat" {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/mesh", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const GeomMesh *mesh = (*result)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  // Check material:binding relationship
  TEST_CHECK(mesh->materialBinding.has_value());
}

// ===========================================================================
// Metadata (2)
// ===========================================================================

void usda_reader_prim_metadata_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" (
    doc = "A test scope"
    kind = "component"
    customData = {
        string author = "lightusd"
        int version = 1
    }
) {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;

  // doc
  TEST_CHECK(prim->metas().has_doc());
  if (prim->metas().has_doc()) {
    TEST_CHECK(prim->metas().get_doc().value == "A test scope");
  }

  // kind
  TEST_CHECK(prim->metas().has_kind());
  if (prim->metas().has_kind()) {
    TEST_CHECK(prim->metas().get_kind_enum() == Kind::Component);
  }

  // customData
  TEST_CHECK(prim->metas().has_customData());
  if (prim->metas().has_customData()) {
    Dictionary cd = prim->metas().get_customData();
    TEST_CHECK(cd.size() >= 2);
  }
}

void usda_reader_stage_metadata_test(void) {
  const char *usda = R"(#usda 1.0
(
    upAxis = "Z"
    metersPerUnit = 0.01
    defaultPrim = "root"
    startTimeCode = 1
    endTimeCode = 100
    doc = "stage doc"
)

def Xform "root" {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  const auto &m = stage.metas();

  // upAxis
  TEST_CHECK(m.upAxis.get_value() == Axis::Z);

  // metersPerUnit
  TEST_CHECK(math::is_close(m.metersPerUnit.get_value(), 0.01));

  // defaultPrim
  TEST_CHECK(m.defaultPrim.str() == "root");

  // timeCode range
  TEST_CHECK(math::is_close(m.startTimeCode.get_value(), 1.0));
  TEST_CHECK(math::is_close(m.endTimeCode.get_value(), 100.0));

  // doc
  TEST_CHECK(m.doc.value == "stage doc");
}

// ===========================================================================
// Variants & Composition (4)
// ===========================================================================

void usda_reader_variantset_basic_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "model" (
    variants = {
        string color = "red"
    }
    prepend variantSets = "color"
) {
    variantSet "color" = {
        "red" {
        }
        "blue" {
        }
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/model", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;

  // Check variant selection
  TEST_CHECK(prim->metas().variants.has_value());
  if (prim->metas().variants.has_value()) {
    const auto &vs = prim->metas().variants.value();
    auto it = vs.find("color");
    TEST_CHECK(it != vs.end());
    if (it != vs.end()) {
      TEST_CHECK(it->second == "red");
    }
  }
}

void usda_reader_variantset_with_properties_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "model" (
    variants = {
        string size = "small"
    }
    prepend variantSets = "size"
) {
    def Scope "Target" {
    }

    variantSet "size" = {
        "small" {
            custom double scale = 1.0
            rel target = </model/Target>
        }
        "large" {
            custom double scale = 10.0
        }
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/model", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const auto &variant_sets = (*result)->variantSets();
  auto vs_it = variant_sets.find("size");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) return;
  auto small_it = vs_it->second.variantSet.find("small");
  TEST_CHECK(small_it != vs_it->second.variantSet.end());
  if (small_it == vs_it->second.variantSet.end()) return;
  const auto &props = small_it->second.properties();
  auto rel_it = props.find("target");
  TEST_CHECK(rel_it != props.end());
  if (rel_it != props.end()) {
    TEST_CHECK(rel_it->second.is_relationship());
    if (rel_it->second.is_relationship()) {
      const Relationship &rel = rel_it->second.get_relationship();
      TEST_CHECK(rel.is_path());
      if (rel.is_path()) {
        TEST_CHECK(rel.targetPath.full_path_name() == "/model/Target");
      }
    }
  }
}

void usda_reader_class_inherits_test(void) {
  const char *usda = R"(#usda 1.0

class Scope "_class_base" {
    custom string label = "base"
}

def Scope "child" (
    prepend inherits = </_class_base>
) {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // Check class prim
  {
    auto result = stage.GetPrimAtPath(Path("/_class_base", ""));
    TEST_CHECK(bool(result));
    if (result) {
      const Scope *s = (*result)->as<Scope>();
      TEST_CHECK(s != nullptr);
      if (s) { TEST_CHECK(s->spec == Specifier::Class); }
    }
  }

  // Check child with inherits
  {
    auto result = stage.GetPrimAtPath(Path("/child", ""));
    TEST_CHECK(bool(result));
    if (result) {
      TEST_CHECK((*result)->metas().inherits.has_value());
    }
  }
}

void usda_reader_internal_reference_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "source" {
    custom string label = "original"
}

def Scope "target" (
    prepend references = </source>
) {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/target", ""));
  TEST_CHECK(bool(result));
  if (result) {
    TEST_CHECK((*result)->metas().references.has_value());
  }
}

// ===========================================================================
// Hierarchy & Specifiers (2)
// ===========================================================================

void usda_reader_nested_hierarchy_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "a" {
    def Xform "b" {
        def Xform "c" {
            def Scope "d" {
            }
        }
        def Scope "e" {
        }
    }
    def Scope "f" {
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // Root: a
  {
    auto r = stage.GetPrimAtPath(Path("/a", ""));
    TEST_CHECK(bool(r));
    if (r) {
      // a has 2 children: b, f
      TEST_CHECK((*r)->children().size() == 2);
    }
  }

  // 2nd level: a/b
  {
    auto r = stage.GetPrimAtPath(Path("/a/b", ""));
    TEST_CHECK(bool(r));
    if (r) {
      // b has 2 children: c, e
      TEST_CHECK((*r)->children().size() == 2);
    }
  }

  // 3rd level: a/b/c
  {
    auto r = stage.GetPrimAtPath(Path("/a/b/c", ""));
    TEST_CHECK(bool(r));
    if (r) {
      TEST_CHECK((*r)->children().size() == 1);
    }
  }

  // 4th level: a/b/c/d
  {
    auto r = stage.GetPrimAtPath(Path("/a/b/c/d", ""));
    TEST_CHECK(bool(r));
  }

  // Siblings: a/b/e
  {
    auto r = stage.GetPrimAtPath(Path("/a/b/e", ""));
    TEST_CHECK(bool(r));
  }

  // Sibling: a/f
  {
    auto r = stage.GetPrimAtPath(Path("/a/f", ""));
    TEST_CHECK(bool(r));
  }
}

void usda_reader_specifiers_def_over_class_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "defPrim" {
}

over Scope "overPrim" {
}

class Scope "classPrim" {
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // def
  {
    auto r = stage.GetPrimAtPath(Path("/defPrim", ""));
    TEST_CHECK(bool(r));
    if (r) {
      const Scope *s = (*r)->as<Scope>();
      TEST_CHECK(s != nullptr);
      if (s) { TEST_CHECK(s->spec == Specifier::Def); }
    }
  }

  // over
  {
    auto r = stage.GetPrimAtPath(Path("/overPrim", ""));
    TEST_CHECK(bool(r));
    if (r) {
      const Scope *s = (*r)->as<Scope>();
      TEST_CHECK(s != nullptr);
      if (s) { TEST_CHECK(s->spec == Specifier::Over); }
    }
  }

  // class
  {
    auto r = stage.GetPrimAtPath(Path("/classPrim", ""));
    TEST_CHECK(bool(r));
    if (r) {
      const Scope *s = (*r)->as<Scope>();
      TEST_CHECK(s != nullptr);
      if (s) { TEST_CHECK(s->spec == Specifier::Class); }
    }
  }
}

// ===========================================================================
// Edge Cases (3)
// ===========================================================================

void usda_reader_trailing_comma_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom int[] vals = [1, 2, 3,]
    # comment inside code
    custom float[] fvals = [
        1.0, # trailing comma and comment
        2.0,
        3.0, # last trailing comma
    ]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  {
    auto it = props.find("vals");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<int>>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value().size() == 3); }
    }
  }

  {
    auto it = props.find("fvals");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::vector<float>>();
      TEST_CHECK(v.has_value());
      if (v) { TEST_CHECK(v.value().size() == 3); }
    }
  }
}

void usda_reader_empty_prim_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "empty1" {
}

def Xform "empty2" {
}

def Scope "parent" {
    def Scope "emptyChild" {
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  TEST_CHECK(stage.root_prims().size() == 3);

  {
    auto r = stage.GetPrimAtPath(Path("/empty1", ""));
    TEST_CHECK(bool(r));
    if (r) {
      const Scope *s = (*r)->as<Scope>();
      TEST_CHECK(s != nullptr);
      if (s) { TEST_CHECK(s->props.empty()); }
      TEST_CHECK((*r)->children().empty());
    }
  }

  {
    auto r = stage.GetPrimAtPath(Path("/empty2", ""));
    TEST_CHECK(bool(r));
  }

  {
    auto r = stage.GetPrimAtPath(Path("/parent/emptyChild", ""));
    TEST_CHECK(bool(r));
  }
}

void usda_reader_unicode_and_special_strings_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "test" {
    custom string escaped = "line1\nline2\ttab"
    custom string withQuotes = "say \"hello\""
    custom string empty = ""
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/test", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Scope *scope = (*result)->as<Scope>();
  TEST_CHECK(scope != nullptr);
  if (!scope) return;
  const auto &props = scope->props;

  // escaped string
  {
    auto it = props.find("escaped");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::string>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().find('\n') != std::string::npos);
        TEST_CHECK(v.value().find('\t') != std::string::npos);
      }
    }
  }

  // quoted string
  {
    auto it = props.find("withQuotes");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::string>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().find('"') != std::string::npos);
      }
    }
  }

  // empty string
  {
    auto it = props.find("empty");
    TEST_CHECK(it != props.end());
    if (it != props.end()) {
      const auto &attr = it->second.get_attribute();
      auto v = attr.get_value<std::string>();
      TEST_CHECK(v.has_value());
      if (v) {
        TEST_CHECK(v.value().empty());
      }
    }
  }
}

// ===========================================================================
// Error Handling (2)
// ===========================================================================

void usda_reader_malformed_input_test(void) {
  // Missing header
  {
    const char *usda = "def Scope \"test\" { }";
    Stage stage;
    std::string warn, err;
    bool ok = parse_usda(usda, &stage, &warn, &err);
    TEST_CHECK(!ok);
  }

  // Invalid type name
  {
    const char *usda = R"(#usda 1.0
def InvalidTypeName123 "test" {
}
)";
    Stage stage;
    std::string warn, err;
    bool ok = parse_usda(usda, &stage, &warn, &err);
    // Parser may accept unknown types as Model prim, so just verify no crash
    (void)ok;
    TEST_CHECK(true);
  }

  // Empty input
  {
    const char *usda = "";
    Stage stage;
    std::string warn, err;
    bool ok = parse_usda(usda, &stage, &warn, &err);
    TEST_CHECK(!ok);
  }

  // Null bytes (just a single null)
  {
    Stage stage;
    std::string warn, err;
    uint8_t data[1] = {0};
    bool ok = LoadUSDAFromMemory(data, 1, "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
  }
}

void usda_reader_large_nesting_depth_test(void) {
  // Build a 50-level deep hierarchy
  std::string usda = "#usda 1.0\n\n";
  for (int i = 0; i < 50; i++) {
    for (int j = 0; j < i; j++) usda += "    ";
    usda += "def Scope \"level" + std::to_string(i) + "\" {\n";
  }
  for (int i = 49; i >= 0; i--) {
    for (int j = 0; j < i; j++) usda += "    ";
    usda += "}\n";
  }

  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda.c_str(), &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  // Verify we can reach the deepest level
  std::string path = "";
  for (int i = 0; i < 50; i++) {
    path += "/level" + std::to_string(i);
  }
  auto result = stage.GetPrimAtPath(Path(path, ""));
  TEST_CHECK(bool(result));
}

// ===========================================================================
// Typed schema reconstruction (USDA reader -> typed struct fields)
//
// These verify that authored properties land in the *typed* schema fields after
// USDA parsing (not just the generic props bag). `.authored()` on a typed field
// is the key signal: a property the reader fails to map stays unauthored.
// ===========================================================================

void usda_reader_geom_mesh_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh" {
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
    vector3f[] velocities = [(0.1, 0, 0), (0, 0.1, 0), (0, 0, 0.1), (0.1, 0.1, 0)]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/mesh", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const GeomMesh *mesh = (*result)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  TEST_CHECK(mesh->get_points().size() == 4);
  TEST_CHECK(mesh->get_faceVertexCounts().size() == 1);
  TEST_CHECK(mesh->get_faceVertexIndices().size() == 4);
  TEST_CHECK(mesh->normals.authored());
  // Regression: GeomMesh velocities must reconstruct into the typed field.
  TEST_CHECK(mesh->velocities.authored());
}

void usda_reader_geom_subset_schema_test(void) {
  const char *usda = R"(#usda 1.0

def GeomSubset "subset" {
    uniform token elementType = "face"
    uniform token familyName = "materialBind"
    int[] indices = [0, 2, 4]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/subset", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const GeomSubset *subset = (*result)->as<GeomSubset>();
  TEST_CHECK(subset != nullptr);
  if (!subset) return;

  TEST_CHECK(subset->elementType.get_value() == GeomSubset::ElementType::Face);
  lightusd::legacy::value::token fam;
  TEST_CHECK(subset->familyName.get_value(&fam));
  TEST_CHECK(fam.str() == "materialBind");
  TEST_CHECK(subset->indices.authored());
}

void usda_reader_geom_camera_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Camera "cam" {
    float focalLength = 35
    float focusDistance = 10
    float fStop = 2.8
    float horizontalAperture = 36
    float2 clippingRange = (0.1, 1000)
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/cam", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const GeomCamera *cam = (*result)->as<GeomCamera>();
  TEST_CHECK(cam != nullptr);
  if (!cam) return;

  auto chk = [](const TypedAttributeWithFallback<Animatable<float>> &a,
                float expect, const char *nm) {
    TEST_CHECK_(a.authored(), "camera.%s not authored", nm);
    float v = 0.0f;
    if (a.authored() && a.get_value().get_scalar(&v)) {
      TEST_CHECK_(math::is_close(v, expect), "camera.%s = %f, want %f", nm, v, expect);
    }
  };
  chk(cam->focalLength, 35.0f, "focalLength");
  chk(cam->focusDistance, 10.0f, "focusDistance");
  chk(cam->fStop, 2.8f, "fStop");
  chk(cam->horizontalAperture, 36.0f, "horizontalAperture");
  TEST_CHECK(cam->clippingRange.authored());
}

// Regression for the LIGHT_BASE_ATTRS reader fix: the shared LightAPI radiometric
// inputs must reconstruct for every light type, and light:filters must parse.
void usda_reader_light_schema_test(void) {
  const char *usda = R"(#usda 1.0

def SphereLight "light" {
    float inputs:radius = 2.0
    float inputs:intensity = 3.0
    float inputs:exposure = 1.5
    float inputs:diffuse = 0.7
    float inputs:specular = 0.3
    bool inputs:normalize = true
    bool inputs:enableColorTemperature = true
    float inputs:colorTemperature = 5000
    color3f inputs:color = (0.2, 0.4, 0.6)
    rel light:filters = [</Filter1>, </Filter2>]
}

def CylinderLight "clight" {
    float inputs:length = 5.0
    float inputs:radius = 1.0
    float inputs:intensity = 2.5
    color3f inputs:color = (1.0, 0.5, 0.25)
    float inputs:exposure = 0.5
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto sf = [](const TypedAttributeWithFallback<Animatable<float>> &a,
               float expect, const char *nm) {
    TEST_CHECK_(a.authored(), "%s dropped on read", nm);
    float v = 0.0f;
    if (a.authored() && a.get_value().get_scalar(&v)) {
      TEST_CHECK_(math::is_close(v, expect), "%s = %f, want %f", nm, v, expect);
    }
  };

  // SphereLight: full base set + radius + lightFilters.
  {
    auto result = stage.GetPrimAtPath(Path("/light", ""));
    TEST_CHECK(bool(result));
    if (result) {
      const SphereLight *L = (*result)->as<SphereLight>();
      TEST_CHECK(L != nullptr);
      if (L) {
        sf(L->intensity, 3.0f, "sphere.intensity");
        sf(L->exposure, 1.5f, "sphere.exposure");
        sf(L->diffuse, 0.7f, "sphere.diffuse");
        sf(L->specular, 0.3f, "sphere.specular");
        sf(L->colorTemperature, 5000.0f, "sphere.colorTemperature");
        sf(L->radius, 2.0f, "sphere.radius");
        TEST_CHECK(L->normalize.authored());
        TEST_CHECK(L->enableColorTemperature.authored());
        TEST_CHECK(L->color.authored());
        TEST_CHECK(L->lightFilters.authored());
        TEST_CHECK(L->lightFilters.get_targetPaths().size() == 2);
      }
    }
  }

  // CylinderLight: base color/intensity/exposure were dropped before the fix.
  {
    auto result = stage.GetPrimAtPath(Path("/clight", ""));
    TEST_CHECK(bool(result));
    if (result) {
      const CylinderLight *L = (*result)->as<CylinderLight>();
      TEST_CHECK(L != nullptr);
      if (L) {
        sf(L->intensity, 2.5f, "cylinder.intensity");
        sf(L->exposure, 0.5f, "cylinder.exposure");
        sf(L->length, 5.0f, "cylinder.length");
        sf(L->radius, 1.0f, "cylinder.radius");
        TEST_CHECK(L->color.authored());
      }
    }
  }
}

void usda_reader_skel_skeleton_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Skeleton "skel" {
    uniform token[] joints = ["Root", "Root/Hip"]
    uniform matrix4d[] bindTransforms = [
        ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
        ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1))
    ]
    uniform matrix4d[] restTransforms = [
        ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
        ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1))
    ]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/skel", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Skeleton *skel = (*result)->as<Skeleton>();
  TEST_CHECK(skel != nullptr);
  if (!skel) return;

  TEST_CHECK(skel->joints.authored());
  {
    std::vector<lightusd::legacy::value::token> joints;
    if (skel->joints.get_value(&joints)) {
      TEST_CHECK(joints.size() == 2);
    }
  }
  TEST_CHECK(skel->bindTransforms.authored());
  TEST_CHECK(skel->restTransforms.authored());
}

void usda_reader_skel_animation_schema_test(void) {
  const char *usda = R"(#usda 1.0

def SkelAnimation "anim" {
    uniform token[] joints = ["Root"]
    float3[] translations = [(0, 1, 0)]
    quatf[] rotations = [(1, 0, 0, 0)]
    half3[] scales = [(1, 1, 1)]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/anim", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const SkelAnimation *anim = (*result)->as<SkelAnimation>();
  TEST_CHECK(anim != nullptr);
  if (!anim) return;

  TEST_CHECK(anim->joints.authored());
  TEST_CHECK(anim->translations.authored());
  TEST_CHECK(anim->rotations.authored());
  TEST_CHECK(anim->scales.authored());
}

void usda_reader_blendshape_schema_test(void) {
  const char *usda = R"(#usda 1.0

def BlendShape "blend" {
    uniform vector3f[] offsets = [(0.1, 0, 0), (0, 0.1, 0)]
    uniform vector3f[] normalOffsets = [(0, 0, 1), (0, 0, 1)]
    uniform int[] pointIndices = [0, 1]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/blend", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const BlendShape *bs = (*result)->as<BlendShape>();
  TEST_CHECK(bs != nullptr);
  if (!bs) return;

  TEST_CHECK(bs->offsets.authored());
  TEST_CHECK(bs->normalOffsets.authored());
  TEST_CHECK(bs->pointIndices.authored());
}

void usda_reader_physics_collisiongroup_schema_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "cg" {
    uniform token physics:mergeGroup = "groupA"
    uniform bool physics:invertFilteredGroups = true
    rel physics:filteredGroups = [</cg2>]
}
def PhysicsCollisionGroup "cg2" {}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/cg", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const PhysicsCollisionGroup *cg = (*result)->as<PhysicsCollisionGroup>();
  TEST_CHECK(cg != nullptr);
  if (!cg) return;

  TEST_CHECK(cg->mergeGroup.authored());
  lightusd::legacy::value::token mg;
  if (cg->mergeGroup.get_value(&mg)) TEST_CHECK(mg.str() == "groupA");
  TEST_CHECK(cg->invertFilteredGroups.authored());
  TEST_CHECK(cg->invertFilteredGroups.get_value() == true);
  TEST_CHECK(cg->filteredGroups.authored());
  TEST_CHECK(cg->filteredGroups.get_targetPaths().size() == 1);
}

// PhysicsRigidBodyAPI/MassAPI/CollisionAPI are applied API schemas; their props
// live in the host prim's generic map. Verify the typed tydra getters extract
// them. (Regression for the new GetPhysics*API helpers.)
void usda_reader_physics_api_schemas_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "body" (
    prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsCollisionAPI"]
)
{
    bool physics:rigidBodyEnabled = true
    vector3f physics:velocity = (1, 2, 3)
    vector3f physics:angularVelocity = (4, 5, 6)
    float physics:mass = 2.5
    point3f physics:centerOfMass = (0.5, 0.5, 0.5)
    float3 physics:diagonalInertia = (1, 2, 3)
    bool physics:collisionEnabled = false
    rel physics:simulationOwner = [</Scene>]
}
def PhysicsScene "Scene" {}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/body", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;

  // RigidBodyAPI
  {
    PhysicsRigidBodyAPI rb;
    TEST_CHECK(GetPhysicsRigidBodyAPI(*prim, &rb));
    TEST_CHECK(rb.mass.authored());
    if (auto v = rb.mass.get_value()) TEST_CHECK(math::is_close(*v, 2.5f));
    TEST_CHECK(rb.velocity.authored());
    if (rb.velocity.get_value()) {
      lightusd::legacy::value::vector3f vel;
      if (rb.velocity.get_value().value().get_scalar(&vel)) {
        TEST_CHECK(math::is_close(vel[0], 1.0f) && math::is_close(vel[2], 3.0f));
      }
    }
    TEST_CHECK(rb.centerOfMass.authored());
    TEST_CHECK(rb.rigidBodyEnabled.get_value() == true);
  }
  // MassAPI
  {
    PhysicsMassAPI mass;
    TEST_CHECK(GetPhysicsMassAPI(*prim, &mass));
    TEST_CHECK(mass.mass.authored());
    if (mass.mass.authored()) TEST_CHECK(math::is_close(mass.mass.get_value(), 2.5f));
    TEST_CHECK(mass.centerOfMass.authored());
    TEST_CHECK(mass.diagonalInertia.authored());
  }
  // CollisionAPI
  {
    PhysicsCollisionAPI col;
    TEST_CHECK(GetPhysicsCollisionAPI(*prim, &col));
    TEST_CHECK(col.collisionEnabled.authored());
    TEST_CHECK(col.collisionEnabled.get_value() == false);
    TEST_CHECK(col.simulationOwner.authored());
    TEST_CHECK(col.simulationOwner.get_targetPaths().size() == 1);
  }
  // Negative: a schema not in apiSchemas must report false.
  {
    PhysicsMaterialAPI mat;
    TEST_CHECK(GetPhysicsMaterialAPI(*prim, &mat) == false);
  }
}

void usda_reader_physics_material_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat" (
    prepend apiSchemas = ["PhysicsMaterialAPI"]
)
{
    float physics:staticFriction = 0.5
    float physics:dynamicFriction = 0.4
    float physics:restitution = 0.2
    float physics:density = 1000
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/mat", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  PhysicsMaterialAPI matapi;
  TEST_CHECK(GetPhysicsMaterialAPI(**result, &matapi));
  TEST_CHECK(matapi.staticFriction.authored());
  if (auto v = matapi.staticFriction.get_value()) TEST_CHECK(math::is_close(*v, 0.5f));
  TEST_CHECK(matapi.dynamicFriction.authored());
  TEST_CHECK(matapi.restitution.authored());
  TEST_CHECK(matapi.density.authored());
  if (auto v = matapi.density.get_value()) TEST_CHECK(math::is_close(*v, 1000.0f));
}

void usda_reader_collection_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    uniform token collection:foo:expansionRule = "expandPrimsAndProperties"
    bool collection:foo:includeRoot = true
    rel collection:foo:includes = [</mesh>]
    rel collection:foo:excludes = [</other>]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto result = stage.GetPrimAtPath(Path("/mesh", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const GeomMesh *mesh = (*result)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  const CollectionInstance *coll = nullptr;
  TEST_CHECK(mesh->get_instance("foo", &coll));
  if (coll) {
    TEST_CHECK(coll->expansionRule.get_value() ==
               CollectionInstance::ExpansionRule::ExpandPrimsAndProperties);
    TEST_CHECK(coll->includeRoot.authored());
    TEST_CHECK(coll->includes.authored());
    TEST_CHECK(coll->includes.get_targetPaths().size() == 1);
    TEST_CHECK(coll->excludes.authored());
    TEST_CHECK(coll->excludes.get_targetPaths().size() == 1);
  }
}

void usda_reader_lightfilter_schema_test(void) {
  const char *usda = R"(#usda 1.0

def LightFilter "filter" {
    uniform token visibility = "invisible"
}
def PluginLightFilter "plug" {
    uniform token light:shaderId = "MyFilterShader"
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  {
    auto result = stage.GetPrimAtPath(Path("/filter", ""));
    TEST_CHECK(bool(result));
    if (result) {
      const LightFilter *lf = (*result)->as<LightFilter>();
      TEST_CHECK(lf != nullptr);
      if (lf) TEST_CHECK(lf->visibility.authored());
    }
  }
  {
    auto result = stage.GetPrimAtPath(Path("/plug", ""));
    TEST_CHECK(bool(result));
    if (result) {
      const PluginLightFilter *plf = (*result)->as<PluginLightFilter>();
      TEST_CHECK(plf != nullptr);
      if (plf) {
        TEST_CHECK(plf->shaderId.authored());
        if (plf->shaderId.get_value()) {
          lightusd::legacy::value::token sid;
          if (plf->shaderId.get_value().value().get_scalar(&sid)) {
            TEST_CHECK(sid.str() == "MyFilterShader");
          }
        }
      }
    }
  }
}

namespace {
// Read a TypedAttributeWithFallback<Animatable<double>> default value.
bool get_anim_double(const TypedAttributeWithFallback<Animatable<double>> &a,
                     double *out) {
  return a.get_value().get_scalar(out);
}
}  // namespace

// Geometric intrinsics (Cube/Sphere/Cone/Cylinder/Capsule/Plane): size/radius/
// height/width/length + axis. Previously only exercised by the crate writer.
void usda_reader_geom_intrinsics_schema_test(void) {
  const char *usda = R"(#usda 1.0

def Cube "cube" {
    double size = 3.0
}
def Sphere "sphere" {
    double radius = 1.5
}
def Cone "cone" {
    double height = 4
    double radius = 2
    uniform token axis = "X"
}
def Cylinder "cyl" {
    double height = 5
    double radius = 1
    uniform token axis = "Y"
}
def Capsule "cap" {
    double height = 3
    double radius = 0.5
    uniform token axis = "Z"
}
def Plane "plane" {
    double width = 6
    double length = 7
    uniform token axis = "Y"
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  double d = 0.0;
  {
    auto r = stage.GetPrimAtPath(Path("/cube", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomCube *c = (*r)->as<GeomCube>(); TEST_CHECK(c != nullptr);
      if (c) { TEST_CHECK(c->size.authored()); if (get_anim_double(c->size, &d)) TEST_CHECK(math::is_close(d, 3.0)); } }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/sphere", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomSphere *c = (*r)->as<GeomSphere>(); TEST_CHECK(c != nullptr);
      if (c) { TEST_CHECK(c->radius.authored()); if (get_anim_double(c->radius, &d)) TEST_CHECK(math::is_close(d, 1.5)); } }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/cone", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomCone *c = (*r)->as<GeomCone>(); TEST_CHECK(c != nullptr);
      if (c) {
        TEST_CHECK(c->height.authored()); if (get_anim_double(c->height, &d)) TEST_CHECK(math::is_close(d, 4.0));
        TEST_CHECK(c->radius.authored());
        TEST_CHECK(c->axis.authored()); TEST_CHECK(c->axis.get_value() == Axis::X);
      } }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/cyl", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomCylinder *c = (*r)->as<GeomCylinder>(); TEST_CHECK(c != nullptr);
      if (c) { TEST_CHECK(c->height.authored()); TEST_CHECK(c->radius.authored());
        TEST_CHECK(c->axis.get_value() == Axis::Y); } }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/cap", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomCapsule *c = (*r)->as<GeomCapsule>(); TEST_CHECK(c != nullptr);
      if (c) { TEST_CHECK(c->height.authored()); TEST_CHECK(c->radius.authored());
        TEST_CHECK(c->axis.get_value() == Axis::Z); } }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/plane", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeomPlane *c = (*r)->as<GeomPlane>(); TEST_CHECK(c != nullptr);
      if (c) { TEST_CHECK(c->width.authored()); TEST_CHECK(c->length.authored());
        TEST_CHECK(c->axis.get_value() == Axis::Y); } }
  }
}

void usda_reader_geom_tetmesh_schema_test(void) {
  const char *usda = R"(#usda 1.0

def TetMesh "tet" {
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0), (0,0,1)]
    int4[] tetVertexIndices = [(0, 1, 2, 3)]
    int3[] surfaceFaceVertexIndices = [(0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/tet", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const GeomTetMesh *tet = (*r)->as<GeomTetMesh>();
  TEST_CHECK(tet != nullptr);
  if (!tet) return;
  TEST_CHECK(tet->points.authored());
  TEST_CHECK(tet->tetVertexIndices.authored());
  TEST_CHECK(tet->surfaceFaceVertexIndices.authored());
}

void usda_reader_geom_nurbspatch_schema_test(void) {
  const char *usda = R"(#usda 1.0

def NurbsPatch "patch" {
    int uVertexCount = 4
    int vVertexCount = 4
    int uOrder = 3
    int vOrder = 3
    double[] uKnots = [0, 0, 0, 1, 2, 2, 2]
    double[] vKnots = [0, 0, 0, 1, 2, 2, 2]
    point3f[] points = [(0,0,0), (1,0,0), (2,0,0), (3,0,0)]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/patch", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const GeomNurbsPatch *patch = (*r)->as<GeomNurbsPatch>();
  TEST_CHECK(patch != nullptr);
  if (!patch) return;
  TEST_CHECK(patch->uVertexCount.authored());
  TEST_CHECK(patch->vVertexCount.authored());
  TEST_CHECK(patch->uOrder.authored());
  TEST_CHECK(patch->uKnots.authored());
  TEST_CHECK(patch->vKnots.authored());
  TEST_CHECK(patch->points.authored());
}

void usda_reader_geom_hermite_schema_test(void) {
  const char *usda = R"(#usda 1.0

def HermiteCurves "herm" {
    int[] curveVertexCounts = [2]
    point3f[] points = [(0,0,0), (1,1,0)]
    vector3f[] tangents = [(1,0,0), (1,0,0)]
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/herm", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const GeomHermiteCurves *herm = (*r)->as<GeomHermiteCurves>();
  TEST_CHECK(herm != nullptr);
  if (!herm) return;
  TEST_CHECK(herm->curveVertexCounts.authored());
  TEST_CHECK(herm->points.authored());
  TEST_CHECK(herm->tangents.authored());
}

void usda_reader_portal_geometry_dome1_lights_test(void) {
  const char *usda = R"(#usda 1.0

def GeometryLight "glight" {
    float inputs:intensity = 2.0
    rel geometry = </geo>
}
def PortalLight "portal" {
    float inputs:intensity = 1.0
}
def DomeLight_1 "dome" {
    float inputs:intensity = 3.0
    float guideRadius = 500
    uniform token poleAxis = "Y"
}
def Mesh "geo" {}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  {
    auto r = stage.GetPrimAtPath(Path("/glight", ""));
    TEST_CHECK(bool(r));
    if (r) { const GeometryLight *l = (*r)->as<GeometryLight>(); TEST_CHECK(l != nullptr);
      if (l) TEST_CHECK(l->intensity.authored()); }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/portal", ""));
    TEST_CHECK(bool(r));
    if (r) { const PortalLight *l = (*r)->as<PortalLight>(); TEST_CHECK(l != nullptr);
      if (l) TEST_CHECK(l->intensity.authored()); }
  }
  {
    auto r = stage.GetPrimAtPath(Path("/dome", ""));
    TEST_CHECK(bool(r));
    if (r) { const DomeLight_1 *l = (*r)->as<DomeLight_1>(); TEST_CHECK(l != nullptr);
      if (l) {
        TEST_CHECK(l->intensity.authored());
        TEST_CHECK(l->guideRadius.authored());
        TEST_CHECK(l->poleAxis.authored());
        TEST_CHECK(l->poleAxis.get_value().str() == "Y");
      } }
  }
}

// Collection-based material binding: `material:binding:collection:<name>` is
// reconstructed into MaterialBinding's collection map (not just generic props).
void usda_reader_collection_material_binding_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh" (
    prepend apiSchemas = ["CollectionAPI:metalBits"]
)
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
    uniform token collection:metalBits:expansionRule = "expandPrims"
    rel collection:metalBits:includes = [</mesh>]
    rel material:binding:collection:metalBits = [</Mtl/Metal>]
}
def Material "Mtl" { def Material "Metal" {} }
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/mesh", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const GeomMesh *mesh = (*r)->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  // Use the const accessor (has_materialBindingCollection is non-const).
  TEST_CHECK(mesh->materialBindingCollectionMap().count("metalBits") == 1);
}

// Value clips: the `clips` prim metadata (a Dictionary) must reconstruct.
void usda_reader_value_clips_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "anim" (
    clips = {
        dictionary clipSet = {
            asset[] assetPaths = [@./clip0.usd@, @./clip1.usd@]
            string primPath = "/anim"
            double2[] active = [(0, 0), (10, 1)]
            double2[] times = [(0, 0), (10, 10)]
            asset manifestAssetPath = @./manifest.usd@
        }
    }
)
{
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/anim", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  TEST_CHECK((*r)->metas().has_clips());
}

// Light/shadow linking: lights inherit Collection, so collection:lightLink:* /
// collection:shadowLink:* must reconstruct into the typed Collection map
// (previously dropped to generic props because the light reader didn't run
// ReconstructCollectionProperties).
void usda_reader_light_linking_test(void) {
  const char *usda = R"(#usda 1.0

def SphereLight "light" (
    prepend apiSchemas = ["CollectionAPI:lightLink", "CollectionAPI:shadowLink"]
)
{
    float inputs:intensity = 1.0
    uniform token collection:lightLink:expansionRule = "expandPrims"
    rel collection:lightLink:includes = [</geoA>]
    uniform token collection:shadowLink:expansionRule = "expandPrims"
    rel collection:shadowLink:excludes = [</geoB>]
}
def Mesh "geoA" {}
def Mesh "geoB" {}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/light", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const SphereLight *light = (*r)->as<SphereLight>();
  TEST_CHECK(light != nullptr);
  if (!light) return;

  const CollectionInstance *ll = nullptr;
  TEST_CHECK(light->get_instance("lightLink", &ll));
  if (ll) {
    TEST_CHECK(ll->includes.authored());
    TEST_CHECK(ll->includes.get_targetPaths().size() == 1);
  }
  const CollectionInstance *sl = nullptr;
  TEST_CHECK(light->get_instance("shadowLink", &sl));
  if (sl) {
    TEST_CHECK(sl->excludes.authored());
    TEST_CHECK(sl->excludes.get_targetPaths().size() == 1);
  }
}

// Collection-based physics: a PhysicsCollisionGroup's `colliders` collection
// (the set of prims in the group) is read via GetPhysicsCollidersCollection.
void usda_reader_physics_colliders_collection_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "cg" (
    prepend apiSchemas = ["CollectionAPI:colliders"]
)
{
    uniform token collection:colliders:expansionRule = "expandPrims"
    rel collection:colliders:includes = [</geoA>, </geoB>]
    rel collection:colliders:excludes = [</geoC>]
}
def Mesh "geoA" {}
def Mesh "geoB" {}
def Mesh "geoC" {}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &stage, &warn, &err));
  TEST_MSG("err: %s", err.c_str());

  auto r = stage.GetPrimAtPath(Path("/cg", ""));
  TEST_CHECK(bool(r));
  if (!r) return;

  std::vector<Path> includes, excludes;
  TEST_CHECK(GetPhysicsCollidersCollection(**r, &includes, &excludes));
  TEST_CHECK(includes.size() == 2);
  TEST_CHECK(excludes.size() == 1);
}
