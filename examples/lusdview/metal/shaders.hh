// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace lusdview {
// Compiled by the Metal driver at startup; no Xcode shader tools required.
constexpr const char* kMetalShaders = R"msl(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 p, n; packed_float2 uv; packed_float4 color; };
struct Instance { float4 x, y, z, tint; };
struct Uniforms {
  float4x4 view, proj;
  float4 camera, light, lightColor, base, emissive, options;
  float4 planes[8];
  int mode, planeCount, geometricNormal, padding;
};
struct Varying {
  float4 position [[position]];
  float3 p, n;
  float2 uv;
  float4 color;
};
vertex Varying sceneVertex(uint v [[vertex_id]], uint i [[instance_id]],
    device const Vertex* vertices [[buffer(0)]],
    constant Uniforms& u [[buffer(1)]], device const Instance* instances [[buffer(2)]]) {
  Vertex a = vertices[v]; Instance t = instances[i];
  float4 p = float4(float3(a.p), 1);
  float3x3 m = transpose(float3x3(t.x.xyz, t.y.xyz, t.z.xyz));
  // Cofactors preserve normals under nonuniform scale (and reflections).
  float3x3 cofactor = float3x3(cross(m[1],m[2]), cross(m[2],m[0]), cross(m[0],m[1]));
  Varying o; o.p = float3(dot(t.x,p), dot(t.y,p), dot(t.z,p));
  o.n = normalize(cofactor * float3(a.n)) * (determinant(m) < 0 ? -1.0f : 1.0f);
  o.position = u.proj * u.view * float4(o.p, 1);
  o.uv = float2(a.uv); o.color = float4(a.color) * t.tint;
  return o;
}
fragment float4 sceneFragment(Varying a [[stage_in]], bool front [[front_facing]],
    constant Uniforms& u [[buffer(1)]], texture2d<float> tex [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::repeat);
  for (int i=0; i<u.planeCount; ++i)
    if (dot(float4(a.p,1),u.planes[i]) < 0) discard_fragment();
  float4 base = u.base * a.color * tex.sample(s,a.uv);
  if (u.options.w > 0 && base.a < u.options.w) discard_fragment();
  float3 n = normalize(u.geometricNormal ? cross(dfdx(a.p),dfdy(a.p)) : a.n);
  if (!front) n = -n;
  float3 c = base.rgb;
  if (u.mode == 2 || u.mode == 4) c = n*.5+.5;
  else if (u.mode == 5) c = float3(fract(a.uv),0);
  else if (u.mode == 6) c = float3(length(a.p-u.camera.xyz)/max(u.camera.w,1e-6f));
  else if (u.mode == 8) c = front ? float3(.1,1,.1) : float3(1,.1,.1);
  else if (u.mode == 9) c = float3(u.options.y);
  else if (u.mode == 10) c = float3(u.options.x);
  else if (u.mode == 11) c = u.emissive.xyz;
  else if (u.mode == 12) c = float3(base.a);
  else if (u.mode == 0 || u.mode == 1) {
    float3 l=normalize(u.light.xyz), v=normalize(u.camera.xyz-a.p), h=normalize(l+v);
    float nl=max(dot(n,l),0.f), nv=max(dot(n,v),.001f), nh=max(dot(n,h),0.f);
    float r=max(u.options.y,.05f), alpha=r*r, aa=alpha*alpha;
    float d=aa/(M_PI_F*pow(nh*nh*(aa-1)+1,2));
    float k=(r+1)*(r+1)/8;
    float g=nl/(nl*(1-k)+k)*nv/(nv*(1-k)+k);
    float3 f0=mix(float3(.04),base.rgb,u.options.x);
    float3 f=f0+(1-f0)*pow(1-max(dot(h,v),0.f),5);
    c=base.rgb*.15 + ((1-f)*(1-u.options.x)*base.rgb/M_PI_F + d*g*f/max(4*nl*nv,.001f))*nl*u.lightColor.xyz*M_PI_F + u.emissive.xyz;
    c=1-exp(-max(c,0.f)*exp2(u.options.z));
    c=pow(c,float3(1/2.2f));
  }
  return float4(c,base.a);
}
struct GuiVertex { packed_float2 p, uv; uchar4 color; };
struct GuiOut { float4 position [[position]]; float2 uv; float4 color; };
vertex GuiOut guiVertex(uint v [[vertex_id]], device const GuiVertex* vertices [[buffer(0)]],
    constant float4& transform [[buffer(1)]]) {
  GuiVertex a=vertices[v]; GuiOut o;
  o.position=float4(float2(a.p)*transform.xy+transform.zw,0,1);
  o.uv=float2(a.uv); o.color=float4(a.color)/255.f; return o;
}
fragment float4 guiFragment(GuiOut a [[stage_in]], texture2d<float> tex [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return a.color*tex.sample(s,a.uv);
}
)msl";
}  // namespace lusdview
