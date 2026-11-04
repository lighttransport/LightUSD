/* SPDX-License-Identifier: Apache-2.0 */
/*
 * lptex-sample: stateless Ptex sampling math in C11.
 *
 * Edge adjacency (including the two-hop corner case), texel addressing,
 * bilinear page sampling, the anisotropic footprint (minor-axis mip plus
 * tent-weighted taps along the major axis), sRGB decode and half-to-float.
 * Pages come from a caller callback, so cache policy (LRU, locking) stays
 * with the caller. Only 8-bit pages are filtered, matching the CPU preview.
 */
#ifndef LPTEX_SAMPLE_H_
#define LPTEX_SAMPLE_H_

#include <stddef.h>
#include <stdint.h>

#include "lptex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lptex_footprint {
  float major;   /* largest UV extent per pixel */
  float minor;   /* smallest UV extent per pixel */
  float dir_u;   /* unit major direction in (u,v) */
  float dir_v;
  uint32_t mip;  /* selected from the minor axis */
  uint32_t taps; /* clamp(ceil(major/minor), 1, 8) */
} lptex_footprint;

float lptex_half_to_float(uint16_t h);
float lptex_srgb_to_linear(float c);

/* Mip whose texel size matches `uv_per_pixel` for this face. */
uint32_t lptex_choose_mip(const lptex_face_info* face, uint32_t levels,
                          float uv_per_pixel);

/* Footprint of an isotropic world-space pixel disk of radius
 * `world_footprint` under the world->UV gradients gu = du/dP, gv = dv/dP.
 * `face` NULL or levels == 0 yields the default footprint. */
void lptex_compute_uv_footprint(const lptex_face_info* face, uint32_t levels,
                                const float gu[3], const float gv[3],
                                float world_footprint, lptex_footprint* out);

/* Move a UV point that left the face across one edge using the Ptex
 * adjacency (PtexSeparableFilter::applyAcrossEdge rotation). Returns 0 when
 * there is no neighbour, the face uses subface adjacency, or (u,v) is
 * inside; the point is then unchanged. The other axis is left unclamped. */
int lptex_cross_edge(const lptex_face_info* faces, uint32_t nfaces,
                     uint32_t* face, float* u, float* v);

/* Up to two lptex_cross_edge() hops (edge then diagonal corner). Returns 1
 * when at least one hop happened. */
int lptex_resolve_tap(const lptex_face_info* faces, uint32_t nfaces,
                      uint32_t* face, float* u, float* v);

/* Clamped bilinear texel addressing inside one w x h page. */
typedef struct lptex_bilinear {
  int x0, y0, x1, y1;
  float tx, ty;
} lptex_bilinear;
void lptex_page_bilinear(uint32_t w, uint32_t h, float u, float v,
                         lptex_bilinear* out);

/* 8-bit page bilinear lookups; (u,v) clamped to the page. Channels beyond
 * the page replicate channel 0 for RGB. */
void lptex_sample_page_rgb(const lptex_image* page, float u, float v,
                           float out[3]);
void lptex_sample_page_channel(const lptex_image* page, float u, float v,
                               int channel, float* out);

/* Page provider: fill *page (borrowed until the next call) and return 1, or
 * return 0 when the page is unavailable. */
typedef int (*lptex_fetch_fn)(void* user, uint32_t face, uint32_t mip,
                              lptex_image* page);

typedef struct lptex_sample_source {
  const lptex_face_info* faces;
  uint32_t nfaces;
  uint32_t levels;
  lptex_fetch_fn fetch;
  void* user;
} lptex_sample_source;

/* Seam-aware bilinear sample at one mip: each of the four taps that leaves
 * the face follows the adjacency into the neighbour. Return 1 on success
 * (8-bit base page available). *cross_taps (optional) counts remapped taps. */
int lptex_sample_rgb(const lptex_sample_source* src, uint32_t face,
                     uint32_t mip, float u, float v, int srgb, float out[3],
                     uint64_t* cross_taps);
int lptex_sample_channel(const lptex_sample_source* src, uint32_t face,
                         uint32_t mip, float u, float v, int channel,
                         float* out, uint64_t* cross_taps);

/* Footprint sample. Isotropic footprints are one lptex_sample_*() call;
 * anisotropic ones take fp->taps (<= 8) taps one minor extent apart along
 * the major axis, tent-weighted. Returns 1 on success. *samples (optional)
 * receives the number of successful bilinear samples taken. */
int lptex_sample_footprint_rgb(const lptex_sample_source* src, uint32_t face,
                               const lptex_footprint* fp, float u, float v,
                               int srgb, float out[3], uint64_t* cross_taps,
                               uint32_t* samples);
int lptex_sample_footprint_channel(const lptex_sample_source* src,
                                   uint32_t face, const lptex_footprint* fp,
                                   float u, float v, int channel, float* out,
                                   uint64_t* cross_taps, uint32_t* samples);

/* Number of taps an anisotropic footprint sample takes (1 if isotropic). */
uint32_t lptex_footprint_taps(const lptex_footprint* fp);

#ifdef __cplusplus
}
#endif

#endif /* LPTEX_SAMPLE_H_ */
