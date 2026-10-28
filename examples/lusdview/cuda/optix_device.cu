#include "optix_launch_abi.hh"
// SPDX-License-Identifier: Apache-2.0
#include <optix_device.h>
#include <math.h>


struct LusdviewOptixLaunchParams {
  uchar4* output;
  OptixTraversableHandle traversable;
  const float* normals;
  const float* colors;
  const int* materials;
  const float* materialPbr;
  const float* materialBase;
  const float* materialOpenPbr;
  const float* uvs;
  const int* materialTextures;
  const float* materialTextureParams;
  const unsigned char* texels;
  const struct LusdviewTextureDesc* textures;
  float4* accumulation;
  const float* lights;
  const float* triangles;
  const struct LusdviewInstance* instances;
  const int* faces;
  unsigned int width;
  unsigned int height;
  unsigned int background;
  unsigned int validationMode;
  float invViewProjection[16];
  float lightDirection[4];
  float sceneMin[4];
  float sceneExtent[4];
  unsigned int renderMode;
  unsigned int numMaterials;
  unsigned int maxDepth;
  unsigned int numTextures;
  unsigned int sampleIndex;
  unsigned int totalSamples;
  unsigned int pathMode;
  unsigned int pathSeed;
  unsigned int numLights;
  unsigned int numInstances;
  const float* volumeDensity;
  const struct VolParam* volumes;
  unsigned int numVolumes;
  unsigned int volumePadding;
};

static_assert(sizeof(LusdviewOptixLaunchParams) == lusdview::kOptixLaunchParamsBytes,
              "OptiX device launch parameter layout mismatch");

struct LusdviewTextureDesc {
  int offset, width, height, wrapS, wrapT, srgb, isUdim, isPtex;
  int ptexCols, ptexRows, ptexTileEdge, ptexRectTexelOffset, ptexFaceCount;
  int mipCount, firstMip, imageSlot;
  int udimLayer[100];
};

struct LusdviewInstance {
  float worldToObject[12];
  float objectToWorld[12];
  float tint[4];
  int blasRoot;
  int instanceId;
  unsigned int directLightMask;
  unsigned int shadowLightMask;
};

extern "C" {
__constant__ LusdviewOptixLaunchParams lusdviewOptixLaunchParams;
}

static __forceinline__ __device__ float3 add3(const float3 a, const float3 b) {
  return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static __forceinline__ __device__ float3 mul3(const float3 a, const float3 b) {
  return make_float3(a.x * b.x, a.y * b.y, a.z * b.z);
}
static __forceinline__ __device__ float3 scale3(const float3 a, const float b) {
  return make_float3(a.x * b, a.y * b, a.z * b);
}
static __forceinline__ __device__ float dot3(const float3 a, const float3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
static __forceinline__ __device__ float3 normalize3(const float3 a) {
  return scale3(a, rsqrtf(fmaxf(dot3(a, a), 1.0e-30f)));
}
static __forceinline__ __device__ float3 sub3(float3 a, float3 b) {
  return make_float3(a.x-b.x,a.y-b.y,a.z-b.z);
}
// Keep this sampler in sync with raytracer_kernel_src.txt.
// CUDA and OptiX consume the same packed host buffers.
struct VolParam {
  float invModel[16];
  float bmin[4];      // .xyz aabb min, .w level-set selector
  float bmax[4];      // .xyz aabb max, .w isovalue
  int   dim[4];       // .xyz dims, .w = data offset (floats) into volDens
  float albedo[4];    // .xyz albedo, .w densityScale
  float emission[4];  // .xyz emission, .w background
};
__device__ float3 xfPt4(const float* m, float3 p){  // column-major mat4 * point
  float x = m[0]*p.x + m[4]*p.y + m[8]*p.z + m[12];
  float y = m[1]*p.x + m[5]*p.y + m[9]*p.z + m[13];
  float z = m[2]*p.x + m[6]*p.y + m[10]*p.z + m[14];
  float w = m[3]*p.x + m[7]*p.y + m[11]*p.z + m[15];
  if (w==0.f) w=1.f;
  return make_float3(x/w, y/w, z/w);
}
__device__ float3 xfDir4(const float* m, float3 v){
  return make_float3(m[0]*v.x + m[4]*v.y + m[8]*v.z,
            m[1]*v.x + m[5]*v.y + m[9]*v.z,
            m[2]*v.x + m[6]*v.y + m[10]*v.z);
}
__device__ float volFetch(const float* dens, int off, int dx, int dy, int dz,
                          int x, int y, int z){
  x = x<0?0:(x>=dx?dx-1:x);
  y = y<0?0:(y>=dy?dy-1:y);
  z = z<0?0:(z>=dz?dz-1:z);
  return dens[off + x + dx*(y + dy*z)];
}
__device__ float sampleVol(const float* dens, const VolParam* vp, float3 p){
  float ex=vp->bmax[0]-vp->bmin[0], ey=vp->bmax[1]-vp->bmin[1], ez=vp->bmax[2]-vp->bmin[2];
  if (ex<=0.f||ey<=0.f||ez<=0.f) return 0.f;
  int dx=vp->dim[0], dy=vp->dim[1], dz=vp->dim[2], off=vp->dim[3];
  float gx=(p.x-vp->bmin[0])/ex*dx-0.5f;
  float gy=(p.y-vp->bmin[1])/ey*dy-0.5f;
  float gz=(p.z-vp->bmin[2])/ez*dz-0.5f;
  int x0=(int)floorf(gx), y0=(int)floorf(gy), z0=(int)floorf(gz);
  float fx=gx-x0, fy=gy-y0, fz=gz-z0;
  float c00=volFetch(dens,off,dx,dy,dz,x0,y0,z0)*(1-fx)+volFetch(dens,off,dx,dy,dz,x0+1,y0,z0)*fx;
  float c10=volFetch(dens,off,dx,dy,dz,x0,y0+1,z0)*(1-fx)+volFetch(dens,off,dx,dy,dz,x0+1,y0+1,z0)*fx;
  float c01=volFetch(dens,off,dx,dy,dz,x0,y0,z0+1)*(1-fx)+volFetch(dens,off,dx,dy,dz,x0+1,y0,z0+1)*fx;
  float c11=volFetch(dens,off,dx,dy,dz,x0,y0+1,z0+1)*(1-fx)+volFetch(dens,off,dx,dy,dz,x0+1,y0+1,z0+1)*fx;
  float c0=c00*(1-fy)+c10*fy, c1=c01*(1-fy)+c11*fy;
  return c0*(1-fz)+c1*fz;
}
// Emission/absorption composite of one volume over background color `bg`.
__device__ float3 compositeVolume(const float* dens, const VolParam* vp, float3 worg, float3 wdir, float3 bg, float* nearest){
  float3 oo=xfPt4(vp->invModel, worg);
  float3 od=normalize3(xfDir4(vp->invModel, wdir));
  float3 inv=make_float3(1.f/od.x, 1.f/od.y, 1.f/od.z);
  float t1a=(vp->bmin[0]-oo.x)*inv.x, t2a=(vp->bmax[0]-oo.x)*inv.x;
  float tmin=fminf(t1a,t2a), tmax=fmaxf(t1a,t2a);
  t1a=(vp->bmin[1]-oo.y)*inv.y; t2a=(vp->bmax[1]-oo.y)*inv.y;
  tmin=fmaxf(tmin,fminf(t1a,t2a)); tmax=fminf(tmax,fmaxf(t1a,t2a));
  t1a=(vp->bmin[2]-oo.z)*inv.z; t2a=(vp->bmax[2]-oo.z)*inv.z;
  tmin=fmaxf(tmin,fminf(t1a,t2a)); tmax=fminf(tmax,fmaxf(t1a,t2a));
  if (tmax<=fmaxf(tmin,0.f)) return bg;
  float t0=fmaxf(tmin,0.f);
  float ex=vp->bmax[0]-vp->bmin[0], ey=vp->bmax[1]-vp->bmin[1], ez=vp->bmax[2]-vp->bmin[2];
  float vmin=fminf(ex/vp->dim[0], fminf(ey/vp->dim[1], ez/vp->dim[2]));
  float step=vmin*0.5f; if (step<=0.f) step=(tmax-t0)/256.f;
  if(vp->bmin[3]>0.5f){
    float3 objectDirection=xfDir4(vp->invModel,wdir);
    float speed=sqrtf(dot3(objectDirection,objectDirection));
    if(speed<=0.f||!isfinite(speed))return bg;
    tmax=fminf(tmax,(*nearest)*speed);
    if(tmax<=t0)return bg;
    float ds=fmaxf((tmax-t0)/512.f,vmin*.5f);
    float iso=vp->bmax[3],previous=sampleVol(dens,vp,add3(oo,scale3(od,t0)))-iso;
    float previousT=t0,hit=-1.f;
    for(int i=0;i<512;i++){
      float tn=fminf(t0+(i+1)*ds,tmax);
      float f=sampleVol(dens,vp,add3(oo,scale3(od,tn)))-iso;
      if((f<0.f&&previous>0.f)||(f>0.f&&previous<0.f)){
        float lo=previousT,hi=tn,flo=previous;
        for(int k=0;k<16;k++){
          float mid=.5f*(lo+hi);
          float fm=sampleVol(dens,vp,add3(oo,scale3(od,mid)))-iso;
          if(fm==0.f){lo=hi=mid;break;}
          if((fm>0.f)==(flo>0.f)){lo=mid;flo=fm;}else hi=mid;
        }
        hit=.5f*(lo+hi);break;
      }
      if(f!=0.f){previous=f;previousT=tn;}
      else if(previous==0.f)previousT=tn;
      if(tn>=tmax)break;
    }
    if(hit<0.f||hit/speed>=*nearest)return bg;
    float3 p=add3(oo,scale3(od,hit));
    float hx=ex/vp->dim[0],hy=ey/vp->dim[1],hz=ez/vp->dim[2];
    float3 g=make_float3((sampleVol(dens,vp,add3(p,make_float3(hx,0,0)))-sampleVol(dens,vp,sub3(p,make_float3(hx,0,0))))/hx,
            (sampleVol(dens,vp,add3(p,make_float3(0,hy,0)))-sampleVol(dens,vp,sub3(p,make_float3(0,hy,0))))/hy,
            (sampleVol(dens,vp,add3(p,make_float3(0,0,hz)))-sampleVol(dens,vp,sub3(p,make_float3(0,0,hz))))/hz);
    const float* m=vp->invModel;
    float3 n=normalize3(make_float3(m[0]*g.x+m[1]*g.y+m[2]*g.z,
                  m[4]*g.x+m[5]*g.y+m[6]*g.z,
                  m[8]*g.x+m[9]*g.y+m[10]*g.z));
    float3 view=scale3(normalize3(wdir),-1.f);
    if(dot3(n,n)==0.f)n=view;
    float light=.15f+.85f*fabsf(dot3(n,view));
    *nearest=hit/speed;
    return add3(scale3(make_float3(vp->albedo[0],vp->albedo[1],vp->albedo[2]),light),
               make_float3(vp->emission[0],vp->emission[1],vp->emission[2]));
  }
  float scaleD=vp->albedo[3], background=vp->emission[3];
  float T=1.f; float3 L=make_float3(0.f,0.f,0.f);
  float t=t0;
  for (int i=0;i<256 && t<tmax;i++,t+=step){
    float3 p=add3(oo, scale3(od, t+0.5f*step));
    float dn=(sampleVol(dens,vp,p)-background)*scaleD;
    if (dn>0.f){
      float a=1.f-expf(-dn*step);
      L=add3(L, scale3(add3(scale3(make_float3(vp->albedo[0],vp->albedo[1],vp->albedo[2]),a),
                         scale3(make_float3(vp->emission[0],vp->emission[1],vp->emission[2]),dn*step)), T));
      T*=(1.f-a);
      if (T<0.003f) break;
    }
  }
  return add3(L, scale3(bg, T));
}


static __forceinline__ __device__ float3 cross3(const float3 a,
                                                 const float3 b) {
  return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x);
}
static __forceinline__ __device__ float3 transformPoint(const float* m,
                                                         float3 p) {
  return make_float3(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                     m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                     m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
}
static __forceinline__ __device__ unsigned int hash32(unsigned int x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
  return x ^ (x >> 16);
}
static __forceinline__ __device__ float3 idColor(unsigned int id) {
  const unsigned int h = hash32(id + 0x9e3779b9u);
  return make_float3(float((h >> 0u) & 255u) / 255.0f,
                     float((h >> 8u) & 255u) / 255.0f,
                     float((h >> 16u) & 255u) / 255.0f);
}
static __forceinline__ __device__ float random01(unsigned int& state) {
  state = state * 747796405u + 2891336453u;
  unsigned int value = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return float((value >> 22u) ^ value) / 4294967296.0f;
}
static __forceinline__ __device__ float fresnelDielectric(float cosine,
                                                           float etaI,
                                                           float etaT) {
  cosine = fminf(fmaxf(cosine, 0.0f), 1.0f);
  const float eta = etaI / etaT;
  const float sinT2 = eta * eta * fmaxf(0.0f, 1.0f - cosine * cosine);
  if (sinT2 >= 1.0f) return 1.0f;
  const float cosT = sqrtf(fmaxf(0.0f, 1.0f - sinT2));
  const float rs = (etaI * cosine - etaT * cosT) /
                   fmaxf(etaI * cosine + etaT * cosT, 1.0e-6f);
  const float rp = (etaT * cosine - etaI * cosT) /
                   fmaxf(etaT * cosine + etaI * cosT, 1.0e-6f);
  return 0.5f * (rs * rs + rp * rp);
}
static __forceinline__ __device__ float3 cosineHemisphere(float3 normal,
                                                           unsigned int& rng) {
  const float r = sqrtf(random01(rng));
  const float phi = 6.28318530718f * random01(rng);
  const float3 axis = fabsf(normal.x) > 0.9f
                          ? make_float3(0.0f, 1.0f, 0.0f)
                          : make_float3(1.0f, 0.0f, 0.0f);
  const float3 tangent = normalize3(cross3(axis, normal));
  const float3 bitangent = cross3(normal, tangent);
  return normalize3(add3(add3(scale3(tangent, r * cosf(phi)),
                              scale3(bitangent, r * sinf(phi))),
                         scale3(normal, sqrtf(fmaxf(0.0f, 1.0f - r * r)))));
}
static __forceinline__ __device__ float3 backgroundColor() {
  const unsigned int c = lusdviewOptixLaunchParams.background;
  return make_float3(float(c & 255u) / 255.0f,
                     float((c >> 8u) & 255u) / 255.0f,
                     float((c >> 16u) & 255u) / 255.0f);
}
static __forceinline__ __device__ float3 environmentColor(float3 direction) {
  if (!lusdviewOptixLaunchParams.lights) {
    return lusdviewOptixLaunchParams.pathMode
               ? make_float3(0.0f, 0.0f, 0.0f) : backgroundColor();
  }
  for (unsigned int i = 0; i < lusdviewOptixLaunchParams.numLights; ++i) {
    const float* light = lusdviewOptixLaunchParams.lights + i * 80u;
    if (static_cast<int>(light[0] + 0.5f) != 6) continue;
    if (!(static_cast<int>(light[1] + 0.5f) & (1 << 7)))
      return make_float3(fmaxf(light[16], 0.0f), fmaxf(light[17], 0.0f),
                         fmaxf(light[18], 0.0f));
    const float3 e = make_float3(
        light[40] * direction.x + light[41] * direction.y + light[42] * direction.z,
        light[44] * direction.x + light[45] * direction.y + light[46] * direction.z,
        light[48] * direction.x + light[49] * direction.y + light[50] * direction.z);
    float basis[9];
    basis[0] = 0.282095f; basis[1] = 0.488603f * e.y;
    basis[2] = 0.488603f * e.z; basis[3] = 0.488603f * e.x;
    basis[4] = 1.092548f * e.x * e.y; basis[5] = 1.092548f * e.y * e.z;
    basis[6] = 0.315392f * (3.0f * e.z * e.z - 1.0f);
    basis[7] = 1.092548f * e.x * e.z;
    basis[8] = 0.546274f * (e.x * e.x - e.y * e.y);
    float3 result = make_float3(0.0f, 0.0f, 0.0f);
    for (int k = 0; k < 9; ++k) {
      result.x += basis[k] * light[52 + k * 3];
      result.y += basis[k] * light[53 + k * 3];
      result.z += basis[k] * light[54 + k * 3];
    }
    return make_float3(fmaxf(result.x, 0.0f) * light[16],
                       fmaxf(result.y, 0.0f) * light[17],
                       fmaxf(result.z, 0.0f) * light[18]);
  }
  return lusdviewOptixLaunchParams.pathMode
             ? make_float3(0.0f, 0.0f, 0.0f) : backgroundColor();
}
static __forceinline__ __device__ float wrapCoord(float x, int mode) {
  if (mode == 0 || mode == 3) return fminf(fmaxf(x, 0.0f), 1.0f);
  if (mode == 2) {
    const float tile = floorf(x), fraction = x - tile;
    return (static_cast<int>(tile) & 1) ? 1.0f - fraction : fraction;
  }
  return x - floorf(x);
}
static __forceinline__ __device__ float srgbToLinear(float x) {
  return x <= 0.04045f ? x / 12.92f
                       : powf((x + 0.055f) / 1.055f, 2.4f);
}
static __noinline__ __device__ float4 sampleTexture(int textureId,
                                                     float u, float v,
                                                     unsigned int face,
                                                     float lod) {
  const unsigned int count = lusdviewOptixLaunchParams.numTextures;
  if (!lusdviewOptixLaunchParams.texels || !lusdviewOptixLaunchParams.textures ||
      textureId < 0 || static_cast<unsigned int>(textureId) >= count)
    return make_float4(1.0f, 1.0f, 1.0f, 1.0f);
  LusdviewTextureDesc td = lusdviewOptixLaunchParams.textures[textureId];
  if (td.isUdim) {
    const int tile = static_cast<int>(floorf(u)) +
                     static_cast<int>(floorf(v)) * 10;
    if (tile < 0 || tile >= 100 || td.udimLayer[tile] < 0 ||
        static_cast<unsigned int>(td.udimLayer[tile]) >= count)
      return make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    td = lusdviewOptixLaunchParams.textures[td.udimLayer[tile]];
    u -= floorf(u); v -= floorf(v);
  }
  if (td.width <= 0 || td.height <= 0)
    return make_float4(1.0f, 1.0f, 1.0f, 1.0f);
  if (td.isPtex) {
    if (face >= static_cast<unsigned int>(td.ptexFaceCount))
      return make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    unsigned int rectangle[4];
    for (int component = 0; component < 4; ++component) {
      const int offset = td.ptexRectTexelOffset + static_cast<int>(face) * 8 +
                         component * 2;
      const unsigned char* low = lusdviewOptixLaunchParams.texels + td.offset +
                                 offset * 4;
      const unsigned char* high = low + 4;
      rectangle[component] = static_cast<unsigned int>(low[3]) |
                             (static_cast<unsigned int>(high[3]) << 8u);
    }
    if (rectangle[2] == 0u || rectangle[3] == 0u)
      return make_float4(1.0f, 1.0f, 1.0f, 1.0f);
    const float atlasX = float(rectangle[0]) + fminf(fmaxf(u, 0.0f), 1.0f) *
                         float(rectangle[2] - 1u);
    const float atlasY = float(rectangle[1]) + (1.0f - fminf(fmaxf(v, 0.0f), 1.0f)) *
                         float(rectangle[3] - 1u);
    u = td.width > 1 ? atlasX / float(td.width - 1) : 0.0f;
    v = td.height > 1 ? atlasY / float(td.height - 1) : 0.0f;
    lod = 0.0f;
  } else if (td.mipCount > 1 && td.firstMip >= 0) {
    const int level = min(static_cast<int>(floorf(fminf(fmaxf(lod, 0.0f),
        float(td.mipCount - 1))) + 0.5f), td.mipCount - 1);
    if (level > 0 && static_cast<unsigned int>(td.firstMip + level - 1) < count)
      td = lusdviewOptixLaunchParams.textures[td.firstMip + level - 1];
  }
  const float x = wrapCoord(u, td.wrapS) * float(td.width - 1);
  const float y = wrapCoord(v, td.wrapT) * float(td.height - 1);
  const int x0 = static_cast<int>(floorf(x)), y0 = static_cast<int>(floorf(y));
  const int x1 = min(x0 + 1, td.width - 1), y1 = min(y0 + 1, td.height - 1);
  const float fx = x - float(x0), fy = y - float(y0);
  const unsigned char* pixels = lusdviewOptixLaunchParams.texels + td.offset;
  float4 c[4];
  const int offsets[4] = {(y0 * td.width + x0) * 4,
                          (y0 * td.width + x1) * 4,
                          (y1 * td.width + x0) * 4,
                          (y1 * td.width + x1) * 4};
  for (int i = 0; i < 4; ++i) {
    const unsigned char* p = pixels + offsets[i];
    c[i] = make_float4(float(p[0]) / 255.0f, float(p[1]) / 255.0f,
                       float(p[2]) / 255.0f, float(p[3]) / 255.0f);
    if (td.srgb) {
      c[i].x = srgbToLinear(c[i].x); c[i].y = srgbToLinear(c[i].y);
      c[i].z = srgbToLinear(c[i].z);
    }
  }
  const float4 a = make_float4(c[0].x + (c[1].x - c[0].x) * fx,
      c[0].y + (c[1].y - c[0].y) * fx, c[0].z + (c[1].z - c[0].z) * fx,
      c[0].w + (c[1].w - c[0].w) * fx);
  const float4 b = make_float4(c[2].x + (c[3].x - c[2].x) * fx,
      c[2].y + (c[3].y - c[2].y) * fx, c[2].z + (c[3].z - c[2].z) * fx,
      c[2].w + (c[3].w - c[2].w) * fx);
  return make_float4(a.x + (b.x - a.x) * fy, a.y + (b.y - a.y) * fy,
      a.z + (b.z - a.z) * fy, a.w + (b.w - a.w) * fy);
}

extern "C" __global__ void __raygen__lusdview() {
  const uint3 index = optixGetLaunchIndex();
  const uint3 size = optixGetLaunchDimensions();
  unsigned int rng = hash32(index.x + index.y * size.x +
      lusdviewOptixLaunchParams.sampleIndex * 0x9e3779b9u +
      lusdviewOptixLaunchParams.pathSeed * 0x85ebca6bu);
  const float jitterX = lusdviewOptixLaunchParams.pathMode ? random01(rng) : 0.5f;
  const float jitterY = lusdviewOptixLaunchParams.pathMode ? random01(rng) : 0.5f;
  const float2 pixel = make_float2(index.x + jitterX, index.y + jitterY);
  const float2 ndc = make_float2(pixel.x / static_cast<float>(size.x) * 2.0f - 1.0f,
                                 pixel.y / static_cast<float>(size.y) * 2.0f - 1.0f);
  float3 origin;
  float3 direction;
  if (lusdviewOptixLaunchParams.validationMode != 0u) {
    origin = make_float3(0.0f, 0.0f, 3.0f);
    const float inverseLength =
        rsqrtf(ndc.x * ndc.x + ndc.y * ndc.y + 3.24f);
    direction = make_float3(ndc.x * inverseLength, -ndc.y * inverseLength,
                            -1.8f * inverseLength);
  } else {
    const float* m = lusdviewOptixLaunchParams.invViewProjection;
    const float4 nearH = make_float4(
        m[0] * ndc.x + m[4] * -ndc.y + m[8] * -1.0f + m[12],
        m[1] * ndc.x + m[5] * -ndc.y + m[9] * -1.0f + m[13],
        m[2] * ndc.x + m[6] * -ndc.y + m[10] * -1.0f + m[14],
        m[3] * ndc.x + m[7] * -ndc.y + m[11] * -1.0f + m[15]);
    const float4 farH = make_float4(
        m[0] * ndc.x + m[4] * -ndc.y + m[8] + m[12],
        m[1] * ndc.x + m[5] * -ndc.y + m[9] + m[13],
        m[2] * ndc.x + m[6] * -ndc.y + m[10] + m[14],
        m[3] * ndc.x + m[7] * -ndc.y + m[11] + m[15]);
    const float nearInvW = 1.0f / nearH.w;
    const float farInvW = 1.0f / farH.w;
    origin = make_float3(nearH.x * nearInvW, nearH.y * nearInvW,
                         nearH.z * nearInvW);
    const float3 farPoint = make_float3(farH.x * farInvW, farH.y * farInvW,
                                        farH.z * farInvW);
    const float3 delta = make_float3(farPoint.x - origin.x,
                                     farPoint.y - origin.y,
                                     farPoint.z - origin.z);
    const float inverseLength =
        rsqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    direction = make_float3(delta.x * inverseLength, delta.y * inverseLength,
                            delta.z * inverseLength);
  }
  const float3 volumeOrigin = origin, volumeDirection = direction;
  float volumeDepth = 1.0e30f;
  float3 radiance = make_float3(0.0f, 0.0f, 0.0f);
  float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
  unsigned int mediumInstances[8];
  float mediumIors[8];
  float3 mediumSigma[8];
  unsigned int mediumCount = 0u;
  const unsigned int depthLimit = max(1u, min(lusdviewOptixLaunchParams.maxDepth, 16u));
  for (unsigned int depth = 0; depth < depthLimit; ++depth) {
    unsigned int triangle = 0u, hitT = 0u, nx = 0u, ny = 0u, nz = 0u;
    unsigned int front = 0u, instance = 0u, baryU = 0u, baryV = 0u;
    unsigned int gnx = 0u, gny = 0u, gnz = 0u;
    optixTrace(lusdviewOptixLaunchParams.traversable, origin, direction,
               1.0e-4f, 1.0e16f, 0.0f, OptixVisibilityMask(255),
               OPTIX_RAY_FLAG_NONE, 0, 1, 0, triangle, hitT, nx, ny, nz,
               front, instance, baryU, baryV, gnx, gny, gnz);
    if (triangle == 0u) {
      radiance = add3(radiance, mul3(throughput, environmentColor(direction)));
      break;
    }
    const unsigned long long tri = static_cast<unsigned long long>(triangle - 1u);
    float3 normal = normalize3(make_float3(__uint_as_float(nx),
                                            __uint_as_float(ny),
                                            __uint_as_float(nz)));
    const float3 geometricNormal = normalize3(make_float3(
        __uint_as_float(gnx), __uint_as_float(gny), __uint_as_float(gnz)));
    const float t = __uint_as_float(hitT);
    if (depth == 0u) volumeDepth = t;
    const float bu = __uint_as_float(baryU), bv = __uint_as_float(baryV);
    const float3 hit = add3(origin, scale3(direction, t));
    float hitU = 0.0f, hitV = 0.0f;
    if (lusdviewOptixLaunchParams.uvs) {
      const float* uv = lusdviewOptixLaunchParams.uvs + tri * 6ull;
      hitU = uv[0] * (1.0f - bu - bv) + uv[2] * bu + uv[4] * bv;
      hitV = uv[1] * (1.0f - bu - bv) + uv[3] * bu + uv[5] * bv;
    }
    const unsigned int sourceFace = lusdviewOptixLaunchParams.faces
        ? static_cast<unsigned int>(max(lusdviewOptixLaunchParams.faces[tri], 0))
        : static_cast<unsigned int>(tri);
    if (mediumCount != 0u) {
      const float3 sigma = mediumSigma[mediumCount - 1u];
      throughput = mul3(throughput, make_float3(expf(-sigma.x * t),
                                                expf(-sigma.y * t),
                                                expf(-sigma.z * t)));
    }
    if (lusdviewOptixLaunchParams.renderMode == 2u) {
      radiance = make_float3(normal.x * 0.5f + 0.5f,
                             normal.y * 0.5f + 0.5f,
                             normal.z * 0.5f + 0.5f);
      break;
    }
    int material = lusdviewOptixLaunchParams.materials
                       ? lusdviewOptixLaunchParams.materials[tri] : -1;
    float3 base = make_float3(0.8f, 0.8f, 0.8f);
    float3 emission = make_float3(0.0f, 0.0f, 0.0f);
    float transmission = 0.0f, ior = 1.5f, opacity = 1.0f;
    float metallic = 0.0f, roughness = 0.5f;
    float3 transmissionColor = make_float3(1.0f, 1.0f, 1.0f);
    float3 transmissionScatter = make_float3(1.0f, 1.0f, 1.0f);
    float transmissionDepth = 0.0f;
    bool thinWalled = false;
    if (material >= 0 && static_cast<unsigned int>(material) <
                             lusdviewOptixLaunchParams.numMaterials) {
      if (lusdviewOptixLaunchParams.materialBase) {
        const float* b = lusdviewOptixLaunchParams.materialBase + material * 3;
        base = make_float3(b[0], b[1], b[2]);
      }
      if (lusdviewOptixLaunchParams.materialPbr) {
        const float* p = lusdviewOptixLaunchParams.materialPbr + material * 6;
        emission = make_float3(p[2], p[3], p[4]);
        opacity = p[5];
        metallic = fminf(fmaxf(p[0], 0.0f), 1.0f);
        roughness = fminf(fmaxf(p[1], 0.02f), 1.0f);
      }
      if (lusdviewOptixLaunchParams.materialOpenPbr) {
        const float* p = lusdviewOptixLaunchParams.materialOpenPbr + material * 80;
        if (p[51] > 0.5f) {
          base = scale3(make_float3(p[0], p[1], p[2]), p[3]);
          transmissionColor = make_float3(p[8], p[9], p[10]);
          transmission = fminf(fmaxf(p[11], 0.0f), 1.0f);
          transmissionScatter = make_float3(p[12], p[13], p[14]);
          transmissionDepth = fmaxf(p[15], 0.0f);
          opacity = fminf(fmaxf(p[39], 0.0f), 1.0f);
          ior = fmaxf(p[43], 1.0001f);
          emission = scale3(make_float3(p[32], p[33], p[34]), p[35]);
          thinWalled = p[71] > 0.5f;
        }
      }
      if (lusdviewOptixLaunchParams.uvs &&
          lusdviewOptixLaunchParams.materialTextures) {
        float u = hitU, v = hitV;
        const float* tp = lusdviewOptixLaunchParams.materialTextureParams
                              ? lusdviewOptixLaunchParams.materialTextureParams +
                                    material * 155 : nullptr;
        if (tp) {
          const float ou = u, ov = v;
          u = tp[0] * ou + tp[1] * ov + tp[2];
          v = tp[3] * ou + tp[4] * ov + tp[5];
        }
        const int texture =
            lusdviewOptixLaunchParams.materialTextures[material * 12];
        const float4 texel = sampleTexture(texture, u, v, sourceFace,
            roughness * float(8));
        float4 adjusted = texel;
        if (tp) adjusted = make_float4(
            texel.x * tp[36] + tp[40], texel.y * tp[37] + tp[41],
            texel.z * tp[38] + tp[42], texel.w * tp[39] + tp[43]);
        base = mul3(base, make_float3(adjusted.x, adjusted.y, adjusted.z));
        opacity *= adjusted.w;
        const int metallicTexture =
            lusdviewOptixLaunchParams.materialTextures[material * 12 + 1];
        if (metallicTexture >= 0) {
          float mu = hitU, mv = hitV;
          if (tp) { const float ou = mu, ov = mv;
            mu = tp[6] * ou + tp[7] * ov + tp[8];
            mv = tp[9] * ou + tp[10] * ov + tp[11]; }
          const float4 value = sampleTexture(metallicTexture, mu, mv,
                                              sourceFace, roughness * 8.0f);
          const int channel = tp ? static_cast<int>(tp[60] + 0.5f) : 0;
          const float scalar = channel == 1 ? value.y :
              (channel == 2 ? value.z : (channel == 3 ? value.w : value.x));
          metallic = fminf(fmaxf(scalar * (tp ? tp[61] : 1.0f) +
                                  (tp ? tp[62] : 0.0f), 0.0f), 1.0f);
        }
        const int roughnessTexture =
            lusdviewOptixLaunchParams.materialTextures[material * 12 + 2];
        if (roughnessTexture >= 0) {
          float ru = hitU, rv = hitV;
          if (tp) { const float ou = ru, ov = rv;
            ru = tp[12] * ou + tp[13] * ov + tp[14];
            rv = tp[15] * ou + tp[16] * ov + tp[17]; }
          const float4 value = sampleTexture(roughnessTexture, ru, rv,
                                              sourceFace, roughness * 8.0f);
          const int channel = tp ? static_cast<int>(tp[63] + 0.5f) : 0;
          const float scalar = channel == 1 ? value.y :
              (channel == 2 ? value.z : (channel == 3 ? value.w : value.x));
          roughness = fminf(fmaxf(scalar * (tp ? tp[64] : 1.0f) +
                                   (tp ? tp[65] : 0.0f), 0.02f), 1.0f);
        }
        const int emissiveTexture =
            lusdviewOptixLaunchParams.materialTextures[material * 12 + 4];
        if (emissiveTexture >= 0) {
          float eu = hitU, ev = hitV;
          if (tp) { const float ou = eu, ov = ev;
            eu = tp[24] * ou + tp[25] * ov + tp[26];
            ev = tp[27] * ou + tp[28] * ov + tp[29]; }
          float4 value = sampleTexture(emissiveTexture, eu, ev, sourceFace, 0.0f);
          if (tp) value = make_float4(value.x * tp[52] + tp[56],
              value.y * tp[53] + tp[57], value.z * tp[54] + tp[58],
              value.w * tp[55] + tp[59]);
          emission = mul3(emission, make_float3(value.x, value.y, value.z));
        }
        const int opacityTexture =
            lusdviewOptixLaunchParams.materialTextures[material * 12 + 5];
        if (opacityTexture >= 0) {
          float ou = hitU, ov = hitV;
          if (tp) { const float inputU = ou, inputV = ov;
            ou = tp[30] * inputU + tp[31] * inputV + tp[32];
            ov = tp[33] * inputU + tp[34] * inputV + tp[35]; }
          const float4 value = sampleTexture(opacityTexture, ou, ov,
                                              sourceFace, 0.0f);
          const int channel = tp ? static_cast<int>(tp[66] + 0.5f) : 0;
          const float scalar = channel == 1 ? value.y :
              (channel == 2 ? value.z : (channel == 3 ? value.w : value.x));
          opacity *= scalar * (tp ? tp[67] : 1.0f) + (tp ? tp[68] : 0.0f);
          opacity = fminf(fmaxf(opacity, 0.0f), 1.0f);
        }
      }
    }
    if (lusdviewOptixLaunchParams.renderMode != 0u) {
      const unsigned int mode = lusdviewOptixLaunchParams.renderMode;
      const float bw = 1.0f - bu - bv;
      if (mode == 1u) {
        const float edge = fminf(bw, fminf(bu, bv));
        radiance = edge < 0.025f ? make_float3(0.02f, 0.02f, 0.02f)
                                : make_float3(0.82f, 0.82f, 0.82f);
      } else if (mode == 3u) {
        radiance = idColor(static_cast<unsigned int>(max(material, -1) + 1));
      } else if (mode == 4u) {
        radiance = add3(scale3(geometricNormal, 0.5f),
                        make_float3(0.5f, 0.5f, 0.5f));
      } else if (mode == 5u) {
        radiance = make_float3(hitU - floorf(hitU), hitV - floorf(hitV), 0.0f);
      } else if (mode == 6u) {
        const float d = fminf(t / fmaxf(lusdviewOptixLaunchParams.lightDirection[3],
                                        1.0e-6f), 1.0f);
        radiance = make_float3(d, d, d);
      } else if (mode == 7u) {
        radiance = base;
      } else if (mode == 8u) {
        radiance = front ? make_float3(0.1f, 0.9f, 0.2f)
                         : make_float3(0.9f, 0.1f, 0.1f);
      } else if (mode == 9u) {
        radiance = make_float3(roughness, roughness, roughness);
      } else if (mode == 10u) {
        radiance = make_float3(metallic, metallic, metallic);
      } else if (mode == 11u) {
        radiance = emission;
      } else if (mode == 12u) {
        radiance = make_float3(opacity, opacity, opacity);
      } else if (mode == 13u) {
        radiance = make_float3(
            (hit.x - lusdviewOptixLaunchParams.sceneMin[0]) /
                fmaxf(lusdviewOptixLaunchParams.sceneExtent[0], 1.0e-6f),
            (hit.y - lusdviewOptixLaunchParams.sceneMin[1]) /
                fmaxf(lusdviewOptixLaunchParams.sceneExtent[1], 1.0e-6f),
            (hit.z - lusdviewOptixLaunchParams.sceneMin[2]) /
                fmaxf(lusdviewOptixLaunchParams.sceneExtent[2], 1.0e-6f));
      } else if (mode == 14u) {
        radiance = make_float3(bw, bu, bv);
      } else if (mode == 15u) {
        radiance = idColor(static_cast<unsigned int>(tri));
      } else if (mode == 16u || mode == 26u) {
        radiance = idColor(instance);
      } else if (mode == 23u) {
        const int checker = (static_cast<int>(floorf(hitU * 10.0f)) +
                             static_cast<int>(floorf(hitV * 10.0f))) & 1;
        radiance = checker ? make_float3(0.85f, 0.85f, 0.85f)
                           : make_float3(0.08f, 0.08f, 0.08f);
      } else if (mode == 30u) {
        const int tileU = static_cast<int>(floorf(hitU));
        const int tileV = static_cast<int>(floorf(hitV));
        radiance = idColor(static_cast<unsigned int>(1001 + tileU + 10 * tileV));
      } else if (mode == 34u) {
        radiance = idColor(sourceFace);
      } else if (mode == 39u || mode == 40u) {
        const float dielectric = ((ior - 1.0f) / (ior + 1.0f)) *
                                 ((ior - 1.0f) / (ior + 1.0f));
        radiance = mode == 40u
                       ? make_float3(dielectric, dielectric, dielectric)
                       : make_float3(dielectric + (base.x - dielectric) * metallic,
                                     dielectric + (base.y - dielectric) * metallic,
                                     dielectric + (base.z - dielectric) * metallic);
      }
      break;
    }
    radiance = add3(radiance, mul3(throughput, emission));
    float passWeight = fmaxf(transmission, 1.0f - opacity);
    if (lusdviewOptixLaunchParams.pathMode && transmission <= 1.0e-4f &&
        opacity < 0.9999f) {
      // Stochastic alpha keeps the estimator energy-conserving. A covered
      // sample shades this layer; an uncovered sample follows the secondary
      // ray without also adding the preview surface term.
      passWeight = random01(rng) < opacity ? 0.0f : 1.0f;
    }
    const float3 light = normalize3(make_float3(
        lusdviewOptixLaunchParams.lightDirection[0],
        lusdviewOptixLaunchParams.lightDirection[1],
        lusdviewOptixLaunchParams.lightDirection[2]));
    const float diffuse = fmaxf(0.0f, dot3(normal, light));
    const float3 surface = scale3(base, 0.08f + 0.92f * diffuse);
    if (passWeight > 1.0e-4f) {
      // Alpha-blended glass retains its authored surface coverage while the
      // complementary energy continues through secondary traversal. Physical
      // transmission uses its lobe weight instead of opacity for this split.
      if (!lusdviewOptixLaunchParams.pathMode) {
        const float surfaceWeight = opacity * (1.0f - transmission);
        radiance = add3(radiance,
                        mul3(throughput, scale3(surface, surfaceWeight)));
      }
      const bool entering = front != 0u;
      const float currentIor = mediumCount ? mediumIors[mediumCount - 1u] : 1.0f;
      float etaI = currentIor;
      float etaT = entering ? ior : 1.0f;
      int exitingLevel = -1;
      if (!entering) {
        for (int level = static_cast<int>(mediumCount) - 1; level >= 0; --level) {
          if (mediumInstances[level] == instance) { exitingLevel = level; break; }
        }
        etaT = exitingLevel > 0 ? mediumIors[exitingLevel - 1] : 1.0f;
      }
      if (thinWalled) { etaI = 1.0f; etaT = 1.0f; }
      float3 faceNormal = entering ? normal : scale3(normal, -1.0f);
      const float cosI = fminf(fmaxf(-dot3(direction, faceNormal), 0.0f), 1.0f);
      const float eta = etaI / etaT;
      const float sinT2 = eta * eta * fmaxf(0.0f, 1.0f - cosI * cosI);
      float3 nextDirection;
      const float fresnel = fresnelDielectric(cosI, etaI, etaT);
      const bool reflectRay = sinT2 >= 1.0f ||
          (lusdviewOptixLaunchParams.pathMode && random01(rng) < fresnel);
      if (reflectRay) {
        nextDirection = add3(direction, scale3(faceNormal, 2.0f * cosI));
      } else {
        const float cosT = sqrtf(fmaxf(0.0f, 1.0f - sinT2));
        nextDirection = add3(scale3(direction, eta),
                             scale3(faceNormal, eta * cosI - cosT));
        if (!thinWalled) {
          if (entering && mediumCount < 8u) {
            mediumInstances[mediumCount] = instance;
            mediumIors[mediumCount] = ior;
            float3 sigma = make_float3(0.0f, 0.0f, 0.0f);
            if (transmissionDepth > 1.0e-6f) {
              sigma = make_float3(
                  -logf(fmaxf(transmissionScatter.x, 1.0e-4f)) / transmissionDepth,
                  -logf(fmaxf(transmissionScatter.y, 1.0e-4f)) / transmissionDepth,
                  -logf(fmaxf(transmissionScatter.z, 1.0e-4f)) / transmissionDepth);
            }
            mediumSigma[mediumCount] = sigma;
            ++mediumCount;
          } else if (!entering && exitingLevel >= 0) {
            for (unsigned int level = static_cast<unsigned int>(exitingLevel);
                 level + 1u < mediumCount; ++level) {
              mediumInstances[level] = mediumInstances[level + 1u];
              mediumIors[level] = mediumIors[level + 1u];
              mediumSigma[level] = mediumSigma[level + 1u];
            }
            --mediumCount;
          }
        }
      }
      if (reflectRay) {
        throughput = scale3(throughput, fminf(passWeight, 1.0f));
      } else {
        throughput = mul3(throughput,
                          scale3(transmissionColor, fminf(passWeight, 1.0f)));
      }
      origin = add3(hit, scale3(normalize3(nextDirection), 2.0e-4f));
      direction = normalize3(nextDirection);
      continue;
    }
    if (!lusdviewOptixLaunchParams.pathMode) {
      radiance = add3(radiance, mul3(throughput, surface));
      break;
    }
    const float3 orientedNormal = dot3(direction, normal) < 0.0f
                                      ? normal : scale3(normal, -1.0f);
    // One-sample-per-light direct estimate. Finite analytic emitters sample
    // their authored surface; visibility uses the same OptiX IAS as camera rays.
    for (unsigned int lightIndex = 0;
         lusdviewOptixLaunchParams.lights &&
         lightIndex < lusdviewOptixLaunchParams.numLights; ++lightIndex) {
      const float* packedLight = lusdviewOptixLaunchParams.lights + lightIndex * 80u;
      const int lightType = static_cast<int>(packedLight[0] + 0.5f);
      if (lightType == 6) continue;
      if (lightIndex < 32u && instance < lusdviewOptixLaunchParams.numInstances &&
          !(lusdviewOptixLaunchParams.instances[instance].directLightMask &
            (1u << lightIndex)))
        continue;
      float3 lightDirection;
      float maximumT = 1.0e16f;
      float attenuation = 1.0f;
      float emitterCosine = 1.0f;
      float areaFactor = 1.0f;
      if (lightType == 5) {
        lightDirection = normalize3(scale3(make_float3(
            packedLight[8], packedLight[9], packedLight[10]), -1.0f));
      } else {
        const float3 center = make_float3(packedLight[4], packedLight[5],
                                           packedLight[6]);
        const float3 axisX = normalize3(make_float3(
            packedLight[40], packedLight[41], packedLight[42]));
        const float3 axisY = normalize3(make_float3(
            packedLight[44], packedLight[45], packedLight[46]));
        const float3 axisZ = normalize3(make_float3(
            packedLight[48], packedLight[49], packedLight[50]));
        float3 lightPoint = center;
        float3 emitterNormal = axisZ;
        if (lightType == 7 && lusdviewOptixLaunchParams.triangles &&
            lusdviewOptixLaunchParams.instances) {
          const int firstTriangle = static_cast<int>(packedLight[53] + 0.5f);
          const int triangleCount = static_cast<int>(packedLight[54] + 0.5f);
          const int lightInstance = static_cast<int>(packedLight[55] + 0.5f);
          if (firstTriangle < 0 || triangleCount <= 0 || lightInstance < 0 ||
              static_cast<unsigned int>(lightInstance) >=
                  lusdviewOptixLaunchParams.numInstances)
            continue;
          const float* transform =
              lusdviewOptixLaunchParams.instances[lightInstance].objectToWorld;
          float selectedArea = 0.0f;
          int selectedTriangle = -1;
          for (int candidate = 0; candidate < triangleCount; ++candidate) {
            const int triangleIndex = firstTriangle + candidate;
            const float* vertices =
                lusdviewOptixLaunchParams.triangles + triangleIndex * 9;
            const float3 p0 = transformPoint(transform,
                make_float3(vertices[0], vertices[1], vertices[2]));
            const float3 p1 = transformPoint(transform,
                make_float3(vertices[3], vertices[4], vertices[5]));
            const float3 p2 = transformPoint(transform,
                make_float3(vertices[6], vertices[7], vertices[8]));
            const float3 edge1 = add3(p1, scale3(p0, -1.0f));
            const float3 edge2 = add3(p2, scale3(p0, -1.0f));
            const float candidateArea = 0.5f * sqrtf(fmaxf(
                dot3(cross3(edge1, edge2), cross3(edge1, edge2)), 0.0f));
            if (candidateArea <= 1.0e-12f) continue;
            selectedArea += candidateArea;
            if (random01(rng) < candidateArea / selectedArea)
              selectedTriangle = triangleIndex;
          }
          if (selectedTriangle < 0 || selectedArea <= 1.0e-12f) continue;
          const float* vertices =
              lusdviewOptixLaunchParams.triangles + selectedTriangle * 9;
          const float3 p0 = transformPoint(transform,
              make_float3(vertices[0], vertices[1], vertices[2]));
          const float3 p1 = transformPoint(transform,
              make_float3(vertices[3], vertices[4], vertices[5]));
          const float3 p2 = transformPoint(transform,
              make_float3(vertices[6], vertices[7], vertices[8]));
          const float rootU = sqrtf(random01(rng));
          const float bary0 = 1.0f - rootU;
          const float bary1 = random01(rng) * rootU;
          const float bary2 = 1.0f - bary0 - bary1;
          lightPoint = add3(add3(scale3(p0, bary0), scale3(p1, bary1)),
                            scale3(p2, bary2));
          emitterNormal = normalize3(cross3(add3(p1, scale3(p0, -1.0f)),
                                             add3(p2, scale3(p0, -1.0f))));
          areaFactor = selectedArea;
        } else if (lightType == 1 && packedLight[7] > 1.0e-7f) {
          const float z = 1.0f - 2.0f * random01(rng);
          const float phi = 6.28318530718f * random01(rng);
          const float radial = sqrtf(fmaxf(0.0f, 1.0f - z * z));
          emitterNormal = make_float3(radial * cosf(phi), radial * sinf(phi), z);
          lightPoint = add3(center, scale3(emitterNormal, packedLight[7]));
          areaFactor = fmaxf(packedLight[23], 1.0e-7f);
        } else if (lightType == 2 && packedLight[7] > 1.0e-7f) {
          const float radius = packedLight[7] * sqrtf(random01(rng));
          const float phi = 6.28318530718f * random01(rng);
          lightPoint = add3(center, add3(scale3(axisX, radius * cosf(phi)),
                                          scale3(axisY, radius * sinf(phi))));
          emitterNormal = axisZ;
          areaFactor = fmaxf(packedLight[23], 1.0e-7f);
        } else if ((lightType == 3 || lightType == 8) &&
                   packedLight[20] > 1.0e-7f && packedLight[21] > 1.0e-7f) {
          lightPoint = add3(center, add3(
              scale3(axisX, (random01(rng) - 0.5f) * packedLight[20]),
              scale3(axisY, (random01(rng) - 0.5f) * packedLight[21])));
          emitterNormal = axisZ;
          areaFactor = fmaxf(packedLight[23], 1.0e-7f);
        } else if (lightType == 4 && packedLight[7] > 1.0e-7f &&
                   packedLight[22] > 1.0e-7f) {
          const float phi = 6.28318530718f * random01(rng);
          emitterNormal = add3(scale3(axisX, cosf(phi)),
                                scale3(axisY, sinf(phi)));
          lightPoint = add3(center, add3(scale3(emitterNormal, packedLight[7]),
              scale3(axisZ, (random01(rng) - 0.5f) * packedLight[22])));
          areaFactor = fmaxf(packedLight[23], 1.0e-7f);
        }
        const float3 toLight = make_float3(lightPoint.x - hit.x,
                                            lightPoint.y - hit.y,
                                            lightPoint.z - hit.z);
        const float distanceSquared = fmaxf(dot3(toLight, toLight), 1.0e-6f);
        maximumT = sqrtf(distanceSquared);
        lightDirection = scale3(toLight, 1.0f / maximumT);
        attenuation = 1.0f / distanceSquared;
        if ((lightType >= 1 && lightType <= 4) || lightType == 7 ||
            lightType == 8)
          emitterCosine = fabsf(dot3(emitterNormal, scale3(lightDirection, -1.0f)));
      }
      const float cosine = fmaxf(dot3(orientedNormal, lightDirection), 0.0f);
      float shaping = 1.0f;
      const int lightFlags = static_cast<int>(packedLight[1] + 0.5f);
      if ((lightFlags & 4) && lightType != 5) {
        const float coneCosine = dot3(normalize3(make_float3(
            packedLight[8], packedLight[9], packedLight[10])),
            scale3(lightDirection, -1.0f));
        const float angle = fminf(fmaxf(packedLight[24], 0.0f), 180.0f) *
                            0.01745329252f;
        const float softness = fminf(fmaxf(packedLight[25], 0.0f), 1.0f);
        const float outer = cosf(angle), inner = cosf(angle * (1.0f - softness));
        const float blend = fminf(fmaxf((coneCosine - outer) /
            fmaxf(inner - outer, 1.0e-5f), 0.0f), 1.0f);
        shaping = blend * blend * (3.0f - 2.0f * blend) *
                  powf(fmaxf(coneCosine, 0.0f), fmaxf(packedLight[26], 0.0f));
      }
      if (cosine <= 0.0f || emitterCosine <= 0.0f || shaping <= 0.0f) continue;
      float3 visibility = make_float3(1.0f, 1.0f, 1.0f);
      float3 shadowOrigin = add3(hit, scale3(orientedNormal, 2.0e-4f));
      float remainingT = fmaxf(maximumT - 4.0e-4f, 1.0e-4f);
      const int shadowLayerLimit = (lightFlags & 2) ? 8 : 0;
      for (int layer = 0; layer < shadowLayerLimit; ++layer) {
        unsigned int shadowTriangle = 0u, shadowT = 0u, shadowNx = 0u;
        unsigned int shadowNy = 0u, shadowNz = 0u, shadowFront = 0u;
        unsigned int shadowInstance = 0u, shadowU = 0u, shadowV = 0u;
        unsigned int shadowGnx = 0u, shadowGny = 0u, shadowGnz = 0u;
        optixTrace(lusdviewOptixLaunchParams.traversable, shadowOrigin,
            lightDirection, 1.0e-4f, remainingT, 0.0f, OptixVisibilityMask(255),
            OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT, 0, 1, 0, shadowTriangle,
            shadowT, shadowNx, shadowNy, shadowNz, shadowFront,
            shadowInstance, shadowU, shadowV, shadowGnx, shadowGny, shadowGnz);
        if (shadowTriangle == 0u) break;
        if (lightIndex < 32u &&
            shadowInstance < lusdviewOptixLaunchParams.numInstances &&
            !(lusdviewOptixLaunchParams.instances[shadowInstance].shadowLightMask &
              (1u << lightIndex))) {
          const float distance = __uint_as_float(shadowT);
          shadowOrigin = add3(shadowOrigin,
                              scale3(lightDirection, distance + 2.0e-4f));
          if (maximumT < 1.0e15f) remainingT -= distance + 2.0e-4f;
          continue;
        }
        const unsigned long long shadowTri =
            static_cast<unsigned long long>(shadowTriangle - 1u);
        const int shadowMaterial = lusdviewOptixLaunchParams.materials
            ? lusdviewOptixLaunchParams.materials[shadowTri] : -1;
        float layerPass = 0.0f;
        float3 layerTint = make_float3(1.0f, 1.0f, 1.0f);
        if (shadowMaterial >= 0 && static_cast<unsigned int>(shadowMaterial) <
                                      lusdviewOptixLaunchParams.numMaterials) {
          float layerOpacity = lusdviewOptixLaunchParams.materialPbr
              ? lusdviewOptixLaunchParams.materialPbr[shadowMaterial * 6 + 5]
              : 1.0f;
          if (lusdviewOptixLaunchParams.materialOpenPbr) {
            const float* shadowPbr = lusdviewOptixLaunchParams.materialOpenPbr +
                                     shadowMaterial * 80;
            if (shadowPbr[51] > 0.5f) {
              layerOpacity = shadowPbr[39];
              layerPass = fmaxf(shadowPbr[11], 1.0f - layerOpacity);
              layerTint = make_float3(shadowPbr[8], shadowPbr[9], shadowPbr[10]);
            }
          }
          layerPass = fmaxf(layerPass, 1.0f - layerOpacity);
        }
        if (layerPass <= 1.0e-4f) {
          visibility = make_float3(0.0f, 0.0f, 0.0f);
          break;
        }
        visibility = mul3(visibility, scale3(layerTint, layerPass));
        const float distance = __uint_as_float(shadowT);
        shadowOrigin = add3(shadowOrigin,
                            scale3(lightDirection, distance + 2.0e-4f));
        if (maximumT < 1.0e15f) {
          remainingT -= distance + 2.0e-4f;
          if (remainingT <= 1.0e-4f) break;
        }
      }
      if (fmaxf(visibility.x, fmaxf(visibility.y, visibility.z)) <= 1.0e-5f)
        continue;
      const float3 emitted = make_float3(packedLight[12], packedLight[13],
                                          packedLight[14]);
      const float weight = cosine * attenuation * emitterCosine * areaFactor *
                           shaping * 0.31830988618f;
      radiance = add3(radiance, mul3(throughput,
          mul3(scale3(base, 1.0f - metallic),
               mul3(visibility, scale3(emitted, weight)))));
    }
    const float dielectricF0 = ((ior - 1.0f) / (ior + 1.0f)) *
                               ((ior - 1.0f) / (ior + 1.0f));
    const float3 f0 = make_float3(
        dielectricF0 + (base.x - dielectricF0) * metallic,
        dielectricF0 + (base.y - dielectricF0) * metallic,
        dielectricF0 + (base.z - dielectricF0) * metallic);
    const float specularProbability = fminf(fmaxf(
        0.2126f * f0.x + 0.7152f * f0.y + 0.0722f * f0.z,
        0.05f), 0.95f);
    if (random01(rng) < specularProbability) {
      const float3 reflected = normalize3(add3(direction,
          scale3(orientedNormal, -2.0f * dot3(direction, orientedNormal))));
      const float3 roughDirection = cosineHemisphere(reflected, rng);
      const float blend = roughness * roughness;
      direction = normalize3(add3(scale3(reflected, 1.0f - blend),
                                  scale3(roughDirection, blend)));
      if (dot3(direction, orientedNormal) <= 0.0f) direction = reflected;
      throughput = mul3(throughput, scale3(f0, 1.0f / specularProbability));
    } else {
      direction = cosineHemisphere(orientedNormal, rng);
      throughput = mul3(throughput,
          scale3(base, (1.0f - metallic) / (1.0f - specularProbability)));
    }
    origin = add3(hit, scale3(direction, 2.0e-4f));
    if (depth >= 2u) {
      const float survival = fminf(fmaxf(fmaxf(throughput.x,
          fmaxf(throughput.y, throughput.z)), 0.05f), 0.95f);
      if (random01(rng) > survival) break;
      throughput = scale3(throughput, 1.0f / survival);
    }
  }
  if (lusdviewOptixLaunchParams.renderMode == 0u) {
    for (unsigned int i=0; i<lusdviewOptixLaunchParams.numVolumes; ++i)
      radiance = compositeVolume(lusdviewOptixLaunchParams.volumeDensity,
          &lusdviewOptixLaunchParams.volumes[i], volumeOrigin, volumeDirection,
          radiance, &volumeDepth);
  }
  const unsigned int outputIndex =
      index.y * lusdviewOptixLaunchParams.width + index.x;
  if (lusdviewOptixLaunchParams.accumulation &&
      lusdviewOptixLaunchParams.pathMode) {
    float4 accumulated = lusdviewOptixLaunchParams.sampleIndex == 0u
                             ? make_float4(0.0f, 0.0f, 0.0f, 0.0f)
                             : lusdviewOptixLaunchParams.accumulation[outputIndex];
    accumulated.x += radiance.x; accumulated.y += radiance.y;
    accumulated.z += radiance.z; accumulated.w += 1.0f;
    lusdviewOptixLaunchParams.accumulation[outputIndex] = accumulated;
    const float inverse = 1.0f / fmaxf(accumulated.w, 1.0f);
    radiance = make_float3(accumulated.x * inverse, accumulated.y * inverse,
                           accumulated.z * inverse);
  }
  const unsigned int r = static_cast<unsigned int>(fminf(fmaxf(radiance.x, 0.0f), 1.0f) * 255.0f);
  const unsigned int g = static_cast<unsigned int>(fminf(fmaxf(radiance.y, 0.0f), 1.0f) * 255.0f);
  const unsigned int b = static_cast<unsigned int>(fminf(fmaxf(radiance.z, 0.0f), 1.0f) * 255.0f);
  lusdviewOptixLaunchParams.output[outputIndex] =
      make_uchar4(r, g, b, 255u);
}

extern "C" __global__ void __miss__lusdview() {}

extern "C" __global__ void __closesthit__lusdview() {
  struct HitData {
    unsigned long long triangleOffset;
  };
  const HitData* hitData =
      reinterpret_cast<const HitData*>(optixGetSbtDataPointer());
  const unsigned int primitive = optixGetPrimitiveIndex();
  const unsigned long long triangle = hitData->triangleOffset + primitive;
  const float* n = lusdviewOptixLaunchParams.normals + triangle * 9ull;
  const float2 bary = optixGetTriangleBarycentrics();
  const float w = 1.0f - bary.x - bary.y;
  float3 normal = make_float3(w * n[0] + bary.x * n[3] + bary.y * n[6],
                              w * n[1] + bary.x * n[4] + bary.y * n[7],
                              w * n[2] + bary.x * n[5] + bary.y * n[8]);
  normal = optixTransformNormalFromObjectToWorldSpace(normal);
  const float inverseLength = rsqrtf(normal.x * normal.x + normal.y * normal.y +
                                     normal.z * normal.z);
  normal = make_float3(normal.x * inverseLength, normal.y * inverseLength,
                       normal.z * inverseLength);
  const float* p = lusdviewOptixLaunchParams.triangles + triangle * 9ull;
  float3 geometricNormal = cross3(
      make_float3(p[3] - p[0], p[4] - p[1], p[5] - p[2]),
      make_float3(p[6] - p[0], p[7] - p[1], p[8] - p[2]));
  geometricNormal = normalize3(
      optixTransformNormalFromObjectToWorldSpace(geometricNormal));
  optixSetPayload_0(static_cast<unsigned int>(triangle + 1ull));
  optixSetPayload_1(__float_as_uint(optixGetRayTmax()));
  optixSetPayload_2(__float_as_uint(normal.x));
  optixSetPayload_3(__float_as_uint(normal.y));
  optixSetPayload_4(__float_as_uint(normal.z));
  optixSetPayload_5(optixIsFrontFaceHit() ? 1u : 0u);
  optixSetPayload_6(optixGetInstanceId());
  optixSetPayload_7(__float_as_uint(bary.x));
  optixSetPayload_8(__float_as_uint(bary.y));
  optixSetPayload_9(__float_as_uint(geometricNormal.x));
  optixSetPayload_10(__float_as_uint(geometricNormal.y));
  optixSetPayload_11(__float_as_uint(geometricNormal.z));
}
