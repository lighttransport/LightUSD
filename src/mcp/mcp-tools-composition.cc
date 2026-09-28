// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Composition-arc and variant tools of the next MCP server. Edits target the
// prim's spec in the session stage (or the loaded layer that holds it).
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/stage/stage.hh"

namespace lightusd {
namespace mcp {

namespace {

enum class Arc { Reference, Payload, Inherit, Specialize };

std::vector<std::string>& ArcList(next::PrimSpecMeta& m, Arc arc) {
  switch (arc) {
    case Arc::Reference: return m.references;
    case Arc::Payload: return m.payloads;
    case Arc::Inherit: return m.inherits;
    default: return m.specializes;
  }
}

const next::ArcEdit* ArcEdits(const next::PrimSpecMeta& m, Arc arc) {
  const next::ArcListOpEdits* e = m.arc_edits();
  if (!e) return nullptr;
  switch (arc) {
    case Arc::Reference: return &e->references;
    case Arc::Payload: return &e->payloads;
    case Arc::Inherit: return &e->inherits;
    default: return &e->specializes;
  }
}

// "@asset@</prim>?layerOffset=o:s" -> fields.
json ArcToJSON(const std::string& arc, const char* qualifier) {
  json j;
  std::string asset, prim;
  double offset = 0.0, scale = 1.0;
  size_t i = 0;
  if (arc.compare(0, 3, "@@@") == 0) {
    const size_t e = arc.find("@@@", 3);
    asset = arc.substr(3, e == std::string::npos ? std::string::npos : e - 3);
    i = e == std::string::npos ? arc.size() : e + 3;
  } else if (!arc.empty() && arc[0] == '@') {
    const size_t e = arc.find('@', 1);
    asset = arc.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    i = e == std::string::npos ? arc.size() : e + 1;
  }
  if (i < arc.size() && arc[i] == '<') {
    const size_t e = arc.find('>', i);
    prim = arc.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
    i = e == std::string::npos ? arc.size() : e + 1;
  }
  const std::string lo = "?layerOffset=";
  const size_t lo_pos = arc.find(lo, i);
  if (lo_pos != std::string::npos) {
    const std::string v = arc.substr(lo_pos + lo.size());
    const size_t colon = v.find(':');
    offset = std::strtod(v.c_str(), nullptr);
    if (colon != std::string::npos) scale = std::strtod(v.c_str() + colon + 1, nullptr);
  }
  j["asset_path"] = asset;
  j["prim_path"] = prim;
  j["offset"] = offset;
  j["scale"] = scale;
  if (qualifier) j["listOp"] = qualifier;
  return j;
}

json ListArcs(const next::PrimSpecMeta& m, Arc arc) {
  json out = json::array();
  const next::ArcEdit* edits = ArcEdits(m, arc);
  const bool qualified = edits && edits->has_qualifiers();
  if (!qualified) {
    for (const std::string& a : ArcList(const_cast<next::PrimSpecMeta&>(m), arc)) {
      out.push_back(ArcToJSON(a, nullptr));
    }
    return out;
  }
  const std::pair<const char*, const std::vector<std::string>*> lists[] = {
      {"prepend", &edits->prepended}, {"append", &edits->appended},
      {"add", &edits->added}, {"delete", &edits->deleted},
      {"order", &edits->ordered}};
  for (const auto& l : lists) {
    for (const std::string& a : *l.second) out.push_back(ArcToJSON(a, l.first));
  }
  return out;
}

next::PrimSpec* MutableSpec(lightusd_stage* stage, const std::string& path,
                            std::string* err) {
  next::Layer* layer = lightusd_internal::MutableNativeRootLayer(stage);
  next::PrimSpec* spec = layer ? layer->prim_at_path_mutable(path) : nullptr;
  if (!spec) *err = "Prim spec not found in the editable root layer: " + path;
  return spec;
}

const next::PrimSpec* ReadSpec(const PrimSite& site, const std::string& path) {
  const next::Stage* s = lightusd_internal::BorrowNativeStage(site.stage);
  const next::Layer* layer = s ? s->GetRootLayer() : nullptr;
  return layer ? layer->prim_at_path(path) : nullptr;
}

bool RequirePath(const json& args, const char* key, std::string* out,
                 std::string* err) {
  if (!args.contains(key) || !args[key].is_string()) {
    *err = std::string("Missing '") + key + "' argument";
    return false;
  }
  *out = args[key].get<std::string>();
  return true;
}

bool AddArc(Context& ctx, const json& args, json& result, std::string& err,
            Arc arc) {
  std::string path;
  if (!RequirePath(args, "path", &path, &err)) return false;
  std::string asset, prim_path;
  if (arc == Arc::Reference || arc == Arc::Payload) {
    if (!RequirePath(args, "asset_path", &asset, &err)) return false;
    prim_path = args.value("prim_path", std::string());
  } else if (!RequirePath(args, "prim_path", &prim_path, &err)) {
    return false;
  }
  const double offset = args.value("offset", 0.0);
  const double scale = args.value("scale", 1.0);
  if (!std::isfinite(offset) || !std::isfinite(scale)) {
    err = "offset and scale must be finite";
    return false;
  }
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  next::PrimSpec* spec = MutableSpec(site.stage, path, &err);
  if (!spec) return false;
  std::string text;
  if (!asset.empty()) text += "@" + asset + "@";
  if (!prim_path.empty()) text += "<" + prim_path + ">";
  if ((arc == Arc::Reference || arc == Arc::Payload) &&
      (offset != 0.0 || scale != 1.0)) {
    text += "?layerOffset=" + std::to_string(offset) + ":" + std::to_string(scale);
  }
  next::PrimSpecMeta& m = spec->meta();
  const next::ArcEdit* edits = ArcEdits(m, arc);
  if (edits && edits->has_qualifiers()) {
    // Keep the authored list-op form: add to its prepend list.
    next::ArcListOpEdits& e = m.ensure_arc_edits();
    next::ArcEdit& target = arc == Arc::Reference ? e.references
                            : arc == Arc::Payload ? e.payloads
                            : arc == Arc::Inherit ? e.inherits
                                                  : e.specializes;
    target.prepended.push_back(text);
  }
  ArcList(m, arc).push_back(text);
  result["success"] = true;
  result["path"] = path;
  if (!site.layer.empty()) result["layer"] = site.layer;
  return true;
}

bool ListArcTool(Context& ctx, const json& args, json& result, std::string& err,
                 Arc arc, const char* key) {
  std::string path;
  if (!RequirePath(args, "path", &path, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  const next::PrimSpec* spec = ReadSpec(site, path);
  json arcs = spec ? ListArcs(spec->meta(), arc) : json::array();
  result["path"] = path;
  result["layer"] = site.layer;
  result["count"] = arcs.size();
  result[key] = std::move(arcs);
  return true;
}

std::string SvString(lightusd_sv sv) {
  return std::string(sv.data ? sv.data : "", sv.len);
}

}  // namespace

bool ReferenceAdd(Context& ctx, const json& args, json& result, std::string& err) {
  if (!AddArc(ctx, args, result, err, Arc::Reference)) return false;
  result["reference"] = {{"asset_path", args.value("asset_path", std::string())},
                         {"prim_path", args.value("prim_path", std::string())},
                         {"offset", args.value("offset", 0.0)},
                         {"scale", args.value("scale", 1.0)}};
  return true;
}

bool ReferenceList(Context& ctx, const json& args, json& result, std::string& err) {
  return ListArcTool(ctx, args, result, err, Arc::Reference, "references");
}

bool ReferenceClear(Context& ctx, const json& args, json& result, std::string& err) {
  std::string path;
  if (!RequirePath(args, "path", &path, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  next::PrimSpec* spec = MutableSpec(site.stage, path, &err);
  if (!spec) return false;
  spec->meta().references.clear();
  if (spec->meta().arc_edits()) spec->meta().ensure_arc_edits().references = {};
  result["success"] = true;
  result["path"] = path;
  return true;
}

bool PayloadAdd(Context& ctx, const json& args, json& result, std::string& err) {
  return AddArc(ctx, args, result, err, Arc::Payload);
}

bool PayloadList(Context& ctx, const json& args, json& result, std::string& err) {
  return ListArcTool(ctx, args, result, err, Arc::Payload, "payloads");
}

bool InheritAdd(Context& ctx, const json& args, json& result, std::string& err) {
  if (!AddArc(ctx, args, result, err, Arc::Inherit)) return false;
  result["inheritPrimPath"] = args["prim_path"];
  return true;
}

bool SpecializeAdd(Context& ctx, const json& args, json& result, std::string& err) {
  if (!AddArc(ctx, args, result, err, Arc::Specialize)) return false;
  result["specializePrimPath"] = args["prim_path"];
  return true;
}

bool VariantListSets(Context& ctx, const json& args, json& result, std::string& err) {
  std::string path;
  if (!RequirePath(args, "path", &path, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  const lightusd_prim p = lightusd_stage_prim_at_path(site.stage, path.c_str());
  json sets = json::array();
  const size_t n = lightusd_prim_variant_set_count(p);
  for (size_t i = 0; i < n; ++i) {
    const std::string name = SvString(lightusd_prim_variant_set_name(p, i));
    json s;
    s["name"] = name;
    json variants = json::array();
    const size_t vn = lightusd_variant_count(p, name.c_str());
    for (size_t k = 0; k < vn; ++k) {
      variants.push_back(SvString(lightusd_variant_name(p, name.c_str(), k)));
    }
    s["variants"] = std::move(variants);
    const std::string sel = SvString(lightusd_variant_selection(p, name.c_str()));
    s["selection"] = sel.empty() ? json(nullptr) : json(sel);
    sets.push_back(std::move(s));
  }
  result["path"] = path;
  result["count"] = sets.size();
  result["variantSets"] = std::move(sets);
  return true;
}

bool VariantGetSelection(Context& ctx, const json& args, json& result, std::string& err) {
  std::string path, set;
  if (!RequirePath(args, "path", &path, &err)) return false;
  if (!RequirePath(args, "variant_set", &set, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  const lightusd_prim p = lightusd_stage_prim_at_path(site.stage, path.c_str());
  const std::string sel = SvString(lightusd_variant_selection(p, set.c_str()));
  result["path"] = path;
  result["variant_set"] = set;
  if (sel.empty()) {
    result["selection"] = nullptr;
    result["note"] = "No selection set for this variant set";
  } else {
    result["selection"] = sel;
  }
  return true;
}

bool VariantSetSelection(Context& ctx, const json& args, json& result, std::string& err) {
  std::string path, set, variant;
  if (!RequirePath(args, "path", &path, &err)) return false;
  if (!RequirePath(args, "variant_set", &set, &err)) return false;
  if (!RequirePath(args, "variant", &variant, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  if (lightusd_prim_set_variant_selection(site.stage, path.c_str(), set.c_str(),
                                          variant.c_str()) != LIGHTUSD_OK) {
    err = std::string("set variant selection: ") + lightusd_last_error();
    return false;
  }
  result["success"] = true;
  result["path"] = path;
  result["variant_set"] = set;
  result["variant"] = variant;
  return true;
}

bool VariantDefine(Context& ctx, const json& args, json& result, std::string& err) {
  std::string path, set, variant;
  if (!RequirePath(args, "path", &path, &err)) return false;
  if (!RequirePath(args, "variant_set", &set, &err)) return false;
  if (!RequirePath(args, "variant_name", &variant, &err)) return false;
  PrimSite site;
  if (!FindPrimSite(ctx, path, &site, &err)) return false;
  if (lightusd_prim_add_variant_set(site.stage, path.c_str(), set.c_str()) != LIGHTUSD_OK ||
      lightusd_prim_add_variant(site.stage, path.c_str(), set.c_str(),
                                variant.c_str()) != LIGHTUSD_OK) {
    err = std::string("define variant: ") + lightusd_last_error();
    return false;
  }
  result["success"] = true;
  result["path"] = path;
  result["variant_set"] = set;
  result["variant_name"] = variant;
  return true;
}

}  // namespace mcp
}  // namespace lightusd
