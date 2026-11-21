/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX nodegraph flattening (C11 port of FlattenMtlxGraphCall and
 * FlattenMtlxGraph of src/core/mtlx-resolve.cc). The C++ lambdas become
 * functions over a shared call context.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* err = concatenation of the NULL-terminated string arguments. */
static int errf(lu_mtlx_sb *err, ...) {
  lu_mtlx_sb m;
  lu_mtlx_sb_init(&m);
  va_list ap;
  va_start(ap, err);
  for (const char *s = va_arg(ap, const char *); s; s = va_arg(ap, const char *))
    lu_mtlx_sb_puts(&m, s);
  va_end(ap);
  lu_mtlx_err(err, m.oom ? "out of memory" : lu_mtlx_sb_str(&m));
  lu_mtlx_sb_free(&m);
  return 0;
}

/* FindMtlxChild (anonymous). */
static const lu_mtlx_elem *find_child(const lu_mtlx_elem *parent, const char *tag,
                                      const char *name) {
  for (size_t i = 0; i < parent->nchildren; i++) {
    const lu_mtlx_elem *c = &parent->children[i];
    if (streq(c->tag, tag) && streq(lu_mtlx_name(c), name)) return c;
  }
  return NULL;
}

/* FindMtlxNode (anonymous). */
static const lu_mtlx_elem *find_node(const lu_mtlx_elem *graph, const char *name) {
  for (size_t i = 0; i < graph->nchildren; i++) {
    const lu_mtlx_elem *c = &graph->children[i];
    if (lu_mtlx_is_node_elem(c) && streq(lu_mtlx_name(c), name)) return c;
  }
  return NULL;
}

static int is_source_key(const char *k) {
  return streq(k, "nodename") || streq(k, "nodegraph") || streq(k, "interfacename") ||
         streq(k, "output") || streq(k, "value");
}

/* erase_sources lambda of FlattenMtlxGraphCall. */
static void erase_sources(lu_mtlx_elem *port) {
  static const char *const keys[] = {"nodename", "nodegraph", "interfacename", "output", "value"};
  for (size_t i = 0; i < 5; i++) lu_mtlx_attr_erase(port, keys[i]);
}

static void clear_attrs(lu_mtlx_elem *e) {
  for (size_t i = 0; i < e->nattrs; i++) {
    free(e->attrs[i].key);
    free(e->attrs[i].value);
  }
  e->nattrs = 0;
}

static int has_tokens(const lu_mtlx_elem *e) {
  for (size_t i = 0; i < e->nchildren; i++)
    if (streq(e->children[i].tag, "token")) return 1;
  return 0;
}

typedef struct fc_ctx {
  const lu_mtlx_elem *document, *graph, *call, *body;
  const lu_mtlx_library *library;
  const lu_mtlx_scope *outer;
  const lu_mtlx_nodedef *def;
  lu_mtlx_sb prefix, geom_prefix;
  lu_mtlx_elem geom_nodes; /* generated_geom_nodes, held as children */
  lu_mtlx_strv geom_keys, geom_names; /* generated_geom_names pairs */
  lu_mtlx_ports call_outputs;
  lu_mtlx_sb *err;
} fc_ctx;

static int fc_oom(fc_ctx *c) {
  lu_mtlx_err(c->err, "out of memory");
  return 0;
}

/* renamed lambda: prefix + name (replaces out). */
static void renamed(const fc_ctx *c, const char *name, lu_mtlx_sb *out) {
  lu_mtlx_sb_set(out, lu_mtlx_sb_str(&c->prefix));
  lu_mtlx_sb_puts(out, name);
}

/* bind_interface, the call-input branch. */
static int fc_bind_call_input(fc_ctx *c, lu_mtlx_elem *port, const lu_mtlx_elem *call_input) {
  if (call_input->nchildren) return errf(c->err, "unsupported call input children", NULL);
  char *port_name = lu_mtlx_strdup(lu_mtlx_name(port));
  if (!port_name) return fc_oom(c);
  clear_attrs(port);
  int ok = lu_mtlx_attr_set(port, "name", port_name);
  free(port_name);
  for (size_t i = 0; ok && i < call_input->nattrs; i++)
    if (!streq(call_input->attrs[i].key, "name"))
      ok = lu_mtlx_attr_set(port, call_input->attrs[i].key, call_input->attrs[i].value);
  return ok ? 1 : fc_oom(c);
}

/* bind_interface: the geompropdef a default geometry property names. */
static int fc_find_geomdef(fc_ctx *c, const char *geomprop, const lu_mtlx_elem **out) {
  const lu_mtlx_elem *geomdef = NULL;
  for (size_t i = 0; i < c->document->nchildren; i++) {
    const lu_mtlx_elem *child = &c->document->children[i];
    if (!streq(child->tag, "geompropdef") || !streq(lu_mtlx_name(child), geomprop)) continue;
    if (geomdef) return errf(c->err, "ambiguous default geometry property", NULL);
    geomdef = child;
  }
  if (!geomdef) {
    for (size_t i = 0; i < c->library->ngeomprop; i++) {
      const lu_mtlx_elem *cand = &c->library->geomprop_defs[i];
      if (!streq(lu_mtlx_name(cand), geomprop)) continue;
      if (geomdef) return errf(c->err, "ambiguous default geometry property", NULL);
      geomdef = cand;
    }
  }
  if (!geomdef) return errf(c->err, "missing default geometry property '", geomprop, "'", NULL);
  *out = geomdef;
  return 1;
}

/* bind_interface: generate the geometry node for a defaultgeomprop. */
static int fc_bind_geomprop(fc_ctx *c, lu_mtlx_elem *port, const lu_mtlx_port *p) {
  for (size_t i = 0; i < c->geom_keys.n; i++)
    if (streq(c->geom_keys.v[i], p->default_geomprop))
      return lu_mtlx_attr_set(port, "nodename", c->geom_names.v[i]) ? 1 : fc_oom(c);
  const lu_mtlx_elem *geomdef = NULL;
  if (!fc_find_geomdef(c, p->default_geomprop, &geomdef)) return 0;
  if (!streq(lu_mtlx_attr_or(geomdef, "type", ""), p->type) ||
      !*lu_mtlx_attr_or(geomdef, "geomprop", ""))
    return errf(c->err, "invalid default geometry property '", p->default_geomprop, "'", NULL);
  lu_mtlx_sb name;
  lu_mtlx_sb_init(&name);
  lu_mtlx_sb_puts(&name, lu_mtlx_sb_str(&c->geom_prefix));
  lu_mtlx_sb_putu(&name, c->geom_nodes.nchildren);
  const char *geom_name = lu_mtlx_sb_str(&name);
  lu_mtlx_elem geom;
  lu_mtlx_elem_init(&geom);
  int ok = !name.oom && lu_mtlx_elem_set_tag(&geom, lu_mtlx_attr_or(geomdef, "geomprop", "")) &&
           lu_mtlx_attr_set(&geom, "name", geom_name) && lu_mtlx_attr_set(&geom, "type", p->type);
  static const char *const attrs[] = {"space", "index"};
  for (size_t a = 0; ok && a < 2; a++) {
    const char *value = lu_mtlx_get_attr(geomdef, attrs[a]);
    if (!value) continue;
    lu_mtlx_elem input;
    lu_mtlx_elem_init(&input);
    ok = lu_mtlx_elem_set_tag(&input, "input") && lu_mtlx_attr_set(&input, "name", attrs[a]) &&
         lu_mtlx_attr_set(&input, "type", a == 0 ? "string" : "integer") &&
         lu_mtlx_attr_set(&input, "value", value) && lu_mtlx_child_push(&geom, &input) != NULL;
    lu_mtlx_elem_free(&input);
  }
  ok = ok && lu_mtlx_strv_push(&c->geom_keys, p->default_geomprop) &&
       lu_mtlx_strv_push(&c->geom_names, geom_name);
  ok = ok && lu_mtlx_child_push(&c->geom_nodes, &geom) != NULL;
  ok = ok && lu_mtlx_attr_set(port, "nodename", geom_name);
  lu_mtlx_elem_free(&geom);
  lu_mtlx_sb_free(&name);
  return ok ? 1 : fc_oom(c);
}

/* bind_interface lambda of FlattenMtlxGraphCall. */
static int fc_bind_interface(fc_ctx *c, lu_mtlx_elem *port, const char *name) {
  const lu_mtlx_elem *call_input = find_child(c->call, "input", name);
  if (call_input) return fc_bind_call_input(c, port, call_input);
  erase_sources(port);
  lu_mtlx_ports inputs;
  if (!lu_mtlx_library_active_inputs(c->library, c->def, &inputs)) return fc_oom(c);
  int result = -1;
  for (size_t i = 0; result < 0 && i < inputs.n; i++) {
    const lu_mtlx_port *p = &inputs.v[i];
    if (!streq(p->name, name)) continue;
    if (*p->default_geomprop) result = fc_bind_geomprop(c, port, p);
    else if (p->has_value) result = lu_mtlx_attr_set(port, "value", p->value) ? 1 : fc_oom(c);
  }
  free(inputs.v);
  if (result >= 0) return result;
  return errf(c->err, "missing graph call input '", name, "'", NULL);
}

/* rewrite_body_port lambda of FlattenMtlxGraphCall. */
static int fc_rewrite_body_port(fc_ctx *c, lu_mtlx_elem *port) {
  if (lu_mtlx_get_attr(port, "channels") || lu_mtlx_get_attr(port, "nodegraph"))
    return errf(c->err, "unsupported implementation connection", NULL);
  const char *iface = lu_mtlx_get_attr(port, "interfacename");
  if (iface) {
    char *name = lu_mtlx_strdup(iface);
    if (!name) return fc_oom(c);
    const int ok = fc_bind_interface(c, port, name);
    free(name);
    return ok;
  }
  const char *source = lu_mtlx_get_attr(port, "nodename");
  if (source) {
    if (!find_node(c->body, source)) return errf(c->err, "missing implementation node", NULL);
    lu_mtlx_sb r;
    lu_mtlx_sb_init(&r);
    renamed(c, source, &r);
    const int ok = !r.oom && lu_mtlx_attr_set(port, "nodename", lu_mtlx_sb_str(&r));
    lu_mtlx_sb_free(&r);
    return ok ? 1 : fc_oom(c);
  }
  if (lu_mtlx_get_attr(port, "output"))
    return errf(c->err, "unsupported implementation output reference", NULL);
  return 1;
}

/* rewrite_call_port lambda of FlattenMtlxGraphCall. */
static int fc_rewrite_call_port(fc_ctx *c, lu_mtlx_elem *port, const char *call_name) {
  const char *source = lu_mtlx_get_attr(port, "nodename");
  if (!source || !streq(source, call_name)) return 1;
  if (lu_mtlx_get_attr(port, "channels"))
    return errf(c->err, "unsupported call output channels", NULL);
  const char *output_name = lu_mtlx_attr_or(port, "output", "");
  if (!*output_name)
    output_name = c->call_outputs.n == 1 ? c->call_outputs.v[0].name : "out";
  const lu_mtlx_elem *body_output = find_child(c->body, "output", output_name);
  if (!body_output)
    return errf(c->err, "missing implementation output '", output_name, "'", NULL);
  int matching = 0;
  for (size_t i = 0; i < c->call_outputs.n; i++)
    if (streq(c->call_outputs.v[i].name, output_name) &&
        streq(c->call_outputs.v[i].type, lu_mtlx_attr_or(body_output, "type", "")))
      matching = 1;
  if (!matching) return errf(c->err, "implementation output type mismatch", NULL);
  if (lu_mtlx_get_attr(body_output, "channels") || lu_mtlx_get_attr(body_output, "nodegraph"))
    return errf(c->err, "unsupported implementation output", NULL);
  lu_mtlx_elem route;
  if (!lu_mtlx_elem_copy(&route, body_output)) return fc_oom(c);
  int ok = fc_rewrite_body_port(c, &route);
  if (ok && (lu_mtlx_get_attr(port, "value") ||
             (lu_mtlx_get_attr(&route, "value") && lu_mtlx_get_attr(&route, "nodename"))))
    ok = errf(c->err, "unsupported connected output fallback", NULL);
  if (ok) {
    erase_sources(port);
    int bound = 0;
    for (size_t i = 0; ok && i < route.nattrs; i++)
      if (is_source_key(route.attrs[i].key)) {
        ok = lu_mtlx_attr_set(port, route.attrs[i].key, route.attrs[i].value) ? 1 : fc_oom(c);
        bound = 1;
      }
    if (ok && !bound) ok = errf(c->err, "implementation output has no source", NULL);
  }
  lu_mtlx_elem_free(&route);
  return ok;
}

/* FlattenMtlxGraphCall: the implementation (nodegraph or <implementation>)
 * of the call's nodedef, then the body graph. */
static int fc_find_body(fc_ctx *c) {
  const lu_mtlx_elem *doc = c->document;
  const lu_mtlx_elem *impl = NULL;
  for (size_t i = 0; i < doc->nchildren; i++) {
    const lu_mtlx_elem *child = &doc->children[i];
    if ((!streq(child->tag, "nodegraph") && !streq(child->tag, "implementation")) ||
        !streq(lu_mtlx_attr_or(child, "nodedef", ""), c->def->name))
      continue;
    if (impl) return errf(c->err, "ambiguous graph implementation", NULL);
    impl = child;
  }
  if (!impl) return errf(c->err, "missing graph implementation", NULL);
  if (*lu_mtlx_attr_or(impl, "target", ""))
    return errf(c->err, "unsupported targeted implementation", NULL);
  const lu_mtlx_elem *body = impl;
  if (streq(impl->tag, "implementation")) {
    const char *body_name = lu_mtlx_attr_or(impl, "nodegraph", "");
    body = NULL;
    for (size_t i = 0; i < doc->nchildren; i++) {
      const lu_mtlx_elem *child = &doc->children[i];
      if (!streq(child->tag, "nodegraph") || !streq(lu_mtlx_name(child), body_name)) continue;
      if (body) return errf(c->err, "ambiguous implementation graph", NULL);
      body = child;
    }
    if (!body || *lu_mtlx_attr_or(body, "target", ""))
      return errf(c->err, "missing untargeted implementation graph", NULL);
  }
  c->body = body;
  return 1;
}

/* FlattenMtlxGraphCall: scope and token restrictions. */
static int fc_check_scopes(fc_ctx *c, const lu_mtlx_scope *call_scope) {
  lu_mtlx_scope body_scope, graph_scope;
  const int sok = lu_mtlx_scope_with(&body_scope, c->body, c->outer) &&
                  lu_mtlx_scope_with(&graph_scope, c->graph, c->outer);
  int ok = sok ? 1 : fc_oom(c);
  static const char *const keys[] = {"colorspace", "fileprefix", "geomprefix", "namespace"};
  for (size_t k = 0; ok && k < 4; k++) {
    const char *bv = "", *gv = "", *cv = "";
    const int hb = lu_mtlx_scoped_attr(keys[k], &body_scope, &bv);
    const int hg = lu_mtlx_scoped_attr(keys[k], &graph_scope, &gv);
    const int hc = lu_mtlx_scoped_attr(keys[k], call_scope, &cv);
    if (hb != hg || hc != hg || (hg && (!streq(bv, gv) || !streq(cv, gv))))
      ok = errf(c->err, "unsupported differing implementation graph scope", NULL);
  }
  lu_mtlx_scope_free(&body_scope);
  lu_mtlx_scope_free(&graph_scope);
  if (!ok) return 0;
  if (has_tokens(c->body) || has_tokens(c->graph) || has_tokens(c->call))
    return errf(c->err, "unsupported graph or call token scope", NULL);
  for (size_t i = 0; i < c->document->nchildren; i++) {
    const lu_mtlx_elem *child = &c->document->children[i];
    const char *n = lu_mtlx_name(child);
    if (streq(child->tag, "nodedef") && has_tokens(child) &&
        (streq(n, lu_mtlx_attr_or(c->body, "nodedef", "")) ||
         streq(n, lu_mtlx_attr_or(c->graph, "nodedef", "")) || streq(n, c->def->name)))
      return errf(c->err, "unsupported nodedef token scope", NULL);
  }
  return 1;
}

/* FlattenMtlxGraphCall: the renaming prefixes. */
static int fc_prefixes(fc_ctx *c) {
  size_t longest = 0, longest_body = 0;
  for (size_t i = 0; i < c->graph->nchildren; i++) {
    const size_t n = strlen(lu_mtlx_name(&c->graph->children[i]));
    if (n > longest) longest = n;
  }
  for (size_t i = 0; i < c->body->nchildren; i++) {
    const lu_mtlx_elem *b = &c->body->children[i];
    if (!lu_mtlx_is_node_elem(b)) continue;
    const size_t n = strlen(lu_mtlx_name(b));
    if (n > longest_body) longest_body = n;
  }
  for (size_t i = 0; i < longest + 1; i++) lu_mtlx_sb_putc(&c->prefix, '_');
  lu_mtlx_sb_puts(&c->geom_prefix, lu_mtlx_sb_str(&c->prefix));
  for (size_t i = 0; i < longest_body + 1; i++) lu_mtlx_sb_putc(&c->geom_prefix, '_');
  lu_mtlx_sb_puts(&c->geom_prefix, "geom");
  return c->prefix.oom || c->geom_prefix.oom ? fc_oom(c) : 1;
}

/* FlattenMtlxGraphCall: copies of the body nodes in place of the call. */
static int fc_expand_call(fc_ctx *c, lu_mtlx_elem *result) {
  for (size_t i = 0; i < c->body->nchildren; i++) {
    const lu_mtlx_elem *bn = &c->body->children[i];
    if (!lu_mtlx_is_node_elem(bn)) continue;
    lu_mtlx_elem copy;
    if (!lu_mtlx_elem_copy(&copy, bn)) return fc_oom(c);
    lu_mtlx_sb r;
    lu_mtlx_sb_init(&r);
    renamed(c, lu_mtlx_name(bn), &r);
    int ok = !r.oom && lu_mtlx_attr_set(&copy, "name", lu_mtlx_sb_str(&r));
    lu_mtlx_sb_free(&r);
    if (!ok) fc_oom(c);
    for (size_t k = 0; ok && k < copy.nchildren; k++) {
      if (!streq(copy.children[k].tag, "input"))
        ok = errf(c->err, "unsupported implementation node child", NULL);
      else
        ok = fc_rewrite_body_port(c, &copy.children[k]);
    }
    if (ok && !lu_mtlx_child_push(result, &copy)) ok = fc_oom(c);
    lu_mtlx_elem_free(&copy);
    if (!ok) return 0;
  }
  return 1;
}

/* FlattenMtlxGraphCall: build the flattened graph. */
static int fc_build(fc_ctx *c, const char *call_name, lu_mtlx_elem *result) {
  const lu_mtlx_elem *graph = c->graph;
  int ok = lu_mtlx_elem_set_tag(result, graph->tag);
  for (size_t a = 0; ok && a < graph->nattrs; a++)
    ok = lu_mtlx_attr_append(result, graph->attrs[a].key, graph->attrs[a].value);
  if (!ok) return fc_oom(c);
  for (size_t i = 0; i < graph->nchildren; i++) {
    const lu_mtlx_elem *child = &graph->children[i];
    if (lu_mtlx_is_node_elem(child) && streq(lu_mtlx_name(child), call_name)) {
      if (!fc_expand_call(c, result)) return 0;
    } else {
      lu_mtlx_elem copy;
      if (!lu_mtlx_elem_copy(&copy, child) || !lu_mtlx_child_push(result, &copy)) return fc_oom(c);
    }
  }
  if (!lu_mtlx_library_active_outputs(c->library, c->def, &c->call_outputs)) return fc_oom(c);
  for (size_t i = 0; i < result->nchildren; i++) {
    lu_mtlx_elem *child = &result->children[i];
    if (streq(child->tag, "output")) {
      if (!fc_rewrite_call_port(c, child, call_name)) return 0;
    } else if (lu_mtlx_is_node_elem(child)) {
      for (size_t k = 0; k < child->nchildren; k++)
        if (streq(child->children[k].tag, "input") &&
            !fc_rewrite_call_port(c, &child->children[k], call_name))
          return 0;
    }
  }
  for (size_t i = 0; i < c->geom_nodes.nchildren; i++)
    if (!lu_mtlx_child_push(result, &c->geom_nodes.children[i])) return fc_oom(c);
  c->geom_nodes.nchildren = 0;
  return 1;
}

static int fc_resolve(fc_ctx *c, const lu_mtlx_elem *g) {
  lu_mtlx_strv checked;
  lu_mtlx_strv_init(&checked);
  const int ok = lu_mtlx_resolve_graph(c->document, g, c->library, c->outer, &checked, c->err);
  lu_mtlx_strv_free(&checked);
  return ok;
}

static int flatten_call_impl(fc_ctx *c, const char *call_name, lu_mtlx_elem *result) {
  if (!fc_resolve(c, c->graph)) return 0;
  c->call = find_node(c->graph, call_name);
  if (!c->call) return errf(c->err, "missing graph call '", call_name, "'", NULL);
  lu_mtlx_scope gs, call_scope;
  if (!lu_mtlx_scope_with(&gs, c->graph, c->outer)) {
    lu_mtlx_scope_free(&gs);
    return fc_oom(c);
  }
  const int sok = lu_mtlx_scope_with(&call_scope, c->call, &gs);
  lu_mtlx_scope_free(&gs);
  int ok = sok ? 1 : fc_oom(c);
  if (ok) {
    c->def = lu_mtlx_library_resolve_reference(c->library, c->call, &call_scope);
    if (!c->def) ok = errf(c->err, "cannot resolve graph call nodedef", NULL);
  }
  ok = ok && fc_find_body(c) && fc_check_scopes(c, &call_scope);
  lu_mtlx_scope_free(&call_scope);
  if (!ok || !fc_resolve(c, c->body)) return 0;
  if (!fc_prefixes(c) || !fc_build(c, call_name, result)) return 0;
  return fc_resolve(c, result);
}

/* FlattenMtlxGraphCall */
int lu_mtlx_flatten_graph_call(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                               const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                               const char *call_name, lu_mtlx_elem *flattened,
                               lu_mtlx_sb *err) {
  lu_mtlx_sb ignored;
  lu_mtlx_sb_init(&ignored);
  if (!err) err = &ignored;
  lu_mtlx_err(err, "");
  if (!flattened) {
    lu_mtlx_err(err, "missing flattened graph output");
    lu_mtlx_sb_free(&ignored);
    return 0;
  }
  fc_ctx c;
  memset(&c, 0, sizeof(c));
  c.document = document;
  c.graph = graph;
  c.library = library;
  c.outer = outer;
  c.err = err;
  lu_mtlx_sb_init(&c.prefix);
  lu_mtlx_sb_init(&c.geom_prefix);
  lu_mtlx_elem_init(&c.geom_nodes);
  lu_mtlx_strv_init(&c.geom_keys);
  lu_mtlx_strv_init(&c.geom_names);
  lu_mtlx_elem result;
  lu_mtlx_elem_init(&result);
  const int ok = flatten_call_impl(&c, call_name, &result);
  if (ok) {
    lu_mtlx_elem_free(flattened);
    lu_mtlx_elem_move(flattened, &result);
  }
  lu_mtlx_elem_free(&result);
  lu_mtlx_elem_free(&c.geom_nodes);
  lu_mtlx_strv_free(&c.geom_keys);
  lu_mtlx_strv_free(&c.geom_names);
  lu_mtlx_sb_free(&c.prefix);
  lu_mtlx_sb_free(&c.geom_prefix);
  free(c.call_outputs.v);
  lu_mtlx_sb_free(&ignored);
  return ok;
}

/* ---- FlattenMtlxGraph --------------------------------------------------- */

typedef struct pending_node {
  char *name;
  lu_mtlx_strv ancestors;
} pending_node;

typedef struct pending_queue {
  pending_node *v;
  size_t n, cap;
} pending_queue;

/* queue.push_back({name, ancestors}) (copies). */
static int queue_push(pending_queue *q, const char *name, const lu_mtlx_strv *ancestors) {
  if (q->n == q->cap) {
    const size_t cap = q->cap ? q->cap * 2 : 16;
    pending_node *v = (pending_node *)realloc(q->v, cap * sizeof(*v));
    if (!v) return 0;
    q->v = v;
    q->cap = cap;
  }
  pending_node *p = &q->v[q->n];
  p->name = lu_mtlx_strdup(name);
  lu_mtlx_strv_init(&p->ancestors);
  int ok = p->name != NULL;
  for (size_t i = 0; ok && ancestors && i < ancestors->n; i++)
    ok = lu_mtlx_strv_push(&p->ancestors, ancestors->v[i]);
  q->n++;
  return ok;
}

static void queue_free(pending_queue *q) {
  for (size_t i = 0; i < q->n; i++) {
    free(q->v[i].name);
    lu_mtlx_strv_free(&q->v[i].ancestors);
  }
  free(q->v);
}

typedef struct fg_ctx {
  const lu_mtlx_elem *document;
  const lu_mtlx_library *library;
  const lu_mtlx_scope *outer;
  size_t max_expansions, expansions;
  lu_mtlx_elem result;
  pending_queue queue;
  lu_mtlx_sb *err;
} fg_ctx;

/* The graph implementation of def (FlattenMtlxGraph): 1 = expand, 2 = no
 * graph implementation (skip), 0 = error. */
static int fg_implementation(fg_ctx *c, const lu_mtlx_nodedef *def) {
  const lu_mtlx_elem *impl = NULL;
  size_t count = 0;
  int has_graph = 0;
  for (size_t i = 0; i < c->document->nchildren; i++) {
    const lu_mtlx_elem *child = &c->document->children[i];
    if ((!streq(child->tag, "nodegraph") && !streq(child->tag, "implementation")) ||
        !streq(lu_mtlx_attr_or(child, "nodedef", ""), def->name))
      continue;
    ++count;
    if (streq(child->tag, "nodegraph") || *lu_mtlx_attr_or(child, "nodegraph", "")) has_graph = 1;
    impl = child;
  }
  if (!has_graph) return 2;
  if (count != 1) return errf(c->err, "ambiguous graph implementation", NULL);
  if (*lu_mtlx_attr_or(impl, "target", ""))
    return errf(c->err, "unsupported targeted implementation", NULL);
  return 1;
}

/* One iteration of the FlattenMtlxGraph queue loop. */
static int fg_step(fg_ctx *c, size_t i) {
  const lu_mtlx_elem *node = find_node(&c->result, c->queue.v[i].name);
  if (!node) return errf(c->err, "missing pending graph node", NULL);
  lu_mtlx_scope rs, scope;
  if (!lu_mtlx_scope_with(&rs, &c->result, c->outer)) {
    lu_mtlx_scope_free(&rs);
    return errf(c->err, "out of memory", NULL);
  }
  const int sok = lu_mtlx_scope_with(&scope, node, &rs);
  lu_mtlx_scope_free(&rs);
  const lu_mtlx_nodedef *def =
      sok ? lu_mtlx_library_resolve_reference(c->library, node, &scope) : NULL;
  lu_mtlx_scope_free(&scope);
  if (!sok) return errf(c->err, "out of memory", NULL);
  if (!def) return errf(c->err, "cannot resolve graph node nodedef", NULL);
  const int impl = fg_implementation(c, def);
  if (impl != 1) return impl;
  const lu_mtlx_strv *anc = &c->queue.v[i].ancestors;
  for (size_t k = 0; k < anc->n; k++)
    if (streq(anc->v[k], def->name))
      return errf(c->err, "recursive graph implementation '", def->name, "'", NULL);
  if (c->expansions >= c->max_expansions)
    return errf(c->err, "graph expansion limit exceeded", NULL);
  lu_mtlx_strv old_nodes, ancestors;
  lu_mtlx_strv_init(&old_nodes);
  lu_mtlx_strv_init(&ancestors);
  int ok = 1;
  for (size_t k = 0; ok && k < c->result.nchildren; k++)
    if (lu_mtlx_is_node_elem(&c->result.children[k]))
      ok = lu_mtlx_strv_push(&old_nodes, lu_mtlx_name(&c->result.children[k]));
  for (size_t k = 0; ok && k < anc->n; k++) ok = lu_mtlx_strv_push(&ancestors, anc->v[k]);
  ok = ok && lu_mtlx_strv_push(&ancestors, def->name); /* def_name, before `result` changes */
  char *pending_name = ok ? lu_mtlx_strdup(c->queue.v[i].name) : NULL;
  if (!pending_name) ok = errf(c->err, "out of memory", NULL);
  lu_mtlx_elem next;
  lu_mtlx_elem_init(&next);
  if (ok)
    ok = lu_mtlx_flatten_graph_call(c->document, &c->result, c->library, c->outer, pending_name,
                                    &next, c->err);
  for (size_t k = 0; ok && k < next.nchildren; k++) {
    const lu_mtlx_elem *child = &next.children[k];
    if (lu_mtlx_is_node_elem(child) && !lu_mtlx_strv_contains(&old_nodes, lu_mtlx_name(child)) &&
        !queue_push(&c->queue, lu_mtlx_name(child), &ancestors))
      ok = errf(c->err, "out of memory", NULL);
  }
  if (ok) {
    lu_mtlx_elem_free(&c->result);
    lu_mtlx_elem_move(&c->result, &next);
    ++c->expansions;
  }
  lu_mtlx_elem_free(&next);
  free(pending_name);
  lu_mtlx_strv_free(&old_nodes);
  lu_mtlx_strv_free(&ancestors);
  return ok;
}

static int flatten_graph_impl(fg_ctx *c, const lu_mtlx_elem *graph) {
  lu_mtlx_strv checked;
  lu_mtlx_strv_init(&checked);
  const int rok =
      lu_mtlx_resolve_graph(c->document, graph, c->library, c->outer, &checked, c->err);
  lu_mtlx_strv_free(&checked);
  if (!rok) return 0;
  if (!lu_mtlx_elem_copy(&c->result, graph)) return errf(c->err, "out of memory", NULL);
  for (size_t k = 0; k < c->result.nchildren; k++)
    if (lu_mtlx_is_node_elem(&c->result.children[k]) &&
        !queue_push(&c->queue, lu_mtlx_name(&c->result.children[k]), NULL))
      return errf(c->err, "out of memory", NULL);
  for (size_t i = 0; i < c->queue.n; i++)
    if (!fg_step(c, i)) return 0;
  return 1;
}

/* FlattenMtlxGraph */
int lu_mtlx_flatten_graph(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                          const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                          size_t max_expansions, lu_mtlx_elem *flattened, lu_mtlx_sb *err) {
  lu_mtlx_sb ignored;
  lu_mtlx_sb_init(&ignored);
  if (!err) err = &ignored;
  lu_mtlx_err(err, "");
  if (!flattened) {
    lu_mtlx_err(err, "missing flattened graph output");
    lu_mtlx_sb_free(&ignored);
    return 0;
  }
  fg_ctx c;
  memset(&c, 0, sizeof(c));
  c.document = document;
  c.library = library;
  c.outer = outer;
  c.max_expansions = max_expansions;
  c.err = err;
  lu_mtlx_elem_init(&c.result);
  const int ok = flatten_graph_impl(&c, graph);
  if (ok) {
    lu_mtlx_elem_free(flattened);
    lu_mtlx_elem_move(flattened, &c.result);
  }
  lu_mtlx_elem_free(&c.result);
  queue_free(&c.queue);
  lu_mtlx_sb_free(&ignored);
  return ok;
}
