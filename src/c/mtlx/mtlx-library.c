/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX node-definition library (C11 port of MtlxLibrary and
 * IsMtlxNodeElem of src/core/mtlx-resolve.cc) and ApplyMtlxNodeDefDefaults of
 * src/core/mtlx-dom.cc.
 */
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

/* ---- helpers ------------------------------------------------------------ */

/* NamespaceAt (anonymous, mtlx-resolve.cc): namespace in scope at `e` (its own
 * attribute first). *ns is borrowed. */
static int namespace_at(const lu_mtlx_elem *e, const lu_mtlx_scope *outer, const char **ns) {
  const char *v = lu_mtlx_get_attr(e, "namespace");
  if (v) {
    *ns = v;
    return 1;
  }
  if (!outer) return 0;
  return lu_mtlx_scoped_attr("namespace", outer, ns);
}

/* QualifyWith (anonymous, mtlx-resolve.cc): result replaces out. */
static void qualify_with(const char *ns, const char *name, lu_mtlx_sb *out) {
  lu_mtlx_sb_clear(out);
  if (!ns || !*ns) {
    lu_mtlx_sb_puts(out, name);
    return;
  }
  const size_t nl = strlen(ns);
  if (strncmp(name, ns, nl) == 0 && name[nl] == ':') {
    lu_mtlx_sb_puts(out, name);
    return;
  }
  lu_mtlx_sb_puts(out, ns);
  lu_mtlx_sb_putc(out, ':');
  lu_mtlx_sb_puts(out, name);
}

static void port_free(lu_mtlx_port *p) {
  free(p->name);
  free(p->type);
  free(p->value);
  free(p->default_geomprop);
}

static void nodedef_free(lu_mtlx_nodedef *d) {
  free(d->name);
  free(d->node);
  free(d->key);
  free(d->version);
  free(d->target);
  free(d->inherit);
  for (size_t i = 0; i < d->ninputs; i++) port_free(&d->inputs[i]);
  for (size_t i = 0; i < d->noutputs; i++) port_free(&d->outputs[i]);
  free(d->inputs);
  free(d->outputs);
  memset(d, 0, sizeof(*d));
}

static int port_copy(lu_mtlx_port *dst, const lu_mtlx_port *src) {
  dst->has_value = src->has_value;
  dst->name = lu_mtlx_strdup(src->name);
  dst->type = lu_mtlx_strdup(src->type);
  dst->value = lu_mtlx_strdup(src->value);
  dst->default_geomprop = lu_mtlx_strdup(src->default_geomprop);
  return dst->name && dst->type && dst->value && dst->default_geomprop;
}

static int ports_copy(lu_mtlx_port **dst, size_t *ndst, const lu_mtlx_port *src, size_t n) {
  *dst = NULL;
  *ndst = 0;
  if (!n) return 1;
  *dst = (lu_mtlx_port *)calloc(n, sizeof(lu_mtlx_port));
  if (!*dst) return 0;
  *ndst = n;
  int ok = 1;
  for (size_t i = 0; i < n; i++) ok = port_copy(&(*dst)[i], &src[i]) && ok;
  return ok;
}

static int nodedef_copy(lu_mtlx_nodedef *dst, const lu_mtlx_nodedef *src) {
  memset(dst, 0, sizeof(*dst));
  dst->has_version = src->has_version;
  dst->is_default = src->is_default;
  dst->has_inherit = src->has_inherit;
  dst->name = lu_mtlx_strdup(src->name);
  dst->node = lu_mtlx_strdup(src->node);
  dst->key = lu_mtlx_strdup(src->key);
  dst->version = lu_mtlx_strdup(src->version);
  dst->target = lu_mtlx_strdup(src->target);
  dst->inherit = lu_mtlx_strdup(src->inherit);
  int ok = dst->name && dst->node && dst->key && dst->version && dst->target && dst->inherit;
  ok = ports_copy(&dst->inputs, &dst->ninputs, src->inputs, src->ninputs) && ok;
  ok = ports_copy(&dst->outputs, &dst->noutputs, src->outputs, src->noutputs) && ok;
  return ok;
}

static int reserve_defs(lu_mtlx_library *l, size_t need) {
  if (need <= l->capdefs) return 1;
  size_t cap = l->capdefs ? l->capdefs * 2 : 64;
  while (cap < need) cap *= 2;
  lu_mtlx_nodedef *v = (lu_mtlx_nodedef *)realloc(l->defs, cap * sizeof(*v));
  if (!v) return 0;
  l->defs = v;
  l->capdefs = cap;
  return 1;
}

static int push_geomprop(lu_mtlx_library *l, const lu_mtlx_elem *e) {
  if (l->ngeomprop == l->capgeomprop) {
    const size_t cap = l->capgeomprop ? l->capgeomprop * 2 : 8;
    lu_mtlx_elem *v = (lu_mtlx_elem *)realloc(l->geomprop_defs, cap * sizeof(*v));
    if (!v) return 0;
    l->geomprop_defs = v;
    l->capgeomprop = cap;
  }
  if (!lu_mtlx_elem_copy(&l->geomprop_defs[l->ngeomprop], e)) return 0;
  l->ngeomprop++;
  return 1;
}

/* ---- MtlxLibrary -------------------------------------------------------- */

void lu_mtlx_library_init(lu_mtlx_library *l) { memset(l, 0, sizeof(*l)); }

void lu_mtlx_library_free(lu_mtlx_library *l) {
  for (size_t i = 0; i < l->ndefs; i++) nodedef_free(&l->defs[i]);
  for (size_t i = 0; i < l->ngeomprop; i++) lu_mtlx_elem_free(&l->geomprop_defs[i]);
  free(l->defs);
  free(l->geomprop_defs);
  memset(l, 0, sizeof(*l));
}

/* One MtlxPort of AddNodeDef. */
static int port_from_elem(lu_mtlx_port *p, const lu_mtlx_elem *c) {
  const char *v = lu_mtlx_get_attr(c, "value");
  p->name = lu_mtlx_strdup(lu_mtlx_name(c));
  p->type = lu_mtlx_strdup(lu_mtlx_attr_or(c, "type", ""));
  p->default_geomprop = lu_mtlx_strdup(lu_mtlx_attr_or(c, "defaultgeomprop", ""));
  p->has_value = v != NULL;
  p->value = lu_mtlx_strdup(v ? v : "");
  return p->name && p->type && p->default_geomprop && p->value;
}

/* MtlxLibrary::AddNodeDef */
int lu_mtlx_library_add_nodedef(lu_mtlx_library *l, const lu_mtlx_elem *e,
                                const lu_mtlx_scope *outer) {
  if (!reserve_defs(l, l->ndefs + 1)) goto oom;
  lu_mtlx_nodedef *nd = &l->defs[l->ndefs];
  memset(nd, 0, sizeof(*nd));
  l->ndefs++;
  nd->name = lu_mtlx_strdup(lu_mtlx_name(e));
  nd->node = lu_mtlx_strdup(lu_mtlx_attr_or(e, "node", ""));
  const char *ns = NULL;
  if (namespace_at(e, outer, &ns)) {
    lu_mtlx_sb k;
    lu_mtlx_sb_init(&k);
    qualify_with(ns, nd->node ? nd->node : "", &k);
    nd->key = lu_mtlx_sb_take(&k);
  } else {
    nd->key = lu_mtlx_strdup(nd->node);
  }
  const char *v = lu_mtlx_get_attr(e, "version");
  nd->has_version = v != NULL;
  nd->version = lu_mtlx_strdup(v ? v : "");
  nd->is_default = strcmp(lu_mtlx_attr_or(e, "isdefaultversion", ""), "true") == 0;
  nd->target = lu_mtlx_strdup(lu_mtlx_attr_or(e, "target", ""));
  v = lu_mtlx_get_attr(e, "inherit");
  nd->has_inherit = v != NULL;
  nd->inherit = lu_mtlx_strdup(v ? v : "");
  if (!nd->name || !nd->node || !nd->key || !nd->version || !nd->target || !nd->inherit)
    goto oom;
  size_t ni = 0, no = 0;
  for (size_t i = 0; i < e->nchildren; i++) {
    if (!strcmp(e->children[i].tag, "input")) ni++;
    else if (!strcmp(e->children[i].tag, "output")) no++;
  }
  if (ni && !(nd->inputs = (lu_mtlx_port *)calloc(ni, sizeof(lu_mtlx_port)))) goto oom;
  if (no && !(nd->outputs = (lu_mtlx_port *)calloc(no, sizeof(lu_mtlx_port)))) goto oom;
  for (size_t i = 0; i < e->nchildren; i++) {
    const lu_mtlx_elem *c = &e->children[i];
    lu_mtlx_port *p;
    if (!strcmp(c->tag, "input")) p = &nd->inputs[nd->ninputs++];
    else if (!strcmp(c->tag, "output")) p = &nd->outputs[nd->noutputs++];
    else continue;
    if (!port_from_elem(p, c)) goto oom;
  }
  return 1;
oom:
  l->oom = 1;
  return 0;
}

/* MtlxLibrary::AddFrom */
int lu_mtlx_library_add_from(lu_mtlx_library *l, const lu_mtlx_elem *root) {
  lu_mtlx_scope outer;
  lu_mtlx_scope_init(&outer);
  int ok = lu_mtlx_scope_push(&outer, root);
  for (size_t i = 0; ok && i < root->nchildren; i++) {
    const lu_mtlx_elem *e = &root->children[i];
    if (!strcmp(e->tag, "nodedef")) ok = lu_mtlx_library_add_nodedef(l, e, &outer);
    else if (!strcmp(e->tag, "geompropdef")) ok = push_geomprop(l, e);
  }
  lu_mtlx_scope_free(&outer);
  if (!ok) l->oom = 1;
  return ok;
}

/* MtlxLibrary::AddLibrary */
int lu_mtlx_library_add_library(lu_mtlx_library *l, const lu_mtlx_library *other) {
  if (!reserve_defs(l, l->ndefs + other->ndefs)) goto oom;
  for (size_t i = 0; i < other->ndefs; i++) {
    const int ok = nodedef_copy(&l->defs[l->ndefs], &other->defs[i]);
    l->ndefs++;
    if (!ok) goto oom;
  }
  for (size_t i = 0; i < other->ngeomprop; i++)
    if (!push_geomprop(l, &other->geomprop_defs[i])) goto oom;
  return 1;
oom:
  l->oom = 1;
  return 0;
}

/* MtlxLibrary::Find */
const lu_mtlx_nodedef *lu_mtlx_library_find(const lu_mtlx_library *l, const char *name) {
  for (size_t i = 0; i < l->ndefs; i++)
    if (!strcmp(l->defs[i].name, name)) return &l->defs[i];
  return NULL;
}

/* MtlxLibrary::ActivePorts: inheritance chain (bounded by the library size),
 * overlaid from the most-base definition up. Entries are borrowed. */
static int active_ports(const lu_mtlx_library *l, const lu_mtlx_nodedef *def, int inputs,
                        lu_mtlx_ports *out) {
  out->v = NULL;
  out->n = 0;
  size_t nchain = 1, capchain = 8;
  const lu_mtlx_nodedef **chain =
      (const lu_mtlx_nodedef **)malloc(capchain * sizeof(*chain));
  if (!chain) return 0;
  const lu_mtlx_nodedef *cur = def;
  chain[0] = cur;
  for (size_t fuel = l->ndefs; fuel > 0 && cur->has_inherit; fuel--) {
    const lu_mtlx_nodedef *base = lu_mtlx_library_find(l, cur->inherit);
    if (!base) break;
    if (nchain == capchain) {
      capchain *= 2;
      const lu_mtlx_nodedef **c =
          (const lu_mtlx_nodedef **)realloc((void *)chain, capchain * sizeof(*chain));
      if (!c) {
        free((void *)chain);
        return 0;
      }
      chain = c;
    }
    chain[nchain++] = base;
    cur = base;
  }
  /* Upper bound on the merged size: sum of all levels. */
  size_t total = 0;
  for (size_t i = 0; i < nchain; i++) total += inputs ? chain[i]->ninputs : chain[i]->noutputs;
  lu_mtlx_port *acc = (lu_mtlx_port *)malloc((total ? total : 1) * sizeof(lu_mtlx_port));
  lu_mtlx_port *merged = (lu_mtlx_port *)malloc((total ? total : 1) * sizeof(lu_mtlx_port));
  if (!acc || !merged) {
    free(acc);
    free(merged);
    free((void *)chain);
    return 0;
  }
  const lu_mtlx_nodedef *b = chain[nchain - 1];
  size_t nacc = inputs ? b->ninputs : b->noutputs;
  if (nacc) memcpy(acc, inputs ? b->inputs : b->outputs, nacc * sizeof(lu_mtlx_port));
  for (size_t i = nchain - 1; i-- > 0;) {
    const lu_mtlx_port *own = inputs ? chain[i]->inputs : chain[i]->outputs;
    const size_t nown = inputs ? chain[i]->ninputs : chain[i]->noutputs;
    size_t nm = 0;
    for (size_t k = 0; k < nown; k++) merged[nm++] = own[k];
    for (size_t a = 0; a < nacc; a++) {
      int overridden = 0;
      for (size_t k = 0; k < nown; k++)
        if (!strcmp(own[k].name, acc[a].name)) {
          overridden = 1;
          break;
        }
      if (!overridden) merged[nm++] = acc[a];
    }
    lu_mtlx_port *t = acc;
    acc = merged;
    merged = t;
    nacc = nm;
  }
  free(merged);
  free((void *)chain);
  out->v = acc;
  out->n = nacc;
  return 1;
}

/* MtlxLibrary::ActiveInputs */
int lu_mtlx_library_active_inputs(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                                  lu_mtlx_ports *out) {
  return active_ports(l, d, 1, out);
}

/* MtlxLibrary::ActiveOutputs */
int lu_mtlx_library_active_outputs(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                                   lu_mtlx_ports *out) {
  return active_ports(l, d, 0, out);
}

/* MtlxLibrary::OutType, as a borrowed string (NULL on allocation failure). */
static const char *out_type_ref(const lu_mtlx_library *l, const lu_mtlx_nodedef *d) {
  lu_mtlx_ports outs;
  if (!active_ports(l, d, 0, &outs)) return NULL;
  const char *t = outs.n == 0 ? "color3" : outs.n == 1 ? outs.v[0].type : "multioutput";
  free(outs.v);
  return t;
}

/* MtlxLibrary::OutType (replaces out). */
int lu_mtlx_library_out_type(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                             lu_mtlx_sb *out) {
  const char *t = out_type_ref(l, d);
  if (!t) return 0;
  lu_mtlx_sb_set(out, t);
  return !out->oom;
}

/* CanonNat (anonymous, mtlx-resolve.cc): s[0..n) all digits, leading zeros
 * stripped (keeping one). Writes the canonical span. */
static int canon_nat(const char *s, const char **b, size_t *n) {
  const size_t len = strlen(s);
  if (!len) return 0;
  for (size_t i = 0; i < len; i++)
    if (s[i] < '0' || s[i] > '9') return 0;
  size_t k = 0;
  while (k + 1 < len && s[k] == '0') k++;
  *b = s + k;
  *n = len - k;
  return 1;
}

typedef struct nat_span { const char *p; size_t n; } nat_span;

/* ParseNodeVersion (anonymous): "major[.minor]" -> canonical spans. The
 * strings live in *parts (caller frees). */
static int parse_node_version(const char *s, lu_mtlx_strv *parts, nat_span *major,
                              nat_span *minor, int *oom) {
  if (!lu_mtlx_split_on('.', s, parts)) {
    *oom = 1;
    return 0;
  }
  if (parts->n == 1) {
    minor->p = "0";
    minor->n = 1;
    return canon_nat(parts->v[0], &major->p, &major->n);
  }
  if (parts->n == 2)
    return canon_nat(parts->v[0], &major->p, &major->n) &&
           canon_nat(parts->v[1], &minor->p, &minor->n);
  return 0;
}

static int span_eq(nat_span a, nat_span b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }

/* VersionCompatible (anonymous). */
static int version_compatible(const lu_mtlx_nodedef *nd, const char *req, int *oom) {
  if (!req) return nd->has_version ? nd->is_default : 1;
  if (!nd->has_version) return 0;
  lu_mtlx_strv pa, pb;
  lu_mtlx_strv_init(&pa);
  lu_mtlx_strv_init(&pb);
  nat_span a0, a1, b0, b1;
  int r;
  if (parse_node_version(req, &pa, &a0, &a1, oom) &&
      parse_node_version(nd->version, &pb, &b0, &b1, oom))
    r = span_eq(a0, b0) && span_eq(a1, b1);
  else
    r = strcmp(req, nd->version) == 0;
  lu_mtlx_strv_free(&pa);
  lu_mtlx_strv_free(&pb);
  return r;
}

/* Exact-signature test of MtlxLibrary::Candidates. -1 on allocation failure. */
static int exact_signature(const lu_mtlx_library *l, const lu_mtlx_nodedef *nd,
                           const lu_mtlx_elem *node) {
  lu_mtlx_ports decl;
  if (!active_ports(l, nd, 1, &decl)) return -1;
  int exact = 1;
  for (size_t c = 0; c < node->nchildren && exact; c++) {
    const lu_mtlx_elem *in = &node->children[c];
    if (strcmp(in->tag, "input") != 0) continue;
    const char *in_name = lu_mtlx_name(in);
    const char *in_type = lu_mtlx_attr_or(in, "type", "");
    int found = 0;
    for (size_t k = 0; k < decl.n; k++)
      if (!strcmp(decl.v[k].name, in_name) && !strcmp(decl.v[k].type, in_type)) {
        found = 1;
        break;
      }
    if (!found) exact = 0;
  }
  free(decl.v);
  return exact;
}

/* MtlxLibrary::Candidates: *first = first candidate, *count = number of
 * candidates found capped at `limit`. Returns 0 on allocation failure. */
static int candidates(const lu_mtlx_library *l, const lu_mtlx_elem *node,
                      const lu_mtlx_scope *outer, int exact_only, size_t limit,
                      const lu_mtlx_nodedef **first, size_t *count) {
  *first = NULL;
  *count = 0;
  const char *type = lu_mtlx_attr_or(node, "type", "");
  const char *version = lu_mtlx_get_attr(node, "version");
  const char *ns = NULL;
  lu_mtlx_sb q;
  lu_mtlx_sb_init(&q);
  const char *keys[2];
  size_t nkeys = 0;
  if (namespace_at(node, outer, &ns)) {
    qualify_with(ns, node->tag, &q);
    if (q.oom) return 0;
    keys[nkeys++] = lu_mtlx_sb_str(&q);
  }
  if (nkeys == 0 || strcmp(keys[0], node->tag) != 0) keys[nkeys++] = node->tag;
  int oom = 0;
  for (size_t k = 0; k < nkeys && *count < limit && !oom; k++) {
    for (size_t i = 0; i < l->ndefs && *count < limit && !oom; i++) {
      const lu_mtlx_nodedef *nd = &l->defs[i];
      if (strcmp(nd->key, keys[k]) != 0) continue;
      if (!version_compatible(nd, version, &oom)) continue;
      const char *ot = out_type_ref(l, nd);
      if (!ot) {
        oom = 1;
        break;
      }
      if (strcmp(ot, type) != 0) continue;
      if (exact_only) {
        const int ex = exact_signature(l, nd, node);
        if (ex < 0) oom = 1;
        if (ex <= 0) continue;
      }
      if (*count == 0) *first = nd;
      (*count)++;
    }
  }
  lu_mtlx_sb_free(&q);
  return !oom;
}

/* MtlxLibrary::FindNamed */
static const lu_mtlx_nodedef *find_named(const lu_mtlx_library *l, const lu_mtlx_elem *node,
                                         const lu_mtlx_scope *outer, const char *name) {
  const char *ns = NULL;
  if (namespace_at(node, outer, &ns)) {
    lu_mtlx_sb q;
    lu_mtlx_sb_init(&q);
    qualify_with(ns, name, &q);
    const lu_mtlx_nodedef *nd = q.oom ? NULL : lu_mtlx_library_find(l, lu_mtlx_sb_str(&q));
    lu_mtlx_sb_free(&q);
    if (nd) return nd;
  }
  return lu_mtlx_library_find(l, name);
}

/* MtlxLibrary::ResolveStrict */
const lu_mtlx_nodedef *lu_mtlx_library_resolve_strict(const lu_mtlx_library *l,
                                                      const lu_mtlx_elem *node,
                                                      const lu_mtlx_scope *outer) {
  const char *n = lu_mtlx_get_attr(node, "nodedef");
  if (n) return find_named(l, node, outer, n);
  const lu_mtlx_nodedef *first;
  size_t count;
  if (!candidates(l, node, outer, 1, 2, &first, &count)) return NULL;
  return count == 1 ? first : NULL;
}

/* MtlxLibrary::ResolveReference */
const lu_mtlx_nodedef *lu_mtlx_library_resolve_reference(const lu_mtlx_library *l,
                                                         const lu_mtlx_elem *node,
                                                         const lu_mtlx_scope *outer) {
  const char *n = lu_mtlx_get_attr(node, "nodedef");
  if (n) return find_named(l, node, outer, n);
  const lu_mtlx_nodedef *first;
  size_t count;
  if (!candidates(l, node, outer, 1, 1, &first, &count)) return NULL;
  return first;
}

/* MtlxLibrary::ResolveRough */
const lu_mtlx_nodedef *lu_mtlx_library_resolve_rough(const lu_mtlx_library *l,
                                                     const lu_mtlx_elem *node,
                                                     const lu_mtlx_scope *outer) {
  const lu_mtlx_nodedef *nd = lu_mtlx_library_resolve_reference(l, node, outer);
  if (nd) return nd;
  if (lu_mtlx_get_attr(node, "nodedef")) return NULL;
  size_t count;
  if (!candidates(l, node, outer, 0, 1, &nd, &count)) return NULL;
  return nd;
}

/* IsMtlxNodeElem */
int lu_mtlx_is_node_elem(const lu_mtlx_elem *e) {
  static const char *const kNonNode[] = {
      "nodedef",    "nodegraph",  "typedef",        "unittypedef",
      "unitdef",    "geompropdef", "implementation", "look",
      "lookgroup",  "collection", "geominfo",       "propertyset",
      "variantset", "targetdef",  "attributedef",   "backdrop",
      "xi:include", "comment",    "newline",        "input",
      "output",     "token",      "materialassign"};
  for (size_t i = 0; i < sizeof(kNonNode) / sizeof(kNonNode[0]); i++)
    if (!strcmp(e->tag, kNonNode[i])) return 0;
  return 1;
}

/* ---- ApplyMtlxNodeDefDefaults (mtlx-dom.cc) ------------------------------ */

/* Body of ApplyMtlxNodeDefDefaults(elem, library, outer) for one element. */
static int apply_defaults_one(lu_mtlx_elem *elem, const lu_mtlx_library *library,
                              const lu_mtlx_scope *outer) {
  if (!lu_mtlx_is_node_elem(elem)) return 1;
  const lu_mtlx_nodedef *def = lu_mtlx_library_resolve_reference(library, elem, outer);
  if (!def) return 1;
  lu_mtlx_ports ports;
  if (!lu_mtlx_library_active_inputs(library, def, &ports)) return 0;
  int ok = 1;
  for (size_t k = 0; ok && k < ports.n; k++) {
    const lu_mtlx_port *port = &ports.v[k];
    if (!port->has_value || port->default_geomprop[0]) continue;
    lu_mtlx_elem *input = NULL;
    for (size_t c = 0; c < elem->nchildren; c++) {
      lu_mtlx_elem *child = &elem->children[c];
      if (!strcmp(child->tag, "input") && !strcmp(lu_mtlx_name(child), port->name)) {
        input = child;
        break;
      }
    }
    if (!input) {
      lu_mtlx_elem fresh;
      lu_mtlx_elem_init(&fresh);
      ok = lu_mtlx_elem_set_tag(&fresh, "input") && lu_mtlx_attr_set(&fresh, "name", port->name) &&
           lu_mtlx_attr_set(&fresh, "type", port->type) &&
           lu_mtlx_attr_set(&fresh, "value", port->value);
      if (!ok) {
        lu_mtlx_elem_free(&fresh);
        break;
      }
      ok = lu_mtlx_child_push(elem, &fresh) != NULL;
      continue;
    }
    if (lu_mtlx_get_attr(input, "value") || lu_mtlx_get_attr(input, "nodename") ||
        lu_mtlx_get_attr(input, "nodegraph") || lu_mtlx_get_attr(input, "interfacename"))
      continue;
    if (!lu_mtlx_get_attr(input, "type")) ok = lu_mtlx_attr_set(input, "type", port->type);
    ok = ok && lu_mtlx_attr_set(input, "value", port->value);
  }
  free(ports.v);
  return ok;
}

typedef struct apply_frame {
  lu_mtlx_elem *e;
  size_t next;
} apply_frame;

/* ApplyMtlxNodeDefDefaults(MtlxElem *root): pre-order walk (explicit stack)
 * over root's descendants; each element is processed before its children,
 * scope = enclosing chain innermost first. */
int lu_mtlx_apply_nodedef_defaults(lu_mtlx_elem *root) {
  if (!root) return 1;
  lu_mtlx_library library;
  lu_mtlx_library_init(&library);
  int ok = lu_mtlx_library_add_from(&library, root) && lu_mtlx_library_add_stdlib(&library);
  lu_mtlx_scope scope;
  lu_mtlx_scope_init(&scope);
  apply_frame *stack = NULL;
  size_t n = 0, cap = 0;
  if (ok) {
    stack = (apply_frame *)malloc(16 * sizeof(apply_frame));
    cap = 16;
    ok = stack != NULL;
  }
  if (ok) {
    stack[n].e = root;
    stack[n].next = 0;
    n++;
  }
  while (ok && n > 0) {
    apply_frame *top = &stack[n - 1];
    if (top->next >= top->e->nchildren) {
      n--;
      continue;
    }
    lu_mtlx_elem *child = &top->e->children[top->next++];
    /* scope: stack elements innermost first */
    scope.n = 0;
    for (size_t i = n; ok && i-- > 0;) ok = lu_mtlx_scope_push(&scope, stack[i].e);
    if (!ok) break;
    if (!apply_defaults_one(child, &library, &scope)) {
      ok = 0;
      break;
    }
    if (n == cap) {
      apply_frame *s = (apply_frame *)realloc(stack, cap * 2 * sizeof(apply_frame));
      if (!s) {
        ok = 0;
        break;
      }
      stack = s;
      cap *= 2;
    }
    stack[n].e = child;
    stack[n].next = 0;
    n++;
  }
  free(stack);
  lu_mtlx_scope_free(&scope);
  lu_mtlx_library_free(&library);
  return ok;
}
