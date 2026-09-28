// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - High-level API Implementation

#include "lightusd-next.hh"
#include "composition/composition.hh"
#include "diff/layer-diff.hh"
#include "pcp/cache.hh"
#include "reader/usdz-reader.hh"
#include "resolver/asset-resolver.hh"
#include "../logger.hh"
#include "../safe-arithmetic.hh"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

namespace lightusd {
namespace next {

StageSessionOptions MakeHardenedStageSessionOptions(size_t max_memory) {
  StageSessionOptions options;
  // Zero must never turn a hardened request into an unlimited mode.
  max_memory = std::max<size_t>(max_memory, 1);
  options.load.input_policy = InputPolicy::Untrusted;
  options.load.limits.max_input_bytes = max_memory;
  options.load.limits.max_asset_bytes = max_memory;
  options.load.limits.max_resident_bytes = max_memory;
  options.load.usda_options.parse_options.strict_aousd_conformance = true;
  options.load.usda_options.parse_options.max_file_size = max_memory;
  options.load.usda_options.parse_options.max_usda_lazy_array_elements =
      std::min(options.load.limits.max_array_elements, max_memory);
  options.load.usdc_options.crate_options.strict_aousd_conformance = true;
  options.load.usdc_options.crate_options.max_memory = max_memory;
  options.load.usdc_options.crate_options.max_array_elements =
      options.load.limits.max_array_elements;
  options.load.usdc_options.crate_options.use_mmap = false;
  options.load.usdz_options.max_archive_size = max_memory;
  options.load.usdz_options.max_entry_size = max_memory;
  options.load.usdz_options.max_entries =
      options.load.limits.max_archive_entries;
  options.composition.strict_aousd_conformance = true;
  options.composition.error_when_asset_not_found = true;
  options.composition.max_layer_memory = max_memory;
  options.composition.usdc_use_mmap = false;
  options.resolver.allow_absolute_paths = false;
  options.resolver.allow_parent_paths = false;
  options.resolver.search_recursively = false;
  options.resolver.enable_suffix_fallback = false;
  options.execution.max_threads = 1;
  options.execution.callback_concurrency = CallbackConcurrency::Serialized;
  return options;
}

namespace {

class StageOperationScope;
thread_local const StageOperationScope* active_stage_operation = nullptr;

class StageOperationScope {
 public:
  explicit StageOperationScope(const void* session)
      : session_(session), previous_(active_stage_operation) {
    active_stage_operation = this;
  }
  ~StageOperationScope() { active_stage_operation = previous_; }

  bool Contains(const void* session) const {
    for (const StageOperationScope* scope = this; scope;
         scope = scope->previous_) {
      if (scope->session_ == session) return true;
    }
    return false;
  }

 private:
  const void* session_;
  const StageOperationScope* previous_;
};

bool StageOperationActive(const void* session) {
  return active_stage_operation && active_stage_operation->Contains(session);
}

// File format detection
enum class FileFormat {
  Unknown,
  USDA,
  USDC,
  USDZ
};

FileFormat DetectFormat(const uint8_t* data, size_t size) {
  if (size < 8) return FileFormat::Unknown;

  // USDC (Crate) magic: "PXR-USDC"
  if (std::memcmp(data, "PXR-USDC", 8) == 0) {
    return FileFormat::USDC;
  }

  // USDZ (ZIP archive) magic: "PK\x03\x04"
  if (data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) {
    return FileFormat::USDZ;
  }

  // USDA is text - check for ASCII/UTF-8
  // Look for #usda header or common patterns
  bool is_text = true;
  for (size_t i = 0; i < std::min(size, size_t(1024)); ++i) {
    uint8_t c = data[i];
    if (c < 0x09 || (c > 0x0D && c < 0x20 && c != 0x1B)) {
      is_text = false;
      break;
    }
  }

  if (is_text) {
    return FileFormat::USDA;
  }

  return FileFormat::Unknown;
}

FileFormat DetectFormatFromExtension(const std::string& filename) {
  size_t dot = filename.rfind('.');
  if (dot == std::string::npos) return FileFormat::Unknown;

  std::string ext = filename.substr(dot);
  // Convert to lowercase
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }

  if (ext == ".usda") return FileFormat::USDA;
  if (ext == ".usdc") return FileFormat::USDC;
  if (ext == ".usdz") return FileFormat::USDZ;
  if (ext == ".usd") return FileFormat::Unknown;  // Need content detection

  return FileFormat::Unknown;
}

FileFormat DetectFormatFromFileHeader(const std::string& filename, std::string* err) {
  std::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    if (err) *err = "Failed to open file for format detection: " + filename;
    return FileFormat::Unknown;
  }

  uint8_t header[4096] = {};
  ifs.read(reinterpret_cast<char*>(header), sizeof(header));
  const std::streamsize nread = ifs.gcount();
  if (nread <= 0) {
    if (err) *err = "Failed to read file header: " + filename;
    return FileFormat::Unknown;
  }
  return DetectFormat(header, static_cast<size_t>(nread));
}

size_t MinNonZero(size_t a, size_t b) {
  if (a == 0) return b;
  if (b == 0) return a;
  return std::min(a, b);
}

LoadOptions EffectiveUSDAOptions(const LoadUSDOptions& options) {
  LoadOptions out = options.usda_options;
  if (options.input_policy == InputPolicy::Untrusted) {
    out.parse_options.strict_aousd_conformance = true;
  }
  out.parse_options.max_file_size =
      MinNonZero(out.parse_options.max_file_size,
                 options.limits.max_input_bytes);
  out.parse_options.max_usda_lazy_array_elements = MinNonZero(
      out.parse_options.max_usda_lazy_array_elements,
      options.limits.max_array_elements);
  out.parse_options.max_depth = MinNonZero(
      out.parse_options.max_depth, options.limits.max_parse_depth);
  return out;
}

USDCLoadOptions EffectiveUSDCOptions(const LoadUSDOptions& options) {
  USDCLoadOptions out = options.usdc_options;
  if (options.input_policy == InputPolicy::Untrusted) {
    out.crate_options.strict_aousd_conformance = true;
    out.crate_options.use_mmap = false;
  }
  out.crate_options.max_memory =
      MinNonZero(out.crate_options.max_memory, options.limits.max_input_bytes);
  out.crate_options.max_array_elements = MinNonZero(
      out.crate_options.max_array_elements, options.limits.max_array_elements);
  return out;
}

USDZReadOptions EffectiveUSDZOptions(const LoadUSDOptions& options) {
  USDZReadOptions out = options.usdz_options;
  out.max_archive_size = MinNonZero(out.max_archive_size,
                                    options.limits.max_input_bytes);
  out.max_entry_size = MinNonZero(out.max_entry_size,
                                  options.limits.max_asset_bytes);
  out.max_entries = MinNonZero(out.max_entries,
                               options.limits.max_archive_entries);
  return out;
}

}  // namespace

bool LoadUSD(const std::string& filename, Stage* stage,
             std::string* warn, std::string* err) {
  return LoadUSD(filename, stage, LoadUSDOptions{}, warn, err);
}

bool LoadUSD(const std::string& filename, Stage* stage,
             const LoadUSDOptions& options, std::string* warn,
             std::string* err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }
  if (!options.limits.valid()) {
    if (err) *err = "invalid resource limits: zero is not unlimited";
    return false;
  }

  FileFormat format = DetectFormatFromExtension(filename);
  if (format == FileFormat::Unknown) {
    format = DetectFormatFromFileHeader(filename, err);
  }

  switch (format) {
    case FileFormat::USDA: {
      LoadResult result = LoadUSDAFromFile(filename, EffectiveUSDAOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) {
          *warn += w + "\n";
        }
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromFile(filename, EffectiveUSDCOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) {
          *warn += w + "\n";
        }
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDZ: {
      USDZReader usdz;
      if (!usdz.OpenFile(filename, EffectiveUSDZOptions(options))) {
        if (err) {
          *err = usdz.Error().empty() ? "Failed to open USDZ file"
                                      : usdz.Error();
        }
        return false;
      }
      int idx = usdz.FindRootLayer();
      if (idx < 0) {
        if (err) *err = "USDZ first entry is not a valid USD root layer";
        return false;
      }
      const uint8_t* entry_data = usdz.EntryData(idx);
      size_t entry_size = usdz.EntrySize(idx);
      if (!entry_data || entry_size == 0) {
        if (err) *err = "Empty USD entry in USDZ";
        return false;
      }

      const FileFormat inner_fmt = DetectFormat(entry_data, entry_size);
      // Delegate to USDC or USDA reader
      if (inner_fmt == FileFormat::USDC) {
        USDCLoadResult result =
            LoadUSDCFromMemory(entry_data, entry_size,
                               EffectiveUSDCOptions(options));
        if (!result.success) {
          if (err) *err = result.error_summary;
          return false;
        }
        if (warn && !result.warnings.empty()) {
          for (const auto& w : result.warnings) *warn += w + "\n";
        }
        *stage = std::move(result.stage);
      } else {
        LoadResult result = LoadUSDAFromString(
            reinterpret_cast<const char*>(entry_data), entry_size,
            EffectiveUSDAOptions(options));
        if (!result.success) {
          if (err) *err = result.error_summary;
          return false;
        }
        if (warn && !result.warnings.empty()) {
          for (const auto& w : result.warnings) *warn += w + "\n";
        }
        *stage = std::move(result.stage);
      }
      return true;
    }

    default:
      if (err) *err = "Unknown file format";
      return false;
  }
}

namespace {

// True if the root layer carries any composition arc the compositor must
// resolve: sublayers, or per-prim references/payloads/inherits/specializes/
// variants. A self-contained layer flattens to itself, so composing it is pure
// overhead (and must be skipped to preserve the lazy single-file fast path).
bool RootNeedsComposition(const Layer& root) {
  if (!root.meta().subLayers.empty()) return true;
  for (const PrimSpec& prim : root.prims()) {
    if (HasCompositionArcs(prim)) return true;
  }
  return false;
}

std::string DirOfPath(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
}

std::string ChildPath(const std::string& parent, const std::string& child) {
  if (parent.empty() || parent == "/") return "/" + child;
  return parent + "/" + child;
}

StageChangeFlag ClassifyPropertyChange(const Stage& stage,
                                       const std::string& prim_path,
                                       const std::string& property,
                                       const std::vector<std::string>& reasons) {
  StageChangeFlag flags = StageChangeFlag::None;
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  if (property == "visibility" || property == "purpose") {
    flags |= StageChangeFlag::Visibility;
  }
  if (contains(property, "xformOp") || property == "xformOpOrder" ||
      property == "resetXformStack" || property == "positions" ||
      property == "orientations" || property == "scales" ||
      property == "velocities" || property == "angularVelocities") {
    flags |= StageChangeFlag::Transform;
  }
  if (property == "points" || property == "faceVertexCounts" ||
      property == "faceVertexIndices" || property == "curveVertexCounts" ||
      property == "protoIndices" || property == "prototypes" ||
      property == "ids") {
    flags |= StageChangeFlag::Topology;
  }
  if (property == "invisibleIds") flags |= StageChangeFlag::Visibility;
  if (contains(property, "primvars:") || property == "normals" ||
      property == "widths" || property == "displayColor" ||
      property == "displayOpacity") {
    flags |= StageChangeFlag::Primvar;
  }
  if (contains(property, "material:") || contains(property, "inputs:") ||
      contains(property, "outputs:") || contains(property, "surface") ||
      contains(property, "displacement") || contains(property, "volume")) {
    flags |= StageChangeFlag::Material;
  }
  if (contains(property, "file") || contains(property, "texture") ||
      contains(property, "sourceColorSpace")) {
    flags |= StageChangeFlag::Texture;
  }
  if (property == "focalLength" || contains(property, "Aperture") ||
      property == "projection" || property == "clippingRange" ||
      property == "clippingPlanes" || property == "focusDistance" ||
      property == "fStop" || contains(property, "shutter:") ||
      property == "stereoRole") {
    flags |= StageChangeFlag::Camera;
  }
  for (const std::string& reason : reasons) {
    if (contains(reason, "timeSample")) flags |= StageChangeFlag::Animation;
    if (contains(reason, "meta:")) flags |= StageChangeFlag::Metadata;
  }

  const UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (prim) {
    const std::string& type = prim.GetTypeName();
    if (type == "Camera") flags |= StageChangeFlag::Camera;
    if (contains(type, "Light") || type == "DomeLight") {
      flags |= StageChangeFlag::Light;
    }
    if (type == "Material" || type == "Shader" || type == "NodeGraph") {
      flags |= StageChangeFlag::Material;
    }
  }
  if (flags == StageChangeFlag::None) flags = StageChangeFlag::Metadata;
  return flags;
}

StageChangeSet BuildStageChangeSet(const Stage* previous, const Stage& next,
                                   uint64_t base_revision,
                                   uint64_t new_revision) {
  StageChangeSet out;
  out.base_revision = base_revision;
  out.new_revision = new_revision;
  if (!previous || !previous->GetRootLayer() || !next.GetRootLayer()) {
    out.full_resync = true;
    PrimChange root;
    root.path = Path("/");
    root.flags = StageChangeFlag::Resync;
    out.prims.push_back(std::move(root));
    return out;
  }

  std::unordered_map<std::string, PrimSpecDiff> prim_diffs;
  std::unordered_map<std::string, PropDiff> prop_diffs;
  LayerMetaDiff meta_diff;
  Diff(*previous->GetRootLayer(), *next.GetRootLayer(), prim_diffs,
       prop_diffs, DiffOptions{}, &meta_diff);
  out.stage_metadata_changed = meta_diff.changed();

  std::map<std::string, PrimChange> changes;
  auto resync = [&](const std::string& path) {
    PrimChange& c = changes[path];
    c.path = Path(path);
    c.flags |= StageChangeFlag::Resync;
  };
  for (const auto& item : prim_diffs) {
    for (const std::string& name : item.second.addedPS) {
      resync(ChildPath(item.first, name));
    }
    for (const std::string& name : item.second.deletedPS) {
      resync(ChildPath(item.first, name));
    }
    for (const std::string& name : item.second.modifiedPS) {
      resync(ChildPath(item.first, name));
    }
  }
  for (const auto& item : prop_diffs) {
    PrimChange& c = changes[item.first];
    c.path = Path(item.first);
    auto add_prop = [&](const std::string& name,
                        const std::vector<std::string>& reasons) {
      c.properties.push_back(name);
      c.flags |= ClassifyPropertyChange(next, item.first, name, reasons);
    };
    for (const std::string& name : item.second.addedProps) add_prop(name, {});
    for (const std::string& name : item.second.deletedProps) add_prop(name, {});
    for (const PropDiff::ModifiedProp& detail :
         item.second.modifiedPropDetails) {
      add_prop(detail.name, detail.reasons);
    }
    std::sort(c.properties.begin(), c.properties.end());
    c.properties.erase(
        std::unique(c.properties.begin(), c.properties.end()),
        c.properties.end());
  }
  out.prims.reserve(changes.size());
  for (auto& item : changes) out.prims.push_back(std::move(item.second));
  return out;
}

}  // namespace

struct StageSession::Impl {
  mutable std::recursive_mutex operation_mu;
  mutable std::mutex publication_mu;
  StageSessionOptions options;
  std::string root_identifier;
  // For packaged files this includes the selected root entry, e.g.
  // `scene.usdz[root.usda]`.  Keep it separate from the public filename so a
  // released composition cache can be restored against the same package root.
  std::string composition_identifier;
  // Cache borrows this object. Transactional Open/Reload moves Impl state;
  // keep the resolver's address stable across AdoptState.
  std::unique_ptr<AssetResolver> resolver{new AssetResolver()};
  std::unique_ptr<pcp::Cache> cache;
  std::shared_ptr<Stage> stage{new Stage()};
  uint64_t revision = 0;
  StageChangeSet last_changes;
  std::vector<Diagnostic> diagnostics;
  std::string warning;
  std::string error;
  OperationStatus operation_status = OperationStatus::InvalidData;
  StageSessionMemoryStats memory_stats;
  pcp::LoadRules released_load_rules;
  pcp::CompositionOptions::VariantSelectionMap released_variant_selections;
  std::vector<Path> released_deferred_payloads;
  std::vector<pcp::Cache::CompositionIssue> released_composition_issues;
  std::vector<std::string> released_layer_dependencies;
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::thread retiring_cache_thread;
#endif
  bool composition_cache_released = false;
  bool open = false;

  ~Impl() { WaitForRetiringCache(); }

  void BeginOperation() {
    operation_status = OperationStatus::InvalidData;
    warning.clear();
    error.clear();
  }

  void AdoptState(Impl&& other) {
    WaitForRetiringCache();
    options = std::move(other.options);
    root_identifier = std::move(other.root_identifier);
    composition_identifier = std::move(other.composition_identifier);
    cache.reset();  // Destroy borrowers before replacing their resolver.
    resolver = std::move(other.resolver);
    cache = std::move(other.cache);
    stage = std::move(other.stage);
    revision = other.revision;
    last_changes = std::move(other.last_changes);
    diagnostics = std::move(other.diagnostics);
    warning = std::move(other.warning);
    error = std::move(other.error);
    operation_status = other.operation_status;
    memory_stats = other.memory_stats;
    released_load_rules = std::move(other.released_load_rules);
    released_variant_selections =
        std::move(other.released_variant_selections);
    released_deferred_payloads = std::move(other.released_deferred_payloads);
    released_composition_issues =
        std::move(other.released_composition_issues);
    released_layer_dependencies =
        std::move(other.released_layer_dependencies);
#if defined(LIGHTUSD_ENABLE_THREAD)
    retiring_cache_thread = std::move(other.retiring_cache_thread);
#endif
    composition_cache_released = other.composition_cache_released;
    open = other.open;
  }

  void WaitForRetiringCache() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (retiring_cache_thread.joinable()) retiring_cache_thread.join();
#endif
  }

  void UpdateMemoryStats(const size_t* known_stage_bytes = nullptr) {
    StageSessionMemoryStats next;
    if (cache) {
      const pcp::Cache::MemoryStats cache_stats = cache->GetMemoryStats();
      next.source_layer_bytes = cache_stats.source_layer_bytes;
      next.transient_cache_bytes = cache_stats.transient_cache_bytes;
      next.layer_count = cache_stats.layer_count;
      next.prim_index_count = cache_stats.prim_index_count;
      next.composed_prim_count = cache_stats.composed_prim_count;
    }
    next.composed_stage_bytes = known_stage_bytes
                                    ? *known_stage_bytes
                                    : (stage ? stage->GetMemoryUsage() : 0);
    next.estimated_total_bytes = safe::saturating_add(
        safe::saturating_add(next.source_layer_bytes,
                             next.transient_cache_bytes),
        next.composed_stage_bytes);
    next.peak_estimated_total_bytes = std::max(
        memory_stats.peak_estimated_total_bytes, next.estimated_total_bytes);
    memory_stats = next;
  }

  bool CheckMemoryBudget(DiagnosticDomain domain) {
    UpdateMemoryStats();
    if (memory_stats.estimated_total_bytes <=
        options.load.limits.max_resident_bytes) {
      return true;
    }
    error = "aggregate memory budget exceeded: estimated " +
            std::to_string(memory_stats.estimated_total_bytes) +
            " bytes, limit " +
            std::to_string(options.load.limits.max_resident_bytes) +
            " bytes";
    operation_status = OperationStatus::ResourceLimit;
    AddDiagnostic(DiagnosticSeverity::Error, domain, "memory_budget", error,
                  root_identifier);
    return false;
  }

  bool CheckMemoryBudgetFor(const Stage& candidate, DiagnosticDomain domain,
                            StageSessionMemoryStats* accepted = nullptr) {
    StageSessionMemoryStats projected = memory_stats;
    if (cache) {
      const pcp::Cache::MemoryStats cache_stats = cache->GetMemoryStats();
      projected.source_layer_bytes = cache_stats.source_layer_bytes;
      projected.transient_cache_bytes = cache_stats.transient_cache_bytes;
      projected.layer_count = cache_stats.layer_count;
      projected.prim_index_count = cache_stats.prim_index_count;
      projected.composed_prim_count = cache_stats.composed_prim_count;
    }
    projected.composed_stage_bytes = candidate.GetMemoryUsage();
    projected.estimated_total_bytes = safe::saturating_add(
        safe::saturating_add(projected.source_layer_bytes,
                             projected.transient_cache_bytes),
        projected.composed_stage_bytes);
    if (projected.estimated_total_bytes <=
        options.load.limits.max_resident_bytes) {
      projected.peak_estimated_total_bytes = std::max(
          memory_stats.peak_estimated_total_bytes,
          projected.estimated_total_bytes);
      if (accepted) *accepted = projected;
      return true;
    }
    error = "aggregate memory budget exceeded: estimated " +
            std::to_string(projected.estimated_total_bytes) +
            " bytes, limit " +
            std::to_string(options.load.limits.max_resident_bytes) +
            " bytes";
    operation_status = OperationStatus::ResourceLimit;
    AddDiagnostic(DiagnosticSeverity::Error, domain, "memory_budget", error,
                  root_identifier);
    return false;
  }

  void EnsureUniqueStage() {
    if (!stage) {
      stage.reset(new Stage());
    } else if (stage.use_count() != 1) {
      stage.reset(new Stage(stage->Clone()));
    }
  }

  StageEditResult EditResult(bool success) const {
    StageEditResult result;
    result.success = success;
    result.status = success ? OperationStatus::Ok : operation_status;
    {
      std::lock_guard<std::mutex> lock(publication_mu);
      result.snapshot.revision = revision;
      result.snapshot.stage = stage;
    }
    if (success) result.changes = last_changes;
    result.diagnostics = diagnostics;
    result.warning = warning;
    result.error = error;
    return result;
  }

  StageEditResult BusyResult() const {
    StageEditResult result;
    result.status = OperationStatus::Busy;
    result.error = "stage session mutation is already in progress";
    std::lock_guard<std::mutex> lock(publication_mu);
    result.snapshot.revision = revision;
    result.snapshot.stage = stage;
    return result;
  }

  bool Progress(ProgressPhase phase, float progress,
                const std::string& message) {
    if (!options.progress_callback) return true;
    ProgressEvent event;
    event.phase = phase;
    event.progress = progress;
    event.message = message;
    event.estimated_resident_bytes = memory_stats.estimated_total_bytes;
    if (options.progress_callback(event)) return true;
    error = "operation cancelled";
    operation_status = OperationStatus::Cancelled;
    AddDiagnostic(DiagnosticSeverity::Error, DiagnosticDomain::Load,
                  "cancelled", error, root_identifier);
    return false;
  }

  bool RestoreCompositionCache() {
    if (cache) return true;
    if (!composition_cache_released || root_identifier.empty()) return false;
    // Do not overlap a reparse with destruction of the previous layer set. A
    // normal viewer edit happens well after this has completed; an immediate
    // edit waits here rather than doubling the source-layer memory peak.
    WaitForRetiringCache();

    warning.clear();
    error.clear();
    Stage root;
    if (!LoadUSD(root_identifier, &root, options.load, &warning, &error)) {
      RecordMessages(DiagnosticDomain::Load);
      return false;
    }
    if (!options.compose || !StageNeedsComposition(root)) {
      error = "composition cache cannot be restored for an uncomposed stage";
      RecordMessages(DiagnosticDomain::Compose);
      return false;
    }

    pcp::CompositionOptions composition = options.composition;
    composition.max_layer_memory = MinNonZero(
        composition.max_layer_memory, options.load.limits.max_asset_bytes);
    composition.usda_parse_options = options.load.usda_options.parse_options;
    std::shared_ptr<Layer> root_layer(root.ReleaseRootLayer());
    const std::string &identifier = composition_identifier.empty()
                                        ? root_identifier
                                        : composition_identifier;
    auto restored = pcp::Cache::Open(*resolver, std::move(root_layer),
                                     identifier, composition);
    if (!restored) {
      error = restored.error();
      RecordMessages(DiagnosticDomain::Compose);
      return false;
    }
    std::unique_ptr<pcp::Cache> next_cache(
        new pcp::Cache(std::move(*restored)));
    next_cache->SetLoadRules(released_load_rules);
    next_cache->SetVariantSelections(released_variant_selections);
    cache = std::move(next_cache);
    composition_cache_released = false;
    UpdateMemoryStats();
    return true;
  }

  void AddDiagnostic(DiagnosticSeverity severity, DiagnosticDomain domain,
                     const std::string& code, const std::string& message,
                     const std::string& path = std::string()) {
    Diagnostic d;
    d.severity = severity;
    d.domain = domain;
    d.code = code;
    d.message = message;
    d.path = path;
    diagnostics.push_back(std::move(d));
  }

  void RecordMessages(DiagnosticDomain domain) {
    if (!warning.empty()) {
      AddDiagnostic(DiagnosticSeverity::Warning, domain, "warning", warning,
                    root_identifier);
    }
    if (!error.empty()) {
      AddDiagnostic(DiagnosticSeverity::Error, domain, "error", error,
                    root_identifier);
    }
  }

  bool Rebuild(ProgressPhase phase) {
    if (!cache) return open;
    warning.clear();
    error.clear();
    if (!Progress(phase, 0.0f, "composing stage")) return false;
    Stage next_stage;
    pcp::Cache::PreviewCallback preview_callback;
    if (phase == ProgressPhase::Compose && options.preview_callback) {
      preview_callback = [this](Stage&& preview_stage) {
        if (!Progress(ProgressPhase::PreviewCompose, 0.0f,
                      "publishing stage preview")) {
          return false;
        }
        StagePreview preview;
        preview.snapshot.revision = revision + 1;
        preview.snapshot.stage.reset(new Stage(std::move(preview_stage)));
        if (!options.preview_callback(preview)) {
          error = "stage preview callback cancelled";
          operation_status = OperationStatus::Cancelled;
          AddDiagnostic(DiagnosticSeverity::Error, DiagnosticDomain::Load,
                        "cancelled", error, root_identifier);
          return false;
        }
        return Progress(ProgressPhase::PreviewCompose, 1.0f,
                        "stage preview ready");
      };
    }
    if (!cache->BuildStage(&next_stage, &warning, &error, preview_callback)) {
      RecordMessages(DiagnosticDomain::Compose);
      return false;
    }
    StageSessionMemoryStats accepted_memory;
    if (!CheckMemoryBudgetFor(next_stage, DiagnosticDomain::Compose,
                              &accepted_memory)) {
      return false;
    }
    // A completion callback may still cancel. Run it before publication so a
    // failed operation never exposes a new revision or stage.
    if (!Progress(phase, 1.0f, "stage ready")) return false;
    {
      std::lock_guard<std::mutex> lock(publication_mu);
      const uint64_t next_revision = revision + 1;
      StageChangeSet changes = BuildStageChangeSet(
          revision ? stage.get() : nullptr, next_stage, revision,
          next_revision);
      stage.reset(new Stage(std::move(next_stage)));
      revision = next_revision;
      last_changes = std::move(changes);
    }
    RecordMessages(DiagnosticDomain::Compose);
    memory_stats = accepted_memory;
    if (options.cache_retention == CacheRetention::LayersOnly) {
      cache->TrimTransientCaches();
      const size_t composed_stage_bytes = memory_stats.composed_stage_bytes;
      UpdateMemoryStats(&composed_stage_bytes);
    }
    return true;
  }
};

StageSession::StageSession() : impl_(new Impl()) {}
StageSession::~StageSession() = default;
StageSession::StageSession(StageSession&&) noexcept = default;
StageSession& StageSession::operator=(StageSession&&) noexcept = default;

StageOperationResult StageSession::OpenFile(
    const std::string& filename, const StageSessionOptions& options) {
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> operation_lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  using Clock = std::chrono::steady_clock;
  const auto open_begin = Clock::now();
  StageSessionOptions normalized = options;
  auto finish = [&](std::unique_ptr<Impl> candidate,
                    bool success) -> StageOperationResult {
    if (success) {
      impl_->WaitForRetiringCache();
      {
        std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
        impl_->AdoptState(std::move(*candidate));
      }
      return impl_->EditResult(true);
    }
    // Preserve the last committed stage, cache, and revision. Diagnostics from
    // the failed transaction remain observable through both the result and the
    // value-returning compatibility getters.
    StageOperationResult result = candidate->EditResult(false);
    {
      std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
      result.snapshot.revision = impl_->revision;
      result.snapshot.stage = impl_->stage;
    }
    impl_->warning = candidate->warning;
    impl_->error = candidate->error;
    impl_->diagnostics = candidate->diagnostics;
    return result;
  };
  if (filename.empty() || !normalized.load.limits.valid() ||
      normalized.execution.max_threads < 0) {
    std::unique_ptr<Impl> failed(new Impl());
    failed->operation_status = OperationStatus::InvalidArgument;
    failed->error = filename.empty() ? "empty stage filename"
                                     : "invalid session options";
    failed->AddDiagnostic(DiagnosticSeverity::Error, DiagnosticDomain::Load,
                          "invalid_argument", failed->error, filename);
    return finish(std::move(failed), false);
  }
  normalized.load.usda_options.parse_options.max_depth = MinNonZero(
      normalized.load.usda_options.parse_options.max_depth,
      normalized.load.limits.max_parse_depth);
  normalized.composition.max_layer_memory = MinNonZero(
      normalized.composition.max_layer_memory,
      normalized.load.limits.max_asset_bytes);
  normalized.composition.max_array_elements = MinNonZero(
      normalized.composition.max_array_elements,
      normalized.load.limits.max_array_elements);
  normalized.composition.max_archive_entries = MinNonZero(
      normalized.composition.max_archive_entries,
      normalized.load.limits.max_archive_entries);
  normalized.composition.max_depth = static_cast<uint32_t>(
      std::min<size_t>(normalized.composition.max_depth,
                       normalized.load.limits.max_composition_depth));
  normalized.composition.max_namespace_depth = static_cast<uint32_t>(
      std::min<size_t>(normalized.composition.max_namespace_depth,
                       normalized.load.limits.max_namespace_depth));
  const int threads = normalized.execution.max_threads == 0
                          ? -1
                          : ClampExecutionThreads(
                                normalized.execution.max_threads);
  normalized.composition.num_threads = threads;
  normalized.load.usda_options.parse_options.num_threads = threads;
  normalized.composition.usda_parse_options.num_threads = threads;
  if (normalized.load.input_policy == InputPolicy::Untrusted) {
    normalized.load.usda_options.parse_options.strict_aousd_conformance = true;
    normalized.load.usdc_options.crate_options.strict_aousd_conformance = true;
    normalized.load.usdc_options.crate_options.use_mmap = false;
    normalized.composition.strict_aousd_conformance = true;
    normalized.composition.error_when_asset_not_found = true;
    normalized.composition.usdc_use_mmap = false;
    normalized.resolver.allow_absolute_paths = false;
    normalized.resolver.allow_parent_paths = false;
    normalized.resolver.search_recursively = false;
    normalized.resolver.enable_suffix_fallback = false;
  }
  if (normalized.composition.num_threads > 1) {
    normalized.composition.num_threads =
        ClampExecutionThreads(normalized.composition.num_threads);
  }
  if (normalized.load.usda_options.parse_options.num_threads > 1) {
    normalized.load.usda_options.parse_options.num_threads =
        ClampExecutionThreads(normalized.load.usda_options.parse_options.num_threads);
  }
  if (normalized.composition.usda_parse_options.num_threads > 1) {
    normalized.composition.usda_parse_options.num_threads =
        ClampExecutionThreads(normalized.composition.usda_parse_options.num_threads);
  }
  std::unique_ptr<Impl> next(new Impl());
  next->options = normalized;
  next->root_identifier = filename;
  next->resolver->SetConfig(normalized.resolver);
  if (next->resolver->GetWorkingDirectory().empty()) {
    next->resolver->SetWorkingDirectory(DirOfPath(filename));
  }
  if (!next->Progress(ProgressPhase::RootLoad, 0.0f, "loading root layer")) {
    return finish(std::move(next), false);
  }

  Stage root;
  if (!LoadUSD(filename, &root, normalized.load, &next->warning, &next->error)) {
    next->RecordMessages(DiagnosticDomain::Load);
    return finish(std::move(next), false);
  }
  const auto root_loaded = Clock::now();
  if (normalized.early_preview_callback) {
    StagePreview preview;
    preview.snapshot.revision = 0;
    preview.snapshot.stage =
        std::shared_ptr<const Stage>(new Stage(root.Clone()));
    preview.namespace_complete = false;
    preview.spatial_subset = false;
    preview.authoritative = false;
    if (!normalized.early_preview_callback(preview)) {
      next->error = "stage early preview callback cancelled";
      next->operation_status = OperationStatus::Cancelled;
      next->AddDiagnostic(DiagnosticSeverity::Error, DiagnosticDomain::Load,
                          "cancelled", next->error, filename);
      return finish(std::move(next), false);
    }
  }
  // Composition arcs in a package are relative to its root entry, not to the
  // directory containing the .usdz file. Keep the public root identifier as the
  // filename, but anchor the composition cache at package.usdz[root.usd].
  std::string composition_identifier = filename;
  if (DetectFormatFromExtension(filename) == FileFormat::USDZ) {
    USDZReader package;
    if (package.OpenFile(filename, EffectiveUSDZOptions(normalized.load))) {
      const int root_index = package.FindRootLayer();
      if (root_index >= 0) {
        composition_identifier += "[" +
            package.EntryName(static_cast<size_t>(root_index)) + "]";
      }
    }
  }
  next->composition_identifier = composition_identifier;
  if (!next->Progress(ProgressPhase::RootLoad, 1.0f, "root layer loaded")) {
    return finish(std::move(next), false);
  }

  if (!normalized.compose || !StageNeedsComposition(root)) {
    next->stage.reset(new Stage(std::move(root)));
    next->revision = 1;
    next->last_changes.base_revision = 0;
    next->last_changes.new_revision = 1;
    next->last_changes.full_resync = true;
    PrimChange root_change;
    root_change.path = Path("/");
    root_change.flags = StageChangeFlag::Resync;
    next->last_changes.prims.push_back(std::move(root_change));
    if (!next->CheckMemoryBudget(DiagnosticDomain::Load)) {
      return finish(std::move(next), false);
    }
    next->open = true;
    next->RecordMessages(DiagnosticDomain::Load);
    return finish(std::move(next), true);
  }

  pcp::CompositionOptions composition = normalized.composition;
  composition.max_layer_memory = MinNonZero(
      composition.max_layer_memory, normalized.load.limits.max_asset_bytes);
  composition.max_depth = static_cast<uint32_t>(std::min<size_t>(
      composition.max_depth, normalized.load.limits.max_composition_depth));
  composition.max_namespace_depth = static_cast<uint32_t>(std::min<size_t>(
      composition.max_namespace_depth,
      normalized.load.limits.max_namespace_depth));
  composition.usda_parse_options = normalized.load.usda_options.parse_options;
  std::shared_ptr<Layer> root_layer(root.ReleaseRootLayer());
  auto opened = pcp::Cache::Open(*next->resolver, std::move(root_layer),
                                 composition_identifier, composition);
  if (!opened) {
    next->error = opened.error();
    next->RecordMessages(DiagnosticDomain::Compose);
    return finish(std::move(next), false);
  }
  const auto cache_opened = Clock::now();
  next->cache.reset(new pcp::Cache(std::move(*opened)));
  if (!next->Rebuild(ProgressPhase::Compose)) {
    return finish(std::move(next), false);
  }
  const auto stage_built = Clock::now();
  if (normalized.composition.enable_timing) {
    auto milliseconds = [](Clock::duration duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    };
    LIGHTUSD_LOG_I("[next_session] root_load=" +
                std::to_string(milliseconds(root_loaded - open_begin)) +
                "ms cache_open=" +
                std::to_string(milliseconds(cache_opened - root_loaded)) +
                "ms rebuild=" +
                std::to_string(milliseconds(stage_built - cache_opened)) +
                "ms");
  }
  next->open = true;
  return finish(std::move(next), true);
}

StageSnapshot StageSession::GetSnapshot() const {
  StageSnapshot snapshot;
  if (!impl_) return snapshot;
  std::lock_guard<std::mutex> lock(impl_->publication_mu);
  snapshot.revision = impl_->revision;
  snapshot.stage = impl_->stage;
  return snapshot;
}
StageChangeSet StageSession::GetLastChangeSet() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> operation_lock(impl_->operation_mu);
  std::lock_guard<std::mutex> lock(impl_->publication_mu);
  return impl_->last_changes;
}
nonstd::expected<Stage, OperationStatus> StageSession::CloseAndTakeStage() {
  if (!impl_) return nonstd::make_unexpected(OperationStatus::InvalidArgument);
  if (StageOperationActive(impl_.get())) {
    return nonstd::make_unexpected(OperationStatus::Busy);
  }
  std::lock_guard<std::recursive_mutex> operation_lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
  if (!impl_->stage || impl_->stage.use_count() != 1) {
    return nonstd::make_unexpected(OperationStatus::Busy);
  }
  impl_->open = false;
  impl_->cache.reset();
  impl_->composition_cache_released = false;
  impl_->released_load_rules.Clear();
  impl_->released_variant_selections.clear();
  impl_->released_deferred_payloads.clear();
  impl_->released_composition_issues.clear();
  impl_->released_layer_dependencies.clear();
  Stage out;
  if (impl_->stage) {
    out = std::move(*impl_->stage);
    impl_->stage.reset(new Stage());
  }
  return nonstd::expected<Stage, OperationStatus>(std::move(out));
}
StageSessionOptions StageSession::GetOptions() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->options;
}
std::string StageSession::GetRootIdentifier() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->root_identifier;
}
bool StageSession::IsOpen() const {
  if (!impl_) return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->open;
}
bool StageSession::IsComposed() const {
  if (!impl_) return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->open &&
         (impl_->cache != nullptr || impl_->composition_cache_released);
}
StageEditResult StageSession::Rebuild() {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->open) return impl_->EditResult(false);
  if (impl_->composition_cache_released &&
      !impl_->RestoreCompositionCache()) {
    return impl_->EditResult(false);
  }
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  return impl_->EditResult(success);
}

StageEditResult StageSession::LoadPayload(const Path& prim_path,
                                          pcp::Cache::LoadPolicy policy) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->RestoreCompositionCache()) return impl_->EditResult(false);
  const pcp::LoadRules prior_rules = impl_->cache->GetLoadRules();
  if (!impl_->cache->LoadPayload(prim_path, policy, &impl_->warning,
                                 &impl_->error)) {
    impl_->cache->SetLoadRules(prior_rules);
    impl_->RecordMessages(DiagnosticDomain::Compose);
    return impl_->EditResult(false);
  }
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetLoadRules(prior_rules);
  return impl_->EditResult(success);
}

StageEditResult StageSession::UnloadPayload(const Path& prim_path) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->RestoreCompositionCache()) {
    return impl_->EditResult(false);
  }
  const pcp::LoadRules prior_rules = impl_->cache->GetLoadRules();
  if (!impl_->cache->UnloadPayload(prim_path)) {
    impl_->cache->SetLoadRules(prior_rules);
    return impl_->EditResult(false);
  }
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetLoadRules(prior_rules);
  return impl_->EditResult(success);
}

StageEditResult StageSession::LoadPayloads(
    const std::vector<Path>& prim_paths, pcp::Cache::LoadPolicy policy) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->RestoreCompositionCache()) return impl_->EditResult(false);
  const pcp::LoadRules prior_rules = impl_->cache->GetLoadRules();
  if (!impl_->cache->LoadPayloads(prim_paths, policy)) {
    impl_->cache->SetLoadRules(prior_rules);
    return impl_->EditResult(false);
  }
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetLoadRules(prior_rules);
  return impl_->EditResult(success);
}

StageEditResult StageSession::SetVariantSelection(
    const Path& prim_path, const std::string& variant_set,
    const std::string& selection) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (prim_path.empty() || variant_set.empty() || selection.empty() ||
      !impl_->RestoreCompositionCache()) {
    return impl_->EditResult(false);
  }
  auto selections = impl_->cache->GetVariantSelections();
  selections[prim_path.str()][variant_set] = selection;
  const auto prior = impl_->cache->GetVariantSelections();
  impl_->cache->SetVariantSelections(selections);
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetVariantSelections(prior);
  return impl_->EditResult(success);
}

StageEditResult StageSession::ClearVariantSelection(
    const Path& prim_path, const std::string& variant_set) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->RestoreCompositionCache()) return impl_->EditResult(false);
  auto selections = impl_->cache->GetVariantSelections();
  auto path_it = selections.find(prim_path.str());
  if (path_it == selections.end()) {
    impl_->last_changes = StageChangeSet{};
    impl_->last_changes.base_revision = impl_->revision;
    impl_->last_changes.new_revision = impl_->revision;
    return impl_->EditResult(true);
  }
  path_it->second.erase(variant_set);
  if (path_it->second.empty()) selections.erase(path_it);
  const auto prior = impl_->cache->GetVariantSelections();
  impl_->cache->SetVariantSelections(selections);
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetVariantSelections(prior);
  return impl_->EditResult(success);
}

StageEditResult StageSession::SetVariantSelections(
    const pcp::CompositionOptions::VariantSelectionMap& selections) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->RestoreCompositionCache()) return impl_->EditResult(false);
  const auto prior = impl_->cache->GetVariantSelections();
  impl_->cache->SetVariantSelections(selections);
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  if (!success) impl_->cache->SetVariantSelections(prior);
  return impl_->EditResult(success);
}

StageEditResult StageSession::ReloadLayer(
    const std::string& resolved_layer_id) {
  if (!impl_) return {};
  if (StageOperationActive(impl_.get())) return impl_->BusyResult();
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  impl_->BeginOperation();
  if (!impl_->open || resolved_layer_id.empty())
    return impl_->EditResult(false);

  if (resolved_layer_id == impl_->root_identifier) {
    StageSession replacement;
    StageOperationResult opened =
        replacement.OpenFile(impl_->root_identifier, impl_->options);
    if (!opened) {
      StageEditResult failed = std::move(opened);
      failed.snapshot = GetSnapshot();
      return failed;
    }
    impl_->WaitForRetiringCache();
    {
      std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
      const uint64_t next_revision = impl_->revision + 1;
      replacement.impl_->last_changes = BuildStageChangeSet(
          impl_->stage.get(), *replacement.impl_->stage, impl_->revision,
          next_revision);
      replacement.impl_->revision = next_revision;
      impl_->AdoptState(std::move(*replacement.impl_));
    }
    return impl_->EditResult(true);
  }

  if (!impl_->RestoreCompositionCache()) return impl_->EditResult(false);
  if (!impl_->cache->ReloadLayer(resolved_layer_id, &impl_->warning,
                                 &impl_->error)) {
    return impl_->EditResult(false);
  }
  const bool success = impl_->Rebuild(ProgressPhase::Recompose);
  return impl_->EditResult(success);
}

pcp::CompositionOptions::VariantSelectionMap
StageSession::GetVariantSelections() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->cache
             ? impl_->cache->GetVariantSelections()
             : (impl_ ? impl_->released_variant_selections
                      : pcp::CompositionOptions::VariantSelectionMap());
}
std::vector<Path> StageSession::GetDeferredPayloadPaths() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_ && impl_->cache
             ? impl_->cache->GetDeferredPayloadPaths()
             : (impl_ ? impl_->released_deferred_payloads
                      : std::vector<Path>());
}
std::vector<pcp::Cache::CompositionIssue>
StageSession::GetCompositionIssues() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_ && impl_->cache
             ? impl_->cache->GetCompositionIssues()
             : (impl_ ? impl_->released_composition_issues
                      : std::vector<pcp::Cache::CompositionIssue>());
}

size_t StageSession::GetCompositionIssueCount() const {
  if (!impl_) return 0;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->cache ? impl_->cache->GetCompositionIssueCount()
                      : impl_->released_composition_issues.size();
}

bool StageSession::GetCompositionIssue(
    size_t index, pcp::Cache::CompositionIssue* out) const {
  if (!impl_ || !out) return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  if (impl_->cache) return impl_->cache->GetCompositionIssue(index, out);
  if (index >= impl_->released_composition_issues.size()) return false;
  *out = impl_->released_composition_issues[index];
  return true;
}

std::vector<std::string> StageSession::GetLayerDependencies() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  if (!impl_) return {};
  return impl_->cache ? impl_->cache->GetLayerDependencies()
                      : impl_->released_layer_dependencies;
}
std::vector<Diagnostic> StageSession::GetDiagnostics() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->diagnostics;
}
StageSessionMemoryStats StageSession::GetMemoryStats() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  impl_->UpdateMemoryStats();
  return impl_->memory_stats;
}
OperationStatus StageSession::TrimCaches() {
  if (!impl_) return OperationStatus::InvalidArgument;
  if (StageOperationActive(impl_.get())) return OperationStatus::Busy;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  if (!impl_->cache) return OperationStatus::Ok;
  impl_->cache->TrimTransientCaches();
  impl_->UpdateMemoryStats();
  return OperationStatus::Ok;
}
OperationStatus StageSession::ReleaseCompositionCache() {
  if (!impl_) return OperationStatus::InvalidArgument;
  if (StageOperationActive(impl_.get())) return OperationStatus::Busy;
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  if (!impl_->cache) return OperationStatus::Ok;
  impl_->WaitForRetiringCache();
  impl_->released_load_rules = impl_->cache->GetLoadRules();
  impl_->released_variant_selections = impl_->cache->GetVariantSelections();
  impl_->released_deferred_payloads = impl_->cache->GetDeferredPayloadPaths();
  impl_->released_composition_issues = impl_->cache->GetCompositionIssues();
  impl_->released_layer_dependencies = impl_->cache->GetLayerDependencies();
  std::unique_ptr<pcp::Cache> retiring = std::move(impl_->cache);
  impl_->composition_cache_released = true;
  impl_->UpdateMemoryStats();
#if defined(LIGHTUSD_ENABLE_THREAD)
  impl_->retiring_cache_thread = std::thread(
      [retiring = std::move(retiring)]() mutable { retiring.reset(); });
#else
  retiring.reset();
#endif
  return OperationStatus::Ok;
}
OperationStatus StageSession::ReleaseStaticGeometryArraysByPath(
    const Path* prim_path, size_t min_array_elements,
    Stage::StaticGeometryReleaseStats* out) {
  if (!impl_ || !out) return OperationStatus::InvalidArgument;
  if (StageOperationActive(impl_.get())) return OperationStatus::Busy;
  std::lock_guard<std::recursive_mutex> operation_lock(impl_->operation_mu);
  StageOperationScope operation_scope(impl_.get());
  if (!impl_->open) return OperationStatus::InvalidData;
  if (!impl_->cache && !impl_->composition_cache_released)
    return OperationStatus::Unsupported;
  std::lock_guard<std::mutex> publish_lock(impl_->publication_mu);
  if (prim_path && (!prim_path->is_absolute() || prim_path->is_root() ||
                    prim_path->has_property() ||
                    !impl_->stage->GetPrimAtPath(*prim_path).IsValid()))
    return OperationStatus::InvalidArgument;
  impl_->EnsureUniqueStage();
  if (prim_path) {
    // Resolve against the current backing layer AFTER copy-on-write. A prim
    // borrowed from a retained snapshot belongs to the old layer after cloning.
    *out = impl_->stage->ReleaseStaticGeometryArraysForPrim(
        impl_->stage->GetPrimAtPath(*prim_path), min_array_elements);
  } else {
    *out = impl_->stage->ReleaseStaticGeometryArrays(min_array_elements);
    impl_->UpdateMemoryStats();
  }
  return OperationStatus::Ok;
}

Stage::StaticGeometryReleaseStats StageSession::ReleaseStaticGeometryArrays(
    size_t min_array_elements) {
  Stage::StaticGeometryReleaseStats stats;
  ReleaseStaticGeometryArraysByPath(nullptr, min_array_elements, &stats);
  return stats;
}
Stage::StaticGeometryReleaseStats StageSession::ReleaseStaticGeometryArraysForPrim(
    const UsdPrim& prim, size_t min_array_elements) {
  Stage::StaticGeometryReleaseStats stats;
  if (!prim.IsValid()) return stats;
  const Path path = prim.GetPath();
  ReleaseStaticGeometryArraysByPath(&path, min_array_elements, &stats);
  return stats;
}
std::string StageSession::GetWarning() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->warning;
}
std::string StageSession::GetError() const {
  if (!impl_) return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->operation_mu);
  return impl_->error;
}

bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     std::string* warn, std::string* err,
                     const pcp::CompositionOptions* comp_opts) {
  return LoadUSDComposed(filename, stage, LoadUSDOptions{}, warn, err,
                         comp_opts);
}

bool StageNeedsComposition(const Stage& stage) {
  const Layer* root = stage.GetRootLayer();
  return root && RootNeedsComposition(*root);
}

bool ComposeLoadedStage(Stage* stage, AssetResolver& resolver,
                        const std::string& anchor_label,
                        const LoadUSDOptions& load_options,
                        std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts,
                        pcp::CompositionReport* report) {
  if (!stage) {
    if (err) *err = "composition failed: null stage";
    return false;
  }
  if (!load_options.limits.valid()) {
    if (err) *err = "invalid resource limits: zero is not unlimited";
    return false;
  }
  Layer* root = stage->GetRootLayer();
  if (!root || !RootNeedsComposition(*root)) {
    // No external/internal arcs to resolve — keep the lazy stage as-is. This is
    // the byte-identical fast path for pre-flattened / self-contained scenes.
    return true;
  }
  const std::string& filename = anchor_label;

  // The root has composition arcs. Resolve them through the full PCP composition
  // engine (sublayers + references + payloads + inherits/specializes + variants
  // + relocates + instancing), anchored to the input file's directory. External
  // USDC layers load lazily.
  //
  // Keep NATIVE instancing (detect_instances=true, the default): each prototype
  // group's geometry is stored ONCE (the prototype member holds the subtree;
  // sibling instances carry instance_prototype meta and emit empty). Consumers
  // traverse instances transparently via UsdPrim::GetChildren(), which follows
  // instance_prototype to the prototype's children — so lusdrender expands every
  // instance into the flat triangle/BVH stream at its own world transform while
  // the composed STAGE keeps just one copy per prototype. (Inline expansion via
  // detect_instances=false instead duplicates every instance's geometry into the
  // stage and OOMs on large composed scenes.)
  pcp::CompositionOptions copts;
  copts.load_payloads = true;
  // Diagnostics and worker counts are explicit CompositionOptions. Keeping
  // these out of process environment makes callers and benchmarks reproducible.
  // Parallel composition is opt-in via LIGHTUSD_ENABLE_THREAD so wasm builds do
  // not require Emscripten pthreads / SharedArrayBuffer by default.
#if defined(LIGHTUSD_ENABLE_THREAD)
  copts.num_threads = std::min<int>(
      static_cast<int>(std::thread::hardware_concurrency()),
      kMaxExecutionThreads);
  if (copts.num_threads < 1) copts.num_threads = 1;
#else
  copts.num_threads = 1;
#endif
  copts.max_layer_memory = load_options.limits.max_asset_bytes;
  copts.max_array_elements = load_options.limits.max_array_elements;
  copts.max_archive_entries = load_options.limits.max_archive_entries;
  copts.max_depth = static_cast<uint32_t>(std::min<size_t>(
      copts.max_depth, load_options.limits.max_composition_depth));
  copts.max_namespace_depth = static_cast<uint32_t>(std::min<size_t>(
      copts.max_namespace_depth, load_options.limits.max_namespace_depth));
  copts.strict_aousd_conformance =
      load_options.input_policy == InputPolicy::Untrusted;
  copts.error_when_asset_not_found = copts.strict_aousd_conformance;
  copts.usdc_use_mmap = !copts.strict_aousd_conformance;
  copts.usda_parse_options = load_options.usda_options.parse_options;
  // Merge caller-supplied composition options (e.g. variant_overrides) into our
  // defaults. Caller-populated fields take precedence.
  if (comp_opts) {
    copts.load_payloads = comp_opts->load_payloads;
    copts.max_depth = comp_opts->max_depth;
    copts.max_namespace_depth = comp_opts->max_namespace_depth;
    copts.error_when_asset_not_found = comp_opts->error_when_asset_not_found;
    copts.detect_instances = comp_opts->detect_instances;
    copts.flatten_instances = comp_opts->flatten_instances;
    copts.instance_flatten_mode = comp_opts->instance_flatten_mode;
    copts.prototype_numbering = comp_opts->prototype_numbering;
    copts.apply_list_ops = comp_opts->apply_list_ops;
    copts.strict_aousd_conformance =
        copts.strict_aousd_conformance ||
        comp_opts->strict_aousd_conformance;
    // -1/0 keep the default policy above:
    //  -1 -> auto (hardware_concurrency on threaded builds)
    //  0  -> explicit serial (1)
    if (comp_opts->num_threads == 0) {
      copts.num_threads = 1;
    } else if (comp_opts->num_threads > 1) {
      copts.num_threads = ClampExecutionThreads(comp_opts->num_threads);
    }
    copts.enable_timing = comp_opts->enable_timing || copts.enable_timing;
    if (comp_opts->payload_policy) copts.payload_policy = comp_opts->payload_policy;
    if (comp_opts->payload_policy_with_prim) {
      copts.payload_policy_with_prim = comp_opts->payload_policy_with_prim;
    }
    if (!comp_opts->variant_overrides.empty())
      copts.variant_overrides = comp_opts->variant_overrides;
    if (!comp_opts->variant_overrides_by_path.empty()) {
      copts.variant_overrides_by_path = comp_opts->variant_overrides_by_path;
    }
    copts.usda_parse_options = comp_opts->usda_parse_options;
    copts.max_layer_memory =
        MinNonZero(copts.max_layer_memory, comp_opts->max_layer_memory);
    copts.usdc_lazy_arrays = comp_opts->usdc_lazy_arrays;
    copts.usdc_use_mmap = comp_opts->usdc_use_mmap;
  }
  // Resource limits and the untrusted policy are authoritative even when a
  // caller also supplies legacy CompositionOptions. Re-apply them after the
  // merge so compatibility knobs cannot accidentally weaken the boundary.
  copts.max_depth = static_cast<uint32_t>(std::min<size_t>(
      copts.max_depth, load_options.limits.max_composition_depth));
  copts.max_namespace_depth = static_cast<uint32_t>(std::min<size_t>(
      copts.max_namespace_depth, load_options.limits.max_namespace_depth));
  copts.max_layer_memory = MinNonZero(
      copts.max_layer_memory, load_options.limits.max_asset_bytes);
  copts.max_array_elements = MinNonZero(
      copts.max_array_elements, load_options.limits.max_array_elements);
  copts.max_archive_entries = MinNonZero(
      copts.max_archive_entries, load_options.limits.max_archive_entries);
  copts.usda_parse_options.max_file_size = MinNonZero(
      copts.usda_parse_options.max_file_size,
      load_options.limits.max_asset_bytes);
  copts.usda_parse_options.max_usda_lazy_array_elements = MinNonZero(
      copts.usda_parse_options.max_usda_lazy_array_elements,
      load_options.limits.max_array_elements);
  copts.usda_parse_options.max_depth = MinNonZero(
      copts.usda_parse_options.max_depth,
      load_options.limits.max_parse_depth);
  if (load_options.input_policy == InputPolicy::Untrusted) {
    copts.strict_aousd_conformance = true;
    copts.error_when_asset_not_found = true;
    copts.usdc_use_mmap = false;
    copts.usda_parse_options.strict_aousd_conformance = true;
  }

  Stage composed;
  std::string cwarn, cerr;
  std::shared_ptr<Layer> root_layer(stage->ReleaseRootLayer());
  if (!root_layer) {
    if (err) *err = "composition failed: root layer is null";
    return false;
  }
  if (!pcp::ComposeStageFromLayer(std::move(root_layer), resolver, &composed,
                                  filename, copts, &cwarn, &cerr, report)) {
    if (err) {
      *err = "composition failed for " + filename +
             (cerr.empty() ? "" : ": " + cerr);
    }
    return false;
  }
  if (warn) {
    if (!cwarn.empty()) *warn += cwarn + (cwarn.back() == '\n' ? "" : "\n");
    if (!cerr.empty()) *warn += cerr + (cerr.back() == '\n' ? "" : "\n");
  }
  *stage = std::move(composed);
  return true;
}

bool ComposeLoadedStage(Stage* stage, std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts,
                        const std::string& anchor_label) {
  // Memory-rooted stages have no anchor directory; external arcs resolve
  // through the resolver's custom callbacks only (or surface as warnings).
  AssetResolver resolver;
  resolver.SetWorkingDirectory("");
  return ComposeLoadedStage(stage, resolver, anchor_label, LoadUSDOptions{},
                            warn, err, comp_opts);
}

bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     const LoadUSDOptions& load_options,
                     std::string* warn, std::string* err,
                     const pcp::CompositionOptions* comp_opts) {
  // Lazy single-layer load first (keeps arrays as lazy ValueRefs).
  if (!LoadUSD(filename, stage, load_options, warn, err)) {
    return false;
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory(DirOfPath(filename));
  std::string anchor = filename;
  if (DetectFormatFromExtension(filename) == FileFormat::USDZ) {
    USDZReader package;
    if (package.OpenFile(filename, EffectiveUSDZOptions(load_options))) {
      const int root_index = package.FindRootLayer();
      if (root_index >= 0) {
        anchor += "[" + package.EntryName(static_cast<size_t>(root_index)) + "]";
      }
    }
  }
  return ComposeLoadedStage(stage, resolver, anchor, load_options, warn, err,
                            comp_opts);
}

bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       std::string* warn, std::string* err) {
  return LoadUSDFromMemory(data, size, stage, LoadUSDOptions{}, warn, err);
}

bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       const LoadUSDOptions& options,
                       std::string* warn, std::string* err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }
  if (!data || size == 0) {
    if (err) *err = "input buffer is empty";
    return false;
  }
  if (!options.limits.valid()) {
    if (err) *err = "invalid resource limits: zero is not unlimited";
    return false;
  }

  switch (DetectFormat(data, size)) {
    case FileFormat::USDA: {
      LoadResult result =
          LoadUSDAFromString(reinterpret_cast<const char*>(data), size,
                             EffectiveUSDAOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) *warn += w + "\n";
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromMemory(data, size, EffectiveUSDCOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) *warn += w + "\n";
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDZ: {
      USDZReader usdz;
      if (!usdz.Open(data, size, EffectiveUSDZOptions(options))) {
        if (err) {
          *err = usdz.Error().empty() ? "Failed to open USDZ archive"
                                      : usdz.Error();
        }
        return false;
      }
      int idx = usdz.FindRootLayer();
      if (idx < 0) {
        if (err) *err = "USDZ first entry is not a valid USD root layer";
        return false;
      }
      const uint8_t* entry_data = usdz.EntryData(idx);
      size_t entry_size = usdz.EntrySize(idx);
      if (!entry_data || entry_size == 0) {
        if (err) *err = "Empty USD entry in USDZ";
        return false;
      }
      if (DetectFormat(entry_data, entry_size) == FileFormat::USDC) {
        USDCLoadResult result = LoadUSDCFromMemory(entry_data, entry_size,
                                                   EffectiveUSDCOptions(options));
        if (!result.success) {
          if (err) *err = result.error_summary;
          return false;
        }
        if (warn && !result.warnings.empty()) {
          for (const auto& w : result.warnings) *warn += w + "\n";
        }
        *stage = std::move(result.stage);
      } else {
        LoadResult result = LoadUSDAFromString(
            reinterpret_cast<const char*>(entry_data), entry_size,
            EffectiveUSDAOptions(options));
        if (!result.success) {
          if (err) *err = result.error_summary;
          return false;
        }
        if (warn && !result.warnings.empty()) {
          for (const auto& w : result.warnings) *warn += w + "\n";
        }
        *stage = std::move(result.stage);
      }
      return true;
    }

    default:
      if (err) *err = "Unknown data format (not USDA/USDC/USDZ)";
      return false;
  }
}

bool LoadUSDFromMemoryOwned(std::string&& data, Stage* stage,
                            const LoadUSDOptions& options,
                            std::string* warn, std::string* err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }
  if (data.empty()) {
    if (err) *err = "input buffer is empty";
    return false;
  }
  if (!options.limits.valid()) {
    if (err) *err = "invalid resource limits: zero is not unlimited";
    return false;
  }

  const FileFormat format = DetectFormat(
      reinterpret_cast<const uint8_t*>(data.data()), data.size());
  switch (format) {
    case FileFormat::USDA: {
      LoadResult result =
          LoadUSDAFromStringOwned(std::move(data), EffectiveUSDAOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) *warn += w + "\n";
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromMemoryOwned(std::move(data), EffectiveUSDCOptions(options));
      if (!result.success) {
        if (err) *err = result.error_summary;
        return false;
      }
      if (warn && !result.warnings.empty()) {
        for (const auto& w : result.warnings) *warn += w + "\n";
      }
      *stage = std::move(result.stage);
      return true;
    }

    case FileFormat::USDZ:
      return LoadUSDFromMemory(reinterpret_cast<const uint8_t*>(data.data()),
                               data.size(), stage, options, warn, err);

    default:
      if (err) *err = "Unknown file format";
      return false;
  }
}

bool LoadUSDA(const std::string& filename, Stage* stage,
              std::string* warn, std::string* err) {
  return LoadUSDA(filename, stage, LoadOptions{}, warn, err);
}

bool LoadUSDA(const std::string& filename, Stage* stage,
              const LoadOptions& options, std::string* warn, std::string* err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }

  LoadResult result = LoadUSDAFromFile(filename, options);
  if (!result.success) {
    if (err) *err = result.error_summary;
    return false;
  }

  if (warn && !result.warnings.empty()) {
    for (const auto& w : result.warnings) {
      *warn += w + "\n";
    }
  }

  *stage = std::move(result.stage);
  return true;
}

bool LoadUSDC(const std::string& filename, Stage* stage,
              std::string* warn, std::string* err) {
  return LoadUSDC(filename, stage, USDCLoadOptions{}, warn, err);
}

bool LoadUSDC(const std::string& filename, Stage* stage,
              const USDCLoadOptions& options, std::string* warn,
              std::string* err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }

  USDCLoadResult result = LoadUSDCFromFile(filename, options);
  if (!result.success) {
    if (err) *err = result.error_summary;
    return false;
  }

  if (warn && !result.warnings.empty()) {
    for (const auto& w : result.warnings) {
      *warn += w + "\n";
    }
  }

  *stage = std::move(result.stage);
  return true;
}

bool WriteUSDA(const Stage& stage, const std::string& filename,
               std::string* err) {
  USDAWriteResult result = WriteUSDAToFile(filename, stage);
  if (!result.success) {
    if (err) *err = result.error;
    return false;
  }
  return true;
}

bool WriteUSDC(const Stage& stage, const std::string& filename,
               std::string* err) {
  USDCWriteResult result = WriteUSDCToFile(filename, stage);
  if (!result.success) {
    if (err) *err = result.error;
    return false;
  }
  return true;
}

}  // namespace next
}  // namespace lightusd
