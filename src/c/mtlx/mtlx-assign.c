/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX look assignments (C11): MtlxDocument::
 * GetMaterialAssignmentsForGeometry and the geometry/collection matching of
 * the former src/core/mtlx-dom.cc (SplitMtlxList, MtlxGeomPathParts,
 * MtlxGeomPathMatches, MtlxGeomStringMatches, MtlxCollectionMatchesGeometry). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "mtlx-doc.h"

/* SplitMtlxList: ',' and whitespace separate tokens (istream >> string). */
static int split_list(const char *text, lu_mtlx_strv *out) {
  lu_mtlx_strv_init(out);
  const char *p = text;
  for (;;) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    if (!*p) return 1;
    const char *b = p;
    while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
    if (!lu_mtlx_strv_push_n(out, b, (size_t)(p - b))) {
      lu_mtlx_strv_free(out);
      return 0;
    }
  }
}

/* MtlxGeomPathParts: '/'-separated, empty parts dropped. */
static int path_parts(const char *path, lu_mtlx_strv *out) {
  lu_mtlx_strv_init(out);
  const char *p = path;
  while (*p) {
    const char *e = strchr(p, '/');
    const size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n && !lu_mtlx_strv_push_n(out, p, n)) {
      lu_mtlx_strv_free(out);
      return 0;
    }
    if (!e) break;
    p = e + 1;
  }
  return 1;
}

/* MtlxGeomPathMatches. -1 on oom. */
static int path_matches(const char *selector, const char *geometry, int contains) {
  if (!selector[0] || !geometry[0]) return 0;
  lu_mtlx_strv s, g;
  if (!path_parts(selector, &s)) return -1;
  if (!path_parts(geometry, &g)) {
    lu_mtlx_strv_free(&s);
    return -1;
  }
  int ok = 1;
  if (contains && s.n > g.n) ok = 0;
  const size_t common = s.n < g.n ? s.n : g.n;
  for (size_t i = 0; ok && i < common; i++)
    if (strcmp(s.v[i], g.v[i])) ok = 0;
  lu_mtlx_strv_free(&s);
  lu_mtlx_strv_free(&g);
  return ok;
}

/* MtlxGeomStringMatches. -1 on oom. */
static int string_matches(const char *selectors, const char *geometry, int contains) {
  lu_mtlx_strv list;
  if (!split_list(selectors, &list)) return -1;
  int r = 0;
  for (size_t i = 0; r == 0 && i < list.n; i++) r = path_matches(list.v[i], geometry, contains);
  lu_mtlx_strv_free(&list);
  return r;
}

/* MtlxCollectionMatchesGeometry with an explicit stack of collection frames.
 * Returns 1 (result in *matches) or 0 with the C++ error message. */
typedef struct cframe {
  const lu_mtlx_elem *coll;
  lu_mtlx_strv included;
  size_t next;
} cframe;

static int collection_matches(const lu_mtlx_document *d, const char *name, const char *geometry,
                              int *matches, lu_mtlx_sb *err) {
  cframe *st = NULL;
  size_t n = 0, cap = 0;
  int ok = 1, result = 0, have_result = 0;
  const char *open_name = name; /* collection to enter next, or NULL */
  *matches = 0;
  while (ok) {
    if (open_name) {
      const lu_mtlx_elem *c = lu_mtlx_document_find(d, LU_MTLX_COLLECTIONS, open_name);
      if (!c) {
        lu_mtlx_sb_set(err, "Unknown MaterialX collection '");
        lu_mtlx_sb_puts(err, open_name);
        lu_mtlx_sb_puts(err, "'");
        ok = 0;
        break;
      }
      for (size_t i = 0; i < n; i++)
        if (!strcmp(lu_mtlx_name(st[i].coll), open_name)) {
          lu_mtlx_sb_set(err, "Cycle in MaterialX collection includes at '");
          lu_mtlx_sb_puts(err, open_name);
          lu_mtlx_sb_puts(err, "'");
          ok = 0;
        }
      if (!ok) break;
      /* exclude, then include, decide without descending */
      int r = string_matches(lu_mtlx_attr_or(c, "excludegeom", ""), geometry, 1);
      if (r < 0) {
        ok = 0;
        lu_mtlx_sb_set(err, "out of memory");
        break;
      }
      if (r) {
        result = 0;
        have_result = 1;
      } else {
        r = string_matches(lu_mtlx_attr_or(c, "includegeom", ""), geometry, 0);
        if (r < 0) {
          ok = 0;
          lu_mtlx_sb_set(err, "out of memory");
          break;
        }
        if (r) {
          result = 1;
          have_result = 1;
        }
      }
      if (!have_result) {
        if (n == cap) {
          const size_t c2 = cap ? cap * 2 : 8;
          cframe *s2 = (cframe *)realloc(st, c2 * sizeof(cframe));
          if (!s2) {
            ok = 0;
            lu_mtlx_sb_set(err, "out of memory");
            break;
          }
          st = s2;
          cap = c2;
        }
        st[n].coll = c;
        st[n].next = 0;
        if (!split_list(lu_mtlx_attr_or(c, "includecollection", ""), &st[n].included)) {
          ok = 0;
          lu_mtlx_sb_set(err, "out of memory");
          break;
        }
        n++;
      }
      open_name = NULL;
    }
    if (have_result) {
      /* a child returned: a match propagates to every open frame */
      have_result = 0;
      if (!n) {
        *matches = result;
        break;
      }
      if (result) {
        while (n) lu_mtlx_strv_free(&st[--n].included);
        *matches = 1;
        break;
      }
    }
    if (!n) break;
    cframe *top = &st[n - 1];
    if (top->next < top->included.n) {
      open_name = top->included.v[top->next++];
      continue;
    }
    /* exhausted: this collection does not match */
    lu_mtlx_strv_free(&top->included);
    n--;
    result = 0;
    have_result = 1;
  }
  for (size_t i = 0; i < n; i++) lu_mtlx_strv_free(&st[i].included);
  free(st);
  return ok;
}

int lu_mtlx_document_assignments(const lu_mtlx_document *d, const char *look_name,
                                 const char *geometry, lu_mtlx_assignment **out,
                                 size_t *count, char **error) {
  *out = NULL;
  *count = 0;
  if (error) *error = NULL;
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  lu_mtlx_assignment *res = NULL;
  size_t nres = 0, capres = 0;
  lu_mtlx_strv visited;
  lu_mtlx_strv_init(&visited);
  int ok = 1;
  const lu_mtlx_elem *look = lu_mtlx_document_find(d, LU_MTLX_LOOKS, look_name);
  if (!look) {
    lu_mtlx_sb_set(&err, "MaterialX look not found: ");
    lu_mtlx_sb_puts(&err, look_name);
    ok = 0;
  }
  while (ok && look) {
    if (lu_mtlx_strv_contains(&visited, lu_mtlx_name(look))) {
      lu_mtlx_sb_set(&err, "Cycle in MaterialX look inheritance at '");
      lu_mtlx_sb_puts(&err, lu_mtlx_name(look));
      lu_mtlx_sb_puts(&err, "'");
      ok = 0;
      break;
    }
    if (!lu_mtlx_strv_push(&visited, lu_mtlx_name(look))) {
      ok = 0;
      lu_mtlx_sb_set(&err, "out of memory");
      break;
    }
    for (size_t i = 0; ok && i < look->nchildren; i++) {
      const lu_mtlx_elem *a = &look->children[i];
      if (strcmp(a->tag, "materialassign")) continue;
      int m = string_matches(lu_mtlx_attr_or(a, "geom", ""), geometry, 1);
      if (m < 0) {
        ok = 0;
        lu_mtlx_sb_set(&err, "out of memory");
        break;
      }
      const char *collection = lu_mtlx_attr_or(a, "collection", "");
      if (!m && collection[0] && !collection_matches(d, collection, geometry, &m, &err)) {
        ok = 0;
        break;
      }
      if (!m) continue;
      const char *material_name = lu_mtlx_attr_or(a, "material", "");
      const lu_mtlx_elem *material = lu_mtlx_document_find(d, LU_MTLX_MATERIALS, material_name);
      if (!material) {
        lu_mtlx_sb_set(&err, "MaterialX material not found for assignment '");
        lu_mtlx_sb_puts(&err, lu_mtlx_name(a));
        lu_mtlx_sb_puts(&err, "': ");
        lu_mtlx_sb_puts(&err, material_name);
        ok = 0;
        break;
      }
      if (nres == capres) {
        const size_t c2 = capres ? capres * 2 : 8;
        lu_mtlx_assignment *r2 = (lu_mtlx_assignment *)realloc(res, c2 * sizeof(*r2));
        if (!r2) {
          ok = 0;
          lu_mtlx_sb_set(&err, "out of memory");
          break;
        }
        res = r2;
        capres = c2;
      }
      res[nres].assignment = a;
      res[nres].material = material;
      nres++;
    }
    if (!ok) break;
    const char *inherited = lu_mtlx_attr_or(look, "inherit", "");
    if (!inherited[0]) break;
    look = lu_mtlx_document_find(d, LU_MTLX_LOOKS, inherited);
    if (!look) {
      lu_mtlx_sb_set(&err, "Inherited MaterialX look not found: ");
      lu_mtlx_sb_puts(&err, inherited);
      ok = 0;
    }
  }
  lu_mtlx_strv_free(&visited);
  if (!ok) {
    free(res);
    if (error) *error = lu_mtlx_sb_take(&err);
    else lu_mtlx_sb_free(&err);
    return 0;
  }
  lu_mtlx_sb_free(&err);
  *out = res;
  *count = nres;
  return 1;
}
