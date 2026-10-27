// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023, Light Transport Entertainment Inc.
//
// USD ASCII pretty printer — layer and prim tree traversal.
// Most of the former content has been split into:
//   pprint-enum.cc, pprint-meta.cc, pprint-detail.hh,
//   pprint-geom.cc, pprint-shader.cc, pprint-light.cc, pprint-skel.cc
//
#include "pprinter.hh"

#include "prim-pprint.hh"
#include "prim-pprint-parallel.hh"
#include "layer.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "value-pprint.hh"
#include "stream-writer.hh"
//
#include "common-macros.inc"

namespace lightusd {

std::string print_layer_metas(const LayerMetas &metas, const uint32_t indent) {
  // Each authored field is collected as (sortKey, rendered-text). usdcat writes
  // layer metadata fields in ALPHABETICAL order by their Sdf field TOKEN (note
  // `doc` sorts as "documentation"); the default keeps lightusd's historical
  // emission order. So under the USD-text-format opt-in we sort by sortKey,
  // otherwise we emit in insertion (historical) order -- byte-identical default.
  std::vector<std::pair<std::string, std::string>> fields;
  auto add = [&](const std::string &sort_key, const std::string &text) {
    fields.emplace_back(sort_key, text);
  };

  // Double-scalar layer metadata (metersPerUnit etc.). The default uses
  // std::ostream's 6-significant-digit formatting, which is LOSSY (e.g. the
  // float-widened 0.009999999776482582 prints as "0.01"). usdcat uses the
  // shortest round-trip representation, so under the OpenUSD float-format opt-in
  // emit these via dtos() to match (and avoid the precision loss).
  const bool usd_num = GetUSDFloatFormat();
  auto dnum = [usd_num](double v) -> std::string {
    if (usd_num) {
      return dtos(v);
    }
    std::ostringstream os;
    os << v;
    return os.str();
  };

  if (!metas.doc.value.empty()) {
    add("documentation", pprint::Indent(indent) + "doc = " +
                             to_string(metas.doc) + "\n");
  }

  if (metas.metersPerUnit.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "metersPerUnit = " << dnum(metas.metersPerUnit.get_value()) << "\n";
    add("metersPerUnit", pp_ss.str());
  }

  if (metas.kilogramsPerUnit.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "kilogramsPerUnit = " << dnum(metas.kilogramsPerUnit.get_value()) << "\n";
    add("kilogramsPerUnit", pp_ss.str());
  }

  if (metas.upAxis.authored()) {
    add("upAxis", pprint::Indent(indent) + "upAxis = " +
                      quote(to_string(metas.upAxis.get_value())) + "\n");
  }

  if (metas.timeCodesPerSecond.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "timeCodesPerSecond = " << dnum(metas.timeCodesPerSecond.get_value()) << "\n";
    add("timeCodesPerSecond", pp_ss.str());
  }

  if (metas.startTimeCode.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "startTimeCode = " << dnum(metas.startTimeCode.get_value()) << "\n";
    add("startTimeCode", pp_ss.str());
  }

  if (metas.endTimeCode.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "endTimeCode = " << dnum(metas.endTimeCode.get_value()) << "\n";
    add("endTimeCode", pp_ss.str());
  }

  if (metas.framesPerSecond.authored()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "framesPerSecond = " << dnum(metas.framesPerSecond.get_value()) << "\n";
    add("framesPerSecond", pp_ss.str());
  }

  // TODO: Do not print subLayers when consumed(after composition evaluated)
  if (metas.subLayers.size()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent) << "subLayers = " << metas.subLayers << "\n";
    add("subLayers", pp_ss.str());
  }

  if (metas.defaultPrim.str().size()) {
    add("defaultPrim", pprint::Indent(1) + "defaultPrim = " +
                           lightusd::quote(metas.defaultPrim.str()) + "\n");
  }

  if (metas.autoPlay.authored()) {
    add("autoPlay", pprint::Indent(1) + "autoPlay = " +
                        to_string(metas.autoPlay.get_value()) + "\n");
  }

  if (metas.playbackMode.authored()) {
    auto v = metas.playbackMode.get_value();
    const char *pv = (v == LayerMetas::PlaybackMode::PlaybackModeLoop)
                         ? "playbackMode = \"loop\"\n"
                         : "playbackMode = \"none\"\n";
    add("playbackMode", pprint::Indent(indent) + pv);
  }

  if (!metas.comment.value.empty()) {
    // Stage meta omits 'comment'
    add("comment", pprint::Indent(indent) + to_string(metas.comment) + "\n");
  }

  if (metas.customLayerData.size()) {
    add("customLayerData",
        print_customData(metas.customLayerData, "customLayerData",
                         /* indent */ 1));
  } else if (metas.customLayerDataAuthored) {
    // Print empty customLayerData if explicitly authored (match pxrUSD behavior)
    add("customLayerData", pprint::Indent(1) + "customLayerData = {\n" +
                               pprint::Indent(1) + "}\n");
  }

  // AOUSD Core Spec layer metadata
  if (metas.colorConfiguration) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent)
       << "colorConfiguration = " << metas.colorConfiguration.value() << "\n";
    add("colorConfiguration", pp_ss.str());
  }

  if (metas.colorManagementSystem) {
    add("colorManagementSystem",
        pprint::Indent(indent) + "colorManagementSystem = " +
            quote(metas.colorManagementSystem.value().str()) + "\n");
  }

  if (metas.renderSettingsPrimPath) {
    add("renderSettingsPrimPath",
        pprint::Indent(indent) + "renderSettingsPrimPath = " +
            quote(metas.renderSettingsPrimPath.value()) + "\n");
  }

  if (metas.owner) {
    add("owner", pprint::Indent(indent) + "owner = " +
                     quote(metas.owner.value()) + "\n");
  }

  if (metas.hasOwnedSubLayers) {
    add("hasOwnedSubLayers",
        pprint::Indent(indent) + "hasOwnedSubLayers = " +
            (metas.hasOwnedSubLayers.value() ? "true" : "false") + "\n");
  }

  if (metas.expressionVariables) {
    add("expressionVariables",
        print_customData(metas.expressionVariables.value(),
                         "expressionVariables", indent));
  }

  // AOUSD Core Spec 10.3.2.6: relocates
  if (!metas.layerRelocates.empty()) {
    ::lightusd::pprint::ScratchRef pp_ss;
    std::ostream &ss = pp_ss.os();
    ss << pprint::Indent(indent) << "relocates = {\n";
    for (size_t i = 0; i < metas.layerRelocates.size(); i++) {
      const auto &entry = metas.layerRelocates[i];
      // A relocate target may be the empty path (`<>`), denoting a deletion
      // relocate. Emit `<>` rather than the invalid-path placeholder.
      const std::string target =
          entry.second.is_valid() ? entry.second.full_path_name() : "";
      ss << pprint::Indent(indent + 1) << "<" << entry.first.full_path_name()
         << "> : " << "<" << target << ">";
      if (i + 1 < metas.layerRelocates.size()) {
        ss << ",";
      }
      ss << "\n";
    }
    ss << pprint::Indent(indent) << "}\n";
    add("layerRelocates", pp_ss.str());
  }

  // Unregistered layer metadata: write the stored raw text verbatim -- it
  // already contains quotes if the original value was a quoted string, and
  // non-string values (numbers, arrays, None, ...) are stored without quotes.
  // This matches OpenUSD behaviour (SdfUnregisteredValue).
  for (const auto &item : metas.unregisteredMetas) {
    add(item.first,
        pprint::Indent(indent) + item.first + " = " + item.second + "\n");
  }

  if (pprint::GetUSDTextFormat()) {
    std::stable_sort(fields.begin(), fields.end(),
                     [](const std::pair<std::string, std::string> &a,
                        const std::pair<std::string, std::string> &b) {
                       return a.first < b.first;
                     });
  }

  ::lightusd::pprint::ScratchRef pp_meta_ss;
  std::ostream &meta_ss = pp_meta_ss.os();
  for (const auto &f : fields) {
    meta_ss << f.second;
  }
  return pp_meta_ss.str();
}

std::string print_layer(const Layer &layer, const uint32_t indent, bool parallel) {
#if !defined(LIGHTUSD_ENABLE_THREAD)
  (void)parallel; // Threading disabled
#endif

  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  // FIXME: print magic-header outside of this function?
  ss << pprint::Indent(indent) << "#usda 1.0\n";

  ::lightusd::pprint::ScratchRef pp_meta_ss;
  std::ostream &meta_ss = pp_meta_ss.os();
  meta_ss << print_layer_metas(layer.metas(), indent + 1);

  if (pp_meta_ss.str().size()) {
    ss << "(\n";
    ss << pp_meta_ss.str();
    ss << ")\n";
  }

  ss << "\n";

  if (layer.metas().primChildren.size() == layer.primspecs().size()) {
    std::map<std::string, const PrimSpec *> primNameTable;
    for (const auto &item : layer.primspecs()) {
      primNameTable.emplace(item.first, &item.second);
    }

#if defined(LIGHTUSD_ENABLE_THREAD)
    if (parallel) {
      // Parallel printing path
      std::vector<const PrimSpec*> ordered_primspecs;
      ordered_primspecs.reserve(layer.metas().primChildren.size());

      for (size_t i = 0; i < layer.metas().primChildren.size(); i++) {
        value::token nameTok = layer.metas().primChildren[i];
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          ordered_primspecs.push_back(it->second);
        }
      }

      prim::ParallelPrintConfig config;
      ss << prim::print_primspecs_parallel(ordered_primspecs, indent, config);
    } else
#endif  // LIGHTUSD_ENABLE_THREAD
    {
      // Sequential printing path (original)
      for (size_t i = 0; i < layer.metas().primChildren.size(); i++) {
        value::token nameTok = layer.metas().primChildren[i];
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          ss << prim::print_primspec((*it->second), indent);
          if (i != (layer.metas().primChildren.size() - 1)) {
            ss << "\n";
          }
        }
      }
    }
  } else {
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (parallel) {
      // Parallel printing path
      std::vector<const PrimSpec*> primspecs;
      primspecs.reserve(layer.primspecs().size());
      for (const auto &item : layer.primspecs()) {
        primspecs.push_back(&item.second);
      }

      prim::ParallelPrintConfig config;
      ss << prim::print_primspecs_parallel(primspecs, indent, config);
    } else
#endif  // LIGHTUSD_ENABLE_THREAD
    {
      // Sequential printing path (original)
      size_t i = 0;
      for (const auto &item : layer.primspecs()) {
        ss << prim::print_primspec(item.second, indent);
        if (i != (layer.primspecs().size() - 1)) {
          ss << "\n";
        }
        i++;
      }
    }
  }

  return pp_ss.str();
}

// prim-pprint.hh
namespace prim {


// `reorder nameChildren/properties = [...]` body statements (preserved
// verbatim from parse; pxr keeps them in unflattened output).
static std::string print_reorder_stmts(const PrimMetas &metas,
                                       const uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  auto emit = [&](const char *field, const std::vector<value::token> &toks) {
    if (toks.empty()) return;
    ss << pprint::Indent(indent) << "reorder " << field << " = [";
    for (size_t i = 0; i < toks.size(); i++) {
      if (i) ss << ", ";
      ss << "\"" << toks[i].str() << "\"";
    }
    ss << "]\n";
  };
  emit("nameChildren", metas.nameChildrenReorder);
  emit("properties", metas.propertiesReorder);
  return pp_ss.str();
}

std::string print_prim(const Prim &prim, const uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  // Two-phase iterative DFS: ENTER prints header + variant + children-start,
  // EXIT prints closing brace.
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const Prim* prim;
    uint32_t indent;
    Phase phase;
    bool need_newline_before;  // for separating siblings
  };

  constexpr size_t kMaxIter = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&prim, indent, ENTER, false});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxIter) {
    WorkItem item = std::move(stack.back());
    stack.pop_back();

    if (item.phase == EXIT) {
      // Emit variantSets AFTER the prim's children, matching OpenUSD/usdcat
      // serialization order (children first, then variantSets).
      if (item.prim->variantSets().size()) {
        ss << print_variantSetStmt(item.prim->variantSets(), item.indent + 1);
      }
      ss << pprint::Indent(item.indent) << "}\n";
      continue;
    }

    // ENTER phase
    if (item.need_newline_before) {
      ss << "\n";
    }

    std::string s = pprint_value(item.prim->data(), item.indent, /* closing_brace */ false);

    bool require_newline = true;
    if (s.size() > 2) {
      if ((s[s.size() - 2] == '{') && (s[s.size() - 1] == '\n')) {
        require_newline = false;
      }
    }
    ss << s;

    {
      const std::string rs =
          print_reorder_stmts(item.prim->metas(), item.indent + 1);
      if (!rs.empty()) {
        if (require_newline) {
          ss << "\n";
          require_newline = false;
        }
        ss << rs;
      }
    }

    // variantSets are emitted at EXIT (after children) to match OpenUSD ordering.


    // Push EXIT for closing brace + variantSets (processed after all children)
    stack.push_back({item.prim, item.indent, EXIT, false});

    // Collect children in the order they should be printed
    std::vector<const Prim*> ordered_children;
    if (item.prim->children().size()) {
      if (require_newline) {
        ss << "\n";
      }

      if (pprint::GetPreserveAuthoredOrder() &&
          !item.prim->metas().primChildren.empty()) {
        // Opt-in authored order (matches usdcat): emit children named in the
        // `primChildren` field first, in that order, then any remaining children
        // lexicographically. Unlike the exact-size path below this tolerates a
        // `primChildren` that no longer matches the child set (e.g. composition
        // added children not recorded in the field) -- exactly OpenUSD's rule of
        // "listed order, else lexicographical".
        std::map<std::string, const Prim *> byName;  // sorted => lexicographical
        for (size_t i = 0; i < item.prim->children().size(); i++) {
          byName.emplace(item.prim->children()[i].element_name(),
                         &item.prim->children()[i]);
        }
        std::set<std::string> emitted;
        for (const auto &nameTok : item.prim->metas().primChildren) {
          const auto it = byName.find(nameTok.str());
          if (it != byName.end() && emitted.insert(it->first).second) {
            ordered_children.push_back(it->second);
          }
        }
        for (const auto &kv : byName) {
          if (!emitted.count(kv.first)) ordered_children.push_back(kv.second);
        }
      } else if (item.prim->metas().primChildren.size() == item.prim->children().size()) {
        std::map<std::string, const Prim *> primNameTable;
        for (size_t i = 0; i < item.prim->children().size(); i++) {
          primNameTable.emplace(item.prim->children()[i].element_name(),
                                &item.prim->children()[i]);
        }
        for (size_t i = 0; i < item.prim->metas().primChildren.size(); i++) {
          value::token nameTok = item.prim->metas().primChildren[i];
          DCOUT(fmt::format("primChildren  {}/{} = {}", i,
                            item.prim->metas().primChildren.size(), nameTok.str()));
          const auto it = primNameTable.find(nameTok.str());
          if (it != primNameTable.end()) {
            ordered_children.push_back(it->second);
          }
        }
      } else {
        for (size_t i = 0; i < item.prim->children().size(); i++) {
          ordered_children.push_back(&item.prim->children()[i]);
        }
      }
    } else if (item.prim->variantSets().size() && require_newline) {
      // No children, but variantSets follow at EXIT: break the line after `{`.
      ss << "\n";
    }

    // Push children in reverse order (so first child is processed first)
    for (auto it = ordered_children.rbegin(); it != ordered_children.rend(); ++it) {
      bool is_first_child = (it + 1 == ordered_children.rend());
      stack.push_back({*it, item.indent + 1, ENTER, !is_first_child});
    }
  }

  return pp_ss.str();
}

std::string print_primspec(const PrimSpec &primspec, const uint32_t indent) {
  // Children are walked iteratively below, but variant nesting recurses through
  // print_variantSetSpecStmt() -> print_primspec(); `indent` strictly increases
  // per level, so this entry guard bounds that mutual recursion (stack-overflow
  // protection on pathologically deep variant trees).
  if (indent > kMaxDefaultTraversalLimit) {
    return {};
  }

  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  // Two-phase iterative DFS
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const PrimSpec* primspec;
    uint32_t indent;
    Phase phase;
    bool need_separator;  // blank line before sibling
  };

  constexpr size_t kMaxIter = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&primspec, indent, ENTER, false});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxIter) {
    WorkItem item = std::move(stack.back());
    stack.pop_back();

    if (item.phase == EXIT) {
      ss << print_variantSetSpecStmt(item.primspec->variantSets(), item.indent + 1);
      ss << pprint::Indent(item.indent) << "}\n";
      continue;
    }

    // ENTER phase
    const bool usd_text = pprint::GetUSDTextFormat();
    if (item.need_separator) {
      // usdcat separates siblings with a PLAIN blank line; lightusd's default
      // indents it.
      if (usd_text) {
        ss << "\n";
      } else {
        ss << pprint::Indent(item.indent) << "\n";
      }
    }

    ss << pprint::Indent(item.indent) << to_string(item.primspec->specifier()) << " ";
    if (item.primspec->typeName().empty() || item.primspec->typeName() == "Model") {
      // do not emit typeName
    } else {
      ss << item.primspec->typeName() << " ";
    }

    ss << "\"" << item.primspec->name() << "\"";

    if (item.primspec->metas().authored()) {
      // usdcat puts the metadata opening paren on the `def` line (`def M "n" (`);
      // lightusd's default puts it on its own line.
      if (usd_text) {
        ss << " (\n";
      } else {
        ss << "\n" << pprint::Indent(item.indent) << "(\n";
      }
      ss << print_prim_metas(item.primspec->metas(), item.indent + 1);
      ss << pprint::Indent(item.indent) << ")\n";
    } else {
      ss << "\n";
    }
    ss << pprint::Indent(item.indent) << "{\n";

    const bool preserve_order = pprint::GetPreserveAuthoredOrder();

    // Properties stay alphabetical (std::map order): pxr/usdcat ALSO sorts
    // properties alphabetically (its crate writer sorts the `properties` field),
    // so the default already matches usdcat. Only prim CHILDREN preserve authored
    // order in USD, handled below.
    ss << print_props(item.primspec->props(), item.indent + 1);

    // usdcat blank-lines BETWEEN body sections: a blank line precedes each child
    // prim, but NOT the property block. So the FIRST child gets a separator only
    // when the prim authored properties (otherwise it sits directly under `{`).
    const bool has_props = !item.primspec->props().empty();

    // Push EXIT (processed after all children)
    stack.push_back({item.primspec, item.indent, EXIT, false});

    // Children: under the opt-in, emit those named in `primChildren` first (in
    // that order), then any remainder lexicographically; otherwise keep authored
    // vector order. Pushed in reverse so the first child is processed first.
    const auto& children = item.primspec->children();
    if (preserve_order && !item.primspec->metas().primChildren.empty()) {
      std::map<std::string, const PrimSpec *> byName;  // sorted => lexicographical
      for (const auto &c : children) byName.emplace(c.name(), &c);
      std::vector<const PrimSpec *> ordered;
      std::set<std::string> emitted;
      for (const auto &tok : item.primspec->metas().primChildren) {
        const auto it = byName.find(tok.str());
        if (it != byName.end() && emitted.insert(it->first).second) {
          ordered.push_back(it->second);
        }
      }
      for (const auto &kv : byName) {
        if (!emitted.count(kv.first)) ordered.push_back(kv.second);
      }
      for (size_t i = ordered.size(); i > 0; --i) {
        // usdcat: blank line before every child except the first; the first child
        // also gets one when properties precede it. ordered[0] is i==1.
        bool need_sep = usd_text ? (i != 1 || has_props) : (i < ordered.size());
        stack.push_back({ordered[i - 1], item.indent + 1, ENTER, need_sep});
      }
    } else {
      for (size_t i = children.size(); i > 0; --i) {
        bool need_sep =
            usd_text ? (i != 1 || has_props) : (i < children.size());
        stack.push_back({&children[i - 1], item.indent + 1, ENTER, need_sep});
      }
    }
  }

  return pp_ss.str();
}

// ============================================================================
// StreamWriter overloads for efficient printing
// ============================================================================

void print_prim(StreamWriter& writer, const Prim &prim, const uint32_t indent) {
  writer.write(print_prim(prim, indent));
}

void print_primspec(StreamWriter& writer, const PrimSpec &primspec, const uint32_t indent) {
  writer.write(print_primspec(primspec, indent));
}

// ============================================================================
// ChunkedStreamWriter template implementations
// ============================================================================

template <size_t ChunkSize, size_t Alignment>
void print_prim(ChunkedStreamWriter<ChunkSize, Alignment>& writer, const Prim &prim, const uint32_t indent) {
  std::string s = pprint_value(prim.data(), indent, /* closing_brace */ false);

  bool require_newline = true;

  if (s.size() > 2) {
    if ((s[s.size() - 2] == '{') && (s[s.size() - 1] == '\n')) {
      require_newline = false;
    }
  }

  writer.write(s);

  //
  // print variant
  //
  if (prim.variantSets().size()) {
    if (require_newline) {
      writer.write("\n");
    }

    require_newline = true;

    for (const auto &variantSet : prim.variantSets()) {
      writer.write(pprint::Indent(indent + 1));
      writer.write("variantSet ");
      writer.write(quote(variantSet.first));
      writer.write(" = {\n");

      for (const auto &variantItem : variantSet.second.variantSet) {
        writer.write(pprint::Indent(indent + 2));
        writer.write(quote(variantItem.first));

        const Variant &variant = variantItem.second;

        if (variant.metas().authored()) {
          writer.write(" (\n");
          writer.write(print_prim_metas(variant.metas(), indent + 3));
          writer.write(pprint::Indent(indent + 2));
          writer.write(")");
        }

        writer.write(" {\n");

        writer.write(print_props(variant.properties(), indent + 3));

        if (variant.metas().variantChildren.has_value() &&
            (variant.metas().variantChildren.value().size() ==
             variant.primChildren().size())) {
          std::map<std::string, const Prim *> primNameTable;
          for (size_t i = 0; i < variant.primChildren().size(); i++) {
            primNameTable.emplace(variant.primChildren()[i].element_name(),
                                  &variant.primChildren()[i]);
          }

          for (size_t i = 0; i < variant.metas().variantChildren.value().size();
               i++) {
            value::token nameTok = variant.metas().variantChildren.value()[i];
            const auto it = primNameTable.find(nameTok.str());
            if (it != primNameTable.end()) {
              print_prim(writer, *(it->second), indent + 3);
              if (i != (variant.primChildren().size() - 1)) {
                writer.write("\n");
              }
            }
          }

        } else {
          for (size_t i = 0; i < variant.primChildren().size(); i++) {
            print_prim(writer, variant.primChildren()[i], indent + 3);
            if (i != (variant.primChildren().size() - 1)) {
              writer.write("\n");
            }
          }
        }

        writer.write(pprint::Indent(indent + 2));
        writer.write("}\n");
      }

      writer.write(pprint::Indent(indent + 1));
      writer.write("}\n");
    }
  }

  //
  // primChildren
  //
  if (prim.children().size()) {
    if (require_newline) {
      writer.write("\n");
      require_newline = false;
    }
    if (prim.metas().primChildren.size() == prim.children().size()) {
      std::map<std::string, const Prim *> primNameTable;
      for (size_t i = 0; i < prim.children().size(); i++) {
        primNameTable.emplace(prim.children()[i].element_name(),
                              &prim.children()[i]);
      }

      for (size_t i = 0; i < prim.metas().primChildren.size(); i++) {
        if (i > 0) {
          writer.write("\n");
        }
        value::token nameTok = prim.metas().primChildren[i];
        DCOUT(fmt::format("primChildren  {}/{} = {}", i,
                          prim.metas().primChildren.size(), nameTok.str()));
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          print_prim(writer, *(it->second), indent + 1);
        }
      }

    } else {
      for (size_t i = 0; i < prim.children().size(); i++) {
        if (i > 0) {
          writer.write("\n");
        }
        print_prim(writer, prim.children()[i], indent + 1);
      }
    }
  }

  writer.write(pprint::Indent(indent));
  writer.write("}\n");
}

template <size_t ChunkSize, size_t Alignment>
void print_primspec(ChunkedStreamWriter<ChunkSize, Alignment>& writer, const PrimSpec &primspec, const uint32_t indent) {
  writer.write(pprint::Indent(indent));
  writer.write(to_string(primspec.specifier()));
  writer.write(" ");

  if (primspec.typeName().empty() || primspec.typeName() == "Model") {
    // do not emit typeName
  } else {
    writer.write(primspec.typeName());
    writer.write(" ");
  }

  writer.write("\"");
  writer.write(primspec.name());
  writer.write("\"\n");

  if (primspec.metas().authored()) {
    writer.write(pprint::Indent(indent));
    writer.write("(\n");
    writer.write(print_prim_metas(primspec.metas(), indent + 1));
    writer.write(pprint::Indent(indent));
    writer.write(")\n");
  }
  writer.write(pprint::Indent(indent));
  writer.write("{\n");

  writer.write(print_reorder_stmts(primspec.metas(), indent + 1));
  writer.write(print_props(primspec.props(), indent + 1));

  for (size_t i = 0; i < primspec.children().size(); i++) {
    if (i > 0) {
      writer.write(pprint::Indent(indent));
      writer.write("\n");
    }
    print_primspec(writer, primspec.children()[i], indent + 1);
  }

  writer.write(print_variantSetSpecStmt(primspec.variantSets(), indent + 1));

  writer.write(pprint::Indent(indent));
  writer.write("}\n");
}

// Explicit template instantiations for common parameters
template void print_prim<4096, 16>(ChunkedStreamWriter<4096, 16>& writer, const Prim &prim, const uint32_t indent);
template void print_primspec<4096, 16>(ChunkedStreamWriter<4096, 16>& writer, const PrimSpec &primspec, const uint32_t indent);

}  // namespace prim

std::string to_string(const Layer &layer, const uint32_t indent,
                      bool closing_brace) {
  (void)closing_brace;
  return print_layer(layer, indent);
}

std::string to_string(const PrimSpec &primspec, const uint32_t indent,
                      bool closing_brace) {
  (void)closing_brace;
  return prim::print_primspec(primspec, indent);
}

}  // namespace lightusd
