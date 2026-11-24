/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX documents (C11): parse pipeline, element classes and accessors,
 * input values, material shader references, graph resolution and flattening.
 * Port of MtlxDocument and the MtlxElement classes of the former
 * src/core/mtlx-dom.cc. See lightusd-mtlx.h. */
#include "c/lightusd-mtlx.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "mtlx-doc.h"

/* ---- document object ------------------------------------------------------ */

typedef lu_mtlx_elem_list elem_list;

static void list_free(elem_list *l) {
  free((void *)l->v);
  memset(l, 0, sizeof(*l));
}

static int list_push(elem_list *l, const lu_mtlx_elem *e) {
  if (l->n == l->cap) {
    const size_t cap = l->cap ? l->cap * 2 : 16;
    const lu_mtlx_elem **v = (const lu_mtlx_elem **)realloc((void *)l->v, cap * sizeof(*v));
    if (!v) return 0;
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = e;
  return 1;
}

lu_mtlx_document *lu_mtlx_document_create(void) {
  lu_mtlx_document *d = (lu_mtlx_document *)calloc(1, sizeof(*d));
  if (!d) return NULL;
  lu_mtlx_elem_init(&d->root);
  lu_mtlx_sb_init(&d->error);
  lu_mtlx_sb_init(&d->warning);
  return d;
}

static void clear_classes(lu_mtlx_document *d) {
  for (int k = 0; k < LU_MTLX_DOC_CLASSES; k++) list_free(&d->classes[k]);
}

void lu_mtlx_document_destroy(lu_mtlx_document *d) {
  if (!d) return;
  clear_classes(d);
  lu_mtlx_elem_free(&d->root);
  lu_mtlx_sb_free(&d->error);
  lu_mtlx_sb_free(&d->warning);
  free(d);
}

void lu_mtlx_doc_set_error(lu_mtlx_document *d, const char *prefix, const char *msg) {
  lu_mtlx_sb_clear(&d->error);
  d->error.oom = 0;
  lu_mtlx_sb_puts(&d->error, prefix);
  lu_mtlx_sb_puts(&d->error, msg);
}

/* MtlxDocument::ParseElement classification of the root's children. */
static int build_classes(lu_mtlx_document *d) {
  clear_classes(d);
  for (size_t i = 0; i < d->root.nchildren; i++) {
    const lu_mtlx_elem *c = &d->root.children[i];
    const char *t = c->tag;
    int k;
    if (!strcmp(t, "nodegraph")) k = LU_MTLX_NODEGRAPHS;
    else if (!strcmp(t, "look")) k = LU_MTLX_LOOKS;
    else if (!strcmp(t, "collection")) k = LU_MTLX_COLLECTIONS;
    else if (!strcmp(t, "surfacematerial") || !strcmp(t, "volumematerial")) k = LU_MTLX_MATERIALS;
    else if (!strcmp(t, "typedef") || !strcmp(t, "geominfo")) continue;
    else k = LU_MTLX_NODES;
    if (!list_push(&d->classes[k], c)) return 0;
  }
  return 1;
}

/* MtlxDocument::ParseRoot(MtlxElemToXMLNode(root)): the XMLNode-normalised
 * tree becomes the document. `root` is consumed. */
int lu_mtlx_doc_adopt_root(lu_mtlx_document *d, lu_mtlx_elem *root) {
  if (!lu_mtlx_xmlnode_normalize(root)) {
    lu_mtlx_elem_free(root);
    lu_mtlx_doc_set_error(d, "", "out of memory");
    return 0;
  }
  if (strcmp(root->tag, "materialx") != 0) {
    lu_mtlx_elem_free(root);
    lu_mtlx_doc_set_error(d, "", "Invalid MaterialX document");
    return 0;
  }
  lu_mtlx_elem_free(&d->root);
  lu_mtlx_elem_move(&d->root, root);
  d->parsed = 1;
  if (!build_classes(d)) {
    lu_mtlx_doc_set_error(d, "", "out of memory");
    return 0;
  }
  return 1;
}

/* UpgradeMtlxVersion, InheritMtlxAttributes, ApplyMtlxNodeDefDefaults, then
 * ParseRoot. */
static int finish(lu_mtlx_document *d, lu_mtlx_elem *root) {
  lu_mtlx_upgrade_version(root);
  lu_mtlx_inherit_attributes(root);
  if (!lu_mtlx_apply_nodedef_defaults(root)) {
    lu_mtlx_elem_free(root);
    lu_mtlx_doc_set_error(d, "", "out of memory");
    return 0;
  }
  return lu_mtlx_doc_adopt_root(d, root);
}

int lu_mtlx_document_parse(lu_mtlx_document *d, const char *xml, size_t len) {
  lu_mtlx_elem root;
  lu_mtlx_elem_init(&root);
  lu_mtlx_sb err, warn;
  lu_mtlx_sb_init(&err);
  lu_mtlx_sb_init(&warn);
  if (!lu_mtlx_parse_xml_checked(xml, len, &root, &err, &warn)) {
    lu_mtlx_doc_set_error(d, "", lu_mtlx_sb_str(&err));
    lu_mtlx_sb_free(&err);
    lu_mtlx_sb_free(&warn);
    lu_mtlx_elem_free(&root);
    return 0;
  }
  lu_mtlx_sb_set(&d->warning, lu_mtlx_sb_str(&warn));
  lu_mtlx_sb_free(&err);
  lu_mtlx_sb_free(&warn);
  return finish(d, &root);
}

int lu_mtlx_document_parse_with_loader(lu_mtlx_document *d, const char *xml, size_t len,
                                       lu_mtlx_document_loader loader, void *user) {
  lu_mtlx_elem root;
  lu_mtlx_elem_init(&root);
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  if (!lu_mtlx_parse_xml(xml, len, &root, &err)) {
    lu_mtlx_doc_set_error(d, "Failed to parse MaterialX XML: ", lu_mtlx_sb_str(&err));
    lu_mtlx_sb_free(&err);
    lu_mtlx_elem_free(&root);
    return 0;
  }
  if (!lu_mtlx_expand_includes(&root, (lu_mtlx_include_loader)loader, user, &err)) {
    lu_mtlx_doc_set_error(d, "Failed to resolve MaterialX XInclude: ", lu_mtlx_sb_str(&err));
    lu_mtlx_sb_free(&err);
    lu_mtlx_elem_free(&root);
    return 0;
  }
  lu_mtlx_sb_free(&err);
  return finish(d, &root);
}

const char *lu_mtlx_document_error(const lu_mtlx_document *d) { return lu_mtlx_sb_str(&d->error); }
const char *lu_mtlx_document_warning(const lu_mtlx_document *d) {
  return lu_mtlx_sb_str(&d->warning);
}
static const char *root_attr(const lu_mtlx_document *d, const char *key) {
  return d->parsed ? lu_mtlx_attr_or(&d->root, key, "") : "";
}
const char *lu_mtlx_document_version(const lu_mtlx_document *d) { return root_attr(d, "version"); }
const char *lu_mtlx_document_colorspace(const lu_mtlx_document *d) {
  return root_attr(d, "colorspace");
}
const char *lu_mtlx_document_namespace(const lu_mtlx_document *d) {
  return root_attr(d, "namespace");
}

char *lu_mtlx_document_resolved_xml(const lu_mtlx_document *d) {
  lu_mtlx_sb out;
  lu_mtlx_sb_init(&out);
  if (d->parsed && !lu_mtlx_write_xml(&d->root, &out)) {
    lu_mtlx_sb_free(&out);
    return NULL;
  }
  return lu_mtlx_sb_take(&out);
}

size_t lu_mtlx_document_count(const lu_mtlx_document *d, lu_mtlx_class cls) {
  return (unsigned)cls < LU_MTLX_DOC_CLASSES ? d->classes[cls].n : 0;
}

const lu_mtlx_elem *lu_mtlx_document_at(const lu_mtlx_document *d, lu_mtlx_class cls, size_t i) {
  if ((unsigned)cls >= LU_MTLX_DOC_CLASSES || i >= d->classes[cls].n) return NULL;
  return d->classes[cls].v[i];
}

const lu_mtlx_elem *lu_mtlx_document_find(const lu_mtlx_document *d, lu_mtlx_class cls,
                                          const char *name) {
  if ((unsigned)cls >= LU_MTLX_DOC_CLASSES) return NULL;
  const elem_list *l = &d->classes[cls];
  for (size_t i = 0; i < l->n; i++)
    if (!strcmp(lu_mtlx_name(l->v[i]), name)) return l->v[i];
  if (cls != LU_MTLX_NODES) return NULL;
  /* MtlxDocument::FindNode also searches the nodes of every nodegraph. */
  const elem_list *g = &d->classes[LU_MTLX_NODEGRAPHS];
  for (size_t i = 0; i < g->n; i++) {
    const lu_mtlx_elem *n = lu_mtlx_elem_find(g->v[i], LU_MTLX_CHILD_GRAPH_NODES, name);
    if (n) return n;
  }
  return NULL;
}

void lu_mtlx_free_strings(char **v, size_t n) {
  for (size_t i = 0; i < n; i++) free(v[i]);
  free(v);
}

/* ---- elements ------------------------------------------------------------- */

const char *lu_mtlx_elem_tag(const lu_mtlx_elem *e) { return e->tag; }
const char *lu_mtlx_elem_get(const lu_mtlx_elem *e, const char *key) {
  return lu_mtlx_attr_or(e, key, "");
}
int lu_mtlx_elem_has(const lu_mtlx_elem *e, const char *key) {
  return lu_mtlx_get_attr(e, key) != NULL;
}
const char *lu_mtlx_elem_category(const lu_mtlx_elem *e) {
  const char *c = lu_mtlx_get_attr(e, "category");
  return c && c[0] ? c : e->tag;
}

static int child_matches(const lu_mtlx_elem *c, lu_mtlx_children which) {
  switch (which) {
    case LU_MTLX_CHILD_ALL: return 1;
    case LU_MTLX_CHILD_INPUTS: return !strcmp(c->tag, "input");
    case LU_MTLX_CHILD_OUTPUTS: return !strcmp(c->tag, "output");
    case LU_MTLX_CHILD_GRAPH_NODES: return strcmp(c->tag, "input") && strcmp(c->tag, "output");
    case LU_MTLX_CHILD_ASSIGNMENTS: return !strcmp(c->tag, "materialassign");
  }
  return 0;
}

size_t lu_mtlx_elem_count(const lu_mtlx_elem *e, lu_mtlx_children which) {
  size_t n = 0;
  for (size_t i = 0; i < e->nchildren; i++) n += child_matches(&e->children[i], which);
  return n;
}

const lu_mtlx_elem *lu_mtlx_elem_at(const lu_mtlx_elem *e, lu_mtlx_children which, size_t i) {
  for (size_t k = 0; k < e->nchildren; k++)
    if (child_matches(&e->children[k], which) && i-- == 0) return &e->children[k];
  return NULL;
}

const lu_mtlx_elem *lu_mtlx_elem_find(const lu_mtlx_elem *e, lu_mtlx_children which,
                                      const char *name) {
  for (size_t k = 0; k < e->nchildren; k++)
    if (child_matches(&e->children[k], which) && !strcmp(lu_mtlx_name(&e->children[k]), name))
      return &e->children[k];
  return NULL;
}

/* ---- input values (MtlxInput::ParseFromXML) -------------------------------- */

/* std::strtof on the whole text. */
static int whole_strtof(const char *s, float *out) {
  char *end = NULL;
  const float v = strtof(s, &end);
  if (*end != '\0') return 0;
  *out = v;
  return 1;
}

/* std::from_chars(long, base 10) on the whole text, then static_cast<int>. */
static int whole_from_chars_int(const char *s, int *out) {
  const char *p = s;
  const int neg = *p == '-';
  if (neg) p++;
  if (*p < '0' || *p > '9') return 0;
  unsigned long long acc = 0;
  const unsigned long long limit = neg ? (unsigned long long)LONG_MAX + 1ull
                                       : (unsigned long long)LONG_MAX;
  for (; *p >= '0' && *p <= '9'; p++) {
    const unsigned d = (unsigned)(*p - '0');
    if (acc > (limit - d) / 10) return 0; /* errc::result_out_of_range */
    acc = acc * 10 + d;
  }
  if (*p != '\0') return 0;
  const long v = neg ? (acc == (unsigned long long)LONG_MAX + 1ull ? LONG_MIN : -(long)acc)
                     : (long)acc;
  *out = (int)v;
  return 1;
}

/* One getline(',') token with " \t" trimmed; NULL-terminated scan. */
typedef struct tok {
  const char *b;
  size_t n;
} tok;

static size_t comma_tokens(const char *s, tok *out, size_t max) {
  size_t k = 0;
  const char *p = s;
  while (*p) {
    const char *e = strchr(p, ',');
    const char *end = e ? e : p + strlen(p);
    const char *b = p, *x = end;
    while (b < x && (*b == ' ' || *b == '\t')) b++;
    while (x > b && (x[-1] == ' ' || x[-1] == '\t')) x--;
    if (x > b) {
      if (out && k < max) {
        out[k].b = b;
        out[k].n = (size_t)(x - b);
      }
      k++;
    }
    if (!e) break;
    p = e + 1;
  }
  return k;
}

static int parse_float_vector(const char *s, lu_mtlx_value *out) {
  const size_t n = comma_tokens(s, NULL, 0);
  tok *t = n ? (tok *)malloc(n * sizeof(tok)) : NULL;
  if (n && !t) return 0;
  comma_tokens(s, t, n);
  out->floats = n ? (float *)malloc(n * sizeof(float)) : NULL;
  if (n && !out->floats) {
    free(t);
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    char *buf = lu_mtlx_strndup(t[i].b, t[i].n);
    if (!buf) {
      free(t);
      return 0;
    }
    float v;
    if (whole_strtof(buf, &v)) out->floats[out->nfloats++] = v;
    free(buf);
  }
  free(t);
  out->type = LU_MTLX_VALUE_FLOATS;
  return 1;
}

static int parse_int_vector(const char *s, lu_mtlx_value *out) {
  const size_t n = comma_tokens(s, NULL, 0);
  tok *t = n ? (tok *)malloc(n * sizeof(tok)) : NULL;
  if (n && !t) return 0;
  comma_tokens(s, t, n);
  out->ints = n ? (int *)malloc(n * sizeof(int)) : NULL;
  if (n && !out->ints) {
    free(t);
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    char *buf = lu_mtlx_strndup(t[i].b, t[i].n);
    if (!buf) {
      free(t);
      return 0;
    }
    int v;
    if (whole_from_chars_int(buf, &v)) out->ints[out->nints++] = v;
    free(buf);
  }
  free(t);
  out->type = LU_MTLX_VALUE_INTS;
  return 1;
}

int lu_mtlx_input_value(const lu_mtlx_elem *e, lu_mtlx_value *out) {
  memset(out, 0, sizeof(*out));
  if (strcmp(e->tag, "input") != 0) return 1;
  const char *type = lu_mtlx_attr_or(e, "type", "");
  const char *text = lu_mtlx_attr_or(e, "value", "");
  if (!text[0] || !type[0]) return 1;
  if (!strcmp(type, "float")) {
    if (whole_strtof(text, &out->f)) out->type = LU_MTLX_VALUE_FLOAT;
  } else if (!strcmp(type, "integer")) {
    if (whole_from_chars_int(text, &out->i)) out->type = LU_MTLX_VALUE_INT;
  } else if (!strcmp(type, "boolean")) {
    out->type = LU_MTLX_VALUE_BOOL;
    out->b = !strcmp(text, "true") || !strcmp(text, "1");
  } else if (!strcmp(type, "string") || !strcmp(type, "filename")) {
    out->type = LU_MTLX_VALUE_STRING;
    out->s = text;
  } else if (!strcmp(type, "color3") || !strcmp(type, "vector3") || !strcmp(type, "color4") ||
             !strcmp(type, "vector4") || !strcmp(type, "vector2") ||
             !strcmp(type, "floatarray")) {
    if (!parse_float_vector(text, out)) {
      lu_mtlx_value_free(out);
      return 0;
    }
  } else if (!strcmp(type, "integerarray")) {
    if (!parse_int_vector(text, out)) {
      lu_mtlx_value_free(out);
      return 0;
    }
  }
  return 1;
}

void lu_mtlx_value_free(lu_mtlx_value *v) {
  free(v->floats);
  free(v->ints);
  memset(v, 0, sizeof(*v));
}

/* ---- materials (MtlxMaterial::ParseFromXML) -------------------------------- */

void lu_mtlx_material_refs_get(const lu_mtlx_elem *m, lu_mtlx_material_refs *o) {
  o->surface = o->surface_graph = o->surface_output = "";
  o->displacement = "";
  o->volume = o->volume_graph = o->volume_output = "";
  for (size_t i = 0; i < m->nchildren; i++) {
    const lu_mtlx_elem *c = &m->children[i];
    if (!strcmp(c->tag, "shaderref")) {
      const char *name = lu_mtlx_attr_or(c, "name", "");
      const char *node = lu_mtlx_attr_or(c, "node", "");
      if (!strcmp(name, "surfaceshader") || !strcmp(name, "sr")) o->surface = node;
      else if (!strcmp(name, "displacementshader") || !strcmp(name, "dr")) o->displacement = node;
      else if (!strcmp(name, "volumeshader") || !strcmp(name, "vr")) o->volume = node;
    } else if (!strcmp(c->tag, "input")) {
      const char *name = lu_mtlx_attr_or(c, "name", "");
      const char *node = lu_mtlx_attr_or(c, "nodename", "");
      if (!node[0]) node = lu_mtlx_attr_or(c, "node", "");
      const char *graph = lu_mtlx_attr_or(c, "nodegraph", "");
      const char *output = lu_mtlx_attr_or(c, "output", "");
      if (!output[0]) output = "out";
      if (!strcmp(name, "surfaceshader") || !strcmp(name, "sr")) {
        o->surface = node;
        o->surface_graph = graph;
        o->surface_output = output;
      } else if (!strcmp(name, "displacementshader") || !strcmp(name, "dr")) {
        o->displacement = node;
      } else if (!strcmp(name, "volumeshader") || !strcmp(name, "vr")) {
        o->volume = node;
        o->volume_graph = graph;
        o->volume_output = output;
      }
    }
  }
}

/* ---- graphs (MtlxDocument::ResolveNodeGraphs / FlattenNodeGraph) ----------- */

static char *take_error(lu_mtlx_sb *err) {
  char *s = lu_mtlx_sb_take(err);
  return s;
}

/* AddFrom(root) + AddLibrary(external) + the standard nodedefs. */
static int document_library(const lu_mtlx_elem *root, const lu_mtlx_library *external,
                            lu_mtlx_library *lib) {
  lu_mtlx_library_init(lib);
  if (!lu_mtlx_library_add_from(lib, root) ||
      (external && !lu_mtlx_library_add_library(lib, external)) || !lu_mtlx_library_add_stdlib(lib)) {
    lu_mtlx_library_free(lib);
    return 0;
  }
  return 1;
}

int lu_mtlx_document_resolve_graphs(const lu_mtlx_document *d, const lu_mtlx_library *external,
                                    char ***order, size_t *count, char **error) {
  *order = NULL;
  *count = 0;
  if (error) *error = NULL;
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  if (!d->parsed) {
    lu_mtlx_sb_set(&err, "MaterialX document has not been parsed");
    if (error) *error = take_error(&err);
    else lu_mtlx_sb_free(&err);
    return 0;
  }
  lu_mtlx_library lib;
  if (!document_library(&d->root, external, &lib)) {
    lu_mtlx_sb_set(&err, "out of memory");
    if (error) *error = take_error(&err);
    else lu_mtlx_sb_free(&err);
    return 0;
  }
  lu_mtlx_strv names;
  lu_mtlx_strv_init(&names);
  const int ok = lu_mtlx_resolve_graphs(&d->root, &lib, &names, &err);
  lu_mtlx_library_free(&lib);
  if (!ok) {
    lu_mtlx_strv_free(&names);
    if (error) *error = take_error(&err);
    else lu_mtlx_sb_free(&err);
    return 0;
  }
  lu_mtlx_sb_free(&err);
  *order = names.v;
  *count = names.n;
  return 1;
}

static int flatten_fail(lu_mtlx_sb *err, char **error, const char *msg) {
  if (msg) lu_mtlx_sb_set(err, msg);
  if (error) *error = take_error(err);
  else lu_mtlx_sb_free(err);
  return 0;
}

int lu_mtlx_document_flatten_graph(lu_mtlx_document *d, const char *graph,
                                   const lu_mtlx_library *external, size_t max_expansions,
                                   char **error) {
  if (error) *error = NULL;
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  if (!d->parsed) return flatten_fail(&err, error, "MaterialX document has not been parsed");
  lu_mtlx_elem root;
  if (!lu_mtlx_elem_copy(&root, &d->root)) return flatten_fail(&err, error, "out of memory");
  lu_mtlx_library lib;
  if (!document_library(&root, external, &lib)) {
    lu_mtlx_elem_free(&root);
    return flatten_fail(&err, error, "out of memory");
  }
  size_t index = root.nchildren;
  int ok = 1;
  for (size_t i = 0; i < root.nchildren; i++) {
    if (strcmp(root.children[i].tag, "nodegraph") || strcmp(lu_mtlx_name(&root.children[i]), graph))
      continue;
    if (index != root.nchildren) {
      lu_mtlx_sb_set(&err, "ambiguous nodegraph '");
      lu_mtlx_sb_puts(&err, graph);
      lu_mtlx_sb_puts(&err, "'");
      ok = 0;
      break;
    }
    index = i;
  }
  if (ok && index == root.nchildren) {
    lu_mtlx_sb_set(&err, "nodegraph not found: ");
    lu_mtlx_sb_puts(&err, graph);
    ok = 0;
  }
  /* lookup_document: the document plus external, then standard, geompropdefs
   * whose names it does not define. */
  lu_mtlx_elem lookup;
  lu_mtlx_elem_init(&lookup);
  if (ok) ok = lu_mtlx_elem_copy(&lookup, &root);
  lu_mtlx_strv doc_names, avail;
  lu_mtlx_strv_init(&doc_names);
  lu_mtlx_strv_init(&avail);
  for (size_t i = 0; ok && i < root.nchildren; i++)
    if (!strcmp(root.children[i].tag, "geompropdef"))
      ok = lu_mtlx_strv_push(&doc_names, lu_mtlx_name(&root.children[i])) &&
           lu_mtlx_strv_push(&avail, lu_mtlx_name(&root.children[i]));
  for (size_t i = 0; ok && external && i < external->ngeomprop; i++) {
    const lu_mtlx_elem *def = &external->geomprop_defs[i];
    if (lu_mtlx_strv_contains(&doc_names, lu_mtlx_name(def))) continue;
    lu_mtlx_elem c;
    ok = lu_mtlx_elem_copy(&c, def) && lu_mtlx_child_push(&lookup, &c) &&
         lu_mtlx_strv_push(&avail, lu_mtlx_name(def));
  }
  for (size_t i = 0; ok && i < lu_mtlx_stdlib_geomprop_count(); i++) {
    lu_mtlx_elem c;
    ok = lu_mtlx_stdlib_geomprop_elem(i, &c);
    if (!ok) break;
    if (lu_mtlx_strv_contains(&avail, lu_mtlx_name(&c))) {
      lu_mtlx_elem_free(&c);
      continue;
    }
    ok = lu_mtlx_child_push(&lookup, &c) != NULL;
  }
  lu_mtlx_strv_free(&doc_names);
  lu_mtlx_strv_free(&avail);
  lu_mtlx_elem flattened;
  lu_mtlx_elem_init(&flattened);
  if (ok) {
    lu_mtlx_scope outer;
    lu_mtlx_scope_init(&outer);
    ok = lu_mtlx_scope_push(&outer, &root) &&
         lu_mtlx_flatten_graph(&lookup, &root.children[index], &lib, &outer, max_expansions,
                               &flattened, &err);
    lu_mtlx_scope_free(&outer);
  }
  lu_mtlx_elem_free(&lookup);
  lu_mtlx_library_free(&lib);
  if (!ok) {
    lu_mtlx_elem_free(&flattened);
    lu_mtlx_elem_free(&root);
    return flatten_fail(&err, error, err.n || err.oom ? NULL : "out of memory");
  }
  lu_mtlx_elem_free(&root.children[index]);
  lu_mtlx_elem_move(&root.children[index], &flattened);
  /* ParseRoot(MtlxElemToXMLNode(root)) into a fresh document; warning kept. */
  lu_mtlx_sb saved;
  lu_mtlx_sb_init(&saved);
  lu_mtlx_sb_set(&saved, lu_mtlx_sb_str(&d->error));
  if (!lu_mtlx_doc_adopt_root(d, &root)) {
    lu_mtlx_doc_set_error(d, "", lu_mtlx_sb_str(&saved));
    lu_mtlx_sb_free(&saved);
    return flatten_fail(&err, error, "Failed to rebuild MaterialX document after graph flattening");
  }
  lu_mtlx_sb_free(&saved);
  lu_mtlx_sb_clear(&d->error); /* the rebuilt document starts without an error */
  lu_mtlx_sb_free(&err);
  return 1;
}
