/* SPDX-License-Identifier: Apache-2.0 */
/* libFuzzer harness for the vendored tptex Ptex library: container parsing
 * (lazy and eager), face decode at stored and reduced resolutions, metadata,
 * seam-aware sampling and every PtexFilter equivalent. Built with
 * LIGHTUSD_BUILD_FUZZERS=ON (clang). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "external/textools/ptex/include/tptex.h"
#include "external/textools/ptex/include/tptex_filter.h"
#include "external/textools/ptex/include/tptex_sample.h"

#define FUZZ_MAX_BYTES ((uint64_t)1 << 20)
#define FUZZ_MAX_FACES 24

typedef struct fuzz_pages {
  tptex_texture* tex;
  const tptex_info* info;
  uint8_t* buf;
  size_t cap;
} fuzz_pages;

/* Page provider for the sampler: face at half resolution `mip` times. */
static int fetch(void* user, uint32_t face, uint32_t mip, tptex_image* out) {
  fuzz_pages* p = (fuzz_pages*)user;
  tptex_face_info fi;
  int ul, vl;
  uint64_t bytes;
  if (tptex_get_face_info(p->tex, (int)face, &fi) != TPTEX_OK) return 0;
  ul = fi.res.ulog2 > (int)mip ? fi.res.ulog2 - (int)mip : 0;
  vl = fi.res.vlog2 > (int)mip ? fi.res.vlog2 - (int)mip : 0;
  bytes = ((uint64_t)tptex_pixel_size(p->tex)) << (ul + vl);
  if (bytes > FUZZ_MAX_BYTES) return 0;
  if (bytes > p->cap) {
    free(p->buf);
    p->buf = (uint8_t*)malloc((size_t)bytes);
    p->cap = p->buf ? (size_t)bytes : 0;
    if (!p->buf) return 0;
  }
  if (tptex_get_data(p->tex, (int)face, tptex_make_res(ul, vl), p->buf, 0) != TPTEX_OK) return 0;
  out->width = 1u << ul;
  out->height = 1u << vl;
  out->channels = (uint16_t)p->info->num_channels;
  out->type = p->info->data_type;
  out->data = p->buf;
  out->size = (size_t)bytes;
  return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  tptex_options opt;
  tptex_texture* t = NULL;
  const tptex_info* info;
  fuzz_pages pages;
  int face, nfaces, ft, level, count, i;
  tptex_options_init(&opt);
  opt.limits.max_block_bytes = FUZZ_MAX_BYTES;
  opt.limits.max_meta_bytes = FUZZ_MAX_BYTES;
  opt.limits.max_faces = 1u << 20;
  if (size && (data[0] & 1)) opt.flags |= TPTEX_OPEN_NO_LAZY;
  if (tptex_open_memory(data, size, &opt, &t) != TPTEX_OK) return 0;
  info = tptex_get_info(t);
  nfaces = info->num_faces < FUZZ_MAX_FACES ? info->num_faces : FUZZ_MAX_FACES;
  memset(&pages, 0, sizeof pages);
  pages.tex = t;
  pages.info = info;

  for (face = 0; face < nfaces; ++face) {
    tptex_face_info fi;
    if (tptex_get_face_info(t, face, &fi) != TPTEX_OK) continue;
    for (level = 0; level < 4; ++level) {
      tptex_face_data fd;
      int ul = fi.res.ulog2 > level ? fi.res.ulog2 - level : 0;
      int vl = fi.res.vlog2 > level ? fi.res.vlog2 - level : 0;
      if ((((uint64_t)tptex_pixel_size(t)) << (ul + vl)) > FUZZ_MAX_BYTES) continue;
      if (tptex_get_face_data(t, face, tptex_make_res(ul, vl), &fd) == TPTEX_OK)
        tptex_release_face_data(t, &fd);
    }
    {
      float px[8];
      (void)tptex_get_pixel(t, face, 0, 0, px, 0, 8);
    }
  }

  if (tptex_meta_count(t, &count) == TPTEX_OK) {
    for (i = 0; i < count && i < 16; ++i) {
      const char* key;
      tptex_meta_type mt;
      const void* d;
      int n;
      (void)tptex_meta_key(t, i, &key, &mt);
      (void)tptex_meta_value(t, i, &d, &n);
    }
  }

  if (nfaces) {
    const uint32_t f32 = (uint32_t)(size % (size_t)nfaces);
    tptex_face_info fi;
    if (tptex_get_face_info(t, (int)f32, &fi) == TPTEX_OK) {
      tptex_sample_source src;
      tptex_footprint fp;
      tptex_face_info* faces = (tptex_face_info*)calloc((size_t)info->num_faces, sizeof *faces);
      const float gu[3] = {3.0f, 0.0f, 0.5f}, gv[3] = {0.1f, 0.2f, 0.0f};
      float rgb[3], ch;
      uint64_t cross = 0;
      uint32_t n = 0;
      if (faces) {
        for (i = 0; i < info->num_faces; ++i) (void)tptex_get_face_info(t, i, &faces[i]);
        src.faces = faces;
        src.nfaces = (uint32_t)info->num_faces;
        src.levels = (uint32_t)info->num_levels;
        src.fetch = fetch;
        src.user = &pages;
        tptex_compute_uv_footprint(&fi, src.levels, gu, gv, 0.05f, &fp);
        (void)tptex_sample_rgb(&src, f32, 0, -0.1f, 1.2f, 1, rgb, &cross);
        (void)tptex_sample_channel(&src, f32, 1, 1.1f, -0.2f, 2, &ch, &cross);
        (void)tptex_sample_footprint_rgb(&src, f32, &fp, 0.9f, 0.1f, 0, rgb, &cross, &n);
        (void)tptex_sample_footprint_channel(&src, f32, &fp, 0.0f, 1.0f, 0, &ch, &cross, &n);
        free(faces);
      }
    }
    for (ft = 0; ft < 8; ++ft) {
      tptex_filter_options fo;
      tptex_filter* flt = NULL;
      float out[8];
      tptex_filter_options_init(&fo);
      fo.filter = (tptex_filter_type)ft;
      fo.lerp = ft & 1;
      fo.noedgeblend = (ft >> 1) & 1;
      fo.sharpness = 0.3f;
      if (tptex_filter_create(t, &fo, &flt) != TPTEX_OK) continue;
      for (i = 0; i < 4; ++i) {
        const float w = 0.5f / (float)(1 << (i * 2));
        (void)tptex_filter_eval(flt, out, 0, 8, (int)(size % (size_t)nfaces), 0.3f + 0.4f * (float)i,
                                -0.1f + 0.4f * (float)i, w, 0.0f, 0.2f * w, w, 1.0f, 0.001f);
      }
      tptex_filter_destroy(flt);
    }
  }
  free(pages.buf);
  tptex_close(t);
  return 0;
}
