/* SPDX-License-Identifier: Apache-2.0 */
/* libFuzzer harness for the C11 Ptex core (src/ptex): container parsing,
 * face decode under a byte budget, and seam-aware sampling over decoded
 * pages. Built with LIGHTUSD_NEXT_BUILD_FUZZERS=ON (clang). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ptex/lptex.h"
#include "ptex/lptex-sample.h"

#define FUZZ_MAX_BYTES ((size_t)1 << 20)
#define FUZZ_MAX_FACES 64u

typedef struct fuzz_pages {
  const lptex_reader* reader;
  lptex_image page;
} fuzz_pages;

/* Decode on demand; the previous page is released first, which also checks
 * that the sampler never keeps a page view across fetches. */
static int fetch(void* user, uint32_t face, uint32_t mip, lptex_image* out) {
  fuzz_pages* p = (fuzz_pages*)user;
  lptex_image_free(p->reader, &p->page);
  if (lptex_read_face(p->reader, face, mip, FUZZ_MAX_BYTES, &p->page) !=
      LPTEX_OK)
    return 0;
  *out = p->page;
  return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lptex_reader* r = NULL;
  const lptex_info* info;
  uint32_t face, level, faces;
  if (lptex_open_memory(data, size, NULL, &r) != LPTEX_OK) return 0;
  info = lptex_reader_info(r);
  faces = info->faces < FUZZ_MAX_FACES ? info->faces : FUZZ_MAX_FACES;
  for (face = 0; face < faces; ++face) {
    for (level = 0; level < info->levels; ++level) {
      lptex_image img;
      if (lptex_read_face(r, face, level, FUZZ_MAX_BYTES, &img) == LPTEX_OK) {
        float rgb[3], ch;
        lptex_sample_page_rgb(&img, 0.25f, 0.75f, rgb);
        lptex_sample_page_channel(&img, 0.5f, 0.5f, 7, &ch);
        lptex_image_free(r, &img);
      }
    }
  }
  if (faces) {
    fuzz_pages pages;
    lptex_sample_source src;
    lptex_footprint fp;
    const float gu[3] = {3.0f, 0.0f, 0.5f}, gv[3] = {0.1f, 0.2f, 0.0f};
    float rgb[3], ch;
    uint64_t cross = 0;
    uint32_t n = 0;
    memset(&pages, 0, sizeof(pages));
    pages.reader = r;
    src.faces = info->face_info;
    src.nfaces = info->faces;
    src.levels = info->levels;
    src.fetch = fetch;
    src.user = &pages;
    face = (uint32_t)(size % faces);
    lptex_compute_uv_footprint(&info->face_info[face], info->levels, gu, gv,
                               0.05f, &fp);
    (void)lptex_sample_rgb(&src, face, 0, -0.1f, 1.2f, 1, rgb, &cross);
    (void)lptex_sample_channel(&src, face, 1, 1.1f, -0.2f, 2, &ch, &cross);
    (void)lptex_sample_footprint_rgb(&src, face, &fp, 0.9f, 0.1f, 0, rgb,
                                     &cross, &n);
    (void)lptex_sample_footprint_channel(&src, face, &fp, 0.0f, 1.0f, 0, &ch,
                                         &cross, &n);
    lptex_image_free(r, &pages.page);
  }
  lptex_close(r);
  return 0;
}
