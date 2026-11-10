// SPDX-License-Identifier: Apache-2.0
#include "udim-convert.hh"
#include "io-util.hh"
#include "layer/asset-anchor.hh"
#include "reader/usdz-reader.hh"
#include "resolver/asset-resolver.hh"
#include "sha256.hh"
#include "udim-core-layer.hh"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace lightusd::usdz {
namespace {
bool Fail(std::string* error, const std::string& message) {
  if (error) *error = "UDIM bake: " + message;
  return false;
}
bool ReadBounded(const std::string& path, std::vector<uint8_t>* bytes,
                 size_t limit, std::string* error) {
  std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
  const auto size = stream ? stream.tellg() : std::streampos(-1);
  if (size < 0 || uint64_t(size) > limit ||
      uint64_t(size) > uint64_t((std::numeric_limits<std::streamsize>::max)()))
    return Fail(error, "missing asset or encoded bytes exceed memory limit: " + path);
  bytes->resize(static_cast<size_t>(size));
  stream.seekg(0);
  if (size && !stream.read(reinterpret_cast<char*>(bytes->data()), std::streamsize(size)))
    return Fail(error, "cannot read tile: " + path);
  return true;
}
uint32_t MatchTile(const std::string& name, const std::string& pattern) {
  std::string pre, post;
  if (!udim::SplitPattern(pattern, &pre, &post) ||
      name.size() != pre.size() + 4 + post.size() ||
      name.compare(0, pre.size(), pre) ||
      name.compare(name.size() - post.size(), post.size(), post)) return 0;
  uint32_t id = 0;
  for (size_t i = pre.size(); i < pre.size() + 4; ++i) {
    if (name[i] < '0' || name[i] > '9') return 0;
    id = id * 10 + uint32_t(name[i] - '0');
  }
  return id >= 1001 && id <= 9999 ? id : 0;
}
struct Package {
  std::vector<uint8_t> bytes;
  USDZReader reader;
};
class NativeAssets {
 public:
  explicit NativeAssets(size_t limit) : limit_(limit) {}
  size_t retained() const { return retained_; }
  Package* package(const std::string& path, std::string* error) {
    auto found = packages_.find(path);
    if (found != packages_.end()) return found->second.get();
    auto value = std::make_unique<Package>();
    if (!ReadBounded(path, &value->bytes, limit_ - retained_, error)) return nullptr;
    USDZReadOptions options;
    options.max_archive_size = limit_ - retained_;
    options.max_entry_size = limit_ - retained_;
    if (!value->reader.Open(value->bytes.data(), value->bytes.size(), options)) {
      Fail(error, value->reader.Error());
      return nullptr;
    }
    retained_ += value->bytes.size();
    auto* result = value.get();
    packages_.emplace(path, std::move(value));
    return result;
  }
  bool discover(const std::string& pattern, size_t maximum,
                std::vector<uint32_t>* ids, std::string* error) {
    std::set<uint32_t> tiles;
    std::string archive, entry;
    if (AssetResolver::ParsePackagePath(pattern, &archive, &entry)) {
      auto* value = package(archive, error);
      if (!value) return false;
      for (size_t i = 0; i < value->reader.NumEntries(); ++i) {
        const auto id = MatchTile(value->reader.EntryName(i), entry);
        if (id) tiles.insert(id);
        if (tiles.size() > maximum) return Fail(error, "tile count exceeds configured limit");
      }
    } else {
      const auto path = std::filesystem::u8path(pattern);
      std::error_code ec;
      std::filesystem::directory_iterator it(path.parent_path(), ec), end;
      if (ec) return Fail(error, "cannot enumerate tiles for " + pattern);
      for (; it != end; it.increment(ec)) {
        if (ec) return Fail(error, "tile enumeration failed for " + pattern);
        const auto id = MatchTile(it->path().filename().u8string(), path.filename().u8string());
        if (id) tiles.insert(id);
        if (tiles.size() > maximum) return Fail(error, "tile count exceeds configured limit");
      }
      if (ec) return Fail(error, "tile enumeration failed for " + pattern);
    }
    if (tiles.empty()) return Fail(error, "no tiles resolved for " + pattern);
    ids->assign(tiles.begin(), tiles.end());
    return true;
  }
  bool read(const std::string& path, std::vector<uint8_t>* bytes,
            std::string* error, size_t limit) {
    std::string archive, entry;
    if (!AssetResolver::ParsePackagePath(path, &archive, &entry))
      return ReadBounded(path, bytes, limit, error);
    auto* value = package(archive, error);
    if (!value) return false;
    for (size_t i = 0; i < value->reader.NumEntries(); ++i) {
      if (value->reader.EntryName(i) != entry) continue;
      const auto size = value->reader.EntrySize(i);
      if (size > limit) return Fail(error, "encoded package tile exceeds memory limit");
      const auto* data = value->reader.EntryData(i);
      bytes->assign(data, data + size);
      return true;
    }
    return Fail(error, "missing package tile: " + path);
  }
 private:
  size_t limit_, retained_{0};
  std::map<std::string, std::unique_ptr<Package>> packages_;
};
struct Job {
  udim::Site site;
  std::string pattern;
  std::vector<uint32_t> ids;
  udim::Layout inspected;
};
std::string LayoutKey(const std::string& pattern, bool srgb, const udim::Layout& layout) {
  std::string key = std::to_string(pattern.size()) + ":" + pattern + (srgb ? ":s:" : ":d:") +
      std::to_string(layout.tile_width) + ":" + std::to_string(layout.tile_height);
  for (const auto& cell : layout.cells) key += ":" + std::to_string(cell.id);
  return key;
}
}

bool BakeUDIMs(Layer& layer, const UsdzConvertOptions& options, const std::string& root_dir,
               std::map<std::string, std::vector<uint8_t>>* assets, std::string* error) {
  if (options.udim_bake == UDIMBakeMode::Off) return true;
  if (!assets) return Fail(error, "null atlas output");
  if (options.udim_bake != UDIMBakeMode::Grid && options.udim_bake != UDIMBakeMode::Dense)
    return Fail(error, "invalid baking mode");
  udim::Options config;
  config.mode = options.udim_bake == UDIMBakeMode::Dense ? udim::BakeMode::Dense : udim::BakeMode::Grid;
  config.max_tiles = options.udim_max_tiles;
  config.max_atlas_size = options.udim_max_atlas_size;
  config.memory_budget_bytes = options.udim_memory_budget_bytes;
  config.cross_tile = options.udim_cross_tile_split ? udim::CrossTilePolicy::Split : udim::CrossTilePolicy::Reject;
  config.dense_padding = options.udim_dense_padding;
  config.subdivision_level = options.udim_subdivision_level;
  config.max_tile_size = options.max_texture_size;
  if (!udim::ValidateOptions(config, error)) return false;
  std::vector<udim::Site> sites;
  std::vector<uint32_t> anchors;
  if (!udim::DescribeCoreLayer(layer, &sites, error, config.memory_budget_bytes, &anchors)) return false;
  if (sites.size() > 4096) return Fail(error, "too many texture sites");
  if (sites.empty()) return true;
  NativeAssets input(config.memory_budget_bytes);
  udim::Fetch fetch = [&](const std::string& path, std::vector<uint8_t>* bytes,
                          std::string* message, size_t limit) {
    return input.read(path, bytes, message, limit);
  };
  std::vector<Job> jobs;
  std::map<std::string, std::vector<size_t>> groups;
  for (const auto& site : sites) {
    Job job;
    job.site = site;
    const uint32_t anchor = anchors[jobs.size()];
    job.pattern = ResolveAssetAnchorPath(site.pattern,
        anchor ? anchor : InternAssetAnchor(root_dir));
    if (job.pattern.find('\0') != std::string::npos) return Fail(error, "invalid tile path");
    if (!input.discover(job.pattern, config.max_tiles, &job.ids, error)) return false;
    udim::Options budget = config;
    budget.memory_budget_bytes -= input.retained();
    if (!udim::InspectLayout(job.pattern, job.ids, fetch, budget, &job.inspected, error)) return false;
    groups[site.path].push_back(jobs.size());
    jobs.push_back(std::move(job));
  }
  std::map<std::string, udim::Layout> layouts;
  for (const auto& group : groups) {
    std::set<uint32_t> ids;
    int width = 0, height = 0;
    for (size_t index : group.second) {
      const Job& job = jobs[index];
      ids.insert(job.ids.begin(), job.ids.end());
      width = std::max(width, job.inspected.tile_width);
      height = std::max(height, job.inspected.tile_height);
    }
    if (ids.size() > config.max_tiles) return Fail(error, "animation tile count exceeds configured limit");
    if (!udim::MakeLayout(std::vector<uint32_t>(ids.begin(), ids.end()), width, height,
                          config, &layouts[group.first], error)) return false;
  }
  std::vector<udim::Plan> plans;
  std::map<std::string, std::pair<udim::Layout, std::string>> cache;
  size_t retained = input.retained();
  for (const auto& job : jobs) {
    const udim::Layout& layout = layouts[job.site.path];
    const std::string key = LayoutKey(job.pattern, job.site.srgb, layout);
    auto previous = cache.find(key);
    if (previous == cache.end()) {
      if (retained >= config.memory_budget_bytes) return Fail(error, "retained atlases exceed memory limit");
      udim::Options budget = config;
      budget.memory_budget_bytes -= retained;
      udim::Atlas atlas;
      const std::string format = options.texture_format == OutputTextureFormat::JPEG ? "jpeg"
          : options.texture_format == OutputTextureFormat::PNG ? "png" : "keep";
      if (!udim::BakeAtlas(job.pattern, job.ids, fetch, budget, job.site.srgb, format,
                            options.jpeg_quality, &atlas, error, &layout)) return false;
      const std::string base = "textures/udim_" +
          sha256(reinterpret_cast<const char*>(atlas.bytes.data()), atlas.bytes.size());
      std::string name = base + "." + atlas.extension;
      // Preserve existing input/output files even if they deliberately occupy
      // a generated name with different bytes.
      for (size_t suffix = 0;;) {
        std::vector<std::string> candidates{io::JoinPath(root_dir, name)};
        if (options.output_format != OutputFormat::USDZ) {
          std::error_code ec;
          const auto output = std::filesystem::absolute(std::filesystem::u8path(options.output), ec);
          if (ec) return Fail(error, "cannot resolve atlas output directory");
          candidates.push_back((output.parent_path() / std::filesystem::u8path(name)).u8string());
        }
        bool conflict = false;
        for (const auto& candidate : candidates) {
          if (!io::FileExists(candidate)) continue;
          std::vector<uint8_t> existing;
          std::string ignored;
          if (!ReadBounded(candidate, &existing, budget.memory_budget_bytes - atlas.bytes.size(), &ignored) ||
              existing != atlas.bytes) { conflict = true; break; }
        }
        if (!conflict) break;
        if (++suffix > 100000) return Fail(error, "atlas name collision limit");
        name = base + "_" + std::to_string(suffix) + "." + atlas.extension;
      }
      if (!assets->count(name)) {
        if (atlas.bytes.size() > config.memory_budget_bytes - retained)
          return Fail(error, "retained atlas bytes exceed memory limit");
        retained += atlas.bytes.size();
        assets->emplace(name, std::move(atlas.bytes));
      }
      previous = cache.emplace(key, std::make_pair(std::move(atlas.layout), name)).first;
    }
    plans.push_back({job.site, previous->second.first, previous->second.second});
  }
  if (retained >= config.memory_budget_bytes) return Fail(error, "no working memory remains for layer edits");
  config.memory_budget_bytes -= retained;
  return udim::ApplyCorePlans(layer, plans, config, error);
}
}
