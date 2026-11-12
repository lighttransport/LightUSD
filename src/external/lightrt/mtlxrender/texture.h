/*
 * texture.h - JPG/PNG texture cache (stb_image), filtered sampling.
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

/* Opt-in counterpart that builds round-up, linear-RGBA mip pyramids for all
 * preloaded images before freezing the cache. Returns 0 if any chain could not
 * be built; images without a chain continue to use the bounded footprint path. */
int texcache_preload_mips(TextureCache *tc, const struct MtlxDoc *doc);

/* base_dir is prepended to relative texture paths (the .mtlx directory). */
TextureCache *texcache_create(const char *base_dir);
void texcache_free(TextureCache *tc);

/* Returns a texture id (>=0), loading on first use; -1 on failure.
 * `srgb` selects sRGB->linear decode for this logical use of the file. */
int texcache_get(TextureCache *tc, const char *rel_path, int srgb);

/* Build a round-up box-filtered mip pyramid in linear RGBA. Call before the
 * cache is frozen/rendered. Returns 1 on success (including an existing chain). */
int texcache_build_mips(TextureCache *tc, int id);

/* Bilinear sample at adjacent mip levels and blend by continuous LOD. LOD is
 * clamped to the available chain. If no chain was built, samples level zero. */
void texcache_sample_trilinear(TextureCache *tc, int id, float u, float v,
                               float lod, const char *wrap_s,
                               const char *wrap_t, float out[4]);

/* Select LOD as log2 of the larger screen-space footprint in texels, clamped
 * to zero. Use this with texcache_sample_trilinear for derivative selection. */
float texcache_lod_from_grad(TextureCache *tc, int id, float dudx, float dvdx,
                             float dudy, float dvdy);

/* Bilinear sample into out[4] (rgba, linear). Missing channels: rgb replicate
 * luminance for 1-channel, alpha defaults to 1. id<0 yields (0,0,0,1). */
void texcache_sample(TextureCache *tc, int id, float u, float v, float out[4]);

/* Address-aware variant. Modes are MaterialX names: periodic, clamp, mirror,
 * and constant. The evaluator applies a constant mode's node default before
 * sampling an out-of-range UV, since the cache does not know the node default. */
void texcache_sample_address(TextureCache *tc, int id, float u, float v,
                             const char *wrap_s, const char *wrap_t,
                             float out[4]);

/* MaterialX filter selector: closest, linear (bilinear), or cubic
 * (Catmull-Rom bicubic convolution). Unknown tokens fall back to linear. */
void texcache_sample_address_filter(TextureCache *tc, int id, float u, float v,
                                    const char *wrap_s, const char *wrap_t,
                                    const char *filtertype, float out[4]);
void texcache_sample_cubic_address(TextureCache *tc, int id, float u, float v,
                                   const char *wrap_s, const char *wrap_t,
                                   float out[4]);

/* Box-filter a regular image over the UV footprint described by the two
 * screen-space gradients. Small footprints use the ordinary bilinear path;
 * larger footprints are integrated with a bounded 4x4 tap grid. */
void texcache_sample_address_grad(TextureCache *tc, int id, float u, float v,
                                  float dudx, float dvdx, float dudy, float dvdy,
                                  const char *wrap_s, const char *wrap_t,
                                  float out[4]);
void texcache_sample_address_grad_filter(TextureCache *tc, int id, float u, float v,
                                         float dudx, float dvdx,
                                         float dudy, float dvdy,
                                         const char *wrap_s, const char *wrap_t,
                                         const char *filtertype, float out[4]);

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
int texcache_sample_file_filter(TextureCache *tc, const char *path, int srgb,
                                float u, float v, const char *filtertype,
                                float out[4]);
int texcache_sample_file_grad_filter(TextureCache *tc, const char *path,
                                     int srgb, float u, float v,
                                     float dudx, float dvdx, float dudy,
                                     float dvdy, const char *filtertype,
                                     float out[4]);

int texcache_sample_file_nearest(TextureCache *tc, const char *path, int srgb,
                                 float u, float v, float out[4]);

#endif /* MTLXRENDER_TEXTURE_H_ */
