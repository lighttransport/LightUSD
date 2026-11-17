// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// mtlx_oracle_dump FILE.mtlx
//
// Transitional differential oracle for the C11 MaterialX rewrite (removed once
// the C++ DOM/resolver is gone). Prints, in a stable line format, everything a
// consumer of the C++ MaterialX API observes for one document:
//   - parse result, error and warning of both MtlxDocument::ParseFromXML
//     overloads (in-memory, and anchored at FILE for XIncludes);
//   - GetResolvedXML();
//   - the query walk: document attributes, nodes, nodegraphs (nodes, inputs,
//     outputs), materials, looks, collections, with every getter consumers use
//     and the typed value of each input;
//   - ResolveNodeGraphs() order/error;
//   - FlattenNodeGraph() of every nodegraph (resolved XML after each);
//   - ConvertToUnit() for every unittype/unit declared in the document;
//   - GetMaterialAssignmentsForGeometry() for every look and authored geom;
//   - the canonical DumpMtlxDocument() of the parsed, expanded element tree
//     (with and without the version upgrade).
// tests/core/run_mtlx_diff.py compares this output with the C implementation.

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/mtlx-dom.hh"
#include "core/mtlx-upgrade.hh"

using namespace lightusd::mtlx;

namespace {

bool ReadFile(const std::string &path, std::string *out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

std::string DirOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Escape so that every record stays on one line.
std::string Q(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

std::string F(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", double(v));
  return buf;
}

std::string Value(const MtlxValue &v) {
  std::string out;
  switch (v.type) {
    case MtlxValue::TYPE_NONE: return "none";
    case MtlxValue::TYPE_BOOL: return std::string("bool ") + (v.bool_val ? "1" : "0");
    case MtlxValue::TYPE_INT: return "int " + std::to_string(v.int_val);
    case MtlxValue::TYPE_FLOAT: return "float " + F(v.float_val);
    case MtlxValue::TYPE_STRING: return "string " + Q(v.string_val);
    case MtlxValue::TYPE_FLOAT_VECTOR:
      out = "floats";
      for (float f : v.float_vec) out += " " + F(f);
      return out;
    case MtlxValue::TYPE_INT_VECTOR:
      out = "ints";
      for (int i : v.int_vec) out += " " + std::to_string(i);
      return out;
    case MtlxValue::TYPE_STRING_VECTOR:
      out = "strings";
      for (const auto &s : v.string_vec) out += " " + Q(s);
      return out;
  }
  return "?";
}

void Element(const char *indent, const MtlxElement &e) {
  std::printf("%sname=%s type=%s nodedef=%s colorspace=%s fileprefix=%s namespace=%s value=%s\n",
              indent, Q(e.GetName()).c_str(), Q(e.GetType()).c_str(),
              Q(e.GetNodeDef()).c_str(), Q(e.GetColorSpace()).c_str(),
              Q(e.GetFilePrefix()).c_str(), Q(e.GetNamespace()).c_str(),
              Value(e.GetValue()).c_str());
}

void Input(const char *indent, const MtlxInput &in) {
  Element(indent, in);
  std::printf("%s  nodename=%s nodegraph=%s output=%s interfacename=%s channels=%s unit=%s unittype=%s\n",
              indent, Q(in.GetNodeName()).c_str(), Q(in.GetNodeGraph()).c_str(),
              Q(in.GetOutput()).c_str(), Q(in.GetInterfaceName()).c_str(),
              Q(in.GetChannels()).c_str(), Q(in.GetUnit()).c_str(),
              Q(in.GetUnitType()).c_str());
}

void Node(const char *indent, const MtlxNode &n) {
  std::printf("%snode category=%s\n", indent, Q(n.GetCategory()).c_str());
  Element(indent, n);
  for (const auto &in : n.GetInputs()) {
    std::printf("%s  input\n", indent);
    Input((std::string(indent) + "    ").c_str(), *in);
  }
}

void Walk(const MtlxDocument &doc) {
  std::printf("doc version=%s colorspace=%s namespace=%s\n", Q(doc.GetVersion()).c_str(),
              Q(doc.GetColorSpace()).c_str(), Q(doc.GetNamespace()).c_str());
  for (const auto &n : doc.GetNodes()) Node("", *n);
  for (const auto &g : doc.GetNodeGraphs()) {
    std::printf("nodegraph\n");
    Element("  ", *g);
    for (const auto &in : g->GetInputs()) {
      std::printf("  graphinput\n");
      Input("    ", *in);
    }
    for (const auto &out : g->GetOutputs()) {
      std::printf("  graphoutput nodename=%s output=%s\n", Q(out->GetNodeName()).c_str(),
                  Q(out->GetOutput()).c_str());
      Element("    ", *out);
    }
    for (const auto &n : g->GetNodes()) Node("  ", *n);
  }
  for (const auto &m : doc.GetMaterials()) {
    std::printf("material surface=%s surfacegraph=%s surfaceoutput=%s displacement=%s volume=%s volumegraph=%s volumeoutput=%s\n",
                Q(m->GetSurfaceShader()).c_str(), Q(m->GetSurfaceNodeGraph()).c_str(),
                Q(m->GetSurfaceOutput()).c_str(), Q(m->GetDisplacementShader()).c_str(),
                Q(m->GetVolumeShader()).c_str(), Q(m->GetVolumeNodeGraph()).c_str(),
                Q(m->GetVolumeOutput()).c_str());
    Element("  ", *m);
  }
  for (const auto &l : doc.GetLooks()) {
    std::printf("look\n");
    Element("  ", *l);
    for (const auto &a : l->GetMaterialAssignments())
      std::printf("  assign name=%s material=%s geom=%s collection=%s\n", Q(a->GetName()).c_str(),
                  Q(a->GetMaterial()).c_str(), Q(a->GetGeom()).c_str(),
                  Q(a->GetCollection()).c_str());
  }
  for (const auto &c : doc.GetCollections()) {
    std::printf("collection includegeom=%s excludegeom=%s includecollections=", Q(c->GetIncludeGeom()).c_str(),
                Q(c->GetExcludeGeom()).c_str());
    for (const auto &s : c->GetIncludedCollections()) std::printf("%s,", Q(s).c_str());
    std::printf("\n");
    Element("  ", *c);
  }
}

void Lookups(const MtlxDocument &doc) {
  std::set<std::string> names;
  for (const auto &n : doc.GetNodes()) names.insert(n->GetName());
  for (const auto &g : doc.GetNodeGraphs()) names.insert(g->GetName());
  for (const auto &m : doc.GetMaterials()) names.insert(m->GetName());
  names.insert("__missing__");
  for (const std::string &n : names)
    std::printf("find %s node=%d graph=%d material=%d look=%d collection=%d\n", Q(n).c_str(),
                doc.FindNode(n) != nullptr, doc.FindNodeGraph(n) != nullptr,
                doc.FindMaterial(n) != nullptr, doc.FindLook(n) != nullptr,
                doc.FindCollection(n) != nullptr);
}

void Section(const char *name) { std::printf("== %s\n", name); }

void Resolved(const MtlxDocument &doc) {
  const std::string xml = doc.GetResolvedXML();
  std::printf("resolved %s\n", Q(xml).c_str());
}

// Unit types and units declared by <unittypedef>/<unitdef>/<unit> elements.
void CollectUnits(const MtlxElem &e, std::set<std::pair<std::string, std::string>> *out) {
  std::vector<const MtlxElem *> stack{&e};
  while (!stack.empty()) {
    const MtlxElem *x = stack.back();
    stack.pop_back();
    if (x->tag == "unitdef") {
      const std::string type = x->AttrOr("unittype");
      for (const MtlxElem &u : x->children)
        if (u.tag == "unit") out->insert({type, u.AttrOr("name")});
    }
    for (const MtlxElem &c : x->children) stack.push_back(&c);
  }
}

void Geoms(const MtlxElem &e, std::set<std::string> *out) {
  std::vector<const MtlxElem *> stack{&e};
  while (!stack.empty()) {
    const MtlxElem *x = stack.back();
    stack.pop_back();
    for (const char *key : {"geom", "includegeom", "excludegeom"})
      if (const std::string *v = x->Attr(key)) {
        std::string cur;
        for (char c : *v + ",") {
          if (c == ',') {
            while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) {
              out->insert(cur);
              out->insert(cur + "/child");
            }
            cur.clear();
          } else {
            cur += c;
          }
        }
      }
    for (const MtlxElem &c : x->children) stack.push_back(&c);
  }
  out->insert("/not/assigned");
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: mtlx_oracle_dump FILE.mtlx\n");
    return 2;
  }
  const std::string path = argv[1];
  std::string text;
  if (!ReadFile(path, &text)) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 2;
  }

  Section("parse-memory");
  {
    MtlxDocument doc;
    const bool ok = doc.ParseFromXML(text);
    std::printf("ok=%d error=%s warning=%s\n", ok, Q(doc.GetError()).c_str(),
                Q(doc.GetWarning()).c_str());
    if (ok) {
      Resolved(doc);
      Walk(doc);
    }
  }

  Section("parse-anchored");
  MtlxDocument doc;
  const bool ok = doc.ParseFromXML(text, path);
  std::printf("ok=%d error=%s warning=%s\n", ok, Q(doc.GetError()).c_str(),
              Q(doc.GetWarning()).c_str());
  if (ok) {
    Resolved(doc);
    Walk(doc);
    Lookups(doc);

    Section("resolve");
    std::vector<std::string> order;
    std::string err;
    const bool rok = doc.ResolveNodeGraphs(&order, &err);
    std::printf("ok=%d error=%s order=", rok, Q(err).c_str());
    for (const auto &g : order) std::printf("%s,", Q(g).c_str());
    std::printf("\n");

    Section("flatten");
    for (const auto &g : doc.GetNodeGraphs()) {
      MtlxDocument copy;
      copy.ParseFromXML(text, path);
      std::string ferr;
      const bool fok = copy.FlattenNodeGraph(g->GetName(), 256, &ferr);
      std::printf("graph %s ok=%d error=%s\n", Q(g->GetName()).c_str(), fok, Q(ferr).c_str());
      if (fok) Resolved(copy);
    }

    // Element tree of the input (expanded) for the remaining queries.
    MtlxElem root;
    std::string perr;
    const bool tree = ParseMtlxXml(text, &root, &perr) &&
                      ExpandMtlxIncludes(
                          &root,
                          [&](const std::string &href, std::string *xml) {
                            return ReadFile(DirOf(path) + "/" + href, xml);
                          },
                          &perr);

    Section("units");
    if (tree) {
      std::set<std::pair<std::string, std::string>> units;
      CollectUnits(root, &units);
      for (const auto &u : units) {
        MtlxDocument copy;
        copy.ParseFromXML(text, path);
        const bool cok = copy.ConvertToUnit(u.first, u.second);
        std::printf("convert %s %s ok=%d error=%s\n", Q(u.first).c_str(), Q(u.second).c_str(),
                    cok, Q(copy.GetError()).c_str());
        if (cok) Resolved(copy);
      }
    }

    Section("assign");
    if (tree) {
      std::set<std::string> geoms;
      Geoms(root, &geoms);
      for (const auto &l : doc.GetLooks())
        for (const std::string &g : geoms) {
          std::vector<MtlxResolvedMaterialAssignment> out;
          std::string aerr;
          const bool aok = doc.GetMaterialAssignmentsForGeometry(l->GetName(), g, &out, &aerr);
          std::printf("look %s geom %s ok=%d error=%s ->", Q(l->GetName()).c_str(), Q(g).c_str(), aok,
                      Q(aerr).c_str());
          for (const auto &a : out)
            std::printf(" %s:%s", Q(a.assignment ? a.assignment->GetName() : "").c_str(),
                        Q(a.material ? a.material->GetName() : "").c_str());
          std::printf("\n");
        }
    }

    Section("dump");
    if (tree) {
      MtlxLibrary lib;
      lib.AddFrom(root);
      const std::string dump = DumpMtlxDocument(lib, root);
      std::printf("plain %s\n", Q(dump).c_str());
      MtlxElem up = root;
      UpgradeMtlxVersion(&up);
      MtlxLibrary uplib;
      uplib.AddFrom(up);
      std::printf("upgraded %s\n", Q(DumpMtlxDocument(uplib, up)).c_str());
    } else {
      std::printf("tree-error %s\n", Q(perr).c_str());
    }
  }
  return 0;
}
