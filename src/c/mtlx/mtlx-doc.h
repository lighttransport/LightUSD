/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * Document internals shared by mtlx-doc.c, mtlx-units.c, mtlx-assign.c and the
 * C++ file front end (src/core/mtlx-file.cc). */
#ifndef LIGHTUSD_C_MTLX_DOC_H_
#define LIGHTUSD_C_MTLX_DOC_H_

#include "c/lightusd-mtlx.h"
#include "mtlx-internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LU_MTLX_DOC_CLASSES 5

typedef struct lu_mtlx_elem_list {
  const lu_mtlx_elem **v;
  size_t n, cap;
} lu_mtlx_elem_list;

struct lu_mtlx_document {
  int parsed;
  lu_mtlx_elem root; /* XMLNode-normalised (lu_mtlx_xmlnode_normalize) */
  lu_mtlx_sb error, warning;
  lu_mtlx_elem_list classes[LU_MTLX_DOC_CLASSES]; /* indexed by lu_mtlx_class */
};

/* error = prefix + msg */
void lu_mtlx_doc_set_error(lu_mtlx_document *d, const char *prefix, const char *msg);
/* ParseRoot(MtlxElemToXMLNode(root)); consumes root. */
int lu_mtlx_doc_adopt_root(lu_mtlx_document *d, lu_mtlx_elem *root);

#ifdef __cplusplus
}
#endif
#endif
