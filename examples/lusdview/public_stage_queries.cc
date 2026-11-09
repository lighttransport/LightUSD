// SPDX-License-Identifier: Apache-2.0
#include "public_stage_queries.hh"
#include "lightusd-cpp.hh"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
namespace lusdview {
namespace {
std::vector<float> BlendWeightSample(lightusd_prim prim, double time) {
  lightusd::api::Value value;
  if (std::isnan(time) ||
      lightusd_attr_interpolate(prim, "blendShapeWeights", time, 0, value.put()) !=
          LIGHTUSD_OK) {
    if (lightusd_attr_copy_default(prim, "blendShapeWeights", value.put()) !=
        LIGHTUSD_OK) return {};
  }
  lightusd_value_view view{};
  if (lightusd_value_get_view(value.get(), &view) != LIGHTUSD_OK ||
      !view.is_array || view.storage != LIGHTUSD_COMP_FLOAT32 || !view.data)
    return {};
  const auto* data = static_cast<const float*>(view.data);
  return {data, data + view.nbytes / sizeof(float)};
}
std::vector<float> BlendWeightsLerp(lightusd_prim prim, double time) {
  const size_t count = lightusd_attr_timesample_count(prim, "blendShapeWeights");
  if (count < 2 || std::isnan(time)) return BlendWeightSample(prim, time);
  std::vector<double> times(count);
  lightusd_attr_timesample_times(prim, "blendShapeWeights", times.data(), count);
  if (time <= times.front()) return BlendWeightSample(prim, times.front());
  if (time >= times.back()) return BlendWeightSample(prim, times.back());
  const size_t hi = static_cast<size_t>(
      std::lower_bound(times.begin(), times.end(), time) - times.begin());
  auto a = BlendWeightSample(prim, times[hi - 1]);
  const auto b = BlendWeightSample(prim, times[hi]);
  if (a.size() != b.size() || times[hi] <= times[hi - 1]) return a;
  const float factor = static_cast<float>(
      (time - times[hi - 1]) / (times[hi] - times[hi - 1]));
  for (size_t i = 0; i < a.size(); ++i) a[i] += factor * (b[i] - a[i]);
  return a;
}
lightusd_prim FirstAnimation(lightusd_prim root) {
  if (PublicString(lightusd_prim_type_name(root)) == "SkelAnimation") return root;
  const size_t count = lightusd_prim_child_count(root);
  for (size_t i = 0; i < count; ++i) {
    auto found = FirstAnimation(lightusd_prim_child(root, i));
    if (lightusd_prim_is_valid(found)) return found;
  }
  return {};
}
}

std::unordered_map<std::string, float> ReadPublicBlendWeights(
    const lightusd_stage* stage, lightusd_prim mesh, double time) {
  std::unordered_map<std::string, float> weights;
  if (!stage || !lightusd_prim_is_valid(mesh) || mesh._owner != stage) return weights;
  lightusd_prim animation{}, root{};
  for (auto prim = mesh; lightusd_prim_is_valid(prim); prim = lightusd_prim_parent(prim)) {
    const std::string target = PublicString(lightusd_rel_target(prim, "skel:animationSource", 0));
    if (!target.empty()) {
      const auto candidate = lightusd_stage_prim_at_path(stage, target.c_str());
      if (PublicString(lightusd_prim_type_name(candidate)) == "SkelAnimation") {
        animation = candidate;
        break;
      }
    }
    if (PublicString(lightusd_prim_type_name(prim)) == "SkelRoot") root = prim;
    if (PublicString(lightusd_prim_path(prim)) == "/") break;
  }
  if (!lightusd_prim_is_valid(animation) && lightusd_prim_is_valid(root))
    animation = FirstAnimation(root);
  if (!lightusd_prim_is_valid(animation)) return weights;
  lightusd::api::Value namesValue;
  lightusd::api::StringList names;
  auto readNames = [&]() {
    lightusd_value_view view{};
    return lightusd_value_get_view(namesValue.get(), &view) == LIGHTUSD_OK &&
           view.is_array &&
           lightusd_value_get_token_array(namesValue.get(), names.put()) == LIGHTUSD_OK &&
           lightusd_strlist_size(names.get()) != 0;
  };
  if (std::isnan(time) ||
      lightusd_attr_interpolate(animation, "blendShapes", time, 0, namesValue.put()) !=
          LIGHTUSD_OK || !readNames()) {
    if (lightusd_attr_copy_default(animation, "blendShapes", namesValue.put()) !=
            LIGHTUSD_OK || !readNames()) return weights;
  }
  const auto values = BlendWeightsLerp(animation, time);
  const size_t count = std::min(lightusd_strlist_size(names.get()), values.size());
  for (size_t i = 0; i < count; ++i)
    weights[PublicString(lightusd_strlist_get(names.get(), i))] = values[i];
  return weights;
}

bool ReadPublicPreviewExtent(lightusd_prim prim, float min[3], float max[3]) {
  if (!min || !max) return false;
  lightusd::api::Value value;
  auto status = lightusd_attr_copy_default(prim, "extent", value.put());
  if (status == LIGHTUSD_ERR_NOT_FOUND)
    status = lightusd_attr_copy_default(prim, "extentsHint", value.put());
  if (status != LIGHTUSD_OK) return false;
  lightusd_value_view view{};
  if (lightusd_value_get_view(value.get(), &view) != LIGHTUSD_OK ||
      !view.is_array || !view.data) return false;
  if (view.storage == LIGHTUSD_COMP_FLOAT32 && view.nbytes >= 6 * sizeof(float)) {
    const auto* data = static_cast<const float*>(view.data);
    for (int i = 0; i < 3; ++i) { min[i] = data[i]; max[i] = data[i + 3]; }
    return true;
  }
  if (view.storage == LIGHTUSD_COMP_FLOAT64 && view.nbytes >= 6 * sizeof(double)) {
    const auto* data = static_cast<const double*>(view.data);
    for (int i = 0; i < 3; ++i) {
      min[i] = static_cast<float>(data[i]);
      max[i] = static_cast<float>(data[i + 3]);
    }
    return true;
  }
  return false;
}

std::string PublicString(lightusd_sv value) {
  return value.len ? std::string(value.data, value.len) : std::string();
}
std::string PublicDefaultSummary(const lightusd_value_view& value, lightusd_sv text) {
  const char* name = lightusd_type_name(value.type);
  const std::string type = name ? name : "value";
  if (value.is_array) return type + "[" + std::to_string(value.count) + "]";
  if (value.data) {
    switch (value.type) {
      case LIGHTUSD_TYPE_BOOL: return *static_cast<const bool*>(value.data) ? "true" : "false";
      case LIGHTUSD_TYPE_INT: return std::to_string(*static_cast<const int32_t*>(value.data));
      case LIGHTUSD_TYPE_INT64: return std::to_string(*static_cast<const int64_t*>(value.data));
      case LIGHTUSD_TYPE_FLOAT: return std::to_string(*static_cast<const float*>(value.data));
      case LIGHTUSD_TYPE_DOUBLE: return std::to_string(*static_cast<const double*>(value.data));
      default: break;
    }
  }
  if (value.type == LIGHTUSD_TYPE_STRING || value.type == LIGHTUSD_TYPE_TOKEN)
    return PublicString(text);
  if (value.type == LIGHTUSD_TYPE_ASSET_PATH) return "@" + PublicString(text) + "@";
  return type;
}
bool ReadPublicStageInfo(const lightusd_stage* stage, PublicStageInfo* out) {
  if (!stage || !out) return false;
  auto number = [stage](const char* key, double* destination) {
    lightusd::api::Value value;
    if (lightusd_stage_get_metadata(stage, key, value.put()) != LIGHTUSD_OK) return false;
    lightusd_value_view view{};
    if (lightusd::api::ValueView(value, &view) != LIGHTUSD_OK ||
        view.type != LIGHTUSD_TYPE_DOUBLE || view.is_array || view.nbytes != sizeof(double)) return false;
    std::memcpy(destination, view.data, sizeof(double));
    return true;
  };
  auto string = [stage](const char* key, std::string* destination) {
    lightusd::api::Value value;
    if (lightusd_stage_get_metadata(stage, key, value.put()) != LIGHTUSD_OK) return false;
    lightusd_sv view{};
    if (lightusd::api::ValueString(value, &view) != LIGHTUSD_OK) return false;
    *destination = PublicString(view);
    return true;
  };
  PublicStageInfo info;
  if (!string("defaultPrim", &info.defaultPrim) || !string("upAxis", &info.upAxis) ||
      !number("metersPerUnit", &info.metersPerUnit) || !number("startTimeCode", &info.startTimeCode) ||
      !number("endTimeCode", &info.endTimeCode) || !number("timeCodesPerSecond", &info.timeCodesPerSecond)) return false;
  if (!number("framesPerSecond", &info.framesPerSecond) ||
      !string("comment", &info.comment) || !string("doc", &info.documentation)) return false;
  info.startTimeCodeAuthored = lightusd_stage_metadata_is_authored(stage, "startTimeCode");
  info.endTimeCodeAuthored = lightusd_stage_metadata_is_authored(stage, "endTimeCode");
  info.primCount = lightusd_stage_prim_count(stage);
  *out = std::move(info);
  return true;
}
void VisitPublicPrims(const lightusd_stage* stage,
                     const std::function<bool(lightusd_prim)>& visitor) {
  if (!stage || !visitor) return;
  // Store only one frame per ancestor, so wide hierarchies do not require a
  // second array containing every sibling handle.
  struct Frame { lightusd_prim prim; size_t next = 0; size_t count = 0; };
  std::vector<Frame> stack;
  const size_t roots = lightusd_stage_root_prim_count(stage);
  for (size_t root = 0; root < roots; ++root) {
    auto prim = lightusd_stage_root_prim(stage, root);
    if (!visitor(prim)) return;
    stack.push_back({prim, 0, lightusd_prim_child_count(prim)});
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next == frame.count) { stack.pop_back(); continue; }
      auto child = lightusd_prim_child(frame.prim, frame.next++);
      if (!visitor(child)) return;
      stack.push_back({child, 0, lightusd_prim_child_count(child)});
    }
  }
}
}
