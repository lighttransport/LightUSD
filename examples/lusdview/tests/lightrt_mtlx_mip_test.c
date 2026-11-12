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

static int read_memory_image(void *user, const char *path,
                             const unsigned char **data, size_t *size) {
  static const unsigned char ppm[] = "P6\n1 1\n255\n\xff\0\0";
  (void)user;
  if (strcmp(path, "memory.ppm")) return 0;
  *data = ppm;
  *size = sizeof(ppm) - 1;
  return 1;
}

static int check_volume_work_limit(void) {
  char xml[8192];
  int used = snprintf(xml, sizeof(xml),
      "<materialx version=\"1.39\"><absorption_vdf name=\"n0\" type=\"VDF\"/>");
  for (int i = 1; i <= 17; ++i) {
    if (used < 0 || (size_t)used >= sizeof(xml)) return 0;
    used += snprintf(xml + used, sizeof(xml) - (size_t)used,
        "<add name=\"n%d\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"n%d\"/><input name=\"in2\" type=\"VDF\" nodename=\"n%d\"/></add>",
        i, i - 1, i - 1);
  }
  if (used < 0 || (size_t)used >= sizeof(xml)) return 0;
  used += snprintf(xml + used, sizeof(xml) - (size_t)used,
      "<volume name=\"volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"n17\"/></volume></materialx>");
  if (used < 0 || (size_t)used >= sizeof(xml)) return 0;
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) return 0;
  ShadeContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc;
  MtlxVolumeParams out;
  const int id = mtlx_find_node(doc, -1, "volume");
  const int missing_memo_rejected = id >= 0 && mtlx_eval_volume(&ctx, id, &out) != 0;
  ctx.memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
  ctx.memo_done = (char *)calloc((size_t)doc->nnode, 1);
  const int bounded = ctx.memo && ctx.memo_done &&
      mtlx_eval_volume(&ctx, id, &out) != 0;
  free(ctx.memo);
  free(ctx.memo_done);
  mtlx_free(doc);
  return missing_memo_rejected && bounded;
}

int main(void) {
  if (!check_volume_work_limit()) return 1;
  TextureCache *bounded = texcache_create(".");
  if (!bounded) return 1;
  texcache_set_reader(bounded, read_memory_image, NULL, 3);
  if (texcache_get(bounded, "memory.ppm", 0) >= 0 ||
      !texcache_had_load_failure(bounded)) {
    texcache_free(bounded);
    return 1;
  }
  texcache_free(bounded);
  bounded = texcache_create(".");
  if (!bounded) return 1;
  texcache_set_reader(bounded, read_memory_image, NULL, 4);
  if (texcache_get(bounded, "memory.ppm", 0) < 0 ||
      texcache_had_load_failure(bounded)) {
    texcache_free(bounded);
    return 1;
  }
  texcache_free(bounded);
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
      "<constant name=\"density_value\" type=\"vector3\"><input name=\"value\" type=\"vector3\" value=\"0.5, 0.5, 0.5\"/></constant>"
      "<mystery_value name=\"unknown_value\" type=\"vector3\"/>"
      "<absorption_vdf name=\"connected_fog\" type=\"VDF\"><input name=\"absorption\" type=\"vector3\" nodename=\"density_value\"/></absorption_vdf>"
      "<absorption_vdf name=\"bad_fog\" type=\"VDF\"><input name=\"absorption\" type=\"vector3\" nodename=\"unknown_value\"/></absorption_vdf>"
      "<absorption_vdf name=\"missing_fog\" type=\"VDF\"><input name=\"absorption\" type=\"vector3\" nodename=\"missing_value\"/></absorption_vdf>"
      "<uniform_edf name=\"glow\" type=\"EDF\"><input name=\"color\" type=\"color3\" value=\"0.2, 0.4, 0.8\"/></uniform_edf>"
      "<constant name=\"weight\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"0.2, 0.5, 0.8\"/></constant>"
      "<constant name=\"signed_weight\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"-1, 2, 0\"/></constant>"
      "<multiply name=\"weighted_fog\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"fog\"/><input name=\"in2\" type=\"color3\" nodename=\"weight\"/></multiply>"
      "<multiply name=\"weighted_glow\" type=\"EDF\"><input name=\"in1\" type=\"EDF\" nodename=\"glow\"/><input name=\"in2\" type=\"color3\" nodename=\"weight\"/></multiply>"
      "<multiply name=\"signed_fog\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"fog\"/><input name=\"in2\" type=\"color3\" nodename=\"signed_weight\"/></multiply>"
      "<mystery_edf name=\"unknown_edf\" type=\"EDF\"/>"
      "<measured_edf name=\"measured\" type=\"EDF\"><input name=\"file\" type=\"filename\" value=\"profile.ies\"/></measured_edf>"
      "<add name=\"nested_edf\" type=\"EDF\"><input name=\"in1\" type=\"EDF\" nodename=\"glow\"/><input name=\"in2\" type=\"EDF\" nodename=\"unknown_edf\"/></add>"
      "<add name=\"nested\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"fog\"/><input name=\"in2\" type=\"VDF\" nodename=\"unknown\"/></add>"
      "<volume name=\"valid_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"fog\"/><input name=\"edf\" type=\"EDF\" nodename=\"glow\"/></volume>"
      "<volume name=\"invalid_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"unknown\"/></volume>"
      "<volume name=\"nested_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"nested\"/></volume>"
      "<volume name=\"bad_emission\" type=\"volumeshader\"><input name=\"edf\" type=\"EDF\" nodename=\"nested_edf\"/></volume>"
      "<volume name=\"emission_only\" type=\"volumeshader\"><input name=\"edf\" type=\"EDF\" nodename=\"glow\"/></volume>"
      "<volume name=\"measured_volume\" type=\"volumeshader\"><input name=\"edf\" type=\"EDF\" nodename=\"measured\"/></volume>"
      "<volume name=\"connected_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"connected_fog\"/></volume>"
      "<volume name=\"bad_value_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"bad_fog\"/></volume>"
      "<volume name=\"missing_value_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"missing_fog\"/></volume>"
      "<volume name=\"missing_closure_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"missing_vdf\"/></volume>"
      "<volume name=\"weighted_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"weighted_fog\"/><input name=\"edf\" type=\"EDF\" nodename=\"weighted_glow\"/></volume>"
      "<volume name=\"signed_volume\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"signed_fog\"/></volume>"
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
    const int bad_emission = mtlx_find_node(doc, -1, "bad_emission");
    const int emission_only = mtlx_find_node(doc, -1, "emission_only");
    const int measured = mtlx_find_node(doc, -1, "measured_volume");
    const int connected = mtlx_find_node(doc, -1, "connected_volume");
    const int bad_value = mtlx_find_node(doc, -1, "bad_value_volume");
    const int missing_value = mtlx_find_node(doc, -1, "missing_value_volume");
    const int missing_closure = mtlx_find_node(doc, -1, "missing_closure_volume");
    const int weighted = mtlx_find_node(doc, -1, "weighted_volume");
    const int signed_volume = mtlx_find_node(doc, -1, "signed_volume");
    ok = valid >= 0 && invalid >= 0 && nested >= 0 && bad_emission >= 0 &&
         emission_only >= 0 && measured >= 0 &&
         connected >= 0 && bad_value >= 0 && missing_value >= 0 && missing_closure >= 0 && weighted >= 0 && signed_volume >= 0 &&
         mtlx_eval_volume(&ctx, valid, &volume) == 0 &&
         near(volume.absorption.x, 0.1f, 1.0e-5f) &&
         near(volume.absorption.z, 0.3f, 1.0e-5f) &&
         near(volume.emission.y, 0.4f, 1.0e-5f) &&
         mtlx_eval_volume(&ctx, invalid, &volume) != 0 &&
         mtlx_eval_volume(&ctx, nested, &volume) != 0 &&
         mtlx_eval_volume(&ctx, bad_emission, &volume) != 0 &&
         mtlx_eval_volume(&ctx, measured, &volume) != 0 &&
         mtlx_eval_volume(&ctx, emission_only, &volume) == 0 &&
         near(volume.absorption.x, 0.0f, 1.0e-5f) &&
         near(volume.emission.x, 0.2f, 1.0e-5f) &&
         near(volume.emission.y, 0.4f, 1.0e-5f) &&
         near(volume.emission.z, 0.8f, 1.0e-5f) &&
         mtlx_eval_volume(&ctx, connected, &volume) == 0 &&
         near(volume.absorption.y, 0.5f, 1.0e-5f) &&
         mtlx_eval_volume(&ctx, bad_value, &volume) != 0 &&
         mtlx_eval_volume(&ctx, missing_value, &volume) != 0 &&
         mtlx_eval_volume(&ctx, missing_closure, &volume) != 0 &&
         mtlx_eval_volume(&ctx, weighted, &volume) == 0 &&
         near(volume.absorption.x, 0.02f, 1.0e-5f) &&
         near(volume.absorption.y, 0.10f, 1.0e-5f) &&
         near(volume.absorption.z, 0.24f, 1.0e-5f) &&
         near(volume.emission.x, 0.04f, 1.0e-5f) &&
         near(volume.emission.y, 0.20f, 1.0e-5f) &&
         near(volume.emission.z, 0.64f, 1.0e-5f) &&
         mtlx_eval_volume(&ctx, signed_volume, &volume) == 0 &&
         near(volume.absorption.x, -0.1f, 1.0e-5f) &&
         near(volume.absorption.y, 0.4f, 1.0e-5f) &&
         near(volume.absorption.z, 0.0f, 1.0e-5f);
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
