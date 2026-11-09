#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-usdc-reconstruct.h"
#include "lightusd.hh"
#include "core/composition-types.hh"
#include "core/model-scope.hh"
#include "core/path.hh"
#include "core/prim.hh"
#include "core/prim-enums.hh"
#include "core/prim-spec.hh"
#include "layer.hh"
#include "io-util.hh"
#include "crate-writer.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdSkel.hh"
#include "usdPhysics.hh"
#include "core/collection-api.hh"

#include <cmath>
#include <string>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

namespace {

std::string GetUsdcFixturePath(const std::string &filename) {
  std::string base_dir = io::GetBaseDir(std::string(__FILE__));
  std::string usdc_dir = io::JoinPath(base_dir, "../usdc");
  return io::JoinPath(usdc_dir, filename);
}

bool LoadStageFromUsdcFixture(const std::string &filename, Stage *stage,
                              std::string *err) {
  if (!stage) {
    if (err) {
      (*err) = "Stage pointer is null.";
    }
    return false;
  }

  std::string path = GetUsdcFixturePath(filename);
  if (!io::FileExists(path)) {
    if (err) {
      (*err) = "Fixture not found: " + path;
    }
    return false;
  }

  std::string warn;
  std::string load_err;
  bool ret = LoadUSDFromFile(path, stage, &warn, &load_err);
  if (!ret) {
    if (err) {
      (*err) = load_err;
    }
    return false;
  }

  return true;
}

bool LoadStageFromUsdcFixtureWithOptions(const std::string &filename,
                                         const USDLoadOptions &options,
                                         Stage *stage, std::string *err) {
  if (!stage) {
    if (err) {
      (*err) = "Stage pointer is null.";
    }
    return false;
  }

  std::string path = GetUsdcFixturePath(filename);
  if (!io::FileExists(path)) {
    if (err) {
      (*err) = "Fixture not found: " + path;
    }
    return false;
  }

  std::string warn;
  std::string load_err;
  bool ret = LoadUSDFromFile(path, stage, &warn, &load_err, options);
  if (!ret) {
    if (err) {
      (*err) = load_err;
    }
    return false;
  }

  return true;
}

bool LoadLayerFromUsdcFixture(const std::string &filename, Layer *layer,
                              std::string *err) {
  if (!layer) {
    if (err) {
      (*err) = "Layer pointer is null.";
    }
    return false;
  }

  std::string path = GetUsdcFixturePath(filename);
  if (!io::FileExists(path)) {
    if (err) {
      (*err) = "Fixture not found: " + path;
    }
    return false;
  }

  std::string warn;
  std::string load_err;
  bool ret = LoadLayerFromFile(path, layer, &warn, &load_err);
  if (!ret) {
    if (err) {
      (*err) = load_err;
    }
    return false;
  }

  return true;
}

bool LoadLayerFromUsdcFixtureWithOptions(const std::string &filename,
                                         const USDLoadOptions &options,
                                         Layer *layer, std::string *err) {
  if (!layer) {
    if (err) {
      (*err) = "Layer pointer is null.";
    }
    return false;
  }

  std::string path = GetUsdcFixturePath(filename);
  if (!io::FileExists(path)) {
    if (err) {
      (*err) = "Fixture not found: " + path;
    }
    return false;
  }

  std::string warn;
  std::string load_err;
  bool ret = LoadLayerFromFile(path, layer, &warn, &load_err, options);
  if (!ret) {
    if (err) {
      (*err) = load_err;
    }
    return false;
  }

  return true;
}

bool WriteLayerToUsdc(const Layer &layer, const std::string &path,
                      std::string *err) {
  lightusd::legacy::experimental::CrateWriter writer(path);
  std::string local_err;
  if (!writer.Open(&local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  if (!writer.ConvertLayerToSpecs(layer, &local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  if (!writer.Finalize(&local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  writer.Close();
  return true;
}

// Write Layer to in-memory USDC buffer (avoids writing runtime files to
// tests/usdc/ that can race with usdc-parser-unit-test).
bool WriteLayerToUsdcMemory(const Layer &layer, std::vector<uint8_t> *out,
                            std::string *err) {
  auto stream = std::make_unique<lightusd::legacy::experimental::MemoryOutputStream>();
  auto *mem_ptr = stream.get();
  lightusd::legacy::experimental::CrateWriter writer(std::move(stream));
  std::string local_err;
  if (!writer.Open(&local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (!writer.ConvertLayerToSpecs(layer, &local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (!writer.Finalize(&local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (out) *out = mem_ptr->TakeBuffer();
  writer.Close();
  return true;
}

// Write Stage to in-memory USDC buffer.
bool WriteStageToUsdcMemory(const Stage &stage, std::vector<uint8_t> *out,
                            std::string *err) {
  auto stream = std::make_unique<lightusd::legacy::experimental::MemoryOutputStream>();
  auto *mem_ptr = stream.get();
  lightusd::legacy::experimental::CrateWriter writer(std::move(stream));
  std::string local_err;
  if (!writer.Open(&local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (!writer.ConvertStageToSpecs(stage, &local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (!writer.Finalize(&local_err)) {
    if (err) (*err) = local_err;
    return false;
  }
  if (out) *out = mem_ptr->TakeBuffer();
  writer.Close();
  return true;
}

bool WriteStageToUsdc(const Stage &stage, const std::string &path,
                      std::string *err) {
  lightusd::legacy::experimental::CrateWriter writer(path);
  std::string local_err;
  if (!writer.Open(&local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  if (!writer.ConvertStageToSpecs(stage, &local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  if (!writer.Finalize(&local_err)) {
    if (err) {
      (*err) = local_err;
    }
    return false;
  }
  writer.Close();
  return true;
}

bool BuildReferencesUsdcBuffer(std::vector<uint8_t> *out, std::string *err) {
  constexpr size_t kRefCount = 256;
  constexpr size_t kSegmentSize = 256;  // Keep under maxTokenLength (64K)

  Layer layer;
  PrimSpec prim(Specifier::Def, "RefHeavy");
  PrimMeta &meta = prim.metas();

  std::vector<Reference> refs;
  refs.reserve(kRefCount);
  std::string segment(kSegmentSize, 'A');
  for (size_t i = 0; i < kRefCount; ++i) {
    Reference ref;
    std::string asset_path = "./ref_" + std::to_string(i) + "_" + segment + ".usd";
    ref.asset_path = lightusd::legacy::value::AssetPath(asset_path);
    ref.prim_path = Path("/RefHeavy", "");
    refs.emplace_back(std::move(ref));
  }

  std::vector<std::pair<ListEditQual, std::vector<Reference>>> listops;
  listops.emplace_back(ListEditQual::Prepend, std::move(refs));
  meta.references = listops;

  if (!layer.emplace_primspec("RefHeavy", std::move(prim))) {
    if (err) {
      (*err) = "Failed to add primspec for RefHeavy.";
    }
    return false;
  }

  return WriteLayerToUsdcMemory(layer, out, err);
}

bool BuildStageMetaSublayersUsdcBuffer(std::vector<uint8_t> *out,
                                       std::string *err) {
  constexpr size_t kSubLayerCount = 256;
  constexpr size_t kSegmentSize = 4096;

  Stage stage;
  StageMetas &metas = stage.metas();

  std::vector<SubLayer> sublayers;
  sublayers.reserve(kSubLayerCount);

  std::string segment(kSegmentSize, 'S');
  for (size_t i = 0; i < kSubLayerCount; ++i) {
    SubLayer sublayer;
    std::string asset_path =
        "./sublayer_" + std::to_string(i) + "_" + segment + ".usd";
    sublayer.assetPath = lightusd::legacy::value::AssetPath(asset_path);
    sublayers.emplace_back(std::move(sublayer));
  }

  metas.subLayers = std::move(sublayers);
  metas.defaultPrim = lightusd::legacy::value::token("Root");
  metas.primChildren.push_back(lightusd::legacy::value::token("Root"));

  std::string doc_segment(1024, 'D');
  metas.doc.value = doc_segment;
  metas.doc.is_triple_quoted = true;
  metas.comment.value = doc_segment;
  metas.comment.is_triple_quoted = true;

  Model model;
  model.name = "Root";
  Prim root_prim(model);
  if (!stage.add_root_prim(std::move(root_prim))) {
    if (err) {
      (*err) = "Failed to add root prim for Root.";
    }
    return false;
  }
  stage.commit();

  return WriteStageToUsdcMemory(stage, out, err);
}

bool BuildStageMetaCustomDataUsdcBuffer(std::vector<uint8_t> *out,
                                        std::string *err) {
  constexpr size_t kCustomDataSize = 32 * 1024;  // Keep under maxTokenLength (64K)

  Stage stage;
  StageMetas &metas = stage.metas();

  CustomDataType custom;
  custom["blob"] = MetaVariable(std::string(kCustomDataSize, 'C'));
  metas.customLayerData = custom;
  metas.customLayerDataAuthored = true;
  metas.defaultPrim = lightusd::legacy::value::token("Root");

  Model model;
  model.name = "Root";
  Prim root_prim(model);
  if (!stage.add_root_prim(std::move(root_prim))) {
    if (err) {
      (*err) = "Failed to add root prim for Root.";
    }
    return false;
  }
  stage.commit();

  return WriteStageToUsdcMemory(stage, out, err);
}

bool BuildCompositionUsdcBuffer(std::vector<uint8_t> *out, std::string *err) {
  constexpr size_t kArcCount = 256;
  constexpr size_t kPathSegmentSize = 128;  // Keep under maxTokenLength (64K)
  constexpr size_t kApiCount = 128;
  constexpr size_t kApiSegmentSize = 128;   // Keep under maxTokenLength (64K)

  Layer layer;
  PrimSpec prim(Specifier::Def, "ArcHeavy");
  PrimMeta &meta = prim.metas();

  std::string path_segment(kPathSegmentSize, 'P');
  std::vector<Path> inherits_paths;
  inherits_paths.reserve(kArcCount);
  for (size_t i = 0; i < kArcCount; ++i) {
    std::string p =
        "/Inherit_" + std::to_string(i) + "_" + path_segment;
    inherits_paths.emplace_back(Path(p, ""));
  }
  meta.inherits = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
  meta.inherits->emplace_back(ListEditQual::Prepend,
                              std::move(inherits_paths));

  std::vector<Path> specializes_paths;
  specializes_paths.reserve(kArcCount);
  for (size_t i = 0; i < kArcCount; ++i) {
    std::string p =
        "/Specialize_" + std::to_string(i) + "_" + path_segment;
    specializes_paths.emplace_back(Path(p, ""));
  }
  meta.specializes = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
  meta.specializes->emplace_back(ListEditQual::Prepend,
                                 std::move(specializes_paths));

  std::vector<Payload> payloads;
  payloads.reserve(kArcCount);
  for (size_t i = 0; i < kArcCount; ++i) {
    Payload payload;
    std::string asset_path =
        "./payload_" + std::to_string(i) + "_" + path_segment + ".usd";
    payload.asset_path = lightusd::legacy::value::AssetPath(asset_path);
    payload.prim_path = Path("/ArcHeavy", "");
    payloads.emplace_back(std::move(payload));
  }
  meta.payload = std::vector<std::pair<ListEditQual, std::vector<Payload>>>();
  meta.payload->emplace_back(ListEditQual::Prepend, std::move(payloads));

  APISchemas api_schemas;
  api_schemas.listOpQual = ListEditQual::Prepend;
  std::string api_segment(kApiSegmentSize, 'A');
  for (size_t i = 0; i < kApiCount; ++i) {
    std::string instance_name =
        "Instance_" + std::to_string(i) + "_" + api_segment;
    api_schemas.names.push_back(
        {APISchemas::APIName::CollectionAPI, instance_name});
  }
  meta.set_apiSchemas(api_schemas);

  if (!layer.emplace_primspec("ArcHeavy", std::move(prim))) {
    if (err) {
      (*err) = "Failed to add primspec for ArcHeavy.";
    }
    return false;
  }

  return WriteLayerToUsdcMemory(layer, out, err);
}

bool BuildVariantSpecUsdcBuffer(std::vector<uint8_t> *out, std::string *err) {
  Layer layer;
  PrimSpec prim(Specifier::Def, "VariantOwner");
  PrimMeta &meta = prim.metas();

  VariantSelectionMap selections;
  selections["shapeVariant"] = "Capsule";
  meta.variants = selections;
  meta.variantSets = std::vector<std::pair<ListEditQual, std::vector<std::string>>>();
  meta.variantSets->push_back(
      std::make_pair(ListEditQual::Prepend, std::vector<std::string>{"shapeVariant"}));

  VariantSetSpec vs;
  vs.name = "shapeVariant";
  PrimSpec variant_spec(Specifier::Def, "Xform", "VariantPrim");
  vs.variantSet["Capsule"] = variant_spec;
  prim.variantSets()["shapeVariant"] = vs;

  if (!layer.emplace_primspec("VariantOwner", std::move(prim))) {
    if (err) {
      (*err) = "Failed to add primspec for VariantOwner.";
    }
    return false;
  }

  return WriteLayerToUsdcMemory(layer, out, err);
}

bool BuildVariantSpecNestedUsdcBuffer(std::vector<uint8_t> *out, std::string *err) {
  Layer layer;
  PrimSpec prim(Specifier::Def, "VariantOwner");
  PrimMeta &meta = prim.metas();

  VariantSelectionMap selections;
  selections["shapeVariant"] = "Capsule";
  meta.variants = selections;
  meta.variantSets = std::vector<std::pair<ListEditQual, std::vector<std::string>>>();
  meta.variantSets->push_back(
      std::make_pair(ListEditQual::Prepend, std::vector<std::string>{"shapeVariant"}));

  VariantSetSpec vs;
  vs.name = "shapeVariant";
  PrimSpec variant_spec(Specifier::Def, "Xform", "VariantPrim");

  VariantSetSpec nested_vs;
  nested_vs.name = "lod";
  PrimSpec lod_a(Specifier::Def, "Xform", "LodAChild");
  PrimSpec lod_b(Specifier::Def, "Xform", "LodBChild");
  nested_vs.variantSet["LodA"] = lod_a;
  nested_vs.variantSet["LodB"] = lod_b;
  variant_spec.variantSets()["lod"] = nested_vs;

  vs.variantSet["Capsule"] = variant_spec;
  prim.variantSets()["shapeVariant"] = vs;

  if (!layer.emplace_primspec("VariantOwner", std::move(prim))) {
    if (err) {
      (*err) = "Failed to add primspec for VariantOwner.";
    }
    return false;
  }

  return WriteLayerToUsdcMemory(layer, out, err);
}

bool BuildStageVariantUsdcBuffer(std::vector<uint8_t> *out, std::string *err) {
  Stage stage;
  Xform root_xform;
  root_xform.name = "VariantOwner";
  Prim root_prim(root_xform);
  root_prim.prim_type_name() = "Xform";

  PrimMeta &meta = root_prim.metas();
  VariantSelectionMap selections;
  selections["shapeVariant"] = "Capsule";
  meta.variants = selections;
  meta.variantSets = std::vector<std::pair<ListEditQual, std::vector<std::string>>>();
  meta.variantSets->push_back(
      std::make_pair(ListEditQual::Prepend, std::vector<std::string>{"shapeVariant"}));

  VariantSet vs;
  vs.name = "shapeVariant";

  Variant variant;
  Xform child_xform;
  child_xform.name = "Child";
  Prim child_prim(child_xform);
  child_prim.prim_type_name() = "Xform";
  variant.primChildren().push_back(std::move(child_prim));

  VariantSet nested_vs;
  nested_vs.name = "lod";
  Variant lod_a;
  Xform lod_a_child_xform;
  lod_a_child_xform.name = "LodAChild";
  Prim lod_a_child(lod_a_child_xform);
  lod_a_child.prim_type_name() = "Xform";
  lod_a.primChildren().push_back(std::move(lod_a_child));

  Variant lod_b;
  Xform lod_b_child_xform;
  lod_b_child_xform.name = "LodBChild";
  Prim lod_b_child(lod_b_child_xform);
  lod_b_child.prim_type_name() = "Xform";
  lod_b.primChildren().push_back(std::move(lod_b_child));

  nested_vs.variantSet["LodA"] = lod_a;
  nested_vs.variantSet["LodB"] = lod_b;
  variant.variantSets()["lod"] = nested_vs;
  vs.variantSet["Capsule"] = variant;

  root_prim.variantSets()["shapeVariant"] = vs;

  if (!stage.add_root_prim(std::move(root_prim))) {
    if (err) {
      (*err) = "Failed to add root prim for VariantOwner.";
    }
    return false;
  }
  stage.commit();

  return WriteStageToUsdcMemory(stage, out, err);
}

const Prim *FindPrimAtPath(const Stage &stage, const std::string &path_str) {
  const Prim *prim = nullptr;
  Path path(path_str, "");
  if (!stage.find_prim_at_path(path, prim)) {
    return nullptr;
  }
  return prim;
}

bool GetModelFloatProp(const Prim &prim, const std::string &name, float *out) {
  if (!out) {
    return false;
  }
  const Model *model = prim.as<Model>();
  if (!model) {
    return false;
  }
  auto prop_it = model->props.find(name);
  if (prop_it == model->props.end()) {
    return false;
  }
  if (!prop_it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = prop_it->second.get_attribute();
  auto val = attr.get_value<float>();
  if (!val) {
    return false;
  }
  (*out) = val.value();
  return true;
}

const Attribute *FindPrimAttribute(const Prim &prim, const std::string &name) {
  const Model *model = prim.as<Model>();
  if (!model) {
    return nullptr;
  }
  auto prop_it = model->props.find(name);
  if (prop_it == model->props.end()) {
    return nullptr;
  }
  if (!prop_it->second.is_attribute()) {
    return nullptr;
  }
  return &prop_it->second.get_attribute();
}

bool GetPrimSpecFloatProp(const PrimSpec &prim, const std::string &name, float *out) {
  if (!out) {
    return false;
  }
  const auto &props = prim.props();
  auto prop_it = props.find(name);
  if (prop_it == props.end()) {
    return false;
  }
  if (!prop_it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = prop_it->second.get_attribute();
  auto val = attr.get_value<float>();
  if (!val) {
    return false;
  }
  (*out) = val.value();
  return true;
}

bool GetVariantSelection(const PrimMeta &meta, const std::string &set_name,
                         std::string *out) {
  if (!out) {
    return false;
  }
  if (!meta.variants.has_value()) {
    return false;
  }
  const VariantSelectionMap &map = meta.variants.value();
  auto it = map.find(set_name);
  if (it == map.end()) {
    return false;
  }
  (*out) = it->second;
  return true;
}

bool HasVariantSetName(const PrimMeta &meta, const std::string &set_name) {
  if (!meta.variantSets.has_value()) {
    return false;
  }
  const auto &listops = meta.variantSets.value();
  for (const auto &item : listops) {
    for (const auto &name : item.second) {
      if (name == set_name) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void usdc_reconstruct_variant_properties_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("variantSet-prop-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/bora");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }

  const auto &variant_sets = prim->variantSets();
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  const VariantSet &vs = vs_it->second;

  auto capsule_it = vs.variantSet.find("Capsule");
  TEST_CHECK(capsule_it != vs.variantSet.end());
  if (capsule_it != vs.variantSet.end()) {
    const Variant &capsule = capsule_it->second;
    auto prop_it = capsule.properties().find("myval");
    TEST_CHECK(prop_it != capsule.properties().end());
    if (prop_it != capsule.properties().end()) {
      TEST_CHECK(prop_it->second.is_attribute());
      const Attribute &attr = prop_it->second.get_attribute();
      TEST_CHECK(attr.type_name() == "double");
      auto val = attr.get_value<double>();
      TEST_CHECK(val.has_value());
      if (val) {
        TEST_CHECK(std::fabs(val.value() - 2.0) < 1e-12);
      }
    }
  }

  auto cone_it = vs.variantSet.find("Cone");
  TEST_CHECK(cone_it != vs.variantSet.end());
  if (cone_it != vs.variantSet.end()) {
    const Variant &cone = cone_it->second;
    auto prop_it = cone.properties().find("myval");
    TEST_CHECK(prop_it != cone.properties().end());
    if (prop_it != cone.properties().end()) {
      TEST_CHECK(prop_it->second.is_attribute());
      const Attribute &attr = prop_it->second.get_attribute();
      TEST_CHECK(attr.type_name() == "int");
      auto val = attr.get_value<int>();
      TEST_CHECK(val.has_value());
      if (val) {
        TEST_CHECK(val.value() == 3);
      }
    }
  }
}

void usdc_reconstruct_variant_prim_children_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("variantSet-prim-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/bora");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }

  const auto &variant_sets = prim->variantSets();
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  const VariantSet &vs = vs_it->second;

  auto capsule_it = vs.variantSet.find("Capsule");
  TEST_CHECK(capsule_it != vs.variantSet.end());
  if (capsule_it != vs.variantSet.end()) {
    const Variant &capsule = capsule_it->second;
    TEST_CHECK(capsule.primChildren().size() == 1);
    if (!capsule.primChildren().empty()) {
      const Prim &child = capsule.primChildren()[0];
      TEST_CHECK(child.element_name() == "bora");
      const Model *model = child.as<Model>();
      TEST_CHECK(model != nullptr);
      if (model) {
        auto prop_it = model->props.find("mycapsule");
        TEST_CHECK(prop_it != model->props.end());
        if (prop_it != model->props.end()) {
          const Attribute &attr = prop_it->second.get_attribute();
          TEST_CHECK(attr.type_name() == "float");
          auto val = attr.get_value<float>();
          TEST_CHECK(val.has_value());
          if (val) {
            TEST_CHECK(std::fabs(val.value() - 1.3f) < 1e-6f);
          }
        }
      }
    }
  }

  auto cone_it = vs.variantSet.find("Cone");
  TEST_CHECK(cone_it != vs.variantSet.end());
  if (cone_it != vs.variantSet.end()) {
    const Variant &cone = cone_it->second;
    TEST_CHECK(cone.primChildren().size() == 1);
    if (!cone.primChildren().empty()) {
      const Prim &child = cone.primChildren()[0];
      TEST_CHECK(child.element_name() == "bora");
      const Model *model = child.as<Model>();
      TEST_CHECK(model != nullptr);
      if (model) {
        auto prop_it = model->props.find("mycone");
        TEST_CHECK(prop_it != model->props.end());
        if (prop_it != model->props.end()) {
          const Attribute &attr = prop_it->second.get_attribute();
          TEST_CHECK(attr.type_name() == "float");
          auto val = attr.get_value<float>();
          TEST_CHECK(val.has_value());
          if (val) {
            TEST_CHECK(std::fabs(val.value() - 2.3f) < 1e-6f);
          }
        }
      }
    }
  }
}

void usdc_reconstruct_nested_variant_sets_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("variantSet-nested-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/Implicits");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }

  const auto &variant_sets = prim->variantSets();
  TEST_CHECK(variant_sets.find("geo") == variant_sets.end());
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  const VariantSet &shape_vs = vs_it->second;
  auto capsule_it = shape_vs.variantSet.find("Capsule");
  TEST_CHECK(capsule_it != shape_vs.variantSet.end());
  if (capsule_it == shape_vs.variantSet.end()) {
    return;
  }

  const Variant &capsule = capsule_it->second;
  const auto &nested_sets = capsule.variantSets();
  auto nested_it = nested_sets.find("geo");
  TEST_CHECK(nested_it != nested_sets.end());
  if (nested_it == nested_sets.end()) {
    return;
  }

  const VariantSet &geo_vs = nested_it->second;
  auto a_it = geo_vs.variantSet.find("CapsuleA");
  TEST_CHECK(a_it != geo_vs.variantSet.end());
  if (a_it != geo_vs.variantSet.end()) {
    const Variant &variant_a = a_it->second;
    TEST_CHECK(variant_a.primChildren().size() == 1);
    if (!variant_a.primChildren().empty()) {
      const Prim &child = variant_a.primChildren()[0];
      TEST_CHECK(child.element_name() == "PillA");
      TEST_CHECK(child.prim_type_name() == "Capsule");
    }
  }

  auto b_it = geo_vs.variantSet.find("CapsuleB");
  TEST_CHECK(b_it != geo_vs.variantSet.end());
  if (b_it != geo_vs.variantSet.end()) {
    const Variant &variant_b = b_it->second;
    TEST_CHECK(variant_b.primChildren().size() == 1);
    if (!variant_b.primChildren().empty()) {
      const Prim &child = variant_b.primChildren()[0];
      TEST_CHECK(child.element_name() == "PillB");
      TEST_CHECK(child.prim_type_name() == "Capsule");
    }
  }
}

void usdc_reconstruct_variant_name_collision_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("variantSet-collision-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim_a = FindPrimAtPath(stage, "/A");
  const Prim *prim_b = FindPrimAtPath(stage, "/B");
  TEST_CHECK(prim_a != nullptr);
  TEST_CHECK(prim_b != nullptr);
  if (!prim_a || !prim_b) {
    return;
  }

  auto get_capsule_variant = [](const Prim &prim) -> const Variant* {
    const auto &variant_sets = prim.variantSets();
    auto vs_it = variant_sets.find("shapeVariant");
    if (vs_it == variant_sets.end()) {
      return nullptr;
    }
    const VariantSet &vs = vs_it->second;
    auto v_it = vs.variantSet.find("Capsule");
    if (v_it == vs.variantSet.end()) {
      return nullptr;
    }
    return &v_it->second;
  };

  const Variant *capsule_a = get_capsule_variant(*prim_a);
  const Variant *capsule_b = get_capsule_variant(*prim_b);
  TEST_CHECK(capsule_a != nullptr);
  TEST_CHECK(capsule_b != nullptr);
  if (!capsule_a || !capsule_b) {
    return;
  }

  TEST_CHECK(capsule_a->primChildren().size() == 1);
  TEST_CHECK(capsule_b->primChildren().size() == 1);
  if (capsule_a->primChildren().size() != 1 || capsule_b->primChildren().size() != 1) {
    return;
  }

  float tag_a = 0.0f;
  float tag_b = 0.0f;
  TEST_CHECK(GetModelFloatProp(capsule_a->primChildren()[0], "tag", &tag_a));
  TEST_CHECK(GetModelFloatProp(capsule_b->primChildren()[0], "tag", &tag_b));
  TEST_CHECK(std::fabs(tag_a - 1.0f) < 1e-6f);
  TEST_CHECK(std::fabs(tag_b - 3.0f) < 1e-6f);

  const auto &nested_a = capsule_a->variantSets();
  const auto &nested_b = capsule_b->variantSets();
  auto geo_a_it = nested_a.find("geo");
  auto geo_b_it = nested_b.find("geo");
  TEST_CHECK(geo_a_it != nested_a.end());
  TEST_CHECK(geo_b_it != nested_b.end());
  if (geo_a_it == nested_a.end() || geo_b_it == nested_b.end()) {
    return;
  }

  auto lod_a_it = geo_a_it->second.variantSet.find("LodA");
  auto lod_b_it = geo_b_it->second.variantSet.find("LodA");
  TEST_CHECK(lod_a_it != geo_a_it->second.variantSet.end());
  TEST_CHECK(lod_b_it != geo_b_it->second.variantSet.end());
  if (lod_a_it == geo_a_it->second.variantSet.end() ||
      lod_b_it == geo_b_it->second.variantSet.end()) {
    return;
  }

  TEST_CHECK(lod_a_it->second.primChildren().size() == 1);
  TEST_CHECK(lod_b_it->second.primChildren().size() == 1);
  if (lod_a_it->second.primChildren().size() != 1 ||
      lod_b_it->second.primChildren().size() != 1) {
    return;
  }

  float id_a = 0.0f;
  float id_b = 0.0f;
  TEST_CHECK(GetModelFloatProp(lod_a_it->second.primChildren()[0], "id", &id_a));
  TEST_CHECK(GetModelFloatProp(lod_b_it->second.primChildren()[0], "id", &id_b));
  TEST_CHECK(std::fabs(id_a - 10.0f) < 1e-6f);
  TEST_CHECK(std::fabs(id_b - 30.0f) < 1e-6f);
}

void usdc_reconstruct_variant_selection_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("variantSet-prim-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/bora");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }

  std::string selection;
  TEST_CHECK(GetVariantSelection(prim->metas(), "shapeVariant", &selection));
  if (!selection.empty()) {
    TEST_CHECK(selection == "Capsule");
  }
  TEST_CHECK(HasVariantSetName(prim->metas(), "shapeVariant"));
}

void usdc_memory_budget_customdata_limit_test(void) {
  Stage stage;
  std::string err;
  USDLoadOptions options;
  // Set a very tight budget so that even the reduced 32K blob exceeds it
  options.max_memory_limit_in_mb = 0;
  bool loaded = LoadStageFromUsdcFixtureWithOptions(
      "memory-budget-attr-customdata-001.usdc", options, &stage, &err);
  TEST_CHECK(!loaded);
  if (loaded) {
    return;
  }
  if (!err.empty()) {
    TEST_MSG("%s", err.c_str());
  }
}

void usdc_memory_budget_customdata_success_test(void) {
  Stage stage;
  std::string err;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 16;
  bool loaded = LoadStageFromUsdcFixtureWithOptions(
      "memory-budget-attr-customdata-001.usdc", options, &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/MetaHeavy");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }
  const Attribute *attr = FindPrimAttribute(*prim, "payload");
  TEST_CHECK(attr != nullptr);
  if (!attr) {
    return;
  }
  TEST_CHECK(attr->metas().has_customData());
  if (attr->metas().has_customData()) {
    Dictionary custom = attr->metas().get_customData();
    TEST_CHECK(custom.find("blob") != custom.end());
  }
}

void usdc_memory_budget_references_limit_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildReferencesUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 0;  // Tight budget to trigger failure
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err, options);
  TEST_CHECK(!loaded);
  if (loaded) {
    return;
  }
  if (!err.empty()) {
    TEST_MSG("%s", err.c_str());
  }
}

void usdc_memory_budget_references_success_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildReferencesUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 32;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err, options);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("RefHeavy");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  const PrimMeta &meta = prim_it->second.metas();
  TEST_CHECK(meta.references.has_value());
  if (!meta.references.has_value()) {
    return;
  }

  size_t ref_count = 0;
  for (const auto &listop : meta.references.value()) {
    ref_count += listop.second.size();
  }
  TEST_CHECK(ref_count == 256);
}

void usdc_memory_budget_stage_meta_sublayers_limit_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildStageMetaSublayersUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Stage stage;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 1;
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err, options);
  TEST_CHECK(!loaded);
  if (loaded) {
    return;
  }
  if (!err.empty()) {
    TEST_MSG("%s", err.c_str());
  }
}

void usdc_memory_budget_stage_meta_sublayers_success_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildStageMetaSublayersUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Stage stage;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 64;
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err, options);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const StageMetas &metas = stage.metas();
  TEST_CHECK(!metas.subLayers.empty());
  TEST_CHECK(!metas.doc.value.empty());
  TEST_CHECK(!metas.comment.value.empty());
}

void usdc_memory_budget_stage_meta_customdata_limit_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildStageMetaCustomDataUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Stage stage;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 0;  // Tight budget to trigger failure
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err, options);
  TEST_CHECK(!loaded);
  if (loaded) {
    return;
  }
  if (!err.empty()) {
    TEST_MSG("%s", err.c_str());
  }
}

void usdc_memory_budget_stage_meta_customdata_success_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildStageMetaCustomDataUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Stage stage;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 64;
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err, options);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const StageMetas &metas = stage.metas();
  TEST_CHECK(metas.customLayerData.find("blob") != metas.customLayerData.end());
}

void usdc_memory_budget_composition_limit_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildCompositionUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 0;  // Tight budget to trigger failure
  options.strict_apiSchema_check = false;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err, options);
  TEST_CHECK(!loaded);
  if (loaded) {
    return;
  }
  if (!err.empty()) {
    TEST_MSG("%s", err.c_str());
  }
}

void usdc_memory_budget_composition_success_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildCompositionUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  options.max_memory_limit_in_mb = 64;
  options.strict_apiSchema_check = false;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err, options);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("ArcHeavy");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  const PrimMeta &meta = prim_it->second.metas();
  TEST_CHECK(meta.inherits.has_value());
  TEST_CHECK(meta.payload.has_value());
  TEST_CHECK(meta.specializes.has_value());
  TEST_CHECK(meta.has_apiSchemas());
}

void usdc_layer_variant_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildVariantSpecUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("VariantOwner");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  const PrimMeta &meta = prim_it->second.metas();
  TEST_CHECK(meta.variants.has_value());
  if (!meta.variants.has_value()) {
    return;
  }
  auto sel_it = meta.variants.value().find("shapeVariant");
  TEST_CHECK(sel_it != meta.variants.value().end());
  if (sel_it != meta.variants.value().end()) {
    TEST_CHECK(sel_it->second == "Capsule");
  }

  TEST_CHECK(meta.variantSets.has_value());
  if (meta.variantSets.has_value()) {
    bool has_shape = false;
    for (const auto &item : meta.variantSets.value()) {
      for (const auto &name : item.second) {
        if (name == "shapeVariant") {
          has_shape = true;
          break;
        }
      }
    }
    TEST_CHECK(has_shape);
  }

  const auto &variant_sets = prim_it->second.variantSets();
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  auto var_it = vs_it->second.variantSet.find("Capsule");
  TEST_CHECK(var_it != vs_it->second.variantSet.end());
  if (var_it != vs_it->second.variantSet.end()) {
    TEST_CHECK(var_it->second.typeName() == "Xform");
  }
}

void usdc_layer_variant_nested_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildVariantSpecNestedUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Layer layer;
  std::string warn;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("VariantOwner");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  const PrimMeta &meta = prim_it->second.metas();
  TEST_CHECK(meta.variants.has_value());
  TEST_CHECK(meta.variantSets.has_value());

  const auto &variant_sets = prim_it->second.variantSets();
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  auto var_it = vs_it->second.variantSet.find("Capsule");
  TEST_CHECK(var_it != vs_it->second.variantSet.end());
  if (var_it == vs_it->second.variantSet.end()) {
    return;
  }

  const auto &nested_sets = var_it->second.variantSets();
  auto nested_it = nested_sets.find("lod");
  TEST_CHECK(nested_it != nested_sets.end());
  if (nested_it == nested_sets.end()) {
    return;
  }

  auto lod_a_it = nested_it->second.variantSet.find("LodA");
  auto lod_b_it = nested_it->second.variantSet.find("LodB");
  TEST_CHECK(lod_a_it != nested_it->second.variantSet.end());
  TEST_CHECK(lod_b_it != nested_it->second.variantSet.end());
  if (lod_a_it != nested_it->second.variantSet.end()) {
    TEST_CHECK(lod_a_it->second.typeName() == "Xform");
  }
  if (lod_b_it != nested_it->second.variantSet.end()) {
    TEST_CHECK(lod_b_it->second.typeName() == "Xform");
  }
}

void usdc_stage_variant_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;
  if (!BuildStageVariantUsdcBuffer(&usdc_buf, &err)) {
    TEST_MSG("%s", err.c_str());
    TEST_CHECK(false);
    return;
  }

  Stage stage;
  std::string warn;
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  const Prim *prim = FindPrimAtPath(stage, "/VariantOwner");
  TEST_CHECK(prim != nullptr);
  if (!prim) {
    return;
  }

  const PrimMeta &meta = prim->metas();
  TEST_CHECK(meta.variants.has_value());
  if (!meta.variants.has_value()) {
    return;
  }

  auto sel_it = meta.variants.value().find("shapeVariant");
  TEST_CHECK(sel_it != meta.variants.value().end());
  if (sel_it != meta.variants.value().end()) {
    TEST_CHECK(sel_it->second == "Capsule");
  }

  const auto &variant_sets = prim->variantSets();
  auto vs_it = variant_sets.find("shapeVariant");
  TEST_CHECK(vs_it != variant_sets.end());
  if (vs_it == variant_sets.end()) {
    return;
  }

  auto var_it = vs_it->second.variantSet.find("Capsule");
  TEST_CHECK(var_it != vs_it->second.variantSet.end());
  if (var_it != vs_it->second.variantSet.end()) {
    TEST_CHECK(var_it->second.primChildren().size() == 1);
    if (!var_it->second.primChildren().empty()) {
      TEST_CHECK(var_it->second.primChildren()[0].element_name() == "Child");
    }

    const auto &nested_sets = var_it->second.variantSets();
    auto nested_it = nested_sets.find("lod");
    TEST_CHECK(nested_it != nested_sets.end());
    if (nested_it != nested_sets.end()) {
      auto lod_a_it = nested_it->second.variantSet.find("LodA");
      auto lod_b_it = nested_it->second.variantSet.find("LodB");
      TEST_CHECK(lod_a_it != nested_it->second.variantSet.end());
      TEST_CHECK(lod_b_it != nested_it->second.variantSet.end());
      if (lod_a_it != nested_it->second.variantSet.end()) {
        TEST_CHECK(lod_a_it->second.primChildren().size() == 1);
        if (!lod_a_it->second.primChildren().empty()) {
          TEST_CHECK(lod_a_it->second.primChildren()[0].element_name() == "LodAChild");
        }
      }
      if (lod_b_it != nested_it->second.variantSet.end()) {
        TEST_CHECK(lod_b_it->second.primChildren().size() == 1);
        if (!lod_b_it->second.primChildren().empty()) {
          TEST_CHECK(lod_b_it->second.primChildren()[0].element_name() == "LodBChild");
        }
      }
    }
  }
}

void usdc_layer_nested_variant_sets_test(void) {
  Layer layer;
  std::string err;
  bool loaded = LoadLayerFromUsdcFixture("variantSet-nested-001.usdc", &layer, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("Implicits");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  const PrimSpec &ps = prim_it->second;
  auto vs_it = ps.variantSets().find("shapeVariant");
  TEST_CHECK(vs_it != ps.variantSets().end());
  if (vs_it == ps.variantSets().end()) {
    return;
  }

  auto capsule_it = vs_it->second.variantSet.find("Capsule");
  TEST_CHECK(capsule_it != vs_it->second.variantSet.end());
  if (capsule_it == vs_it->second.variantSet.end()) {
    return;
  }

  const PrimSpec &capsule = capsule_it->second;
  auto nested_it = capsule.variantSets().find("geo");
  TEST_CHECK(nested_it != capsule.variantSets().end());
  if (nested_it == capsule.variantSets().end()) {
    return;
  }

  auto a_it = nested_it->second.variantSet.find("CapsuleA");
  auto b_it = nested_it->second.variantSet.find("CapsuleB");
  TEST_CHECK(a_it != nested_it->second.variantSet.end());
  TEST_CHECK(b_it != nested_it->second.variantSet.end());
  if (a_it == nested_it->second.variantSet.end() ||
      b_it == nested_it->second.variantSet.end()) {
    return;
  }

  TEST_CHECK(a_it->second.children().size() == 1);
  TEST_CHECK(b_it->second.children().size() == 1);
  if (a_it->second.children().size() == 1) {
    TEST_CHECK(a_it->second.children()[0].name() == "PillA");
  }
  if (b_it->second.children().size() == 1) {
    TEST_CHECK(b_it->second.children()[0].name() == "PillB");
  }
}

void usdc_layer_variant_name_collision_test(void) {
  Layer layer;
  std::string err;
  bool loaded = LoadLayerFromUsdcFixture("variantSet-collision-001.usdc", &layer, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_a_it = layer.primspecs().find("A");
  auto prim_b_it = layer.primspecs().find("B");
  TEST_CHECK(prim_a_it != layer.primspecs().end());
  TEST_CHECK(prim_b_it != layer.primspecs().end());
  if (prim_a_it == layer.primspecs().end() || prim_b_it == layer.primspecs().end()) {
    return;
  }

  auto get_capsule_variant = [](const PrimSpec &prim) -> const PrimSpec* {
    const auto &variant_sets = prim.variantSets();
    auto vs_it = variant_sets.find("shapeVariant");
    if (vs_it == variant_sets.end()) {
      return nullptr;
    }
    const VariantSetSpec &vs = vs_it->second;
    auto v_it = vs.variantSet.find("Capsule");
    if (v_it == vs.variantSet.end()) {
      return nullptr;
    }
    return &v_it->second;
  };

  const PrimSpec *capsule_a = get_capsule_variant(prim_a_it->second);
  const PrimSpec *capsule_b = get_capsule_variant(prim_b_it->second);
  TEST_CHECK(capsule_a != nullptr);
  TEST_CHECK(capsule_b != nullptr);
  if (!capsule_a || !capsule_b) {
    return;
  }

  TEST_CHECK(capsule_a->children().size() == 1);
  TEST_CHECK(capsule_b->children().size() == 1);
  if (capsule_a->children().size() != 1 || capsule_b->children().size() != 1) {
    return;
  }

  float tag_a = 0.0f;
  float tag_b = 0.0f;
  TEST_CHECK(GetPrimSpecFloatProp(capsule_a->children()[0], "tag", &tag_a));
  TEST_CHECK(GetPrimSpecFloatProp(capsule_b->children()[0], "tag", &tag_b));
  TEST_CHECK(std::fabs(tag_a - 1.0f) < 1e-6f);
  TEST_CHECK(std::fabs(tag_b - 3.0f) < 1e-6f);

  const auto &nested_a = capsule_a->variantSets();
  const auto &nested_b = capsule_b->variantSets();
  auto geo_a_it = nested_a.find("geo");
  auto geo_b_it = nested_b.find("geo");
  TEST_CHECK(geo_a_it != nested_a.end());
  TEST_CHECK(geo_b_it != nested_b.end());
  if (geo_a_it == nested_a.end() || geo_b_it == nested_b.end()) {
    return;
  }

  auto lod_a_it = geo_a_it->second.variantSet.find("LodA");
  auto lod_b_it = geo_b_it->second.variantSet.find("LodA");
  TEST_CHECK(lod_a_it != geo_a_it->second.variantSet.end());
  TEST_CHECK(lod_b_it != geo_b_it->second.variantSet.end());
  if (lod_a_it == geo_a_it->second.variantSet.end() ||
      lod_b_it == geo_b_it->second.variantSet.end()) {
    return;
  }

  TEST_CHECK(lod_a_it->second.children().size() == 1);
  TEST_CHECK(lod_b_it->second.children().size() == 1);
  if (lod_a_it->second.children().size() != 1 ||
      lod_b_it->second.children().size() != 1) {
    return;
  }

  TEST_CHECK(lod_a_it->second.children()[0].name() == "GeoA");
  TEST_CHECK(lod_b_it->second.children()[0].name() == "GeoA");
}

void usdc_layer_variant_selection_test(void) {
  Layer layer;
  std::string err;
  bool loaded = LoadLayerFromUsdcFixture("variantSet-prim-001.usdc", &layer, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    if (!err.empty()) {
      TEST_MSG("%s", err.c_str());
    }
    return;
  }

  auto prim_it = layer.primspecs().find("bora");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) {
    return;
  }

  std::string selection;
  TEST_CHECK(GetVariantSelection(prim_it->second.metas(), "shapeVariant", &selection));
  if (!selection.empty()) {
    TEST_CHECK(selection == "Capsule");
  }
  TEST_CHECK(HasVariantSetName(prim_it->second.metas(), "shapeVariant"));
}

// Roundtrip test: nested variants with properties on the inner variant.
// Exercises Layer write→read for nested VariantSetSpec where the inner
// variant carries both a property and a child PrimSpec.
void usdc_layer_nested_variant_props_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;

  // --- Build Layer with nested variants + properties ---
  {
    Layer layer;
    PrimSpec prim(Specifier::Def, "Xform", "Root");

    // Outer variant set "shape" with variant "Capsule"
    VariantSetSpec outer_vs;
    outer_vs.name = "shape";

    PrimSpec capsule_spec(Specifier::Def, "Xform", "CapsuleChild");

    // Inner variant set "lod" on the Capsule variant
    VariantSetSpec inner_vs;
    inner_vs.name = "lod";

    auto makeVariantWithProp = [](float val, const char *childType,
                                   const char *childName) {
      PrimSpec vs;
      vs.specifier() = Specifier::Def;
      Attribute attr;
      attr.set_value(float(val));
      attr.variability() = Variability::Varying;
      vs.props()["detail"] = Property(attr, /* custom */ false);
      vs.children().emplace_back(Specifier::Def, childType, childName);
      return vs;
    };

    inner_vs.variantSet["High"] = makeVariantWithProp(1.0f, "Mesh", "HighMesh");
    inner_vs.variantSet["Low"] = makeVariantWithProp(0.25f, "Mesh", "LowMesh");

    capsule_spec.variantSets()["lod"] = std::move(inner_vs);
    outer_vs.variantSet["Capsule"] = std::move(capsule_spec);
    prim.variantSets()["shape"] = std::move(outer_vs);

    if (!layer.emplace_primspec("Root", std::move(prim))) {
      TEST_MSG("Failed to add primspec.");
      TEST_CHECK(false);
      return;
    }
    if (!WriteLayerToUsdcMemory(layer, &usdc_buf, &err)) {
      TEST_MSG("Write failed: %s", err.c_str());
      TEST_CHECK(false);
      return;
    }
  }

  // --- Read back and verify ---
  Layer layer;
  std::string warn;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) {
    TEST_MSG("Load failed: %s", err.c_str());
    return;
  }

  auto prim_it = layer.primspecs().find("Root");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) return;

  // Outer variant set "shape"
  auto vs_it = prim_it->second.variantSets().find("shape");
  TEST_CHECK(vs_it != prim_it->second.variantSets().end());
  if (vs_it == prim_it->second.variantSets().end()) return;

  // Outer variant "Capsule"
  auto cap_it = vs_it->second.variantSet.find("Capsule");
  TEST_CHECK(cap_it != vs_it->second.variantSet.end());
  if (cap_it == vs_it->second.variantSet.end()) return;

  // Nested variant set "lod"
  auto nested_it = cap_it->second.variantSets().find("lod");
  TEST_CHECK(nested_it != cap_it->second.variantSets().end());
  if (nested_it == cap_it->second.variantSets().end()) return;

  // Check "High" variant
  auto high_it = nested_it->second.variantSet.find("High");
  TEST_CHECK(high_it != nested_it->second.variantSet.end());
  if (high_it != nested_it->second.variantSet.end()) {
    // Verify property on inner variant
    float detail = 0.0f;
    TEST_CHECK(GetPrimSpecFloatProp(high_it->second, "detail", &detail));
    TEST_CHECK(std::fabs(detail - 1.0f) < 1e-6f);

    // Verify child prim
    TEST_CHECK(high_it->second.children().size() == 1);
    if (!high_it->second.children().empty()) {
      TEST_CHECK(high_it->second.children()[0].name() == "HighMesh");
    }
  }

  // Check "Low" variant
  auto low_it = nested_it->second.variantSet.find("Low");
  TEST_CHECK(low_it != nested_it->second.variantSet.end());
  if (low_it != nested_it->second.variantSet.end()) {
    float detail = 0.0f;
    TEST_CHECK(GetPrimSpecFloatProp(low_it->second, "detail", &detail));
    TEST_CHECK(std::fabs(detail - 0.25f) < 1e-6f);

    TEST_CHECK(low_it->second.children().size() == 1);
    if (!low_it->second.children().empty()) {
      TEST_CHECK(low_it->second.children()[0].name() == "LowMesh");
    }
  }
}

// Layer roundtrip: multiple variant sets on a single prim
void usdc_layer_multiple_variant_sets_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;

  {
    Layer layer;
    PrimSpec prim(Specifier::Def, "Xform", "Car");
    PrimMeta &meta = prim.metas();
    VariantSelectionMap selections;
    selections["color"] = "red";
    selections["engine"] = "electric";
    meta.variants = selections;

    VariantSetSpec color_vs;
    color_vs.name = "color";
    PrimSpec red_spec(Specifier::Def, "", "RedBody");
    PrimSpec blue_spec(Specifier::Def, "", "BlueBody");
    color_vs.variantSet["red"] = std::move(red_spec);
    color_vs.variantSet["blue"] = std::move(blue_spec);

    VariantSetSpec engine_vs;
    engine_vs.name = "engine";
    PrimSpec elec_spec(Specifier::Def, "", "ElecDrive");
    PrimSpec gas_spec(Specifier::Def, "", "GasDrive");
    engine_vs.variantSet["electric"] = std::move(elec_spec);
    engine_vs.variantSet["gas"] = std::move(gas_spec);

    prim.variantSets()["color"] = std::move(color_vs);
    prim.variantSets()["engine"] = std::move(engine_vs);

    if (!layer.emplace_primspec("Car", std::move(prim))) {
      TEST_CHECK(false);
      return;
    }
    if (!WriteLayerToUsdcMemory(layer, &usdc_buf, &err)) {
      TEST_MSG("Write failed: %s", err.c_str());
      TEST_CHECK(false);
      return;
    }
  }

  Layer layer;
  std::string warn;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  auto prim_it = layer.primspecs().find("Car");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) return;

  // Both variant sets exist
  TEST_CHECK(prim_it->second.variantSets().count("color") > 0);
  TEST_CHECK(prim_it->second.variantSets().count("engine") > 0);

  // Variant selections preserved
  const auto &meta = prim_it->second.metas();
  TEST_CHECK(meta.variants.has_value());
  if (meta.variants.has_value()) {
    const auto &sel = meta.variants.value();
    auto c_it = sel.find("color");
    auto e_it = sel.find("engine");
    TEST_CHECK(c_it != sel.end());
    TEST_CHECK(e_it != sel.end());
    if (c_it != sel.end()) TEST_CHECK(c_it->second == "red");
    if (e_it != sel.end()) TEST_CHECK(e_it->second == "electric");
  }

  // Variant contents
  const auto &vsets = prim_it->second.variantSets();
  auto color_it = vsets.find("color");
  auto engine_it = vsets.find("engine");
  TEST_CHECK(color_it != vsets.end());
  TEST_CHECK(engine_it != vsets.end());
  if (color_it != vsets.end()) {
    TEST_CHECK(color_it->second.variantSet.count("red") > 0);
    TEST_CHECK(color_it->second.variantSet.count("blue") > 0);
  }
  if (engine_it != vsets.end()) {
    TEST_CHECK(engine_it->second.variantSet.count("electric") > 0);
    TEST_CHECK(engine_it->second.variantSet.count("gas") > 0);
  }
}

// Layer roundtrip: 3-level nested variant sets
void usdc_layer_3level_nested_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;

  {
    Layer layer;
    PrimSpec prim(Specifier::Def, "Xform", "Root");

    // L1 → L2 → L3
    VariantSetSpec l3;
    l3.name = "L3";
    PrimSpec l3_p(Specifier::Def, "Sphere", "DeepGeo");
    PrimSpec l3_q(Specifier::Def, "Cube", "DeepGeo");
    l3.variantSet["P"] = std::move(l3_p);
    l3.variantSet["Q"] = std::move(l3_q);

    PrimSpec l2_x;
    l2_x.specifier() = Specifier::Def;
    l2_x.variantSets()["L3"] = std::move(l3);
    VariantSetSpec l2;
    l2.name = "L2";
    l2.variantSet["X"] = std::move(l2_x);

    PrimSpec l1_a;
    l1_a.specifier() = Specifier::Def;
    l1_a.variantSets()["L2"] = std::move(l2);
    VariantSetSpec l1;
    l1.name = "L1";
    l1.variantSet["A"] = std::move(l1_a);

    prim.variantSets()["L1"] = std::move(l1);

    if (!layer.emplace_primspec("Root", std::move(prim))) {
      TEST_CHECK(false);
      return;
    }
    if (!WriteLayerToUsdcMemory(layer, &usdc_buf, &err)) {
      TEST_MSG("Write failed: %s", err.c_str());
      TEST_CHECK(false);
      return;
    }
  }

  Layer layer;
  std::string warn;
  bool loaded = LoadLayerFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &layer, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  auto prim_it = layer.primspecs().find("Root");
  TEST_CHECK(prim_it != layer.primspecs().end());
  if (prim_it == layer.primspecs().end()) return;

  // L1
  auto l1_it = prim_it->second.variantSets().find("L1");
  TEST_CHECK(l1_it != prim_it->second.variantSets().end());
  if (l1_it == prim_it->second.variantSets().end()) return;
  auto a_it = l1_it->second.variantSet.find("A");
  TEST_CHECK(a_it != l1_it->second.variantSet.end());
  if (a_it == l1_it->second.variantSet.end()) return;

  // L2
  auto l2_it = a_it->second.variantSets().find("L2");
  TEST_CHECK(l2_it != a_it->second.variantSets().end());
  if (l2_it == a_it->second.variantSets().end()) return;
  auto x_it = l2_it->second.variantSet.find("X");
  TEST_CHECK(x_it != l2_it->second.variantSet.end());
  if (x_it == l2_it->second.variantSet.end()) return;

  // L3
  auto l3_it = x_it->second.variantSets().find("L3");
  TEST_CHECK(l3_it != x_it->second.variantSets().end());
  if (l3_it == x_it->second.variantSets().end()) return;
  auto p_it = l3_it->second.variantSet.find("P");
  auto q_it = l3_it->second.variantSet.find("Q");
  TEST_CHECK(p_it != l3_it->second.variantSet.end());
  TEST_CHECK(q_it != l3_it->second.variantSet.end());
  if (p_it != l3_it->second.variantSet.end()) {
    TEST_CHECK(p_it->second.typeName() == "Sphere");
  }
  if (q_it != l3_it->second.variantSet.end()) {
    TEST_CHECK(q_it->second.typeName() == "Cube");
  }
}

// Stage roundtrip: variant with properties (not just prim children)
void usdc_stage_variant_props_roundtrip_test(void) {
  std::string err;
  std::vector<uint8_t> usdc_buf;

  {
    Stage stage;
    Xform root_xform;
    root_xform.name = "PropOwner";
    Prim root_prim(root_xform);
    root_prim.prim_type_name() = "Xform";

    PrimMeta &meta = root_prim.metas();
    meta.variantSets = std::vector<std::pair<ListEditQual, std::vector<std::string>>>();
    meta.variantSets->push_back(
        std::make_pair(ListEditQual::Prepend, std::vector<std::string>{"quality"}));

    auto makeVariantWithDoubleProp = [](const char *name, double val) {
      Variant v;
      Attribute attr;
      attr.set_value(double(val));
      v.properties()[name] = Property(attr, /* custom */ false);
      return v;
    };

    VariantSet vs;
    vs.name = "quality";
    vs.variantSet["high"] = makeVariantWithDoubleProp("detail", 4.0);
    vs.variantSet["low"] = makeVariantWithDoubleProp("detail", 1.0);
    root_prim.variantSets()["quality"] = std::move(vs);

    if (!stage.add_root_prim(std::move(root_prim))) {
      TEST_CHECK(false);
      return;
    }
    stage.commit();
    if (!WriteStageToUsdcMemory(stage, &usdc_buf, &err)) {
      TEST_MSG("Write failed: %s", err.c_str());
      TEST_CHECK(false);
      return;
    }
  }

  Stage stage;
  std::string warn;
  bool loaded = LoadUSDCFromMemory(usdc_buf.data(), usdc_buf.size(),
      "test.usdc", &stage, &warn, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/PropOwner");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;

  auto vs_it = prim->variantSets().find("quality");
  TEST_CHECK(vs_it != prim->variantSets().end());
  if (vs_it == prim->variantSets().end()) return;

  auto high_it = vs_it->second.variantSet.find("high");
  auto low_it = vs_it->second.variantSet.find("low");
  TEST_CHECK(high_it != vs_it->second.variantSet.end());
  TEST_CHECK(low_it != vs_it->second.variantSet.end());
  if (high_it != vs_it->second.variantSet.end()) {
    TEST_CHECK(high_it->second.properties().count("detail") > 0);
  }
  if (low_it != vs_it->second.variantSet.end()) {
    TEST_CHECK(low_it->second.properties().count("detail") > 0);
  }
}

// ===========================================================================
// Golden USDC schema reconstruction.
//
// The .usdc fixtures under tests/usdc/schema-*.usdc were authored by Pixar
// OpenUSD `usdcat` (ground truth), NOT by lightusd. These verify that the
// LightUSD crate reader reconstructs typed schema fields from real Pixar crate
// files. `.authored()` on a typed field is the key signal that a property was
// mapped into the schema struct rather than dropped.
// ===========================================================================

void usdc_golden_geom_mesh_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-mesh-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/mesh");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomMesh *mesh = prim->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  TEST_CHECK(mesh->get_points().size() == 4);
  TEST_CHECK(mesh->get_faceVertexCounts().size() == 1);
  TEST_CHECK(mesh->get_faceVertexIndices().size() == 4);
  TEST_CHECK(mesh->normals.authored());
  // Regression: GeomMesh velocities must reconstruct from a Pixar crate file.
  TEST_CHECK(mesh->velocities.authored());
}

void usdc_golden_geom_subset_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-geomsubset-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/mesh/subset");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomSubset *subset = prim->as<GeomSubset>();
  TEST_CHECK(subset != nullptr);
  if (!subset) return;

  TEST_CHECK(subset->elementType.get_value() == GeomSubset::ElementType::Face);
  lightusd::legacy::value::token fam;
  TEST_CHECK(subset->familyName.get_value(&fam));
  TEST_CHECK(fam.str() == "materialBind");
  TEST_CHECK(subset->indices.authored());
}

void usdc_golden_geom_camera_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-camera-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/cam");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomCamera *cam = prim->as<GeomCamera>();
  TEST_CHECK(cam != nullptr);
  if (!cam) return;

  auto chk = [](const TypedAttributeWithFallback<Animatable<float>> &a,
                float expect, const char *nm) {
    TEST_CHECK_(a.authored(), "camera.%s not authored", nm);
    float v = 0.0f;
    if (a.authored() && a.get_value().get_scalar(&v)) {
      TEST_CHECK_(std::abs(v - expect) < 1e-3f, "camera.%s = %f, want %f", nm, v, expect);
    }
  };
  chk(cam->focalLength, 35.0f, "focalLength");
  chk(cam->focusDistance, 10.0f, "focusDistance");
  chk(cam->fStop, 2.8f, "fStop");
  chk(cam->horizontalAperture, 36.0f, "horizontalAperture");
  TEST_CHECK(cam->clippingRange.authored());
}

void usdc_golden_light_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-light-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  auto sf = [](const TypedAttributeWithFallback<Animatable<float>> &a,
               float expect, const char *nm) {
    TEST_CHECK_(a.authored(), "%s dropped on read", nm);
    float v = 0.0f;
    if (a.authored() && a.get_value().get_scalar(&v)) {
      TEST_CHECK_(std::abs(v - expect) < 1e-3f, "%s = %f, want %f", nm, v, expect);
    }
  };

  // SphereLight: full base radiometric set + radius + light:filters.
  {
    const Prim *prim = FindPrimAtPath(stage, "/light");
    TEST_CHECK(prim != nullptr);
    if (prim) {
      const SphereLight *L = prim->as<SphereLight>();
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
    const Prim *prim = FindPrimAtPath(stage, "/clight");
    TEST_CHECK(prim != nullptr);
    if (prim) {
      const CylinderLight *L = prim->as<CylinderLight>();
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

void usdc_golden_skeleton_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-skeleton-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/skel");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const Skeleton *skel = prim->as<Skeleton>();
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

void usdc_golden_skelanim_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-skelanim-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/anim");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const SkelAnimation *anim = prim->as<SkelAnimation>();
  TEST_CHECK(anim != nullptr);
  if (!anim) return;

  TEST_CHECK(anim->joints.authored());
  TEST_CHECK(anim->translations.authored());
  TEST_CHECK(anim->rotations.authored());
  TEST_CHECK(anim->scales.authored());
}

void usdc_golden_blendshape_schema_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-blendshape-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/blend");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const BlendShape *bs = prim->as<BlendShape>();
  TEST_CHECK(bs != nullptr);
  if (!bs) return;

  TEST_CHECK(bs->offsets.authored());
  TEST_CHECK(bs->normalOffsets.authored());
  TEST_CHECK(bs->pointIndices.authored());
}

void usdc_golden_physics_scene_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-scene-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/scene");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const PhysicsScene *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;

  TEST_CHECK(scene->gravityDirection.authored());
  TEST_CHECK(scene->gravityMagnitude.authored());
  if (auto v = scene->gravityMagnitude.get_value()) {
    TEST_CHECK(std::abs(*v - 9.81f) < 1e-3f);
  }
}

void usdc_golden_physics_joint_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-joint-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/joint");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const PhysicsRevoluteJoint *joint = prim->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  TEST_CHECK(joint->body0.authored());
  TEST_CHECK(joint->body1.authored());
  TEST_CHECK(joint->localPos0.authored());
  TEST_CHECK(joint->axis.authored());
  lightusd::legacy::value::token ax;
  if (joint->axis.get_value(&ax)) TEST_CHECK(ax.str() == "X");
  TEST_CHECK(joint->lowerLimit.authored());
  if (auto v = joint->lowerLimit.get_value()) TEST_CHECK(std::abs(*v - (-45.0f)) < 1e-3f);
  TEST_CHECK(joint->upperLimit.authored());
}

void usdc_golden_physics_collisiongroup_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-collisiongroup-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/cg");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const PhysicsCollisionGroup *cg = prim->as<PhysicsCollisionGroup>();
  TEST_CHECK(cg != nullptr);
  if (!cg) return;

  TEST_CHECK(cg->mergeGroup.authored());
  lightusd::legacy::value::token mg;
  if (cg->mergeGroup.get_value(&mg)) TEST_CHECK(mg.str() == "groupA");
  TEST_CHECK(cg->invertFilteredGroups.get_value() == true);
  TEST_CHECK(cg->filteredGroups.authored());
  TEST_CHECK(cg->filteredGroups.get_targetPaths().size() == 1);
}

void usdc_golden_physics_apis_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-apis-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/body");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;

  PhysicsRigidBodyAPI rb;
  TEST_CHECK(GetPhysicsRigidBodyAPI(*prim, &rb));
  TEST_CHECK(rb.mass.authored());
  if (auto v = rb.mass.get_value()) TEST_CHECK(std::abs(*v - 2.5f) < 1e-3f);
  TEST_CHECK(rb.velocity.authored());
  TEST_CHECK(rb.centerOfMass.authored());

  PhysicsMassAPI mass;
  TEST_CHECK(GetPhysicsMassAPI(*prim, &mass));
  TEST_CHECK(mass.diagonalInertia.authored());

  PhysicsCollisionAPI col;
  TEST_CHECK(GetPhysicsCollisionAPI(*prim, &col));
  TEST_CHECK(col.collisionEnabled.get_value() == false);
  TEST_CHECK(col.simulationOwner.authored());
  TEST_CHECK(col.simulationOwner.get_targetPaths().size() == 1);
}

void usdc_golden_physics_material_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-material-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/mat");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;

  PhysicsMaterialAPI mat;
  TEST_CHECK(GetPhysicsMaterialAPI(*prim, &mat));
  TEST_CHECK(mat.staticFriction.authored());
  if (auto v = mat.staticFriction.get_value()) TEST_CHECK(std::abs(*v - 0.5f) < 1e-3f);
  TEST_CHECK(mat.dynamicFriction.authored());
  TEST_CHECK(mat.restitution.authored());
  TEST_CHECK(mat.density.authored());
}

void usdc_golden_collection_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-collection-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/mesh");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomMesh *mesh = prim->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;

  const CollectionInstance *coll = nullptr;
  TEST_CHECK(mesh->get_instance("foo", &coll));
  if (coll) {
    TEST_CHECK(coll->expansionRule.get_value() ==
               CollectionInstance::ExpansionRule::ExpandPrimsAndProperties);
    TEST_CHECK(coll->includes.authored());
    TEST_CHECK(coll->includes.get_targetPaths().size() == 1);
    TEST_CHECK(coll->excludes.authored());
  }
}

void usdc_golden_lightfilter_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-lightfilter-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  {
    const Prim *prim = FindPrimAtPath(stage, "/filter");
    TEST_CHECK(prim != nullptr);
    if (prim) {
      const LightFilter *lf = prim->as<LightFilter>();
      TEST_CHECK(lf != nullptr);
      if (lf) TEST_CHECK(lf->visibility.authored());
    }
  }
  {
    const Prim *prim = FindPrimAtPath(stage, "/plug");
    TEST_CHECK(prim != nullptr);
    if (prim) {
      const PluginLightFilter *plf = prim->as<PluginLightFilter>();
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

void usdc_golden_geom_intrinsics_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-geom-intrinsics-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  auto chk_d = [&](const char *path, auto getter) {
    const Prim *prim = FindPrimAtPath(stage, path);
    TEST_CHECK_(prim != nullptr, "missing %s", path);
    if (prim) getter(prim);
  };
  chk_d("/cube", [](const Prim *p) {
    const GeomCube *c = p->as<GeomCube>(); TEST_CHECK(c && c->size.authored()); });
  chk_d("/sphere", [](const Prim *p) {
    const GeomSphere *c = p->as<GeomSphere>(); TEST_CHECK(c && c->radius.authored()); });
  chk_d("/cone", [](const Prim *p) {
    const GeomCone *c = p->as<GeomCone>();
    TEST_CHECK(c && c->height.authored() && c->radius.authored());
    if (c) TEST_CHECK(c->axis.get_value() == Axis::X); });
  chk_d("/cyl", [](const Prim *p) {
    const GeomCylinder *c = p->as<GeomCylinder>();
    TEST_CHECK(c && c->height.authored());
    if (c) TEST_CHECK(c->axis.get_value() == Axis::Y); });
  chk_d("/cap", [](const Prim *p) {
    const GeomCapsule *c = p->as<GeomCapsule>(); TEST_CHECK(c && c->radius.authored()); });
  chk_d("/plane", [](const Prim *p) {
    const GeomPlane *c = p->as<GeomPlane>();
    TEST_CHECK(c && c->width.authored() && c->length.authored()); });
}

void usdc_golden_geom_tetmesh_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-geom-tetmesh-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/tet");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomTetMesh *tet = prim->as<GeomTetMesh>();
  TEST_CHECK(tet != nullptr);
  if (!tet) return;
  TEST_CHECK(tet->points.authored());
  TEST_CHECK(tet->tetVertexIndices.authored());
  TEST_CHECK(tet->surfaceFaceVertexIndices.authored());
}

void usdc_golden_geom_nurbspatch_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-geom-nurbspatch-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/patch");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomNurbsPatch *patch = prim->as<GeomNurbsPatch>();
  TEST_CHECK(patch != nullptr);
  if (!patch) return;
  TEST_CHECK(patch->uVertexCount.authored());
  TEST_CHECK(patch->vVertexCount.authored());
  TEST_CHECK(patch->uKnots.authored());
  TEST_CHECK(patch->points.authored());
}

void usdc_golden_geom_hermite_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-geom-hermite-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/herm");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomHermiteCurves *herm = prim->as<GeomHermiteCurves>();
  TEST_CHECK(herm != nullptr);
  if (!herm) return;
  TEST_CHECK(herm->curveVertexCounts.authored());
  TEST_CHECK(herm->points.authored());
  TEST_CHECK(herm->tangents.authored());
}

void usdc_golden_lights_extra_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-lights-extra-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  {
    const Prim *p = FindPrimAtPath(stage, "/glight");
    TEST_CHECK(p != nullptr);
    if (p) { const GeometryLight *l = p->as<GeometryLight>();
      TEST_CHECK(l && l->intensity.authored()); }
  }
  {
    const Prim *p = FindPrimAtPath(stage, "/portal");
    TEST_CHECK(p != nullptr);
    if (p) { const PortalLight *l = p->as<PortalLight>();
      TEST_CHECK(l && l->intensity.authored()); }
  }
  {
    const Prim *p = FindPrimAtPath(stage, "/dome");
    TEST_CHECK(p != nullptr);
    if (p) { const DomeLight_1 *l = p->as<DomeLight_1>();
      TEST_CHECK(l != nullptr);
      if (l) {
        TEST_CHECK(l->intensity.authored());
        TEST_CHECK(l->guideRadius.authored());
        TEST_CHECK(l->poleAxis.get_value().str() == "Y");
      } }
  }
}

void usdc_golden_matbind_collection_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-matbind-collection-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/mesh");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const GeomMesh *mesh = prim->as<GeomMesh>();
  TEST_CHECK(mesh != nullptr);
  if (!mesh) return;
  TEST_CHECK(mesh->materialBindingCollectionMap().count("metalBits") == 1);
  // The collection instance itself round-trips too.
  const CollectionInstance *coll = nullptr;
  TEST_CHECK(mesh->get_instance("metalBits", &coll));
}

void usdc_golden_light_linking_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-light-linking-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/light");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  const SphereLight *light = prim->as<SphereLight>();
  TEST_CHECK(light != nullptr);
  if (!light) return;

  const CollectionInstance *ll = nullptr;
  TEST_CHECK(light->get_instance("lightLink", &ll));
  if (ll) TEST_CHECK(ll->includes.get_targetPaths().size() == 1);
  const CollectionInstance *sl = nullptr;
  TEST_CHECK(light->get_instance("shadowLink", &sl));
  if (sl) TEST_CHECK(sl->excludes.get_targetPaths().size() == 1);
}

void usdc_golden_physics_colliders_test(void) {
  Stage stage;
  std::string err;
  bool loaded = LoadStageFromUsdcFixture("schema-physics-colliders-001.usdc", &stage, &err);
  TEST_CHECK(loaded);
  if (!loaded) { TEST_MSG("%s", err.c_str()); return; }

  const Prim *prim = FindPrimAtPath(stage, "/cg");
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  TEST_CHECK(prim->as<PhysicsCollisionGroup>() != nullptr);

  std::vector<Path> includes, excludes;
  TEST_CHECK(GetPhysicsCollidersCollection(*prim, &includes, &excludes));
  TEST_CHECK(includes.size() == 2);
  TEST_CHECK(excludes.size() == 1);
}
