/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * validation-tables.c -- lookups over the generated checker tables.
 *
 * Every lookup is a binary search over name-sorted rows plus a string compare
 * inside the NUL-separated arena. There is no per-lookup allocation, no static
 * construction and no mutable state, so the tables can be read from any number
 * of threads and cost nothing until the first call.
 */

#include "lightusd-core-c.h"

#include "generated-schema-tables.h"

#include <stdlib.h>
#include <string.h>

/* --- arena helpers ------------------------------------------------------- */

static lu_view arena_view(uint32_t offset) {
  lu_view v;
  v.data = lu_validation_strings + offset;
  v.len = strlen(lu_validation_strings + offset);
  return v;
}

/* Lexicographic compare of an arena entry against a caller view. */
static int arena_cmp(uint32_t offset, lu_view key) {
  const char *s = lu_validation_strings + offset;
  if (key.data == NULL && key.len != 0) return -1;
  const size_t n = key.len;
  for (size_t i = 0; i < n; i++) {
    /* Arena entries are NUL-terminated, so at i == strlen(s) the byte is 0 and
     * the shorter (arena) entry sorts first. */
    const unsigned char a = (unsigned char)s[i];
    const unsigned char b = (unsigned char)key.data[i];
    if (a != b) return a < b ? -1 : 1;
  }
  return s[n] == '\0' ? 0 : 1;
}

/* --- schema lookups ------------------------------------------------------ */

int lu_validation_schema_count(size_t *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  *out = lu_validation_schema_row_count;
  return LU_OK;
}

int lu_validation_schema_find(lu_view name, size_t *index) {
  if (index == NULL) return LU_ERR_INVALID_ARG;
  size_t lo = 0, hi = lu_validation_schema_row_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = arena_cmp(lu_validation_schema_rows[mid].name, name);
    if (cmp == 0) {
      *index = mid;
      return LU_OK;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return LU_ERR_NOT_FOUND;
}

int lu_validation_schema_at(size_t index, lu_validation_schema_view *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= lu_validation_schema_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_schema_row *r = &lu_validation_schema_rows[index];
  memset(out, 0, sizeof(*out));
  out->name = arena_view(r->name);
  out->parent = arena_view(r->parent);
  out->kind = arena_view(r->kind);
  out->property_namespace = arena_view(r->property_namespace);
  out->source = arena_view(r->source);
  out->property_count = r->property_count;
  return LU_OK;
}

int lu_validation_property_at(size_t schema_index, size_t prop_index,
                              lu_validation_property_view *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  if (schema_index >= lu_validation_schema_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_schema_row *s = &lu_validation_schema_rows[schema_index];
  if (prop_index >= s->property_count) return LU_ERR_INVALID_ARG;
  const size_t i = s->property_first + prop_index;
  if (i >= lu_validation_property_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_property_row *r = &lu_validation_property_rows[i];
  memset(out, 0, sizeof(*out));
  out->name = arena_view(r->name);
  out->type = arena_view(r->type);
  out->variability = arena_view(r->variability);
  out->is_relationship = r->is_relationship;
  out->token_count = r->token_count;
  return LU_OK;
}

/* Property names are sorted inside a schema's range, so a binary search over
 * that range is exact and O(log count). */
int lu_validation_property_find(size_t schema_index, lu_view name,
                                size_t *prop_index) {
  if (prop_index == NULL) return LU_ERR_INVALID_ARG;
  if (schema_index >= lu_validation_schema_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_schema_row *s = &lu_validation_schema_rows[schema_index];
  size_t lo = 0, hi = s->property_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const size_t i = s->property_first + mid;
    if (i >= lu_validation_property_row_count) return LU_ERR_INTERNAL;
    const int cmp = arena_cmp(lu_validation_property_rows[i].name, name);
    if (cmp == 0) {
      *prop_index = mid;
      return LU_OK;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return LU_ERR_NOT_FOUND;
}

int lu_validation_property_token(size_t schema_index, size_t prop_index,
                                 size_t token_index, lu_view *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  if (schema_index >= lu_validation_schema_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_schema_row *s = &lu_validation_schema_rows[schema_index];
  if (prop_index >= s->property_count) return LU_ERR_INVALID_ARG;
  const size_t i = s->property_first + prop_index;
  if (i >= lu_validation_property_row_count) return LU_ERR_INTERNAL;
  const lu_validation_property_row *r = &lu_validation_property_rows[i];
  if (token_index >= r->token_count) return LU_ERR_INVALID_ARG;
  const size_t t = r->token_first + token_index;
  if (t >= lu_validation_token_offset_count) return LU_ERR_INTERNAL;
  *out = arena_view(lu_validation_token_offsets[t]);
  return LU_OK;
}

/* --- shader lookups ------------------------------------------------------ */

int lu_validation_shader_count(size_t *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  *out = lu_validation_shader_row_count;
  return LU_OK;
}

/* Shaders are sorted by (identifier, source_type). */
int lu_validation_shader_find(lu_view identifier, lu_view source_type,
                              size_t *index) {
  if (index == NULL) return LU_ERR_INVALID_ARG;
  size_t lo = 0, hi = lu_validation_shader_row_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const lu_validation_shader_row *r = &lu_validation_shader_rows[mid];
    const int by_id = arena_cmp(r->identifier, identifier);
    if (by_id == 0) {
      const int by_type = arena_cmp(r->source_type, source_type);
      if (by_type == 0) {
        *index = mid;
        return LU_OK;
      }
      if (by_type > 0) {
        hi = mid;
        continue;
      }
      lo = mid + 1;
      continue;
    }
    if (by_id < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return LU_ERR_NOT_FOUND;
}

int lu_validation_shader_at(size_t index, lu_validation_shader_view *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  if (index >= lu_validation_shader_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_shader_row *r = &lu_validation_shader_rows[index];
  memset(out, 0, sizeof(*out));
  out->identifier = arena_view(r->identifier);
  out->source_type = arena_view(r->source_type);
  out->source = arena_view(r->source);
  out->input_count = r->input_count;
  out->output_count = r->output_count;
  return LU_OK;
}

int lu_validation_shader_port(size_t index, int is_output, size_t port_index,
                              lu_view *name, lu_view *type) {
  if (name == NULL || type == NULL) return LU_ERR_INVALID_ARG;
  if (index >= lu_validation_shader_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_shader_row *r = &lu_validation_shader_rows[index];
  const size_t count = is_output ? r->output_count : r->input_count;
  if (port_index >= count) return LU_ERR_INVALID_ARG;
  const size_t i = (is_output ? r->output_first : r->input_first) + port_index;
  if (i >= lu_validation_shader_port_row_count) return LU_ERR_INTERNAL;
  *name = arena_view(lu_validation_shader_port_rows[i].name);
  *type = arena_view(lu_validation_shader_port_rows[i].type);
  return LU_OK;
}

/* --- checker rule reference errors --------------------------------------- */

int lu_validation_ref_error_count(size_t *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  *out = lu_validation_ref_error_row_count;
  return LU_OK;
}

int lu_validation_ref_error_find(lu_view rule, size_t *index, size_t *count) {
  if (index == NULL || count == NULL) return LU_ERR_INVALID_ARG;
  size_t lo = 0, hi = lu_validation_ref_error_row_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = arena_cmp(lu_validation_ref_error_rows[mid].rule, rule);
    if (cmp == 0) {
      *index = mid;
      *count = lu_validation_ref_error_rows[mid].count;
      return LU_OK;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  *count = 0;
  return LU_ERR_NOT_FOUND;
}

int lu_validation_ref_error_item(size_t rule_index, size_t item, lu_view *out) {
  if (out == NULL) return LU_ERR_INVALID_ARG;
  if (rule_index >= lu_validation_ref_error_row_count) return LU_ERR_INVALID_ARG;
  const lu_validation_ref_error_row *r = &lu_validation_ref_error_rows[rule_index];
  if (item >= r->count) return LU_ERR_INVALID_ARG;
  const size_t i = r->first + item;
  if (i >= lu_validation_ref_error_offset_count) return LU_ERR_INTERNAL;
  *out = arena_view(lu_validation_ref_error_offsets[i]);
  return LU_OK;
}
