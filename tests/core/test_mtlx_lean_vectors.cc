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
  g_checks++;
  MtlxDocument look_document;
  const std::string look_xml = R"(<materialx version="1.39">
  <look name="preview_look">
    <materialassign name="body" geom="/World/Body" material="paint"/>
    <materialassign name="set" collection="hero_collection" material="metal"/>
  </look>
</materialx>)";
  const bool look_parsed = look_document.ParseFromXML(look_xml);
  const MtlxLookPtr look = look_document.FindLook("preview_look");
  const MtlxMaterialAssignmentPtr geometry_assignment =
      look ? look->GetMaterialAssignment("body") : nullptr;
  const MtlxMaterialAssignmentPtr collection_assignment =
      look ? look->GetMaterialAssignment("set") : nullptr;
  if (!look_parsed || look_document.GetLooks().size() != 1 || !look ||
      look->GetMaterialAssignments().size() != 2 || !geometry_assignment ||
      geometry_assignment->GetGeom() != "/World/Body" ||
      geometry_assignment->GetMaterial() != "paint" ||
      !collection_assignment || !collection_assignment->GetGeom().empty() ||
      collection_assignment->GetCollection() != "hero_collection" ||
      collection_assignment->GetMaterial() != "metal") {
    Fail("MTLX_LOOK_MATERIAL_ASSIGN", 0,
         "look material assignments did not retain geometry and collection selectors");
  }

  g_checks++;
  MtlxDocument unit_document;
  const bool unit_parsed = unit_document.ParseFromXML(
      R"(<materialx version="1.39"><constant name="distance" type="float">
      <input name="value" type="float" value="2.5" unit="centimeter" unittype="distance"/>
      </constant></materialx>)");
  const MtlxNodePtr distance_node = unit_document.FindNode("distance");
  const MtlxInputPtr distance_input =
      distance_node ? distance_node->GetInput("value") : nullptr;
  if (!unit_parsed || !distance_input ||
      distance_input->GetUnit() != "centimeter" ||
      distance_input->GetUnitType() != "distance") {
    Fail("MTLX_UNIT_METADATA", 0,
         "authored unit and unit-type metadata were not retained");
  }

  g_checks++;
  MtlxDocument assignment_document;
  const bool assignments_parsed = assignment_document.ParseFromXML(
      R"(<materialx version="1.39">
      <surfacematerial name="world_material"/>
      <surfacematerial name="hero_material"/>
      <collection name="hero_collection" includegeom="/World/Hero"
                  excludegeom="/World/Hero/Hidden"/>
      <collection name="visible_heroes" includecollection="hero_collection"/>
      <look name="main">
        <materialassign name="world" geom="/World" material="world_material"/>
        <materialassign name="hero" collection="visible_heroes" material="hero_material"/>
      </look>
      <look name="derived" inherit="main">
        <materialassign name="derived_assignment" geom="/World/Hero" material="hero_material"/>
      </look>
      </materialx>)");
  std::vector<MtlxResolvedMaterialAssignment> resolved_assignments;
  std::string assignment_error;
  const bool hero_assignments_ok = assignments_parsed &&
      assignment_document.GetMaterialAssignmentsForGeometry(
          "main", "/World/Hero/Body", &resolved_assignments,
          &assignment_error);
  const bool hero_ordered = resolved_assignments.size() == 2 &&
      resolved_assignments[0].assignment && resolved_assignments[0].material &&
      resolved_assignments[0].assignment->GetName() == "world" &&
      resolved_assignments[0].material->GetName() == "world_material" &&
      resolved_assignments[1].assignment && resolved_assignments[1].material &&
      resolved_assignments[1].assignment->GetName() == "hero" &&
      resolved_assignments[1].material->GetName() == "hero_material";
  const bool excluded_hero_ok = assignment_document
      .GetMaterialAssignmentsForGeometry("main", "/World/Hero/Hidden/Body",
                                          &resolved_assignments,
                                          &assignment_error) &&
      resolved_assignments.size() == 1 &&
      resolved_assignments[0].assignment->GetName() == "world";
  if (!hero_assignments_ok || !hero_ordered || !excluded_hero_ok ||
      !assignment_error.empty()) {
    Fail("MTLX_LOOK_ASSIGNMENT_RESOLVE", 0,
         "geometry and nested collection assignments did not resolve in source order with exclusions");
  }
  g_checks++;
  if (!assignment_document.GetMaterialAssignmentsForGeometry(
          "derived", "/World/Hero/Body", &resolved_assignments,
          &assignment_error) || resolved_assignments.size() != 3 ||
      resolved_assignments[0].assignment->GetName() != "derived_assignment" ||
      resolved_assignments[1].assignment->GetName() != "world" ||
      resolved_assignments[2].assignment->GetName() != "hero") {
    Fail("MTLX_LOOK_INHERITANCE", 0, "inherited assignments were not returned in derived-first order");
  }

  g_checks++;
  MtlxDocument invalid_look_document;
  const bool invalid_looks_parsed = invalid_look_document.ParseFromXML(
      R"(<materialx version="1.39">
      <surfacematerial name="paint"/>
      <look name="first" inherit="second">
        <materialassign name="early" geom="/World" material="paint"/>
      </look>
      <look name="second" inherit="first"/>
      <look name="missing" inherit="absent">
        <materialassign name="early" geom="/World" material="paint"/>
      </look>
      </materialx>)");
  const bool cycle_failed = invalid_looks_parsed &&
      !invalid_look_document.GetMaterialAssignmentsForGeometry(
          "first", "/World/Body", &resolved_assignments, &assignment_error) &&
      assignment_error.find("Cycle") != std::string::npos &&
      resolved_assignments.empty();
  const bool missing_failed = invalid_looks_parsed &&
      !invalid_look_document.GetMaterialAssignmentsForGeometry(
          "missing", "/World/Body", &resolved_assignments, &assignment_error) &&
      assignment_error.find("absent") != std::string::npos &&
      resolved_assignments.empty();
  if (!cycle_failed || !missing_failed) {
    Fail("MTLX_LOOK_INHERITANCE_ERRORS", 0,
         "invalid inheritance did not fail without returning partial assignments");
  }

  g_checks++;
  MtlxDocument collection_cycle_document;
  const bool cycle_parsed = collection_cycle_document.ParseFromXML(
      R"(<materialx version="1.39">
      <surfacematerial name="paint"/>
      <collection name="first" includecollection="second"/>
      <collection name="second" includecollection="first"/>
      <look name="cycle_look">
        <materialassign name="cycle" collection="first" material="paint"/>
      </look>
      </materialx>)");
  const bool cycle_rejected = cycle_parsed &&
      !collection_cycle_document.GetMaterialAssignmentsForGeometry(
          "cycle_look", "/World/Body", &resolved_assignments,
          &assignment_error) &&
      assignment_error.find("Cycle") != std::string::npos &&
      resolved_assignments.empty();
  if (!cycle_rejected) {
    Fail("MTLX_LOOK_COLLECTION_CYCLE", 0,
         "cyclic collection assignments were not rejected atomically");
  }

  g_checks++;
  MtlxDocument conversion_document;
  const bool conversion_parsed = conversion_document.ParseFromXML(
      R"(<materialx version="1.39">
      <unittypedef name="distance"/>
      <unitdef name="UD_test_distance" unittype="distance">
        <unit name="meter" scale="1.0"/>
        <unit name="centimeter" scale="0.01"/>
      </unitdef>
      <constant name="length" type="float">
        <input name="value" type="float" value="250" unit="centimeter" unittype="distance"/>
      </constant>
      <nodegraph name="graph">
        <input name="interface_length" type="float" value="250" unit="centimeter" unittype="distance"/>
        <constant name="position" type="vector3">
          <input name="value" type="vector3" value="100, 200, 300" unit="centimeter" unittype="distance"/>
        </constant>
      </nodegraph>
      <nodedef name="ND_test_length" node="testlength" type="float">
        <input name="value" type="float" value="250" unit="centimeter" unittype="distance"/>
      </nodedef>
      </materialx>)");
  const bool conversion_ok =
      conversion_parsed && conversion_document.ConvertToUnit("distance", "meter");
  const MtlxInputPtr converted_length = conversion_document.FindNode("length")
      ? conversion_document.FindNode("length")->GetInput("value")
      : nullptr;
  const MtlxNodeGraphPtr conversion_graph =
      conversion_document.FindNodeGraph("graph");
  const MtlxNodePtr converted_position =
      conversion_graph ? conversion_graph->GetNode("position") : nullptr;
  const MtlxInputPtr converted_position_value =
      converted_position ? converted_position->GetInput("value") : nullptr;
  const MtlxInputPtr graph_interface = conversion_graph &&
          !conversion_graph->GetInputs().empty()
      ? conversion_graph->GetInputs().front()
      : nullptr;
  float converted_length_value = 0.0f;
  std::vector<float> converted_position_values;
  const std::string converted_xml = conversion_document.GetResolvedXML();
  const size_t nodedef_start = converted_xml.find("<nodedef");
  const size_t nodedef_end = converted_xml.find("</nodedef>", nodedef_start);
  const bool nodedef_units_retained =
      nodedef_start != std::string::npos && nodedef_end != std::string::npos &&
      converted_xml.substr(nodedef_start, nodedef_end - nodedef_start)
              .find("unittype=\"distance\"") != std::string::npos;
  if (!conversion_ok || !converted_length || !converted_position_value ||
      !converted_length->GetValueAsFloat(converted_length_value) ||
      std::fabs(converted_length_value - 2.5f) > 1e-6f ||
      !converted_length->GetUnit().empty() ||
      !converted_length->GetUnitType().empty() ||
      !converted_position_value->GetValueAsFloatVector(
          converted_position_values) ||
      converted_position_values.size() != 3 ||
      std::fabs(converted_position_values[0] - 1.0f) > 1e-6f ||
      std::fabs(converted_position_values[1] - 2.0f) > 1e-6f ||
      std::fabs(converted_position_values[2] - 3.0f) > 1e-6f ||
      !graph_interface || graph_interface->GetUnit() != "centimeter" ||
      !nodedef_units_retained) {
    Fail("MTLX_UNIT_CONVERSION", 0,
         "unit conversion values, metadata cleanup, or nodegraph interface handling differed");
  }

  g_checks++;
  MtlxDocument invalid_conversion_document;
  const bool invalid_conversion_parsed =
      invalid_conversion_document.ParseFromXML(
          R"(<materialx version="1.39">
          <unittypedef name="distance"/>
          <unitdef name="UD_test_distance" unittype="distance">
            <unit name="meter" scale="1.0"/>
            <unit name="centimeter" scale="0.01"/>
          </unitdef>
          <constant name="length" type="float">
            <input name="value" type="float" value="250" unit="centimeter" unittype="distance"/>
          </constant>
          </materialx>)");
  const std::string unconverted_xml =
      invalid_conversion_document.GetResolvedXML();
  if (!invalid_conversion_parsed ||
      invalid_conversion_document.ConvertToUnit("distance", "unknown") ||
      invalid_conversion_document.GetError().empty() ||
      invalid_conversion_document.GetResolvedXML() != unconverted_xml) {
    Fail("MTLX_UNIT_CONVERSION_ATOMIC", 0,
         "an unknown target unit changed the document or failed without an error");
  }

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

  // Inline a graph call whose interface is fed by an upstream node.
  MtlxElem inline_doc = elem("materialx", "");
  MtlxElem body = elem("nodegraph", "test_impl");
  body.SetAttr("nodedef", "ND_test_graph");
  MtlxElem body_node = add_node("body_sum");
  MtlxElem body_input = elem("input", "in1");
  body_input.SetAttr("type", "float");
  body_input.SetAttr("interfacename", "gain");
  body_node.children.push_back(body_input);
  body.children.push_back(body_node);
  MtlxElem body_output = elem("output", "out");
  body_output.SetAttr("type", "float");
  body_output.SetAttr("nodename", "body_sum");
  body.children.push_back(body_output);
  inline_doc.children.push_back(body);
  MtlxElem caller = elem("nodegraph", "caller");
  caller.children.push_back(constant_node("seed"));
  MtlxElem call = elem("testgraph", "call");
  call.SetAttr("type", "float");
  call.SetAttr("nodedef", "ND_test_graph");
  MtlxElem call_input = elem("input", "gain");
  call_input.SetAttr("type", "float");
  call_input.SetAttr("nodename", "seed");
  call.children.push_back(call_input);
  caller.children.push_back(call);
  MtlxElem caller_output = elem("output", "out");
  caller_output.SetAttr("type", "float");
  caller_output.SetAttr("nodename", "call");
  caller.children.push_back(caller_output);
  inline_doc.children.push_back(caller);
  MtlxElem flattened;
  std::string inline_error;
  g_checks++;
  if (!FlattenMtlxGraphCall(inline_doc, caller, library, {}, "call",
                            &flattened, &inline_error) ||
      flattened.children.size() != 3 ||
      flattened.children[1].tag != "testadd" ||
      flattened.children[1].children[0].AttrOr("nodename") != "seed" ||
      flattened.children[2].AttrOr("nodename") != flattened.children[1].Name()) {
    Fail("MTLX_GRAPH_INLINE", 0, inline_error.empty() ?
         "flattened graph connections differed" : inline_error);
  }
  MtlxElem metadata_caller = caller;
  metadata_caller.children[1].children[0].SetAttr("unit", "meter");
  metadata_caller.children[1].children[0].SetAttr("unittype", "distance");
  g_checks++;
  if (!FlattenMtlxGraphCall(inline_doc, metadata_caller, library, {}, "call",
                            &flattened, &inline_error) ||
      flattened.children[1].children[0].AttrOr("unit") != "meter" ||
      flattened.children[1].children[0].AttrOr("unittype") != "distance") {
    Fail("MTLX_GRAPH_INLINE_INPUT_METADATA", 0, inline_error.empty() ?
         "authored call input metadata was not transferred" : inline_error);
  }
  MtlxLibrary default_library;
  default_library.AddNodeDef(constant_def, {});
  default_library.AddNodeDef(add_def, {});
  MtlxElem default_graph_def = graph_def;
  default_graph_def.children[0].SetAttr("value", "0.75");
  default_library.AddNodeDef(default_graph_def, {});
  MtlxElem default_caller = caller;
  default_caller.children[1].children.clear();
  g_checks++;
  if (!FlattenMtlxGraphCall(inline_doc, default_caller, default_library, {},
                            "call", &flattened, &inline_error) ||
      flattened.children[1].children[0].AttrOr("value") != "0.75" ||
      flattened.children[1].children[0].Attr("interfacename")) {
    Fail("MTLX_GRAPH_INLINE_DEFAULT", 0, inline_error.empty() ?
         "nodedef input default was not transferred" : inline_error);
  }
  MtlxLibrary geomprop_library;
  geomprop_library.AddNodeDef(constant_def, {});
  geomprop_library.AddNodeDef(add_def, {});
  default_graph_def.children[0].SetAttr("defaultgeomprop", "Pworld");
  geomprop_library.AddNodeDef(default_graph_def, {});
  MtlxElem geomprop_result = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraphCall(inline_doc, default_caller, geomprop_library, {},
                           "call", &geomprop_result, &inline_error) ||
      geomprop_result.Name() != "unchanged" ||
      inline_error.find("geometry") == std::string::npos) {
    Fail("MTLX_GRAPH_INLINE_DEFAULT_GEOMPROP", 0,
         "geometry-property default was silently replaced by a literal");
  }
  MtlxElem geomprop_doc = inline_doc;
  MtlxElem geomdef = elem("geompropdef", "Pworld");
  geomdef.SetAttr("type", "float");
  geomdef.SetAttr("geomprop", "testconstant");
  geomprop_doc.children.push_back(geomdef);
  MtlxElem second_geom_input = body_input;
  second_geom_input.SetAttr("name", "in2");
  geomprop_doc.children[0].children[0].children.push_back(second_geom_input);
  g_checks++;
  if (!FlattenMtlxGraphCall(geomprop_doc, default_caller, geomprop_library, {},
                            "call", &flattened, &inline_error) ||
      flattened.children.size() != 4 ||
      flattened.children[3].tag != "testconstant" ||
      flattened.children[1].children.size() != 2 ||
      flattened.children[1].children[0].AttrOr("nodename") != flattened.children[3].Name() ||
      flattened.children[1].children[1].AttrOr("nodename") != flattened.children[3].Name() ||
      flattened.children[1].children[0].Attr("value")) {
    Fail("MTLX_GRAPH_INLINE_GEOMPROP", 0, inline_error.empty() ?
         "geometry node was not generated and shared" : inline_error);
  }
  MtlxElem geometry_node_def = elem("nodedef", "ND_test_geometry");
  geometry_node_def.SetAttr("node", "testgeometry");
  geometry_node_def.SetAttr("type", "float");
  add_port(&geometry_node_def, "input", "space", "string");
  add_port(&geometry_node_def, "output", "out", "float");
  geomprop_library.AddNodeDef(geometry_node_def, {});
  MtlxElem spatial_geomprop_doc = geomprop_doc;
  spatial_geomprop_doc.children.back().SetAttr("geomprop", "testgeometry");
  spatial_geomprop_doc.children.back().SetAttr("space", "world");
  g_checks++;
  if (!FlattenMtlxGraphCall(spatial_geomprop_doc, default_caller, geomprop_library, {},
                            "call", &flattened, &inline_error) ||
      flattened.children.size() != 4 || flattened.children[3].tag != "testgeometry" ||
      flattened.children[3].children.size() != 1 ||
      flattened.children[3].children[0].AttrOr("name") != "space" ||
      flattened.children[3].children[0].AttrOr("type") != "string" ||
      flattened.children[3].children[0].AttrOr("value") != "world") {
    Fail("MTLX_GRAPH_INLINE_GEOMPROP_SPACE", 0, inline_error.empty() ?
         "geometry property space was not transferred" : inline_error);
  }
  MtlxElem ambiguous_geomprop_doc = geomprop_doc;
  ambiguous_geomprop_doc.children.push_back(geomdef);
  MtlxElem ambiguous_geomprop_result = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraphCall(ambiguous_geomprop_doc, default_caller, geomprop_library, {},
                           "call", &ambiguous_geomprop_result, &inline_error) ||
      ambiguous_geomprop_result.Name() != "unchanged" ||
      inline_error.find("ambiguous") == std::string::npos) {
    Fail("MTLX_GRAPH_INLINE_GEOMPROP_AMBIGUOUS", 0,
         "duplicate geometry definitions were accepted or changed the output");
  }
  MtlxElem wrong_geomprop_doc = geomprop_doc;
  wrong_geomprop_doc.children.back().SetAttr("type", "vector3");
  MtlxElem wrong_geomprop_result = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraphCall(wrong_geomprop_doc, default_caller, geomprop_library, {},
                           "call", &wrong_geomprop_result, &inline_error) ||
      wrong_geomprop_result.Name() != "unchanged" ||
      inline_error.find("geometry") == std::string::npos) {
    Fail("MTLX_GRAPH_INLINE_GEOMPROP_TYPE", 0,
         "mismatched geometry type was accepted or changed the output");
  }
  MtlxElem downstream = caller;
  MtlxElem consumer = add_node("consumer");
  MtlxElem consumer_input = elem("input", "in1");
  consumer_input.SetAttr("type", "float");
  consumer_input.SetAttr("nodename", "call");
  consumer.children.push_back(consumer_input);
  downstream.children.insert(downstream.children.end() - 1, consumer);
  downstream.children.back().SetAttr("nodename", "consumer");
  g_checks++;
  if (!FlattenMtlxGraphCall(inline_doc, downstream, library, {}, "call",
                            &flattened, &inline_error) ||
      flattened.children.size() != 4 ||
      flattened.children[2].children[0].AttrOr("nodename") !=
          flattened.children[1].Name() ||
      flattened.children[3].AttrOr("nodename") != "consumer") {
    Fail("MTLX_GRAPH_INLINE_CONSUMER", 0, inline_error.empty() ?
         "downstream node was not rewired" : inline_error);
  }
  MtlxElem literal_doc = inline_doc;
  literal_doc.children[0].children.erase(literal_doc.children[0].children.begin());
  MtlxElem &inline_literal_output = literal_doc.children[0].children[0];
  for (auto it = inline_literal_output.attrs.begin(); it != inline_literal_output.attrs.end(); ++it) {
    if (it->first == "nodename") { inline_literal_output.attrs.erase(it); break; }
  }
  inline_literal_output.SetAttr("value", "0.25");
  g_checks++;
  if (!FlattenMtlxGraphCall(literal_doc, downstream, library, {}, "call",
                            &flattened, &inline_error) ||
      flattened.children.size() != 3 ||
      flattened.children[1].children[0].AttrOr("value") != "0.25" ||
      flattened.children[1].children[0].Attr("nodename")) {
    Fail("MTLX_GRAPH_INLINE_LITERAL", 0, inline_error.empty() ?
         "literal implementation output was not propagated" : inline_error);
  }
  MtlxElem linked_doc = elem("materialx", "");
  MtlxElem linked_body = body;
  for (auto it = linked_body.attrs.begin(); it != linked_body.attrs.end(); ++it) {
    if (it->first == "nodedef") { linked_body.attrs.erase(it); break; }
  }
  linked_doc.children.push_back(linked_body);
  MtlxElem link = elem("implementation", "test_link");
  link.SetAttr("nodedef", "ND_test_graph");
  link.SetAttr("nodegraph", "test_impl");
  linked_doc.children.push_back(link);
  linked_doc.children.push_back(caller);
  g_checks++;
  if (!FlattenMtlxGraphCall(linked_doc, caller, library, {}, "call",
                            &flattened, &inline_error) ||
      flattened.children.size() != 3 ||
      flattened.children[2].AttrOr("nodename") != flattened.children[1].Name()) {
    Fail("MTLX_GRAPH_INLINE_LINKED", 0, inline_error.empty() ?
         "linked graph was not flattened" : inline_error);
  }
  MtlxElem outer_def = elem("nodedef", "ND_test_outer");
  outer_def.SetAttr("node", "testouter");
  outer_def.SetAttr("type", "float");
  add_port(&outer_def, "input", "gain", "float");
  add_port(&outer_def, "output", "out", "float");
  library.AddNodeDef(outer_def, {});
  MtlxElem outer_body = elem("nodegraph", "outer_impl");
  outer_body.SetAttr("nodedef", "ND_test_outer");
  MtlxElem inner_call = call;
  inner_call.SetAttr("name", "inner");
  inner_call.children[0] = body_input;
  inner_call.children[0].SetAttr("name", "gain");
  outer_body.children.push_back(inner_call);
  MtlxElem outer_output = body_output;
  outer_output.SetAttr("nodename", "inner");
  outer_body.children.push_back(outer_output);
  MtlxElem nested_doc = inline_doc;
  nested_doc.children.push_back(outer_body);
  MtlxElem shader_impl = elem("implementation", "test_add_glsl");
  shader_impl.SetAttr("nodedef", "ND_test_add");
  shader_impl.SetAttr("target", "genglsl");
  shader_impl.SetAttr("file", "test.glsl");
  nested_doc.children.push_back(shader_impl);
  shader_impl.SetAttr("name", "test_add_mdl");
  shader_impl.SetAttr("target", "genmdl");
  nested_doc.children.push_back(shader_impl);
  MtlxElem nested_caller = caller;
  nested_caller.children[1].tag = "testouter";
  nested_caller.children[1].SetAttr("nodedef", "ND_test_outer");
  g_checks++;
  if (!FlattenMtlxGraph(nested_doc, nested_caller, library, {}, 2,
                        &flattened, &inline_error) ||
      flattened.children.size() != 3 ||
      flattened.children[1].tag != "testadd" ||
      flattened.children[1].children[0].AttrOr("nodename") != "seed" ||
      flattened.children[2].AttrOr("nodename") != flattened.children[1].Name()) {
    Fail("MTLX_GRAPH_INLINE_NESTED", 0, inline_error.empty() ?
         "nested graph was not flattened" : inline_error);
  }
  MtlxElem limited = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraph(nested_doc, nested_caller, library, {}, 1,
                       &limited, &inline_error) ||
      limited.Name() != "unchanged" || inline_error.find("limit") == std::string::npos) {
    Fail("MTLX_GRAPH_INLINE_LIMIT", 0, "expansion limit did not fail atomically");
  }
  MtlxElem recursive_doc = inline_doc;
  recursive_doc.children[0].children[0] = inner_call;
  recursive_doc.children[0].children[1].SetAttr("nodename", "inner");
  MtlxElem recursive_result = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraph(recursive_doc, caller, library, {}, 10,
                       &recursive_result, &inline_error) ||
      recursive_result.Name() != "unchanged" ||
      inline_error.find("recursive") == std::string::npos) {
    Fail("MTLX_GRAPH_INLINE_RECURSIVE", 0,
         inline_error.empty() ? "recursive implementation was accepted" : inline_error);
  }
  MtlxElem targeted = body;
  targeted.SetAttr("name", "targeted_impl");
  targeted.SetAttr("target", "genglsl");
  inline_doc.children.push_back(targeted);
  MtlxElem sentinel = elem("nodegraph", "unchanged");
  g_checks++;
  if (FlattenMtlxGraphCall(inline_doc, caller, library, {}, "call",
                           &sentinel, &inline_error) ||
      sentinel.Name() != "unchanged") {
    Fail("MTLX_GRAPH_INLINE_AMBIGUOUS", 0,
         "ambiguous implementation was accepted or output was modified");
  }

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

  // The reference permits a graph output to carry a literal value.
  MtlxElem literal_root = root;
  MtlxElem literal_output = elem("output", "value");
  literal_output.SetAttr("type", "float");
  literal_output.SetAttr("value", "0.25");
  literal_root.children[0].children[1] = literal_output;
  g_checks++;
  if (!ResolveMtlxGraph(literal_root, literal_root.children[0], library, {},
                        &resolved, &error)) {
    Fail("MTLX_GRAPH_LITERAL_OUTPUT", 0, error);
  }
  g_checks++;
  if (ResolveMtlxGraph(literal_root, literal_root.children[1], library, {},
                       &resolved, &error)) {
    Fail("MTLX_GRAPH_LITERAL_OUTPUT_CONNECTION", 0,
         "reference-invalid connection to a literal graph output was accepted");
  }
  MtlxElem invalid_literal_root = literal_root;
  invalid_literal_root.children[0].children[1].SetAttr("value", "not-a-float");
  g_checks++;
  if (ResolveMtlxGraph(invalid_literal_root, invalid_literal_root.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_LITERAL_OUTPUT_INVALID", 0, "invalid graph output literal was accepted");
  }
  g_checks++;
  if (ResolveMtlxGraph(invalid_literal_root, invalid_literal_root.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_LITERAL_OUTPUT_CONNECTION_INVALID", 0,
         "connection to invalid graph output literal was accepted");
  }
  MtlxElem wrong_literal_type = literal_root;
  wrong_literal_type.children[0].children[1].SetAttr("type", "integer");
  wrong_literal_type.children[0].children[1].SetAttr("value", "1");
  g_checks++;
  if (ResolveMtlxGraph(wrong_literal_type, wrong_literal_type.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_LITERAL_OUTPUT_CONNECTION_TYPE", 0,
         "connection to a differently typed literal output was accepted");
  }

  MtlxElem empty_output_root = root;
  MtlxElem empty_output = elem("output", "value");
  empty_output.SetAttr("type", "float");
  empty_output_root.children[0].children[1] = empty_output;
  g_checks++;
  if (!ResolveMtlxGraph(empty_output_root, empty_output_root.children[0],
                        library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_EMPTY_OUTPUT", 0, error);
  }
  g_checks++;
  if (ResolveMtlxGraph(empty_output_root, empty_output_root.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_EMPTY_OUTPUT_CONNECTION", 0,
         "connection to an unconnected graph output was accepted");
  }

  MtlxElem connected_bad_literal = root;
  connected_bad_literal.children[0].children[1].SetAttr("value", "not-a-float");
  g_checks++;
  if (ResolveMtlxGraph(connected_bad_literal, connected_bad_literal.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CONNECTED_OUTPUT_BAD_VALUE", 0,
         "connected graph output with invalid literal was accepted");
  }
  g_checks++;
  if (ResolveMtlxGraph(connected_bad_literal, connected_bad_literal.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CONNECTED_OUTPUT_BAD_VALUE_REFERENCE", 0,
         "connection to graph output with invalid literal was accepted");
  }
  MtlxElem connected_with_value = root;
  connected_with_value.children[0].children[1].SetAttr("value", "0.5");
  g_checks++;
  if (!ResolveMtlxGraph(connected_with_value, connected_with_value.children[0],
                        library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CONNECTED_OUTPUT_VALUE", 0, error);
  }
  MtlxElem connected_bad_interface = root;
  connected_bad_interface.children[0].children[1].SetAttr("interfacename", "gain");
  g_checks++;
  if (ResolveMtlxGraph(connected_bad_interface, connected_bad_interface.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CONNECTED_OUTPUT_INTERFACE", 0,
         "graph output interface binding was accepted");
  }
  g_checks++;
  if (ResolveMtlxGraph(connected_bad_interface, connected_bad_interface.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_CONNECTED_OUTPUT_INTERFACE_REFERENCE", 0,
         "connection to graph output with interface binding was accepted");
  }

  MtlxElem node_and_value = root;
  node_and_value.children[1].children[2].children[0] = elem("input", "in1");
  node_and_value.children[1].children[2].children[0].SetAttr("type", "float");
  node_and_value.children[1].children[2].children[0].SetAttr("nodename", "seed");
  node_and_value.children[1].children[2].children[0].SetAttr("value", "0.5");
  g_checks++;
  if (ResolveMtlxGraph(node_and_value, node_and_value.children[1], library, {},
                       &resolved, &error)) {
    Fail("MTLX_GRAPH_INPUT_NODE_AND_VALUE", 0, "node input with two bindings was accepted");
  }
  MtlxElem graph_and_value = root;
  graph_and_value.children[1].children[2].children[0].SetAttr("value", "0.5");
  g_checks++;
  if (ResolveMtlxGraph(graph_and_value, graph_and_value.children[1], library, {},
                       &resolved, &error)) {
    Fail("MTLX_GRAPH_INPUT_GRAPH_AND_VALUE", 0, "graph input with two bindings was accepted");
  }
  MtlxElem interface_and_value = root;
  interface_and_value.children[1].children[2].children[1].SetAttr("value", "0.5");
  g_checks++;
  if (ResolveMtlxGraph(interface_and_value, interface_and_value.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INPUT_INTERFACE_AND_VALUE", 0,
         "interface input with two bindings was accepted");
  }

  MtlxElem empty_node_input = root;
  empty_node_input.children[1].children[2].children[0] = elem("input", "in1");
  empty_node_input.children[1].children[2].children[0].SetAttr("type", "float");
  g_checks++;
  if (ResolveMtlxGraph(empty_node_input, empty_node_input.children[1], library, {},
                       &resolved, &error)) {
    Fail("MTLX_GRAPH_INPUT_NO_BINDING", 0, "authored input without a binding was accepted");
  }

  MtlxElem document_output_root = root;
  document_output_root.children.push_back(constant_node("global_source"));
  MtlxElem document_output = elem("output", "global_output");
  document_output.SetAttr("type", "float");
  document_output.SetAttr("nodename", "global_source");
  document_output_root.children.push_back(document_output);
  MtlxElem document_output_input = elem("input", "in1");
  document_output_input.SetAttr("type", "float");
  document_output_input.SetAttr("output", "global_output");
  document_output_root.children[1].children[2].children[0] = document_output_input;
  g_checks++;
  if (!ResolveMtlxGraph(document_output_root, document_output_root.children[1],
                        library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_DOCUMENT_OUTPUT", 0, error);
  }
  MtlxElem missing_document_output = document_output_root;
  missing_document_output.children[1].children[2].children[0].SetAttr("output", "missing");
  g_checks++;
  if (ResolveMtlxGraph(missing_document_output, missing_document_output.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_DOCUMENT_OUTPUT_MISSING", 0, "missing document output was accepted");
  }
  MtlxElem bad_document_output_type = document_output_root;
  bad_document_output_type.children.back().SetAttr("type", "integer");
  g_checks++;
  if (ResolveMtlxGraph(bad_document_output_type, bad_document_output_type.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_DOCUMENT_OUTPUT_TYPE", 0, "wrong document output type was accepted");
  }
  MtlxElem bad_document_output_node = document_output_root;
  bad_document_output_node.children.back().SetAttr("nodename", "missing");
  g_checks++;
  if (ResolveMtlxGraph(bad_document_output_node, bad_document_output_node.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_DOCUMENT_OUTPUT_NODE", 0, "missing document output source was accepted");
  }

  MtlxElem bad_document_output_value = document_output_root;
  bad_document_output_value.children.back().SetAttr("value", "not-a-float");
  g_checks++;
  if (ResolveMtlxGraph(bad_document_output_value, bad_document_output_value.children[1],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_DOCUMENT_OUTPUT_VALUE", 0, "invalid document output value was accepted");
  }

  MtlxElem indirect_dependency = document_output_root;
  MtlxElem global_add = add_node("global_source");
  MtlxElem global_graph_input = elem("input", "in1");
  global_graph_input.SetAttr("type", "float");
  global_graph_input.SetAttr("nodegraph", "helper");
  global_graph_input.SetAttr("output", "value");
  global_add.children.push_back(global_graph_input);
  MtlxElem global_value = elem("input", "in2");
  global_value.SetAttr("type", "float");
  global_value.SetAttr("value", "1");
  global_add.children.push_back(global_value);
  indirect_dependency.children[2] = global_add;
  std::swap(indirect_dependency.children[0], indirect_dependency.children[1]);
  g_checks++;
  if (!ResolveMtlxGraphs(indirect_dependency, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_DOCUMENT_OUTPUT_DEPENDENCY", 0,
         error.empty() ? "indirect document-output dependency was not ordered" : error);
  }
  MtlxElem multi_hop_dependency = indirect_dependency;
  MtlxElem bridge = add_node("bridge");
  MtlxElem bridge_input = elem("input", "in1");
  bridge_input.SetAttr("type", "float");
  bridge_input.SetAttr("nodename", "global_source");
  bridge.children.push_back(bridge_input);
  bridge.children.push_back(global_value);
  multi_hop_dependency.children.push_back(bridge);
  multi_hop_dependency.children[3].SetAttr("nodename", "bridge");
  g_checks++;
  if (!ResolveMtlxGraphs(multi_hop_dependency, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_DOCUMENT_OUTPUT_MULTI_HOP", 0,
         error.empty() ? "multi-hop document dependency was not ordered" : error);
  }
  MtlxElem shared_root_dependency = multi_hop_dependency;
  MtlxElem bridge_second = bridge_input;
  bridge_second.SetAttr("name", "in2");
  shared_root_dependency.children.back().children[1] = bridge_second;
  g_checks++;
  if (!ResolveMtlxGraphs(shared_root_dependency, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_DOCUMENT_ROOT_SHARED_DEPENDENCY", 0,
         error.empty() ? "shared root dependency was treated as a cycle" : error);
  }
  MtlxElem root_node_cycle = multi_hop_dependency;
  MtlxElem root_cycle_input = elem("input", "in1");
  root_cycle_input.SetAttr("type", "float");
  root_cycle_input.SetAttr("nodename", "bridge");
  root_node_cycle.children[2].children[0] = root_cycle_input;
  g_checks++;
  if (ResolveMtlxGraphs(root_node_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_DOCUMENT_ROOT_NODE_CYCLE", 0,
         "cycle between document nodes was accepted");
  }
  MtlxElem indirect_cycle = indirect_dependency;
  indirect_cycle.children[2].children[0].SetAttr("nodegraph", "main");
  indirect_cycle.children[2].children[0].SetAttr("output", "out");
  g_checks++;
  if (ResolveMtlxGraphs(indirect_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_DOCUMENT_OUTPUT_CYCLE", 0,
         "cycle through document output and root node was accepted");
  }

  MtlxElem compound_main = elem("nodegraph", "main");
  compound_main.children = root.children[1].children;
  compound_main.children[0].SetAttr("nodegraph", "helper");
  compound_main.children[0].SetAttr("output", "value");
  MtlxElem local_value = elem("input", "in1");
  local_value.SetAttr("type", "float");
  local_value.SetAttr("value", "1");
  compound_main.children[2].children[0] = local_value;
  MtlxElem compound_root = elem("materialx", "");
  compound_root.children = {compound_main, root.children[0]};
  g_checks++;
  if (!ResolveMtlxGraphs(compound_root, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_INTERFACE_DEPENDENCY", 0,
         error.empty() ? "compound graph input dependency was not ordered" : error);
  }
  MtlxElem missing_compound_source = compound_root;
  missing_compound_source.children[0].children[0].SetAttr("nodegraph", "missing");
  g_checks++;
  if (ResolveMtlxGraphs(missing_compound_source, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_INTERFACE_MISSING_SOURCE", 0,
         "compound graph input with missing source was accepted");
  }
  MtlxElem bad_compound_port = compound_root;
  bad_compound_port.children[0].children[0].SetAttr("output", "missing");
  g_checks++;
  if (ResolveMtlxGraph(bad_compound_port, bad_compound_port.children[0], library,
                       {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_MISSING_OUTPUT", 0,
         "compound graph input with missing output was accepted");
  }
  MtlxElem bad_compound_type = compound_root;
  bad_compound_type.children[0].children[0].SetAttr("type", "integer");
  MtlxElem independent_in2 = elem("input", "in2");
  independent_in2.SetAttr("type", "float");
  independent_in2.SetAttr("value", "1");
  bad_compound_type.children[0].children[2].children[1] = independent_in2;
  g_checks++;
  if (ResolveMtlxGraph(bad_compound_type, bad_compound_type.children[0], library,
                       {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_SOURCE_TYPE", 0,
         "compound graph input with incompatible source type was accepted");
  }
  MtlxElem bad_compound_value = compound_root;
  bad_compound_value.children[1].children[1].SetAttr("value", "not-a-float");
  g_checks++;
  if (ResolveMtlxGraph(bad_compound_value, bad_compound_value.children[0], library,
                       {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_SOURCE_VALUE", 0,
         "compound graph input with invalid source output value was accepted");
  }

  MtlxElem compound_input_with_value = compound_root;
  compound_input_with_value.children[0].children[0].SetAttr("value", "0.5");
  g_checks++;
  if (!ResolveMtlxGraph(compound_input_with_value, compound_input_with_value.children[0],
                        library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_CONNECTED_VALUE", 0, error);
  }
  compound_input_with_value.children[0].children[0].SetAttr("value", "not-a-float");
  g_checks++;
  if (ResolveMtlxGraph(compound_input_with_value, compound_input_with_value.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_CONNECTED_BAD_VALUE", 0,
         "compound graph input with invalid authored value was accepted");
  }

  MtlxElem root_connected_compound = compound_root;
  MtlxElem root_connected_input = elem("input", "gain");
  root_connected_input.SetAttr("type", "float");
  root_connected_input.SetAttr("nodename", "global_source");
  root_connected_compound.children[0].children[0] = root_connected_input;
  root_connected_compound.children.push_back(global_add);
  g_checks++;
  if (!ResolveMtlxGraphs(root_connected_compound, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_INTERFACE_ROOT_DEPENDENCY", 0,
         error.empty() ? "root-node graph input dependency was not ordered" : error);
  }
  MtlxElem root_connected_cycle = root_connected_compound;
  root_connected_cycle.children.back().children[0].SetAttr("nodegraph", "main");
  root_connected_cycle.children.back().children[0].SetAttr("output", "out");
  g_checks++;
  if (ResolveMtlxGraphs(root_connected_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_INTERFACE_ROOT_CYCLE", 0,
         "cycle through graph input and root node was accepted");
  }
  MtlxElem missing_graph_input_node = root_connected_compound;
  missing_graph_input_node.children[0].children[0].SetAttr("nodename", "missing");
  g_checks++;
  if (ResolveMtlxGraph(missing_graph_input_node, missing_graph_input_node.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_ROOT_MISSING", 0, "missing graph input node was accepted");
  }
  MtlxElem wrong_graph_input_node_type = root_connected_compound;
  wrong_graph_input_node_type.children[0].children[0].SetAttr("type", "integer");
  wrong_graph_input_node_type.children[0].children[2].children[1] = independent_in2;
  g_checks++;
  if (ResolveMtlxGraph(wrong_graph_input_node_type, wrong_graph_input_node_type.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_ROOT_TYPE", 0, "incompatible graph input node was accepted");
  }

  MtlxElem output_connected_compound = root_connected_compound;
  output_connected_compound.children.push_back(document_output);
  MtlxElem output_connected_input = elem("input", "gain");
  output_connected_input.SetAttr("type", "float");
  output_connected_input.SetAttr("output", "global_output");
  output_connected_compound.children[0].children[0] = output_connected_input;
  g_checks++;
  if (!ResolveMtlxGraphs(output_connected_compound, library, &graph_order, &error) ||
      graph_order != std::vector<std::string>({"helper", "main"})) {
    Fail("MTLX_GRAPHS_INTERFACE_DOCUMENT_OUTPUT", 0,
         error.empty() ? "document output dependency was not ordered" : error);
  }
  MtlxElem output_connected_missing = output_connected_compound;
  output_connected_missing.children[0].children[0].SetAttr("output", "missing");
  g_checks++;
  if (ResolveMtlxGraph(output_connected_missing, output_connected_missing.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_DOCUMENT_OUTPUT_MISSING", 0,
         "missing document output was accepted");
  }
  MtlxElem output_connected_wrong_type = output_connected_compound;
  output_connected_wrong_type.children[0].children[0].SetAttr("type", "integer");
  output_connected_wrong_type.children[0].children[2].children[1] = independent_in2;
  g_checks++;
  if (ResolveMtlxGraph(output_connected_wrong_type, output_connected_wrong_type.children[0],
                       library, {}, &resolved, &error)) {
    Fail("MTLX_GRAPH_INTERFACE_DOCUMENT_OUTPUT_TYPE", 0,
         "incompatible document output type was accepted");
  }
  MtlxElem output_connected_cycle = output_connected_compound;
  output_connected_cycle.children[2].children[0].SetAttr("nodegraph", "main");
  output_connected_cycle.children[2].children[0].SetAttr("output", "out");
  g_checks++;
  if (ResolveMtlxGraphs(output_connected_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_INTERFACE_DOCUMENT_OUTPUT_CYCLE", 0,
         "cycle through graph input and document output was accepted");
  }

  MtlxElem compound_cycle = compound_root;
  MtlxElem helper_input = elem("input", "cycle");
  helper_input.SetAttr("type", "float");
  helper_input.SetAttr("nodegraph", "main");
  helper_input.SetAttr("output", "out");
  compound_cycle.children[1].children.push_back(helper_input);
  g_checks++;
  if (ResolveMtlxGraphs(compound_cycle, library, &graph_order, &error)) {
    Fail("MTLX_GRAPHS_INTERFACE_CYCLE", 0,
         "cycle through compound graph inputs was accepted");
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

  const std::string flatten_xml = R"(<materialx version="1.39" colorspace="lin_rec709" fileprefix="textures/">
  <nodedef name="ND_test_constant" node="testconstant" type="float"><output name="out" type="float"/></nodedef>
  <nodedef name="ND_test_add" node="testadd" type="float"><input name="in1" type="float"/><output name="out" type="float"/></nodedef>
  <nodedef name="ND_test_graph" node="testgraph" type="float"><input name="gain" type="float" value="0.75"/><output name="out" type="float"/></nodedef>
  <nodegraph name="impl" nodedef="ND_test_graph">
    <testadd name="inner" type="float" nodedef="ND_test_add"><input name="in1" type="float" interfacename="gain"/></testadd>
    <output name="out" type="float" nodename="inner"/>
  </nodegraph>
  <nodegraph name="caller">
    <testconstant name="seed" type="float" nodedef="ND_test_constant"/>
    <testgraph name="call" type="float" nodedef="ND_test_graph"><input name="gain" type="float" nodename="seed"/></testgraph>
    <output name="out" type="float" nodename="call"/>
  </nodegraph>
</materialx>)";
  MtlxDocument flatten_document;
  g_checks++;
  if (!flatten_document.ParseFromXML(flatten_xml)) {
    Fail("MTLX_DOCUMENT_FLATTEN_PARSE", 0, flatten_document.GetError());
  } else {
    const std::string before = flatten_document.GetResolvedXML();
    g_checks++;
    if (flatten_document.FlattenNodeGraph("caller", 0, &error) ||
        flatten_document.GetResolvedXML() != before ||
        error.find("limit") == std::string::npos) {
      Fail("MTLX_DOCUMENT_FLATTEN_ATOMIC", 0,
           "failed flattening modified the document or missed its limit");
    }
    g_checks++;
    if (!flatten_document.FlattenNodeGraph("caller", 1, &error)) {
      Fail("MTLX_DOCUMENT_FLATTEN", 0, error);
    } else {
      const MtlxNodeGraphPtr flat_graph = flatten_document.FindNodeGraph("caller");
      const MtlxNodePtr flat_inner = flat_graph && flat_graph->GetNodes().size() == 2
                                         ? flat_graph->GetNodes()[1] : nullptr;
      const MtlxInputPtr flat_input = flat_inner ? flat_inner->GetInput("in1") : nullptr;
      if (!flat_graph || flat_graph->GetNode("call") || !flat_input ||
          flat_input->GetNodeName() != "seed" ||
          flat_graph->GetOutputs().size() != 1 ||
          flat_graph->GetOutputs()[0]->GetNodeName() != flat_inner->GetName() ||
          flat_input->GetColorSpace() != "lin_rec709" ||
          flat_input->GetFilePrefix() != "textures/") {
        Fail("MTLX_DOCUMENT_FLATTEN", 1,
             "rebuilt graph did not retain connections or inherited scope");
      }
    }
  }
  std::string geomprop_xml = flatten_xml;
  const std::string default_input = "<input name=\"gain\" type=\"float\" value=\"0.75\"/>";
  const size_t default_pos = geomprop_xml.find(default_input);
  const std::string authored_input = "<input name=\"gain\" type=\"float\" nodename=\"seed\"/>";
  if (default_pos != std::string::npos)
    geomprop_xml.replace(default_pos, default_input.size(),
        "<input name=\"gain\" type=\"float\" value=\"0.75\" defaultgeomprop=\"Pworld\"/>");
  const size_t authored_pos = geomprop_xml.find(authored_input);
  if (authored_pos != std::string::npos)
    geomprop_xml.erase(authored_pos, authored_input.size());
  MtlxDocument geomprop_document;
  g_checks++;
  if (default_pos == std::string::npos || authored_pos == std::string::npos ||
      !geomprop_document.ParseFromXML(geomprop_xml)) {
    Fail("MTLX_DOCUMENT_FLATTEN_GEOMPROP_SETUP", 0, geomprop_document.GetError());
  } else {
    const std::string before = geomprop_document.GetResolvedXML();
    const MtlxNodeGraphPtr graph_with_call = geomprop_document.FindNodeGraph("caller");
    const MtlxNodePtr geometry_call = graph_with_call ? graph_with_call->GetNode("call") : nullptr;
    g_checks++;
    if (!geometry_call || geometry_call->GetInput("gain") ||
        geomprop_document.FlattenNodeGraph("caller", 1, &error) ||
        error.find("geometry") == std::string::npos ||
        geomprop_document.GetResolvedXML() != before) {
      Fail("MTLX_DOCUMENT_FLATTEN_GEOMPROP", 0,
           "geometry-property default was lost or flattening changed the document");
    }
  }
  std::string scoped_flatten_xml = flatten_xml;
  const std::string impl_tag = "name=\"impl\" nodedef=\"ND_test_graph\"";
  const size_t impl_pos = scoped_flatten_xml.find(impl_tag);
  if (impl_pos != std::string::npos)
    scoped_flatten_xml.replace(impl_pos, impl_tag.size(), impl_tag + " fileprefix=\"other/\"");
  MtlxDocument scoped_flatten_document;
  g_checks++;
  if (impl_pos == std::string::npos || !scoped_flatten_document.ParseFromXML(scoped_flatten_xml)) {
    Fail("MTLX_DOCUMENT_FLATTEN_SCOPE_SETUP", 0, scoped_flatten_document.GetError());
  } else {
    const std::string before = scoped_flatten_document.GetResolvedXML();
    g_checks++;
    if (scoped_flatten_document.FlattenNodeGraph("caller", 1, &error) ||
        error.find("scope") == std::string::npos ||
        scoped_flatten_document.GetResolvedXML() != before) {
      Fail("MTLX_DOCUMENT_FLATTEN_SCOPE", 0,
           "differing graph scopes were accepted or modified the document");
    }
  }
  std::string token_flatten_xml = flatten_xml;
  const std::string body_node_start = "<testadd name=\"inner\"";
  const size_t token_pos = token_flatten_xml.find(body_node_start);
  if (token_pos != std::string::npos)
    token_flatten_xml.insert(token_pos, "<token name=\"asset\" type=\"string\" value=\"example\"/>");
  MtlxDocument token_flatten_document;
  g_checks++;
  if (token_pos == std::string::npos || !token_flatten_document.ParseFromXML(token_flatten_xml)) {
    Fail("MTLX_DOCUMENT_FLATTEN_TOKEN_SETUP", 0, token_flatten_document.GetError());
  } else {
    const std::string before = token_flatten_document.GetResolvedXML();
    g_checks++;
    if (token_flatten_document.FlattenNodeGraph("caller", 1, &error) ||
        error.find("token") == std::string::npos ||
        token_flatten_document.GetResolvedXML() != before) {
      Fail("MTLX_DOCUMENT_FLATTEN_TOKEN", 0,
           "token scope was ignored or flattening modified the document");
    }
  }
  const std::string standard_geomprop_xml = R"(<materialx version="1.39">
  <nodedef name="ND_test_uv" node="testuv" type="float">
    <input name="uv" type="vector2"/><output name="out" type="float"/>
  </nodedef>
  <nodegraph name="image_impl" nodedef="ND_image_float">
    <testuv name="body" type="float" nodedef="ND_test_uv">
      <input name="uv" type="vector2" interfacename="texcoord"/>
    </testuv>
    <output name="out" type="float" nodename="body"/>
  </nodegraph>
  <nodegraph name="image_caller">
    <image name="call" type="float" nodedef="ND_image_float"/>
    <output name="out" type="float" nodename="call"/>
  </nodegraph>
</materialx>)";
  MtlxDocument standard_geomprop_document;
  g_checks++;
  if (!standard_geomprop_document.ParseFromXML(standard_geomprop_xml)) {
    Fail("MTLX_DOCUMENT_STDLIB_GEOMPROP_SETUP", 0, standard_geomprop_document.GetError());
  } else {
    g_checks++;
    if (!standard_geomprop_document.FlattenNodeGraph("image_caller", 1, &error)) {
      Fail("MTLX_DOCUMENT_STDLIB_GEOMPROP", 0,
           error.empty() ? "standard UV default was not resolved" : error);
    } else {
      const MtlxNodeGraphPtr flat = standard_geomprop_document.FindNodeGraph("image_caller");
      const MtlxNodePtr geom = flat && flat->GetNodes().size() == 2 ? flat->GetNodes()[1] : nullptr;
      const MtlxNodePtr body = flat && flat->GetNodes().size() == 2 ? flat->GetNodes()[0] : nullptr;
      const MtlxInputPtr uv = body ? body->GetInput("uv") : nullptr;
      const MtlxInputPtr index = geom ? geom->GetInput("index") : nullptr;
      if (!geom || geom->GetCategory() != "texcoord" || !uv ||
          uv->GetNodeName() != geom->GetName() || !index ||
          index->GetValue().type != MtlxValue::TYPE_INT || index->GetValue().int_val != 0 ||
          standard_geomprop_document.GetResolvedXML().find("<geompropdef") != std::string::npos) {
        Fail("MTLX_DOCUMENT_STDLIB_GEOMPROP", 1,
             "standard UV node was incorrect or library definitions were written into the document");
      }
    }
  }
  std::string resolved_geomprop_xml = standard_geomprop_xml;
  const std::string image_impl_tag = "<nodegraph name=\"image_impl\"";
  const size_t image_impl_pos = resolved_geomprop_xml.find(image_impl_tag);
  if (image_impl_pos != std::string::npos)
    resolved_geomprop_xml.insert(image_impl_pos,
        "<geompropdef name=\"UV0\" type=\"vector2\" geomprop=\"texcoord\" index=\"3\"/>");
  MtlxDocument resolved_geomprop_document;
  g_checks++;
  if (image_impl_pos == std::string::npos ||
      !resolved_geomprop_document.ParseFromXML(resolved_geomprop_xml) ||
      !resolved_geomprop_document.FlattenNodeGraph("image_caller", 1, &error)) {
    Fail("MTLX_DOCUMENT_STDLIB_GEOMPROP_NODE", 0,
         error.empty() ? resolved_geomprop_document.GetError() : error);
  } else {
    const MtlxNodeGraphPtr flat_graph = resolved_geomprop_document.FindNodeGraph("image_caller");
    const MtlxNodePtr flat_body = flat_graph && flat_graph->GetNodes().size() == 2
                                      ? flat_graph->GetNodes()[0] : nullptr;
    const MtlxNodePtr geom_node = flat_graph && flat_graph->GetNodes().size() == 2
                                      ? flat_graph->GetNodes()[1] : nullptr;
    const MtlxInputPtr uv = flat_body ? flat_body->GetInput("uv") : nullptr;
    const MtlxInputPtr index = geom_node ? geom_node->GetInput("index") : nullptr;
    if (!uv || !geom_node || geom_node->GetCategory() != "texcoord" ||
        uv->GetNodeName() != geom_node->GetName() || !index ||
        index->GetValue().type != MtlxValue::TYPE_INT || index->GetValue().int_val != 3) {
      Fail("MTLX_DOCUMENT_STDLIB_GEOMPROP_NODE", 1,
           "the document UV0 override did not supply texcoord index three");
    }
  }
  std::string explicit_uv_xml = standard_geomprop_xml;
  const std::string image_call = "<image name=\"call\" type=\"float\" nodedef=\"ND_image_float\"/>";
  const size_t image_call_pos = explicit_uv_xml.find(image_call);
  if (image_call_pos != std::string::npos)
    explicit_uv_xml.replace(image_call_pos, image_call.size(),
        "<image name=\"call\" type=\"float\" nodedef=\"ND_image_float\">"
        "<input name=\"texcoord\" type=\"vector2\" value=\"0,0\"/></image>");
  MtlxDocument explicit_uv_document;
  g_checks++;
  if (image_call_pos == std::string::npos ||
      !explicit_uv_document.ParseFromXML(explicit_uv_xml) ||
      !explicit_uv_document.FlattenNodeGraph("image_caller", 1, &error)) {
    Fail("MTLX_DOCUMENT_STDLIB_AUTHORED_UV", 0,
         error.empty() ? explicit_uv_document.GetError() : error);
  } else {
    const MtlxNodeGraphPtr flat_graph = explicit_uv_document.FindNodeGraph("image_caller");
    const MtlxNodePtr flat_body = flat_graph && flat_graph->GetNodes().size() == 1
                                      ? flat_graph->GetNodes()[0] : nullptr;
    const MtlxInputPtr flat_uv = flat_body ? flat_body->GetInput("uv") : nullptr;
    if (!flat_uv || flat_uv->GetValue().type != MtlxValue::TYPE_FLOAT_VECTOR ||
        flat_uv->GetValue().float_vec.size() != 2) {
      Fail("MTLX_DOCUMENT_STDLIB_AUTHORED_UV", 1, "authored UV was not preserved");
    }
  }
  const std::string standard_position_xml = R"(<materialx version="1.39">
  <nodedef name="ND_test_position_sink" node="position_sink" type="float">
    <input name="position" type="vector3"/><output name="out" type="float"/>
  </nodedef>
  <nodedef name="ND_test_position_graph" node="position_graph" type="float">
    <input name="position" type="vector3" defaultgeomprop="Pworld"/>
    <output name="out" type="float"/>
  </nodedef>
  <nodegraph name="position_impl" nodedef="ND_test_position_graph">
    <position_sink name="body" type="float" nodedef="ND_test_position_sink">
      <input name="position" type="vector3" interfacename="position"/>
    </position_sink>
    <output name="out" type="float" nodename="body"/>
  </nodegraph>
  <nodegraph name="position_caller">
    <position_graph name="call" type="float" nodedef="ND_test_position_graph"/>
    <output name="out" type="float" nodename="call"/>
  </nodegraph>
</materialx>)";
  MtlxDocument standard_position_document;
  g_checks++;
  if (!standard_position_document.ParseFromXML(standard_position_xml) ||
      !standard_position_document.FlattenNodeGraph("position_caller", 1, &error)) {
    Fail("MTLX_DOCUMENT_STDLIB_WORLD_POSITION", 0,
         error.empty() ? standard_position_document.GetError() : error);
  } else {
    const MtlxNodeGraphPtr flat = standard_position_document.FindNodeGraph("position_caller");
    const MtlxNodePtr geometry = flat && flat->GetNodes().size() == 2 ? flat->GetNodes()[1] : nullptr;
    const MtlxNodePtr body = flat && flat->GetNodes().size() == 2 ? flat->GetNodes()[0] : nullptr;
    const MtlxInputPtr input = body ? body->GetInput("position") : nullptr;
    const MtlxInputPtr space = geometry ? geometry->GetInput("space") : nullptr;
    if (!geometry || geometry->GetCategory() != "position" || !input ||
        input->GetNodeName() != geometry->GetName() || !space ||
        space->GetValue().type != MtlxValue::TYPE_STRING ||
        space->GetValue().string_val != "world") {
      Fail("MTLX_DOCUMENT_STDLIB_WORLD_POSITION", 1,
           "Pworld did not create a connected world-space position node");
    }
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
  const std::string external_flatten_xml = R"(<materialx version="1.39">
  <nodegraph name="external_impl" nodedef="ND_external_passthrough">
    <externalconstant name="source" type="float" nodedef="ND_external_constant"/>
    <output name="out" type="float" nodename="source"/>
  </nodegraph>
  <nodegraph name="external_caller">
    <externalpassthrough name="call" type="float" nodedef="ND_external_passthrough"/>
    <output name="out" type="float" nodename="call"/>
  </nodegraph>
</materialx>)";
  MtlxDocument external_flatten_document;
  g_checks++;
  if (!external_flatten_document.ParseFromXML(external_flatten_xml) ||
      !external_flatten_document.FlattenNodeGraph("external_caller", external_library,
                                                  1, &error)) {
    Fail("MTLX_DOCUMENT_FLATTEN_EXTERNAL_LIBRARY", 0,
         error.empty() ? external_flatten_document.GetError() : error);
  } else {
    const MtlxNodeGraphPtr flat_graph = external_flatten_document.FindNodeGraph("external_caller");
    if (!flat_graph || flat_graph->GetNode("call") || flat_graph->GetNodes().size() != 1 ||
        flat_graph->GetNodes()[0]->GetCategory() != "externalconstant") {
      Fail("MTLX_DOCUMENT_FLATTEN_EXTERNAL_LIBRARY", 1,
           "external-library graph did not flatten");
    }
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
