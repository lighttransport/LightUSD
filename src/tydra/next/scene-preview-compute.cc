// SPDX-License-Identifier: Apache-2.0
#include "scene-preview-compute.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

#include "next/eval/xform-eval.hh"
#include "next/layer/asset-anchor.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "render-converter.hh"
#include "scene-preview-photometry.hh"
#include "texture-cache.hh"
#include "tydra/vol/volume-field.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace {
namespace kernel {
using uint = uint32_t;
using std::abs;
using std::acos;
using std::asin;
using std::atan2;
using std::cos;
using std::exp;
using std::floor;
using std::log;
using std::max;
using std::min;
using std::pow;
using std::sin;
using std::sqrt;
float Bits(uint32_t x) {
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}
#define FN inline
#define DATA_ARG const uint *inData,
#define DATA_PASS inData,
#define WORK_ARGS , const uint *inData, float *outData
#define BITS Bits
#include "scene-preview-kernel.inc"
#undef FN
#undef DATA_ARG
#undef DATA_PASS
#undef WORK_ARGS
#undef BITS
}  // namespace kernel
class CpuCompute final : public PreviewCompute {
  uint64_t budget_;
  bool debug_validate_;

 public:
  CpuCompute(uint64_t budget, bool debug_validate)
      : budget_(budget), debug_validate_(debug_validate) {}
  const char* name() const override { return "cpu"; }
  bool dispatch(const std::vector<uint32_t>& data, size_t count,
                std::vector<float>* out, std::string* error, bool) override {
    if (!ValidatePreviewDispatch(data, count, error, debug_validate_)) return false;
    if (data.size() * 4 + count * 4 > budget_) {
      *error = "CPU preview compute exceeds budget";
      return false;
    }
    out->resize(count);
    size_t n = count / (data[0] == 2 ? 16 : 4);
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
    const size_t threads = std::max(
        size_t(1),
        std::min(size_t(16), size_t(std::thread::hardware_concurrency())));
    std::vector<std::thread> workers;
    for (size_t t = 0; t < threads; ++t)
      workers.emplace_back([&, t] {
        for (size_t i = n * t / threads; i < n * (t + 1) / threads; ++i)
          kernel::work(static_cast<uint32_t>(i), data.data(), out->data());
      });
    for (auto& thread : workers) thread.join();
#else
    for (size_t i = 0; i < n; ++i)
      kernel::work(static_cast<uint32_t>(i), data.data(), out->data());
#endif
    return true;
  }
};
std::array<float, 3> Temperature(float kelvin) {
  const float t = std::max(1000.f, std::min(40000.f, kelvin)) / 100.f;
  auto clamp = [](float x) { return std::max(0.f, std::min(1.f, x)); };
  return {
      t <= 66 ? 1.f
              : clamp(329.698727446f * std::pow(t - 60, -.1332047592f) / 255),
      t <= 66 ? clamp((99.4708025861f * std::log(t) - 161.1195681661f) / 255)
              : clamp(288.1221695283f * std::pow(t - 60, -.0755148492f) / 255),
      t >= 66   ? 1.f
      : t <= 19 ? 0.f
                : clamp((138.5177312231f * std::log(t - 10) - 305.0447927307f) /
                        255)};
}
void Put(std::vector<uint32_t>& data, size_t index, float value) {
  std::memcpy(&data[index], &value, 4);
}
}  // namespace
#if defined(LIGHTUSD_PREVIEW_VULKAN)
std::unique_ptr<PreviewCompute> CreateVulkanPreviewCompute(uint64_t,
    std::string*, const PreviewComputeOptions&);
#endif
#if defined(LIGHTUSD_PREVIEW_CUDA)
std::unique_ptr<PreviewCompute> CreateCudaPreviewCompute(uint64_t,
    std::string*, const PreviewComputeOptions&);
#endif
bool ValidatePreviewDispatch(const std::vector<uint32_t>& data, size_t count,
                             std::string* error, bool debug_validate) {
  auto fail = [&](const char* why = nullptr) {
    if (debug_validate)
      std::fprintf(stderr,
                   "[validate] reason=%s mode=%u size=%zu count=%zu "
                   "h1=%u h2=%u h3=%u limit=%zu\n",
                   why ? why : "-", data.empty() ? 0u : data[0], data.size(),
                   count, data.size() > 1 ? data[1] : 0u,
                   data.size() > 2 ? data[2] : 0u,
                   data.size() > 3 ? data[3] : 0u,
                   data.size() > 16 ? (data.size() - 16) / 64 : 0u);
    if (error)
      *error = why ? std::string("invalid preview compute buffers: ") + why
                   : std::string("invalid preview compute buffers");
    return false;
  };
  auto scalar = [&](size_t i) { return kernel::Bits(data[i]); };
  if (data.size() < 16 || data.size() > UINT32_MAX || !count ||
      count > UINT32_MAX || count % 4)
    return fail("dispatch header or output count");
  if (data[0] == 0) {
    const uint64_t dim = data[1], samples = data[2];
    if (!dim || dim > 1024 || dim * dim * dim * 4 != count ||
        samples > (data.size() - 16) / 12 || !std::isfinite(scalar(3)) ||
        scalar(3) <= 0)
      return fail("voxel conversion header");
    for (size_t i = 16; i < 16 + size_t(samples) * 12; ++i)
      if (!std::isfinite(scalar(i))) return fail();
    return true;
  }
  if (data[0] == 2) {
    if (!data[1] || !data[2] || uint64_t(data[1]) * 16 != count ||
        data[2] > (data.size() - 16) / 64)
      return fail("geometry batch header");
    uint64_t samples = 0;
    for (size_t t = 0; t < data[2]; ++t) {
      const size_t m = 16 + t * 64;
      const uint64_t n = data[m + 49], w = data[m + 51];
      if (!n || data[m + 48] != samples || w > data.size() ||
          n > (data.size() - w) / 3)
        return fail("geometry batch payload");
      samples += n;
      if (samples > data[1]) return fail();
      for (size_t k = 0; k < 48; ++k)
        if (!std::isfinite(scalar(m + k))) {
          if (debug_validate)
            std::fprintf(stderr,
                         "[validate] geometry record %zu lane %zu value=%f\n",
                         t, k, double(scalar(m + k)));
          return fail("geometry record values");
        }
      if (!std::isfinite(scalar(m + 50)) || scalar(m + 50) <= 0)
        return fail("geometry batch cell size");
      for (size_t k = 0; k < n * 3; ++k)
        if (!std::isfinite(scalar(size_t(w) + k))) return fail();
    }
    return samples == data[1] ? true : fail();
  }
  if (data[0] != 1 || data.size() < 64 || !data[1] || !data[2] ||
      uint64_t(data[1]) * data[2] * 4 != count ||
      data[3] > (data.size() - 64) / 16)
    return fail();
  // Scalar-volume header: receiver prim count for light/shadow linking. Geometry
  // and voxel-conversion buffers reuse this index as payload.
  if (data[47] > 4096) return fail();
  for (size_t i : {size_t(4), size_t(8), size_t(12), size_t(16), size_t(20)})
    for (size_t k = 0; k < 3; ++k)
      if (!std::isfinite(scalar(i + k))) return fail();
  if (data[25] > 1024 || data[26] > data.size() ||
      data[25] > (data.size() - data[26]) / 40 || data[32] > 65536 ||
      data[39] > 65536)
    return fail();
  if (data[35] > 256 || data[36] > 64 || (data[35] > 1 && !data[36]) ||
      data[38] > 64)
    return fail();
  if (!std::isfinite(scalar(42)) || scalar(42) < 0 || scalar(42) > 2 ||
      !std::isfinite(scalar(43)) || scalar(43) < 0 || scalar(43) > 1)
    return fail();
  if (!std::isfinite(scalar(46)) || scalar(46) < 0 || scalar(46) > 1)
    return fail();
  for (size_t i = 28; i < 32; ++i)
    if (!std::isfinite(scalar(i))) return fail();
  for (size_t l = 0; l < data[25]; ++l) {
    const size_t m = data[26] + l * 40;
    for (size_t k = 20; k < 31; ++k)
      if (!std::isfinite(scalar(m + k))) return fail();
    for (size_t k = 0; k < 16; ++k)
      if (k != 3 && k != 12 && k != 15 && !std::isfinite(scalar(m + k)))
        return fail();
    if (data[m + 3] > 8) return fail();
    const uint64_t ies = data[m + 32];
    if (ies) {
      if (ies > data.size() || 9 + 32 * 16 > data.size() - ies) return fail();
      for (size_t k = 0; k < 9 + 32 * 16; ++k)
        if (!std::isfinite(scalar(size_t(ies) + k))) return fail();
      for (size_t axis = 0; axis < 3; ++axis) {
        float length = 0;
        for (size_t k = 0; k < 3; ++k) {
          const float value = scalar(size_t(ies) + axis * 3 + k);
          length += value * value;
        }
        if (length < 1e-6f) return fail();
      }
    }
    for (size_t which = 0; which < 2; ++which) {
      const uint64_t pointer = data[m + 33 + which];
      if (!pointer) continue;
      const uint64_t words = (uint64_t(data[47]) + 32) / 32;
      if (!data[47] || pointer > data.size() || words > data.size() - pointer)
        return fail();
    }
    if (data[m + 31] && !data[m + 16]) return fail();
    if (data[m + 16]) {
      const uint64_t w = data[m + 17], h = data[m + 18], off = data[m + 16];
      if (!w || !h || w > 256 || h > 256 || off > data.size() ||
          w * h > (data.size() - off) / 3 || data[m + 19] > 3)
        return fail();
      for (size_t k = 20; k < 31; ++k)
        if (!std::isfinite(scalar(m + k))) return fail();
      for (size_t k = 0; k < w * h * 3; ++k)
        if (!std::isfinite(scalar(size_t(off) + k))) return fail();
      if (data[m + 31]) {
        const uint64_t cdf = data[m + 31], n = w * h;
        if (!((data[m + 3] == 5 && data[m + 19] >= 1 && data[m + 19] <= 3) ||
              data[m + 3] == 3) ||
            cdf > data.size() || n + 1 > data.size() - cdf ||
            scalar(cdf) != 0 || scalar(cdf + n) != 1)
          return fail();
        float previous = 0;
        for (size_t k = 1; k <= n; ++k) {
          const float value = scalar(cdf + k);
          if (!std::isfinite(value) || value < previous || value > 1)
            return fail();
          previous = value;
        }
      }
    }
  }
  if (data[40]) {
    const uint64_t offset = data[40], nodes = data[41];
    if (!data[3] || nodes != uint64_t(data[3]) * 2 - 1 ||
        offset > data.size() || nodes > (data.size() - offset) / 8)
      return fail();
    for (size_t n = 0; n < nodes; ++n) {
      const size_t node = offset + n * 8;
      for (size_t k = 0; k < 3; ++k)
        if (!std::isfinite(scalar(node + k)) ||
            !std::isfinite(scalar(node + 3 + k)) ||
            scalar(node + k) > scalar(node + 3 + k))
          return fail();
      const uint32_t left = data[node + 6], right = data[node + 7];
      if (left & 0x80000000u) {
        const uint32_t m = left & 0x7fffffffu;
        if (m < 64 || (m - 64) % 16 || (m - 64) / 16 >= data[3]) return fail();
      } else if (left != n + 1 || right < n + 3)
        return fail();
      if (right <= n || right > nodes) return fail();
    }
  }
  for (size_t b = 0; b < data[3]; ++b) {
    const size_t m = 64 + b * 16;
    const uint64_t nx = data[m + 4], ny = data[m + 5], nz = data[m + 6],
                   offset = data[m + 7];
    if (!nx || !ny || !nz || nx > 1024 || ny > 1024 || nz > 1024 ||
        offset > data.size() || nx * ny * nz > (data.size() - offset) / 4)
      return fail();
    for (size_t k = 0; k < 4; ++k)
      if (!std::isfinite(scalar(m + k))) return fail();
    if (scalar(m + 3) <= 0 || data[m + 10] > 1 || data[m + 11] > data[47])
      return fail();
  }
  return true;
}

std::unique_ptr<PreviewCompute> CreatePreviewCompute(
    PreviewComputeBackend backend, uint64_t budget, std::string* error,
    const PreviewComputeOptions& options) {
  std::string ignored;
  if (!error) error = &ignored;
  const bool software_only = options.software_only;
  if (software_only && backend == PreviewComputeBackend::Cuda) {
    *error = "hardware compute is disabled in software development mode";
    return {};
  }
#if defined(LIGHTUSD_PREVIEW_CUDA)
  if (!software_only && (backend == PreviewComputeBackend::Auto ||
                         backend == PreviewComputeBackend::Cuda)) {
    auto engine = CreateCudaPreviewCompute(budget, error, options);
    if (engine) return engine;
    if (backend == PreviewComputeBackend::Cuda) return {};
  }
#endif
#if defined(LIGHTUSD_PREVIEW_VULKAN)
  if (backend == PreviewComputeBackend::Auto ||
      backend == PreviewComputeBackend::Vulkan) {
    auto engine = CreateVulkanPreviewCompute(budget, error, options);
    if (engine) return engine;
    if (backend == PreviewComputeBackend::Vulkan) return {};
  }
#endif
  if (backend != PreviewComputeBackend::Auto &&
      backend != PreviewComputeBackend::Cpu) {
    *error = "requested preview compute backend was not built";
    return {};
  }
  error->clear();
  return std::make_unique<CpuCompute>(budget, options.debug_validate);
}
struct PreviewVolumeRenderer::Impl {
  std::vector<uint32_t> data;
  std::unique_ptr<PreviewCompute> compute;
  bool uploaded = false;
  bool have_image = false;
  PreviewVolumeCamera last_camera;
  int last_width = 0, last_height = 0;
  std::vector<uint8_t> last_image;
  std::vector<float> linear_average;
  uint32_t accumulated_frames = 0;
  uint64_t budget = 0;
  bool density_only = false;
};
PreviewVolumeRenderer::PreviewVolumeRenderer() : impl_(new Impl) {}
PreviewVolumeRenderer::~PreviewVolumeRenderer() = default;
uint32_t PreviewVolumeRenderer::accumulatedFrames() const {
  return impl_->accumulated_frames;
}
const char* PreviewVolumeRenderer::backend() const {
  return impl_->compute ? impl_->compute->name() : "uninitialized";
}
bool PreviewVolumeRenderer::load(const ScenePreviewResult& result,
                                 PreviewComputeBackend backend, uint64_t budget,
                                 std::string* error,
                                 const PreviewComputeOptions& compute_options) {
  namespace usd = ::lightusd::next;
  impl_.reset(new Impl);
  impl_->budget = budget;
  impl_->compute = CreatePreviewCompute(backend, budget, error, compute_options);
  if (!impl_->compute) return false;
  usd::Stage stage;
  usd::AssetResolver resolver;
  usd::pcp::CompositionOptions options;
  options.num_threads = 1;
  std::string warning;
  if (!usd::pcp::ComposeStageFromFile(result.usd_path, resolver, &stage,
                                      options, &warning, error))
    return false;
  struct Field {
    std::string path, name;
    std::array<double, 16> world;
    uint32_t group = 0;
    uint32_t prim = 0;
    bool independent = true;
  };
  std::map<std::string, Field> fields;
  std::map<std::string, uint32_t> overlap_groups;
  // Dense receiver ids (1-based) for light/shadow linking, one per source
  // volume prim path.
  std::map<std::string, uint32_t> prim_ids;
  struct LightSource {
    RenderLight value;
    std::string texture;
    uint32_t format = 0;
    bool srgb = true;
    std::string ies;
    std::vector<std::string> light_links, shadow_links;
  };
  std::vector<LightSource> lights;
  RenderSceneConverter converter;
  stage.Traverse([&](const usd::UsdPrim& prim) {
    if (prim.GetTypeName().find("Light") != std::string::npos) {
      RenderLight light;
      if (converter.ConvertLight(stage, prim, &light)) {
        double world[16];
        usd::ComputeWorldTransform(stage, prim, world);
        for (size_t k = 0; k < 16; ++k) light.transform.m[k] = float(world[k]);
        LightSource item;
        item.value = std::move(light);
        if (const auto* asset = prim.GetPropertyValue("inputs:texture:file"))
          if (asset->as_asset_path()) {
            const auto* spec = prim.GetPrimSpec();
            const auto anchor =
                spec ? usd::AssetAnchorPath(spec->asset_anchor_id()) : "";
            const auto base =
                anchor.empty()
                    ? std::filesystem::path(result.usd_path).parent_path()
                    : std::filesystem::path(anchor);
            item.texture = anchor.empty()
                ? (base / std::string(asset->as_asset_path())).lexically_normal().string()
                : usd::ResolveAssetAnchorPath(std::string(asset->as_asset_path()), spec->asset_anchor_id());
            if (spec)
              if (const auto* meta = spec->property_meta("inputs:texture:file"))
                item.srgb = meta->colorSpace != "raw" &&
                            meta->colorSpace != "linear" &&
                            meta->colorSpace != "data" &&
                            meta->colorSpace != "lin_rec709_scene";
          }
        if (!item.value.shaping_ies_file.empty()) {
          const auto* spec = prim.GetPrimSpec();
          const auto anchor =
              spec ? usd::AssetAnchorPath(spec->asset_anchor_id()) : "";
          const auto base =
              anchor.empty()
                  ? std::filesystem::path(result.usd_path).parent_path()
                  : std::filesystem::path(anchor);
          item.ies = anchor.empty()
              ? (base / item.value.shaping_ies_file).lexically_normal().string()
              : usd::ResolveAssetAnchorPath(item.value.shaping_ies_file, spec->asset_anchor_id());
        }
        auto read_links = [&](const char* first, const char* second) {
          std::vector<std::string> out;
          for (const char* name : {first, second}) {
            if (const auto* targets = prim.GetRelationship(name)) {
              for (const auto& target : *targets) out.push_back(target.str());
              if (!out.empty()) break;
            }
          }
          return out;
        };
        item.light_links =
            read_links("light:link", "collection:lightLink:includes");
        item.shadow_links =
            read_links("shadow:link", "collection:shadowLink:includes");
        if (item.value.type == LightType::Dome) {
          item.format = 1;
          if (item.value.params.dome.texture_format ==
              RenderLight::DomeTextureFormat::Angular)
            item.format = 2;
          if (item.value.params.dome.texture_format ==
              RenderLight::DomeTextureFormat::MirroredBall)
            item.format = 3;
        }
        lights.push_back(std::move(item));
      }
    }
    if (prim.GetTypeName() != "OpenVDBAsset") return true;
    auto* name = prim.GetPropertyValue("fieldName");
    auto* path = prim.GetPropertyValue("filePath");
    if (!name || !path || !path->as_asset_path()) return true;
    const std::string field = name->as_token();
    if (field != "surface" && field != "density") return true;
    const auto parent =
        prim.GetPath().str().substr(0, prim.GetPath().str().rfind('/'));
    if (!fields.count(parent) || field == "surface") {
      Field selected;
      const auto* spec = stage.GetRootLayer()->prim_at_path(prim.GetPath());
      const std::string anchor =
          spec ? usd::AssetAnchorPath(spec->asset_anchor_id()) : "";
      const auto base =
          anchor.empty() ? std::filesystem::path(result.usd_path).parent_path()
                         : std::filesystem::path(anchor);
      selected.path = anchor.empty()
          ? (base / std::string(path->as_asset_path())).lexically_normal().string()
          : usd::ResolveAssetAnchorPath(std::string(path->as_asset_path()), spec->asset_anchor_id());
      selected.name = field;
      const auto volume = stage.GetPrimAtPath(usd::Path(parent));
      const auto* marker =
          volume ? volume.GetPropertyValue("lightusd:preview:overlapGroup")
                 : nullptr;
      const bool legacy = parent.rfind("/ScenePreview/", 0) == 0;
      selected.independent = !(marker && marker->as_token()) && !legacy;
      const std::string group =
          selected.independent
              ? parent
              : (parent.substr(0, parent.rfind('/')) + ":" +
                 (marker && marker->as_token() ? marker->as_token()
                                               : "surface"));
      auto found = overlap_groups.emplace(
          group, static_cast<uint32_t>(overlap_groups.size() + 1));
      selected.group = found.first->second;
      selected.prim =
          prim_ids.emplace(parent, static_cast<uint32_t>(prim_ids.size() + 1))
              .first->second;

      usd::ComputeWorldTransform(stage, prim, selected.world.data());
      fields[parent] = std::move(selected);
    }
    return true;
  });
  if (fields.empty() || lights.size() > 1024) {
    *error = "preview has no UsdVol surface/density grids or too many lights";
    return false;
  }
  std::vector<Field> files;
  for (const auto& field : fields) files.push_back(field.second);
  impl_->density_only =
      std::all_of(files.begin(), files.end(),
                  [](const Field& f) { return f.name == "density"; });
  auto& data = impl_->data;
  data.resize(64 + files.size() * 16);
  data[0] = 1;
  data[3] = static_cast<uint32_t>(files.size());
  data[25] = static_cast<uint32_t>(lights.size());
  data[26] = static_cast<uint32_t>(data.size());
  const uint64_t reserve = std::min(budget / 2, uint64_t(512) << 20);
  TextureDecodeOptions decoding;
  decoding.max_edge = 256;
  decoding.budget_bytes = std::min(uint64_t(32) << 20, budget / 16);
  decoding.max_source_bytes = uint64_t(256) << 20;
  TextureDecoder decoder(decoding);
  std::map<std::string, std::array<uint32_t, 3>> texture_records;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> importance_records;
  // Reserve all light records before appending shared texture payloads.
  const size_t lights_start = data.size();
  // 40 lanes per light: the 32 kernel fields plus the IES profile-block offset
  // at lane 32 (0 = unshaped). The remaining lanes are reserved.
  data.resize(data.size() + lights.size() * 40);
  for (size_t light_index = 0; light_index < lights.size(); ++light_index) {
    const auto& item = lights[light_index];
    const auto& light = item.value;
    const size_t m = lights_start + light_index * 40;
    data[m + 3] = static_cast<uint32_t>(light.type);
    for (size_t k = 0; k < 3; ++k) {
      Put(data, m + k, light.transform.m[12 + k]);
      Put(data, m + 4 + k, -light.transform.m[8 + k]);
    }
    const float power =
        std::max(0.f, light.intensity) *
        std::exp2(std::max(-30.f, std::min(30.f, light.exposure))) *
        std::max(0.f, light.diffuse);
    const auto temperature = light.enable_color_temperature
                                 ? Temperature(light.color_temperature)
                                 : std::array<float, 3>{1, 1, 1};
    Put(data, m + 8, power * std::max(0.f, light.color.x) * temperature[0]);
    Put(data, m + 9, power * std::max(0.f, light.color.y) * temperature[1]);
    Put(data, m + 10, power * std::max(0.f, light.color.z) * temperature[2]);
    const float pi = 3.141592653589793f;
    float area = 1;
    if (light.type == LightType::Rect)
      area = light.params.rect.width * light.params.rect.height;
    if (light.type == LightType::Disk)
      area = pi * light.params.disk.radius * light.params.disk.radius;
    if (light.type == LightType::Sphere)
      area = 4 * pi * light.params.sphere.radius * light.params.sphere.radius;
    if (light.type == LightType::Cylinder)
      area =
          2 * pi * light.params.cylinder.radius * light.params.cylinder.length;
    Put(data, m + 7, std::sqrt(std::max(0.f, area) / pi));
    if (light.type == LightType::Directional)
      Put(data, m + 7,
          std::max(0.f, std::min(360.f, light.params.distant.angle)) * pi /
              360);
    Put(data, m + 11, std::cos(light.shaping_cone_angle * pi / 180.f));
    data[m + 12] = light.enable_shadow ? 1 : 0;
    Put(data, m + 13, std::max(0.f, light.shaping_cone_softness));
    Put(data, m + 14, std::max(1e-6f, area));
    data[m + 15] = light.normalize ? 1 : 0;
    for (size_t a = 0; a < 3; ++a) {
      float axis[3] = {light.transform.m[a * 4], light.transform.m[a * 4 + 1],
                       light.transform.m[a * 4 + 2]};
      float length =
          std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
      for (size_t k = 0; k < 3; ++k)
        Put(data, m + 20 + a * 4 + k,
            length > 1e-20f ? axis[k] / length : float(a == k));
    }
    if (light.type == LightType::Rect) {
      const auto& mat = light.transform.m;
      const float sx =
          std::sqrt(mat[0] * mat[0] + mat[1] * mat[1] + mat[2] * mat[2]);
      const float sy =
          std::sqrt(mat[4] * mat[4] + mat[5] * mat[5] + mat[6] * mat[6]);
      Put(data, m + 23, light.params.rect.width * sx);
      Put(data, m + 27, light.params.rect.height * sy);
      Put(data, m + 14, std::max(1e-6f, area * sx * sy));
    }
    {
      // Store transformed shape dimensions in the spare axis lanes. Surface
      // Jacobians retain nonuniform scale and shear in the integration.
      const auto& mat = light.transform.m;
      const auto axis = [&](size_t a) {
        return std::array<float, 3>{mat[a * 4], mat[a * 4 + 1], mat[a * 4 + 2]};
      };
      const auto cross = [](const auto& a, const auto& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1],
                                    a[2] * b[0] - a[0] * b[2],
                                    a[0] * b[1] - a[1] * b[0]};
      };
      const auto length = [](const auto& a) {
        return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
      };
      const auto ax = axis(0), ay = axis(1), az = axis(2);
      const float sx = length(ax), sy = length(ay), sz = length(az);
      if (light.type == LightType::Rect)
        Put(data, m + 14, std::max(1e-12f, area * length(cross(ax, ay))));
      if (light.type == LightType::Disk) {
        Put(data, m + 23, light.params.disk.radius * sx);
        Put(data, m + 27, light.params.disk.radius * sy);
        Put(data, m + 14, std::max(1e-12f, area * length(cross(ax, ay))));
      }
      if (light.type == LightType::Sphere || light.type == LightType::Spot) {
        const float radius = light.type == LightType::Sphere
                                 ? light.params.sphere.radius
                                 : light.spot_source_radius;
        Put(data, m + 23, radius * sx);
        Put(data, m + 27, radius * sy);
        Put(data, m + 7, radius * sz);
        const auto c0 = cross(ay, az), c1 = cross(az, ax), c2 = cross(ax, ay);
        double sum = 0;
        for (size_t q = 0; q < 512; ++q) {
          const double z = 1 - 2 * (q + .5) / 512, phi = q * 2.399963229728653,
                       sine = std::sqrt(std::max(0., 1 - z * z));
          const double direction[] = {sine * std::cos(phi),
                                      sine * std::sin(phi), z};
          double square = 0;
          for (size_t k = 0; k < 3; ++k) {
            const double value = direction[0] * c0[k] + direction[1] * c1[k] +
                                 direction[2] * c2[k];
            square += value * value;
          }
          sum += std::sqrt(square);
        }
        Put(data, m + 14,
            std::max(1e-12f, float(sum * (4 * pi / 512) * radius * radius)));
      }
      if (light.type == LightType::Cylinder) {
        const float radius = light.params.cylinder.radius,
                    extent = light.params.cylinder.length;
        Put(data, m + 23, extent * sx);
        Put(data, m + 27, radius * sy);
        Put(data, m + 7, radius * sz);
        double sum = 0;
        for (size_t q = 0; q < 512; ++q) {
          const double phi = (q + .5) * 2 * pi / 512;
          std::array<float, 3> tangent{};
          for (size_t k = 0; k < 3; ++k)
            tangent[k] = float(-ay[k] * radius * std::sin(phi) +
                               az[k] * radius * std::cos(phi));
          sum += length(cross(ax, tangent)) * extent;
        }
        Put(data, m + 14, std::max(1e-12f, float(sum * (2 * pi / 512))));
      }
    }
    if (!item.texture.empty() &&
        (light.type == LightType::Dome || light.type == LightType::Rect)) {
      const std::string key = item.texture + (item.srgb ? ":srgb" : ":linear");
      auto found = texture_records.find(key);
      if (found == texture_records.end()) {
        DecodedImage image;
        if (!decoder.Decode(item.texture, item.srgb, &image) || image.empty()) {
          *error = "cannot decode light emission texture: " + item.texture;
          return false;
        }
        const size_t n = size_t(image.width) * image.height;
        if (data.size() > UINT32_MAX - n * 3 ||
            uint64_t(data.size() + n * 3) * 8 > budget - reserve) {
          *error = "light emission textures exceed preview budget";
          return false;
        }
        const auto offset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + n * 3);
        for (size_t i = 0; i < n; ++i)
          for (size_t k = 0; k < 3; ++k) {
            float x =
                image.hdr
                    ? image.float_pixels[i * 3 + k]
                    : image.pixels[i * image.channels +
                                   std::min(k, size_t(image.channels - 1))] /
                          255.f;
            if (!image.hdr && item.srgb)
              x = x <= .04045f ? x / 12.92f
                               : std::pow((x + .055f) / 1.055f, 2.4f);
            if (!std::isfinite(x)) {
              *error = "non-finite light emission texel";
              return false;
            }
            Put(data, offset + i * 3 + k, std::max(0.f, x));
          }
        found = texture_records
                    .emplace(key, std::array<uint32_t, 3>{offset, image.width,
                                                          image.height})
                    .first;
      }
      data[m + 16] = found->second[0];
      data[m + 17] = found->second[1];
      data[m + 18] = found->second[2];
      data[m + 19] = item.format;
      if (light.type == LightType::Rect || light.type == LightType::Dome) {
        const auto key = std::make_pair(
            data[m + 16], light.type == LightType::Dome ? item.format : 0u);
        auto found_cdf = importance_records.find(key);
        if (found_cdf == importance_records.end()) {
          const size_t w = data[m + 17], h = data[m + 18], n = w * h;
          if (data.size() > UINT32_MAX - (n + 1) ||
              uint64_t(data.size() + n + 1) * 8 > budget - reserve) {
            *error = "light importance tables exceed preview budget";
            return false;
          }
          std::vector<double> prefix(n + 1, 0);
          for (size_t j = 0; j < n; ++j) {
            const auto rgb = data[m + 16] + j * 3;
            float color[3];
            for (size_t k = 0; k < 3; ++k)
              std::memcpy(&color[k], &data[rgb + k], 4);
            double solid = 1;
            if (key.second == 1)
              solid = std::cos(double(j / w) * pi / h) -
                      std::cos(double(j / w + 1) * pi / h);
            if (key.second >= 2) {
              // Sampling density may approximate this integral; the kernel
              // divides by its exact discrete probability and evaluates the
              // spherical Jacobian at each sample, preserving expected energy.
              solid = 0;
              for (size_t yy = 0; yy < 4; ++yy)
                for (size_t xx = 0; xx < 4; ++xx) {
                  const double u = (double(j % w) + (xx + .5) / 4) / w - .5;
                  const double v = (double(j / w) + (yy + .5) / 4) / h - .5;
                  const double r = std::sqrt(u * u + v * v);
                  if (r < .5)
                    solid +=
                        key.second == 3
                            ? 16.
                            : (r < 1e-9 ? 4 * pi * pi
                                        : 2 * pi * std::sin(2 * pi * r) / r);
                }
              solid /= 16;
              // Do not discard tiny intersections with the valid projection
              // disk merely because all quadrature samples missed its edge.
              const double u0 = double(j % w) / w - .5,
                           u1 = double(j % w + 1) / w - .5;
              const double v0 = double(j / w) / h - .5,
                           v1 = double(j / w + 1) / h - .5;
              const double u = std::max(u0, std::min(0., u1)),
                           v = std::max(v0, std::min(0., v1));
              if (u * u + v * v < .25) solid = std::max(solid, 1e-5);
            }
            prefix[j + 1] =
                prefix[j] + solid * (.2126 * color[0] + .7152 * color[1] +
                                     .0722 * color[2]);
          }
          uint32_t cdf = 0;
          if (prefix.back() > 0) {
            cdf = static_cast<uint32_t>(data.size());
            data.resize(data.size() + n + 1);
            for (size_t j = 0; j <= n; ++j)
              Put(data, cdf + j, static_cast<float>(prefix[j] / prefix.back()));
          }
          found_cdf = importance_records.emplace(key, cdf).first;
        }
        data[m + 31] = found_cdf->second;
      }
    }
  }
  // Photometric profiles are resampled once into a 32x16 candela table in the
  // light's local frame (x,y,z axes followed by the table, all floats). The
  // shared kernel interpolates it, so CPU, Vulkan and CUDA agree.
  for (size_t light_index = 0; light_index < lights.size(); ++light_index) {
    const auto& item = lights[light_index];
    if (item.ies.empty()) continue;
    PreviewIesProfile profile;
    std::string ies_error;
    if (!LoadPreviewIes(item.ies, &profile, &ies_error)) {
      *error = "cannot load light IES profile: " + item.ies + " (" +
               ies_error + ")";
      return false;
    }
    const size_t base = data.size();
    if (base + 9 + 32 * 16 > UINT32_MAX ||
        uint64_t(base + 9 + 32 * 16) * 4 > budget - reserve) {
      *error = "light IES profiles exceed preview budget";
      return false;
    }
    data.resize(base + 9 + 32 * 16);
    const auto& mat = item.value.transform.m;
    for (size_t axis = 0; axis < 3; ++axis) {
      const float xyz[3] = {mat[axis * 4], mat[axis * 4 + 1],
                            mat[axis * 4 + 2]};
      const float length = std::sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] +
                                     xyz[2] * xyz[2]);
      for (size_t k = 0; k < 3; ++k)
        Put(data, base + axis * 3 + k,
            length > 1e-20f ? xyz[k] / length : float(axis == k));
    }
    const float pi = 3.141592653589793f;
    for (size_t v = 0; v < 32; ++v) {
      const float theta = pi * (v + .5f) / 32.f;
      for (size_t h = 0; h < 16; ++h) {
        const float phi = 2 * pi * (h + .5f) / 16.f;
        const std::array<float, 3> direction = {
            std::sin(theta) * std::cos(phi),
            std::sin(theta) * std::sin(phi), -std::cos(theta)};
        const float value =
            profile.sample(direction, item.value.shaping_ies_angle_scale,
                           item.value.shaping_ies_normalize);
        Put(data, base + 9 + v * 16 + h,
            std::isfinite(value) ? std::max(0.f, value) : 0.f);
      }
    }
    data[lights_start + light_index * 40 + 32] =
        static_cast<uint32_t>(base);
  }
  // Light/shadow linking masks. Ids are 1-based, so a 32-bit word covers ids
  // 1..31 and one trailing word covers the remainder. A zero pointer means the
  // light links everything.
  const uint32_t prim_count = static_cast<uint32_t>(prim_ids.size());
  data[47] = prim_count;
  const size_t link_words = (size_t(prim_count) + 32u) / 32u;
  if (link_words) {
    for (size_t light_index = 0; light_index < lights.size(); ++light_index) {
      const auto& item = lights[light_index];
      const size_t m = lights_start + light_index * 40;
      for (size_t which = 0; which < 2; ++which) {
        const auto& links = which == 0 ? item.light_links : item.shadow_links;
        if (links.empty()) continue;
        std::vector<uint32_t> mask(link_words, 0);
        for (const auto& target : links) {
          const auto found = prim_ids.find(target);
          if (found == prim_ids.end()) continue;
          mask[found->second >> 5] |= 1u << (found->second & 31u);
        }
        if (data.size() + link_words > UINT32_MAX ||
            uint64_t(data.size() + link_words) * 4 > budget - reserve) {
          *error = "light link masks exceed preview budget";
          return false;
        }
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.resize(data.size() + link_words);
        for (size_t w = 0; w < link_words; ++w) data[offset + w] = mask[w];
        data[m + 33 + which] = offset;
      }
    }
  }
  for (size_t b = 0; b < files.size(); ++b) {
    vol::FieldCache cache({}, uint64_t(32) << 20);
    vol::FieldRequest req;
    req.prim_type = "OpenVDBAsset";
    req.file_path = files[b].path;
    req.field_name = files[b].name;
    req.field_class = files[b].name == "surface" ? "levelSet" : "fogVolume";
    vol::ResolvedField field;
    usdVol::VDBGrid grid;
    usdVol::VDBReadOptions read;
    read.max_voxels = 1000000;
    if (!cache.Resolve(req, &field, error) ||
        !cache.TakeDense(field, read, &grid, &warning, error))
      return false;
    if (grid.components != 1 || grid.stride != 1 || grid.data.empty()) {
      *error = "invalid preview scalar grid";
      return false;
    }
    const auto& world = files[b].world;
    const double scale = world[0];
    bool aligned = std::isfinite(scale) && scale > 0;
    for (double x : world) aligned = aligned && std::isfinite(x);
    for (size_t k = 0; k < 3; ++k)
      aligned = aligned && std::isfinite(grid.index_to_world[k][3]);
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 3; ++c)
        aligned = aligned &&
                  std::abs(world[c * 4 + r] - (r == c ? scale : 0)) < 1e-8 &&
                  std::abs(grid.index_to_world[r][c] -
                           (r == c ? grid.voxel_size[0] : 0)) < 1e-8;
    if (!aligned) {
      *error = "direct preview grids require axis-aligned uniform transforms";
      return false;
    }
    const size_t n = grid.data.size();
    if (n > 1000000 || data.size() > UINT32_MAX - n * 4 ||
        uint64_t(data.size() + n * 4) * 8 > budget - reserve) {
      *error = "direct volume grids exceed preview budget";
      return false;
    }
    const size_t meta = 64 + b * 16;
    data[meta + 7] = static_cast<uint32_t>(data.size());
    data[meta + 8] = files[b].name == "density" ? 1 : 0;
    data[meta + 9] = files[b].group;
    data[meta + 10] = files[b].independent ? 1u : 0u;
    data[meta + 11] = files[b].prim;
    const double cell = grid.voxel_size[0] * scale;
    for (size_t k = 0; k < 3; ++k) {
      Put(data, meta + k,
          static_cast<float>(world[12 + k] +
                             scale *
                                 (grid.index_to_world[k][3] +
                                  grid.index_to_world[k][k] * grid.origin[k])));
      data[meta + 4 + k] = static_cast<uint32_t>(grid.dim[k]);
    }
    Put(data, meta + 3, static_cast<float>(cell));
    std::vector<double> points(n * 3);
    for (size_t i = 0; i < n; ++i) {
      const size_t xyz[] = {i % size_t(grid.dim[0]),
                            (i / size_t(grid.dim[0])) % size_t(grid.dim[1]),
                            i / (size_t(grid.dim[0]) * size_t(grid.dim[1]))};
      for (size_t k = 0; k < 3; ++k)
        points[i * 3 + k] = grid.index_to_world[k][3] +
                            grid.index_to_world[k][k] *
                                (grid.origin[k] + static_cast<double>(xyz[k]));
    }
    req.field_name = "albedo";
    req.field_class.clear();
    req.field_data_type = "float3";
    std::vector<float> colors;
    int components = 0;
    std::string optional_error;
    if (!cache.Resolve(req, &field, &optional_error) ||
        !cache.SampleWorld(field, points.data(), n, true, &colors, &components,
                           &optional_error) ||
        components != 3) {
      colors.assign(n * 3, 1.f);
    }
    const size_t start = data.size();
    data.resize(start + n * 4);
    for (size_t i = 0; i < n; ++i) {
      if (!std::isfinite(grid.data[i]) || !std::isfinite(colors[i * 3]) ||
          !std::isfinite(colors[i * 3 + 1]) ||
          !std::isfinite(colors[i * 3 + 2])) {
        *error = "non-finite volume grid texel";
        return false;
      }
      Put(data, start + i * 4,
          grid.data[i] * (files[b].name == "surface" ? float(scale) : 1.f));
      for (size_t k = 0; k < 3; ++k)
        Put(data, start + i * 4 + 1 + k, colors[i * 3 + k]);
    }
  }
  // Median-split field bounds index. Scalar/GPU kernels share this table.
  const size_t nodes = files.size() * 2 - 1, offset = data.size();
  if (offset > UINT32_MAX - nodes * 8 ||
      uint64_t(offset + nodes * 8) * 8 > budget - reserve) {
    *error = "volume spatial index exceeds preview budget";
    return false;
  }
  data[40] = static_cast<uint32_t>(offset);
  data[41] = static_cast<uint32_t>(nodes);
  data.resize(offset + nodes * 8);
  std::vector<uint32_t> order(files.size());
  for (size_t b = 0; b < files.size(); ++b)
    order[b] = static_cast<uint32_t>(64 + b * 16);
  uint32_t next_node = 0;
  auto build_bounds = [&](auto&& self, size_t first, size_t last) -> uint32_t {
    const uint32_t index = next_node++;
    const size_t node = offset + index * 8;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t b = first; b < last; ++b) {
      const uint32_t m = order[b];
      for (size_t k = 0; k < 3; ++k) {
        const float a = kernel::Bits(data[m + k]),
                    z = a +
                        float(data[m + 4 + k] - 1) * kernel::Bits(data[m + 3]);
        lo[k] = std::min(lo[k], a);
        hi[k] = std::max(hi[k], z);
      }
    }
    for (size_t k = 0; k < 3; ++k) {
      Put(data, node + k, lo[k]);
      Put(data, node + 3 + k, hi[k]);
    }
    if (last - first == 1) {
      data[node + 6] = order[first] | 0x80000000u;
      data[node + 7] = next_node;
      return index;
    }
    size_t axis = 0;
    for (size_t k = 1; k < 3; ++k)
      if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
    const size_t mid = first + (last - first) / 2;
    std::nth_element(order.begin() + first, order.begin() + mid,
                     order.begin() + last, [&](uint32_t a, uint32_t b) {
                       const float x = kernel::Bits(data[a + axis]) +
                                       float(data[a + 4 + axis] - 1) *
                                           kernel::Bits(data[a + 3]) * .5f;
                       const float y = kernel::Bits(data[b + axis]) +
                                       float(data[b + 4 + axis] - 1) *
                                           kernel::Bits(data[b + 3]) * .5f;
                       return x == y ? a < b : x < y;
                     });
    data[node + 6] = self(self, first, mid);
    self(self, mid, last);
    data[node + 7] = next_node;
    return index;
  };
  build_bounds(build_bounds, 0, order.size());
  return true;
}
void PreviewVolumeRenderer::bounds(double* lo, double* hi, double* cell) const {
  for (size_t k = 0; k < 3; ++k) {
    lo[k] = 1e300;
    hi[k] = -1e300;
  }
  *cell = 1e300;
  for (size_t b = 0; b < impl_->data[3]; ++b) {
    size_t m = 64 + b * 16;
    const double size = kernel::Bits(impl_->data[m + 3]);
    *cell = std::min(*cell, size);
    for (size_t k = 0; k < 3; ++k) {
      double start = kernel::Bits(impl_->data[m + k]);
      lo[k] = std::min(lo[k], start);
      hi[k] = std::max(hi[k], start + (impl_->data[m + 4 + k] - 1) * size);
    }
  }
}
bool PreviewVolumeRenderer::render(const PreviewVolumeCamera& camera, int width,
                                   int height, std::vector<uint8_t>* rgba,
                                   std::string* error, bool advance) {
  if (!impl_->compute || width < 1 || height < 1 ||
      uint64_t(width) * uint64_t(height) > impl_->budget / 64) {
    *error = "invalid or over-budget direct volume frame";
    return false;
  }
  const auto& previous = impl_->last_camera;
  const bool same_frame =
      impl_->have_image && impl_->last_width == width &&
      impl_->last_height == height && previous.eye == camera.eye &&
      previous.forward == camera.forward && previous.right == camera.right &&
      previous.up == camera.up && previous.background == camera.background &&
      previous.orthographic == camera.orthographic &&
      previous.near_plane == camera.near_plane &&
      previous.far_plane == camera.far_plane &&
      previous.exposure == camera.exposure &&
      previous.settings == camera.settings;
  if (same_frame && (!advance || impl_->accumulated_frames >=
                                     camera.settings.accumulation_frames)) {
    *rgba = impl_->last_image;
    return true;
  }
  if (!std::isfinite(camera.exposure) || !std::isfinite(camera.near_plane) ||
      !std::isfinite(camera.far_plane) || camera.near_plane < 0 ||
      camera.far_plane <= camera.near_plane) {
    *error = "invalid preview camera clipping range";
    return false;
  }
  const auto& settings = camera.settings;
  if (!std::isfinite(settings.density) || settings.density < 0 ||
      !std::isfinite(settings.scattering) || settings.scattering < 0 ||
      settings.scattering > 1 || !std::isfinite(settings.emission) ||
      settings.emission < 0 || !std::isfinite(settings.anisotropy) ||
      std::abs(settings.anisotropy) > .95f || !settings.max_steps ||
      settings.max_steps > 65536 || !settings.shadow_steps ||
      settings.shadow_steps > 65536 ||
      !std::isfinite(settings.transmittance_cutoff) ||
      settings.transmittance_cutoff < 0 || settings.transmittance_cutoff > 1 ||
      !std::isfinite(settings.step_size) || settings.step_size <= 0 ||
      settings.step_size > 2 || !std::isfinite(settings.integration_error) ||
      settings.integration_error < 0 || settings.integration_error > 1 ||
      !settings.scatter_bounces || settings.scatter_bounces > 256 ||
      !settings.scatter_samples || settings.scatter_samples > 64 ||
      !settings.light_samples || settings.light_samples > 64 ||
      !settings.accumulation_frames || settings.accumulation_frames > 4096) {
    *error = "invalid volume shading settings";
    return false;
  }
  auto& data = impl_->data;
  data[27] = settings.medium || impl_->density_only ? 1 : 0;
  Put(data, 28, settings.density);
  Put(data, 29, settings.scattering);
  Put(data, 30, settings.emission);
  Put(data, 31, settings.anisotropy);
  data[32] = settings.max_steps;
  data[33] = settings.scene_lights ? 1 : 0;
  data[34] = settings.shadows ? 1 : 0;
  data[35] = settings.scatter_bounces;
  data[36] = settings.scatter_samples;
  const uint32_t previous_frames = same_frame ? impl_->accumulated_frames : 0;
  data[37] = settings.scatter_seed + previous_frames * 0x9e3779b9u;
  data[38] = settings.light_samples;
  data[39] = settings.shadow_steps;
  Put(data, 42, settings.step_size);
  Put(data, 43, settings.integration_error);
  Put(data, 46, settings.transmittance_cutoff);
  data[1] = static_cast<uint32_t>(width);
  data[2] = static_cast<uint32_t>(height);
  data[7] = camera.orthographic ? 1 : 0;
  Put(data, 23, camera.near_plane);
  Put(data, 24, camera.far_plane);
  for (size_t k = 0; k < 3; ++k) {
    Put(data, 4 + k, camera.eye[k]);
    Put(data, 8 + k, camera.forward[k]);
    Put(data, 12 + k, camera.right[k]);
    Put(data, 16 + k, camera.up[k]);
    Put(data, 20 + k, camera.background[k]);
  }
  std::vector<float> pixels;
  if (!impl_->compute->dispatch(data, size_t(width) * size_t(height) * 4,
                                &pixels, error, impl_->uploaded))
    return false;
  impl_->uploaded = true;
  for (size_t i = 0; i < pixels.size(); ++i)
    if (!std::isfinite(pixels[i]) || (i % 4 == 3 && pixels[i] < 0)) {
      *error =
          "volume integration did not converge within the ray/shadow step "
          "budget or quadrature tolerance; increase --preview-volume-steps or "
          "--preview-shadow-steps, reduce step size, or relax "
          "--preview-volume-error";
      return false;
    }
  if (!previous_frames) impl_->linear_average.assign(pixels.size(), 0.f);
  impl_->accumulated_frames = previous_frames + 1;
  for (size_t i = 0; i < pixels.size(); ++i)
    impl_->linear_average[i] += (pixels[i] - impl_->linear_average[i]) /
                                float(impl_->accumulated_frames);
  rgba->resize(pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i) {
    float value = std::max(0.f, impl_->linear_average[i]);
    if (i % 4 != 3) {
      value *= std::exp2(std::max(-30.f, std::min(30.f, camera.exposure)));
      if ((camera.settings.scene_lights && data[25]) ||
          camera.settings.emission > 0 || camera.settings.scatter_bounces > 1)
        value = value / (1 + value);
    }
    value = std::min(1.f, value);
    if (i % 4 != 3)
      value = value <= .0031308f ? 12.92f * value
                                 : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
    (*rgba)[i] = static_cast<uint8_t>(std::round(value * 255));
  }
  impl_->last_camera = camera;
  impl_->last_width = width;
  impl_->last_height = height;
  impl_->last_image = *rgba;
  impl_->have_image = true;
  return true;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
