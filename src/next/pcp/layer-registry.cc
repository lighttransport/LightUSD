// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP LayerRegistry implementation

#include "layer-registry.hh"
#include "../safe-file-size.hh"

#include "../layer/asset-anchor.hh"
#include "../layer/layer.hh"
#include "../reader/usda-reader.hh"
#include "../reader/usdc-reader.hh"
#include "../reader/usdz-reader.hh"
#include "../../mtlx-dom.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace lightusd {
namespace next {
namespace pcp {

namespace {

size_t MinNonZero(size_t a, size_t b) {
  if (a == 0) return b;
  if (b == 0) return a;
  return std::min(a, b);
}

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

bool EndsWithNoCase(const std::string &s, const char *suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char a = static_cast<unsigned char>(s[s.size() - n + i]);
    unsigned char b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
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
  return r.stage.ReleaseRootLayer();
}

std::shared_ptr<Layer> ConvertLoadedUSDC(USDCLoadResult &&r,
                                         const std::string &label,
                                         std::string *err) {
  if (!r.success) {
    if (err) {
      *err += "Failed to load USDC layer: " + label + " : " +
              r.error_summary + "\n";
    }
    return nullptr;
  }
  return r.stage.ReleaseRootLayer();
}

ParseOptions MakeUSDAParseOptions(const LayerLoadOptions &options) {
  ParseOptions popts = options.usda_parse_options;
  if (options.progress_callback) popts.progress_callback = options.progress_callback;
  if (options.strict_aousd_conformance) {
    popts.strict_aousd_conformance = true;
  }
  popts.num_threads = options.parse_num_threads > 0 ? options.parse_num_threads
                                                    : popts.num_threads;
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
  bool is_usdc = EndsWithNoCase(reader.EntryName(static_cast<size_t>(idx)), ".usdc");
  bool is_usda = EndsWithNoCase(reader.EntryName(static_cast<size_t>(idx)), ".usda");
  if (!is_usdc && !is_usda && size >= 8 &&
      std::memcmp(data, "PXR-USDC", 8) == 0) {
    is_usdc = true;
  }
  if (is_usdc) {
    USDCLoadOptions lopts;
    lopts.crate_options.max_memory = options.max_memory;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.use_mmap = options.usdc_use_mmap;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    return ConvertLoadedUSDC(LoadUSDCFromMemory(data, size, lopts), label, err);
  }
  if (is_usda) {
    LoadOptions lopts;
    lopts.parse_options = MakeUSDAParseOptions(options);
    return ConvertLoadedUSDA(
        LoadUSDAFromString(reinterpret_cast<const char *>(data), size, lopts),
        label, warn, err);
  }

  if (err) *err += "Unsupported USDZ layer entry format: " + label + "\n";
  return nullptr;
}

std::shared_ptr<Layer> LoadLayerFromUSDZ(const std::string &package_file,
                                         const std::string &entry_name,
                                         const LayerLoadOptions &options,
                                         std::string *warn, std::string *err) {
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

  return LoadLayerFromUSDZEntry(reader, package_file, entry_name, options,
                                warn, err);
}

}  // namespace

namespace {

// Stamp every prim in a freshly-loaded layer with the directory its RELATIVE
// asset paths anchor to: the layer's own directory. Composition flattens prims
// from many layers into one, so without this the authoring layer -- and with it
// the only correct anchor for `@../tex/foo.png@` -- is lost. See asset-anchor.hh.
//
// USDZ is deliberately excluded: paths inside a package are package-relative and
// are resolved against the archive, not the filesystem, so they must keep
// anchor 0 (the consumer's existing package handling takes over).
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
  // Package entries resolve inside the archive; leave them at anchor 0.
  if (layer && !AssetResolver::IsPackagePath(resolved_path)) {
    StampAssetAnchor(layer.get(), resolved_path);
  }
  return layer;
}

namespace {

std::shared_ptr<Layer> LoadLayerFromFileUnstamped(
    const std::string &resolved_path, std::string *warn, std::string *err,
    const LayerLoadOptions &options) {
  if (AssetResolver::IsPackagePath(resolved_path)) {
    std::string package_file;
    std::string entry_name;
    if (AssetResolver::ParsePackagePath(resolved_path, &package_file,
                                        &entry_name)) {
      return LoadLayerFromUSDZ(package_file, entry_name, options, warn, err);
    }
  }

  std::string ext = ToLowerExt(resolved_path);

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
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.use_mmap = options.usdc_use_mmap;
    return ConvertLoadedUSDC(LoadUSDCFromFile(resolved_path, lopts),
                             resolved_path, err);
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

Value MtlxValueToNextValue(const mtlx::MtlxValue &v) {
  switch (v.type) {
    case mtlx::MtlxValue::TYPE_BOOL: return Value(v.bool_val);
    case mtlx::MtlxValue::TYPE_INT: return Value(static_cast<int32_t>(v.int_val));
    case mtlx::MtlxValue::TYPE_FLOAT: return Value(v.float_val);
    case mtlx::MtlxValue::TYPE_STRING: return Value::MakeToken(v.string_val);
    case mtlx::MtlxValue::TYPE_FLOAT_VECTOR: {
      const std::vector<float> &f = v.float_vec;
      if (f.size() == 2) return Value::MakeFloat2(f[0], f[1]);
      if (f.size() == 3) return Value::MakeFloat3(f[0], f[1], f[2]);
      if (f.size() == 4) return Value::MakeFloat4(f[0], f[1], f[2], f[3]);
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

void EmitMtlxNodePrim(LayerBuilder &lb, const mtlx::MtlxNode &node,
                      const char *forced_info_id,
                      const std::string &node_prefix = std::string(),
                      const std::string &nodegraph_prefix = std::string()) {
  lb.begin_prim(node.GetName().empty() ? std::string("node") : node.GetName(),
                "Shader");
  const char *info_id = forced_info_id ? forced_info_id
                                       : MtlxShaderInfoId(node.GetCategory());
  lb.add_property("info:id", Value::MakeToken(
      info_id ? std::string(info_id) : node.GetCategory()));
  for (const mtlx::MtlxInputPtr &input : node.GetInputs()) {
    if (!input) continue;
    Value value =
        input->GetType() == "filename" &&
                input->GetValue().type == mtlx::MtlxValue::TYPE_STRING
            ? Value::MakeAssetPath(input->GetValue().string_val)
            : MtlxValueToNextValue(input->GetValue());
    if (!value.is_empty()) {
      const std::string property = "inputs:" + input->GetName();
      lb.add_property(property, std::move(value));
      if (!input->GetColorSpace().empty()) {
        if (PrimSpec *prim = lb.current()) {
          PropMeta &meta = prim->ensure_property_meta(property);
          meta.colorSpace = input->GetColorSpace();
          meta.authored |= PropMeta::kColorSpace;
        }
      }
    }
    {
      std::string target;
      if (!node_prefix.empty() && !input->GetNodeName().empty()) {
        target = node_prefix + "/" + input->GetNodeName();
      } else if (!nodegraph_prefix.empty() &&
                 !input->GetNodeGraph().empty()) {
        target = nodegraph_prefix + "/" + input->GetNodeGraph();
      }
      if (!target.empty()) {
        const std::string &output = input->GetOutput();
        const std::string property = "inputs:" + input->GetName();
        DeclareMtlxConnectionProperty(lb, property, input->GetType());
        lb.add_connection(property,
                          Path(target + (output.empty()
                                             ? ".outputs:out"
                                             : ".outputs:" + output)));
      }
    }
  }
  if (!node.GetType().empty()) {
    lb.add_property("outputs:out", Value::MakeToken(node.GetType()));
  }
  lb.end_prim();
}

mtlx::MtlxNodePtr FindMtlxNode(const mtlx::MtlxDocument &doc,
                               const std::string &name) {
  if (name.empty()) return nullptr;
  mtlx::MtlxNodePtr node = doc.FindNode(name);
  if (node) return node;
  for (const auto &graph : doc.GetNodeGraphs()) {
    if (!graph) continue;
    node = graph->GetNode(name);
    if (node) return node;
  }
  return nullptr;
}

void EmitMtlxNodeGraphPrim(LayerBuilder &lb,
                           const mtlx::MtlxNodeGraph &graph,
                           const std::string &graph_path,
                           const std::string &nodegraph_prefix) {
  lb.begin_prim(graph.GetName(), "NodeGraph");
  for (const auto &node : graph.GetNodes()) {
    if (!node || node->GetName().empty()) continue;
    EmitMtlxNodePrim(lb, *node, nullptr, graph_path, nodegraph_prefix);
  }
  for (const auto &output : graph.GetOutputs()) {
    if (!output || output->GetName().empty() ||
        output->GetNodeName().empty()) {
      continue;
    }
    const std::string output_target =
        graph_path + "/" + output->GetNodeName() +
        (output->GetOutput().empty()
             ? ".outputs:out"
             : ".outputs:" + output->GetOutput());
    const std::string property = "outputs:" + output->GetName();
    DeclareMtlxConnectionProperty(lb, property, output->GetType());
    lb.add_connection(property, Path(output_target));
  }
  lb.end_prim();
}

}  // namespace

std::shared_ptr<Layer> LoadLayerFromMtlxMemory(const std::string &key,
                                               const uint8_t *data,
                                               size_t size, std::string *warn,
                                               std::string *err) {
  mtlx::MtlxDocument doc;
  if (!doc.ParseFromXML(
          std::string(reinterpret_cast<const char *>(data), size))) {
    if (err) {
      *err += "Failed to parse MaterialX layer: " + key +
              (doc.GetError().empty() ? std::string()
                                      : " : " + doc.GetError()) +
              "\n";
    }
    return nullptr;
  }
  if (warn && !doc.GetWarning().empty()) {
    *warn += "MaterialX layer " + key + ": " + doc.GetWarning() + "\n";
  }

  auto layer = std::make_shared<Layer>();
  LayerBuilder lb(*layer);

  lb.begin_prim("MaterialX", "");

  lb.begin_prim("Materials", "");
  for (const mtlx::MtlxMaterialPtr &mat : doc.GetMaterials()) {
    if (!mat || mat->GetName().empty()) continue;
    lb.begin_prim(mat->GetName(), "Material");
    if (PrimSpec *prim = lb.current()) {
      prim->meta().apiSchemas().push_back("MaterialXConfigAPI");
    }
    if (!doc.GetVersion().empty()) {
      lb.add_property("config:mtlx:version", Value(doc.GetVersion()));
    }
    if (!doc.GetColorSpace().empty()) {
      lb.add_property("config:mtlx:colorspace", Value(doc.GetColorSpace()));
    }
    // A USD reference to /MaterialX/Materials/<name> only brings that
    // material subtree into the host layer. Keep the terminal surface shader
    // inside the material as well; the canonical library namespaces below
    // (/MaterialX/Shaders and /MaterialX/NodeGraphs) are useful for flattened
    // layers but are outside the reference target and therefore cannot be
    // relied on by a composed external MaterialX asset.
    const std::string material_prefix =
        "/MaterialX/Materials/" + mat->GetName();
    const mtlx::MtlxNodePtr surface =
        FindMtlxNode(doc, mat->GetSurfaceShader());
    const mtlx::MtlxNodePtr displacement =
        FindMtlxNode(doc, mat->GetDisplacementShader());
    const mtlx::MtlxNodePtr volume =
        FindMtlxNode(doc, mat->GetVolumeShader());
    if (surface || displacement || volume) {
      if (surface) {
        EmitMtlxNodePrim(lb, *surface, nullptr, material_prefix,
                         material_prefix);
      }
      if (displacement && displacement != surface) {
        EmitMtlxNodePrim(lb, *displacement, nullptr, material_prefix,
                         material_prefix);
      }
      if (volume && volume != surface && volume != displacement) {
        EmitMtlxNodePrim(lb, *volume, nullptr, material_prefix,
                         material_prefix);
      }
      // Keep the graph and its output routing inside the referenced Material
      // subtree. This preserves image/normal/utility nodes and lets the next
      // shader evaluator follow the same connections as the source MaterialX
      // document after USD composition.
      for (const auto &graph : doc.GetNodeGraphs()) {
        if (!graph || graph->GetName().empty()) continue;
        const std::string graph_prefix =
            material_prefix + "/" + graph->GetName();
        EmitMtlxNodeGraphPrim(lb, *graph, graph_prefix, material_prefix);
      }
    }
    if (!mat->GetSurfaceShader().empty()) {
      lb.add_relationship(
          "mtlx:surface:source",
          Path(surface ? material_prefix + "/" + mat->GetSurfaceShader()
                       : "/MaterialX/Shaders/" + mat->GetSurfaceShader()));
    }
    if (!mat->GetDisplacementShader().empty()) {
      lb.add_relationship(
          "mtlx:displacement:source",
          Path(displacement
                   ? material_prefix + "/" + mat->GetDisplacementShader()
                   : "/MaterialX/Shaders/" + mat->GetDisplacementShader()));
    }
    if (!mat->GetVolumeShader().empty()) {
      lb.add_relationship(
          "mtlx:volume:source",
          Path(volume ? material_prefix + "/" + mat->GetVolumeShader()
                      : "/MaterialX/Shaders/" + mat->GetVolumeShader()));
    }
    lb.end_prim();
  }
  lb.end_prim();  // Materials

  lb.begin_prim("Shaders", "");
  for (const mtlx::MtlxNodePtr &node : doc.GetNodes()) {
    if (!node || node->GetName().empty()) continue;
    // surfacematerial nodes are represented under /MaterialX/Materials.
    if (node->GetCategory() == "surfacematerial") continue;
    EmitMtlxNodePrim(lb, *node, nullptr, "/MaterialX/Shaders",
                     "/MaterialX/NodeGraphs");
  }
  lb.end_prim();  // Shaders

  if (!doc.GetNodeGraphs().empty()) {
    lb.begin_prim("NodeGraphs", "");
    for (const mtlx::MtlxNodeGraphPtr &graph : doc.GetNodeGraphs()) {
      if (!graph || graph->GetName().empty()) continue;
      const std::string graph_path =
          "/MaterialX/NodeGraphs/" + graph->GetName();
      EmitMtlxNodeGraphPrim(lb, *graph, graph_path,
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
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    return ConvertLoadedUSDC(LoadUSDCFromMemory(data, size, lopts), key, err);
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
  if (options.max_memory > 0 && data.size() > options.max_memory) {
    if (err) {
      *err += "Layer buffer exceeds max_memory for: " + key + "\n";
    }
    return nullptr;
  }

  if (data.size() >= 8 && std::memcmp(data.data(), "PXR-USDC", 8) == 0) {
    USDCLoadOptions lopts;
    lopts.crate_options.max_memory = options.max_memory;
    lopts.crate_options.max_array_elements = options.max_array_elements;
    lopts.crate_options.progress_callback = options.progress_callback;
    lopts.crate_options.lazy_arrays = options.usdc_lazy_arrays;
    lopts.crate_options.strict_aousd_conformance =
        options.strict_aousd_conformance;
    return ConvertLoadedUSDC(LoadUSDCFromMemoryOwned(std::move(data), lopts),
                             key, err);
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

std::shared_ptr<Layer> LayerRegistry::GetOrLoad(AssetResolver &resolver,
                                                const std::string &asset_path,
                                                const std::string &anchor,
                                                std::string *warn,
                                                std::string *err,
                                                const LayerLoadOptions &options) {
  ResolvedAsset resolved_asset = resolver.Resolve(
      asset_path, anchor, !options.strict_aousd_conformance);
  const std::string resolved = resolved_asset.resolved_path;
  if (resolved.empty()) {
    if (err) *err += "Failed to resolve asset path: " + asset_path + "\n";
    return nullptr;
  }
  auto load_resolved = [&](std::string* load_warn,
                           std::string* load_err) -> std::shared_ptr<Layer> {
    if (!AssetResolver::GetIdentifierScheme(resolved).empty() ||
        resolver.HasAssetReader()) {
      std::vector<uint8_t> bytes;
      if (!resolver.ReadAsset(resolved, &bytes, load_err)) return nullptr;
      return LoadLayerFromMemory(resolved, bytes.data(), bytes.size(),
                                 load_warn, load_err, options);
    }
    return LoadLayerFromFile(resolved, load_warn, load_err, options);
  };

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::shared_future<LoadOutcome> wait_fut;
  std::shared_ptr<std::promise<LoadOutcome>> my_promise;
  {
    std::lock_guard<std::mutex> lk(*mu_);
    auto it = by_resolved_.find(resolved);
    if (it != by_resolved_.end()) return it->second;  // already parsed

    auto fit = in_flight_.find(resolved);
    if (fit != in_flight_.end()) {
      wait_fut = fit->second;  // another thread is parsing this path
    } else {
      // Become the loader: publish a future, then parse outside the lock.
      my_promise = std::make_shared<std::promise<LoadOutcome>>();
      in_flight_.emplace(resolved, my_promise->get_future().share());
    }
  }

  if (!my_promise) {
    // Someone else is loading this exact path; wait for them (outside the lock).
    LoadOutcome outcome = wait_fut.get();
    if (warn) *warn += outcome.warn;
    if (err) *err += outcome.err;
    return outcome.layer;
  }

  // Parse WITHOUT holding the lock, so other paths load concurrently.
  LoadOutcome outcome;
  outcome.layer = load_resolved(&outcome.warn, &outcome.err);
  {
    std::lock_guard<std::mutex> lk(*mu_);
    if (outcome.layer) {
      ++parse_count_;
      by_resolved_.emplace(resolved, outcome.layer);
    }
    in_flight_.erase(resolved);  // a failed load is retried by the next caller
  }
  if (warn) *warn += outcome.warn;
  if (err) *err += outcome.err;
  my_promise->set_value(outcome);  // unblock any waiters
  return outcome.layer;
#else
  auto it = by_resolved_.find(resolved);
  if (it != by_resolved_.end()) {
    return it->second;  // Cache hit -- no re-parse.
  }

  std::shared_ptr<Layer> layer = load_resolved(warn, err);
  if (!layer) {
    return nullptr;
  }

  ++parse_count_;
  by_resolved_.emplace(resolved, layer);
  return layer;
#endif
}

void LayerRegistry::Drop(const std::string &resolved_path) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*mu_);
#endif
  by_resolved_.erase(resolved_path);
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
