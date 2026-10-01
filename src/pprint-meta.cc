// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Metadata, property, and structural printing (extracted from pprinter.cc).
//
#include "pprint-meta.hh"

#include "pprint-detail.hh"
#include "prim-pprint.hh"
#include "str-util.hh"
#include "timesamples-pprint.hh"
#include "usdGeom.hh"      // kMaterialBinding, kMaterialBindingCollection, etc.
#include "tiny-format.hh"   // fmt::format (used in variantSet printing)
#include "core/layer-types.hh" // SubLayer, LayerMetas
#include "layer.hh"           // Layer
//
#include "common-macros.inc"

namespace lightusd {
// Forward declaration — defined in pprinter.cc
std::string to_string(const Layer &layer, const uint32_t indent, bool closing_brace);
}

namespace std {

#define DEFINE_OSTREAM_OP(TYPE) \
  std::ostream &operator<<(std::ostream &ofs, const lightusd::TYPE v) { \
    ofs << to_string(v); return ofs; }
DEFINE_OSTREAM_OP(Visibility)
DEFINE_OSTREAM_OP(Extent)
DEFINE_OSTREAM_OP(Interpolation)
#undef DEFINE_OSTREAM_OP

std::ostream &operator<<(std::ostream &ofs, const lightusd::Path &v) {
  ofs << lightusd::pquote(v.full_path_name());

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::LayerOffset &v) {
  bool print_offset{true};
  bool print_scale{true};

  if (std::fabs(v._offset) < std::numeric_limits<double>::epsilon()) {
    print_offset = false;
  }

  if (std::fabs(v._scale - 1.0) < std::numeric_limits<double>::epsilon()) {
    print_scale = false;
  }

  if (!print_offset && !print_scale) {
    // No need to print LayerOffset.
    return ofs;
  }

  // Print in pxr USDA form: ` (offset = X; scale = Y)`
  // (space before paren, `;` separator).
  ofs << " (";
  if (print_offset && print_scale) {
    ofs << "offset = " << lightusd::dtos(v._offset)
        << "; scale = " << lightusd::dtos(v._scale);
  } else if (print_offset) {
    ofs << "offset = " << lightusd::dtos(v._offset);
  } else {  // print_scale
    ofs << "scale = " << lightusd::dtos(v._scale);
  }
  ofs << ")";

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::Reference &v) {
  // For internal references (no asset path, just prim path), don't output "@@"
  if (!v.asset_path.GetAssetPath().empty()) {
    ofs << v.asset_path;
  }
  if (v.prim_path.is_valid()) {
    ofs << v.prim_path;
  }
  // When customData is present, fold layerOffset + customData into one
  // parens block. Otherwise let LayerOffset emit its own (possibly
  // empty) clause.
  if (!v.customData.empty()) {
    const bool has_offset =
        std::fabs(v.layerOffset._offset) >=
        std::numeric_limits<double>::epsilon();
    const bool has_scale =
        std::fabs(v.layerOffset._scale - 1.0) >=
        std::numeric_limits<double>::epsilon();
    ofs << " (\n";
    if (has_offset) {
      ofs << "    offset = " << lightusd::dtos(v.layerOffset._offset) << "\n";
    }
    if (has_scale) {
      ofs << "    scale = " << lightusd::dtos(v.layerOffset._scale) << "\n";
    }
    ofs << lightusd::print_customData(v.customData, "customData",
                                      /* indent */ 1);
    ofs << ")";
  } else {
    ofs << v.layerOffset;
  }

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::Payload &v) {
  if (v.is_none()) {
    ofs << "None";
  } else {
    // For internal payloads (no asset path, just prim path), don't output "@@"
    if (!v.asset_path.GetAssetPath().empty()) {
      ofs << v.asset_path;
    }
    if (v.prim_path.is_valid()) {
      ofs << v.prim_path;
    }
    ofs << v.layerOffset;
  }

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::SubLayer &v) {
  ofs << v.assetPath << v.layerOffset;

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::StringData &v) {
   ofs << lightusd::buildEscapedAndQuotedStringForUSDA(v.value);
  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::Layer &layer) {
  ofs << lightusd::to_string(layer, 0, true);
  return ofs;
}

}  // namespace std

namespace lightusd {

namespace pprint {

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

static std::string sIndentString = "    ";

#ifdef __clang__
#pragma clang diagnostic pop
#endif

static thread_local uint32_t sColumnLimit = 0;
static thread_local uint32_t sPrefixColumns = 0;

// Opt-in: emit prim children and properties in their AUTHORED order, recovered
// from the crate's `primChildren`/`properties` token-vector fields (what
// OpenUSD's usdcat does), instead of the default lexicographical order. Plain
// (not thread_local) so parallel-print worker threads observe the setting.
// Read by the USDC Layer reader (whether to record the order metadata) and by
// the USDA writers (whether to order by it). See SetPreserveAuthoredOrder().
static bool sPreserveAuthoredOrder = false;

// Opt-in: usdcat-style USDA layout (metadata paren on the `def` line, plain blank
// separators). See SetUSDTextFormat(). Plain (not thread_local) for parallel print.
static bool sUSDTextFormat = false;

std::string Indent(uint32_t n) {
  std::stringstream ss;

  for (uint32_t i = 0; i < n; i++) {
    ss << sIndentString;
  }

  return ss.str();
}

void SetIndentString(const std::string &s) { sIndentString = s; }

void SetColumnLimit(uint32_t limit) { sColumnLimit = limit; }
uint32_t GetColumnLimit() { return sColumnLimit; }

void SetPreserveAuthoredOrder(bool enable) { sPreserveAuthoredOrder = enable; }
bool GetPreserveAuthoredOrder() { return sPreserveAuthoredOrder; }

void SetUSDTextFormat(bool enable) { sUSDTextFormat = enable; }
bool GetUSDTextFormat() { return sUSDTextFormat; }
uint32_t GetPrefixColumns() { return sPrefixColumns; }

ScopedPrefixColumns::ScopedPrefixColumns(uint32_t cols) : prev_(sPrefixColumns) {
  sPrefixColumns = cols;
}
ScopedPrefixColumns::~ScopedPrefixColumns() { sPrefixColumns = prev_; }

}  // namespace pprint

namespace {

// Print a single reference listop (e.g., "prepend references = ...")
std::string print_reference_listop(const prim::ReferenceListOp &ref_op,
                                   const uint32_t indent) {
  std::stringstream ss;

  auto listEditQual = std::get<0>(ref_op);
  auto vars = std::get<1>(ref_op);

  ss << pprint::Indent(indent);

  if (listEditQual != ListEditQual::ResetToExplicit) {
    ss << to_string(listEditQual) << " ";
  }

  ss << "references = ";

  if (vars.empty()) {
    ss << "None";
  } else {
    if (vars.size() == 1) {
      ss << vars[0];
    } else {
      ss << vars;
    }
  }
  ss << "\n";

  return ss.str();
}

// Print all reference listops (supports multiple listops per prim)
std::string print_references(const prim::ReferenceList &references,
                             const uint32_t indent) {
  std::stringstream ss;
  for (const auto &ref_op : references) {
    ss << print_reference_listop(ref_op, indent);
  }
  return ss.str();
}

}  // namespace

// Print a single payload listop (e.g., "prepend payload = ...")
static std::string print_payload_listop(const prim::PayloadListOp &payload_op,
                                        const uint32_t indent) {
  std::stringstream ss;

  auto listEditQual = std::get<0>(payload_op);
  auto vars = std::get<1>(payload_op);

  ss << pprint::Indent(indent);

  if (listEditQual != ListEditQual::ResetToExplicit) {
    ss << to_string(listEditQual) << " ";
  }

  ss << "payload = ";
  if (vars.empty()) {
    ss << "None";
  } else {
    if (vars.size() == 1) {
      ss << vars[0];
    } else {
      ss << vars;
    }
  }
  ss << "\n";

  return ss.str();
}

// Print all payload listops (supports multiple listops per prim)
static std::string print_payload(const prim::PayloadList &payload,
                                 const uint32_t indent) {
  std::stringstream ss;
  for (const auto &payload_op : payload) {
    ss << print_payload_listop(payload_op, indent);
  }
  return ss.str();
}

// Emit the `apiSchemas` metadatum (handles `= None`, authored list-ops, and the
// resolved name view). Factored out so the USD-text-format path can emit it
// FIRST in the metadata block (where usdcat places it) while the default keeps
// it in lightusd's historical position.
static void print_apiSchemas_block(std::stringstream &ss,
                                   const APISchemas &schemas, uint32_t indent) {
  if (schemas.explicitlyEmpty) {
    // Authored as `apiSchemas = None` (explicit empty list). pxrUSD preserves
    // this; reproduce it verbatim.
    ss << pprint::Indent(indent) << "apiSchemas = None\n";
  } else if (!schemas.authoredOps.empty()) {
    // Emit the authored SdfTokenListOp verbatim. A single prim may carry
    // multiple ops (e.g. `delete` + `prepend`); pxrUSD prints them in a
    // canonical qualifier order, so match that for a stable round-trip.
    const ListEditQual kOrder[] = {
        ListEditQual::ResetToExplicit,  // explicit (unqualified) first
        ListEditQual::Delete, ListEditQual::Add,
        ListEditQual::Prepend, ListEditQual::Append, ListEditQual::Order};
    for (ListEditQual qual : kOrder) {
      for (const auto &op : schemas.authoredOps) {
        if (op.first != qual) continue;
        const std::string qualStr = to_string(qual);
        ss << pprint::Indent(indent);
        if (!qualStr.empty()) {
          ss << qualStr << " ";
        }
        ss << "apiSchemas = [";
        bool first = true;
        for (const auto &item : op.second) {
          if (!first) ss << ", ";
          first = false;
          ss << "\"" << item.first;  // schema name (raw)
          if (!item.second.empty()) {
            ss << ":" << item.second;  // instance name
          }
          ss << "\"";
        }
        ss << "]\n";
      }
    }
  } else if (schemas.names.size() || schemas.unknownSchemas.size()) {
    ss << pprint::Indent(indent) << to_string(schemas.listOpQual)
       << " apiSchemas = [";

    bool first = true;

    // Print unknown schemas first (they typically appear first in USD files)
    for (size_t i = 0; i < schemas.unknownSchemas.size(); i++) {
      if (!first) {
        ss << ", ";
      }
      first = false;

      auto schemaName = std::get<0>(schemas.unknownSchemas[i]);
      ss << "\"" << schemaName;

      auto instanceName = std::get<1>(schemas.unknownSchemas[i]);
      if (!instanceName.empty()) {
        ss << ":" << instanceName;
      }

      ss << "\"";
    }

    // Print known schemas
    for (size_t i = 0; i < schemas.names.size(); i++) {
      if (!first) {
        ss << ", ";
      }
      first = false;

      auto name = std::get<0>(schemas.names[i]);
      ss << "\"" << to_string(name);

      auto instanceName = std::get<1>(schemas.names[i]);

      if (!instanceName.empty()) {
        ss << ":" << instanceName;
      }

      ss << "\"";
    }
    ss << "]\n";
  }
}

std::string print_prim_metas(const PrimMeta &meta, const uint32_t indent) {
  std::stringstream ss;

  // usdcat places `apiSchemas` FIRST in the prim-metadata block; lightusd's
  // default emits it later. Under the USD-text-format opt-in, emit it up front.
  const bool usd_text_meta = pprint::GetUSDTextFormat();
  if (usd_text_meta && meta.has_apiSchemas()) {
    print_apiSchemas_block(ss, meta.get_apiSchemas(), indent);
  }

  if (meta.has_active()) {
    ss << pprint::Indent(indent)
       << "active = " << to_string(meta.get_active()) << "\n";
  }

  if (meta.has_clips()) {
    ss << print_customData(meta.get_clips(), "clips", indent);
  }

  if (meta.has_instanceable()) {
    ss << pprint::Indent(indent)
       << "instanceable = " << to_string(meta.get_instanceable()) << "\n";
  }

  if (meta.has_hidden()) {
    ss << pprint::Indent(indent)
       << "hidden = " << to_string(meta.get_hidden()) << "\n";
  }

  if (meta.has_kind()) {
    ss << pprint::Indent(indent) << "kind = " << quote(meta.get_kind()) << "\n";
  }

  if (meta.has_sceneName()) {
    ss << pprint::Indent(indent)
       << "sceneName = " << quote(meta.get_sceneName()) << "\n";
  }

  if (meta.has_displayName()) {
    ss << pprint::Indent(indent)
       << "displayName = " << quote(meta.get_displayName()) << "\n";
  }

  if (meta.has_assetInfo()) {
    ss << print_customData(meta.get_assetInfo(), "assetInfo", indent);
  }

  if (meta.inherits) {
    // Print all inherits listops
    for (const auto &inherit_op : meta.inherits.value()) {
      ss << pprint::Indent(indent);
      auto listEditQual = std::get<0>(inherit_op);
      auto var = std::get<1>(inherit_op);

      if (listEditQual != ListEditQual::ResetToExplicit) {
        ss << to_string(listEditQual) << " ";
      }

      if (var.empty() && listEditQual == ListEditQual::ResetToExplicit) {
        ss << "inherits = None";
      } else if (var.size() == 1) {
        // print as scalar
        ss << "inherits = " << var[0];
      } else {
        ss << "inherits = " << var;
      }
      ss << "\n";
    }
  }

  if (meta.specializes) {
    // Print all specializes listops
    for (const auto &specialize_op : meta.specializes.value()) {
      ss << pprint::Indent(indent);
      auto listEditQual = std::get<0>(specialize_op);
      auto var = std::get<1>(specialize_op);

      if (listEditQual != ListEditQual::ResetToExplicit) {
        ss << to_string(listEditQual) << " ";
      }

      if (var.empty() && listEditQual == ListEditQual::ResetToExplicit) {
        ss << "specializes = None";
      } else if (var.size() == 1) {
        // print as scalar
        ss << "specializes = " << var[0];
      } else {
        ss << "specializes = " << var;
      }
      ss << "\n";
    }
  }

  if (meta.references) {
    ss << print_references(meta.references.value(), indent);
  }

  if (meta.payload) {
    ss << print_payload(meta.payload.value(), indent);
  }

  // TODO: only print in usdShade Prims.
  if (meta.has_sdrMetadata()) {
    ss << print_customData(meta.get_sdrMetadata(), "sdrMetadata", indent);
  }

  if (meta.variants) {
    ss << print_variantSelectionMap(meta.variants.value(), indent);
  }

  if (meta.variantSets) {
    // Print all variantSets listops
    for (const auto &variantSets_op : meta.variantSets.value()) {
      ss << pprint::Indent(indent);
      auto listEditQual = std::get<0>(variantSets_op);
      const std::vector<std::string> &vs = std::get<1>(variantSets_op);  // string[]

      if (listEditQual != ListEditQual::ResetToExplicit) {
        ss << to_string(listEditQual) << " ";
      }

      ss << "variantSets = ";

      if (vs.empty()) {
        ss << "None";
      } else if (vs.size() == 1) {
        // Single element: print as scalar (without brackets)
        ss << quote(vs[0]);
      } else {
        ss << to_string(vs);
      }

      ss << "\n";
    }
  }

  if (!usd_text_meta && meta.has_apiSchemas()) {
    print_apiSchemas_block(ss, meta.get_apiSchemas(), indent);
  }

  if (meta.has_doc()) {
    ss << pprint::Indent(indent) << "doc = "
       << buildEscapedAndQuotedStringForUSDA(meta.get_doc().value) << "\n";
  }

  if (meta.has_comment()) {
    // Output with or without "comment =" prefix based on how it was parsed
    ss << pprint::Indent(indent);
    if (meta.get_comment().has_comment_prefix) {
      ss << "comment = ";
    }
    ss << buildEscapedAndQuotedStringForUSDA(meta.get_comment().value) << "\n";
  }

  if (meta.has_customData()) {
    ss << print_customData(meta.get_customData(), "customData", indent);
  }

  for (const auto &item : meta.unregisteredMetas) {
    // Write verbatim — the stored string already contains quotes if the
    // original value was a quoted string.  Non-string values (numbers,
    // arrays, None, tuples, …) are stored without quotes and must be
    // written back without quotes.  This matches OpenUSD behaviour.
    ss << pprint::Indent(indent) << item.first << " = " << item.second << "\n";
  }

  // TODO: deprecate meta.meta and remove it.
  for (const auto &item : meta.meta) {
    ss << print_meta(item.second, indent + 1, true, item.first);
  }

  // for (const auto &item : meta.stringData) {
  //   ss << pprint::Indent(indent) << to_string(item) << "\n";
  // }

  return ss.str();
}

std::string print_attr_metas(const AttrMeta &meta, const uint32_t indent) {
  std::stringstream ss;

  if (meta.has_interpolation()) {
    ss << pprint::Indent(indent)
       << "interpolation = " << to_string(meta.get_interpolation())
       << "\n";
  }

  if (meta.has_elementSize()) {
    ss << pprint::Indent(indent)
       << "elementSize = " << to_string(meta.get_elementSize()) << "\n";
  }

  if (meta.has_bindMaterialAs()) {
    ss << pprint::Indent(indent)
       << "bindMaterialAs = " << to_string(meta.get_bindMaterialAs())
       << "\n";
  }

  if (meta.has_connectability()) {
    ss << pprint::Indent(indent)
       << "connectability = " << to_string(meta.get_connectability())
       << "\n";
  }

  if (meta.has_displayName()) {
    ss << pprint::Indent(indent)
       << "displayName = " << quote(meta.get_displayName()) << "\n";
  }

  if (meta.has_displayGroup()) {
    ss << pprint::Indent(indent)
       << "displayGroup = " << quote(meta.get_displayGroup()) << "\n";
  }

  if (meta.has_outputName()) {
    ss << pprint::Indent(indent)
       << "outputName = " << to_string(meta.get_outputName()) << "\n";
  }

  if (meta.has_renderType()) {
    ss << pprint::Indent(indent)
       << "renderType = " << to_string(meta.get_renderType()) << "\n";
  }

  if (meta.has_sdrMetadata()) {
    ss << pprint::Indent(indent)
       << print_customData(meta.get_sdrMetadata(), "sdrMetadata", indent);
  }

  if (meta.has_hidden()) {
    ss << pprint::Indent(indent)
       << "hidden = " << to_string(meta.get_hidden()) << "\n";
  }

  if (meta.has_comment()) {
    // Honor how it was authored, exactly as the PRIM metadata printer above
    // does. A bare string in a metadata block IS the comment in USD; only the
    // ASCII spelling differs, and only the ASCII parser knows which was used.
    ss << pprint::Indent(indent);
    if (meta.get_comment().has_comment_prefix) {
      ss << "comment = ";
    }
    ss << to_string(meta.get_comment()) << "\n";
  }

  if (meta.has_weight()) {
    ss << pprint::Indent(indent) << "weight = " << dtos(meta.get_weight())
       << "\n";
  }

  if (meta.has_customData()) {
    ss << print_customData(meta.get_customData(), "customData", indent);
  }

  // other user defined metadataum.
  auto is_known_key = [](const std::string &key) {
    // Keys already printed above
    constexpr const char* known[] = {
      "interpolation", "elementSize", "bindMaterialAs", "connectability",
      "displayName", "displayGroup", "outputName", "renderType",
      "sdrMetadata", "hidden", "comment", "weight", "customData"
    };
    for (const char* k : known) {
      if (key == k) return true;
    }
    return false;
  };
  for (const auto &item : meta.data()) {
    if (is_known_key(item.first)) continue;
    // attribute meta does not emit type_name
    ss << print_meta(item.second, indent, /* emit_type_name */false, item.first);
  }

  // Unregistered property metadata: write the stored raw text verbatim -- it
  // already contains quotes if the original value was a quoted string, and
  // non-string values (numbers, arrays, None, ...) are stored without quotes.
  // This matches OpenUSD behaviour (SdfUnregisteredValue).
  for (const auto &item : meta.unregisteredMetas) {
    ss << pprint::Indent(indent) << item.first << " = " << item.second << "\n";
  }

  for (const auto &item : meta.stringData) {
    ss << pprint::Indent(indent) << to_string(item) << "\n";
  }

  return ss.str();
}

// Forward declaration (defined later in this file).
std::string print_spline(const primvar::PrimVar::SplineData &sd,
                         const uint32_t indent);

std::string print_timesamples(const value::TimeSamples &v,
                              const uint32_t indent) {
  // Use the new pprint_timesamples function from timesamples-pprint,
  // which handles both binary and generic value-backed cases efficiently.
  std::string result = pprint_timesamples(v, indent);

  // Add a trailing newline if not present (for consistency with other pprinter functions)
  if (!result.empty() && result.back() != '\n') {
    result += '\n';
  }

  return result;
}

std::string print_rel_prop(const Property &prop, const std::string &name,
                           uint32_t indent) {
  std::stringstream ss;

  if (!prop.is_relationship()) {
    return ss.str();
  }

  ss << pprint::Indent(indent);

  // USD spec order: [listop] [custom] [variability] rel name
  // List editing qualifier comes first
  if (prop.get_listedit_qual() != ListEditQual::ResetToExplicit) {
    ss << to_string(prop.get_listedit_qual()) << " ";
  }

  if (prop.has_custom()) {
    ss << "custom ";
  }

  const Relationship &rel = prop.get_relationship();
  if (rel.is_varying_authored()) {
    ss << "varying ";
  }

  ss << print_rel_only(rel, name, indent);

  return ss.str();
}

std::string print_prop(const Property &prop, const std::string &prop_name,
                       uint32_t indent) {
  std::stringstream ss;

  if (prop.is_relationship()) {
    ss << print_rel_prop(prop, prop_name, indent);

    // Attribute or AttributeConnection
  } else if (prop.is_attribute()) {
    const Attribute &attr = prop.get_attribute();

    //
    // May print multiple times.
    // e.g.
    // float var = 1.0
    // float var.timeSamples = ...
    // float var.connect = <...>
    //
    // timeSamples and connect cannot have attrMeta
    //

    // Print attribute if it has metadata, has a value, OR is just typed (but not connection-only)
    // NOTE: Some attributes (like outputs:out) may be typed but not have a value
    // Skip printing declaration if this is a connection-only attribute (will be printed in the connection section below)
    bool is_connection_only = attr.has_connections() && !attr.has_value() && !attr.has_timesamples() && !attr.metas().authored();

    if ((attr.metas().authored() || attr.has_value() || !attr.type_name().empty()) && !is_connection_only) {

      ss << pprint::Indent(indent);

      if (prop.has_custom()) {
        ss << "custom ";
      }

      if (attr.variability() == Variability::Uniform) {
        ss << "uniform ";
      } else if (attr.is_varying_authored()) {
        // For Attribute, `varying` is the default variability and does not shown
        // in USDA do nothing
      }

      std::string ty;

      ty = attr.type_name();
      ss << ty << " " << prop_name;

      if (!attr.has_value()) {
        // Nothing to do
      } else {
        // has value content

        if (attr.is_blocked()) {
          ss << " = None";
        } else {
          // default value
          // Compute prefix width for column-wrapping of arrays
          size_t qual_len = 0;
          if (prop.has_custom()) qual_len += 7;  // "custom "
          if (attr.variability() == Variability::Uniform) qual_len += 8;  // "uniform "
          uint32_t pcols = static_cast<uint32_t>(
              pprint::Indent(indent).size() + qual_len +
              ty.size() + 1 + prop_name.size() + 3);  // " " + name + " = "
          pprint::ScopedPrefixColumns spc(pcols);
          std::string value_str = value::pprint_value(attr.get_var().value_raw());
          // Only print " = value" if value_str is not empty
          // (value could be typed but empty when only timeSamples are set)
          if (!value_str.empty()) {
            ss << " = " << value_str;
          }
        }
      }

      if (prop.get_attribute().metas().authored()) {
        ss << " (\n"
           << print_attr_metas(prop.get_attribute().metas(), indent + 1)
           << pprint::Indent(indent) << ")";
      }

      ss << "\n";
    }

    // `has_timesamples()` now means "timeSamples were AUTHORED", empty or not
    // (value::TimeSamples::authored()). It used to be size() > 0, so this had to
    // sniff a valid-type_id-but-no-samples TimeSamples to catch an authored-empty
    // block -- which the WRITER had no equivalent of, so it dropped it.
    bool has_timesamples_authored = attr.has_timesamples();

    if (has_timesamples_authored && (attr.variability() != Variability::Uniform)) {

      ss << pprint::Indent(indent);

      if (prop.has_custom()) {
        ss << "custom ";
      }

      std::string ty;

      ty = attr.type_name();
      ss << ty << " " << prop_name;

      ss << ".timeSamples";

      ss << " = ";

      ss << print_timesamples(attr.get_var().ts_raw(), indent);

      ss << "\n";
    }

    if (attr.get_var().has_spline()) {
      ss << pprint::Indent(indent);
      if (prop.has_custom()) {
        ss << "custom ";
      }
      ss << attr.type_name() << " " << prop_name << ".spline = ";
      ss << print_spline(attr.get_var().spline_data(), indent);
      ss << "\n";
    }

    if (attr.has_connections()) {

      ss << pprint::Indent(indent);

      if (prop.has_custom()) {
        ss << "custom ";
      }

      if (attr.variability() == Variability::Uniform) {
        ss << "uniform ";
      } else if (attr.is_varying_authored()) {
        // For Attribute, `varying` is the default variability and does not shown
        // in USDA do nothing
      }

      std::string ty;

      ty = attr.type_name();
      ss << ty << " " << prop_name;

      ss << ".connect = ";

      const std::vector<Path> &paths = attr.connections();
      if (paths.size() == 1) {
        ss << paths[0];
      } else if (paths.size() == 0) {
        ss << "[InternalError]";
      } else {
        ss << paths;
      }

      ss << "\n";
    }

  } else {
    ss << "[Invalid Property] " << prop_name << "\n";
  }

  return ss.str();
}

std::string print_props(const std::map<std::string, Property> &props,
                        uint32_t indent) {
  std::stringstream ss;

  for (const auto &item : props) {
    const Property &prop = item.second;

    ss << print_prop(prop, item.first, indent);
  }

  return ss.str();
}

// Print user-defined (custom) properties.
std::string print_props(const std::map<std::string, Property> &props,
                        std::set<std::string> &tok_table,
                        const std::vector<value::token> &propNames,
                        uint32_t indent) {
  std::stringstream ss;

  if (propNames.size()) {
    for (size_t i = 0; i < propNames.size(); i++) {
      if (tok_table.count(propNames[i].str())) {
        continue;
      }

      const auto it = props.find(propNames[i].str());
      if (it != props.end()) {
        ss << print_prop(it->second, it->first, indent);

        tok_table.insert(propNames[i].str());
      }
    }
  } else {
    ss << print_props(props, indent);
  }

  return ss.str();
}

std::string print_xformOpOrder(const std::vector<XformOp> &xformOps,
                               const uint32_t indent) {
  std::stringstream ss;

  if (xformOps.size()) {
    ss << pprint::Indent(indent) << "uniform token[] xformOpOrder = [";
    for (size_t i = 0; i < xformOps.size(); i++) {
      if (i > 0) {
        ss << ", ";
      }

      auto xformOp = xformOps[i];
      ss << "\"";
      if (xformOp.inverted) {
        ss << "!invert!";
      }
      ss << to_string(xformOp.op_type);
      if (!xformOp.suffix.empty()) {
        ss << ":" << xformOp.suffix;
      }
      ss << "\"";
    }
    ss << "]\n";
  }

  return ss.str();
}

std::string print_xformOps(const std::vector<XformOp> &xformOps,
                           const uint32_t indent) {
  std::stringstream ss;

  // To prevent printing xformOp attributes multiple times.
  std::set<std::string> printed_vars;

  // xforms props
  if (xformOps.size()) {
    for (size_t i = 0; i < xformOps.size(); i++) {
      const auto xformOp = xformOps[i];

      if (xformOp.op_type == XformOp::OpType::ResetXformStack) {
        // No need to print value.
        continue;
      }

      std::string varname = to_string(xformOp.op_type);
      if (!xformOp.suffix.empty()) {
        varname += ":" + xformOp.suffix;
      }

      DCOUT("has_default " << xformOp.has_default());
      DCOUT("has_timesamples " << xformOp.has_timesamples());

      if (xformOp.has_default()) {
        if (printed_vars.count(varname)) {
          continue;
        }

        printed_vars.insert(varname);

        ss << pprint::Indent(indent);
        ss << xformOp.get_value_type_name() << " ";
        ss << varname;
        ss << " = ";

        if (xformOp.is_blocked()) {
          ss << "None";
        } else if (auto pv = xformOp.get_scalar()) {
          ss << value::pprint_value(pv.value(), indent);
        } else {
          ss << "[InternalError]";
        }

        // TODO: metadata
        ss << "\n";
      }

      // Check if timeSamples were authored (even if empty)
      bool has_timesamples_authored = (xformOp.has_timesamples() || xformOp.get_var().ts_raw().type_id() != 0);

      if (has_timesamples_authored) {

        if (printed_vars.count(varname + ".timeSamples")) {
          continue;
        }

        printed_vars.insert(varname + ".timeSamples");

        ss << pprint::Indent(indent);
        ss << xformOp.get_value_type_name() << " ";
        ss << varname;
        ss << ".timeSamples";
        ss << " = ";

        // Always use ts_raw() to get timeSamples even if empty
        ss << print_timesamples(xformOp.get_var().ts_raw(), indent);
        ss << "\n";
      }

    }
  }

  // uniform token[] xformOpOrder
  ss << print_xformOpOrder(xformOps, indent);

  return ss.str();
}


std::string print_material_binding(const MaterialBinding *mb, const uint32_t indent) {
  if (!mb) {
    return std::string();
  }

  std::stringstream ss;

  if (mb->materialBinding.authored()) {
    ss << print_relationship(mb->materialBinding.relationship(),
                             mb->materialBinding.relationship().get_listedit_qual(),
                             /* custom */ false, kMaterialBinding, indent);
  }

  if (mb->materialBindingPreview.authored()) {
    ss << print_relationship(
        mb->materialBindingPreview.relationship(),
        mb->materialBindingPreview.relationship().get_listedit_qual(),
        /* custom */ false, kMaterialBindingPreview, indent);
  }

  if (mb->materialBindingFull.authored()) {
    ss << print_relationship(
        mb->materialBindingFull.relationship(),
        mb->materialBindingFull.relationship().get_listedit_qual(),
        /* custom */ false, kMaterialBindingFull, indent);
  }

  // NOTE: matb does not include "material:binding", "material:binding:preview" and "material:binding:full"
  for (const auto &matb : mb->materialBindingMap()) {
    if (matb.first.empty()) {
      // this should not happen
      continue;
    }

    std::string matb_name = kMaterialBinding + std::string(":") + matb.first;

    ss << print_relationship(
        matb.second,
        matb.second.get_listedit_qual(),
        /* custom */ false, matb_name, indent);

  }

  // TODO: sort by collection name?
  for (const auto &collection : mb->materialBindingCollectionMap()) {

    std::string purpose_name;
    if (!collection.first.empty()) {
      purpose_name = std::string(":") + collection.first;
    }

    for (size_t i = 0; i < collection.second.size(); i++) {
      std::string coll_name = collection.second.keys()[i];

      const Relationship *rel{nullptr};
      if (!collection.second.at(i, &rel)) {
        // this should not happen though.
        continue;
      }

      std::string rel_name;

      if (coll_name.empty()) {
        rel_name = kMaterialBindingCollection + purpose_name;
      } else {
        rel_name = kMaterialBindingCollection + std::string(":") + coll_name + purpose_name;
      }

      ss << print_relationship(
          *rel,
          rel->get_listedit_qual(),
          /* custom */ false, rel_name, indent);
    }
  }

  return ss.str();
}

// Emit a `.spline = { ... }` value block (AOUSD Core Spec 7.4.2.4).
std::string print_spline(const primvar::PrimVar::SplineData &sd,
                         const uint32_t indent) {
  std::stringstream ss;

  auto num = [](const value::Value &v) -> double {
    if (auto d = v.get_value<double>()) return d.value();
    if (auto f = v.get_value<float>()) return double(f.value());
    if (auto h = v.get_value<value::half>())
      return double(value::half_to_float(h.value()));
    return 0.0;
  };
  auto extrapStr = [](int mode, double slope) -> std::string {
    switch (mode) {
      case 0: return "none";
      case 1: return "held";
      case 2: return "linear";
      case 3: return "sloped(" + lightusd::dtos(slope) + ")";
      case 4: return "loop repeat";
      case 5: return "loop reset";
      case 6: return "loop oscillate";
      default: return "held";
    }
  };

  const bool hermite = (sd.curveType == 1);
  const uint32_t ki = indent + 1;

  ss << "{\n";
  ss << pprint::Indent(ki) << (hermite ? "hermite" : "bezier") << ",\n";

  // Only emit extrapolation when it differs from the default (Held).
  if (sd.preExtrapolation != 1) {
    ss << pprint::Indent(ki) << "pre: "
       << extrapStr(sd.preExtrapolation, sd.preExtrapolationSlope) << ",\n";
  }
  if (sd.postExtrapolation != 1) {
    ss << pprint::Indent(ki) << "post: "
       << extrapStr(sd.postExtrapolation, sd.postExtrapolationSlope) << ",\n";
  }
  if (sd.hasLoop) {
    ss << pprint::Indent(ki) << "loop: (" << lightusd::dtos(sd.loopProtoStart)
       << ", " << lightusd::dtos(sd.loopProtoEnd) << ", " << sd.loopNumPreLoops
       << ", " << sd.loopNumPostLoops << ", "
       << lightusd::dtos(sd.loopValueOffset) << "),\n";
  }

  // A non-None tangent algorithm is emitted as a trailing keyword inside the
  // tangent parens, e.g. `(2, 0, autoEase)` (crate 0.13.0 feature).
  auto algoSuffix = [](int algo) -> std::string {
    switch (algo) {
      case 1: return ", custom";
      case 2: return ", autoEase";
      default: return "";  // None
    }
  };

  auto tangent = [&](double width, double slope, int algo) -> std::string {
    if (hermite) {
      return "(" + lightusd::dtos(slope) + algoSuffix(algo) + ")";
    }
    return "(" + lightusd::dtos(width) + ", " + lightusd::dtos(slope) +
           algoSuffix(algo) + ")";
  };

  for (const auto &k : sd.knots) {
    ss << pprint::Indent(ki) << lightusd::dtos(k.time) << ": ";
    if (k.hasDualValue) {
      ss << lightusd::dtos(num(k.preValue)) << " & " << lightusd::dtos(num(k.val));
    } else {
      ss << lightusd::dtos(num(k.val));
    }
    // pre-tangent (only when authored, including a non-None algorithm)
    if (k.preTangentWidth != 0.0 || k.preTangentSlope != 0.0 ||
        k.preTangentAlgorithm != 0) {
      ss << "; pre "
         << tangent(k.preTangentWidth, k.preTangentSlope, k.preTangentAlgorithm);
    }
    // next-segment interpolation
    switch (k.interpolationMode) {
      case 0: ss << "; post none"; break;
      case 1: ss << "; post held"; break;
      case 2: ss << "; post linear"; break;
      case 3:
        ss << "; post curve "
           << tangent(k.postTangentWidth, k.postTangentSlope,
                      k.postTangentAlgorithm);
        break;
      default: break;
    }
    ss << ",\n";
  }

  ss << pprint::Indent(indent) << "}";
  return ss.str();
}

std::string print_collection(const Collection *coll, const uint32_t indent) {
  std::stringstream ss;

  if (!coll) {
    return std::string();
  }

  const auto &instances = coll->instances();

  for (size_t i = 0; i < instances.size(); i++) {
    std::string name = instances.keys()[i];

    CollectionInstance instance;
    if (!instances.at(i, &instance)) {
      continue;
    }

    std::string prefix = "collection";
    if (name.size()) {
      prefix += ":" + name;
    }

    if (instance.expansionRule.authored()) {
      ss << print_typed_token_attr(instance.expansionRule, prefix + ":expansionRule", indent);

    }


    if (instance.includeRoot.authored()) {
      ss << print_typed_attr(instance.includeRoot, prefix + ":includeRoot", indent);
    }

    if (instance.includes.authored()) {
      ss << print_relationship(
          instance.includes.relationship(),
          instance.includes.relationship().get_listedit_qual(),
          /* custom */ false, prefix + ":includes", indent);

    }

    if (instance.excludes.authored()) {
      ss << print_relationship(
          instance.excludes.relationship(),
          instance.excludes.relationship().get_listedit_qual(),
          /* custom */ false, prefix + ":excludes", indent);

    }

    ss << print_typed_attr(instance.membershipExpression,
                           prefix + ":membershipExpression", indent);
  }

  return ss.str();
}

std::string print_variantSelectionMap(const VariantSelectionMap &m,
                                      const uint32_t indent) {
  std::stringstream ss;

  if (m.empty()) {
    return ss.str();
  }

  ss << pprint::Indent(indent) << "variants = {\n";
  for (const auto &item : m) {
    ss << pprint::Indent(indent + 1) << "string " << item.first << " = "
       << quote(item.second) << "\n";
  }
  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string print_customData(const CustomDataType &customData,
                             const std::string &dict_name,
                             const uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent);
  if (!dict_name.empty()) {
    std::string name = dict_name;

    if (!isValidIdentifier(name)) {
      // May contain "/", quote it
      name = quote(name);
    }

    ss << name << " = {\n";
  } else {
    ss << "{\n";
  }
  for (const auto &item : customData) {
    ss << print_meta(item.second, indent + 1, true, item.first);
  }
  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string print_meta(const MetaVariable &meta, const uint32_t indent, bool emit_type_name,
                       const std::string &varname) {
  // Guard against deeply nested metadata (dict-in-dict) recursion; `indent`
  // grows per nesting level, so this bounds call-stack depth.
  if (indent > kMaxDefaultTraversalLimit) {
    return {};
  }

  std::stringstream ss;

  // ss << "TODO: isObject " << meta.is_object() << ", isValue " <<
  // meta.IsValue() << "\n";

  // Use varname if meta.name is empty
  std::string name = meta.get_name();
  if (name.empty()) {
    name = varname;
  }

  if (name.empty()) {
    name = "[ERROR:EmptyName]";
  }

  if (auto pv = meta.get_value<CustomDataType>()) {
    // dict
    if (!isValidIdentifier(name)) {
      // May contain "/", quote it
      name = quote(name);
    }
    ss << pprint::Indent(indent) << "dictionary " << name << " = {\n";
    for (const auto &item : pv.value()) {
      ss << print_meta(item.second, indent + 1, /* emit_type_name */true, item.first);
    }
    ss << pprint::Indent(indent) << "}\n";
  } else {
    ss << pprint::Indent(indent);
    if (emit_type_name) {
      ss << meta.type_name() << " ";
    }
    // Dictionary keys may contain characters that are not valid in a bare
    // identifier (e.g. namespaced keys like `rtx:sceneDb:ambientLightIntensity`).
    // Quote them so the emitted USDA round-trips (matches pxrUSD behavior).
    if (!isValidIdentifier(name)) {
      name = quote(name);
    }
    ss << name << " = "
       << pprint_value(meta.get_raw_value()) << "\n";
  }

  return ss.str();
}

// Forward declaration — print_prim is defined in pprinter.cc namespace prim
namespace prim { std::string print_prim(const Prim &prim, const uint32_t indent); }

std::string print_variantSetStmt(
    const std::map<std::string, VariantSet> &vslist, const uint32_t indent) {
  std::stringstream ss;

  for (const auto &variantSet : vslist) {
    if (variantSet.second.variantSet.empty()) {
      continue;
    }

    ss << pprint::Indent(indent) << "variantSet " << quote(variantSet.first)
       << " = {\n";

    for (const auto &item : variantSet.second.variantSet) {
      ss << pprint::Indent(indent + 1) << quote(item.first) << " ";

      if (item.second.metas().authored()) {
        ss << "(\n";
        ss << print_prim_metas(item.second.metas(), indent + 2);
        ss << pprint::Indent(indent + 1) << ") ";
      }

      ss << "{\n";

      // props
      ss << print_props(item.second.properties(), indent + 2);

      // primChildren
      // TODO: print child Prims based on `primChildren` Prim metadata
      const auto &variantPrimMetas = item.second.metas();
      const auto &variantPrimChildren = item.second.primChildren();

      if (variantPrimMetas.primChildren.size() == variantPrimChildren.size()) {
        std::map<std::string, const Prim *> primNameTable;
        for (size_t i = 0; i < variantPrimChildren.size(); i++) {
          primNameTable.emplace(variantPrimChildren[i].element_name(),
                                &variantPrimChildren[i]);
        }

        for (size_t i = 0; i < variantPrimMetas.primChildren.size(); i++) {
          value::token nameTok = variantPrimMetas.primChildren[i];
          DCOUT(fmt::format("variantPrimChildren  {}/{} = {}", i,
                            variantPrimMetas.primChildren.size(),
                            nameTok.str()));
          const auto it = primNameTable.find(nameTok.str());
          if (it != primNameTable.end()) {
            ss << prim::print_prim(*(it->second), indent + 2);
          } else {
            // TODO: Report warning?
          }
        }
      } else {
        for (const auto &child : variantPrimChildren) {
          ss << prim::print_prim(child, indent + 2);
        }
      }

      // nested variantSets
      if (item.second.variantSets().size()) {
        ss << print_variantSetStmt(item.second.variantSets(), indent+2);
      }

      ss << pprint::Indent(indent+1) << "}\n";

    }

    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string print_variantSetSpecStmt(
    const std::map<std::string, VariantSetSpec> &vslist,
    const uint32_t indent) {
  std::stringstream ss;

  // ss << "# variantSet.size = " << std::to_string(vslist.size()) << "\n";
  for (const auto &variantSet : vslist) {
    if (variantSet.second.variantSet.empty()) {
      continue;
    }

    ss << pprint::Indent(indent) << "variantSet " << quote(variantSet.first)
       << " = {\n";

    for (const auto &item : variantSet.second.variantSet) {
      ss << pprint::Indent(indent + 1) << quote(item.first) << " ";

      if (item.second.metas().authored()) {
        ss << "(\n";
        ss << print_prim_metas(item.second.metas(), indent + 2);
        ss << pprint::Indent(indent + 1) << ") ";
      }

      ss << "{\n";

      // props
      ss << print_props(item.second.props(), indent + 2);

      // primChildren
      // TODO: print child Prims based on `primChildren` Prim metadata
      const auto &variantPrimMetas = item.second.metas();
      const auto &variantPrimChildren = item.second.children();

      if (variantPrimMetas.primChildren.size() == variantPrimChildren.size()) {
        std::map<std::string, const PrimSpec *> primNameTable;
        for (size_t i = 0; i < variantPrimChildren.size(); i++) {
          primNameTable.emplace(variantPrimChildren[i].name(),
                                &variantPrimChildren[i]);
        }

        for (size_t i = 0; i < variantPrimMetas.primChildren.size(); i++) {
          value::token nameTok = variantPrimMetas.primChildren[i];
          DCOUT(fmt::format("variantPrimChildren  {}/{} = {}", i,
                            variantPrimMetas.primChildren.size(),
                            nameTok.str()));
          const auto it = primNameTable.find(nameTok.str());
          if (it != primNameTable.end()) {
            ss << prim::print_primspec(*(it->second), indent + 2);
          } else {
            // TODO: Report warning?
          }
        }
      } else {
        for (const auto &child : variantPrimChildren) {
          ss << prim::print_primspec(child, indent + 2);
        }
      }

      // ss << "# variantSet end\n";
      ss << pprint::Indent(indent + 1) << "}\n";
    }

    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

// prim:: namespace wrappers
namespace prim {

std::string print_references(const ReferenceList &references,
                             const uint32_t indent) {
  // Delegate to anonymous namespace implementation
  std::stringstream ss;
  for (const auto &ref_op : references) {
    std::stringstream rs;
    auto listEditQual = std::get<0>(ref_op);
    auto vars = std::get<1>(ref_op);
    rs << pprint::Indent(indent);
    if (listEditQual != ListEditQual::ResetToExplicit) {
      rs << to_string(listEditQual) << " ";
    }
    rs << "references = ";
    if (vars.empty()) {
      rs << "None";
    } else {
      if (vars.size() == 1) {
        rs << vars[0];
      } else {
        rs << vars;
      }
    }
    rs << "\n";
    ss << rs.str();
  }
  return ss.str();
}

std::string print_payload(const PayloadList &payload, const uint32_t indent) {
  // Delegate to file-scope implementation
  return lightusd::print_payload(payload, indent);
}

std::string print_layeroffset(const LayerOffset &layeroffset,
                              const uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent);

  bool print_offset{true};
  bool print_scale{true};

  if (std::fabs(layeroffset._offset) < std::numeric_limits<double>::epsilon()) {
    print_offset = false;
  }

  if (std::fabs(layeroffset._scale - 1.0) < std::numeric_limits<double>::epsilon()) {
    print_scale = false;
  }

  if (!print_offset && !print_scale) {
    return std::string();
  }

  ss << "(";
  if (print_offset && print_scale) {
    ss << "offset = " << dtos(layeroffset._offset)
       << ", scale = " << dtos(layeroffset._scale);
  } else if (print_offset) {
    ss << "offset = " << dtos(layeroffset._offset);
  } else {  // print_scale
    ss << "scale = " << dtos(layeroffset._scale);
  }
  ss << ")";

  return ss.str();
}

}  // namespace prim

}  // namespace lightusd
