/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * path-expr.c -- C11 SdfPathExpression parser / serializer / matcher.
 *
 * Behavioural port of the next-side SdfPathExpression model: an expression is
 * parsed into a set-algebra tree which is flattened into a prefix-ordered op
 * stream plus left-to-right pattern/reference streams. Matching is a memoized
 * dynamic program over (element, segment) with `//` stretch support.
 *
 * Hardening differences from a literal translation:
 * - Every allocation is explicitly sized and checked; failure degrades to an
 *   invalid expression, never an abort.
 * - The serializer is iterative (no recursion), so a long operator chain
 *   cannot exhaust the stack.
 * - The matcher drives an explicit frame stack (bounded by the element count)
 *   and the memo product is bounded, so pathological-but-legal inputs stay
 *   linear in the number of elements and segments.
 * - Character classification is deterministic ASCII (no locale-dependent
 *   ctype), so results cannot depend on the ambient locale.
 */

#include "lightusd-core-c.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Small helpers
 * ============================================================ */

static int lu_is_blank(char c) { return c == ' ' || c == '\t'; }

/* Deterministic ASCII alnum (avoids locale-dependent ctype). */
static int lu_is_factor_start(char c) {
  const unsigned char u = (unsigned char)c;
  return c == '~' || c == '(' || c == '%' || c == '/' || c == '.' ||
         c == '*' || c == '?' || c == '_' ||
         (u >= (unsigned char)'0' && u <= (unsigned char)'9') ||
         (u >= (unsigned char)'a' && u <= (unsigned char)'z') ||
         (u >= (unsigned char)'A' && u <= (unsigned char)'Z');
}

static int lu_has_wildcard(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '*' || s[i] == '?') return 1;
  }
  return 0;
}

static int lu_has_brace(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '{') return 1;
  }
  return 0;
}

static char *lu_dup(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  if (p == NULL) return NULL;
  if (n != 0 && s != NULL) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

/* Growable byte buffer (canonical text, prefix assembly, raw token scan). */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  int oom;
} strbuf;

static int sb_init(strbuf *b) {
  b->cap = 64;
  b->len = 0;
  b->oom = 0;
  b->buf = (char *)malloc(b->cap);
  if (b->buf == NULL) {
    b->oom = 1;
    return 0;
  }
  b->buf[0] = '\0';
  return 1;
}

static void sb_free(strbuf *b) {
  free(b->buf);
  b->buf = NULL;
  b->len = 0;
  b->cap = 0;
}

static int sb_reserve(strbuf *b, size_t extra) {
  if (b->oom) return 0;
  if (b->len + extra + 1 <= b->cap) return 1;
  size_t cap = b->cap;
  while (cap < b->len + extra + 1) {
    if (cap > (((size_t)-1) / 2)) {
      b->oom = 1;
      return 0;
    }
    cap *= 2;
  }
  char *grown = (char *)realloc(b->buf, cap);
  if (grown == NULL) {
    b->oom = 1;
    return 0;
  }
  b->buf = grown;
  b->cap = cap;
  return 1;
}

static int sb_put(strbuf *b, const char *s, size_t n) {
  if (!sb_reserve(b, n)) return 0;
  if (n != 0) memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
  return 1;
}

static int sb_putc(strbuf *b, char c) { return sb_put(b, &c, 1); }

/* ============================================================
 * Expression object
 * ============================================================ */

typedef struct lu_component {
  char *text;
  size_t text_len;
  char *predicate;
  size_t predicate_len;
  int stretch;
} lu_component;

typedef struct lu_pattern {
  char *prefix;
  size_t prefix_len;
  lu_component *components;
  size_t ncomponents;
  size_t comp_cap;
  int match_property;
  char *property_name;
  size_t property_len;
} lu_pattern;

typedef struct lu_ref {
  char *path;
  size_t path_len;
  char *name;
  size_t name_len;
} lu_ref;

struct lu_path_expr {
  int valid;
  char *err; /* diagnostic; only meaningful when !valid */

  uint8_t *ops;
  size_t nops;
  size_t ops_cap;

  lu_pattern *patterns;
  size_t npatterns;
  size_t pat_cap;

  lu_ref *refs;
  size_t nrefs;
  size_t ref_cap;
};

static void pattern_init(lu_pattern *p) { memset(p, 0, sizeof(*p)); }

static void pattern_free(lu_pattern *p) {
  for (size_t i = 0; i < p->ncomponents; i++) {
    free(p->components[i].text);
    free(p->components[i].predicate);
  }
  free(p->components);
  free(p->prefix);
  free(p->property_name);
  pattern_init(p);
}

static int pattern_push_component(lu_pattern *p, const char *text,
                                  size_t text_len, const char *predicate,
                                  size_t predicate_len, int stretch) {
  if (p->ncomponents == p->comp_cap) {
    const size_t cap = p->comp_cap == 0 ? 4 : p->comp_cap * 2;
    if (cap > LU_PATH_EXPR_MAX_MATCH_ELEMS) return 0;
    lu_component *grown =
        (lu_component *)realloc(p->components, cap * sizeof(lu_component));
    if (grown == NULL) return 0;
    p->components = grown;
    p->comp_cap = cap;
  }
  lu_component *c = &p->components[p->ncomponents];
  memset(c, 0, sizeof(*c));
  c->text = lu_dup(text, text_len);
  c->predicate = lu_dup(predicate, predicate_len);
  if (c->text == NULL || c->predicate == NULL) {
    free(c->text);
    free(c->predicate);
    memset(c, 0, sizeof(*c));
    return 0;
  }
  c->text_len = text_len;
  c->predicate_len = predicate_len;
  /* "stretch" is derived (empty text and empty predicate), not a stored kind:
   * a component from an empty text run (e.g. the middle slash of "///A")
   * serializes and matches as `//`, like the reference is_stretch(). */
  (void)stretch;
  c->stretch = (text_len == 0 && predicate_len == 0) ? 1 : 0;
  p->ncomponents++;
  return 1;
}

static int pattern_clone(lu_pattern *dst, const lu_pattern *src) {
  pattern_init(dst);
  dst->prefix = lu_dup(src->prefix, src->prefix_len);
  dst->prefix_len = src->prefix_len;
  dst->match_property = src->match_property;
  dst->property_name = lu_dup(src->property_name, src->property_len);
  dst->property_len = src->property_len;
  if ((src->prefix != NULL && dst->prefix == NULL) ||
      (src->property_name != NULL && dst->property_name == NULL)) {
    pattern_free(dst);
    return 0;
  }
  for (size_t i = 0; i < src->ncomponents; i++) {
    const lu_component *c = &src->components[i];
    if (!pattern_push_component(dst, c->text, c->text_len, c->predicate,
                                c->predicate_len, c->stretch)) {
      pattern_free(dst);
      return 0;
    }
  }
  return 1;
}

static void ref_init(lu_ref *r) { memset(r, 0, sizeof(*r)); }

static void ref_free(lu_ref *r) {
  free(r->path);
  free(r->name);
  ref_init(r);
}

static int ref_clone(lu_ref *dst, const lu_ref *src) {
  ref_init(dst);
  dst->path = lu_dup(src->path, src->path_len);
  dst->name = lu_dup(src->name, src->name_len);
  dst->path_len = src->path_len;
  dst->name_len = src->name_len;
  if ((src->path != NULL && dst->path == NULL) ||
      (src->name != NULL && dst->name == NULL)) {
    ref_free(dst);
    return 0;
  }
  return 1;
}

static lu_path_expr *expr_new(void) {
  return (lu_path_expr *)calloc(1, sizeof(lu_path_expr));
}

static void expr_set_error(lu_path_expr *e, const char *msg) {
  if (e->err != NULL) return;
  e->err = lu_dup(msg, strlen(msg));
}

static int expr_reserve_ops(lu_path_expr *e, size_t extra) {
  if (e->nops + extra <= e->ops_cap) return 1;
  size_t cap = e->ops_cap == 0 ? 16 : e->ops_cap;
  while (cap < e->nops + extra) {
    if (cap > (((size_t)-1) / 2)) return 0;
    cap *= 2;
  }
  uint8_t *grown = (uint8_t *)realloc(e->ops, cap);
  if (grown == NULL) return 0;
  e->ops = grown;
  e->ops_cap = cap;
  return 1;
}

static int expr_push_op(lu_path_expr *e, uint8_t op) {
  if (!expr_reserve_ops(e, 1)) return 0;
  e->ops[e->nops++] = op;
  return 1;
}

static int expr_reserve_patterns(lu_path_expr *e) {
  if (e->npatterns != e->pat_cap) return 1;
  const size_t cap = e->pat_cap == 0 ? 8 : e->pat_cap * 2;
  lu_pattern *grown =
      (lu_pattern *)realloc(e->patterns, cap * sizeof(lu_pattern));
  if (grown == NULL) return 0;
  e->patterns = grown;
  e->pat_cap = cap;
  return 1;
}

static int expr_push_pattern(lu_path_expr *e, const lu_pattern *src) {
  if (!expr_reserve_patterns(e)) return 0;
  lu_pattern *dst = &e->patterns[e->npatterns];
  pattern_init(dst);
  if (!pattern_clone(dst, src)) return 0;
  e->npatterns++;
  return 1;
}

static int expr_reserve_refs(lu_path_expr *e) {
  if (e->nrefs != e->ref_cap) return 1;
  const size_t cap = e->ref_cap == 0 ? 8 : e->ref_cap * 2;
  lu_ref *grown = (lu_ref *)realloc(e->refs, cap * sizeof(lu_ref));
  if (grown == NULL) return 0;
  e->refs = grown;
  e->ref_cap = cap;
  return 1;
}

static int expr_push_ref(lu_path_expr *e, const lu_ref *src) {
  if (!expr_reserve_refs(e)) return 0;
  lu_ref *dst = &e->refs[e->nrefs];
  ref_init(dst);
  if (!ref_clone(dst, src)) return 0;
  e->nrefs++;
  return 1;
}

static void expr_destroy(lu_path_expr *e) {
  if (e == NULL) return;
  for (size_t i = 0; i < e->npatterns; i++) pattern_free(&e->patterns[i]);
  for (size_t i = 0; i < e->nrefs; i++) ref_free(&e->refs[i]);
  free(e->patterns);
  free(e->refs);
  free(e->ops);
  free(e->err);
  free(e);
}

/* ============================================================
 * Parser
 * ============================================================ */

enum { NODE_PATTERN = 0, NODE_REF = 1, NODE_COMPLEMENT = 2, NODE_BINOP = 3 };

typedef struct pnode {
  int kind;
  uint8_t op; /* binop operator when kind == NODE_BINOP */
  lu_pattern pattern;
  lu_ref ref;
  int a, b; /* child node indices; -1 = none */
} pnode;

typedef struct parser {
  const char *s;
  size_t n;
  size_t pos;

  pnode *nodes;
  size_t nnodes;
  size_t nodes_cap;
  size_t node_count; /* budget across the whole tree */

  int failed;
  const char *err; /* static diagnostic */
} parser;

static void parser_free_nodes(parser *p) {
  if (p->nodes != NULL) {
    for (size_t i = 0; i < p->nnodes; i++) {
      pattern_free(&p->nodes[i].pattern);
      ref_free(&p->nodes[i].ref);
    }
    free(p->nodes);
  }
  p->nodes = NULL;
  p->nnodes = 0;
  p->nodes_cap = 0;
}

static int parser_fail(parser *p, const char *msg) {
  if (!p->failed) {
    p->failed = 1;
    p->err = msg;
  }
  return -1;
}

static int parser_new_node(parser *p, int kind) {
  if (p->node_count >= LU_PATH_EXPR_MAX_NODES) {
    p->failed = 1;
    if (p->err == NULL) p->err = "Path expression has too many nodes";
    return -1;
  }
  if (p->nnodes == p->nodes_cap) {
    size_t cap = p->nodes_cap == 0 ? 16 : p->nodes_cap * 2;
    if (cap > LU_PATH_EXPR_MAX_NODES) cap = LU_PATH_EXPR_MAX_NODES;
    pnode *grown = (pnode *)realloc(p->nodes, cap * sizeof(pnode));
    if (grown == NULL) return parser_fail(p, "Out of memory");
    p->nodes = grown;
    p->nodes_cap = cap;
  }
  pnode *nd = &p->nodes[p->nnodes];
  memset(nd, 0, sizeof(*nd));
  nd->kind = kind;
  nd->op = (uint8_t)LU_PATH_EXPR_OP_UNION;
  nd->a = -1;
  nd->b = -1;
  p->node_count++;
  return (int)(p->nnodes++);
}

static int p_eof(const parser *p) { return p->pos >= p->n; }
static char p_peek(const parser *p) { return p_eof(p) ? '\0' : p->s[p->pos]; }

static void p_skip_blanks(parser *p) {
  while (!p_eof(p) && lu_is_blank(p->s[p->pos])) p->pos++;
}

/* Split "text{pred}" into text + predicate (raw, no braces). */
static int make_component(const char *seg, size_t seg_len, lu_pattern *out) {
  size_t open = (size_t)-1;
  for (size_t i = 0; i < seg_len; i++) {
    if (seg[i] == '{') {
      open = i;
      break;
    }
  }
  if (open != (size_t)-1 && seg_len != 0 && seg[seg_len - 1] == '}') {
    return pattern_push_component(out, seg, open, seg + open + 1,
                                  seg_len - open - 2, 0);
  }
  return pattern_push_component(out, seg, seg_len, NULL, 0, 0);
}

/* One scanned pattern element: a text run or a stretch. */
typedef struct {
  int stretch;
  const char *text;
  size_t text_len;
} scan_elem;

/* Scan a raw pattern token into elements, recognizing `//` as a stretch and a
 * single `/` as a separator. Text runs may contain predicate braces (which may
 * themselves contain '/'). */
static int scan_pattern(const char *raw, size_t n, int *absolute,
                        scan_elem *elems, size_t elems_cap, size_t *out_count) {
  size_t count = 0;
  size_t i = 0;

  *absolute = (n > 0 && raw[0] == '/');
  if (*absolute) {
    if (n > 1 && raw[1] == '/') {
      if (count >= elems_cap) return 0;
      elems[count].stretch = 1;
      elems[count].text = NULL;
      elems[count].text_len = 0;
      count++;
      i = 2;
    } else {
      i = 1;
    }
  }

  while (i < n) {
    const char *start = raw + i;
    size_t run_len = 0;
    int brace = 0;
    while (i < n) {
      char c = raw[i];
      if (c == '{') brace++;
      else if (c == '}' && brace > 0) brace--;
      if (c == '/' && brace == 0) break;
      i++;
      run_len++;
    }
    if (count >= elems_cap) return 0;
    elems[count].stretch = 0;
    elems[count].text = start;
    elems[count].text_len = run_len;
    count++;

    if (i < n && raw[i] == '/') {
      if (i + 1 < n && raw[i + 1] == '/') {
        if (count >= elems_cap) return 0;
        elems[count].stretch = 1;
        elems[count].text = NULL;
        elems[count].text_len = 0;
        count++;
        i += 2;
      } else {
        i += 1; /* plain separator; a trailing single '/' is ignored */
      }
    }
  }

  *out_count = count;
  return 1;
}

/* Decompose a raw pattern token into a lu_pattern. */
static int decompose_pattern(parser *p, const char *raw, size_t raw_len,
                             lu_pattern *out) {
  pattern_init(out);
  if (raw_len == 0) return 1;

  const size_t elems_cap = raw_len + 2;
  scan_elem *elems = (scan_elem *)malloc(elems_cap * sizeof(scan_elem));
  if (elems == NULL) return parser_fail(p, "Out of memory");

  int absolute = 0;
  size_t nelems = 0;
  if (!scan_pattern(raw, raw_len, &absolute, elems, elems_cap, &nelems)) {
    free(elems);
    return parser_fail(p, "Path expression is too large");
  }

  /* Detect a trailing property on the final text element: "name.prop"
   * (not the relative tokens "." / ".."), outside a predicate. */
  char *property = NULL;
  size_t property_len = 0;
  int has_property = 0;
  if (nelems > 0 && !elems[nelems - 1].stretch) {
    const char *last = elems[nelems - 1].text;
    size_t last_len = elems[nelems - 1].text_len;
    int is_relative_token = (last_len == 1 && last[0] == '.') ||
                            (last_len == 2 && last[0] == '.' && last[1] == '.');
    int has_brace = 0;
    for (size_t i = 0; i < last_len; i++) {
      if (last[i] == '{') {
        has_brace = 1;
        break;
      }
    }
    if (!is_relative_token && !has_brace) {
      for (size_t d = 0; d < last_len; d++) {
        if (last[d] != '.') continue;
        if (d > 0 && d + 1 < last_len) {
          property = lu_dup(last + d + 1, last_len - d - 1);
          property_len = last_len - d - 1;
          if (property == NULL) {
            free(elems);
            return parser_fail(p, "Out of memory");
          }
          has_property = 1;
          elems[nelems - 1].text_len = d;
        }
        break;
      }
    }
  }

  /* Fold leading literal (non-wildcard, non-predicate, non-stretch) elements
   * into the prefix path; the remainder become components. */
  strbuf prefix;
  if (!sb_init(&prefix)) {
    free(elems);
    free(property);
    return parser_fail(p, "Out of memory");
  }
  if (absolute) sb_putc(&prefix, '/');

  size_t i = 0;
  for (; i < nelems; i++) {
    const scan_elem *e = &elems[i];
    /* A literal is a plain, wildcard-free, predicate-free text element. A
     * predicate element must never be folded into the prefix. */
    if (e->stretch || lu_has_wildcard(e->text, e->text_len) ||
        lu_has_brace(e->text, e->text_len)) {
      break;
    }
    if (prefix.len != 0 && !(prefix.len == 1 && prefix.buf[0] == '/')) {
      sb_putc(&prefix, '/');
    }
    sb_put(&prefix, e->text, e->text_len);
  }

  int ok = 1;
  if (prefix.oom) {
    ok = parser_fail(p, "Out of memory");
  } else {
    out->prefix = lu_dup(prefix.buf, prefix.len);
    out->prefix_len = prefix.len;
    if (out->prefix == NULL) ok = parser_fail(p, "Out of memory");
  }
  sb_free(&prefix);

  for (; ok && i < nelems; i++) {
    const scan_elem *e = &elems[i];
    if (e->stretch) {
      ok = pattern_push_component(out, NULL, 0, NULL, 0, 1);
    } else {
      ok = make_component(e->text, e->text_len, out);
    }
  }

  if (ok && has_property) {
    out->match_property = 1;
    out->property_name = property;
    out->property_len = property_len;
    property = NULL; /* ownership moved into the pattern */
    ok = pattern_push_component(out, out->property_name, out->property_len,
                                NULL, 0, 0);
  }

  free(elems);
  free(property);
  if (!ok) {
    if (!p->failed) parser_fail(p, "Out of memory");
    pattern_free(out);
    return -1;
  }
  return 1;
}

/* Per OpenUSD grammar: OptSpaced<'+'|'&'|'-'> | implied-union (plus <blank>). */
static int parse_operator(parser *p, uint8_t *op) {
  size_t save = p->pos;
  size_t blanks = 0;
  while (!p_eof(p) && lu_is_blank(p->s[p->pos])) {
    p->pos++;
    blanks++;
  }
  if (p_eof(p)) {
    p->pos = save;
    return 0;
  }
  char c = p_peek(p);
  if (c == '+' || c == '&' || c == '-') {
    p->pos++;
    p_skip_blanks(p);
    *op = (c == '+') ? (uint8_t)LU_PATH_EXPR_OP_UNION
                     : (c == '&') ? (uint8_t)LU_PATH_EXPR_OP_INTERSECTION
                                  : (uint8_t)LU_PATH_EXPR_OP_DIFFERENCE;
    return 1;
  }
  if (blanks >= 1 && lu_is_factor_start(c)) {
    *op = (uint8_t)LU_PATH_EXPR_OP_IMPLIED_UNION;
    return 1;
  }
  p->pos = save;
  return 0;
}

static int parse_expr(parser *p, size_t depth);

static int parse_ref(parser *p) {
  strbuf body;
  if (!sb_init(&body)) return parser_fail(p, "Out of memory");
  p->pos++; /* consume '%' */
  while (!p_eof(p)) {
    char c = p_peek(p);
    if (lu_is_blank(c) || c == '+' || c == '&' || c == '-' || c == '~' ||
        c == '(' || c == ')') {
      break;
    }
    if (!sb_putc(&body, c)) break;
    p->pos++;
  }

  int idx = parser_new_node(p, NODE_REF);
  if (idx < 0) {
    sb_free(&body);
    return -1;
  }
  lu_ref *ref = &p->nodes[idx].ref;

  int weaker = (body.len == 1 && body.buf[0] == '_');
  if (weaker) {
    ref->name = lu_dup("_", 1);
    ref->name_len = 1;
  } else {
    size_t colon = (size_t)-1;
    for (size_t k = body.len; k > 0; k--) {
      if (body.buf[k - 1] == ':') {
        colon = k - 1;
        break;
      }
    }
    size_t path_len, name_off, name_len;
    if (colon != (size_t)-1) {
      path_len = colon;
      name_off = colon + 1;
      name_len = body.len - colon - 1;
    } else {
      path_len = body.len;
      name_off = 0;
      name_len = 0;
    }
    ref->path = lu_dup(body.buf, path_len);
    ref->path_len = path_len;
    ref->name = lu_dup(name_len != 0 ? body.buf + name_off : NULL, name_len);
    ref->name_len = name_len;
  }
  sb_free(&body);

  if ((!weaker && ref->path == NULL) || ref->name == NULL) {
    return parser_fail(p, "Out of memory");
  }
  return idx;
}

static int parse_pattern(parser *p) {
  strbuf raw;
  if (!sb_init(&raw)) return parser_fail(p, "Out of memory");

  while (!p_eof(p)) {
    char c = p_peek(p);
    if (lu_is_blank(c) || c == '+' || c == '&' || c == '-' || c == '~' ||
        c == '(' || c == ')') {
      break;
    }
    if (c == '{') {
      int depth = 0;
      while (!p_eof(p)) {
        char cc = p_peek(p);
        if (!sb_putc(&raw, cc)) break;
        p->pos++;
        if (cc == '{') {
          depth++;
        } else if (cc == '}') {
          depth--;
          if (depth == 0) break;
        }
      }
      continue;
    }
    if (!sb_putc(&raw, c)) break;
    p->pos++;
  }

  int failed_oom = raw.oom;
  int empty = raw.len == 0;
  int idx = -1;
  if (!failed_oom && !empty) {
    idx = parser_new_node(p, NODE_PATTERN);
  }
  if (idx >= 0) {
    int ok = decompose_pattern(p, raw.buf, raw.len, &p->nodes[idx].pattern);
    if (!ok) idx = -1;
  }
  sb_free(&raw);

  if (failed_oom) return parser_fail(p, "Out of memory");
  if (empty) {
    return parser_fail(p, "Expected a path pattern in path expression");
  }
  return idx;
}

static int parse_atom(parser *p, size_t depth) {
  p_skip_blanks(p);
  char c = p_peek(p);
  if (c == '(') {
    if (depth >= LU_PATH_EXPR_MAX_DEPTH) {
      return parser_fail(p, "Path expression nesting is too deep");
    }
    p->pos++;
    p_skip_blanks(p);
    int e = parse_expr(p, depth + 1);
    if (p->failed) return -1;
    p_skip_blanks(p);
    if (p_peek(p) != ')') {
      return parser_fail(p, "Expected ')' in path expression");
    }
    p->pos++;
    return e;
  }
  if (c == '%') return parse_ref(p);
  return parse_pattern(p);
}

static int parse_factor(parser *p, size_t depth) {
  p_skip_blanks(p);
  int complement = 0;
  if (p_peek(p) == '~') {
    complement = 1;
    p->pos++;
    p_skip_blanks(p);
  }
  int atom = parse_atom(p, depth);
  if (p->failed) return -1;
  if (!complement) return atom;

  int idx = parser_new_node(p, NODE_COMPLEMENT);
  if (idx < 0) return -1;
  p->nodes[idx].a = atom;
  return idx;
}

static int parse_expr(parser *p, size_t depth) {
  int left = parse_factor(p, depth);
  if (p->failed) return -1;
  for (;;) {
    uint8_t op;
    if (!parse_operator(p, &op)) break;
    int right = parse_factor(p, depth);
    if (p->failed) return -1;
    int bin = parser_new_node(p, NODE_BINOP);
    if (bin < 0) return -1;
    p->nodes[bin].op = op;
    p->nodes[bin].a = left;
    p->nodes[bin].b = right;
    left = bin;
  }
  return left;
}

/* Returns a node index, -1 for "empty (valid)", or -1 with failed set. */
static int parse_top(parser *p) {
  if (p->n > LU_PATH_EXPR_MAX_BYTES) {
    p->failed = 1;
    p->err = "Path expression is too large";
    return -1;
  }
  p_skip_blanks(p);
  if (p_eof(p)) return -1; /* empty expression (valid) */
  int n = parse_expr(p, 0);
  if (p->failed) return -1;
  p_skip_blanks(p);
  if (!p_eof(p)) {
    return parser_fail(p, "Unexpected trailing characters in path expression");
  }
  return n;
}

/* Flatten the parse tree into prefix-order ops plus left-to-right operands. */
static int expr_flatten(lu_path_expr *e, const parser *p, int root) {
  if (root < 0) return 1;
  const size_t cap = p->nnodes + 2;
  int *stack = (int *)malloc(cap * sizeof(int));
  if (stack == NULL) return 0;
  size_t top = 0;
  stack[top++] = root;
  int ok = 1;
  while (top > 0 && ok) {
    int idx = stack[--top];
    if (idx < 0) continue;
    const pnode *nd = &p->nodes[idx];
    switch (nd->kind) {
      case NODE_PATTERN:
        ok = expr_push_op(e, (uint8_t)LU_PATH_EXPR_OP_PATTERN) &&
             expr_push_pattern(e, &nd->pattern);
        break;
      case NODE_REF:
        ok = expr_push_op(e, (uint8_t)LU_PATH_EXPR_OP_EXPRESSION_REF) &&
             expr_push_ref(e, &nd->ref);
        break;
      case NODE_COMPLEMENT:
        ok = expr_push_op(e, (uint8_t)LU_PATH_EXPR_OP_COMPLEMENT);
        if (ok && top < cap) stack[top++] = nd->a;
        break;
      case NODE_BINOP:
        ok = expr_push_op(e, nd->op);
        if (ok && top < cap) stack[top++] = nd->b;
        if (ok && top < cap) stack[top++] = nd->a;
        break;
      default:
        break;
    }
  }
  free(stack);
  return ok;
}

/* ============================================================
 * Canonical text
 * ============================================================ */

static const char *op_text(uint8_t op) {
  switch (op) {
    case LU_PATH_EXPR_OP_COMPLEMENT: return "~";
    case LU_PATH_EXPR_OP_IMPLIED_UNION: return " ";
    case LU_PATH_EXPR_OP_UNION: return " + ";
    case LU_PATH_EXPR_OP_INTERSECTION: return " & ";
    case LU_PATH_EXPR_OP_DIFFERENCE: return " - ";
    default: return "";
  }
}

static int is_binary_op(uint8_t op) {
  return op == LU_PATH_EXPR_OP_IMPLIED_UNION || op == LU_PATH_EXPR_OP_UNION ||
         op == LU_PATH_EXPR_OP_INTERSECTION || op == LU_PATH_EXPR_OP_DIFFERENCE;
}

/* Serialize one pattern into `out`.
 *
 * The separator decision must be made against THIS pattern's own text (its
 * prefix), exactly like the reference implementation which builds a local
 * `result` string. `out` may already hold surrounding operators, so the last
 * character this pattern emitted is tracked in `last`. */
static void pattern_text(const lu_pattern *p, strbuf *out) {
  sb_put(out, p->prefix, p->prefix_len);
  char last = (p->prefix_len != 0) ? p->prefix[p->prefix_len - 1] : '\0';
  const int prefix_is_abs_root = (p->prefix_len == 1 && p->prefix[0] == '/');
  const size_t end = p->ncomponents;
  for (size_t i = 0; i < end; i++) {
    const lu_component *c = &p->components[i];
    if (c->stretch) {
      if (i == 0 && prefix_is_abs_root) {
        sb_putc(out, '/');
      } else {
        sb_put(out, "//", 2);
      }
      last = '/';
      continue;
    }
    if ((i + 1 == end) && p->match_property) {
      sb_putc(out, '.');
      last = '.';
    } else if (last != '\0' && last != '/') {
      sb_putc(out, '/');
      last = '/';
    }
    if (c->text_len != 0) {
      sb_put(out, c->text, c->text_len);
      last = c->text[c->text_len - 1];
    }
    if (c->predicate_len != 0) {
      sb_putc(out, '{');
      sb_put(out, c->predicate, c->predicate_len);
      sb_putc(out, '}');
      last = '}';
    }
  }
}

static void ref_text(const lu_ref *r, strbuf *out) {
  const int weaker =
      (r->name_len == 1 && r->name[0] == '_' && r->path_len == 0);
  sb_putc(out, '%');
  if (weaker) {
    sb_put(out, "_", 1);
    return;
  }
  sb_put(out, r->path, r->path_len);
  if (r->name_len != 0) {
    sb_putc(out, ':');
    sb_put(out, r->name, r->name_len);
  }
}

/* Iterative serializer over the prefix-op stream.
 *
 * Frames replace recursion: a frame with stage 0 has not emitted its first
 * operand, stage 1 has (binary op text follows), stage 2 is complete.
 * `close_paren` records the grouping decision the parent made. */
static void expr_serialize(const lu_path_expr *e, strbuf *out) {
  if (e->nops == 0) return;

  const uint8_t kNone = 255u; /* emits nothing */
  const size_t cap = e->nops + 2;

  typedef struct {
    uint8_t op;
    int stage;
    int close_paren;
  } sframe;

  sframe *stack = (sframe *)malloc(cap * sizeof(sframe));
  if (stack == NULL) {
    out->oom = 1;
    return;
  }

  size_t top = 0;
  size_t oi = 0, pi = 0, ri = 0;
  size_t peek_op = 0;

  stack[top].op = (oi < e->nops) ? e->ops[oi++] : kNone;
  stack[top].stage = 0;
  stack[top].close_paren = 0;
  top++;

  while (top > 0 && !out->oom) {
    sframe *f = &stack[top - 1];
    /* The parent decides grouping; a grouped frame opens its parenthesis here
     * and closes it when its subtree completes (stage 2). For a Complement the
     * '~' is emitted first (by this frame) and the child carries the group. */
    if (f->stage == 0 && f->close_paren) sb_putc(out, '(');
    if (f->stage == 0) {
      switch (f->op) {
        case LU_PATH_EXPR_OP_PATTERN:
          if (pi < e->npatterns) pattern_text(&e->patterns[pi++], out);
          top--;
          continue;
        case LU_PATH_EXPR_OP_EXPRESSION_REF:
          if (ri < e->nrefs) ref_text(&e->refs[ri++], out);
          top--;
          continue;
        case LU_PATH_EXPR_OP_COMPLEMENT: {
          sb_putc(out, '~');
          peek_op = oi < e->nops ? e->ops[oi] : (size_t)kNone;
          int child_close = is_binary_op((uint8_t)peek_op) ||
                            peek_op == LU_PATH_EXPR_OP_COMPLEMENT;
          if (top >= cap) {
            out->oom = 1;
            break;
          }
          stack[top].op = (peek_op != kNone) ? e->ops[oi++] : kNone;
          stack[top].stage = 0;
          stack[top].close_paren = child_close;
          top++;
          f->stage = 2;
          continue;
        }
        default: { /* binary operator: first operand, then text, then second */
          peek_op = oi < e->nops ? e->ops[oi] : (size_t)kNone;
          int child_close = is_binary_op((uint8_t)peek_op);
          if (top >= cap) {
            out->oom = 1;
            break;
          }
          stack[top].op = (peek_op != kNone) ? e->ops[oi++] : kNone;
          stack[top].stage = 0;
          stack[top].close_paren = child_close;
          top++;
          f->stage = 1;
          continue;
        }
      }
      continue;
    }
    if (f->stage == 1) {
      const char *txt = op_text(f->op);
      sb_put(out, txt, strlen(txt));
      peek_op = oi < e->nops ? e->ops[oi] : (size_t)kNone;
      int child_close = is_binary_op((uint8_t)peek_op);
      if (top >= cap) {
        out->oom = 1;
        break;
      }
      stack[top].op = (peek_op != kNone) ? e->ops[oi++] : kNone;
      stack[top].stage = 0;
      stack[top].close_paren = child_close;
      top++;
      f->stage = 2;
      continue;
    }
    /* stage == 2: this subtree is complete */
    if (f->close_paren) sb_putc(out, ')');
    top--;
  }

  free(stack);
}

/* ============================================================
 * Matcher
 * ============================================================ */

typedef struct {
  const char *p;
  size_t n;
} cstr;

static int glob_match(const char *pattern, size_t pn, const char *text,
                      size_t tn) {
  size_t p = 0, t = 0;
  size_t star = (size_t)-1;
  size_t retry = 0;
  while (t < tn) {
    if (p < pn && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pn && pattern[p] == '*') {
      star = p++;
      retry = t;
    } else if (star != (size_t)-1) {
      p = star + 1;
      t = ++retry;
    } else {
      return 0;
    }
  }
  while (p < pn && pattern[p] == '*') ++p;
  return p == pn;
}

typedef struct {
  int absolute;
  cstr *segments;
  size_t nsegments;
  int has_property;
  cstr property;
} path_parts;

/* `segbuf` must have room for `n + 2` entries. */
static void split_path(const char *path, size_t n, cstr *segbuf, size_t segcap,
                       path_parts *out) {
  memset(out, 0, sizeof(*out));
  out->segments = segbuf;
  if (n == 0 || segcap == 0) return;

  size_t last_slash = (size_t)-1;
  for (size_t i = n; i > 0; i--) {
    if (path[i - 1] == '/') {
      last_slash = i - 1;
      break;
    }
  }
  const char *tail =
      (last_slash == (size_t)-1) ? path : path + last_slash + 1;
  size_t tail_len = (last_slash == (size_t)-1) ? n : n - last_slash - 1;

  size_t dot = (size_t)-1;
  for (size_t i = 0; i < tail_len; i++) {
    if (tail[i] == '.') {
      dot = i;
      break;
    }
  }
  size_t prim_len = n;
  if (dot != (size_t)-1 && dot > 0) {
    out->has_property = 1;
    out->property.p = tail + dot + 1;
    out->property.n = tail_len - dot - 1;
    prim_len = (last_slash == (size_t)-1) ? dot : last_slash + 1 + dot;
  }

  out->absolute = (prim_len > 0 && path[0] == '/');
  size_t i = out->absolute ? 1 : 0;
  size_t start = i;
  for (; i < prim_len; i++) {
    if (path[i] == '/') {
      if (i > start && out->nsegments < segcap) {
        out->segments[out->nsegments].p = path + start;
        out->segments[out->nsegments].n = i - start;
        out->nsegments++;
      }
      start = i + 1;
    }
  }
  if (prim_len > start && out->nsegments < segcap) {
    out->segments[out->nsegments].p = path + start;
    out->segments[out->nsegments].n = prim_len - start;
    out->nsegments++;
  }
}

typedef struct {
  int stretch;
  cstr text;     /* glob text; exact for prefix segments */
  cstr predicate; /* empty when absent */
  int has_pred;
} match_elem;

typedef struct {
  size_t ei, si, k;
  int started;
} mframe;

/* Number of literal segments in a prefix path string. */
static size_t prefix_segment_count(const char *prefix, size_t prefix_len) {
  size_t count = 0;
  size_t i = (prefix_len > 0 && prefix[0] == '/') ? 1 : 0;
  size_t start = i;
  for (; i <= prefix_len; i++) {
    if (i == prefix_len || prefix[i] == '/') {
      if (i > start) count++;
      start = i + 1;
    }
  }
  return count;
}

static int match_pattern_impl(const lu_pattern *pat, lu_view path,
                              const lu_path_expr_callbacks *cb, void *user) {
  const size_t segcap = path.len + 2;
  cstr *segscratch = (cstr *)malloc(segcap * sizeof(cstr));
  if (segscratch == NULL) return 0;

  path_parts pp;
  split_path(path.data, path.len, segscratch, segcap, &pp);

  const size_t ncomp = pat->ncomponents;
  const size_t prim_comp_end =
      (pat->match_property && ncomp > 0) ? ncomp - 1 : ncomp;
  const size_t max_elems = prefix_segment_count(pat->prefix, pat->prefix_len) +
                           prim_comp_end;

  int result = 0;
  if (max_elems <= LU_PATH_EXPR_MAX_MATCH_ELEMS &&
      pp.nsegments <= LU_PATH_EXPR_MAX_MATCH_SEGMENTS) {
    match_elem *elems = (match_elem *)malloc(
        max_elems != 0 ? max_elems * sizeof(match_elem) : 1);
    if (elems != NULL) {
      size_t nelems = 0;
      {
        size_t i = (pat->prefix_len > 0 && pat->prefix[0] == '/') ? 1 : 0;
        size_t start = i;
        for (; i <= pat->prefix_len; i++) {
          if (i == pat->prefix_len || pat->prefix[i] == '/') {
            if (i > start) {
              elems[nelems].stretch = 0;
              elems[nelems].text.p = pat->prefix + start;
              elems[nelems].text.n = i - start;
              elems[nelems].predicate.p = NULL;
              elems[nelems].predicate.n = 0;
              elems[nelems].has_pred = 0;
              nelems++;
            }
            start = i + 1;
          }
        }
      }
      for (size_t i = 0; i < prim_comp_end && nelems < max_elems; i++) {
        const lu_component *c = &pat->components[i];
        elems[nelems].stretch = c->stretch;
        elems[nelems].text.p = c->text;
        elems[nelems].text.n = c->text_len;
        elems[nelems].predicate.p = c->predicate;
        elems[nelems].predicate.n = c->predicate_len;
        elems[nelems].has_pred = c->predicate_len != 0;
        nelems++;
      }

      /* Property gating. */
      int ok = 1;
      if (pat->match_property) {
        if (!pp.has_property ||
            !glob_match(pat->property_name, pat->property_len, pp.property.p,
                        pp.property.n)) {
          ok = 0;
        }
      } else if (pp.has_property) {
        ok = 0;
      }

      /* Absolute-ness must agree (collections use absolute paths). */
      const int pat_absolute =
          (pat->prefix_len > 0 && pat->prefix[0] == '/');
      if (ok && pat_absolute != pp.absolute) ok = 0;

      /* Precompute prim paths for predicate evaluation. */
      char *prim_buf = NULL;
      size_t *prim_off = NULL;
      if (ok) {
        size_t total = 0;
        for (size_t i = 0; i < pp.nsegments; i++) {
          total += pp.segments[i].n + 1;
        }
        prim_buf = (char *)malloc(total + 1);
        prim_off = (size_t *)malloc((pp.nsegments + 1) * sizeof(size_t));
        if (prim_buf == NULL || prim_off == NULL) {
          ok = 0;
        } else {
          size_t off = 0;
          for (size_t i = 0; i < pp.nsegments; i++) {
            prim_buf[off++] = '/';
            memcpy(prim_buf + off, pp.segments[i].p, pp.segments[i].n);
            off += pp.segments[i].n;
            prim_off[i] = off;
          }
        }
      }

      const size_t width = pp.nsegments + 1;
      const size_t cells = width * (nelems + 1);
      if (ok && cells > 0 && cells <= LU_PATH_EXPR_MAX_MEMO_CELLS) {
        uint8_t *memo = (uint8_t *)calloc(cells, 1);
        mframe *stk = (mframe *)malloc((nelems + 2) * sizeof(mframe));
        if (memo != NULL && stk != NULL) {
          size_t top = 0;
          stk[top].ei = 0;
          stk[top].si = 0;
          stk[top].k = 0;
          stk[top].started = 0;
          top++;

          while (top > 0) {
            mframe *f = &stk[top - 1];
            const size_t ei = f->ei, si = f->si;
            uint8_t *cell = &memo[ei * width + si];
            if (*cell != 0) {
              top--;
              continue;
            }
            if (ei == nelems) {
              *cell = (si == pp.nsegments) ? 2 : 1;
              top--;
              continue;
            }
            const match_elem *e = &elems[ei];
            if (e->stretch) {
              if (!f->started) {
                f->k = si;
                f->started = 1;
              }
              int pushed = 0;
              while (f->k <= pp.nsegments) {
                uint8_t cv = memo[(ei + 1) * width + f->k];
                if (cv == 2) {
                  *cell = 2;
                  break;
                }
                if (cv != 0) {
                  f->k++;
                  continue;
                }
                stk[top].ei = ei + 1;
                stk[top].si = f->k;
                stk[top].k = 0;
                stk[top].started = 0;
                top++;
                pushed = 1;
                break;
              }
              if (pushed) continue;
              if (*cell == 0) *cell = 1;
              top--;
              continue;
            }
            if (!f->started) {
              f->started = 1;
              if (si >= pp.nsegments) {
                *cell = 1;
                top--;
                continue;
              }
              if (e->text.n != 0 &&
                  !glob_match(e->text.p, e->text.n, pp.segments[si].p,
                              pp.segments[si].n)) {
                *cell = 1;
                top--;
                continue;
              }
              if (e->has_pred && cb != NULL && cb->eval_predicate != NULL) {
                lu_view pred;
                pred.data = e->predicate.p;
                pred.len = e->predicate.n;
                lu_view prim;
                prim.data = prim_buf;
                prim.len = prim_off[si];
                if (!cb->eval_predicate(user, pred, prim)) {
                  *cell = 1;
                  top--;
                  continue;
                }
              }
            }
            if (si + 1 < width) {
              uint8_t cv = memo[(ei + 1) * width + (si + 1)];
              if (cv != 0) {
                *cell = cv;
                top--;
                continue;
              }
              stk[top].ei = ei + 1;
              stk[top].si = si + 1;
              stk[top].k = 0;
              stk[top].started = 0;
              top++;
              continue;
            }
            *cell = 1;
            top--;
          }
          result = (memo[0] == 2);
        }
        free(memo);
        free(stk);
      }

      free(prim_buf);
      free(prim_off);
      free(elems);
    }
  }

  free(segscratch);
  return result;
}

/* ============================================================
 * Evaluator (set algebra over the prefix-op stream)
 * ============================================================ */

typedef struct {
  const lu_path_expr *e;
  const char *path;
  size_t path_len;
  const lu_path_expr_callbacks *cb;
  void *user;
  int depth;
  size_t oi, pi, ri;
} eval_ctx;

static int eval_rec(eval_ctx *st) {
  const lu_path_expr *e = st->e;
  if (e == NULL || st->oi >= e->nops) return 0;
  uint8_t op = e->ops[st->oi++];
  switch (op) {
    case LU_PATH_EXPR_OP_PATTERN: {
      if (st->pi >= e->npatterns) return 0;
      const lu_pattern *pat = &e->patterns[st->pi++];
      lu_view pv;
      pv.data = st->path;
      pv.len = st->path_len;
      return match_pattern_impl(pat, pv, st->cb, st->user);
    }
    case LU_PATH_EXPR_OP_EXPRESSION_REF: {
      if (st->ri >= e->nrefs) return 0;
      const lu_ref *r = &e->refs[st->ri++];
      if (st->cb == NULL || st->cb->resolve_ref == NULL || st->depth <= 0) {
        return 0;
      }
      const int weaker =
          (r->name_len == 1 && r->name[0] == '_' && r->path_len == 0);
      lu_view rp, rn;
      rp.data = r->path;
      rp.len = r->path_len;
      rn.data = r->name;
      rn.len = r->name_len;
      const lu_path_expr *sub = st->cb->resolve_ref(st->user, rp, rn, weaker);
      if (sub == NULL) return 0;

      eval_ctx nested;
      memset(&nested, 0, sizeof(nested));
      nested.e = sub;
      nested.path = st->path;
      nested.path_len = st->path_len;
      nested.cb = st->cb;
      nested.user = st->user;
      nested.depth = st->depth - 1;
      return eval_rec(&nested);
    }
    case LU_PATH_EXPR_OP_COMPLEMENT:
      return eval_rec(st) ? 0 : 1;
    case LU_PATH_EXPR_OP_UNION:
    case LU_PATH_EXPR_OP_IMPLIED_UNION: {
      int a = eval_rec(st);
      int b = eval_rec(st);
      return (a || b) ? 1 : 0;
    }
    case LU_PATH_EXPR_OP_INTERSECTION: {
      int a = eval_rec(st);
      int b = eval_rec(st);
      return (a && b) ? 1 : 0;
    }
    case LU_PATH_EXPR_OP_DIFFERENCE: {
      int a = eval_rec(st);
      int b = eval_rec(st);
      return (a && !b) ? 1 : 0;
    }
    default:
      return 0;
  }
}

/* ============================================================
 * Public API
 * ============================================================ */

lu_view lu_view_from_cstr(const char *s) {
  lu_view v;
  v.data = s;
  v.len = (s == NULL) ? 0 : strlen(s);
  return v;
}

static void copy_msg(char *buf, size_t cap, const char *msg) {
  if (buf == NULL || cap == 0) return;
  size_t n = strlen(msg);
  if (n >= cap) n = cap - 1;
  memcpy(buf, msg, n);
  buf[n] = '\0';
}

int lu_path_expr_parse(lu_view text, lu_path_expr **out, char *err_buf,
                       size_t err_cap) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  *out = NULL;
  if (text.data == NULL && text.len != 0) return LU_ERR_INVALID_ARG;
  if (text.len > LU_PATH_EXPR_MAX_BYTES) {
    copy_msg(err_buf, err_cap, "Path expression is too large");
    return LU_ERR_LIMIT;
  }

  lu_path_expr *e = expr_new();
  if (e == NULL) {
    copy_msg(err_buf, err_cap, "Out of memory");
    return LU_ERR_OUT_OF_MEMORY;
  }

  parser p;
  memset(&p, 0, sizeof(p));
  p.s = text.data;
  p.n = text.len;

  int root = parse_top(&p);
  if (p.failed || !expr_flatten(e, &p, root)) {
    parser_free_nodes(&p);
    expr_set_error(e, p.err != NULL ? p.err : "Out of memory");
    e->valid = 0;
  } else {
    parser_free_nodes(&p);
    e->valid = 1;
  }

  copy_msg(err_buf, err_cap, e->err != NULL ? e->err : "");

  *out = e;
  return LU_OK;
}

void lu_path_expr_destroy(lu_path_expr *expr) { expr_destroy(expr); }

int lu_path_expr_clone(const lu_path_expr *expr, lu_path_expr **out) {
  if (out == NULL || expr == NULL) return LU_ERR_INVALID_ARG;
  *out = NULL;
  lu_path_expr *e = expr_new();
  if (e == NULL) return LU_ERR_OUT_OF_MEMORY;
  e->valid = expr->valid;
  if (expr->err != NULL) {
    e->err = lu_dup(expr->err, strlen(expr->err));
    if (e->err == NULL) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
  }
  if (expr->nops != 0) {
    e->ops = (uint8_t *)malloc(expr->nops);
    if (e->ops == NULL) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
    memcpy(e->ops, expr->ops, expr->nops);
    e->nops = expr->nops;
    e->ops_cap = expr->nops;
  }
  for (size_t i = 0; i < expr->npatterns; i++) {
    if (!expr_reserve_patterns(e)) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
    if (!pattern_clone(&e->patterns[e->npatterns], &expr->patterns[i])) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
    e->npatterns++;
  }
  for (size_t i = 0; i < expr->nrefs; i++) {
    if (!expr_reserve_refs(e)) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
    if (!ref_clone(&e->refs[e->nrefs], &expr->refs[i])) {
      expr_destroy(e);
      return LU_ERR_OUT_OF_MEMORY;
    }
    e->nrefs++;
  }
  *out = e;
  return LU_OK;
}

int lu_path_expr_valid(const lu_path_expr *expr) {
  if (expr == NULL) return 0;
  return expr->valid != 0;
}

int lu_path_expr_empty(const lu_path_expr *expr) {
  if (expr == NULL) return 1;
  return expr->nops == 0;
}

int lu_path_expr_op_count(const lu_path_expr *expr, size_t *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  *out = expr->nops;
  return LU_OK;
}

int lu_path_expr_op_at(const lu_path_expr *expr, size_t index,
                       lu_path_expr_op *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= expr->nops) return LU_ERR_INVALID_ARG;
  *out = (lu_path_expr_op)expr->ops[index];
  return LU_OK;
}

int lu_path_expr_pattern_count(const lu_path_expr *expr, size_t *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  *out = expr->npatterns;
  return LU_OK;
}

int lu_path_expr_ref_count(const lu_path_expr *expr, size_t *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  *out = expr->nrefs;
  return LU_OK;
}

int lu_path_expr_pattern_at(const lu_path_expr *expr, size_t index,
                            lu_pattern_view *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= expr->npatterns) return LU_ERR_INVALID_ARG;
  const lu_pattern *p = &expr->patterns[index];
  out->prefix.data = p->prefix;
  out->prefix.len = p->prefix_len;
  out->component_count = p->ncomponents;
  out->match_property = p->match_property;
  out->property_name.data = p->property_name;
  out->property_name.len = p->property_len;
  return LU_OK;
}

int lu_path_expr_component_at(const lu_path_expr *expr, size_t index,
                              size_t component, lu_component_view *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= expr->npatterns) return LU_ERR_INVALID_ARG;
  const lu_pattern *p = &expr->patterns[index];
  if (component >= p->ncomponents) return LU_ERR_INVALID_ARG;
  const lu_component *c = &p->components[component];
  out->text.data = c->text;
  out->text.len = c->text_len;
  out->predicate.data = c->predicate;
  out->predicate.len = c->predicate_len;
  out->is_stretch = c->stretch;
  return LU_OK;
}

int lu_path_expr_ref_at(const lu_path_expr *expr, size_t index,
                        lu_ref_view *out) {
  if (expr == NULL || out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= expr->nrefs) return LU_ERR_INVALID_ARG;
  const lu_ref *r = &expr->refs[index];
  out->path.data = r->path;
  out->path.len = r->path_len;
  out->name.data = r->name;
  out->name.len = r->name_len;
  out->is_weaker =
      (r->name_len == 1 && r->name[0] == '_' && r->path_len == 0);
  return LU_OK;
}

int lu_path_expr_get_text(const lu_path_expr *expr, char *buf, size_t cap,
                          size_t *out_len) {
  if (expr == NULL) return LU_ERR_INVALID_ARG;
  if (!expr->valid) return LU_ERR_INVALID_ARG;
  if (out_len != NULL) *out_len = 0;

  strbuf sb;
  if (!sb_init(&sb)) return LU_ERR_OUT_OF_MEMORY;
  expr_serialize(expr, &sb);
  if (sb.oom) {
    sb_free(&sb);
    return LU_ERR_OUT_OF_MEMORY;
  }
  const size_t need = sb.len;
  if (buf == NULL && cap == 0) {
    if (out_len != NULL) *out_len = need;
    sb_free(&sb);
    return LU_OK;
  }
  if (buf == NULL || need + 1 < need || cap < need + 1) {
    if (out_len != NULL) *out_len = need;
    sb_free(&sb);
    return LU_ERR_BUFFER;
  }
  memcpy(buf, sb.buf, need);
  buf[need] = '\0';
  if (out_len != NULL) *out_len = need;
  sb_free(&sb);
  return LU_OK;
}

int lu_path_expr_match_pattern(const lu_path_expr *expr, size_t index,
                               lu_view path,
                               const lu_path_expr_callbacks *cb, void *user) {
  if (expr == NULL || index >= expr->npatterns) return 0;
  if (path.data == NULL && path.len != 0) return 0;
  return match_pattern_impl(&expr->patterns[index], path, cb, user);
}

int lu_path_expr_match_path(const lu_path_expr *expr, lu_view path,
                            const lu_path_expr_callbacks *cb, void *user) {
  if (expr == NULL) return 0;
  if (!expr->valid || expr->nops == 0) return 0;
  if (expr->nops > LU_PATH_EXPR_MAX_EVAL_OPS) return 0;
  if (path.data == NULL && path.len != 0) return 0;

  int depth = 32;
  if (cb != NULL && cb->max_ref_depth > 0) depth = cb->max_ref_depth;

  eval_ctx st;
  memset(&st, 0, sizeof(st));
  st.e = expr;
  st.path = path.data;
  st.path_len = path.len;
  st.cb = cb;
  st.user = user;
  st.depth = depth;
  return eval_rec(&st);
}
