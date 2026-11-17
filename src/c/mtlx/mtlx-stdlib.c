/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX standard library tables (C11 port of the static POD X-macro rows
 * and BuildStandardMtlxNodeDefs / BuildStandardMtlxGeomPropDefs of
 * src/core/mtlx-dom.cc). The rows are static const data; the elements are
 * built per call (no global mutable state).
 */
#include <stdlib.h>
#include <string.h>

#include "c/mtlx/mtlx-internal.h"

typedef struct std_nodedef_row {
  const char *name, *node, *out_type;
  unsigned first, count;
} std_nodedef_row;
typedef struct std_input_row {
  const char *name, *type;
  int has_default;
  const char *default_value, *geomprop;
} std_input_row;
typedef struct std_geomprop_row {
  const char *name, *type, *geomprop;
  int has_space;
  const char *space;
  int has_index;
  const char *index;
} std_geomprop_row;

#define MTLX_STD_NODEDEF(name, node, output_type, first_input, input_count) \
  {name, node, output_type, first_input, input_count},
#define MTLX_STD_INPUT(name, type, has_default, default_value, geomprop)
static const std_nodedef_row kStdNodeDefRows[] = {
#include "../../external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
};
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
#define MTLX_STD_NODEDEF(name, node, output_type, first_input, input_count)
#define MTLX_STD_INPUT(name, type, has_default, default_value, geomprop) \
  {name, type, has_default, default_value, geomprop},
static const std_input_row kStdInputRows[] = {
#include "../../external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
};
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
#define MTLX_STD_GEOMPROPDEF(nm, ty, category, has_space, space, has_index, index) \
  {nm, ty, category, has_space, space, has_index, index},
static const std_geomprop_row kStdGeomPropRows[] = {
#include "../../core/generated/mtlx-stdlib-geomprops.inc"
};
#undef MTLX_STD_GEOMPROPDEF

static int add_child(lu_mtlx_elem *parent, lu_mtlx_elem *c) {
  return lu_mtlx_child_push(parent, c) != NULL;
}

/* One element of BuildStandardMtlxNodeDefs (out uninitialised). */
static int build_nodedef(const std_nodedef_row *row, lu_mtlx_elem *def) {
  lu_mtlx_elem_init(def);
  int ok = lu_mtlx_elem_set_tag(def, "nodedef") && lu_mtlx_attr_set(def, "name", row->name) &&
           lu_mtlx_attr_set(def, "node", row->node) &&
           lu_mtlx_attr_set(def, "type", row->out_type);
  lu_mtlx_elem output;
  lu_mtlx_elem_init(&output);
  ok = ok && lu_mtlx_elem_set_tag(&output, "output") && lu_mtlx_attr_set(&output, "name", "out") &&
       lu_mtlx_attr_set(&output, "type", row->out_type) && add_child(def, &output);
  lu_mtlx_elem_free(&output);
  for (unsigned k = 0; ok && k < row->count; ++k) {
    const std_input_row *in = &kStdInputRows[row->first + k];
    lu_mtlx_elem input;
    lu_mtlx_elem_init(&input);
    ok = lu_mtlx_elem_set_tag(&input, "input") && lu_mtlx_attr_set(&input, "name", in->name) &&
         lu_mtlx_attr_set(&input, "type", in->type);
    if (ok && in->has_default) ok = lu_mtlx_attr_set(&input, "value", in->default_value);
    if (ok && in->geomprop[0]) ok = lu_mtlx_attr_set(&input, "defaultgeomprop", in->geomprop);
    ok = ok && add_child(def, &input);
    lu_mtlx_elem_free(&input);
  }
  if (!ok) lu_mtlx_elem_free(def);
  return ok;
}

/* `for (def : StandardMtlxNodeDefs()) library.AddNodeDef(def, {})` */
int lu_mtlx_library_add_stdlib(lu_mtlx_library *l) {
  const size_t n = sizeof(kStdNodeDefRows) / sizeof(kStdNodeDefRows[0]);
  for (size_t i = 0; i < n; i++) {
    lu_mtlx_elem def;
    if (!build_nodedef(&kStdNodeDefRows[i], &def)) {
      l->oom = 1;
      return 0;
    }
    const int ok = lu_mtlx_library_add_nodedef(l, &def, NULL);
    lu_mtlx_elem_free(&def);
    if (!ok) return 0;
  }
  return 1;
}

size_t lu_mtlx_stdlib_geomprop_count(void) {
  return sizeof(kStdGeomPropRows) / sizeof(kStdGeomPropRows[0]);
}

/* StandardMtlxGeomPropDefs()[i] (BuildStandardMtlxGeomPropDefs). */
int lu_mtlx_stdlib_geomprop_elem(size_t i, lu_mtlx_elem *out) {
  lu_mtlx_elem_init(out);
  if (i >= lu_mtlx_stdlib_geomprop_count()) return 0;
  const std_geomprop_row *row = &kStdGeomPropRows[i];
  int ok = lu_mtlx_elem_set_tag(out, "geompropdef") && lu_mtlx_attr_set(out, "name", row->name) &&
           lu_mtlx_attr_set(out, "type", row->type) &&
           lu_mtlx_attr_set(out, "geomprop", row->geomprop);
  if (ok && row->has_space) ok = lu_mtlx_attr_set(out, "space", row->space);
  if (ok && row->has_index) ok = lu_mtlx_attr_set(out, "index", row->index);
  if (!ok) lu_mtlx_elem_free(out);
  return ok;
}
