// SPDX-License-Identifier: Apache-2.0
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nearf(float a, float b) { return fabsf(a - b) <= 1.0e-6f; }

static int sample_texcoord(void *user, int index, int probe_axis,
                           float out_uv[2]) {
  (void)user;
  if (index != 1) return 0;
  out_uv[0] = 0.375f;
  out_uv[1] = 0.625f;
  if (probe_axis == 1) { out_uv[0] += 0.05f; out_uv[1] += 0.01f; }
  if (probe_axis == 2) { out_uv[0] -= 0.02f; out_uv[1] += 0.04f; }
  return 1;
}


static int eval_xml(const char *xml, OpenPBRParams *p) {
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) {
    fprintf(stderr, "mtlx_load_string failed\n");
    return 0;
  }
  if (doc->nmat != 1 || doc->mats[0].surface_node < 0) {
    fprintf(stderr, "surface material did not resolve\n");
    mtlx_free(doc);
    return 0;
  }

  ShadeContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc;
  ctx.tex = NULL;
  ctx.uv[0] = 0.5f;
  ctx.uv[1] = 0.5f;
  ctx.P = v3_make(0.0f, 0.0f, 0.0f);
  ctx.Ns = v3_make(0.0f, 0.0f, 1.0f);
  ctx.Ng = v3_make(0.0f, 0.0f, 1.0f);
  ctx.dpdu = v3_make(1.0f, 0.0f, 0.0f);
  ctx.dpdv = v3_make(0.0f, 1.0f, 0.0f);
  ctx.V = v3_make(0.0f, 0.0f, 1.0f);
  ctx.memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
  ctx.memo_done = (char *)calloc((size_t)doc->nnode, 1);
  if (!ctx.memo || !ctx.memo_done) {
    fprintf(stderr, "allocation failed\n");
    free(ctx.memo);
    free(ctx.memo_done);
    mtlx_free(doc);
    return 0;
  }

  int ok = mtlx_eval_surface(&ctx, doc->mats[0].surface_node, p) == 0;
  free(ctx.memo);
  free(ctx.memo_done);
  mtlx_free(doc);
  if (!ok) {
    fprintf(stderr, "mtlx_eval_surface failed\n");
  }
  return ok;
}

static int eval_volume_xml(const char *xml, MtlxVolumeParams *p) {
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc || doc->nmat != 1 || doc->mats[0].volume_node < 0) {
    fprintf(stderr, "volume material did not resolve\n");
    mtlx_free(doc);
    return 0;
  }
  ShadeContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc;
  ctx.Ns = v3_make(0.0f, 0.0f, 1.0f);
  ctx.V = v3_make(0.0f, 0.0f, 1.0f);
  ctx.memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
  ctx.memo_done = (char *)calloc((size_t)doc->nnode, 1);
  const int ok = ctx.memo && ctx.memo_done &&
      mtlx_eval_volume(&ctx, doc->mats[0].volume_node, p) == 0;
  free(ctx.memo);
  free(ctx.memo_done);
  mtlx_free(doc);
  return ok;
}

static int test_texture_colorspace_aliases(void) {
  const char *spaces[] = {"srgb_texture", "sRGB", "srgb",
                          "srgb_rec709_scene", "lin_rec709_scene", "raw"};
  const int expected[] = {1, 1, 1, 1, 0, 0};
  char xml[2048] = "<materialx version=\"1.39\">";
  size_t used = strlen(xml);
  for (size_t i = 0; i < sizeof(spaces) / sizeof(spaces[0]); ++i) {
    int n = snprintf(xml + used, sizeof(xml) - used,
                     "<image name=\"image%zu\" type=\"color3\">"
                     "<input name=\"file\" type=\"filename\" "
                     "value=\"unused%zu.png\" colorspace=\"%s\"/>"
                     "</image>", i, i, spaces[i]);
    if (n < 0 || (size_t)n >= sizeof(xml) - used) return 0;
    used += (size_t)n;
  }
  strcat(xml, "</materialx>");
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) return 0;
  int ok = 1;
  int found = 0;
  for (int i = 0; ok && i < doc->nnode; ++i) {
    const MtlxNode *node = &doc->nodes[i];
    if (strcmp(node->category, "image") != 0) continue;
    if (strncmp(node->name, "image", 5) != 0) { ok = 0; break; }
    const int case_index = atoi(node->name + 5);
    if (case_index < 0 || case_index >= (int)(sizeof(spaces) / sizeof(spaces[0]))) {
      ok = 0; break;
    }
    ++found;
    const MtlxInput *file = NULL;
    for (int j = 0; j < node->ninput; ++j)
      if (!strcmp(node->inputs[j].name, "file")) file = &node->inputs[j];
    if (!file || file->colorspace_srgb != expected[case_index]) {
      fprintf(stderr, "colorspace case %d (%s): node=%s flag=%d expected=%d\n",
              case_index, spaces[case_index], node->name,
              file ? file->colorspace_srgb : -1, expected[case_index]);
      ok = 0;
    }
  }
  ok = ok && found == (int)(sizeof(spaces) / sizeof(spaces[0]));
  mtlx_free(doc);
  return ok;
}

int main(void) {
  if (!test_texture_colorspace_aliases()) {
    fprintf(stderr, "MaterialX texture colorspace aliases were not parsed\n");
    return 1;
  }
  const char *anisotropic_standard_xml =
      "<materialx version=\"1.39\">"
      "<standard_surface name=\"ss\" type=\"surfaceshader\">"
      "<input name=\"base_color\" type=\"color3\" value=\"0.2,0.4,0.6\"/>"
      "<input name=\"metalness\" type=\"float\" value=\"0.2\"/>"
      "<input name=\"specular_color\" type=\"color3\" value=\"0.5,0.6,0.7\"/>"
      "<input name=\"specular\" type=\"float\" value=\"0.75\"/>"
      "<input name=\"specular_roughness\" type=\"float\" value=\"0.25\"/>"
      "<input name=\"specular_IOR\" type=\"float\" value=\"2.4\"/>"
      "<input name=\"specular_anisotropy\" type=\"float\" value=\"0.5\"/>"
      "<input name=\"specular_rotation\" type=\"float\" value=\"0.25\"/>"
      "</standard_surface>"
      "<surfacematerial name=\"mat\" type=\"material\">"
      "<input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"ss\"/>"
      "</surfacematerial></materialx>";
  OpenPBRParams anisotropic_standard;
  if (!eval_xml(anisotropic_standard_xml, &anisotropic_standard) ||
      anisotropic_standard.specular_lobe_count != 1 ||
      !nearf(anisotropic_standard.specular_lobes[0].frame.roughness,
             0.08838835f) ||
      !nearf(anisotropic_standard.specular_lobes[0].frame.roughness_y,
             0.04419417f) ||
      !nearf(anisotropic_standard.specular_lobes[0].frame.tangent.x, 0.0f) ||
      !nearf(anisotropic_standard.specular_lobes[0].frame.tangent.y, 1.0f) ||
      !nearf(anisotropic_standard.specular_lobes[0].frame.weight, 0.6f) ||
      anisotropic_standard.diffuse_lobe_count != 1 ||
      anisotropic_standard.conductor_lobe_count != 1) {
    fprintf(stderr, "Standard Surface anisotropic specular closure was not retained\n");
    return 1;
  }
  /* A document geompropdef can redirect the standard UV0 alias to UV set 1.
   * Verify the generated texcoord node carries that index to the renderer. */
  MtlxDoc *uv_doc = mtlx_load_string(
      "<materialx version=\"1.39\"><geompropdef name=\"UV0\" "
      "geomprop=\"texcoord\" type=\"vector2\" index=\"1\"/>"
      "<image name=\"img\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"unused.png\"/></image></materialx>");
  int uv_node = -1;
  if (uv_doc) for (int i = 0; i < uv_doc->nnode; ++i)
    if (!strcmp(uv_doc->nodes[i].category, "texcoord")) { uv_node = i; break; }
  ShadeContext uv_ctx = {0};
  uv_ctx.doc = uv_doc;
  uv_ctx.uv[0] = 0.1f; uv_ctx.uv[1] = 0.2f;
  uv_ctx.texcoord = sample_texcoord;
  if (uv_doc && uv_doc->nnode > 0) {
    const size_t uv_node_count = (size_t)uv_doc->nnode;
    uv_ctx.memo = (MtlxValue *)calloc(uv_node_count, sizeof(MtlxValue));
    uv_ctx.memo_done = (char *)calloc(uv_node_count, 1);
  }
  int uv_ok = uv_doc && uv_node >= 0 && uv_ctx.memo && uv_ctx.memo_done;
  if (uv_ok) {
    const MtlxValue uv = mtlx_eval_node_test(&uv_ctx, uv_node);
    uv_ok = uv.type == MV_VEC2 && nearf(uv.v[0], 0.375f) && nearf(uv.v[1], 0.625f);
  }
  free(uv_ctx.memo); free(uv_ctx.memo_done); mtlx_free(uv_doc);
  if (!uv_ok) {
    fprintf(stderr, "indexed geomprop texcoord callback failed\n");
    return 1;
  }

  /* Exercise the shared UDIM resolver directly. Keep the fixture local to the
   * test working directory so the texture cache's relative search semantics
   * are identical to a real MaterialX document. */
  const char *udim_1001 = "lightrt_udim_regression.1001.ppm";
  const char *udim_1002 = "lightrt_udim_regression.1002.ppm";
  const char *udim_1101 = "lightrt_udim_regression.1101.ppm";
  FILE *udim_file = fopen(udim_1001, "wb");
  if (!udim_file) return 1;
  fputs("P6\n1 1\n255\n", udim_file);
  fputc(255, udim_file); fputc(0, udim_file); fputc(0, udim_file);
  fclose(udim_file);
  udim_file = fopen(udim_1002, "wb");
  if (!udim_file) { remove(udim_1001); return 1; }
  fputs("P6\n1 1\n255\n", udim_file);
  fputc(0, udim_file); fputc(0, udim_file); fputc(255, udim_file);
  fclose(udim_file);
  udim_file = fopen(udim_1101, "wb");
  if (!udim_file) {
    remove(udim_1001); remove(udim_1002); return 1;
  }
  fputs("P6\n1 1\n255\n", udim_file);
  fputc(0, udim_file); fputc(255, udim_file); fputc(0, udim_file);
  fclose(udim_file);
  TextureCache *udim_cache = texcache_create(".");
  MtlxDoc *udim_doc = mtlx_load_string(
      "<materialx><image name=\"udim\" type=\"color3\"><input "
      "name=\"file\" type=\"filename\" value=\"lightrt_udim_regression.<UDIM>.ppm\"/>"
      "</image><tiledimage name=\"udim_scaled\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_udim_regression.<UDIM>.ppm\"/>"
      "<input name=\"uvtiling\" type=\"vector2\" value=\"2,1\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</tiledimage><image name=\"udim_literal\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_udim_regression.<UDIM>.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" value=\"0.92,0.5\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</image><texcoord name=\"udim_uv_source\" type=\"vector2\"/>"
      "<place2d name=\"udim_uv_scaled\" type=\"vector2\">"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"udim_uv_source\"/>"
      "<input name=\"scale\" type=\"vector2\" value=\"0.25,1\"/></place2d>"
      "<image name=\"udim_graph\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_udim_regression.<UDIM>.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"udim_uv_scaled\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image></materialx>");
  if (!udim_doc) {
    texcache_free(udim_cache); remove(udim_1001); remove(udim_1002);
    remove(udim_1101); return 1;
  }
  texcache_preload(udim_cache, udim_doc);
  ShadeContext udim_ctx = {0};
  udim_ctx.doc = udim_doc;
  udim_ctx.tex = udim_cache;
  udim_ctx.uv[0] = 0.46f; udim_ctx.uv[1] = 0.5f;
  udim_ctx.uv_dx[0] = 0.4f; udim_ctx.has_uv_derivatives = 1;
  udim_ctx.memo = (MtlxValue *)calloc((size_t)udim_doc->nnode, sizeof(MtlxValue));
  udim_ctx.memo_done = (char *)calloc((size_t)udim_doc->nnode, 1);
  const int udim_scaled_node = mtlx_find_node(udim_doc, -1, "udim_scaled");
  const int udim_literal_node = mtlx_find_node(udim_doc, -1, "udim_literal");
  const int udim_graph_node = mtlx_find_node(udim_doc, -1, "udim_graph");
  int udim_scaled_ok = udim_ctx.memo && udim_ctx.memo_done &&
                       udim_scaled_node >= 0 && udim_literal_node >= 0 && udim_graph_node >= 0;
  if (udim_scaled_ok) {
    const MtlxValue value = mtlx_eval_node_test(&udim_ctx, udim_scaled_node);
    udim_scaled_ok = fabsf(value.v[0] - 0.5f) < 1e-6f &&
                     fabsf(value.v[2] - 0.5f) < 1e-6f;
    const MtlxValue literal = mtlx_eval_node_test(&udim_ctx, udim_literal_node);
    udim_scaled_ok = udim_scaled_ok && literal.v[0] > 0.99f && literal.v[2] < 0.01f;
    udim_ctx.uv[0] = 0.23f; udim_ctx.uv_dx[0] = 0.2f;
    const MtlxValue graph = mtlx_eval_node_test(&udim_ctx, udim_graph_node);
    udim_scaled_ok = udim_scaled_ok && fabsf(graph.v[0] - 0.5f) < 1e-6f &&
                     fabsf(graph.v[2] - 0.5f) < 1e-6f;
  }
  free(udim_ctx.memo); free(udim_ctx.memo_done);
  mtlx_free(udim_doc);
  float udim_sample[4];
  const int udim_first = texcache_sample_file(
      udim_cache, "lightrt_udim_regression.<UDIM>.ppm", 0, 0.25f, 0.5f,
      udim_sample);
  const int first_red = udim_sample[0] > 0.99f && udim_sample[2] < 0.01f;
  const int udim_second = texcache_sample_file(
      udim_cache, "lightrt_udim_regression.<UDIM>.ppm", 0, 1.25f, 0.5f,
      udim_sample);
  const int second_blue = udim_sample[0] < 0.01f && udim_sample[2] > 0.99f;
  const int udim_high = texcache_sample_file(
      udim_cache, "lightrt_udim_regression.<UDIM>.ppm", 0, 0.25f, 10.5f,
      udim_sample);
  const int high_green = udim_sample[0] < 0.01f && udim_sample[1] > 0.99f;
  const int udim_missing = texcache_sample_file(
      udim_cache, "lightrt_udim_regression.<UDIM>.ppm", 0, 2.25f, 0.5f,
      udim_sample);
  texcache_free(udim_cache);
  remove(udim_1001);
  remove(udim_1002);
  remove(udim_1101);
  if (!udim_first || !udim_second || !udim_high || !first_red ||
      !second_blue || !high_green || !udim_scaled_ok || udim_missing ||
      udim_sample[3] != 1.0f) {
    fprintf(stderr, "UDIM present/missing tile resolution failed\n");
    return 1;
  }

  /* Projection nodes also own image filenames, but their lookups happen after
   * the cache is frozen.  Verify all of those files are preloaded instead of
   * falling back to the node default during render-time evaluation. */
  const char *projection_files[] = {"lightrt_latlong.ppm",
                                    "lightrt_triplanar_x.ppm",
                                    "lightrt_triplanar_y.ppm",
                                    "lightrt_triplanar_z.ppm"};
  for (size_t i = 0; i < sizeof(projection_files) / sizeof(projection_files[0]);
       i++) {
    FILE *f = fopen(projection_files[i], "wb");
    if (!f) return 1;
    fputs("P6\n1 1\n255\n", f);
    fputc(255, f); fputc(255, f); fputc(255, f);
    fclose(f);
  }
  MtlxDoc *projection_doc = mtlx_load_string(
      "<materialx>"
      "<latlongimage name=\"lat\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"lightrt_latlong.ppm\"/></latlongimage>"
      "<triplanarprojection name=\"tri\" type=\"color3\">"
      "<input name=\"filex\" type=\"filename\" value=\"lightrt_triplanar_x.ppm\"/>"
      "<input name=\"filey\" type=\"filename\" value=\"lightrt_triplanar_y.ppm\"/>"
      "<input name=\"filez\" type=\"filename\" value=\"lightrt_triplanar_z.ppm\"/>"
      "</triplanarprojection></materialx>");
  TextureCache *projection_cache = texcache_create(".");
  int projection_ok = projection_doc && projection_cache;
  if (projection_ok) {
    texcache_preload(projection_cache, projection_doc);
    for (size_t i = 0; i < sizeof(projection_files) / sizeof(projection_files[0]);
         i++) {
      if (texcache_get(projection_cache, projection_files[i], 0) < 0)
        projection_ok = 0;
    }
  }
  texcache_free(projection_cache);
  mtlx_free(projection_doc);
  for (size_t i = 0; i < sizeof(projection_files) / sizeof(projection_files[0]);
       i++) remove(projection_files[i]);
  if (!projection_ok) {
    fprintf(stderr, "projection image preload failed\n");
    return 1;
  }

  /* Projection nodes use the same explicit filter contract as image nodes.
   * Keep a two-texel fixture so nearest sampling cannot be mistaken for the
   * default bilinear midpoint. */
  const char *projection_filter_file = "lightrt_projection_filter.ppm";
  FILE *projection_filter = fopen(projection_filter_file, "wb");
  if (!projection_filter) return 1;
  fputs("P6\n2 1\n255\n", projection_filter);
  fputc(255, projection_filter); fputc(0, projection_filter); fputc(0, projection_filter);
  fputc(0, projection_filter); fputc(0, projection_filter); fputc(255, projection_filter);
  fclose(projection_filter);
  MtlxDoc *projection_filter_doc = mtlx_load_string(
      "<materialx>"
      "<latlongimage name=\"latf\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</latlongimage>"
      "<latlongimage name=\"latg\" type=\"color3\"><input name=\"file\" "
      "type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "</latlongimage>"
      "<triplanarprojection name=\"trif\" type=\"color3\">"
      "<input name=\"filex\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filey\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filez\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</triplanarprojection>"
      "<triplanarprojection name=\"trig\" type=\"color3\">"
      "<input name=\"filex\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filey\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "<input name=\"filez\" type=\"filename\" value=\"lightrt_projection_filter.ppm\"/>"
      "</triplanarprojection></materialx>");
  TextureCache *projection_filter_cache = texcache_create(".");
  int projection_filter_ok = projection_filter_doc && projection_filter_cache;
  if (projection_filter_ok) {
    texcache_preload(projection_filter_cache, projection_filter_doc);
    ShadeContext projection_filter_ctx;
    memset(&projection_filter_ctx, 0, sizeof(projection_filter_ctx));
    projection_filter_ctx.doc = projection_filter_doc;
    projection_filter_ctx.tex = projection_filter_cache;
    projection_filter_ctx.uv[0] = 0.5f;
    projection_filter_ctx.uv[1] = 0.5f;
    projection_filter_ctx.P = v3_make(0.5f, 0.0f, 0.0f);
    projection_filter_ctx.Ns = v3_make(0.0f, 0.0f, 1.0f);
    projection_filter_ctx.V = v3_make(1.0f, 0.0f, 0.0f);
    projection_filter_ctx.memo = (MtlxValue *)calloc((size_t)projection_filter_doc->nnode, sizeof(MtlxValue));
    projection_filter_ctx.memo_done = (char *)calloc((size_t)projection_filter_doc->nnode, 1);
    int latf = mtlx_find_node(projection_filter_doc, -1, "latf");
    int latg = mtlx_find_node(projection_filter_doc, -1, "latg");
    int trif = mtlx_find_node(projection_filter_doc, -1, "trif");
    int trig = mtlx_find_node(projection_filter_doc, -1, "trig");
    MtlxValue latf_value;
    MtlxValue trif_value;
    memset(&latf_value, 0, sizeof(latf_value));
    memset(&trif_value, 0, sizeof(trif_value));
    if (projection_filter_ctx.memo && projection_filter_ctx.memo_done &&
        latf >= 0 && latg >= 0 && trif >= 0 && trig >= 0) {
      latf_value = mtlx_eval_node_test(&projection_filter_ctx, latf);
      memset(projection_filter_ctx.memo_done, 0,
             (size_t)projection_filter_doc->nnode);
      trif_value = mtlx_eval_node_test(&projection_filter_ctx, trif);
      projection_filter_ok = latf_value.v[0] > 0.99f &&
          latf_value.v[2] < 0.01f && trif_value.v[2] > 0.99f &&
          trif_value.v[0] < 0.01f;
      projection_filter_ctx.V = v3_make(1.0f, 0.0f, 0.0f);
      projection_filter_ctx.dVdx = v3_make(0.0f, 0.0f, 12.5663706f);
      projection_filter_ctx.has_view_derivatives = 1;
      memset(projection_filter_ctx.memo_done, 0,
             (size_t)projection_filter_doc->nnode);
      MtlxValue latg_value = mtlx_eval_node_test(&projection_filter_ctx, latg);
      projection_filter_ok = projection_filter_ok &&
          fabsf(latg_value.v[0] - latg_value.v[2]) < 0.08f;
      projection_filter_ctx.P.x = 0.25f;
      projection_filter_ctx.dPdx = v3_make(1.0f, 0.0f, 0.0f);
      projection_filter_ctx.dPdy = v3_make(0.0f, 0.0f, 0.0f);
      projection_filter_ctx.has_position_derivatives = 1;
      memset(projection_filter_ctx.memo_done, 0,
             (size_t)projection_filter_doc->nnode);
      MtlxValue trig_value = mtlx_eval_node_test(&projection_filter_ctx, trig);
      projection_filter_ok = projection_filter_ok &&
          fabsf(trig_value.v[0] - trig_value.v[2]) < 0.08f;
    } else {
      projection_filter_ok = 0;
    }
    free(projection_filter_ctx.memo);
    free(projection_filter_ctx.memo_done);
  }
  texcache_free(projection_filter_cache);
  mtlx_free(projection_filter_doc);
  remove(projection_filter_file);
  if (!projection_filter_ok) {
    fprintf(stderr, "projection closest or gradient filtering failed\n");
    return 1;
  }

  /* Verify the cache's color/alpha contract independently of the projection
   * evaluator: RGB is linearized only for an sRGB logical use, while alpha is
   * preserved. Use a minimal uncompressed 32-bit TGA to carry an alpha byte. */
  const char *rgba_file = "lightrt_rgba_regression.tga";
  FILE *rgba_stream = fopen(rgba_file, "wb");
  if (!rgba_stream) return 1;
  unsigned char tga_header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  1, 0, 1, 0, 32, 8};
  unsigned char rgba_pixel[4] = {0, 0, 128, 64}; /* B, G, R, A */
  fwrite(tga_header, 1, sizeof(tga_header), rgba_stream);
  fwrite(rgba_pixel, 1, sizeof(rgba_pixel), rgba_stream);
  fclose(rgba_stream);
  TextureCache *rgba_cache = texcache_create(".");
  int rgba_ok = rgba_cache != NULL;
  float rgba_sample[4] = {0, 0, 0, 1};
  if (rgba_ok) {
    int rgba_id = texcache_get(rgba_cache, rgba_file, 1);
    rgba_ok = rgba_id >= 0;
    if (rgba_ok) {
      texcache_sample(rgba_cache, rgba_id, 0.5f, 0.5f, rgba_sample);
      rgba_ok = rgba_sample[0] > 0.21f && rgba_sample[0] < 0.22f &&
          rgba_sample[1] < 0.01f && rgba_sample[2] < 0.01f &&
          rgba_sample[3] > 0.24f && rgba_sample[3] < 0.26f;
    }
  }
  texcache_free(rgba_cache);
  remove(rgba_file);
  if (!rgba_ok) {
    fprintf(stderr, "sRGB RGB or alpha texture handling failed\n");
    return 1;
  }

  const char *nearest_file = "lightrt_nearest_regression.ppm";
  FILE *nearest_stream = fopen(nearest_file, "wb");
  if (!nearest_stream) return 1;
  fputs("P6\n2 1\n255\n", nearest_stream);
  fputc(255, nearest_stream); fputc(0, nearest_stream); fputc(0, nearest_stream);
  fputc(0, nearest_stream); fputc(0, nearest_stream); fputc(255, nearest_stream);
  fclose(nearest_stream);
  MtlxDoc *nearest_doc = mtlx_load_string(
      "<materialx><image name=\"nearest\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</image><texcoord name=\"uv_source\" type=\"vector2\"/>"
      "<place2d name=\"uv_scaled\" type=\"vector2\">"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_source\"/>"
      "<input name=\"scale\" type=\"vector2\" value=\"0.25,1\"/></place2d>"
      "<constant name=\"uv_constant\" type=\"vector2\">"
      "<input name=\"value\" type=\"vector2\" value=\"0.25,0.5\"/></constant>"
      "<position name=\"uv_position\" type=\"vector3\">"
      "<input name=\"space\" type=\"string\" value=\"world\"/></position>"
      "<convert name=\"uv_from_position\" type=\"vector2\">"
      "<input name=\"in\" type=\"vector3\" nodename=\"uv_position\"/></convert>"
      "<multiply name=\"uv_squared\" type=\"vector2\">"
      "<input name=\"in1\" type=\"vector2\" nodename=\"uv_source\"/>"
      "<input name=\"in2\" type=\"vector2\" nodename=\"uv_source\"/></multiply>"
      "<image name=\"mapped_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_scaled\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<image name=\"constant_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_constant\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<image name=\"swizzle_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_source\" channels=\"yx\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<image name=\"position_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_from_position\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<image name=\"squared_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_squared\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<tiledimage name=\"dynamic_tiling\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" value=\"0.75,0.5\"/>"
      "<input name=\"uvtiling\" type=\"vector2\" nodename=\"uv_source\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></tiledimage>"
      "<tiledimage name=\"dynamic_position\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" value=\"0.75,0.5\"/>"
      "<input name=\"uvtiling\" type=\"vector2\" nodename=\"uv_from_position\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></tiledimage>"
      "<convert name=\"uv_as_color\" type=\"color3\">"
      "<input name=\"in\" type=\"vector2\" nodename=\"uv_scaled\"/></convert>"
      "<add name=\"shared_uv\" type=\"color3\">"
      "<input name=\"in1\" type=\"color3\" nodename=\"mapped_uv\"/>"
      "<input name=\"in2\" type=\"color3\" nodename=\"uv_as_color\"/></add>"
      "<convert name=\"cycle_uv\" type=\"vector2\">"
      "<input name=\"in\" type=\"color3\" nodename=\"cycle_image\"/></convert>"
      "<image name=\"cycle_image\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" nodename=\"cycle_uv\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/></image>"
      "<image name=\"literal_uv\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"texcoord\" type=\"vector2\" value=\"0.25,0.5\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "</image><hextiledimage name=\"hex_nearest\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"filtertype\" type=\"string\" value=\"closest\"/>"
      "<input name=\"rotationrange\" type=\"vector2\" value=\"0,0\"/>"
      "<input name=\"scalerange\" type=\"vector2\" value=\"1,1\"/>"
      "<input name=\"offsetrange\" type=\"vector2\" value=\"0,0\"/>"
      "</hextiledimage><hextiledimage name=\"hex_linear\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"lightrt_nearest_regression.ppm\"/>"
      "<input name=\"rotationrange\" type=\"vector2\" value=\"0,0\"/>"
      "<input name=\"scalerange\" type=\"vector2\" value=\"1,1\"/>"
      "<input name=\"offsetrange\" type=\"vector2\" value=\"0,0\"/>"
      "</hextiledimage></materialx>");
  TextureCache *nearest_cache = texcache_create(".");
  int nearest_ok = nearest_doc && nearest_cache;
  MtlxValue nearest_value;
  memset(&nearest_value, 0, sizeof(nearest_value));
  if (nearest_ok) {
    texcache_preload(nearest_cache, nearest_doc);
    ShadeContext nearest_ctx;
    memset(&nearest_ctx, 0, sizeof(nearest_ctx));
    nearest_ctx.doc = nearest_doc;
    nearest_ctx.tex = nearest_cache;
    nearest_ctx.uv[0] = 0.5f;
    nearest_ctx.uv[1] = 0.37f;
    nearest_ctx.memo = (MtlxValue *)calloc((size_t)nearest_doc->nnode,
                                            sizeof(MtlxValue));
    nearest_ctx.memo_done = (char *)calloc((size_t)nearest_doc->nnode, 1);
    int nearest_node = mtlx_find_node(nearest_doc, -1, "nearest");
    int literal_uv_node = mtlx_find_node(nearest_doc, -1, "literal_uv");
    int mapped_uv_node = mtlx_find_node(nearest_doc, -1, "mapped_uv");
    int constant_uv_node = mtlx_find_node(nearest_doc, -1, "constant_uv");
    int swizzle_uv_node = mtlx_find_node(nearest_doc, -1, "swizzle_uv");
    int position_uv_node = mtlx_find_node(nearest_doc, -1, "position_uv");
    int dynamic_tiling_node = mtlx_find_node(nearest_doc, -1, "dynamic_tiling");
    int dynamic_position_node = mtlx_find_node(nearest_doc, -1, "dynamic_position");
    int squared_uv_node = mtlx_find_node(nearest_doc, -1, "squared_uv");
    int shared_uv_node = mtlx_find_node(nearest_doc, -1, "shared_uv");
    int cycle_image_node = mtlx_find_node(nearest_doc, -1, "cycle_image");
    int hex_nearest_node = mtlx_find_node(nearest_doc, -1, "hex_nearest");
    int hex_linear_node = mtlx_find_node(nearest_doc, -1, "hex_linear");
    if (!nearest_ctx.memo || !nearest_ctx.memo_done || nearest_node < 0 || literal_uv_node < 0 ||
        mapped_uv_node < 0 || constant_uv_node < 0 || swizzle_uv_node < 0 || position_uv_node < 0 ||
        dynamic_tiling_node < 0 || dynamic_position_node < 0 || squared_uv_node < 0 ||
        shared_uv_node < 0 || cycle_image_node < 0 ||
        hex_nearest_node < 0 || hex_linear_node < 0) {
      nearest_ok = 0;
    } else {
      nearest_value = mtlx_eval_node_test(&nearest_ctx, nearest_node);
      nearest_ok = nearest_value.v[2] > 0.99f && nearest_value.v[0] < 0.01f;
      MtlxValue literal_value = mtlx_eval_node_test(&nearest_ctx, literal_uv_node);
      nearest_ok = nearest_ok && literal_value.v[0] > 0.99f && literal_value.v[2] < 0.01f;
      nearest_ctx.has_uv_derivatives = 1;
      nearest_ctx.uv_dx[0] = 1.0f;
      literal_value = mtlx_eval_node_test(&nearest_ctx, literal_uv_node);
      nearest_ok = nearest_ok && literal_value.v[0] > 0.99f && literal_value.v[2] < 0.01f;
      nearest_ctx.has_uv_derivatives = 0;
      nearest_ctx.uv_dx[0] = 0.0f;
      memset(nearest_ctx.memo_done, 0, (size_t)nearest_doc->nnode);
      MtlxValue hex_nearest_value =
          mtlx_eval_node_test(&nearest_ctx, hex_nearest_node);
      memset(nearest_ctx.memo_done, 0, (size_t)nearest_doc->nnode);
      MtlxValue hex_linear_value =
          mtlx_eval_node_test(&nearest_ctx, hex_linear_node);
      nearest_ok = nearest_ok &&
          (fabsf(hex_nearest_value.v[0] - hex_linear_value.v[0]) > 0.001f ||
           fabsf(hex_nearest_value.v[1] - hex_linear_value.v[1]) > 0.001f ||
           fabsf(hex_nearest_value.v[2] - hex_linear_value.v[2]) > 0.001f);
      nearest_ctx.uv[0] = 0.0625f; nearest_ctx.uv[1] = 0.5f;
      nearest_ctx.uv_dx[0] = 0.25f; nearest_ctx.has_uv_derivatives = 1;
      MtlxValue mapped = mtlx_eval_node_test(&nearest_ctx, mapped_uv_node);
      nearest_ok = nearest_ok && fabsf(mapped.v[0] - 0.5f) < 1e-6f &&
                   fabsf(mapped.v[2] - 0.5f) < 1e-6f;
      nearest_ctx.uv_dy[1] = 0.1f;
      MtlxValue shared = mtlx_eval_node_test(&nearest_ctx, shared_uv_node);
      nearest_ok = nearest_ok && fabsf(shared.v[0] - 0.75f) < 1e-6f &&
                   fabsf(shared.v[1] - 0.5f) < 1e-6f &&
                   fabsf(shared.v[2] - 0.5f) < 1e-6f;
      MtlxValue cycle = mtlx_eval_node_test(&nearest_ctx, cycle_image_node);
      nearest_ok = nearest_ok && isfinite(cycle.v[0]) && isfinite(cycle.v[2]);
      nearest_ctx.uv_dy[1] = 0.0f;
      nearest_ctx.uv[0] = 0.25f; nearest_ctx.uv[1] = 0.5f;
      nearest_ctx.uv_dx[0] = 1.0f;
      MtlxValue dynamic_tiling = mtlx_eval_node_test(&nearest_ctx, dynamic_tiling_node);
      nearest_ok = nearest_ok && fabsf(dynamic_tiling.v[0] - 0.75f) < 1e-6f &&
                   fabsf(dynamic_tiling.v[2] - 0.25f) < 1e-6f;
      MtlxValue squared = mtlx_eval_node_test(&nearest_ctx, squared_uv_node);
      nearest_ok = nearest_ok && fabsf(squared.v[0] - 0.25f) < 1e-6f &&
                   fabsf(squared.v[2] - 0.75f) < 1e-6f;
      nearest_ctx.uv_dx[0] = 1.0f;
      MtlxValue constant = mtlx_eval_node_test(&nearest_ctx, constant_uv_node);
      nearest_ok = nearest_ok && constant.v[0] > 0.99f && constant.v[2] < 0.01f;
      nearest_ctx.uv[0] = 0.5f; nearest_ctx.uv[1] = 0.0625f;
      nearest_ctx.uv_dx[0] = 0.0f; nearest_ctx.uv_dx[1] = 1.0f;
      MtlxValue swizzle = mtlx_eval_node_test(&nearest_ctx, swizzle_uv_node);
      nearest_ok = nearest_ok && fabsf(swizzle.v[0] - 0.5f) < 1e-6f &&
                   fabsf(swizzle.v[2] - 0.5f) < 1e-6f;
      nearest_ctx.has_uv_derivatives = 0;
      nearest_ctx.P = v3_make(0.25f, 0.5f, 0.0f);
      nearest_ctx.dPdx = v3_make(1.0f, 0.0f, 0.0f);
      nearest_ctx.has_position_derivatives = 1;
      MtlxValue projected = mtlx_eval_node_test(&nearest_ctx, position_uv_node);
      nearest_ok = nearest_ok && fabsf(projected.v[0] - 0.5f) < 1e-6f &&
                   fabsf(projected.v[2] - 0.5f) < 1e-6f;
      MtlxValue position_tiling = mtlx_eval_node_test(&nearest_ctx, dynamic_position_node);
      nearest_ok = nearest_ok && fabsf(position_tiling.v[0] - 0.75f) < 1e-6f &&
                   fabsf(position_tiling.v[2] - 0.25f) < 1e-6f;
    }
    free(nearest_ctx.memo);
    free(nearest_ctx.memo_done);
  }
  texcache_free(nearest_cache);
  mtlx_free(nearest_doc);
  remove(nearest_file);
  if (!nearest_ok) {
    fprintf(stderr, "nearest image filtering failed\n");
    return 1;
  }

  /* A footprint wider than one texel must integrate the image instead of
   * returning only the bilinear value at the center. */
  const char *gradient_file = "lightrt_gradient_regression.ppm";
  FILE *gradient_stream = fopen(gradient_file, "wb");
  if (!gradient_stream) return 1;
  fputs("P6\n2 1\n255\n", gradient_stream);
  fputc(255, gradient_stream); fputc(0, gradient_stream); fputc(0, gradient_stream);
  fputc(0, gradient_stream); fputc(0, gradient_stream); fputc(255, gradient_stream);
  fclose(gradient_stream);
  TextureCache *gradient_cache = texcache_create(".");
  int gradient_ok = gradient_cache != NULL;
  if (gradient_ok) {
    int gradient_id = texcache_get(gradient_cache, gradient_file, 0);
    float point[4] = {0, 0, 0, 1};
    float footprint[4] = {0, 0, 0, 1};
    gradient_ok = gradient_id >= 0;
    if (gradient_ok) {
      texcache_sample(gradient_cache, gradient_id, 0.25f, 0.5f, point);
      texcache_sample_address_grad(gradient_cache, gradient_id, 0.25f, 0.5f,
                                   1.0f, 0.0f, 0.0f, 0.0f,
                                   "periodic", "periodic", footprint);
      gradient_ok = point[0] > point[2] + 0.2f &&
          fabsf(footprint[0] - footprint[2]) < 0.08f;
    }
  }
  texcache_free(gradient_cache);
  remove(gradient_file);
  if (!gradient_ok) {
    fprintf(stderr, "derivative-aware texture filtering failed\n");
    return 1;
  }

  /* UDIM filtering must resolve taps on both sides of a tile seam. */
  const char *udim_red = "lightrt_udim_seam.1001.ppm";
  const char *udim_blue = "lightrt_udim_seam.1002.ppm";
  FILE *udim_red_stream = fopen(udim_red, "wb");
  FILE *udim_blue_stream = fopen(udim_blue, "wb");
  if (!udim_red_stream || !udim_blue_stream) return 1;
  fputs("P6\n1 1\n255\n", udim_red_stream);
  fputc(255, udim_red_stream); fputc(0, udim_red_stream); fputc(0, udim_red_stream);
  fputs("P6\n1 1\n255\n", udim_blue_stream);
  fputc(0, udim_blue_stream); fputc(0, udim_blue_stream); fputc(255, udim_blue_stream);
  fclose(udim_red_stream); fclose(udim_blue_stream);
  TextureCache *udim_seam_cache = texcache_create(".");
  int udim_ok = udim_seam_cache != NULL;
  if (udim_ok) {
    float seam[4] = {0, 0, 0, 1};
    udim_ok = texcache_sample_file_grad(
        udim_seam_cache, "lightrt_udim_seam.<UDIM>.ppm", 0, 0.99f, 0.5f,
        0.20f, 0.0f, 0.0f, 0.0f, seam);
    udim_ok = udim_ok && fabsf(seam[0] - seam[2]) < 0.08f;
  }
  texcache_free(udim_seam_cache);
  remove(udim_red); remove(udim_blue);
  if (!udim_ok) {
    fprintf(stderr, "UDIM seam footprint filtering failed\n");
    return 1;
  }

  const char *xml =
      "<materialx version=\"1.38\">"
      "  <open_pbr_surface name=\"Preview\" type=\"surfaceshader\">"
      "    <input name=\"base_weight\" type=\"float\" value=\"0.75\"/>"
      "    <input name=\"base_color\" type=\"color3\" value=\"0.2, 0.4, 0.6\"/>"
      "    <input name=\"base_diffuse_roughness\" type=\"float\" value=\"0.12\"/>"
      "    <input name=\"base_metalness\" type=\"float\" value=\"0.5\"/>"
      "    <input name=\"subsurface_radius\" type=\"color3\" value=\"0.2,0.4,0.6\"/>"
      "    <input name=\"subsurface_radius_scale\" type=\"color3\" value=\"2,3,4\"/>"
      "    <input name=\"subsurface_scale\" type=\"float\" value=\"1.5\"/>"
      "    <input name=\"thin_film_thickness\" type=\"float\" value=\"0.45\"/>"
      "    <input name=\"specular_weight\" type=\"float\" value=\"0.25\"/>"
      "    <input name=\"base_roughness\" type=\"float\" value=\"0.35\"/>"
      "    <input name=\"specular_ior\" type=\"float\" value=\"1.6\"/>"
      "    <input name=\"coat_weight\" type=\"float\" value=\"0.3\"/>"
      "    <input name=\"coat_roughness\" type=\"float\" value=\"0.2\"/>"
      "    <input name=\"emission_luminance\" type=\"float\" value=\"2.0\"/>"
      "    <input name=\"emission_color\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "    <input name=\"geometry_opacity\" type=\"float\" value=\"0.65\"/>"
      "  </open_pbr_surface>"
      "  <surfacematerial name=\"Mat\">"
      "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Preview\"/>"
      "  </surfacematerial>"
      "</materialx>";

  OpenPBRParams p;
  if (!eval_xml(xml, &p)) {
    return 1;
  }

  int ok = nearf(p.base_weight, 0.75f) && nearf(p.base_color.x, 0.2f) &&
           nearf(p.base_color.y, 0.4f) && nearf(p.base_color.z, 0.6f) &&
           nearf(p.diffuse_roughness, 0.12f) &&
           nearf(p.metalness, 0.5f) && nearf(p.specular_weight, 0.25f) &&
           nearf(p.subsurface_radius.x, 0.4f) &&
           nearf(p.subsurface_radius.y, 1.2f) &&
           nearf(p.subsurface_radius.z, 2.4f) &&
           nearf(p.subsurface_scale, 1.5f) &&
           nearf(p.thin_film_thickness, 450.0f) &&
           nearf(p.specular_roughness, 0.35f) && nearf(p.specular_ior, 1.6f) &&
           nearf(p.coat_weight, 0.3f) && nearf(p.coat_roughness, 0.2f) &&
           nearf(p.emission, 2.0f) && nearf(p.emission_color.x, 0.1f) &&
           nearf(p.emission_color.y, 0.2f) && nearf(p.emission_color.z, 0.3f) &&
           nearf(p.opacity, 0.65f);

  if (!ok) {
    fprintf(stderr, "unexpected evaluated OpenPBR params\n");
    return 1;
  }

  /* Conical EDF angles are full cone angles. At 90 degrees from the authored
   * axis, a 60/120 degree cone is outside its 60 degree outer half-angle and
   * must emit zero. This also guards the cosine-space smoothstep contract. */
  const char *conical_xml =
      "<materialx version=\"1.39\">"
      " <conical_edf name=\"Cone\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"0.2,0.4,0.8\"/>"
      "  <input name=\"normal\" type=\"vector3\" value=\"0,1,0\"/>"
      "  <input name=\"inner_angle\" type=\"float\" value=\"60\"/>"
      "  <input name=\"outer_angle\" type=\"float\" value=\"120\"/>"
      " </conical_edf>"
      " <surface name=\"ConeSurface\" type=\"surfaceshader\">"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"Cone\"/>"
      " </surface>"
      " <surfacematerial name=\"ConeMat\">"
      "  <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"ConeSurface\"/>"
      " </surfacematerial></materialx>";
  if (!eval_xml(conical_xml, &p) || !nearf(p.emission_color.x, 0.0f) ||
      !nearf(p.emission_color.y, 0.0f) || !nearf(p.emission_color.z, 0.0f)) {
    fprintf(stderr, "MaterialX conical EDF full-angle falloff was not evaluated\n");
    return 1;
  }

  const char *standard_xml =
      "<materialx version=\"1.38\">"
      "  <nodegraph name=\"NG\">"
      "    <combine3 name=\"packed\" type=\"color3\">"
      "      <input name=\"in1\" type=\"float\" value=\"0.2\"/>"
      "      <input name=\"in2\" type=\"float\" value=\"0.4\"/>"
      "      <input name=\"in3\" type=\"float\" value=\"0.6\"/>"
      "    </combine3>"
      "    <extract name=\"green\" type=\"float\">"
      "      <input name=\"in\" type=\"color3\" nodename=\"packed\"/>"
      "      <input name=\"index\" type=\"integer\" value=\"1\"/>"
      "    </extract>"
      "    <convert name=\"green_color\" type=\"color3\">"
      "      <input name=\"in\" type=\"float\" nodename=\"green\"/>"
      "    </convert>"
      "    <add name=\"tint\" type=\"color3\">"
      "      <input name=\"in1\" type=\"color3\" nodename=\"green_color\"/>"
      "      <input name=\"in2\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "    </add>"
      "    <output name=\"base\" type=\"color3\" nodename=\"tint\"/>"
      "  </nodegraph>"
      "  <standard_surface name=\"Standard\" type=\"surfaceshader\">"
      "    <input name=\"base_color\" type=\"color3\" nodegraph=\"NG\" output=\"base\"/>"
      "    <input name=\"roughness\" type=\"float\" value=\"0.42\"/>"
      "    <input name=\"specular_ior\" type=\"float\" value=\"1.55\"/>"
      "    <input name=\"coat_ior\" type=\"float\" value=\"1.45\"/>"
      "    <input name=\"opacity\" type=\"float\" value=\"0.37\"/>"
      "  </standard_surface>"
      "  <surfacematerial name=\"StdMat\">"
      "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Standard\"/>"
      "  </surfacematerial>"
      "</materialx>";
  if (!eval_xml(standard_xml, &p)) {
    return 1;
  }
  ok = nearf(p.base_color.x, 0.5f) && nearf(p.base_color.y, 0.6f) &&
       nearf(p.base_color.z, 0.7f) && nearf(p.specular_roughness, 0.42f) &&
       nearf(p.specular_ior, 1.55f) && nearf(p.coat_ior, 1.45f) &&
       nearf(p.opacity, 0.37f);
  if (!ok) {
    fprintf(stderr, "unexpected evaluated standard_surface graph params\n");
    return 1;
  }

  const char *channels_xml =
      "<materialx version=\"1.38\">"
      "  <constant name=\"Packed\" type=\"color4\">"
      "    <input name=\"value\" type=\"color4\" value=\"0.1, 0.2, 0.3, 0.4\"/>"
      "  </constant>"
      "  <swizzle name=\"Bgr\" type=\"color3\">"
      "    <input name=\"in\" type=\"color4\" nodename=\"Packed\"/>"
      "    <input name=\"channels\" type=\"string\" value=\"bgr\"/>"
      "  </swizzle>"
      "  <open_pbr_surface name=\"Channels\" type=\"surfaceshader\">"
      "    <input name=\"base_color\" type=\"color3\" nodename=\"Bgr\"/>"
      "    <input name=\"base_roughness\" type=\"float\" nodename=\"Packed\" channels=\"g\"/>"
      "    <input name=\"geometry_opacity\" type=\"float\" nodename=\"Packed\" channels=\"a\"/>"
      "  </open_pbr_surface>"
      "  <surfacematerial name=\"ChannelMat\">"
      "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Channels\"/>"
      "  </surfacematerial>"
      "</materialx>";
  if (!eval_xml(channels_xml, &p)) {
    return 1;
  }
  ok = nearf(p.base_color.x, 0.3f) && nearf(p.base_color.y, 0.2f) &&
       nearf(p.base_color.z, 0.1f) && nearf(p.specular_roughness, 0.2f) &&
       nearf(p.opacity, 0.4f);
  if (!ok) {
    fprintf(stderr, "MaterialX channel selectors were not evaluated\n");
    return 1;
  }

  const char *unlit_xml =
      "<materialx version=\"1.39\">"
      " <surface_unlit name=\"Unlit\" type=\"surfaceshader\">"
      "  <input name=\"emission\" type=\"float\" value=\"2.5\"/>"
      "  <input name=\"emission_color\" type=\"color3\" value=\"0.2,0.4,0.8\"/>"
      "  <input name=\"transmission\" type=\"float\" value=\"0.3\"/>"
      "  <input name=\"transmission_color\" type=\"color3\" value=\"0.7,0.6,0.5\"/>"
      "  <input name=\"opacity\" type=\"float\" value=\"0.45\"/>"
      " </surface_unlit>"
      " <surfacematerial name=\"UnlitMat\"><input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Unlit\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(unlit_xml, &p)) return 1;
  ok = nearf(p.base_weight, 0.0f) && nearf(p.specular_weight, 0.0f) &&
       nearf(p.emission, 2.5f) && nearf(p.emission_color.x, 0.2f) &&
       nearf(p.emission_color.y, 0.4f) && nearf(p.emission_color.z, 0.8f) &&
       nearf(p.transmission, 0.3f) && nearf(p.transmission_color.x, 0.7f) &&
       nearf(p.transmission_color.y, 0.6f) && nearf(p.transmission_color.z, 0.5f) &&
       nearf(p.opacity, 0.45f);
  if (!ok) {
    fprintf(stderr, "surface_unlit parameters were not evaluated\n");
    return 1;
  }

  const char *closure_xml =
      "<materialx version=\"1.39\">"
      " <diffuse_bsdf name=\"Diffuse\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.4\"/>"
      "  <input name=\"color\" type=\"color3\" value=\"0.2,0.3,0.4\"/>"
      "  <input name=\"roughness\" type=\"float\" value=\"0.25\"/>"
      " </diffuse_bsdf>"
      " <dielectric_bsdf name=\"Glass\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.6\"/>"
      "  <input name=\"tint\" type=\"color3\" value=\"0.8,0.9,1\"/>"
      "  <input name=\"ior\" type=\"float\" value=\"1.45\"/>"
      "  <input name=\"roughness\" type=\"vector2\" value=\"0.12,0.2\"/>"
      "  <input name=\"scatter_mode\" type=\"string\" value=\"RT\"/>"
      " </dielectric_bsdf>"
      " <add name=\"Combined\" type=\"BSDF\">"
      "  <input name=\"in1\" type=\"BSDF\" nodename=\"Diffuse\"/>"
      "  <input name=\"in2\" type=\"BSDF\" nodename=\"Glass\"/>"
      " </add>"
      " <uniform_edf name=\"Emit\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"0.1,0.2,0.3\"/>"
      " </uniform_edf>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"Combined\"/>"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"Emit\"/>"
      "  <input name=\"opacity\" type=\"float\" value=\"0.75\"/>"
      " </surface>"
      " <surfacematerial name=\"ClosureMat\" type=\"material\">"
      "  <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Surface\"/>"
      " </surfacematerial>"
      "</materialx>";
  if (!eval_xml(closure_xml, &p)) return 1;
  ok = nearf(p.base_weight, 0.4f) && nearf(p.base_color.x, 0.2f) &&
       nearf(p.diffuse_roughness, 0.25f) &&
       nearf(p.specular_weight, 0.6f) && nearf(p.specular_ior, 1.45f) &&
       nearf(p.specular_roughness, 0.12f) && nearf(p.transmission, 0.6f) &&
       nearf(p.emission_color.z, 0.3f) && nearf(p.opacity, 0.75f);
  if (!ok) {
    fprintf(stderr, "MaterialX surface closure graph was not evaluated: "
            "base=%g color=%g rough=%g spec=%g ior=%g srough=%g trans=%g "
            "emit=%g opacity=%g\n", p.base_weight, p.base_color.x,
            p.diffuse_roughness, p.specular_weight, p.specular_ior,
            p.specular_roughness, p.transmission, p.emission_color.z,
            p.opacity);
    return 1;
  }

  const char *weighted_roughness_xml =
      "<materialx version=\"1.39\">"
      " <dielectric_bsdf name=\"GlassA\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.5\"/>"
      "  <input name=\"roughness\" type=\"float\" value=\"0.1\"/>"
      " </dielectric_bsdf>"
      " <dielectric_bsdf name=\"GlassB\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.5\"/>"
      "  <input name=\"roughness\" type=\"float\" value=\"0.5\"/>"
      " </dielectric_bsdf>"
      " <add name=\"Combined\" type=\"BSDF\">"
      "  <input name=\"in1\" type=\"BSDF\" nodename=\"GlassA\"/>"
      "  <input name=\"in2\" type=\"BSDF\" nodename=\"GlassB\"/>"
      " </add>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"Combined\"/>"
      " </surface>"
      " <surfacematerial name=\"Mat\"><input name=\"surfaceshader\""
      " type=\"surfaceshader\" nodename=\"Surface\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(weighted_roughness_xml, &p) ||
      !nearf(p.specular_weight, 1.0f) ||
      !nearf(p.specular_roughness, 0.3f)) {
    fprintf(stderr, "composed specular roughness was not weighted\n");
    return 1;
  }

  const char *light_wrapper_xml =
      "<materialx version=\"1.39\">"
      " <uniform_edf name=\"Source\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"0.2,0.4,0.6\"/>"
      " </uniform_edf>"
      " <light name=\"Wrapped\" type=\"EDF\">"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"Source\"/>"
      "  <input name=\"intensity\" type=\"color3\" value=\"2,1,0.5\"/>"
      "  <input name=\"exposure\" type=\"float\" value=\"1\"/>"
      " </light>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"Wrapped\"/>"
      " </surface>"
      " <surfacematerial name=\"Mat\"><input name=\"surfaceshader\""
      " type=\"surfaceshader\" nodename=\"Surface\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(light_wrapper_xml, &p) ||
      !nearf(p.emission_color.x, 0.8f) ||
      !nearf(p.emission_color.y, 0.8f) ||
      !nearf(p.emission_color.z, 0.6f)) {
    fprintf(stderr, "MaterialX light wrapper was not evaluated\n");
    return 1;
  }

  const char *scatter_mode_xml =
      "<materialx version=\"1.39\">"
      " <dielectric_bsdf name=\"Glass\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.6\"/>"
      "  <input name=\"tint\" type=\"color3\" value=\"0.8,0.9,1\"/>"
      "  <input name=\"scatter_mode\" type=\"string\" value=\"T\"/>"
      " </dielectric_bsdf>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"Glass\"/>"
      " </surface>"
      " <surfacematerial name=\"GlassMat\"><input name=\"surfaceshader\""
      " type=\"surfaceshader\" nodename=\"Surface\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(scatter_mode_xml, &p) || !nearf(p.specular_weight, 0.0f) ||
      !nearf(p.transmission, 0.6f)) {
    fprintf(stderr, "MaterialX transmission-only scatter mode failed\n");
    return 1;
  }

  const char *invalid_scatter_mode_xml =
      "<materialx version=\"1.39\">"
      " <dielectric_bsdf name=\"Glass\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.6\"/>"
      "  <input name=\"scatter_mode\" type=\"string\" value=\"TR\"/>"
      " </dielectric_bsdf>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"Glass\"/>"
      " </surface>"
      " <surfacematerial name=\"GlassMat\"><input name=\"surfaceshader\""
      " type=\"surfaceshader\" nodename=\"Surface\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(invalid_scatter_mode_xml, &p) ||
      !nearf(p.specular_weight, 0.0f) || !nearf(p.transmission, 0.0f)) {
    fprintf(stderr, "invalid MaterialX scatter mode was accepted\n");
    return 1;
  }

  const char *schlick_transmission_xml =
      "<materialx version=\"1.39\">"
      " <generalized_schlick_bsdf name=\"Glass\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.6\"/>"
      "  <input name=\"color0\" type=\"color3\" value=\"0.2,0.3,0.4\"/>"
      "  <input name=\"color90\" type=\"color3\" value=\"0.4,0.5,0.6\"/>"
      "  <input name=\"scatter_mode\" type=\"string\" value=\"T\"/>"
      " </generalized_schlick_bsdf>"
      " <surface name=\"Surface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"Glass\"/>"
      " </surface>"
      " <surfacematerial name=\"GlassMat\"><input name=\"surfaceshader\""
      " type=\"surfaceshader\" nodename=\"Surface\"/></surfacematerial>"
      "</materialx>";
  if (!eval_xml(schlick_transmission_xml, &p) ||
      !nearf(p.specular_weight, 0.0f) || !nearf(p.transmission, 0.6f) ||
      !nearf(p.transmission_color.x, 1.0f) ||
      !nearf(p.transmission_color.y, 1.0f) ||
      !nearf(p.transmission_color.z, 1.0f)) {
    fprintf(stderr, "generalized Schlick transmission tint was not neutral\n");
    return 1;
  }

  const char *volume_xml =
      "<materialx version=\"1.39\">"
      " <anisotropic_vdf name=\"Fog\" type=\"VDF\">"
      "  <input name=\"absorption\" type=\"vector3\" value=\"0.1,0.2,0.3\"/>"
      "  <input name=\"scattering\" type=\"vector3\" value=\"0.4,0.5,0.6\"/>"
      "  <input name=\"anisotropy\" type=\"float\" value=\"0.35\"/>"
      " </anisotropic_vdf>"
      " <absorption_vdf name=\"FogFill\" type=\"VDF\">"
      "  <input name=\"absorption\" type=\"vector3\" value=\"0.2,0.1,0.0\"/>"
      " </absorption_vdf>"
      " <mix name=\"MixedFog\" type=\"VDF\">"
      "  <input name=\"bg\" type=\"VDF\" nodename=\"Fog\"/>"
      "  <input name=\"fg\" type=\"VDF\" nodename=\"FogFill\"/>"
      "  <input name=\"mix\" type=\"float\" value=\"0.25\"/>"
      " </mix>"
      " <uniform_edf name=\"Glow\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"2,1,0.5\"/>"
      " </uniform_edf>"
      " <uniform_edf name=\"Fill\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"0.25,0.5,0.75\"/>"
      " </uniform_edf>"
      " <mix name=\"MixedGlow\" type=\"EDF\">"
      "  <input name=\"bg\" type=\"EDF\" nodename=\"Glow\"/>"
      "  <input name=\"fg\" type=\"EDF\" nodename=\"Fill\"/>"
      "  <input name=\"mix\" type=\"float\" value=\"0.25\"/>"
      " </mix>"
      " <volume name=\"Volume\" type=\"volumeshader\">"
      "  <input name=\"vdf\" type=\"VDF\" nodename=\"MixedFog\"/>"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"MixedGlow\"/>"
      " </volume>"
      " <volumematerial name=\"VolumeMat\" type=\"material\">"
      "  <input name=\"volumeshader\" type=\"volumeshader\" nodename=\"Volume\"/>"
      " </volumematerial>"
      "</materialx>";
  MtlxVolumeParams volume;
  if (!eval_volume_xml(volume_xml, &volume)) return 1;
  ok = nearf(volume.absorption.x, 0.125f) &&
       nearf(volume.absorption.y, 0.175f) &&
       nearf(volume.absorption.z, 0.225f) &&
       nearf(volume.scattering.x, 0.3f) &&
       nearf(volume.scattering.y, 0.375f) &&
       nearf(volume.scattering.z, 0.45f) &&
       nearf(volume.anisotropy, 0.35f) &&
       volume.lobe_count == 1 &&
       nearf(volume.lobes[0].anisotropy, 0.35f) &&
       nearf(volume.lobes[0].scattering.y, 0.375f) &&
       nearf(volume.emission.x, 1.5625f) &&
       nearf(volume.emission.y, 0.875f) &&
       nearf(volume.emission.z, 0.5625f);
  if (!ok) {
    fprintf(stderr, "MaterialX volume/VDF/EDF graph was not evaluated\n");
    return 1;
  }

  /* Multiple VDF closures retain their individual phase functions. */
  const char *two_lobes_xml =
      "<materialx version=\"1.39\">"
      "<anisotropic_vdf name=\"a\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"1,0,0\"/><input name=\"anisotropy\" type=\"float\" value=\"0.6\"/></anisotropic_vdf>"
      "<anisotropic_vdf name=\"b\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"0,0,2\"/><input name=\"anisotropy\" type=\"float\" value=\"-0.6\"/></anisotropic_vdf>"
      "<add name=\"sum\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"a\"/><input name=\"in2\" type=\"VDF\" nodename=\"b\"/></add>"
      "<volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"sum\"/></volume>"
      "<volumematerial name=\"M\"><input name=\"volumeshader\" type=\"volumeshader\" nodename=\"V\"/></volumematerial></materialx>";
  if (!eval_volume_xml(two_lobes_xml, &volume) || volume.lobe_count != 2 ||
      !nearf(volume.lobes[0].anisotropy, 0.6f) ||
      !nearf(volume.lobes[1].anisotropy, -0.6f) ||
      !nearf(volume.lobes[0].scattering.x, 1.0f) ||
      !nearf(volume.lobes[1].scattering.z, 2.0f)) {
    fprintf(stderr, "MaterialX volume phase lobes were not preserved\n");
    return 1;
  }

  /* Measured profiles require IES evaluation. The volume evaluator must
   * reject an unsupported profile rather than silently ignore the file. */
  const char *measured_volume_xml =
      "<materialx version=\"1.39\">"
      " <measured_edf name=\"MeasuredGlow\" type=\"EDF\">"
      "  <input name=\"color\" type=\"color3\" value=\"0.15,0.35,0.65\"/>"
      "  <input name=\"file\" type=\"filename\" value=\"profile.ies\"/>"
      " </measured_edf>"
      " <volume name=\"MeasuredVolume\" type=\"volumeshader\">"
      "  <input name=\"edf\" type=\"EDF\" nodename=\"MeasuredGlow\"/>"
      " </volume>"
      " <volumematerial name=\"MeasuredMat\" type=\"material\">"
      "  <input name=\"volumeshader\" type=\"volumeshader\" nodename=\"MeasuredVolume\"/>"
      " </volumematerial></materialx>";
  if (eval_volume_xml(measured_volume_xml, &volume)) {
    fprintf(stderr, "MaterialX measured EDF was accepted without IES evaluation\n");
    return 1;
  }

  /* A volume's VDF inputs may be ordinary MaterialX image nodes. Exercise the
   * texture-backed path separately from the constant-only graph above. */
  const char *volume_texture_file = "lightrt_volume_regression.ppm";
  FILE *volume_texture = fopen(volume_texture_file, "wb");
  if (!volume_texture) return 1;
  fputs("P6\n1 1\n255\n", volume_texture);
  fputc(51, volume_texture); fputc(102, volume_texture);
  fputc(153, volume_texture);
  fclose(volume_texture);
  const char *volume_texture_xml =
      "<materialx version=\"1.39\">"
      " <image name=\"volume_tex\" type=\"color3\"><input name=\"file\""
      " type=\"filename\" value=\"lightrt_volume_regression.ppm\"/></image>"
      " <absorption_vdf name=\"TexturedFog\" type=\"VDF\">"
      "  <input name=\"absorption\" type=\"color3\" nodename=\"volume_tex\"/>"
      " </absorption_vdf>"
      " <volume name=\"TexturedVolume\" type=\"volumeshader\">"
      "  <input name=\"vdf\" type=\"VDF\" nodename=\"TexturedFog\"/>"
      " </volume>"
      " <volumematerial name=\"TexturedMat\" type=\"material\">"
      "  <input name=\"volumeshader\" type=\"volumeshader\""
      " nodename=\"TexturedVolume\"/></volumematerial></materialx>";
  MtlxDoc *volume_texture_doc = mtlx_load_string(volume_texture_xml);
  TextureCache *volume_texture_cache = texcache_create(".");
  int volume_texture_ok = volume_texture_doc && volume_texture_cache;
  if (volume_texture_ok) {
    texcache_preload(volume_texture_cache, volume_texture_doc);
    ShadeContext volume_texture_ctx;
    memset(&volume_texture_ctx, 0, sizeof(volume_texture_ctx));
    volume_texture_ctx.doc = volume_texture_doc;
    volume_texture_ctx.tex = volume_texture_cache;
    volume_texture_ctx.uv[0] = volume_texture_ctx.uv[1] = 0.5f;
    volume_texture_ctx.memo = (MtlxValue *)calloc(
        (size_t)volume_texture_doc->nnode, sizeof(MtlxValue));
    volume_texture_ctx.memo_done = (char *)calloc(
        (size_t)volume_texture_doc->nnode, 1);
    const int volume_node = volume_texture_doc->mats[0].volume_node;
    volume_texture_ok = volume_texture_ctx.memo && volume_texture_ctx.memo_done &&
        volume_node >= 0 && mtlx_eval_volume(
            &volume_texture_ctx, volume_node, &volume) == 0 &&
        nearf(volume.absorption.x, 0.2f) &&
        nearf(volume.absorption.y, 0.4f) &&
        nearf(volume.absorption.z, 0.6f);
    free(volume_texture_ctx.memo);
    free(volume_texture_ctx.memo_done);
  }
  texcache_free(volume_texture_cache);
  mtlx_free(volume_texture_doc);
  remove(volume_texture_file);
  if (!volume_texture_ok) {
    fprintf(stderr, "texture-backed MaterialX volume was not evaluated\n");
    return 1;
  }

  const char *subsurface_volume_xml =
      "<materialx version=\"1.39\">"
      " <subsurface_vdf name=\"Subsurface\" type=\"VDF\">"
      "  <input name=\"scattering\" type=\"color3\" value=\"0.3,0.5,0.7\"/>"
      "  <input name=\"anisotropy\" type=\"float\" value=\"0.2\"/>"
      " </subsurface_vdf>"
      " <volume name=\"SubsurfaceVolume\" type=\"volumeshader\">"
      "  <input name=\"vdf\" type=\"VDF\" nodename=\"Subsurface\"/>"
      " </volume>"
      " <volumematerial name=\"SubsurfaceMat\" type=\"material\">"
      "  <input name=\"volumeshader\" type=\"volumeshader\" nodename=\"SubsurfaceVolume\"/>"
      " </volumematerial></materialx>";
  if (!eval_volume_xml(subsurface_volume_xml, &volume) ||
      !nearf(volume.scattering.x, 0.3f) ||
      !nearf(volume.scattering.y, 0.5f) ||
      !nearf(volume.scattering.z, 0.7f) ||
      !nearf(volume.anisotropy, 0.2f)) {
    fprintf(stderr, "MaterialX subsurface VDF was not evaluated\n");
    return 1;
  }

  const char *subsurface_bsdf_xml =
      "<materialx version=\"1.39\">"
      " <subsurface_bsdf name=\"SubsurfaceBsdf\" type=\"BSDF\">"
      "  <input name=\"weight\" type=\"float\" value=\"0.7\"/>"
      "  <input name=\"color\" type=\"color3\" value=\"0.2,0.4,0.6\"/>"
      "  <input name=\"radius\" type=\"color3\" value=\"0.3,0.5,0.7\"/>"
      "  <input name=\"scale\" type=\"float\" value=\"2.5\"/>"
      "  <input name=\"anisotropy\" type=\"float\" value=\"0.2\"/>"
      " </subsurface_bsdf>"
      " <surface name=\"SubsurfaceSurface\" type=\"surfaceshader\">"
      "  <input name=\"bsdf\" type=\"BSDF\" nodename=\"SubsurfaceBsdf\"/>"
      " </surface>"
      " <surfacematerial name=\"SubsurfaceMat\" type=\"material\">"
      "  <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"SubsurfaceSurface\"/>"
      " </surfacematerial></materialx>";
  OpenPBRParams subsurface;
  if (!eval_xml(subsurface_bsdf_xml, &subsurface) ||
      !nearf(subsurface.subsurface, 0.7f) ||
      !nearf(subsurface.subsurface_radius.x, 0.3f) ||
      !nearf(subsurface.subsurface_radius.y, 0.5f) ||
      !nearf(subsurface.subsurface_radius.z, 0.7f) ||
      !nearf(subsurface.subsurface_scale, 2.5f) ||
      !nearf(subsurface.subsurface_anisotropy, 0.2f)) {
    fprintf(stderr, "MaterialX subsurface BSDF scale was not evaluated\n");
    return 1;
  }

  return 0;
}
