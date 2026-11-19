// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - PCP LayerRegistry implementation

#include "core/native-io.hh"
#include "layer-registry.hh"
#if defined(LIGHTUSD_WITH_USDOBJ)
#include "reader/obj-reader.hh"
#endif
#if defined(LIGHTUSD_WITH_UFBX)
#include "reader/fbx-reader.hh"
#endif
#include "core-util.hh"
#include "safe-file-size.hh"

#include "layer/asset-anchor.hh"
#include "layer/layer.hh"
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "reader/usdz-reader.hh"
#include "c/lightusd-mtlx.h"
#include "external/filesystem/include/ghc/filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <limits>

namespace lightusd {
namespace pcp {

namespace {
#if defined(LIGHTUSD_WITH_USDOBJ)
tydra::import_asset::Options OBJOptions(const LayerLoadOptions& options) {
  tydra::import_asset::Options obj;
  obj.format = "obj";
  if (options.max_memory) {
    obj.max_memory = options.max_memory;
    obj.max_input = options.max_memory;
  }
  obj.read = options.asset_read;
  obj.open = options.asset_open;
  return obj;
}
bool IsOBJIdentifier(const std::string& key) {
  std::string package, entry;
  return EndsWithNoCase(AssetResolver::ParsePackagePath(key, &package, &entry) ? entry : key, ".obj");
}
#endif
#if defined(LIGHTUSD_WITH_UFBX)
bool IsFBXIdentifier(const std::string& key) {
  std::string package, entry;
  return EndsWithNoCase(AssetResolver::ParsePackagePath(key, &package, &entry) ? entry : key, ".fbx");
}
#endif


std::string ToLowerExt(const std::string &path) {
  std::string p = path;
  size_t bracket = p.find('[');
  if (bracket != std::string::npos) p.resize(bracket);
  auto dot = p.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string ext = p.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return ext;
}

std::string NormalizeEntryName(std::string name) {
  std::replace(name.begin(), name.end(), '\\', '/');
  while (!name.empty() && name.front() == '/') {
    name.erase(name.begin());
  }
  return name;
}


std::shared_ptr<Layer> ConvertLoadedUSDA(LoadResult &&r,
                                         const std::string &label,
                                         std::string *warn, std::string *err) {
  if (!r.success) {
    if (err) {
      *err += "Failed to load USDA layer: " + label + " : " +
              r.error_summary + "\n";
    }
    return nullptr;
  }
  for (const auto &w : r.warnings) {
    if (warn) *warn += w + "\n";
  }
  std::shared_ptr<Layer> layer = r.stage.ReleaseRootLayer();
  // Callback/memory layers need the same provenance as filesystem layers.
  // Composition otherwise loses the directory that authored texture assets.
  StampLayerAssetAnchor(layer.get(), label, true);
  return layer;
}

std::shared_ptr<Layer> ConvertLoadedUSDC(USDCLoadResult &&r,
                                         const std::string &label,
                                         std::string *warn, std::string *err) {
  if (!r.success) {
    if (err) {
      *err += "Failed to load USDC layer: " + label + " : " +
              r.error_summary + "\n";
    }
    return nullptr;
  }
  // Forward reader warnings like the USDA path does: a field the reader
  // could not decode is dropped from the layer, and the caller must see it.
  for (const auto &w : r.warnings) {
    if (warn) *warn += "USDC layer " + label + ": " + w + "\n";
  }
  std::shared_ptr<Layer> layer = r.stage.ReleaseRootLayer();
  // Callback/memory layers need the same provenance as filesystem layers.
  // Composition otherwise loses the directory that authored texture assets.
  StampLayerAssetAnchor(layer.get(), label, true);
  return layer;
}

ParseOptions MakeUSDAParseOptions(const LayerLoadOptions &options) {
  ParseOptions popts = options.usda_parse_options;
  popts.enable_timing = options.enable_timing || popts.enable_timing;
  if (options.progress_callback) popts.progress_callback = options.progress_callback;
  if (options.strict_aousd_conformance) {
    popts.strict_aousd_conformance = true;
  }
  popts.num_threads = options.parse_num_threads > 0 ? options.parse_num_threads
                                                    : popts.num_threads;
  popts.min_parallel_parse_bytes = std::max(
      popts.min_parallel_parse_bytes, options.min_parallel_parse_bytes);
  popts.max_file_size = MinNonZero(popts.max_file_size, options.max_memory);
  return popts;
}

std::shared_ptr<Layer> LoadLayerFromUSDZEntry(USDZReader &reader,
                                              const std::string &package_label,
                                              const std::string &entry_name,
                                              const LayerLoadOptions &options,
                                              std::string *warn,
                                              std::string *err) {
  int idx = -1;
  if (entry_name.empty()) {
    idx = reader.FindRootLayer();
  } else {
    const std::string want = NormalizeEntryName(entry_name);
    for (size_t i = 0; i < reader.NumEntries(); ++i) {
      if (NormalizeEntryName(reader.EntryName(i)) == want) {
        idx = static_cast<int>(i);
        break;
      }
    }
  }
  if (idx < 0) {
    if (err) {
      *err += "USDZ layer entry not found: " + package_label +
              (entry_name.empty() ? std::string() : "[" + entry_name + "]") +
              "\n";
    }
    return nullptr;
  }

  const uint8_t *data = reader.EntryData(static_cast<size_t>(idx));
  const size_t size = reader.EntrySize(static_cast<size_t>(idx));
  if (!data || size == 0) {
    if (err) *err += "USDZ layer entry is empty\n";
    return nullptr;
  }

  const std::string label = package_label + "[" + reader.EntryName(static_cast<size_t>(idx)) + "]";
#if defined(LIGHTUSD_WITH_USDOBJ)
  if (EndsWithNoCase(reader.EntryName(static_cast<size_t>(idx)), ".obj")) {
    auto obj_options = OBJOptions(options);
    auto resolve_entry = [&reader, &package_label](
        const std::string& name, const std::string& anchor, size_t* index,
        std::string* id, std::string* error) {
      std::string package, entry;
      if (!AssetResolver::ParsePackagePath(name, &package, &entry)) {
        if (!AssetResolver::ParsePackagePath(anchor, &package, &entry)) return false;
        entry = (ghc::filesystem::path(entry).parent_path() / name).lexically_normal().generic_string();
      }
      if (package != package_label || entry.empty() || entry.front() == '/' ||
          entry == ".." || entry.rfind("../", 0) == 0) {
        if (error) *error = "OBJ dependency escapes package: " + name;
        return false;
      }
      for (size_t i = 0; i < reader.NumEntries(); ++i) {
        if (NormalizeEntryName(reader.EntryName(i)) != entry) continue;
        *index = i;
        *id = package + "[" + entry + "]";
        return true;
      }
      if (error) *error = "Missing package OBJ dependency: " + entry;
      return false;
    };
    obj_options.read = [&reader, resolve_entry](const std::string& name,
        const std::string& anchor, std::vector<uint8_t>* bytes,
        std::string* resolved, std::string* error) {
      size_t index = 0;
      if (!resolve_entry(name, anchor, &index, resolved, error)) return false;
      const size_t size = reader.EntrySize(index);
      const auto* p = reader.EntryData(index);
      if (size && p) bytes->assign(p, p + size);
      else bytes->clear();
      return true;
    };
    // Separate resolution from I/O so aliased texture paths that map to one
    // package entry reuse the retained bytes instead of re-reading the zip.
    obj_options.open = [&reader, resolve_entry](const std::string& name,
        const std::string& anchor, tydra::import_asset::AssetSource* source,
        std::string* error) {
      if (!source) return false;
      size_t index = 0;
      std::string id;
      if (!resolve_entry(name, anchor, &index, &id, error)) return false;
      source->identifier = std::move(id);
      source->read = [&reader, index](std::vector<uint8_t>* bytes, std::string*) {
        const size_t size = reader.EntrySize(index);
        const auto* p = reader.EntryData(index);
        if (size && p) bytes->assign(p, p + size);
        else bytes->clear();
        return true;
      };
      return true;
    };
    return LoadOBJLayerFromMemory(label, data, size, obj_options, warn, err);
  }
#endif
  bool is_usdc = EndsWithNoCase(reader.EntryName(static_cast<size_t>(idx)), ".usdc");
  bool is_usda = EndsWithNoCase(reader.EntryName(static_cast<size_t>(idx)), ".usda");
  if (!is_usdc && !is_usda && size >= 8 &&
      std::memcmp(data, "PXR-USDC", 8) == 0) {
    is_usdc = true;
  }
  if (is_usdc) {
    USDCLoadOptions lopts;
    lopts.crate_options.max_memory = options.max_memory;
    static_cast<CrateLimits&>(lopts.crate_options) = options.usdc_limits;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.num_threads = options.parse_num_threads;
    lopts.crate_options.min_parallel_parse_bytes = options.min_parallel_parse_bytes;
    lopts.crate_options.enable_timing = options.enable_timing;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.use_mmap = options.usdc_use_mmap;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    auto layer = ConvertLoadedUSDC(LoadUSDCFromMemory(data, size, lopts), label, warn, err);
    StampPackageAssetAnchor(layer.get(), label);
    return layer;
  }
  if (is_usda) {
    LoadOptions lopts;
    lopts.parse_options = MakeUSDAParseOptions(options);
    auto layer = ConvertLoadedUSDA(
        LoadUSDAFromString(reinterpret_cast<const char *>(data), size, lopts),
        label, warn, err);
    StampPackageAssetAnchor(layer.get(), label);
    return layer;
  }

  if (err) *err += "Unsupported USDZ layer entry format: " + label + "\n";
  return nullptr;
}

std::shared_ptr<Layer> LoadLayerFromUSDZ(const std::string &package_file,
                                         const std::string &entry_name,
                                         const LayerLoadOptions &options,
                                         std::string *warn, std::string *err,
                                         const std::string& identifier = {}) {
  USDZReadOptions zopts;
  zopts.max_archive_size = options.max_memory;
  zopts.max_entry_size = options.max_memory;
  zopts.max_entries = options.max_archive_entries;

  USDZReader reader;
  if (!reader.OpenFile(package_file, zopts)) {
    if (err) {
      *err += "Failed to load USDZ package: " + package_file + " : " +
              (reader.Error().empty() ? "open failed" : reader.Error()) + "\n";
    }
    return nullptr;
  }

  return LoadLayerFromUSDZEntry(reader, identifier.empty() ? package_file : identifier,
                                entry_name, options,
                                warn, err);
}

}  // namespace

namespace {

// Stamp every prim in a freshly-loaded layer with the directory its RELATIVE
// asset paths anchor to: the layer's own directory. Composition flattens prims
// from many layers into one, so without this the authoring layer -- and with it
// the only correct anchor for `@../tex/foo.png@` -- is lost. See asset-anchor.hh.
//
// Package layers receive their full package-layer anchor when decoded. They
// must not be overwritten by the outer archive's filesystem directory.
void StampAssetAnchor(Layer *layer, const std::string &resolved_path) {
  if (!layer) return;
  const size_t slash = resolved_path.find_last_of("/\\");
  if (slash == std::string::npos) return;  // bare filename: cwd, nothing to add

  const uint32_t id = InternAssetAnchor(resolved_path.substr(0, slash));
  if (id == 0) return;
  for (size_t i = 0; i < layer->prim_count(); ++i) {
    if (PrimSpec *ps = layer->prim_mutable(static_cast<uint32_t>(i))) {
      ps->set_asset_anchor_id(id);
    }
  }
}

std::shared_ptr<Layer> LoadLayerFromFileUnstamped(
    const std::string &resolved_path, std::string *warn, std::string *err,
    const LayerLoadOptions &options);

}  // namespace

std::shared_ptr<Layer> LoadLayerFromFile(const std::string &resolved_path,
                                         std::string *warn, std::string *err,
                                         const LayerLoadOptions &options) {
  std::shared_ptr<Layer> layer =
      LoadLayerFromFileUnstamped(resolved_path, warn, err, options);
  // Package layers already carry their archive-aware anchor.
  if (layer && !AssetResolver::IsPackagePath(resolved_path) &&
      !EndsWithNoCase(resolved_path, ".usdz")) {
    StampAssetAnchor(layer.get(), resolved_path);
  }
  return layer;
}

std::shared_ptr<Layer> LoadLayerFromSourceSnapshot(
    const std::string& identifier, const std::string& storage_path,
    std::string* warn, std::string* err, const LayerLoadOptions& options,
    bool content_dispatch) {
  if (!::lightusd::NativeIOAvailable(err)) return {};
  char magic[8] = {};
  std::ifstream input(storage_path, std::ios::binary);
  input.read(magic, sizeof(magic));
  const size_t got = static_cast<size_t>(input.gcount());
  const bool crate_magic = got == 8 && std::memcmp(magic, "PXR-USDC", 8) == 0;
  const bool zip_magic = got >= 4 && std::memcmp(magic, "PK\x03\x04", 4) == 0;
  const std::string ext = ToLowerExt(identifier);
  const bool crate = content_dispatch ? crate_magic
      : !AssetResolver::IsPackagePath(identifier) &&
          (ext == "usdc" || (ext == "usd" && crate_magic));
  const bool zip = content_dispatch ? zip_magic
      : ext == "usdz" || AssetResolver::IsPackagePath(identifier);
  std::shared_ptr<Layer> layer;
  if (crate) {
    USDCLoadOptions opts;
    opts.crate_options.strict_aousd_conformance = options.strict_aousd_conformance;
    opts.crate_options.max_memory = options.max_memory;
    static_cast<CrateLimits&>(opts.crate_options) = options.usdc_limits;
    opts.crate_options.max_array_elements = options.max_array_elements;
    opts.crate_options.progress_callback = options.progress_callback;
    opts.crate_options.num_threads = options.parse_num_threads;
    opts.crate_options.min_parallel_parse_bytes = options.min_parallel_parse_bytes;
    opts.crate_options.enable_timing = options.enable_timing;
    opts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    opts.crate_options.use_mmap = options.usdc_use_mmap;
    // `storage_path` is already the loader's private, write-once snapshot.
    // Re-snapshotting it would copy the closure twice; keep the owned replay.
    opts.crate_options.paged_snapshot = false;
    layer = ConvertLoadedUSDC(LoadUSDCFromFile(storage_path, opts),
                             identifier, warn, err);
  } else if (zip) {
    std::string package, entry;
    AssetResolver::ParsePackagePath(identifier, &package, &entry);
    layer = LoadLayerFromUSDZ(storage_path, entry, options, warn, err,
                              package.empty() ? identifier : package);
  } else {
    LoadOptions opts;
    opts.parse_options = MakeUSDAParseOptions(options);
    layer = ConvertLoadedUSDA(LoadUSDAFromFile(storage_path, opts),
                             identifier, warn, err);
  }
  if (layer && !zip && !AssetResolver::IsPackagePath(identifier))
    StampAssetAnchor(layer.get(), identifier);
  return layer;
}

namespace {

std::shared_ptr<Layer> LoadLayerFromFileUnstamped(
    const std::string &resolved_path, std::string *warn, std::string *err,
    const LayerLoadOptions &options) {
  if (!::lightusd::NativeIOAvailable(err)) return {};
  if (AssetResolver::IsPackagePath(resolved_path)) {
    std::string package_file;
    std::string entry_name;
    if (AssetResolver::ParsePackagePath(resolved_path, &package_file,
                                        &entry_name)) {
      return LoadLayerFromUSDZ(package_file, entry_name, options, warn, err);
    }
  }

  std::string ext = ToLowerExt(resolved_path);
#if defined(LIGHTUSD_WITH_USDOBJ)
  if (ext == "obj")
    return LoadOBJLayerFromFile(resolved_path, OBJOptions(options), warn, err);
#endif
#if defined(LIGHTUSD_WITH_UFBX)
  if (ext == "fbx")
    return LoadFBXLayerFromFile(resolved_path, OBJOptions(options), warn, err);
#endif

  // `.usd` is ambiguous (USDA text OR crate binary). Disambiguate by the crate
  // magic ("PXR-USDC"); UE-exported scenes ship the root layer as `.usd` crate.
  if (ext == "usd") {
    char magic[8] = {0};
    std::ifstream f(resolved_path, std::ios::binary);
    if (f.read(magic, sizeof(magic)) &&
        std::memcmp(magic, "PXR-USDC", sizeof(magic)) == 0) {
      ext = "usdc";
    } else {
      ext = "usda";
    }
  }

  if (ext == "usda") {
    LoadOptions lopts;
    lopts.parse_options = options.usda_parse_options;
    lopts.parse_options.num_threads =
        options.parse_num_threads > 0 ? options.parse_num_threads
                                     : lopts.parse_options.num_threads;
    lopts.parse_options.min_parallel_parse_bytes = std::max(
        lopts.parse_options.min_parallel_parse_bytes,
        options.min_parallel_parse_bytes);
    lopts.parse_options.max_file_size =
        MinNonZero(lopts.parse_options.max_file_size, options.max_memory);
    return ConvertLoadedUSDA(LoadUSDAFromFile(resolved_path, lopts),
                             resolved_path, warn, err);
  }

  if (ext == "usdc") {
    USDCLoadOptions lopts;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    lopts.crate_options.max_memory = options.max_memory;
    static_cast<CrateLimits&>(lopts.crate_options) = options.usdc_limits;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.num_threads = options.parse_num_threads;
    lopts.crate_options.min_parallel_parse_bytes = options.min_parallel_parse_bytes;
    lopts.crate_options.enable_timing = options.enable_timing;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.use_mmap = options.usdc_use_mmap;
    return ConvertLoadedUSDC(LoadUSDCFromFile(resolved_path, lopts),
                             resolved_path, warn, err);
  }

  if (ext == "usdz") {
    return LoadLayerFromUSDZ(resolved_path, std::string(), options, warn, err);
  }

  if (ext == "mtlx") {
    std::ifstream f(resolved_path, std::ios::binary | std::ios::ate);
    if (!f) {
      if (err) *err += "Failed to open MaterialX layer: " + resolved_path + "\n";
      return nullptr;
    }
    // A MaterialX document is XML; 256 MB is far past anything real, and the
    // bound must be UNCONDITIONAL -- the old max_memory check only applied
    // when a budget was configured, and the size_t comparison it sat next to
    // is a no-op on LP64. A directory reports LLONG_MAX here (see
    // SafeStreamSize) and reached std::string(n, '\0'), which threw an
    // uncaught length_error and killed the process.
    constexpr uint64_t kMaxMtlxBytes = 256ull * 1024 * 1024;
    const uint64_t mtlx_cap =
        options.max_memory > 0
            ? (static_cast<uint64_t>(options.max_memory) < kMaxMtlxBytes
                   ? static_cast<uint64_t>(options.max_memory)
                   : kMaxMtlxBytes)
            : kMaxMtlxBytes;
    size_t size = 0;
    if (!SafeStreamSize(f, mtlx_cap, &size)) {
      if (err) *err += "Invalid MaterialX layer size: " + resolved_path + "\n";
      return nullptr;
    }
    std::string data(size, '\0');
    f.seekg(0, std::ios::beg);
    if (!f.read(&data[0], static_cast<std::streamsize>(size))) {
      if (err) *err += "Failed to read MaterialX layer: " + resolved_path + "\n";
      return nullptr;
    }
    return LoadLayerFromMtlxMemory(
        resolved_path, reinterpret_cast<const uint8_t *>(data.data()),
        data.size(), warn, err);
  }

  if (err) {
    *err += "Unsupported layer file format for: " + resolved_path + "\n";
  }
  return nullptr;
}

}  // namespace

std::shared_ptr<Layer> LoadLayerFromFile(const std::string &resolved_path,
                                         std::string *warn, std::string *err,
                                         int parse_num_threads) {
  LayerLoadOptions options;
  options.parse_num_threads = parse_num_threads;
  options.usda_parse_options.num_threads = parse_num_threads;
  return LoadLayerFromFile(resolved_path, warn, err, options);
}


// ============================================================
// MaterialX (.mtlx) layers
// ============================================================
//
// A reference like @mat.mtlx@</MaterialX/Materials/Foo> composes against a
// skeletal /MaterialX prim tree synthesized from the MaterialX XML (the same
// shape legacy usdMtlx ToPrimSpec produces):
//
//   /MaterialX/Materials/<mat>       Material  (config:mtlx:version,
//                                    outputs:mtlx:surface source shader name)
//   /MaterialX/Shaders/<node>        Shader    (info:id + scalar inputs)
//   /MaterialX/NodeGraphs/<ng>/<n>   Shader    (info:id = mtlx category)

bool LooksLikeMtlxXML(const uint8_t *data, size_t size) {
  size_t pos = 0;
  while (pos < size &&
         (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' ||
          data[pos] == '\n')) {
    pos++;
  }
  if (pos + 5 < size && std::memcmp(data + pos, "<?xml", 5) == 0) {
    // XML prolog: look for a materialx root element in the head of the file.
    const size_t scan = size - pos < 512 ? size - pos : 512;
    const char *head = reinterpret_cast<const char *>(data + pos);
    for (size_t i = 0; i + 10 < scan; ++i) {
      if (std::memcmp(head + i, "<materialx", 10) == 0) return true;
    }
    return false;
  }
  return pos + 10 < size && std::memcmp(data + pos, "<materialx", 10) == 0;
}

namespace {

using MtlxElemPtr = const lu_mtlx_elem *;

// Attribute of a MaterialX element ("" when absent).
std::string MtlxGet(MtlxElemPtr e, const char *key) { return lu_mtlx_elem_get(e, key); }

// Typed value of an <input> (see lu_mtlx_input_value).
struct MtlxInputValue {
  lu_mtlx_value v{};
  explicit MtlxInputValue(MtlxElemPtr input) {
    if (!lu_mtlx_input_value(input, &v)) v.type = LU_MTLX_VALUE_NONE;
  }
  ~MtlxInputValue() { lu_mtlx_value_free(&v); }
  MtlxInputValue(const MtlxInputValue &) = delete;
  MtlxInputValue &operator=(const MtlxInputValue &) = delete;
};

Value MtlxValueToCoreValue(const lu_mtlx_value &v, const std::string &mtlx_type = std::string()) {
  // Type float tuples by the MaterialX port type (UsdMtlx: color3 ->
  // color3f, vector3 -> vector3f, ...), not just by arity.
  if (v.type == LU_MTLX_VALUE_FLOATS) {
    const float *f = v.floats;
    if (mtlx_type == "color3" && v.nfloats == 3) return Value::MakeColor3f(f[0], f[1], f[2]);
    if (mtlx_type == "color4" && v.nfloats == 4) return Value::MakeColor4f(f[0], f[1], f[2], f[3]);
    if (mtlx_type == "vector3" && v.nfloats == 3) return Value::MakeVector3f(f[0], f[1], f[2]);
  }
  switch (v.type) {
    case LU_MTLX_VALUE_BOOL: return Value(v.b != 0);
    case LU_MTLX_VALUE_INT: return Value(static_cast<int32_t>(v.i));
    case LU_MTLX_VALUE_FLOAT: return Value(v.f);
    case LU_MTLX_VALUE_STRING: return Value::MakeToken(v.s);
    case LU_MTLX_VALUE_FLOATS: {
      const float *f = v.floats;
      if (v.nfloats == 2) return Value::MakeFloat2(f[0], f[1]);
      if (v.nfloats == 3) return Value::MakeFloat3(f[0], f[1], f[2]);
      if (v.nfloats == 4) return Value::MakeFloat4(f[0], f[1], f[2], f[3]);
      return Value();
    }
    default: return Value();
  }
}

const char *MtlxShaderInfoId(const std::string &category) {
  if (category == "standard_surface") return "MtlxAutodeskStandardSurface";
  if (category == "open_pbr_surface") return "MtlxOpenPBRSurface";
  if (category == "UsdPreviewSurface") return "UsdPreviewSurface";
  return nullptr;
}

std::pair<TypeId, const char *> MtlxConnectionType(
    const std::string &type) {
  if (type == "boolean") return {TypeId::Bool, "bool"};
  if (type == "integer") return {TypeId::Int, "int"};
  if (type == "float") return {TypeId::Float, "float"};
  if (type == "vector2") return {TypeId::Float2, "float2"};
  if (type == "vector3") return {TypeId::Vector3f, "vector3f"};
  if (type == "vector4") return {TypeId::Float4, "float4"};
  if (type == "color3") return {TypeId::Color3f, "color3f"};
  if (type == "color4") return {TypeId::Color4f, "color4f"};
  if (type == "filename") return {TypeId::AssetPath, "asset"};
  return {TypeId::Token, "token"};
}

void DeclareMtlxConnectionProperty(LayerBuilder &lb,
                                   const std::string &name,
                                   const std::string &mtlx_type) {
  PrimSpec *prim = lb.current();
  if (!prim) return;
  const PropNameId name_id = GetPropNameTable().intern(name);
  if (prim->property(name_id)) return;
  const auto mapped = MtlxConnectionType(mtlx_type);
  prim->add_property_slot(name_id, mapped.first, PropSlot::kFlagConnection);
  prim->set_property_type_name(name, mapped.second);
}

// Declares a typed, value-less output port (UsdMtlx: `color3f outputs:out`,
// `token outputs:out` for shader-semantic types).
void DeclareMtlxOutput(LayerBuilder &lb, const std::string &name, const std::string &mtlx_type) {
  PrimSpec *prim = lb.current();
  if (!prim) return;
  const PropNameId name_id = GetPropNameTable().intern(name);
  if (prim->property(name_id)) return;
  const auto mapped = MtlxConnectionType(mtlx_type);
  prim->add_property_slot(name_id, mapped.first, 0);
  prim->set_property_type_name(name, mapped.second);
}

void EmitMtlxNodePrim(LayerBuilder &lb, MtlxElemPtr node,
                      const char *forced_info_id,
                      const std::string &node_prefix = std::string(),
                      const std::string &nodegraph_prefix = std::string(),
                      const std::string &interface_owner = std::string()) {
  const std::string name = MtlxGet(node, "name");
  const std::string category = lu_mtlx_elem_category(node);
  lb.begin_prim(name.empty() ? std::string("node") : name, "Shader");
  const char *info_id = forced_info_id ? forced_info_id : MtlxShaderInfoId(category);
  lb.add_property("info:id", Value::MakeToken(info_id ? std::string(info_id) : category));
  const size_t ninputs = lu_mtlx_elem_count(node, LU_MTLX_CHILD_INPUTS);
  for (size_t i = 0; i < ninputs; ++i) {
    MtlxElemPtr input = lu_mtlx_elem_at(node, LU_MTLX_CHILD_INPUTS, i);
    const std::string input_name = MtlxGet(input, "name");
    const std::string input_type = MtlxGet(input, "type");
    const MtlxInputValue input_value(input);
    Value value =
        input_type == "filename" && input_value.v.type == LU_MTLX_VALUE_STRING
            // MaterialX: the nearest enclosing `fileprefix` is prepended to
            // filename inputs only (scoped attributes are inherited onto
            // every element at parse time, see lu_mtlx_inherit_attributes).
            ? Value::MakeAssetPath(MtlxGet(input, "fileprefix") + input_value.v.s)
            : MtlxValueToCoreValue(input_value.v, input_type);
    if (!value.is_empty()) {
      const std::string property = "inputs:" + input_name;
      lb.add_property(property, std::move(value));
      const std::string colorspace = MtlxGet(input, "colorspace");
      if (!colorspace.empty()) {
        if (PrimSpec *prim = lb.current()) {
          PropMeta &meta = prim->ensure_property_meta(property);
          meta.colorSpace = colorspace;
          meta.authored |= PropMeta::kColorSpace;
        }
      }
    }
    {
      const std::string nodename = MtlxGet(input, "nodename");
      const std::string nodegraph = MtlxGet(input, "nodegraph");
      std::string target;
      if (!node_prefix.empty() && !nodename.empty()) {
        target = node_prefix + "/" + nodename;
      } else if (!nodegraph_prefix.empty() && !nodegraph.empty()) {
        target = nodegraph_prefix + "/" + nodegraph;
      }
      if (!target.empty()) {
        const std::string output = MtlxGet(input, "output");
        const std::string property = "inputs:" + input_name;
        DeclareMtlxConnectionProperty(lb, property, input_type);
        lb.add_connection(property,
                          Path(target + (output.empty()
                                             ? ".outputs:out"
                                             : ".outputs:" + output)));
      } else {
        // `interfacename` binds the input to the enclosing nodegraph's
        // interface input (UsdMtlx: <graph>.inputs:<interfacename>).
        const std::string iface = MtlxGet(input, "interfacename");
        if (!iface.empty() && !interface_owner.empty()) {
          const std::string property = "inputs:" + input_name;
          DeclareMtlxConnectionProperty(lb, property, input_type);
          lb.add_connection(property, Path(interface_owner + ".inputs:" + iface));
        }
      }
    }
  }
  const std::string type = MtlxGet(node, "type");
  if (!type.empty()) DeclareMtlxOutput(lb, "outputs:out", type);
  lb.end_prim();
}

// Nodegraphs and top-level nodes reachable from `roots` through input
// `nodegraph`/`nodename` references (worklist; documents may share graphs).
void CollectMtlxReachable(const lu_mtlx_document *doc,
                          std::initializer_list<MtlxElemPtr> roots,
                          std::set<std::string> *graphs,
                          std::set<std::string> *nodes) {
  // (element whose <input>s to visit, at document scope): inside a nodegraph
  // `nodename` names a sibling of that graph, not a top-level node.
  std::vector<std::pair<MtlxElemPtr, bool>> work;
  for (MtlxElemPtr r : roots)
    if (r) work.push_back({r, true});
  while (!work.empty()) {
    const auto item = work.back();
    work.pop_back();
    const size_t ninputs = lu_mtlx_elem_count(item.first, LU_MTLX_CHILD_INPUTS);
    for (size_t i = 0; i < ninputs; ++i) {
      MtlxElemPtr in = lu_mtlx_elem_at(item.first, LU_MTLX_CHILD_INPUTS, i);
      const std::string g = MtlxGet(in, "nodegraph");
      const std::string nodename = MtlxGet(in, "nodename");
      if (!g.empty()) {
        if (!graphs->insert(g).second) continue;
        const size_t ngraphs = lu_mtlx_document_count(doc, LU_MTLX_NODEGRAPHS);
        for (size_t k = 0; k < ngraphs; ++k) {
          MtlxElemPtr graph = lu_mtlx_document_at(doc, LU_MTLX_NODEGRAPHS, k);
          if (MtlxGet(graph, "name") != g) continue;
          // interface inputs live in the enclosing (document) scope
          work.push_back({graph, true});
          const size_t nnodes = lu_mtlx_elem_count(graph, LU_MTLX_CHILD_GRAPH_NODES);
          for (size_t n = 0; n < nnodes; ++n)
            work.push_back({lu_mtlx_elem_at(graph, LU_MTLX_CHILD_GRAPH_NODES, n), false});
        }
      } else if (item.second && !nodename.empty()) {
        MtlxElemPtr top = lu_mtlx_document_find(doc, LU_MTLX_NODES, nodename.c_str());
        if (top && nodes->insert(nodename).second) work.push_back({top, true});
      }
    }
  }
}

// A node by name: top-level nodes, then the nodes of every nodegraph.
MtlxElemPtr FindMtlxNode(const lu_mtlx_document *doc, const std::string &name) {
  if (name.empty()) return nullptr;
  return lu_mtlx_document_find(doc, LU_MTLX_NODES, name.c_str());
}

void EmitMtlxNodeGraphPrim(LayerBuilder &lb, MtlxElemPtr graph,
                           const std::string &graph_path,
                           const std::string &nodegraph_prefix) {
  lb.begin_prim(MtlxGet(graph, "name"), "NodeGraph");
  // Interface inputs (nodegraph-level <input>s) with their typed defaults.
  const size_t ninterface = lu_mtlx_elem_count(graph, LU_MTLX_CHILD_INPUTS);
  for (size_t i = 0; i < ninterface; ++i) {
    MtlxElemPtr input = lu_mtlx_elem_at(graph, LU_MTLX_CHILD_INPUTS, i);
    const std::string input_name = MtlxGet(input, "name");
    if (input_name.empty()) continue;
    const std::string input_type = MtlxGet(input, "type");
    const MtlxInputValue input_value(input);
    Value value =
        input_type == "filename" && input_value.v.type == LU_MTLX_VALUE_STRING
            ? Value::MakeAssetPath(MtlxGet(input, "fileprefix") + input_value.v.s)
            : MtlxValueToCoreValue(input_value.v, input_type);
    if (!value.is_empty()) {
      lb.add_property("inputs:" + input_name, std::move(value));
    } else {
      DeclareMtlxOutput(lb, "inputs:" + input_name, input_type);
    }
  }
  const size_t nnodes = lu_mtlx_elem_count(graph, LU_MTLX_CHILD_GRAPH_NODES);
  for (size_t i = 0; i < nnodes; ++i) {
    MtlxElemPtr node = lu_mtlx_elem_at(graph, LU_MTLX_CHILD_GRAPH_NODES, i);
    if (MtlxGet(node, "name").empty()) continue;
    EmitMtlxNodePrim(lb, node, nullptr, graph_path, nodegraph_prefix, graph_path);
  }
  const size_t noutputs = lu_mtlx_elem_count(graph, LU_MTLX_CHILD_OUTPUTS);
  for (size_t i = 0; i < noutputs; ++i) {
    MtlxElemPtr output = lu_mtlx_elem_at(graph, LU_MTLX_CHILD_OUTPUTS, i);
    const std::string output_name = MtlxGet(output, "name");
    const std::string nodename = MtlxGet(output, "nodename");
    if (output_name.empty() || nodename.empty()) continue;
    const std::string out = MtlxGet(output, "output");
    const std::string output_target =
        graph_path + "/" + nodename + (out.empty() ? ".outputs:out" : ".outputs:" + out);
    const std::string property = "outputs:" + output_name;
    DeclareMtlxConnectionProperty(lb, property, MtlxGet(output, "type"));
    lb.add_connection(property, Path(output_target));
  }
  lb.end_prim();
}

}  // namespace

std::shared_ptr<Layer> LoadLayerFromMtlxMemory(const std::string &key,
                                               const uint8_t *data,
                                               size_t size, std::string *warn,
                                               std::string *err) {
  std::unique_ptr<lu_mtlx_document, decltype(&lu_mtlx_document_destroy)> doc_owner(
      lu_mtlx_document_create(), &lu_mtlx_document_destroy);
  const lu_mtlx_document *doc = doc_owner.get();
  if (!doc_owner ||
      !lu_mtlx_document_parse_anchored(doc_owner.get(), reinterpret_cast<const char *>(data),
                                       size, key.c_str())) {
    if (err) {
      const std::string error = doc_owner ? lu_mtlx_document_error(doc) : "out of memory";
      *err += "Failed to parse MaterialX layer: " + key +
              (error.empty() ? std::string() : " : " + error) + "\n";
    }
    return nullptr;
  }
  const std::string doc_warning = lu_mtlx_document_warning(doc);
  if (warn && !doc_warning.empty()) {
    *warn += "MaterialX layer " + key + ": " + doc_warning + "\n";
  }
  // The upgraded version, as UsdMtlx reports it (MaterialX upgrades documents
  // to the library version on read).
  const std::string doc_version = lu_mtlx_document_version(doc);
  const std::string doc_colorspace = lu_mtlx_document_colorspace(doc);

  auto layer = std::make_shared<Layer>();
  LayerBuilder lb(*layer);

  lb.begin_prim("MaterialX", "");

  lb.begin_prim("Materials", "");
  const size_t nmaterials = lu_mtlx_document_count(doc, LU_MTLX_MATERIALS);
  for (size_t mi = 0; mi < nmaterials; ++mi) {
    MtlxElemPtr mat = lu_mtlx_document_at(doc, LU_MTLX_MATERIALS, mi);
    const std::string mat_name = MtlxGet(mat, "name");
    if (mat_name.empty()) continue;
    lu_mtlx_material_refs refs;
    lu_mtlx_material_refs_get(mat, &refs);
    const std::string surface_name = refs.surface;
    const std::string displacement_name = refs.displacement;
    const std::string volume_name = refs.volume;
    lb.begin_prim(mat_name, "Material");
    if (PrimSpec *prim = lb.current()) {
      prim->meta().apiSchemas().push_back("MaterialXConfigAPI");
    }
    if (!doc_version.empty()) {
      lb.add_property("config:mtlx:version", Value(doc_version));
    }
    if (!doc_colorspace.empty()) {
      lb.add_property("config:mtlx:colorspace", Value(doc_colorspace));
    }
    // A USD reference to /MaterialX/Materials/<name> only brings that
    // material subtree into the host layer. Keep the terminal surface shader
    // inside the material as well; the canonical library namespaces below
    // (/MaterialX/Shaders and /MaterialX/NodeGraphs) are useful for flattened
    // layers but are outside the reference target and therefore cannot be
    // relied on by a composed external MaterialX asset.
    const std::string material_prefix = "/MaterialX/Materials/" + mat_name;
    MtlxElemPtr surface = FindMtlxNode(doc, surface_name);
    MtlxElemPtr displacement = FindMtlxNode(doc, displacement_name);
    MtlxElemPtr volume = FindMtlxNode(doc, volume_name);
    if (surface || displacement || volume) {
      if (surface) {
        EmitMtlxNodePrim(lb, surface, nullptr, material_prefix, material_prefix);
      }
      if (displacement && displacement != surface) {
        EmitMtlxNodePrim(lb, displacement, nullptr, material_prefix, material_prefix);
      }
      if (volume && volume != surface && volume != displacement) {
        EmitMtlxNodePrim(lb, volume, nullptr, material_prefix, material_prefix);
      }
      // Keep the graph and its output routing inside the referenced Material
      // subtree. This preserves image/normal/utility nodes and lets the next
      // shader evaluator follow the same connections as the source MaterialX
      // document after USD composition.
      // Only what this material reaches: emitting every nodegraph of the
      // document into every material multiplied the layer by the material
      // count. Top-level nodes reached through `nodename` are emitted here
      // too, since the shader's connections target <material>/<node>.
      std::set<std::string> graphs, nodes;
      CollectMtlxReachable(doc, {surface, displacement, volume}, &graphs,
                           &nodes);
      const size_t nnodes = lu_mtlx_document_count(doc, LU_MTLX_NODES);
      for (size_t ni = 0; ni < nnodes; ++ni) {
        MtlxElemPtr node = lu_mtlx_document_at(doc, LU_MTLX_NODES, ni);
        if (node == surface || node == displacement || node == volume ||
            !nodes.count(MtlxGet(node, "name"))) {
          continue;
        }
        EmitMtlxNodePrim(lb, node, nullptr, material_prefix, material_prefix);
      }
      const size_t ngraphs = lu_mtlx_document_count(doc, LU_MTLX_NODEGRAPHS);
      for (size_t gi = 0; gi < ngraphs; ++gi) {
        MtlxElemPtr graph = lu_mtlx_document_at(doc, LU_MTLX_NODEGRAPHS, gi);
        const std::string graph_name = MtlxGet(graph, "name");
        if (graph_name.empty() || !graphs.count(graph_name)) continue;
        EmitMtlxNodeGraphPrim(lb, graph, material_prefix + "/" + graph_name, material_prefix);
      }
    }
    // UsdMtlx terminals: outputs:mtlx:<terminal> connects to the shader.
    if (surface) {
      DeclareMtlxConnectionProperty(lb, "outputs:mtlx:surface", "surfaceshader");
      lb.add_connection("outputs:mtlx:surface",
                        Path(material_prefix + "/" + surface_name + ".outputs:out"));
    }
    if (displacement) {
      DeclareMtlxConnectionProperty(lb, "outputs:mtlx:displacement", "displacementshader");
      lb.add_connection("outputs:mtlx:displacement",
                        Path(material_prefix + "/" + displacement_name + ".outputs:out"));
    }
    if (volume) {
      DeclareMtlxConnectionProperty(lb, "outputs:mtlx:volume", "volumeshader");
      lb.add_connection("outputs:mtlx:volume",
                        Path(material_prefix + "/" + volume_name + ".outputs:out"));
    }
    if (!surface_name.empty()) {
      lb.add_relationship("mtlx:surface:source",
                          Path(surface ? material_prefix + "/" + surface_name
                                       : "/MaterialX/Shaders/" + surface_name));
    }
    if (!displacement_name.empty()) {
      lb.add_relationship("mtlx:displacement:source",
                          Path(displacement ? material_prefix + "/" + displacement_name
                                            : "/MaterialX/Shaders/" + displacement_name));
    }
    if (!volume_name.empty()) {
      lb.add_relationship("mtlx:volume:source",
                          Path(volume ? material_prefix + "/" + volume_name
                                      : "/MaterialX/Shaders/" + volume_name));
    }
    lb.end_prim();
  }
  lb.end_prim();  // Materials

  lb.begin_prim("Shaders", "");
  const size_t ntop = lu_mtlx_document_count(doc, LU_MTLX_NODES);
  for (size_t ni = 0; ni < ntop; ++ni) {
    MtlxElemPtr node = lu_mtlx_document_at(doc, LU_MTLX_NODES, ni);
    if (MtlxGet(node, "name").empty()) continue;
    // surfacematerial nodes are represented under /MaterialX/Materials.
    if (std::string(lu_mtlx_elem_category(node)) == "surfacematerial") continue;
    EmitMtlxNodePrim(lb, node, nullptr, "/MaterialX/Shaders", "/MaterialX/NodeGraphs");
  }
  lb.end_prim();  // Shaders

  const size_t nallgraphs = lu_mtlx_document_count(doc, LU_MTLX_NODEGRAPHS);
  if (nallgraphs) {
    lb.begin_prim("NodeGraphs", "");
    for (size_t gi = 0; gi < nallgraphs; ++gi) {
      MtlxElemPtr graph = lu_mtlx_document_at(doc, LU_MTLX_NODEGRAPHS, gi);
      const std::string graph_name = MtlxGet(graph, "name");
      if (graph_name.empty()) continue;
      EmitMtlxNodeGraphPrim(lb, graph, "/MaterialX/NodeGraphs/" + graph_name,
                            "/MaterialX/NodeGraphs");
    }
    lb.end_prim();  // NodeGraphs
  }

  lb.end_prim();  // MaterialX
  lb.finalize();
  layer->build_path_index();
  return layer;
}

std::shared_ptr<Layer> LoadLayerFromMemory(const std::string &key,
                                           const uint8_t *data, size_t size,
                                           std::string *warn, std::string *err,
                                           const LayerLoadOptions &options) {
  if (!data || size == 0) {
    if (err) *err += "Empty layer buffer for: " + key + "\n";
    return nullptr;
  }
  if (options.max_memory > 0 && size > options.max_memory) {
    if (err) {
      *err += "Layer buffer exceeds max_memory for: " + key + "\n";
    }
    return nullptr;
  }

  if (size >= 8 && std::memcmp(data, "PXR-USDC", 8) == 0) {
    USDCLoadOptions lopts;
    lopts.crate_options.max_memory = options.max_memory;
    static_cast<CrateLimits&>(lopts.crate_options) = options.usdc_limits;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.num_threads = options.parse_num_threads;
    lopts.crate_options.min_parallel_parse_bytes = options.min_parallel_parse_bytes;
    lopts.crate_options.enable_timing = options.enable_timing;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    return ConvertLoadedUSDC(LoadUSDCFromMemory(data, size, lopts), key, warn, err);
  }

  if (size >= 4 && std::memcmp(data, "PK\x03\x04", 4) == 0) {
    // A package-path key ("pkg.usdz[entry]") selects a specific entry;
    // otherwise the first .usdc (then .usda) entry is the root layer.
    std::string entry_name;
    if (AssetResolver::IsPackagePath(key)) {
      std::string package_file;
      AssetResolver::ParsePackagePath(key, &package_file, &entry_name);
    }
    USDZReadOptions zopts;
    zopts.max_archive_size = options.max_memory;
    zopts.max_entry_size = options.max_memory;
    zopts.max_entries = options.max_archive_entries;
    USDZReader reader;
    if (!reader.Open(data, size, zopts)) {
      if (err) {
        *err += "Failed to open USDZ layer buffer: " + key + " : " +
                (reader.Error().empty() ? "open failed" : reader.Error()) +
                "\n";
      }
      return nullptr;
    }
    return LoadLayerFromUSDZEntry(reader, key, entry_name, options, warn, err);
  }

  if (LooksLikeMtlxXML(data, size)) {
    return LoadLayerFromMtlxMemory(key, data, size, warn, err);
  }
#if defined(LIGHTUSD_WITH_USDOBJ)
  if (IsOBJIdentifier(key))
    return LoadOBJLayerFromMemory(key, data, size, OBJOptions(options), warn, err);
#endif
#if defined(LIGHTUSD_WITH_UFBX)
  if (IsFBXIdentifier(key))
    return LoadFBXLayerFromMemory(key, data, size, OBJOptions(options), warn, err);
#endif

  LoadOptions lopts;
  lopts.parse_options = MakeUSDAParseOptions(options);
  return ConvertLoadedUSDA(
      LoadUSDAFromString(reinterpret_cast<const char *>(data), size, lopts),
      key, warn, err);
}

std::shared_ptr<Layer> LoadLayerFromMemoryOwned(const std::string &key,
                                                std::string &&data,
                                                std::string *warn,
                                                std::string *err,
                                                const LayerLoadOptions &options) {
  if (data.empty()) {
    if (err) *err += "Empty layer buffer for: " + key + "\n";
    return nullptr;
  }
#if defined(LIGHTUSD_WITH_USDOBJ)
  if (IsOBJIdentifier(key))
    return LoadLayerFromMemory(key, reinterpret_cast<const uint8_t*>(data.data()),
                                data.size(), warn, err, options);
#endif
#if defined(LIGHTUSD_WITH_UFBX)
  if (IsFBXIdentifier(key))
    return LoadLayerFromMemory(key, reinterpret_cast<const uint8_t*>(data.data()),
                                data.size(), warn, err, options);
#endif
  if (options.max_memory > 0 && data.size() > options.max_memory) {
    if (err) {
      *err += "Layer buffer exceeds max_memory for: " + key + "\n";
    }
    return nullptr;
  }

  if (data.size() >= 8 && std::memcmp(data.data(), "PXR-USDC", 8) == 0) {
    USDCLoadOptions lopts;
    lopts.crate_options.max_memory = options.max_memory;
    static_cast<CrateLimits&>(lopts.crate_options) = options.usdc_limits;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.num_threads = options.parse_num_threads;
    lopts.crate_options.min_parallel_parse_bytes = options.min_parallel_parse_bytes;
    lopts.crate_options.enable_timing = options.enable_timing;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    return ConvertLoadedUSDC(LoadUSDCFromMemoryOwned(std::move(data), lopts),
                             key, warn, err);
  }

  if (data.size() >= 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0) {
    // USDZ entries are parsed with copying readers, so the archive buffer can
    // be released when this call returns.
    return LoadLayerFromMemory(key,
                               reinterpret_cast<const uint8_t *>(data.data()),
                               data.size(), warn, err, options);
  }

  if (LooksLikeMtlxXML(reinterpret_cast<const uint8_t *>(data.data()),
                       data.size())) {
    return LoadLayerFromMtlxMemory(
        key, reinterpret_cast<const uint8_t *>(data.data()), data.size(), warn,
        err);
  }

  LoadOptions lopts;
  lopts.parse_options = MakeUSDAParseOptions(options);
  return ConvertLoadedUSDA(LoadUSDAFromStringOwned(std::move(data), lopts),
                           key, warn, err);
}

}  // namespace pcp
}  // namespace lightusd
