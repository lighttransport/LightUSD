/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX nodegraph validation and ordering (C11 port of ResolveMtlxGraph
 * and ResolveMtlxGraphs of src/core/mtlx-resolve.cc, with their anonymous
 * helpers). The ~450-line C++ ResolveMtlxGraph is split into one function
 * per phase; its lambdas become functions over a shared context.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

/* ---- shared helpers ----------------------------------------------------- */

/* err = concatenation of the NULL-terminated string arguments. */
static void errf(lu_mtlx_sb *err, ...) {
  lu_mtlx_sb m;
  lu_mtlx_sb_init(&m);
  va_list ap;
  va_start(ap, err);
  for (const char *s = va_arg(ap, const char *); s; s = va_arg(ap, const char *))
    lu_mtlx_sb_puts(&m, s);
  va_end(ap);
  lu_mtlx_err(err, m.oom ? "out of memory" : lu_mtlx_sb_str(&m));
  lu_mtlx_sb_free(&m);
}

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* NamespaceAt (anonymous). *ns borrowed. */
static int namespace_at(const lu_mtlx_elem *e, const lu_mtlx_scope *outer, const char **ns) {
  const char *v = lu_mtlx_get_attr(e, "namespace");
  if (v) {
    *ns = v;
    return 1;
  }
  return outer && lu_mtlx_scoped_attr("namespace", outer, ns);
}

/* QualifyWith (anonymous); replaces out. */
static void qualify_with(const char *ns, const char *name, lu_mtlx_sb *out) {
  lu_mtlx_sb_clear(out);
  const size_t nl = ns ? strlen(ns) : 0;
  if (nl && !(strncmp(name, ns, nl) == 0 && name[nl] == ':')) {
    lu_mtlx_sb_puts(out, ns);
    lu_mtlx_sb_putc(out, ':');
  }
  lu_mtlx_sb_puts(out, name);
}

/* MtlxChannelConversionMatches (anonymous). */
static int channel_conversion_matches(const lu_mtlx_elem *input, const char *source_type,
                                      const char *input_type) {
  const char *channels = lu_mtlx_attr_or(input, "channels", "");
  if (!*channels || !streq(source_type, "float")) return 0;
  const size_t expected = streq(input_type, "color3") || streq(input_type, "vector3")   ? 3
                          : streq(input_type, "color4") || streq(input_type, "vector4") ? 4
                          : streq(input_type, "vector2")                                ? 2
                                                                                        : 0;
  if (expected == 0 || strlen(channels) != expected) return 0;
  const char first = channels[0];
  if (!strchr("rgbaxyzw", first)) return 0;
  for (const char *c = channels; *c; c++)
    if (*c != first) return 0;
  return 1;
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

/* FindMtlxScopedChild (anonymous). */
static const lu_mtlx_elem *find_scoped_child(const lu_mtlx_elem *parent, const char *tag,
                                             const char *name, const lu_mtlx_elem *at,
                                             const lu_mtlx_scope *outer) {
  const char *ns = NULL;
  if (namespace_at(at, outer, &ns)) {
    lu_mtlx_sb q;
    lu_mtlx_sb_init(&q);
    qualify_with(ns, name, &q);
    const lu_mtlx_elem *found = find_child(parent, tag, lu_mtlx_sb_str(&q));
    lu_mtlx_sb_free(&q);
    if (found) return found;
  }
  return find_child(parent, tag, name);
}

/* FindMtlxNode (anonymous). */
static const lu_mtlx_elem *find_node(const lu_mtlx_elem *graph, const char *name) {
  for (size_t i = 0; i < graph->nchildren; i++) {
    const lu_mtlx_elem *c = &graph->children[i];
    if (lu_mtlx_is_node_elem(c) && streq(lu_mtlx_name(c), name)) return c;
  }
  return NULL;
}

/* {e} + outer, or {} + outer when e is NULL. 0 on allocation failure. */
static int scope_of(lu_mtlx_scope *out, const lu_mtlx_elem *e, const lu_mtlx_scope *outer) {
  if (e) return lu_mtlx_scope_with(out, e, outer);
  lu_mtlx_scope_init(out);
  for (size_t i = 0; outer && i < outer->n; i++)
    if (!lu_mtlx_scope_push(out, outer->v[i])) return 0;
  return 1;
}

/* MtlxNodeOutputType (anonymous): *type borrowed from the library/node. */
static int node_output_type(const lu_mtlx_library *library, const lu_mtlx_elem *node,
                            const lu_mtlx_scope *outer, const char *output, const char **type,
                            lu_mtlx_sb *err) {
  lu_mtlx_scope scope;
  if (!scope_of(&scope, node, outer)) {
    lu_mtlx_scope_free(&scope);
    lu_mtlx_err(err, "out of memory");
    return 0;
  }
  const lu_mtlx_nodedef *def = lu_mtlx_library_resolve_reference(library, node, &scope);
  lu_mtlx_scope_free(&scope);
  if (def) {
    lu_mtlx_ports outputs;
    if (!lu_mtlx_library_active_outputs(library, def, &outputs)) {
      lu_mtlx_err(err, "out of memory");
      return 0;
    }
    int found = 0;
    const char *requested = *output ? output : "out";
    if (!*output && outputs.n == 1) {
      *type = outputs.v[0].type;
      found = 1;
    }
    for (size_t i = 0; !found && i < outputs.n; i++)
      if (streq(outputs.v[i].name, requested)) {
        *type = outputs.v[i].type;
        found = 1;
      }
    if (!found && outputs.n == 1 && streq(outputs.v[0].name, "out") && streq(requested, "out")) {
      *type = outputs.v[0].type;
      found = 1;
    }
    free(outputs.v);
    if (!found)
      errf(err, "node '", lu_mtlx_name(node), "' has no output '", requested, "'", NULL);
    return found;
  }
  if (!*output || streq(output, "out")) {
    const char *declared = lu_mtlx_get_attr(node, "type");
    if (declared) {
      *type = declared;
      return 1;
    }
  }
  errf(err, "cannot resolve output type for node '", lu_mtlx_name(node), "'", NULL);
  return 0;
}

/* ParseMtlxAuthoredValue (anonymous). */
static int parse_authored_value(const char *type, const char *text) {
  lu_mtlx_typed_value v;
  lu_mtlx_typed_value_init(&v);
  int ok = lu_mtlx_parse_value(type, text, &v);
  if (!ok && !strchr(text, ',') && lu_mtlx_type_arity(type) != 0) {
    lu_mtlx_sb cs;
    lu_mtlx_sb_init(&cs);
    size_t nfields = 0;
    const char *p = text;
    while (*p) {
      while (*p && strchr(" \t\n\r\f", *p)) p++;
      if (!*p) break;
      const char *b = p;
      while (*p && !strchr(" \t\n\r\f", *p)) p++;
      if (nfields++) lu_mtlx_sb_putc(&cs, ',');
      lu_mtlx_sb_put(&cs, b, (size_t)(p - b));
    }
    if (nfields && !cs.oom) ok = lu_mtlx_parse_value(type, lu_mtlx_sb_str(&cs), &v);
    lu_mtlx_sb_free(&cs);
  }
  lu_mtlx_typed_value_free(&v);
  return ok;
}

/* ValidateMtlxGraphOutputAttributes (anonymous). */
static int validate_output_attributes(const lu_mtlx_elem *output, const char *type,
                                      lu_mtlx_sb *err) {
  if (lu_mtlx_get_attr(output, "interfacename")) {
    errf(err, "graph output '", lu_mtlx_name(output), "' cannot use interfacename", NULL);
    return 0;
  }
  const char *literal = lu_mtlx_get_attr(output, "value");
  if (literal) {
    if (!*type || (lu_mtlx_is_builtin_type(type) && !parse_authored_value(type, literal))) {
      errf(err, "invalid literal graph output '", lu_mtlx_name(output), "'", NULL);
      return 0;
    }
  }
  return 1;
}

/* ---- ResolveMtlxGraph --------------------------------------------------- */

typedef struct edge { size_t from, to; } edge;
typedef struct edges { edge *v; size_t n, cap; } edges;

static int edge_push(edges *e, size_t from, size_t to) {
  if (e->n == e->cap) {
    const size_t cap = e->cap ? e->cap * 2 : 16;
    edge *v = (edge *)realloc(e->v, cap * sizeof(edge));
    if (!v) return 0;
    e->v = v;
    e->cap = cap;
  }
  e->v[e->n].from = from;
  e->v[e->n].to = to;
  e->n++;
  return 1;
}

/* Kahn ordering shared by ResolveMtlxGraph / ResolveMtlxGraphs: successors
 * of each source in insertion order, ready queue FIFO. order[] receives the
 * item indices; returns the number ordered (or (size_t)-1 on oom). */
static size_t topo_order(size_t count, const edges *es, size_t *order) {
  size_t *indeg = (size_t *)calloc(count + 1, sizeof(size_t));
  size_t *start = (size_t *)calloc(count + 1, sizeof(size_t));
  size_t *succ = (size_t *)malloc((es->n + 1) * sizeof(size_t));
  size_t *fill = (size_t *)calloc(count + 1, sizeof(size_t));
  size_t done = (size_t)-1;
  if (!indeg || !start || !succ || !fill) goto out;
  for (size_t i = 0; i < es->n; i++) {
    start[es->v[i].from + 1]++;
    indeg[es->v[i].to]++;
  }
  for (size_t i = 0; i < count; i++) start[i + 1] += start[i];
  for (size_t i = 0; i < es->n; i++) {
    const size_t f = es->v[i].from;
    succ[start[f] + fill[f]++] = es->v[i].to;
  }
  done = 0;
  for (size_t i = 0; i < count; i++)
    if (indeg[i] == 0) order[done++] = i;
  for (size_t cursor = 0; cursor < done; cursor++) {
    const size_t s = order[cursor];
    for (size_t k = start[s]; k < start[s + 1]; k++)
      if (--indeg[succ[k]] == 0) order[done++] = succ[k];
  }
out:
  free(indeg);
  free(start);
  free(succ);
  free(fill);
  return done;
}

typedef struct rg_ctx {
  const lu_mtlx_elem *document, *graph;
  const lu_mtlx_library *library;
  const lu_mtlx_scope *outer;
  lu_mtlx_scope graph_scope;
  lu_mtlx_ports graph_inputs, graph_outputs;
  const lu_mtlx_elem **nodes;
  size_t nnodes;
  edges es;
  lu_mtlx_sb *err;
} rg_ctx;

static int rg_oom(rg_ctx *c) {
  lu_mtlx_err(c->err, "out of memory");
  return 0;
}

static const lu_mtlx_port *find_port(const lu_mtlx_ports *ports, const char *name) {
  for (size_t i = 0; i < ports->n; i++)
    if (streq(ports->v[i].name, name)) return &ports->v[i];
  return NULL;
}

/* ResolveMtlxGraph: graph nodedef lookup (direct, or through a sibling
 * <implementation>), then its active inputs/outputs. */
static int rg_graph_def(rg_ctx *c) {
  const lu_mtlx_elem *graph = c->graph;
  const lu_mtlx_nodedef *graph_def =
      lu_mtlx_library_resolve_reference(c->library, graph, c->outer);
  if (!graph_def && !lu_mtlx_get_attr(graph, "nodedef")) {
    for (size_t i = 0; i < c->document->nchildren; i++) {
      const lu_mtlx_elem *child = &c->document->children[i];
      if (!streq(child->tag, "implementation") ||
          !streq(lu_mtlx_attr_or(child, "nodegraph", ""), lu_mtlx_name(graph)))
        continue;
      const char *nodedef = lu_mtlx_get_attr(child, "nodedef");
      if (!nodedef) continue;
      /* `reference = graph; SetAttr("nodedef")`: with a nodedef attribute only
       * the attributes take part in resolution, so children are not copied. */
      lu_mtlx_elem reference;
      lu_mtlx_elem_init(&reference);
      int ok = lu_mtlx_elem_set_tag(&reference, graph->tag);
      for (size_t a = 0; ok && a < graph->nattrs; a++)
        ok = lu_mtlx_attr_append(&reference, graph->attrs[a].key, graph->attrs[a].value);
      ok = ok && lu_mtlx_attr_set(&reference, "nodedef", nodedef);
      if (ok) graph_def = lu_mtlx_library_resolve_reference(c->library, &reference, c->outer);
      lu_mtlx_elem_free(&reference);
      if (!ok) return rg_oom(c);
      if (graph_def) break;
    }
  }
  if (lu_mtlx_get_attr(graph, "nodedef") && !graph_def) {
    errf(c->err, "cannot resolve nodedef '", lu_mtlx_attr_or(graph, "nodedef", ""),
         "' for nodegraph '", lu_mtlx_name(graph), "'", NULL);
    return 0;
  }
  if (graph_def) {
    if (!lu_mtlx_library_active_inputs(c->library, graph_def, &c->graph_inputs) ||
        !lu_mtlx_library_active_outputs(c->library, graph_def, &c->graph_outputs))
      return rg_oom(c);
  }
  return 1;
}

/* Default output name of validate_graph_connection: the only output, else
 * "out". */
static const char *only_output_name(const lu_mtlx_elem *g) {
  const lu_mtlx_elem *only = NULL;
  for (size_t i = 0; i < g->nchildren; i++) {
    if (!streq(g->children[i].tag, "output")) continue;
    if (only) {
      only = NULL;
      break;
    }
    only = &g->children[i];
  }
  return only ? lu_mtlx_name(only) : "out";
}

/* Type checks of validate_graph_connection after the source type is known. */
static int rg_check_graph_output_type(rg_ctx *c, const lu_mtlx_elem *input,
                                      const char *input_type, const lu_mtlx_elem *source_graph,
                                      const lu_mtlx_elem *output, const char *graph_name,
                                      const char *output_name, const char *source_type) {
  const lu_mtlx_nodedef *sgd =
      lu_mtlx_library_resolve_reference(c->library, source_graph, c->outer);
  lu_mtlx_ports sgo = {NULL, 0};
  if (sgd && !lu_mtlx_library_active_outputs(c->library, sgd, &sgo)) return rg_oom(c);
  const lu_mtlx_port *declared_output = find_port(&sgo, output_name);
  const char *otype = lu_mtlx_attr_or(output, "type", "");
  const char *resolved = *otype ? otype : declared_output ? declared_output->type : "";
  int ok = 0;
  if (*otype && declared_output && !streq(otype, declared_output->type))
    errf(c->err, "output '", output_name, "' of nodegraph '", graph_name,
         "' disagrees with its nodedef output type", NULL);
  else if (!validate_output_attributes(output, resolved, c->err))
    ;
  else if (*resolved && !streq(resolved, source_type))
    errf(c->err, "output '", output_name, "' of nodegraph '", graph_name, "' declares type ",
         resolved, ", source has type ", source_type, NULL);
  else if (!*resolved)
    errf(c->err, "cannot resolve output type '", output_name, "' of nodegraph '", graph_name,
         "'", NULL);
  else if (!streq(resolved, input_type) &&
           !channel_conversion_matches(input, resolved, input_type))
    errf(c->err, "nodegraph connection to input '", lu_mtlx_name(input), "' has type ",
         resolved, ", expected ", input_type, NULL);
  else
    ok = 1;
  free(sgo.v);
  return ok;
}

/* validate_graph_connection lambda of ResolveMtlxGraph. */
static int rg_validate_graph_connection(rg_ctx *c, const lu_mtlx_elem *input,
                                        const char *input_type,
                                        const lu_mtlx_scope *input_scope) {
  const char *graph_name = lu_mtlx_get_attr(input, "nodegraph");
  if (!graph_name) return 0;
  const char *input_name = lu_mtlx_name(input);
  const lu_mtlx_elem *source_graph =
      find_scoped_child(c->document, "nodegraph", graph_name, input, input_scope);
  if (!source_graph) {
    errf(c->err, "input '", input_name, "' references missing nodegraph '", graph_name, "'",
         NULL);
    return 0;
  }
  const char *output_name = lu_mtlx_attr_or(input, "output", "");
  if (!*output_name) output_name = only_output_name(source_graph);
  const lu_mtlx_elem *output = find_child(source_graph, "output", output_name);
  if (!output) {
    errf(c->err, "nodegraph '", graph_name, "' has no output '", output_name, "'", NULL);
    return 0;
  }
  const char *source_name = lu_mtlx_attr_or(output, "nodename", "");
  const lu_mtlx_elem *source = find_node(source_graph, source_name);
  if (!source) {
    errf(c->err, "output '", output_name, "' of nodegraph '", graph_name,
         "' references missing node '", source_name, "'", NULL);
    return 0;
  }
  lu_mtlx_scope source_scope;
  if (!scope_of(&source_scope, source_graph, c->outer)) {
    lu_mtlx_scope_free(&source_scope);
    return rg_oom(c);
  }
  const char *source_type = NULL;
  const int ok = node_output_type(c->library, source, &source_scope,
                                  lu_mtlx_attr_or(output, "output", ""), &source_type, c->err);
  lu_mtlx_scope_free(&source_scope);
  if (!ok) return 0;
  return rg_check_graph_output_type(c, input, input_type, source_graph, output, graph_name,
                                    output_name, source_type);
}

/* validate_document_output lambda of ResolveMtlxGraph. */
static int rg_validate_document_output(rg_ctx *c, const lu_mtlx_elem *input,
                                       const char *input_type) {
  const char *output_name = lu_mtlx_get_attr(input, "output");
  if (!output_name) return 0;
  const lu_mtlx_elem *doc_output = find_child(c->document, "output", output_name);
  if (!doc_output) {
    errf(c->err, "input '", lu_mtlx_name(input), "' references missing document output '",
         output_name, "'", NULL);
    return 0;
  }
  const char *output_type = lu_mtlx_attr_or(doc_output, "type", "");
  if (!*output_type || !validate_output_attributes(doc_output, output_type, c->err)) {
    if (c->err->n == 0)
      errf(c->err, "cannot resolve document output type for '", output_name, "'", NULL);
    return 0;
  }
  const char *source_name = lu_mtlx_attr_or(doc_output, "nodename", "");
  const lu_mtlx_elem *source = find_node(c->document, source_name);
  if (!source) {
    errf(c->err, "document output '", output_name, "' references missing node '", source_name,
         "'", NULL);
    return 0;
  }
  lu_mtlx_scope ds;
  if (!scope_of(&ds, c->document, NULL)) {
    lu_mtlx_scope_free(&ds);
    return rg_oom(c);
  }
  const char *source_type = NULL;
  const int ok = node_output_type(c->library, source, &ds,
                                  lu_mtlx_attr_or(doc_output, "output", ""), &source_type, c->err);
  lu_mtlx_scope_free(&ds);
  if (!ok) return 0;
  if (!streq(output_type, source_type) ||
      (!streq(output_type, input_type) &&
       !channel_conversion_matches(input, output_type, input_type))) {
    errf(c->err, "document output '", output_name, "' has incompatible type", NULL);
    return 0;
  }
  return 1;
}

/* Graph-input binding to a document node (ResolveMtlxGraph). */
static int rg_graph_input_nodename(rg_ctx *c, const lu_mtlx_elem *input, const char *input_type) {
  const char *source_name = lu_mtlx_attr_or(input, "nodename", "");
  const lu_mtlx_elem *source = find_node(c->document, source_name);
  if (!source) {
    errf(c->err, "graph input '", lu_mtlx_name(input), "' references missing document node '",
         source_name, "'", NULL);
    return 0;
  }
  lu_mtlx_scope ds;
  if (!scope_of(&ds, c->document, NULL)) {
    lu_mtlx_scope_free(&ds);
    return rg_oom(c);
  }
  const char *source_type = NULL;
  const int ok = node_output_type(c->library, source, &ds, lu_mtlx_attr_or(input, "output", ""),
                                  &source_type, c->err);
  lu_mtlx_scope_free(&ds);
  if (!ok) return 0;
  if (!streq(source_type, input_type) &&
      !channel_conversion_matches(input, source_type, input_type)) {
    errf(c->err, "graph input '", lu_mtlx_name(input), "' has incompatible source type", NULL);
    return 0;
  }
  return 1;
}

/* ResolveMtlxGraph: the compound graph inputs loop. */
static int rg_graph_inputs(rg_ctx *c) {
  for (size_t i = 0; i < c->graph->nchildren; i++) {
    const lu_mtlx_elem *input = &c->graph->children[i];
    if (!streq(input->tag, "input") ||
        (!lu_mtlx_get_attr(input, "nodegraph") && !lu_mtlx_get_attr(input, "nodename") &&
         !lu_mtlx_get_attr(input, "output")))
      continue;
    const char *input_type = lu_mtlx_attr_or(input, "type", "");
    if (!*input_type) {
      const lu_mtlx_port *p = find_port(&c->graph_inputs, lu_mtlx_name(input));
      if (p) input_type = p->type;
    }
    if (!*input_type) {
      errf(c->err, "cannot resolve graph input type for '", lu_mtlx_name(input), "'", NULL);
      return 0;
    }
    const char *value = lu_mtlx_get_attr(input, "value");
    if (value && lu_mtlx_is_builtin_type(input_type) && !parse_authored_value(input_type, value)) {
      errf(c->err, "invalid value for graph input '", lu_mtlx_name(input), "'", NULL);
      return 0;
    }
    if (lu_mtlx_get_attr(input, "nodegraph")) {
      lu_mtlx_scope input_scope;
      if (!scope_of(&input_scope, input, &c->graph_scope)) {
        lu_mtlx_scope_free(&input_scope);
        return rg_oom(c);
      }
      const int ok = rg_validate_graph_connection(c, input, input_type, &input_scope);
      lu_mtlx_scope_free(&input_scope);
      if (!ok) return 0;
    } else if (lu_mtlx_get_attr(input, "nodename")) {
      if (!rg_graph_input_nodename(c, input, input_type)) return 0;
    } else if (!rg_validate_document_output(c, input, input_type)) {
      return 0;
    }
  }
  return 1;
}

/* ResolveMtlxGraph: collect the node children (named, unique). */
static int rg_collect_nodes(rg_ctx *c) {
  c->nodes = (const lu_mtlx_elem **)malloc((c->graph->nchildren + 1) * sizeof(*c->nodes));
  if (!c->nodes) return rg_oom(c);
  for (size_t i = 0; i < c->graph->nchildren; i++) {
    const lu_mtlx_elem *child = &c->graph->children[i];
    if (!lu_mtlx_is_node_elem(child)) continue;
    const char *name = lu_mtlx_name(child);
    if (!*name) {
      lu_mtlx_err(c->err, "nodegraph contains an unnamed node");
      return 0;
    }
    if (find_node(c->graph, name) != child) {
      errf(c->err, "duplicate node name '", name, "'", NULL);
      return 0;
    }
    c->nodes[c->nnodes++] = child;
  }
  return 1;
}

/* ResolveMtlxGraph: a node input bound with `nodename` (adds an edge). */
static int rg_input_nodename(rg_ctx *c, size_t i, const lu_mtlx_elem *node,
                             const lu_mtlx_elem *input, const char *input_type,
                             const char *node_name) {
  const char *input_name = lu_mtlx_name(input);
  const lu_mtlx_elem *source = find_node(c->graph, node_name);
  if (!source) {
    errf(c->err, "input '", input_name, "' on node '", lu_mtlx_name(node),
         "' references missing node '", node_name, "'", NULL);
    return 0;
  }
  const char *source_type = NULL;
  if (!node_output_type(c->library, source, &c->graph_scope, lu_mtlx_attr_or(input, "output", ""),
                        &source_type, c->err))
    return 0;
  if (!streq(source_type, input_type) &&
      !channel_conversion_matches(input, source_type, input_type)) {
    errf(c->err, "connection to input '", input_name, "' on node '", lu_mtlx_name(node),
         "' has type ", source_type, ", expected ", input_type, NULL);
    return 0;
  }
  size_t pos = 0;
  while (pos < c->nnodes && c->nodes[pos] != source) ++pos;
  if (pos == c->nnodes) {
    lu_mtlx_err(c->err, "internal node lookup failure");
    return 0;
  }
  if (!edge_push(&c->es, pos, i)) return rg_oom(c);
  return 1;
}

/* ResolveMtlxGraph: a node input bound with `interfacename`. */
static int rg_input_interface(rg_ctx *c, const lu_mtlx_elem *input, const char *input_type,
                              const char *interface_name) {
  const lu_mtlx_elem *iface = find_child(c->graph, "input", interface_name);
  const lu_mtlx_port *declared = find_port(&c->graph_inputs, interface_name);
  if (!iface && !declared) {
    errf(c->err, "input '", lu_mtlx_name(input), "' references missing graph interface '",
         interface_name, "'", NULL);
    return 0;
  }
  const char *itype = iface && *lu_mtlx_attr_or(iface, "type", "")
                          ? lu_mtlx_attr_or(iface, "type", "")
                          : (declared ? declared->type : "");
  if (*itype && declared && !streq(itype, declared->type)) {
    errf(c->err, "graph interface '", interface_name, "' disagrees with its nodedef input type",
         NULL);
    return 0;
  }
  if (!streq(itype, input_type) && !channel_conversion_matches(input, itype, input_type)) {
    errf(c->err, "graph interface '", interface_name, "' has type ", itype, ", expected ",
         input_type, NULL);
    return 0;
  }
  return 1;
}

/* ResolveMtlxGraph: one input of node i. */
static int rg_node_input(rg_ctx *c, size_t i, const lu_mtlx_elem *node,
                         const lu_mtlx_scope *node_scope, const lu_mtlx_ports *declared_inputs,
                         const lu_mtlx_elem *input) {
  const char *input_name = lu_mtlx_name(input);
  const lu_mtlx_port *declared = find_port(declared_inputs, input_name);
  if (!declared) {
    errf(c->err, "input '", input_name, "' is not declared by the nodedef of node '",
         lu_mtlx_name(node), "'", NULL);
    return 0;
  }
  const char *input_type = lu_mtlx_attr_or(input, "type", declared->type);
  const char *authored_type = lu_mtlx_attr_or(input, "type", "");
  if (*authored_type && !streq(authored_type, declared->type)) {
    errf(c->err, "input '", input_name, "' on node '", lu_mtlx_name(node), "' has type ",
         authored_type, ", nodedef requires ", declared->type, NULL);
    return 0;
  }
  if (!*input_type) {
    errf(c->err, "cannot resolve type of input '", input_name, "' on node '", lu_mtlx_name(node),
         "'", NULL);
    return 0;
  }
  const char *node_name = lu_mtlx_get_attr(input, "nodename");
  const char *graph_name = lu_mtlx_get_attr(input, "nodegraph");
  const char *interface_name = lu_mtlx_get_attr(input, "interfacename");
  const char *value = lu_mtlx_get_attr(input, "value");
  const int has_output = lu_mtlx_get_attr(input, "output") != NULL;
  const int binding_count = (node_name != NULL) + (graph_name != NULL) +
                            (interface_name != NULL) + (value != NULL) +
                            (has_output && !node_name && !graph_name);
  if (binding_count == 0) {
    errf(c->err, "input '", input_name, "' has no authored binding", NULL);
    return 0;
  }
  if (binding_count > 1) {
    errf(c->err, "input '", input_name, "' has multiple connection bindings", NULL);
    return 0;
  }
  if (node_name) return rg_input_nodename(c, i, node, input, input_type, node_name);
  if (graph_name) {
    lu_mtlx_scope input_scope;
    if (!scope_of(&input_scope, input, node_scope)) {
      lu_mtlx_scope_free(&input_scope);
      return rg_oom(c);
    }
    const int ok = rg_validate_graph_connection(c, input, input_type, &input_scope);
    lu_mtlx_scope_free(&input_scope);
    return ok;
  }
  if (interface_name) return rg_input_interface(c, input, input_type, interface_name);
  if (has_output) return rg_validate_document_output(c, input, input_type);
  if (value) {
    if (lu_mtlx_is_builtin_type(input_type) && !parse_authored_value(input_type, value)) {
      errf(c->err, "invalid value for input '", input_name, "' on node '", lu_mtlx_name(node),
           "'", NULL);
      return 0;
    }
  } else if (!declared->has_value && !lu_mtlx_is_builtin_type(input_type)) {
    errf(c->err, "input '", input_name, "' has no value or nodedef default", NULL);
    return 0;
  }
  return 1;
}

/* ResolveMtlxGraph: definition, category, type and inputs of node i. */
static int rg_node(rg_ctx *c, size_t i) {
  const lu_mtlx_elem *node = c->nodes[i];
  lu_mtlx_scope node_scope;
  if (!scope_of(&node_scope, node, &c->graph_scope)) {
    lu_mtlx_scope_free(&node_scope);
    return rg_oom(c);
  }
  int ok = 0;
  lu_mtlx_ports declared = {NULL, 0};
  lu_mtlx_sb ot;
  lu_mtlx_sb_init(&ot);
  const lu_mtlx_nodedef *def = lu_mtlx_library_resolve_reference(c->library, node, &node_scope);
  const char *ntype = lu_mtlx_attr_or(node, "type", "");
  if (!def) {
    errf(c->err, "cannot resolve nodedef for node '", lu_mtlx_name(node), "' in category '",
         node->tag, "'", NULL);
  } else if (!streq(node->tag, def->node) && !streq(node->tag, def->key)) {
    errf(c->err, "node '", lu_mtlx_name(node), "' category '", node->tag,
         "' does not match nodedef category '", def->node, "'", NULL);
  } else if (!lu_mtlx_library_out_type(c->library, def, &ot)) {
    rg_oom(c);
  } else if (*ntype && !streq(lu_mtlx_sb_str(&ot), "multioutput") &&
             !streq(ntype, lu_mtlx_sb_str(&ot))) {
    errf(c->err, "node '", lu_mtlx_name(node), "' has type ", ntype, ", nodedef requires ",
         lu_mtlx_sb_str(&ot), NULL);
  } else if (!lu_mtlx_library_active_inputs(c->library, def, &declared)) {
    rg_oom(c);
  } else {
    ok = 1;
    for (size_t k = 0; ok && k < node->nchildren; k++) {
      const lu_mtlx_elem *input = &node->children[k];
      if (!streq(input->tag, "input")) continue;
      ok = rg_node_input(c, i, node, &node_scope, &declared, input);
    }
  }
  free(declared.v);
  lu_mtlx_sb_free(&ot);
  lu_mtlx_scope_free(&node_scope);
  return ok;
}

/* ResolveMtlxGraph: the graph outputs loop. */
static int rg_graph_outputs(rg_ctx *c) {
  for (size_t i = 0; i < c->graph->nchildren; i++) {
    const lu_mtlx_elem *output = &c->graph->children[i];
    if (!streq(output->tag, "output")) continue;
    const char *oname = lu_mtlx_name(output);
    const lu_mtlx_port *declared = find_port(&c->graph_outputs, oname);
    const char *authored = lu_mtlx_attr_or(output, "type", "");
    const char *output_type = *authored ? authored : declared ? declared->type : "";
    if (declared && *authored && !streq(authored, declared->type)) {
      errf(c->err, "graph output '", oname, "' disagrees with its nodedef output type", NULL);
      return 0;
    }
    if (!validate_output_attributes(output, output_type, c->err)) return 0;
    const char *source_name = lu_mtlx_attr_or(output, "nodename", "");
    if (!lu_mtlx_get_attr(output, "nodename") && !lu_mtlx_get_attr(output, "nodegraph") &&
        !lu_mtlx_get_attr(output, "output")) {
      if (!*output_type) {
        errf(c->err, "cannot resolve graph output type for '", oname, "'", NULL);
        return 0;
      }
      continue;
    }
    const lu_mtlx_elem *source = find_node(c->graph, source_name);
    if (!source) {
      errf(c->err, "graph output '", oname, "' references missing node '", source_name, "'",
           NULL);
      return 0;
    }
    const char *source_type = NULL;
    if (!node_output_type(c->library, source, &c->graph_scope,
                          lu_mtlx_attr_or(output, "output", ""), &source_type, c->err))
      return 0;
    if (!*output_type) {
      errf(c->err, "cannot resolve graph output type for '", oname, "'", NULL);
      return 0;
    }
    if (!streq(output_type, source_type)) {
      errf(c->err, "graph output '", oname, "' has type ", output_type, ", source has type ",
           source_type, NULL);
      return 0;
    }
  }
  return 1;
}

/* ResolveMtlxGraph: topological order of the nodes. */
static int rg_order(rg_ctx *c, lu_mtlx_strv *node_order) {
  size_t *order = (size_t *)malloc((c->nnodes + 1) * sizeof(size_t));
  if (!order) return rg_oom(c);
  const size_t done = topo_order(c->nnodes, &c->es, order);
  int ok = done != (size_t)-1;
  for (size_t k = 0; ok && k < done; k++)
    ok = lu_mtlx_strv_push(node_order, lu_mtlx_name(c->nodes[order[k]]));
  free(order);
  if (!ok) {
    lu_mtlx_strv_free(node_order);
    return rg_oom(c);
  }
  if (node_order->n != c->nnodes) {
    lu_mtlx_strv_free(node_order);
    lu_mtlx_err(c->err, "nodegraph contains a cycle");
    return 0;
  }
  return 1;
}

static int resolve_graph_impl(rg_ctx *c, lu_mtlx_strv *node_order) {
  if (!streq(c->graph->tag, "nodegraph")) {
    lu_mtlx_err(c->err, "expected a nodegraph element");
    return 0;
  }
  if (!scope_of(&c->graph_scope, c->graph, c->outer)) return rg_oom(c);
  if (!rg_graph_def(c) || !rg_graph_inputs(c) || !rg_collect_nodes(c)) return 0;
  for (size_t i = 0; i < c->nnodes; i++)
    if (!rg_node(c, i)) return 0;
  if (!rg_graph_outputs(c)) return 0;
  return rg_order(c, node_order);
}

/* ResolveMtlxGraph: node_order is cleared first and filled on success. */
int lu_mtlx_resolve_graph(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                          const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                          lu_mtlx_strv *node_order, lu_mtlx_sb *err) {
  lu_mtlx_sb ignored;
  lu_mtlx_sb_init(&ignored);
  if (!err) err = &ignored;
  lu_mtlx_err(err, "");
  if (!node_order) {
    lu_mtlx_err(err, "missing resolved-graph output");
    lu_mtlx_sb_free(&ignored);
    return 0;
  }
  lu_mtlx_strv_free(node_order);
  rg_ctx c;
  memset(&c, 0, sizeof(c));
  c.document = document;
  c.graph = graph;
  c.library = library;
  c.outer = outer;
  c.err = err;
  lu_mtlx_scope_init(&c.graph_scope);
  const int ok = resolve_graph_impl(&c, node_order);
  lu_mtlx_scope_free(&c.graph_scope);
  free(c.graph_inputs.v);
  free(c.graph_outputs.v);
  free((void *)c.nodes);
  free(c.es.v);
  lu_mtlx_sb_free(&ignored);
  return ok;
}

/* ---- ResolveMtlxGraphs -------------------------------------------------- */

typedef struct rgs_ctx {
  const lu_mtlx_elem *document;
  const lu_mtlx_elem **graphs;
  size_t ngraphs;
  edges es;
  lu_mtlx_sb *err;
} rgs_ctx;

/* add_dependency lambda of ResolveMtlxGraphs. */
static int rgs_add_dependency(rgs_ctx *c, size_t consumer, const char *name,
                              const lu_mtlx_elem *at, const lu_mtlx_scope *scope) {
  const char *ns = NULL;
  lu_mtlx_sb q;
  lu_mtlx_sb_init(&q);
  if (namespace_at(at, scope, &ns)) qualify_with(ns, name, &q);
  else lu_mtlx_sb_puts(&q, name);
  if (q.oom) {
    lu_mtlx_err(c->err, "out of memory");
    return 0;
  }
  const char *qualified = lu_mtlx_sb_str(&q);
  size_t dep = 0;
  while (dep < c->ngraphs && !streq(lu_mtlx_name(c->graphs[dep]), qualified)) ++dep;
  if (dep == c->ngraphs && !streq(qualified, name)) {
    dep = 0;
    while (dep < c->ngraphs && !streq(lu_mtlx_name(c->graphs[dep]), name)) ++dep;
  }
  lu_mtlx_sb_free(&q);
  if (dep == c->ngraphs) {
    errf(c->err, "nodegraph connection references missing graph '", name, "'", NULL);
    return 0;
  }
  if (!edge_push(&c->es, dep, consumer)) {
    lu_mtlx_err(c->err, "out of memory");
    return 0;
  }
  return 1;
}

typedef struct ptr_set { const lu_mtlx_elem **v; size_t n, cap; } ptr_set;

static int ptr_has(const ptr_set *s, const lu_mtlx_elem *e) {
  for (size_t i = 0; i < s->n; i++)
    if (s->v[i] == e) return 1;
  return 0;
}
static int ptr_add(ptr_set *s, const lu_mtlx_elem *e) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 16;
    const lu_mtlx_elem **v = (const lu_mtlx_elem **)realloc((void *)s->v, cap * sizeof(*v));
    if (!v) return 0;
    s->v = v;
    s->cap = cap;
  }
  s->v[s->n++] = e;
  return 1;
}
static void ptr_del(ptr_set *s, const lu_mtlx_elem *e) {
  for (size_t i = 0; i < s->n; i++)
    if (s->v[i] == e) {
      s->v[i] = s->v[--s->n];
      return;
    }
}

typedef struct pend { const lu_mtlx_elem *e; int exiting; } pend;
typedef struct pend_stack { pend *v; size_t n, cap; } pend_stack;

static int pend_push(pend_stack *s, const lu_mtlx_elem *e, int exiting) {
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 16;
    pend *v = (pend *)realloc(s->v, cap * sizeof(pend));
    if (!v) return 0;
    s->v = v;
    s->cap = cap;
  }
  s->v[s->n].e = e;
  s->v[s->n].exiting = exiting;
  s->n++;
  return 1;
}

/* The upstream inputs of one document node in trace_root_dependencies. */
static int rgs_trace_inputs(rgs_ctx *c, size_t consumer, const lu_mtlx_elem *current,
                            pend_stack *pending) {
  for (size_t k = 0; k < current->nchildren; k++) {
    const lu_mtlx_elem *up = &current->children[k];
    if (!streq(up->tag, "input")) continue;
    const char *name;
    if ((name = lu_mtlx_get_attr(up, "nodegraph"))) {
      const lu_mtlx_elem *sv[3] = {up, current, c->document};
      const lu_mtlx_scope scope = {sv, 3, 3};
      if (!rgs_add_dependency(c, consumer, name, up, &scope)) return 0;
    } else if ((name = lu_mtlx_get_attr(up, "nodename"))) {
      const lu_mtlx_elem *next = find_node(c->document, name);
      if (next && !pend_push(pending, next, 0)) goto oom;
    } else if ((name = lu_mtlx_get_attr(up, "output"))) {
      const lu_mtlx_elem *o = find_child(c->document, "output", name);
      const lu_mtlx_elem *next = o ? find_node(c->document, lu_mtlx_attr_or(o, "nodename", "")) : NULL;
      if (next && !pend_push(pending, next, 0)) goto oom;
    }
  }
  return 1;
oom:
  lu_mtlx_err(c->err, "out of memory");
  return 0;
}

/* trace_root_dependencies lambda of ResolveMtlxGraphs (iterative DFS). */
static int rgs_trace(rgs_ctx *c, size_t consumer, const lu_mtlx_elem *source) {
  pend_stack pending = {NULL, 0, 0};
  ptr_set active = {NULL, 0, 0}, visited = {NULL, 0, 0};
  int ok = pend_push(&pending, source, 0);
  if (!ok) lu_mtlx_err(c->err, "out of memory");
  while (ok && pending.n > 0) {
    const pend top = pending.v[--pending.n];
    if (top.exiting) {
      ptr_del(&active, top.e);
      if (!ptr_has(&visited, top.e) && !ptr_add(&visited, top.e)) {
        lu_mtlx_err(c->err, "out of memory");
        ok = 0;
      }
      continue;
    }
    if (ptr_has(&visited, top.e)) continue;
    if (ptr_has(&active, top.e)) {
      lu_mtlx_err(c->err, "document nodes contain a dependency cycle");
      ok = 0;
      break;
    }
    if (!ptr_add(&active, top.e) || !pend_push(&pending, top.e, 1)) {
      lu_mtlx_err(c->err, "out of memory");
      ok = 0;
      break;
    }
    ok = rgs_trace_inputs(c, consumer, top.e, &pending);
  }
  free(pending.v);
  free((void *)active.v);
  free((void *)visited.v);
  return ok;
}

/* Document source of an `output` binding: output -> its nodename node. */
static const lu_mtlx_elem *doc_output_source(const lu_mtlx_elem *document, const char *name) {
  const lu_mtlx_elem *o = find_child(document, "output", name);
  return o ? find_node(document, lu_mtlx_attr_or(o, "nodename", "")) : NULL;
}

/* ResolveMtlxGraphs: dependencies of one consumer graph. */
static int rgs_consumer(rgs_ctx *c, size_t consumer) {
  const lu_mtlx_elem *g = c->graphs[consumer];
  for (size_t i = 0; i < g->nchildren; i++) {
    const lu_mtlx_elem *input = &g->children[i];
    if (!streq(input->tag, "input")) continue;
    const char *name;
    if ((name = lu_mtlx_get_attr(input, "nodegraph"))) {
      const lu_mtlx_elem *sv[3] = {input, g, c->document};
      const lu_mtlx_scope scope = {sv, 3, 3};
      if (!rgs_add_dependency(c, consumer, name, input, &scope)) return 0;
    } else if ((name = lu_mtlx_get_attr(input, "nodename"))) {
      const lu_mtlx_elem *source = find_node(c->document, name);
      if (source && !rgs_trace(c, consumer, source)) return 0;
    } else if ((name = lu_mtlx_get_attr(input, "output"))) {
      const lu_mtlx_elem *source = doc_output_source(c->document, name);
      if (source && !rgs_trace(c, consumer, source)) return 0;
    }
  }
  for (size_t n = 0; n < g->nchildren; n++) {
    const lu_mtlx_elem *node = &g->children[n];
    if (!lu_mtlx_is_node_elem(node)) continue;
    for (size_t k = 0; k < node->nchildren; k++) {
      const lu_mtlx_elem *input = &node->children[k];
      if (!streq(input->tag, "input")) continue;
      const char *name = lu_mtlx_get_attr(input, "nodegraph");
      if (name) {
        const lu_mtlx_elem *sv[4] = {input, node, g, c->document};
        const lu_mtlx_scope scope = {sv, 4, 4};
        if (!rgs_add_dependency(c, consumer, name, input, &scope)) return 0;
        continue;
      }
      const char *output_name = lu_mtlx_get_attr(input, "output");
      if (!output_name || lu_mtlx_get_attr(input, "nodename")) continue;
      const lu_mtlx_elem *source = doc_output_source(c->document, output_name);
      if (!source) continue;
      if (!rgs_trace(c, consumer, source)) return 0;
    }
  }
  return 1;
}

static int resolve_graphs_impl(rgs_ctx *c, const lu_mtlx_library *library,
                               lu_mtlx_strv *graph_order) {
  const lu_mtlx_elem *document = c->document;
  for (size_t i = 0; i < document->nchildren; i++) {
    const lu_mtlx_elem *child = &document->children[i];
    if (!streq(child->tag, "nodegraph")) continue;
    const char *name = lu_mtlx_name(child);
    if (!*name) {
      lu_mtlx_err(c->err, "document contains an unnamed nodegraph");
      return 0;
    }
    for (size_t p = 0; p < c->ngraphs; p++)
      if (streq(lu_mtlx_name(c->graphs[p]), name)) {
        errf(c->err, "duplicate nodegraph name '", name, "'", NULL);
        return 0;
      }
    c->graphs[c->ngraphs++] = child;
  }
  const lu_mtlx_elem *dv[1] = {document};
  const lu_mtlx_scope outer = {dv, 1, 1};
  for (size_t i = 0; i < c->ngraphs; i++) {
    lu_mtlx_strv resolved;
    lu_mtlx_strv_init(&resolved);
    const int ok = lu_mtlx_resolve_graph(document, c->graphs[i], library, &outer, &resolved, c->err);
    lu_mtlx_strv_free(&resolved);
    if (!ok) return 0;
  }
  for (size_t consumer = 0; consumer < c->ngraphs; consumer++)
    if (!rgs_consumer(c, consumer)) return 0;
  size_t *order = (size_t *)malloc((c->ngraphs + 1) * sizeof(size_t));
  const size_t done = order ? topo_order(c->ngraphs, &c->es, order) : (size_t)-1;
  int ok = done != (size_t)-1;
  for (size_t k = 0; ok && k < done; k++)
    ok = lu_mtlx_strv_push(graph_order, lu_mtlx_name(c->graphs[order[k]]));
  free(order);
  if (!ok) {
    lu_mtlx_strv_free(graph_order);
    lu_mtlx_err(c->err, "out of memory");
    return 0;
  }
  if (graph_order->n != c->ngraphs) {
    lu_mtlx_strv_free(graph_order);
    lu_mtlx_err(c->err, "document nodegraphs contain a dependency cycle");
    return 0;
  }
  return 1;
}

/* ResolveMtlxGraphs */
int lu_mtlx_resolve_graphs(const lu_mtlx_elem *document, const lu_mtlx_library *library,
                           lu_mtlx_strv *graph_order, lu_mtlx_sb *err) {
  lu_mtlx_sb ignored;
  lu_mtlx_sb_init(&ignored);
  if (!err) err = &ignored;
  lu_mtlx_err(err, "");
  if (!graph_order) {
    lu_mtlx_err(err, "missing nodegraph-order output");
    lu_mtlx_sb_free(&ignored);
    return 0;
  }
  lu_mtlx_strv_free(graph_order);
  rgs_ctx c;
  memset(&c, 0, sizeof(c));
  c.document = document;
  c.err = err;
  c.graphs = (const lu_mtlx_elem **)malloc((document->nchildren + 1) * sizeof(*c.graphs));
  int ok;
  if (!c.graphs) {
    lu_mtlx_err(err, "out of memory");
    ok = 0;
  } else {
    ok = resolve_graphs_impl(&c, library, graph_order);
  }
  free((void *)c.graphs);
  free(c.es.v);
  lu_mtlx_sb_free(&ignored);
  return ok;
}
