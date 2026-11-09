// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP arc list-op merge helpers.
// Included inside Cache::Impl.

  // --- cross-layer list-op merge (Phase 7 S5) -----------------------------

  enum class ArcSel { References, Payloads, Inherits, Specializes };

  static const std::vector<std::string> &SelectInlineArc(const PrimMetadata &m,
                                                         ArcSel f) ;

  static const ArcListOp *SelectArcEdit(const PrimMetadata &m, ArcSel f) ;

  // Compose one arc field across a site's specs (input is strong-first) per the
  // AOUSD list-op rules, applying weakest->strongest. Returns (arc-string,
  // authoring-layer-id) pairs so relative reference/payload asset paths can be
  // anchored to the layer that authored them.
  std::vector<std::pair<std::string, std::string>> MergeArcField(
      const std::vector<SpecRef> &specs, ArcSel f) const ;
