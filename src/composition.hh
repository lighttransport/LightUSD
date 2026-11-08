// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment Inc.

///
/// @file composition.hh
/// @brief USD Layer and Prim composition system
///
/// Implements USD's composition arcs system for building complex scenes
/// from multiple layers and referenced assets. Composition allows USD
/// scenes to reference, inherit from, and specialize other USD files.
///
/// Supported composition arcs:
/// - References: Include content from other USD files
/// - Payloads: Deferred loading of heavy content
/// - Inherits: Class-like inheritance between prims (multiple targets supported)
/// - Specializes: Globally-weak shared opinions (multiple targets supported)
/// - SubLayers: Layer-level composition
/// - VariantSets: Multiple versions of content with deferred evaluation
/// - Overs: Overrides of existing content
///
/// Key concepts:
/// - Layer: Individual USD file or memory content
/// - Stage: Composed result of multiple layers
/// - Composition arcs: Rules for combining layers
/// - Load states: Control what gets loaded when
/// - LIVRPS ordering: L > I > V > R > P > S (via CompositeAllArcs)
/// - Cycle detection: all arc types detect circular references
/// - Active metadatum: prims with active=false are excluded post-composition
///
#pragma once

#include <functional>

#include "asset-resolution.hh"
#include "tiny-hashmap.hh"
#include "core/prim-spec.hh"  // PrimSpec, FileFormatHandler (transitively: property, composition-types, prim-enums, prim-metas, variant-types)
#include "layer.hh"           // Layer class

// Remaining TODO items:
// - [ ] Multi-level implied inherits/specializes propagation (currently single-level)
// - [ ] `order` ListEditQual for references/payloads (implemented for inherits/specializes)

namespace lightusd {

// Forward decl.
class Stage;

// USD asset loading state.
enum class LoadState : uint32_t {
  Toplevel = 1,        // load initial .usd(default)
  Sublayer = 1 << 1,   // load USD from Stage meta sublayer
  Reference = 1 << 2,  // load USD from Prim meta reference
  Payload = 1 << 3     // load USD from Prim meta payload
};

struct SublayersCompositionOptions {
  // The maximum depth for nested `subLayers`
  uint32_t max_depth = 1024u;

  // Make an error when referenced asset does not contain prims.
  bool error_when_no_prims_in_sublayer{false};

  // Make an error when referenced asset is not found
  bool error_when_asset_not_found{false};

  // Make an error when referenced asset is unsupported(e.g. unknown file extension)
  bool error_when_unsupported_fileformat{false};

  // File formats
  lightusd::HashMap<std::string, FileFormatHandler> fileformats;

  // Memory optimization options
  bool enable_inplace_composition{false};  // Enable in-place memory management
  size_t max_memory_limit_mb{16384};      // Maximum memory limit in MB
  size_t max_asset_bytes{0};              // 0 = security_policy default

  // Allow parent-directory ('..') segments in asset paths. Resolution of the
  // surviving '..' is delegated to the asset resolver, so only enable this when
  // a sandboxed/custom resolver controls reachability. See
  // security_policy::ValidateAndNormalizeAssetPath.
  bool allow_parent_relative_paths{false};
};

struct ReferencesCompositionOptions {
  // The maximum depth for nested `references`
  uint32_t max_depth = 1024u;

  // Make an error when referenced asset is not found
  bool error_when_asset_not_found{false};

  // Make an error when referenced asset is unsupported(e.g. unknown file extension)
  bool error_when_unsupported_fileformat{false};

  // File formats
  lightusd::HashMap<std::string, FileFormatHandler> fileformats;

  // Memory optimization options
  bool enable_inplace_composition{false};  // Enable in-place memory management
  size_t max_memory_limit_mb{16384};      // Maximum memory limit in MB
  size_t max_asset_bytes{0};              // 0 = security_policy default

  // Allow parent-directory ('..') segments in asset paths (resolution delegated
  // to the asset resolver). See security_policy::ValidateAndNormalizeAssetPath.
  bool allow_parent_relative_paths{false};

  ///
  /// Lazy reference resolution policy (mirrors
  /// PayloadCompositionOptions::load_policy).
  ///
  /// When set, this callback is called before resolving each `references` arc.
  /// Return true to resolve the reference, false to skip it (deferred).
  ///
  /// Parameters:
  ///   prim_path  — The path of the prim that has the reference arc
  ///   reference  — The Reference descriptor (asset path, prim path, layer offset)
  ///
  /// When nullptr (default), all references are resolved eagerly.
  ///
  /// NOTE: unlike `payload`, USD semantics assume references are always
  /// resolved — skipping one also skips its implied inherits/specializes
  /// propagation and typeName resolution. Intended for viewers that recompose
  /// from a retained layer later (CompositeReferences strips the reference
  /// metadata even for skipped arcs).
  ///
  std::function<bool(const Path &prim_path, const Reference &reference)> load_policy;

  // Optional cache of parsed referenced layers (keyed internally by resolved
  // path + resolution context). Each referenced file is parsed once and its
  // Layer copied per arc — with the copy-on-write value storage, every
  // referencing prim then SHARES one copy of the heavy attribute arrays
  // (UE-export scenes reference the same mesh files thousands of times).
  // Owned by the caller and typically kept alive across the whole fixed-point
  // composition loop. nullptr = parse per arc (legacy behavior).
  std::map<std::string, Layer> *layer_cache{nullptr};
  // Emit parallel layer-cache warmup timings to stderr.
  bool profile_layer_cache{false};
};

struct PayloadCompositionOptions {
  // The maximum depth for nested `payload`
  uint32_t max_depth = 1024u;

  // Make an error when referenced asset is not found
  bool error_when_asset_not_found{false};

  // Make an error when referenced asset is unsupported(e.g. unknown file extension)
  bool error_when_unsupported_fileformat{false};

  // File formats
  lightusd::HashMap<std::string, FileFormatHandler> fileformats;

  // Memory optimization options
  bool enable_inplace_composition{false};  // Enable in-place memory management
  size_t max_memory_limit_mb{16384};      // Maximum memory limit in MB
  size_t max_asset_bytes{0};              // 0 = security_policy default

  // Allow parent-directory ('..') segments in asset paths (resolution delegated
  // to the asset resolver). See security_policy::ValidateAndNormalizeAssetPath.
  bool allow_parent_relative_paths{false};

  // See ReferencesCompositionOptions::layer_cache.
  std::map<std::string, Layer> *layer_cache{nullptr};

  ///
  /// Lazy payload loading policy.
  ///
  /// When set, this callback is called before loading each payload asset.
  /// Return true to load the payload, false to skip it (lazy/deferred).
  ///
  /// Parameters:
  ///   prim_path  — The path of the prim that has the payload arc
  ///   payload    — The Payload descriptor (asset path, prim path, layer offset)
  ///
  /// When nullptr (default), all payloads are loaded eagerly.
  ///
  std::function<bool(const Path &prim_path, const Payload &payload)> load_policy;
  // Emit parallel layer-cache warmup timings to stderr.
  bool profile_layer_cache{false};
};


///
/// Extract subLayers asset paths
///
std::vector<std::string> ExtractSublayerAssetPaths(const Layer &layer);

///
/// Extract references asset paths
///
std::vector<std::string> ExtractReferencesAssetPaths(const Layer &layer);

///
/// Extract payload asset paths
///
std::vector<std::string> ExtractPayloadAssetPaths(const Layer &layer);

///
/// Return true when any PrimSpec in the Layer contains `references` Prim metadataum
///
/// Layer has cached flag for quicky detecting whether Layer has unresolved `references` or not.
///
/// @param[in] layer Layer
/// @param[in] force_check When true, traverse PrimSpec hierarchy and find `references` metadatum. Use cached flag in `layer` when false.
///
bool HasReferences(const Layer &layer, const bool force_check = false, const ReferencesCompositionOptions options = ReferencesCompositionOptions());

///
/// Return true when any PrimSpec in the Layer contains `payload` Prim metadataum
///
/// Layer has cached flag for quicky detecting whether Layer has unresolved `payload` or not.
///
/// @param[in] layer Layer
/// @param[in] force_check When true, traverse PrimSpec hierarchy and find `payload` metadatum. Use cached flag in `layer` when false.
///
bool HasPayload(const Layer &layer, const bool force_check = false, const PayloadCompositionOptions options = PayloadCompositionOptions());

///
/// Return true when any PrimSpec in the Layer contains `specializes` Prim metadataum.
/// We think specializers are not intensively used, so no caching.
///
/// @param[in] layer Layer
///
bool HasSpecializes(const Layer &layer);

///
/// Return true when any PrimSpec in the Layer contains `inherits` Prim metadataum.
///
/// @param[in] layer Layer
///
bool HasInherits(const Layer &layer);

///
/// Return true when any PrimSpec in the Layer contains `variants` Prim metadataum.
///
/// @param[in] layer Layer
///
bool HasVariants(const Layer &layer);

///
/// AOUSD Core Spec 10.3.2.5: variant selection is computed from opinions across
/// ALL composition arcs, so an iterative flatten must DEFER variant composition
/// until references and payloads are resolved. Otherwise a strong local variant
/// selection is baked against an empty/placeholder variantSet from a weaker
/// layer and consumed, so a deep default selection ends up winning (e.g. Pixar
/// Kitchen_set's Chair.usd: empty variant blocks + a payload chain to the layer
/// that holds the real variantSet).
///
/// Returns true when variant composition for `layer` should be deferred because
/// references and/or payloads are still unresolved (gated by the enabled-arc
/// flags — pass the driver's composition-feature toggles). The reference/payload
/// checks do not descend into unselected variant blocks, so deferring cannot
/// deadlock on arcs that only appear once a variant is selected.
///
/// @param[in] layer Layer
/// @param[in] references_enabled Whether the caller resolves `references`.
/// @param[in] payload_enabled Whether the caller resolves `payload`.
///
bool ShouldDeferVariantComposition(const Layer &layer,
                                   bool references_enabled = true,
                                   bool payload_enabled = true);

///
/// Bake every PrimSpec's `apiSchemas` applied-schema list-op into EXPLICIT form
/// (recursively, in place). Call this ONCE after a Layer has been fully
/// flattened (all arcs composed): pxrUSD's `usdcat --flatten` emits a composed
/// prim's apiSchemas as `apiSchemas = [...]` (explicit), never the authored
/// `prepend`/`append` qualifier -- the qualifier is meaningful only against
/// weaker opinions, which no longer exist in a flattened layer (keeping it would
/// wrongly re-prepend if the flattened layer were itself re-referenced). Resolve
/// the authored ops (prepend/append/add/delete/explicit) into one explicit op
/// carrying the composed name set. No-op for prims with no authored apiSchemas
/// or authored as `apiSchemas = None`.
///
/// MUST run only at the END of flatten, not between composition iterations: a
/// mid-flatten reset-to-explicit would shadow apiSchemas a later arc still adds.
///
/// @param[in,out] layer Fully-flattened Layer to finalize.
///
void FlattenAppliedSchemas(Layer &layer);

///
/// Opt-in: when making an authored RELATIVE asset path absolute on flatten,
/// lexically NORMALIZE the result (collapse `.`/`..`), matching OpenUSD usdcat
/// (`.../Meshes/Foo/../../../Materials/x.png` -> `.../Materials/x.png`). Default
/// false keeps lightusd's existing behavior (the joined path retains `..`).
///
/// MUST be set BEFORE composition/flatten (it is consumed while resolving asset
/// attributes, not at print time).
///
void SetNormalizeAssetPathOnFlatten(bool enable);
bool GetNormalizeAssetPathOnFlatten();

///
/// Return true when any PrimSpec in the Layer contains `over` Prim.
///
/// @param[in] layer Layer
///
bool HasOver(const Layer &layer);


///
/// Load subLayer USD files in `layer`, and return composited(flattened) Layer
/// to `composited_layer` Supply AssetResolutionResolver
///
bool CompositeSublayers(
    AssetResolutionResolver &resolver /* inout */, const Layer &layer,
    Layer *composited_layer, std::string *warn, std::string *err,
    const SublayersCompositionOptions options = SublayersCompositionOptions());

///
/// In-place version of CompositeSublayers that frees memory progressively
///
bool CompositeSublayersInPlace(
    AssetResolutionResolver &resolver /* inout */, std::unique_ptr<Layer> layer,
    Layer *composited_layer, std::string *warn, std::string *err,
    const SublayersCompositionOptions options = SublayersCompositionOptions());

///
/// Resolve `references` for each PrimSpe, and return composited(flattened)
/// Layer to `composited_layer` in `layer`.
///
bool CompositeReferences(AssetResolutionResolver &resolver /* inout */,
                         const Layer &layer, Layer *composited_layer,
                         std::string *warn, std::string *err,
                         const ReferencesCompositionOptions options =
                             ReferencesCompositionOptions());

///
/// In-place version of CompositeReferences that frees memory progressively
///
bool CompositeReferencesInPlace(AssetResolutionResolver &resolver /* inout */,
                                std::unique_ptr<Layer> layer, Layer *composited_layer,
                                std::string *warn, std::string *err,
                                const ReferencesCompositionOptions options =
                                    ReferencesCompositionOptions());

///
/// Resolve `payload` for each PrimSpec, and return composited(flattened) Layer
/// to `composited_layer` in `layer`.
///
bool CompositePayload(
    AssetResolutionResolver &resolver /* inout */, const Layer &layer,
    Layer *composited_layer, std::string *warn, std::string *err,
    const PayloadCompositionOptions options = PayloadCompositionOptions());

///
/// In-place version of CompositePayload that frees memory progressively
///
bool CompositePayloadInPlace(
    AssetResolutionResolver &resolver /* inout */, std::unique_ptr<Layer> layer,
    Layer *composited_layer, std::string *warn, std::string *err,
    const PayloadCompositionOptions options = PayloadCompositionOptions());

///
/// Resolve `variantSet` for each PrimSpec, and return composited(flattened) Layer
/// to `composited_layer` in `layer`.
/// Use variant selection info in each PrimSpec.
/// To externally specify variants to select, Use `ApplyVariantSelector`.
///
bool CompositeVariant(
    const Layer &layer,
    Layer *composited_layer, std::string *warn, std::string *err);

///
/// In-place version of CompositeVariant: consumes `layer` instead of
/// deep-copying it (variant selection needs no pristine-layer lookups, so the
/// copy the const-ref API makes is pure overhead on large scenes).
///
bool CompositeVariantInPlace(std::unique_ptr<Layer> layer,
                             Layer *composited_layer, std::string *warn,
                             std::string *err);

///
/// Resolve `specializes` for each PrimSpec, and return composited(flattened) Layer
/// to `composited_layer` in `layer`.
///
bool CompositeSpecializes(const Layer &layer,
    Layer *composited_layer, std::string *warn, std::string *err);

///
/// Resolve `inherits` for each PrimSpec, and return composited(flattened) Layer
/// to `composited_layer` in `layer`.
///
bool CompositeInherits(const Layer &layer,
    Layer *composited_layer, std::string *warn, std::string *err);

///
/// Override a PrimSpec with another PrimSpec.
///
/// @param[inout] dst PrimSpec to be override(must be `def` or `class` spec)
/// @param[in] src PrimSpec for override(must be `over` spec)
///
/// @return true upon success. false when error.
///
bool OverridePrimSpec(PrimSpec &dst, const PrimSpec &src, std::string *warn,
                      std::string *err);

///
/// Inherit PrimSpec. All PrimSpec tree in `src` PrimSpec will be inheritated to
/// `dst` PrimSpec.
///
/// @param[inout] dst PrimSpec to be inheritated
/// @param[in] src Source PrimSpec. Source PrimSpec can be any specifier(i.e,
/// `class`, `def` or `over`), but `class` recommended.
///
/// @return true upon success. false when error.
///
bool InheritPrimSpec(PrimSpec &dst, const PrimSpec &src, std::string *warn,
                     std::string *err);

///
/// Move-in variant: identical result, but `src` is consumed (valid-but-
/// unspecified afterwards). Removes one PrimSpec-subtree deep copy when the
/// caller passes a discardable prepared arc source.
///
bool InheritPrimSpec(PrimSpec &dst, PrimSpec &&src, std::string *warn,
                     std::string *err);


///
/// Apply `layerRelocates` to the composed layer.
/// Relocates rename prims in the namespace per AOUSD Core Spec 10.3.2.6.
///
bool CompositeRelocates(const Layer &in_layer,
    Layer *composited_layer, std::string *warn, std::string *err);

///
/// Composite all composition arcs in LIVERPS order.
///
/// AOUSD Core Spec 10.4: Applies arcs in strength order:
///   L(ocal/sublayers) > I(nherits) > V(ariants) > R(eferences) > P(ayloads) > S(pecializes)
///
/// Specializes (S) are applied last and are globally weaker than all other
/// opinions, per Spec 10.4.1.
///
/// @param[in] resolver Asset resolution resolver
/// @param[in] layer Source layer (after sublayer composition)
/// @param[out] composited_layer Output composited layer
/// @param[out] warn Warning messages
/// @param[out] err Error messages
/// @return true upon success
///
bool CompositeAllArcs(AssetResolutionResolver &resolver, const Layer &layer,
                      Layer *composited_layer, std::string *warn,
                      std::string *err);

///
/// Options for CompositeAllArcs: per-arc-type composition options for the
/// R and P phases (parent-relative path policy, asset size caps, the shared
/// parsed-layer cache, ...). The option-less overload uses defaults.
///
struct AllArcsCompositionOptions {
  ReferencesCompositionOptions references;
  PayloadCompositionOptions payload;
};

bool CompositeAllArcs(AssetResolutionResolver &resolver, const Layer &layer,
                      Layer *composited_layer, std::string *warn,
                      std::string *err,
                      const AllArcsCompositionOptions &options);

///
/// Build USD Stage from Layer
///
/// `layer` object will be destroyed after `stage` is being build.
///
bool LayerToStage(Layer &&layer, Stage *stage, std::string *warn,
                  std::string *err);

///
/// Build USD Stage from Layer with in-place memory management
///
/// This version takes ownership of the Layer via unique_ptr and progressively
/// frees memory as PrimSpecs are converted to Prims. This significantly reduces
/// peak memory usage during conversion.
///
/// @param[in] layer Unique pointer to Layer. Will be reset after conversion.
/// @param[out] stage Output Stage
/// @param[out] warn Warning messages
/// @param[out] err Error messages
/// @return true upon success
///
bool LayerToStageInPlace(std::unique_ptr<Layer> layer, Stage *stage,
                         std::string *warn, std::string *err);

///
/// Convert PrimSpec to Prim with in-place memory management
///
/// Takes ownership of PrimSpec and frees its memory after conversion.
/// This is useful for converting individual PrimSpecs without keeping
/// the source data in memory.
///
/// @param[in] primspec Unique pointer to PrimSpec. Will be reset after conversion.
/// @param[out] prim Output Prim
/// @param[out] warn Warning messages
/// @param[out] err Error messages
/// @return true upon success
///
bool PrimSpecToPrimInPlace(std::unique_ptr<PrimSpec> primspec, Prim *prim,
                           std::string *warn, std::string *err);

struct VariantSelector {
  std::string selection;  // current selection
  VariantSelectionMap vsmap;
};

using VariantSelectorMap = std::map<Path, VariantSelector>;

///
/// Recursively traverse PrimSpec tree and collect variantSelection information.
///
/// key : PrimSpec path(e.g. "/root/xform0")
/// value : VariantSelectionInfo
///
/// TODO: Move to Tydra API?
///
bool ListVariantSelectionMaps(const Layer &layer, VariantSelectorMap &m);

///
/// Select variant(PrimSpec subtree) `variant_name` from `src` PrimSpec and
/// write it to `dst` PrimSpec.
///
/// @param[inout] dst PrimSpec where selected variant are written.
/// @param[in] src Source PrimSpec. Source PrimSpec.
/// @param[in] variant_selection Variant Selection list. key = variantSet name, value = variant name. Can be empty(when empty, use PrimSpec's variants information)
///
/// @return true upon success. false when error. No error when any of variant info in `variant_selection` does not exist in `src` PrimSpec.
///
bool VariantSelectPrimSpec(PrimSpec &dst, const PrimSpec &src,
                           const std::map<std::string, std::string> &variant_selection, std::string *warn,
                           std::string *err);

///
/// Move-in variant of VariantSelectPrimSpec: identical result, but `src` is
/// consumed (left in a valid-but-unspecified state). Prefer this when the
/// caller immediately replaces `src` with `dst` — it removes one full
/// PrimSpec-subtree deep copy per variant prim and all copies for prims
/// without variants.
///
bool VariantSelectPrimSpec(PrimSpec &dst, PrimSpec &&src,
                           const std::map<std::string, std::string> &variant_selection, std::string *warn,
                           std::string *err);

///
/// Resolve variant in PrimSpec tree and write result to `dst`.
/// `dst` does not contain any variant info.
///
bool ApplyVariantSelector(const Layer &layer, const VariantSelectorMap &vsmap,
                          Layer *dst, std::string *warn, std::string *err);

///
/// Handy version of ApplyVariantSelector.
/// Use same variant name for all variantSets in Prim tree.
///
bool ApplyVariantSelector(const Layer &layer, const std::string &variant_name,
                          Layer *dst, std::string *warn, std::string *err);

///
/// Implementation of `references`
///
/// Import `layer` to this PrimSpec.
///
/// @param[inout] dst PrimSpec to be referenced.
/// @param[in] layer Layer(PrimSpec tree) to reference.
/// @param[in] primPath root Prim path in `layer`. Default = invalid Path =
/// defaultPrim in `layer`.
/// @param[in] layerOffset Layer(PrimSpec tree) to reference.
///
/// Use `defaultPrim` in `layer` as the root PrimSpec to import
///
///
bool ReferenceLayerToPrimSpec(PrimSpec &dst, const Layer &layer,
                              const Path primPath = Path(),
                              const LayerOffset layerOffset = LayerOffset());

///
/// Extract Variant information from Layer.
///
/// Example:
///
/// { "/cube0" : { "variantSets" : ["colorVariant"], "variants" : { "colorVariant" : "green" } } }
///
bool ExtractVariants(const Layer &layer, Dictionary *dict, std::string *err);

///
/// Extract Variant information from Stage.
///
bool ExtractVariants(const Stage &stage, Dictionary *dict, std::string *err);


}  // namespace lightusd
