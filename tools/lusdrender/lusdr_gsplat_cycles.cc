// SPDX-License-Identifier: Apache-2.0
#include "lusdr_gsplat_cycles.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cmath>
#include <iostream>
#include <thread>

#include "rt_scene_build.hh"
#include "image-writer.hh"
#include "lusdr_math.hh"
#include "tydra/render-converter-internal.hh"

#include "lightrt_c_gsplat.h"
#ifdef HAVE_VULKAN
#include "lightrt_c_vk.h"
#endif
#ifdef HAVE_HIP
#include "lightrt_c_hip.h"
#endif

namespace lusdr {

namespace gs = lightusd::gsplat;

namespace {

// Appends every Gaussian splat field at `time` to `tracer` in traversal order
// (keep_all: one record per authored splat, culled ones as placeholders).
void CollectGSplatFields(const lightusd::Stage &stage, double time,
                         gs::GSplatTracer *tracer, size_t *skipped_prims,
                         std::vector<size_t> *field_counts) {
  stage.Traverse([&](const lightusd::UsdPrim &prim) {
    const std::string &type = prim.GetTypeName();
    if (type != "ParticleField3DGaussianSplat" && type != "ParticleField")
      return true;
    lightusd::tydra::RenderParticleField f;
    if (!lightusd::tydra::ConvertParticleField(stage, prim, time, &f)) {
      std::cerr << "WARN: cannot read splats: " << prim.GetPath().str() << "\n";
      if (skipped_prims) ++*skipped_prims;
      return true;
    }
    if (type != "ParticleField3DGaussianSplat" &&
        f.kernel != "gaussianEllipsoid") {
      if (skipped_prims) ++*skipped_prims;  // surflets are not 3D Gaussians
      return true;
    }
    // USD row-vector matrix (p_w = p * M): column-vector linear part is M^T.
    const auto &M = f.world_matrix;
    const double linear[9] = {M[0][0], M[1][0], M[2][0], M[0][1], M[1][1],
                              M[2][1], M[0][2], M[1][2], M[2][2]};
    const double translation[3] = {M[3][0], M[3][1], M[3][2]};
    const int deg = std::max(0, std::min(3, f.sh_degree));
    const size_t coeffs = size_t(deg + 1) * size_t(deg + 1) * 3;
    const bool have_sh = f.sh_coefficients.size() >= f.count * coeffs;
    tracer->AddField(linear, translation, f.count, f.positions.data(),
                     f.orientations.data(), f.scales.data(), f.opacities.data(),
                     deg, have_sh ? f.sh_coefficients.data() : nullptr,
                     /*keep_all=*/true);
    if (field_counts) field_counts->push_back(f.count);
    return true;
  });
}

lrt_gsplat ToLrt(const gs::GSplatWorld &g, uint32_t id) {
  lrt_gsplat d;
  d.mu[0] = g.mu.x;
  d.mu[1] = g.mu.y;
  d.mu[2] = g.mu.z;
  d.opacity = g.opacity;
  std::memcpy(d.a, g.a, sizeof(d.a));
  d.radius = g.radius;
  d.id = id;
  d.pad = 0;
  return d;
}

}  // namespace

bool BuildGSplatCyclesScene(const lightusd::Stage &stage, double time,
                            GSplatCyclesScene *scene) {
  scene->tracer.Clear();
  scene->bounds = Bounds();
  scene->skipped_prims = 0;
  scene->motion_keys.clear();
  scene->motion_nkeys = 1;
  // Traversal is done by LightRT; the tracer keeps the authored splat order
  // (no Build()) so motion keys and SH stay index-aligned.
  CollectGSplatFields(stage, time, &scene->tracer, &scene->skipped_prims,
                      nullptr);
  if (scene->tracer.empty() ||
      scene->tracer.size() == scene->tracer.skipped())
    return false;
  scene->bounds.lo = {scene->tracer.bounds_lo()[0], scene->tracer.bounds_lo()[1],
                      scene->tracer.bounds_lo()[2]};
  scene->bounds.hi = {scene->tracer.bounds_hi()[0], scene->tracer.bounds_hi()[1],
                      scene->tracer.bounds_hi()[2]};
  scene->bounds.valid = true;
  return true;
}

bool PackGSplatCyclesScene(GSplatCyclesScene* scene, bool srgb,
                          lusdview::GsplatGpuData* out) {
  lusdview::GsplatKeys keys;
  keys.count = scene->tracer.size();
  keys.nkeys = scene->motion_nkeys;
  keys.keys = std::move(scene->motion_keys);
  keys.dc = std::move(scene->motion_dc);
  if (keys.keys.empty()) {
    keys.nkeys = 1;
    keys.keys.reserve(keys.count);
    for (const auto& record : scene->tracer.splats())
      keys.keys.push_back(ToLrt(record, uint32_t(keys.keys.size())));
  }
  keys.center = std::make_shared<gs::GSplatTracer>(std::move(scene->tracer));
  return lusdview::BuildGsplatGpuData(keys, srgb, out);
}

bool BuildGSplatCyclesMotion(const lightusd::Stage &stage,
                             const std::vector<double> &times,
                             GSplatCyclesScene *scene) {
  const size_t n = scene->tracer.size();
  if (times.size() < 2 || n == 0) return false;
  const auto &center = scene->tracer.splats();
  std::vector<lrt_gsplat> keys(times.size() * n);
  std::vector<float> dc(times.size() * n * 3);
  bool moves = false, recolours = false;
  for (size_t k = 0; k < times.size(); ++k) {
    gs::GSplatTracer tk;
    CollectGSplatFields(stage, times[k], &tk, nullptr, nullptr);
    const bool aligned = tk.size() == n;
    if (!aligned)
      std::cerr << "WARN: splat count changes across the shutter; motion blur "
                   "disabled for this step.\n";
    for (size_t i = 0; i < n; ++i) {
      const gs::GSplatWorld &g = aligned ? tk.splats()[i] : center[i];
      keys[k * n + i] = ToLrt(g, uint32_t(i));
      const float *cdc = &scene->tracer.sh_data()[scene->tracer.sh_offset(i)];
      const float *kdc = aligned ? &tk.sh_data()[tk.sh_offset(i)] : cdc;
      for (int c = 0; c < 3; ++c) {
        dc[(k * n + i) * 3 + c] = kdc[c];
        if (kdc[c] != cdc[c]) recolours = true;
      }
      const gs::GSplatWorld &c = center[i];
      // Geometry motion only: Cycles interpolates radiance_base (DC colour
      // and opacity) solely for deforming primitives. The bounding radius
      // follows opacity, and scale changes already show in A.
      if (g.mu.x != c.mu.x || g.mu.y != c.mu.y || g.mu.z != c.mu.z ||
          std::memcmp(g.a, c.a, sizeof(g.a)) != 0)
        moves = true;
    }
  }
  if (!moves) {
    if (recolours)
      std::cerr << "NOTE: splat colour/opacity is animated but the splats do "
                   "not move; like Cycles, the frame-time value is used.\n";
    return false;
  }
  scene->motion_keys.swap(keys);
  scene->motion_nkeys = uint32_t(times.size());
  if (recolours) scene->motion_dc.swap(dc);
  return true;
}

void GSplatDcAt(const std::vector<float> &dc, uint32_t nkeys, size_t n,
                size_t i, float tau, float out[3]) {
  float f = std::min(std::max(tau, 0.0f), 1.0f) * float(nkeys - 1);
  uint32_t step = std::min(uint32_t(f), nkeys - 2);
  const float t = f - float(step);
  for (int c = 0; c < 3; ++c)
    out[c] = dc[(step * n + i) * 3 + c] * (1.0f - t) +
             dc[((step + 1) * n + i) * 3 + c] * t;
}

CameraFrame LerpCameraFrame(const std::vector<CameraFrame> &keys, float tau) {
  if (keys.size() < 2) return keys.empty() ? CameraFrame() : keys[0];
  const uint32_t K = uint32_t(keys.size());
  float f = std::min(std::max(tau, 0.0f), 1.0f) * float(K - 1);
  uint32_t step = std::min(uint32_t(f), K - 2);
  const float t = f - float(step);
  const CameraFrame &a = keys[step], &b = keys[step + 1];
  CameraFrame c = a;
  auto lerp = [&](const Vec3 &x, const Vec3 &y) {
    return Vec3{x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t};
  };
  c.origin = lerp(a.origin, b.origin);
  c.forward = Normalize(lerp(a.forward, b.forward));
  c.up = Normalize(lerp(a.up, b.up));
  c.right = Normalize(lerp(a.right, b.right));
  return c;
}

bool BuildCameraMotion(const lightusd::Stage &stage, const std::string &camera,
                       const std::vector<double> &times, GSplatCyclesScene *scene) {
  if (camera.empty() || times.size() < 2) return false;
  std::vector<CameraFrame> keys(times.size());
  bool moves = false;
  for (size_t k = 0; k < times.size(); ++k) {
    float aspect = 1.0f;
    if (!FindCoreCameraFrame(stage, camera, times[k], &keys[k], &aspect))
      return false;
    const CameraFrame &a = keys[0], &b = keys[k];
    if (a.origin.x != b.origin.x || a.origin.y != b.origin.y ||
        a.origin.z != b.origin.z || a.forward.x != b.forward.x ||
        a.forward.y != b.forward.y || a.forward.z != b.forward.z ||
        a.up.x != b.up.x || a.up.y != b.up.y || a.up.z != b.up.z)
      moves = true;
  }
  if (!moves) return false;
  scene->camera_keys.swap(keys);
  return true;
}

bool AttachGSplatCyclesToDirect(const lightusd::Stage &stage, double time,
                                bool srgb, DirectScene *direct,
                                const std::string &camera, int motion_steps) {
  auto scene = std::make_shared<GSplatCyclesScene>();
  // Shutter keys (same sampling as the splat-only renderer). The camera is
  // blurred even when the stage has no splats.
  std::vector<double> times;
  CameraFrame frame;
  float aspect = 1.0f;
  if (motion_steps > 1 && !camera.empty() &&
      FindCoreCameraFrame(stage, camera, time, &frame, &aspect) &&
      frame.shutter_close > frame.shutter_open) {
    times.resize(static_cast<size_t>(motion_steps));
    for (size_t k = 0; k < times.size(); ++k)
      times[k] = time + double(frame.shutter_open) +
                 double(frame.shutter_close - frame.shutter_open) * double(k) /
                     double(times.size() - 1);
    if (BuildCameraMotion(stage, camera, times, scene.get())) {
      direct->camera_keys = scene->camera_keys;
      direct->gsplat_motion = true;
      std::cerr << "Cycles camera motion blur: " << times.size() << " keys\n";
    }
  }
  if (!BuildGSplatCyclesScene(stage, time, scene.get())) return false;
  if (!times.empty()) BuildGSplatCyclesMotion(stage, times, scene.get());
  const auto &src = scene->tracer.splats();
  const uint32_t nkeys = scene->motion_nkeys;
  std::vector<lrt_gsplat> splats;
  if (nkeys == 1) {
    splats.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) splats[i] = ToLrt(src[i], uint32_t(i));
  }
  const std::vector<lrt_gsplat> &keys = nkeys > 1 ? scene->motion_keys : splats;
  lrt_result err = LRT_RESULT_OK;
  lrt_gsplat_scene *ls = lrt_gsplat_scene_build_motion(keys.data(), src.size(),
                                                       nkeys, nullptr, &err);
  if (!ls) return false;
  direct->gsplat_scene.reset(ls, lrt_gsplat_scene_free);
  // Share the tracer (SH + per-field transforms) owned by the scene object.
  direct->gsplat_tracer = std::shared_ptr<gs::GSplatTracer>(scene, &scene->tracer);
  direct->gsplat_srgb = srgb;
  direct->gsplat_nkeys = nkeys;
  if (!scene->motion_dc.empty())
    direct->gsplat_dc = std::shared_ptr<const std::vector<float>>(scene, &scene->motion_dc);
  direct->gsplat_motion = nkeys > 1 || direct->gsplat_dc || direct->camera_keys.size() > 1;
  std::cerr << "Cycles gsplat kernel (mixed scene, CPU integrator): "
            << src.size() << " splat(s)";
  if (nkeys > 1) std::cerr << ", " << nkeys << " motion keys";
  if (direct->gsplat_dc) std::cerr << ", animated colour";
  if (direct->camera_keys.size() > 1) std::cerr << ", camera motion";
  std::cerr << "\n";
  return true;
}

bool StageHasNonSplatGeometry(const lightusd::Stage &stage) {
  static const char *const kTypes[] = {
      "Mesh", "Sphere", "Cube", "Cylinder", "Cylinder_1", "Capsule",
      "Capsule_1", "Cone", "Plane", "BasisCurves", "HermiteCurves",
      "NurbsCurves", "NurbsPatch", "Points", "TetMesh", "PointInstancer"};
  bool found = false;
  stage.Traverse([&](const lightusd::UsdPrim &prim) {
    const std::string &t = prim.GetTypeName();
    for (const char *k : kTypes)
      if (t == k) {
        found = true;
        return false;
      }
    return true;
  });
  return found;
}

namespace {

// Trace backend for the splat rays: LightRT CPU, Vulkan compute or HIP. GPU
// backends stay within `budget` bytes of device memory (chunked upload).
class SplatBackend {
 public:
  ~SplatBackend() {
#ifdef HAVE_VULKAN
    if (vk_) lrt_vk_engine_destroy(vk_);
#endif
#ifdef HAVE_HIP
    if (hip_) lrt_hip_engine_destroy(hip_);
#endif
    lrt_gsplat_scene_free(scene_);
  }

  bool Init(const Options &opt, const GSplatCyclesScene &scene) {
    budget_ = uint64_t((opt.max_vram_gib > 0.0 ? opt.max_vram_gib : 4.0) *
                       double(1ull << 30));
    const bool want_vk = opt.vulkan || opt.vulkan_rt;
    const bool want_hip = opt.hip;
    if (opt.use_d3d || opt.cuda)
      std::cerr << "WARN: --gsplat-kernel cycles supports -vk/-vkr and -hip "
                   "GPU backends; tracing on the CPU.\n";
#ifdef HAVE_VULKAN
    if (want_vk) {
      lrt_vk_engine_options vopts;
      std::memset(&vopts, 0, sizeof(vopts));
      vopts.device_index = opt.vulkan_device;
      vopts.prefer_discrete = 1;
      lrt_result err = LRT_RESULT_OK;
      vk_ = lrt_vk_engine_create(&vopts, &err);
      if (vk_) name_ = std::string("Vulkan ") + lrt_vk_engine_device_name(vk_);
      else std::cerr << "WARN: Vulkan unavailable (err=" << int(err)
                     << "); tracing splats on the CPU.\n";
    }
#else
    if (want_vk)
      std::cerr << "WARN: built without Vulkan; tracing splats on the CPU.\n";
#endif
#ifdef HAVE_HIP
    if (want_hip && !vk_) {
      lrt_hip_engine_options hopts;
      std::memset(&hopts, 0, sizeof(hopts));
      hopts.device_index = -1;
      lrt_result err = LRT_RESULT_OK;
      hip_ = lrt_hip_engine_create(&hopts, &err);
      if (hip_) name_ = std::string("HIP ") + lrt_hip_engine_device_name(hip_);
      else std::cerr << "WARN: HIP unavailable (err=" << int(err)
                     << "); tracing splats on the CPU.\n";
    }
#else
    if (want_hip)
      std::cerr << "WARN: built without HIP; tracing splats on the CPU.\n";
#endif
    const auto &src = scene.tracer.splats();
    std::vector<lrt_gsplat> splats;
    const uint32_t nkeys =
        scene.motion_nkeys > 1 &&
                scene.motion_keys.size() == size_t(scene.motion_nkeys) * src.size()
            ? scene.motion_nkeys
            : 1u;
    if (nkeys == 1) {
      splats.resize(src.size());
      for (size_t i = 0; i < src.size(); ++i) splats[i] = ToLrt(src[i], uint32_t(i));
    }
    const std::vector<lrt_gsplat> &keys = nkeys > 1 ? scene.motion_keys : splats;
    lrt_gsplat_build_options bo;
    std::memset(&bo, 0, sizeof(bo));
    if (gpu()) bo.max_chunk_splats = lrt_gsplat_chunk_splats_for_budget(
                   budget_, kGpuBatch) / nkeys;
    lrt_result err = LRT_RESULT_OK;
    scene_ = lrt_gsplat_scene_build_motion(keys.data(), src.size(), nkeys, &bo,
                                           &err);
    nkeys_ = nkeys;
    if (!scene_) {
      std::cerr << "LightRT gsplat build failed (err=" << int(err) << ").\n";
      return false;
    }
    return true;
  }

  bool gpu() const { return vk_ || hip_; }
  uint32_t batch() const { return gpu() ? kGpuBatch : kCpuBatch; }
  const std::string &name() const { return name_; }
  size_t chunks() const { return lrt_gsplat_scene_chunk_count(scene_); }
  uint64_t budget() const { return budget_; }

  // Falls back to the CPU permanently if the GPU trace fails.
  bool motion() const { return nkeys_ > 1; }
  uint32_t nkeys() const { return nkeys_; }

  bool Trace(const std::vector<lrt_ray> &rays, const float *times,
             uint32_t seed_base, std::vector<lrt_hit> *hits) {
    hits->resize(rays.size());
    const uint32_t n = uint32_t(rays.size());
    lrt_result err = LRT_RESULT_OK;
#ifdef HAVE_VULKAN
    if (vk_) {
      if (lrt_vk_gsplat_trace_time(vk_, scene_, rays.data(), times, n,
                                   seed_base, budget_, hits->data(), &err) >= 0)
        return true;
      std::cerr << "WARN: Vulkan gsplat trace failed (err=" << int(err)
                << "): " << lrt_vk_engine_last_error(vk_)
                << "; continuing on the CPU.\n";
      lrt_vk_engine_destroy(vk_);
      vk_ = nullptr;
      name_ = "CPU";
    }
#endif
#ifdef HAVE_HIP
    if (hip_) {
      if (lrt_hip_gsplat_trace_time(hip_, scene_, rays.data(), times, n,
                                    seed_base, budget_, hits->data(), &err) >= 0)
        return true;
      std::cerr << "WARN: HIP gsplat trace failed (err=" << int(err)
                << "): " << lrt_hip_engine_last_error(hip_)
                << "; continuing on the CPU.\n";
      lrt_hip_engine_destroy(hip_);
      hip_ = nullptr;
      name_ = "CPU";
    }
#endif
    lrt_gsplat_intersect1N_time(scene_, rays.data(), times, n, seed_base,
                                hits->data());
    return true;
  }

 private:
  static constexpr uint32_t kGpuBatch = 8u << 20;
  static constexpr uint32_t kCpuBatch = 1u << 20;
#ifdef HAVE_VULKAN
  lrt_vk_engine *vk_ = nullptr;
#else
  void *vk_ = nullptr;
#endif
#ifdef HAVE_HIP
  lrt_hip_engine *hip_ = nullptr;
#else
  void *hip_ = nullptr;
#endif
  lrt_gsplat_scene *scene_ = nullptr;
  uint32_t nkeys_ = 1;
  uint64_t budget_ = 0;
  std::string name_ = "CPU";
};

}  // namespace

bool RenderGSplatCycles(const Options &opt, const GSplatCyclesScene &scene,
                        const CameraFrame &camera, int height) {
  const int w = std::max(1, opt.width > 0 ? opt.width : 960);
  const int h = std::max(1, height);
  const int spp = opt.samples > 1 ? opt.samples : 16;
  const float aspect = float(w) / float(h);
  const gs::GSplatColorSpace cs =
      opt.gsplat_srgb ? gs::GSplatColorSpace::SRGB : gs::GSplatColorSpace::Linear;
  SplatBackend backend;
  const auto t0 = std::chrono::steady_clock::now();
  if (!backend.Init(opt, scene)) return false;
  const auto t1 = std::chrono::steady_clock::now();
  std::vector<float> rgb(size_t(w) * size_t(h) * 3, 0.f);
  // Rays are generated in (y*w + x)*spp + s order; that index is the seed,
  // so every backend (and the batch size) gives the same stochastic choices.
  const int rows = std::max(1, int(backend.batch() / (uint32_t(w) * uint32_t(spp))));
  std::vector<lrt_ray> rays;
  std::vector<float> times;
  // Ray times are needed for moving splats, animated colour or a moving camera.
  const bool timed = backend.motion() || !scene.motion_dc.empty() ||
                     scene.camera_keys.size() > 1;
  std::vector<lrt_hit> hits;
  const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
  auto parallel_rows = [&](int y0, int y1, auto &&fn) {
    std::atomic<int> next{y0};
    auto work = [&]() {
      for (int y; (y = next.fetch_add(1)) < y1;) fn(y);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
    work();
    for (auto &t : pool) t.join();
  };
  for (int y0 = 0; y0 < h; y0 += rows) {
    const int y1 = std::min(h, y0 + rows);
    const size_t base = size_t(y0) * size_t(w) * size_t(spp);
    rays.resize(size_t(y1 - y0) * size_t(w) * size_t(spp));
    if (timed) times.resize(rays.size());
    parallel_rows(y0, y1, [&](int y) {
      for (int x = 0; x < w; ++x)
        for (int s = 0; s < spp; ++s) {
          float jx, jy;
          PixelJitter(s, spp, &jx, &jy);
          float tau = 0.5f;
          if (timed) {
            // Stratified shutter time per pixel (Cramer-Patterson rotation).
            const uint32_t px = uint32_t(y) * uint32_t(w) + uint32_t(x);
            uint32_t hash = px * 0x9e3779b1u;
            hash ^= hash >> 16;
            hash *= 0x85ebca6bu;
            hash ^= hash >> 13;
            const float rot = float(hash >> 8) * (1.f / 16777216.f);
            const float tm = (float(s) + 0.5f) / float(spp) + rot;
            tau = tm - std::floor(tm);
            times[(size_t(y) * size_t(w) + size_t(x)) * size_t(spp) +
                  size_t(s) - base] = tau;
          }
          const CameraFrame cam = scene.camera_keys.size() > 1
                                      ? LerpCameraFrame(scene.camera_keys, tau)
                                      : camera;
          Vec3 org, dir;
          MakeRay(cam, aspect, (float(x) + jx) / float(w),
                  (float(y) + jy) / float(h), &org, &dir);
          const float cosine =
              cam.ortho ? 1.f : std::max(1e-6f, Dot(cam.forward, dir));
          lrt_ray &r = rays[(size_t(y) * size_t(w) + size_t(x)) * size_t(spp) +
                            size_t(s) - base];
          r.org[0] = org.x;
          r.org[1] = org.y;
          r.org[2] = org.z;
          r.dir[0] = dir.x;
          r.dir[1] = dir.y;
          r.dir[2] = dir.z;
          r.tmin = cam.znear / cosine;
          r.tmax = cam.zfar / cosine;
        }
    });
    if (!backend.Trace(rays, timed ? times.data() : nullptr,
                       uint32_t(base), &hits))
      return false;
    parallel_rows(y0, y1, [&](int y) {
      for (int x = 0; x < w; ++x) {
        gs::Vec3 sum = {0, 0, 0};
        const size_t r0 = (size_t(y) * size_t(w) + size_t(x)) * size_t(spp) - base;
        for (int s = 0; s < spp; ++s) {
          const lrt_hit &hit = hits[r0 + size_t(s)];
          if (hit.prim_id != LRT_TRI_NO_HIT) {
            const lrt_ray &r = rays[r0 + size_t(s)];
            gs::GSplatTracer::Hit th;
            th.t = hit.t;
            th.index = hit.prim_id;
            if (!scene.motion_dc.empty()) {
              float dcv[3];
              GSplatDcAt(scene.motion_dc, scene.motion_nkeys, scene.tracer.size(),
                         hit.prim_id, times[r0 + size_t(s)], dcv);
              sum = gs::Add(sum, scene.tracer.Radiance(
                                     th, {r.dir[0], r.dir[1], r.dir[2]}, cs, dcv));
            } else {
              sum = gs::Add(sum, scene.tracer.Radiance(
                                     th, {r.dir[0], r.dir[1], r.dir[2]}, cs));
            }
          } else {
            sum = gs::Add(sum, {opt.bg.x, opt.bg.y, opt.bg.z});
          }
        }
        float *px = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        px[0] = sum.x / float(spp);
        px[1] = sum.y / float(spp);
        px[2] = sum.z / float(spp);
      }
    });
  }
  const auto t2 = std::chrono::steady_clock::now();
  if (opt.stats) {
    const double bms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double rms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cerr << "Cycles gsplat kernel (LightRT, " << backend.name() << "): "
              << scene.tracer.size() << " splat(s) in " << backend.chunks()
              << " chunk(s)";
    if (backend.motion()) std::cerr << ", " << backend.nkeys() << " motion keys";
    if (backend.gpu())
      std::cerr << ", device budget "
                << double(backend.budget()) / double(1ull << 30) << " GiB";
    std::cerr << ", build " << bms << " ms, trace+shade " << rms << " ms, "
              << spp << " spp\n";
  }

  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = 4;
  std::string ext = opt.output.size() > 4
                        ? opt.output.substr(opt.output.size() - 4)
                        : std::string();
  for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".exr") {
    // Scene-linear float output (no display transform), like Blender's EXR.
    img.bpp = 32;
    img.format = lightusd::Image::PixelFormat::Float;
    img.data.resize(size_t(w) * size_t(h) * 4 * sizeof(float));
    float *dst = reinterpret_cast<float *>(img.data.data());
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
      for (int c = 0; c < 3; ++c) dst[i * 4 + size_t(c)] = rgb[i * 3 + size_t(c)];
      dst[i * 4 + 3] = 1.0f;
    }
  } else {
    img.bpp = 8;
    img.format = lightusd::Image::PixelFormat::UInt;
    img.data.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
      for (int c = 0; c < 3; ++c)
        img.data[i * 4 + size_t(c)] = ToSRGB8(rgb[i * 3 + size_t(c)], true);
      img.data[i * 4 + 3] = 255;
    }
  }
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return false;
  }
  if (opt.stats)
    std::cerr << "Cycles gsplat: skipped splats " << scene.tracer.skipped()
              << ", skipped prims " << scene.skipped_prims << "\n";
  return true;
}

}  // namespace lusdr
