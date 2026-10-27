// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// usdMedia prim reconstruction specializations.
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdMedia.hh"

#include "common-macros.inc"
#include "value-types.hh"

#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

namespace lightusd {
namespace prim {

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "prim-reconstruct-common.inc"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

// ============================================================================
// SpatialAudio
// ============================================================================
template <>
bool ReconstructPrim<SpatialAudio>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    SpatialAudio *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    SPATIAL_AUDIO_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, SpatialAudio, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

}  // namespace prim
}  // namespace lightusd
