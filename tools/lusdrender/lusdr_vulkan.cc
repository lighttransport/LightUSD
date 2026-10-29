// SPDX-License-Identifier: Apache-2.0
// lusdrender — LightRT Vulkan backend (GPU BVH traversal via lightrt_c_vk).
// Compiles to nothing unless HAVE_VULKAN is defined. Geometry flatten, ray
// generation and shading are shared with the HIP backend (lusdr_gpu_common).
#ifdef HAVE_VULKAN
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "image-writer.hh"
#include "lightrt_mtlx_bridge.hh"
#include "light3d/math.h"
#include "lightrt_c_vk.h"
#include "lusdr_context.hh"
#include "lusdr_gpu_common.hh"
#include "displacement_bake.hh"

namespace lusdr {

namespace {

// Seconds since `t0`, for the -stats per-stage timing lines.
double SecsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

size_t GpuTriangleCount(
    const std::vector<RTPreviewStats::MeshGeometry> &geos) {
  size_t n = 0;
  for (const auto &geo : geos) n += geo.indices.size() / 3u;
  return n;
}

size_t GpuChunkLimit() {
  size_t limit = size_t(262144);
  if (const char *s = std::getenv("LUSDR_GPU_TRIANGLE_CHUNK")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && n > 0) limit = static_cast<size_t>(n);
  }
  return limit;
}

bool RunMaterialXPath(lrt_vk_engine *vk, lrt_vk_rtx_scene *rtx,
                      const Options &opt, const GpuTriScene &geometry,
                      const lusdview::DrawScene &scene,
                      const CameraFrame &camera, int height) {
  const size_t material_count = std::max<size_t>(scene.materials.size(), 1u);
  std::vector<float> materials(
      material_count * static_cast<size_t>(lusdview::kLightRtOpenPBRFloats),
      0.0f);
  std::vector<float> graphs(material_count * lusdview::kRtMaterialGraphFloats,
                            0.0f);
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const lusdview::DrawMaterialCPU &m = scene.materials[i];
    float *dst = materials.data() +
                 i * static_cast<size_t>(lusdview::kLightRtOpenPBRFloats);
    lusdview::PackLightRtOpenPBR(m, dst);
    lusdview::PackMaterialXGraphRuntime(
        m, graphs.data() + i * lusdview::kRtMaterialGraphFloats);
  }
  if (scene.materials.empty()) {
    materials[0] = materials[1] = materials[2] = 0.5f;
    materials[3] = 1.0f;   // base weight
    materials[7] = 1.0f;   // specular weight
    materials[39] = 1.0f;  // opacity
    materials[42] = 0.55f; materials[43] = 1.5f;
  }
  if (opt.stats && !materials.empty()) {
    size_t graph_materials = 0;
    size_t graph_nodes = 0;
    for (size_t i = 0; i < material_count; ++i) {
      const size_t gb = i * static_cast<size_t>(lusdview::kRtMaterialGraphFloats);
      if (graphs[gb] > 0.0f) ++graph_materials;
      graph_nodes += static_cast<size_t>(std::max(graphs[gb], 0.0f));
    }
    std::cerr << "Vulkan material ABI: count=" << material_count
              << " base=" << materials[0] << "," << materials[1] << ","
              << materials[2] << " weight=" << materials[3]
              << " metal=" << materials[40]
              << " rough=" << materials[42]
              << " opacity=" << materials[39]
              << " graphMaterials=" << graph_materials
              << " graphNodes=" << graph_nodes << " firstRoutes=";
    for (int route = 0; route < lusdview::kRtMaterialGraphOutputCount; ++route)
      if (graphs[1 + route] >= 0.0f) std::cerr << route << ",";
    std::cerr << "\n";
  }

  std::vector<uint32_t> texels;
  std::vector<int32_t> texture_descs(scene.textures.size() * 8u, 0);
  const auto packed_pixel = [](const light3d::Image &image, size_t p) {
    const size_t base = p * static_cast<size_t>(std::max(image.channels, 1));
    const auto channel = [&](int c, uint8_t fallback) {
      return c < image.channels && base + static_cast<size_t>(c) < image.data.size()
                 ? image.data[base + static_cast<size_t>(c)] : fallback;
    };
    return uint32_t(channel(0, 255u)) |
           (uint32_t(channel(1, channel(0, 255u))) << 8u) |
           (uint32_t(channel(2, channel(0, 255u))) << 16u) |
           (uint32_t(channel(3, 255u)) << 24u);
  };
  for (size_t i = 0; i < scene.textures.size(); ++i) {
    const lusdview::DrawTextureCPU &t = scene.textures[i];
    const light3d::Image *image = &t.image;
    int32_t *d = texture_descs.data() + i * 8u;
    d[0] = static_cast<int32_t>(texels.size());
    d[3] = t.wrapS; d[4] = t.wrapT; d[5] = t.srgb ? 1 : 0;
    if (t.isUdim && !t.udimTiles.empty()) {
      uint32_t min_u = 65535u, min_v = 65535u, max_u = 0u, max_v = 0u;
      for (const lusdview::DrawUdimTileCPU &tile : t.udimTiles) {
        min_u = std::min(min_u, tile.u); min_v = std::min(min_v, tile.v);
        max_u = std::max(max_u, tile.u); max_v = std::max(max_v, tile.v);
      }
      const int tile_w = std::max(t.udimTileWidth, t.udimTiles.front().image.width);
      const int tile_h = std::max(t.udimTileHeight, t.udimTiles.front().image.height);
      const uint32_t grid_w = max_u - min_u + 1u;
      const uint32_t grid_h = max_v - min_v + 1u;
      d[1] = tile_w * static_cast<int>(grid_w);
      d[2] = tile_h * static_cast<int>(grid_h);
      d[6] = -1 - static_cast<int32_t>((min_u & 0xffffu) | (min_v << 16u));
      d[7] = static_cast<int32_t>((grid_w & 0xffffu) | (grid_h << 16u));
      const size_t atlas_start = texels.size();
      texels.resize(atlas_start + static_cast<size_t>(d[1]) *
                                      static_cast<size_t>(d[2]), 0u);
      for (const lusdview::DrawUdimTileCPU &tile : t.udimTiles) {
        const light3d::Image &src = tile.image;
        const uint32_t ox = (tile.u - min_u) * static_cast<uint32_t>(tile_w);
        const uint32_t oy = (tile.v - min_v) * static_cast<uint32_t>(tile_h);
        for (int y = 0; y < std::min(src.height, tile_h); ++y)
          for (int x = 0; x < std::min(src.width, tile_w); ++x) {
            const size_t dst = atlas_start +
                static_cast<size_t>(oy + static_cast<uint32_t>(y)) * d[1] +
                ox + static_cast<uint32_t>(x);
            texels[dst] = packed_pixel(src, static_cast<size_t>(y) * src.width + x);
          }
      }
      continue;
    }
    d[1] = image->width; d[2] = image->height;
    const size_t pixels = static_cast<size_t>(std::max(image->width, 0)) *
                          static_cast<size_t>(std::max(image->height, 0));
    for (size_t p = 0; p < pixels; ++p) texels.push_back(packed_pixel(*image, p));
  }
  std::vector<float> lights;
  size_t geometry_lights = 0;
  for (const lusdview::DrawLightCPU &light : scene.lights) {
    const bool geometry_light = light.type == lusdview::DrawLightCPU::Type::Geometry;
    geometry_lights += geometry_light ? 1u : 0u;
    lights.insert(lights.end(), {
        light.position[0], light.position[1], light.position[2],
        static_cast<float>(light.type),
        light.direction[0], light.direction[1], light.direction[2],
        geometry_light ? static_cast<float>(light.geometryTriOffset) : light.radius,
        light.effectiveColor[0], light.effectiveColor[1],
        light.effectiveColor[2], geometry_light ? light.area : light.width,
        light.height, light.length, light.angle,
        static_cast<float>(light.envmapTexture)});
  }
  if (opt.stats) {
    std::cerr << "Vulkan descriptor lights: count=" << lights.size() / 16u
              << " geometry=" << geometry_lights << "\n";
    for (size_t li = 0; li < lights.size() / 16u; ++li) {
      const size_t lb = li * 16u;
      if (static_cast<int>(lights[lb + 3u]) ==
          static_cast<int>(lusdview::DrawLightCPU::Type::Geometry)) {
        std::cerr << "  first geometry light: tri=" << lights[lb + 7u]
                  << " area=" << lights[lb + 11u] << " radiance="
                  << lights[lb + 8u] << "," << lights[lb + 9u] << ","
                  << lights[lb + 10u] << "\n";
        break;
      }
    }
  }

  const int width = opt.width > 0 ? opt.width : 960;
  const int h = height > 0 ? height : 540;
  const light3d::Vec3 eye{camera.origin.x, camera.origin.y, camera.origin.z};
  const light3d::Vec3 fwd{camera.forward.x, camera.forward.y, camera.forward.z};
  const light3d::Vec3 up{camera.up.x, camera.up.y, camera.up.z};
  const light3d::Mat4 view = light3d::lookAt(eye, eye + fwd, up);
  const float znear = std::max(camera.znear, 1.0e-5f);
  const float zfar = std::max(camera.zfar, znear + 1.0f);
  const light3d::Mat4 proj = light3d::perspectiveZeroOne(
      camera.yfov, static_cast<float>(width) / static_cast<float>(h), znear, zfar);
  const light3d::Mat4 inv_vp = (proj * view).inverse();
  lrt_vk_material_path_desc desc{};
  desc.vertices = geometry.flat_attrs.data();
  desc.nverts = static_cast<uint32_t>(geometry.flat_attrs.size() / 8u);
  desc.indices = geometry.flat_idx.data();
  desc.triangle_materials = geometry.material_ids.data();
  desc.ntris = geometry.ntris; desc.materials = materials.data();
  desc.nmaterials = static_cast<uint32_t>(material_count);
  desc.material_stride_floats = lusdview::kLightRtOpenPBRFloats;
  desc.graphs = graphs.data();
  desc.graph_stride_floats = lusdview::kRtMaterialGraphFloats;
  desc.texels = texels.empty() ? nullptr : texels.data();
  desc.ntexels = static_cast<uint32_t>(texels.size());
  desc.texture_descs = texture_descs.empty() ? nullptr : texture_descs.data();
  desc.ntextures = static_cast<uint32_t>(scene.textures.size());
  desc.lights = lights.empty() ? nullptr : lights.data();
  desc.nlights = static_cast<uint32_t>(lights.size() / 16u);
  std::memcpy(desc.inv_view_proj, inv_vp.m, sizeof(desc.inv_view_proj));
  desc.camera[0] = camera.origin.x; desc.camera[1] = camera.origin.y;
  desc.camera[2] = camera.origin.z; desc.clear_color[0] = opt.bg.x;
  desc.clear_color[1] = opt.bg.y; desc.clear_color[2] = opt.bg.z;
  desc.exposure = 1.0f; desc.width = static_cast<uint32_t>(width);
  desc.scene_frame = std::isfinite(opt.timecode)
                         ? static_cast<float>(opt.timecode) : 0.0f;
  desc.scene_time = desc.scene_frame / 24.0f;
  desc.height = static_cast<uint32_t>(h);
  desc.samples = opt.path_trace && opt.path_trace_samples > 0
                     ? opt.path_trace_samples : static_cast<uint32_t>(std::max(1, opt.samples));
  desc.max_depth = opt.path_trace ? opt.path_trace_max_depth : 1u;
  desc.rr_depth = opt.path_trace_rr_depth; desc.seed = opt.path_trace_seed;
  std::vector<float> rgba(static_cast<size_t>(width) * static_cast<size_t>(h) * 4u);
  lrt_result err = LRT_RESULT_OK;
  if (lrt_vk_rtx_scene_render_materialx_path(vk, rtx, &desc, rgba.data(), &err) != 0) {
    std::cerr << "Vulkan MaterialX/path dispatch failed (" << int(err) << "): "
              << lrt_vk_engine_last_error(vk) << "\n";
    return false;
  }
  lightusd::Image out;
  out.width = width; out.height = h; out.channels = 4; out.bpp = 8;
  out.data.resize(rgba.size());
  for (size_t i = 0; i < rgba.size(); ++i)
    out.data[i] = static_cast<uint8_t>(std::lround(
        std::max(0.0f, std::min(1.0f, rgba[i])) * 255.0f));
  lightusd::image::WriteOption write_opt;
  write_opt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto written = lightusd::image::WriteImageToFile(opt.output, out, write_opt);
  if (!written) {
    std::cerr << "Failed to write Vulkan MaterialX/path image: "
              << written.error() << "\n";
    return false;
  }
  if (opt.stats) {
    std::cerr << "triangles: " << geometry.ntris << "\n";
  }
  std::cerr << "backend: LightRT VK (ray_query, descriptor MaterialX, "
            << (opt.path_trace ? "production path" : "OpenPBR preview")
            << ", samples=" << desc.samples << ")\n";
  return true;
}

bool RunVulkanLightRTChunked(
    const Options &opt, const std::vector<Vec3> &base_colors,
    std::vector<RTPreviewStats::MeshGeometry> &geos,
    const CameraFrame &camera, int height, size_t chunk_limit) {
  lrt_vk_engine_options vopts;
  std::memset(&vopts, 0, sizeof(vopts));
  vopts.device_index = opt.vulkan_device;
  vopts.prefer_discrete = 1;
  vopts.want_ray_tracing = opt.vulkan_rt;
  lrt_result vkerr = LRT_RESULT_OK;
  lrt_vk_engine *vk = lrt_vk_engine_create(&vopts, &vkerr);
  if (!vk) {
    std::cerr << "Failed to create LightRT Vulkan engine for mesh chunks (err="
              << int(vkerr) << ").\n";
    return false;
  }
  const uint32_t caps = lrt_vk_engine_caps(vk);
  bool use_hw_rt = (caps & LRT_VK_CAP_RAY_QUERY) != 0 && opt.vulkan_rt;
  std::cerr << "Vulkan device: " << lrt_vk_engine_device_name(vk) << " ("
            << (lrt_vk_device_local_bytes(1) >> 20)
            << " MiB device-local), mesh chunks=" << chunk_limit << " tris\n";
  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height > 0 ? height : 540;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "Vulkan", &nrays)) {
    lrt_vk_engine_destroy(vk);
    return false;
  }
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);
  std::vector<lrt_hit> hits(nrays), chunk_hits(nrays);
  for (lrt_hit &hit : hits) {
    hit.t = std::numeric_limits<float>::max();
    hit.u = hit.v = 0.0f;
    hit.prim_id = LRT_TRI_NO_HIT;
  }
  GpuTriScene shade;
  const size_t total_triangles = GpuTriangleCount(geos);
  if (!ValidateGpuTriangleCount(total_triangles, "Vulkan")) {
    lrt_vk_engine_destroy(vk);
    return false;
  }
  shade.ntris = static_cast<uint32_t>(total_triangles);
  size_t mesh = 0, tri = 0, global_first = 0, chunk_count = 0;
  double flatten_s = 0.0, trace_s = 0.0, as_s = 0.0;
  bool used_hw_rt = false, fell_back = false;
  const bool force_vkr_fallback = std::getenv("LUSDR_FORCE_VKR_FALLBACK") != nullptr;
  while (mesh < geos.size()) {
    std::vector<Vec3> chunk_colors;
    std::vector<RTPreviewStats::MeshGeometry> chunk_geos;
    size_t chunk_tris = 0;
    if (!BuildGpuTriChunk(base_colors, geos, chunk_limit, &mesh, &tri,
                           &chunk_colors, &chunk_geos, &chunk_tris)) {
      lrt_vk_engine_destroy(vk);
      return false;
    }
    GpuTriScene scene;
    auto t0 = std::chrono::steady_clock::now();
    if (!BuildGpuTriScene(chunk_colors, chunk_geos, opt.threads,
                          !use_hw_rt, &scene, opt.quality)) {
      lrt_vk_engine_destroy(vk);
      return false;
    }
    flatten_s += SecsSince(t0);
    shade.base_colors.insert(shade.base_colors.end(), scene.base_colors.begin(),
                             scene.base_colors.end());
    shade.normals.insert(shade.normals.end(), scene.normals.begin(),
                         scene.normals.end());
    AppendGpuSmoothNormals(scene, &shade);
    std::fill(chunk_hits.begin(), chunk_hits.end(), lrt_hit{});
    for (lrt_hit &hit : chunk_hits) {
      hit.t = std::numeric_limits<float>::max();
      hit.prim_id = LRT_TRI_NO_HIT;
    }
    lrt_result trace_err = LRT_RESULT_OK;
    int traced = -1;
    if (use_hw_rt) {
      auto as_t0 = std::chrono::steady_clock::now();
      lrt_vk_rtx_scene *rtx = lrt_vk_rtx_scene_build_indexed(
          vk, scene.flat_verts.data(), uint32_t(scene.flat_verts.size() / 3u),
          scene.flat_idx.data(), scene.ntris, &trace_err);
      as_s += SecsSince(as_t0);
      if (rtx) {
        auto trace_t0 = std::chrono::steady_clock::now();
        if (!force_vkr_fallback)
          traced = lrt_vk_rtx_scene_trace(vk, rtx, rays.data(), uint32_t(nrays),
                                          chunk_hits.data(), &trace_err);
        lrt_vk_rtx_scene_free(vk, rtx);
        trace_s += SecsSince(trace_t0);
      }
      if (traced < 0) {
        std::cerr << "Vulkan mesh chunk ray-query failed at [" << global_first
                  << ", " << (global_first + chunk_tris)
                  << "]; switching remaining chunks to compute trace.\n";
        use_hw_rt = false;
        fell_back = true;
        if (!BuildGpuCpuScene(opt.threads, &scene, opt.quality)) {
          lrt_vk_engine_destroy(vk);
          return false;
        }
      } else {
        used_hw_rt = true;
      }
    }
    if (!use_hw_rt) {
      auto trace_t0 = std::chrono::steady_clock::now();
      traced = lrt_vk_trace_scene(vk, scene.scene, rays.data(), uint32_t(nrays),
                                  chunk_hits.data(), &trace_err);
      trace_s += SecsSince(trace_t0);
    }
    if (traced < 0) {
      std::cerr << "Vulkan mesh chunk trace failed [" << global_first << ", "
                << (global_first + chunk_tris) << "] (err=" << int(trace_err)
                << "): " << lrt_vk_engine_last_error(vk) << "\n";
      if (scene.scene) lrt_tri_scene_free(scene.scene);
      lrt_vk_engine_destroy(vk);
      return false;
    }
    for (size_t i = 0; i < nrays; ++i) {
      if (chunk_hits[i].prim_id != LRT_TRI_NO_HIT &&
          chunk_hits[i].t < hits[i].t) {
        hits[i] = chunk_hits[i];
        hits[i].prim_id += static_cast<uint32_t>(global_first);
      }
    }
    if (scene.scene) lrt_tri_scene_free(scene.scene);
    global_first += chunk_tris;
    ++chunk_count;
  }
  const bool ok = ShadeAndWriteImage(opt, shade, rays, hits, w, h, spp);
  if (opt.stats) {
    std::cerr << "[gpu-stats] mesh chunks " << chunk_count
              << ", flatten+bvh " << flatten_s << " s, as-build " << as_s
              << " s, trace " << trace_s << " s\n";
  }
  if (ok) {
    std::cerr << "triangles: " << shade.ntris << " (" << geos.size()
              << " meshes, " << chunk_count << " GPU chunks)\n";
    std::cerr << "backend: LightRT VK ("
              << (used_hw_rt ? "ray_query" : "compute trace")
              << (fell_back ? ", ray-query fallback" : "") << ")\n";
  }
  lrt_vk_engine_destroy(vk);
  return ok;
}

}  // namespace

bool RunVulkanLightRT(const Options &opt, const std::vector<Vec3> &base_colors,
                      std::vector<RTPreviewStats::MeshGeometry> &geos,
                      const std::vector<ResolvedMat> &materials,
                      const std::vector<Texture> &textures,
                      const LightCache &lights,
                      const CameraFrame &camera, int height) {
  const size_t total_triangles = GpuTriangleCount(geos);
  const size_t chunk_limit = GpuChunkLimit();
  if (total_triangles > chunk_limit && !opt.path_trace) {
    return RunVulkanLightRTChunked(opt, base_colors, geos, camera, height,
                                   chunk_limit);
  }
  // Create the Vulkan engine first: whether the device traces via hardware ray
  // query decides if the CPU BVH is needed at all (the -vkr AS is GPU-built
  // from the indexed mesh, so the CPU build would be pure waste there).
  lrt_vk_engine_options vopts;
  std::memset(&vopts, 0, sizeof(vopts));
  vopts.device_index = opt.vulkan_device;
  vopts.prefer_discrete = 1;
  vopts.want_ray_tracing = opt.vulkan_rt;
  lrt_result vkerr = LRT_RESULT_OK;
  lrt_vk_engine *vk = lrt_vk_engine_create(&vopts, &vkerr);
  if (!vk) {
    std::cerr << "Failed to create LightRT Vulkan engine.\n";
    return false;
  }

  uint32_t vk_caps = lrt_vk_engine_caps(vk);
  bool has_rt = (vk_caps & LRT_VK_CAP_RAY_QUERY) != 0;
  const bool use_hw_rt = has_rt && opt.vulkan_rt;
  std::cerr << "Vulkan device: " << lrt_vk_engine_device_name(vk) << " ("
            << (lrt_vk_device_local_bytes(1) >> 20) << " MiB device-local)\n";
  std::cerr << "Vulkan caps: compute=1"
            << ((vk_caps & LRT_VK_CAP_BUFFER_ADDRESS) ? " buf_addr" : "")
            << ((vk_caps & LRT_VK_CAP_ACCEL_STRUCT) ? " accel" : "")
            << ((vk_caps & LRT_VK_CAP_RAY_QUERY) ? " ray_query" : "")
            << "\n";

  lusdview::DrawScene shared_scene;
  const bool have_shared_scene = BuildSharedDrawScene(
      base_colors, geos, materials, textures, lights, opt.input, &shared_scene);
  if (have_shared_scene && opt.displace && opt.displace_scale != 0.0f) {
    const size_t count = std::min(geos.size(), shared_scene.meshes.size());
    for (size_t i = 0; i < count; ++i) {
      std::vector<lusdview::DrawVertex> displaced;
      if (!lusdview::BakeDisplacedVertices(
              shared_scene, shared_scene.meshes[i], opt.displace_scale,
              &displaced) || displaced.size() * 3u != geos[i].positions.size()) {
        continue;
      }
      geos[i].normals.resize(displaced.size() * 3u);
      for (size_t v = 0; v < displaced.size(); ++v) {
        geos[i].positions[v * 3u + 0u] = displaced[v].px;
        geos[i].positions[v * 3u + 1u] = displaced[v].py;
        geos[i].positions[v * 3u + 2u] = displaced[v].pz;
        geos[i].normals[v * 3u + 0u] = displaced[v].nx;
        geos[i].normals[v * 3u + 1u] = displaced[v].ny;
        geos[i].normals[v * 3u + 2u] = displaced[v].nz;
      }
    }
  }
  GpuTriScene s;
  auto t0 = std::chrono::steady_clock::now();
  if (!BuildGpuTriScene(base_colors, geos, opt.threads, !use_hw_rt, &s,
                        opt.quality)) {
    lrt_vk_engine_destroy(vk);
    return false;
  }
  const double flatten_s = SecsSince(t0);
  for (RTPreviewStats::MeshGeometry &geo : geos) {
    std::vector<float>().swap(geo.positions);
    std::vector<float>().swap(geo.normals);
    std::vector<float>().swap(geo.uvs);
    std::vector<uint32_t>().swap(geo.indices);
  }

  // Generate every primary ray and trace the whole frame in ONE batched GPU
  // dispatch (the -vkr ray-query AS is built once, not per pixel).
  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "Vulkan", &nrays)) {
    if (s.scene) lrt_tri_scene_free(s.scene);
    lrt_vk_engine_destroy(vk);
    return false;
  }
  const uint32_t ray_count = uint32_t(nrays);
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);

  std::vector<lrt_hit> hits(nrays);
  lrt_result trerr = LRT_RESULT_OK;
  int traced = -1;
  double as_build_s = 0.0, trace_s = 0.0;
  bool used_hw_rt = false;
  bool ray_query_fallback = false;
  const bool force_vkr_fallback = std::getenv("LUSDR_FORCE_VKR_FALLBACK") != nullptr;
  if (use_hw_rt) {
    // Build the ray-query acceleration structure directly from the indexed mesh
    // (the GPU builds a VK_INDEX_TYPE_UINT32 BLAS, so no de-indexing needed);
    // primitiveIndex == triangle build order == caller index, matching the
    // CPU/compute paths. Build once, trace the whole frame, free.
    uint32_t nverts = uint32_t(s.flat_verts.size() / 3u);
    t0 = std::chrono::steady_clock::now();
    lrt_vk_rtx_scene *rtx = lrt_vk_rtx_scene_build_indexed(
        vk, s.flat_verts.data(), nverts, s.flat_idx.data(), s.ntris, &trerr);
    as_build_s = SecsSince(t0);
    if (rtx) {
      if (!force_vkr_fallback && have_shared_scene &&
          opt.gpu_shade != Options::GpuShadeMode::Cpu) {
        const bool material_path_ok =
            RunMaterialXPath(vk, rtx, opt, s, shared_scene, camera, height);
        if (material_path_ok) {
          lrt_vk_rtx_scene_free(vk, rtx);
          if (s.scene) lrt_tri_scene_free(s.scene);
          lrt_vk_engine_destroy(vk);
          return true;
        }
        if (opt.path_trace) {
          if (s.scene) lrt_tri_scene_free(s.scene);
          lrt_vk_engine_destroy(vk);
          return false;
        }
        std::cerr << "Vulkan descriptor material preview failed; falling back "
                     "to hit readback shading.\n";
      }
    }
    if (rtx) {
      t0 = std::chrono::steady_clock::now();
      if (force_vkr_fallback) {
        trerr = LRT_RESULT_UNSUPPORTED;
      } else {
        traced = lrt_vk_rtx_scene_trace(vk, rtx, rays.data(), ray_count,
                                        hits.data(), &trerr);
      }
      trace_s = SecsSince(t0);
      lrt_vk_rtx_scene_free(vk, rtx);
    }
    if (traced >= 0) {
      used_hw_rt = true;
    } else {
      std::cerr << "Vulkan ray-query trace failed (rc=" << trerr
                << "); falling back to compute trace.\n";
      t0 = std::chrono::steady_clock::now();
      if (!BuildGpuCpuScene(opt.threads, &s, opt.quality)) {
        lrt_vk_engine_destroy(vk);
        return false;
      }
      trerr = LRT_RESULT_OK;
      traced = lrt_vk_trace_scene(vk, s.scene, rays.data(), ray_count,
                                  hits.data(), &trerr);
      trace_s = SecsSince(t0);
      ray_query_fallback = traced >= 0;
    }
  } else {
    t0 = std::chrono::steady_clock::now();
    traced = lrt_vk_trace_scene(vk, s.scene, rays.data(), ray_count,
                                hits.data(), &trerr);
    trace_s = SecsSince(t0);
  }
  if (traced < 0) {
    std::cerr << "Vulkan trace failed (rc=" << trerr << ").\n";
    if (s.scene) lrt_tri_scene_free(s.scene);
    lrt_vk_engine_destroy(vk);
    return false;
  }

  t0 = std::chrono::steady_clock::now();
  const bool preview_shade =
      opt.gpu_shade != Options::GpuShadeMode::Cpu;
  bool ok = ShadeAndWriteImage(opt, s, rays, hits, w, h, spp,
                               preview_shade ? &materials : nullptr,
                               preview_shade ? &textures : nullptr);
  if (opt.stats) {
    std::cerr << "[gpu-stats] flatten+bvh " << flatten_s << " s, ";
    if (use_hw_rt) std::cerr << "as-build " << as_build_s << " s, ";
    std::cerr << "trace " << trace_s << " s, shade+write " << SecsSince(t0)
              << " s\n";  // compute-path trace includes the BVH upload
  }
  if (ok) {
    std::cerr << "triangles: " << s.ntris << " (" << geos.size() << " meshes)\n";
    std::cerr << "backend: LightRT VK (";
    if (used_hw_rt) {
      std::cerr << "ray_query, indexed Vulkan AS, CPU BVH skipped";
    } else if (ray_query_fallback) {
      std::cerr << "compute trace, ray_query fallback";
    } else {
      std::cerr << "compute trace";
    }
    std::cerr << ", " << (preview_shade ? "OpenPBR preview" : "CPU flat")
              << ")\n";
  }
  if (s.scene) lrt_tri_scene_free(s.scene);
  lrt_vk_engine_destroy(vk);
  return ok;
}

bool RunVulkanGaussianLightRT(const Options &opt, DirectScene *direct,
                              const CameraFrame &camera, int height) {
  if (!direct || direct->ellipse_chunks.empty())
    return false;
  lrt_vk_engine_options vopts;
  std::memset(&vopts, 0, sizeof(vopts));
  vopts.device_index = opt.vulkan_device;
  vopts.prefer_discrete = 1;
  lrt_result err = LRT_RESULT_OK;
  lrt_vk_engine *vk = lrt_vk_engine_create(&vopts, &err);
  if (!vk) {
    std::cerr << "Failed to create LightRT Vulkan Gaussian engine (err="
              << int(err) << ").\n";
    return false;
  }
  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height > 0 ? height : 540;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "Vulkan Gaussian", &nrays)) {
    lrt_vk_engine_destroy(vk);
    return false;
  }
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);
  std::vector<lrt_hit> hits(nrays);
  for (lrt_hit &hit : hits) {
    hit.t = std::numeric_limits<float>::max();
    hit.u = hit.v = 0.0f;
    hit.prim_id = LRT_TRI_NO_HIT;
  }
  std::vector<lrt_hit> chunk_hits(nrays);
  size_t released_chunks = 0;
  for (EllipseSceneChunk &chunk : direct->ellipse_chunks) {
    if (!BuildDeferredGaussianChunk(opt, &chunk)) {
      lrt_vk_engine_destroy(vk);
      return false;
    }
    int traced = lrt_vk_trace_scene(vk, chunk.scene.get(), rays.data(),
                                    uint32_t(nrays), chunk_hits.data(), &err);
    if (traced < 0) {
      std::cerr << "Vulkan Gaussian chunk trace failed [" << chunk.first
                << ", " << (chunk.first + chunk.count) << "] (err=" << int(err)
                << "): " << lrt_vk_engine_last_error(vk) << "\n";
      lrt_vk_engine_destroy(vk);
      return false;
    }
    if(opt.stats) std::cerr<<"Vulkan Gaussian hit rays: "<<traced<<" / "<<nrays<<"\n";
    for (size_t i = 0; i < nrays; ++i) {
      if (chunk_hits[i].prim_id != LRT_TRI_NO_HIT &&
          chunk_hits[i].t < hits[i].t) {
        hits[i] = chunk_hits[i];
        hits[i].prim_id += static_cast<uint32_t>(chunk.first);
      }
    }
    // The nearest-hit reduction above is the only later consumer of the
    // LightRT scene.  Keep the compact TriInfo metadata for shading, but drop
    // the chunk BVH immediately so large Gaussian fields do not retain every
    // uploaded/build allocation until the final image write.
    chunk.scene.reset();
    ++released_chunks;
  }
  size_t total_ellipses = 0;
  for (const EllipseSceneChunk &chunk : direct->ellipse_chunks)
    total_ellipses += chunk.info.size();
  const bool ok = ShadeAndWriteGaussianImage(
      opt, direct->ellipse_chunks, rays, hits, w, h, spp);
  if (ok) {
    std::cerr << "native Gaussian ellipses: " << total_ellipses
              << " in " << direct->ellipse_chunks.size() << " Vulkan chunk(s)\n"
              << "released Gaussian BVH chunks: " << released_chunks << "\n"
              << "\nbackend: LightRT VK (native point/ellipse BVH)\n";
  }
  lrt_vk_engine_destroy(vk);
  return ok;
}

bool RunVulkanLightRTInstanced(const Options &opt, GpuInstancedScene &scene,
                               const CameraFrame &camera, int height) {
  if (scene.protos.empty() || scene.insts.empty()) return false;

  lrt_vk_engine_options vopts;
  std::memset(&vopts, 0, sizeof(vopts));
  vopts.device_index = opt.vulkan_device;
  vopts.prefer_discrete = 1;
  vopts.want_ray_tracing = 1;
  lrt_result vkerr = LRT_RESULT_OK;
  lrt_vk_engine *vk = lrt_vk_engine_create(&vopts, &vkerr);
  if (!vk) {
    std::cerr << "Failed to create LightRT Vulkan engine.\n";
    return false;
  }
  uint32_t vk_caps = lrt_vk_engine_caps(vk);
  if (!(vk_caps & LRT_VK_CAP_RAY_QUERY)) {
    std::cerr << "-vkInstanced needs ray query; falling back to the flat path.\n";
    lrt_vk_engine_destroy(vk);
    return false;
  }
  std::cerr << "Vulkan device: " << lrt_vk_engine_device_name(vk) << " ("
            << (lrt_vk_device_local_bytes(1) >> 20) << " MiB device-local)\n";

  // Marshal the prototype + instance lists for the C API (views into `scene`).
  std::vector<lrt_vk_proto> cprotos(scene.protos.size());
  uint64_t unique_tris = 0;
  for (size_t p = 0; p < scene.protos.size(); ++p) {
    const GpuInstProto &pr = scene.protos[p];
    cprotos[p].vertices = pr.verts.data();
    cprotos[p].nverts = uint32_t(pr.verts.size() / 3u);
    cprotos[p].indices = pr.idx.data();
    cprotos[p].ntris = pr.ntris;
    unique_tris += pr.ntris;
  }
  std::vector<lrt_vk_instance> cinsts(scene.insts.size());
  uint64_t placed_tris = 0;
  for (size_t i = 0; i < scene.insts.size(); ++i) {
    std::memcpy(cinsts[i].transform, scene.insts[i].o2w, 12u * sizeof(float));
    cinsts[i].proto = scene.insts[i].proto;
    placed_tris += scene.protos[scene.insts[i].proto].ntris;
  }

  // Pick the hit encoding: the narrow (4-word) trace packs the hit as
  // instanceId*stride + localTri, which must fit 32 bits (ninsts*maxProtoTris <
  // 2^32). When it would overflow (Moana-island scale, a large prototype poisons
  // stride), use the wide (5-word) trace that stores instanceId + localTri
  // separately -- no product to overflow. The wide build is the MULTI-TLAS builder,
  // which additionally splits > ~16M instances across several TLASes (sharing one
  // BLAS set) so scenes past the device TLAS maxInstanceCount (2^24) -- the full
  // ~42.8M-instance Moana island -- render in full; it builds a single TLAS when
  // the scene fits, so smaller wide scenes are unaffected.
  uint32_t max_ntris = 1;
  for (const lrt_vk_proto &pr : cprotos)
    max_ntris = std::max(max_ntris, pr.ntris);
  bool wide = uint64_t(cinsts.size()) * uint64_t(max_ntris) >= 0xFFFFFFFFull ||
              cinsts.size() > 16000000u;  // multi-TLAS also needs the wide decode
  // Debug: force the wide encoding on any scene to validate the wide path against
  // the narrow one (they must be pixel-identical).
  if (std::getenv("LUSDR_FORCE_WIDE")) wide = true;

  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "Vulkan instanced", &nrays)) {
    lrt_vk_engine_destroy(vk);
    return false;
  }
  const uint32_t ray_count = uint32_t(nrays);
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);
  std::vector<InstSampleHit> decoded(nrays);

  lrt_result builderr = LRT_RESULT_OK, trerr = LRT_RESULT_OK;
  int traced = -1;
  uint32_t scene_ntlas = 1;
  double as_build_s = 0.0, trace_s = 0.0;
  auto t0 = std::chrono::steady_clock::now();
  if (wide) {
    lrt_vk_rtx_scene *rtx = lrt_vk_rtx_scene_build_instanced_multi(
        vk, cprotos.data(), uint32_t(cprotos.size()), cinsts.data(),
        uint32_t(cinsts.size()), &builderr);
    as_build_s = SecsSince(t0);
    if (!rtx) {
      // OUT_OF_MEMORY here means the GPU ran out building the BLAS/TLAS set (e.g.
      // full Moana island: ~110k prototype BLAS exhaust VRAM). The flat fallback
      // would flatten to even MORE geometry and fail harder, so report and stop
      // rather than fall back. Lower LUSDR_INST_BUDGET to render a bounded subset.
      const bool oom = builderr == LRT_RESULT_OUT_OF_MEMORY;
      std::cerr << "-vkInstanced wide/multi build failed (rc=" << builderr << ")"
                << (oom ? "; the scene exceeds GPU memory -- lower LUSDR_INST_BUDGET "
                          "to render a bounded subset.\n"
                        : "; falling back to the flat path.\n");
      lrt_vk_engine_destroy(vk);
      return oom;  // true = handled (do not try the doomed flat path)
    }
    scene_ntlas = lrt_vk_rtx_scene_ntlas(rtx);
    scene.stride = max_ntris;  // reported only; wide decode does not use it
    std::vector<lrt_hit_wide> hits(rays.size());
    t0 = std::chrono::steady_clock::now();
    traced = lrt_vk_rtx_scene_trace_wide(vk, rtx, rays.data(),
                                         ray_count, hits.data(), &trerr);
    trace_s = SecsSince(t0);
    lrt_vk_rtx_scene_free(vk, rtx);
    if (traced >= 0)
      for (size_t i = 0; i < hits.size(); ++i) {
        const lrt_hit_wide &hd = hits[i];
        decoded[i].valid = hd.inst != 0xFFFFFFFFu;
        decoded[i].inst = hd.inst;
        decoded[i].local = hd.local;
        decoded[i].u = hd.u;
        decoded[i].v = hd.v;
      }
  } else {
    uint32_t stride = 0;
    lrt_vk_rtx_scene *rtx = lrt_vk_rtx_scene_build_instanced(
        vk, cprotos.data(), uint32_t(cprotos.size()), cinsts.data(),
        uint32_t(cinsts.size()), &stride, &builderr);
    as_build_s = SecsSince(t0);
    if (!rtx) {
      std::cerr << "-vkInstanced build failed (rc=" << builderr
                << "); falling back to the flat path.\n";
      lrt_vk_engine_destroy(vk);
      return false;
    }
    scene.stride = stride;
    std::vector<lrt_hit> hits(rays.size());
    t0 = std::chrono::steady_clock::now();
    traced = lrt_vk_rtx_scene_trace(vk, rtx, rays.data(), ray_count,
                                    hits.data(), &trerr);
    trace_s = SecsSince(t0);
    lrt_vk_rtx_scene_free(vk, rtx);
    if (traced >= 0)
      for (size_t i = 0; i < hits.size(); ++i) {
        const lrt_hit &hd = hits[i];
        decoded[i].valid = hd.prim_id != 0xFFFFFFFFu && stride != 0;
        if (decoded[i].valid) {
          decoded[i].inst = hd.prim_id / stride;
          decoded[i].local = hd.prim_id % stride;
        }
        decoded[i].u = hd.u;
        decoded[i].v = hd.v;
      }
  }
  if (traced < 0) {
    std::cerr << "Vulkan instanced trace failed (rc=" << trerr << ").\n";
    lrt_vk_engine_destroy(vk);
    return false;
  }

  t0 = std::chrono::steady_clock::now();
  bool ok = ShadeAndWriteImageInstanced(opt, scene, rays, decoded, w, h, spp);
  if (opt.stats) {
    std::cerr << "[gpu-stats] as-build " << as_build_s << " s, trace+decode "
              << trace_s << " s, shade+write " << SecsSince(t0) << " s\n";
  }
  if (ok) {
    std::cerr << "backend: LightRT VK (ray_query, two-level TLAS, "
              << (wide ? "wide 64-bit hit id" : "32-bit hit id");
    if (scene_ntlas > 1) std::cerr << ", " << scene_ntlas << "-way multi-TLAS";
    std::cerr << ")\n";
    std::cerr << "instanced: " << scene.protos.size() << " prototypes ("
              << unique_tris << " unique tris) x " << scene.insts.size()
              << " instances = " << placed_tris << " placed tris; BLAS memory "
              << "stores " << unique_tris << " (vs " << placed_tris
              << " flattened)\n";
  }
  lrt_vk_engine_destroy(vk);
  return ok;
}

}  // namespace lusdr
#endif  // HAVE_VULKAN
