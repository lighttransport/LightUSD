// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Lean-generated MaterialX conformance vectors.
//
// The expected values in tests/core/generated/lean-mtlx/*.inc are computed by
// the Lean 4 formalization of MaterialX 1.39 in the materialx-conformance
// repository (`lake exe mtlxspec-gen` in verif/lean). Each section replays one
// vector table against src/core/mtlx-resolve. The tables are COMMITTED; this
// binary is self-contained and never skips.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/mtlx-dom.hh"
#include "core/mtlx-resolve.hh"
#include "pcp/layer-registry.hh"

using namespace lightusd::mtlx;

static int g_fail = 0;
static int g_checks = 0;

static void Fail(const char *table, int id, const std::string &msg) {
  std::fprintf(stderr, "FAIL %s[%d]: %s\n", table, id, msg.c_str());
  g_fail++;
}

static std::vector<std::string> Split(const std::string &s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

// "num/den" -> double.
static double Rat(const std::string &s) {
  const size_t slash = s.find('/');
  const double n = std::strtod(s.substr(0, slash).c_str(), nullptr);
  const double d = slash == std::string::npos
                       ? 1.0
                       : std::strtod(s.substr(slash + 1).c_str(), nullptr);
  return n / d;
}

static bool Close(double a, double b) {
  if (a == b) return true;
  const double scale = std::fmax(std::fabs(a), std::fabs(b));
  return std::fabs(a - b) <= scale * 1e-15;
}

static const char *KindName(MtlxValueKind k) {
  switch (k) {
    case MtlxValueKind::None: return "none";
    case MtlxValueKind::Int: return "int";
    case MtlxValueKind::Bool: return "bool";
    case MtlxValueKind::Num: return "num";
    case MtlxValueKind::Agg: return "agg";
    case MtlxValueKind::Str: return "str";
    case MtlxValueKind::Ints: return "ints";
    case MtlxValueKind::Nums: return "nums";
    case MtlxValueKind::Strs: return "strs";
  }
  return "?";
}

static bool PayloadMatches(const MtlxTypedValue &v, const std::string &payload) {
  switch (v.kind) {
    case MtlxValueKind::None: return payload.empty();
    case MtlxValueKind::Int: return std::to_string(v.i) == payload;
    case MtlxValueKind::Bool: return payload == (v.b ? "true" : "false");
    case MtlxValueKind::Num: return Close(v.num, Rat(payload));
    case MtlxValueKind::Str: return v.str == payload;
    case MtlxValueKind::Agg:
    case MtlxValueKind::Nums: {
      const std::vector<std::string> parts = Split(payload, ',');
      if (parts.size() != v.nums.size()) return false;
      for (size_t i = 0; i < parts.size(); i++) {
        if (!Close(v.nums[i], Rat(parts[i]))) return false;
      }
      return true;
    }
    case MtlxValueKind::Ints: {
      const std::vector<std::string> parts = Split(payload, ',');
      if (parts.size() != v.ints.size()) return false;
      for (size_t i = 0; i < parts.size(); i++) {
        if (std::to_string(v.ints[i]) != parts[i]) return false;
      }
      return true;
    }
    case MtlxValueKind::Strs: {
      // The model's empty array and its one-empty-member array both print as
      // "", so compare through the same join.
      std::string joined;
      for (size_t i = 0; i < v.strs.size(); i++) {
        if (i) joined.push_back('\037');
        joined += v.strs[i];
      }
      return joined == payload;
    }
  }
  return false;
}

static void CheckName(int id, const char *text, int spec, int ref) {
  g_checks++;
  if (IsValidMtlxName(text) != (spec != 0)) Fail("MTLX_NAME", id, "strict validity");
  if (IsReferenceValidMtlxName(text) != (ref != 0)) {
    Fail("MTLX_NAME", id, "reference validity");
  }
}

static void CheckValue(int id, const char *type, const char *text, int ok,
                       const char *kind, const char *payload) {
  g_checks++;
  MtlxTypedValue v;
  const bool got = ParseMtlxValue(type, text, &v);
  if (got != (ok != 0)) {
    Fail("MTLX_VALUE", id, std::string("acceptance of [") + text + "] as " + type);
    return;
  }
  if (!got) return;
  if (std::string(KindName(v.kind)) != kind) {
    Fail("MTLX_VALUE", id, std::string("kind ") + KindName(v.kind) + " != " + kind);
  } else if (!PayloadMatches(v, payload)) {
    Fail("MTLX_VALUE", id, std::string("payload of [") + text + "]");
  }
}

static void CheckDefault(const char *type, const char *kind, const char *payload) {
  g_checks++;
  MtlxTypedValue v;
  if (!DefaultMtlxValue(type, &v)) {
    Fail("MTLX_DEFAULT", 0, std::string("no default for ") + type);
    return;
  }
  if (std::string(KindName(v.kind)) != kind || !PayloadMatches(v, payload)) {
    Fail("MTLX_DEFAULT", 0, std::string("default of ") + type);
  }
}

static void CheckScope(int id, const char *xml, const char *path, int has_fn,
                       const char *fn, const char *cs) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_SCOPE", id, "parse: " + err);
    return;
  }
  MtlxScope chain = {&root};
  for (const std::string &name : Split(path, '/')) {
    const MtlxElem *c = chain.front()->Child(name);
    if (!c) {
      Fail("MTLX_SCOPE", id, "path " + name);
      return;
    }
    chain.insert(chain.begin(), c);
  }
  std::string got_fn;
  const bool got = ResolvedMtlxFilename(chain, &got_fn);
  if (got != (has_fn != 0) || (got && got_fn != fn)) {
    Fail("MTLX_SCOPE", id, "filename [" + got_fn + "] != [" + fn + "]");
  }
  std::string got_cs;
  (void)ActiveMtlxColorSpace(chain, &got_cs);
  if (got_cs != cs) Fail("MTLX_SCOPE", id, "colorspace [" + got_cs + "] != [" + cs + "]");
}

static std::string Listing(const MtlxElem &root) {
  static const char *const kKeys[] = {"fileprefix", "geomprefix", "colorspace",
                                      "namespace"};
  std::string out;
  for (size_t i = 0; i < root.children.size(); i++) {
    const MtlxElem &c = root.children[i];
    if (i) out.push_back(',');
    out += c.tag + ":" + c.Name() + "[";
    bool first = true;
    for (const char *k : kKeys) {
      if (const std::string *v = c.Attr(k)) {
        if (!first) out.push_back(';');
        out += std::string(k) + "=" + *v;
        first = false;
      }
    }
    out += "]";
  }
  return out;
}

static void CheckInclude(int id, const char *main_xml, const char *href_a,
                         const char *xml_a, const char *href_b,
                         const char *xml_b, int ok, const char *listing) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(main_xml, &root, &err)) {
    Fail("MTLX_INCLUDE", id, "parse: " + err);
    return;
  }
  const std::string ha = href_a, hb = href_b, xa = xml_a, xb = xml_b;
  const bool got = ExpandMtlxIncludes(
      &root,
      [&](const std::string &href, std::string *xml) {
        if (!ha.empty() && href == ha) {
          *xml = xa;
          return true;
        }
        if (!hb.empty() && href == hb) {
          *xml = xb;
          return true;
        }
        return false;
      },
      &err);
  if (got != (ok != 0)) {
    Fail("MTLX_INCLUDE", id, "acceptance (" + err + ")");
    return;
  }
  if (got && Listing(root) != listing) {
    Fail("MTLX_INCLUDE", id, "[" + Listing(root) + "] != [" + listing + "]");
  }
}

static MtlxLibrary g_resolve_lib;

static void SetResolveLib(const char *xml) {
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_RESOLVE_LIB", 0, "parse: " + err);
    return;
  }
  g_resolve_lib.AddFrom(root);
}

static void CheckResolve(int id, const char *node_xml, const char *strict,
                         const char *ref, const char *rough) {
  g_checks++;
  MtlxElem node;
  std::string err;
  if (!ParseMtlxXml(node_xml, &node, &err)) {
    Fail("MTLX_RESOLVE", id, "parse: " + err);
    return;
  }
  const MtlxNodeDefInfo *s = g_resolve_lib.ResolveStrict(node);
  const MtlxNodeDefInfo *r = g_resolve_lib.ResolveReference(node);
  if ((s ? s->name : std::string()) != strict) {
    Fail("MTLX_RESOLVE", id, "strict [" + (s ? s->name : std::string()) + "]");
  }
  if ((r ? r->name : std::string()) != ref) {
    Fail("MTLX_RESOLVE", id, "reference [" + (r ? r->name : std::string()) + "]");
  }
  const MtlxNodeDefInfo *g = g_resolve_lib.ResolveRough(node);
  if ((g ? g->name : std::string()) != rough) {
    Fail("MTLX_RESOLVE", id, "rough [" + (g ? g->name : std::string()) + "]");
  }
}

static void CheckDump(int id, const char *xml, const char *dump) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_DUMP", id, "parse: " + err);
    return;
  }
  MtlxLibrary lib;
  lib.AddFrom(root);
  const std::string got = DumpMtlxDocument(lib, root);
  if (got != dump) {
    Fail("MTLX_DUMP", id, "dump differs:\n--- got ---\n" + got + "--- want ---\n" + dump);
  }
}

static void CheckMtlxDocumentXInclude() {
  g_checks++;
  MtlxDocument doc;
  if (!doc.ParseFromFile("tests/fixtures/mtlx-xinclude/main.mtlx")) {
    Fail("MTLX_DOCUMENT_INCLUDE", 0, doc.GetError());
    return;
  }
  const MtlxNodePtr node = doc.FindNode("included_constant");
  if (!node) {
    Fail("MTLX_DOCUMENT_INCLUDE", 0,
         "node from a nested relative XInclude was not parsed");
    return;
  }
  const MtlxInputPtr input = node->GetInput("value");
  if (!input || input->GetValue().type != MtlxValue::TYPE_FLOAT_VECTOR ||
      input->GetValue().float_vec.size() != 3 ||
      std::fabs(input->GetValue().float_vec[0] - 0.25f) > 1.0e-6f ||
      std::fabs(input->GetValue().float_vec[1] - 0.5f) > 1.0e-6f ||
      std::fabs(input->GetValue().float_vec[2] - 0.75f) > 1.0e-6f) {
    Fail("MTLX_DOCUMENT_INCLUDE", 0,
         "included node input values were not preserved");
  }

  g_checks++;
  std::string warn, err;
  const std::shared_ptr<lightusd::Layer> layer =
      lightusd::pcp::LoadLayerFromFile(
          "tests/fixtures/mtlx-xinclude/main.mtlx", &warn, &err);
  if (!layer ||
      !layer->prim_at_path("/MaterialX/Shaders/included_constant")) {
    Fail("MTLX_LAYER_INCLUDE", 0,
         layer ? "included node was not emitted into the composition layer"
               : err);
  }

  g_checks++;
  MtlxDocument unsafe;
  const std::string unsafe_xml =
      "<materialx version=\"1.39\" "
      "xmlns:xi=\"http://www.w3.org/2001/XInclude\">"
      "<xi:include href=\"../outside.mtlx\"/></materialx>";
  if (unsafe.ParseFromXML(unsafe_xml,
                          "tests/fixtures/mtlx-xinclude/main.mtlx")) {
    Fail("MTLX_DOCUMENT_INCLUDE_PATH", 0,
         "parent-traversal XInclude unexpectedly succeeded");
  }

  g_checks++;
  MtlxDocument defaults;
  const std::string defaults_xml = R"(<materialx version="1.39">
  <nodedef name="ND_base" node="defaulted" type="color3">
    <input name="value" type="color3" value="0.1, 0.2, 0.3"/>
  </nodedef>
  <nodedef name="ND_defaulted" node="defaulted" type="color3" inherit="ND_base"/>
  <defaulted name="defaulted_node" type="color3" nodedef="ND_defaulted"/>
  <defaulted name="wired_node" type="color3" nodedef="ND_defaulted">
    <input name="value" type="color3" nodename="wire"/>
  </defaulted>
</materialx>)";
  if (!defaults.ParseFromXML(defaults_xml)) {
    Fail("MTLX_DOCUMENT_NODEDEF_DEFAULT", 0, defaults.GetError());
    return;
  }
  const MtlxNodePtr defaulted_node = defaults.FindNode("defaulted_node");
  const MtlxInputPtr defaulted_value =
      defaulted_node ? defaulted_node->GetInput("value") : nullptr;
  if (!defaulted_value ||
      defaulted_value->GetValue().type != MtlxValue::TYPE_FLOAT_VECTOR ||
      defaulted_value->GetValue().float_vec.size() != 3 ||
      std::fabs(defaulted_value->GetValue().float_vec[0] - 0.1f) > 1.0e-6f ||
      std::fabs(defaulted_value->GetValue().float_vec[1] - 0.2f) > 1.0e-6f ||
      std::fabs(defaulted_value->GetValue().float_vec[2] - 0.3f) > 1.0e-6f) {
    Fail("MTLX_DOCUMENT_NODEDEF_DEFAULT", 0,
         "nodedef default input was not added to the node instance");
  }
  const MtlxNodePtr wired_node = defaults.FindNode("wired_node");
  const MtlxInputPtr wired_value =
      wired_node ? wired_node->GetInput("value") : nullptr;
  if (!wired_value || wired_value->GetNodeName() != "wire" ||
      wired_value->GetValue().type != MtlxValue::TYPE_NONE) {
    Fail("MTLX_DOCUMENT_NODEDEF_BINDING", 0,
         "nodedef default overwrote an authored node connection");
  }

  g_checks++;
  MtlxDocument scoped;
  const std::string scoped_xml = R"(<materialx version="1.39"
      colorspace="lin_rec709" fileprefix="textures/">
  <image name="inherited_image" type="color3">
    <input name="file" type="filename" value="albedo.exr"/>
  </image>
  <image name="overridden_image" type="color3">
    <input name="file" type="filename" value="mask.exr" colorspace="data"/>
  </image>
</materialx>)";
  if (!scoped.ParseFromXML(scoped_xml)) {
    Fail("MTLX_DOCUMENT_SCOPED_ATTRIBUTES", 0, scoped.GetError());
    return;
  }
  const MtlxNodePtr inherited_image = scoped.FindNode("inherited_image");
  const MtlxInputPtr inherited_file =
      inherited_image ? inherited_image->GetInput("file") : nullptr;
  const MtlxNodePtr overridden_image = scoped.FindNode("overridden_image");
  const MtlxInputPtr overridden_file =
      overridden_image ? overridden_image->GetInput("file") : nullptr;
  if (!inherited_file || inherited_file->GetColorSpace() != "lin_rec709" ||
      inherited_file->GetFilePrefix() != "textures/" || !overridden_file ||
      overridden_file->GetColorSpace() != "data" ||
      overridden_file->GetFilePrefix() != "textures/") {
    Fail("MTLX_DOCUMENT_SCOPED_ATTRIBUTES", 0,
         "scoped colorspace/fileprefix inheritance or override failed");
  }

  g_checks++;
  MtlxDocument standard_defaults;
  const std::string standard_defaults_xml = R"(<materialx version="1.39">
  <constant name="standard_default" type="color3"/>
</materialx>)";
  if (!standard_defaults.ParseFromXML(standard_defaults_xml)) {
    Fail("MTLX_DOCUMENT_STDLIB_DEFAULT", 0, standard_defaults.GetError());
    return;
  }
  const MtlxNodePtr standard_default =
      standard_defaults.FindNode("standard_default");
  const MtlxInputPtr standard_value =
      standard_default ? standard_default->GetInput("value") : nullptr;
  if (!standard_value ||
      standard_value->GetValue().type != MtlxValue::TYPE_FLOAT_VECTOR ||
      standard_value->GetValue().float_vec.size() != 3 ||
      std::fabs(standard_value->GetValue().float_vec[0]) > 1.0e-6f ||
      std::fabs(standard_value->GetValue().float_vec[1]) > 1.0e-6f ||
      std::fabs(standard_value->GetValue().float_vec[2]) > 1.0e-6f) {
    Fail("MTLX_DOCUMENT_STDLIB_DEFAULT", 0,
         "standard-library nodedef default was not applied");
  }
}

int main() {
#define MTLX_NAME(id, text, spec, ref) CheckName(id, text, spec, ref);
#include "generated/lean-mtlx/mtlx-name.inc"
#undef MTLX_NAME

#define MTLX_VALUE(id, type, text, ok, kind, payload) \
  CheckValue(id, type, text, ok, kind, payload);
#define MTLX_DEFAULT(type, kind, payload) CheckDefault(type, kind, payload);
#include "generated/lean-mtlx/mtlx-value.inc"
#undef MTLX_VALUE
#undef MTLX_DEFAULT

#define MTLX_SCOPE(id, xml, path, has_fn, fn, cs) \
  CheckScope(id, xml, path, has_fn, fn, cs);
#define MTLX_INCLUDE(id, main_xml, ha, xa, hb, xb, ok, listing) \
  CheckInclude(id, main_xml, ha, xa, hb, xb, ok, listing);
#define MTLX_RESOLVE_LIB(xml) SetResolveLib(xml);
#define MTLX_RESOLVE(id, node_xml, strict, ref, rough) \
  CheckResolve(id, node_xml, strict, ref, rough);
#define MTLX_DUMP(id, xml, dump) CheckDump(id, xml, dump);
#include "generated/lean-mtlx/mtlx-doc.inc"
#undef MTLX_SCOPE
#undef MTLX_INCLUDE
#undef MTLX_RESOLVE_LIB
#undef MTLX_RESOLVE
#undef MTLX_DUMP

  CheckMtlxDocumentXInclude();

  std::printf("test_mtlx_lean_vectors: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
