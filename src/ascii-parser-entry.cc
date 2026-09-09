// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
//
// Entry-point functions split from ascii-parser.cc:
// ParseVariantSet, ParseBlock, Parse, and supporting utilities.

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#if defined(__wasi__)
#else
#include <mutex>
#include <thread>
#endif
#include <unordered_map>
#include <vector>

#include "ascii-parser.hh"
#include "parser-timing.hh"
#include "path-util.hh"
#include "str-util.hh"
#include "tiny-format.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external
#include "nonstd/expected.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "common-macros.inc"

#include "io-util.hh"
#include "pprint-enum.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"

namespace lightusd {
namespace ascii {

extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<bool>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<int32_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<uint32_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<int64_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<uint64_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<float>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<double>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quath>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quatf>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quatd>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix2f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix4f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix2d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix4d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::token>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::StringData>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<std::string>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Reference>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Payload>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Path>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::AssetPath>> *result);

extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<bool> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<char> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::char2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::char3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::char4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint8_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uchar2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uchar3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uchar4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<int16_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::short2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::short3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::short4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint16_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::ushort2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::ushort3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::ushort4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<int32_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint32_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<int64_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint64_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<float> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<double> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quath> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quatf> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quatd> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix2f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix4f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix2d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix4d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::token> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::StringData> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<std::string> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Reference> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Payload> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Path> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::AssetPath> *result);


constexpr auto kAscii = "[ASCII]";

// Keyword database for fix suggestions (Priority 5)
// Contains common USD specifiers, types, and keywords
// Using C-style array to avoid static initialization requirements
static constexpr const char* g_usd_keywords[] = {
  // Specifiers
  "def", "over", "class",
  // Variability
  "uniform", "varying", "token",
  // Metadata indicators
  "custom", "documentation", "doc", "comment",
  // List edit qualifiers
  "add", "delete", "reorder", "append", "prepend",
  // Relationship indicators
  "rel", "relationship",
  // Attribute modifiers
  "timeVarying",
  // Common scalar types
  "bool", "byte", "ubyte", "int", "uint", "long", "ulong",
  "half", "float", "double", "string", "asset",
  // Vector types
  "int2", "int3", "int4",
  "uint2", "uint3", "uint4",
  "float2", "float3", "float4",
  "double2", "double3", "double4",
  "half2", "half3", "half4",
  // Color types
  "color3h", "color3f", "color3d",
  "color4h", "color4f", "color4d",
  // Matrix types
  "matrix2f", "matrix3f", "matrix4f",
  "matrix2d", "matrix3d", "matrix4d",
  // Geometric types
  "point3h", "point3f", "point3d",
  "vector3h", "vector3f", "vector3d",
  "normal3h", "normal3f", "normal3d",
  "texcoord2h", "texcoord2f", "texcoord2d",
  "texcoord3h", "texcoord3f", "texcoord3d",
  "quath", "quatf", "quatd",
  // Special types
  "path", "reference", "payload",
  // Prim types (common)
  "Mesh", "Sphere", "Cube", "Cylinder", "Cone",
  "Xform", "Scope", "Group", "Assembly",
  "Light", "SphereLight", "RectLight", "DomeLight",
  "Material", "Shader", "Texture",
  "BasisCurves", "PointInstancer", "Points",
  // Attributes (common)
  "points", "normals", "primvars", "indices",
  "extent", "visibility", "purpose", "kind",
  "interpolation", "faceVertexCounts", "faceVertexIndices",
  // Metadata field names
  "timeSamples", "connect", "customData",
  "subLayers", "defaultPrim", "upAxis",
  // Time/frame related
  "timeCodesPerSecond", "startTimeCode", "endTimeCode",
  "framesPerSecond", "metersPerUnit", "kilogramsPerUnit"
};

static constexpr size_t g_usd_keywords_count =
    sizeof(g_usd_keywords) / sizeof(g_usd_keywords[0]);


static void RegisterStageMetas(
    lightusd::HashMap<std::string, AsciiParser::VariableDef> &metas) {
  metas.clear();
  metas["doc"] = AsciiParser::VariableDef(value::kString, "doc");
  metas["documentation"] =
      AsciiParser::VariableDef(value::kString, "documentation");
  metas["documentation"] =
      AsciiParser::VariableDef(value::kString, "doc");  // alias to 'doc'

  metas["comment"] = AsciiParser::VariableDef(value::kString, "comment");

  // TODO: both support float and double?
  metas["metersPerUnit"] =
      AsciiParser::VariableDef(value::kDouble, "metersPerUnit");
  metas["timeCodesPerSecond"] =
      AsciiParser::VariableDef(value::kDouble, "timeCodesPerSecond");
  metas["framesPerSecond"] =
      AsciiParser::VariableDef(value::kDouble, "framesPerSecond");

  metas["startTimeCode"] =
      AsciiParser::VariableDef(value::kDouble, "startTimeCode");
  metas["endTimeCode"] =
      AsciiParser::VariableDef(value::kDouble, "endTimeCode");

  metas["defaultPrim"] = AsciiParser::VariableDef(value::kToken, "defaultPrim");
  metas["upAxis"] = AsciiParser::VariableDef(value::kToken, "upAxis");
  metas["customLayerData"] =
      AsciiParser::VariableDef(value::kDictionary, "customLayerData");

  // Composition arc.
  // Type can be array. i.e. asset, asset[]
  metas["subLayers"] = AsciiParser::VariableDef(value::kAssetPath, "subLayers",
                                                /* allow array type */ true);

  // UsdPhysics
  metas["kilogramsPerUnit"] =
      AsciiParser::VariableDef(value::kDouble, "kilogramsPerUnit");

  // AOUSD Core Spec fields
  metas["colorConfiguration"] =
      AsciiParser::VariableDef(value::kAssetPath, "colorConfiguration");
  metas["colorManagementSystem"] =
      AsciiParser::VariableDef(value::kToken, "colorManagementSystem");
  metas["renderSettingsPrimPath"] =
      AsciiParser::VariableDef(value::kString, "renderSettingsPrimPath");
  metas["owner"] = AsciiParser::VariableDef(value::kString, "owner");
  metas["hasOwnedSubLayers"] =
      AsciiParser::VariableDef(value::kBool, "hasOwnedSubLayers");
  metas["expressionVariables"] =
      AsciiParser::VariableDef(value::kDictionary, "expressionVariables");
  // relocates: path-to-path map (custom parsing in ParseStageMetaOpt)
  // Registered as dictionary type for IsStageMeta() lookup only.
  metas["relocates"] =
      AsciiParser::VariableDef(value::kDictionary, "relocates");

  // USDZ extension
  metas["autoPlay"] = AsciiParser::VariableDef(value::kBool, "autoPlay");
  metas["playbackMode"] =
      AsciiParser::VariableDef(value::kToken, "playbackMode");
}

static void RegisterPrimMetas(
    lightusd::HashMap<std::string, AsciiParser::VariableDef> &metas) {
  metas.clear();

  metas["kind"] = AsciiParser::VariableDef(value::kToken, "kind");
  metas["doc"] = AsciiParser::VariableDef(value::kString, "doc");
  metas["documentation"] =
      AsciiParser::VariableDef(value::kString, "documentation");

  //
  // Composition arcs -----------------------
  //

  // Type can be array. i.e. path, path[]
  metas["references"] = AsciiParser::VariableDef("Reference", "references",
                                                 /* allow array type */ true);

  // TODO: Use relatioship type?
  metas["inherits"] = AsciiParser::VariableDef(value::kPath, "inherits", true);
  metas["payload"] = AsciiParser::VariableDef("Payload", "payload", true);
  metas["specializes"] =
      AsciiParser::VariableDef(value::kPath, "specializes", true);

  // Use `string`
  metas["variantSets"] = AsciiParser::VariableDef(value::kString, "variantSets",
                                                  /* allow array type */ true);

  // Parse as dict. TODO: Use ParseVariants()
  metas["variants"] = AsciiParser::VariableDef(value::kDictionary, "variants");

  // ------------------------------------------

  metas["assetInfo"] =
      AsciiParser::VariableDef(value::kDictionary, "assetInfo");
  metas["customData"] =
      AsciiParser::VariableDef(value::kDictionary, "customData");

  metas["active"] = AsciiParser::VariableDef(value::kBool, "active");
  metas["hidden"] = AsciiParser::VariableDef(value::kBool, "hidden");
  metas["instanceable"] =
      AsciiParser::VariableDef(value::kBool, "instanceable");

  // ListOp
  metas["apiSchemas"] = AsciiParser::VariableDef(
      value::Add1DArraySuffix(value::kToken), "apiSchemas");

  // usdShade
  // NOTE: items are expected to be all string type.
  metas["sdrMetadata"] =
      AsciiParser::VariableDef(value::kDictionary, "sdrMetadata");

  metas["clips"] = AsciiParser::VariableDef(value::kDictionary, "clips");

  // USDZ extension
  metas["sceneName"] = AsciiParser::VariableDef(value::kString, "sceneName");

  // Builtin from pxrUSD 23.xx
  metas["displayName"] =
      AsciiParser::VariableDef(value::kString, "displayName");

}

static void RegisterPropMetas(
    lightusd::HashMap<std::string, AsciiParser::VariableDef> &metas) {
  metas.clear();

  metas["doc"] = AsciiParser::VariableDef(value::kString, "doc");
  metas["documentation"] =
      AsciiParser::VariableDef(value::kString, "documentation");
  metas["active"] = AsciiParser::VariableDef(value::kBool, "active");
  metas["hidden"] = AsciiParser::VariableDef(value::kBool, "hidden");
  metas["customData"] =
      AsciiParser::VariableDef(value::kDictionary, "customData");

  // for sparse primvars
  metas["unauthoredValuesIndex"] =
      AsciiParser::VariableDef(value::kInt, "unauthoredValuesIndex");

  // usdSkel
  metas["elementSize"] = AsciiParser::VariableDef(value::kInt, "elementSize");

  // usdSkel inbetween BlendShape
  // use Double in LightUSD. its float type in pxrUSD.
  metas["weight"] = AsciiParser::VariableDef(value::kDouble, "weight");

  // usdShade?
  metas["colorSpace"] = AsciiParser::VariableDef(value::kToken, "colorSpace");

  metas["interpolation"] =
      AsciiParser::VariableDef(value::kToken, "interpolation");

  // usdShade
  metas["bindMaterialAs"] =
      AsciiParser::VariableDef(value::kToken, "bindMaterialAs");
  metas["connectability"] =
      AsciiParser::VariableDef(value::kToken, "connectability");
  metas["renderType"] = AsciiParser::VariableDef(value::kToken, "renderType");
  metas["outputName"] = AsciiParser::VariableDef(value::kToken, "outputName");
  metas["sdrMetadata"] =
      AsciiParser::VariableDef(value::kDictionary, "sdrMetadata");

  // Builtin from pxrUSD 23.xx
  metas["displayName"] =
      AsciiParser::VariableDef(value::kString, "displayName");

  // Builtin from pxrUSD 24.xx?
  metas["displayGroup"] =
      AsciiParser::VariableDef(value::kString, "displayGroup");

  // pxrUSD attribute metadatum: enumerated set of valid token values.
  // Stored as `token[]` in AttrMetas via set_allowedTokens.
  metas["allowedTokens"] = AsciiParser::VariableDef(
      "token", "allowedTokens", /* allow_array_type */ true);

  // `kind` on a Property (Relationship/Attribute). USD spec restricts
  // `kind` to prims, but pxr's parser accepts it on properties too —
  // we mirror that tolerance and store via the generic data() dict so
  // USDA round-trip preserves it.
  metas["kind"] = AsciiParser::VariableDef(value::kToken, "kind");
}

// Shared implementation lives in value-types.hh:
// value::RegisterPrimAttrTypes<SetType>(d, include_variant_set)

static void RegisterPrimTypes(std::unordered_set<std::string> &d) {
  d.insert("Xform");
  d.insert("Sphere");
  d.insert("Cube");
  d.insert("Cone");
  d.insert("Cylinder");
  d.insert("Capsule");
  d.insert("BasisCurves");
  d.insert("Mesh");
  d.insert("Points");
  d.insert("GeomSubset");
  d.insert("Scope");
  // UsdVol / UsdRender placeholder prim types.
  d.insert("Volume");
  d.insert("OpenVDBAsset");
  d.insert("Field3DAsset");
  d.insert("RenderSettings");
  d.insert("RenderProduct");
  d.insert("RenderVar");
  d.insert("GenerativeProcedural");
  d.insert("Material");
  d.insert("NodeGraph");
  d.insert("Shader");
  d.insert("SphereLight");
  d.insert("DomeLight");
  d.insert("DiskLight");
  d.insert("DistantLight");
  d.insert("RectLight");
  d.insert("CylinderLight");
  d.insert("GeometryLight");
  d.insert("PortalLight");
  d.insert("Camera");
  d.insert("SkelRoot");
  d.insert("Skeleton");
  d.insert("SkelAnimation");
  d.insert("BlendShape");

  // UsdVol
  d.insert("Volume");
  d.insert("FieldAsset");
  d.insert("OpenVDBAsset");
  d.insert("Field3DAsset");

  d.insert("GPrim");
}

// LightUSD does not allow user-defined API schema at the moment
// (Primarily for security reason, secondary it requires re-design of Prim
// classes to support user-defined API schema)
static void RegisterAPISchemas(std::unordered_set<std::string> &d) {
  d.insert("MaterialBindingAPI");
  d.insert("SkelBindingAPI");
  d.insert("MaterialXConfigAPI");  // usdMtlx: config:mtlx:* on Material
  d.insert("ColorSpaceAPI");
  d.insert("ColorSpaceDefinitionAPI");
  // CollectionAPI is a supported multiple-apply schema.  The instance name
  // (for example CollectionAPI:render) is retained in APISchemas and is
  // validated separately from the base schema name.
  d.insert("CollectionAPI");
  d.insert("NewtonSceneAPI");
  d.insert("NewtonXpbdSceneAPI");
  d.insert("NewtonKaminoSceneAPI");
  d.insert("NewtonArticulationRootAPI");
  d.insert("NewtonCollisionAPI");
  d.insert("NewtonMeshCollisionAPI");
  d.insert("NewtonMaterialAPI");
  d.insert("NewtonMimicAPI");
  d.insert("NewtonActuatorDelayAPI");
  d.insert("NewtonActuatorControlBaseAPI");
  d.insert("NewtonPDControlAPI");
  d.insert("NewtonPIDControlAPI");
  d.insert("NewtonNeuralControlAPI");
  d.insert("NewtonActuatorClampingBaseAPI");
  d.insert("NewtonMaxEffortClampingAPI");
  d.insert("NewtonDCMotorClampingAPI");
  d.insert("NewtonPositionBasedClampingAPI");

  // d.insert("PhysicsCollisionAPI");
  // d.insert("PhysicsRigidBodyAPI");

  // PhysicsLimitAPI and PhysicsDriveAPI are also multi-apply schemas, but
  // their typed schema properties are not yet reconstructed by the legacy
  // Prim API. Keep them on the unknown-schema path until that support lands.
}

std::string AsciiParser::GetCurrentPrimPath() {
  if (_path_stack.empty()) {
    return "/";
  }

  return _path_stack.top();
}

//
// -- ctor, dtor
//

AsciiParser::AsciiParser() { Setup(); }

AsciiParser::AsciiParser(StreamReader *sr) : _sr(sr) { Setup(); }

std::string AsciiParser::GenerateSuggestion(const std::string& invalid_token) {
  // Only generate suggestions if feature is enabled and token is not empty
  if (!LIGHTUSD_ENABLE_SUGGEST_FIX || invalid_token.empty()) {
    return "";
  }

  // Convert C-style keyword array to vector for string similarity matching
  std::vector<std::string> keywords(g_usd_keywords,
                                     g_usd_keywords + g_usd_keywords_count);

  // Find closest matching keyword
  std::string best_match = string_similarity::FindClosestMatch(
      invalid_token, keywords, 0.6);  // 0.6 = 60% similarity threshold

  if (!best_match.empty()) {
    return fmt::format("Did you mean '{}'?", best_match);
  }

  return "";
}

void AsciiParser::Setup() {
  RegisterStageMetas(_supported_stage_metas);
  RegisterPrimMetas(_supported_prim_metas);
  RegisterPropMetas(_supported_prop_metas);
  value::RegisterPrimAttrTypes(_supported_prim_attr_types, /* include_variant_set */ true);
  RegisterPrimTypes(_supported_prim_types);
  RegisterAPISchemas(_supported_api_schemas);
  _cursor_store.Clear();
}

void AsciiParser::RecordLayerMetaCursor(const std::string &meta_name,
                                        const Cursor &cursor) {
  _cursor_store.StoreLayerMeta(meta_name, cursor);
}

void AsciiParser::RecordPrimCursor(const std::string &prim_path,
                                   const Cursor &cursor) {
  _cursor_store.StorePrim(prim_path, cursor);
}

void AsciiParser::RecordPrimAttrCursor(const std::string &prim_path,
                                       const std::string &property_name,
                                       const Cursor &cursor) {
  _cursor_store.StorePrimAttr(prim_path, property_name, cursor);
}

void AsciiParser::RecordPropertyCursor(const std::string &prim_path,
                                       const std::string &property_name,
                                       const Cursor &cursor) {
  _cursor_store.StoreProperty(prim_path, property_name, cursor);
}

namespace {

std::string GetSourceLineAtCursor(const lightusd::StreamReader *sr, int target_row) {
  if (!sr || !sr->data() || (target_row < 0)) {
    return std::string();
  }

  const uint8_t *data = sr->data();
  const uint64_t size = sr->size();

  int row = 0;
  std::string line;
  for (uint64_t i = 0; i < size; ++i) {
    const char ch = static_cast<char>(data[i]);
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      if (row == target_row) {
        return std::string(line);
      }
      row++;
      line.clear();
      continue;
    }

    if (row == target_row) {
      line.push_back(ch);
    }
  }

  return (row == target_row) ? std::string(line) : std::string();
}

}  // namespace

std::string AsciiParser::FormatCursorDiagnostic(const Cursor &cursor,
                                                int column_width) const {
  if ((cursor.row < 0) || (cursor.col < 0)) {
    return std::string();
  }

  if (column_width < 16) {
    column_width = 16;
  }

  std::stringstream ss;
  ss << "Semantic source at line " << (cursor.row + 1) << ", column "
     << (cursor.col + 1) << ":\n";

  std::string source_line = GetSourceLineAtCursor(_sr, cursor.row);
  if (source_line.empty()) {
    std::string diag = ss.str();
    if (!diag.empty() && diag.back() == '\n') {
      diag.pop_back();
    }
    return diag;
  }

  std::string display_line = source_line;
  int caret_offset = cursor.col;

  if (static_cast<int>(source_line.size()) > column_width) {
    const int half_width = column_width / 2;
    int start_col = std::max(0, cursor.col - half_width);
    int end_col = std::min(static_cast<int>(source_line.size()),
                           start_col + column_width);
    if ((end_col - start_col) < column_width) {
      start_col = std::max(0, end_col - column_width);
    }

    display_line = source_line.substr(
        static_cast<size_t>(start_col),
        static_cast<size_t>(end_col - start_col));
    caret_offset = cursor.col - start_col;

    if (start_col > 0) {
      display_line = "..." + display_line;
      caret_offset += 3;
    }

    if (end_col < static_cast<int>(source_line.size())) {
      display_line += "...";
    }
  }

  ss << "  > " << display_line << "\n";
  ss << "    ";
  for (int i = 0; i < caret_offset; ++i) {
    ss << " ";
  }
  ss << "^";

  return ss.str();
}

std::string AsciiParser::FormatStoredCursorDiagnostic(const StoredCursor *stored,
                                                      int column_width) const {
  if (!stored) {
    return std::string();
  }

  return FormatCursorDiagnostic(stored->cursor, column_width);
}

std::string AsciiParser::FormatLayerMetaSourceDiagnostic(
    const std::string &meta_name, int column_width) const {
  return FormatStoredCursorDiagnostic(_cursor_store.FindLayerMeta(meta_name),
                                      column_width);
}

std::string AsciiParser::FormatPrimSourceDiagnostic(
    const std::string &prim_path, int column_width) const {
  return FormatStoredCursorDiagnostic(_cursor_store.FindPrim(prim_path),
                                      column_width);
}

std::string AsciiParser::FormatPrimAttrSourceDiagnostic(
    const std::string &prim_path, const std::string &property_name,
    int column_width) const {
  const StoredCursor *stored = _cursor_store.FindPrimAttr(prim_path, property_name);
  if (!stored) {
    stored = _cursor_store.FindProperty(prim_path, property_name);
  }
  if (!stored) {
    stored = _cursor_store.FindPrim(prim_path);
  }
  return FormatStoredCursorDiagnostic(stored, column_width);
}

std::string AsciiParser::FormatPropertySourceDiagnostic(
    const std::string &prim_path, const std::string &property_name,
    int column_width) const {
  const StoredCursor *stored = _cursor_store.FindProperty(prim_path, property_name);
  if (!stored) {
    stored = _cursor_store.FindPrimAttr(prim_path, property_name);
  }
  if (!stored) {
    stored = _cursor_store.FindPrim(prim_path);
  }
  return FormatStoredCursorDiagnostic(stored, column_width);
}

bool AsciiParser::ReportProgress() {
  // Check if callback exists and is callable
  if (!_progress_callback) {
    return true;  // No callback, continue parsing
  }

  if (!_sr) {
    return true;  // No stream reader, can't compute progress
  }

  // Calculate progress based on current position in stream
  uint64_t current_pos = _sr->tell();
  uint64_t total_size = _sr->size();

  float progress = 0.0f;
  if (total_size > 0) {
    progress = static_cast<float>(current_pos) / static_cast<float>(total_size);
    // Clamp to [0, 1] range
    if (progress > 1.0f) progress = 1.0f;
    if (progress < 0.0f) progress = 0.0f;
  }

  // Call the callback and return its result
  return _progress_callback(progress, _progress_userptr);
}

AsciiParser::~AsciiParser() {}

bool AsciiParser::CheckHeader() { return ParseMagicHeader(); }

bool AsciiParser::IsRegisteredPrimMeta(const std::string &name) {
  return _supported_prim_metas.count(name) ? true : false;
}

bool AsciiParser::IsStageMeta(const std::string &name) {
  return _supported_stage_metas.count(name) ? true : false;
}

bool AsciiParser::ParseVariantSet(
    const int64_t primIdx, const int64_t parentPrimIdx, const uint32_t depth,
    VariantSetContent *variantSetContentOut) {

  if (depth > 512) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii, "[InternalError] VariantSet nesting too deep (> 512).");
  }

  if (!variantSetContentOut) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii,
                              "[InternalError] variantSetContentOut arg is nullptr.");
  }

  // variantSet =
  // {
  //   "variantName0" ( metas ) { ... }
  //   "variantName1" ( metas ) { ... }
  //   ...
  // }
  if (!Expect('{')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  VariantSetContent variantSetContent;

  // for each variantStatement
  while (!Eof()) {
    {
      char c;
      if (!Char1(&c)) {
        return false;
      }

      if (c == '}') {
        // end
        break;
      }

      Rewind(1);
    }

    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    // string
    std::string variantName;
    if (!ReadBasicType(&variantName)) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, "Failed to parse variant name for `variantSet` statement.");
    }

    if (!SkipWhitespace()) {
      return false;
    }

    // Optional: PrimSpec meta
    PrimMetaMap metas;
    {
      char mc;
      if (!LookChar1(&mc)) {
        return false;
      }

      if (mc == '(') {
        if (!ParsePrimMetas(&metas)) {
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii, "Failed to parse PrimSpec metas in variant statement.");
        }
      }
    }

    if (!Expect('{')) {
      return false;
    }

    VariantContent variantContent;

    int64_t variantPrimIdx = _prim_idx_assign_fun(parentPrimIdx);
    //variantContent.variantPrimIdx = variantPrimIdx;
    DCOUT("primIdx for variant = " << variantPrimIdx);

    while (!Eof()) {

      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      {
        char c;
        if (!Char1(&c)) {
          return false;
        }

        if (c == '}') {
          DCOUT("End block in variantSet stmt.");
          // end block
          break;
        }
      }

      if (!Rewind(1)) {
        return false;
      }

      DCOUT("Read first token in VariantSet stmt");
      Identifier tok;
      if (!ReadBasicType(&tok)) {
        PUSH_ERROR_AND_RETURN(
            "Failed to parse an identifier in variantSet block statement.");
      }

      if (tok == "variantSet") {

        if (!SkipWhitespace()) {
          return false;
        }

        std::string childVariantName;
        if (!ReadBasicType(&childVariantName)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `variantSet` statement.");
        }

        DCOUT("childVariantName = " << childVariantName);

        if (!SkipWhitespace()) {
          return false;
        }

        if (!Expect('=')) {
          return false;
        }

        if (!SkipWhitespace()) {
          return false;
        }


        VariantSetContent child_vmap;
        if (!ParseVariantSet(variantPrimIdx, primIdx, depth+1, &child_vmap)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `variantSet` statement.");
        }

        variantContent.variantSets[childVariantName] = child_vmap;

      } else {

        if (!Rewind(tok.size())) {
          return false;
        }

        if (!SkipWhitespace()) {
          return false;
        }

        Specifier child_spec{Specifier::Invalid};
        if (tok == "def") {
          child_spec = Specifier::Def;
        } else if (tok == "over") {
          child_spec = Specifier::Over;
        } else if (tok == "class") {
          child_spec = Specifier::Class;
        }

        // No specifier => Assume properties only.
        // Has specifier => Prim
        if (child_spec != Specifier::Invalid) {
          int64_t idx = _prim_idx_assign_fun(parentPrimIdx);
          DCOUT("enter parseBlock in variantSet. spec = " << to_string(child_spec) << ", idx = "
                                          << idx << ", rootIdx = " << primIdx);

          // recusive call
          if (!ParseBlock(child_spec, idx, primIdx, depth + 1, /* in_variantStmt */true)) {
            PUSH_ERROR_AND_RETURN(
                fmt::format("`{}` block parse failed.", to_string(child_spec)));
          }
          DCOUT(fmt::format("Done parse `{}` block.", to_string(child_spec)));

          DCOUT(fmt::format("Add primIdx {} to variant {}", idx, variantName));
          variantContent.primIndices.push_back(idx);

        } else {
          DCOUT("Enter ParsePrimProps.");
          if (!ParsePrimProps(&variantContent.props, &variantContent.properties)) {
            PUSH_ERROR_AND_RETURN("Failed to parse Prim attribute.");
          }
          DCOUT(fmt::format("Done parse ParsePrimProps."));
        }

        if (!SkipCommentAndWhitespaceAndNewline()) {
          return false;
        }
      }
    }

    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    DCOUT(fmt::format("variantSet item {} parsed.", variantName));



    variantContent.metas = metas;
    variantSetContent.variantSets[variantName] = variantContent;
  }

  variantSetContent.variantPrimIdx = primIdx;

  (*variantSetContentOut) = std::move(variantSetContent);

  return true;
}

///
/// Parse block.
///
/// block = spec prim_type? token metas? { ... }
/// metas = '(' args ')'
///
/// spec = `def`, `over` or `class`
///
///
bool AsciiParser::ParseBlock(const Specifier spec, const int64_t primIdx,
                             const int64_t parentPrimIdx, const uint32_t depth,
                             const bool in_variantStaement) {
  (void)in_variantStaement;

  DCOUT("ParseBlock");

  if (depth > 512) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii, "[InternalError] Prim definition nesting too deep (> 512).");
  }

  // Report progress and check for cancellation
  if (!ReportProgress()) {
    PUSH_ERROR_AND_RETURN("Parsing cancelled by progress callback.");
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    DCOUT("SkipCommentAndWhitespaceAndNewline failed");
    return false;
  }

  Cursor prim_decl_cursor = _curr_cursor;

  Identifier def;
  if (!ReadIdentifier(&def)) {
    DCOUT("ReadIdentifier failed");
    return false;
  }
  DCOUT("spec = " << def);

  if ((def == "def") || (def == "over") || (def == "class")) {
    // ok
  } else {
    PUSH_ERROR_AND_RETURN("Invalid specifier.");
  }

  // Ensure spec and def is same.
  if (def == "def") {
    if (spec != Specifier::Def) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, "Internal error. Invalid Specifier token combination. def = "
                      << def << ", spec = " << to_string(spec));
    }
  } else if (def == "over") {
    if (spec != Specifier::Over) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, "Internal error. Invalid Specifier token combination. def = "
                      << def << ", spec = " << to_string(spec));
    }
  } else if (def == "class") {
    if (spec != Specifier::Class) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, "Internal error. Invalid Specifier token combination. def = "
                      << def << ", spec = " << to_string(spec));
    }
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  // look ahead
  bool has_primtype = false;
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (!Rewind(1)) {
      return false;
    }

    if (c == '"') {
      // token
      has_primtype = false;
    } else {
      has_primtype = true;
    }
  }

  Identifier prim_type;

  DCOUT("has_primtype = " << has_primtype);

  if (has_primtype) {
    if (!ReadIdentifier(&prim_type)) {
      return false;
    }
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  std::string prim_name;
  if (!ReadBasicType(&prim_name)) {
    return false;
  }

  DCOUT("prim name = " << prim_name);
  if (!ValidatePrimElementName(prim_name)) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii, "Prim name contains invalid chacracter.");
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  PrimMetaMap in_metas;
  // Reorder statements collected while parsing THIS block's body (nested
  // blocks consume their own entries before returning).
  const size_t reorder_mark = _pending_reorders.size();
  {
    // look ahead
    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    if (c == '(') {
      // meta

      if (!ParsePrimMetas(&in_metas)) {
        DCOUT("Parse Prim metas failed.");
        return false;
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }
    }
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect('{')) {
    return false;
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  std::map<std::string, Property> props;
  std::vector<value::token> propNames;
  VariantSetList variantSetList;

  {
    std::string full_path = GetCurrentPrimPath();
    if (full_path == "/") {
      full_path += prim_name;
    } else {
      full_path += "/" + prim_name;
    }
    RecordPrimCursor(full_path, prim_decl_cursor);
    PushPrimPath(full_path);
  }

  // expect = '}'
  //        | def_block
  //        | prim_attr+
  //        | variantSet '{' ... '}'
  while (!Eof()) {
    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '}') {
      // end block
      break;
    } else {
      if (!Rewind(1)) {
        return false;
      }

      DCOUT("Read stmt token");
      Identifier tok;
      if (!ReadBasicType(&tok)) {
        // maybe ';'?

        if (LookChar1(&c)) {
          if (c == ';') {
            PUSH_ERROR_AND_RETURN(
                "Semicolon is not allowd in `def` block statement.");
          }
        }
        PUSH_ERROR_AND_RETURN(
            "Failed to parse an identifier in `def` block statement.");
      }

      if (tok == "variantSet") {
        if (!SkipWhitespace()) {
          return false;
        }

        std::string variantName;
        if (!ReadBasicType(&variantName)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `variantSet` statement.");
        }

        DCOUT("variantName = " << variantName);

        if (!SkipWhitespace()) {
          return false;
        }

        if (!Expect('=')) {
          return false;
        }

        if (!SkipWhitespace()) {
          return false;
        }

        int64_t variantPrimIdx = _prim_idx_assign_fun(parentPrimIdx);

        VariantSetContent vs;
        if (!ParseVariantSet(variantPrimIdx, primIdx, depth, &vs)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `variantSet` statement.");
        }

        vs.variantPrimIdx = variantPrimIdx;
        variantSetList[variantName] = vs;

        continue;
      }

      if (!Rewind(tok.size())) {
        return false;
      }

      Specifier child_spec{Specifier::Invalid};
      if (tok == "def") {
        child_spec = Specifier::Def;
      } else if (tok == "over") {
        child_spec = Specifier::Over;
      } else if (tok == "class") {
        child_spec = Specifier::Class;
      }

      if (child_spec != Specifier::Invalid) {
        int64_t idx = _prim_idx_assign_fun(parentPrimIdx);
        DCOUT("enter parseDef. spec = " << to_string(child_spec) << ", idx = "
                                        << idx << ", rootIdx = " << primIdx);

        // recusive call
        if (!ParseBlock(child_spec, idx, primIdx, depth + 1)) {
          PUSH_ERROR_AND_RETURN(
              fmt::format("Failed to parse `{}` block.", to_string(child_spec)));
        }
        DCOUT(fmt::format("Done parse `{}` block.", to_string(child_spec)));
      } else {
        DCOUT("Enter ParsePrimProps.");
        // Assume PrimAttr
        int row_before = _curr_cursor.row;
        if (!ParsePrimProps(&props, &propNames)) {
          PUSH_ERROR_AND_RETURN("Failed to parse Prim attribute.");
        }

        // Each property statement must be terminated by a newline (or
        // comment / closing brace / EOF).  Reject multiple statements
        // crammed onto a single line separated only by whitespace.
        // Only check when ParsePrimProps stayed on the same line (some
        // code paths inside ParsePrimProps consume trailing newlines).
        if (_curr_cursor.row == row_before && !Eof()) {
          if (!SkipWhitespace()) {
            return false;
          }
          if (!Eof()) {
            char nc;
            if (!LookChar1(&nc)) {
              return false;
            }
            if (nc != '\n' && nc != '\r' && nc != '#' && nc != '}' && nc != ';') {
              PUSH_ERROR_AND_RETURN(
                  "Newline expected after property statement. "
                  "Multiple property statements on a single line are not allowed.");
            }
          }
        }
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }
    }
  }

  // Attach `reorder` body statements parsed in THIS block to the prim's
  // metas as synthetic "reorder:<field>" entries (nested blocks already
  // consumed theirs); the reader routes them to PrimMetas and the printer
  // re-emits the body statements (pxr keeps them verbatim).
  for (size_t ri = reorder_mark; ri < _pending_reorders.size(); ri++) {
    MetaVariable rv;
    rv.set_value(_pending_reorders[ri].second);
    in_metas.emplace("reorder:" + _pending_reorders[ri].first,
                     std::make_pair(ListEditQual::ResetToExplicit, rv));
  }
  _pending_reorders.resize(reorder_mark);

  std::string pTy = prim_type;

  if (_primspec_mode) {
    // Load scene as PrimSpec tree
    if (_primspec_fun) {
      Path fullpath(GetCurrentPrimPath(), "");
      Path pname(prim_name, "");

      // pass prim_type as is(empty = empty string)
      nonstd::expected<bool, std::string> ret =
          _primspec_fun(fullpath, spec, prim_type, pname, primIdx,
                        parentPrimIdx, props, in_metas, variantSetList);

      if (!ret) {
        // construction failed.
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to construct PrimSpec (type `{}`, element `{}`): {}",
            prim_type, prim_name, ret.error()));
      }
    } else {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, "[Internal Error] PrimSpec handler is not found.");
    }

  } else {
    // Create typed Prim.

    if (prim_type.empty()) {
      // No Prim type specified. Treat it as Model

      pTy = "Model";
    }

    auto it = _prim_construct_fun_map.find(pTy);
    if (it == _prim_construct_fun_map.end()) {
      if (_option.allow_unknown_prim) {
        // Unknown Prim type specified. Treat it as Model
        // Prim's type name will be storead in Model::prim_type_name
        pTy = "Model";
        it = _prim_construct_fun_map.find(pTy);
      }
    }

    if (it != _prim_construct_fun_map.end()) {
      auto construct_fun = it->second;

      Path fullpath(GetCurrentPrimPath(), "");
      Path pname(prim_name, "");
      nonstd::expected<bool, std::string> ret =
          construct_fun(fullpath, spec, prim_type, pname, primIdx,
                        parentPrimIdx, props, in_metas, variantSetList);

      if (!ret) {
        // construction failed.
        PUSH_ERROR_AND_RETURN("Failed to construct prim `" +
                              fullpath.full_path_name() + "` of type `" + pTy +
                              "`: " + ret.error());
      }

    } else {
      PUSH_WARN(fmt::format(
          "TODO: Unsupported/Unimplemented Prim type: `{}`. Skipping parsing.",
          pTy));
    }
  }

  PopPrimPath();

  return true;
}

///
/// Parser entry point
/// TODO: Refactor and use unified code path regardless of LoadState.
///
bool AsciiParser::Parse(const uint32_t load_states,
                        const AsciiParserOption &parser_option) {
  LIGHTUSD_PROFILE_FUNCTION("ascii-parser");

  _cursor_store.Clear();

  _toplevel = (load_states & static_cast<uint32_t>(LoadState::Toplevel));
  _sub_layered = (load_states & static_cast<uint32_t>(LoadState::Sublayer));
  _referenced = (load_states & static_cast<uint32_t>(LoadState::Reference));
  _payloaded = (load_states & static_cast<uint32_t>(LoadState::Payload));
  _option = parser_option;

  bool header_ok;
  {
    LIGHTUSD_PROFILE_SCOPE("ascii-parser", "ParseMagicHeader");
    header_ok = ParseMagicHeader();
  }
  if (!header_ok) {
    PUSH_ERROR_AND_RETURN("Failed to parse USDA magic header.\n");
  }

  SkipCommentAndWhitespaceAndNewline();

  if (Eof()) {
    // Empty USDA
    return true;
  }

  {
    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    if (c == '(') {
      // stage meta.
      LIGHTUSD_PROFILE_SCOPE("ascii-parser", "ParseStageMetas");
      if (!ParseStageMetas()) {
        PUSH_ERROR_AND_RETURN("Failed to parse Stage metas.");
      }
    }
  }

  if (_stage_meta_process_fun) {
    DCOUT("Invoke StageMeta callback.");
    bool ret = _stage_meta_process_fun(_stage_metas);
    if (!ret) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct Stage metas.");
    }
  } else {
    // TODO: Report error when StageMeta callback is not set?
    PUSH_WARN("Stage metadata processing callback is not set.");
  }

  PushPrimPath("/");

  // parse blocks
  {
    LIGHTUSD_PROFILE_SCOPE("ascii-parser", "ParseBlocks");
    while (!Eof()) {
      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      if (Eof()) {
        // Whitespaces in the end of line.
        break;
      }

      // Look ahead token
      auto curr_loc = _sr->tell();

      Identifier tok;
      if (!ReadBasicType(&tok)) {
        PUSH_ERROR_AND_RETURN("Identifier expected.\n");
      }

      // Rewind
      if (!SeekTo(curr_loc)) {
        return false;
      }

      // Layer-level `reorder rootPrims = [...]`. This is a namespace *ordering*
      // statement, not a prim, so it appears where a specifier would and would
      // otherwise be reported as an invalid specifier. Parse and consume it.
      //
      // The prim-level counterpart (`reorder nameChildren` / `reorder
      // properties`, ParsePrimProps) preserves its statements verbatim through
      // PrimMetas, because PrimMetaMap already has a slot for them and the prim
      // printer re-emits them. There is no equivalent layer-level slot: storing
      // this into LayerMetas::primChildren would make the exporter reorder the
      // emitted root prims (stage.cc), so a parse -> export -> re-parse cycle
      // would no longer compare equal. Ordering carries no data opinions, so the
      // statement is consumed rather than applied.
      if (tok == "reorder") {
        // Consume the `reorder` identifier we only peeked at above.
        Identifier reorder_kw;
        if (!ReadBasicType(&reorder_kw)) {
          PUSH_ERROR_AND_RETURN("Failed to read `reorder` keyword.");
        }
        if (!SkipWhitespace()) {
          return false;
        }
        std::string reorder_field;
        if (!ReadIdentifier(&reorder_field)) {
          PUSH_ERROR_AND_RETURN(
              "Expected a field name (e.g. `rootPrims`) after `reorder`.");
        }
        if (!SkipWhitespace()) {
          return false;
        }
        if (!Expect('=')) {
          PUSH_ERROR_AND_RETURN("Expected `=` in `reorder` statement.");
        }
        if (!SkipWhitespaceAndNewline()) {
          return false;
        }
        std::vector<value::token> reorder_toks;
        if (!ParseTokenArrayOptimized(&reorder_toks)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to parse token array for `reorder {}`.", reorder_field));
        }
        DCOUT("Parsed layer-level `reorder " << reorder_field << "` ("
              << reorder_toks.size() << " entries; consumed).");
        if (!SkipCommentAndWhitespaceAndNewline()) {
          return false;
        }
        continue;
      }

      Specifier spec{Specifier::Invalid};
      if (tok == "def") {
        spec = Specifier::Def;
      } else if (tok == "over") {
        spec = Specifier::Over;
      } else if (tok == "class") {
        spec = Specifier::Class;
      } else {
        // Generate suggestion for invalid specifier using string similarity (Priority 5)
        std::string suggestion = GenerateSuggestion(tok);
        std::string error_msg = "Invalid specifier token '" + tok + "'";
        PushError(error_msg, ErrorType::SyntaxError, ErrorRecoveryHint::NoHint, suggestion);
        return false;
      }

      int64_t primIdx = _prim_idx_assign_fun(-1);
      DCOUT("Enter parseDef. primIdx = " << primIdx
                                         << ", parentPrimIdx = root(-1)");
      bool block_ok;
      {
        LIGHTUSD_PROFILE_SCOPE("ascii-parser", "ParseBlock");
        block_ok = ParseBlock(spec, primIdx, /* parent */ -1, /* depth */ 0,
                             /* in_variantStmt */ false);
      }
      if (!block_ok) {
        PUSH_ERROR_AND_RETURN("Failed to parse `def` block.");
      }
    }
  }

  return true;
}

bool ParseUnregistredValue(const std::string &_typeName, const std::string &str,
                           value::Value *value, std::string *err) {
  if (!value) {
    if (err) {
      (*err) += "`value` argument is nullptr.\n";
    }
    return false;
  }

  bool array_qual = false;
  std::string typeName = _typeName;
  if (endsWith(typeName, "[]")) {
    typeName = removeSuffix(typeName, "[]");
    array_qual = true;
  }

  nonstd::optional<uint32_t> typeId = value::TryGetTypeId(typeName);

  if (!typeId) {
    if (err) {
      (*err) += "Unsupported type: " + typeName + "\n";
    }
    return false;
  }

  lightusd::StreamReader sr(reinterpret_cast<const uint8_t *>(str.data()),
                            str.size(), /* swap endian */ false);
  lightusd::ascii::AsciiParser parser(&sr);

#define PARSE_BASE_TYPE(__ty)                                            \
  case value::TypeTraits<__ty>::type_id(): {                             \
    if (array_qual) {                                                    \
      std::vector<__ty> vss;                                             \
      if (!parser.ParseBasicTypeArray(&vss)) {                           \
        if (err) {                                                       \
          (*err) = fmt::format("Failed to parse a value of type `{}[]`", \
                               value::TypeTraits<__ty>::type_name());    \
        }                                                                \
        return false;                                                    \
      }                                                                  \
      dst = vss;                                                         \
    } else {                                                             \
      __ty val;                                                          \
      if (!parser.ReadBasicType(&val)) {                                 \
        if (err) {                                                       \
          (*err) = fmt::format("Failed to parse a value of type `{}`",   \
                               value::TypeTraits<__ty>::type_name());    \
        }                                                                \
        return false;                                                    \
      }                                                                  \
      dst = val;                                                         \
    }                                                                    \
    break;                                                               \
  }

  value::Value dst;

  switch (typeId.value()) {
    PARSE_BASE_TYPE(char)
    PARSE_BASE_TYPE(value::char2)
    PARSE_BASE_TYPE(value::char3)
    PARSE_BASE_TYPE(value::char4)
    PARSE_BASE_TYPE(uint8_t)
    PARSE_BASE_TYPE(value::uchar2)
    PARSE_BASE_TYPE(value::uchar3)
    PARSE_BASE_TYPE(value::uchar4)
    PARSE_BASE_TYPE(int16_t)
    PARSE_BASE_TYPE(value::short2)
    PARSE_BASE_TYPE(value::short3)
    PARSE_BASE_TYPE(value::short4)
    PARSE_BASE_TYPE(uint16_t)
    PARSE_BASE_TYPE(value::ushort2)
    PARSE_BASE_TYPE(value::ushort3)
    PARSE_BASE_TYPE(value::ushort4)
    PARSE_BASE_TYPE(value::uint2)
    PARSE_BASE_TYPE(value::uint3)
    PARSE_BASE_TYPE(value::uint4)
    PARSE_BASE_TYPE(value::matrix2f)
    PARSE_BASE_TYPE(value::matrix3f)
    PARSE_BASE_TYPE(value::matrix4f)
    default: {
      if (err) {
        (*err) =
            fmt::format("Unsupported or unimplemeneted type `{}`", typeName);
      }
      return false;
    }
  }

  (*value) = std::move(dst);

  return true;
}

}  // namespace ascii
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
