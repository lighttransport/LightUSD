#include <emscripten/emscripten.h>
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
#include <emscripten/bind.h>
#include <emscripten/val.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "external/lightrt/lightrt_c_tri.h"
#include "external/meshoptimizer/meshoptimizer.h"
#include "lightrt-wasm-api.h"

static_assert(sizeof(lightusd_lrt_scene_info) == 48, "LightRT scene ABI size");
static_assert(offsetof(lightusd_lrt_scene_info, lengths) == 20,
              "LightRT scene ABI lengths offset");

namespace {

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
template <typename T>
std::vector<T> CopyArray(const emscripten::val &src, const char *ctor) {
  const size_t n = src["length"].as<size_t>();
  std::vector<T> out(n);
  if (!n) return out;
  (void)ctor;
  emscripten::val view(emscripten::typed_memory_view(n, out.data()));
  view.call<void>("set", src);
  return out;
}
#endif

struct Vec3 { float x, y, z; };
Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Mul(Vec3 a, float b) { return {a.x*b,a.y*b,a.z*b}; }
Vec3 Had(Vec3 a, Vec3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
float Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Norm(Vec3 a) { float l=std::sqrt(std::max(1.0e-20f,Dot(a,a))); return Mul(a,1.0f/l); }
Vec3 Reflect(Vec3 d, Vec3 n) { return Add(d, Mul(n, -2.0f*Dot(d,n))); }

uint32_t Hash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
float Random(uint32_t *s) { *s = Hash(*s + 0x9e3779b9u); return float(*s >> 8) * (1.0f/16777216.0f); }
Vec3 CosineHemisphere(Vec3 n, uint32_t *seed) {
  const float r=std::sqrt(Random(seed)), a=6.28318530718f*Random(seed);
  Vec3 t=Norm(std::fabs(n.y)<0.9f?Cross({0,1,0},n):Cross({1,0,0},n));
  Vec3 b=Cross(n,t); float z=std::sqrt(std::max(0.0f,1.0f-r*r));
  return Norm(Add(Add(Mul(t,r*std::cos(a)),Mul(b,r*std::sin(a))),Mul(n,z)));
}

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
template <typename T>
emscripten::val TypedArrayCopy(const char *ctor, const T *data, size_t n) {
  // JS typed-array lengths are Numbers; size_t is a BigInt in memory64.
  emscripten::val out=emscripten::val::global(ctor).new_(double(n));
  if(n) out.call<void>("set",emscripten::val(emscripten::typed_memory_view(n,data)));
  return out;
}
#endif

class LightRTPathTracer {
 public:
  ~LightRTPathTracer() { clear(); }
  void clear() { if (scene_) lrt_tri_scene_free(scene_); scene_=nullptr; triangles_=0; positions_.clear(); normals_.clear(); colors_.clear(); vertex_params_.clear(); material_ids_.clear(); materials_.clear(); }

  bool buildVectors(std::vector<float> positions, std::vector<float> normals,
                    std::vector<float> colors,
                    std::vector<float> vertex_params,
                    std::vector<int32_t> material_ids,
                    std::vector<float> materials) {
    clear(); error_.clear();
    positions_=std::move(positions); normals_=std::move(normals);
    colors_=std::move(colors); vertex_params_=std::move(vertex_params);
    material_ids_=std::move(material_ids); materials_=std::move(materials);
    if (positions_.empty() || positions_.size()%9) { error_="positions must contain triangle soup"; return false; }
    const size_t n=positions_.size()/9;
    if (normals_.size()!=positions_.size() || colors_.size()!=positions_.size() ||
        vertex_params_.size()!=n*12 || material_ids_.size()!=n || materials_.size()%10) { error_="attribute array size mismatch"; return false; }
    // WebAssembly has no native AVX2-width traversal. Avoid forcing the wider
    // scalar fallback; LightRT's conservative browser layout is BVH4.
    lrt_tri_build_options opts{}; opts.quality=LRT_TRI_BUILD_FAST; opts.layout=LRT_TRI_LAYOUT_BVH4; opts.num_threads=1;
    lrt_result result{}; scene_=lrt_tri_scene_build(positions_.data(),n,&opts,&result);
    if (!scene_) { error_="LightRT BVH build failed"; return false; }
    triangles_=n; return true;
  }

  bool buildRaw(const float* positions, uint32_t position_count,
                const float* normals, uint32_t normal_count,
                const float* colors, uint32_t color_count,
                const float* vertex_params, uint32_t param_count,
                const int32_t* material_ids, uint32_t id_count,
                const float* materials, uint32_t material_count) {
    // Reject malformed shape before copying caller-owned WASM buffers into
    // scene storage. Keep the same failure text and cleared-scene state as
    // buildVectors(), which validates the legacy embind input.
    if (!position_count || position_count % 9) {
      clear(); error_ = "positions must contain triangle soup"; return false;
    }
    const uint32_t triangle_count = position_count / 9;
    if (normal_count != position_count || color_count != position_count ||
        param_count != triangle_count * 12 || id_count != triangle_count ||
        material_count % 10) {
      clear(); error_ = "attribute array size mismatch"; return false;
    }
    std::vector<float> p, n, c, v, m;
    std::vector<int32_t> ids;
    if (position_count) p.assign(positions, positions + position_count);
    if (normal_count) n.assign(normals, normals + normal_count);
    if (color_count) c.assign(colors, colors + color_count);
    if (param_count) v.assign(vertex_params, vertex_params + param_count);
    if (id_count) ids.assign(material_ids, material_ids + id_count);
    if (material_count) m.assign(materials, materials + material_count);
    return buildVectors(std::move(p), std::move(n), std::move(c),
                        std::move(v), std::move(ids), std::move(m));
  }

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
  bool build(const emscripten::val &positions, const emscripten::val &normals,
             const emscripten::val &colors, const emscripten::val &vertex_params,
             const emscripten::val &material_ids, const emscripten::val &materials) {
    return buildVectors(CopyArray<float>(positions,"Float32Array"),
        CopyArray<float>(normals,"Float32Array"),
        CopyArray<float>(colors,"Float32Array"),
        CopyArray<float>(vertex_params,"Float32Array"),
        CopyArray<int32_t>(material_ids,"Int32Array"),
        CopyArray<float>(materials,"Float32Array"));
  }
#endif

  bool traceRaw(const float* inv, uint32_t inv_count,
                const float* cp, uint32_t cp_count, int width, int height,
                int sample_start, int sample_count, int max_bounces,
                float exposure, float* pixels, uint32_t pixel_count) const {
    (void)exposure;
    if (!scene_ || width<1 || height<1 || width>4096 || height>4096 ||
        !inv || inv_count!=16 || !cp || cp_count!=3 || !pixels ||
        pixel_count < size_t(width)*size_t(height)*4) return false;
    sample_count=std::clamp(sample_count,1,64); max_bounces=std::clamp(max_bounces,1,8);
    Vec3 cam{cp[0],cp[1],cp[2]};
    auto unproject=[&](float x,float y) { float v[4]={x,y,1,1},o[4]={}; for(int r=0;r<4;r++) for(int c=0;c<4;c++) o[r]+=inv[c*4+r]*v[c]; float w=std::fabs(o[3])>1e-12f?o[3]:1; return Vec3{o[0]/w,o[1]/w,o[2]/w}; };
    const Vec3 light=Norm({-0.45f,0.8f,0.35f});
    for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
      Vec3 sum{0,0,0};
      for(int s=0;s<sample_count;s++) {
        uint32_t seed=Hash(uint32_t((sample_start+s+1)*9781u)^uint32_t(y*width+x));
        float nx=2.0f*(float(x)+Random(&seed))/float(width)-1.0f;
        float ny=1.0f-2.0f*(float(y)+Random(&seed))/float(height);
        Vec3 org=cam, dir=Norm(Add(unproject(nx,ny),Mul(cam,-1))), throughput{1,1,1}, radiance{0,0,0};
        for(int bounce=0;bounce<max_bounces;bounce++) {
          lrt_ray ray{}; ray.org[0]=org.x;ray.org[1]=org.y;ray.org[2]=org.z;ray.dir[0]=dir.x;ray.dir[1]=dir.y;ray.dir[2]=dir.z;ray.tmin=1e-4f;ray.tmax=1e30f;
          lrt_hit hit{}; if (!lrt_tri_intersect1(scene_,&ray,&hit)) { float sky=0.08f+0.18f*std::max(0.0f,dir.y); radiance=Add(radiance,Had(throughput,{sky*0.85f,sky*0.92f,sky})); break; }
          size_t tri=hit.prim_id; if(tri>=triangles_) break; float w=1-hit.u-hit.v; const float *ns=&normals_[tri*9];
          Vec3 n=Norm({w*ns[0]+hit.u*ns[3]+hit.v*ns[6],w*ns[1]+hit.u*ns[4]+hit.v*ns[7],w*ns[2]+hit.u*ns[5]+hit.v*ns[8]}); if(Dot(n,dir)>0)n=Mul(n,-1);
          int mid=material_ids_[tri]; const float *m=(mid>=0 && size_t(mid)*10+9<materials_.size())?&materials_[size_t(mid)*10]:nullptr;
          const float *cs=&colors_[tri*9]; Vec3 tex={w*cs[0]+hit.u*cs[3]+hit.v*cs[6],w*cs[1]+hit.u*cs[4]+hit.v*cs[7],w*cs[2]+hit.u*cs[5]+hit.v*cs[8]};
          const float *vp=&vertex_params_[tri*12]; auto interp=[&](int lane){return w*vp[lane]+hit.u*vp[4+lane]+hit.v*vp[8+lane];};
          Vec3 base=Had(m?Vec3{m[0],m[1],m[2]}:Vec3{0.7f,0.7f,0.7f},tex); float metal=std::clamp(interp(0),0.0f,1.0f); float rough=std::clamp(interp(1),0.03f,1.0f); Vec3 emit=m?Vec3{m[5],m[6],m[7]}:Vec3{0,0,0}; float transmission=std::clamp(interp(2),0.0f,1.0f); float sss=std::clamp(interp(3),0.0f,1.0f);
          radiance=Add(radiance,Had(throughput,emit)); Vec3 hp=Add(org,Mul(dir,hit.t));
          lrt_ray shadow{}; shadow.org[0]=hp.x+n.x*1e-3f;shadow.org[1]=hp.y+n.y*1e-3f;shadow.org[2]=hp.z+n.z*1e-3f;shadow.dir[0]=light.x;shadow.dir[1]=light.y;shadow.dir[2]=light.z;shadow.tmin=1e-4f;shadow.tmax=1e30f;
          float nl=std::max(0.0f,Dot(n,light)); if(nl>0 && !lrt_tri_occluded1(scene_,&shadow)) radiance=Add(radiance,Mul(Had(throughput,base),nl*(0.7f+0.3f*sss)));
          float choose=Random(&seed); if(choose<transmission){ org=Add(hp,Mul(dir,1e-3f)); throughput=Had(throughput,base); }
          else if(choose<transmission+metal){ dir=Norm(Add(Reflect(dir,n),Mul(CosineHemisphere(n,&seed),rough*rough)));org=Add(hp,Mul(n,1e-3f));throughput=Had(throughput,base); }
          else { dir=CosineHemisphere(n,&seed);org=Add(hp,Mul(n,1e-3f));throughput=Had(throughput,Add(Mul(base,1-0.25f*sss),Mul({1,0.55f,0.42f},0.25f*sss))); }
          if(bounce>=2){float p=std::clamp(std::max({throughput.x,throughput.y,throughput.z}),0.1f,0.95f);if(Random(&seed)>p)break;throughput=Mul(throughput,1/p);}
        }
        sum=Add(sum,radiance);
      }
      size_t o=(size_t(y)*width+x)*4; pixels[o]=sum.x/sample_count;pixels[o+1]=sum.y/sample_count;pixels[o+2]=sum.z/sample_count;pixels[o+3]=1;
    }
    return true;
  }
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
  emscripten::val trace(const emscripten::val &inv_view_projection,
                        const emscripten::val &camera_position, int width, int height,
                        int sample_start, int sample_count, int max_bounces,
                        float exposure) {
    if (width<1 || height<1 || width>4096 || height>4096)
      return emscripten::val::undefined();
    const auto inv=CopyArray<float>(inv_view_projection,"Float32Array");
    const auto cp=CopyArray<float>(camera_position,"Float32Array");
    std::vector<float> pixels(size_t(width)*size_t(height)*4);
    if (!traceRaw(inv.data(),static_cast<uint32_t>(inv.size()),cp.data(),
                  static_cast<uint32_t>(cp.size()),width,height,sample_start,
                  sample_count,max_bounces,exposure,pixels.data(),
                  static_cast<uint32_t>(pixels.size())))
      return emscripten::val::undefined();
    return TypedArrayCopy("Float32Array", pixels.data(), pixels.size());
  }
#endif
  // Batched visibility queries for UV-space baking. Origins and directions
  // are xyz-packed object-space arrays; the returned bytes are 1 for an
  // occluded ray and 0 for a miss. Keeping this operation batched avoids a
  // JS/WASM call for every texel while reusing the resident LightRT BVH.
  bool occludedRaw(const float* org, uint32_t origin_count,
                   const float* dir, uint32_t direction_count,
                   float max_distance, uint8_t* result,
                   uint32_t result_cap) const {
    if (!scene_ || !org || !dir || !result || !origin_count ||
        origin_count != direction_count || origin_count % 3 != 0 ||
        origin_count > (1u << 26) || result_cap < origin_count / 3)
      return false;
    const size_t ray_count = origin_count / 3;
    std::vector<lrt_ray> rays(ray_count);
    for (size_t i = 0; i < ray_count; i++) {
      lrt_ray &ray = rays[i];
      ray.org[0] = org[i * 3]; ray.org[1] = org[i * 3 + 1]; ray.org[2] = org[i * 3 + 2];
      ray.dir[0] = dir[i * 3]; ray.dir[1] = dir[i * 3 + 1]; ray.dir[2] = dir[i * 3 + 2];
      ray.tmin = 1.0e-5f; ray.tmax = std::isfinite(max_distance) && max_distance > 0 ? max_distance : 1.0e30f;
    }
    lrt_tri_occluded1N(scene_, rays.data(), result, ray_count,
                       LRT_TRI_BATCH_INCOHERENT);
    return true;
  }
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
  emscripten::val occluded(const emscripten::val &origins,
                           const emscripten::val &directions,
                           float max_distance = 1.0e30f) const {
    const size_t origin_count = origins["length"].as<size_t>();
    if (!origin_count || origin_count != directions["length"].as<size_t>() ||
        origin_count % 3 || origin_count > (1u << 26))
      return emscripten::val::undefined();
    const auto org = CopyArray<float>(origins, "Float32Array");
    const auto dir = CopyArray<float>(directions, "Float32Array");
    std::vector<uint8_t> result(org.size()/3);
    if (!occludedRaw(org.data(),static_cast<uint32_t>(org.size()),dir.data(),
                     static_cast<uint32_t>(dir.size()),max_distance,
                     result.data(),static_cast<uint32_t>(result.size())))
      return emscripten::val::undefined();
    return TypedArrayCopy("Uint8Array", result.data(), result.size());
  }
#endif
  // Batched closest-hit queries for high-to-low texture projection. The
  // returned barycentrics are ordered (w, u, v), matching LightRT's
  // hit-point convention: p = w*a + u*b + v*c.
  bool raycastRaw(const float* org, uint32_t origin_count,
                  const float* dir, uint32_t direction_count,
                  float max_distance, float* distance,
                  int32_t* triangle, float* barycentrics,
                  uint32_t result_cap) const {
    if (!scene_ || !org || !dir || !distance || !triangle ||
        !barycentrics || !origin_count || origin_count != direction_count ||
        origin_count % 3 != 0 || origin_count > (1u << 26) ||
        result_cap < origin_count / 3) return false;
    const size_t ray_count = origin_count / 3;
    std::vector<lrt_ray> rays(ray_count);
    for (size_t i = 0; i < ray_count; i++) {
      const float ox = org[i * 3], oy = org[i * 3 + 1], oz = org[i * 3 + 2];
      const float dx = dir[i * 3], dy = dir[i * 3 + 1], dz = dir[i * 3 + 2];
      lrt_ray &ray = rays[i];
      ray.org[0] = ox; ray.org[1] = oy; ray.org[2] = oz;
      ray.dir[0] = dx; ray.dir[1] = dy; ray.dir[2] = dz;
      ray.tmin = 1.0e-5f; ray.tmax = std::isfinite(max_distance) && max_distance > 0 ? max_distance : 1.0e30f;
    }
    std::vector<lrt_hit> hits(ray_count);
    lrt_tri_intersect1N(scene_, rays.data(), hits.data(), ray_count,
                        LRT_TRI_BATCH_INCOHERENT);
    std::fill(distance, distance + ray_count, 0.0f);
    std::fill(barycentrics, barycentrics + ray_count * 3, 0.0f);
    std::fill(triangle, triangle + ray_count, -1);
    for (size_t i = 0; i < ray_count; i++) {
      const lrt_hit &hit = hits[i];
      if (hit.prim_id == LRT_TRI_NO_HIT || hit.prim_id >= triangles_) continue;
      distance[i] = hit.t; triangle[i] = static_cast<int32_t>(hit.prim_id); barycentrics[i * 3] = 1.0f - hit.u - hit.v; barycentrics[i * 3 + 1] = hit.u; barycentrics[i * 3 + 2] = hit.v;
    }
    return true;
  }
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
  emscripten::val raycast(const emscripten::val &origins,
                          const emscripten::val &directions,
                          float max_distance = 1.0e30f) const {
    const size_t origin_count = origins["length"].as<size_t>();
    if (!origin_count || origin_count != directions["length"].as<size_t>() ||
        origin_count % 3 || origin_count > (1u << 26))
      return emscripten::val::undefined();
    const auto org = CopyArray<float>(origins, "Float32Array");
    const auto dir = CopyArray<float>(directions, "Float32Array");
    const size_t count=org.size()/3;
    std::vector<float> distance(count), barycentrics(count*3);
    std::vector<int32_t> triangle(count);
    if (!raycastRaw(org.data(),static_cast<uint32_t>(org.size()),dir.data(),
                    static_cast<uint32_t>(dir.size()),max_distance,
                    distance.data(),triangle.data(),barycentrics.data(),
                    static_cast<uint32_t>(count)))
      return emscripten::val::undefined();
    emscripten::val result = emscripten::val::object();
    result.set("distance", TypedArrayCopy("Float32Array", distance.data(), distance.size()));
    result.set("triangle", TypedArrayCopy("Int32Array", triangle.data(), triangle.size()));
    result.set("barycentrics", TypedArrayCopy("Float32Array", barycentrics.data(), barycentrics.size()));
    return result;
  }
#endif
  std::string error() const { return error_; }
  const std::string& errorView() const { return error_; }
  double triangleCount() const { return double(triangles_); }
  bool sceneInfo(lightusd_lrt_scene_info* out) const {
    if (!scene_ || !out || out->struct_size < sizeof(*out)) return false;
    const void *nodes=nullptr,*blocks=nullptr;
    uint32_t nc=0,ns=0,bc=0,bs=0,root=0,layout=0,kind=0,point=0;
    if (lrt_tri_scene_raw(scene_,&nodes,&nc,&ns,&blocks,&bc,&bs,&root,
                          &layout,&kind,&point)!=0 || ns%4 || bs%4) return false;
    const uint64_t node_bytes=uint64_t(nc)*ns, block_bytes=uint64_t(bc)*bs;
    if (node_bytes>INT32_MAX || block_bytes>INT32_MAX ||
        normals_.size()>uint32_t(INT32_MAX/4) ||
        colors_.size()>uint32_t(INT32_MAX/4) ||
        vertex_params_.size()>uint32_t(INT32_MAX/4) ||
        material_ids_.size()>uint32_t(INT32_MAX/4) ||
        materials_.size()>uint32_t(INT32_MAX/4)) return false;
    out->root=root; out->node_count=nc; out->block_count=bc;
    out->width=layout==LRT_TRI_LAYOUT_BVH8?8:4;
    out->lengths[0]=static_cast<uint32_t>(node_bytes/4);
    out->lengths[1]=static_cast<uint32_t>(block_bytes/4);
    out->lengths[2]=static_cast<uint32_t>(normals_.size());
    out->lengths[3]=static_cast<uint32_t>(colors_.size());
    out->lengths[4]=static_cast<uint32_t>(vertex_params_.size());
    out->lengths[5]=static_cast<uint32_t>(material_ids_.size());
    out->lengths[6]=static_cast<uint32_t>(materials_.size());
    return true;
  }
  int32_t sceneBuffer(uint8_t kind, uint8_t* out, uint32_t cap) const {
    lightusd_lrt_scene_info info{}; info.struct_size=sizeof(info);
    if (kind>6 || !sceneInfo(&info)) return -1;
    const void* data=nullptr;
    if (kind<=1) {
      const void *nodes=nullptr,*blocks=nullptr;
      uint32_t nc=0,ns=0,bc=0,bs=0,root=0,layout=0,type=0,point=0;
      if (lrt_tri_scene_raw(scene_,&nodes,&nc,&ns,&blocks,&bc,&bs,&root,
                            &layout,&type,&point)!=0) return -1;
      data=kind==0?nodes:blocks;
    } else {
      switch (kind) {
        case 2: data=normals_.data(); break;
        case 3: data=colors_.data(); break;
        case 4: data=vertex_params_.data(); break;
        case 5: data=material_ids_.data(); break;
        case 6: data=materials_.data(); break;
      }
    }
    const uint32_t bytes=info.lengths[kind]*4;
    if (out && cap>=bytes && bytes) std::memcpy(out,data,bytes);
    return static_cast<int32_t>(bytes);
  }
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
  emscripten::val webGPUScene() const {
    if(!scene_) return emscripten::val::undefined();
    const void *nodes=nullptr,*blocks=nullptr; uint32_t nc=0,ns=0,bc=0,bs=0,root=0,layout=0,kind=0,point=0;
    if(lrt_tri_scene_raw(scene_,&nodes,&nc,&ns,&blocks,&bc,&bs,&root,&layout,&kind,&point)!=0) return emscripten::val::undefined();
    emscripten::val o=emscripten::val::object();
    o.set("nodes",TypedArrayCopy("Uint32Array",static_cast<const uint32_t*>(nodes),size_t(nc)*ns/4));
    o.set("blocks",TypedArrayCopy("Uint32Array",static_cast<const uint32_t*>(blocks),size_t(bc)*bs/4));
    o.set("root",root);o.set("nodeCount",nc);o.set("blockCount",bc);o.set("width",layout==LRT_TRI_LAYOUT_BVH8?8:4);
    o.set("normals",TypedArrayCopy("Float32Array",normals_.data(),normals_.size()));
    o.set("colors",TypedArrayCopy("Float32Array",colors_.data(),colors_.size()));
    o.set("vertexParams",TypedArrayCopy("Float32Array",vertex_params_.data(),vertex_params_.size()));
    o.set("materialIds",TypedArrayCopy("Int32Array",material_ids_.data(),material_ids_.size()));
    o.set("materials",TypedArrayCopy("Float32Array",materials_.data(),materials_.size()));
    return o;
  }
#endif
 private:
  lrt_tri_scene *scene_{nullptr}; size_t triangles_{0}; std::string error_;
  std::vector<float> positions_,normals_,colors_,vertex_params_,materials_; std::vector<int32_t> material_ids_;
};

// Error-driven mesh reduction for Lucia's worker-side retopology/LOD path.
// The binding deliberately returns an index buffer that references the input
// vertices; JS can then compact all aligned USD/render attributes together.
int32_t SimplifyMesh(const float* pos, uint32_t pos_count,
                     const uint32_t* idx, uint32_t idx_count,
                     const float* nrm, uint32_t nrm_count,
                     const float* tex, uint32_t tex_count,
                     const uint8_t* lock, uint32_t lock_count,
                     uint32_t target_index_count, float target_error,
                     uint32_t options, uint32_t* out, uint32_t out_cap,
                     float* result_error) {
  if (!pos || !idx || !out || !result_error ||
      (nrm_count && !nrm) || (tex_count && !tex) ||
      (lock_count && !lock) || !pos_count || pos_count % 3 ||
      !idx_count || idx_count % 3 || idx_count > (1u << 27) ||
      out_cap < idx_count || target_index_count < 3 ||
      target_index_count > idx_count || target_index_count % 3 ||
      target_error < 0 || !std::isfinite(target_error)) return -1;
  const size_t vertex_count = pos_count / 3;
  for (uint32_t i = 0; i < idx_count; ++i) if (idx[i] >= vertex_count) return -1;
  if (lock_count && lock_count != vertex_count) return -1;
  const bool with_attributes = nrm_count == vertex_count * 3 &&
                               tex_count == vertex_count * 2;
  const bool with_locks = lock_count != 0;
  size_t count = 0;
  *result_error = 0.0f;
  if (with_attributes || with_locks) {
    const size_t attribute_count = with_attributes ? 5 : 1;
    std::vector<float> attributes(vertex_count * attribute_count, 0.0f);
    if (with_attributes) {
      for (size_t i = 0; i < vertex_count; ++i) {
        attributes[i * 5 + 0] = nrm[i * 3 + 0];
        attributes[i * 5 + 1] = nrm[i * 3 + 1];
        attributes[i * 5 + 2] = nrm[i * 3 + 2];
        attributes[i * 5 + 3] = tex[i * 2 + 0];
        attributes[i * 5 + 4] = tex[i * 2 + 1];
      }
    }
    const float weights[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    count = meshopt_simplifyWithAttributes(out, idx, idx_count, pos,
        vertex_count, sizeof(float) * 3, attributes.data(),
        attribute_count * sizeof(float), weights, attribute_count,
        with_locks ? lock : nullptr, target_index_count, target_error,
        options, result_error);
  } else {
    count = meshopt_simplify(out, idx, idx_count, pos, vertex_count,
        sizeof(float) * 3, target_index_count, target_error, options,
        result_error);
  }
  // Preserve the deterministic cache-friendly triangle order.
  if (count >= 3) meshopt_optimizeVertexCache(out, out, count, vertex_count);
  return static_cast<int32_t>(count);
}

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
class MeshoptSimplifier {
 public:
  emscripten::val simplify(const emscripten::val &positions,
                           const emscripten::val &indices,
                           const emscripten::val &normals,
                           const emscripten::val &uvs,
                           const emscripten::val &locks,
                           size_t target_index_count, float target_error,
                           unsigned int options) const {
    const auto pos = CopyArray<float>(positions, "Float32Array");
    const auto idx = CopyArray<uint32_t>(indices, "Uint32Array");
    const auto nrm = CopyArray<float>(normals, "Float32Array");
    const auto tex = CopyArray<float>(uvs, "Float32Array");
    const auto lock = CopyArray<uint8_t>(locks, "Uint8Array");
    std::vector<uint32_t> out(idx.size());
    float error = 0.0f;
    if (target_index_count > UINT32_MAX) return emscripten::val::undefined();
    const int32_t count = SimplifyMesh(pos.data(), static_cast<uint32_t>(pos.size()),
        idx.data(), static_cast<uint32_t>(idx.size()), nrm.data(),
        static_cast<uint32_t>(nrm.size()), tex.data(),
        static_cast<uint32_t>(tex.size()), lock.data(),
        static_cast<uint32_t>(lock.size()),
        static_cast<uint32_t>(target_index_count), target_error, options,
        out.data(), static_cast<uint32_t>(out.size()), &error);
    if (count < 0) return emscripten::val::undefined();
    emscripten::val result = emscripten::val::object();
    result.set("indices", TypedArrayCopy("Uint32Array", out.data(), count));
    result.set("error", error);
    result.set("sourceVertexCount", pos.size() / 3);
    result.set("vertexCacheOptimized", true);
    return result;
  }
};
#endif
}

#if defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
namespace {
struct TracerSlot {
  LightRTPathTracer* object;
  uint32_t generation;
};
TracerSlot* tracer_slots=nullptr;
uint32_t tracer_slot_count=0;
constexpr uint32_t tracer_index_bits=20;
constexpr uint32_t tracer_index_mask=(1u<<tracer_index_bits)-1u;
constexpr uint32_t tracer_max_generation=(1u<<(32-tracer_index_bits))-1u;
LightRTPathTracer* TracerAt(uint32_t handle) {
  const uint32_t index=handle&tracer_index_mask;
  if (!index || index>tracer_slot_count) return nullptr;
  TracerSlot& slot=tracer_slots[index-1];
  return slot.object && slot.generation==(handle>>tracer_index_bits)
      ? slot.object : nullptr;
}
}

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t lightusd_lrt_create(void) {
  uint32_t index=0;
  while (index<tracer_slot_count &&
         (tracer_slots[index].object || !tracer_slots[index].generation)) ++index;
  if (index==tracer_slot_count) {
    if (tracer_slot_count==tracer_index_mask) return 0;
    void* allocation=std::realloc(tracer_slots,
        (size_t(tracer_slot_count)+1)*sizeof(TracerSlot));
    if (!allocation) return 0;
    tracer_slots=static_cast<TracerSlot*>(allocation);
    tracer_slots[tracer_slot_count++]=TracerSlot{nullptr,1};
  }
  auto* tracer=new (std::nothrow) LightRTPathTracer;
  if (!tracer) return 0;
  tracer_slots[index].object=tracer;
  return (tracer_slots[index].generation<<tracer_index_bits)|(index+1);
}
extern "C" EMSCRIPTEN_KEEPALIVE void lightusd_lrt_destroy(uint32_t handle) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return;
  TracerSlot& slot=tracer_slots[(handle&tracer_index_mask)-1];
  slot.object=nullptr;
  slot.generation=slot.generation==tracer_max_generation?0:slot.generation+1;
  delete tracer;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_clear(uint32_t handle) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return -1;
  tracer->clear();
  return 0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_build(
    uint32_t handle, const float* positions, uint32_t position_count,
    const float* normals, uint32_t normal_count,
    const float* colors, uint32_t color_count,
    const float* vertex_params, uint32_t param_count,
    const int32_t* material_ids, uint32_t id_count,
    const float* materials, uint32_t material_count) {
  auto* tracer=TracerAt(handle);
  constexpr uint32_t max_values=1u<<28;
  if (!tracer || position_count>max_values || normal_count>max_values ||
      color_count>max_values || param_count>max_values || id_count>max_values ||
      material_count>max_values || (position_count&&!positions) ||
      (normal_count&&!normals) || (color_count&&!colors) ||
      (param_count&&!vertex_params) || (id_count&&!material_ids) ||
      (material_count&&!materials)) return -1;
  const uint64_t total_values = uint64_t(position_count) + normal_count +
      color_count + param_count + id_count + material_count;
  if (total_values > (uint64_t(1) << 28)) return -1;
  return tracer->buildRaw(positions,position_count,normals,normal_count,
      colors,color_count,vertex_params,param_count,material_ids,id_count,
      materials,material_count)?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_trace(
    uint32_t handle, const float* inv, uint32_t inv_count,
    const float* camera, uint32_t camera_count, int32_t width,
    int32_t height, int32_t sample_start, int32_t sample_count,
    int32_t max_bounces, float exposure, float* pixels,
    uint32_t pixel_cap) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return -1;
  return tracer->traceRaw(inv,inv_count,camera,camera_count,width,height,
      sample_start,sample_count,max_bounces,exposure,pixels,pixel_cap)?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_occluded(
    uint32_t handle, const float* origins, uint32_t origin_count,
    const float* directions, uint32_t direction_count, float max_distance,
    uint8_t* out, uint32_t out_cap) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return -1;
  return tracer->occludedRaw(origins,origin_count,directions,direction_count,
                             max_distance,out,out_cap)?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_raycast(
    uint32_t handle, const float* origins, uint32_t origin_count,
    const float* directions, uint32_t direction_count, float max_distance,
    float* distance, int32_t* triangle, float* barycentrics,
    uint32_t result_cap) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return -1;
  return tracer->raycastRaw(origins,origin_count,directions,direction_count,
      max_distance,distance,triangle,barycentrics,result_cap)?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_error(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  auto* tracer=TracerAt(handle);
  if (!tracer) return -1;
  const std::string& error=tracer->errorView();
  if (error.size()>INT32_MAX) return -1;
  if (out && cap>=error.size()) std::memcpy(out,error.data(),error.size());
  return static_cast<int32_t>(error.size());
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_triangle_count(
    uint32_t handle) {
  auto* tracer=TracerAt(handle);
  if (!tracer || tracer->triangleCount()>INT32_MAX) return -1;
  return static_cast<int32_t>(tracer->triangleCount());
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_scene_info_get(
    uint32_t handle, lightusd_lrt_scene_info* out) {
  auto* tracer=TracerAt(handle);
  if (!tracer || !out || out->struct_size<sizeof(*out)) return -1;
  return tracer->sceneInfo(out)?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_lrt_scene_buffer(
    uint32_t handle, uint8_t kind, uint8_t* out, uint32_t cap) {
  auto* tracer=TracerAt(handle);
  return tracer?tracer->sceneBuffer(kind,out,cap):-1;
}

extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_meshopt_simplify(
    const float* pos, uint32_t pos_count, const uint32_t* idx,
    uint32_t idx_count, const float* nrm, uint32_t nrm_count,
    const float* tex, uint32_t tex_count, const uint8_t* lock,
    uint32_t lock_count, uint32_t target_index_count, float target_error,
    uint32_t options, uint32_t* out, uint32_t out_cap, float* result_error) {
  return SimplifyMesh(pos, pos_count, idx, idx_count, nrm, nrm_count,
                      tex, tex_count, lock, lock_count, target_index_count,
                      target_error, options, out, out_cap, result_error);
}
#endif

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
EMSCRIPTEN_BINDINGS(lightusd_lightrt_path_tracer) {
  emscripten::class_<LightRTPathTracer>("LightRTPathTracer")
    .constructor<>().function("build",&LightRTPathTracer::build)
    .function("trace",&LightRTPathTracer::trace).function("occluded",&LightRTPathTracer::occluded).function("raycast",&LightRTPathTracer::raycast).function("clear",&LightRTPathTracer::clear)
    .function("error",&LightRTPathTracer::error).function("triangleCount",&LightRTPathTracer::triangleCount)
    .function("webGPUScene",&LightRTPathTracer::webGPUScene);
}
#endif

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
EMSCRIPTEN_BINDINGS(lightusd_meshoptimizer) {
  emscripten::class_<MeshoptSimplifier>("MeshoptSimplifier")
      .constructor<>()
      .function("simplify", &MeshoptSimplifier::simplify);
}
#endif
