// SPDX-License-Identifier: Apache-2.0
#include "usdz-udim-bake.hh"

#include <algorithm>
#include <set>

#include "core/attribute.hh"
#include "core/prim-spec.hh"
#include "layer.hh"
#include "sha256.hh"
#include "usdShade.hh"
#include "usdz-udim-layer.hh"

namespace lightusd {
namespace usdz {
udim::Options UDIMOptions(const UsdzConvertOptions& o) {
  udim::Options r;
  r.mode = o.udim_bake;
  r.max_tiles = o.udim_max_tiles;
  r.max_atlas_size = o.udim_max_atlas_size;
  r.memory_budget_bytes = o.udim_memory_budget_bytes;
  r.cross_tile = o.udim_cross_tile;
  r.dense_padding = o.udim_dense_padding;
  r.subdivision_level = o.udim_subdivision_level;
  r.max_tile_size = o.max_texture_size;
  return r;
}
bool BakeUDIMInLayer(const UsdzConvertOptions& options, Layer* layer,
                     const udim::Fetch& fetch,
                     const std::function<bool(const std::string&)>& exists,
                     std::map<std::string, std::vector<uint8_t>>* assets,
                     UsdzConvertStats* stats, std::string* error) {
  if (!layer || !assets || !exists ||
      !udim::ValidateOptions(UDIMOptions(options), error))
    return false;
  if (options.udim_bake == udim::BakeMode::Off) return true;
  LegacyUDIMLayer access(*layer);
  access.setMemoryBudget(options.udim_memory_budget_bytes);
  std::vector<udim::Site> sites;
  std::vector<udim::Plan> plans;
  if (!udim::DescribeLayer(access, &sites, error)) return false;
  if (sites.empty()) return true;
  size_t retained = 0;
  for (const auto& asset : *assets) {
    if (asset.second.size() >= options.udim_memory_budget_bytes - retained) {
      if (error) *error = "UDIM bake: retained atlases exhaust memory limit";
      return false;
    }
    retained += asset.second.size();
  }
  auto working_options = UDIMOptions(options);
  working_options.memory_budget_bytes -= retained;
  std::map<std::string, std::pair<udim::Layout, std::string>> cache;
  std::map<std::string, std::vector<uint32_t>> pattern_ids;
  for (const auto& site : sites)
    if (!pattern_ids.count(site.pattern)) {
      std::string pre, post;
      udim::SplitPattern(site.pattern, &pre, &post);
      auto& ids = pattern_ids[site.pattern];
      for (uint32_t id = 1001; id <= 9999; ++id)
        if (exists(pre + std::to_string(id) + post)) {
          ids.push_back(id);
          if (ids.size() > options.udim_max_tiles) {
            if (error)
              *error = "UDIM bake: tile count exceeds configured limit";
            return false;
          }
        }
    }
  std::map<std::string, std::vector<const udim::Site*>> groups;
  for (const auto& site : sites) groups[site.path].push_back(&site);
  std::map<std::string, udim::Layout> shared;
  for (const auto& group : groups)
    if (group.second.size() > 1) {
      std::set<uint32_t> all;
      std::set<std::string> patterns;
      int width = 0, height = 0;
      for (const auto* site : group.second) {
        patterns.insert(site->pattern);
        for (uint32_t id : pattern_ids[site->pattern]) all.insert(id);
      }
      if (all.size() > options.udim_max_tiles) {
        if (error)
          *error = "UDIM bake: animation tile count exceeds configured limit";
        return false;
      }
      for (const auto& pattern : patterns) {
        udim::Layout local;
        if (!udim::InspectLayout(pattern, pattern_ids[pattern], fetch,
                                 working_options, &local, error))
          return false;
        width = std::max(width, local.tile_width);
        height = std::max(height, local.tile_height);
      }
      const std::vector<uint32_t> ids(all.begin(), all.end());
      if (!udim::MakeLayout(ids, width, height, working_options,
                            &shared[group.first], error))
        return false;
    }
  for (const auto& site : sites) {
    const auto common = shared.find(site.path);
    const udim::Layout* common_layout =
        common == shared.end() ? nullptr : &common->second;
    std::string key = site.pattern + (site.srgb ? "|srgb" : "|raw");
    if (common_layout) {
      key += "|" + std::to_string(common_layout->tile_width) + "x" +
             std::to_string(common_layout->tile_height);
      for (const auto& cell : common_layout->cells)
        key += "," + std::to_string(cell.id);
    }
    auto found = cache.find(key);
    if (found == cache.end()) {
      const auto& ids = pattern_ids[site.pattern];
      udim::Atlas atlas;
      const std::string format =
          options.texture_format == OutputTextureFormat::JPEG  ? "jpeg"
          : options.texture_format == OutputTextureFormat::PNG ? "png"
                                                               : "keep";
      auto bake_options = UDIMOptions(options);
      if (retained >= bake_options.memory_budget_bytes) {
        if (error) *error = "UDIM bake: retained atlases exceed memory limit";
        return false;
      }
      bake_options.memory_budget_bytes -= retained;
      if (!udim::BakeAtlas(site.pattern, ids, fetch, bake_options, site.srgb,
                           format, options.jpeg_quality, &atlas, error,
                           common_layout))
        return false;
      const std::string base_name =
          "textures/udim_" +
          sha256(reinterpret_cast<const char*>(atlas.bytes.data()),
                 atlas.bytes.size()) +
          "." + atlas.extension;
      std::string name = base_name;
      for (size_t suffix = 0; exists(name);) {
        std::vector<uint8_t> original;
        if (!fetch(name, &original, error,
                   options.udim_memory_budget_bytes - retained -
                       atlas.bytes.size()))
          return false;
        if (original == atlas.bytes) break;
        const auto dot = base_name.rfind('.');
        name = base_name.substr(0, dot) + "_" + std::to_string(++suffix) +
               base_name.substr(dot);
        if (suffix > 100000) {
          if (error) *error = "UDIM bake: atlas name collision limit";
          return false;
        }
      }
      if (!assets->count(name)) retained += atlas.bytes.size();
      found = cache.emplace(key, std::make_pair(std::move(atlas.layout), name))
                  .first;
      (*assets)[name] = std::move(atlas.bytes);
      if (stats) {
        ++stats->num_udim_sets_baked;
        ++stats->num_udim_atlases;
        stats->num_udim_tiles_baked += ids.size();
      }
    }
    plans.push_back({site, found->second.first, found->second.second});
  }
  auto geometry_options = UDIMOptions(options);
  if (retained >= geometry_options.memory_budget_bytes) {
    if (error) *error = "UDIM bake: atlases exhaust memory limit";
    return false;
  }
  geometry_options.memory_budget_bytes -= retained;
  return udim::ApplyPlans(access, plans, geometry_options, error);
}
}  // namespace usdz
}  // namespace lightusd
