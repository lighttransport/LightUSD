// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Skeleton prim to_string (extracted from pprinter.cc).
//
#include "pprinter.hh"       // for all declarations
#include "pprint-detail.hh"  // for templates

#include "common-macros.inc"

namespace lightusd {

std::string to_string(const SkelRoot &root, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  ::lightusd::pprint::Appender &ss = pp_ss.app();

  ss << pprint::Indent(indent) << to_string(root.spec) << " SkelRoot \""
     << root.name << "\"\n";
  if (root.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(root.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_token_attr(root.visibility, "visibility", indent + 1);
  ss << print_typed_token_attr(root.purpose, "purpose", indent + 1);
  ss << print_typed_attr(root.extent, "extent", indent + 1);

  if (root.proxyPrim) {
    ss << print_relationship(root.proxyPrim.value(),
                             root.proxyPrim.value().get_listedit_qual(),
                             /* custom */ false, "proxyPrim", indent + 1);
  }

  if (root.animationSource) {
    ss << print_relationship(root.animationSource.value(),
                             root.animationSource.value().get_listedit_qual(),
                             /* custom */ false, "skel:animationSource",
                             indent + 1);
  }

  if (root.skeleton) {
    ss << print_relationship(root.skeleton.value(),
                             root.skeleton.value().get_listedit_qual(),
                             /* custom */ false, "skel:skeleton", indent + 1);
  }

  ss << print_xformOps(root.xformOps, indent + 1);

  ss << print_props(root.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return pp_ss.str();
}

std::string to_string(const Skeleton &skel, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  ::lightusd::pprint::Appender &ss = pp_ss.app();

  ss << pprint::Indent(indent) << to_string(skel.spec) << " Skeleton \""
     << skel.name << "\"\n";
  if (skel.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(skel.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_attr(skel.bindTransforms, "bindTransforms", indent + 1);
  ss << print_typed_attr(skel.jointNames, "jointNames", indent + 1);
  ss << print_typed_attr(skel.joints, "joints", indent + 1);
  ss << print_typed_attr(skel.restTransforms, "restTransforms", indent + 1);

  if (skel.animationSource) {
    ss << print_relationship(skel.animationSource.value(),
                             skel.animationSource.value().get_listedit_qual(),
                             /* custom */ false, "skel:animationSource",
                             indent + 1);
  }

  if (skel.proxyPrim) {
    ss << print_relationship(skel.proxyPrim.value(),
                             skel.proxyPrim.value().get_listedit_qual(),
                             /* custom */ false, "proxyPrim", indent + 1);
  }

  ss << print_xformOps(skel.xformOps, indent + 1);

  ss << print_typed_token_attr(skel.visibility, "visibility", indent + 1);
  ss << print_typed_token_attr(skel.purpose, "purpose", indent + 1);
  ss << print_typed_attr(skel.extent, "extent", indent + 1);

  ss << print_props(skel.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return pp_ss.str();
}

std::string to_string(const SkelAnimation &skelanim, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  ::lightusd::pprint::Appender &ss = pp_ss.app();

  ss << pprint::Indent(indent) << to_string(skelanim.spec)
     << " SkelAnimation \"" << skelanim.name << "\"\n";
  if (skelanim.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(skelanim.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_attr(skelanim.blendShapes, "blendShapes", indent + 1);
  ss << print_typed_attr(skelanim.blendShapeWeights, "blendShapeWeights",
                         indent + 1);
  ss << print_typed_attr(skelanim.joints, "joints", indent + 1);
  ss << print_typed_attr(skelanim.rotations, "rotations", indent + 1);
  ss << print_typed_attr(skelanim.scales, "scales", indent + 1);
  ss << print_typed_attr(skelanim.translations, "translations", indent + 1);

  ss << print_props(skelanim.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return pp_ss.str();
}

std::string to_string(const BlendShape &prim, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  ::lightusd::pprint::Appender &ss = pp_ss.app();

  ss << pprint::Indent(indent) << to_string(prim.spec) << " BlendShape \""
     << prim.name << "\"\n";
  if (prim.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(prim.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_attr(prim.offsets, "offsets", indent + 1);
  ss << print_typed_attr(prim.normalOffsets, "normalOffsets", indent + 1);
  ss << print_typed_attr(prim.pointIndices, "pointIndices", indent + 1);

  ss << print_props(prim.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return pp_ss.str();
}

}  // namespace lightusd
