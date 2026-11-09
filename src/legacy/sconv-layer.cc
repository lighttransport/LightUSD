// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage-converter: Layer/PrimSpec conversion.
//

#include "sconv-detail.hh"
#include "layer.hh"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {
namespace experimental {

// ============================================================================
// Layer/PrimSpec Conversion Implementation
// ============================================================================

bool CrateWriter::ConvertLayerToSpecs(const Layer& layer, std::string* err) {
  // 1. Add PseudoRoot spec with layer metadata
  // The PseudoRoot is a special prim at "/" that serves as the root of the scene
  // Layer metadata (upAxis, metersPerUnit, etc.) is stored as fields on PseudoRoot
  {
    Path root_path("/", "");  // Root path - use standard constructor
    crate::FieldValuePairVector root_fields;

    // Extract and add layer metadata from Layer.metas()
    const LayerMetas& metas = layer.metas();

    // upAxis (token: "X", "Y", or "Z")
    if (metas.upAxis.authored()) {
      Axis axis = metas.upAxis.get_value();
      std::string axis_str = lightusd::to_string(axis);  // "X", "Y", or "Z"
      crate::CrateValue axis_value;
      value::token axis_tok(axis_str);
      axis_value.Set(axis_tok);
      root_fields.push_back({"upAxis", axis_value});
    }

    // metersPerUnit (double)
    if (metas.metersPerUnit.authored()) {
      crate::CrateValue meters_value;
      meters_value.Set(metas.metersPerUnit.get_value());
      root_fields.push_back({"metersPerUnit", meters_value});
    }

    // timeCodesPerSecond (double)
    if (metas.timeCodesPerSecond.authored()) {
      crate::CrateValue time_value;
      time_value.Set(metas.timeCodesPerSecond.get_value());
      root_fields.push_back({"timeCodesPerSecond", time_value});
    }

    // framesPerSecond (double)
    if (metas.framesPerSecond.authored()) {
      crate::CrateValue fps_value;
      fps_value.Set(metas.framesPerSecond.get_value());
      root_fields.push_back({"framesPerSecond", fps_value});
    }

    // startTimeCode (double)
    if (metas.startTimeCode.authored()) {
      crate::CrateValue start_value;
      start_value.Set(metas.startTimeCode.get_value());
      root_fields.push_back({"startTimeCode", start_value});
    }

    // endTimeCode (double)
    if (metas.endTimeCode.authored()) {
      crate::CrateValue end_value;
      end_value.Set(metas.endTimeCode.get_value());
      root_fields.push_back({"endTimeCode", end_value});
    }

    // defaultPrim (token)
    if (!metas.defaultPrim.str().empty()) {
      crate::CrateValue default_prim_value;
      default_prim_value.Set(metas.defaultPrim);
      root_fields.push_back({"defaultPrim", default_prim_value});
    }

    // documentation (string)
    if (!metas.doc.value.empty()) {
      crate::CrateValue doc_value;
      doc_value.Set(metas.doc.value);
      root_fields.push_back({"documentation", doc_value});
    }

    // comment (string)
    if (!metas.comment.value.empty()) {
      crate::CrateValue comment_value;
      comment_value.Set(metas.comment.value);
      root_fields.push_back({"comment", comment_value});
    }

    // kilogramsPerUnit (double) - UsdPhysics
    if (metas.kilogramsPerUnit.authored()) {
      crate::CrateValue kg_value;
      kg_value.Set(metas.kilogramsPerUnit.get_value());
      root_fields.push_back({"kilogramsPerUnit", kg_value});
    }

    // customLayerData (dict)
    if (!metas.customLayerData.empty()) {
      crate::CrateValue custom_data_value;
      custom_data_value.Set(metas.customLayerData);
      root_fields.push_back({"customLayerData", custom_data_value});
    }

    // Unregistered layer metadata: emit as the crate UNREGISTERED_VALUE type
    // (what pxr writes for SdfUnregisteredValue), carrying the raw USDA text.
    for (const auto &kv : metas.unregisteredMetas) {
      crate::CrateValue v;
      v.SetUnregisteredValueString(kv.second);
      root_fields.push_back({kv.first, v});
    }

    if (!metas.subLayers.empty()) {
      std::vector<std::string> sublayer_paths;
      std::vector<LayerOffset> sublayer_offsets;
      sublayer_paths.reserve(metas.subLayers.size());
      sublayer_offsets.reserve(metas.subLayers.size());

      for (const SubLayer &sublayer : metas.subLayers) {
        sublayer_paths.push_back(sublayer.assetPath.GetAssetPath());
        sublayer_offsets.push_back(sublayer.layerOffset);
      }

      crate::CrateValue sublayers_value;
      sublayers_value.Set(sublayer_paths);
      root_fields.push_back({"subLayers", sublayers_value});

      // pxr's SdfLayer requires subLayerOffsets to be the same length as
      // subLayers ("Invalid sublayer index" otherwise); pxr always writes
      // identity offsets alongside.
      crate::CrateValue offsets_value;
      offsets_value.Set(sublayer_offsets);
      root_fields.push_back({"subLayerOffsets", offsets_value});
    }

    // Root `primChildren`: list the top-level prim names so readers can
    // enumerate the scene (see per-prim note below). Prefer the authored
    // ordering from layer metadata; fall back to the primspecs map.
    {
      std::vector<value::token> root_children;
      for (const auto& tok : metas.primChildren) {
        if (layer.has_primspec(tok.str())) root_children.push_back(tok);
      }
      for (const auto& item : layer.primspecs()) {
        bool listed = false;
        for (const auto& t : root_children) {
          if (t.str() == item.first) { listed = true; break; }
        }
        if (!listed) root_children.push_back(value::token(item.first));
      }
      if (!root_children.empty()) {
        crate::CrateValue pc_value;
        pc_value.Set(root_children);
        root_fields.push_back({"primChildren", pc_value});
      }
    }

    if (root_fields.empty()) {
      crate::CrateValue doc_value;
      doc_value.Set(std::string());
      root_fields.push_back({"documentation", doc_value});
    }

    // Add PseudoRoot spec
    if (!AddSpec(root_path, SpecType::PseudoRoot, root_fields, err)) {
      if (err) *err = "Failed to add PseudoRoot spec";
      return false;
    }
  }

  // 2. Convert all primspecs in the layer
  for (const auto& item : layer.primspecs()) {
    const auto& prim_name = item.first;
    const auto& primspec = item.second;
    // Each top-level primspec is a direct child of root
    Path root_path("/", "");

    if (!ConvertPrimSpecIterative(primspec, root_path, err, prim_name)) {
      if (err) *err = "Failed to convert primspec: " + prim_name + ". Error: " + (*err);
      return false;
    }
  }

  // NOTE: Sublayers are exported via layer metadata (subLayers/subLayerOffsets fields)
  // We intentionally do NOT traverse and re-export sublayer content at write time because:
  // 1. Sublayers are composition references (asset paths) to external files
  // 2. The Stage already contains merged/composed prims from all sublayers
  // 3. Re-loading sublayers would duplicate content and break on moved files
  // 4. Sublayer metadata alone is sufficient for reconstruction at load time
  // This design is correct - composition happens at LOAD time, not WRITE time.

  return true;
}

bool CrateWriter::ConvertSinglePrimSpec(
    const PrimSpec& primspec,
    const Path& parent_path,
    std::string* err) {

  // 1. Build path for this prim
  Path prim_path = primspec.name().empty()
      ? parent_path
      : parent_path.AppendPrim(primspec.name());

  // 2. Create fields for this prim spec
  crate::FieldValuePairVector fields;

  // Add specifier (def, over, class) - use Specifier enum directly, not token
  Specifier spec = primspec.specifier();
  // USD files don't allow Invalid specifier - default to Def if we get Invalid
  if (spec == Specifier::Invalid) {
    spec = Specifier::Def;
  }
  crate::CrateValue spec_value;
  spec_value.Set(spec);  // CrateValue supports Specifier directly
  fields.push_back({"specifier", spec_value});

  // Add typeName if present - use value::token type, not token index
  if (!primspec.typeName().empty()) {
    crate::CrateValue type_value;
    value::token tok(primspec.typeName());
    type_value.Set(tok);
    fields.push_back({"typeName", type_value});
  }

  // 3. Convert properties (attributes, relationships, connections)
  for (const auto& item : primspec.props()) {
    const auto& prop_name = item.first;
    const auto& prop = item.second;
    if (!ConvertPropertyToFields(prop_name, prop, prim_path, fields, err)) {
      if (err) *err = "Failed to convert property: " + prop_name + " on prim: " + prim_path.full_path_name();
      return false;
    }
  }

  // 4. Convert metadata from PrimMeta
  const PrimMeta& metas = primspec.metas();

  // Add common metadata
  if (metas.has_active()) {
    crate::CrateValue active_value;
    active_value.Set(metas.get_active());
    fields.push_back({"active", active_value});
  }

  if (metas.has_hidden()) {
    crate::CrateValue hidden_value;
    hidden_value.Set(metas.get_hidden());
    fields.push_back({"hidden", hidden_value});
  }

  if (metas.has_kind()) {
    // `kind` is a `token`-typed field. Store it as a value::token (the crate
    // writer interns it); storing the raw token *index* as a uint produces a
    // mistyped field that strict readers (incl. lightusd's own) reject.
    crate::CrateValue kind_value;
    kind_value.Set(value::token(metas.get_kind()));
    fields.push_back({"kind", kind_value});
  }

  if (metas.has_displayName()) {
    crate::CrateValue display_value;
    display_value.Set(metas.get_displayName());
    fields.push_back({"displayName", display_value});
  }

  if (metas.has_doc()) {
    crate::CrateValue doc_value;
    // get_doc() returns StringData, extract .value for the string
    doc_value.Set(metas.get_doc().value);
    fields.push_back({"documentation", doc_value});
  }

  // Add variant selection if present
  if (metas.variants) {
    const VariantSelectionMap& variant_map = metas.variants.value();
    crate::CrateValue variant_value;
    variant_value.Set(variant_map);
    fields.push_back({"variantSelection", variant_value});

    DCOUT("[ConvertPrimSpecRecursive] Added variants field with "
              << variant_map.size() << " selections");
  }

  // Add variantSetNames if present (crate format uses ListOp<String>)
  if (metas.variantSets) {
    ListOp<std::string> vs_listop;

    for (const auto& variantSets_op : metas.variantSets.value()) {
      const ListEditQual& qual = variantSets_op.first;
      const std::vector<std::string>& vs_list = variantSets_op.second;

      switch (qual) {
        case ListEditQual::ResetToExplicit:
          vs_listop.ClearAndMakeExplicit();
          vs_listop.SetExplicitItems(vs_list);
          break;
        case ListEditQual::Append:
          vs_listop.SetAppendedItems(vs_list);
          break;
        case ListEditQual::Prepend:
          vs_listop.SetPrependedItems(vs_list);
          break;
        case ListEditQual::Add:
          vs_listop.SetAddedItems(vs_list);
          break;
        case ListEditQual::Delete:
          vs_listop.SetDeletedItems(vs_list);
          break;
        default:
          vs_listop.SetPrependedItems(vs_list);
          break;
      }
    }

    crate::CrateValue variant_sets_value;
    variant_sets_value.Set(vs_listop);
    fields.push_back({"variantSetNames", variant_sets_value});

    DCOUT("[ConvertPrimSpecRecursive] Added variantSetNames field");
  }

  // Add references if present
  if (metas.references) {
    // Convert all listops to ListOp<Reference>
    ListOp<Reference> ref_listop;
    size_t total_refs = 0;

    for (const auto& ref_op : metas.references.value()) {
      const ListEditQual& qual = ref_op.first;
      const std::vector<Reference>& ref_list = ref_op.second;
      total_refs += ref_list.size();

      // Accumulate items based on ListEditQual
      switch (qual) {
        case ListEditQual::ResetToExplicit:
          ref_listop.ClearAndMakeExplicit();
          ref_listop.SetExplicitItems(ref_list);
          break;
        case ListEditQual::Append:
          ref_listop.SetAppendedItems(ref_list);
          break;
        case ListEditQual::Prepend:
          ref_listop.SetPrependedItems(ref_list);
          break;
        case ListEditQual::Add:
          ref_listop.SetAddedItems(ref_list);
          break;
        case ListEditQual::Delete:
          ref_listop.SetDeletedItems(ref_list);
          break;
        default:
          // Default to explicit
          ref_listop.ClearAndMakeExplicit();
          ref_listop.SetExplicitItems(ref_list);
          break;
      }
    }

    crate::CrateValue ref_value;
    ref_value.Set(ref_listop);
    fields.push_back({"references", ref_value});

    DCOUT("[ConvertPrimSpecRecursive] Added references field with "
              << total_refs << " references");
    (void)total_refs;
  }

  // Add payload if present
  if (metas.payload) {
    // Convert all listops to ListOp<Payload>
    ListOp<Payload> payload_listop;
    size_t total_payloads = 0;

    for (const auto& payload_op : metas.payload.value()) {
      const ListEditQual& qual = payload_op.first;
      const std::vector<Payload>& payload_list = payload_op.second;
      total_payloads += payload_list.size();

      // Accumulate items based on ListEditQual
      switch (qual) {
        case ListEditQual::ResetToExplicit:
          payload_listop.ClearAndMakeExplicit();
          payload_listop.SetExplicitItems(payload_list);
          break;
        case ListEditQual::Append:
          payload_listop.SetAppendedItems(payload_list);
          break;
        case ListEditQual::Prepend:
          payload_listop.SetPrependedItems(payload_list);
          break;
        case ListEditQual::Add:
          payload_listop.SetAddedItems(payload_list);
          break;
        case ListEditQual::Delete:
          payload_listop.SetDeletedItems(payload_list);
          break;
        default:
          // Default to explicit
          payload_listop.ClearAndMakeExplicit();
          payload_listop.SetExplicitItems(payload_list);
          break;
      }
    }

    crate::CrateValue payload_value;
    payload_value.Set(payload_listop);
    fields.push_back({"payload", payload_value});

    DCOUT("[ConvertPrimSpecRecursive] Added payload field with "
              << total_payloads << " payloads");
    (void)total_payloads;
  }

  // Add customData if present
  if (metas.has_customData()) {
    crate::CrateValue custom_data_value;
    custom_data_value.Set(metas.get_customData());
    fields.push_back({"customData", custom_data_value});
    DCOUT("[ConvertPrimSpecRecursive] Added customData with "
              << metas.get_customData().size() << " entries");
  }

  // Add unregistered metadata as the crate UNREGISTERED_VALUE type (what pxr
  // writes for SdfUnregisteredValue), carrying the raw USDA text.
  for (const auto &item : metas.unregisteredMetas) {
    crate::CrateValue unreg_value;
    unreg_value.SetUnregisteredValueString(item.second);
    fields.push_back({item.first, unreg_value});
  }

  // Add apiSchemas if present
  if (metas.has_apiSchemas()) {
    const APISchemas& api_schemas = metas.get_apiSchemas();

    // Helper: an op's items (schemaName[, instanceName]) -> tokens.
    auto toTokens = [](const std::vector<std::pair<std::string, std::string>>& items) {
      std::vector<value::token> toks;
      for (const auto& it : items) {
        std::string s = it.first;  // raw schema name (known or unknown)
        if (!it.second.empty()) s += ":" + it.second;  // multi-apply instance
        toks.push_back(value::token(s));
      }
      return toks;
    };
    // Helper: apply one op group to the ListOp by qualifier.
    auto applyOp = [](ListOp<value::token>& op, ListEditQual qual,
                      const std::vector<value::token>& toks) {
      switch (qual) {
        case ListEditQual::ResetToExplicit:
          op.ClearAndMakeExplicit();
          op.SetExplicitItems(toks);
          break;
        case ListEditQual::Prepend: op.SetPrependedItems(toks); break;
        case ListEditQual::Append:  op.SetAppendedItems(toks);  break;
        case ListEditQual::Add:     op.SetAddedItems(toks);     break;
        case ListEditQual::Delete:  op.SetDeletedItems(toks);   break;
        default:                    op.SetPrependedItems(toks); break;
      }
    };

    ListOp<value::token> api_listop;
    bool emit = true;

    if (api_schemas.explicitlyEmpty) {
      // `apiSchemas = None` (explicit empty list).
      api_listop.ClearAndMakeExplicit();
    } else if (!api_schemas.authoredOps.empty()) {
      // Faithful path: reproduce each authored op verbatim (preserves
      // delete+prepend, unknown schemas, and multi-apply instances).
      for (const auto& op : api_schemas.authoredOps) {
        applyOp(api_listop, op.first, toTokens(op.second));
      }
    } else {
      // Fallback for programmatically-built APISchemas (no authoredOps):
      // use the resolved view, including unknown schemas so they round-trip.
      std::vector<value::token> api_tokens;
      for (const auto& item : api_schemas.names) {
        std::string s = to_string(item.first);
        if (!item.second.empty()) s += ":" + item.second;
        api_tokens.push_back(value::token(s));
      }
      for (const auto& item : api_schemas.unknownSchemas) {
        std::string s = item.first;
        if (!item.second.empty()) s += ":" + item.second;
        api_tokens.push_back(value::token(s));
      }
      // Avoid emitting a non-explicit ListOp with no items (which strict
      // readers reject); nothing meaningful to author in that case.
      if (api_tokens.empty()) {
        emit = false;
      } else {
        applyOp(api_listop, api_schemas.listOpQual, api_tokens);
      }
    }

    if (emit) {
      crate::CrateValue api_value;
      api_value.Set(api_listop);
      fields.push_back({"apiSchemas", api_value});
    }
  }

  // Add inherits if present
  if (metas.inherits) {
    // Convert all listops to ListOp<Path>
    ListOp<Path> inherits_listop;
    size_t total_inherits = 0;

    for (const auto& inherits_op : metas.inherits.value()) {
      const ListEditQual& qual = inherits_op.first;
      const std::vector<Path>& inherits_list = inherits_op.second;
      total_inherits += inherits_list.size();

      // Accumulate items based on ListEditQual
      switch (qual) {
        case ListEditQual::ResetToExplicit:
          inherits_listop.ClearAndMakeExplicit();
          inherits_listop.SetExplicitItems(inherits_list);
          break;
        case ListEditQual::Append:
          inherits_listop.SetAppendedItems(inherits_list);
          break;
        case ListEditQual::Prepend:
          inherits_listop.SetPrependedItems(inherits_list);
          break;
        case ListEditQual::Add:
          inherits_listop.SetAddedItems(inherits_list);
          break;
        case ListEditQual::Delete:
          inherits_listop.SetDeletedItems(inherits_list);
          break;
        default:
          // Default to explicit
          inherits_listop.ClearAndMakeExplicit();
          inherits_listop.SetExplicitItems(inherits_list);
          break;
      }
    }

    crate::CrateValue inherits_value;
    inherits_value.Set(inherits_listop);
    fields.push_back({"inherits", inherits_value});

    DCOUT("[ConvertPrimSpecRecursive] Added inherits with "
              << total_inherits << " paths");
    (void)total_inherits;
  }

  // Add specializes if present
  if (metas.specializes) {
    // Convert all listops to ListOp<Path>
    ListOp<Path> specializes_listop;
    size_t total_specializes = 0;

    for (const auto& specializes_op : metas.specializes.value()) {
      const ListEditQual& qual = specializes_op.first;
      const std::vector<Path>& specializes_list = specializes_op.second;
      total_specializes += specializes_list.size();

      // Accumulate items based on ListEditQual
      switch (qual) {
        case ListEditQual::ResetToExplicit:
          specializes_listop.ClearAndMakeExplicit();
          specializes_listop.SetExplicitItems(specializes_list);
          break;
        case ListEditQual::Append:
          specializes_listop.SetAppendedItems(specializes_list);
          break;
        case ListEditQual::Prepend:
          specializes_listop.SetPrependedItems(specializes_list);
          break;
        case ListEditQual::Add:
          specializes_listop.SetAddedItems(specializes_list);
          break;
        case ListEditQual::Delete:
          specializes_listop.SetDeletedItems(specializes_list);
          break;
        default:
          // Default to explicit
          specializes_listop.ClearAndMakeExplicit();
          specializes_listop.SetExplicitItems(specializes_list);
          break;
      }
    }

    crate::CrateValue specializes_value;
    specializes_value.Set(specializes_listop);
    fields.push_back({"specializes", specializes_value});

    DCOUT("[ConvertPrimSpecRecursive] Added specializes with "
              << total_specializes << " paths");
    (void)total_specializes;
  }

  // Add assetInfo if present
  if (metas.has_assetInfo()) {
    crate::CrateValue asset_info_value;
    asset_info_value.Set(metas.get_assetInfo());
    fields.push_back({"assetInfo", asset_info_value});
    DCOUT("[ConvertPrimSpecRecursive] Added assetInfo with "
              << metas.get_assetInfo().size() << " entries");
  }

  // Add instanceable if present
  if (metas.has_instanceable()) {
    crate::CrateValue instanceable_value;
    instanceable_value.Set(metas.get_instanceable());
    fields.push_back({"instanceable", instanceable_value});
    DCOUT("[ConvertPrimSpecRecursive] Added instanceable: "
              << metas.get_instanceable());
  }

  // Emit `primChildren` / `properties` token-vectors. USD crate readers
  // (including pxrUSD) use these fields to enumerate a prim's children and
  // properties — the spec table alone is not traversed, so omitting them makes
  // the prim appear empty (or absent) to other USD tools. Prefer the authored
  // ordering vectors; fall back to the live children()/props() containers.
  {
    std::vector<value::token> child_tokens = primspec.primChildren();
    if (child_tokens.empty()) {
      for (const auto& child : primspec.children()) {
        child_tokens.push_back(value::token(child.name()));
      }
    }
    if (!child_tokens.empty()) {
      crate::CrateValue pc_value;
      pc_value.Set(child_tokens);
      fields.push_back({"primChildren", pc_value});
    }
  }
  {
    std::vector<value::token> prop_tokens = primspec.propertyNames();
    if (prop_tokens.empty()) {
      for (const auto& item : primspec.props()) {
        prop_tokens.push_back(value::token(item.first));
      }
    }
    if (!prop_tokens.empty()) {
      crate::CrateValue props_value;
      props_value.Set(prop_tokens);
      fields.push_back({"properties", props_value});
    }
  }

  // 5. Add spec to file
  if (!AddSpec(prim_path, SpecType::Prim, fields, err)) {
    if (err) *err = "Failed to add spec for prim: " + prim_path.full_path_name();
    return false;
  }

  // 6. Process variant sets (must happen after parent prim spec is added)
  for (const auto& vs_item : primspec.variantSets()) {
    if (!ConvertVariantSetSpecToFields(vs_item.first, vs_item.second, prim_path, err)) {
      if (err) *err = "Failed to convert VariantSetSpec '" + vs_item.first
                     + "' for " + prim_path.full_path_name() + ": " + *err;
      return false;
    }
  }

  return true;
}

// ============================================================================
// Iterative Depth-First PrimSpec Conversion
// ============================================================================

bool CrateWriter::ConvertPrimSpecIterative(
    const PrimSpec& root_primspec,
    const Path& parent_path,
    std::string* err,
    const std::string& name_override) {

  struct WorkItem {
    const PrimSpec* primspec;
    Path parent_path;
  };

  // For the root item, use name_override if PrimSpec has no name set
  Path effective_parent = parent_path;
  if (root_primspec.name().empty() && !name_override.empty()) {
    // Pre-build the prim path so ConvertSinglePrimSpec finds it via the
    // empty-name fallback (prim_path = parent_path when name is empty)
    effective_parent = parent_path.AppendPrim(name_override);
  }

  std::vector<WorkItem> stack;
  stack.push_back({&root_primspec,
                   root_primspec.name().empty() ? effective_parent : parent_path});

  while (!stack.empty()) {
    if (stack.size() > kMaxDefaultTraversalLimit) {
      if (err) *err = "ConvertPrimSpecIterative: tree too deep.";
      return false;
    }

    WorkItem item = std::move(stack.back());
    stack.pop_back();

    if (!ConvertSinglePrimSpec(*item.primspec, item.parent_path, err)) {
      return false;
    }

    // Build this prim's path for its children
    Path prim_path = item.primspec->name().empty()
        ? item.parent_path  // ConvertSinglePrimSpec used parent_path directly
        : item.parent_path.AppendPrim(item.primspec->name());

    // Push children in reverse order so left-most child is processed first
    const auto& children = item.primspec->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({&(*it), prim_path});  // children use their own name()
    }
  }

  return true;
}

// ============================================================================
// VariantSetSpec / VariantSpec Conversion (Layer/PrimSpec path)
// ============================================================================

bool CrateWriter::ConvertVariantSetSpecToFields(
    const std::string& variantset_name,
    const VariantSetSpec& variantset,
    const Path& parent_path,
    std::string* err) {

  // VariantSet path: parent{variantSetName} (e.g., /Chair{materialVariant})
  // pxr's SdfPath has no bare `{set}` form: a VariantSet spec lives at the
  // EMPTY variant selection, `{set=}`. See ConvertVariantSetToFields in
  // stage-converter.cc.
  std::string variantset_path_str = parent_path.prim_part() + "{" + variantset_name + "=}";
  Path vs_path(variantset_path_str, "");

  DCOUT("[ConvertVariantSetSpecToFields] Creating VariantSet spec: "
            << vs_path.full_path_name());

  // VariantSet spec needs "variantChildren" field listing all variant names
  crate::FieldValuePairVector vs_fields;

  std::vector<value::token> variant_tokens;
  for (const auto& variant_item : variantset.variantSet) {
    variant_tokens.push_back(value::token(variant_item.first));
  }

  crate::CrateValue variant_children_value;
  variant_children_value.Set(variant_tokens);
  vs_fields.push_back({"variantChildren", variant_children_value});

  if (!AddSpec(vs_path, SpecType::VariantSet, vs_fields, err)) {
    if (err) *err = "Failed to add VariantSet spec: " + vs_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertVariantSetSpecToFields] Created VariantSet with "
            << variantset.variantSet.size() << " variants");

  // For each variant, create a Variant spec
  // IMPORTANT: Pass parent_path (the Prim), not vs_path (the VariantSet)
  // Variant path should be /Prim{variantSet=value}, NOT /Prim{variantSet}{variantSet=value}
  for (const auto& variant_item : variantset.variantSet) {
    const auto& variant_name = variant_item.first;
    const auto& variant_primspec = variant_item.second;

    if (!ConvertVariantSpecToFields(variant_name, variant_primspec, parent_path, variantset_name, err)) {
      if (err) *err = "Failed to convert variant '" + variant_name + "': " + *err;
      return false;
    }
  }

  return true;
}

bool CrateWriter::ConvertVariantSpecToFields(
    const std::string& variant_name,
    const PrimSpec& variant_primspec,
    const Path& parent_prim_path,
    const std::string& variantset_name,
    std::string* err) {

  // Variant path: parent_prim_path{variantSetName=variant_name}
  // (e.g., /Chair{materialVariant=plastic})
  // NOTE: The variant path is based on the prim path, NOT the variantSet path
  std::string variant_path_str = parent_prim_path.prim_part() + "{" +
                                 variantset_name + "=" + variant_name + "}";
  Path v_path(variant_path_str, "");

  DCOUT("[ConvertVariantSpecToFields] Creating Variant spec: "
            << v_path.full_path_name());

  // Variant spec contains variant metadata and prim children
  crate::FieldValuePairVector v_fields;

  // Add variant specifier
  Specifier spec = variant_primspec.specifier();
  if (spec == Specifier::Invalid) {
    spec = Specifier::Def;
  }
  crate::CrateValue spec_value;
  spec_value.Set(spec);
  v_fields.push_back({"specifier", spec_value});

  // Add typeName if present
  if (!variant_primspec.typeName().empty()) {
    crate::CrateValue type_value;
    value::token tok(variant_primspec.typeName());
    type_value.Set(tok);
    v_fields.push_back({"typeName", type_value});
  }

  // Add variant metadata (hidden, active, etc.)
  const PrimMeta& metas = variant_primspec.metas();

  if (metas.has_hidden()) {
    crate::CrateValue hidden_value;
    hidden_value.Set(metas.get_hidden());
    v_fields.push_back({"hidden", hidden_value});
  }

  if (metas.has_active()) {
    crate::CrateValue active_value;
    active_value.Set(metas.get_active());
    v_fields.push_back({"active", active_value});
  }

  if (metas.has_doc()) {
    crate::CrateValue doc_value;
    doc_value.Set(metas.get_doc().value);
    v_fields.push_back({"documentation", doc_value});
  }

  // Add variantSets list if variant has nested variant sets
  if (metas.variantSets) {
    std::vector<std::string> variant_sets_list;
    for (const auto& variantSets_op : metas.variantSets.value()) {
      for (const auto& vs : variantSets_op.second) {
        variant_sets_list.push_back(vs);
      }
    }
    crate::CrateValue variant_sets_value;
    variant_sets_value.Set(variant_sets_list);
    v_fields.push_back({"variantSets", variant_sets_value});
  }

  // Add variant properties as separate attribute/relationship specs
  for (const auto& prop_item : variant_primspec.props()) {
    const auto& prop_name = prop_item.first;
    const auto& prop = prop_item.second;

    if (prop.is_attribute()) {
      if (!ConvertAttributeToFields(prop_name, prop.get_attribute(), v_path, prop.has_custom(), err)) {
        if (err) *err = "Failed to convert variant attribute: " + prop_name;
        return false;
      }
    } else if (prop.is_relationship()) {
      if (!ConvertRelationshipToFields(prop_name, prop.get_relationship(), v_path, err)) {
        if (err) *err = "Failed to convert variant relationship: " + prop_name;
        return false;
      }
    }
  }

  // Create the variant spec BEFORE adding child prims
  // The parent spec must exist before children in the spec list
  if (!AddSpec(v_path, SpecType::Variant, v_fields, err)) {
    if (err) *err = "Failed to add Variant spec: " + v_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertVariantSpecToFields] Created Variant spec with "
            << v_fields.size() << " fields");

  // Add variant prim children (recursively convert each child PrimSpec)
  const auto& children = variant_primspec.children();
  if (!children.empty()) {
    DCOUT("[ConvertVariantSpecToFields] Processing " << children.size()
              << " prim children for variant: " << variant_name);
    for (const auto& child : children) {
      if (!ConvertPrimSpecIterative(child, v_path, err)) {
        if (err) *err = "Failed to convert variant prim child: " + child.name() + ": " + *err;
        return false;
      }
    }
  }

  // Process nested variant sets
  for (const auto& vs_item : variant_primspec.variantSets()) {
    if (!ConvertVariantSetSpecToFields(vs_item.first, vs_item.second, v_path, err)) {
      if (err) *err = "Failed to convert nested VariantSetSpec '" + vs_item.first
                     + "' in variant " + variant_name + ": " + *err;
      return false;
    }
  }

  return true;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
