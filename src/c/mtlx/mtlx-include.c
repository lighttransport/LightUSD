/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * XInclude expansion (C11 port of ExpandMtlxIncludes with the anchored
 * loader, src/core/mtlx-resolve.cc: AddUnique, ImportLibrary, IncludeFrame,
 * InitFrame). Semantics: see mtlx-resolve.hh.
 */
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

static const char *const kDistributedKeys[] = {"fileprefix", "geomprefix", "colorspace",
                                               "namespace"};
#define kMaxIncludeDepth 64

/* A growable list of elements (std::vector<MtlxElem>). */
typedef struct elem_list {
  lu_mtlx_elem *v;
  size_t n, cap;
} elem_list;

static void list_free(elem_list *l) {
  for (size_t i = 0; i < l->n; i++) lu_mtlx_elem_free(&l->v[i]);
  free(l->v);
  memset(l, 0, sizeof(*l));
}

/* IsInclude */
static int is_include(const lu_mtlx_elem *e) { return strcmp(e->tag, "xi:include") == 0; }

/* AddUnique: `c` is moved in, or freed when a same-named element exists. */
static int add_unique(elem_list *acc, lu_mtlx_elem *c) {
  const char *name = lu_mtlx_name(c);
  for (size_t i = 0; i < acc->n; i++) {
    if (strcmp(lu_mtlx_name(&acc->v[i]), name) == 0) {
      lu_mtlx_elem_free(c);
      return 1;
    }
  }
  if (acc->n == acc->cap) {
    const size_t cap = acc->cap ? acc->cap * 2 : 16;
    lu_mtlx_elem *v = (lu_mtlx_elem *)realloc(acc->v, cap * sizeof(*v));
    if (!v) {
      lu_mtlx_elem_free(c);
      return 0;
    }
    acc->v = v;
    acc->cap = cap;
  }
  lu_mtlx_elem_move(&acc->v[acc->n++], c);
  return 1;
}

/* Distribution + namespace qualification of one library child. */
static int qualify_child(lu_mtlx_elem *c, const lu_mtlx_elem *lib) {
  for (size_t k = 0; k < sizeof(kDistributedKeys) / sizeof(kDistributedKeys[0]); k++) {
    if (lu_mtlx_get_attr(c, kDistributedKeys[k])) continue;
    const char *v = lu_mtlx_get_attr(lib, kDistributedKeys[k]);
    if (v && !lu_mtlx_attr_set(c, kDistributedKeys[k], v)) return 0;
  }
  const char *ns = lu_mtlx_get_attr(c, "namespace");
  if (!ns || !ns[0]) return 1;
  for (size_t i = 0; i < c->nattrs; i++) {
    if (strcmp(c->attrs[i].key, "name") != 0) continue;
    lu_mtlx_sb q;
    lu_mtlx_sb_init(&q);
    lu_mtlx_sb_puts(&q, ns);
    lu_mtlx_sb_putc(&q, ':');
    lu_mtlx_sb_puts(&q, c->attrs[i].value);
    char *nv = lu_mtlx_sb_take(&q);
    if (!nv) return 0;
    free(c->attrs[i].value);
    c->attrs[i].value = nv;
  }
  return 1;
}

/* ImportLibrary: children of an expanded library root, distributed and
 * qualified. `lib` is consumed. */
static int import_library(elem_list *acc, lu_mtlx_elem *lib) {
  int ok = 1;
  for (size_t i = 0; i < lib->nchildren; i++) {
    lu_mtlx_elem *c = &lib->children[i];
    if (ok && qualify_child(c, lib)) {
      lu_mtlx_elem moved;
      lu_mtlx_elem_move(&moved, c);
      ok = add_unique(acc, &moved);
    } else {
      ok = 0;
    }
  }
  lu_mtlx_elem_free(lib);
  return ok;
}

/* IncludeFrame */
typedef struct include_frame {
  lu_mtlx_elem root;
  char *href;
  lu_mtlx_strv incs;
  size_t next;
  elem_list imported;
} include_frame;

static void frame_free(include_frame *f) {
  lu_mtlx_elem_free(&f->root);
  free(f->href);
  lu_mtlx_strv_free(&f->incs);
  list_free(&f->imported);
}

/* InitFrame */
static int init_frame(include_frame *f) {
  for (size_t i = 0; i < f->root.nchildren; i++) {
    const lu_mtlx_elem *c = &f->root.children[i];
    if (is_include(c) && !lu_mtlx_strv_push(&f->incs, lu_mtlx_attr_or(c, "href", ""))) return 0;
  }
  return 1;
}

typedef struct frame_stack {
  include_frame *v;
  size_t n, cap;
} frame_stack;

static include_frame *stack_push(frame_stack *s) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    include_frame *v = (include_frame *)realloc(s->v, cap * sizeof(*v));
    if (!v) return NULL;
    s->v = v;
    s->cap = cap;
  }
  include_frame *f = &s->v[s->n++];
  memset(f, 0, sizeof(*f));
  lu_mtlx_elem_init(&f->root);
  lu_mtlx_strv_init(&f->incs);
  return f;
}

/* Finishes the top frame: imported children, then its own non-include
 * children (AddUnique). The finished root is moved into *out. */
static int finish_frame(include_frame *f, lu_mtlx_elem *out) {
  elem_list kids = f->imported;
  memset(&f->imported, 0, sizeof(f->imported));
  int ok = 1;
  for (size_t i = 0; i < f->root.nchildren; i++) {
    lu_mtlx_elem *c = &f->root.children[i];
    if (!ok || is_include(c)) continue;
    lu_mtlx_elem moved;
    lu_mtlx_elem_move(&moved, c);
    ok = add_unique(&kids, &moved);
  }
  /* Replace the child array. */
  for (size_t i = 0; i < f->root.nchildren; i++) lu_mtlx_elem_free(&f->root.children[i]);
  free(f->root.children);
  f->root.children = kids.v;
  f->root.nchildren = kids.n;
  f->root.capchildren = kids.cap;
  lu_mtlx_elem_move(out, &f->root);
  return ok;
}

/* Loads, parses and pushes the frame of `href`. Returns 1, 0 on error. */
static int open_include(frame_stack *st, const char *href, lu_mtlx_include_loader loader,
                        void *user, lu_mtlx_sb *err) {
  for (size_t i = 1; i < st->n; i++) {
    if (strcmp(st->v[i].href, href) == 0) {
      lu_mtlx_err(err, "XInclude cycle detected");
      return 0;
    }
  }
  const char *including = st->v[st->n - 1].href ? st->v[st->n - 1].href : "";
  char *text = NULL;
  size_t len = 0;
  if (!loader || !loader(user, href, including, &text, &len)) {
    free(text);
    if (err) {
      lu_mtlx_sb_set(err, "missing include: ");
      lu_mtlx_sb_puts(err, href);
    }
    return 0;
  }
  if (st->n >= kMaxIncludeDepth) {
    free(text);
    lu_mtlx_err(err, "maximum XInclude depth exceeded");
    return 0;
  }
  lu_mtlx_elem root;
  lu_mtlx_elem_init(&root);
  lu_mtlx_sb perr;
  lu_mtlx_sb_init(&perr);
  const int ok = lu_mtlx_parse_xml(text ? text : "", text ? len : 0, &root, &perr);
  free(text);
  if (!ok) {
    if (err) {
      lu_mtlx_sb_set(err, href);
      lu_mtlx_sb_puts(err, ": ");
      lu_mtlx_sb_puts(err, lu_mtlx_sb_str(&perr));
    }
    lu_mtlx_sb_free(&perr);
    lu_mtlx_elem_free(&root);
    return 0;
  }
  lu_mtlx_sb_free(&perr);
  include_frame *f = stack_push(st);
  char *h = lu_mtlx_strdup(href);
  if (!f || !h) {
    free(h);
    lu_mtlx_elem_free(&root);
    lu_mtlx_err(err, "out of memory");
    return 0;
  }
  f->href = h;
  lu_mtlx_elem_move(&f->root, &root);
  if (!init_frame(f)) {
    lu_mtlx_err(err, "out of memory");
    return 0;
  }
  return 1;
}

/* ExpandMtlxIncludes (anchored loader). */
int lu_mtlx_expand_includes(lu_mtlx_elem *root, lu_mtlx_include_loader loader, void *user,
                            lu_mtlx_sb *err) {
  if (!root) return 0;
  frame_stack st = {NULL, 0, 0};
  include_frame *top = stack_push(&st);
  if (!top) {
    lu_mtlx_err(err, "out of memory");
    return 0;
  }
  lu_mtlx_elem_move(&top->root, root);
  int ok = init_frame(top);
  if (!ok) lu_mtlx_err(err, "out of memory");
  while (ok) {
    include_frame *f = &st.v[st.n - 1];
    if (f->next < f->incs.n) {
      char *href = lu_mtlx_strdup(f->incs.v[f->next++]);
      if (!href) {
        lu_mtlx_err(err, "out of memory");
        ok = 0;
        break;
      }
      ok = open_include(&st, href, loader, user, err);
      free(href);
      continue;
    }
    lu_mtlx_elem finished;
    ok = finish_frame(f, &finished);
    frame_free(f);
    st.n--;
    if (!ok) {
      lu_mtlx_elem_free(&finished);
      lu_mtlx_err(err, "out of memory");
      break;
    }
    if (st.n == 0) {
      lu_mtlx_elem_move(root, &finished);
      free(st.v);
      return 1;
    }
    if (!import_library(&st.v[st.n - 1].imported, &finished)) {
      lu_mtlx_err(err, "out of memory");
      ok = 0;
    }
  }
  for (size_t i = 0; i < st.n; i++) frame_free(&st.v[i]);
  free(st.v);
  return 0;
}
