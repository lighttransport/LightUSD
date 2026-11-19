// SPDX-License-Identifier: Apache-2.0
#include "lusdr_gsplat_cycles.hh"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <iostream>
#include <thread>

#include "image-writer.hh"
#include "lusdr_math.hh"
#include "tydra/render-converter-internal.hh"

namespace lusdr {

namespace gs = lightusd::gsplat;

bool BuildGSplatCyclesScene(const lightusd::Stage &stage, double time,
                            GSplatCyclesScene *scene) {
  scene->tracer.Clear();
  scene->bounds = Bounds();
  scene->skipped_prims = 0;
  stage.Traverse([&](const lightusd::UsdPrim &prim) {
    const std::string &type = prim.GetTypeName();
    if (type != "ParticleField3DGaussianSplat" && type != "ParticleField")
      return true;
    lightusd::tydra::RenderParticleField f;
    if (!lightusd::tydra::ConvertParticleField(stage, prim, time, &f)) {
      std::cerr << "WARN: cannot read splats: " << prim.GetPath().str() << "\n";
      ++scene->skipped_prims;
      return true;
    }
    if (type != "ParticleField3DGaussianSplat" &&
        f.kernel != "gaussianEllipsoid") {
      ++scene->skipped_prims;  // surflet kernels are not 3D Gaussians
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
    if (!scene->tracer.AddField(linear, translation, f.count,
                                f.positions.data(), f.orientations.data(),
                                f.scales.data(), f.opacities.data(), deg,
                                have_sh ? f.sh_coefficients.data() : nullptr))
      ++scene->skipped_prims;
    return true;
  });
  if (scene->tracer.empty()) return false;
  scene->tracer.Build();
  scene->bounds.lo = {scene->tracer.bounds_lo()[0], scene->tracer.bounds_lo()[1],
                      scene->tracer.bounds_lo()[2]};
  scene->bounds.hi = {scene->tracer.bounds_hi()[0], scene->tracer.bounds_hi()[1],
                      scene->tracer.bounds_hi()[2]};
  scene->bounds.valid = true;
  return true;
}

bool RenderGSplatCycles(const Options &opt, const GSplatCyclesScene &scene,
                        const CameraFrame &camera, int height) {
  const int w = std::max(1, opt.width > 0 ? opt.width : 960);
  const int h = std::max(1, height);
  const int spp = opt.samples > 1 ? opt.samples : 16;
  const float aspect = float(w) / float(h);
  const gs::GSplatColorSpace cs =
      opt.gsplat_srgb ? gs::GSplatColorSpace::SRGB : gs::GSplatColorSpace::Linear;
  std::vector<float> rgb(size_t(w) * size_t(h) * 3, 0.f);
  std::atomic<int> next_row{0};
  auto worker = [&]() {
    for (int y; (y = next_row.fetch_add(1)) < h;) {
      for (int x = 0; x < w; ++x) {
        gs::Vec3 sum = {0, 0, 0};
        for (int s = 0; s < spp; ++s) {
          float jx, jy;
          PixelJitter(s, spp, &jx, &jy);
          Vec3 org, dir;
          MakeRay(camera, aspect, (float(x) + jx) / float(w),
                  (float(y) + jy) / float(h), &org, &dir);
          const float cosine =
              camera.ortho ? 1.f : std::max(1e-6f, Dot(camera.forward, dir));
          const gs::Vec3 ro = {org.x, org.y, org.z}, rd = {dir.x, dir.y, dir.z};
          const uint32_t seed =
              uint32_t((size_t(y) * size_t(w) + size_t(x)) * size_t(spp) + s);
          gs::GSplatTracer::Hit hit;
          if (scene.tracer.Trace(ro, rd, camera.znear / cosine,
                                 camera.zfar / cosine, seed, &hit))
            sum = gs::Add(sum, scene.tracer.Radiance(hit, rd, cs));
          else
            sum = gs::Add(sum, {opt.bg.x, opt.bg.y, opt.bg.z});
        }
        float *px = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        px[0] = sum.x / float(spp);
        px[1] = sum.y / float(spp);
        px[2] = sum.z / float(spp);
      }
    }
  };
  const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < nt; ++t) pool.emplace_back(worker);
  worker();
  for (auto &t : pool) t.join();

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
    std::cerr << "Cycles gsplat kernel: " << scene.tracer.size()
              << " splat(s), " << scene.tracer.node_count()
              << " BVH node(s), " << spp << " spp, skipped splats "
              << scene.tracer.skipped() << ", skipped prims "
              << scene.skipped_prims << "\n";
  return true;
}

}  // namespace lusdr
