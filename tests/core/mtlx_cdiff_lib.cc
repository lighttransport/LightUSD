// SPDX-License-Identifier: Apache-2.0
// Differential harness: C++ MtlxLibrary / ResolveMtlxGraph(s) /
// FlattenMtlxGraph(Call) / DumpMtlxDocument / ApplyMtlxNodeDefDefaults vs the
// C11 port (src/c/mtlx/mtlx-{stdlib,library,resolve,flatten,dump}.c).
// Not registered in CMake. Usage:
//   mtlx_cdiff_lib [--mutations N] <file.mtlx | @listfile>...
// Also replays tests/core/test_mtlx_lean_vectors.cc with every graph
// resolve/flatten/dump call tapped (C++ result compared to the C result), and
// checks the Lean MTLX_RESOLVE / MTLX_DUMP vectors directly against the C.
// Exit status is non-zero on any mismatch.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/mtlx-dom.hh"
#include "core/mtlx-resolve.hh"
#include "core/mtlx-upgrade.hh"
#include "c/mtlx/mtlx-internal.h"
#include "pcp/layer-registry.hh"

namespace cdiff {

using lightusd::mtlx::MtlxElem;
using lightusd::mtlx::MtlxLibrary;
using lightusd::mtlx::MtlxNodeDefInfo;
using lightusd::mtlx::MtlxPort;
using lightusd::mtlx::MtlxResolvedGraph;
using lightusd::mtlx::MtlxScope;

size_t g_checks = 0, g_mismatch = 0;
std::map<std::string, size_t> g_counts;
std::string g_label;

void Mismatch(const std::string &what, const std::string &why) {
  ++g_mismatch;
  if (g_mismatch <= 60) std::cerr << "MISMATCH " << what << " [" << g_label << "]: " << why << "\n";
}
void Count(const char *k) {
  ++g_checks;
  ++g_counts[k];
}

// ---- verbatim copies of anonymous-namespace code of src/core/mtlx-dom.cc ---

struct StdNodeDefRow {
  const char *name, *node, *out_type;
  unsigned first, count;
};
struct StdInputRow {
  const char *name, *type;
  int has_default;
  const char *default_value, *geomprop;
};
struct StdGeomPropRow {
  const char *name, *type, *geomprop;
  int has_space;
  const char *space;
  int has_index;
  const char *index;
};
#define MTLX_STD_NODEDEF(name, node, output_type, first_input, input_count) \
  {name, node, output_type, first_input, input_count},
#define MTLX_STD_INPUT(name, type, has_default, default_value, geomprop)
const StdNodeDefRow kStdNodeDefRows[] = {
#include "external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
};
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
#define MTLX_STD_NODEDEF(name, node, output_type, first_input, input_count)
#define MTLX_STD_INPUT(name, type, has_default, default_value, geomprop) \
  {name, type, has_default, default_value, geomprop},
const StdInputRow kStdInputRows[] = {
#include "external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
};
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
#define MTLX_STD_GEOMPROPDEF(nm, ty, category, has_space, space, has_index, index) \
  {nm, ty, category, has_space, space, has_index, index},
const StdGeomPropRow kStdGeomPropRows[] = {
#include "core/generated/mtlx-stdlib-geomprops.inc"
};
#undef MTLX_STD_GEOMPROPDEF

std::vector<MtlxElem> BuildStandardMtlxNodeDefs() {
  std::vector<MtlxElem> result;
  for (const StdNodeDefRow &row : kStdNodeDefRows) {
    MtlxElem def;
    def.tag = "nodedef";
    def.SetAttr("name", row.name);
    def.SetAttr("node", row.node);
    def.SetAttr("type", row.out_type);
    MtlxElem output;
    output.tag = "output";
    output.SetAttr("name", "out");
    output.SetAttr("type", row.out_type);
    def.children.push_back(std::move(output));
    for (unsigned k = 0; k < row.count; ++k) {
      const StdInputRow &in = kStdInputRows[row.first + k];
      MtlxElem input;
      input.tag = "input";
      input.SetAttr("name", in.name);
      input.SetAttr("type", in.type);
      if (in.has_default) input.SetAttr("value", in.default_value);
      if (in.geomprop[0]) input.SetAttr("defaultgeomprop", in.geomprop);
      def.children.push_back(std::move(input));
    }
    result.push_back(std::move(def));
  }
  return result;
}
std::vector<MtlxElem> BuildStandardMtlxGeomPropDefs() {
  std::vector<MtlxElem> result;
  for (const StdGeomPropRow &row : kStdGeomPropRows) {
    MtlxElem def;
    def.tag = "geompropdef";
    def.SetAttr("name", row.name);
    def.SetAttr("type", row.type);
    def.SetAttr("geomprop", row.geomprop);
    if (row.has_space) def.SetAttr("space", row.space);
    if (row.has_index) def.SetAttr("index", row.index);
    result.push_back(std::move(def));
  }
  return result;
}
const std::vector<MtlxElem> &StandardMtlxNodeDefs() {
  static const std::vector<MtlxElem> defs = BuildStandardMtlxNodeDefs();
  return defs;
}
const std::vector<MtlxElem> &StandardMtlxGeomPropDefs() {
  static const std::vector<MtlxElem> defs = BuildStandardMtlxGeomPropDefs();
  return defs;
}

void ApplyMtlxNodeDefDefaults(MtlxElem *elem, const MtlxLibrary &library,
                              const MtlxScope &outer) {
  if (!elem) return;
  if (lightusd::mtlx::IsMtlxNodeElem(*elem)) {
    if (const MtlxNodeDefInfo *def = library.ResolveReference(*elem, outer)) {
      for (const MtlxPort &port : library.ActiveInputs(*def)) {
        if (!port.has_value || !port.default_geomprop.empty()) continue;
        MtlxElem *input = nullptr;
        for (MtlxElem &child : elem->children) {
          if (child.tag == "input" && child.Name() == port.name) {
            input = &child;
            break;
          }
        }
        if (!input) {
          elem->children.emplace_back();
          input = &elem->children.back();
          input->tag = "input";
          input->SetAttr("name", port.name);
          input->SetAttr("type", port.type);
          input->SetAttr("value", port.value);
          continue;
        }
        if (input->Attr("value") || input->Attr("nodename") ||
            input->Attr("nodegraph") || input->Attr("interfacename")) {
          continue;
        }
        if (input->Attr("type") == nullptr) input->SetAttr("type", port.type);
        input->SetAttr("value", port.value);
      }
    }
  }
  MtlxScope child_scope;
  child_scope.reserve(outer.size() + 1);
  child_scope.push_back(elem);
  child_scope.insert(child_scope.end(), outer.begin(), outer.end());
  for (MtlxElem &child : elem->children)
    ApplyMtlxNodeDefDefaults(&child, library, child_scope);
}

void ApplyMtlxNodeDefDefaults(MtlxElem *root) {
  if (!root) return;
  MtlxLibrary library;
  library.AddFrom(*root);
  for (const MtlxElem &def : StandardMtlxNodeDefs()) library.AddNodeDef(def, {});
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children) ApplyMtlxNodeDefDefaults(&child, library, outer);
}

void InheritMtlxAttributes(MtlxElem *elem, const MtlxScope &outer) {
  if (!elem) return;
  static const char *const kScopedAttributes[] = {"colorspace", "fileprefix", "geomprefix",
                                                  "namespace"};
  for (const char *key : kScopedAttributes) {
    if (elem->Attr(key)) continue;
    std::string value;
    if (lightusd::mtlx::ScopedMtlxAttr(key, outer, &value)) elem->SetAttr(key, value);
  }
  MtlxScope child_scope{elem};
  child_scope.insert(child_scope.end(), outer.begin(), outer.end());
  for (MtlxElem &child : elem->children) InheritMtlxAttributes(&child, child_scope);
}
void InheritMtlxAttributes(MtlxElem *root) {
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children) InheritMtlxAttributes(&child, outer);
}

// ---- conversions & comparisons -------------------------------------------

void ToC(const MtlxElem &src, lu_mtlx_elem *dst) {
  lu_mtlx_elem_init(dst);
  lu_mtlx_elem_set_tag(dst, src.tag.c_str());
  for (const auto &a : src.attrs) lu_mtlx_attr_append(dst, a.first.c_str(), a.second.c_str());
  for (const MtlxElem &c : src.children) {
    lu_mtlx_elem tmp;
    ToC(c, &tmp);
    lu_mtlx_child_push(dst, &tmp);
  }
}

bool Same(const MtlxElem &a, const lu_mtlx_elem &b, const std::string &path, std::string *why) {
  const std::string here = path + "/" + a.tag + "[" + a.Name() + "]";
  if (a.tag != b.tag) { *why = here + ": tag " + a.tag + " vs " + b.tag; return false; }
  if (a.attrs.size() != b.nattrs) {
    *why = here + ": attr count " + std::to_string(a.attrs.size()) + " vs " + std::to_string(b.nattrs);
    return false;
  }
  for (size_t i = 0; i < b.nattrs; i++) {
    if (a.attrs[i].first != b.attrs[i].key || a.attrs[i].second != b.attrs[i].value) {
      *why = here + ": attr " + std::to_string(i) + " " + a.attrs[i].first + "=" +
             a.attrs[i].second + " vs " + b.attrs[i].key + "=" + b.attrs[i].value;
      return false;
    }
  }
  if (a.children.size() != b.nchildren) {
    *why = here + ": child count " + std::to_string(a.children.size()) + " vs " +
           std::to_string(b.nchildren);
    return false;
  }
  for (size_t i = 0; i < b.nchildren; i++)
    if (!Same(a.children[i], b.children[i], here, why)) return false;
  return true;
}

std::string S(const lu_mtlx_sb &b) { return lu_mtlx_sb_str(&b); }

bool SamePort(const MtlxPort &a, const lu_mtlx_port &b) {
  return a.name == b.name && a.type == b.type && a.has_value == (b.has_value != 0) &&
         a.value == b.value && a.default_geomprop == b.default_geomprop;
}
bool SamePorts(const std::vector<MtlxPort> &a, const lu_mtlx_port *b, size_t n) {
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++)
    if (!SamePort(a[i], b[i])) return false;
  return true;
}
bool SameDef(const MtlxNodeDefInfo &a, const lu_mtlx_nodedef &b) {
  return a.name == b.name && a.node == b.node && a.key == b.key &&
         a.has_version == (b.has_version != 0) && a.version == b.version &&
         a.is_default == (b.is_default != 0) && a.target == b.target &&
         a.has_inherit == (b.has_inherit != 0) && a.inherit == b.inherit &&
         SamePorts(a.inputs, b.inputs, b.ninputs) && SamePorts(a.outputs, b.outputs, b.noutputs);
}

void CompareLibs(const MtlxLibrary &cpp, const lu_mtlx_library &c, const std::string &what) {
  Count("library");
  if (c.oom) { Mismatch(what, "oom"); return; }
  if (cpp.Defs().size() != c.ndefs) {
    Mismatch(what, "def count " + std::to_string(cpp.Defs().size()) + " vs " + std::to_string(c.ndefs));
    return;
  }
  for (size_t i = 0; i < c.ndefs; i++)
    if (!SameDef(cpp.Defs()[i], c.defs[i])) {
      Mismatch(what, "def " + std::to_string(i) + " " + cpp.Defs()[i].name);
      return;
    }
  if (cpp.GeomPropDefs().size() != c.ngeomprop) { Mismatch(what, "geomprop count"); return; }
  std::string why;
  for (size_t i = 0; i < c.ngeomprop; i++)
    if (!Same(cpp.GeomPropDefs()[i], c.geomprop_defs[i], "", &why)) { Mismatch(what, why); return; }
}

// C library mirroring a C++ library (for the tapped calls).
void LibToC(const MtlxLibrary &cpp, lu_mtlx_library *c) {
  lu_mtlx_library_init(c);
  // Build each def through a synthetic nodedef element would lose `key`;
  // copy fields directly instead.
  c->ndefs = c->capdefs = cpp.Defs().size();
  c->defs = (lu_mtlx_nodedef *)calloc(c->ndefs + 1, sizeof(lu_mtlx_nodedef));
  auto ports = [](const std::vector<MtlxPort> &v, lu_mtlx_port **out, size_t *n) {
    *n = v.size();
    *out = v.empty() ? nullptr : (lu_mtlx_port *)calloc(v.size(), sizeof(lu_mtlx_port));
    for (size_t i = 0; i < v.size(); i++) {
      (*out)[i].name = lu_mtlx_strdup(v[i].name.c_str());
      (*out)[i].type = lu_mtlx_strdup(v[i].type.c_str());
      (*out)[i].has_value = v[i].has_value;
      (*out)[i].value = lu_mtlx_strdup(v[i].value.c_str());
      (*out)[i].default_geomprop = lu_mtlx_strdup(v[i].default_geomprop.c_str());
    }
  };
  for (size_t i = 0; i < c->ndefs; i++) {
    const MtlxNodeDefInfo &a = cpp.Defs()[i];
    lu_mtlx_nodedef &b = c->defs[i];
    b.name = lu_mtlx_strdup(a.name.c_str());
    b.node = lu_mtlx_strdup(a.node.c_str());
    b.key = lu_mtlx_strdup(a.key.c_str());
    b.has_version = a.has_version;
    b.version = lu_mtlx_strdup(a.version.c_str());
    b.is_default = a.is_default;
    b.target = lu_mtlx_strdup(a.target.c_str());
    b.has_inherit = a.has_inherit;
    b.inherit = lu_mtlx_strdup(a.inherit.c_str());
    ports(a.inputs, &b.inputs, &b.ninputs);
    ports(a.outputs, &b.outputs, &b.noutputs);
  }
  c->ngeomprop = c->capgeomprop = cpp.GeomPropDefs().size();
  c->geomprop_defs = (lu_mtlx_elem *)calloc(c->ngeomprop + 1, sizeof(lu_mtlx_elem));
  for (size_t i = 0; i < c->ngeomprop; i++) ToC(cpp.GeomPropDefs()[i], &c->geomprop_defs[i]);
}

// Pointer mapping from a C++ tree to its C mirror.
const lu_mtlx_elem *MapIn(const MtlxElem *p, const MtlxElem &cpp, const lu_mtlx_elem &c) {
  if (p == &cpp) return &c;
  for (size_t i = 0; i < cpp.children.size(); i++)
    if (const lu_mtlx_elem *r = MapIn(p, cpp.children[i], c.children[i])) return r;
  return nullptr;
}

struct Mirror {
  const MtlxElem *doc;
  lu_mtlx_elem cdoc;
  std::vector<lu_mtlx_elem *> extras;
  explicit Mirror(const MtlxElem &d) : doc(&d) { ToC(d, &cdoc); }
  ~Mirror() {
    lu_mtlx_elem_free(&cdoc);
    for (auto *e : extras) { lu_mtlx_elem_free(e); delete e; }
  }
  const lu_mtlx_elem *Map(const MtlxElem *p) {
    if (const lu_mtlx_elem *r = MapIn(p, *doc, cdoc)) return r;
    lu_mtlx_elem *e = new lu_mtlx_elem;
    ToC(*p, e);
    extras.push_back(e);
    return e;
  }
  void Scope(const MtlxScope &s, lu_mtlx_scope *out) {
    lu_mtlx_scope_init(out);
    for (const MtlxElem *p : s) lu_mtlx_scope_push(out, p ? Map(p) : nullptr);
  }
};

std::string Join(const std::vector<std::string> &v) {
  std::string s;
  for (const auto &x : v) s += x + ",";
  return s;
}
std::string Join(const lu_mtlx_strv &v) {
  std::string s;
  for (size_t i = 0; i < v.n; i++) s += std::string(v.v[i]) + ",";
  return s;
}

// ---- the compared operations ----------------------------------------------

bool DiffResolveGraph(const MtlxElem &doc, const MtlxElem &graph, const MtlxLibrary &lib,
                      const lu_mtlx_library &clib, const MtlxScope &outer,
                      MtlxResolvedGraph *resolved, std::string *err) {
  MtlxResolvedGraph local;
  std::string lerr;
  const bool ok = lightusd::mtlx::ResolveMtlxGraph(doc, graph, lib, outer, resolved ? resolved : &local,
                                                   err ? err : &lerr);
  Mirror m(doc);
  lu_mtlx_scope cs;
  m.Scope(outer, &cs);
  lu_mtlx_strv order;
  lu_mtlx_strv_init(&order);
  lu_mtlx_sb cerr;
  lu_mtlx_sb_init(&cerr);
  const int cok = lu_mtlx_resolve_graph(&m.cdoc, m.Map(&graph), &clib, &cs, &order, &cerr);
  Count("resolve_graph");
  const std::string &e = err ? *err : lerr;
  const std::vector<std::string> &no = (resolved ? resolved : &local)->node_order;
  if (ok != (cok != 0) || e != S(cerr) || Join(no) != Join(order))
    Mismatch("resolve_graph " + graph.Name(), std::to_string(ok) + " '" + e + "' [" + Join(no) +
                                                  "] vs " + std::to_string(cok) + " '" + S(cerr) +
                                                  "' [" + Join(order) + "]");
  lu_mtlx_strv_free(&order);
  lu_mtlx_sb_free(&cerr);
  lu_mtlx_scope_free(&cs);
  return ok;
}

bool DiffResolveGraphs(const MtlxElem &doc, const MtlxLibrary &lib, const lu_mtlx_library &clib,
                       std::vector<std::string> *order_out, std::string *err) {
  std::vector<std::string> local;
  std::string lerr;
  std::vector<std::string> *order = order_out ? order_out : &local;
  const bool ok = lightusd::mtlx::ResolveMtlxGraphs(doc, lib, order, err ? err : &lerr);
  Mirror m(doc);
  lu_mtlx_strv corder;
  lu_mtlx_strv_init(&corder);
  lu_mtlx_sb cerr;
  lu_mtlx_sb_init(&cerr);
  const int cok = lu_mtlx_resolve_graphs(&m.cdoc, &clib, &corder, &cerr);
  Count("resolve_graphs");
  const std::string &e = err ? *err : lerr;
  if (ok != (cok != 0) || e != S(cerr) || Join(*order) != Join(corder))
    Mismatch("resolve_graphs", std::to_string(ok) + " '" + e + "' [" + Join(*order) + "] vs " +
                                   std::to_string(cok) + " '" + S(cerr) + "' [" + Join(corder) + "]");
  lu_mtlx_strv_free(&corder);
  lu_mtlx_sb_free(&cerr);
  return ok;
}

// call_name non-null: FlattenMtlxGraphCall; else FlattenMtlxGraph(max).
bool DiffFlatten(const MtlxElem &doc, const MtlxElem &graph, const MtlxLibrary &lib,
                 const lu_mtlx_library &clib, const MtlxScope &outer, const std::string *call_name,
                 size_t max, MtlxElem *flattened, std::string *err) {
  MtlxElem local;
  std::string lerr;
  MtlxElem *out = flattened ? flattened : &local;
  const bool ok =
      call_name ? lightusd::mtlx::FlattenMtlxGraphCall(doc, graph, lib, outer, *call_name, out,
                                                       err ? err : &lerr)
                : lightusd::mtlx::FlattenMtlxGraph(doc, graph, lib, outer, max, out,
                                                   err ? err : &lerr);
  Mirror m(doc);
  lu_mtlx_scope cs;
  m.Scope(outer, &cs);
  lu_mtlx_elem cflat;
  lu_mtlx_elem_init(&cflat);
  lu_mtlx_sb cerr;
  lu_mtlx_sb_init(&cerr);
  const int cok = call_name ? lu_mtlx_flatten_graph_call(&m.cdoc, m.Map(&graph), &clib, &cs,
                                                         call_name->c_str(), &cflat, &cerr)
                            : lu_mtlx_flatten_graph(&m.cdoc, m.Map(&graph), &clib, &cs, max,
                                                    &cflat, &cerr);
  Count(call_name ? "flatten_call" : "flatten_graph");
  const std::string &e = err ? *err : lerr;
  const std::string what = call_name ? "flatten_call " + graph.Name() + "/" + *call_name
                                     : "flatten_graph " + graph.Name() + " max=" + std::to_string(max);
  std::string why;
  if (ok != (cok != 0) || e != S(cerr))
    Mismatch(what, std::to_string(ok) + " '" + e + "' vs " + std::to_string(cok) + " '" + S(cerr) + "'");
  else if (ok && !Same(*out, cflat, "", &why))
    Mismatch(what, why);
  lu_mtlx_elem_free(&cflat);
  lu_mtlx_sb_free(&cerr);
  lu_mtlx_scope_free(&cs);
  return ok;
}

std::string DiffDump(const MtlxLibrary &lib, const lu_mtlx_library &clib, const MtlxElem &root) {
  const std::string want = lightusd::mtlx::DumpMtlxDocument(lib, root);
  lu_mtlx_elem c;
  ToC(root, &c);
  lu_mtlx_sb got;
  lu_mtlx_sb_init(&got);
  const int ok = lu_mtlx_dump_document(&clib, &c, &got);
  Count("dump");
  if (!ok || want != S(got)) Mismatch("dump", "--- c++ ---\n" + want + "--- c ---\n" + S(got));
  lu_mtlx_sb_free(&got);
  lu_mtlx_elem_free(&c);
  return want;
}

// ---- per-document checks ---------------------------------------------------

void BuildLibs(const MtlxElem &root, int with_std, int with_geom, MtlxLibrary *cpp,
               lu_mtlx_library *c) {
  cpp->AddFrom(root);
  lu_mtlx_library_init(c);
  lu_mtlx_elem croot;
  ToC(root, &croot);
  lu_mtlx_library_add_from(c, &croot);
  lu_mtlx_elem_free(&croot);
  if (with_std) {
    for (const MtlxElem &def : StandardMtlxNodeDefs()) cpp->AddNodeDef(def, {});
    lu_mtlx_library_add_stdlib(c);
  }
  if (with_geom) {
    MtlxElem g;
    g.tag = "materialx";
    g.children = StandardMtlxGeomPropDefs();
    MtlxLibrary gl;
    gl.AddFrom(g);
    cpp->AddLibrary(gl);
    lu_mtlx_library cg;
    lu_mtlx_library_init(&cg);
    lu_mtlx_elem ge;
    lu_mtlx_elem_init(&ge);
    lu_mtlx_elem_set_tag(&ge, "materialx");
    for (size_t i = 0; i < lu_mtlx_stdlib_geomprop_count(); i++) {
      lu_mtlx_elem e;
      lu_mtlx_stdlib_geomprop_elem(i, &e);
      lu_mtlx_child_push(&ge, &e);
    }
    lu_mtlx_library_add_from(&cg, &ge);
    lu_mtlx_library_add_library(c, &cg);
    lu_mtlx_library_free(&cg);
    lu_mtlx_elem_free(&ge);
  }
}

void CheckDefs(const MtlxLibrary &cpp, const lu_mtlx_library &c) {
  if (cpp.Defs().size() != c.ndefs) return;
  for (size_t i = 0; i < c.ndefs; i++) {
    const MtlxNodeDefInfo &a = cpp.Defs()[i];
    lu_mtlx_ports in, out;
    lu_mtlx_library_active_inputs(&c, &c.defs[i], &in);
    lu_mtlx_library_active_outputs(&c, &c.defs[i], &out);
    lu_mtlx_sb t;
    lu_mtlx_sb_init(&t);
    lu_mtlx_library_out_type(&c, &c.defs[i], &t);
    Count("def_ports");
    if (!SamePorts(cpp.ActiveInputs(a), in.v, in.n)) Mismatch("active_inputs", a.name);
    if (!SamePorts(cpp.ActiveOutputs(a), out.v, out.n)) Mismatch("active_outputs", a.name);
    if (cpp.OutType(a) != S(t)) Mismatch("out_type", a.name);
    const MtlxNodeDefInfo *f = cpp.Find(a.name);
    const lu_mtlx_nodedef *cf = lu_mtlx_library_find(&c, c.defs[i].name);
    if ((f ? f - cpp.Defs().data() : -1) != (cf ? cf - c.defs : -1)) Mismatch("find", a.name);
    free(in.v);
    free(out.v);
    lu_mtlx_sb_free(&t);
  }
}

long Idx(const MtlxLibrary &l, const MtlxNodeDefInfo *d) { return d ? d - l.Defs().data() : -1; }
long Idx(const lu_mtlx_library &l, const lu_mtlx_nodedef *d) { return d ? d - l.defs : -1; }

void CheckResolveAll(const MtlxElem &e, const MtlxScope &outer, const lu_mtlx_elem &ce,
                     const lu_mtlx_scope &couter, const MtlxLibrary &cpp, const lu_mtlx_library &c) {
  Count("resolve_node");
  if (Idx(cpp, cpp.ResolveStrict(e, outer)) != Idx(c, lu_mtlx_library_resolve_strict(&c, &ce, &couter)))
    Mismatch("resolve_strict", e.tag + " " + e.Name());
  if (Idx(cpp, cpp.ResolveReference(e, outer)) !=
      Idx(c, lu_mtlx_library_resolve_reference(&c, &ce, &couter)))
    Mismatch("resolve_reference", e.tag + " " + e.Name());
  if (Idx(cpp, cpp.ResolveRough(e, outer)) != Idx(c, lu_mtlx_library_resolve_rough(&c, &ce, &couter)))
    Mismatch("resolve_rough", e.tag + " " + e.Name());
  if (lightusd::mtlx::IsMtlxNodeElem(e) != (lu_mtlx_is_node_elem(&ce) != 0))
    Mismatch("is_node_elem", e.tag);
  MtlxScope inner{&e};
  inner.insert(inner.end(), outer.begin(), outer.end());
  lu_mtlx_scope cinner;
  lu_mtlx_scope_with(&cinner, &ce, &couter);
  for (size_t i = 0; i < e.children.size(); i++)
    CheckResolveAll(e.children[i], inner, ce.children[i], cinner, cpp, c);
  lu_mtlx_scope_free(&cinner);
}

void CheckDoc(const MtlxElem &root, bool heavy) {
  for (int variant = 0; variant < 3; variant++) {
    MtlxLibrary cpp;
    lu_mtlx_library c;
    BuildLibs(root, variant >= 1, variant >= 2, &cpp, &c);
    CompareLibs(cpp, c, "library v" + std::to_string(variant));
    if (heavy || variant == 0) CheckDefs(cpp, c);
    {
      lu_mtlx_elem croot;
      ToC(root, &croot);
      lu_mtlx_scope empty;
      lu_mtlx_scope_init(&empty);
      for (size_t i = 0; i < root.children.size(); i++) {
        MtlxScope outer{&root};
        lu_mtlx_scope co;
        lu_mtlx_scope_with(&co, &croot, nullptr);
        CheckResolveAll(root.children[i], outer, croot.children[i], co, cpp, c);
        lu_mtlx_scope_free(&co);
      }
      lu_mtlx_elem_free(&croot);
    }
    const MtlxScope outer{&root};
    for (const MtlxElem &g : root.children) {
      if (g.tag != "nodegraph") continue;
      DiffResolveGraph(root, g, cpp, c, outer, nullptr, nullptr);
      if (variant == 0) DiffResolveGraph(root, g, cpp, c, {}, nullptr, nullptr);
      for (size_t max : {0, 1, 2, 256}) DiffFlatten(root, g, cpp, c, outer, nullptr, max, nullptr, nullptr);
      for (const MtlxElem &n : g.children) {
        if (!lightusd::mtlx::IsMtlxNodeElem(n)) continue;
        const std::string name = n.Name();
        DiffFlatten(root, g, cpp, c, outer, &name, 0, nullptr, nullptr);
      }
    }
    DiffResolveGraphs(root, cpp, c, nullptr, nullptr);
    DiffDump(cpp, c, root);
    lu_mtlx_library_free(&c);
  }
  // ApplyMtlxNodeDefDefaults.
  MtlxElem cpp = root;
  ApplyMtlxNodeDefDefaults(&cpp);
  lu_mtlx_elem c;
  ToC(root, &c);
  const int ok = lu_mtlx_apply_nodedef_defaults(&c);
  std::string why;
  Count("apply_defaults");
  if (!ok || !Same(cpp, c, "", &why)) Mismatch("apply_defaults", why);
  lu_mtlx_elem_free(&c);
}

// ---- mutations -------------------------------------------------------------

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  size_t Below(size_t n) { return n ? Next() % n : 0; }
};

void Collect(MtlxElem *e, std::vector<MtlxElem *> *all) {
  all->push_back(e);
  for (MtlxElem &c : e->children) Collect(&c, all);
}

void Mutate(MtlxElem *root, Rng *rng) {
  std::vector<MtlxElem *> all;
  Collect(root, &all);
  std::vector<std::string> values, tags;
  for (MtlxElem *e : all) {
    tags.push_back(e->tag);
    for (auto &a : e->attrs) values.push_back(a.second);
  }
  static const char *const kKeys[] = {
      "nodename", "nodegraph", "output",  "interfacename",    "value",   "type",
      "nodedef",  "channels",  "version", "namespace",        "inherit", "isdefaultversion",
      "target",   "colorspace", "name",   "defaultgeomprop", "geomprop", "fileprefix"};
  static const char *const kVals[] = {"", "float", "color3", "vector3", "out", "true", "1.0",
                                      "0,0,0", "1 2 3", "x", "rrr", "multioutput", "ns", "1.39"};
  values.insert(values.end(), std::begin(kVals), std::end(kVals));
  tags.push_back("input");
  tags.push_back("output");
  tags.push_back("nodegraph");
  tags.push_back("implementation");
  MtlxElem *e = all[rng->Below(all.size())];
  switch (rng->Below(7)) {
    case 0:
      if (!e->attrs.empty()) e->attrs.erase(e->attrs.begin() + rng->Below(e->attrs.size()));
      break;
    case 1:
    case 2:
      e->SetAttr(kKeys[rng->Below(sizeof(kKeys) / sizeof(kKeys[0]))], values[rng->Below(values.size())]);
      break;
    case 3:
      if (!e->children.empty()) e->children.erase(e->children.begin() + rng->Below(e->children.size()));
      break;
    case 4:
      if (!e->children.empty()) {
        MtlxElem dup = e->children[rng->Below(e->children.size())];
        e->children.push_back(dup);
      }
      break;
    case 5:
      if (e != root) e->tag = tags[rng->Below(tags.size())];
      break;
    case 6:
      if (!e->attrs.empty()) e->attrs[rng->Below(e->attrs.size())].second = values[rng->Below(values.size())];
      break;
  }
}

}  // namespace cdiff

// ---- replay of tests/core/test_mtlx_lean_vectors.cc with taps --------------

namespace cdiff_tap {
using namespace lightusd::mtlx;
inline bool TapResolveMtlxGraph(const MtlxElem &document, const MtlxElem &graph,
                                const MtlxLibrary &library, const MtlxScope &outer,
                                MtlxResolvedGraph *resolved, std::string *err) {
  lu_mtlx_library c;
  cdiff::LibToC(library, &c);
  const bool ok = cdiff::DiffResolveGraph(document, graph, library, c, outer, resolved, err);
  lu_mtlx_library_free(&c);
  return ok;
}
inline bool TapResolveMtlxGraphs(const MtlxElem &document, const MtlxLibrary &library,
                                 std::vector<std::string> *order, std::string *err) {
  lu_mtlx_library c;
  cdiff::LibToC(library, &c);
  const bool ok = cdiff::DiffResolveGraphs(document, library, c, order, err);
  lu_mtlx_library_free(&c);
  return ok;
}
inline bool TapFlattenMtlxGraphCall(const MtlxElem &document, const MtlxElem &graph,
                                    const MtlxLibrary &library, const MtlxScope &outer,
                                    const std::string &call_name, MtlxElem *flattened,
                                    std::string *err) {
  lu_mtlx_library c;
  cdiff::LibToC(library, &c);
  const bool ok = cdiff::DiffFlatten(document, graph, library, c, outer, &call_name, 0, flattened, err);
  lu_mtlx_library_free(&c);
  return ok;
}
inline bool TapFlattenMtlxGraph(const MtlxElem &document, const MtlxElem &graph,
                                const MtlxLibrary &library, const MtlxScope &outer, size_t max,
                                MtlxElem *flattened, std::string *err) {
  lu_mtlx_library c;
  cdiff::LibToC(library, &c);
  const bool ok = cdiff::DiffFlatten(document, graph, library, c, outer, nullptr, max, flattened, err);
  lu_mtlx_library_free(&c);
  return ok;
}
inline std::string TapDumpMtlxDocument(const MtlxLibrary &library, const MtlxElem &root) {
  lu_mtlx_library c;
  cdiff::LibToC(library, &c);
  const std::string s = cdiff::DiffDump(library, c, root);
  lu_mtlx_library_free(&c);
  return s;
}
}  // namespace cdiff_tap

using namespace cdiff_tap;
#define ResolveMtlxGraph TapResolveMtlxGraph
#define ResolveMtlxGraphs TapResolveMtlxGraphs
#define FlattenMtlxGraphCall TapFlattenMtlxGraphCall
#define FlattenMtlxGraph TapFlattenMtlxGraph
#define DumpMtlxDocument TapDumpMtlxDocument
#define main lean_vectors_main
#include "tests/core/test_mtlx_lean_vectors.cc"
#undef main
#undef ResolveMtlxGraph
#undef ResolveMtlxGraphs
#undef FlattenMtlxGraphCall
#undef FlattenMtlxGraph
#undef DumpMtlxDocument

// ---- Lean vectors checked directly against the C ---------------------------

namespace cdiff {

lu_mtlx_library g_lean_lib;

void LeanParse(const char *xml, lu_mtlx_elem *root) {
  lu_mtlx_elem_init(root);
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  if (!lu_mtlx_parse_xml(xml, strlen(xml), root, &err)) Mismatch("lean parse", S(err));
  lu_mtlx_sb_free(&err);
}
void LeanResolveLib(const char *xml) {
  lu_mtlx_elem root;
  LeanParse(xml, &root);
  lu_mtlx_library_add_from(&g_lean_lib, &root);
  lu_mtlx_elem_free(&root);
}
void LeanResolve(int id, const char *xml, const char *strict, const char *ref, const char *rough) {
  lu_mtlx_elem node;
  LeanParse(xml, &node);
  lu_mtlx_scope empty;
  lu_mtlx_scope_init(&empty);
  const lu_mtlx_nodedef *s = lu_mtlx_library_resolve_strict(&g_lean_lib, &node, &empty);
  const lu_mtlx_nodedef *r = lu_mtlx_library_resolve_reference(&g_lean_lib, &node, &empty);
  const lu_mtlx_nodedef *g = lu_mtlx_library_resolve_rough(&g_lean_lib, &node, &empty);
  Count("lean_resolve");
  if (std::string(s ? s->name : "") != strict || std::string(r ? r->name : "") != ref ||
      std::string(g ? g->name : "") != rough)
    Mismatch("lean MTLX_RESOLVE", std::to_string(id));
  lu_mtlx_elem_free(&node);
}
void LeanDump(int id, const char *xml, const char *dump) {
  lu_mtlx_elem root;
  LeanParse(xml, &root);
  lu_mtlx_library lib;
  lu_mtlx_library_init(&lib);
  lu_mtlx_library_add_from(&lib, &root);
  lu_mtlx_sb got;
  lu_mtlx_sb_init(&got);
  lu_mtlx_dump_document(&lib, &root, &got);
  Count("lean_dump");
  if (S(got) != dump) Mismatch("lean MTLX_DUMP", std::to_string(id) + "\n" + S(got));
  lu_mtlx_sb_free(&got);
  lu_mtlx_library_free(&lib);
  lu_mtlx_elem_free(&root);
}
void RunLeanDirect() {
  lu_mtlx_library_init(&g_lean_lib);
#define MTLX_SCOPE(...)
#define MTLX_INCLUDE(...)
#define MTLX_RESOLVE_LIB(xml) LeanResolveLib(xml);
#define MTLX_RESOLVE(id, node_xml, strict, ref, rough) LeanResolve(id, node_xml, strict, ref, rough);
#define MTLX_DUMP(id, xml, dump) LeanDump(id, xml, dump);
#include "tests/core/generated/lean-mtlx/mtlx-doc.inc"
#undef MTLX_SCOPE
#undef MTLX_INCLUDE
#undef MTLX_RESOLVE_LIB
#undef MTLX_RESOLVE
#undef MTLX_DUMP
  lu_mtlx_library_free(&g_lean_lib);
}

void AddArg(const std::string &arg, std::vector<std::string> *files) {
  if (!arg.empty() && arg[0] == '@') {
    std::ifstream in(arg.substr(1));
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) files->push_back(line);
  } else {
    files->push_back(arg);
  }
}

}  // namespace cdiff

int main(int argc, char **argv) {
  std::vector<std::string> files;
  size_t mutations = 0;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--mutations" && i + 1 < argc) {
      mutations = std::stoul(argv[++i]);
      continue;
    }
    cdiff::AddArg(argv[i], &files);
  }
  cdiff::g_label = "lean-direct";
  cdiff::RunLeanDirect();
  cdiff::g_label = "lean-replay";
  const int lean_rc = lean_vectors_main();
  if (lean_rc != 0) cdiff::Mismatch("lean replay", "C++ test_mtlx_lean_vectors failed");
  size_t docs = 0, parse_fail = 0;
  for (const std::string &path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    MtlxElem root;
    std::string err;
    if (!lightusd::mtlx::ParseMtlxXml(ss.str(), &root, &err)) { ++parse_fail; continue; }
    ++docs;
    cdiff::g_label = path;
    cdiff::CheckDoc(root, true);
    MtlxElem up = root;
    lightusd::mtlx::UpgradeMtlxVersion(&up);
    cdiff::g_label = path + "+upgraded";
    cdiff::CheckDoc(up, true);
    MtlxElem prepared = up;
    cdiff::InheritMtlxAttributes(&prepared);
    cdiff::ApplyMtlxNodeDefDefaults(&prepared);
    cdiff::g_label = path + "+prepared";
    cdiff::CheckDoc(prepared, false);
    cdiff::Rng rng{0x9e3779b97f4a7c15ull ^ std::hash<std::string>()(path)};
    for (size_t m = 0; m < mutations; m++) {
      MtlxElem mut = (m & 1) ? up : root;
      const size_t steps = 1 + rng.Below(3);
      for (size_t k = 0; k < steps; k++) cdiff::Mutate(&mut, &rng);
      cdiff::g_label = path + "#mut" + std::to_string(m);
      cdiff::CheckDoc(mut, false);
    }
  }
  std::printf("files=%zu parsed=%zu parse_fail=%zu checks=%zu mismatch=%zu\n", files.size(), docs,
              parse_fail, cdiff::g_checks, cdiff::g_mismatch);
  for (const auto &kv : cdiff::g_counts) std::printf("  %-16s %zu\n", kv.first.c_str(), kv.second);
  return cdiff::g_mismatch || docs == 0 ? 1 : 0;
}
