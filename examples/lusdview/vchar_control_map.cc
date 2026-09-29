// SPDX-License-Identifier: Apache-2.0
#include "vchar_control_map.hh"

#include <algorithm>

#include "lightusd-cpp.hh"
#include "stage.hh"
#include "tydra/scene-access.hh"

namespace lusdview {
namespace {
struct ControlVisit { std::vector<VcharControl> controls; };

template <typename T>
bool CustomValue(const lightusd::Dictionary& data, const std::string& key,
                 T* value) {
  lightusd::MetaVariable variable;
  return lightusd::GetCustomDataByKey(data, key, &variable) &&
         variable.get_value<T>(value);
}

bool VisitControls(const lightusd::Path&, const lightusd::Prim& prim,
                   const int32_t, void* userdata, std::string*) {
  auto* visit = static_cast<ControlVisit*>(userdata);
  if (!visit || !visit->controls.empty() || !prim.metas().has_customData()) return true;
  const lightusd::Dictionary data = prim.metas().get_customData();
  std::vector<std::string> names, mappings;
  std::vector<lightusd::value::float2> ranges;
  std::vector<float> defaults;
  if (!CustomValue(data, "vchar:controlNames", &names)) return true;
  CustomValue(data, "vchar:controlMappings", &mappings);
  CustomValue(data, "vchar:controlRanges", &ranges);
  CustomValue(data, "vchar:controlDefaults", &defaults);
  for (size_t i = 0; i < names.size(); ++i) {
    VcharControl c;
    c.name = names[i];
    c.blendshape = i < mappings.size() ? mappings[i] : names[i];
    if (i < ranges.size()) {
      c.minimum = ranges[i][0]; c.maximum = ranges[i][1];
      if (c.minimum > c.maximum) std::swap(c.minimum, c.maximum);
    }
    if (i < defaults.size()) c.defaultValue = defaults[i];
    visit->controls.push_back(std::move(c));
  }
  return true;
}

}  // namespace

std::vector<VcharControl> ReadVcharControls(const lightusd::Stage& stage) {
  ControlVisit visit;
  std::string ignored;
  lightusd::tydra::VisitPrims(stage, VisitControls, &visit, &ignored);
  return visit.controls;
}

std::vector<VcharControl> ReadVcharControls(const lightusd_stage* stage) {
  using lightusd::api::StringList;
  std::vector<VcharControl> controls;
  std::vector<lightusd_prim> pending;
  for (size_t i = lightusd_stage_root_prim_count(stage); i > 0; --i)
    pending.push_back(lightusd_stage_root_prim(stage, i - 1));
  while (!pending.empty() && controls.empty()) {
    const lightusd_prim prim = pending.back();
    pending.pop_back();
    for (size_t i = lightusd_prim_child_count(prim); i > 0; --i)
      pending.push_back(lightusd_prim_child(prim, i - 1));
    lightusd_dict_ref data{}, group{};
    if (lightusd_prim_custom_data(prim, &data) != LIGHTUSD_OK ||
        lightusd_dict_find(data, "vchar", nullptr, nullptr, &group) != LIGHTUSD_OK ||
        !lightusd_dict_is_valid(group)) continue;
    StringList names, mappings;
    if (lightusd_dict_get_token_array(group, "controlNames", names.put()) != LIGHTUSD_OK) continue;
    lightusd_dict_get_token_array(group, "controlMappings", mappings.put());
    lightusd_value_view ranges{}, defaults{};
    lightusd_dict_find(group, "controlRanges", &ranges, nullptr, nullptr);
    lightusd_dict_find(group, "controlDefaults", &defaults, nullptr, nullptr);
    const bool valid_ranges = ranges.is_array && ranges.type == LIGHTUSD_TYPE_FLOAT2 &&
        ranges.data && ranges.nbytes / (2 * sizeof(float)) >= ranges.count;
    const bool valid_defaults = defaults.is_array && defaults.type == LIGHTUSD_TYPE_FLOAT &&
        defaults.data && defaults.nbytes / sizeof(float) >= defaults.count;
    for (size_t i = 0; i < lightusd_strlist_size(names.get()); ++i) {
      VcharControl c;
      const lightusd_sv name = lightusd_strlist_get(names.get(), i);
      c.name.assign(name.data, name.len);
      c.blendshape = c.name;
      if (i < lightusd_strlist_size(mappings.get())) {
        const lightusd_sv mapping = lightusd_strlist_get(mappings.get(), i);
        c.blendshape.assign(mapping.data, mapping.len);
      }
      if (valid_ranges && i < ranges.count) {
        const auto* values = static_cast<const float*>(ranges.data);
        c.minimum = values[i * 2]; c.maximum = values[i * 2 + 1];
        if (c.minimum > c.maximum) std::swap(c.minimum, c.maximum);
      }
      if (valid_defaults && i < defaults.count)
        c.defaultValue = static_cast<const float*>(defaults.data)[i];
      controls.push_back(std::move(c));
    }
  }
  return controls;
}
}  // namespace lusdview
