#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Instanced flat-shaded prototype fragment shader. Mirrors the GL kInstancedFS
// AOV ladder + headlight so instanced geometry looks identical across backends.
// Prototypes carry no UV / most material scalars, so those AOV modes fall
// through to neutral gray. Per-instance/prototype opacity is available.
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 3) in float vOpacity;
layout(location = 4) flat in int vInstanceId;
layout(location = 5) flat in int vDrawSlot;
struct RasterLight { vec4 positionType; vec4 directionAngle;
                     vec4 colorDiffuse; vec4 specularShape; vec4 areaParams;
                     vec4 iesAxisX; vec4 iesAxisY; vec4 iesProfile[6]; };

// Per-draw metadata (set 6), indexed by the vertex-resolved draw slot. Replaces
// the old per-draw push constant so a whole multi-draw-indirect batch shares one
// binding: each draw's meshId + flag bits come from meta[vDrawSlot]. Instanced
// prototypes are never selection-highlighted, so there is no emissive term.
// Must match DrawMetaCPU / mesh_inst.vert: the skin addresses are unused here but
// are part of the layout.
struct DrawMeta { ivec4 ids; uint64_t jointAddr; uint64_t weightAddr; };
layout(set = 3, binding = 0, std430) readonly buffer DrawMetaB { DrawMeta meta[]; };

// Frame UBO (set 5): camera / scene bbox / renderMode (frame-constant).
// DomeLight IBL irradiance (diffuse-only: prototypes carry no material
// scalars). Set 0 is otherwise unused by the instanced pipeline; a 1x1 black
// cube is bound when no dome IBL is baked.
layout(set = 0, binding = 12) uniform samplerCube uIrradianceMap;

layout(set = 2, binding = 0) uniform Frame {
  vec4 disp;
  mat4 viewProj;
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
  vec4 iblParams;     // .x = prefiltered mip count, .y = exposure stops
  mat4 shadowViewProj;
  vec4 pointShadowLight;
  mat4 pointShadowViewProj[6];
  vec4 clippingPlanes[8];
  ivec4 clippingInfo;
} fr;
layout(push_constant) uniform InstPushC { ivec4 draw; } pc;  // .x = baseDraw (unused here)

#ifdef LUSDVIEW_OIT
layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;
vec4 outColor;
#else
layout(location = 0) out vec4 outColor;
#endif

vec3 idColor(int id) {
  if (id < 0) return vec3(0.45);
  uint h = (uint(id) + 1u) * 2654435761u;
  return vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) * (1.0 / 255.0);
}

// Linear -> sRGB OETF for the final shaded output (see mesh.frag); the
// framebuffer is UNORM and the scene is lit in linear space.
vec3 linearToSrgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  vec3 lo = c * 12.92;
  vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, greaterThan(c, vec3(0.0031308)));
}

float ggxD(float nh, float r) {
  float a = max(r * r, 0.002), a2 = a * a;
  float d = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / max(3.14159265 * d * d, 1e-6);
}
float ggxG1(float nx, float r) {
  float k = (r + 1.0) * (r + 1.0) * 0.125;
  return nx / max(nx * (1.0 - k) + k, 1e-6);
}
vec3 fresnel(float vh, vec3 f0) {
  return f0 + (vec3(1.0) - f0) *
                  pow(1.0 - clamp(vh, 0.0, 1.0), 5.0);
}
vec3 purposeColor(int p) {
  if (p == 1) return vec3(0.2, 0.8, 0.3);
  if (p == 2) return vec3(0.2, 0.45, 0.95);
  if (p == 3) return vec3(0.95, 0.75, 0.1);
  return vec3(0.5);
}
vec3 kindColor(int k) {
  if (k == 1) return vec3(0.2, 0.8, 0.8);
  if (k == 2) return vec3(0.85, 0.3, 0.85);
  if (k == 3) return vec3(0.95, 0.6, 0.15);
  if (k == 4) return vec3(0.5, 0.85, 0.4);
  return vec3(0.35);
}

void shadeFragment() {
  for (int i = 0; i < fr.clippingInfo.x; ++i) {
    if (dot(vec4(vWorldPos, 1.0), fr.clippingPlanes[i]) < 0.0) discard;
  }
  vec3 Ngeo = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
  // Face the geometric normal toward the camera (winding-independent). Using the
  // view vector instead of gl_FrontFacing avoids the VK Y-flipped-viewport
  // winding inversion that would otherwise leave every face unlit.
  vec3 Vdir = normalize(fr.camPos.xyz - vWorldPos);
  vec3 N = Ngeo;
  if (dot(N, Vdir) < 0.0) N = -N;
  const ivec4 ids = meta[vDrawSlot].ids;
  const bool geoNrm = (ids.y & 1) != 0;
  const bool dsided = (ids.y & 2) != 0;
  const int purpose = (ids.y >> 2) & 3;
  const int kind = (ids.y >> 4) & 7;
  if (fr.mode.x != 0) {
    vec3 Nshade = geoNrm ? Ngeo : normalize(vNormal);
    if (fr.mode.x == 2) { outColor = vec4(Nshade * 0.5 + 0.5, 1.0); return; }
    if (fr.mode.x == 4) { outColor = vec4(Ngeo * 0.5 + 0.5, 1.0); return; }
    if (fr.mode.x == 6) {
      float d = clamp(length(fr.camPos.xyz - vWorldPos) / max(fr.camPos.w, 1e-3), 0.0, 1.0);
      outColor = vec4(vec3(1.0 - d), 1.0); return;
    }
    if (fr.mode.x == 7) { outColor = vec4(vColor, 1.0); return; }  // albedo
    if (fr.mode.x == 8) {
      outColor = gl_FrontFacing ? vec4(0.1, 0.7, 0.1, 1.0) : vec4(0.7, 0.1, 0.1, 1.0); return;
    }
    if (fr.mode.x == 13) {  // world position
      outColor = vec4(clamp((vWorldPos - fr.sceneMin.xyz) / fr.sceneExtent.xyz, 0.0, 1.0), 1.0); return;
    }
    if (fr.mode.x == 15) { outColor = vec4(idColor(gl_PrimitiveID), 1.0); return; }  // prim id
    if (fr.mode.x == 16) { outColor = vec4(idColor(ids.x), 1.0); return; }           // mesh id
    if (fr.mode.x == 19) {  // missing normals
      outColor = geoNrm ? vec4(0.95, 0.1, 0.85, 1.0) : vec4(0.2, 0.2, 0.2, 1.0); return;
    }
    if (fr.mode.x == 20) {  // double-sided
      outColor = dsided ? vec4(0.95, 0.55, 0.1, 1.0) : vec4(0.2, 0.2, 0.2, 1.0); return;
    }
    if (fr.mode.x == 18) { outColor = vec4(purposeColor(purpose), 1.0); return; }
    if (fr.mode.x == 29) { outColor = vec4(kindColor(kind), 1.0); return; }
    if (fr.mode.x == 26) { outColor = vec4(idColor(vInstanceId), 1.0); return; }  // instance id
    if (fr.mode.x == 12) { outColor = vec4(vec3(vOpacity), 1.0); return; }        // opacity
    if (fr.mode.x == 25) {  // curvature (screen-space geometric normal variation)
      vec3 n = Ngeo;
      float c = clamp((length(dFdx(n)) + length(dFdy(n))) * 8.0, 0.0, 1.0);
      outColor = vec4(c, 1.0 - abs(c - 0.5) * 2.0, 1.0 - c, 1.0); return;
    }
    // Modes instanced prototypes cannot supply (UV/other material scalars): neutral gray.
    outColor = vec4(0.18, 0.18, 0.18, 1.0); return;
  }
  vec3 V = normalize(fr.camPos.xyz - vWorldPos);
  // Fixed dielectric GGX material, matching the GL instanced path.
  vec3 Nf = (dot(N, V) < 0.0) ? -N : N;
  float nv = max(dot(Nf, V), 1e-4);
  const float r = 0.5;
  vec3 ambient = (fr.iblColor.w > 0.5)
                 ? vColor * texture(uIrradianceMap,
                                    normalize(mat3(fr.envRot) * Nf)).rgb *
                       fr.iblColor.rgb
                 : vColor * 0.20;
  vec3 direct = vec3(0.0);
  uint lightMask = uint(meta[vDrawSlot].ids.z);
  for (uint li = 0u; li < min(fr.rasterLightInfo.x, 16u); ++li) {
    if ((lightMask & (1u << li)) == 0u) continue;
    vec4 pt = fr.rasterLights[li].positionType;
    vec4 da = fr.rasterLights[li].directionAngle;
    vec4 lc = fr.rasterLights[li].colorDiffuse;
    vec4 ss = fr.rasterLights[li].specularShape;
    int lightType = int(pt.w + 0.5);
    int sampleCount = (lightType == 2 || lightType == 3 || lightType == 4 ||
                       lightType == 7 ||
                       lightType == 8) ? 8 : 1;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
    vec3 samplePos = pt.xyz;
    if (lightType == 7) {
      int base = sampleIndex * 3;
      samplePos = vec3(
          fr.rasterLights[li].iesProfile[base / 4][base % 4],
          fr.rasterLights[li].iesProfile[(base + 1) / 4][(base + 1) % 4],
          fr.rasterLights[li].iesProfile[(base + 2) / 4][(base + 2) % 4]);
    }
    vec3 areaX = normalize(fr.rasterLights[li].iesAxisX.xyz);
    vec3 areaY = normalize(fr.rasterLights[li].iesAxisY.xyz);
    if (lightType == 3 || lightType == 8) {
      float sx = (float(sampleIndex % 4) + 0.5) * 0.25 - 0.5;
      float sy = (float(sampleIndex / 4) + 0.5) * 0.5 - 0.5;
      samplePos += areaX * (sx * fr.rasterLights[li].areaParams.y) +
                   areaY * (sy * fr.rasterLights[li].areaParams.z);
    } else if (lightType == 2 || lightType == 4) {
      const float k = 0.5;
      float a = 6.28318530718 * (float(sampleIndex) + 0.5) / 8.0;
      float sx = cos(a) * k * fr.rasterLights[li].areaParams.x;
      float sy = sin(a) * k * fr.rasterLights[li].areaParams.x;
      samplePos += areaX * sx + areaY * sy;
    }
    vec3 L;
    float attenuation = 1.0;
    if (lightType == 5) L = normalize(da.xyz);
    else {
      vec3 toLight = samplePos - vWorldPos;
      float dist2 = max(dot(toLight, toLight), 1e-6);
      L = toLight * inversesqrt(dist2);
      attenuation = 1.0 / dist2;
    }
    float shape = 1.0;
    if (ss.w > 0.5 && lightType != 5) {
      float coneCos = dot(normalize(da.xyz), -L);
      float outer = cos(radians(clamp(da.w, 0.0, 180.0)));
      float inner = cos(radians(clamp(da.w * (1.0 - clamp(ss.y, 0.0, 1.0)),
                                      0.0, 180.0)));
      shape = smoothstep(outer, max(inner, outer + 1e-5), coneCos) *
              pow(max(coneCos, 0.0), max(ss.z, 0.0));
    }
    float ies = 1.0;
    if (lightType != 5 && lightType != 7 &&
        dot(fr.rasterLights[li].iesProfile[0],
                              fr.rasterLights[li].iesProfile[0]) > 1e-8) {
      vec3 iesDir = normalize(-L);
      float v = degrees(acos(clamp(dot(iesDir, normalize(da.xyz)), -1.0, 1.0)));
      float fy = clamp(v / 60.0, 0.0, 3.0);
      int y0 = int(floor(fy));
      int y1 = min(y0 + 1, 3);
      float az = degrees(atan(dot(iesDir, fr.rasterLights[li].iesAxisY.xyz),
                              dot(iesDir, fr.rasterLights[li].iesAxisX.xyz)));
      if (az < 0.0) az += 360.0;
      float fx = az / 60.0;
      int x0 = min(int(floor(fx)), 5);
      int x1 = (x0 + 1) % 6;
      float tx = fx - float(x0);
      float a0 = mix(fr.rasterLights[li].iesProfile[y0][x0],
                     fr.rasterLights[li].iesProfile[y0][x1], tx);
      float a1 = mix(fr.rasterLights[li].iesProfile[y1][x0],
                     fr.rasterLights[li].iesProfile[y1][x1], tx);
      ies = mix(a0, a1, fy - float(y0));
    }
    float nl = max(dot(Nf, L), 0.0);
    if (nl <= 0.0 || shape <= 0.0) continue;
    vec3 H = normalize(L + V);
    float nh = max(dot(Nf, H), 0.0), vh = max(dot(V, H), 0.0);
    vec3 F = fresnel(vh, vec3(0.04));
    vec3 spec = ggxD(nh, r) * ggxG1(nv, r) * ggxG1(nl, r) * F /
                max(4.0 * nv * nl, 1e-5);
    vec3 diff = (vec3(1.0) - F) * vColor * (1.0 / 3.14159265);
    direct += (diff * lc.w + spec * ss.x) * lc.rgb *
              (attenuation * shape * ies * nl) / float(sampleCount);
    }
  }
  if (fr.rasterLightInfo.x == 0u) {
    vec3 L = (dot(fr.lightDir.xyz, fr.lightDir.xyz) > 1e-8)
                 ? normalize(fr.lightDir.xyz)
                 : normalize(vec3(0.3, 0.5, 0.8));
    vec3 lightColor = (dot(fr.lightColor.rgb, fr.lightColor.rgb) > 1e-8)
                          ? fr.lightColor.rgb : vec3(1.0);
    float nl = max(dot(Nf, L), 0.0);
    vec3 H = normalize(L + V);
    float nh = max(dot(Nf, H), 0.0), vh = max(dot(V, H), 0.0);
    vec3 F = fresnel(vh, vec3(0.04));
    vec3 spec = ggxD(nh, r) * ggxG1(nv, r) * ggxG1(nl, r) * F /
                max(4.0 * nv * nl, 1e-5);
    vec3 diff = (vec3(1.0) - F) * vColor * (1.0 / 3.14159265);
    direct = (diff + spec) * lightColor * nl;
  }
  vec3 col = ambient + direct;
  vec3 display = col * exp2(fr.iblParams.y);
#ifndef LUSDVIEW_OIT
  display = linearToSrgb(display);
#endif
  outColor = vec4(display, vOpacity);  // no selection emissive here
}

#ifdef LUSDVIEW_OIT
void main() {
  shadeFragment();
  float alpha = clamp(outColor.a, 0.0, 1.0);
  if (alpha <= 1.0e-4) discard;
  float weight = clamp(
      pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1.0e8 *
          pow(1.0 - gl_FragCoord.z * 0.9, 3.0),
      1.0e-2, 3.0e3);
  outAccum = vec4(outColor.rgb * alpha, alpha) * weight;
  outReveal = alpha;
}
#else
void main() { shadeFragment(); }
#endif
