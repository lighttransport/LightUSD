// SPDX-License-Identifier: Apache-2.0
#include "mtlx_doc.h"
#include "mtlx_eval.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nearf_eps(float a, float b) { return fabsf(a - b) <= 1.0e-5f; }

static int lookup_geomprop(void *user,const char *name,MtlxType type,
                           MtlxValue *out){
  (void)user;
  if(strcmp(name,"temperature")||type!=MV_FLOAT)return 0;
  memset(out,0,sizeof(*out));out->type=MV_FLOAT;out->v[0]=6500.0f;return 1;
}

static int check1(const char *name, MtlxValue value, float expected) {
  if (!nearf_eps(value.v[0], expected)) {
    fprintf(stderr, "%s: got %.9f, expected %.9f\n", name, value.v[0],
            expected);
    return 0;
  }
  return 1;
}

static int check3(const char *name, MtlxValue value, float x, float y, float z) {
  if (!nearf_eps(value.v[0], x) || !nearf_eps(value.v[1], y) ||
      !nearf_eps(value.v[2], z)) {
    fprintf(stderr, "%s: got %.6f %.6f %.6f, expected %.6f %.6f %.6f\n",
            name, value.v[0], value.v[1], value.v[2], x, y, z);
    return 0;
  }
  return 1;
}

static int check4(const char *name, MtlxValue value, float x, float y, float z,
                  float w) {
  return check3(name, value, x, y, z) && nearf_eps(value.v[3], w);
}

static MtlxValue eval_named(ShadeContext *ctx, const char *name) {
  int id = mtlx_find_node(ctx->doc, -1, name);
  for (int graph = 0; id < 0 && graph < ctx->doc->ngraph; ++graph)
    id = mtlx_find_node(ctx->doc, graph, name);
  return mtlx_eval_node_test(ctx, id);
}

int main(void) {
  const char *xml =
      "<materialx version=\"1.39\"><nodegraph name=\"NG\">"
      " <constant name=\"src\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"0.2,0.4,0.8\"/></constant>"
      " <add name=\"add\" type=\"color3\"><input name=\"in1\" type=\"color3\" nodename=\"src\"/><input name=\"in2\" type=\"color3\" value=\"0.1,0.2,0.3\"/></add>"
      " <multiply name=\"mul\" type=\"color3\"><input name=\"in1\" type=\"color3\" nodename=\"add\"/><input name=\"in2\" type=\"float\" value=\"2\"/></multiply>"
      " <clamp name=\"clamped\" type=\"color3\"><input name=\"in\" nodename=\"mul\"/><input name=\"low\" value=\"0.0\"/><input name=\"high\" value=\"1.0\"/></clamp>"
      " <texcoord name=\"st\" type=\"vector2\"/>"
      " <ramplr name=\"ramp\" type=\"color3\"><input name=\"valuel\" type=\"color3\" value=\"1,0,0\"/><input name=\"valuer\" type=\"color3\" value=\"0,0,1\"/><input name=\"texcoord\" type=\"vector2\" nodename=\"st\"/></ramplr>"
      " <splitlr name=\"split\" type=\"color3\"><input name=\"valuel\" type=\"color3\" value=\"0,1,0\"/><input name=\"valuer\" type=\"color3\" value=\"1,0,1\"/><input name=\"center\" type=\"float\" value=\"0.6\"/><input name=\"texcoord\" type=\"vector2\" nodename=\"st\"/></splitlr>"
      " <ramptb name=\"ramp_tb\" type=\"color3\"><input name=\"valuet\" type=\"color3\" value=\"1,1,0\"/><input name=\"valueb\" type=\"color3\" value=\"0,1,1\"/><input name=\"texcoord\" type=\"vector2\" nodename=\"st\"/></ramptb>"
      " <splittb name=\"split_tb\" type=\"color3\"><input name=\"valuet\" type=\"color3\" value=\"1,0,0\"/><input name=\"valueb\" type=\"color3\" value=\"0,0,1\"/><input name=\"center\" type=\"float\" value=\"0.8\"/><input name=\"texcoord\" type=\"vector2\" nodename=\"st\"/></splittb>"
      " <screen name=\"screen\" type=\"color3\"><input name=\"fg\" type=\"color3\" value=\"0.2,0.5,0.8\"/><input name=\"bg\" type=\"color3\" value=\"0.1,0.4,0.7\"/><input name=\"mix\" type=\"float\" value=\"0.5\"/></screen>"
      " <overlay name=\"overlay\" type=\"color3\"><input name=\"fg\" type=\"color3\" value=\"0.2,0.5,0.8\"/><input name=\"bg\" type=\"color3\" value=\"0.1,0.4,0.7\"/><input name=\"mix\" type=\"float\" value=\"0.5\"/></overlay>"
      " <burn name=\"burn\" type=\"float\"><input name=\"fg\" type=\"float\" value=\"0.5\"/><input name=\"bg\" type=\"float\" value=\"0.25\"/><input name=\"mix\" type=\"float\" value=\"0.5\"/></burn>"
      " <dodge name=\"dodge\" type=\"float\"><input name=\"fg\" type=\"float\" value=\"0.5\"/><input name=\"bg\" type=\"float\" value=\"0.25\"/><input name=\"mix\" type=\"float\" value=\"0.5\"/></dodge>"
      " <saturate name=\"desaturate\" type=\"color3\"><input name=\"in\" type=\"color3\" value=\"0.2,0.4,0.8\"/><input name=\"amount\" type=\"float\" value=\"0\"/></saturate>"
      " <dotproduct name=\"dot\" type=\"float\"><input name=\"in1\" type=\"vector3\" value=\"1,2,3\"/><input name=\"in2\" type=\"vector3\" value=\"4,5,6\"/></dotproduct>"
      " <normalize name=\"norm\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"0,3,4\"/></normalize>"
      " <fraction name=\"fraction\" type=\"color3\"><input name=\"in\" type=\"color3\" value=\"-1.25,1.5,2.75\"/></fraction>"
      " <step name=\"step\" type=\"color3\"><input name=\"in\" type=\"color3\" value=\"0.2,0.5,0.8\"/><input name=\"edge\" type=\"float\" value=\"0.5\"/></step>"
      " <rgbtohsv name=\"hsv\" type=\"color3\"><input name=\"in\" type=\"color3\" value=\"0.2,0.4,0.8\"/></rgbtohsv>"
      " <hsvtorgb name=\"rgb\" type=\"color3\"><input name=\"in\" type=\"color3\" nodename=\"hsv\"/></hsvtorgb>"
      " <rotate2d name=\"rotate\" type=\"vector2\"><input name=\"in\" type=\"vector2\" value=\"1,0\"/><input name=\"amount\" type=\"float\" value=\"90\"/></rotate2d>"
      " <ramp4 name=\"quad\" type=\"color3\"><input name=\"valuetl\" type=\"color3\" value=\"1,0,0\"/><input name=\"valuetr\" type=\"color3\" value=\"0,1,0\"/><input name=\"valuebl\" type=\"color3\" value=\"0,0,1\"/><input name=\"valuebr\" type=\"color3\" value=\"1,1,1\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.25,0.75\"/></ramp4>"
      " <ramp name=\"full_ramp\" type=\"color4\"><input name=\"texcoord\" type=\"vector2\" value=\"0.25,0.5\"/><input name=\"interpolation\" type=\"integer\" value=\"0\"/><input name=\"num_intervals\" type=\"integer\" value=\"3\"/><input name=\"interval1\" type=\"float\" value=\"0\"/><input name=\"color1\" type=\"color4\" value=\"0,0,0,1\"/><input name=\"interval2\" type=\"float\" value=\"0.5\"/><input name=\"color2\" type=\"color4\" value=\"1,0,0,1\"/><input name=\"interval3\" type=\"float\" value=\"1\"/><input name=\"color3\" type=\"color4\" value=\"1,1,1,1\"/></ramp>"
      " <ramp_gradient name=\"gradient\" type=\"color4\"><input name=\"x\" type=\"float\" value=\"0.25\"/><input name=\"interval1\" type=\"float\" value=\"0\"/><input name=\"interval2\" type=\"float\" value=\"1\"/><input name=\"color1\" type=\"color4\" value=\"0,0,0,1\"/><input name=\"color2\" type=\"color4\" value=\"1,0.5,0,1\"/><input name=\"interpolation\" type=\"integer\" value=\"1\"/><input name=\"prev_color\" type=\"color4\" value=\"0,0,1,1\"/><input name=\"interval_num\" type=\"integer\" value=\"1\"/><input name=\"num_intervals\" type=\"integer\" value=\"2\"/></ramp_gradient>"
      " <blur name=\"blur\" type=\"color3\"><input name=\"in\" type=\"color3\" nodename=\"src\"/><input name=\"size\" type=\"float\" value=\"0.5\"/><input name=\"filtertype\" type=\"string\" value=\"gaussian\"/></blur>"
      " <flake2d name=\"flakes\" type=\"multioutput\"><input name=\"size\" type=\"float\" value=\"0.2\"/><input name=\"roughness\" type=\"float\" value=\"0.25\"/><input name=\"coverage\" type=\"float\" value=\"1\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.37,0.61\"/><input name=\"normal\" type=\"vector3\" value=\"0,0,1\"/><input name=\"tangent\" type=\"vector3\" value=\"1,0,0\"/><input name=\"bitangent\" type=\"vector3\" value=\"0,1,0\"/></flake2d>"
      " <multiply name=\"flake_presence\" type=\"float\"><input name=\"in1\" type=\"float\" nodename=\"flakes\" output=\"presence\"/><input name=\"in2\" type=\"float\" value=\"1\"/></multiply>"
      " <multiply name=\"flake_rand\" type=\"float\"><input name=\"in1\" type=\"float\" nodename=\"flakes\" output=\"rand\"/><input name=\"in2\" type=\"float\" value=\"1\"/></multiply>"
      " <creatematrix name=\"matrix\" type=\"matrix33\"><input name=\"in1\" type=\"vector3\" value=\"2,0,0\"/><input name=\"in2\" type=\"vector3\" value=\"0,3,0\"/><input name=\"in3\" type=\"vector3\" value=\"0,0,4\"/></creatematrix>"
      " <determinant name=\"matrix_det\" type=\"float\"><input name=\"in\" type=\"matrix33\" nodename=\"matrix\"/></determinant>"
      " <transformmatrix name=\"matrix_transform\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"mat\" type=\"matrix33\" nodename=\"matrix\"/></transformmatrix>"
      " <invertmatrix name=\"matrix_inverse\" type=\"matrix33\"><input name=\"in\" type=\"matrix33\" nodename=\"matrix\"/></invertmatrix>"
      " <transformmatrix name=\"matrix_roundtrip\" type=\"vector3\"><input name=\"in\" type=\"vector3\" nodename=\"matrix_transform\"/><input name=\"mat\" type=\"matrix33\" nodename=\"matrix_inverse\"/></transformmatrix>"
      " <transpose name=\"matrix_transpose\" type=\"matrix33\"><input name=\"in\" type=\"matrix33\" nodename=\"matrix\"/></transpose>"
      " <determinant name=\"transpose_det\" type=\"float\"><input name=\"in\" type=\"matrix33\" nodename=\"matrix_transpose\"/></determinant>"
      " <creatematrix name=\"matrix4\" type=\"matrix44\"><input name=\"in1\" type=\"vector3\" value=\"1,0,0\"/><input name=\"in2\" type=\"vector3\" value=\"0,1,0\"/><input name=\"in3\" type=\"vector3\" value=\"0,0,1\"/><input name=\"in4\" type=\"vector3\" value=\"5,6,7\"/></creatematrix>"
      " <transformmatrix name=\"matrix4_transform\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"mat\" type=\"matrix44\" nodename=\"matrix4\"/></transformmatrix>"
      " <determinant name=\"matrix4_det\" type=\"float\"><input name=\"in\" type=\"matrix44\" nodename=\"matrix4\"/></determinant>"
      " <latlongimage name=\"latlong_missing\" type=\"color3\"><input name=\"file\" type=\"filename\" value=\"\"/><input name=\"default\" type=\"color3\" value=\"0.125,0.25,0.5\"/><input name=\"viewdir\" type=\"vector3\" value=\"1,0,0\"/><input name=\"rotation\" type=\"float\" value=\"90\"/></latlongimage>"
      " <triplanarprojection name=\"triplanar_missing\" type=\"color3\"><input name=\"default\" type=\"color3\" value=\"0.2,0.4,0.8\"/><input name=\"position\" type=\"vector3\" value=\"1,2,3\"/><input name=\"normal\" type=\"vector3\" value=\"1,2,3\"/><input name=\"upaxis\" type=\"integer\" value=\"2\"/><input name=\"blend\" type=\"float\" value=\"0.25\"/></triplanarprojection>"
      " <extract name=\"height_ramp\" type=\"float\"><input name=\"in\" type=\"vector2\" nodename=\"st\"/><input name=\"index\" type=\"integer\" value=\"0\"/></extract>"
      " <heighttonormal name=\"height_normal\" type=\"vector3\"><input name=\"in\" type=\"float\" nodename=\"height_ramp\"/><input name=\"scale\" type=\"float\" value=\"4\"/></heighttonormal>"
      " <bump name=\"bump_normal\" type=\"vector3\"><input name=\"height\" type=\"float\" nodename=\"height_ramp\"/><input name=\"scale\" type=\"float\" value=\"4\"/><input name=\"normal\" type=\"vector3\" value=\"0,0,1\"/><input name=\"tangent\" type=\"vector3\" value=\"1,0,0\"/><input name=\"bitangent\" type=\"vector3\" value=\"0,1,0\"/></bump>"
      " <switch name=\"pick\" type=\"color3\"><input name=\"which\" type=\"integer\" value=\"1\"/><input name=\"in1\" type=\"color3\" value=\"1,0,0\"/><input name=\"in2\" type=\"color3\" nodename=\"quad\"/><input name=\"in3\" type=\"color3\" value=\"0,0,1\"/></switch>"
      " <distance name=\"distance\" type=\"float\"><input name=\"in1\" type=\"vector3\" value=\"1,2,3\"/><input name=\"in2\" type=\"vector3\" value=\"1,5,7\"/></distance>"
      " <reflect name=\"reflect\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,-1,0\"/><input name=\"normal\" type=\"vector3\" value=\"0,1,0\"/></reflect>"
      " <premult name=\"premult\" type=\"color4\"><input name=\"in\" type=\"color4\" value=\"0.8,0.4,0.2,0.5\"/></premult>"
      " <unpremult name=\"unpremult\" type=\"color4\"><input name=\"in\" type=\"color4\" nodename=\"premult\"/></unpremult>"
      " <mincomponent name=\"mincomp\" type=\"float\"><input name=\"in\" type=\"color3\" value=\"0.8,0.2,0.5\"/></mincomponent>"
      " <xor name=\"xor\" type=\"boolean\"><input name=\"in1\" type=\"boolean\" value=\"true\"/><input name=\"in2\" type=\"boolean\" value=\"false\"/></xor>"
      " <inside name=\"inside\" type=\"color3\"><input name=\"in\" type=\"color3\" value=\"0.2,0.4,0.8\"/><input name=\"mask\" type=\"float\" value=\"0.25\"/></inside>"
      " <trianglewave name=\"tri_wave\" type=\"float\"><input name=\"in\" type=\"float\" value=\"1.25\"/></trianglewave>"
      " <checkerboard name=\"checker\" type=\"color3\"><input name=\"color1\" type=\"color3\" value=\"1,0,0\"/><input name=\"color2\" type=\"color3\" value=\"0,0,1\"/><input name=\"uvtiling\" type=\"vector2\" value=\"2,2\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.6,0.1\"/></checkerboard>"
      " <circle name=\"circle\" type=\"float\"><input name=\"center\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"radius\" type=\"float\" value=\"0.25\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.6,0.6\"/></circle>"
      " <line name=\"line\" type=\"float\"><input name=\"point1\" type=\"vector2\" value=\"0.25,0.25\"/><input name=\"point2\" type=\"vector2\" value=\"0.75,0.75\"/><input name=\"radius\" type=\"float\" value=\"0.1\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.55,0.5\"/></line>"
      " <difference name=\"difference\" type=\"color3\"><input name=\"fg\" type=\"color3\" value=\"0.8,0.1,0.4\"/><input name=\"bg\" type=\"color3\" value=\"0.2,0.5,0.1\"/><input name=\"mix\" type=\"float\" value=\"0.5\"/></difference>"
      " <over name=\"over\" type=\"color4\"><input name=\"fg\" type=\"color4\" value=\"0.4,0.1,0.2,0.5\"/><input name=\"bg\" type=\"color4\" value=\"0.2,0.4,0.6,0.25\"/></over>"
      " <matte name=\"matte\" type=\"color4\"><input name=\"fg\" type=\"color4\" value=\"0.4,0.1,0.2,0.5\"/><input name=\"bg\" type=\"color4\" value=\"0.2,0.4,0.6,0.25\"/></matte>"
      " <disjointover name=\"disjoint\" type=\"color4\"><input name=\"fg\" type=\"color4\" value=\"0.4,0.1,0.2,0.8\"/><input name=\"bg\" type=\"color4\" value=\"0.2,0.4,0.6,0.6\"/></disjointover>"
      " <colorcorrect name=\"colorcorrect\" type=\"color4\"><input name=\"in\" type=\"color4\" value=\"0.5,0.25,0.75,0.3\"/><input name=\"gamma\" type=\"float\" value=\"2\"/><input name=\"lift\" type=\"float\" value=\"0.1\"/><input name=\"gain\" type=\"float\" value=\"0.8\"/><input name=\"exposure\" type=\"float\" value=\"1\"/></colorcorrect>"
      " <cellnoise2d name=\"cell2a\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"1.2,2.8\"/></cellnoise2d>"
      " <cellnoise2d name=\"cell2b\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"1.9,2.1\"/></cellnoise2d>"
      " <cellnoise2d name=\"cell2next\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.9,2.1\"/></cellnoise2d>"
      " <cellnoise3d name=\"cell3\" type=\"float\"><input name=\"position\" type=\"vector3\" value=\"1.2,2.8,3.4\"/></cellnoise3d>"
      " <randomfloat name=\"random\" type=\"float\"><input name=\"in\" type=\"float\" value=\"0.25\"/><input name=\"min\" type=\"float\" value=\"2\"/><input name=\"max\" type=\"float\" value=\"4\"/><input name=\"seed\" type=\"integer\" value=\"7\"/></randomfloat>"
      " <randomcolor name=\"random_color\" type=\"color3\"><input name=\"in\" type=\"float\" value=\"0.25\"/><input name=\"seed\" type=\"integer\" value=\"7\"/><input name=\"huelow\" type=\"float\" value=\"0.1\"/><input name=\"huehigh\" type=\"float\" value=\"0.2\"/><input name=\"saturationlow\" type=\"float\" value=\"0.5\"/><input name=\"saturationhigh\" type=\"float\" value=\"0.6\"/><input name=\"brightnesslow\" type=\"float\" value=\"0.7\"/><input name=\"brightnesshigh\" type=\"float\" value=\"0.8\"/></randomcolor>"
      " <fractal2d name=\"fractal_scalar\" type=\"float\"><input name=\"amplitude\" type=\"float\" value=\"1.5\"/><input name=\"octaves\" type=\"integer\" value=\"3\"/><input name=\"lacunarity\" type=\"float\" value=\"2\"/><input name=\"diminish\" type=\"float\" value=\"0.5\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/></fractal2d>"
      " <fractal2d name=\"fractal_color\" type=\"color3\"><input name=\"amplitude\" type=\"color3\" value=\"1,2,3\"/><input name=\"octaves\" type=\"integer\" value=\"3\"/><input name=\"lacunarity\" type=\"float\" value=\"2\"/><input name=\"diminish\" type=\"float\" value=\"0.5\"/><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/></fractal2d>"
      " <worleynoise2d name=\"worley2_distance\" type=\"vector3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"jitter\" type=\"float\" value=\"1\"/><input name=\"style\" type=\"integer\" value=\"0\"/></worleynoise2d>"
      " <worleynoise2d name=\"worley2_solid\" type=\"vector3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"jitter\" type=\"float\" value=\"1\"/><input name=\"style\" type=\"integer\" value=\"1\"/></worleynoise2d>"
      " <worleynoise3d name=\"worley3_distance\" type=\"vector2\"><input name=\"position\" type=\"vector3\" value=\"0.2,0.4,0.7\"/><input name=\"jitter\" type=\"float\" value=\"0.75\"/><input name=\"style\" type=\"integer\" value=\"0\"/></worleynoise3d>"
      " <noise2d name=\"noise2_color\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"amplitude\" type=\"color3\" value=\"1,2,3\"/><input name=\"pivot\" type=\"color3\" value=\"0.1,0.2,0.3\"/></noise2d>"
      " <noise3d name=\"noise3_color\" type=\"color3\"><input name=\"position\" type=\"vector3\" value=\"0.2,0.4,0.7\"/><input name=\"amplitude\" type=\"color3\" value=\"1,2,3\"/><input name=\"pivot\" type=\"color3\" value=\"0.1,0.2,0.3\"/></noise3d>"
      " <unifiednoise2d name=\"unified_perlin\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"freq\" type=\"vector2\" value=\"2,3\"/><input name=\"offset\" type=\"vector2\" value=\"0.1,0.2\"/><input name=\"jitter\" type=\"float\" value=\"0.8\"/><input name=\"outmin\" type=\"float\" value=\"-1\"/><input name=\"outmax\" type=\"float\" value=\"2\"/><input name=\"type\" type=\"integer\" value=\"0\"/></unifiednoise2d>"
      " <unifiednoise2d name=\"unified_cell\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"jitter\" type=\"float\" value=\"0.8\"/><input name=\"type\" type=\"integer\" value=\"1\"/></unifiednoise2d>"
      " <unifiednoise2d name=\"unified_worley\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"jitter\" type=\"float\" value=\"0.8\"/><input name=\"type\" type=\"integer\" value=\"2\"/></unifiednoise2d>"
      " <unifiednoise2d name=\"unified_fractal\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.4\"/><input name=\"jitter\" type=\"float\" value=\"0.8\"/><input name=\"type\" type=\"integer\" value=\"3\"/></unifiednoise2d>"
      " <unifiednoise3d name=\"unified3_fractal\" type=\"float\"><input name=\"position\" type=\"vector3\" value=\"0.2,0.4,0.7\"/><input name=\"jitter\" type=\"float\" value=\"0.8\"/><input name=\"type\" type=\"integer\" value=\"3\"/></unifiednoise3d>"
      " <cloverleaf name=\"clover_inside\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.65\"/><input name=\"center\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"radius\" type=\"float\" value=\"0.4\"/></cloverleaf>"
      " <cloverleaf name=\"clover_outside\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.95,0.95\"/><input name=\"center\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"radius\" type=\"float\" value=\"0.4\"/></cloverleaf>"
      " <hexagon name=\"hex_inside\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"center\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"radius\" type=\"float\" value=\"0.4\"/></hexagon>"
      " <hexagon name=\"hex_outside\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.95,0.95\"/><input name=\"center\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"radius\" type=\"float\" value=\"0.4\"/></hexagon>"
      " <grid name=\"grid_line\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.01,0.5\"/><input name=\"thickness\" type=\"float\" value=\"0.1\"/></grid>"
      " <grid name=\"grid_fill\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"thickness\" type=\"float\" value=\"0.1\"/></grid>"
      " <crosshatch name=\"cross_center\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"thickness\" type=\"float\" value=\"0.1\"/></crosshatch>"
      " <tiledcircles name=\"tile_circle\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"size\" type=\"float\" value=\"0.4\"/></tiledcircles>"
      " <tiledcloverleafs name=\"tile_clover\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"size\" type=\"float\" value=\"0.4\"/></tiledcloverleafs>"
      " <tiledhexagons name=\"tile_hex\" type=\"color3\"><input name=\"texcoord\" type=\"vector2\" value=\"0.5,0.5\"/><input name=\"size\" type=\"float\" value=\"0.4\"/></tiledhexagons>"
      " <position name=\"geom_position_object\" type=\"vector3\"/>"
      " <position name=\"geom_position_world\" type=\"vector3\"><input name=\"space\" type=\"string\" value=\"world\"/></position>"
      " <normal name=\"geom_normal_object\" type=\"vector3\"/>"
      " <normal name=\"geom_normal_world\" type=\"vector3\"><input name=\"space\" type=\"string\" value=\"world\"/></normal>"
      " <tangent name=\"geom_tangent_object\" type=\"vector3\"/>"
      " <bitangent name=\"geom_bitangent_object\" type=\"vector3\"/>"
      " <noise3d name=\"geom_noise_default\" type=\"float\"/>"
      " <noise3d name=\"geom_noise_object\" type=\"float\"><input name=\"position\" type=\"vector3\" value=\"1.2,2.3,3.4\"/></noise3d>"
      " <noise3d name=\"geom_noise_world\" type=\"float\"><input name=\"position\" type=\"vector3\" value=\"7.4,12.9,20.6\"/></noise3d>"
      " <noise2d name=\"geom_uv_default\" type=\"float\"/>"
      " <noise2d name=\"geom_uv_explicit\" type=\"float\"><input name=\"texcoord\" type=\"vector2\" value=\"0.8,0.9\"/></noise2d>"
      " <viewdirection name=\"view_direction\" type=\"vector3\"/>"
      " <time name=\"scene_time\" type=\"float\"/>"
      " <frame name=\"scene_frame\" type=\"float\"/>"
      " <transformpoint name=\"object_point\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"fromspace\" type=\"string\" value=\"object\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformpoint>"
      " <transformvector name=\"object_vector\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"fromspace\" type=\"string\" value=\"object\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformvector>"
      " <transformnormal name=\"object_normal\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,1,1\"/><input name=\"fromspace\" type=\"string\" value=\"object\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformnormal>"
      " <transformpoint name=\"view_point\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"fromspace\" type=\"string\" value=\"view\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformpoint>"
      " <transformvector name=\"view_vector\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"1,2,3\"/><input name=\"fromspace\" type=\"string\" value=\"view\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformvector>"
      " <transformnormal name=\"view_normal\" type=\"vector3\"><input name=\"in\" type=\"vector3\" value=\"0,0,1\"/><input name=\"fromspace\" type=\"string\" value=\"view\"/><input name=\"tospace\" type=\"string\" value=\"world\"/></transformnormal>"
      " <geompropvalue name=\"temperature\" type=\"float\"><input name=\"geomprop\" type=\"string\" value=\"temperature\"/><input name=\"default\" type=\"float\" value=\"2700\"/></geompropvalue>"
      " <geompropvalue name=\"missing_prop\" type=\"float\"><input name=\"geomprop\" type=\"string\" value=\"missing\"/><input name=\"default\" type=\"float\" value=\"12\"/></geompropvalue>"
      " <blackbody name=\"blackbody_warm\" type=\"color3\"><input name=\"temperature\" type=\"float\" value=\"800\"/></blackbody>"
      " <roughness_anisotropy name=\"rough_aniso\" type=\"vector2\"><input name=\"roughness\" type=\"float\" value=\"0.5\"/><input name=\"anisotropy\" type=\"float\" value=\"0.75\"/></roughness_anisotropy>"
      " <roughness_dual name=\"rough_dual\" type=\"vector2\"><input name=\"roughness\" type=\"vector2\" value=\"0.5,-1\"/></roughness_dual>"
      " <glossiness_anisotropy name=\"gloss_aniso\" type=\"vector2\"><input name=\"glossiness\" type=\"float\" value=\"0.5\"/><input name=\"anisotropy\" type=\"float\" value=\"0.75\"/></glossiness_anisotropy>"
      " <artistic_ior name=\"artistic\" type=\"multioutput\"><input name=\"reflectivity\" type=\"color3\" value=\"0.25,0.25,0.25\"/><input name=\"edge_color\" type=\"color3\" value=\"1,1,1\"/></artistic_ior>"
      " <multiply name=\"artistic_extinction\" type=\"color3\"><input name=\"in1\" type=\"color3\" nodename=\"artistic\" output=\"extinction\"/><input name=\"in2\" type=\"float\" value=\"1\"/></multiply>"
      " <deon_hair_absorption_from_melanin name=\"deon_hair\" type=\"vector3\"><input name=\"melanin_concentration\" type=\"float\" value=\"1\"/><input name=\"melanin_redness\" type=\"float\" value=\"0\"/><input name=\"eumelanin_color\" type=\"color3\" value=\"0.367879441,0.367879441,0.367879441\"/></deon_hair_absorption_from_melanin>"
      " <chiang_hair_absorption_from_color name=\"chiang_absorption\" type=\"vector3\"><input name=\"color\" type=\"color3\" value=\"0.5,0.5,0.5\"/><input name=\"azimuthal_roughness\" type=\"float\" value=\"0.2\"/></chiang_hair_absorption_from_color>"
      " <chiang_hair_roughness name=\"chiang_roughness\" type=\"multioutput\"><input name=\"longitudinal\" type=\"float\" value=\"0.1\"/><input name=\"azimuthal\" type=\"float\" value=\"0.2\"/><input name=\"scale_TT\" type=\"float\" value=\"0.5\"/><input name=\"scale_TRT\" type=\"float\" value=\"2\"/></chiang_hair_roughness>"
      " <multiply name=\"chiang_tt\" type=\"vector2\"><input name=\"in1\" type=\"vector2\" nodename=\"chiang_roughness\" output=\"roughness_TT\"/><input name=\"in2\" value=\"1\"/></multiply>"
      " <multiply name=\"chiang_trt\" type=\"vector2\"><input name=\"in1\" type=\"vector2\" nodename=\"chiang_roughness\" output=\"roughness_TRT\"/><input name=\"in2\" value=\"1\"/></multiply>"
      " <ifgreater name=\"choose\" type=\"color3\"><input name=\"value1\" value=\"2\"/><input name=\"value2\" value=\"1\"/><input name=\"in1\" nodename=\"ramp\"/><input name=\"in2\" nodename=\"split\"/></ifgreater>"
      " <ifgreatereq name=\"choose_eq\" type=\"color3\"><input name=\"value1\" value=\"1\"/><input name=\"value2\" value=\"1\"/><input name=\"in1\" type=\"color3\" value=\"0.1,0.2,0.3\"/><input name=\"in2\" type=\"color3\" value=\"0.8,0.7,0.6\"/></ifgreatereq>"
      " <ifequal name=\"choose_ne\" type=\"color3\"><input name=\"value1\" value=\"1\"/><input name=\"value2\" value=\"2\"/><input name=\"in1\" type=\"color3\" value=\"1,0,0\"/><input name=\"in2\" type=\"color3\" value=\"0,0,1\"/></ifequal>"
      "</nodegraph></materialx>";
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) return 1;
  ShadeContext ctx = {0};
  ctx.doc = doc;
  ctx.uv[0] = 0.25f; ctx.uv[1] = 0.75f;
  ctx.Ns = ctx.Ng = v3_make(0, 0, 1);
  ctx.dpdu = v3_make(1, 0, 0); ctx.dpdv = v3_make(0, 1, 0);
  ctx.V = v3_make(0, 0, 2); ctx.time = 1.25f; ctx.frame = 42.0f;
  { const float o2w[16]={2,0,0,0, 0,3,0,0, 0,0,4,0, 5,6,7,1};
    const float w2o[16]={.5f,0,0,0, 0,1.0f/3.0f,0,0,
                         0,0,.25f,0, -2.5f,-2,-1.75f,1};
    memcpy(ctx.object_to_world,o2w,sizeof(o2w));
    memcpy(ctx.world_to_object,w2o,sizeof(w2o));
    const float w2v[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1};
    const float v2w[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, -10,-20,-30,1};
    memcpy(ctx.world_to_view,w2v,sizeof(w2v));
    memcpy(ctx.view_to_world,v2w,sizeof(v2w));
    ctx.has_space_transforms=1;
  }
  ctx.geomprop=lookup_geomprop;
  ctx.memo = (MtlxValue *)calloc((size_t)doc->nnode, sizeof(MtlxValue));
  ctx.memo_done = (char *)calloc((size_t)doc->nnode, 1);
  if (!ctx.memo || !ctx.memo_done) {
    free(ctx.memo); free(ctx.memo_done); mtlx_free(doc); return 1;
  }
  int ok = check3("chain", eval_named(&ctx, "clamped"), 0.6f, 1.0f, 1.0f) &&
           check3("ramp-left", eval_named(&ctx, "ramp"), 0.75f, 0.0f, 0.25f) &&
           check3("split-left", eval_named(&ctx, "split"), 0.0f, 1.0f, 0.0f) &&
           check3("ramp-tb", eval_named(&ctx, "ramp_tb"), 0.75f, 1.0f, 0.25f) &&
           check3("split-tb-bottom", eval_named(&ctx, "split_tb"), 0.0f, 0.0f, 1.0f) &&
           check3("screen", eval_named(&ctx, "screen"), 0.19f, 0.55f, 0.82f) &&
           check3("overlay", eval_named(&ctx, "overlay"), 0.07f, 0.4f, 0.79f) &&
           nearf_eps(eval_named(&ctx, "burn").v[0], -0.125f) &&
           nearf_eps(eval_named(&ctx, "dodge").v[0], 0.375f) &&
           check3("saturate", eval_named(&ctx, "desaturate"), 0.38636f,
                  0.38636f, 0.38636f) &&
           check3("conditional", eval_named(&ctx, "choose"), 0.75f, 0.0f, 0.25f) &&
           check3("conditional-gte-boundary", eval_named(&ctx, "choose_eq"),
                  0.1f, 0.2f, 0.3f) &&
           check3("conditional-equal-false", eval_named(&ctx, "choose_ne"),
                  0.0f, 0.0f, 1.0f) &&
           nearf_eps(eval_named(&ctx, "dot").v[0], 32.0f) &&
           check3("normalize", eval_named(&ctx, "norm"), 0.0f, 0.6f, 0.8f);
  ok = check3("fraction-alias", eval_named(&ctx, "fraction"), 0.75f, 0.5f,
              0.75f) &&
       check3("step-boundary", eval_named(&ctx, "step"), 0.0f, 1.0f, 1.0f) && ok;
  ok = check3("hsv-roundtrip", eval_named(&ctx, "rgb"), 0.2f, 0.4f, 0.8f) &&
       nearf_eps(eval_named(&ctx, "rotate").v[0], 0.0f) &&
       nearf_eps(eval_named(&ctx, "rotate").v[1], 1.0f) && ok;
  ok = check3("ramp4", eval_named(&ctx, "quad"), 0.625f, 0.25f, 0.25f) &&
       check3("switch-connected", eval_named(&ctx, "pick"), 0.625f, 0.25f,
              0.25f) && ok;
  ok = check4("ramp", eval_named(&ctx, "full_ramp"), 0.5f, 0.0f, 0.0f,
              1.0f) &&
       check4("ramp-gradient", eval_named(&ctx, "gradient"), 0.15625f,
              0.078125f, 0.0f, 1.0f) && ok;
  ok = check3("blur-pass-through", eval_named(&ctx, "blur"), 0.2f, 0.4f,
              0.8f) && ok;
  {
    MtlxValue fn=eval_named(&ctx,"flakes"),fp=eval_named(&ctx,"flake_presence"),
              fr=eval_named(&ctx,"flake_rand");
    float length=sqrtf(fn.v[0]*fn.v[0]+fn.v[1]*fn.v[1]+fn.v[2]*fn.v[2]);
    if(!nearf_eps(length,1.0f)||fp.v[0]<0||fp.v[0]>1||fr.v[0]<0||fr.v[0]>1){
      fprintf(stderr,"flake multi-output evaluation invalid\n");ok=0;
    }
  }
  ok = check1("matrix-determinant",eval_named(&ctx,"matrix_det"),24.0f) &&
       check3("matrix-transform",eval_named(&ctx,"matrix_transform"),2,6,12) &&
       check3("matrix-inverse-roundtrip",eval_named(&ctx,"matrix_roundtrip"),1,2,3) &&
       check1("matrix-transpose-determinant",eval_named(&ctx,"transpose_det"),24.0f) &&
       check3("matrix44-transform",eval_named(&ctx,"matrix4_transform"),6,8,10) &&
       check1("matrix44-determinant",eval_named(&ctx,"matrix4_det"),1.0f) &&
       check3("latlong-missing-default",eval_named(&ctx,"latlong_missing"),
              0.125f,0.25f,0.5f) &&
       check3("triplanar-missing-default",eval_named(&ctx,"triplanar_missing"),
              0.2f,0.4f,0.8f) && ok;
  { MtlxValue hn=eval_named(&ctx,"height_normal"),bn=eval_named(&ctx,"bump_normal");
    if(!(hn.v[0]<0.5f&&nearf_eps(hn.v[1],0.5f)&&hn.v[2]>0.99f&&
         bn.v[0]<0.0f&&nearf_eps(bn.v[1],0.0f)&&bn.v[2]>0.99f)){
      fprintf(stderr,"heighttonormal/bump spatial derivative failed\n");ok=0;
    }
  }
  ok = nearf_eps(eval_named(&ctx, "distance").v[0], 5.0f) &&
       check3("reflect", eval_named(&ctx, "reflect"), 1.0f, 1.0f, 0.0f) &&
       check3("unpremult", eval_named(&ctx, "unpremult"), 0.8f, 0.4f, 0.2f) &&
       nearf_eps(eval_named(&ctx, "mincomp").v[0], 0.2f) &&
       nearf_eps(eval_named(&ctx, "xor").v[0], 1.0f) &&
       check3("inside", eval_named(&ctx, "inside"), 0.05f, 0.1f, 0.2f) && ok;
  ok = nearf_eps(eval_named(&ctx, "tri_wave").v[0], 0.25f) && ok;
  /* floor((0.6,0.1) * (2,2)) sums to odd parity: the MaterialX
   * checkerboard graph selects color1 (red) for this cell. */
  ok = check3("checkerboard", eval_named(&ctx, "checker"), 1.0f, 0.0f, 0.0f) && ok;
  ok = nearf_eps(eval_named(&ctx, "circle").v[0], 1.0f) && ok;
  ok = nearf_eps(eval_named(&ctx, "line").v[0], 1.0f) && ok;
  ok = check3("view-direction",eval_named(&ctx,"view_direction"),0,0,1) &&
       check1("time",eval_named(&ctx,"scene_time"),1.25f) &&
       check1("frame",eval_named(&ctx,"scene_frame"),42.0f) &&
       check3("transform-point",eval_named(&ctx,"object_point"),7,12,19) &&
       check3("transform-vector",eval_named(&ctx,"object_vector"),2,6,12) &&
       check3("transform-normal",eval_named(&ctx,"object_normal"),
              0.7682213f,0.5121475f,0.3841106f) &&
       check3("view-point",eval_named(&ctx,"view_point"),-9,-18,-27) &&
       check3("view-vector",eval_named(&ctx,"view_vector"),1,2,3) &&
       check3("view-normal",eval_named(&ctx,"view_normal"),0,0,1) &&
       check1("geomprop-callback",eval_named(&ctx,"temperature"),6500.0f) &&
       check1("geomprop-default",eval_named(&ctx,"missing_prop"),12.0f) &&
       check3("roughness-anisotropy",eval_named(&ctx,"rough_aniso"),.5f,.125f,0) &&
       check3("roughness-dual",eval_named(&ctx,"rough_dual"),.25f,.25f,0) &&
       check3("glossiness-anisotropy",eval_named(&ctx,"gloss_aniso"),.5f,.125f,0) &&
       check3("artistic-ior",eval_named(&ctx,"artistic"),.6f,.6f,.6f) &&
       check3("artistic-extinction",eval_named(&ctx,"artistic_extinction"),.8f,.8f,.8f) &&
       check3("deon-hair-absorption",eval_named(&ctx,"deon_hair"),1,1,1) &&
       nearf_eps(eval_named(&ctx,"chiang_roughness").v[0],0.0065157184f) &&
       nearf_eps(eval_named(&ctx,"chiang_roughness").v[1],0.1007600f) &&
       nearf_eps(eval_named(&ctx,"chiang_tt").v[0],0.0016289296f) &&
       nearf_eps(eval_named(&ctx,"chiang_tt").v[1],0.1007600f) && ok;
  if (!nearf_eps(eval_named(&ctx,"chiang_trt").v[0],0.0260628736f) ||
      !nearf_eps(eval_named(&ctx,"chiang_trt").v[1],0.1007600f)) ok = 0;
  if (!nearf_eps(eval_named(&ctx,"chiang_roughness").v[0],0.0065157184f) ||
      !nearf_eps(eval_named(&ctx,"chiang_roughness").v[1],0.1007600f) ||
      !nearf_eps(eval_named(&ctx,"chiang_tt").v[0],0.0016289296f) ||
      !nearf_eps(eval_named(&ctx,"chiang_tt").v[1],0.1007600f))
    fprintf(stderr, "chiang values: R=(%.9f,%.9f) TT=(%.9f,%.9f)\n",
            eval_named(&ctx,"chiang_roughness").v[0],
            eval_named(&ctx,"chiang_roughness").v[1],
            eval_named(&ctx,"chiang_tt").v[0],
            eval_named(&ctx,"chiang_tt").v[1]);
  { MtlxValue absorption=eval_named(&ctx,"chiang_absorption");
    if(!(absorption.v[0]>0&&nearf_eps(absorption.v[0],absorption.v[1])&&nearf_eps(absorption.v[1],absorption.v[2]))){fprintf(stderr,"Chiang hair absorption invalid\n");ok=0;}
  }
  { MtlxValue warm=eval_named(&ctx,"blackbody_warm");
    if(!(warm.v[0]>warm.v[1]&&warm.v[1]>=warm.v[2]&&warm.v[2]>=0)){
      fprintf(stderr,"blackbody warm spectrum invalid: %g %g %g\n",warm.v[0],warm.v[1],warm.v[2]);ok=0;
    }
  }
  ok = check3("difference", eval_named(&ctx, "difference"), 0.4f, 0.45f,
              0.2f) && ok;
  ok = check4("over", eval_named(&ctx, "over"), 0.5f, 0.3f, 0.5f,
              0.625f) && ok;
  ok = check4("matte", eval_named(&ctx, "matte"), 0.3f, 0.25f, 0.4f,
              0.625f) && ok;
  ok = check4("disjointover", eval_named(&ctx, "disjoint"),
              0.46666667f, 0.23333333f, 0.4f, 1.0f) && ok;
  ok = check4("colorcorrect", eval_named(&ctx, "colorcorrect"), 1.178234f,
              0.88f, 1.407077f, 0.3f) && ok;
  ok = check1("cellnoise2d-a", eval_named(&ctx, "cell2a"), 0.06335137f) &&
       check1("cellnoise2d-same-cell", eval_named(&ctx, "cell2b"),
              0.06335137f) &&
       check1("cellnoise2d-next-cell", eval_named(&ctx, "cell2next"),
              0.97482122f) &&
       check1("cellnoise3d", eval_named(&ctx, "cell3"), 0.72194636f) && ok;
  ok = check1("randomfloat", eval_named(&ctx, "random"), 2.31866973f) && ok;
  ok = check3("randomcolor", eval_named(&ctx, "random_color"), 0.72096912f,
              0.68491890f, 0.32840088f) && ok;
  ok = check1("fractal2d-scalar", eval_named(&ctx, "fractal_scalar"),
              0.27252933f) &&
       check3("fractal2d-color", eval_named(&ctx, "fractal_color"),
              0.18168622f, 1.98365822f, -0.40455556f) && ok;
  ok = check3("worley2d-distance", eval_named(&ctx, "worley2_distance"),
              0.334542f, 0.463517f, 0.984037f) && ok;
  ok = check3("worley2d-solid", eval_named(&ctx, "worley2_solid"),
              0.960777f, 0.166139f, 0.444949f) && ok;
  { MtlxValue w = eval_named(&ctx, "worley3_distance");
    ok = check1("worley3d-distance-f1", w, 0.321671426f) && ok;
    if (!nearf_eps(w.v[1], 0.943064690f))
      fprintf(stderr, "worley3d-distance-f2: got %.9f, expected %.9f\n",
              w.v[1], 0.943064690f);
    ok = nearf_eps(w.v[1], 0.943064690f) && ok;
  }
  ok = check3("noise2d-color", eval_named(&ctx, "noise2_color"),
              0.377306f, 1.246215f, 0.188793f) && ok;
  ok = check3("noise3d-color", eval_named(&ctx, "noise3_color"),
              0.338275f, -0.837646f, 0.669710f) && ok;
  ok = check1("unified-perlin", eval_named(&ctx, "unified_perlin"),
              -0.390847325f) && ok;
  ok = check1("unified-cell", eval_named(&ctx, "unified_cell"),
              0.860312760f) && ok;
  ok = check1("unified-worley", eval_named(&ctx, "unified_worley"),
              0.371120334f) && ok;
  ok = check1("unified-fractal", eval_named(&ctx, "unified_fractal"),
              0.475442290f) && ok;
  ok = check1("unified3-fractal", eval_named(&ctx, "unified3_fractal"),
              0.079035468f) && ok;
  ok = check1("cloverleaf-inside", eval_named(&ctx, "clover_inside"), 1.0f) && ok;
  ok = check1("cloverleaf-outside", eval_named(&ctx, "clover_outside"), 0.0f) && ok;
  ok = check1("hexagon-inside", eval_named(&ctx, "hex_inside"), 1.0f) && ok;
  ok = check1("hexagon-outside", eval_named(&ctx, "hex_outside"), 0.0f) && ok;
  ok = check3("grid-line",eval_named(&ctx,"grid_line"),1,1,1)&&ok;
  ok = check3("grid-fill",eval_named(&ctx,"grid_fill"),0,0,0)&&ok;
  ok = check3("crosshatch-center",eval_named(&ctx,"cross_center"),1,1,1)&&ok;
  ok = check3("tiled-circle",eval_named(&ctx,"tile_circle"),1,1,1)&&ok;
  ok = check3("tiled-clover",eval_named(&ctx,"tile_clover"),1,1,1)&&ok;
  ok = check3("tiled-hex",eval_named(&ctx,"tile_hex"),1,1,1)&&ok;
  /* mtlx_eval_node_test must clear memoization between shade points. */
  ctx.uv[0] = 0.8f;
  ctx.uv[1] = 0.9f;
  ok = check3("ramp-right", eval_named(&ctx, "ramp"), 0.2f, 0.0f, 0.8f) &&
       check3("split-right", eval_named(&ctx, "split"), 1.0f, 0.0f, 1.0f) && ok;
  ok = check3("split-tb-top", eval_named(&ctx, "split_tb"), 1.0f, 0.0f,
              0.0f) && ok;
  /* World-to-object point, normal and tangent transforms with nonuniform scale. */
  ctx.P = v3_make(7.0f, 12.0f, 19.0f);
  ctx.Ns = v3_make(0.7682213f, 0.5121475f, 0.3841106f);
  ctx.dpdu = v3_make(2.0f, 3.0f, 0.0f);
  ctx.dpdv = v3_make(0.0f, 3.0f, 4.0f);
  ok = check3("geometry-object-position", eval_named(&ctx, "geom_position_object"), 1, 2, 3) &&
       check3("geometry-world-position", eval_named(&ctx, "geom_position_world"), 7, 12, 19) &&
       check3("geometry-object-normal", eval_named(&ctx, "geom_normal_object"),
              0.5773503f, 0.5773503f, 0.5773503f) &&
       check3("geometry-world-normal", eval_named(&ctx, "geom_normal_world"),
              0.7682213f, 0.5121475f, 0.3841106f) &&
       check3("geometry-object-tangent", eval_named(&ctx, "geom_tangent_object"),
              0.7071068f, 0.7071068f, 0.0f) &&
       check3("geometry-object-bitangent", eval_named(&ctx, "geom_bitangent_object"),
              0.0f, 0.7071068f, 0.7071068f) && ok;
  /* The stdlib defaults are UV0 and Pobject, not world-space P. */
  ctx.P = v3_make(7.4f, 12.9f, 20.6f);
  const float noise_default = eval_named(&ctx, "geom_noise_default").v[0];
  const float noise_object = eval_named(&ctx, "geom_noise_object").v[0];
  const float noise_world = eval_named(&ctx, "geom_noise_world").v[0];
  const float uv_default = eval_named(&ctx, "geom_uv_default").v[0];
  const float uv_explicit = eval_named(&ctx, "geom_uv_explicit").v[0];
  if (!nearf_eps(noise_default, noise_object) || nearf_eps(noise_default, noise_world) ||
      !nearf_eps(uv_default, uv_explicit)) {
    fprintf(stderr, "implicit geometry defaults did not use Pobject and UV0\n");
    ok = 0;
  }
  free(ctx.memo); free(ctx.memo_done); mtlx_free(doc);

  /* An authored geometry definition overrides Pobject; two users share its
   * generated node, while an authored input remains a literal. */
  {
    const char *override_xml =
        "<materialx version=\"1.39\">"
        "<geompropdef name=\"Pobject\" type=\"vector3\" geomprop=\"position\" space=\"world\"/>"
        "<nodegraph name=\"G\"><noise3d name=\"a\" type=\"float\"/>"
        "<noise3d name=\"b\" type=\"float\"/>"
        "<noise3d name=\"authored\" type=\"float\"><input name=\"position\" type=\"vector3\" value=\"7.4,12.9,20.6\"/></noise3d>"
        "</nodegraph></materialx>";
    MtlxDoc *override_doc = mtlx_load_string(override_xml);
    ShadeContext override_ctx = ctx;
    int a = override_doc ? mtlx_find_node(override_doc, 0, "a") : -1;
    int b = override_doc ? mtlx_find_node(override_doc, 0, "b") : -1;
    int authored = override_doc ? mtlx_find_node(override_doc, 0, "authored") : -1;
    int source_a = -1, source_b = -1, literal = 0;
    if (override_doc && a >= 0 && b >= 0 && authored >= 0) {
      for (int i = 0; i < override_doc->nodes[a].ninput; ++i)
        if (!strcmp(override_doc->nodes[a].inputs[i].name, "position"))
          source_a = override_doc->nodes[a].inputs[i].src_node;
      for (int i = 0; i < override_doc->nodes[b].ninput; ++i)
        if (!strcmp(override_doc->nodes[b].inputs[i].name, "position"))
          source_b = override_doc->nodes[b].inputs[i].src_node;
      for (int i = 0; i < override_doc->nodes[authored].ninput; ++i)
        if (!strcmp(override_doc->nodes[authored].inputs[i].name, "position"))
          literal = override_doc->nodes[authored].inputs[i].has_value;
      override_ctx.doc = override_doc;
      override_ctx.memo = (MtlxValue *)calloc((size_t)override_doc->nnode, sizeof(MtlxValue));
      override_ctx.memo_done = (char *)calloc((size_t)override_doc->nnode, 1);
      float override_value = 0.0f, authored_value = 1.0f;
      if (override_ctx.memo && override_ctx.memo_done) {
        override_value = mtlx_eval_node_test(&override_ctx, a).v[0];
        authored_value = mtlx_eval_node_test(&override_ctx, authored).v[0];
      }
      if (!override_ctx.memo || !override_ctx.memo_done || source_a < 0 || source_a != source_b ||
          !literal || override_doc->nnode != 4 ||
          !nearf_eps(override_value, authored_value)) {
        fprintf(stderr, "geometry-definition override or generated-node reuse failed\n");
        ok = 0;
      }
      free(override_ctx.memo); free(override_ctx.memo_done);
    } else {
      fprintf(stderr, "geometry-definition override fixture did not load\n");
      ok = 0;
    }
    mtlx_free(override_doc);
  }

  /* A nonzero UV set cannot be represented by the current single-UV context. */
  {
    const char *unsupported_xml =
        "<materialx version=\"1.39\">"
        "<geompropdef name=\"UV0\" type=\"vector2\" geomprop=\"texcoord\" index=\"1\"/>"
        "<surfacematerial name=\"m\"/><nodegraph name=\"g\">"
        "<noise3d name=\"first\" type=\"float\"/><noise2d name=\"n\" type=\"float\"/>"
        "<output name=\"out\" type=\"float\" nodename=\"n\"/>"
        "</nodegraph></materialx>";
    MtlxDoc *unsupported_doc = mtlx_load_string(unsupported_xml);
    if (unsupported_doc) {
      fprintf(stderr, "unsupported geometry default was accepted\n");
      ok = 0;
      mtlx_free(unsupported_doc);
    }
    const char *authored_xml =
        "<materialx version=\"1.39\">"
        "<geompropdef name=\"UV0\" type=\"vector2\" geomprop=\"texcoord\" index=\"1\"/>"
        "<nodegraph name=\"g\"><noise2d name=\"n\" type=\"float\">"
        "<input name=\"texcoord\" type=\"vector2\" value=\"0.2,0.3\"/>"
        "</noise2d></nodegraph></materialx>";
    MtlxDoc *authored_doc = mtlx_load_string(authored_xml);
    if (!authored_doc) {
      fprintf(stderr, "authored UV was rejected due to an unused geometry default\n");
      ok = 0;
    }
    mtlx_free(authored_doc);
  }

  /* Cyclic graph edges must terminate through the evaluator's zero sentinel
   * instead of recursing indefinitely. */
  {
    const char *cycle_xml =
        "<materialx version=\"1.39\"><nodegraph name=\"Cycle\">"
        "<add name=\"a\" type=\"float\"><input name=\"in1\" nodename=\"b\"/>"
        "<input name=\"in2\" value=\"1\"/></add>"
        "<add name=\"b\" type=\"float\"><input name=\"in1\" nodename=\"a\"/>"
        "<input name=\"in2\" value=\"2\"/></add></nodegraph></materialx>";
    MtlxDoc *cycle_doc = mtlx_load_string(cycle_xml);
    ShadeContext cycle_ctx = {0};
    int cycle_id = cycle_doc ? mtlx_find_node(cycle_doc, 0, "a") : -1;
    if (cycle_doc) {
      cycle_ctx.doc = cycle_doc;
      cycle_ctx.memo = (MtlxValue *)calloc((size_t)cycle_doc->nnode,
                                            sizeof(MtlxValue));
      cycle_ctx.memo_done = (char *)calloc((size_t)cycle_doc->nnode, 1);
    }
    if (!cycle_doc || cycle_id < 0 || !cycle_ctx.memo || !cycle_ctx.memo_done ||
        !nearf_eps(mtlx_eval_node_test(&cycle_ctx, cycle_id).v[0], 3.0f)) {
      fprintf(stderr, "cyclic MaterialX graph did not terminate safely\n");
      ok = 0;
    }
    if (cycle_doc) {
      free(cycle_ctx.memo);
      free(cycle_ctx.memo_done);
      mtlx_free(cycle_doc);
    }
  }
  if (!ok) fprintf(stderr, "MaterialX graph evaluation suite failed\n");
  return ok ? 0 : 1;
}
