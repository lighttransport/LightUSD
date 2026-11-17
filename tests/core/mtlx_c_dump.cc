// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// mtlx_c_dump FILE.mtlx
//
// The C11 MaterialX core's side of the differential check: the same output
// format as mtlx_oracle_dump (the former C++ API), produced through
// lightusd-mtlx.h and the core's internal functions. tests/core/
// run_mtlx_diff.py compares the two byte for byte.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "c/lightusd-mtlx.h"
#include "c/mtlx/mtlx-internal.h"

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

const char *G(const lu_mtlx_elem *e, const char *key) { return lu_mtlx_elem_get(e, key); }

std::string Value(const lu_mtlx_elem *e) {
  lu_mtlx_value v;
  if (!lu_mtlx_input_value(e, &v)) return "oom";
  std::string out;
  switch (v.type) {
    case LU_MTLX_VALUE_NONE: out = "none"; break;
    case LU_MTLX_VALUE_BOOL: out = std::string("bool ") + (v.b ? "1" : "0"); break;
    case LU_MTLX_VALUE_INT: out = "int " + std::to_string(v.i); break;
    case LU_MTLX_VALUE_FLOAT: out = "float " + F(v.f); break;
    case LU_MTLX_VALUE_STRING: out = "string " + Q(v.s); break;
    case LU_MTLX_VALUE_FLOATS:
      out = "floats";
      for (size_t i = 0; i < v.nfloats; i++) out += " " + F(v.floats[i]);
      break;
    case LU_MTLX_VALUE_INTS:
      out = "ints";
      for (size_t i = 0; i < v.nints; i++) out += " " + std::to_string(v.ints[i]);
      break;
  }
  lu_mtlx_value_free(&v);
  return out;
}

// MtlxElement getters: name/type/nodedef are dedicated members, the rest are
// "extra" attributes.
void Element(const char *indent, const lu_mtlx_elem *e) {
  std::printf("%sname=%s type=%s nodedef=%s colorspace=%s fileprefix=%s namespace=%s value=%s\n",
              indent, Q(G(e, "name")).c_str(), Q(G(e, "type")).c_str(), Q(G(e, "nodedef")).c_str(),
              Q(G(e, "colorspace")).c_str(), Q(G(e, "fileprefix")).c_str(),
              Q(G(e, "namespace")).c_str(), Value(e).c_str());
}

void Input(const char *indent, const lu_mtlx_elem *in) {
  Element(indent, in);
  std::printf("%s  nodename=%s nodegraph=%s output=%s interfacename=%s channels=%s unit=%s unittype=%s\n",
              indent, Q(G(in, "nodename")).c_str(), Q(G(in, "nodegraph")).c_str(),
              Q(G(in, "output")).c_str(), Q(G(in, "interfacename")).c_str(),
              Q(G(in, "channels")).c_str(), Q(G(in, "unit")).c_str(), Q(G(in, "unittype")).c_str());
}

void Node(const char *indent, const lu_mtlx_elem *n) {
  std::printf("%snode category=%s\n", indent, Q(lu_mtlx_elem_category(n)).c_str());
  Element(indent, n);
  for (size_t i = 0; i < lu_mtlx_elem_count(n, LU_MTLX_CHILD_INPUTS); i++) {
    std::printf("%s  input\n", indent);
    Input((std::string(indent) + "    ").c_str(), lu_mtlx_elem_at(n, LU_MTLX_CHILD_INPUTS, i));
  }
}

std::vector<std::string> SplitList(const std::string &text) {
  std::string normalized = text;
  for (char &c : normalized)
    if (c == ',') c = ' ';
  std::istringstream input(normalized);
  std::vector<std::string> result;
  std::string token;
  while (input >> token) result.push_back(token);
  return result;
}

void Walk(const lu_mtlx_document *d) {
  std::printf("doc version=%s colorspace=%s namespace=%s\n", Q(lu_mtlx_document_version(d)).c_str(),
              Q(lu_mtlx_document_colorspace(d)).c_str(), Q(lu_mtlx_document_namespace(d)).c_str());
  for (size_t i = 0; i < lu_mtlx_document_count(d, LU_MTLX_NODES); i++)
    Node("", lu_mtlx_document_at(d, LU_MTLX_NODES, i));
  for (size_t i = 0; i < lu_mtlx_document_count(d, LU_MTLX_NODEGRAPHS); i++) {
    const lu_mtlx_elem *g = lu_mtlx_document_at(d, LU_MTLX_NODEGRAPHS, i);
    std::printf("nodegraph\n");
    Element("  ", g);
    for (size_t k = 0; k < lu_mtlx_elem_count(g, LU_MTLX_CHILD_INPUTS); k++) {
      std::printf("  graphinput\n");
      Input("    ", lu_mtlx_elem_at(g, LU_MTLX_CHILD_INPUTS, k));
    }
    for (size_t k = 0; k < lu_mtlx_elem_count(g, LU_MTLX_CHILD_OUTPUTS); k++) {
      const lu_mtlx_elem *out = lu_mtlx_elem_at(g, LU_MTLX_CHILD_OUTPUTS, k);
      std::printf("  graphoutput nodename=%s output=%s\n", Q(G(out, "nodename")).c_str(),
                  Q(G(out, "output")).c_str());
      Element("    ", out);
    }
    for (size_t k = 0; k < lu_mtlx_elem_count(g, LU_MTLX_CHILD_GRAPH_NODES); k++)
      Node("  ", lu_mtlx_elem_at(g, LU_MTLX_CHILD_GRAPH_NODES, k));
  }
  for (size_t i = 0; i < lu_mtlx_document_count(d, LU_MTLX_MATERIALS); i++) {
    const lu_mtlx_elem *m = lu_mtlx_document_at(d, LU_MTLX_MATERIALS, i);
    lu_mtlx_material_refs r;
    lu_mtlx_material_refs_get(m, &r);
    std::printf("material surface=%s surfacegraph=%s surfaceoutput=%s displacement=%s volume=%s volumegraph=%s volumeoutput=%s\n",
                Q(r.surface).c_str(), Q(r.surface_graph).c_str(), Q(r.surface_output).c_str(),
                Q(r.displacement).c_str(), Q(r.volume).c_str(), Q(r.volume_graph).c_str(),
                Q(r.volume_output).c_str());
    Element("  ", m);
  }
  for (size_t i = 0; i < lu_mtlx_document_count(d, LU_MTLX_LOOKS); i++) {
    const lu_mtlx_elem *l = lu_mtlx_document_at(d, LU_MTLX_LOOKS, i);
    std::printf("look\n");
    Element("  ", l);
    for (size_t k = 0; k < lu_mtlx_elem_count(l, LU_MTLX_CHILD_ASSIGNMENTS); k++) {
      const lu_mtlx_elem *a = lu_mtlx_elem_at(l, LU_MTLX_CHILD_ASSIGNMENTS, k);
      std::printf("  assign name=%s material=%s geom=%s collection=%s\n", Q(G(a, "name")).c_str(),
                  Q(G(a, "material")).c_str(), Q(G(a, "geom")).c_str(),
                  Q(G(a, "collection")).c_str());
    }
  }
  for (size_t i = 0; i < lu_mtlx_document_count(d, LU_MTLX_COLLECTIONS); i++) {
    const lu_mtlx_elem *c = lu_mtlx_document_at(d, LU_MTLX_COLLECTIONS, i);
    std::printf("collection includegeom=%s excludegeom=%s includecollections=",
                Q(G(c, "includegeom")).c_str(), Q(G(c, "excludegeom")).c_str());
    for (const auto &s : SplitList(G(c, "includecollection"))) std::printf("%s,", Q(s).c_str());
    std::printf("\n");
    Element("  ", c);
  }
}

void Lookups(const lu_mtlx_document *d) {
  std::set<std::string> names;
  for (lu_mtlx_class k : {LU_MTLX_NODES, LU_MTLX_NODEGRAPHS, LU_MTLX_MATERIALS})
    for (size_t i = 0; i < lu_mtlx_document_count(d, k); i++)
      names.insert(G(lu_mtlx_document_at(d, k, i), "name"));
  names.insert("__missing__");
  for (const std::string &n : names)
    std::printf("find %s node=%d graph=%d material=%d look=%d collection=%d\n", Q(n).c_str(),
                lu_mtlx_document_find(d, LU_MTLX_NODES, n.c_str()) != nullptr,
                lu_mtlx_document_find(d, LU_MTLX_NODEGRAPHS, n.c_str()) != nullptr,
                lu_mtlx_document_find(d, LU_MTLX_MATERIALS, n.c_str()) != nullptr,
                lu_mtlx_document_find(d, LU_MTLX_LOOKS, n.c_str()) != nullptr,
                lu_mtlx_document_find(d, LU_MTLX_COLLECTIONS, n.c_str()) != nullptr);
}

void Section(const char *name) { std::printf("== %s\n", name); }

void Resolved(const lu_mtlx_document *d) {
  char *xml = lu_mtlx_document_resolved_xml(d);
  std::printf("resolved %s\n", Q(xml ? xml : "").c_str());
  std::free(xml);
}

std::string Take(char *s) {
  std::string out = s ? s : "";
  std::free(s);
  return out;
}

void CollectUnits(const lu_mtlx_elem *root, std::set<std::pair<std::string, std::string>> *out) {
  std::vector<const lu_mtlx_elem *> stack{root};
  while (!stack.empty()) {
    const lu_mtlx_elem *x = stack.back();
    stack.pop_back();
    if (!std::strcmp(x->tag, "unitdef")) {
      const std::string type = lu_mtlx_attr_or(x, "unittype", "");
      for (size_t i = 0; i < x->nchildren; i++)
        if (!std::strcmp(x->children[i].tag, "unit"))
          out->insert({type, lu_mtlx_name(&x->children[i])});
    }
    for (size_t i = 0; i < x->nchildren; i++) stack.push_back(&x->children[i]);
  }
}

void Geoms(const lu_mtlx_elem *root, std::set<std::string> *out) {
  std::vector<const lu_mtlx_elem *> stack{root};
  while (!stack.empty()) {
    const lu_mtlx_elem *x = stack.back();
    stack.pop_back();
    for (const char *key : {"geom", "includegeom", "excludegeom"})
      if (const char *v = lu_mtlx_get_attr(x, key)) {
        std::string cur;
        for (char c : std::string(v) + ",") {
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
    for (size_t i = 0; i < x->nchildren; i++) stack.push_back(&x->children[i]);
  }
  out->insert("/not/assigned");
}

struct DirLoader {
  std::string dir;
};

int LoadRelative(void *user, const char *href, const char * /*including*/, char **xml,
                 size_t *len) {
  std::string text;
  if (!ReadFile(static_cast<DirLoader *>(user)->dir + "/" + href, &text)) return 0;
  char *buf = static_cast<char *>(std::malloc(text.size() + 1));
  if (!buf) return 0;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  *xml = buf;
  *len = text.size();
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: mtlx_c_dump FILE.mtlx\n");
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
    lu_mtlx_document *doc = lu_mtlx_document_create();
    const int ok = lu_mtlx_document_parse(doc, text.data(), text.size());
    std::printf("ok=%d error=%s warning=%s\n", ok, Q(lu_mtlx_document_error(doc)).c_str(),
                Q(lu_mtlx_document_warning(doc)).c_str());
    if (ok) {
      Resolved(doc);
      Walk(doc);
    }
    lu_mtlx_document_destroy(doc);
  }

  Section("parse-anchored");
  lu_mtlx_document *doc = lu_mtlx_document_create();
  const int ok = lu_mtlx_document_parse_anchored(doc, text.data(), text.size(), path.c_str());
  std::printf("ok=%d error=%s warning=%s\n", ok, Q(lu_mtlx_document_error(doc)).c_str(),
              Q(lu_mtlx_document_warning(doc)).c_str());
  if (ok) {
    Resolved(doc);
    Walk(doc);
    Lookups(doc);

    Section("resolve");
    char **order = nullptr;
    size_t norder = 0;
    char *err = nullptr;
    const int rok = lu_mtlx_document_resolve_graphs(doc, nullptr, &order, &norder, &err);
    std::printf("ok=%d error=%s order=", rok, Q(Take(err)).c_str());
    for (size_t i = 0; i < norder; i++) std::printf("%s,", Q(order[i]).c_str());
    std::printf("\n");
    lu_mtlx_free_strings(order, norder);

    Section("flatten");
    for (size_t i = 0; i < lu_mtlx_document_count(doc, LU_MTLX_NODEGRAPHS); i++) {
      const std::string name = G(lu_mtlx_document_at(doc, LU_MTLX_NODEGRAPHS, i), "name");
      lu_mtlx_document *copy = lu_mtlx_document_create();
      lu_mtlx_document_parse_anchored(copy, text.data(), text.size(), path.c_str());
      char *ferr = nullptr;
      const int fok = lu_mtlx_document_flatten_graph(copy, name.c_str(), nullptr, 256, &ferr);
      std::printf("graph %s ok=%d error=%s\n", Q(name).c_str(), fok, Q(Take(ferr)).c_str());
      if (fok) Resolved(copy);
      lu_mtlx_document_destroy(copy);
    }

    lu_mtlx_elem root;
    lu_mtlx_elem_init(&root);
    lu_mtlx_sb perr;
    lu_mtlx_sb_init(&perr);
    DirLoader loader{DirOf(path)};
    const bool tree = lu_mtlx_parse_xml(text.data(), text.size(), &root, &perr) &&
                      lu_mtlx_expand_includes(&root, LoadRelative, &loader, &perr);

    Section("units");
    if (tree) {
      std::set<std::pair<std::string, std::string>> units;
      CollectUnits(&root, &units);
      for (const auto &u : units) {
        lu_mtlx_document *copy = lu_mtlx_document_create();
        lu_mtlx_document_parse_anchored(copy, text.data(), text.size(), path.c_str());
        const int cok = lu_mtlx_document_convert_to_unit(copy, u.first.c_str(), u.second.c_str());
        std::printf("convert %s %s ok=%d error=%s\n", Q(u.first).c_str(), Q(u.second).c_str(), cok,
                    Q(lu_mtlx_document_error(copy)).c_str());
        if (cok) Resolved(copy);
        lu_mtlx_document_destroy(copy);
      }
    }

    Section("assign");
    if (tree) {
      std::set<std::string> geoms;
      Geoms(&root, &geoms);
      for (size_t i = 0; i < lu_mtlx_document_count(doc, LU_MTLX_LOOKS); i++) {
        const std::string look = G(lu_mtlx_document_at(doc, LU_MTLX_LOOKS, i), "name");
        for (const std::string &g : geoms) {
          lu_mtlx_assignment *out = nullptr;
          size_t n = 0;
          char *aerr = nullptr;
          const int aok =
              lu_mtlx_document_assignments(doc, look.c_str(), g.c_str(), &out, &n, &aerr);
          std::printf("look %s geom %s ok=%d error=%s ->", Q(look).c_str(), Q(g).c_str(), aok,
                      Q(Take(aerr)).c_str());
          for (size_t k = 0; k < n; k++)
            std::printf(" %s:%s", Q(G(out[k].assignment, "name")).c_str(),
                        Q(G(out[k].material, "name")).c_str());
          std::printf("\n");
          std::free(out);
        }
      }
    }

    Section("dump");
    if (tree) {
      lu_mtlx_library lib;
      lu_mtlx_library_init(&lib);
      lu_mtlx_library_add_from(&lib, &root);
      lu_mtlx_sb dump;
      lu_mtlx_sb_init(&dump);
      lu_mtlx_dump_document(&lib, &root, &dump);
      std::printf("plain %s\n", Q(lu_mtlx_sb_str(&dump)).c_str());
      lu_mtlx_library_free(&lib);
      lu_mtlx_elem up;
      lu_mtlx_elem_copy(&up, &root);
      lu_mtlx_upgrade_version(&up);
      lu_mtlx_library uplib;
      lu_mtlx_library_init(&uplib);
      lu_mtlx_library_add_from(&uplib, &up);
      lu_mtlx_sb_clear(&dump);
      lu_mtlx_dump_document(&uplib, &up, &dump);
      std::printf("upgraded %s\n", Q(lu_mtlx_sb_str(&dump)).c_str());
      lu_mtlx_sb_free(&dump);
      lu_mtlx_library_free(&uplib);
      lu_mtlx_elem_free(&up);
    } else {
      std::printf("tree-error %s\n", Q(lu_mtlx_sb_str(&perr)).c_str());
    }
    lu_mtlx_sb_free(&perr);
    lu_mtlx_elem_free(&root);
  }
  lu_mtlx_document_destroy(doc);
  return 0;
}
