// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "external/tinyvdb/tinyvdb_io.h"
#include "external/tinyvdb/tinyvdb_sparse_tree.h"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "tydra/next/scene-preview-compute.hh"
namespace td = lightusd::tydra::next;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "cpu";
  const auto backend = name == "cuda"     ? td::PreviewComputeBackend::Cuda
                       : name == "vulkan" ? td::PreviewComputeBackend::Vulkan
                                          : td::PreviewComputeBackend::Cpu;
  constexpr uint64_t budget = uint64_t(512) << 20;
  std::string error;
  auto reference =
      td::CreatePreviewCompute(td::PreviewComputeBackend::Cpu, budget, &error);
  auto engine = td::CreatePreviewCompute(backend, budget, &error);
  auto require = [&](bool ok, const std::string& what) {
    if (!ok) std::cerr << "FAIL: " << what << " " << error << "\n";
    return ok;
  };
  if (!require(bool(engine), "create backend")) return 1;
  if (!require(!td::ValidatePreviewDispatch({}, 16, &error),
               "reject truncated compute buffers"))
    return 1;
  error.clear();
  std::vector<uint32_t> geometry(16 + 128 + 15);
  geometry[0] = 2;
  geometry[1] = 5;
  geometry[2] = 2;
  auto geo = [&](size_t i, float x) { std::memcpy(&geometry[i], &x, 4); };
  for (size_t t = 0; t < 2; ++t) {
    const size_t m = 16 + t * 64;
    const float x = float(t) * 100;
    const float positions[3][3] = {{x - 1, 0, 2}, {x + 3, 0, 2}, {x, 4, 2}};
    for (size_t c = 0; c < 3; ++c) {
      for (size_t k = 0; k < 3; ++k) geo(m + c * 16 + k, positions[c][k]);
      geo(m + c * 16 + 3, .5f + .25f * float(c));
      geo(m + c * 16 + 6, 1);
      geo(m + c * 16 + 8, .2f + .2f * float(c));
      geo(m + c * 16 + 12, c == 1 ? 1.f : 0.f);
      geo(m + c * 16 + 13, c == 2 ? 1.f : 0.f);
    }
    geometry[m + 48] = t ? 3 : 0;
    geometry[m + 49] = t ? 2 : 3;
    geo(m + 50, .5f + .2f * float(t));
    geometry[m + 51] = t ? 153 : 144;
  }
  const float weights[] = {1, 0, 0, 0, 1, 0, .25f, .25f, .5f, 1, 0, 0, 0, 1, 0};
  for (size_t i = 0; i < 15; ++i) geo(144 + i, weights[i]);
  std::vector<float> geo_cpu, geo_gpu;
  if (!reference->dispatch(geometry, 80, &geo_cpu, &error) ||
      !engine->dispatch(geometry, 80, &geo_gpu, &error))
    return 1;
  float geometry_error = 0;
  for (size_t i = 0; i < geo_cpu.size(); ++i)
    geometry_error =
        std::max(geometry_error, std::abs(geo_cpu[i] - geo_gpu[i]));
  if (!require(geometry_error < 1e-5f && std::abs(geo_gpu[32] - .5f) < 1e-6f &&
                   std::abs(geo_gpu[33] - 2.f) < 1e-6f && geo_gpu[48] == 99.f &&
                   geo_gpu[64] == 103.f,
               "geometry interpolation, attributes and triangle lookup"))
    return 1;
  std::vector<uint32_t> input(16 + 12 * 2);
  input[1] = 16;
  input[2] = 2;
  auto put = [&](size_t i, float x) { std::memcpy(&input[i], &x, 4); };
  put(3, 1);
  put(16, 8);
  put(17, 8);
  put(18, 8);
  put(19, 4);
  put(22, 1);
  put(24, 1);
  put(28, 6);
  put(29, 8);
  put(30, 7);
  put(31, 2);
  put(32, 1);
  put(36, .2f);
  put(37, .8f);
  put(38, .4f);
  std::vector<float> expected, actual;
  if (!reference->dispatch(input, 16 * 16 * 16 * 4, &expected, &error) ||
      !engine->dispatch(input, expected.size(), &actual, &error))
    return 1;
  float max_error = 0;
  for (size_t i = 0; i < actual.size(); ++i)
    max_error = std::max(max_error, std::abs(expected[i] - actual[i]));
  if (!require(max_error < 1e-5f, "volume conversion parity")) return 1;
  const auto folder = argc > 2
                          ? fs::path(argv[2])
                          : fs::temp_directory_path() /
                                ("preview-compute-" + name + "-" +
                                 std::to_string(std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count()));
  fs::create_directories(folder);
  struct Cleanup {
    fs::path folder;
    bool keep;
    ~Cleanup() {
      std::error_code ec;
      if (!keep) fs::remove_all(folder, ec);
    }
  } cleanup{folder, argc > 2};
  const auto source = folder / "source.usda";
  {
    std::ofstream file(source);
    file << R"usd(#usda 1.0
 def Cube "Cube" {
   double size=2
   rel material:binding=</Material>
 }
 def Camera "Camera" {}
 def DistantLight "Sun" {
   float inputs:intensity=1.5
   color3f inputs:color=(1,0.8,0.6)
 }
 def Material "Material" {
   token outputs:surface.connect=</Material/Surface.outputs:surface>
   def Shader "Surface" {
     uniform token info:id="UsdPreviewSurface"
     color3f inputs:diffuseColor=(0.8,0.2,0.1)
   }
 }
)usd";
  }
  td::ScenePreviewOptions options;
  options.kind = td::ScenePreviewKind::Volume;
  options.compute_backend = backend;
  options.volume_surface_carrier = false;
  options.gpu_budget_bytes = budget;
  options.resolution = 8;
  options.cache_directory = (folder / "cache").string();
  lightusd::next::AssetResolver resolver;
  const auto result =
      td::ConvertScenePreview(source.string(), resolver, options);
  if (!require(result.success, result.error)) return 1;
  for (const auto& file :
       fs::directory_iterator(fs::path(result.usd_path).parent_path()))
    if (file.path().filename().string().rfind("surface_", 0) == 0) return 1;
  td::PreviewVolumeRenderer cpu, gpu;
  if (!cpu.load(result, td::PreviewComputeBackend::Cpu, budget, &error) ||
      !gpu.load(result, backend, budget, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  td::PreviewVolumeCamera camera;
  camera.eye = {0, 0, 5};
  camera.right = {.5f, 0, 0};
  camera.up = {0, .5f, 0};
  std::vector<uint8_t> a, b;
  if (!cpu.render(camera, 64, 64, &a, &error) ||
      !gpu.render(camera, 64, 64, &b, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  int max_pixel_error = 0;
  size_t foreground = 0, colored = 0;
  for (size_t i = 0; i < a.size(); ++i)
    max_pixel_error =
        std::max(max_pixel_error, std::abs(int(a[i]) - int(b[i])));
  for (size_t i = 0; i < a.size(); i += 4)
    if (std::abs(int(a[i]) - 69) > 10 || std::abs(int(a[i + 1]) - 69) > 10)
      ++foreground;
  for (size_t i = 0; i < a.size(); i += 4)
    if (a[i] > a[i + 1] + 20 && a[i + 1] > a[i + 2] + 5) ++colored;
  if (!require(max_pixel_error <= 2 && foreground > 100 && colored > 100,
               "direct SDF/albedo rendering parity and foreground"))
    return 1;
  const auto lit_image = b;
  camera.settings.scene_lights = false;
  if (!gpu.render(camera, 64, 64, &b, &error) ||
      !require(b != lit_image, "authored scene lights change surface shading"))
    return 1;
  camera.settings.scene_lights = true;
  camera.near_plane = 20;
  camera.far_plane = 100;
  if (!gpu.render(camera, 64, 64, &b, &error) ||
      !require(a != b, "camera clipping excludes the SDF surface"))
    return 1;
  camera.near_plane = .01f;
  camera.far_plane = 1e30f;
  camera.eye[0] = 20;
  if (!gpu.render(camera, 64, 64, &b, &error) ||
      !require(a != b, "camera-only upload changes the frame"))
    return 1;
  // Homogeneous fog slab: Beer-Lambert transmission has an analytic answer.
  std::vector<uint32_t> fog(80 + 27 * 4, 0);
  auto fp = [&](size_t i, float value) { std::memcpy(&fog[i], &value, 4); };
  fog[0] = 1;
  fog[1] = fog[2] = fog[3] = 1;
  fp(6, 3);
  fp(10, -1);
  fp(20, 1);
  fp(21, 1);
  fp(22, 1);
  fp(24, 20);
  fog[27] = 1;
  fp(28, .7f);
  fog[32] = 512;
  fp(64, -1);
  fp(65, -1);
  fp(66, -1);
  fp(67, 1);
  fog[68] = fog[69] = fog[70] = 3;
  fog[71] = 80;
  fog[72] = 1;
  for (size_t i = 80; i < fog.size(); i += 4) {
    fp(i, 1);
    fp(i + 1, 1);
    fp(i + 2, .5f);
    fp(i + 3, .25f);
  }
  std::vector<float> fog_cpu, fog_gpu;
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(std::abs(fog_gpu[0] - std::exp(-1.4f)) < 1e-4f,
               "analytic density absorption"))
    return 1;
  {
    // Independent overlapping fields add extinction; preview halos select a
    // single level inside each overlap group instead of doubling the medium.
    std::vector<uint32_t> overlap(96 + 2 * 27 * 4, 0);
    std::copy(fog.begin(), fog.begin() + 64, overlap.begin());
    overlap[3] = 2;
    auto put_overlap = [&](size_t index, float value) {
      std::memcpy(&overlap[index], &value, 4);
    };
    for (size_t b = 0; b < 2; ++b) {
      const size_t m = 64 + b * 16, voxels = 96 + b * 27 * 4;
      std::copy(fog.begin() + 64, fog.begin() + 80, overlap.begin() + m);
      overlap[m + 7] = static_cast<uint32_t>(voxels);
      overlap[m + 9] = static_cast<uint32_t>(b + 1);
      overlap[m + 10] = 1;
      std::copy(fog.begin() + 80, fog.end(), overlap.begin() + voxels);
    }
    put_overlap(82,
                0);  // Partial z overlap, distinct entry and exit boundaries.
    if (!reference->dispatch(overlap, 4, &fog_cpu, &error) ||
        !engine->dispatch(overlap, 4, &fog_gpu, &error) ||
        !require(
            std::abs(fog_gpu[0] - std::exp(-2.8f)) < 1e-4f && fog_gpu[3] == 1,
            "partial overlap adds independent extinction"))
      return 1;
    put_overlap(82, -1);
    put_overlap(30, 2);
    for (size_t i = 0; i < 27; ++i) {
      put_overlap(96 + i * 4 + 1, 1);
      put_overlap(96 + i * 4 + 2, 0);
      put_overlap(96 + i * 4 + 3, 0);
      put_overlap(204 + i * 4 + 1, 0);
      put_overlap(204 + i * 4 + 2, 0);
      put_overlap(204 + i * 4 + 3, 1);
    }
    if (!engine->dispatch(overlap, 4, &fog_gpu, &error) ||
        !require(std::abs(fog_gpu[0] - 1) < 1e-4f &&
                     std::abs(fog_gpu[2] - 1) < 1e-4f &&
                     std::abs(fog_gpu[1] - std::exp(-2.8f)) < 1e-4f,
                 "overlap emission uses extinction-weighted color"))
      return 1;
    put_overlap(30, 0);
    overlap[73] = overlap[89] = 0;
    overlap[74] = overlap[90] = 0;
    put_overlap(83, .5f);
    for (size_t i = 0; i < 27; ++i) put_overlap(204 + i * 4, 2);
    if (!engine->dispatch(overlap, 4, &fog_gpu, &error) ||
        !require(std::abs(fog_gpu[0] - std::exp(-2.1f)) < 1e-4f,
                 "fine halo replaces coarse density at its exact boundary"))
      return 1;
    // One ray step can no longer silently turn the unvisited medium into air.
    overlap[32] = 1;
    if (!engine->dispatch(overlap, 4, &fog_gpu, &error) ||
        !require(fog_gpu[3] < 0, "exhausted primary integration is reported"))
      return 1;
  }
  {
    auto varying = fog;
    auto put_varying = [&](size_t index, float value) {
      std::memcpy(&varying[index], &value, 4);
    };
    put_varying(42,
                2);  // A two-voxel proposal spans the density's central peak.
    for (size_t i = 0; i < 27; ++i)
      put_varying(80 + i * 4, i / 9 == 1 ? 1.f : 0.f);
    if (!engine->dispatch(varying, 4, &fog_gpu, &error)) return 1;
    const float coarse_error = std::abs(fog_gpu[0] - std::exp(-.7f));
    put_varying(43, 1e-4f);
    if (!reference->dispatch(varying, 4, &fog_cpu, &error) ||
        !engine->dispatch(varying, 4, &fog_gpu, &error) ||
        !require(std::abs(fog_gpu[0] - std::exp(-.7f)) < 1e-4f &&
                     std::abs(fog_gpu[0] - std::exp(-.7f)) < coarse_error * .1f,
                 "adaptive quadrature converges on a thin density peak"))
      return 1;
    // A separable trilinear grid becomes a cubic along the diagonal ray.
    for (size_t i = 0; i < 27; ++i)
      put_varying(80 + i * 4, float((i % 3) * ((i / 3) % 3) * (i / 9)) / 8);
    put_varying(4, 3);
    put_varying(5, 3);
    put_varying(6, 3);
    put_varying(8, -1);
    put_varying(9, -1);
    put_varying(10, -1);
    if (!engine->dispatch(varying, 4, &fog_gpu, &error) ||
        !require(std::abs(fog_gpu[0] - std::exp(-.7f * std::sqrt(3.f) * .5f)) <
                     2e-4f,
                 "diagonal cubic-density optical depth"))
      return 1;
  }
  {
    // Isotropic scattering next to a finite emitter measures its solid angle.
    // A dense medium localizes the receiver near the front of the slab.
    auto area = fog;
    const size_t lamp_area = area.size();
    area.resize(lamp_area + 40);
    auto ap = [&](size_t index, float value) {
      std::memcpy(&area[index], &value, 4);
    };
    area[1] = 128;
    area[25] = 1;
    area[26] = static_cast<uint32_t>(lamp_area);
    area[33] = 1;
    area[38] = 64;
    ap(20, 0);
    ap(21, 0);
    ap(22, 0);
    ap(28, 1000);
    ap(46, 1e-7f);
    ap(29, 1);
    ap(lamp_area + 6, -1);
    ap(lamp_area + 8, 1);
    ap(lamp_area + 9, 1);
    ap(lamp_area + 10, 1);
    ap(lamp_area + 20, 1);
    ap(lamp_area + 25, 1);
    ap(lamp_area + 30, 1);
    const auto solid_angle = [&](uint32_t type, double h) {
      if (type == 3) return 4 * std::atan(1 / (h * std::sqrt(h * h + 2)));
      if (type == 4) return 6.283185307179586 * (1 - h / std::sqrt(h * h + 1));
      if (type == 6)
        return 6.283185307179586 * (1 - std::sqrt(1 - 1 / (h * h)));
      double integral = 0;
      for (size_t a = 0; a < 64; ++a)
        for (size_t b = 0; b < 256; ++b) {
          const double x = (a + .5) * 2 / 64 - 1,
                       phi = (b + .5) * 6.283185307179586 / 256;
          const double y = std::cos(phi), z = h + std::sin(phi),
                       square = x * x + y * y + z * z;
          integral += std::max(0., -1 - h * std::sin(phi)) /
                      std::pow(square, 1.5) * 2 * 6.283185307179586 /
                      (64 * 256);
        }
      return integral;
    };
    for (uint32_t type : {3u, 4u, 6u, 7u}) {
      area[lamp_area + 3] = type;
      area[lamp_area + 15] = 0;
      const double height = type == 3 || type == 4 ? 1. : 2.;
      ap(lamp_area + 2, float(height + 1));
      ap(lamp_area + 23, type == 3 || type == 7 ? 2.f : 1.f);
      ap(lamp_area + 27, type == 3 ? 2.f : 1.f);
      ap(lamp_area + 7, 1);
      const float emitter_area = type == 3   ? 4.f
                                 : type == 4 ? 3.141592653589793f
                                             : 12.566370614359172f;
      ap(lamp_area + 14, emitter_area);
      double expected_area = 0;
      for (size_t q = 0; q < 32; ++q)
        expected_area +=
            std::exp(-double(q)) * (1 - std::exp(-1.)) *
            solid_angle(type, height + (q + 1 - 1 / (std::exp(1.) - 1)) / 1000);
      expected_area /= 12.566370614359172;
      if (!reference->dispatch(area, 512, &fog_cpu, &error) ||
          !engine->dispatch(area, 512, &fog_gpu, &error))
        return 1;
      double mean = 0, variance = 0, mse = 0;
      for (size_t q = 0; q < 128; ++q) {
        mean += fog_gpu[q * 4];
        mse += std::pow(fog_gpu[q * 4] - fog_cpu[q * 4], 2);
      }
      mean /= 128;
      for (size_t q = 0; q < 128; ++q)
        variance += std::pow(fog_gpu[q * 4] - mean, 2);
      if (!require(std::abs(mean - expected_area) <
                       5 * std::sqrt(variance / (128 * 128)) + .001,
                   "near-field finite emitter solid angle type=" +
                       std::to_string(type)) ||
          !require(std::sqrt(mse / 128) < .002,
                   "finite emitter integration backend parity"))
        return 1;
      area[lamp_area + 15] = 1;
      if (!engine->dispatch(area, 512, &fog_cpu, &error)) return 1;
      for (size_t q = 0; q < 128; ++q)
        if (!require(std::abs(fog_cpu[q * 4] * emitter_area - fog_gpu[q * 4]) <
                         1e-4f,
                     "normalized area-light energy"))
          return 1;
    }
  }
  {
    auto long_ray = fog;
    long_ray.resize(80 + 2 * 2 * 1024 * 4, 0);
    auto lp = [&](size_t i, float x) { std::memcpy(&long_ray[i], &x, 4); };
    lp(4, .5f);
    lp(5, .5f);
    lp(6, 1100);
    lp(24, 2000);
    lp(28, .001f);
    lp(42, .1f);
    lp(64, 0);
    lp(65, 0);
    lp(66, 0);
    long_ray[68] = long_ray[69] = 2;
    long_ray[70] = 1024;
    for (size_t i = 80; i < long_ray.size(); i += 4) lp(i, 1);
    long_ray[32] = 4096;
    if (!engine->dispatch(long_ray, 4, &fog_gpu, &error) ||
        !require(fog_gpu[3] < 0,
                 "long ray exposes the previous maximum step limit"))
      return 1;
    long_ray[32] = 16384;
    if (!reference->dispatch(long_ray, 4, &fog_cpu, &error) ||
        !engine->dispatch(long_ray, 4, &fog_gpu, &error) ||
        !require(
            fog_gpu[3] > 0 && std::abs(fog_gpu[0] - std::exp(-1.023f)) < 2e-4f,
            "converged optical depth beyond 4096 steps"))
      return 1;
  }
  {
    auto long_surface = fog;
    long_surface.resize(80 + 2 * 2 * 1024 * 4, 0);
    auto sp = [&](size_t i, float x) { std::memcpy(&long_surface[i], &x, 4); };
    long_surface[27] = 0;
    long_surface[72] = 0;
    long_surface[68] = long_surface[69] = 2;
    long_surface[70] = 1024;
    sp(4, .5f);
    sp(5, .5f);
    sp(6, 1100);
    sp(24, 2000);
    sp(42, .5f);
    sp(64, 0);
    sp(65, 0);
    sp(66, 0);
    sp(20, 0);
    sp(21, 0);
    sp(22, 0);
    for (size_t i = 0; i < 4096; ++i) {
      sp(80 + i * 4, float(i / 4) - .25f);
      sp(81 + i * 4, 1);
      sp(82 + i * 4, 1);
      sp(83 + i * 4, 1);
    }
    long_surface[32] = 512;
    if (!engine->dispatch(long_surface, 4, &fog_gpu, &error) ||
        !require(fog_gpu[3] < 0, "surface step exhaustion is reported"))
      return 1;
    long_surface[32] = 4096;
    if (!reference->dispatch(long_surface, 4, &fog_cpu, &error) ||
        !engine->dispatch(long_surface, 4, &fog_gpu, &error) ||
        !require(fog_gpu[3] > 0 && fog_gpu[0] > .9f &&
                     std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
                 "direct SDF tracing beyond 512 steps"))
      return 1;
  }
  fp(30, 2);
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(fog_gpu[0] > 1 && fog_gpu[0] > fog_gpu[1] &&
                   std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
               "density emission and backend parity"))
    return 1;
  fp(30, 0);
  fp(29, 1);
  fog[33] = 1;
  fog[25] = 1;
  fog[26] = static_cast<uint32_t>(fog.size());
  const size_t lamp = fog.size();
  fog.resize(lamp + 40);
  fog[lamp + 3] = 1;
  fp(lamp + 6, -1);
  fp(lamp + 8, 1);
  fp(lamp + 9, 1);
  fp(lamp + 10, 1);
  fp(lamp + 14, 1);
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(std::abs(fog_gpu[1] -
                        (std::exp(-1.4f) + .5f / 12.566370614359172f *
                                               (1 - std::exp(-1.4f)))) < 1e-4f,
               "analytic single scattering with distant light"))
    return 1;
  {
    // Light/shadow linking: receivers are media prim ids carried in field
    // metadata lane 11. Masks are per-light bit words; zero links everything.
    fog[47] = 1;          // one receiver prim in this scene
    fog[64 + 11] = 1;     // this field's prim id
    const float unshadowed = fog_gpu[1];
    auto linked = fog;
    const size_t word = linked.size();
    linked.resize(word + 1);
    linked[lamp + 33] = static_cast<uint32_t>(word);
    linked[word] = 0u;    // no receivers: the light is skipped
    std::vector<float> link_cpu, link_gpu;
    if (!reference->dispatch(linked, 4, &link_cpu, &error) ||
        !engine->dispatch(linked, 4, &link_gpu, &error))
      return 1;
    if (!require(std::abs(link_gpu[1] - std::exp(-1.4f)) < 1e-4f &&
                     std::abs(link_cpu[1] - link_gpu[1]) < 1e-4f,
                 "light link excludes the receiver"))
      return 1;
    linked[word] = 2u;    // bit 1 = this field's prim
    if (!reference->dispatch(linked, 4, &link_cpu, &error) ||
        !engine->dispatch(linked, 4, &link_gpu, &error) ||
        !require(std::abs(link_gpu[1] - unshadowed) < 1e-4f &&
                     std::abs(link_cpu[1] - link_gpu[1]) < 1e-4f,
                 "light link includes the receiver"))
      return 1;
    // Shadow linking is independent: excluding the receiver drops only the
    // shadow ray, so the frame matches the unshadowed result, while including
    // it reproduces the unmasked shadows-on frame exactly.
    auto shadow_on = fog;
    shadow_on[lamp + 12] = 1;
    std::vector<float> shadow_cpu, shadow_gpu;
    if (!reference->dispatch(shadow_on, 4, &shadow_cpu, &error) ||
        !engine->dispatch(shadow_on, 4, &shadow_gpu, &error))
      return 1;
    auto shadow_linked = shadow_on;
    const size_t shadow_word = shadow_linked.size();
    shadow_linked.resize(shadow_word + 1);
    shadow_linked[lamp + 34] = static_cast<uint32_t>(shadow_word);
    shadow_linked[shadow_word] = 0u;
    std::vector<float> unlinked_cpu, unlinked_gpu;
    if (!reference->dispatch(shadow_linked, 4, &unlinked_cpu, &error) ||
        !engine->dispatch(shadow_linked, 4, &unlinked_gpu, &error))
      return 1;
    if (!require(std::abs(unlinked_gpu[1] - unshadowed) < 1e-4f &&
                     std::abs(unlinked_cpu[1] - unlinked_gpu[1]) < 1e-4f,
                 "shadow link excludes the receiver"))
      return 1;
    shadow_linked[shadow_word] = 2u;
    if (!reference->dispatch(shadow_linked, 4, &unlinked_cpu, &error) ||
        !engine->dispatch(shadow_linked, 4, &unlinked_gpu, &error) ||
        !require(unlinked_gpu[1] == shadow_gpu[1],
                 "shadow link includes the receiver"))
      return 1;
  }
  {
    // A photometric profile scales direct emission only; transmitted ambient
    // radiance is unchanged. A constant half-candela table must produce an
    // exact midpoint, and both backends must agree.
    auto with_ies = fog;
    const size_t table = with_ies.size();
    with_ies.resize(table + 9 + 32 * 16);
    auto ip = [&](size_t index, float value) {
      std::memcpy(&with_ies[index], &value, 4);
    };
    ip(table + 0, 1);
    ip(table + 4, 1);
    ip(table + 8, 1);
    for (size_t i = 0; i < 32 * 16; ++i) ip(table + 9 + i, .5f);
    with_ies[lamp + 32] = static_cast<uint32_t>(table);
    std::vector<float> ies_cpu, ies_gpu;
    const bool ies_cpu_ok = reference->dispatch(with_ies, 4, &ies_cpu, &error);
    const std::string ies_cpu_error = error;
    const bool ies_gpu_ok = engine->dispatch(with_ies, 4, &ies_gpu, &error);
    if (!ies_cpu_ok || !ies_gpu_ok) {
      std::cerr << "diag: IES dispatch cpu=" << ies_cpu_ok
                << " gpu=" << ies_gpu_ok << " cpu_error=" << ies_cpu_error
                << " gpu_error=" << error << "\n";
      return 1;
    }
    const float transmission = std::exp(-1.4f);
    const float expected = .5f * (fog_gpu[1] + transmission);
    if (!require(std::abs(ies_gpu[1] - expected) < 1e-4f &&
                     std::abs(ies_cpu[1] - ies_gpu[1]) < 1e-4f,
                 "photometric profile shapes direct emission"))
      return 1;
    with_ies[lamp + 32] = static_cast<uint32_t>(with_ies.size());
    if (!require(!reference->dispatch(with_ies, 4, &ies_cpu, &error) ||
                     !td::ValidatePreviewDispatch(with_ies, 4, &error),
                 "reject out-of-range IES block"))
      return 1;
  }
  {
    auto shadow = fog;
    auto sp = [&](size_t index, float value) {
      std::memcpy(&shadow[index], &value, 4);
    };
    shadow[34] = 1;
    shadow[lamp + 12] = 1;
    sp(42, .03125f);
    if (!reference->dispatch(shadow, 4, &fog_cpu, &error) ||
        !engine->dispatch(shadow, 4, &fog_gpu, &error) ||
        !require(std::abs(fog_gpu[0] - (std::exp(-1.4f) +
                                        .5f / 12.566370614359172f *
                                            (1 - std::exp(-2.8f)))) < 1e-4f,
                 "analytic medium shadow attenuation along the whole segment"))
      return 1;
    shadow[39] = 1;
    if (!engine->dispatch(shadow, 4, &fog_gpu, &error) ||
        !require(!std::isfinite(fog_gpu[0]),
                 "exhausted shadow integration is reported"))
      return 1;
    // Restore the unshadowed baseline used by the phase comparison below.
    if (!reference->dispatch(fog, 4, &fog_cpu, &error)) return 1;
  }
  fp(31, .5f);
  if (!engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(fog_gpu[0] < fog_cpu[0],
               "forward anisotropy suppresses backscattering"))
    return 1;
  // Coupled random walks preserve Beer-Lambert transmission in expectation.
  fp(31, 0);
  fp(29, 0);
  fog[1] = 1024;
  fog[35] = 4;
  fog[36] = 64;
  if (!reference->dispatch(fog, 4096, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4096, &fog_gpu, &error))
    return 1;
  double mean = 0, mse = 0;
  for (size_t i = 0; i < 1024; ++i) {
    mean += fog_gpu[i * 4];
    mse += std::pow(fog_gpu[i * 4] - fog_cpu[i * 4], 2);
  }
  if (!require(std::abs(mean / 1024 - std::exp(-1.4)) < .012,
               "Monte Carlo analytic absorption") ||
      !require(std::sqrt(mse / 1024) < .01, "random walk backend parity"))
    return 1;
  fp(29, .8f);
  fog[35] = 2;
  if (!engine->dispatch(fog, 4096, &fog_cpu, &error)) return 1;
  fog[35] = 4;
  if (!engine->dispatch(fog, 4096, &fog_gpu, &error)) return 1;
  double extra = 0;
  for (size_t i = 0; i < 1024; ++i) {
    if (!require(fog_gpu[i * 4] + 1e-5f >= fog_cpu[i * 4],
                 "additional scattering is nonnegative"))
      return 1;
    extra += fog_gpu[i * 4] - fog_cpu[i * 4];
  }
  if (!require(extra > 1, "higher scattering orders contribute")) return 1;
  auto repeated = fog_gpu;
  if (!engine->dispatch(fog, 4096, &fog_gpu, &error) ||
      !require(repeated == fog_gpu, "fixed seed repeatability"))
    return 1;
  // Scattering beyond eight bounces: more orders may only add energy, every
  // sample stays finite and non-negative, and both backends agree.
  fog[35] = 12;
  std::vector<float> many_cpu, many_gpu;
  if (!reference->dispatch(fog, 4096, &many_cpu, &error) ||
      !engine->dispatch(fog, 4096, &many_gpu, &error))
    return 1;
  double many_energy = 0, few_energy = 0, many_mse = 0;
  for (size_t i = 0; i < 1024; ++i) {
    const float value = many_gpu[i * 4];
    if (!require(std::isfinite(value) && value >= 0,
                 "twelve-bounce scattering stays finite and non-negative"))
      return 1;
    many_energy += value;
    few_energy += fog_gpu[i * 4];
    many_mse += std::pow(value - many_cpu[i * 4], 2);
  }
  if (!require(many_energy + 1e-3 >= few_energy &&
                   std::sqrt(many_mse / 1024) < .02,
               "scattering beyond eight bounces is bounded and matches CPU"))
    return 1;
  fog[35] = 4;
  fog[37] = 19;
  if (!engine->dispatch(fog, 4096, &fog_gpu, &error) ||
      !require(repeated != fog_gpu, "seed changes stochastic samples"))
    return 1;
  {
    auto deep = fog;
    deep[1] = 256;
    deep[35] = 8;
    deep[36] = 8;
    const float dense = 4, scatter = 1, anisotropy = 0;
    std::memcpy(&deep[28], &dense, 4);
    std::memcpy(&deep[29], &scatter, 4);
    std::memcpy(&deep[31], &anisotropy, 4);
    if (!engine->dispatch(deep, 1024, &fog_cpu, &error)) return 1;
    deep[35] = 32;
    if (!engine->dispatch(deep, 1024, &fog_gpu, &error)) return 1;
    double higher_energy = 0;
    for (size_t i = 0; i < 256; ++i) {
      if (!require(fog_gpu[i * 4] + 1e-5f >= fog_cpu[i * 4],
                   "higher bounce limits preserve previous orders"))
        return 1;
      higher_energy += fog_gpu[i * 4] - fog_cpu[i * 4];
    }
    if (!require(higher_energy > 1,
                 "scattering above eight bounces contributes"))
      return 1;
  }
  {
    // Conservative scattering in a unit-radiance environment must not create
    // energy when secondary orders and Russian roulette are enabled.
    auto furnace = fog;
    furnace[1] = 256;
    furnace[35] = 32;
    furnace[36] = 8;
    furnace[38] = 2;
    furnace[34] = 1;
    furnace[lamp + 3] = 5;
    furnace[lamp + 12] = 1;
    const float dense = 2, unit = 1, zero_value = 0, cutoff = 1e-7f;
    std::memcpy(&furnace[28], &dense, 4);
    std::memcpy(&furnace[29], &unit, 4);
    std::memcpy(&furnace[31], &zero_value, 4);
    std::memcpy(&furnace[46], &cutoff, 4);
    for (size_t i = 80; i < 188; i += 4)
      for (size_t k = 1; k < 4; ++k) std::memcpy(&furnace[i + k], &unit, 4);
    if (!reference->dispatch(furnace, 1024, &fog_cpu, &error) ||
        !engine->dispatch(furnace, 1024, &fog_gpu, &error))
      return 1;
    double mean = 0, variance = 0, mse = 0;
    for (size_t i = 0; i < 256; ++i) {
      mean += fog_gpu[i * 4];
      mse += std::pow(fog_gpu[i * 4] - fog_cpu[i * 4], 2);
    }
    mean /= 256;
    for (size_t i = 0; i < 256; ++i)
      variance += std::pow(fog_gpu[i * 4] - mean, 2);
    if (!require(
            std::abs(mean - 1) < 5 * std::sqrt(variance / (256 * 256)) + .003,
            "unit-environment multiple scattering conserves radiance") ||
        !require(std::sqrt(mse / 256) < .01,
                 "conservative medium backend parity"))
      return 1;
  }
  // Constant colored texture modulates direct emission without affecting
  // geometry.
  fog[1] = 1;
  fog[35] = 1;
  fog[36] = 2;
  fp(29, 1);
  fog[lamp + 3] = 5;
  const auto texture = static_cast<uint32_t>(fog.size());
  fog[lamp + 16] = texture;
  fog[lamp + 17] = fog[lamp + 18] = 1;
  fog[lamp + 19] = 1;
  fog.resize(texture + 3);
  fp(texture, 1);
  fp(texture + 1, 0);
  fp(texture + 2, 0);
  fp(lamp + 20, 1);
  fp(lamp + 25, 1);
  fp(lamp + 30, 1);
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(fog_gpu[0] > fog_gpu[1] + .5f &&
                   std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
               "textured dome emission"))
    return 1;
  fog[lamp + 17] = 4;
  fog.resize(texture + 12);
  for (size_t i = 0; i < 4; ++i) {
    fp(texture + i * 3, i < 2 ? 1.f : 0.f);
    fp(texture + i * 3 + 1, 0);
    fp(texture + i * 3 + 2, i < 2 ? 0.f : 1.f);
  }
  fp(31, .7f);
  if (!engine->dispatch(fog, 4, &fog_cpu, &error)) return 1;
  fp(lamp + 20, 0);
  fp(lamp + 22, -1);
  fp(lamp + 28, 1);
  fp(lamp + 30, 0);
  if (!engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(std::abs(fog_cpu[0] - fog_gpu[0]) > .02f,
               "dome orientation rotates textured illumination"))
    return 1;
  for (uint32_t format = 1; format <= 3; ++format) {
    fog[lamp + 19] = format;
    if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
        !engine->dispatch(fog, 4, &fog_gpu, &error) ||
        !require(
            std::isfinite(fog_gpu[0]) &&
                std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
            "environment projection parity format=" + std::to_string(format) +
                " cpu=" + std::to_string(fog_cpu[0]) +
                " device=" + std::to_string(fog_gpu[0])))
      return 1;
  }
  // A tiny off-axis HDR emitter must retain its integrated energy, even at
  // one light sample. Axis quadrature cannot guarantee this.
  fog[lamp + 19] = 1;
  fog[lamp + 17] = 8;
  fog[lamp + 18] = 4;
  fp(lamp + 20, 1);
  fp(lamp + 22, 0);
  fp(lamp + 28, 0);
  fp(lamp + 30, 1);
  fp(31, 0);
  fog[38] = 1;
  const size_t bright = 9, ntex = 32, cdf = texture + ntex * 3;
  fog.resize(cdf + ntex + 1);
  for (size_t i = 0; i < ntex; ++i) {
    fp(texture + i * 3, i == bright ? 100.f : 0.f);
    fp(texture + i * 3 + 1, 0);
    fp(texture + i * 3 + 2, 0);
  }
  fog[lamp + 31] = static_cast<uint32_t>(cdf);
  for (size_t i = 0; i <= ntex; ++i) fp(cdf + i, i <= bright ? 0.f : 1.f);
  const float transmission = std::exp(-1.4f);
  const float mean_emission = 100.f * std::cos(3.141592653589793f / 4) / 16;
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(std::abs(fog_gpu[0] -
                        (transmission + mean_emission * (1 - transmission))) <
                       1e-4f &&
                   std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
               "importance sampling preserves tiny HDR dome energy"))
    return 1;
  // Disk projection Jacobians preserve energy without a latlong resample.
  for (uint32_t format = 2; format <= 3; ++format)
    for (size_t emitter : {size_t(9), size_t(0)}) {
      fog[lamp + 19] = format;
      fog[1] = 1024;
      fog[38] = 64;
      for (size_t i = 0; i < ntex; ++i)
        fp(texture + i * 3, i == emitter ? 100.f : 0.f);
      for (size_t i = 0; i <= ntex; ++i) fp(cdf + i, i <= emitter ? 0.f : 1.f);
      double integral = 0;
      for (size_t y = 0; y < 512; ++y)
        for (size_t x = 0; x < 512; ++x) {
          const double u = (double(emitter % 8) + (x + .5) / 512) / 8 - .5;
          const double v = (double(emitter / 8) + (y + .5) / 512) / 4 - .5;
          const double radius = std::sqrt(u * u + v * v);
          if (radius < .5)
            integral +=
                format == 3
                    ? 16.
                    : (radius < 1e-10
                           ? 39.47841760435743
                           : 6.283185307179586 *
                                 std::sin(6.283185307179586 * radius) / radius);
        }
      const double expected =
          transmission + 100 * integral /
                             (512 * 512 * 32 * 12.566370614359172) *
                             (1 - transmission);
      if (!reference->dispatch(fog, 4096, &fog_cpu, &error) ||
          !engine->dispatch(fog, 4096, &fog_gpu, &error))
        return 1;
      double mean = 0, mse = 0;
      for (size_t i = 0; i < 1024; ++i) {
        mean += fog_gpu[i * 4];
        mse += std::pow(fog_gpu[i * 4] - fog_cpu[i * 4], 2);
      }
      double variance = 0;
      for (size_t i = 0; i < 1024; ++i)
        variance += std::pow(fog_gpu[i * 4] - mean / 1024, 2);
      const double tolerance = 5 * std::sqrt(variance / (1024 * 1024)) + .0002;
      if (!require(
              std::abs(mean / 1024 - expected) < tolerance,
              "angular/mirrored-ball emitter energy, including disk edges") ||
          !require(std::sqrt(mse / 1024) < 1e-3,
                   "disk-projection backend parity"))
        return 1;
    }
  fog[lamp + 19] = 1;
  fog[1] = 1;
  fog[38] = 1;
  for (size_t i = 0; i < ntex; ++i)
    fp(texture + i * 3, i == bright ? 100.f : 0.f);
  for (size_t i = 0; i <= ntex; ++i) fp(cdf + i, i <= bright ? 0.f : 1.f);
  auto malformed_cdf = fog;
  malformed_cdf[lamp + 31] = static_cast<uint32_t>(fog.size());
  if (!require(!td::ValidatePreviewDispatch(malformed_cdf, 4, &error),
               "reject truncated light CDF"))
    return 1;
  malformed_cdf = fog;
  float invalid = -1;
  std::memcpy(&malformed_cdf[cdf + 2], &invalid, 4);
  if (!require(!td::ValidatePreviewDispatch(malformed_cdf, 4, &error),
               "reject nonmonotone light CDF"))
    return 1;
  malformed_cdf = fog;
  malformed_cdf[38] = 65;
  if (!require(!td::ValidatePreviewDispatch(malformed_cdf, 4, &error),
               "reject excessive light samples"))
    return 1;
  error.clear();
  fog[lamp + 3] = 3;
  fog[lamp + 19] = 0;
  fp(lamp + 2, 1000);
  fp(lamp + 8, 1e6f);
  fp(lamp + 23, 1);
  fp(lamp + 27, 1);
  fp(42, .125f);
  double rect_expected = transmission;
  for (size_t i = 0; i < 16384; ++i) {
    const double distance = (i + .5) * 2 / 16384;
    rect_expected += .7 * std::exp(-.7 * distance) *
                     (100. / (32 * 12.566370614359172)) * 1e6 /
                     std::pow(999 + distance, 2) * 2 / 16384;
  }
  if (!reference->dispatch(fog, 4, &fog_cpu, &error) ||
      !engine->dispatch(fog, 4, &fog_gpu, &error) ||
      !require(std::abs(fog_gpu[0] - rect_expected) < 1e-4f &&
                   std::abs(fog_gpu[0] - fog_cpu[0]) < 1e-4f,
               "finite-distance textured rect energy"))
    return 1;
  auto malformed = fog;
  malformed[lamp + 17] = 257;
  if (!require(!td::ValidatePreviewDispatch(malformed, 4, &error),
               "reject oversized emission texture"))
    return 1;
  malformed = fog;
  malformed[35] = 2;
  malformed[36] = 0;
  if (!require(!td::ValidatePreviewDispatch(malformed, 4, &error),
               "reject zero scattering samples"))
    return 1;
  error.clear();
  // Scene lighting and medium controls also work through the renderer API.
  camera.eye = {0, 0, 5};
  camera.settings.medium = true;
  camera.settings.density = .5f;
  camera.settings.emission = .1f;
  if (!cpu.render(camera, 64, 64, &a, &error) ||
      !gpu.render(camera, 64, 64, &b, &error))
    return 1;
  int medium_error = 0;
  size_t medium_bad = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    int e = std::abs(int(a[i]) - int(b[i]));
    medium_error = std::max(medium_error, e);
    if (e > 2) ++medium_bad;
  }
  if (medium_error > 2)
    std::cerr << "medium error=" << medium_error << " channels=" << medium_bad
              << "\n";
  if (!require(medium_error <= 4, "medium renderer parity")) return 1;
  const auto medium = b;
  camera.settings.density = 0;
  if (!gpu.render(camera, 64, 64, &b, &error) ||
      !require(b != medium, "zero density removes participating medium"))
    return 1;
  // Read a true density-only UsdVol, without a surface or albedo grid.
  tvdb_grid_t prototype{}, density_grid{};
  prototype.transform.type = TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE;
  for (int k = 0; k < 3; ++k) {
    prototype.transform.voxel_size[k] = prototype.transform.scale_values[k] = 1;
    prototype.transform.translation[k] = -1;
    prototype.transform.matrix[k][k] = 1;
    prototype.transform.matrix[k][3] = -1;
  }
  prototype.transform.matrix[3][3] = 1;
  prototype.tree.layout.num_levels = 4;
  for (int k = 0; k < 4; ++k) {
    prototype.tree.layout.levels[k].node_type = k == 0   ? TVDB_NODE_ROOT
                                                : k == 3 ? TVDB_NODE_LEAF
                                                         : TVDB_NODE_INTERNAL;
    prototype.tree.layout.levels[k].value_type = TVDB_VALUE_FLOAT;
    prototype.tree.layout.levels[k].log2dim = k == 1   ? 5
                                              : k == 2 ? 4
                                              : k == 3 ? 3
                                                       : 0;
  }
  std::vector<tvdb_vec3i> coords;
  std::vector<float> densities;
  for (int z = 0; z < 3; ++z)
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x) {
        coords.push_back({x, y, z});
        densities.push_back(1);
      }
  const float zero = 0;
  tvdb_error_t vdb_error{};
  if (!tvdb_grid_from_sparse_typed_using_template(
          &prototype, coords.data(), densities.data(), coords.size(),
          TVDB_VALUE_FLOAT, &zero, "density", &density_grid))
    return 1;
  tvdb_grid_set_metadata_string(&density_grid, "class", "fog volume",
                                &vdb_error);
  tvdb_file_t vdb;
  tvdb_file_init_for_write(&vdb, nullptr);
  vdb.grids = &density_grid;
  vdb.num_grids = 1;
  const auto vdb_status = tvdb_file_save(
      &vdb, (folder / "density.vdb").string().c_str(),
      TVDB_COMPRESS_ZIP | TVDB_COMPRESS_ACTIVE_MASK, 1, 0, &vdb_error);
  tvdb_grid_destroy_owned(&density_grid);
  if (vdb_status != TVDB_OK) return 1;
  const auto fog_source = folder / "fog.usda";
  fs::create_directories(folder / "nested");
  {
    std::ofstream wrapper(fog_source);
    wrapper << "#usda 1.0\ndef Volume \"Fog\" "
               "(references=@nested/density.usda@</Fog>) {}\n";
    std::ofstream file(folder / "nested" / "density.usda");
    file << R"usd(#usda 1.0
 def Volume "Fog" {
   rel field:density=</Fog/Density>
   def OpenVDBAsset "Density" {
     asset filePath=@../density.vdb@
     token fieldName="density"
   }
 }
)usd";
  }
  td::ScenePreviewResult fog_result;
  fog_result.success = true;
  fog_result.usd_path = fog_source.string();
  td::PreviewVolumeRenderer fog_renderer;
  camera = td::PreviewVolumeCamera{};
  camera.eye = {0, 0, 3};
  camera.right = {0, 0, 0};
  camera.up = {0, 0, 0};
  camera.background = {1, 1, 1};
  camera.settings.scattering = 0;
  if (!fog_renderer.load(fog_result, backend, budget, &error) ||
      !fog_renderer.render(camera, 1, 1, &b, &error) ||
      !require(b[0] >= 100 && b[0] <= 106,
               "density-only UsdVol automatic medium rendering"))
    return 1;
  camera.settings.max_steps = 1;
  if (!require(!fog_renderer.render(camera, 1, 1, &b, &error) &&
                   error.find("step budget") != std::string::npos,
               "renderer rejects an exhausted integration budget"))
    return 1;
  camera.settings.max_steps = 4096;
  error.clear();
  const auto native_volume =
      td::OpenScenePreviewVolume(fog_source.string(), options);
  if (!require(native_volume.success &&
                   fs::is_regular_file(native_volume.render_usd_path),
               native_volume.error) ||
      !require(std::abs(native_volume.bounds_min[0] + 1) < 1e-6 &&
                   std::abs(native_volume.bounds_max[0] - 1) < 1e-6,
               "native volume framing metadata"))
    return 1;
  {
    std::ofstream file(folder / "overlap-fog.usda");
    file << "#usda 1.0\ndef Volume \"A\" (references=@fog.usda@</Fog>) {}\n"
            "def Volume \"B\" (references=@fog.usda@</Fog>) {}\n";
  }
  const auto lit_fog = folder / "textured-fog.usda";
  {
    std::ofstream texture(folder / "emitter.ppm", std::ios::binary);
    texture << "P6\n2 1\n255\n";
    const unsigned char colors[] = {255, 0, 0, 0, 0, 255};
    texture.write(reinterpret_cast<const char*>(colors), 6);
    std::ofstream file(lit_fog);
    file << R"usd(#usda 1.0
 def Volume "Fog" (references=@fog.usda@</Fog>) {}
 def DomeLight "Sky" {
  asset inputs:texture:file=@emitter.ppm@
  token inputs:texture:format="latlong"
  float inputs:intensity=.5
  float3 xformOp:rotateXYZ=(0,45,0)
  uniform token[] xformOpOrder=["xformOp:rotateXYZ"]
 }
 def RectLight "Panel" {
  asset inputs:texture:file=@emitter.ppm@
  float inputs:intensity=3
  float inputs:width=4
  float inputs:height=4
  double3 xformOp:translate=(0,0,3)
  uniform token[] xformOpOrder=["xformOp:translate"]
 }
)usd";
  }
  fog_result.usd_path = lit_fog.string();
  td::PreviewVolumeRenderer lit_cpu, lit_gpu;
  camera.right = {.8, 0, 0};
  camera.up = {0, .8, 0};
  camera.settings.scattering = .8f;
  camera.settings.density = .7f;
  camera.settings.scatter_bounces = 4;
  camera.settings.scatter_samples = 16;
  if (!lit_cpu.load(fog_result, td::PreviewComputeBackend::Cpu, budget,
                    &error) ||
      !lit_gpu.load(fog_result, backend, budget, &error) ||
      !lit_cpu.render(camera, 32, 32, &a, &error) ||
      !lit_gpu.render(camera, 32, 32, &b, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  double texture_mse = 0, colored_energy = 0;
  for (size_t i = 0; i < a.size(); ++i)
    texture_mse += std::pow(int(a[i]) - int(b[i]), 2);
  for (size_t i = 0; i < a.size(); i += 4)
    colored_energy += int(b[i]) + int(b[i + 2]) - 2 * int(b[i + 1]);
  if (!require(std::sqrt(texture_mse / a.size()) < 2,
               "textured rectangle and rotated dome random-walk parity") ||
      !require(colored_energy > 1000,
               "authored texture colors survive lighting upload"))
    return 1;
  // Progressive accumulation averages linear radiance and resets cleanly.
  camera.settings.scatter_samples = 1;
  camera.settings.light_samples = 1;
  camera.settings.accumulation_frames = 256;
  td::PreviewVolumeRenderer progressive;
  if (!progressive.load(fog_result, backend, budget, &error)) return 1;
  std::vector<uint8_t> first, coarse, converged;
  for (uint32_t frame = 0; frame < 256; ++frame) {
    if (!progressive.render(camera, 8, 8, &b, &error)) return 1;
    if (frame == 0) first = b;
    if (frame == 15) coarse = b;
  }
  converged = b;
  double first_mse = 0, coarse_mse = 0;
  for (size_t i = 0; i < b.size(); ++i) {
    first_mse += std::pow(int(first[i]) - int(converged[i]), 2);
    coarse_mse += std::pow(int(coarse[i]) - int(converged[i]), 2);
  }
  if (!require(coarse_mse < first_mse * .5,
               "progressive accumulation reduces image noise") ||
      !require(progressive.accumulatedFrames() == 256,
               "progressive frame count") ||
      !progressive.render(camera, 8, 8, &b, &error) ||
      !require(b == converged && progressive.accumulatedFrames() == 256,
               "stable-camera accumulation cap"))
    return 1;
  camera.eye[0] += .2f;
  if (!progressive.render(camera, 8, 8, &b, &error) ||
      !require(progressive.accumulatedFrames() == 1,
               "camera movement resets accumulation"))
    return 1;
  if (!progressive.render(camera, 8, 8, &b, &error, false) ||
      !require(progressive.accumulatedFrames() == 1,
               "capture does not advance accumulation"))
    return 1;
  camera.settings.density += .1f;
  if (!progressive.render(camera, 8, 8, &b, &error) ||
      !require(progressive.accumulatedFrames() == 1,
               "shading change resets accumulation"))
    return 1;
  if (!progressive.render(camera, 9, 8, &b, &error) ||
      !require(progressive.accumulatedFrames() == 1,
               "resize resets accumulation"))
    return 1;
  if (!progressive.load(fog_result, backend, budget, &error) ||
      !require(progressive.accumulatedFrames() == 0,
               "scene reload resets accumulation"))
    return 1;
  camera.settings.accumulation_frames = 64;
  camera.settings.light_samples = 8;
  camera.settings.scatter_samples = 16;
  if (!lit_gpu.render(camera, 32, 32, &b, &error)) return 1;
  auto previous_light_samples = b;
  camera.settings.light_samples = 32;
  if (!lit_gpu.render(camera, 32, 32, &b, &error) ||
      !require(previous_light_samples != b,
               "changing light samples invalidates the frame cache"))
    return 1;
  camera.settings.light_samples = 0;
  if (!require(!lit_gpu.render(camera, 32, 32, &b, &error),
               "reject zero light-sample count"))
    return 1;
  error.clear();
  const auto asset = folder / "asset.usda",
             instances = folder / "instances.usda";
  {
    std::ofstream file(asset);
    file << "#usda 1.0\ndef Xform \"Asset\" { def Cube \"Cube\" { double "
            "size=2 } }\n";
  }
  {
    std::ofstream file(instances);
    file << R"usd(#usda 1.0
 def Xform "A" (references=@asset.usda@</Asset> instanceable=true) {}
 def Xform "B" (references=@asset.usda@</Asset> instanceable=true) {
   double3 xformOp:translate=(8,0,0)
   uniform token[] xformOpOrder=["xformOp:translate"]
 }
)usd";
  }
  auto instance_options = options;
  instance_options.kind = td::ScenePreviewKind::Gaussian;
  instance_options.resolution = 16;
  instance_options.batch_roots = 1;
  const auto instance_result =
      td::ConvertScenePreview(instances.string(), resolver, instance_options);
  if (!require(instance_result.success, instance_result.error)) return 1;
  lightusd::next::Stage stage;
  lightusd::next::pcp::CompositionOptions compose;
  compose.num_threads = 1;
  std::string warning;
  if (!lightusd::next::pcp::ComposeStageFromFile(instance_result.usd_path,
                                                 resolver, &stage, compose,
                                                 &warning, &error))
    return 1;
  double minimum = 1e30, maximum = -1e30;
  stage.Traverse([&](const lightusd::next::UsdPrim& prim) {
    if (const auto* positions = prim.GetPropertyValue("positions")) {
      auto values = positions->view().as_float_array();
      for (size_t i = 0; i < values.size(); i += 3) {
        minimum = std::min(minimum, double(values[i]));
        maximum = std::max(maximum, double(values[i]));
      }
    }
    return true;
  });
  if (!require(minimum < -.5 && maximum > 8.5,
               "geometry batches preserve cross-batch native placements"))
    return 1;
  td::ScenePreviewLodCamera close;
  close.eye = {8, 0, 1.25};
  bool indexed = false;
  instance_options.progress = [&](const std::string& phase, uint64_t) {
    if (phase == "source-index") indexed = true;
    return phase != "open";  // A new region must not recompose source geometry.
  };
  const auto instance_fine = td::RefineScenePreview(
      instances.string(), instance_result, instance_options, close, 640, 480);
  if (!require(instance_fine.success && indexed, instance_fine.error) ||
      !require(instance_fine.samples > instance_result.samples,
               "GPU source triangle resampling adds native-instance detail"))
    return 1;
  std::cout << "PASS " << engine->name()
            << " geometry_max_error=" << geometry_error
            << " conversion max_error=" << max_error
            << " direct-grid pixel_error=" << max_pixel_error
            << " medium_pixel_error=" << medium_error
            << " foreground=" << foreground << "\n";
  return 0;
}
