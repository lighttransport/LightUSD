// SPDX-License-Identifier: Apache-2.0

#include "next/composition/composition.hh"
#include "next/eval/value-clip.hh"
#include "next/layer/listop-field-table.hh"
#include "next/prim/identifier.hh"
#include "next/eval/attribute-eval.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/types/spline.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include "next/schema/schema-registry.hh"

using namespace lightusd::next;

namespace {

LoadResult Parse(const std::string& body, bool strict = false) {
  LoadOptions options;
  options.parse_options.strict_aousd_conformance = strict;
  return LoadUSDAFromString("#usda 1.0\n" + body, options);
}

std::unique_ptr<Layer> TakeLayer(LoadResult* result) {
  assert(result && result->success);
  return result->stage.ReleaseRootLayer();
}

std::string UsdaFixturePath(const std::string& filename) {
  const std::string file_path(__FILE__);
  const std::string marker = "/tests/next/";
  const size_t pos = file_path.rfind(marker);
  assert(pos != std::string::npos);
  return file_path.substr(0, pos) + "/tests/usda/" + filename;
}

std::string NextFixturePath(const std::string& filename) {
  const std::string file_path(__FILE__);
  const size_t slash = file_path.rfind('/');
  assert(slash != std::string::npos);
  return file_path.substr(0, slash) + "/fixtures/" + filename;
}

std::vector<uint8_t> ReadHexFixture(const std::string& filename) {
  std::ifstream ifs(NextFixturePath(filename));
  assert(ifs);
  std::vector<uint8_t> bytes;
  char hi = 0;
  char lo = 0;
  auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    assert(c >= 'A' && c <= 'F');
    return static_cast<uint8_t>(c - 'A' + 10);
  };
  while (ifs >> hi) {
    assert(ifs >> lo);
    bytes.push_back(static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo)));
  }
  return bytes;
}

void TestUnicodeAndPaths() {
  LoadResult unicode = Parse("def Xform \"München\" {}\n", true);
  assert(unicode.success);
  assert(unicode.stage.GetPrimAtPath("/München").IsValid());

  const std::string invalid =
      "def Xform \"Root\" { rel r = </Root//Child> }\n";
  assert(Parse(invalid, false).success);
  LoadResult strict = Parse(invalid, true);
  assert(!strict.success);
  assert(strict.error_summary.find("Invalid AOUSD path") != std::string::npos);

  // Table-driven boundaries from AOUSD Core §8.3. In particular, absolute
  // paths use PrimFirstPathElements (at most one property element), while
  // relative paths permit relational-property chains.
  const char* valid_paths[] = {
      "/", ".", "..", "../..", "../../Sibling", "../.points",
      "Descendant", ".property", ".relationship.attribute",
      "/City{ selection = NewYork }",
      "/City/Street{selection=5thAvenue}",
      "/City/Street{selection=}",
  };
  for (const char* path : valid_paths) assert(IsValidPathString(path));
  const char* invalid_paths[] = {
      "", "./Descendant/Prim", "/New York/New York", "/abc/123",
      "/City/Street{=}", "/abc{123=}", "/Root/", "/Root//Child",
      "/Prim/namespace:", "/Prim.:property", "/Prim.namespace::property",
      "/Prim.abc:123", "/Prim.relationship.attribute",
  };
  for (const char* path : invalid_paths) assert(!IsValidPathString(path));

  // UTF-8 / Unicode boundary cases per grammar production: malformed byte
  // sequences (truncated, lone continuation, overlong, surrogate range,
  // beyond U+10FFFF) must be rejected; XID identifiers reject noncharacters
  // and private-use codepoints, and accept valid non-ASCII XID letters.
  {
    const std::string truncated = std::string("Pr") + char(0xC3);
    const std::string lone_continuation = std::string("P") + char(0x80) + "x";
    const std::string overlong = std::string("P") + char(0xC0) + char(0xAF);
    const std::string surrogate =            // U+D800 (CESU-8 encoding)
        std::string("P") + char(0xED) + char(0xA0) + char(0x80);
    const std::string beyond_max =           // U+110000
        std::string("P") + char(0xF4) + char(0x90) + char(0x80) + char(0x80);
    const std::string noncharacter =         // U+FDD0
        std::string("P") + char(0xEF) + char(0xB7) + char(0x90);
    const std::string reversed_bom =         // U+FFFE
        std::string("P") + char(0xEF) + char(0xBF) + char(0xBE);
    const std::string private_use =          // U+E000
        std::string("P") + char(0xEE) + char(0x80) + char(0x80);
    const std::string invalid_names[] = {
        truncated, lone_continuation, overlong, surrogate, beyond_max,
        noncharacter, reversed_bom, private_use,
    };
    for (const std::string& name : invalid_names) {
      assert(!IsValidIdentifier(name));
      assert(!IsValidNamespacedIdentifier("ns:" + name));
      assert(!IsValidPathString("/" + name));
      // Authoring boundaries reject the same inputs.
      assert(Path("/World").append_child(name).empty());
      assert(Path("/World").append_property(name).empty());
      Layer layer;
      assert(layer.define_prim_at_path("/" + name) == UINT32_MAX);
      assert(layer.define_prim_at_path("/Ok/" + name + "/Child") ==
             UINT32_MAX);
    }
    const char* valid_names[] = {
        "M\xC3\xBCnchen",              // U+00FC, XID letter
        "\xE6\x9D\xB1\xE4\xBA\xAC",    // 東京
        "_1", "a\xCC\x81",             // combining acute (XID_Continue)
    };
    for (const char* name : valid_names) {
      assert(IsValidIdentifier(name));
      assert(!Path("/World").append_child(name).empty());
      Layer layer;
      assert(layer.define_prim_at_path(std::string("/") + name) !=
             UINT32_MAX);
    }
    // Digits are XID_Continue but not XID_Start.
    assert(!IsValidIdentifier("1abc"));
    assert(IsValidIdentifier("abc1"));
  }

  // Path::is_valid / Path::Parse mirror the string validator at the Path
  // authoring boundary.
  assert(Path("/World/Cube").is_valid());
  assert(!Path("/Root//Child").is_valid());
  assert(!Path("").is_valid());
  assert(Path::Parse("/World/Cube").str() == "/World/Cube");
  assert(Path::Parse("/Root//Child").empty());
  // Namespaced properties append; a raw '.' in a property name is rejected.
  assert(Path("/P").append_property("xformOp:translate").str() ==
         "/P.xformOp:translate");
  assert(Path("/P").append_property("bad.name").empty());
}

void TestLosslessUnsupportedValues() {
  // Typed splines are now first-class: they parse (even in strict mode),
  // evaluate, and encode to USDC (Crate type 59).
  const std::string spline =
      "def Xform \"S\" {\n"
      "  double value.spline = { 0: 0; post linear, 10: 10, }\n"
      "}\n";
  LoadResult compat = Parse(spline, false);
  assert(compat.success);
  const std::string rewritten = WriteUSDAToString(compat.stage);
  assert(rewritten.find("value.spline") != std::string::npos);
  assert(rewritten.find("post linear") != std::string::npos);
  // Strict AOUSD parse now accepts the (grammatically valid) spline.
  assert(Parse(spline, true).success);
  // A grammatically malformed spline is a hard error in strict mode.
  assert(!Parse(
              "def Xform \"B\" {\n"
              "  double value.spline = { 0: 0; post bogus, }\n"
              "}\n",
              true)
              .success);

  // frame4d is now a first-class matrix4d role type (pxr itself rejects
  // the old `= identity` shorthand): the matrix form parses, round-trips,
  // and keeps the frame4d name.
  const std::string frame =
      "def Xform \"F\" {\n"
      "    frame4d frame = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,2,1))\n"
      "}\n";
  LoadResult frame_compat = Parse(frame, false);
  assert(frame_compat.success);
  {
    UsdPrim f = frame_compat.stage.GetPrimAtPath("/F");
    const Value* v = f.GetPropertyValue("frame");
    assert(v && v->type_id() == TypeId::Frame4d);
    const double* m = v->as_matrix4d();
    assert(m && m[14] == 2.0);
    const std::string rew = WriteUSDAToString(frame_compat.stage);
    assert(rew.find("frame4d frame") != std::string::npos);
  }
  // pathExpression is first-class too.
  const std::string pe =
      "def Xform \"P\" {\n"
      "    pathExpression expr = \"/World//Mesh*\"\n"
      "}\n";
  LoadResult pe_res = Parse(pe, false);
  assert(pe_res.success);
  {
    UsdPrim pprim = pe_res.stage.GetPrimAtPath("/P");
    const Value* v = pprim.GetPropertyValue("expr");
    assert(v && v->type_id() == TypeId::PathExpression);
    assert(v->as_string() && *v->as_string() == "/World//Mesh*");
    const std::string rew = WriteUSDAToString(pe_res.stage);
    assert(rew.find("pathExpression expr = \"/World//Mesh*\"") !=
           std::string::npos);
  }

  // `opaque` and its `group` semantic alias can only agree with a ValueBlock.
  // Strict mode must accept that normative form and preserve the declaration.
  LoadResult opaque = Parse(
      "def Scope \"O\" {\n"
      "    opaque marker = None\n"
      "    group bundle = None\n"
      "}\n",
      true);
  assert(opaque.success);
  const std::string opaque_usda = WriteUSDAToString(opaque.stage);
  assert(opaque_usda.find("opaque marker = None") != std::string::npos);
  assert(opaque_usda.find("group bundle = None") != std::string::npos);
  std::vector<uint8_t> opaque_crate;
  USDCWriteOptions opaque_write;
  opaque_write.crate_options.strict_aousd_conformance = true;
  assert(WriteUSDCToMemory(opaque_crate, opaque.stage, opaque_write).success);
  USDCLoadResult opaque_back =
      LoadUSDCFromMemory(opaque_crate.data(), opaque_crate.size());
  assert(opaque_back.success);
  const std::string opaque_crate_usda = WriteUSDAToString(opaque_back.stage);
  assert(opaque_crate_usda.find("opaque marker = None") != std::string::npos);
  assert(opaque_crate_usda.find("group bundle = None") != std::string::npos);
}

void TestSchemaRegistryBreadth() {
  // Fallbacks expanded from pxr's generated schemas: Imageable purpose /
  // visibility, camera clippingRange, common light inputs, curve basis,
  // GeomSubset elementType, ShadowAPI defaults.
  const std::string usda =
      "def Mesh \"M\" { }\n"
      "def Camera \"C\" { }\n"
      "def SphereLight \"L\" { }\n"
      "def BasisCurves \"B\" { }\n"
      "def PointInstancer \"I\" { }\n"
      "def Shader \"S\" { }\n"
      "def Mesh \"Skinned\" (prepend apiSchemas = [\"SkelBindingAPI\"]) {}\n";
  LoadResult r = Parse("#usda 1.0\n" + usda, false);
  assert(r.success);
  auto expect_tok = [&](const char* prim, const char* attr,
                        const char* want) {
    UsdPrim p = r.stage.GetPrimAtPath(prim);
    assert(p.IsValid());
    const SchemaPropertyDefinition* def =
        GetSchemaRegistry().FindProperty(*p.GetPrimSpec(), attr);
    assert(def && def->has_fallback);
    const std::string* t = def->fallback.as_token();
    assert(t && *t == want);
  };
  expect_tok("/M", "purpose", "default");
  expect_tok("/M", "visibility", "inherited");
  expect_tok("/B", "basis", "bezier");
  {
    UsdPrim c = r.stage.GetPrimAtPath("/C");
    const SchemaPropertyDefinition* def =
        GetSchemaRegistry().FindProperty(*c.GetPrimSpec(), "clippingRange");
    assert(def && def->has_fallback);
  }
  const SchemaRegistry& registry = GetSchemaRegistry();
  assert(registry.SchemaTypes().size() >= 30);
  assert(registry.IsKnownSchema("SkelBindingAPI") &&
         registry.IsKnownSchema("PhysicsMaterialAPI"));
  {
    const UsdPrim instancer = r.stage.GetPrimAtPath("/I");
    const SchemaPropertyDefinition* def = registry.FindProperty(
        *instancer.GetPrimSpec(), "protoIndices");
    assert(def && !def->has_fallback && def->type_name == "int[]");
  }
  {
    const UsdPrim shader = r.stage.GetPrimAtPath("/S");
    const SchemaPropertyDefinition* def = registry.FindProperty(
        *shader.GetPrimSpec(), "info:implementationSource");
    assert(def && def->has_fallback && def->fallback.as_token() &&
           *def->fallback.as_token() == "id");
  }
  {
    const UsdPrim skinned = r.stage.GetPrimAtPath("/Skinned");
    assert(registry.FindProperty(*skinned.GetPrimSpec(), "skel:joints"));
  }
  {
    UsdPrim l = r.stage.GetPrimAtPath("/L");
    const SchemaPropertyDefinition* def = GetSchemaRegistry().FindProperty(
        *l.GetPrimSpec(), "inputs:intensity");
    assert(def && def->fallback.as_float() && *def->fallback.as_float() == 1.0f);
    def = GetSchemaRegistry().FindProperty(*l.GetPrimSpec(), "inputs:radius");
    assert(def && def->fallback.as_float() && *def->fallback.as_float() == 0.5f);
  }

  std::cout << "  Schema registry breadth: PASSED\n";
}

void TestDictionaryAndRelationshipComposition() {
  LoadResult weak_result = Parse(
      "def \"Root\" (\n"
      "  customData = {\n"
      "    dictionary nested = { int a = 1 int b = 2 }\n"
      "    int weak = 3\n"
      "  }\n"
      ") { rel r = </A> }\n");
  LoadResult strong_result = Parse(
      "( subLayers = [@weak.usda@] )\n"
      "over \"Root\" (\n"
      "  customData = {\n"
      "    dictionary nested = { int a = 9 }\n"
      "    int strong = 4\n"
      "  }\n"
      ") { prepend rel r = </B> }\n");
  std::unique_ptr<Layer> weak = TakeLayer(&weak_result);
  std::unique_ptr<Layer> strong = TakeLayer(&strong_result);

  Compositor compositor;
  compositor.SetLayerLoader([&](const std::string& path, std::string*) {
    if (path.find("weak.usda") == std::string::npos)
      return std::unique_ptr<Layer>();
    return std::make_unique<Layer>(weak->Clone());
  });
  std::unique_ptr<Layer> composed = compositor.Compose(*strong, "root.usda");
  assert(composed);
  const PrimSpec* root = composed->prim_at_path("/Root");
  assert(root);
  const Dict* custom = root->meta().customData().as_dictionary();
  assert(custom && custom->find("weak") && custom->find("strong"));
  const Value* nested_value = custom->find("nested");
  const Dict* nested = nested_value ? nested_value->as_dictionary() : nullptr;
  assert(nested && nested->find("a") && nested->find("b"));
  assert(nested->find("a")->as_int() && *nested->find("a")->as_int() == 9);
  const std::vector<Path>* targets = root->relationship("r");
  assert(targets && targets->size() == 2);
  assert((*targets)[0].str() == "/B" && (*targets)[1].str() == "/A");

  // Three-site stack: weak explicit A, middle append C, strong prepend B.
  PrimSpec weak_rel("R"), middle_rel("R"), strong_rel("R"), merged("R");
  weak_rel.set_relationship_targets("r", {Path("/A")});
  middle_rel.set_relationship_targets("r", {Path("/C")});
  ArcEdit& middle_edit = middle_rel.ensure_relationship_edit("r");
  middle_edit.authored = true;
  middle_edit.is_explicit = false;
  middle_edit.appended = {"/C"};
  strong_rel.set_relationship_targets("r", {Path("/B")});
  ArcEdit& strong_edit = strong_rel.ensure_relationship_edit("r");
  strong_edit.authored = true;
  strong_edit.is_explicit = false;
  strong_edit.prepended = {"/B"};
  Compositor::CopyLocalOpinions(merged, strong_rel);
  Compositor::CopyLocalOpinions(merged, middle_rel);
  Compositor::CopyLocalOpinions(merged, weak_rel);
  const std::vector<Path>* stacked = merged.relationship("r");
  assert(stacked && stacked->size() == 3);
  assert((*stacked)[0].str() == "/B" && (*stacked)[1].str() == "/A" &&
         (*stacked)[2].str() == "/C");

  // Property metadata resolves field-by-field. This matrix covers fields that
  // the former hand-picked copier omitted, recursive dictionaries, unknown raw
  // metadata, and typed extension dictionaries.
  PrimSpec weak_prop("P"), strong_prop("P"), composed_prop("P");
  weak_prop.add_property("x", Value(1.0f));
  strong_prop.add_property("x", Value(2.0f));
  weak_prop.meta().unknownMeta().push_back({"weakPrimMeta", "2"});
  strong_prop.meta().unknownMeta().push_back({"strongPrimMeta", "1"});
  PropMeta& weak_meta = weak_prop.ensure_property_meta("x");
  weak_meta.renderType = "color";
  weak_meta.connectability = "interfaceOnly";
  weak_meta.allowedTokens = {"a", "b"};
  weak_meta.authored |= PropMeta::kRenderType | PropMeta::kConnectability |
                        PropMeta::kAllowedTokens | PropMeta::kCustomData |
                        PropMeta::kAssetInfo | PropMeta::kUnknownMeta;
  weak_meta.unknownMeta.push_back({"weakMeta", "17"});
  weak_meta.customData = Value::MakeDictionary();
  Value weak_nested = Value::MakeDictionary();
  weak_nested.as_dictionary()->set("b", Value(int32_t(2)));
  weak_meta.customData.as_dictionary()->set("nested", weak_nested);
  weak_meta.assetInfo = Value::MakeDictionary();
  weak_meta.assetInfo.as_dictionary()->set("weak", Value(true));

  PropMeta& strong_meta = strong_prop.ensure_property_meta("x");
  strong_meta.displayName = "Strong";
  strong_meta.authored |= PropMeta::kDisplayName | PropMeta::kCustomData |
                          PropMeta::kUnknownMeta;
  strong_meta.unknownMeta.push_back({"strongMeta", "\"yes\""});
  strong_meta.customData = Value::MakeDictionary();
  Value strong_nested = Value::MakeDictionary();
  strong_nested.as_dictionary()->set("a", Value(int32_t(9)));
  strong_meta.customData.as_dictionary()->set("nested", strong_nested);

  TypedExtensionField strong_extension;
  strong_extension.name = "extensionDict";
  strong_extension.unregistered = true;
  strong_extension.unregistered_source =
      "{ dictionary nested = { int a = 9 } }";
  strong_extension.value = Value::MakeDictionary();
  Value strong_extension_nested = Value::MakeDictionary();
  strong_extension_nested.as_dictionary()->set("a", Value(int32_t(9)));
  strong_extension.value.as_dictionary()->set("nested",
                                               strong_extension_nested);
  strong_meta.unknownFields.push_back(strong_extension);
  TypedExtensionField weak_extension;
  weak_extension.name = "extensionDict";
  weak_extension.unregistered = true;
  weak_extension.value = Value::MakeDictionary();
  Value weak_extension_nested = Value::MakeDictionary();
  weak_extension_nested.as_dictionary()->set("b", Value(int32_t(2)));
  weak_extension.value.as_dictionary()->set("nested", weak_extension_nested);
  weak_extension.value.as_dictionary()->set("weak", Value(true));
  weak_meta.unknownFields.push_back(weak_extension);

  Compositor::CopyLocalOpinions(composed_prop, strong_prop);
  Compositor::CopyLocalOpinions(composed_prop, weak_prop);
  const PropMeta* merged_meta = composed_prop.property_meta("x");
  assert(merged_meta && merged_meta->displayName == "Strong" &&
         merged_meta->renderType == "color" &&
         merged_meta->connectability == "interfaceOnly" &&
         merged_meta->allowedTokens == std::vector<std::string>({"a", "b"}) &&
         merged_meta->assetInfo.as_dictionary()->find("weak") &&
         merged_meta->unknownMeta.size() == 2);
  assert(composed_prop.meta().unknownMeta().size() == 2 &&
         "unknown prim metadata must fill by field name across sites");
  const Dict* merged_custom = merged_meta->customData.as_dictionary();
  const Dict* merged_nested =
      merged_custom->find("nested")->as_dictionary();
  assert(merged_nested->find("a") && merged_nested->find("b"));
  assert(merged_meta->unknownFields.size() == 1);
  const Dict* merged_extension =
      merged_meta->unknownFields[0].value.as_dictionary();
  const Dict* merged_extension_nested =
      merged_extension->find("nested")->as_dictionary();
  assert(merged_extension->find("weak") &&
         merged_extension_nested->find("a") &&
         merged_extension_nested->find("b") &&
         merged_meta->unknownFields[0].unregistered_source.empty());

  LoadResult extension_stage = Parse("def Scope \"P\" { float x = 2 }\n");
  PrimSpec* extension_prim =
      extension_stage.stage.GetRootLayer()->prim_at_path_mutable("/P");
  extension_prim->ensure_property_meta("x") = *merged_meta;
  std::vector<uint8_t> extension_crate;
  assert(WriteUSDCToMemory(extension_crate, extension_stage.stage).success);
  USDCLoadResult extension_back = LoadUSDCFromMemory(
      extension_crate.data(), extension_crate.size());
  assert(extension_back.success);
  const PrimSpec* extension_back_prim =
      extension_back.stage.GetRootLayer()->prim_at_path("/P");
  const PropMeta* extension_back_meta =
      extension_back_prim->property_meta("x");
  assert(extension_back_meta && extension_back_meta->unknownFields.size() == 1);
  const Dict* extension_back_dict =
      extension_back_meta->unknownFields[0].value.as_dictionary();
  assert(extension_back_dict && extension_back_dict->find("weak") &&
         extension_back_dict->find("nested")->as_dictionary()->find("a") &&
         extension_back_dict->find("nested")->as_dictionary()->find("b"));
}

void TestNamespaceOrdering() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult result = LoadUSDAFromFile(
      UsdaFixturePath("aousd-namespace-order.usda"), strict);
  assert(result.success);
  const std::vector<UsdPrim> roots = result.stage.GetRootPrims();
  assert(roots.size() == 2 && roots[0].GetName() == "B" &&
         roots[1].GetName() == "A");
  const UsdPrim b = result.stage.GetPrimAtPath("/B");
  const std::vector<UsdPrim> children = b.GetChildren();
  assert(children.size() == 2 && children[0].GetName() == "D" &&
         children[1].GetName() == "C");
  const std::vector<std::string> properties = b.GetPropertyNames();
  const auto y = std::find(properties.begin(), properties.end(), "y");
  const auto x = std::find(properties.begin(), properties.end(), "x");
  assert(y != properties.end() && x != properties.end() && y < x);
  const std::string rewritten = WriteUSDAToString(result.stage);
  assert(rewritten.find("reorder rootPrims") != std::string::npos);
  assert(rewritten.find("reorder properties") != std::string::npos);
  assert(rewritten.find("reorder nameChildren") != std::string::npos);

  // primOrder/propertyOrder are real population fields, not aliases for the
  // natural primChildren/properties lists. They must survive Crate explicitly.
  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, result.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const std::vector<UsdPrim> crate_roots = back.stage.GetRootPrims();
  assert(crate_roots.size() == 2 && crate_roots[0].GetName() == "B" &&
         crate_roots[1].GetName() == "A");
  const UsdPrim crate_b = back.stage.GetPrimAtPath("/B");
  const std::vector<UsdPrim> crate_children = crate_b.GetChildren();
  assert(crate_children.size() == 2 && crate_children[0].GetName() == "D" &&
         crate_children[1].GetName() == "C");
  const std::vector<std::string> crate_props = crate_b.GetPropertyNames();
  const auto crate_y = std::find(crate_props.begin(), crate_props.end(), "y");
  const auto crate_x = std::find(crate_props.begin(), crate_props.end(), "x");
  assert(crate_y != crate_props.end() && crate_x != crate_props.end() &&
         crate_y < crate_x);
  const std::string crate_usda = WriteUSDAToString(back.stage);
  assert(crate_usda.find("reorder rootPrims") != std::string::npos);
  assert(crate_usda.find("reorder properties") != std::string::npos);
  assert(crate_usda.find("reorder nameChildren") != std::string::npos);
}

void TestDefaultPrimReferenceEncoding() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult result = LoadUSDAFromFile(
      UsdaFixturePath("aousd-defaultprim-reference.usda"), strict);
  assert(result.success);
  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, result.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const std::string usda = WriteUSDAToString(back.stage);
  assert(usda.find("@aousd-defaultprim-target.usda@") != std::string::npos);
  assert(usda.find("@aousd-defaultprim-target.usda@</>") ==
             std::string::npos &&
         "omitted reference prim path must stay empty, not become </>");

  // The empty path is semantic, not just textual: after the USDC round trip,
  // composition must select the referenced layer's defaultPrim.
  LoadResult target = LoadUSDAFromFile(
      UsdaFixturePath("aousd-defaultprim-target.usda"), strict);
  assert(target.success);
  std::unique_ptr<Layer> target_layer = TakeLayer(&target);
  std::unique_ptr<Layer> host_layer = back.stage.ReleaseRootLayer();
  assert(host_layer);
  Compositor compositor;
  compositor.SetLayerLoader([&](const std::string& path, std::string*) {
    if (path.find("aousd-defaultprim-target.usda") == std::string::npos)
      return std::unique_ptr<Layer>();
    return std::make_unique<Layer>(target_layer->Clone());
  });
  std::unique_ptr<Layer> composed =
      compositor.Compose(*host_layer, "host.usdc");
  assert(composed);
  const PrimSpec* host = composed->prim_at_path("/Host");
  assert(host);
  const Value* marker = host->property_value("marker");
  assert(marker && marker->as_int() && *marker->as_int() == 7 &&
         "omitted reference path must compose the target defaultPrim");
}

void TestRelationshipForwarding() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult result = LoadUSDAFromFile(
      NextFixturePath("aousd-relationship-forwarding.usda"), strict);
  assert(result.success);

  const UsdPrim foo = result.stage.GetPrimAtPath("/foo");
  const std::vector<Path>* raw = foo.GetRelationship("myRel");
  assert(raw && raw->size() == 3 && (*raw)[0] == Path("/foo/bar") &&
         (*raw)[1] == Path("/baz.bazrel") &&
         (*raw)[2] == Path("/foo/bar"));

  std::vector<Path> forwarded;
  assert(foo.GetForwardedRelationshipTargets("myRel", &forwarded));
  assert(forwarded.size() == 4 && forwarded[0] == Path("/foo/bar") &&
         forwarded[1] == Path("/foo/terminalAttr") &&
         forwarded[2] == Path("/foo/foobar") &&
         forwarded[3] == Path("/foo/foobar/barbaz") &&
         "forwarding must recurse, preserve first-seen order, and deduplicate");

  assert(foo.GetForwardedRelationshipTargets("cycle", &forwarded));
  assert(forwarded.size() == 1 && forwarded[0] == Path("/foo/bar") &&
         "relationship forwarding cycles must terminate");
  assert(!foo.GetForwardedRelationshipTargets("missing", &forwarded));

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, result.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const UsdPrim crate_foo = back.stage.GetPrimAtPath("/foo");
  assert(crate_foo.GetForwardedRelationshipTargets("myRel", &forwarded));
  assert(forwarded.size() == 4 && forwarded[0] == Path("/foo/bar") &&
         forwarded[1] == Path("/foo/terminalAttr") &&
         forwarded[2] == Path("/foo/foobar") &&
         forwarded[3] == Path("/foo/foobar/barbaz") &&
         "forwarding semantics must survive USDC round-trip");
}

void TestAuthoredEmptyMetadata() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult default_prim = LoadUSDAFromFile(
      NextFixturePath("aousd-authored-empty-defaultprim.usda"), strict);
  assert(default_prim.success);
  const Layer* default_prim_layer = default_prim.stage.GetRootLayer();
  assert(default_prim_layer && default_prim_layer->meta().defaultPrim_set &&
         default_prim_layer->meta().defaultPrim.empty());
  assert(WriteUSDAToString(default_prim.stage).find("defaultPrim = \"\"") !=
         std::string::npos);
  std::vector<uint8_t> default_prim_crate;
  assert(WriteUSDCToMemory(default_prim_crate, default_prim.stage).success);
  USDCLoadResult default_prim_back = LoadUSDCFromMemory(
      default_prim_crate.data(), default_prim_crate.size());
  assert(default_prim_back.success);
  const Layer* default_prim_back_layer =
      default_prim_back.stage.GetRootLayer();
  assert(default_prim_back_layer &&
         default_prim_back_layer->meta().defaultPrim_set &&
         default_prim_back_layer->meta().defaultPrim.empty());

  LoadResult weak_default = Parse("(defaultPrim = \"Weak\")\n"
                                  "def Scope \"Weak\" {}\n", true);
  assert(weak_default.success);
  std::unique_ptr<Layer> strong_default_layer = TakeLayer(&default_prim);
  std::unique_ptr<Layer> weak_default_layer = TakeLayer(&weak_default);
  assert(strong_default_layer && weak_default_layer);
  strong_default_layer->meta().FillAbsentStageMetaFrom(
      weak_default_layer->meta());
  assert(strong_default_layer->meta().defaultPrim_set &&
         strong_default_layer->meta().defaultPrim.empty() &&
         "authored-empty defaultPrim must block a weaker defaultPrim");

  LoadResult result = LoadUSDAFromFile(
      NextFixturePath("aousd-authored-empty-metadata.usda"), strict);
  assert(result.success);
  const Layer* layer = result.stage.GetRootLayer();
  assert(layer && layer->meta().doc_set && layer->meta().doc.empty());
  const UsdPrim prim = result.stage.GetPrimAtPath("/P");
  assert(prim && prim.GetMeta().doc_authored() && prim.GetMeta().doc().empty());
  const PropMeta* property_meta = prim.GetPropertyMeta("value");
  assert(property_meta && (property_meta->authored & PropMeta::kDoc) &&
         property_meta->doc.empty());

  auto count_empty_docs = [](const std::string& text) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find("doc = \"\"", pos)) != std::string::npos) {
      ++count;
      pos += 8;
    }
    return count;
  };
  assert(count_empty_docs(WriteUSDAToString(result.stage)) == 3 &&
         "layer, prim, and property authored-empty documentation must survive");

  std::vector<uint8_t> crate;
  USDCWriteOptions write_options;
  write_options.crate_options.strict_aousd_conformance = true;
  assert(WriteUSDCToMemory(crate, result.stage, write_options).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  assert(count_empty_docs(WriteUSDAToString(back.stage)) == 3 &&
         "USDC documentation must use the standard field and retain authored state");

  LoadResult empty_prim_strings = Parse(
      "over \"P\" (kind = \"\" displayName = \"\" comment = \"\") {}\n",
      true);
  assert(empty_prim_strings.success);
  const UsdPrim empty_strings_prim =
      empty_prim_strings.stage.GetPrimAtPath("/P");
  assert(empty_strings_prim.GetMeta().kindAuthored() &&
         empty_strings_prim.GetMeta().displayNameAuthored() &&
         empty_strings_prim.GetMeta().comment_authored());
  LoadResult weak_prim_strings = Parse(
      "def \"P\" (kind = \"component\" displayName = \"Weak\" "
      "comment = \"Weak comment\") {}\n",
      true);
  PrimSpec merged_strings("P");
  Compositor::CopyLocalOpinions(
      merged_strings, *empty_prim_strings.stage.GetRootLayer()->prim_at_path("/P"));
  Compositor::CopyLocalOpinions(
      merged_strings, *weak_prim_strings.stage.GetRootLayer()->prim_at_path("/P"));
  assert(merged_strings.meta().kindAuthored() &&
         merged_strings.meta().kind().empty() &&
         merged_strings.meta().displayNameAuthored() &&
         merged_strings.meta().displayName().empty() &&
         merged_strings.meta().comment_authored() &&
         merged_strings.meta().comment().empty() &&
         "authored-empty prim strings must block weaker opinions");
  std::vector<uint8_t> empty_strings_crate;
  assert(WriteUSDCToMemory(empty_strings_crate,
                           empty_prim_strings.stage).success);
  USDCLoadResult empty_strings_back = LoadUSDCFromMemory(
      empty_strings_crate.data(), empty_strings_crate.size());
  assert(empty_strings_back.success);
  const UsdPrim empty_strings_back_prim =
      empty_strings_back.stage.GetPrimAtPath("/P");
  assert(empty_strings_back_prim.GetMeta().kindAuthored() &&
         empty_strings_back_prim.GetMeta().displayNameAuthored() &&
         empty_strings_back_prim.GetMeta().comment_authored());
  const std::string empty_strings_usda =
      WriteUSDAToString(empty_strings_back.stage);
  // Comment round-trips as a BARE empty string literal (pxr spelling).
  assert(empty_strings_usda.find("kind = \"\"") != std::string::npos &&
         empty_strings_usda.find("displayName = \"\"") !=
             std::string::npos &&
         empty_strings_usda.find("\n    \"\"\n") != std::string::npos);

  LoadResult empty_variant_selection = Parse(
      "over \"V\" (variants = { string look = \"\" }) {}\n", true);
  assert(empty_variant_selection.success);
  const PrimSpec* empty_variant_prim =
      empty_variant_selection.stage.GetRootLayer()->prim_at_path("/V");
  const std::vector<std::pair<std::string, std::string>> empty_look = {
      {"look", ""}};
  assert(empty_variant_prim &&
         empty_variant_prim->meta().variantSelections() == empty_look);
  assert(WriteUSDAToString(empty_variant_selection.stage)
             .find("string look = \"\"") != std::string::npos);
  std::vector<uint8_t> empty_variant_crate;
  assert(WriteUSDCToMemory(empty_variant_crate,
                           empty_variant_selection.stage).success);
  USDCLoadResult empty_variant_back = LoadUSDCFromMemory(
      empty_variant_crate.data(), empty_variant_crate.size());
  assert(empty_variant_back.success);
  const PrimSpec* empty_variant_back_prim =
      empty_variant_back.stage.GetRootLayer()->prim_at_path("/V");
  assert(empty_variant_back_prim &&
         empty_variant_back_prim->meta().variantSelections() == empty_look &&
         "an authored empty selection must survive USDC and block weaker "
         "variant selections");
  PrimSpec empty_variant_merged("V");
  PrimSpec weak_variant("V");
  weak_variant.meta().variantSelections().push_back({"look", "red"});
  Compositor::CopyLocalOpinions(empty_variant_merged,
                                *empty_variant_back_prim);
  Compositor::CopyLocalOpinions(empty_variant_merged, weak_variant);
  assert(empty_variant_merged.meta().variantSelections() == empty_look);

  LoadResult dictionaries = LoadUSDAFromFile(
      NextFixturePath("aousd-authored-empty-dictionaries.usda"), strict);
  assert(dictionaries.success);
  const Layer* dict_layer = dictionaries.stage.GetRootLayer();
  const UsdPrim dict_prim = dictionaries.stage.GetPrimAtPath("/P");
  const PropMeta* dict_prop = dict_prim.GetPropertyMeta("value");
  assert(dict_layer && dict_layer->meta().customLayerData_set &&
         dict_prim.GetMeta().customDataAuthored() && dict_prop &&
         (dict_prop->authored & PropMeta::kCustomData));
  auto count = [](const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
         pos += needle.size()) {
      ++n;
    }
    return n;
  };
  const std::string dict_usda = WriteUSDAToString(dictionaries.stage);
  assert(count(dict_usda, "customLayerData = {") == 1);
  assert(count(dict_usda, "customData = {") == 2 &&
         "authored-empty dictionaries must survive at all core scopes");
  std::vector<uint8_t> dict_crate;
  assert(WriteUSDCToMemory(dict_crate, dictionaries.stage).success);
  USDCLoadResult dict_back =
      LoadUSDCFromMemory(dict_crate.data(), dict_crate.size());
  assert(dict_back.success);
  const std::string dict_back_usda = WriteUSDAToString(dict_back.stage);
  assert(count(dict_back_usda, "customLayerData = {") == 1);
  assert(count(dict_back_usda, "customData = {") == 2);

  LoadResult color = LoadUSDAFromFile(
      NextFixturePath("aousd-authored-empty-color.usda"), strict);
  assert(color.success);
  const Layer* color_layer = color.stage.GetRootLayer();
  assert(color_layer && color_layer->meta().colorConfiguration_set &&
         color_layer->meta().colorManagementSystem_set &&
         color_layer->meta().colorConfiguration.empty() &&
         color_layer->meta().colorManagementSystem.empty());
  const std::string color_usda = WriteUSDAToString(color.stage);
  assert(color_usda.find("colorConfiguration = @@") != std::string::npos);
  assert(color_usda.find("colorManagementSystem = \"\"") !=
         std::string::npos);
  std::vector<uint8_t> color_crate;
  assert(WriteUSDCToMemory(color_crate, color.stage).success);
  USDCLoadResult color_back =
      LoadUSDCFromMemory(color_crate.data(), color_crate.size());
  assert(color_back.success);
  const std::string color_back_usda = WriteUSDAToString(color_back.stage);
  assert(color_back_usda.find("colorConfiguration = @@") !=
         std::string::npos);
  assert(color_back_usda.find("colorManagementSystem = \"\"") !=
         std::string::npos);

  // OpenUSD-authored USDC can retain authored-empty namespace ordering even
  // though USDA rejects `reorder ... = []`. Preserve that field presence in
  // the layer model and when writing another crate. The same oracle fixture
  // also carries an explicit-empty variantSetNames list-op; that richer
  // list-op authored state is audited separately.
  const std::vector<uint8_t> empty_order_fixture =
      ReadHexFixture("aousd-authored-empty-orders.usdc.hex");
  USDCLoadResult empty_orders = LoadUSDCFromMemory(
      empty_order_fixture.data(), empty_order_fixture.size());
  assert(empty_orders.success);
  const Layer* empty_order_layer = empty_orders.stage.GetRootLayer();
  const UsdPrim empty_order_prim = empty_orders.stage.GetPrimAtPath("/P");
  assert(empty_order_layer && empty_order_layer->meta().rootPrimOrder_set &&
         empty_order_layer->meta().rootPrimOrder.empty());
  assert(empty_order_prim && empty_order_prim.GetMeta().primOrderAuthored() &&
         empty_order_prim.GetMeta().primOrder().empty() &&
         empty_order_prim.GetMeta().propertyOrderAuthored() &&
         empty_order_prim.GetMeta().propertyOrder().empty());

  std::vector<uint8_t> empty_order_crate;
  assert(WriteUSDCToMemory(empty_order_crate, empty_orders.stage).success);
  USDCLoadResult empty_order_back = LoadUSDCFromMemory(
      empty_order_crate.data(), empty_order_crate.size());
  assert(empty_order_back.success);
  const Layer* empty_order_back_layer = empty_order_back.stage.GetRootLayer();
  const UsdPrim empty_order_back_prim =
      empty_order_back.stage.GetPrimAtPath("/P");
  assert(empty_order_back_layer &&
         empty_order_back_layer->meta().rootPrimOrder_set);
  assert(empty_order_back_prim &&
         empty_order_back_prim.GetMeta().primOrderAuthored() &&
         empty_order_back_prim.GetMeta().propertyOrderAuthored());
}

void TestSpecifierResolution() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult strong = LoadUSDAFromFile(
      NextFixturePath("aousd-specifier-strong.usda"), strict);
  LoadResult weak = LoadUSDAFromFile(
      NextFixturePath("aousd-specifier-weak.usda"), strict);
  assert(strong.success && weak.success);
  std::unique_ptr<Layer> strong_layer = TakeLayer(&strong);
  std::unique_ptr<Layer> weak_layer = TakeLayer(&weak);
  assert(strong_layer && weak_layer);

  Compositor compositor;
  compositor.SetLayerLoader([&](const std::string& path, std::string*) {
    if (path.find("aousd-specifier-weak.usda") == std::string::npos) {
      return std::unique_ptr<Layer>();
    }
    return std::make_unique<Layer>(weak_layer->Clone());
  });
  std::unique_ptr<Layer> composed =
      compositor.Compose(*strong_layer, "aousd-specifier-strong.usda");
  assert(composed);
  assert(composed->prim_at_path("/WeakDefStrongOver")->specifier() ==
         PrimSpecifier::Def);
  assert(composed->prim_at_path("/WeakClassStrongOver")->specifier() ==
         PrimSpecifier::Class &&
         "a stronger over backed by a weaker class resolves as class");
  assert(composed->prim_at_path("/WeakDefStrongClass")->specifier() ==
         PrimSpecifier::Class);
  assert(composed->prim_at_path("/WeakClassStrongDef")->specifier() ==
         PrimSpecifier::Def);

  LoadResult inherit_strong = LoadUSDAFromFile(
      NextFixturePath("aousd-specifier-inherit-strong.usda"), strict);
  LoadResult inherit_weak = LoadUSDAFromFile(
      NextFixturePath("aousd-specifier-inherit-weak.usda"), strict);
  assert(inherit_strong.success && inherit_weak.success);
  std::unique_ptr<Layer> inherit_strong_layer = TakeLayer(&inherit_strong);
  std::unique_ptr<Layer> inherit_weak_layer = TakeLayer(&inherit_weak);
  Compositor inherit_compositor;
  inherit_compositor.SetLayerLoader(
      [&](const std::string& path, std::string*) {
        if (path.find("aousd-specifier-inherit-weak.usda") ==
            std::string::npos) {
          return std::unique_ptr<Layer>();
        }
        return std::make_unique<Layer>(inherit_weak_layer->Clone());
      });
  std::unique_ptr<Layer> inherited = inherit_compositor.Compose(
      *inherit_strong_layer, "aousd-specifier-inherit-strong.usda");
  assert(inherited);
  assert(inherited->prim_at_path("/OnlyInheritedClass")->specifier() ==
         PrimSpecifier::Class);
  assert(inherited->prim_at_path("/OnlyInheritedDef")->specifier() ==
         PrimSpecifier::Def);
  assert(inherited->prim_at_path("/LocalDefVsInheritedClass")->specifier() ==
         PrimSpecifier::Def);
  assert(inherited->prim_at_path("/LocalClassVsInheritedDef")->specifier() ==
         PrimSpecifier::Class &&
         "weaker local and direct-inherit specifier rules must match AOUSD");

  LoadResult multi = LoadUSDAFromFile(
      NextFixturePath("aousd-specifier-specializes.usda"), strict);
  assert(multi.success);
  std::unique_ptr<Layer> multi_layer = TakeLayer(&multi);
  Compositor multi_compositor;
  std::unique_ptr<Layer> multi_composed =
      multi_compositor.Compose(*multi_layer, "specifier-specializes.usda");
  assert(multi_composed);
  assert(multi_composed->prim_at_path("/TwoInherits")->specifier() ==
         PrimSpecifier::Def);
  assert(multi_composed->prim_at_path("/TwoInheritsReversed")->specifier() ==
         PrimSpecifier::Def &&
         "a def among direct inherits must concretely define in either order");
  assert(multi_composed->prim_at_path("/TwoSpecializes")->specifier() ==
         PrimSpecifier::Class &&
         "specializes remain strength-ordered rather than def-preferred");
}

void TestVariantSetListOpFidelity() {
  LoadResult text = Parse(
      "def Scope \"Text\" (\n"
      "  add variantSets = [\"add\"]\n"
      "  prepend variantSets = [\"pre\"]\n"
      "  append variantSets = [\"app\"]\n"
      "  delete variantSets = [\"del\"]\n"
      "  reorder variantSets = [\"ord\"]\n"
      ") {}\n",
      true);
  assert(text.success);
  const StringListOpEdits& text_edits =
      text.stage.GetPrimAtPath("/Text").GetMeta().variantSetNameEdits();
  assert(text_edits.added == std::vector<std::string>{"add"} &&
         text_edits.prepended == std::vector<std::string>{"pre"} &&
         text_edits.appended == std::vector<std::string>{"app"} &&
         text_edits.deleted == std::vector<std::string>{"del"} &&
         text_edits.ordered == std::vector<std::string>{"ord"});

  const std::vector<uint8_t> oracle =
      ReadHexFixture("aousd-variantset-listops.usdc.hex");
  USDCLoadResult loaded = LoadUSDCFromMemory(oracle.data(), oracle.size());
  assert(loaded.success);
  auto edits = [&](const char* path) -> const StringListOpEdits& {
    const UsdPrim prim = loaded.stage.GetPrimAtPath(path);
    assert(prim);
    return prim.GetMeta().variantSetNameEdits();
  };
  assert(edits("/ExplicitEmpty").authored &&
         edits("/ExplicitEmpty").is_explicit &&
         edits("/ExplicitEmpty").explicit_items.empty());
  assert(edits("/Explicit").is_explicit &&
         edits("/Explicit").explicit_items ==
             std::vector<std::string>{"explicit"});
  assert(edits("/Prepended").prepended ==
         std::vector<std::string>{"pre"});
  assert(edits("/Appended").appended ==
         std::vector<std::string>{"app"});
  assert(edits("/Added").added == std::vector<std::string>{"add"});
  assert(edits("/Deleted").deleted == std::vector<std::string>{"del"});
  assert(edits("/Ordered").ordered == std::vector<std::string>{"ord"});
  const StringListOpEdits& mixed = edits("/Mixed");
  assert(mixed.authored && !mixed.is_explicit &&
         mixed.added == std::vector<std::string>{"add"} &&
         mixed.prepended == std::vector<std::string>{"pre"} &&
         mixed.appended == std::vector<std::string>{"app"} &&
         mixed.deleted == std::vector<std::string>{"del"} &&
         mixed.ordered == std::vector<std::string>{"ord"});

  const std::string usda = WriteUSDAToString(loaded.stage);
  assert(usda.find("variantSets = [\"explicit\"]") != std::string::npos);
  assert(usda.find("add variantSets = [\"add\"]") != std::string::npos);
  assert(usda.find("prepend variantSets = [\"pre\"]") !=
         std::string::npos);
  assert(usda.find("append variantSets = [\"app\"]") !=
         std::string::npos);
  assert(usda.find("delete variantSets = [\"del\"]") !=
         std::string::npos);
  assert(usda.find("reorder variantSets = [\"ord\"]") !=
         std::string::npos);

  std::vector<uint8_t> rewritten;
  assert(WriteUSDCToMemory(rewritten, loaded.stage).success);
  USDCLoadResult back =
      LoadUSDCFromMemory(rewritten.data(), rewritten.size());
  assert(back.success);
  const UsdPrim empty_back = back.stage.GetPrimAtPath("/ExplicitEmpty");
  const UsdPrim mixed_back = back.stage.GetPrimAtPath("/Mixed");
  assert(empty_back && empty_back.GetMeta().variantSetNameEdits().authored &&
         empty_back.GetMeta().variantSetNameEdits().is_explicit &&
         empty_back.GetMeta().variantSetNameEdits().explicit_items.empty());
  assert(mixed_back &&
         mixed_back.GetMeta().variantSetNameEdits().added ==
             std::vector<std::string>{"add"} &&
         mixed_back.GetMeta().variantSetNameEdits().prepended ==
             std::vector<std::string>{"pre"} &&
         mixed_back.GetMeta().variantSetNameEdits().appended ==
             std::vector<std::string>{"app"} &&
         mixed_back.GetMeta().variantSetNameEdits().deleted ==
             std::vector<std::string>{"del"} &&
         mixed_back.GetMeta().variantSetNameEdits().ordered ==
             std::vector<std::string>{"ord"});
}

void TestApiSchemaListOpFidelity() {
  LoadResult text = Parse(
      "def Scope \"Empty\" (apiSchemas = None) {}\n"
      "def Scope \"Text\" (\n"
      "  add apiSchemas = [\"AddAPI\"]\n"
      "  prepend apiSchemas = [\"PreAPI\"]\n"
      "  append apiSchemas = [\"AppAPI\"]\n"
      "  delete apiSchemas = [\"DelAPI\"]\n"
      "  reorder apiSchemas = [\"OrdAPI\"]\n"
      ") {}\n",
      true);
  assert(text.success);
  const StringListOpEdits& empty_text =
      text.stage.GetPrimAtPath("/Empty").GetMeta().apiSchemaEdits();
  const StringListOpEdits& mixed_text =
      text.stage.GetPrimAtPath("/Text").GetMeta().apiSchemaEdits();
  assert(empty_text.authored && empty_text.is_explicit &&
         empty_text.explicit_items.empty());
  assert(mixed_text.added == std::vector<std::string>{"AddAPI"} &&
         mixed_text.prepended == std::vector<std::string>{"PreAPI"} &&
         mixed_text.appended == std::vector<std::string>{"AppAPI"} &&
         mixed_text.deleted == std::vector<std::string>{"DelAPI"} &&
         mixed_text.ordered == std::vector<std::string>{"OrdAPI"});

  const std::vector<uint8_t> oracle =
      ReadHexFixture("aousd-apischemas-listops.usdc.hex");
  USDCLoadResult loaded = LoadUSDCFromMemory(oracle.data(), oracle.size());
  assert(loaded.success);
  auto edits = [&](const char* path) -> const StringListOpEdits& {
    const UsdPrim prim = loaded.stage.GetPrimAtPath(path);
    assert(prim);
    return prim.GetMeta().apiSchemaEdits();
  };
  assert(edits("/ExplicitEmpty").authored &&
         edits("/ExplicitEmpty").is_explicit &&
         edits("/ExplicitEmpty").explicit_items.empty());
  assert(edits("/Explicit").explicit_items ==
         std::vector<std::string>{"ExplicitAPI"});
  assert(edits("/Prepended").prepended ==
         std::vector<std::string>{"PreAPI"});
  assert(edits("/Appended").appended ==
         std::vector<std::string>{"AppAPI"});
  assert(edits("/Added").added == std::vector<std::string>{"AddAPI"});
  assert(edits("/Deleted").deleted == std::vector<std::string>{"DelAPI"});
  assert(edits("/Ordered").ordered == std::vector<std::string>{"OrdAPI"});
  const StringListOpEdits& mixed = edits("/Mixed");
  assert(mixed.added == std::vector<std::string>{"AddAPI"} &&
         mixed.prepended == std::vector<std::string>{"PreAPI"} &&
         mixed.appended == std::vector<std::string>{"AppAPI"} &&
         mixed.deleted == std::vector<std::string>{"DelAPI"} &&
         mixed.ordered == std::vector<std::string>{"OrdAPI"});

  const std::string usda = WriteUSDAToString(loaded.stage);
  assert(usda.find("apiSchemas = None") != std::string::npos);
  assert(usda.find("add apiSchemas = [\"AddAPI\"]") != std::string::npos);
  assert(usda.find("prepend apiSchemas = [\"PreAPI\"]") !=
         std::string::npos);
  assert(usda.find("append apiSchemas = [\"AppAPI\"]") !=
         std::string::npos);
  assert(usda.find("delete apiSchemas = [\"DelAPI\"]") !=
         std::string::npos);
  assert(usda.find("reorder apiSchemas = [\"OrdAPI\"]") !=
         std::string::npos);

  std::vector<uint8_t> rewritten;
  assert(WriteUSDCToMemory(rewritten, loaded.stage).success);
  USDCLoadResult back =
      LoadUSDCFromMemory(rewritten.data(), rewritten.size());
  assert(back.success);
  const StringListOpEdits& empty_back =
      back.stage.GetPrimAtPath("/ExplicitEmpty").GetMeta().apiSchemaEdits();
  const StringListOpEdits& mixed_back =
      back.stage.GetPrimAtPath("/Mixed").GetMeta().apiSchemaEdits();
  assert(empty_back.authored && empty_back.is_explicit &&
         empty_back.explicit_items.empty());
  assert(mixed_back.added == mixed.added &&
         mixed_back.prepended == mixed.prepended &&
         mixed_back.appended == mixed.appended &&
         mixed_back.deleted == mixed.deleted &&
         mixed_back.ordered == mixed.ordered);

  LoadResult strong = LoadUSDAFromFile(
      NextFixturePath("aousd-apischemas-compose-strong.usda"));
  LoadResult weak = LoadUSDAFromFile(
      NextFixturePath("aousd-apischemas-compose-weak.usda"));
  assert(strong.success && weak.success);
  std::unique_ptr<Layer> strong_layer = TakeLayer(&strong);
  std::unique_ptr<Layer> weak_layer = TakeLayer(&weak);
  Compositor compositor;
  compositor.SetLayerLoader([&](const std::string& path, std::string*) {
    if (path.find("aousd-apischemas-compose-weak.usda") ==
        std::string::npos) {
      return std::unique_ptr<Layer>();
    }
    return std::make_unique<Layer>(weak_layer->Clone());
  });
  std::unique_ptr<Layer> composed = compositor.Compose(
      *strong_layer, NextFixturePath("aousd-apischemas-compose-strong.usda"));
  assert(composed);
  const PrimSpec* composed_prim = composed->prim_at_path("/P");
  assert(composed_prim &&
         composed_prim->meta().apiSchemas() ==
             std::vector<std::string>{"KeepAPI"});
  const StringListOpEdits& resolved =
      composed_prim->meta().apiSchemaEdits();
  assert(resolved.authored && resolved.is_explicit &&
         resolved.explicit_items == std::vector<std::string>{"KeepAPI"} &&
         "strong delete must remove only the weaker matching API schema");
}

void TestConnectionListOpFidelity() {
  LoadResult authored = Parse(
      "def Scope \"P\" {\n"
      "  add float x.connect = </Add.out>\n"
      "  prepend float x.connect = </Pre.out>\n"
      "  append float x.connect = </App.out>\n"
      "  delete float x.connect = </Del.out>\n"
      "  reorder float x.connect = </Ord.out>\n"
      "}\n",
      true);
  assert(authored.success);
  const UsdPrim p = authored.stage.GetPrimAtPath("/P");
  const ArcEdit* edit = p.GetPrimSpec()->connection_edit("x");
  assert(edit && edit->authored && !edit->is_explicit &&
         edit->added == std::vector<std::string>{"/Add.out"} &&
         edit->prepended == std::vector<std::string>{"/Pre.out"} &&
         edit->appended == std::vector<std::string>{"/App.out"} &&
         edit->deleted == std::vector<std::string>{"/Del.out"} &&
         edit->ordered == std::vector<std::string>{"/Ord.out"});
  const std::string text = WriteUSDAToString(authored.stage);
  assert(text.find("add float x.connect = </Add.out>") != std::string::npos &&
         text.find("prepend float x.connect = </Pre.out>") !=
             std::string::npos &&
         text.find("append float x.connect = </App.out>") !=
             std::string::npos &&
         text.find("delete float x.connect = </Del.out>") !=
             std::string::npos &&
         text.find("reorder float x.connect = </Ord.out>") !=
             std::string::npos);

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, authored.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const ArcEdit* back_edit =
      back.stage.GetPrimAtPath("/P").GetPrimSpec()->connection_edit("x");
  assert(back_edit && back_edit->added == edit->added &&
         back_edit->prepended == edit->prepended &&
         back_edit->appended == edit->appended &&
         back_edit->deleted == edit->deleted &&
         back_edit->ordered == edit->ordered);

  LoadResult weak = Parse(
      "def Scope \"P\" { float x.connect = [</Weak.out>, </Drop.out>] }\n");
  LoadResult strong = Parse(
      "over \"P\" {\n"
      "  prepend float x.connect = </Strong.out>\n"
      "  delete float x.connect = </Drop.out>\n"
      "}\n");
  assert(weak.success && strong.success);
  std::unique_ptr<Layer> weak_layer = TakeLayer(&weak);
  std::unique_ptr<Layer> strong_layer = TakeLayer(&strong);
  PrimSpec* strong_p = strong_layer->prim_at_path_mutable("/P");
  const PrimSpec* weak_p = weak_layer->prim_at_path("/P");
  assert(strong_p && weak_p);
  Compositor::CopyLocalOpinions(*strong_p, *weak_p);
  const std::vector<Path>* resolved = strong_p->connection("x");
  assert(resolved && resolved->size() == 2 &&
         (*resolved)[0].str() == "/Strong.out" &&
         (*resolved)[1].str() == "/Weak.out");
  const ArcEdit* resolved_edit = strong_p->connection_edit("x");
  assert(resolved_edit && resolved_edit->is_explicit);
}

void TestStageQueryAncestry() {
  LoadOptions strict;
  strict.parse_options.strict_aousd_conformance = true;
  LoadResult result = LoadUSDAFromFile(
      NextFixturePath("aousd-stage-query-ancestry.usda"), strict);
  assert(result.success);

  const UsdPrim class_child =
      result.stage.GetPrimAtPath("/DefParent/ClassChild");
  const UsdPrim grandchild =
      result.stage.GetPrimAtPath("/DefParent/ClassChild/Grandchild");
  assert(class_child.IsDefined() && class_child.IsAbstract() &&
         !class_child.IsConcretelyDefined());
  assert(grandchild.IsDefined() && grandchild.IsAbstract() &&
         !grandchild.IsConcretelyDefined() &&
         "a class ancestor makes descendants abstract but still defined");

  const UsdPrim over_child =
      result.stage.GetPrimAtPath("/DefParent/OverChild");
  const UsdPrim over_desc =
      result.stage.GetPrimAtPath("/DefParent/OverChild/Descendant");
  assert(!over_child.IsDefined() && !over_desc.IsDefined() &&
         "an undefining ancestor makes the whole descendant path undefined");

  const UsdPrim class_parent = result.stage.GetPrimAtPath("/ClassParent");
  const UsdPrim class_parent_child =
      result.stage.GetPrimAtPath("/ClassParent/Child");
  assert(class_parent.IsDefined() && class_parent.IsAbstract());
  assert(class_parent_child.IsDefined() && class_parent_child.IsAbstract());

  const UsdPrim inactive_child =
      result.stage.GetPrimAtPath("/Inactive/Child");
  assert(!inactive_child.IsActive() &&
         "a stronger active=true child cannot override an inactive ancestor");

  LoadResult models = LoadUSDAFromFile(
      NextFixturePath("aousd-model-hierarchy.usda"), strict);
  assert(models.success);
  assert(models.stage.GetPrimAtPath("/RootGroup").IsInModelHierarchy());
  assert(models.stage.GetPrimAtPath("/RootGroup/Assembly")
             .IsInModelHierarchy());
  assert(models.stage.GetPrimAtPath("/RootGroup/Assembly/Component")
             .IsInModelHierarchy());
  assert(!models.stage
              .GetPrimAtPath(
                  "/RootGroup/Assembly/Component/Subcomponent")
              .IsInModelHierarchy());
  assert(models.stage.GetPrimAtPath("/RootComponent").IsInModelHierarchy());
  assert(!models.stage.GetPrimAtPath("/Broken/Component")
              .IsInModelHierarchy() &&
         "an unkinded ancestor breaks model-hierarchy continuity");
}

void TestInterpolationMatrix() {
  LoadResult result = Parse(
      "def Scope \"I\" {\n"
      "  half h.timeSamples = { 0: 0, 2: 2 }\n"
      "  float f.timeSamples = { 0: 0, 2: 2 }\n"
      "  double d.timeSamples = { 0: 0, 2: 2 }\n"
      "  timecode tc.timeSamples = { 0: 0, 2: 2 }\n"
      "  half2 h2.timeSamples = { 0: (0,0), 2: (2,4) }\n"
      "  float2 f2.timeSamples = { 0: (0,0), 2: (2,4) }\n"
      "  double2 d2.timeSamples = { 0: (0,0), 2: (2,4) }\n"
      "  half3 h3.timeSamples = { 0: (0,0,0), 2: (2,4,6) }\n"
      "  float3 f3.timeSamples = { 0: (0,0,0), 2: (2,4,6) }\n"
      "  double3 d3.timeSamples = { 0: (0,0,0), 2: (2,4,6) }\n"
      "  half4 h4.timeSamples = { 0: (0,0,0,0), 2: (2,4,6,8) }\n"
      "  float4 f4.timeSamples = { 0: (0,0,0,0), 2: (2,4,6,8) }\n"
      "  double4 d4.timeSamples = { 0: (0,0,0,0), 2: (2,4,6,8) }\n"
      "  color3d cd.timeSamples = { 0: (0,0,0), 2: (2,4,6) }\n"
      "  matrix2d m2.timeSamples = { 0: ((0,0),(0,0)), 2: ((2,4),(6,8)) }\n"
      "  matrix3d m3.timeSamples = { 0: ((0,0,0),(0,0,0),(0,0,0)), 2: ((2,4,6),(8,10,12),(14,16,18)) }\n"
      "  matrix4d m4.timeSamples = { 0: ((0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)), 2: ((2,4,6,8),(10,12,14,16),(18,20,22,24),(26,28,30,32)) }\n"
      "  quath qh.timeSamples = { 0: (1,0,0,0), 2: (1,0,0,0) }\n"
      "  quatf qf.timeSamples = { 0: (1,0,0,0), 2: (1,0,0,0) }\n"
      "  quatd qd.timeSamples = { 0: (1,0,0,0), 2: (1,0,0,0) }\n"
      "  token held.timeSamples = { 0: \"left\", 2: \"right\" }\n"
      "}\n",
      true);
  assert(result.success);
  const UsdPrim prim = result.stage.GetPrimAtPath("/I");
  AttributeEval eval(&result.stage);
  EvalOptions opts;
  opts.time = 1.0;
  EvalResult h = eval.EvalWith(prim, "h", opts);
  float hf = 0.0f;
  assert(h.success && h.value.type_id() == TypeId::Half &&
         h.value.to_float(&hf) && std::fabs(hf - 1.0f) < 1e-3f);
  const std::pair<const char*, TypeId> linear_types[] = {
      {"h", TypeId::Half},       {"f", TypeId::Float},
      {"d", TypeId::Double},     {"tc", TypeId::TimeCode},
      {"h2", TypeId::Half2},     {"f2", TypeId::Float2},
      {"d2", TypeId::Double2},   {"h3", TypeId::Half3},
      {"f3", TypeId::Float3},    {"d3", TypeId::Double3},
      {"h4", TypeId::Half4},     {"f4", TypeId::Float4},
      {"d4", TypeId::Double4},   {"m2", TypeId::Matrix2d},
      {"m3", TypeId::Matrix3d},  {"m4", TypeId::Matrix4d},
      {"qh", TypeId::Quath},     {"qf", TypeId::Quatf},
      {"qd", TypeId::Quatd},     {"cd", TypeId::Color3d},
  };
  for (const auto& item : linear_types) {
    EvalResult value = eval.EvalWith(prim, item.first, opts);
    assert(value.success && value.interpolated &&
           value.value.type_id() == item.second);
  }

  EvalResult hv = eval.EvalWith(prim, "h3", opts);
  float h3[3] = {};
  assert(hv.success && hv.value.type_id() == TypeId::Half3 &&
         hv.value.to_float3(h3));
  assert(std::fabs(h3[0] - 1.0f) < 1e-3f &&
         std::fabs(h3[1] - 2.0f) < 1e-3f &&
         std::fabs(h3[2] - 3.0f) < 1e-3f);
  EvalResult cd = eval.EvalWith(prim, "cd", opts);
  const double* c3 = cd.value.as_double3();
  assert(cd.success && cd.value.type_id() == TypeId::Color3d && c3 &&
         c3[0] == 1.0 && c3[1] == 2.0 && c3[2] == 3.0);
  EvalResult m = eval.EvalWith(prim, "m2", opts);
  const double* m2 = m.value.as_matrix2d();
  assert(m.success && m2 && m2[0] == 1.0 && m2[1] == 2.0 &&
         m2[2] == 3.0 && m2[3] == 4.0);
  EvalResult q = eval.EvalWith(prim, "qh", opts);
  assert(q.success && q.value.type_id() == TypeId::Quath);
  EvalResult held = eval.EvalWith(prim, "held", opts);
  assert(held.success && held.value.as_token() &&
         *held.value.as_token() == "left" && !held.interpolated);
  opts.time = -1.0;
  EvalResult before = eval.EvalWith(prim, "h", opts);
  assert(before.success && before.value.to_float(&hf) && hf == 0.0f);
  opts.time = 3.0;
  EvalResult after = eval.EvalWith(prim, "h", opts);
  assert(after.success && after.value.to_float(&hf) && hf == 2.0f);
}

void TestSchemaFallbackAndValueClips() {
  LoadResult mesh_result = Parse("def Mesh \"M\" {}\n", true);
  assert(mesh_result.success);
  const UsdPrim mesh = mesh_result.stage.GetPrimAtPath("/M");
  assert(mesh.HasProperty("doubleSided"));
  const Value* double_sided = mesh.GetPropertyValue("doubleSided");
  assert(double_sided && double_sided->as_bool() &&
         !*double_sided->as_bool());
  AttributeEval mesh_eval(&mesh_result.stage);
  EvalOptions default_time;
  default_time.time = TimeQuery::Default();
  default_time.strict_aousd_conformance = true;
  EvalResult fallback = mesh_eval.EvalWith(mesh, "orientation", default_time);
  assert(fallback.success && fallback.from_schema_fallback);
  assert(fallback.value.as_token() &&
         *fallback.value.as_token() == "rightHanded");

  LoadResult blocked_result = Parse(
      "def Mesh \"Blocked\" {\n"
      "  uniform token orientation = None\n"
      "  custom token noFallback = None\n"
      "}\n",
      true);
  assert(blocked_result.success);
  const UsdPrim blocked = blocked_result.stage.GetPrimAtPath("/Blocked");
  AttributeEval blocked_eval(&blocked_result.stage);
  EvalResult blocked_fallback =
      blocked_eval.EvalWith(blocked, "orientation", default_time);
  assert(blocked_fallback.success && blocked_fallback.blocked &&
         blocked_fallback.from_schema_fallback &&
         blocked_fallback.value.as_token() &&
         *blocked_fallback.value.as_token() == "rightHanded");
  EvalResult blocked_missing =
      blocked_eval.EvalWith(blocked, "noFallback", default_time);
  assert(!blocked_missing.success && blocked_missing.blocked &&
         !blocked_missing.value.is_block());

  LoadResult time_query_result = Parse(
      "def Xform \"Timed\" {\n"
      "  double sampleOnly.timeSamples = { 0: 4 }\n"
      "}\n",
      true);
  assert(time_query_result.success);
  const UsdPrim timed = time_query_result.stage.GetPrimAtPath("/Timed");
  AttributeEval timed_eval(&time_query_result.stage);
  EvalResult at_default =
      timed_eval.EvalWith(timed, "sampleOnly", default_time);
  assert(!at_default.success &&
         "DefaultTime must not alias numeric time zero");
  EvalOptions numeric_zero;
  numeric_zero.time = TimeQuery::Numeric(0.0);
  EvalResult at_zero = timed_eval.EvalWith(timed, "sampleOnly", numeric_zero);
  assert(at_zero.success && at_zero.from_time_sample &&
         at_zero.value.as_double() && *at_zero.value.as_double() == 4.0);

  LoadResult root_result = Parse(
      "def Xform \"Root\" (\n"
      "  clips = {\n"
      "    dictionary default = {\n"
      "      double2[] active = [(0, 0)]\n"
      "      asset[] assetPaths = [@clip.usda@]\n"
      "      string primPath = \"/Root\"\n"
      "      double2[] times = [(0, 0), (1, 1)]\n"
      "    }\n"
      "  }\n"
      ") { float x }\n");
  assert(root_result.success);
  AttributeEval clip_eval(&root_result.stage);
  EvalOptions clip_options;
  clip_options.time = 0.5;
  clip_options.strict_aousd_conformance = true;
  clip_options.clip_stage_cache = std::make_shared<ValueClipStageCache>();
  int clip_load_count = 0;
  clip_options.clip_stage_loader =
      [&clip_load_count](const std::string& asset, Stage* out, std::string*,
                         std::string*) {
        ++clip_load_count;
        if (asset != "clip.usda" || !out) return false;
        LoadResult clip = Parse(
            "def Xform \"Root\" {\n"
            "  float x.timeSamples = { 0: 2, 1: 4 }\n"
            "}\n");
        if (!clip.success) return false;
        *out = std::move(clip.stage);
        return true;
      };
  EvalResult clipped = clip_eval.EvalWith(
      root_result.stage.GetPrimAtPath("/Root"), "x", clip_options);
  assert(clipped.success && clipped.source_asset == "clip.usda");
  assert(clipped.value.as_float() &&
         std::fabs(*clipped.value.as_float() - 3.0f) < 1e-6f);
  clip_options.time = TimeQuery::Numeric(0.75);
  EvalResult cached_clip = clip_eval.EvalWith(
      root_result.stage.GetPrimAtPath("/Root"), "x", clip_options);
  assert(cached_clip.success && cached_clip.value.as_float() &&
         std::fabs(*cached_clip.value.as_float() - 3.5f) < 1e-6f &&
         clip_load_count == 1 &&
         "caller-owned clip cache must reuse stages across queries");

  LoadResult template_result = Parse(
      "def Xform \"Root\" ( clips = { dictionary default = {\n"
      "  asset templateAssetPath = @clip.##.usd@\n"
      "  double templateStartTime = 12\n"
      "  double templateEndTime = 25\n"
      "  double templateStride = 6\n"
      "  double templateActiveOffset = 0.5\n"
      "} } ) {}\n");
  assert(template_result.success);
  std::vector<ValueClipSet> template_sets;
  std::string template_error;
  assert(ParseValueClipSets(template_result.stage.GetPrimAtPath("/Root"),
                            &template_sets, &template_error));
  assert(template_sets.size() == 1);
  assert(template_sets[0].asset_paths ==
         std::vector<std::string>({"clip.12.usd", "clip.18.usd",
                                   "clip.24.usd"}));
  const std::vector<std::pair<double, int>> expected_template_active = {
      {12.5, 0}, {18.5, 1}, {24.5, 2}};
  assert(template_sets[0].active == expected_template_active);
  const std::vector<std::pair<double, double>> expected_template_times = {
      {11.5, 11.5}, {12.0, 12.0}, {18.0, 18.0},
      {24.0, 24.0}, {25.5, 25.5}};
  assert(template_sets[0].times == expected_template_times);

  LoadResult excessive_template = Parse(
      "def Xform \"Root\" ( clips = { dictionary default = {\n"
      "  asset templateAssetPath = @clip.#.usd@\n"
      "  double templateStartTime = 0\n"
      "  double templateEndTime = 1\n"
      "  double templateStride = 1e-12\n"
      "} } ) {}\n");
  assert(excessive_template.success);
  template_sets.clear();
  template_error.clear();
  assert(!ParseValueClipSets(excessive_template.stage.GetPrimAtPath("/Root"),
                             &template_sets, &template_error));
  assert(template_error.find("expansion limit") != std::string::npos);

  LoadResult fractional_template = Parse(
      "def Xform \"Root\" ( clips = { dictionary default = {\n"
      "  asset templateAssetPath = @clip.#.##.usd@\n"
      "  double templateStartTime = 0\n"
      "  double templateEndTime = 0.3\n"
      "  double templateStride = 0.1\n"
      "} } ) {}\n");
  assert(fractional_template.success);
  template_sets.clear();
  template_error.clear();
  assert(ParseValueClipSets(fractional_template.stage.GetPrimAtPath("/Root"),
                            &template_sets, &template_error));
  assert(template_sets.size() == 1);
  assert(template_sets[0].asset_paths ==
         std::vector<std::string>({"clip.0.00.usd", "clip.0.10.usd",
                                   "clip.0.20.usd", "clip.0.30.usd"}));

  // clipSets is independent from the clips dictionary and defines
  // strongest-to-weakest set traversal.
  LoadResult multi_result = Parse(
      "def Xform \"Root\" (\n"
      "  clipSets = [\"zWeak\", \"aStrong\"]\n"
      "  clips = {\n"
      "    dictionary zWeak = {\n"
      "      asset[] assetPaths = [@weak.usda@]\n"
      "      double2[] active = [(0, 0)]\n"
      "      string primPath = \"/Root\"\n"
      "    }\n"
      "    dictionary aStrong = {\n"
      "      asset[] assetPaths = [@strong.usda@]\n"
      "      double2[] active = [(0, 0)]\n"
      "      string primPath = \"/Root\"\n"
      "    }\n"
      "  }\n"
      ") { float x }\n");
  assert(multi_result.success);
  AttributeEval multi_eval(&multi_result.stage);
  EvalOptions multi_options;
  multi_options.time = TimeQuery::Numeric(0.0);
  multi_options.clip_stage_loader =
      [](const std::string& asset, Stage* out, std::string*, std::string*) {
        const float value = asset == "strong.usda" ? 2.0f : 1.0f;
        LoadResult clip = Parse(
            "def Xform \"Root\" { float x = " + std::to_string(value) +
            " }\n");
        if (!clip.success || !out) return false;
        *out = std::move(clip.stage);
        return true;
      };
  EvalResult multi = multi_eval.EvalWith(
      multi_result.stage.GetPrimAtPath("/Root"), "x", multi_options);
  assert(multi.success && multi.source_clip_set == "zWeak" &&
         multi.source_asset == "weak.usda" && multi.value.as_float() &&
         *multi.value.as_float() == 1.0f);

  const StringListOpEdits& clip_sets =
      multi_result.stage.GetPrimAtPath("/Root").GetMeta().clipSetEdits();
  assert(clip_sets.authored && clip_sets.is_explicit &&
         clip_sets.explicit_items ==
             std::vector<std::string>({"zWeak", "aStrong"}));
  const std::string clip_sets_usda = WriteUSDAToString(multi_result.stage);
  assert(clip_sets_usda.find(
             "clipSets = [\"zWeak\", \"aStrong\"]") !=
         std::string::npos);
  std::vector<uint8_t> clip_sets_usdc;
  assert(WriteUSDCToMemory(clip_sets_usdc, multi_result.stage).success);
  USDCLoadResult clip_sets_back =
      LoadUSDCFromMemory(clip_sets_usdc.data(), clip_sets_usdc.size());
  assert(clip_sets_back.success);
  const StringListOpEdits& clip_sets_back_edits =
      clip_sets_back.stage.GetPrimAtPath("/Root").GetMeta().clipSetEdits();
  assert(clip_sets_back_edits.authored && clip_sets_back_edits.is_explicit &&
         clip_sets_back_edits.explicit_items == clip_sets.explicit_items);

  LoadResult clip_ops = Parse(
      "def Scope \"Empty\" ( clipSets = None ) {}\n"
      "def Scope \"Ops\" (\n"
      "  add clipSets = [\"add\"]\n"
      "  prepend clipSets = [\"pre\"]\n"
      "  append clipSets = [\"app\"]\n"
      "  delete clipSets = [\"del\"]\n"
      "  reorder clipSets = [\"ord\"]\n"
      ") {}\n",
      true);
  assert(clip_ops.success);
  const StringListOpEdits& empty_clip_sets =
      clip_ops.stage.GetPrimAtPath("/Empty").GetMeta().clipSetEdits();
  assert(empty_clip_sets.authored && empty_clip_sets.is_explicit &&
         empty_clip_sets.explicit_items.empty());
  assert(WriteUSDAToString(clip_ops.stage).find("clipSets = None") !=
         std::string::npos);
  const StringListOpEdits& ops =
      clip_ops.stage.GetPrimAtPath("/Ops").GetMeta().clipSetEdits();
  assert(ops.authored && !ops.is_explicit &&
         ops.added == std::vector<std::string>{"add"} &&
         ops.prepended == std::vector<std::string>{"pre"} &&
         ops.appended == std::vector<std::string>{"app"} &&
         ops.deleted == std::vector<std::string>{"del"} &&
         ops.ordered == std::vector<std::string>{"ord"});
  std::vector<uint8_t> clip_ops_usdc;
  assert(WriteUSDCToMemory(clip_ops_usdc, clip_ops.stage).success);
  USDCLoadResult clip_ops_back =
      LoadUSDCFromMemory(clip_ops_usdc.data(), clip_ops_usdc.size());
  assert(clip_ops_back.success);
  const StringListOpEdits& back_empty =
      clip_ops_back.stage.GetPrimAtPath("/Empty").GetMeta().clipSetEdits();
  assert(back_empty.authored && back_empty.is_explicit &&
         back_empty.explicit_items.empty());
  const StringListOpEdits& back_ops =
      clip_ops_back.stage.GetPrimAtPath("/Ops").GetMeta().clipSetEdits();
  assert(back_ops.added == ops.added &&
         back_ops.prepended == ops.prepended &&
         back_ops.appended == ops.appended &&
         back_ops.deleted == ops.deleted &&
         back_ops.ordered == ops.ordered);

  LoadResult weak_clip_sets = Parse(
      "def Xform \"Root\" (\n"
      "  clipSets = [\"weakB\", \"weakA\"]\n"
      "  clips = {\n"
      "    dictionary weakA = { asset[] assetPaths = [@a.usda@] "
      "double2[] active = [(0, 0)] }\n"
      "    dictionary weakB = { asset[] assetPaths = [@b.usda@] "
      "double2[] active = [(0, 0)] }\n"
      "  }\n"
      ") {}\n");
  LoadResult strong_clip_sets = Parse(
      "def Xform \"Root\" (\n"
      "  prepend references = @weak.usda@</Root>\n"
      "  prepend clipSets = [\"strong\"]\n"
      "  clips = { dictionary strong = {\n"
      "    asset[] assetPaths = [@strong.usda@]\n"
      "    double2[] active = [(0, 0)]\n"
      "  } }\n"
      ") {}\n");
  assert(weak_clip_sets.success && strong_clip_sets.success);
  std::unique_ptr<Layer> weak_clip_layer = TakeLayer(&weak_clip_sets);
  std::unique_ptr<Layer> strong_clip_layer = TakeLayer(&strong_clip_sets);
  Compositor clip_compositor;
  clip_compositor.SetLayerLoader(
      [&](const std::string& path, std::string*) -> std::unique_ptr<Layer> {
        if (path.find("weak.usda") == std::string::npos) return nullptr;
        return std::make_unique<Layer>(weak_clip_layer->Clone());
      });
  std::unique_ptr<Layer> composed_clip_sets =
      clip_compositor.Compose(*strong_clip_layer, "strong.usda");
  assert(composed_clip_sets);
  const PrimSpec* composed_root =
      composed_clip_sets->prim_at_path("/Root");
  assert(composed_root);
  const StringListOpEdits& composed_order =
      composed_root->meta().clipSetEdits();
  assert(composed_order.authored && composed_order.is_explicit &&
         composed_order.explicit_items ==
             std::vector<std::string>({"strong", "weakB", "weakA"}));

  LoadResult invalid_clip_result = Parse(
      "def Xform \"Root\" ( clips = { dictionary bad = {\n"
      "  asset[] assetPaths = [@only.usda@]\n"
      "  double2[] active = [(0, 2)]\n"
      "} } ) { float x }\n");
  assert(invalid_clip_result.success);
  AttributeEval invalid_clip_eval(&invalid_clip_result.stage);
  EvalOptions invalid_options;
  invalid_options.time = TimeQuery::Numeric(0.0);
  invalid_options.strict_aousd_conformance = true;
  invalid_options.clip_stage_loader = multi_options.clip_stage_loader;
  EvalResult invalid_clip = invalid_clip_eval.EvalWith(
      invalid_clip_result.stage.GetPrimAtPath("/Root"), "x",
      invalid_options);
  assert(!invalid_clip.success &&
         invalid_clip.error.find("out of range") != std::string::npos);

  LoadResult cyclic_clips = Parse(
      "def Xform \"Root\" ( clips = { dictionary default = {\n"
      "  asset[] assetPaths = [@loop.usda@]\n"
      "  double2[] active = [(0, 0)]\n"
      "  string primPath = \"/Root\"\n"
      "} } ) { float x }\n");
  assert(cyclic_clips.success);
  AttributeEval cyclic_eval(&cyclic_clips.stage);
  EvalOptions cyclic_options;
  cyclic_options.time = TimeQuery::Numeric(0.0);
  cyclic_options.strict_aousd_conformance = true;
  cyclic_options.clip_stage_cache = std::make_shared<ValueClipStageCache>();
  cyclic_options.clip_stage_loader =
      [](const std::string& asset, Stage* out, std::string*, std::string*) {
        if (asset != "loop.usda" || !out) return false;
        LoadResult loop = Parse(
            "def Xform \"Root\" ( clips = { dictionary default = {\n"
            "  asset[] assetPaths = [@loop.usda@]\n"
            "  double2[] active = [(0, 0)]\n"
            "  string primPath = \"/Root\"\n"
            "} } ) { float x }\n");
        if (!loop.success) return false;
        *out = std::move(loop.stage);
        return true;
      };
  EvalResult cyclic = cyclic_eval.EvalWith(
      cyclic_clips.stage.GetPrimAtPath("/Root"), "x", cyclic_options);
  assert(!cyclic.success &&
         cyclic.error.find("cycle detected") != std::string::npos);

  clip_options.clip_stage_loader = {};
  EvalResult strict_missing_loader = clip_eval.EvalWith(
      root_result.stage.GetPrimAtPath("/Root"), "x", clip_options);
  assert(!strict_missing_loader.success &&
         strict_missing_loader.error.find("clip_stage_loader") !=
             std::string::npos);

  // Manifest gating + interpolateMissingClipValues: clipB has no opinion
  // for x, so the value comes from the neighboring clipA; y is NOT
  // declared in the manifest so it must not resolve through clips.
  LoadResult manifest_result = Parse(
      "def Xform \"Root\" (\n"
      "  clips = {\n"
      "    dictionary default = {\n"
      "      double2[] active = [(0, 0), (10, 1)]\n"
      "      asset[] assetPaths = [@clipA.usda@, @clipB.usda@]\n"
      "      string primPath = \"/Root\"\n"
      "      asset manifestAssetPath = @manifest.usda@\n"
      "      bool interpolateMissingClipValues = true\n"
      "      double2[] times = [(0, 0), (20, 20)]\n"
      "    }\n"
      "  }\n"
      ") { float x\n  float y\n }\n");
  assert(manifest_result.success);
  AttributeEval m_eval(&manifest_result.stage);
  EvalOptions m_opts;
  m_opts.time = 12.0;  // clipB active
  m_opts.clip_stage_loader =
      [](const std::string& asset, Stage* out, std::string*, std::string*) {
        const char* text = nullptr;
        if (asset == "clipA.usda") {
          // visibility is a SCHEMA FALLBACK of Xform (Imageable). The gate must
          // still exclude it because it is not authored in the manifest — a
          // HasProperty()-based gate would wrongly let it through.
          text = "def Xform \"Root\" {\n"
                 "  float x.timeSamples = { 0: 7, 20: 7 }\n"
                 "  float y.timeSamples = { 0: 9, 20: 9 }\n"
                 "  token visibility.timeSamples = "
                 "{ 0: \"invisible\", 20: \"invisible\" }\n"
                 "}\n";
        } else if (asset == "clipB.usda") {
          text = "def Xform \"Root\" { }\n";  // no opinion for x
        } else if (asset == "manifest.usda") {
          text = "def Xform \"Root\" { float x }\n";  // declares x only
        } else {
          return false;
        }
        LoadResult clip = Parse(text);
        if (!clip.success || !out) return false;
        *out = std::move(clip.stage);
        return true;
      };
  EvalResult from_neighbor = m_eval.EvalWith(
      manifest_result.stage.GetPrimAtPath("/Root"), "x", m_opts);
  assert(from_neighbor.success && from_neighbor.source_asset == "clipA.usda");
  assert(from_neighbor.value.as_float() &&
         *from_neighbor.value.as_float() == 7.0f);
  // y is not in the manifest: no clip resolution (falls back to no value).
  EvalResult gated = m_eval.EvalWith(
      manifest_result.stage.GetPrimAtPath("/Root"), "y", m_opts);
  assert(!(gated.success && gated.source_asset == "clipA.usda") &&
         "manifest must gate undeclared properties out of clip resolution");
  // visibility is a schema fallback authored in the clip but NOT in the
  // manifest: it must NOT resolve through clips (guards the HasProperty ->
  // authored-spec gating fix; a HasProperty gate would leak it in).
  EvalResult gated_fallback = m_eval.EvalWith(
      manifest_result.stage.GetPrimAtPath("/Root"), "visibility", m_opts);
  assert(!(gated_fallback.success &&
           gated_fallback.source_asset == "clipA.usda") &&
         "manifest gate must exclude schema-fallback properties too");

  // interpolateMissingClipValues must INTERPOLATE (not hold) between the
  // nearest earlier and later clips that carry a value. Three clips, empty
  // middle: A holds 0 at t=0, C holds 20 at t=20; querying t=12 (empty B
  // active) must yield 12.0 (pxr valueClips.md worked example), not 0.0.
  LoadResult interp_result = Parse(
      "def Xform \"Root\" (\n"
      "  clips = {\n"
      "    dictionary default = {\n"
      "      double2[] active = [(0, 0), (10, 1), (20, 2)]\n"
      "      asset[] assetPaths = [@a.usda@, @b.usda@, @c.usda@]\n"
      "      string primPath = \"/Root\"\n"
      "      bool interpolateMissingClipValues = true\n"
      "      double2[] times = [(0, 0), (30, 30)]\n"
      "    }\n"
      "  }\n"
      ") { double v }\n");
  assert(interp_result.success);
  AttributeEval i_eval(&interp_result.stage);
  EvalOptions i_opts;
  i_opts.time = 12.0;  // clip b (empty) active
  i_opts.clip_stage_loader =
      [](const std::string& asset, Stage* out, std::string*, std::string*) {
        const char* text = nullptr;
        if (asset == "a.usda")
          text = "def Xform \"Root\" { double v.timeSamples = { 0: 0 } }\n";
        else if (asset == "b.usda")
          text = "def Xform \"Root\" { }\n";  // empty
        else if (asset == "c.usda")
          text = "def Xform \"Root\" { double v.timeSamples = { 20: 20 } }\n";
        else
          return false;
        LoadResult clip = Parse(text);
        if (!clip.success || !out) return false;
        *out = std::move(clip.stage);
        return true;
      };
  EvalResult interp = i_eval.EvalWith(
      interp_result.stage.GetPrimAtPath("/Root"), "v", i_opts);
  assert(interp.success && interp.value.as_double());
  assert(std::fabs(*interp.value.as_double() - 12.0) < 1e-9 &&
         "missing clip value must interpolate to 12.0, not hold 0.0");

  // Vectors (and other linear types) must interpolate component-wise, not
  // hold — the common animated float3 case. Same clip layout, float3 p:
  // a@0=(0,0,0), c@20=(20,40,60) -> t=12 -> (12,24,36).
  LoadResult vinterp_result = Parse(
      "def Xform \"Root\" (\n"
      "  clips = {\n"
      "    dictionary default = {\n"
      "      double2[] active = [(0, 0), (10, 1), (20, 2)]\n"
      "      asset[] assetPaths = [@a.usda@, @b.usda@, @c.usda@]\n"
      "      string primPath = \"/Root\"\n"
      "      bool interpolateMissingClipValues = true\n"
      "      double2[] times = [(0, 0), (30, 30)]\n"
      "    }\n"
      "  }\n"
      ") { float3 p }\n");
  assert(vinterp_result.success);
  AttributeEval v_eval(&vinterp_result.stage);
  EvalOptions v_opts;
  v_opts.time = 12.0;
  v_opts.clip_stage_loader =
      [](const std::string& asset, Stage* out, std::string*, std::string*) {
        const char* text = nullptr;
        if (asset == "a.usda")
          text = "def Xform \"Root\" { float3 p.timeSamples = { 0: (0,0,0) } }\n";
        else if (asset == "b.usda")
          text = "def Xform \"Root\" { }\n";
        else if (asset == "c.usda")
          text =
              "def Xform \"Root\" { float3 p.timeSamples = { 20: (20,40,60) } }\n";
        else
          return false;
        LoadResult clip = Parse(text);
        if (!clip.success || !out) return false;
        *out = std::move(clip.stage);
        return true;
      };
  EvalResult vinterp = v_eval.EvalWith(
      vinterp_result.stage.GetPrimAtPath("/Root"), "p", v_opts);
  assert(vinterp.success && vinterp.value.as_float3());
  const float* vp = vinterp.value.as_float3();
  assert(std::fabs(vp[0] - 12.0f) < 1e-4f && std::fabs(vp[1] - 24.0f) < 1e-4f &&
         std::fabs(vp[2] - 36.0f) < 1e-4f &&
         "float3 missing clip value must interpolate component-wise");
}

void TestRelationshipVariabilityFidelity() {
  LoadResult loaded = Parse(
      "def Xform \"P\" { varying rel r = </P> }\n");
  assert(loaded.success && loaded.stage.GetRootLayer());
  const PrimSpec* prim = loaded.stage.GetRootLayer()->prim_at_path("/P");
  assert(prim);
  uint16_t flags = prim->relationship_flags("r");
  assert((flags & PropSlot::kFlagVariabilityAuthored) != 0);
  assert((flags & PropSlot::kFlagVarying) != 0);
  assert(WriteUSDAToString(loaded.stage).find("varying rel r") !=
         std::string::npos);

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, loaded.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success && back.stage.GetRootLayer());
  const PrimSpec* back_prim = back.stage.GetRootLayer()->prim_at_path("/P");
  assert(back_prim);
  flags = back_prim->relationship_flags("r");
  assert((flags & PropSlot::kFlagVariabilityAuthored) != 0);
  assert((flags & PropSlot::kFlagVarying) != 0);
  assert(WriteUSDAToString(back.stage).find("varying rel r") !=
         std::string::npos);
}

void TestLayerOwnerFidelity() {
  LoadResult loaded = LoadUSDAFromString(
      "#usda 1.0\n( owner = \"\" )\ndef Scope \"P\" {}\n",
      LoadOptions{});
  assert(loaded.success && loaded.stage.GetRootLayer());
  assert(loaded.stage.GetRootLayer()->meta().owner_set);
  assert(loaded.stage.GetRootLayer()->meta().owner.empty());
  assert(WriteUSDAToString(loaded.stage).find("owner = \"\"") !=
         std::string::npos);

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, loaded.stage).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success && back.stage.GetRootLayer());
  assert(back.stage.GetRootLayer()->meta().owner_set);
  assert(back.stage.GetRootLayer()->meta().owner.empty());
  assert(WriteUSDAToString(back.stage).find("owner = \"\"") !=
         std::string::npos);
}

void TestGeneratedCoreSchemaCoverage() {
  LoadResult parsed = Parse(
      "def Scope \"P\" (prepend apiSchemas = [\"ColorSpaceDefinitionAPI\", "
      "\"ColorSpaceAPI\", \"CollectionAPI:rooms\"]) {}\n");
  assert(parsed.success);
  UsdPrim prim = parsed.stage.GetPrimAtPath("/P");
  const SchemaRegistry& registry = GetSchemaRegistry();
  const SchemaPropertyDefinition* color =
      registry.FindProperty(*prim.GetPrimSpec(), "whitePoint");
  assert(color && color->has_fallback && color->type_name == "float2");
  const SchemaPropertyDefinition* collection = registry.FindProperty(
      *prim.GetPrimSpec(), "collection:rooms:expansionRule");
  assert(collection && collection->has_fallback &&
         collection->fallback.as_token() &&
         *collection->fallback.as_token() == "expandPrims");
  const std::vector<std::string> names = registry.PropertyNames(
      *prim.GetPrimSpec());
  assert(std::find(names.begin(), names.end(),
                   "collection:rooms:includes") != names.end());
  assert(std::find(names.begin(), names.end(),
                   "collection:__INSTANCE__:includes") == names.end());
}

void TestExpressionVariablePolicy() {
  LoadResult root_result = Parse(
      "( expressionVariables = { string TARGET = \"usd-anon:target\" } )\n"
      "def Xform \"Root\" (references = @`${TARGET}`@) {}\n");
  assert(root_result.success);
  std::shared_ptr<Layer> root(root_result.stage.ReleaseRootLayer().release());
  assert(root->meta().expressionVariables.is_dictionary());
  const PrimSpec* root_spec = root->prim_at_path("/Root");
  assert(root_spec && root_spec->meta().references.size() == 1);
  const ExpressionEvaluation direct = EvaluateAssetPathExpression(
      root_spec->meta().references[0].substr(
          1, root_spec->meta().references[0].size() - 2),
      root->meta().expressionVariables);
  assert(direct.success);

  AssetResolver resolver;
  const std::string target =
      "#usda 1.0\n( defaultPrim = \"Target\" )\n"
      "def Xform \"Target\" { int marker = 7 }\n";
  resolver.RegisterMemoryAsset(
      "usd-anon:target", std::vector<uint8_t>(target.begin(), target.end()));
  assert(resolver.Resolve("usd-anon:target", "usd-anon:root").exists);
  pcp::CompositionOptions options;
  options.expression_variable_policy = ExpressionVariablePolicy::RequireResolved;
  auto opened = pcp::Cache::Open(resolver, root, "usd-anon:root", options);
  assert(opened);
  const pcp::PrimIndex* index =
      opened->ComputePrimIndex(Path("/Root"), nullptr, nullptr);
  assert(index && index->GetNodeCount() >= 2);
  Stage composed;
  std::string warn;
  std::string err;
  assert(opened->BuildStage(&composed, &warn, &err));
  assert(composed.GetPrimAtPath("/Root").IsValid());
  const Value* marker = composed.GetPrimAtPath("/Root").GetPropertyValue("marker");
  assert(marker && marker->as_int() && *marker->as_int() == 7);

  LoadResult missing_result = Parse(
      "def Xform \"Root\" (references = @`${MISSING}`@) {}\n");
  assert(missing_result.success);
  std::shared_ptr<Layer> missing(
      missing_result.stage.ReleaseRootLayer().release());
  auto rejected = pcp::Cache::Open(resolver, missing, "usd-anon:missing", options);
  assert(rejected);
  Stage rejected_stage;
  assert(rejected->BuildStage(&rejected_stage, &warn, &err));
  const auto issues = rejected->GetCompositionIssues();
  assert(std::find_if(issues.begin(), issues.end(), [](const auto& issue) {
           return issue.code == pcp::Cache::ErrorCode::ExpressionVariableError;
         }) != issues.end());
}

// SdfVariableExpression function-language grammar: typed literals, the
// function set, string interpolation + escapes, recursive variable
// evaluation with cycle detection, and error/None propagation.
// Authored-state bits for the previously structural-only fields: explicit
// empty vs unauthored must be distinguishable and must round-trip through
// USDA and USDC for layer relocates/subLayers and prim relocates/variants.
void TestAuthoredStateBits() {
  // Explicit-empty authored state.
  {
    LoadResult r = Parse(
        "(\n"
        "    relocates = {}\n"
        "    subLayers = []\n"
        ")\n"
        "def Xform \"P\" (\n"
        "    relocates = {}\n"
        "    variants = {}\n"
        ") {}\n");
    assert(r.success);
    const Layer* layer = r.stage.GetRootLayer();
    assert(layer->meta().relocates_set && layer->meta().relocates.empty());
    assert(layer->meta().subLayers_set && layer->meta().subLayers.empty());
    const PrimSpec* prim = layer->prim_at_path("/P");
    assert(prim);
    assert(prim->meta().relocatesAuthored() && prim->meta().relocates().empty());
    assert(prim->meta().variantSelectionsAuthored() &&
           prim->meta().variantSelections().empty());

    const std::string usda = WriteUSDAToString(r.stage);
    assert(usda.find("relocates = {}") != std::string::npos);
    assert(usda.find("subLayers = []") != std::string::npos);
    assert(usda.find("variants = {}") != std::string::npos);
    // The USDA spelling itself re-parses to the same authored state.
    LoadResult again = LoadUSDAFromString(usda, LoadOptions{});
    assert(again.success);
    assert(again.stage.GetRootLayer()->meta().relocates_set);
    assert(again.stage.GetRootLayer()->meta().subLayers_set);

    std::vector<uint8_t> crate;
    assert(WriteUSDCToMemory(crate, r.stage, USDCWriteOptions{}).success);
    USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
    assert(back.success);
    const Layer* blayer = back.stage.GetRootLayer();
    assert(blayer->meta().relocates_set && blayer->meta().relocates.empty());
    assert(blayer->meta().subLayers_set && blayer->meta().subLayers.empty());
    const PrimSpec* bprim = blayer->prim_at_path("/P");
    assert(bprim && bprim->meta().relocatesAuthored() &&
           bprim->meta().relocates().empty());
    assert(bprim->meta().variantSelectionsAuthored() &&
           bprim->meta().variantSelections().empty());
  }
  // Unauthored: no bits, no spurious emission.
  {
    LoadResult r = Parse("def Xform \"P\" {}\n");
    assert(r.success);
    const Layer* layer = r.stage.GetRootLayer();
    assert(!layer->meta().relocates_set && !layer->meta().subLayers_set);
    const PrimSpec* prim = layer->prim_at_path("/P");
    assert(prim && !prim->meta().relocatesAuthored() &&
           !prim->meta().variantSelectionsAuthored());
    const std::string usda = WriteUSDAToString(r.stage);
    assert(usda.find("relocates") == std::string::npos);
    assert(usda.find("subLayers") == std::string::npos);
    assert(usda.find("variants") == std::string::npos);
  }
}

// Registered string list-op field table (listop-field-table.hh): the shared
// stronger-over-weaker merge drives apiSchemas / variantSetNames / clipSets
// uniformly, including apiSchemas' legacy qualifier normalization.
void TestStringListOpFieldTable() {
  size_t count = 0;
  const StringListOpFieldDef* fields = GetStringListOpFieldTable(&count);
  assert(count == 3);
  const StringListOpFieldDef* api = nullptr;
  const StringListOpFieldDef* vsets = nullptr;
  const StringListOpFieldDef* clips = nullptr;
  for (size_t i = 0; i < count; ++i) {
    if (std::string(fields[i].name) == "apiSchemas") api = &fields[i];
    if (std::string(fields[i].name) == "variantSetNames") vsets = &fields[i];
    if (std::string(fields[i].name) == "clipSets") clips = &fields[i];
  }
  assert(api && vsets && clips);

  // apiSchemas: stronger prepend+delete edits over a weaker bare list.
  {
    PrimSpecMeta weaker;
    weaker.apiSchemas() = {"KeepAPI", "DropAPI"};
    weaker.setApiSchemasAuthored();
    PrimSpecMeta stronger;
    StringListOpEdits& e = stronger.apiSchemaEdits();
    e.authored = true;
    e.is_explicit = false;
    e.prepended = {"NewAPI"};
    e.deleted = {"DropAPI"};
    MergeWeakerStringListOpField(*api, weaker, stronger);
    const std::vector<std::string> expected = {"NewAPI", "KeepAPI"};
    assert(stronger.apiSchemas() == expected);
    assert(stronger.apiSchemaEdits().is_explicit &&
           stronger.apiSchemaEdits().explicit_items == expected);
  }
  // apiSchemas: legacy qualifier (no edits) normalizes and merges.
  {
    PrimSpecMeta weaker;
    weaker.apiSchemas() = {"WeakAPI"};
    weaker.setApiSchemasAuthored();
    PrimSpecMeta stronger;
    stronger.apiSchemas() = {"StrongAPI"};
    stronger.setApiSchemasAuthored();
    stronger.apiSchemasQualifier() = "append";
    MergeWeakerStringListOpField(*api, weaker, stronger);
    const std::vector<std::string> expected = {"WeakAPI", "StrongAPI"};
    assert(stronger.apiSchemas() == expected);
    assert(stronger.apiSchemasQualifier().empty() &&
           "legacy qualifier resolves into an explicit edit");
  }
  // variantSetNames: stronger prepend MERGES over the weaker declaration
  // (was fill-absent-only, dropping the weaker set name).
  {
    PrimSpecMeta weaker;
    StringListOpEdits& we = weaker.variantSetNameEdits();
    we.authored = true;
    we.is_explicit = false;
    we.prepended = {"vsWeak"};
    PrimSpecMeta stronger;
    StringListOpEdits& se = stronger.variantSetNameEdits();
    se.authored = true;
    se.is_explicit = false;
    se.prepended = {"vsStrong"};
    MergeWeakerStringListOpField(*vsets, weaker, stronger);
    const std::vector<std::string> expected = {"vsStrong", "vsWeak"};
    assert(stronger.variantSetNameEdits().is_explicit &&
           stronger.variantSetNameEdits().explicit_items == expected);
  }
  // clipSets: stronger reorder over the weaker clips dictionary's name order.
  {
    PrimSpecMeta weaker;
    Value wclips = Value::MakeDictionary();
    wclips.as_dictionary()->set("setA", Value::MakeDictionary());
    wclips.as_dictionary()->set("setB", Value::MakeDictionary());
    weaker.clips() = std::move(wclips);
    PrimSpecMeta stronger;
    StringListOpEdits& ce = stronger.clipSetEdits();
    ce.authored = true;
    ce.is_explicit = false;
    ce.ordered = {"setB", "setA"};
    MergeWeakerStringListOpField(*clips, weaker, stronger);
    const std::vector<std::string> expected = {"setB", "setA"};
    assert(stronger.clipSetEdits().is_explicit &&
           stronger.clipSetEdits().explicit_items == expected);
  }
}

// Variant-scope generic field storage: unknown (unmodeled) variant-option
// metadata survives USDA re-emission verbatim and a USDC round trip (via the
// materialized holder prim's unknownMeta/unknownFields).
void TestVariantExtensionFields() {
  const std::string body =
      "def Xform \"P\" (\n"
      "    prepend variantSets = \"look\"\n"
      "    variants = { string look = \"red\" }\n"
      ") {\n"
      "    variantSet \"look\" = {\n"
      "        \"red\" (\n"
      "            customPipelineTag = \"hero\"\n"
      "        ) {\n"
      "            int c = 1\n"
      "        }\n"
      "        \"blue\" {\n"
      "            int c = 2\n"
      "        }\n"
      "    }\n"
      "}\n";
  LoadResult r = Parse(body);
  assert(r.success);
  const PrimSpec* prim = r.stage.GetRootLayer()->prim_at_path("/P");
  assert(prim);
  const VariantData* red = nullptr;
  for (const VariantSetData& vs : prim->meta().variantSets()) {
    for (const VariantData& vd : vs.variants) {
      if (vd.name == "red") red = &vd;
    }
  }
  assert(red && red->unknownMeta.size() == 1);
  assert(red->unknownMeta[0].first == "customPipelineTag" &&
         red->unknownMeta[0].second == "\"hero\"");

  const std::string usda = WriteUSDAToString(r.stage);
  assert(usda.find("customPipelineTag = \"hero\"") != std::string::npos &&
         "variant-option unknown metadata must re-emit verbatim");
  // The spelling itself round-trips.
  LoadResult again = LoadUSDAFromString(usda, LoadOptions{});
  assert(again.success);

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, r.stage, USDCWriteOptions{}).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const std::string usdc_usda = WriteUSDAToString(back.stage);
  assert(usdc_usda.find("customPipelineTag") != std::string::npos &&
         "variant-option unknown metadata must survive USDC");
}

void TestVariableExpressionGrammar() {
  Value vars = Value::MakeDictionary();
  Dict* d = vars.as_dictionary();
  d->set("A", Value(std::string("v1")));
  d->set("B", Value(std::string("v2")));
  d->set("N", Value(int32_t(3)));
  d->set("FLAG", Value(true));
  d->set("EXPR", Value(std::string("`\"pre_${A}\"`")));
  d->set("CYC1", Value(std::string("`${CYC2}`")));
  d->set("CYC2", Value(std::string("`${CYC1}`")));

  const auto eval = [&](const std::string& body) {
    return EvaluateAssetPathExpression("`" + body + "`", vars);
  };
  const auto value_of = [&](const std::string& body) {
    const ExpressionEvaluation r = eval(body);
    assert(r.is_expression && r.success && !r.is_none);
    return r.value;
  };
  const auto fails = [&](const std::string& body) {
    const ExpressionEvaluation r = eval(body);
    return r.is_expression && !r.success;
  };
  const auto is_none = [&](const std::string& body) {
    const ExpressionEvaluation r = eval(body);
    return r.is_expression && r.success && r.is_none;
  };
  const auto bool_of = [&](const std::string& body) {
    // Booleans surface through if(): the asset-path wrapper requires string.
    return value_of("if(" + body + ", \"T\", \"F\")");
  };

  // Non-expressions pass through untouched.
  assert(!EvaluateAssetPathExpression("./plain.usda", vars).is_expression);

  // Variables, quoted strings, interpolation, escapes.
  assert(value_of("${A}") == "v1");
  assert(value_of("\"x_${A}_${B}\"") == "x_v1_v2");
  assert(value_of("'single_${A}'") == "single_v1");
  assert(value_of("\"a\\${A}\"") == "a${A}");   // escaped interpolation
  assert(value_of("'don\\'t'") == "don't");
  assert(fails("\"unterminated"));
  assert(fails("${UNDEFINED}"));
  assert(fails("${A} trailing"));

  // Typed literals. Non-string results are type errors for asset paths;
  // None is "no opinion".
  assert(fails("42"));
  assert(fails("True"));
  assert(is_none("None"));
  assert(fails("99999999999999999999"));  // int64 overflow
  assert(fails("[\"a\", [\"nested\"]]"));  // nested lists

  // if / and / or / not.
  assert(value_of("if(True, \"a\")") == "a");
  assert(is_none("if(False, \"a\")"));
  assert(value_of("if(False, \"a\", \"b\")") == "b");
  assert(value_of("if(${FLAG}, \"y\", \"n\")") == "y");
  assert(bool_of("and(True, True)") == "T");
  assert(bool_of("and(True, False)") == "F");
  assert(bool_of("or(False, True)") == "T");
  assert(bool_of("not(False)") == "T");
  assert(fails("if(\"notbool\", \"a\")"));
  assert(fails("and(True)"));  // arity

  // Comparisons.
  assert(bool_of("eq(${A}, \"v1\")") == "T");
  assert(bool_of("neq(${A}, ${B})") == "T");
  assert(bool_of("eq(${N}, 3)") == "T");
  assert(bool_of("lt(2, 3)") == "T");
  assert(bool_of("leq(3, 3)") == "T");
  assert(bool_of("gt(\"b\", \"a\")") == "T");
  assert(bool_of("geq(${N}, 3)") == "T");
  assert(fails("eq(1, \"one\")"));  // type mismatch
  assert(fails("lt(True, False)"));

  // contains / at / len over lists and strings.
  assert(bool_of("contains([\"a\", \"b\"], \"b\")") == "T");
  assert(bool_of("contains([1, 2, 3], ${N})") == "T");
  assert(bool_of("contains(\"hello\", \"ell\")") == "T");
  assert(value_of("at([\"a\", \"b\", \"c\"], 1)") == "b");
  assert(value_of("at([\"a\", \"b\", \"c\"], -1)") == "c");
  assert(value_of("at(\"xyz\", 0)") == "x");
  assert(fails("at([\"a\"], 5)"));
  assert(bool_of("eq(len([\"a\", \"b\"]), 2)") == "T");
  assert(bool_of("eq(len(${A}), 2)") == "T");

  // defined().
  assert(bool_of("defined(\"A\")") == "T");
  assert(bool_of("defined(\"A\", \"MISSING\")") == "F");

  // Nested calls and whitespace tolerance.
  assert(value_of("if( and( eq(${A}, \"v1\"), not(False) ), ${B}, \"no\" )") ==
         "v2");

  // Recursive variable evaluation + cycle detection.
  assert(value_of("${EXPR}") == "pre_v1");
  assert(fails("${CYC1}"));

  // Unknown function / garbage.
  assert(fails("nosuchfn(1)"));
  assert(fails("@!!"));
}

void TestRemainingElectiveFieldCoverage() {
  struct FieldCoverage { const char* scope; const char* name; const char* mode; };
  const FieldCoverage generated_fields[] = {
#define AOUSD_FIELD(scope, name, coverage) {#scope, #name, #coverage},
#include "next/schema/generated/aousd-elective-field-coverage.inc"
#undef AOUSD_FIELD
  };
  assert(sizeof(generated_fields) / sizeof(generated_fields[0]) >= 70);
  auto covered = [&](const char* scope, const char* name, const char* mode) {
    return std::find_if(std::begin(generated_fields), std::end(generated_fields),
                        [&](const FieldCoverage& field) {
                          return std::string(field.scope) == scope &&
                                 field.name == std::string(name) &&
                                 field.mode == std::string(mode);
                        }) != std::end(generated_fields);
  };
  assert(covered("Prim", "displayGroupOrder", "Typed"));
  assert(covered("Property", "comment", "Typed"));
  assert(covered("Prim", "prefixSubstitutions", "Opaque"));
  const std::string source =
      "def Xform \"P\" (displayGroupOrder = [\"Geometry\", \"Look\"]) {\n"
      "  int value = 1 (comment = \"\")\n"
      "}\n";
  LoadResult parsed = Parse(source, true);
  assert(parsed.success);
  const UsdPrim prim = parsed.stage.GetPrimAtPath("/P");
  assert(prim.GetMeta().displayGroupOrderAuthored() &&
         prim.GetMeta().displayGroupOrder().size() == 2);
  const PropMeta* meta = prim.GetPrimSpec()->property_meta("value");
  assert(meta && (meta->authored & PropMeta::kComment) && meta->comment.empty());
  const std::string text = WriteUSDAToString(parsed.stage);
  assert(text.find("displayGroupOrder = [\"Geometry\", \"Look\"]") !=
         std::string::npos);
  // Comment round-trips as a BARE empty string literal (pxr spelling).
  assert(text.find("\"\"") != std::string::npos);

  std::vector<uint8_t> crate;
  assert(WriteUSDCToMemory(crate, parsed.stage, USDCWriteOptions{}).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const std::string binary_text = WriteUSDAToString(back.stage);
  assert(binary_text.find("displayGroupOrder") != std::string::npos);
  assert(binary_text.find("\"\"") != std::string::npos);
}

void TestTypedSplines() {
  // A linear spline: value ramps 0->10 over t in [0,10], held outside.
  const std::string body =
      "def Xform \"S\" {\n"
      "  double v.spline = {\n"
      "    bezier,\n"
      "    0: 0; post linear,\n"
      "    10: 10,\n"
      "  }\n"
      "}\n";
  LoadResult res = Parse(body, true);
  assert(res.success);

  AttributeEval eval(&res.stage);
  const UsdPrim s = res.stage.GetPrimAtPath("/S");

  auto sample = [&](double t) -> double {
    EvalOptions o;
    o.time = t;
    EvalResult r = eval.EvalWith(s, "v", o);
    assert(r.success && r.value.as_double());
    return *r.value.as_double();
  };
  assert(std::fabs(sample(0.0) - 0.0) < 1e-9);
  assert(std::fabs(sample(5.0) - 5.0) < 1e-9);
  assert(std::fabs(sample(10.0) - 10.0) < 1e-9);
  assert(std::fabs(sample(-3.0) - 0.0) < 1e-9);   // pre-extrapolation: held
  assert(std::fabs(sample(20.0) - 10.0) < 1e-9);  // post-extrapolation: held

  // Round-trip through USDC (Crate type 59) and re-evaluate.
  USDCWriteOptions usdc_opts;
  usdc_opts.crate_options.strict_aousd_conformance = true;
  std::vector<uint8_t> crate;
  USDCWriteResult wr = WriteUSDCToMemory(crate, res.stage, usdc_opts);
  assert(wr.success && !crate.empty());

  USDCLoadOptions lopts;
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size(), lopts);
  assert(back.success);
  const UsdPrim s2 = back.stage.GetPrimAtPath("/S");
  assert(s2.IsValid());
  AttributeEval eval2(&back.stage);
  EvalOptions o5;
  o5.time = 5.0;
  EvalResult r5 = eval2.EvalWith(s2, "v", o5);
  assert(r5.success && r5.value.as_double() &&
         std::fabs(*r5.value.as_double() - 5.0) < 1e-9);
  // The decoded USDC also re-emits the spline as USDA text.
  const std::string usda_back = WriteUSDAToString(back.stage);
  assert(usda_back.find("v.spline") != std::string::npos);

  // A float-typed spline keeps its declared scalar type through evaluation.
  LoadResult fr = Parse(
      "def Xform \"F\" {\n"
      "  float w.spline = { 0: 1, 4: 5; post linear, }\n"
      "}\n",
      true);
  assert(fr.success);
  AttributeEval feval(&fr.stage);
  EvalOptions fo;
  fo.time = 2.0;
  EvalResult fres =
      feval.EvalWith(fr.stage.GetPrimAtPath("/F"), "w", fo);
  assert(fres.success && fres.value.as_float());

  // Review regressions:
  // (a) half-typed spline values round to the nearest half (round-to-even),
  // matching pxr — a truncating encoder would give 0.299805 for 0.3.
  {
    SplineData sd;
    sd.value_desc = 3;  // half
    SplineKnot k0;
    k0.time = 0;
    k0.value = 0.3;
    k0.interp = 1;
    sd.knots.push_back(k0);
    std::vector<uint8_t> blob;
    std::string err;
    assert(EncodeSplineBinary(sd, &blob, &err));
    SplineData rt;
    assert(DecodeSplineBinary(blob.data(), blob.size(), &rt, &err));
    assert(std::fabs(rt.knots[0].value - 0.300049) < 1e-5 &&
           "half spline value must round to nearest, not truncate");
  }
  // (b) autoEase tangents are recomputed for evaluation (slope 0 at a local
  // max), so the curve peaks exactly at the knot instead of overshooting with
  // the authored placeholder tangents.
  {
    LoadResult ae = Parse(
        "def Xform \"E\" {\n"
        "  double v.spline = {\n"
        "    bezier,\n"
        "    0: 0; post curve (2, 3),\n"
        "    5: 10; pre (2, 9, autoEase); post curve (2, 9, autoEase),\n"
        "    10: 0; pre (2, 3),\n"
        "  }\n"
        "}\n",
        true);
    assert(ae.success);
    AttributeEval aeval(&ae.stage);
    EvalOptions aopts;
    aopts.time = 5.0;
    EvalResult apk = aeval.EvalWith(ae.stage.GetPrimAtPath("/E"), "v", aopts);
    assert(apk.success && apk.value.as_double() &&
           std::fabs(*apk.value.as_double() - 10.0) < 1e-6 &&
           "autoEase knot at a local max must evaluate to its value, not "
           "overshoot");
  }
  // (c) knot customData containing a brace inside a string must not desync the
  // dictionary skip (strict parse must accept it).
  {
    LoadResult cd = Parse(
        "def Xform \"C\" {\n"
        "  double v.spline = { 0: 1; { string s = \"}\" }, 10: 2, }\n"
        "}\n",
        true);
    assert(cd.success &&
           "spline knot customData with a brace in a string must parse");
    const std::string cd_usda = WriteUSDAToString(cd.stage);
    assert(cd_usda.find("string s = \"}\"") != std::string::npos &&
           "spline knot customData must survive USDA writing");
    std::vector<uint8_t> cd_crate;
    assert(WriteUSDCToMemory(cd_crate, cd.stage).success);
    USDCLoadResult cd_back =
        LoadUSDCFromMemory(cd_crate.data(), cd_crate.size());
    assert(cd_back.success);
    const std::string cd_back_usda = WriteUSDAToString(cd_back.stage);
    assert(cd_back_usda.find("string s = \"}\"") != std::string::npos &&
           "spline knot customData must survive USDC round-trip");
    // Triple-quoted string with an embedded quote and brace must not desync.
    LoadResult tq = Parse(
        "def Xform \"T\" {\n"
        "  double v.spline = { 0: 1; { string s = \"\"\"a\"b}\"\"\" }, 10: 2, }\n"
        "}\n",
        true);
    assert(tq.success &&
           "spline customData triple-quoted string with a brace must parse");
  }
}

// AOUSD-TYPE-001 coverage: every supported foundational type must survive a
// USDA -> USDC -> USDA round trip with no value/type loss and no spurious
// structural additions (e.g. a `reorder properties` synthesized from the
// crate `properties` field).
void TestFoundationalTypeMatrix() {
  const std::string body =
      "def Scope \"T\" {\n"
      "    bool b = true\n"
      "    uchar uc = 200\n"
      "    int i = -5\n"
      "    uint ui = 7\n"
      "    int64 i64 = -100000000000\n"
      "    uint64 u64 = 100000000000\n"
      "    half h = 1.5\n"
      "    float f = 2.5\n"
      "    double d = 3.5\n"
      "    timecode tc = 24\n"
      "    string s = \"hi\"\n"
      "    token tok = \"abc\"\n"
      "    asset a = @./tex.png@\n"
      "    int2 i2 = (1, 2)\n"
      "    int3 i3 = (1, 2, 3)\n"
      "    int4 i4 = (1, 2, 3, 4)\n"
      "    uint2 ui2 = (1, 2)\n"
      "    uint3 ui3 = (1, 2, 3)\n"
      "    uint4 ui4 = (1, 2, 3, 4)\n"
      "    half2 h2 = (1.5, 2.5)\n"
      "    half3 h3 = (1.5, 2.5, 3.5)\n"
      "    half4 h4 = (1.5, 2.5, 3.5, 4.5)\n"
      "    float2 f2 = (1.5, 2.5)\n"
      "    float3 f3 = (1.5, 2.5, 3.5)\n"
      "    float4 f4 = (1.5, 2.5, 3.5, 4.5)\n"
      "    double2 d2 = (1.5, 2.5)\n"
      "    double3 d3 = (1.5, 2.5, 3.5)\n"
      "    double4 d4 = (1.5, 2.5, 3.5, 4.5)\n"
      "    point3h p3h = (1, 2, 3)\n"
      "    point3f p3f = (1, 2, 3)\n"
      "    point3d p3d = (1, 2, 3)\n"
      "    normal3h n3h = (0, 1, 0)\n"
      "    normal3f n3f = (0, 1, 0)\n"
      "    normal3d n3d = (0, 1, 0)\n"
      "    vector3h v3h = (1, 0, 0)\n"
      "    vector3f v3f = (1, 0, 0)\n"
      "    vector3d v3d = (1, 0, 0)\n"
      "    color3h c3h = (0.1, 0.2, 0.3)\n"
      "    color3f c3f = (0.1, 0.2, 0.3)\n"
      "    color3d c3d = (0.1, 0.2, 0.3)\n"
      "    color4h c4h = (0.1, 0.2, 0.3, 1)\n"
      "    color4f c4f = (0.1, 0.2, 0.3, 1)\n"
      "    color4d c4d = (0.1, 0.2, 0.3, 1)\n"
      "    texCoord2h uvh = (0.5, 0.5)\n"
      "    texCoord2f uv = (0.5, 0.5)\n"
      "    texCoord2d uvd = (0.5, 0.5)\n"
      "    texCoord3h uv3h = (0.5, 0.5, 0.5)\n"
      "    texCoord3f uv3f = (0.5, 0.5, 0.5)\n"
      "    texCoord3d uv3d = (0.5, 0.5, 0.5)\n"
      "    quatf qf = (1, 0, 0, 0)\n"
      "    quatd qd = (1, 0, 0, 0)\n"
      "    quath qh = (1, 0, 0, 0)\n"
      "    matrix2d m2 = ((1, 0), (0, 1))\n"
      "    matrix3d m3 = ((1, 0, 0), (0, 1, 0), (0, 0, 1))\n"
      "    matrix4d m4 = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))\n"
      "    matrix2f m2f = ((1, 0), (0, 1))\n"
      "    matrix3f m3f = ((1, 0, 0), (0, 1, 0), (0, 0, 1))\n"
      "    matrix4f m4f = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))\n"
      "    frame4d fr = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 2, 1))\n"
      "    pathExpression pe = \"/World//Mesh*\"\n"
      "    int[] ia = [1, 2, 3]\n"
      "    float[] fa = [1.5, 2.5]\n"
      "    double[] da = [1.5, 2.5]\n"
      "    half[] ha = [1.5, 2.5]\n"
      "    timecode[] tca = [1, 2]\n"
      "    uchar[] uca = [1, 2, 200]\n"
      "    token[] toka = [\"a\", \"b\"]\n"
      "    string[] sa = [\"x\", \"y\"]\n"
      "    asset[] aa = [@./p.png@, @./q.png@]\n"
      "    float3[] f3a = [(1, 2, 3), (4, 5, 6)]\n"
      "    point3f[] p3a = [(1, 2, 3), (4, 5, 6)]\n"
      "    point3h[] p3ha = [(1, 2, 3), (4, 5, 6)]\n"
      "    point3d[] p3da = [(1, 2, 3), (4, 5, 6)]\n"
      "    color3f[] c3a = [(0.1, 0.2, 0.3)]\n"
      "    quath[] qha = [(1, 0, 0, 0)]\n"
      "    pathExpression[] pea = [\"/A\", \"/B\"]\n"
      "    matrix4d[] m4a = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))]\n"
      "    bool[] ba = [true, false, true]\n"
      "}\n";
  LoadResult direct = Parse(body, true);
  assert(direct.success);
  // Registry-driven completeness guard: adding a public TypeId now fails this
  // test until the foundational fixture authors it. Structural pseudo-types
  // and Extent (the schema role for float3[]) are intentionally excluded.
  const PrimSpec* type_prim = direct.stage.GetRootLayer()->prim_at_path("/T");
  assert(type_prim);
  for (uint16_t raw = static_cast<uint16_t>(TypeId::Bool);
       raw < static_cast<uint16_t>(TypeId::Count); ++raw) {
    const TypeId id = static_cast<TypeId>(raw);
    if (id == TypeId::Extent || id == TypeId::Dictionary ||
        id == TypeId::Relationship || id == TypeId::Reference) {
      continue;
    }
    const char* expected = GetTypeName(id);
    assert(expected);
    bool authored = false;
    for (const PropSlot& slot : type_prim->properties().slots()) {
      const std::string name(GetPropNameTable().get(slot.name_id));
      const std::string* declared = type_prim->property_type_name(name);
      if (declared && *declared == expected) {
        authored = true;
        break;
      }
    }
    assert(authored && "public foundational TypeId is absent from the matrix");
  }
  const std::string a1 = WriteUSDAToString(direct.stage);

  USDCWriteOptions usdc_opts;
  std::vector<uint8_t> crate;
  USDCWriteResult wr = WriteUSDCToMemory(crate, direct.stage, usdc_opts);
  assert(wr.success);
  USDCLoadOptions lopts;
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size(), lopts);
  assert(back.success);
  const std::string a2 = WriteUSDAToString(back.stage);
  assert(a1 == a2 &&
         "foundational type matrix must survive USDA->USDC->USDA unchanged");

  // An attribute that carries BOTH a default value and a connection must keep
  // the connection through USDC (the connection flag was formerly set only
  // when the connect statement created the slot, so `v = x` + `v.connect = <t>`
  // dropped the connection in the crate).
  LoadResult conn = Parse(
      "def Scope \"C\" {\n"
      "    float v = 100.5\n"
      "    float v.connect = </C.other>\n"
      "    float other = 1\n"
      "}\n",
      true);
  assert(conn.success);
  std::vector<uint8_t> ccrate;
  assert(WriteUSDCToMemory(ccrate, conn.stage, usdc_opts).success);
  USDCLoadResult cback = LoadUSDCFromMemory(ccrate.data(), ccrate.size(), lopts);
  assert(cback.success);
  assert(WriteUSDAToString(cback.stage).find("v.connect = </C.other>") !=
             std::string::npos &&
         "value + connection must both survive USDC");
}

// ---------------------------------------------------------------------------
// AOUSD §6 normative foundational type table, embedded from the supplemental
// corpus `data_types/tests/foundational_data_types.json` (release_dec2025).
// The corpus is intentionally not vendored; the table below is its content
// de-duplicated (the JSON repeats `uint[]` and lists a stray scalar `uint64`
// in the array section). Every scalar named type has a normative `[]` form.
// `dictionary` is a metadata value type (covered in the dictionary context),
// `listop<...>` are metadata list-ops (covered by the list-op fidelity
// tests), and `opaque`/`group` carry no non-block values by definition.
static const char* kNormativeScalars[] = {
    "asset",    "bool",     "double",   "float",    "half",     "int",
    "int64",    "string",   "timecode", "token",    "uchar",    "uint",
    "uint64",   "double2",  "double3",  "double4",  "float2",   "float3",
    "float4",   "half2",    "half3",    "half4",    "int2",     "int3",
    "int4",     "matrix2d", "matrix3d", "matrix4d", "quatd",    "quatf",
    "quath",
};

// Semantic aliases (alias -> underlying); the scalar and `[]` forms are both
// normative. `group -> opaque` is block-only and covered with opaque.
static const char* kNormativeAliases[][2] = {
    {"color3d", "double3"},   {"color3f", "float3"},   {"color3h", "half3"},
    {"color4d", "double4"},   {"color4f", "float4"},   {"color4h", "half4"},
    {"normal3d", "double3"},  {"normal3f", "float3"},  {"normal3h", "half3"},
    {"point3d", "double3"},   {"point3f", "float3"},   {"point3h", "half3"},
    {"vector3d", "double3"},  {"vector3f", "float3"},  {"vector3h", "half3"},
    {"frame4d", "matrix4d"},  {"texCoord2d", "double2"},
    {"texCoord2f", "float2"}, {"texCoord2h", "half2"},
    {"texCoord3d", "double3"},{"texCoord3f", "float3"},
    {"texCoord3h", "half3"},
};

// Two distinct USDA literals per normative type (default vs second sample).
std::string NormativeLiteral(const std::string& type, int variant) {
  const bool second = variant != 0;
  auto tuple = [&](int n, bool integral) {
    std::string out = "(";
    for (int i = 0; i < n; ++i) {
      if (i) out += ", ";
      const int base = i + 1 + (second ? 4 : 0);
      out += integral ? std::to_string(base)
                      : (std::to_string(base) + ".5");
    }
    return out + ")";
  };
  auto matrix = [&](int n) {
    std::string out = "(";
    for (int r = 0; r < n; ++r) {
      if (r) out += ", ";
      out += "(";
      for (int c = 0; c < n; ++c) {
        if (c) out += ", ";
        if (r == c) out += "1";
        else if (second && r == n - 1 && c == 0) out += "2";
        else out += "0";
      }
      out += ")";
    }
    return out + ")";
  };
  if (type == "bool") return second ? "false" : "true";
  if (type == "uchar") return second ? "9" : "200";
  if (type == "int" || type == "uint") return second ? "9" : "7";
  if (type == "int64") return second ? "-9" : "-100000000000";
  if (type == "uint64") return second ? "9" : "100000000000";
  if (type == "half" || type == "float" || type == "double")
    return second ? "2.5" : "1.5";
  if (type == "timecode") return second ? "48" : "24";
  if (type == "string") return second ? "\"there\"" : "\"hi\"";
  if (type == "token") return second ? "\"xyz\"" : "\"abc\"";
  if (type == "asset") return second ? "@./q.png@" : "@./p.png@";
  if (type.compare(0, 6, "matrix") == 0) return matrix(type[6] - '0');
  if (type.compare(0, 4, "quat") == 0)
    return second ? "(0, 1, 0, 0)" : "(1, 0, 0, 0)";
  // Component tuples: trailing digit gives the count; int tuples authored
  // integral, the rest floating.
  const int n = type.back() - '0';
  const bool integral = type.compare(0, 3, "int") == 0;
  return tuple(n, integral);
}

std::string SanitizeTypeIdent(const std::string& type) {
  std::string out;
  for (char c : type) {
    if (c == '[' || c == ']') continue;
    out += c;
  }
  return out;
}

// Parse strict + verify every authored property keeps its declared type name,
// then round-trip USDA -> USDC -> USDA byte-identically.
void CheckNormativeBody(const std::string& body, const char* label) {
  LoadResult direct = Parse(body, true);
  if (!direct.success) {
    std::fprintf(stderr, "normative matrix (%s) failed to parse:\n", label);
    for (const auto& e : direct.errors) {
      std::fprintf(stderr, "  %s\n", e.message.c_str());
    }
    std::fprintf(stderr, "%s\n", body.c_str());
  }
  assert(direct.success);
  const std::string a1 = WriteUSDAToString(direct.stage);
  std::vector<uint8_t> crate;
  USDCWriteOptions usdc_opts;
  assert(WriteUSDCToMemory(crate, direct.stage, usdc_opts).success);
  USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
  assert(back.success);
  const std::string a2 = WriteUSDAToString(back.stage);
  if (a1 != a2) {
    std::fprintf(stderr, "normative matrix (%s) crate round-trip diff:\n", label);
    std::fprintf(stderr, "--- usda\n%s\n--- usda after crate\n%s\n", a1.c_str(),
                 a2.c_str());
  }
  assert(a1 == a2 && "normative type matrix must survive USDA->USDC->USDA");
}

// AOUSD-TYPE-001: cases GENERATED from the normative specification table
// (rather than the implementation registry) across the scalar / array /
// time-sampled / dictionary / semantic-alias contexts.
void TestNormativeTypeMatrix() {
  // Context 1+2: scalar and array defaults, declared-type fidelity.
  {
    std::string body = "def Scope \"T\" {\n";
    for (const char* t : kNormativeScalars) {
      const std::string id = SanitizeTypeIdent(t);
      body += "    " + std::string(t) + " v_" + id + " = " +
              NormativeLiteral(t, 0) + "\n";
      body += "    " + std::string(t) + "[] va_" + id + " = [" +
              NormativeLiteral(t, 0) + ", " + NormativeLiteral(t, 1) + "]\n";
    }
    body += "}\n";
    LoadResult direct = Parse(body, true);
    assert(direct.success);
    const PrimSpec* prim = direct.stage.GetRootLayer()->prim_at_path("/T");
    assert(prim);
    for (const char* t : kNormativeScalars) {
      const std::string id = SanitizeTypeIdent(t);
      const std::string* scalar_decl = prim->property_type_name("v_" + id);
      assert(scalar_decl && *scalar_decl == t &&
             "normative scalar type name must survive parsing");
      const std::string* array_decl = prim->property_type_name("va_" + id);
      assert(array_decl && *array_decl == std::string(t) + "[]" &&
             "normative array type name must survive parsing");
    }
    CheckNormativeBody(body, "defaults");
  }

  // Context 3: time-sampled values for every scalar and array type.
  {
    std::string body = "def Scope \"TS\" {\n";
    for (const char* t : kNormativeScalars) {
      const std::string id = SanitizeTypeIdent(t);
      body += "    " + std::string(t) + " ts_" + id + ".timeSamples = {\n" +
              "        1: " + NormativeLiteral(t, 0) + ",\n" +
              "        2: " + NormativeLiteral(t, 1) + "\n    }\n";
      body += "    " + std::string(t) + "[] tsa_" + id + ".timeSamples = {\n" +
              "        1: [" + NormativeLiteral(t, 0) + "],\n" +
              "        2: [" + NormativeLiteral(t, 1) + "]\n    }\n";
    }
    body += "}\n";
    CheckNormativeBody(body, "timeSamples");
  }

  // Context 4: every normative type nested as a typed dictionary entry
  // (customData), scalar and array forms, plus a nested dictionary.
  {
    std::string body = "def Scope \"D\" (\n    customData = {\n";
    for (const char* t : kNormativeScalars) {
      const std::string id = SanitizeTypeIdent(t);
      body += "        " + std::string(t) + " k_" + id + " = " +
              NormativeLiteral(t, 0) + "\n";
      body += "        " + std::string(t) + "[] ka_" + id + " = [" +
              NormativeLiteral(t, 0) + "]\n";
    }
    body += "        dictionary nested = { int a = 1 }\n";
    body += "    }\n) {\n}\n";
    // Guard against silent entry loss (a byte-compare alone cannot catch a
    // key dropped by BOTH the parser and the writer): every typed entry must
    // appear in the writer output.
    LoadResult direct = Parse(body, true);
    assert(direct.success);
    const std::string out = WriteUSDAToString(direct.stage);
    for (const char* t : kNormativeScalars) {
      const std::string id = SanitizeTypeIdent(t);
      assert(out.find(" k_" + id + " = ") != std::string::npos &&
             "typed dictionary entry lost");
      assert(out.find(" ka_" + id + " = ") != std::string::npos &&
             "typed dictionary array entry lost");
    }
    CheckNormativeBody(body, "dictionary");
  }

  // Context 5: semantic aliases — the DECLARED name must stay the alias (not
  // collapse to the underlying type) through parse and crate round-trip.
  {
    std::string body = "def Scope \"A\" {\n";
    for (const auto& alias : kNormativeAliases) {
      const std::string id = SanitizeTypeIdent(alias[0]);
      body += "    " + std::string(alias[0]) + " al_" + id + " = " +
              NormativeLiteral(alias[1], 0) + "\n";
      body += "    " + std::string(alias[0]) + "[] ala_" + id + " = [" +
              NormativeLiteral(alias[1], 0) + "]\n";
    }
    body += "}\n";
    LoadResult direct = Parse(body, true);
    assert(direct.success);
    const PrimSpec* prim = direct.stage.GetRootLayer()->prim_at_path("/A");
    assert(prim);
    for (const auto& alias : kNormativeAliases) {
      const std::string id = SanitizeTypeIdent(alias[0]);
      const std::string* decl = prim->property_type_name("al_" + id);
      assert(decl && *decl == alias[0] &&
             "semantic alias must not collapse to its underlying type");
      const std::string* adecl = prim->property_type_name("ala_" + id);
      assert(adecl && *adecl == std::string(alias[0]) + "[]");
    }
    CheckNormativeBody(body, "aliases");
  }

  // opaque/group: `None` (block) is the only representable value; ordinary
  // values must be rejected in strict mode (checked in
  // TestLosslessUnsupportedValues). Re-assert the block round-trip here so the
  // normative matrix is self-contained.
  CheckNormativeBody(
      "def Scope \"O\" {\n"
      "    opaque marker = None\n"
      "    group bundle = None\n"
      "}\n",
      "opaque/group");
}

void TestMetadataAndListOpFidelity() {
  // NaN sublayer offset scale: strict rejects; compat substitutes identity
  // (a `scale <= 0` guard would let NaN through since NaN <= 0 is false).
  const char* nan_scale =
      "def Xform \"R\" {}\n";  // body irrelevant; the offset is in stage meta
  const std::string layer_txt =
      std::string("#usda 1.0\n(\n  subLayers = [\n"
                  "    @sub.usda@ (offset = 1; scale = nan)\n  ]\n)\n") +
      nan_scale;
  LoadOptions strict_opts;
  strict_opts.parse_options.strict_aousd_conformance = true;
  LoadResult strict = LoadUSDAFromString(layer_txt, strict_opts);
  assert(!strict.success && "NaN sublayer scale must be rejected in strict mode");
  LoadResult compat = LoadUSDAFromString(layer_txt, LoadOptions{});
  assert(compat.success);
  const Layer* rl = compat.stage.GetRootLayer();
  assert(rl && !rl->meta().subLayerOffsets.empty());
  assert(rl->meta().subLayerOffsets[0].second == 1.0 &&
         "NaN sublayer scale must degrade to identity (1.0)");

  const std::string invalid_arc_offset =
      "def Xform \"R\" (\n"
      "  prepend references = @sub.usda@</R> "
      "(offset = 5; scale = -2)\n"
      ") {}\n";
  assert(!Parse(invalid_arc_offset, true).success &&
         "negative reference scale must be rejected in strict mode");
  LoadResult compat_arc = Parse(invalid_arc_offset, false);
  assert(compat_arc.success);
  const UsdPrim compat_arc_prim = compat_arc.stage.GetPrimAtPath("/R");
  assert(compat_arc_prim &&
         !compat_arc_prim.GetMeta().references.empty() &&
         compat_arc_prim.GetMeta().references.front().find("layerOffset=") ==
             std::string::npos &&
         "invalid reference offset must degrade to identity");

  // apiSchemas listop qualifier survives a USDC round trip: a bare authoring
  // stays explicit (not flipped to prepend); an authored prepend stays prepend.
  auto roundtrip_api = [](const std::string& body) -> std::string {
    LoadResult r = Parse(body, true);
    assert(r.success);
    USDCWriteOptions wo;
    std::vector<uint8_t> crate;
    assert(WriteUSDCToMemory(crate, r.stage, wo).success);
    USDCLoadOptions lo;
    USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size(), lo);
    assert(back.success);
    return WriteUSDAToString(back.stage);
  };
  const std::string bare =
      roundtrip_api("def \"P\" ( apiSchemas = [\"SkelBindingAPI\"] ) {}\n");
  assert(bare.find("apiSchemas = [\"SkelBindingAPI\"]") != std::string::npos &&
         bare.find("prepend apiSchemas") == std::string::npos &&
         "bare apiSchemas must stay explicit through USDC");
  const std::string prep = roundtrip_api(
      "def \"P\" ( prepend apiSchemas = [\"SkelBindingAPI\"] ) {}\n");
  assert(prep.find("prepend apiSchemas") != std::string::npos &&
         "authored prepend apiSchemas must stay prepend through USDC");

  // In SdfListOp semantics deletion applies to the weaker/base list before
  // local prepends. Therefore local prepend [A,B,C] + delete [C] has effective
  // value [A,B,C], while both authored sublists survive USDC exactly.
  const std::string del = roundtrip_api(
      "def \"P\" (\n"
      "  prepend apiSchemas = [\"AAPI\", \"BAPI\", \"CAPI\"]\n"
      "  delete apiSchemas = [\"CAPI\"]\n"
      ") {}\n");
  assert(del.find("prepend apiSchemas = [\"AAPI\", \"BAPI\", \"CAPI\"]") !=
             std::string::npos &&
         del.find("delete apiSchemas = [\"CAPI\"]") != std::string::npos &&
         "prepend+delete apiSchemas sublists must survive USDC exactly");
  LoadResult del_effective = LoadUSDAFromString(del, strict_opts);
  assert(del_effective.success);
  const std::vector<std::string>& applied =
      del_effective.stage.GetPrimAtPath("/P").GetMeta().apiSchemas();
  assert(applied == std::vector<std::string>({"AAPI", "BAPI", "CAPI"}) &&
         "same-site delete must not remove a locally prepended API schema");

  // A target-less declaration and an authored explicit-empty targetPaths
  // opinion are distinct. The latter clears weaker targets and must not
  // collapse to bare `rel r` during either USDA or USDC round trips.
  const std::string empty_rel = roundtrip_api(
      "def Scope \"P\" {\n"
      "  rel declared\n"
      "  rel cleared = None\n"
      "}\n");
  assert(empty_rel.find("rel declared\n") != std::string::npos &&
         empty_rel.find("rel declared =") == std::string::npos);
  assert(empty_rel.find("rel cleared = None") != std::string::npos &&
         "authored explicit-empty relationship targets must survive USDC");

  // A stronger explicit-empty apiSchemas list blocks weaker applications. An
  // empty vector without authored state used to look unauthored, allowing the
  // weaker API to leak into the populated prim.
  LoadResult api_strong = LoadUSDAFromString(
      "#usda 1.0\n( subLayers = [@weak.usda@] )\n"
      "over \"P\" ( apiSchemas = [] ) {}\n",
      LoadOptions{});
  LoadResult api_weak = Parse(
      "def \"P\" ( apiSchemas = [\"WeakAPI\"] ) {}\n", true);
  assert(api_strong.success && api_weak.success);
  std::unique_ptr<Layer> api_strong_layer = TakeLayer(&api_strong);
  std::unique_ptr<Layer> api_weak_layer = TakeLayer(&api_weak);
  Compositor api_compositor;
  api_compositor.SetLayerLoader(
      [&](const std::string& path, std::string*) {
        if (path.find("weak.usda") == std::string::npos) {
          return std::unique_ptr<Layer>();
        }
        return std::make_unique<Layer>(api_weak_layer->Clone());
      });
  std::unique_ptr<Layer> api_composed =
      api_compositor.Compose(*api_strong_layer, "strong.usda");
  assert(api_composed);
  const PrimSpec* api_prim = api_composed->prim_at_path("/P");
  assert(api_prim && api_prim->meta().apiSchemasAuthored() &&
         api_prim->meta().apiSchemas().empty() &&
         "strong explicit-empty apiSchemas must block weaker schemas");
  Stage api_stage;
  api_stage.SetRootLayer(std::move(*api_composed));
  const std::string api_usda = WriteUSDAToString(api_stage);
  assert(api_usda.find("apiSchemas = None") != std::string::npos);
  std::vector<uint8_t> api_crate;
  assert(WriteUSDCToMemory(api_crate, api_stage).success);
  USDCLoadResult api_back =
      LoadUSDCFromMemory(api_crate.data(), api_crate.size());
  assert(api_back.success);
  const UsdPrim api_back_prim = api_back.stage.GetPrimAtPath("/P");
  assert(api_back_prim.GetMeta().apiSchemasAuthored() &&
         api_back_prim.GetMeta().apiSchemas().empty());
  assert(WriteUSDAToString(api_back.stage).find("apiSchemas = None") !=
         std::string::npos);

  // Variant declaration vs dangling selection through USDC:
  // - a prim that only SELECTS a variant (no `prepend variantSets`) must NOT
  //   gain a synthesized variantSets declaration / empty variantSet block;
  // - a prim that DECLARES `prepend variantSets` must keep it.
  const std::string dangling = roundtrip_api(
      "def Xform \"H\" ( variants = { string v = \"a\" } ) {}\n");
  assert(dangling.find("variants = {") != std::string::npos &&
         "dangling variant selection must survive USDC");
  assert(dangling.find("variantSets") == std::string::npos &&
         "a dangling selection must not synthesize a variantSets declaration");
  const std::string declared = roundtrip_api(
      "def Xform \"H\" (\n"
      "  variants = { string v = \"a\" }\n"
      "  prepend variantSets = \"v\"\n"
      ") {}\n");
  assert(declared.find("prepend variantSets") != std::string::npos &&
         "authored prepend variantSets must survive USDC");
  assert(declared.find("variantSet \"v\"") == std::string::npos &&
         "a declaration-only variant set must NOT emit an empty variantSet "
         "block (matches pxr)");

  // A relocate-to-nothing target `<>` (the empty SdfPath) must survive USDC as
  // `<>`, not `</>`, and must NOT be written as a PATHS tree node (pxr rejects
  // a crate with an empty spec path). Parsed in compatibility mode (pxr itself
  // treats `<>` as ill-formed, so it is not a strict-conformant construct).
  LoadResult relo = LoadUSDAFromString(
      "#usda 1.0\n"
      "def Xform \"W\" (\n"
      "  relocates = { </W/keep>: </W/moved>, </W/drop>: <> }\n"
      ") {\n}\n",
      LoadOptions{});
  assert(relo.success);
  USDCWriteOptions rwo;
  std::vector<uint8_t> rcrate;
  assert(WriteUSDCToMemory(rcrate, relo.stage, rwo).success);
  USDCLoadOptions rlo;
  USDCLoadResult rback = LoadUSDCFromMemory(rcrate.data(), rcrate.size(), rlo);
  assert(rback.success);
  const std::string relo_usda = WriteUSDAToString(rback.stage);
  assert(relo_usda.find("</W/drop>: <>") != std::string::npos &&
         "empty relocate target must round-trip as <>, not </>");

  // Unmodeled (unknown) prim- AND layer-level metadata must survive USDC
  // verbatim (encoded in a lightusd-private field). Parsed non-strict since
  // strict AOUSD mode rejects unknown metadata.
  LoadResult um = LoadUSDAFromString(
      "#usda 1.0\n"
      "(\n  customStageKey = 42\n)\n"
      "def Xform \"P\" (\n  customPrimKey = \"hi\"\n) {\n}\n",
      LoadOptions{});
  assert(um.success);
  USDCWriteOptions umwo;
  std::vector<uint8_t> umcrate;
  assert(WriteUSDCToMemory(umcrate, um.stage, umwo).success);
  USDCLoadOptions umlo;
  USDCLoadResult umback =
      LoadUSDCFromMemory(umcrate.data(), umcrate.size(), umlo);
  assert(umback.success);
  const std::string um_usda = WriteUSDAToString(umback.stage);
  assert(um_usda.find("customStageKey = 42") != std::string::npos &&
         "unknown layer metadata must survive USDC");
  assert(um_usda.find("customPrimKey = \"hi\"") != std::string::npos &&
         "unknown prim metadata must survive USDC");
}

}  // namespace

// Path grammar production matrix: cases DERIVED FROM the §8.3 PEG rules,
// beyond the spec's own example tables (those are covered verbatim in
// TestUnicodeAndPaths). Each row exercises one production choice: variant
// whitespace, chained selections, prim elements resumed after a selection,
// XID_Continue-leading variant names vs XID_Start-required set names,
// reserved characters, and relative-vs-absolute property chain arity.
void TestPathGrammarProductions() {
  struct PathCase {
    const char* path;
    bool valid;
  };
  static const PathCase kPathGrammarMatrix[] = {
      // VariantSelection whitespace forms ("spaces and tabs" per §8.3).
      {"/City{\tselection\t=\tNewYork\t}", true},
      {"/City{ selection = }", true},
      // Chained selections and prim elements resumed after a selection.
      {"/A{v=x}{w=y}", true},
      {"/A{v=x}B", true},
      {"/A{v=x}B/C", true},
      {"/A{v=x}B.prop", true},
      {"/A{v=x}.prop", true},
      // Variant NAMES start at XID_Continue (digits, combining marks);
      // variant SET names are Identifiers (XID_Start required).
      {"/A{v=5th}", true},
      {"/A{v=\xCC\x81x}", true},  // leading combining acute (XID_Continue)
      {"/A{5v=x}", false},
      {"/A{v x=y}", false},
      {"/A{v=x y}", false},
      {"/A{v=x}", true},
      {"/A{v=x", false},
      // ReservedCharacters can never appear inside an Identifier.
      {"/Prim=Name", false},
      {"/Pri{m", false},
      {"/Prim}", false},
      // Relative forms: parent chains need '/'; '.' never precedes '/'.
      {".a.b", true},
      {"..points", false},
      {"../", false},
      {"../../", false},
      {"a/b.p", true},
      {"a/b.p.q", true},
      // Absolute paths take at most ONE property element
      // (PrimFirstPathElements); relative paths permit chains.
      {"/a/b.p.q", false},
      {"/.points", false},
      // Namespaced property elements inside longer paths.
      {"a/b.ns:prop", true},
      {"/a.ns:sub:prop", true},
      {"/a.ns:", false},
  };
  for (const PathCase& c : kPathGrammarMatrix) {
    if (IsValidPathString(c.path) != c.valid) {
      std::cout << "path grammar mismatch: '" << c.path << "' expected "
                << (c.valid ? "VALID" : "INVALID") << "\n";
      assert(false && "path grammar production mismatch");
    }
  }
}

// USDA escape-sequence matrix from the §usda string grammar: Escaped <-
// Backslash (EscapeSingleCharacter / EscapeHex / EscapeOct). Each authored
// escape must decode to the exact byte(s), and the decoded value must
// round-trip byte-identically through the writer and a re-parse.
void TestEscapeMatrix() {
  struct EscapeCase {
    const char* authored;  // inside double quotes in the layer text
    const char* expected;  // decoded bytes
  };
  static const EscapeCase kEscapeMatrix[] = {
      {"\\a", "\a"},   {"\\b", "\b"},  {"\\f", "\f"},  {"\\n", "\n"},
      {"\\r", "\r"},   {"\\t", "\t"},  {"\\v", "\v"},  {"\\'", "'"},
      {"\\\"", "\""},  {"\\x41", "A"}, {"\\x7", "\x07"},
      {"\\101", "A"},  {"\\7", "\x07"}, {"\\60", "0"},
      {"pre\\tpost", "pre\tpost"},
      {"two\\n\\nlines", "two\n\nlines"},
  };
  int idx = 0;
  for (const EscapeCase& c : kEscapeMatrix) {
    const std::string attr = "e" + std::to_string(idx++);
    const std::string body = std::string("def Xform \"E\" {\n    string ") +
                             attr + " = \"" + c.authored + "\"\n}\n";
    LoadResult r = Parse(body);
    assert(r.success && "escape case failed to parse");
    const Layer* layer = r.stage.GetRootLayer();
    assert(layer);
    const PrimSpec* spec = layer->prim_at_path("/E");
    assert(spec);
    const Value* v = spec->property_value(attr);
    assert(v && v->as_string());
    if (*v->as_string() != c.expected) {
      std::cout << "escape decode mismatch for '" << c.authored << "'\n";
      assert(false && "escape decode mismatch");
    }
    // Round-trip: write and re-parse must preserve the exact bytes.
    const std::string out = WriteUSDAToString(r.stage);
    LoadResult back = LoadUSDAFromString(out);
    assert(back.success && "escape round-trip failed to re-parse");
    const PrimSpec* spec2 = back.stage.GetRootLayer()->prim_at_path("/E");
    assert(spec2);
    const Value* v2 = spec2->property_value(attr);
    assert(v2 && v2->as_string() && *v2->as_string() == c.expected &&
           "escape round-trip byte mismatch");
  }
}

// Crate version feature matrix: which authored feature forces which minimum
// crate minor version on WRITE (bootstrap byte 9), and a minor version past
// the supported window must be REJECTED on read.
void TestCrateVersionMatrix() {
  struct VersionCase {
    const char* body;
    uint8_t min_minor;  // required minimum written minor version
  };
  static const VersionCase kCrateVersionMatrix[] = {
      // Plain data stays at the writer default (0.8).
      {"def Xform \"A\" { double v = 1 }\n", 8},
      // timecode values need 0.9+.
      {"def Xform \"A\" { timecode t = 5 }\n", 9},
      {"def Xform \"A\" { timecode[] t = [5, 6] }\n", 9},
      // layer relocates need 0.11.
      {"(\n    relocates = { </A/Old>: </A/New> }\n)\ndef Xform \"A\" {\n"
       "    def Xform \"Old\" {}\n}\n", 11},
      // splines need 0.12.
      {"def Xform \"A\" {\n    double v.spline = {\n        1: 1; post held,\n"
       "    }\n}\n", 12},
  };
  for (const VersionCase& c : kCrateVersionMatrix) {
    LoadResult r = Parse(c.body);
    assert(r.success && "crate version case failed to parse");
    std::vector<uint8_t> crate;
    assert(WriteUSDCToMemory(crate, r.stage).success);
    assert(crate.size() > 10);
    const uint8_t minor = crate[9];
    if (minor < c.min_minor) {
      std::cout << "crate version too low: got 0." << int(minor)
                << " need 0." << int(c.min_minor) << "\n";
      assert(false && "crate feature did not bump the version");
    }
    // The written file must load back.
    USDCLoadResult back = LoadUSDCFromMemory(crate.data(), crate.size());
    assert(back.success && "versioned crate failed to load back");
    // An unsupported NEWER minor version must be rejected wholesale.
    std::vector<uint8_t> future = crate;
    future[9] = 99;
    USDCLoadResult rejected = LoadUSDCFromMemory(future.data(), future.size());
    assert(!rejected.success && "future crate version must be rejected");
  }
}

int main() {
  TestUnicodeAndPaths();
  TestPathGrammarProductions();
  TestEscapeMatrix();
  TestCrateVersionMatrix();
  TestLosslessUnsupportedValues();
  TestTypedSplines();
  TestFoundationalTypeMatrix();
  TestNormativeTypeMatrix();
  TestMetadataAndListOpFidelity();
  TestDictionaryAndRelationshipComposition();
  TestNamespaceOrdering();
  TestDefaultPrimReferenceEncoding();
  TestRelationshipForwarding();
  TestAuthoredEmptyMetadata();
  TestVariantSetListOpFidelity();
  TestApiSchemaListOpFidelity();
  TestConnectionListOpFidelity();
  TestSpecifierResolution();
  TestStageQueryAncestry();
  TestInterpolationMatrix();
  TestSchemaFallbackAndValueClips();
  TestSchemaRegistryBreadth();
  TestRelationshipVariabilityFidelity();
  TestLayerOwnerFidelity();
  TestGeneratedCoreSchemaCoverage();
  TestExpressionVariablePolicy();
  TestVariableExpressionGrammar();
  TestAuthoredStateBits();
  TestStringListOpFieldTable();
  TestVariantExtensionFields();
  TestRemainingElectiveFieldCoverage();
  std::cout << "AOUSD conformance regressions: PASSED\n";
  return 0;
}
