#include "texture.h"
#include "color.h"
#include "mtlx_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"

typedef struct {
    char         *key;     /* rel_path + srgb flag */
    unsigned char *pixels; /* 8-bit, comp channels */
    int           w, h, comp;
    int           srgb;
} Texture;

struct TextureCache {
    char    *base_dir;
    Texture *tex;
    int      ntex, cap;
    int      frozen; /* when set, texcache_get never mutates (render-safe) */
};

TextureCache *texcache_create(const char *base_dir) {
    TextureCache *tc = (TextureCache *)calloc(1, sizeof(TextureCache));
    tc->base_dir = strdup(base_dir ? base_dir : ".");
    return tc;
}

void texcache_free(TextureCache *tc) {
    if (!tc) return;
    for (int i = 0; i < tc->ntex; i++) {
        free(tc->tex[i].key);
        stbi_image_free(tc->tex[i].pixels);
    }
    free(tc->tex);
    free(tc->base_dir);
    free(tc);
}

/* Resolve a texture filename the way MaterialX does: it is given relative to a
 * search path, not just the document directory. Example materials reference
 * images by bare name (e.g. "brick_normal.jpg") that live in resources/Images/,
 * several levels above the .mtlx. Walk up from base_dir and try both <dir>/<rel>
 * and <dir>/Images/<rel> at each ancestor level. */
static unsigned char *load_searchpath(const char *base_dir, const char *rel,
                                      int *w, int *h, int *comp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", (base_dir && base_dir[0]) ? base_dir : ".");
    for (int up = 0; up < 12; up++) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/%s", dir, rel);
        unsigned char *px = stbi_load(path, w, h, comp, 0);
        if (px) return px;
        snprintf(path, sizeof(path), "%s/Images/%s", dir, rel);
        px = stbi_load(path, w, h, comp, 0);
        if (px) return px;
        char *slash = strrchr(dir, '/');
        if (!slash) break;
        *slash = '\0';
        if (!dir[0]) break;
    }
    return NULL;
}

static int texcache_add(TextureCache *tc, const char *rel_path, int srgb,
                        unsigned char *px, int w, int h, int comp) {
    if (tc->ntex >= tc->cap) {
        tc->cap = tc->cap ? tc->cap * 2 : 16;
        tc->tex = (Texture *)realloc(tc->tex, sizeof(Texture) * tc->cap);
    }
    Texture *t = &tc->tex[tc->ntex];
    char key[1100];
    snprintf(key, sizeof(key), "%s|%d", rel_path, srgb ? 1 : 0);
    t->key = strdup(key);
    t->pixels = px;
    t->w = w; t->h = h; t->comp = comp; t->srgb = srgb;
    return tc->ntex++;
}

int texcache_get(TextureCache *tc, const char *rel_path, int srgb) {
    if (!rel_path || !rel_path[0]) return -1;
    char key[1100];
    snprintf(key, sizeof(key), "%s|%d", rel_path, srgb ? 1 : 0);
    for (int i = 0; i < tc->ntex; i++)
        if (strcmp(tc->tex[i].key, key) == 0) return i;

    if (tc->frozen) return -1; /* render-time miss: never mutate the cache */

    int w, h, comp;
    unsigned char *px = load_searchpath(tc->base_dir, rel_path, &w, &h, &comp);
    if (!px) { fprintf(stderr, "texture: failed to load '%s' (searched from '%s')\n", rel_path, tc->base_dir); return -1; }

    return texcache_add(tc, rel_path, srgb, px, w, h, comp);
}

static int udim_path(const char *pattern, float u, float v, char *path,
                     size_t path_size) {
    const char *token = strstr(pattern, "<UDIM>");
    if (!token) {
        snprintf(path, path_size, "%s", pattern);
        return 1001;
    }
    int tile = 1001 + (int)floorf(u) + 10 * (int)floorf(v);
    if (tile < 1001 || tile > 1999) return -1;
    size_t prefix = (size_t)(token - pattern);
    size_t suffix = strlen(token + 6);
    if (prefix + 4 + suffix + 1 > path_size) return -1;
    memcpy(path, pattern, prefix);
    snprintf(path + prefix, path_size - prefix, "%d%s", tile, token + 6);
    return tile;
}

int texcache_sample_file(TextureCache *tc, const char *path, int srgb,
                         float u, float v, float out[4]) {
    return texcache_sample_file_filter(tc, path, srgb, u, v, "linear", out);
}

int texcache_sample_file_filter(TextureCache *tc, const char *path, int srgb,
                                float u, float v, const char *filtertype,
                                float out[4]) {
    if (!tc || !path || !path[0] || !out) return 0;
    char resolved[1024];
    if (udim_path(path, u, v, resolved, sizeof(resolved)) < 0) return 0;
    int id = texcache_get(tc, resolved, srgb);
    if (id < 0) return 0;
    texcache_sample_address_filter(tc, id, u - floorf(u), v - floorf(v),
                                   "periodic", "periodic", filtertype, out);
    return 1;
}

int texcache_sample_file_grad(TextureCache *tc, const char *path, int srgb,
                              float u, float v, float dudx, float dvdx,
                              float dudy, float dvdy, float out[4]) {
    return texcache_sample_file_grad_filter(tc, path, srgb, u, v, dudx, dvdx,
                                            dudy, dvdy, "linear", out);
}

int texcache_sample_file_grad_filter(TextureCache *tc, const char *path,
                                     int srgb, float u, float v,
                                     float dudx, float dvdx, float dudy,
                                     float dvdy, const char *filtertype,
                                     float out[4]) {
    if (!tc || !path || !path[0] || !out) return 0;
    const char *udim = strstr(path, "<UDIM>");
    if (udim) {
        /* A footprint crossing a tile boundary must resolve each tap against
         * its own tile. Sampling one preselected tile would wrap the seam and
         * duplicate the wrong texels. */
        const float half_u = 0.5f * (fabsf(dudx) + fabsf(dudy));
        const float half_v = 0.5f * (fabsf(dvdx) + fabsf(dvdy));
        const int crosses_u = (int)floorf(u - half_u) !=
                              (int)floorf(u + half_u);
        const int crosses_v = (int)floorf(v - half_v) !=
                              (int)floorf(v + half_v);
        if (crosses_u || crosses_v) {
            float sum[4] = {0, 0, 0, 0};
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                const float ax = ((float)x + 0.5f) * 0.25f - 0.5f;
                const float ay = ((float)y + 0.5f) * 0.25f - 0.5f;
                float sample[4];
                if (!texcache_sample_file_filter(tc, path, srgb,
                                          u + ax * dudx + ay * dudy,
                                          v + ax * dvdx + ay * dvdy,
                                          filtertype, sample)) {
                    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
                    return 0;
                }
                for (int c = 0; c < 4; ++c) sum[c] += sample[c] * 0.0625f;
            }
            for (int c = 0; c < 4; ++c) out[c] = sum[c];
            return 1;
        }
    }
    char resolved[1024];
    if (udim_path(path, u, v, resolved, sizeof(resolved)) < 0) return 0;
    int id = texcache_get(tc, resolved, srgb);
    if (id < 0) return 0;
    texcache_sample_address_grad_filter(tc, id, u - floorf(u), v - floorf(v),
                                        dudx, dvdx, dudy, dvdy,
                                        "periodic", "periodic", filtertype, out);
    return 1;
}

void texcache_preload(TextureCache *tc, const MtlxDoc *doc) {
    for (int i = 0; i < doc->nnode; i++) {
        const MtlxNode *n = &doc->nodes[i];
        /* match every image variant the evaluator treats as OP_IMAGE, else its
         * file never enters the cache and the frozen render-time lookup misses. */
        const char *c = n->category;
        if (strcmp(c, "image") && strcmp(c, "tiledimage") && strcmp(c, "hextiledimage") &&
            strcmp(c, "gltf_image") && strcmp(c, "gltf_colorimage") &&
            strcmp(c, "normalmap") && strcmp(c, "gltf_normalmap") &&
            strcmp(c, "latlongimage") && strcmp(c, "triplanarprojection")) continue;
        const char *file_names[3] = {"file", NULL, NULL};
        int file_count = 1;
        if (!strcmp(c, "triplanarprojection")) {
            file_names[0] = "filex";
            file_names[1] = "filey";
            file_names[2] = "filez";
            file_count = 3;
        }
        for (int j = 0; j < n->ninput; j++) {
            const MtlxInput *in = &n->inputs[j];
            int is_file = 0;
            for (int k = 0; k < file_count; k++)
                if (!strcmp(in->name, file_names[k])) is_file = 1;
            if (!is_file || !in->value.s) continue;
            const char *token = strstr(in->value.s, "<UDIM>");
            if (!token) {
                texcache_get(tc, in->value.s, in->colorspace_srgb);
                continue;
            }
            /* MaterialX UDIM addressing permits tiles 1001 through 1999.
             * Probe the complete bounded range before freezing so a valid
             * higher-row tile cannot become a render-time cache miss; absent
             * tiles are still probed silently and use the node default. */
            for (int tile = 1001; tile <= 1999; ++tile) {
                char path[1024];
                size_t prefix = (size_t)(token - in->value.s);
                size_t suffix = strlen(token + 6);
                if (prefix + 4 + suffix + 1 > sizeof(path)) break;
                memcpy(path, in->value.s, prefix);
                snprintf(path + prefix, sizeof(path) - prefix, "%d%s", tile,
                         token + 6);
                int w, h, comp;
                unsigned char *px = load_searchpath(tc->base_dir, path, &w, &h,
                                                    &comp);
                if (px) texcache_add(tc, path, in->colorspace_srgb, px, w, h,
                                     comp);
            }
        }
    }
    tc->frozen = 1;
    fprintf(stderr, "texture: preloaded %d textures\n", tc->ntex);
}

static int wrap(int x, int n) {
    x %= n;
    if (x < 0) x += n;
    return x;
}

static int address_index(int x, int n, const char *mode, int *outside) {
    *outside = 0;
    if (!mode || !strcmp(mode, "periodic")) return wrap(x, n);
    if (!strcmp(mode, "clamp")) return x < 0 ? 0 : (x >= n ? n - 1 : x);
    if (!strcmp(mode, "mirror")) {
        const int period = n * 2;
        int p = x % period;
        if (p < 0) p += period;
        return p < n ? p : period - 1 - p;
    }
    if (!strcmp(mode, "constant")) {
        /* Out-of-range UVs are handled by the MaterialX image evaluator,
         * which has access to the node's typed default. Filter taps at an
         * in-range edge clamp to the edge texel. */
        return x < 0 ? 0 : (x >= n ? n - 1 : x);
    }
    return wrap(x, n);
}

static void fetch_texel(const Texture *t, int x, int y, float out[4]) {
    x = wrap(x, t->w);
    y = wrap(y, t->h);
    const unsigned char *p = t->pixels + (size_t)(y * t->w + x) * t->comp;
    float r, g, b, a = 1.0f;
    if (t->comp >= 3) { r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f; if (t->comp == 4) a = p[3] / 255.0f; }
    else { r = g = b = p[0] / 255.0f; if (t->comp == 2) a = p[1] / 255.0f; }
    if (t->srgb) { r = srgb_to_linear_f(r); g = srgb_to_linear_f(g); b = srgb_to_linear_f(b); }
    out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

void texcache_sample_address(TextureCache *tc, int id, float u, float v,
                             const char *wrap_s, const char *wrap_t,
                             float out[4]) {
    if (id < 0 || id >= tc->ntex) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }
    const Texture *t = &tc->tex[id];
    /* flip V to match image-space (OpenEXR/glTF: V grows downward in texel space) */
    float fu = u * t->w - 0.5f;
    float fv = (1.0f - v) * t->h - 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    float dx = fu - x0, dy = fv - y0;
    float c00[4], c10[4], c01[4], c11[4];
    int ox0, ox1, oy0, oy1;
    int xx0 = address_index(x0, t->w, wrap_s, &ox0);
    int xx1 = address_index(x0 + 1, t->w, wrap_s, &ox1);
    int yy0 = address_index(y0, t->h, wrap_t, &oy0);
    int yy1 = address_index(y0 + 1, t->h, wrap_t, &oy1);
    if (ox0 || ox1 || oy0 || oy1) {
        for (int c = 0; c < 4; c++) out[c] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    fetch_texel(t, xx0, yy0, c00);
    fetch_texel(t, xx1, yy0, c10);
    fetch_texel(t, xx0, yy1, c01);
    fetch_texel(t, xx1, yy1, c11);
    for (int c = 0; c < 4; c++) {
        float a = c00[c] * (1 - dx) + c10[c] * dx;
        float b = c01[c] * (1 - dx) + c11[c] * dx;
        out[c] = a * (1 - dy) + b * dy;
    }
}

static float catmull_rom_weight(float x) {
    x = fabsf(x);
    if (x < 1.0f) return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    if (x < 2.0f) return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    return 0.0f;
}

void texcache_sample_cubic_address(TextureCache *tc, int id, float u, float v,
                                   const char *wrap_s, const char *wrap_t,
                                   float out[4]) {
    if (id < 0 || id >= tc->ntex) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return;
    }
    const Texture *t = &tc->tex[id];
    const float fu = u * t->w - 0.5f;
    const float fv = (1.0f - v) * t->h - 0.5f;
    const int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    const float dx = fu - (float)x0, dy = fv - (float)y0;
    const float wx[4] = {catmull_rom_weight(1.0f + dx),
                         catmull_rom_weight(dx),
                         catmull_rom_weight(1.0f - dx),
                         catmull_rom_weight(2.0f - dx)};
    const float wy[4] = {catmull_rom_weight(1.0f + dy),
                         catmull_rom_weight(dy),
                         catmull_rom_weight(1.0f - dy),
                         catmull_rom_weight(2.0f - dy)};
    float sum[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
        int outside_x, outside_y;
        const int x = address_index(x0 + i - 1, t->w, wrap_s, &outside_x);
        const int y = address_index(y0 + j - 1, t->h, wrap_t, &outside_y);
        float texel[4];
        fetch_texel(t, x, y, texel);
        const float weight = wx[i] * wy[j];
        for (int c = 0; c < 4; ++c) sum[c] += texel[c] * weight;
    }
    for (int c = 0; c < 4; ++c) out[c] = sum[c];
}

void texcache_sample_address_filter(TextureCache *tc, int id, float u, float v,
                                    const char *wrap_s, const char *wrap_t,
                                    const char *filtertype, float out[4]) {
    if (filtertype && !strcmp(filtertype, "closest"))
        texcache_sample_nearest_address(tc, id, u, v, wrap_s, wrap_t, out);
    else if (filtertype && !strcmp(filtertype, "cubic"))
        texcache_sample_cubic_address(tc, id, u, v, wrap_s, wrap_t, out);
    else
        texcache_sample_address(tc, id, u, v, wrap_s, wrap_t, out);
}

void texcache_sample_address_grad(TextureCache *tc, int id, float u, float v,
                                  float dudx, float dvdx, float dudy, float dvdy,
                                  const char *wrap_s, const char *wrap_t,
                                  float out[4]) {
    texcache_sample_address_grad_filter(tc, id, u, v, dudx, dvdx, dudy, dvdy,
                                        wrap_s, wrap_t, "linear", out);
}

void texcache_sample_address_grad_filter(TextureCache *tc, int id, float u, float v,
                                         float dudx, float dvdx,
                                         float dudy, float dvdy,
                                         const char *wrap_s, const char *wrap_t,
                                         const char *filtertype, float out[4]) {
    float span_u = fmaxf(fabsf(dudx), fabsf(dudy));
    float span_v = fmaxf(fabsf(dvdx), fabsf(dvdy));
    if (span_u * (id >= 0 && id < tc->ntex ? tc->tex[id].w : 0) <= 1.0f &&
        span_v * (id >= 0 && id < tc->ntex ? tc->tex[id].h : 0) <= 1.0f) {
        texcache_sample_address_filter(tc, id, u, v, wrap_s, wrap_t,
                                       filtertype, out);
        return;
    }
    if (id < 0 || id >= tc->ntex) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return;
    }
    /* Integrate the parallelogram footprint with a fixed, bounded grid. */
    float sum[4] = {0, 0, 0, 0};
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        float ax = ((float)x + 0.5f) * 0.25f - 0.5f;
        float ay = ((float)y + 0.5f) * 0.25f - 0.5f;
        float sample[4];
        texcache_sample_address_filter(tc, id, u + ax * dudx + ay * dudy,
                                       v + ax * dvdx + ay * dvdy,
                                       wrap_s, wrap_t, filtertype, sample);
        for (int c = 0; c < 4; ++c) sum[c] += sample[c] * 0.0625f;
    }
    for (int c = 0; c < 4; ++c) out[c] = sum[c];
}

void texcache_sample(TextureCache *tc, int id, float u, float v, float out[4]) {
    texcache_sample_address(tc, id, u, v, "periodic", "periodic", out);
}

void texcache_sample_nearest_address(TextureCache *tc, int id, float u, float v,
                                     const char *wrap_s, const char *wrap_t,
                                     float out[4]) {
    if (id < 0 || id >= tc->ntex) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return;
    }
    const Texture *t = &tc->tex[id];
    /* Texture coordinates address texel centers: floor(u*w) selects the
     * nearest texel for the half-open [0,1) interval. V is image-downward. */
    int x = (int)floorf(u * t->w);
    int y = (int)floorf((1.0f - v) * t->h);
    int ox, oy;
    x = address_index(x, t->w, wrap_s, &ox);
    y = address_index(y, t->h, wrap_t, &oy);
    if (ox || oy) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return;
    }
    fetch_texel(t, x, y, out);
}

int texcache_sample_file_nearest(TextureCache *tc, const char *path, int srgb,
                                 float u, float v, float out[4]) {
    return texcache_sample_file_filter(tc, path, srgb, u, v, "closest", out);
}
