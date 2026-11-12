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

static void CheckTypedMtlxGraphResolution() {
  const auto elem = [](const std::string &tag, const std::string &name) {
    MtlxElem value;
    value.tag = tag;
    if (!name.empty()) value.SetAttr("name", name);
    return value;
  };
  const auto add_port = [&elem](MtlxElem *parent, const std::string &tag,
                                const std::string &name,
                                const std::string &type) {
    MtlxElem port = elem(tag, name);
    port.SetAttr("type", type);
    parent->children.push_back(std::move(port));
  };
  MtlxLibrary library;
  MtlxElem constant_def = elem("nodedef", "ND_test_constant");
  constant_def.SetAttr("node", "testconstant");
  constant_def.SetAttr("type", "float");
  add_port(&constant_def, "output", "out", "float");
  library.AddNodeDef(constant_def, {});
  MtlxElem add_def = elem("nodedef", "ND_test_add");
  add_def.SetAttr("node", "testadd");
  add_def.SetAttr("type", "float");
  add_port(&add_def, "input", "in1", "float");
  add_port(&add_def, "input", "in2", "float");
  add_port(&add_def, "output", "out", "float");
  library.AddNodeDef(add_def, {});
  MtlxElem graph_def = elem("nodedef", "ND_test_graph");
  graph_def.SetAttr("node", "testgraph");
  graph_def.SetAttr("type", "float");
  add_port(&graph_def, "input", "gain", "float");
  add_port(&graph_def, "output", "out", "float");
  library.AddNodeDef(graph_def, {});

  const auto constant_node = [&elem](const std::string &name) {
    MtlxElem node = elem("testconstant", name);
    node.SetAttr("type", "float");
    node.SetAttr("nodedef", "ND_test_constant");
    return node;
  };
  const auto add_node = [&elem](const std::string &name) {
    MtlxElem node = elem("testadd", name);
    node.SetAttr("type", "float");
    node.SetAttr("nodedef", "ND_test_add");
    return node;
  };

  MtlxElem root = elem("materialx", "");
  MtlxElem helper = elem("nodegraph", "helper");
  helper.children.push_back(constant_node("source"));
  MtlxElem helper_output = elem("output", "value");
  helper_output.SetAttr("type", "float");
  helper_output.SetAttr("nodename", "source");
  helper.children.push_back(helper_output);
  root.children.push_back(helper);

  MtlxElem graph = elem("nodegraph", "main");
  graph.SetAttr("nodedef", "ND_test_graph");
  MtlxElem graph_input = elem("input", "gain");
  graph_input.SetAttr("type", "float");
  graph.children.push_back(graph_input);
  graph.children.push_back(constant_node("seed"));
  MtlxElem sum = add_node("sum");
  MtlxElem from_nodegraph = elem("input", "in1");
  from_nodegraph.SetAttr("type", "float");
  from_nodegraph.SetAttr("nodegraph", "helper");
  from_nodegraph.SetAttr("output", "value");
  sum.children.push_back(from_nodegraph);
  MtlxElem from_interface = elem("input", "in2");
  from_interface.SetAttr("type", "float");
  from_interface.SetAttr("interfacename", "gain");
  sum.children.push_back(from_interface);
  graph.children.push_back(sum);
  MtlxElem output = elem("output", "out");
  output.SetAttr("type", "float");
  output.SetAttr("nodename", "sum");
  graph.children.push_back(output);
  root.children.push_back(graph);

  g_checks++;
  MtlxResolvedGraph resolved;
  std::string error;
  if (!ResolveMtlxGraph(root, root.children[1], library, {}, &resolved, &error) ||
      resolved.node_order != std::vector<std::string>({"seed", "sum"})) {
    Fail("MTLX_GRAPH_RESOLVE", 0,
         error.empty() ? "dependency order or resolved graph differed" : error);
  }
  std::vector<std::string> graph_order;
  g_checks++;
  if (!ResolveMtlxGraphs(root, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_RESOLVE", 0,
         error.empty() ? "nodegraph dependency order differed" : error);
  }

  MtlxElem namespaced_root = root;
  namespaced_root.SetAttr("namespace", "studio");
  namespaced_root.children[0].SetAttr("name", "studio:helper");
  namespaced_root.children[1].SetAttr("name", "studio:main");
  g_checks++;
  if (!ResolveMtlxGraphs(namespaced_root, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"studio:helper", "studio:main"})) {
    Fail("MTLX_GRAPHS_NAMESPACE", 0,
         error.empty() ? "namespaced nodegraph dependency differed" : error);
  }

  MtlxElem wrong_type = root.children[1];
  wrong_type.children[2].children[0].SetAttr("type", "integer");
  g_checks++;
  if (ResolveMtlxGraph(root, wrong_type, library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_TYPE", 0, "mismatched input type was accepted");
  }

  MtlxElem wrong_node_type = root.children[1];
  wrong_node_type.children[1].SetAttr("type", "integer");
  g_checks++;
  if (ResolveMtlxGraph(root, wrong_node_type, library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_NODE_TYPE", 0, "node type differing from its nodedef was accepted");
  }

  MtlxElem wrong_category = root.children[1];
  wrong_category.children[1].tag = "otherconstant";
  g_checks++;
  if (ResolveMtlxGraph(root, wrong_category, library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_NODE_CATEGORY", 0,
         "node category differing from its nodedef was accepted");
  }

  MtlxElem missing_source = root.children[1];
  missing_source.children[2].children[0].SetAttr("nodegraph", "missing");
  g_checks++;
  if (ResolveMtlxGraph(root, missing_source, library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_SOURCE", 0, "missing nodegraph source was accepted");
  }

  MtlxElem cyclic = elem("nodegraph", "cycle");
  MtlxElem a = add_node("a");
  MtlxElem a_in1 = elem("input", "in1");
  a_in1.SetAttr("type", "float");
  a_in1.SetAttr("nodename", "b");
  a.children.push_back(a_in1);
  MtlxElem a_in2 = elem("input", "in2");
  a_in2.SetAttr("type", "float");
  a_in2.SetAttr("value", "1");
  a.children.push_back(a_in2);
  MtlxElem b = add_node("b");
  MtlxElem b_in1 = elem("input", "in1");
  b_in1.SetAttr("type", "float");
  b_in1.SetAttr("nodename", "a");
  b.children.push_back(b_in1);
  MtlxElem b_in2 = elem("input", "in2");
  b_in2.SetAttr("type", "float");
  b_in2.SetAttr("value", "1");
  b.children.push_back(b_in2);
  cyclic.children.push_back(a);
  cyclic.children.push_back(b);
  g_checks++;
  if (ResolveMtlxGraph(root, cyclic, library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CYCLE", 0, "cyclic graph was accepted");
  }

  MtlxElem graph_cycle = root;
  MtlxElem helper_cycle_node = add_node("helper_cycle");
  MtlxElem helper_cycle_input = elem("input", "in1");
  helper_cycle_input.SetAttr("type", "float");
  helper_cycle_input.SetAttr("nodegraph", "main");
  helper_cycle_input.SetAttr("output", "out");
  helper_cycle_node.children.push_back(helper_cycle_input);
  MtlxElem helper_cycle_value = elem("input", "in2");
  helper_cycle_value.SetAttr("type", "float");
  helper_cycle_value.SetAttr("value", "1");
  helper_cycle_node.children.push_back(helper_cycle_value);
  graph_cycle.children[0].children.push_back(helper_cycle_node);
  graph_cycle.children[0].children[1].SetAttr("nodename", "helper_cycle");
  g_checks++;
  if (ResolveMtlxGraphs(graph_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_CYCLE", 0, "cross-nodegraph dependency cycle was accepted");
  }

  g_checks++;
  MtlxDocument parsed_document;
  const std::string graph_xml = R"(<materialx version="1.39">
  <nodegraph name="validated">
    <constant name="color" type="color3" nodedef="ND_constant_color3"/>
    <output name="out" type="color3" nodename="color"/>
  </nodegraph>
</materialx>)";
  std::vector<std::string> parsed_order;
  if (!parsed_document.ParseFromXML(graph_xml) ||
      !parsed_document.ResolveNodeGraphs(&parsed_order, &error) ||
      parsed_order != std::vector<std::string>({"validated"})) {
    Fail("MTLX_DOCUMENT_GRAPH_RESOLVE", 0,
         error.empty() ? parsed_document.GetError() : error);
  }

  g_checks++;
  MtlxDocument external_document;
  MtlxLibrary external_library;
  MtlxElem external_defs = elem("materialx", "");
  MtlxElem external_constant = elem("nodedef", "ND_external_constant");
  external_constant.SetAttr("node", "externalconstant");
  external_constant.SetAttr("type", "float");
  add_port(&external_constant, "output", "out", "float");
  external_defs.children.push_back(external_constant);
  MtlxElem external_passthrough = elem("nodedef", "ND_external_passthrough");
  external_passthrough.SetAttr("node", "externalpassthrough");
  external_passthrough.SetAttr("type", "float");
  add_port(&external_passthrough, "input", "gain", "float");
  add_port(&external_passthrough, "output", "out", "float");
  external_defs.children.push_back(external_passthrough);
  MtlxElem external_color = elem("nodedef", "ND_external_color");
  external_color.SetAttr("node", "externalcolor");
  external_color.SetAttr("type", "color3");
  add_port(&external_color, "input", "value", "color3");
  add_port(&external_color, "output", "out", "color3");
  external_defs.children.push_back(external_color);
  external_library.AddFrom(external_defs);
  const std::string external_graph_xml = R"(<materialx version="1.39">
  <implementation name="IM_external" nodedef="ND_external_passthrough" nodegraph="external_graph"/>
  <nodegraph name="external_graph">
    <input name="gain" type="float"/>
    <externalpassthrough name="value" type="float" nodedef="ND_external_passthrough">
      <input name="gain" type="float" interfacename="gain"/>
    </externalpassthrough>
    <externalcolor name="literal" type="color3" nodedef="ND_external_color">
      <input name="value" type="color3" value="0.944 0.776 0.373"/>
    </externalcolor>
    <output name="out" type="float" nodename="value"/>
  </nodegraph>
</materialx>)";
  if (!external_document.ParseFromXML(external_graph_xml) ||
      !external_document.ResolveNodeGraphs(external_library, &parsed_order,
                                           &error) ||
      parsed_order != std::vector<std::string>({"external_graph"})) {
    Fail("MTLX_DOCUMENT_EXTERNAL_LIBRARY", 0,
         error.empty() ? external_document.GetError() : error);
  }

  g_checks++;
  MtlxDocument legacy_document;
  const std::string legacy_xml = R"(<materialx version="1.37">
  <nodegraph name="legacy">
    <diffuse_brdf name="diffuse" type="BSDF"/>
  </nodegraph>
</materialx>)";
  if (!legacy_document.ParseFromXML(legacy_xml) ||
      legacy_document.GetVersion() != "1.39" ||
      !legacy_document.FindNodeGraph("legacy") ||
      !legacy_document.FindNodeGraph("legacy")->GetNode("diffuse") ||
      legacy_document.FindNodeGraph("legacy")->GetNode("diffuse")->GetCategory() !=
          "oren_nayar_diffuse_bsdf") {
    Fail("MTLX_VERSION_UPGRADE_137", 0,
         "legacy diffuse_brdf node was not upgraded to 1.39");
  }

  g_checks++;
  MtlxDocument legacy_opgraph_document;
  const std::string legacy_opgraph_xml = R"(<materialx version="1.22">
  <opgraph name="legacy_graph">
    <constant name="base" type="float">
      <parameter name="color" type="float" value="0.75"/>
    </constant>
    <output name="out" type="float">
      <parameter name="in" type="opgraphnode" value="base"/>
    </output>
  </opgraph>
  <shader name="legacy_shader" shadertype="surface" shadername="surface_node">
    <input name="base" type="float" graphname="legacy_graph" graphoutput="out"/>
  </shader>
  <material name="legacy_material">
    <shaderref name="legacy_shader" shadertype="surface"/>
  </material>
</materialx>)";
  const bool legacy_opgraph_parsed =
      legacy_opgraph_document.ParseFromXML(legacy_opgraph_xml);
  const MtlxNodeGraphPtr legacy_opgraph =
      legacy_opgraph_document.FindNodeGraph("legacy_graph");
  const MtlxNodePtr legacy_opgraph_constant =
      legacy_opgraph ? legacy_opgraph->GetNode("base") : nullptr;
  const MtlxInputPtr legacy_opgraph_value =
      legacy_opgraph_constant ? legacy_opgraph_constant->GetInput("value")
                              : nullptr;
  const MtlxNodePtr legacy_shader =
      legacy_opgraph_document.FindNode("legacy_shader");
  const MtlxMaterialPtr legacy_material =
      legacy_opgraph_document.FindMaterial("legacy_material");
  float legacy_constant_value = 0.0f;
  if (!legacy_opgraph_parsed ||
      legacy_opgraph_document.GetVersion() != "1.39" || !legacy_opgraph ||
      !legacy_opgraph_constant || !legacy_opgraph_value ||
      !legacy_opgraph_value->GetValueAsFloat(legacy_constant_value) ||
      legacy_constant_value != 0.75f ||
      legacy_opgraph->GetOutputs().size() != 1 ||
      legacy_opgraph->GetOutputs()[0]->GetNodeName() != "base" ||
      !legacy_shader || legacy_shader->GetCategory() != "surface_node" ||
      !legacy_shader->GetInput("base") ||
      legacy_shader->GetInput("base")->GetNodeGraph() != "legacy_graph" ||
      legacy_shader->GetInput("base")->GetOutput() != "out" ||
      !legacy_material || legacy_material->GetSurfaceShader() != "legacy_shader") {
    Fail("MTLX_VERSION_UPGRADE_122", 0,
         "legacy opgraph, shader bindings or material were not upgraded");
  }

  g_checks++;
  MtlxDocument legacy_override_document;
  const std::string legacy_override_xml = R"(<materialx version="1.25">
  <shader name="standard_surface" shadertype="surface" shaderprogram="standard_surface">
    <input name="base" type="float" value="1.0" publicname="base_public"/>
  </shader>
  <material name="legacy_paint">
    <shaderref name="standard_surface" shadertype="surface"/>
    <override name="base_public" type="float" value="0.5"/>
  </material>
</materialx>)";
  if (!legacy_override_document.ParseFromXML(legacy_override_xml) ||
      legacy_override_document.GetVersion() != "1.39" ||
      !legacy_override_document.FindNode("standard_surface") ||
      legacy_override_document.FindNode("standard_surface")->GetCategory() !=
          "standard_surface" ||
      !legacy_override_document.FindNode("standard_surface")->GetInput("base") ||
      !legacy_override_document.FindNode("standard_surface")
           ->GetInput("base")
           ->GetValueAsFloat(legacy_constant_value) ||
      legacy_constant_value != 0.5f ||
      !legacy_override_document.FindMaterial("legacy_paint") ||
      legacy_override_document.FindMaterial("legacy_paint")->GetSurfaceShader() !=
          "standard_surface") {
    Fail("MTLX_VERSION_UPGRADE_125", 0,
         "legacy shader override was not applied to the upgraded node");
  }

  g_checks++;
  MtlxDocument legacy_bsdf_document;
  const std::string legacy_bsdf_xml = R"(<materialx version="1.37">
  <nodegraph name="legacy_bsdf">
    <diffuse_brdf name="base" type="BSDF"/>
    <dielectric_brdf name="coat" type="BSDF">
      <input name="base" type="BSDF" nodename="base"/>
    </dielectric_brdf>
    <conductor_brdf name="metal" type="BSDF">
      <input name="reflectivity" type="color3" value="0.5, 0.5, 0.5"/>
      <input name="edge_color" type="color3" value="1, 1, 1"/>
    </conductor_brdf>
  </nodegraph>
</materialx>)";
  MtlxNodeGraphPtr legacy_bsdf_graph;
  if (!legacy_bsdf_document.ParseFromXML(legacy_bsdf_xml) ||
      !(legacy_bsdf_graph = legacy_bsdf_document.FindNodeGraph("legacy_bsdf")) ||
      !legacy_bsdf_graph->GetNode("coat") ||
      legacy_bsdf_graph->GetNode("coat")->GetCategory() != "layer" ||
      !legacy_bsdf_graph->GetNode("coat__layer_top") ||
      legacy_bsdf_graph->GetNode("coat__layer_top")->GetCategory() !=
          "dielectric_bsdf" ||
      !legacy_bsdf_graph->GetNode("metal") ||
      legacy_bsdf_graph->GetNode("metal")->GetCategory() != "conductor_bsdf" ||
      !legacy_bsdf_graph->GetNode("metal__artistic_ior") ||
      legacy_bsdf_graph->GetNode("metal__artistic_ior")->GetCategory() !=
          "artistic_ior") {
    Fail("MTLX_VERSION_UPGRADE_BSDF", 0,
         "legacy BSDF layering or conductor inputs were not upgraded");
  }

  // Expected port names and preserved values were checked with MaterialX
  // 1.39.5 Document::upgradeVersion for each source document version.
  auto check_atan2_upgrade = [](const char *version, float expected_inx,
                                float expected_iny, const char *case_name) {
    MtlxDocument document;
    const std::string xml = std::string("<materialx version=\"") + version +
        "\"><nodegraph name=\"g\"><atan2 name=\"n\" type=\"float\">"
        "<input name=\"in1\" type=\"float\" value=\"1\"/>"
        "<input name=\"in2\" type=\"float\" value=\"2\"/>"
        "</atan2></nodegraph></materialx>";
    if (!document.ParseFromXML(xml)) {
      Fail(case_name, 0, document.GetError());
      return;
    }
    const MtlxNodeGraphPtr graph = document.FindNodeGraph("g");
    const MtlxNodePtr node = graph ? graph->GetNode("n") : nullptr;
    float inx = 0.0f, iny = 0.0f;
    const MtlxInputPtr inx_input = node ? node->GetInput("inx") : nullptr;
    const MtlxInputPtr iny_input = node ? node->GetInput("iny") : nullptr;
    if (!inx_input || !iny_input || node->GetInput("in1") ||
        node->GetInput("in2") || !inx_input->GetValueAsFloat(inx) ||
        !iny_input->GetValueAsFloat(iny) || inx != expected_inx ||
        iny != expected_iny) {
      Fail(case_name, 0, "atan2 port names/values differ from the reference upgrade");
    }
  };
  check_atan2_upgrade("1.37", 1.0f, 2.0f, "MTLX_VERSION_UPGRADE_ATAN2_137");
  check_atan2_upgrade("1.38", 2.0f, 1.0f, "MTLX_VERSION_UPGRADE_ATAN2_138");

  // MaterialX 1.39.5 lowers both legacy color2 `ra` and vector2 `xy`
  // channel bindings to this same separate2/combine2 graph.
  auto check_color2_swizzle = [](const char *version, const char *source_type,
                                 const char *channels, const char *case_name) {
    MtlxDocument document;
    const std::string xml = std::string("<materialx version=\"") + version +
        "\"><nodegraph name=\"NG\"><constant name=\"c\" type=\"" +
        source_type + "\"><input name=\"value\" type=\"" + source_type +
        "\" value=\"0.25, 0.75\"/></constant>"
        "<convert name=\"v\" type=\"vector2\"><input name=\"in\" type=\"" +
        source_type + "\" nodename=\"c\" channels=\"" + channels +
        "\"/></convert><output name=\"out\" type=\"vector2\" nodename=\"v\"/>"
        "</nodegraph></materialx>";
    if (!document.ParseFromXML(xml)) {
      Fail(case_name, 0, document.GetError());
      return;
    }
    const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
    const MtlxNodePtr source = graph ? graph->GetNode("c") : nullptr;
    const MtlxNodePtr convert = graph ? graph->GetNode("v") : nullptr;
    const MtlxNodePtr separate = graph ? graph->GetNode("separate") : nullptr;
    const MtlxNodePtr combine = graph ? graph->GetNode("swizzle") : nullptr;
    const MtlxInputPtr convert_input = convert ? convert->GetInput("in") : nullptr;
    const MtlxInputPtr separate_input = separate ? separate->GetInput("in") : nullptr;
    const MtlxInputPtr combine_x = combine ? combine->GetInput("in1") : nullptr;
    const MtlxInputPtr combine_y = combine ? combine->GetInput("in2") : nullptr;
    if (!source || source->GetType() != "vector2" || !convert_input ||
        convert_input->GetNodeName() != "swizzle" ||
        !convert_input->GetChannels().empty() || !separate ||
        separate->GetCategory() != "separate2" || !separate_input ||
        separate_input->GetNodeName() != "c" || !combine ||
        combine->GetCategory() != "combine2" || !combine_x || !combine_y ||
        combine_x->GetNodeName() != "separate" ||
        combine_x->GetOutput() != "outx" ||
        combine_y->GetNodeName() != "separate" ||
        combine_y->GetOutput() != "outy") {
      Fail(case_name, 0, "color2/channel binding differs from MaterialX upgrade");
    }
  };
  check_color2_swizzle("1.37", "color2", "ra",
                       "MTLX_VERSION_UPGRADE_COLOR2_CHANNELS");
  check_color2_swizzle("1.38", "vector2", "xy",
                       "MTLX_VERSION_UPGRADE_CHANNELS_138");

  auto parse_channel_case = [](const char *xml, MtlxDocument *document,
                               const char *case_name) {
    if (document->ParseFromXML(xml)) return true;
    Fail(case_name, 0, document->GetError());
    return false;
  };
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <constant name="c" type="vector3"><input name="value" type="vector3" value="0.25, 0.5, 0.75"/></constant>
      <convert name="v" type="float"><input name="in" type="float" nodename="c" channels="y"/></convert>
    </nodegraph></materialx>)";
    if (parse_channel_case(xml, &document, "MTLX_VERSION_UPGRADE_EXTRACT")) {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr extract = graph ? graph->GetNode("swizzle") : nullptr;
      const MtlxInputPtr extract_in = extract ? extract->GetInput("in") : nullptr;
      const MtlxInputPtr index = extract ? extract->GetInput("index") : nullptr;
      int index_value = -1;
      const MtlxNodePtr convert = graph ? graph->GetNode("v") : nullptr;
      const MtlxInputPtr convert_in = convert ? convert->GetInput("in") : nullptr;
      if (!extract || extract->GetCategory() != "extract" ||
          extract->GetType() != "float" || !extract_in ||
          extract_in->GetType() != "vector3" ||
          extract_in->GetNodeName() != "c" || !index ||
          !index->GetValueAsInt(index_value) || index_value != 1 ||
          !convert_in || convert_in->GetNodeName() != "swizzle" ||
          !convert_in->GetChannels().empty()) {
        Fail("MTLX_VERSION_UPGRADE_EXTRACT", 0,
             "scalar channel selection was not lowered to extract");
      }
    }
  }
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <constant name="c" type="float"><input name="value" type="float" value="0.25"/></constant>
      <convert name="v" type="vector3"><input name="in" type="vector3" nodename="c" channels="x0x"/></convert>
    </nodegraph></materialx>)";
    if (parse_channel_case(xml, &document, "MTLX_VERSION_UPGRADE_SCALAR_BROADCAST")) {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr combine = graph ? graph->GetNode("swizzle") : nullptr;
      const MtlxNodePtr convert = graph ? graph->GetNode("v") : nullptr;
      const MtlxInputPtr convert_in = convert ? convert->GetInput("in") : nullptr;
      bool valid = combine && combine->GetCategory() == "combine3" &&
          convert_in && convert_in->GetNodeName() == "swizzle";
      const MtlxInputPtr x1 = combine ? combine->GetInput("in1") : nullptr;
      const MtlxInputPtr zero = combine ? combine->GetInput("in2") : nullptr;
      const MtlxInputPtr x2 = combine ? combine->GetInput("in3") : nullptr;
      float zero_value = -1.0f;
      valid = valid && x1 && x1->GetType() == "float" &&
          x1->GetNodeName() == "c" && x1->GetChannels().empty() && zero &&
          zero->GetValueAsFloat(zero_value) && zero_value == 0.0f && x2 &&
          x2->GetType() == "float" && x2->GetNodeName() == "c" &&
          x2->GetChannels().empty();
      if (!valid) {
        Fail("MTLX_VERSION_UPGRADE_SCALAR_BROADCAST", 0,
             "scalar channel broadcast was not lowered to combine3");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <constant name="c" type="vector3"><input name="value" type="vector3" value="0.25, 0.5, 0.75"/></constant>
      <convert name="v" type="color3"><input name="in" type="color3" nodename="c" channels="rgb"/></convert>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_CHANNEL_CONVERT", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr swizzle = graph ? graph->GetNode("swizzle") : nullptr;
      const MtlxNodePtr convert = graph ? graph->GetNode("v") : nullptr;
      const MtlxInputPtr source = swizzle ? swizzle->GetInput("in") : nullptr;
      const MtlxInputPtr convert_in = convert ? convert->GetInput("in") : nullptr;
      if (!swizzle || swizzle->GetCategory() != "convert" ||
          swizzle->GetType() != "color3" || !source ||
          source->GetType() != "vector3" || source->GetNodeName() != "c" ||
          !source->GetChannels().empty() || !convert_in ||
          convert_in->GetNodeName() != "swizzle" ||
          !convert_in->GetChannels().empty()) {
        Fail("MTLX_VERSION_UPGRADE_CHANNEL_CONVERT", 0,
             "canonical rgb selectors were not lowered to convert");
      }
    }
  }
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <constant name="c" type="vector2"><input name="value" type="vector2" value="0.25, 0.75"/></constant>
      <convert name="v" type="vector3"><input name="in" type="vector3" nodename="c" channels="x01"/></convert>
    </nodegraph></materialx>)";
    if (parse_channel_case(xml, &document, "MTLX_VERSION_UPGRADE_CHANNEL_CONSTANTS")) {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr separate = graph ? graph->GetNode("separate") : nullptr;
      const MtlxNodePtr combine = graph ? graph->GetNode("swizzle") : nullptr;
      const MtlxInputPtr x = combine ? combine->GetInput("in1") : nullptr;
      const MtlxInputPtr zero = combine ? combine->GetInput("in2") : nullptr;
      const MtlxInputPtr one = combine ? combine->GetInput("in3") : nullptr;
      float zero_value = -1.0f, one_value = -1.0f;
      if (!separate || separate->GetCategory() != "separate2" || !combine ||
          combine->GetCategory() != "combine3" || !x ||
          x->GetNodeName() != "separate" || x->GetOutput() != "outx" ||
          !zero || !zero->GetValueAsFloat(zero_value) || zero_value != 0.0f ||
          !one || !one->GetValueAsFloat(one_value) || one_value != 1.0f) {
        Fail("MTLX_VERSION_UPGRADE_CHANNEL_CONSTANTS", 0,
             "constant selectors were not preserved in the combine node");
      }
    }
  }
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <convert name="v" type="vector2"><input name="in" type="vector2" value="0.2, 0.4" channels="01"/></convert>
    </nodegraph></materialx>)";
    if (parse_channel_case(xml, &document, "MTLX_VERSION_UPGRADE_CONSTANT_SWIZZLE")) {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr constant = graph ? graph->GetNode("swizzle") : nullptr;
      const MtlxInputPtr value = constant ? constant->GetInput("value") : nullptr;
      const MtlxNodePtr convert = graph ? graph->GetNode("v") : nullptr;
      const MtlxInputPtr convert_in = convert ? convert->GetInput("in") : nullptr;
      std::vector<float> value_vec;
      if (!constant || constant->GetCategory() != "constant" ||
          constant->GetType() != "vector2" || !value ||
          !value->GetValueAsFloatVector(value_vec) || value_vec.size() != 2 ||
          value_vec[0] != 0.0f || value_vec[1] != 1.0f || !convert_in ||
          convert_in->GetNodeName() != "swizzle" ||
          !convert_in->GetChannels().empty()) {
        Fail("MTLX_VERSION_UPGRADE_CONSTANT_SWIZZLE", 0,
             "constant-only selectors were not lowered to a constant node");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <subsurface_bsdf name="s" type="BSDF"><input name="radius" type="vector3" value="1, 2, 3"/></subsurface_bsdf>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_SUBSURFACE_RADIUS", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr subsurface = graph ? graph->GetNode("s") : nullptr;
      const MtlxInputPtr radius = subsurface ? subsurface->GetInput("radius") : nullptr;
      const MtlxNodePtr convert = graph ? graph->GetNode("convert") : nullptr;
      const MtlxInputPtr convert_in = convert ? convert->GetInput("in") : nullptr;
      std::vector<float> radius_value;
      if (!radius || radius->GetType() != "color3" ||
          radius->GetNodeName() != "convert" || !convert ||
          convert->GetCategory() != "convert" ||
          convert->GetType() != "color3" || !convert_in ||
          convert_in->GetType() != "vector3" ||
          !convert_in->GetValueAsFloatVector(radius_value) ||
          radius_value != std::vector<float>({1.0f, 2.0f, 3.0f})) {
        Fail("MTLX_VERSION_UPGRADE_SUBSURFACE_RADIUS", 0,
             "vector3 radius was not converted to color3");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <switch name="integer_switch" type="float"><input name="which" type="integer" value="5"/></switch>
      <switch name="float_switch" type="float"><input name="which" type="float" value="6.5"/></switch>
      <switch name="in_range" type="float"><input name="which" type="integer" value="4"/></switch>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_SWITCH_RANGE", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr integer_switch = graph ? graph->GetNode("integer_switch") : nullptr;
      const MtlxNodePtr float_switch = graph ? graph->GetNode("float_switch") : nullptr;
      const MtlxNodePtr in_range = graph ? graph->GetNode("in_range") : nullptr;
      int integer_value = -1, in_range_value = -1;
      float float_value = -1.0f;
      if (!integer_switch || !integer_switch->GetInput("which") ||
          !integer_switch->GetInput("which")->GetValueAsInt(integer_value) ||
          integer_value != 0 || !float_switch ||
          !float_switch->GetInput("which") ||
          !float_switch->GetInput("which")->GetValueAsFloat(float_value) ||
          float_value != 0.0f || !in_range ||
          !in_range->GetInput("which") ||
          !in_range->GetInput("which")->GetValueAsInt(in_range_value) ||
          in_range_value != 4) {
        Fail("MTLX_VERSION_UPGRADE_SWITCH_RANGE", 0,
             "out-of-range switch selectors were not reset to zero");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <constant name="src" type="vector3"><input name="value" type="vector3" value="0.25, 0.5, 0.75"/></constant>
      <normalmap name="n" type="vector3" nodedef="ND_normalmap">
        <input name="in" type="vector3" nodename="src"/>
        <input name="space" type="string" value="object"/>
      </normalmap>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_NORMALMAP_OBJECT", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr normal = graph ? graph->GetNode("n") : nullptr;
      const MtlxNodePtr multiply = graph ? graph->GetNode("multiply") : nullptr;
      const MtlxNodePtr subtract = graph ? graph->GetNode("subtract") : nullptr;
      const MtlxInputPtr normal_in = normal ? normal->GetInput("in") : nullptr;
      const MtlxInputPtr multiply_in = multiply ? multiply->GetInput("in1") : nullptr;
      const MtlxInputPtr multiply_factor = multiply ? multiply->GetInput("in2") : nullptr;
      const MtlxInputPtr subtract_in = subtract ? subtract->GetInput("in1") : nullptr;
      const MtlxInputPtr subtract_offset = subtract ? subtract->GetInput("in2") : nullptr;
      float factor = 0.0f, offset = 0.0f;
      if (!normal || normal->GetCategory() != "normalize" ||
          normal->GetNodeDef() != "ND_normalize_vector3" ||
          normal->GetInput("space") || !normal_in ||
          normal_in->GetNodeName() != "subtract" || !multiply ||
          multiply->GetCategory() != "multiply" || !multiply_in ||
          multiply_in->GetNodeName() != "src" || !multiply_factor ||
          !multiply_factor->GetValueAsFloat(factor) || factor != 2.0f ||
          !subtract || subtract->GetCategory() != "subtract" ||
          !subtract_in || subtract_in->GetNodeName() != "multiply" ||
          !subtract_offset || !subtract_offset->GetValueAsFloat(offset) ||
          offset != 1.0f) {
        Fail("MTLX_VERSION_UPGRADE_NORMALMAP_OBJECT", 0,
             "object-space normalmap was not replaced by the reference normalization graph");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <normalmap name="n" type="vector3" nodedef="ND_normalmap">
        <input name="space" type="string" value="tangent"/>
        <input name="normal" type="vector3" nodename="normal_src"/>
      </normalmap>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_NORMALMAP_FRAME", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr normal = graph ? graph->GetNode("n") : nullptr;
      const MtlxNodePtr cross = graph ? graph->GetNode("normalmap_cross") : nullptr;
      const MtlxNodePtr tangent = graph ? graph->GetNode("normalmap_tangent") : nullptr;
      const MtlxNodePtr normalize = graph ? graph->GetNode("normalmap_cross_norm") : nullptr;
      const MtlxInputPtr in1 = cross ? cross->GetInput("in1") : nullptr;
      const MtlxInputPtr in2 = cross ? cross->GetInput("in2") : nullptr;
      const MtlxInputPtr space = tangent ? tangent->GetInput("space") : nullptr;
      const MtlxInputPtr bitangent = normal ? normal->GetInput("bitangent") : nullptr;
      std::string space_value;
      if (!normal || normal->GetNodeDef() != "ND_normalmap_float" ||
          normal->GetInput("space") || !cross ||
          cross->GetCategory() != "crossproduct" || !in1 ||
          in1->GetNodeName() != "normal_src" || !in2 ||
          in2->GetNodeName() != "normalmap_tangent" || !tangent ||
          tangent->GetCategory() != "tangent" || !space ||
          !space->GetValueAsString(space_value) || space_value != "world" ||
          !normalize || normalize->GetCategory() != "normalize" ||
          !bitangent || bitangent->GetNodeName() != "normalmap_cross_norm") {
        Fail("MTLX_VERSION_UPGRADE_NORMALMAP_FRAME", 0,
             "tangent-space frame was not completed from the world tangent");
      }
    }
  }
  g_checks++;
  {
    MtlxDocument document;
    const char *xml = R"(<materialx version="1.38"><nodegraph name="NG">
      <thin_film_bsdf name="film" type="BSDF">
        <input name="thickness" type="float" value="400"/>
        <input name="ior" type="float" value="1.4"/>
      </thin_film_bsdf>
      <dielectric_bsdf name="coat" type="BSDF"><input name="scatter_mode" type="string" value="F"/></dielectric_bsdf>
      <dielectric_bsdf name="transmission" type="BSDF"><input name="scatter_mode" type="string" value="T"/></dielectric_bsdf>
      <layer name="coated" type="BSDF"><input name="top" type="BSDF" nodename="film"/><input name="base" type="BSDF" nodename="coat"/></layer>
      <layer name="transmission_layer" type="BSDF"><input name="top" type="BSDF" nodename="film"/><input name="base" type="BSDF" nodename="transmission"/></layer>
      <output name="out" type="BSDF" nodename="coated"/>
      <output name="transmission_out" type="BSDF" nodename="transmission_layer"/>
    </nodegraph></materialx>)";
    if (!document.ParseFromXML(xml)) {
      Fail("MTLX_VERSION_UPGRADE_THIN_FILM_LAYER", 0, document.GetError());
    } else {
      const MtlxNodeGraphPtr graph = document.FindNodeGraph("NG");
      const MtlxNodePtr coat = graph ? graph->GetNode("coat") : nullptr;
      const MtlxNodePtr transmission = graph ? graph->GetNode("transmission") : nullptr;
      const MtlxInputPtr thickness = coat ? coat->GetInput("thinfilm_thickness") : nullptr;
      const MtlxInputPtr ior = coat ? coat->GetInput("thinfilm_ior") : nullptr;
      float thickness_value = 0.0f, ior_value = 0.0f;
      const auto &outputs = graph ? graph->GetOutputs() : std::vector<MtlxOutputPtr>();
      if (!graph || graph->GetNode("film") || graph->GetNode("coated") ||
          graph->GetNode("transmission_layer") || !coat || !thickness ||
          !thickness->GetValueAsFloat(thickness_value) ||
          thickness_value != 400.0f || !ior || !ior->GetValueAsFloat(ior_value) ||
          std::fabs(ior_value - 1.4f) > 1.0e-6f || !transmission ||
          transmission->GetInput("thinfilm_thickness") || outputs.size() != 2 ||
          outputs[0]->GetNodeName() != "coat" ||
          outputs[1]->GetNodeName() != "transmission") {
        Fail("MTLX_VERSION_UPGRADE_THIN_FILM_LAYER", 0,
             "thin-film layering was not folded into eligible BSDFs");
      }
    }
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
  CheckTypedMtlxGraphResolution();
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
