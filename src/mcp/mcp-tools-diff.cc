// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Value-level layer diff tools of the next MCP server (next::Diff; the same
// summary / paths / tree / prim / text / json contract as the legacy tools).
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/stage/stage.hh"

namespace lightusd {
namespace mcp {

using next::DiffOptions;
using next::LayerMetaDiff;
using next::ModifiedPrimSpec;
using next::PrimSpecDiff;
using next::PropDiff;

namespace {

std::string JoinPrimPath(const std::string &parent, const std::string &child) {
  if (parent.empty() || parent == "/") return "/" + child;
  return parent + "/" + child;
}

std::string ParentPath(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return "";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string LeafName(const std::string &path) {
  const auto pos = path.find_last_of('/');
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Root layer of a stage, or its flattened composition.
next::Layer SideLayer(const next::Stage &stage, bool flatten) {
  if (flatten) return stage.Flatten();
  const next::Layer *root = stage.GetRootLayer();
  return root ? root->Clone() : next::Layer();
}

// Load one diff side from {path | data(base64) | uuid}.
bool LoadDiffSide(Context &ctx, const json &side, bool flatten,
                  next::Layer *out, std::string *default_name,
                  std::string &err) {
  if (side.contains("uuid") && side["uuid"].is_string()) {
    const std::string uuid = side["uuid"].get<std::string>();
    auto it = ctx.layers.find(uuid);
    if (it == ctx.layers.end()) {
      err = "No loaded layer with uuid: " + uuid;
      return false;
    }
    *out = SideLayer(*NativeStage(it->second.stage), flatten);
    *default_name = it->second.name.empty() ? uuid : it->second.name;
    return true;
  }
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  opts.composed = flatten ? 1 : 0;
  lightusd_stage *stage = nullptr;
  if (side.contains("path") && side["path"].is_string()) {
#if defined(__EMSCRIPTEN__)
    err = "diff side 'path' is not supported in this build; use 'data' or 'uuid'";
    return false;
#else
    const std::string path = side["path"].get<std::string>();
    if (lightusd_stage_load(path.c_str(), &opts, &stage) != LIGHTUSD_OK) {
      err = std::string("Failed to load ") + path + ": " + lightusd_last_error();
      return false;
    }
    *default_name = path;
#endif
  } else if (side.contains("data") && side["data"].is_string()) {
    std::string binary;
    if (!DecodeBase64(side["data"].get<std::string>(), &binary, &err)) return false;
    if (lightusd_stage_load_from_memory(
            reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
            &opts, &stage) != LIGHTUSD_OK) {
      err = std::string("Failed to load data: ") + lightusd_last_error();
      return false;
    }
    *default_name = side.value("name", std::string("memory.usd"));
  } else {
    err = "diff side must specify one of: 'path', 'data' (base64), or 'uuid'";
    return false;
  }
  StageRef ref(stage);
  *out = SideLayer(*NativeStage(ref), flatten);
  return true;
}

next::DiffOptions BuildDiffOptions(const json &args) {
  next::DiffOptions opts;
  if (args.contains("ulps") && args["ulps"].is_number()) {
    const uint64_t u = args["ulps"].get<uint64_t>();
    opts.floatUlps = static_cast<uint32_t>(u);
    opts.doubleUlps = u;
  }
  if (args.contains("eps") && args["eps"].is_number()) {
    opts.absEps = args["eps"].get<double>();
  }
  if (args.contains("compareMetadata") && args["compareMetadata"].is_boolean()) {
    opts.compareMetadata = args["compareMetadata"].get<bool>();
  }
  return opts;
}

}  // namespace

// ===========================================================================
// Shared query helpers
// ===========================================================================
void DiffComputeSummary(const DiffSession &d, nlohmann::json &out) {
  size_t prims_added = 0, prims_deleted = 0, prims_modified = 0;
  size_t props_added = 0, props_deleted = 0, props_modified = 0;
  std::map<std::string, size_t> reason_tally;

  for (const auto &kv : d.psDiffs) {
    prims_added += kv.second.addedPS.size();
    prims_deleted += kv.second.deletedPS.size();
    prims_modified += kv.second.modifiedPS.size();
    for (const auto &m : kv.second.modifiedDetails) {
      for (const auto &r : m.reasons) reason_tally[r]++;
    }
  }
  for (const auto &kv : d.propDiffs) {
    props_added += kv.second.addedProps.size();
    props_deleted += kv.second.deletedProps.size();
    props_modified += kv.second.modifiedProps.size();
    for (const auto &m : kv.second.modifiedPropDetails) {
      for (const auto &r : m.reasons) reason_tally[r]++;
    }
  }

  out["left"] = d.left_name;
  out["right"] = d.right_name;
  out["prims"] = {{"added", prims_added},
                  {"deleted", prims_deleted},
                  {"modified", prims_modified}};
  out["properties"] = {{"added", props_added},
                       {"deleted", props_deleted},
                       {"modified", props_modified}};
  out["layerMetadataChanged"] = d.layerMetaDiff.changedFields;

  // reasons sorted by count desc (most informative first).
  std::vector<std::pair<std::string, size_t>> reasons(reason_tally.begin(),
                                                      reason_tally.end());
  std::sort(reasons.begin(), reasons.end(),
            [](const std::pair<std::string, size_t> &a,
               const std::pair<std::string, size_t> &b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });
  out["reasons"] = nlohmann::json::array();
  for (const auto &r : reasons) {
    out["reasons"].push_back({{"reason", r.first}, {"count", r.second}});
  }
  out["hasDiffs"] = (prims_added + prims_deleted + prims_modified + props_added +
                         props_deleted + props_modified >
                     0) ||
                    d.layerMetaDiff.changed();
}

namespace {

bool MatchesReason(const std::vector<std::string> &reasons,
                   const std::string &needle) {
  if (needle.empty()) return true;
  for (const auto &r : reasons) {
    if (r.find(needle) != std::string::npos) return true;
  }
  return false;
}

struct PathEntry {
  std::string path;
  std::string kind;  // prim_added / prim_deleted / prim_modified / prop_*
  std::vector<std::string> reasons;
};

// Enumerate every change in the diff as a flat list of (path, kind, reasons).
void CollectEntries(const DiffSession &d, std::vector<PathEntry> &entries) {
  for (const auto &kv : d.psDiffs) {
    const std::string &parent = kv.first;
    for (const auto &name : kv.second.addedPS) {
      entries.push_back({JoinPrimPath(parent, name), "prim_added", {}});
    }
    for (const auto &name : kv.second.deletedPS) {
      entries.push_back({JoinPrimPath(parent, name), "prim_deleted", {}});
    }
    for (const auto &m : kv.second.modifiedDetails) {
      entries.push_back({JoinPrimPath(parent, m.name), "prim_modified", m.reasons});
    }
  }
  for (const auto &kv : d.propDiffs) {
    const std::string &prim = kv.first;
    for (const auto &name : kv.second.addedProps) {
      entries.push_back({prim + "." + name, "prop_added", {}});
    }
    for (const auto &name : kv.second.deletedProps) {
      entries.push_back({prim + "." + name, "prop_deleted", {}});
    }
    for (const auto &m : kv.second.modifiedPropDetails) {
      entries.push_back({prim + "." + m.name, "prop_modified", m.reasons});
    }
  }
}

bool PassesFilter(const PathEntry &e, const std::string &f_reason,
                  const std::string &f_substr, const std::string &f_kind) {
  if (!f_kind.empty() && e.kind.find(f_kind) == std::string::npos) return false;
  if (!f_substr.empty() && e.path.find(f_substr) == std::string::npos)
    return false;
  return MatchesReason(e.reasons, f_reason);
}

// The owning prim path for an entry (strip a trailing ".prop" for prop_*).
std::string PrimPathOfEntry(const PathEntry &e) {
  if (e.kind.rfind("prop", 0) == 0) {
    const auto pos = e.path.find_last_of('.');
    if (pos != std::string::npos) return e.path.substr(0, pos);
  }
  return e.path;
}

// Emit a {key,count} group array sorted by count desc into out[arr_name].
void EmitGroups(const std::map<std::string, size_t> &tally,
                nlohmann::json &out) {
  std::vector<std::pair<std::string, size_t>> v(tally.begin(), tally.end());
  std::sort(v.begin(), v.end(),
            [](const std::pair<std::string, size_t> &a,
               const std::pair<std::string, size_t> &b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });
  out = nlohmann::json::array();
  for (const auto &g : v) {
    out.push_back({{"key", g.first}, {"count", g.second}});
  }
}

}  // namespace

void DiffComputePaths(const DiffSession &d, const nlohmann::json &filter,
                      nlohmann::json &out) {
  const std::string f_reason = filter.value("reason", std::string());
  const std::string f_substr = filter.value("path_substr", std::string());
  const std::string f_kind = filter.value("kind", std::string());
  const std::string group_by = filter.value("group_by", std::string());
  const size_t offset = filter.value("offset", size_t(0));
  const size_t limit = filter.value("limit", size_t(200));

  std::vector<PathEntry> entries;
  CollectEntries(d, entries);

  // Apply filters.
  std::vector<PathEntry> filtered;
  for (auto &e : entries) {
    if (!PassesFilter(e, f_reason, f_substr, f_kind)) continue;
    filtered.push_back(std::move(e));
  }

  // Grouped/aggregated view (no per-path listing).
  if (!group_by.empty()) {
    std::map<std::string, size_t> tally;
    for (const auto &e : filtered) {
      if (group_by == "reason") {
        if (e.reasons.empty()) {
          tally[e.kind]++;  // added/deleted have no reasons; bucket by kind
        } else {
          for (const auto &r : e.reasons) tally[r]++;
        }
      } else if (group_by == "property") {
        const auto pos = e.path.find_last_of('.');
        tally[pos == std::string::npos ? "(prim)" : e.path.substr(pos + 1)]++;
      } else if (group_by == "prim") {
        tally[PrimPathOfEntry(e)]++;
      } else {  // "kind" or anything else
        tally[e.kind]++;
      }
    }
    out["group_by"] = group_by;
    out["total"] = filtered.size();
    EmitGroups(tally, out["groups"]);
    return;
  }

  std::sort(filtered.begin(), filtered.end(),
            [](const PathEntry &a, const PathEntry &b) {
              if (a.path != b.path) return a.path < b.path;
              return a.kind < b.kind;
            });

  out["total"] = filtered.size();
  out["offset"] = offset;
  out["limit"] = limit;
  out["paths"] = nlohmann::json::array();
  for (size_t i = offset; i < filtered.size() && i < offset + limit; i++) {
    nlohmann::json j;
    j["path"] = filtered[i].path;
    j["kind"] = filtered[i].kind;
    if (!filtered[i].reasons.empty()) j["reasons"] = filtered[i].reasons;
    out["paths"].push_back(j);
  }
}

void DiffComputeTree(const DiffSession &d, const std::string &root, int depth,
                     nlohmann::json &out) {
  auto segs = [](const std::string &p) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < p.size()) {
      if (p[i] == '/') { ++i; continue; }
      size_t j = p.find('/', i);
      if (j == std::string::npos) j = p.size();
      v.push_back(p.substr(i, j - i));
      i = j;
    }
    return v;
  };
  auto build = [](const std::vector<std::string> &s, size_t n) {
    if (n == 0) return std::string("/");
    std::string out_path;
    for (size_t i = 0; i < n; ++i) { out_path += "/"; out_path += s[i]; }
    return out_path;
  };

  const std::vector<std::string> rootSegs = segs(root);
  const size_t rootLevel = rootSegs.size();
  const size_t maxAbsLevel = rootLevel + (depth < 0 ? 0 : size_t(depth));

  std::vector<PathEntry> entries;
  CollectEntries(d, entries);

  std::map<std::string, size_t> tally;
  for (const auto &e : entries) {
    const std::vector<std::string> ps = segs(PrimPathOfEntry(e));
    if (ps.size() < rootLevel) continue;
    bool under = true;  // P must be at/under root
    for (size_t i = 0; i < rootLevel; ++i) {
      if (ps[i] != rootSegs[i]) { under = false; break; }
    }
    if (!under) continue;
    const size_t top = std::min(ps.size(), maxAbsLevel);
    for (size_t L = rootLevel; L <= top; ++L) {
      tally[build(ps, L)]++;
    }
  }

  // Rollup nodes sorted by change count desc (most-changed subtrees first).
  std::vector<std::pair<std::string, size_t>> nodes(tally.begin(), tally.end());
  std::sort(nodes.begin(), nodes.end(),
            [](const std::pair<std::string, size_t> &a,
               const std::pair<std::string, size_t> &b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });
  out["root"] = root.empty() ? "/" : root;
  out["depth"] = depth;
  out["nodes"] = nlohmann::json::array();
  constexpr size_t kMaxNodes = 300;
  for (size_t i = 0; i < nodes.size() && i < kMaxNodes; ++i) {
    out["nodes"].push_back({{"path", nodes[i].first},
                            {"changes", nodes[i].second}});
  }
}

void DiffComputePrim(const DiffSession &d, const std::string &path,
                     nlohmann::json &out) {
  out["path"] = path;

  // This prim's own structural/metadata reasons live under the PARENT path,
  // keyed by leaf name.
  const std::string parent = ParentPath(path);
  const std::string leaf = LeafName(path);
  out["primReasons"] = nlohmann::json::array();
  auto pit = d.psDiffs.find(parent);
  if (pit != d.psDiffs.end()) {
    for (const auto &m : pit->second.modifiedDetails) {
      if (m.name == leaf) {
        out["primReasons"] = m.reasons;
        break;
      }
    }
  }

  // Added/deleted children of this prim.
  out["childrenAdded"] = nlohmann::json::array();
  out["childrenDeleted"] = nlohmann::json::array();
  auto cit = d.psDiffs.find(path);
  if (cit != d.psDiffs.end()) {
    out["childrenAdded"] = cit->second.addedPS;
    out["childrenDeleted"] = cit->second.deletedPS;
  }

  // Property changes on this prim.
  out["propsAdded"] = nlohmann::json::array();
  out["propsDeleted"] = nlohmann::json::array();
  out["propsModified"] = nlohmann::json::array();
  auto ppit = d.propDiffs.find(path);
  if (ppit != d.propDiffs.end()) {
    out["propsAdded"] = ppit->second.addedProps;
    out["propsDeleted"] = ppit->second.deletedProps;
    for (const auto &m : ppit->second.modifiedPropDetails) {
      // Center long values on the first difference so a shared prefix (e.g.
      // asset paths) does not hide what changed.
      auto pr = next::CenterValuePairForDiff(m.lhs, m.rhs);
      out["propsModified"].push_back({{"name", m.name},
                                      {"left", pr.first},
                                      {"right", pr.second},
                                      {"reasons", m.reasons}});
    }
  }
}

// ===========================================================================
// Tool handlers
// ===========================================================================
bool DiffOpen(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  if (!args.contains("left") || !args["left"].is_object() ||
      !args.contains("right") || !args["right"].is_object()) {
    err = "diff_open requires 'left' and 'right' objects "
          "({path|data|uuid, name?})";
    return false;
  }

  auto session = std::unique_ptr<DiffSession>(new DiffSession());
  session->opts = BuildDiffOptions(args);

  const bool flatten_sides = args.value("flatten", false);
  if (!LoadDiffSide(ctx, args["left"], flatten_sides, &session->left,
                    &session->left_name, err)) {
    err = "left: " + err;
    return false;
  }
  if (!LoadDiffSide(ctx, args["right"], flatten_sides, &session->right,
                    &session->right_name, err)) {
    err = "right: " + err;
    return false;
  }
  // Explicit names override.
  session->left_name = args["left"].value("name", session->left_name);
  session->right_name = args["right"].value("name", session->right_name);

  const bool flatten = args.value("flatten", false);
  next::Diff(session->left, session->right, session->psDiffs, session->propDiffs,
       session->opts, &session->layerMetaDiff);

  ctx.diff = std::move(session);

  DiffComputeSummary(*ctx.diff, result);
  result["flattened"] = flatten;
  result["ulps"] = ctx.diff->opts.doubleUlps;
  result["compareMetadata"] = ctx.diff->opts.compareMetadata;
  return true;
}

bool DiffSummary(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err) {
  (void)args;
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  DiffComputeSummary(*ctx.diff, result);
  return true;
}

bool DiffPaths(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
               std::string &err) {
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  DiffComputePaths(*ctx.diff, args, result);
  return true;
}

bool DiffPrim(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "diff_prim requires a 'path' string";
    return false;
  }
  DiffComputePrim(*ctx.diff, args["path"].get<std::string>(), result);
  return true;
}

bool DiffTree(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  std::string root = args.value("path", std::string("/"));
  if (root.empty()) root = "/";
  const int depth = args.value("depth", 2);
  DiffComputeTree(*ctx.diff, root, depth, result);
  return true;
}

bool DiffText(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  (void)args;
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  result["text"] = next::DiffToText(ctx.diff->left, ctx.diff->right,
                              ctx.diff->left_name, ctx.diff->right_name,
                              ctx.diff->opts);
  return true;
}

bool DiffJson(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  (void)args;
  if (!ctx.diff) {
    err = "No diff loaded. Call diff_open first.";
    return false;
  }
  const std::string s = next::DiffToJSON(ctx.diff->left, ctx.diff->right,
                                   ctx.diff->left_name, ctx.diff->right_name,
                                   ctx.diff->opts);
  result["json"] = nlohmann::json::parse(s, nullptr, false);
  if (result["json"].is_discarded()) result["json"] = s;  // fallback: raw
  return true;
}

}  // namespace mcp
}  // namespace lightusd
