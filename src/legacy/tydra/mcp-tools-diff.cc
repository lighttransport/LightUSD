// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// mcp-tools-diff.cc - MCP tools for value-level USD layer diffing.
//
#include "mcp-tools-diff.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "../lightusd.hh"
#include "../composition.hh"
#include "../asset-resolution.hh"
#include "str-util.hh"
#include "diff-and-compare.hh"

namespace lightusd { namespace legacy {
namespace tydra {
namespace mcp {

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

std::string DirOf(const std::string &path) {
  const auto pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

// Flatten a layer with the same composition sequence lusdcat uses
// (subLayers, then references/payload/inherits/variant to a fixed point).
bool FlattenLayerForDiff(lightusd::legacy::Layer &&in, const std::string &base_dir,
                         lightusd::legacy::Layer *out, std::string *warn,
                         std::string *err) {
  lightusd::legacy::AssetResolutionResolver resolver;
  if (!base_dir.empty()) {
    resolver.set_search_paths({base_dir});
  }

  lightusd::legacy::Layer src = std::move(in);

  {
    lightusd::legacy::Layer composited;
    if (!lightusd::legacy::CompositeSublayers(resolver, src, &composited, warn, err)) {
      return false;
    }
    src = std::move(composited);
  }

  constexpr int kMaxIteration = 16;
  for (int i = 0; i < kMaxIteration; i++) {
    bool any = false;
    if (src.check_unresolved_references()) {
      any = true;
      lightusd::legacy::Layer composited;
      if (!lightusd::legacy::CompositeReferencesInPlace(
              resolver, std::make_unique<lightusd::legacy::Layer>(std::move(src)),
              &composited, warn, err)) {
        return false;
      }
      src = std::move(composited);
    }
    if (src.check_unresolved_payload()) {
      any = true;
      lightusd::legacy::Layer composited;
      if (!lightusd::legacy::CompositePayloadInPlace(
              resolver, std::make_unique<lightusd::legacy::Layer>(std::move(src)),
              &composited, warn, err)) {
        return false;
      }
      src = std::move(composited);
    }
    if (src.check_unresolved_inherits()) {
      any = true;
      lightusd::legacy::Layer composited;
      if (!lightusd::legacy::CompositeInherits(src, &composited, warn, err)) {
        return false;
      }
      src = std::move(composited);
    }
    if (src.check_unresolved_variant()) {
      any = true;
      // AOUSD Core Spec 10.3.2.5: defer variant composition until references and
      // payloads are resolved (this loop always resolves both).
      if (!lightusd::legacy::ShouldDeferVariantComposition(src)) {
        lightusd::legacy::Layer composited;
        if (!lightusd::legacy::CompositeVariant(src, &composited, warn, err)) {
          return false;
        }
        src = std::move(composited);
      }
    }
    if (!any) break;
  }

  *out = std::move(src);
  return true;
}

// Load one diff side from {path | data(base64) | uuid}. Sets default_name and,
// for path inputs, base_dir (used by optional flatten). Returns false on error.
bool LoadDiffSide(Context &ctx, const nlohmann::json &side, lightusd::legacy::Layer *out,
                  std::string *default_name, std::string *base_dir,
                  std::string &err) {
  if (side.contains("uuid") && side["uuid"].is_string()) {
    const std::string uuid = side["uuid"].get<std::string>();
    auto it = ctx.layers.find(uuid);
    if (it == ctx.layers.end()) {
      err = "No loaded layer with uuid: " + uuid;
      return false;
    }
    *out = it->second.layer;  // copy
    *default_name = it->second.name.empty() ? uuid : it->second.name;
    return true;
  }

  std::string warn;
  if (side.contains("path") && side["path"].is_string()) {
    const std::string path = side["path"].get<std::string>();
    if (!lightusd::legacy::LoadLayerFromFile(path, out, &warn, &err)) {
      return false;
    }
    *default_name = path;
    *base_dir = DirOf(path);
    return true;
  }

  if (side.contains("data") && side["data"].is_string()) {
    const std::string binary =
        lightusd::legacy::base64_decode(side["data"].get<std::string>());
    const std::string name = side.value("name", std::string("memory.usd"));
    if (!lightusd::legacy::LoadLayerFromMemory(
            reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
            name, out, &warn, &err)) {
      return false;
    }
    *default_name = name;
    return true;
  }

  err = "diff side must specify one of: 'path', 'data' (base64), or 'uuid'";
  return false;
}

DiffOptions BuildDiffOptions(const nlohmann::json &args) {
  DiffOptions opts;
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
      auto pr = CenterValuePairForDiff(m.lhs, m.rhs);
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

  std::string left_base, right_base;
  if (!LoadDiffSide(ctx, args["left"], &session->left, &session->left_name,
                    &left_base, err)) {
    return false;
  }
  if (!LoadDiffSide(ctx, args["right"], &session->right, &session->right_name,
                    &right_base, err)) {
    return false;
  }
  // Explicit names override.
  session->left_name = args["left"].value("name", session->left_name);
  session->right_name = args["right"].value("name", session->right_name);

  const bool flatten = args.value("flatten", false);
  if (flatten) {
    std::string warn;
    lightusd::legacy::Layer fl, fr;
    if (!FlattenLayerForDiff(std::move(session->left), left_base, &fl, &warn,
                             &err)) {
      err = "flatten(left) failed: " + err;
      return false;
    }
    if (!FlattenLayerForDiff(std::move(session->right), right_base, &fr, &warn,
                             &err)) {
      err = "flatten(right) failed: " + err;
      return false;
    }
    session->left = std::move(fl);
    session->right = std::move(fr);
  }

  Diff(session->left, session->right, session->psDiffs, session->propDiffs,
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
  result["text"] = DiffToText(ctx.diff->left, ctx.diff->right,
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
  const std::string s = DiffToJSON(ctx.diff->left, ctx.diff->right,
                                   ctx.diff->left_name, ctx.diff->right_name,
                                   ctx.diff->opts);
  result["json"] = nlohmann::json::parse(s, nullptr, false);
  if (result["json"].is_discarded()) result["json"] = s;  // fallback: raw
  return true;
}

}  // namespace mcp
}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd
