// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Enum / Path string converters (extracted from pprinter.hh).
//
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "pprint-buf.hh"           // pprint::ScratchRef
#include "core/prim-enums.hh"        // Specifier, Permission, Variability, SpecType, Kind, Axis, Visibility, Orientation, Interpolation, Purpose
#include "core/path.hh"              // Path
#include "core/composition-types.hh" // Reference, Payload, APISchemas
#include "core/meta-variable.hh"     // CustomDataType
#include "core/collection-api.hh"    // CollectionInstance
#include "core/xform-op.hh"          // XformOp
#include "core/list-op.hh"           // ListOp
#include "core/extent.hh"            // Extent
#include "value-types.hh"            // value:: numeric types

namespace lightusd {

namespace pprint {

// Declared here, defined in pprint-meta.cc (or pprinter.cc).
void SetIndentString(const std::string &s);
std::string Indent(uint32_t level);

// Column-wrap support for numeric arrays.
// SetColumnLimit(0) disables wrapping (default).
void SetColumnLimit(uint32_t limit);
uint32_t GetColumnLimit();
uint32_t GetPrefixColumns();

// Opt-in: emit prim children and properties in AUTHORED order (recovered from
// the crate's `primChildren`/`properties` order fields), matching OpenUSD's
// usdcat. Default false = existing lightusd behavior (lexicographical for
// flattened crates / sorted property maps). When enabled, the USDC Layer reader
// records the order metadata and the USDA writers order children/properties by
// it (entries listed in the order field first, any remainder lexicographically).
void SetPreserveAuthoredOrder(bool enable);
bool GetPreserveAuthoredOrder();

// Opt-in: emit USDA text laid out like OpenUSD's usdcat -- the prim-metadata
// opening paren on the same line as the `def`/`over`/`class` (e.g.
// `def Mesh "M" (`), and plain (non-indented) blank lines between sibling prims.
// Default false = existing lightusd layout (paren on its own line; indented
// blank separators). Affects the PrimSpec (Layer) writer.
void SetUSDTextFormat(bool enable);
bool GetUSDTextFormat();

// RAII helper: sets prefix_columns in thread-local context, restores on
// destruction. Use before emitting an array value so that operator<< can
// compute continuation indentation.
class ScopedPrefixColumns {
 public:
  explicit ScopedPrefixColumns(uint32_t cols);
  ~ScopedPrefixColumns();
  ScopedPrefixColumns(const ScopedPrefixColumns &) = delete;
  ScopedPrefixColumns &operator=(const ScopedPrefixColumns &) = delete;

 private:
  uint32_t prev_;
};

// Column-aware array formatting (defined in value-pprint.cc).
// Takes pre-formatted element strings and packs them into lines.
std::string format_wrapped_array(const std::vector<std::string> &elements,
                                 uint32_t prefix_cols, uint32_t column_limit);

// Non-template 1D-array emitter (defined in value-pprint.cc). Takes
// pre-stringified elements; `wrappable` selects column wrapping vs plain
// "[a, b, c]" join. Lets the templated array operator<< stay thin.
void print_1d_array(std::ostream &os, const std::vector<std::string> &elems,
                    bool wrappable);

// Compile-time trait: true for value types whose arrays should be
// column-wrapped (i.e. numeric / geometric types, not strings/tokens).
template <typename T>
inline constexpr bool is_wrappable_element_v =
    std::is_same_v<T, float> || std::is_same_v<T, double> ||
    std::is_same_v<T, value::half> ||
    std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t> ||
    std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> ||
    std::is_same_v<T, value::half2> || std::is_same_v<T, value::half3> ||
    std::is_same_v<T, value::half4> ||
    std::is_same_v<T, value::int2> || std::is_same_v<T, value::int3> ||
    std::is_same_v<T, value::int4> ||
    std::is_same_v<T, value::uint2> || std::is_same_v<T, value::uint3> ||
    std::is_same_v<T, value::uint4> ||
    std::is_same_v<T, value::float2> || std::is_same_v<T, value::float3> ||
    std::is_same_v<T, value::float4> ||
    std::is_same_v<T, value::double2> || std::is_same_v<T, value::double3> ||
    std::is_same_v<T, value::double4> ||
    std::is_same_v<T, value::quath> || std::is_same_v<T, value::quatf> ||
    std::is_same_v<T, value::quatd> ||
    std::is_same_v<T, value::normal3h> || std::is_same_v<T, value::normal3f> ||
    std::is_same_v<T, value::normal3d> ||
    std::is_same_v<T, value::vector3h> || std::is_same_v<T, value::vector3f> ||
    std::is_same_v<T, value::vector3d> ||
    std::is_same_v<T, value::point3h> || std::is_same_v<T, value::point3f> ||
    std::is_same_v<T, value::point3d> ||
    std::is_same_v<T, value::color3h> || std::is_same_v<T, value::color3f> ||
    std::is_same_v<T, value::color3d> ||
    std::is_same_v<T, value::color4h> || std::is_same_v<T, value::color4f> ||
    std::is_same_v<T, value::color4d> ||
    std::is_same_v<T, value::texcoord2h> || std::is_same_v<T, value::texcoord2f> ||
    std::is_same_v<T, value::texcoord2d> ||
    std::is_same_v<T, value::texcoord3h> || std::is_same_v<T, value::texcoord3f> ||
    std::is_same_v<T, value::texcoord3d> ||
    std::is_same_v<T, value::matrix2d> || std::is_same_v<T, value::matrix3d> ||
    std::is_same_v<T, value::matrix4d>;

}  // namespace pprint

// --- simple enum/struct to_string converters (core/ headers) ---------

std::string to_string(Visibility v);
std::string to_string(Orientation o);
std::string to_string(Extent e);
std::string to_string(Interpolation interp);
std::string to_string(Axis axis);
std::string to_string(ListEditQual qual);
std::string to_string(Specifier specifier);
std::string to_string(Purpose purpose);
std::string to_string(Permission permission);
std::string to_string(Variability variability);
std::string to_string(SpecType spec_type);
std::string to_string(Kind kind);

std::string to_string(const Reference &reference);
std::string to_string(const Payload &payload);

std::string to_string(const XformOp::OpType &ty);

std::string to_string(const Path &path, bool show_full_path = true);
std::string to_string(const std::vector<Path> &v, bool show_full_path = true);

// For debugging.
std::string dump_path(const Path &p);

std::string to_string(const APISchemas::APIName &name);
std::string to_string(const CustomDataType &customData);

std::string to_string(const CollectionInstance::ExpansionRule rule);

// Forward-declare to_string(string) so the vector<T> template below can find
// it via unqualified lookup when T = std::string (ADL won't help because
// std::string lives in namespace std, not lightusd).
std::string to_string(const std::string &s);

// --- generic template helpers (inline) ------------------------------------

template <typename T>
std::string to_string(const std::vector<T> &v, const uint32_t level = 0) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << pprint::Indent(level) << "[";
  for (size_t i = 0; i < v.size(); i++) {
    ss << to_string(v[i]);
    if (i != (v.size() - 1)) {
      ss << ", ";
    }
  }
  ss << "]";
  return pp_ss.str();
}

template <typename T>
std::string to_string(const ListOp<T> &op, const uint32_t indent_level = 0) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << pprint::Indent(indent_level) << "ListOp(isExplicit " << op.IsExplicit()
     << ") {\n";
  ss << pprint::Indent(indent_level)
     << "  explicit_items = " << to_string(op.GetExplicitItems()) << "\n";
  ss << pprint::Indent(indent_level)
     << "  added_items = " << to_string(op.GetAddedItems()) << "\n";
  ss << pprint::Indent(indent_level)
     << "  appended_items = " << to_string(op.GetAppendedItems()) << "\n";
  ss << pprint::Indent(indent_level)
     << "  prepended_items = " << to_string(op.GetPrependedItems()) << "\n";
  ss << pprint::Indent(indent_level)
     << "  deleted_items = " << to_string(op.GetDeletedItems()) << "\n";
  ss << pprint::Indent(indent_level)
     << "  ordered_items = " << to_string(op.GetOrderedItems()) << "\n";
  ss << pprint::Indent(indent_level) << "}";
  return pp_ss.str();
}

}  // namespace lightusd
