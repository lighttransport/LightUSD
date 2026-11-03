#include "mesh_renderer.hh"

#include <d3dcompiler.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

constexpr UINT kFrameCbStride = 512;  // >= sizeof(FrameCB), 256-aligned

// Mirrors `cbuffer Frame` in the shader.
struct FrameCB {
  float view_proj[16];
  float shadow_vp[16];
  float eye[4];
  int32_t idx[4];         // light count, env SRV, shadow SRV (<0 off), shadow light
  float shadow_params[4]; // texel, bias, strength
  float env_params[4];    // intensity
  float tone[4];          // exposure stops
  float cm[3][4];         // working -> display-linear rows
  float sh[9][4];
};
static_assert(sizeof(FrameCB) <= kFrameCbStride, "FrameCB too large");

// Root signature: b0 = Frame CBV (root descriptor), b1 = Draw (20 DWORDs: world
// matrix + material index), t0 = materials and t1 = lights (root SRVs), one
// bindless 2D texture table in space1, and the shadow map in space2. s0..s3 are
// the wrap/clamp/mirror/border samplers (explicit gradients keep the divergent
// material branches valid), s4 is the shadow comparison sampler.
const char kShader[] = R"hlsl(
struct Slot {
  int index;       // SRV descriptor index, < 0 = no texture
  int flags;       // wrap_s | wrap_t << 4 | channel << 8 | uv_set << 12 |
                   // kind << 16 (0 = 2D, 1 = UDIM array, 2 = Ptex atlas)
  float rot;
  int aux;          // UDIM: tile LUT offset; Ptex: rect table offset
  float4 uv_os;    // offset xy, scale zw
  float4 bias;
  float4 scale;
};
struct Material {
  float4 base;          // rgb, weight
  float4 emission;      // rgb, luminance scale
  float4 params;        // metalness, roughness, alpha mode, alpha cutoff
  float4 extra;         // normal scale, opacity, coat normal scale, ior
  float4 specular;      // rgb colour, weight
  float4 coat;          // rgb colour, weight
  float4 sheen;         // rgb colour, weight
  float4 transmission;  // rgb colour, weight
  float4 coat_params;   // coat roughness, coat ior, sheen roughness, thin walled
  float4 sss;           // subsurface colour, weight
  float4 misc;          // transmission depth, subsurface scale, coat affects colour
  float4 thin;          // thin film weight, thickness nm, thin film ior, anisotropy
  Slot slots[7];        // base, metallic, roughness, normal, emission, opacity,
                        // coat normal
};
struct Light {
  float4 position_type;    // xyz position, w = type (1 directional, 2 spot)
  float4 color_intensity;  // rgb * intensity, w = cos outer angle (spot)
  float4 direction;        // xyz towards the light
  float4 params;           // y = cos inner angle (spot)
};
cbuffer Frame : register(b0) {
  float4x4 view_proj;
  float4x4 shadow_vp;
  float4 eye;
  int4 idx;             // light count, env SRV, shadow SRV, shadow light
  float4 shadow_params; // texel, bias, strength
  float4 env_params;    // intensity
  float4 tone;          // exposure stops
  float4 cm0; float4 cm1; float4 cm2;
  float4 sh[9];
};
cbuffer Draw : register(b1) {
  float4x4 world;
  uint material_index;
  uint skinned;       // GPU skinning: world matrix is baked into the palette
  uint morph_targets; // blend-shape targets
  uint has_face_ids;  // mesh carries per-vertex Ptex face ids
};
StructuredBuffer<Material> materials : register(t0);
StructuredBuffer<Light> lights : register(t1);
// Vertex-stage deformation data (skinning + blend shapes).
StructuredBuffer<float4x4> skin_mats : register(t2);
StructuredBuffer<float> morph_deltas : register(t3);
StructuredBuffer<float> morph_weights : register(t4);
ByteAddressBuffer joint_indices : register(t5);   // ushort x4 per vertex
StructuredBuffer<float4> joint_weights : register(t6);
Texture2D g_tex[] : register(t0, space1);
Texture2DArray g_array[] : register(t0, space3);  // UDIM tiles
StructuredBuffer<int> udim_lut : register(t7);        // 100 per UDIM texture
StructuredBuffer<float4> ptex_rects : register(t8);   // header + one per face
StructuredBuffer<uint> face_ids : register(t9);       // per vertex (vertex stage)
Texture2D<float> g_shadow : register(t0, space2);
SamplerState s_wrap   : register(s0);
SamplerState s_clamp  : register(s1);
SamplerState s_mirror : register(s2);
SamplerState s_border : register(s3);
SamplerComparisonState s_shadow : register(s4);

struct VSIn {
  float3 pos : POSITION;
  float3 nrm : NORMAL;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
  float4 col : COLOR;
};
struct VSOut {
  float4 pos : SV_Position;
  float3 wpos : TEXCOORD0;
  float3 nrm : TEXCOORD1;
  float2 uv0 : TEXCOORD2;
  float2 uv1 : TEXCOORD3;
  float4 col : COLOR;
  nointerpolation uint face : TEXCOORD4;
};
// Blend shapes then skinning, done per vertex so playback never re-uploads
// geometry. Skinned vertices leave in world space (the palette includes it).
void Deform(VSIn i, uint vid, out float4 wp, out float3 wn) {
  float3 p = i.pos;
  for (uint t = 0; t < morph_targets; ++t) {
    uint b = (vid * morph_targets + t) * 3;
    p += morph_weights[t] *
         float3(morph_deltas[b], morph_deltas[b + 1], morph_deltas[b + 2]);
  }
  if (skinned != 0) {
    uint2 raw = joint_indices.Load2(vid * 8);
    uint4 ji = uint4(raw.x & 0xffff, raw.x >> 16, raw.y & 0xffff, raw.y >> 16);
    float4 jw = joint_weights[vid];
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float3 accn = float3(0.0, 0.0, 0.0);
    float sum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
      float w = jw[k];
      if (w <= 0.0) continue;
      float4x4 m = skin_mats[ji[k]];
      acc += w * mul(m, float4(p, 1.0));
      accn += w * mul(m, float4(i.nrm, 0.0)).xyz;
      sum += w;
    }
    if (sum > 0.0) {
      wp = acc;
      wn = accn / sum;
      return;
    }
  }
  wp = mul(world, float4(p, 1.0));
  wn = mul((float3x3)world, i.nrm);
}

VSOut VSMain(VSIn i, uint vid : SV_VertexID) {
  VSOut o;
  float4 wp;
  float3 wn;
  Deform(i, vid, wp, wn);
  o.pos = mul(view_proj, wp);
  o.wpos = wp.xyz;
  o.nrm = wn;
  o.uv0 = i.uv0;
  o.uv1 = i.uv1;
  o.col = i.col;
  o.face = has_face_ids != 0 ? face_ids[vid] : 0;
  return o;
}
struct ShadowOut {
  float4 pos : SV_Position;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
  float4 col : COLOR;
};
ShadowOut ShadowVS(VSIn i, uint vid : SV_VertexID) {
  float4 wp;
  float3 wn;
  Deform(i, vid, wp, wn);
  ShadowOut o;
  o.pos = mul(shadow_vp, wp);
  o.uv0 = i.uv0;
  o.uv1 = i.uv1;
  o.col = i.col;
  return o;
}

static const float PI = 3.14159265;

float3 LinearToSRGB(float3 c) {
  float3 v = saturate(c);
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
// Khronos PBR Neutral tone mapping.
float3 Tonemap(float3 color, float stops) {
  color *= exp2(stops);
  color = max(color, 0.0);
  const float start = 0.8 - 0.04;
  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;
  float peak = max(color.r, max(color.g, color.b));
  if (peak < start) return color;
  float d = 1.0 - start;
  float new_peak = 1.0 - d * d / (peak + d - start);
  color *= new_peak / peak;
  float g = 1.0 - 1.0 / (0.15 * (peak - new_peak) + 1.0);
  return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

float2 SlotUV(Slot s, float2 uv0, float2 uv1) {
  return ((s.flags >> 12) & 15) == 1 ? uv1 : uv0;
}
int SlotKind(Slot s) { return (s.flags >> 16) & 15; }
// USD st: scale, rotate, offset, then v = 0 at the image bottom. UDIM keeps the
// unflipped coordinate (its integer part picks the tile; the flip happens
// inside the tile) and Ptex uses the raw per-face uv.
float2 SlotST(Slot s, float2 uv) {
  int kind = SlotKind(s);
  if (kind == 2) return uv;
  float2 st = uv * s.uv_os.zw;
  float sn, cs;
  sincos(s.rot, sn, cs);
  st = float2(st.x * cs - st.y * sn, st.x * sn + st.y * cs);
  st += s.uv_os.xy;
  if (kind == 0) st.y = 1.0 - st.y;
  return st;
}
float4 SampleSlot(Slot s, float2 st, float2 gx, float2 gy, uint face) {
  int wrap = s.flags & 15;
  int kind = SlotKind(s);
  const float4 missing = float4(0.5, 0.0, 0.5, 1.0);
  if (kind == 1) {  // UDIM: tile from the integer part, texel from the fraction
    int u = clamp((int)floor(st.x), 0, 9);
    int v = clamp((int)floor(st.y), 0, 9);
    int slice = udim_lut[s.aux + v * 10 + u];
    if (slice < 0) return missing;
    float2 local = frac(st);
    local.y = 1.0 - local.y;  // flip inside the tile, not the tile row
    float2 lgx = float2(gx.x, -gx.y), lgy = float2(gy.x, -gy.y);
    Texture2DArray arr = g_array[NonUniformResourceIndex(s.index)];
    float4 value = wrap == 0 ? arr.SampleGrad(s_wrap, float3(local, slice), lgx, lgy)
                             : arr.SampleGrad(s_clamp, float3(local, slice), lgx, lgy);
    return value * s.scale + s.bias;
  }
  if (kind == 2) {  // Ptex: this face's rectangle inside the atlas
    uint count = (uint)ptex_rects[s.aux].x;
    if (face >= count) return missing;
    float4 rect = ptex_rects[s.aux + 1 + face];
    if (rect.z <= 0.0 || rect.w <= 0.0) return missing;  // face not decoded
    float2 atlas_uv = rect.xy + saturate(st) * rect.zw;
    Texture2D atlas = g_tex[NonUniformResourceIndex(s.index)];
    float4 value = atlas.SampleGrad(s_clamp, atlas_uv, gx * rect.zw, gy * rect.zw);
    return value * s.scale + s.bias;
  }
  Texture2D t = g_tex[NonUniformResourceIndex(s.index)];
  float4 v;
  if (wrap == 1)      v = t.SampleGrad(s_clamp, st, gx, gy);
  else if (wrap == 2) v = t.SampleGrad(s_mirror, st, gx, gy);
  else if (wrap == 3) v = t.SampleGrad(s_border, st, gx, gy);
  else                v = t.SampleGrad(s_wrap, st, gx, gy);
  return v * s.scale + s.bias;
}
float Channel(float4 v, int flags) {
  int c = (flags >> 8) & 15;
  return c == 1 ? v.g : c == 2 ? v.b : c == 3 ? v.a : v.r;
}

float3 ApplyColorMatrix(float3 rgb) {
  return float3(dot(cm0.xyz, rgb), dot(cm1.xyz, rgb), dot(cm2.xyz, rgb));
}

)hlsl"
R"hlsl(// ---- Environment ----------------------------------------------------------
float3 StudioEnvironment(float3 dir) {
  float up = saturate(dir.y * 0.5 + 0.5);
  float3 sky = lerp(float3(0.10, 0.11, 0.13), float3(0.55, 0.60, 0.68), up);
  float3 ground = float3(0.14, 0.13, 0.12);
  float3 c = dir.y >= 0.0 ? sky : lerp(ground, sky, saturate(1.0 + dir.y * 4.0));
  float key = pow(saturate(dot(dir, normalize(float3(0.5, 0.7, 0.5)))), 32.0);
  c += float3(3.2, 3.0, 2.8) * key;
  float rim = pow(saturate(dot(dir, normalize(float3(-0.6, 0.2, -0.7)))), 16.0);
  c += float3(1.0, 1.2, 1.6) * rim * 0.8;
  return c;
}
float3 EnvironmentRadiance(float3 dir, float roughness) {
  if (idx.y >= 0) {
    float u = atan2(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = 1.0 - (asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5);
    float lod = clamp(roughness * env_params.y, 0.0, 5.0);
    return g_tex[idx.y].SampleLevel(s_wrap, float2(u, v), lod).rgb *
           env_params.x;
  }
  return StudioEnvironment(dir) * env_params.x;
}
// Cosine-convolved SH9 irradiance (coefficients pre-convolved on the CPU).
float3 ShIrradiance(float3 n) {
  float x = n.x, y = n.y, z = n.z;
  float3 r = sh[0].xyz * 0.282095;
  r += sh[1].xyz * (0.488603 * y);
  r += sh[2].xyz * (0.488603 * z);
  r += sh[3].xyz * (0.488603 * x);
  r += sh[4].xyz * (1.092548 * x * y);
  r += sh[5].xyz * (1.092548 * y * z);
  r += sh[6].xyz * (0.315392 * (3.0 * z * z - 1.0));
  r += sh[7].xyz * (1.092548 * x * z);
  r += sh[8].xyz * (0.546274 * (x * x - y * y));
  return max(r, 0.0);
}

// ---- BRDF -----------------------------------------------------------------
float GgxDistribution(float n_dot_h, float roughness) {
  float a = max(roughness * roughness, 1e-3);
  float a2 = a * a;
  float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
  return a2 / max(PI * d * d, 1e-6);
}
float SmithG1(float n_dot_x, float roughness) {
  float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  return n_dot_x / max(n_dot_x * (1.0 - k) + k, 1e-5);
}
float3 FresnelSchlick(float cos_theta, float3 f0) {
  return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}
float3 EvalSpecular(float3 n, float3 v, float3 l, float3 f0, float roughness) {
  float n_dot_l = saturate(dot(n, l));
  float n_dot_v = max(saturate(dot(n, v)), 1e-4);
  if (n_dot_l <= 0.0) return float3(0.0, 0.0, 0.0);
  float3 h = normalize(v + l);
  float n_dot_h = saturate(dot(n, h));
  float v_dot_h = saturate(dot(v, h));
  float d = GgxDistribution(n_dot_h, roughness);
  float g = SmithG1(n_dot_l, roughness) * SmithG1(n_dot_v, roughness);
  float3 f = FresnelSchlick(v_dot_h, f0);
  return d * g * f / max(4.0 * n_dot_l * n_dot_v, 1e-5) * n_dot_l;
}

float ShadowFactor(float3 wpos, float3 n, float3 to_light) {
  float4 clip = mul(shadow_vp, float4(wpos, 1.0));
  float3 ndc = clip.xyz / clip.w;
  float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
  if (uv.x <= 0.001 || uv.x >= 0.999 || uv.y <= 0.001 || uv.y >= 0.999 ||
      ndc.z <= 0.0 || ndc.z >= 1.0)
    return 1.0;
  float texel = shadow_params.x;
  float bias = shadow_params.y + 0.004 * (1.0 - saturate(dot(n, to_light)));
  float acc = 0.0;
  [unroll] for (int oy = -1; oy <= 1; ++oy) {
    [unroll] for (int ox = -1; ox <= 1; ++ox) {
      acc += g_shadow.SampleCmpLevelZero(
          s_shadow, uv + float2(ox, oy) * texel, ndc.z - bias);
    }
  }
  return lerp(1.0, acc / 9.0, shadow_params.z);
}

)hlsl"
R"hlsl(// ---- OpenPBR-lite extras ---------------------------------------------------
// Airy reflectance of a thin dielectric film, as a scale on the base Fresnel
// (550 nm reference wavelength keeps it stable without a spectral fit).
float3 ThinFilmScale(float cos_theta, float ior, float thickness_nm, float3 f0) {
  if (thickness_nm <= 0.0) return float3(1.0, 1.0, 1.0);
  float cos_i = clamp(cos_theta, 1e-3, 1.0);
  float sin_i2 = max(0.0, 1.0 - cos_i * cos_i);
  float eta = max(ior, 1.0001);
  float cos_t = sqrt(max(0.0, 1.0 - sin_i2 / (eta * eta)));
  float r01 = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
  float r12 = (eta * cos_t - cos_i) / (eta * cos_t + cos_i);
  float phase = 4.0 * PI * eta * thickness_nm / 550.0 * cos_t;
  float c = cos(phase);
  float denom = 1.0 + r01 * r01 * r12 * r12 + 2.0 * r01 * r12 * c;
  float refl = (r01 * r01 + r12 * r12 + 2.0 * r01 * r12 * c) / max(denom, 1e-4);
  float base = max(FresnelSchlick(cos_theta, f0).x, 1e-4);
  float scale = clamp(refl / base, 0.0, 6.0);
  return float3(scale, scale, scale);
}

float GgxAniso(float n_dot_h, float t_dot_h, float b_dot_h, float at, float ab) {
  float a2 = at * ab;
  float3 v = float3(ab * t_dot_h, at * b_dot_h, a2 * n_dot_h);
  float d = max(dot(v, v), 1e-6);
  return a2 / max(PI * d * d, 1e-6);
}
float3 EvalSpecularAniso(float3 n, float3 v, float3 l, float3 f0, float roughness,
                         float anisotropy, float3 t, float3 b) {
  float n_dot_l = saturate(dot(n, l));
  float n_dot_v = max(saturate(dot(n, v)), 1e-4);
  if (n_dot_l <= 0.0) return float3(0.0, 0.0, 0.0);
  float3 h = normalize(v + l);
  float n_dot_h = saturate(dot(n, h));
  float v_dot_h = saturate(dot(v, h));
  float at = max(0.002, roughness * (1.0 + anisotropy));
  float ab = max(0.002, roughness * (1.0 - anisotropy));
  float d = GgxAniso(n_dot_h, dot(t, h), dot(b, h), at, ab);
  float g = SmithG1(n_dot_l, roughness) * SmithG1(n_dot_v, roughness);
  float3 f = FresnelSchlick(v_dot_h, f0);
  return d * g * f / max(4.0 * n_dot_l * n_dot_v, 1e-5) * n_dot_l;
}

// Irradiance-like term of one light at `wpos` for normal `n` (includes N.L).
float3 LightRadiance(Light light, float3 wpos, float3 n, out float3 to_light) {
  int type = int(light.position_type.w + 0.5);
  to_light = light.direction.xyz;
  float attenuation = 1.0;
  if (type != 1) {
    float3 delta = light.position_type.xyz - wpos;
    float dist2 = max(dot(delta, delta), 1e-6);
    to_light = delta * rsqrt(dist2);
    attenuation = 1.0 / dist2;
    if (type == 2) {
      float cos_outer = light.color_intensity.w;
      float cos_inner = light.params.y;
      float c = dot(-to_light, light.direction.xyz);
      attenuation *= smoothstep(cos_outer, max(cos_inner, cos_outer + 1e-4), c);
    }
  }
  float n_dot_l = saturate(dot(n, to_light));
  if (n_dot_l <= 0.0) return float3(0.0, 0.0, 0.0);
  return light.color_intensity.rgb * attenuation * n_dot_l;
}

)hlsl"
R"hlsl(float4 PSMain(VSOut i) : SV_Target {
  Material m = materials[material_index];

  // Gradients of every slot's texture coordinate, in uniform control flow.
  float2 st[7], gx[7], gy[7];
  [unroll] for (int k = 0; k < 7; ++k) {
    st[k] = SlotST(m.slots[k], SlotUV(m.slots[k], i.uv0, i.uv1));
    gx[k] = ddx(st[k]);
    gy[k] = ddy(st[k]);
  }
  float4 tv[7];
  [unroll] for (int k2 = 0; k2 < 7; ++k2) {
    tv[k2] = m.slots[k2].index >= 0
                 ? SampleSlot(m.slots[k2], st[k2], gx[k2], gy[k2], i.face)
                 : float4(1.0, 1.0, 1.0, 1.0);
  }

  float3 N = normalize(i.nrm);
  float3 V = normalize(eye.xyz - i.wpos);

  // Tangent frame from screen-space derivatives (no stored tangents): used for
  // normal maps, the coat normal map and anisotropic highlights.
  float2 uvn = SlotUV(m.slots[3], i.uv0, i.uv1);
  float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
  float2 duv1 = ddx(uvn), duv2 = ddy(uvn);
  float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
  float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
  float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
  float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
  T *= invmax;
  B *= invmax;

  if (m.slots[3].index >= 0) {
    float3 ts = float3((tv[3].xy * 2.0 - 1.0) * m.extra.x, tv[3].z * 2.0 - 1.0);
    if (dot(ts, ts) > 1e-6) N = normalize(T * ts.x + B * ts.y + N * ts.z);
  }
  if (dot(N, V) < 0.0) N = -N;  // preview: shade both sides

  // Orthonormal tangent/bitangent around the final normal.
  float3 tangent = T - N * dot(N, T);
  if (!all(isfinite(tangent)) || dot(tangent, tangent) < 1e-12)
    tangent = cross(N, abs(N.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0));
  tangent = normalize(tangent);
  float3 bitangent = cross(N, tangent);

  float alpha = m.extra.y * i.col.a;
  if (m.slots[5].index >= 0) alpha *= Channel(tv[5], m.slots[5].flags);
  if (m.params.z == 1.0 && alpha < m.params.w) discard;

  // A connected texture replaces the constant colour (the constant is only the
  // fallback: UsdPreviewSurface leaves its 0.18 default in the packed material
  // when diffuseColor is textured); weight and vertex colour still apply.
  float3 base_color = (m.slots[0].index >= 0 ? tv[0].rgb : m.base.rgb) * i.col.rgb;
  base_color *= m.base.a;
  float metal = m.params.x;
  if (m.slots[1].index >= 0) metal = Channel(tv[1], m.slots[1].flags);
  float rough = m.params.y;
  if (m.slots[2].index >= 0) rough = Channel(tv[2], m.slots[2].flags);
  metal = saturate(metal);
  rough = clamp(rough, 0.02, 1.0);
  float3 emission = (m.slots[4].index >= 0 ? tv[4].rgb : m.emission.rgb) * m.emission.w;

  float n_dot_v = max(saturate(dot(N, V)), 1e-4);
  float3 f0 = lerp(float3(0.04, 0.04, 0.04) * m.specular.w, base_color, metal) *
              m.specular.rgb;
  if (m.thin.x > 0.0) f0 *= ThinFilmScale(n_dot_v, m.thin.z, m.thin.y, f0);
  // A tinted coat darkens what is underneath it.
  base_color *= lerp(float3(1.0, 1.0, 1.0), m.coat.rgb,
                     saturate(m.coat.w * m.misc.z));
  float3 diffuse_color = base_color * (1.0 - metal) * (1.0 - m.transmission.w);
  bool aniso = abs(m.thin.w) > 0.01;

  float3 color = float3(0.0, 0.0, 0.0);
  for (int li = 0; li < idx.x; ++li) {
    Light light = lights[li];
    float3 to_light;
    float3 radiance = LightRadiance(light, i.wpos, N, to_light);
    if (radiance.r + radiance.g + radiance.b <= 0.0) continue;
    float shadow = 1.0;
    if (idx.z >= 0 && li == idx.w && int(light.position_type.w + 0.5) == 1)
      shadow = ShadowFactor(i.wpos, N, to_light);
    float3 spec = aniso ? EvalSpecularAniso(N, V, to_light, f0, rough, m.thin.w,
                                            tangent, bitangent)
                        : EvalSpecular(N, V, to_light, f0, rough);
    color += (diffuse_color / PI + spec) * radiance * shadow;
  }

  // Image-based lighting: SH diffuse + environment specular.
  float3 irradiance = idx.y >= 0 ? ShIrradiance(N) : float3(0.28, 0.29, 0.32);
  color += diffuse_color * irradiance * 0.5;
  float3 refl = reflect(-V, N);
  float3 env_spec = ApplyColorMatrix(EnvironmentRadiance(refl, rough));
  color += env_spec *
           (f0 + (max(float3(1.0, 1.0, 1.0) - rough, f0) - f0) *
                     pow(1.0 - n_dot_v, 5.0)) *
           0.6;

  // Coat: a second, smoother dielectric lobe with its own normal.
  if (m.coat.w > 0.0) {
    float3 cn = N;
    if (m.slots[6].index >= 0) {
      float3 cs = normalize(float3((tv[6].xy * 2.0 - 1.0) * m.extra.z,
                                   tv[6].z * 2.0 - 1.0));
      cn = normalize(tangent * cs.x + bitangent * cs.y + N * cs.z);
    }
    float3 coat_f0 = float3(0.04, 0.04, 0.04) * m.coat.w * m.coat.rgb;
    float coat_rough = clamp(m.coat_params.x, 0.02, 1.0);
    for (int ci = 0; ci < idx.x; ++ci) {
      float3 to_light;
      float3 radiance = LightRadiance(lights[ci], i.wpos, cn, to_light);
      color += EvalSpecular(cn, V, to_light, coat_f0, coat_rough) * radiance;
    }
    float3 coat_env = EnvironmentRadiance(reflect(-V, cn), coat_rough);
    color += ApplyColorMatrix(coat_env) * coat_f0 * pow(1.0 - n_dot_v, 3.0);
  }

  // Sheen: grazing-angle fabric response.
  if (m.sheen.w > 0.0) {
    float sheen_rough = clamp(m.coat_params.z, 0.05, 1.0);
    float sheen = pow(1.0 - n_dot_v, 3.0) * m.sheen.w;
    color += m.sheen.rgb * sheen * (0.25 + 0.75 * irradiance) / max(sheen_rough, 0.1);
  }

  // Subsurface: wrapped diffuse plus a view-dependent translucency term.
  if (m.sss.w > 0.0) {
    float3 sss = float3(0.0, 0.0, 0.0);
    for (int si = 0; si < idx.x; ++si) {
      Light light = lights[si];
      int type = int(light.position_type.w + 0.5);
      float3 to_light = light.direction.xyz;
      if (type != 1) to_light = normalize(light.position_type.xyz - i.wpos);
      float wrap = saturate((dot(N, to_light) + 0.5) / 1.5);
      float forward = pow(saturate(dot(V, -to_light)), 4.0);
      sss += light.color_intensity.rgb * (wrap * 0.5 + forward * 0.25);
    }
    color += m.sss.rgb * m.sss.w * sss * 0.5;
  }

  // Transmission: refracted environment plus tint; no volumetric absorption.
  if (m.transmission.w > 0.0) {
    float eta = 1.0 / max(m.extra.w, 1.0001);
    float3 refracted = refract(-V, N, eta);
    bool tir = dot(refracted, refracted) < 1e-6;
    float3 dir = tir ? refl : normalize(refracted);
    float3 refr = EnvironmentRadiance(dir, rough);
    color += m.transmission.rgb * m.transmission.w * ApplyColorMatrix(refr) *
             (1.0 - saturate(m.misc.x * 0.1));
    color += m.transmission.rgb * m.transmission.w * irradiance * 0.15;
  }

  color += emission;
  color = Tonemap(color, tone.x);
  return float4(LinearToSRGB(color), m.params.z == 2.0 ? saturate(alpha) : 1.0);
}

// Alpha-tested shadow caster: masked materials only cast where they are opaque.
void ShadowMaskPS(ShadowOut i) {
  Material m = materials[material_index];
  Slot s = m.slots[5];
  float alpha = m.extra.y * i.col.a;
  float2 st = SlotST(s, SlotUV(s, i.uv0, i.uv1));
  float2 gx = ddx(st), gy = ddy(st);
  if (s.index >= 0) alpha *= Channel(SampleSlot(s, st, gx, gy, 0), s.flags);
  if (alpha < m.params.w) discard;
}
)hlsl";

bool Fail(std::string* err, const char* what, HRESULT hr,
          ID3DBlob* messages = nullptr) {
  if (err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    *err = buf;
    if (messages && messages->GetBufferSize()) {
      err->append(": ");
      err->append(static_cast<const char*>(messages->GetBufferPointer()),
                  messages->GetBufferSize());
    }
  }
  return false;
}

#ifndef D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES
#define D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES (1 << 20)
#endif

bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>* out,
             std::string* err) {
  ComPtr<ID3DBlob> messages;
  const HRESULT hr =
      D3DCompile(kShader, sizeof(kShader) - 1, "mesh.hlsl", nullptr, nullptr,
                 entry, target,
                 D3DCOMPILE_ENABLE_STRICTNESS |
                     D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES,
                 0, out->GetAddressOf(), messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3DCompile", hr, messages.Get());
  return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

}  // namespace

bool MeshRenderer::Init(ID3D12Device* device, std::string* err) {
  ComPtr<ID3DBlob> vs, ps, shadow_vs, shadow_mask_ps;
  if (!Compile("VSMain", "vs_5_1", &vs, err)) return false;
  if (!Compile("PSMain", "ps_5_1", &ps, err)) return false;
  if (!Compile("ShadowVS", "vs_5_1", &shadow_vs, err)) return false;
  if (!Compile("ShadowMaskPS", "ps_5_1", &shadow_mask_ps, err)) return false;

  D3D12_DESCRIPTOR_RANGE tex_range = {};
  tex_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  tex_range.NumDescriptors = UINT_MAX;  // unbounded: needs binding tier 2+
  tex_range.BaseShaderRegister = 0;
  tex_range.RegisterSpace = 1;
  tex_range.OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_DESCRIPTOR_RANGE shadow_range = {};
  shadow_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  shadow_range.NumDescriptors = 1;
  shadow_range.BaseShaderRegister = 0;
  shadow_range.RegisterSpace = 2;
  shadow_range.OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[15] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // Frame
  params[0].Descriptor.ShaderRegister = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // Draw
  params[1].Constants.ShaderRegister = 1;
  params[1].Constants.Num32BitValues = 20;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // materials
  params[2].Descriptor.ShaderRegister = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // lights
  params[3].Descriptor.ShaderRegister = 1;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[4].DescriptorTable.NumDescriptorRanges = 1;
  params[4].DescriptorTable.pDescriptorRanges = &tex_range;
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[5].DescriptorTable.NumDescriptorRanges = 1;
  params[5].DescriptorTable.pDescriptorRanges = &shadow_range;
  params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // t2..t6: skin palette, morph deltas, morph weights, joint indices/weights.
  for (UINT k = 0; k < 5; ++k) {
    params[6 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6 + k].Descriptor.ShaderRegister = 2 + k;
    params[6 + k].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }

  // Texture2DArray table (UDIM tiles) over the same heap, then the UDIM tile
  // LUT, the Ptex rect table and the per-vertex Ptex face ids.
  D3D12_DESCRIPTOR_RANGE array_range = tex_range;
  array_range.RegisterSpace = 3;
  params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[11].DescriptorTable.NumDescriptorRanges = 1;
  params[11].DescriptorTable.pDescriptorRanges = &array_range;
  params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // udim_lut
  params[12].Descriptor.ShaderRegister = 7;
  params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // ptex_rects
  params[13].Descriptor.ShaderRegister = 8;
  params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // face_ids
  params[14].Descriptor.ShaderRegister = 9;
  params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_STATIC_SAMPLER_DESC samplers[5] = {};
  const D3D12_TEXTURE_ADDRESS_MODE modes[4] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
  for (UINT i = 0; i < 4; ++i) {
    samplers[i].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
        modes[i];
    samplers[i].MaxAnisotropy = 8;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  samplers[4].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[4].AddressU = samplers[4].AddressV = samplers[4].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[4].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[4].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[4].ShaderRegister = 4;
  samplers[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsd = {};
  rsd.NumParameters = 15;
  rsd.pParameters = params;
  rsd.NumStaticSamplers = 5;
  rsd.pStaticSamplers = samplers;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, messages;
  HRESULT hr = D3D12SerializeRootSignature(
      &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.GetAddressOf(),
      messages.GetAddressOf());
  if (FAILED(hr))
    return Fail(err, "D3D12SerializeRootSignature", hr, messages.Get());
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                   blob->GetBufferSize(),
                                   IID_PPV_ARGS(&root_signature_));
  if (FAILED(hr)) return Fail(err, "CreateRootSignature", hr);

  static_assert(sizeof(lmv_vertex) == 56, "lmv_vertex layout changed");
  const D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = root_signature_.Get();
  pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.DepthStencilState.DepthEnable = TRUE;
  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pd.InputLayout = {layout, 5};
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = kBackbufferFormat;
  pd.DSVFormat = kDepthFormat;
  pd.SampleDesc.Count = 1;
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_solid_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(solid)", hr);
  {
    // Blend-mode materials: source-over, depth tested but not written.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC bd = pd;
    D3D12_RENDER_TARGET_BLEND_DESC& rt = bd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    hr = device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&pso_blend_));
    if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(blend)", hr);
  }
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_wire_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(wire)", hr);

  // Depth-only shadow pipeline: no pixel shader, no colour target.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC sd = pd;
  sd.VS = {shadow_vs->GetBufferPointer(), shadow_vs->GetBufferSize()};
  sd.PS = {};
  sd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  sd.RasterizerState.SlopeScaledDepthBias = 1.5f;
  sd.NumRenderTargets = 0;
  sd.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
  hr = device->CreateGraphicsPipelineState(&sd, IID_PPV_ARGS(&pso_shadow_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(shadow)", hr);
  sd.PS = {shadow_mask_ps->GetBufferPointer(), shadow_mask_ps->GetBufferSize()};
  hr = device->CreateGraphicsPipelineState(&sd, IID_PPV_ARGS(&pso_shadow_mask_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(shadow mask)", hr);

  // Per-frame constants: one persistently mapped upload slot per swapchain
  // buffer (BeginFrame has already waited for the slot's previous use).
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = UINT64(kFrameCbStride) * kFrameCount;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr, IID_PPV_ARGS(&frame_cb_));
  if (FAILED(hr)) return Fail(err, "CreateCommittedResource(frame cb)", hr);
  {
    D3D12_RESOURCE_DESC dd = rd;
    dd.Width = 256;
    hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&dummy_));
    if (FAILED(hr)) return Fail(err, "CreateCommittedResource(dummy)", hr);
  }
  const D3D12_RANGE none = {0, 0};
  void* mapped = nullptr;
  hr = frame_cb_->Map(0, &none, &mapped);
  if (FAILED(hr)) return Fail(err, "Map(frame cb)", hr);
  frame_cb_mapped_ = static_cast<uint8_t*>(mapped);
  return true;
}

// Binds the per-mesh buffers a range needs and issues its draw.
void MeshRenderer::Emit(ID3D12GraphicsCommandList* list, const SceneGPU& gpu,
                        UINT slot, const DrawItem& item, int32_t* bound_mesh) {
  const MeshGPU& mesh = gpu.meshes[static_cast<size_t>(item.mesh_id)];
  if (mesh.face_ids)
    list->SetGraphicsRootShaderResourceView(
        14, mesh.face_ids->GetGPUVirtualAddress());
  if (mesh.deforms()) {
    const D3D12_GPU_VIRTUAL_ADDRESS ring = gpu.deform_va(slot);
    if (mesh.skinned) {
      list->SetGraphicsRootShaderResourceView(6, ring + mesh.palette_offset);
      list->SetGraphicsRootShaderResourceView(
          9, mesh.joint_indices->GetGPUVirtualAddress());
      list->SetGraphicsRootShaderResourceView(
          10, mesh.joint_weights->GetGPUVirtualAddress());
    }
    if (mesh.morph_targets) {
      list->SetGraphicsRootShaderResourceView(
          7, mesh.morph_deltas->GetGPUVirtualAddress());
      list->SetGraphicsRootShaderResourceView(8, ring + mesh.weights_offset);
    }
  }
  if (*bound_mesh != item.mesh_id) {
    list->IASetVertexBuffers(0, 1, &mesh.vbv);
    list->IASetIndexBuffer(&mesh.ibv);
    *bound_mesh = item.mesh_id;
  }
  uint32_t constants[20];
  std::memcpy(constants, item.world, 16 * sizeof(float));
  constants[16] = item.material;
  constants[17] = mesh.skinned ? 1u : 0u;
  constants[18] = mesh.morph_targets;
  constants[19] = mesh.face_ids ? 1u : 0u;
  list->SetGraphicsRoot32BitConstants(1, 20, constants, 0);
  list->DrawIndexedInstanced(item.count, 1, item.first, 0, 0);
}

void MeshRenderer::DrawMeshes(ID3D12GraphicsCommandList* list,
                              const lmv_scene* scene, const SceneGPU& gpu,
                              UINT slot, Pass pass, const float eye[3]) {
  const D3D12_GPU_VIRTUAL_ADDRESS dummy = dummy_->GetGPUVirtualAddress();
  for (UINT k = 0; k < 5; ++k)
    list->SetGraphicsRootShaderResourceView(6 + k, dummy);
  list->SetGraphicsRootShaderResourceView(14, dummy);  // face ids
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Split every drawable range into opaque/masked and blend-mode lists. Only
  // the lit main pass blends: wireframe draws everything as lines, and blended
  // surfaces do not cast shadows.
  opaque_items_.clear();
  mask_items_.clear();
  blend_items_.clear();
  const int32_t scene_materials = static_cast<int32_t>(gpu.materials.size()) - 1;
  const uint32_t draw_count = lmv_scene_draw_count(scene);
  for (uint32_t d = 0; d < draw_count; ++d) {
    lmv_draw_view dv = {};
    if (lmv_scene_draw(scene, d, &dv) != LMV_OK || !dv.visible || !dv.world)
      continue;
    if (dv.mesh_id < 0 || static_cast<size_t>(dv.mesh_id) >= gpu.meshes.size())
      continue;
    const MeshGPU& mesh = gpu.meshes[static_cast<size_t>(dv.mesh_id)];
    if (!mesh.valid()) continue;

    // Eye distance to the transformed bounding-box centre (sort key).
    float depth = 0.0f;
    {
      const float c[3] = {0.5f * (mesh.bbox_min[0] + mesh.bbox_max[0]),
                          0.5f * (mesh.bbox_min[1] + mesh.bbox_max[1]),
                          0.5f * (mesh.bbox_min[2] + mesh.bbox_max[2])};
      const float* w = dv.world;
      float p[3];
      for (int i = 0; i < 3; ++i)
        p[i] = mesh.skinned ? c[i]  // skinned vertices are already in world space
                            : w[i] * c[0] + w[4 + i] * c[1] + w[8 + i] * c[2] + w[12 + i];
      for (int i = 0; i < 3; ++i) depth += (p[i] - eye[i]) * (p[i] - eye[i]);
    }

    auto add_range = [&](uint32_t first, uint32_t count, int32_t mat_id) {
      uint32_t material = gpu.default_material();
      if (dv.material_override >= 0 && dv.material_override < scene_materials)
        material = static_cast<uint32_t>(dv.material_override);
      else if (mat_id >= 0 && mat_id < scene_materials)
        material = static_cast<uint32_t>(mat_id);
      const bool blend = gpu.materials[material].params[2] == 2.0f;
      if (blend && pass == Pass::Shadow) return;
      DrawItem item;
      item.mesh_id = dv.mesh_id;
      item.world = dv.world;
      item.first = first;
      item.count = count;
      item.material = material;
      item.depth = depth;
      const bool mask = gpu.materials[material].params[2] == 1.0f;
      if (blend && pass == Pass::Main)
        blend_items_.push_back(item);
      else if (mask && pass == Pass::Shadow)
        mask_items_.push_back(item);  // alpha-tested in the shadow map
      else
        opaque_items_.push_back(item);
    };
    if (mesh.subsets.empty()) {
      add_range(0, mesh.index_count, mesh.material_id);
    } else {
      for (const lmv_subset& s : mesh.subsets)
        add_range(s.first_index, s.index_count,
                  s.material_id >= 0 ? s.material_id : mesh.material_id);
    }
  }

  int32_t bound_mesh = -1;
  list->SetPipelineState(pass == Pass::Shadow ? pso_shadow_.Get()
                         : pass == Pass::Wire   ? pso_wire_.Get()
                                                : pso_solid_.Get());
  for (const DrawItem& item : opaque_items_) Emit(list, gpu, slot, item, &bound_mesh);

  if (!mask_items_.empty()) {
    list->SetPipelineState(pso_shadow_mask_.Get());
    for (const DrawItem& item : mask_items_) Emit(list, gpu, slot, item, &bound_mesh);
  }

  // Blended ranges last, farthest first, over the opaque image.
  if (!blend_items_.empty()) {
    std::stable_sort(blend_items_.begin(), blend_items_.end(),
                     [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; });
    list->SetPipelineState(pso_blend_.Get());
    for (const DrawItem& item : blend_items_) Emit(list, gpu, slot, item, &bound_mesh);
  }
}

void MeshRenderer::Draw(Device& device, ID3D12GraphicsCommandList* list,
                        const FrameParams& frame, const lmv_scene* scene,
                        const SceneGPU& gpu) {
  const bool shadows = frame.shadows && gpu.shadow_light >= 0 && gpu.shadow_map;

  FrameCB cb = {};
  std::memcpy(cb.view_proj, frame.view_proj, sizeof(cb.view_proj));
  std::memcpy(cb.eye, frame.eye, sizeof(frame.eye));
  cb.idx[0] = static_cast<int32_t>(gpu.lights.size());
  // Specular reads the GGX-prefiltered chain (mip = roughness * 5) when it
  // exists, else the raw image with a blur-by-mip approximation.
  cb.idx[1] = gpu.env_spec_index >= 0 ? gpu.env_spec_index : gpu.env_index;
  cb.env_params[1] = gpu.env_spec_index >= 0 ? 5.0f : 6.0f;
  cb.idx[2] = shadows ? gpu.shadow_srv_index : -1;
  cb.idx[3] = gpu.shadow_light;
  cb.shadow_params[0] = 1.0f / static_cast<float>(SceneGPU::kShadowSize);
  cb.shadow_params[1] = 0.0015f;
  cb.shadow_params[2] = 0.65f;
  cb.env_params[0] = 1.0f;
  cb.tone[0] = frame.exposure_stops;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) cb.cm[r][c] = gpu.color_matrix[r * 3 + c];
  std::memcpy(cb.sh, gpu.sh, sizeof(cb.sh));
  if (shadows) {
    const LightGPU& l = gpu.lights[static_cast<size_t>(gpu.shadow_light)];
    DirectionalShadowMatrix(l.direction, gpu.bounds_min, gpu.bounds_max,
                            cb.shadow_vp);
  }
  const UINT slot = device.frame_index();
  std::memcpy(frame_cb_mapped_ + size_t(slot) * kFrameCbStride, &cb, sizeof(cb));

  ID3D12DescriptorHeap* heaps[] = {gpu.srv_heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRootConstantBufferView(
      0, frame_cb_->GetGPUVirtualAddress() + UINT64(slot) * kFrameCbStride);
  list->SetGraphicsRootShaderResourceView(
      2, gpu.material_buffer->GetGPUVirtualAddress());
  list->SetGraphicsRootShaderResourceView(
      3, gpu.light_va(slot));
  const D3D12_GPU_DESCRIPTOR_HANDLE heap_start =
      gpu.srv_heap->GetGPUDescriptorHandleForHeapStart();
  list->SetGraphicsRootDescriptorTable(4, heap_start);
  list->SetGraphicsRootDescriptorTable(11, heap_start);  // UDIM arrays
  const D3D12_GPU_VIRTUAL_ADDRESS no_data = dummy_->GetGPUVirtualAddress();
  list->SetGraphicsRootShaderResourceView(
      12, gpu.udim_lut_buffer ? gpu.udim_lut_buffer->GetGPUVirtualAddress() : no_data);
  list->SetGraphicsRootShaderResourceView(
      13, gpu.ptex_rect_buffer ? gpu.ptex_rect_buffer->GetGPUVirtualAddress() : no_data);
  D3D12_GPU_DESCRIPTOR_HANDLE shadow_handle = heap_start;  // placeholder: white
  if (gpu.shadow_srv_index >= 0) {
    const UINT stride = device.device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    shadow_handle.ptr += UINT64(gpu.shadow_srv_index) * stride;
  }
  list->SetGraphicsRootDescriptorTable(5, shadow_handle);

  if (shadows) {
    const D3D12_RESOURCE_BARRIER to_depth =
        Transition(gpu.shadow_map.Get(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list->ResourceBarrier(1, &to_depth);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        gpu.shadow_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                nullptr);
    const float size = static_cast<float>(SceneGPU::kShadowSize);
    const D3D12_VIEWPORT vp = {0.0f, 0.0f, size, size, 0.0f, 1.0f};
    const D3D12_RECT sc = {0, 0, static_cast<LONG>(SceneGPU::kShadowSize),
                           static_cast<LONG>(SceneGPU::kShadowSize)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    DrawMeshes(list, scene, gpu, slot, Pass::Shadow, frame.eye);
    const D3D12_RESOURCE_BARRIER to_srv =
        Transition(gpu.shadow_map.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_srv);
    device.BindMainTargets(list);
  }

  DrawMeshes(list, scene, gpu, slot, frame.wireframe ? Pass::Wire : Pass::Main,
             frame.eye);
}

}  // namespace ld3d12
