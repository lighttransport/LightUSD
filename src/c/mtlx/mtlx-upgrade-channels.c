/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026-Present Light Transport Entertainment Inc.
 *
 * MaterialX upgrade: color2 channel aliases (1.37 -> 1.38) and the
 * `channels` / swizzle removal of 1.38 -> 1.39 (C11 port of the anonymous
 * namespace helpers of src/core/mtlx-dom.cc).
 */
#include "mtlx-upgrade.h"

#include <stdlib.h>
#include <string.h>

/* ---- UpgradeColor2ChannelAliases ----------------------------------------- */

/* RemapColor2Channels, applied to attribute `key` of `e` when present. */
static int remap_color2_attr(lu_mtlx_elem *e, const char *key) {
  const char *v = lu_mtlx_get_attr(e, key);
  if (!v) return 1;
  char *r = lu_mtlx_strdup(v);
  if (!r) return 0;
  for (char *c = r; *c; c++) {
    if (*c == 'r') *c = 'x';
    else if (*c == 'a') *c = 'y';
  }
  const int ok = lu_mtlx_attr_set(e, key, r);
  free(r);
  return ok;
}

/* One source node of UpgradeColor2ChannelAliases over scope's children. */
static int color2_source(lu_mtlx_elem *scope, const char *source_name) {
  int ok = 1;
  for (size_t k = 0; k < scope->nchildren; k++) {
    lu_mtlx_elem *child = &scope->children[k];
    if (lu_mtlx_upg_is_node(child)) {
      for (size_t p = 0; p < child->nchildren; p++) {
        lu_mtlx_elem *port = &child->children[p];
        if (!LU_UPG_TAG_IS(port, "input") && !LU_UPG_TAG_IS(port, "output")) continue;
        if (!LU_UPG_ATTR_IS(port, "nodename", source_name)) continue;
        ok &= remap_color2_attr(port, "channels");
        ok &= remap_color2_attr(port, "output");
      }
    }
    if (!LU_UPG_NAME_IS(child, source_name)) continue;
    for (size_t p = 0; p < child->nchildren; p++) {
      lu_mtlx_elem *input = &child->children[p];
      if (LU_UPG_TAG_IS(input, "input") && LU_UPG_NAME_IS(input, "channels"))
        ok &= remap_color2_attr(input, "value");
    }
  }
  return ok;
}

/* UpgradeColor2ChannelAliases: one scope (then every child, pre-order). */
static int color2_visit(lu_mtlx_elem *scope, lu_mtlx_elem *parent, void *user) {
  int *ok = (int *)user;
  (void)parent;
  lu_mtlx_strv names;
  lu_mtlx_strv_init(&names);
  for (size_t k = 0; k < scope->nchildren; k++) {
    const lu_mtlx_elem *c = &scope->children[k];
    if (lu_mtlx_upg_is_node(c) && LU_UPG_ATTR_IS(c, "type", "color2"))
      *ok &= lu_mtlx_strv_push(&names, lu_mtlx_name(c));
  }
  for (size_t i = 0; i < names.n; i++) *ok &= color2_source(scope, names.v[i]);
  lu_mtlx_strv_free(&names);
  return 1;
}

int lu_mtlx_upg_color2_channel_aliases(lu_mtlx_elem *root) {
  int ok = 1;
  return lu_mtlx_upg_preorder(root, color2_visit, &ok) && ok;
}

/* ---- channel helpers ------------------------------------------------------ */

/* MtlxChannelCount. */
static size_t channel_count(const char *type) {
  if (!strcmp(type, "float")) return 1;
  if (!strcmp(type, "vector2")) return 2;
  if (!strcmp(type, "vector3") || !strcmp(type, "color3")) return 3;
  if (!strcmp(type, "vector4") || !strcmp(type, "color4")) return 4;
  return 0;
}

/* ConnectedNodeType (borrowed result; "" when unknown). */
static const char *connected_node_type(const lu_mtlx_elem *scope, const lu_mtlx_elem *port) {
  const char *source_name = lu_mtlx_attr_or(port, "nodename", "");
  if (source_name[0]) {
    for (size_t k = 0; k < scope->nchildren; k++) {
      const lu_mtlx_elem *node = &scope->children[k];
      if (!lu_mtlx_upg_is_node(node) || !LU_UPG_NAME_IS(node, source_name)) continue;
      const char *output_name = lu_mtlx_attr_or(port, "output", "");
      if (output_name[0]) {
        for (size_t o = 0; o < node->nchildren; o++) {
          const lu_mtlx_elem *out = &node->children[o];
          if (LU_UPG_TAG_IS(out, "output") && LU_UPG_NAME_IS(out, output_name))
            return lu_mtlx_attr_or(out, "type", "");
        }
      }
      return lu_mtlx_attr_or(node, "type", "");
    }
  }
  const char *iface = lu_mtlx_attr_or(port, "interfacename", "");
  if (iface[0]) {
    for (size_t k = 0; k < scope->nchildren; k++) {
      const lu_mtlx_elem *in = &scope->children[k];
      if (LU_UPG_TAG_IS(in, "input") && LU_UPG_NAME_IS(in, iface))
        return lu_mtlx_attr_or(in, "type", "");
    }
  }
  return "";
}

/* InferChannelSourceType. */
static const char *infer_source_type(const char *ch, const char *dest_type) {
  if (!strcmp(ch, "xx") || !strcmp(ch, "xxx") || !strcmp(ch, "xxxx") || !strcmp(ch, "rr") ||
      !strcmp(ch, "rrr"))
    return "float";
  if (!strcmp(ch, "xyz") || !strcmp(ch, "x") || !strcmp(ch, "y") || !strcmp(ch, "z"))
    return "vector3";
  if (!strcmp(ch, "rgba") || !strcmp(ch, "a")) return "color4";
  return !strcmp(dest_type, "color3") ? "color4" : "color3";
}

/* SplitChannelValue. */
static int split_channel_value(const char *value, lu_mtlx_strv *tokens) {
  lu_mtlx_strv_init(tokens);
  const char *start = NULL;
  for (const char *c = value;; c++) {
    const int sep = *c == ',' || *c == ';' || *c == ' ' || *c == '\t' || *c == '\r' ||
                    *c == '\n' || *c == '\0';
    if (sep && start) {
      if (!lu_mtlx_strv_push_n(tokens, start, (size_t)(c - start))) return 0;
      start = NULL;
    } else if (!sep && !start) {
      start = c;
    }
    if (!*c) break;
  }
  return 1;
}

/* MtlxChannelIndex. */
static int channel_index(char c) {
  switch (c) {
    case 'r': case 'x': return 0;
    case 'g': case 'y': return 1;
    case 'b': case 'z': return 2;
    case 'a': case 'w': return 3;
    default: return -1;
  }
}

/* MtlxChannelConstant. */
static int channel_constant(char c, const char **value) {
  if (c == '0') { if (value) *value = "0.000000"; return 1; }
  if (c == '1') { if (value) *value = "1.000000"; return 1; }
  return 0;
}

/* SwizzledConstantValue. */
static int swizzled_constant(const lu_mtlx_elem *port, const char *channels, size_t dest_count,
                             lu_mtlx_sb *out) {
  lu_mtlx_strv src;
  if (!split_channel_value(lu_mtlx_attr_or(port, "value", "0"), &src)) {
    lu_mtlx_strv_free(&src);
    return 0;
  }
  const size_t nch = strlen(channels);
  lu_mtlx_sb_clear(out);
  for (size_t i = 0; i < dest_count; ++i) {
    if (i) lu_mtlx_sb_puts(out, ", ");
    const char ch = i < nch ? channels[i] : '\0';
    const char *constant = NULL;
    if (channel_constant(ch, &constant)) {
      lu_mtlx_sb_puts(out, constant);
      continue;
    }
    const int index = channel_index(ch);
    if (index >= 0 && (size_t)index < src.n) lu_mtlx_sb_puts(out, src.v[index]);
    else if (src.n) lu_mtlx_sb_puts(out, src.v[0]);
    else lu_mtlx_sb_puts(out, "0");
  }
  lu_mtlx_strv_free(&src);
  return !out->oom;
}

/* All of the first dest_count selector characters are '0'/'1'. */
static int all_constant_channels(const char *channels, size_t dest_count) {
  const size_t nch = strlen(channels);
  for (size_t i = 0; i < dest_count; ++i)
    if (!channel_constant(i < nch ? channels[i] : '\0', NULL)) return 0;
  return 1;
}

static int has_connection(const lu_mtlx_elem *p) {
  return lu_mtlx_attr_or(p, "nodename", "")[0] || lu_mtlx_attr_or(p, "nodegraph", "")[0] ||
         lu_mtlx_attr_or(p, "interfacename", "")[0];
}

/* UniqueMtlxChildName (numbered = 0) / UniqueMtlxNumberedChildName (1). */
static int unique_child_name(const lu_mtlx_elem *scope, const lu_mtlx_elem *gen,
                             const char *base, int numbered, lu_mtlx_sb *out) {
  for (size_t suffix = numbered ? 1 : 0;; ++suffix) {
    lu_mtlx_sb_set(out, base);
    if (suffix != (numbered ? 1u : 0u)) {
      if (!numbered) lu_mtlx_sb_putc(out, '_');
      lu_mtlx_sb_putu(out, suffix);
    }
    if (out->oom) return 0;
    const char *cand = lu_mtlx_sb_str(out);
    if (!lu_mtlx_upg_has_child_named(scope, cand) && !lu_mtlx_upg_has_child_named(gen, cand))
      return 1;
  }
}

/* RewireToGeneratedNode. */
static int rewire(lu_mtlx_elem *port, const char *node_name) {
  static const char *const kErase[] = {"nodename", "output", "interfacename", "channels",
                                       "value", "nodegraph"};
  for (size_t k = 0; k < 6; k++) lu_mtlx_attr_erase(port, kErase[k]);
  return lu_mtlx_attr_set(port, "nodename", node_name);
}

/* A copy of `port` re-tagged as input `name` of `type` without channels
 * (the `MtlxElem x = *port; x.tag = "input"; ...` pattern), pushed to dst. */
static lu_mtlx_elem *push_port_copy(lu_mtlx_elem *dst, const lu_mtlx_elem *port,
                                    const char *name, const char *type) {
  lu_mtlx_elem *c = lu_mtlx_upg_push_copy(dst, port);
  if (!c || !lu_mtlx_elem_set_tag(c, "input") || !lu_mtlx_attr_set(c, "name", name) ||
      !lu_mtlx_attr_set(c, "type", type))
    return NULL;
  lu_mtlx_attr_erase(c, "channels");
  return c;
}

/* New node `tag` named `name` of `type` pushed to gen. */
static lu_mtlx_elem *push_node(lu_mtlx_elem *gen, const char *tag, const char *name,
                               const char *type) {
  lu_mtlx_elem *n = lu_mtlx_upg_push_new(gen, tag);
  if (!n || !lu_mtlx_attr_set(n, "name", name) || !lu_mtlx_attr_set(n, "type", type)) return NULL;
  return n;
}

static void tag_with_count(char *buf, const char *base, size_t count) {
  const size_t n = strlen(base);
  memcpy(buf, base, n);
  buf[n] = (char)('0' + count);
  buf[n + 1] = '\0';
}

static int set_input_index_name(lu_mtlx_elem *e, size_t i) {
  char buf[32];
  buf[0] = 'i';
  buf[1] = 'n';
  lu_mtlx_sb s;
  lu_mtlx_sb_init(&s);
  lu_mtlx_sb_put(&s, buf, 2);
  lu_mtlx_sb_putu(&s, i + 1);
  const int ok = !s.oom && lu_mtlx_attr_set(e, "name", lu_mtlx_sb_str(&s));
  lu_mtlx_sb_free(&s);
  return ok;
}

/* ---- UpgradeChannelPort --------------------------------------------------- */

typedef struct port_job {
  lu_mtlx_elem *port, *scope, *gen;
  const char *channels, *dest_type, *source_type;
  size_t dest_count, source_count;
} port_job;

/* UpgradeChannelPort: literal / unbound / all-constant -> constant node. */
static int port_constant(port_job *j, lu_mtlx_sb *name) {
  lu_mtlx_sb value;
  lu_mtlx_sb_init(&value);
  int ok = unique_child_name(j->scope, j->gen, "swizzle", 0, name) &&
           swizzled_constant(j->port, j->channels, j->dest_count, &value);
  lu_mtlx_elem *c = ok ? push_node(j->gen, "constant", lu_mtlx_sb_str(name), j->dest_type) : NULL;
  lu_mtlx_elem *v = c ? lu_mtlx_upg_push_new(c, "input") : NULL;
  ok = v && lu_mtlx_attr_set(v, "name", "value") && lu_mtlx_attr_set(v, "type", j->dest_type) &&
       lu_mtlx_attr_set(v, "value", lu_mtlx_sb_str(&value));
  lu_mtlx_sb_free(&value);
  return ok;
}

/* UpgradeChannelPort: direct convert. */
static int port_convert(port_job *j, lu_mtlx_sb *name) {
  if (!unique_child_name(j->scope, j->gen, "swizzle", 0, name)) return 0;
  lu_mtlx_elem *c = push_node(j->gen, "convert", lu_mtlx_sb_str(name), j->dest_type);
  return c && push_port_copy(c, j->port, "in", j->source_type);
}

/* UpgradeChannelPort: single channel -> extract. */
static int port_extract(port_job *j, int index, lu_mtlx_sb *name) {
  if (!unique_child_name(j->scope, j->gen, "swizzle", 0, name)) return 0;
  lu_mtlx_elem *e = push_node(j->gen, "extract", lu_mtlx_sb_str(name), j->dest_type);
  if (!e || !push_port_copy(e, j->port, "in", j->source_type)) return 0;
  lu_mtlx_elem *ix = lu_mtlx_upg_push_new(e, "input");
  char v[2] = {(char)('0' + index), '\0'};
  return ix && lu_mtlx_attr_set(ix, "name", "index") && lu_mtlx_attr_set(ix, "type", "integer") &&
         lu_mtlx_attr_set(ix, "value", v);
}

/* UpgradeChannelPort: one input of the combine node. */
static int port_combine_input(port_job *j, lu_mtlx_elem *combine, size_t i,
                              const char *separate_name) {
  const size_t nch = strlen(j->channels);
  const char ch = i < nch ? j->channels[i] : j->channels[0];
  const char *constant = NULL;
  if (!channel_constant(ch, &constant) && j->source_count == 1) {
    lu_mtlx_sb nm;
    lu_mtlx_sb_init(&nm);
    lu_mtlx_sb_puts(&nm, "in");
    lu_mtlx_sb_putu(&nm, i + 1);
    const int ok = !nm.oom && push_port_copy(combine, j->port, lu_mtlx_sb_str(&nm), "float");
    lu_mtlx_sb_free(&nm);
    return ok;
  }
  lu_mtlx_elem *in = lu_mtlx_upg_push_new(combine, "input");
  if (!in || !set_input_index_name(in, i) || !lu_mtlx_attr_set(in, "type", "float")) return 0;
  if (constant) return lu_mtlx_attr_set(in, "value", constant);
  const int ci = channel_index(ch);
  const int is_color = !strcmp(j->source_type, "color3") || !strcmp(j->source_type, "color4");
  char out[4] = {'o', 'u', 't', '\0'};
  char buf[5];
  memcpy(buf, out, 3);
  buf[3] = ci < 0 ? (is_color ? 'r' : 'x') : ch;
  buf[4] = '\0';
  return lu_mtlx_attr_set(in, "nodename", separate_name) && lu_mtlx_attr_set(in, "output", buf);
}

/* UpgradeChannelPort: separate + combine. */
static int port_combine(port_job *j, lu_mtlx_sb *name) {
  lu_mtlx_sb sep;
  lu_mtlx_sb_init(&sep);
  char tag[32];
  int ok = unique_child_name(j->scope, j->gen, "swizzle", 0, name);
  /* The combine node is built first but pushed after the separate node. */
  lu_mtlx_elem combine;
  lu_mtlx_elem_init(&combine);
  tag_with_count(tag, "combine", j->dest_count);
  ok = ok && lu_mtlx_elem_set_tag(&combine, tag) &&
       lu_mtlx_attr_set(&combine, "name", lu_mtlx_sb_str(name)) &&
       lu_mtlx_attr_set(&combine, "type", j->dest_type);
  if (ok && j->source_count > 1) {
    tag_with_count(tag, "separate", j->source_count);
    ok = unique_child_name(j->scope, j->gen, "separate", 0, &sep);
    lu_mtlx_elem *s = ok ? push_node(j->gen, tag, lu_mtlx_sb_str(&sep), "multioutput") : NULL;
    ok = s && push_port_copy(s, j->port, "in", j->source_type);
  }
  for (size_t i = 0; ok && i < j->dest_count; ++i)
    ok = port_combine_input(j, &combine, i, lu_mtlx_sb_str(&sep));
  ok = ok && lu_mtlx_child_push(j->gen, &combine) != NULL;
  lu_mtlx_elem_free(&combine);
  lu_mtlx_sb_free(&sep);
  return ok;
}

static int is_direct_convert(const char *channels, size_t source_count) {
  static const struct { const char *ch; size_t n; } kPatterns[] = {
      {"rgb", 3}, {"rgb", 4}, {"rgba", 4}, {"xyz", 3}, {"xyz", 4},
      {"xyzw", 4}, {"rr", 1}, {"rrr", 1}, {"xx", 1}, {"xxx", 1}};
  for (size_t k = 0; k < sizeof(kPatterns) / sizeof(kPatterns[0]); k++)
    if (!strcmp(channels, kPatterns[k].ch) && source_count == kPatterns[k].n) return 1;
  return 0;
}

/* Dispatch of UpgradeChannelPort once the types are known. */
static int port_dispatch(port_job *j) {
  lu_mtlx_sb name;
  lu_mtlx_sb_init(&name);
  int ok = 1, rewired = 1;
  if (all_constant_channels(j->channels, j->dest_count) || !has_connection(j->port)) {
    ok = port_constant(j, &name);
  } else if (is_direct_convert(j->channels, j->source_count)) {
    ok = port_convert(j, &name);
  } else if (j->dest_count == 1) {
    const int index = channel_index(j->channels[0]);
    if (index < 0) rewired = 0;
    else ok = port_extract(j, index, &name);
  } else {
    ok = port_combine(j, &name);
  }
  if (ok && rewired) ok = rewire(j->port, lu_mtlx_sb_str(&name));
  lu_mtlx_sb_free(&name);
  return ok;
}

/* UpgradeChannelPort. */
static int upgrade_channel_port(lu_mtlx_elem *port, lu_mtlx_elem *scope, lu_mtlx_elem *gen) {
  const char *ch = lu_mtlx_attr_or(port, "channels", "");
  if (!ch[0]) return 1;
  char *channels = lu_mtlx_strdup(ch);
  char *dest_type = lu_mtlx_strdup(lu_mtlx_attr_or(port, "type", ""));
  char *source_type = lu_mtlx_strdup(connected_node_type(scope, port));
  int ok = channels && dest_type && source_type;
  if (ok && (!source_type[0] || !strcmp(source_type, "multioutput"))) {
    free(source_type);
    source_type = lu_mtlx_strdup(infer_source_type(channels, dest_type));
    ok = source_type != NULL;
  }
  if (ok) {
    port_job j = {port, scope, gen, channels, dest_type, source_type,
                  channel_count(dest_type), channel_count(source_type)};
    if (j.source_count == 0 || j.dest_count == 0) {
      /* unchanged */
    } else if (!strcmp(source_type, "float") && !strcmp(dest_type, "float")) {
      lu_mtlx_attr_erase(port, "channels");
    } else {
      ok = port_dispatch(&j);
    }
  }
  free(channels);
  free(dest_type);
  free(source_type);
  return ok;
}

/* ---- UpgradeMtlxSwizzleNode ----------------------------------------------- */

typedef struct swz_job {
  lu_mtlx_elem *node, *scope, *gen;
  const lu_mtlx_elem *source;
  const char *channels, *dest_type, *source_type;
  size_t dest_count, source_count;
} swz_job;

/* UpgradeMtlxSwizzleNode: all-constant / unbound -> constant node. */
static int swz_constant(swz_job *j) {
  lu_mtlx_sb value;
  lu_mtlx_sb_init(&value);
  lu_mtlx_elem kids;
  lu_mtlx_elem_init(&kids);
  int ok = swizzled_constant(j->source, j->channels, j->dest_count, &value);
  lu_mtlx_elem *v = ok ? lu_mtlx_upg_push_new(&kids, "input") : NULL;
  ok = v && lu_mtlx_attr_set(v, "name", "value") && lu_mtlx_attr_set(v, "type", j->dest_type) &&
       lu_mtlx_attr_set(v, "value", lu_mtlx_sb_str(&value)) &&
       lu_mtlx_elem_set_tag(j->node, "constant");
  if (ok) {
    lu_mtlx_attr_erase(j->node, "nodedef");
    lu_mtlx_upg_take_children(j->node, &kids);
  }
  lu_mtlx_elem_free(&kids);
  lu_mtlx_sb_free(&value);
  return ok;
}

/* UpgradeMtlxSwizzleNode: single channel -> extract. */
static int swz_extract(swz_job *j) {
  const int index = channel_index(j->channels[0]);
  if (index < 0 || j->source_count == 1) return 1;
  lu_mtlx_elem kids;
  lu_mtlx_elem_init(&kids);
  lu_mtlx_elem *s = lu_mtlx_upg_push_copy(&kids, j->source);
  int ok = s != NULL;
  if (ok) {
    lu_mtlx_attr_erase(s, "channels");
    ok = lu_mtlx_attr_set(s, "name", "in") && lu_mtlx_attr_set(s, "type", j->source_type);
  }
  lu_mtlx_elem *ix = ok ? lu_mtlx_upg_push_new(&kids, "input") : NULL;
  char v[2] = {(char)('0' + index), '\0'};
  ok = ix && lu_mtlx_attr_set(ix, "name", "index") && lu_mtlx_attr_set(ix, "type", "integer") &&
       lu_mtlx_attr_set(ix, "value", v) && lu_mtlx_elem_set_tag(j->node, "extract");
  if (ok) {
    lu_mtlx_attr_erase(j->node, "nodedef");
    lu_mtlx_upg_take_children(j->node, &kids);
  }
  lu_mtlx_elem_free(&kids);
  return ok;
}

/* UpgradeMtlxSwizzleNode: one input of the combine node. */
static int swz_combine_input(swz_job *j, lu_mtlx_elem *kids, size_t i, const char *sep) {
  const size_t nch = strlen(j->channels);
  const char ch = i < nch ? j->channels[i] : j->channels[0];
  lu_mtlx_elem *in;
  if (!channel_constant(ch, NULL) && j->source_count == 1) {
    in = lu_mtlx_upg_push_copy(kids, j->source);
    if (!in) return 0;
    lu_mtlx_attr_erase(in, "channels");
    return set_input_index_name(in, i) && lu_mtlx_attr_set(in, "type", "float");
  }
  in = lu_mtlx_upg_push_new(kids, "input");
  if (!in || !set_input_index_name(in, i) || !lu_mtlx_attr_set(in, "type", "float")) return 0;
  if (channel_constant(ch, NULL)) return lu_mtlx_attr_set(in, "value", ch == '0' ? "0" : "1");
  const int index = channel_index(ch);
  const int is_color = !strcmp(j->source_type, "color3") || !strcmp(j->source_type, "color4");
  char buf[5] = {'o', 'u', 't', 0, 0};
  buf[3] = index < 0 ? (is_color ? 'r' : 'x') : (is_color ? "rgba"[index] : "xyzw"[index]);
  return lu_mtlx_attr_set(in, "nodename", sep) && lu_mtlx_attr_set(in, "output", buf);
}

/* UpgradeMtlxSwizzleNode: separate + combine. */
static int swz_combine(swz_job *j) {
  lu_mtlx_sb sep;
  lu_mtlx_sb_init(&sep);
  char tag[32];
  int ok = 1;
  if (j->source_count > 1) {
    ok = unique_child_name(j->scope, j->gen, "separate", 1, &sep);
    tag_with_count(tag, "separate", j->source_count);
    lu_mtlx_elem *s = ok ? push_node(j->gen, tag, lu_mtlx_sb_str(&sep), "multioutput") : NULL;
    lu_mtlx_elem *in = s ? lu_mtlx_upg_push_copy(s, j->source) : NULL;
    ok = in != NULL;
    if (ok) {
      lu_mtlx_attr_erase(in, "channels");
      ok = lu_mtlx_attr_set(in, "name", "in") && lu_mtlx_attr_set(in, "type", j->source_type);
    }
  }
  lu_mtlx_elem kids;
  lu_mtlx_elem_init(&kids);
  for (size_t i = 0; ok && i < j->dest_count; ++i)
    ok = swz_combine_input(j, &kids, i, lu_mtlx_sb_str(&sep));
  tag_with_count(tag, "combine", j->dest_count);
  ok = ok && lu_mtlx_elem_set_tag(j->node, tag);
  if (ok) {
    lu_mtlx_attr_erase(j->node, "nodedef");
    lu_mtlx_upg_take_children(j->node, &kids);
  }
  lu_mtlx_elem_free(&kids);
  lu_mtlx_sb_free(&sep);
  return ok;
}

/* The type part of UpgradeMtlxSwizzleNode, then extract / combine. */
static int swz_typed(swz_job *j) {
  char *st = lu_mtlx_strdup(connected_node_type(j->scope, j->source));
  if (!st) return 0;
  if (!st[0] || !strcmp(st, "multioutput")) {
    free(st);
    st = lu_mtlx_strdup(lu_mtlx_attr_or(j->source, "type", ""));
    if (!st) return 0;
  }
  if (!st[0]) {
    free(st);
    st = lu_mtlx_strdup(infer_source_type(j->channels, j->dest_type));
    if (!st) return 0;
  }
  j->source_type = st;
  j->source_count = channel_count(st);
  int ok = 1;
  if (j->source_count != 0) ok = j->dest_count == 1 ? swz_extract(j) : swz_combine(j);
  free(st);
  return ok;
}

/* UpgradeMtlxSwizzleNode. */
static int upgrade_swizzle_node(lu_mtlx_elem *node, lu_mtlx_elem *scope, lu_mtlx_elem *gen) {
  if (!LU_UPG_TAG_IS(node, "swizzle")) return 1;
  const lu_mtlx_elem *source = NULL;
  const char *ch = "";
  for (size_t k = 0; k < node->nchildren; k++) {
    const lu_mtlx_elem *c = &node->children[k];
    if (!LU_UPG_TAG_IS(c, "input")) continue;
    if (LU_UPG_ATTR_IS(c, "name", "in")) source = c;
    else if (LU_UPG_ATTR_IS(c, "name", "channels")) ch = lu_mtlx_attr_or(c, "value", "");
  }
  if (!source || !ch[0]) return 1;
  char *channels = lu_mtlx_strdup(ch);
  char *dest_type = lu_mtlx_strdup(lu_mtlx_attr_or(node, "type", ""));
  int ok = channels && dest_type;
  if (ok) {
    swz_job j = {node, scope, gen, source, channels, dest_type, NULL,
                 channel_count(dest_type), 0};
    if (j.dest_count == 0) {
      /* unchanged */
    } else if (all_constant_channels(channels, j.dest_count) || !has_connection(source)) {
      ok = swz_constant(&j);
    } else {
      ok = swz_typed(&j);
    }
  }
  free(channels);
  free(dest_type);
  return ok;
}

/* ---- UpgradeMtlxChannels -------------------------------------------------- */

static int is_port(const lu_mtlx_elem *e) {
  return LU_UPG_TAG_IS(e, "input") || LU_UPG_TAG_IS(e, "output");
}

/* UpgradeMtlxChannels: one child of scope; generated nodes go to gen. */
static int channels_child(lu_mtlx_elem *scope, lu_mtlx_elem *child, lu_mtlx_elem *gen) {
  int ok = 1;
  if (is_port(child)) ok = upgrade_channel_port(child, scope, gen);
  if (ok && lu_mtlx_upg_is_node(child)) {
    if (LU_UPG_TAG_IS(child, "swizzle")) ok = upgrade_swizzle_node(child, scope, gen);
    for (size_t p = 0; ok && p < child->nchildren; p++)
      if (is_port(&child->children[p]))
        ok = upgrade_channel_port(&child->children[p], scope, gen);
  }
  return ok;
}

/* UpgradeMtlxChannels: one scope (root, nodegraphs and materials). */
static int channels_visit(lu_mtlx_elem *scope, lu_mtlx_elem *parent, void *user) {
  int *ok = (int *)user;
  if (parent && !LU_UPG_TAG_IS(scope, "nodegraph") && !LU_UPG_TAG_IS(scope, "material"))
    return 0;
  for (size_t ci = 0; *ok && ci < scope->nchildren; ++ci) {
    lu_mtlx_elem gen;
    lu_mtlx_elem_init(&gen);
    *ok = channels_child(scope, &scope->children[ci], &gen);
    /* Generated nodes are inserted, in order, before the child. */
    const size_t ng = gen.nchildren;
    for (size_t g = 0; *ok && g < ng; g++)
      *ok = lu_mtlx_child_insert(scope, ci + g, &gen.children[g]) != NULL;
    lu_mtlx_elem_free(&gen);
    ci += ng;
  }
  return 1;
}

int lu_mtlx_upg_channels(lu_mtlx_elem *root) {
  int ok = 1;
  return lu_mtlx_upg_preorder(root, channels_visit, &ok) && ok;
}
