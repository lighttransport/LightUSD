// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USD ASCII parser

#pragma once

#include <functional>
#include <stdio.h>

#include <stack>
#include <unordered_map>
#include <unordered_set>

// #include "external/better-enums/enum.h"
#include "composition.hh"
#include "core/prim-spec.hh"  // PrimSpec, Property, composition-types (transitively: prim-enums, prim-metas, variant-types)
#include "stream-reader.hh"
#include "string-similarity.hh"
#include "lightusd.hh"
#include "typed-array-core.hh"

// Configuration flag for enabling fix suggestions in parse errors
// When enabled, parser will suggest similar keywords/identifiers for unrecognized tokens
#ifndef LIGHTUSD_ENABLE_SUGGEST_FIX
#define LIGHTUSD_ENABLE_SUGGEST_FIX 1
#endif

//
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external
#include "nonstd/expected.hpp"

//
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {

namespace ascii {

// keywords
constexpr auto kUniform = "uniform";
constexpr auto kToken = "token";

// Frequently used attr/meta keywords
constexpr auto kKind = "kind";
constexpr auto kInterpolation = "interpolation";

struct Identifier : std::string {
  // using std::string;
};

// FIXME: Not used? remove.
struct PathIdentifier : std::string {
  // using std::string;
};

///
/// Progress callback function type.
/// @param[in] progress Progress value between 0.0 and 1.0
/// @param[in] userptr User-provided pointer for custom data
/// @return true to continue parsing, false to cancel
///
using ProgressCallback = std::function<bool(float progress, void *userptr)>;

///
/// Parser configuration options.
/// For strict configurations (e.g. reading USDZ on mobile devices),
/// should disallow unknown items for security and performance.
///
struct AsciiParserOption {
  bool allow_unknown_prim{true};         ///< Allow parsing unknown prim types
  bool allow_unknown_apiSchema{true};    ///< Allow parsing unknown API schemas
  bool strict_allowedToken_check{false}; ///< Enforce strict token validation
};

///
/// Test if input file is USDA ascii format.
///
/// @param[in] filename Path to file to check
/// @param[in] max_filesize Maximum file size to read (0 = no limit)
/// @return true if file is in USDA ASCII format
///
bool IsUSDA(const std::string &filename, size_t max_filesize = 0);

///
/// Hand-written USDA (USD ASCII) format parser.
/// This parser provides secure, dependency-free parsing of USD ASCII files
/// with comprehensive error handling and configurable strictness levels.
///
/// Usage:
/// ```cpp
/// lightusd::StreamReader reader(filename);
/// lightusd::ascii::AsciiParser parser(&reader);
/// lightusd::Layer layer;
/// if (parser.Parse(&layer)) {
///   // Success - use the layer
/// } else {
///   std::cerr << "Parse error: " << parser.GetError() << std::endl;
/// }
/// ```
///
class AsciiParser {
 public:
  // TODO: refactor
  struct PrimMetas {
    // Frequently used prim metas
    nonstd::optional<Kind> kind;

    value::dict customData;  // `customData`
    std::vector<value::StringData>
        strings;  // String only unregistered metadata.
  };

  // TODO: Unifity class with StageMetas in core/layer-types.hh
  struct StageMetas {
    ///
    /// Predefined Stage metas
    ///
    std::vector<SubLayer> subLayers;  // 'subLayers' (with optional LayerOffset)
    value::token defaultPrim;                 // 'defaultPrim'
    value::StringData doc;                    // 'doc' or 'documentation'
    nonstd::optional<Axis> upAxis;            // not specified = nullopt
    nonstd::optional<double> metersPerUnit;
    nonstd::optional<double> kilogramsPerUnit;
    nonstd::optional<double> timeCodesPerSecond;
    nonstd::optional<double> startTimeCode;
    nonstd::optional<double> endTimeCode;
    nonstd::optional<double> framesPerSecond;

    nonstd::optional<bool> autoPlay;
    nonstd::optional<value::token> playbackMode;  // 'none' or 'loop'

    std::map<std::string, MetaVariable> customLayerData;  // `customLayerData`.
    bool customLayerDataAuthored{false};  // Track if customLayerData was explicitly authored
    value::StringData comment;  // String only comment string.

    // AOUSD Core Spec fields
    nonstd::optional<value::AssetPath> colorConfiguration;
    nonstd::optional<value::token> colorManagementSystem;
    nonstd::optional<std::string> renderSettingsPrimPath;
    nonstd::optional<std::string> owner;
    nonstd::optional<bool> hasOwnedSubLayers;
    nonstd::optional<std::map<std::string, MetaVariable>> expressionVariables;
    // relocates: source path -> target path mappings
    std::vector<std::pair<Path, Path>> relocates;

    // Unregistered layer metadata, preserved verbatim as the raw USDA text of
    // the value (quotes included for string values). OpenUSD preserves such
    // opinions; mirrors the unregistered *prim* metadata handling.
    std::map<std::string, std::string> unregisteredMetas;
  };

  struct ParseState {
    int64_t loc{-1};  // byte location in StreamReder
  };

  struct Cursor {
    int row{0};
    int col{0};
  };

  struct StoredCursor {
    Cursor cursor;
  };

  struct CursorStore {
    lightusd::HashMap<std::string, StoredCursor> layer_metas;
    lightusd::HashMap<std::string, StoredCursor> prims;
    lightusd::HashMap<std::string, StoredCursor> prim_attrs;
    lightusd::HashMap<std::string, StoredCursor> properties;

    static std::string MakePropertyKey(const std::string &prim_path,
                                       const std::string &property_name) {
      return prim_path + "." + property_name;
    }

    void Clear() {
      layer_metas.clear();
      prims.clear();
      prim_attrs.clear();
      properties.clear();
    }

    void StoreLayerMeta(const std::string &meta_name, const Cursor &cursor) {
      layer_metas[meta_name] = StoredCursor{cursor};
    }

    void StorePrim(const std::string &prim_path, const Cursor &cursor) {
      prims[prim_path] = StoredCursor{cursor};
    }

    void StorePrimAttr(const std::string &prim_path,
                       const std::string &property_name,
                       const Cursor &cursor) {
      prim_attrs[MakePropertyKey(prim_path, property_name)] = StoredCursor{cursor};
    }

    void StoreProperty(const std::string &prim_path,
                       const std::string &property_name,
                       const Cursor &cursor) {
      properties[MakePropertyKey(prim_path, property_name)] = StoredCursor{cursor};
    }

    const StoredCursor *FindLayerMeta(const std::string &meta_name) const {
      return layer_metas.find_value_ptr(meta_name);
    }

    const StoredCursor *FindPrim(const std::string &prim_path) const {
      return prims.find_value_ptr(prim_path);
    }

    const StoredCursor *FindPrimAttr(const std::string &prim_path,
                                     const std::string &property_name) const {
      return prim_attrs.find_value_ptr(MakePropertyKey(prim_path, property_name));
    }

    const StoredCursor *FindProperty(const std::string &prim_path,
                                     const std::string &property_name) const {
      return properties.find_value_ptr(MakePropertyKey(prim_path, property_name));
    }
  };

  /// Error type enumeration for categorizing parser errors
  enum class ErrorType {
    SyntaxError,      ///< Parse/syntax error
    SemanticError,    ///< Type or value error
    ValidationError,  ///< Constraint violation
    IOError,          ///< File access error
    UnknownError      ///< Uncategorized error
  };

  /// Error recovery suggestion enumeration (Priority 4c)
  enum class ErrorRecoveryHint {
    NoHint,                     ///< No suggestion available
    CheckBracketMatching,      ///< Check if brackets/parens are balanced
    CheckQuotes,               ///< Check if strings are properly quoted
    CheckTypeName,             ///< Verify type name is correct
    CheckAttributeName,        ///< Verify attribute name syntax
    CheckIndentation,          ///< Check file indentation
    CheckLineEndings           ///< Check for mixed line endings
  };

  /// Error position mode - whether cursor position is exact or approximate
  enum class ErrorPositionMode {
    Exact,  ///< Exact cursor position is known
    Near    ///< Approximate position (error happened near this location)
  };

  struct ErrorDiagnostic {
    std::string err;
    Cursor cursor;
    ErrorType type{ErrorType::UnknownError};  ///< Error category
    ErrorRecoveryHint hint{ErrorRecoveryHint::NoHint};  ///< Recovery suggestion (Priority 4c)
    std::string suggestion;  ///< Suggested fix for the error (Priority 5)
    ErrorPositionMode position_mode{ErrorPositionMode::Exact};  ///< Whether position is exact or approximate

    /// Get a human-readable error type name
    const char* TypeName() const {
      switch (type) {
        case ErrorType::SyntaxError:
          return "Syntax Error";
        case ErrorType::SemanticError:
          return "Semantic Error";
        case ErrorType::ValidationError:
          return "Validation Error";
        case ErrorType::IOError:
          return "IO Error";
        case ErrorType::UnknownError:
          return "Error";
      }
      return "Error";  // Unreachable but satisfies compilers
    }

    /// Get human-readable recovery hint (Priority 4c)
    const char* GetHint() const {
      switch (hint) {
        case ErrorRecoveryHint::NoHint:
          return "";
        case ErrorRecoveryHint::CheckBracketMatching:
          return "Check bracket/parenthesis matching";
        case ErrorRecoveryHint::CheckQuotes:
          return "Check string quote matching";
        case ErrorRecoveryHint::CheckTypeName:
          return "Verify type name is valid USD type";
        case ErrorRecoveryHint::CheckAttributeName:
          return "Verify attribute name follows USD naming conventions";
        case ErrorRecoveryHint::CheckIndentation:
          return "Check file indentation for consistency";
        case ErrorRecoveryHint::CheckLineEndings:
          return "Check for mixed line endings (LF vs CRLF)";
      }
      return "";
    }
  };

  void PushError(const std::string &msg,
                 ErrorType type = ErrorType::UnknownError,
                 ErrorRecoveryHint hint = ErrorRecoveryHint::NoHint,
                 const std::string &suggestion = "",
                 ErrorPositionMode position_mode = ErrorPositionMode::Exact) {
    ErrorDiagnostic diag;
    diag.cursor.row = _curr_cursor.row;
    diag.cursor.col = _curr_cursor.col;
    diag.err = msg;
    diag.type = type;
    diag.hint = hint;
    diag.suggestion = suggestion;
    diag.position_mode = position_mode;
    err_stack.push(diag);
  }

  // This function is used to cancel recent parsing error.
  void PopError() {
    if (!err_stack.empty()) {
      err_stack.pop();
    }
  }

  void PushWarn(const std::string &msg,
                ErrorType type = ErrorType::UnknownError,
                ErrorRecoveryHint hint = ErrorRecoveryHint::NoHint,
                const std::string &suggestion = "",
                ErrorPositionMode position_mode = ErrorPositionMode::Exact) {
    ErrorDiagnostic diag;
    diag.cursor.row = _curr_cursor.row;
    diag.cursor.col = _curr_cursor.col;
    diag.err = msg;
    diag.type = type;
    diag.hint = hint;
    diag.suggestion = suggestion;
    diag.position_mode = position_mode;
    warn_stack.push(diag);
  }

  // This function is used to cancel recent parsing warning.
  void PopWarn() {
    if (!warn_stack.empty()) {
      warn_stack.pop();
    }
  }

  bool IsStageMeta(const std::string &name);
  bool IsRegisteredPrimMeta(const std::string &name);

  class VariableDef {
   public:
    // Handler functor in post parsing stage.
    // e.g. Check input string is a valid one: one of "common", "group",
    // "assembly", "component" or "subcomponent" for "kind" metadata
    using PostParseHandler =
        std::function<nonstd::expected<bool, std::string>(const std::string &)>;

    static nonstd::expected<bool, std::string> DefaultPostParseHandler(
        const std::string &) {
      return true;
    }

    std::string type;  // e.g. token, color3f
    std::string name;
    bool allow_array_type{false};  // when true, we accept `type` and `type[]`

    PostParseHandler post_parse_handler;

    VariableDef() = default;

    VariableDef(const std::string &t, const std::string &n, bool a = false,
                PostParseHandler ph = DefaultPostParseHandler)
        : type(t), name(n), allow_array_type(a), post_parse_handler(ph) {}

    VariableDef(const VariableDef &rhs) = default;
    VariableDef &operator=(const VariableDef &rhs) = default;

    // VariableDef &operator=(const VariableDef &rhs) {
    //   type = rhs.type;
    //   name = rhs.name;
    //   parse_handler = rhs.parse_handler;

    //  return *this;
    //}
  };

  // Use multimap to support multiple listop qualifiers per composition arc
  // (e.g., both "delete references" and "prepend references" on same prim)
  using PrimMetaMap =
      std::multimap<std::string, std::pair<ListEditQual, MetaVariable>>;

  // `reorder <field> = [...]` body statements parsed by ParsePrimProps,
  // consumed by the enclosing ParseBlock (injected into that prim's
  // PrimMetaMap as "reorder:<field>" entries). Scoped by index mark so
  // nested prim blocks take only their own entries.
  std::vector<std::pair<std::string, std::vector<value::token>>>
      _pending_reorders;

  struct VariantContent;

  //
  // variantSet "keyname" = {
  //    "key0" : { ... }
  //    "key1" : { ... }
  // }
  //
  struct VariantSetContent {
    int64_t variantPrimIdx{-1}; // Pseudo Prim Idx for `variantSet`. -1 = no variantSet node
    std::map<std::string, VariantContent> variantSets;
  };

  struct VariantContent {
    PrimMetaMap metas;
    std::vector<int64_t> primIndices;  // primIdx of Reconstrcuted Prim.
    std::map<std::string, Property> props;
    std::vector<value::token> properties;

    // for nested `variantSet`
    std::map<std::string, VariantSetContent> variantSets;
  };



  // TODO: Use std::vector instead of std::map?
  using VariantSetList =
      std::map<std::string, VariantSetContent>;

  AsciiParser();
  AsciiParser(lightusd::StreamReader *sr);

  AsciiParser(const AsciiParser &rhs) = delete;
  AsciiParser(AsciiParser &&rhs) = delete;

  ~AsciiParser();

  ///
  /// Assign index to primitive for index-based prim scene graph representation.
  /// -1 = root
  ///
  using PrimIdxAssignFunctin = std::function<int64_t(const int64_t parentIdx)>;
  void RegisterPrimIdxAssignFunction(PrimIdxAssignFunctin fun) {
    _prim_idx_assign_fun = fun;
  }

  ///
  /// Stage Meta construction callback function
  ///
  using StageMetaProcessFunction = std::function<bool(const StageMetas &metas)>;

  ///
  /// Register Stage metadatum processing callback function.
  /// Called when after parsing Stage metadatum.
  ///
  void RegisterStageMetaProcessFunction(StageMetaProcessFunction fun) {
    _stage_meta_process_fun = fun;
  }

  ///
  /// Prim Meta construction callback function
  ///
  // using PrimMetaProcessFunction = std::function<bool(const PrimMetas
  // &metas)>;

  ///
  /// Prim construction callback function
  /// TODO: Refactor arguments
  ///
  /// @param[in] full_path Absolute Prim Path(e.g. "/scope/gmesh0")
  /// @param[in] spec Specifier(`def`, `over` or `class`)
  /// @param[in] primTypeName typeName of this Prim(e.g. "Mesh", "SphereLight")
  /// @param[in] primIdx primitive index
  /// @param[in] parentPrimIdx parent Prim index. -1 for root
  /// @param[in] properties Prim properties
  /// @param[in] in_meta Input Prim metadataum
  /// @param[in] in_variantSetList Input VariantSet contents.
  /// @return true upon success or error message.
  ///
  using PrimConstructFunction =
      std::function<nonstd::expected<bool, std::string>(
          const Path &full_path, const Specifier spec,
          const std::string &primTypeName, const Path &prim_name,
          const int64_t primIdx, const int64_t parentPrimIdx,
          std::map<std::string, Property> &properties,
          const PrimMetaMap &in_meta, const VariantSetList &in_variantSetList)>;

  ///
  /// Register Prim construction callback function.
  /// Example: "Xform", ReconstrctXform
  ///
  void RegisterPrimConstructFunction(const std::string &prim_type,
                                     PrimConstructFunction fun) {
    _prim_construct_fun_map[prim_type] = fun;
  }

  ///
  /// Callbacks called at closing `def` block.
  ///
  using PostPrimConstructFunction =
      std::function<nonstd::expected<bool, std::string>(
          const Path &path, const int64_t primIdx,
          const int64_t parentPrimIdx)>;
  void RegisterPostPrimConstructFunction(const std::string &prim_type,
                                         PostPrimConstructFunction fun) {
    _post_prim_construct_fun_map[prim_type] = fun;
  }

  ///
  /// For composition(Treat Prim as generic container).
  /// AsciiParser(i.e. USDAReader)
  ///
  using PrimSpecFunction = std::function<nonstd::expected<bool, std::string>(
      const Path &full_path, const Specifier spec,
      const std::string &primTypeName, const Path &prim_name,
      const int64_t primIdx, const int64_t parentPrimIdx,
      const std::map<std::string, Property> &properties,
      const PrimMetaMap &in_meta, const VariantSetList &in_variantSetLists)>;

  void RegisterPrimSpecFunction(PrimSpecFunction fun) { _primspec_fun = fun; }

  ///
  /// Base filesystem directory to search asset files.
  ///
  void SetBaseDir(const std::string &base_dir);

  ///
  /// Set ASCII data stream
  ///
  void SetStream(lightusd::StreamReader *sr);

  const CursorStore &GetCursorStore() const { return _cursor_store; }
  std::string FormatLayerMetaSourceDiagnostic(const std::string &meta_name,
                                              int column_width = 80) const;
  std::string FormatPrimSourceDiagnostic(const std::string &prim_path,
                                         int column_width = 80) const;
  std::string FormatPrimAttrSourceDiagnostic(const std::string &prim_path,
                                             const std::string &property_name,
                                             int column_width = 80) const;
  std::string FormatPropertySourceDiagnostic(const std::string &prim_path,
                                             const std::string &property_name,
                                             int column_width = 80) const;

  ///
  /// Set memory limit in MB
  ///
  void SetMaxMemoryLimit(size_t limit_mb) {
    _max_memory_limit_bytes = limit_mb * 1024ull * 1024ull;
  }

  ///
  /// Set progress callback function
  /// @param[in] callback Progress callback function
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetProgressCallback(ProgressCallback callback, void *userptr = nullptr) {
    _progress_callback = callback;
    _progress_userptr = userptr;
  }

  ///
  /// Check if header data is USDA
  ///
  bool CheckHeader();

  ///
  /// True: create PrimSpec instead of typed Prim.
  /// Set true if you do USD composition.
  ///
  void set_primspec_mode(bool onoff) { _primspec_mode = onoff; }

  ///
  /// Parser entry point
  ///
  /// @param[in] load_states Bit mask of LoadState
  /// @param[in] parser_option Parse option(optional)
  ///
  /// TODO: Move `load_states` to AsciiParserOption?
  ///
  bool Parse(
      const uint32_t load_states = static_cast<uint32_t>(LoadState::Toplevel),
      const AsciiParserOption &parser_option = AsciiParserOption());

  ///
  /// Parse TimeSample value with specified array type of
  /// `type_id`(value::TypeId) (You can obrain type_id from string using
  /// value::GetTypeId())
  ///
  bool ParseTimeSampleValue(const uint32_t type_id, value::Value *result);

  ///
  /// Parse TimeSample value with specified `type_name`(Appears in USDA. .e.g.
  /// "float", "matrix2d")
  ///
  bool ParseTimeSampleValue(const std::string &type_name, value::Value *result);

  ///
  /// Parse TimeSample value with specified base type of
  /// `type_id`(value::TypeId) (You can obrain type_id from string using
  /// value::GetTypeId())
  ///
  bool ParseTimeSampleValueOfArrayType(const uint32_t base_type_id,
                                       value::Value *result);

  ///
  /// Parse TimeSample value with specified array type of `type_name`("[]"
  /// omiotted. .e.g. "float" for "float[]")
  ///
  bool ParseTimeSampleValueOfArrayType(const std::string &type_name,
                                       value::Value *result);

  // TODO: ParseBasicType?
  bool ParsePurpose(Purpose *result);

  ///
  /// Return true but `value` is set to nullopt for `None`(Attribute Blocked)
  ///
  // template <typename T>
  // bool ReadBasicType(nonstd::optional<T> *value);

  bool ReadBasicType(nonstd::optional<bool> *value);
  bool ReadBasicType(nonstd::optional<value::half> *value);
  bool ReadBasicType(nonstd::optional<value::half2> *value);
  bool ReadBasicType(nonstd::optional<value::half3> *value);
  bool ReadBasicType(nonstd::optional<value::half4> *value);
  bool ReadBasicType(nonstd::optional<int32_t> *value);
  bool ReadBasicType(nonstd::optional<value::int2> *value);
  bool ReadBasicType(nonstd::optional<value::int3> *value);
  bool ReadBasicType(nonstd::optional<value::int4> *value);
  bool ReadBasicType(nonstd::optional<uint32_t> *value);
  bool ReadBasicType(nonstd::optional<value::uint2> *value);
  bool ReadBasicType(nonstd::optional<value::uint3> *value);
  bool ReadBasicType(nonstd::optional<value::uint4> *value);
  // char types (int8_t)
  bool ReadBasicType(nonstd::optional<char> *value);
  bool ReadBasicType(nonstd::optional<value::char2> *value);
  bool ReadBasicType(nonstd::optional<value::char3> *value);
  bool ReadBasicType(nonstd::optional<value::char4> *value);
  // uchar types (uint8_t)
  bool ReadBasicType(nonstd::optional<uint8_t> *value);
  bool ReadBasicType(nonstd::optional<value::uchar2> *value);
  bool ReadBasicType(nonstd::optional<value::uchar3> *value);
  bool ReadBasicType(nonstd::optional<value::uchar4> *value);
  // short types (int16_t)
  bool ReadBasicType(nonstd::optional<int16_t> *value);
  bool ReadBasicType(nonstd::optional<value::short2> *value);
  bool ReadBasicType(nonstd::optional<value::short3> *value);
  bool ReadBasicType(nonstd::optional<value::short4> *value);
  // ushort types (uint16_t)
  bool ReadBasicType(nonstd::optional<uint16_t> *value);
  bool ReadBasicType(nonstd::optional<value::ushort2> *value);
  bool ReadBasicType(nonstd::optional<value::ushort3> *value);
  bool ReadBasicType(nonstd::optional<value::ushort4> *value);
  bool ReadBasicType(nonstd::optional<int64_t> *value);
  bool ReadBasicType(nonstd::optional<uint64_t> *value);
  bool ReadBasicType(nonstd::optional<float> *value);
  bool ReadBasicType(nonstd::optional<value::float2> *value);
  bool ReadBasicType(nonstd::optional<value::float3> *value);
  bool ReadBasicType(nonstd::optional<value::float4> *value);
  bool ReadBasicType(nonstd::optional<double> *value);
  bool ReadBasicType(nonstd::optional<value::double2> *value);
  bool ReadBasicType(nonstd::optional<value::double3> *value);
  bool ReadBasicType(nonstd::optional<value::double4> *value);
  bool ReadBasicType(nonstd::optional<value::quath> *value);
  bool ReadBasicType(nonstd::optional<value::quatf> *value);
  bool ReadBasicType(nonstd::optional<value::quatd> *value);
  bool ReadBasicType(nonstd::optional<value::point3h> *value);
  bool ReadBasicType(nonstd::optional<value::point3f> *value);
  bool ReadBasicType(nonstd::optional<value::point3d> *value);
  bool ReadBasicType(nonstd::optional<value::vector3h> *value);
  bool ReadBasicType(nonstd::optional<value::vector3f> *value);
  bool ReadBasicType(nonstd::optional<value::vector3d> *value);
  bool ReadBasicType(nonstd::optional<value::normal3h> *value);
  bool ReadBasicType(nonstd::optional<value::normal3f> *value);
  bool ReadBasicType(nonstd::optional<value::normal3d> *value);
  bool ReadBasicType(nonstd::optional<value::color3h> *value);
  bool ReadBasicType(nonstd::optional<value::color3f> *value);
  bool ReadBasicType(nonstd::optional<value::color3d> *value);
  bool ReadBasicType(nonstd::optional<value::color4h> *value);
  bool ReadBasicType(nonstd::optional<value::color4f> *value);
  bool ReadBasicType(nonstd::optional<value::color4d> *value);
  bool ReadBasicType(nonstd::optional<value::matrix2f> *value);
  bool ReadBasicType(nonstd::optional<value::matrix3f> *value);
  bool ReadBasicType(nonstd::optional<value::matrix4f> *value);
  bool ReadBasicType(nonstd::optional<value::matrix2d> *value);
  bool ReadBasicType(nonstd::optional<value::matrix3d> *value);
  bool ReadBasicType(nonstd::optional<value::matrix4d> *value);
  bool ReadBasicType(nonstd::optional<value::frame4d> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord2h> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord2f> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord2d> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord3h> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord3f> *value);
  bool ReadBasicType(nonstd::optional<value::texcoord3d> *value);
  bool ReadBasicType(nonstd::optional<value::StringData> *value);
  bool ReadBasicType(nonstd::optional<std::string> *value);
  bool ReadBasicType(nonstd::optional<value::token> *value);
  bool ReadBasicType(nonstd::optional<Path> *value);
  bool ReadBasicType(nonstd::optional<value::AssetPath> *value);
  bool ReadBasicType(nonstd::optional<value::PathExpression> *value);
  bool ReadBasicType(nonstd::optional<Reference> *value);
  bool ReadBasicType(nonstd::optional<Payload> *value);
  bool ReadBasicType(nonstd::optional<Identifier> *value);
  bool ReadBasicType(nonstd::optional<PathIdentifier> *value);

  // template <typename T>
  // bool ReadBasicType(T *value);

  bool ReadBasicType(bool *value);
  bool ReadBasicType(value::half *value);
  bool ReadBasicType(value::half2 *value);
  bool ReadBasicType(value::half3 *value);
  bool ReadBasicType(value::half4 *value);
  bool ReadBasicType(int32_t *value);
  bool ReadBasicType(value::int2 *value);
  bool ReadBasicType(value::int3 *value);
  bool ReadBasicType(value::int4 *value);
  bool ReadBasicType(uint32_t *value);
  bool ReadBasicType(value::uint2 *value);
  bool ReadBasicType(value::uint3 *value);
  bool ReadBasicType(value::uint4 *value);
  // char types (int8_t)
  bool ReadBasicType(char *value);
  bool ReadBasicType(value::char2 *value);
  bool ReadBasicType(value::char3 *value);
  bool ReadBasicType(value::char4 *value);
  // uchar types (uint8_t)
  bool ReadBasicType(uint8_t *value);
  bool ReadBasicType(value::uchar2 *value);
  bool ReadBasicType(value::uchar3 *value);
  bool ReadBasicType(value::uchar4 *value);
  // short types (int16_t)
  bool ReadBasicType(int16_t *value);
  bool ReadBasicType(value::short2 *value);
  bool ReadBasicType(value::short3 *value);
  bool ReadBasicType(value::short4 *value);
  // ushort types (uint16_t)
  bool ReadBasicType(uint16_t *value);
  bool ReadBasicType(value::ushort2 *value);
  bool ReadBasicType(value::ushort3 *value);
  bool ReadBasicType(value::ushort4 *value);
  bool ReadBasicType(int64_t *value);
  bool ReadBasicType(uint64_t *value);
  bool ReadBasicType(float *value);
  bool ReadBasicType(value::float2 *value);
  bool ReadBasicType(value::float3 *value);
  bool ReadBasicType(value::float4 *value);
  bool ReadBasicType(double *value);
  bool ReadBasicType(value::double2 *value);
  bool ReadBasicType(value::double3 *value);
  bool ReadBasicType(value::double4 *value);
  bool ReadBasicType(value::quath *value);
  bool ReadBasicType(value::quatf *value);
  bool ReadBasicType(value::quatd *value);
  bool ReadBasicType(value::point3h *value);
  bool ReadBasicType(value::point3f *value);
  bool ReadBasicType(value::point3d *value);
  bool ReadBasicType(value::vector3h *value);
  bool ReadBasicType(value::vector3f *value);
  bool ReadBasicType(value::vector3d *value);
  bool ReadBasicType(value::normal3h *value);
  bool ReadBasicType(value::normal3f *value);
  bool ReadBasicType(value::normal3d *value);
  bool ReadBasicType(value::color3h *value);
  bool ReadBasicType(value::color3f *value);
  bool ReadBasicType(value::color3d *value);
  bool ReadBasicType(value::color4h *value);
  bool ReadBasicType(value::color4f *value);
  bool ReadBasicType(value::color4d *value);
  bool ReadBasicType(value::texcoord2h *value);
  bool ReadBasicType(value::texcoord2f *value);
  bool ReadBasicType(value::texcoord2d *value);
  bool ReadBasicType(value::texcoord3h *value);
  bool ReadBasicType(value::texcoord3f *value);
  bool ReadBasicType(value::texcoord3d *value);
  bool ReadBasicType(value::matrix2f *value);
  bool ReadBasicType(value::matrix3f *value);
  bool ReadBasicType(value::matrix4f *value);
  bool ReadBasicType(value::matrix2d *value);
  bool ReadBasicType(value::matrix3d *value);
  bool ReadBasicType(value::matrix4d *value);
  bool ReadBasicType(value::frame4d *value);
  bool ReadBasicType(value::StringData *value);
  bool ReadBasicType(std::string *value);
  bool ReadBasicType(value::token *value);
  bool ReadBasicType(Path *value);
  bool ReadBasicType(value::AssetPath *value);
  bool ReadBasicType(value::PathExpression *value);
  bool ReadBasicType(Reference *value);
  bool ReadBasicType(Payload *value);
  bool ReadBasicType(Identifier *value);
  bool ReadBasicType(PathIdentifier *value);

  template <typename T>
  bool ReadBasicType(nonstd::optional<std::vector<T>> *value);

  template <typename T>
  bool ReadBasicType(std::vector<T> *value);

  bool ParseMatrix(value::matrix2f *result);
  bool ParseMatrix(value::matrix3f *result);
  bool ParseMatrix(value::matrix4f *result);

  bool ParseMatrix(value::matrix2d *result);
  bool ParseMatrix(value::matrix3d *result);
  bool ParseMatrix(value::matrix4d *result);

  ///
  /// Parse '(', Sep1By(','), ')'
  ///
  template <typename T, size_t N>
  bool ParseBasicTypeTuple(std::array<T, N> *result);

  ///
  /// Parse '(', Sep1By(','), ')'
  /// Can have `None`
  ///
  template <typename T, size_t N>
  bool ParseBasicTypeTuple(nonstd::optional<std::array<T, N>> *result);

  template <typename T, size_t N>
  bool ParseTupleArray(std::vector<std::array<T, N>> *result);

  ///
  /// Parse the array of tuple. some may be None(e.g. `float3`: [(0, 1, 2),
  /// None, (2, 3, 4), ...] )
  ///
  template <typename T, size_t N>
  bool ParseTupleArray(std::vector<nonstd::optional<std::array<T, N>>> *result);

  template <typename T>
  bool SepBy1BasicType(const char sep, std::vector<T> *result);

  ///
  /// Allow the appearance of `sep` in the last item of array.
  /// (e.g. `[1, 2, 3,]`)
  ///
  template <typename T>
  bool SepBy1BasicType(const char sep, const char end_symbol,
                       std::vector<T> *result);

  ///
  /// Parse '[', Sep1By(','), ']'
  ///
  template <typename T>
  bool ParseBasicTypeArray(std::vector<nonstd::optional<T>> *result);

  ///
  /// Parse '[', Sep1By(','), ']'
  ///
  template <typename T>
  bool ParseBasicTypeArray(std::vector<T> *result);

  ///
  /// Parse '[', Sep1By(','), ']' using TypedArray<T> for memory optimization
  ///
  template <typename T>
  bool ParseBasicTypeArray(TypedArray<T> *result);

  ///
  /// Optimized float array parsing using tiny-string
  ///
  bool ParseFloatArrayOptimized(std::vector<float> *result);
  bool ParseDoubleArrayOptimized(std::vector<double> *result);
  bool ParseIntArrayOptimized(std::vector<int32_t> *result);
  bool ParseUIntArrayOptimized(std::vector<uint32_t> *result);
  bool ParseInt64ArrayOptimized(std::vector<int64_t> *result);
  bool ParseUInt64ArrayOptimized(std::vector<uint64_t> *result);
  bool ParseHalfArrayOptimized(std::vector<value::half> *result);
  bool ParseTokenArrayOptimized(std::vector<value::token> *result);
  bool ParseStringDataArrayOptimized(std::vector<value::StringData> *result);
  bool ParseStdStringArrayOptimized(std::vector<std::string> *result);

  ///
  /// Optimized compound-type array parsing using tiny-string
  ///
  bool ParseHalf2ArrayOptimized(std::vector<value::half2> *result);
  bool ParseHalf3ArrayOptimized(std::vector<value::half3> *result);
  bool ParseHalf4ArrayOptimized(std::vector<value::half4> *result);
  bool ParseFloat2ArrayOptimized(std::vector<value::float2> *result);
  bool ParseFloat3ArrayOptimized(std::vector<value::float3> *result);
  bool ParseFloat4ArrayOptimized(std::vector<value::float4> *result);
  bool ParsePoint3fArrayOptimized(std::vector<value::point3f> *result);
  bool ParseNormal3fArrayOptimized(std::vector<value::normal3f> *result);
  bool ParseDouble2ArrayOptimized(std::vector<value::double2> *result);
  bool ParseDouble3ArrayOptimized(std::vector<value::double3> *result);
  bool ParseDouble4ArrayOptimized(std::vector<value::double4> *result);
  bool ParseQuathArrayOptimized(std::vector<value::quath> *result);
  bool ParseQuatfArrayOptimized(std::vector<value::quatf> *result);
  bool ParseQuatdArrayOptimized(std::vector<value::quatd> *result);
  bool ParseMatrix2fArrayOptimized(std::vector<value::matrix2f> *result);
  bool ParseMatrix3fArrayOptimized(std::vector<value::matrix3f> *result);
  bool ParseMatrix4fArrayOptimized(std::vector<value::matrix4f> *result);
  bool ParseMatrix2dArrayOptimized(std::vector<value::matrix2d> *result);
  bool ParseMatrix3dArrayOptimized(std::vector<value::matrix3d> *result);
  bool ParseMatrix4dArrayOptimized(std::vector<value::matrix4d> *result);

  ///
  /// Parses 1 or more occurences of value with basic type 'T', separated by
  /// `sep`
  ///
  template <typename T>
  bool SepBy1BasicType(const char sep,
                       std::vector<nonstd::optional<T>> *result);

  ///
  /// Parses 1 or more occurences of tuple values with type 'T', separated by
  /// `sep`. Allows 'None'
  ///
  template <typename T, size_t N>
  bool SepBy1TupleType(const char sep,
                       std::vector<nonstd::optional<std::array<T, N>>> *result);

  ///
  /// Parses N occurences of tuple values with type 'T', separated by
  /// `sep`. Allows 'None'
  ///
  template <typename T, size_t M, size_t N>
  bool SepByNTupleType(
      const char sep,
      std::array<nonstd::optional<std::array<T, M>>, N> *result);

  ///
  /// Parses 1 or more occurences of tuple values with type 'T', separated by
  /// `sep`
  ///
  template <typename T, size_t N>
  bool SepBy1TupleType(const char sep, std::vector<std::array<T, N>> *result);

  bool ParseDictElement(std::string *out_key, MetaVariable *out_var);
  bool ParseDict(std::map<std::string, MetaVariable> *out_dict);

  ///
  /// Parse TimeSample data with concrete type for optimized binary storage.
  /// This template function is optimized for binary-serializable types and uses direct
  /// storage without value::Value wrapping for better performance.
  ///
  /// @tparam T The concrete type for time sample values
  /// @param ts Output TimeSamples container
  /// @return true if parsing succeeded with optimized path, false otherwise
  ///
  template<typename T>
  bool ParseTypedTimeSamples(value::TimeSamples *ts);

  ///
  /// Parse TimeSample data(scalar type) and store it to type-erased data
  /// structure value::TimeSamples.
  ///
  /// @param[in] type_name Name of TimeSamples type(seen in .usda file. e.g.
  /// "float" for `float var.timeSamples = ..`)
  ///
  bool ParseTimeSamples(const std::string &type_name, value::TimeSamples *ts);

  ///
  /// Parse TimeSample data(array type) and store it to type-erased data
  /// structure value::TimeSamples.
  ///
  /// @param[in] type_name Name of TimeSamples type(seen in .usda file. array
  /// suffix `[]` is omitted. e.g. "float" for `float[] var.timeSamples = ..`)
  ///
  bool ParseTimeSamplesOfArray(const std::string &type_name,
                               value::TimeSamples *ts);

  ///
  /// Parse a `.spline` value block (AOUSD Core Spec 7.4.2.4) and store it to the
  /// type-erased primvar::PrimVar::SplineData. `type_name` is the (scalar)
  /// attribute value type ("double"/"float"/"half"); knot values are stored as
  /// value::Value of that type.
  ///
  bool ParseSplineValue(const std::string &type_name,
                        primvar::PrimVar::SplineData *out);

  ///
  /// `variants` in Prim meta.
  ///
  bool ParseVariantsElement(std::string *out_key, std::string *out_var);
  bool ParseVariants(VariantSelectionMap *out_map);

  bool MaybeListEditQual(lightusd::ListEditQual *qual);
  bool MaybeVariability(lightusd::Variability *variability,
                        bool *varying_authored);

  ///
  /// Try parsing single-quoted(`"`) string
  ///
  bool MaybeString(value::StringData *str);

  ///
  /// Try parsing triple-quited(`"""`) multi-line string.
  ///
  bool MaybeTripleQuotedString(value::StringData *str);

  ///
  /// Parse assset path identifier.
  ///
  bool ParseAssetIdentifier(value::AssetPath *out, bool *triple_deliminated);

  /// Parse optional `(offset = N; scale = M; customData = {...})` clause
  /// that may appear after a Reference or Payload's asset+prim_path.
  /// Pass `out_customData = nullptr` to reject the `customData` key
  /// (Payload has no customData field).
  bool ParseOptionalLayerOffset(LayerOffset *out,
                                Dictionary *out_customData = nullptr);

  class PrimIterator;
  using const_iterator = PrimIterator;
  const_iterator begin() const;
  const_iterator end() const;

  ///
  /// Get error message(when `Parse` failed)
  ///
  std::string GetError();

  ///
  /// Get warning message(warnings in `Parse`)
  ///
  std::string GetWarning();

  ///
  /// Get error message with context showing surrounding source lines.
  /// @param[in] context_lines Number of lines of context to show around error
  /// (default 2)
  /// @return Formatted error message with source code context and caret indicator
  ///
  std::string GetErrorWithContext(int context_lines = 2);

  ///
  /// Get warning message with context showing surrounding source lines.
  /// @param[in] context_lines Number of lines of context to show around warning
  /// (default 2)
  /// @return Formatted warning message with source code context and caret
  /// indicator
  ///
  std::string GetWarningWithContext(int context_lines = 2);

  ///
  /// Get error message with aggressive deduplication and recovery hints (Priority 4b & 4c).
  /// Groups similar errors and provides recovery suggestions based on error type.
  /// @param[in] show_hints If true, include recovery hints for each error type
  /// @return Formatted error messages with deduplication and optional hints
  ///
  std::string GetErrorWithHints(bool show_hints = true);

  ///
  /// Get warning message with aggressive deduplication and recovery hints (Priority 4b & 4c).
  /// @param[in] show_hints If true, include recovery hints for each warning type
  /// @return Formatted warning messages with deduplication and optional hints
  ///
  std::string GetWarningWithHints(bool show_hints = true);

  ///
  /// Get error message with source code context including surrounding lines.
  /// Shows actual file content with caret (^) and visual indicators (~~~~).
  /// @param[in] filename Path to the source USDA file (for context retrieval)
  /// @param[in] context_lines Number of lines of context to show around error
  /// @return Formatted error messages with source code context and visual indicators
  ///
  std::string GetErrorWithSourceContext(const std::string& filename, int context_lines = 2, int column_width = 40);


  // Return true if the .udsa is read in the top layer(stage)
  bool IsToplevel() {
    return _toplevel;
    // return !IsReferenced() && !IsSubLayered() && !IsPayloaded();
  }

  bool MaybeNone();
  bool MaybeAnimationBlock();
  bool MaybeCustom();

  template <typename T>
  bool MaybeNonFinite(T *out);

  bool LexFloat(std::string *result);

  bool Expect(char expect_c);

  bool ReadStringLiteral(
      std::string *literal);  // identifier wrapped with " or '. result contains
                              // quote chars.
  bool ReadPrimAttrIdentifier(std::string *token);
  bool ReadIdentifier(std::string *token);  // no '"'
  bool ReadPathIdentifier(
      std::string *path_identifier);  // '<' + identifier + '>'

  // read until newline
  bool ReadUntilNewline(std::string *str);


  /// Parse magic
  /// #usda FLOAT
  bool ParseMagicHeader();

  bool SkipWhitespace();

  // skip_semicolon true: ';' can be used as a separator. this flag is for
  // statement block.
  bool SkipWhitespaceAndNewline(const bool allow_semicolon = true);
  bool SkipCommentAndWhitespaceAndNewline(const bool allow_semicolon = true);

  bool SkipUntilNewline();

  // bool ParseAttributeMeta();
  bool ParseAttrMeta(AttrMeta *out_meta);

  bool ParsePrimMetas(PrimMetaMap *out_metamap);

  bool ParseMetaValue(const VariableDef &def, MetaVariable *outvar);

  bool ParseStageMetaOpt();
  // Parsed Stage metadatum is stored in this instance.
  bool ParseStageMetas();

  bool ParseCustomMetaValue();

  bool ParseReference(Reference *out, bool *triple_deliminated);
  bool ParsePayload(Payload *out, bool *triple_deliminated);

  ///
  /// Parse optional LayerOffset group: `(offset = <double>; scale = <double>)`
  /// (both fields optional, `;`-separated, pxr USDA syntax).
  /// When no `(` group follows at the current location, returns true and
  /// leaves `out` unchanged.
  ///
  bool MaybeParseLayerOffset(LayerOffset *out);

  // `#` style comment
  bool ParseSharpComment();

  bool IsSupportedPrimAttrType(const std::string &ty);
  bool IsSupportedPrimType(const std::string &ty);
  bool IsSupportedAPISchema(const std::string &ty);

  bool Eof() {
    // end of buffer, or current char is nullchar('\0')
    return _sr->eof() || _sr->is_nullchar();
  }

  bool ParseRelationship(Relationship *result);
  bool ParseProperties(std::map<std::string, Property> *props,
                       std::vector<value::token> *propNames);

  //
  // Look***() : Fetch chars but do not change input stream position.
  //

  bool LookChar1(char *c);
  bool LookCharN(size_t n, std::vector<char> *nc);

  // Inlined: previously an out-of-line one-liner that perf showed as ~15%
  // self-time on USDA-heavy parses. Inlining lets the compiler also inline
  // StreamReader::read1 (already header-inline) and hoist the _sr pointer
  // load out of hot scan loops in LexFloat / SkipWhitespaceAndNewline / etc.
  bool Char1(char *c) { return _sr->read1(c); }

  bool CharN(size_t n, std::vector<char> *nc);
  bool CharN(size_t n, char *dst); // assume dest has n >= bytes

  bool Rewind(size_t offset);
  uint64_t CurrLoc();
  bool SeekTo(uint64_t pos);  // Move to absolute `pos` bytes location

  bool PushParserState();
  bool PopParserState(ParseState *state);

  //
  // Valid after ParseStageMetas() --------------
  //
  StageMetas GetStageMetas() const { return _stage_metas; }

  // primIdx is assigned through `PrimIdxAssignFunctin`
  // parentPrimIdx = -1 => root prim
  // depth = tree level(recursion count)
  // bool ParseClassBlock(const int64_t primIdx, const int64_t parentPrimIdx,
  // const uint32_t depth = 0); bool ParseOverBlock(const int64_t primIdx, const
  // int64_t parentPrimIdx, const uint32_t depth = 0); bool ParseDefBlock(const
  // int64_t primIdx, const int64_t parentPrimIdx, const uint32_t depth = 0);

  // Parse `def`, `over` or `class` block
  // @param[in] in_variantStmt : true when this Block is parsed within
  // `variantSet` statement. Default true.
  bool ParseBlock(const Specifier spec, const int64_t primIdx,
                  const int64_t parentPrimIdx, const uint32_t depth,
                  const bool in_variant = false);

  // Parse `variantSet` stmt
  bool ParseVariantSet(const int64_t primIdx, const int64_t parentPrimIdx,
                       const uint32_t depth,
                       VariantSetContent *variantSetContent);

  // --------------------------------------------

 private:
  ///
  /// Generate a fix suggestion for an invalid token (Priority 5).
  /// Uses string similarity matching to suggest corrections.
  /// @param[in] invalid_token The unrecognized token
  /// @return Suggestion string (e.g. "Did you mean 'def'?"), or empty if no match
  ///
  std::string GenerateSuggestion(const std::string& invalid_token);

  ///
  /// Do common setups. Assume called in ctor.
  ///
  void Setup();

  nonstd::optional<std::pair<ListEditQual, MetaVariable>> ParsePrimMeta();
  bool ParsePrimProps(std::map<std::string, Property> *props,
                      std::vector<value::token> *propNames);

  template <typename T>
  bool ParseBasicPrimAttr(bool array_qual, const std::string &primattr_name,
                          Attribute *out_attr);

  // Parse the body of an `edit [ <op>; ... ]` array-edit value (the `edit`
  // keyword is consumed by the caller). Element literals are read as type T;
  // builds the literal array and the Vt_ArrayEditOps `_ins` instruction stream.
  template <typename T>
  bool ParseArrayEditValue(std::vector<T> *literals, std::vector<int64_t> *ops);

  // Consume the upcoming `edit` keyword iff present (array-edit value).
  bool MaybeArrayEdit();

  bool ParseStageMeta(std::pair<ListEditQual, MetaVariable> *out);

  nonstd::optional<VariableDef> GetStageMetaDefinition(const std::string &name);
  nonstd::optional<VariableDef> GetPrimMetaDefinition(const std::string &arg);
  nonstd::optional<VariableDef> GetPropMetaDefinition(const std::string &arg);

  std::string GetCurrentPrimPath();
  void RecordLayerMetaCursor(const std::string &meta_name, const Cursor &cursor);
  void RecordPrimCursor(const std::string &prim_path, const Cursor &cursor);
  void RecordPrimAttrCursor(const std::string &prim_path,
                            const std::string &property_name,
                            const Cursor &cursor);
  void RecordPropertyCursor(const std::string &prim_path,
                            const std::string &property_name,
                            const Cursor &cursor);
  std::string FormatStoredCursorDiagnostic(const StoredCursor *stored,
                                           int column_width) const;
  std::string FormatCursorDiagnostic(const Cursor &cursor,
                                     int column_width) const;
  bool PrimPathStackDepth() { return _path_stack.size(); }
  void PushPrimPath(const std::string &abs_path) {
    // TODO: validate `abs_path` is really absolute full path.
    _path_stack.push(abs_path);
  }
  void PopPrimPath() {
    if (!_path_stack.empty()) {
      _path_stack.pop();
    }
  }

  const lightusd::StreamReader *_sr = nullptr;

  // "class" defs
  // std::map<std::string, Klass> _klasses;
  std::stack<std::string> _path_stack;

  Cursor _curr_cursor;
  CursorStore _cursor_store;

  // Supported Prim types
  std::unordered_set<std::string> _supported_prim_types;
  std::unordered_set<std::string> _supported_prim_attr_types;

  // Supported API schemas
  std::unordered_set<std::string> _supported_api_schemas;

  // Supported metadataum for Stage
  lightusd::HashMap<std::string, VariableDef> _supported_stage_metas;

  // Supported metadataum for Prim.
  lightusd::HashMap<std::string, VariableDef> _supported_prim_metas;

  // Supported metadataum for Property(Attribute and Relation).
  lightusd::HashMap<std::string, VariableDef> _supported_prop_metas;

  std::stack<ErrorDiagnostic> err_stack;
  std::stack<ErrorDiagnostic> warn_stack;
  std::stack<ParseState> parse_stack;

  float _version{1.0f};

  // load flags
  bool _toplevel{true};
  // TODO: deprecate?
  bool _sub_layered{false};
  bool _referenced{false};
  bool _payloaded{false};

  AsciiParserOption _option;

  std::string _base_dir;

  StageMetas _stage_metas;

  // Memory tracking. Default 16GB (matches the public LoadOptions default and
  // is overridden per-load via SetMaxMemoryLimit). The previous 128GB default
  // was effectively unlimited, making the tracker a no-op guard.
  uint64_t _max_memory_limit_bytes{16ull * 1024ull * 1024ull * 1024ull};
  uint64_t _memory_usage{0};
  uint32_t _dict_nesting_depth{0}; ///< Tracks ParseDict recursion depth

  //
  // Callbacks
  //
  PrimIdxAssignFunctin _prim_idx_assign_fun;
  StageMetaProcessFunction _stage_meta_process_fun;
  // PrimMetaProcessFunction _prim_meta_process_fun;
  lightusd::HashMap<std::string, PrimConstructFunction> _prim_construct_fun_map;
  lightusd::HashMap<std::string, PostPrimConstructFunction> _post_prim_construct_fun_map;

  bool _primspec_mode{false};

  // For composition. PrimSpec is typeless so single callback function only.
  PrimSpecFunction _primspec_fun{nullptr};

  // Progress callback
  ProgressCallback _progress_callback;  // Default-initialized (empty)
  void *_progress_userptr{nullptr};

  ///
  /// Call progress callback and return false if parsing should be cancelled
  ///
  bool ReportProgress();
};

///
/// For USDC.
/// Parse string representation of UnregisteredValue(Attribute value).
/// e.g. "[(0, 1), (2, 3)]" for uint2[] type
///
/// @param[in] typeName typeName(e.g. "uint2")
/// @param[in] str Ascii representation of value.
/// @param[out] value Ascii representation of value.
/// @param[out] err Parse error message when returning false.
///
bool ParseUnregistredValue(const std::string &typeName, const std::string &str,
                           value::Value *value, std::string *err);

}  // namespace ascii

}  // namespace lightusd
