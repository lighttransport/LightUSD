// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Metadata, property, and structural printing (extracted from pprinter.hh).
//
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "pprint-enum.hh"  // enum to_string converters + pprint::Indent
#include "core/prim-metas.hh"     // PrimMeta, AttrMeta
#include "core/property.hh"       // Property
#include "core/meta-variable.hh"  // MetaVariable, CustomDataType, VariantSelectionMap
#include "core/variant-types.hh"  // VariantSet, VariantSetSpec
#include "core/xform-op.hh"       // XformOp
#include "core/composition-types.hh" // Reference, Payload, LayerOffset
#include "core/prim-spec.hh"         // ReferenceList, PayloadList
#include "core/material-binding.hh"  // MaterialBinding
#include "core/collection-api.hh"    // Collection
#include "core/model-scope.hh"       // Model, Scope (needed by downstream pprinter.hh)
#include "value-pprint.hh"

namespace lightusd {

class Layer;  // Forward declaration for operator<< below

std::string print_prim_metas(const PrimMeta &meta, const uint32_t indent);
std::string print_attr_metas(const AttrMeta &meta, const uint32_t indent);

// varname = optional variable name which is used when meta.get_name() is empty.
std::string print_meta(const MetaVariable &meta, const uint32_t indent,
                       bool emit_type_name,
                       const std::string &varname = std::string());

std::string print_customData(const CustomDataType &customData,
                             const std::string &name, const uint32_t indent);

std::string print_variantSelectionMap(const VariantSelectionMap &m,
                                      const uint32_t indent);

std::string print_variantSetStmt(
    const std::map<std::string, VariantSet> &vslist, const uint32_t indent);

std::string print_variantSetSpecStmt(
    const std::map<std::string, VariantSetSpec> &vslist,
    const uint32_t indent);

std::string print_rel_prop(const Property &prop, const std::string &name,
                           uint32_t indent);

std::string print_prop(const Property &prop, const std::string &prop_name,
                       uint32_t indent);

// Print properties.
std::string print_props(const std::map<std::string, Property> &props,
                        uint32_t indent);

// tok_table: Manages property is already printed(built-in props) or not.
// propNames: Specify the order of property to print
// When `propNames` is empty, print all of items in `props`.
std::string print_props(const std::map<std::string, Property> &props,
                        /* input */ std::set<std::string> &tok_table,
                        const std::vector<value::token> &propNames,
                        uint32_t indent);

std::string print_xformOpOrder(const std::vector<XformOp> &xformOps,
                               const uint32_t indent);
std::string print_xformOps(const std::vector<XformOp> &xformOps,
                           const uint32_t indent);

std::string print_material_binding(const MaterialBinding *mb,
                                   const uint32_t indent);
std::string print_collection(const Collection *coll, const uint32_t indent);

std::string print_timesamples(const value::TimeSamples &v,
                              const uint32_t indent);

namespace prim {

std::string print_references(const ReferenceList &references,
                             const uint32_t indent);
std::string print_payload(const PayloadList &payload, const uint32_t indent);
std::string print_layeroffset(const LayerOffset &layeroffset,
                              const uint32_t indent);

}  // namespace prim

}  // namespace lightusd

namespace std {

std::ostream &operator<<(std::ostream &ofs, const lightusd::Visibility v);
std::ostream &operator<<(std::ostream &ofs, const lightusd::Extent v);
std::ostream &operator<<(std::ostream &ofs, const lightusd::Interpolation v);
std::ostream &operator<<(std::ostream &ofs, const lightusd::Layer &layer);

// StringData needs proper quoting for USDA output
std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::StringData &v);

}  // namespace std
