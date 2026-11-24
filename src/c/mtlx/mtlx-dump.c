/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * Canonical MaterialX document dump (C11 port of DumpMtlxDocument and its
 * anonymous helpers OrDash / BindingOf / PortLine / DumpNode / DumpNodeDef of
 * src/core/mtlx-resolve.cc).
 */
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* OrDash */
static const char *or_dash(const char *v) { return v ? v : "-"; }

/* BindingOf (appends to o). */
static int binding_of(lu_mtlx_sb *o, const lu_mtlx_elem *port, const lu_mtlx_scope *outer) {
  const char *type = lu_mtlx_attr_or(port, "type", "");
  const char *out = or_dash(lu_mtlx_get_attr(port, "output"));
  const char *s;
  if ((s = lu_mtlx_get_attr(port, "interfacename"))) {
    lu_mtlx_sb_puts(o, "iface=");
    lu_mtlx_sb_puts(o, s);
    return 1;
  }
  const char *kind = NULL;
  if ((s = lu_mtlx_get_attr(port, "nodename"))) kind = "node=";
  else if ((s = lu_mtlx_get_attr(port, "nodegraph"))) kind = "graph=";
  if (kind) {
    lu_mtlx_sb_puts(o, kind);
    lu_mtlx_sb_puts(o, s);
    lu_mtlx_sb_puts(o, " out=");
    lu_mtlx_sb_puts(o, out);
    return 1;
  }
  const char *v = lu_mtlx_get_attr(port, "value");
  if (!v) {
    lu_mtlx_sb_puts(o, "none");
    return 1;
  }
  lu_mtlx_scope chain;
  if (!lu_mtlx_scope_with(&chain, port, outer)) {
    lu_mtlx_scope_free(&chain);
    return 0;
  }
  lu_mtlx_sb text;
  lu_mtlx_sb_init(&text);
  int ok = 1;
  if (streq(type, "filename")) {
    if (!lu_mtlx_resolved_filename(&chain, &text)) lu_mtlx_sb_set(&text, v);
  } else {
    ok = lu_mtlx_lex_value(type, v, &text);
  }
  lu_mtlx_sb_puts(o, "value=");
  lu_mtlx_sb_puts(o, lu_mtlx_sb_str(&text));
  if (streq(type, "color3") || streq(type, "color4") || streq(type, "filename")) {
    lu_mtlx_sb cs;
    lu_mtlx_sb_init(&cs);
    if (lu_mtlx_active_colorspace(&chain, &cs)) {
      lu_mtlx_sb_puts(o, " cs=");
      lu_mtlx_sb_puts(o, lu_mtlx_sb_str(&cs));
    }
    lu_mtlx_sb_free(&cs);
  }
  ok = ok && !text.oom;
  lu_mtlx_sb_free(&text);
  lu_mtlx_scope_free(&chain);
  return ok;
}

/* PortLine */
static int port_line(lu_mtlx_sb *o, const char *indent, const char *kind,
                     const lu_mtlx_elem *port, const lu_mtlx_scope *outer) {
  lu_mtlx_sb_puts(o, indent);
  lu_mtlx_sb_puts(o, kind);
  lu_mtlx_sb_putc(o, ' ');
  lu_mtlx_sb_puts(o, lu_mtlx_name(port));
  lu_mtlx_sb_putc(o, ' ');
  lu_mtlx_sb_puts(o, lu_mtlx_attr_or(port, "type", ""));
  lu_mtlx_sb_putc(o, ' ');
  const int ok = binding_of(o, port, outer);
  lu_mtlx_sb_putc(o, '\n');
  return ok;
}

/* DumpNode */
static int dump_node(lu_mtlx_sb *o, const lu_mtlx_library *lib, const char *indent,
                     const lu_mtlx_elem *e, const lu_mtlx_scope *outer) {
  const lu_mtlx_nodedef *nd = lu_mtlx_library_resolve_reference(lib, e, outer);
  lu_mtlx_sb_puts(o, indent);
  lu_mtlx_sb_puts(o, "node ");
  lu_mtlx_sb_puts(o, lu_mtlx_name(e));
  lu_mtlx_sb_putc(o, ' ');
  lu_mtlx_sb_puts(o, e->tag);
  lu_mtlx_sb_putc(o, ' ');
  lu_mtlx_sb_puts(o, lu_mtlx_attr_or(e, "type", ""));
  lu_mtlx_sb_puts(o, " def=");
  lu_mtlx_sb_puts(o, nd ? nd->name : "?");
  lu_mtlx_sb_putc(o, '\n');
  lu_mtlx_scope chain;
  if (!lu_mtlx_scope_with(&chain, e, outer)) {
    lu_mtlx_scope_free(&chain);
    return 0;
  }
  lu_mtlx_sb in_indent;
  lu_mtlx_sb_init(&in_indent);
  lu_mtlx_sb_puts(&in_indent, indent);
  lu_mtlx_sb_puts(&in_indent, "  ");
  int ok = !in_indent.oom;
  for (size_t i = 0; ok && i < e->nchildren; i++)
    if (streq(e->children[i].tag, "input"))
      ok = port_line(o, lu_mtlx_sb_str(&in_indent), "in", &e->children[i], &chain);
  lu_mtlx_sb_free(&in_indent);
  lu_mtlx_scope_free(&chain);
  return ok;
}

/* DumpNodeDef */
static int dump_nodedef(lu_mtlx_sb *o, const lu_mtlx_library *lib, const lu_mtlx_elem *e,
                        const lu_mtlx_scope *outer) {
  lu_mtlx_library one;
  lu_mtlx_library_init(&one);
  if (!lu_mtlx_library_add_nodedef(&one, e, outer)) {
    lu_mtlx_library_free(&one);
    return 0;
  }
  const lu_mtlx_nodedef *nd = &one.defs[0];
  lu_mtlx_sb t;
  lu_mtlx_sb_init(&t);
  lu_mtlx_ports ins = {NULL, 0};
  int ok = lu_mtlx_library_out_type(lib, nd, &t) && lu_mtlx_library_active_inputs(lib, nd, &ins);
  if (ok) {
    lu_mtlx_sb_puts(o, "nodedef ");
    lu_mtlx_sb_puts(o, nd->name);
    lu_mtlx_sb_puts(o, " node=");
    lu_mtlx_sb_puts(o, nd->node);
    lu_mtlx_sb_puts(o, " type=");
    lu_mtlx_sb_puts(o, lu_mtlx_sb_str(&t));
    lu_mtlx_sb_puts(o, " version=");
    lu_mtlx_sb_puts(o, nd->has_version ? nd->version : "-");
    lu_mtlx_sb_puts(o, " default=");
    lu_mtlx_sb_puts(o, nd->is_default ? "1" : "0");
    lu_mtlx_sb_puts(o, " inherit=");
    lu_mtlx_sb_puts(o, nd->has_inherit ? nd->inherit : "-");
    lu_mtlx_sb_putc(o, '\n');
  }
  for (size_t i = 0; ok && i < ins.n; i++) {
    const lu_mtlx_port *p = &ins.v[i];
    lu_mtlx_sb_puts(o, "  in ");
    lu_mtlx_sb_puts(o, p->name);
    lu_mtlx_sb_putc(o, ' ');
    lu_mtlx_sb_puts(o, p->type);
    lu_mtlx_sb_putc(o, ' ');
    if (p->has_value) {
      ok = lu_mtlx_lex_value(p->type, p->value, &t);
      lu_mtlx_sb_puts(o, lu_mtlx_sb_str(&t));
    } else {
      lu_mtlx_sb_putc(o, '-');
    }
    lu_mtlx_sb_putc(o, '\n');
  }
  for (size_t i = 0; ok && i < nd->noutputs; i++) {
    lu_mtlx_sb_puts(o, "  out ");
    lu_mtlx_sb_puts(o, nd->outputs[i].name);
    lu_mtlx_sb_putc(o, ' ');
    lu_mtlx_sb_puts(o, nd->outputs[i].type);
    lu_mtlx_sb_putc(o, '\n');
  }
  free(ins.v);
  lu_mtlx_sb_free(&t);
  lu_mtlx_library_free(&one);
  return ok;
}

/* The nodegraph section of DumpMtlxDocument. */
static int dump_graph(lu_mtlx_sb *o, const lu_mtlx_library *lib, const lu_mtlx_elem *e,
                      const lu_mtlx_elem *root) {
  lu_mtlx_sb_puts(o, "nodegraph ");
  lu_mtlx_sb_puts(o, lu_mtlx_name(e));
  lu_mtlx_sb_puts(o, " nodedef=");
  lu_mtlx_sb_puts(o, or_dash(lu_mtlx_get_attr(e, "nodedef")));
  lu_mtlx_sb_putc(o, '\n');
  const lu_mtlx_elem *sv[2] = {e, root};
  const lu_mtlx_scope scope = {sv, 2, 2};
  int ok = 1;
  for (size_t i = 0; ok && i < e->nchildren; i++)
    if (streq(e->children[i].tag, "input"))
      ok = port_line(o, "  ", "in", &e->children[i], &scope);
  for (size_t i = 0; ok && i < e->nchildren; i++)
    if (lu_mtlx_is_node_elem(&e->children[i]))
      ok = dump_node(o, lib, "  ", &e->children[i], &scope);
  for (size_t i = 0; ok && i < e->nchildren; i++)
    if (streq(e->children[i].tag, "output"))
      ok = port_line(o, "  ", "out", &e->children[i], &scope);
  return ok;
}

/* DumpMtlxDocument (appends to out). */
int lu_mtlx_dump_document(const lu_mtlx_library *lib, const lu_mtlx_elem *root,
                          lu_mtlx_sb *out) {
  lu_mtlx_sb o;
  lu_mtlx_sb_init(&o);
  lu_mtlx_sb_puts(&o, "doc version=");
  lu_mtlx_sb_puts(&o, or_dash(lu_mtlx_get_attr(root, "version")));
  lu_mtlx_sb_puts(&o, " colorspace=");
  lu_mtlx_sb_puts(&o, or_dash(lu_mtlx_get_attr(root, "colorspace")));
  lu_mtlx_sb_putc(&o, '\n');
  const lu_mtlx_elem *rv[1] = {root};
  const lu_mtlx_scope root_scope = {rv, 1, 1};
  int ok = 1;
  for (size_t i = 0; ok && i < root->nchildren; i++) {
    const lu_mtlx_elem *e = &root->children[i];
    if (streq(e->tag, "nodedef")) ok = dump_nodedef(&o, lib, e, &root_scope);
    else if (streq(e->tag, "nodegraph")) ok = dump_graph(&o, lib, e, root);
    else if (lu_mtlx_is_node_elem(e)) ok = dump_node(&o, lib, "", e, &root_scope);
  }
  ok = ok && !o.oom;
  if (ok) lu_mtlx_sb_put(out, o.p ? o.p : "", o.n);
  lu_mtlx_sb_free(&o);
  return ok && !out->oom;
}
