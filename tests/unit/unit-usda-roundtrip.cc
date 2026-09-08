// SPDX-License-Identifier: Apache 2.0
// USDA roundtrip test: parse USDA, export to string, re-parse, compare via JSON

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usda-roundtrip.h"
#include "math-util.inc"
#include "lightusd.hh"
#include "usd-to-json.hh"
#include "json-to-usd.hh"
#include "json-writer.hh"
#include "usd-dump.hh"
#include "minijson.hh"
#include "usdGeom.hh"

#include <iostream>
#include <sstream>
#include <cmath>

using namespace lightusd;

namespace {

struct RoundtripStages {
  Stage original;
  Stage reparsed;
  std::string exported;
};

static int JSONTestResolveAsset(const char *asset_name,
                                const std::vector<std::string> &,
                                std::string *resolved_asset_name,
                                std::string *, void *) {
  if (!asset_name || !resolved_asset_name) return -1;
  *resolved_asset_name = asset_name;
  return 0;
}

static int JSONTestSizeAsset(const char *, uint64_t *nbytes, std::string *,
                             void *) {
  if (!nbytes) return -1;
  *nbytes = 1;
  return 0;
}

static int JSONTestReadAsset(const char *, uint64_t req_nbytes, uint8_t *out,
                             uint64_t *nbytes, std::string *, void *) {
  if (!out || !nbytes || req_nbytes < 1) return -1;
  out[0] = 0x42;
  *nbytes = 1;
  return 0;
}

// Helper function: parse USDA string and return Stage
static bool parseUSDString(const std::string &usd_content, Stage *stage,
                           std::string *warn, std::string *err) {
  return LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd_content.data()),
      usd_content.size(),
      "memory.usda",
      stage,
      warn,
      err);
}

// Helper function: perform roundtrip test
// Returns true if roundtrip succeeds and JSON comparison matches
static bool doRoundtripTest(const std::string &usd_content,
                            RoundtripStages *roundtrip = nullptr,
                            std::string *err_msg = nullptr) {
  // Step 1: Parse original USDA
  Stage stage1;
  std::string warn1, err1;
  bool ret1 = parseUSDString(usd_content, &stage1, &warn1, &err1);
  if (!ret1) {
    if (err_msg) *err_msg = "Failed to parse original USDA: " + err1;
    return false;
  }

  // Step 2: Export to string
  std::string exported = stage1.ExportToString();
  if (exported.empty()) {
    if (err_msg) *err_msg = "ExportToString returned empty string";
    return false;
  }

  // Step 3: Re-parse the exported string
  Stage stage2;
  std::string warn2, err2;
  bool ret2 = parseUSDString(exported, &stage2, &warn2, &err2);
  if (!ret2) {
    if (err_msg) {
      *err_msg = "Failed to re-parse exported USDA: " + err2 + "\n";
      *err_msg += "Exported content:\n" + exported;
    }
    return false;
  }

  // Step 4: Convert both stages to JSON and compare
#if defined(LIGHTUSD_WITH_JSON)
  USDToJSONOptions options;

  auto json1_result = ToJSON(stage1, options);
  if (!json1_result) {
    if (err_msg) *err_msg = "Failed to convert stage1 to JSON: " + json1_result.error();
    return false;
  }

  auto json2_result = ToJSON(stage2, options);
  if (!json2_result) {
    if (err_msg) *err_msg = "Failed to convert stage2 to JSON: " + json2_result.error();
    return false;
  }

  std::string json1 = json1_result.value();
  std::string json2 = json2_result.value();

  if (json1 != json2) {
    if (err_msg) {
      *err_msg = "JSON mismatch after roundtrip.\n";
      *err_msg += "Original JSON:\n" + json1 + "\n";
      *err_msg += "Roundtrip JSON:\n" + json2 + "\n";
    }
    return false;
  }
#else
  // Without JSON support, just verify that re-parsing succeeds
  // and both stages have the same number of root prims
  if (stage1.root_prims().size() != stage2.root_prims().size()) {
    if (err_msg) {
      std::ostringstream oss;
      oss << "Root prim count mismatch: " << stage1.root_prims().size()
          << " vs " << stage2.root_prims().size();
      *err_msg = oss.str();
    }
    return false;
  }
#endif

  if (roundtrip) {
    roundtrip->original = std::move(stage1);
    roundtrip->reparsed = std::move(stage2);
    roundtrip->exported = std::move(exported);
  }

  return true;
}

static bool doRoundtripTest(const std::string &usd_content,
                            std::string *err_msg) {
  return doRoundtripTest(usd_content, nullptr, err_msg);
}

} // anonymous namespace

void json_layer_primspec_roundtrip_test(void) {
#if defined(LIGHTUSD_WITH_JSON)
  Layer source;
  source.set_name("json-layer");
  source.metas().customLayerDataAuthored = true;
  source.metas().customLayerData["quality"] = MetaVariable(int64_t(9));
  Dictionary layer_nested;
  layer_nested["tag"] = MetaVariable(std::string("source"));
  source.metas().customLayerData["nested"] =
      MetaVariable(std::move(layer_nested));
  source.metas().customLayerData["samples"] =
      MetaVariable(std::vector<int>{1, 2, 3});
  source.metas().colorConfiguration = value::AssetPath("ocio/config.ocio");
  source.metas().colorManagementSystem = value::token("ocio");
  source.metas().renderSettingsPrimPath = std::string("/Render/Settings");
  source.metas().owner = std::string("lightusd");
  source.metas().hasOwnedSubLayers = true;
  Dictionary expression_variables;
  expression_variables["ASSET_ROOT"] = MetaVariable(std::string("assets"));
  source.metas().expressionVariables = std::move(expression_variables);
  source.metas().layerRelocates.emplace_back(Path("/Old", ""),
                                             Path("/New", ""));
  source.metas().unregisteredMetas["vendorMeta"] = "(1, 2)";
  PrimSpec root(Specifier::Def, "Root");
  root.typeName() = "Xform";
  Attribute attr;
  attr.set_name("size");
  attr.set_type_name("float");
  attr.set_value(2.5f);
  attr.get_var().set_timesample(1.0, 3.5f);
  Dictionary custom_data;
  custom_data["quality"] = MetaVariable(int64_t(7));
  Dictionary nested;
  nested["tag"] = MetaVariable(std::string("hero"));
  custom_data["nested"] = MetaVariable(std::move(nested));
  attr.metas().set_customData(custom_data);
  Dictionary sdr_metadata;
  sdr_metadata["role"] = MetaVariable(std::string("surface"));
  attr.metas().set_sdrMetadata(sdr_metadata);
  root.props()["size"] = Property(std::move(attr), true);
  PrimSpec child(Specifier::Over, "Child");
  child.typeName() = "Scope";
  root.children().push_back(std::move(child));

  Reference reference;
  reference.asset_path = value::AssetPath("assets/reference.usda");
  reference.prim_path = Path("/Referenced", "");
  reference.layerOffset._offset = 2.0;
  reference.layerOffset._scale = 0.5;
  reference.customData["source"] = MetaVariable(std::string("fixture"));
  root.metas().references = std::vector<std::pair<ListEditQual,
                                                  std::vector<Reference>>>{
      {ListEditQual::Prepend, {reference}}};
  Payload payload;
  payload.asset_path = value::AssetPath("assets/payload.usdc");
  payload.prim_path = Path("/Payload", "");
  root.metas().payload = std::vector<std::pair<ListEditQual,
                                               std::vector<Payload>>>{
      {ListEditQual::Append, {payload}}};
  root.metas().inherits = std::vector<std::pair<ListEditQual,
                                                 std::vector<Path>>>{
      {ListEditQual::ResetToExplicit, {Path("/Base", "")}}};
  root.metas().specializes = std::vector<std::pair<ListEditQual,
                                                   std::vector<Path>>>{
      {ListEditQual::Add, {Path("/Special", "")}}};
  VariantSelectionMap selections;
  selections["modelingVariant"] = "high";
  root.metas().variants = std::move(selections);
  source.primspecs()["/Root"] = std::move(root);

  std::string encoded, warn, err;
  TEST_CHECK(to_json_string(source, &encoded, &warn, &err));
  Layer decoded;
  TEST_CHECK(JSONToLayer(encoded, &decoded, &warn, &err));
  TEST_CHECK(decoded.metas().customLayerDataAuthored);
  TEST_CHECK(decoded.metas().customLayerData.size() == 3);
  if (decoded.metas().customLayerData.size() == 3) {
    auto quality = decoded.metas().customLayerData.at("quality").get_value<int64_t>();
    TEST_CHECK(quality && *quality == 9);
    TEST_CHECK(decoded.metas().customLayerData.at("nested")
                   .get_value<Dictionary>()
                   .has_value());
    auto samples = decoded.metas().customLayerData.at("samples")
                       .get_value<std::vector<int>>();
    TEST_CHECK(samples && *samples == std::vector<int>({1, 2, 3}));
  }
  TEST_CHECK(decoded.metas().colorConfiguration.has_value());
  TEST_CHECK(decoded.metas().colorManagementSystem.has_value());
  TEST_CHECK(decoded.metas().renderSettingsPrimPath.has_value());
  TEST_CHECK(decoded.metas().owner.has_value());
  TEST_CHECK(decoded.metas().hasOwnedSubLayers.has_value());
  TEST_CHECK(decoded.metas().expressionVariables.has_value());
  TEST_CHECK(decoded.metas().layerRelocates.size() == 1);
  TEST_CHECK(decoded.metas().unregisteredMetas.at("vendorMeta") == "(1, 2)");
  if (decoded.metas().colorConfiguration) {
    TEST_CHECK(decoded.metas().colorConfiguration->GetAssetPath() ==
               "ocio/config.ocio");
  }
  if (decoded.metas().renderSettingsPrimPath) {
    TEST_CHECK(*decoded.metas().renderSettingsPrimPath == "/Render/Settings");
  }
  auto it = decoded.primspecs().find("/Root");
  TEST_CHECK(it != decoded.primspecs().end());
  if (it != decoded.primspecs().end()) {
    TEST_CHECK(it->second.typeName() == "Xform");
    TEST_CHECK(it->second.specifier() == Specifier::Def);
    TEST_CHECK(it->second.metas().references.has_value());
    TEST_CHECK(it->second.metas().payload.has_value());
    TEST_CHECK(it->second.metas().inherits.has_value());
    TEST_CHECK(it->second.metas().specializes.has_value());
    TEST_CHECK(it->second.metas().variants.has_value());
    if (it->second.metas().references) {
      TEST_CHECK(it->second.metas().references->size() == 1);
      TEST_CHECK(it->second.metas().references->at(0).first == ListEditQual::Prepend);
      TEST_CHECK(it->second.metas().references->at(0).second.size() == 1);
      if (!it->second.metas().references->at(0).second.empty()) {
        const auto &ref = it->second.metas().references->at(0).second[0];
        TEST_CHECK(ref.asset_path.GetAssetPath() == "assets/reference.usda");
        TEST_CHECK(ref.prim_path.full_path_name() == "/Referenced");
        TEST_CHECK(std::fabs(ref.layerOffset._offset - 2.0) < 1.0e-9);
        TEST_CHECK(std::fabs(ref.layerOffset._scale - 0.5) < 1.0e-9);
        TEST_CHECK(ref.customData.at("source").get_value<std::string>() ==
                   "fixture");
      }
    }
    if (it->second.metas().payload) {
      TEST_CHECK(it->second.metas().payload->at(0).first == ListEditQual::Append);
      TEST_CHECK(it->second.metas().payload->at(0).second[0].asset_path.GetAssetPath() ==
                 "assets/payload.usdc");
    }
    if (it->second.metas().variants) {
      TEST_CHECK(it->second.metas().variants->at("modelingVariant") == "high");
    }
    TEST_CHECK(it->second.props().find("size") != it->second.props().end());
    TEST_CHECK(it->second.children().size() == 1);
    if (it->second.props().find("size") != it->second.props().end()) {
      const Attribute &decoded_attr = it->second.props().at("size").get_attribute();
      auto value = decoded_attr.get_value<float>();
      TEST_CHECK(value && std::fabs(*value - 2.5f) < 1.0e-6f);
      TEST_CHECK(decoded_attr.is_timesamples());
      TEST_CHECK(decoded_attr.metas().has_customData());
      TEST_CHECK(decoded_attr.metas().has_sdrMetadata());
      if (decoded_attr.metas().has_customData()) {
        const auto decoded_data = decoded_attr.metas().get_customData();
        auto quality = decoded_data.at("quality").get_value<int64_t>();
        TEST_CHECK(quality && *quality == 7);
        TEST_CHECK(decoded_data.at("nested").get_value<Dictionary>().has_value());
      }
      const auto &samples = decoded_attr.get_var().ts_raw().get_samples();
      TEST_CHECK(samples.size() == 1);
      if (samples.size() == 1) {
        TEST_CHECK(std::fabs(samples[0].t - 1.0) < 1.0e-9);
        const auto *sample_value = samples[0].value.as<float>();
        TEST_CHECK(sample_value && std::fabs(*sample_value - 3.5f) < 1.0e-6f);
      }
    }
  }

  const std::string metadata_json = R"JSON({
    "primSpecs": {"/Meta": {"name":"Meta", "typeName":"Scope",
      "specifier":"def", "properties": {"a": {"propertyType":"attribute",
        "isCustom":true, "attribute": {"name":"a", "typeName":"float",
          "hasValue":true, "valueType":"data", "value":"1.0",
          "metadata": {"customData": {"quality": 7},
                       "sdrMetadata": {"role": "surface"}}}}}}}
  })JSON";
  Layer metadata_layer;
  TEST_CHECK(JSONToLayer(metadata_json, &metadata_layer, &warn, &err));
  auto meta_it = metadata_layer.primspecs().find("/Meta");
  TEST_CHECK(meta_it != metadata_layer.primspecs().end());
  if (meta_it != metadata_layer.primspecs().end()) {
    const Attribute &metadata_attr = meta_it->second.props().at("a").get_attribute();
    TEST_CHECK(metadata_attr.metas().has_customData());
    TEST_CHECK(metadata_attr.metas().has_sdrMetadata());
    if (metadata_attr.metas().has_customData()) {
      auto quality = metadata_attr.metas().get_customData().at("quality").get_value<int64_t>();
      TEST_CHECK(quality && *quality == 7);
    }
  }
#else
  TEST_CHECK(true);
#endif
}

void json_writer_facade_test(void) {
#if defined(LIGHTUSD_WITH_JSON)
  Layer layer;
  layer.set_name("writer-layer");
  PrimSpec root(Specifier::Def, "Root");
  root.typeName() = "Xform";
  layer.primspecs()["/Root"] = std::move(root);

  lightusd::json::JsonWriter writer;
  writer.set_indent(0);
  std::string encoded;
  TEST_CHECK(writer.to_json(layer, &encoded));
  TEST_CHECK(!encoded.empty());

  Layer decoded;
  std::string warn, err;
  TEST_CHECK(JSONToLayer(encoded, &decoded, &warn, &err));
  TEST_CHECK(decoded.primspecs().find("/Root") != decoded.primspecs().end());
  TEST_CHECK(!writer.to_json(layer, nullptr));
#endif
}

void json_stage_runtime_roundtrip_test(void) {
#if defined(LIGHTUSD_WITH_JSON)
  const std::string stage_json = R"JSON({
    "version": 1,
    "properties": {"upAxis": "Z", "comment": "runtime stage"},
    "primChildren": {
      "Root": {"name": "Root", "typeName": "Xform", "specifier": "def",
        "primChildren": {
          "Child": {"name": "Child", "typeName": "Xform", "specifier": "over"}
        }
      }
    }
  })JSON";
  Stage stage;
  std::string warn, err;
  TEST_CHECK(JSONToStage(stage_json, &stage, &warn, &err));
  TEST_CHECK(stage.metas().upAxis.authored());
  TEST_CHECK(stage.metas().upAxis.get_value() == Axis::Z);
  TEST_CHECK(stage.metas().comment.value == "runtime stage");
  TEST_CHECK(stage.root_prims().size() == 1);
  if (stage.root_prims().size() == 1) {
    TEST_CHECK(stage.root_prims()[0].element_name() == "Root");
    TEST_CHECK(stage.root_prims()[0].children().size() == 1);
    TEST_CHECK(stage.root_prims()[0].children()[0].specifier() == Specifier::Over);
  }
  lightusd::json::JsonWriter writer;
  writer.set_indent(0);
  std::string writer_json;
  TEST_CHECK(writer.to_json(stage, &writer_json));
  Stage writer_stage;
  TEST_CHECK(JSONToStage(writer_json, &writer_stage, &warn, &err));
  TEST_CHECK(writer_stage.root_prims().size() == stage.root_prims().size());
  Prim prim;
  TEST_CHECK(JSONToStage(stage_json, &prim, &warn, &err));
  TEST_CHECK(prim.element_name() == "Root");

  const std::string external_buffer_json = R"JSON({
    "version": 1,
    "buffers": [{"byteLength": 1, "uri": "missing-buffer.bin"}],
    "primChildren": {"Root": {"name": "Root", "typeName": "Xform", "specifier": "def"}}
  })JSON";
  Stage external_buffer_stage;
  err.clear();
  TEST_CHECK(!JSONToStage(external_buffer_json, &external_buffer_stage, &warn,
                          &err));
  TEST_CHECK(err.find("asset resolver") != std::string::npos);
  AssetResolutionResolver json_resolver;
  AssetResolutionHandler json_handler;
  json_handler.resolve_fun = JSONTestResolveAsset;
  json_handler.size_fun = JSONTestSizeAsset;
  json_handler.read_fun = JSONTestReadAsset;
  json_resolver.register_wildcard_asset_resolution_handler(json_handler);
  JSONToUSDOptions json_options;
  json_options.resolver = &json_resolver;
  TEST_CHECK(JSONToStage(external_buffer_json, &external_buffer_stage, &warn,
                         &err, json_options));
  const std::string malformed_buffer_json = R"JSON({
    "version": 1,
    "buffers": [{"byteLength": 1, "uri": 7}],
    "primChildren": {"Root": {"name": "Root", "typeName": "Xform", "specifier": "def"}}
  })JSON";
  err.clear();
  TEST_CHECK(!JSONToStage(malformed_buffer_json, &external_buffer_stage, &warn,
                          &err, json_options));
  TEST_CHECK(err.find("buffer.uri") != std::string::npos);

  const std::string mesh_usda = R"USDA(#usda 1.0
def Mesh "Mesh" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)USDA";
  Stage mesh_stage;
  TEST_CHECK(parseUSDString(mesh_usda, &mesh_stage, &warn, &err));
  USDToJSONOptions options;
  auto mesh_json = ToJSON(mesh_stage, options);
  TEST_CHECK(mesh_json.has_value());
  if (mesh_json) {
    Stage decoded_mesh_stage;
    TEST_CHECK(JSONToStage(mesh_json.value(), &decoded_mesh_stage, &warn, &err));
    TEST_CHECK(decoded_mesh_stage.root_prims().size() == 1);
    if (decoded_mesh_stage.root_prims().size() == 1) {
      TEST_CHECK(decoded_mesh_stage.root_prims()[0].type_name() == "GeomMesh");
    }
  }

  USDToJSONOptions buffer_options;
  buffer_options.arrayMode = ArraySerializationMode::Buffer;
  auto buffered_mesh_json = ToJSON(mesh_stage, buffer_options);
  TEST_CHECK(buffered_mesh_json.has_value());
  if (buffered_mesh_json) {
    minijson::Value buffered;
    minijson::Error buffered_error;
    TEST_CHECK(minijson::Parse(buffered_mesh_json.value(), &buffered,
                               &buffered_error));
    TEST_CHECK(buffered.find("buffers") != nullptr);
    TEST_CHECK(buffered.find("bufferViews") != nullptr);
    TEST_CHECK(buffered.find("accessors") != nullptr);
    const minijson::Value *roots = buffered.find("primChildren");
    const minijson::Value *mesh = roots ? roots->find("Mesh") : nullptr;
    TEST_CHECK(mesh && mesh->find("points") &&
               mesh->find("points")->find("accessor") != nullptr);
    Stage decoded_buffered_mesh;
    TEST_CHECK(JSONToStage(buffered_mesh_json.value(), &decoded_buffered_mesh,
                           &warn, &err));
    TEST_CHECK(decoded_buffered_mesh.root_prims().size() == 1);
    if (decoded_buffered_mesh.root_prims().size() == 1) {
      auto decoded_mesh = decoded_buffered_mesh.root_prims()[0].data()
                              .get_value<GeomMesh>();
      TEST_CHECK(decoded_mesh.has_value());
      if (decoded_mesh) {
        auto decoded_points = decoded_mesh->points.get_value();
        TEST_CHECK(decoded_points.has_value());
        if (decoded_points) {
          std::vector<value::point3f> points;
          TEST_CHECK(decoded_points->get(value::TimeCode::Default(), &points));
          TEST_CHECK(points.size() == 3);
        }
      }
    }

    // Also accept the standard vector accessor convention, where VEC3 count
    // is the number of points rather than the legacy scalarized count emitted
    // by older LightUSD writers.
    if (mesh && mesh->find("points") &&
        mesh->find("points")->find("accessor")) {
      const size_t points_accessor = static_cast<size_t>(
          mesh->find("points")->find("accessor")->get_uint64());
      buffered["accessors"][points_accessor]["count"] = 3u;
      std::string standard_buffered_json;
      minijson::Error serialize_error;
      TEST_CHECK(minijson::Serialize(buffered, &standard_buffered_json,
                                     &serialize_error));
      Stage standard_buffered_stage;
      TEST_CHECK(JSONToStage(standard_buffered_json, &standard_buffered_stage,
                             &warn, &err));
      TEST_CHECK(standard_buffered_stage.root_prims().size() == 1);
    }

    // The direct GeomMesh convenience API must consume the same buffered
    // representation as JSONToStage.
    if (mesh) {
      minijson::Value direct_mesh = *mesh;
      direct_mesh["buffers"] = *buffered.find("buffers");
      direct_mesh["bufferViews"] = *buffered.find("bufferViews");
      direct_mesh["accessors"] = *buffered.find("accessors");
      std::string direct_mesh_json;
      minijson::Error direct_serialize_error;
      TEST_CHECK(minijson::Serialize(direct_mesh, &direct_mesh_json,
                                     &direct_serialize_error));
      GeomMesh decoded_direct_mesh;
      TEST_CHECK(JSONToGeomMesh(direct_mesh_json, &decoded_direct_mesh, &warn,
                                &err));
      TEST_CHECK(decoded_direct_mesh.points.authored());
    }
  }

  const std::string curves_usda = R"USDA(#usda 1.0
def BasisCurves "Curves" {
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] curveVertexCounts = [3]
    float[] widths = [0.1]
}
)USDA";
  Stage curves_stage;
  TEST_CHECK(parseUSDString(curves_usda, &curves_stage, &warn, &err));
  auto curves_json = ToJSON(curves_stage, options);
  TEST_CHECK(curves_json.has_value());
  if (curves_json) {
    Stage decoded_curves_stage;
    TEST_CHECK(JSONToStage(curves_json.value(), &decoded_curves_stage, &warn,
                           &err));
    TEST_CHECK(decoded_curves_stage.root_prims().size() == 1);
    if (decoded_curves_stage.root_prims().size() == 1) {
      TEST_CHECK(decoded_curves_stage.root_prims()[0].type_name() ==
                 "GeomBasisCurves");
    }
  }
  auto buffered_curves_json = ToJSON(curves_stage, buffer_options);
  TEST_CHECK(buffered_curves_json.has_value());
  if (buffered_curves_json) {
    minijson::Value buffered;
    minijson::Error buffered_error;
    TEST_CHECK(minijson::Parse(buffered_curves_json.value(), &buffered,
                               &buffered_error));
    TEST_CHECK(buffered.find("accessors") != nullptr);
    const minijson::Value *roots = buffered.find("primChildren");
    const minijson::Value *curves = roots ? roots->find("Curves") : nullptr;
    TEST_CHECK(curves && curves->find("points") &&
               curves->find("points")->find("accessor") != nullptr);
  }

  const std::string sphere_usda = R"USDA(#usda 1.0
def Sphere "Ball" {
    double radius = 2.25
}
)USDA";
  Stage sphere_stage;
  TEST_CHECK(parseUSDString(sphere_usda, &sphere_stage, &warn, &err));
  auto sphere_json = ToJSON(sphere_stage, options);
  TEST_CHECK(sphere_json.has_value());
  if (sphere_json) {
    Stage decoded_sphere_stage;
    TEST_CHECK(JSONToStage(sphere_json.value(), &decoded_sphere_stage, &warn,
                           &err));
    TEST_CHECK(decoded_sphere_stage.root_prims().size() == 1);
    if (decoded_sphere_stage.root_prims().size() == 1) {
      TEST_CHECK(decoded_sphere_stage.root_prims()[0].type_name() ==
                 "GeomSphere");
      auto sphere = decoded_sphere_stage.root_prims()[0].data()
                        .get_value<GeomSphere>();
      TEST_CHECK(sphere.has_value());
      if (sphere) {
        TEST_CHECK(std::fabs(sphere->radius.get_value().get_value() - 2.25) <
                   1.0e-9);
      }
    }
  }
  const std::string analytic_json = R"JSON({
    "version": 1,
    "primChildren": {
      "Cone": {"name":"Cone", "typeName":"GeomCone", "height":4.0,
        "radius":1.25, "axis":"Y", "specifier":"def"},
      "Plane": {"name":"Plane", "typeName":"GeomPlane", "width":3.0,
        "length":5.0, "axis":"X", "specifier":"over"}
    }
  })JSON";
  Stage analytic_stage;
  TEST_CHECK(JSONToStage(analytic_json, &analytic_stage, &warn, &err));
  TEST_CHECK(analytic_stage.root_prims().size() == 2);
  if (analytic_stage.root_prims().size() == 2) {
    TEST_CHECK(analytic_stage.root_prims()[0].type_name() == "GeomCone");
    TEST_CHECK(analytic_stage.root_prims()[1].type_name() == "GeomPlane");
  }

  const std::string unknown_json = R"JSON({
    "version": 1,
    "primChildren": {
      "Proc": {"name":"Proc", "typeName":"UsdProcCustom", "specifier":"class",
        "properties": {"vendor:weight": {"propertyType":"attribute",
          "isCustom":true, "attribute": {"typeName":"float", "hasValue":true,
            "valueType":"data", "value":"3.5"}}},
        "primChildren": {"Nested": {"name":"Nested", "typeName":"VendorShape"}}}
    }
  })JSON";
  Stage unknown_stage;
  TEST_CHECK(JSONToStage(unknown_json, &unknown_stage, &warn, &err));
  TEST_CHECK(unknown_stage.root_prims().size() == 1);
  if (unknown_stage.root_prims().size() == 1) {
    const Prim &proc = unknown_stage.root_prims()[0];
    TEST_CHECK(proc.data().get_value<Model>().has_value());
    if (auto model = proc.data().get_value<Model>()) {
      TEST_CHECK(model->prim_type_name == "UsdProcCustom");
      TEST_CHECK(model->props.find("vendor:weight") != model->props.end());
      if (model->props.find("vendor:weight") != model->props.end()) {
        TEST_CHECK(model->props.at("vendor:weight").has_custom());
      }
    }
    auto generic_json = ToJSON(unknown_stage, options);
    TEST_CHECK(generic_json.has_value());
    if (generic_json) {
      TEST_CHECK(generic_json->find("primChildren") != nullptr);
      const auto *roots = generic_json->find("primChildren");
      if (roots && roots->is_object()) {
        const auto *encoded = roots->find("Proc");
        TEST_CHECK(encoded && encoded->find("typeName") &&
                   encoded->find("typeName")->get_string() == "UsdProcCustom");
        TEST_CHECK(encoded && encoded->find("properties") &&
                   encoded->find("properties")->find("vendor:weight"));
      }
    }
  }
#else
  TEST_CHECK(true);
#endif
}

//
// Test cases
//

void usda_roundtrip_basic_test(void) {
  std::string err;

  // Test 1: Empty stage with just header
  {
    std::string usd = R"(#usda 1.0
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Empty stage test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Test 2: Simple def Xform
  {
    std::string usd = R"(#usda 1.0

def Xform "Root"
{
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Simple Xform test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Test 3: Nested hierarchy
  {
    std::string usd = R"(#usda 1.0

def Xform "Root"
{
    def Xform "Child1"
    {
        def Xform "GrandChild"
        {
        }
    }

    def Xform "Child2"
    {
    }
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Nested hierarchy test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }
}

void usd_dump_json_test(void) {
  const std::string usd = R"(#usda 1.0

def Xform "Root"
{
    string purpose = "render"
    def Scope "Child"
    {
    }
}
)";
  Layer layer;
  std::string warn, err;
  const bool loaded = LoadLayerFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()), usd.size(),
      "inspect-json.usda", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) return;

  InspectOptions options;
  options.format = InspectOutputFormat::Json;
  options.indent_width = 2;
  const std::string output = InspectLayer(layer, options);
  TEST_CHECK(output.find("JSON output not yet implemented") == std::string::npos);

  minijson::Value parsed;
  minijson::Error parse_error;
  TEST_CHECK(minijson::Parse(output, &parsed, &parse_error));
  TEST_CHECK(parsed.is_object());
  const minijson::Value *prims = parsed.find("primSpecs");
  TEST_CHECK(prims && prims->is_object());
  const minijson::Value *root = prims ? prims->find("Root") : nullptr;
  TEST_CHECK(root && root->is_object());
  TEST_CHECK(root && root->find("children") && root->find("children")->is_object());
}

void usda_roundtrip_xform_test(void) {
  std::string err;

  // Test with xformOps
  {
    std::string usd = R"(#usda 1.0

def Xform "Root"
{
    double3 xformOp:translate = (1.0, 2.0, 3.0)
    double3 xformOp:scale = (1.5, 1.5, 1.5)
    float3 xformOp:rotateXYZ = (0.0, 45.0, 0.0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("XformOps test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Test with matrix transform
  {
    std::string usd = R"(#usda 1.0

def Xform "MatrixXform"
{
    matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (10, 20, 30, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Matrix transform test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }
}

void usda_roundtrip_mesh_test(void) {
  std::string err;

  // Simple triangle mesh
  {
    std::string usd = R"(#usda 1.0

def Mesh "Triangle"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0.5, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Triangle mesh test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Cube mesh with normals
  {
    std::string usd = R"(#usda 1.0

def Mesh "Cube"
{
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
        (0, 0, -1), (0, 0, 1), (0, -1, 0),
        (1, 0, 0), (0, 1, 0), (-1, 0, 0)
    ]
    uniform token subdivisionScheme = "none"
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Cube mesh test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }
}

void usda_roundtrip_material_test(void) {
  std::string err;

  // UsdPreviewSurface material
  {
    std::string usd = R"(#usda 1.0

def Material "SimpleMaterial"
{
    token outputs:surface.connect = </SimpleMaterial/PreviewSurface.outputs:surface>

    def Shader "PreviewSurface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
        float inputs:metallic = 0.0
        float inputs:roughness = 0.5
        token outputs:surface
    }
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Material test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }
}

void usda_roundtrip_timesamples_test(void) {
  std::string err;

  // Simple time-sampled animation
  {
    std::string usd = R"(#usda 1.0
(
    startTimeCode = 1
    endTimeCode = 10
)

def Xform "AnimatedXform"
{
    double3 xformOp:translate.timeSamples = {
        1: (0, 0, 0),
        5: (5, 0, 0),
        10: (10, 0, 0),
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Timesamples test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Animated visibility
  {
    std::string usd = R"(#usda 1.0

def Xform "VisibilityAnim"
{
    token visibility.timeSamples = {
        1: "inherited",
        5: "invisible",
        10: "inherited",
    }
}
)";
    bool ret = doRoundtripTest(usd, &err);
    if (!ret) {
      TEST_MSG("Visibility animation test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);
  }

  // Animated mesh points should preserve array timesamples across roundtrip.
  {
    std::string usd = R"(#usda 1.0
(
    startTimeCode = 1
    endTimeCode = 3
)

def Mesh "AnimatedMesh"
{
    point3f[] points.timeSamples = {
        1: [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
        2: [(0, 0, 0), (2, 0, 0), (0, 2, 0)],
        3: [(0, 0, 0), (3, 0, 0), (0, 3, 0)],
    }
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
}
)";

    RoundtripStages roundtrip;
    bool ret = doRoundtripTest(usd, &roundtrip, &err);
    if (!ret) {
      TEST_MSG("Animated mesh points timesamples test failed: %s", err.c_str());
    }
    TEST_CHECK(ret == true);

    if (!ret) {
      return;
    }

    auto prim_result =
        roundtrip.reparsed.GetPrimAtPath(Path("/AnimatedMesh", ""));
    TEST_CHECK(bool(prim_result));
    if (!prim_result) {
      return;
    }

    const GeomMesh *mesh = (*prim_result)->data().as<GeomMesh>();
    TEST_CHECK(mesh != nullptr);
    if (!mesh) {
      return;
    }

    const auto &points_attr = mesh->points.get_value_ref();
    TEST_CHECK(points_attr.has_value());
    if (!points_attr.has_value()) {
      return;
    }

    const value::TimeSamples *ts = points_attr.value().get_timesamples_ptr();
    TEST_CHECK(ts != nullptr);
    if (!ts) {
      return;
    }
    TEST_CHECK(ts->size() == 3);

    const auto &samples = ts->get_samples();
    TEST_CHECK(samples.size() == 3);
    if (samples.size() == 3) {
      TEST_CHECK(math::is_close(samples[0].t, 1.0));
      TEST_CHECK(math::is_close(samples[1].t, 2.0));
      TEST_CHECK(math::is_close(samples[2].t, 3.0));
      const auto *v0 = samples[0].value.as<std::vector<value::point3f>>();
      const auto *v1 = samples[1].value.as<std::vector<value::point3f>>();
      const auto *v2 = samples[2].value.as<std::vector<value::point3f>>();
      TEST_CHECK(v0 && v0->size() == 3);
      TEST_CHECK(v1 && v1->size() == 3);
      TEST_CHECK(v2 && v2->size() == 3);
      if (v1 && v1->size() == 3) {
        TEST_CHECK(math::is_close((*v1)[1].x, 2.0f));
        TEST_CHECK(math::is_close((*v1)[1].y, 0.0f));
        TEST_CHECK(math::is_close((*v1)[1].z, 0.0f));
        TEST_CHECK(math::is_close((*v1)[2].x, 0.0f));
        TEST_CHECK(math::is_close((*v1)[2].y, 2.0f));
        TEST_CHECK(math::is_close((*v1)[2].z, 0.0f));
      }
    }

    std::vector<value::point3f> held_points;
    TEST_CHECK(ts->get(&held_points, 2.0, value::TimeSampleInterpolationType::Held));
    TEST_CHECK(held_points.size() == 3);
    if (held_points.size() == 3) {
      TEST_CHECK(math::is_close(held_points[1].x, 2.0f));
      TEST_CHECK(math::is_close(held_points[2].y, 2.0f));
    }
  }
}
