#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uvec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in uvec2 aInfluence;
layout(location = 6) in vec2 aUV1;        // 2nd texcoord set (multi-UV AOV)
layout(location = 7) in float aMorphInfl; // blendshape influence (world units)
layout(location = 8) in uvec2 aMorphOffsetCount; // GPU morph (offset,count); 0 = none

// Blendshape morph + linear-blend skinning (sets 1,2,7,8,9 + fetchBone /
// morphChanId / applyMorphSkin); shared with the GPU-tessellation vertex stage.
#include "deform.glsl"

// Coarse displacement height map (red channel), sampled in the vertex stage. The
// renderer binds black (red=0) when the submesh has no displacement, so this is an
// unconditional no-op there -- no push-constant lane is spent on an enable flag.
layout(set = 0, binding = 16) uniform sampler2D uDisplacementTex;
layout(set = 0, binding = 5) uniform sampler2D uUdimLutAtlas;
layout(set = 0, binding = 31) uniform sampler2DArray uDisplacementUdimTex;
struct RasterLight { vec4 positionType; vec4 directionAngle;
                     vec4 colorDiffuse; vec4 specularShape; vec4 areaParams;
                     vec4 iesAxisX; vec4 iesAxisY; vec4 iesProfile[6]; };
// Frame-constant UBO (set 5): disp sliders + camera. The vertex stage derives
// mvp = viewProj*model and the normal matrix from pc.model, so neither needs a
// push-constant lane (keeps the push block <= 128 B, the Vulkan minimum).
layout(set = 2, binding = 0) uniform Frame {
  vec4 disp;          // .x = displacement scale, .y = maxTessLevel
  mat4 viewProj;      // P * V
  vec4 camPos;        // .xyz camera, .w depth normalizer
  vec4 sceneMin;      // .xyz
  vec4 sceneExtent;   // .xyz
  vec4 lightDir;
  vec4 lightColor;
  RasterLight rasterLights[16];
  uvec4 rasterLightInfo;
  ivec4 mode;         // .x = renderMode
  mat4 envRot;        // world -> environment rotation (dome IBL)
  vec4 iblColor;      // .rgb dome effectiveColor, .w = hasIbl (0/1)
  vec4 iblParams;     // .x = prefiltered mip count
  mat4 shadowViewProj;
  vec4 pointShadowLight;
  mat4 pointShadowViewProj[6];
  vec4 clippingPlanes[8];
  ivec4 clippingInfo;
} fr;
struct MaterialTexParam {
  vec4 baseUv0; vec4 baseUv1;
  vec4 mrUv0; vec4 mrUv1;
  vec4 normalUv0; vec4 normalUv1;
  vec4 emissiveUv0; vec4 emissiveUv1;
  vec4 dispUv0; vec4 dispUv1;
  vec4 baseScale; vec4 baseBias;
  vec4 normalScale; vec4 normalBias;
  vec4 emissiveScale; vec4 emissiveBias;
  vec4 scalar0;  // metallicChannel, roughnessChannel, metallicScale, metallicBias
  vec4 scalar1;  // roughnessScale, roughnessBias, displacementScale, displacementBias
  vec4 uvSets;   // per-slot UV set (see mesh.frag); unused here but the struct
                 // layout must stay byte-identical across every stage.
  vec4 specParams;  // specular F0 (see mesh.frag); unused here, kept for
                    // the byte-identical SSBO stride.
  vec4 opacityUv0; vec4 opacityUv1;
  vec4 opacityParams;
  vec4 udimSlots0;
  vec4 udimSlots1;
  vec4 roughUv0; vec4 roughUv1;
  vec4 coatParams;
  vec4 coatColor;
  vec4 occlusionUv0; vec4 occlusionUv1;
  vec4 occlusionParams;
  // Padding rows so the std430 array stride stays byte-identical with
  // mesh.frag, which uses these extra semantic texture slots.
  vec4 specColorUv0; vec4 specColorUv1;
  vec4 coatWeightUv0; vec4 coatWeightUv1;
  vec4 coatColorUv0; vec4 coatColorUv1;
  vec4 coatRoughUv0; vec4 coatRoughUv1;
  vec4 coatTexParams;
  vec4 extraUvSets;
  vec4 specColorScale; vec4 specColorBias;
  vec4 coatWeightScale; vec4 coatWeightBias;
  vec4 coatColorScale; vec4 coatColorBias;
  vec4 coatRoughScale; vec4 coatRoughBias;
  vec4 coatNormalUv0; vec4 coatNormalUv1;
  vec4 coatNormalScale; vec4 coatNormalBias;
  vec4 semanticUdimSlots;
  vec4 semanticUdimSlots2;
  vec4 ptexBaseInfo;
};
layout(set = 3, binding = 0, std430) readonly buffer MatTex { MaterialTexParam p[]; } mtp;
// Per-vertex displayColor + displayOpacity (set 24): 4 floats per vertex.
// gl_VertexIndex. Bound to a shared dummy for meshes without color; the fetch is
// gated by pc.ids.w bit 32 so the dummy's contents never matter.
layout(set = 1, binding = 5, std430) readonly buffer VtxColor { float c[]; } vtxcol;

// 128-byte per-draw push constant block (matches struct PushC in vk_renderer.cc).
layout(push_constant) uniform PushConstants {
  mat4 model;
  vec4 baseColor;   // rgb + .w opacity
  vec4 matAux;      // .x metallic, .y roughness, .z alphaMode, .w alphaCutoff
  vec4 emissive;    // .xyz emissive (AOV)
  ivec4 ids;        // .x matId, .y flags, .z meshId
} pc;

vec4 sampleDisplacement(vec2 uv, float row) {
  int tile = 1001 + int(floor(uv.x)) + 10 * int(floor(uv.y));
  int idx = tile - 1001;
  if (idx < 0 || idx >= 100) return vec4(0.0);
  int layer = int(texelFetch(uUdimLutAtlas, ivec2(idx, int(row + 0.5)), 0).r *
                  255.0 + 0.5) - 1;
  if (layer < 0) return vec4(0.0);
  return textureLod(uDisplacementUdimTex,
                    vec3(fract(uv), float(layer)), 0.0);
}

layout(location = 0) out vec3 vNormalW;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) flat out int vDomJoint;   // dominant skin joint (SkinWeights AOV)
layout(location = 4) out float vDomWeight;
layout(location = 5) out vec2 vUV1;            // 2nd texcoord set (multi-UV AOV)
layout(location = 6) out float vMorphInfl;     // blendshape influence (world units)
layout(location = 7) out vec4 vColor;          // displayColor.rgb + displayOpacity

void main() {
  // Blendshape morph (before skin) + linear-blend skin -> object-space pos/nrm.
  vec3 pos, nrm;
  applyMorphSkin(aPos, aNormal, aMorphOffsetCount, aJoint, aWeight, aInfluence,
                 pos, nrm);
  // Coarse displacement: offset along the (object-space) normal by the height map's
  // red channel (no derivatives in the vertex stage -> textureLod 0). Black map =>
  // no offset. Geometric normals (flags bit0, set by the renderer) keep shading
  // consistent with the deformed surface.
  int mid = max(pc.ids.x, 0);
  vec2 duv = vec2(dot(vec3(aUV, 1.0), mtp.p[mid].dispUv0.xyz),
                  dot(vec3(aUV, 1.0), mtp.p[mid].dispUv1.xyz));
  vec2 dsb = pc.ids.x >= 0 ? mtp.p[mid].scalar1.zw : vec2(1.0, 0.0);
  float dispRow = mtp.p[mid].semanticUdimSlots2.y;
  // emissive.w bit 0 is a per-draw displacement enable; the remaining bits
  // preserve the logical material id while ids.x addresses a canonical raster
  // payload. Do not rely on the black
  // fallback descriptor here: every material descriptor contains binding 16,
  // and a stale/reused fallback image must never be able to deform an unrelated
  // mesh.  GL has always gated this sample with uHasDisplacement.
  float disp = 0.0;
  if ((floatBitsToUint(pc.emissive.w) & 1u) != 0u) {
    float height = dispRow >= 0.0
        ? sampleDisplacement(duv, dispRow).r
        : textureLod(uDisplacementTex, duv, 0.0).r;
    disp = height * dsb.x + dsb.y;
  }
  // Guard normalize(): geometric-normal meshes (e.g. the --next flat preview)
  // store a zero normal, and normalize(vec3(0)) is NaN. With no displacement that
  // NaN still propagates (NaN * 0 == NaN) into pos -> gl_Position, clipping the
  // whole triangle. Only offset when the normal is usable.
  vec3 ndir = dot(nrm, nrm) > 1e-12 ? normalize(nrm) : vec3(0.0);
  pos += ndir * (disp * fr.disp.x);
  // Normal matrix = inverse-transpose of the model's upper-left 3x3 (derived
  // here so it costs no push-constant space).
  mat3 nmat = transpose(inverse(mat3(pc.model)));
  vNormalW = nmat * nrm;
  vUV = aUV;
  vWorldPos = (pc.model * vec4(pos, 1.0)).xyz;
  // Dominant skin joint (SkinWeights AOV) from the base 4-weight set.
  vDomJoint = -1;
  vDomWeight = 0.0;
  if (aWeight.x > vDomWeight) { vDomWeight = aWeight.x; vDomJoint = int(aJoint.x); }
  if (aWeight.y > vDomWeight) { vDomWeight = aWeight.y; vDomJoint = int(aJoint.y); }
  if (aWeight.z > vDomWeight) { vDomWeight = aWeight.z; vDomJoint = int(aJoint.z); }
  if (aWeight.w > vDomWeight) { vDomWeight = aWeight.w; vDomJoint = int(aJoint.w); }
  vUV1 = aUV1;
  vMorphInfl = aMorphInfl;
  // Per-vertex displayColor (USD primvars:displayColor, vertex interp). The GL
  // backend multiplies it into the base color (attrib 9); parity requires the
  // same here. Gated on pc.ids.w bit 32 -- meshes without color see white.
  vColor = ((pc.ids.w & 32) != 0)
               ? vec4(vtxcol.c[4 * gl_VertexIndex + 0],
                      vtxcol.c[4 * gl_VertexIndex + 1],
                      vtxcol.c[4 * gl_VertexIndex + 2],
                      vtxcol.c[4 * gl_VertexIndex + 3])
               : vec4(1.0);
  gl_Position = (pc.ids.z <= -3 ? fr.pointShadowViewProj[-3 - pc.ids.z]
                 : (pc.ids.z == -2 ? fr.shadowViewProj : fr.viewProj)) *
                pc.model * vec4(pos, 1.0);
}
