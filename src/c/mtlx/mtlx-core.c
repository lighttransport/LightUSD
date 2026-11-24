/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX core (C11): string builder, string vectors, element tree, scope.
 * See mtlx-internal.h. */
#include "mtlx-internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- string builder ----------------------------------------------------- */

void lu_mtlx_sb_init(lu_mtlx_sb *b) { memset(b, 0, sizeof(*b)); }

void lu_mtlx_sb_free(lu_mtlx_sb *b) {
  free(b->p);
  memset(b, 0, sizeof(*b));
}

void lu_mtlx_sb_clear(lu_mtlx_sb *b) {
  b->n = 0;
  if (b->p) b->p[0] = '\0';
}

static int sb_reserve(lu_mtlx_sb *b, size_t extra) {
  if (b->oom) return 0;
  if (extra > SIZE_MAX - b->n - 1) {
    b->oom = 1;
    return 0;
  }
  const size_t need = b->n + extra + 1;
  if (need <= b->cap) return 1;
  size_t cap = b->cap ? b->cap : 32;
  while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
  char *p = (char *)realloc(b->p, cap);
  if (!p) {
    b->oom = 1;
    return 0;
  }
  b->p = p;
  b->cap = cap;
  return 1;
}

void lu_mtlx_sb_put(lu_mtlx_sb *b, const char *s, size_t n) {
  if (!sb_reserve(b, n)) return;
  if (n) memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = '\0';
}

void lu_mtlx_sb_puts(lu_mtlx_sb *b, const char *s) {
  if (s) lu_mtlx_sb_put(b, s, strlen(s));
  else if (sb_reserve(b, 0)) b->p[b->n] = '\0';
}

void lu_mtlx_sb_putc(lu_mtlx_sb *b, char c) { lu_mtlx_sb_put(b, &c, 1); }

void lu_mtlx_sb_putu(lu_mtlx_sb *b, uint64_t v) {
  char buf[24];
  size_t i = sizeof(buf);
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  lu_mtlx_sb_put(b, buf + i, sizeof(buf) - i);
}

void lu_mtlx_sb_puti(lu_mtlx_sb *b, int64_t v) {
  if (v < 0) {
    lu_mtlx_sb_putc(b, '-');
    lu_mtlx_sb_putu(b, (uint64_t)0 - (uint64_t)v);
  } else {
    lu_mtlx_sb_putu(b, (uint64_t)v);
  }
}

void lu_mtlx_sb_set(lu_mtlx_sb *b, const char *s) {
  lu_mtlx_sb_clear(b);
  lu_mtlx_sb_puts(b, s);
}

const char *lu_mtlx_sb_str(const lu_mtlx_sb *b) { return b->p && !b->oom ? b->p : ""; }

char *lu_mtlx_sb_take(lu_mtlx_sb *b) {
  if (b->oom) {
    lu_mtlx_sb_free(b);
    return NULL;
  }
  if (!b->p && !sb_reserve(b, 0)) return NULL;
  b->p[b->n] = '\0';
  char *p = b->p;
  memset(b, 0, sizeof(*b));
  return p;
}

void lu_mtlx_err(lu_mtlx_sb *err, const char *msg) {
  if (!err) return;
  err->oom = 0;
  lu_mtlx_sb_set(err, msg);
}

/* ---- strings ------------------------------------------------------------ */

char *lu_mtlx_strndup(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  if (!p) return NULL;
  if (n) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *lu_mtlx_strdup(const char *s) { return lu_mtlx_strndup(s ? s : "", s ? strlen(s) : 0); }

int lu_mtlx_streq(const char *a, const char *b) {
  if (!a || !b) return a == b;
  return strcmp(a, b) == 0;
}

int lu_mtlx_starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

int lu_mtlx_ends_with(const char *s, const char *suffix) {
  const size_t n = strlen(s), m = strlen(suffix);
  return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

/* ---- string vectors ----------------------------------------------------- */

void lu_mtlx_strv_init(lu_mtlx_strv *s) { memset(s, 0, sizeof(*s)); }

void lu_mtlx_strv_free(lu_mtlx_strv *s) {
  for (size_t i = 0; i < s->n; i++) free(s->v[i]);
  free(s->v);
  memset(s, 0, sizeof(*s));
}

int lu_mtlx_strv_push_n(lu_mtlx_strv *s, const char *str, size_t n) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    char **v = (char **)realloc(s->v, cap * sizeof(char *));
    if (!v) return 0;
    s->v = v;
    s->cap = cap;
  }
  char *d = lu_mtlx_strndup(str, n);
  if (!d) return 0;
  s->v[s->n++] = d;
  return 1;
}

int lu_mtlx_strv_push(lu_mtlx_strv *s, const char *str) {
  return lu_mtlx_strv_push_n(s, str ? str : "", str ? strlen(str) : 0);
}

int lu_mtlx_strv_contains(const lu_mtlx_strv *s, const char *str) {
  for (size_t i = 0; i < s->n; i++)
    if (strcmp(s->v[i], str) == 0) return 1;
  return 0;
}

/* ---- elements ------------------------------------------------------------ */

static char kEmpty[1] = "";

void lu_mtlx_elem_init(lu_mtlx_elem *e) {
  memset(e, 0, sizeof(*e));
  e->tag = kEmpty;
}

static void free_tag(char *t) {
  if (t && t != kEmpty) free(t);
}

static void free_shallow(lu_mtlx_elem *e) {
  free_tag(e->tag);
  for (size_t i = 0; i < e->nattrs; i++) {
    free(e->attrs[i].key);
    free(e->attrs[i].value);
  }
  free(e->attrs);
}

void lu_mtlx_elem_free(lu_mtlx_elem *e) {
  if (!e) return;
  /* Explicit stack of child arrays still to release. */
  typedef struct {
    lu_mtlx_elem *v;
    size_t n;
  } arr;
  arr *stack = NULL;
  size_t ns = 0, cap = 0;
  lu_mtlx_elem *kids = e->children;
  size_t nk = e->nchildren;
  free_shallow(e);
  lu_mtlx_elem_init(e);
  for (;;) {
    if (kids) {
      if (ns == cap) {
        const size_t c2 = cap ? cap * 2 : 16;
        arr *s2 = (arr *)realloc(stack, c2 * sizeof(arr));
        if (!s2) {
          /* Cannot track more arrays: release this subtree's own data only. */
          for (size_t i = 0; i < nk; i++) free_shallow(&kids[i]);
          free(kids);
          kids = NULL;
          continue;
        }
        stack = s2;
        cap = c2;
      }
      stack[ns].v = kids;
      stack[ns].n = nk;
      ns++;
      kids = NULL;
    }
    if (!ns) break;
    arr top = stack[--ns];
    for (size_t i = 0; i < top.n; i++) {
      lu_mtlx_elem *c = &top.v[i];
      free_shallow(c);
      if (c->children) {
        if (ns == cap) {
          const size_t c2 = cap ? cap * 2 : 16;
          arr *s2 = (arr *)realloc(stack, c2 * sizeof(arr));
          if (!s2) {
            free(c->children); /* leak-free best effort: grandchildren lost */
            continue;
          }
          stack = s2;
          cap = c2;
        }
        stack[ns].v = c->children;
        stack[ns].n = c->nchildren;
        ns++;
      }
    }
    free(top.v);
  }
  free(stack);
}

static int copy_shallow(lu_mtlx_elem *dst, const lu_mtlx_elem *src) {
  lu_mtlx_elem_init(dst);
  if (src->tag && src->tag[0]) {
    dst->tag = lu_mtlx_strdup(src->tag);
    if (!dst->tag) {
      dst->tag = kEmpty;
      return 0;
    }
  }
  if (src->nattrs) {
    dst->attrs = (lu_mtlx_attr *)calloc(src->nattrs, sizeof(lu_mtlx_attr));
    if (!dst->attrs) return 0;
    dst->capattrs = src->nattrs;
    for (size_t i = 0; i < src->nattrs; i++) {
      dst->attrs[i].key = lu_mtlx_strdup(src->attrs[i].key);
      dst->attrs[i].value = lu_mtlx_strdup(src->attrs[i].value);
      dst->nattrs = i + 1;
      if (!dst->attrs[i].key || !dst->attrs[i].value) return 0;
    }
  }
  if (src->nchildren) {
    dst->children = (lu_mtlx_elem *)malloc(src->nchildren * sizeof(lu_mtlx_elem));
    if (!dst->children) return 0;
    dst->capchildren = src->nchildren;
    for (size_t i = 0; i < src->nchildren; i++) lu_mtlx_elem_init(&dst->children[i]);
    dst->nchildren = src->nchildren;
  }
  return 1;
}

int lu_mtlx_elem_copy(lu_mtlx_elem *dst, const lu_mtlx_elem *src) {
  typedef struct {
    lu_mtlx_elem *d;
    const lu_mtlx_elem *s;
  } job;
  job *stack = NULL;
  size_t ns = 0, cap = 0;
  int ok = copy_shallow(dst, src);
  if (ok && src->nchildren) {
    stack = (job *)malloc(16 * sizeof(job));
    ok = stack != NULL;
    cap = 16;
    if (ok) stack[ns++] = (job){dst, src};
  }
  while (ok && ns) {
    job j = stack[--ns];
    for (size_t i = 0; ok && i < j.s->nchildren; i++) {
      ok = copy_shallow(&j.d->children[i], &j.s->children[i]);
      if (ok && j.s->children[i].nchildren) {
        if (ns == cap) {
          job *s2 = (job *)realloc(stack, cap * 2 * sizeof(job));
          if (!s2) {
            ok = 0;
            break;
          }
          stack = s2;
          cap *= 2;
        }
        stack[ns++] = (job){&j.d->children[i], &j.s->children[i]};
      }
    }
  }
  free(stack);
  if (!ok) {
    lu_mtlx_elem_free(dst);
    lu_mtlx_elem_init(dst);
  }
  return ok;
}

void lu_mtlx_elem_move(lu_mtlx_elem *dst, lu_mtlx_elem *src) {
  *dst = *src;
  lu_mtlx_elem_init(src);
}

int lu_mtlx_elem_set_tag(lu_mtlx_elem *e, const char *tag) {
  if (!tag || !tag[0]) {
    free_tag(e->tag);
    e->tag = kEmpty;
    return 1;
  }
  char *t = lu_mtlx_strdup(tag);
  if (!t) return 0;
  free_tag(e->tag);
  e->tag = t;
  return 1;
}

const char *lu_mtlx_get_attr(const lu_mtlx_elem *e, const char *key) {
  for (size_t i = 0; i < e->nattrs; i++)
    if (strcmp(e->attrs[i].key, key) == 0) return e->attrs[i].value;
  return NULL;
}

const char *lu_mtlx_attr_or(const lu_mtlx_elem *e, const char *key, const char *fb) {
  const char *v = lu_mtlx_get_attr(e, key);
  return v ? v : fb;
}

const char *lu_mtlx_name(const lu_mtlx_elem *e) { return lu_mtlx_attr_or(e, "name", ""); }

int lu_mtlx_attr_append(lu_mtlx_elem *e, const char *key, const char *value) {
  if (e->nattrs == e->capattrs) {
    const size_t cap = e->capattrs ? e->capattrs * 2 : 4;
    lu_mtlx_attr *a = (lu_mtlx_attr *)realloc(e->attrs, cap * sizeof(lu_mtlx_attr));
    if (!a) return 0;
    e->attrs = a;
    e->capattrs = cap;
  }
  char *k = lu_mtlx_strdup(key), *v = lu_mtlx_strdup(value);
  if (!k || !v) {
    free(k);
    free(v);
    return 0;
  }
  e->attrs[e->nattrs].key = k;
  e->attrs[e->nattrs].value = v;
  e->nattrs++;
  return 1;
}

void lu_mtlx_attr_erase(lu_mtlx_elem *e, const char *key) {
  size_t w = 0;
  for (size_t i = 0; i < e->nattrs; i++) {
    if (strcmp(e->attrs[i].key, key) == 0) {
      free(e->attrs[i].key);
      free(e->attrs[i].value);
    } else {
      e->attrs[w++] = e->attrs[i];
    }
  }
  e->nattrs = w;
}

int lu_mtlx_attr_set(lu_mtlx_elem *e, const char *key, const char *value) {
  /* Copy first: `key`/`value` may point into this element's attributes. */
  char *k = lu_mtlx_strdup(key), *v = lu_mtlx_strdup(value);
  if (!k || !v) {
    free(k);
    free(v);
    return 0;
  }
  lu_mtlx_attr_erase(e, k);
  if (e->nattrs == e->capattrs) {
    const size_t cap = e->capattrs ? e->capattrs * 2 : 4;
    lu_mtlx_attr *a = (lu_mtlx_attr *)realloc(e->attrs, cap * sizeof(lu_mtlx_attr));
    if (!a) {
      free(k);
      free(v);
      return 0;
    }
    e->attrs = a;
    e->capattrs = cap;
  }
  e->attrs[e->nattrs].key = k;
  e->attrs[e->nattrs].value = v;
  e->nattrs++;
  return 1;
}

static int reserve_children(lu_mtlx_elem *e, size_t need) {
  if (need <= e->capchildren) return 1;
  size_t cap = e->capchildren ? e->capchildren * 2 : 4;
  while (cap < need) cap *= 2;
  lu_mtlx_elem *c = (lu_mtlx_elem *)realloc(e->children, cap * sizeof(lu_mtlx_elem));
  if (!c) return 0;
  e->children = c;
  e->capchildren = cap;
  return 1;
}

lu_mtlx_elem *lu_mtlx_child_insert(lu_mtlx_elem *e, size_t at, lu_mtlx_elem *moved) {
  if (at > e->nchildren) at = e->nchildren;
  if (!reserve_children(e, e->nchildren + 1)) {
    lu_mtlx_elem_free(moved);
    lu_mtlx_elem_init(moved);
    return NULL;
  }
  memmove(&e->children[at + 1], &e->children[at],
          (e->nchildren - at) * sizeof(lu_mtlx_elem));
  lu_mtlx_elem_move(&e->children[at], moved);
  e->nchildren++;
  return &e->children[at];
}

lu_mtlx_elem *lu_mtlx_child_push(lu_mtlx_elem *e, lu_mtlx_elem *moved) {
  return lu_mtlx_child_insert(e, e->nchildren, moved);
}

void lu_mtlx_child_erase(lu_mtlx_elem *e, size_t at) {
  if (at >= e->nchildren) return;
  lu_mtlx_elem_free(&e->children[at]);
  memmove(&e->children[at], &e->children[at + 1],
          (e->nchildren - at - 1) * sizeof(lu_mtlx_elem));
  e->nchildren--;
}

const lu_mtlx_elem *lu_mtlx_child_named(const lu_mtlx_elem *e, const char *name) {
  for (size_t i = 0; i < e->nchildren; i++)
    if (strcmp(lu_mtlx_name(&e->children[i]), name) == 0) return &e->children[i];
  return NULL;
}

lu_mtlx_elem *lu_mtlx_child_named_mut(lu_mtlx_elem *e, const char *name) {
  return (lu_mtlx_elem *)lu_mtlx_child_named(e, name);
}

/* ---- scope ----------------------------------------------------------------- */

void lu_mtlx_scope_init(lu_mtlx_scope *s) { memset(s, 0, sizeof(*s)); }

void lu_mtlx_scope_free(lu_mtlx_scope *s) {
  free((void *)s->v);
  memset(s, 0, sizeof(*s));
}

int lu_mtlx_scope_push(lu_mtlx_scope *s, const lu_mtlx_elem *e) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    const lu_mtlx_elem **v =
        (const lu_mtlx_elem **)realloc((void *)s->v, cap * sizeof(*v));
    if (!v) return 0;
    s->v = v;
    s->cap = cap;
  }
  s->v[s->n++] = e;
  return 1;
}

int lu_mtlx_scope_with(lu_mtlx_scope *out, const lu_mtlx_elem *e, const lu_mtlx_scope *outer) {
  lu_mtlx_scope_init(out);
  if (!lu_mtlx_scope_push(out, e)) return 0;
  for (size_t i = 0; outer && i < outer->n; i++)
    if (!lu_mtlx_scope_push(out, outer->v[i])) {
      lu_mtlx_scope_free(out);
      return 0;
    }
  return 1;
}

/* ---- shared token helpers (mtlx-resolve.cc Trim / SplitOn) --------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int lu_mtlx_trim(const char *s, lu_mtlx_sb *out) {
  size_t b = 0, e = strlen(s);
  while (b < e && is_space(s[b])) b++;
  while (e > b && is_space(s[e - 1])) e--;
  lu_mtlx_sb_clear(out);
  lu_mtlx_sb_put(out, s + b, e - b);
  return !out->oom;
}

int lu_mtlx_split_on(char sep, const char *s, lu_mtlx_strv *out) {
  lu_mtlx_strv_init(out);
  const char *start = s;
  for (const char *c = s;; c++) {
    if (*c == sep || *c == '\0') {
      if (!lu_mtlx_strv_push_n(out, start, (size_t)(c - start))) {
        lu_mtlx_strv_free(out);
        return 0;
      }
      if (*c == '\0') break;
      start = c + 1;
    }
  }
  return 1;
}

/* ---- ParseMtlxVersion ------------------------------------------------------ */

static int parse_nat_u64(const char *s, uint64_t *out) {
  if (!*s) return 0;
  uint64_t acc = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return 0;
    const uint64_t d = (uint64_t)(*s - '0');
    if (acc > (UINT64_MAX - d) / 10) return 0;
    acc = acc * 10 + d;
  }
  *out = acc;
  return 1;
}

int lu_mtlx_parse_version(const char *text, uint64_t *major, uint64_t *minor) {
  lu_mtlx_strv parts;
  if (!lu_mtlx_split_on('.', text, &parts)) return 0;
  uint64_t a = 0, b = 0;
  const int ok = parts.n == 2 && parse_nat_u64(parts.v[0], &a) && parse_nat_u64(parts.v[1], &b);
  lu_mtlx_strv_free(&parts);
  if (!ok) return 0;
  if (major) *major = a;
  if (minor) *minor = b;
  return 1;
}
