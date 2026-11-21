/*
 * texture_stub.c - neutral stand-ins for the texture cache, so the evaluator
 * tests link without the image loaders. No test here samples a texture.
 */
#include "texture.h"

static void neutral(float out[4]) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }
int texcache_get(TextureCache *tc, const char *rel_path, int srgb) { (void)tc; (void)rel_path; (void)srgb; return -1; }
void texcache_sample(TextureCache *tc, int id, float u, float v, float out[4]) { (void)tc; (void)id; (void)u; (void)v; neutral(out); }
void texcache_sample_address(TextureCache *tc, int id, float u, float v, const char *ws, const char *wt, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)ws; (void)wt; neutral(out); }
void texcache_sample_address_filter(TextureCache *tc, int id, float u, float v, const char *ws, const char *wt, const char *filter, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)ws; (void)wt; (void)filter; neutral(out); }
void texcache_sample_cubic_address(TextureCache *tc, int id, float u, float v, const char *ws, const char *wt, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)ws; (void)wt; neutral(out); }
void texcache_sample_address_grad(TextureCache *tc, int id, float u, float v, float dudx, float dvdx, float dudy, float dvdy, const char *ws, const char *wt, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)dudx; (void)dvdx; (void)dudy; (void)dvdy; (void)ws; (void)wt; neutral(out); }
void texcache_sample_address_grad_filter(TextureCache *tc, int id, float u, float v, float dudx, float dvdx, float dudy, float dvdy, const char *ws, const char *wt, const char *filter, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)dudx; (void)dvdx; (void)dudy; (void)dvdy; (void)ws; (void)wt; (void)filter; neutral(out); }
void texcache_sample_nearest_address(TextureCache *tc, int id, float u, float v, const char *ws, const char *wt, float out[4]) { (void)tc; (void)id; (void)u; (void)v; (void)ws; (void)wt; neutral(out); }
int texcache_sample_file(TextureCache *tc, const char *path, int srgb, float u, float v, float out[4]) { (void)tc; (void)path; (void)srgb; (void)u; (void)v; neutral(out); return 0; }
int texcache_sample_file_filter(TextureCache *tc, const char *path, int srgb, float u, float v, const char *filter, float out[4]) { (void)tc; (void)path; (void)srgb; (void)u; (void)v; (void)filter; neutral(out); return 0; }
int texcache_sample_file_grad(TextureCache *tc, const char *path, int srgb, float u, float v, float dudx, float dvdx, float dudy, float dvdy, float out[4]) { (void)tc; (void)path; (void)srgb; (void)u; (void)v; (void)dudx; (void)dvdx; (void)dudy; (void)dvdy; neutral(out); return 0; }
int texcache_sample_file_grad_filter(TextureCache *tc, const char *path, int srgb, float u, float v, float dudx, float dvdx, float dudy, float dvdy, const char *filter, float out[4]) { (void)tc; (void)path; (void)srgb; (void)u; (void)v; (void)dudx; (void)dvdx; (void)dudy; (void)dvdy; (void)filter; neutral(out); return 0; }
int texcache_sample_file_nearest(TextureCache *tc, const char *path, int srgb, float u, float v, float out[4]) { (void)tc; (void)path; (void)srgb; (void)u; (void)v; neutral(out); return 0; }
