/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * LightUSD MaterialX core (C11), internal contract between its translation
 * units. Every function declared here is implemented in exactly one TU of
 * src/c/mtlx/ (named in the section header). This is a port of the former
 * C++ src/core/mtlx-{dom,resolve}.cc; behaviour (and every produced string)
 * must stay byte-identical to it -- tests/core/run_mtlx_diff.py checks that.
 *
 * Conventions
 *  - Strings are NUL-terminated, heap-owned by the structure that holds them
 *    (element tags/attributes, library entries, string vectors) unless a
 *    comment says "borrowed". Input text containing NUL bytes is rejected.
 *  - Functions returning int return 1 on success and 0 on failure. Allocation
 *    failure is a failure; the error builder then reads "out of memory".
 *  - `lu_mtlx_sb *err` may be NULL. On failure the message replaces err's
 *    contents (as the C++ code assigned *err).
 *  - Tree walks use explicit stacks (no recursion); no assert().
 */
#ifndef LIGHTUSD_C_MTLX_INTERNAL_H_
#define LIGHTUSD_C_MTLX_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- strings & builder (mtlx-core.c) ---------------------------------- */

typedef struct lu_mtlx_sb {
  char *p;   /* NUL-terminated when non-NULL */
  size_t n, cap;
  int oom;
} lu_mtlx_sb;

void lu_mtlx_sb_init(lu_mtlx_sb *b);
void lu_mtlx_sb_free(lu_mtlx_sb *b);
void lu_mtlx_sb_clear(lu_mtlx_sb *b);
void lu_mtlx_sb_put(lu_mtlx_sb *b, const char *s, size_t n);
void lu_mtlx_sb_puts(lu_mtlx_sb *b, const char *s);
void lu_mtlx_sb_putc(lu_mtlx_sb *b, char c);
void lu_mtlx_sb_putu(lu_mtlx_sb *b, uint64_t v);   /* std::to_string(unsigned) */
void lu_mtlx_sb_puti(lu_mtlx_sb *b, int64_t v);    /* std::to_string(signed) */
void lu_mtlx_sb_set(lu_mtlx_sb *b, const char *s); /* clear + puts */
const char *lu_mtlx_sb_str(const lu_mtlx_sb *b);  /* "" when empty */
char *lu_mtlx_sb_take(lu_mtlx_sb *b);             /* caller frees; NULL on oom */
/* Assign an error message (no-op when err is NULL). */
void lu_mtlx_err(lu_mtlx_sb *err, const char *msg);

char *lu_mtlx_strdup(const char *s);
char *lu_mtlx_strndup(const char *s, size_t n);
int lu_mtlx_streq(const char *a, const char *b); /* NULL-safe */
int lu_mtlx_starts_with(const char *s, const char *prefix);
int lu_mtlx_ends_with(const char *s, const char *suffix);

/* Growable vector of owned strings. */
typedef struct lu_mtlx_strv {
  char **v;
  size_t n, cap;
} lu_mtlx_strv;
void lu_mtlx_strv_init(lu_mtlx_strv *s);
void lu_mtlx_strv_free(lu_mtlx_strv *s);
int lu_mtlx_strv_push(lu_mtlx_strv *s, const char *str);
int lu_mtlx_strv_push_n(lu_mtlx_strv *s, const char *str, size_t n);
int lu_mtlx_strv_contains(const lu_mtlx_strv *s, const char *str);

/* ---- element tree (mtlx-core.c) --------------------------------------- */

typedef struct lu_mtlx_attr {
  char *key;
  char *value;
} lu_mtlx_attr;

/* Port of MtlxElem: tag, attributes in document order, child elements held
 * by value (a child array may move on insertion, like std::vector). */
typedef struct lu_mtlx_elem {
  char *tag; /* never NULL after init ("" ) */
  lu_mtlx_attr *attrs;
  size_t nattrs, capattrs;
  struct lu_mtlx_elem *children;
  size_t nchildren, capchildren;
} lu_mtlx_elem;

void lu_mtlx_elem_init(lu_mtlx_elem *e);
void lu_mtlx_elem_free(lu_mtlx_elem *e);                      /* iterative */
int lu_mtlx_elem_copy(lu_mtlx_elem *dst, const lu_mtlx_elem *src); /* dst uninit */
void lu_mtlx_elem_move(lu_mtlx_elem *dst, lu_mtlx_elem *src); /* src re-inited */
int lu_mtlx_elem_set_tag(lu_mtlx_elem *e, const char *tag);

const char *lu_mtlx_get_attr(const lu_mtlx_elem *e, const char *key); /* first; NULL */
const char *lu_mtlx_attr_or(const lu_mtlx_elem *e, const char *key, const char *fb);
const char *lu_mtlx_name(const lu_mtlx_elem *e); /* attr "name" or "" */
/* MtlxElem::SetAttr: removes every `key` attribute, appends (key, value). */
int lu_mtlx_attr_set(lu_mtlx_elem *e, const char *key, const char *value);
/* Appends without removing (parser use). */
int lu_mtlx_attr_append(lu_mtlx_elem *e, const char *key, const char *value);
void lu_mtlx_attr_erase(lu_mtlx_elem *e, const char *key); /* every match */

/* Child storage. `moved` is moved in and re-initialised. Returns the stored
 * child, or NULL on allocation failure (then `moved` is freed). */
lu_mtlx_elem *lu_mtlx_child_push(lu_mtlx_elem *e, lu_mtlx_elem *moved);
lu_mtlx_elem *lu_mtlx_child_insert(lu_mtlx_elem *e, size_t at, lu_mtlx_elem *moved);
void lu_mtlx_child_erase(lu_mtlx_elem *e, size_t at); /* frees the child */
const lu_mtlx_elem *lu_mtlx_child_named(const lu_mtlx_elem *e, const char *name);
lu_mtlx_elem *lu_mtlx_child_named_mut(lu_mtlx_elem *e, const char *name);

/* Scope: the enclosing chain of an element, innermost first (MtlxScope). The
 * pointer array is owned; the elements are borrowed. */
typedef struct lu_mtlx_scope {
  const lu_mtlx_elem **v;
  size_t n, cap;
} lu_mtlx_scope;
void lu_mtlx_scope_init(lu_mtlx_scope *s);
void lu_mtlx_scope_free(lu_mtlx_scope *s);
int lu_mtlx_scope_push(lu_mtlx_scope *s, const lu_mtlx_elem *e);
/* *out = {e} followed by `outer` (the scope of a child of the outer chain). */
int lu_mtlx_scope_with(lu_mtlx_scope *out, const lu_mtlx_elem *e, const lu_mtlx_scope *outer);

/* ---- XML (mtlx-xml.c) ------------------------------------------------- */

/* ParseMtlxXml. `root` must be initialised; replaced on success. */
int lu_mtlx_parse_xml(const char *xml, size_t len, lu_mtlx_elem *root, lu_mtlx_sb *err);
/* The resolved-XML writer of MtlxDocument::GetResolvedXML (the element tree
 * re-serialised through the XMLNode conversion of mtlx-dom.cc). */
int lu_mtlx_write_xml(const lu_mtlx_elem *root, lu_mtlx_sb *out);

/* The class parser (src/mtlx-xml-parser.cc MaterialXParser) front end used by
 * the in-memory MtlxDocument::ParseFromXML(string): same element tree, plus
 * its validation errors and warnings ("Unknown MaterialX version: ..."). */
int lu_mtlx_parse_xml_checked(const char *xml, size_t len, lu_mtlx_elem *root,
                              lu_mtlx_sb *err, lu_mtlx_sb *warning);

/* ---- names & values (mtlx-value.c) ------------------------------------ */

typedef enum lu_mtlx_value_kind {
  LU_MTLX_V_NONE, LU_MTLX_V_INT, LU_MTLX_V_BOOL, LU_MTLX_V_NUM, LU_MTLX_V_AGG,
  LU_MTLX_V_STR, LU_MTLX_V_INTS, LU_MTLX_V_NUMS, LU_MTLX_V_STRS
} lu_mtlx_value_kind;

/* MtlxTypedValue. */
typedef struct lu_mtlx_typed_value {
  lu_mtlx_value_kind kind;
  int64_t i;
  int b;
  double num;
  double *nums; size_t nnums;    /* Agg, Nums */
  int64_t *ints; size_t nints;   /* Ints */
  char *str;                     /* Str */
  lu_mtlx_strv strs;             /* Strs */
} lu_mtlx_typed_value;
void lu_mtlx_typed_value_init(lu_mtlx_typed_value *v);
void lu_mtlx_typed_value_free(lu_mtlx_typed_value *v);

int lu_mtlx_is_valid_name(const char *name);
int lu_mtlx_is_reference_valid_name(const char *name);
size_t lu_mtlx_type_arity(const char *type);
size_t lu_mtlx_array_stride(const char *type);
int lu_mtlx_is_builtin_type(const char *type);
int lu_mtlx_parse_value(const char *type, const char *text, lu_mtlx_typed_value *out);
int lu_mtlx_default_value(const char *type, lu_mtlx_typed_value *out);
int lu_mtlx_lex_value(const char *type, const char *text, lu_mtlx_sb *out);
/* Shared token helpers of mtlx-resolve.cc (Trim, Fields, ...); trim and
 * split_on live in mtlx-core.c. split_on keeps empty fields ("" -> [""]) and
 * initialises *out. */
int lu_mtlx_trim(const char *s, lu_mtlx_sb *out);
int lu_mtlx_split_on(char sep, const char *s, lu_mtlx_strv *out);
int lu_mtlx_parse_int_token(const char *s, int64_t *out);
int lu_mtlx_parse_float_token(const char *s, double *out);
/* "%.9g"-equivalent of std::ostream << float with max_digits10, classic locale. */
int lu_mtlx_format_float(float v, lu_mtlx_sb *out);

/* ---- scoped attributes, version, includes (mtlx-scope.c, mtlx-include.c) */

int lu_mtlx_scoped_attr(const char *key, const lu_mtlx_scope *chain, const char **out);
int lu_mtlx_active_colorspace(const lu_mtlx_scope *chain, lu_mtlx_sb *out);
int lu_mtlx_resolved_filename(const lu_mtlx_scope *chain, lu_mtlx_sb *out);
int lu_mtlx_parse_version(const char *text, uint64_t *major, uint64_t *minor);  /* mtlx-core.c */

/* MtlxAnchoredIncludeLoader: `including_href` is "" for the root document.
 * On success *xml/ *len is a malloc'd buffer the core frees. */
typedef int (*lu_mtlx_include_loader)(void *user, const char *href,
                                      const char *including_href, char **xml,
                                      size_t *len);
int lu_mtlx_expand_includes(lu_mtlx_elem *root, lu_mtlx_include_loader loader,
                            void *user, lu_mtlx_sb *err);

/* ---- version upgrade & document preparation (mtlx-upgrade-*.c) -------- */

void lu_mtlx_upgrade_version(lu_mtlx_elem *root);      /* UpgradeMtlxVersion */
void lu_mtlx_inherit_attributes(lu_mtlx_elem *root);   /* InheritMtlxAttributes */

/* ---- node definitions (mtlx-library.c) -------------------------------- */

typedef struct lu_mtlx_port {
  char *name, *type;
  int has_value;
  char *value, *default_geomprop;
} lu_mtlx_port;

typedef struct lu_mtlx_nodedef {
  char *name, *node, *key;
  int has_version; char *version;
  int is_default;
  char *target;
  int has_inherit; char *inherit;
  lu_mtlx_port *inputs; size_t ninputs;
  lu_mtlx_port *outputs; size_t noutputs;
} lu_mtlx_nodedef;

typedef struct lu_mtlx_library {
  lu_mtlx_nodedef *defs; size_t ndefs, capdefs;
  lu_mtlx_elem *geomprop_defs; size_t ngeomprop, capgeomprop;
  int oom;
} lu_mtlx_library;

void lu_mtlx_library_init(lu_mtlx_library *l);
void lu_mtlx_library_free(lu_mtlx_library *l);
int lu_mtlx_library_add_from(lu_mtlx_library *l, const lu_mtlx_elem *root);
int lu_mtlx_library_add_library(lu_mtlx_library *l, const lu_mtlx_library *other);
int lu_mtlx_library_add_nodedef(lu_mtlx_library *l, const lu_mtlx_elem *e,
                                const lu_mtlx_scope *outer);
/* The standard-library nodedefs / geompropdefs (static tables in
 * mtlx-stdlib.c), appended exactly as AddNodeDef(def, {}) of the former
 * StandardMtlxNodeDefs() elements would. */
int lu_mtlx_library_add_stdlib(lu_mtlx_library *l);
size_t lu_mtlx_stdlib_geomprop_count(void);
int lu_mtlx_stdlib_geomprop_elem(size_t i, lu_mtlx_elem *out); /* out uninit */

const lu_mtlx_nodedef *lu_mtlx_library_find(const lu_mtlx_library *l, const char *name);
typedef struct lu_mtlx_ports { lu_mtlx_port *v; size_t n; } lu_mtlx_ports; /* borrowed entries */
int lu_mtlx_library_active_inputs(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                                  lu_mtlx_ports *out); /* free(out->v) */
int lu_mtlx_library_active_outputs(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                                   lu_mtlx_ports *out);
int lu_mtlx_library_out_type(const lu_mtlx_library *l, const lu_mtlx_nodedef *d,
                             lu_mtlx_sb *out);
const lu_mtlx_nodedef *lu_mtlx_library_resolve_strict(const lu_mtlx_library *l,
                                                      const lu_mtlx_elem *node,
                                                      const lu_mtlx_scope *outer);
const lu_mtlx_nodedef *lu_mtlx_library_resolve_reference(const lu_mtlx_library *l,
                                                         const lu_mtlx_elem *node,
                                                         const lu_mtlx_scope *outer);
const lu_mtlx_nodedef *lu_mtlx_library_resolve_rough(const lu_mtlx_library *l,
                                                     const lu_mtlx_elem *node,
                                                     const lu_mtlx_scope *outer);
int lu_mtlx_is_node_elem(const lu_mtlx_elem *e); /* IsMtlxNodeElem */

/* ApplyMtlxNodeDefDefaults(root) of mtlx-dom.cc (document + standard library). */
int lu_mtlx_apply_nodedef_defaults(lu_mtlx_elem *root);

/* ---- graphs (mtlx-resolve.c, mtlx-flatten.c) -------------------------- */

int lu_mtlx_resolve_graph(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                          const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                          lu_mtlx_strv *node_order, lu_mtlx_sb *err);
int lu_mtlx_resolve_graphs(const lu_mtlx_elem *document, const lu_mtlx_library *library,
                           lu_mtlx_strv *graph_order, lu_mtlx_sb *err);
/* `flattened` is initialised by the caller and only replaced on success. */
int lu_mtlx_flatten_graph_call(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                               const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                               const char *call_name, lu_mtlx_elem *flattened,
                               lu_mtlx_sb *err);
int lu_mtlx_flatten_graph(const lu_mtlx_elem *document, const lu_mtlx_elem *graph,
                          const lu_mtlx_library *library, const lu_mtlx_scope *outer,
                          size_t max_expansions, lu_mtlx_elem *flattened, lu_mtlx_sb *err);

/* ---- canonical dump (mtlx-dump.c) ------------------------------------- */

int lu_mtlx_dump_document(const lu_mtlx_library *lib, const lu_mtlx_elem *root,
                          lu_mtlx_sb *out);

#ifdef __cplusplus
}
#endif
#endif /* LIGHTUSD_C_MTLX_INTERNAL_H_ */
