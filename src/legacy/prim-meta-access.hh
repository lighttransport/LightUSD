// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Internal: GetPrimMeta() — dispatches v.as<SchemaType>() across every USD prim
// type to fetch the embedded PrimMeta& from a type-erased schema-prim Value.
// Defined (with GetPrimElementName/SetPrimElementName/CastToXformable, the sibling
// schema dispatchers — those are declared in core/prim.hh) in prim-types-schema.cc,
// which carries all 8 schema headers. Called by Prim::metas() in prim-types.cc.
// Split out so prim-types.cc (Path/Prim/PrimMetas/Property) need not parse the
// schema headers. Not part of the public API.
#pragma once

namespace lightusd {

struct PrimMetas;
using PrimMeta = PrimMetas;
namespace value {
class Value;
}  // namespace value

const PrimMeta *GetPrimMeta(const value::Value &v);
PrimMeta *GetPrimMeta(value::Value &v);

}  // namespace lightusd
