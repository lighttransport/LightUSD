// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Prim Printer Implementation
//
// Debug pretty-printer. Writes through the freestanding StreamWriter sink (no
// iostream / ostringstream): the string-returning entry points serialize into a
// string-backed StreamWriter, so this whole printer is libc-stdio-free.

#include "prim-printer.hh"
#include "usda-format-utils.hh"
#include "value-printer.hh"
#include "stream-writer.hh"
#include "dtoa.hh"
#include "../strfmt.hh"
#include "../layer/property-index.hh"

#include <unordered_set>
#include <vector>

namespace lightusd {
namespace next {

namespace {

// Get specifier string
const char* GetSpecifierString(PrimSpecifier spec) {
  switch (spec) {
    case PrimSpecifier::Def:   return "def";
    case PrimSpecifier::Over:  return "over";
    case PrimSpecifier::Class: return "class";
    default: return "def";
  }
}

// Escape a string for USDA output (duplicated from usda-writer.cc — debug-only).
std::string EscapeString(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 2);
  result += '"';
  for (char c : s) {
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
          static const char* hexd = "0123456789abcdef";
          result += "\\x";
          result += hexd[uc >> 4];
          result += hexd[uc & 0xf];
        } else {
          result += c;
        }
        break;
      }
    }
  }
  result += '"';
  return result;
}

// ostream's default float formatting is "%.6g" (6 significant digits); match it
// with the freestanding formatter so the debug output is unchanged.
inline std::string G6(double v) { return format_g(v, 6); }

// Print indent
void PrintIndent(StreamWriter& os, int depth, const std::string& indent) {
  for (int i = 0; i < depth; ++i) {
    os << indent;
  }
}

// Print a single property
void PrintProperty(StreamWriter& os, const PropSlot& slot, const PrimSpec& spec,
                   int depth, const PrimPrintOptions& opts) {
  // Get property name
  PropNameTable& name_table = GetPropNameTable();
  const std::string name(name_table.get(slot.name_id));

  // Type name: prefer the declared type recorded on read (preserves role
  // types like `color3f`/`normal3f` and array `[]` that the raw TypeId loses),
  // else synthesize from the slot's value type.
  std::string type_name;
  if (const std::string* tn = spec.property_type_name(name)) {
    type_name = *tn;
  } else {
    type_name = GetTypeName(static_cast<TypeId>(slot.value_type));
    if (slot.is_array()) type_name += "[]";
  }

  // Emit the leading qualifiers + `type name`, shared by the value line and the
  // `.connect` line (USDA repeats the type on each statement).
  auto emit_decl = [&](const std::string& suffix) {
    PrintIndent(os, depth, opts.indent);
    if (slot.is_uniform()) os << "uniform ";
    // `custom` is deprecated; emit only under opt-in.
    if (opts.emit_custom && slot.is_custom()) os << "custom ";
    os << type_name << " " << name << suffix;
  };

  const Value* value =
      opts.print_values ? spec.property_value(slot.name_id) : nullptr;
  const bool has_value = value && !value->is_empty();
  const std::vector<Path>* conns = spec.connection(name);
  const bool has_conn = conns && !conns->empty();

  // Authored default value (a property may carry BOTH a value and a connection
  // — they are separate USDA statements).
  if (has_value) {
    emit_decl(" = ");
    PrintOptions print_opts;
    print_opts.indent = opts.indent;
    os << PrintValue(*value, print_opts) << "\n";
  }

  // Connection target(s): `<type> <name>.connect = </path>` (or `[...]`).
  if (has_conn) {
    emit_decl(".connect = ");
    if (conns->size() == 1) {
      os << "<" << (*conns)[0].str() << ">";
    } else {
      os << "[";
      for (size_t i = 0; i < conns->size(); ++i) {
        if (i) os << ", ";
        os << "<" << (*conns)[i].str() << ">";
      }
      os << "]";
    }
    os << "\n";
  }

  // Declared-only attribute (no value, no connection): emit the bare
  // declaration so connection-targets/typed-only props round-trip.
  if (!has_value && !has_conn) {
    emit_decl("");
    os << "\n";
  }
}

// Print relationships
void PrintRelationships(StreamWriter& os, const PrimSpec& spec,
                        int depth, const PrimPrintOptions& opts) {
  // Note: PrimSpec stores relationships in a private map
  // We need to iterate through properties and check relationship flag
  for (const auto& slot : spec.properties().slots()) {
    if (slot.is_relationship()) {
      PrintIndent(os, depth, opts.indent);

      PropNameTable& name_table = GetPropNameTable();
      const std::string name(name_table.get(slot.name_id));

      // Get targets
      const std::vector<Path>* targets = spec.relationship(name);
      if (targets && !targets->empty()) {
        os << "rel " << name << " = ";
        if (targets->size() == 1) {
          os << "<" << (*targets)[0].str() << ">";
        } else {
          os << "[";
          for (size_t i = 0; i < targets->size(); ++i) {
            if (i > 0) os << ", ";
            os << "<" << (*targets)[i].str() << ">";
          }
          os << "]";
        }
        os << "\n";
      }
    }
  }
}

// Print metadata block
void PrintMetadata(StreamWriter& os, const PrimSpecMeta& meta, int depth,
                   const PrimPrintOptions& opts) {
  bool has_meta = false;

  // Check what metadata we have
  if (!meta.active) has_meta = true;
  if (meta.hidden) has_meta = true;
  if (!meta.doc().empty()) has_meta = true;
  if (!meta.apiSchemas().empty()) has_meta = true;
  if (!meta.references.empty()) has_meta = true;
  if (!meta.payloads.empty()) has_meta = true;
  if (!meta.inherits.empty()) has_meta = true;
  if (!meta.specializes.empty()) has_meta = true;

  if (!has_meta) return;

  // Prim metadata is a parenthesized block between the prim declaration and
  // its body. Emitting it inside the braces produces invalid USDA.
  os << "\n";
  PrintIndent(os, depth, opts.indent);
  os << "(\n";
  const int item_depth = depth + 1;

  if (!meta.active) {
    PrintIndent(os, item_depth, opts.indent);
    os << "active = false\n";
  }
  if (meta.hidden) {
    PrintIndent(os, item_depth, opts.indent);
    os << "hidden = true\n";
  }
  if (!meta.doc().empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "doc = " << EscapeString(meta.doc()) << "\n";
  }
  if (!meta.apiSchemas().empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "apiSchemas = [";
    for (size_t i = 0; i < meta.apiSchemas().size(); ++i) {
      if (i > 0) os << ", ";
      os << EscapeString(meta.apiSchemas()[i]);
    }
    os << "]\n";
  }
  if (!meta.references.empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "references = [";
    for (size_t i = 0; i < meta.references.size(); ++i) {
      if (i > 0) os << ", ";
      os << FormatArcRef(meta.references[i]);
    }
    os << "]\n";
  }
  if (!meta.payloads.empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "payload = [";
    for (size_t i = 0; i < meta.payloads.size(); ++i) {
      if (i > 0) os << ", ";
      os << FormatArcRef(meta.payloads[i]);
    }
    os << "]\n";
  }
  if (!meta.inherits.empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "inherits = [";
    for (size_t i = 0; i < meta.inherits.size(); ++i) {
      if (i > 0) os << ", ";
      os << "<" << meta.inherits[i] << ">";
    }
    os << "]\n";
  }
  if (!meta.specializes.empty()) {
    PrintIndent(os, item_depth, opts.indent);
    os << "specializes = [";
    for (size_t i = 0; i < meta.specializes.size(); ++i) {
      if (i > 0) os << ", ";
      os << "<" << meta.specializes[i] << ">";
    }
    os << "]\n";
  }

  PrintIndent(os, depth, opts.indent);
  os << ")";
}

void PrintPrimSpecStart(StreamWriter& os, const PrimSpec& spec, int depth,
                        const PrimPrintOptions& opts) {
  // Print prim header
  PrintIndent(os, depth, opts.indent);
  os << GetSpecifierString(spec.specifier());

  // Print type name
  const std::string& type_name = spec.type_name();
  if (!type_name.empty()) {
    os << " " << type_name;
  }

  // Print prim name
  os << " \"" << spec.name() << "\"";

  if (opts.print_metadata) {
    PrintMetadata(os, spec.meta(), depth, opts);
  }

  // Open brace
  os << "\n";
  PrintIndent(os, depth, opts.indent);
  os << "{\n";

  int content_depth = depth + 1;

  // Print properties
  const auto& slots = spec.properties().slots();
  size_t prop_count = 0;
  for (const auto& slot : slots) {
    if (!slot.is_relationship()) {  // Skip relationships, handled separately
      PrintProperty(os, slot, spec, content_depth, opts);
      ++prop_count;
      if (opts.max_properties > 0 && prop_count >= opts.max_properties) {
        PrintIndent(os, content_depth, opts.indent);
        os << "# ... " << UIntToStr(slots.size() - prop_count) << " more properties\n";
        break;
      }
    }
  }

  // Print relationships
  if (opts.print_relationships) {
    PrintRelationships(os, spec, content_depth, opts);
  }
}

void PrintPrimSpecHierarchy(StreamWriter& os, const PrimSpec& spec,
                            const Layer& layer, int depth,
                            const PrimPrintOptions& opts) {
  if (opts.max_depth >= 0 && depth > opts.max_depth) {
    return;
  }

  struct Frame {
    const PrimSpec* spec{nullptr};
    int depth{0};
    size_t next_child{0};
  };

  std::vector<Frame> stack;
  std::unordered_set<const PrimSpec*> visited;
  PrintPrimSpecStart(os, spec, depth, opts);
  stack.push_back(Frame{&spec, depth, 0});
  visited.insert(&spec);

  while (!stack.empty()) {
    Frame& frame = stack.back();
    const std::vector<uint32_t>& children = frame.spec->child_indices();
    if (frame.next_child < children.size()) {
      const uint32_t child_idx = children[frame.next_child++];
      const PrimSpec* child = layer.prim(child_idx);
      if (!child) {
        continue;
      }

      // Match the recursive printer's blank line before every valid child,
      // including one suppressed by max_depth.
      os << "\n";
      const int child_depth = frame.depth + 1;
      if ((opts.max_depth >= 0 && child_depth > opts.max_depth) ||
          !visited.insert(child).second) {
        continue;
      }

      PrintPrimSpecStart(os, *child, child_depth, opts);
      stack.push_back(Frame{child, child_depth, 0});
      continue;
    }

    PrintIndent(os, frame.depth, opts.indent);
    os << "}\n";
    stack.pop_back();
  }
}

// Print layer metadata
void PrintLayerMeta(StreamWriter& os, const LayerMeta& meta, const PrimPrintOptions& opts) {
  os << "#usda 1.0\n";
  os << "(\n";

  bool first = true;
  if (meta.defaultPrim_set || !meta.defaultPrim.empty()) {
    os << opts.indent << "defaultPrim = \"" << meta.defaultPrim << "\"";
    first = false;
  }
  if (meta.upAxis != "Y") {
    if (!first) os << "\n";
    os << opts.indent << "upAxis = \"" << meta.upAxis << "\"";
    first = false;
  }
  if (meta.metersPerUnit != 0.01) {
    if (!first) os << "\n";
    os << opts.indent << "metersPerUnit = " << G6(meta.metersPerUnit);
    first = false;
  }
  if (meta.timeCodesPerSecond != 24.0) {
    if (!first) os << "\n";
    os << opts.indent << "timeCodesPerSecond = " << G6(meta.timeCodesPerSecond);
    first = false;
  }
  if (meta.startTimeCode != 0.0 || meta.endTimeCode != 0.0) {
    if (!first) os << "\n";
    os << opts.indent << "startTimeCode = " << G6(meta.startTimeCode);
    os << "\n" << opts.indent << "endTimeCode = " << G6(meta.endTimeCode);
    first = false;
  }
  if (!meta.doc.empty()) {
    if (!first) os << "\n";
    os << opts.indent << "doc = " << EscapeString(meta.doc);
    first = false;
  }
  if (!meta.subLayers.empty()) {
    if (!first) os << "\n";
    os << opts.indent << "subLayers = [\n";
    for (const auto& layer : meta.subLayers) {
      os << opts.indent << opts.indent << "@" << layer << "@,\n";
    }
    os << opts.indent << "]";
    first = false;
  }

  os << "\n)\n\n";
}

}  // anonymous namespace

// ============================================================
// Public API implementations
// ============================================================

std::string PrintPrim(const UsdPrim& prim, const PrimPrintOptions& opts) {
  std::string out;
  StreamWriter ss(&out);
  PrintPrim(ss, prim, opts);
  return out;
}

void PrintPrim(StreamWriter& os, const UsdPrim& prim, const PrimPrintOptions& opts) {
  if (!prim.IsValid()) {
    os << "# Invalid prim\n";
    return;
  }

  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) {
    os << "# Null prim spec\n";
    return;
  }

  // For UsdPrim printing, we don't have layer context for children
  // Print just this prim without recursion into children
  PrintIndent(os, 0, opts.indent);
  os << GetSpecifierString(spec->specifier());

  const std::string& type_name = spec->type_name();
  if (!type_name.empty()) {
    os << " " << type_name;
  }

  os << " \"" << spec->name() << "\"";
  if (opts.print_metadata) {
    PrintMetadata(os, spec->meta(), 0, opts);
  }
  os << "\n{\n";

  // Print properties
  const auto& slots = spec->properties().slots();
  size_t prop_count = 0;
  for (const auto& slot : slots) {
    if (!slot.is_relationship()) {
      PrintProperty(os, slot, *spec, 1, opts);
      ++prop_count;
      if (opts.max_properties > 0 && prop_count >= opts.max_properties) {
        PrintIndent(os, 1, opts.indent);
        os << "# ... " << UIntToStr(slots.size() - prop_count) << " more properties\n";
        break;
      }
    }
  }

  // Print relationships
  if (opts.print_relationships) {
    PrintRelationships(os, *spec, 1, opts);
  }

  // Note: children printed via Stage traversal, not here
  os << "}\n";
}

std::string PrintStage(const Stage& stage, const PrimPrintOptions& opts) {
  std::string out;
  StreamWriter ss(&out);
  PrintStage(ss, stage, opts);
  return out;
}

void PrintStage(StreamWriter& os, const Stage& stage, const PrimPrintOptions& opts) {
  const Layer* root_layer = stage.GetRootLayer();
  if (!root_layer) {
    os << "# Empty stage\n";
    return;
  }

  // Print via layer
  PrintLayer(os, *root_layer, opts);
}

std::string PrintPrimSpec(const PrimSpec& spec, const PrimPrintOptions& opts) {
  std::string out;
  StreamWriter ss(&out);
  // For standalone PrimSpec, we don't have layer context
  // Print just this spec without children
  ss << GetSpecifierString(spec.specifier());

  const std::string& type_name = spec.type_name();
  if (!type_name.empty()) {
    ss << " " << type_name;
  }

  ss << " \"" << spec.name() << "\"";
  if (opts.print_metadata) {
    PrintMetadata(ss, spec.meta(), 0, opts);
  }
  ss << "\n{\n";

  const auto& slots = spec.properties().slots();
  size_t prop_count = 0;
  for (const auto& slot : slots) {
    if (!slot.is_relationship()) {
      PrintProperty(ss, slot, spec, 1, opts);
      ++prop_count;
      if (opts.max_properties > 0 && prop_count >= opts.max_properties) {
        PrintIndent(ss, 1, opts.indent);
        ss << "# ... " << UIntToStr(slots.size() - prop_count) << " more properties\n";
        break;
      }
    }
  }

  if (opts.print_relationships) {
    PrintRelationships(ss, spec, 1, opts);
  }

  ss << "}\n";
  return out;
}

void PrintPrimSpec(StreamWriter& os, const PrimSpec& spec, int depth, const PrimPrintOptions& opts) {
  // This version needs Layer for children, but signature doesn't include it
  // Just print the spec without children
  PrintIndent(os, depth, opts.indent);
  os << GetSpecifierString(spec.specifier());

  const std::string& type_name = spec.type_name();
  if (!type_name.empty()) {
    os << " " << type_name;
  }

  os << " \"" << spec.name() << "\"";
  if (opts.print_metadata) {
    PrintMetadata(os, spec.meta(), depth, opts);
  }
  os << "\n";
  PrintIndent(os, depth, opts.indent);
  os << "{\n";

  const auto& slots = spec.properties().slots();
  size_t prop_count = 0;
  for (const auto& slot : slots) {
    if (!slot.is_relationship()) {
      PrintProperty(os, slot, spec, depth + 1, opts);
      ++prop_count;
      if (opts.max_properties > 0 && prop_count >= opts.max_properties) {
        PrintIndent(os, depth + 1, opts.indent);
        os << "# ... " << UIntToStr(slots.size() - prop_count) << " more properties\n";
        break;
      }
    }
  }

  if (opts.print_relationships) {
    PrintRelationships(os, spec, depth + 1, opts);
  }

  PrintIndent(os, depth, opts.indent);
  os << "}\n";
}

std::string PrintLayer(const Layer& layer, const PrimPrintOptions& opts) {
  std::string out;
  StreamWriter ss(&out);
  PrintLayer(ss, layer, opts);
  return out;
}

void PrintLayer(StreamWriter& os, const Layer& layer, const PrimPrintOptions& opts) {
  // Print layer header
  PrintLayerMeta(os, layer.meta(), opts);

  // Print root prims
  for (uint32_t root_idx : layer.root_indices()) {
    const PrimSpec* root = layer.prim(root_idx);
    if (root) {
      PrintPrimSpecHierarchy(os, *root, layer, 0, opts);
      os << "\n";
    }
  }
}

}  // namespace next
}  // namespace lightusd
