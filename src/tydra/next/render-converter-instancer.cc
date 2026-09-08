// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "next/schema/geom-point-instancer.hh"
#include "tydra/tangent-quantize.hh"
#include <algorithm>
#include <vector>
#include <unordered_set>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim;
namespace {
Matrix4 MatrixFromPointInstancerTransform(
    const ::lightusd::next::PointInstancerTransform& src) {
  Matrix4 dst;
  for (size_t i = 0; i < 16; ++i) {
    dst.m[i] = static_cast<float>(src.matrix[i]);
  }
  return dst;
}

std::vector<uint8_t> BuildInstanceVisibility(
    size_t instance_count,
    const std::vector<int64_t>& ids,
    const std::vector<int64_t>& invisible_ids,
    const std::vector<int64_t>& inactive_ids) {
  std::vector<uint8_t> visible(instance_count, uint8_t{1});
  if (instance_count == 0) return visible;

  std::unordered_set<int64_t> hidden;
  hidden.reserve(invisible_ids.size() + inactive_ids.size());
  hidden.insert(invisible_ids.begin(), invisible_ids.end());
  hidden.insert(inactive_ids.begin(), inactive_ids.end());
  if (hidden.empty()) return visible;

  if (ids.size() == instance_count) {
    for (size_t i = 0; i < ids.size(); ++i) {
      if (hidden.find(ids[i]) != hidden.end()) {
        visible[i] = 0;
      }
    }
    return visible;
  }

  for (int64_t id : hidden) {
    if (id >= 0 && static_cast<size_t>(id) < instance_count) {
      visible[static_cast<size_t>(id)] = 0;
    }
  }
  return visible;
}

uint32_t PackSnorm8Quaternion(const float* q) {
  const float identity[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  if (!q) q = identity;
  uint32_t packed = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const float component = std::max(-1.0f, std::min(1.0f, q[i]));
    const int32_t quantized = static_cast<int32_t>(std::lround(component * 127.0f));
    packed |= (static_cast<uint32_t>(static_cast<uint8_t>(quantized))
               << (i * 8));
  }
  return packed;
}

void BuildCompactInstances(
    const PointInstancerData& data, const std::vector<uint8_t>& visible,
    std::vector<RenderPointInstancer::CompactInstance>* out) {
  if (!out) return;
  out->clear();
  out->reserve(data.proto_indices.size());
  for (size_t i = 0; i < data.proto_indices.size(); ++i) {
    RenderPointInstancer::CompactInstance instance{};
    const size_t position_offset = i * 3;
    if (position_offset + 3 <= data.positions.size()) {
      for (size_t j = 0; j < 3; ++j) {
        instance.position[j] = data.positions[position_offset + j];
      }
    }
    const size_t orientation_offset = i * 4;
    const float* orientation =
        orientation_offset + 4 <= data.orientations.size()
            ? data.orientations.data() + orientation_offset
            : nullptr;
    instance.packed_orientation = PackSnorm8Quaternion(orientation);
    const size_t scale_offset = i * 3;
    for (size_t j = 0; j < 3; ++j) {
      const float scale = scale_offset + j < data.scales.size()
                              ? data.scales[scale_offset + j]
                              : 1.0f;
      instance.scale[j] = tangent_quantize::float_to_half(scale);
    }
    instance.flags =
        (i >= visible.size() || visible[i] != 0) ? uint16_t{1} : uint16_t{0};
    instance.prototype_index = data.proto_indices[i];
    instance.source_index = static_cast<uint32_t>(i);
    out->push_back(instance);
  }
}
}  // namespace

bool RenderSceneConverter::ConvertPointInstancer(const UsdPrim& prim,
                                                 RenderPointInstancer* out) {
  if (!out || !::lightusd::next::IsPointInstancer(prim)) {
    SetLastError("Invalid PointInstancer prim");
    return false;
  }

  PointInstancerData data;
  const bool build_draws = config_.point_instancer.build_instance_draws ||
                           config_.point_instancer.duplicate_meshes;
  const bool build_transforms =
      config_.point_instancer.build_instance_transforms || build_draws;
  if (!ReadPointInstancerData(prim, config_.time_code, &data,
                             build_transforms)) {
    SetLastError(data.validation_error.empty()
                      ? "Failed to read PointInstancer data"
                      : data.validation_error);
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  out->prototype_paths.reserve(data.prototypes.size());
  for (const ::lightusd::next::Path& path : data.prototypes) {
    out->prototype_paths.push_back(path.str());
  }
  std::vector<uint8_t> visibility = BuildInstanceVisibility(
      data.proto_indices.size(), data.ids, data.invisible_ids,
      data.inactive_ids);
  if (config_.point_instancer.compact_instances) {
    BuildCompactInstances(data, visibility, &out->compact_instances);
  }
  const bool retain_source_arrays =
      config_.point_instancer.retain_source_arrays || build_draws ||
      !config_.point_instancer.compact_instances;
  if (retain_source_arrays) {
    out->proto_indices = std::move(data.proto_indices);
    out->positions = std::move(data.positions);
    out->orientations = std::move(data.orientations);
    out->scales = std::move(data.scales);
    out->velocities = std::move(data.velocities);
    out->angular_velocities = std::move(data.angular_velocities);
    out->ids = std::move(data.ids);
    out->invisible_ids = std::move(data.invisible_ids);
    out->inactive_ids = std::move(data.inactive_ids);
    out->instance_visible = std::move(visibility);
  }
  out->transforms.reserve(data.transforms.size());
  for (const ::lightusd::next::PointInstancerTransform& transform :
       data.transforms) {
    out->transforms.push_back(MatrixFromPointInstancerTransform(transform));
  }
  out->valid = data.valid;
  out->validation_error = std::move(data.validation_error);
  return true;
}
}}}  // namespace lightusd::tydra::next
