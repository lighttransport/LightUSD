// SPDX-License-Identifier: Apache-2.0
#include "usda-writer-print.hh"

namespace lightusd {
namespace next {

const char* SpecifierKeyword(PrimSpecifier spec) {
  switch (spec) {
    case PrimSpecifier::Def:   return "def";
    case PrimSpecifier::Over:  return "over";
    case PrimSpecifier::Class: return "class";
    default: return "def";
  }
}

// Write indent.
//
// Emits `depth` copies of `indent` in a single write, backed by a thread-local
// cache of the repeated unit (grown on demand). This avoids `depth` tiny writes
// per prim/property on the hot path. The cache is thread_local so it stays
// correct under the parallel subtree stitcher (each worker has its own
// StreamWriter). Byte-identical to emitting `indent` in a loop for any unit.
void WriteIndent(StreamWriter& os, int depth, const std::string& indent) {
  if (depth <= 0 || indent.empty()) return;
  thread_local std::string pad;    // cached repetition of `unit`
  thread_local std::string unit;   // the indent unit `pad` was built from
  thread_local int levels = 0;     // number of `unit` copies currently in `pad`
  if (unit != indent) {
    unit = indent;
    pad.clear();
    levels = 0;
  }
  if (depth > levels) {
    pad.reserve(static_cast<size_t>(depth) * indent.size());
    for (; levels < depth; ++levels) pad += indent;
  }
  os.write(pad.data(), static_cast<size_t>(depth) * indent.size());
}

// Escape a string for USDA output

// Prefix every line of `s` after the first with `prefix`. Used to re-indent a
// multi-line dictionary value (from PrintValue) under its metadata key so the
// `{` stays inline and the body/closing brace align with the surrounding depth.
std::string IndentContinuation(const std::string& s, const std::string& prefix) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(c);
    if (c == '\n') out += prefix;
  }
  return out;
}

// Render a dictionary-valued metadatum as `<key> = { ... }` with body indented
// `level` units. Returns empty if the value is not a (non-empty) dictionary.
std::string DictMetaLine(const std::string& key, const Value& v, int level,
                         const USDAWriteOptions& opts) {
  if (!v.is_dictionary()) return "";
  auto d = v.as_dictionary();
  if (!d) return "";
  PrintOptions print_opts;
  print_opts.indent = opts.indent;
  std::string base;
  for (int i = 0; i < level; ++i) base += opts.indent;
  return key + " = " + IndentContinuation(PrintValue(v, print_opts), base);
}

// Write layer metadata block
void WriteLayerMeta(StreamWriter& os, const LayerMeta& meta,
                    const USDAWriteOptions& opts) {
  os << "#usda 1.0\n";
  os << "(\n";

  std::vector<std::string> lines;

  if (meta.defaultPrim_set || !meta.defaultPrim.empty()) {
    lines.push_back(opts.indent + "defaultPrim = " + EscapeUSDAString(meta.defaultPrim));
  }

  if ((meta.doc_set || !meta.doc.empty()) && opts.include_comments) {
    lines.push_back(opts.indent + "doc = " + EscapeUSDAString(meta.doc));
  }

  if ((meta.comment_set || !meta.comment.empty()) && opts.include_comments) {
    // Comments are spelled as a BARE string literal (pxr 26.x rejects
    // `comment = "..."` as a non-metadata field).
    lines.push_back(opts.indent + EscapeUSDAString(meta.comment));
  }

  if (meta.hasOwnedSubLayers_set) {
    lines.push_back(opts.indent + std::string("hasOwnedSubLayers = ") +
                    (meta.hasOwnedSubLayers ? "true" : "false"));
  }
  if (meta.owner_set || !meta.owner.empty()) {
    lines.push_back(opts.indent + "owner = " + EscapeUSDAString(meta.owner));
  }
  if (meta.autoPlay_set) {
    lines.push_back(opts.indent + std::string("autoPlay = ") +
                    (meta.autoPlay ? "true" : "false"));
  }
  if (meta.playbackMode_set) {
    lines.push_back(opts.indent + "playbackMode = " +
                    EscapeUSDAString(meta.playbackMode));
  }

  if (meta.metersPerUnit_set || meta.metersPerUnit != 0.01) {
    lines.push_back(opts.indent + "metersPerUnit = " + dtos(meta.metersPerUnit));
  }

  if (meta.startTimeCode_set || meta.startTimeCode != 0.0) {
    lines.push_back(opts.indent + "startTimeCode = " + dtos(meta.startTimeCode));
  }

  if (meta.endTimeCode_set || meta.endTimeCode != 0.0) {
    lines.push_back(opts.indent + "endTimeCode = " + dtos(meta.endTimeCode));
  }

  if (meta.timeCodesPerSecond_set || meta.timeCodesPerSecond != 24.0) {
    lines.push_back(opts.indent + "timeCodesPerSecond = " + dtos(meta.timeCodesPerSecond));
  }

  if (meta.upAxis_set || meta.upAxis != "Y") {
    lines.push_back(opts.indent + "upAxis = " + EscapeUSDAString(meta.upAxis));
  }

  if (meta.framesPerSecond_set) {
    lines.push_back(opts.indent + "framesPerSecond = " + dtos(meta.framesPerSecond));
  }

  if (meta.kilogramsPerUnit_set) {
    lines.push_back(opts.indent + "kilogramsPerUnit = " + dtos(meta.kilogramsPerUnit));
  }

  for (const auto& um : meta.unknownMeta) {
    lines.push_back(opts.indent + um.first + " = " + um.second);
  }
  for (const auto& field : meta.unknownFields) {
    // Verbatim raw source only for DICT payloads (its whole purpose); plain
    // strings must go through PrintValue so quoting survives ("$Side").
    lines.push_back(opts.indent + field.name + " = " +
                    (field.unregistered && !field.unregistered_source.empty() &&
                             field.value.is_dictionary()
                         ? field.unregistered_source
                         : PrintValue(field.value, PrintOptions{})));
  }
  if (meta.relocates_set || !meta.relocates.empty()) {
    if (meta.relocates.empty()) {
      lines.push_back(opts.indent + "relocates = {}");
    } else {
      std::string s = opts.indent + "relocates = {\n";
      for (const auto& r : meta.relocates) {
        s += opts.indent + opts.indent + "<" + r.first + ">: <" + r.second +
             ">,\n";
      }
      s += opts.indent + "}";
      lines.push_back(s);
    }
  }
  if (meta.colorConfiguration_set || !meta.colorConfiguration.empty()) {
    lines.push_back(opts.indent + "colorConfiguration = " +
                    FormatAssetPathForUsda(meta.colorConfiguration));
  }

  if (meta.colorManagementSystem_set || !meta.colorManagementSystem.empty()) {
    lines.push_back(opts.indent + "colorManagementSystem = " +
                    EscapeUSDAString(meta.colorManagementSystem));
  }

  if (meta.renderSettingsPrimPath_set || !meta.renderSettingsPrimPath.empty()) {
    lines.push_back(opts.indent + "renderSettingsPrimPath = " +
                    EscapeUSDAString(meta.renderSettingsPrimPath));
  }

  {
    std::string s;
    if (meta.customLayerData_set ||
        (meta.customLayerData.is_dictionary() &&
         meta.customLayerData.as_dictionary() &&
         !meta.customLayerData.as_dictionary()->empty())) {
      s = DictMetaLine("customLayerData", meta.customLayerData, 1, opts);
    }
    if (!s.empty()) lines.push_back(opts.indent + s);
    s.clear();
    if (meta.expressionVariables_set ||
        (meta.expressionVariables.is_dictionary() &&
         meta.expressionVariables.as_dictionary() &&
         !meta.expressionVariables.as_dictionary()->empty())) {
      s = DictMetaLine("expressionVariables", meta.expressionVariables, 1,
                       opts);
    }
    if (!s.empty()) lines.push_back(opts.indent + s);
  }

  if (meta.subLayers_set && meta.subLayers.empty()) {
    lines.push_back(opts.indent + "subLayers = []");
  } else if (!meta.subLayers.empty()) {
    std::string s = opts.indent + "subLayers = [\n";
    for (size_t i = 0; i < meta.subLayers.size(); ++i) {
      s += opts.indent + opts.indent + FormatAssetPathForUsda(meta.subLayers[i]);
      if (i < meta.subLayerOffsets.size()) {
        const double off = meta.subLayerOffsets[i].first;
        const double scl = meta.subLayerOffsets[i].second;
        if (off != 0.0 || scl != 1.0) {
          s += " (";
          if (off != 0.0) {
            s += "offset = " + dtos(off);
            if (scl != 1.0) s += "; ";
          }
          if (scl != 1.0) s += "scale = " + dtos(scl);
          s += ")";
        }
      }
      s += ",\n";
    }
    s += opts.indent + "]";
    lines.push_back(std::move(s));
  }

  // pxr emits layer metadata in ALPHABETICAL key order (the bare-string
  // comment's leading quote sorts first, matching pxr's comment-first form).
  std::stable_sort(lines.begin(), lines.end(),
                   [&](const std::string& a, const std::string& b) {
                     const size_t n = opts.indent.size();
                     return a.compare(n, std::string::npos, b, n,
                                      std::string::npos) < 0;
                   });

  for (size_t i = 0; i < lines.size(); ++i) {
    os << lines[i];
    if (i + 1 < lines.size()) {
      os << "\n";
    }
  }

  os << "\n)\n\n";
}

// Emit the property metadata `( ... )` block (interpolation, customData, ...)
// when authored. Streams " (\n ... )" with NO trailing newline so the caller can
// place it before the property's terminating newline. Returns true if emitted.
bool WritePropMeta(StreamWriter& os, const PrimSpec& spec, PropNameId name_id,
                   int depth, const USDAWriteOptions& opts) {
  const PropMeta* m = spec.property_meta(name_id);
  if (!m || m->empty()) return false;
  const int md = depth + 1;
  os << " (\n";
  auto kv = [&](const std::string& s) {
    if (s.empty()) return;  // e.g. DictMetaLine of an empty dict
    WriteIndent(os, md, opts.indent);
    os << s << "\n";
  };
  if (m->authored & PropMeta::kInterpolation)
    kv("interpolation = " + EscapeUSDAString(m->interpolation));
  if (m->authored & PropMeta::kElementSize)
    kv("elementSize = " + IntToStr(static_cast<long long>(m->elementSize)));
  if (m->authored & PropMeta::kColorSpace)
    kv("colorSpace = " + EscapeUSDAString(m->colorSpace));
  if (m->authored & PropMeta::kCustomData)
    kv(DictMetaLine("customData", m->customData, md, opts));
  if (m->authored & PropMeta::kDisplayName)
    kv("displayName = " + EscapeUSDAString(m->displayName));
  if (m->authored & PropMeta::kDisplayGroup)
    kv("displayGroup = " + EscapeUSDAString(m->displayGroup));
  if (m->authored & PropMeta::kDoc)
    kv("doc = " + EscapeUSDAString(m->doc));
  if (m->authored & PropMeta::kComment)
    kv(EscapeUSDAString(m->comment));  // bare string literal = comment (pxr form)
  if (m->authored & PropMeta::kPermission)
    kv("permission = " + m->permission);  // unquoted token
  if (m->authored & PropMeta::kHidden)
    kv(std::string("hidden = ") + (m->hidden ? "true" : "false"));
  if (m->authored & PropMeta::kRenderType)
    kv("renderType = " + EscapeUSDAString(m->renderType));
  if (m->authored & PropMeta::kConnectability)
    kv("connectability = " + EscapeUSDAString(m->connectability));
  if (m->authored & PropMeta::kOutputName)
    kv("outputName = " + EscapeUSDAString(m->outputName));
  if (m->authored & PropMeta::kBindMaterialAs)
    kv("bindMaterialAs = " + EscapeUSDAString(m->bindMaterialAs));
  if (m->authored & PropMeta::kKind)
    kv("kind = " + EscapeUSDAString(m->kind));
  if (m->authored & PropMeta::kWeight) {
    kv("weight = " + dtos(m->weight));
  }
  if (m->authored & PropMeta::kUnauthoredIdx)
    kv("unauthoredValuesIndex = " + IntToStr(static_cast<long long>(m->unauthoredValuesIndex)));
  if (m->authored & PropMeta::kAllowedTokens) {
    std::string s = "allowedTokens = [";
    for (size_t i = 0; i < m->allowedTokens.size(); ++i) {
      if (i) s += ", ";
      s += EscapeUSDAString(m->allowedTokens[i]);
    }
    s += "]";
    kv(s);
  }
  if (m->authored & PropMeta::kAssetInfo)
    kv(DictMetaLine("assetInfo", m->assetInfo, md, opts));
  if (m->authored & PropMeta::kSdrMetadata)
    kv(DictMetaLine("sdrMetadata", m->sdrMetadata, md, opts));
  if (!m->unknownMeta.empty()) {
    for (const auto& um : m->unknownMeta) {
      kv(um.first + " = " + um.second);  // verbatim raw source text
    }
  }
  for (const auto& field : m->unknownFields) {
    kv(field.name + " = " +
       (field.unregistered && !field.unregistered_source.empty() &&
                field.value.is_dictionary()
            ? field.unregistered_source
            : PrintValue(field.value, PrintOptions{})));
  }
  WriteIndent(os, depth, opts.indent);
  os << ")";
  return true;
}

// Write time samples for a property
void WriteTimeSamples(StreamWriter& os, const std::string& name, PropNameId name_id,
                      const PrimSpec& spec, int depth, const USDAWriteOptions& opts,
                      SegmentSink* segsink) {
  const auto* samples = spec.time_samples(name_id);
  if (!samples || samples->empty()) return;

  WriteIndent(os, depth, opts.indent);

  // Type name: prefer the declared name, else derive from the first sample.
  if (const std::string* decl = spec.property_type_name(name_id)) {
    os << *decl;
  } else {
    const Value* first_val = spec.time_sample_value(samples->front().second);
    if (first_val) {
      const char* tn = GetTypeName(first_val->type_id());
      os << (tn ? tn : "token");
      if (first_val->is_array()) os << "[]";
    }
  }

  os << " " << name << ".timeSamples = {\n";

  PrintOptions print_opts;
  print_opts.float_precision = opts.float_precision;
  print_opts.double_precision = opts.double_precision;
  print_opts.max_array_elements = opts.max_elements_per_line;
  print_opts.compact = opts.compact;

  for (size_t i = 0; i < samples->size(); ++i) {
    const auto& sample = (*samples)[i];
    WriteIndent(os, depth + 1, opts.indent);
    // Format the time into a stack buffer (no per-sample std::string alloc);
    // kDtoaBufSize is dtos_to's buffer contract (SIMD fast-path overshoot).
    char time_buf[kDtoaBufSize];
    const size_t tlen = dtos_to(time_buf, sample.first);
    os.write(time_buf, tlen);
    os << ": ";

    const Value* val = spec.time_sample_value(sample.second);
    if (val && val->is_block()) {
      os << "None";  // a blocked sample (`123: None`)
    } else if (val) {
      // Stream large arrays directly; lazy crate-backed POD arrays can be
      // borrowed without decoding, while unsupported encodings fall back to a
      // transient materialized value inside the value printer. In a parallel
      // build a non-trivial sampled array is offloaded like a default value
      // (animated points would otherwise be formatted by the one build worker).
      if (segsink && val->is_array() &&
          ArrayElementCount(*val) >= kOffloadMinElems) {
        segsink->offload_array(val);
      } else {
        PrintValue(os, *val, print_opts);
      }
    } else {
      os << "None";
    }
    os << ",\n";
  }

  WriteIndent(os, depth, opts.indent);
  os << "}";  // caller appends optional metadata block + newline
}

// Write a property value
void WriteProperty(StreamWriter& os, const PropSlot& slot, const PrimSpec& spec,
                   int depth, const USDAWriteOptions& opts,
                   SegmentSink* segsink) {
  PropNameTable& name_table = GetPropNameTable();
  const std::string name(name_table.get(slot.name_id));

  if (const std::string* raw = spec.raw_default_source(slot.name_id)) {
    WriteIndent(os, depth, opts.indent);
    if (opts.emit_custom && slot.is_custom()) os << "custom ";
    if (slot.is_uniform()) os << "uniform ";
    const std::string* decl = spec.property_type_name(slot.name_id);
    os << (decl ? *decl : std::string("opaque")) << " " << name << " = "
       << *raw;
    WritePropMeta(os, spec, slot.name_id, depth, opts);
    os << "\n";
    return;
  }

  if (const std::string* spline = spec.spline_source(slot.name_id)) {
    WriteIndent(os, depth, opts.indent);
    if (opts.emit_custom && slot.is_custom()) os << "custom ";
    if (slot.is_uniform()) os << "uniform ";
    if (const std::string* decl = spec.property_type_name(slot.name_id)) {
      os << *decl;
    } else {
      const char* tn = GetTypeName(static_cast<TypeId>(slot.value_type));
      os << (tn ? tn : "double");
    }
    os << " " << name << ".spline = " << *spline << "\n";
    return;
  }

  // Type name. Prefer the declared name (round-trips string[] vs token[],
  // role types, and value-less / connection-only attrs whose stored value type
  // is Invalid). GetTypeName returns nullptr for Invalid, so guard the fallback.
  TypeId type_id = static_cast<TypeId>(slot.value_type);
  std::string type_name;
  if (const std::string* decl = spec.property_type_name(slot.name_id)) {
    type_name = *decl;
  } else {
    const char* tn = GetTypeName(type_id);
    type_name = tn ? tn : "token";
    if (slot.is_array()) type_name += "[]";
  }

  // Emit the leading `<indent><qualifiers><type> <name>` shared by the value
  // line, the `.timeSamples` line and the `.connect` line (USDA repeats the
  // type on each statement).
  auto emit_decl = [&](const char* prefix = "") {
    WriteIndent(os, depth, opts.indent);
    os << prefix;
    // `custom` is a deprecated qualifier; emit only under opt-in (--openusd-compat).
    if (opts.emit_custom && slot.is_custom()) os << "custom ";
    if (slot.is_uniform()) os << "uniform ";
    os << type_name << " " << name;
  };

  // Connection targets live in the prim's connection map, NOT in the property
  // value (a connection-only attr has no authored default). A property may
  // carry a value AND time samples AND a connection -> separate statements.
  // A present-but-empty entry is an authored connection BLOCK
  // (`.connect = None`); absent means no connection opinion.
  const std::vector<Path>* conns = spec.connection(name);
  bool has_conn = conns != nullptr;
  // Composed-stage output: an authored connection BLOCK (`.connect = None`)
  // resolved to "no connection"; pxr flatten drops the statement entirely
  // (value blocks, by contrast, are preserved). Qualified edits keep their
  // statement.
  if (opts.composed_stage_output && has_conn && conns->empty()) {
    const ArcListOp* cedit = spec.connection_edit(name);
    if (!(cedit && cedit->authored && !cedit->is_explicit)) has_conn = false;
  }

  // Connection statement: `<type> <name>.connect = </path>` (or `[...]`).
  // `wrote_meta` = authored property metadata already emitted on an earlier
  // statement for this property.
  auto emit_connection = [&](bool wrote_meta) {
    const ArcListOp* edit = spec.connection_edit(name);
    auto emit_targets = [&](const std::vector<std::string>& targets) {
      if (targets.empty()) {
        os << "None";
      } else if (targets.size() == 1) {
        os << "<" << targets[0] << ">";
      } else {
        os << "[";
        for (size_t i = 0; i < targets.size(); ++i) {
          if (i) os << ", ";
          os << "<" << targets[i] << ">";
        }
        os << "]";
      }
    };
    if (edit && edit->authored && !edit->is_explicit) {
      auto emit_connection_op = [&](const char* qualifier,
                                    const std::vector<std::string>& targets) {
        if (targets.empty()) return;
        emit_decl(qualifier);
        os << ".connect = ";
        emit_targets(targets);
        if (!wrote_meta) {
          WritePropMeta(os, spec, slot.name_id, depth, opts);
          wrote_meta = true;
        }
        os << "\n";
      };
      emit_connection_op("add ", edit->added);
      emit_connection_op("prepend ", edit->prepended);
      emit_connection_op("append ", edit->appended);
      emit_connection_op("delete ", edit->deleted);
      emit_connection_op("reorder ", edit->ordered);
    } else {
      emit_decl();
      os << ".connect = ";
      std::vector<std::string> targets;
      targets.reserve(conns->size());
      for (const Path& path : *conns) targets.push_back(path.str());
      emit_targets(targets);
      if (!wrote_meta) WritePropMeta(os, spec, slot.name_id, depth, opts);
      os << "\n";
    }
  };

  // Check if this property has time samples
  if (slot.is_time_sampled() && spec.has_time_samples(slot.name_id)) {
    auto emit_ts_decl = [&]() { emit_decl(); };
    // An authored DEFAULT coexists with time samples as an independent
    // field (pxr keeps both; dropping it loses the value used outside the
    // sampled range / by consumers that ignore samples). Emit it first.
    const Value* def_val = spec.property_value(slot.name_id);
    const PropMeta* pm = spec.property_meta(slot.name_id);
    if (def_val && !def_val->is_empty()) {
      emit_ts_decl();
      if (def_val->is_block()) {
        os << " = None";
      } else {
        os << " = ";
        PrintOptions po;
        po.float_precision = opts.float_precision;
        po.double_precision = opts.double_precision;
        po.indent = opts.indent;
        // Offload only when this statement's print options match the array
        // options the offload workers use (they differ in the element
        // limit/compact fields, which this statement leaves at defaults).
        if (segsink && def_val->is_array() &&
            ArrayElementCount(*def_val) >= kOffloadMinElems &&
            po.max_array_elements == opts.max_elements_per_line &&
            po.compact == opts.compact) {
          segsink->offload_array(def_val);
        } else {
          PrintValue(os, *def_val, po);
        }
      }
      WritePropMeta(os, spec, slot.name_id, depth, opts);
      os << "\n";
    } else if (pm && !pm->empty()) {
      // USDA forbids metadata after a `.timeSamples` block: emit authored
      // metadata on a bare declaration line first (usdcat form).
      emit_ts_decl();
      WritePropMeta(os, spec, slot.name_id, depth, opts);
      os << "\n";
    }
    WriteTimeSamples(os, name, slot.name_id, spec, depth, opts, segsink);
    os << "\n";
    // A connection coexists with time samples (pxr emits it as a third
    // statement after the `.timeSamples` block); dropping it here silently
    // severed shading networks with animated fallback values.
    if (has_conn) {
      emit_connection(/*wrote_meta=*/(def_val && !def_val->is_empty()) ||
                      (pm && !pm->empty()));
    }
    return;
  }

  const Value* value = spec.property_value(slot.name_id);
  const bool has_value = value && !value->is_empty();

  // Value statement (authored default).
  if (has_value && value->is_block()) {
    // Authored value block (`= None`): a typed opinion with no data that blocks
    // weaker values. Emit it verbatim (the declared type carries the type name).
    emit_decl();
    os << " = None";
    WritePropMeta(os, spec, slot.name_id, depth, opts);
    os << "\n";
  } else if (has_value) {
    emit_decl();
    PrintOptions print_opts;
    print_opts.float_precision = opts.float_precision;
    print_opts.double_precision = opts.double_precision;
    print_opts.max_array_elements = opts.max_elements_per_line;
    print_opts.compact = opts.compact;
    os << " = ";
    // Parallel build: offload a non-trivial array value to the worker pool, which
    // emits its complete `[ ... ]` (byte-identical to the inline PrintValue). Tiny
    // arrays and non-array values are formatted inline.
    if (segsink && value->is_array() &&
        ArrayElementCount(*value) >= kOffloadMinElems) {
      segsink->offload_array(value);
    } else {
      PrintValue(os, *value, print_opts);
    }
    WritePropMeta(os, spec, slot.name_id, depth, opts);
    os << "\n";
  }

  // Connection statement: `<type> <name>.connect = </path>` (or `[...]`).
  if (has_conn) {
    emit_connection(/*wrote_meta=*/has_value);
  }

  // Declared-only attribute (no value, no connection): emit the bare
  // declaration so typed-only / namespace-declared props round-trip.
  if (!has_value && !has_conn) {
    emit_decl();
    WritePropMeta(os, spec, slot.name_id, depth, opts);
    os << "\n";
  }
}

// Write relationship
void WriteRelationship(StreamWriter& os, const std::string& name,
                       const std::vector<Path>& targets, const PrimSpec& spec,
                       PropNameId name_id, int depth,
                       const USDAWriteOptions& opts) {
  const bool is_custom =
      (spec.relationship_flags(name) & PropSlot::kFlagCustom) != 0;
  const uint16_t relationship_flags = spec.relationship_flags(name);
  auto head = [&]() {
    WriteIndent(os, depth, opts.indent);
    if (opts.emit_custom && is_custom) os << "custom ";
    // Uniform is the relationship DEFAULT: pxr only ever prints `varying rel`
    // (an authored-uniform variability field, e.g. from crate, stays silent).
    if ((relationship_flags & PropSlot::kFlagVariabilityAuthored) &&
        (relationship_flags & PropSlot::kFlagVarying)) {
      os << "varying ";
    }
    os << "rel " << name;
  };
  auto targets_text = [&](const std::vector<std::string>& tgts) {
    if (tgts.size() == 1) {
      os << " = <" << tgts[0] << ">";
    } else {
      os << " = [\n";
      for (const auto& t : tgts) {
        WriteIndent(os, depth + 1, opts.indent);
        os << "<" << t << ">,\n";
      }
      WriteIndent(os, depth, opts.indent);
      os << "]";
    }
  };

  // Authored list-op edits re-emit their qualifiers (a bare explicit list
  // changes composition semantics for delete/prepend against weaker layers).
  const ArcListOp* re = nullptr;
  bool explicit_empty = false;
  bool value_block = false;
  {
    const auto& edits = spec.relationship_edits();
    const auto it = edits.find(name);
    if (it != edits.end() && it->second.authored) {
      if (!it->second.is_explicit) re = &it->second;
      else if (targets.empty()) {
        explicit_empty = true;
        value_block = it->second.is_value_block;
      }
    }
  }
  if (re) {
    bool first = true;
    auto qual_line = [&](const char* qual,
                         const std::vector<std::string>& items) {
      if (items.empty()) return;
      WriteIndent(os, depth, opts.indent);
      if (opts.emit_custom && is_custom && first) os << "custom ";
      os << qual << "rel " << name;
      targets_text(items);
      if (first) WritePropMeta(os, spec, name_id, depth, opts);
      os << "\n";
      first = false;
    };
    qual_line("add ", re->added);
    qual_line("prepend ", re->prepended);
    qual_line("append ", re->appended);
    qual_line("delete ", re->deleted);
    qual_line("reorder ", re->ordered);
    if (first) {  // authored but empty edit: keep the declaration
      head();
      WritePropMeta(os, spec, name_id, depth, opts);
      os << "\n";
    }
    return;
  }

  head();
  if (targets.empty()) {
    // Composed-stage output: an explicit-None (block) relationship resolved
    // to "no targets"; pxr flatten writes the bare declaration.
    if (explicit_empty && !opts.composed_stage_output)
      os << (value_block ? " = None" : " = []");
    // Otherwise this is a declared-only relationship: bare `rel name` (pxr
    // re-parses it without an authored targetPaths opinion).
  } else {
    std::vector<std::string> tgts;
    tgts.reserve(targets.size());
    for (const auto& t : targets) tgts.push_back(t.str());
    targets_text(tgts);
  }
  WritePropMeta(os, spec, name_id, depth, opts);
  os << "\n";
}

// Forward declaration

// Emit `variantSet "name" = { "opt" (meta) { body } ... }` blocks at `depth`.
// Bodies merge two representations: inline PrimVariantData (USDA parse /
// authoring: properties, relationships, arcs, nested sets, content sub-layer)
// and bracketed holder prims in `layer` (crate representation). When both
// exist (MergeVariantData pattern), inline opinions win and the holder
// contributes only what the inline data lacks.
void WriteVariantSets(StreamWriter& os,
                      const std::vector<PrimVariantSetData>& sets,
                      const Layer& layer, const std::string& initial_owner_path,
                      int initial_depth, const USDAWriteOptions& opts,
                      SegmentSink* segsink) {
  enum class VariantPhase {
    BeginSet,
    BeginVariant,
    AfterInlineNested,
    AfterHolderNested,
  };
  struct VariantFrame {
    const std::vector<PrimVariantSetData>* sets;
    size_t owner_path_size;
    int depth;
    size_t set_index{0};
    size_t variant_index{0};
    VariantPhase phase{VariantPhase::BeginSet};
    const PrimSpec* holder{nullptr};
  };

  // Nested variant sets are recursively owned data, but writing them does not
  // need to consume one C++ stack frame per level. The explicit continuation
  // phases preserve the exact inline/holder output order of the recursive
  // implementation. Keep one mutable owner path rather than copying every
  // ancestor path into every frame (which would retain O(depth^2) bytes).
  std::string owner_path = initial_owner_path;
  std::vector<VariantFrame> stack;
  stack.push_back(VariantFrame{&sets, owner_path.size(), initial_depth});
  while (!stack.empty()) {
    VariantFrame& frame = stack.back();
    if (frame.phase == VariantPhase::BeginSet) {
      while (frame.set_index < frame.sets->size() &&
             (*frame.sets)[frame.set_index].variants.empty()) {
        ++frame.set_index;
      }
      if (frame.set_index >= frame.sets->size()) {
        stack.pop_back();
        continue;
      }

      const PrimVariantSetData& vs = (*frame.sets)[frame.set_index];
      // A declaration-only variant set (`prepend variantSets = "v"` with no
      // local content / options) is emitted only as the `variantSets`
      // declaration, not as an empty `variantSet "v" = {}` block — matching
      // pxr, which never emits the empty block.
      os << "\n";
      WriteIndent(os, frame.depth, opts.indent);
      os << "variantSet " << EscapeUSDAString(vs.name) << " = {\n";
      frame.variant_index = 0;
      frame.phase = VariantPhase::BeginVariant;
      continue;
    }

    const PrimVariantSetData& vs = (*frame.sets)[frame.set_index];
    if (frame.phase == VariantPhase::BeginVariant) {
      if (frame.variant_index >= vs.variants.size()) {
        WriteIndent(os, frame.depth, opts.indent);
        os << "}\n";
        ++frame.set_index;
        frame.phase = VariantPhase::BeginSet;
        continue;
      }

      const PrimVariantData& var = vs.variants[frame.variant_index];
      const int depth = frame.depth;
      owner_path.resize(frame.owner_path_size);
      owner_path += "/{" + vs.name + "=" + var.name + "}";
      frame.holder = layer.prim_at_path(owner_path);
      const PrimSpec* holder = frame.holder;

      WriteIndent(os, depth + 1, opts.indent);
      os << EscapeUSDAString(var.name);

      // Variant OPTION metadata: composition arcs / active / hidden / doc
      // authored on the option itself (inline or on the holder prim).
      const PrimMetadata* hmeta = holder ? &holder->meta() : nullptr;
      auto pick_arcs = [&](const std::vector<std::string>& inline_arcs,
                           const std::vector<std::string>* holder_arcs)
          -> const std::vector<std::string>& {
        static const std::vector<std::string> kEmpty;
        if (!inline_arcs.empty()) return inline_arcs;
        return holder_arcs ? *holder_arcs : kEmpty;
      };
      const std::vector<std::string>& o_refs =
          pick_arcs(var.references, hmeta ? &hmeta->references.get() : nullptr);
      const std::vector<std::string>& o_pls =
          pick_arcs(var.payloads, hmeta ? &hmeta->payloads.get() : nullptr);
      const std::vector<std::string>& o_inh =
          pick_arcs(var.inherits, hmeta ? &hmeta->inherits.get() : nullptr);
      const std::vector<std::string>& o_spz =
          pick_arcs(var.specializes, hmeta ? &hmeta->specializes.get() : nullptr);
      const bool o_inactive = !var.active || (hmeta && !hmeta->active);
      const bool o_hidden = var.hidden || (hmeta && hmeta->hidden);
      // Authored `hidden = false` is a real opinion that must round-trip.
      const bool o_hidden_false =
          !o_hidden && ((var.hidden_authored) ||
                        (hmeta && hmeta->hidden_authored));
      const std::string& o_doc =
          !var.doc.empty() ? var.doc : (hmeta ? hmeta->doc() : var.doc);
      // Nested variant SELECTIONS: authored on the option's own metadata
      // (`"o1" ( variants = { string inner = "i2" } )`). Gather from the
      // inline nested sets, or the holder's meta for crate-loaded layers.
      std::vector<std::pair<std::string, std::string>> o_sels;
      {
        o_sels = var.variantSelections;
        const std::vector<PrimVariantSetData>& nsets =
            !var.variantSets.empty()
                ? var.variantSets
                : (hmeta ? hmeta->variantSets() : var.variantSets);
        for (const PrimVariantSetData& nvs : nsets) {
          if (!nvs.selected.empty()) {
            bool have = false;
            for (const auto& existing : o_sels) {
              if (existing.first == nvs.name) { have = true; break; }
            }
            if (!have) o_sels.emplace_back(nvs.name, nvs.selected);
          }
        }
        if (hmeta) {
          for (const auto& kv : hmeta->variantSelections()) {
            bool have = false;
            for (auto& e : o_sels) {
              if (e.first == kv.first) { have = true; break; }
            }
            if (!have && !kv.second.empty()) o_sels.push_back(kv);
          }
        }
      }
      // Unknown (unmodeled) option metadata: inline (USDA-parsed) or from
      // the materialized holder prim (crate-read layers).
      const std::vector<std::pair<std::string, std::string>>& o_unknown =
          !var.unknownMeta.empty()
              ? var.unknownMeta
              : (hmeta ? hmeta->unknownMeta() : var.unknownMeta);
      const bool has_opt_meta = !o_refs.empty() || !o_pls.empty() ||
                                !o_inh.empty() || !o_spz.empty() ||
                                o_inactive || o_hidden || o_hidden_false ||
                                !o_sels.empty() ||
                                !o_unknown.empty() ||
                                (!o_doc.empty() && opts.include_comments);
      if (has_opt_meta) {
        os << " (\n";
        auto arc_line = [&](const char* keyword,
                            const std::vector<std::string>& arcs) {
          for (const std::string& a : arcs) {
            WriteIndent(os, depth + 2, opts.indent);
            os << "prepend " << keyword << " = " << FormatArcRef(a) << "\n";
          }
        };
        arc_line("references", o_refs);
        arc_line("payload", o_pls);
        arc_line("inherits", o_inh);
        arc_line("specializes", o_spz);
        if (o_inactive) {
          WriteIndent(os, depth + 2, opts.indent);
          os << "active = false\n";
        }
        if (o_hidden) {
          WriteIndent(os, depth + 2, opts.indent);
          os << "hidden = true\n";
        } else if (o_hidden_false) {
          WriteIndent(os, depth + 2, opts.indent);
          os << "hidden = false\n";
        }
        if (!o_doc.empty() && opts.include_comments) {
          WriteIndent(os, depth + 2, opts.indent);
          os << "doc = " << EscapeUSDAString(o_doc) << "\n";
        }
        if (!o_sels.empty()) {
          WriteIndent(os, depth + 2, opts.indent);
          os << "variants = {\n";
          for (const auto& kv : o_sels) {
            WriteIndent(os, depth + 3, opts.indent);
            os << "string " << kv.first << " = " << EscapeUSDAString(kv.second)
               << "\n";
          }
          WriteIndent(os, depth + 2, opts.indent);
          os << "}\n";
        }
        // Unknown metadata re-emitted verbatim (raw authored value text).
        for (const auto& um : o_unknown) {
          WriteIndent(os, depth + 2, opts.indent);
          os << um.first << " = " << um.second << "\n";
        }
        WriteIndent(os, depth + 1, opts.indent);
        os << ")";
      }
      os << " {\n";

      PrintOptions vpopts;
      vpopts.float_precision = opts.float_precision;
      vpopts.double_precision = opts.double_precision;
      for (const VariantProperty& vp : var.properties) {
        if (vp.value.is_empty()) continue;  // unknown-typed placeholder
        WriteIndent(os, depth + 2, opts.indent);
        if (opts.emit_custom && (vp.flags & PropSlot::kFlagCustom)) {
          os << "custom ";
        }
        if (vp.flags & PropSlot::kFlagUniform) os << "uniform ";
        const char* tn = GetTypeName(vp.value.type_id());
        os << (tn ? tn : "token");
        if (vp.value.is_array()) os << "[]";
        os << " " << vp.name << " = ";
        PrintValue(os, vp.value, vpopts);
        os << "\n";
      }
      for (const auto& rel : var.relationships) {
        WriteIndent(os, depth + 2, opts.indent);
        os << "rel " << rel.first;
        if (rel.second.size() == 1) {
          os << " = <" << rel.second[0].str() << ">";
        } else if (!rel.second.empty()) {
          os << " = [";
          for (size_t t = 0; t < rel.second.size(); ++t) {
            if (t) os << ", ";
            os << "<" << rel.second[t].str() << ">";
          }
          os << "]";
        }
        os << "\n";
      }

      // Nested inline sets run before content/holder contributions. Save the
      // continuation before pushing because vector growth invalidates `frame`.
      frame.phase = VariantPhase::AfterInlineNested;
      if (!var.variantSets.empty()) {
        stack.push_back(
            VariantFrame{&var.variantSets, owner_path.size(), depth + 2});
      }
      continue;
    }

    const PrimVariantData& var = vs.variants[frame.variant_index];
    const int depth = frame.depth;
    const PrimSpec* holder = frame.holder;

    if (frame.phase == VariantPhase::AfterInlineNested) {

      // Variant CHILD prims + holder extras. Inline content sub-layer first
      // (USDA representation), then holder contributions not already covered
      // by the inline data (crate representation / merged pattern).
      if (var.content) {
        if (const PrimSpec* self = var.content->prim_at_path("/__self__")) {
          // The content root's OWN opinions (time-sampled attributes routed
          // here by the parser, connections, ...) belong in the option body
          // too — only its non-time-sampled defaults are already covered by
          // the inline VariantProperty list.
          auto has_inline = [&](const std::string& name) {
            for (const VariantProperty& vp : var.properties) {
              if (vp.name == name) return true;
            }
            return false;
          };
          PropNameTable& stable = GetPropNameTable();
          for (const PropSlot& sslot : self->properties().slots()) {
            if (sslot.is_relationship()) continue;
            if (!sslot.is_time_sampled() &&
                has_inline(std::string(stable.get(sslot.name_id)))) {
              continue;
            }
            WriteProperty(os, sslot, *self, depth + 2, opts, segsink);
          }
          for (uint32_t ci : self->child_indices()) {
            const PrimSpec* child = var.content->prim(ci);
            if (!child) continue;
            os << "\n";
            WritePrimSpec(os, *child, *var.content, depth + 2, opts, segsink);
          }
        }
      }
      if (holder) {
        auto has_inline_prop = [&](const std::string& name) {
          for (const VariantProperty& vp : var.properties) {
            if (vp.name == name) return true;
          }
          return false;
        };
        PropNameTable& htable = GetPropNameTable();
        for (const PropSlot& hslot : holder->properties().slots()) {
          if (hslot.is_relationship()) continue;
          if (has_inline_prop(std::string(htable.get(hslot.name_id)))) continue;
          WriteProperty(os, hslot, *holder, depth + 2, opts, segsink);
        }
        for (const std::string& rel_name : holder->relationship_names()) {
          if (var.relationships.find(rel_name) != var.relationships.end()) {
            continue;
          }
          const std::vector<Path>* targets = holder->relationship(rel_name);
          if (!targets) continue;
          WriteRelationship(os, rel_name, *targets, *holder,
                            htable.find(rel_name), depth + 2, opts);
        }
        // Holder-side nested sets (crate representation) run after the
        // holder's own properties/relationships and before ordinary children.
        frame.phase = VariantPhase::AfterHolderNested;
        if (var.variantSets.empty() && !holder->meta().variantSets().empty()) {
          stack.push_back(VariantFrame{&holder->meta().variantSets(),
                                       owner_path.size(), depth + 2});
          continue;
        }
      }
      frame.phase = VariantPhase::AfterHolderNested;
      continue;
    }

    if (frame.phase == VariantPhase::AfterHolderNested) {
      if (holder) {
        for (uint32_t ci : holder->child_indices()) {
          const PrimSpec* child = layer.prim(ci);
          if (!child) continue;
          const std::string& cn = child->name();
          if (cn.size() >= 2 && cn.front() == '{' && cn.back() == '}') {
            continue;  // nested holder: emitted via variantSets above
          }
          os << "\n";
          WritePrimSpec(os, *child, layer, depth + 2, opts, segsink);
        }
      }
      WriteIndent(os, depth + 1, opts.indent);
      os << "}\n";
      owner_path.resize(frame.owner_path_size);
      frame.holder = nullptr;
      ++frame.variant_index;
      frame.phase = VariantPhase::BeginVariant;
      continue;
    }
  }
}

// Write one prim's header and body opinions, but not its ordinary children or
// closing brace. WritePrimSpec owns hierarchy traversal so API-created deep
// layers do not consume one C++ stack frame per prim.
void WritePrimSpecOpen(StreamWriter& os, const PrimSpec& spec,
                       const Layer& layer, int depth,
                       const USDAWriteOptions& opts, SegmentSink* segsink) {
  // Write prim definition line
  WriteIndent(os, depth, opts.indent);
  os << SpecifierKeyword(spec.specifier());

  const std::string& type_name = spec.type_name();
  if (!type_name.empty()) {
    os << " " << type_name;
  }

  os << " " << EscapeUSDAString(spec.name());

  // All prim metadata goes in the parenthesized header section so it re-parses
  // (the parser reads prim metadata from `( ... )`, never from the prim body).
  const PrimMetadata& meta = spec.meta();
  const bool has_doc = (meta.doc_authored() || !meta.doc().empty()) &&
                       opts.include_comments;
  const bool has_comment =
      (meta.comment_authored() || !meta.comment().empty()) &&
      opts.include_comments;
  auto has_dict = [](const Value& v) {
    return v.is_dictionary() && v.as_dictionary() && !v.as_dictionary()->empty();
  };
  const bool has_customData =
      meta.customDataAuthored() || has_dict(meta.customData());
  const bool has_assetInfo =
      meta.assetInfoAuthored() || has_dict(meta.assetInfo());
  const bool has_sdr =
      meta.sdrMetadataAuthored() || has_dict(meta.sdrMetadata());
  const bool has_clips = meta.clipsAuthored() || has_dict(meta.clips());
  const bool has_clip_set_edits = meta.clipSetEdits().authored;
  // An authored arc EDIT with empty inline lists still needs the metadata
  // block: explicit-clear (`references = None`) has no items but is a real
  // opinion.
  const ArcListOps* arc_edits = meta.arc_edits();
  const bool has_arc_edits =
      arc_edits && (arc_edits->references.has_authored_opinion() ||
                    arc_edits->payloads.has_authored_opinion() ||
                    arc_edits->inherits.has_authored_opinion() ||
                    arc_edits->specializes.has_authored_opinion());
  const StringListOps& variant_edits = meta.variantSetNameEdits();
  const bool has_variant_edits =
      variant_edits.authored &&
      (variant_edits.is_explicit ? !variant_edits.explicit_items.empty()
                                 : variant_edits.has_nonexplicit_items());
  bool has_meta = !meta.active || meta.active_authored || meta.hidden ||
                  meta.hidden_authored || meta.instanceable ||
                  meta.instanceable_authored || !meta.permission().empty() ||
                  meta.kindAuthored() || meta.displayNameAuthored() ||
                  meta.displayGroupOrderAuthored() ||
                  !meta.kind().empty() || !meta.displayName().empty() ||
                  has_doc || has_comment || meta.apiSchemasAuthored() ||
                  !meta.apiSchemas().empty() ||
                  has_customData || has_assetInfo || has_sdr || has_clips ||
                  has_clip_set_edits ||
                  !meta.references.empty() || !meta.payloads.empty() ||
                  !meta.inherits.empty() || !meta.specializes.empty() ||
                  has_arc_edits || meta.relocatesAuthored() ||
                  !meta.relocates().empty() ||
                  meta.variantSelectionsAuthored() ||
                  !meta.variantSelections().empty() ||
                  !meta.variantSelection.empty() ||
                  has_variant_edits || !meta.variantSets().empty() ||
                  !meta.unknownMeta().empty() || !meta.unknownFields().empty();

  if (has_meta) {
    const int md = depth + 1;
    os << " (\n";
    auto kv = [&](const std::string& s) {
      if (s.empty()) return;  // e.g. DictMetaLine of an empty dict
      WriteIndent(os, md, opts.indent);
      os << s << "\n";
    };
    if (!meta.active) kv("active = false");
    else if (meta.active_authored) kv("active = true");
    if (meta.hidden) kv("hidden = true");
    else if (meta.hidden_authored) kv("hidden = false");
    if (meta.instanceable) kv("instanceable = true");
    else if (meta.instanceable_authored) kv("instanceable = false");
    if (!meta.permission().empty()) kv("permission = " + meta.permission());
    if (meta.kindAuthored() || !meta.kind().empty())
      kv("kind = " + EscapeUSDAString(meta.kind()));
    if (meta.displayNameAuthored() || !meta.displayName().empty())
      kv("displayName = " + EscapeUSDAString(meta.displayName()));
    if (meta.displayGroupOrderAuthored() ||
        !meta.displayGroupOrder().empty()) {
      std::string value = "displayGroupOrder = [";
      for (size_t i = 0; i < meta.displayGroupOrder().size(); ++i) {
        if (i) value += ", ";
        value += EscapeUSDAString(meta.displayGroupOrder()[i]);
      }
      value += "]";
      kv(value);
    }
    if (has_doc) kv("doc = " + EscapeUSDAString(meta.doc()));
    // Bare string literal = comment (pxr 26.x rejects `comment = "..."`).
    if (has_comment) kv(EscapeUSDAString(meta.comment()));
    if (meta.apiSchemasAuthored() || !meta.apiSchemas().empty()) {
      const StringListOps& edits = meta.apiSchemaEdits();
      auto write_api_op = [&](const char* qualifier,
                              const std::vector<std::string>& schemas) {
        if (schemas.empty()) return;
        std::string s = qualifier;
        s += "apiSchemas = [";
        for (size_t i = 0; i < schemas.size(); ++i) {
          if (i) s += ", ";
          s += EscapeUSDAString(schemas[i]);
        }
        s += "]";
        kv(s);
      };
      if (edits.authored) {
        if (opts.composed_stage_output && !edits.is_explicit) {
          // Composed-stage output: the residual list-op has nothing weaker
          // left to edit, so publish its composed result as an EXPLICIT list
          // (pxr UsdStage::Flatten parity).
          const std::vector<std::string> composed =
              ApplyStringListOp(edits, {});
          if (composed.empty()) kv("apiSchemas = None");
          else write_api_op("", composed);
        } else if (edits.is_explicit) {
          if (edits.explicit_items.empty()) kv("apiSchemas = None");
          else write_api_op("", edits.explicit_items);
        } else {
          write_api_op("delete ", edits.deleted);
          write_api_op("add ", edits.added);
          write_api_op("prepend ", edits.prepended);
          write_api_op("append ", edits.appended);
          write_api_op("reorder ", edits.ordered);
        }
      } else {
        std::string qualifier;
        if (!opts.composed_stage_output && !meta.apiSchemasQualifier().empty()) {
          qualifier = meta.apiSchemasQualifier() + " ";
        }
        write_api_op(qualifier.c_str(), meta.apiSchemas());
        if (meta.apiSchemas().empty()) kv("apiSchemas = None");
      }
    }
    if (meta.relocatesAuthored() && meta.relocates().empty()) {
      kv("relocates = {}");
    } else if (!meta.relocates().empty()) {
      WriteIndent(os, md, opts.indent);
      os << "relocates = {\n";
      for (const auto& r : meta.relocates()) {
        WriteIndent(os, md + 1, opts.indent);
        os << "<" << r.first << ">: <" << r.second << ">,\n";
      }
      WriteIndent(os, md, opts.indent);
      os << "}\n";
    }
    if (has_customData) kv(DictMetaLine("customData", meta.customData(), md, opts));
    if (has_assetInfo) kv(DictMetaLine("assetInfo", meta.assetInfo(), md, opts));
    if (has_sdr) kv(DictMetaLine("sdrMetadata", meta.sdrMetadata(), md, opts));
    if (has_clips) kv(DictMetaLine("clips", meta.clips(), md, opts));
    if (has_clip_set_edits) {
      const StringListOps& edits = meta.clipSetEdits();
      auto write_clip_set_op = [&](const char* qualifier,
                                   const std::vector<std::string>& names) {
        if (names.empty()) return;
        std::string s = qualifier;
        s += "clipSets = [";
        for (size_t i = 0; i < names.size(); ++i) {
          if (i) s += ", ";
          s += EscapeUSDAString(names[i]);
        }
        s += "]";
        kv(s);
      };
      if (edits.is_explicit) {
        if (edits.explicit_items.empty()) kv("clipSets = None");
        else write_clip_set_op("", edits.explicit_items);
      } else {
        write_clip_set_op("delete ", edits.deleted);
        write_clip_set_op("add ", edits.added);
        write_clip_set_op("prepend ", edits.prepended);
        write_clip_set_op("append ", edits.appended);
        write_clip_set_op("reorder ", edits.ordered);
      }
    }
    // Unknown (unmodeled) metadata preserved by the parser: re-emit the raw
    // authored value text verbatim (it re-parses back into unknownMeta).
    for (const auto& um : meta.unknownMeta()) {
      kv(um.first + " = " + um.second);
    }
    for (const auto& field : meta.unknownFields()) {
      kv(field.name + " = " +
         (field.unregistered && !field.unregistered_source.empty() &&
                  field.value.is_dictionary()
              ? field.unregistered_source
              : PrintValue(field.value, PrintOptions{})));
    }

    // Composition arcs, re-emitting the authored list-op qualifier (Phase 7 S5):
    // a bare/explicit list as `references = [...]`, otherwise the
    // prepend/append/delete sublists. With no recorded edit the inline list is
    // an implicit explicit (bare) list.
    const ArcListOps* edits = meta.arc_edits();
    auto write_arc = [&](const char* field, const std::vector<std::string>& inl,
                         const ArcListOp* e) {
      auto emit = [&](const char* qual, const std::vector<std::string>& items) {
        if (items.empty()) return;
        WriteIndent(os, md, opts.indent);
        // pxr prints a single arc target without list brackets.
        if (items.size() == 1) {
          os << qual << field << " = " << FormatArcRef(items[0]) << "\n";
          return;
        }
        os << qual << field << " = [\n";
        for (const auto& a : items) {
          WriteIndent(os, md + 1, opts.indent);
          os << FormatArcRef(a) << ",\n";
        }
        WriteIndent(os, md, opts.indent);
        os << "]\n";
      };
      if (!e || !e->authored || e->is_explicit) {
        if (e && e->authored && e->is_explicit && inl.empty()) {
          // Authored explicit-clear (`references = None`).
          WriteIndent(os, md, opts.indent);
          os << field << " = None\n";
        } else {
          emit("", inl);  // bare/explicit list
        }
      } else {
        emit("add ", e->added);
        emit("prepend ", e->prepended);
        emit("append ", e->appended);
        emit("delete ", e->deleted);
        emit("reorder ", e->ordered);
      }
    };
    write_arc("references", meta.references,
              edits ? &edits->references : nullptr);
    write_arc("payload", meta.payloads, edits ? &edits->payloads : nullptr);
    write_arc("inherits", meta.inherits, edits ? &edits->inherits : nullptr);
    write_arc("specializes", meta.specializes,
              edits ? &edits->specializes : nullptr);

    // Variant selections: `variants = { string <set> = "<sel>" }`.
    auto write_variants = [&](const std::vector<std::pair<std::string, std::string>>& sels) {
      WriteIndent(os, md, opts.indent);
      os << "variants = {\n";
      for (const auto& sel : sels) {
        WriteIndent(os, md + 1, opts.indent);
        os << "string " << sel.first << " = " << EscapeUSDAString(sel.second) << "\n";
      }
      WriteIndent(os, md, opts.indent);
      os << "}\n";
    };
    {
      // Merge every selection source (per-set `selected` is authoritative,
      // then the plural list, then the legacy single string): a crate-read
      // multi-set prim may carry only ONE set in the legacy string, and a
      // USDA-parsed prim carries selections only in the plural list.
      std::vector<std::pair<std::string, std::string>> sels;
      auto add_sel = [&sels](const std::string& set, const std::string& v) {
        if (set.empty()) return;
        for (auto& kv : sels) {
          if (kv.first == set) return;  // first writer wins
        }
        sels.emplace_back(set, v);
      };
      for (const auto& vs : meta.variantSets()) {
        if (!vs.selected.empty()) add_sel(vs.name, vs.selected);
      }
      for (const auto& kv : meta.variantSelections()) {
        add_sel(kv.first, kv.second);
      }
      if (!meta.variantSelection.empty()) {
        auto eq = meta.variantSelection.find('=');
        if (eq != std::string::npos) {
          add_sel(meta.variantSelection.substr(0, eq),
                  meta.variantSelection.substr(eq + 1));
        }
      }
      if (!sels.empty()) {
        write_variants(sels);
      } else if (meta.variantSelectionsAuthored()) {
        kv("variants = {}");
      }
    }

    // Variant set declarations retain their exact authored SdfStringListOp
    // sublists when USDA has a spelling for them. Explicit-empty is valid in
    // USDC/API state but has no valid USDA text spelling and is omitted here.
    if (has_variant_edits) {
      auto write_variant_op = [&](const char* qualifier,
                                  const std::vector<std::string>& names) {
        if (names.empty()) return;
        WriteIndent(os, md, opts.indent);
        os << qualifier << "variantSets = [";
        for (size_t i = 0; i < names.size(); ++i) {
          if (i) os << ", ";
          os << EscapeUSDAString(names[i]);
        }
        os << "]\n";
      };
      if (variant_edits.is_explicit) {
        write_variant_op("", variant_edits.explicit_items);
      } else {
        write_variant_op("delete ", variant_edits.deleted);
        write_variant_op("add ", variant_edits.added);
        write_variant_op("prepend ", variant_edits.prepended);
        write_variant_op("append ", variant_edits.appended);
        write_variant_op("reorder ", variant_edits.ordered);
      }
    } else if (!variant_edits.authored && !meta.variantSets().empty()) {
      WriteIndent(os, md, opts.indent);
      os << "prepend variantSets = [";
      for (size_t i = 0; i < meta.variantSets().size(); ++i) {
        if (i) os << ", ";
        os << EscapeUSDAString(meta.variantSets()[i].name);
      }
      os << "]\n";
    }

    WriteIndent(os, depth, opts.indent);
    os << ")";
  }

  os << "\n";
  WriteIndent(os, depth, opts.indent);
  os << "{\n";

  int content_depth = depth + 1;

  auto write_order = [&](const char* field,
                         const std::vector<std::string>& names) {
    if (names.empty()) return;
    WriteIndent(os, content_depth, opts.indent);
    os << "reorder " << field << " = [";
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) os << ", ";
      os << EscapeUSDAString(names[i]);
    }
    os << "]\n";
  };
  write_order("properties", meta.propertyOrder());
  write_order("nameChildren", meta.primOrder());

  // Get property slots
  std::vector<const PropSlot*> prop_slots;
  for (const auto& slot : spec.properties().slots()) {
    if (!slot.is_relationship()) {
      prop_slots.push_back(&slot);
    }
  }

  // Optionally sort by name
  if (opts.sort_properties) {
    PropNameTable& name_table = GetPropNameTable();
    std::sort(prop_slots.begin(), prop_slots.end(),
              [&name_table](const PropSlot* a, const PropSlot* b) {
                return name_table.get(a->name_id) < name_table.get(b->name_id);
              });
  }

  // Write properties
  for (const PropSlot* slot : prop_slots) {
    WriteProperty(os, *slot, spec, content_depth, opts, segsink);
  }

  // Write relationships. These live in the relationship map (add_relationship
  // does not create a PropSlot), so iterate the map's names, not prop slots.
  PropNameTable& name_table = GetPropNameTable();
  for (const std::string& rel_name : spec.relationship_names()) {
    const std::vector<Path>* targets = spec.relationship(rel_name);
    if (!targets) continue;
    PropNameId rid = name_table.find(rel_name);
    WriteRelationship(os, rel_name, *targets, spec, rid, content_depth, opts);
  }

  // Write variant set bodies (recursive: options may carry nested sets).
  WriteVariantSets(os, spec.meta().variantSets(), layer, spec.path().str(),
                   content_depth, opts, segsink);

}

// Pre-order hierarchy walk shared by the streaming writer and the parallel
// writer's skeleton pass, so both visit exactly the same prims in the same
// order (including the cycle/depth guards). `open(prim, depth, is_child)` fires
// when a prim is entered (`is_child` = entered from its parent, which the
// writer separates with a blank line); `close(prim, depth)` when it is left.
void WritePrimSpec(StreamWriter& os, const PrimSpec& spec, const Layer& layer,
                   int depth, const USDAWriteOptions& opts,
                   SegmentSink* segsink) {
  WalkPrimSubtree(
      spec, layer, depth, opts,
      [&](const PrimSpec& prim, int d, bool is_child) {
        if (is_child) os << "\n";
        WritePrimSpecOpen(os, prim, layer, d, opts, segsink);
      },
      [&](const PrimSpec&, int d) {
        WriteIndent(os, d, opts.indent);
        os << "}\n";
      });
}

void WriteRootPrimOrder(StreamWriter& os, const LayerMeta& meta) {
  if (meta.rootPrimOrder.empty()) return;
  os << "reorder rootPrims = [";
  for (size_t i = 0; i < meta.rootPrimOrder.size(); ++i) {
    if (i) os << ", ";
    os << EscapeUSDAString(meta.rootPrimOrder[i]);
  }
  os << "]\n\n";
}

// Serialize the layer-stage header metadata + all root prims serially (the
// classic streaming path). Shared by the serial entry and as a fallback.
void WriteStageBodySerial(StreamWriter& os, const Layer& layer,
                          const LayerMeta& meta, const USDAWriteOptions& opts) {
  WriteLayerMeta(os, meta, opts);
  WriteRootPrimOrder(os, meta);
  for (uint32_t root_idx : layer.root_indices()) {
    const PrimSpec* root = layer.prim(root_idx);
    if (root) {
      WritePrimSpec(os, *root, layer, 0, opts);
      os << "\n";
    }
  }
}

}  // namespace next
}  // namespace lightusd
