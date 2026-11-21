/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * lightusd-core-c.h -- C11 core for the `next` product.
 *
 * This is the C11 boundary for algorithmic core modules that the `next` build
 * shares with C++ consumers. The first module ported here is the SdfPathExpression
 * parser / serializer / matcher; further leaf modules follow the same shape.
 *
 * Design (mirrors src/c-api/lightusd-c.h, but internal to the next build):
 * - C11 only: no C++ types, no exceptions, no RTTI cross this boundary.
 * - Counted views (lu_view) instead of NUL-terminated strings; every buffer is
 *   bounded by an explicit capacity and every length is size_t.
 * - Owning opaque handles are released with their *_destroy function; borrowed
 *   views point into owned storage and stay valid while the owner lives.
 * - Fallible functions return 0 / negative lu_status. Match results are
 *   reported as 1 (match) / 0 (no match); negative is an error, never a match.
 * - There are no global side effects and no static state: every handle is
 *   independent, so this is safe to call from any number of threads.
 */

#ifndef LIGHTUSD_CORE_C_H_
#define LIGHTUSD_CORE_C_H_

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <cstddef>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Maximally aligned scalar. MSVC declares max_align_t only as std::max_align_t
 * in C++ and not at all in C (even in C11 mode). Its C++ max_align_t is double,
 * which the C union matches in size and alignment, so C and C++ translation
 * units agree on lu_value's layout. */
#if defined(__cplusplus)
typedef std::max_align_t lu_max_align_t;
#elif defined(_MSC_VER)
typedef union lu_max_align_t {
    long long ll;
    long double ld;
    double d;
    void* p;
} lu_max_align_t;
#else
typedef max_align_t lu_max_align_t;
#endif

#ifndef LU_CORE_API
#if defined(_WIN32) && defined(LU_CORE_BUILD_SHARED)
#define LU_CORE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(LU_CORE_USE_SHARED)
#define LU_CORE_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define LU_CORE_API __attribute__((visibility("default")))
#else
#define LU_CORE_API
#endif
#endif

#define LU_CORE_C_VERSION_MAJOR 2
#define LU_CORE_C_VERSION_MINOR 0
#define LU_CORE_C_VERSION_PATCH 0

/* ============================================================
 * Status codes
 * ============================================================ */

typedef enum lu_status {
  LU_OK = 0,
  LU_ERR_INVALID_ARG = -1,    /* NULL / out-of-range argument */
  LU_ERR_OUT_OF_MEMORY = -2,  /* allocation failure */
  LU_ERR_LIMIT = -3,          /* structural limit exceeded (see below) */
  LU_ERR_BUFFER = -4,        /* out buffer too small; *out_len = needed size */
  LU_ERR_NOT_FOUND = -6,     /* no such schema/property/shader/rule */
  LU_ERR_INTERNAL = -99
} lu_status;

/* ============================================================
 * Counted byte view
 * ============================================================ */

typedef struct lu_view {
  const char *data;
  size_t len;
} lu_view;

/* View over a NUL-terminated string (uses strlen; returns an empty view for
 * NULL). Not part of the hot path. */
LU_CORE_API lu_view lu_view_from_cstr(const char *s);

/* ============================================================
 * Structural limits
 *
 * These mirror the historical next-side limits. They are unconditional: an
 * expression that exceeds them is rejected rather than parsed further, so a
 * hostile input cannot drive unbounded work or memory use.
 * ============================================================ */

#define LU_PATH_EXPR_MAX_BYTES ((size_t)1 << 20) /* 1 MiB of expression text */
#define LU_PATH_EXPR_MAX_DEPTH ((size_t)256)     /* nesting `((( )))` */
#define LU_PATH_EXPR_MAX_NODES ((size_t)4096)    /* parse-tree nodes */

#define LU_PATH_EXPR_MAX_MATCH_ELEMS ((size_t)4096)
#define LU_PATH_EXPR_MAX_MATCH_SEGMENTS ((size_t)4096)
#define LU_PATH_EXPR_MAX_EVAL_OPS ((size_t)4096)
/* Defence in depth: the matcher memo table is (elems+1) * (segments+1) entries.
 * Bounding the product keeps a pathological (but individually legal) pair from
 * allocating an unbounded memo. */
#define LU_PATH_EXPR_MAX_MEMO_CELLS ((size_t)1 << 22)

/* Operator of a prefix-op stream (values are part of the ABI). */
typedef enum lu_path_expr_op {
  LU_PATH_EXPR_OP_COMPLEMENT = 0,    /* ~   (unary) */
  LU_PATH_EXPR_OP_IMPLIED_UNION = 1, /* whitespace */
  LU_PATH_EXPR_OP_UNION = 2,         /* + */
  LU_PATH_EXPR_OP_INTERSECTION = 3,  /* & */
  LU_PATH_EXPR_OP_DIFFERENCE = 4,    /* - */
  LU_PATH_EXPR_OP_EXPRESSION_REF = 5,/* operand: consumes next reference */
  LU_PATH_EXPR_OP_PATTERN = 6        /* operand: consumes next pattern */
} lu_path_expr_op;

/* ============================================================
 * Parsed SdfPathExpression
 * ============================================================ */

/* Opaque parsed expression. Owns all strings it exposes. */
typedef struct lu_path_expr lu_path_expr;

/* One component of a path pattern. `text` may contain '*'/'?' wildcards; a
 * "stretch" component (`//`) has both views empty and is_stretch set. */
typedef struct lu_component_view {
  lu_view text;       /* literal/glob text; empty for a stretch */
  lu_view predicate;  /* raw text inside `{...}`; empty when absent */
  int is_stretch;     /* 1 for `//`, 0 otherwise */
} lu_component_view;

/* SdfPathPattern: a prefix path followed by components. */
typedef struct lu_pattern_view {
  lu_view prefix;         /* canonical literal prefix, e.g. "/World/Geom" */
  size_t component_count;
  int match_property;     /* 1 when the pattern targets `.property` */
  lu_view property_name;  /* property glob when match_property */
} lu_pattern_view;

/* Expression reference: `%/Owner:name`, `%:name` (same prim) or `%_`. */
typedef struct lu_ref_view {
  lu_view path;        /* owning-prim path; empty for same-prim / weaker */
  lu_view name;        /* collection name; "_" denotes the weaker reference */
  int is_weaker;
} lu_ref_view;

/* Caller-provided hooks. Both are optional: when unset the matcher evaluates
 * structurally (predicates pass, references match nothing). The `user` pointer
 * is passed through unchanged and is never dereferenced here. */
typedef struct lu_path_expr_callbacks {
  /* Evaluate a predicate (raw text inside `{...}`, e.g. "model") against the
   * prim at `prim_path`. Return non-zero when satisfied, 0 when not. */
  int (*eval_predicate)(void *user, lu_view predicate, lu_view prim_path);

  /* Resolve a reference to a parsed expression. Return NULL when unresolved;
   * unresolved references match nothing. The returned handle is only read for
   * the duration of the calling lu_path_expr_match_path() invocation. */
  const lu_path_expr *(*resolve_ref)(void *user, lu_view ref_path,
                                      lu_view ref_name, int is_weaker);

  /* Guard against reference cycles. */
  int max_ref_depth;
} lu_path_expr_callbacks;

/* Parse `text` into `*out`.
 *
 * Always returns LU_OK and creates an object on success (including a valid
 * empty expression, and an *invalid* expression carrying a diagnostic -- use
 * lu_path_expr_valid() to tell). `err_buf` (optional) receives the diagnostic
 * when the expression is invalid; nothing is written for a valid parse.
 *
 * Negative status means no object was created (`*out` is set to NULL):
 * LU_ERR_OUT_OF_MEMORY, or LU_ERR_LIMIT when `text` exceeds
 * LU_PATH_EXPR_MAX_BYTES.
 */
LU_CORE_API int lu_path_expr_parse(lu_view text, lu_path_expr **out,
                                   char *err_buf, size_t err_cap);

/* Release an expression created by lu_path_expr_parse() / clone(). NULL-safe. */
LU_CORE_API void lu_path_expr_destroy(lu_path_expr *expr);

/* Deep copy. NULL-safe; returns LU_ERR_INVALID_ARG for NULL inputs. */
LU_CORE_API int lu_path_expr_clone(const lu_path_expr *expr, lu_path_expr **out);

/* 0 when the expression is invalid (parse failed), non-zero otherwise. */
LU_CORE_API int lu_path_expr_valid(const lu_path_expr *expr);

/* Non-zero when the expression has no ops (the empty expression). */
LU_CORE_API int lu_path_expr_empty(const lu_path_expr *expr);

/* Number of entries in the prefix-op stream. */
LU_CORE_API int lu_path_expr_op_count(const lu_path_expr *expr, size_t *out);

/* Op at `index`; returns LU_ERR_INVALID_ARG when out of range. */
LU_CORE_API int lu_path_expr_op_at(const lu_path_expr *expr, size_t index,
                                   lu_path_expr_op *out);

/* Op counts of the operand streams. */
LU_CORE_API int lu_path_expr_pattern_count(const lu_path_expr *expr,
                                           size_t *out);
LU_CORE_API int lu_path_expr_ref_count(const lu_path_expr *expr, size_t *out);

/* Borrowed view of pattern `index`. Views point into the expression's own
 * storage and stay valid until the expression is destroyed or mutated. */
LU_CORE_API int lu_path_expr_pattern_at(const lu_path_expr *expr, size_t index,
                                        lu_pattern_view *out);
LU_CORE_API int lu_path_expr_component_at(const lu_path_expr *expr,
                                          size_t index, size_t component,
                                          lu_component_view *out);

/* Borrowed view of reference `index`. */
LU_CORE_API int lu_path_expr_ref_at(const lu_path_expr *expr, size_t index,
                                    lu_ref_view *out);

/* Canonical text of the expression.
 *
 * Counted-buffer contract:
 *   - buf == NULL && cap == 0  -> size query; *out_len = bytes needed (no NUL).
 *   - buf != NULL && cap > 0   -> writes text + NUL when cap >= len + 1 and
 *                                 returns LU_OK with *out_len = len.
 *   - otherwise                -> LU_ERR_BUFFER with *out_len = len.
 * `out_len` may be NULL when only the text is wanted.
 */
LU_CORE_API int lu_path_expr_get_text(const lu_path_expr *expr, char *buf,
                                      size_t cap, size_t *out_len);

/* Match a parsed expression against `path`.
 * Returns 1 (match), 0 (no match) or a negative lu_status on error.
 * A short memo table makes this O(elems * segments) per expression. */
LU_CORE_API int lu_path_expr_match_path(const lu_path_expr *expr, lu_view path,
                                        const lu_path_expr_callbacks *cb,
                                        void *user);

/* Match a single pattern (by index into the expression) against `path`. */
LU_CORE_API int lu_path_expr_match_pattern(const lu_path_expr *expr,
                                           size_t index, lu_view path,
                                           const lu_path_expr_callbacks *cb,
                                           void *user);

/* ============================================================
 * Generated checker tables (src/next/c/generated-schema-tables.c)
 *
 * The AOUSD schema inventory: 134 schemas, 403 properties, 691 shaders and the
 * checker rule -> specification mapping. All of it is a NUL-separated string
 * arena plus index rows, so nothing is constructed at startup and nothing
 * allocates per lookup. Rows are name-sorted for binary search.
 * ============================================================ */

typedef struct lu_validation_schema_view {
  lu_view name;
  lu_view parent;            /* canonical parent schema name; empty for roots */
  lu_view kind;              /* abstract, concrete, singleApply, ... */
  lu_view property_namespace;/* namespaced prefix for multipleApply schemas */
  lu_view source;            /* defining schema.usda path */
  size_t property_count;
} lu_validation_schema_view;

typedef struct lu_validation_property_view {
  lu_view name;
  lu_view type;
  lu_view variability;
  int is_relationship;
  size_t token_count;
} lu_validation_property_view;

typedef struct lu_validation_shader_view {
  lu_view identifier;
  lu_view source_type;
  lu_view source;
  size_t input_count;
  size_t output_count;
} lu_validation_shader_view;

/* Number of schemas in the builtin inventory. */
LU_CORE_API int lu_validation_schema_count(size_t *out);

/* Index of the schema named `name`, or LU_ERR_NOT_FOUND-like -6 (see
 * lu_validation_lookup_miss). */
LU_CORE_API int lu_validation_schema_find(lu_view name, size_t *index);

/* Borrowed view of schema `index`; views point into the arena and stay valid
 * for the lifetime of the program. */
LU_CORE_API int lu_validation_schema_at(size_t index,
                                        lu_validation_schema_view *out);

/* Property `prop_index` within schema `schema_index`. */
LU_CORE_API int lu_validation_property_at(size_t schema_index, size_t prop_index,
                                          lu_validation_property_view *out);

/* Index of the property named `name` within schema `schema_index`. */
LU_CORE_API int lu_validation_property_find(size_t schema_index, lu_view name,
                                            size_t *prop_index);

/* `token_index` of the property's allowedTokens list. */
LU_CORE_API int lu_validation_property_token(size_t schema_index,
                                             size_t prop_index,
                                             size_t token_index, lu_view *out);

LU_CORE_API int lu_validation_shader_count(size_t *out);
LU_CORE_API int lu_validation_shader_find(lu_view identifier,
                                          lu_view source_type, size_t *index);
LU_CORE_API int lu_validation_shader_at(size_t index,
                                        lu_validation_shader_view *out);
LU_CORE_API int lu_validation_shader_port(size_t index, int is_output,
                                          size_t port_index, lu_view *name,
                                          lu_view *type);

LU_CORE_API int lu_validation_ref_error_count(size_t *out);
/* Row index and error count for a rule; the individual entries are then read
 * one by one with lu_validation_ref_error_item(). */
LU_CORE_API int lu_validation_ref_error_find(lu_view rule, size_t *index,
                                             size_t *count);
LU_CORE_API int lu_validation_ref_error_item(size_t rule_index, size_t item,
                                             lu_view *out);

/* ============================================================
 * Integer formatting (src/next/c/strfmt.c)
 *
 * Locale-independent, libc-free decimal formatting. Byte-identical to
 * `ss << v` under the classic locale. `buf` needs >= 20 bytes for lu_uint_to
 * and >= 21 for lu_int_to; the return value is the number of bytes written
 * (no NUL terminator).
 * ============================================================ */

LU_CORE_API size_t lu_uint_to(char *buf, uint64_t v);
LU_CORE_API size_t lu_int_to(char *buf, int64_t v);

/* ============================================================
 * IEEE 754 half <-> float (src/next/c/half.c)
 * ============================================================ */

LU_CORE_API float lu_half_to_float(uint16_t h);
LU_CORE_API uint16_t lu_float_to_half(float value);

/* ============================================================
 * USDA lexer token types (src/next/c/lexer.c)
 *
 * Mirror of next::TokenType (parser/lexer.hh); the numeric values must stay in
 * sync. Used by lu_lex_lookup_keyword.
 * ============================================================ */

typedef enum lu_token_type {
  LU_TOK_INVALID = 0,
  LU_TOK_EOF = 1,
  LU_TOK_IDENTIFIER = 2,
  LU_TOK_STRING = 3,
  LU_TOK_NUMBER = 4,
  LU_TOK_PATHREF = 5,
  LU_TOK_DEF = 6,
  LU_TOK_OVER = 7,
  LU_TOK_CLASS = 8,
  LU_TOK_TRUE = 9,
  LU_TOK_FALSE = 10,
  LU_TOK_NONE = 11,
  LU_TOK_OPENPAREN = 12,
  LU_TOK_CLOSEPAREN = 13,
  LU_TOK_OPENBRACKET = 14,
  LU_TOK_CLOSEBRACKET = 15,
  LU_TOK_OPENBRACE = 16,
  LU_TOK_CLOSEBRACE = 17,
  LU_TOK_EQUALS = 18,
  LU_TOK_COLON = 19,
  LU_TOK_DOT = 20,
  LU_TOK_COMMA = 21,
  LU_TOK_SEMICOLON = 22,
  LU_TOK_AT = 23,
  LU_TOK_TIMESAMPLES = 24,
  LU_TOK_CUSTOM = 25,
  LU_TOK_UNIFORM = 26,
  LU_TOK_VARYING = 27,
  LU_TOK_PREPEND = 28,
  LU_TOK_APPEND = 29,
  LU_TOK_DELETE = 30,
  LU_TOK_ADD = 31,
  LU_TOK_REORDER = 32,
  LU_TOK_REL = 33,
} lu_token_type;

/* ============================================================
 * Built-in USD type metadata (src/next/c/type-names.c)
 *
 * The USD<->TypeId mapping and per-type layout. Metadata only: no per-type
 * executable callbacks. C++ TypeInfo descriptors are constructed field by
 * field from these records; they do not alias the C storage.
 * ============================================================ */

/* TypeId values; keep in sync with next::TypeId (type-id.hh). */
typedef enum lu_type_id {
  LU_TYPE_INVALID = 0,
  LU_TYPE_BOOL = 1,
  LU_TYPE_INT = 2,
  LU_TYPE_UINT = 3,
  LU_TYPE_INT64 = 4,
  LU_TYPE_UINT64 = 5,
  LU_TYPE_HALF = 6,
  LU_TYPE_FLOAT = 7,
  LU_TYPE_DOUBLE = 8,
  LU_TYPE_STRING = 9,
  LU_TYPE_TOKEN = 10,
  LU_TYPE_ASSETPATH = 11,
  LU_TYPE_INT2 = 12,
  LU_TYPE_INT3 = 13,
  LU_TYPE_INT4 = 14,
  LU_TYPE_UINT2 = 15,
  LU_TYPE_UINT3 = 16,
  LU_TYPE_UINT4 = 17,
  LU_TYPE_HALF2 = 18,
  LU_TYPE_HALF3 = 19,
  LU_TYPE_HALF4 = 20,
  LU_TYPE_FLOAT2 = 21,
  LU_TYPE_FLOAT3 = 22,
  LU_TYPE_FLOAT4 = 23,
  LU_TYPE_DOUBLE2 = 24,
  LU_TYPE_DOUBLE3 = 25,
  LU_TYPE_DOUBLE4 = 26,
  LU_TYPE_QUATH = 27,
  LU_TYPE_QUATF = 28,
  LU_TYPE_QUATD = 29,
  LU_TYPE_POINT3H = 30,
  LU_TYPE_POINT3F = 31,
  LU_TYPE_POINT3D = 32,
  LU_TYPE_VECTOR3H = 33,
  LU_TYPE_VECTOR3F = 34,
  LU_TYPE_VECTOR3D = 35,
  LU_TYPE_NORMAL3H = 36,
  LU_TYPE_NORMAL3F = 37,
  LU_TYPE_NORMAL3D = 38,
  LU_TYPE_COLOR3H = 39,
  LU_TYPE_COLOR3F = 40,
  LU_TYPE_COLOR3D = 41,
  LU_TYPE_COLOR4H = 42,
  LU_TYPE_COLOR4F = 43,
  LU_TYPE_COLOR4D = 44,
  LU_TYPE_MATRIX2F = 45,
  LU_TYPE_MATRIX2D = 46,
  LU_TYPE_MATRIX3F = 47,
  LU_TYPE_MATRIX3D = 48,
  LU_TYPE_MATRIX4F = 49,
  LU_TYPE_MATRIX4D = 50,
  LU_TYPE_TEXCOORD2H = 51,
  LU_TYPE_TEXCOORD2F = 52,
  LU_TYPE_TEXCOORD2D = 53,
  LU_TYPE_TEXCOORD3H = 54,
  LU_TYPE_TEXCOORD3F = 55,
  LU_TYPE_TEXCOORD3D = 56,
  LU_TYPE_TIMECODE = 57,
  LU_TYPE_EXTENT = 58,
  LU_TYPE_DICTIONARY = 59,
  LU_TYPE_RELATIONSHIP = 60,
  LU_TYPE_REFERENCE = 61,
  LU_TYPE_UCHAR = 62,
  LU_TYPE_FRAME4D = 63,
  LU_TYPE_PATHEXPRESSION = 64,
  LU_TYPE_COUNT = 65,
} lu_type_id;

#define LU_TYPE_FLAG_SCALAR 1
#define LU_TYPE_FLAG_NUMERIC 2


typedef struct lu_type_info {
  uint16_t id;
  const char *name;      /* USD type name, e.g. "float3"; NULL for Invalid */
  const char *cpp_name;  /* C++ spelling; informational */
  size_t size;           /* 0 for variable-size types */
  size_t alignment;
  uint16_t component_type;
  uint8_t component_count;
  uint8_t flags;         /* LU_TYPE_FLAG_* */
} lu_type_info;

/* Row for `id`, or NULL when out of range. */
LU_CORE_API const lu_type_info *lu_type_info_at(uint16_t id);
/* USD type name for `id`, or NULL for Invalid/unknown. */
LU_CORE_API const char *lu_type_name(uint16_t id);
/* TypeId for a bounded type name, or LU_TYPE_INVALID when unknown. */
LU_CORE_API uint16_t lu_type_id_from_name(const char *name, size_t len);
LU_CORE_API size_t lu_type_size(uint16_t id);
LU_CORE_API size_t lu_type_alignment(uint16_t id);
LU_CORE_API int lu_type_is_scalar(uint16_t id);
LU_CORE_API int lu_type_is_numeric(uint16_t id);
LU_CORE_API uint16_t lu_type_component_type(uint16_t id);
LU_CORE_API uint8_t lu_type_component_count(uint16_t id);

/* ============================================================
 * Time-sample interpolation primitives (src/next/c/interpolation.c)
 *
 * Value-free numeric core of src/next/types/interpolation.cc: the
 * linearly-interpolatable type predicate and the scalar/vector lerp and
 * quaternion slerp kernels. The Value-typed InterpolateValues stays C++.
 * ============================================================ */

LU_CORE_API int lu_type_is_linear_interpolatable(uint16_t id);
LU_CORE_API float lu_lerp_f(float a, float b, float t);
LU_CORE_API double lu_lerp_d(double a, double b, double t);
LU_CORE_API void lu_lerp_fn(float *out, const float *a, const float *b,
                            float t, size_t n);
LU_CORE_API void lu_lerp_dn(double *out, const double *a, const double *b,
                            double t, size_t n);
/* out/a/b are 4-element xyzw quaternions. */
LU_CORE_API void lu_slerp_quatf(float *out, const float *a, const float *b,
                                float t);
LU_CORE_API void lu_slerp_quatd(double *out, const double *a, const double *b,
                                double t);

/* ============================================================
 * Value array/storage type queries (src/next/c/value-storage.c)
 *
 * Value-free type predicates of src/next/types/value.cc and value-view.cc:
 * which backing storage an array element type uses, the scalar-component and
 * byte-size mapping, and the array-size validity checks. Pure functions of
 * lu_type_id.
 * ============================================================ */

LU_CORE_API int lu_value_uses_string_storage(uint16_t id);
LU_CORE_API int lu_value_is_float_backed_array(uint16_t id);
LU_CORE_API int lu_value_is_int_backed_array(uint16_t id);
LU_CORE_API int lu_value_is_uint_backed_array(uint16_t id);
LU_CORE_API int lu_value_is_double_backed_array(uint16_t id);
LU_CORE_API int lu_value_fits_array_size(size_t count);
LU_CORE_API int lu_value_is_valid_component_array(size_t scalar_count,
                                                  uint16_t elem_type,
                                                  uint32_t comps_per_elem,
                                                  int storage_matches);
LU_CORE_API size_t lu_value_component_array_size(size_t scalar_count,
                                                 size_t components);
LU_CORE_API int lu_value_is_array_type(uint16_t id);
/* Scalar component type of an element type, or the type itself when scalar. */
LU_CORE_API uint16_t lu_scalar_component(uint16_t id);
LU_CORE_API int lu_requires_lazy_component_reorder(uint16_t id);
/* Byte size of a borrowable POD scalar component, or 0 if unsupported. */
LU_CORE_API size_t lu_scalar_byte_size(uint16_t comp);

/* ============================================================
 * SIMD byte scanning for the USDA parser (src/next/c/simd-scan.c)
 *
 * Bulk-skip "boring" array/prim bytes to the next structurally-interesting
 * byte. Byte-identical to the scalar scan; the SIMD kernels only change speed.
 * `newlines`/`commas` accumulate tallies over the scanned-over prefix.
 * ============================================================ */

LU_CORE_API const char *lu_simd_scan_array_structural(const char *p,
                                                      const char *end,
                                                      size_t *newlines,
                                                      size_t *commas);
LU_CORE_API const char *lu_simd_scan_prim_structural(const char *p,
                                                     const char *end,
                                                     size_t *newlines);
/* Active backend name: "sse2" / "neon" / "scalar". */
LU_CORE_API const char *lu_simd_scan_backend(void);

/* ============================================================
 * USDA lexical primitives (src/next/c/lexer.c)
 *
 * Byte classification, the inf/infinity/nan special-float word matcher, the
 * keyword table lookup and the numeric-literal length scanner. The stateful
 * Lexer class stays C++.
 * ============================================================ */

LU_CORE_API int lu_lex_is_identifier_start(const char *data, size_t size,
                                           size_t pos);
LU_CORE_API int lu_lex_is_identifier_continue_ascii(char c);
LU_CORE_API int lu_lex_is_digit(char c);
LU_CORE_API int lu_lex_is_hex_digit(char c);
LU_CORE_API int lu_lex_hex_val(char c);
LU_CORE_API size_t lu_lex_match_float_special(const char *p, const char *end);
/* Shared array grammar. rows=0, columns=1 denotes scalars; rows=0,
 * columns=2..4 denotes tuples; rows=columns=2..4 denotes matrices.
 * The callback reads up to max_values comma-separated scalars, returning its
 * consumed byte count (zero on failure) and the number of values read. It stops
 * before the separator following the last requested value. A shorter batch is
 * allowed for scalar arrays; tuple/matrix rows require exactly columns values.
 * It may append to caller-owned storage. Batching keeps conversions inlined
 * inside the caller rather than making an indirect call for every component.
 * Returns 1 only after consuming the complete array, otherwise 0. out_count
 * is optional, zeroed on entry and published only on success. Callback side
 * effects on failure are not rolled back: discard partial output. Limits are
 * literal ceilings (zero allows only an empty array). No allocation or ownership
 * crosses this boundary; callback spans are borrowed for the duration of the call.
 */
typedef size_t (*lu_usda_read_values)(void *user, lu_view remaining,
                                      size_t max_values, size_t *out_values);
LU_CORE_API int lu_usda_walk_array(lu_view input, unsigned rows,
                                   unsigned columns, size_t max_elements,
                                   size_t max_scalars,
                                   lu_usda_read_values read_values, void *user,
                                   size_t *out_count);

/* TokenType (lu_token_type) for a bounded identifier, else LU_TOK_IDENTIFIER. */
LU_CORE_API uint8_t lu_lex_lookup_keyword(const char *p, size_t n);
/* Byte length of the numeric literal starting at p. */
LU_CORE_API size_t lu_lex_scan_number(const char *p, const char *end);

/* ============================================================
 * Identifier / path validation (src/next/c/identifier.c)
 *
 * AOUSD Core Spec section 8 identifier, namespaced-property and textual path
 * grammar, plus UTF-8 decoding. All allocation-free.
 * ============================================================ */

/* Decode one UTF-8 scalar at `offset`. Returns 0 on malformed input. */
LU_CORE_API int lu_utf8_decode(const char *data, size_t size, size_t offset,
                               uint32_t *codepoint, size_t *width);
/* Identifier character classes: XID_Start (plus '_') / XID_Continue. */
LU_CORE_API int lu_is_identifier_start(uint32_t codepoint);
LU_CORE_API int lu_is_identifier_continue(uint32_t codepoint);
LU_CORE_API int lu_is_valid_identifier(const char *data, size_t size);
LU_CORE_API int lu_is_valid_namespaced_identifier(const char *data, size_t size);
LU_CORE_API int lu_is_valid_path_string(const char *data, size_t size);
/* ============================================================
 * Decimal integer parsing (src/next/c/decimal.c)
 *
 * ASCII-parser numeric helpers. NUL-terminated input, optional leading sign,
 * saturation (or overflow failure for the checked variants).
 * ============================================================ */

LU_CORE_API int64_t lu_decimal_to_i64(const char *s);
LU_CORE_API int32_t lu_decimal_to_i32(const char *s);
LU_CORE_API uint64_t lu_decimal_to_u64(const char *s);
LU_CORE_API uint32_t lu_decimal_to_u32(const char *s);
/* 0 on overflow (pxr errors on out-of-range integer literals). */
LU_CORE_API int lu_decimal_to_i64_checked(const char *s, int64_t *out);
LU_CORE_API int lu_decimal_to_u64_checked(const char *s, uint64_t *out);
/* Bounded "is this a decimal integer token" check. */
LU_CORE_API int lu_is_decimal_int_token(const char *s, size_t len,
                                        int allow_neg);
/* ============================================================
 * 4x4 double transform math (src/next/c/mat4.c)
 *
 * Row-vector (USD/legacy) convention, bit-identical to the legacy evaluator:
 * reduced-argument sin_pi/cos_pi rather than sin/cos of deg*pi/180. Pure
 * double[16] math, no allocation.
 * ============================================================ */

LU_CORE_API void lu_mat4_identity(double *m);
/* result = a * b (row-vector convention). result may alias neither input. */
LU_CORE_API void lu_mat4_mul(double *result, const double *a, const double *b);
LU_CORE_API double lu_cos_pi(double x);
LU_CORE_API double lu_sin_pi(double x);
LU_CORE_API void lu_mat4_translation(double *m, double x, double y, double z);
LU_CORE_API void lu_mat4_scale(double *m, double x, double y, double z);
LU_CORE_API void lu_mat4_rot_x(double *m, double deg);
LU_CORE_API void lu_mat4_rot_y(double *m, double deg);
LU_CORE_API void lu_mat4_rot_z(double *m, double deg);
LU_CORE_API void lu_mat4_rot_axis(double *m, char axis, double deg);
LU_CORE_API void lu_mat4_orient(double *m, double w, double x, double y,
                                double z);
/* Invert; identity when singular. out may alias a. */
LU_CORE_API void lu_mat4_invert(const double *a, double *out);
/* ============================================================
 * USD path string algorithms (src/next/c/path.c)
 *
 * Counted-buffer decomposition of a USD path. Each function writes at most
 * `cap` bytes and sets *out_len to the full length; it returns 0 (with
 * *out_len = needed) when the output does not fit.
 * ============================================================ */

LU_CORE_API int lu_path_is_absolute(const char *p, size_t n);
LU_CORE_API int lu_path_is_root(const char *p, size_t n);
LU_CORE_API int lu_path_has_property(const char *p, size_t n);
/* Returns 1 and writes the parent, or 0 when the path has no parent (empty or
 * root). */
LU_CORE_API int lu_path_parent(const char *p, size_t n, char *out, size_t cap,
                               size_t *out_len);
LU_CORE_API int lu_path_name(const char *p, size_t n, char *out, size_t cap,
                             size_t *out_len);
LU_CORE_API int lu_path_property_name(const char *p, size_t n, char *out,
                                      size_t cap, size_t *out_len);
LU_CORE_API int lu_path_prim_path(const char *p, size_t n, char *out,
                                  size_t cap, size_t *out_len);
/* `prim` must already be a prim path (no property). */
LU_CORE_API int lu_path_element_count(const char *prim, size_t n, size_t *out);
LU_CORE_API int lu_path_element(const char *prim, size_t n, size_t index,
                                char *out, size_t cap, size_t *out_len);
LU_CORE_API int lu_path_make_relative_to(const char *p, size_t n,
                                         const char *base, size_t bn, char *out,
                                         size_t cap, size_t *out_len);
/* ============================================================
 * Cubic spline segment math (src/next/c/spline.c)
 *
 * Hermite basis, cubic Bezier evaluation/derivative, the Bezier time solver and
 * anti-regression from src/spline-eval.hh. Pure double math; the templated knot
 * walk stays C++.
 * ============================================================ */

LU_CORE_API double lu_hermite_h00(double u);
LU_CORE_API double lu_hermite_h10(double u);
LU_CORE_API double lu_hermite_h01(double u);
LU_CORE_API double lu_hermite_h11(double u);
LU_CORE_API double lu_cubic_bezier(double p0, double p1, double p2, double p3,
                                   double u);
LU_CORE_API double lu_cubic_bezier_deriv(double p0, double p1, double p2,
                                         double p3, double u);
LU_CORE_API double lu_solve_bezier_time_for_u(double t0, double t1, double t2,
                                              double t3, double target_time,
                                              int max_iter);
LU_CORE_API void lu_spline_anti_regress(double t0, double *tw0_post,
                                        double *tw1_pre, double t3);

/* Numeric view of one spline knot, mirroring the fields the auto-ease
 * algorithm reads/writes. The C++ SplineKnot adds a std::string, so this is a
 * plain-C value type the caller marshals to/from. */
typedef struct lu_spline_knot {
  double time;
  double value;
  double pre_value;       /* dual-valued knot: value approaching from before */
  uint8_t dual;
  uint8_t interp;         /* interpolation of the segment AFTER this knot */
  uint8_t pre_algo;       /* TsTangentAlgorithm: 0=None, 1=Custom, 2=AutoEase */
  uint8_t post_algo;
  double pre_tan_width;
  double pre_tan_slope;
  double post_tan_width;
  double post_tan_slope;
} lu_spline_knot;

/* Recompute the pre/post tangent slope+width of every AutoEase (algo==2) knot
 * from its neighbors, in place. Zeroes the tangent at a discontinuity. Matches
 * next::ComputeAutoEaseTangents (which forwards here). */
LU_CORE_API void lu_spline_compute_auto_ease(lu_spline_knot *knots, size_t n);

/* ============================================================
 * Crate integer decode (src/next/c/crate-int.c)
 *
 * pxrUSD crate integer sections: the common-prefix and delta-coded formats,
 * plus the packed code-table size. The decode side turns untrusted crate bytes
 * into integers; all functions bound every read against src_size and return 0
 * on malformed input.
 * ============================================================ */

/* ============================================================
 * LZ4 + crate blob framing (src/next/c/crate-blob.c)
 *
 * The pxrUSD TfFastCompression layout and raw LZ4 wrappers. All functions
 * write into a caller buffer bounded by `dst_cap`/`dst_size` and report the
 * number of bytes written; `*out_error` is set to a static message on failure.
 * ============================================================ */

/* Decompress one LZ4 block, up to dst_cap bytes; *out_size is the actual size. */
LU_CORE_API int lu_lz4_decompress_upto(const uint8_t *src, size_t src_size,
                                       uint8_t *dst, size_t dst_cap,
                                       size_t *out_size, const char **out_error);
/* Decompress one LZ4 block that must decode to exactly dst_size bytes. */
LU_CORE_API int lu_lz4_decompress(const uint8_t *src, size_t src_size,
                                  uint8_t *dst, size_t dst_size,
                                  size_t *out_size, const char **out_error);
/* LZ4_compressBound; 0 on failure. */
LU_CORE_API int lu_lz4_compress_bound(size_t src_size, size_t *out_bound);
LU_CORE_API int lu_lz4_compress(const uint8_t *src, size_t src_size,
                                uint8_t *dst, size_t dst_cap, size_t *out_size,
                                const char **out_error);

/* ============================================================
 * Crate value-type metadata (src/next/c/crate-typeid.c)
 *
 * Wire values of the SdfValueTypeName byte and per-type fixed sizes. Keep in
 * sync with next::CrateTypeId (crate-format.hh).
 * ============================================================ */

typedef enum lu_crate_type_id {
  LU_CRATE_TYPE_INVALID = 0,
  LU_CRATE_TYPE_BOOL = 1,
  LU_CRATE_TYPE_UCHAR = 2,
  LU_CRATE_TYPE_INT = 3,
  LU_CRATE_TYPE_UINT = 4,
  LU_CRATE_TYPE_INT64 = 5,
  LU_CRATE_TYPE_UINT64 = 6,
  LU_CRATE_TYPE_HALF = 7,
  LU_CRATE_TYPE_FLOAT = 8,
  LU_CRATE_TYPE_DOUBLE = 9,
  LU_CRATE_TYPE_STRING = 10,
  LU_CRATE_TYPE_TOKEN = 11,
  LU_CRATE_TYPE_ASSETPATH = 12,
  LU_CRATE_TYPE_MATRIX2D = 13,
  LU_CRATE_TYPE_MATRIX3D = 14,
  LU_CRATE_TYPE_MATRIX4D = 15,
  LU_CRATE_TYPE_QUATD = 16,
  LU_CRATE_TYPE_QUATF = 17,
  LU_CRATE_TYPE_QUATH = 18,
  LU_CRATE_TYPE_VEC2D = 19,
  LU_CRATE_TYPE_VEC2F = 20,
  LU_CRATE_TYPE_VEC2H = 21,
  LU_CRATE_TYPE_VEC2I = 22,
  LU_CRATE_TYPE_VEC3D = 23,
  LU_CRATE_TYPE_VEC3F = 24,
  LU_CRATE_TYPE_VEC3H = 25,
  LU_CRATE_TYPE_VEC3I = 26,
  LU_CRATE_TYPE_VEC4D = 27,
  LU_CRATE_TYPE_VEC4F = 28,
  LU_CRATE_TYPE_VEC4H = 29,
  LU_CRATE_TYPE_VEC4I = 30,
  LU_CRATE_TYPE_DICTIONARY = 31,
  LU_CRATE_TYPE_TOKENLISTOP = 32,
  LU_CRATE_TYPE_STRINGLISTOP = 33,
  LU_CRATE_TYPE_PATHLISTOP = 34,
  LU_CRATE_TYPE_REFERENCELISTOP = 35,
  LU_CRATE_TYPE_INTLISTOP = 36,
  LU_CRATE_TYPE_INT64LISTOP = 37,
  LU_CRATE_TYPE_UINTLISTOP = 38,
  LU_CRATE_TYPE_UINT64LISTOP = 39,
  LU_CRATE_TYPE_PATHVECTOR = 40,
  LU_CRATE_TYPE_TOKENVECTOR = 41,
  LU_CRATE_TYPE_SPECIFIER = 42,
  LU_CRATE_TYPE_PERMISSION = 43,
  LU_CRATE_TYPE_VARIABILITY = 44,
  LU_CRATE_TYPE_VARIANTSELECTIONMAP = 45,
  LU_CRATE_TYPE_TIMESAMPLES = 46,
  LU_CRATE_TYPE_PAYLOAD = 47,
  LU_CRATE_TYPE_DOUBLEVECTOR = 48,
  LU_CRATE_TYPE_LAYEROFFSETVECTOR = 49,
  LU_CRATE_TYPE_STRINGVECTOR = 50,
  LU_CRATE_TYPE_VALUEBLOCK = 51,
  LU_CRATE_TYPE_VALUE = 52,
  LU_CRATE_TYPE_UNREGISTEREDVALUE = 53,
  LU_CRATE_TYPE_UNREGISTEREDVALUELISTOP = 54,
  LU_CRATE_TYPE_PAYLOADLISTOP = 55,
  LU_CRATE_TYPE_TIMECODE = 56,
  LU_CRATE_TYPE_PATHEXPRESSION = 57,
  LU_CRATE_TYPE_RELOCATES = 58,
  LU_CRATE_TYPE_SPLINE = 59,
} lu_crate_type_id;

/* Diagnostic name of a crate value type, or "Unknown". */
LU_CORE_API const char *lu_crate_type_name(uint8_t id);
/* Fixed element size in bytes; 0 for variable-size/unknown types. */
LU_CORE_API size_t lu_crate_type_size(uint8_t id);

/* Packed 2-bit code-table bytes for `count` values. */
LU_CORE_API int lu_crate_blob_compress_bound(size_t src_size,
                                             size_t *out_bound);
LU_CORE_API int lu_crate_blob_compress(const uint8_t *src, size_t src_size,
                                       uint8_t *dst, size_t dst_cap,
                                       size_t *out_size,
                                       const char **out_error);
/* Decompress a framed crate blob into dst_size bytes; *out_size <= dst_size. */
LU_CORE_API int lu_crate_blob_decompress(const uint8_t *src, size_t src_size,
                                         uint8_t *dst, size_t dst_size,
                                         size_t *out_size,
                                         const char **out_error);
/* Explicit-budget variant for validated large structural tables. No memory is
 * allocated here. dst must back dst_size bytes, which must not exceed the
 * caller's max_output_size. The ordinary entry point keeps its 1 GiB cap. */
LU_CORE_API int lu_crate_blob_decompress_bounded(
    const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size,
    size_t max_output_size, size_t *out_size, const char **out_error);

/* Packed 2-bit code-table bytes for `count` values. */
LU_CORE_API int lu_crate_delta_code_bytes(size_t count, size_t *out);
LU_CORE_API int lu_crate_decode_integers(const uint8_t *src, size_t src_size,
                                         uint32_t *dst, size_t count);
LU_CORE_API int lu_crate_decode_integers64(const uint8_t *src, size_t src_size,
                                           uint64_t *dst, size_t count);
LU_CORE_API int lu_crate_decode_delta_u32(const uint8_t *buffer,
                                          size_t buffer_size, uint32_t *dst,
                                          size_t count);
LU_CORE_API int lu_crate_decode_delta_s32(const uint8_t *buffer,
                                          size_t buffer_size, int32_t *dst,
                                          size_t count);
LU_CORE_API int lu_crate_decode_delta_u64(const uint8_t *buffer,
                                          size_t buffer_size, uint64_t *dst,
                                          size_t count);
/* Common-prefix integer encode: `lu_crate_encode_integers_bound` is the worst
 * case (1 + 4*count bytes); *out_size is the exact written size. */
LU_CORE_API int lu_crate_encode_integers_bound(size_t count, size_t *out_bound);
LU_CORE_API int lu_crate_encode_integers(const uint32_t *values, size_t count,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size,
                                         const char **out_error);

/* Delta-coded integer encode (src/next/c/crate-delta-encode.c). The bound is
 * the worst case: 4 + code bytes + one full-width value per element
 * (u32 -> i32, s32 -> i64). *out_size is the exact written size. */
LU_CORE_API int lu_crate_encode_delta_u32_bound(size_t count,
                                                size_t *out_bound);
LU_CORE_API int lu_crate_encode_delta_s32_bound(size_t count,
                                                size_t *out_bound);
LU_CORE_API int lu_crate_encode_delta_u32(const uint32_t *values, size_t count,
                                          uint8_t *dst, size_t dst_cap,
                                          size_t *out_size,
                                          const char **out_error);
LU_CORE_API int lu_crate_encode_delta_s32(const int32_t *values, size_t count,
                                          uint8_t *dst, size_t dst_cap,
                                          size_t *out_size,
                                          const char **out_error);


/* ============================================================
 * C11 semi-any value core (src/next/c/value.c)
 *
 * One C11 implementation owns inline values and retained, copy-on-write
 * strings, arrays, dictionaries, boxes and lazy decoder contexts. C++ and
 * language bindings use this carrier without constructing C++ payloads in it.
 * ============================================================ */
#define LU_VALUE_INLINE_CAP 40

typedef enum lu_value_kind {
    LU_VALUE_EMPTY = 0,
    LU_VALUE_BLOCK,
    LU_VALUE_POD,
    LU_VALUE_STRING,
    LU_VALUE_ARRAY,
    LU_VALUE_LAZY_ARRAY,
    LU_VALUE_DICT,
    LU_VALUE_BOX
} lu_value_kind;

typedef struct lu_value {
    uint16_t type;  /* lu_type_id */
    uint8_t kind;   /* lu_value_kind */
    uint8_t flags;
    uint32_t size;  /* array element count where relevant */
    /* 8-byte alignment covers every inline scalar (double / int64 lanes);
     * max_align_t (16 on x86-64) would pad the carrier from 48 to 64 bytes,
     * and composed stages hold tens of millions of values. The layout is
     * reported at run time by lu_abi_get() (value_size / value_alignment). */
    union {
        uint64_t _align;
        double _align_f;
        unsigned char bytes[LU_VALUE_INLINE_CAP];
    } store;
} lu_value;

LU_CORE_API lu_value_kind lu_value_kind_of (uint16_t type);
LU_CORE_API void lu_value_init (lu_value* value, uint16_t type);
LU_CORE_API void lu_value_init_empty (lu_value* value);
LU_CORE_API void lu_value_init_block (lu_value* value);
LU_CORE_API void lu_value_destroy (lu_value* value);
/* Destinations must be initialized. Copies retain C11-owned storage and writes
 * detach shared payloads. Moving between a dictionary and its own child is
 * unsupported; use a separate initialized carrier for that operation. */
LU_CORE_API void lu_value_copy (lu_value* dst, const lu_value* src);
LU_CORE_API void lu_value_move (lu_value* dst, lu_value* src);



#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LIGHTUSD_CORE_C_H_ */
