// SPDX-License-Identifier: Apache-2.0
#include "hip_processor.hh"
#include <hip/hip_runtime.h>
#include "astc_kernel.cuh"
#include "bc6h_kernel.cuh"
#include "hdr_resize_kernel.cuh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>

namespace lusdview_texture_bench {
namespace {

constexpr int kBC1 = 0, kBC3 = 1, kBC5 = 2, kBC7 = 3, kASTC = 4;

__device__ __forceinline__ uchar4 load(const uchar4* src, unsigned x,
                                       unsigned y, unsigned w, unsigned h) {
  x = min(x, w - 1u); y = min(y, h - 1u);
  return src[y * w + x];
}
__device__ __forceinline__ unsigned pack565(float r, float g, float b) {
  return (unsigned)(r * 31.0f + .5f) << 11 | (unsigned)(g * 63.0f + .5f) << 5 |
         (unsigned)(b * 31.0f + .5f);
}
__device__ __forceinline__ float3 unpack565(unsigned c) {
  return make_float3(float((c >> 11) & 31) / 31.0f,
                     float((c >> 5) & 63) / 63.0f, float(c & 31) / 31.0f);
}
__device__ __forceinline__ unsigned nearest(float3 c, float3 a, float3 b) {
  float3 p[4] = {a, b, make_float3((2*a.x+b.x)/3,(2*a.y+b.y)/3,(2*a.z+b.z)/3),
                    make_float3((a.x+2*b.x)/3,(a.y+2*b.y)/3,(a.z+2*b.z)/3)};
  unsigned best = 0; float d = 1e30f;
  for (unsigned i = 0; i < 4; ++i) { float3 q = make_float3(c.x-p[i].x,c.y-p[i].y,c.z-p[i].z); float e=q.x*q.x+q.y*q.y+q.z*q.z; if(e<d){d=e;best=i;} }
  return best;
}
__device__ __forceinline__ unsigned nearest16(float3 c, float3 a, float3 b) { unsigned best=0; float d=1e30f; for(unsigned i=0;i<16;++i){float f=i/15.f;float3 p=make_float3(a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f),q=make_float3(c.x-p.x,c.y-p.y,c.z-p.z);float e=q.x*q.x+q.y*q.y+q.z*q.z;if(e<d){d=e;best=i;}}return best; }
__device__ __forceinline__ float endpointError(const uchar4* s,unsigned w,unsigned h,unsigned bx,unsigned by,float3 a,float3 b){float total=0;for(unsigned i=0;i<16;++i){uchar4 c=load(s,bx*4+i%4,by*4+i/4,w,h);float3 v=make_float3(c.x/255.f,c.y/255.f,c.z/255.f);float best=1e30f;for(unsigned j=0;j<16;++j){float f=j/15.f;float3 p=make_float3(a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f),q=make_float3(v.x-p.x,v.y-p.y,v.z-p.z);float e=q.x*q.x+q.y*q.y+q.z*q.z;if(e<best)best=e;}total+=best;}return total;}
__device__ __forceinline__ unsigned alphaIndex(float v, float lo, float hi) {
  unsigned a0 = (unsigned)(hi * 255.0f + .5f), a1 = (unsigned)(lo * 255.0f + .5f);
  float p[8] = {a0/255.f,a1/255.f,0,0,0,0,0,0};
  for(unsigned i=2;i<8;++i)p[i]=((8-i)*p[0]+(i-1)*p[1])/7.f;
  unsigned best=0;float d=1e30f;for(unsigned i=0;i<8;++i){float e=fabsf(v-p[i]);if(e<d){d=e;best=i;}}return best;
}
__device__ __forceinline__ unsigned alphaWord(const uchar4* src,unsigned w,unsigned h,unsigned bx,unsigned by,unsigned channel,unsigned word){
  float lo=1,hi=0;for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);float v=c[channel]/255.f;lo=fminf(lo,v);hi=fmaxf(hi,v);}unsigned r=word==0?((unsigned)(hi*255+.5f)|((unsigned)(lo*255+.5f)<<8)):0;for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);unsigned v=alphaIndex(c[channel]/255.f,lo,hi);unsigned bit=16+i*3;for(unsigned k=0;k<3;++k)if((bit+k)/32==word)r|=((v>>k)&1)<<((bit+k)&31);}return r;
}
__device__ __forceinline__ void bits(unsigned* b, int bit, int n, unsigned v) { for(int i=0;i<n;++i) if((v>>i)&1u) b[(bit+i)>>5] |= 1u<<((bit+i)&31); }

__global__ void resizeKernel(const uchar4* src, uchar4* dst, unsigned sw,unsigned sh,unsigned dw,unsigned dh) {
  unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;if(x>=dw||y>=dh)return;
  float sx=(x+.5f)*sw/dw-.5f, sy=(y+.5f)*sh/dh-.5f; unsigned x0=(unsigned)max(0.f,floorf(sx)),y0=(unsigned)max(0.f,floorf(sy)); float fx=sx-floorf(sx),fy=sy-floorf(sy);
  uchar4 a=load(src,x0,y0,sw,sh),b=load(src,x0+1,y0,sw,sh),c=load(src,x0,y0+1,sw,sh),d=load(src,x0+1,y0+1,sw,sh); uchar4 o;for(int k=0;k<4;++k)o[k]=(unsigned char)fminf(255.f,(1-fy)*((1-fx)*a[k]+fx*b[k])+fy*((1-fx)*c[k]+fx*d[k]));dst[y*dw+x]=o;
}

__global__ void compressKernel(const uchar4* src,unsigned* dst,unsigned w,unsigned h,unsigned format){
  unsigned bx=blockIdx.x*blockDim.x+threadIdx.x,by=blockIdx.y*blockDim.y+threadIdx.y;if(bx*4>=w||by*4>=h)return;unsigned base=(by*((w+3)/4)+bx)*(format==kBC1?2:4);float3 lo=make_float3(1,1,1),hi=make_float3(0,0,0);
  for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);float3 v=make_float3(c.x/255.f,c.y/255.f,c.z/255.f);lo.x=fminf(lo.x,v.x);lo.y=fminf(lo.y,v.y);lo.z=fminf(lo.z,v.z);hi.x=fmaxf(hi.x,v.x);hi.y=fmaxf(hi.y,v.y);hi.z=fmaxf(hi.z,v.z);}unsigned e0=pack565(hi.x,hi.y,hi.z),e1=pack565(lo.x,lo.y,lo.z);if(e0<e1){unsigned t=e0;e0=e1;e1=t;}float3 p0=unpack565(e0),p1=unpack565(e1);if(format==kBC7){float3 llo=make_float3(1,1,1),lhi=make_float3(0,0,0);float minL=1e30f,maxL=-1e30f;for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);float3 v=make_float3(c.x/255.f,c.y/255.f,c.z/255.f);float lum=.2126f*v.x+.7152f*v.y+.0722f*v.z;if(lum<minL)minL=lum,llo=v;if(lum>=maxL)maxL=lum,lhi=v;}if(endpointError(src,w,h,bx,by,llo,lhi)<endpointError(src,w,h,bx,by,p0,p1))p0=llo,p1=lhi;}unsigned ind=0;for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);ind|=nearest(make_float3(c.x/255.f,c.y/255.f,c.z/255.f),p0,p1)<<(2*i);}
  if(format==kBC1){dst[base]=e0|(e1<<16);dst[base+1]=ind;return;} if(format==kBC3){dst[base]=alphaWord(src,w,h,bx,by,3,0);dst[base+1]=alphaWord(src,w,h,bx,by,3,1);dst[base+2]=e0|(e1<<16);dst[base+3]=ind;return;} if(format==kBC5){dst[base]=alphaWord(src,w,h,bx,by,0,0);dst[base+1]=alphaWord(src,w,h,bx,by,0,1);dst[base+2]=alphaWord(src,w,h,bx,by,1,0);dst[base+3]=alphaWord(src,w,h,bx,by,1,1);return;}
  if(format==kASTC){unsigned b[4]={0,0,0,0};float3 avg=make_float3(0,0,0);for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);avg.x+=c.x/16.f;avg.y+=c.y/16.f;avg.z+=c.z/16.f;} // ASTC void-extent block, valid and fast.
    b[0]=0x1fc00000u|((unsigned)(avg.x)<<8)|((unsigned)(avg.y)>>0);b[1]=((unsigned)avg.z<<16)|0x0001fc00u;b[2]=0;b[3]=0;for(int i=0;i<4;++i)dst[base+i]=b[i];return;}
  // BC7 mode 6, low-cost endpoint line search.
  unsigned b[4]={0,0,0,0};bits(b,0,7,64);int bit=7;for(int k=0;k<4;++k){float v=k==0?hi.x:k==1?hi.y:k==2?hi.z:1;bits(b,bit,7,(unsigned)(v*127+.5f));bit+=7;}for(int k=0;k<4;++k){float v=k==0?lo.x:k==1?lo.y:k==2?lo.z:1;bits(b,bit,7,(unsigned)(v*127+.5f));bit+=7;}bits(b,63,1,0);bits(b,64,1,0);bit=65;for(unsigned i=0;i<16;++i){uchar4 c=load(src,bx*4+i%4,by*4+i/4,w,h);unsigned q=nearest16(make_float3(c.x/255.f,c.y/255.f,c.z/255.f),p0,p1);bits(b,bit,i==0?3:4,q);bit+=i==0?3:4;}for(int i=0;i<4;++i)dst[base+i]=b[i];
}

}  // namespace

static std::string HipError(hipError_t e, const char* what) { return std::string(what)+": "+hipGetErrorString(e); }
static bool Check(hipError_t e, const char* what, std::string* error) { if(e==hipSuccess)return true;if(error)*error=HipError(e,what);return false; }
static void IgnoreHipResult(hipError_t result) { (void)result; }

HipProcessor::HipProcessor(const std::string& selector) : selector_(selector) {}
HipProcessor::~HipProcessor(){if(stream_)(void)hipStreamDestroy((hipStream_t)stream_);if(input_)(void)hipFree(input_);if(hdrInput_)(void)hipFree(hdrInput_);if(hdrResized_)(void)hipFree(hdrResized_);if(resized_)(void)hipFree(resized_);if(compressed_)(void)hipFree(compressed_);}
bool HipProcessor::init(std::string* error){int n=0;if(!Check(hipGetDeviceCount(&n),"hipGetDeviceCount",error)||n==0){if(error&&n==0)*error="no HIP devices";return false;}for(int i=0;i<n;++i){hipDeviceProp_t candidate{};if(hipGetDeviceProperties(&candidate,i)!=hipSuccess)continue;std::string name=candidate.name;std::string a=name,b=selector_;for(char& c:a)c=char(std::tolower((unsigned char)c));for(char& c:b)c=char(std::tolower((unsigned char)c));if(selector_.empty()||a.find(b)!=std::string::npos){device_=i;break;}}if(device_<0){if(error)*error="no HIP device matches --device";return false;}hipDeviceProp_t p{};if(!Check(hipGetDeviceProperties(&p,device_),"hipGetDeviceProperties",error))return false;if(!Check(hipSetDevice(device_),"hipSetDevice",error)||!Check(hipStreamCreate((hipStream_t*)&stream_),"hipStreamCreate",error))return false;info_.backend="hip";info_.name=p.name;info_.driver=std::to_string(p.major)+"."+std::to_string(p.minor);info_.hardware=true;info_.supportsBC1=info_.supportsBC3=info_.supportsBC5=info_.supportsBC7=true;info_.supportsBC6H=true;return true;}
bool HipProcessor::process(const TextureRequest& r,TextureResult* out,std::string* error){if(!r.rgba||!r.width||!r.height)return Check(hipErrorInvalidValue,"invalid request",error);unsigned dw=std::max(1u,r.dstWidth),dh=std::max(1u,r.dstHeight);size_t in=size_t(r.width)*r.height*4,sz=size_t(dw)*dh*4,co=CompressedSize(r.format,dw,dh);auto s=(hipStream_t)stream_;bool chainInput=ldrChainActive_&&ldrChainWidth_==r.width&&ldrChainHeight_==r.height;if(!r.deviceMipChain&&!chainInput)ldrChainActive_=false;if(r.format==CompressionFormat::BC6H){if(!r.rgbf||r.rgbfBytes<size_t(r.width)*r.height*3*sizeof(float)){if(error)*error="BC6H requires HDR float input";return false;}size_t bytes=size_t(r.width)*r.height*3*sizeof(float);if(hdrInputCapacity_<bytes){if(hdrInput_)(void)hipFree(hdrInput_);if(!Check(hipMalloc(&hdrInput_,bytes),"hipMalloc HDR input",error))return false;hdrInputCapacity_=bytes;}if(co>compressedCapacity_){if(compressed_)(void)hipFree(compressed_);if(!Check(hipMalloc(&compressed_,co),"hipMalloc compressed",error))return false;compressedCapacity_=co;}size_t hdrOutBytes=size_t(dw)*dh*3*sizeof(float);if(hdrResizedCapacity_<hdrOutBytes){if(hdrResized_)(void)hipFree(hdrResized_);if(!Check(hipMalloc(&hdrResized_,hdrOutBytes),"hipMalloc HDR resized",error))return false;hdrResizedCapacity_=hdrOutBytes;}hipEvent_t a,u,b,c;IgnoreHipResult(hipEventCreate(&a));IgnoreHipResult(hipEventCreate(&u));IgnoreHipResult(hipEventCreate(&b));IgnoreHipResult(hipEventCreate(&c));IgnoreHipResult(hipEventRecord(a,s));if(!Check(hipMemcpyAsync(hdrInput_,r.rgbf,bytes,hipMemcpyHostToDevice,s),"hipMemcpy HDR",error))return false;IgnoreHipResult(hipEventRecord(u,s));hdr_gpu::resizeCuda<<<dim3((dw+15)/16,(dh+15)/16),dim3(16,16),0,s>>>((const float*)hdrInput_,(float*)hdrResized_,r.width,r.height,dw,dh);if(!Check(hipGetLastError(),"HDR resize",error))return false;IgnoreHipResult(hipEventRecord(b,s));bc6h_gpu::encodeCuda<<<dim3((dw+31)/32,(dh+31)/32),dim3(8,8),0,s>>>((const float*)hdrResized_,(unsigned*)compressed_,dw,dh,dw,dh);if(!Check(hipGetLastError(),"BC6H kernel",error))return false;IgnoreHipResult(hipEventRecord(c,s));if(!Check(hipEventSynchronize(c),"BC6H synchronize",error))return false;out->width=dw;out->height=dh;out->compressed.resize(co);out->resizedRGBF.resize(size_t(dw)*dh*3);if(!Check(hipMemcpy(out->compressed.data(),compressed_,co,hipMemcpyDeviceToHost),"hipMemcpy BC6H",error)||!Check(hipMemcpy(out->resizedRGBF.data(),hdrResized_,hdrOutBytes,hipMemcpyDeviceToHost),"hipMemcpy HDR resized",error))return false;float up=0,rz=0,cp=0;IgnoreHipResult(hipEventElapsedTime(&up,a,u));IgnoreHipResult(hipEventElapsedTime(&rz,u,b));IgnoreHipResult(hipEventElapsedTime(&cp,b,c));out->timing.uploadMs=up;out->timing.resizeGpuMs=rz;out->timing.compressGpuMs=cp;out->timing.totalMs=up+cp;IgnoreHipResult(hipEventDestroy(a));IgnoreHipResult(hipEventDestroy(u));IgnoreHipResult(hipEventDestroy(b));IgnoreHipResult(hipEventDestroy(c));return true;}if(in>inputCapacity_){if(input_)(void)hipFree(input_);if(!Check(hipMalloc(&input_,in),"hipMalloc input",error))return false;inputCapacity_=in;}if(sz>resizedCapacity_){if(resized_)(void)hipFree(resized_);if(!Check(hipMalloc(&resized_,sz),"hipMalloc resized",error))return false;resizedCapacity_=sz;}if(co>compressedCapacity_){if(compressed_)(void)hipFree(compressed_);if(!Check(hipMalloc(&compressed_,co),"hipMalloc compressed",error))return false;compressedCapacity_=co;}if(chainInput){std::swap(input_,resized_);std::swap(inputCapacity_,resizedCapacity_);}hipEvent_t a,u,b,c;IgnoreHipResult(hipEventCreate(&a));IgnoreHipResult(hipEventCreate(&u));IgnoreHipResult(hipEventCreate(&b));IgnoreHipResult(hipEventCreate(&c));IgnoreHipResult(hipEventRecord(a,s));if(!chainInput&&!Check(hipMemcpyAsync(input_,r.rgba,in,hipMemcpyHostToDevice,s),"hipMemcpy HtoD",error))return false;IgnoreHipResult(hipEventRecord(u,s));dim3 t(16,16),g((dw+15)/16,(dh+15)/16);resizeKernel<<<g,t,0,s>>>((const uchar4*)input_,(uchar4*)resized_,r.width,r.height,dw,dh);if(!Check(hipGetLastError(),"resizeKernel",error))return false;IgnoreHipResult(hipEventRecord(b,s));if(r.format==CompressionFormat::ASTC){astc_gpu::encodeCuda<<<dim3((dw+31)/32,(dh+31)/32),dim3(8,8),0,s>>>((const uchar4*)resized_,(unsigned*)compressed_,dw,dh);if(!Check(hipGetLastError(),"ASTC kernel",error))return false;}else{unsigned f=r.format==CompressionFormat::BC1?kBC1:r.format==CompressionFormat::BC3?kBC3:r.format==CompressionFormat::BC5?kBC5:kBC7;dim3 ct(8,8),cg((dw+31)/32,(dh+31)/32);compressKernel<<<cg,ct,0,s>>>((const uchar4*)resized_,(unsigned*)compressed_,dw,dh,f);if(!Check(hipGetLastError(),"compressKernel",error))return false;}IgnoreHipResult(hipEventRecord(c,s));if(!Check(hipEventSynchronize(c),"hipEventSynchronize",error))return false;float up=0,rz=0,cp=0;IgnoreHipResult(hipEventElapsedTime(&up,a,u));IgnoreHipResult(hipEventElapsedTime(&rz,u,b));IgnoreHipResult(hipEventElapsedTime(&cp,b,c));out->width=dw;out->height=dh;out->compressed.resize(co);if(!r.deviceMipChain||r.downloadResized)out->resizedRGBA.resize(sz);if(!Check(hipMemcpy(out->compressed.data(),compressed_,co,hipMemcpyDeviceToHost),"hipMemcpy DtoH",error))return false;if((!r.deviceMipChain||r.downloadResized)&&!Check(hipMemcpy(out->resizedRGBA.data(),resized_,sz,hipMemcpyDeviceToHost),"hipMemcpy resized",error))return false;ldrChainActive_=r.deviceMipChain;ldrChainWidth_=dw;ldrChainHeight_=dh;out->timing.uploadMs=up;out->timing.resizeGpuMs=rz;out->timing.compressGpuMs=cp;out->timing.downloadMs=0;out->timing.totalMs=up+rz+cp;IgnoreHipResult(hipEventDestroy(a));IgnoreHipResult(hipEventDestroy(u));IgnoreHipResult(hipEventDestroy(b));IgnoreHipResult(hipEventDestroy(c));return true;}
} // namespace lusdview_texture_bench

extern "C" lusdview_texture_bench::Processor* lusdview_texture_hip_create(
    const char* selector, std::string* error) {
  auto* processor = new lusdview_texture_bench::HipProcessor(
      selector ? selector : "");
  if (!processor->init(error)) {
    delete processor;
    return nullptr;
  }
  return processor;
}

extern "C" void lusdview_texture_hip_destroy(
    lusdview_texture_bench::Processor* processor) {
  delete processor;
}
