// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - Metal Shading Language source.
//
// Compiled at runtime with MTLDevice.makeLibrary(source:). Keeping the shader in
// a Swift string means the example needs no offline Metal toolchain (xcodebuild
// -downloadComponent MetalToolchain), which is what lusdview's Metal backend
// does too. A headless CTest run compiles and renders this source, so shader
// syntax errors fail CI.
//
// Material model: tydra-next resolves UsdPreviewSurface / MaterialX OpenPBR
// into the canonical packed OpenPBR block (see lmv.h lmv_pbr_index). The shader
// evaluates an OpenPBR-lite response: GGX base specular + energy-conserving
// diffuse, coat, sheen, emission, mask/blend opacity and an approximate
// transmission. Subsurface/thin-film/anisotropy are intentionally not evaluated
// in this preview.
import Foundation

enum Shaders {
    static let source = #"""
#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// Layout shared with Swift (Types.swift) and the C core (core/lmv.h)
// ---------------------------------------------------------------------------

struct LMVVertex {
  packed_float3 p;
  packed_float3 n;
  packed_float2 uv;
  packed_float2 uv1;
  packed_float4 c;
};

struct GPULight {
  float4 positionType;   // xyz position / direction, w = lmv_light_type
  float4 colorIntensity; // rgb * intensity, w = cos outer angle (spot)
  float4 direction;      // xyz towards the light
  float4 params;         // x radius, y cos inner angle, z, w unused
};

struct SlotParams {
  float4 uvOffsetScale; // xy offset, zw scale
  float4 uvRot;         // x cos, y sin
  float4 valueScale;    // rgb
  float4 valueBias;     // rgb
  int4 meta;            // x wrap, y srgb, z channel, w valid
  int4 udim;            // x is_udim, y LUT offset, z tile count, w unused
  int4 ptex;            // x is_ptex, y rect offset, z face count, w unused
};

struct MaterialUniforms {
  float4 baseColor;      // rgb, w = base weight
  float4 emission;       // rgb, w = luminance
  float4 specular;       // rgb, w = specular weight
  float4 coat;           // rgb, w = weight
  float4 sheen;          // rgb, w = weight
  float4 transmission;   // rgb, w = weight
  float4 p0;             // metalness, diffuse roughness, spec roughness, ior
  float4 p1;             // coat roughness, coat ior, sheen roughness, opacity
  float4 p2;             // alpha mode, alpha cutoff, normal scale, thin walled
  float4 p3;             // coat normal scale, -, -, -
  float4 p4;             // thin film weight, thickness nm, ior, anisotropy
  float4 p5;             // subsurface weight, transmission depth, coat affect color, spec rotation
  float4 p6;             // subsurface color rgb, scale
  int4 unused0;
  int4 unused1;
  SlotParams slots[7];   // base, metallic, roughness, normal, emission, opacity, coat normal
};

struct DrawUniforms {
  float4x4 world;
  float4x4 normalMat;
  float4 params; // x = double sided, y = interior
};

struct FrameUniforms {
  float4x4 view;
  float4x4 proj;
  float4x4 invViewProj;
  float4x4 shadowMatrix;
  float4 cameraPos;    // xyz
  float4 shadowParams; // x enabled, y texel, z bias, w strength
  float4 envParams;    // x rotation, y intensity, z background bluer, w unused
  float4 toneParams;   // x exposure stops
  float4 sh0;
  float4 sh1;
  float4 sh2;
  float4 sh3;
  float4 sh4;
  float4 sh5;
  float4 sh6;
  float4 sh7;
  float4 sh8;
  float4 misc;         // x light count, y has env map, z studio key, w grid
  float4 gridParams;   // x spacing, y half extent, z up axis, w enabled
  float4 gridCenter;   // xyz grid center (world), w unused
  float4 flags;        // x wireframe
  float4 cm0;          // working-to-display 3x3, row 0
  float4 cm1;
  float4 cm2;
  float4x4 shadowMatrices[4]; // 2x2 atlas casters, column-major
  float4 shadowFlags;  // per-caster enabled (x..w)
};

// ---------------------------------------------------------------------------
// Constants / helpers
// ---------------------------------------------------------------------------

constexpr sampler linearClamp(coord::normalized, address::clamp_to_edge,
                              filter::linear, mip_filter::linear);
constexpr sampler linearRepeat(coord::normalized, address::repeat,
                               filter::linear, mip_filter::linear);
constexpr sampler shadowSampler(coord::normalized, address::clamp_to_edge,
                                filter::linear, compare_func::less_equal);

// Sample one caster from the 2x2 shadow atlas. Returns 1 = lit.
float sampleShadow(constant FrameUniforms& frame, int index, float3 worldPos,
                   float3 n, float3 toLight, depth2d<float> shadowMap) {
  const float4 clip = frame.shadowMatrices[index] * float4(worldPos, 1.0f);
  if (clip.w <= 1e-6f) return 1.0f;
  const float3 ndc = clip.xyz / clip.w;
  float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
  if (uv.x <= 0.001f || uv.x >= 0.999f || uv.y <= 0.001f ||
      uv.y >= 0.999f || ndc.z <= 0.0f || ndc.z >= 1.0f) {
    return 1.0f;
  }
  // 2x2 atlas tiles, row 0 at v = 0.
  const float2 tile = float2(float(index & 1), float(index >> 1));
  uv = (uv + tile) * 0.5f;
  const float texel = frame.shadowParams.y;
  const float bias = frame.shadowParams.z +
                     0.004f * (1.0f - saturate(dot(n, toLight)));
  float acc = 0.0f;
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      acc += shadowMap.sample_compare(
          shadowSampler, uv + float2(float(ox), float(oy)) * texel,
          ndc.z - bias);
    }
  }
  return mix(1.0f, acc / 9.0f, frame.shadowParams.w);
}

constant float kPi = 3.14159265358979323846;

float saturatef(float v) { return clamp(v, 0.0f, 1.0f); }
float3 saturate3(float3 v) { return clamp(v, float3(0.0f), float3(1.0f)); }

float3 srgbToLinear(float3 c) {
  float3 lo = c / 12.92f;
  float3 hi = pow(max(c + 0.055f, float3(0.0f)) / 1.055f, float3(2.4f));
  return select(hi, lo, c <= float3(0.04045f));
}

float luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Khronos PBR Neutral tone mapping + sRGB encode.
float3 tonemap(float3 color, float exposureStops) {
  color *= exp2(exposureStops);
  color = max(color, float3(0.0f));
  const float startCompression = 0.8f - 0.04f;
  const float x = min(color.r, min(color.g, color.b));
  const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
  color -= offset;
  const float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return color;
  const float d = 1.0f - startCompression;
  const float newPeak = 1.0f - d * d / (peak + d - startCompression);
  color *= newPeak / peak;
  const float g = 1.0f - 1.0f / (0.15f * (peak - newPeak) + 1.0f);
  return mix(color, float3(newPeak), g);
}

float3 linearToSrgb(float3 c) {
  float3 v = saturate3(c);
  float3 lo = v * 12.92f;
  float3 hi = 1.055f * pow(v, float3(1.0f / 2.4f)) - 0.055f;
  return select(hi, lo, v <= float3(0.0031308f));
}

float3 dirToEquirect(float3 dir) {
  float u = atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;
  float v = asin(clamp(dir.y, -1.0f, 1.0f)) / kPi + 0.5f;
  return float3(u, 1.0f - v, 0.0f);
}

float3 studioEnvironment(float3 dir) {
  const float up = saturatef(dir.y * 0.5f + 0.5f);
  const float3 sky = mix(float3(0.10f, 0.11f, 0.13f),
                         float3(0.55f, 0.60f, 0.68f), up);
  const float3 ground = float3(0.14f, 0.13f, 0.12f);
  float3 c = dir.y >= 0.0f ? sky : mix(ground, sky, saturatef(1.0f + dir.y * 4.0f));
  const float key = pow(saturatef(dot(dir, normalize(float3(0.5f, 0.7f, 0.5f)))),
                        32.0f);
  c += float3(3.2f, 3.0f, 2.8f) * key;
  const float rim = pow(saturatef(dot(dir, normalize(float3(-0.6f, 0.2f, -0.7f)))),
                        16.0f);
  c += float3(1.0f, 1.2f, 1.6f) * rim * 0.8f;
  return c;
}

float3 environmentRadiance(texture2d<float> envMap, bool hasEnv,
                           constant FrameUniforms& frame, float3 dir,
                           float roughness, float levelCount) {
  if (hasEnv) {
    const float3 uv = dirToEquirect(dir);
    const float lod = clamp(roughness * levelCount, 0.0f, levelCount - 1.0f);
    return envMap.sample(linearRepeat, uv.xy, level(lod)).rgb *
           frame.envParams.y;
  }
  return studioEnvironment(dir) * frame.envParams.y;
}

// Cosine-convolved SH9 irradiance (the coefficients are pre-convolved on the
// CPU, so evaluation is just the polynomial).
float3 shIrradiance(constant FrameUniforms& frame, float3 n) {
  const float x = n.x, y = n.y, z = n.z;
  float3 r = frame.sh0.xyz * 0.282095f;
  r += frame.sh1.xyz * (0.488603f * y);
  r += frame.sh2.xyz * (0.488603f * z);
  r += frame.sh3.xyz * (0.488603f * x);
  r += frame.sh4.xyz * (1.092548f * x * y);
  r += frame.sh5.xyz * (1.092548f * y * z);
  r += frame.sh6.xyz * (0.315392f * (3.0f * z * z - 1.0f));
  r += frame.sh7.xyz * (1.092548f * x * z);
  r += frame.sh8.xyz * (0.546274f * (x * x - y * y));
  return max(r, float3(0.0f));
}

// ---------------------------------------------------------------------------
// Mesh shading
// ---------------------------------------------------------------------------

struct VSOut {
  float4 position [[position]];
  float3 worldPos;
  float3 normal;
  float2 uv;
  float2 uv1;
  float4 color;
  int faceId [[flat]];
};

struct DeformedVertex {
  float4 worldPos;
  float3 worldNormal;
};

// Shared by the mesh and shadow passes. Bind poses stay in the vertex buffer;
// blend-shape deltas and the per-joint skin palette are applied per vertex so
// playback never re-uploads geometry.
inline DeformedVertex deformVertex(uint vid,
                                   const LMVVertex v,
                                   constant DrawUniforms& draw,
                                   device const float4x4* skinMats,
                                   device const float* morphDeltas,
                                   device const float* morphWeights,
                                   device const ushort* jointIndices,
                                   device const float* jointWeights) {
  float3 localPos = float3(v.p);
  const int morphTargets = int(draw.params.w);
  if (morphTargets > 0) {
    for (int t = 0; t < morphTargets; ++t) {
      const int base = (int(vid) * morphTargets + t) * 3;
      localPos += morphWeights[t] *
                  float3(morphDeltas[base], morphDeltas[base + 1],
                         morphDeltas[base + 2]);
    }
  }

  DeformedVertex out;
  if (draw.params.z > 0.5f) {
    float4 acc = float4(0.0f);
    float3 accN = float3(0.0f);
    float weightSum = 0.0f;
    const int base = int(vid) * 4;
    const float3 localN = float3(v.n);
    for (int k = 0; k < 4; ++k) {
      const float w = jointWeights[base + k];
      if (w <= 0.0f) continue;
      const float4x4 m = skinMats[jointIndices[base + k]];
      acc += w * (m * float4(localPos, 1.0f));
      accN += w * (m * float4(localN, 0.0f)).xyz;
      weightSum += w;
    }
    if (weightSum > 0.0f) {
      out.worldPos = acc;
      out.worldNormal = accN / weightSum;
    } else {
      out.worldPos = draw.world * float4(localPos, 1.0f);
      out.worldNormal = (draw.normalMat * float4(localN, 0.0f)).xyz;
    }
  } else {
    out.worldPos = draw.world * float4(localPos, 1.0f);
    out.worldNormal =
        (draw.normalMat * float4(float3(v.n), 0.0f)).xyz;
  }
  return out;
}

vertex VSOut meshVertex(uint vid [[vertex_id]],
                        device const LMVVertex* verts [[buffer(0)]],
                        constant DrawUniforms& draw [[buffer(1)]],
                        constant FrameUniforms& frame [[buffer(2)]],
                        device const uint* faceIds [[buffer(3)]],
                        device const float4x4* skinMats [[buffer(4)]],
                        device const float* morphDeltas [[buffer(5)]],
                        device const float* morphWeights [[buffer(6)]],
                        device const ushort* jointIndices [[buffer(7)]],
                        device const float* jointWeights [[buffer(8)]]) {
  const LMVVertex v = verts[vid];
  const DeformedVertex deformed =
      deformVertex(vid, v, draw, skinMats, morphDeltas, morphWeights,
                   jointIndices, jointWeights);
  VSOut o;
  o.position = frame.proj * (frame.view * deformed.worldPos);
  o.worldPos = deformed.worldPos.xyz;
  o.normal = normalize(deformed.worldNormal);
  o.uv = float2(v.uv);
  o.uv1 = float2(v.uv1);
  o.color = float4(v.c);
  o.faceId = int(faceIds[vid]);
  return o;
}

float4 sampleSlot(texture2d<float> tex, constant SlotParams& sp, float2 uv) {
  if (sp.meta.w == 0) return float4(1.0f);
  float2 st = uv * sp.uvOffsetScale.zw;
  const float2 rot = sp.uvRot.xy;
  st = float2(st.x * rot.x - st.y * rot.y, st.x * rot.y + st.y * rot.x);
  st += sp.uvOffsetScale.xy;
  // USD `st` has v = 0 at the image bottom; uploaded textures are top-row-first.
  st.y = 1.0f - st.y;
  float4 v = (sp.meta.x == 0) ? tex.sample(linearRepeat, st)
                              : tex.sample(linearClamp, st);
  v = v * sp.valueScale + sp.valueBias;
  return v;
}

float2 slotUv(constant SlotParams& sp, float2 uv, float2 uv1) {
  return sp.udim.w != 0 ? uv1 : uv;
}

float3 applyColorMatrix(constant FrameUniforms& frame, float3 rgb) {
  return float3(dot(frame.cm0.xyz, rgb), dot(frame.cm1.xyz, rgb),
                dot(frame.cm2.xyz, rgb));
}

float4 samplePtex(texture2d<float> tex, constant SlotParams& sp, float2 uv,
                  int faceId, constant float4* ptexRects) {
  if (sp.ptex.x == 0) return float4(1.0f);
  const int index = sp.ptex.y + faceId;
  if (faceId < 0 || faceId >= sp.ptex.z) {
    return float4(0.5f, 0.0f, 0.5f, 1.0f);
  }
  const float4 rect = ptexRects[index];
  if (rect.z <= 0.0f || rect.w <= 0.0f) {
    return float4(0.5f, 0.0f, 0.5f, 1.0f);  // face was not decoded
  }
  const float2 local = clamp(uv, float2(0.0f), float2(1.0f));
  const float2 st = rect.xy + local * rect.zw;
  const float4 value = tex.sample(linearClamp, st);
  return value * sp.valueScale + sp.valueBias;
}

float4 sampleSlotArray(texture2d_array<float> tex, constant SlotParams& sp,
                       float2 uv, constant int* udimLut) {
  if (sp.meta.w == 0) return float4(1.0f);
  float2 st = uv * sp.uvOffsetScale.zw;
  const float2 rot = sp.uvRot.xy;
  st = float2(st.x * rot.x - st.y * rot.y, st.x * rot.y + st.y * rot.x);
  st += sp.uvOffsetScale.xy;
  const int u = clamp(int(floor(st.x)), 0, 9);
  const int v = clamp(int(floor(st.y)), 0, 9);
  const int slice = udimLut[sp.udim.y + v * 10 + u];
  if (slice < 0) return float4(0.5f, 0.0f, 0.5f, 1.0f); // missing tile
  float2 local = fract(st);
  // Flip inside the tile, not the tile row (v = 0 selects tile row 0).
  local.y = 1.0f - local.y;
  float4 value = sp.meta.x == 0
                     ? tex.sample(linearRepeat, local, uint(slice))
                     : tex.sample(linearClamp, local, uint(slice));
  return value * sp.valueScale + sp.valueBias;
}

float channelValue(float4 v, int channel) {
  switch (channel) {
    case 0: return v.r;
    case 1: return v.g;
    case 2: return v.b;
    case 3: return v.a;
    default: return v.r;
  }
}

float ggxDistribution(float nDotH, float roughness) {
  const float a = max(roughness * roughness, 1e-3f);
  const float a2 = a * a;
  const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
  return a2 / max(kPi * d * d, 1e-6f);
}

float smithG1(float nDotX, float roughness) {
  const float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
  return nDotX / max(nDotX * (1.0f - k) + k, 1e-5f);
}

float3 fresnelSchlick(float cosTheta, float3 f0) {
  return f0 + (float3(1.0f) - f0) * pow(1.0f - cosTheta, 5.0f);
}

float ggxAniso(float nDotH, float tDotH, float bDotH, float at, float ab) {
  const float a2 = at * ab;
  const float3 v = float3(ab * tDotH, at * bDotH, a2 * nDotH);
  const float d = max(dot(v, v), 1e-6f);
  return a2 / max(kPi * d * d, 1e-6f);
}

float3 evalSpecularAniso(float3 n, float3 v, float3 l, float3 f0,
                         float roughness, float anisotropy, float3 t, float3 b) {
  const float nDotL = saturatef(dot(n, l));
  const float nDotV = max(saturatef(dot(n, v)), 1e-4f);
  if (nDotL <= 0.0f) return float3(0.0f);
  const float3 h = normalize(v + l);
  const float nDotH = saturatef(dot(n, h));
  const float vDotH = saturatef(dot(v, h));
  const float at = max(0.002f, roughness * (1.0f + anisotropy));
  const float ab = max(0.002f, roughness * (1.0f - anisotropy));
  const float d = ggxAniso(nDotH, dot(t, h), dot(b, h), at, ab);
  const float g = smithG1(nDotL, roughness) * smithG1(nDotV, roughness);
  const float3 f = fresnelSchlick(vDotH, f0);
  return d * g * f / max(4.0f * nDotL * nDotV, 1e-5f) * nDotL;
}

// Airy reflectance of a thin dielectric film, expressed as a scale on the base
// Fresnel. A 550 nm reference wavelength keeps it stable without a spectral fit.
float3 thinFilmScale(float cosTheta, float ior, float thicknessNm, float3 f0) {
  if (thicknessNm <= 0.0f) return float3(1.0f);
  const float cosI = clamp(cosTheta, 1e-3f, 1.0f);
  const float sinI2 = max(0.0f, 1.0f - cosI * cosI);
  const float eta = max(ior, 1.0001f);
  const float cosT = sqrt(max(0.0f, 1.0f - sinI2 / (eta * eta)));
  const float r01 = (cosI - eta * cosT) / (cosI + eta * cosT);
  const float r12 = (eta * cosT - cosI) / (eta * cosT + cosI);
  const float phase = 4.0f * kPi * eta * thicknessNm / 550.0f * cosT;
  const float c = cos(phase);
  const float denom = 1.0f + r01 * r01 * r12 * r12 + 2.0f * r01 * r12 * c;
  const float reflect = (r01 * r01 + r12 * r12 + 2.0f * r01 * r12 * c) /
                        max(denom, 1e-4f);
  const float base = max(fresnelSchlick(cosTheta, f0).x, 1e-4f);
  return float3(clamp(reflect / base, 0.0f, 6.0f));
}

float3 evalSpecular(float3 n, float3 v, float3 l, float3 f0, float roughness) {
  const float nDotL = saturatef(dot(n, l));
  const float nDotV = max(saturatef(dot(n, v)), 1e-4f);
  if (nDotL <= 0.0f) return float3(0.0f);
  const float3 h = normalize(v + l);
  const float nDotH = saturatef(dot(n, h));
  const float vDotH = saturatef(dot(v, h));
  const float d = ggxDistribution(nDotH, roughness);
  const float g = smithG1(nDotL, roughness) * smithG1(nDotV, roughness);
  const float3 f = fresnelSchlick(vDotH, f0);
  return d * g * f / max(4.0f * nDotL * nDotV, 1e-5f) * nDotL;
}

float3 evalDirect(GPULight light, float3 p, float3 n, float3 v,
                  constant FrameUniforms& frame) {
  const int type = int(light.positionType.w + 0.5f);
  float3 toLight = light.direction.xyz;
  float attenuation = 1.0f;
  if (type != 1) {
    const float3 delta = light.positionType.xyz - p;
    const float dist2 = max(dot(delta, delta), 1e-6f);
    toLight = delta * rsqrt(dist2);
    attenuation = 1.0f / dist2;
    if (type == 2) {
      const float cosOuter = light.colorIntensity.w;
      const float cosInner = light.params.y;
      const float c = dot(-toLight, light.direction.xyz);
      attenuation *= smoothstep(cosOuter, max(cosInner, cosOuter + 1e-4f), c);
    }
  }
  const float nDotL = saturatef(dot(n, toLight));
  if (nDotL <= 0.0f) return float3(0.0f);
  return light.colorIntensity.rgb * attenuation * nDotL;
}

fragment float4 meshFragment(VSOut in [[stage_in]],
                             bool frontFacing [[front_facing]],
                             constant FrameUniforms& frame [[buffer(2)]],
                             constant MaterialUniforms& mat [[buffer(3)]],
                             constant GPULight* lights [[buffer(4)]],
                             texture2d<float> tex0 [[texture(0)]],
                             texture2d<float> tex1 [[texture(1)]],
                             texture2d<float> tex2 [[texture(2)]],
                             texture2d<float> tex3 [[texture(3)]],
                             texture2d<float> tex4 [[texture(4)]],
                             texture2d<float> tex5 [[texture(5)]],
                             texture2d<float> tex6 [[texture(6)]],
                             depth2d<float> shadowMap [[texture(7)]],
                             texture2d<float> envMap [[texture(8)]],
                             texture2d_array<float> arr0 [[texture(9)]],
                             texture2d_array<float> arr1 [[texture(10)]],
                             texture2d_array<float> arr2 [[texture(11)]],
                             texture2d_array<float> arr3 [[texture(12)]],
                             texture2d_array<float> arr4 [[texture(13)]],
                             texture2d_array<float> arr5 [[texture(14)]],
                             texture2d_array<float> arr6 [[texture(15)]],
                             constant int* udimLut [[buffer(5)]],
                             constant float4* ptexRects [[buffer(6)]]) {
  float3 n = normalize(in.normal);
  if (!frontFacing) n = -n;
  const float3 v = normalize(frame.cameraPos.xyz - in.worldPos);

  // Resolve all six material slots. UDIM slots sample a texture array through
  // the (udim-1001) LUT; regular slots sample a 2D texture.
  float4 s0 = mat.slots[0].ptex.x != 0
                  ? samplePtex(tex0, mat.slots[0], slotUv(mat.slots[0], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[0].udim.x != 0
                         ? sampleSlotArray(arr0, mat.slots[0], slotUv(mat.slots[0], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex0, mat.slots[0], slotUv(mat.slots[0], in.uv, in.uv1)));
  float4 s1 = mat.slots[1].ptex.x != 0
                  ? samplePtex(tex1, mat.slots[1], slotUv(mat.slots[1], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[1].udim.x != 0
                         ? sampleSlotArray(arr1, mat.slots[1], slotUv(mat.slots[1], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex1, mat.slots[1], slotUv(mat.slots[1], in.uv, in.uv1)));
  float4 s2 = mat.slots[2].ptex.x != 0
                  ? samplePtex(tex2, mat.slots[2], slotUv(mat.slots[2], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[2].udim.x != 0
                         ? sampleSlotArray(arr2, mat.slots[2], slotUv(mat.slots[2], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex2, mat.slots[2], slotUv(mat.slots[2], in.uv, in.uv1)));
  float4 s3 = mat.slots[3].ptex.x != 0
                  ? samplePtex(tex3, mat.slots[3], slotUv(mat.slots[3], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[3].udim.x != 0
                         ? sampleSlotArray(arr3, mat.slots[3], slotUv(mat.slots[3], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex3, mat.slots[3], slotUv(mat.slots[3], in.uv, in.uv1)));
  float4 s4 = mat.slots[4].ptex.x != 0
                  ? samplePtex(tex4, mat.slots[4], slotUv(mat.slots[4], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[4].udim.x != 0
                         ? sampleSlotArray(arr4, mat.slots[4], slotUv(mat.slots[4], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex4, mat.slots[4], slotUv(mat.slots[4], in.uv, in.uv1)));
  float4 s5 = mat.slots[5].ptex.x != 0
                  ? samplePtex(tex5, mat.slots[5], slotUv(mat.slots[5], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[5].udim.x != 0
                         ? sampleSlotArray(arr5, mat.slots[5], slotUv(mat.slots[5], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex5, mat.slots[5], slotUv(mat.slots[5], in.uv, in.uv1)));
  float4 s6 = mat.slots[6].ptex.x != 0
                  ? samplePtex(tex6, mat.slots[6], slotUv(mat.slots[6], in.uv, in.uv1), in.faceId, ptexRects)
                  : (mat.slots[6].udim.x != 0
                         ? sampleSlotArray(arr6, mat.slots[6], slotUv(mat.slots[6], in.uv, in.uv1), udimLut)
                         : sampleSlot(tex6, mat.slots[6], slotUv(mat.slots[6], in.uv, in.uv1)));

  // Normal mapping with a derivative TBN: no tangent storage required.
  const float3 dpdx = dfdx(in.worldPos);
  const float3 dpdy = dfdy(in.worldPos);
  const float2 duvx = dfdx(in.uv);
  const float2 duvy = dfdy(in.uv);
  float3 tangent = normalize(dpdx * duvy.y - dpdy * duvx.y);
  if (!all(isfinite(tangent)) || length(tangent) < 1e-6f) {
    tangent = normalize(cross(n, float3(0.0f, 1.0f, 0.0f)));
    if (length(tangent) < 1e-6f) tangent = float3(1.0f, 0.0f, 0.0f);
  }
  tangent = normalize(tangent - n * dot(n, tangent));
  const float3 bitangent = cross(n, tangent);

  const float3 nrmSample = normalize(float3((s3.xy * 2.0f - 1.0f) *
                                                mat.p2.z,
                                            s3.z * 2.0f - 1.0f));
  if (mat.slots[3].meta.w != 0 && dot(nrmSample, nrmSample) > 1e-6f) {
    n = normalize(tangent * nrmSample.x + bitangent * nrmSample.y +
                  n * nrmSample.z);
  }

  float3 baseColor = mat.baseColor.rgb * in.color.rgb;
  if (mat.slots[0].meta.w != 0) {
    const float3 rgb =
        mat.slots[0].meta.y != 0 ? srgbToLinear(s0.rgb) : s0.rgb;
    baseColor *= rgb;
  }
  baseColor *= mat.baseColor.w;

  float metalness = mat.p0.x;
  if (mat.slots[1].meta.w != 0) {
    metalness = channelValue(s1, mat.slots[1].meta.z);
  }
  float roughness = mat.p0.z;
  if (mat.slots[2].meta.w != 0) {
    roughness = channelValue(s2, mat.slots[2].meta.z);
  }
  roughness = clamp(roughness, 0.02f, 1.0f);
  metalness = saturatef(metalness);

  float alpha = mat.p1.w;
  if (mat.slots[5].meta.w != 0) {
    alpha *= channelValue(s5, mat.slots[5].meta.z);
  }
  alpha *= in.color.a;
  const int alphaMode = int(mat.p2.x + 0.5f);
  if (alphaMode == 1 && alpha < mat.p2.y) discard_fragment();
  if (alphaMode == 0) alpha = 1.0f;

  float3 emission = mat.emission.rgb * mat.emission.w;
  if (mat.slots[4].meta.w != 0) {
    const float3 rgb = mat.slots[4].meta.y != 0 ? srgbToLinear(s4.rgb) : s4.rgb;
    emission *= rgb;
  }

  const float nDotV = max(saturatef(dot(n, v)), 1e-4f);
  float3 f0 = mix(float3(0.04f) * mat.specular.w, baseColor, metalness) *
              mat.specular.rgb;
  if (mat.p4.x > 0.0f) {
    f0 *= thinFilmScale(nDotV, mat.p4.z, mat.p4.y, f0);
  }
  // Coat tint darkens what is underneath it.
  baseColor *= mix(float3(1.0f), mat.coat.rgb,
                   saturatef(mat.coat.w * mat.p5.z));
  const float3 diffuseColor =
      baseColor * (1.0f - metalness) * (1.0f - mat.transmission.w);

  float3 color = float3(0.0f);

  // Direct analytic lights.
  const int lightCount = int(frame.misc.x + 0.5f);
  for (int i = 0; i < lightCount; ++i) {
    const GPULight light = lights[i];
    const float3 radiance = evalDirect(light, in.worldPos, n, v, frame);
    if (radiance.r + radiance.g + radiance.b <= 0.0f) continue;
    const int type = int(light.positionType.w + 0.5f);
    float3 toLight = light.direction.xyz;
    if (type != 1) toLight = normalize(light.positionType.xyz - in.worldPos);
    float shadow = 1.0f;
    if (frame.shadowParams.x > 0.5f && type == 1) {
      const float4 clip = frame.shadowMatrix * float4(in.worldPos, 1.0f);
      const float3 ndc = clip.xyz / clip.w;
      const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
      if (uv.x > 0.001f && uv.x < 0.999f && uv.y > 0.001f && uv.y < 0.999f &&
          ndc.z > 0.0f && ndc.z < 1.0f) {
        float acc = 0.0f;
        const float texel = frame.shadowParams.y;
        const float slopeBias =
            frame.shadowParams.z + 0.004f * (1.0f - saturate(dot(n, toLight)));
        for (int oy = -1; oy <= 1; ++oy) {
          for (int ox = -1; ox <= 1; ++ox) {
            acc += shadowMap.sample_compare(
                shadowSampler,
                uv + float2(float(ox), float(oy)) * texel, ndc.z - slopeBias);
          }
        }
        shadow = mix(1.0f, acc / 9.0f, frame.shadowParams.w);
      }
    }
    const float3 specular =
        fabs(mat.p4.w) > 0.01f
            ? evalSpecularAniso(n, v, toLight, f0, roughness, mat.p4.w,
                                tangent, bitangent)
            : evalSpecular(n, v, toLight, f0, roughness);
    color += (diffuseColor / kPi + specular) * radiance * shadow;
  }

  // Image-based lighting: SH diffuse + environment specular.
  const bool hasEnv = frame.misc.y > 0.5f;
  float3 irradiance = shIrradiance(frame, n);
  if (!hasEnv) {
    irradiance = float3(0.28f, 0.29f, 0.32f);
  }
  color += diffuseColor * irradiance * 0.5f;
  const float3 reflection = reflect(-v, n);
  const float mipLevels = 6.0f;
  const float3 envSpec = environmentRadiance(envMap, hasEnv, frame, reflection,
                                             roughness, mipLevels);
  const float3 envSpecLinear = applyColorMatrix(frame, envSpec);
  color += envSpecLinear * (f0 + (max(float3(1.0f - roughness), f0) - f0) *
                                        pow(1.0f - nDotV, 5.0f)) *
           0.6f;

  // Coat: a second, smoother dielectric lobe with its own normal.
  if (mat.coat.w > 0.0f) {
    float3 cn = n;
    if (mat.slots[6].meta.w != 0) {
      const float3 coatNrm = normalize(float3(
          (s6.xy * 2.0f - 1.0f) * mat.p3.x, s6.z * 2.0f - 1.0f));
      cn = normalize(tangent * coatNrm.x + bitangent * coatNrm.y +
                     n * coatNrm.z);
    }
    const float3 coatF0 = float3(0.04f) * mat.coat.w * mat.coat.rgb;
    const float coatRough = clamp(mat.p1.x, 0.02f, 1.0f);
    for (int i = 0; i < lightCount; ++i) {
      const GPULight light = lights[i];
      const int type = int(light.positionType.w + 0.5f);
      float3 toLight = light.direction.xyz;
      if (type != 1) toLight = normalize(light.positionType.xyz - in.worldPos);
      const float3 radiance = evalDirect(light, in.worldPos, cn, v, frame);
      color += evalSpecular(cn, v, toLight, coatF0, coatRough) * radiance;
    }
    const float3 coatReflection = reflect(-v, cn);
    const float3 coatEnv = environmentRadiance(envMap, hasEnv, frame, coatReflection,
                                               coatRough, mipLevels);
    color += applyColorMatrix(frame, coatEnv) * coatF0 * pow(1.0f - nDotV, 3.0f);
  }

  // Sheen: grazing-angle fabric response.
  if (mat.sheen.w > 0.0f) {
    const float sheenRough = clamp(mat.p1.z, 0.05f, 1.0f);
    const float sheen = pow(1.0f - nDotV, 3.0f) * mat.sheen.w;
    color += mat.sheen.rgb * sheen * (0.25f + 0.75f * irradiance) /
             max(sheenRough, 0.1f);
  }

  // Subsurface: wrapped diffuse plus a view-dependent translucency term.
  if (mat.p5.x > 0.0f) {
    float3 sss = float3(0.0f);
    for (int i = 0; i < lightCount; ++i) {
      const GPULight light = lights[i];
      const int type = int(light.positionType.w + 0.5f);
      float3 toLight = light.direction.xyz;
      if (type != 1) toLight = normalize(light.positionType.xyz - in.worldPos);
      const float wrap = saturatef((dot(n, toLight) + 0.5f) / 1.5f);
      const float forward = pow(saturatef(dot(v, -toLight)), 4.0f);
      sss += light.colorIntensity.rgb * (wrap * 0.5f + forward * 0.25f);
    }
    color += mat.p6.rgb * mat.p5.x * sss * 0.5f;
  }

  // Transmission: refracted environment plus tint; no volumetric absorption.
  if (mat.transmission.w > 0.0f) {
    const float eta = 1.0f / max(mat.p0.w, 1.0001f);
    const float3 refracted = refract(-v, n, eta);
    const bool tir = dot(refracted, refracted) < 1e-6f;
    const float3 dir = tir ? reflection : normalize(refracted);
    const float3 refr = environmentRadiance(envMap, hasEnv, frame, dir,
                                            roughness, mipLevels);
    color += mat.transmission.rgb * mat.transmission.w *
             applyColorMatrix(frame, refr) *
             (1.0f - saturatef(mat.p5.y * 0.1f));
    color += mat.transmission.rgb * mat.transmission.w * irradiance * 0.15f;
  }

  color += emission;
  color = tonemap(color, frame.toneParams.x);
  return float4(linearToSrgb(color), alpha);
}

fragment float4 wireFragment(VSOut in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(2)]]) {
  const float3 c = linearToSrgb(tonemap(float3(1.0f, 0.62f, 0.12f),
                                        frame.toneParams.x));
  return float4(c, 1.0f);
}

// ---------------------------------------------------------------------------
// Point splats (UsdGeomPoints / Gaussian particles) and curve ribbons
// ---------------------------------------------------------------------------

struct LMVSplat {
  packed_float3 p;
  float radius;
  packed_float4 color;
};

struct SplatOut {
  float4 position [[position]];
  float size [[point_size]];
  float4 color;
};

vertex SplatOut splatVertex(uint vid [[vertex_id]],
                            device const LMVSplat* splats [[buffer(0)]],
                            constant FrameUniforms& frame [[buffer(2)]],
                            constant float2& viewport [[buffer(4)]]) {
  const LMVSplat s = splats[vid];
  const float4 world = float4(float3(s.p), 1.0f);
  const float4 view = frame.view * world;
  SplatOut o;
  o.position = frame.proj * view;
  const float viewZ = max(-view.z, 1e-4f);
  const float pixels = s.radius * frame.proj[1][1] * viewport.y / viewZ;
  o.size = clamp(pixels, 1.0f, 256.0f);
  o.color = float4(s.color);
  return o;
}

fragment float4 splatFragment(SplatOut in [[stage_in]],
                              float2 pointCoord [[point_coord]],
                              constant FrameUniforms& frame [[buffer(2)]]) {
  const float2 d = pointCoord * 2.0f - 1.0f;
  if (dot(d, d) > 1.0f) discard_fragment();
  // Soft edge, tone mapped like the surface shader.
  const float edge = saturatef(1.0f - (dot(d, d) - 0.7f) * 3.0f);
  const float3 color = linearToSrgb(tonemap(in.color.rgb, frame.toneParams.x));
  return float4(color, in.color.a * edge);
}

struct LMVLineSegment {
  packed_float3 p0;
  packed_float3 p1;
  float width;
  packed_float4 color;
};

struct LineOut {
  float4 position [[position]];
  float4 color;
};

vertex LineOut lineVertex(uint vid [[vertex_id]],
                          device const LMVLineSegment* segments [[buffer(0)]],
                          constant FrameUniforms& frame [[buffer(2)]]) {
  const uint segment = vid / 4u;
  const uint corner = vid % 4u;
  const LMVLineSegment s = segments[segment];
  const float4 world0 = float4(float3(s.p0), 1.0f);
  const float4 world1 = float4(float3(s.p1), 1.0f);
  const float4 view0 = frame.view * world0;
  const float4 view1 = frame.view * world1;
  const float4 clip0 = frame.proj * view0;
  const float4 clip1 = frame.proj * view1;
  const float2 ndc0 = clip0.xy / max(clip0.w, 1e-6f);
  const float2 ndc1 = clip1.xy / max(clip1.w, 1e-6f);
  const float2 dir = ndc1 - ndc0;
  const float len = length(dir);
  const float2 perp = len > 1e-6f ? float2(-dir.y, dir.x) / len
                                  : float2(0.0f, 1.0f);
  const float viewZ0 = max(-view0.z, 1e-3f);
  const float viewZ1 = max(-view1.z, 1e-3f);
  const float width0 = s.width * frame.proj[1][1] / viewZ0;
  const float width1 = s.width * frame.proj[1][1] / viewZ1;
  const bool atEnd = corner >= 2u;
  const float side = (corner == 0u || corner == 2u) ? -1.0f : 1.0f;
  float4 clip = atEnd ? clip1 : clip0;
  const float width = atEnd ? width1 : width0;
  clip.xy += perp * (width * 0.5f) * side * clip.w;
  LineOut o;
  o.position = clip;
  o.color = float4(s.color);
  return o;
}

fragment float4 lineFragment(LineOut in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(2)]]) {
  const float3 color = linearToSrgb(tonemap(in.color.rgb, frame.toneParams.x));
  return float4(color, in.color.a);
}

// ---------------------------------------------------------------------------
// Background / grid
// ---------------------------------------------------------------------------

struct BgOut {
  float4 position [[position]];
  float2 ndc;
};

vertex BgOut backgroundVertex(uint vid [[vertex_id]]) {
  const float2 p = float2(float((vid << 1) & 2), float(vid & 2));
  BgOut o;
  o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
  o.ndc = p * 2.0f - 1.0f;
  return o;
}

fragment float4 backgroundFragment(BgOut in [[stage_in]],
                                   constant FrameUniforms& frame [[buffer(2)]]) {
  const float4 far = frame.invViewProj * float4(in.ndc, 0.5f, 1.0f);
  const float3 dir = normalize(far.xyz / far.w - frame.cameraPos.xyz);
  float3 c;
  if (frame.misc.y > 0.5f) {
    c = applyColorMatrix(frame, studioEnvironment(dir) * 0.35f);
  } else {
    const float up = saturatef(dir.y * 0.5f + 0.5f);
    c = mix(float3(0.055f, 0.06f, 0.07f), float3(0.16f, 0.175f, 0.20f), up);
  }
  return float4(linearToSrgb(tonemap(c, frame.toneParams.x)), 1.0f);
}

struct GridVertex {
  packed_float3 position;
  packed_float3 normal;
  packed_float2 uv;
  packed_float2 uv1;
  packed_float4 color;
};

struct GridOut {
  float4 position [[position]];
  float3 worldPos;
};

vertex GridOut gridVertex(uint vid [[vertex_id]],
                          device const GridVertex* verts [[buffer(0)]],
                          constant FrameUniforms& frame [[buffer(2)]]) {
  const GridVertex v = verts[vid];
  const float4 worldPos = float4(float3(v.position), 1.0f);
  GridOut o;
  o.position = frame.proj * (frame.view * worldPos);
  o.worldPos = worldPos.xyz;
  return o;
}

fragment float4 gridFragment(GridOut in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(2)]],
                             depth2d<float> shadowMap [[texture(7)]]) {
  const float spacing = max(frame.gridParams.x, 1e-4f);
  float3 p = in.worldPos;
  // Choose the two axes of the ground plane from the scene up axis.
  float2 coord = frame.gridParams.z > 0.5f ? p.xy : p.xz;
  const float2 g = coord / spacing;
  const float2 w = fwidth(g);
  const float2 grid = abs(fract(g - 0.5f) - 0.5f) / max(w, float2(1e-5f));
  const float line = 1.0f - min(min(grid.x, grid.y), 1.0f);
  // Radial fade from the framed scene center: the ground reads as a soft disc
  // under the subject instead of a giant plane with a hard horizon.
  const float2 center = frame.gridParams.z > 0.5f ? frame.gridCenter.xy
                                                  : frame.gridCenter.xz;
  const float radial =
      1.0f - saturatef(length(coord - center) /
                       max(frame.gridParams.y, 1e-3f));
  const float fade = radial * radial;
  const float axisWidth = max(w.x, w.y);
  const float axisX = 1.0f - saturatef(abs(coord.x) / (axisWidth * spacing * 1.5f));
  const float axisY = 1.0f - saturatef(abs(coord.y) / (axisWidth * spacing * 1.5f));
  float3 color = mix(float3(0.30f, 0.32f, 0.36f), float3(0.55f, 0.60f, 0.66f), line);
  float a = line * fade * 0.55f;
  if (axisX > 0.5f) a = max(a, axisX * fade * 0.85f);
  if (axisY > 0.5f) a = max(a, axisY * fade * 0.85f);
  // Ground contact shadow: the grid is the only large receiver in the scene.
  float shadow = 1.0f;
  if (frame.shadowFlags.x > 0.5f) {
    const float3 n = float3(0.0f, 1.0f, 0.0f);
    const float3 toLight = float3(0.0f, 1.0f, 0.0f);
    shadow = sampleShadow(frame, 0, in.worldPos, n, toLight, shadowMap);
  }
  color *= mix(0.35f, 1.0f, shadow);
  a *= mix(0.55f, 1.0f, shadow);
  // Lines only: no synthetic solid ground. A filled preview plane reads as
  // unexpected geometry in assets that author no floor (QL-style viewers keep
  // the subject on the background). Shadows still darken the lines.
  color *= mix(0.35f, 1.0f, shadow);
  a *= mix(0.55f, 1.0f, shadow);
  return float4(linearToSrgb(tonemap(color, frame.toneParams.x)),
                a * frame.gridParams.w);
}

// ---------------------------------------------------------------------------
// Shadow pass
// ---------------------------------------------------------------------------

struct ShadowOut {
  float4 position [[position]];
};

vertex ShadowOut shadowVertex(uint vid [[vertex_id]],
                              device const LMVVertex* verts [[buffer(0)]],
                              constant DrawUniforms& draw [[buffer(1)]],
                              constant FrameUniforms& frame [[buffer(2)]],
                              device const float4x4* skinMats [[buffer(4)]],
                              device const float* morphDeltas [[buffer(5)]],
                              device const float* morphWeights [[buffer(6)]],
                              device const ushort* jointIndices [[buffer(7)]],
                              device const float* jointWeights [[buffer(8)]]) {
  const LMVVertex v = verts[vid];
  const DeformedVertex deformed =
      deformVertex(vid, v, draw, skinMats, morphDeltas, morphWeights,
                   jointIndices, jointWeights);
  ShadowOut o;
  o.position = frame.shadowMatrix * deformed.worldPos;
  return o;
}

fragment float4 shadowFragment() { return float4(1.0f); }

// ---------------------------------------------------------------------------
// Final blit (offscreen screenshot / MSAA resolve path)
// ---------------------------------------------------------------------------

struct BlitOut {
  float4 position [[position]];
  float2 uv;
};

vertex BlitOut blitVertex(uint vid [[vertex_id]]) {
  const float2 p = float2(float((vid << 1) & 2), float(vid & 2));
  BlitOut o;
  o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
  o.uv = float2(p.x, 1.0f - p.y);
  return o;
}

fragment float4 blitFragment(BlitOut in [[stage_in]],
                             texture2d<float> src [[texture(0)]]) {
  return src.sample(linearClamp, in.uv);
}
"""#
}
