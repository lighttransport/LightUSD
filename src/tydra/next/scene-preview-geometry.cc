// SPDX-License-Identifier: Apache-2.0
// Bounded GPU geometry interpolation shared by ordinary and regional sampling.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "scene-preview-internal.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
void Put(std::vector<uint32_t>& out, size_t i, float value) {
  std::memcpy(&out[i], &value, 4);
}
}  // namespace
bool Build::geometry_backend() {
  if (opts.compute_backend == PreviewComputeBackend::Cpu) return false;
  if (!compute) {
    compute = CreatePreviewCompute(opts.compute_backend, opts.gpu_budget_bytes,
                                   &result.error);
    if (!compute) {
      if (result.error.empty())
        result.error = "geometry compute initialization failed";
      return false;
    }
    std::fprintf(stderr, "[scene-preview] geometry conversion compute=%s\n",
                 compute->name());
  }
  return std::string(compute->name()) != "cpu";
}
bool Build::queue_geometry(
    const Sample* vertices, const float* uv, size_t count, float radius,
    const std::string& material, size_t face,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors,
    const double* origin) {
  if (!geometry) geometry = std::make_unique<GeometryBatch>(*this);
  return geometry->add(vertices, uv, count, radius, material, face, ancestors,
                       origin);
}
bool Build::flush_geometry() { return !geometry || geometry->flush(); }
bool GeometryBatch::add(
    const Sample* vertices, const float* uv, size_t count, float radius,
    const std::string& material, size_t face,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors,
    const double* origin) {
  if (!count || count > 65536) {
    build.result.error = "invalid geometry sampling count";
    return false;
  }
  if (samples + count > 65536 && !flush()) return false;
  const size_t m = input.size();
  input.resize(m + 64);
  for (size_t c = 0; c < 3; ++c) {
    const auto& v = vertices[c];
    for (size_t k = 0; k < 3; ++k) {
      Put(input, m + c * 16 + k, float(v.p[k]));
      Put(input, m + c * 16 + 4 + k, v.n[k]);
      Put(input, m + c * 16 + 8 + k, v.color[k]);
    }
    Put(input, m + c * 16 + 3, v.opacity);
    Put(input, m + c * 16 + 12, uv[c * 2]);
    Put(input, m + c * 16 + 13, uv[c * 2 + 1]);
  }
  // Defensive: a non-finite position, normal, color or texcoord from any caller
  // would invalidate the whole dispatch buffer, so sanitize the record here and
  // report it once.
  for (size_t k = 0; k < 48; ++k) {
    float value;
    std::memcpy(&value, &input[m + k], 4);
    if (!std::isfinite(value)) {
      Put(input, m + k, 0.0f);
      if (build.result.warning.size() < 8192 &&
          build.result.warning.find("non-finite geometry sample") ==
              std::string::npos)
        build.result.warning +=
            "Preview sanitized a non-finite geometry sample\n";
    }
  }
  auto w = weight_offsets.find(count);
  if (w == weight_offsets.end()) {
    const auto offset = static_cast<uint32_t>(weights.size());
    w = weight_offsets.emplace(count, offset).first;
    const size_t start = weights.size();
    weights.resize(start + count * 3);
    for (size_t j = 0; j < count; ++j) {
      const double u = std::sqrt((j + .5) / count),
                   v = std::fmod((j + .5) * .6180339887498949, 1.);
      Put(weights, start + j * 3, float(1 - u));
      Put(weights, start + j * 3 + 1, float(u * (1 - v)));
      Put(weights, start + j * 3 + 2, float(u * v));
    }
  }
  input[m + 48] = static_cast<uint32_t>(samples);
  input[m + 49] = static_cast<uint32_t>(count);
  Put(input, m + 50, radius);
  input[m + 51] = w->second;
  Record record;
  record.material = material;
  record.face = face;
  record.count = count;
  record.ancestors = ancestors;
  std::copy(origin, origin + 3, record.origin.begin());
  records.push_back(std::move(record));
  samples += count;
  return true;
}
bool GeometryBatch::flush() {
  if (!samples) return true;
  input[0] = 2;
  input[1] = static_cast<uint32_t>(samples);
  input[2] = static_cast<uint32_t>(records.size());
  const auto offset = static_cast<uint32_t>(input.size());
  for (size_t i = 0; i < records.size(); ++i) input[16 + i * 64 + 51] += offset;
  input.insert(input.end(), weights.begin(), weights.end());
  std::vector<float> output;
  if (!build.compute->dispatch(input, samples * 16, &output,
                               &build.result.error))
    return false;
  RenderMesh shade_mesh;
  size_t index = 0;
  for (const auto& record : records)
    for (size_t j = 0; j < record.count; ++j, ++index) {
      const float* p = output.data() + index * 16;
      Sample sample;
      for (size_t k = 0; k < 3; ++k) {
        sample.p[k] = record.origin[k] + p[k];
        sample.n[k] = p[4 + k];
        sample.color[k] = p[8 + k];
      }
      sample.radius = p[3];
      sample.opacity = p[7];
      const auto shade =
          build.shade(record.material, shade_mesh, record.face, p + 12);
      for (size_t k = 0; k < 3; ++k) sample.color[k] *= shade[k];
      sample.opacity *= shade[3];
      if (build.opts.region_enabled) {
        if (!build.insert(sample)) return false;
      } else if (!build.emit(sample, record.ancestors))
        return false;
    }
  input.assign(16, 0);
  weights.clear();
  weight_offsets.clear();
  records.clear();
  samples = 0;
  return build.check("geometry-kernel", index);
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
