#version 450

// Instanced authored-cage wireframe (--next GPU-instanced prototypes).
//
// The single-draw wire program (wire.vert) cannot serve instanced prototypes:
// its skin/morph attributes occupy locations 3/4/5, which the instanced vertex
// layout uses for the per-instance 3x4 object-to-world rows. This program reads
// the same rows as mesh_inst.vert and places the prototype's authored cage at
// every visible placement, so 'V' (wireframe) also shows instanced content.
//
// Prototypes are never subdivided, so their cage indices address the render
// vertex buffer and need no separate control-point array.

layout(location = 0) in vec3 aPosition;
layout(location = 3) in vec4 aRow0;  // o2w row 0 (instance-rate)
layout(location = 4) in vec4 aRow1;
layout(location = 5) in vec4 aRow2;

layout(set = 2, binding = 0) uniform Frame {
  vec4 disp;
  mat4 viewProj;
  vec4 camPos;
  vec4 sceneMin;
  vec4 sceneExtent;
  vec4 lightDir;
  vec4 lightColor;
  ivec4 mode;
  mat4 shadowViewProj;
  vec4 shadowParams;
  mat4 pointShadowViewProj[6];
  vec4 pointShadowLight;
} fr;

layout(push_constant) uniform PushConstants {
  mat4 model;
  vec4 baseColor;
  vec4 matAux;
  vec4 emissive;
  ivec4 ids;
} pc;

void main() {
  const vec4 p = vec4(aPosition, 1.0);
  // Row-vector convention, matching mesh_inst.vert: rows are the output axes.
  const vec3 world = vec3(dot(p, aRow0), dot(p, aRow1), dot(p, aRow2));
  gl_Position = fr.viewProj * pc.model * vec4(world, 1.0);
}
