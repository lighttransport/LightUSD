#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-tydra-renderscene.h"

#include <cstring>
#include <cmath>

#include "math-util.inc"
#include "lightusd.hh"
#include "tydra/color-management.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-mesh-internal.hh"
#include "tydra/tangent-quantize.hh"
#include "tydra/render-data-converter.hh"
#include "tydra/scene-access.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "stage.hh"

using namespace lightusd;

namespace {

bool SyntheticTextureLoader(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver, tydra::TextureImage *imageOut,
    std::vector<uint8_t> *imageData, void *userdata, std::string *warn,
    std::string *err) {
  (void)assetInfo;
  (void)assetResolver;
  (void)userdata;
  (void)warn;

  if (!imageOut || !imageData) {
    if (err) {
      *err = "output buffer is null";
    }
    return false;
  }

  imageOut->asset_identifier = assetPath.GetAssetPath();
  imageOut->width = 1;
  imageOut->height = 1;
  imageOut->channels = 4;
  imageOut->assetTexelComponentType = tydra::ComponentType::UInt8;
  imageData->assign({255, 128, 64, 255});
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// a. Empty stage -> RenderScene should succeed with zero content
// ---------------------------------------------------------------------------
void tydra_renderscene_empty_stage_test(void) {
  Stage stage;
  stage.commit();

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  bool ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene should succeed on empty stage");

  TEST_CHECK(scene.meshes.size() == 0);
  TEST_MSG("Empty stage should produce 0 meshes");

  TEST_CHECK(scene.materials.size() == 0);
  TEST_MSG("Empty stage should produce 0 materials");

  TEST_CHECK(scene.lights.size() == 0);
  TEST_MSG("Empty stage should produce 0 lights");

  TEST_CHECK(scene.cameras.size() == 0);
  TEST_MSG("Empty stage should produce 0 cameras");
}

// ---------------------------------------------------------------------------
// b. Single triangle mesh -> parse, convert, verify mesh exists with vertices
// ---------------------------------------------------------------------------
void tydra_renderscene_single_mesh_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "Tri" {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.meshes.size() >= 1);
  TEST_MSG("Expected at least 1 mesh, got %zu", scene.meshes.size());

  TEST_CHECK(scene.meshes[0].points.size() > 0);
  TEST_MSG("Mesh should have vertices");
}

// ---------------------------------------------------------------------------
// c. Xform containing Mesh child -> verify nodes and meshes
// ---------------------------------------------------------------------------
void tydra_renderscene_xform_hierarchy_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Xform "Group" {
    def Mesh "ChildMesh" {
      point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.nodes.size() >= 1);
  TEST_MSG("Expected at least 1 node, got %zu", scene.nodes.size());

  TEST_CHECK(scene.meshes.size() >= 1);
  TEST_MSG("Expected at least 1 mesh, got %zu", scene.meshes.size());
}

// ---------------------------------------------------------------------------
// d. Mesh bound to Material with UsdPreviewSurface -> verify material
// ---------------------------------------------------------------------------
void tydra_renderscene_material_binding_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" {
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
        float inputs:roughness = 0.4
        float inputs:metallic = 0.0
        token outputs:surface
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.materials.size() >= 1);
  TEST_MSG("Expected at least 1 material, got %zu", scene.materials.size());
}

void tydra_renderscene_rendering_colorspace_test(void) {
  const char *usda = R"(#usda 1.0
(
  renderSettingsPrimPath = "/World/Settings"
)
def Xform "World" (
  prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio"]
) {
  uniform token colorSpaceDefinition:studio:name = "studio_linear"
  float2 colorSpaceDefinition:studio:redChroma = (0.64, 0.33)
  float2 colorSpaceDefinition:studio:greenChroma = (0.30, 0.60)
  float2 colorSpaceDefinition:studio:blueChroma = (0.15, 0.06)
  float2 colorSpaceDefinition:studio:whitePoint = (0.3127, 0.3290)
  float colorSpaceDefinition:studio:gamma = 1
  float colorSpaceDefinition:studio:linearBias = 0
  def RenderSettings "Settings" {
    uniform token renderingColorSpace = "studio_linear"
  }
  def Mesh "Mesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </World/Mat>
  }
  def Material "Mat" {
    token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
    def Shader "Surface" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.25, 0.5, 0.75) (
        colorSpace = "srgb_rec709_scene"
      )
      color3f inputs:diffuseColor.timeSamples = {
        1: (0.1, 0.2, 0.3),
        2: (0.9, 0.6, 0.1),
      }
      color3f inputs:emissiveColor = (0.25, 0.5, 0.75) (
        colorSpace = "raw"
      )
      token outputs:surface
    }
  }
}
def Scope "Legacy" (
  prepend apiSchemas = ["ColorSpaceDefinitionAPI"]
) {
  uniform token name = "legacy_linear"
  float2 redChroma = (0.64, 0.33)
  float2 greenChroma = (0.30, 0.60)
  float2 blueChroma = (0.15, 0.06)
  float2 whitePoint = (0.3127, 0.3290)
  float gamma = 1
  float linearBias = 0
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "colorspace.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());
  if (!ok) return;

  color::ColorSpaceDesc legacy_definition;
  std::string definition_error;
  ok = tydra::color_management::ResolveColorSpaceDefinition(
      stage, Path("/Legacy", ""), "legacy_linear", &legacy_definition,
      &definition_error);
  TEST_CHECK(ok);
  TEST_MSG("Legacy ColorSpaceDefinitionAPI failed: %s",
           definition_error.c_str());
  TEST_CHECK(color::IsLinear(legacy_definition));

  tydra::color_management::RenderingColorConfig fallback;
  std::string fallback_warning;
  ok = tydra::color_management::ResolveRenderingColorConfig(
      stage, "/missing", &fallback, &fallback_warning);
  TEST_CHECK(ok);
  TEST_CHECK(fallback.used_override);
  TEST_CHECK(fallback.used_fallback);
  TEST_CHECK(fallback.working_space == "lin_rec709_scene");
  TEST_CHECK(!fallback_warning.empty());

  tydra::RenderSceneConverterEnv env(stage);
  env.timecode = 1.5;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());
  if (!ok || scene.materials.empty() ||
      !scene.materials[0].surfaceShader) return;

  TEST_CHECK(scene.meta.renderSettingsPrimPath == "/World/Settings");
  TEST_CHECK(scene.meta.workingColorSpace == "studio_linear");
  const auto &c = scene.materials[0].surfaceShader->diffuseColor.value;
  float display[3] = {
      scene.meta.workingToDisplayLinear[0] * c[0] +
          scene.meta.workingToDisplayLinear[1] * c[1] +
          scene.meta.workingToDisplayLinear[2] * c[2],
      scene.meta.workingToDisplayLinear[3] * c[0] +
          scene.meta.workingToDisplayLinear[4] * c[1] +
          scene.meta.workingToDisplayLinear[5] * c[2],
      scene.meta.workingToDisplayLinear[6] * c[0] +
          scene.meta.workingToDisplayLinear[7] * c[1] +
          scene.meta.workingToDisplayLinear[8] * c[2]};
  // Interpolate authored samples in their source encoding first, then apply
  // the source-to-working conversion to midpoint (0.5, 0.4, 0.2).
  const float expected_display[3] = {0.214041f, 0.132868f, 0.0331048f};
  for (int channel = 0; channel < 3; ++channel) {
    TEST_CHECK(std::fabs(display[channel] - expected_display[channel]) <
               2.0e-4f);
    TEST_MSG("Unexpected display-linear channel %d: got %f", channel,
             display[channel]);
  }
  const auto &raw = scene.materials[0].surfaceShader->emissiveColor.value;
  const float expected_raw[3] = {0.25f, 0.5f, 0.75f};
  for (int channel = 0; channel < 3; ++channel) {
    TEST_CHECK(std::fabs(raw[channel] - expected_raw[channel]) < 1.0e-7f);
    TEST_MSG("Raw channel %d was color transformed: got %f", channel,
             raw[channel]);
  }
}

void tydra_renderscene_custom_texture_colorspace_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "World" (
  prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
) {
  uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
  float2 colorSpaceDefinition:studio_ap0:redChroma = (0.7348552434, 0.2642253252)
  float2 colorSpaceDefinition:studio_ap0:greenChroma = (-0.0061709125, 1.0113149590)
  float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.0159675593, -0.0642355031)
  float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
  float colorSpaceDefinition:studio_ap0:gamma = 1
  float colorSpaceDefinition:studio_ap0:linearBias = 0
  def Mesh "Mesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </World/Mat>
  }
  def Material "Mat" {
    token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
    def Shader "Surface" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
      token outputs:surface
    }
    def Shader "Texture" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @custom.png@ (colorSpace = "studio_ap0")
      float3 outputs:rgb
    }
  }
  def Xform "Alt" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
  ) {
    uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
    float2 colorSpaceDefinition:studio_ap0:redChroma = (0.64, 0.33)
    float2 colorSpaceDefinition:studio_ap0:greenChroma = (0.30, 0.60)
    float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.15, 0.06)
    float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio_ap0:gamma = 1
    float colorSpaceDefinition:studio_ap0:linearBias = 0
    def Mesh "Mesh" (
      prepend apiSchemas = ["MaterialBindingAPI"]
    ) {
      point3f[] points = [(2,0,0),(3,0,0),(2,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
      rel material:binding = </World/Alt/Mat>
    }
    def Material "Mat" {
      token outputs:surface.connect = </World/Alt/Mat/Surface.outputs:surface>
      def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </World/Alt/Mat/Texture.outputs:rgb>
        token outputs:surface
      }
      def Shader "Texture" {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @custom.png@ (colorSpace = "studio_ap0")
        float3 outputs:rgb
      }
    }
  }
  def Xform "Inherited" {
    def Mesh "Mesh" (
      prepend apiSchemas = ["MaterialBindingAPI"]
    ) {
      point3f[] points = [(4,0,0),(5,0,0),(4,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
      rel material:binding = </World/Inherited/Mat>
    }
    def Material "Mat" {
      token outputs:surface.connect = </World/Inherited/Mat/Surface.outputs:surface>
      def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </World/Inherited/Mat/Texture.outputs:rgb>
        token outputs:surface
      }
      def Shader "Texture" (
        prepend apiSchemas = ["ColorSpaceAPI"]
      ) {
        uniform token colorSpace:name = "lin_ap1_scene"
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @custom.png@
        float3 outputs:rgb
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "custom-texture-colorspace.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());
  if (!ok) return;

  tydra::RenderSceneConverterEnv env(stage);
  env.material_config.texture_image_loader_function = SyntheticTextureLoader;
  env.material_config.linearize_color_space = true;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());
  if (!ok || scene.images.empty()) return;

  // Same asset and token name, but two scoped definitions, plus an ordinary
  // UsdUVTexture inheriting ColorSpaceAPI. Each effective transform gets its
  // own cache entry.
  TEST_CHECK(scene.images.size() == 3);
  if (scene.images.size() != 3) return;
  const tydra::TextureImage *image_ptr = nullptr;
  const tydra::TextureImage *inherited_ptr = nullptr;
  for (const tydra::TextureImage &candidate : scene.images) {
    if (candidate.sourceToDisplayLinear[0] > 2.0f) image_ptr = &candidate;
    if (candidate.sourceColorSpaceName == "lin_ap1_scene") {
      inherited_ptr = &candidate;
    }
  }
  TEST_CHECK(image_ptr != nullptr);
  TEST_CHECK(inherited_ptr != nullptr);
  if (inherited_ptr) {
    TEST_CHECK(inherited_ptr->colorTransformValid);
    TEST_CHECK(!inherited_ptr->colorTransformBypass);
  }
  if (!image_ptr) return;
  const tydra::TextureImage &image = *image_ptr;
  TEST_CHECK(image.usdColorSpace == tydra::ColorSpace::Custom);
  TEST_CHECK(image.colorSpace == tydra::ColorSpace::Lin_sRGB);
  TEST_CHECK(image.sourceColorSpaceName == "studio_ap0");
  TEST_CHECK(image.colorTransformValid);
  TEST_CHECK(image.colorTransformApplied);
  TEST_CHECK(!image.colorTransformBypass);
  TEST_CHECK(image.buffer_id >= 0);
  if (image.buffer_id < 0) return;
  const tydra::BufferData &buffer =
      scene.buffers[static_cast<size_t>(image.buffer_id)];
  TEST_CHECK(buffer.componentType == tydra::ComponentType::Float);
  TEST_CHECK(buffer.data.size() == 4 * sizeof(float));
  if (buffer.data.size() != 4 * sizeof(float)) return;
  float actual[4];
  std::memcpy(actual, buffer.data.data(), sizeof(actual));
  color::ColorTransform expected_transform;
  ok = tydra::color_management::BuildColorTransform(
      stage, Path("/World/Mat/Texture", ""), "studio_ap0",
      "lin_rec709_scene", &expected_transform, &err);
  TEST_CHECK(ok);
  float expected[3] = {1.0f, 128.0f / 255.0f, 64.0f / 255.0f};
  color::TransformRGB(expected_transform, expected);
  for (int channel = 0; channel < 3; ++channel) {
    TEST_CHECK(std::fabs(actual[channel] - expected[channel]) < 2.0e-5f);
  }
  TEST_CHECK(std::fabs(actual[3] - 1.0f) < 1.0e-7f);
}

// ---------------------------------------------------------------------------
// d2. MaterialX NodeGraph constant folding of the newly-supported ops:
//     swizzle, separate (per-channel output), smoothstep, ifgreatereq, saturate.
//     A UsdPreviewSurface's inputs connect to NodeGraph outputs that fold to
//     constants; the converter must evaluate them into the surface shader.
// ---------------------------------------------------------------------------
void tydra_renderscene_mtlx_nodegraph_ops_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" {
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </Root/Materials/MyMat/NG.outputs:diff_out>
        color3f inputs:emissiveColor.connect = </Root/Materials/MyMat/NG.outputs:emis_out>
        color3f inputs:specularColor.connect = </Root/Materials/MyMat/NG.outputs:mixed_out>
        float inputs:roughness.connect = </Root/Materials/MyMat/NG.outputs:rough_out>
        float inputs:metallic.connect = </Root/Materials/MyMat/NG.outputs:metal_out>
        float inputs:clearcoat.connect = </Root/Materials/MyMat/NG.outputs:coat_out>
        float inputs:opacity.connect = </Root/Materials/MyMat/NG.outputs:opacity_out>
        token outputs:surface
      }
      def NodeGraph "NG" {
        color3f outputs:diff_out.connect = </Root/Materials/MyMat/NG/swizzleNode.outputs:out>
        color3f outputs:emis_out.connect = </Root/Materials/MyMat/NG/satNode.outputs:out>
        color3f outputs:mixed_out.connect = </Root/Materials/MyMat/NG/mixNode.outputs:out>
        float outputs:rough_out.connect = </Root/Materials/MyMat/NG/sepNode.outputs:outg>
        float outputs:metal_out.connect = </Root/Materials/MyMat/NG/smoothNode.outputs:out>
        float outputs:coat_out.connect = </Root/Materials/MyMat/NG/condNode.outputs:out>
        float outputs:opacity_out.connect = </Root/Materials/MyMat/NG/sep4Node.outputs:outa>

        def Shader "constNode" {
          uniform token info:id = "ND_constant_color3"
          color3f inputs:value = (0.1, 0.6, 0.9)
          color3f outputs:out
        }
        def Shader "swizzleNode" {
          uniform token info:id = "ND_swizzle_color3_color3"
          color3f inputs:in.connect = </Root/Materials/MyMat/NG/constNode.outputs:out>
          string inputs:channels = "bgr"
          color3f outputs:out
        }
        def Shader "sepNode" {
          uniform token info:id = "ND_separate3_color3"
          color3f inputs:in.connect = </Root/Materials/MyMat/NG/constNode.outputs:out>
          float outputs:outr
          float outputs:outg
          float outputs:outb
        }
        def Shader "smoothNode" {
          uniform token info:id = "ND_smoothstep_float"
          float inputs:in = 0.8
          float inputs:low = 0.0
          float inputs:high = 1.0
          float outputs:out
        }
        def Shader "condNode" {
          uniform token info:id = "ND_ifgreatereq_float"
          float inputs:value1 = 0.5
          float inputs:value2 = 0.5
          float inputs:in1 = 0.7
          float inputs:in2 = 0.2
          float outputs:out
        }
        def Shader "satNode" {
          uniform token info:id = "ND_saturate_color3"
          color3f inputs:in.connect = </Root/Materials/MyMat/NG/constNode.outputs:out>
          float inputs:amount = 0.0
          color3f outputs:out
        }
        def Shader "encodedColor" {
          uniform token info:id = "ND_constant_color3"
          color3f inputs:value = (0.5, 0, 0) (
            colorSpace = "srgb_rec709_scene"
          )
          color3f outputs:out
        }
        def Shader "linearColor" {
          uniform token info:id = "ND_constant_color3"
          color3f inputs:value = (0, 0.5, 0) (
            colorSpace = "lin_rec709_scene"
          )
          color3f outputs:out
        }
        def Shader "mixNode" {
          uniform token info:id = "ND_mix_color3"
          color3f inputs:bg.connect = </Root/Materials/MyMat/NG/encodedColor.outputs:out>
          color3f inputs:fg.connect = </Root/Materials/MyMat/NG/linearColor.outputs:out>
          float inputs:mix = 0.5
          color3f outputs:out
        }
        def Shader "combine4Node" {
          uniform token info:id = "ND_combine4_color4"
          float inputs:in1 = 0.2
          float inputs:in2 = 0.4
          float inputs:in3 = 0.6
          float inputs:in4 = 0.8
          color4f outputs:out
        }
        def Shader "sep4Node" {
          uniform token info:id = "ND_separate4_color4"
          color4f inputs:in.connect = </Root/Materials/MyMat/NG/combine4Node.outputs:out>
          float outputs:outr
          float outputs:outg
          float outputs:outb
          float outputs:outa
        }
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.materials.size() >= 1);
  if (scene.materials.empty()) return;
  const tydra::RenderMaterial &mat = scene.materials[0];
  TEST_CHECK(mat.surfaceShader.has_value());
  if (!mat.surfaceShader) return;
  const tydra::PreviewSurfaceShader &s = *mat.surfaceShader;

  // swizzle "bgr" of (0.1,0.6,0.9) -> (0.9,0.6,0.1)
  TEST_CHECK(math::is_close(s.diffuseColor.value[0], 0.9f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[1], 0.6f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[2], 0.1f, 1e-3f));
  TEST_MSG("swizzle diffuseColor = (%f,%f,%f)", s.diffuseColor.value[0],
           s.diffuseColor.value[1], s.diffuseColor.value[2]);

  // separate3(.outg) of (0.1,0.6,0.9) -> 0.6
  TEST_CHECK(math::is_close(s.roughness.value, 0.6f, 1e-3f));
  TEST_MSG("separate outg roughness = %f", s.roughness.value);

  // smoothstep(0.8, 0, 1) = 0.8^2*(3-2*0.8) = 0.896
  TEST_CHECK(math::is_close(s.metallic.value, 0.896f, 1e-3f));
  TEST_MSG("smoothstep metallic = %f", s.metallic.value);

  // ifgreatereq(0.5 >= 0.5) ? 0.7 : 0.2 -> 0.7
  TEST_CHECK(math::is_close(s.clearcoat.value, 0.7f, 1e-3f));
  TEST_MSG("ifgreatereq clearcoat = %f", s.clearcoat.value);

  // combine4 -> separate4(.outa) = 0.8
  TEST_CHECK(math::is_close(s.opacity.value, 0.8f, 1e-3f));
  TEST_MSG("combine4/separate4 opacity = %f", s.opacity.value);

  // saturate(amount=0) collapses to luminance:
  // 0.2126*0.1 + 0.7152*0.6 + 0.0722*0.9 = 0.51536
  TEST_CHECK(math::is_close(s.emissiveColor.value[0], 0.51536f, 1e-3f));
  TEST_CHECK(math::is_close(s.emissiveColor.value[1], 0.51536f, 1e-3f));
  TEST_CHECK(math::is_close(s.emissiveColor.value[2], 0.51536f, 1e-3f));
  TEST_MSG("saturate emissiveColor = (%f,%f,%f)", s.emissiveColor.value[0],
           s.emissiveColor.value[1], s.emissiveColor.value[2]);

  // Convert each source into the graph's linear Rec.709 space before mixing.
  TEST_MSG("mixed specularColor = (%f,%f,%f)", s.specularColor.value[0],
           s.specularColor.value[1], s.specularColor.value[2]);
  TEST_CHECK(math::is_close(s.specularColor.value[0], 0.1070205f, 1e-3f));
  TEST_CHECK(math::is_close(s.specularColor.value[1], 0.25f, 1e-3f));
  TEST_CHECK(math::is_close(s.specularColor.value[2], 0.0f, 1e-3f));
}

// ---------------------------------------------------------------------------
// d3. MaterialX NodeGraph constant folding when the graph is shared outside the
//     Material prim and forwards through a nested NodeGraph. Production assets
//     commonly keep reusable NodeGraphs next to a material library instead of
//     as children of a single Material.
// ---------------------------------------------------------------------------
void tydra_renderscene_mtlx_nonlocal_nodegraph_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" {
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </Root/SharedNG.outputs:diff_out>
        float inputs:roughness.connect = </Root/SharedNG.outputs:rough_out>
        token outputs:surface
      }
    }
  }
  def NodeGraph "SharedNG" {
    color3f outputs:diff_out.connect = </Root/SharedNG/Nested.outputs:diff_out>
    float outputs:rough_out.connect = </Root/SharedNG/Nested.outputs:rough_out>
    def NodeGraph "Nested" {
      color3f outputs:diff_out.connect = </Root/SharedNG/Nested/colorNode.outputs:out>
      float outputs:rough_out.connect = </Root/SharedNG/Nested/roughNode.outputs:out>
      def Shader "colorNode" {
        uniform token info:id = "ND_constant_color3"
        color3f inputs:value = (0.25, 0.5, 0.75) (
          colorSpace = "srgb_rec709_scene"
        )
        color3f outputs:out
      }
      def Shader "roughNode" {
        uniform token info:id = "ND_constant_float"
        float inputs:value = 0.35
        float outputs:out
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.materials.size() >= 1);
  if (scene.materials.empty()) return;
  const tydra::RenderMaterial &mat = scene.materials[0];
  TEST_CHECK(mat.surfaceShader.has_value());
  if (!mat.surfaceShader) return;
  const tydra::PreviewSurfaceShader &s = *mat.surfaceShader;

  TEST_CHECK(math::is_close(s.diffuseColor.value[0], 0.0508761f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[1], 0.214041f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[2], 0.522522f, 1e-3f));
  TEST_CHECK(math::is_close(s.roughness.value, 0.35f, 1e-3f));
  TEST_MSG("nonlocal NG diffuse=(%f,%f,%f) roughness=%f",
           s.diffuseColor.value[0], s.diffuseColor.value[1],
           s.diffuseColor.value[2], s.roughness.value);
}

// ---------------------------------------------------------------------------
// d4. MaterialX interface inputs. Flattened MaterialX commonly connects
//     surface-shader inputs to constants authored on the enclosing Material
//     interface instead of to texture or compute nodes.
// ---------------------------------------------------------------------------
void tydra_renderscene_mtlx_interface_inputs_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" {
      color3f inputs:ifaceColor = (0.25, 0.5, 0.75)
      float inputs:ifaceScalar = 0.4
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </Root/Materials/MyMat.inputs:ifaceColor>
        color3f inputs:emissiveColor.connect = </Root/Materials/MyMat.inputs:ifaceScalar>
        float inputs:roughness.connect = </Root/Materials/MyMat.inputs:ifaceColor>
        float inputs:metallic.connect = </Root/Materials/MyMat.inputs:ifaceScalar>
        token outputs:surface
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());
  TEST_CHECK(converter.GetWarning().find("MaterialX connection") ==
             std::string::npos);
  TEST_MSG("Unexpected MaterialX interface warning: %s",
           converter.GetWarning().c_str());

  TEST_CHECK(scene.materials.size() >= 1);
  if (scene.materials.empty()) return;
  const tydra::RenderMaterial &mat = scene.materials[0];
  TEST_CHECK(mat.surfaceShader.has_value());
  if (!mat.surfaceShader) return;
  const tydra::PreviewSurfaceShader &s = *mat.surfaceShader;

  TEST_CHECK(math::is_close(s.diffuseColor.value[0], 0.25f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[1], 0.5f, 1e-3f));
  TEST_CHECK(math::is_close(s.diffuseColor.value[2], 0.75f, 1e-3f));
  TEST_CHECK(math::is_close(s.emissiveColor.value[0], 0.4f, 1e-3f));
  TEST_CHECK(math::is_close(s.emissiveColor.value[1], 0.4f, 1e-3f));
  TEST_CHECK(math::is_close(s.emissiveColor.value[2], 0.4f, 1e-3f));
  TEST_CHECK(math::is_close(s.roughness.value, 0.25f, 1e-3f));
  TEST_CHECK(math::is_close(s.metallic.value, 0.4f, 1e-3f));
}

// ---------------------------------------------------------------------------
// d5. MaterialX geompropvalue texture coordinates. ND_image texcoord chains
//     that pass through ND_geompropvalue must preserve the authored primvar
//     name on the renderer-facing UVTexture.
// ---------------------------------------------------------------------------
void tydra_renderscene_mtlx_geomprop_texture_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    texCoord2f[] primvars:uv_custom = [(0,0),(1,0),(0,1)] (
      interpolation = "vertex"
    )
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" {
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </Root/Materials/MyMat/NG.outputs:diff_out>
        token outputs:surface
      }
      def NodeGraph "NG" {
        color3f outputs:diff_out.connect = </Root/Materials/MyMat/NG/Image.outputs:out>
        def Shader "Image" {
          uniform token info:id = "ND_image_color3"
          asset inputs:file = @synthetic.png@
          float2 inputs:texcoord.connect = </Root/Materials/MyMat/NG/GeomProp.outputs:out>
          color3f outputs:out
        }
        def Shader "GeomProp" {
          uniform token info:id = "ND_geompropvalue_vector2"
          string inputs:geomprop = "uv_custom"
          float2 outputs:out
        }
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  env.material_config.texture_image_loader_function = SyntheticTextureLoader;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.textures.size() >= 1);
  TEST_MSG("Expected at least 1 texture, got %zu", scene.textures.size());
  if (scene.textures.empty()) return;
  TEST_CHECK(scene.textures[0].varname_uv == "uv_custom");
  TEST_MSG("MaterialX geomprop varname_uv = '%s'",
           scene.textures[0].varname_uv.c_str());
  TEST_CHECK(scene.images.size() >= 1);
  if (!scene.images.empty()) {
    TEST_CHECK(scene.images[0].asset_identifier == "synthetic.png");
  }
}

// ---------------------------------------------------------------------------
// d6. MaterialX texture color-space semantics. Untagged color parameters use
//     the MaterialX document working space, while scalar/data parameters stay
//     Raw. This is distinct from the historical sRGB fallback used when no
//     MaterialXConfigAPI opinion exists.
// ---------------------------------------------------------------------------
void tydra_renderscene_mtlx_texture_colorspace_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "MyMesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MyMat>
  }
  def Scope "Materials" {
    def Material "MyMat" (
      prepend apiSchemas = ["MaterialXConfigAPI"]
    ) {
      string config:mtlx:colorspace = "acescg"
      token outputs:surface.connect = </Root/Materials/MyMat/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </Root/Materials/MyMat/NG.outputs:diff_out>
        color3f inputs:emissiveColor.connect = </Root/Materials/MyMat/NG.outputs:api_out>
        float inputs:roughness.connect = </Root/Materials/MyMat/NG.outputs:rough_out>
        token outputs:surface
      }
      def NodeGraph "NG" {
        color3f outputs:diff_out.connect = </Root/Materials/MyMat/NG/ColorImage.outputs:out>
        color3f outputs:api_out.connect = </Root/Materials/MyMat/NG/ApiImage.outputs:out>
        float outputs:rough_out.connect = </Root/Materials/MyMat/NG/RoughImage.outputs:out>
        def Shader "ColorImage" {
          uniform token info:id = "ND_image_color3"
          asset inputs:file = @shared.png@
          color3f outputs:out
        }
        def Shader "RoughImage" {
          uniform token info:id = "ND_image_float"
          asset inputs:file = @shared.png@
          float outputs:out
        }
        def Shader "ApiImage" (
          prepend apiSchemas = ["ColorSpaceAPI"]
        ) {
          uniform token colorSpace:name = "lin_rec709_scene"
          uniform token info:id = "ND_image_color3"
          asset inputs:file = @shared.png@
          color3f outputs:out
        }
      }
    }
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  std::string inherited_source;
  bool inherited_authored = false;
  ok = tydra::color_management::ComputeColorSpaceName(
      stage, Path("/Root/Materials/MyMat/NG/ApiImage", ""), nullptr,
      &inherited_source, &inherited_authored);
  TEST_CHECK(ok);
  TEST_CHECK(inherited_authored);
  TEST_MSG("Expected ColorSpaceAPI on ApiImage; resolved '%s'",
           inherited_source.c_str());
  TEST_CHECK(inherited_source == "lin_rec709_scene");

  tydra::RenderSceneConverterEnv env(stage);
  env.material_config.texture_image_loader_function = SyntheticTextureLoader;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.materials.size() >= 1);
  if (scene.materials.empty()) return;
  const tydra::RenderMaterial &mat = scene.materials[0];
  TEST_CHECK(mat.surfaceShader.has_value());
  if (!mat.surfaceShader) return;
  const tydra::PreviewSurfaceShader &s = *mat.surfaceShader;
  TEST_CHECK(s.diffuseColor.texture_id >= 0);
  TEST_CHECK(s.emissiveColor.texture_id >= 0);
  TEST_CHECK(s.roughness.texture_id >= 0);
  if (s.diffuseColor.texture_id < 0 || s.emissiveColor.texture_id < 0 ||
      s.roughness.texture_id < 0) return;
  TEST_CHECK(scene.images.size() == 3);

  const auto textureImageFor = [&](int32_t texture_id)
      -> const tydra::TextureImage & {
    const tydra::UVTexture &tex = scene.textures[static_cast<size_t>(texture_id)];
    return scene.images[static_cast<size_t>(tex.texture_image_id)];
  };

  const tydra::TextureImage &color_image =
      textureImageFor(s.diffuseColor.texture_id);
  TEST_CHECK(color_image.usdColorSpace == tydra::ColorSpace::Lin_ACEScg);
  TEST_CHECK(color_image.sourceColorSpaceName == "lin_ap1_scene");
  TEST_CHECK(color_image.colorTransformValid);
  TEST_CHECK(!color_image.colorTransformApplied);
  TEST_CHECK(!color_image.colorTransformBypass);
  TEST_CHECK(std::fabs(color_image.sourceToDisplayLinear[0] - 1.70505f) <
             1.0e-4f);

  const tydra::TextureImage &inherited_image =
      textureImageFor(s.emissiveColor.texture_id);
  TEST_CHECK(inherited_image.usdColorSpace == tydra::ColorSpace::Lin_Rec709);
  TEST_CHECK(inherited_image.sourceColorSpaceName == "lin_rec709_scene");
  TEST_CHECK(inherited_image.colorTransformValid);
  TEST_CHECK(inherited_image.colorTransformBypass);

  const tydra::TextureImage &data_image =
      textureImageFor(s.roughness.texture_id);
  TEST_CHECK(data_image.usdColorSpace == tydra::ColorSpace::Raw);
  TEST_CHECK(data_image.sourceColorIsData);
  TEST_CHECK(data_image.colorTransformBypass);
}

// ---------------------------------------------------------------------------
// e. SphereLight -> verify light in converted scene
// ---------------------------------------------------------------------------
void tydra_renderscene_sphere_light_test(void) {
  const char *usda = R"(#usda 1.0
def SphereLight "MyLight" {
  float inputs:intensity = 100
  color3f inputs:color = (1, 1, 1)
  float inputs:radius = 0.5
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.lights.size() >= 1);
  TEST_MSG("Expected at least 1 light, got %zu", scene.lights.size());
}

// ---------------------------------------------------------------------------
// f. Camera -> verify camera in converted scene
// ---------------------------------------------------------------------------
void tydra_renderscene_camera_test(void) {
  const char *usda = R"(#usda 1.0
def Camera "MainCam" {
  float focalLength = 50
  float horizontalAperture = 36
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.cameras.size() >= 1);
  TEST_MSG("Expected at least 1 camera, got %zu", scene.cameras.size());
}

// ---------------------------------------------------------------------------
// g. Memory estimation -> convert scene, check estimate_memory_usage() > 0
// ---------------------------------------------------------------------------
void tydra_renderscene_memory_estimation_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "Quad" {
    point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  size_t mem = scene.estimate_memory_usage();
  TEST_CHECK(mem > 0);
  TEST_MSG("estimate_memory_usage() should be > 0, got %zu", mem);

  // Also verify that at least the arrays are non-empty as a sanity check
  bool has_content = !scene.nodes.empty() || !scene.meshes.empty();
  TEST_CHECK(has_content);
  TEST_MSG("Converted scene should have non-empty arrays");
}

// ---------------------------------------------------------------------------
// Shared fixture for the streaming tests: two meshes (one with a bound
// material), a camera and a light under an Xform.
// ---------------------------------------------------------------------------
static const char *kStreamingUSDA = R"(#usda 1.0
(
  upAxis = "Y"
)
def Xform "Root" {
  def Mesh "MeshA" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </Root/Materials/MatA>
  }
  def Mesh "MeshB" {
    point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
  }
  def Scope "Materials" {
    def Material "MatA" {
      token outputs:surface.connect = </Root/Materials/MatA/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
        token outputs:surface
      }
    }
  }
  def Camera "Cam" {
    float focalLength = 50
  }
  def SphereLight "Light" {
    float inputs:intensity = 100
  }
}
)";

// ---------------------------------------------------------------------------
// h. Streaming conversion equivalence: ConvertToRenderSceneStreaming must
//    deliver every element via the sink (counts == final array sizes), in the
//    documented order (material before the mesh that binds it; no node before
//    the last mesh), fire on_complete exactly once, and produce a RenderScene
//    equal to the monolithic ConvertToRenderScene.
// ---------------------------------------------------------------------------
void tydra_renderscene_streaming_equivalence_test(void) {
  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(kStreamingUSDA),
      std::strlen(kStreamingUSDA), "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  // Monolithic reference.
  tydra::RenderScene sceneA;
  {
    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderSceneConverter converter;
    ok = converter.ConvertToRenderScene(env, &sceneA);
    TEST_CHECK(ok);
    TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());
  }

  // Streaming.
  struct StreamLog {
    size_t images = 0, buffers = 0, textures = 0, materials = 0, meshes = 0;
    size_t lights = 0, cameras = 0, nodes = 0;
    size_t skeletons = 0, animations = 0, instances = 0;
    size_t completes = 0;
    bool material_before_mesh_ok = true;  // each mesh.material_id < materials seen
    bool node_seen = false;
    bool mesh_after_node = false;  // a mesh arrived after a node (ordering bug)
  } log;

  tydra::RenderSceneSink sink;
  sink.on_image = [&](const tydra::TextureImage &, size_t, void *) {
    log.images++; return true; };
  sink.on_buffer = [&](const tydra::BufferData &, size_t, void *) {
    log.buffers++; return true; };
  sink.on_texture = [&](const tydra::UVTexture &, size_t, const std::string &, void *) {
    log.textures++; return true; };
  sink.on_material = [&](const tydra::RenderMaterial &, size_t, const std::string &, void *) {
    log.materials++; return true; };
  sink.on_mesh = [&](const tydra::RenderMesh &m, size_t, const std::string &, void *) {
    if (m.material_id >= 0 && size_t(m.material_id) >= log.materials) {
      log.material_before_mesh_ok = false;
    }
    if (log.node_seen) log.mesh_after_node = true;
    log.meshes++;
    return true;
  };
  sink.on_light = [&](const tydra::RenderLight &, size_t, const std::string &, void *) {
    log.lights++; return true; };
  sink.on_camera = [&](const tydra::RenderCamera &, size_t, const std::string &, void *) {
    log.cameras++; return true; };
  sink.on_root_node = [&](const tydra::Node &, size_t, void *) {
    log.node_seen = true; log.nodes++; return true; };
  sink.on_skeleton = [&](const tydra::SkelHierarchy &, size_t, const std::string &, void *) {
    log.skeletons++; return true; };
  sink.on_animation = [&](const tydra::AnimationClip &, size_t, const std::string &, void *) {
    log.animations++; return true; };
  sink.on_instance = [&](const tydra::RenderInstance &, size_t, const std::string &, void *) {
    log.instances++; return true; };
  sink.on_complete = [&](const tydra::RenderScene &, void *) {
    log.completes++; return true; };

  tydra::RenderScene sceneB;
  {
    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderSceneConverter converter;
    ok = converter.ConvertToRenderSceneStreaming(env, sink, &sceneB);
    TEST_CHECK(ok);
    TEST_MSG("ConvertToRenderSceneStreaming failed: %s",
             converter.GetError().c_str());
  }

  // Streamed counts match the streamed scene's arrays.
  TEST_CHECK(log.meshes == sceneB.meshes.size());
  TEST_MSG("on_mesh count %zu != meshes %zu", log.meshes, sceneB.meshes.size());
  TEST_CHECK(log.materials == sceneB.materials.size());
  TEST_MSG("on_material count %zu != materials %zu", log.materials,
           sceneB.materials.size());
  TEST_CHECK(log.textures == sceneB.textures.size());
  TEST_CHECK(log.images == sceneB.images.size());
  TEST_CHECK(log.buffers == sceneB.buffers.size());
  TEST_CHECK(log.lights == sceneB.lights.size());
  TEST_CHECK(log.cameras == sceneB.cameras.size());
  TEST_CHECK(log.nodes == sceneB.nodes.size());
  TEST_CHECK(log.skeletons == sceneB.skeletons.size());
  TEST_CHECK(log.animations == sceneB.animations.size());
  TEST_CHECK(log.instances == sceneB.instances.size());

  // Ordering guarantees.
  TEST_CHECK(log.material_before_mesh_ok);
  TEST_MSG("a mesh referenced a material that was not emitted before it");
  TEST_CHECK(!log.mesh_after_node);
  TEST_MSG("a mesh was streamed after a node (meshes must precede the hierarchy)");

  // on_complete fires exactly once.
  TEST_CHECK(log.completes == 1);
  TEST_MSG("on_complete fired %zu times (expected 1)", log.completes);

  // Streaming result equals the monolithic result.
  TEST_CHECK(sceneA.meshes.size() == sceneB.meshes.size());
  TEST_CHECK(sceneA.materials.size() == sceneB.materials.size());
  TEST_CHECK(sceneA.nodes.size() == sceneB.nodes.size());
  TEST_CHECK(sceneA.lights.size() == sceneB.lights.size());
  TEST_CHECK(sceneA.cameras.size() == sceneB.cameras.size());
  bool per_mesh_ok = true;
  for (size_t i = 0; i < sceneA.meshes.size() && i < sceneB.meshes.size(); i++) {
    if (sceneA.meshes[i].points.size() != sceneB.meshes[i].points.size() ||
        sceneA.meshes[i].material_id != sceneB.meshes[i].material_id) {
      per_mesh_ok = false;
    }
  }
  TEST_CHECK(per_mesh_ok);
  TEST_MSG("streamed meshes differ from monolithic meshes");

  // Sanity: the fixture really exercises meshes + a material.
  TEST_CHECK(sceneB.meshes.size() == 2);
  TEST_CHECK(sceneB.materials.size() >= 1);
}

// ---------------------------------------------------------------------------
// i. Streaming cancellation: a sink that returns false from on_mesh aborts the
//    conversion; the consumer keeps exactly the meshes delivered so far.
// ---------------------------------------------------------------------------
void tydra_renderscene_streaming_cancel_test(void) {
  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(kStreamingUSDA),
      std::strlen(kStreamingUSDA), "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  size_t meshes_seen = 0;
  tydra::RenderSceneSink sink;
  sink.on_mesh = [&](const tydra::RenderMesh &, size_t, const std::string &, void *) {
    meshes_seen++;
    return false;  // cancel on the very first mesh
  };

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  ok = converter.ConvertToRenderSceneStreaming(env, sink, &scene);

  TEST_CHECK(!ok);
  TEST_MSG("streaming conversion should report failure when cancelled");
  TEST_CHECK(meshes_seen == 1);
  TEST_MSG("expected exactly 1 mesh delivered before cancel, got %zu",
           meshes_seen);
}

// ---------------------------------------------------------------------------
// Legacy-audit regressions: holeIndices skipped during triangulation and
// leftHanded meshes get flipped computed normals.
// ---------------------------------------------------------------------------
void tydra_renderscene_hole_indices_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root" {
  def Mesh "TwoQuads" {
    point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0),(2,0,0),(2,1,0)]
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0,1,2,3, 1,4,5,2]
    int[] holeIndices = [0]
  }
}
)";

  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  TEST_CHECK(ok);
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  env.mesh_config.triangulate = true;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  TEST_CHECK(ok);
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.meshes.size() == 1);
  TEST_MSG("Expected 1 mesh, got %zu", scene.meshes.size());

  const tydra::RenderMesh &rmesh = scene.meshes[0];

  // 2 quads = 4 triangles, but face 0 is a hole => only 2 triangles.
  TEST_CHECK(rmesh.triangulatedFaceVertexCounts.size() == 2);
  TEST_MSG("Hole face must be removed: expected 2 triangles, got %zu",
           rmesh.triangulatedFaceVertexCounts.size());

  // All triangulated indices must reference the non-hole face's vertices
  // {1, 4, 5, 2} only.
  for (size_t i = 0; i < rmesh.triangulatedFaceVertexIndices.size(); i++) {
    uint32_t idx = rmesh.triangulatedFaceVertexIndices[i];
    bool from_face1 = (idx == 1) || (idx == 4) || (idx == 5) || (idx == 2);
    TEST_CHECK(from_face1);
    TEST_MSG("triangulated index %u does not belong to the non-hole face",
             idx);
  }
}

// ---------------------------------------------------------------------------
// i. leftHanded orientation -> computed normals must be flipped
// ---------------------------------------------------------------------------
static bool GetComputedNormalZ(const char *usda, float *out_z) {
  Stage stage;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", &stage, &warn, &err);
  if (!ok) {
    return false;
  }

  tydra::RenderSceneConverterEnv env(stage);
  env.mesh_config.triangulate = true;
  env.mesh_config.compute_normals = true;
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;

  ok = converter.ConvertToRenderScene(env, &scene);
  if (!ok || scene.meshes.size() != 1) {
    return false;
  }

  const tydra::RenderMesh &rmesh = scene.meshes[0];
  const std::vector<uint8_t> &data = rmesh.normals.get_data();
  if (data.size() < 3 * sizeof(float)) {
    return false;
  }

  const float *fp = reinterpret_cast<const float *>(data.data());
  (*out_z) = fp[2];  // z component of the first normal
  return true;
}

void tydra_renderscene_lefthanded_normals_test(void) {
  // CCW triangle in the XY plane. Geometric(CCW) normal is +Z.
  const char *usda_right = R"(#usda 1.0
def Mesh "Tri" {
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  uniform token orientation = "rightHanded"
}
)";

  // Same index order, but leftHanded orientation: faces wind clockwise,
  // so the geometric normal must be -Z.
  const char *usda_left = R"(#usda 1.0
def Mesh "Tri" {
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  uniform token orientation = "leftHanded"
}
)";

  float rz{0.0f};
  bool ok = GetComputedNormalZ(usda_right, &rz);
  TEST_CHECK(ok);
  TEST_MSG("Failed to compute normals for rightHanded mesh");
  TEST_CHECK(rz > 0.5f);
  TEST_MSG("rightHanded mesh: expected +Z normal, got z = %f", double(rz));

  float lz{0.0f};
  ok = GetComputedNormalZ(usda_left, &lz);
  TEST_CHECK(ok);
  TEST_MSG("Failed to compute normals for leftHanded mesh");
  TEST_CHECK(lz < -0.5f);
  TEST_MSG("leftHanded mesh: expected -Z normal, got z = %f", double(lz));
}

void tydra_deferred_packed_normals_test(void) {
  using namespace lightusd::tydra;
  using namespace lightusd::tydra::tangent_quantize;
  const VertexAttributeFormat formats[] = {VertexAttributeFormat::Vec3,
      VertexAttributeFormat::Char3, VertexAttributeFormat::Short3, VertexAttributeFormat::Uint};
  for (auto format : formats) {
    for (bool face_uv : {false, true}) {
      for (auto method : {MeshConverterConfig::TangentComputationMethod::Lengyel,
                          MeshConverterConfig::TangentComputationMethod::MikkTSpace,
                          MeshConverterConfig::TangentComputationMethod::FastMikkTSpace,
                          MeshConverterConfig::TangentComputationMethod::Hybrid}) {
      RenderMesh mesh;
      mesh.points = {{0,0,0}, {1,0,0}, {0,1,0}, {1,1,0}};
      mesh.usdFaceVertexCounts = {3,3};
      mesh.usdFaceVertexIndices = {2,0,1,2,1,3};
      mesh.is_single_indexable = !face_uv;
      mesh.normals.format = format;
      mesh.normals.variability = VertexVariability::Vertex;
      for (int i = 0; i < 4; ++i) {
        const vec3 normal{0,0,1};
        const auto n8 = pack_normal_snorm8(0,0,1);
        const auto n16 = pack_normal_snorm16(0,0,1);
        const auto n32 = pack_normal_1010102(0,0,1);
        const void *data = &normal;
        size_t bytes = sizeof(normal);
        if (format == VertexAttributeFormat::Char3) { data = &n8; bytes = sizeof(n8); }
        if (format == VertexAttributeFormat::Short3) { data = &n16; bytes = sizeof(n16); }
        if (format == VertexAttributeFormat::Uint) { data = &n32; bytes = sizeof(n32); }
        const auto *begin = static_cast<const uint8_t *>(data);
        mesh.normals.data.insert(mesh.normals.data.end(), begin, begin + bytes);
      }
      auto &uv = mesh.texcoords[0];
      uv.format = VertexAttributeFormat::Vec2;
      uv.variability = face_uv ? VertexVariability::FaceVarying : VertexVariability::Vertex;
      const std::vector<vec2> coords = face_uv
          ? std::vector<vec2>{{0,1},{0,0},{1,0},{1,0},{0,-1},{1,-1}}
          : std::vector<vec2>{{0,0},{1,0},{0,1},{1,1}};
      uv.set_buffer(reinterpret_cast<const uint8_t *>(coords.data()), coords.size() * sizeof(vec2));
      mesh.tangent_computation_deferred = true;
      const auto normal_bytes = mesh.normals.data;
      std::string err;
      TEST_CHECK(RenderSceneConverter::ComputeDeferredTangents(&mesh,
          method,
          MeshConverterConfig::TangentStorageFormat::Packed1010102, &err));
      TEST_MSG("%s", err.c_str());
      TEST_CHECK(!mesh.tangent_computation_deferred);
      TEST_CHECK(mesh.normals.data == normal_bytes);
      TEST_CHECK(mesh.normals.format == format);
      TEST_CHECK(mesh.tangents.format == VertexAttributeFormat::Uint);
      TEST_CHECK(mesh.tangents.vertex_count() == 6);
      for (size_t i = 0; i < mesh.tangents.vertex_count(); ++i) {
        PackedTangent1010102 packed;
        memcpy(&packed, mesh.tangents.data.data() + i * sizeof(packed), sizeof(packed));
        float x, y, z, w;
        unpack_tangent_1010102(packed, x, y, z, w);
        const bool rotated = face_uv && i >= 3;
        TEST_CHECK(std::fabs(x - (rotated ? 0.0f : 1.0f)) < 0.005f);
        TEST_CHECK(std::fabs(y - (rotated ? 1.0f : 0.0f)) < 0.005f);
        TEST_CHECK(std::fabs(z) < 0.005f);
        TEST_CHECK(w == 1.0f);
        vec3 normal;
        TEST_CHECK(ReadNormalForTangent(mesh, i, &normal));
        TEST_CHECK(std::fabs(normal[2] - 1) < 0.005f);
      }
      mesh.tangent_computation_deferred = true;
      mesh.normals.data.resize(1);
      TEST_CHECK(!RenderSceneConverter::ComputeDeferredTangents(&mesh,
          method,
          MeshConverterConfig::TangentStorageFormat::Packed1010102, &err));
      TEST_CHECK(mesh.tangent_computation_deferred);
      }
    }
  }
  // Equal normal/corner counts still require vertex-index remapping.
  RenderMesh indexed;
  indexed.usdFaceVertexCounts = {3};
  indexed.usdFaceVertexIndices = {2,0,1};
  indexed.tangents.variability = VertexVariability::FaceVarying;
  const vec3 normals[] = {{0,0,1},{0,1,0},{1,0,0}};
  indexed.normals.set_buffer(reinterpret_cast<const uint8_t *>(normals), sizeof(normals));
  vec3 remapped;
  TEST_CHECK(ReadNormalForTangent(indexed, 0, &remapped));
  TEST_CHECK(remapped[0] == 1 && remapped[1] == 0 && remapped[2] == 0);
  indexed.normals.indices = {1,2,0};
  TEST_CHECK(ReadNormalForTangent(indexed, 0, &remapped));
  TEST_CHECK(remapped[0] == 0 && remapped[1] == 1 && remapped[2] == 0);
  indexed.normals.indices[0] = 999;
  TEST_CHECK(!ReadNormalForTangent(indexed, 0, &remapped));
  VertexAttribute normal;
  normal.format = VertexAttributeFormat::Short3;
  normal.data = {0,0, 0,0, 255,127, 0,0};
  normal.stride = 8;
  vec3 decoded;
  TEST_CHECK(ReadNormalAttribute(normal, 0, &decoded));
  TEST_CHECK(decoded[2] == 1.0f);
  TEST_CHECK(!ReadNormalAttribute(normal, 1, &decoded));
  TEST_CHECK(!ReadNormalAttribute(normal, size_t(-1), &decoded));
  normal.stride = 1;
  TEST_CHECK(!ReadNormalAttribute(normal, 0, &decoded));
  normal.elementSize = 0;
  TEST_CHECK(!ReadNormalAttribute(normal, 0, &decoded));
}
