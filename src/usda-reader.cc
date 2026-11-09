// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDA reader
//   - [ ] Refactor and unify Prim and PrimSpec related code.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stack>

#include "ascii-parser.hh"
//#include "asset-resolution.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "usdGeom.hh"
#include "usdSkel.hh"
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"
#include "mjcPhysics.hh"
#if defined(__wasi__)
#else
#include <mutex>
#include <thread>
#endif
#include <vector>

#include "usda-reader.hh"
#include "layer.hh"
#include "parser-timing.hh"
#include "enum-handlers.hh"

//
#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

//

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external

#include "nonstd/expected.hpp"
#include "nonstd/optional.hpp"

//

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

// Tentative
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif

#include "io-util.hh"
#include "math-util.inc"
#include "pprint-enum.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "prim-reconstruct.hh"
#include "primvar.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "usdShade.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "tiny-format.hh"

#include "common-macros.inc"
#include "usda-reader-impl.hh"

namespace lightusd {


namespace usda {


namespace {

static std::string TrimTrailingNewlines(std::string s) {
  while (!s.empty() && ((s.back() == '\n') || (s.back() == '\r'))) {
    s.pop_back();
  }
  return s;
}

static bool IsStructuredErrorHeader(const std::string &line) {
  if (line.empty()) {
    return false;
  }

  if ((line.rfind("Error at line ", 0) == 0) ||
      (line.rfind("Syntax Error at line ", 0) == 0) ||
      (line.rfind("Semantic Error at line ", 0) == 0) ||
      (line.rfind("Validation Error at line ", 0) == 0) ||
      (line.rfind("IO Error at line ", 0) == 0)) {
    return true;
  }

  return (line.find("():") != std::string::npos) &&
         ((line.rfind("/", 0) == 0) || (line.rfind("[", 0) == 0));
}

static std::vector<std::string> SplitStructuredErrorBlocks(
    const std::string &text) {
  std::vector<std::string> blocks;
  std::stringstream input(text);
  std::string line;
  std::string current;

  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }

    if (IsStructuredErrorHeader(line) && !current.empty()) {
      blocks.emplace_back(TrimTrailingNewlines(current));
      current.clear();
    }

    if (!current.empty()) {
      current += "\n";
    }
    current += line;
  }

  if (!current.empty()) {
    blocks.emplace_back(TrimTrailingNewlines(current));
  }

  return blocks;
}

static std::vector<std::string> SplitBlockLines(const std::string &block) {
  std::vector<std::string> lines;
  std::stringstream input(block);
  std::string line;
  while (std::getline(input, line)) {
    lines.emplace_back(line);
  }
  return lines;
}

static std::string JoinBlockLines(const std::vector<std::string> &lines) {
  std::stringstream ss;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) {
      ss << "\n";
    }
    ss << lines[i];
  }
  return ss.str();
}

static std::string GetBlockHeaderLine(const std::string &block) {
  size_t eol = block.find('\n');
  if (eol == std::string::npos) {
    return block;
  }

  return block.substr(0, eol);
}

static std::string ExtractStructuredMessage(const std::string &header_line) {
  size_t sig = header_line.rfind("):");
  if (sig == std::string::npos) {
    return header_line;
  }

  size_t space = header_line.find(' ', sig + 2);
  if (space == std::string::npos) {
    return header_line;
  }

  return header_line.substr(space + 1);
}

static std::string NormalizeRedundantMessage(std::string msg) {
  msg = TrimTrailingNewlines(msg);

  auto remove_prefix = [&](const std::string &prefix) {
    if (msg.rfind(prefix, 0) == 0) {
      msg = msg.substr(prefix.size());
      return true;
    }
    return false;
  };

  auto remove_suffix = [&](const std::string &suffix) {
    if ((msg.size() >= suffix.size()) &&
        (msg.compare(msg.size() - suffix.size(), suffix.size(), suffix) == 0)) {
      msg.resize(msg.size() - suffix.size());
      return true;
    }
    return false;
  };

  remove_prefix("Failed to parse ");
  remove_prefix("Failed to parse");
  remove_suffix(" parse failed.");
  remove_suffix(" parse failed");
  remove_suffix(" failed.");
  remove_suffix(" failed");

  std::string normalized;
  normalized.reserve(msg.size());
  for (char ch : msg) {
    if ((ch == '`') || (ch == '\'') || (ch == '"') || (ch == '.') ||
        (ch == ',') || (ch == ':')) {
      continue;
    }

    normalized.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }

  return normalized;
}

static bool IsGenericReconstructWrapperMessage(const std::string &msg) {
  if (msg.rfind("Failed to reconstruct ", 0) != 0) {
    return false;
  }

  return (msg.find(" Prim") != std::string::npos) ||
         (msg.find(" prim") != std::string::npos);
}

static bool IsSpecificReconstructMessage(const std::string &msg) {
  return (msg.rfind("Failed to Reconstruct ", 0) == 0) ||
         (msg.rfind("Failed to reconstruct ", 0) == 0);
}

static bool HasTrailingReconstructPrimMarker(const std::string &msg) {
  return msg.find(": Failed to reconstruct Prim: ") != std::string::npos;
}

static std::string StripTrailingReconstructPrimMarker(
    const std::string &block) {
  std::vector<std::string> lines = SplitBlockLines(block);
  if (lines.empty()) {
    return block;
  }

  const std::string marker = ": Failed to reconstruct Prim: ";
  size_t pos = lines[0].find(marker);
  if (pos == std::string::npos) {
    return block;
  }

  lines[0] = lines[0].substr(0, pos);
  return JoinBlockLines(lines);
}

static bool ShouldStripTrailingReconstructPrimMarker(
    const std::string &parent_block, const std::string &child_block) {
  const std::string parent_msg =
      ExtractStructuredMessage(GetBlockHeaderLine(parent_block));
  const std::string child_msg =
      ExtractStructuredMessage(GetBlockHeaderLine(child_block));

  return HasTrailingReconstructPrimMarker(parent_msg) &&
         IsSpecificReconstructMessage(child_msg);
}

static bool AreRedundantStructuredBlocks(const std::string &parent_block,
                                         const std::string &child_block) {
  const std::string parent_header = GetBlockHeaderLine(parent_block);
  const std::string child_header = GetBlockHeaderLine(child_block);

  if ((parent_header.rfind("Error at line ", 0) != 0) ||
      (child_header.rfind("Error at line ", 0) != 0)) {
    return false;
  }

  const std::string parent_msg =
      NormalizeRedundantMessage(ExtractStructuredMessage(parent_header));
  const std::string child_msg =
      NormalizeRedundantMessage(ExtractStructuredMessage(child_header));

  if (parent_msg.empty() || child_msg.empty()) {
    return false;
  }

  return parent_msg == child_msg;
}

static std::string MergeParentDetailsIntoChild(const std::string &parent_block,
                                               const std::string &child_block) {
  std::vector<std::string> parent_lines = SplitBlockLines(parent_block);
  std::vector<std::string> child_lines = SplitBlockLines(child_block);
  if (child_lines.empty()) {
    return child_block;
  }

  std::vector<std::string> merged;
  merged.reserve(parent_lines.size() + child_lines.size());
  merged.emplace_back(child_lines[0]);

  for (size_t i = 1; i < parent_lines.size(); ++i) {
    if (parent_lines[i].empty()) {
      continue;
    }

    bool duplicate = false;
    for (size_t j = 1; j < child_lines.size(); ++j) {
      if (child_lines[j] == parent_lines[i]) {
        duplicate = true;
        break;
      }
    }

    if (!duplicate) {
      merged.emplace_back(parent_lines[i]);
    }
  }

  for (size_t i = 1; i < child_lines.size(); ++i) {
    merged.emplace_back(child_lines[i]);
  }

  return JoinBlockLines(merged);
}

static bool ShouldMergeReconstructWrapper(const std::string &parent_block,
                                          const std::string &child_block) {
  const std::string parent_msg =
      ExtractStructuredMessage(GetBlockHeaderLine(parent_block));
  const std::string child_msg =
      ExtractStructuredMessage(GetBlockHeaderLine(child_block));

  if (!IsGenericReconstructWrapperMessage(parent_msg)) {
    return false;
  }

  if (!IsSpecificReconstructMessage(child_msg)) {
    return false;
  }

  return NormalizeRedundantMessage(parent_msg) !=
         NormalizeRedundantMessage(child_msg);
}

static std::vector<std::string> DeduplicateStructuredBlocks(
    const std::vector<std::string> &blocks) {
  std::vector<std::string> deduped;
  deduped.reserve(blocks.size());

  size_t i = 0;
  while (i < blocks.size()) {
    std::string current = blocks[i];

    if ((i + 1) < blocks.size()) {
      std::string next = blocks[i + 1];

      if (ShouldStripTrailingReconstructPrimMarker(current, next)) {
        current = StripTrailingReconstructPrimMarker(current);
      }

      if (AreRedundantStructuredBlocks(current, next)) {
        ++i;
        continue;
      }

      if (ShouldMergeReconstructWrapper(current, next)) {
        deduped.emplace_back(MergeParentDetailsIntoChild(current, next));
        i += 2;
        continue;
      }
    }

    deduped.emplace_back(current);
    ++i;
  }

  return deduped;
}

static void AppendIndentedBlock(std::stringstream &ss,
                                const std::string &block,
                                size_t depth) {
  std::stringstream input(block);
  std::string line;
  bool first_line = true;
  const std::string indent(depth * 2, ' ');
  const std::string detail_indent((depth * 2) + 2, ' ');
  bool first_detail_line = true;

  while (std::getline(input, line)) {
    if (first_line) {
      ss << indent << "- " << line << "\n";
      first_line = false;
    } else {
      if (IsStructuredErrorHeader(line) && first_detail_line) {
        ss << detail_indent << "-> " << line << "\n";
      } else {
        ss << detail_indent << line << "\n";
      }
      first_detail_line = false;
    }
  }
}

static std::string FormatStructuredErrorStack(
    const std::vector<std::string> &blocks,
    size_t max_blocks = 8) {
  if (blocks.empty()) {
    return std::string();
  }

  std::stringstream ss;
  ss << "Error stack:\n";

  size_t display_count = blocks.size();
  bool snipped = false;
  if ((max_blocks > 0) && (display_count > max_blocks)) {
    display_count = max_blocks - 1;
    snipped = true;
  }

  for (size_t i = 0; i < display_count; ++i) {
    AppendIndentedBlock(ss, blocks[i], i);
  }

  if (snipped) {
    const size_t omitted = blocks.size() - display_count;
    ss << std::string(display_count * 2, ' ')
       << "- ... " << omitted << " more frame"
       << ((omitted == 1) ? "" : "s") << " omitted ...";
  }

  return TrimTrailingNewlines(ss.str());
}

static std::string AppendPrimPath(const std::string &msg,
                                  const std::string &prim_path) {
  if (prim_path.empty()) {
    return msg;
  }

  size_t newline_pos = msg.find('\n');
  if (newline_pos == std::string::npos) {
    return msg + "\nPrim path: " + prim_path;
  }

  std::string result = msg.substr(0, newline_pos);
  result += "\nPrim path: " + prim_path;
  result += msg.substr(newline_pos);
  return result;
}

static std::string BuildStructuredReadErrorReport(
    const std::string &read_frame,
    const std::string &parser_error,
    const std::string &reconstruct_error,
    size_t max_blocks) {
  std::vector<std::string> blocks;
  blocks.emplace_back(read_frame);

  std::vector<std::string> parser_blocks =
      SplitStructuredErrorBlocks(parser_error);
  std::reverse(parser_blocks.begin(), parser_blocks.end());
  for (const auto &block : parser_blocks) {
    blocks.emplace_back(block);
  }

  std::vector<std::string> reconstruct_blocks =
      SplitStructuredErrorBlocks(reconstruct_error);
  for (const auto &block : reconstruct_blocks) {
    blocks.emplace_back(block);
  }

  return FormatStructuredErrorStack(DeduplicateStructuredBlocks(blocks),
                                    max_blocks);
}

}  // namespace  (error-helper anonymous namespace; supporting types moved to usda-reader-impl.hh)

class VariableDef {
 public:
  std::string type;
  std::string name;

  VariableDef() = default;

  VariableDef(const std::string &t, const std::string &n) : type(t), name(n) {}

  VariableDef(const VariableDef &rhs) = default;

  VariableDef &operator=(const VariableDef &rhs) {
    type = rhs.type;
    name = rhs.name;

    return *this;
  }
};

inline bool hasConnect(const std::string &str) {
  return endsWith(str, ".connect");
}

inline bool hasInputs(const std::string &str) {
  return startsWith(str, "inputs:");
}

inline bool hasOutputs(const std::string &str) {
  return startsWith(str, "outputs:");
}

// NOTE: CheckAllowedTokens and EnumHandler templates removed.
// Use centralized handlers from enum-handlers.hh instead.


namespace {

// Forward decl (mutually recursive with BuildVariantSetsRec for nested variantSets).
bool ToPrimSpecRec(const size_t primSpecIdx,
                        std::vector<PrimSpecNode> &primspec_nodes, PrimSpec &parent, std::string *err);

// Convert a variantNodeMap (variantSetName -> variantName -> VariantNode) into
// VariantSetSpecs. Recurses into each variant block's OWN nested variantSets
// (VariantNode::variantSets) so layer-mode load preserves them -- e.g. ALab's
// `render_high` geo variant holds a `geo_vis` variantSet supplying the proxy
// mesh. Without this recursion the Layer parser silently drops nested variant
// content (the Stage path in ConstructVariantPrimTreeRec already recurses).
bool BuildVariantSetsRec(
    const std::map<std::string, std::map<std::string, VariantNode>> &variantNodeMap,
    std::vector<PrimSpecNode> &primspec_nodes,
    std::set<int64_t> &variantChildrenIndices,
    std::map<std::string, VariantSetSpec> &out, std::string *err) {
  for (const auto &variantNodes : variantNodeMap) {
    VariantSetSpec variantSet;
    for (const auto &item : variantNodes.second) {
      PrimSpec variant;  // variantNode can be represented as PrimSpec.
      for (const int64_t vidx : item.second.primChildren) {
        if (variantChildrenIndices.count(vidx)) {
          if (err) {
            (*err) = fmt::format("variant primIdx {} is referenced multiple times.\n", vidx);
          }
          return false;
        }
        if ((vidx >= 0) && (size_t(vidx) < primspec_nodes.size())) {
          PrimSpec variantChildPrim;
          if (!ToPrimSpecRec(size_t(vidx), primspec_nodes, variantChildPrim, err)) {
            return false;
          }
          variant.children().emplace_back(variantChildPrim);
        } else {
          if (err) {
            (*err) = "primIndex exceeds prim_nodes.size()\n";
          }
          return false;
        }
        variantChildrenIndices.insert(vidx);
      }

      variant.metas() = item.second.metas;
      variant.props() = item.second.props;

      // Recurse into this variant block's own nested variantSets.
      if (!item.second.variantSets.empty()) {
        std::map<std::string, VariantSetSpec> nested;
        if (!BuildVariantSetsRec(item.second.variantSets, primspec_nodes,
                                 variantChildrenIndices, nested, err)) {
          return false;
        }
        variant.variantSets() = std::move(nested);
      }

      variantSet.name = variantNodes.first;
      variantSet.variantSet.emplace(item.first, std::move(variant));
    }
    out.emplace(variantNodes.first, std::move(variantSet));
  }
  return true;
}

// bottom up conversion.
bool ToPrimSpecRec(const size_t primSpecIdx,
                        std::vector<PrimSpecNode> &primspec_nodes, PrimSpec &parent, std::string *err) {

  if (primSpecIdx >= primspec_nodes.size()) {
    if (err) {
      (*err) += "Internal error; primSpecIdx exceeds primspec_nodes.size.";
    }
    return false;
  }

  const PrimSpecNode &node = primspec_nodes[primSpecIdx];

  PrimSpec primspec = node.primSpec;

  // Firstly process variants.
  std::set<int64_t> variantChildrenIndices; // record variantChildren indices
  {
    std::map<std::string, VariantSetSpec> variantSets;
    if (!BuildVariantSetsRec(node.variantNodeMap, primspec_nodes,
                             variantChildrenIndices, variantSets, err)) {
      return false;
    }
    primspec.variantSets() = std::move(variantSets);
  }

  for (const auto &cidx : node.children) {

    if (variantChildrenIndices.count(int64_t(cidx))) {
      // PrimSpec is already processed
      continue;
    }

    PrimSpec childPrimSpec;
    if (!ToPrimSpecRec(cidx, primspec_nodes, childPrimSpec, err)) {
      return false;
    }
    primspec.children().emplace_back(std::move(childPrimSpec));
  }

  parent = std::move(primspec);

  return true;
}

}  // namespace

bool USDAReader::Impl::GetAsLayer(Layer *layer) {

  if (!layer) {
    PUSH_ERROR_AND_RETURN("layer arg is nullptr.");
  }

  if (_primspec_invalidated) {
    PUSH_ERROR_AND_RETURN("PrimSpec data is invalid. USD data is not loaded or there was an error in earlier GetAsLayer call, or GetAsLayer was invoked multiple times.");
  }

  layer->clear_primspecs();
  DCOUT("# of subLayers = " << _stage.metas().subLayers.size());
  layer->metas() = _stage.metas();

  for (const auto &idx : _toplevel_primspecs) {
    DCOUT("Toplevel primspec idx: " << std::to_string(idx));

    if (idx >= _primspec_nodes.size()) {
      PUSH_ERROR_AND_RETURN("[Internal Error] out-of-bounds access.");
    }

    auto &node = _primspec_nodes[idx];
    PrimSpec &primSpec = node.primSpec;

    DCOUT("primspec[" << idx << "].typeName = " << primSpec.typeName());
    DCOUT("primspec[" << idx << "].name = " << primSpec.name());
    DCOUT("root prim[" << idx << "].num_children = " << primSpec.children().size());

    if (!ToPrimSpecRec(idx, _primspec_nodes, /* inout */primSpec, &_err)) {
      _primspec_invalidated = true;
      PUSH_ERROR_AND_RETURN("Construct PrimSpec tree failed.");
    }

    if (!layer->emplace_primspec(primSpec.name(), std::move(_primspec_nodes[idx].primSpec))) {
      PUSH_ERROR_AND_RETURN(fmt::format("Construct PrimSpec tree failed: PrimSpec.name = {}", primSpec.name()));
    }
  }

  // NOTE: _toplevel_primspecs are destroyed(std::move'ed)
  _primspec_invalidated = true;

  return true;
}

///
/// -- Impl reconstruct
//

namespace {

//
// TODO: Refeactor ConstructPrimTreeRec and ConstructVariantPrimTreeRec
//
bool ConstructPrimTreeRec(const size_t primIdx,
                        const std::vector<PrimNode> &prim_nodes,
                        const bool parent_is_variant,
                        Prim *destPrim,
                        std::string *err);

//
// Construct VariantPrim from with botom-up approach
//
bool ConstructVariantPrimTreeRec(const size_t variantPrimIdx,
                        const std::vector<PrimNode> &prim_nodes,
                        const std::string &variantName,
                        const std::map<std::string, VariantNode> &variantNodeMap,
                        std::map<std::string, VariantSet> &destVariantSets, /* inout */
                        std::string *err) {

  if (variantPrimIdx >= prim_nodes.size()) {
    if (err) {
      (*err) = "primIndex exceeds prim_nodes.size()\n";
    }
    return false;
  }

  const auto &node = prim_nodes[variantPrimIdx];

  std::set<int64_t> variantChildrenIndices; // record variantChildren indices

  VariantSet variantSet;
  for (const auto &item : variantNodeMap) {

      DCOUT("variant " << item.first);
      Variant variant;

      // Firstly process nested variants.
      for (const auto &childVariantNode: item.second.variantSets) {
        DCOUT("variantSet child " << childVariantNode.first);
        DCOUT("  variantPrimIdx " << variantPrimIdx);

        const std::string childVariantName = childVariantNode.first;
        Prim variantChildPrim(value::Value(nullptr)); // dummy
        if (!ConstructVariantPrimTreeRec(size_t(variantPrimIdx), prim_nodes, childVariantName, childVariantNode.second, variant.variantSets(), err)) {
          return false;
        }

      }

      for (const int64_t vidx : item.second.primChildren) {
        if (variantChildrenIndices.count(vidx)) {
          // Duplicated variant childrenIndices
          if (err) {
            (*err) = fmt::format("variant primIdx {} is referenced multiple times.\n", vidx);
          }
          return false;
        } else {
          // Add prim to variants
          if ((vidx >= 0) && (size_t(vidx) < prim_nodes.size())) {

            Prim variantChildPrim(value::Value(nullptr)); // dummy
            if (!ConstructPrimTreeRec(size_t(vidx), prim_nodes, /* parent_is_variant */true, &variantChildPrim, err)) {
              return false;
            }

            variant.primChildren().emplace_back(variantChildPrim);
          } else {
            if (err) {
              (*err) = "primIndex exceeds prim_nodes.size()\n";
            }
            return false;
          }

          variantChildrenIndices.insert(vidx);
        }
      }
      variant.metas() = std::move(item.second.metas);
      variant.properties() = std::move(item.second.props);

      variantSet.name = item.first;
      variantSet.variantSet[item.first] = std::move(variant);
    }

  destVariantSets[variantName] = std::move(variantSet);

  for (const auto &cidx : node.children) {
    DCOUT("parent: " << variantPrimIdx << ", child: " << cidx);
    if (variantChildrenIndices.count(int64_t(cidx))) {
      DCOUT("primIdx " << cidx << " processed");
      // Prim is processed
      continue;
    }

    Prim childPrim(value::Value(nullptr)); // dummy
    if (!ConstructPrimTreeRec(cidx, prim_nodes, /*parent_is_variant*/true, &childPrim, err)) {
      return false;
    }

    //DCOUT("Add childPrim " << childPrim.element_name() << " to Prim " << prim.element_name());
    //prim.children().emplace_back(std::move(childPrim));
  }

  //prim.variantSets() = std::move(variantSets);
  //(*destPrim) = std::move(prim);

  return true;
}

//
// Construct Prim from PrimNode with botom-up approach
//
bool ConstructPrimTreeRec(const size_t primIdx,
                        const std::vector<PrimNode> &prim_nodes,
                        const bool parent_is_variant,
                        Prim *destPrim,
                        std::string *err) {

  if (!destPrim) {
    if (err) {
      (*err) = "`destPrim` is nullptr.\n";
    }
    return false;
  }

  if (primIdx >= prim_nodes.size()) {
    if (err) {
      (*err) = "primIndex exceeds prim_nodes.size()\n";
    }
    return false;
  }

  const auto &node = prim_nodes[primIdx];

  Prim prim(node.prim);
  prim.prim_type_name() = node.typeName;
  prim.specifier() = node.specifier;  // def / over / class

  DCOUT("prim[" << primIdx << "].name = " << prim.element_name());
  DCOUT("prim[" << primIdx << "].type = " << node.prim.type_name());
  DCOUT("prim[" << primIdx << "].variantNodeMap.size = " << node.variantNodeMap.size());
  //prim.prim_id() = int64_t(idx);

  // Firstly process variants.
  std::set<int64_t> variantChildrenIndices; // record variantChildren indices

  std::map<std::string, VariantSet> variantSets;
  for (const auto &variantNodes : node.variantNodeMap) {
    DCOUT("variantSet " << variantNodes.first);
    VariantSet variantSet;
    for (const auto &item : variantNodes.second) {
      DCOUT("variant " << item.first);
      Variant variant;

      int64_t variantPrimIdx = item.second.variantPrimIdx;
      if (item.second.variantSets.size() && (variantPrimIdx < 0)) {
        if (err) {
          (*err) = "variantPrimIdx is not set.\n";
        }
        return false;
      }

      DCOUT("# of child variantSet " << item.second.variantSets.size());
      for (const auto &childVariantNode: item.second.variantSets) {
        DCOUT("variantSet node " << childVariantNode.first);
        DCOUT("  variantPrimIdx " << variantPrimIdx);

        Prim variantChildPrim(value::Value(nullptr)); // dummy
        if (!ConstructVariantPrimTreeRec(size_t(variantPrimIdx), prim_nodes, childVariantNode.first, childVariantNode.second, variant.variantSets(), err)) {
          return false;
        }

      }

      for (const int64_t vidx : item.second.primChildren) {
        if (variantChildrenIndices.count(vidx)) {
          // Duplicated variant childrenIndices
          if (err) {
            (*err) = fmt::format("variant primIdx {} is referenced multiple times.\n", vidx);
          }
          return false;
        } else {
          // Add prim to variants
          if ((vidx >= 0) && (size_t(vidx) < prim_nodes.size())) {

            Prim variantChildPrim(value::Value(nullptr)); // dummy
            if (!ConstructPrimTreeRec(size_t(vidx), prim_nodes, /* parent_is_variant */true, &variantChildPrim, err)) {
              return false;
            }

            DCOUT(fmt::format("Added prim {} to variantSet {} : variant {}", variantChildPrim.element_name(), variantNodes.first, item.first));
            variant.primChildren().emplace_back(variantChildPrim);
          } else {
            if (err) {
              (*err) = "primIndex exceeds prim_nodes.size()\n";
            }
            return false;
          }

          variantChildrenIndices.insert(vidx);
        }
      }
      variant.metas() = std::move(item.second.metas);
      variant.properties() = std::move(item.second.props);

      variantSet.name = variantNodes.first;
      variantSet.variantSet[item.first] = std::move(variant);
    }
    variantSets[variantNodes.first] = std::move(variantSet);
  }

  for (const auto &cidx : node.children) {
    DCOUT("parent: " << primIdx << ", child: " << cidx);
    if (variantChildrenIndices.count(int64_t(cidx))) {
      DCOUT("primIdx " << cidx << " processed");
      // Prim is processed
      continue;
    }

    Prim childPrim(value::Value(nullptr)); // dummy
    // inherit `parent_is_variant`
    if (!ConstructPrimTreeRec(cidx, prim_nodes, parent_is_variant, &childPrim, err)) {
      return false;
    }

    DCOUT("Add childPrim " << childPrim.element_name() << " to Prim " << prim.element_name());
    prim.children().emplace_back(std::move(childPrim));
  }

  prim.variantSets() = std::move(variantSets);
  (*destPrim) = std::move(prim);

  return true;
}

}  // namespace



bool USDAReader::Impl::ReconstructStage() {
  _stage.root_prims().clear();

  for (const auto &idx : _toplevel_prims) {
    DCOUT("Toplevel prim idx: " << std::to_string(idx));

    Prim prim(value::Value(nullptr)); // init with dummy Prim
    if (!ConstructPrimTreeRec(idx, _prim_nodes, /* parent_is_variant */false, &prim, &_err)) {
      return false;
    }

    _stage.root_prims().emplace_back(std::move(prim));

    DCOUT("num_children = " << _stage.root_prims()[size_t(_stage.root_prims().size() - 1)].children().size());
  }

  // Compute Abs Path from built Prim tree and Assign prim id.
  _stage.compute_absolute_prim_path_and_assign_prim_id();

  return true;
}

// Generic Prim handler. T = Xform, GeomMesh, ...
void USDAReader::Impl::buildReconstructOptions(
    const Path &full_path, prim::PrimReconstructOptions &options) {
  const int source_column_width = _config.error_detail ? (1024 * 1024) : 40;
  options.strict_allowedToken_check = _config.strict_allowedToken_check;
  options.strict_shader_type_check = _config.strict_shader_type_check;
  // MaterialX validation options
  options.validate_mtlx_connection_types = _config.validate_mtlx_connection_types || _config.strict_mtlx_check;
  options.validate_mtlx_info_id = _config.validate_mtlx_info_id || _config.strict_mtlx_check;
  options.validate_mtlx_connection_targets = _config.validate_mtlx_connection_targets || _config.strict_mtlx_check;
  options.validate_mtlx_duplicate_names = _config.validate_mtlx_duplicate_names || _config.strict_mtlx_check;
  options.validate_mtlx_index_bounds = _config.validate_mtlx_index_bounds || _config.strict_mtlx_check;
  options.strict_mtlx_check = _config.strict_mtlx_check;
  // NOTE: full_path / source_column_width captured by value: these lambdas are
  // stored in `options` and invoked later (during prim::ReconstructPrim), after
  // this function returns, so by-reference capture would dangle. _parser is an
  // Impl member that outlives the call, so capture via `this`.
  options.format_property_source_diagnostic =
      [this, full_path, source_column_width](const std::string &property_name) {
        return _parser.FormatPrimAttrSourceDiagnostic(
            full_path.full_path_name(), property_name, source_column_width);
      };
  options.format_property_path =
      [full_path](const std::string &property_name) {
        return full_path.full_path_name() + "." + property_name;
      };
  options.format_prim_source_diagnostic = [this, full_path, source_column_width]() {
    return _parser.FormatPrimSourceDiagnostic(full_path.full_path_name(),
                                             source_column_width);
  };
  options.format_prim_path = [full_path]() {
    return full_path.full_path_name();
  };
}


bool USDAReader::Impl::reportReconstructPrimError(
    const std::string &type_name, const Path &full_path,
    const std::string &err) {
  PUSH_ERROR_AND_RETURN(
      AppendPrimPath(
          fmt::format("Failed to reconstruct `{}` prim:\n{}", type_name, err),
          full_path.full_path_name()));
}

///
/// -- Impl callback specializations
///

///
/// -- Impl Read
///

bool USDAReader::Impl::Read(const uint32_t state_flags, bool as_primspec) {
  LIGHTUSD_PROFILE_FUNCTION("usda-reader");

  ///
  /// Convert parser option.
  ///
  ascii::AsciiParserOption ascii_parser_option;
  ascii_parser_option.allow_unknown_prim = _config.allow_unknown_prims;
  ascii_parser_option.allow_unknown_apiSchema = _config.allow_unknown_apiSchema;
  ascii_parser_option.strict_allowedToken_check = _config.strict_allowedToken_check;

  ///
  /// Setup callbacks.
  ///
  StageMetaProcessor();

  RegisterPrimIdxAssignCallback();

  // For composition(as_primspec == true)
  RegisterPrimSpecHandler();

  // For direct Prim reconstruction(load state = Toplevel)
  RegisterReconstructCallback<Model>();  // Generic prim.

  RegisterReconstructCallback<GPrim>(); // Geometric prim

  RegisterReconstructCallback<Xform>();
  RegisterReconstructCallback<GeomCube>();
  RegisterReconstructCallback<GeomSphere>();
  RegisterReconstructCallback<GeomCone>();
  RegisterReconstructCallback<GeomPoints>();
  RegisterReconstructCallback<GeomCylinder>();
  RegisterReconstructCallback<GeomCapsule>();
  RegisterReconstructCallback<GeomMesh>();
  RegisterReconstructCallback<Volume>();
  RegisterReconstructCallback<VolumeFieldBase>();
  RegisterReconstructCallback<FieldBase>();
  RegisterReconstructCallback<VolumeFieldAsset>();
  RegisterReconstructCallback<FieldAsset>();
  RegisterReconstructCallback<OpenVDBAsset>();
  RegisterReconstructCallback<Field3DAsset>();
  RegisterReconstructCallback<GeomSubset>();
  RegisterReconstructCallback<GeomBasisCurves>();
  RegisterReconstructCallback<GeomNurbsCurves>();
  RegisterReconstructCallback<GeomPlane>();
  RegisterReconstructCallback<GeomCylinder_1>();
  RegisterReconstructCallback<GeomCapsule_1>();
  RegisterReconstructCallback<GeomTetMesh>();
  RegisterReconstructCallback<GeomNurbsPatch>();
  RegisterReconstructCallback<GeomHermiteCurves>();
  RegisterReconstructCallback<GeomCamera>();
  RegisterReconstructCallback<GeomPointInstancer>();

  RegisterReconstructCallback<Material>();
  RegisterReconstructCallback<Shader>();
  RegisterReconstructCallback<NodeGraph>();

  RegisterReconstructCallback<Scope>();

  // UsdVol / UsdRender placeholder prim types.
  RegisterReconstructCallback<Volume>();
  RegisterReconstructCallback<OpenVDBAsset>();
  RegisterReconstructCallback<Field3DAsset>();
  RegisterReconstructCallback<RenderSettings>();
  RegisterReconstructCallback<RenderProduct>();
  RegisterReconstructCallback<RenderVar>();
  RegisterReconstructCallback<GenerativeProcedural>();

  RegisterReconstructCallback<SphereLight>();
  RegisterReconstructCallback<DomeLight>();
  RegisterReconstructCallback<DiskLight>();
  RegisterReconstructCallback<DistantLight>();
  RegisterReconstructCallback<CylinderLight>();
  RegisterReconstructCallback<RectLight>();
  RegisterReconstructCallback<GeometryLight>();
  RegisterReconstructCallback<PortalLight>();
  RegisterReconstructCallback<DomeLight_1>();
  RegisterReconstructCallback<LightFilter>();
  RegisterReconstructCallback<PluginLightFilter>();

  RegisterReconstructCallback<SkelRoot>();
  RegisterReconstructCallback<Skeleton>();
  RegisterReconstructCallback<SkelAnimation>();
  RegisterReconstructCallback<BlendShape>();

  // UsdPhysics + mjcPhysics
  RegisterReconstructCallback<PhysicsJoint>();
  RegisterReconstructCallback<PhysicsScene>();
  RegisterReconstructCallback<PhysicsRevoluteJoint>();
  RegisterReconstructCallback<PhysicsPrismaticJoint>();
  RegisterReconstructCallback<PhysicsSphericalJoint>();
  RegisterReconstructCallback<PhysicsFixedJoint>();
  RegisterReconstructCallback<PhysicsDistanceJoint>();
  RegisterReconstructCallback<PhysicsCollisionGroup>();
  RegisterReconstructCallback<MjcActuator>();
  RegisterReconstructCallback<NewtonActuator>();
  RegisterReconstructCallback<MjcTendon>();
  RegisterReconstructCallback<MjcKeyframe>();
  RegisterReconstructCallback<MjcSensor>();

  // AR/Interactive (Apple Preliminary_*)
  RegisterReconstructCallback<Preliminary_PhysicsGravitationalForce>();
  RegisterReconstructCallback<Preliminary_InfiniteColliderPlane>();
  RegisterReconstructCallback<Preliminary_ReferenceImage>();
  RegisterReconstructCallback<Preliminary_Behavior>();
  RegisterReconstructCallback<Preliminary_Trigger>();
  RegisterReconstructCallback<Preliminary_Action>();
  RegisterReconstructCallback<Preliminary_Text>();
  // usdMedia
  RegisterReconstructCallback<SpatialAudio>();

  _parser.set_primspec_mode(as_primspec);

  bool ret = _parser.Parse(state_flags, ascii_parser_option);

  std::string warn = _parser.GetWarning();
  if (!warn.empty()) {
    PUSH_WARN("<USDAParser> " + warn);
  }

  if (!ret) {
    std::string error_msg;
    const int source_column_width = _config.error_detail ? (1024 * 1024) : 40;
    if (!_filename.empty()) {
      error_msg = _parser.GetErrorWithSourceContext(_filename, 2,
                                                   source_column_width);
    }
    if (error_msg.empty()) {
      error_msg = _parser.GetError();
    }
    _err = BuildStructuredReadErrorReport(
        fmt::format("{}:Read():{} Failed to parse USDA", __FILE__, __LINE__),
        error_msg, _err, _config.error_detail ? size_t(0) : size_t(8));
    return false;
  }


  return true;
}

//
// --
//

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

///
/// -- USDAReader
///
USDAReader::USDAReader(StreamReader *sr) { _impl = new Impl(sr); }

USDAReader::~USDAReader() { delete _impl; }

bool USDAReader::read(const uint32_t state_flags, bool as_primspec) {
  return _impl->Read(state_flags, as_primspec);
}

void USDAReader::set_base_dir(const std::string &dir) {
  return _impl->SetBaseDir(dir);
}

void USDAReader::set_filename(const std::string &filename) {
  return _impl->SetFilename(filename);
}

// std::vector<GPrim> USDAReader::GetGPrims() { return _impl->GetGPrims(); }

//std::string USDAReader::GetDefaultPrimName() const {
//  return _impl->GetDefaultPrimName();
//}

std::string USDAReader::get_error() { return _impl->GetError(); }
std::string USDAReader::get_warning() { return _impl->GetWarning(); }

bool USDAReader::get_as_layer(Layer *layer) { return _impl->GetAsLayer(layer); }

bool USDAReader::reconstruct_stage() { return _impl->ReconstructStage(); }

const Stage &USDAReader::get_stage() const { return _impl->GetStage(); }

void USDAReader::set_reader_config(const USDAReaderConfig &config) {
  return _impl->set_reader_config(config);
}

const USDAReaderConfig USDAReader::get_reader_config() const {
  return _impl->get_reader_config();
}

void USDAReader::SetProgressCallback(std::function<bool(float progress, void *userptr)> callback, void *userptr) {
  _impl->SetProgressCallback(callback, userptr);
}

}  // namespace usda
}  // namespace lightusd

#else

namespace lightusd {
namespace usda {

USDAReader::USDAReader(StreamReader *sr) {
  _empty_stage = new Stage();
  (void)sr;
}

USDAReader::~USDAReader() {
  delete _empty_stage;
  _empty_stage = nullptr;
}

bool USDAReader::check_header() { return false; }

bool USDAReader::read(const LoadState state, bool as_primspec) {
  (void)state;
  (void)as_primspec;
  return false;
}

void USDAReader::set_base_dir(const std::string &dir) { (void)dir; }

//std::vector<GPrim> USDAReader::GetGPrims() { return {}; }

//std::string USDAReader::GetDefaultPrimName() const { return std::string{}; }

std::string USDAReader::get_error() {
  return "USDA parser feature is disabled in this build.\n";
}
std::string USDAReader::get_warning() { return std::string{}; }
bool USDAReader::reconstruct_stage() { return false; }

bool USDAReader::get_as_layer(Layer *layer) { return false; }

const Stage &USDAReader::get_stage() const {
  return *_empty_stage;
}

void USDAReader::set_reader_config(const USDAReaderConfig &config) {
  (void)config;
}

USDAReaderConfig USDAReader::get_reader_config() const {
  return USDAReaderConfig();
}

void USDAReader::SetProgressCallback(std::function<bool(float progress, void *userptr)> callback, void *userptr) {
  (void)callback;
  (void)userptr;
}

}  // namespace usda
}  // namespace lightusd

#endif
