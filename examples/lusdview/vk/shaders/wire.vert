#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPosition;
layout(location = 3) in uvec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in uvec2 aInfluence;
layout(location = 8) in uvec2 aMorphOffsetCount;

// Same blendshape morph + linear-blend skinning the shaded mesh pass uses. The
// authored cage indices address the render vertex buffer, so a skinned/morphed
// mesh's edges must deform with it instead of staying in the rest pose.
#include "deform.glsl"

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
  // pc.ids.w bit 0: the cage indices address the render vertex buffer, so the
  // deform attributes below are valid. A subdivided mesh keeps its dedicated
  // authored control-point buffer, which has no skin/morph attributes; leave it
  // in rest space rather than feeding it unrelated render-vertex data.
  vec3 pos = aPosition;
  if ((pc.ids.w & 1) != 0) {
    vec3 nrm;
    applyMorphSkin(aPosition, vec3(0.0, 1.0, 0.0), aMorphOffsetCount, aJoint,
                   aWeight, aInfluence, pos, nrm);
  }
  gl_Position = fr.viewProj * pc.model * vec4(pos, 1.0);
}
