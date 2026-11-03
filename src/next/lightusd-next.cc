// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - High-level API Implementation

#include "load-usd.hh"
#include "load-usd-internal.hh"
#include "writer/usda-writer.hh"
#include "writer/usdc-writer.hh"
#include "next-util.hh"
#include "format-detect.hh"
#include "composition/composition.hh"
#include "pcp/cache.hh"
#include "reader/usdz-reader.hh"
#include "resolver/asset-resolver.hh"
#include "layer/asset-anchor.hh"
#include <algorithm>
#include <memory>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

namespace lightusd {
namespace next {

namespace load_detail {
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

std::string DirOfPath(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
}

}  // namespace load_detail

using load_detail::DirOfPath;
using load_detail::EffectiveUSDZOptions;


namespace {

// Publish reader results only after a successful load.
bool CompleteLoad(bool success, Stage&& loaded_stage,
                  const std::vector<std::string>& warnings,
                  const std::string& error_summary, Stage* stage,
                  std::string* warn, std::string* err) {
  if (!success) {
    if (err) *err = error_summary;
    return false;
  }
  if (warn) {
    for (const std::string& warning : warnings) *warn += warning + "\n";
  }
  *stage = std::move(loaded_stage);
  return true;
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

  if (AssetResolver::IsPackagePath(filename)) {
    AssetResolver resolver;
    const auto resolved = resolver.Resolve(filename, "", false);
    USDZReader package;
    if (!resolved.exists || !resolved.is_package ||
        !package.OpenFile(resolved.package_path, EffectiveUSDZOptions(options))) {
      if (err) *err = "Failed to open package layer: " + filename + ": " + package.Error();
      return false;
    }
    for (size_t i = 0; i < package.NumEntries(); ++i) {
      if (package.EntryName(i) != resolved.asset_in_package) continue;
      if (!LoadUSDFromMemory(package.EntryData(i), package.EntrySize(i), stage,
                             options, warn, err)) return false;
      StampPackageAssetAnchor(stage->GetRootLayer(), resolved.resolved_path);
      return true;
    }
    if (err) *err = "Package layer entry not found: " + filename;
    return false;
  }

  FileFormat format = DetectFormatFromExtension(filename);
  if (format == FileFormat::Unknown) {
    format = DetectFormatFromFileHeader(filename, err);
  }

  switch (format) {
    case FileFormat::USDA: {
      LoadResult result = LoadUSDAFromFile(filename, EffectiveUSDAOptions(options));
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromFile(filename, EffectiveUSDCOptions(options));
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
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
        const bool loaded = CompleteLoad(result.success, std::move(result.stage),
                            result.warnings, result.error_summary, stage, warn, err);
        if (loaded) StampPackageAssetAnchor(stage->GetRootLayer(),
            filename + "[" + usdz.EntryName(static_cast<size_t>(idx)) + "]");
        return loaded;
      } else {
        LoadResult result = LoadUSDAFromString(
            reinterpret_cast<const char*>(entry_data), entry_size,
            EffectiveUSDAOptions(options));
        const bool loaded = CompleteLoad(result.success, std::move(result.stage),
                            result.warnings, result.error_summary, stage, warn, err);
        if (loaded) StampPackageAssetAnchor(stage->GetRootLayer(),
            filename + "[" + usdz.EntryName(static_cast<size_t>(idx)) + "]");
        return loaded;
      }
    }

    default:
      if (err) *err = "Unknown file format";
      return false;
  }
}

bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     std::string* warn, std::string* err,
                     const pcp::CompositionOptions* comp_opts) {
  return LoadUSDComposed(filename, stage, LoadUSDOptions{}, warn, err,
                         comp_opts);
}

bool StageNeedsComposition(const Stage& stage) {
  const Layer* root = stage.GetRootLayer();
  return root && LayerNeedsComposition(*root);
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
  if (!root || !LayerNeedsComposition(*root)) {
    // No composition work to resolve — keep the lazy stage as-is. This is
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
      kAutoCompositionThreads);
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
    // Negative keeps the automatic policy above. Zero and one both request
    // serial composition; larger values are explicit bounded worker counts.
    if (comp_opts->num_threads == 0) {
      copts.num_threads = 1;
    } else if (comp_opts->num_threads > 0) {
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
  // Crate structural limits come from the caller's USDC reader options, so a
  // composed load bounds every referenced .usdc exactly like a single-file load.
  copts.usdc_limits = load_options.usdc_options.crate_options;
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
  AssetResolver default_resolver;
  std::unique_ptr<AssetResolver> resolver_copy;
  AssetResolver* resolver = load_options.resolver;
  if (resolver) {
    // Composition may run on worker threads, which cannot see the caller's
    // thread-local context binding. Snapshot the effective context into a
    // private copy's config so every worker resolves identically.
    resolver_copy = std::make_unique<AssetResolver>(*resolver);
    ResolverConfig config = resolver_copy->GetConfig();
    config.context = resolver->GetCurrentContext();
    resolver_copy->SetConfig(config);
    resolver = resolver_copy.get();
  } else {
    default_resolver.SetWorkingDirectory(DirOfPath(filename));
    resolver = &default_resolver;
  }
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
  return ComposeLoadedStage(stage, *resolver, anchor, load_options, warn, err,
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
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromMemory(data, size, EffectiveUSDCOptions(options));
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
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
        return CompleteLoad(result.success, std::move(result.stage),
                            result.warnings, result.error_summary, stage, warn, err);
      } else {
        LoadResult result = LoadUSDAFromString(
            reinterpret_cast<const char*>(entry_data), entry_size,
            EffectiveUSDAOptions(options));
        return CompleteLoad(result.success, std::move(result.stage),
                            result.warnings, result.error_summary, stage, warn, err);
      }
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
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
    }

    case FileFormat::USDC: {
      USDCLoadResult result =
          LoadUSDCFromMemoryOwned(std::move(data), EffectiveUSDCOptions(options));
      return CompleteLoad(result.success, std::move(result.stage),
                          result.warnings, result.error_summary, stage, warn, err);
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
  return CompleteLoad(result.success, std::move(result.stage),
                      result.warnings, result.error_summary, stage, warn, err);
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
  return CompleteLoad(result.success, std::move(result.stage),
                      result.warnings, result.error_summary, stage, warn, err);
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
