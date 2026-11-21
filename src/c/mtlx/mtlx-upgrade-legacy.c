/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX version upgrade ladder (C11 port of src/core/mtlx-dom.cc):
 * UpgradeMtlxVersionImpl, UpgradeMtlxElements, the 1.26 -> 1.34 and
 * 1.35 -> 1.36 steps, nodedef type -> output, legacy shader nodedef removal,
 * InheritMtlxAttributes, and helpers shared by the mtlx-upgrade-*.c TUs.
 * The C++ recursion is replaced by explicit stacks with the same visit order.
 */
#include "mtlx-upgrade.h"

#include <stdlib.h>
#include <string.h>

/* ---- shared helpers ------------------------------------------------------ */

/* IsMtlxNodeElem (mtlx-resolve.cc). */
int lu_mtlx_upg_is_node(const lu_mtlx_elem *e) {
  static const char *const kNonNode[] = {
      "nodedef",     "nodegraph",   "typedef",        "unittypedef",
      "unitdef",     "geompropdef", "implementation", "look",
      "lookgroup",   "collection",  "geominfo",       "propertyset",
      "variantset",  "targetdef",   "attributedef",   "backdrop",
      "xi:include",  "comment",     "newline",        "input",
      "output",      "token",       "materialassign"};
  for (size_t i = 0; i < sizeof(kNonNode) / sizeof(kNonNode[0]); i++)
    if (strcmp(e->tag, kNonNode[i]) == 0) return 0;
  return 1;
}

int lu_mtlx_upg_has_child_named(const lu_mtlx_elem *e, const char *name) {
  return lu_mtlx_child_named(e, name) != NULL;
}

lu_mtlx_elem *lu_mtlx_upg_find_input(lu_mtlx_elem *e, const char *name) {
  for (size_t i = 0; i < e->nchildren; i++) {
    lu_mtlx_elem *c = &e->children[i];
    if (LU_UPG_TAG_IS(c, "input") && LU_UPG_ATTR_IS(c, "name", name)) return c;
  }
  return NULL;
}

lu_mtlx_elem *lu_mtlx_upg_push_new(lu_mtlx_elem *parent, const char *tag) {
  lu_mtlx_elem tmp;
  lu_mtlx_elem_init(&tmp);
  if (!lu_mtlx_elem_set_tag(&tmp, tag)) return NULL;
  return lu_mtlx_child_push(parent, &tmp);
}

lu_mtlx_elem *lu_mtlx_upg_push_copy(lu_mtlx_elem *parent, const lu_mtlx_elem *src) {
  lu_mtlx_elem tmp;
  if (!lu_mtlx_elem_copy(&tmp, src)) return NULL;
  return lu_mtlx_child_push(parent, &tmp);
}

void lu_mtlx_upg_take_children(lu_mtlx_elem *e, lu_mtlx_elem *from) {
  while (e->nchildren) lu_mtlx_child_erase(e, e->nchildren - 1);
  free(e->children);
  e->children = from->children;
  e->nchildren = from->nchildren;
  e->capchildren = from->capchildren;
  from->children = NULL;
  from->nchildren = from->capchildren = 0;
  lu_mtlx_elem_free(from);
  lu_mtlx_elem_init(from);
}

/* IncrementMtlxName. */
int lu_mtlx_upg_increment_name(const char *name, lu_mtlx_sb *out) {
  const size_t end = strlen(name);
  size_t begin = end;
  while (begin > 0 && name[begin - 1] >= '0' && name[begin - 1] <= '9') --begin;
  lu_mtlx_sb_clear(out);
  if (begin == end) {
    lu_mtlx_sb_puts(out, name);
    lu_mtlx_sb_putc(out, '2');
    return !out->oom;
  }
  const unsigned long long number = strtoull(name + begin, NULL, 10);
  lu_mtlx_sb_put(out, name, begin);
  lu_mtlx_sb_putu(out, (uint64_t)(number + 1));
  return !out->oom;
}

/* GetMtlxShaderNodeDef. */
const lu_mtlx_elem *lu_mtlx_upg_shader_nodedef(const lu_mtlx_elem *root,
                                               const lu_mtlx_elem *shader_ref) {
  const char *nd = lu_mtlx_get_attr(shader_ref, "nodedef");
  if (nd) {
    for (size_t i = 0; i < root->nchildren; i++) {
      const lu_mtlx_elem *c = &root->children[i];
      if (LU_UPG_TAG_IS(c, "nodedef") && LU_UPG_NAME_IS(c, nd)) return c;
    }
    return NULL;
  }
  const char *node = lu_mtlx_get_attr(shader_ref, "node");
  if (!node) return NULL;
  const char *type = lu_mtlx_attr_or(shader_ref, "type", "");
  const char *target = lu_mtlx_attr_or(shader_ref, "target", "");
  const char *version = lu_mtlx_attr_or(shader_ref, "version", "");
  for (size_t i = 0; i < root->nchildren; i++) {
    const lu_mtlx_elem *c = &root->children[i];
    if (!LU_UPG_TAG_IS(c, "nodedef") || !LU_UPG_ATTR_IS(c, "node", node)) continue;
    const char *nt = lu_mtlx_attr_or(c, "target", "");
    const int target_ok = !target[0] || !nt[0] || strcmp(nt, target) == 0;
    const int version_ok = !version[0] || LU_UPG_ATTR_IS(c, "version", version);
    if (target_ok && version_ok && (!type[0] || LU_UPG_ATTR_IS(c, "type", type))) return c;
  }
  return NULL;
}

typedef struct upg_frame {
  lu_mtlx_elem *e;
  size_t i;
} upg_frame;

static int frames_push(upg_frame **v, size_t *n, size_t *cap, lu_mtlx_elem *e) {
  if (*n == *cap) {
    const size_t c2 = *cap ? *cap * 2 : 32;
    upg_frame *p = (upg_frame *)realloc(*v, c2 * sizeof(upg_frame));
    if (!p) return 0;
    *v = p;
    *cap = c2;
  }
  (*v)[*n].e = e;
  (*v)[*n].i = 0;
  (*n)++;
  return 1;
}

int lu_mtlx_upg_preorder(lu_mtlx_elem *root, lu_mtlx_upg_visit_fn visit, void *user) {
  if (!visit(root, NULL, user)) return 1;
  upg_frame *fr = NULL;
  size_t n = 0, cap = 0;
  if (!frames_push(&fr, &n, &cap, root)) return 0;
  while (n) {
    upg_frame *f = &fr[n - 1];
    if (f->i >= f->e->nchildren) {
      n--;
      continue;
    }
    lu_mtlx_elem *p = f->e;
    lu_mtlx_elem *c = &p->children[f->i++];
    if (visit(c, p, user) && !frames_push(&fr, &n, &cap, c)) {
      free(fr);
      return 0;
    }
  }
  free(fr);
  return 1;
}

/* ---- UpgradeMtlxElements ------------------------------------------------- */

typedef struct elem_ctx {
  lu_mtlx_elem *elem, *parent;
  lu_mtlx_elem gen; /* `generated`, held as the children of a scratch element */
  uint64_t minor;
  int ok;
} elem_ctx;

/* The `unique_name` lambda (suffix "_N" from 1, checked against parent). */
static void unique_name(elem_ctx *x, const char *base, lu_mtlx_sb *out) {
  lu_mtlx_sb_set(out, base);
  for (size_t suffix = 1;; ++suffix) {
    if (!(x->parent && lu_mtlx_upg_has_child_named(x->parent, lu_mtlx_sb_str(out)))) break;
    lu_mtlx_sb_set(out, base);
    lu_mtlx_sb_putc(out, '_');
    lu_mtlx_sb_putu(out, suffix);
    if (out->oom) break;
  }
  if (out->oom) x->ok = 0;
}

/* The `unique_numbered_name` lambda (suffix "N" from 2). */
static void unique_numbered_name(elem_ctx *x, const char *base, lu_mtlx_sb *out) {
  lu_mtlx_sb_set(out, base);
  for (size_t suffix = 2;; ++suffix) {
    if (!(x->parent && lu_mtlx_upg_has_child_named(x->parent, lu_mtlx_sb_str(out)))) break;
    lu_mtlx_sb_set(out, base);
    lu_mtlx_sb_putu(out, suffix);
    if (out->oom) break;
  }
  if (out->oom) x->ok = 0;
}

/* The `make_input` lambda, appended to `dst`. */
static int push_make_input(lu_mtlx_elem *dst, const char *name, const char *type,
                           const char *node_name, const char *output) {
  lu_mtlx_elem *in = lu_mtlx_upg_push_new(dst, "input");
  if (!in) return 0;
  int ok = lu_mtlx_attr_set(in, "name", name) && lu_mtlx_attr_set(in, "type", type) &&
           lu_mtlx_attr_set(in, "nodename", node_name);
  if (ok && output && output[0]) ok = lu_mtlx_attr_set(in, "output", output);
  return ok;
}

static int set3(lu_mtlx_elem *e, const char *name, const char *type, const char *value) {
  return lu_mtlx_attr_set(e, "name", name) && lu_mtlx_attr_set(e, "type", type) &&
         (!value || lu_mtlx_attr_set(e, "value", value));
}

/* std::remove_if over children with a predicate. */
typedef int (*child_pred_fn)(const lu_mtlx_elem *c);
static void erase_children_if(lu_mtlx_elem *e, child_pred_fn pred) {
  for (size_t i = e->nchildren; i-- > 0;)
    if (pred(&e->children[i])) lu_mtlx_child_erase(e, i);
}

static void rename_children(lu_mtlx_elem *e, const char *from, const char *to, elem_ctx *x) {
  for (size_t i = 0; i < e->nchildren; i++)
    if (LU_UPG_ATTR_IS(&e->children[i], "name", from) &&
        !lu_mtlx_attr_set(&e->children[i], "name", to))
      x->ok = 0;
}

static size_t type_count(const char *t) {
  if (!strcmp(t, "vector2") || !strcmp(t, "color2")) return 2;
  if (!strcmp(t, "vector3") || !strcmp(t, "color3")) return 3;
  if (!strcmp(t, "vector4") || !strcmp(t, "color4")) return 4;
  return 0;
}

static void set_tag_counted(lu_mtlx_elem *e, const char *base, size_t count, elem_ctx *x) {
  char buf[32];
  size_t n = strlen(base);
  memcpy(buf, base, n);
  buf[n] = (char)('0' + count);
  buf[n + 1] = '\0';
  if (!lu_mtlx_elem_set_tag(e, buf)) x->ok = 0;
}

/* UpgradeMtlxElements, the minor_version == 36 node renames. */
static void elements_36(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  const char *type = lu_mtlx_attr_or(elem, "type", "");
  if (LU_UPG_TAG_IS(elem, "invert")) {
    if (!strcmp(type, "matrix33") || !strcmp(type, "matrix44"))
      x->ok &= lu_mtlx_elem_set_tag(elem, "invertmatrix");
  } else if (LU_UPG_TAG_IS(elem, "rotate")) {
    if (!strcmp(type, "vector2")) x->ok &= lu_mtlx_elem_set_tag(elem, "rotate2d");
    else if (!strcmp(type, "vector3")) x->ok &= lu_mtlx_elem_set_tag(elem, "rotate3d");
  } else if (LU_UPG_TAG_IS(elem, "compare")) {
    x->ok &= lu_mtlx_elem_set_tag(elem, "ifgreatereq");
    for (size_t i = 0; i < elem->nchildren; i++) {
      lu_mtlx_elem *c = &elem->children[i];
      if (LU_UPG_ATTR_IS(c, "name", "intest")) x->ok &= lu_mtlx_attr_set(c, "name", "value1");
      else if (LU_UPG_ATTR_IS(c, "name", "cutoff")) x->ok &= lu_mtlx_attr_set(c, "name", "value2");
      else if (LU_UPG_ATTR_IS(c, "name", "in1"))
        x->ok &= lu_mtlx_attr_set(c, "name", "__upgrade_compare_in1");
    }
    rename_children(elem, "in2", "in1", x);
    rename_children(elem, "__upgrade_compare_in1", "in2", x);
  } else if (LU_UPG_TAG_IS(elem, "transformpoint") || LU_UPG_TAG_IS(elem, "transformvector") ||
             LU_UPG_TAG_IS(elem, "transformnormal")) {
    int has_matrix = 0;
    for (size_t i = 0; i < elem->nchildren; i++)
      if (LU_UPG_ATTR_IS(&elem->children[i], "name", "mat")) has_matrix = 1;
    if (has_matrix) x->ok &= lu_mtlx_elem_set_tag(elem, "transformmatrix");
  } else if (LU_UPG_TAG_IS(elem, "combine")) {
    const size_t count = type_count(type);
    if (count) set_tag_counted(elem, "combine", count, x);
  } else if (LU_UPG_TAG_IS(elem, "separate")) {
    size_t count = 0;
    for (size_t i = 0; i < elem->nchildren; i++) {
      const lu_mtlx_elem *c = &elem->children[i];
      if (!LU_UPG_TAG_IS(c, "input") || !LU_UPG_ATTR_IS(c, "name", "in")) continue;
      count = type_count(lu_mtlx_attr_or(c, "type", ""));
      break;
    }
    if (count) set_tag_counted(elem, "separate", count, x);
  } else if (LU_UPG_TAG_IS(elem, "geomattrvalue")) {
    x->ok &= lu_mtlx_elem_set_tag(elem, "geompropvalue");
    const char *attrname = lu_mtlx_get_attr(elem, "attrname");
    if (attrname) {
      char *value = lu_mtlx_strdup(attrname);
      if (!value) { x->ok = 0; return; }
      lu_mtlx_attr_erase(elem, "attrname");
      x->ok &= lu_mtlx_attr_set(elem, "geomprop", value);
      free(value);
    }
  }
}

static int is_interior_input(const lu_mtlx_elem *c) {
  return LU_UPG_TAG_IS(c, "input") && LU_UPG_ATTR_IS(c, "name", "interior");
}
static int is_reflectivity_or_edge(const lu_mtlx_elem *c) {
  return LU_UPG_TAG_IS(c, "input") &&
         (LU_UPG_ATTR_IS(c, "name", "reflectivity") || LU_UPG_ATTR_IS(c, "name", "edge_color"));
}

/* minor 37: dielectric_btdf scatter_mode. */
static void elements_37_btdf(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  erase_children_if(elem, is_interior_input);
  if (lu_mtlx_upg_find_input(elem, "scatter_mode")) return;
  lu_mtlx_elem *s = lu_mtlx_upg_push_new(elem, "input");
  if (!s || !set3(s, "scatter_mode", "string", "T")) x->ok = 0;
}

/* minor 37: conductor_brdf -> conductor_bsdf + artistic_ior. */
static void elements_37_conductor(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  x->ok &= lu_mtlx_elem_set_tag(elem, "conductor_bsdf");
  lu_mtlx_sb base, name;
  lu_mtlx_sb_init(&base);
  lu_mtlx_sb_init(&name);
  lu_mtlx_sb_set(&base, lu_mtlx_name(elem));
  lu_mtlx_sb_puts(&base, "__artistic_ior");
  unique_name(x, lu_mtlx_sb_str(&base), &name);
  const char *an = lu_mtlx_sb_str(&name);
  lu_mtlx_elem art;
  lu_mtlx_elem_init(&art);
  int ok = lu_mtlx_elem_set_tag(&art, "artistic_ior") && lu_mtlx_attr_set(&art, "name", an) &&
           lu_mtlx_attr_set(&art, "type", "multioutput");
  lu_mtlx_elem *o = ok ? lu_mtlx_upg_push_new(&art, "output") : NULL;
  ok = o && set3(o, "ior", "color3", NULL);
  o = ok ? lu_mtlx_upg_push_new(&art, "output") : NULL;
  ok = o && set3(o, "extinction", "color3", NULL);
  static const char *const kInputs[] = {"reflectivity", "edge_color"};
  for (size_t k = 0; ok && k < 2; k++) {
    const lu_mtlx_elem *src = lu_mtlx_upg_find_input(elem, kInputs[k]);
    if (src) ok = lu_mtlx_upg_push_copy(&art, src) != NULL;
  }
  erase_children_if(elem, is_reflectivity_or_edge);
  ok = ok && push_make_input(elem, "ior", "color3", an, "ior") &&
       push_make_input(elem, "extinction", "color3", an, "extinction") &&
       lu_mtlx_child_push(&x->gen, &art) != NULL;
  if (!ok) x->ok = 0;
  lu_mtlx_elem_free(&art);
  lu_mtlx_sb_free(&base);
  lu_mtlx_sb_free(&name);
}

/* minor 37: wrap a bound `base` input in a layer node. */
static void elements_37_layer(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  size_t bi = 0;
  while (bi < elem->nchildren &&
         !(LU_UPG_TAG_IS(&elem->children[bi], "input") &&
           LU_UPG_ATTR_IS(&elem->children[bi], "name", "base")))
    bi++;
  if (bi == elem->nchildren) return;
  const lu_mtlx_elem *base = &elem->children[bi];
  const int has_binding = lu_mtlx_get_attr(base, "nodename") ||
                          lu_mtlx_get_attr(base, "nodegraph") ||
                          lu_mtlx_get_attr(base, "interfacename");
  lu_mtlx_elem layer_base;
  if (!lu_mtlx_elem_copy(&layer_base, base)) { x->ok = 0; return; }
  int ok = lu_mtlx_attr_set(&layer_base, "name", "base") &&
           lu_mtlx_attr_set(&layer_base, "type", "BSDF");
  lu_mtlx_child_erase(elem, bi);
  if (ok && has_binding) {
    lu_mtlx_sb lname, tbase, top;
    lu_mtlx_sb_init(&lname);
    lu_mtlx_sb_init(&tbase);
    lu_mtlx_sb_init(&top);
    lu_mtlx_sb_set(&lname, lu_mtlx_name(elem));
    lu_mtlx_sb_set(&tbase, lu_mtlx_sb_str(&lname));
    lu_mtlx_sb_puts(&tbase, "__layer_top");
    unique_name(x, lu_mtlx_sb_str(&tbase), &top);
    ok = !lname.oom && lu_mtlx_attr_set(elem, "name", lu_mtlx_sb_str(&top));
    lu_mtlx_elem layer;
    lu_mtlx_elem_init(&layer);
    ok = ok && lu_mtlx_elem_set_tag(&layer, "layer") &&
         set3(&layer, lu_mtlx_sb_str(&lname), "BSDF", NULL) &&
         push_make_input(&layer, "top", "BSDF", lu_mtlx_sb_str(&top), NULL) &&
         lu_mtlx_child_push(&layer, &layer_base) != NULL &&
         lu_mtlx_child_push(&x->gen, &layer) != NULL;
    lu_mtlx_elem_free(&layer);
    lu_mtlx_sb_free(&lname);
    lu_mtlx_sb_free(&tbase);
    lu_mtlx_sb_free(&top);
  }
  if (!ok) x->ok = 0;
  lu_mtlx_elem_free(&layer_base);
}

/* UpgradeMtlxElements, the minor_version == 37 node step. */
static void elements_37(elem_ctx *x) {
  static const char *const kRenames[][2] = {
      {"diffuse_brdf", "oren_nayar_diffuse_bsdf"},
      {"burley_diffuse_brdf", "burley_diffuse_bsdf"},
      {"diffuse_btdf", "translucent_bsdf"},
      {"subsurface_brdf", "subsurface_bsdf"},
      {"sheen_brdf", "sheen_bsdf"},
      {"generalized_schlick_brdf", "generalized_schlick_bsdf"},
      {"thin_film_brdf", "thin_film_bsdf"},
      {"dielectric_brdf", "dielectric_bsdf"},
      {"dielectric_btdf", "dielectric_bsdf"}};
  lu_mtlx_elem *elem = x->elem;
  for (size_t i = 0; i < elem->nchildren; i++)
    if (LU_UPG_TAG_IS(&elem->children[i], "parameter"))
      x->ok &= lu_mtlx_elem_set_tag(&elem->children[i], "input");
  const int was_dielectric_btdf = LU_UPG_TAG_IS(elem, "dielectric_btdf");
  const int needs_layer = LU_UPG_TAG_IS(elem, "dielectric_brdf") ||
                          LU_UPG_TAG_IS(elem, "generalized_schlick_brdf") ||
                          LU_UPG_TAG_IS(elem, "sheen_brdf") || LU_UPG_TAG_IS(elem, "thin_film_brdf");
  const int was_conductor = LU_UPG_TAG_IS(elem, "conductor_brdf");
  for (size_t k = 0; k < sizeof(kRenames) / sizeof(kRenames[0]); k++) {
    if (LU_UPG_TAG_IS(elem, kRenames[k][0])) {
      x->ok &= lu_mtlx_elem_set_tag(elem, kRenames[k][1]);
      break;
    }
  }
  if (was_dielectric_btdf) elements_37_btdf(x);
  if (was_conductor) elements_37_conductor(x);
  if (needs_layer) elements_37_layer(x);
}

/* atan2 input renames (minor 37 and 38). */
static void elements_atan2(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  if (x->minor == 37) {
    lu_mtlx_elem *in1 = NULL, *in2 = NULL;
    for (size_t i = 0; i < elem->nchildren; i++) {
      lu_mtlx_elem *c = &elem->children[i];
      if (!LU_UPG_TAG_IS(c, "input")) continue;
      if (LU_UPG_ATTR_IS(c, "name", "in1")) in1 = c;
      else if (LU_UPG_ATTR_IS(c, "name", "in2")) in2 = c;
    }
    if (in1 && in2) {
      x->ok &= lu_mtlx_attr_set(in1, "name", "");
      x->ok &= lu_mtlx_attr_set(in2, "name", "in1");
      x->ok &= lu_mtlx_attr_set(in1, "name", "in2");
    } else if (in1) {
      x->ok &= lu_mtlx_attr_set(in1, "name", "in2");
    } else if (in2) {
      x->ok &= lu_mtlx_attr_set(in2, "name", "in1");
    }
  } else if (x->minor == 38) {
    for (size_t i = 0; i < elem->nchildren; i++) {
      lu_mtlx_elem *c = &elem->children[i];
      if (!LU_UPG_TAG_IS(c, "input")) continue;
      if (LU_UPG_ATTR_IS(c, "name", "in1")) x->ok &= lu_mtlx_attr_set(c, "name", "iny");
      else if (LU_UPG_ATTR_IS(c, "name", "in2")) x->ok &= lu_mtlx_attr_set(c, "name", "inx");
    }
  }
}

/* minor 38: subsurface_bsdf vector3 radius -> convert node. */
static void elements_38_subsurface(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  lu_mtlx_elem *radius = lu_mtlx_upg_find_input(elem, "radius");
  if (!radius || !LU_UPG_ATTR_IS(radius, "type", "vector3")) return;
  lu_mtlx_sb cname;
  lu_mtlx_sb_init(&cname);
  unique_name(x, "convert", &cname);
  lu_mtlx_elem conv;
  lu_mtlx_elem_init(&conv);
  int ok = lu_mtlx_elem_set_tag(&conv, "convert") &&
           set3(&conv, lu_mtlx_sb_str(&cname), "color3", NULL);
  lu_mtlx_elem *ci = ok ? lu_mtlx_upg_push_copy(&conv, radius) : NULL;
  ok = ci && lu_mtlx_attr_set(ci, "name", "in") && lu_mtlx_attr_set(ci, "type", "vector3") &&
       lu_mtlx_child_push(&x->gen, &conv) != NULL;
  static const char *const kErase[] = {"value", "nodename", "nodegraph",
                                       "output", "interfacename", "channels"};
  for (size_t k = 0; k < 6; k++) lu_mtlx_attr_erase(radius, kErase[k]);
  ok = ok && lu_mtlx_attr_set(radius, "type", "color3") &&
       lu_mtlx_attr_set(radius, "nodename", lu_mtlx_sb_str(&cname));
  if (!ok) x->ok = 0;
  lu_mtlx_elem_free(&conv);
  lu_mtlx_sb_free(&cname);
}

/* ParseMtlxValue("integer"/"float", text) >= 5, with the grammar of
 * ParseIntToken / ParseFloatToken on the trimmed text (local helper: the
 * value parsers of mtlx-value.c are owned by another TU). */
static int all_digits(const char *s, size_t b, size_t e) {
  if (b >= e) return 0;
  for (size_t i = b; i < e; i++)
    if (s[i] < '0' || s[i] > '9') return 0;
  return 1;
}

static int int_token_ge5(const char *s) {
  const size_t n = strlen(s);
  size_t b = 0;
  int neg = 0;
  if (n && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; b = 1; }
  if (!all_digits(s, b, n)) return 0;
  int64_t acc = 0;
  const int64_t kMin = INT64_MIN;
  for (size_t i = b; i < n; i++) {
    const int64_t d = s[i] - '0';
    if (acc < (kMin + d) / 10) return 0;
    acc = acc * 10 - d;
  }
  if (!neg) {
    if (acc == kMin) return 0;
    acc = -acc;
  }
  return acc >= 5;
}

static int float_token_ge5(const char *s) {
  const size_t n = strlen(s);
  size_t b = 0;
  int neg = 0;
  if (n && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; b = 1; }
  size_t mant_end = b;
  while (mant_end < n && s[mant_end] != 'e' && s[mant_end] != 'E') mant_end++;
  size_t dot = b;
  while (dot < mant_end && s[dot] != '.') dot++;
  const size_t fp_b = dot < mant_end ? dot + 1 : mant_end;
  /* digits = ip + fp must be non-empty and all digits */
  if (dot - b + mant_end - fp_b == 0) return 0;
  if (dot > b && !all_digits(s, b, dot)) return 0;
  if (mant_end > fp_b && !all_digits(s, fp_b, mant_end)) return 0;
  lu_mtlx_sb norm;
  lu_mtlx_sb_init(&norm);
  if (neg) lu_mtlx_sb_putc(&norm, '-');
  if (dot > b) lu_mtlx_sb_put(&norm, s + b, dot - b); else lu_mtlx_sb_putc(&norm, '0');
  lu_mtlx_sb_putc(&norm, '.');
  if (mant_end > fp_b) lu_mtlx_sb_put(&norm, s + fp_b, mant_end - fp_b);
  else lu_mtlx_sb_putc(&norm, '0');
  if (mant_end < n) {
    size_t eb = mant_end + 1;
    int eneg = 0;
    if (eb < n && (s[eb] == '-' || s[eb] == '+')) { eneg = s[eb] == '-'; eb++; }
    if (!all_digits(s, eb, n)) { lu_mtlx_sb_free(&norm); return 0; }
    lu_mtlx_sb_putc(&norm, 'e');
    if (eneg) lu_mtlx_sb_putc(&norm, '-');
    lu_mtlx_sb_put(&norm, s + eb, n - eb);
  }
  const double v = strtod(lu_mtlx_sb_str(&norm), NULL);
  lu_mtlx_sb_free(&norm);
  return v >= 5.0;
}

static int which_ge5(const char *type, const char *value) {
  lu_mtlx_sb t;
  lu_mtlx_sb_init(&t);
  lu_mtlx_trim(value, &t);
  int r = 0;
  if (!strcmp(type, "integer")) r = int_token_ge5(lu_mtlx_sb_str(&t));
  else if (!strcmp(type, "float")) r = float_token_ge5(lu_mtlx_sb_str(&t));
  lu_mtlx_sb_free(&t);
  return r;
}

/* minor 38: switch `which` >= 5 resets to 0. */
static void elements_38_switch(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  for (size_t i = 0; i < elem->nchildren; i++) {
    lu_mtlx_elem *c = &elem->children[i];
    if (!LU_UPG_TAG_IS(c, "input") || !LU_UPG_ATTR_IS(c, "name", "which")) continue;
    if (which_ge5(lu_mtlx_attr_or(c, "type", ""), lu_mtlx_attr_or(c, "value", "")))
      x->ok &= lu_mtlx_attr_set(c, "value", "0");
  }
}

static int is_input(const lu_mtlx_elem *c) { return LU_UPG_TAG_IS(c, "input"); }
static int is_space_input(const lu_mtlx_elem *c) {
  return LU_UPG_TAG_IS(c, "input") && LU_UPG_ATTR_IS(c, "name", "space");
}

/* minor 38: object-space normalmap -> multiply/subtract/normalize. */
static void normalmap_object(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  lu_mtlx_sb mname, sname;
  lu_mtlx_sb_init(&mname);
  lu_mtlx_sb_init(&sname);
  unique_name(x, "multiply", &mname);
  unique_name(x, "subtract", &sname);
  lu_mtlx_elem mul, sub;
  lu_mtlx_elem_init(&mul);
  lu_mtlx_elem_init(&sub);
  int ok = lu_mtlx_elem_set_tag(&mul, "multiply") &&
           set3(&mul, lu_mtlx_sb_str(&mname), "vector3", NULL);
  const lu_mtlx_elem *in = lu_mtlx_upg_find_input(elem, "in");
  if (ok && in) {
    lu_mtlx_elem *mi = lu_mtlx_upg_push_copy(&mul, in);
    ok = mi && lu_mtlx_attr_set(mi, "name", "in1");
  }
  lu_mtlx_elem *f = ok ? lu_mtlx_upg_push_new(&mul, "input") : NULL;
  ok = f && set3(f, "in2", "float", "2");
  ok = ok && lu_mtlx_elem_set_tag(&sub, "subtract") &&
       set3(&sub, lu_mtlx_sb_str(&sname), "vector3", NULL) &&
       push_make_input(&sub, "in1", "vector3", lu_mtlx_sb_str(&mname), NULL);
  f = ok ? lu_mtlx_upg_push_new(&sub, "input") : NULL;
  ok = f && set3(f, "in2", "float", "1");
  erase_children_if(elem, is_input);
  ok = ok && lu_mtlx_elem_set_tag(elem, "normalize");
  if (ok && lu_mtlx_get_attr(elem, "nodedef"))
    ok = lu_mtlx_attr_set(elem, "nodedef", "ND_normalize_vector3");
  ok = ok && push_make_input(elem, "in", "vector3", lu_mtlx_sb_str(&sname), NULL) &&
       lu_mtlx_child_push(&x->gen, &mul) && lu_mtlx_child_push(&x->gen, &sub);
  if (!ok) x->ok = 0;
  lu_mtlx_elem_free(&mul);
  lu_mtlx_elem_free(&sub);
  lu_mtlx_sb_free(&mname);
  lu_mtlx_sb_free(&sname);
}

/* The `copy_or_geom` lambda of the tangent-space normalmap branch. */
static int copy_or_geom(elem_ctx *x, lu_mtlx_elem *cross, lu_mtlx_elem *geom_nodes,
                        const char *source_name, const char *dest_name,
                        const char *geom_category) {
  const lu_mtlx_elem *src = lu_mtlx_upg_find_input(x->elem, source_name);
  if (src) {
    lu_mtlx_elem *c = lu_mtlx_upg_push_copy(cross, src);
    return c && lu_mtlx_attr_set(c, "name", dest_name);
  }
  lu_mtlx_sb base, gname;
  lu_mtlx_sb_init(&base);
  lu_mtlx_sb_init(&gname);
  lu_mtlx_sb_set(&base, "normalmap_");
  lu_mtlx_sb_puts(&base, geom_category);
  unique_numbered_name(x, lu_mtlx_sb_str(&base), &gname);
  lu_mtlx_elem *g = lu_mtlx_upg_push_new(geom_nodes, geom_category);
  int ok = g && set3(g, lu_mtlx_sb_str(&gname), "vector3", NULL);
  lu_mtlx_elem *s = ok ? lu_mtlx_upg_push_new(g, "input") : NULL;
  ok = s && set3(s, "space", "string", "world") &&
       push_make_input(cross, dest_name, "vector3", lu_mtlx_sb_str(&gname), NULL);
  ok = ok && !base.oom;
  lu_mtlx_sb_free(&base);
  lu_mtlx_sb_free(&gname);
  return ok;
}

/* minor 38: tangent-space normalmap -> explicit bitangent. */
static void normalmap_tangent(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  erase_children_if(elem, is_space_input);
  const int has_normal = lu_mtlx_upg_find_input(elem, "normal") != NULL;
  const int has_tangent = lu_mtlx_upg_find_input(elem, "tangent") != NULL;
  const int has_bitangent = lu_mtlx_upg_find_input(elem, "bitangent") != NULL;
  int ok = 1;
  if ((has_normal || has_tangent) && !has_bitangent) {
    lu_mtlx_sb cname, nname;
    lu_mtlx_sb_init(&cname);
    lu_mtlx_sb_init(&nname);
    unique_numbered_name(x, "normalmap_cross", &cname);
    unique_numbered_name(x, "normalmap_cross_norm", &nname);
    lu_mtlx_elem cross, geoms, norm;
    lu_mtlx_elem_init(&cross);
    lu_mtlx_elem_init(&geoms);
    lu_mtlx_elem_init(&norm);
    ok = lu_mtlx_elem_set_tag(&cross, "crossproduct") &&
         set3(&cross, lu_mtlx_sb_str(&cname), "vector3", NULL) &&
         copy_or_geom(x, &cross, &geoms, "normal", "in1", "normal") &&
         copy_or_geom(x, &cross, &geoms, "tangent", "in2", "tangent") &&
         lu_mtlx_child_push(&x->gen, &cross);
    for (size_t i = 0; ok && i < geoms.nchildren; i++)
      ok = lu_mtlx_child_push(&x->gen, &geoms.children[i]) != NULL;
    ok = ok && lu_mtlx_elem_set_tag(&norm, "normalize") &&
         set3(&norm, lu_mtlx_sb_str(&nname), "vector3", NULL) &&
         push_make_input(&norm, "in", "vector3", lu_mtlx_sb_str(&cname), NULL) &&
         lu_mtlx_child_push(&x->gen, &norm) &&
         push_make_input(elem, "bitangent", "vector3", lu_mtlx_sb_str(&nname), NULL);
    lu_mtlx_elem_free(&cross);
    lu_mtlx_elem_free(&geoms);
    lu_mtlx_elem_free(&norm);
    lu_mtlx_sb_free(&cname);
    lu_mtlx_sb_free(&nname);
  }
  if (ok && LU_UPG_ATTR_IS(elem, "nodedef", "ND_normalmap"))
    ok = lu_mtlx_attr_set(elem, "nodedef", "ND_normalmap_float");
  if (!ok) x->ok = 0;
}

/* UpgradeMtlxElements, the minor_version == 38 node step. */
static void elements_38(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  for (size_t i = 0; i < elem->nchildren; i++) {
    lu_mtlx_elem *c = &elem->children[i];
    if (LU_UPG_TAG_IS(c, "input") && lu_mtlx_get_attr(c, "value") &&
        (lu_mtlx_get_attr(c, "nodename") || lu_mtlx_get_attr(c, "nodegraph") ||
         lu_mtlx_get_attr(c, "interfacename") || lu_mtlx_get_attr(c, "output")))
      lu_mtlx_attr_erase(c, "value");
  }
  if (LU_UPG_TAG_IS(elem, "subsurface_bsdf")) {
    elements_38_subsurface(x);
  } else if (LU_UPG_TAG_IS(elem, "switch")) {
    elements_38_switch(x);
  } else if (LU_UPG_TAG_IS(elem, "normalmap")) {
    int object_space = 0;
    for (size_t i = 0; i < elem->nchildren; i++) {
      const lu_mtlx_elem *c = &elem->children[i];
      if (LU_UPG_TAG_IS(c, "input") && LU_UPG_ATTR_IS(c, "name", "space") &&
          LU_UPG_ATTR_IS(c, "value", "object")) {
        object_space = 1;
        break;
      }
    }
    if (object_space) normalmap_object(x);
    else normalmap_tangent(x);
  }
}

/* Body of UpgradeMtlxElements after its children were upgraded. */
static void elements_post(elem_ctx *x) {
  lu_mtlx_elem *elem = x->elem;
  const int is_node = lu_mtlx_upg_is_node(elem);
  if (x->minor == 36 && is_node) elements_36(x);
  if (x->minor == 37 && lu_mtlx_upg_is_node(elem)) elements_37(x);
  if (lu_mtlx_upg_is_node(elem) && LU_UPG_TAG_IS(elem, "atan2")) elements_atan2(x);
  if (x->minor == 38 && lu_mtlx_upg_is_node(elem)) elements_38(x);
  lu_mtlx_elem *owner = x->parent ? x->parent : elem;
  for (size_t i = 0; i < x->gen.nchildren; i++)
    if (!lu_mtlx_child_push(owner, &x->gen.children[i])) x->ok = 0;
  lu_mtlx_elem_free(&x->gen);
}

typedef struct post_frame {
  lu_mtlx_elem *e, *parent;
  size_t i, original_child_count;
} post_frame;

/* UpgradeMtlxElements(root, minor): post-order over the first
 * original_child_count children of every element; generated elements are
 * appended to the parent (to the element itself at the root). */
static int upgrade_elements(lu_mtlx_elem *root, uint64_t minor) {
  post_frame *fr = NULL;
  size_t n = 0, cap = 0;
  int ok = 1;
  lu_mtlx_elem *next = root, *next_parent = NULL;
  for (;;) {
    if (next) {
      if (minor == 37 && LU_UPG_ATTR_IS(next, "type", "color2"))
        ok &= lu_mtlx_attr_set(next, "type", "vector2");
      if (n == cap) {
        const size_t c2 = cap ? cap * 2 : 32;
        post_frame *p = (post_frame *)realloc(fr, c2 * sizeof(post_frame));
        if (!p) { ok = 0; break; }
        fr = p;
        cap = c2;
      }
      fr[n].e = next;
      fr[n].parent = next_parent;
      fr[n].i = 0;
      fr[n].original_child_count = next->nchildren;
      n++;
      next = NULL;
    }
    if (!n) break;
    post_frame *f = &fr[n - 1];
    if (f->i < f->original_child_count) {
      next_parent = f->e;
      next = &f->e->children[f->i++];
      continue;
    }
    elem_ctx x;
    x.elem = f->e;
    x.parent = f->parent;
    x.minor = minor;
    x.ok = 1;
    lu_mtlx_elem_init(&x.gen);
    elements_post(&x);
    ok &= x.ok;
    n--;
  }
  free(fr);
  return ok;
}

/* ---- 1.26 -> 1.34 --------------------------------------------------------- */

/* Upgrade26to34Elems: one element (the loop over its children). */
static int up26_visit(lu_mtlx_elem *elem, lu_mtlx_elem *parent, void *user) {
  int *ok = (int *)user;
  for (size_t i = 0; i < elem->nchildren;) {
    lu_mtlx_elem *child = &elem->children[i];
    int erase = 0;
    if (LU_UPG_TAG_IS(child, "opgraph")) {
      *ok &= lu_mtlx_elem_set_tag(child, "nodegraph");
    } else if (LU_UPG_TAG_IS(child, "shader")) {
      *ok &= lu_mtlx_elem_set_tag(child, "nodedef");
      if (lu_mtlx_get_attr(child, "shadertype"))
        *ok &= lu_mtlx_attr_set(child, "type", "surfaceshader");
      const char *sp = lu_mtlx_get_attr(child, "shaderprogram");
      if (sp) *ok &= lu_mtlx_attr_set(child, "node", sp);
    } else if (LU_UPG_TAG_IS(child, "shaderref")) {
      if (lu_mtlx_get_attr(child, "shadertype")) {
        *ok &= lu_mtlx_attr_set(child, "type", "surfaceshader");
        lu_mtlx_attr_erase(child, "shadertype");
      }
    } else if (LU_UPG_TAG_IS(child, "parameter") && LU_UPG_ATTR_IS(child, "type", "opgraphnode")) {
      if (!LU_UPG_TAG_IS(elem, "output") && !LU_UPG_TAG_IS(elem, "materialx") &&
          lu_mtlx_upg_is_node(elem)) {
        char *value = lu_mtlx_strdup(lu_mtlx_attr_or(child, "value", ""));
        if (!value) { *ok = 0; return 1; }
        *ok &= lu_mtlx_elem_set_tag(child, "input");
        *ok &= lu_mtlx_attr_set(child, "nodename", value);
        lu_mtlx_attr_erase(child, "value");
        const char *type = "color3";
        if (parent) {
          for (size_t k = 0; k < parent->nchildren; k++) {
            const lu_mtlx_elem *sib = &parent->children[k];
            if (LU_UPG_NAME_IS(sib, value) && lu_mtlx_upg_is_node(sib)) {
              type = lu_mtlx_attr_or(sib, "type", "");
              break;
            }
          }
        }
        *ok &= lu_mtlx_attr_set(child, "type", type);
        free(value);
      } else if (LU_UPG_TAG_IS(elem, "output")) {
        if (LU_UPG_NAME_IS(child, "in"))
          *ok &= lu_mtlx_attr_set(elem, "nodename", lu_mtlx_attr_or(child, "value", ""));
        erase = 1;
      }
    }
    /* Erasing in place (instead of collecting indices and erasing in reverse)
     * gives the same result: later children do not depend on earlier ones. */
    if (erase) lu_mtlx_child_erase(elem, i);
    else i++;
  }
  return 1;
}

/* Upgrade26to34: shaderref nodedef names. */
static int up26_shaderref_names(lu_mtlx_elem *root) {
  int ok = 1;
  for (size_t m = 0; m < root->nchildren; m++) {
    lu_mtlx_elem *mat = &root->children[m];
    if (!LU_UPG_TAG_IS(mat, "material")) continue;
    for (size_t s = 0; s < mat->nchildren; s++) {
      lu_mtlx_elem *sr = &mat->children[s];
      if (!LU_UPG_TAG_IS(sr, "shaderref") || lu_mtlx_upg_shader_nodedef(root, sr)) continue;
      for (size_t k = 0; k < root->nchildren; k++) {
        const lu_mtlx_elem *nd = &root->children[k];
        if (LU_UPG_TAG_IS(nd, "nodedef") && !strcmp(lu_mtlx_name(nd), lu_mtlx_name(sr))) {
          ok &= lu_mtlx_attr_set(sr, "nodedef", lu_mtlx_name(nd));
          ok &= lu_mtlx_attr_set(sr, "node", lu_mtlx_attr_or(nd, "node", ""));
          break;
        }
      }
    }
  }
  return ok;
}

/* Upgrade26to34: one nodedef input's connection -> bindinputs. */
static int up26_bind_input(lu_mtlx_elem *root, const lu_mtlx_elem *nd, lu_mtlx_elem *input) {
  int ok = 1;
  for (size_t m = 0; m < root->nchildren; m++) {
    lu_mtlx_elem *mat = &root->children[m];
    if (!LU_UPG_TAG_IS(mat, "material")) continue;
    for (size_t s = 0; s < mat->nchildren; s++) {
      lu_mtlx_elem *sr = &mat->children[s];
      if (!LU_UPG_TAG_IS(sr, "shaderref")) continue;
      if (lu_mtlx_upg_shader_nodedef(root, sr) != nd ||
          lu_mtlx_child_named(sr, lu_mtlx_name(input)))
        continue;
      lu_mtlx_elem *b = lu_mtlx_upg_push_new(sr, "bindinput");
      ok = b && lu_mtlx_attr_set(b, "name", lu_mtlx_name(input)) &&
           lu_mtlx_attr_set(b, "type", lu_mtlx_attr_or(input, "type", "")) &&
           lu_mtlx_attr_set(b, "nodegraph", lu_mtlx_attr_or(input, "opgraph", "")) &&
           lu_mtlx_attr_set(b, "output", lu_mtlx_attr_or(input, "graphoutput", ""));
      if (!ok) return 0;
    }
  }
  lu_mtlx_attr_erase(input, "opgraph");
  lu_mtlx_attr_erase(input, "graphoutput");
  return ok;
}

static int str_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Upgrade26to34: geomattr -> geomprop and the udimset geominfo. */
static int up26_udims(lu_mtlx_elem *root) {
  int has_udim = 0, has_udimset = 0, ok = 1;
  lu_mtlx_strv udims;
  lu_mtlx_strv_init(&udims);
  for (size_t g = 0; g < root->nchildren; g++) {
    lu_mtlx_elem *gi = &root->children[g];
    if (!LU_UPG_TAG_IS(gi, "geominfo")) continue;
    for (size_t k = 0; k < gi->nchildren; k++) {
      lu_mtlx_elem *ga = &gi->children[k];
      if (LU_UPG_TAG_IS(ga, "geomattr")) ok &= lu_mtlx_elem_set_tag(ga, "geomprop");
      if (LU_UPG_TAG_IS(ga, "geomprop") && LU_UPG_NAME_IS(ga, "udim")) {
        has_udim = 1;
        const char *v = lu_mtlx_attr_or(ga, "value", "");
        if (!lu_mtlx_strv_contains(&udims, v)) ok &= lu_mtlx_strv_push(&udims, v);
      }
      if (LU_UPG_TAG_IS(ga, "geomprop") && LU_UPG_NAME_IS(ga, "udimset")) has_udimset = 1;
    }
  }
  if (has_udim && !has_udimset && ok) {
    qsort(udims.v, udims.n, sizeof(char *), str_cmp); /* std::set order */
    lu_mtlx_sb joined, name;
    lu_mtlx_sb_init(&joined);
    lu_mtlx_sb_init(&name);
    for (size_t i = 0; i < udims.n; i++) {
      if (joined.n) lu_mtlx_sb_puts(&joined, ", ");
      lu_mtlx_sb_puts(&joined, udims.v[i]);
    }
    lu_mtlx_sb_set(&name, "geominfo1");
    for (int n = 2; lu_mtlx_child_named(root, lu_mtlx_sb_str(&name)); ++n) {
      lu_mtlx_sb_set(&name, "geominfo");
      lu_mtlx_sb_puti(&name, n);
    }
    lu_mtlx_elem info;
    lu_mtlx_elem_init(&info);
    ok = !joined.oom && !name.oom && lu_mtlx_elem_set_tag(&info, "geominfo") &&
         lu_mtlx_attr_set(&info, "name", lu_mtlx_sb_str(&name));
    lu_mtlx_elem *p = ok ? lu_mtlx_upg_push_new(&info, "geomprop") : NULL;
    ok = p && set3(p, "udimset", "stringarray", lu_mtlx_sb_str(&joined)) &&
         lu_mtlx_child_push(root, &info);
    lu_mtlx_elem_free(&info);
    lu_mtlx_sb_free(&joined);
    lu_mtlx_sb_free(&name);
  }
  lu_mtlx_strv_free(&udims);
  return ok;
}

/* UpgradeMtlx26to34. */
static int upgrade_26_to_34(lu_mtlx_elem *root) {
  if (!LU_UPG_TAG_IS(root, "materialx")) return 1;
  int ok = 1;
  if (!lu_mtlx_upg_preorder(root, up26_visit, &ok)) return 0;
  ok &= up26_shaderref_names(root);
  for (size_t k = 0; ok && k < root->nchildren; k++) {
    lu_mtlx_elem *nd = &root->children[k];
    if (!LU_UPG_TAG_IS(nd, "nodedef")) continue;
    for (size_t i = 0; ok && i < nd->nchildren; i++) {
      lu_mtlx_elem *input = &nd->children[i];
      if (!LU_UPG_TAG_IS(input, "input") || !lu_mtlx_get_attr(input, "opgraph") ||
          !lu_mtlx_get_attr(input, "graphoutput"))
        continue;
      ok = up26_bind_input(root, nd, input);
    }
  }
  return ok && up26_udims(root);
}

/* ---- 1.35 -> 1.36 --------------------------------------------------------- */

/* The replace_all lambda of Upgrade35to36Elems. */
static void replace_all(lu_mtlx_sb *text, const char *from, const char *to) {
  const size_t fl = strlen(from), tl = strlen(to);
  lu_mtlx_sb out;
  lu_mtlx_sb_init(&out);
  const char *s = lu_mtlx_sb_str(text);
  /* Searching from pos + to.size() in the rewritten text equals searching
   * from just after each match in the original text. */
  for (const char *p = strstr(s, from); p; p = strstr(s, from)) {
    lu_mtlx_sb_put(&out, s, (size_t)(p - s));
    lu_mtlx_sb_put(&out, to, tl);
    s = p + fl;
  }
  lu_mtlx_sb_puts(&out, s);
  lu_mtlx_sb_set(text, lu_mtlx_sb_str(&out));
  if (out.oom) text->oom = 1;
  lu_mtlx_sb_free(&out);
}

/* Upgrade35to36Elems: material override -> bindinputs on its shaderrefs. */
static int up35_override(lu_mtlx_elem *root, lu_mtlx_elem *elem, const lu_mtlx_elem *child) {
  for (size_t s = 0; s < elem->nchildren; s++) {
    lu_mtlx_elem *sr = &elem->children[s];
    if (!LU_UPG_TAG_IS(sr, "shaderref")) continue;
    const lu_mtlx_elem *nd = lu_mtlx_upg_shader_nodedef(root, sr);
    if (!nd) continue;
    for (size_t k = 0; k < nd->nchildren; k++) {
      const lu_mtlx_elem *av = &nd->children[k];
      if (!LU_UPG_TAG_IS(av, "input")) continue;
      if (strcmp(lu_mtlx_attr_or(av, "publicname", ""), lu_mtlx_name(child)) != 0 ||
          lu_mtlx_child_named(sr, lu_mtlx_name(child)))
        continue;
      lu_mtlx_elem *b = lu_mtlx_upg_push_new(sr, "bindinput");
      if (!b || !set3(b, lu_mtlx_name(av), lu_mtlx_attr_or(av, "type", ""),
                      lu_mtlx_attr_or(child, "value", "")))
        return 0;
    }
  }
  return 1;
}

typedef struct up35_ctx {
  lu_mtlx_elem *root;
  int ok;
} up35_ctx;

/* Upgrade35to36Elems: one element. */
static int up35_visit(lu_mtlx_elem *elem, lu_mtlx_elem *parent, void *user) {
  up35_ctx *x = (up35_ctx *)user;
  (void)parent;
  if (LU_UPG_ATTR_IS(elem, "type", "geomname") && LU_UPG_ATTR_IS(elem, "value", "*"))
    x->ok &= lu_mtlx_attr_set(elem, "value", "/");
  if (LU_UPG_ATTR_IS(elem, "type", "filename")) {
    lu_mtlx_sb v;
    lu_mtlx_sb_init(&v);
    lu_mtlx_sb_set(&v, lu_mtlx_attr_or(elem, "value", ""));
    replace_all(&v, "%UDIM", "<UDIM>");
    replace_all(&v, "%UVTILE", "<UVTILE>");
    x->ok &= !v.oom && lu_mtlx_attr_set(elem, "value", lu_mtlx_sb_str(&v));
    lu_mtlx_sb_free(&v);
  }
  /* Removal marks: the C++ collects indices then erases; the loop body
   * still sees every child at its original index. */
  unsigned char *remove = elem->nchildren ? (unsigned char *)calloc(elem->nchildren, 1) : NULL;
  if (elem->nchildren && !remove) { x->ok = 0; return 1; }
  const size_t count = elem->nchildren;
  for (size_t i = 0; i < count; i++) {
    const lu_mtlx_elem *child = &elem->children[i];
    if (LU_UPG_TAG_IS(elem, "material") && LU_UPG_TAG_IS(child, "override")) {
      x->ok &= up35_override(x->root, elem, child);
      remove[i] = 1;
    } else if (LU_UPG_TAG_IS(elem, "material") && LU_UPG_TAG_IS(child, "materialinherit")) {
      x->ok &= lu_mtlx_attr_set(elem, "inherit", lu_mtlx_attr_or(child, "material", ""));
      remove[i] = 1;
    } else if (LU_UPG_TAG_IS(elem, "look") && LU_UPG_TAG_IS(child, "lookinherit")) {
      x->ok &= lu_mtlx_attr_set(elem, "inherit", lu_mtlx_attr_or(child, "look", ""));
      remove[i] = 1;
    }
  }
  for (size_t k = count; k-- > 0;)
    if (remove[k]) lu_mtlx_child_erase(elem, k);
  free(remove);
  return 1;
}

/* ---- 1.36 -> 1.37 --------------------------------------------------------- */

/* UpgradeMtlxNodeDefTypeToOutput. */
static int nodedef_type_to_output(lu_mtlx_elem *root) {
  int ok = 1;
  for (size_t k = 0; k < root->nchildren; k++) {
    lu_mtlx_elem *nd = &root->children[k];
    if (!LU_UPG_TAG_IS(nd, "nodedef") || !lu_mtlx_get_attr(nd, "type")) continue;
    const char *type = lu_mtlx_get_attr(nd, "type");
    lu_mtlx_elem *out = NULL;
    if (type[0] && strcmp(type, "multioutput") != 0) {
      for (size_t i = 0; i < nd->nchildren; i++) {
        lu_mtlx_elem *c = &nd->children[i];
        if (LU_UPG_TAG_IS(c, "output") && LU_UPG_NAME_IS(c, "out")) { out = c; break; }
      }
      if (!out) {
        out = lu_mtlx_upg_push_new(nd, "output");
        if (!out || !set3(out, "out", type, NULL)) return 0;
      }
    }
    lu_mtlx_attr_erase(nd, "type");
    const char *def_input = lu_mtlx_attr_or(nd, "defaultinput", "");
    if (out && def_input[0]) ok &= lu_mtlx_attr_set(out, "defaultinput", def_input);
    lu_mtlx_attr_erase(nd, "defaultinput");
  }
  return ok;
}

/* UpgradeMtlxRemoveLegacyShaderNodeDefs. */
static int remove_legacy_shader_nodedefs(lu_mtlx_elem *root) {
  lu_mtlx_strv names;
  lu_mtlx_strv_init(&names);
  int ok = 1;
  for (size_t k = 0; k < root->nchildren; k++) {
    const lu_mtlx_elem *c = &root->children[k];
    if (LU_UPG_TAG_IS(c, "nodedef") && lu_mtlx_get_attr(c, "shadertype"))
      ok &= lu_mtlx_strv_push(&names, lu_mtlx_name(c));
  }
  for (size_t n = 0; ok && n < names.n; n++) {
    const char *name = names.v[n];
    for (size_t m = 0; m < root->nchildren; m++) {
      lu_mtlx_elem *mat = &root->children[m];
      if (!LU_UPG_TAG_IS(mat, "material")) continue;
      for (size_t s = 0; s < mat->nchildren; s++) {
        lu_mtlx_elem *sr = &mat->children[s];
        if (LU_UPG_TAG_IS(sr, "shaderref") && LU_UPG_ATTR_IS(sr, "nodedef", name))
          lu_mtlx_attr_erase(sr, "nodedef");
      }
    }
    for (size_t k = root->nchildren; k-- > 0;) {
      const lu_mtlx_elem *c = &root->children[k];
      if (LU_UPG_TAG_IS(c, "nodedef") && LU_UPG_NAME_IS(c, name)) lu_mtlx_child_erase(root, k);
    }
  }
  lu_mtlx_strv_free(&names);
  return ok;
}

/* ---- driver --------------------------------------------------------------- */

/* UpgradeMtlxVersionImpl. */
static int upgrade_version_impl(lu_mtlx_elem *root) {
  if (!LU_UPG_TAG_IS(root, "materialx")) return 1;
  uint64_t major = 0, minor = 0;
  if (!lu_mtlx_parse_version(lu_mtlx_attr_or(root, "version", ""), &major, &minor) ||
      major != 1 ||
      (minor != 22 && minor != 23 && minor != 24 && minor != 25 && minor != 26 &&
       minor != 34 && minor != 35 && minor != 36 && minor != 37 && minor != 38))
    return 1;
  const uint64_t start = minor;
  int ok = 1;
  if (ok && start <= 24) ok = lu_mtlx_upg_shader_name_and_graph_name(root, start <= 23, 1);
  if (ok && start <= 25) ok = lu_mtlx_upg_assign_and_constant(root, start <= 23, 1);
  if (ok && start <= 26) ok = upgrade_26_to_34(root);
  if (ok && start <= 34) ok = lu_mtlx_upg_legacy_attributes(root, start == 22);
  if (ok && start <= 35) {
    up35_ctx x = {root, 1};
    ok = lu_mtlx_upg_preorder(root, up35_visit, &x) && x.ok;
    minor = 36;
  }
  if (ok && minor == 36) {
    ok = upgrade_elements(root, 36) && nodedef_type_to_output(root) &&
         remove_legacy_shader_nodedefs(root);
    minor = 37;
  }
  if (ok && minor == 37) {
    ok = lu_mtlx_upg_color2_channel_aliases(root) && upgrade_elements(root, 37) &&
         lu_mtlx_upg_shader_refs(root) && lu_mtlx_upg_interface_name_conflicts(root);
    minor = 38;
  }
  if (ok && minor == 38) {
    ok = upgrade_elements(root, 38) && lu_mtlx_upg_shader_refs(root) &&
         lu_mtlx_upg_channels(root) && lu_mtlx_upg_thin_film_layers(root) &&
         lu_mtlx_attr_set(root, "version", "1.39");
  }
  return ok;
}

/* UpgradeMtlxVersion. On allocation failure the tree is left partially
 * upgraded (the C++ would have thrown std::bad_alloc). */
void lu_mtlx_upgrade_version(lu_mtlx_elem *root) {
  if (!root) return;
  (void)upgrade_version_impl(root);
}

/* ---- InheritMtlxAttributes ------------------------------------------------ */

/* InheritMtlxAttributes(root): every descendant of root (not root itself)
 * takes each missing scoped attribute from the nearest enclosing element
 * (ScopedMtlxAttr over its ancestor chain, innermost first). Ancestors are
 * processed first, so an inherited value propagates down like the C++. */
void lu_mtlx_inherit_attributes(lu_mtlx_elem *root) {
  static const char *const kScoped[] = {"colorspace", "fileprefix", "geomprefix", "namespace"};
  if (!root) return;
  upg_frame *fr = NULL;
  size_t n = 0, cap = 0;
  if (!frames_push(&fr, &n, &cap, root)) return;
  while (n) {
    upg_frame *f = &fr[n - 1];
    if (f->i >= f->e->nchildren) {
      n--;
      continue;
    }
    lu_mtlx_elem *elem = &f->e->children[f->i++];
    for (size_t k = 0; k < 4; k++) {
      if (lu_mtlx_get_attr(elem, kScoped[k])) continue;
      for (size_t a = n; a-- > 0;) {
        const char *v = lu_mtlx_get_attr(fr[a].e, kScoped[k]);
        if (v) {
          (void)lu_mtlx_attr_set(elem, kScoped[k], v);
          break;
        }
      }
    }
    if (!frames_push(&fr, &n, &cap, elem)) break;
  }
  free(fr);
}
