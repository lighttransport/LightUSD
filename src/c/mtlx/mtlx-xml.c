/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX XML (C11):
 *  - lu_mtlx_parse_xml: port of ParseMtlxXml (+ XmlCursor, DecodeEntities,
 *    ReadStartTag) of src/core/mtlx-resolve.cc.
 *  - lu_mtlx_xmlnode_normalize / lu_mtlx_write_xml: port of the XMLNode
 *    round trip of src/core/mtlx-dom.cc (MtlxElemToXMLNode,
 *    XMLNodeToMtlxElem, AppendXmlEscaped, AppendXmlNode, GetResolvedXML).
 *    XMLNode keeps attributes in a std::map: sorted by key (byte order),
 *    a repeated key keeps the last value.
 *  - lu_mtlx_parse_xml_checked: port of MaterialXParser::Parse
 *    (src/mtlx-xml-parser.cc) over XMLTokenizer (src/mtlx-xml-tokenizer.cc),
 *    with the recursive XMLDocument::ParseNode turned into an explicit
 *    frame stack. Text content is not kept (XMLNodeToMtlxElem drops it).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

static int has_nul(const char *s, size_t n) { return n && memchr(s, 0, n) != NULL; }

/* ======================================================================== */
/* ParseMtlxXml                                                             */
/* ======================================================================== */

static int x_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int x_is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* XmlCursor */
typedef struct xml_cursor {
  const char *src;
  size_t n, pos;
} xml_cursor;

static int cur_at_end(const xml_cursor *c) { return c->pos >= c->n; }
static char cur_peek(const xml_cursor *c) { return c->pos < c->n ? c->src[c->pos] : '\0'; }
static int cur_starts_with(const xml_cursor *c, const char *p) {
  const size_t k = strlen(p);
  return c->pos + k <= c->n && memcmp(c->src + c->pos, p, k) == 0;
}
static void cur_skip_ws(xml_cursor *c) {
  while (!cur_at_end(c) && x_is_space(c->src[c->pos])) c->pos++;
}
/* XmlCursor::SkipUntil */
static void cur_skip_until(xml_cursor *c, const char *p) {
  const size_t k = strlen(p);
  for (size_t i = c->pos; i + k <= c->n; i++) {
    if (memcmp(c->src + i, p, k) == 0) {
      c->pos = i + k;
      return;
    }
  }
  c->pos = c->n;
}
/* XmlCursor::SkipMisc */
static void cur_skip_misc(xml_cursor *c) {
  for (;;) {
    cur_skip_ws(c);
    if (cur_starts_with(c, "<!--")) cur_skip_until(c, "-->");
    else if (cur_starts_with(c, "<?")) cur_skip_until(c, "?>");
    else if (cur_starts_with(c, "<!")) cur_skip_until(c, ">");
    else if (!cur_at_end(c) && c->src[c->pos] != '<') c->pos++;
    else return;
  }
}
/* XmlCursor::ReadName: returns the start; *len the length. */
static const char *cur_read_name(xml_cursor *c, size_t *len) {
  const size_t b = c->pos;
  while (!cur_at_end(c)) {
    const char ch = c->src[c->pos];
    if (!(x_is_name_char(ch) || ch == ':' || ch == '-' || ch == '.')) break;
    c->pos++;
  }
  *len = c->pos - b;
  return c->src + b;
}

/* DecodeEntities (owned result, NULL on oom). */
static char *decode_entities(const char *s, size_t n) {
  static const struct {
    const char *name;
    char c;
  } kEntities[] = {{"quot;", '"'}, {"amp;", '&'}, {"apos;", '\''}, {"lt;", '<'}, {"gt;", '>'}};
  lu_mtlx_sb out;
  lu_mtlx_sb_init(&out);
  size_t i = 0;
  while (i < n) {
    if (s[i] == '&') {
      int hit = 0;
      for (size_t k = 0; k < sizeof(kEntities) / sizeof(kEntities[0]); k++) {
        const size_t en = strlen(kEntities[k].name);
        if (n - i - 1 >= en && memcmp(s + i + 1, kEntities[k].name, en) == 0) {
          lu_mtlx_sb_putc(&out, kEntities[k].c);
          i += 1 + en;
          hit = 1;
          break;
        }
      }
      if (hit) continue;
    }
    lu_mtlx_sb_putc(&out, s[i]);
    i++;
  }
  if (!out.p && !out.oom) return lu_mtlx_strdup("");
  return lu_mtlx_sb_take(&out);
}

static void err_at(lu_mtlx_sb *err, const char *msg, size_t off) {
  if (!err) return;
  lu_mtlx_sb_set(err, msg);
  lu_mtlx_sb_putu(err, off);
}
static void err_cat(lu_mtlx_sb *err, const char *a, const char *b, size_t bn, const char *c) {
  if (!err) return;
  lu_mtlx_sb_set(err, a);
  lu_mtlx_sb_put(err, b, bn);
  lu_mtlx_sb_puts(err, c);
}

/* ReadStartTag: reads "<tag attr=... " up to '>' or "/>". Returns 1, 0 on a
 * syntax error, -1 on allocation failure. */
static int read_start_tag(xml_cursor *c, lu_mtlx_elem *e, lu_mtlx_sb *err) {
  if (cur_peek(c) != '<') {
    err_at(err, "expected '<' at offset ", c->pos);
    return 0;
  }
  c->pos++;
  size_t tn;
  const char *tag = cur_read_name(c, &tn);
  if (tn == 0) {
    err_at(err, "bad element name at offset ", c->pos);
    return 0;
  }
  char *t = lu_mtlx_strndup(tag, tn);
  if (!t) return -1;
  const int ok = lu_mtlx_elem_set_tag(e, t);
  free(t);
  if (!ok) return -1;
  for (;;) {
    cur_skip_ws(c);
    if (cur_at_end(c)) {
      lu_mtlx_err(err, "unexpected end in tag");
      return 0;
    }
    if (cur_peek(c) == '>' || cur_peek(c) == '/') return 1;
    size_t kn;
    const char *key = cur_read_name(c, &kn);
    if (kn == 0) {
      err_at(err, "bad attribute at offset ", c->pos);
      return 0;
    }
    cur_skip_ws(c);
    if (cur_peek(c) != '=') {
      err_cat(err, "expected '=' after attribute ", key, kn, "");
      return 0;
    }
    c->pos++;
    cur_skip_ws(c);
    const char q = cur_peek(c);
    if (q != '"' && q != '\'') {
      err_cat(err, "expected quoted value for attribute ", key, kn, "");
      return 0;
    }
    c->pos++;
    const char *close = (const char *)memchr(c->src + c->pos, q, c->n - c->pos);
    if (!close) {
      lu_mtlx_err(err, "unterminated attribute value");
      return 0;
    }
    char *k = lu_mtlx_strndup(key, kn);
    char *v = decode_entities(c->src + c->pos, (size_t)(close - (c->src + c->pos)));
    const int ok2 = k && v && lu_mtlx_attr_append(e, k, v);
    free(k);
    free(v);
    if (!ok2) return -1;
    c->pos = (size_t)(close - c->src) + 1;
  }
}

#define kMaxXmlDepth 1024

/* Open-element stack of ParseMtlxXml. */
typedef struct elem_stack {
  lu_mtlx_elem *v;
  size_t n, cap;
} elem_stack;

static int estack_push(elem_stack *s, lu_mtlx_elem *e) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 16;
    lu_mtlx_elem *v = (lu_mtlx_elem *)realloc(s->v, cap * sizeof(*v));
    if (!v) return 0;
    s->v = v;
    s->cap = cap;
  }
  lu_mtlx_elem_move(&s->v[s->n++], e);
  return 1;
}

static void estack_free(elem_stack *s) {
  for (size_t i = 0; i < s->n; i++) lu_mtlx_elem_free(&s->v[i]);
  free(s->v);
}

/* Moves `e` into the parent on top of `open`, or into *result. */
static int attach_closed(elem_stack *open, lu_mtlx_elem *e, lu_mtlx_elem *result, int *done) {
  if (open->n == 0) {
    lu_mtlx_elem_move(result, e);
    *done = 1;
    return 1;
  }
  return lu_mtlx_child_push(&open->v[open->n - 1], e) != NULL;
}

/* The end-tag run of ParseMtlxXml. Returns 1, 0 (error), -1 (oom). */
static int read_end_tags(xml_cursor *c, elem_stack *open, lu_mtlx_elem *result, int *done,
                         lu_mtlx_sb *err) {
  while (!*done && open->n) {
    lu_mtlx_elem *top = &open->v[open->n - 1];
    cur_skip_misc(c);
    if (cur_at_end(c)) {
      err_cat(err, "unterminated element <", top->tag, strlen(top->tag), ">");
      return 0;
    }
    if (!cur_starts_with(c, "</")) break;
    c->pos += 2;
    size_t cn;
    const char *close = cur_read_name(c, &cn);
    if (cn != strlen(top->tag) || memcmp(close, top->tag, cn) != 0) {
      err_cat(err, "mismatched end tag for <", top->tag, strlen(top->tag), ">");
      return 0;
    }
    cur_skip_ws(c);
    if (cur_peek(c) != '>') {
      err_cat(err, "malformed end tag for <", top->tag, strlen(top->tag), ">");
      return 0;
    }
    c->pos++;
    lu_mtlx_elem closed;
    lu_mtlx_elem_move(&closed, top);
    open->n--;
    if (!attach_closed(open, &closed, result, done)) return -1;
  }
  return 1;
}

/* ParseMtlxXml body; result is filled on success. */
static int parse_xml_body(xml_cursor *c, elem_stack *open, lu_mtlx_elem *result, lu_mtlx_sb *err) {
  int done = 0;
  while (!done) {
    lu_mtlx_elem e;
    lu_mtlx_elem_init(&e);
    int r = read_start_tag(c, &e, err);
    if (r <= 0) {
      lu_mtlx_elem_free(&e);
      return r;
    }
    int leaf = 0;
    if (cur_starts_with(c, "/>")) {
      c->pos += 2;
      leaf = 1;
    } else if (cur_peek(c) == '>') {
      c->pos++;
    } else {
      err_cat(err, "malformed start tag <", e.tag, strlen(e.tag), ">");
      lu_mtlx_elem_free(&e);
      return 0;
    }
    if (leaf) {
      if (!attach_closed(open, &e, result, &done)) return -1;
    } else {
      if (open->n >= kMaxXmlDepth) {
        lu_mtlx_err(err, "maximum element depth exceeded");
        lu_mtlx_elem_free(&e);
        return 0;
      }
      if (!estack_push(open, &e)) {
        lu_mtlx_elem_free(&e);
        return -1;
      }
    }
    r = read_end_tags(c, open, result, &done, err);
    if (r <= 0) return r;
  }
  cur_skip_misc(c);
  if (!cur_at_end(c)) {
    lu_mtlx_err(err, "content after root element");
    return 0;
  }
  return 1;
}

/* ParseMtlxXml */
int lu_mtlx_parse_xml(const char *xml, size_t len, lu_mtlx_elem *root, lu_mtlx_sb *err) {
  if (!root) return 0;
  if (!xml) len = 0;
  if (has_nul(xml, len)) {
    lu_mtlx_err(err, "NUL byte in MaterialX XML");
    return 0;
  }
  xml_cursor c = {xml ? xml : "", len, 0};
  cur_skip_misc(&c);
  if (cur_at_end(&c)) {
    lu_mtlx_err(err, "no root element");
    return 0;
  }
  elem_stack open = {NULL, 0, 0};
  lu_mtlx_elem result;
  lu_mtlx_elem_init(&result);
  const int r = parse_xml_body(&c, &open, &result, err);
  estack_free(&open);
  if (r <= 0) {
    if (r < 0) lu_mtlx_err(err, "out of memory");
    lu_mtlx_elem_free(&result);
    return 0;
  }
  lu_mtlx_elem_free(root);
  lu_mtlx_elem_move(root, &result);
  return 1;
}

/* ======================================================================== */
/* XMLNode round trip and GetResolvedXML                                    */
/* ======================================================================== */

static int cmp_attr_key(const void *a, const void *b) {
  return strcmp(((const lu_mtlx_attr *)a)->key, ((const lu_mtlx_attr *)b)->key);
}

/* std::map attribute semantics on one element: repeated keys keep the last
 * value, then sort by key. */
static void map_attrs(lu_mtlx_elem *e) {
  size_t w = 0;
  for (size_t i = 0; i < e->nattrs; i++) {
    int later = 0;
    for (size_t j = i + 1; j < e->nattrs && !later; j++)
      later = strcmp(e->attrs[i].key, e->attrs[j].key) == 0;
    if (later) {
      free(e->attrs[i].key);
      free(e->attrs[i].value);
    } else {
      e->attrs[w++] = e->attrs[i];
    }
  }
  e->nattrs = w;
  if (w > 1) qsort(e->attrs, w, sizeof(lu_mtlx_attr), cmp_attr_key);
}

/* XMLNodeToMtlxElem(MtlxElemToXMLNode(root)), in place. */
int lu_mtlx_xmlnode_normalize(lu_mtlx_elem *root) {
  if (!root) return 0;
  lu_mtlx_elem **stack = NULL;
  size_t n = 0, cap = 0;
  lu_mtlx_elem *e = root;
  for (;;) {
    map_attrs(e);
    if (n + e->nchildren > cap) {
      size_t nc = cap ? cap * 2 : 64;
      while (nc < n + e->nchildren) nc *= 2;
      lu_mtlx_elem **v = (lu_mtlx_elem **)realloc(stack, nc * sizeof(*v));
      if (!v) {
        free(stack);
        return 0;
      }
      stack = v;
      cap = nc;
    }
    for (size_t i = 0; i < e->nchildren; i++) stack[n++] = &e->children[i];
    if (!n) break;
    e = stack[--n];
  }
  free(stack);
  return 1;
}

/* AppendXmlEscaped */
static void append_escaped(lu_mtlx_sb *out, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&': lu_mtlx_sb_puts(out, "&amp;"); break;
      case '<': lu_mtlx_sb_puts(out, "&lt;"); break;
      case '>': lu_mtlx_sb_puts(out, "&gt;"); break;
      case '"': lu_mtlx_sb_puts(out, "&quot;"); break;
      case '\'': lu_mtlx_sb_puts(out, "&apos;"); break;
      default: lu_mtlx_sb_putc(out, *s); break;
    }
  }
}

/* The start tag of AppendXmlNode with MtlxElemToXMLNode's attribute map. */
static int append_start_tag(lu_mtlx_sb *out, const lu_mtlx_elem *e) {
  /* Shallow view of the surviving attributes, sorted. */
  lu_mtlx_attr *view = NULL;
  size_t nv = 0;
  if (e->nattrs) {
    view = (lu_mtlx_attr *)malloc(e->nattrs * sizeof(*view));
    if (!view) return 0;
    for (size_t i = 0; i < e->nattrs; i++) {
      int later = 0;
      for (size_t j = i + 1; j < e->nattrs && !later; j++)
        later = strcmp(e->attrs[i].key, e->attrs[j].key) == 0;
      if (!later) view[nv++] = e->attrs[i];
    }
    if (nv > 1) qsort(view, nv, sizeof(*view), cmp_attr_key);
  }
  lu_mtlx_sb_putc(out, '<');
  lu_mtlx_sb_puts(out, e->tag);
  for (size_t i = 0; i < nv; i++) {
    lu_mtlx_sb_putc(out, ' ');
    lu_mtlx_sb_puts(out, view[i].key);
    lu_mtlx_sb_puts(out, "=\"");
    append_escaped(out, view[i].value);
    lu_mtlx_sb_putc(out, '"');
  }
  free(view);
  if (e->nchildren == 0) {
    lu_mtlx_sb_puts(out, "/>");
  } else {
    lu_mtlx_sb_putc(out, '>');
  }
  return 1;
}

typedef struct write_frame {
  const lu_mtlx_elem *e;
  size_t next;
} write_frame;

/* GetResolvedXML: "<?xml version=\"1.0\"?>" + AppendXmlNode(MtlxElemToXMLNode(root)).
 * Appends to out. */
int lu_mtlx_write_xml(const lu_mtlx_elem *root, lu_mtlx_sb *out) {
  if (!root || !out) return 0;
  lu_mtlx_sb_puts(out, "<?xml version=\"1.0\"?>");
  write_frame *st = NULL;
  size_t n = 0, cap = 0;
  int ok = 1;
  const lu_mtlx_elem *e = root;
  for (;;) {
    if (e) {
      if (!append_start_tag(out, e)) { ok = 0; break; }
      if (e->nchildren) {
        if (n == cap) {
          const size_t nc = cap ? cap * 2 : 64;
          write_frame *v = (write_frame *)realloc(st, nc * sizeof(*v));
          if (!v) { ok = 0; break; }
          st = v;
          cap = nc;
        }
        st[n].e = e;
        st[n].next = 0;
        n++;
      }
      e = NULL;
    }
    if (!n) break;
    write_frame *f = &st[n - 1];
    if (f->next < f->e->nchildren) {
      e = &f->e->children[f->next++];
    } else {
      lu_mtlx_sb_puts(out, "</");
      lu_mtlx_sb_puts(out, f->e->tag);
      lu_mtlx_sb_putc(out, '>');
      n--;
    }
  }
  free(st);
  return ok && !out->oom;
}

/* ======================================================================== */
/* XMLTokenizer                                                             */
/* ======================================================================== */

typedef enum tok_type {
  TK_START, TK_END, TK_SELFCLOSE, TK_ATTR, TK_TEXT, TK_COMMENT, TK_PI, TK_CDATA, TK_EOD, TK_ERROR
} tok_type;

/* Token (line/column are not observable and are not tracked). */
typedef struct token {
  tok_type type;
  lu_mtlx_sb name, value;
} token;

#define MAX_NAME_LENGTH 256
#define MAX_STRING_LENGTH (64 * 1024)
#define MAX_TEXT_LENGTH (1024 * 1024)

typedef struct tokenizer {
  const char *data;
  size_t size, pos;
  int in_tag;
  lu_mtlx_sb tag_name; /* current_tag_name_ */
} tokenizer;

static void token_reset(token *t) {
  t->type = TK_START; /* Token() value-initialises type to 0 == StartTag */
  lu_mtlx_sb_clear(&t->name);
  lu_mtlx_sb_clear(&t->value);
}

static char tk_peek(const tokenizer *t, size_t off) {
  return t->pos + off >= t->size ? '\0' : t->data[t->pos + off];
}
static char tk_next(tokenizer *t) { return t->pos >= t->size ? '\0' : t->data[t->pos++]; }
static int tk_match(const tokenizer *t, const char *s) {
  const size_t k = strlen(s);
  return t->pos + k <= t->size && memcmp(t->data + t->pos, s, k) == 0;
}
static int tk_consume(tokenizer *t, const char *s) {
  if (!tk_match(t, s)) return 0;
  t->pos += strlen(s);
  return 1;
}
static void tk_skip_ws(tokenizer *t) {
  while (t->pos < t->size) {
    const char c = tk_peek(t, 0);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') t->pos++;
    else break;
  }
}
static int c_isalpha(char c) { return isalpha((unsigned char)c) != 0; }
static int c_isalnum(char c) { return isalnum((unsigned char)c) != 0; }
static int c_isspace(char c) { return isspace((unsigned char)c) != 0; }

/* XMLTokenizer::ParseName */
static int tk_parse_name(tokenizer *t, lu_mtlx_sb *name) {
  lu_mtlx_sb_clear(name);
  char c = tk_peek(t, 0);
  if (!c_isalpha(c) && c != '_' && c != ':') return 0;
  while (t->pos < t->size && name->n < MAX_NAME_LENGTH) {
    c = tk_peek(t, 0);
    if (c_isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':') lu_mtlx_sb_putc(name, tk_next(t));
    else break;
  }
  if (name->n >= MAX_NAME_LENGTH) return 0;
  return name->n != 0;
}

/* The five predefined entities of ParseQuotedString / ParseText. */
static void tk_entity(tokenizer *t, lu_mtlx_sb *s) {
  static const struct {
    const char *ent;
    char c;
  } k[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
  for (size_t i = 0; i < 5; i++) {
    if (tk_match(t, k[i].ent)) {
      tk_consume(t, k[i].ent);
      lu_mtlx_sb_putc(s, k[i].c);
      return;
    }
  }
  lu_mtlx_sb_putc(s, tk_next(t));
}

/* XMLTokenizer::ParseQuotedString */
static int tk_parse_quoted(tokenizer *t, lu_mtlx_sb *s, char quote) {
  lu_mtlx_sb_clear(s);
  if (tk_peek(t, 0) != quote) return 0;
  tk_next(t);
  while (t->pos < t->size && s->n < MAX_STRING_LENGTH) {
    const char c = tk_peek(t, 0);
    if (c == quote) {
      tk_next(t);
      return 1;
    } else if (c == '&') {
      tk_entity(t, s);
    } else if (c == '\0') {
      return 0;
    } else {
      lu_mtlx_sb_putc(s, tk_next(t));
    }
  }
  return 0;
}

/* XMLTokenizer::ParseUntil */
static int tk_parse_until(tokenizer *t, lu_mtlx_sb *s, const char *delim) {
  lu_mtlx_sb_clear(s);
  while (t->pos < t->size && s->n < MAX_TEXT_LENGTH) {
    if (tk_match(t, delim)) return 1;
    lu_mtlx_sb_putc(s, tk_next(t));
  }
  return 0;
}

/* ParseComment / ParseCDATA */
static int tk_parse_delimited(tokenizer *t, token *tok, const char *open, const char *close,
                              tok_type type) {
  if (!tk_consume(t, open)) return 0;
  tok->type = type;
  if (!tk_parse_until(t, &tok->value, close)) return 0;
  tk_consume(t, close);
  return 1;
}

/* XMLTokenizer::ParseProcessingInstruction */
static int tk_parse_pi(tokenizer *t, token *tok) {
  if (!tk_consume(t, "<?")) return 0;
  tok->type = TK_PI;
  if (!tk_parse_name(t, &tok->name)) return 0;
  tk_skip_ws(t);
  if (!tk_parse_until(t, &tok->value, "?>")) return 0;
  tk_consume(t, "?>");
  return 1;
}

/* XMLTokenizer::ParseEndTag */
static int tk_parse_end_tag(tokenizer *t, token *tok) {
  if (!tk_consume(t, "</")) return 0;
  tok->type = TK_END;
  if (!tk_parse_name(t, &tok->name)) return 0;
  tk_skip_ws(t);
  if (tk_peek(t, 0) != '>') return 0;
  tk_next(t);
  t->in_tag = 0;
  return 1;
}

/* XMLTokenizer::ParseStartTag */
static int tk_parse_start_tag(tokenizer *t, token *tok) {
  if (tk_peek(t, 0) != '<') return 0;
  if (tk_match(t, "<!--")) return tk_parse_delimited(t, tok, "<!--", "-->", TK_COMMENT);
  if (tk_match(t, "<![CDATA[")) return tk_parse_delimited(t, tok, "<![CDATA[", "]]>", TK_CDATA);
  if (tk_match(t, "<?")) return tk_parse_pi(t, tok);
  if (tk_match(t, "</")) return tk_parse_end_tag(t, tok);
  tk_next(t);
  tok->type = TK_START;
  if (!tk_parse_name(t, &tok->name)) return 0;
  lu_mtlx_sb_set(&t->tag_name, lu_mtlx_sb_str(&tok->name));
  t->in_tag = 1;
  return 1;
}

/* XMLTokenizer::ParseAttribute */
static int tk_parse_attribute(tokenizer *t, token *tok) {
  if (!t->in_tag) return 0;
  tk_skip_ws(t);
  if (tk_match(t, "/>")) {
    tk_consume(t, "/>");
    tok->type = TK_SELFCLOSE;
    lu_mtlx_sb_set(&tok->name, lu_mtlx_sb_str(&t->tag_name));
    t->in_tag = 0;
    return 1;
  }
  if (tk_peek(t, 0) == '>') {
    tk_next(t);
    t->in_tag = 0;
    return 0;
  }
  tok->type = TK_ATTR;
  if (!tk_parse_name(t, &tok->name)) return 0;
  tk_skip_ws(t);
  if (tk_peek(t, 0) != '=') {
    lu_mtlx_sb_clear(&tok->value);
    return 1;
  }
  tk_next(t);
  tk_skip_ws(t);
  const char quote = tk_peek(t, 0);
  if (quote != '"' && quote != '\'') return 0;
  return tk_parse_quoted(t, &tok->value, quote);
}

static int tk_next_token(tokenizer *t, token *tok);

/* XMLTokenizer::ParseText. Returns 1/0, or 2 for "skip whitespace-only text
 * and return NextToken" (the tail call is a loop in tk_next_token). */
static int tk_parse_text(tokenizer *t, token *tok) {
  tok->type = TK_TEXT;
  lu_mtlx_sb_clear(&tok->value);
  while (t->pos < t->size && tok->value.n < MAX_TEXT_LENGTH) {
    const char c = tk_peek(t, 0);
    if (c == '<') break;
    if (c == '&') tk_entity(t, &tok->value);
    else lu_mtlx_sb_putc(&tok->value, tk_next(t));
  }
  if (tok->value.n >= MAX_TEXT_LENGTH) return 0;
  int all_ws = 1;
  for (size_t i = 0; i < tok->value.n; i++) {
    if (!c_isspace(tok->value.p[i])) {
      all_ws = 0;
      break;
    }
  }
  if (all_ws && tok->value.n) return 2;
  return tok->value.n != 0;
}

/* XMLTokenizer::NextToken */
static int tk_next_token(tokenizer *t, token *tok) {
  for (;;) {
    token_reset(tok);
    if (t->pos >= t->size) {
      tok->type = TK_EOD;
      return 1;
    }
    if (t->in_tag) {
      if (tk_parse_attribute(t, tok)) return 1;
      tok->type = TK_TEXT;
      lu_mtlx_sb_clear(&tok->value);
      return 1;
    }
    tk_skip_ws(t);
    if (t->pos >= t->size) {
      tok->type = TK_EOD;
      return 1;
    }
    if (tk_peek(t, 0) == '<') return tk_parse_start_tag(t, tok);
    const int r = tk_parse_text(t, tok);
    if (r != 2) return r;
  }
}

/* ======================================================================== */
/* XMLDocument / MaterialXParser                                            */
/* ======================================================================== */

/* Node arena: nodes in creation order; a node's children are the later
 * nodes naming it as parent, in creation order (XMLNode::AddChild order). */
typedef struct xnode_arena {
  lu_mtlx_elem *v;
  size_t *parent;
  size_t n, cap;
} xnode_arena;

#define NO_PARENT ((size_t)-1)

static size_t arena_new(xnode_arena *a, size_t parent, const char *name) {
  if (a->n == a->cap) {
    const size_t cap = a->cap ? a->cap * 2 : 64;
    lu_mtlx_elem *v = (lu_mtlx_elem *)realloc(a->v, cap * sizeof(*v));
    if (!v) return NO_PARENT;
    a->v = v;
    size_t *p = (size_t *)realloc(a->parent, cap * sizeof(*p));
    if (!p) return NO_PARENT;
    a->parent = p;
    a->cap = cap;
  }
  lu_mtlx_elem_init(&a->v[a->n]);
  a->parent[a->n] = parent;
  if (!lu_mtlx_elem_set_tag(&a->v[a->n], name)) {
    return NO_PARENT;
  }
  return a->n++;
}

static void arena_free(xnode_arena *a) {
  for (size_t i = 0; i < a->n; i++) lu_mtlx_elem_free(&a->v[i]);
  free(a->v);
  free(a->parent);
}

/* XMLNode::SetAttribute (attributes_[name] = value). */
static int node_set_attr(lu_mtlx_elem *e, const char *k, const char *v) {
  for (size_t i = 0; i < e->nattrs; i++) {
    if (!strcmp(e->attrs[i].key, k)) {
      char *nv = lu_mtlx_strdup(v);
      if (!nv) return 0;
      free(e->attrs[i].value);
      e->attrs[i].value = nv;
      return 1;
    }
  }
  return lu_mtlx_attr_append(e, k, v);
}

/* XMLNodeToMtlxElem over the arena: node 0 becomes *out. */
static int arena_to_elem(xnode_arena *a, lu_mtlx_elem *out) {
  size_t *nkids = (size_t *)calloc(a->n, sizeof(size_t));
  size_t *ord = (size_t *)calloc(a->n, sizeof(size_t));
  int ok = nkids && ord;
  for (size_t i = 1; ok && i < a->n; i++) ord[i] = nkids[a->parent[i]]++;
  for (size_t i = 0; ok && i < a->n; i++) {
    if (a->v[i].nattrs > 1)
      qsort(a->v[i].attrs, a->v[i].nattrs, sizeof(lu_mtlx_attr), cmp_attr_key);
    if (nkids[i]) {
      a->v[i].children = (lu_mtlx_elem *)malloc(nkids[i] * sizeof(lu_mtlx_elem));
      if (!a->v[i].children) {
        ok = 0;
        break;
      }
      a->v[i].capchildren = nkids[i];
    }
  }
  if (ok) {
    /* Children are complete before their (lower-index) parent moves. */
    for (size_t i = a->n; i-- > 1;) {
      lu_mtlx_elem *p = &a->v[a->parent[i]];
      lu_mtlx_elem_move(&p->children[ord[i]], &a->v[i]);
      p->nchildren++;
    }
    lu_mtlx_elem_move(out, &a->v[0]);
  }
  free(nkids);
  free(ord);
  return ok;
}

/* ParseNode activation: `node`; when it returns, `then_child` (or NO_PARENT)
 * is the child whose own ParseNode the caller runs next (the nested-start-tag
 * path of the attribute loop). */
typedef struct parse_frame {
  size_t node;
  size_t then_child;
} parse_frame;

typedef struct doc_parser {
  tokenizer tk;
  token tok, at;
  xnode_arena arena;
  parse_frame *st;
  size_t nst, capst;
  lu_mtlx_sb error;
  int oom;
} doc_parser;

#define MAX_DEPTH 1000

static int push_frame(doc_parser *d, size_t node, size_t then_child) {
  if (d->nst == d->capst) {
    const size_t cap = d->capst ? d->capst * 2 : 64;
    parse_frame *v = (parse_frame *)realloc(d->st, cap * sizeof(*v));
    if (!v) {
      d->oom = 1;
      return 0;
    }
    d->st = v;
    d->capst = cap;
  }
  d->st[d->nst].node = node;
  d->st[d->nst].then_child = then_child;
  d->nst++;
  /* ParseNode entry: current_depth_ equals the number of active frames. */
  if (d->nst > MAX_DEPTH) {
    lu_mtlx_sb_set(&d->error, "Maximum nesting depth exceeded");
    return 0;
  }
  return 1;
}

static int set_attr_tok(doc_parser *d, size_t node, const token *t) {
  if (!node_set_attr(&d->arena.v[node], lu_mtlx_sb_str(&t->name), lu_mtlx_sb_str(&t->value))) {
    d->oom = 1;
    return 0;
  }
  return 1;
}

/* XMLDocument::ParseAttributes (never fails except out of memory). */
static int parse_attributes(doc_parser *d, size_t node) {
  while (tk_next_token(&d->tk, &d->at)) {
    if (d->at.type != TK_ATTR) break; /* SelfClosingTag returns; others dropped */
    if (!set_attr_tok(d, node, &d->at)) return 0;
  }
  return 1;
}

static int fail_msg(doc_parser *d, const char *a, const char *b, const char *c, const char *e2,
                    const char *f) {
  lu_mtlx_sb_set(&d->error, a);
  lu_mtlx_sb_puts(&d->error, b);
  lu_mtlx_sb_puts(&d->error, c);
  if (e2) lu_mtlx_sb_puts(&d->error, e2);
  if (f) lu_mtlx_sb_puts(&d->error, f);
  return 0;
}

static const char *node_name(const doc_parser *d, size_t node) { return d->arena.v[node].tag; }

/* The StartTag case of XMLDocument::ParseNode for parent frame `parent`:
 * creates the child, runs its attribute loop and pushes the frame(s) the
 * C++ code would recurse into. Returns 0 on failure. */
static int on_start_tag(doc_parser *d, size_t parent) {
  const size_t child = arena_new(&d->arena, parent, lu_mtlx_sb_str(&d->tok.name));
  if (child == NO_PARENT) {
    d->oom = 1;
    return 0;
  }
  int self_closing = 0;
  while (tk_next_token(&d->tk, &d->at)) {
    if (d->at.type == TK_ATTR) {
      if (!set_attr_tok(d, child, &d->at)) return 0;
      continue;
    }
    if (d->at.type == TK_SELFCLOSE) {
      self_closing = 1;
      break;
    }
    if (d->at.type == TK_START) {
      const size_t nested = arena_new(&d->arena, child, lu_mtlx_sb_str(&d->at.name));
      if (nested == NO_PARENT) {
        d->oom = 1;
        return 0;
      }
      if (!parse_attributes(d, nested)) return 0;
      /* ParseNode(nested); then ParseNode(child) (self_closing is false). */
      return push_frame(d, nested, child);
    }
    if (d->at.type == TK_END && strcmp(lu_mtlx_sb_str(&d->at.name), node_name(d, child)) != 0)
      return fail_msg(d, "Mismatched end tag: expected </", node_name(d, child), "> but got </",
                      lu_mtlx_sb_str(&d->at.name), ">");
    break; /* Text (SetText), matching EndTag, anything else */
  }
  if (!self_closing) return push_frame(d, child, NO_PARENT);
  return 1;
}

/* One iteration of the ParseNode loop of the top frame. Returns 1 to go on,
 * 0 on failure. Pops the frame when ParseNode returns true. */
static int parse_node_step(doc_parser *d) {
  parse_frame *f = &d->st[d->nst - 1];
  const size_t node = f->node;
  int ret = 0; /* ParseNode returned true */
  if (!tk_next_token(&d->tk, &d->tok)) {
    ret = 1;
  } else {
    switch (d->tok.type) {
      case TK_START:
        return on_start_tag(d, node);
      case TK_END:
        if (strcmp(lu_mtlx_sb_str(&d->tok.name), node_name(d, node)) != 0)
          return fail_msg(d, "Mismatched end tag: expected </", node_name(d, node), "> but got </",
                          lu_mtlx_sb_str(&d->tok.name), ">");
        ret = 1;
        break;
      case TK_TEXT:
      case TK_CDATA:
      case TK_COMMENT:
        return 1;
      case TK_EOD:
        return fail_msg(d, "Unexpected end of document while parsing <", node_name(d, node), ">",
                        NULL, NULL);
      case TK_ERROR:
        return fail_msg(d, "Tokenizer error", "", "", NULL, NULL);
      default:
        return fail_msg(d, "Unexpected token type at this position", "", "", NULL, NULL);
    }
  }
  (void)ret;
  const size_t then_child = f->then_child;
  d->nst--;
  if (then_child != NO_PARENT) return push_frame(d, then_child, NO_PARENT);
  return 1;
}

/* XMLDocument::ParseMemory. Returns 1 when the arena holds the root. */
static int parse_memory(doc_parser *d, const char *data, size_t size) {
  if (size > (size_t)1024 * 1024 * 100) {
    lu_mtlx_sb_set(&d->error,
                   "Failed to initialize tokenizer: Input size exceeds maximum allowed size");
    return 0;
  }
  d->tk.data = data;
  d->tk.size = size;
  int have_root = 0;
  while (tk_next_token(&d->tk, &d->tok)) {
    if (d->tok.type == TK_PI) continue;
    if (d->tok.type == TK_START) {
      if (arena_new(&d->arena, NO_PARENT, lu_mtlx_sb_str(&d->tok.name)) == NO_PARENT) {
        d->oom = 1;
        return 0;
      }
      have_root = 1;
      if (!parse_attributes(d, 0)) return 0;
      if (!push_frame(d, 0, NO_PARENT)) return 0;
      while (d->nst)
        if (!parse_node_step(d)) return 0;
      break;
    }
    if (d->tok.type == TK_EOD) {
      lu_mtlx_sb_set(&d->error, "No root element found");
      return 0;
    }
  }
  if (!have_root) {
    lu_mtlx_sb_set(&d->error, "Failed to parse root element");
    return 0;
  }
  return 1;
}

static int parser_oom(const doc_parser *d) {
  return d->oom || d->tok.name.oom || d->tok.value.oom || d->at.name.oom || d->at.value.oom ||
         d->tk.tag_name.oom || d->error.oom;
}

static void doc_parser_free(doc_parser *d) {
  lu_mtlx_sb_free(&d->tok.name);
  lu_mtlx_sb_free(&d->tok.value);
  lu_mtlx_sb_free(&d->at.name);
  lu_mtlx_sb_free(&d->at.value);
  lu_mtlx_sb_free(&d->tk.tag_name);
  lu_mtlx_sb_free(&d->error);
  arena_free(&d->arena);
  free(d->st);
}

/* MaterialXParser::ValidateVersion */
static int validate_version(const char *v) {
  return !strcmp(v, "1.39") || !strcmp(v, "1.38") || !strcmp(v, "1.37") || !strcmp(v, "1.36");
}

/* MaterialXParser::Parse + XMLNodeToMtlxElem(GetRoot()). */
int lu_mtlx_parse_xml_checked(const char *xml, size_t len, lu_mtlx_elem *root, lu_mtlx_sb *err,
                              lu_mtlx_sb *warning) {
  if (warning) lu_mtlx_sb_clear(warning);
  if (!root) return 0;
  if (!xml) len = 0;
  if (has_nul(xml, len)) {
    lu_mtlx_err(err, "NUL byte in MaterialX XML");
    return 0;
  }
  doc_parser d;
  memset(&d, 0, sizeof(d));
  int ok = parse_memory(&d, xml ? xml : "", len);
  if (parser_oom(&d)) {
    lu_mtlx_err(err, "out of memory");
    doc_parser_free(&d);
    return 0;
  }
  if (!ok) {
    lu_mtlx_err(err, lu_mtlx_sb_str(&d.error));
    doc_parser_free(&d);
    return 0;
  }
  const lu_mtlx_elem *r = &d.arena.v[0];
  const char *msg = NULL;
  const char *version = lu_mtlx_attr_or(r, "version", "");
  if (strcmp(r->tag, "materialx") != 0) msg = "Root element must be <materialx>";
  else if (!version[0]) msg = "Missing version attribute in <materialx>";
  if (msg) {
    lu_mtlx_err(err, msg);
    doc_parser_free(&d);
    return 0;
  }
  if (warning && !validate_version(version)) {
    lu_mtlx_sb_set(warning, "Unknown MaterialX version: ");
    lu_mtlx_sb_puts(warning, version);
  }
  lu_mtlx_elem result;
  ok = arena_to_elem(&d.arena, &result);
  doc_parser_free(&d);
  if (!ok || (warning && warning->oom)) {
    if (ok) lu_mtlx_elem_free(&result);
    lu_mtlx_err(err, "out of memory");
    return 0;
  }
  lu_mtlx_elem_free(root);
  lu_mtlx_elem_move(root, &result);
  return 1;
}
