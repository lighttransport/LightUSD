// SPDX-License-Identifier: Apache-2.0
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_ppm(const char *path, const unsigned char *rgb, int width,
                     int height) {
  FILE *file = fopen(path, "wb");
  if (!file) return 0;
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  const size_t bytes = (size_t)width * (size_t)height * 3;
  const size_t written = fwrite(rgb, 1, bytes, file);
  const int close_ok = fclose(file) == 0;
  return written == bytes && close_ok;
}

static int near(float actual, float expected, float tolerance) {
  return fabsf(actual - expected) <= tolerance;
}

int main(void) {
  const char *checker_path = "lightusd_mtlx_mip_checker.ppm";
  const char *srgb_path = "lightusd_mtlx_mip_srgb.ppm";
  const unsigned char checker_pixels[12] = {
      255, 0, 0, 0, 255, 0,
      0, 0, 255, 255, 255, 255};
  const unsigned char srgb_pixels[6] = {128, 128, 128, 128, 128, 128};
  if (!write_ppm(checker_path, checker_pixels, 2, 2) ||
      !write_ppm(srgb_path, srgb_pixels, 2, 1)) {
    remove(checker_path);
    remove(srgb_path);
    return 1;
  }

  const char *xml =
      "<materialx version=\"1.39\">"
      "<image name=\"checker\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"lightusd_mtlx_mip_checker.ppm\"/>"
      "</image>"
      "<image name=\"srgb\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"lightusd_mtlx_mip_srgb.ppm\" "
      "colorspace=\"srgb_texture\"/></image>"
      "<absorption_vdf name=\"fog\" type=\"VDF\"><input name=\"absorption\" type=\"vector3\" value=\"0.1, 0.2, 0.3\"/></absorption_vdf>"
      "<mystery_vdf name=\"unknown\" type=\"VDF\"/>"
      "<add name=\"nested\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"fog\"/><input name=\"in2\" type=\"VDF\" nodename=\"unknown\"/></add>"
      "<volume name=\"valid_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"fog\"/></volume>"
      "<volume name=\"invalid_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"unknown\"/></volume>"
      "<volume name=\"nested_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"nested\"/></volume>"
      "</materialx>";
  MtlxDoc *doc = mtlx_load_string(xml);
  TextureCache *cache = texcache_create(".");
  int ok = doc && cache && texcache_preload_mips(cache, doc);
  ShadeContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc;
  ctx.tex = cache;
  ctx.uv[0] = 0.25f;
  ctx.uv[1] = 0.25f;
  ctx.uv_dx[0] = 1.0f;
  ctx.uv_dy[1] = 1.0f;
  ctx.has_uv_derivatives = 1;
  if (ok) {
    ctx.memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
    ctx.memo_done = (char *)calloc((size_t)doc->nnode, 1);
    ok = ctx.memo && ctx.memo_done;
  }
  const int checker_id = doc ? mtlx_find_node(doc, -1, "checker") : -1;
  const int srgb_id = doc ? mtlx_find_node(doc, -1, "srgb") : -1;
  if (ok && (checker_id < 0 || srgb_id < 0)) ok = 0;

  if (ok) {
    const MtlxValue checker = mtlx_eval_node_test(&ctx, checker_id);
    const MtlxValue srgb = mtlx_eval_node_test(&ctx, srgb_id);
    /* LOD 1 selects each 1x1 terminal mip, which is the linear RGBA mean. */
    ok = near(checker.v[0], 0.5f, 1.0e-5f) &&
         near(checker.v[1], 0.5f, 1.0e-5f) &&
         near(checker.v[2], 0.5f, 1.0e-5f) &&
         near(srgb.v[0], 0.21586f, 2.0e-4f) &&
         near(srgb.v[1], 0.21586f, 2.0e-4f) &&
         near(srgb.v[2], 0.21586f, 2.0e-4f);
  }
  if (ok) {
    MtlxVolumeParams volume;
    const int valid = mtlx_find_node(doc, -1, "valid_volume");
    const int invalid = mtlx_find_node(doc, -1, "invalid_volume");
    const int nested = mtlx_find_node(doc, -1, "nested_volume");
    ok = valid >= 0 && invalid >= 0 && nested >= 0 &&
         mtlx_eval_volume(&ctx, valid, &volume) == 0 &&
         near(volume.absorption.x, 0.1f, 1.0e-5f) &&
         near(volume.absorption.z, 0.3f, 1.0e-5f) &&
         mtlx_eval_volume(&ctx, invalid, &volume) != 0 &&
         mtlx_eval_volume(&ctx, nested, &volume) != 0;
  }
  if (!ok) fprintf(stderr, "MaterialX evaluator mip sampling failed\n");

  free(ctx.memo);
  free(ctx.memo_done);
  texcache_free(cache);
  mtlx_free(doc);
  remove(checker_path);
  remove(srgb_path);
  return ok ? 0 : 1;
}
