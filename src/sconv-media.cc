// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// USDC writer: usdMedia prim property extraction
//
#include "sconv-detail.hh"
#include "usdMedia.hh"

namespace lightusd {
namespace experimental {

#define EXTRACT_TYPED(attr, name) do { \
  auto _opt = (attr).get_value(); \
  if (_opt.has_value()) { \
    crate::CrateValue _cv; \
    value::Value _v(_opt.value()); \
    std::string _cerr; \
    if (ConvertValue(_v, _cv, &_cerr)) { \
      fields.push_back({(name), _cv}); \
    } \
  } \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

// NOTE: guarded on authored(). A TypedAttributeWithFallback ALWAYS yields a
// value from get_value() -- the schema fallback when nothing was authored -- so
// writing it unconditionally turns "no opinion" into an AUTHORED opinion on
// read-back. That is not cosmetic: an authored opinion is a STRONG one, and it
// blocks weaker opinions during composition.
#define EXTRACT_FALLBACK(attr, name) do { \
  if (!(attr).authored()) break; \
  crate::CrateValue _cv; \
  value::Value _v((attr).get_value()); \
  std::string _cerr; \
  if (ConvertValue(_v, _cv, &_cerr)) { \
    fields.push_back({(name), _cv}); \
  } \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

#define EXTRACT_TOKEN_FALLBACK(attr, name) do { \
  if (!(attr).authored()) break; \
  crate::CrateValue _cv; \
  _cv.Set((attr).get_value()); \
  fields.push_back({(name), _cv}); \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

bool CrateWriter::ExtractSpatialAudioProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const auto *p = prim.data().as<SpatialAudio>();
  if (!p) { if (err) *err = "Failed to cast to SpatialAudio"; return false; }
  EXTRACT_TYPED(p->filePath, "filePath");
  EXTRACT_TOKEN_FALLBACK(p->auralMode, "auralMode");
  EXTRACT_TOKEN_FALLBACK(p->playbackMode, "playbackMode");
  EXTRACT_TYPED(p->startTime, "startTime");
  EXTRACT_TYPED(p->endTime, "endTime");
  EXTRACT_FALLBACK(p->mediaOffset, "mediaOffset");
  EXTRACT_FALLBACK(p->gain, "gain");
  (void)prim_path;
  return true;
}

}  // namespace experimental
}  // namespace lightusd
