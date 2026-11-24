/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * Scoped MaterialX attributes (C11 port of src/core/mtlx-resolve.cc
 * ScopedMtlxAttr / ActiveMtlxColorSpace / ResolvedMtlxFilename).
 */
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

/* ScopedMtlxAttr: *out (borrowed from the chain) may be NULL. */
int lu_mtlx_scoped_attr(const char *key, const lu_mtlx_scope *chain, const char **out) {
  for (size_t i = 0; i < chain->n; i++) {
    const lu_mtlx_elem *e = chain->v[i];
    if (!e) continue;
    const char *v = lu_mtlx_get_attr(e, key);
    if (v) {
      if (out) *out = v;
      return 1;
    }
  }
  return 0;
}

/* ActiveMtlxColorSpace (result replaces out's contents; out may be NULL). */
int lu_mtlx_active_colorspace(const lu_mtlx_scope *chain, lu_mtlx_sb *out) {
  const char *cs = NULL;
  if (!lu_mtlx_scoped_attr("colorspace", chain, &cs)) return 0;
  if (!strcmp(cs, "data") || !strcmp(cs, "none")) return 0;
  if (out) {
    lu_mtlx_sb_set(out, cs);
    if (out->oom) return 0;
  }
  return 1;
}

/* One interface-token substitution: "[name]" -> value (value borrowed). */
typedef struct token_sub {
  char *key;
  const char *value;
  size_t keylen;
} token_sub;

typedef struct token_subs {
  token_sub *v;
  size_t n, cap;
  int oom;
} token_subs;

/* add_tokens lambda of ResolvedMtlxFilename */
static void add_tokens(token_subs *subs, const lu_mtlx_elem *e) {
  for (size_t i = 0; i < e->nchildren && !subs->oom; i++) {
    const lu_mtlx_elem *t = &e->children[i];
    if (strcmp(t->tag, "token") != 0) continue;
    const char *tv = lu_mtlx_get_attr(t, "value");
    if (!tv) continue;
    if (subs->n == subs->cap) {
      const size_t cap = subs->cap ? subs->cap * 2 : 8;
      token_sub *v = (token_sub *)realloc(subs->v, cap * sizeof(*v));
      if (!v) {
        subs->oom = 1;
        return;
      }
      subs->v = v;
      subs->cap = cap;
    }
    lu_mtlx_sb k;
    lu_mtlx_sb_init(&k);
    lu_mtlx_sb_putc(&k, '[');
    lu_mtlx_sb_puts(&k, lu_mtlx_name(t));
    lu_mtlx_sb_putc(&k, ']');
    const size_t klen = k.n;
    char *key = lu_mtlx_sb_take(&k);
    if (!key) {
      subs->oom = 1;
      return;
    }
    subs->v[subs->n].key = key;
    subs->v[subs->n].keylen = klen;
    subs->v[subs->n].value = tv;
    subs->n++;
  }
}

static void free_subs(token_subs *s) {
  for (size_t i = 0; i < s->n; i++) free(s->v[i].key);
  free(s->v);
}

/* ResolvedMtlxFilename (result replaces out's contents; out may be NULL). */
int lu_mtlx_resolved_filename(const lu_mtlx_scope *chain, lu_mtlx_sb *out) {
  if (chain->n == 0 || !chain->v[0]) return 0;
  const lu_mtlx_elem *inp = chain->v[0];
  if (strcmp(lu_mtlx_attr_or(inp, "type", ""), "filename") != 0) return 0;
  const char *v = lu_mtlx_get_attr(inp, "value");
  if (!v) return 0;
  const char *prefix = "";
  (void)lu_mtlx_scoped_attr("fileprefix", chain, &prefix);
  token_subs subs = {NULL, 0, 0, 0};
  const lu_mtlx_elem *root = chain->n > 1 ? chain->v[chain->n - 1] : NULL;
  for (size_t i = 1; i < chain->n; i++) {
    const lu_mtlx_elem *ci = chain->v[i];
    if (!ci) continue;
    add_tokens(&subs, ci);
    if (!strcmp(ci->tag, "nodegraph") && root) {
      const char *nd = lu_mtlx_get_attr(ci, "nodedef");
      if (nd) {
        for (size_t c = 0; c < root->nchildren; c++) {
          const lu_mtlx_elem *rc = &root->children[c];
          if (!strcmp(rc->tag, "nodedef") && !strcmp(lu_mtlx_name(rc), nd)) {
            add_tokens(&subs, rc);
            break;
          }
        }
      }
    }
  }
  lu_mtlx_sb full, res;
  lu_mtlx_sb_init(&full);
  lu_mtlx_sb_init(&res);
  lu_mtlx_sb_puts(&full, prefix);
  lu_mtlx_sb_puts(&full, v);
  const char *f = lu_mtlx_sb_str(&full);
  const size_t fn = full.n;
  size_t i = 0;
  while (i < fn) {
    int hit = 0;
    for (size_t k = 0; k < subs.n; k++) {
      if (subs.v[k].keylen <= fn - i && !memcmp(f + i, subs.v[k].key, subs.v[k].keylen)) {
        lu_mtlx_sb_puts(&res, subs.v[k].value);
        i += subs.v[k].keylen;
        hit = 1;
        break;
      }
    }
    if (!hit) lu_mtlx_sb_putc(&res, f[i++]);
  }
  const int ok = !subs.oom && !full.oom && !res.oom;
  if (ok && out) lu_mtlx_sb_set(out, lu_mtlx_sb_str(&res));
  free_subs(&subs);
  lu_mtlx_sb_free(&full);
  lu_mtlx_sb_free(&res);
  return ok && (!out || !out->oom);
}
