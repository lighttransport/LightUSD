// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache diagnostics and layer-stack setup.
// Included inside Cache::Impl.

  void AddIssue(ErrorCode code, const std::string &site,
                const std::string &message, std::string *err) {
    issues_.push_back(CompositionIssue{code, site, message});
    if (err) *err += message + "\n";
  }
  // --- layer stacks -------------------------------------------------------

  static constexpr uint32_t kInvalidStack =
      (std::numeric_limits<uint32_t>::max)();

  // Deterministic fingerprint of an expression-variable dictionary, appended
  // to the layer-stack identity: pxr keys layer stacks by (identifier,
  // expression variables), so the same asset referenced under different
  // inherited variable contexts is a DIFFERENT stack (its sublayer
  // expressions can resolve differently). Keys sort for stability; scalar
  // kinds render directly, nested dictionaries recurse, other kinds render
  // their type id (identity-conservative: distinct enough to split).
  static void AppendExpressionVarsFingerprint(const Value &vars,
                                              std::string *out);

  uint32_t InternLayerStack(
      std::shared_ptr<Layer> layer, const std::string &identifier,
      std::string *warn, std::string *err,
      const std::shared_ptr<const Value> &inherited_vars = nullptr);

  // Resolve the `relocates` authored by every layer of `st` (layer metadata and
  // per-prim `relocates`) into this stack's namespace-edit tables. Relocates are
  // a LAYER-STACK property: a referenced layer's relocates rename prims inside
  // the referenced namespace, and the rename maps through the arc into root
  // space (see DeriveChildSources).
  static void BuildStackRelocates(LayerStack *st);

  bool AppendLayerAndSublayers(LayerStack &st, std::shared_ptr<Layer> layer,
                               const std::string &identifier,
                               std::set<std::string> *visiting,
                               uint32_t depth, const LayerOffset &cum_offset,
                               const Value &expr_vars,
                               std::string *warn, std::string *err);
