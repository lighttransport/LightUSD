#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "lightusd.hh"
#include "core/prim-spec.hh"
#include "layer.hh"
#include "pprinter.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "usd-to-json.hh"
#include "usd-dump.hh"
#include "value-pprint.hh"
#include "logger.hh"
#include "crate-dump.hh"
#include "usdc-reader.hh"
#include "usdc-writer.hh"
#include "usda-writer.hh"
#include "usd-validation.hh"

#include "variant-format.hh"
#include "comp-graph-dump.hh"

struct CompositionFeatures {
  bool subLayers{true};
  bool inherits{true};
  bool variantSets{true};
  bool references{true};
  bool payload{true}; // Not 'payloads'
  bool specializes{true};
};

enum class OutputFormat {
  Infer,
  USDA,
  USDC,
  USDZ
};

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); }
  );
  return s;
}

static bool ParseOutputFormat(const std::string &value, OutputFormat *format) {
  if (!format) {
    return false;
  }

  const std::string fmt = str_tolower(value);
  if (fmt == "usda") {
    *format = OutputFormat::USDA;
    return true;
  }
  if (fmt == "usdc") {
    *format = OutputFormat::USDC;
    return true;
  }
  if (fmt == "usdz") {
    *format = OutputFormat::USDZ;
    return true;
  }

  return false;
}

static bool HasProp(const lightusd::PrimSpec &ps, const std::string &name) {
  return ps.props().find(name) != ps.props().end();
}

static bool HasMaterialBinding(const lightusd::PrimSpec &ps) {
  for (const auto &item : ps.props()) {
    if (item.first.rfind("material:binding", 0) == 0 &&
        item.second.is_relationship()) {
      return true;
    }
  }
  return false;
}

static std::string RelationTargetsString(const lightusd::Property &prop) {
  std::stringstream ss;
  const std::vector<lightusd::Path> targets = prop.get_relationTargets();
  for (size_t i = 0; i < targets.size(); i++) {
    if (i > 0) {
      ss << ", ";
    }
    ss << lightusd::to_string(targets[i]);
  }
  return ss.str();
}

static size_t IntArrayValueCount(const lightusd::Property &prop) {
  if (!prop.is_attribute()) {
    return 0;
  }
  const auto view = prop.get_attribute().get_value_view<int32_t>();
  return view.size();
}

static std::string SnipString(std::string s, size_t max_len = 160) {
  if (s.size() <= max_len) {
    return s;
  }
  return s.substr(0, max_len) + "...";
}

static bool StartsWithAny(const std::string &s,
                          const std::initializer_list<const char *> prefixes) {
  for (const char *prefix : prefixes) {
    if (s.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

static bool ContainsAny(const std::string &s,
                        const std::initializer_list<const char *> needles) {
  for (const char *needle : needles) {
    if (s.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static void PrintMeshPrims(const lightusd::Stage &stage) {
  std::vector<std::pair<const lightusd::Prim *, std::string>> pending;
  for (auto it = stage.root_prims().rbegin();
       it != stage.root_prims().rend(); ++it) {
    pending.emplace_back(&*it, "");
  }
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();
    const auto name = current.first->element_name();
    const std::string path = current.second + "/" +
                             std::string(name.data(), name.size());
    if (current.first->type_name() == "Mesh") {
      std::cout << "Prim : " << path << "\n";
    }
    const auto &children = current.first->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      pending.emplace_back(&*it, path);
    }
  }
}

template <typename T>
static size_t AttrArrayCountAs(const lightusd::Attribute &attr) {
  return attr.get_value_view<T>().size();
}

static size_t AttrArrayCount(const lightusd::Attribute &attr) {
  if (!attr.has_value() || attr.has_timesamples() || attr.is_connection()) {
    return 0;
  }
  size_t n = AttrArrayCountAs<int32_t>(attr);
  if (n) return n;
  n = AttrArrayCountAs<uint32_t>(attr);
  if (n) return n;
  n = AttrArrayCountAs<float>(attr);
  if (n) return n;
  n = AttrArrayCountAs<double>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::float2>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::float3>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::point3f>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::point3d>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::normal3f>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::vector3f>(attr);
  if (n) return n;
  n = AttrArrayCountAs<lightusd::value::quatf>(attr);
  if (n) return n;
  return 0;
}

static size_t AttrArrayCountForName(const lightusd::PrimSpec &ps,
                                    const std::string &name) {
  auto it = ps.props().find(name);
  if (it == ps.props().end() || !it->second.is_attribute()) {
    return 0;
  }
  return AttrArrayCount(it->second.get_attribute());
}

static std::string AttrValueSummary(const lightusd::Attribute &attr,
                                    size_t max_len = 160) {
  if (attr.is_connection()) {
    std::stringstream ss;
    ss << "connections=[";
    const auto &paths = attr.connections();
    for (size_t i = 0; i < paths.size(); i++) {
      if (i > 0) {
        ss << ", ";
      }
      ss << lightusd::to_string(paths[i]);
    }
    ss << "]";
    return ss.str();
  }
  if (attr.has_timesamples()) {
    return "timeSamples";
  }
  if (attr.is_blocked()) {
    return "blocked";
  }
  if (!attr.has_value()) {
    return "noValue";
  }
  return SnipString(lightusd::value::pprint_value(attr.get_var().value_raw(), 0, false),
                    max_len);
}

static std::string PropertySummary(const lightusd::Property &prop,
                                   size_t max_len = 160) {
  if (prop.is_relationship()) {
    return "targets=[" + RelationTargetsString(prop) + "]";
  }
  if (prop.is_attribute()) {
    const lightusd::Attribute &attr = prop.get_attribute();
    std::stringstream ss;
    ss << "type=" << attr.type_name();
    ss << " variability="
       << (attr.variability() == lightusd::Variability::Uniform ? "uniform" : "varying");
    const size_t count = AttrArrayCount(attr);
    if (count > 0) {
      ss << " count=" << count;
    }
    if (attr.metas().authored()) {
      ss << " meta=yes";
      if (attr.metas().has_interpolation()) {
        ss << " interpolation=" << attr.metas().get_interpolation().str();
      }
      if (attr.metas().has_unauthoredValuesIndex()) {
        ss << " unauthoredValuesIndex="
           << attr.metas().get_unauthoredValuesIndex();
      }
    }
    ss << " value=" << AttrValueSummary(attr, max_len);
    return ss.str();
  }
  return "empty";
}

static bool PathSelected(const std::string &pattern, const std::string &path) {
  return pattern.empty() || lightusd::GlobMatchPath(pattern, path);
}

static void PrintPropertyLine(const std::string &name,
                              const lightusd::Property &prop,
                              const std::string &indent = "    ") {
  std::cout << indent << name << ": " << PropertySummary(prop) << "\n";
}

static bool PathMatchesOrDescendantMayMatch(const std::string &pattern,
                                            const std::string &path) {
  if (pattern.empty()) {
    return true;
  }
  if (lightusd::GlobMatchPath(pattern, path)) {
    return true;
  }
  if (pattern.size() > path.size() && pattern.compare(0, path.size(), path) == 0 &&
      pattern[path.size()] == '/') {
    return true;
  }
  return false;
}

static bool AnyChildPathMatches(const lightusd::PrimSpec &ps,
                                const std::string &path,
                                const std::string &pattern) {
  if (pattern.empty()) {
    return true;
  }
  for (const auto &child : ps.children()) {
    const std::string child_path = path + "/" + child.name();
    if (PathMatchesOrDescendantMayMatch(pattern, child_path) ||
        AnyChildPathMatches(child, child_path, pattern)) {
      return true;
    }
  }
  return false;
}

static void PrintMeshSubsetReportRec(const lightusd::PrimSpec &ps,
                                     const std::string &path,
                                     const std::string &pattern,
                                     size_t *mesh_count,
                                     size_t *suspicious_count) {
  const bool path_selected =
      PathMatchesOrDescendantMayMatch(pattern, path) ||
      AnyChildPathMatches(ps, path, pattern);

  if (ps.typeName() == "Mesh" && path_selected) {
    (*mesh_count)++;
    std::cout << "\n" << path << "\n";
    size_t subset_count = 0;
    size_t suspicious_here = 0;

    for (const auto &child : ps.children()) {
      const std::string child_path = path + "/" + child.name();
      const bool child_selected = PathMatchesOrDescendantMayMatch(pattern, child_path);
      const bool has_subset_fields =
          HasProp(child, "indices") || HasProp(child, "elementType") ||
          HasProp(child, "familyName");
      const bool has_binding = HasMaterialBinding(child);
      const bool looks_relevant =
          child.typeName() == "GeomSubset" || has_subset_fields || has_binding;
      if (!looks_relevant || (!pattern.empty() && !child_selected &&
                              !PathMatchesOrDescendantMayMatch(pattern, path))) {
        continue;
      }

      subset_count++;
      const bool suspicious =
          child.typeName() != "GeomSubset" && has_binding &&
          (!HasProp(child, "indices") || !HasProp(child, "elementType"));
      if (suspicious) {
        suspicious_here++;
        (*suspicious_count)++;
      }

      std::cout << "  - " << child.name();
      std::cout << " type=" << (child.typeName().empty() ? "<empty>" : child.typeName());
      std::cout << " indices=";
      auto it = child.props().find("indices");
      if (it != child.props().end()) {
        std::cout << IntArrayValueCount(it->second);
      } else {
        std::cout << "missing";
      }
      std::cout << " elementType=" << (HasProp(child, "elementType") ? "yes" : "missing");
      std::cout << " familyName=" << (HasProp(child, "familyName") ? "yes" : "missing");

      for (const auto &prop_item : child.props()) {
        if (prop_item.first.rfind("material:binding", 0) == 0 &&
            prop_item.second.is_relationship()) {
          std::cout << " " << prop_item.first << "=["
                    << RelationTargetsString(prop_item.second) << "]";
        }
      }
      if (suspicious) {
        std::cout << " WARNING=material-bound-non-GeomSubset";
      }
      std::cout << "\n";
    }

    std::cout << "  summary: subset-like children=" << subset_count
              << ", suspicious=" << suspicious_here << "\n";
  }

  for (const auto &child : ps.children()) {
    PrintMeshSubsetReportRec(child, path + "/" + child.name(), pattern,
                             mesh_count, suspicious_count);
  }
}

static void PrintMeshSubsetReport(const lightusd::Layer &layer,
                                  const std::string &pattern) {
  size_t mesh_count = 0;
  size_t suspicious_count = 0;
  std::cout << "# Mesh subset/material binding report\n";
  if (!pattern.empty()) {
    std::cout << "path_filter: " << pattern << "\n";
  }
  for (const auto &item : layer.primspecs()) {
    PrintMeshSubsetReportRec(item.second, "/" + item.first, pattern,
                             &mesh_count, &suspicious_count);
  }
  std::cout << "\nmeshes_reported: " << mesh_count
            << "\nsuspicious_children: " << suspicious_count << "\n";
}

static std::vector<std::pair<std::string, std::string>> MaterialBindings(
    const lightusd::PrimSpec &ps) {
  std::vector<std::pair<std::string, std::string>> bindings;
  for (const auto &item : ps.props()) {
    if (item.first.rfind("material:binding", 0) == 0 &&
        item.second.is_relationship()) {
      bindings.emplace_back(item.first, RelationTargetsString(item.second));
    }
  }
  return bindings;
}

static bool IsMaterialPrim(const lightusd::PrimSpec &ps) {
  return ps.typeName() == "Material" || ps.typeName() == "Shader" ||
         ps.typeName() == "NodeGraph";
}

static bool IsGeomPrim(const lightusd::PrimSpec &ps) {
  return ps.typeName() == "Mesh" || ps.typeName() == "GeomSubset" ||
         ps.typeName() == "BasisCurves" || ps.typeName() == "NurbsCurves" ||
         ps.typeName() == "Points" || ps.typeName() == "PointInstancer" ||
         ps.typeName() == "Cube" || ps.typeName() == "Sphere" ||
         ps.typeName() == "Capsule" || ps.typeName() == "Cone" ||
         ps.typeName() == "Cylinder" || ps.typeName() == "Plane";
}

static bool IsSkelPrim(const lightusd::PrimSpec &ps) {
  return ps.typeName() == "SkelRoot" || ps.typeName() == "Skeleton" ||
         ps.typeName() == "SkelAnimation" || ps.typeName() == "BlendShape";
}

static bool IsTextureLikeProperty(const std::string &name,
                                  const lightusd::Property &prop) {
  if (!prop.is_attribute()) {
    return false;
  }
  const lightusd::Attribute &attr = prop.get_attribute();
  return attr.type_name() == "asset" ||
         ContainsAny(name, {"file", "filename", "texture", "Texture"}) ||
         (attr.has_value() &&
          lightusd::value::pprint_value(attr.get_var().value_raw(), 0, false).find('@') !=
              std::string::npos);
}

static void PrintMaterialReportRec(
    const lightusd::PrimSpec &ps, const std::string &path,
    const std::string &pattern,
    const std::vector<std::pair<std::string, std::string>> &inherited_bindings,
    size_t *prim_count, size_t *binding_count, size_t *texture_count) {
  std::vector<std::pair<std::string, std::string>> current_bindings =
      MaterialBindings(ps);
  const std::vector<std::pair<std::string, std::string>> &effective_bindings =
      current_bindings.empty() ? inherited_bindings : current_bindings;
  const bool selected = PathSelected(pattern, path);

  bool has_texture = false;
  bool has_shader_io = false;
  for (const auto &item : ps.props()) {
    has_texture = has_texture || IsTextureLikeProperty(item.first, item.second);
    has_shader_io =
        has_shader_io || StartsWithAny(item.first, {"inputs:", "outputs:", "info:"});
  }

  const bool should_print =
      selected && (IsMaterialPrim(ps) || !current_bindings.empty() ||
                  (ps.typeName() == "Mesh" && !effective_bindings.empty()) ||
                  has_texture || has_shader_io);

  if (should_print) {
    (*prim_count)++;
    std::cout << "\n" << path << " type="
              << (ps.typeName().empty() ? "<empty>" : ps.typeName()) << "\n";
    if (!current_bindings.empty()) {
      for (const auto &b : current_bindings) {
        (*binding_count)++;
        std::cout << "  binding " << b.first << " -> [" << b.second << "]\n";
      }
    } else if (ps.typeName() == "Mesh" && !effective_bindings.empty()) {
      for (const auto &b : effective_bindings) {
        std::cout << "  inheritedBinding " << b.first << " -> [" << b.second
                  << "]\n";
      }
    }

    for (const auto &item : ps.props()) {
      if (StartsWithAny(item.first, {"info:", "inputs:", "outputs:"}) ||
          IsTextureLikeProperty(item.first, item.second)) {
        if (IsTextureLikeProperty(item.first, item.second)) {
          (*texture_count)++;
        }
        PrintPropertyLine(item.first, item.second);
      }
    }
  }

  for (const auto &child : ps.children()) {
    PrintMaterialReportRec(child, path + "/" + child.name(), pattern,
                           effective_bindings, prim_count, binding_count,
                           texture_count);
  }
}

static void PrintMaterialReport(const lightusd::Layer &layer,
                                const std::string &pattern) {
  size_t prim_count = 0;
  size_t binding_count = 0;
  size_t texture_count = 0;
  std::cout << "# Material/shader/texture report\n";
  if (!pattern.empty()) {
    std::cout << "path_filter: " << pattern << "\n";
  }
  const std::vector<std::pair<std::string, std::string>> no_bindings;
  for (const auto &item : layer.primspecs()) {
    PrintMaterialReportRec(item.second, "/" + item.first, pattern, no_bindings,
                           &prim_count, &binding_count, &texture_count);
  }
  std::cout << "\nprims_reported: " << prim_count
            << "\nbindings_reported: " << binding_count
            << "\ntexture_like_properties: " << texture_count << "\n";
}

static void PrintGeomReportRec(const lightusd::PrimSpec &ps,
                               const std::string &path,
                               const std::string &pattern,
                               size_t *prim_count, size_t *primvar_count) {
  const bool selected = PathSelected(pattern, path);
  const bool has_geom_api_props =
      HasProp(ps, "points") || HasProp(ps, "faceVertexCounts") ||
      HasProp(ps, "faceVertexIndices") || HasProp(ps, "normals") ||
      HasProp(ps, "extent");

  if (selected && (IsGeomPrim(ps) || has_geom_api_props)) {
    (*prim_count)++;
    std::cout << "\n" << path << " type="
              << (ps.typeName().empty() ? "<empty>" : ps.typeName()) << "\n";
    if (HasProp(ps, "points")) {
      std::cout << "  points=" << AttrArrayCountForName(ps, "points") << "\n";
    }
    if (HasProp(ps, "faceVertexCounts")) {
      std::cout << "  faceVertexCounts="
                << AttrArrayCountForName(ps, "faceVertexCounts") << "\n";
    }
    if (HasProp(ps, "faceVertexIndices")) {
      std::cout << "  faceVertexIndices="
                << AttrArrayCountForName(ps, "faceVertexIndices") << "\n";
    }
    if (HasProp(ps, "normals")) {
      std::cout << "  normals=" << AttrArrayCountForName(ps, "normals") << "\n";
    }
    if (HasProp(ps, "extent")) {
      PrintPropertyLine("extent", ps.props().at("extent"), "  ");
    }
    const auto bindings = MaterialBindings(ps);
    for (const auto &b : bindings) {
      std::cout << "  binding " << b.first << " -> [" << b.second << "]\n";
    }

    for (const auto &item : ps.props()) {
      if (StartsWithAny(item.first, {"primvars:", "skel:"}) ||
          item.first.find(":indices") != std::string::npos) {
        (*primvar_count)++;
        PrintPropertyLine(item.first, item.second);
      }
    }
  }

  for (const auto &child : ps.children()) {
    PrintGeomReportRec(child, path + "/" + child.name(), pattern, prim_count,
                       primvar_count);
  }
}

static void PrintGeomReport(const lightusd::Layer &layer,
                            const std::string &pattern) {
  size_t prim_count = 0;
  size_t primvar_count = 0;
  std::cout << "# Geometry/topology/primvar report\n";
  if (!pattern.empty()) {
    std::cout << "path_filter: " << pattern << "\n";
  }
  for (const auto &item : layer.primspecs()) {
    PrintGeomReportRec(item.second, "/" + item.first, pattern, &prim_count,
                       &primvar_count);
  }
  std::cout << "\nprims_reported: " << prim_count
            << "\nprimvar_or_skel_properties: " << primvar_count << "\n";
}

static bool HasSkelProperty(const lightusd::PrimSpec &ps) {
  for (const auto &item : ps.props()) {
    if (StartsWithAny(item.first, {"skel:", "primvars:skel:"}) ||
        ContainsAny(item.first, {"jointIndices", "jointWeights", "blendShape"})) {
      return true;
    }
  }
  return false;
}

static void PrintSkinningReportRec(const lightusd::PrimSpec &ps,
                                   const std::string &path,
                                   const std::string &pattern,
                                   size_t *prim_count,
                                   size_t *skel_prop_count) {
  const bool selected = PathSelected(pattern, path);
  const bool should_print = selected && (IsSkelPrim(ps) || HasSkelProperty(ps));
  if (should_print) {
    (*prim_count)++;
    std::cout << "\n" << path << " type="
              << (ps.typeName().empty() ? "<empty>" : ps.typeName()) << "\n";
    for (const auto &item : ps.props()) {
      if (StartsWithAny(item.first, {"skel:", "primvars:skel:"}) ||
          ContainsAny(item.first, {"jointIndices", "jointWeights", "joints",
                                   "bindTransforms", "restTransforms",
                                   "blendShape", "rotations", "translations",
                                   "scales"})) {
        (*skel_prop_count)++;
        PrintPropertyLine(item.first, item.second);
      }
    }
  }
  for (const auto &child : ps.children()) {
    PrintSkinningReportRec(child, path + "/" + child.name(), pattern, prim_count,
                           skel_prop_count);
  }
}

static void PrintSkinningReport(const lightusd::Layer &layer,
                                const std::string &pattern) {
  size_t prim_count = 0;
  size_t skel_prop_count = 0;
  std::cout << "# Skinning/skeleton report\n";
  if (!pattern.empty()) {
    std::cout << "path_filter: " << pattern << "\n";
  }
  for (const auto &item : layer.primspecs()) {
    PrintSkinningReportRec(item.second, "/" + item.first, pattern, &prim_count,
                           &skel_prop_count);
  }
  std::cout << "\nprims_reported: " << prim_count
            << "\nskel_properties: " << skel_prop_count << "\n";
}

static bool InferOutputFormatFromFilename(const std::string &filename,
                                          OutputFormat *format,
                                          std::string *err) {
  if (!format) {
    if (err) {
      (*err) = "`format` is nullptr.";
    }
    return false;
  }

  const std::string lower = str_tolower(filename);
  if (lightusd::endsWith(lower, ".usda") ||
      lightusd::endsWith(lower, ".usda.zst")) {
    *format = OutputFormat::USDA;
    return true;
  }
  if (lightusd::endsWith(lower, ".usdc") ||
      lightusd::endsWith(lower, ".usdc.zst")) {
    *format = OutputFormat::USDC;
    return true;
  }
  if (lightusd::endsWith(lower, ".usdz")) {
    *format = OutputFormat::USDZ;
    return true;
  }

  if (err) {
    (*err) =
        "Failed to infer output format from filename `" + filename +
        "`. Use .usda, .usdc, .usdz (or .usda.zst/.usdc.zst), or specify "
        "--output-format=usda|usdc|usdz.";
  }
  return false;
}

static std::string format_memory_size(size_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit_index = 0;
  double size = static_cast<double>(bytes);

  while (size >= 1024.0 && unit_index < 4) {
    size /= 1024.0;
    unit_index++;
  }

  std::stringstream ss;
  if (unit_index == 0) {
    ss << static_cast<size_t>(size) << " " << units[unit_index];
  } else {
    ss.precision(2);
    ss << std::fixed << size << " " << units[unit_index];
  }
  return ss.str();
}

// Memory cap for USDA *text* output of a composed stage. USDA serialization of a
// deeply-composed stage (e.g. baked vertex-animation timeSamples) can balloon to
// many GB; emitting it as one std::string then exhausts memory and the process
// is OOM-killed/aborted. When the composed stage's estimated size exceeds this
// cap we serialize to compact USDC in memory instead of the huge USDA text.
// Configurable via env `LUSDCAT_MAX_USDA_MB` (0 = unlimited). Default 0.
static size_t GetMaxUsdaOutputBytes() {
  const char *e = std::getenv("LUSDCAT_MAX_USDA_MB");
  if (!e || !e[0]) {
    return 0;  // unlimited (preserve existing behavior unless opted in)
  }
  char *end = nullptr;
  unsigned long long mb = std::strtoull(e, &end, 10);
  if (end == e) {
    return 0;
  }
  return static_cast<size_t>(mb) * 1024ull * 1024ull;
}

static bool WriteStageToFile(const lightusd::Stage &stage,
                             const std::string &output_path,
                             OutputFormat format,
                             bool compress_float_arrays = false) {
  std::string warn;
  std::string err;

  switch (format) {
    case OutputFormat::USDA:
      if (!lightusd::usda::SaveAsUSDA(output_path, stage, &warn, &err)) {
        std::cerr << "Failed to write USDA file: " << err << "\n";
        return false;
      }
      break;
    case OutputFormat::USDC: {
      lightusd::USDWriteOptions wopts;
      wopts.compress_float_arrays = compress_float_arrays;
      if (!lightusd::usdc::SaveAsUSDCToFile(output_path, stage, &warn, &err,
                                            wopts)) {
        std::cerr << "Failed to write USDC file: " << err << "\n";
        return false;
      }
      break;
    }
    case OutputFormat::USDZ: {
      const std::map<std::string, std::vector<uint8_t>> assets;
      if (!lightusd::SaveAsUSDZToFile(output_path, stage, assets, &warn, &err)) {
        std::cerr << "Failed to write USDZ file: " << err << "\n";
        return false;
      }
      std::cout << "Wrote USDZ to [" << output_path << "]\n";
      break;
    }
    case OutputFormat::Infer:
      std::cerr << "Internal error: output format was not resolved.\n";
      return false;
  }

  if (!warn.empty()) {
    std::cerr << "WARN: " << warn << "\n";
  }

  return true;
}

// Progress bar state
struct ProgressState {
  std::chrono::steady_clock::time_point start_time;
  bool display_started{false};
  float last_progress{0.0f};
  static constexpr int kBarWidth = 40;
  static constexpr double kDelaySeconds = 3.0;  // Don't show progress under 3 seconds
};

static bool progress_callback(float progress, void *userptr) {
  ProgressState *state = static_cast<ProgressState*>(userptr);
  if (!state) {
    return true;
  }

  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - state->start_time).count();

  // Don't show progress if loading takes less than 3 seconds
  if (elapsed < ProgressState::kDelaySeconds) {
    return true;
  }

  // Only update display if progress changed significantly (1% or more)
  if (progress - state->last_progress < 0.01f && progress < 1.0f) {
    return true;
  }
  state->last_progress = progress;

  if (!state->display_started) {
    state->display_started = true;
    std::cerr << "\n";  // Start on new line
  }

  int percent = static_cast<int>(progress * 100.0f);
  int filled = static_cast<int>(progress * ProgressState::kBarWidth);

  std::cerr << "\r[";
  for (int i = 0; i < ProgressState::kBarWidth; ++i) {
    if (i < filled) {
      std::cerr << "=";
    } else if (i == filled) {
      std::cerr << ">";
    } else {
      std::cerr << " ";
    }
  }
  std::cerr << "] " << std::setw(3) << percent << " %" << std::flush;

  if (progress >= 1.0f) {
    std::cerr << "\n";  // Finish with newline
  }

  return true;  // Continue parsing
}

static bool LoadUSDCWithMemoryReport(
    const std::string &filepath, const bool show_progress, lightusd::Stage *stage,
    lightusd::usdc::USDCMemoryUsageReport *memory_report, std::string *warn,
    std::string *err) {
  if (!stage) {
    if (err) {
      (*err) = "`stage` is nullptr.";
    }
    return false;
  }

  // Use mmap when available to avoid 188MB+ heap allocation for file data
  lightusd::io::MMapFileHandle mmap_handle;
  std::vector<uint8_t> data;
  const uint8_t *file_data = nullptr;
  size_t file_size = 0;
  std::string local_err;

  if (lightusd::io::IsMMapSupported()) {
    if (!lightusd::io::MMapFile(filepath, &mmap_handle, /* writable */ false, &local_err)) {
      if (err) {
        (*err) = "Failed to mmap file: " + local_err;
      }
      return false;
    }
    file_data = mmap_handle.addr;
    file_size = static_cast<size_t>(mmap_handle.size);
  } else {
    if (!lightusd::io::ReadWholeFile(&data, &local_err, filepath)) {
      if (err) {
        (*err) = local_err;
      }
      return false;
    }
    file_data = data.data();
    file_size = data.size();
  }

  lightusd::StreamReader sr(file_data, file_size, /* swap_endian */ false);
  lightusd::usdc::USDCReaderConfig config;
  if (const char *lazy_env = std::getenv("LIGHTUSD_USDC_LAZY")) {
    std::string v = str_tolower(std::string(lazy_env));
    if ((v == "0") || (v == "false") || (v == "off") || (v == "no")) {
      config.use_lazy_property_construction = false;
    } else if ((v == "1") || (v == "true") || (v == "on") || (v == "yes")) {
      config.use_lazy_property_construction = true;
    }
  }
  lightusd::usdc::USDCReader reader(&sr, config);

  ProgressState progress_state;
  if (show_progress) {
    progress_state.start_time = std::chrono::steady_clock::now();
    reader.SetProgressCallback(progress_callback, &progress_state);
  }

  if (!reader.ReadUSDC()) {
    if (warn) {
      (*warn) = reader.GetWarning();
    }
    if (err) {
      (*err) = reader.GetError();
    }
    if (mmap_handle.addr) {
      lightusd::io::UnmapFile(mmap_handle, &local_err);
    }
    return false;
  }

  if (!reader.ReconstructStage(stage)) {
    if (warn) {
      (*warn) = reader.GetWarning();
    }
    if (err) {
      (*err) = reader.GetError();
    }
    if (mmap_handle.addr) {
      lightusd::io::UnmapFile(mmap_handle, &local_err);
    }
    return false;
  }

  // Capture memory report AFTER ReconstructStage so peak includes
  // all DecodeFieldSet → UnpackValueRep allocations during property parsing.
  if (memory_report) {
    (*memory_report) = reader.GetMemoryUsageReport();
  }

  if (warn) {
    (*warn) = reader.GetWarning();
  }
  if (err) {
    (*err) = reader.GetError();
  }

  if (mmap_handle.addr) {
    lightusd::io::UnmapFile(mmap_handle, &local_err);
  }

  return true;
}

static void PrintUSDCParserMemoryReport(
    const lightusd::usdc::USDCMemoryUsageReport &report) {
  std::cout << "  USDC parser current usage: "
            << format_memory_size(size_t(report.current_usage_bytes)) << " ("
            << report.current_usage_bytes << " bytes)\n";
  std::cout << "  USDC parser peak usage:    "
            << format_memory_size(size_t(report.peak_usage_bytes)) << " ("
            << report.peak_usage_bytes << " bytes)\n";
  std::cout << "  USDC memory budget:        "
            << format_memory_size(size_t(report.max_budget_bytes)) << " ("
            << report.max_budget_bytes << " bytes)\n";
  std::cout << "  USDC budget remaining:     "
            << format_memory_size(size_t(report.remaining_budget_bytes)) << " ("
            << report.remaining_budget_bytes << " bytes)\n";

  if (report.max_budget_bytes > 0) {
    double ratio =
        100.0 * (double(report.peak_usage_bytes) / double(report.max_budget_bytes));
    std::cout << "  USDC peak/budget ratio:    " << std::fixed
              << std::setprecision(2) << ratio << " %\n";
  }
}

void print_help() {
  std::cout << "Usage: lusdcat [OPTIONS] input.usda/usdc/usdz\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help          Show this help message\n";
  std::cout << "  -f, --flatten       Do composition (load sublayers, references,\n";
  std::cout << "                      payload, evaluate `over`, inherit, variants)\n";
  std::cout << "                      (not fully implemented yet)\n";
  std::cout << "  --composition=LIST  Specify which composition features to enable\n";
  std::cout << "                      (valid when --flatten is supplied).\n";
  std::cout << "                      Comma-separated list of:\n";
  std::cout << "                        l or subLayers, i or inherits,\n";
  std::cout << "                        v or variantSets, r or references,\n";
  std::cout << "                        p or payload, s or specializes\n";
  std::cout << "                      Example: --composition=r,p\n";
  std::cout << "  --extract-variants  Dump variants information to JSON (w.i.p)\n";
  std::cout << "  --relative          Print Path as relative Path (not implemented)\n";
  std::cout << "  -l, --loadOnly      Load/parse USD file only (validate input)\n";
  std::cout << "  -j, --json          Output parsed USD as JSON string\n";
  std::cout << "  -o, --output FILE   Write output to FILE\n";
  std::cout << "  --output-format FMT Output format: usda, usdc, usdz\n";
  std::cout << "                      Default: infer from output filename extension\n";
  std::cout << "  --compress-float-arrays\n";
  std::cout << "                      Enable OpenUSD-compatible tagged compression\n";
  std::cout << "                      for float[]/double[] arrays in USDC output\n";
  std::cout << "                      (default off).\n";
  std::cout << "  --memstat           Print memory usage statistics\n";
  std::cout << "                      (includes USDC parser budget report for .usdc)\n";
  std::cout << "  --relax-asset-cap   Raise composition asset cap to 8 GiB\n";
  std::cout << "                      (opt-in for trusted public large scenes)\n";
  std::cout << "  --max-composition-asset-mb=N\n";
  std::cout << "                      Override per-layer composition asset cap in MiB\n";
  std::cout << "  --no-asset-path-fallback Disable suffix-fallback rebasing of "
               "unresolvable composition asset paths\n";
  std::cout << "  --error-detail      Show full error stack and full source lines\n";
  std::cout << "                      (disable stack snipping and line truncation)\n";
  std::cout << "  --progress          Show ASCII progress bar\n";
  std::cout << "                      (only shown if loading takes > 3 seconds)\n";
  std::cout << "  --loglevel INT      Set logging level:\n";
  std::cout << "                        0=Debug, 1=Warn, 2=Info,\n";
  std::cout << "                        3=Error, 4=Critical, 5=Off\n";
  std::cout << "\n";
  std::cout << "Inspect options (YAML-like tree output):\n";
  std::cout << "  --inspect           Inspect Layer structure (YAML-like output)\n";
  std::cout << "  --inspect-json      Inspect Layer structure (JSON output)\n";
  std::cout << "  --explain /Prim.attr  Explain property sources (with --json/--time)\n";
  std::cout << "  --mesh-subset-report\n";
  std::cout << "                      Report mesh child GeomSubset/material binding state\n";
  std::cout << "                      to find typeless or incomplete subset children\n";
  std::cout << "  --material-report   Report material bindings, shader inputs/outputs,\n";
  std::cout << "                      texture-like asset properties, and inherited mesh binding\n";
  std::cout << "  --geom-report       Report geometry topology counts, primvars, subsets,\n";
  std::cout << "                      material bindings, and attribute metadata hints\n";
  std::cout << "  --skinning-report   Report UsdSkel prims, skel relationships, joint arrays,\n";
  std::cout << "                      blend shapes, and skinning primvars\n";
  std::cout << "  --value=MODE        Value printing mode:\n";
  std::cout << "                        none = schema only, no values\n";
  std::cout << "                        snip = first N items (default)\n";
  std::cout << "                        full = all values\n";
  std::cout << "  --snip=N            Show first N items in snip mode (default: 8)\n";
  std::cout << "  --path=PATTERN      Filter prims by path glob pattern\n";
  std::cout << "                      (* = any chars, ** = any path segments)\n";
  std::cout << "  --attr=PATTERN      Filter attributes by name glob pattern\n";
  std::cout << "  --time=T            Query TimeSamples at time T\n";
  std::cout << "  --time=S:E          Query TimeSamples in range [S, E]\n";
  std::cout << "\n";
  std::cout << "Low-level USDC dump options:\n";
  std::cout << "  --dumpcrate         Dump low-level USDC Crate structure (YAML)\n";
  std::cout << "                      Only works with .usdc files\n";
  std::cout << "  --dumpcrate-path=TEXT   Only show crate paths/specs containing TEXT\n";
  std::cout << "  --dumpcrate-token=TEXT  Only show crate tokens/fields containing TEXT\n";
  std::cout << "  --dumpcrate-limit=N     Limit crate path/spec/token/field output\n";
  std::cout << "\n";
  std::cout << "MaterialX validation options:\n";
  std::cout << "  --strict-mtlx-check Enable strict MaterialX validation\n";
  std::cout << "                      (validates info:id, index bounds, etc.)\n";
  std::cout << "  --validate         Validate against AOUSD Core semantic rules\n";
  std::cout << "                      (core schemas/metadata; binary inputs add Crate/USDZ checks)\n";
  std::cout << "  --validate-all     Validate with all rule groups (core + geom + shade + lux + physics + crate)\n";
  std::cout << "                      (adds geom/shade/lux/physics/crate checks; warning-heavy)\n";
  std::cout << "\n";
  std::cout << "Composition graph dump options:\n";
  std::cout << "  --dump-comp-graph[=FMT]  Dump composition graph\n";
  std::cout << "                           FMT: yaml (default), json, dot\n";
  std::cout << "  --comp-graph-recursive   Follow external references recursively\n";
  std::cout << "  --comp-graph-no-payload  Skip payload arcs (payload off mode)\n";
  std::cout << "  Combined with -l: parse-only mode (validate all files)\n";
  std::cout << "  Combined with --memstat: per-file memory report\n";
}

// Address/thread sanitizers reserve tens of TB of virtual address space at
// startup; an RLIMIT_AS cap makes the sanitizer runtime abort with cryptic
// "out of memory: failed to allocate ... InternalMmapVector" errors before
// main() even runs. Skip the cap in sanitized builds. See doc/sanitizers.md.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define LUSDCAT_NO_AS_LIMIT 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define LUSDCAT_NO_AS_LIMIT 1
#endif
#endif

int ExplainProperty(int argc, char** argv);
int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--explain") return ExplainProperty(argc, argv);
  // Set 32GB virtual memory limit to prevent OOM / memory thrashing
#if !defined(_WIN32) && !defined(LUSDCAT_NO_AS_LIMIT)
  {
    struct rlimit mem_limit;
    mem_limit.rlim_cur = static_cast<rlim_t>(32) * 1024 * 1024 * 1024;  // 32 GB
    mem_limit.rlim_max = static_cast<rlim_t>(32) * 1024 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &mem_limit);
  }
#endif

  // Enable DCOUT output if LIGHTUSD_ENABLE_DCOUT environment variable is set
  const char* enable_dcout_env = std::getenv("LIGHTUSD_ENABLE_DCOUT");
  if (enable_dcout_env != nullptr && std::strlen(enable_dcout_env) > 0) {
    // Any non-empty value enables DCOUT
    lightusd::g_enable_dcout_output = true;
  }

  if (argc < 2) {
    print_help();
    return EXIT_FAILURE;
  }

  bool has_flatten{false};
  bool has_relative{false};
  bool has_extract_variants{false};
  bool load_only{false};
  bool preserve_order{false};
  bool openusd_compat{false};
  std::string variant_format = "yaml";  // Default format: yaml
  bool json_output{false};
  bool memstat{false};
  bool error_detail{false};
  bool show_progress{false};
  bool asset_path_fallback{true};
  bool compress_float_arrays{false};
  size_t max_composition_asset_mb{0};
  OutputFormat output_format{OutputFormat::Infer};

  // Inspect options
  bool do_inspect{false};
  bool do_mesh_subset_report{false};
  bool do_material_report{false};
  bool do_geom_report{false};
  bool do_skinning_report{false};
  lightusd::InspectOptions inspect_opts;

  // Dumpcrate option
  bool do_dumpcrate{false};
  lightusd::crate::DumpOptions dump_opts;
  dump_opts.format = lightusd::crate::OutputFormat::YAML;

  // MaterialX validation
  bool strict_mtlx_check{false};
  bool validate_against_core{false};
  bool validate_all_groups{false};

  // Composition graph dump
  bool do_dump_comp_graph{false};
  std::string comp_graph_format = "yaml";
  bool comp_graph_recursive{false};
  bool comp_graph_no_payload{false};

  constexpr int kMaxIteration = 128;

  std::string filepath;
  std::string output_filepath;

  int input_index = -1;
  CompositionFeatures comp_features;

  for (size_t i = 1; i < argc; i++) {
    std::string arg = argv[i];
    bool arg_matched = false;
    // NOTE: this ~40-branch else-if chain on CLI flag string `arg` was
    // converted to standalone ifs -- same MSVC C1061 ("blocks nested too
    // deeply") risk class already fixed for the same reason elsewhere in
    // this codebase (crate-writer-values.cc, stage-converter.cc,
    // lusdr_args.cc). OPT_MATCH folds the "did anything match yet" guard
    // into the condition itself so the final positional-arg fallback below
    // only fires when no flag matched -- exactly the original else-if
    // chain's semantics -- without needing to touch every branch body.
#define OPT_MATCH(cond) if (!arg_matched && (cond) && (arg_matched = true))
    OPT_MATCH((arg.compare("-h") == 0) || (arg.compare("--help") ==0)) {
      print_help();
      return EXIT_FAILURE;
    }
    OPT_MATCH((arg.compare("-f") == 0) || (arg.compare("--flatten") == 0)) {
      has_flatten = true;
    }
    OPT_MATCH(arg.compare("--relative") == 0) {
      has_relative = true;
    }
    OPT_MATCH((arg.compare("--preserve-order") == 0) ||
               (arg.compare("--usd-order") == 0)) {
      // Opt-in: emit prim children in authored (USD) order recovered from the
      // crate's primChildren field instead of the default lexicographical order
      // (properties stay alphabetical, matching usdcat). Must be set BEFORE
      // loading so the USDC reader records the order metadata.
      preserve_order = true;
      lightusd::pprint::SetPreserveAuthoredOrder(true);
    }
    OPT_MATCH(arg.compare("--openusd-compat") == 0) {
      // Aggregate opt-in: emit output as close to OpenUSD `usdcat` as lightusd
      // can -- authored child order + alphabetical properties (Layer output),
      // OpenUSD float notation, and OpenUSD USDA text layout (metadata paren on
      // the `def` line, plain blank separators).
      preserve_order = true;
      openusd_compat = true;
      lightusd::pprint::SetPreserveAuthoredOrder(true);
      lightusd::SetUSDFloatFormat(true);
      lightusd::pprint::SetUSDTextFormat(true);
      lightusd::SetNormalizeAssetPathOnFlatten(true);
    }
    OPT_MATCH((arg.compare("-l") == 0) || (arg.compare("--loadOnly") == 0)) {
      load_only = true;
    }
    OPT_MATCH((arg.compare("-j") == 0) || (arg.compare("--json") == 0)) {
      json_output = true;
    }
    OPT_MATCH((arg.compare("-o") == 0) || (arg.compare("--output") == 0)) {
      if (i + 1 >= argc) {
        std::cerr << "-o/--output requires a filename argument\n";
        return EXIT_FAILURE;
      }
      i++; // Move to next argument
      output_filepath = argv[i];
    }
    OPT_MATCH(lightusd::startsWith(arg, "--output-format=")) {
      std::string fmt = lightusd::removePrefix(arg, "--output-format=");
      if (fmt.empty()) {
        std::cerr << "No format specified to --output-format.\n";
        return EXIT_FAILURE;
      }
      if (!ParseOutputFormat(fmt, &output_format)) {
        std::cerr << "Invalid output format: " << fmt
                  << ". Must be 'usda', 'usdc', or 'usdz'.\n";
        return EXIT_FAILURE;
      }
    }
    OPT_MATCH(arg.compare("--compress-float-arrays") == 0) {
      compress_float_arrays = true;
    }
    OPT_MATCH(arg.compare("--extract-variants") == 0) {
      has_extract_variants = true;
    }
    OPT_MATCH(lightusd::startsWith(arg, "--variant-format=")) {
      std::string fmt = lightusd::removePrefix(arg, "--variant-format=");
      if (fmt.empty()) {
        std::cerr << "No format specified to --variant-format.\n";
        exit(-1);
      }
      std::string fmt_lower = str_tolower(fmt);
      if (fmt_lower == "yaml" || fmt_lower == "json") {
        variant_format = fmt_lower;
      } else {
        std::cerr << "Invalid variant format: " << fmt << ". Must be 'yaml' or 'json'.\n";
        exit(-1);
      }
    }
    OPT_MATCH(arg.compare("--memstat") == 0) {
      memstat = true;
    }
    OPT_MATCH(arg.compare("--relax-asset-cap") == 0) {
      max_composition_asset_mb = 8192;
    }
    OPT_MATCH(lightusd::startsWith(arg, "--max-composition-asset-mb=")) {
      std::string mb_str =
          lightusd::removePrefix(arg, "--max-composition-asset-mb=");
      if (mb_str.empty()) {
        std::cerr << "--max-composition-asset-mb requires a value.\n";
        return EXIT_FAILURE;
      }
      char *end = nullptr;
      unsigned long long mb = std::strtoull(mb_str.c_str(), &end, 10);
      if ((end == mb_str.c_str()) || (end && *end != '\0')) {
        std::cerr << "Invalid --max-composition-asset-mb value: "
                  << mb_str << "\n";
        return EXIT_FAILURE;
      }
      max_composition_asset_mb = static_cast<size_t>(mb);
    }
    OPT_MATCH(arg.compare("--no-asset-path-fallback") == 0) {
      asset_path_fallback = false;
    }
    OPT_MATCH(arg.compare("--error-detail") == 0) {
      error_detail = true;
    }
    OPT_MATCH(arg.compare("--progress") == 0) {
      show_progress = true;
    }
    OPT_MATCH(arg.compare("--dumpcrate") == 0) {
      do_dumpcrate = true;
    }
    OPT_MATCH(lightusd::startsWith(arg, "--dumpcrate-path=")) {
      dump_opts.path_filter = lightusd::removePrefix(arg, "--dumpcrate-path=");
    }
    OPT_MATCH(lightusd::startsWith(arg, "--dumpcrate-token=")) {
      dump_opts.token_filter = lightusd::removePrefix(arg, "--dumpcrate-token=");
    }
    OPT_MATCH(lightusd::startsWith(arg, "--dumpcrate-limit=")) {
      std::string limit_str = lightusd::removePrefix(arg, "--dumpcrate-limit=");
      nonstd::optional<int> limit_val = lightusd::atoi(limit_str);
      if (!limit_val.has_value() || limit_val.value() < 1) {
        std::cerr << "Invalid dumpcrate limit: " << limit_str << "\n";
        return EXIT_FAILURE;
      }
      dump_opts.max_tokens = limit_val.value();
      dump_opts.max_fields = limit_val.value();
      dump_opts.max_fieldsets = limit_val.value();
      dump_opts.max_paths = limit_val.value();
      dump_opts.max_specs = limit_val.value();
    }
    OPT_MATCH(arg.compare("--strict-mtlx-check") == 0) {
      strict_mtlx_check = true;
    }
    OPT_MATCH(arg.compare("--validate") == 0) {
      validate_against_core = true;
    }
    OPT_MATCH(arg.compare("--validate-all") == 0) {
      validate_against_core = true;
      validate_all_groups = true;
    }
    OPT_MATCH(lightusd::startsWith(arg, "--dump-comp-graph")) {
      do_dump_comp_graph = true;
      std::string rest = arg.substr(strlen("--dump-comp-graph"));
      if (rest.empty() || rest == "=yaml") {
        comp_graph_format = "yaml";
      } else if (rest == "=json") {
        comp_graph_format = "json";
      } else if (rest == "=dot") {
        comp_graph_format = "dot";
      } else {
        std::cerr << "Invalid format for --dump-comp-graph. Use: json, yaml, or dot\n";
        return EXIT_FAILURE;
      }
    }
    OPT_MATCH(arg.compare("--comp-graph-recursive") == 0) {
      comp_graph_recursive = true;
    }
    OPT_MATCH(arg.compare("--comp-graph-no-payload") == 0) {
      comp_graph_no_payload = true;
    }
    OPT_MATCH(arg.compare("--inspect") == 0) {
      do_inspect = true;
      inspect_opts.format = lightusd::InspectOutputFormat::Yaml;
    }
    OPT_MATCH(arg.compare("--inspect-json") == 0) {
      do_inspect = true;
      inspect_opts.format = lightusd::InspectOutputFormat::Json;
    }
    OPT_MATCH(arg.compare("--mesh-subset-report") == 0) {
      do_mesh_subset_report = true;
    }
    OPT_MATCH(arg.compare("--material-report") == 0) {
      do_material_report = true;
    }
    OPT_MATCH(arg.compare("--geom-report") == 0) {
      do_geom_report = true;
    }
    OPT_MATCH(arg.compare("--skinning-report") == 0) {
      do_skinning_report = true;
    }
    OPT_MATCH(lightusd::startsWith(arg, "--value=")) {
      std::string value_str = lightusd::removePrefix(arg, "--value=");
      if (value_str == "none") {
        inspect_opts.value_mode = lightusd::InspectValueMode::NoValue;
      } else if (value_str == "snip") {
        inspect_opts.value_mode = lightusd::InspectValueMode::Snip;
      } else if (value_str == "full") {
        inspect_opts.value_mode = lightusd::InspectValueMode::Full;
      } else {
        std::cerr << "Invalid value mode: " << value_str
                  << ". Use: none, snip, or full\n";
        return EXIT_FAILURE;
      }
    }
    OPT_MATCH(lightusd::startsWith(arg, "--snip=")) {
      std::string snip_str = lightusd::removePrefix(arg, "--snip=");
      nonstd::optional<int> snip_val = lightusd::atoi(snip_str);
      if (!snip_val.has_value()) {
        std::cerr << "Invalid snip value: " << snip_str << "\n";
        return EXIT_FAILURE;
      }
      if (snip_val.value() < 1) {
        std::cerr << "Invalid snip value: " << snip_val.value()
                  << ". Must be >= 1\n";
        return EXIT_FAILURE;
      }
      inspect_opts.snip_count = static_cast<size_t>(snip_val.value());
    }
    OPT_MATCH(lightusd::startsWith(arg, "--path=")) {
      inspect_opts.prim_path_pattern = lightusd::removePrefix(arg, "--path=");
    }
    OPT_MATCH(lightusd::startsWith(arg, "--attr=")) {
      inspect_opts.attr_pattern = lightusd::removePrefix(arg, "--attr=");
    }
    OPT_MATCH(lightusd::startsWith(arg, "--time=")) {
      std::string time_str = lightusd::removePrefix(arg, "--time=");
      inspect_opts.has_time_query = true;
      // Check for range format "start:end"
      size_t colon_pos = time_str.find(':');
      if (colon_pos != std::string::npos) {
        std::string start_str = time_str.substr(0, colon_pos);
        std::string end_str = time_str.substr(colon_pos + 1);
        nonstd::optional<double> t_start = lightusd::atod(start_str);
        nonstd::optional<double> t_end = lightusd::atod(end_str);
        if (!t_start.has_value() || !t_end.has_value()) {
          std::cerr << "Invalid time range: " << time_str << "\n";
          return EXIT_FAILURE;
        }
        inspect_opts.time_start = t_start.value();
        inspect_opts.time_end = t_end.value();
      } else {
        // Single time value
        nonstd::optional<double> t = lightusd::atod(time_str);
        if (!t.has_value()) {
          std::cerr << "Invalid time value: " << time_str << "\n";
          return EXIT_FAILURE;
        }
        inspect_opts.time_start = t.value();
        inspect_opts.time_end = t.value();
      }
    }
    OPT_MATCH(arg.compare("--loglevel") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "--loglevel requires an integer argument\n";
        return EXIT_FAILURE;
      }
      i++; // Move to next argument
      {
        nonstd::optional<int> log_level = lightusd::atoi(argv[i]);
        if (!log_level.has_value()) {
          std::cerr << "Invalid log level argument: " << argv[i] << ". Must be an integer.\n";
          return EXIT_FAILURE;
        }
        int ll = log_level.value();
        if (ll < 0 || ll > 5) {
          std::cerr << "Invalid log level: " << ll << ". Must be between 0 and 5.\n";
          return EXIT_FAILURE;
        }
        lightusd::logging::Logger::getInstance().setLogLevel(
            static_cast<lightusd::logging::LogLevel>(ll));
      }
    }
    OPT_MATCH(lightusd::startsWith(arg, "--composition=")) {
      std::string value_str = lightusd::removePrefix(arg, "--composition=");
      if (value_str.empty()) {
        std::cerr << "No values specified to --composition.\n";
        exit(-1);
      }

      std::vector<std::string> items = lightusd::split(value_str, ",");
      comp_features.subLayers = false;
      comp_features.inherits = false;
      comp_features.variantSets = false;
      comp_features.references = false;
      comp_features.payload = false;
      comp_features.specializes = false;

      for (const auto &item : items) {
        if ((item == "l") || (item == "subLayers")) {
          comp_features.subLayers = true;
        } else if ((item == "i") || (item == "inherits")) {
          comp_features.inherits = true;
        } else if ((item == "v") || (item == "variantSets")) {
          comp_features.variantSets = true;
        } else if ((item == "r") || (item == "references")) {
          comp_features.references = true;
        } else if ((item == "p") || (item == "payload")) {
          comp_features.payload = true;
        } else if ((item == "s") || (item == "specializes")) {
          comp_features.specializes = true;
        } else {
          std::cerr << "Invalid string for --composition : " << item << "\n";
          exit(-1);
        }
      }

    }
#undef OPT_MATCH
    if (!arg_matched) {
      filepath = arg;
      input_index = i;
    }
  }

  if (filepath.empty() || (input_index < 0)) {
    std::cout << "Input USD filename missing.\n";
    return EXIT_FAILURE;
  }

  std::string warn;
  std::string err;

  std::string ext = str_tolower(GetFileExtension(filepath));
  const bool has_output_file = !output_filepath.empty();
  const bool suppress_usd_text_output = has_output_file;
  std::string base_dir;
  base_dir = lightusd::io::GetBaseDir(filepath);

  if ((output_format != OutputFormat::Infer) && !has_output_file) {
    std::cerr << "--output-format requires -o/--output.\n";
    return EXIT_FAILURE;
  }

  if (has_output_file && (output_format == OutputFormat::Infer)) {
    if (!InferOutputFormatFromFilename(output_filepath, &output_format, &err)) {
      std::cerr << err << "\n";
      return EXIT_FAILURE;
    }
  }

  if (validate_against_core) {
    if (has_flatten || do_dumpcrate || do_dump_comp_graph || do_inspect ||
        json_output || has_extract_variants || !output_filepath.empty()) {
      std::cerr
          << "--validate cannot be combined with other output/transform modes\n";
      return EXIT_FAILURE;
    }

    lightusd::USDLoadOptions options;
    options.error_detail = error_detail;

    lightusd::ValidationOptions validation_options;
    if (validate_all_groups) {
      validation_options = lightusd::MakeValidateAllOptions();
    }

    lightusd::USDValidationResult validation;
    const bool ret = lightusd::ValidateUSDFileAgainstAOUSDCore(
        filepath, validation_options, options, &validation, &warn, &err);
    if (!warn.empty()) {
      std::cerr << "WARN: " << warn << "\n";
    }

    if (!ret) {
      std::cerr << "Failed to load USD file as Layer: " << filepath << "\n";
      if (!err.empty()) {
        std::cerr << err << "\n";
      }
      return EXIT_FAILURE;
    }

    std::cout << lightusd::FormatValidationResult(validation);
    return validation.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // Handle --dumpcrate mode (low-level USDC crate dump)
  if (do_dumpcrate) {
    if (ext != "usdc") {
      std::cerr << "Error: --dumpcrate only works with .usdc files\n";
      std::cerr << "  Input file: " << filepath << "\n";
      std::cerr << "  Extension: ." << ext << "\n";
      return EXIT_FAILURE;
    }

    if (!lightusd::crate::DumpCrate(filepath, dump_opts, &err)) {
      std::cerr << "Failed to dump crate: " << err << "\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  }

  // Handle focused Layer report modes
  if (do_mesh_subset_report || do_material_report || do_geom_report ||
      do_skinning_report) {
    lightusd::Layer layer;
    bool ret = lightusd::LoadLayerFromFile(filepath, &layer, &warn, &err);

    if (!warn.empty()) {
      std::cerr << "WARN: " << warn << "\n";
    }

    if (!ret) {
      std::cerr << "Failed to load USD file as Layer: " << filepath << "\n";
      if (!err.empty()) {
        std::cerr << err << "\n";
      }
      return EXIT_FAILURE;
    }

    bool need_separator = false;
    auto separator = [&]() {
      if (need_separator) {
        std::cout << "\n";
      }
      need_separator = true;
    };
    if (do_mesh_subset_report) {
      separator();
      PrintMeshSubsetReport(layer, inspect_opts.prim_path_pattern);
    }
    if (do_material_report) {
      separator();
      PrintMaterialReport(layer, inspect_opts.prim_path_pattern);
    }
    if (do_geom_report) {
      separator();
      PrintGeomReport(layer, inspect_opts.prim_path_pattern);
    }
    if (do_skinning_report) {
      separator();
      PrintSkinningReport(layer, inspect_opts.prim_path_pattern);
    }
    return EXIT_SUCCESS;
  }

  // Handle --dump-comp-graph mode
  if (do_dump_comp_graph) {
    comp_graph_dump::ExtractOptions opts;
    opts.skip_payloads = comp_graph_no_payload;
    opts.parse_only = load_only;
    opts.track_memory = memstat;

    comp_graph_dump::CompGraphDump graph;

    if (comp_graph_recursive) {
      std::string rec_warn, rec_err;
      if (!comp_graph_dump::ExtractCompGraphRecursive(filepath, &graph, opts,
                                                      &rec_warn, &rec_err)) {
        std::cerr << "Failed to extract composition graph: " << rec_err << "\n";
        return EXIT_FAILURE;
      }
      if (!rec_warn.empty()) {
        std::cerr << rec_warn;
      }
    } else {
      // Single file mode
      lightusd::Layer layer;
      bool loaded = lightusd::LoadLayerFromFile(filepath, &layer, &warn, &err);

      if (!warn.empty()) {
        std::cerr << "WARN: " << warn << "\n";
      }

      if (load_only) {
        // Parse-only single file: use recursive with depth=1
        comp_graph_dump::ExtractOptions single_opts = opts;
        comp_graph_dump::ExtractCompGraphRecursive(filepath, &graph, single_opts,
                                                    &warn, &err);
      } else {
        if (!loaded) {
          std::cerr << "Failed to load USD file as Layer: " << filepath << "\n";
          if (!err.empty()) {
            std::cerr << err << "\n";
          }
          return EXIT_FAILURE;
        }

        if (!comp_graph_dump::ExtractCompGraph(layer, filepath, &graph, &err)) {
          std::cerr << "Failed to extract composition graph: " << err << "\n";
          return EXIT_FAILURE;
        }

        // Apply memory tracking to the single node
        if (memstat && !graph.nodes.empty()) {
          graph.nodes[0].memory_usage =
              static_cast<int64_t>(layer.estimate_memory_usage());
        }
      }
    }

    graph.ComputeSizeSummary();

    if (comp_graph_format == "json") {
      std::cout << comp_graph_dump::CompGraphToJSON(graph);
    } else if (comp_graph_format == "yaml") {
      std::cout << comp_graph_dump::CompGraphToYAML(graph);
    } else if (comp_graph_format == "dot") {
      std::cout << comp_graph_dump::CompGraphToDOT(graph);
    }

    return EXIT_SUCCESS;
  }

  // Handle --inspect mode
  if (do_inspect) {
    // Load as Layer for inspection
    lightusd::Layer layer;
    bool ret = lightusd::LoadLayerFromFile(filepath, &layer, &warn, &err);

    if (!warn.empty()) {
      std::cerr << "WARN: " << warn << "\n";
    }

    if (!ret) {
      std::cerr << "Failed to load USD file as Layer: " << filepath << "\n";
      if (!err.empty()) {
        std::cerr << err << "\n";
      }
      return EXIT_FAILURE;
    }

    // Output inspection result
    std::string output = lightusd::InspectLayer(layer, inspect_opts);
    std::cout << output;

    return EXIT_SUCCESS;
  }

  if (has_flatten) {

    if (load_only) {
      std::cerr << "--flatten and --loadOnly cannot be specified at a time\n";
      return EXIT_FAILURE;
    }

    // TODO: flatten for USDZ
    if (lightusd::IsUSDZ(filepath)) {

      std::cout << "--flatten is ignored for USDZ at the moment.\n";

      lightusd::Stage stage;
      lightusd::USDLoadOptions usdz_options;

      // MaterialX validation
      usdz_options.strict_mtlx_check = strict_mtlx_check;
      usdz_options.error_detail = error_detail;

      // Set up progress callback if requested
      ProgressState usdz_progress_state;
      if (show_progress) {
        usdz_progress_state.start_time = std::chrono::steady_clock::now();
        usdz_options.progress_callback = progress_callback;
        usdz_options.progress_userptr = &usdz_progress_state;
      }

      bool ret = lightusd::LoadUSDZFromFile(filepath, &stage, &warn, &err, usdz_options);
      if (!warn.empty()) {
        std::cerr << "WARN : " << warn << "\n";
      }
      if (!err.empty()) {
        std::cerr << "ERR : " << err << "\n";
        //return EXIT_FAILURE;
      }

      if (!ret) {
        std::cerr << "Failed to load USDZ file: " << filepath << "\n";
        return EXIT_FAILURE;
      }

      if (memstat) {
        auto detail = stage.estimate_memory_usage_detail();
        std::cout << "# Memory Statistics (Stage from USDZ)\n";
        std::cout << "  Allocated (capacity): " << format_memory_size(detail.allocated_bytes)
                  << " (" << detail.allocated_bytes << " bytes)\n";
        std::cout << "  Actual (in use):      " << format_memory_size(detail.actual_bytes)
                  << " (" << detail.actual_bytes << " bytes)\n";
        if (detail.allocated_bytes > 0) {
          double efficiency = 100.0 * (double(detail.actual_bytes) / double(detail.allocated_bytes));
          std::cout << "  Efficiency:           " << std::fixed << std::setprecision(1)
                    << efficiency << " %\n";
        }
        std::cout << "\n";
      }

      if (json_output) {
#if defined(LIGHTUSD_WITH_JSON)
        auto json_result = lightusd::ToJSON(stage);
        if (json_result) {
          std::cout << json_result.value() << "\n";
        } else {
          std::cerr << "Failed to convert USDZ stage to JSON: " << json_result.error() << "\n";
          return EXIT_FAILURE;
        }
      } else if (!suppress_usd_text_output) {
        std::cout << to_string(stage) << "\n";
#else
      std::cerr << "JSON output is not supported in this build\n";
      return EXIT_FAILURE;
#endif
      }

      if (has_output_file) {
        if (!WriteStageToFile(stage, output_filepath, output_format,
                            compress_float_arrays)) {
          return EXIT_FAILURE;
        }
      }

      return EXIT_SUCCESS;
    }

    // Coarse flatten phase timing (LIGHTUSD_CRATE_PROFILE=1): stderr marks at
    // each pipeline boundary, complementing the crate-writer's Finalize
    // profiler. Starts BEFORE the root-layer load so nothing is unaccounted.
    const bool profile_phases = (std::getenv("LIGHTUSD_CRATE_PROFILE") != nullptr);
    auto phase_t0 = std::chrono::steady_clock::now();
    auto phase_mark = [&](const char* name) {
      if (!profile_phases) return;
      const auto now = std::chrono::steady_clock::now();
      fprintf(stderr, "[lusdcat profile] %s: %.1fms\n", name,
              std::chrono::duration<double, std::milli>(now - phase_t0).count());
      phase_t0 = now;
    };

    lightusd::Layer root_layer;
    bool ret = lightusd::LoadLayerFromFile(filepath, &root_layer, &warn, &err);
    if (warn.size()) {
      std::cerr << "WARN: " << warn << "\n"; warn.clear();
    }

    if (!ret) {
      std::cerr << "Failed to read USD data as Layer: \n";
      std::cerr << err << "\n";
      return -1;
    }

    if (memstat) {
      size_t layer_mem = root_layer.estimate_memory_usage();
      std::cout << "# Memory Statistics (Layer)\n";
      std::cout << "  Layer memory usage: " << format_memory_size(layer_mem)
                << " (" << layer_mem << " bytes)\n\n";
    }

    if (!suppress_usd_text_output) {
      std::cout << "# input\n";
      std::cout << root_layer << "\n";
    }

    phase_mark("load-root-layer");

    lightusd::Stage stage;
    stage.metas() = root_layer.metas();

    std::string warn;

    lightusd::AssetResolutionResolver resolver;
    resolver.set_current_working_path(base_dir);
    resolver.set_search_paths({base_dir});
    resolver.set_enable_suffix_fallback(asset_path_fallback);

    //
    // LIVRPS strength ordering
    // - [x] Local(subLayers)
    // - [x] Inherits
    // - [x] VariantSets
    // - [x] References
    // - [x] Payload
    // - [ ] Specializes
    //

    lightusd::Layer src_layer = root_layer;

    // lusdcat resolves assets against the local filesystem (the input file's
    // directory), where USD's parent-relative references (e.g.
    // `@../common/foo.usd@`) are legitimate and ubiquitous — OpenUSD resolves
    // them too. Allow '..' in composition asset paths.
    lightusd::SublayersCompositionOptions sublayer_opts;
    sublayer_opts.allow_parent_relative_paths = true;
    lightusd::ReferencesCompositionOptions reference_opts;
    reference_opts.allow_parent_relative_paths = true;
    lightusd::PayloadCompositionOptions payload_opts;
    payload_opts.allow_parent_relative_paths = true;
    if (max_composition_asset_mb > 0) {
      const size_t max_composition_asset_bytes =
          max_composition_asset_mb * 1024ull * 1024ull;
      sublayer_opts.max_asset_bytes = max_composition_asset_bytes;
      reference_opts.max_asset_bytes = max_composition_asset_bytes;
      payload_opts.max_asset_bytes = max_composition_asset_bytes;
    }

    // Parse each referenced file once across the whole fixed-point loop; all
    // arcs to the same file share one copy of the heavy attribute data (COW).
    std::map<std::string, lightusd::Layer> layer_cache;
    reference_opts.layer_cache = &layer_cache;
    payload_opts.layer_cache = &layer_cache;

    // Whether to dump each INTERMEDIATE composited layer as USDA text per
    // iteration (debug aid). For heavy scenes this USDA serialization is itself
    // the blow-up (e.g. baked vertex-animation timeSamples), and it happens
    // inside the composition loop — before any post-loop memory cap. So when a
    // memory cap is set, skip these intermediate dumps; the final result is
    // still emitted (USDA, or compact USDC if over the cap) after the loop.
    const bool print_intermediate =
        !suppress_usd_text_output && (GetMaxUsdaOutputBytes() == 0);

    if (comp_features.subLayers) {
      lightusd::Layer composited_layer;
      if (!lightusd::CompositeSublayers(resolver, src_layer, &composited_layer, &warn, &err, sublayer_opts)) {
        std::cerr << "Failed to composite subLayers: " << err << "\n";
        return -1;
      }

      if (warn.size()) {
        std::cerr << "WARN: " << warn << "\n"; warn.clear();
      }

      if (print_intermediate) {
        std::cout << "# `subLayers` composited\n";
        std::cout << composited_layer << "\n";
      }

      src_layer = std::move(composited_layer);
    }

    // When the full set of arc features is enabled (default --flatten), use
    // CompositeAllArcs which implements the correct LIVRPS strength ordering
    // (L > I > V > R > P > S) with deferred variant evaluation. The legacy
    // per-feature loop below applies R -> P -> I -> V, which inverts LIVRPS
    // (references win over inherits/variants).
    const bool full_livrps = comp_features.inherits &&
                             comp_features.variantSets &&
                             comp_features.references && comp_features.payload;

    if (full_livrps) {
      for (int i = 0; i < kMaxIteration; i++) {
        bool has_unresolved = src_layer.check_unresolved_references() ||
                              src_layer.check_unresolved_payload() ||
                              src_layer.check_unresolved_inherits() ||
                              src_layer.check_unresolved_variant() ||
                              src_layer.check_unresolved_specializes();

        if (!has_unresolved) {
          break;
        }

        lightusd::Layer composited_layer;
        // Pass the configured per-arc options (parent-relative path policy for
        // UE-style exports, asset size caps, the shared parsed-layer cache) —
        // the option-less overload would reject `../` asset paths.
        lightusd::AllArcsCompositionOptions all_opts;
        all_opts.references = reference_opts;
        all_opts.payload = payload_opts;
        if (!lightusd::CompositeAllArcs(resolver, src_layer, &composited_layer,
                                        &warn, &err, all_opts)) {
          std::cerr << "Failed to composite arcs: " << err << "\n";
          return -1;
        }

        if (warn.size()) {
          std::cerr << "WARN: " << warn << "\n";
          warn.clear();
        }

        src_layer = std::move(composited_layer);
      }
    } else {
    // TODO: Find more better way to Recursively resolve references/payload/variants
    for (int i = 0; i < kMaxIteration; i++) {

      bool has_unresolved = false;

      if (comp_features.references) {
        if (!src_layer.check_unresolved_references()) {
          std::cout << "# iter " << i << ": no unresolved references.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          // InPlace: consumes src_layer (no internal arcs) instead of holding
          // input + output copies — halves the peak of the pass.
          if (!lightusd::CompositeReferencesInPlace(resolver,
                  std::make_unique<lightusd::Layer>(std::move(src_layer)),
                  &composited_layer, &warn, &err, reference_opts)) {
            std::cerr << "Failed to composite `references`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cerr << "WARN: " << warn << "\n"; warn.clear();
          }

          if (print_intermediate) {
            std::cout << "# `references` composited\n";
            std::cout << composited_layer << "\n";
          }

          src_layer = std::move(composited_layer);
          phase_mark("  compose:references");
        }
      }

      if (comp_features.payload) {
        if (!src_layer.check_unresolved_payload()) {
          std::cout << "# iter " << i << ": no unresolved payload.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositePayloadInPlace(resolver,
                  std::make_unique<lightusd::Layer>(std::move(src_layer)),
                  &composited_layer, &warn, &err, payload_opts)) {
            std::cerr << "Failed to composite `payload`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cerr << "WARN: " << warn << "\n"; warn.clear();
          }

          if (print_intermediate) {
            std::cout << "# `payload` composited\n";
            std::cout << composited_layer << "\n";
          }

          src_layer = std::move(composited_layer);
          phase_mark("  compose:payload");
        }
      }

      if (comp_features.inherits) {
        if (!src_layer.check_unresolved_inherits()) {
          std::cout << "# iter " << i << ": no unresolved inherits.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositeInherits(src_layer, &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `inherits`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cerr << "WARN: " << warn << "\n"; warn.clear();
          }

          if (print_intermediate) {
            std::cout << "# `inherits` composited\n";
            std::cout << composited_layer << "\n";
          }

          src_layer = std::move(composited_layer);
          phase_mark("  compose:inherits");
        }
      }

      if (comp_features.variantSets) {
        // AOUSD Core Spec 10.3.2.5: defer variant composition until references
        // and payloads are resolved (see ShouldDeferVariantComposition) — the
        // populated variantSet may live in a referenced/payloaded sublayer.
        if (!src_layer.check_unresolved_variant()) {
          std::cout << "# iter " << i << ": no unresolved variant.\n";
        } else if (lightusd::ShouldDeferVariantComposition(
                       src_layer, comp_features.references,
                       comp_features.payload)) {
          std::cout << "# iter " << i
                    << ": variant resolution deferred (refs/payloads pending).\n";
          has_unresolved = true;
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          // InPlace: consumes src_layer (variant selection needs no
          // pristine-layer lookups) — skips the whole-layer deep copy.
          if (!lightusd::CompositeVariantInPlace(
                  std::make_unique<lightusd::Layer>(std::move(src_layer)),
                  &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `variantSet`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cerr << "WARN: " << warn << "\n"; warn.clear();
          }

          if (print_intermediate) {
            std::cout << "# `variantSet` composited\n";
            std::cout << composited_layer << "\n";
          }

          src_layer = std::move(composited_layer);
          phase_mark("  compose:variantSets");
        }
      }

      // TODO
      // - [ ] specializes
      // - [ ] `class` Prim?

      std::cout << "# has_unresolved_references: " << src_layer.check_unresolved_references() << "\n";
      std::cout << "# all resolved? " << !has_unresolved << "\n";

      if (!has_unresolved) {
        std::cout << "# of composition iteration to resolve fully: " << (i + 1) << "\n";
        break;
      }

    }
    }  // !full_livrps
    phase_mark("compose(LIVRP fixed-point)");

    if (has_extract_variants) {
      std::cout << "\n=== VARIANT EXTRACTION (" << variant_format << ") ===\n";

      lightusd::Dictionary dict;
      if (!lightusd::ExtractVariants(src_layer, &dict, &err)) {
        std::cerr << "Failed to extract variants info: " << err;
      } else {
        if (variant_format == "json") {
          std::cout << variant_format::dictionary_to_json(dict) << "\n";
        } else {
          std::cout << variant_format::dictionary_to_yaml(dict) << "\n";
        }
      }

    }

    // Flatten finalize: the layer is now fully composed, so bake every
    // apiSchemas list-op to explicit form -- matches `usdcat --flatten`, which
    // never emits a `prepend`/`append` qualifier on a composed prim. Must run
    // after the iteration loop (a mid-flatten reset would shadow apiSchemas a
    // later arc still contributes).
    lightusd::FlattenAppliedSchemas(src_layer);

    // --preserve-order (USDA only): emit the composed LAYER directly so prim
    // children AND properties keep their authored order (the generic PrimSpec
    // property map + primChildren/properties metadata honor the order under
    // pprint::SetPreserveAuthoredOrder). The Stage path serializes typed prims
    // in schema-fixed attribute order, so it cannot reproduce usdcat's property
    // order. Captured BEFORE the Layer is moved into LayerToStage below.
    std::string preserved_layer_usda;
    const bool emit_preserved_layer =
        preserve_order && !json_output &&
        output_format != OutputFormat::USDC &&
        output_format != OutputFormat::USDZ;  // USDA or Infer(stdout)
    if (emit_preserved_layer) {
      // --openusd-compat: stamp the flattened layer's `documentation` with the
      // same provenance line `usdcat --flatten` injects (UsdStage::Flatten with
      // addSourceFileComment=true): "Generated from Composed Stage of root layer
      // <root layer path>\n", appended after any existing doc (separated by a
      // blank line), and emitted triple-quoted. Only under --openusd-compat: the
      // line embeds an absolute, machine-specific path, so it is intentionally
      // omitted by default (and from --preserve-order) for reproducible output.
      if (openusd_compat) {
        std::string &docval = src_layer.metas().doc.value;
        if (!docval.empty()) {
          docval += "\n\n";
        }
        docval += "Generated from Composed Stage of root layer " + filepath + "\n";
        src_layer.metas().doc.is_triple_quoted = true;
      }
      preserved_layer_usda = lightusd::to_string(src_layer);
    }

    lightusd::Stage comp_stage;
    phase_mark("flatten-finalize(apiSchemas/preserve-order)");
    try {
      ret = LayerToStage(std::move(src_layer), &comp_stage, &warn, &err);
    } catch (const std::bad_alloc &) {
      // OOM detection: turn an allocation failure into a clean error instead of
      // an uncaught std::bad_alloc -> std::terminate -> abort().
      std::cerr << "ERR: out of memory while building the composed Stage. "
                   "Set LUSDCAT_MAX_USDA_MB or use USDC output for heavy scenes.\n";
      return EXIT_FAILURE;
    }
    if (warn.size()) {
      std::cout << warn<< "\n";
    }

    if (!ret) {
      std::cerr << err << "\n";
    }
    phase_mark("LayerToStage");

    if (memstat) {
      size_t stage_mem = comp_stage.estimate_memory_usage();
      std::cout << "\n# Memory Statistics (Stage after composition)\n";
      std::cout << "  Stage memory usage: " << format_memory_size(stage_mem)
                << " (" << stage_mem << " bytes)\n\n";
    }

    if (json_output) {
#if defined(LIGHTUSD_WITH_JSON)
      auto json_result = lightusd::ToJSON(comp_stage);
      if (json_result) {
        std::cout << json_result.value() << "\n";
      } else {
        std::cerr << "Failed to convert composed stage to JSON: " << json_result.error() << "\n";
        return EXIT_FAILURE;
      }
#else
      std::cerr << "JSON output is not supported in this build\n";
#endif
    } else if (!suppress_usd_text_output) {
      const size_t est_bytes = comp_stage.estimate_memory_usage();
      const size_t cap_bytes = GetMaxUsdaOutputBytes();
      if (cap_bytes && est_bytes > cap_bytes) {
        // Over the USDA cap: keep timeSamples compact by serializing to USDC in
        // memory (binary, far smaller than baked USDA text) instead of emitting
        // a multi-GB USDA string that would exhaust memory.
        std::vector<uint8_t> usdc_bytes;
        std::string c_warn, c_err;
        if (lightusd::usdc::SaveAsUSDCToMemory(comp_stage, &usdc_bytes, &c_warn,
                                               &c_err)) {
          std::cerr << "# Composed stage estimate " << format_memory_size(est_bytes)
                    << " exceeds USDA output cap " << format_memory_size(cap_bytes)
                    << "; serialized compact USDC to memory ("
                    << format_memory_size(usdc_bytes.size())
                    << ") instead of USDA text. (Use -o out.usdc to write it.)\n";
        } else {
          std::cerr << "ERR: composed stage too large for USDA output ("
                    << format_memory_size(est_bytes) << " > cap "
                    << format_memory_size(cap_bytes)
                    << ") and the compact USDC fallback failed: " << c_err << "\n";
          return EXIT_FAILURE;
        }
      } else if (emit_preserved_layer) {
        // --preserve-order: emit the composed Layer (authored child/property
        // order) instead of the Stage.
        std::cout << preserved_layer_usda << "\n";
      } else {
        // Guard the (potentially huge) USDA serialization against allocation
        // failure: turn an out-of-memory condition into a clean error instead of
        // an uncaught std::bad_alloc -> std::terminate -> abort().
        try {
          std::cout << comp_stage.ExportToString() << "\n";
        } catch (const std::bad_alloc &) {
          std::cerr << "ERR: out of memory while serializing composed stage to "
                       "USDA text (estimate " << format_memory_size(est_bytes)
                    << "). Use USDC output (-o out.usdc) or set LUSDCAT_MAX_USDA_MB "
                       "for large composed scenes.\n";
          return EXIT_FAILURE;
        }
      }
    }

    if (has_output_file) {
      if (emit_preserved_layer) {
        // --preserve-order: write the composed Layer (authored order) as USDA.
        // usdcat ends the file with a trailing blank line; match it (the stdout
        // path above already appends a newline).
        if (preserve_order && !preserved_layer_usda.empty() &&
            preserved_layer_usda.back() == '\n') {
          preserved_layer_usda.push_back('\n');
        }
        if (!lightusd::io::WriteWholeFile(output_filepath,
                reinterpret_cast<const uint8_t *>(preserved_layer_usda.data()),
                preserved_layer_usda.size(), &err)) {
          std::cerr << "Failed to write " << output_filepath << ": " << err << "\n";
          return EXIT_FAILURE;
        }
        std::cout << "Wrote USDA to [" << output_filepath << "]\n";
      } else if (!WriteStageToFile(comp_stage, output_filepath, output_format)) {
        return EXIT_FAILURE;
      }
    }
    phase_mark("write-output");

    PrintMeshPrims(comp_stage);
    phase_mark("list-prims(tail)");

  } else {

    lightusd::Stage stage;
    lightusd::usdc::USDCMemoryUsageReport usdc_memory_report;
    bool has_usdc_memory_report{false};

    lightusd::USDLoadOptions options;

    // MaterialX validation
    options.strict_mtlx_check = strict_mtlx_check;
    options.error_detail = error_detail;

    // Set up progress callback if requested
    ProgressState progress_state;
    if (show_progress) {
      progress_state.start_time = std::chrono::steady_clock::now();
      options.progress_callback = progress_callback;
      options.progress_userptr = &progress_state;
    }

    bool ret{false};
    if (ext == "usdc") {
      ret = LoadUSDCWithMemoryReport(filepath, show_progress, &stage,
                                     &usdc_memory_report, &warn, &err);
      has_usdc_memory_report = true;
    } else {
      // auto detect format.
      ret = lightusd::LoadUSDFromFile(filepath, &stage, &warn, &err, options);
    }
    if (!warn.empty()) {
      std::cerr << "WARN : " << warn << "\n";
    }
    if (!err.empty()) {
      std::cerr << "ERR : " << err << "\n";
      //return EXIT_FAILURE;
    }

    if (!ret) {
      std::cerr << "Failed to load USD file: " << filepath << "\n";
      return EXIT_FAILURE;
    }

    if (load_only) {
      if (memstat) {
        auto detail = stage.estimate_memory_usage_detail();
        std::cout << "# Memory Statistics (Stage)\n";
        std::cout << "  Allocated (capacity): " << format_memory_size(detail.allocated_bytes)
                  << " (" << detail.allocated_bytes << " bytes)\n";
        std::cout << "  Actual (in use):      " << format_memory_size(detail.actual_bytes)
                  << " (" << detail.actual_bytes << " bytes)\n";
        if (detail.allocated_bytes > 0) {
          double efficiency = 100.0 * (double(detail.actual_bytes) / double(detail.allocated_bytes));
          std::cout << "  Efficiency:           " << std::fixed << std::setprecision(1)
                    << efficiency << " %\n";
        }
        if (has_usdc_memory_report) {
          PrintUSDCParserMemoryReport(usdc_memory_report);
        }
      }
      return EXIT_SUCCESS;
    }

    if (memstat) {
      auto detail = stage.estimate_memory_usage_detail();
      std::cout << "# Memory Statistics (Stage)\n";
      std::cout << "  Allocated (capacity): " << format_memory_size(detail.allocated_bytes)
                << " (" << detail.allocated_bytes << " bytes)\n";
      std::cout << "  Actual (in use):      " << format_memory_size(detail.actual_bytes)
                << " (" << detail.actual_bytes << " bytes)\n";
      if (detail.allocated_bytes > 0) {
        double efficiency = 100.0 * (double(detail.actual_bytes) / double(detail.allocated_bytes));
        std::cout << "  Efficiency:           " << std::fixed << std::setprecision(1)
                  << efficiency << " %\n";
      }
      std::cout << "\n";
      if (has_usdc_memory_report) {
        PrintUSDCParserMemoryReport(usdc_memory_report);
        std::cout << "\n";
      }
    }

    if (json_output) {
#if defined(LIGHTUSD_WITH_JSON)
      auto json_result = lightusd::ToJSON(stage);
      if (json_result) {
        std::cout << json_result.value() << "\n";
      } else {
        std::cerr << "Failed to convert stage to JSON: " << json_result.error() << "\n";
        return EXIT_FAILURE;
      }
#else
      std::cerr << "JSON output is not supported in this build\n";
#endif
    } else if (!suppress_usd_text_output) {
      std::string s = stage.ExportToString(has_relative);
      std::cout << s << "\n";
    }

    if (has_output_file) {
      if (!WriteStageToFile(stage, output_filepath, output_format,
                            compress_float_arrays)) {
        return EXIT_FAILURE;
      }
    }

    if (has_extract_variants) {
      std::cout << "\n=== VARIANT EXTRACTION (" << variant_format << ") ===\n";

      lightusd::Dictionary dict;
      if (!lightusd::ExtractVariants(stage, &dict, &err)) {
        std::cerr << "Failed to extract variants info: " << err;
      } else {
        if (variant_format == "json") {
          std::cout << variant_format::dictionary_to_json(dict) << "\n";
        } else {
          std::cout << variant_format::dictionary_to_yaml(dict) << "\n";
        }
      }

    }
  }

  return EXIT_SUCCESS;
}
