/*
 * mtlx_doc.h - semantic MaterialX document built from the XML tree.
 *
 * Nodes from every <nodegraph> plus top-level shader nodes are flattened into a
 * single global node table. Input references (nodename / nodegraph+output) are
 * resolved to global node ids at load time, so the evaluator never re-parses
 * names or walks nodegraph-output indirection in the hot path.
 */
#ifndef MTLXRENDER_MTLX_DOC_H_
#define MTLXRENDER_MTLX_DOC_H_

typedef enum {
    MV_NONE = 0, MV_FLOAT, MV_COLOR3, MV_COLOR4,
    MV_VEC2, MV_VEC3, MV_VEC4, MV_MATRIX33, MV_MATRIX44,
    MV_INT, MV_BOOL, MV_STRING, MV_FILENAME
} MtlxType;

typedef struct {
    MtlxType type;
    float    v[16];  /* numeric payload, including column-major matrices */
    char    *s;      /* string/filename payload (owned), else NULL */
} MtlxValue;

typedef struct {
    char    *name;        /* input name, e.g. "base_color" */
    MtlxType type;
    int      has_value;   /* 1 if a literal value was given */
    MtlxValue value;
    int      src_node;    /* connected upstream node id, or -1 */
    int      has_connection; /* an authored connection, including unresolved ones */
    char    *src_output;  /* sub-output name on a multi-output node, or NULL */
    char    *channels;    /* MaterialX input channels selector, e.g. "g" */
    int      colorspace_srgb; /* supported Rec.709 sRGB texture aliases */
    char    *colorspace;  /* authored token for renderer color management */
} MtlxInput;

typedef struct {
    char    *category;    /* node tag: image, normalmap, standard_surface, ... */
    char    *name;        /* node instance name */
    MtlxType type;        /* declared output type */
    int      graph_id;    /* owning nodegraph index, or -1 for document scope */
    char    *nodedef;     /* explicit `nodedef` attribute, or NULL */
    MtlxInput *inputs;
    int      ninput;
} MtlxNode;

typedef struct {
    char *name;          /* surface/volume material name */
    int   surface_node;  /* resolved surface shader node id, or -1 */
    int   volume_node;   /* resolved volume shader node id, or -1 */
} MtlxMaterial;

typedef struct {
    char *geom;      /* geometry name, e.g. "Bishop_B" */
    char *material;  /* surfacematerial name, e.g. "M_Bishop_B" */
} MtlxAssign;

typedef struct MtlxDoc {
    MtlxNode     *nodes;   int nnode;
    MtlxMaterial *mats;    int nmat;
    MtlxAssign   *assigns; int nassign;
    char        **graph_names; int ngraph;
    int           doc_colorspace_srgb; /* unused-but-kept document default */
} MtlxDoc;

/* Build a document from a .mtlx file. Returns NULL on error. */
MtlxDoc *mtlx_load(const char *path);
/* Build a document from an in-memory .mtlx string (for tests). */
MtlxDoc *mtlx_load_string(const char *xml);
void mtlx_free(MtlxDoc *d);

/* Lookups used by material binding. */
int mtlx_find_material(const MtlxDoc *d, const char *surfacematerial_name);
int mtlx_find_node(const MtlxDoc *d, int graph_id, const char *name);

#endif /* MTLXRENDER_MTLX_DOC_H_ */
