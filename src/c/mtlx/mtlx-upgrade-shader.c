/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX upgrade: shaderref -> material nodes, thin-film layers, interface
 * name conflicts, assign/constant renames, shadername/graphname and the
 * attribute-level legacy upgrades (C11 port of src/core/mtlx-dom.cc).
 */
#include "mtlx-upgrade.h"

#include <stdlib.h>
#include <string.h>

/* ---- UpgradeMtlxShaderRefs ------------------------------------------------ */

/* std::set<std::string> semantics on a string vector. */
static int set_insert(lu_mtlx_strv *s, const char *v) {
  return lu_mtlx_strv_contains(s, v) ? 1 : lu_mtlx_strv_push(s, v);
}
static void set_erase(lu_mtlx_strv *s, const char *v) {
  for (size_t i = 0; i < s->n; i++) {
    if (strcmp(s->v[i], v) == 0) {
      free(s->v[i]);
      s->v[i] = s->v[--s->n];
      return;
    }
  }
}

/* The `valid_name` lambda: increment until unused. */
static int valid_name(const lu_mtlx_strv *names, const char *base, lu_mtlx_sb *out) {
  lu_mtlx_sb tmp;
  lu_mtlx_sb_init(&tmp);
  lu_mtlx_sb_set(out, base);
  while (!out->oom && lu_mtlx_strv_contains(names, lu_mtlx_sb_str(out))) {
    if (!lu_mtlx_upg_increment_name(lu_mtlx_sb_str(out), &tmp)) break;
    lu_mtlx_sb_set(out, lu_mtlx_sb_str(&tmp));
  }
  const int ok = !out->oom && !tmp.oom;
  lu_mtlx_sb_free(&tmp);
  return ok;
}

/* shader_type / category of one shaderref. */
static int shader_type_of(const lu_mtlx_elem *root, const lu_mtlx_elem *sr, const char **type,
                          const char **category) {
  const lu_mtlx_elem *nd = lu_mtlx_upg_shader_nodedef(root, sr);
  *type = "surfaceshader";
  *category = lu_mtlx_attr_or(sr, "node", "");
  if (!nd) return 1;
  *type = lu_mtlx_attr_or(nd, "type", "");
  if (!(*type)[0]) {
    for (size_t k = 0; k < nd->nchildren; k++)
      if (LU_UPG_TAG_IS(&nd->children[k], "output")) {
        *type = lu_mtlx_attr_or(&nd->children[k], "type", "");
        break;
      }
  }
  if (!(*type)[0]) *type = "color3";
  *category = lu_mtlx_attr_or(nd, "node", "");
  return 1;
}

/* One shaderref -> a shader node in `nodes` and maybe an input in `inputs`. */
static int convert_shader_ref(const lu_mtlx_elem *root, const lu_mtlx_elem *sr,
                              lu_mtlx_strv *names, lu_mtlx_elem *nodes, lu_mtlx_elem *inputs) {
  const char *type, *category;
  shader_type_of(root, sr, &type, &category);
  lu_mtlx_sb sname;
  lu_mtlx_sb_init(&sname);
  int ok = valid_name(names, lu_mtlx_name(sr), &sname) &&
           set_insert(names, lu_mtlx_sb_str(&sname));
  const char *shader_name = lu_mtlx_sb_str(&sname);
  lu_mtlx_elem *sh = ok ? lu_mtlx_upg_push_new(nodes, category) : NULL;
  ok = sh && lu_mtlx_attr_set(sh, "name", shader_name) && lu_mtlx_attr_set(sh, "type", type);
  static const char *const kAttrs[] = {"nodedef", "target", "version"};
  for (size_t k = 0; ok && k < 3; k++) {
    const char *v = lu_mtlx_attr_or(sr, kAttrs[k], "");
    if (v[0]) ok = lu_mtlx_attr_set(sh, kAttrs[k], v);
  }
  for (size_t k = 0; ok && k < sr->nchildren; k++) {
    const lu_mtlx_elem *b = &sr->children[k];
    const char *tag = LU_UPG_TAG_IS(b, "bindinput") || LU_UPG_TAG_IS(b, "bindparam") ? "input"
                      : LU_UPG_TAG_IS(b, "bindtoken")                                ? "token"
                                                                                     : NULL;
    if (!tag) continue;
    lu_mtlx_elem *c = lu_mtlx_upg_push_copy(sh, b);
    ok = c && lu_mtlx_elem_set_tag(c, tag);
  }
  if (ok && !lu_mtlx_upg_has_child_named(inputs, type)) {
    lu_mtlx_elem *in = lu_mtlx_upg_push_new(inputs, "input");
    ok = in && lu_mtlx_attr_set(in, "name", type) && lu_mtlx_attr_set(in, "type", type) &&
         lu_mtlx_attr_set(in, "nodename", shader_name);
  }
  lu_mtlx_sb_free(&sname);
  return ok;
}

/* One material of UpgradeMtlxShaderRefs. *keep is cleared when the material
 * leaves its position (moved to `migrated` or dropped). */
static int convert_material(lu_mtlx_elem *root, size_t index, lu_mtlx_strv *names,
                            lu_mtlx_elem *migrated) {
  lu_mtlx_elem nodes, inputs;
  lu_mtlx_elem_init(&nodes);
  lu_mtlx_elem_init(&inputs);
  int ok = 1;
  const lu_mtlx_elem *child = &root->children[index];
  for (size_t k = 0; ok && k < child->nchildren; k++)
    if (LU_UPG_TAG_IS(&child->children[k], "shaderref"))
      ok = convert_shader_ref(root, &child->children[k], names, &nodes, &inputs);
  if (ok) set_erase(names, lu_mtlx_name(child));
  if (ok && nodes.nchildren) {
    for (size_t k = 0; ok && k < nodes.nchildren; k++)
      ok = lu_mtlx_child_push(migrated, &nodes.children[k]) != NULL;
    lu_mtlx_elem *mat = &root->children[index];
    const int volume = inputs.nchildren &&
                       LU_UPG_ATTR_IS(&inputs.children[0], "type", "volumeshader");
    ok = ok && lu_mtlx_elem_set_tag(mat, volume ? "volumematerial" : "surfacematerial") &&
         lu_mtlx_attr_set(mat, "type", "material");
    if (ok) {
      lu_mtlx_upg_take_children(mat, &inputs);
      ok = set_insert(names, lu_mtlx_name(mat));
    }
    /* std::move leaves an empty element behind (no tag, attrs, children). */
    lu_mtlx_elem moved;
    lu_mtlx_elem_move(&moved, mat);
    ok = ok && lu_mtlx_child_push(migrated, &moved) != NULL;
  }
  lu_mtlx_elem_free(&nodes);
  lu_mtlx_elem_free(&inputs);
  return ok;
}

/* UpgradeMtlxShaderRefs. Non-material children keep their place (the C++
 * copies them into `upgraded`); converted materials and their shader nodes
 * follow in `migrated` order; materials without shaderrefs are dropped. */
int lu_mtlx_upg_shader_refs(lu_mtlx_elem *root) {
  if (!LU_UPG_TAG_IS(root, "materialx")) return 1;
  lu_mtlx_strv names;
  lu_mtlx_strv_init(&names);
  int ok = 1;
  for (size_t k = 0; ok && k < root->nchildren; k++)
    ok = set_insert(&names, lu_mtlx_name(&root->children[k]));
  lu_mtlx_elem migrated, upgraded;
  lu_mtlx_elem_init(&migrated);
  lu_mtlx_elem_init(&upgraded);
  const size_t count = root->nchildren;
  unsigned char *is_material = count ? (unsigned char *)calloc(count, 1) : NULL;
  if (count && !is_material) ok = 0;
  for (size_t k = 0; ok && k < count; k++) {
    if (!LU_UPG_TAG_IS(&root->children[k], "material")) continue;
    is_material[k] = 1;
    ok = convert_material(root, k, &names, &migrated);
  }
  if (ok) {
    for (size_t k = 0; ok && k < count; k++)
      if (!is_material[k]) ok = lu_mtlx_child_push(&upgraded, &root->children[k]) != NULL;
    for (size_t k = 0; ok && k < migrated.nchildren; k++)
      ok = lu_mtlx_child_push(&upgraded, &migrated.children[k]) != NULL;
    if (ok) lu_mtlx_upg_take_children(root, &upgraded);
  }
  free(is_material);
  lu_mtlx_elem_free(&migrated);
  lu_mtlx_elem_free(&upgraded);
  lu_mtlx_strv_free(&names);
  return ok;
}

/* ---- UpgradeMtlxThinFilmLayers -------------------------------------------- */

/* The `find_node` lambda. */
static lu_mtlx_elem *find_node(lu_mtlx_elem *scope, const char *name) {
  for (size_t k = 0; k < scope->nchildren; k++) {
    lu_mtlx_elem *c = &scope->children[k];
    if (lu_mtlx_upg_is_node(c) && LU_UPG_NAME_IS(c, name)) return c;
  }
  return NULL;
}

/* Copy the thin-film inputs of `top` onto a supported BSDF `node`. */
static int apply_thin_film(lu_mtlx_elem *node, const lu_mtlx_elem *top) {
  static const char *const kBind[][2] = {{"thickness", "thinfilm_thickness"},
                                         {"ior", "thinfilm_ior"}};
  for (size_t b = 0; b < 2; b++) {
    const lu_mtlx_elem *source = NULL;
    for (size_t k = 0; k < top->nchildren && !source; k++)
      if (LU_UPG_TAG_IS(&top->children[k], "input") &&
          LU_UPG_ATTR_IS(&top->children[k], "name", kBind[b][0]))
        source = &top->children[k];
    if (!source) continue;
    lu_mtlx_elem rep;
    if (!lu_mtlx_elem_copy(&rep, source)) return 0;
    if (!lu_mtlx_attr_set(&rep, "name", kBind[b][1])) { lu_mtlx_elem_free(&rep); return 0; }
    lu_mtlx_elem *dest = lu_mtlx_upg_find_input(node, kBind[b][1]);
    if (dest) {
      lu_mtlx_elem_free(dest);
      lu_mtlx_elem_move(dest, &rep);
    } else if (!lu_mtlx_child_push(node, &rep)) {
      return 0;
    }
  }
  return 1;
}

static int is_thin_film_bsdf(const lu_mtlx_elem *n) {
  return LU_UPG_TAG_IS(n, "dielectric_bsdf") || LU_UPG_TAG_IS(n, "conductor_bsdf") ||
         LU_UPG_TAG_IS(n, "generalized_schlick_bsdf");
}

/* The pending/visited walk from the layer's base node. */
static int thin_film_walk(lu_mtlx_elem *scope, const lu_mtlx_elem *top, const char *base_name) {
  lu_mtlx_strv pending, visited;
  lu_mtlx_strv_init(&pending);
  lu_mtlx_strv_init(&visited);
  int ok = lu_mtlx_strv_push(&pending, base_name);
  while (ok && pending.n) {
    char *name = pending.v[--pending.n];
    if (lu_mtlx_strv_contains(&visited, name)) {
      free(name);
      continue;
    }
    if (!lu_mtlx_strv_push(&visited, name)) {
      free(name);
      ok = 0;
      break;
    }
    lu_mtlx_elem *node = find_node(scope, name);
    free(name);
    if (!node) continue;
    const lu_mtlx_elem *scatter = lu_mtlx_upg_find_input(node, "scatter_mode");
    if (is_thin_film_bsdf(node) && (!scatter || !LU_UPG_ATTR_IS(scatter, "value", "T")))
      ok = apply_thin_film(node, top);
    for (size_t k = 0; ok && k < node->nchildren; k++) {
      const lu_mtlx_elem *c = &node->children[k];
      const char *nn = lu_mtlx_attr_or(c, "nodename", "");
      if ((LU_UPG_TAG_IS(c, "input") || LU_UPG_TAG_IS(c, "output")) && nn[0])
        ok = lu_mtlx_strv_push(&pending, nn);
    }
  }
  lu_mtlx_strv_free(&pending);
  lu_mtlx_strv_free(&visited);
  return ok;
}

/* Redirect every connection to the layer onto its base. */
static int thin_film_redirect(lu_mtlx_elem *scope, const char *layer_name, const char *base_name) {
  int ok = 1;
  for (size_t k = 0; k < scope->nchildren; k++) {
    lu_mtlx_elem *child = &scope->children[k];
    if (lu_mtlx_upg_is_node(child)) {
      for (size_t p = 0; p < child->nchildren; p++) {
        lu_mtlx_elem *port = &child->children[p];
        if ((LU_UPG_TAG_IS(port, "input") || LU_UPG_TAG_IS(port, "output")) &&
            LU_UPG_ATTR_IS(port, "nodename", layer_name))
          ok &= lu_mtlx_attr_set(port, "nodename", base_name);
      }
    } else if (LU_UPG_TAG_IS(child, "output") && LU_UPG_ATTR_IS(child, "nodename", layer_name)) {
      ok &= lu_mtlx_attr_set(child, "nodename", base_name);
    }
  }
  return ok;
}

/* One layer of UpgradeMtlxThinFilmLayers. */
static int thin_film_layer(lu_mtlx_elem *scope, const char *layer_name, lu_mtlx_strv *removed) {
  lu_mtlx_elem *layer = find_node(scope, layer_name);
  lu_mtlx_elem *top_input = layer ? lu_mtlx_upg_find_input(layer, "top") : NULL;
  lu_mtlx_elem *base_input = layer ? lu_mtlx_upg_find_input(layer, "base") : NULL;
  if (!top_input || !base_input) return 1;
  char *top_name = lu_mtlx_strdup(lu_mtlx_attr_or(top_input, "nodename", ""));
  char *base_name = lu_mtlx_strdup(lu_mtlx_attr_or(base_input, "nodename", ""));
  int ok = top_name && base_name;
  lu_mtlx_elem *top = ok ? find_node(scope, top_name) : NULL;
  if (ok && top && LU_UPG_TAG_IS(top, "thin_film_bsdf") && base_name[0]) {
    lu_mtlx_elem top_copy;
    ok = lu_mtlx_elem_copy(&top_copy, top);
    if (ok) {
      ok = thin_film_walk(scope, &top_copy, base_name) &&
           thin_film_redirect(scope, layer_name, base_name) &&
           lu_mtlx_strv_push(removed, layer_name) && lu_mtlx_strv_push(removed, top_name);
      lu_mtlx_elem_free(&top_copy);
    }
  }
  free(top_name);
  free(base_name);
  return ok;
}

/* UpgradeMtlxThinFilmLayers: one scope (root, nodegraphs, materials). */
static int thin_film_visit(lu_mtlx_elem *scope, lu_mtlx_elem *parent, void *user) {
  int *ok = (int *)user;
  if (parent && !LU_UPG_TAG_IS(scope, "nodegraph") && !LU_UPG_TAG_IS(scope, "material"))
    return 0;
  lu_mtlx_strv layers, removed;
  lu_mtlx_strv_init(&layers);
  lu_mtlx_strv_init(&removed);
  for (size_t k = 0; *ok && k < scope->nchildren; k++)
    if (LU_UPG_TAG_IS(&scope->children[k], "layer"))
      *ok = lu_mtlx_strv_push(&layers, lu_mtlx_name(&scope->children[k]));
  for (size_t i = 0; *ok && i < layers.n; i++) *ok = thin_film_layer(scope, layers.v[i], &removed);
  for (size_t k = scope->nchildren; *ok && k-- > 0;)
    if (lu_mtlx_strv_contains(&removed, lu_mtlx_name(&scope->children[k])))
      lu_mtlx_child_erase(scope, k);
  lu_mtlx_strv_free(&layers);
  lu_mtlx_strv_free(&removed);
  return 1;
}

int lu_mtlx_upg_thin_film_layers(lu_mtlx_elem *root) {
  int ok = 1;
  return lu_mtlx_upg_preorder(root, thin_film_visit, &ok) && ok;
}

/* ---- UpgradeMtlxLegacyAttributes / AssignAndConstant / ShaderName ---------- */

typedef struct flags_ctx {
  int a, b, ok;
} flags_ctx;

/* UpgradeMtlxLegacyAttributes: one element. */
static int legacy_attr_visit(lu_mtlx_elem *elem, lu_mtlx_elem *parent, void *user) {
  flags_ctx *x = (flags_ctx *)user;
  (void)parent;
  const char *t = lu_mtlx_attr_or(elem, "type", "");
  const int is_vector = !strcmp(t, "vector"), is_matrix = !strcmp(t, "matrix");
  if (x->a && is_vector) x->ok &= lu_mtlx_attr_set(elem, "type", "vector3");
  if (is_matrix) x->ok &= lu_mtlx_attr_set(elem, "type", "matrix44");
  if (lu_mtlx_get_attr(elem, "default") && !lu_mtlx_get_attr(elem, "value")) {
    char *v = lu_mtlx_strdup(lu_mtlx_get_attr(elem, "default"));
    if (!v) { x->ok = 0; return 1; }
    lu_mtlx_attr_erase(elem, "default");
    x->ok &= lu_mtlx_attr_set(elem, "value", v);
    free(v);
  }
  return 1;
}

int lu_mtlx_upg_legacy_attributes(lu_mtlx_elem *root, int from_122) {
  flags_ctx x = {from_122, 0, 1};
  return lu_mtlx_upg_preorder(root, legacy_attr_visit, &x) && x.ok;
}

/* UpgradeMtlxAssignAndConstant: one element. */
static int assign_visit(lu_mtlx_elem *elem, lu_mtlx_elem *parent, void *user) {
  flags_ctx *x = (flags_ctx *)user;
  (void)parent;
  if (x->a && LU_UPG_TAG_IS(elem, "materialx")) {
    for (size_t k = 0; k < elem->nchildren; k++)
      if (LU_UPG_TAG_IS(&elem->children[k], "assign"))
        x->ok &= lu_mtlx_elem_set_tag(&elem->children[k], "materialassign");
  }
  if (x->b && LU_UPG_TAG_IS(elem, "constant")) {
    for (size_t k = 0; k < elem->nchildren; k++)
      if (LU_UPG_NAME_IS(&elem->children[k], "color"))
        x->ok &= lu_mtlx_attr_set(&elem->children[k], "name", "value");
  }
  return 1;
}

int lu_mtlx_upg_assign_and_constant(lu_mtlx_elem *root, int from_123, int from_125) {
  flags_ctx x = {from_123, from_125, 1};
  return lu_mtlx_upg_preorder(root, assign_visit, &x) && x.ok;
}

/* Move attribute `from` to `to` (SetAttr(to, value) then erase from). */
static int rename_attr(lu_mtlx_elem *e, const char *from, const char *to) {
  const int ok = lu_mtlx_attr_set(e, to, lu_mtlx_attr_or(e, from, ""));
  lu_mtlx_attr_erase(e, from);
  return ok;
}

/* UpgradeMtlxShaderNameAndGraphName: one element. */
static int shader_name_visit(lu_mtlx_elem *elem, lu_mtlx_elem *parent, void *user) {
  flags_ctx *x = (flags_ctx *)user;
  (void)parent;
  if (x->a && LU_UPG_TAG_IS(elem, "shader") && lu_mtlx_get_attr(elem, "shadername"))
    x->ok &= rename_attr(elem, "shadername", "node");
  if (x->b && LU_UPG_TAG_IS(elem, "input") && lu_mtlx_get_attr(elem, "graphname"))
    x->ok &= rename_attr(elem, "graphname", "opgraph");
  return 1;
}

int lu_mtlx_upg_shader_name_and_graph_name(lu_mtlx_elem *root, int from_123, int from_124) {
  flags_ctx x = {from_123, from_124, 1};
  return lu_mtlx_upg_preorder(root, shader_name_visit, &x) && x.ok;
}

/* ---- UpgradeMtlxInterfaceNameConflicts ------------------------------------ */

/* Rename the node `iface` of `graph` and redirect connections to it. */
static int rename_conflict(lu_mtlx_elem *graph, const char *iface) {
  lu_mtlx_elem *node = find_node(graph, iface);
  if (!node) return 1;
  lu_mtlx_sb fresh, tmp;
  lu_mtlx_sb_init(&fresh);
  lu_mtlx_sb_init(&tmp);
  lu_mtlx_sb_set(&fresh, iface);
  int ok = 1;
  do {
    ok = lu_mtlx_upg_increment_name(lu_mtlx_sb_str(&fresh), &tmp);
    lu_mtlx_sb_set(&fresh, lu_mtlx_sb_str(&tmp));
  } while (ok && !fresh.oom && lu_mtlx_upg_has_child_named(graph, lu_mtlx_sb_str(&fresh)));
  const char *f = lu_mtlx_sb_str(&fresh);
  for (size_t k = 0; ok && k < graph->nchildren; k++) {
    lu_mtlx_elem *child = &graph->children[k];
    if (LU_UPG_TAG_IS(child, "output") && LU_UPG_ATTR_IS(child, "nodename", iface))
      ok &= lu_mtlx_attr_set(child, "nodename", f);
    if (!lu_mtlx_upg_is_node(child)) continue;
    for (size_t p = 0; p < child->nchildren; p++)
      if (LU_UPG_ATTR_IS(&child->children[p], "nodename", iface))
        ok &= lu_mtlx_attr_set(&child->children[p], "nodename", f);
  }
  ok = ok && !fresh.oom && lu_mtlx_attr_set(node, "name", f);
  lu_mtlx_sb_free(&fresh);
  lu_mtlx_sb_free(&tmp);
  return ok;
}

/* UpgradeMtlxInterfaceNameConflicts (root's nodegraphs only). */
int lu_mtlx_upg_interface_name_conflicts(lu_mtlx_elem *root) {
  int ok = 1;
  for (size_t g = 0; ok && g < root->nchildren; g++) {
    lu_mtlx_elem *graph = &root->children[g];
    if (!LU_UPG_TAG_IS(graph, "nodegraph")) continue;
    lu_mtlx_attr_erase(graph, "version");
    lu_mtlx_strv ifaces;
    lu_mtlx_strv_init(&ifaces);
    for (size_t k = 0; ok && k < graph->nchildren; k++) {
      const lu_mtlx_elem *node = &graph->children[k];
      if (!lu_mtlx_upg_is_node(node)) continue;
      for (size_t p = 0; ok && p < node->nchildren; p++) {
        const char *iface = lu_mtlx_get_attr(&node->children[p], "interfacename");
        if (iface && iface[0] && !lu_mtlx_strv_contains(&ifaces, iface))
          ok = lu_mtlx_strv_push(&ifaces, iface);
      }
    }
    for (size_t i = 0; ok && i < ifaces.n; i++) ok = rename_conflict(graph, ifaces.v[i]);
    lu_mtlx_strv_free(&ifaces);
  }
  return ok;
}
