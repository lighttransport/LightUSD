// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

//
#include "nonstd/optional.hpp"
//
#include "crate-format.hh"
#include "dynamic-bitset.hh"
#include "memory-budget.hh"
#include "core/prim-spec.hh"  // PrimSpec (transitively: property, composition-types, prim-enums, prim-metas, variant-types)
#include "spin-mutex.hh"
#include "stream-reader.hh"
#include "typed-array-core.hh"

namespace lightusd {
namespace crate {

///
/// Progress callback function type.
/// @param[in] progress Progress value between 0.0 and 1.0
/// @param[in] userptr User-provided pointer for custom data
/// @return true to continue parsing, false to cancel
///
using ProgressCallback = std::function<bool(float progress, void *userptr)>;

// Use the iterative PathIndex tree decoder. It avoids recursion depth risk on
// attacker-controlled crate path tables.
#define LIGHTUSD_CRATE_USE_FOR_BASED_PATH_INDEX_DECODER

///
/// Configuration for secure USDC (Crate binary) parsing.
/// These limits are essential for security to prevent malicious files from
/// causing infinite loops, buffer overruns, or out-of-memory conditions.
///
struct CrateReaderConfig {
  int numThreads = -1;                   ///< Number of threads (-1 = auto-detect)
  bool use_mmap = false;                 ///< Use mmap for reading uncompressed arrays

  // Security limits for malicious Crate data
  size_t maxTOCSections = 32;            ///< Maximum number of TOC sections

  size_t maxNumTokens = 1024 * 1024 * 64;        ///< Max tokens (64M)
  size_t maxNumStrings = 1024 * 1024 * 64;       ///< Max string entries (64M)
  size_t maxNumFields = 1024 * 1024 * 256;       ///< Max field entries (256M)
  size_t maxNumFieldSets = 1024 * 1024 * 256;    ///< Max fieldset entries (256M)
  size_t maxNumSpecifiers = 1024 * 1024 * 256;   ///< Max spec entries (256M)
  size_t maxNumPaths = 1024 * 1024 * 256;        ///< Max path entries (256M)

  size_t maxNumIndices = 1024 * 1024 * 256;      ///< Max index entries (256M)
  size_t maxDictElements = 256;                   ///< Max dictionary elements
  size_t maxArrayElements = 1024 * 1024 * 1024;  ///< Max array elements (1B)
  size_t maxAssetPathElements = 512;              ///< Max asset path components

  size_t maxTokenLength = 1024 * 64;               ///< Max token string length (64K)
  size_t maxStringLength = 1024 * 1024 * 64;     ///< Max string length (64MB)

  size_t maxVariantsMapElements = 128;            ///< Max variant map elements

  size_t maxValueRecursion = 16;                         ///< Max value unpack recursion depth
  size_t maxPathIndicesDecodeIteration = 1024 * 1024 * 256; ///< Max path decode iterations

  size_t maxInts = 1024 * 1024 * 1024;            ///< Max generic int array size (1B)

  ///< Total memory budget for uncompressed data in bytes (default 2GB)
  size_t maxMemoryBudget = 1024ull * 1024 * 1024;  ///< 1 GB default

  /// Optional external memory budget manager shared with other components.
  /// If set, CrateReader uses this manager instead of an internal one.
  MemoryBudgetManager *memory_budget_manager = nullptr;
};

// Enable SoA (Struct of Arrays) layout for TypedTimeSamples
// Default is AoS (Array of Structs) layout
// #define LIGHTUSD_CRATE_TIMESAMPLES_USE_SOA


///
/// Secure USDC (Crate binary format) reader.
///
/// This reader provides memory-safe parsing of USD binary files with extensive
/// security checks and configurable limits to prevent malicious file attacks.
/// The Crate format is Pixar's binary serialization of USD data.
///
/// Key security features:
/// - Memory budget enforcement
/// - Bounds checking on all reads
/// - Configurable limits on data structures
/// - Protection against infinite loops and recursion
///
/// Usage:
/// ```cpp
/// lightusd::StreamReader reader(filename);
/// lightusd::crate::CrateReader cratereader(&reader);
/// lightusd::Layer layer;
/// if (cratereader.Read(&layer)) {
///   // Success - use the layer
/// } else {
///   std::cerr << "Read error: " << cratereader.GetError() << std::endl;
/// }
/// ```
///

/// Clear all dedup entries for TimeSamples arrays (called at start of each file load)
void clear_all_timesamples_dedup_entries();

/// Clear dedup entries for a specific TimeSamples pointer
void clear_timesamples_dedup_entries(void* timesamples_ptr);

struct CrateIndexFNV1Hash {
  size_t operator()(const crate::Index &idx) const noexcept {
    static constexpr uint64_t kFNV_Prime = 0x00000100000001B3ull;
    static constexpr uint64_t kFNV_Offset_Basis = 0xcbf29ce484222325ull;

    uint64_t hash = kFNV_Offset_Basis;
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&idx.value);
    for (size_t i = 0; i < sizeof(idx.value); ++i) {
      hash = (kFNV_Prime * hash) ^ ptr[i];
    }
    return static_cast<size_t>(hash);
  }
};

using LiveFieldSetMap =
    std::unordered_map<crate::Index, crate::FieldValuePairVector,
                       CrateIndexFNV1Hash>;

class CrateReader {
 public:
  ///
  /// Intermediate Node data structure for scene graph.
  /// This does not contain actual prim/property data.
  ///
  class Node {
   public:
    // -2 = initialize as invalid node
    Node() : _parent(-2) {}

    Node(int64_t parent, Path &path) : _parent(parent), _path(path) {}

    int64_t GetParent() const { return _parent; }

    const std::vector<size_t> &GetChildren() const { return _children; }

    ///
    /// child_name is used when reconstructing scene graph.
    /// Return false when `child_name` is already added to a children.
    ///
    bool AddChildren(const std::string &child_name, size_t node_index) {
      if (_primChildren.count(child_name)) {
        return false;
      }
      // assert(_primChildren.count(child_name) == 0);
      _primChildren.emplace(child_name);
      _children.push_back(node_index);
      return true;
    }

    ///
    /// Get full path(e.g. `/muda/dora/bora` when the parent is `/muda/dora` and
    /// this node is `bora`)
    ///
    // std::string GetFullPath() const { return _path.full_path_name(); }

    ///
    /// Get local path
    ///
    std::string GetLocalPath() const { return _path.full_path_name(); }

    ///
    /// Element Path(= name of Prim. Tokens in `primChildren` field). Prim node
    /// only.
    ///
    void SetElementPath(Path &path) { _elemPath = path; }

    nonstd::optional<std::string> GetElementName() const {
      if (_elemPath.is_relative_path()) {
        return _elemPath.full_path_name();
      } else {
        return nonstd::nullopt;
      }
    }

    // Element path(e.g. `geom0`)
    const Path &GetElementPath() const { return _elemPath; }

    // Full path(e.g. `/root/geom0`
    const Path &GetPath() const { return _path; }

    // crate::CrateDataType GetNodeDataType() const { return _node_type; }

    const std::unordered_set<std::string> &GetPrimChildren() const {
      return _primChildren;
    }

    // void SetAssetInfo(const value::dict &dict) { _assetInfo = dict; }
    // const value::dict &GetAssetInfo() const { return _assetInfo; }

   private:
    int64_t
        _parent;  // -1 = this node is the root node. -2 = invalid or leaf node
    std::vector<size_t> _children;  // index to child nodes.
    std::unordered_set<std::string>
        _primChildren;  // List of name of child nodes

    Path _path;  // local path
    // value::dict _assetInfo;
    Path _elemPath;

    // value::TypeId _node_type;
    // NodeType _node_type;
  };

 public:
 private:
  CrateReader() = delete;

 public:
  CrateReader(StreamReader *sr,
              const CrateReaderConfig &config = CrateReaderConfig());
  ~CrateReader();

  ///
  /// Set progress callback for monitoring parsing progress.
  ///
  /// @param[in] callback Function to call during parsing to report progress
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetProgressCallback(ProgressCallback callback, void *userptr = nullptr) {
    _progress_callback = callback;
    _progress_userptr = userptr;
  }

  bool ReadBootStrap();
  bool ReadTOC();

  ///
  /// Read TOC section
  ///
  bool ReadSection(crate::Section *s);

  // Read known sections
  bool ReadPaths();
  bool ReadTokens();
  bool ReadStrings();
  bool ReadFields();
  bool ReadFieldSets();
  bool ReadSpecs();

  bool BuildLiveFieldSets();

  /// Decode one fieldset on demand from `fieldset_index`.
  /// Unlike BuildLiveFieldSets(), this does not materialize all fieldsets.
  bool DecodeFieldSet(crate::Index fieldset_index, FieldValuePairVector *pairs);

  std::string GetError();
  std::string GetWarning();

  // Approximated memory usage in [mb]
  size_t GetMemoryUsageInMB() const {
    return memory_manager_->GetUsageInMB();
  }

  uint64_t GetMemoryUsageInBytes() const {
    return memory_manager_->GetCurrentUsage();
  }

  uint64_t GetPeakMemoryUsageInBytes() const {
    return memory_manager_->GetPeakUsage();
  }

  uint64_t GetMemoryBudgetInBytes() const {
    return memory_manager_->GetMaxBudget();
  }

  uint64_t GetRemainingMemoryBudgetInBytes() const {
    return memory_manager_->GetRemainingBudget();
  }

  // Release memory budget externally (e.g., when scratch buffers are freed
  // after lazy DecodeFieldSet)
  void ReleaseMemoryBudget(uint64_t bytes) {
    memory_manager_->Release(bytes);
  }

  /// Release decompression buffers and return their budget.
  /// Call after parsing is complete to reclaim memory.
  void ShrinkDecompressionBuffers() {
    if (_decomp_comp_buffer_budget > 0) {
      memory_manager_->Release(_decomp_comp_buffer_budget);
      _decomp_comp_buffer_budget = 0;
    }
    { std::vector<char> tmp; _decomp_comp_buffer.swap(tmp); }

    if (_decomp_working_buffer_budget > 0) {
      memory_manager_->Release(_decomp_working_buffer_budget);
      _decomp_working_buffer_budget = 0;
    }
    { std::vector<char> tmp; _decomp_working_buffer.swap(tmp); }
  }

  /// -------------------------------------
  /// Following Methods are valid after successfull parsing of Crate data.
  ///
  size_t NumNodes() const { return _nodes.size(); }

  const std::vector<Node> &GetNodes() const { return _nodes; }

  const std::vector<value::token> &GetTokens() const { return _tokens; }

  const std::vector<crate::Index> &GetStringIndices() const {
    return _string_indices;
  }

  const std::vector<crate::Field> &GetFields() const { return _fields; }

  const std::vector<crate::Index> &GetFieldsetIndices() const {
    return _fieldset_indices;
  }

  const std::vector<Path> &GetPaths() const { return _paths; }

  const std::vector<Path> &GetElemPaths() const { return _elemPaths; }

  const std::vector<crate::Spec> &GetSpecs() const { return _specs; }

  const LiveFieldSetMap &GetLiveFieldSets() const {
    return _live_fieldsets;
  }

  const uint8_t *GetVersion() const { return _version; }

  int64_t GetTOCOffset() const { return _toc_offset; }

  const crate::TableOfContents &GetTableOfContents() const { return _toc; }

  bool IsFieldSetStartIndex(crate::Index index) const {
    static constexpr uint32_t kInvalidFieldSetEnd = ~0u;
    return (index.value < _fieldset_end_indices.size()) &&
           (_fieldset_end_indices[index.value] != kInvalidFieldSetEnd);
  }

  const nonstd::optional<value::token> GetToken(crate::Index token_index) const;
  const nonstd::optional<value::token> GetStringToken(
      crate::Index string_index) const;

  bool HasField(const std::string &key) const;
  nonstd::optional<crate::Field> GetField(crate::Index index) const;
  nonstd::optional<std::string> GetFieldString(crate::Index index) const;
  nonstd::optional<std::string> GetSpecString(crate::Index index) const;

  size_t NumPaths() const { return _paths.size(); }

  nonstd::optional<Path> GetPath(crate::Index index) const;
  nonstd::optional<Path> GetElementPath(crate::Index index) const;
  nonstd::optional<std::string> GetPathString(crate::Index index) const;

  ///
  /// Find if a field with (`name`, `tyname`) exists in FieldValuePairVector.
  ///
  bool HasFieldValuePair(const FieldValuePairVector &fvs,
                         const std::string &name, const std::string &tyname);

  ///
  /// Find if a field with `name`(type can be arbitrary) exists in
  /// FieldValuePairVector.
  ///
  bool HasFieldValuePair(const FieldValuePairVector &fvs,
                         const std::string &name);

  nonstd::expected<FieldValuePair, std::string> GetFieldValuePair(
      const FieldValuePairVector &fvs, const std::string &name,
      const std::string &tyname);

  nonstd::expected<FieldValuePair, std::string> GetFieldValuePair(
      const FieldValuePairVector &fvs, const std::string &name);

  // bool ParseAttribute(const FieldValuePairVector &fvs,
  //                                   PrimAttrib *attr,
  //                                   const std::string &prop_name);

  bool VersionGreaterThanOrEqualTo_0_8_0() const {
    if (_version[0] > 0) {
      return true;
    }

    if (_version[1] >= 8) {
      return true;
    }

    return false;
  }

 private:
  /// Report progress during parsing
  bool ReportProgress(float progress);

  /// Validate an uncompressed payload before allocating its destination.
  bool CheckArrayRead(uint64_t count, size_t element_size) const;

#if defined(LIGHTUSD_CRATE_USE_FOR_BASED_PATH_INDEX_DECODER)
  // To save stack usage
  struct BuildDecompressedPathsArg {
    std::vector<uint32_t> *pathIndexes{};
    std::vector<int32_t> *elementTokenIndexes{};
    std::vector<int32_t> *jumps{};
    std::vector<bool> *visit_table{};
    size_t startIndex{}; // usually 0
    size_t endIndex{}; // inclusive. usually pathIndexes.size() - 1
    Path parentPath;
  };

  bool BuildDecompressedPathsImpl(
      BuildDecompressedPathsArg *arg);

#else
  bool BuildDecompressedPathsImpl(
      std::vector<uint32_t> const &pathIndexes,
      std::vector<int32_t> const &elementTokenIndexes,
      std::vector<int32_t> const &jumps,
      std::vector<bool> &visit_table,  // track visited pathIndex to prevent
                                       // circular referencing
      size_t curIndex, const Path &parentPath);
#endif

  bool UnpackValueRep(const crate::ValueRep &rep, crate::CrateValue *value);
  bool UnpackInlinedValueRep(const crate::ValueRep &rep,
                             crate::CrateValue *value);
  bool IsThreadSafeInlinedValueRep(const crate::ValueRep &rep) const;
  bool DecodeFieldSetRange(uint32_t start_idx, uint32_t end_idx,
                           FieldValuePairVector *pairs);

  /// Describe an uncompressed array ValueRep without reading data.
  /// Records offset/count in *ref, sets *value to empty typed array.
  /// Returns false if the ValueRep is not eligible (compressed, inlined, scalar).
  bool DescribeValueRep(const crate::ValueRep &rep,
                        MMapArrayRef *ref, crate::CrateValue *value);

  bool UnpackValueRepsToTimeSamples(const std::vector<double> &times,
    const std::vector<crate::ValueRep> &vreps,
    value::TimeSamples *d);

  // implementation in crate-reader-timesamples.cc
  bool UnpackTimeSampleValue_BOOL(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_UCHAR(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_INT32(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_UINT32(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_INT64(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_UINT64(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_HALF(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_FLOAT(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_DOUBLE(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_HALF2(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_HALF3(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_HALF4(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_INT2(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_INT3(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_INT4(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_FLOAT2(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_FLOAT3(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_FLOAT4(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_DOUBLE2(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_DOUBLE3(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_DOUBLE4(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_QUATH(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_QUATF(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_QUATD(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_ASSET_PATH(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_STRING(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_TOKEN(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_MATRIX2D(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_MATRIX3D(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);
  bool UnpackTimeSampleValue_MATRIX4D(double t, const crate::ValueRep &rep, value::TimeSamples &dst, size_t expected_total_samples = 0);

  // times(double[])
  bool UnpackTimeSampleTimes(const crate::ValueRep &rep, std::vector<double> &dst);

  // Build O(1) lookup table for fieldset start -> end(terminator) index.
  bool BuildFieldSetBoundaryIndex();

  bool ReadCompressedPaths(const uint64_t ref_num_paths);

  template <class Int>
  bool ReadCompressedInts(Int *out, size_t num_elements);

  bool ReadIndices(std::vector<crate::Index> *is);
  bool ReadIndex(crate::Index *i);
  bool ReadString(std::string *s);
  bool ReadValueRep(crate::ValueRep *rep);
  // Decode a VtArrayEdit ValueRep (IsArrayEdit bit set, crate >= 0.14.0).
  bool UnpackArrayEditRep(const crate::ValueRep &rep, crate::CrateValue *value);

  bool ReadPathArray(std::vector<Path> *d);
  bool ReadStringArray(std::vector<std::string> *d);
  bool ReadLayerOffsetArray(std::vector<LayerOffset> *d);

  bool ReadReference(Reference *d);
  bool ReadPayload(Payload *d);
  bool ReadLayerOffset(LayerOffset *d);

  // customData(Dictionary)
  bool ReadCustomData(CustomDataType *d);

  bool ReadTimeSamples(value::TimeSamples *d);


  // integral array
  template <typename T>
  bool ReadIntArray(bool is_compressed, std::vector<T> *d);

  // TypedArray versions for mmap support
  template <typename T>
  bool ReadIntArrayTyped(bool is_compressed, TypedArray<T> *d);

  bool ReadHalfArray(bool is_compressed, std::vector<value::half> *d);
  bool ReadFloatArray(bool is_compressed, std::vector<float> *d);
  bool ReadDoubleArray(bool is_compressed, std::vector<double> *d);

  // TypedArray versions for mmap support
  bool ReadFloatArrayTyped(bool is_compressed, TypedArray<float> *d);
  bool ReadFloat2ArrayTyped(TypedArray<value::float2> *d);
  bool ReadDoubleArrayTyped(bool is_compressed, TypedArray<double> *d);

  bool ReadDoubleVector(std::vector<double> *d);

  // template <class T>
  // struct IsIntType {
  //   static const bool value =
  //     std::is_same<T, int32_t>::value ||
  //     std::is_same<T, uint32_t>::value ||
  //     std::is_same<T, int64_t>::value ||
  //     std::is_same<T, uint64_t>::value;
  // };

  template <typename T>
  bool ReadArray(std::vector<T> *d);

  // template <typename T,
  // typename std::enable_if<IsIntType<T>::value, bool>::type>
  // bool ReadArray(std::vector<T> *d);

  template <typename T>
  bool ReadListOp(ListOp<T> *d);

  // TODO: Templatize
  bool ReadPathListOp(ListOp<Path> *d);
  bool ReadTokenListOp(ListOp<value::token> *d);
  bool ReadStringListOp(ListOp<std::string> *d);
  // bool ReadIntListOp(ListOp<int32_t> *d);
  // bool ReadUIntListOp(ListOp<uint32_t> *d);
  // bool ReadInt64ListOp(ListOp<int64_t> *d);
  // bool ReadUInt64ListOp(ListOp<uint64_t> *d);
  // bool ReadReferenceListOp(ListOp<Reference> *d);
  // bool ReadPayloadListOp(ListOp<Payload> *d);

  bool ReadVariantSelectionMap(VariantSelectionMap *d);

  template <typename T>
  bool ReadTimeSampleScalarValue(T *value, size_t nbytes,
                                 const char *read_error) {
    if (!memory_manager_->Reserve(nbytes)) {
      PushError("[Crate]: Reached maximum memory budget");
      return false;
    }
    if (!sr()->read_exact(nbytes, nbytes, reinterpret_cast<uint8_t *>(value))) {
      PushError(std::string(__func__) + "(): " + read_error);
      return false;
    }
    return true;
  }

  // Header(bootstrap)
  uint8_t _version[3] = {0, 0, 0};

  crate::TableOfContents _toc;

  int64_t _toc_offset{0};

  // index to _toc.sections
  int64_t _tokens_index{-1};
  int64_t _paths_index{-1};
  int64_t _strings_index{-1};
  int64_t _fields_index{-1};
  int64_t _fieldsets_index{-1};
  int64_t _specs_index{-1};

  std::vector<value::token> _tokens;
  std::vector<crate::Index> _string_indices;
  std::vector<crate::Field> _fields;
  std::vector<crate::Index> _fieldset_indices;
  std::vector<uint32_t> _fieldset_end_indices;   // valid only at fieldset starts
  std::vector<uint32_t> _fieldset_start_indices; // list of fieldset starts
  std::vector<crate::Spec> _specs;
  std::vector<Path> _paths;
  std::vector<Path> _elemPaths;

  std::vector<Node> _nodes;  // [0] = root node
                             //
  // `_live_fieldsets` contains unpacked value keyed by fieldset index.
  // Used for reconstructing Scene object
  LiveFieldSetMap
      _live_fieldsets;  // <fieldset index, List of field with unpacked Values>

  const StreamReader *_sr{};

  //
  // Parallel reconstruction support. The StreamReader cursor and the
  // decompression scratch buffers are per-thread state: while
  // set_parallel_io_active(true) is in effect, sr()/decomp_*() return
  // thread-local instances over the same underlying memory. All crate read
  // paths access the stream and scratch buffers exclusively through these
  // accessors.
  //
  const StreamReader *sr() const;
  std::vector<char> &decomp_comp_buffer() const;
  std::vector<char> &decomp_working_buffer() const;
  size_t &decomp_comp_buffer_budget() const;
  size_t &decomp_working_buffer_budget() const;
  std::unordered_set<uint64_t> &unpack_recursion_guard() const;
  // Per-thread (during parallel reconstruction) value-unpack nesting depths.
  uint32_t &custom_data_depth() const;
  uint32_t &array_edit_depth() const;

 public:
  /// Enable/disable per-thread stream & scratch state (called by the USDC
  /// reader around parallel prim reconstruction). Not reentrant.
  void set_parallel_io_active(bool onoff);

 private:
  bool _parallel_io_active{false};
  uint64_t _parallel_io_generation{0};

  // Diagnostics can be pushed from worker threads during parallel
  // reconstruction.
  void PushError(const std::string &s) const {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _err += s + "\n";
  }
  void PushWarn(const std::string &s) const {
    ScopedSpinLock<SpinMutex> lk(_diag_mutex);
    _warn += s + "\n";
  }
  mutable SpinMutex _diag_mutex;
  mutable std::string _err;
  mutable std::string _warn;

  ProgressCallback _progress_callback;  // Default-initialized (empty)
  void *_progress_userptr{nullptr};

  // To prevent recursive Value unpack(The Value encodes itself).
  // Access through unpack_recursion_guard() (thread-local during parallel
  // reconstruction).
  mutable std::unordered_set<uint64_t> unpackRecursionGuard;
  // Nesting depth for ReadCustomData (crate DICTIONARY) — capped against
  // _config.maxValueRecursion to stop a deeply-nested / cyclic dict from
  // overflowing the native stack. Access through custom_data_depth()
  // (thread-local during parallel reconstruction).
  mutable uint32_t _customDataDepth{0};
  // Nesting depth for UnpackArrayEditRep (VtArrayEdit) — same purpose;
  // access through array_edit_depth().
  mutable uint32_t _arrayEditDepth{0};

  bool ReserveDecompressionBuffers(size_t comp_buffer_size,
                                   size_t working_buffer_size) const;

  CrateReaderConfig _config;

  // RAII Memory budget manager (owned) and optional external manager.
  mutable MemoryBudgetManager owned_memory_manager_;
  MemoryBudgetManager *memory_manager_{nullptr};

  // Shared times cache: deduplicates times arrays that share the same file
  // offset (ValueRep payload). Multiple TimeSamples in a crate file often
  // reference the same times array; this avoids redundant copies.
  // Guarded by _times_cache_mutex (accessed from worker threads during
  // parallel reconstruction).
  std::unordered_map<uint64_t, std::shared_ptr<std::vector<double>>> _shared_times_cache;
  mutable SpinMutex _times_cache_mutex;

  // Reusable buffers for integer decompression to avoid repeated allocation
  // These are mutable because they're used as internal working buffers in const-like operations
  mutable std::vector<char> _decomp_comp_buffer;      // Buffer for compressed data
  mutable std::vector<char> _decomp_working_buffer;   // Buffer for decompression working space
  // Budget already reserved for the persistent decompression buffers above
  mutable size_t _decomp_comp_buffer_budget{0};
  mutable size_t _decomp_working_buffer_budget{0};

  class Impl;
  Impl *_impl;
};

// Explicit template instantiation declarations (defined in crate-reader-arrays.cc)
extern template bool CrateReader::ReadCompressedInts<int32_t>(int32_t*, size_t);
extern template bool CrateReader::ReadCompressedInts<uint32_t>(uint32_t*, size_t);
extern template bool CrateReader::ReadCompressedInts<int64_t>(int64_t*, size_t);
extern template bool CrateReader::ReadCompressedInts<uint64_t>(uint64_t*, size_t);
extern template bool CrateReader::ReadIntArray<int32_t>(bool, std::vector<int32_t>*);
extern template bool CrateReader::ReadIntArray<uint32_t>(bool, std::vector<uint32_t>*);
extern template bool CrateReader::ReadIntArray<int64_t>(bool, std::vector<int64_t>*);
extern template bool CrateReader::ReadIntArray<uint64_t>(bool, std::vector<uint64_t>*);
extern template bool CrateReader::ReadIntArrayTyped<int32_t>(bool, TypedArray<int32_t>*);
extern template bool CrateReader::ReadIntArrayTyped<uint32_t>(bool, TypedArray<uint32_t>*);
extern template bool CrateReader::ReadIntArrayTyped<int64_t>(bool, TypedArray<int64_t>*);
extern template bool CrateReader::ReadIntArrayTyped<uint64_t>(bool, TypedArray<uint64_t>*);
extern template bool CrateReader::ReadListOp<Payload>(ListOp<Payload>*);
extern template bool CrateReader::ReadListOp<Reference>(ListOp<Reference>*);
extern template bool CrateReader::ReadListOp<int32_t>(ListOp<int32_t>*);
extern template bool CrateReader::ReadListOp<int64_t>(ListOp<int64_t>*);
extern template bool CrateReader::ReadListOp<uint32_t>(ListOp<uint32_t>*);
extern template bool CrateReader::ReadListOp<uint64_t>(ListOp<uint64_t>*);
extern template bool CrateReader::ReadListOp<Path>(ListOp<Path>*);
extern template bool CrateReader::ReadListOp<std::string>(ListOp<std::string>*);
extern template bool CrateReader::ReadListOp<Token>(ListOp<Token>*);

}  // namespace crate
}  // namespace lightusd
