/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX unit conversion (C11): MtlxDocument::ConvertToUnit and its
 * helpers (CollectMtlxUnitMetadata, ConvertMtlxInput, ConvertMtlxNodeInputs)
 * of the former src/core/mtlx-dom.cc. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mtlx-doc.h"

/* std::map<std::string, float>: unique names, last assignment wins. */
typedef struct unit_scales {
  char **names;
  float *scales;
  size_t n, cap;
} unit_scales;

static void scales_free(unit_scales *s) {
  for (size_t i = 0; i < s->n; i++) free(s->names[i]);
  free(s->names);
  free(s->scales);
  memset(s, 0, sizeof(*s));
}

static const float *scales_find(const unit_scales *s, const char *name) {
  for (size_t i = 0; i < s->n; i++)
    if (!strcmp(s->names[i], name)) return &s->scales[i];
  return NULL;
}

static int scales_set(unit_scales *s, const char *name, float v) {
  for (size_t i = 0; i < s->n; i++)
    if (!strcmp(s->names[i], name)) {
      s->scales[i] = v;
      return 1;
    }
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    char **n2 = (char **)realloc(s->names, cap * sizeof(char *));
    if (!n2) return 0;
    s->names = n2;
    float *v2 = (float *)realloc(s->scales, cap * sizeof(float));
    if (!v2) return 0;
    s->scales = v2;
    s->cap = cap;
  }
  if (!(s->names[s->n] = lu_mtlx_strdup(name))) return 0;
  s->scales[s->n++] = v;
  return 1;
}

/* ParseFiniteFloat: std::strtof on the whole non-empty text, finite. */
static int parse_finite_float(const char *text, float *value) {
  if (!text[0]) return 0;
  char *end = NULL;
  const float v = strtof(text, &end);
  if (*end != '\0' || !isfinite(v)) return 0;
  *value = v;
  return 1;
}

/* Explicit-stack pre-order walk (the C++ recursion's visiting order). */
typedef struct walk {
  lu_mtlx_elem **v;
  size_t n, cap;
} walk;

static int walk_push(walk *w, lu_mtlx_elem *e) {
  if (w->n == w->cap) {
    const size_t cap = w->cap ? w->cap * 2 : 32;
    lu_mtlx_elem **v = (lu_mtlx_elem **)realloc(w->v, cap * sizeof(*v));
    if (!v) return 0;
    w->v = v;
    w->cap = cap;
  }
  w->v[w->n++] = e;
  return 1;
}

/* CollectMtlxUnitMetadata: 1 ok, 0 error (message in err), -1 oom. */
static int collect_units(lu_mtlx_elem *root, const char *requested, lu_mtlx_strv *types,
                         unit_scales *scales, int *have_scales, lu_mtlx_sb *err) {
  walk w = {0};
  int rc = walk_push(&w, root) ? 1 : -1;
  while (rc == 1 && w.n) {
    lu_mtlx_elem *e = w.v[--w.n];
    if (!strcmp(e->tag, "unittypedef")) {
      const char *name = lu_mtlx_name(e);
      if (name[0] && !lu_mtlx_strv_contains(types, name) && !lu_mtlx_strv_push(types, name))
        rc = -1;
    } else if (!strcmp(e->tag, "unitdef")) {
      if (!strcmp(lu_mtlx_attr_or(e, "unittype", ""), requested)) {
        *have_scales = 1;
        for (size_t i = 0; rc == 1 && i < e->nchildren; i++) {
          const lu_mtlx_elem *u = &e->children[i];
          if (strcmp(u->tag, "unit")) continue;
          const char *name = lu_mtlx_name(u);
          if (!name[0]) continue;
          float scale = 1.0f;
          const char *authored = lu_mtlx_get_attr(u, "scale");
          if (authored && !parse_finite_float(authored, &scale)) {
            lu_mtlx_sb_set(err, "Invalid scale for MaterialX unit '");
            lu_mtlx_sb_puts(err, name);
            lu_mtlx_sb_puts(err, "'");
            rc = 0;
            break;
          }
          if (!scales_set(scales, name, scale)) rc = -1;
        }
      }
    }
    for (size_t i = e->nchildren; rc == 1 && i-- > 0;)
      if (!walk_push(&w, &e->children[i])) rc = -1;
  }
  free(w.v);
  return rc;
}

/* IsMtlxStructuralElement */
static int is_structural(const char *tag) {
  static const char *const kStructural[] = {
      "materialx", "nodegraph", "nodedef", "typedef", "unittypedef", "unitdef",
      "targetdef", "attributedef", "geompropdef", "implementation", "look", "collection",
      "geominfo", "materialassign", "input", "output", "unit", "include"};
  for (size_t i = 0; i < sizeof(kStructural) / sizeof(kStructural[0]); i++)
    if (!strcmp(tag, kStructural[i])) return 1;
  return 0;
}

static void err3(lu_mtlx_sb *err, const char *a, const char *b, const char *c) {
  lu_mtlx_sb_set(err, a);
  lu_mtlx_sb_puts(err, b);
  lu_mtlx_sb_puts(err, c);
}

/* ConvertMtlxInput: 1 ok (maybe nothing done), 0 error, -1 oom. */
static int convert_input(lu_mtlx_elem *input, const char *unit_type, const char *target,
                         const unit_scales *scales, int *converted, lu_mtlx_sb *err) {
  const char *authored_type = lu_mtlx_get_attr(input, "unittype");
  const char *authored_unit = lu_mtlx_get_attr(input, "unit");
  const char *value_text = lu_mtlx_get_attr(input, "value");
  const char *type = lu_mtlx_attr_or(input, "type", "");
  if (!authored_type || strcmp(authored_type, unit_type) || !authored_unit || !value_text ||
      (strcmp(type, "float") && strcmp(type, "vector2") && strcmp(type, "vector3") &&
       strcmp(type, "vector4")))
    return 1;
  lu_mtlx_typed_value parsed;
  lu_mtlx_typed_value_init(&parsed);
  if (!lu_mtlx_parse_value(type, value_text, &parsed)) {
    lu_mtlx_typed_value_free(&parsed);
    err3(err, "Invalid value on MaterialX input '", lu_mtlx_name(input), "'");
    return 0;
  }
  float ratio = 1.0f;
  if (strcmp(authored_unit, target)) {
    const float *from = scales_find(scales, authored_unit);
    const float *to = scales_find(scales, target);
    int bad = 1;
    if (!from) err3(err, "Unknown source MaterialX unit '", authored_unit, "'");
    else if (!to) err3(err, "Unknown target MaterialX unit '", target, "'");
    else if (*to == 0.0f) err3(err, "Target MaterialX unit has a zero scale: '", target, "'");
    else {
      ratio = *from / *to;
      if (!isfinite(ratio)) {
        lu_mtlx_sb_set(err, "Invalid conversion ratio for MaterialX units '");
        lu_mtlx_sb_puts(err, authored_unit);
        lu_mtlx_sb_puts(err, "' and '");
        lu_mtlx_sb_puts(err, target);
        lu_mtlx_sb_puts(err, "'");
      } else {
        bad = 0;
      }
    }
    if (bad) {
      lu_mtlx_typed_value_free(&parsed);
      return 0;
    }
  }
  const size_t n = !strcmp(type, "float") ? 1 : parsed.nnums;
  lu_mtlx_sb text;
  lu_mtlx_sb_init(&text);
  for (size_t i = 0; i < n; i++) {
    float v = !strcmp(type, "float") ? (float)parsed.num : (float)parsed.nums[i];
    v *= ratio;
    if (!isfinite(v)) {
      lu_mtlx_typed_value_free(&parsed);
      lu_mtlx_sb_free(&text);
      err3(err, "Non-finite result converting MaterialX input '", lu_mtlx_name(input), "'");
      return 0;
    }
    if (i) lu_mtlx_sb_puts(&text, ", ");
    lu_mtlx_format_float(v, &text);
  }
  lu_mtlx_typed_value_free(&parsed);
  int ok = !text.oom && lu_mtlx_attr_set(input, "value", lu_mtlx_sb_str(&text));
  lu_mtlx_sb_free(&text);
  if (!ok) return -1;
  lu_mtlx_attr_erase(input, "unit");
  lu_mtlx_attr_erase(input, "unittype");
  *converted = 1;
  return 1;
}

/* ConvertMtlxNodeInputs over every child of the root (pre-order: an
 * element's own inputs, then its non-input children). */
static int convert_all(lu_mtlx_elem *root, const char *unit_type, const char *target,
                       const unit_scales *scales, int *converted, lu_mtlx_sb *err) {
  walk w = {0};
  int rc = 1;
  for (size_t i = root->nchildren; rc == 1 && i-- > 0;)
    if (!walk_push(&w, &root->children[i])) rc = -1;
  while (rc == 1 && w.n) {
    lu_mtlx_elem *e = w.v[--w.n];
    if (!is_structural(e->tag))
      for (size_t i = 0; rc == 1 && i < e->nchildren; i++)
        if (!strcmp(e->children[i].tag, "input"))
          rc = convert_input(&e->children[i], unit_type, target, scales, converted, err);
    for (size_t i = e->nchildren; rc == 1 && i-- > 0;)
      if (strcmp(e->children[i].tag, "input") && !walk_push(&w, &e->children[i])) rc = -1;
  }
  free(w.v);
  return rc;
}

int lu_mtlx_document_convert_to_unit(lu_mtlx_document *d, const char *unit_type,
                                     const char *target) {
  lu_mtlx_sb_clear(&d->error);
  d->error.oom = 0;
  if (!d->parsed) {
    lu_mtlx_doc_set_error(d, "", "MaterialX document has not been parsed");
    return 0;
  }
  lu_mtlx_elem candidate;
  if (!lu_mtlx_elem_copy(&candidate, &d->root)) {
    lu_mtlx_doc_set_error(d, "", "out of memory");
    return 0;
  }
  lu_mtlx_strv types;
  lu_mtlx_strv_init(&types);
  unit_scales scales = {0};
  int have_scales = 0;
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  int rc = collect_units(&candidate, unit_type, &types, &scales, &have_scales, &err);
  if (rc == 1 && (!lu_mtlx_strv_contains(&types, unit_type) || !have_scales || !scales.n)) {
    lu_mtlx_sb_set(&err, "No MaterialX unit converter is defined for type '");
    lu_mtlx_sb_puts(&err, unit_type);
    lu_mtlx_sb_puts(&err, "'");
    rc = 0;
  }
  int converted = 0;
  if (rc == 1) rc = convert_all(&candidate, unit_type, target, &scales, &converted, &err);
  lu_mtlx_strv_free(&types);
  scales_free(&scales);
  if (rc != 1 || !converted) {
    if (rc == 0) lu_mtlx_doc_set_error(d, "", lu_mtlx_sb_str(&err));
    else if (rc < 0) lu_mtlx_doc_set_error(d, "", "out of memory");
    lu_mtlx_sb_free(&err);
    lu_mtlx_elem_free(&candidate);
    return 0;
  }
  lu_mtlx_sb_free(&err);
  if (!lu_mtlx_doc_adopt_root(d, &candidate)) {
    lu_mtlx_doc_set_error(d, "", "Failed to rebuild MaterialX document after unit conversion");
    return 0;
  }
  return 1;
}
