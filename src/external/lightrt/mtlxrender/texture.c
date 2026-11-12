#include "texture.h"
#include "color.h"
#include "mtlx_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#ifdef LIGHTUSD_MTLX_STB_STATIC
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#endif
#include "stb_image.h"
#ifdef LIGHTUSD_MTLX_STB_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

typedef struct {
    int w, h;
    float *rgba; /* linear, interleaved RGBA */
} TextureMip;

typedef struct {
    char         *key;     /* rel_path + srgb flag */
    unsigned char *pixels; /* 8-bit, comp channels */
    int           w, h, comp;
    int           srgb;
    TextureMip   *mips;
    int           nmips;
} Texture;

struct TextureCache {
    char    *base_dir;
    Texture *tex;
    int      ntex, cap;
    int      frozen; /* when set, texcache_get never mutates (render-safe) */
    MtlxTextureReadFn read_fn;
    void *read_user;
    size_t max_decoded_bytes, decoded_bytes;
    int load_failed;
    int auto_mips;
};

TextureCache *texcache_create(const char *base_dir) {
    TextureCache *tc = (TextureCache *)calloc(1, sizeof(TextureCache));
    if (!tc) return NULL;
    tc->base_dir = strdup(base_dir ? base_dir : ".");
    if (!tc->base_dir) { free(tc); return NULL; }
    return tc;
}

void texcache_set_reader(TextureCache *tc, MtlxTextureReadFn read_fn,
                         void *user, size_t max_decoded_bytes) {
    if (!tc) return;
    tc->read_fn = read_fn;
    tc->read_user = user;
    tc->max_decoded_bytes = max_decoded_bytes;
}

int texcache_had_load_failure(const TextureCache *tc) {
    return tc ? tc->load_failed : 1;
}

void texcache_clear_load_failure(TextureCache *tc) {
    if (tc) tc->load_failed = 0;
}

void texcache_set_auto_mips(TextureCache *tc, int enabled) {
    if (tc) tc->auto_mips = enabled != 0;
}

void texcache_free(TextureCache *tc) {
    if (!tc) return;
    for (int i = 0; i < tc->ntex; i++) {
        free(tc->tex[i].key);
        stbi_image_free(tc->tex[i].pixels);
        for (int level = 0; level < tc->tex[i].nmips; ++level)
            free(tc->tex[i].mips[level].rgba);
        free(tc->tex[i].mips);
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
        if (tc->cap > INT_MAX / 2) { stbi_image_free(px); tc->load_failed = 1; return -1; }
        const int new_cap = tc->cap ? tc->cap * 2 : 16;
        if ((size_t)new_cap > SIZE_MAX / sizeof(Texture)) {
            stbi_image_free(px); tc->load_failed = 1; return -1;
        }
        Texture *new_tex = (Texture *)realloc(tc->tex, sizeof(Texture) * (size_t)new_cap);
        if (!new_tex) { stbi_image_free(px); tc->load_failed = 1; return -1; }
        tc->tex = new_tex;
        tc->cap = new_cap;
    }
    Texture *t = &tc->tex[tc->ntex];
    char key[1100];
    snprintf(key, sizeof(key), "%s|%d", rel_path, srgb ? 1 : 0);
    t->key = strdup(key);
    if (!t->key) { stbi_image_free(px); tc->load_failed = 1; return -1; }
    t->pixels = px;
    t->w = w; t->h = h; t->comp = comp; t->srgb = srgb;
    t->mips = NULL; t->nmips = 0;
    return tc->ntex++;
}

int texcache_get(TextureCache *tc, const char *rel_path, int srgb) {
    if (!rel_path || !rel_path[0]) return -1;
    char key[1100];
    snprintf(key, sizeof(key), "%s|%d", rel_path, srgb ? 1 : 0);
    for (int i = 0; i < tc->ntex; i++) {
        if (strcmp(tc->tex[i].key, key) == 0) {
            if (tc->auto_mips && !texcache_build_mips(tc, i)) {
                tc->load_failed = 1;
                return -1;
            }
            return i;
        }
    }

    if (tc->frozen) return -1; /* render-time miss: never mutate the cache */

    int w, h, comp;
    unsigned char *px = NULL;
    if (tc->read_fn) {
        const unsigned char *bytes = NULL;
        size_t size = 0;
        if (tc->read_fn(tc->read_user, rel_path, &bytes, &size) && bytes &&
            size <= INT_MAX && stbi_info_from_memory(bytes, (int)size, &w, &h, &comp) &&
            w > 0 && h > 0 && tc->decoded_bytes <= tc->max_decoded_bytes &&
            (size_t)w <= (tc->max_decoded_bytes - tc->decoded_bytes) / 4 / (size_t)h) {
            px = stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 0);
            if (px) tc->decoded_bytes += (size_t)w * (size_t)h * 4;
        }
    } else {
        px = load_searchpath(tc->base_dir, rel_path, &w, &h, &comp);
    }
    if (!px) { tc->load_failed = 1; fprintf(stderr, "texture: failed to load '%s' (searched from '%s')\n", rel_path, tc->base_dir); return -1; }

    const int id = texcache_add(tc, rel_path, srgb, px, w, h, comp);
    if (id < 0 && tc->read_fn)
        tc->decoded_bytes -= (size_t)w * (size_t)h * 4;
    if (id >= 0 && tc->auto_mips && !texcache_build_mips(tc, id)) {
        tc->load_failed = 1;
        return -1;
    }
    return id;
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

static int texcache_preload_impl(TextureCache *tc, const MtlxDoc *doc,
                                int build_mips) {
    if (!tc || !doc) return 0;
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
    int ok = 1;
    if (build_mips) {
        for (int i = 0; i < tc->ntex; ++i) {
            if (!texcache_build_mips(tc, i)) {
                fprintf(stderr, "texture: failed to build mip chain for '%s'\n",
                        tc->tex[i].key ? tc->tex[i].key : "<unknown>");
                ok = 0;
            }
        }
    }
    tc->frozen = 1;
    fprintf(stderr, "texture: preloaded %d textures\n", tc->ntex);
    return ok;
}

void texcache_preload(TextureCache *tc, const MtlxDoc *doc) {
    (void)texcache_preload_impl(tc, doc, 0);
}

int texcache_preload_mips(TextureCache *tc, const MtlxDoc *doc) {
    return texcache_preload_impl(tc, doc, 1);
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

static float *alloc_mip_pixels(int w, int h) {
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h) return NULL;
    const size_t texels = (size_t)w * (size_t)h;
    if (texels > SIZE_MAX / (4 * sizeof(float))) return NULL;
    return (float *)calloc(texels * 4, sizeof(float));
}

int texcache_build_mips(TextureCache *tc, int id) {
    if (!tc || id < 0 || id >= tc->ntex) return 0;
    Texture *t = &tc->tex[id];
    if (t->mips && t->nmips > 0) return 1;
    if (tc->frozen) return 0;
    if (t->w <= 0 || t->h <= 0) return 0;

    int count = 1, w = t->w, h = t->h;
    size_t mip_bytes = 0;
    for (;;) {
        if ((size_t)w > SIZE_MAX / (size_t)h / (4 * sizeof(float))) return 0;
        const size_t level_bytes = (size_t)w * (size_t)h * 4 * sizeof(float);
        if (level_bytes > SIZE_MAX - mip_bytes) return 0;
        mip_bytes += level_bytes;
        if (w == 1 && h == 1) break;
        w = w / 2 + w % 2;
        h = h / 2 + h % 2;
    }
    if (tc->read_fn && (tc->decoded_bytes > tc->max_decoded_bytes ||
        mip_bytes > tc->max_decoded_bytes - tc->decoded_bytes)) return 0;
    w = t->w; h = t->h;
    while (w > 1 || h > 1) {
        w = w / 2 + w % 2;
        h = h / 2 + h % 2;
        if (count == INT32_MAX) return 0;
        ++count;
    }
    TextureMip *mips = (TextureMip *)calloc((size_t)count, sizeof(TextureMip));
    if (!mips) return 0;
    mips[0].w = t->w;
    mips[0].h = t->h;
    mips[0].rgba = alloc_mip_pixels(t->w, t->h);
    if (!mips[0].rgba) { free(mips); return 0; }
    for (int y = 0; y < t->h; ++y) for (int x = 0; x < t->w; ++x) {
        float color[4];
        fetch_texel(t, x, y, color);
        const size_t offset = ((size_t)y * (size_t)t->w + (size_t)x) * 4;
        for (int c = 0; c < 4; ++c) mips[0].rgba[offset + (size_t)c] = color[c];
    }

    for (int level = 1; level < count; ++level) {
        const TextureMip *src = &mips[level - 1];
        TextureMip *dst = &mips[level];
        dst->w = src->w / 2 + src->w % 2;
        dst->h = src->h / 2 + src->h % 2;
        dst->rgba = alloc_mip_pixels(dst->w, dst->h);
        if (!dst->rgba) {
            for (int free_level = 0; free_level < level; ++free_level)
                free(mips[free_level].rgba);
            free(mips);
            return 0;
        }
        for (int y = 0; y < dst->h; ++y) for (int x = 0; x < dst->w; ++x) {
            const int x0 = (int)((size_t)x * 2);
            const int y0 = (int)((size_t)y * 2);
            const int x1 = x0 + 1 < src->w ? x0 + 1 : src->w - 1;
            const int y1 = y0 + 1 < src->h ? y0 + 1 : src->h - 1;
            const size_t dst_offset = ((size_t)y * (size_t)dst->w + (size_t)x) * 4;
            const size_t offsets[4] = {
                ((size_t)y0 * (size_t)src->w + (size_t)x0) * 4,
                ((size_t)y0 * (size_t)src->w + (size_t)x1) * 4,
                ((size_t)y1 * (size_t)src->w + (size_t)x0) * 4,
                ((size_t)y1 * (size_t)src->w + (size_t)x1) * 4,
            };
            for (int c = 0; c < 4; ++c) {
                dst->rgba[dst_offset + (size_t)c] =
                    (src->rgba[offsets[0] + (size_t)c] +
                     src->rgba[offsets[1] + (size_t)c] +
                     src->rgba[offsets[2] + (size_t)c] +
                     src->rgba[offsets[3] + (size_t)c]) * 0.25f;
            }
        }
    }
    t->mips = mips;
    t->nmips = count;
    if (tc->read_fn) tc->decoded_bytes += mip_bytes;
    return 1;
}

static void sample_mip_level(const TextureMip *mip, float u, float v,
                             const char *wrap_s, const char *wrap_t,
                             float out[4]) {
    const float fu = u * mip->w - 0.5f;
    const float fv = (1.0f - v) * mip->h - 0.5f;
    const int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    const float dx = fu - (float)x0, dy = fv - (float)y0;
    int ox0, ox1, oy0, oy1;
    const int xx0 = address_index(x0, mip->w, wrap_s, &ox0);
    const int xx1 = address_index(x0 + 1, mip->w, wrap_s, &ox1);
    const int yy0 = address_index(y0, mip->h, wrap_t, &oy0);
    const int yy1 = address_index(y0 + 1, mip->h, wrap_t, &oy1);
    if (ox0 || ox1 || oy0 || oy1) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return;
    }
    const size_t offsets[4] = {
        ((size_t)yy0 * (size_t)mip->w + (size_t)xx0) * 4,
        ((size_t)yy0 * (size_t)mip->w + (size_t)xx1) * 4,
        ((size_t)yy1 * (size_t)mip->w + (size_t)xx0) * 4,
        ((size_t)yy1 * (size_t)mip->w + (size_t)xx1) * 4,
    };
    for (int c = 0; c < 4; ++c) {
        const float a = mip->rgba[offsets[0] + (size_t)c] * (1 - dx) +
                        mip->rgba[offsets[1] + (size_t)c] * dx;
        const float b = mip->rgba[offsets[2] + (size_t)c] * (1 - dx) +
                        mip->rgba[offsets[3] + (size_t)c] * dx;
        out[c] = a * (1 - dy) + b * dy;
    }
}

void texcache_sample_trilinear(TextureCache *tc, int id, float u, float v,
                               float lod, const char *wrap_s,
                               const char *wrap_t, float out[4]) {
    if (!tc || id < 0 || id >= tc->ntex) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return;
    }
    const Texture *t = &tc->tex[id];
    if (!t->mips || t->nmips <= 0) {
        texcache_sample_address(tc, id, u, v, wrap_s, wrap_t, out);
        return;
    }
    if (!isfinite(lod)) lod = 0.0f;
    lod = fmaxf(0.0f, fminf(lod, (float)(t->nmips - 1)));
    const int low = (int)floorf(lod);
    const int high = low + 1 < t->nmips ? low + 1 : low;
    const float blend = lod - (float)low;
    float a[4], b[4];
    sample_mip_level(&t->mips[low], u, v, wrap_s, wrap_t, a);
    sample_mip_level(&t->mips[high], u, v, wrap_s, wrap_t, b);
    for (int c = 0; c < 4; ++c) out[c] = a[c] * (1 - blend) + b[c] * blend;
}

float texcache_lod_from_grad(TextureCache *tc, int id, float dudx, float dvdx,
                             float dudy, float dvdy) {
    if (!tc || id < 0 || id >= tc->ntex) return 0.0f;
    const Texture *t = &tc->tex[id];
    const float dux = dudx * (float)t->w, dvx = dvdx * (float)t->h;
    const float duy = dudy * (float)t->w, dvy = dvdy * (float)t->h;
    const float rho_x = sqrtf(dux * dux + dvx * dvx);
    const float rho_y = sqrtf(duy * duy + dvy * dvy);
    const float rho = fmaxf(rho_x, rho_y);
    return rho > 1.0f ? log2f(rho) : 0.0f;
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
    if (!out) return;
    if (!tc || id < 0 || id >= tc->ntex) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return;
    }
    const Texture *t = &tc->tex[id];
    float span_u = fmaxf(fabsf(dudx), fabsf(dudy));
    float span_v = fmaxf(fabsf(dvdx), fabsf(dvdy));
    if (span_u * t->w <= 1.0f && span_v * t->h <= 1.0f) {
        texcache_sample_address_filter(tc, id, u, v, wrap_s, wrap_t,
                                       filtertype, out);
        return;
    }
    /* The opt-in mip path uses trilinear bilinear sampling for minification.
     * Point filtering stays on the requested base-level filter. */
    if (t->mips && t->nmips > 0 &&
        (!filtertype || strcmp(filtertype, "closest") != 0)) {
        const float lod = texcache_lod_from_grad(tc, id, dudx, dvdx,
                                                 dudy, dvdy);
        texcache_sample_trilinear(tc, id, u, v, lod, wrap_s, wrap_t, out);
        return;
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
