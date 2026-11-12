#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtlx_eval.h"
#include "mtlx_doc.h"
#include "color.h"
#include "texture.h"

static int near(float actual, float expected, float tolerance) {
  return isfinite(actual) && fabsf(actual - expected) <= tolerance;
}

static int fail(const char *what, const float sample[4]) {
  fprintf(stderr, "FAIL: %s: (%.6f, %.6f, %.6f, %.6f)\n", what,
          sample[0], sample[1], sample[2], sample[3]);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: texture-sampling <fixture.ppm>\n");
    return 2;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s", argv[1]);
  char *slash = strrchr(path, '/');
  char *backslash = strrchr(path, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
  const char *base_dir = ".";
  const char *filename = path;
  if (slash) {
    *slash = '\0';
    base_dir = path[0] ? path : "/";
    filename = slash + 1;
  }

  FILE *image = fopen(argv[1], "wb");
  if (!image) {
    perror("cannot create texture fixture");
    return 1;
  }
  static const unsigned char pixels[12] = {
      128, 128, 128,  0, 255, 0,
      0,   0,   255,  255, 255, 255,
  };
  const int wrote = fprintf(image, "P6\n2 2\n255\n") > 0 &&
                    fwrite(pixels, 1, sizeof(pixels), image) == sizeof(pixels);
  const int closed = fclose(image) == 0;
  if (!wrote || !closed) {
    fprintf(stderr, "cannot write texture fixture\n");
    remove(argv[1]);
    return 1;
  }

  TextureCache *cache = texcache_create(base_dir);
  if (!cache) {
    remove(argv[1]);
    return 1;
  }
  const int linear_id = texcache_get(cache, filename, 0);
  const int srgb_id = texcache_get(cache, filename, 1);
  if (linear_id < 0 || srgb_id < 0) {
    texcache_free(cache);
    remove(argv[1]);
    return 1;
  }

  float sample[4];
  if (!near(srgb_to_linear_f(10.0f / 255.0f), 0.00303527f, 2e-8f) ||
      !near(srgb_to_linear_f(11.0f / 255.0f), 0.00334654f, 2e-8f)) {
    texcache_free(cache);
    remove(argv[1]);
    fprintf(stderr, "FAIL: sRGB linear/power transfer branches\n");
    return 1;
  }
  texcache_sample_nearest_address(cache, linear_id, 0.25f, 0.75f,
                                  "clamp", "clamp", sample);
  if (!near(sample[0], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[1], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[2], 128.0f / 255.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("nearest texel-center sample", sample);
  }
  texcache_sample_nearest_address(cache, linear_id, 0.25f, 0.25f,
                                  "clamp", "clamp", sample);
  if (!near(sample[0], 0.0f, 1e-6f) || !near(sample[1], 0.0f, 1e-6f) ||
      !near(sample[2], 1.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("lower-left UV row orientation", sample);
  }

  texcache_sample_address(cache, linear_id, 0.5f, 0.5f,
                          "clamp", "clamp", sample);
  if (!near(sample[0], 0.37549f, 2e-5f) ||
      !near(sample[1], 0.62549f, 2e-5f) ||
      !near(sample[2], 0.62549f, 2e-5f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("bilinear center sample", sample);
  }

  texcache_sample_address_filter(cache, linear_id, 0.375f, 0.75f,
                                 "clamp", "clamp", "cubic", sample);
  if (!near(sample[0], 0.39998f, 2e-4f) ||
      !near(sample[1], 0.60313f, 2e-4f) ||
      !near(sample[2], 0.39998f, 2e-4f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("Catmull-Rom cubic reconstruction", sample);
  }

  texcache_sample_nearest_address(cache, linear_id, -0.25f, 0.75f,
                                  "periodic", "clamp", sample);
  if (!near(sample[0], 0.0f, 1e-6f) || !near(sample[1], 1.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("periodic address mode", sample);
  }
  texcache_sample_nearest_address(cache, linear_id, -0.25f, 0.75f,
                                  "clamp", "clamp", sample);
  if (!near(sample[0], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[1], 128.0f / 255.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("clamp address mode", sample);
  }
  texcache_sample_nearest_address(cache, linear_id, -0.25f, 0.75f,
                                  "mirror", "clamp", sample);
  if (!near(sample[0], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[1], 128.0f / 255.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("mirror address mode", sample);
  }
  texcache_sample_address(cache, linear_id, 0.0f, 0.75f,
                          "constant", "constant", sample);
  if (!near(sample[0], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[1], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[2], 128.0f / 255.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("constant address edge sample", sample);
  }

  texcache_sample_nearest_address(cache, srgb_id, 0.25f, 0.75f,
                                  "clamp", "clamp", sample);
  if (!near(sample[0], 0.21586f, 1e-4f) ||
      !near(sample[1], 0.21586f, 1e-4f) ||
      !near(sample[2], 0.21586f, 1e-4f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("sRGB decode", sample);
  }

  char xml[4096];
  snprintf(xml, sizeof(xml),
      "<materialx version=\"1.39\">"
      "<image name=\"sample\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"%s\"/>"
      "<input name=\"default\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "<input name=\"uaddressmode\" type=\"string\" value=\"constant\"/>"
      "<input name=\"vaddressmode\" type=\"string\" value=\"clamp\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"cubic\"/>"
      "</image>"
      "<tiledimage name=\"tiled\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"%s\"/>"
      "<input name=\"default\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "<input name=\"uvtiling\" type=\"vector2\" value=\"2, 2\"/>"
      "<input name=\"uvoffset\" type=\"vector2\" value=\"0.5, 0\"/>"
      "<input name=\"realworldimagesize\" type=\"vector2\" value=\"2, 2\"/>"
      "<input name=\"realworldtilesize\" type=\"vector2\" value=\"1, 1\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</tiledimage>"
      "<tiledimage name=\"tiled_zero\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"%s\"/>"
      "<input name=\"default\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "<input name=\"realworldimagesize\" type=\"vector2\" value=\"0, 2\"/>"
      "</tiledimage></materialx>", filename, filename, filename);
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) {
    texcache_free(cache);
    remove(argv[1]);
    fprintf(stderr, "FAIL: could not load constant-address MaterialX fixture\n");
    return 1;
  }
  const int node = mtlx_find_node(doc, -1, "sample");
  const int tiled_node = mtlx_find_node(doc, -1, "tiled");
  const int tiled_zero_node = mtlx_find_node(doc, -1, "tiled_zero");
  MtlxValue *memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
  char *memo_done = (char *)calloc((size_t)doc->nnode, sizeof(char));
  if (node < 0 || tiled_node < 0 || tiled_zero_node < 0 || !memo || !memo_done) {
    free(memo);
    free(memo_done);
    mtlx_free(doc);
    texcache_free(cache);
    remove(argv[1]);
    fprintf(stderr, "FAIL: invalid constant-address MaterialX fixture state\n");
    return 1;
  }
  ShadeContext context;
  memset(&context, 0, sizeof(context));
  context.doc = doc;
  context.tex = cache;
  context.uv[0] = -0.25f;
  context.uv[1] = 0.75f;
  context.memo = memo;
  context.memo_done = memo_done;
  const MtlxValue fallback = mtlx_eval_node_test(&context, node);
  const int fallback_ok = fallback.type == MV_COLOR3 &&
      near(fallback.v[0], 0.1f, 1e-6f) &&
      near(fallback.v[1], 0.2f, 1e-6f) &&
      near(fallback.v[2], 0.3f, 1e-6f);
  context.uv[0] = 0.375f;
  context.uv[1] = 0.75f;
  memset(memo_done, 0, (size_t)doc->nnode * sizeof(char));
  const MtlxValue cubic_image = mtlx_eval_node_test(&context, node);
  const int cubic_image_ok = cubic_image.type == MV_COLOR3 &&
      near(cubic_image.v[0], 0.39998f, 2e-4f) &&
      near(cubic_image.v[1], 0.60313f, 2e-4f) &&
      near(cubic_image.v[2], 0.39998f, 2e-4f);
  context.uv[0] = 0.5f;
  context.uv[1] = 0.75f;
  memset(memo_done, 0, (size_t)doc->nnode * sizeof(char));
  const MtlxValue tiled_sample = mtlx_eval_node_test(&context, tiled_node);
  const int tiled_sample_ok = tiled_sample.type == MV_COLOR3 &&
      near(tiled_sample.v[0], 128.0f / 255.0f, 1e-6f) &&
      near(tiled_sample.v[1], 128.0f / 255.0f, 1e-6f) &&
      near(tiled_sample.v[2], 128.0f / 255.0f, 1e-6f);
  memset(memo_done, 0, (size_t)doc->nnode * sizeof(char));
  const MtlxValue tiled_zero = mtlx_eval_node_test(&context, tiled_zero_node);
  const int tiled_zero_ok = tiled_zero.type == MV_COLOR3 &&
      near(tiled_zero.v[0], 0.1f, 1e-6f) &&
      near(tiled_zero.v[1], 0.2f, 1e-6f) &&
      near(tiled_zero.v[2], 0.3f, 1e-6f);
  free(memo);
  free(memo_done);
  mtlx_free(doc);
  if (!fallback_ok || !cubic_image_ok || !tiled_sample_ok || !tiled_zero_ok) {
    texcache_free(cache);
    remove(argv[1]);
    fprintf(stderr, "FAIL: MaterialX texture filter, tiledimage transform, or constant default behavior\n");
    return 1;
  }

  texcache_free(cache);
  remove(argv[1]);
  puts("PASS: nearest, bilinear, Catmull-Rom cubic, UV row orientation, all address modes, and sRGB sampling");
  return 0;
}
