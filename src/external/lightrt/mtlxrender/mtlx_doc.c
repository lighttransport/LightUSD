#include "mtlx_doc.h"
#include "mtlx_xml.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- type / value parsing --------------------------------------------- */

static MtlxType parse_type(const char *t) {
    if (!t) return MV_NONE;
    if (!strcmp(t, "float")) return MV_FLOAT;
    if (!strcmp(t, "color3")) return MV_COLOR3;
    if (!strcmp(t, "color4")) return MV_COLOR4;
    if (!strcmp(t, "vector2")) return MV_VEC2;
    if (!strcmp(t, "vector3")) return MV_VEC3;
    if (!strcmp(t, "vector4")) return MV_VEC4;
    if (!strcmp(t, "matrix33")) return MV_MATRIX33;
    if (!strcmp(t, "matrix44")) return MV_MATRIX44;
    if (!strcmp(t, "integer")) return MV_INT;
    if (!strcmp(t, "boolean")) return MV_BOOL;
    if (!strcmp(t, "string")) return MV_STRING;
    if (!strcmp(t, "filename")) return MV_FILENAME;
    return MV_NONE;
}

static int type_ncomp(MtlxType t) {
    switch (t) {
        case MV_FLOAT: case MV_INT: case MV_BOOL: return 1;
        case MV_VEC2: return 2;
        case MV_COLOR3: case MV_VEC3: return 3;
        case MV_COLOR4: case MV_VEC4: return 4;
        case MV_MATRIX33: return 9;
        case MV_MATRIX44: return 16;
        default: return 0;
    }
}

static void parse_value(const char *s, MtlxType type, MtlxValue *out) {
    memset(out, 0, sizeof(*out));
    out->type = type;
    if (!s) return;
    if (type == MV_STRING || type == MV_FILENAME) {
        out->s = strdup(s);
        return;
    }
    if (type == MV_BOOL) {
        out->v[0] = (!strcmp(s, "true") || !strcmp(s, "1")) ? 1.0f : 0.0f;
        return;
    }
    int n = type_ncomp(type);
    if (n == 0) n = 1;
    const char *p = s;
    for (int i = 0; i < n; i++) {
        char *end = NULL;
        float val = strtof(p, &end);
        out->v[i] = val;
        if (end == p) break;
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
    }
    /* scalar -> broadcast to all components for color/vector convenience */
    if (n >= 3 && p == s) { /* nothing parsed */ }
}

/* ---- growable document builder ---------------------------------------- */

typedef struct {
    MtlxDoc d;
    /* pending output records: graph_id, output-name -> nodename (+sub) */
    struct { int graph_id; char *name; char *nodename; char *sub; } *outs;
    int nout;
    /* pending material shader nodenames (top-level scope) */
    char **mat_shadernames;
    char **mat_volumenames;
} Builder;

static int add_graph(Builder *b, const char *name) {
    b->d.graph_names = realloc(b->d.graph_names, sizeof(char *) * (b->d.ngraph + 1));
    b->d.graph_names[b->d.ngraph] = strdup(name ? name : "");
    return b->d.ngraph++;
}

static int add_node(Builder *b, const char *category, const char *name, MtlxType type, int graph_id) {
    b->d.nodes = realloc(b->d.nodes, sizeof(MtlxNode) * (b->d.nnode + 1));
    MtlxNode *n = &b->d.nodes[b->d.nnode];
    memset(n, 0, sizeof(*n));
    n->category = strdup(category ? category : "");
    n->name = strdup(name ? name : "");
    n->type = type;
    n->graph_id = graph_id;
    return b->d.nnode++;
}

static void node_add_input(MtlxNode *n, const XmlNode *x, const char *fileprefix) {
    /* The nearest enclosing `fileprefix` applies; an input may declare its own. */
    if (xml_attr(x, "fileprefix")) fileprefix = xml_attr(x, "fileprefix");
    n->inputs = realloc(n->inputs, sizeof(MtlxInput) * (n->ninput + 1));
    MtlxInput *in = &n->inputs[n->ninput];
    memset(in, 0, sizeof(*in));
    in->src_node = -1;
    in->name = strdup(xml_attr(x, "name") ? xml_attr(x, "name") : "");
    in->type = parse_type(xml_attr(x, "type"));
    const char *val = xml_attr(x, "value");
    if (val) { in->has_value = 1; parse_value(val, in->type, &in->value); }
    /* MaterialX `fileprefix` is prepended to every filename value in scope;
     * the value declared by the nearest enclosing element (input, node,
     * nodegraph, document) replaces any outer one. e.g. the boombox
     * material's fileprefix="boombox/" puts its textures in a subdirectory
     * the search path could never reach by walking up. */
    if (in->type == MV_FILENAME && in->value.s && fileprefix && fileprefix[0]) {
        size_t L = strlen(fileprefix) + strlen(in->value.s) + 1;
        char *full = (char *)malloc(L);
        snprintf(full, L, "%s%s", fileprefix, in->value.s);
        free(in->value.s);
        in->value.s = full;
    }
    const char *cs = xml_attr(x, "colorspace");
    if (cs && !strcmp(cs, "srgb_texture")) in->colorspace_srgb = 1;
    const char *channels = xml_attr(x, "channels");
    if (channels && channels[0]) in->channels = strdup(channels);
    /* connection attrs stashed temporarily as strings in src_output/value.s;
     * resolved in pass B. We store raw nodename/nodegraph/output via a small
     * side channel encoded in src_output: "N:<nodename>" or "G:<graph>|<out>". */
    const char *nodename = xml_attr(x, "nodename");
    const char *nodegraph = xml_attr(x, "nodegraph");
    const char *output = xml_attr(x, "output");
    const char *interfacename = xml_attr(x, "interfacename");
    in->has_connection = nodename || nodegraph || interfacename;
    if (nodename) {
        const char *o = output ? output : "";
        size_t L = strlen(nodename) + strlen(o) + 4;
        in->src_output = malloc(L);
        snprintf(in->src_output, L, "N:%s\x01%s", nodename, o);
    } else if (nodegraph) {
        const char *o = output ? output : "";
        size_t L = strlen(nodegraph) + strlen(o) + 4;
        in->src_output = malloc(L);
        snprintf(in->src_output, L, "G:%s|%s", nodegraph, o);
    } else if (interfacename) {
        /* references an interface input of the owning nodegraph; resolved in
         * pass B against the synthetic "input"-category node for that name. */
        size_t L = strlen(interfacename) + 3;
        in->src_output = malloc(L);
        snprintf(in->src_output, L, "I:%s", interfacename);
    }
    n->ninput++;
}

static void node_set_nodedef(MtlxNode *n, const XmlNode *x) {
    const char *nd = xml_attr(x, "nodedef");
    if (nd && nd[0]) n->nodedef = strdup(nd);
}

/* ---- standard-library node definitions --------------------------------
 * mtlx_stdlib_defs.inc is generated from MaterialX libraries/stdlib/
 * stdlib_defs.mtlx by the Lean model in the materialx-conformance repository
 * (`lake exe mtlxspec-gen`): one row per <nodedef> with inheritance applied,
 * followed by its inputs and their default value strings. */
typedef struct { const char *name, *node, *out_type; int first, count; } StdNodeDef;
typedef struct { const char *name, *type; int has_default; const char *dflt, *default_geomprop; } StdInput;

static const StdNodeDef g_std_nodedefs[] = {
#define MTLX_STD_NODEDEF(name, node, type, first, count) { name, node, type, first, count },
#define MTLX_STD_INPUT(name, type, has_default, dflt, geomprop)
#include "mtlx_stdlib_defs.inc"
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
};
static const StdInput g_std_inputs[] = {
#define MTLX_STD_NODEDEF(name, node, type, first, count)
#define MTLX_STD_INPUT(name, type, has_default, dflt, geomprop) { name, type, has_default, dflt, geomprop },
#include "mtlx_stdlib_defs.inc"
#undef MTLX_STD_NODEDEF
#undef MTLX_STD_INPUT
};
#define STD_NODEDEF_COUNT ((int)(sizeof(g_std_nodedefs) / sizeof(g_std_nodedefs[0])))

/* MaterialX signature resolution: an explicit `nodedef`, else the first
 * definition in library order with the node's category and output type whose
 * declared inputs include every input the instance supplies, name and type. */
static const StdNodeDef *find_std_nodedef(const MtlxNode *n) {
    if (n->nodedef) {
        for (int i = 0; i < STD_NODEDEF_COUNT; i++)
            if (!strcmp(g_std_nodedefs[i].name, n->nodedef)) return &g_std_nodedefs[i];
        return NULL;
    }
    for (int i = 0; i < STD_NODEDEF_COUNT; i++) {
        const StdNodeDef *d = &g_std_nodedefs[i];
        if (strcmp(d->node, n->category) || parse_type(d->out_type) != n->type) continue;
        int exact = 1;
        for (int j = 0; j < n->ninput && exact; j++) {
            int found = 0;
            for (int k = 0; k < d->count && !found; k++) {
                const StdInput *p = &g_std_inputs[d->first + k];
                found = !strcmp(p->name, n->inputs[j].name) && parse_type(p->type) == n->inputs[j].type;
            }
            exact = found;
        }
        if (exact) return d;
    }
    return NULL;
}

static void apply_stdlib_defaults(MtlxNode *n) {
    if (!strcmp(n->category, "input")) return; /* synthetic interface node */
    const StdNodeDef *d = find_std_nodedef(n);
    if (!d) return;
    for (int k = 0; k < d->count; k++) {
        const StdInput *p = &g_std_inputs[d->first + k];
        MtlxType t = parse_type(p->type);
        /* Shader-typed and file inputs have no usable literal default. */
        if (!p->has_default || p->default_geomprop[0] || t == MV_NONE || t == MV_FILENAME) continue;
        /* The library default of `lumacoeffs` is the ACEScg triple; the
         * evaluator substitutes its working-space coefficients instead. */
        if (!strcmp(p->name, "lumacoeffs")) continue;
        int present = 0;
        for (int j = 0; j < n->ninput && !present; j++) present = !strcmp(n->inputs[j].name, p->name);
        if (present) continue;
        n->inputs = realloc(n->inputs, sizeof(MtlxInput) * (n->ninput + 1));
        MtlxInput *in = &n->inputs[n->ninput++];
        memset(in, 0, sizeof(*in));
        in->src_node = -1;
        in->name = strdup(p->name);
        in->type = t;
        in->has_value = 1;
        parse_value(p->dflt, t, &in->value);
    }
}

static int find_graph(const Builder *b, const char *name) {
    for (int i = 0; i < b->d.ngraph; i++)
        if (!strcmp(b->d.graph_names[i], name)) return i;
    return -1;
}

int mtlx_find_node(const MtlxDoc *d, int graph_id, const char *name) {
    for (int i = 0; i < d->nnode; i++)
        if (d->nodes[i].graph_id == graph_id && !strcmp(d->nodes[i].name, name)) return i;
    return -1;
}

int mtlx_find_material(const MtlxDoc *d, const char *name) {
    for (int i = 0; i < d->nmat; i++)
        if (!strcmp(d->mats[i].name, name)) return i;
    return -1;
}

/* resolve a graph output (graph_id, outname) -> global node id; sub via *sub */
static int resolve_graph_output(Builder *b, int graph_id, const char *outname, char **sub) {
    for (int i = 0; i < b->nout; i++) {
        if (b->outs[i].graph_id == graph_id && !strcmp(b->outs[i].name, outname)) {
            if (sub) *sub = b->outs[i].sub;
            return mtlx_find_node(&b->d, graph_id, b->outs[i].nodename);
        }
    }
    return -1;
}

static char *material_shader_ref(const XmlNode *input) {
    const char *node = xml_attr(input, "nodename");
    if (node && node[0]) return strdup(node);
    const char *graph = xml_attr(input, "nodegraph");
    if (graph && graph[0]) {
        const char *output = xml_attr(input, "output");
        if (!output || !output[0]) output = "out";
        const size_t size = strlen(graph) + strlen(output) + 4;
        char *ref = (char *)malloc(size);
        if (ref) snprintf(ref, size, "G:%s|%s", graph, output);
        return ref;
    }
    return strdup("");
}

static int resolve_material_shader(Builder *b, char *ref) {
    if (!ref) return -1;
    if (ref[0] == 'G' && ref[1] == ':') {
        char *bar = strchr(ref + 2, '|');
        if (!bar) return mtlx_find_node(&b->d, -1, ref);
        *bar = '\0';
        const int graph = find_graph(b, ref + 2);
        char *sub = NULL;
        const int node = graph < 0 ? -1 : resolve_graph_output(b, graph, bar + 1, &sub);
        return sub && sub[0] && strcmp(sub, "out") ? -1 : node;
    }
    return mtlx_find_node(&b->d, -1, ref);
}

static const char *is_shader_category(const char *tag) {
    if (!strcmp(tag, "standard_surface")) return tag;
    if (!strcmp(tag, "open_pbr_surface")) return tag;
    return NULL;
}

/* Resolve the standard defaultgeomprop declarations into real graph edges.
 * A document geompropdef overrides the generated standard definition. */
typedef struct {
    const char *name, *type, *category;
    int has_space; const char *space;
    int has_index; const char *index;
} StdGeomProp;
static const StdGeomProp g_std_geomprops[] = {
#define MTLX_STD_GEOMPROPDEF(name, type, category, has_space, space, has_index, index) \
    { name, type, category, has_space, space, has_index, index },
#include "mtlx_stdlib_geomprops.inc"
#undef MTLX_STD_GEOMPROPDEF
};

typedef struct { int graph_id; const char *name; int node_id; } DefaultGeometry;

static void node_add_geometry_literal(MtlxNode *node, const char *name,
                                      MtlxType type, const char *value) {
    node->inputs = realloc(node->inputs, sizeof(MtlxInput) * (node->ninput + 1));
    MtlxInput *in = &node->inputs[node->ninput++];
    memset(in, 0, sizeof(*in));
    in->name = strdup(name);
    in->type = type;
    in->src_node = -1;
    in->has_value = 1;
    parse_value(value, type, &in->value);
}

static int geometry_index_is_zero(const char *index) {
    if (!index) return 1;
    char *end = NULL;
    long value = strtol(index, &end, 10);
    if (end == index) return 0;
    while (isspace((unsigned char)*end)) end++;
    return value == 0 && *end == '\0';
}

static int apply_stdlib_geometry_defaults(Builder *b, const XmlNode *document) {
    DefaultGeometry *cache = NULL;
    int ncache = 0;
    const int original_count = b->d.nnode;
    for (int i = 0; i < original_count; i++) {
        const StdNodeDef *def = find_std_nodedef(&b->d.nodes[i]);
        if (!def || !strcmp(b->d.nodes[i].category, "input")) continue;
        for (int j = 0; j < def->count; j++) {
            const StdInput *port = &g_std_inputs[def->first + j];
            if (!port->default_geomprop[0]) continue;
            int present = 0;
            for (int k = 0; k < b->d.nodes[i].ninput; k++)
                if (!strcmp(b->d.nodes[i].inputs[k].name, port->name)) present = 1;
            if (present) continue;
            const int gid = b->d.nodes[i].graph_id;
            int source = -1;
            for (int k = 0; k < ncache; k++)
                if (cache[k].graph_id == gid && !strcmp(cache[k].name, port->default_geomprop))
                    source = cache[k].node_id;
            if (source < 0) {
                const char *category = NULL, *type = NULL, *space = NULL, *index = NULL;
                int found = 0;
                for (int k = 0; k < document->nchild; k++) {
                    const XmlNode *elem = &document->children[k];
                    const char *name = xml_attr(elem, "name");
                    if (strcmp(elem->tag, "geompropdef") || !name || strcmp(name, port->default_geomprop)) continue;
                    if (found++) { category = NULL; break; }
                    category = xml_attr(elem, "geomprop");
                    type = xml_attr(elem, "type");
                    space = xml_attr(elem, "space");
                    index = xml_attr(elem, "index");
                }
                if (!found) {
                    size_t count = sizeof(g_std_geomprops) / sizeof(g_std_geomprops[0]);
                    for (size_t k = 0; k < count; k++) {
                        const StdGeomProp *g = &g_std_geomprops[k];
                        if (strcmp(g->name, port->default_geomprop)) continue;
                        category = g->category; type = g->type;
                        space = g->has_space ? g->space : NULL;
                        index = g->has_index ? g->index : NULL;
                        break;
                    }
                }
                if (!category || !type || strcmp(type, port->type) ||
                    (strcmp(category, "texcoord") && strcmp(category, "position") &&
                     strcmp(category, "normal") && strcmp(category, "tangent") &&
                     strcmp(category, "bitangent")) ||
                    (space && strcmp(space, "model") && strcmp(space, "object") && strcmp(space, "world")) ||
                    !geometry_index_is_zero(index)) {
                    free(cache);
                    return 0;
                }
                char generated_name[64];
                int suffix = b->d.nnode;
                do { snprintf(generated_name, sizeof(generated_name), "__mtlx_geom_%d", suffix++); }
                while (mtlx_find_node(&b->d, gid, generated_name) >= 0);
                source = add_node(b, category, generated_name, parse_type(type), gid);
                if (space) node_add_geometry_literal(&b->d.nodes[source], "space", MV_STRING, space);
                if (index) node_add_geometry_literal(&b->d.nodes[source], "index", MV_INT, index);
                cache = realloc(cache, sizeof(*cache) * (ncache + 1));
                cache[ncache++] = (DefaultGeometry){gid, port->default_geomprop, source};
            }
            MtlxNode *node = &b->d.nodes[i];
            node->inputs = realloc(node->inputs, sizeof(MtlxInput) * (node->ninput + 1));
            MtlxInput *in = &node->inputs[node->ninput++];
            memset(in, 0, sizeof(*in));
            in->name = strdup(port->name);
            in->type = parse_type(port->type);
            in->src_node = source;
            in->has_connection = 1;
        }
    }
    free(cache);
    return 1;
}

static void mtlx_free_contents(MtlxDoc *d);

static MtlxDoc *mtlx_build(XmlNode *root) {
    if (!root) return NULL;
    const XmlNode *mx = xml_child(root, "materialx");
    if (!mx) { xml_free(root); return NULL; }

    Builder b;
    memset(&b, 0, sizeof(b));

    /* document-level file prefix (prepended to all filename values in scope) */
    const char *root_pre = xml_attr(mx, "fileprefix");
    if (!root_pre) root_pre = "";

    /* ---- pass A: create nodes, graphs, outputs, materials, assigns ---- */
    for (int i = 0; i < mx->nchild; i++) {
        const XmlNode *el = &mx->children[i];
        const char *tag = el->tag;

        if (!strcmp(tag, "nodegraph")) {
            int gid = add_graph(&b, xml_attr(el, "name"));
            /* a nodegraph fileprefix replaces the document one within the graph */
            const char *gfp = xml_attr(el, "fileprefix");
            const char *gpre = gfp ? gfp : root_pre;
            for (int j = 0; j < el->nchild; j++) {
                const XmlNode *c = &el->children[j];
                if (!strcmp(c->tag, "output")) {
                    b.outs = realloc(b.outs, sizeof(*b.outs) * (b.nout + 1));
                    b.outs[b.nout].graph_id = gid;
                    b.outs[b.nout].name = strdup(xml_attr(c, "name") ? xml_attr(c, "name") : "");
                    const char *nn = xml_attr(c, "nodename");
                    b.outs[b.nout].nodename = strdup(nn ? nn : "");
                    const char *sub = xml_attr(c, "output");
                    b.outs[b.nout].sub = sub ? strdup(sub) : NULL;
                    b.nout++;
                } else if (!strcmp(c->tag, "input")) {
                    /* nodegraph interface input: a node of category "input"
                     * whose self-input carries the interface's value (or an
                     * upstream connection). Referenced via interfacename. */
                    int nid = add_node(&b, "input", xml_attr(c, "name"), parse_type(xml_attr(c, "type")), gid);
                    node_add_input(&b.d.nodes[nid], c, gpre);
                } else {
                    int nid = add_node(&b, c->tag, xml_attr(c, "name"), parse_type(xml_attr(c, "type")), gid);
                    node_set_nodedef(&b.d.nodes[nid], c);
                    const char *npre = xml_attr(c, "fileprefix") ? xml_attr(c, "fileprefix") : gpre;
                    for (int k = 0; k < c->nchild; k++)
                        if (!strcmp(c->children[k].tag, "input"))
                            node_add_input(&b.d.nodes[nid], &c->children[k], npre);
                }
            }
        } else if (!strcmp(tag, "geompropdef")) {
            /* Consumed by geometry-default resolution below. */
        } else if (is_shader_category(tag)) {
            int nid = add_node(&b, tag, xml_attr(el, "name"), parse_type(xml_attr(el, "type")), -1);
            node_set_nodedef(&b.d.nodes[nid], el);
            const char *npre = xml_attr(el, "fileprefix") ? xml_attr(el, "fileprefix") : root_pre;
            for (int k = 0; k < el->nchild; k++)
                if (!strcmp(el->children[k].tag, "input"))
                    node_add_input(&b.d.nodes[nid], &el->children[k], npre);
        } else if (!strcmp(tag, "surfacematerial") ||
                   !strcmp(tag, "volumematerial")) {
            b.d.mats = realloc(b.d.mats, sizeof(MtlxMaterial) * (b.d.nmat + 1));
            b.mat_shadernames = realloc(b.mat_shadernames, sizeof(char *) * (b.d.nmat + 1));
            b.mat_volumenames = realloc(b.mat_volumenames, sizeof(char *) * (b.d.nmat + 1));
            MtlxMaterial *m = &b.d.mats[b.d.nmat];
            m->name = strdup(xml_attr(el, "name") ? xml_attr(el, "name") : "");
            m->surface_node = -1;
            m->volume_node = -1;
            char *surface_name = NULL;
            char *volume_name = NULL;
            for (int k = 0; k < el->nchild; k++) {
                const XmlNode *in = &el->children[k];
                if (!strcmp(in->tag, "input") && xml_attr(in, "name") &&
                    !strcmp(xml_attr(in, "name"), "surfaceshader"))
                    { free(surface_name); surface_name = material_shader_ref(in); }
                if (!strcmp(in->tag, "input") && xml_attr(in, "name") &&
                    !strcmp(xml_attr(in, "name"), "volumeshader"))
                    { free(volume_name); volume_name = material_shader_ref(in); }
            }
            b.mat_shadernames[b.d.nmat] = surface_name;
            b.mat_volumenames[b.d.nmat] = volume_name;
            b.d.nmat++;
        } else if (!strcmp(tag, "look")) {
            for (int k = 0; k < el->nchild; k++) {
                const XmlNode *a = &el->children[k];
                if (strcmp(a->tag, "materialassign")) continue;
                b.d.assigns = realloc(b.d.assigns, sizeof(MtlxAssign) * (b.d.nassign + 1));
                const char *g = xml_attr(a, "geom");
                const char *mtl = xml_attr(a, "material");
                b.d.assigns[b.d.nassign].geom = strdup(g ? g : "");
                b.d.assigns[b.d.nassign].material = strdup(mtl ? mtl : "");
                b.d.nassign++;
            }
        } else {
            /* any other top-level node (constant, etc.) */
            int nid = add_node(&b, tag, xml_attr(el, "name"), parse_type(xml_attr(el, "type")), -1);
            node_set_nodedef(&b.d.nodes[nid], el);
            const char *npre = xml_attr(el, "fileprefix") ? xml_attr(el, "fileprefix") : root_pre;
            for (int k = 0; k < el->nchild; k++)
                if (!strcmp(el->children[k].tag, "input"))
                    node_add_input(&b.d.nodes[nid], &el->children[k], npre);
        }
    }

    /* Bind every input a standard-library definition declares but the
     * instance omits to the definition's default value. */
    for (int i = 0; i < b.d.nnode; i++) apply_stdlib_defaults(&b.d.nodes[i]);
    if (!apply_stdlib_geometry_defaults(&b, mx)) {
        for (int i = 0; i < b.nout; i++) {
            free(b.outs[i].name); free(b.outs[i].nodename); free(b.outs[i].sub);
        }
        free(b.outs);
        for (int i = 0; i < b.d.nmat; i++) {
            free(b.mat_shadernames[i]); free(b.mat_volumenames[i]);
        }
        free(b.mat_shadernames); free(b.mat_volumenames);
        mtlx_free_contents(&b.d);
        xml_free(root);
        return NULL;
    }

    /* ---- pass B: resolve input connections ---- */
    for (int i = 0; i < b.d.nnode; i++) {
        MtlxNode *n = &b.d.nodes[i];
        for (int j = 0; j < n->ninput; j++) {
            MtlxInput *in = &n->inputs[j];
            if (!in->src_output) continue; /* literal or unconnected */
            char *enc = in->src_output;
            if (enc[0] == 'N' && enc[1] == ':') {
                char *sep = strchr(enc + 2, '\x01');
                char *sub = NULL;
                if (sep) { *sep = '\0'; if (sep[1]) sub = sep + 1; }
                int nid = mtlx_find_node(&b.d, n->graph_id, enc + 2);
                in->src_node = nid;
                char *subdup = sub ? strdup(sub) : NULL;
                free(in->src_output);
                in->src_output = subdup;
            } else if (enc[0] == 'I' && enc[1] == ':') {
                /* interface input: bind to the owning graph's "input" node. */
                int nid = mtlx_find_node(&b.d, n->graph_id, enc + 2);
                in->src_node = nid;
                free(in->src_output);
                in->src_output = NULL;
            } else if (enc[0] == 'G' && enc[1] == ':') {
                char *bar = strchr(enc + 2, '|');
                char *sub = NULL;
                if (bar) {
                    *bar = '\0';
                    int gid = find_graph(&b, enc + 2);
                    int nid = (gid >= 0) ? resolve_graph_output(&b, gid, bar + 1, &sub) : -1;
                    in->src_node = nid;
                }
                free(in->src_output);
                in->src_output = sub ? strdup(sub) : NULL;
            }
        }
    }

    /* ---- resolve material surface nodes (top-level scope) ---- */
    for (int i = 0; i < b.d.nmat; i++) {
        b.d.mats[i].surface_node = resolve_material_shader(&b, b.mat_shadernames[i]);
        b.d.mats[i].volume_node = resolve_material_shader(&b, b.mat_volumenames[i]);
        free(b.mat_shadernames[i]);
        free(b.mat_volumenames[i]);
    }
    free(b.mat_shadernames);
    free(b.mat_volumenames);

    /* free pending output records */
    for (int i = 0; i < b.nout; i++) { free(b.outs[i].name); free(b.outs[i].nodename); free(b.outs[i].sub); }
    free(b.outs);

    const char *cs = xml_attr(mx, "colorspace");
    b.d.doc_colorspace_srgb = (cs && strstr(cs, "srgb")) ? 1 : 0;

    xml_free(root);

    MtlxDoc *out = malloc(sizeof(MtlxDoc));
    *out = b.d;
    return out;
}

MtlxDoc *mtlx_load(const char *path) {
    return mtlx_build(xml_parse_file(path));
}

MtlxDoc *mtlx_load_string(const char *xml) {
    return mtlx_build(xml_parse_memory(xml, strlen(xml)));
}

static void mtlx_free_contents(MtlxDoc *d) {
    for (int i = 0; i < d->nnode; i++) {
        MtlxNode *n = &d->nodes[i];
        for (int j = 0; j < n->ninput; j++) {
            free(n->inputs[j].name);
            free(n->inputs[j].value.s);
            free(n->inputs[j].src_output);
            free(n->inputs[j].channels);
        }
        free(n->inputs);
        free(n->category);
        free(n->name);
        free(n->nodedef);
    }
    free(d->nodes);
    for (int i = 0; i < d->nmat; i++) free(d->mats[i].name);
    free(d->mats);
    for (int i = 0; i < d->nassign; i++) { free(d->assigns[i].geom); free(d->assigns[i].material); }
    free(d->assigns);
    for (int i = 0; i < d->ngraph; i++) free(d->graph_names[i]);
    free(d->graph_names);
}

void mtlx_free(MtlxDoc *d) {
    if (!d) return;
    mtlx_free_contents(d);
    free(d);
}
