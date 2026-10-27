// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// AR/Interactive prim to_string (Apple Preliminary_* schemas).
//
#include "pprinter.hh"
#include "pprint-detail.hh"
#include "str-util.hh"

#include "common-macros.inc"

namespace lightusd {

// Helper: print a RelationshipProperty if authored
static std::string print_rel_prop(const RelationshipProperty &rp,
                                  const std::string &name, uint32_t indent) {
  if (!rp.authored()) return "";
  return print_relationship(rp.relationship(), rp.get_listedit_qual(),
                            /* custom */ false, name, indent);
}

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

std::string to_string(const Preliminary_PhysicsGravitationalForce &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_PhysicsGravitationalForce");
  ss << print_typed_attr(prim.acceleration, "physics:gravitationalForce:acceleration", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

std::string to_string(const Preliminary_InfiniteColliderPlane &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_InfiniteColliderPlane");
  ss << print_typed_attr(prim.position, "position", indent + 1);
  ss << print_typed_attr(prim.normal, "normal", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

std::string to_string(const Preliminary_ReferenceImage &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_ReferenceImage");
  ss << print_typed_attr(prim.image, "image", indent + 1);
  ss << print_typed_attr(prim.physicalWidth, "physicalWidth", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

std::string to_string(const Preliminary_Behavior &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_Behavior");
  ss << print_rel_prop(prim.triggers, "triggers", indent + 1);
  ss << print_rel_prop(prim.actions, "actions", indent + 1);
  ss << print_typed_attr(prim.exclusive, "exclusive", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

std::string to_string(const Preliminary_Trigger &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_Trigger");
  ss << print_typed_attr(prim.info_id, "info:id", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

std::string to_string(const Preliminary_Action &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_Action");
  ss << print_typed_attr(prim.info_id, "info:id", indent + 1);
  ss << print_typed_attr(prim.multiplePerformOperation, "multiplePerformOperation", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

// Helper: print a string attribute with proper quoting
static std::string print_string_attr(const TypedAttributeWithFallback<std::string> &attr,
                                     const std::string &name, uint32_t indent) {
  if (!attr.authored()) return "";
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << pprint::Indent(indent) << "string " << name << " = "
     << quote(attr.get_value()) << "\n";
  return pp_ss.str();
}

// Helper: print a string array attribute with proper quoting
static std::string print_string_array_attr(const TypedAttribute<std::vector<std::string>> &attr,
                                           const std::string &name, uint32_t indent) {
  if (!attr.authored()) return "";
  auto v = attr.get_value();
  if (!v) return "";
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << pprint::Indent(indent) << "string[] " << name << " = [";
  for (size_t i = 0; i < v.value().size(); i++) {
    if (i > 0) ss << ", ";
    ss << quote(v.value()[i]);
  }
  ss << "]\n";
  return pp_ss.str();
}

std::string to_string(const Preliminary_Text &prim,
                      const uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  PRINT_PRIM_HEADER(prim, "Preliminary_Text");
  ss << print_string_attr(prim.content, "content", indent + 1);
  ss << print_string_array_attr(prim.font, "font", indent + 1);
  ss << print_typed_attr(prim.pointSize, "pointSize", indent + 1);
  ss << print_typed_attr(prim.width, "width", indent + 1);
  ss << print_typed_attr(prim.height, "height", indent + 1);
  ss << print_typed_attr(prim.depth, "depth", indent + 1);
  ss << print_typed_attr(prim.wrapMode, "wrapMode", indent + 1);
  ss << print_typed_attr(prim.horizontalAlignment, "horizontalAlignment", indent + 1);
  ss << print_typed_attr(prim.verticalAlignment, "verticalAlignment", indent + 1);
  PRINT_PRIM_FOOTER(prim);
  return pp_ss.str();
}

}  // namespace lightusd
