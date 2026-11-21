// SPDX-License-Identifier: Apache-2.0
// Counted, heap-checked image conversion and budget-fit jobs; no emval/legacy.
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>

#include "core/minijson.hh"
#include "image-convert.hh"
#include "image-loader.hh"
#include "load-usd.hh"
#include "resolver/asset-resolver.hh"
#include "stage/stage.hh"
#include "tydra/texture-util.hh"

namespace {
using JSON = lightusd::minijson::Value;
constexpr size_t kBudget = size_t(1) << 30;
constexpr size_t kDefaultImageBudget = size_t(512) << 20;
struct Job {
  std::vector<lightusd::tydra::FitTextureInput> inputs;
  std::vector<lightusd::tydra::FitTextureOutput> outputs;
  std::vector<std::string> names;
  JSON info;
  std::string text, error;
  size_t retained{0}, name_reserve{0};
  bool finished{false};
};
std::map<uint32_t, std::unique_ptr<Job>> jobs;
uint32_t next_id = 1;
bool Span(const void* p, size_t n) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(p);
  const size_t heap = emscripten_get_heap_size();
  return start && start <= heap && n <= heap - start;
}
Job* Find(uint32_t id) {
  auto i = jobs.find(id);
  return i == jobs.end() ? nullptr : i->second.get();
}
uint32_t Create() {
  if (jobs.size() >= 4 || !next_id) return 0;
  uint32_t id = next_id++;
  jobs[id] = std::unique_ptr<Job>(new Job);
  return id;
}
bool Parse(const uint8_t* p, uint32_t n, JSON* config) {
  return n && n <= 65536 && Span(p, n) &&
         lightusd::minijson::Parse(
             std::string(reinterpret_cast<const char*>(p), n), config) &&
         config->is_object();
}
bool Integer(const JSON& json, const char* key, size_t fallback, size_t lo,
             size_t hi, size_t* out) {
  const JSON* value = json.find(key);
  if (!value) {
    *out = fallback;
    return true;
  }
  double n = 0;
  if (!value->is_number() || !value->as_double(&n) || !std::isfinite(n) ||
      std::floor(n) != n || n < double(lo) || n > double(hi))
    return false;
  *out = size_t(n);
  return true;
}
size_t OtherRetained(uint32_t id) {
  size_t size = 0;
  for (const auto& pair : jobs)
    if (pair.first != id) size += pair.second->retained;
  return size;
}
bool Error(Job* job, const std::string& error) {
  job->error = error;
  return false;
}
}  // namespace
extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_image_convert(const uint8_t* data,
                                                     uint32_t size,
                                                     const uint8_t* json,
                                                     uint32_t json_size) {
  const uint32_t id = Create();
  Job* job = Find(id);
  if (!job) return 0;
  JSON config;
  if (!Span(data, size) || !size || !Parse(json, json_size, &config)) {
    Error(job, "Invalid image conversion input/options");
    return id;
  }
  size_t max_size, width, height, quality, budget;
  if (!Integer(config, "maxSize", 0, 0, 16384, &max_size) ||
      !Integer(config, "width", 0, 0, 16384, &width) ||
      !Integer(config, "height", 0, 0, 16384, &height) ||
      !Integer(config, "jpegQuality", 90, 1, 100, &quality) ||
      !Integer(config, "memoryBudgetBytes", kDefaultImageBudget, 1, kBudget,
               &budget)) {
    Error(job, "Invalid image conversion dimensions/quality/memory limit");
    return id;
  }
  for (const char* key :
       {"format", "pngEncoder", "colorspace", "resizeColorspace"}) {
    const auto* v = config.find(key);
    if (v && !v->is_string()) {
      Error(job, "Image mode options must be strings");
      return id;
    }
  }
  const size_t other = OtherRetained(id);
  if (other >= kBudget) {
    Error(job, "Active image jobs exceed aggregate memory limit");
    return id;
  }
  lightusd::image::ConvertOptions opts;
  opts.max_size = int(max_size);
  opts.width = int(width);
  opts.height = int(height);
  opts.jpeg_quality = int(quality);
  opts.memory_budget_bytes = std::min(budget, kBudget - other);
  opts.format = config.value("format", std::string("png"));
  opts.png_encoder = config.value("pngEncoder", std::string("auto"));
  opts.colorspace = config.value("colorspace", std::string());
  opts.resize_colorspace = config.value("resizeColorspace", std::string());
  const auto* low = config.find("lowMemory");
  opts.low_memory = low && ((low->is_boolean() && low->get<bool>()) ||
                            (low->is_number() && low->get<int>() != 0));
  auto converted = lightusd::image::ConvertImage(data, size, opts);
  if (!converted) {
    Error(job, converted.error());
    return id;
  }
  lightusd::tydra::FitTextureOutput output;
  output.bytes = std::move(converted->data);
  output.width = converted->width;
  output.height = converted->height;
  output.ext =
      opts.format == "jpeg" || opts.format == "jpg" ? "jpg" : opts.format;
  output.changed = true;
  job->info["width"] = output.width;
  job->info["height"] = output.height;
  job->info["resized"] = converted->resized;
  job->retained = output.bytes.size();
  job->outputs.push_back(std::move(output));
  job->finished = true;
  return id;
}
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_image_fit_begin() { return Create(); }
EMSCRIPTEN_KEEPALIVE int32_t lightusd_image_fit_add(uint32_t id,
                                                    const uint8_t* data,
                                                    uint32_t size,
                                                    const uint8_t* name,
                                                    uint32_t name_size) {
  Job* job = Find(id);
  if (!job || job->finished || !job->error.empty()) return 0;
  if (!Span(data, size) || !size || !Span(name, name_size) || !name_size ||
      name_size > 1048576 || job->inputs.size() >= 4096)
    return Error(job, "Invalid texture fit input/count");
  const size_t retained = job->retained + OtherRetained(id);
  if (retained >= kBudget ||
      uint64_t(size) * 2 + uint64_t(name_size) * 8 + 4096 > kBudget - retained)
    return Error(job, "Texture fit input memory limit exceeded");
  const std::string filename(reinterpret_cast<const char*>(name), name_size);
  auto metadata = lightusd::image::GetImageInfoFromMemoryBounded(
      data, size, filename, kBudget - retained - size);
  size_t decoded_estimate = 0;
  if (metadata) {
    const uint64_t estimate =
        uint64_t(metadata->width) * metadata->height * metadata->channels * 4;
    if (!metadata->width || !metadata->height || metadata->width > 16384 ||
        metadata->height > 16384 || !metadata->channels ||
        metadata->channels > 4 ||
        estimate > uint64_t(kBudget - retained - size) / 8)
      return Error(job, "Texture fit decoded working memory limit exceeded");
    decoded_estimate = size_t(estimate);
  }
  const size_t fixed = size_t(size) * 2 + size_t(name_size) * 8 + 4096;
  if (decoded_estimate > (kBudget - retained - fixed) / 4)
    return Error(job, "Texture fit working memory limit exceeded");
  lightusd::tydra::FitTextureInput input;
  input.reencodable = false;
  const auto dot = filename.rfind('.');
  input.ext = dot == std::string::npos ? "" : filename.substr(dot + 1);
  for (char& c : input.ext)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  if (metadata) {
    auto decoded = lightusd::image::LoadImageFromMemoryBounded(
        data, size, filename, kBudget - retained - size);
    if (decoded) {
      input.image = std::move(decoded->image);
      input.reencodable =
          input.image.bpp == 8 || input.image.bpp == 16 ||
          (input.image.bpp == 32 &&
           input.image.format == lightusd::Image::PixelFormat::Float);
    }
  }
  input.original_bytes.assign(data, data + size);
  job->retained +=
      size_t(size) * 2 + size_t(name_size) * 8 + 4096 + decoded_estimate * 4;
  job->name_reserve += size_t(name_size) * 8 +
                       4096;  // reserve fitter's image copies/encode scratch
  job->names.push_back(filename);
  job->inputs.push_back(std::move(input));
  return 1;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_image_fit_finish(uint32_t id,
                                                       const uint8_t* json,
                                                       uint32_t json_size) {
  Job* job = Find(id);
  if (!job || job->finished || !job->error.empty()) return 0;
  JSON config;
  if (!Parse(json, json_size, &config))
    return Error(job, "Invalid texture fit options");
  size_t target, start, min_size, min_quality, quality;
  if (!Integer(config, "targetBytes", 0, 0, kBudget, &target) ||
      !Integer(config, "startMaxSize", 0, 0, 16384, &start) ||
      !Integer(config, "minTextureSize", 64, 1, 16384, &min_size) ||
      !Integer(config, "minQuality", 30, 1, 100, &min_quality) ||
      !Integer(config, "jpegQuality", 90, 1, 100, &quality))
    return Error(job, "Invalid texture fit limits");
  const std::string strategy = config.value("strategy", std::string("size"));
  if (strategy != "size" && strategy != "quality")
    return Error(job, "Unknown texture fit strategy");
  lightusd::tydra::FitTextureOptions opts;
  opts.target_total_bytes = target;
  opts.start_max_size = int(start);
  opts.min_texture_size = int(min_size);
  opts.min_jpeg_quality = int(min_quality);
  opts.jpeg_quality = int(quality);
  opts.strategy = strategy == "quality" ? lightusd::tydra::FitStrategy::Quality
                                        : lightusd::tydra::FitStrategy::Size;
  const std::string encoder = config.value("pngEncoder", std::string("auto"));
  opts.png_encoder = encoder == "fpng"    ? lightusd::image::PngEncoder::Fpng
                     : encoder == "fpnge" ? lightusd::image::PngEncoder::Fpnge
                                          : lightusd::image::PngEncoder::Auto;
  std::string warn;
  if (!lightusd::tydra::FitTexturesToBudget(job->inputs, opts, &job->outputs,
                                            &warn, &job->error))
    return 0;
  size_t total = 0;
  for (const auto& out : job->outputs) {
    if (out.bytes.size() > kBudget - total)
      return Error(job, "Texture fit output memory limit exceeded");
    total += out.bytes.size();
  }
  job->inputs.clear();
  job->retained = total + job->name_reserve;
  job->finished = true;
  job->info["totalBytes"] = double(total);
  job->info["warn"] = warn;
  return 1;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_image_repack_finish(uint32_t id,
                                                          const uint8_t* json,
                                                          uint32_t json_size) {
  Job* job = Find(id);
  if (!job || job->finished || !job->error.empty()) return 0;
  JSON config;
  if (!Parse(json, json_size, &config))
    return Error(job, "Invalid channel pack options");
  size_t width, height, channels, quality;
  if (!Integer(config, "width", 0, 0, 16384, &width) ||
      !Integer(config, "height", 0, 0, 16384, &height) ||
      !Integer(config, "channels", 4, 1, 4, &channels) ||
      !Integer(config, "jpegQuality", 90, 1, 100, &quality))
    return Error(job, "Invalid channel pack dimensions/channels/quality");
  lightusd::tydra::ChannelPackSpec spec;
  spec.out_width = int(width);
  spec.out_height = int(height);
  spec.out_channels = int(channels);
  lightusd::tydra::ChannelSource* slots[] = {&spec.r, &spec.g, &spec.b,
                                             &spec.a};
  const char* names[] = {"r", "g", "b", "a"};
  for (size_t c = 0; c < 4; ++c) {
    const auto* slot = config.find(names[c]);
    if (!slot) continue;
    if (!slot->is_object()) return Error(job, "Invalid channel source");
    size_t index, channel, constant;
    if (!Integer(*slot, "input", 4096, 0, 4096, &index) ||
        !Integer(*slot, "channel", 0, 0, 3, &channel) ||
        !Integer(*slot, "const", c == 3 ? 255 : 0, 0, 255, &constant))
      return Error(job, "Invalid channel source index/value");
    slots[c]->input_index = index == 4096 ? -1 : int(index);
    slots[c]->channel = int(channel);
    slots[c]->constant = uint8_t(constant);
    if (index != 4096 && index >= job->inputs.size())
      return Error(job, "Channel source input is unavailable");
  }
  for (const auto& input : job->inputs) {
    width = std::max(width, size_t(std::max(0, input.image.width)));
    height = std::max(height, size_t(std::max(0, input.image.height)));
  }
  if (width * height >
      (kBudget - std::min(kBudget, job->retained + OtherRetained(id))) / 32)
    return Error(job, "Channel packing working memory limit exceeded");
  std::vector<lightusd::Image> inputs;
  for (const auto& input : job->inputs) inputs.push_back(input.image);
  lightusd::Image packed;
  if (!lightusd::tydra::PackChannels(inputs, spec, &packed, &job->error))
    return 0;
  const std::string format = config.value("format", std::string("png"));
  if (format != "png" && format != "jpeg" && format != "exr")
    return Error(job, "Unknown channel pack output format");
  auto prepared = lightusd::image::PrepareImageForEncoding(
      std::move(packed), format, kBudget - job->retained - OtherRetained(id));
  if (!prepared) return Error(job, prepared.error());
  lightusd::image::WriteOption options;
  options.jpeg_quality = int(quality);
  const std::string encoder = config.value("pngEncoder", std::string("auto"));
  options.png_encoder = encoder == "fpng" ? lightusd::image::PngEncoder::Fpng
                        : encoder == "fpnge"
                            ? lightusd::image::PngEncoder::Fpnge
                            : lightusd::image::PngEncoder::Auto;
  options.format = format == "png"    ? lightusd::image::WriteImageFormat::PNG
                   : format == "jpeg" ? lightusd::image::WriteImageFormat::JPEG
                                      : lightusd::image::WriteImageFormat::EXR;
  auto encoded = lightusd::image::WriteImageToMemory(*prepared, options);
  if (!encoded) return Error(job, encoded.error());
  lightusd::tydra::FitTextureOutput output;
  output.width = prepared->width;
  output.height = prepared->height;
  output.bytes = std::move(*encoded);
  output.ext = format;
  job->outputs.push_back(std::move(output));
  job->inputs.clear();
  job->retained = job->outputs[0].bytes.size() + job->name_reserve;
  job->info["width"] = prepared->width;
  job->info["height"] = prepared->height;
  job->info["channels"] = int(channels);
  job->finished = true;
  return 1;
}
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_image_colorspaces(const uint8_t* data,
                                                         uint32_t size) {
  const uint32_t id = Create();
  Job* job = Find(id);
  if (!job) return 0;
  if (!Span(data, size) || !size || size > kBudget / 2) {
    Error(job, "Invalid texture colorspace layer/input limit");
    return id;
  }
  lightusd::Stage stage;
  std::string warn;
  lightusd::LoadUSDOptions opts;
  opts.usda_options.parse_options.enable_usda_lazy_arrays = true;
  opts.limits.max_input_bytes = kBudget / 2;
  opts.limits.max_resident_bytes = kBudget / 2;
  if (!lightusd::LoadUSDFromMemory(data, size, &stage, opts, &warn,
                                   &job->error))
    return id;
  const lightusd::Layer* layer = stage.GetRootLayer();
  JSON map = JSON::object();
  for (size_t i = 0; layer && i < layer->prim_count(); ++i) {
    const auto* prim = layer->prim(uint32_t(i));
    if (!prim || prim->type_name() != "Shader") continue;
    const auto* shader = prim->property_value("info:id");
    if (!shader || (shader->as_token() != "UsdUVTexture" &&
                    shader->as_string() != "UsdUVTexture"))
      continue;
    std::string cs = "auto";
    if (const auto* value = prim->property_value("inputs:sourceColorSpace")) {
      if (auto token = value->as_token())
        cs = *token;
      else if (auto text = value->as_string())
        cs = *text;
    }
    auto add = [&](const lightusd::Value* value) {
      if (value)
        if (auto asset = value->as_asset_path())
          map[lightusd::AssetResolver::NormalizePath(*asset)] = cs;
    };
    add(prim->property_value("inputs:file"));
    const auto file = lightusd::GetPropNameTable().find("inputs:file");
    if (const auto* samples = prim->time_samples(file))
      for (const auto& sample : *samples)
        add(prim->time_sample_value(sample.second));
  }
  job->info["map"] = std::move(map);
  job->finished = true;
  return id;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_image_job_info(uint32_t id,
                                                     uint8_t* output,
                                                     uint32_t capacity) {
  Job* job = Find(id);
  if (!job) return -1;
  job->info["success"] = job->finished && job->error.empty();
  job->info["error"] = job->error;
  JSON results = JSON::array();
  for (size_t i = 0; i < job->outputs.size(); ++i) {
    const auto& out = job->outputs[i];
    JSON item;
    item["width"] = out.width;
    item["height"] = out.height;
    item["ext"] = out.ext;
    item["changed"] = out.changed;
    if (i < job->names.size()) item["name"] = job->names[i];
    results.push_back(std::move(item));
  }
  job->info["results"] = std::move(results);
  job->text = job->info.dump();
  if (output) {
    if (capacity < job->text.size() || !Span(output, job->text.size()))
      return -1;
    std::memcpy(output, job->text.data(), job->text.size());
  }
  return int32_t(job->text.size());
}
EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_image_job_data(uint32_t id,
                                                       uint32_t index) {
  Job* job = Find(id);
  return job && job->finished && job->error.empty() &&
                 index < job->outputs.size()
             ? reinterpret_cast<uintptr_t>(job->outputs[index].bytes.data())
             : 0;
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_image_job_size(uint32_t id,
                                                     uint32_t index) {
  Job* job = Find(id);
  return job && job->finished && job->error.empty() &&
                 index < job->outputs.size()
             ? int32_t(job->outputs[index].bytes.size())
             : -1;
}
EMSCRIPTEN_KEEPALIVE void lightusd_image_job_release(uint32_t id) {
  jobs.erase(id);
}
}
