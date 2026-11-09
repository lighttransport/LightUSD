// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Forward declarations for prim-types.hh
// Include this instead of prim-types.hh when only forward declarations are needed.
// This significantly reduces compilation time for headers and translation units
// that only use pointers or references to these types.
//
#pragma once

namespace lightusd {

// Core types from prim-types.hh
class Prim;
class PrimSpec;
class PrimNode;
class Layer;
class Stage;

// Path and property types
class Path;
class Property;
class Attribute;
class Relationship;
class RelationshipProperty;

// Metadata types
class MetaVariable;
class MetadataBase;

// Collection and binding types
class Collection;
class MaterialBinding;

// Layer composition types
struct LayerOffset;
struct Reference;
struct Payload;
struct SubLayer;
struct VariantSet;
struct VariantSelectionMap;

// Primvar and attribute types
template<typename T> class TypedAttribute;
template<typename T> class TypedAttributeWithFallback;
template<typename T> class TypedConnection;
template<typename T> struct Animatable;

// Extent
struct Extent;

}  // namespace lightusd
