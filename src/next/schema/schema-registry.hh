// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include "../layer/prim-spec.hh"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

struct SchemaPropertyDefinition {
  std::string schema_type;
  std::string name;
  std::string type_name;
  Value fallback;
  bool has_fallback = false;
  // Offset of the "__INSTANCE__" placeholder in `name`, or npos when the name
  // is literal. Fixed for the lifetime of the definition, so it is computed
  // once at registration: FindProperty walks every definition on every call,
  // and re-running name.find("__INSTANCE__") there made that substring search
  // one of the hottest operations in a material-heavy load.
  std::size_t instance_marker_pos = std::string::npos;
};

/// Compact built-in prim-definition registry shared by stage population,
/// value resolution, and validation. It intentionally covers the schemas
/// implemented by next; generated expansion can add definitions without
/// changing the resolver.
class SchemaRegistry {
 public:
  const SchemaPropertyDefinition* FindProperty(
      const PrimSpec& prim, const std::string& property_name) const;
  std::vector<std::string> PropertyNames(const PrimSpec& prim) const;
  /// Whether the identifier is in the supported schema surface. Recognition
  /// does not imply that every schema property has a registered definition.
  bool IsKnownSchema(const std::string& schema_type) const;
  std::vector<std::string> SchemaTypes() const;
  /// True when `schema_type` is `ancestor` or reaches it through the
  /// registered parents chain (e.g. InheritsFrom("Volume", "Gprim")).
  bool InheritsFrom(const std::string& schema_type,
                    const std::string& ancestor) const;
  /// True when the registry records an inheritance parent for `schema_type`
  /// (i.e. its ancestry is KNOWN — InheritsFrom answers are meaningful).
  bool HasParentEntry(const std::string& schema_type) const;
  /// AOUSD Core 1.1 "Typed Schemas": a composed prim whose typeName names an
  /// abstract typed schema (Imageable, Gprim, ...), an API schema, or an
  /// unidentifiable name is typeless and receives no typed-schema fallbacks.
  /// True only for a name that can be a concrete prim type here.
  bool IsConcreteTypedSchemaName(const std::string& schema_type) const;
  /// Abstract typed schemas known to the registry (OpenUSD v26.11 parity).
  static bool IsAbstractTypedSchemaName(const std::string& schema_type);
  /// UsdPrim::HasAPI semantics on a composed prim: true when `api_schema`
  /// is applied — authored in the composed apiSchemas, built into the prim's
  /// typed schema (e.g. LightAPI on SphereLight), or auto-applied (the 3D
  /// Gaussian ParticleField APIs). For a multiple-apply schema an empty
  /// `instance` matches any applied instance, otherwise only
  /// "<api_schema>:<instance>". A single-apply schema never matches a
  /// non-empty instance.
  bool HasAPI(const PrimSpec& prim, const std::string& api_schema,
              const std::string& instance = std::string()) const;
  /// The prim's applied API schemas in pxr GetAppliedSchemas order: built-in
  /// (typed-schema / auto-applied) entries first, then authored ones.
  std::vector<std::string> AppliedSchemas(const PrimSpec& prim) const;

 private:
  SchemaRegistry();
  friend const SchemaRegistry& GetSchemaRegistry();

  std::vector<SchemaPropertyDefinition> properties_;
  std::vector<std::pair<std::string, std::string>> parents_;
  std::vector<std::string> known_schemas_;
};

const SchemaRegistry& GetSchemaRegistry();

/// AOUSD Core 1.1 "Schema Families and Versioning": split a schema name into
/// its family and optional version. `SchemaName <- Family ('_' Version)?`,
/// matching the LAST `_<digits>` suffix; the family must be non-empty (so
/// `_1` is the unversioned family `_1`, per the grammar's AsciiStart).
/// `Gaussian_Splatting_3` -> (`Gaussian_Splatting`, 3); `MySchema_` and
/// `Foo_1a` are unversioned. Versions saturate at UINT32_MAX like OpenUSD.
/// Returns false only for an empty name.
bool ParseSchemaFamilyAndVersion(const std::string& name, std::string* family,
                                 uint32_t* version, bool* has_version);

}  // namespace next
}  // namespace lightusd
