// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Unit tests for MaterialX support in LightUSD

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#include <direct.h>
#define LIGHTUSD_TEST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define LIGHTUSD_TEST_MKDIR(p) mkdir((p), 0755)
#endif

#include "unit-materialx.h"
#include "prim-reconstruct.hh"
#include "usda-reader.hh"
#include "usdShade.hh"
#include "layer.hh"
#include "composition.hh"
#include "usdMtlx.hh"
#include "asset-resolution.hh"
#include "mtlx-xml-parser.hh"
#include "value-types.hh"
#include "lightusd.hh"
#include "math-util.inc"

using namespace lightusd;

void materialx_139_version_test(void) {
  mtlx::MaterialXParser parser;
  TEST_CHECK(parser.Parse(
      R"(<materialx version="1.39"><open_pbr_surface name="surface" type="surfaceshader"/></materialx>)"));
  TEST_CHECK(parser.GetVersion() == "1.39");
  TEST_CHECK(parser.GetWarning().empty());
}

namespace {

// Helpers for the <include> path-traversal regression test.
bool MtlxTestWriteFile(const std::string &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    return false;
  }
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  return ofs.good();
}

bool MtlxTestContains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Test MaterialXConfigAPI structure extension
void materialx_config_api_struct_test(void) {
  MaterialXConfigAPI config;

  // Check default values by calling get_value on unset attributes
  // When not authored, get_value() returns the fallback value
  TEST_CHECK(!config.mtlx_version.authored());
  TEST_CHECK(config.mtlx_version.get_value() == "1.38");

  TEST_CHECK(!config.mtlx_namespace.authored());
  TEST_CHECK(config.mtlx_namespace.get_value() == "");

  TEST_CHECK(!config.mtlx_colorspace.authored());
  TEST_CHECK(config.mtlx_colorspace.get_value() == "lin_rec709");

  TEST_CHECK(!config.mtlx_sourceUri.authored());
  TEST_CHECK(config.mtlx_sourceUri.get_value() == "");
}

// Test MaterialXConfigAPI parsing from USD
void materialx_config_api_parsing_test(void) {
  std::string usda = R"(#usda 1.0

def Material "TestMaterial" (
    prepend apiSchemas = ["MaterialXConfigAPI"]
)
{
    uniform string config:mtlx:version = "1.39"
    uniform string config:mtlx:namespace = "test_namespace"
    uniform string config:mtlx:colorspace = "acescg"
    uniform string config:mtlx:sourceUri = "test.mtlx"

    token outputs:surface.connect = </TestMaterial/TestShader.outputs:surface>
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);

  if (ret) {
    // Find the Material prim
    const Prim *material_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/TestMaterial", ""), material_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && material_prim) {
      // Regression: MaterialXConfigAPI must be recognized as a known (built-in)
      // API schema, not preserved as an "unknown API schema". Recognition is
      // separate from the typed reconstruct below.
      TEST_CHECK(material_prim->metas().has_apiSchemas());
      if (material_prim->metas().has_apiSchemas()) {
        const auto &schemas = material_prim->metas().get_apiSchemas();
        bool known = false;
        for (const auto &n : schemas.names) {
          if (n.first == APISchemas::APIName::MaterialXConfigAPI) {
            known = true;
          }
        }
        TEST_CHECK(known);
        TEST_CHECK(schemas.unknownSchemas.empty());
      }

      const Material *mat = material_prim->data().as<Material>();
      TEST_CHECK(mat != nullptr);

      if (mat) {
        // Check MaterialXConfigAPI was parsed
        TEST_CHECK(mat->materialXConfig.has_value() == true);

        if (mat->materialXConfig.has_value()) {
          // Check values - need to access the actual value, not fallback
          const auto& config = mat->materialXConfig.value();

          // Check if values were authored (parsed from USD)
          TEST_CHECK(config.mtlx_version.authored() == true);
          TEST_CHECK(config.mtlx_namespace.authored() == true);
          TEST_CHECK(config.mtlx_colorspace.authored() == true);
          TEST_CHECK(config.mtlx_sourceUri.authored() == true);

          // Check actual values
          if (config.mtlx_version.authored()) {
            TEST_CHECK(config.mtlx_version.get_value() == "1.39");
          }
          if (config.mtlx_namespace.authored()) {
            TEST_CHECK(config.mtlx_namespace.get_value() == "test_namespace");
          }
          if (config.mtlx_colorspace.authored()) {
            TEST_CHECK(config.mtlx_colorspace.get_value() == "acescg");
          }
          if (config.mtlx_sourceUri.authored()) {
            TEST_CHECK(config.mtlx_sourceUri.get_value() == "test.mtlx");
          }
        }
      }
    }
  }
}

// Test OpenPBRSurface shader reconstruction
void openpbr_surface_reconstruction_test(void) {
  std::string usda = R"(#usda 1.0

def Shader "OpenPBRShader"
{
    uniform token info:id = "OpenPBRSurface"

    # Base layer
    float inputs:base_weight = 0.9
    color3f inputs:base_color = (0.5, 0.6, 0.7)
    float inputs:base_roughness = 0.3
    float inputs:base_metalness = 0.2

    # Specular layer
    float inputs:specular_weight = 0.8
    color3f inputs:specular_color = (0.9, 0.9, 1.0)
    float inputs:specular_roughness = 0.15
    float inputs:specular_ior = 1.45

    token outputs:surface
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);

  if (ret) {
    const Prim *shader_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/OpenPBRShader", ""), shader_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && shader_prim) {
      const Shader *shader = shader_prim->data().as<Shader>();
      TEST_CHECK(shader != nullptr);

      if (shader) {
        TEST_CHECK(shader->info_id == kOpenPBRSurface);

        const OpenPBRSurface *openpbr = shader->value.as<OpenPBRSurface>();
        TEST_CHECK(openpbr != nullptr);

        if (openpbr) {
          // Test base layer values
          // The get_value() returns an Animatable<T>, and we need to extract the scalar value
          if (openpbr->base_weight.authored()) {
            const auto& base_weight_anim = openpbr->base_weight.get_value();
            float val;
            if (base_weight_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.9f));
            }
          }
          if (openpbr->base_color.authored()) {
            const auto& base_color_anim = openpbr->base_color.get_value();
            value::color3f color;
            if (base_color_anim.get_scalar(&color)) {
              TEST_CHECK(math::is_close(color[0], 0.5f));
              TEST_CHECK(math::is_close(color[1], 0.6f));
              TEST_CHECK(math::is_close(color[2], 0.7f));
            }
          }
          if (openpbr->base_roughness.authored()) {
            const auto& base_roughness_anim = openpbr->base_roughness.get_value();
            float val;
            if (base_roughness_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.3f));
            }
          }
          if (openpbr->base_metalness.authored()) {
            const auto& base_metalness_anim = openpbr->base_metalness.get_value();
            float val;
            if (base_metalness_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.2f));
            }
          }

          // Test specular layer values
          if (openpbr->specular_weight.authored()) {
            const auto& specular_weight_anim = openpbr->specular_weight.get_value();
            float val;
            if (specular_weight_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.8f));
            }
          }
          if (openpbr->specular_ior.authored()) {
            const auto& specular_ior_anim = openpbr->specular_ior.get_value();
            float val;
            if (specular_ior_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 1.45f));
            }
          }
        }
      }
    }
  }
}

// Test MtlxAutodeskStandardSurface shader reconstruction
void mtlx_standard_surface_reconstruction_test(void) {
  std::string usda = R"(#usda 1.0

def Shader "StandardSurfaceShader"
{
    uniform token info:id = "MtlxAutodeskStandardSurface"

    # Base properties
    float inputs:base = 0.95
    color3f inputs:base_color = (0.18, 0.18, 0.18)
    float inputs:metalness = 0.9

    # Specular properties
    float inputs:specular = 0.85
    float inputs:specular_roughness = 0.05
    float inputs:specular_IOR = 1.52

    # Coat
    float inputs:coat = 0.2
    float inputs:coat_roughness = 0.02

    token outputs:out
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);

  if (ret) {
    const Prim *shader_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/StandardSurfaceShader", ""), shader_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && shader_prim) {
      const Shader *shader = shader_prim->data().as<Shader>();
      TEST_CHECK(shader != nullptr);

      if (shader) {
        TEST_CHECK(shader->info_id == kMtlxAutodeskStandardSurface);

        const MtlxAutodeskStandardSurface *standardSurf = shader->value.as<MtlxAutodeskStandardSurface>();
        TEST_CHECK(standardSurf != nullptr);

        if (standardSurf) {
          // Test base properties
          if (standardSurf->base.authored()) {
            const auto& base_anim = standardSurf->base.get_value();
            float val;
            if (base_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.95f));
            }
          }
          if (standardSurf->metalness.authored()) {
            const auto& metalness_anim = standardSurf->metalness.get_value();
            float val;
            if (metalness_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.9f));
            }
          }

          // Test specular properties
          if (standardSurf->specular.authored()) {
            const auto& specular_anim = standardSurf->specular.get_value();
            float val;
            if (specular_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.85f));
            }
          }
          if (standardSurf->specular_roughness.authored()) {
            const auto& specular_roughness_anim = standardSurf->specular_roughness.get_value();
            float val;
            if (specular_roughness_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.05f));
            }
          }
          if (standardSurf->specular_IOR.authored()) {
            const auto& specular_IOR_anim = standardSurf->specular_IOR.get_value();
            float val;
            if (specular_IOR_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 1.52f));
            }
          }

          // Test coat properties
          if (standardSurf->coat.authored()) {
            const auto& coat_anim = standardSurf->coat.get_value();
            float val;
            if (coat_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.2f));
            }
          }
          if (standardSurf->coat_roughness.authored()) {
            const auto& coat_roughness_anim = standardSurf->coat_roughness.get_value();
            float val;
            if (coat_roughness_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.02f));
            }
          }
        }
      }
    }
  }
}

// Test NodeGraph support
void nodegraph_support_test(void) {
  // NodeGraph reconstruction is not yet implemented - this will be added in a future step
  // For now, just test that the NodeGraph struct is defined and has correct defaults
  NodeGraph ng;
  TEST_CHECK(!ng.nodedef.authored());
  TEST_CHECK(!ng.nodegraph_type.authored());

  // TypedAttribute doesn't have authored values by default
  TEST_CHECK(!ng.nodedef.has_value());
  TEST_CHECK(!ng.nodegraph_type.has_value());

  // NodeGraph reconstruction is covered by materialx_nodegraph_inputs_outputs_test.
  /*
  std::string usda = R"(#usda 1.0

def NodeGraph "TestNodeGraph"
{
    uniform string nodedef = "test_nodedef"
    uniform string nodegraph_type = "material"

    # NodeGraph outputs are stored in props
    token outputs:result.connect = </TestNodeGraph/InternalShader.outputs:out>
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);

  if (ret) {
    const Prim *nodegraph_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/TestNodeGraph", ""), nodegraph_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && nodegraph_prim) {
      const NodeGraph *nodegraph = nodegraph_prim->data().as<NodeGraph>();
      TEST_CHECK(nodegraph != nullptr);

      if (nodegraph) {
        // Check MaterialX-specific attributes
        if (nodegraph->nodedef.authored()) {
          TEST_CHECK(nodegraph->nodedef.get_value() == "test_nodedef");
        }
        if (nodegraph->nodegraph_type.authored()) {
          TEST_CHECK(nodegraph->nodegraph_type.get_value() == "material");
        }

        // Check that outputs are stored in props
        auto it = nodegraph->props.find("outputs:result");
        TEST_CHECK(it != nodegraph->props.end());
      }
    }
  }
  */
}

// Regression: a NodeGraph PrimSpec must survive Layer->Stage reconstruction
// (composition flatten). It was previously dropped as "TODO or unsupported prim
// type: NodeGraph", which lost the whole MaterialX network on a USDZ roundtrip.
void nodegraph_reconstruct_from_layer_test(void) {
  Layer layer;
  PrimSpec ng(Specifier::Def, "NodeGraph", "MyNodeGraph");

  // A MaterialX image shader node inside the node graph.
  PrimSpec shader(Specifier::Def, "Shader", "ImageNode");
  {
    Attribute attr;
    attr.set_value(value::token("ND_image_color3"));
    attr.set_type_name("token");
    attr.variability() = Variability::Uniform;
    shader.props()["info:id"] = Property(attr, false);
  }
  ng.children().push_back(shader);
  layer.add_primspec("MyNodeGraph", ng);

  Stage stage;
  std::string warn, err;
  bool ok = LayerToStage(std::move(layer), &stage, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("LayerToStage failed: %s", err.c_str());
    return;
  }

  // The NodeGraph prim (and its Shader child) must survive, not be dropped.
  Path path("/MyNodeGraph", "");
  auto result = stage.GetPrimAtPath(path);
  TEST_CHECK(result.has_value());
  if (result) {
    TEST_CHECK(result.value()->data().as<NodeGraph>() != nullptr);
    TEST_CHECK(result.value()->children().size() == 1);
    if (result.value()->children().size() == 1) {
      TEST_CHECK(result.value()->children()[0].data().as<Shader>() != nullptr);
    }
  }
}

// Nodegraph interface properties must survive MaterialX import as typed
// values/connections. Previously <input> elements were silently ignored and
// <output> connections were stored as strings.
void materialx_nodegraph_inputs_outputs_test(void) {
  const std::string xml = R"XML(
<materialx version="1.39">
  <nodegraph name="Graph&amp;Main">
    <input name="gain" type="float" value="0.75" />
    <input name="transform" type="matrix33" value="1, 0, 0, 0, 1, 0, 0, 0, 1" />
    <input name="weights" type="floatarray" value="0.1, 0.2, 0.3" />
    <input name="labels" type="stringarray" value="first, &quot;second, item&quot;" />
    <image name="ImageNode" type="color3">
      <input name="file" type="filename" value="albedo&amp;rough.jpg" />
    </image>
    <output name="outColor" type="color3" nodename="ImageNode" output="rgb" />
  </nodegraph>
  <open_pbr_surface name="Surface" type="surfaceshader">
    <input name="base_color" type="color3" nodegraph="Graph&amp;Main" output="outColor" />
  </open_pbr_surface>
</materialx>
)XML";

  MtlxModel model;
  std::string warn;
  std::string err;
  TEST_CHECK(ReadMaterialXFromString(xml, "nodegraph-inputs.mtlx", &model,
                                     &warn, &err));
  auto ng_it = model.nodegraphs.find("Graph&Main");
  TEST_CHECK(ng_it != model.nodegraphs.end());
  if (ng_it == model.nodegraphs.end()) return;

  const PrimSpec &ng = ng_it->second;
  auto input_it = ng.props().find("inputs:gain");
  TEST_CHECK(input_it != ng.props().end());
  if (input_it != ng.props().end() && input_it->second.is_attribute()) {
    const auto value = input_it->second.get_attribute().get_value<float>();
    TEST_CHECK(value.has_value());
    if (value) TEST_CHECK(math::is_close(value.value(), 0.75f));
  }

  auto output_it = ng.props().find("outputs:outColor");
  TEST_CHECK(output_it != ng.props().end());
  if (output_it != ng.props().end() && output_it->second.is_attribute()) {
    const Attribute &output = output_it->second.get_attribute();
    TEST_CHECK(output.has_connections());
    if (output.has_connections()) {
      TEST_CHECK(output.connections()[0].full_path_name() ==
                 "ImageNode.outputs:rgb");
    }
  }
  auto transform_it = ng.props().find("inputs:transform");
  TEST_CHECK(transform_it != ng.props().end());
  if (transform_it != ng.props().end() && transform_it->second.is_attribute()) {
    TEST_CHECK(transform_it->second.get_attribute().type_id() == value::TYPE_ID_MATRIX3F);
  }
  auto weights_it = ng.props().find("inputs:weights");
  TEST_CHECK(weights_it != ng.props().end());
  if (weights_it != ng.props().end() && weights_it->second.is_attribute()) {
    const auto weights = weights_it->second.get_attribute().get_value<TypedArray<float>>();
    TEST_CHECK(weights.has_value());
    if (weights) TEST_CHECK(weights->size() == 3);
  }
  auto labels_it = ng.props().find("inputs:labels");
  TEST_CHECK(labels_it != ng.props().end());
  if (labels_it != ng.props().end() && labels_it->second.is_attribute()) {
    const auto labels = labels_it->second.get_attribute().get_value<std::vector<std::string>>();
    TEST_CHECK(labels.has_value());
    if (labels) {
      TEST_CHECK(labels->size() == 2);
      if (labels->size() == 2) TEST_CHECK((*labels)[1] == "second, item");
    }
  }

  std::string output_xml;
  TEST_CHECK(WriteMaterialXToString(model, output_xml, &warn, &err));
  TEST_CHECK(output_xml.find("nodegraph=\"Graph&amp;Main\" output=\"outColor\"") !=
             std::string::npos);
  TEST_CHECK(output_xml.find("name=\"transform\" type=\"matrix33\" value=\"1, 0, 0, 0, 1, 0, 0, 0, 1\"") !=
             std::string::npos);
  TEST_CHECK(output_xml.find("name=\"weights\" type=\"floatarray\" value=\"0.1, 0.2, 0.3\"") !=
             std::string::npos);
  TEST_CHECK(output_xml.find("name=\"labels\" type=\"stringarray\"") !=
             std::string::npos);
  TEST_CHECK(output_xml.find("name=\"labels\" type=\"stringarray\"") !=
             std::string::npos);
  TEST_CHECK(output_xml.find("<output name=\"outColor\" type=\"color3\" nodename=\"ImageNode\" output=\"rgb\" />") !=
             std::string::npos);
}

void materialx_openpbr_extended_inputs_test(void) {
  const std::string xml = R"XML(
<materialx version="1.39">
  <open_pbr_surface name="Extended" type="surfaceshader">
    <input name="base_diffuse_roughness" type="float" value="0.09" />
    <input name="specular_roughness_anisotropy" type="float" value="0.11" />
    <input name="transmission_dispersion_abbe_number" type="float" value="42.0" />
    <input name="transmission_dispersion_scale" type="float" value="0.25" />
    <input name="subsurface_scatter_anisotropy" type="float" value="0.33" />
    <input name="coat_roughness_anisotropy" type="float" value="0.44" />
    <input name="coat_darkening" type="float" value="0.55" />
    <input name="fuzz_weight" type="float" value="0.66" />
    <input name="fuzz_color" type="color3" value="0.1, 0.2, 0.3" />
    <input name="fuzz_roughness" type="float" value="0.77" />
    <input name="thin_film_weight" type="float" value="0.88" />
    <input name="coat_normal" type="vector3" value="0, 0, 1" />
    <input name="coat_tangent" type="vector3" value="1, 0, 0" />
    <input name="geometry_thin_walled" type="boolean" value="true" />
    <input name="vendor_gain" type="float" value="0.125" />
    <input name="vendor_texture" type="filename" nodegraph="VendorGraph"
           output="out" />
    <input name="normal" type="vector3" nodegraph="NormalGraph"
           output="out" />
  </open_pbr_surface>
</materialx>
)XML";

  MtlxModel model;
  std::string warn;
  std::string err;
  TEST_CHECK(ReadMaterialXFromString(xml, "openpbr-extended.mtlx", &model,
                                     &warn, &err));
  TEST_CHECK(warn.find("Unknown/unsupported OpenPBR input") == std::string::npos);
  TEST_CHECK(model.shader.as<MtlxOpenPBRSurface>() != nullptr);
  TEST_CHECK(model.custom_shader_inputs["Extended"].size() == 1);
  TEST_CHECK(model.shader_connections["Extended"].size() == 2);
  if (!model.custom_shader_inputs["Extended"].empty()) {
    TEST_CHECK(model.custom_shader_inputs["Extended"][0].name == "vendor_gain");
    TEST_CHECK(model.custom_shader_inputs["Extended"][0].value == "0.125");
  }
  std::string exported;
  TEST_CHECK(WriteMaterialXToString(model, exported, &warn, &err));
  TEST_CHECK(exported.find("name=\"vendor_gain\" type=\"float\" value=\"0.125\"") !=
             std::string::npos);
  TEST_CHECK(exported.find("name=\"vendor_texture\" type=\"filename\" nodegraph=\"VendorGraph\" output=\"out\"") !=
             std::string::npos);
  const std::string normal_input =
      "name=\"normal\" type=\"vector3\" nodegraph=\"NormalGraph\"";
  const size_t normal_first = exported.find(normal_input);
  TEST_CHECK(normal_first != std::string::npos);
  TEST_CHECK(exported.find(normal_input, normal_first + 1) == std::string::npos);
  MtlxModel reparsed;
  TEST_CHECK(ReadMaterialXFromString(exported, "openpbr-extended-roundtrip.mtlx",
                                     &reparsed, &warn, &err));
  TEST_CHECK(reparsed.custom_shader_inputs["Extended"].size() == 1);
  TEST_CHECK(reparsed.shader_connections["Extended"].size() == 2);
  if (const auto *surface = model.shader.as<MtlxOpenPBRSurface>()) {
    float fuzz_weight = 0.0f;
    float base_diffuse_roughness = 0.0f;
    float thin_film_weight = 0.0f;
    bool thin_walled = false;
    value::normal3f coat_normal;
    value::vector3f coat_tangent;
    TEST_CHECK(surface->fuzz_weight.get_value().get_scalar(&fuzz_weight));
    TEST_CHECK(surface->base_diffuse_roughness.get_value().get_scalar(
        &base_diffuse_roughness));
    TEST_CHECK(surface->thin_film_weight.get_value().get_scalar(&thin_film_weight));
    TEST_CHECK(surface->geometry_thin_walled.get_value().get_scalar(&thin_walled));
    if (surface->geometry_coat_normal.authored()) {
      const auto anim = surface->geometry_coat_normal.get_value();
      TEST_CHECK(anim && anim->get_scalar(&coat_normal));
    }
    if (surface->geometry_coat_tangent.authored()) {
      const auto anim = surface->geometry_coat_tangent.get_value();
      TEST_CHECK(anim && anim->get_scalar(&coat_tangent));
    }
    TEST_CHECK(math::is_close(fuzz_weight, 0.66f));
    TEST_CHECK(math::is_close(base_diffuse_roughness, 0.09f));
    TEST_CHECK(math::is_close(thin_film_weight, 0.88f));
    TEST_CHECK(thin_walled);
    TEST_CHECK(math::is_close(coat_normal[2], 1.0f));
    TEST_CHECK(math::is_close(coat_tangent[0], 1.0f));
  }
}

void materialx_look_roundtrip_test(void) {
  const std::string xml = R"XML(
<materialx version="1.39" colorspace="lin_rec709" cms="ocio" cmsconfig="config&amp;v2.ocio" namespace="Studio&amp;Main">
  <open_pbr_surface name="LookShader" type="surfaceshader">
    <input name="base_weight" type="float" value="0.8" />
  </open_pbr_surface>
  <look name="Hero&amp;Look">
    <materialassign name="Assign" material="HeroMaterial" geom="/World/Body" />
    <propertyassign property="inputs:base_weight" value="0.9" custom="a&amp;b" />
    <assigngroup name="Group&amp;A"><propertyassign property="inputs:specular" value="0.4" /></assigngroup>
  </look>
</materialx>
)XML";

  MtlxModel model;
  std::string warn;
  std::string err;
  TEST_CHECK(ReadMaterialXFromString(xml, "look.mtlx", &model, &warn, &err));
  TEST_CHECK(model.looks.find("Hero&Look") != model.looks.end());
  TEST_CHECK(model.version == "1.39");
  TEST_CHECK(model.cms == "ocio");
  TEST_CHECK(model.cmsconfig == "config&v2.ocio");
  TEST_CHECK(model.name_space == "Studio&Main");
  if (model.looks.find("Hero&Look") != model.looks.end()) {
    const auto &elements = model.looks.at("Hero&Look").elements;
    TEST_CHECK(elements.size() == 3);
    if (elements.size() == 3) TEST_CHECK(elements[2].children.size() == 1);
  }

  std::string output;
  TEST_CHECK(WriteMaterialXToString(model, output, &warn, &err));
  TEST_CHECK(output.find("<look name=\"Hero&amp;Look\">") != std::string::npos);
  TEST_CHECK(output.find("custom=\"a&amp;b\"") != std::string::npos);
  TEST_CHECK(output.find("version=\"1.39\"") != std::string::npos);
  TEST_CHECK(output.find("cms=\"ocio\"") != std::string::npos);
  TEST_CHECK(output.find("cmsconfig=\"config&amp;v2.ocio\"") != std::string::npos);
  TEST_CHECK(output.find("namespace=\"Studio&amp;Main\"") != std::string::npos);
  TEST_CHECK(output.find("<assigngroup name=\"Group&amp;A\">") != std::string::npos);
  TEST_CHECK(output.find("<propertyassign property=\"inputs:specular\" value=\"0.4\" />") != std::string::npos);
}

void materialx_light_shader_roundtrip_test(void) {
  const std::string xml = R"XML(
<materialx version="1.39">
  <open_pbr_surface name="Surface" type="surfaceshader" />
  <uniform_edf name="UniformEDF" type="EDF">
    <input name="color" type="color3" value="0.2, 0.4, 0.6" />
  </uniform_edf>
  <conical_edf name="ConeEDF" type="EDF">
    <input name="inner_angle" type="float" value="20.0" />
    <input name="outer_angle" type="float" value="40.0" />
  </conical_edf>
  <measured_edf name="MeasuredEDF" type="EDF">
    <input name="file" type="filename" value="profile.ies" />
  </measured_edf>
  <light name="Lamp" type="lightshader">
    <input name="edf" type="EDF" nodename="UniformEDF" />
    <input name="intensity" type="color3" value="1.0, 0.8, 0.5" />
    <input name="exposure" type="float" value="2.0" />
  </light>
</materialx>
)XML";

  MtlxModel model;
  std::string warn;
  std::string err;
  TEST_CHECK(ReadMaterialXFromString(xml, "lights.mtlx", &model, &warn, &err));
  TEST_CHECK(model.light_shaders.size() == 4);
  TEST_CHECK(model.light_shaders.find("UniformEDF") != model.light_shaders.end());
  TEST_CHECK(model.light_shaders.find("Lamp") != model.light_shaders.end());

  std::string output;
  TEST_CHECK(WriteMaterialXToString(model, output, &warn, &err));
  TEST_CHECK(output.find("<uniform_edf name=\"UniformEDF\" type=\"EDF\">") != std::string::npos);
  TEST_CHECK(output.find("<conical_edf name=\"ConeEDF\" type=\"EDF\">") != std::string::npos);
  TEST_CHECK(output.find("<measured_edf name=\"MeasuredEDF\" type=\"EDF\">") != std::string::npos);
  TEST_CHECK(output.find("<light name=\"Lamp\" type=\"lightshader\">") != std::string::npos);
  TEST_CHECK(output.find("nodename=\"UniformEDF\"") != std::string::npos);

  MtlxModel reparsed;
  TEST_CHECK(ReadMaterialXFromString(output, "lights-roundtrip.mtlx", &reparsed,
                                     &warn, &err));
  TEST_CHECK(reparsed.light_shaders.size() == 4);
  TEST_CHECK(reparsed.light_shaders.find("Lamp") != reparsed.light_shaders.end());
}

// Test MaterialX shader type constants
void materialx_shader_constants_test(void) {
  // Check that the constants are defined and have expected values
  TEST_CHECK(std::string(kOpenPBRSurface) == "OpenPBRSurface");
  TEST_CHECK(std::string(kMtlxAutodeskStandardSurface) == "MtlxAutodeskStandardSurface");
  TEST_CHECK(std::string(kMtlxUsdPreviewSurface) == "MtlxUsdPreviewSurface");
  TEST_CHECK(std::string(kNodeGraph) == "NodeGraph");
}

// Test fallback values for MaterialX shaders
void materialx_shader_fallback_values_test(void) {
  // Test OpenPBRSurface default values
  // When not authored, get_value() returns the fallback value (an Animatable with the fallback value set)
  {
    OpenPBRSurface surface;
    TEST_CHECK(!surface.base_weight.authored());
    const auto& base_weight_anim = surface.base_weight.get_value();
    float val;
    if (base_weight_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.0f));
    }

    TEST_CHECK(!surface.base_roughness.authored());
    const auto& base_roughness_anim = surface.base_roughness.get_value();
    if (base_roughness_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 0.0f));
    }

    TEST_CHECK(!surface.base_metalness.authored());
    const auto& base_metalness_anim = surface.base_metalness.get_value();
    if (base_metalness_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 0.0f));
    }

    TEST_CHECK(!surface.specular_weight.authored());
    const auto& specular_weight_anim = surface.specular_weight.get_value();
    if (specular_weight_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.0f));
    }

    TEST_CHECK(!surface.specular_ior.authored());
    const auto& specular_ior_anim = surface.specular_ior.get_value();
    if (specular_ior_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.5f));
    }
  }

  // Test MtlxAutodeskStandardSurface default values
  {
    MtlxAutodeskStandardSurface surface;
    TEST_CHECK(!surface.base.authored());
    const auto& base_anim = surface.base.get_value();
    float val;
    if (base_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.0f));
    }

    TEST_CHECK(!surface.metalness.authored());
    const auto& metalness_anim = surface.metalness.get_value();
    if (metalness_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 0.0f));
    }

    TEST_CHECK(!surface.specular.authored());
    const auto& specular_anim = surface.specular.get_value();
    if (specular_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.0f));
    }

    TEST_CHECK(!surface.specular_roughness.authored());
    const auto& specular_roughness_anim = surface.specular_roughness.get_value();
    if (specular_roughness_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 0.2f));
    }

    TEST_CHECK(!surface.specular_IOR.authored());
    const auto& specular_IOR_anim = surface.specular_IOR.get_value();
    if (specular_IOR_anim.get_scalar(&val)) {
      TEST_CHECK(math::is_close(val, 1.5f));
    }
  }
}

// Security regression test: MaterialX <include filename="..."/> must not be
// usable for path traversal / arbitrary file read.
//
// ProcessIncludes() in src/usdMtlx.cc resolves the attacker-controlled
// `filename` attribute against the document's base directory. Previously the
// value was used verbatim, so an absolute path ("/etc/passwd") or a "../"
// traversal escaped the base directory. The fix routes the include filename
// through security_policy::ValidateAndNormalizeAssetPath(), which rejects
// absolute paths, Windows drive letters, and any ".." segment.
//
// This test confirms (1) "../" traversal is rejected, (2) an absolute path is
// rejected, and (3) a contained relative include still loads.
void materialx_include_path_traversal_test(void) {
  // Message fragment emitted by the path-validation guard in ProcessIncludes().
  const std::string kRejectMsg = "safe relative path";
  const std::string kTmpRoot = "unit_mtlx_traversal_tmp";
  const std::string kBaseDir = "unit_mtlx_traversal_tmp/base";

  // Fixture setup (cwd is the build dir per CTest WORKING_DIRECTORY).
  LIGHTUSD_TEST_MKDIR(kTmpRoot.c_str());
  LIGHTUSD_TEST_MKDIR(kBaseDir.c_str());  // EEXIST from a prior run is harmless.

  // A "secret" file living OUTSIDE the base dir (in the parent) — the target an
  // attacker would try to reach via "../".
  TEST_CHECK(MtlxTestWriteFile(kTmpRoot + "/secret_outside.txt",
                               "TOP-SECRET-MARKER\n"));

  const char *kIncludeFragment = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <surfacematerial name="TestMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="TestMaterial_shader" />
  </surfacematerial>
  <open_pbr_surface name="TestMaterial_shader" type="surfaceshader">
    <input name="base_color" type="color3" value="0.8, 0.2, 0.2" />
    <input name="base_weight" type="float" value="1.0" />
  </open_pbr_surface>
</materialx>
)";

  TEST_CHECK(MtlxTestWriteFile(kBaseDir + "/main_traversal.mtlx",
                               R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="../secret_outside.txt"/>
</materialx>
)"));
  TEST_CHECK(MtlxTestWriteFile(kBaseDir + "/main_absolute.mtlx",
                               R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="/etc/passwd"/>
</materialx>
)"));
  TEST_CHECK(MtlxTestWriteFile(kBaseDir + "/inc_ok.mtlx", kIncludeFragment));
  TEST_CHECK(MtlxTestWriteFile(kBaseDir + "/main_ok.mtlx",
                               R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="inc_ok.mtlx"/>
</materialx>
)"));

  AssetResolutionResolver resolver;
  resolver.set_search_paths({kBaseDir});

  // (1) "../" traversal must be rejected by the path validator.
  {
    MtlxModel mtlx;
    std::string warn, err;
    bool ret = ReadMaterialXFromFile(resolver, "main_traversal.mtlx", &mtlx,
                                     &warn, &err);
    TEST_CHECK(ret == false);
    TEST_CHECK(MtlxTestContains(err, kRejectMsg));
    TEST_MSG("err: %s", err.c_str());
  }

  // (2) Absolute-path include must be rejected by the path validator.
  {
    MtlxModel mtlx;
    std::string warn, err;
    bool ret = ReadMaterialXFromFile(resolver, "main_absolute.mtlx", &mtlx,
                                     &warn, &err);
    TEST_CHECK(ret == false);
    TEST_CHECK(MtlxTestContains(err, kRejectMsg));
    TEST_MSG("err: %s", err.c_str());
  }

  // (3) A contained relative include must NOT be rejected and should load.
  {
    MtlxModel mtlx;
    std::string warn, err;
    bool ret =
        ReadMaterialXFromFile(resolver, "main_ok.mtlx", &mtlx, &warn, &err);
    TEST_CHECK(!MtlxTestContains(err, kRejectMsg));
    TEST_CHECK(ret == true);
    TEST_MSG("err: %s", err.c_str());
  }

  // Best-effort cleanup of fixture files.
  std::remove((kBaseDir + "/main_traversal.mtlx").c_str());
  std::remove((kBaseDir + "/main_absolute.mtlx").c_str());
  std::remove((kBaseDir + "/main_ok.mtlx").c_str());
  std::remove((kBaseDir + "/inc_ok.mtlx").c_str());
  std::remove((kTmpRoot + "/secret_outside.txt").c_str());
}

// Regression: UsdPreviewSurface `inputs:displacement` must be reconstructed into
// the typed `displacement` field. The reconstruction code previously matched a
// misspelled "inputs:dispacement" token, so a correctly-authored displacement
// silently fell through to the generic props bag and never populated
// `surface->displacement`.
void usdpreviewsurface_displacement_test(void) {
  std::string usda = R"(#usda 1.0

def Shader "PreviewShader"
{
    uniform token info:id = "UsdPreviewSurface"

    float inputs:displacement = 0.42
    token outputs:surface
    token outputs:displacement
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);
  TEST_MSG("warn: %s err: %s", warn.c_str(), err.c_str());

  if (ret) {
    const Prim *shader_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/PreviewShader", ""), shader_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && shader_prim) {
      const Shader *shader = shader_prim->data().as<Shader>();
      TEST_CHECK(shader != nullptr);

      if (shader) {
        TEST_CHECK(shader->info_id == kUsdPreviewSurface);

        const UsdPreviewSurface *surface = shader->value.as<UsdPreviewSurface>();
        TEST_CHECK(surface != nullptr);

        if (surface) {
          // The whole point of the regression: displacement must be authored.
          TEST_CHECK(surface->displacement.authored() == true);
          if (surface->displacement.authored()) {
            const auto &disp_anim = surface->displacement.get_value();
            float val = 0.0f;
            if (disp_anim.get_scalar(&val)) {
              TEST_CHECK(math::is_close(val, 0.42f));
            }
          }
          // And it must NOT have leaked into the generic props bag.
          TEST_CHECK(surface->props.count("inputs:displacement") == 0);
        }
      }
    }
  }
}

// Regression: UsdUVTexture `inputs:uv_set` / `inputs:uv_set_name` (lightusd
// extensions) must reconstruct into the typed fields, not be left in the
// generic props bag.
void usduvtexture_uvset_test(void) {
  std::string usda = R"(#usda 1.0

def Shader "Tex"
{
    uniform token info:id = "UsdUVTexture"

    asset inputs:file = @./tex.png@
    int inputs:uv_set = 2
    token inputs:uv_set_name = "st1"
    float3 outputs:rgb
}
)";

  Stage stage;
  std::string warn, err;

  bool ret = LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, &warn, &err);
  TEST_CHECK(ret == true);
  TEST_MSG("warn: %s err: %s", warn.c_str(), err.c_str());

  if (ret) {
    const Prim *shader_prim = nullptr;
    ret = stage.find_prim_at_path(Path("/Tex", ""), shader_prim, &err);
    TEST_CHECK(ret == true);

    if (ret && shader_prim) {
      const Shader *shader = shader_prim->data().as<Shader>();
      TEST_CHECK(shader != nullptr);
      if (shader) {
        const UsdUVTexture *tex = shader->value.as<UsdUVTexture>();
        TEST_CHECK(tex != nullptr);
        if (tex) {
          TEST_CHECK(tex->uv_set.authored() == true);
          TEST_CHECK(tex->uv_set.get_value() == 2);
          TEST_CHECK(tex->uv_set_name.authored() == true);
          value::token name_tok;
          if (tex->uv_set_name.get_value(&name_tok)) {
            TEST_CHECK(name_tok.str() == "st1");
          }
          // Must be consumed into typed fields, not leaked into props.
          TEST_CHECK(tex->props.count("inputs:uv_set") == 0);
          TEST_CHECK(tex->props.count("inputs:uv_set_name") == 0);
        }
      }
    }
  }
}

// Regression: OpenPBRSurface accepts both the OpenPBR-canonical
// `inputs:geometry_opacity` and the legacy `inputs:opacity`. When both are
// authored, geometry_opacity must win regardless of property iteration order.
void openpbr_opacity_precedence_test(void) {
  auto load_opacity = [](const std::string &usda, float *out, std::string *warn) -> bool {
    Stage stage;
    std::string err;
    if (!LoadUSDAFromMemory((const uint8_t*)usda.c_str(), usda.length(), "", &stage, warn, &err)) {
      return false;
    }
    const Prim *p = nullptr;
    if (!stage.find_prim_at_path(Path("/S", ""), p, &err) || !p) return false;
    const Shader *shader = p->data().as<Shader>();
    if (!shader) return false;
    const OpenPBRSurface *s = shader->value.as<OpenPBRSurface>();
    if (!s || !s->opacity.authored()) return false;
    return s->opacity.get_value().get_scalar(out);
  };

  // Both authored -> geometry_opacity (0.7) wins; a warning is emitted.
  {
    std::string usda = R"(#usda 1.0
def Shader "S"
{
    uniform token info:id = "OpenPBRSurface"
    float inputs:opacity = 0.3
    float inputs:geometry_opacity = 0.7
    token outputs:surface
}
)";
    float val = -1.0f;
    std::string warn;
    TEST_CHECK(load_opacity(usda, &val, &warn) == true);
    TEST_CHECK(math::is_close(val, 0.7f));
    TEST_CHECK(!warn.empty());  // both-authored warning
  }

  // Only legacy opacity authored -> used as-is.
  {
    std::string usda = R"(#usda 1.0
def Shader "S"
{
    uniform token info:id = "OpenPBRSurface"
    float inputs:opacity = 0.3
    token outputs:surface
}
)";
    float val = -1.0f;
    std::string warn;
    TEST_CHECK(load_opacity(usda, &val, &warn) == true);
    TEST_CHECK(math::is_close(val, 0.3f));
  }

  // Only canonical geometry_opacity authored.
  {
    std::string usda = R"(#usda 1.0
def Shader "S"
{
    uniform token info:id = "OpenPBRSurface"
    float inputs:geometry_opacity = 0.7
    token outputs:surface
}
)";
    float val = -1.0f;
    std::string warn;
    TEST_CHECK(load_opacity(usda, &val, &warn) == true);
    TEST_CHECK(math::is_close(val, 0.7f));
  }
}

void openpbr_geometry_connection_alias_test(void) {
  const std::string usda = R"(#usda 1.0
def Shader "S"
{
    uniform token info:id = "OpenPBRSurface"
    normal3f inputs:geometry_normal.connect = </N.outputs:rgb>
    vector3f inputs:geometry_tangent.connect = </T.outputs:rgb>
    normal3f inputs:coat_normal.connect = </CN.outputs:rgb>
    vector3f inputs:coat_tangent.connect = </CT.outputs:rgb>
    token outputs:surface
}
)";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(LoadUSDAFromMemory((const uint8_t *)usda.c_str(), usda.length(),
                                "", &stage, &warn, &err));
  const Prim *p = nullptr;
  TEST_CHECK(stage.find_prim_at_path(Path("/S", ""), p, &err));
  const Shader *shader = p ? p->data().as<Shader>() : nullptr;
  const OpenPBRSurface *surface =
      shader ? shader->value.as<OpenPBRSurface>() : nullptr;
  TEST_CHECK(surface != nullptr);
  if (surface) {
    TEST_CHECK(surface->normal.get_connections().size() == 1);
    TEST_CHECK(surface->tangent.get_connections().size() == 1);
  }
}

// Main test runner
void materialx_tests(void) {
  materialx_config_api_struct_test();
  materialx_config_api_parsing_test();
  openpbr_surface_reconstruction_test();
  mtlx_standard_surface_reconstruction_test();
  usdpreviewsurface_displacement_test();
  nodegraph_support_test();
  materialx_shader_constants_test();
  materialx_shader_fallback_values_test();
  materialx_include_path_traversal_test();
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
