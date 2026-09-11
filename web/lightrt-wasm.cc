#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "external/lightrt/lightrt_c_tri.h"
#include "external/meshoptimizer/meshoptimizer.h"

namespace {

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

template <typename T>
emscripten::val TypedArrayCopy(const char *ctor, const T *data, size_t n) {
  emscripten::val out=emscripten::val::global(ctor).new_(n);
  if(n) out.call<void>("set",emscripten::val(emscripten::typed_memory_view(n,data)));
  return out;
}

class LightRTPathTracer {
 public:
  ~LightRTPathTracer() { clear(); }
  void clear() { if (scene_) lrt_tri_scene_free(scene_); scene_=nullptr; positions_.clear(); normals_.clear(); colors_.clear(); vertex_params_.clear(); material_ids_.clear(); materials_.clear(); }

  bool build(const emscripten::val &positions, const emscripten::val &normals,
             const emscripten::val &colors, const emscripten::val &vertex_params,
             const emscripten::val &material_ids, const emscripten::val &materials) {
    clear(); error_.clear();
    positions_=CopyArray<float>(positions,"Float32Array");
    normals_=CopyArray<float>(normals,"Float32Array");
    colors_=CopyArray<float>(colors,"Float32Array");
    vertex_params_=CopyArray<float>(vertex_params,"Float32Array");
    material_ids_=CopyArray<int32_t>(material_ids,"Int32Array");
    materials_=CopyArray<float>(materials,"Float32Array");
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

  emscripten::val trace(const emscripten::val &inv_view_projection,
                        const emscripten::val &camera_position, int width, int height,
                        int sample_start, int sample_count, int max_bounces,
                        float exposure) {
    if (!scene_ || width<1 || height<1 || width>4096 || height>4096) return emscripten::val::undefined();
    auto inv=CopyArray<float>(inv_view_projection,"Float32Array");
    auto cp=CopyArray<float>(camera_position,"Float32Array");
    if (inv.size()!=16 || cp.size()!=3) return emscripten::val::undefined();
    sample_count=std::clamp(sample_count,1,64); max_bounces=std::clamp(max_bounces,1,8);
    std::vector<float> pixels(size_t(width)*size_t(height)*4,0.0f);
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
    emscripten::val out = emscripten::val::global("Float32Array").new_(pixels.size());
    out.call<void>("set", emscripten::val(emscripten::typed_memory_view(pixels.size(),pixels.data())));
    return out;
  }
  // Batched visibility queries for UV-space baking. Origins and directions
  // are xyz-packed object-space arrays; the returned bytes are 1 for an
  // occluded ray and 0 for a miss. Keeping this operation batched avoids a
  // JS/WASM call for every texel while reusing the resident LightRT BVH.
  emscripten::val occluded(const emscripten::val &origins,
                           const emscripten::val &directions,
                           float max_distance = 1.0e30f) const {
    if (!scene_) return emscripten::val::undefined();
    const size_t origin_count = origins["length"].as<size_t>();
    const size_t direction_count = directions["length"].as<size_t>();
    if (!origin_count || origin_count != direction_count || origin_count % 3 != 0 || origin_count > (size_t(1) << 26)) return emscripten::val::undefined();
    const auto org = CopyArray<float>(origins, "Float32Array"), dir = CopyArray<float>(directions, "Float32Array");
    const size_t ray_count = origin_count / 3;
    std::vector<lrt_ray> rays(ray_count);
    for (size_t i = 0; i < ray_count; i++) {
      lrt_ray &ray = rays[i];
      ray.org[0] = org[i * 3]; ray.org[1] = org[i * 3 + 1]; ray.org[2] = org[i * 3 + 2];
      ray.dir[0] = dir[i * 3]; ray.dir[1] = dir[i * 3 + 1]; ray.dir[2] = dir[i * 3 + 2];
      ray.tmin = 1.0e-5f; ray.tmax = std::isfinite(max_distance) && max_distance > 0 ? max_distance : 1.0e30f;
    }
    std::vector<uint8_t> result(ray_count, 0);
    lrt_tri_occluded1N(scene_, rays.data(), result.data(), ray_count,
                       LRT_TRI_BATCH_INCOHERENT);
    return TypedArrayCopy("Uint8Array", result.data(), result.size());
  }
  // Batched closest-hit queries for high-to-low texture projection. The
  // returned barycentrics are ordered (w, u, v), matching LightRT's
  // hit-point convention: p = w*a + u*b + v*c.
  emscripten::val raycast(const emscripten::val &origins,
                          const emscripten::val &directions,
                          float max_distance = 1.0e30f) const {
    if (!scene_) return emscripten::val::undefined();
    const size_t origin_count = origins["length"].as<size_t>();
    const size_t direction_count = directions["length"].as<size_t>();
    if (!origin_count || origin_count != direction_count || origin_count % 3 != 0 || origin_count > (size_t(1) << 26)) return emscripten::val::undefined();
    const auto org = CopyArray<float>(origins, "Float32Array"), dir = CopyArray<float>(directions, "Float32Array");
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
    std::vector<float> distance(ray_count, 0.0f), barycentrics(ray_count * 3, 0.0f);
    std::vector<int32_t> triangle(ray_count, -1);
    for (size_t i = 0; i < ray_count; i++) {
      const lrt_hit &hit = hits[i];
      if (hit.prim_id == LRT_TRI_NO_HIT || hit.prim_id >= triangles_) continue;
      distance[i] = hit.t; triangle[i] = static_cast<int32_t>(hit.prim_id); barycentrics[i * 3] = 1.0f - hit.u - hit.v; barycentrics[i * 3 + 1] = hit.u; barycentrics[i * 3 + 2] = hit.v;
    }
    emscripten::val result = emscripten::val::object();
    result.set("distance", TypedArrayCopy("Float32Array", distance.data(), distance.size()));
    result.set("triangle", TypedArrayCopy("Int32Array", triangle.data(), triangle.size()));
    result.set("barycentrics", TypedArrayCopy("Float32Array", barycentrics.data(), barycentrics.size()));
    return result;
  }
  std::string error() const { return error_; }
  double triangleCount() const { return double(triangles_); }
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
 private:
  lrt_tri_scene *scene_{nullptr}; size_t triangles_{0}; std::string error_;
  std::vector<float> positions_,normals_,colors_,vertex_params_,materials_; std::vector<int32_t> material_ids_;
};

// Error-driven mesh reduction for Lucia's worker-side retopology/LOD path.
// The binding deliberately returns an index buffer that references the input
// vertices; JS can then compact all aligned USD/render attributes together.
class MeshoptSimplifier {
 public:
  emscripten::val simplify(const emscripten::val &positions,
                           const emscripten::val &indices,
                           const emscripten::val &normals,
                           const emscripten::val &uvs,
                           const emscripten::val &locks,
                           size_t target_index_count, float target_error,
                           unsigned int options) const {
    const std::vector<float> pos = CopyArray<float>(positions, "Float32Array");
    const std::vector<uint32_t> idx = CopyArray<uint32_t>(indices, "Uint32Array");
    const std::vector<float> nrm = CopyArray<float>(normals, "Float32Array");
    const std::vector<float> tex = CopyArray<float>(uvs, "Float32Array");
    const std::vector<uint8_t> lock = CopyArray<uint8_t>(locks, "Uint8Array");
    const size_t vertex_count = pos.size() / 3;
    if (!vertex_count || pos.size() % 3 || idx.empty() || idx.size() % 3 ||
        idx.size() > (size_t(1) << 27) || target_index_count < 3 ||
        target_index_count > idx.size() || target_index_count % 3 ||
        target_error < 0 || !std::isfinite(target_error)) {
      return emscripten::val::undefined();
    }
    for (uint32_t i : idx) if (i >= vertex_count) return emscripten::val::undefined();
    if (!lock.empty() && lock.size() != vertex_count) return emscripten::val::undefined();
    const bool with_attributes = nrm.size() == vertex_count * 3 &&
                                 tex.size() == vertex_count * 2;
    const bool with_locks = !lock.empty();
    std::vector<unsigned int> out(idx.size());
    float result_error = 0.0f;
    size_t count = 0;
    if (with_attributes || with_locks) {
      const size_t attribute_count = with_attributes ? 5 : 1;
      const size_t attribute_stride = attribute_count * sizeof(float);
      std::vector<float> attributes(vertex_count * attribute_count, 0.0f);
      for (size_t i = 0; i < vertex_count; ++i) {
        if (with_attributes) {
          attributes[i * 5 + 0] = nrm[i * 3 + 0];
          attributes[i * 5 + 1] = nrm[i * 3 + 1];
          attributes[i * 5 + 2] = nrm[i * 3 + 2];
          attributes[i * 5 + 3] = tex[i * 2 + 0];
          attributes[i * 5 + 4] = tex[i * 2 + 1];
        }
      }
      const float weights[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
      count = meshopt_simplifyWithAttributes(out.data(), idx.data(), idx.size(),
          pos.data(), vertex_count, sizeof(float) * 3, attributes.data(),
          attribute_stride, weights, attribute_count, lock.empty() ? nullptr : lock.data(), target_index_count,
          target_error, options, &result_error);
    } else {
      count = meshopt_simplify(out.data(), idx.data(), idx.size(), pos.data(),
          vertex_count, sizeof(float) * 3, target_index_count, target_error,
          options, &result_error);
    }
    // Keep the reduced triangle order friendly to the post-transform cache.
    // The JS worker compacts vertices in this final order, which also gives a
    // deterministic vertex-fetch order without copying arbitrary attributes
    // through this ABI.
    if (count >= 3) meshopt_optimizeVertexCache(out.data(), out.data(), count, vertex_count);
    emscripten::val result = emscripten::val::object();
    result.set("indices", TypedArrayCopy("Uint32Array", out.data(), count));
    result.set("error", result_error);
    result.set("sourceVertexCount", vertex_count);
    result.set("vertexCacheOptimized", true);
    return result;
  }
};
}

EMSCRIPTEN_BINDINGS(lightusd_lightrt_path_tracer) {
  emscripten::class_<LightRTPathTracer>("LightRTPathTracer")
    .constructor<>().function("build",&LightRTPathTracer::build)
    .function("trace",&LightRTPathTracer::trace).function("occluded",&LightRTPathTracer::occluded).function("raycast",&LightRTPathTracer::raycast).function("clear",&LightRTPathTracer::clear)
    .function("error",&LightRTPathTracer::error).function("triangleCount",&LightRTPathTracer::triangleCount)
    .function("webGPUScene",&LightRTPathTracer::webGPUScene);
}

EMSCRIPTEN_BINDINGS(lightusd_meshoptimizer) {
  emscripten::class_<MeshoptSimplifier>("MeshoptSimplifier")
      .constructor<>()
      .function("simplify", &MeshoptSimplifier::simplify);
}
