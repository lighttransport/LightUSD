// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// usdMedia prim to_string.
//
#include "pprinter.hh"
#include "pprint-detail.hh"

#include "common-macros.inc"

namespace lightusd {

#define PRINT_PRIM_HEADER(prim, type_name)                                       \
  do {                                                                            \
    ss << pprint::Indent(indent) << to_string(prim.spec) << " " << type_name     \
       << " \"" << prim.name << "\"\n";                                           \
    if (prim.meta.authored()) {                                                   \
      ss << pprint::Indent(indent) << "(\n";                                      \
      ss << print_prim_metas(prim.meta, indent + 1);                              \
      ss << pprint::Indent(indent) << ")\n";                                      \
    }                                                                             \
    ss << pprint::Indent(indent) << "{\n";                                        \
  } while (0)

#define PRINT_PRIM_FOOTER(prim)                                                   \
  do {                                                                            \
    ss << print_props(prim.props, indent + 1);                                    \
    if (closing_brace) {                                                          \
      ss << pprint::Indent(indent) << "}\n";                                      \
    }                                                                             \
  } while (0)

std::string to_string(const SpatialAudio &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "SpatialAudio");
  ss << print_typed_attr(prim.filePath, "filePath", indent + 1);
  ss << print_typed_attr(prim.auralMode, "auralMode", indent + 1);
  ss << print_typed_attr(prim.playbackMode, "playbackMode", indent + 1);
  ss << print_typed_attr(prim.startTime, "startTime", indent + 1);
  ss << print_typed_attr(prim.endTime, "endTime", indent + 1);
  ss << print_typed_attr(prim.mediaOffset, "mediaOffset", indent + 1);
  ss << print_typed_attr(prim.gain, "gain", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

}  // namespace lightusd
