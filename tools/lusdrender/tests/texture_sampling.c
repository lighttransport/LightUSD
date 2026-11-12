#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  texcache_sample_nearest_address(cache, linear_id, 0.25f, 0.75f,
                                  "clamp", "clamp", sample);
  if (!near(sample[0], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[1], 128.0f / 255.0f, 1e-6f) ||
      !near(sample[2], 128.0f / 255.0f, 1e-6f)) {
    texcache_free(cache);
    remove(argv[1]);
    return fail("nearest texel-center sample", sample);
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

  texcache_free(cache);
  remove(argv[1]);
  puts("PASS: nearest, bilinear, constant/periodic/clamp/mirror, and sRGB sampling");
  return 0;
}
