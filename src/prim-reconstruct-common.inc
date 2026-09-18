#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-template"
#endif

// prim-reconstruct-common.inc
// Shared infrastructure for prim-reconstruct split modules.
// Included by prim-reconstruct.cc and prim-reconstruct-shader.cc
//
// This file must be included inside namespace lightusd::prim after
// the ReconstructShader template declaration.

namespace {


struct ParseResult
{
  enum class ResultCode
  {
    Success,
    Unmatched,
    AlreadyProcessed,
    TypeMismatch,
    VariabilityMismatch,
    ConnectionNotAllowed,
    InvalidConnection,
    PropertyTypeMismatch,
    InternalError,
  };

  ResultCode code;
  std::string err;
  std::string warn;
};


template<typename T>
static nonstd::optional<Animatable<T>> ConvertToAnimatable(const primvar::PrimVar &var)
{
  Animatable<T> dst;

  if (!var.has_value() && !var.has_timesamples()) {
    DCOUT("PrimVar has neither value nor timesamples");
    return nonstd::nullopt;
  }

  bool ok = false;

  if (var.has_value()) {

    if (auto pv = var.get_value<T>()) {
      dst.set_default(pv.value());

      ok = true;
      //return std::move(dst);
    }
  }

  if (var.has_timesamples()) {
    const auto &samples = var.ts_raw().get_samples();

    for (size_t i = 0; i < samples.size(); i++) {
      const value::TimeSamples::Sample &s = samples[i];

      // Attribute Block?
      if (s.blocked || s.value.is_none()) {
        dst.add_blocked_sample(s.t);
      } else if (auto pv = s.value.get_value<T>()) {
        dst.add_sample(s.t, pv.value());
      } else {
        // Type mismatch
        DCOUT(i << "/" << var.ts_raw().size() << " type mismatch. expected " << value::TypeTraits<T>::type_name() << ", but got " << s.value.type_name());
        return nonstd::nullopt;
      }
    }

    ok = true;
  }

  if (ok) {
    return std::move(dst);
  }

  DCOUT("???");
  return nonstd::nullopt;
}

// Require special treatment for Extent(float3[2])
template<>
nonstd::optional<Animatable<Extent>> ConvertToAnimatable(const primvar::PrimVar &var)
{
  Animatable<Extent> dst;

  if (!var.has_value() && !var.has_timesamples()) {
    DCOUT("PrimVar has neither value nor timesamples");
    return nonstd::nullopt;
  }

  bool value_ok = false;

  if (var.has_default()) {

    if (auto pv = var.get_value<std::vector<value::float3>>()) {
      if (pv.value().size() == 2) {
        Extent ext;
        ext.lower = pv.value()[0];
        ext.upper = pv.value()[1];

        dst.set_default(ext);

      } else {
        return nonstd::nullopt;
      }

      //return std::move(dst);
    }
    value_ok = true;
  }

  if (var.has_timesamples()) {
    for (size_t i = 0; i < var.ts_raw().size(); i++) {
      const value::TimeSamples::Sample &s = var.ts_raw().get_samples()[i];

      // Attribute Block?
      if (s.blocked || s.value.is_none()) {
        dst.add_blocked_sample(s.t);
      } else if (auto pv = s.value.get_value<std::vector<value::float3>>()) {
        if (pv.value().size() == 2) {
          Extent ext;
          ext.lower = pv.value()[0];
          ext.upper = pv.value()[1];
          dst.add_sample(s.t, ext);
        } else {
          DCOUT(i << "/" << var.ts_raw().size() << " array size mismatch.");
          return nonstd::nullopt;
        }
      } else {
        // Type mismatch
        DCOUT(i << "/" << var.ts_raw().size() << " type mismatch.");
        return nonstd::nullopt;
      }
    }

    value_ok = true;
    //return std::move(dst);
  }

  if (value_ok) {
    return std::move(dst);
  }

  DCOUT("???");
  return nonstd::nullopt;
}



// ============================================================================
// Traits for ParseTypedAttribute unification
// ============================================================================

// Detect if type is Animatable<T>
template<typename T>
struct is_animatable : std::false_type {
  using value_type = T;
};

template<typename T>
struct is_animatable<Animatable<T>> : std::true_type {
  using value_type = T;
};

// Detect if type is TypedAttributeWithFallback<T>
template<typename T>
struct is_with_fallback : std::false_type {};

template<typename T>
struct is_with_fallback<TypedAttributeWithFallback<T>> : std::true_type {};

// Extract value type from target
template<typename Target>
struct target_traits; // Forward declaration

// Specialization for TypedAttribute<T>
template<typename T>
struct target_traits<TypedAttribute<T>> {
  using value_type = T;
  static constexpr bool has_fallback = false;
  static constexpr bool is_varying = is_animatable<T>::value;
  using underlying_type = typename is_animatable<T>::value_type;
};

// Specialization for TypedAttributeWithFallback<T>
template<typename T>
struct target_traits<TypedAttributeWithFallback<T>> {
  using value_type = T;
  static constexpr bool has_fallback = true;
  static constexpr bool is_varying = is_animatable<T>::value;
  using underlying_type = typename is_animatable<T>::value_type;
};

// ============================================================================
// Unified ParseTypedAttribute implementation
// ============================================================================

template<typename Target>
static ParseResult ParseTypedAttributeUnified(
    std::set<std::string> &table,
    const std::string prop_name,
    Property &prop,
    const std::string &name,
    Target &target)
{
  using Traits = target_traits<Target>;
  using T = typename Traits::underlying_type;
  constexpr bool is_varying = Traits::is_varying;
  constexpr bool has_fallback = Traits::has_fallback;

  ParseResult ret;

  if (prop_name.compare(name) != 0) {
    ret.code = ParseResult::ResultCode::Unmatched;
    return ret;
  }

  // Check if property is relationship (should be attribute)
  if (prop.is_relationship()) {
    ret.code = ParseResult::ResultCode::PropertyTypeMismatch;
    ret.err = fmt::format("Property `{}` must be Attribute, but declared as Relationship.", name);
    return ret;
  }

  Attribute &attr = prop.attribute();
  std::string attr_type_name = attr.type_name();

  // Check type match. Role types (for example color4f) are accepted when
  // their underlying storage matches the schema type (for example float4).
  const uint32_t target_underlying_type_id =
      value::TypeTraits<T>::underlying_type_id();
  const uint32_t attr_underlying_type_id =
      value::GetUnderlyingTypeId(attr_type_name);
  if ((value::TypeTraits<T>::type_name() != attr_type_name) &&
      (value::TypeTraits<T>::underlying_type_name() != attr_type_name) &&
      (target_underlying_type_id == value::TYPE_ID_INVALID ||
       attr_underlying_type_id == value::TYPE_ID_INVALID ||
       target_underlying_type_id != attr_underlying_type_id)) {
    // A connection-only property (`X.connect = </path>` with no authored value)
    // carries no value to type-check — a connection is a type-agnostic path.
    // Some assets declare it with a non-conformant type (e.g.
    // `token inputs:in.connect` on a float2 input — usd-wg TextureTransformTest).
    // Attach the connection to the typed target rather than failing the type
    // check, which would demote it to a custom property and drop the connection
    // from the typed field (breaking e.g. UsdTransform2d -> PrimvarReader
    // traversal in tydra).
    if (attr.has_connections() && !attr.has_value() &&
        !attr.get_var().has_timesamples()) {
      target.set_connections(attr.connections());
      // Record the connection-only state (authored, but no value) — matches the
      // normal connection-only path and the EmptyAttrib path below.
      target.set_value_empty();
      target.metas() = std::move(prop.attribute().metas());
      table.insert(name);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    }
    DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
    ret.code = ParseResult::ResultCode::TypeMismatch;
    ret.err = fmt::format("Property type mismatch. {} expects type `{}` but defined as type `{}`",
                          name, value::TypeTraits<T>::type_name(), attr_type_name);
    return ret;
  }

  bool has_connections = false;
  bool has_default = false;
  bool has_timesamples = false;

  // The type gate above accepts spellings other than the schema's own (role
  // types over the same storage, e.g. authored `float3` for a color3f field).
  // Record the authored spelling so printers/writers do not silently re-type
  // the attribute (OpenUSD preserves the authored typeName on the spec).
  if (!attr_type_name.empty() &&
      attr_type_name != value::TypeTraits<T>::type_name()) {
    target.set_actual_type_name(attr_type_name);
  }

  // Handle connections
  if (attr.has_connections()) {
    target.set_connections(attr.connections());
    has_connections = true;
  }

  // Handle empty attribute
  if (prop.get_property_type() == Property::Type::EmptyAttrib) {
    DCOUT("Added prop with empty value: " << name);
    target.set_value_empty();
    target.metas() = std::move(prop.attribute().metas());
    table.insert(name);
    ret.code = ParseResult::ResultCode::Success;
    return ret;
  }

  // Handle non-empty attribute
  // Note: Type::Connection means the attribute has connections, but it may also have values.
  // Connections are already extracted above, so we need to also parse any values.
  if (prop.get_property_type() == Property::Type::Attrib ||
      prop.get_property_type() == Property::Type::Connection) {
    DCOUT("Adding typed prop: " << name);

    // Check blocked attribute
    if (attr.is_blocked()) {
      target.set_blocked(true);
      has_default = true;
    } else {
      // Variability checks
      bool is_config_attr = (name.find("config:") == 0);

      if constexpr (!is_varying) {
        // Uniform attribute - no timeSamples allowed
        if (!is_config_attr && attr.variability() != Variability::Uniform) {
          // Allow schema-default varying (not explicitly authored as `varying`)
          // Only error when `varying` was explicitly authored in USDA
          if (!(attr.variability() == Variability::Varying && !attr.is_varying_authored())) {
            ret.code = ParseResult::ResultCode::VariabilityMismatch;
            ret.err = fmt::format("Attribute `{}` must be `uniform` variability.", name);
            return ret;
          }
        }

        // TODO: Re-enable this warning after test files are updated
        // if (is_config_attr && attr.variability() != Variability::Uniform) {
        //   ret.warn = fmt::format("Config attribute `{}` should have explicit `uniform` variability.", name);
        // }

        if (attr.get_var().has_timesamples()) {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = "TimeSample assigned to a property where `uniform` variability is set.";
          return ret;
        }
      }

      // Extract value based on varying vs uniform
      if constexpr (is_varying) {
        // Varying: can have both timeSamples and default
        typename Traits::value_type animatable_value;

        // Check uniform variability for attributes with fallback
        if constexpr (has_fallback) {
          if (attr.variability() == Variability::Uniform) {
            if (attr.get_var().has_timesamples()) {
              ret.code = ParseResult::ResultCode::VariabilityMismatch;
              ret.err = fmt::format("TimeSample value is assigned to `uniform` property `{}`", name);
              return ret;
            }

            if (auto pv = attr.get_var().get_value<T>()) {
              target.set_value(std::move(pv.value()));
              target.metas() = std::move(prop.attribute().metas());
              table.insert(name);
              ret.code = ParseResult::ResultCode::Success;
              return ret;
            } else {
              ret.code = ParseResult::ResultCode::TypeMismatch;
              ret.err = fmt::format("Failed to retrieve value with requested type `{}`.", value::TypeTraits<T>::type_name());
              return ret;
            }
          }
        }

        // Parse timeSamples
        if (attr.get_var().has_timesamples()) {
          if (auto av = ConvertToAnimatable<T>(attr.get_var())) {
            animatable_value = std::move(av.value());
            has_timesamples = true;
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = fmt::format("Converting timeSamples failed for `{}`. TimeSamples may have values with different type (expected `{}`).",
                                  prop_name, value::TypeTraits<T>::type_name());
            return ret;
          }
        }

        // Parse default value (move to avoid copying large arrays)
        if (attr.get_var().has_default()) {
          if (auto pv = attr.get_var().get_value<T>()) {
            animatable_value.set(std::move(pv.value()));
            has_default = true;
          } else {
            ret.code = ParseResult::ResultCode::TypeMismatch;
            ret.err = fmt::format("take_value<{}> failed. Attribute has type {}",
                                  value::TypeTraits<T>::type_name(), attr.get_var().type_name());
            return ret;
          }
        }

        if (has_timesamples || has_default) {
          target.set_value(std::move(animatable_value));
        } else if (!has_connections) {
          // No value, default, timeSamples, or connections - use default-constructed
          target.set_value(typename Traits::value_type());
        }

      } else {
        // Uniform: only default value (move to avoid copying large arrays)
        if (attr.get_var().has_default()) {
          if (auto pv = attr.get_var().get_value<T>()) {
            target.set_value(std::move(pv.value()));
            has_default = true;
          } else {
            ret.code = ParseResult::ResultCode::TypeMismatch;
            ret.err = fmt::format("take_value<{}> failed. Attribute has type {}",
                                  value::TypeTraits<T>::type_name(), attr.get_var().type_name());
            return ret;
          }
        } else if (!has_connections) {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = "TimeSample or corrupted value assigned to a property where `uniform` variability is set.";
          return ret;
        }
      }
    }
  } else {
    ret.code = ParseResult::ResultCode::InternalError;
    ret.err = "ParseTypedAttribute: Invalid Property type(internal error)";
    return ret;
  }

  // Connections only case
  if (has_connections && !has_timesamples && !has_default) {
    target.set_value_empty();
  }

  // Success cases
  if (has_connections || has_timesamples || has_default) {
    target.metas() = std::move(prop.attribute().metas());
    table.insert(name);
    ret.code = ParseResult::ResultCode::Success;
    return ret;
  }

  ret.code = ParseResult::ResultCode::InternalError;
  ret.err = "ParseTypedAttribute: No valid data found(internal error)";
  return ret;
}

// ============================================================================
// Overload wrappers (delegate to unified implementation)
// ============================================================================

// For animatable attribute(`varying`) with fallback
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedAttributeWithFallback<Animatable<T>> &target)
{
  return ParseTypedAttributeUnified(table, prop_name, prop, name, target);
}


// For 'uniform' attribute with fallback
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedAttributeWithFallback<T> &target) /* out */
{
  return ParseTypedAttributeUnified(table, prop_name, prop, name, target);
}


// For animatable attribute(`varying`) without fallback
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedAttribute<Animatable<T>> &target) /* out */
{
  return ParseTypedAttributeUnified(table, prop_name, prop, name, target);
}


// For uniform attribute without fallback
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedAttribute<T> &target) /* out */
{
  return ParseTypedAttributeUnified(table, prop_name, prop, name, target);
}


// Special case for Extent(float3[2]) type.
// TODO: Reuse code of ParseTypedAttribute as much as possible
[[maybe_unused]] static ParseResult ParseExtentAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedAttribute<Animatable<Extent>> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name) == 0 ||
      prop_name.compare(name + ".connect") == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    const Attribute &attr = prop.get_attribute();

    std::string attr_type_name = attr.type_name();
    if (prop.get_property_type() == Property::Type::EmptyAttrib) {
      DCOUT("Added prop with empty value: " << name);
      target.set_value_empty();
      target.metas() = attr.metas();
      table.insert(name);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    } else if (prop.get_property_type() == Property::Type::Attrib ||
               prop.get_property_type() == Property::Type::Connection) {

      //bool has_default{false};
      bool has_connections{false};

      if (attr.has_connections()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        //target.metas() = prop.get_attribute().metas();
        //table.insert(prop_name);
        //ret.code = ParseResult::ResultCode::Success;
        //return ret;
        has_connections = true;
      }

      DCOUT("Adding typed extent attribute: " << name);

      if (attr.is_blocked()) {
        // e.g. "float3[] extent = None"
        target.set_blocked(true);
      }


      const auto &var = attr.get_var();

      if (var.has_default() || var.has_timesamples()) {
        if (auto av = ConvertToAnimatable<Extent>(var)) {
          target.set_value(std::move(av.value()));  // Use move to avoid copy
        } else {
          DCOUT("ConvertToAnimatable failed.");
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Converting Attribute data failed. Maybe TimeSamples have values with different types?";
          return ret;
        }

        DCOUT("Added typed extent attribute: " << name);

        target.metas() = std::move(prop.attribute().metas());  // Move instead of copy
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      }

      if (has_connections) {
        DCOUT("Added Extent connection attribute: " << name);
        target.metas() = std::move(prop.attribute().metas());  // Move instead of copy
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      }


    } else {
      DCOUT("Invalid Property.type");
      ret.err = "[extent] Invalid Property type(internal error)";
      ret.code = ParseResult::ResultCode::InternalError;
      return ret;
    }

  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}


// CheckAllowedTokens template removed - now in enum-handlers.cc

// Allowed syntax:
//   "T varname"
template<typename T>
static ParseResult ParseShaderOutputTerminalAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedTerminalAttribute<T> &target, /* out */
  bool strict_type_check) // false: accept non-conformant authored type with a warning
{
  ParseResult ret;

  if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      goto end;
    }

    if (prop.is_attribute_connection()) {
      ret.code = ParseResult::ResultCode::ConnectionNotAllowed;
      ret.err = "Connection is not allowed for output terminal attribute.";
      goto end;
    }

    if (prop.get_property_type() != Property::Type::EmptyAttrib) {
        DCOUT("Output Invalid shader output terminal attribute");
        ret.err = "No value should be assigned for shader output terminal attribute.";
        ret.code = ParseResult::ResultCode::InvalidConnection;
        goto end;
    }

    {
      const Attribute &attr = prop.get_attribute();

      std::string attr_type_name = attr.type_name();

      bool attr_is_role_type = value::IsRoleType(attr_type_name);

      DCOUT("attrname = " << name);
      DCOUT("value typename = " << value::TypeTraits<T>::type_name());
      DCOUT("attr-type_name = " << attr_type_name);

      // Accept a non-conformant authored type for a shader output terminal:
      // keep the schema type `T`(the canonical type inferred from `info:id` /
      // the UsdPreviewSurface spec) for connection/render semantics, remember
      // the authored type, and report a warning. This matches OpenUSD, which
      // does not validate shader output types. Only used when
      // `strict_type_check` is false.
      auto accept_nonconformant = [&]() {
        target.set_authored(true);
        target.set_actual_type_name(attr_type_name);
        target.metas() = prop.get_attribute().metas();
        table.insert(name);
        ret.warn = fmt::format(
            "Shader output `{}` is declared as type `{}` but `{}` is expected per "
            "the shader schema(UsdPreviewSurface / info:id). Accepting the declared "
            "type; the schema type is used for connection/render semantics(matches "
            "OpenUSD). Set strict_shader_type_check=true to make this an error.",
            name, attr_type_name, value::TypeTraits<T>::type_name());
        ret.code = ParseResult::ResultCode::Success;
      };

      // First check if both types are same, then
      // Allow either type is role-types(e.g. allow color3f attribute for TypedTerminalAttribute<float3>)
      // TODO: Allow both role-types case?(e.g. point3f attribute for TypedTerminalAttribute<vector3f>)
      if (value::TypeTraits<T>::type_name() == attr_type_name) {
        DCOUT("Author output terminal attribute: " << name);
        target.set_authored(true);
        target.metas() = prop.get_attribute().metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        goto end;
      }

      if (value::TypeTraits<T>::is_role_type()) {
        if (attr_is_role_type) {
          if (!strict_type_check) { accept_nonconformant(); goto end; }
          ret.code = ParseResult::ResultCode::TypeMismatch;
          ret.err = fmt::format("Attribute type mismatch. {} expects type `{}` but defined as type `{}`.", name, value::TypeTraits<T>::type_name(), attr_type_name);
          goto end;
        }

        if (value::TypeTraits<T>::underlying_type_name() == attr_type_name) {
          target.set_authored(true);
          target.set_actual_type_name(attr_type_name);
          target.metas() = prop.get_attribute().metas();
          table.insert(name);
          ret.code = ParseResult::ResultCode::Success;
          goto end;
        }

        if (!strict_type_check) { accept_nonconformant(); goto end; }
        ret.code = ParseResult::ResultCode::TypeMismatch;
        ret.err = fmt::format("Attribute type mismatch. {} expects type `{}`(and its underlying types) but defined as type `{}`.", name, value::TypeTraits<T>::type_name(), attr_type_name);
        goto end;
      }

      if (attr_is_role_type) {
        if (value::TypeTraits<T>::is_role_type()) {
          if (!strict_type_check) { accept_nonconformant(); goto end; }
          ret.code = ParseResult::ResultCode::TypeMismatch;
          ret.err = fmt::format("Attribute type mismatch. {} expects type `{}` but defined as type `{}`.", name, value::TypeTraits<T>::type_name(), attr_type_name);
          goto end;
        }

        {
          uint32_t attr_underlying_type_id = value::GetUnderlyingTypeId(attr_type_name);
          if (value::TypeTraits<T>::type_id() == attr_underlying_type_id) {
            target.set_authored(true);
            target.set_actual_type_name(attr_type_name);
            target.metas() = prop.get_attribute().metas();
            table.insert(name);
            ret.code = ParseResult::ResultCode::Success;
            goto end;
          }
        }

        if (!strict_type_check) { accept_nonconformant(); goto end; }
        ret.code = ParseResult::ResultCode::TypeMismatch;
        ret.err = fmt::format("Attribute type mismatch. {} expects type `{}` but defined as type `{}`(and its underlying types).", name, value::TypeTraits<T>::type_name(), attr_type_name);
        goto end;
      }

      DCOUT("attr.type = " << attr_type_name);
      if (!strict_type_check) { accept_nonconformant(); goto end; }
      ret.code = ParseResult::ResultCode::TypeMismatch;
      ret.err = fmt::format("Property type mismatch. {} expects type `{}` but defined as type `{}`.", name, value::TypeTraits<T>::type_name(), attr_type_name);
      goto end;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
end:
  return ret;
}


// Allowed syntax:
//   "token outputs:surface.connect = </path/to/conn/>"
[[maybe_unused]] static ParseResult ParseShaderInputConnectionProperty(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  Property &prop,  // Non-const to allow move from metadata
  const std::string &name,
  TypedConnection<value::token> &target) /* out */
{
  ParseResult ret;
  ret.code = ParseResult::ResultCode::InternalError;

  if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    DCOUT("is_attribute = " << prop.is_attribute());
    DCOUT("is_attribute_connection = " << prop.is_attribute_connection());

    // allow empty value
    if (prop.is_empty()) {
      target.set_empty();
      target.metas() = prop.get_attribute().metas();
      table.insert(prop_name);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    } else if (prop.is_attribute_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set(attr.connections());
        target.metas() = prop.get_attribute().metas();

        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Property is invalid Attribute connection.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = fmt::format("Property `{}` must be Attribute connection.", prop_name);
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// Rel with single targetPath(or empty)
#define PARSE_SINGLE_TARGET_PATH_RELATION(__table, __prop, __propname, __target) \
  if (prop.first == __propname) { \
    if (__table.count(__propname)) { \
       continue; \
    } \
    if (!prop.second.is_relationship()) { \
      PUSH_ERROR_AND_RETURN(fmt::format("Property `{}` must be a Relationship.", __propname)); \
    } \
    const Relationship &rel = prop.second.get_relationship(); \
    if (rel.is_path()) { \
      __target = rel; \
      table.insert(prop.first); \
      DCOUT("Added rel " << __propname); \
      continue; \
    } else if (rel.is_pathvector()) { \
      if (rel.targetPathVector.size() <= 1) { \
        /* 0 targets = None/blocked, 1 target = normal */ \
        __target = rel; \
        table.insert(prop.first); \
        DCOUT("Added rel " << __propname); \
        continue; \
      } \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` has multiple target Paths ({}).", __propname, rel.targetPathVector.size())); \
    } else if (!rel.has_value()) { \
      /* define-only. accept  */ \
      __target = rel; \
      table.insert(prop.first); \
      DCOUT("Added rel " << __propname); \
    } else if (rel.is_blocked()) { \
      __target = rel; \
      table.insert(prop.first); \
      DCOUT("Added ValueBlocked rel " << __propname); \
    } else { \
      PUSH_ERROR_AND_RETURN(fmt::format("Internal error. Property `{}` is not a valid Relationship.", __propname)); \
    } \
  }

// Rel with targetPaths(single path or array of Paths)
#define PARSE_TARGET_PATHS_RELATION(__table, __prop, __propname, __target) \
  if (prop.first == __propname) { \
    if (__table.count(__propname)) { \
       continue; \
    } \
    if (!prop.second.is_relationship()) { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", __propname)); \
    } \
    const Relationship &rel = prop.second.get_relationship(); \
    __target = rel; \
    table.insert(prop.first); \
    DCOUT("Added rel " << __propname); \
    continue; \
  }


#define PARSE_SHADER_TERMINAL_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseShaderOutputTerminalAttribute(__table, __prop.first, __prop.second, __name, __target, options.strict_shader_type_check); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    if (!ret.warn.empty()) { PUSH_WARN(ret.warn); } \
    DCOUT("Added shader terminal attribute: " << __name); \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader output property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}


#define PARSE_SHADER_INPUT_CONNECTION_PROPERTY(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseShaderInputConnectionProperty(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    DCOUT("Added shader input connection: " << __name); \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

// EnumHandler and CheckAllowedTokens templates removed.
// Enum handling is now done via centralized handlers in enum-handlers.cc

} // namespace

#define PARSE_TYPED_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseTypedAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    if (!ret.warn.empty()) { \
      PUSH_WARN(ret.warn); \
    } \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else if (ret.code == ParseResult::ResultCode::TypeMismatch) { \
    /* Sdf-level leniency (matches OpenUSD, which does not type-check builtin \
       attributes at parse time): keep the authored attribute — the trailing \
       ADD_PROPERTY captures it as a custom property — instead of failing the \
       whole prim. The canonical typed field stays unauthored. */ \
    PUSH_WARN(fmt::format("Attribute `{}` declared with a non-conformant type: {} Preserving the authored value as a custom property.", __name, ret.err)); \
    /* do NOT `continue`: fall through so the trailing ADD_PROPERTY captures it */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing attribute `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

// Shader INPUT attribute parse with relaxed type-conformance.
// Same as PARSE_TYPED_ATTRIBUTE, but when the authored Sdf type does not match
// the canonical schema type, behavior depends on options.strict_shader_type_check:
//   true  -> hard error (same as PARSE_TYPED_ATTRIBUTE).
//   false -> warn and leave the property unconsumed so the trailing
//            ADD_PROPERTY captures it as a custom property (no data loss). The
//            typed field stays unauthored; the canonical schema type is kept.
// Intended for UsdPreviewSurface-family shader inputs. Requires `options`
// (PrimReconstructOptions) and ADD_PROPERTY at the end of the parse loop.
#define PARSE_SHADER_INPUT_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseTypedAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    if (!ret.warn.empty()) { PUSH_WARN(ret.warn); } \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else if ((ret.code == ParseResult::ResultCode::TypeMismatch) && !options.strict_shader_type_check) { \
    PUSH_WARN(fmt::format("Shader input `{}` declared with a non-conformant type: {} Accepting; preserving the authored value as a custom property. Set strict_shader_type_check=true to make this an error.", __name, ret.err)); \
    /* do not consume: fall through so the trailing ADD_PROPERTY captures it */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader input property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

#define PARSE_TYPED_ATTRIBUTE_NOCONTINUE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseTypedAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    if (!ret.warn.empty()) { \
      PUSH_WARN(ret.warn); \
    } \
    /* do nothing */ \
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing attribute `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

#define PARSE_EXTENT_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseExtentAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing attribute `extent` failed. Error: {}", ret.err)); \
  } \
}

// Use centralized enum handlers from enum-handlers.hh
// These wrapper functions maintain backwards compatibility with existing macro usage
[[maybe_unused]] static nonstd::expected<Axis, std::string> AxisEnumHandler(const std::string &tok) {
  return enum_handler::Axis(tok);
}

[[maybe_unused]] static nonstd::expected<Visibility, std::string> VisibilityEnumHandler(const std::string &tok) {
  return enum_handler::Visibility(tok);
}

[[maybe_unused]] static nonstd::expected<Purpose, std::string> PurposeEnumHandler(const std::string &tok) {
  return enum_handler::Purpose(tok);
}

[[maybe_unused]] static nonstd::expected<Orientation, std::string> OrientationEnumHandler(const std::string &tok) {
  return enum_handler::Orientation(tok);
}

static std::string AppendSemanticSourceDiagnostic(
    const std::string &msg,
    const std::string &prop_name,
    const PrimReconstructOptions &options) {
  std::string result = msg;
  bool has_property_source = false;

  if (options.format_property_path) {
    std::string path = options.format_property_path(prop_name);
    if (!path.empty()) {
      result += "\nProperty path: " + path;
    }
  }

  if (options.format_property_source_diagnostic) {
    std::string diag = options.format_property_source_diagnostic(prop_name);
    if (!diag.empty()) {
      result += "\n" + diag;
      has_property_source = true;
    }
  }

  if (!has_property_source && options.format_prim_source_diagnostic) {
    std::string diag = options.format_prim_source_diagnostic();
    if (!diag.empty()) {
      result += "\n" + diag;
    }
  }

  return result;
}

#if 1

template<typename T, typename EnumTy>
bool ParseUniformEnumProperty(
  const std::string &prop_name,
  bool strict_allowedToken_check,
  EnumHandlerFun<EnumTy> enum_handler,
  const Attribute &attr,
  TypedAttributeWithFallback<T> *result,
  std::string *warn = nullptr,
  std::string *err = nullptr,
  const PrimReconstructOptions &options = PrimReconstructOptions())
{

  if (!result) {
    PUSH_ERROR_AND_RETURN("[Internal error] `result` must not be null.");
  }

  if (attr.is_connection()) {
    result->set_connections(attr.connections());
    result->set_value_empty();
    return true;
  }


  if (attr.variability() == Variability::Uniform) {
    // scalar

    if (attr.is_blocked()) {
      result->set_blocked(true);
      return true;
    }

    if ((!attr.get_var().has_value() && attr.get_var().has_timesamples())) {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
              fmt::format(
                  "Attribute `{}` is declared `uniform`, but a time-sampled value was authored.",
                  prop_name),
              prop_name, options));
    }

    if (auto tok = attr.get_value<value::token>()) {
      auto e = enum_handler(tok.value().str());
      if (e) {
        (*result) = e.value();
        return true;
      } else if (strict_allowedToken_check) {
        PUSH_ERROR_AND_RETURN(
            AppendSemanticSourceDiagnostic(
                fmt::format("Attribute `{}`: `{}` is not an allowed token.", prop_name, tok.value().str()),
                prop_name, options));
      } else {
        PUSH_WARN_F("Attribute `{}`: `{}` is not an allowed token. Ignore it.", prop_name, tok.value().str());
        result->set_value_empty();
        return true;
      }
    } else {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
                  fmt::format(
                      "Internal error: attribute `{}` must have type `token`, but got `{}`.",
                            prop_name, attr.type_name()),
                  prop_name, options));
    }


  } else {
    // uniform or TimeSamples
    if (attr.get_var().has_value() && !attr.get_var().has_timesamples()) {

      if (attr.is_blocked()) {
        result->set_blocked(true);
        return true;
      }

      if (auto tok = attr.get_value<value::token>()) {
        auto e = enum_handler(tok.value().str());
        if (e) {
          (*result) = e.value();
          return true;
        } else if (strict_allowedToken_check) {
          PUSH_ERROR_AND_RETURN(
              AppendSemanticSourceDiagnostic(
                  fmt::format("Attribute `{}`: `{}` is not an allowed token.", prop_name, tok.value().str()),
                  prop_name, options));
        } else {
          PUSH_WARN_F("Attribute `{}`: `{}` is not an allowed token. Ignore it.", prop_name, tok.value().str());
          result->set_value_empty();
          return true;
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            AppendSemanticSourceDiagnostic(
                fmt::format(
                    "Internal error: attribute `{}` must have type `token`, but got `{}`.",
                            prop_name, attr.type_name()),
                prop_name, options));
      }
    } else if ((!attr.get_var().has_value() && attr.get_var().has_timesamples())) {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
              fmt::format(
                  "Attribute `{}` is declared `uniform`, but a time-sampled value was authored.",
                          prop_name),
              prop_name, options));

    } else {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
              fmt::format("Internal error: attribute `{}` is invalid.", prop_name),
              prop_name, options));
    }

  }

  return false;
}

// Animatable enum tokens
template<typename T, typename EnumTy>
bool ParseTimeSampledEnumProperty(
  const std::string &prop_name,
  bool strict_allowedToken_check,
  EnumHandlerFun<EnumTy> enum_handler,
  const Attribute &attr,
  TypedAttributeWithFallback<Animatable<T>> *result,
  std::string *warn = nullptr,
  std::string *err = nullptr,
  const PrimReconstructOptions &options = PrimReconstructOptions())
{

  if (!result) {
    PUSH_ERROR_AND_RETURN("[Internal error] `result` must not be null.");
  }

  if (attr.is_connection()) {
    result->set_connections(attr.connections());
    result->set_value_empty();
    return true;
  }


  if (attr.variability() == Variability::Uniform) {
    // scalar

    if (attr.is_blocked()) {
      result->set_blocked(true);
      return true;
    }

    if ((!attr.get_var().has_value() && attr.get_var().has_timesamples())) {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
              fmt::format(
                  "Attribute `{}` is declared `uniform`, but a time-sampled value was authored.",
                  prop_name),
              prop_name, options));
    }

    if (auto tok = attr.get_value<value::token>()) {
      auto e = enum_handler(tok.value().str());
      if (e) {
        (*result) = e.value();
        return true;
      } else if (strict_allowedToken_check) {
        PUSH_ERROR_AND_RETURN(
            AppendSemanticSourceDiagnostic(
                fmt::format("Attribute `{}`: `{}` is not an allowed token.", prop_name, tok.value().str()),
                prop_name, options));
      } else {
        PUSH_WARN_F("Attribute `{}`: `{}` is not an allowed token. Ignore it.", prop_name, tok.value().str());
        result->set_value_empty();
        return true;
      }
    } else {
      PUSH_ERROR_AND_RETURN(
          AppendSemanticSourceDiagnostic(
                fmt::format(
                    "Internal error: attribute `{}` must have type `token`, but got `{}`.",
                            prop_name, attr.type_name()),
                prop_name, options));
    }


  } else {
    // default and/or TimeSamples
    bool has_default{false};
    bool has_timesamples{false};

    Animatable<T> animatable_value;

    if (attr.is_blocked()) {
      result->set_blocked(true);
      has_default = true;
      //return true;
    }

    if (attr.get_var().has_default()) {
      DCOUT("has default.");

      if (auto tok = attr.get_value<value::token>()) {
        auto e = enum_handler(tok.value().str());
        if (e) {
          animatable_value.set_default(e.value());
          has_default = true;
          //return true;

        } else if (strict_allowedToken_check) {
          PUSH_ERROR_AND_RETURN(
              AppendSemanticSourceDiagnostic(
                  fmt::format("Attribute `{}`: `{}` is not an allowed token.", prop_name, tok.value().str()),
                  prop_name, options));
        } else {
          PUSH_WARN_F("Attribute `{}`: `{}` is not an allowed token. Ignore it.", prop_name, tok.value().str());
          //result->set_value_empty();
          //return true;
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            AppendSemanticSourceDiagnostic(
                fmt::format(
                    "Internal error: attribute `{}` must have type `token`, but got `{}`.",
                            prop_name, attr.type_name()),
                prop_name, options));
      }
    }

    if (attr.get_var().has_timesamples()) {
      DCOUT("has timesamples.");
      size_t n = attr.get_var().num_timesamples();

      for (size_t i = 0; i < n; i++) {

        double sample_time{value::TimeCode::Default()};

        if (auto pv = attr.get_var().ts_raw().get_time(i)) {
          sample_time = pv.value();
        } else {
          // This should not happen.
          PUSH_ERROR_AND_RETURN(
              AppendSemanticSourceDiagnostic(
                  fmt::format("Internal error: failed to get the time code for `{}`.", prop_name),
                  prop_name, options));
        }

        {
          const auto &samples = attr.get_var().ts_raw().get_samples();
          if (i < samples.size() && samples[i].blocked) {
            animatable_value.add_blocked_sample(sample_time);
            continue;
          }
        }

        if (auto pv = attr.get_var().ts_raw().get_value(i)) {
          if (auto tok = pv.value().get_value<value::token>()) {
            auto e = enum_handler(tok.value().str());
            if (e) {
              animatable_value.add_sample(sample_time, e.value());
            } else if (strict_allowedToken_check) {
              PUSH_ERROR_AND_RETURN(
                  AppendSemanticSourceDiagnostic(
                      fmt::format("Attribute `{}`: `{}` is not an allowed token.", prop_name, tok.value().str()),
                      prop_name, options));
            } else {
              PUSH_WARN_F("Attribute `{}`: `{}` at {}'th timesample is not an allowed token. Ignore it.", prop_name, i, tok.value().str());
              continue;
            }
          } else {
            PUSH_ERROR_AND_RETURN(
                AppendSemanticSourceDiagnostic(
                    fmt::format(
                        "Internal error: timesample {} for attribute `{}` must have type `token`, but got `{}`.",
                        i, prop_name, attr.type_name()),
                    prop_name, options));
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              AppendSemanticSourceDiagnostic(
                  fmt::format(
                      "Internal error: timesample {} for attribute `{}` must have type `token`, but got `{}`.",
                      i, prop_name, attr.type_name()),
                  prop_name, options));
        }
      }

      has_timesamples = true;
      //return true;

    }

    if (has_default || has_timesamples) {
      result->set_value(animatable_value);
    }

    return true;

  }

  return false;
}
#endif


// Unified enum property parsing macro
// __parser_fn should be ParseUniformEnumProperty or ParseTimeSampledEnumProperty
#define PARSE_ENUM_PROPERTY_IMPL(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                                 __target, __strict_check, __parser_fn) {        \
  if (__prop.first == __name) {                                                  \
    if (__table.count(__name)) { continue; }                                     \
    const Attribute &attr = __prop.second.get_attribute();                       \
    if ((__prop.second.value_type_name() == value::TypeTraits<value::token>::type_name()) && \
        __prop.second.is_attribute() && __prop.second.is_empty()) {              \
      DCOUT("No value assigned to `" << __name << "` token attribute. Set default token value."); \
      __target.metas() = std::move(__prop.second.attribute().metas());           \
      __table.insert(__name);                                                    \
      continue;                                                                  \
    }                                                                            \
    std::function<nonstd::expected<__enum_ty, std::string>(const std::string &)> fun = __enum_handler; \
    if (!__parser_fn(__name, __strict_check, fun, attr, &__target, warn, err)) { \
      return false;                                                              \
    }                                                                            \
    __target.metas() = std::move(__prop.second.attribute().metas());             \
    __table.insert(__name);                                                      \
    continue;                                                                    \
  }                                                                              \
}

// Convenience wrappers for backward compatibility
#define PARSE_UNIFORM_ENUM_PROPERTY(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                                    __target, __strict_check) \
  PARSE_ENUM_PROPERTY_IMPL(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                           __target, __strict_check, ParseUniformEnumProperty)

#define PARSE_TIMESAMPLED_ENUM_PROPERTY(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                                        __target, __strict_check) \
  PARSE_ENUM_PROPERTY_IMPL(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                           __target, __strict_check, ParseTimeSampledEnumProperty)

// NOCONTINUE version for use within if-else branches where we need to do additional work after parsing
#define PARSE_UNIFORM_ENUM_PROPERTY_NOCONTINUE(__table, __prop, __name, __enum_ty, __enum_handler, __klass, \
                                               __target, __strict_check) {                              \
  if (__prop.first == __name) {                                                                         \
    if (!__table.count(__name)) {                                                                       \
      const Attribute &attr = __prop.second.get_attribute();                                            \
      if ((__prop.second.value_type_name() == value::TypeTraits<value::token>::type_name()) &&          \
          __prop.second.is_attribute() && __prop.second.is_empty()) {                                   \
        DCOUT("No value assigned to `" << __name << "` token attribute. Set default token value."); \
        __target.metas() = std::move(__prop.second.attribute().metas());                                \
        __table.insert(__name);                                                                         \
      } else {                                                                                          \
        std::function<nonstd::expected<__enum_ty, std::string>(const std::string &)> fun = __enum_handler; \
        if (!ParseUniformEnumProperty(__name, __strict_check, fun, attr, &__target, warn, err)) { \
          return false;                                                                                 \
        }                                                                                               \
        __target.metas() = std::move(__prop.second.attribute().metas());                                \
        __table.insert(__name);                                                                         \
      }                                                                                                 \
    }                                                                                                   \
  }                                                                                                     \
}


// Add custom property(including property with "primvars" prefix)
// Please call this macro after listing up all predefined property with
// `PARSE_PROPERTY` and `PARSE_***_ENUM_PROPERTY`
#define ADD_PROPERTY(__table, __prop, __klass, __dst) {        \
  /* Check if the property name is a predefined property */  \
  if (!__table.count(__prop.first)) {                        \
    DCOUT("custom property added: name = " << __prop.first); \
    __dst[__prop.first] = std::move(__prop.second);          \
    __table.insert(__prop.first);                            \
  } \
 }

// This code path should not be reached though.
#define PARSE_PROPERTY_END_MAKE_ERROR(__table, __prop) {                     \
  if (!__table.count(__prop.first)) {                              \
    PUSH_ERROR_AND_RETURN("Unsupported/unimplemented property: " + \
                          __prop.first);                           \
  } \
 }

// This code path should not be reached though.
#define PARSE_PROPERTY_END_MAKE_WARN(__table, __prop) { \
  if (!__table.count(__prop.first)) { \
    PUSH_WARN("Unsupported/unimplemented property: " + __prop.first); \
   } \
 }

#ifdef __clang__
#pragma clang diagnostic pop
#endif
