/*
 * mtlx_eval.h - demand-driven MaterialX node-graph evaluator.
 *
 * Given a shade point (uv + tangent frame) and a surface shader node, walk the
 * graph and produce a flat OpenPBRParams the BSDF can consume. standard_surface
 * inputs map onto the OpenPBR layering ~1:1.
 */
#ifndef MTLXRENDER_MTLX_EVAL_H_
#define MTLXRENDER_MTLX_EVAL_H_

#include "mtlx_doc.h"
#include "texture.h"
#include "vecmath.h"

enum {
    MTLX_DIFFUSE_LAMBERT = 0,
    MTLX_DIFFUSE_EON = 1,
    MTLX_DIFFUSE_OREN_NAYAR = 2,
    MTLX_DIFFUSE_BURLEY = 3
};

#define MTLX_MAX_DIFFUSE_LOBES 32

typedef struct {
    v3 color;
    float weight;
    v3 weight_color; /* channel scaling applied after the lobe response */
    float roughness;
    int model; /* MTLX_DIFFUSE_* */
} MtlxDiffuseLobe;

#define MTLX_MAX_CONDUCTOR_LOBES 32

typedef struct {
    v3 ior, extinction;
    float weight;
    v3 weight_color;
    float roughness; /* MaterialX GGX alpha along tangent */
    float roughness_y; /* GGX alpha along bitangent */
    v3 tangent; /* world-space tangent at graph evaluation */
    float thinfilm_thickness; /* nanometers; zero disables the film */
    float thinfilm_ior;
} MtlxConductorLobe;

#define MTLX_MAX_SHEEN_LOBES 32
#define MTLX_SHEEN_CONTY_KULLA 0
#define MTLX_SHEEN_ZELTNER 1

typedef struct {
    v3 color;
    float weight;
    v3 weight_color;
    float roughness;
    int model; /* MTLX_SHEEN_* */
} MtlxSheenLobe;

typedef struct {
    /* base / diffuse */
    float base_weight;
    v3    base_color;
    float diffuse_roughness;
    int   diffuse_model; /* MTLX_DIFFUSE_* */
    int   diffuse_lobe_count;
    int   diffuse_lobe_overflow;
    MtlxDiffuseLobe diffuse_lobes[MTLX_MAX_DIFFUSE_LOBES];
    float metalness;
    int conductor_lobe_count;
    int conductor_lobe_overflow;
    MtlxConductorLobe conductor_lobes[MTLX_MAX_CONDUCTOR_LOBES];
    /* specular (GGX dielectric) */
    float specular_weight;
    v3    specular_color;
    float specular_roughness;
    float specular_ior;
    /* transmission */
    float transmission;
    v3    transmission_color;
    float transmission_depth; /* Beer-Lambert distance for transmission_color (0 = no absorption) */
    v3    transmission_scatter;         /* volumetric scattering color (0 = no in-scattering) */
    float transmission_scatter_anisotropy; /* Henyey-Greenstein g of the interior medium */
    /* subsurface */
    float subsurface;
    v3    subsurface_color;
    v3    subsurface_radius;
    float subsurface_scale;
    float subsurface_anisotropy;
    /* coat */
    float coat_weight;
    v3    coat_color;
    float coat_roughness;
    float coat_ior;
    /* sheen */
    float sheen_weight;
    v3    sheen_color;
    float sheen_roughness;
    int   sheen_lobe_count;
    int   sheen_lobe_overflow;
    MtlxSheenLobe sheen_lobes[MTLX_MAX_SHEEN_LOBES];
    /* thin-film iridescence (Belcour-Barla airy reflectance over the specular) */
    float thin_film_weight;    /* blend of iridescent vs plain Fresnel [0,1] */
    float thin_film_thickness; /* film thickness in nanometers (0 = off) */
    float thin_film_ior;       /* refractive index of the film */
    /* emission */
    float emission;
    v3    emission_color;
    /* geometry */
    v3    normal;   /* world-space shading normal (normal-map applied) */
    float opacity;
} OpenPBRParams;

/* Preserve distinct phase functions in composed VDFs. Exceeding this bound
 * fails volume evaluation rather than silently merging different lobes. */
#define MTLX_MAX_VOLUME_SCATTERING_LOBES 64
typedef struct {
    v3 scattering;
    float anisotropy;
} MtlxVolumeScatteringLobe;
typedef struct {
    v3 absorption;
    v3 scattering;
    float anisotropy; /* legacy single-phase preview */
    v3 emission;
    MtlxVolumeScatteringLobe lobes[MTLX_MAX_VOLUME_SCATTERING_LOBES];
    int lobe_count;
    int lobe_overflow;
} MtlxVolumeParams;

typedef struct {
    const MtlxDoc *doc;
    TextureCache  *tex;
    /* shade point */
    float uv[2];
    /* UV footprint in screen-space. Zero/false preserves point evaluation. */
    float uv_dx[2], uv_dy[2];
    int has_uv_derivatives;
    v3    P;       /* world position (for position/noise nodes) */
    v3    dPdx, dPdy; /* world-position screen-space derivatives */
    int has_position_derivatives;
    v3    Ns;      /* shading normal (world) */
    v3    Ng;      /* geometric normal (world) */
    v3    dpdu, dpdv; /* tangent frame for normal mapping */
    v3    V;       /* normalized direction from the shade point to the camera */
    v3    dVdx, dVdy; /* view-direction screen-space derivatives */
    int has_view_derivatives;
    float time;    /* scene time in seconds */
    float frame;   /* authored/render frame number */
    /* Column-major affine transforms.  When has_space_transforms is zero,
     * named-space transform nodes use the identity transform. */
    float object_to_world[16], world_to_object[16];
    float world_to_view[16], view_to_world[16];
    int has_space_transforms;
    /* Optional renderer callback for arbitrary geompropvalue primvars. */
    int (*geomprop)(void *user,const char *name,MtlxType type,MtlxValue *out);
    void *geomprop_user;
    /* per-shade-point memo (size doc->nnode) */
    MtlxValue *memo;
    char      *memo_done;
} ShadeContext;

/* Set sane OpenPBR defaults. */
void openpbr_defaults(OpenPBRParams *p);

/* Evaluate a surface shader node into params. Returns 0 on success. */
int mtlx_eval_surface(ShadeContext *ctx, int surface_node, OpenPBRParams *out);

/* Evaluate a volume constructor; returns nonzero for unsupported closure or
 * connected value nodes, unresolved inputs, or exhausted graph work limits. */
int mtlx_eval_volume(ShadeContext *ctx, int volume_node,
                     MtlxVolumeParams *out);

/* Evaluate any node by id (resets the per-shade memo first). For tests. */
MtlxValue mtlx_eval_node_test(ShadeContext *ctx, int node_id);

#endif /* MTLXRENDER_MTLX_EVAL_H_ */
