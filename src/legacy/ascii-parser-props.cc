// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment, Inc.

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#if defined(__wasi__)
#else
#include <mutex>
#include <thread>
#endif
#include <vector>

#include "ascii-parser.hh"
#include "str-util.hh"
#include "tiny-format.hh"

//
#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

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

//

#include "common-macros.inc"

#define CHECK_MEMORY_USAGE(__nbytes) do { \
  uint64_t _chk_nbytes = static_cast<uint64_t>(__nbytes); \
  if (_chk_nbytes > (_max_memory_limit_bytes - _memory_usage)) { \
    PushError(fmt::format("Memory limit exceeded. Limit: {} MB, Current usage: {} MB", \
      _max_memory_limit_bytes / (1024*1024), _memory_usage / (1024*1024))); \
    return false; \
  } \
  _memory_usage += _chk_nbytes; \
  } while(0)

#include "io-util.hh"
#include "path-util.hh"
#include "pprint-enum.hh"
#include "core/prim-spec.hh"
#include "core/material-binding.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "array-edit.hh"  // value::ArrayEdit + op helpers

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
    std::vector<nonstd::optional<value::PathExpression>> *result);

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
    std::vector<value::frame4d> *result);
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
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::PathExpression> *result);


constexpr auto kRel = "rel";
constexpr auto kTimeSamplesSuffix = ".timeSamples";
constexpr auto kConnectSuffix = ".connect";
constexpr auto kSplineSuffix = ".spline";

constexpr auto kAscii = "[ASCII]";

inline bool hasInputs(const std::string &str) {
  return startsWith(str, "inputs:");
}

inline bool hasOutputs(const std::string &str) {
  return startsWith(str, "outputs:");
}

nonstd::optional<std::pair<ListEditQual, MetaVariable>>
AsciiParser::ParsePrimMeta() {
  if (!SkipCommentAndWhitespaceAndNewline()) {
    return nonstd::nullopt;
  }

  lightusd::ListEditQual qual{ListEditQual::ResetToExplicit};

  // May be string only(varname is "comment")
  // For some reason, string-only data is just stored in `MetaVariable` and
  // reconstructed in ReconstructPrimMeta in usda-reader.cc later
  //
  {
    value::StringData sdata;
    if (MaybeTripleQuotedString(&sdata)) {
      MetaVariable var;
      // empty name
      var.set_value("comment", sdata);

      return std::make_pair(qual, var);

    } else if (MaybeString(&sdata)) {
      MetaVariable var;
      var.set_value("comment", sdata);

      return std::make_pair(qual, var);
    }
  }

  if (!MaybeListEditQual(&qual)) {
    return nonstd::nullopt;
  }

  DCOUT("list-edit qual: " << lightusd::to_string(qual));

  if (!SkipWhitespaceAndNewline()) {
    return nonstd::nullopt;
  }

  std::string varname;
  if (!ReadIdentifier(&varname)) {
    return nonstd::nullopt;
  }

  DCOUT("Identifier = " << varname);

  bool registered_meta = IsRegisteredPrimMeta(varname);

  if (!Expect('=')) {
    PUSH_ERROR("'=' expected in Prim Metadata line.");
    return nonstd::nullopt;
  }
  SkipWhitespace();

  if (!registered_meta) {
    // Special handling for "comment =" syntax (extension to USD spec)
    // Parse comment value as a proper string (including triple-quoted)
    if (varname == "comment") {
      value::StringData sdata;
      if (MaybeTripleQuotedString(&sdata)) {
        sdata.has_comment_prefix = true;  // Mark as having "comment =" prefix
        MetaVariable var;
        var.set_value("comment", sdata);
        return std::make_pair(qual, var);
      } else if (MaybeString(&sdata)) {
        sdata.has_comment_prefix = true;  // Mark as having "comment =" prefix
        MetaVariable var;
        var.set_value("comment", sdata);
        return std::make_pair(qual, var);
      } else {
        PUSH_ERROR("Failed to parse string value for 'comment' metadata.");
        return nonstd::nullopt;
      }
    }

    // parse as string until newline

    std::string content;
    if (!ReadUntilNewline(&content)) {
      PUSH_ERROR("Failed to parse unregistered Prim metadata.");
      return nonstd::nullopt;
    }

    MetaVariable var;
    var.set_value(varname, content);

    return std::make_pair(qual, var);
  } else {
    if (auto pv = GetPrimMetaDefinition(varname)) {
      MetaVariable var;
      const auto vardef = pv.value();
      if (!ParseMetaValue(vardef, &var)) {
        PUSH_ERROR("Failed to parse Prim meta value.");
        return nonstd::nullopt;
      }
      var.set_name(varname);

      return std::make_pair(qual, var);
    } else {
      // Unregistered Prim metadata without the `custom` qualifier. OpenUSD
      // accepts metadata it does not know, so erroring here rejects an
      // otherwise valid prim. Consume the opinion (line-oriented, like the
      // `custom` path above) and carry on.
      if (!Expect('=')) {
        PUSH_ERROR(fmt::format(
            "'=' expected after unregistered Prim metadata {}", varname));
        return nonstd::nullopt;
      }
      if (!SkipWhitespace()) {
        return nonstd::nullopt;
      }
      std::string content;
      if (!ReadUntilNewline(&content)) {
        PUSH_ERROR(fmt::format(
            "Failed to parse unregistered Prim metadata {}", varname));
        return nonstd::nullopt;
      }
      DCOUT("Consumed unregistered Prim metadata: " << varname);
      MetaVariable var;
      var.set_value(varname, content);
      return std::make_pair(qual, var);
    }
  }
}

bool AsciiParser::ParsePrimMetas(PrimMetaMap *args) {
  // '(' args ')'
  // args = list of argument, separated by newline.

  if (!Expect('(')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    // std::cout << "skip comment/whitespace/nl failed\n";
    DCOUT("SkipCommentAndWhitespaceAndNewline failed.");
    return false;
  }

  while (!Eof()) {
    if (!SkipCommentAndWhitespaceAndNewline()) {
      // std::cout << "2: skip comment/whitespace/nl failed\n";
      return false;
    }

    char s;
    if (!Char1(&s)) {
      return false;
    }

    if (s == ')') {
      DCOUT("Prim meta end");
      // End
      break;
    }

    Rewind(1);

    DCOUT("Start PrimMeta parse.");

    // ty = std::pair<ListEditQual, MetaVariable>;
    if (auto m = ParsePrimMeta()) {
      DCOUT("PrimMeta: list-edit qual = "
            << lightusd::to_string(std::get<0>(m.value()))
            << ", name = " << std::get<1>(m.value()).get_name());

      if (std::get<1>(m.value()).get_name().empty()) {
        PUSH_ERROR_AND_RETURN("[InternalError] Metadataum name is empty.");
      }

      constexpr size_t kMaxMetaEntries = 100000; // 100K entries max
      if (args->size() >= kMaxMetaEntries) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii, fmt::format("Metadata entry count exceeds limit ({}).", kMaxMetaEntries));
      }

      // Use insert/emplace for multimap (supports multiple listops per arc)
      args->emplace(std::get<1>(m.value()).get_name(), m.value());
    } else {
      PUSH_ERROR_AND_RETURN("Failed to parse Meta value.");
    }
  }

  return true;
}

bool AsciiParser::ParseAttrMeta(AttrMeta *out_meta) {
  // '(' metas ')'
  //
  // currently we only support 'interpolation', 'elementSize' and 'cutomData'

  if (!SkipWhitespace()) {
    return false;
  }

  // The first character.
  {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '(') {
      // ok
    } else {
      _sr->seek_from_current(-1);

      // Still ok. No meta
      DCOUT("No attribute meta.");
      return true;
    }
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == ')') {
      // end meta
      break;
    } else {
      if (!Rewind(1)) {
        return false;
      }

      // May be string only
      {
        constexpr size_t kMaxStringDataEntries = 100000;
        value::StringData sdata;
        if (MaybeTripleQuotedString(&sdata)) {
          CHECK_MEMORY_USAGE(sizeof(value::StringData) + sdata.value.length());
          if (out_meta->stringData.size() >= kMaxStringDataEntries) {
            PUSH_ERROR_AND_RETURN_TAG(kAscii, fmt::format("Attribute meta string count exceeds limit ({}).", kMaxStringDataEntries));
          }
          out_meta->stringData.push_back(sdata);

          DCOUT("Add triple-quoted string to attr meta:" << to_string(sdata));
          if (!SkipWhitespaceAndNewline()) {
            return false;
          }
          continue;
        } else if (MaybeString(&sdata)) {
          CHECK_MEMORY_USAGE(sizeof(value::StringData) + sdata.value.length());
          if (out_meta->stringData.size() >= kMaxStringDataEntries) {
            PUSH_ERROR_AND_RETURN_TAG(kAscii, fmt::format("Attribute meta string count exceeds limit ({}).", kMaxStringDataEntries));
          }
          out_meta->stringData.push_back(sdata);

          DCOUT("Add string to attr meta:" << to_string(sdata));
          if (!SkipWhitespaceAndNewline()) {
            return false;
          }
          continue;
        }
      }

      std::string varname;
      if (!ReadIdentifier(&varname)) {
        return false;
      }

      DCOUT("Property/Attribute meta name: " << varname);

      bool supported = _supported_prop_metas.count(varname);
      if (!supported) {
        // Unregistered property metadata (attribute or relationship). OpenUSD
        // accepts metadata it does not know; rejecting made an otherwise valid
        // property unloadable. Preserve the opinion verbatim (raw USDA text of
        // the value), matching the unregistered Prim/Stage metadata paths.
        if (!Expect('=')) {
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii,
              fmt::format("'=' expected after unregistered Property metadatum "
                          "'{}'.", varname));
        }
        if (!SkipWhitespace()) {
          return false;
        }
        std::string content;
        if (!ReadUntilNewline(&content)) {
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii,
              fmt::format("Failed to parse unregistered Property metadatum "
                          "'{}'.", varname));
        }
        DCOUT("Preserved unregistered Property metadatum: " << varname);
        out_meta->unregisteredMetas[varname] = content;
        if (!SkipCommentAndWhitespaceAndNewline()) {
          return false;
        }
        continue;
      }

      {
        std::string name_err;
        if (!pathutil::ValidatePropPath(Path("", varname), &name_err)) {
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii,
              fmt::format("Invalid Property name `{}`: {}", varname, name_err));
        }
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }

      if (!Expect('=')) {
        return false;
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }

      //
      // First-class predefind prop metas.
      //
      if (varname == "interpolation") {
        std::string value;
        if (!ReadStringLiteral(&value)) {
          return false;
        }

        DCOUT("Got `interpolation` meta : " << value);
        out_meta->set_interpolation(value);
      } else if (varname == "elementSize") {
        uint32_t value;
        if (!ReadBasicType(&value)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `elementSize`");
        }

        DCOUT("Got `elementSize` meta : " << value);
        out_meta->set_elementSize(value);
      } else if (varname == "colorSpace") {
        value::token tok;
        if (!ReadBasicType(&tok)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `colorSpace`");
        }
        // Add as custom meta value.
        MetaVariable metavar;
        metavar.set_value("colorSpace", tok);
        out_meta->data()["colorSpace"] = metavar;
      } else if (varname == "unauthoredValuesIndex") {
        int value;
        if (!ReadBasicType(&value)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `unauthoredValuesIndex`");
        }

        DCOUT("Got `unauthoredValuesIndex` meta : " << value);
        out_meta->set_unauthoredValuesIndex(value);
      } else if (varname == "customData") {
        Dictionary dict;

        if (!ParseDict(&dict)) {
          return false;
        }

        DCOUT("Got `customData` meta");
        out_meta->set_customData(dict);

      } else if (varname == "weight") {
        double value;
        if (!ReadBasicType(&value)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `weight`");
        }

        DCOUT("Got `weight` meta : " << value);
        out_meta->set_weight(value);
      } else if (varname == "bindMaterialAs") {
        value::token tok;
        if (!ReadBasicType(&tok)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `bindMaterialAs`");
        }
        if ((tok.str() == kWeakerThanDescendants) ||
            (tok.str() == kStrongerThanDescendants)) {
          // ok
        } else {
          // still valid though
          PUSH_WARN("Unsupported token for bindMaterialAs: " << tok.str());
        }
        DCOUT("bindMaterialAs: " << tok);
        out_meta->set_bindMaterialAs(tok);
      } else if (varname == "displayName") {
        std::string str;
        if (!ReadStringLiteral(&str)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `displayName`(string type)");
        }
        DCOUT("displayName: " << str);
        out_meta->set_displayName(str);
      } else if (varname == "doc" || varname == "documentation") {
        // `doc` is the USDA shorthand for `documentation`. Both must end up
        // under MetadataBase::kDoc so the writer's `has_doc()` check finds
        // them and emits a non-empty `documentation` field on USDC.
        std::string str;
        if (!ReadStringLiteral(&str)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `documentation`(string type)");
        }
        DCOUT("documentation: " << str);
        out_meta->set_doc(str);
      } else if (varname == "displayGroup") {
        std::string str;
        if (!ReadStringLiteral(&str)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `displayGroup`(string type)");
        }
        DCOUT("displayGroup: " << str);
        out_meta->set_displayGroup(str);

      } else if (varname == "connectability") {
        value::token tok;
        if (!ReadBasicType(&tok)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `connectability`");
        }
        DCOUT("connectability: " << tok);
        out_meta->set_connectability(tok);
      } else if (varname == "renderType") {
        value::token tok;
        if (!ReadBasicType(&tok)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `renderType`");
        }
        DCOUT("renderType: " << tok);
        out_meta->set_renderType(tok);
      } else if (varname == "outputName") {
        value::token tok;
        if (!ReadBasicType(&tok)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `outputName`");
        }
        DCOUT("outputName: " << tok);
        out_meta->set_outputName(tok);
      } else if (varname == "sdrMetadata") {
        Dictionary dict;

        if (!ParseDict(&dict)) {
          return false;
        }

        out_meta->set_sdrMetadata(dict);
      } else {
        if (auto pv = GetPropMetaDefinition(varname)) {
          // Parse as generic metadata variable
          MetaVariable metavar;
          const auto &vardef = pv.value();

          if (!ParseMetaValue(vardef, &metavar)) {
            return false;
          }
          metavar.set_name(varname);

          // add to custom meta
          out_meta->data()[varname] = metavar;

        } else {
          // This should not happen though.
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii,
              fmt::format(
                  "[InternalErrror] Failed to parse Property metadataum `{}`",
                  varname));
        }
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }
    }
  }

  return true;
}

bool IsUSDA(const std::string &filename, size_t max_filesize) {
  // TODO: Read only first N bytes
  std::vector<uint8_t> data;
  std::string err;

  if (!io::ReadWholeFile(&data, &err, filename, max_filesize)) {
    return false;
  }

  lightusd::StreamReader sr(data.data(), data.size(), /* swap endian */ false);
  lightusd::ascii::AsciiParser parser(&sr);

  return parser.CheckHeader();
}

//
// -- Impl
//

///
/// Parse `rel`
///
bool AsciiParser::ParseRelationship(Relationship *result) {
  char c;
  if (!LookChar1(&c)) {
    return false;
  }

  if (c == '<') {
    // Path
    Path value;
    if (!ReadBasicType(&value)) {
      PUSH_ERROR_AND_RETURN("Failed to parse Path.");
    }

    // Resolve relative path here.
    // NOTE: Internally, USD(Crate) does not allow relative path.
    Path base_prim_path(GetCurrentPrimPath(), "");
    Path abs_path;
    std::string err;
    if (!pathutil::ResolveRelativePath(base_prim_path, value, &abs_path,
                                       &err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Invalid relative Path: {}. error = {}", value, err));
    }

    result->set(abs_path);
  } else if (c == '[') {
    // PathVector
    std::vector<Path> values;
    if (!ParseBasicTypeArray(&values)) {
      PUSH_ERROR_AND_RETURN("Failed to parse PathVector.");
    }

    // Resolve relative path here.
    // NOTE: Internally, USD(Crate) does not allow relative path.
    for (size_t i = 0; i < values.size(); i++) {
      Path base_prim_path(GetCurrentPrimPath(), "");
      Path abs_path;
      if (!pathutil::ResolveRelativePath(base_prim_path, values[i],
                                         &abs_path)) {
        PUSH_ERROR_AND_RETURN(fmt::format("Invalid relative Path: {}.",
                                          values[i].full_path_name()));
      }

      // replace
      values[i] = abs_path;
    }

    result->set(values);
  } else if (c == 'N') {
    // None
    nonstd::optional<Path> value;
    if (!ReadBasicType(&value)) {
      PUSH_ERROR_AND_RETURN("Failed to parse None.");
    }

    // Should be empty for None.
    if (value.has_value()) {
      PUSH_ERROR_AND_RETURN("Failed to parse None.");
    }

    DCOUT("Relationship valueblock.");
    result->set_blocked();
  } else {
    PUSH_ERROR_AND_RETURN("Unexpected char \"" + std::to_string(c) +
                          "\" found. Expects Path or PathVector.");
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::ParseArrayEditValue(std::vector<T> *literals,
                                     std::vector<int64_t> *ops) {
  if (!literals || !ops) return false;

  // The `edit` keyword has already been consumed. Parse `[ <op>; ... ]`.
  if (!SkipWhitespace()) return false;
  if (!Expect('[')) {
    PUSH_ERROR_AND_RETURN("Expected `[` after `edit`.");
  }

  // `[ <int64> ]`
  auto parseIndex = [&](int64_t *idx) -> bool {
    if (!SkipWhitespace()) return false;
    if (!Expect('[')) return false;
    if (!SkipWhitespace()) return false;
    if (!ReadBasicType(idx)) return false;
    if (!SkipWhitespace()) return false;
    if (!Expect(']')) return false;
    return true;
  };

  // Read a literal element of type T, append it, return its literal index.
  auto addLiteral = [&](int64_t *out_idx) -> bool {
    if (!SkipWhitespace()) return false;
    T v;
    if (!ReadBasicType(&v)) return false;
    literals->push_back(v);
    *out_idx = static_cast<int64_t>(literals->size()) - 1;
    return true;
  };

  // Expect a bare keyword (`to` / `at`).
  auto expectWord = [&](const char *w) -> bool {
    if (!SkipWhitespace()) return false;
    std::string kw;
    if (!ReadIdentifier(&kw)) return false;
    return kw == std::string(w);
  };

  // True iff the next non-whitespace char starts an index ref `[`.
  auto nextIsIndex = [&]() -> bool {
    if (!SkipWhitespace()) return false;
    char c;
    return LookChar1(&c) && (c == '[');
  };

  auto emit = [&](value::ArrayEditOp op, int64_t a1, bool hasA2, int64_t a2) {
    ops->push_back(value::ArrayEditPackOpWord(op, 1));
    ops->push_back(a1);
    if (hasA2) ops->push_back(a2);
  };

  while (true) {
    // Ops may span multiple lines and be separated by `;` and/or newlines.
    if (!SkipCommentAndWhitespaceAndNewline(/* allow_semicolon */ true)) {
      return false;
    }
    char c;
    if (!LookChar1(&c)) {
      PUSH_ERROR_AND_RETURN("Unexpected EOF in array edit value.");
    }
    if (c == ']') {
      Char1(&c);
      break;
    }
    if (c == ',') {  // optional comma separator
      Char1(&c);
      continue;
    }

    std::string kw;
    if (!ReadIdentifier(&kw)) {
      PUSH_ERROR_AND_RETURN("Expected array edit op keyword.");
    }

    if (kw == "write") {
      if (nextIsIndex()) {
        int64_t a1, a2;
        if (!parseIndex(&a1)) return false;
        if (!expectWord("to"))
          PUSH_ERROR_AND_RETURN("Expected `to` in array edit `write`.");
        if (!parseIndex(&a2)) return false;
        emit(value::ArrayEditOp::WriteRef, a1, true, a2);
      } else {
        int64_t li, a2;
        if (!addLiteral(&li)) return false;
        if (!expectWord("to"))
          PUSH_ERROR_AND_RETURN("Expected `to` in array edit `write`.");
        if (!parseIndex(&a2)) return false;
        emit(value::ArrayEditOp::WriteLiteral, li, true, a2);
      }
    } else if (kw == "insert") {
      if (nextIsIndex()) {
        int64_t a1, a2;
        if (!parseIndex(&a1)) return false;
        if (!expectWord("at"))
          PUSH_ERROR_AND_RETURN("Expected `at` in array edit `insert`.");
        if (!parseIndex(&a2)) return false;
        emit(value::ArrayEditOp::InsertRef, a1, true, a2);
      } else {
        int64_t li, a2;
        if (!addLiteral(&li)) return false;
        if (!expectWord("at"))
          PUSH_ERROR_AND_RETURN("Expected `at` in array edit `insert`.");
        if (!parseIndex(&a2)) return false;
        emit(value::ArrayEditOp::InsertLiteral, li, true, a2);
      }
    } else if (kw == "prepend") {
      if (nextIsIndex()) {
        int64_t a1;
        if (!parseIndex(&a1)) return false;
        emit(value::ArrayEditOp::InsertRef, a1, true, 0);
      } else {
        int64_t li;
        if (!addLiteral(&li)) return false;
        emit(value::ArrayEditOp::InsertLiteral, li, true, 0);
      }
    } else if (kw == "append") {
      if (nextIsIndex()) {
        int64_t a1;
        if (!parseIndex(&a1)) return false;
        emit(value::ArrayEditOp::InsertRef, a1, true, value::kArrayEditEndIndex);
      } else {
        int64_t li;
        if (!addLiteral(&li)) return false;
        emit(value::ArrayEditOp::InsertLiteral, li, true,
             value::kArrayEditEndIndex);
      }
    } else if (kw == "erase") {
      int64_t a1;
      if (!parseIndex(&a1)) return false;
      emit(value::ArrayEditOp::EraseRef, a1, false, 0);
    } else if (kw == "minsize" || kw == "resize") {
      if (!SkipWhitespace()) return false;
      int64_t n;
      if (!ReadBasicType(&n)) return false;
      // Optional `fill <literal>`.
      bool hasFill = false;
      int64_t li = 0;
      {
        auto loc = CurrLoc();
        char fc;
        if (SkipWhitespace() && LookChar1(&fc) &&
            std::isalpha(static_cast<unsigned char>(fc))) {
          std::string fkw;
          if (ReadIdentifier(&fkw) && (fkw == "fill")) {
            if (!addLiteral(&li)) return false;
            hasFill = true;
          } else {
            SeekTo(loc);
          }
        } else {
          SeekTo(loc);
        }
      }
      value::ArrayEditOp op =
          (kw == "minsize")
              ? (hasFill ? value::ArrayEditOp::MinSizeFill
                         : value::ArrayEditOp::MinSize)
              : (hasFill ? value::ArrayEditOp::SetSizeFill
                         : value::ArrayEditOp::SetSize);
      emit(op, n, hasFill, li);
    } else if (kw == "maxsize") {
      if (!SkipWhitespace()) return false;
      int64_t n;
      if (!ReadBasicType(&n)) return false;
      emit(value::ArrayEditOp::MaxSize, n, false, 0);
    } else {
      PUSH_ERROR_AND_RETURN("Unknown array edit op: " + kw);
    }
  }

  return true;
}

template <typename T>
bool AsciiParser::ParseBasicPrimAttr(bool array_qual,
                                     const std::string &primattr_name,
                                     Attribute *out_attr) {
  Attribute attr;
  primvar::PrimVar var;
  bool blocked{false};
  bool anim_blocked{false};
  bool array_edit{false};
  std::vector<T> ae_literals;
  std::vector<int64_t> ae_ops;

  // `AnimationBlock` is a sentinel keyword (like `None`) that may appear in
  // place of a typed value; it blocks animation while letting the default
  // resolve. Detect it before attempting to parse a typed value.
  if (MaybeAnimationBlock()) {
    anim_blocked = true;
  } else if (array_qual) {
    if (MaybeNone()) {
    } else if (MaybeArrayEdit()) {
      // VtArrayEdit value: `edit [ <op>; ... ]` (only for array-typed attrs).
      if (!ParseArrayEditValue<T>(&ae_literals, &ae_ops)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to parse array edit value for `{}` (type {}[])",
            primattr_name, std::string(value::TypeTraits<T>::type_name())));
      }
      array_edit = true;
    } else {
      std::vector<T> value;
      if (!ParseBasicTypeArray(&value)) {
        PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse Primtive Attribute {} type = {}[]", primattr_name,
                              std::string(value::TypeTraits<T>::type_name())));
      }

      // Empty array allowed.
      DCOUT("Got it: primatrr " << primattr_name << ", ty = " + std::string(value::TypeTraits<T>::type_name()) +
            ", sz = " + std::to_string(value.size()));
      var.set_value(value);
    }

  } else {
    nonstd::optional<T> value;
    if (!ReadBasicType(&value)) {
      PUSH_ERROR_AND_RETURN("Failed to parse " +
                            std::string(value::TypeTraits<T>::type_name()));
    }

    if (value) {
      DCOUT("ParseBasicPrimAttr: " << value::TypeTraits<T>::type_name() << " = "
                                   << (*value));

      var.set_value(value.value());

    } else {
      blocked = true;
      // std::cout << "ParseBasicPrimAttr: " <<
      // value::TypeTraits<T>::type_name()
      //           << " = None\n";
    }
  }

  // optional: attribute meta.
  AttrMeta meta;
  if (!ParseAttrMeta(&meta)) {
    PUSH_ERROR_AND_RETURN("Failed to parse Attribute meta.");
  }
  attr.metas() = meta;

  if (array_edit) {
    // VtArrayEdit value. The declared attribute type is the array element type
    // (`int[]` etc.); the element crate type is derived from the literals at
    // write time, so element_type_id is left unset here.
    value::ArrayEdit aedit;
    aedit.literals = value::Value(std::move(ae_literals));
    aedit.ops = std::move(ae_ops);
    attr.set_type_name(value::TypeTraits<T>::type_name() + std::string("[]"));
    attr.set_value(std::move(aedit));
  } else if (anim_blocked) {
    // AnimationBlock sentinel: carried as a normal default value (not a full
    // ValueBlock), so the declared type is retained.
    value::AnimationBlock animval;
    attr.set_value(std::move(animval));
    if (array_qual) {
      attr.set_type_name(value::TypeTraits<T>::type_name() + "[]");
    } else {
      attr.set_type_name(value::TypeTraits<T>::type_name());
    }
  } else if (blocked) {
    // There is still have a type for ValueBlock.
    value::ValueBlock noneval;
    attr.set_value(std::move(noneval));
    attr.set_blocked(true);
    if (array_qual) {
      attr.set_type_name(value::TypeTraits<T>::type_name() + "[]");
    } else {
      attr.set_type_name(value::TypeTraits<T>::type_name());
    }
  } else {
    attr.set_var(std::move(var));
  }

  (*out_attr) = std::move(attr);

  return true;
}

bool AsciiParser::ParseSplineValue(const std::string &type_name,
                                   primvar::PrimVar::SplineData *out) {
  if (!out) {
    return false;
  }
  if (type_name != value::kDouble && type_name != value::kFloat &&
      type_name != value::kHalf) {
    PUSH_ERROR_AND_RETURN(
        "Spline value type must be double, float or half, but got: " +
        type_name);
  }

  // Wrap a parsed numeric literal in a value::Value of the attribute type.
  auto makeVal = [&](double d) -> value::Value {
    if (type_name == value::kFloat) {
      return value::Value(static_cast<float>(d));
    }
    if (type_name == value::kHalf) {
      return value::Value(value::float_to_half_full(static_cast<float>(d)));
    }
    return value::Value(d);
  };

  if (!SkipWhitespace()) return false;
  if (!Expect('{')) {
    PUSH_ERROR_AND_RETURN("Expected `{` for spline value.");
  }

  // Optional tangent algorithm keyword: none|custom|autoEase
  // (OpenUSD TsTangentAlgorithm: 0=None, 1=Custom, 2=AutoEase).
  auto parseTangentAlgo = [&](int *algo) -> bool {
    std::string kw;
    if (!ReadIdentifier(&kw)) return false;
    if (kw == "none") {
      *algo = 0;
    } else if (kw == "custom") {
      *algo = 1;
    } else if (kw == "autoEase") {
      *algo = 2;
    } else {
      PUSH_ERROR_AND_RETURN("Unknown spline tangent algorithm: " + kw);
    }
    return true;
  };

  // `( <width>, <slope> [, <algo>] )` (bezier) or `( <slope> [, <algo>] )`
  // (hermite). Detected by the presence of a comma; a trailing identifier
  // after a comma is the tangent algorithm (crate 0.13.0 feature).
  auto parseTangent = [&](double *width, double *slope, int *algo) -> bool {
    *algo = 0;
    if (!SkipWhitespace()) return false;
    if (!Expect('(')) return false;
    if (!SkipWhitespace()) return false;
    double a;
    if (!ReadBasicType(&a)) return false;
    if (!SkipWhitespace()) return false;
    char c;
    if (LookChar1(&c) && c == ',') {
      Char1(&c);
      if (!SkipWhitespace()) return false;
      char nc;
      if (LookChar1(&nc) &&
          (std::isalpha(static_cast<unsigned char>(nc)) || nc == '_')) {
        // hermite with algorithm: `( <slope>, <algo> )`
        *width = 1.0;
        *slope = a;
        if (!parseTangentAlgo(algo)) return false;
      } else {
        // bezier: `( <width>, <slope> [, <algo>] )`
        double b;
        if (!ReadBasicType(&b)) return false;
        *width = a;
        *slope = b;
        if (!SkipWhitespace()) return false;
        char c2;
        if (LookChar1(&c2) && c2 == ',') {
          Char1(&c2);
          if (!SkipWhitespace()) return false;
          if (!parseTangentAlgo(algo)) return false;
        }
      }
    } else {
      *width = 1.0;  // hermite: width is implied
      *slope = a;
    }
    if (!SkipWhitespace()) return false;
    if (!Expect(')')) return false;
    return true;
  };

  // none|held|linear|sloped(x)|loop [repeat|reset|oscillate]
  auto parseExtrap = [&](int *mode, double *slope) -> bool {
    if (!SkipWhitespace()) return false;
    std::string kw;
    if (!ReadIdentifier(&kw)) return false;
    *slope = 0.0;
    if (kw == "none") {
      *mode = 0;
    } else if (kw == "held") {
      *mode = 1;
    } else if (kw == "linear") {
      *mode = 2;
    } else if (kw == "sloped") {
      *mode = 3;
      if (!SkipWhitespace()) return false;
      if (!Expect('(')) return false;
      if (!SkipWhitespace()) return false;
      if (!ReadBasicType(slope)) return false;
      if (!SkipWhitespace()) return false;
      if (!Expect(')')) return false;
    } else if (kw == "loop") {
      *mode = 4;  // LoopRepeat
      if (!SkipWhitespace()) return false;
      char c;
      if (LookChar1(&c) && (std::isalpha(static_cast<unsigned char>(c)))) {
        std::string sub;
        if (!ReadIdentifier(&sub)) return false;
        if (sub == "repeat") {
          *mode = 4;
        } else if (sub == "reset") {
          *mode = 5;
        } else if (sub == "oscillate") {
          *mode = 6;
        }
      }
    } else {
      PUSH_ERROR_AND_RETURN("Unknown spline extrapolation mode: " + kw);
    }
    return true;
  };

  while (true) {
    if (!SkipCommentAndWhitespaceAndNewline()) return false;
    char c;
    if (!LookChar1(&c)) {
      PUSH_ERROR_AND_RETURN("Unexpected EOF in spline value.");
    }
    if (c == '}') {
      Char1(&c);
      break;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::string ident;
      if (!ReadIdentifier(&ident)) return false;
      if (ident == "bezier") {
        out->curveType = 0;
      } else if (ident == "hermite") {
        out->curveType = 1;
      } else if (ident == "pre" || ident == "post") {
        if (!SkipWhitespace()) return false;
        if (!Expect(':')) {
          PUSH_ERROR_AND_RETURN("Expected `:` after spline `" + ident + "`.");
        }
        int mode = 1;
        double slope = 0.0;
        if (!parseExtrap(&mode, &slope)) return false;
        if (ident == "pre") {
          out->preExtrapolation = mode;
          out->preExtrapolationSlope = slope;
        } else {
          out->postExtrapolation = mode;
          out->postExtrapolationSlope = slope;
        }
      } else if (ident == "loop") {
        if (!SkipWhitespace()) return false;
        if (!Expect(':')) {
          PUSH_ERROR_AND_RETURN("Expected `:` after spline `loop`.");
        }
        if (!SkipWhitespace()) return false;
        if (!Expect('(')) {
          PUSH_ERROR_AND_RETURN("Expected `(` for spline loop params.");
        }
        // (protoStart, protoEnd, numPreLoops, numPostLoops, valueOffset)
        double vals[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < 5; i++) {
          if (!SkipWhitespace()) return false;
          if (!ReadBasicType(&vals[i])) return false;
          if (!SkipWhitespace()) return false;
          char sc;
          if (i < 4) {
            if (!Expect(',')) {
              PUSH_ERROR_AND_RETURN("Expected `,` in spline loop params.");
            }
          } else if (LookChar1(&sc) && sc == ')') {
            Char1(&sc);
          }
        }
        out->hasLoop = true;
        out->loopProtoStart = vals[0];
        out->loopProtoEnd = vals[1];
        out->loopNumPreLoops = static_cast<int>(vals[2]);
        out->loopNumPostLoops = static_cast<int>(vals[3]);
        out->loopValueOffset = vals[4];
      } else {
        PUSH_ERROR_AND_RETURN("Unknown spline keyword: " + ident);
      }
    } else {
      // Knot: <time> : <value> [& <postValue>] [; <specs>] [{ customData }]
      primvar::PrimVar::SplineKnotData kd;
      double time;
      if (!ReadBasicType(&time)) return false;
      kd.time = time;
      if (!SkipWhitespace()) return false;
      if (!Expect(':')) {
        PUSH_ERROR_AND_RETURN("Expected `:` in spline knot.");
      }
      if (!SkipWhitespace()) return false;
      double v0;
      if (!ReadBasicType(&v0)) return false;
      if (!SkipWhitespace()) return false;
      char amp;
      if (LookChar1(&amp) && amp == '&') {
        // Dual-valued knot: `<preValue> & <value>`.
        Char1(&amp);
        if (!SkipWhitespace()) return false;
        double v1;
        if (!ReadBasicType(&v1)) return false;
        kd.preValue = makeVal(v0);
        kd.val = makeVal(v1);
        kd.hasDualValue = true;
      } else {
        kd.val = makeVal(v0);
        kd.preValue = kd.val;
      }

      // Optional `;`-separated tangent / interpolation specs on the same line.
      while (true) {
        if (!SkipWhitespace()) return false;
        char pc;
        if (!LookChar1(&pc) || pc != ';') {
          break;
        }
        Char1(&pc);  // consume ';'
        if (!SkipWhitespace()) return false;
        if (LookChar1(&pc) && pc == '{') {
          // Per-knot customData dictionary: consume (not retained).
          int depth = 0;
          do {
            char cc;
            if (!Char1(&cc)) {
              PUSH_ERROR_AND_RETURN("Unterminated spline knot customData.");
            }
            if (cc == '{') depth++;
            else if (cc == '}') depth--;
          } while (depth > 0);
          continue;
        }
        std::string spec;
        if (!ReadIdentifier(&spec)) return false;
        if (spec == "pre") {
          if (!parseTangent(&kd.preTangentWidth, &kd.preTangentSlope,
                            &kd.preTangentAlgorithm)) {
            return false;
          }
        } else if (spec == "post") {
          if (!SkipWhitespace()) return false;
          std::string interp;
          if (!ReadIdentifier(&interp)) return false;
          if (interp == "none") {
            kd.interpolationMode = 0;
          } else if (interp == "held") {
            kd.interpolationMode = 1;
          } else if (interp == "linear") {
            kd.interpolationMode = 2;
          } else if (interp == "curve") {
            kd.interpolationMode = 3;
            if (!parseTangent(&kd.postTangentWidth, &kd.postTangentSlope,
                              &kd.postTangentAlgorithm)) {
              return false;
            }
          } else {
            PUSH_ERROR_AND_RETURN("Unknown spline interpolation: " + interp);
          }
        } else {
          PUSH_ERROR_AND_RETURN("Unknown spline knot spec: " + spec);
        }
      }

      out->knots.push_back(kd);
    }

    // Optional `,` entry separator.
    if (!SkipWhitespace()) return false;
    char sep;
    if (LookChar1(&sep) && sep == ',') {
      Char1(&sep);
    }
  }

  return true;
}

bool AsciiParser::ParsePrimProps(std::map<std::string, Property> *props,
                                 std::vector<value::token> *propNames) {
  (void)propNames;

  // prim_prop : (custom?) (variability?) type (array_qual?) name '=' value
  //           | (custom?) type (array_qual?) name '=' value interpolation?
  //           | (custom?) (variability?) type (array_qual?) name interpolation?
  //           | (custom?) (listeditqual?) (variability?) rel attr_name = None
  //           | (custom?) (listeditqual?) (variability?) rel attr_name = string
  //           meta | (custom?) (listeditqual?) (variability?) rel attr_name =
  //           path meta | (custom?) (listeditqual?) (variability?) rel
  //           attr_name = pathvector meta | (custom?) (listeditqual?)
  //           (variability?) rel attr_name meta
  //           ;

  // NOTE:
  //  custom append varying ... is not allowed.
  //  append varying custom ... is not allowed.
  //  append custom varying ... is allowed(decomposed into `custom varying ...`
  //  and `append varying ...`

  // Skip comment
  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  // Parse `custom`
  bool custom_qual = MaybeCustom();

  if (!SkipWhitespace()) {
    return false;
  }

  Cursor property_cursor = _curr_cursor;
  const auto record_prim_attr_and_property = [&](const std::string &property_name) {
    const std::string prim_path = GetCurrentPrimPath();
    RecordPrimAttrCursor(prim_path, property_name, property_cursor);
    RecordPropertyCursor(prim_path, property_name, property_cursor);
  };

  ListEditQual listop_qual;
  if (!MaybeListEditQual(&listop_qual)) {
    return false;
  }

  // `custom` then listop is not allowed.
  if (listop_qual != ListEditQual::ResetToExplicit) {
    if (custom_qual) {
      PUSH_ERROR_AND_RETURN("`custom` then ListEdit qualifier is not allowed.");
    }

    // listop then `custom` is allowed.
    custom_qual = MaybeCustom();
  }

  bool varying_authored{false};
  lightusd::Variability variability{lightusd::Variability::Varying};

  if (!MaybeVariability(&variability, &varying_authored)) {
    return false;
  }
  DCOUT("variability = " << to_string(variability) << ", varying_authored "
                         << varying_authored);

  std::string type_name;

  if (!ReadIdentifier(&type_name)) {
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  DCOUT("type_name = " << type_name);

  // `reorder nameChildren = [...]` / `reorder properties = [...]` (also
  // variantSets, etc.). These reorder the namespace output ordering and carry
  // no data opinions, so parse and consume the statement as a no-op rather than
  // mis-reading `reorder` as a typed-attribute declaration and failing.
  if (type_name == "reorder") {
    std::string reorder_field;
    if (!ReadIdentifier(&reorder_field)) {
      PUSH_ERROR_AND_RETURN(
          "Expected a field name (e.g. `nameChildren`/`properties`) after "
          "`reorder`.");
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
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to parse token array for `reorder {}`.",
                      reorder_field));
    }
    DCOUT("Parsed `reorder " << reorder_field << "` ("
          << reorder_toks.size() << " entries; preserved for round-trip).");
    _pending_reorders.emplace_back(reorder_field, std::move(reorder_toks));
    return true;
  }

  // `uniform` or `varying`

  // Relation('rel')
  if (type_name == kRel) {
    DCOUT("relation");

    if (variability == Variability::Uniform) {
      PUSH_ERROR_AND_RETURN(
          "Explicit `uniform` variability keyword is not allowed for "
          "Relationship.");
    }

    // - prim_identifier
    // - prim_identifier, '(' metadataum ')'
    // - prim_identifier, '=', (None|string|path|pathvector)
    // NOTE: There should be no 'uniform rel'

    std::string attr_name;

    if (!ReadPrimAttrIdentifier(&attr_name)) {
      PUSH_ERROR_AND_RETURN(
          "Attribute name(Identifier) expected but got non-identifier.");
    }

    if (!SkipWhitespace()) {
      return false;
    }

    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    nonstd::optional<AttrMeta> metap;

    if (c == '(') {
      // FIXME: Implement Relation specific metadatum parser?
      AttrMeta meta;
      if (!ParseAttrMeta(&meta)) {
        PUSH_ERROR_AND_RETURN("Failed to parse metadataum.");
      }

      metap = meta;

      if (!LookChar1(&c)) {
        return false;
      }
    }

    if (c != '=') {
      DCOUT("Relationship with no target: " << attr_name);

      // No targets. Define only.
      Property p;
      p.set_property_type(Property::Type::NoTargetsRelation);
      p.set_listedit_qual(listop_qual);
      // Keep the inner Relationship's list-edit qualifier in sync (see note
      // in the has-targets branch below).
      p.relationship().set_listedit_qual(listop_qual);

      if (varying_authored) {
        p.relationship().set_varying_authored();
      }

      if (metap) {
        // TODO: metadataum for Rel
        p.relationship().metas() = metap.value();
      }

      (*props)[attr_name] = p;

      return true;
    }

    // has targets
    if (!Expect('=')) {
      return false;
    }

    if (metap) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii,
          "Syntax error. Property metadatum must be defined after `=` and "
          "relationship target(s).");
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    Relationship rel;
    if (!ParseRelationship(&rel)) {
      PUSH_ERROR_AND_RETURN("Failed to parse `rel` property.");
    }

    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    if (!LookChar1(&c)) {
      return false;
    }

    if (c == '(') {
      if (metap) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii, "[InternalError] parser error.");
      }

      AttrMeta meta;

      // FIXME: Implement Relation specific metadatum parser?
      if (!ParseAttrMeta(&meta)) {
        PUSH_ERROR_AND_RETURN("Failed to parse metadataum.");
      }

      metap = meta;
    }

    DCOUT("Relationship with target: " << attr_name);
    Property p(rel, custom_qual);
    p.set_listedit_qual(listop_qual);
    // Keep the inner Relationship's list-edit qualifier in sync with the
    // property-level one. ParseRelationship() leaves it at the default
    // (ResetToExplicit); without this the Crate reader (which sets both) and
    // the USDA reader disagree, producing spurious diffs on round-trip.
    p.relationship().set_listedit_qual(listop_qual);

    if (varying_authored) {
      p.relationship().set_varying_authored();
    }

    if (metap) {
      p.relationship().metas() = metap.value();
    }

    (*props)[attr_name] = p;

    return true;
  }

  //
  // Attrib.
  //

  // Attribute cannot have 'varying' keyword
  if (varying_authored) {
    PUSH_ERROR_AND_RETURN_TAG(
        kAscii, "Syntax error. `varying` keyword is not allowed for Attribute.");
  }

  if (listop_qual != ListEditQual::ResetToExplicit) {
    PUSH_ERROR_AND_RETURN_TAG(
        kAscii, "List editing qualifier is not allowed for Attribute.");
  }

  if (!IsSupportedPrimAttrType(type_name)) {
    PUSH_ERROR_AND_RETURN("Unknown or unsupported primtive attribute type `" +
                          type_name);
  }

  // Has array qualifier? `[]`
  bool array_qual = false;
  {
    char c0, c1;
    if (!Char1(&c0)) {
      return false;
    }

    if (c0 == '[') {
      if (!Char1(&c1)) {
        return false;
      }

      if (c1 == ']') {
        array_qual = true;
      } else {
        // Invalid syntax
        PUSH_ERROR_AND_RETURN("Invalid syntax found.");
      }

    } else {
      if (!Rewind(1)) {
        return false;
      }
    }
  }

  if (!SkipWhitespace()) {
    return false;
  }

  // Save cursor position before reading attribute name for accurate error reporting
  Cursor attr_name_cursor = _curr_cursor;

  std::string primattr_name;
  if (!ReadPrimAttrIdentifier(&primattr_name)) {
    // Restore cursor to start of attribute name for error reporting
    _curr_cursor = attr_name_cursor;
    PUSH_ERROR_AND_RETURN("Failed to parse primAttr identifier.");
  }

  if (!SkipWhitespace()) {
    return false;
  }

  bool isTimeSample = endsWith(primattr_name, kTimeSamplesSuffix);
  bool isConnection = endsWith(primattr_name, kConnectSuffix);
  bool isSpline = endsWith(primattr_name, kSplineSuffix);

  // Remove suffix
  std::string attr_name = primattr_name;
  if (isTimeSample) {
    attr_name = removeSuffix(primattr_name, kTimeSamplesSuffix);
  }
  if (isConnection) {
    attr_name = removeSuffix(primattr_name, kConnectSuffix);
  }
  if (isSpline) {
    attr_name = removeSuffix(primattr_name, kSplineSuffix);
  }

  bool define_only = false;
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c != '=') {
      // Define only(e.g. output variable)
      define_only = true;
    }
  }

  DCOUT("define only:" << define_only);

  if (define_only) {
    Rewind(1);

    // optional: attribute meta.
    AttrMeta meta;
    if (!ParseAttrMeta(&meta)) {
      PUSH_ERROR_AND_RETURN("Failed to parse Attribute meta.");
    }

    DCOUT("Define only property = " + primattr_name);

    // Empty Attribute. type info only
    Property p;
    p.set_property_type(Property::Type::EmptyAttrib);
    p.set_custom(custom_qual);
    std::string typeName = type_name;
    if (array_qual) {
      typeName += "[]";
    }
    p.attribute().set_type_name(typeName);

    p.attribute().variability() = variability;
    if (varying_authored) {
      p.attribute().set_varying_authored();
    }

    p.attribute().metas() = meta;

    (*props)[attr_name] = p;
    record_prim_attr_and_property(attr_name);

    return true;
  }

  // Continue to parse argument
  if (!SkipWhitespace()) {
    return false;
  }

  bool value_blocked{false};

  if (MaybeNone()) {
    value_blocked = true;
  }

  if (isConnection) {
    // atribute connection
    DCOUT("isConnection");

    // The target is either a single path `</...>`, an array of paths
    // `[</a>, </b>]`, or `None` (value_blocked).
    std::vector<Path> targets;
    if (!value_blocked) {
      char c;
      if (!LookChar1(&c)) {
        return false;
      }
      if (c == '[') {
        std::vector<Path> values;
        if (!ParseBasicTypeArray(&values)) {
          PUSH_ERROR_AND_RETURN(
              "Failed to parse path array for .connect target.");
        }
        Path base_abs_path(GetCurrentPrimPath(), "");
        for (auto &v : values) {
          Path resolved;
          std::string rerr;
          if (!pathutil::ResolveRelativePath(base_abs_path, v, &resolved,
                                             &rerr)) {
            PUSH_ERROR_AND_RETURN(
                fmt::format("Invalid relative Path: {}. error = {}",
                            v.full_path_name(), rerr));
          }
          targets.push_back(resolved);
        }
      } else {
        Path path;
        if (!ReadBasicType(&path)) {
          PUSH_ERROR_AND_RETURN("Path expected for .connect target.");
        }
        Path base_abs_path(GetCurrentPrimPath(), "");
        Path abs_path;
        std::string err;
        if (!pathutil::ResolveRelativePath(base_abs_path, path, &abs_path,
                                           &err)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Invalid relative Path: {}. error = {}",
                                            path.full_path_name(), err));
        }
        targets.push_back(abs_path);
      }
    }
    Path abs_path = targets.empty() ? Path() : targets.front();

    // Check if attribute metadatum is not authored.
    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    if (c == '(') {
      PUSH_ERROR_AND_RETURN(fmt::format("Attribute connection cannot have attribute metadataum: {}", attr_name));
    }

    bool attr_exists = props->count(attr_name) && props->at(attr_name).is_attribute();
    if (attr_exists) {

      // TODO: Check if type is the same.

      // Check if variability is the same
      if (props->at(attr_name).attribute().variability() != variability) {
        PUSH_ERROR_AND_RETURN(fmt::format("Variability mismatch. Attribute `{}` already has variability `{}`, but timeSampled value has variability `{}`.", attr_name, to_string(props->at(attr_name).attribute().variability()), to_string(variability)));
      }

      if (targets.size() > 1) {
        props->at(attr_name).attribute().set_connections(targets);
      } else {
        props->at(attr_name).attribute().set_connection(abs_path);
      }

      // Set PropType to Attrib(since previously created Property may have EmptyAttrib).
      props->at(attr_name).set_property_type(Property::Type::Attrib);
    } else {

      Attribute attr;
      attr.set_type_name(type_name);
      if (targets.size() > 1) {
        attr.set_connections(targets);
      } else {
        attr.set_connection(abs_path);
      }
      attr.variability() = variability;

      //Property p(abs_path, /* value typename */ type_name, custom_qual);

      //p.attribute().variability() = variability;
      //if (varying_authored) {
      //  p.attribute().set_varying_authored();
      //}

      Property p(std::move(attr), custom_qual);
      (*props)[attr_name] = p;
    }

    record_prim_attr_and_property(attr_name);

    DCOUT(fmt::format("Added attribute connection to `{}`", attr_name));

    return true;

  } else if (isTimeSample) {
    // float.timeSamples = None is syntax error.
    if (value_blocked) {
      PUSH_ERROR_AND_RETURN(fmt::format("Syntax error. ValueBlock to .timeSamples is invalid: {}", attr_name));
    }

    //
    // TODO(syoyo): Refactror and implement value parser dispatcher.
    //
    if (array_qual) {
      DCOUT("timeSample data. type = " << type_name << "[]");
    } else {
      DCOUT("timeSample data. type = " << type_name);
    }

    value::TimeSamples ts;
    if (array_qual) {
      if (!ParseTimeSamplesOfArray(type_name, &ts)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse TimeSamples of type {}[]", type_name));
      }
    } else {
      if (!ParseTimeSamples(type_name, &ts)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse TimeSamples of type {}", type_name));
      }
    }

    // Finalize (sort) once, single-threaded, so later const reads are pure and
    // the TimeSamples can be safely shared across threads. See
    // value::TimeSamples::update().
    ts.update();

    // Attribute metadatum is not allowed for timeSamples.
    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    if (c == '(') {
      PUSH_ERROR_AND_RETURN(fmt::format("TimeSampled Attribute cannot have attribute metadataum: {}", attr_name));
    }

    DCOUT("timeSamples primattr: type = " << type_name
                                          << ", name = " << attr_name);

    Attribute attr;
    Attribute *pattr{nullptr};
    bool attr_exists = props->count(attr_name) && props->at(attr_name).is_attribute();
    if (attr_exists) {
      DCOUT("Attr exists");
      // Add timeSamples to existing Attribute
      pattr = &(props->at(attr_name).attribute());

      // Check if variability is the same
      if (pattr->variability() != variability) {
        PUSH_ERROR_AND_RETURN(fmt::format("Variability mismatch. Attribute `{}` already has variability `{}`, but timeSampled value has variability `{}`.", attr_name, to_string(pattr->variability()), to_string(variability)));
      }

      pattr->get_var().set_timesamples(std::move(ts));

      // Set PropType to Attrib(since previously created Property may have EmptyAttrib).
      props->at(attr_name).set_property_type(Property::Type::Attrib);

    } else {
      // new Attribute
      pattr = &attr;

      primvar::PrimVar var;
      var.set_timesamples(std::move(ts));
      if (array_qual) {
        pattr->set_type_name(type_name + "[]");
      } else {
        pattr->set_type_name(type_name);
      }
      pattr->set_var(std::move(var));
      pattr->variability() = variability;

      //if (varying_authored) {
      //  pattr->set_varying_authored();
      //}

      pattr->name() = attr_name;

      Property p(attr, custom_qual);
      p.set_property_type(Property::Type::Attrib);
      (*props)[attr_name] = p;
    }

    record_prim_attr_and_property(attr_name);

    return true;

  } else if (isSpline) {
    // `<type> <attr>.spline = { ... }` (AOUSD Core Spec 7.4.2.4).
    if (value_blocked) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Syntax error. ValueBlock to .spline is invalid: {}", attr_name));
    }
    if (array_qual) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Spline value cannot be an array type: {}", attr_name));
    }

    primvar::PrimVar::SplineData sd;
    if (!ParseSplineValue(type_name, &sd)) {
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, fmt::format("Failed to parse spline value of type {} for {}",
                              type_name, attr_name));
    }

    Attribute attr;
    Attribute *pattr{nullptr};
    bool attr_exists =
        props->count(attr_name) && props->at(attr_name).is_attribute();
    if (attr_exists) {
      pattr = &(props->at(attr_name).attribute());
      pattr->get_var().set_spline(std::move(sd));
      props->at(attr_name).set_property_type(Property::Type::Attrib);
    } else {
      pattr = &attr;
      primvar::PrimVar var;
      var.set_spline(std::move(sd));
      pattr->set_type_name(type_name);
      pattr->set_var(std::move(var));
      pattr->variability() = variability;
      pattr->name() = attr_name;

      Property p(attr, custom_qual);
      p.set_property_type(Property::Type::Attrib);
      (*props)[attr_name] = p;
    }

    record_prim_attr_and_property(attr_name);

    return true;

  } else {

    Attribute _attr;
    Attribute *pattr{nullptr};
    bool attr_exists = props->count(attr_name) && props->at(attr_name).is_attribute();
    DCOUT("attr_exists " << attr_exists);
    if (attr_exists) {
      pattr = &(props->at(attr_name).attribute());
    } else {
      pattr = &_attr;
      pattr->set_name(primattr_name);
    }

    if (!value_blocked) {
      // TODO: Refactor. ParseAttrMeta is currently called inside
      // ParseBasicPrimAttr()
      bool type_matched = false;
      // NOTE: this ~81-branch else-if chain on the USDA property
      // `type_name` string was converted to standalone ifs -- same MSVC
      // C1061 ("blocks nested too deeply") risk class already fixed for
      // the same reason elsewhere in this codebase (crate-writer-values.cc,
      // stage-converter.cc, lusdr_args.cc, lusdcat/main.cc). OPT_MATCH
      // folds the "did anything match yet" guard into the condition itself
      // so the final unsupported-type fallback below only fires when no
      // type matched -- exactly the original else-if chain's semantics --
      // without needing to touch every branch body.
#define OPT_MATCH(cond) if (!type_matched && (cond) && (type_matched = true))
      OPT_MATCH(type_name == value::kBool) {
        if (!ParseBasicPrimAttr<bool>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kChar) {
        if (!ParseBasicPrimAttr<char>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kChar2) {
        if (!ParseBasicPrimAttr<value::char2>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kChar3) {
        if (!ParseBasicPrimAttr<value::char3>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kChar4) {
        if (!ParseBasicPrimAttr<value::char4>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUChar) {
        if (!ParseBasicPrimAttr<uint8_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUChar2) {
        if (!ParseBasicPrimAttr<value::uchar2>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUChar3) {
        if (!ParseBasicPrimAttr<value::uchar3>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUChar4) {
        if (!ParseBasicPrimAttr<value::uchar4>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kShort) {
        if (!ParseBasicPrimAttr<int16_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kShort2) {
        if (!ParseBasicPrimAttr<value::short2>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kShort3) {
        if (!ParseBasicPrimAttr<value::short3>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kShort4) {
        if (!ParseBasicPrimAttr<value::short4>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUShort) {
        if (!ParseBasicPrimAttr<uint16_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUShort2) {
        if (!ParseBasicPrimAttr<value::ushort2>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUShort3) {
        if (!ParseBasicPrimAttr<value::ushort3>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUShort4) {
        if (!ParseBasicPrimAttr<value::ushort4>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kInt) {
        if (!ParseBasicPrimAttr<int>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kInt2) {
        if (!ParseBasicPrimAttr<value::int2>(array_qual, primattr_name,
                                             pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kInt3) {
        if (!ParseBasicPrimAttr<value::int3>(array_qual, primattr_name,
                                             pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kInt4) {
        if (!ParseBasicPrimAttr<value::int4>(array_qual, primattr_name,
                                             pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUInt) {
        if (!ParseBasicPrimAttr<uint32_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUInt2) {
        if (!ParseBasicPrimAttr<value::uint2>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUInt3) {
        if (!ParseBasicPrimAttr<value::uint3>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUInt4) {
        if (!ParseBasicPrimAttr<value::uint4>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kInt64) {
        if (!ParseBasicPrimAttr<int64_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kUInt64) {
        if (!ParseBasicPrimAttr<uint64_t>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kDouble) {
        if (!ParseBasicPrimAttr<double>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kString) {
        if (!ParseBasicPrimAttr<std::string>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kToken) {
        if (!ParseBasicPrimAttr<value::token>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kHalf) {
        if (!ParseBasicPrimAttr<value::half>(array_qual, primattr_name,
                                             pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kHalf2) {
        if (!ParseBasicPrimAttr<value::half2>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kHalf3) {
        if (!ParseBasicPrimAttr<value::half3>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kHalf4) {
        if (!ParseBasicPrimAttr<value::half4>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kFloat) {
        if (!ParseBasicPrimAttr<float>(array_qual, primattr_name, pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kFloat2) {
        if (!ParseBasicPrimAttr<value::float2>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kFloat3) {
        if (!ParseBasicPrimAttr<value::float3>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kFloat4) {
        if (!ParseBasicPrimAttr<value::float4>(array_qual, primattr_name,
                                               pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kDouble2) {
        if (!ParseBasicPrimAttr<value::double2>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kDouble3) {
        if (!ParseBasicPrimAttr<value::double3>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kDouble4) {
        if (!ParseBasicPrimAttr<value::double4>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kQuath) {
        if (!ParseBasicPrimAttr<value::quath>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kQuatf) {
        if (!ParseBasicPrimAttr<value::quatf>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kQuatd) {
        if (!ParseBasicPrimAttr<value::quatd>(array_qual, primattr_name,
                                              pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kPoint3f) {
        if (!ParseBasicPrimAttr<value::point3f>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor3f) {
        if (!ParseBasicPrimAttr<value::color3f>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor4f) {
        if (!ParseBasicPrimAttr<value::color4f>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kPoint3d) {
        if (!ParseBasicPrimAttr<value::point3d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kNormal3f) {
        if (!ParseBasicPrimAttr<value::normal3f>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kNormal3d) {
        if (!ParseBasicPrimAttr<value::normal3d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kVector3f) {
        if (!ParseBasicPrimAttr<value::vector3f>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kVector3d) {
        if (!ParseBasicPrimAttr<value::vector3d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor3d) {
        if (!ParseBasicPrimAttr<value::color3d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor4d) {
        if (!ParseBasicPrimAttr<value::color4d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix2f) {
        if (!ParseBasicPrimAttr<value::matrix2f>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix3f) {
        if (!ParseBasicPrimAttr<value::matrix3f>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix4f) {
        if (!ParseBasicPrimAttr<value::matrix4f>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix2d) {
        if (!ParseBasicPrimAttr<value::matrix2d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix3d) {
        if (!ParseBasicPrimAttr<value::matrix3d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kMatrix4d) {
        if (!ParseBasicPrimAttr<value::matrix4d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      // AOUSD Core Spec 6.2: timecode scalar type (parsed as double)
      }
      OPT_MATCH(type_name == value::kTimeCode) {
        if (!ParseBasicPrimAttr<double>(array_qual, primattr_name, pattr)) {
          return false;
        }
        pattr->set_type_name(array_qual ? std::string(value::kTimeCode) + "[]"
                                        : std::string(value::kTimeCode));
      // AOUSD Core Spec 6.5: Semantic aliases - half-precision variants
      }
      OPT_MATCH(type_name == value::kNormal3h) {
        if (!ParseBasicPrimAttr<value::normal3h>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kPoint3h) {
        if (!ParseBasicPrimAttr<value::point3h>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kVector3h) {
        if (!ParseBasicPrimAttr<value::vector3h>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor3h) {
        if (!ParseBasicPrimAttr<value::color3h>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor4h) {
        if (!ParseBasicPrimAttr<value::color4h>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      // AOUSD Core Spec 6.5: Semantic aliases - double-precision variants
      }
      OPT_MATCH(type_name == value::kPoint3d) {
        if (!ParseBasicPrimAttr<value::point3d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kNormal3d) {
        if (!ParseBasicPrimAttr<value::normal3d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kVector3d) {
        if (!ParseBasicPrimAttr<value::vector3d>(array_qual, primattr_name,
                                                 pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor3d) {
        if (!ParseBasicPrimAttr<value::color3d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kColor4d) {
        if (!ParseBasicPrimAttr<value::color4d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      // AOUSD Core Spec 6.5: texCoord variants (all precisions)
      }
      OPT_MATCH(type_name == value::kTexCoord2h) {
        if (!ParseBasicPrimAttr<value::texcoord2h>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kTexCoord2f) {
        if (!ParseBasicPrimAttr<value::texcoord2f>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kTexCoord2d) {
        if (!ParseBasicPrimAttr<value::texcoord2d>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kTexCoord3h) {
        if (!ParseBasicPrimAttr<value::texcoord3h>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kTexCoord3f) {
        if (!ParseBasicPrimAttr<value::texcoord3f>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kTexCoord3d) {
        if (!ParseBasicPrimAttr<value::texcoord3d>(array_qual, primattr_name,
                                                   pattr)) {
          return false;
        }
      // AOUSD Core Spec 6.5: frame4d (semantic alias for matrix4d)
      }
      OPT_MATCH(type_name == value::kFrame4d) {
        if (!ParseBasicPrimAttr<value::frame4d>(array_qual, primattr_name,
                                                pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kAssetPath) {
        if (!ParseBasicPrimAttr<value::AssetPath>(array_qual, primattr_name,
                                                  pattr)) {
          return false;
        }
      }
      OPT_MATCH(type_name == value::kPathExpression) {
        if (!ParseBasicPrimAttr<value::PathExpression>(array_qual,
                                                       primattr_name, pattr)) {
          return false;
        }
      }
#undef OPT_MATCH
      if (!type_matched) {
        PUSH_ERROR_AND_RETURN("Unsupported property attribute type: " + type_name);
      }
    }


    if (varying_authored) {
      pattr->set_varying_authored();
    }

    // TODO: Check if type is the same with existing attribute.
    if (value_blocked) {
      if (array_qual) {
        pattr->set_type_name(type_name + "[]");
      } else {
        pattr->set_type_name(type_name);
      }
      pattr->set_blocked(true);
    }

    DCOUT("primattr: type = " << type_name << ", name = " << primattr_name);
    DCOUT(" value_blocked " << value_blocked);

    if (attr_exists) {
      // Check if variability is the same
      if (pattr->variability() != variability) {
        PUSH_ERROR_AND_RETURN(fmt::format("Variability mismatch. Attribute `{}` already has variability `{}`, but 'default' value has variability `{}`.", attr_name, to_string(pattr->variability()), to_string(variability)));
      }

      // Set PropType to Attrib(since previously created Property may have EmptyAttrib).
      props->at(attr_name).set_property_type(Property::Type::Attrib);
    } else {
      pattr->variability() = variability;
      Property p(*pattr, custom_qual);

      (*props)[primattr_name] = p;
    }

    record_prim_attr_and_property(attr_name);

    return true;
  }
}

// propNames stores list of property name in its appearance order.
bool AsciiParser::ParseProperties(std::map<std::string, Property> *props,
                                  std::vector<value::token> *propNames) {
  // property : primm_attr
  //          | 'rel' name '=' path
  //          ;

  // Report progress and check for cancellation
  if (!ReportProgress()) {
    PUSH_ERROR_AND_RETURN("Parsing cancelled by progress callback.");
  }

  if (!SkipWhitespace()) {
    return false;
  }

  // Relationship or attribute.
  return ParsePrimProps(props, propNames);
}

}  // namespace ascii
}  // namespace lightusd

#endif  // !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)
