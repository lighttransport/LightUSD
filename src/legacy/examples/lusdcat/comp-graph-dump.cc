#include "comp-graph-dump.hh"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <sys/stat.h>

#include "io-util.hh"
#include "str-util.hh"

namespace comp_graph_dump {

// Join base directory with a relative path, handling the missing trailing '/'
static std::string JoinPath(const std::string &base_dir, const std::string &rel) {
  if (base_dir.empty()) return rel;
  if (rel.empty()) return base_dir;
  char last = base_dir.back();
  if (last == '/' || last == '\\') {
    return base_dir + rel;
  }
  return base_dir + "/" + rel;
}

// Resolve an asset path relative to a source file
static std::string ResolveAssetPath(const std::string &source_file,
                                     const std::string &asset_path) {
  if (asset_path.empty()) return asset_path;
  if (lightusd::io::IsAbsPath(asset_path) || asset_path[0] == '~') return asset_path;
  std::string base = lightusd::io::GetBaseDir(source_file);
  return JoinPath(base, asset_path);
}

static const char *ListEditQualStr(lightusd::legacy::ListEditQual q) {
  switch (q) {
    case lightusd::legacy::ListEditQual::ResetToExplicit: return "explicit";
    case lightusd::legacy::ListEditQual::Append: return "append";
    case lightusd::legacy::ListEditQual::Add: return "add";
    case lightusd::legacy::ListEditQual::Delete: return "delete";
    case lightusd::legacy::ListEditQual::Prepend: return "prepend";
    case lightusd::legacy::ListEditQual::Order: return "order";
    case lightusd::legacy::ListEditQual::Invalid: return "invalid";
  }
  return "unknown";
}

static std::string DetectFormatByExt(const std::string &filepath) {
  std::string ext = lightusd::io::GetFileExtension(filepath);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (ext == "usda") return "usda";
  if (ext == "usdc") return "usdc";
  if (ext == "usdz") return "usdz";
  if (ext == "usd") return "usd";
  return ext;
}

static std::string DetectUSDType(const std::string &filepath) {
  if (lightusd::legacy::IsUSDZ(filepath)) return "usdz";
  if (lightusd::legacy::IsUSDC(filepath)) return "crate";
  if (lightusd::legacy::IsUSDA(filepath)) return "ascii";
  return "unknown";
}

static int64_t GetFileSize(const std::string &filepath) {
  struct stat st;
  if (stat(filepath.c_str(), &st) == 0) {
    return static_cast<int64_t>(st.st_size);
  }
  return -1;
}

static void PopulateFileInfo(CompGraphNode &node) {
  node.file_size = GetFileSize(node.file_path);
  if (node.file_size > 0) {
    node.usd_type = DetectUSDType(node.file_path);
  } else {
    node.usd_type = "unknown";
  }
}

// Stack-based DFS over PrimSpec tree to collect all composition arcs.
static void CollectArcsFromPrimSpecs(
    const std::string &parent_path,
    const std::vector<lightusd::legacy::PrimSpec> &children,
    std::vector<CompArc> &arcs) {

  struct StackEntry {
    const lightusd::legacy::PrimSpec *ps;
    std::string path;
  };

  std::vector<StackEntry> stack;
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    std::string p = parent_path + "/" + it->name();
    stack.push_back({&(*it), p});
  }

  constexpr size_t kMaxIter = 1024 * 1024;
  size_t iter = 0;

  while (!stack.empty() && iter < kMaxIter) {
    ++iter;
    auto entry = stack.back();
    stack.pop_back();

    const lightusd::legacy::PrimSpec *ps = entry.ps;
    const std::string &prim_path = entry.path;

    if (ps->metas().references.has_value()) {
      for (const auto &listop : ps->metas().references.value()) {
        for (const auto &ref : listop.second) {
          CompArc arc;
          arc.arc_type = "reference";
          arc.source_prim_path = prim_path;
          arc.target_asset_path = ref.asset_path.GetAssetPath();
          arc.target_prim_path = ref.prim_path.prim_part();
          arc.list_edit_qual = ListEditQualStr(listop.first);
          arcs.push_back(std::move(arc));
        }
      }
    }

    if (ps->metas().payload.has_value()) {
      for (const auto &listop : ps->metas().payload.value()) {
        for (const auto &pl : listop.second) {
          if (pl.is_none()) continue;
          CompArc arc;
          arc.arc_type = "payload";
          arc.source_prim_path = prim_path;
          arc.target_asset_path = pl.asset_path.GetAssetPath();
          arc.target_prim_path = pl.prim_path.prim_part();
          arc.list_edit_qual = ListEditQualStr(listop.first);
          arcs.push_back(std::move(arc));
        }
      }
    }

    if (ps->metas().inherits.has_value()) {
      for (const auto &listop : ps->metas().inherits.value()) {
        for (const auto &inh_path : listop.second) {
          CompArc arc;
          arc.arc_type = "inherits";
          arc.source_prim_path = prim_path;
          arc.target_prim_path = inh_path.prim_part();
          arc.list_edit_qual = ListEditQualStr(listop.first);
          arcs.push_back(std::move(arc));
        }
      }
    }

    if (ps->metas().specializes.has_value()) {
      for (const auto &listop : ps->metas().specializes.value()) {
        for (const auto &spec_path : listop.second) {
          CompArc arc;
          arc.arc_type = "specializes";
          arc.source_prim_path = prim_path;
          arc.target_prim_path = spec_path.prim_part();
          arc.list_edit_qual = ListEditQualStr(listop.first);
          arcs.push_back(std::move(arc));
        }
      }
    }

    if (ps->metas().variantSets.has_value()) {
      for (const auto &listop : ps->metas().variantSets.value()) {
        for (const auto &vs_name : listop.second) {
          CompArc arc;
          arc.arc_type = "variantSet";
          arc.source_prim_path = prim_path;
          arc.variant_set_name = vs_name;
          arc.list_edit_qual = ListEditQualStr(listop.first);

          const auto &vs_map = ps->variantSets();
          auto vs_it = vs_map.find(vs_name);
          if (vs_it != vs_map.end()) {
            for (const auto &v : vs_it->second.variantSet) {
              arc.variant_names.push_back(v.first);
            }
          }

          if (ps->metas().variants.has_value()) {
            const auto &sel = ps->metas().variants.value();
            auto sel_it = sel.find(vs_name);
            if (sel_it != sel.end()) {
              arc.selected_variant = sel_it->second;
            }
          }

          arcs.push_back(std::move(arc));
        }
      }
    }

    const auto &ch = ps->children();
    for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
      std::string child_path = prim_path + "/" + it->name();
      stack.push_back({&(*it), child_path});
    }
  }
}

bool ExtractCompGraph(const lightusd::legacy::Layer &layer,
                      const std::string &file_path,
                      CompGraphDump *out, std::string *err) {
  if (!out) {
    if (err) *err = "out is nullptr";
    return false;
  }

  out->root_file = file_path;

  CompGraphNode node;
  node.file_path = file_path;
  node.file_format = DetectFormatByExt(file_path);
  PopulateFileInfo(node);
  node.parse_attempted = true;
  node.parse_ok = true;

  // Sublayers (layer-level)
  const auto &sublayers = layer.metas().subLayers;
  for (const auto &sl : sublayers) {
    CompArc arc;
    arc.arc_type = "sublayer";
    arc.target_asset_path = sl.assetPath.GetAssetPath();
    node.arcs.push_back(std::move(arc));
  }

  // Traverse all root PrimSpecs
  const auto &primspecs = layer.primspecs();
  for (const auto &kv : primspecs) {
    std::vector<lightusd::legacy::PrimSpec> root_vec = {kv.second};
    CollectArcsFromPrimSpecs("", root_vec, node.arcs);
  }

  out->nodes.push_back(std::move(node));
  return true;
}

bool ExtractCompGraphRecursive(const std::string &root_path,
                               CompGraphDump *out,
                               const ExtractOptions &opts,
                               std::string *warn, std::string *err) {
  if (!out) {
    if (err) *err = "out is nullptr";
    return false;
  }

  out->root_file = root_path;

  struct QueueEntry {
    std::string file_path;
  };

  std::vector<QueueEntry> queue;
  std::set<std::string> visited;

  queue.push_back({root_path});
  visited.insert(root_path);

  while (!queue.empty()) {
    QueueEntry entry = queue.back();
    queue.pop_back();

    CompGraphNode node;
    node.file_path = entry.file_path;
    node.file_format = DetectFormatByExt(entry.file_path);
    PopulateFileInfo(node);
    node.parse_attempted = true;

    lightusd::legacy::Layer layer;
    std::string local_warn, local_err;
    bool loaded = lightusd::legacy::LoadLayerFromFile(entry.file_path, &layer,
                                               &local_warn, &local_err);

    if (!loaded) {
      node.parse_ok = false;
      node.parse_error = local_err;
      out->nodes.push_back(std::move(node));
      if (warn) {
        *warn += "Warning: failed to load " + entry.file_path + ": " +
                 local_err + "\n";
      }
      continue;
    }

    node.parse_ok = true;

    // Memory tracking
    if (opts.track_memory) {
      node.memory_usage = static_cast<int64_t>(layer.estimate_memory_usage());
    }

    if (!opts.parse_only) {
      // Extract arcs: sublayers
      const auto &sublayers = layer.metas().subLayers;
      for (const auto &sl : sublayers) {
        CompArc arc;
        arc.arc_type = "sublayer";
        arc.target_asset_path = sl.assetPath.GetAssetPath();
        node.arcs.push_back(std::move(arc));
      }

      // Extract arcs: primspecs
      const auto &primspecs = layer.primspecs();
      for (const auto &kv : primspecs) {
        std::vector<lightusd::legacy::PrimSpec> root_vec = {kv.second};
        CollectArcsFromPrimSpecs("", root_vec, node.arcs);
      }
    } else {
      // parse_only: still need to discover arcs for recursive traversal
      // but don't store them in the output (except we need them to enqueue)
      // We'll collect temporarily
      std::vector<CompArc> temp_arcs;

      const auto &sublayers = layer.metas().subLayers;
      for (const auto &sl : sublayers) {
        CompArc arc;
        arc.arc_type = "sublayer";
        arc.target_asset_path = sl.assetPath.GetAssetPath();
        temp_arcs.push_back(std::move(arc));
      }

      const auto &primspecs = layer.primspecs();
      for (const auto &kv : primspecs) {
        std::vector<lightusd::legacy::PrimSpec> root_vec = {kv.second};
        CollectArcsFromPrimSpecs("", root_vec, temp_arcs);
      }

      // Enqueue from temp_arcs
      for (const auto &arc : temp_arcs) {
        if (arc.target_asset_path.empty()) continue;
        if (opts.skip_payloads && arc.arc_type == "payload") continue;
        std::string resolved = ResolveAssetPath(entry.file_path,
                                                 arc.target_asset_path);
        if (visited.count(resolved) == 0) {
          visited.insert(resolved);
          queue.push_back({resolved});
        }
      }

      out->nodes.push_back(std::move(node));
      continue;
    }

    // Enqueue referenced files from node.arcs
    for (const auto &arc : node.arcs) {
      if (arc.target_asset_path.empty()) continue;
      if (opts.skip_payloads && arc.arc_type == "payload") continue;
      std::string resolved = ResolveAssetPath(entry.file_path,
                                               arc.target_asset_path);
      if (visited.count(resolved) == 0) {
        visited.insert(resolved);
        queue.push_back({resolved});
      }
    }

    out->nodes.push_back(std::move(node));
  }

  return true;
}

void CompGraphDump::ComputeSizeSummary() {
  total_file_size = 0;
  total_no_payload = 0;
  total_with_payload = 0;
  file_count = 0;
  file_count_no_payload = 0;
  file_count_with_payload = 0;
  total_memory = 0;
  total_memory_no_payload = 0;
  total_memory_with_payload = 0;
  parse_ok_count = 0;
  parse_fail_count = 0;

  if (nodes.empty()) return;

  // Count parse status
  for (const auto &node : nodes) {
    if (node.parse_attempted) {
      if (node.parse_ok) parse_ok_count++;
      else parse_fail_count++;
    }
  }

  // Build node lookup
  std::map<std::string, size_t> path_to_idx;
  for (size_t i = 0; i < nodes.size(); ++i) {
    path_to_idx[nodes[i].file_path] = i;
  }

  // BFS for no-payload reachable files
  std::set<std::string> no_payload_files;
  {
    std::vector<std::string> bfs_queue;
    std::set<std::string> bfs_visited;
    bfs_queue.push_back(nodes[0].file_path);
    bfs_visited.insert(nodes[0].file_path);

    while (!bfs_queue.empty()) {
      std::string cur = bfs_queue.back();
      bfs_queue.pop_back();
      no_payload_files.insert(cur);

      auto it = path_to_idx.find(cur);
      if (it == path_to_idx.end()) continue;
      const auto &node = nodes[it->second];

      for (const auto &arc : node.arcs) {
        if (arc.target_asset_path.empty()) continue;
        if (arc.arc_type == "payload") continue;
        std::string resolved = ResolveAssetPath(node.file_path, arc.target_asset_path);
        if (bfs_visited.count(resolved) == 0) {
          bfs_visited.insert(resolved);
          bfs_queue.push_back(resolved);
        }
      }
    }
  }

  // All files = every node + leaf targets
  std::set<std::string> all_files;
  for (const auto &node : nodes) {
    all_files.insert(node.file_path);
    for (const auto &arc : node.arcs) {
      if (arc.target_asset_path.empty()) continue;
      std::string resolved = ResolveAssetPath(node.file_path, arc.target_asset_path);
      all_files.insert(resolved);
    }
  }

  // Size and memory caches
  std::map<std::string, int64_t> size_cache;
  std::map<std::string, int64_t> mem_cache;
  for (const auto &node : nodes) {
    size_cache[node.file_path] = node.file_size;
    if (node.memory_usage >= 0) {
      mem_cache[node.file_path] = node.memory_usage;
    }
  }

  auto get_size = [&](const std::string &path) -> int64_t {
    auto it = size_cache.find(path);
    if (it != size_cache.end()) return std::max(it->second, int64_t(0));
    int64_t sz = GetFileSize(path);
    size_cache[path] = sz;
    return std::max(sz, int64_t(0));
  };

  auto get_mem = [&](const std::string &path) -> int64_t {
    auto it = mem_cache.find(path);
    if (it != mem_cache.end()) return it->second;
    return 0;
  };

  for (const auto &f : all_files) {
    total_file_size += get_size(f);
    total_memory += get_mem(f);
    file_count++;
  }

  for (const auto &f : no_payload_files) {
    total_no_payload += get_size(f);
    total_memory_no_payload += get_mem(f);
    file_count_no_payload++;
  }

  total_with_payload = total_file_size;
  total_memory_with_payload = total_memory;
  file_count_with_payload = file_count;
}

// --- Output Formatters ---

static std::string FormatSize(int64_t bytes) {
  if (bytes < 0) return "N/A";
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int idx = 0;
  double sz = static_cast<double>(bytes);
  while (sz >= 1024.0 && idx < 4) { sz /= 1024.0; idx++; }
  std::ostringstream ss;
  if (idx == 0) {
    ss << bytes << " B";
  } else {
    ss << std::fixed;
    ss.precision(2);
    ss << sz << " " << units[idx];
  }
  return ss.str();
}

static std::string EscapeJSON(const std::string &s) {
  std::string result;
  result.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result += c;
    }
  }
  return result;
}

// Helper to emit per-node JSON fields (parse_status, memory)
static void EmitNodeJSON(std::ostringstream &ss, const CompGraphNode &node) {
  ss << "      \"path\": \"" << EscapeJSON(node.file_path) << "\",\n";
  ss << "      \"format\": \"" << EscapeJSON(node.file_format) << "\",\n";
  ss << "      \"usd_type\": \"" << EscapeJSON(node.usd_type) << "\",\n";
  if (node.file_size >= 0) {
    ss << "      \"file_size\": " << node.file_size << ",\n";
    ss << "      \"file_size_human\": \"" << EscapeJSON(FormatSize(node.file_size)) << "\",\n";
  }
  if (node.parse_attempted) {
    ss << "      \"parse_status\": \"" << (node.parse_ok ? "ok" : "fail") << "\",\n";
    if (!node.parse_ok && !node.parse_error.empty()) {
      ss << "      \"parse_error\": \"" << EscapeJSON(node.parse_error) << "\",\n";
    }
  }
  if (node.memory_usage >= 0) {
    ss << "      \"memory_usage\": " << node.memory_usage << ",\n";
    ss << "      \"memory_usage_human\": \"" << EscapeJSON(FormatSize(node.memory_usage)) << "\",\n";
  }
}

// Helper: emit memory fields in summary JSON
static void EmitMemorySummaryJSON(std::ostringstream &ss, int64_t mem, bool has_mem) {
  if (has_mem) {
    ss << ",\n      \"total_memory\": " << mem;
    ss << ",\n      \"total_memory_human\": \"" << EscapeJSON(FormatSize(mem)) << "\"";
  }
}

std::string CompGraphToJSON(const CompGraphDump &graph) {
  bool has_mem = graph.total_memory > 0 || graph.total_memory_with_payload > 0;
  // Check if any node has memory_usage
  for (const auto &n : graph.nodes) {
    if (n.memory_usage >= 0) { has_mem = true; break; }
  }

  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"root\": \"" << EscapeJSON(graph.root_file) << "\",\n";
  ss << "  \"files\": [\n";

  for (size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto &node = graph.nodes[i];
    ss << "    {\n";
    EmitNodeJSON(ss, node);
    ss << "      \"arcs\": [\n";

    for (size_t j = 0; j < node.arcs.size(); ++j) {
      const auto &arc = node.arcs[j];
      ss << "        {\n";
      ss << "          \"type\": \"" << EscapeJSON(arc.arc_type) << "\"";

      if (!arc.source_prim_path.empty())
        ss << ",\n          \"source\": \"" << EscapeJSON(arc.source_prim_path) << "\"";
      if (!arc.target_asset_path.empty())
        ss << ",\n          \"target_asset\": \"" << EscapeJSON(arc.target_asset_path) << "\"";
      if (!arc.target_prim_path.empty())
        ss << ",\n          \"target_prim\": \"" << EscapeJSON(arc.target_prim_path) << "\"";
      if (!arc.list_edit_qual.empty())
        ss << ",\n          \"listop\": \"" << EscapeJSON(arc.list_edit_qual) << "\"";
      if (!arc.variant_set_name.empty()) {
        ss << ",\n          \"variant_set\": \"" << EscapeJSON(arc.variant_set_name) << "\"";
        if (!arc.variant_names.empty()) {
          ss << ",\n          \"variants\": [";
          for (size_t k = 0; k < arc.variant_names.size(); ++k) {
            if (k > 0) ss << ", ";
            ss << "\"" << EscapeJSON(arc.variant_names[k]) << "\"";
          }
          ss << "]";
        }
        if (!arc.selected_variant.empty())
          ss << ",\n          \"selected\": \"" << EscapeJSON(arc.selected_variant) << "\"";
      }
      ss << "\n        }";
      if (j + 1 < node.arcs.size()) ss << ",";
      ss << "\n";
    }

    ss << "      ]\n";
    ss << "    }";
    if (i + 1 < graph.nodes.size()) ss << ",";
    ss << "\n";
  }

  ss << "  ],\n";
  ss << "  \"summary\": {\n";
  ss << "    \"file_count\": " << graph.file_count << ",\n";
  ss << "    \"parse_ok\": " << graph.parse_ok_count << ",\n";
  ss << "    \"parse_fail\": " << graph.parse_fail_count << ",\n";
  ss << "    \"total_size\": " << graph.total_file_size << ",\n";
  ss << "    \"total_size_human\": \"" << EscapeJSON(FormatSize(graph.total_file_size)) << "\"";
  if (has_mem) {
    ss << ",\n    \"total_memory\": " << graph.total_memory;
    ss << ",\n    \"total_memory_human\": \"" << EscapeJSON(FormatSize(graph.total_memory)) << "\"";
  }
  ss << ",\n    \"payload_off\": {\n";
  ss << "      \"file_count\": " << graph.file_count_no_payload << ",\n";
  ss << "      \"total_size\": " << graph.total_no_payload << ",\n";
  ss << "      \"total_size_human\": \"" << EscapeJSON(FormatSize(graph.total_no_payload)) << "\"";
  EmitMemorySummaryJSON(ss, graph.total_memory_no_payload, has_mem);
  ss << "\n    },\n";
  ss << "    \"payload_on\": {\n";
  ss << "      \"file_count\": " << graph.file_count_with_payload << ",\n";
  ss << "      \"total_size\": " << graph.total_with_payload << ",\n";
  ss << "      \"total_size_human\": \"" << EscapeJSON(FormatSize(graph.total_with_payload)) << "\"";
  EmitMemorySummaryJSON(ss, graph.total_memory_with_payload, has_mem);
  ss << "\n    }\n";
  ss << "  }\n";
  ss << "}\n";
  return ss.str();
}

std::string CompGraphToYAML(const CompGraphDump &graph) {
  bool has_mem = false;
  for (const auto &n : graph.nodes) {
    if (n.memory_usage >= 0) { has_mem = true; break; }
  }

  std::ostringstream ss;
  ss << "root: " << graph.root_file << "\n";
  ss << "files:\n";

  for (const auto &node : graph.nodes) {
    ss << "  - path: " << node.file_path << "\n";
    ss << "    format: " << node.file_format << "\n";
    ss << "    usd_type: " << node.usd_type << "\n";
    if (node.file_size >= 0) {
      ss << "    file_size: " << node.file_size << "\n";
      ss << "    file_size_human: " << FormatSize(node.file_size) << "\n";
    }
    if (node.parse_attempted) {
      ss << "    parse_status: " << (node.parse_ok ? "ok" : "fail") << "\n";
      if (!node.parse_ok && !node.parse_error.empty()) {
        ss << "    parse_error: " << node.parse_error << "\n";
      }
    }
    if (node.memory_usage >= 0) {
      ss << "    memory_usage: " << node.memory_usage << "\n";
      ss << "    memory_usage_human: " << FormatSize(node.memory_usage) << "\n";
    }

    if (node.arcs.empty()) {
      ss << "    arcs: []\n";
      continue;
    }

    ss << "    arcs:\n";
    for (const auto &arc : node.arcs) {
      ss << "      - type: " << arc.arc_type << "\n";
      if (!arc.source_prim_path.empty())
        ss << "        source: " << arc.source_prim_path << "\n";
      if (!arc.target_asset_path.empty())
        ss << "        target_asset: " << arc.target_asset_path << "\n";
      if (!arc.target_prim_path.empty())
        ss << "        target_prim: " << arc.target_prim_path << "\n";
      if (!arc.list_edit_qual.empty())
        ss << "        listop: " << arc.list_edit_qual << "\n";
      if (!arc.variant_set_name.empty()) {
        ss << "        variant_set: " << arc.variant_set_name << "\n";
        if (!arc.variant_names.empty()) {
          ss << "        variants: [";
          for (size_t k = 0; k < arc.variant_names.size(); ++k) {
            if (k > 0) ss << ", ";
            ss << arc.variant_names[k];
          }
          ss << "]\n";
        }
        if (!arc.selected_variant.empty())
          ss << "        selected: " << arc.selected_variant << "\n";
      }
    }
  }

  ss << "summary:\n";
  ss << "  file_count: " << graph.file_count << "\n";
  ss << "  parse_ok: " << graph.parse_ok_count << "\n";
  ss << "  parse_fail: " << graph.parse_fail_count << "\n";
  ss << "  total_size: " << graph.total_file_size << "\n";
  ss << "  total_size_human: " << FormatSize(graph.total_file_size) << "\n";
  if (has_mem) {
    ss << "  total_memory: " << graph.total_memory << "\n";
    ss << "  total_memory_human: " << FormatSize(graph.total_memory) << "\n";
  }
  ss << "  payload_off:\n";
  ss << "    file_count: " << graph.file_count_no_payload << "\n";
  ss << "    total_size: " << graph.total_no_payload << "\n";
  ss << "    total_size_human: " << FormatSize(graph.total_no_payload) << "\n";
  if (has_mem) {
    ss << "    total_memory: " << graph.total_memory_no_payload << "\n";
    ss << "    total_memory_human: " << FormatSize(graph.total_memory_no_payload) << "\n";
  }
  ss << "  payload_on:\n";
  ss << "    file_count: " << graph.file_count_with_payload << "\n";
  ss << "    total_size: " << graph.total_with_payload << "\n";
  ss << "    total_size_human: " << FormatSize(graph.total_with_payload) << "\n";
  if (has_mem) {
    ss << "    total_memory: " << graph.total_memory_with_payload << "\n";
    ss << "    total_memory_human: " << FormatSize(graph.total_memory_with_payload) << "\n";
  }

  return ss.str();
}

std::string CompGraphToDOT(const CompGraphDump &graph) {
  std::ostringstream ss;
  ss << "digraph composition {\n";
  ss << "  rankdir=LR;\n";
  ss << "  node [shape=box, fontname=\"Helvetica\", fontsize=10];\n";
  ss << "  edge [fontname=\"Helvetica\", fontsize=9];\n";
  ss << "\n";

  // Collect unique file nodes
  std::set<std::string> file_nodes;
  for (const auto &node : graph.nodes) {
    file_nodes.insert(node.file_path);
    for (const auto &arc : node.arcs) {
      if (!arc.target_asset_path.empty()) {
        std::string resolved = ResolveAssetPath(node.file_path, arc.target_asset_path);
        file_nodes.insert(resolved);
      }
    }
  }

  // Build parse status and size info for labels
  std::map<std::string, const CompGraphNode*> node_map;
  for (const auto &node : graph.nodes) {
    node_map[node.file_path] = &node;
  }

  for (const auto &f : file_nodes) {
    std::string label = lightusd::io::GetBaseFilename(f);
    auto it = node_map.find(f);
    if (it != node_map.end()) {
      const auto *n = it->second;
      label += "\\n" + FormatSize(n->file_size);
      if (n->parse_attempted && !n->parse_ok) {
        label += "\\nFAIL";
      }
    }
    ss << "  \"" << EscapeJSON(f) << "\" [label=\"" << EscapeJSON(label) << "\"";
    if (it != node_map.end() && it->second->parse_attempted && !it->second->parse_ok) {
      ss << ", color=red, fontcolor=red";
    }
    ss << "];\n";
  }
  ss << "\n";

  for (const auto &node : graph.nodes) {
    for (const auto &arc : node.arcs) {
      if (arc.target_asset_path.empty()) continue;
      std::string target = ResolveAssetPath(node.file_path, arc.target_asset_path);
      std::string label = arc.arc_type;
      if (!arc.source_prim_path.empty()) {
        label += "\\n" + arc.source_prim_path;
      }
      ss << "  \"" << EscapeJSON(node.file_path) << "\" -> \""
         << EscapeJSON(target) << "\" [label=\"" << EscapeJSON(label) << "\"];\n";
    }
  }

  ss << "}\n";
  return ss.str();
}

}  // namespace comp_graph_dump
