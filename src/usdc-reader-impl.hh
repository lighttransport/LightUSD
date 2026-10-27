// SPDX-License-Identifier: Apache 2.0
// Copyright 2020 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDCReader::Impl private class definition (shared across split .cc files).
// This header is NOT part of the public API.
//
#pragma once

#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include "spin-mutex.hh"
#include "usdc-reader.hh"

#include "common-macros.inc"
#include "core/prim.hh"        // Prim class
#include "core/prim-spec.hh"   // PrimSpec, Property (transitively: composition-types, prim-enums, prim-metas, variant-types)
#include "core/model-scope.hh" // Model, Scope
#include "layer.hh"
#include "lightusd.hh"
#include "value-types.hh"

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__wasi__) && !defined(__EMSCRIPTEN__)
#include <thread>
#endif

#include "crate-format.hh"
#include "crate-pprint.hh"
#include "crate-reader.hh"
#include "mmap-array-ref.hh"
#include "tiny-container.hh"
#include "integerCoding.h"
#include "lz4-compression.hh"
#include "path-util.hh"
#include "pprint-enum.hh"
#include "prim-reconstruct.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "tiny-format.hh"
#include "value-pprint.hh"
#include "usdShade.hh"
// NOTE: ascii-parser.hh (the heavy AsciiParser class, ~78ms ParseClass) is NOT
// included here — the USDC (binary crate) reader impl does not use it. Pulling it
// dragged that parse into all 8 usdc-reader TUs; only usdc-reader-property.cc
// actually uses AsciiParser and includes it directly.
#include "parser-timing.hh"
#include "enum-handlers.hh"

//
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "nonstd/expected.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

namespace lightusd {

namespace prim {

// template specialization forward decls.
// implementations will be located in prim-reconstruct.cc
#define RECONSTRUCT_PRIM_DECL(__ty)                                      \
  template <>                                                            \
  bool ReconstructPrim<__ty>(const Specifier &spec, PropertyMap &, const ReferenceList &, \
                             __ty *, std::string *, std::string *, const PrimReconstructOptions &)

RECONSTRUCT_PRIM_DECL(Xform);
RECONSTRUCT_PRIM_DECL(Model);
RECONSTRUCT_PRIM_DECL(Scope);
RECONSTRUCT_PRIM_DECL(GeomPoints);
RECONSTRUCT_PRIM_DECL(GeomMesh);
RECONSTRUCT_PRIM_DECL(GeomCapsule);
RECONSTRUCT_PRIM_DECL(GeomCube);
RECONSTRUCT_PRIM_DECL(GeomCone);
RECONSTRUCT_PRIM_DECL(GeomCylinder);
RECONSTRUCT_PRIM_DECL(GeomSphere);
RECONSTRUCT_PRIM_DECL(GeomSubset);
RECONSTRUCT_PRIM_DECL(GeomBasisCurves);
RECONSTRUCT_PRIM_DECL(GeomNurbsCurves);
RECONSTRUCT_PRIM_DECL(GeomCamera);
RECONSTRUCT_PRIM_DECL(GeomPointInstancer);
RECONSTRUCT_PRIM_DECL(SphereLight);
RECONSTRUCT_PRIM_DECL(DomeLight);
RECONSTRUCT_PRIM_DECL(DiskLight);
RECONSTRUCT_PRIM_DECL(DistantLight);
RECONSTRUCT_PRIM_DECL(CylinderLight);
RECONSTRUCT_PRIM_DECL(RectLight);
RECONSTRUCT_PRIM_DECL(GeometryLight);
RECONSTRUCT_PRIM_DECL(PortalLight);
RECONSTRUCT_PRIM_DECL(DomeLight_1);
RECONSTRUCT_PRIM_DECL(SkelRoot);
RECONSTRUCT_PRIM_DECL(SkelAnimation);
RECONSTRUCT_PRIM_DECL(Skeleton);
RECONSTRUCT_PRIM_DECL(BlendShape);
RECONSTRUCT_PRIM_DECL(Material);
RECONSTRUCT_PRIM_DECL(Shader);
RECONSTRUCT_PRIM_DECL(NodeGraph);

#undef RECONSTRUCT_PRIM_DECL

}  // namespace prim

namespace usdc {

constexpr auto kTag = "[USDC]";

static inline bool IsUnregisteredValueType(const std::string &typeName)
{
  std::string tyname = typeName;

  if (endsWith(typeName, "[]")) {
    tyname = removeSuffix(typeName, "[]");
  }

  if (tyname == value::TypeTraits<value::uint2>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::uint3>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::uint4>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<char>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::char2>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::char3>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::char4>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::uchar2>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::uchar3>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::uchar4>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<int16_t>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::short2>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::short3>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::short4>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<uint16_t>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::ushort2>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::ushort3>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::ushort4>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::matrix2f>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::matrix3f>::type_name()) {
    return true;
  }
  if (tyname == value::TypeTraits<value::matrix4f>::type_name()) {
    return true;
  }

  return false;
}

class USDCReader::Impl {
 public:
  Impl(StreamReader *sr, const USDCReaderConfig &config) : _sr(sr) {
    set_reader_config(config);
    value::RegisterPrimAttrTypes(_supported_prim_attr_types);
  }

  ~Impl() {
    delete crate_reader;
    crate_reader = nullptr;
  }

  // Prevent copy/move — raw owning pointer in crate_reader.
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  void set_reader_config(const USDCReaderConfig &config) {
    _config = config;

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__wasi__) && !defined(__EMSCRIPTEN__)
    if (_config.numThreads == -1) {
      _config.numThreads =
          (std::max)(1, int(std::thread::hardware_concurrency()));
    }
    // Limit to 1024 threads.
    _config.numThreads = (std::min)(1024, _config.numThreads);
#else
    _config.numThreads = 1;
#endif
  }

  const USDCReaderConfig get_reader_config() const {
    return _config;
  }

  void set_progress_callback(usdc::ProgressCallback callback, void *userptr) {
    _progress_callback = callback;
    _progress_userptr = userptr;
  }

  bool ReadUSDC();

  using PathIndexToSpecIndexMap = std::unordered_map<uint32_t, uint32_t>;

  ///
  /// Construct Property(Attribute, Relationship/Connection) from
  /// FieldValuePairs
  ///
  bool ParseProperty(const SpecType specType,
                     crate::FieldValuePairVector &fvs, Property *prop,
                     bool allow_move_from_fvs = false);

  ///
  /// Parse Prim spec from FieldValuePairs
  ///
  bool ParsePrimSpec(const crate::FieldValuePairVector &fvs,
                     nonstd::optional<std::string> &typeName, /* out */
                     nonstd::optional<Specifier> &specifier,  /* out */
                     std::vector<value::token> &primChildren,   /* out */
                     std::vector<value::token> &properties,   /* out */
                     PrimMeta &primMeta);                     /* out */

  bool ParseVariantSetFields(
      const crate::FieldValuePairVector &fvs,
      std::vector<value::token> &variantChildren); /* out */

  template <typename T>
  bool ReconstructPrim(const Specifier &spec, const crate::CrateReader::Node &node,
                       const PathIndexToSpecIndexMap &psmap, T *prim);

  // Per-type body of ReconstructPrimFromTypeName's dispatch: reconstruct a T,
  // type-erase it into a value::Value, and wrap in a Prim. Split out as a member
  // template (defined in usdc-reader-prim-detail.inc, instantiated in the
  // usdc-reader-prim-reconstruct-*.cc siblings) so the per-type value::Value<T>
  // construction is not all codegen'd in usdc-reader-prim.cc. Returns nullptr on
  // failure.
  template <typename T>
  std::unique_ptr<Prim> ReconstructTypedPrim(
      const crate::CrateReader::Node &node, const Specifier spec,
      const PathIndexToSpecIndexMap &psmap, const std::string &prim_name,
      const std::string &primTypeName, const PrimMeta &meta,
      const std::vector<value::token> &properties,
      const std::vector<value::token> &primChildren);

  ///
  /// Reconstrcut Prim node.
  /// Returns the pointer of reconstructed Prim to `primOut`
  /// When `current` is 0(StageMeta), `primOut` is not set.
  /// `is_parent_variant` : True when parent path is Variant
  /// Uses unique_ptr for move-friendly output (no Prim copies)
  ///
  bool ReconstructPrimNode(int parent, int current, int level,
                           bool is_parent_variant,
                           const PathIndexToSpecIndexMap &psmap, Stage *stage,
                           std::unique_ptr<Prim> *primOut);

  ///
  /// Reconstrcut PrimSpec node.
  ///
  bool ReconstructPrimSpecNode(int parent, int current, int level,
                           bool is_parent_variant,
                           const PathIndexToSpecIndexMap &psmap, Layer *layer,
                           std::unique_ptr<PrimSpec> *primOut);

  ///
  /// Reconstruct Prim from given `typeName` string(e.g. "Xform")
  ///
  std::unique_ptr<Prim> ReconstructPrimFromTypeName(
      const std::string &typeName, const std::string &primTypeName, const std::string &prim_name,
      const crate::CrateReader::Node &node, const Specifier spec,
      const std::vector<value::token> &primChildren,
      const std::vector<value::token> &properties,
      const PathIndexToSpecIndexMap &psmap, const PrimMeta &meta, bool *is_unsupported_prim = nullptr);

  /// When `out_root_prim` is non-null the reconstructed subtree-root Prim is
  /// returned through it instead of being attached to the stage/parent
  /// (used by parallel reconstruction jobs).
  bool ReconstructPrimRecursively(int parent_id, int current_id, Prim *rootPrimPtr,
                                  int level,
                                  const PathIndexToSpecIndexMap &psmap,
                                  Stage *stage,
                                  std::unique_ptr<Prim> *out_root_prim = nullptr);

#if defined(LIGHTUSD_ENABLE_THREAD)
  ///
  /// Parallel prim reconstruction: non-variant subtrees are reconstructed on
  /// worker threads (per-thread crate stream/scratch state), shells (split
  /// interior nodes) and variant-containing subtrees stay on the calling
  /// thread, and the tree is assembled serially in traversal order so the
  /// result is identical to the serial walk.
  ///
  bool ReconstructPrimHierarchyParallel(const PathIndexToSpecIndexMap &psmap,
                                        Stage *stage);
#endif

  bool ReconstructStage(Stage *stage);

  ///
  /// For Layer
  ///

  /// `top_level_sink`: when non-null, the TOP-LEVEL call appends the
  /// reconstructed subtree root to `top_level_sink->children()` instead of
  /// the normal destination (layer->primspecs() for parent 0 / the parent
  /// PrimSpec otherwise). Used by the parallel reconstruction so worker
  /// threads never touch the shared Layer map; the child recursion always
  /// passes nullptr, so serial behavior is unchanged.
  bool ReconstructPrimSpecRecursively(int parent_id, int current_id, PrimSpec *rootPrim,
                                  int level,
                                  const PathIndexToSpecIndexMap &psmap,
                                  Layer *stage,
                                  PrimSpec *top_level_sink = nullptr);

#if defined(LIGHTUSD_ENABLE_THREAD)
  /// Parallel PrimSpec-hierarchy reconstruction — the Layer analog of
  /// ReconstructPrimHierarchyParallel above (same shells/jobs split).
  /// Produces byte-equal Layer content to the serial walk.
  bool ReconstructPrimSpecHierarchyParallel(const PathIndexToSpecIndexMap &psmap,
                                            Layer *layer);
#endif

  bool ToLayer(Layer *layer);

  ///
  /// --------------------------------------------------
  ///

  static std::string StackMessage(const std::string &outer,
                                  const std::string &inner) {
    if (inner.empty()) {
      return outer;
    }
    if (outer.empty() || outer.back() == '\n') {
      return outer + inner;
    }
    return outer + "\n" + inner;
  }

  // Diagnostics can be pushed from worker threads during parallel
  // reconstruction.
  void PushError(const std::string &s) {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _err = StackMessage(s, _err);
  }

  void PushWarn(const std::string &s) {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _warn = StackMessage(s, _warn);
  }

  // Append verbatim (no message stacking) — used to merge worker-local
  // diagnostic buffers.
  void AppendErrorRaw(const std::string &s) {
    if (s.empty()) return;
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _err += s;
  }
  void AppendWarnRaw(const std::string &s) {
    if (s.empty()) return;
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _warn += s;
  }

  std::string GetError() {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    return _err;
  }

  std::string GetWarning() {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    return _warn;
  }

  // Approximated memory usage in [mb]
  size_t GetMemoryUsage() const {
    if (!crate_reader) {
      return 0;
    }
    return crate_reader->GetMemoryUsageInMB();
  }

  USDCMemoryUsageReport GetMemoryUsageReport() const {
    USDCMemoryUsageReport report;
    if (!crate_reader) {
      return report;
    }

    report.current_usage_bytes = crate_reader->GetMemoryUsageInBytes();
    report.peak_usage_bytes = crate_reader->GetPeakMemoryUsageInBytes();
    report.max_budget_bytes = crate_reader->GetMemoryBudgetInBytes();
    report.remaining_budget_bytes = crate_reader->GetRemainingMemoryBudgetInBytes();
    return report;
  }

  // --- Internal helpers exposed for split .cc files ---

  ///
  /// Result of parsing common prim fields shared by ReconstructPrimNode
  /// and ReconstructPrimSpecNode.
  ///
  struct PrimFieldsResult {
    nonstd::optional<std::string> typeName;
    nonstd::optional<Specifier> specifier;
    std::vector<value::token> primChildren;
    std::vector<value::token> properties;
    PrimMeta primMeta;
    Path elemPath;
    std::string prim_name;      // from elemPath.prim_part()
    std::string pTyName;        // LightUSD prim type ("Model" if missing)
    std::string primTypeName;   // raw type name ("" if missing or __AnyType__)
  };

  bool ParseCommonPrimFields(
      const crate::FieldValuePairVector &fvs,
      int current,
      Specifier default_spec,
      PrimFieldsResult *result);

  nonstd::expected<APISchemas, std::string> ToAPISchemas(
      const ListOp<value::token> &, bool ignore_unknown, std::string &warn);

  // ListOp<T> -> (ListEditOp, [T])
  template <typename T>
  std::vector<std::pair<ListEditQual, std::vector<T>>> DecodeListOp(
      const ListOp<T> &arg) {
    std::vector<std::pair<ListEditQual, std::vector<T>>> dst;

    if (arg.IsExplicit()) {  // fast path
      dst.push_back({ListEditQual::ResetToExplicit, arg.GetExplicitItems()});
    } else {
      if (arg.HasExplicitItems()) {
        dst.push_back({ListEditQual::ResetToExplicit, arg.GetExplicitItems()});
      }
      // Bucket order matters: it is the order the qualifiers get PRINTED in, and
      // a crate ListOp has no record of the order they were authored in. Use
      // USD's canonical one (delete, add, prepend, append), so a prim authored
      // with `prepend references` + `append references` does not come back with
      // the two lines swapped.
      if (arg.HasDeletedItems()) {
        dst.push_back({ListEditQual::Delete, arg.GetDeletedItems()});
      }
      if (arg.HasAddedItems()) {
        dst.push_back({ListEditQual::Add, arg.GetAddedItems()});
      }
      if (arg.HasPrependedItems()) {
        dst.push_back({ListEditQual::Prepend, arg.GetPrependedItems()});
      }
      if (arg.HasAppendedItems()) {
        dst.push_back({ListEditQual::Append, arg.GetAppendedItems()});
      }
      if (arg.HasOrderedItems()) {
        dst.push_back({ListEditQual::Order, arg.GetOrderedItems()});
      }
    }

    return dst;
  }

  ///
  /// Builds std::map<std::string, Property> from the list of Path(Spec)
  /// indices.
  ///
  bool BuildPropertyMap(const std::vector<size_t> &pathIndices,
                        const PathIndexToSpecIndexMap &psmap,
                        prim::PropertyMap *props);

  /// `reserved_bytes_out` (lazy mode): memory budget reserved while decoding
  /// into `scratch`; the caller releases it (ReleaseMemoryBudget) once the
  /// decoded values are consumed.
  bool ResolveFieldValuePairs(
      const crate::Spec &spec,
      const crate::FieldValuePairVector **fvs,
      crate::FieldValuePairVector *scratch,
      uint64_t *reserved_bytes_out = nullptr);

  bool ReconstrcutStageMeta(const crate::FieldValuePairVector &fvs,
                            StageMetas *out);

  Prim *ResolveVariantOwnerPrim(int32_t parent_node_id, int32_t current_node_id,
                                Prim *current_prim, Prim *parent_prim,
                                bool include_current_node);

  bool AttachVariantPrimChildrenToOwner(int32_t owner_node_id, Prim *owner_prim);

  bool IsExpectedNonPrimVariantChild(
      int32_t node_id, const PathIndexToSpecIndexMap &psmap) const;

  bool AddVariantChildrenToPrimNode(
      int32_t parent_prim_idx, int32_t prim_idx, const std::string &variantSetName, const std::vector<value::token> &variantChildren) {
    if (parent_prim_idx < 0) {
      return false;
    }

    if (prim_idx < 0) {
      return false;
    }

    _variantChildren[uint32_t(parent_prim_idx)][prim_idx] = {variantSetName, variantChildren};

    return true;
  }

  bool AddVariantChildrenToVariantNode(
      int32_t parent_variant_idx, int32_t prim_idx, const std::string &variantSetName, const std::vector<value::token> &variantChildren) {
    if (parent_variant_idx < 0) {
      return false;
    }

    if (prim_idx < 0) {
      return false;
    }

    // Store variant children for variant nodes (nested variants)
    _variantChildren[uint32_t(parent_variant_idx)][prim_idx] = {variantSetName, variantChildren};

    return true;
  }

  bool FindVariantSet(int32_t parent_prim_idx, const std::string &variantSetName, uint32_t &spec_path_idx) {
    if (parent_prim_idx < 0) {
      return false;
    }

    for (const auto &it : _variantChildren[uint32_t(parent_prim_idx)]) {
      if (variantSetName == it.second.first) {
        spec_path_idx = uint32_t(it.first);
        return true;
      }
    }

    return false;

  }

  bool AddVariantToPrimNode(int32_t prim_idx, const value::Value &variant);

  crate::CrateReader *crate_reader{nullptr};

  StreamReader *_sr = nullptr;
  std::string _err;
  std::string _warn;
  SpinMutex _diag_mutex;

  USDCReaderConfig _config;

  usdc::ProgressCallback _progress_callback;
  void *_progress_userptr{nullptr};

  nonstd::optional<Path> GetPath(crate::Index index) const {
    if (index.value < _paths->size()) {
      return (*_paths)[index.value];
    }

    return nonstd::nullopt;
  }

  nonstd::optional<Path> GetElemPath(crate::Index index) const {
    if (index.value < _elemPaths->size()) {
      return (*_elemPaths)[index.value];
    }

    return nonstd::nullopt;
  }

  // Direct output version - avoids optional extraction overhead
  bool GetPathDirect(crate::Index index, Path *out) const {
    if (!out) return false;
    if (index.value < _paths->size()) {
      *out = (*_paths)[index.value];
      return true;
    }
    return false;
  }

  /// Borrowed path for `index`, or nullptr when out of range. Prefer this over
  /// GetPath()/GetPathDirect() on hot paths: it neither copies the Path nor
  /// builds an optional. Valid while crate_reader exists.
  const Path *GetPathRef(crate::Index index) const {
    if (index.value < _paths->size()) {
      return &(*_paths)[index.value];
    }
    return nullptr;
  }

  bool GetElemPathDirect(crate::Index index, Path *out) const {
    if (!out) return false;
    if (index.value < _elemPaths->size()) {
      *out = (*_elemPaths)[index.value];
      return true;
    }
    return false;
  }

  // Use const references to avoid copying data from crate_reader when possible
  // NOTE: These are only valid while crate_reader exists
  const std::vector<crate::CrateReader::Node> *_nodes = nullptr;
  const std::vector<crate::Spec> *_specs = nullptr;
  const std::vector<crate::Field> *_fields = nullptr;
  const std::vector<crate::Index> *_fieldset_indices = nullptr;
  const std::vector<crate::Index> *_string_indices = nullptr;
  const std::vector<Path> *_paths = nullptr;
  const std::vector<Path> *_elemPaths = nullptr;

  const crate::LiveFieldSetMap
      *_live_fieldsets = nullptr;  // <fieldset index, List of field with unpacked Values>

  // VariantSet Spec.
  // key = parent idx, value = (prim_id, (variantSetName, variantChildren names))
  std::map<uint32_t, std::unordered_map<int32_t, std::pair<std::string, std::vector<value::token>>>> _variantChildren;

  std::unordered_map<int32_t, Prim> _prims; // For Stage

  // For Prim/Props defined as Variant(SpecType::VariantSet)
  // key = path index.
  std::unordered_map<int32_t, Prim> _variantPrims; // For Stage
  std::map<int32_t, PrimSpec> _variantPrimSpecs; // For Layer
  std::map<int32_t, std::pair<Path, Property>> _variantProps;
  std::map<int32_t, Variant> _variants;

  // key = parent path index, values = keys to `_variantPrims`, `_variantProps`
  std::map<int32_t, std::vector<int32_t>> _variantPrimChildren;
  std::map<int32_t, std::vector<int32_t>> _variantPropChildren;

  // Fast lookup for variant prims by name: {variantSetName, variantName} -> path index
  std::map<std::pair<std::string, std::string>, int32_t> _variantNameIndex;

  // Check if given node_id is a prim node. Flag-per-node instead of a
  // std::set: parallel reconstruction workers mark disjoint node ids
  // concurrently (element writes to a pre-sized vector<uint8_t> are safe;
  // shells/ancestors are marked before workers start).
  struct PrimNodeFlagTable {
    void reset(size_t num_nodes) { _f.assign(num_nodes, 0); }
    size_t count(int64_t idx) const {
      return ((idx >= 0) && (size_t(idx) < _f.size()) && _f[size_t(idx)]) ? 1
                                                                          : 0;
    }
    void insert(int64_t idx) {
      if ((idx >= 0) && (size_t(idx) < _f.size())) {
        _f[size_t(idx)] = 1;
      }
    }
    std::vector<uint8_t> _f;
  };
  PrimNodeFlagTable _prim_table;

  std::set<std::string> _supported_prim_attr_types;

  // mmap zero-copy support
  MMapArrayTable _mmap_table;
  std::string _current_prim_path;  // Set during reconstruction for mmap table key building
  MMapArrayRef _pending_mmap_ref{};
  bool _has_pending_mmap_ref{false};
};

}  // namespace usdc
}  // namespace lightusd
