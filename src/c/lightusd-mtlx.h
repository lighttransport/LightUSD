/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * LightUSD MaterialX documents (C11).
 *
 * A document is parsed into an element tree (the attribute and child layout of
 * the MaterialX XML, after XInclude expansion, version upgrade, scoped
 * attribute inheritance and nodedef default application). Elements are viewed
 * through the accessors below; they stay valid until the document is changed
 * (flatten, unit conversion, re-parse) or destroyed.
 *
 * Element classes (as the former C++ MtlxDocument API):
 *   nodes        top-level children other than nodegraph / look / collection /
 *                surfacematerial / volumematerial / typedef / geominfo
 *   nodegraphs   <nodegraph>; their inputs (<input>), outputs (<output>) and
 *                nodes (every other child)
 *   materials    <surfacematerial>, <volumematerial>
 *   looks        <look>; assignments are its <materialassign> children
 *   collections  <collection>
 * Attribute getters return "" for an absent attribute.
 */
#ifndef LIGHTUSD_C_MTLX_H_
#define LIGHTUSD_C_MTLX_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lu_mtlx_document lu_mtlx_document;
typedef struct lu_mtlx_elem lu_mtlx_elem;
typedef struct lu_mtlx_library lu_mtlx_library; /* src/c/mtlx/mtlx-internal.h */

/* ---- documents ----------------------------------------------------------- */

lu_mtlx_document *lu_mtlx_document_create(void);
void lu_mtlx_document_destroy(lu_mtlx_document *doc);

/* In-memory document without XInclude resolution (warnings such as unknown
 * versions are reported through lu_mtlx_document_warning). */
int lu_mtlx_document_parse(lu_mtlx_document *doc, const char *xml, size_t len);

/* XIncludes resolved through `loader` (see lu_mtlx_include_loader in
 * mtlx-internal.h: href, including document href or "", malloc'd text). */
typedef int (*lu_mtlx_document_loader)(void *user, const char *href,
                                       const char *including_href, char **xml,
                                       size_t *len);
int lu_mtlx_document_parse_with_loader(lu_mtlx_document *doc, const char *xml, size_t len,
                                       lu_mtlx_document_loader loader, void *user);

/* File-system front ends (src/core/mtlx-file.cc): XIncludes resolved relative
 * to `anchor_path` / the file through LightUSD's asset resolver and security
 * policy. */
int lu_mtlx_document_parse_anchored(lu_mtlx_document *doc, const char *xml, size_t len,
                                    const char *anchor_path);
int lu_mtlx_document_parse_file(lu_mtlx_document *doc, const char *path);

const char *lu_mtlx_document_error(const lu_mtlx_document *doc);
const char *lu_mtlx_document_warning(const lu_mtlx_document *doc);
const char *lu_mtlx_document_version(const lu_mtlx_document *doc);
const char *lu_mtlx_document_colorspace(const lu_mtlx_document *doc);
const char *lu_mtlx_document_namespace(const lu_mtlx_document *doc);

/* Resolved XML (XML prolog + document); malloc'd, "" before a successful
 * parse; NULL on allocation failure. Free with free(). */
char *lu_mtlx_document_resolved_xml(const lu_mtlx_document *doc);

typedef enum lu_mtlx_class {
  LU_MTLX_NODES,
  LU_MTLX_NODEGRAPHS,
  LU_MTLX_MATERIALS,
  LU_MTLX_LOOKS,
  LU_MTLX_COLLECTIONS
} lu_mtlx_class;

size_t lu_mtlx_document_count(const lu_mtlx_document *doc, lu_mtlx_class cls);
const lu_mtlx_elem *lu_mtlx_document_at(const lu_mtlx_document *doc, lu_mtlx_class cls,
                                        size_t i);
/* By name; LU_MTLX_NODES also searches the nodes of every nodegraph. */
const lu_mtlx_elem *lu_mtlx_document_find(const lu_mtlx_document *doc, lu_mtlx_class cls,
                                          const char *name);

/* Validate every nodegraph against the document's, `library`'s (may be NULL)
 * and the standard nodedefs. On success *order (malloc'd array of `*count`
 * malloc'd names, dependency first) is set; free with lu_mtlx_free_strings.
 * The error string is malloc'd (free with free()) or NULL. */
int lu_mtlx_document_resolve_graphs(const lu_mtlx_document *doc,
                                    const lu_mtlx_library *library, char ***order,
                                    size_t *count, char **error);
/* Inline graph implementations in the named nodegraph (document unchanged on
 * failure). */
int lu_mtlx_document_flatten_graph(lu_mtlx_document *doc, const char *graph,
                                   const lu_mtlx_library *library, size_t max_expansions,
                                   char **error);
/* Convert inputs of `unit_type` to `target_unit` (see MaterialX
 * UnitConverterRegistry::convertToUnit). 0 when nothing converted or on error
 * (then lu_mtlx_document_error is set and the document is unchanged). */
int lu_mtlx_document_convert_to_unit(lu_mtlx_document *doc, const char *unit_type,
                                     const char *target_unit);

typedef struct lu_mtlx_assignment {
  const lu_mtlx_elem *assignment; /* <materialassign> */
  const lu_mtlx_elem *material;
} lu_mtlx_assignment;
/* Assignments of `look` (and inherited looks) matching `geometry`, derived
 * first. *out is malloc'd (free()). */
int lu_mtlx_document_assignments(const lu_mtlx_document *doc, const char *look,
                                 const char *geometry, lu_mtlx_assignment **out,
                                 size_t *count, char **error);

void lu_mtlx_free_strings(char **v, size_t n);

/* ---- elements ------------------------------------------------------------ */

const char *lu_mtlx_elem_tag(const lu_mtlx_elem *e);
const char *lu_mtlx_elem_get(const lu_mtlx_elem *e, const char *key); /* "" if absent */
int lu_mtlx_elem_has(const lu_mtlx_elem *e, const char *key);
/* Node category: the "category" attribute, else the element tag. */
const char *lu_mtlx_elem_category(const lu_mtlx_elem *e);

typedef enum lu_mtlx_children {
  LU_MTLX_CHILD_ALL,
  LU_MTLX_CHILD_INPUTS,      /* <input> */
  LU_MTLX_CHILD_OUTPUTS,     /* <output> */
  LU_MTLX_CHILD_GRAPH_NODES, /* nodegraph children other than input/output */
  LU_MTLX_CHILD_ASSIGNMENTS  /* <materialassign> */
} lu_mtlx_children;
size_t lu_mtlx_elem_count(const lu_mtlx_elem *e, lu_mtlx_children which);
const lu_mtlx_elem *lu_mtlx_elem_at(const lu_mtlx_elem *e, lu_mtlx_children which, size_t i);
const lu_mtlx_elem *lu_mtlx_elem_find(const lu_mtlx_elem *e, lu_mtlx_children which,
                                      const char *name);

/* Typed value of an <input> (the former MtlxInput value parse: float,
 * integer, boolean, string/filename, vector2-4/color3-4/floatarray,
 * integerarray). Other elements and unparsable texts give LU_MTLX_VALUE_NONE. */
typedef enum lu_mtlx_value_type {
  LU_MTLX_VALUE_NONE,
  LU_MTLX_VALUE_BOOL,
  LU_MTLX_VALUE_INT,
  LU_MTLX_VALUE_FLOAT,
  LU_MTLX_VALUE_STRING,
  LU_MTLX_VALUE_FLOATS,
  LU_MTLX_VALUE_INTS
} lu_mtlx_value_type;
typedef struct lu_mtlx_value {
  lu_mtlx_value_type type;
  int b;
  int i;
  float f;
  const char *s; /* borrowed from the element */
  float *floats; size_t nfloats;
  int *ints; size_t nints;
} lu_mtlx_value;
int lu_mtlx_input_value(const lu_mtlx_elem *input, lu_mtlx_value *out); /* 0 on oom */
void lu_mtlx_value_free(lu_mtlx_value *v);

/* Shader connections of a material (shaderref / input children; later
 * children override earlier ones). Strings are borrowed; outputs default to
 * "out" when the connection names none. */
typedef struct lu_mtlx_material_refs {
  const char *surface, *surface_graph, *surface_output;
  const char *displacement;
  const char *volume, *volume_graph, *volume_output;
} lu_mtlx_material_refs;
void lu_mtlx_material_refs_get(const lu_mtlx_elem *material, lu_mtlx_material_refs *out);

#ifdef __cplusplus
}
#endif
#endif /* LIGHTUSD_C_MTLX_H_ */
