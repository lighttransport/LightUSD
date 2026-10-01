// SPDX-License-Identifier: Apache 2.0
// Copyright 2020 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDC reader: Property parsing and PropertyMap building
// (extracted from usdc-reader.cc)
//

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "usdc-reader-impl.hh"
#include "ascii-parser.hh"  // AsciiParser (no longer pulled transitively via usdc-reader-impl.hh)

#if !defined(LIGHTUSD_DISABLE_MODULE_USDC_READER)

namespace lightusd {
namespace usdc {

bool USDCReader::Impl::BuildPropertyMap(const std::vector<size_t> &pathIndices,
                                        const PathIndexToSpecIndexMap &psmap,
                                        prim::PropertyMap *props) {

  for (size_t i = 0; i < pathIndices.size(); i++) {
    int child_index = int(pathIndices[i]);
    if ((child_index < 0) || (child_index >= int(_nodes->size()))) {
      PUSH_ERROR("Invalid child node id: " + std::to_string(child_index) +
                 ". Must be in range [0, " + std::to_string(_nodes->size()) +
                 ")");
      return false;
    }

    if (!psmap.count(uint32_t(child_index))) {
      // No specifier assigned to this child node.
      continue;
    }

    uint32_t spec_index = psmap.at(uint32_t(child_index));
    if (spec_index >= _specs->size()) {
      PUSH_ERROR("Invalid specifier id: " + std::to_string(spec_index) +
                 ". Must be in range [0, " + std::to_string(_specs->size()) +
                 ")");
      return false;
    }

    const crate::Spec &spec = (*_specs)[spec_index];

    // Property must be Attribute, Relationship, or Connection
    if ((spec.spec_type == SpecType::Attribute) ||
        (spec.spec_type == SpecType::Relationship) ||
        (spec.spec_type == SpecType::Connection)) {
      // OK
    } else {
      continue;
    }

    nonstd::optional<Path> path = GetPath(spec.path_index);

    if (!path) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid PathIndex.");
    }

    DCOUT("Path prim part: " << path.value().prim_part()
                             << ", prop part: " << path.value().prop_part()
                             << ", spec_index = " << spec_index);

    crate::FieldValuePairVector decoded_fvs;
    const crate::FieldValuePairVector *child_fvs = nullptr;
    uint64_t scratch_reserved = 0;
    if (!ResolveFieldValuePairs(spec, &child_fvs, &decoded_fvs,
                                &scratch_reserved)) {
      return false;
    }
    // decoded_fvs is per-iteration; release its budget reservation when this
    // iteration's property has been built.
    struct ScratchBudgetRelease {
      crate::CrateReader *cr;
      uint64_t bytes;
      ~ScratchBudgetRelease() {
        if (cr && bytes) {
          cr->ReleaseMemoryBudget(bytes);
        }
      }
    } _scratch_release{crate_reader, scratch_reserved};

    {
      std::string prop_name = path.value().prop_part();
      if (spec.spec_type == SpecType::Connection) {
        if (!endsWith(prop_name, ".connect")) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag,
              fmt::format("Invalid connection property name `{}`", prop_name));
        }
        prop_name = removeSuffix(prop_name, ".connect");
      }

      if (prop_name.empty()) {
        DCOUT("path = " << dump_path(path.value()));
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Property Prop.PropPart is empty");
      }

      std::string prop_err;
      if (!pathutil::ValidatePropPath(Path("", prop_name), &prop_err)) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("Invalid Property name `{}`: {}", prop_name, prop_err));
      }

      Property prop;
      // In lazy mode, child_fvs points to local decoded_fvs — safe to move from.
      if (!ParseProperty(spec.spec_type,
                         const_cast<crate::FieldValuePairVector &>(*child_fvs),
                         &prop, _config.use_lazy_property_construction)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag,
            fmt::format(
                "Failed to construct Property `{}` from FieldValuePairVector.",
                prop_name));
      }

      (*props)[prop_name] = std::move(prop);
      DCOUT("Add property : " << prop_name);

      // Record mmap ref with full path key
      if (_has_pending_mmap_ref) {
        std::string prim_path = path.value().prim_part();
        _mmap_table.add(prim_path, prop_name, _pending_mmap_ref);
        _has_pending_mmap_ref = false;
      }
    }
  }

  return true;
}


/// Property fieldSet example
///
///   specTyppe = SpecTypeAttribute
///
///     - typeName(token) : type name of Attribute(e.g. `float`)
///     - custom(bool) : `custom` qualifier
///     - variability(variability) : Variability(meta?)
///     <value>
///       - default : Default(fallback) value.
///       - timeSample(TimeSamples) : `.timeSamples` data.
///       - connectionPaths(type = ListOpPath) : `.connect`
///       - (Empty) : Define only(Neiher connection nor value assigned. e.g.
///       "float outputs:rgb")
bool USDCReader::Impl::ParseProperty(const SpecType spec_type,
                                     crate::FieldValuePairVector &fvs,
                                     Property *prop,
                                     bool allow_move_from_fvs) {
  if (fvs.size() > _config.kMaxFieldValuePairs) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too much FieldValue pairs.");
  }

  if (!prop) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Internal error. prop is nullptr.");
  }

  bool custom{false};
  nonstd::optional<value::token> typeName;
  nonstd::optional<Interpolation> interpolation;
  nonstd::optional<int> elementSize;
  nonstd::optional<bool> hidden;
  nonstd::optional<CustomDataType> customData;
  nonstd::optional<double> weight;
  nonstd::optional<value::token> bindMaterialAs;
  nonstd::optional<value::token> connectability;
  nonstd::optional<value::token> renderType;
  nonstd::optional<value::token> outputName;
  nonstd::optional<CustomDataType> sdrMetadata;
  nonstd::optional<value::StringData> comment;
  nonstd::optional<Variability> variability;
  AttrMeta meta; // for other not frequently-used attribute/relationship metadata.
  Attribute attr;

  nonstd::optional<value::Value> defaultValue;
  Relationship rel;

  // for attribute
  bool isValueBlock{false};
  bool hasDefault{false};
  bool hasTimeSamples{false};
  bool hasSpline{false};
  bool hasConnectionPaths{false};

  // for relationship
  bool hasTargetPaths{false};

  DCOUT("== List of Fields");

  primvar::PrimVar var;

  // first detect typeName
  for (auto &fv : fvs) {
    if (fv.first == "typeName") {
      if (auto pv = fv.second.get_value<value::token>()) {
        DCOUT("  typeName = " << pv.value().str());
        typeName = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`typeName` field is not `token` type.");
      }
    }
  }

  if (typeName) { // this should be always true though.
    attr.set_type_name(typeName.value().str());
  }

  for (auto &fv : fvs) {
    DCOUT(" fv name " << fv.first << "(type = " << fv.second.type_name()
                      << ")");

    // Debug: Check timeSamples field specifically
    if (fv.first.find("time") != std::string::npos) {
      DCOUT(">>> DEBUG: Found field with 'time' in name: '" << fv.first << "', length = " << fv.first.size());
    }

    if (fv.first == "custom") {
      if (auto pv = fv.second.get_value<bool>()) {
        custom = pv.value();
        DCOUT("  custom = " << pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "`custom` field is not `bool` type.");
      }
    } else if (fv.first == "variability") {
      if (auto pv = fv.second.get_value<Variability>()) {
        variability = pv.value();
        DCOUT("  variability = " << to_string(variability.value()));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`variability` field is not `varibility` type.");
      }
    } else if (fv.first == "typeName") {
      // 'typeName' is already processed. nothing to do here.
      continue;
    } else if (fv.first == "default") {

      // Check for mmap ref before move
      if (_config.mmap_zero_copy && fv.second.has_mmap_ref()) {
        _pending_mmap_ref = fv.second.mmap_ref();
        _has_pending_mmap_ref = true;
      }

      // In lazy mode (allow_move_from_fvs=true), fvs is a local scratch
      // buffer so we can move large arrays directly. Otherwise copy
      // because shared fieldsets may be referenced by other specs.
      if (allow_move_from_fvs) {
        defaultValue = std::move(fv.second.get_raw());
      } else {
        const value::Value &raw = fv.second.get_raw();
        defaultValue = raw;
      }
      hasDefault = true;

      // Handle UnregisteredValue in crate-reader.cc
      if (const auto pv = defaultValue.value().get_value<std::string>()) {
        if (typeName && (typeName.value().str() != "string")) {
          if (IsUnregisteredValueType(typeName.value().str())) {
            DCOUT("UnregisteredValue type: " << typeName.value().str());

            std::string local_err;
            value::Value v;
            if (!ascii::ParseUnregistredValue(typeName.value().str(), pv.value(), &v, &local_err)) {
              PUSH_ERROR_AND_RETURN(fmt::format("Failed to parse UnregisteredValue string with type `{}`: {}", typeName.value().str(), local_err));
            }

            defaultValue = std::move(v);
          }
        }
      }

    } else if (fv.first == "timeSamples") {
      DCOUT(">>> Entering timeSamples block");

      hasTimeSamples = true;

      if (const value::TimeSamples *vptr = fv.second.as<value::TimeSamples>()) {
        // DANGER:
        // TODO: remove const from func arg
        value::TimeSamples &ts = *(const_cast<value::TimeSamples *>(vptr));

        DCOUT("ts.type_id " << ts.type_id());

        // If TimeSamples is uninitialized (all samples were VALUE_BLOCK),
        // initialize it with the type from the attribute's typeName
        if (ts.type_id() == 0 && typeName) {
          uint32_t type_id = value::GetTypeId(typeName.value().str());

          if (type_id == value::TYPE_ID_INVALID) {
            PUSH_ERROR_AND_RETURN(fmt::format("Invalid typeName `{}` for TimeSamples", typeName.value().str()));
          }

          ts.set_type_id(type_id);
        }

        DCOUT("set_timesamples");

        // In lazy mode, fvs is a local scratch buffer (decoded_fvs) — safe to move.
        // In non-lazy mode, fvs points into shared _live_fieldsets — must copy.
        value::TimeSamples ts_final;
        if (allow_move_from_fvs) {
          ts_final = std::move(ts);
        } else {
          ts_final = ts;  // deep copy
        }

        // Apply role type casting if typeName specifies a role type
        if (typeName) {
          uint32_t role_type_id = value::GetTypeId(typeName.value().str());
          if (role_type_id != value::TYPE_ID_INVALID) {
            if (ts_final.cast_to_role_type(role_type_id)) {
              DCOUT(fmt::format("Cast TimeSamples to role type {}", typeName.value().str()));
            }
            // It's ok if casting fails - the base type is still valid
          }
        }

        // A `timeSamples` FIELD is present, so timeSamples were authored --
        // even if the block holds no samples (`float x.timeSamples = {}`). Mark
        // it, or PrimVar::has_timesamples() (size() > 0) reports false and the
        // empty block is dropped again on the way out.
        ts_final.set_authored();

        var.set_timesamples(std::move(ts_final));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`timeSamples` is not TimeSamples data.");
      }
    } else if (fv.first == "spline") {
      // Crate type-59 spline, decoded to a SplineData carrier by the value
      // reader (see crate-reader-values.cc).
      if (const primvar::PrimVar::SplineData *sp =
              fv.second.as<primvar::PrimVar::SplineData>()) {
        var.set_spline(*sp);
        hasSpline = true;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "`spline` field is not SplineData.");
      }
    } else if (fv.first == "interpolation") {

      if (auto pv = fv.second.get_value<value::token>()) {
        DCOUT("  interpolation = " << pv.value().str());

        if (auto interp = InterpolationFromString(pv.value().str())) {
          interpolation = interp.value();
        } else {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid token for `interpolation`.");
        }
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`interpolation` field is not `token` type.");
      }
    } else if (fv.first == "connectionPaths") {
      // Attribute connection(.connect)
      hasConnectionPaths = true;

      if (auto pv = fv.second.get_value<ListOp<Path>>()) {
        auto p = pv.value();
        DCOUT("connectionPaths = " << to_string(p));

        if (!p.IsExplicit()) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag, "`connectionPaths` must be composed of Explicit items.");
        }

        // Must be explicit_items for now.
        auto items = p.GetExplicitItems();
        if (items.size() == 0) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag, "`connectionPaths` have empty Explicit items.");
        }

        attr.set_connections(items);

      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`connectionPaths` field is not `ListOp[Path]` type.");
      }
    } else if (fv.first == "targetPaths") {
      // `rel`
      hasTargetPaths = true;

      // Check for ValueBlock first (rel ... = None)
      if (fv.second.get_value<value::ValueBlock>()) {
        // Relationship or connection is blocked (None)
        if (spec_type == SpecType::Connection) {
          hasDefault = true;
        } else {
          rel.set_blocked();
          DCOUT("targetPaths = None (blocked)");
        }
      } else if (auto pv = fv.second.get_value<ListOp<Path>>()) {
        const ListOp<Path> &p = pv.value();
        DCOUT("targetPaths = " << to_string(p));

        auto ps = DecodeListOp<Path>(p);

        if (ps.empty()) {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "`targetPaths` is empty.");
        }

        if (ps.size() > 1) {
          PUSH_WARN(
              "ListOp with multiple ListOpType is not supported for now. Use "
              "the first one: " +
              to_string(std::get<0>(ps[0])));
        }

        auto qual = std::get<0>(ps[0]);
        auto items = std::get<1>(ps[0]);

        if (items.empty() && (spec_type != SpecType::Connection)) {
          if (qual == ListEditQual::ResetToExplicit) {
            // An explicit empty target list clears weaker targets. Keep the
            // opinion; a declaration-only relationship has no targetPaths.
            rel.set(std::vector<Path>{});
          } else {
            // An authored but EMPTY bucket: a list-edit qualifier on a
            // relationship with no targets (`append rel myval`). rel.set({}) would
            // make it a PathVector of zero targets and print `rel myval = []`;
            // it is declaration-only, and the qualifier below is all it carries.
            rel.set_novalue();
          }
        } else if (items.size() == 1 && (spec_type != SpecType::Connection)) {
          // Single (relationship)
          const Path path = items[0];
          rel.set(path);
        } else if (spec_type == SpecType::Connection) {
          attr.set_connections(items);
        } else {
          rel.set(items);  // [Path]
        }

        if (spec_type != SpecType::Connection) {
          rel.set_listedit_qual(qual);
        }

      } else if (auto path_values = fv.second.get_value<std::vector<Path>>()) {
        auto &items = path_values.value();
        if (spec_type == SpecType::Connection) {
          attr.set_connections(items);
        } else {
          rel.set(items);  // [Path]
        }
      } else {
        if (spec_type == SpecType::Connection) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag, "`targetPaths` field is not `PathVector`, `ListOp[Path]`, "
                    "or `None`.");
        } else {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag, "`targetPaths` field is not `ListOp[Path]` or ValueBlock type.");
        }
      }

    } else if (fv.first == "hidden") {
      // Attribute hidden param
      if (auto pv = fv.second.get_value<bool>()) {
        auto p = pv.value();
        DCOUT("hidden = " << to_string(p));
        hidden = p;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`elementSize` field is not `int` type.");
      }
    } else if (fv.first == "elementSize") {
      // Attribute Meta
      if (auto pv = fv.second.get_value<int>()) {
        auto p = pv.value();
        DCOUT("elementSize = " << to_string(p));

        if ((p < 1) || (uint32_t(p) > _config.kMaxElementSize)) {
          // Reject out-of-range `elementSize`: it feeds downstream stride
          // computations, so an oversized value must not be propagated.
          PUSH_ERROR_AND_RETURN_TAG(
              kTag,
              fmt::format("`elementSize` is out of range. Must be within [{}, "
                          "{}], but got {}",
                          1, _config.kMaxElementSize, p));
        }

        elementSize = p;

      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`elementSize` field is not `int` type.");
      }
    } else if (fv.first == "weight") {
      // pxrUSD uses float type.
      if (auto pv = fv.second.get_value<float>()) {
        auto p = pv.value();
        DCOUT("weight = " << p);
        weight = double(p);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`weight` field is not `float` type.");
      }
    } else if (fv.first == "bindMaterialAs") {
      // Attribute Meta
      if (auto pv = fv.second.get_value<value::token>()) {
        auto p = pv.value();
        DCOUT("bindMaterialAs = " << to_string(p));

        if ((p.str() == kWeakerThanDescendants) || (p.str() == kStrongerThanDescendants)) {
          // ok
        } else {
          PUSH_WARN("Unsupported bindMaterialAs token: " << p.str());
        }
        bindMaterialAs = p;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`bindMaterialAs` field is not `token` type.");
      }
    } else if (fv.first == "targetChildren") {
      if (auto pv = fv.second.get_value<std::vector<Path>>()) {
        DCOUT("targetChildren = " << pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`targetChildren` field is not `PathVector` type.");
      }
    } else if (fv.first == "connectionChildren") {
      if (auto pv = fv.second.get_value<std::vector<Path>>()) {
        DCOUT("connectionChildren = " << pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`connectionChildren` field is not `PathVector` type.");
      }
    } else if (fv.first == "connectability") {
      if (auto pv = fv.second.get_value<value::token>()) {
        connectability = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`connectability` must be type `token`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "outputName") {
      if (auto pv = fv.second.get_value<value::token>()) {
        outputName = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`outputName` must be type `token`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "renderType") {
      if (auto pv = fv.second.get_value<value::token>()) {
        renderType = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`renderType` must be type `token`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "sdrMetadata") {
      if (auto pv = fv.second.get_value<CustomDataType>()) {
        sdrMetadata = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`sdrMetadata` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "customData") {
      // CustomData(dict)
      if (auto pv = fv.second.get_value<CustomDataType>()) {
        customData = pv.value();
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`customData` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "comment") {
      if (auto pv = fv.second.get_value<std::string>()) {
        value::StringData s;
        s.value = pv.value();
        s.is_triple_quoted = hasNewline(s.value);
        comment = s;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`comment` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }

    } else if (fv.first == "colorSpace") {
      if (auto pv = fv.second.get_value<value::token>()) {
        meta.set_colorSpace(pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`colorSpace` must be type `token`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "displayName") {
      if (auto pv = fv.second.get_value<std::string>()) {
        meta.set_displayName(pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`displayName` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "displayGroup") {
      if (auto pv = fv.second.get_value<std::string>()) {
        meta.set_displayGroup(pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`displayGroup` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "documentation" || fv.first == "doc") {
      if (auto pv = fv.second.get_value<std::string>()) {
        meta.set_doc(pv.value());
      } else if (auto pv2 = fv.second.get_value<value::StringData>()) {
        meta.set_doc(pv2.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`documentation` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "allowedTokens") {
      if (auto pv = fv.second.get_value<std::vector<value::token>>()) {
        meta.set_allowedTokens(pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`allowedTokens` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "unauthoredValuesIndex") {
      if (auto pv = fv.second.get_value<int>()) {
        meta.set_unauthoredValuesIndex(pv.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`unauthoredValuesIndex` must be type `int`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.second.IsUnregisteredValue()) {
      // Unregistered property metadata (pxr SdfUnregisteredValue): the crate
      // stores the raw USDA text of the value (quotes included for string
      // values). Keep it verbatim so the printer can write it back unchanged.
      if (const std::string *uv = fv.second.GetUnregisteredValueString()) {
        meta.unregisteredMetas[fv.first] = *uv;
      }
      DCOUT("Preserved unregistered property metadata: " << fv.first);
    } else {
      MetaVariable unknown_meta;
      if (allow_move_from_fvs) {
        unknown_meta.set_value(fv.first, std::move(fv.second.get_raw()));
      } else {
        unknown_meta.set_value(fv.first, fv.second.get_raw());
      }
      meta.data()[fv.first] = std::move(unknown_meta);
      PUSH_WARN("Preserved unknown property metadata: " << fv.first);
      DCOUT("Preserved unknown property metadata: " << fv.first);
    }
  }
  DCOUT("== End List of Fields");

  // Post check

  // Do role type cast for default value.
  if (defaultValue.has_value()) {
    if (typeName) {
      if (defaultValue.value().type_id() == value::TypeTraits<value::ValueBlock>::type_id()) {
        // nothing to do
      } else {
        std::string reqTy = typeName.value().str();
        std::string scalarTy = defaultValue.value().type_name();

        if (reqTy.compare(scalarTy) != 0) {

          bool ret = value::UpcastType(reqTy, defaultValue.value());
          if (ret) {
            DCOUT(fmt::format("Upcast type from {} to {}.", scalarTy, reqTy));
          }

          scalarTy = defaultValue.value().type_name();
          if (value::RoleTypeCast(value::GetTypeId(reqTy), defaultValue.value())) {
            DCOUT(fmt::format("Casted to Role type {} from type {}.", reqTy, scalarTy));
          } else {
            // Its ok.
          }
        }
      }
    }
    // Check type before moving
    bool is_value_block = (defaultValue.value().type_id() == value::TypeTraits<value::ValueBlock>::type_id());
    var.set_value(std::move(defaultValue.value()));

    if (is_value_block) {
      isValueBlock = true;
    }
  }

  // HACK
  attr.set_var(std::move(var));

  if (isValueBlock) {
    if (typeName) {
      attr.set_type_name(typeName.value().str());
    }
  }

  // Attribute metas
  {
    if (interpolation) {
      meta.set_interpolation_enum(interpolation.value());
    }
    if (elementSize) {
      meta.set_elementSize(static_cast<uint32_t>(elementSize.value()));
    }
    if (hidden) {
      meta.set_hidden(hidden.value());
    }
    if (customData) {
      meta.set_customData(customData.value());
    }
    if (weight) {
      meta.set_weight(weight.value());
    }
    if (comment) {
      meta.set_comment(comment.value());
    }
    if (bindMaterialAs) {
      meta.set_bindMaterialAs(bindMaterialAs.value());
    }
    if (outputName) {
      meta.set_outputName(outputName.value());
    }
    if (sdrMetadata) {
      meta.set_sdrMetadata(sdrMetadata.value());
    }
    if (connectability) {
      meta.set_connectability(connectability.value());
    }
    if (renderType) {
      meta.set_renderType(renderType.value());
    }
  }



  if (hasTargetPaths && (spec_type != SpecType::Connection)) {
    // Relationship spec

    if (hasDefault) {
      PUSH_WARN("Relationship property has `default` field. Ignore `default` field.");
    }

    if (hasTimeSamples) {
      PUSH_WARN("Relationship property has `timeSamples` field. Ignore `timeSamples` field.");
    }

    if (hasConnectionPaths) {
      PUSH_WARN("Relationship property has `connectionPaths` field. Ignore `connectionPaths` field.");
    }

    if (variability) {
      if (variability.value() == Variability::Varying) {
        rel.set_varying_authored();
      }
    }
    rel.metas() = std::move(meta);
    (*prop) = Property(std::move(rel), custom);
  } else if (hasDefault || hasTimeSamples || hasConnectionPaths ||
             hasSpline ||
             ((spec_type == SpecType::Connection) && hasTargetPaths)) {

    // Attribute
    if (hasTargetPaths && (spec_type != SpecType::Connection)) {
      PUSH_WARN("Attribute property has `targetPaths` field. Ignore `targetPaths` field.");
    }

    if (variability) {
      attr.variability() = variability.value();
    }
    attr.metas() = std::move(meta);
    (*prop) = Property(std::move(attr), custom);

  } else {

    if (typeName) {
      std::string baseTypeName = typeName.value().str();
      if (endsWith(baseTypeName, "[]")) {
        baseTypeName = removeSuffix(baseTypeName, "[]");
      }

      // Assume Attribute
      if (!_supported_prim_attr_types.count(baseTypeName)) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("Invalid or unsupported `typeName` {}", typeName.value()));
      }

      Property p;
      p.set_property_type(Property::Type::EmptyAttrib);
      p.attribute().set_type_name(typeName.value().str());
      p.set_custom(custom);

      if (variability) {
        p.attribute().variability() = variability.value();
      }
      p.attribute().metas() = std::move(meta);

      (*prop) = std::move(p);

    } else {
      DCOUT("spec_type = " << to_string(spec_type));
      if (spec_type == SpecType::Relationship) {
        rel = Relationship();
        rel.set_novalue();
        if (variability == Variability::Varying) {
          rel.set_varying_authored();
        }
        rel.metas() = std::move(meta);
        (*prop) = Property(std::move(rel), custom);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "`typeName` field is missing.");
      }
    }
  }

  return true;
}

}  // namespace usdc
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDC_READER
