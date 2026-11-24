/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * Declarations shared only between the version-upgrade TUs
 * (mtlx-upgrade-legacy.c, mtlx-upgrade-channels.c, mtlx-upgrade-shader.c).
 * Port of the upgrade ladder of the former C++ src/core/mtlx-dom.cc.
 * Functions returning int return 1 on success, 0 on allocation failure.
 */
#ifndef LIGHTUSD_C_MTLX_UPGRADE_H_
#define LIGHTUSD_C_MTLX_UPGRADE_H_

#include <string.h>

#include "mtlx-internal.h"

#define LU_UPG_TAG_IS(e, t) (strcmp((e)->tag, (t)) == 0)
#define LU_UPG_NAME_IS(e, n) (strcmp(lu_mtlx_name(e), (n)) == 0)
/* AttrOr(key) == v */
#define LU_UPG_ATTR_IS(e, k, v) (strcmp(lu_mtlx_attr_or((e), (k), ""), (v)) == 0)

/* ---- common helpers (mtlx-upgrade-legacy.c) ---------------------------- */

/* IsMtlxNodeElem (mtlx-resolve.cc); local copy so the upgrade TUs do not
 * depend on mtlx-library.c. */
int lu_mtlx_upg_is_node(const lu_mtlx_elem *e);
/* Any child whose Name() == name. */
int lu_mtlx_upg_has_child_named(const lu_mtlx_elem *e, const char *name);
/* First child with tag "input" and AttrOr("name") == name, or NULL. */
lu_mtlx_elem *lu_mtlx_upg_find_input(lu_mtlx_elem *e, const char *name);
/* Append a new empty child with `tag`; NULL on allocation failure. The
 * returned pointer is invalidated by the next insertion into `parent`. */
lu_mtlx_elem *lu_mtlx_upg_push_new(lu_mtlx_elem *parent, const char *tag);
/* Append a deep copy of `src` (src must not live in parent->children). */
lu_mtlx_elem *lu_mtlx_upg_push_copy(lu_mtlx_elem *parent, const lu_mtlx_elem *src);
/* Replace e's children with the children of `from` (from is re-inited). */
void lu_mtlx_upg_take_children(lu_mtlx_elem *e, lu_mtlx_elem *from);
/* IncrementMtlxName. */
int lu_mtlx_upg_increment_name(const char *name, lu_mtlx_sb *out);
/* GetMtlxShaderNodeDef: borrowed pointer into root->children or NULL. */
const lu_mtlx_elem *lu_mtlx_upg_shader_nodedef(const lu_mtlx_elem *root,
                                               const lu_mtlx_elem *shader_ref);

/* Pre-order walk in the order of the former recursive C++ walkers: `visit`
 * is called for root (parent NULL) and then for each child of a visited
 * element whose visit returned 1. A visit may mutate its element and the
 * element's subtree, never its ancestors' child arrays. Returns 0 when the
 * walk stack could not be allocated. */
typedef int (*lu_mtlx_upg_visit_fn)(lu_mtlx_elem *e, lu_mtlx_elem *parent, void *user);
int lu_mtlx_upg_preorder(lu_mtlx_elem *root, lu_mtlx_upg_visit_fn visit, void *user);

/* ---- channel steps (mtlx-upgrade-channels.c) --------------------------- */

int lu_mtlx_upg_color2_channel_aliases(lu_mtlx_elem *root); /* UpgradeColor2ChannelAliases */
int lu_mtlx_upg_channels(lu_mtlx_elem *root);               /* UpgradeMtlxChannels */

/* ---- shader / attribute steps (mtlx-upgrade-shader.c) ------------------ */

int lu_mtlx_upg_shader_refs(lu_mtlx_elem *root);              /* UpgradeMtlxShaderRefs */
int lu_mtlx_upg_thin_film_layers(lu_mtlx_elem *root);         /* UpgradeMtlxThinFilmLayers */
int lu_mtlx_upg_legacy_attributes(lu_mtlx_elem *root, int from_122);
int lu_mtlx_upg_assign_and_constant(lu_mtlx_elem *root, int from_123, int from_125);
int lu_mtlx_upg_interface_name_conflicts(lu_mtlx_elem *root);
int lu_mtlx_upg_shader_name_and_graph_name(lu_mtlx_elem *root, int from_123, int from_124);

#endif /* LIGHTUSD_C_MTLX_UPGRADE_H_ */
