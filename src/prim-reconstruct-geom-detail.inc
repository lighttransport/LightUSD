// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Shared helpers for prim reconstruction, used by prim-reconstruct.cc and
// prim-reconstruct-geom.cc. The anonymous-namespace helpers get internal linkage
// per including TU. Requires prim-reconstruct.hh (ReconstructXformOpsFromProperties),
// common-macros.inc, prim-property-tables.hh, and prim-reconstruct-common.inc to be
// included first (as in both including TUs).

namespace {

[[maybe_unused]] bool ReconstructMaterialBindingProperties(
  ParsePropertyTable &table, /* inout */
  std::map<std::string, Property> &properties,
  MaterialBinding *mb, /* inout */
  std::string *err)
{

  if (!mb) {
    return false;
  }

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, mb->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, mb->materialBindingPreview)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, mb->materialBindingFull)
    // material:binding:collection
    if (prop.first == kMaterialBindingCollection) {

      if (table.count(prop.first)) {
         continue;
      }

      if (!prop.second.is_relationship()) {
        PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", prop.first));
      }

      const Relationship &rel = prop.second.get_relationship();

      mb->set_materialBindingCollection(value::token(""), value::token(""), rel);

      table.insert(prop.first);
      continue;
    }
    // material:binding:collection[:PURPOSE]:NAME
    if (startsWith(prop.first, kMaterialBindingCollection + std::string(":"))) {

      if (table.count(prop.first)) {
         continue;
      }

      if (!prop.second.is_relationship()) {
        PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", prop.first));
      }

      std::string collection_name = removePrefix(prop.first, kMaterialBindingCollection + std::string(":"));
      if (collection_name.empty()) {
        PUSH_ERROR_AND_RETURN("empty NAME is not allowed for 'mateirial:binding:collection'");
      }
      std::vector<std::string> names = split(collection_name, ":");
      if (names.size() > 2) {
        PUSH_ERROR_AND_RETURN("3 or more namespaces is not allowed for 'mateirial:binding:collection'");
      }
      value::token mat_purpose; // empty = all-purpose
      if (names.size() == 1) {
        collection_name = names[0];
      } else {
        mat_purpose = value::token(names[0]);
        collection_name = names[1];
      }

      const Relationship &rel = prop.second.get_relationship();

      mb->set_materialBindingCollection(value::token(collection_name), mat_purpose, rel);

      table.insert(prop.first);
      continue;
    }
    // material:binding:PURPOSE
    if (startsWith(prop.first, kMaterialBinding + std::string(":"))) {

      if (table.count(prop.first)) {
         continue;
      }

      if (!prop.second.is_relationship()) {
        PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", prop.first));
      }

      std::string purpose_name = removePrefix(prop.first, kMaterialBinding + std::string(":"));
      if (purpose_name.empty()) {
        PUSH_ERROR_AND_RETURN("empty PURPOSE is not allowed for 'mateirial:binding:'");
      }
      std::vector<std::string> names = split(purpose_name, ":");
      if (names.size() > 1) {
        PUSH_ERROR_AND_RETURN(fmt::format("PURPOSE `{}` must not have nested namespaces for 'mateirial:binding'", purpose_name));
      }
      value::token mat_purpose = value::token(names[0]);

      const Relationship &rel = prop.second.get_relationship();

      mb->set_materialBinding(rel, mat_purpose);

      table.insert(prop.first);
      continue;
    }
  }

  return true;
}

[[maybe_unused]] bool ReconstructCollectionProperties(
  ParsePropertyTable &table, /* inout */
  std::map<std::string, Property> &properties,
  Collection *coll, /* inout */
  std::string *warn,
  std::string *err,
  bool strict_allowedToken_check)
{
  constexpr auto kCollectionPrefix = "collection:";

  // Use centralized enum handler
  std::function<nonstd::expected<CollectionInstance::ExpansionRule, std::string>(const std::string &)> ExpansionRuleEnumHandler = enum_handler::ExpansionRule;

  if (!coll) {
    return false;
  }

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    if (startsWith(prop.first, kCollectionPrefix)) {
      if (table.count(prop.first)) {
         continue;
      }

      std::string suffix = removePrefix(prop.first, kCollectionPrefix);
      std::vector<std::string> names = split(suffix, ":");
      if (names.size() != 2) {
        PUSH_ERROR_AND_RETURN(fmt::format("Invalid collection property name. Must be 'collection:INSTANCE_NAME:<prop_name>' but got '{}'",  prop.first));
      }
      if (names[0].empty()) {
        PUSH_ERROR_AND_RETURN("INSTANCE_NAME is empty for collection property name");
      }
      if (names[1].empty()) {
        PUSH_ERROR_AND_RETURN("Collection property name is empty");
      }

      std::string instance_name = names[0];

      if (names[1] == "includes") {

        if (!prop.second.is_relationship()) {
          PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", prop.first));
        }

        CollectionInstance &coll_instance = coll->get_or_add_instance(instance_name);
        coll_instance.includes = prop.second.get_relationship();
        table.insert(prop.first);

      } else if (names[1] == "expansionRule") {

        TypedAttributeWithFallback<CollectionInstance::ExpansionRule> r{CollectionInstance::ExpansionRule::ExpandPrims};

        PARSE_UNIFORM_ENUM_PROPERTY_NOCONTINUE(table, prop, prop.first, CollectionInstance::ExpansionRule, ExpansionRuleEnumHandler, CollectionInstance,
                       r, strict_allowedToken_check)

        if (table.count(prop.first)) {
          CollectionInstance &coll_instance = coll->get_or_add_instance(instance_name);
          coll_instance.expansionRule = r;  // Assign full TypedAttributeWithFallback to preserve authored state
        }
      } else if (names[1] == "includeRoot") {

        TypedAttributeWithFallback<Animatable<bool>> includeRoot{false};
        PARSE_TYPED_ATTRIBUTE_NOCONTINUE(table, prop, prop.first, CollectionInstance, includeRoot)

        if (table.count(prop.first)) {
          CollectionInstance &coll_instance = coll->get_or_add_instance(instance_name);
          coll_instance.includeRoot = includeRoot;
        }
      } else if (names[1] == "excludes") {

        if (!prop.second.is_relationship()) {
          PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", prop.first));
        }

        CollectionInstance &coll_instance = coll->get_or_add_instance(instance_name);
        coll_instance.excludes = prop.second.get_relationship();
        table.insert(prop.first);

      } else if (names[1] == "membershipExpression") {

        TypedAttribute<value::PathExpression> membershipExpression;
        PARSE_TYPED_ATTRIBUTE_NOCONTINUE(table, prop, prop.first, CollectionInstance, membershipExpression)

        if (table.count(prop.first)) {
          CollectionInstance &coll_instance = coll->get_or_add_instance(instance_name);
          coll_instance.membershipExpression = membershipExpression;
        }

      }
    }
  }

  return true;
}
// xformOps and built-in props
[[maybe_unused]] bool ReconstructGPrimProperties(
  const Specifier &spec,
  ParsePropertyTable &table, /* inout */
  std::map<std::string, Property> &properties,
  GPrim *gprim, /* inout */
  std::string *warn,
  std::string *err,
  bool strict_allowedToken_check)
{

  (void)warn;
  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &gprim->xformOps, err)) {
    return false;
  }

  if (!prim::ReconstructMaterialBindingProperties(table, properties, gprim, err)) {
    return false;
  }

  if (!prim::ReconstructCollectionProperties(
    table, properties, gprim, warn, err, strict_allowedToken_check)) {
    return false;
  }

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kProxyPrim, gprim->proxyPrim)
    PARSE_TYPED_ATTRIBUTE(table, prop, "doubleSided", GPrim, gprim->doubleSided)
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, GPrim,
                   gprim->visibility, strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, "purpose", Purpose, PurposeEnumHandler, GPrim,
                       gprim->purpose, strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, "orientation", Orientation, OrientationEnumHandler, GPrim,
                       gprim->orientation, strict_allowedToken_check)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GPrim, gprim->extent)
  }

  return true;
}

} // namespace local

#define RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(PrimClass, prim_ptr, TYPED_ATTRS, ENUM_EXPANSION) \
  (void)references; \
  \
  ParsePropertyTable table; \
  if (!ReconstructGPrimProperties(spec, table, properties, prim_ptr, warn, err, \
                                   options.strict_allowedToken_check)) { \
    return false; \
  } \
  \
  for (auto &prop : properties) { \
    TYPED_ATTRS(EXPAND_TYPED_ATTR) \
    ENUM_EXPANSION \
    ADD_PROPERTY(table, prop, PrimClass, prim_ptr->props) \
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop) \
  } \
  \
  return true;
