#include "mcp-tools-composition.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "../lightusd.hh"
#include "value-to-json.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

// ===========================================================================
// Composition arcs are authored at the Layer/PrimSpec level, not the
// composed Stage level. These tools operate on uncomposed layers stored
// in the Context.
//
// For each composition operation:
//   1. Find the Layer containing the target prim
//   2. Find the PrimSpec at the given path
//   3. Use the composition arc API to modify it
// ===========================================================================

namespace {

// Helper: find a PrimSpec by path in the context's layers
const PrimSpec *FindPrimSpecInLayers(
    const lightusd::HashMap<std::string, USDLayer> &layers,
    const std::string &path_str, std::string &layer_name,
    std::string &err) {
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return nullptr;
  }

  for (const auto &[uuid, usd_layer] : layers) {
    const PrimSpec *ps = nullptr;
    std::string local_err;
    if (usd_layer.layer.find_primspec_at(path, &ps, &local_err)) {
      layer_name = usd_layer.name;
      return ps;
    }
  }

  err = "Prim not found in any loaded layer: " + path_str;
  return nullptr;
}

// Helper: find mutable PrimSpec by path
PrimSpec *FindMutablePrimSpecInLayers(
    lightusd::HashMap<std::string, USDLayer> &layers,
    const std::string &path_str, std::string &layer_uuid,
    std::string &err) {
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return nullptr;
  }

  for (auto &[uuid, usd_layer] : layers) {
    const PrimSpec *cps = nullptr;
    std::string local_err;
    if (usd_layer.layer.find_primspec_at(path, &cps, &local_err)) {
      layer_uuid = uuid;
      return const_cast<PrimSpec*>(cps);
    }
  }

  err = "Prim not found in any loaded layer: " + path_str;
  return nullptr;
}

} // namespace

namespace {

void AddToListOp(std::vector<std::pair<ListEditQual, std::vector<Reference>>> &v,
                  const Reference &ref) {
  v.push_back(std::make_pair(ListEditQual::ResetToExplicit, std::vector<Reference>{ref}));
}

void AddToListOp(std::vector<std::pair<ListEditQual, std::vector<Payload>>> &v,
                  const Payload &p) {
  v.push_back(std::make_pair(ListEditQual::ResetToExplicit, std::vector<Payload>{p}));
}

void AddToListOp(std::vector<std::pair<ListEditQual, std::vector<Path>>> &v,
                  const Path &p) {
  v.push_back(std::make_pair(ListEditQual::ResetToExplicit, std::vector<Path>{p}));
}

} // namespace

// ===========================================================================
// reference_add
// ===========================================================================
bool ReferenceAdd(Context &ctx, const nlohmann::json &args,
                  nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("asset_path") || !args["asset_path"].is_string()) {
    err = "Missing 'asset_path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string asset_path = args["asset_path"].get<std::string>();
  std::string prim_path = args.value("prim_path", std::string());
  double offset = args.value("offset", 0.0);
  double scale = args.value("scale", 1.0);

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  if (!metas.references) {
    metas.references = std::vector<std::pair<ListEditQual, std::vector<Reference>>>();
  }

  Reference ref;
  ref.asset_path = value::AssetPath(asset_path);
  if (!prim_path.empty()) {
    ref.prim_path = Path(prim_path, "");
  }
  ref.layerOffset._offset = offset;
  ref.layerOffset._scale = scale;

  AddToListOp(metas.references.value(), ref);

  result["success"] = true;
  result["path"] = path_str;
  result["reference"] = {
      {"asset_path", asset_path},
      {"prim_path", prim_path},
      {"offset", offset},
      {"scale", scale},
  };
  return true;
}

// ===========================================================================
// reference_list
// ===========================================================================
bool ReferenceList(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string layer_name;
  const PrimSpec *ps = FindPrimSpecInLayers(ctx.layers, path_str,
                                             layer_name, err);
  if (!ps) return false;

  const auto &metas = ps->metas();
  nlohmann::json refs = nlohmann::json::array();

  if (metas.references.has_value()) {
    for (const auto &ref_pair : metas.references.value()) {
      for (const auto &ref : ref_pair.second) {
        nlohmann::json j;
        j["asset_path"] = ref.asset_path.GetAssetPath();
        j["prim_path"] = ref.prim_path.full_path_name();
        j["offset"] = ref.layerOffset._offset;
        j["scale"] = ref.layerOffset._scale;
        refs.push_back(j);
      }
    }
  }

  result["path"] = path_str;
  result["layer"] = layer_name;
  result["references"] = refs;
  result["count"] = refs.size();
  return true;
}

// ===========================================================================
// reference_clear
// ===========================================================================
bool ReferenceClear(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  ps->metas().references.reset();
  result["success"] = true;
  result["path"] = path_str;
  return true;
}

// ===========================================================================
// payload_add
// ===========================================================================
bool PayloadAdd(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("asset_path") || !args["asset_path"].is_string()) {
    err = "Missing 'asset_path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string asset_path = args["asset_path"].get<std::string>();
  std::string prim_path = args.value("prim_path", std::string());
  double offset = args.value("offset", 0.0);
  double scale = args.value("scale", 1.0);

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  // Payload type is std::vector<std::pair<ListEditQual, std::vector<Payload>>>
  if (!metas.payload) {
    metas.payload = std::vector<std::pair<ListEditQual, std::vector<Payload>>>();
  }

  Payload p;
  p.asset_path = value::AssetPath(asset_path);
  if (!prim_path.empty()) {
    p.prim_path = Path(prim_path, "");
  }
  p.layerOffset._offset = offset;
  p.layerOffset._scale = scale;

  AddToListOp(metas.payload.value(), p);

  result["success"] = true;
  result["path"] = path_str;
  return true;
}

// ===========================================================================
// payload_list
// ===========================================================================
bool PayloadList(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string layer_name;
  const PrimSpec *ps = FindPrimSpecInLayers(ctx.layers, path_str,
                                             layer_name, err);
  if (!ps) return false;

  const auto &metas = ps->metas();
  nlohmann::json payloads = nlohmann::json::array();
  if (metas.payload.has_value()) {
    for (const auto &p_pair : metas.payload.value()) {
      for (const auto &p : p_pair.second) {
        nlohmann::json j;
        j["asset_path"] = p.asset_path.GetAssetPath();
        j["prim_path"] = p.prim_path.full_path_name();
        payloads.push_back(j);
      }
    }
  }

  result["path"] = path_str;
  result["payloads"] = payloads;
  result["count"] = payloads.size();
  return true;
}

// ===========================================================================
// inherit_add
// ===========================================================================
bool InheritAdd(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("prim_path") || !args["prim_path"].is_string()) {
    err = "Missing 'prim_path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string inherit_prim_path = args["prim_path"].get<std::string>();

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  if (!metas.inherits) {
    metas.inherits = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
  }

  Path inherit_path(inherit_prim_path, "");
  AddToListOp(metas.inherits.value(), inherit_path);

  result["success"] = true;
  result["path"] = path_str;
  result["inheritPrimPath"] = inherit_prim_path;
  return true;
}

// ===========================================================================
// specialize_add
// ===========================================================================
bool SpecializeAdd(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("prim_path") || !args["prim_path"].is_string()) {
    err = "Missing 'prim_path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string spec_prim_path = args["prim_path"].get<std::string>();

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  if (!metas.specializes) {
    metas.specializes = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
  }

  Path spec_path(spec_prim_path, "");
  AddToListOp(metas.specializes.value(), spec_path);

  result["success"] = true;
  return true;
}

// ===========================================================================
// variant_list_sets
// ===========================================================================
bool VariantListSets(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string layer_name;
  const PrimSpec *ps = FindPrimSpecInLayers(ctx.layers, path_str,
                                             layer_name, err);
  if (!ps) return false;

  const auto &metas = ps->metas();

  // Check variant sets from PrimSpec metas
  nlohmann::json sets = nlohmann::json::object();

  // Also check VariantSets stored in the PrimSpec level
  if (metas.variantSets.has_value()) {
    for (const auto &vs_pair : metas.variantSets.value()) {
      for (const auto &vs_name : vs_pair.second) {
        sets[vs_name] = nlohmann::json::object();
      }
    }
  }

  // Try to get variant selections
  if (metas.variants.has_value()) {
    for (const auto &[vs_name, sel] : metas.variants.value()) {
      if (sets.contains(vs_name)) {
        sets[vs_name]["selection"] = sel;
      } else {
        nlohmann::json vs;
        vs["selection"] = sel;
        sets[vs_name] = vs;
      }
    }
  }

  result["path"] = path_str;
  result["variantSets"] = sets;
  result["count"] = sets.size();
  return true;
}

// ===========================================================================
// variant_get_selection
// ===========================================================================
bool VariantGetSelection(Context &ctx, const nlohmann::json &args,
                         nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("variant_set") || !args["variant_set"].is_string()) {
    err = "Missing 'variant_set' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string vs_name = args["variant_set"].get<std::string>();

  std::string layer_name;
  const PrimSpec *ps = FindPrimSpecInLayers(ctx.layers, path_str,
                                             layer_name, err);
  if (!ps) return false;

  const auto &metas = ps->metas();
  if (metas.variants.has_value()) {
    auto it = metas.variants.value().find(vs_name);
    if (it != metas.variants.value().end()) {
      result["path"] = path_str;
      result["variant_set"] = vs_name;
      result["selection"] = it->second;
      return true;
    }
  }

  result["path"] = path_str;
  result["variant_set"] = vs_name;
  result["selection"] = nullptr;
  result["note"] = "No selection set for this variant set";
  return true;
}

// ===========================================================================
// variant_set_selection
// ===========================================================================
bool VariantSetSelection(Context &ctx, const nlohmann::json &args,
                         nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("variant_set") || !args["variant_set"].is_string()) {
    err = "Missing 'variant_set' argument";
    return false;
  }
  if (!args.contains("variant") || !args["variant"].is_string()) {
    err = "Missing 'variant' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string vs_name = args["variant_set"].get<std::string>();
  std::string variant = args["variant"].get<std::string>();

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  if (!metas.variants.has_value()) {
    metas.variants = std::map<std::string, std::string>();
  }
  (*metas.variants)[vs_name] = variant;

  result["success"] = true;
  result["path"] = path_str;
  result["variant_set"] = vs_name;
  result["variant"] = variant;
  return true;
}

// ===========================================================================
// variant_define
// ===========================================================================
bool VariantDefine(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("variant_set") || !args["variant_set"].is_string()) {
    err = "Missing 'variant_set' argument";
    return false;
  }
  if (!args.contains("variant_name") || !args["variant_name"].is_string()) {
    err = "Missing 'variant_name' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string vs_name = args["variant_set"].get<std::string>();
  std::string variant_name = args["variant_name"].get<std::string>();

  std::string layer_uuid;
  PrimSpec *ps = FindMutablePrimSpecInLayers(ctx.layers, path_str,
                                              layer_uuid, err);
  if (!ps) return false;

  auto &metas = ps->metas();
  if (!metas.variantSets) {
    using VSPair = std::pair<ListEditQual, std::vector<std::string>>;
    metas.variantSets = std::vector<VSPair>();
  }

  // Add variant set name if not already present
  bool found_set = false;
  for (const auto &vs_pair : metas.variantSets.value()) {
    for (const auto &name : vs_pair.second) {
      if (name == vs_name) {
        found_set = true;
        break;
      }
    }
    if (found_set) break;
  }
  if (!found_set) {
    metas.variantSets->push_back(
        std::make_pair(ListEditQual::ResetToExplicit, std::vector<std::string>{vs_name}));
  }

  // Define the variant itself (requires VariantSet API)
  // TODO: Full variant definition requires deeper API integration
  result["success"] = true;
  result["path"] = path_str;
  result["variant_set"] = vs_name;
  result["variant_name"] = variant_name;
  result["note"] = "Variant set name registered. Full variant content definition requires additional API support.";
  return true;
}

} // namespace mcp
} // namespace tydra
} // namespace lightusd
