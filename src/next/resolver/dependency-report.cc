// SPDX-License-Identifier: Apache-2.0
#include "dependency-report.hh"
#include "../composition/composition.hh"
#include "../composition/expression-variables.hh"
#include "../eval/value-clip.hh"
#include "../layer/asset-anchor.hh"
#include "../pcp/cache.hh"
#include "../reader/usdz-reader.hh"
#include "../../minijson.hh"
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace lightusd { namespace next {
namespace {
class Collector {
 public:
  Collector(AssetResolver& r, const DependencyOptions& o) : resolver(r), opt(o) {}
  AssetResolver& resolver;
  const DependencyOptions& opt;
  DependencyReport report;
  pcp::LayerRegistry registry;
  std::set<std::string> visited, active, issues;
  bool stopped = false;
  std::map<std::string, std::set<std::string>> package_entries;

  bool AllVariants() const {
    return opt.all_authored_variants ||
           opt.scope == DependencyScope::AllAuthoredVariants;
  }

  bool Exists(const ResolvedAsset& asset) {
    if (!asset.exists || !asset.is_package) return asset.exists;
    auto found = package_entries.find(asset.package_path);
    if (found == package_entries.end()) {
      auto& names = package_entries[asset.package_path];
      USDZReader package;
      USDZReadOptions read;
      read.max_archive_size = opt.max_resident_bytes;
      read.max_entries = opt.max_records;
      std::vector<uint8_t> bytes;
      std::string error;
      const bool opened = resolver.HasUserCallbacks()
          ? resolver.ReadAsset(asset.package_path, &bytes, &error) && package.Open(bytes.data(), bytes.size(), read)
          : package.OpenFile(asset.package_path, read);
      if (!opened) { Issue("cannot inspect package entries: " + asset.package_path); return false; }
      for (size_t i = 0; i < package.NumEntries(); ++i) names.insert(package.EntryName(i));
      found = package_entries.find(asset.package_path);
    }
    return found->second.count(asset.asset_in_package) != 0;
  }

  void Issue(const std::string& text) {
    report.complete = false;
    if (issues.size() < opt.max_records && issues.insert(text).second)
      report.diagnostics.push_back(text);
  }
  bool Room() {
    if (report.records.size() < opt.max_records && !stopped) return true;
    Issue("dependency record limit reached"); stopped = true; return false;
  }
  static bool IsLayer(const std::string& path) {
    std::string p = path;
    if (!p.empty() && p.back() == ']') p.pop_back();
    const auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz" || ext == ".mtlx";
  }
  void Add(const std::string& authored, const std::string& layer,
           const std::string& site, const std::string& kind, size_t depth,
           const Value& variables, bool expand_udim = true) {
    if (authored.empty() || !Room()) return;
    auto expr = EvaluateAssetPathExpression(authored, variables);
    if (expr.is_none) return;
    if (!expr.success) {
      report.records.push_back({authored, "", layer, site, kind, "error"});
      Issue(layer + ": " + site + ": " + expr.error); return;
    }
    const std::string path = expr.value;
    const size_t udim = path.find("<UDIM>");
    if (expand_udim && udim != std::string::npos) {
      bool any = false;
      for (unsigned tile = opt.first_udim; tile <= opt.last_udim && !stopped; ++tile) {
        std::string candidate = path;
        candidate.replace(udim, 6, std::to_string(tile));
        if (Exists(resolver.Resolve(candidate, layer))) {
          const size_t before = report.records.size();
          Add(candidate, layer, site, "texture", depth, variables, false);
          // Preserve the authored pattern and enumerate resolved tile identifiers.
          if (report.records.size() > before) report.records[before].authored_path = authored;
          any = true;
        }
      }
      if (any) return;
    }
    ResolvedAsset resolved = resolver.Resolve(path, layer);
    resolved.exists = Exists(resolved);
    DependencyRecord rec{authored, resolved.resolved_path, layer, site, kind,
                         resolved.exists ? "resolved" : "missing"};
    bool follow = resolved.exists && IsLayer(resolved.resolved_path);
    if (kind == "payload" && !opt.follow_payloads) { rec.status = "deferred"; follow = false; }
    if (follow && active.count(resolved.resolved_path)) { rec.status = "cycle"; follow = false; }
    if (!resolved.exists) Issue(layer + ": unresolved asset " + authored);
    if (rec.status == "cycle") Issue(layer + ": dependency cycle through " + resolved.resolved_path);
    report.records.push_back(std::move(rec));
    if (follow) Visit(resolved.resolved_path, depth + 1);
  }
  void ValueAssets(const Value& value, const std::string& layer, const std::string& site,
                   const std::string& kind, size_t depth, const Value& vars, size_t nesting = 0) {
    if (stopped) return;
    if (nesting > opt.max_depth) { Issue("metadata depth limit at " + site); return; }
    if (value.is_array() && value.type_id() == TypeId::AssetPath) {
      if (const auto* a = value.as_token_array())
        for (const auto& v : *a) Add(v, layer, site, kind, depth, vars);
    } else if (const auto* a = value.as_asset_path()) Add(*a, layer, site, kind, depth, vars);
    else if (const auto* dict = value.as_dictionary()) {
      for (const auto& item : dict->entries())
        ValueAssets(item.second, layer, site + ":" + item.first, kind, depth, vars, nesting + 1);
    }
  }
  void Arcs(const std::vector<std::string>& arcs, const std::string& layer,
            const std::string& site, const std::string& kind, size_t depth, const Value& vars) {
    for (const auto& encoded : arcs)
      Add(Compositor::ParseReference(encoded).asset_path, layer, site, kind, depth, vars);
  }
  void Variants(const std::vector<VariantSetData>& sets,
                const std::vector<std::pair<std::string, std::string>>& selections,
                const std::string& layer, const std::string& site, size_t depth,
                const Value& vars, size_t nesting) {
    if (nesting > opt.max_depth) { Issue("variant depth limit at " + site); return; }
    for (const auto& set : sets) {
      std::string selected = set.selected;
      for (const auto& s : selections) if (s.first == set.name) selected = s.second;
      for (const auto& v : set.variants) {
        if (!AllVariants() && v.name != selected) continue;
        const std::string where = site + "{" + set.name + "=" + v.name + "}";
        Arcs(v.references, layer, where, "reference", depth, vars);
        Arcs(v.payloads, layer, where, "payload", depth, vars);
        for (const auto& p : v.properties) ValueAssets(p.value, layer, where + "." + p.name, "asset", depth, vars);
        for (const auto& f : v.unknownFields) ValueAssets(f.value, layer, where + ":" + f.name, "asset", depth, vars);
        if (v.content) Scan(*v.content, layer, depth, vars, where, nesting + 1);
        Variants(v.variantSets, v.variantSelections, layer, where, depth, vars, nesting + 1);
      }
    }
  }
  void Scan(const Layer& input, const std::string& id, size_t depth, const Value& vars,
            const std::string& prefix = "", size_t nesting = 0) {
    if (nesting > opt.max_depth || stopped) { Issue("variant depth limit at " + prefix); return; }
    for (const auto& sub : input.meta().subLayers) Add(sub, id, "subLayers", "sublayer", depth, vars);
    Add(input.meta().colorConfiguration, id, "colorConfiguration", "asset", depth, vars);
    ValueAssets(input.meta().customLayerData, id, "customLayerData", "asset", depth, vars);
    for (const auto& f : input.meta().unknownFields) ValueAssets(f.value, id, f.name, "asset", depth, vars);
    for (const auto& p : input.prims()) {
      if (stopped) break;
      const std::string site = prefix + p.path().str();
      Arcs(p.meta().references, id, site, "reference", depth, vars);
      Arcs(p.meta().payloads, id, site, "payload", depth, vars);
      ValueAssets(p.meta().customData(), id, site + ":customData", "asset", depth, vars);
      ValueAssets(p.meta().assetInfo(), id, site + ":assetInfo", "asset", depth, vars);
      ValueAssets(p.meta().sdrMetadata(), id, site + ":sdrMetadata", "asset", depth, vars);
      ValueAssets(p.meta().clips(), id, site + ":clips", "clip", depth, vars);
      for (const auto& f : p.meta().unknownFields()) ValueAssets(f.value, id, site + ":" + f.name, "asset", depth, vars);
      if (p.meta().clips().is_dictionary()) {
        std::vector<ValueClipSet> clips;
        std::string error;
        if (ParseValueClipSets(UsdPrim(&p, &input, 0), &clips, &error)) {
          for (const auto& clip : clips) {
            for (const auto& asset : clip.asset_paths) Add(asset, id, site + ":clips:" + clip.name, "clip", depth, vars);
            Add(clip.manifest_asset_path, id, site + ":clips:" + clip.name + ":manifest", "clip", depth, vars);
          }
        } else Issue(id + ": " + site + ": " + error);
      }
      for (const auto& slot : p.properties().slots()) {
        const std::string prop = site + "." + std::string(GetPropNameTable().get(slot.name_id));
        const std::string kind = prop.find("inputs:file") != std::string::npos ? "texture" : "asset";
        if (const auto* value = p.property_value(slot.name_id)) ValueAssets(*value, id, prop, kind, depth, vars);
        if (const auto* meta = p.property_meta(slot.name_id)) {
          ValueAssets(meta->customData, id, prop + ":customData", "asset", depth, vars);
          ValueAssets(meta->assetInfo, id, prop + ":assetInfo", "asset", depth, vars);
          ValueAssets(meta->sdrMetadata, id, prop + ":sdrMetadata", "asset", depth, vars);
          for (const auto& f : meta->unknownFields) ValueAssets(f.value, id, prop + ":" + f.name, "asset", depth, vars);
        }
        if (const auto* samples = p.time_samples(slot.name_id))
          for (const auto& sample : *samples)
            if (const auto* value = p.time_sample_value(sample.second))
              ValueAssets(*value, id, prop + "@" + std::to_string(sample.first), kind, depth, vars);
      }
      Variants(p.meta().variantSets(), p.meta().variantSelections(), id, site, depth, vars, nesting);
    }
  }
  void Visit(const std::string& id, size_t depth) {
    if (visited.count(id) || stopped) return;
    if (depth > opt.max_depth || visited.size() >= opt.max_layers) { Issue("layer/depth limit at " + id); return; }
    if (registry.memory_usage() >= opt.max_resident_bytes) { Issue("dependency memory limit reached"); stopped = true; return; }
    visited.insert(id);
    std::string warn, err;
    auto limits = opt.load;
    limits.max_memory = std::min(limits.max_memory, opt.max_resident_bytes - registry.memory_usage());
    auto layer = registry.GetOrLoad(resolver, id, "", &warn, &err, limits);
    if (!layer) { Issue(id + ": " + err); return; }
    if (registry.memory_usage() > opt.max_resident_bytes) { Issue("dependency memory limit reached"); stopped = true; return; }
    if (!warn.empty()) Issue(id + ": " + warn);
    std::string anchor = id;
    // A package root's assets are relative to its first layer entry, not to
    // the filesystem directory that contains the archive.
    if (id.size() >= 5 && id.substr(id.size() - 5) == ".usdz") {
      USDZReader package;
      USDZReadOptions read;
      read.max_archive_size = limits.max_memory;
      std::vector<uint8_t> bytes;
      const bool opened = resolver.HasUserCallbacks()
          ? resolver.ReadAsset(id, &bytes, &err) && package.Open(bytes.data(), bytes.size(), read)
          : package.OpenFile(id, read);
      if (!opened || package.FindRootLayer() < 0) { Issue("cannot inspect package root: " + id); return; }
      anchor += "[" + package.EntryName(size_t(package.FindRootLayer())) + "]";
    }
    active.insert(id);
    Scan(*layer, anchor, depth, layer->meta().expressionVariables);
    active.erase(id);
  }

  Value PropertyVariables(pcp::Cache& cache, const Path& path,
                          const std::string& property) {
    bool truncated = false;
    std::string warning;
    std::string error;
    const auto opinions = cache.GetPropertyStack(
        path, property, opt.max_records, &truncated, &warning, &error);
    if (!warning.empty()) Issue(warning);
    if (!error.empty()) Issue(error);
    if (truncated) Issue("property opinion limit reached at " + path.str() +
                         "." + property);
    for (const auto& opinion : opinions) {
      if (!opinion.suppressed &&
          (opinion.has_default || opinion.has_samples)) {
        return opinion.expression_variables;
      }
    }
    return Value();
  }

  void ScanComposedValues(pcp::Cache& cache, const Stage& stage,
                          const std::string& root) {
    const Layer* layer = stage.GetRootLayer();
    if (!layer) { Issue("composed stage has no root layer"); return; }
    const Value& layer_vars = layer->meta().expressionVariables;
    Add(layer->meta().colorConfiguration, root, "colorConfiguration", "asset",
        0, layer_vars);
    ValueAssets(layer->meta().customLayerData, root, "customLayerData", "asset",
                0, layer_vars);
    stage.Traverse([&](const UsdPrim& prim) {
      const PrimSpec* spec = prim.GetPrimSpec();
      if (!spec || stopped) return !stopped;
      std::string anchor =
          spec->asset_anchor_id() != 0
              ? AssetAnchorPath(spec->asset_anchor_id())
              : root;
      // Asset anchors stored on composed prims are directories.  The resolver
      // otherwise treats its anchor argument as a layer filename and removes
      // its last path component, so make the directory intent explicit.
      if (spec->asset_anchor_id() != 0 && !anchor.empty() &&
          anchor.back() != '/' && anchor.back() != '\\') {
        anchor.push_back('/');
      }
      const std::string site = prim.GetPath().str();
      const Value empty_vars;
      ValueAssets(spec->meta().customData(), anchor, site + ":customData",
                  "asset", 0, empty_vars);
      ValueAssets(spec->meta().assetInfo(), anchor, site + ":assetInfo",
                  "asset", 0, empty_vars);
      ValueAssets(spec->meta().sdrMetadata(), anchor, site + ":sdrMetadata",
                  "asset", 0, empty_vars);
      ValueAssets(spec->meta().clips(), anchor, site + ":clips", "clip", 0,
                  empty_vars);
      for (const auto& field : spec->meta().unknownFields())
        ValueAssets(field.value, anchor, site + ":" + field.name, "asset", 0,
                    empty_vars);
      for (const auto& slot : spec->properties().slots()) {
        const std::string name(GetPropNameTable().get(slot.name_id));
        const std::string location = site + "." + name;
        const std::string kind =
            name.find("inputs:file") != std::string::npos ? "texture" : "asset";
        const Value property_vars = PropertyVariables(cache, prim.GetPath(), name);
        if (const Value* value = spec->property_value(slot.name_id))
          ValueAssets(*value, anchor, location, kind, 0, property_vars);
        if (const auto* samples = spec->time_samples(slot.name_id)) {
          for (const auto& sample : *samples) {
            if (const Value* value = spec->time_sample_value(sample.second))
              ValueAssets(*value, anchor,
                          location + "@" + std::to_string(sample.first), kind,
                          0, property_vars);
          }
        }
      }
      return !stopped;
    });
  }

  void Compose(const std::string& id) {
    std::string warn, err;
    std::string composition_id = id;
    if (id.size() >= 5 && id.substr(id.size() - 5) == ".usdz") {
      USDZReader package;
      USDZReadOptions read;
      read.max_archive_size = opt.load.max_memory;
      std::vector<uint8_t> bytes;
      const bool opened_package = resolver.HasUserCallbacks()
          ? resolver.ReadAsset(id, &bytes, &err) &&
                package.Open(bytes.data(), bytes.size(), read)
          : package.OpenFile(id, read);
      const int root_index = opened_package ? package.FindRootLayer() : -1;
      if (root_index < 0) {
        Issue("cannot inspect package root: " + id);
        return;
      }
      composition_id +=
          "[" + package.EntryName(size_t(root_index)) + "]";
    }
    auto root = registry.GetOrLoad(resolver, composition_id, "", &warn, &err,
                                   opt.load);
    if (!root) { Issue(composition_id + ": " + err); return; }
    pcp::CompositionOptions composition;
    composition.load_payloads = opt.follow_payloads;
    composition.max_depth = uint32_t(std::min<size_t>(
        opt.max_depth, std::numeric_limits<uint32_t>::max()));
    composition.strict_aousd_conformance = opt.load.strict_aousd_conformance;
    composition.usda_parse_options = opt.load.usda_parse_options;
    composition.usdc_limits = opt.load.usdc_limits;
    auto opened = pcp::Cache::Open(resolver, root, composition_id, composition);
    if (!opened) { Issue(opened.error()); return; }
    pcp::Cache cache = std::move(opened.value());
    Stage stage;
    if (!cache.BuildStage(&stage, &warn, &err)) {
      Issue(err.empty() ? "composition failed" : err);
      return;
    }
    if (!warn.empty()) Issue(warn);
    for (const std::string& dependency : cache.GetLayerDependencies()) {
      if (dependency == id || dependency == composition_id || !Room()) continue;
      report.records.push_back({dependency, dependency, composition_id, "compositionGraph",
                                "layer", "resolved"});
    }
    for (const auto& issue : cache.GetCompositionIssues()) Issue(issue.message);
    ScanComposedValues(cache, stage, composition_id);
  }
};
}  // namespace
DependencyReport CollectDependencies(const std::string& root, AssetResolver& resolver,
                                     const DependencyOptions& options) {
  Collector c(resolver, options);
  c.report.root = root;
  const DependencyScope scope = options.all_authored_variants
      ? DependencyScope::AllAuthoredVariants : options.scope;
  c.report.scope = scope == DependencyScope::ComposedStage
      ? "composedStage" : scope == DependencyScope::AllAuthoredVariants
      ? "allAuthoredVariants" : "authoredSelections";
  if (!options.max_layers || !options.max_records || !options.max_depth ||
      !options.max_resident_bytes || !options.load.max_memory ||
      options.first_udim < 1001 || options.last_udim > 1999 || options.first_udim > options.last_udim) {
    c.Issue("invalid dependency limits"); return c.report;
  }
  const auto resolved = resolver.Resolve(root);
  if (!resolved.exists) c.Issue("cannot resolve root: " + root);
  else if (scope == DependencyScope::ComposedStage)
    c.Compose(resolved.resolved_path);
  else
    c.Visit(resolved.resolved_path, 0);
  auto key = [](const DependencyRecord& r) {
    return std::tie(r.referring_layer, r.location, r.kind, r.authored_path, r.resolved_identifier, r.status);
  };
  auto& records = c.report.records;
  std::sort(records.begin(), records.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
  records.erase(std::unique(records.begin(), records.end(), [&](const auto& a, const auto& b) { return key(a) == key(b); }), records.end());
  std::sort(c.report.diagnostics.begin(), c.report.diagnostics.end());
  return std::move(c.report);
}
std::string DependencyReportToJSON(const DependencyReport& report) {
  using Json = minijson::Value;
  Json records = Json::array(), diagnostics = Json::array();
  for (const auto& r : report.records)
    records.push_back(Json{{"authoredPath", r.authored_path}, {"resolvedIdentifier", r.resolved_identifier},
        {"referringLayer", r.referring_layer}, {"location", r.location}, {"kind", r.kind}, {"status", r.status}});
  for (const auto& d : report.diagnostics) diagnostics.push_back(d);
  return Json{{"schemaVersion", 1}, {"root", report.root}, {"complete", report.complete}, {"scope", report.scope},
              {"dependencies", records}, {"diagnostics", diagnostics}}.dump(2);
}
} }  // namespace lightusd::next
