// SPDX-License-Identifier: Apache 2.0

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usd-validation.h"

#include <cstring>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "usd-validation.hh"
#include "usdc-writer.hh"

using namespace lightusd;

namespace {

static bool parse_layer(const char *usda, Layer *layer, std::string *warn,
                        std::string *err) {
  return LoadLayerFromMemory(reinterpret_cast<const uint8_t *>(usda),
                             std::strlen(usda), "validation-test.usda", layer,
                             warn, err);
}

static bool HasRule(const USDValidationResult &result,
                    const std::string &rule_id) {
  for (const auto &issue : result.issues) {
    if (issue.rule_id == rule_id) {
      return true;
    }
  }
  return false;
}

static ValidationOptions AllGroups() {
  return MakeValidateAllOptions();
}

static bool Contains(const std::string &haystack, const char *needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

void usd_validation_valid_core_schema_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "Root"
    colorConfiguration = @ocio://default@
)

def Xform "Root" (
    prepend apiSchemas = ["ColorSpaceAPI", "CollectionAPI:lookset"]
)
{
    uniform token colorSpace:name = "sRGB"
    uniform token collection:lookset:expansionRule = "expandPrims"
    uniform bool collection:lookset:includeRoot = 1
    rel collection:lookset:includes = [</Root>]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
  }
  TEST_CHECK(ok);
  if (!ok) {
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  if (!result.ok()) {
    TEST_MSG("%s", FormatValidationResult(result).c_str());
  }
  TEST_CHECK(result.ok());
}

void usd_validation_invalid_default_prim_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "Missing"
)

def Scope "Root"
{
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!result.ok());
  TEST_CHECK(HasRule(result, "core.layer.defaultPrim"));
}

void usd_validation_invalid_collection_rule_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "Root" (
    prepend apiSchemas = ["CollectionAPI:lookset"]
)
{
    uniform token collection:lookset:expansionRule = "bogusRule"
    rel collection:lookset:includes = [</Root>]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!result.ok());
  TEST_CHECK(HasRule(result, "core.schema.CollectionAPI.expansionRule"));
}

void usd_validation_invalid_color_space_test(void) {
  const char *usda = R"(#usda 1.0

def Scope "Root" (
    prepend apiSchemas = ["ColorSpaceAPI", "ColorSpaceDefinitionAPI:bad",
        "ColorSpaceDefinitionAPI:dupA", "ColorSpaceDefinitionAPI:dupB"]
)
{
    uniform token colorSpace:name = "not_a_real_color_space"
    uniform token colorSpaceDefinition:bad:name = "bad"
    float2 colorSpaceDefinition:bad:redChroma = (0, 0)
    float2 colorSpaceDefinition:bad:greenChroma = (0, 0)
    float2 colorSpaceDefinition:bad:blueChroma = (0, 0)
    float2 colorSpaceDefinition:bad:whitePoint = (0, 0)
    float colorSpaceDefinition:bad:gamma = 0
    uniform token colorSpaceDefinition:dupA:name = "duplicated"
    uniform token colorSpaceDefinition:dupB:name = "duplicated"
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!result.ok());
  TEST_CHECK(HasRule(result, "core.schema.ColorSpaceAPI.name"));
  TEST_CHECK(HasRule(result, "core.schema.ColorSpaceDefinitionAPI.invalid"));
  TEST_CHECK(HasRule(result, "core.schema.ColorSpaceDefinitionAPI.duplicate"));
}

void usd_validation_layer_metadata_test(void) {
  const char *usda = R"(#usda 1.0
(
    metersPerUnit = 0
    startTimeCode = 10
    endTimeCode = 0
)

def Xform "Root"
{
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!result.ok());
  TEST_CHECK(HasRule(result, "core.layer.metersPerUnit"));
  TEST_CHECK(HasRule(result, "core.layer.timeCodeRange"));
}

void usd_validation_xformop_order_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "Root"
{
    float3 xformOp:scale = (1, 1, 1)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!result.ok());
  TEST_CHECK(HasRule(result, "core.xformOp.order"));
}

// A prim that carries a composition arc may inherit its xformOps; the rule must
// not fire (no false positive on uncomposed layers).
void usd_validation_xformop_order_gated_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "Root" (
    prepend references = @./base.usda@</Base>
)
{
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!HasRule(result, "core.xformOp.order"));
}

void usd_validation_nested_gprim_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Mesh "outer"
    {
        def Cube "inner"
        {
        }
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  // Off by default (core only).
  const USDValidationResult core_only = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!HasRule(core_only, "geom.encapsulation.nestedGprim"));

  // Enabled with the geom group.
  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "geom.encapsulation.nestedGprim"));
}

void usd_validation_shader_encapsulation_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Shader "shader"
    {
        uniform token info:id = "UsdPreviewSurface"
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  const bool ok = parse_layer(usda, &layer, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("parse failed: %s", err.c_str());
    return;
  }

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "shade.encapsulation.shaderParent"));
}

void usd_validation_material_binding_test(void) {
  // material:binding authored as an attribute -> error.
  {
    const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Mesh "geom"
    {
        custom token material:binding = "/World/mat"
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    const bool ok = parse_layer(usda, &layer, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      const USDValidationResult all =
          ValidateLayerAgainstAOUSDCore(layer, AllGroups());
      TEST_CHECK(HasRule(all, "shade.material.binding"));
    }
  }

  // material:binding relationship without MaterialBindingAPI -> warning.
  {
    const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Mesh "geom"
    {
        rel material:binding = </World/mat>
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    const bool ok = parse_layer(usda, &layer, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      const USDValidationResult all =
          ValidateLayerAgainstAOUSDCore(layer, AllGroups());
      TEST_CHECK(HasRule(all, "shade.material.bindingAPI"));
      // A well-formed relationship must not trip the "must be a relationship"
      // error.
      TEST_CHECK(!HasRule(all, "shade.material.binding"));
    }
  }

  // material:binding relationship WITH MaterialBindingAPI -> no warning.
  // (Verifies known API schemas surface through CollectAppliedSchemas.)
  {
    const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Mesh "geom" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/mat>
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    const bool ok = parse_layer(usda, &layer, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      const USDValidationResult all =
          ValidateLayerAgainstAOUSDCore(layer, AllGroups());
      TEST_CHECK(!HasRule(all, "shade.material.bindingAPI"));
      TEST_CHECK(!HasRule(all, "shade.material.binding"));
    }
  }
}

void usd_validation_preview_surface_test(void) {
  // Off-schema UsdPreviewSurface inputs: wrong type + unknown name.
  {
    const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:diffuseColor = 0.5
        float inputs:rooughness = 0.5
        token outputs:surface
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    const bool ok = parse_layer(usda, &layer, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      const USDValidationResult all =
          ValidateLayerAgainstAOUSDCore(layer, AllGroups());
      TEST_CHECK(HasRule(all, "shade.preview.inputType"));
      TEST_CHECK(HasRule(all, "shade.preview.unknownInput"));
    }
  }

  // Schema-conformant UsdPreviewSurface: no preview warnings. Also verifies
  // role-type agreement (color3f accepted where color3f is expected, and a
  // connected input keeps its declared type).
  {
    const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.1, 0.2, 0.3)
        float inputs:roughness = 0.4
        float inputs:metallic = 0.0
        normal3f inputs:normal = (0, 0, 1)
        int inputs:useSpecularWorkflow = 0
        token outputs:surface
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    const bool ok = parse_layer(usda, &layer, &warn, &err);
    TEST_CHECK(ok);
    if (ok) {
      const USDValidationResult all =
          ValidateLayerAgainstAOUSDCore(layer, AllGroups());
      TEST_CHECK(!HasRule(all, "shade.preview.inputType"));
      TEST_CHECK(!HasRule(all, "shade.preview.unknownInput"));
    }
  }
}

void usd_validation_material_output_connection_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat"
{
    token outputs:surface = "notAConnection"

    def Mesh "notShader"
    {
        token outputs:surface
    }

    token outputs:volume.connect = </mat/notShader.outputs:surface>
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "shade.material.outputConnection"));
  TEST_CHECK(HasRule(all, "shade.connection.target"));
}

void usd_validation_uv_texture_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "tex"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @@
        token inputs:wrapS = "tileForever"
        token inputs:sourceColorSpace = "ACEScg"
        float3 outputs:rgb
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "shade.uvTexture.file"));
  TEST_CHECK(HasRule(all, "shade.uvTexture.wrap"));
  TEST_CHECK(HasRule(all, "shade.uvTexture.sourceColorSpace"));
}

void usd_validation_primvar_reader_test(void) {
  {
    const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        float2 outputs:result
    }
}
)";

    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

    const USDValidationResult all =
        ValidateLayerAgainstAOUSDCore(layer, AllGroups());
    TEST_CHECK(HasRule(all, "shade.primvarReader.varname"));
  }

  {
    const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = ""
        float2 outputs:result
    }
}
)";

    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

    const USDValidationResult all =
        ValidateLayerAgainstAOUSDCore(layer, AllGroups());
    TEST_CHECK(HasRule(all, "shade.primvarReader.varname"));
  }

  {
    const char *usda = R"(#usda 1.0

def Material "mat"
{
    token inputs:stPrimvarName = "st"

    def Shader "reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname.connect = </mat.inputs:stPrimvarName>
        token outputs:result
    }
}
)";

    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

    const USDValidationResult all =
        ValidateLayerAgainstAOUSDCore(layer, AllGroups());
    TEST_CHECK(HasRule(all, "shade.primvarReader.result"));
  }
}

void usd_validation_preview_surface_range_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat"
{
    def Shader "surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:roughness = 2.0
        float inputs:ior = 0.0
        token inputs:opacityMode = "vanish"
        token outputs:surface
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "shade.preview.inputRange"));
  TEST_CHECK(HasRule(all, "shade.preview.opacityMode"));
}

void usd_validation_usdskel_test(void) {
  const char *usda = R"(#usda 1.0

def SkelRoot "Root"
{
    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root/Spine", "Root"]
        uniform matrix4d[] bindTransforms = [
            ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))
        ]
    }

    def SkelAnimation "Anim"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
        float3[] translations = [(0, 0, 0)]
        uniform token[] blendShapes = ["smile", "blink"]
        float[] blendShapeWeights = [1]
    }

    def Mesh "Mesh"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0)]
        int[] faceVertexCounts = [2]
        int[] faceVertexIndices = [0, 1]
        rel skel:skeleton = </Root/Skel>
        int[] primvars:skel:jointIndices = [0, -1, 1] (
            interpolation = "constant"
            elementSize = 2
        )
        float[] primvars:skel:jointWeights = [1, 0] (
            interpolation = "vertex"
            elementSize = 1
        )
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "geom.skel.skeleton.topology"));
  TEST_CHECK(HasRule(all, "geom.skel.skeleton.transforms"));
  TEST_CHECK(HasRule(all, "geom.skel.animation.transforms"));
  TEST_CHECK(HasRule(all, "geom.skel.animation.blendShapes"));
  TEST_CHECK(HasRule(all, "geom.skel.skinning.interpolation"));
  TEST_CHECK(HasRule(all, "geom.skel.skinning.elementSize"));
  TEST_CHECK(HasRule(all, "geom.skel.skinning.size"));
  TEST_CHECK(HasRule(all, "geom.skel.skinning.index"));
}

void usd_validation_materialx_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat" (
    references = @./look.mtlx@
)
{
    string config:mtlx:version = "2.0"
    string config:mtlx:sourceUri = "look.usda"

    def Shader "mtlxShader"
    {
        uniform token info:id = "ND_open_pbr_surface_surfaceshader"
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "shade.materialX.configAPI"));
  TEST_CHECK(HasRule(all, "shade.materialX.version"));
  TEST_CHECK(HasRule(all, "shade.materialX.sourceUri"));
  TEST_CHECK(HasRule(all, "shade.materialX.referencePrimPath"));
  TEST_CHECK(HasRule(all, "shade.materialX.output"));
}

void usd_validation_composition_arcs_test(void) {
  const char *usda = R"(#usda 1.0

class Xform "NotAClassTarget"
{
}

def Xform "Root" (
    references = @./ref.usda@</Ref> (scale = 0)
    payload = @./payload.usda@</Payload> (scale = 0)
    inherits = </Root>
    specializes = </Root>
    variantSets = ["bad-name"]
)
{
}

over "Empty"
{
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(HasRule(result, "core.composition.reference"));
  TEST_CHECK(HasRule(result, "core.composition.payload"));
  TEST_CHECK(HasRule(result, "core.composition.inherits"));
  TEST_CHECK(HasRule(result, "core.composition.specializes"));
  TEST_CHECK(HasRule(result, "core.composition.variantSets"));
  TEST_CHECK(HasRule(result, "core.composition.over"));
}

void usd_validation_report_format_test(void) {
  // Failing core report: states checked groups + FAILED status.
  {
    const char *usda = R"(#usda 1.0
(
    metersPerUnit = 0
)

def Xform "Root"
{
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
    const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
    const std::string report = FormatValidationResult(result);
    TEST_CHECK(Contains(report, "Checked rule groups: core"));
    TEST_CHECK(Contains(report, "core.layer.metersPerUnit"));
    TEST_CHECK(Contains(report, "FAILED"));
    TEST_CHECK(Contains(report, "1 error"));
  }

  // Clean report: PASSED with no issues.
  {
    const char *usda = R"(#usda 1.0

def Xform "Root"
{
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
    const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
    const std::string report = FormatValidationResult(result);
    TEST_CHECK(Contains(report, "PASSED - no issues found"));
  }

  // Warnings-only report (all groups): PASSED with warnings + group coverage.
  {
    const char *usda = R"(#usda 1.0

def Xform "World"
{
    def Mesh "outer"
    {
        def Cube "inner"
        {
        }
    }
}
)";
    Layer layer;
    std::string warn;
    std::string err;
    TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
    const USDValidationResult result =
        ValidateLayerAgainstAOUSDCore(layer, AllGroups());
    const std::string report = FormatValidationResult(result);
    TEST_CHECK(Contains(report,
                        "Checked rule groups: core, geom, shade, lux, physics"));
    TEST_CHECK(Contains(report, "PASSED with warnings"));
  }
}

void usd_validation_group_gating_test(void) {
  const char *usda = R"(#usda 1.0
(
    metersPerUnit = 0
)

def Scope "Root" (
    prepend apiSchemas = ["CollectionAPI:bad"]
)
{
    uniform token collection:bad:expansionRule = "notARealRule"
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  ValidationOptions geom_only;
  geom_only.core = false;
  geom_only.geom = true;
  geom_only.shade = false;
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, geom_only);
  TEST_CHECK(!HasRule(result, "core.layer.metersPerUnit"));
  TEST_CHECK(!HasRule(result, "core.schema.CollectionAPI.expansionRule"));
  TEST_CHECK(result.error_count() == 0);
}

void usd_validation_mesh_topology_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "badMesh"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0)]
    int[] faceVertexCounts = [3, 0]
    int[] faceVertexIndices = [0, 1, 9, 0]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult core_only = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!HasRule(core_only, "geom.mesh.topology.count"));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "geom.mesh.topology.count"));
  TEST_CHECK(HasRule(all, "geom.mesh.topology.size"));
  TEST_CHECK(HasRule(all, "geom.mesh.topology.index"));
}

void usd_validation_geom_subset_indices_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)]
    int[] faceVertexCounts = [3, 3]
    int[] faceVertexIndices = [0, 1, 2, 1, 3, 2]

    def GeomSubset "subset"
    {
        uniform token elementType = "face"
        uniform token familyName = "materialBind"
        int[] indices = [0, 2, 0]
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "geom.subset.indices"));
  TEST_CHECK(all.error_count() >= 1);
  TEST_CHECK(all.warning_count() >= 1);
}

void usd_validation_primvar_indices_metadata_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    float[] primvars:weights = [0.25, 0.5] (
        interpolation = "bogus"
        elementSize = 0
    )
    int[] primvars:weights:indices = [0, 2, -1]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult all =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(all, "geom.primvar.metadata"));
  TEST_CHECK(HasRule(all, "geom.primvar.indices"));
}

void usd_validation_usdc_crate_test(void) {
  Stage stage;
  std::vector<uint8_t> usdc;
  std::string warn;
  std::string err;
  TEST_CHECK(usdc::SaveAsUSDCToMemory(stage, &usdc, &warn, &err));
  TEST_CHECK(!usdc.empty());

  ValidationOptions opts;
  opts.core = false;
  opts.geom = false;
  opts.shade = false;
  opts.crate = true;
  const USDValidationResult result =
      ValidateUSDContainerAgainstAOUSDCore(usdc.data(), usdc.size(),
                                           "generated.usdc", opts);
  TEST_CHECK(result.checked_groups.crate);
  TEST_CHECK(result.error_count() == 0);
}

void usd_validation_validate_usd_memory_entrypoint_test(void) {
  const char *usda = R"(#usda 1.0

def Material "mat"
{
    token inputs:stPrimvarName = "st"

    def Shader "reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname.connect = </mat.inputs:stPrimvarName>
        token outputs:result
    }
}
)";

  USDValidationResult result;
  std::string warn;
  std::string err;
  USDLoadOptions load_options;
  const bool ok = ValidateUSDFromMemoryAgainstAOUSDCore(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "entrypoint.usda", MakeValidateAllOptions(), load_options, &result,
      &warn, &err);
  if (!ok) {
    TEST_MSG("validate entrypoint failed: %s", err.c_str());
  }
  TEST_CHECK(ok);
  TEST_CHECK(result.checked_groups.core);
  TEST_CHECK(result.checked_groups.geom);
  TEST_CHECK(result.checked_groups.shade);
  TEST_CHECK(!result.checked_groups.crate);
  TEST_CHECK(HasRule(result, "shade.primvarReader.result"));
}

void usd_validation_usdskel_animation_timesamples_test(void) {
  const char *usda = R"(#usda 1.0

def SkelRoot "Root"
{
    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
    }

    def SkelAnimation "Anim"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
        quatf[] rotations.timeSamples = {
            1: [(1, 0, 0, 0), (1, 0, 0, 0)]
            2: [(1, 0, 0, 0)]
        }
        uniform token[] blendShapes = ["Smile", "Smile"]
        float[] blendShapeWeights.timeSamples = {
            1: [0, 1]
            2: [0]
        }
    }

    def Mesh "Mesh"
    {
        point3f[] points = [(0, 0, 0)]
        int[] faceVertexCounts = [1]
        int[] faceVertexIndices = [0]
        rel skel:skeleton = </Root/Skel>
        int[] primvars:skel:jointIndices = [0, 3] (
            interpolation = "vertex"
            elementSize = 2
        )
        float[] primvars:skel:jointWeights = [0.25, 0.25] (
            interpolation = "vertex"
            elementSize = 2
        )
    }
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.skel.animation.transforms"));
  TEST_CHECK(HasRule(result, "geom.skel.animation.blendShapes"));
  TEST_CHECK(HasRule(result, "geom.skel.skinning.index"));
  TEST_CHECK(HasRule(result, "geom.skel.skinning.weight"));
}

void usd_validation_blendshape_test(void) {
  const char *usda = R"(#usda 1.0

def BlendShape "Bad"
{
    uniform vector3f[] offsets = [(0, 0, 0), (1, 0, 0)]
    uniform vector3f[] normalOffsets = [(0, 0, 1)]
    uniform int[] pointIndices = [0, -1]
    uniform vector3f[] inbetweens:low = [(0, 0, 0)]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.skel.blendShape.normalOffsets"));
  TEST_CHECK(HasRule(result, "geom.skel.blendShape.pointIndices"));
  TEST_CHECK(HasRule(result, "geom.skel.blendShape.inbetween"));
}

void usd_validation_value_clips_test(void) {
  const char *usda = R"(#usda 1.0

over "Animated" (
    clips = {
        dictionary bad = {
            asset[] assetPaths = [@clip_0.usdc@]
            double2[] active = [(0, 0), (1, 2)]
            string primPath = "NotAbsolute"
            double2[] times = [(10, 10), (5, 5)]
        }
        dictionary templ = {
            asset templateAssetPath = @clips/anim.###.usd@
            double templateStartTime = 10
            double templateEndTime = 1
            double templateStride = 0
        }
    }
)
{
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "core.clips.active"));
  TEST_CHECK(HasRule(result, "core.clips.primPath"));
  TEST_CHECK(HasRule(result, "core.clips.times"));
  TEST_CHECK(HasRule(result, "core.clips.template"));
}

void usd_validation_metadata_semantics_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "1Bad"
    kilogramsPerUnit = 0
    colorConfiguration = @@
    colorManagementSystem = ""
    owner = ""
)

def Xform "Root" (
    kind = ""
    instanceable = true
    assetInfo = {
        asset identifier = @@
        string name = ""
        asset[] payloadAssetDependencies = [@@]
    }
    prepend apiSchemas = [
        "CollectionAPI",
        "MaterialBindingAPI:bad",
        "CollectionAPI:look",
        "CollectionAPI:look",
        "UnknownAPI:bad:name"
    ]
)
{
    uniform token side = "middle" (
        allowedTokens = ["left", "right", "left"]
        interpolation = "bogus"
        elementSize = 0
        connectability = "sometimes"
        renderType = ""
        outputName = ""
    )
    varying rel material:binding = </Root>
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  if (!err.empty()) {
    TEST_MSG("parse error: %s", err.c_str());
  }
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "core.layer.defaultPrim"));
  TEST_CHECK(HasRule(result, "core.layer.kilogramsPerUnit"));
  TEST_CHECK(HasRule(result, "core.layer.colorConfiguration"));
  TEST_CHECK(HasRule(result, "core.layer.colorManagementSystem"));
  TEST_CHECK(HasRule(result, "core.layer.owner"));
  TEST_CHECK(HasRule(result, "core.prim.kind"));
  TEST_CHECK(HasRule(result, "core.prim.instanceable"));
  TEST_CHECK(HasRule(result, "core.prim.assetInfo"));
  TEST_CHECK(HasRule(result, "core.apiSchema.instance"));
  TEST_CHECK(HasRule(result, "core.apiSchema.duplicate"));
  TEST_CHECK(HasRule(result, "core.apiSchema.unknown"));
  TEST_CHECK(HasRule(result, "core.attr.allowedTokens"));
  TEST_CHECK(HasRule(result, "core.attr.interpolation"));
  TEST_CHECK(HasRule(result, "core.attr.elementSize"));
  TEST_CHECK(HasRule(result, "core.attr.connectability"));
  TEST_CHECK(HasRule(result, "core.attr.renderType"));
  TEST_CHECK(HasRule(result, "core.attr.outputName"));
  TEST_CHECK(HasRule(result, "core.relationship.variability"));
}

void usd_validation_geom_common_metadata_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "mesh"
{
    float3[] extent = [(1, 0, 0), (0, 0, 0)]
    token orientation = "sideways"
    token purpose = "fx"
    token visibility = "hidden"
    point3f[] points = [(0, 0, 0), (1, 0, 0)]
    vector3f[] velocities = [(0, 0, 0)]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.gprim.extent"));
  TEST_CHECK(HasRule(result, "geom.gprim.orientation"));
  TEST_CHECK(HasRule(result, "geom.gprim.purpose"));
  TEST_CHECK(HasRule(result, "geom.gprim.visibility"));
  TEST_CHECK(HasRule(result, "geom.gprim.arraySize"));
}

void usd_validation_geom_primitives_test(void) {
  const char *usda = R"(#usda 1.0

def Sphere "sphere"
{
    double radius = -1
}

def Cube "cube"
{
    double size = 0
}

def Cylinder "cyl"
{
    double radius = 1
    double height = 0
    uniform token axis = "W"
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.primitive.size"));
  TEST_CHECK(HasRule(result, "geom.primitive.axis"));
}

void usd_validation_geom_points_curves_test(void) {
  const char *usda = R"(#usda 1.0

def Points "pts"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0)]
    int64[] ids = [1]
    float[] widths = [0.1, -1]
    vector3f[] velocities = [(0, 0, 0)]
}

def BasisCurves "curves"
{
    uniform token type = "cubic"
    uniform token basis = "bogus"
    uniform token wrap = "loop"
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]
    int[] curveVertexCounts = [2, 2]
    float[] widths = [0.1, 0.2, 0.3, 0.4]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.points.size"));
  TEST_CHECK(HasRule(result, "geom.points.widths"));
  TEST_CHECK(HasRule(result, "geom.curves.tokens"));
  TEST_CHECK(HasRule(result, "geom.curves.topology"));
  TEST_CHECK(HasRule(result, "geom.curves.widths"));
}

void usd_validation_point_instancer_test(void) {
  const char *usda = R"(#usda 1.0

def PointInstancer "pi"
{
    point3f[] positions = [(0, 0, 0), (1, 0, 0)]
    int[] protoIndices = [0, -1]
    quath[] orientations = [(1, 0, 0, 0)]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.pointInstancer.prototypes"));
  TEST_CHECK(HasRule(result, "geom.pointInstancer.indices"));
  TEST_CHECK(HasRule(result, "geom.pointInstancer.arraySize"));
}

void usd_validation_camera_test(void) {
  const char *usda = R"(#usda 1.0

def Camera "cam"
{
    float2 clippingRange = (10, 1)
    float focalLength = 0
    float horizontalAperture = -1
    float verticalAperture = 0
    float fStop = -1
    token projection = "fisheye"
    token stereoRole = "center"
    double shutter:open = 1
    double shutter:close = 0
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));
  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "geom.camera.range"));
  TEST_CHECK(HasRule(result, "geom.camera.optics"));
  TEST_CHECK(HasRule(result, "geom.camera.tokens"));
  TEST_CHECK(HasRule(result, "geom.camera.shutter"));
}

void usd_validation_lux_light_test(void) {
  const char *usda = R"(#usda 1.0

def SphereLight "sphere"
{
    float inputs:intensity = -1
    float inputs:shaping:cone:softness = 2
    float inputs:shadow:distance = -2
    float inputs:radius = 0
    token light:filters = "notARelationship"
}

def RectLight "rect"
{
    float inputs:width = -1
    float inputs:height = 0
    token inputs:texture:file = "notAnAssetPath"
}

def DistantLight "sun"
{
    float inputs:angle = 200
}

def DomeLight_1 "dome"
{
    float guideRadius = -1
    token inputs:texture:format = "cubeMap"
    token poleAxis = "X"
    asset inputs:texture:file = @@
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult core_only = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!HasRule(core_only, "lux.light.inputs"));
  TEST_CHECK(!HasRule(core_only, "lux.light.size"));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "lux.light.inputs"));
  TEST_CHECK(HasRule(result, "lux.shaping.inputs"));
  TEST_CHECK(HasRule(result, "lux.shadow.inputs"));
  TEST_CHECK(HasRule(result, "lux.light.size"));
  TEST_CHECK(HasRule(result, "lux.light.angle"));
  TEST_CHECK(HasRule(result, "lux.texture.file"));
  TEST_CHECK(HasRule(result, "lux.texture.format"));
  TEST_CHECK(HasRule(result, "lux.dome.poleAxis"));
  TEST_CHECK(HasRule(result, "lux.relationship.target"));
}

void usd_validation_physics_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "scene"
{
    vector3f physics:gravityDirection = (0, 0, 0)
    float physics:gravityMagnitude = -1
}

def Mesh "body" (
    prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsCollisionAPI", "PhysicsMeshCollisionAPI", "PhysicsFilteredPairsAPI"]
)
{
    float physics:mass = -1
    float physics:density = -2
    token physics:approximation = "triangleSoup"
    token physics:simulationOwner = "notARelationship"
    token physics:filteredPairs = "notARelationship"
}

def PhysicsRevoluteJoint "hinge" (
    prepend apiSchemas = ["PhysicsDriveAPI:rotQ", "PhysicsLimitAPI:transX"]
)
{
    token physics:axis = "W"
    token physics:body0 = "notARelationship"
    float physics:breakForce = -1
    float physics:lowerLimit = 10
    float physics:upperLimit = 0
    token physics:drive:rotQ:type = "speed"
    float physics:drive:rotQ:maxForce = -1
    float physics:limit:transX:low = 10
    float physics:limit:transX:high = 0
}

def PhysicsDistanceJoint "distance"
{
    float physics:minDistance = 5
    float physics:maxDistance = 1
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult core_only = ValidateLayerAgainstAOUSDCore(layer);
  TEST_CHECK(!HasRule(core_only, "physics.scene.gravity"));
  TEST_CHECK(!HasRule(core_only, "physics.value.range"));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.scene.gravity"));
  TEST_CHECK(HasRule(result, "physics.value.range"));
  TEST_CHECK(HasRule(result, "physics.collision.approximation"));
  TEST_CHECK(HasRule(result, "physics.relationship.target"));
  TEST_CHECK(HasRule(result, "physics.joint.body"));
  TEST_CHECK(HasRule(result, "physics.joint.axis"));
  TEST_CHECK(HasRule(result, "physics.joint.limit"));
  TEST_CHECK(HasRule(result, "physics.joint.distance"));
  TEST_CHECK(HasRule(result, "physics.drive.dof"));
  TEST_CHECK(HasRule(result, "physics.drive.type"));
  TEST_CHECK(HasRule(result, "physics.drive.value"));
  TEST_CHECK(HasRule(result, "physics.limit.range"));
}

void usd_validation_physics_inertia_and_joint_transform_test(void) {
  const char *usda = R"(#usda 1.0

def Mesh "body" (
    prepend apiSchemas = ["PhysicsMassAPI"]
)
{
    float3 physics:diagonalInertia = (-1, 0, 0)
    quatf physics:principalAxes = (0, 0, 0, 0)
    vector3f physics:velocity = (1, 2, 3)
}

def PhysicsRevoluteJoint "joint"
{
    point3f physics:localPos0 = (0, 0, 0)
    quatf physics:localRot0 = (2, 0, 0, 0)
}

def PhysicsCollisionGroup "group"
{
    token physics:mergeGroup = ""
    rel physics:filteredGroups = </body>
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.inertia"));
  TEST_CHECK(HasRule(result, "physics.quaternion"));
  TEST_CHECK(HasRule(result, "physics.collisionGroup.mergeGroup"));
  TEST_CHECK(HasRule(result, "physics.collisionGroup.filteredGroups"));
}

void usd_validation_physics_mjc_extension_test(void) {
  const char *usda = R"(#usda 1.0

def MjcActuator "act"
{
    token mjc:ctrlLimited = "maybe"
    double mjc:ctrlRange:min = 2
    double mjc:ctrlRange:max = 1
    double[] mjc:gear = [1, 2, 3]
    double[] mjc:solref = [1]
    rel mjc:target = </act>
}

def MjcTendon "tendon"
{
    token mjc:type = "rope"
    int mjc:maxhullvert = -1
    double mjc:width = -1
    double[] mjc:solimp = [1, 2, 3]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.extension.mjc.token"));
  TEST_CHECK(HasRule(result, "physics.extension.mjc.range"));
  TEST_CHECK(HasRule(result, "physics.extension.mjc.array"));
  TEST_CHECK(HasRule(result, "physics.extension.mjc.relationship"));

  const char *valid_sentinel_usda = R"(#usda 1.0

def Mesh "mesh"
{
    int mjc:maxhullvert = -1
}
)";
  Layer valid_layer;
  TEST_CHECK(parse_layer(valid_sentinel_usda, &valid_layer, &warn, &err));
  const USDValidationResult valid_result =
      ValidateLayerAgainstAOUSDCore(valid_layer, AllGroups());
  TEST_CHECK(!HasRule(valid_result, "physics.extension.mjc.range"));
}

void usd_validation_physics_mjc_tendon_route_arrays_test(void) {
  const char *usda = R"(#usda 1.0

def MjcTendon "badRoute"
{
    rel mjc:path = [</a>, </b>]
    int[] mjc:path:indices = [0]
    double[] mjc:path:coef = [1]
    rel mjc:sideSites = [</s>]
    int[] mjc:sideSites:indices = [0, 1]
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.extension.mjc.array"));

  const char *valid_usda = R"(#usda 1.0

def MjcTendon "goodRoute"
{
    rel mjc:path = [</a>, </b>]
    int[] mjc:path:indices = [0, 1, 0]
    double[] mjc:path:coef = [1, -0.5, 1]
    rel mjc:sideSites = [</s>]
    int[] mjc:sideSites:indices = [0]
}
)";

  Layer valid_layer;
  TEST_CHECK(parse_layer(valid_usda, &valid_layer, &warn, &err));
  const USDValidationResult valid_result =
      ValidateLayerAgainstAOUSDCore(valid_layer, AllGroups());
  TEST_CHECK(!HasRule(valid_result, "physics.extension.mjc.array"));
}

void usd_validation_physics_mjc_sensor_type_test(void) {
  const char *valid_usda = R"(#usda 1.0

def MjcSensor "gyro"
{
    token mjc:type = "gyro"
    token mjc:objtype = "site"
    token mjc:objname = "imu"
    double mjc:cutoff = 34.9
    double mjc:noise = 0.01
}

def MjcSensor "frame"
{
    token mjc:type = "framepos"
    token mjc:objtype = "body"
    token mjc:objname = "torso"
    token mjc:reftype = "site"
    token mjc:refname = "imu"
}
)";

  Layer valid_layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(valid_usda, &valid_layer, &warn, &err));

  const USDValidationResult valid_result =
      ValidateLayerAgainstAOUSDCore(valid_layer, AllGroups());
  TEST_CHECK(!HasRule(valid_result, "physics.extension.mjc.token"));

  const char *invalid_usda = R"(#usda 1.0

def MjcSensor "bad"
{
    token mjc:type = "notasensor"
}

def MjcSensor "badObject"
{
    token mjc:type = "gyro"
    token mjc:objtype = "definitely_not_an_object_type"
    token mjc:reftype = "also_bad"
}
)";

  Layer invalid_layer;
  TEST_CHECK(parse_layer(invalid_usda, &invalid_layer, &warn, &err));
  const USDValidationResult invalid_result =
      ValidateLayerAgainstAOUSDCore(invalid_layer, AllGroups());
  TEST_CHECK(HasRule(invalid_result, "physics.extension.mjc.token"));
}

void usd_validation_physics_newton_extension_test(void) {
  const char *usda = R"(#usda 1.0

def NewtonActuator "act"
{
    int newton:delaySteps = 0
    float newton:kp = -1
    asset newton:modelPath = @@
    float[] newton:lookupPositions = [0, 0]
    float[] newton:lookupEfforts = [1]
    rel newton:targets = </act>
}

def PhysicsScene "scene"
{
    int newton:maxSolverIterations = -1
    token newton:kamino:padmm:warmstarting = "hot"
    float newton:xpbd:softBodyRelaxation = 2
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.extension.newton.range"));
  TEST_CHECK(HasRule(result, "physics.extension.newton.asset"));
  TEST_CHECK(HasRule(result, "physics.extension.newton.lookup"));
  TEST_CHECK(HasRule(result, "physics.extension.newton.relationship"));
  TEST_CHECK(HasRule(result, "physics.extension.newton.token"));

  const char *valid_sentinel_usda = R"(#usda 1.0

def PhysicsScene "scene"
{
    int newton:maxSolverIterations = -1
}
)";
  Layer valid_layer;
  TEST_CHECK(parse_layer(valid_sentinel_usda, &valid_layer, &warn, &err));
  const USDValidationResult valid_result =
      ValidateLayerAgainstAOUSDCore(valid_layer, AllGroups());
  TEST_CHECK(!HasRule(valid_result, "physics.extension.newton.range"));
}

void usd_validation_physics_preliminary_test(void) {
  const char *usda = R"(#usda 1.0

def Preliminary_PhysicsGravitationalForce "gravity"
{
    double3 physics:gravitationalForce:acceleration = (0, -9.81, 0)
}

def Preliminary_InfiniteColliderPlane "plane"
{
    double3 normal = (0, 0, 0)
}

def Mesh "collider"
{
    double preliminary:physics:material:friction:static = -1
    double preliminary:physics:rigidBody:mass = 0
    token preliminary:physics:collider:convexShape = "notARelationship"
}
)";

  Layer layer;
  std::string warn;
  std::string err;
  TEST_CHECK(parse_layer(usda, &layer, &warn, &err));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "physics.preliminary.normal"));
  TEST_CHECK(HasRule(result, "physics.preliminary.range"));
  TEST_CHECK(HasRule(result, "physics.preliminary.relationship"));
}

void usd_validation_relationship_bind_material_as_metadata_test(void) {
  Layer layer;
  PrimSpec root(Specifier::Def, "Xform", "Root");
  Relationship rel;
  rel.set(Path("/Root", ""));
  rel.metas().set_bindMaterialAs(value::token("sideways"));
  root.props()["material:binding"] = Property(rel);
  TEST_CHECK(layer.add_primspec("Root", root));

  const USDValidationResult result =
      ValidateLayerAgainstAOUSDCore(layer, AllGroups());
  TEST_CHECK(HasRule(result, "core.relationship.bindMaterialAs"));
}
