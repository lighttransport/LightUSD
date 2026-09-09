// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
//
// AOUSD Core validation tests for src/next/validation: a valid USDA passes;
// specific rule violations are flagged with the expected rule ids; option
// group toggling controls which rules run.

#include "next/validation/usd-validation.hh"
#include "next/pcp/layer-registry.hh"

#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using namespace lightusd::next;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << "\n"; ++g_fail; }      \
    else { std::cout << "  ok: " << msg << "\n"; }                          \
  } while (0)

// Validate a USDA snippet (from memory) with the given options.
static bool Validate(const std::string &usda, const ValidationOptions &options,
                     USDValidationResult *result) {
  std::string warn;
  std::string err;
  const bool ok = ValidateUSDFromMemoryAgainstAOUSDCore(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "input.usda", options, result, &warn, &err);
  if (!ok) {
    std::cerr << "  parse error: " << err << "\n";
  }
  return ok;
}

static size_t CountRule(const USDValidationResult &result,
                        const std::string &rule_id,
                        USDValidationSeverity severity) {
  size_t n = 0;
  for (const auto &issue : result.issues) {
    if (issue.rule_id == rule_id && issue.severity == severity) {
      n++;
    }
  }
  return n;
}

static size_t CountErrors(const USDValidationResult &result,
                          const std::string &rule_id) {
  return CountRule(result, rule_id, USDValidationSeverity::Error);
}

static size_t CountWarnings(const USDValidationResult &result,
                            const std::string &rule_id) {
  return CountRule(result, rule_id, USDValidationSeverity::Warning);
}

static void DumpIssues(const USDValidationResult &result) {
  for (const USDValidationIssue *issue : GetOrderedValidationIssues(result)) {
    std::cerr << "    ["
              << (issue->severity == USDValidationSeverity::Error ? "E" : "W")
              << "] " << issue->rule_id << " @ " << issue->location << ": "
              << issue->message << "\n";
  }
}

// ---------------------------------------------------------------------------

static void test_valid_usda_passes() {
  std::cout << "[valid usda passes]\n";
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    timeCodesPerSecond = 24
)

def Xform "World"
{
    float3 xformOp:translate = (0, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        float3[] extent = [(0, 0, 0), (1, 1, 0)]
        token visibility = "inherited"
        token purpose = "default"
    }
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, MakeValidateAllOptions(), &result), "parses");
  if (!result.ok()) {
    DumpIssues(result);
  }
  CHECK(result.ok(), "no errors on a valid layer");
  CHECK(result.error_count() == 0, "error_count is 0");
  CHECK(result.checked_groups.core && result.checked_groups.geom &&
            result.checked_groups.shade && result.checked_groups.lux &&
            result.checked_groups.physics,
        "all requested groups ran");
  CHECK(!result.checked_groups.crate,
        "crate group never runs on the next validator");
}

static void test_layer_metadata_rules() {
  std::cout << "[core.layer.*]\n";
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "Missing"
    metersPerUnit = 0
    timeCodesPerSecond = -1
    framesPerSecond = 0
    kilogramsPerUnit = -2
    startTimeCode = 10
    endTimeCode = 1
)

def Xform "Root"
{
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.layer.defaultPrim") == 1,
        "missing defaultPrim root flagged");
  CHECK(CountErrors(result, "core.layer.metersPerUnit") == 1,
        "metersPerUnit <= 0 flagged");
  CHECK(CountErrors(result, "core.layer.timeCodesPerSecond") == 1,
        "timeCodesPerSecond <= 0 flagged");
  CHECK(CountErrors(result, "core.layer.framesPerSecond") == 1,
        "framesPerSecond <= 0 flagged");
  CHECK(CountErrors(result, "core.layer.kilogramsPerUnit") == 1,
        "kilogramsPerUnit <= 0 flagged");
  CHECK(CountWarnings(result, "core.layer.timeCodeRange") == 1,
        "startTimeCode > endTimeCode warns");
  CHECK(!result.ok(), "layer with metadata errors fails");
}

static void test_xform_op_order() {
  std::cout << "[core.xformOp.order]\n";
  const std::string usda = R"(#usda 1.0

def Xform "Root"
{
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    float3 xformOp:translate = (1, 2, 3)
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.xformOp.order") == 1,
        "missing xformOp attribute flagged");
}

static void test_api_schema_rules() {
  std::cout << "[core.apiSchema.* / core.schema.CollectionAPI.*]\n";
  const std::string usda = R"(#usda 1.0

def Xform "Root" (
    prepend apiSchemas = ["CollectionAPI", "MaterialBindingAPI", "MaterialBindingAPI"]
)
{
    uniform token collection:render:expansionRule = "bogusRule"
    rel collection:render:includes = </Root>
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.apiSchema.instance") == 1,
        "multiple-apply CollectionAPI without instance name flagged");
  CHECK(CountWarnings(result, "core.apiSchema.duplicate") == 1,
        "duplicate apiSchemas entry warns");
  CHECK(CountErrors(result, "core.schema.CollectionAPI.applied") >= 1,
        "collection property without applied instance flagged");
  CHECK(CountErrors(result, "core.schema.CollectionAPI.expansionRule") == 1,
        "invalid expansionRule token flagged");
}

static void test_attr_metadata_rules() {
  std::cout << "[core.attr.*]\n";
  const std::string usda = R"(#usda 1.0

def Mesh "M"
{
    token mode = "banana" (
        allowedTokens = ["apple", "orange"]
    )
    float[] primvars:foo = [1, 2, 3] (
        elementSize = 0
        interpolation = "sideways"
    )
    float bad:conn = 1 (
        connectability = "sometimes"
    )
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.attr.allowedTokens") == 1,
        "token outside allowedTokens flagged");
  CHECK(CountErrors(result, "core.attr.elementSize") == 1,
        "elementSize 0 flagged");
  CHECK(CountErrors(result, "core.attr.interpolation") == 1,
        "invalid interpolation flagged");
  CHECK(CountErrors(result, "core.attr.connectability") == 1,
        "invalid connectability flagged");
}

static void test_empty_over_and_variant_selection() {
  std::cout << "[core.composition.*]\n";
  const std::string usda = R"(#usda 1.0

over "Empty"
{
}

def Xform "Root" (
    variants = {
        string shapeVariant = "nosuch"
    }
    prepend variantSets = "shapeVariant"
)
{
    variantSet "shapeVariant" = {
        "round" {
        }
    }
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountWarnings(result, "core.composition.over") == 1,
        "empty typeless over warns");
  CHECK(CountErrors(result, "core.composition.variantSelection") == 1,
        "selection of unauthored variant flagged");
}

static void test_reference_arc_rules() {
  std::cout << "[core.composition.reference]\n";
  const std::string usda = R"(#usda 1.0

def Xform "Root" (
    prepend references = </Root/child property/oops>
)
{
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.composition.reference") >= 1,
        "invalid reference primPath flagged");
}

static void test_layer_offset_rules() {
  std::cout << "[layer offset validation]\n";
  Layer layer;
  layer.meta().subLayers = {"zero.usda", "negative.usda", "offset.usda",
                            "nan.usda"};
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  layer.meta().subLayerOffsets = {
      {0.0, 0.0}, {0.0, -1.0}, {inf, 1.0}, {0.0, nan}};

  PrimSpec prim("Root", "Xform");
  prim.set_path(Path("/Root"));
  prim.meta().layer_offset = {inf, nan};
  prim.meta().references.push_back(
      "@asset.usda@</Root>?layerOffset=1:0");
  const uint32_t index = layer.add_prim(std::move(prim));
  layer.add_root(index);
  layer.finalize();

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  CHECK(CountErrors(result, "core.layer.subLayerOffset") == 4,
        "zero/negative/non-finite sublayer offsets flagged");
  CHECK(CountErrors(result, "core.composition.reference") >= 3,
        "arc and prim layer offsets reject zero/non-finite components");
}

static void test_geom_group_toggle() {
  std::cout << "[geom group toggle]\n";
  const std::string usda = R"(#usda 1.0

def Mesh "Broken"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 9]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]
}
)";
  // core-only: geom rules must NOT run.
  USDValidationResult core_only;
  CHECK(Validate(usda, ValidationOptions(), &core_only), "parses (core)");
  CHECK(CountErrors(core_only, "geom.mesh.topology.size") == 0,
        "geom rules skipped when geom group is off");
  CHECK(core_only.ok(), "core-only validation passes the broken mesh");

  // geom on: the topology mismatch and out-of-range index are flagged.
  ValidationOptions geom_opts;
  geom_opts.geom = true;
  USDValidationResult with_geom;
  CHECK(Validate(usda, geom_opts, &with_geom), "parses (geom)");
  CHECK(CountErrors(with_geom, "geom.mesh.topology.size") == 1,
        "faceVertexCounts/Indices mismatch flagged");
  CHECK(CountErrors(with_geom, "geom.mesh.topology.index") == 1,
        "out-of-range faceVertexIndices flagged");
  CHECK(with_geom.checked_groups.geom && with_geom.checked_groups.core,
        "checked groups reflect the requested options");
}

static void test_geom_primitive_and_subset() {
  std::cout << "[geom.primitive / geom.subset]\n";
  const std::string usda = R"(#usda 1.0

def Sphere "Ball"
{
    double radius = -5
}

def Mesh "M"
{
    int[] faceVertexCounts = [3, 3]
    int[] faceVertexIndices = [0, 1, 2, 0, 2, 1]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]

    def GeomSubset "Sub"
    {
        uniform token elementType = "face"
        int[] indices = [0, 5]
    }
}
)";
  ValidationOptions opts;
  opts.geom = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses");
  CHECK(CountErrors(result, "geom.primitive.size") == 1,
        "negative sphere radius flagged");
  CHECK(CountErrors(result, "geom.subset.indices") == 1,
        "GeomSubset face index outside parent range flagged");
}

static void test_shade_rules() {
  std::cout << "[shade.*]\n";
  const std::string usda = R"(#usda 1.0

def Material "Mat"
{
    token outputs:surface
    token material:binding = "not_a_rel"

    def Shader "Preview"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:roughness = 2.5
        color3f inputs:notAnInput = (1, 0, 0)
    }
}
)";
  ValidationOptions opts;
  opts.shade = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses");
  CHECK(CountErrors(result, "shade.material.outputConnection") == 1,
        "unconnected Material terminal output flagged");
  CHECK(CountErrors(result, "shade.material.binding") == 1,
        "material:binding authored as attribute flagged");
  CHECK(CountWarnings(result, "shade.preview.inputRange") == 1,
        "out-of-range UsdPreviewSurface input warns");
  CHECK(CountWarnings(result, "shade.preview.unknownInput") == 1,
        "unknown UsdPreviewSurface input warns");
}

static void test_color_space_rules() {
  std::cout << "[core.schema.ColorSpace*]\n";
  const std::string usda = R"(#usda 1.0
def Scope "Alias" (
    prepend apiSchemas = ["ColorSpaceAPI"]
)
{
    uniform token colorSpace:name = "sRGB"
}
def Scope "Definitions" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:bad",
        "ColorSpaceDefinitionAPI:dupA", "ColorSpaceDefinitionAPI:dupB"]
)
{
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
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  CHECK(CountErrors(result, "core.schema.ColorSpaceAPI.name") == 0,
        "runtime-supported alias is accepted");
  CHECK(CountErrors(result,
                    "core.schema.ColorSpaceDefinitionAPI.invalid") == 1,
        "invalid chromaticities and curve are rejected");
  CHECK(CountErrors(result,
                    "core.schema.ColorSpaceDefinitionAPI.duplicate") == 1,
        "duplicate resolved definition names are rejected");
}

static void test_materialx_rules() {
  std::cout << "[shade.materialX.*]\n";
  const std::string usda = R"(#usda 1.0

def Material "BadMtlx"
{
    token config:mtlx:version = "2.0"
    token outputs:mtlx:surface

    def Shader "Node"
    {
        uniform token info:id = "ND_test_color3"
    }
}
)";
  ValidationOptions opts;
  opts.shade = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses");
  CHECK(CountWarnings(result, "shade.materialX.configAPI") == 1,
        "MaterialX properties without MaterialXConfigAPI flagged");
  CHECK(CountErrors(result, "shade.materialX.version") == 1,
        "non-string MaterialX version flagged");
  CHECK(CountErrors(result, "shade.material.outputConnection") == 1,
        "unconnected MaterialX terminal flagged");
  CHECK(CountWarnings(result, "shade.materialX.output") == 1,
        "MaterialX shader without an output flagged");
}

static void test_lux_rules() {
  std::cout << "[lux.*]\n";
  const std::string usda = R"(#usda 1.0

def SphereLight "Sun"
{
    float inputs:intensity = -100
    float inputs:radius = 0
}
)";
  ValidationOptions opts;
  opts.lux = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses");
  CHECK(CountErrors(result, "lux.light.inputs") == 1,
        "negative light intensity flagged");
  CHECK(CountErrors(result, "lux.light.size") == 1,
        "non-positive light radius flagged");
}

static void test_physics_rules() {
  std::cout << "[physics.*]\n";
  const std::string usda = R"(#usda 1.0

def Cube "Body" (
    prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]
)
{
    double size = 1
    float physics:mass = -3
}

def PhysicsRevoluteJoint "Hinge"
{
    uniform token physics:axis = "W"
    float physics:lowerLimit = 90
    float physics:upperLimit = -90
}
)";
  ValidationOptions opts;
  opts.physics = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses");
  CHECK(CountErrors(result, "physics.value.range") == 1,
        "negative physics:mass flagged");
  CHECK(CountErrors(result, "physics.joint.axis") == 1,
        "invalid physics:axis token flagged");
  CHECK(CountErrors(result, "physics.joint.limit") == 1,
        "lowerLimit > upperLimit flagged");
}

static void test_prim_name_and_kind() {
  std::cout << "[core.prim.*]\n";
  // Invalid prim names cannot be authored through the USDA parser, so build
  // the layer programmatically for core.prim.name.
  Layer layer;
  PrimSpec bad("123bad", "Xform");
  bad.set_path(Path("/123bad"));
  bad.meta().kind() = "not a token";
  uint32_t idx = layer.add_prim(std::move(bad));
  layer.add_root(idx);
  layer.finalize();

  const USDValidationResult result = ValidateLayerAgainstAOUSDCore(layer);
  CHECK(CountErrors(result, "core.prim.name") == 1,
        "invalid prim name flagged");
  CHECK(CountErrors(result, "core.prim.kind") == 1,
        "invalid kind token flagged");
}

static void test_arkit_rules() {
  std::cout << "[arkit.*]\n";
  const std::string usda = R"(#usda 1.0
(
    upAxis = "Z"
    subLayers = [
        @notes.txt@
    ]
)

def Xform "Asset"
{
    def BasisCurves "Hair"
    {
    }

    def Mesh "Body" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </Asset/Hair>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Asset/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            normal3f inputs:normal.connect = </Asset/Mat/Normal.outputs:rgb>
            color3f inputs:diffuseColor.connect = [
                </Asset/Mat/Normal.outputs:rgb>,
                </Asset/Mat/Tex.outputs:rgb>
            ]
            token outputs:surface
        }

        def Shader "Normal"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @normal.png@
            float3 outputs:rgb
        }

        def Shader "Tex"
        {
            uniform token info:implementationSource = "sourceCode"
            uniform token info:id = "MyBrdf"
            asset inputs:file = @albedo.tga@
            float3 outputs:rgb
        }
    }
}
)";

  // The arkit group is opt-in: none of it runs under MakeValidateAllOptions().
  USDValidationResult all_groups;
  CHECK(Validate(usda, MakeValidateAllOptions(), &all_groups), "parses (all)");
  CHECK(!all_groups.checked_groups.arkit,
        "arkit is not part of MakeValidateAllOptions()");
  size_t arkit_issues = 0;
  for (const auto &issue : all_groups.issues) {
    if (issue.rule_id.rfind("arkit.", 0) == 0) {
      arkit_issues++;
    }
  }
  CHECK(arkit_issues == 0, "no arkit.* rules run when the group is off");

  ValidationOptions opts;
  opts.core = false;
  opts.arkit = true;
  USDValidationResult result;
  CHECK(Validate(usda, opts, &result), "parses (arkit)");
  if (result.error_count() == 0) {
    DumpIssues(result);
  }
  CHECK(result.checked_groups.arkit, "arkit group reported as checked");
  CHECK(CountErrors(result, "arkit.stage.upAxis") == 1, "Z-up flagged");
  CHECK(CountErrors(result, "arkit.stage.metersPerUnit") == 1,
        "unauthored metersPerUnit flagged");
  CHECK(CountErrors(result, "arkit.stage.defaultPrim") == 1,
        "unauthored defaultPrim flagged");
  CHECK(CountErrors(result, "arkit.prim.type") == 1,
        "BasisCurves is outside the ARKit prim-type set");
  CHECK(CountErrors(result, "arkit.material.binding") == 1,
        "material:binding to a non-Material flagged");
  CHECK(CountErrors(result, "arkit.shader.implementationSource") == 1,
        "non-id implementation source flagged");
  CHECK(CountErrors(result, "arkit.shader.id") == 1,
        "unknown shader id flagged");
  CHECK(CountErrors(result, "arkit.shader.connection") == 1,
        "multiple connections on one input flagged");
  CHECK(CountErrors(result, "arkit.texture.format") == 1,
        "non-portable tga texture flagged");
  // scale, bias and sourceColorSpace are all missing on the normal texture.
  CHECK(CountErrors(result, "arkit.normalMap.scaleBias") == 3,
        "8-bit normal map without scale/bias/raw flagged");
  CHECK(CountErrors(result, "arkit.layer.extension") == 1,
        "non-layer subLayer extension flagged");

  const std::vector<std::string> groups =
      GetValidationGroupNames(result.checked_groups);
  CHECK(groups.size() == 1 && groups[0] == "arkit",
        "arkit appears last in the stable group-name order");

  // An ARKit-clean asset reports nothing.
  const std::string clean = R"(#usda 1.0
(
    defaultPrim = "Asset"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "Asset"
{
    def Mesh "Body" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </Asset/Mat>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Asset/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            normal3f inputs:normal.connect = </Asset/Mat/Normal.outputs:rgb>
            token outputs:surface
        }

        def Shader "Normal"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @normal.png@
            float4 inputs:scale = (2, 2, 2, 1)
            float4 inputs:bias = (-1, -1, -1, 0)
            token inputs:sourceColorSpace = "raw"
            float3 outputs:rgb
        }
    }
}
)";
  USDValidationResult clean_result;
  CHECK(Validate(clean, opts, &clean_result), "parses (clean)");
  if (!clean_result.ok()) {
    DumpIssues(clean_result);
  }
  CHECK(clean_result.ok(), "ARKit-clean asset has no errors");
}

// UsdVol / UsdRender structural checks (product parity).
static void test_vol_render_rules() {
  std::cout << "[vol/render rules]\n";
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Volume "Smoke"
    {
        token field:density = "oops"
        rel field:temperature
        rel field:velocity = </World/VelField>
    }
    def OpenVDBAsset "VelField"
    {
        token fieldDataType = "banana"
        token fieldClass = "levelSet"
    }
    def OpenVDBAsset "Animated"
    {
        asset filePath.timeSamples = {
            1: @./smoke.0001.vdb@,
            2: @./smoke.0002.vdb@,
        }
        token fieldDataType = "float"
    }
    def OpenVDBAsset "NoDataType"
    {
        asset filePath = @./still.vdb@
    }
    def OpenVDBAsset "DeclaredOnlyDataType"
    {
        asset filePath = @./still2.vdb@
        token fieldDataType
    }
    def Field3DAsset "F3D"
    {
        asset filePath = @./vol.f3d@
        token fieldDataType = "float"
    }
    def RenderSettings "Rs"
    {
        token aspectRatioConformPolicy = "bogusPolicy"
        rel products = </World/Rp>
        string orderedVars = "customAttrNotASchemaProperty"
    }
    def RenderProduct "Rp"
    {
        rel orderedVars = </World/Rv>
        rel camera = </World/Rv>
    }
    def RenderVar "Rv"
    {
        token sourceType = "telepathy"
    }
    def RenderVar "DeclaredOnly"
    {
        token sourceType
    }
    def RenderVar "AnimatedBad"
    {
        token sourceType.timeSamples = {
            1: "telepathy",
        }
    }
    def RenderVar "AnimatedGood"
    {
        token sourceType.timeSamples = {
            1: "lpe",
            2: "raw",
        }
    }
    def ParticleField3DGaussianSplat "Splats"
    {
        uniform token projectionModeHint = "fisheye"
        uniform token sortingModeHint = "random"
        point3f[] positions = [(0, 0, 0), (1, 0, 0)]
        float3[] scales = [(1, 1, 1)]
    }
    def Camera "PlateCamera" (
        prepend apiSchemas = ["BackPlateAPI:plate"]
    )
    {
        token backPlate:plate:plateVisibility = "sometimes"
    }
    def Xform "CardModel" (
        prepend apiSchemas = ["GeomModelAPI"]
    )
    {
        uniform token model:cardVisibility = "complex"
    }
    def RenderPass "Pass"
    {
        string renderSource = "/World/Rs"
    }
    def RenderPass "PassTargets"
    {
        rel renderSource = </World/Rv>
        rel inputPasses = </World/Rp>
    }
}
)";
  ValidationOptions options;
  options.geom = true;
  options.render = true;
  USDValidationResult result;
  CHECK(Validate(usda, options, &result), "vol/render usda parses");
  CHECK(CountErrors(result, "vol.volume.fieldRel") == 1,
        "field:* attribute (not relationship) is an error");
  CHECK(CountWarnings(result, "vol.volume.fieldRel") == 1,
        "targetless field relationship warns");
  CHECK(CountWarnings(result, "vol.fieldAsset.filePath") == 1,
        "only the asset without any filePath warns (time-sampled filePath "
        "is valid animated-volume authoring)");
  CHECK(CountErrors(result, "vol.fieldAsset.dataType") == 1,
        "invalid fieldDataType is an error");
  CHECK(CountWarnings(result, "vol.fieldAsset.dataType") == 2,
        "missing and declared-only fieldDataType VALUES warn (per-layer "
        "check; the value may arrive from another layer)");
  CHECK(CountErrors(result, "vol.fieldAsset.fieldClass") == 0,
        "valid fieldClass is clean");
  CHECK(CountErrors(result, "render.settings.aspectRatioConformPolicy") == 1,
        "invalid aspectRatioConformPolicy is an error");
  CHECK(CountErrors(result, "render.var.sourceType") == 2,
        "invalid sourceType errors (authored default AND animated sample); "
        "declared-only and validly-animated sourceType are clean");
  CHECK(CountErrors(result, "render.settings.relationship") == 0,
        "authored relationships are clean; a custom `orderedVars` attribute "
        "on RenderSettings is not kind-checked");
  CHECK(CountErrors(result, "vol.particleField.projectionModeHint") == 1 &&
            CountErrors(result, "vol.particleField.sortingModeHint") == 1,
        "invalid Gaussian rendering hints are errors");
  CHECK(CountWarnings(result, "vol.particleField.arraySize") == 1,
        "short Gaussian auxiliary arrays warn about OpenUSD discard behavior");
  CHECK(CountErrors(result, "geom.backPlate.visibility") == 1,
        "invalid BackPlateAPI visibility is an error");
  CHECK(CountErrors(result, "geom.model.cardVisibility") == 1,
        "invalid GeomModelAPI cardVisibility is an error");
  CHECK(CountErrors(result, "render.pass.relationship") == 1,
        "RenderPass renderSource must be a relationship");
  CHECK(CountWarnings(result, "render.product.targetType") == 1,
        "RenderProduct camera target type is checked");
  CHECK(CountWarnings(result, "render.pass.targetType") == 2,
        "RenderPass source/input pass target types are checked");
  if (g_fail) DumpIssues(result);

  // render.* rules are their own group: geom alone must not run them.
  ValidationOptions geom_only;
  geom_only.geom = true;
  USDValidationResult geom_result;
  CHECK(Validate(usda, geom_only, &geom_result), "geom-only re-run parses");
  CHECK(CountErrors(geom_result, "render.settings.aspectRatioConformPolicy") ==
                0 &&
            CountErrors(geom_result, "render.var.sourceType") == 0,
        "render rules do not run under the geom group");
}

static void test_parse_failure() {
  std::cout << "[parse failure]\n";
  const std::string garbage = "this is not a USD file {{{";
  USDValidationResult result;
  std::string warn;
  std::string err;
  const bool ok = ValidateUSDFromMemoryAgainstAOUSDCore(
      reinterpret_cast<const uint8_t *>(garbage.data()), garbage.size(),
      "garbage.usda", ValidationOptions(), &result, &warn, &err);
  CHECK(!ok, "unparseable input returns false");
  CHECK(!err.empty(), "parse error message reported");
}

static void test_report_formatting() {
  std::cout << "[report formatting]\n";
  const std::string usda = R"(#usda 1.0
(
    metersPerUnit = 0
)
def Xform "Root"
{
}
)";
  USDValidationResult result;
  CHECK(Validate(usda, ValidationOptions(), &result), "parses");
  const std::string report = FormatValidationResult(result);
  CHECK(report.find("AOUSD validation report") != std::string::npos,
        "report has a header");
  CHECK(report.find("core.layer.metersPerUnit") != std::string::npos,
        "report lists the rule id");
  CHECK(report.find("FAILED") != std::string::npos,
        "report states FAILED on errors");
  const std::vector<std::string> groups =
      GetValidationGroupNames(result.checked_groups);
  CHECK(groups.size() == 1 && groups[0] == "core",
        "default options report core group only");
}

int main() {
  size_t rule_count = 0;
  const ValidationRuleInfo* rules = GetValidationRuleTable(&rule_count);
  CHECK(rules && rule_count > 100, "borrowed validation registry is populated");
  CHECK(rules == GetValidationRuleTable(nullptr), "rule records have stable lifetime");
  bool complete_records = true;
  for (size_t i = 0; i < rule_count; ++i) {
    complete_records = complete_records && rules[i].id && rules[i].id[0] &&
        rules[i].group && rules[i].group[0] && rules[i].doc && rules[i].doc[0];
  }
  CHECK(complete_records, "rule records retain IDs, groups and descriptions");
  USDValidationResult compat;
  compat.issues.resize(2);
  compat.issues[0].rule_id = "geom.stage.upAxis";
  compat.issues[1].rule_id = "unlisted.rule";
  for (auto& issue : compat.issues) issue.severity = USDValidationSeverity::Warning;
  ApplyUsdcheckerCompatSeverities(&compat);
  ApplyUsdcheckerCompatSeverities(&compat);
  CHECK(compat.error_count() == 1 && compat.warning_count() == 1,
        "compatibility upgrades remain selective and idempotent");
  ApplyUsdcheckerCompatSeverities(nullptr);
  test_valid_usda_passes();
  test_layer_metadata_rules();
  test_xform_op_order();
  test_api_schema_rules();
  test_attr_metadata_rules();
  test_empty_over_and_variant_selection();
  test_reference_arc_rules();
  test_layer_offset_rules();
  test_geom_group_toggle();
  test_geom_primitive_and_subset();
  test_shade_rules();
  test_color_space_rules();
  test_materialx_rules();
  test_lux_rules();
  test_physics_rules();
  test_prim_name_and_kind();
  test_arkit_rules();
  test_parse_failure();
  test_vol_render_rules();
  test_report_formatting();

  if (g_fail) {
    std::cerr << g_fail << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "All validation tests passed\n";
  return 0;
}
