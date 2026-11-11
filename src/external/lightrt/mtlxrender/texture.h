/*
 * texture.h - JPG/PNG texture cache (stb_image), bilinear sampling.
 *
 * Textures are loaded once per relative path and sampled in [0,1] UV with
 * periodic wrap. sRGB textures are linearized at sample time (rgb only).
 */
#ifndef MTLXRENDER_TEXTURE_H_
#define MTLXRENDER_TEXTURE_H_

struct MtlxDoc;

typedef struct TextureCache TextureCache;

/* Preload every texture referenced by image and projection nodes, then freeze
 * the cache so subsequent texcache_get calls are read-only (thread-safe during
 * rendering). Call once before render(). */
void texcache_preload(TextureCache *tc, const struct MtlxDoc *doc);

/* base_dir is prepended to relative texture paths (the .mtlx directory). */
TextureCache *texcache_create(const char *base_dir);
void texcache_free(TextureCache *tc);

/* Returns a texture id (>=0), loading on first use; -1 on failure.
 * `srgb` selects sRGB->linear decode for this logical use of the file. */
int texcache_get(TextureCache *tc, const char *rel_path, int srgb);

/* Bilinear sample into out[4] (rgba, linear). Missing channels: rgb replicate
 * luminance for 1-channel, alpha defaults to 1. id<0 yields (0,0,0,1). */
void texcache_sample(TextureCache *tc, int id, float u, float v, float out[4]);

/* Address-aware variant. Modes are MaterialX names: periodic, clamp, mirror,
 * and constant. The evaluator applies a constant mode's node default before
 * sampling an out-of-range UV, since the cache does not know the node default. */
void texcache_sample_address(TextureCache *tc, int id, float u, float v,
                             const char *wrap_s, const char *wrap_t,
                             float out[4]);

/* Box-filter a regular image over the UV footprint described by the two
 * screen-space gradients. Small footprints use the ordinary bilinear path;
 * larger footprints are integrated with a bounded 4x4 tap grid. */
void texcache_sample_address_grad(TextureCache *tc, int id, float u, float v,
                                  float dudx, float dvdx, float dudy, float dvdy,
                                  const char *wrap_s, const char *wrap_t,
                                  float out[4]);

/* Nearest-neighbor counterpart used by MaterialX image nodes that request
 * filtertype="closest". */
void texcache_sample_nearest_address(TextureCache *tc, int id, float u, float v,
                                    const char *wrap_s, const char *wrap_t,
                                    float out[4]);

/* Sample a regular or UDIM filename. A <UDIM> token selects the tile from
 * floor(u), floor(v); absent tiles return 0 and leave a neutral rgba value. */
int texcache_sample_file(TextureCache *tc, const char *path, int srgb,
                         float u, float v, float out[4]);

int texcache_sample_file_grad(TextureCache *tc, const char *path, int srgb,
                              float u, float v, float dudx, float dvdx,
                              float dudy, float dvdy, float out[4]);

int texcache_sample_file_nearest(TextureCache *tc, const char *path, int srgb,
                                 float u, float v, float out[4]);

#endif /* MTLXRENDER_TEXTURE_H_ */
