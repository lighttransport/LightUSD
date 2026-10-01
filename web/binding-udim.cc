// SPDX-License-Identifier: Apache-2.0
// Shared by combined and next-only products; no emval or legacy scene
// dependency.
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "image-loader.hh"
#include "minijson.hh"
#include "sha256.hh"
#include "udim-bake.hh"

namespace {
struct Job {
  lightusd::udim::AtlasBuilder builder;
  lightusd::udim::Atlas atlas;
  std::string error, info, name;
  size_t retained{0};
  bool finished{false};
};
std::map<uint32_t, std::unique_ptr<Job>> jobs;
uint32_t next_handle = 1;
std::string last_error;
constexpr size_t kMaxRetainedBytes = size_t(1) << 30;
bool Span(const void* data, size_t size) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(data);
  const size_t heap = emscripten_get_heap_size();
  return start != 0 && start <= heap && size <= heap - start;
}
Job* Find(uint32_t handle) {
  const auto it = jobs.find(handle);
  return it == jobs.end() ? nullptr : it->second.get();
}
const std::string& Info(uint32_t handle) {
  Job* job = Find(handle);
  if (!job) return last_error;
  lightusd::minijson::Value value;
  const auto& l = job->builder.layout();
  value["error"] = job->error;
  value["extension"] = job->atlas.extension;
  value["name"] = job->name;
  value["cols"] = l.cols;
  value["rows"] = l.rows;
  value["minU"] = l.min_u;
  value["minV"] = l.min_v;
  value["tileWidth"] = l.tile_width;
  value["tileHeight"] = l.tile_height;
  value["width"] = l.width;
  value["height"] = l.height;
  value["padding"] = l.padding;
  value["blankX"] = l.blank_x;
  value["blankY"] = l.blank_y;
  value["mode"] = l.mode == lightusd::udim::BakeMode::Dense ? "dense" : "grid";
  value["cells"] = lightusd::minijson::Value::array();
  for (const auto& cell : l.cells) {
    lightusd::minijson::Value v;
    v["id"] = cell.id;
    v["x"] = cell.x;
    v["y"] = cell.y;
    value["cells"].push_back(std::move(v));
  }
  job->info = value.dump();
  return job->info;
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void* lightusd_udim_alloc(uint32_t size) {
  return size && size <= uint32_t(INT32_MAX) ? std::malloc(size) : nullptr;
}
EMSCRIPTEN_KEEPALIVE void lightusd_udim_free(void* pointer) {
  std::free(pointer);
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_image_info(const uint8_t* data,
                                                      uint32_t size,
                                                      uint32_t* out,
                                                      uint32_t budget) {
  if (!budget || !Span(data, size) || !Span(out, 3 * sizeof(uint32_t)) || !size)
    return -1;
  auto info = lightusd::image::GetImageInfoFromMemoryBounded(
      data, size, "UDIM tile", budget);
  if (!info) return -1;
  out[0] = info->width;
  out[1] = info->height;
  out[2] = info->channels;
  return 1;
}
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_udim_begin(
    const uint32_t* ids, uint32_t count, int32_t width, int32_t height,
    int32_t mode, uint32_t max_tiles, int32_t max_edge, int32_t padding,
    uint32_t memory_budget, int32_t srgb) {
  if (!Span(ids, size_t(count) * sizeof(uint32_t)) || !count || count > 8999 ||
      (mode != 1 && mode != 2) || jobs.size() >= 4 || next_handle == 0) {
    last_error = "UDIM bake: invalid builder arguments or active-job limit";
    return 0;
  }
  if (count > memory_budget / 64) {
    last_error = "UDIM bake: tile metadata exceeds memory limit";
    return 0;
  }
  lightusd::udim::Options options;
  options.mode = mode == 2 ? lightusd::udim::BakeMode::Dense
                           : lightusd::udim::BakeMode::Grid;
  options.max_tiles = max_tiles;
  options.max_atlas_size = max_edge;
  options.dense_padding = padding;
  options.memory_budget_bytes = memory_budget;
  std::vector<uint32_t> tiles(ids, ids + count);
  lightusd::udim::Layout layout;
  if (!lightusd::udim::MakeLayout(tiles, width, height, options, &layout,
                                  &last_error))
    return 0;
  size_t retained = size_t(layout.width) * size_t(layout.height) * 16;
  for (const auto& job : jobs) {
    if (retained > kMaxRetainedBytes ||
        job.second->retained > kMaxRetainedBytes - retained) {
      last_error = "UDIM bake: aggregate builder memory limit exceeded";
      return 0;
    }
    retained += job.second->retained;
  }
  if (retained > kMaxRetainedBytes) {
    last_error = "UDIM bake: aggregate builder memory limit exceeded";
    return 0;
  }
  std::unique_ptr<Job> job(new (std::nothrow) Job);
  if (!job) {
    last_error = "UDIM bake: cannot allocate builder";
    return 0;
  }
  if (!job->builder.begin(tiles, width, height, options, srgb != 0,
                          &last_error))
    return 0;
  job->retained = size_t(layout.width) * size_t(layout.height) * 16;
  const uint32_t handle = next_handle++;
  jobs.emplace(handle, std::move(job));
  last_error.clear();
  return handle;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_add(uint32_t handle, uint32_t id,
                                               const uint8_t* data,
                                               uint32_t size) {
  Job* job = Find(handle);
  if (!job || job->finished || !Span(data, size)) return -1;
  return job->builder.add(id, data, size, "tile." + std::to_string(id),
                          &job->error)
             ? 1
             : -1;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_blank(uint32_t handle, uint32_t id) {
  Job* job = Find(handle);
  return job && !job->finished && job->builder.blank(id, &job->error) ? 1 : -1;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_finish(uint32_t handle,
                                                  int32_t format,
                                                  int32_t quality) {
  Job* job = Find(handle);
  if (!job || job->finished || format < 0 || format > 3) return -1;
  const char* formats[] = {"keep", "png", "jpeg", "exr"};
  if (!job->builder.finish(formats[format], quality, &job->atlas, &job->error))
    return -1;
  if (job->atlas.bytes.size() > size_t(std::numeric_limits<int32_t>::max())) {
    job->error = "UDIM bake: encoded output exceeds ABI size limit";
    return -1;
  }
  job->retained = job->atlas.bytes.size();
  job->name =
      "textures/udim_" +
      lightusd::sha256(reinterpret_cast<const char*>(job->atlas.bytes.data()),
                       job->atlas.bytes.size()) +
      "." + job->atlas.extension;
  job->finished = true;
  return 1;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_info_size(uint32_t handle) {
  const std::string& text = Info(handle);
  return text.size() <= size_t(std::numeric_limits<int32_t>::max())
             ? int32_t(text.size())
             : -1;
}
EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_udim_info_data(uint32_t handle) {
  const auto* job = Find(handle);
  return reinterpret_cast<uintptr_t>(job ? job->info.data()
                                         : last_error.data());
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_udim_data_size(uint32_t handle) {
  const Job* job = Find(handle);
  return job && job->finished ? int32_t(job->atlas.bytes.size()) : -1;
}
EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_udim_data(uint32_t handle) {
  const Job* job = Find(handle);
  return job && job->finished
             ? reinterpret_cast<uintptr_t>(job->atlas.bytes.data())
             : 0;
}
EMSCRIPTEN_KEEPALIVE void lightusd_udim_release(uint32_t handle) {
  jobs.erase(handle);
}
}
