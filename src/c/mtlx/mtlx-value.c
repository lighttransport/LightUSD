/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX names and value strings (C11 port of the value section of
 * src/core/mtlx-resolve.cc: IsValidMtlxName ... LexMtlxValue) and the float
 * formatting of mtlx-dom.cc FormatMtlxFloat.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

static int is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_name_char(char c) { return is_alpha(c) || is_digit(c) || c == '_'; }

/* AllDigits(s, b, e) */
static int all_digits(const char *s, size_t b, size_t e) {
  if (b >= e) return 0;
  for (size_t i = b; i < e; i++)
    if (!is_digit(s[i])) return 0;
  return 1;
}

/* Trim() returning an owned string (NULL on oom). */
static char *trim_dup(const char *s) {
  size_t b = 0, e = strlen(s);
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
  return lu_mtlx_strndup(s + b, e - b);
}

static int trim_is_empty(const char *s) {
  for (; *s; s++)
    if (!(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) return 0;
  return 1;
}

/* Fields(): SplitOn(',') with every field trimmed. */
static int fields(const char *s, lu_mtlx_strv *out) {
  if (!lu_mtlx_split_on(',', s, out)) return 0;
  for (size_t i = 0; i < out->n; i++) {
    char *t = trim_dup(out->v[i]);
    if (!t) {
      lu_mtlx_strv_free(out);
      return 0;
    }
    free(out->v[i]);
    out->v[i] = t;
  }
  return 1;
}

/* ArrayFields(): {} for blank text. */
static int array_fields(const char *s, lu_mtlx_strv *out) {
  if (trim_is_empty(s)) {
    lu_mtlx_strv_init(out);
    return 1;
  }
  return fields(s, out);
}

/* ParseIntToken: optional sign, one or more digits; whole token. */
int lu_mtlx_parse_int_token(const char *s, int64_t *out) {
  size_t b = 0, n = strlen(s);
  int neg = 0;
  if (n && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    b = 1;
  }
  if (!all_digits(s, b, n)) return 0;
  int64_t acc = 0;
  const int64_t kMin = INT64_MIN;
  for (size_t i = b; i < n; i++) {
    const int64_t d = s[i] - '0';
    if (acc < (kMin + d) / 10) return 0;
    acc = acc * 10 - d;
  }
  if (!neg) {
    if (acc == kMin) return 0;
    acc = -acc;
  }
  *out = acc;
  return 1;
}

/* ParseFloatToken: [+-]? digits? ('.' digits?)? ([eE] [+-]? digits)? with at
 * least one mantissa digit. The normalised literal is converted with strtod
 * (correctly rounded, saturating like fast_float). */
int lu_mtlx_parse_float_token(const char *s, double *out) {
  const size_t n = strlen(s);
  size_t b = 0;
  int neg = 0;
  if (n && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    b = 1;
  }
  size_t mant_end = b;
  while (mant_end < n && s[mant_end] != 'e' && s[mant_end] != 'E') mant_end++;
  size_t dot = b;
  while (dot < mant_end && s[dot] != '.') dot++;
  const size_t ipb = b, ipn = dot - b;
  const size_t fpb = dot + 1, fpn = dot < mant_end ? mant_end - dot - 1 : 0;
  if (ipn + fpn == 0) return 0;
  for (size_t i = 0; i < ipn; i++)
    if (!is_digit(s[ipb + i])) return 0;
  for (size_t i = 0; i < fpn; i++)
    if (!is_digit(s[fpb + i])) return 0;
  lu_mtlx_sb norm;
  lu_mtlx_sb_init(&norm);
  if (neg) lu_mtlx_sb_putc(&norm, '-');
  if (ipn) lu_mtlx_sb_put(&norm, s + ipb, ipn); else lu_mtlx_sb_putc(&norm, '0');
  lu_mtlx_sb_putc(&norm, '.');
  if (fpn) lu_mtlx_sb_put(&norm, s + fpb, fpn); else lu_mtlx_sb_putc(&norm, '0');
  if (mant_end < n) {
    size_t eb = mant_end + 1;
    int eneg = 0;
    if (eb < n && (s[eb] == '-' || s[eb] == '+')) {
      eneg = s[eb] == '-';
      eb++;
    }
    if (!all_digits(s, eb, n)) {
      lu_mtlx_sb_free(&norm);
      return 0;
    }
    lu_mtlx_sb_putc(&norm, 'e');
    if (eneg) lu_mtlx_sb_putc(&norm, '-');
    lu_mtlx_sb_puts(&norm, s + eb);
  }
  if (norm.oom) {
    lu_mtlx_sb_free(&norm);
    return 0;
  }
  *out = strtod(norm.p, NULL);
  lu_mtlx_sb_free(&norm);
  return 1;
}

/* ParseFloats */
static int parse_floats(const lu_mtlx_strv *f, double **out, size_t *nout) {
  *out = NULL;
  *nout = 0;
  if (!f->n) return 1;
  double *v = (double *)malloc(f->n * sizeof(double));
  if (!v) return 0;
  for (size_t i = 0; i < f->n; i++) {
    if (!lu_mtlx_parse_float_token(f->v[i], &v[i])) {
      free(v);
      return 0;
    }
  }
  *out = v;
  *nout = f->n;
  return 1;
}

/* SplitStringArray: "\," "\;" "\\" are literal, unescaped ',' separates. */
static int split_string_array(const char *s, lu_mtlx_strv *out) {
  lu_mtlx_strv_init(out);
  lu_mtlx_sb cur;
  lu_mtlx_sb_init(&cur);
  size_t i = 0;
  const size_t n = strlen(s);
  int ok = 1;
  while (i < n && ok) {
    const char c = s[i];
    if (c == '\\' && i + 1 < n && (s[i + 1] == ',' || s[i + 1] == ';' || s[i + 1] == '\\')) {
      lu_mtlx_sb_putc(&cur, s[i + 1]);
      i += 2;
    } else if (c == ',') {
      ok = !cur.oom && lu_mtlx_strv_push(out, lu_mtlx_sb_str(&cur));
      lu_mtlx_sb_clear(&cur);
      i++;
    } else {
      lu_mtlx_sb_putc(&cur, c);
      i++;
    }
  }
  ok = ok && !cur.oom && lu_mtlx_strv_push(out, lu_mtlx_sb_str(&cur));
  lu_mtlx_sb_free(&cur);
  if (!ok) lu_mtlx_strv_free(out);
  return ok;
}

static const char *const kBuiltinTypes[] = {
    "integer",      "boolean",      "float",        "color3",
    "color4",       "vector2",      "vector3",      "vector4",
    "matrix33",     "matrix44",     "string",       "filename",
    "integerarray", "floatarray",   "color3array",  "color4array",
    "vector2array", "vector3array", "vector4array", "stringarray"};

/* IsValidMtlxName */
int lu_mtlx_is_valid_name(const char *name) {
  if (!name[0]) return 0;
  if (!(is_alpha(name[0]) || name[0] == '_')) return 0;
  for (size_t i = 1; name[i]; i++)
    if (!is_name_char(name[i])) return 0;
  return 1;
}

/* IsReferenceValidMtlxName */
int lu_mtlx_is_reference_valid_name(const char *name) {
  for (; *name; name++)
    if (!(is_name_char(*name) || *name == ':')) return 0;
  return 1;
}

/* MtlxTypeArity */
size_t lu_mtlx_type_arity(const char *t) {
  if (!strcmp(t, "color3") || !strcmp(t, "vector3")) return 3;
  if (!strcmp(t, "color4") || !strcmp(t, "vector4")) return 4;
  if (!strcmp(t, "vector2")) return 2;
  if (!strcmp(t, "matrix33")) return 9;
  if (!strcmp(t, "matrix44")) return 16;
  return 0;
}

/* MtlxArrayStride */
size_t lu_mtlx_array_stride(const char *t) {
  if (!strcmp(t, "floatarray")) return 1;
  if (!strcmp(t, "vector2array")) return 2;
  if (!strcmp(t, "color3array") || !strcmp(t, "vector3array")) return 3;
  if (!strcmp(t, "color4array") || !strcmp(t, "vector4array")) return 4;
  return 0;
}

/* IsBuiltinMtlxType */
int lu_mtlx_is_builtin_type(const char *t) {
  for (size_t i = 0; i < sizeof(kBuiltinTypes) / sizeof(kBuiltinTypes[0]); i++)
    if (!strcmp(t, kBuiltinTypes[i])) return 1;
  return 0;
}

void lu_mtlx_typed_value_init(lu_mtlx_typed_value *v) {
  memset(v, 0, sizeof(*v));
  v->kind = LU_MTLX_V_NONE;
  lu_mtlx_strv_init(&v->strs);
}

void lu_mtlx_typed_value_free(lu_mtlx_typed_value *v) {
  free(v->nums);
  free(v->ints);
  free(v->str);
  lu_mtlx_strv_free(&v->strs);
  lu_mtlx_typed_value_init(v);
}

/* integerarray branch of ParseMtlxValue */
static int parse_int_array(const char *text, lu_mtlx_typed_value *out) {
  lu_mtlx_strv f;
  if (!array_fields(text, &f)) return 0;
  int ok = 1;
  if (f.n) {
    out->ints = (int64_t *)malloc(f.n * sizeof(int64_t));
    ok = out->ints != NULL;
    for (size_t i = 0; ok && i < f.n; i++) ok = lu_mtlx_parse_int_token(f.v[i], &out->ints[i]);
    if (ok) out->nints = f.n;
  }
  lu_mtlx_strv_free(&f);
  if (!ok) {
    free(out->ints);
    out->ints = NULL;
    return 0;
  }
  out->kind = LU_MTLX_V_INTS;
  return 1;
}

/* stringarray branch of ParseMtlxValue */
static int parse_string_array(const char *text, lu_mtlx_typed_value *out) {
  if (!trim_is_empty(text)) {
    if (!split_string_array(text, &out->strs)) return 0;
    for (size_t i = 0; i < out->strs.n; i++) {
      char *t = trim_dup(out->strs.v[i]);
      if (!t) {
        lu_mtlx_strv_free(&out->strs);
        return 0;
      }
      free(out->strs.v[i]);
      out->strs.v[i] = t;
    }
  }
  out->kind = LU_MTLX_V_STRS;
  return 1;
}

/* Aggregate / float-array branches of ParseMtlxValue. */
static int parse_float_list(const char *text, int array, size_t n, lu_mtlx_typed_value *out) {
  lu_mtlx_strv f;
  if (!(array ? array_fields(text, &f) : fields(text, &f))) return 0;
  const int ok = parse_floats(&f, &out->nums, &out->nnums) &&
                 (array ? out->nnums % n == 0 : out->nnums == n);
  lu_mtlx_strv_free(&f);
  if (!ok) {
    free(out->nums);
    out->nums = NULL;
    out->nnums = 0;
    return 0;
  }
  out->kind = array ? LU_MTLX_V_NUMS : LU_MTLX_V_AGG;
  return 1;
}

/* ParseMtlxValue. `out` must be initialised; it is reset first. */
int lu_mtlx_parse_value(const char *type, const char *text, lu_mtlx_typed_value *out) {
  if (!out) return 0;
  lu_mtlx_typed_value_free(out);
  if (!strcmp(type, "integer") || !strcmp(type, "float")) {
    char *t = trim_dup(text);
    if (!t) return 0;
    int ok;
    if (type[0] == 'i') {
      ok = lu_mtlx_parse_int_token(t, &out->i);
      if (ok) out->kind = LU_MTLX_V_INT;
    } else {
      ok = lu_mtlx_parse_float_token(t, &out->num);
      if (ok) out->kind = LU_MTLX_V_NUM;
    }
    free(t);
    return ok;
  }
  if (!strcmp(type, "boolean")) {
    if (!strcmp(text, "true")) out->b = 1;
    else if (!strcmp(text, "false")) out->b = 0;
    else return 0;
    out->kind = LU_MTLX_V_BOOL;
    return 1;
  }
  if (!strcmp(type, "string") || !strcmp(type, "filename")) {
    out->str = lu_mtlx_strdup(text);
    if (!out->str) return 0;
    out->kind = LU_MTLX_V_STR;
    return 1;
  }
  if (!strcmp(type, "integerarray")) return parse_int_array(text, out);
  if (!strcmp(type, "stringarray")) return parse_string_array(text, out);
  const size_t arity = lu_mtlx_type_arity(type);
  if (arity > 0) return parse_float_list(text, 0, arity, out);
  const size_t stride = lu_mtlx_array_stride(type);
  if (stride > 0) return parse_float_list(text, 1, stride, out);
  return 0;
}

/* DefaultMtlxValue */
int lu_mtlx_default_value(const char *type, lu_mtlx_typed_value *out) {
  if (!out) return 0;
  lu_mtlx_typed_value_free(out);
  const size_t arity = lu_mtlx_type_arity(type);
  if (!strcmp(type, "integer")) {
    out->kind = LU_MTLX_V_INT;
  } else if (!strcmp(type, "boolean")) {
    out->kind = LU_MTLX_V_BOOL;
  } else if (!strcmp(type, "float")) {
    out->kind = LU_MTLX_V_NUM;
  } else if (arity > 0) {
    out->nums = (double *)calloc(arity, sizeof(double));
    if (!out->nums) return 0;
    out->nnums = arity;
    if (!strcmp(type, "matrix33") || !strcmp(type, "matrix44")) {
      const size_t n = arity == 9 ? 3 : 4;
      for (size_t i = 0; i < n; i++) out->nums[i * n + i] = 1.0;
    }
    out->kind = LU_MTLX_V_AGG;
  } else if (!strcmp(type, "string") || !strcmp(type, "filename")) {
    out->str = lu_mtlx_strdup("");
    if (!out->str) return 0;
    out->kind = LU_MTLX_V_STR;
  } else if (!strcmp(type, "integerarray")) {
    out->kind = LU_MTLX_V_INTS;
  } else if (!strcmp(type, "stringarray")) {
    out->kind = LU_MTLX_V_STRS;
  } else if (lu_mtlx_array_stride(type) > 0) {
    out->kind = LU_MTLX_V_NUMS;
  } else {
    return 0;
  }
  return 1;
}

/* LexMtlxValue (result replaces out's contents). */
int lu_mtlx_lex_value(const char *type, const char *text, lu_mtlx_sb *out) {
  lu_mtlx_sb_clear(out);
  if (!strcmp(type, "string") || !strcmp(type, "filename")) {
    lu_mtlx_sb_puts(out, text);
    return !out->oom;
  }
  lu_mtlx_strv f;
  if (!fields(text, &f)) return 0;
  for (size_t i = 0; i < f.n; i++) {
    if (i) lu_mtlx_sb_putc(out, ',');
    lu_mtlx_sb_puts(out, f.v[i]);
  }
  lu_mtlx_strv_free(&f);
  return !out->oom;
}

/* FormatMtlxFloat: ostream << setprecision(max_digits10 = 9) (default
 * floatfield == %g), classic locale. Appends to out. */
int lu_mtlx_format_float(float v, lu_mtlx_sb *out) {
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%.9g", (double)v);
  if (n < 0 || (size_t)n >= sizeof(buf)) return 0;
  lu_mtlx_sb_put(out, buf, (size_t)n);
  return !out->oom;
}
