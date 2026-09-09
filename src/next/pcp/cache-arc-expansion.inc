// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache arc expansion.
// Included inside Cache::Impl: arc state and compiled method declarations.
// Implementations live in cache-arcs.cc and cache-relocates.cc.

  // --- arc expansion (inherits/variants/references/payloads/specializes) ---

  // Resolve+recurse an external/internal arc target from source `src`.
  // `kind` is the arc type; `out` collects ordinary opinions and `spec_out`
  // collects specialize-derived opinions (globally weakest). A specialize arc
  // (and its entire subtree) routes into `spec_out`.
  // Unwrap a synthetic variant-content layer identifier
  // ("variant:<hostfile>:<site>:<vset>:<vname>", see the vid built in ExpandArcs)
  // back to the host layer's real FILE path, so a relative reference/payload
  // authored inside a variant anchors to the file that contains the variant.
  // Recurses for nested variants; a real file path passes through unchanged.
  static std::string RealAnchorOf(std::string id);

  // An internal arc may name its target with a path RELATIVE to the prim that
  // authored it ("../_Y", "_Y", "./_Y"): resolve against the authoring site.
  // An external arc's target is always absolute in the referenced layer.
  static std::string ResolveArcPrimPath(const std::string &authoring_site,
                                        const std::string &prim_path);

  // A variantSet whose selection was unknown when its spec expanded
  // (authored at a weaker site): grafted by ProcessDeferredVariants at the
  // recorded strength position once the whole prim's selections are known.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
// Src and ArcChain are intentionally file-local implementation types. GCC
// otherwise warns when this nested, likewise implementation-only helper holds
// them directly.
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
  struct DeferredVariant {
    std::vector<Src> *out;
    std::vector<Src> *spec_out;
    size_t main_pos;
    size_t spec_pos;
    Src src;
    const PrimSpec *spec;
    std::string set_name;
    std::string layer_id;
    ArcChain chain;
  };
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  std::vector<DeferredVariant> deferred_variants_;

  // Graft every deferred variantSet owned by this ExpandList invocation whose
  // selection is now known (fixed-point: a grafted option's own selections can
  // enable further deferred sets), splicing the result at the recorded
  // position. Leftover entries (still no selection anywhere) are dropped.
  void ProcessDeferredVariants(std::vector<Src> *main_list,
                               std::vector<Src> *spec_list,
                               std::map<std::string, std::string> *sels,
                               std::string *warn, std::string *err);

  // One selected variant OPTION grafted onto its host: content subtree,
  // inline opinions, option-authored composition arcs, and nested sets.
  // (Extracted from ExpandArcs so DEFERRED variant sets — whose selection is
  // authored at a WEAKER site and only known once the whole prim expanded —
  // can be grafted afterwards at their recorded strength position.)
  void ApplyVariantOption(const Src &src, const ExpansionFrame *frame,
                          const std::string &set_name, const VariantData *vd,
                          const PrimSpec &owner, int vdepth,
                          const std::string &layer_id,
                          std::vector<Src> *out, std::vector<Src> *spec_out,
                          std::map<std::string, std::string> *sels,
                          const ArcChain &chain, std::string *warn,
                          std::string *err);

  // Does `path` land in namespace edited by `stack`'s relocates? Source-side
  // hits are PROHIBITED arc targets (pxr: cannot address pre-relocation
  // namespace); target-side hits must resolve through the composed/arrival
  // machinery so relocation-source suppression applies.
  bool PathInRelocateSource(uint32_t stack, const std::string &path) const;
  bool PathInRelocateTarget(uint32_t stack, const std::string &path) const;

  // Does any STRICT ancestor of `path` in `stack` author a variantSet? Such a
  // target's composition depends on a variant selection that may be resolved in
  // an OUTER layer stack (a reference selects the variant): the isolated
  // target-stack re-derive uses that stack's DEFAULT selection and grafts the
  // wrong variant's children into the arc target (TrickyInheritsInVariants:
  // /Model/Scope inherits /Model/Class, whose children come from /Model's
  // variantSet — model.usd defaults complexity=low but the root selects high).
  // Reusing the composed target instead picks up the authoritative selection.
  bool AncestorAuthorsVariantSet(uint32_t stack,
                                 const std::string &path) const;

  void ProcessArc(const Src &src, const CompositionArc &arc_in, ArcType kind,
                  const ExpansionFrame *frame, std::vector<Src> *out,
                  std::vector<Src> *spec_out,
                  std::map<std::string, std::string> *sels,
                  const ArcChain &chain, std::string *warn,
                  std::string *err, const std::string &authoring_layer_id = "");

  // Pre-order DFS == strength order. Per source: local (pushed) > inherits >
  // references > payloads; specializes routed to `spec_out` (globally weakest).
  void ExpandArcs(const Src &src, const ExpansionFrame *frame,
                  std::vector<Src> *out, std::vector<Src> *spec_out,
                  std::map<std::string, std::string> *sels,
                  const ArcChain &chain, std::string *warn,
                  std::string *err);

  std::vector<Src> ExpandList(const std::vector<Src> &base,
                              const std::string &root_prim_path,
                              std::string *warn, std::string *err);

  // --- relocates ------------------------------------------------------------

  // Add a layer stack's namespace edits to a mapping FROM that stack into the
  // composed namespace, so paths this mapping translates (relationship and
  // connection TARGETS, implied class sites) land at the relocated address
  // rather than the vacated one: a connection authored in subrig.usd to
  // `/SubRig/Anim/AnimScope.x` must resolve to `/Char/Anim2/AnimScope.x` when
  // two enclosing layer stacks each relocated part of that path
  // (TrickyConnectionToRelocatedAttribute). Renames the ENCLOSING stacks
  // perform are already in `m` (NamespaceMapping::Compose pulls them into the
  // deeper namespace), so mapping each destination through `m` composes the
  // whole chain of edits.
  NamespaceMapping WithStackRelocates(uint32_t stack, NamespaceMapping m) const;

  // Sources-cache key. Stack 0 is the composed (root) namespace and keys by the
  // composed path alone, so every other consumer of the cache (ComposePrim,
  // Invalidate, the parallel warm) is unchanged. Sites expanded inside another
  // layer stack's own namespace (relocate sources) get a stack-qualified key.
  static std::string SourcesKey(uint32_t stack, const std::string &path);

  // The namespace edits in effect at a composed prim, gathered from EVERY layer
  // stack that contributes a source to it: a relocate authored by any of them
  // (root layer stack, or one reached through a reference/payload/variant) edits
  // this prim's composed child namespace.
  struct ChildRelocations {
    std::set<std::string> departed;    // names the edit moved away
    std::vector<std::string> arrived;  // names the edit moved in (authored order)
    bool empty() const { return departed.empty() && arrived.empty(); }
  };
  ChildRelocations RelocationsAt(const std::vector<Src> &srcs) const;

  // The relocated CONTENT sources for an arrival (the source prim's own
  // composition sources, before the departure-suppression + outer rename that
  // AddRelocatedSources composes on top). `child_composed` is the arrival's
  // destination composed path (the recursion-guard key).
  //
  // Step 0: forwards to the isolated relocate-source walk. Step 1 will derive
  // this from the source's COMPOSED parent when not re-entrant, so the caller's
  // variant/implied-class context reaches the relocated prim; the guard
  // (`reloc_content_in_progress`) falls back to the isolated walk on the
  // re-entrant chained-relocate shapes that would otherwise recurse.
  // Same-stack INTERNAL-REFERENCE content: a relocate follows content brought in
  // via arcs into OTHER layer stacks, and same-stack content delivered by an
  // INHERIT (the relocated prim's own class opinions -- TrickyMultipleRelocations4
  // inherits /SymRig), but NOT content delivered by a same-stack (internal)
  // REFERENCE from a DIFFERENT subtree (pxr per-arc scoping). Salt the latter --
  // its specs (incl. a `def` specifier) must not compose, while it stays
  // traversable (RelocatePrimsWithSameName).
  void SaltInternalRefContent(std::vector<Src> &content, const Src &ps,
                              const StackRelocates::Arrival &arrival,
                              std::string *warn, std::string *err);

  // Is this content source's leaf name relocated AWAY (departed) under its
  // parent in the COMPOSED namespace -- e.g. via a reference to a relocating
  // stack (RelocatePrimsWithSameName: /ChainedRef_1/Child is departed because
  // /ChainedRef_1 references /ChainedRef_2 which relocates Child->Child_2)?
  // Such content does not compose. Distinct from an arc-introduced relocated
  // prim that is NOT departed (test_relocates /Lib/Model/Inner -> /World/New).
  // Only checked for same-stack (relocating stack) content whose site sits
  // outside the source subtree -- content brought in by an internal reference.
  bool ContentSiteDeparted(const Src &c, const Src &ps,
                           const StackRelocates::Arrival &arrival,
                           std::string *warn, std::string *err);

  // Did this content source arrive THROUGH a relocation-source path in an
  // intermediate contributing stack? Its `arc_sites` trail records the (stack,
  // target) of every arc it crossed; if the source's leaf name under any
  // INTERMEDIATE arc target is a relocation source there, that stack relocated
  // the prim away, so the content DEPARTED and must not compose (chained
  // departure through a reference chain -- RelocatePrimsWithSameName:
  // base.usd/Base/Child arrives via /ChainedRef_2 where /ChainedRef_2/Child is a
  // relocation source). EXCLUDE the arrival's OWN source (composing the arrival
  // OF that same relocate legitimately pulls this content). Salted (not dropped)
  // so it stays traversable.
  bool FlowsThroughRelocateSource(const Src &c,
                                  const StackRelocates::Arrival &arrival) const;

  std::vector<Src> SourcesForRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err);

  // The composed-parent derivation is equivalent to (and richer than) the
  // isolated walk ONLY for the OUTERMOST, non-chained relocate:
  //   * ctx1: not inside an isolated SourcesForRelocateSource walk (a nested
  //     chained relocate resolved in a stack's own namespace).
  //   * outermost: no composed relocated-content derivation already active
  //     (its SourcesForSite(0,..) would re-enter this one).
  //   * non-chained: no ancestor of the source is itself a relocate
  //     destination of this stack -- deriving via the composed parent would
  //     re-expand that ancestor relocate (double count).
  //   * the source's composed parent is a real prim (not a root-level move).
  // Is an ANCESTOR of the source itself a relocate destination of this stack?
  // (a chained relocate: the source sits under another relocate's arrival).
  bool IsChainedArrival(const Src &ps,
                        const StackRelocates::Arrival &arrival) const;

  // Applicability of the composed-parent derivation INDEPENDENT of chaining:
  // outermost (not nested / not already deriving), the source maps cleanly into
  // ROOT namespace (a SUBROOT-ref source outside every arc pair does not resolve
  // via SourcesForSite(0,..)), and no salted-earth `over` is authored at the
  // source in the relocating stack (whose descendant-suppression only the
  // isolated walk models).
  bool ComposedBaseApplicable(const Src &ps,
                              const StackRelocates::Arrival &arrival);

  // The composed-parent derivation REPLACES the isolated walk only for the
  // non-chained case (a chained source deriving via the composed parent would
  // re-expand the ancestor relocate / lose post-relocation opinions). Chained
  // cases instead SUPPLEMENT the isolated walk (see SourcesForRelocatedContent).
  bool ComposedRelocApplicable(const Src &ps,
                               const StackRelocates::Arrival &arrival);

  // ps.map_idx with this stack's relocate-rename pairs dropped (arc pairs only).
  NamespaceMapping ArcOnlyMapping(const Src &ps) const;

  // Reverse the relocating stack's relocates repeatedly: if `path` lies under a
  // relocate DESTINATION, map it back to that relocate's SOURCE. Iterating this
  // un-does a CHAIN of relocates, returning the fully-pre-relocation path (in the
  // stack's own namespace). Bounded to avoid pathological loops.
  std::string FullyUnrelocate(uint32_t stack, std::string path) const;

  // ISOLATED branch (historical behavior): resolve the source in the relocating
  // stack's OWN namespace, then compose `outer` -- ps's mapping to root plus the
  // src_site -> child_composed redirect rename -- on top of each content
  // source's own map. Offsets compose under ps; a source arriving as a Root
  // opinion becomes a Relocate; the relocation source's own raw-site opinions
  // are salted (suppress_site_specs).
  std::vector<Src> IsolatedRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err);

  // COMPOSED-PARENT branch (Step 1): derive the relocated content from the
  // source's parent resolved in ROOT (stack 0) namespace, so the caller's
  // variant-selection / implied-class context reaches the relocated prim. The
  // source path is mapped through ps's arc into root namespace; its parent is
  // composed there; the source child is derived (with its own departure
  // suppressed) and redirected to child_composed. Returns final composed-form
  // Srcs, directly comparable to IsolatedRelocatedContent.
  std::vector<Src> ComposedRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err);

  // Graft a prim a relocate moved INTO this parent: the opinions authored AT the
  // destination path (`dst`, pushed to `base`) and the content from its pre-
  // relocation source site (pushed to `content_out`). `ps` is the parent source
  // whose stack authored the relocate. When `content_out != base` the caller
  // DEFERS the relocated content below every arrival-local destination-site
  // opinion for the arrival name -- pxr composes arrival-local opinions
  // (including a sibling variant graft) STRONGER than the relocated content
  // (TrickyVariantOverrideOfRelocatedPrim). `content_out == nullptr` keeps the
  // historical inline order.
  void AddRelocatedSources(const Src &ps,
                           const StackRelocates::Arrival &arrival,
                           const std::string &child_composed,
                           std::vector<Src> *base, std::string *warn,
                           std::string *err,
                           std::vector<Src> *content_out = nullptr);

  // Sources of the composed child `cn` (composed path `child_composed`) of a
  // prim whose sources are `psrcs`. Relocates authored by any contributing layer
  // stack apply here: a child the stack moved away contributes nothing from that
  // stack, and a child it moved in is pulled from its pre-relocation site.
  //
  // `raw_stack`/`raw_site` name a single source for which the departure rule is
  // suppressed -- used when the walk is deliberately addressing the relocate's
  // own source prim (see SourcesForRelocateSource).
  std::vector<Src> DeriveChildSources(const std::vector<Src> &psrcs,
                                      const std::string &cn,
                                      const std::string &child_composed,
                                      std::string *warn, std::string *err,
                                      uint32_t raw_stack = kInvalidStack,
                                      const std::string &raw_site = "");

  // Composition sources of a relocate's SOURCE prim, expanded in `stack`'s own
  // namespace. The composed namespace no longer holds this prim (its parent
  // there lists it as departed), but its ANCESTORS do (relocate paths are keyed
  // in the composed namespace -- BuildStackRelocates), either as normal prims or
  // as arrivals of ancestral relocates. So the ancestor chain is composed with
  // arrival handling ON (bringing each arrival's `over` opinions AND its
  // relocated content) and only the LEAF's own departure is suppressed. Handles
  // both an arc-delivered source (`/CharRig/Rig/SubRig` references subrig.usd,
  // relocate moves `.../Anim/AnimScope` up -- content via the reference) and a
  // source authored at a post-relocation path (`over Model_2 { Rig }` on an
  // ancestral arrival -- TrickyMultipleRelocations2).
  const std::vector<Src> &SourcesForRelocateSource(uint32_t stack,
                                                   const std::string &site,
                                                   std::string *warn,
                                                   std::string *err);

  // --- ancestral source resolution (cached) -------------------------------

  // The expanded composition sources for a composed prim `path`.
  const std::vector<Src> &SourcesForPath(const Path &path, std::string *warn,
                                         std::string *err);

  // The expanded composition sources for `path` in layer stack `stack`'s
  // namespace (stack 0 == the composed/root namespace). A root-level prim seeds
  // from that layer stack; a descendant derives each of its parent's expanded
  // sources at the child name (so arcs authored on an ancestor reach it, and so
  // relocates authored by any contributing stack apply), then expands that
  // child's own arcs. Cached per (stack, path).
  const std::vector<Src> &SourcesForSite(uint32_t stack, const Path &path,
                                         std::string *warn, std::string *err);

  // --- value resolution ---------------------------------------------------

  // Merge all sources into a fresh PrimSpec; return the composed child names.
  //
  // OPINIONS compose STRONG->WEAK (fill-absent: the strongest source that
  // authored a field/property wins). CHILD-NAME ORDER, however, composes
  // WEAK->STRONG -- pxr's PcpComposeSiteChildNames appends each node's (and each
  // layer's) primChildren weakest-first, so the weakest source that introduces a
  // name fixes its position and stronger duplicates are skipped (e.g. a
  // reference's `[LOD0, Materials]` precedes a local-only `LOD1` ->
  // `[LOD0, Materials, LOD1]`). So we collect names in a separate reverse pass.
