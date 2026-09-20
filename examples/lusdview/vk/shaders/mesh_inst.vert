#version 450
#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Instanced flat-shaded prototype (large-scene --next path). Per-instance 3x4
// object-to-world (instance-rate, rows = output x/y/z) at locations 3/4/5 -- the
// unused skin slots, leaving location 8 free for the per-vertex morph CSR --
// plus per-instance color/opacity + per-vertex prototype color. Mirrors the GL
// kInstancedVS so both rasterizers match.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec4 aRow0;       // o2w row 0 (instance-rate)
layout(location = 4) in vec4 aRow1;
layout(location = 5) in vec4 aRow2;
layout(location = 9) in vec4 aInstColor;  // per-instance color/opacity (instance-rate)
layout(location = 10) in vec4 aVtxColor;  // displayColor.rgb + displayOpacity
layout(location = 8) in uvec2 aMorphOffsetCount;  // GPU morph (offset,count); 0=none
struct RasterLight { vec4 positionType; vec4 directionAngle;
                     vec4 colorDiffuse; vec4 specularShape; vec4 areaParams;
                     vec4 iesAxisX; vec4 iesAxisY; vec4 iesProfile[6]; };

// Skeletal skinning of the PROTOTYPE, in prototype-LOCAL space (before the
// per-instance transform), so all instances of a prototype share one bone block --
// sound because USD instancing requires identical composed contents, hence one
// skeleton and one pose. Joints/weights come in BY ADDRESS (DrawMeta), not as
// vertex attributes: locations 3/4/5 are taken by the instance rows, and the
// merged multi-draw path could not supply per-mesh vertex bindings anyway. Those
// MDI draws carry jointAddr == 0 and skip skinning entirely.
layout(set = 1, binding = 0, std430) readonly buffer BoneRows { vec4 boneRows[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer JointRef {
  uvec4 v[];
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WeightRef {
  vec4 v[];
};

// Per-draw metadata (set 6), shared with mesh_inst.frag -- must match DrawMetaCPU.
struct DrawMeta { ivec4 ids; uint64_t jointAddr; uint64_t weightAddr; };
layout(set = 3, binding = 0, std430) readonly buffer DrawMetaB { DrawMeta meta[]; };

mat4 fetchBone(uint idx) {
  int base = int(idx) * 4;
  return mat4(boneRows[base + 0], boneRows[base + 1],
              boneRows[base + 2], boneRows[base + 3]);
}

// GPU blendshape morph (sets 7/8/9, same layout as deform.glsl): a morphed
// prototype is summed into its local position before the per-instance transform.
// Non-morphed meshes bind the shared dummy sets and carry count 0, so the loop
// is skipped. The morph is per-prototype -- shared by all instances of it.
layout(set = 1, binding = 2, std430) readonly buffer MorphDeltas { uvec2 morphDeltas[]; };
layout(set = 1, binding = 3, std430) readonly buffer MorphCoeffs { float morphCoeff[]; };
layout(set = 1, binding = 4, std430) readonly buffer MorphChan { uint morphChanPacked[]; };
uint morphChanId(uint e) {  // entry index -> channelId (low/high 16 of the uint)
  return (morphChanPacked[e >> 1u] >> ((e & 1u) << 4u)) & 0xffffu;
}

// Frame UBO (set 5): viewProj / camPos / scene bbox / renderMode, frame-constant
// for the whole instanced pass (shared with the mesh pipeline).
layout(set = 2, binding = 0) uniform Frame {
  vec4 disp;
  mat4 viewProj;     // P * V
  vec4 camPos;       // xyz camera, w depthScale
  vec4 sceneMin;
  vec4 sceneExtent;
  vec4 lightDir;
  vec4 lightColor;
  RasterLight rasterLights[16];
  uvec4 rasterLightInfo;
  ivec4 mode;        // .x renderMode
  mat4 envRot;        // world -> environment rotation (dome IBL)
  vec4 iblColor;      // .rgb dome effectiveColor, .w = hasIbl (0/1)
  vec4 iblParams;     // .x = prefiltered mip count
  mat4 shadowViewProj;
  vec4 pointShadowLight;
  mat4 pointShadowViewProj[6];
  vec4 clippingPlanes[8];
  ivec4 clippingInfo;
} fr;
// Per-draw push constant: the base index of this draw in the DrawMeta SSBO. In
// the per-mesh loop each draw is separate (gl_DrawIDARB == 0) so baseDraw selects
// the mesh; in a multi-draw-indirect batch baseDraw is the batch's first slot and
// gl_DrawIDARB walks its commands. The resolved slot is passed flat to the
// fragment stage (gl_DrawIDARB is not available there).
layout(push_constant) uniform InstPushC { ivec4 draw; } pc;  // .x = baseDraw

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out float vOpacity;
layout(location = 4) flat out int vInstanceId;
layout(location = 5) flat out int vDrawSlot;

void main() {
  vInstanceId = gl_InstanceIndex;
  vDrawSlot = pc.draw.x + gl_DrawIDARB;
  // GPU blendshape morph (active-channel skip), before the per-instance transform.
  vec3 pos = aPos;
  if (aMorphOffsetCount.y > 0u) {
    int mbase = int(aMorphOffsetCount.x);
    int mcount = min(int(aMorphOffsetCount.y), 256);
    for (int i = 0; i < 256; ++i) {
      if (i >= mcount) break;
      float c = morphCoeff[int(morphChanId(uint(mbase + i)))];
      if (abs(c) < 1e-6) continue;
      uvec2 raw = morphDeltas[mbase + i];
      vec2 a = unpackHalf2x16(raw.x);  // (channelId, dx)
      vec2 b = unpackHalf2x16(raw.y);  // (dy, dz)
      pos += c * vec3(a.y, b.x, b.y);
    }
  }
  // Linear-blend skinning (prototype-local), before the per-instance transform.
  // A vertex whose weights sum to 0 -- every vertex of an unskinned prototype --
  // passes through untouched, mirroring the non-instanced program.
  vec3 nrm = aNormal;
  DrawMeta md = meta[vDrawSlot];
  if (md.jointAddr != 0ul) {
    uvec4 joint = JointRef(md.jointAddr).v[gl_VertexIndex];
    vec4 weight = WeightRef(md.weightAddr).v[gl_VertexIndex];
    float wsum = weight.x + weight.y + weight.z + weight.w;
    if (wsum > 0.0) {
      mat4 skin = fetchBone(joint.x) * weight.x + fetchBone(joint.y) * weight.y
                + fetchBone(joint.z) * weight.z + fetchBone(joint.w) * weight.w;
      pos = (skin * vec4(pos, 1.0)).xyz;
      nrm = normalize((skin * vec4(aNormal, 0.0)).xyz);
    }
  }
  vec4 p = vec4(pos, 1.0);
  vec3 wp = vec3(dot(p, aRow0), dot(p, aRow1), dot(p, aRow2));
  vec3 n = vec3(dot(nrm, aRow0.xyz), dot(nrm, aRow1.xyz),
                dot(nrm, aRow2.xyz));
  vWorldPos = wp;
  vNormal = normalize(n);
  vColor = aInstColor.rgb * aVtxColor.rgb;
  vOpacity = clamp(aInstColor.a * aVtxColor.a, 0.0, 1.0);
  gl_Position = (pc.draw.y >= 2 ? fr.pointShadowViewProj[pc.draw.y - 2]
                 : (pc.draw.y != 0 ? fr.shadowViewProj : fr.viewProj)) *
                vec4(wp, 1.0);
}
