// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage-converter: Geometry prim property extraction.
//

#include "sconv-detail.hh"
#include "usdGeom.hh"
#include "usdSkel.hh"  // For BlendShape

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {

namespace {

// Emit `<name>.timeSamples` for an Animatable<T> whose samples are stored in the
// SAME representation on disk as in memory (arrays of points/floats/quats, ...),
// i.e. everything except the enums and Extent, which need a shape conversion and
// are handled at their own sites.
//
// Several typed writers emitted only has_default() and had NO timeSamples branch
// at all, so animation on those attributes was dropped wholesale on write. This
// is the missing half.
template <typename T>
void EmitAnimatableTimeSamples(const std::string &name,
                               const Animatable<T> &anim,
                               crate::FieldValuePairVector &fields) {
  if (!anim.has_timesamples()) {
    return;
  }

  const value::TimeSamples *src = anim.get_timesamples_ptr();
  if (!src) {
    return;
  }

  value::TimeSamples ts;
  const auto &samples = src->get_samples();
  for (size_t i = 0; i < samples.size(); i++) {
    if (samples[i].blocked) {
      ts.add_blocked_sample(samples[i].t, value::Value());
    } else {
      ts.add_sample(samples[i].t, samples[i].value);
    }
  }

  crate::CrateValue ts_crate_val;
  ts_crate_val.Set(ts);
  fields.push_back({name + ".timeSamples", ts_crate_val});
}

}  // namespace

namespace experimental {

// ============================================================================
// Xform Property Extraction
// ============================================================================

bool CrateWriter::ExtractXformProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const Xform* xform = prim.data().as<Xform>();
  if (!xform) {
    if (err) *err = "Failed to cast prim to Xform";
    return false;
  }

  // Extract xformOps from the Xformable base class
  // (Xform inherits from Xformable, so this will handle xformOps and xformOpOrder)
  if (!ExtractXformOpsFromXformable(prim, prim_path, fields, err)) {
    return false;
  }

  // Xform also inherits from GPrim — extract visibility/purpose/extent.
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// Mesh Property Extraction
// ============================================================================

bool CrateWriter::ExtractMeshProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomMesh* mesh = prim.data().as<GeomMesh>();
  if (!mesh) {
    if (err) *err = "Failed to cast prim to GeomMesh";
    return false;
  }

  // `uniform token subsetFamily:<FAMILYNAME>:familyType`. This lives on the
  // MESH, not on the GeomSubset (see usdGeom.hh), so it is not a member of any
  // schema struct -- it is a map keyed by family name, and the writer had no
  // branch for it at all, silently dropping every family's type on write. The
  // reader reconstructs it by pattern-matching the property name
  // (prim-reconstruct-geom2.cc), so emitting the same spelling is enough.
  for (const auto& item : mesh->subsetFamilyTypeMap) {
    crate::CrateValue crate_val;
    crate_val.Set(value::token(to_string(item.second)));
    fields.push_back(
        {"subsetFamily:" + item.first.str() + ":familyType", crate_val});
  }

  // Extract points
  if (mesh->points.has_value()) {
    auto points_animatable = mesh->points.get_value();
    if (points_animatable && points_animatable->has_default()) {
      std::vector<value::point3f> points_val;
      if (points_animatable->get_default(&points_val)) {
        crate::CrateValue crate_val;
        // Convert point3f[] to float3[]
        value::Value points_value(points_val);
        if (ConvertValue(points_value, crate_val, err)) {
          fields.push_back({"points", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (points_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(points_value.type_name()));
            fields.push_back({std::string("points") + ".typeName", ty_val});
          }
        }
      }
    }

    // Handle time samples for animated points
    if (points_animatable && points_animatable->has_timesamples()) {
      // Value-type Animatable stores a type-erased value::TimeSamples directly.
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = points_animatable->get_timesamples_ptr()) {
        ts = *_tsp;
      }

      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      fields.push_back({"points.timeSamples", ts_crate_val});

    }
    // Preserve AttrMeta on `points`. The stage-converter's prop_entries
    // router recognizes ".interpolation"/".timeSamples" and any key in
    // kAttrMetaSuffixes (elementSize, customData, displayName, ...).
    const auto& pmetas = mesh->points.metas();
    if (pmetas.has_interpolation()) {
      crate::CrateValue v; v.Set(pmetas.get_interpolation());
      fields.push_back({"points.interpolation", v});
    }
    if (pmetas.has_elementSize()) {
      crate::CrateValue v;
      v.Set(static_cast<int32_t>(pmetas.get_elementSize()));
      fields.push_back({"points.elementSize", v});
    }
    if (pmetas.has_customData()) {
      crate::CrateValue v; v.Set(pmetas.get_customData());
      fields.push_back({"points.customData", v});
    }
    if (pmetas.has_displayName()) {
      crate::CrateValue v; v.Set(pmetas.get_displayName());
      fields.push_back({"points.displayName", v});
    }
    if (pmetas.has_displayGroup()) {
      crate::CrateValue v; v.Set(pmetas.get_displayGroup());
      fields.push_back({"points.displayGroup", v});
    }
    if (pmetas.has_doc()) {
      crate::CrateValue v; v.Set(pmetas.get_doc().value);
      fields.push_back({"points.documentation", v});
    }
    if (pmetas.has_hidden()) {
      crate::CrateValue v; v.Set(pmetas.get_hidden());
      fields.push_back({"points.hidden", v});
    }
  }
  /* If `points` lives only in the props map (typed field unauthored),
   * leave it for the generic props-map iteration in stage-converter so
   * AttrMeta (displayName, doc, interpolation, ...) is preserved. */

  // Extract normals
  if (mesh->normals.has_value()) {
    auto normals_animatable = mesh->normals.get_value();
    if (normals_animatable && normals_animatable->has_default()) {
      std::vector<value::normal3f> normals_val;
      if (normals_animatable->get_default(&normals_val)) {
        crate::CrateValue crate_val;
        value::Value normals_value(normals_val);
        if (ConvertValue(normals_value, crate_val, err)) {
          fields.push_back({"normals", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (normals_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(normals_value.type_name()));
            fields.push_back({std::string("normals") + ".typeName", ty_val});
          }
        }
      }
    }
    // Time samples (mirrors the points.timeSamples emit above).
    if (normals_animatable && normals_animatable->has_timesamples()) {
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = normals_animatable->get_timesamples_ptr()) {
        ts = *_tsp;
      }
      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      fields.push_back({"normals.timeSamples", ts_crate_val});
    }
    // Preserve AttrMeta on `normals` (mirrors `points` AttrMeta emit).
    const auto& nmetas = mesh->normals.metas();
    if (nmetas.has_interpolation()) {
      crate::CrateValue v; v.Set(nmetas.get_interpolation());
      fields.push_back({"normals.interpolation", v});
    }
    if (nmetas.has_elementSize()) {
      crate::CrateValue v;
      v.Set(static_cast<int32_t>(nmetas.get_elementSize()));
      fields.push_back({"normals.elementSize", v});
    }
    if (nmetas.has_customData()) {
      crate::CrateValue v; v.Set(nmetas.get_customData());
      fields.push_back({"normals.customData", v});
    }
    if (nmetas.has_displayName()) {
      crate::CrateValue v; v.Set(nmetas.get_displayName());
      fields.push_back({"normals.displayName", v});
    }
    if (nmetas.has_displayGroup()) {
      crate::CrateValue v; v.Set(nmetas.get_displayGroup());
      fields.push_back({"normals.displayGroup", v});
    }
    if (nmetas.has_doc()) {
      crate::CrateValue v; v.Set(nmetas.get_doc().value);
      fields.push_back({"normals.documentation", v});
    }
    if (nmetas.has_hidden()) {
      crate::CrateValue v; v.Set(nmetas.get_hidden());
      fields.push_back({"normals.hidden", v});
    }
  }

  // Extract velocities (vector3f[])
  if (mesh->velocities.authored()) {
    auto vel_opt = mesh->velocities.get_value();
    if (vel_opt.has_value() && vel_opt.value().has_default()) {
      std::vector<value::vector3f> vel_val;
      if (vel_opt.value().get_default(&vel_val)) {
        crate::CrateValue crate_val;
        value::Value val(vel_val);
        if (ConvertValue(val, crate_val, err)) {
          fields.push_back({"velocities", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (val.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(val.type_name()));
            fields.push_back({std::string("velocities") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract faceVertexCounts
  if (mesh->faceVertexCounts.has_value()) {
    auto counts_animatable = mesh->faceVertexCounts.get_value();
    if (counts_animatable && counts_animatable->has_default()) {
      std::vector<int32_t> counts_val;
      if (counts_animatable->get_default(&counts_val)) {
        crate::CrateValue crate_val;
        value::Value counts_value(counts_val);
        if (ConvertValue(counts_value, crate_val, err)) {
          fields.push_back({"faceVertexCounts", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (counts_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(counts_value.type_name()));
            fields.push_back({std::string("faceVertexCounts") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract faceVertexIndices
  if (mesh->faceVertexIndices.has_value()) {
    auto indices_animatable = mesh->faceVertexIndices.get_value();
    if (indices_animatable && indices_animatable->has_default()) {
      std::vector<int32_t> indices_val;
      if (indices_animatable->get_default(&indices_val)) {
        crate::CrateValue crate_val;
        value::Value indices_value(indices_val);
        if (ConvertValue(indices_value, crate_val, err)) {
          fields.push_back({"faceVertexIndices", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (indices_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(indices_value.type_name()));
            fields.push_back({std::string("faceVertexIndices") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract subdivision surface corner properties
  // cornerIndices - indices of corner vertices
  if (mesh->cornerIndices.has_value()) {
    auto corner_indices_animatable = mesh->cornerIndices.get_value();
    if (corner_indices_animatable && corner_indices_animatable->has_default()) {
      std::vector<int32_t> corner_indices_val;
      if (corner_indices_animatable->get_default(&corner_indices_val)) {
        crate::CrateValue crate_val;
        value::Value corner_indices_value(corner_indices_val);
        if (ConvertValue(corner_indices_value, crate_val, err)) {
          fields.push_back({"cornerIndices", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (corner_indices_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(corner_indices_value.type_name()));
            fields.push_back({std::string("cornerIndices") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // cornerSharpnesses - sharpness values for corners
  if (mesh->cornerSharpnesses.has_value()) {
    auto corner_sharp_animatable = mesh->cornerSharpnesses.get_value();
    if (corner_sharp_animatable && corner_sharp_animatable->has_default()) {
      std::vector<float> corner_sharp_val;
      if (corner_sharp_animatable->get_default(&corner_sharp_val)) {
        crate::CrateValue crate_val;
        value::Value corner_sharp_value(corner_sharp_val);
        if (ConvertValue(corner_sharp_value, crate_val, err)) {
          fields.push_back({"cornerSharpnesses", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (corner_sharp_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(corner_sharp_value.type_name()));
            fields.push_back({std::string("cornerSharpnesses") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract crease properties
  // creaseIndices - indices of crease edge endpoints
  if (mesh->creaseIndices.has_value()) {
    auto crease_indices_animatable = mesh->creaseIndices.get_value();
    if (crease_indices_animatable && crease_indices_animatable->has_default()) {
      std::vector<int32_t> crease_indices_val;
      if (crease_indices_animatable->get_default(&crease_indices_val)) {
        crate::CrateValue crate_val;
        value::Value crease_indices_value(crease_indices_val);
        if (ConvertValue(crease_indices_value, crate_val, err)) {
          fields.push_back({"creaseIndices", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (crease_indices_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(crease_indices_value.type_name()));
            fields.push_back({std::string("creaseIndices") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // creaseLengths - number of edges in each crease chain
  if (mesh->creaseLengths.has_value()) {
    auto crease_lengths_animatable = mesh->creaseLengths.get_value();
    if (crease_lengths_animatable && crease_lengths_animatable->has_default()) {
      std::vector<int32_t> crease_lengths_val;
      if (crease_lengths_animatable->get_default(&crease_lengths_val)) {
        crate::CrateValue crate_val;
        value::Value crease_lengths_value(crease_lengths_val);
        if (ConvertValue(crease_lengths_value, crate_val, err)) {
          fields.push_back({"creaseLengths", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (crease_lengths_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(crease_lengths_value.type_name()));
            fields.push_back({std::string("creaseLengths") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // creaseSharpnesses - sharpness values for creases
  if (mesh->creaseSharpnesses.has_value()) {
    auto crease_sharp_animatable = mesh->creaseSharpnesses.get_value();
    if (crease_sharp_animatable && crease_sharp_animatable->has_default()) {
      std::vector<float> crease_sharp_val;
      if (crease_sharp_animatable->get_default(&crease_sharp_val)) {
        crate::CrateValue crate_val;
        value::Value crease_sharp_value(crease_sharp_val);
        if (ConvertValue(crease_sharp_value, crate_val, err)) {
          fields.push_back({"creaseSharpnesses", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (crease_sharp_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(crease_sharp_value.type_name()));
            fields.push_back({std::string("creaseSharpnesses") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract hole indices
  if (mesh->holeIndices.has_value()) {
    auto hole_indices_animatable = mesh->holeIndices.get_value();
    if (hole_indices_animatable && hole_indices_animatable->has_default()) {
      std::vector<int32_t> hole_indices_val;
      if (hole_indices_animatable->get_default(&hole_indices_val)) {
        crate::CrateValue crate_val;
        value::Value hole_indices_value(hole_indices_val);
        if (ConvertValue(hole_indices_value, crate_val, err)) {
          fields.push_back({"holeIndices", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (hole_indices_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(hole_indices_value.type_name()));
            fields.push_back({std::string("holeIndices") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract subdivision control attributes
  // interpolateBoundary - TypedAttributeWithFallback<Animatable<InterpolateBoundary>>
  if (mesh->interpolateBoundary.authored()) {
    auto interp_boundary_anim = mesh->interpolateBoundary.get_value();
    if (interp_boundary_anim.has_default()) {
      GeomMesh::InterpolateBoundary interp_val;
      if (interp_boundary_anim.get_default(&interp_val)) {
        crate::CrateValue crate_val;
        value::token tok(to_string(interp_val));
        crate_val.Set(tok);
        fields.push_back({"interpolateBoundary", crate_val});
      }
    }
  }

  // subdivisionScheme - TypedAttributeWithFallback<SubdivisionScheme> (no Animatable!)
  if (mesh->subdivisionScheme.authored()) {
    const auto& subdiv_scheme = mesh->subdivisionScheme.get_value();
    crate::CrateValue crate_val;
    value::token tok(to_string(subdiv_scheme));
    crate_val.Set(tok);
    fields.push_back({"subdivisionScheme", crate_val});
  }

  // triangleSubdivisionRule - TypedAttributeWithFallback<TriangleSubdivisionRule> (no Animatable!)
  if (mesh->triangleSubdivisionRule.authored()) {
    const auto& tri_rule = mesh->triangleSubdivisionRule.get_value();
    crate::CrateValue crate_val;
    value::token tok(to_string(tri_rule));
    crate_val.Set(tok);
    fields.push_back({"triangleSubdivisionRule", crate_val});
  }

  // faceVaryingLinearInterpolation - TypedAttributeWithFallback<Animatable<FaceVaryingLinearInterpolation>>
  if (mesh->faceVaryingLinearInterpolation.authored()) {
    auto fv_interp_anim = mesh->faceVaryingLinearInterpolation.get_value();
    if (fv_interp_anim.has_default()) {
      GeomMesh::FaceVaryingLinearInterpolation fv_interp_val;
      if (fv_interp_anim.get_default(&fv_interp_val)) {
        crate::CrateValue crate_val;
        value::token tok(to_string(fv_interp_val));
        crate_val.Set(tok);
        fields.push_back({"faceVaryingLinearInterpolation", crate_val});
      }
    }
  }

  // Extract blend shapes (token array)
  if (mesh->blendShapes.has_value()) {
    auto blend_shapes_val = mesh->blendShapes.get_value();
    if (blend_shapes_val) {
      crate::CrateValue crate_val;
      value::Value val(*blend_shapes_val);
      if (ConvertValue(val, crate_val, err)) {
        // On a Mesh this is the UsdSkelBindingAPI attribute, so it is NAMESPACED:
        // `uniform token[] skel:blendShapes`. (SkelAnimation's own `blendShapes`
        // is genuinely unprefixed -- both spellings are correct, on different
        // prim types. See AddSkelAnimationAttrs in sconv-skel.cc.)
        fields.push_back({"skel:blendShapes", crate_val});
      }
    }
  }

  // Extract skeleton relationship (skel:skeleton). The reader stores
  // this on `mesh->skeleton` via GEOM_MESH_RELATIONS; without an explicit
  // re-emit, the relationship is dropped on USDC round-trip.
  if (mesh->skeleton) {
    if (!ConvertRelationshipToFields("skel:skeleton",
                                     mesh->skeleton.value(), prim_path,
                                     err)) {
      return false;
    }
  }

  // Extract blend shape targets relationship (skel:blendShapeTargets)
  if (mesh->blendShapeTargets) {
    if (!ConvertRelationshipToFields("skel:blendShapeTargets",
                                     mesh->blendShapeTargets.value(),
                                     prim_path, err)) {
      return false;
    }
  }

  // Extract common GPrim properties
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// Cube Property Extraction
// ============================================================================

bool CrateWriter::ExtractCubeProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCube* cube = prim.data().as<GeomCube>();
  if (!cube) {
    if (err) *err = "Failed to cast prim to GeomCube";
    return false;
  }

  if (!EmitTypedAnimatableAttr("size", cube->size, fields, err)) {
    return false;
  }

  // Extract extent
  if (cube->extent.has_value()) {
    auto extent_animatable = cube->extent.get_value();
    if (extent_animatable && extent_animatable->has_default()) {
      Extent extent_val;
      if (extent_animatable->get_default(&extent_val)) {
        // Extent is a pair of float3
        crate::CrateValue crate_val;
        // Create array of 2 float3 values
        std::vector<value::float3> extent_array = {extent_val.lower, extent_val.upper};
        value::Value extent_value(extent_array);
        if (ConvertValue(extent_value, crate_val, err)) {
          fields.push_back({"extent", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (extent_value.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(extent_value.type_name()));
            fields.push_back({std::string("extent") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract common GPrim properties (including xformOps)
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// Sphere Property Extraction
// ============================================================================

bool CrateWriter::ExtractSphereProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomSphere* sphere = prim.data().as<GeomSphere>();
  if (!sphere) {
    if (err) *err = "Failed to cast prim to GeomSphere";
    return false;
  }

  if (!EmitTypedAnimatableAttr("radius", sphere->radius, fields,
                               err)) {
    return false;
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// UsdVol Property Extraction
// ============================================================================

// Emit one FieldAsset TypedAttribute<Animatable<T>> (get_value() yields an
// optional<Animatable<T>>; unwrap before extracting).
#define EXTRACT_FIELD_TYPED_ATTR(attr, attr_name)                              \
  if ((attr).has_value()) {                                                    \
    auto _anim = (attr).get_value();                                           \
    if (_anim) {                                                               \
      if (!ExtractAnimatableDefault(*_anim, attr_name, fields, err))           \
        return false;                                                          \
    }                                                                          \
  }

// Emit the common FieldAsset attributes (filePath / fieldName / fieldDataType /
// fieldIndex). `field` may be an OpenVDBAsset/Field3DAsset bound as the base.
#define EXTRACT_FIELD_ASSET_ATTRS(field)                  \
  EXTRACT_FIELD_TYPED_ATTR((field).filePath, "filePath")  \
  EXTRACT_FIELD_TYPED_ATTR((field).fieldName, "fieldName") \
  EXTRACT_FIELD_TYPED_ATTR((field).fieldDataType, "fieldDataType") \
  EXTRACT_FIELD_TYPED_ATTR((field).fieldIndex, "fieldIndex")

bool CrateWriter::ExtractFieldAssetProperties(
  const Prim& prim, const Path& prim_path,
  crate::FieldValuePairVector& fields, std::string* err
) {
  const FieldAsset* field = prim.data().as<FieldAsset>();
  if (!field) { if (err) *err = "Failed to cast prim to FieldAsset"; return false; }
  EXTRACT_FIELD_ASSET_ATTRS(*field)
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractOpenVDBAssetProperties(
  const Prim& prim, const Path& prim_path,
  crate::FieldValuePairVector& fields, std::string* err
) {
  const OpenVDBAsset* asset = prim.data().as<OpenVDBAsset>();
  if (!asset) { if (err) *err = "Failed to cast prim to OpenVDBAsset"; return false; }
  EXTRACT_FIELD_ASSET_ATTRS(*asset)
  EXTRACT_FIELD_TYPED_ATTR(asset->fieldClass, "fieldClass")
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractField3DAssetProperties(
  const Prim& prim, const Path& prim_path,
  crate::FieldValuePairVector& fields, std::string* err
) {
  const Field3DAsset* asset = prim.data().as<Field3DAsset>();
  if (!asset) { if (err) *err = "Failed to cast prim to Field3DAsset"; return false; }
  EXTRACT_FIELD_ASSET_ATTRS(*asset)
  EXTRACT_FIELD_TYPED_ATTR(asset->fieldPurpose, "fieldPurpose")
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

#undef EXTRACT_FIELD_ASSET_ATTRS
#undef EXTRACT_FIELD_TYPED_ATTR

bool CrateWriter::ExtractVolumeProperties(
  const Prim& prim, const Path& prim_path,
  crate::FieldValuePairVector& fields, std::string* err
) {
  const Volume* volume = prim.data().as<Volume>();
  if (!volume) { if (err) *err = "Failed to cast prim to Volume"; return false; }

  // `field:<name>` relationships -> field-asset prim paths.
  for (const auto& item : volume->fieldRelationships) {
    const std::string rel_name = "field:" + item.first;
    if (!ConvertRelationshipToFields(rel_name, item.second, prim_path, err)) {
      return false;
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// Cylinder Property Extraction
// ============================================================================

bool CrateWriter::ExtractCylinderProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCylinder* cylinder = prim.data().as<GeomCylinder>();
  if (!cylinder) {
    if (err) *err = "Failed to cast prim to GeomCylinder";
    return false;
  }

  if (!EmitTypedAnimatableAttr("radius", cylinder->radius, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("height", cylinder->height, fields, err)) {
    return false;
  }

  // Extract axis (mirrors Cone/Capsule). Was missing — caused the
  // axis token to drop on USDC roundtrip.
  if (cylinder->axis.authored()) {
    const Axis& axis_val = cylinder->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractConeProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCone* cone = prim.data().as<GeomCone>();
  if (!cone) {
    if (err) *err = "Failed to cast prim to GeomCone";
    return false;
  }

  if (!EmitTypedAnimatableAttr("radius", cone->radius, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("height", cone->height, fields, err)) {
    return false;
  }

  // Extract axis
  if (cone->axis.authored()) {
    const Axis& axis_val = cone->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractCapsuleProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCapsule* capsule = prim.data().as<GeomCapsule>();
  if (!capsule) {
    if (err) *err = "Failed to cast prim to GeomCapsule";
    return false;
  }

  if (!EmitTypedAnimatableAttr("radius", capsule->radius, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("height", capsule->height, fields, err)) {
    return false;
  }

  // Extract axis
  if (capsule->axis.authored()) {
    const Axis& axis_val = capsule->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractPointsProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomPoints* points = prim.data().as<GeomPoints>();
  if (!points) {
    if (err) *err = "Failed to cast prim to GeomPoints";
    return false;
  }


  // Extract points (point3f[]) - TypedAttribute returns optional
  if (points->points.authored()) {
    auto points_opt = points->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract widths (float[]) - TypedAttribute returns optional
  if (points->widths.authored()) {
    auto widths_opt = points->widths.get_value();
    if (widths_opt.has_value()) {
      const Animatable<std::vector<float>>& widths_anim = widths_opt.value();
      if (widths_anim.has_default()) {
        std::vector<float> widths_val;
        if (widths_anim.get_default(&widths_val)) {
          value::Value widths_value(widths_val);
          if (!AddArrayAttribute("widths", widths_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract ids (int64[])
  if (points->ids.authored()) {
    auto ids_opt = points->ids.get_value();
    if (ids_opt.has_value() && ids_opt.value().has_default()) {
      std::vector<int64_t> ids_val;
      if (ids_opt.value().get_default(&ids_val)) {
        crate::CrateValue crate_val;
        value::Value val(ids_val);
        if (ConvertValue(val, crate_val, err)) {
          fields.push_back({"ids", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (val.type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(val.type_name()));
            fields.push_back({std::string("ids") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  // Extract normals (normal3f[]) - TypedAttribute returns optional
  if (points->normals.authored()) {
    auto normals_opt = points->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[]) - TypedAttribute returns optional
  if (points->velocities.authored()) {
    auto velocities_opt = points->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
      EmitAnimatableTimeSamples("velocities", velocities_anim, fields);
    }
  }

  // Extract accelerations (vector3f[]) - TypedAttribute returns optional
  if (points->accelerations.authored()) {
    auto accelerations_opt = points->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
      EmitAnimatableTimeSamples("accelerations", accelerations_anim, fields);
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractCameraProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCamera* camera = prim.data().as<GeomCamera>();
  if (!camera) {
    if (err) *err = "Failed to cast prim to GeomCamera";
    return false;
  }

  // Camera's float/double attributes all have NON-ZERO schema fallbacks
  // (focalLength 50, clippingRange (0.1, 1e6), ...), so they must go through
  // EmitTypedAnimatableAttr, which declines to invent one for an attribute that
  // was declared without a value. These two lambdas used to hand-roll the
  // default + timeSamples extraction that ExtractAnimatableDefault already does.
  auto add_float_attr =
      [&](const std::string &name,
          const TypedAttributeWithFallback<Animatable<float>> &attr) -> bool {
    return EmitTypedAnimatableAttr(name.c_str(), attr, fields, err);
  };

  auto add_double_attr =
      [&](const std::string &name,
          const TypedAttributeWithFallback<Animatable<double>> &attr) -> bool {
    return EmitTypedAnimatableAttr(name.c_str(), attr, fields, err);
  };

  // Extract float2 clippingRange
  if (!EmitTypedAnimatableAttr("clippingRange", camera->clippingRange, fields,
                               err)) {
    return false;
  }

  // Extract exposure (float)
  add_float_attr("exposure", camera->exposure);

  // Extract focalLength (float, default 50.0mm)
  add_float_attr("focalLength", camera->focalLength);

  // Extract focusDistance (float)
  add_float_attr("focusDistance", camera->focusDistance);

  // Extract horizontalAperture (float)
  add_float_attr("horizontalAperture", camera->horizontalAperture);

  // Extract horizontalApertureOffset (float)
  add_float_attr("horizontalApertureOffset", camera->horizontalApertureOffset);

  // Extract verticalAperture (float)
  add_float_attr("verticalAperture", camera->verticalAperture);

  // Extract verticalApertureOffset (float)
  add_float_attr("verticalApertureOffset", camera->verticalApertureOffset);

  // Extract fStop (float, 0.0 = no focusing)
  add_float_attr("fStop", camera->fStop);

  // Extract projection (token enum)
  if (camera->projection.authored()) {
    const Animatable<GeomCamera::Projection>& proj_anim = camera->projection.get_value();

    auto proj_to_token = [](GeomCamera::Projection p) {
      return value::token(p == GeomCamera::Projection::Perspective ? "perspective"
                                                                  : "orthographic");
    };

    GeomCamera::Projection proj_val;
    if (proj_anim.has_default() && proj_anim.get_default(&proj_val)) {
      crate::CrateValue crate_val;
      crate_val.Set(proj_to_token(proj_val));
      fields.push_back({"projection", crate_val});
    }

    // ANIMATED projection. There was no timeSamples branch, so `token
    // projection.timeSamples` was dropped wholesale. An enum's samples are stored
    // as int64 in memory (same as visibility, see ExtractImageableAttrs) and must
    // go out as tokens.
    if (proj_anim.has_timesamples()) {
      const value::TimeSamples *proj_ts = proj_anim.get_timesamples_ptr();

      value::TimeSamples ts;
      const auto &samples = proj_ts->get_samples();
      for (size_t i = 0; i < samples.size(); i++) {
        if (samples[i].blocked) {
          ts.add_blocked_sample(samples[i].t, value::Value());
        } else if (const int64_t *iv = samples[i].value.as<int64_t>()) {
          ts.add_sample(samples[i].t, value::Value(proj_to_token(
                                          static_cast<GeomCamera::Projection>(*iv))));
        } else {
          ts.add_sample(samples[i].t, samples[i].value);
        }
      }

      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      fields.push_back({"projection.timeSamples", ts_crate_val});
    }
  }

  // Extract stereoRole (uniform token enum)
  if (camera->stereoRole.authored()) {
    std::string stereo_str;
    const GeomCamera::StereoRole& stereo = camera->stereoRole.get_value();
    if (stereo == GeomCamera::StereoRole::Left) {
      stereo_str = "left";
    } else if (stereo == GeomCamera::StereoRole::Right) {
      stereo_str = "right";
    } else {
      stereo_str = "mono";
    }
    crate::CrateValue crate_val;
    value::token tok(stereo_str);
    crate_val.Set(tok);
    fields.push_back({"stereoRole", crate_val});
  }

  // Extract shutter:open (double). Namespaced in the schema (usdGeom.hh,
  // prim-property-tables.hh) unlike this function's other Camera attributes,
  // which are all plain names -- writing it as "shutterOpen" put it under a
  // property name the reader never looks for, so it round-tripped as if
  // never authored.
  add_double_attr("shutter:open", camera->shutterOpen);

  // Extract shutter:close (double), same namespacing as shutter:open above.
  add_double_attr("shutter:close", camera->shutterClose);

  // Extract clippingPlanes (float4[]) if present - TypedAttribute (no fallback)
  if (camera->clippingPlanes.authored()) {
    auto planes_opt = camera->clippingPlanes.get_value();
    if (planes_opt.has_value()) {
      const Animatable<std::vector<value::float4>>& planes_anim = planes_opt.value();
      std::vector<value::float4> planes_val;
      if (planes_anim.get_default(&planes_val)) {
        crate::CrateValue crate_val;
        if (ConvertValue(value::Value(planes_val), crate_val, err)) {
          fields.push_back({"clippingPlanes", crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (value::Value(planes_val).type_name() != crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(value::Value(planes_val).type_name()));
            fields.push_back({std::string("clippingPlanes") + ".typeName", ty_val});
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractBasisCurvesProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomBasisCurves* basis_curves = prim.data().as<GeomBasisCurves>();
  if (!basis_curves) {
    if (err) *err = "Failed to cast prim to GeomBasisCurves";
    return false;
  }
  // Extract type enum (Cubic/Linear) only if authored on the typed field;
  // otherwise let any props-map value flow through.
  if (basis_curves->type.authored()) {
    const GeomBasisCurves::Type& type_val = basis_curves->type.get_value();
    std::string type_str = (type_val == GeomBasisCurves::Type::Cubic) ? "cubic" : "linear";
    AddEnumAttribute("type", type_str, fields);
  }

  if (basis_curves->basis.authored()) {
    const GeomBasisCurves::Basis& basis_val = basis_curves->basis.get_value();
    std::string basis_str;
    if (basis_val == GeomBasisCurves::Basis::Bezier) {
      basis_str = "bezier";
    } else if (basis_val == GeomBasisCurves::Basis::Bspline) {
      basis_str = "bspline";
    } else {  // CatmullRom
      basis_str = "catmullRom";
    }
    AddEnumAttribute("basis", basis_str, fields);
  }

  if (basis_curves->wrap.authored()) {
    const GeomBasisCurves::Wrap& wrap_val = basis_curves->wrap.get_value();
    std::string wrap_str;
    if (wrap_val == GeomBasisCurves::Wrap::Nonperiodic) {
      wrap_str = "nonperiodic";
    } else if (wrap_val == GeomBasisCurves::Wrap::Periodic) {
      wrap_str = "periodic";
    } else {  // Pinned
      wrap_str = "pinned";
    }
    AddEnumAttribute("wrap", wrap_str, fields);
  }

  // Extract points (point3f[]) - TypedAttribute returns optional
  if (basis_curves->points.authored()) {
    auto points_opt = basis_curves->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
      if (points_anim.has_timesamples()) {
        if (const value::TimeSamples *ts = points_anim.get_timesamples_ptr()) {
          value::TimeSamples wire_ts;
          std::string ts_err;
          for (const auto &sample : ts->get_samples()) {
            if (sample.blocked) {
              if (!wire_ts.add_blocked_sample<std::vector<value::float3>>(
                      sample.t, &ts_err)) {
                if (err) *err = ts_err;
                return false;
              }
              continue;
            }
            auto points_sample =
                sample.value.get_value<std::vector<value::point3f>>(false);
            if (!points_sample) {
              if (err) *err = "Failed to read BasisCurves point time sample";
              return false;
            }
            std::vector<value::float3> wire_points;
            wire_points.reserve(points_sample->size());
            for (const auto &point : *points_sample) {
              value::float3 wire_point;
              wire_point[0] = point[0];
              wire_point[1] = point[1];
              wire_point[2] = point[2];
              wire_points.push_back(wire_point);
            }
            if (!wire_ts.add_sample(sample.t, value::Value(std::move(wire_points)),
                                    &ts_err)) {
              if (err) *err = ts_err;
              return false;
            }
          }
          crate::CrateValue ts_value;
          ts_value.Set(wire_ts);
          fields.push_back({"points.timeSamples", ts_value});
          crate::CrateValue type_value;
          type_value.Set(value::token("point3f[]"));
          fields.push_back({"points.typeName", type_value});
        }
      }
    }
  }

  // Extract curveVertexCounts (int[]) - TypedAttribute returns optional
  if (basis_curves->curveVertexCounts.authored()) {
    auto counts_opt = basis_curves->curveVertexCounts.get_value();
    if (counts_opt.has_value()) {
      const Animatable<std::vector<int>>& counts_anim = counts_opt.value();
      if (counts_anim.has_default()) {
        std::vector<int> counts_val;
        if (counts_anim.get_default(&counts_val)) {
          value::Value counts_value(counts_val);
          if (!AddArrayAttribute("curveVertexCounts", counts_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract widths (float[]) - TypedAttribute returns optional.
  // Route through ConvertAttributeToFields so authored interpolation and
  // other AttrMetas survive USDC roundtrip.
  if (basis_curves->widths.authored()) {
    auto widths_opt = basis_curves->widths.get_value();
    if (widths_opt.has_value()) {
      const Animatable<std::vector<float>>& widths_anim = widths_opt.value();
      if (widths_anim.has_default()) {
        std::vector<float> widths_val;
        if (widths_anim.get_default(&widths_val)) {
          value::Value widths_value(widths_val);
          if (!AddArrayAttribute("widths", widths_value, fields, err)) {
            return false;
          }
        }
      }
      if (widths_anim.has_timesamples()) {
        if (const value::TimeSamples *ts = widths_anim.get_timesamples_ptr()) {
          crate::CrateValue ts_value;
          ts_value.Set(*ts);
          fields.push_back({"widths.timeSamples", ts_value});
        }
      }

      // Preserve the metadata needed by groom importers.  The time-sample
      // field must be assembled on the same attribute spec as the default;
      // emitting the default through AddArrayAttributeWithMetas would create
      // that spec immediately and leave the animation in a duplicate field.
      const auto &width_metas = basis_curves->widths.metas();
      if (width_metas.has_interpolation()) {
        crate::CrateValue v;
        v.Set(width_metas.get_interpolation());
        fields.push_back({"widths.interpolation", v});
      }
    }
  }

  // Extract normals (normal3f[]) - TypedAttribute returns optional
  if (basis_curves->normals.authored()) {
    auto normals_opt = basis_curves->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[]) - TypedAttribute returns optional
  if (basis_curves->velocities.authored()) {
    auto velocities_opt = basis_curves->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[]) - TypedAttribute returns optional
  if (basis_curves->accelerations.authored()) {
    auto accelerations_opt = basis_curves->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractNurbsCurvesProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomNurbsCurves* nurbs_curves = prim.data().as<GeomNurbsCurves>();
  if (!nurbs_curves) {
    if (err) *err = "Failed to cast prim to GeomNurbsCurves";
    return false;
  }


  // Extract points (point3f[]) - TypedAttribute returns optional
  if (nurbs_curves->points.authored()) {
    auto points_opt = nurbs_curves->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract order (int[]) - NURBS curve order per curve
  if (nurbs_curves->order.authored()) {
    auto order_opt = nurbs_curves->order.get_value();
    if (order_opt.has_value()) {
      const Animatable<std::vector<int>>& order_anim = order_opt.value();
      if (order_anim.has_default()) {
        std::vector<int> order_val;
        if (order_anim.get_default(&order_val)) {
          value::Value order_value(order_val);
          if (!AddArrayAttribute("order", order_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract knots (double[]) - NURBS knot vector
  if (nurbs_curves->knots.authored()) {
    auto knots_opt = nurbs_curves->knots.get_value();
    if (knots_opt.has_value()) {
      const Animatable<std::vector<double>>& knots_anim = knots_opt.value();
      if (knots_anim.has_default()) {
        std::vector<double> knots_val;
        if (knots_anim.get_default(&knots_val)) {
          value::Value knots_value(knots_val);
          if (!AddArrayAttribute("knots", knots_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract ranges (double2[]) - NURBS curve parameter ranges
  if (nurbs_curves->ranges.authored()) {
    auto ranges_opt = nurbs_curves->ranges.get_value();
    if (ranges_opt.has_value()) {
      const Animatable<std::vector<value::double2>>& ranges_anim = ranges_opt.value();
      if (ranges_anim.has_default()) {
        std::vector<value::double2> ranges_val;
        if (ranges_anim.get_default(&ranges_val) && !ranges_val.empty()) {
          value::Value ranges_value(ranges_val);
          if (!AddArrayAttribute("ranges", ranges_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract pointWeights (double[]) - Weights for curve control points
  if (nurbs_curves->pointWeights.authored()) {
    auto weights_opt = nurbs_curves->pointWeights.get_value();
    if (weights_opt.has_value()) {
      const Animatable<std::vector<double>>& weights_anim = weights_opt.value();
      if (weights_anim.has_default()) {
        std::vector<double> weights_val;
        if (weights_anim.get_default(&weights_val)) {
          value::Value weights_value(weights_val);
          if (!AddArrayAttribute("pointWeights", weights_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract curveVertexCounts (int[]) - Vertices per curve
  if (nurbs_curves->curveVertexCounts.authored()) {
    auto counts_opt = nurbs_curves->curveVertexCounts.get_value();
    if (counts_opt.has_value()) {
      const Animatable<std::vector<int>>& counts_anim = counts_opt.value();
      if (counts_anim.has_default()) {
        std::vector<int> counts_val;
        if (counts_anim.get_default(&counts_val)) {
          value::Value counts_value(counts_val);
          if (!AddArrayAttribute("curveVertexCounts", counts_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract widths (float[]) - TypedAttribute returns optional
  if (nurbs_curves->widths.authored()) {
    auto widths_opt = nurbs_curves->widths.get_value();
    if (widths_opt.has_value()) {
      const Animatable<std::vector<float>>& widths_anim = widths_opt.value();
      if (widths_anim.has_default()) {
        std::vector<float> widths_val;
        if (widths_anim.get_default(&widths_val)) {
          value::Value widths_value(widths_val);
          if (!AddArrayAttribute("widths", widths_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract normals (normal3f[]) - TypedAttribute returns optional
  if (nurbs_curves->normals.authored()) {
    auto normals_opt = nurbs_curves->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[]) - TypedAttribute returns optional
  if (nurbs_curves->velocities.authored()) {
    auto velocities_opt = nurbs_curves->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[]) - TypedAttribute returns optional
  if (nurbs_curves->accelerations.authored()) {
    auto accelerations_opt = nurbs_curves->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractPointInstancerProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomPointInstancer* instancer = prim.data().as<GeomPointInstancer>();
  if (!instancer) {
    if (err) *err = "Failed to cast prim to GeomPointInstancer";
    return false;
  }


  // Extract protoIndices (int32[]) - Indices into prototypes array
  if (instancer->protoIndices.authored()) {
    auto indices_opt = instancer->protoIndices.get_value();
    if (indices_opt.has_value()) {
      const Animatable<std::vector<int32_t>>& indices_anim = indices_opt.value();
      if (indices_anim.has_default()) {
        std::vector<int32_t> indices_val;
        if (indices_anim.get_default(&indices_val)) {
          value::Value indices_value(indices_val);
          if (!AddArrayAttribute("protoIndices", indices_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract positions (point3f[]) - Instance positions
  if (instancer->positions.authored()) {
    auto positions_opt = instancer->positions.get_value();
    if (positions_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& positions_anim = positions_opt.value();
      if (positions_anim.has_default()) {
        std::vector<value::point3f> positions_val;
        if (positions_anim.get_default(&positions_val)) {
          value::Value positions_value(positions_val);
          if (!AddArrayAttribute("positions", positions_value, fields, err)) {
            return false;
          }
        }
      }
      EmitAnimatableTimeSamples("positions", positions_anim, fields);
    }
  }

  // Extract scales (float3[]) - Instance scales
  if (instancer->scales.authored()) {
    auto scales_opt = instancer->scales.get_value();
    if (scales_opt.has_value()) {
      const Animatable<std::vector<value::float3>>& scales_anim = scales_opt.value();
      if (scales_anim.has_default()) {
        std::vector<value::float3> scales_val;
        if (scales_anim.get_default(&scales_val)) {
          value::Value scales_value(scales_val);
          if (!AddArrayAttribute("scales", scales_value, fields, err)) {
            return false;
          }
        }
      }
      EmitAnimatableTimeSamples("scales", scales_anim, fields);
    }
  }

  // Extract velocities (vector3f[]) - Instance velocities
  if (instancer->velocities.authored()) {
    auto velocities_opt = instancer->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[]) - Instance accelerations
  if (instancer->accelerations.authored()) {
    auto accelerations_opt = instancer->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract angularVelocities (vector3f[]) - Instance angular velocities
  if (instancer->angularVelocities.authored()) {
    auto ang_vel_opt = instancer->angularVelocities.get_value();
    if (ang_vel_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& ang_vel_anim = ang_vel_opt.value();
      if (ang_vel_anim.has_default()) {
        std::vector<value::vector3f> ang_vel_val;
        if (ang_vel_anim.get_default(&ang_vel_val)) {
          value::Value ang_vel_value(ang_vel_val);
          if (!AddArrayAttribute("angularVelocities", ang_vel_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract ids (int64[]) - Instance identifiers
  if (instancer->ids.authored()) {
    auto ids_opt = instancer->ids.get_value();
    if (ids_opt.has_value()) {
      const Animatable<std::vector<int64_t>>& ids_anim = ids_opt.value();
      if (ids_anim.has_default()) {
        std::vector<int64_t> ids_val;
        if (ids_anim.get_default(&ids_val)) {
          value::Value ids_value(ids_val);
          if (!AddArrayAttribute("ids", ids_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract orientations (quath[]) - Instance orientations as quaternions (half precision)
  if (instancer->orientations.authored()) {
    auto orientations_opt = instancer->orientations.get_value();
    if (orientations_opt.has_value()) {
      const Animatable<std::vector<value::quath>>& orientations_anim = orientations_opt.value();
      if (orientations_anim.has_default()) {
        std::vector<value::quath> orientations_val;
        if (orientations_anim.get_default(&orientations_val)) {
          value::Value orientations_value(orientations_val);
          if (!AddArrayAttribute("orientations", orientations_value, fields, err)) {
            return false;
          }
        }
      }
      EmitAnimatableTimeSamples("orientations", orientations_anim, fields);
    }
  }

  // Extract invisibleIds (int64[]) - IDs of instances to hide
  if (instancer->invisibleIds.authored()) {
    auto invisible_opt = instancer->invisibleIds.get_value();
    if (invisible_opt.has_value()) {
      const Animatable<std::vector<int64_t>>& invisible_anim = invisible_opt.value();
      if (invisible_anim.has_default()) {
        std::vector<int64_t> invisible_val;
        if (invisible_anim.get_default(&invisible_val)) {
          value::Value invisible_value(invisible_val);
          if (!AddArrayAttribute("invisibleIds", invisible_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract inactiveIds (int64[]) - IDs of instances to deactivate
  if (instancer->inactiveIds.authored()) {
    std::vector<int64_t> inactive_val;
    if (instancer->inactiveIds.get_value(&inactive_val)) {
      value::Value inactive_value(inactive_val);
      if (!AddArrayAttribute("inactiveIds", inactive_value, fields, err)) {
        return false;
      }
    }
  }

  // Note: prototypes (relationship) - handled separately in AddPointInstancerPrototypesSpec

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

bool CrateWriter::ExtractGeomSubsetProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomSubset* subset = prim.data().as<GeomSubset>();
  if (!subset) {
    if (err) *err = "Failed to cast prim to GeomSubset";
    return false;
  }



  // Extract elementType enum (Face/Point/Edge/Tetrahedron). Only when AUTHORED:
  // elementType is a TypedAttributeWithFallback, so get_value() hands back
  // ElementType::Face for an unauthored subset and writing it unconditionally
  // invented `uniform token elementType = "face"` on read-back.
  if (subset->elementType.authored()) {
    const GeomSubset::ElementType& elem_type = subset->elementType.get_value();
    std::string elem_str;
    switch (elem_type) {
      case GeomSubset::ElementType::Face: elem_str = "face"; break;
      case GeomSubset::ElementType::Point: elem_str = "point"; break;
      case GeomSubset::ElementType::Edge: elem_str = "edge"; break;
      case GeomSubset::ElementType::Tetrahedron: elem_str = "tetrahedron"; break;
    }
    AddEnumAttribute("elementType", elem_str, fields);
  }

  // Extract familyName token if authored
  if (subset->familyName.authored()) {
    auto familyname_opt = subset->familyName.get_value();
    if (familyname_opt.has_value()) {
      const value::token& familyname_val = familyname_opt.value();
      crate::CrateValue crate_val;
      crate_val.Set(familyname_val);
      fields.push_back({"familyName", crate_val});
    }
  }

  // Extract indices (int[]) - TypedAttribute returns optional
  if (subset->indices.authored()) {
    auto indices_opt = subset->indices.get_value();
    if (indices_opt.has_value()) {
      const Animatable<std::vector<int32_t>>& indices_anim = indices_opt.value();
      if (indices_anim.has_default()) {
        std::vector<int32_t> indices_val;
        if (indices_anim.get_default(&indices_val)) {
          value::Value indices_value(indices_val);
          if (!AddArrayAttribute("indices", indices_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return true;
}

// ============================================================================
// BlendShape Property Extraction
// ============================================================================

bool CrateWriter::ExtractBlendShapeProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const BlendShape* blend_shape = prim.data().as<BlendShape>();
  if (!blend_shape) {
    if (err) *err = "Failed to cast prim to BlendShape";
    return false;
  }


  // Extract offsets (vector3f[]) - Position deltas for each vertex
  if (blend_shape->offsets.has_value()) {
    auto offsets_val = blend_shape->offsets.get_value();
    if (offsets_val) {
      value::Value offsets_value(*offsets_val);
      if (!AddArrayAttribute("offsets", offsets_value, fields, err)) {
        return false;
      }
    }
  }

  // Extract normalOffsets (vector3f[]) - Normal deltas for each vertex
  if (blend_shape->normalOffsets.has_value()) {
    auto normal_offsets_val = blend_shape->normalOffsets.get_value();
    if (normal_offsets_val) {
      value::Value normal_offsets_value(*normal_offsets_val);
      if (!AddArrayAttribute("normalOffsets", normal_offsets_value, fields, err)) {
        return false;
      }
    }
  }

  // Extract pointIndices (int[]) - Subset of vertices affected (optional)
  if (blend_shape->pointIndices.has_value()) {
    auto point_indices_val = blend_shape->pointIndices.get_value();
    if (point_indices_val) {
      value::Value point_indices_value(*point_indices_val);
      if (!AddArrayAttribute("pointIndices", point_indices_value, fields, err)) {
        return false;
      }
    }
  }

  // In-between blend shapes are namespaced attributes stored in BlendShape::props.
  // ConvertSinglePrim emits them through the generic props-map path.

  return true;
}


// ============================================================================
// XformOp Extraction Helper
// ============================================================================

bool CrateWriter::ExtractXformOpsFromXformable(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  // GPrim and its subclasses aren't directly castable from value::Value
  // Instead, we need to check the specific type and access the Xformable base
  const Xformable* xformable = nullptr;

  // Try casting to specific geometry types that inherit from GPrim/Xformable
  if (auto* cube = prim.data().as<GeomCube>()) {
    xformable = static_cast<const Xformable*>(cube);
  } else if (auto* mesh = prim.data().as<GeomMesh>()) {
    xformable = static_cast<const Xformable*>(mesh);
  } else if (auto* sphere = prim.data().as<GeomSphere>()) {
    xformable = static_cast<const Xformable*>(sphere);
  } else if (auto* cylinder = prim.data().as<GeomCylinder>()) {
    xformable = static_cast<const Xformable*>(cylinder);
  } else if (auto* cone = prim.data().as<GeomCone>()) {
    xformable = static_cast<const Xformable*>(cone);
  } else if (auto* capsule = prim.data().as<GeomCapsule>()) {
    xformable = static_cast<const Xformable*>(capsule);
  } else if (auto* points = prim.data().as<GeomPoints>()) {
    xformable = static_cast<const Xformable*>(points);
  } else if (auto* camera = prim.data().as<GeomCamera>()) {
    xformable = static_cast<const Xformable*>(camera);
  } else if (auto* basis_curves = prim.data().as<GeomBasisCurves>()) {
    xformable = static_cast<const Xformable*>(basis_curves);
  } else if (auto* nurbs_curves = prim.data().as<GeomNurbsCurves>()) {
    xformable = static_cast<const Xformable*>(nurbs_curves);
  } else if (auto* plane = prim.data().as<GeomPlane>()) {
    xformable = static_cast<const Xformable*>(plane);
  } else if (auto* cylinder1 = prim.data().as<GeomCylinder_1>()) {
    xformable = static_cast<const Xformable*>(cylinder1);
  } else if (auto* capsule1 = prim.data().as<GeomCapsule_1>()) {
    xformable = static_cast<const Xformable*>(capsule1);
  } else if (auto* tetmesh = prim.data().as<GeomTetMesh>()) {
    xformable = static_cast<const Xformable*>(tetmesh);
  } else if (auto* nurbspatch = prim.data().as<GeomNurbsPatch>()) {
    xformable = static_cast<const Xformable*>(nurbspatch);
  } else if (auto* hermite = prim.data().as<GeomHermiteCurves>()) {
    xformable = static_cast<const Xformable*>(hermite);
  } else if (auto* instancer = prim.data().as<GeomPointInstancer>()) {
    xformable = static_cast<const Xformable*>(instancer);
  } else if (auto* xform = prim.data().as<Xform>()) {
    xformable = xform;
  } else if (auto* volume = prim.data().as<Volume>()) {
    xformable = static_cast<const Xformable*>(volume);
  } else if (auto* skelroot = prim.data().as<SkelRoot>()) {
    xformable = static_cast<const Xformable*>(skelroot);
  } else if (auto* skel = prim.data().as<Skeleton>()) {
    xformable = static_cast<const Xformable*>(skel);
  }
  // Lights are Xformable too (via Boundable/NonboundableLight). Leaving them out
  // of this list silently dropped every light's transform on write, so a scene
  // round-tripped through .usdc came back with all its lights at the origin.
  else if (auto* sphere_light = prim.data().as<SphereLight>()) {
    xformable = static_cast<const Xformable*>(sphere_light);
  } else if (auto* rect_light = prim.data().as<RectLight>()) {
    xformable = static_cast<const Xformable*>(rect_light);
  } else if (auto* disk_light = prim.data().as<DiskLight>()) {
    xformable = static_cast<const Xformable*>(disk_light);
  } else if (auto* cylinder_light = prim.data().as<CylinderLight>()) {
    xformable = static_cast<const Xformable*>(cylinder_light);
  } else if (auto* distant_light = prim.data().as<DistantLight>()) {
    xformable = static_cast<const Xformable*>(distant_light);
  } else if (auto* dome_light = prim.data().as<DomeLight>()) {
    xformable = static_cast<const Xformable*>(dome_light);
  } else if (auto* dome_light_1 = prim.data().as<DomeLight_1>()) {
    xformable = static_cast<const Xformable*>(dome_light_1);
  } else if (auto* geometry_light = prim.data().as<GeometryLight>()) {
    xformable = static_cast<const Xformable*>(geometry_light);
  } else if (auto* portal_light = prim.data().as<PortalLight>()) {
    xformable = static_cast<const Xformable*>(portal_light);
  } else {
    // Not a type we handle yet
    return true;
  }

  if (!xformable) {
    return true;
  }

  // Check if there are any xformOps
  if (xformable->xformOps.empty()) {
    return true;
  }


  // Extract each xformOp
  for (const auto& xformOp : xformable->xformOps) {
    // Build attribute name (e.g., "xformOp:translate")
    std::string op_name = "xformOp:";

    switch (xformOp.op_type) {
      case XformOp::OpType::Transform:
        op_name += "transform";
        break;
      case XformOp::OpType::Translate:
        op_name += "translate";
        break;
      case XformOp::OpType::Scale:
        op_name += "scale";
        break;
      case XformOp::OpType::RotateX:
        op_name += "rotateX";
        break;
      case XformOp::OpType::RotateY:
        op_name += "rotateY";
        break;
      case XformOp::OpType::RotateZ:
        op_name += "rotateZ";
        break;
      case XformOp::OpType::RotateXYZ:
        op_name += "rotateXYZ";
        break;
      case XformOp::OpType::RotateXZY:
        op_name += "rotateXZY";
        break;
      case XformOp::OpType::RotateYXZ:
        op_name += "rotateYXZ";
        break;
      case XformOp::OpType::RotateYZX:
        op_name += "rotateYZX";
        break;
      case XformOp::OpType::RotateZXY:
        op_name += "rotateZXY";
        break;
      case XformOp::OpType::RotateZYX:
        op_name += "rotateZYX";
        break;
      case XformOp::OpType::Orient:
        op_name += "orient";
        break;
      case XformOp::OpType::ResetXformStack:
        // Skip this one, it's handled in xformOpOrder
        continue;
    }

    // Add suffix if present (e.g., ":spin" for "xformOp:rotateZ:spin")
    if (!xformOp.suffix.empty()) {
      op_name += ":" + xformOp.suffix;
    }


    // Create a separate Attribute spec for this xformOp
    Path attr_path = prim_path.AppendProperty(op_name);
    crate::FieldValuePairVector attr_fields;

    // Add typeName based on the value type
    std::string type_name;
    if (xformOp.has_default()) {
      type_name = xformOp._var.value_raw().type_name();
    } else if (xformOp.has_timesamples()) {
      // Get type from first sample
      const value::TimeSamples& ts = xformOp._var.ts_raw();
      if (ts.size() > 0) {
        type_name = ts.get_samples()[0].value.type_name();
      } else {
        type_name = "float";  // fallback
      }
    } else {
      type_name = "float";  // fallback
    }

    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    attr_fields.push_back({"typeName", type_value});

    // A BLOCKED value (`float xformOp:rotateZ:spin = None`) must be written as a
    // ValueBlock. It has to be tested BEFORE has_default(), because has_default()
    // is has_value(), which deliberately reports true for a ValueBlock -- so the
    // block fell into the branch below, where ConvertValue turned it into the
    // type's zero and `= None` came back as `= 0`. ConvertAttributeToFields (the
    // generic path, stage-converter.cc) already ordered these two correctly.
    // Use XformOp::is_blocked(), not _var.is_blocked(): XformOp keeps its OWN
    // _is_blocked flag (the reader sets that one) and only its accessor ORs the
    // two together.
    if (xformOp.is_blocked()) {
      crate::CrateValue blocked_value;
      blocked_value.Set(value::ValueBlock());
      attr_fields.push_back({"default", blocked_value});
    } else if (xformOp.has_default()) {
      const value::Value& val = xformOp._var.value_raw();

      crate::CrateValue crate_val;
      if (ConvertValue(val, crate_val, err)) {
        attr_fields.push_back({"default", crate_val});
      } else {
        DCOUT("WARNING: Failed to convert xformOp value for " << op_name
                  << ": " << (err ? *err : "unknown"));
      }
    }

    // Add timeSamples if present (as a field on the same spec)
    if (xformOp.has_timesamples()) {
      const value::TimeSamples& ts = xformOp._var.ts_raw();

      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      attr_fields.push_back({"timeSamples", ts_crate_val});

      DCOUT("Successfully extracted animated xformOp: " << op_name
                << " with " << ts.size() << " samples");
    }

    // Create the Attribute spec
    if (!AddSpec(attr_path, SpecType::Attribute, std::move(attr_fields), err)) {
      DCOUT("WARNING: Failed to add spec for xformOp: " << op_name);
    }
  }

  // Extract xformOpOrder as a separate Attribute spec
  const std::vector<value::token>& xform_op_order = xformable->xformOpOrder();
  if (!xform_op_order.empty()) {
    Path order_path = prim_path.AppendProperty("xformOpOrder");
    crate::FieldValuePairVector order_fields;

    // Add typeName: token[]
    crate::CrateValue type_value;
    value::token type_tok("token[]");
    type_value.Set(type_tok);
    order_fields.push_back({"typeName", type_value});

    // Add variability: uniform
    crate::CrateValue variability_value;
    variability_value.Set(Variability::Uniform);
    order_fields.push_back({"variability", variability_value});

    // Add default value: token array
    std::vector<value::token> op_order_tokens;
    for (const auto& tok : xform_op_order) {
      op_order_tokens.push_back(tok);
    }

    crate::CrateValue default_value;
    default_value.Set(op_order_tokens);
    order_fields.push_back({"default", default_value});

    // Create the Attribute spec
    if (!AddSpec(order_path, SpecType::Attribute, std::move(order_fields), err)) {
      DCOUT("WARNING: Failed to add spec for xformOpOrder");
    }
  }

  return true;
}

// Every UsdGeomImageable carries `visibility` and `purpose` as TYPED fields, so
// each writer has to emit them explicitly -- and the ones that forgot dropped
// them silently on write (Scope, all the lights, Volume, Material/NodeGraph).
// One shared implementation so a new imageable cannot quietly lose them again.
bool CrateWriter::ExtractImageableAttrs(
  const TypedAttributeWithFallback<Animatable<Visibility>>& visibility,
  const TypedAttributeWithFallback<Purpose>& purpose,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  (void)err;

  // NOTE: an AUTHORED opinion is written even when it equals the schema fallback.
  // `visibility = "inherited"` is not the same as no opinion at all: an authored
  // opinion blocks weaker ones during composition, so dropping it because it
  // "looks like the default" silently changes what the layer means.
  if (visibility.authored()) {
    const auto& vis_animatable = visibility.get_value();
    if (vis_animatable.has_default()) {
      Visibility vis_val;
      if (vis_animatable.get_default(&vis_val)) {
        crate::CrateValue vis_crate_val;
        vis_crate_val.Set(value::token(to_string(vis_val)));
        fields.push_back({"visibility", vis_crate_val});
      }
    }

    // Animated visibility (TimeSamples). Enum timesamples are stored as int64;
    // cast back to Visibility and emit token timeSamples for the crate writer.
    if (vis_animatable.has_timesamples()) {
      const value::TimeSamples *vis_ts = vis_animatable.get_timesamples_ptr();

      value::TimeSamples ts;
      const auto &samples = vis_ts->get_samples();
      for (size_t i = 0; i < samples.size(); i++) {
        if (samples[i].blocked) {
          ts.add_blocked_sample(samples[i].t, value::Value());
        } else if (const int64_t *iv = samples[i].value.as<int64_t>()) {
          value::token vis_token(to_string(static_cast<Visibility>(*iv)));
          ts.add_sample(samples[i].t, value::Value(vis_token));
        }
      }

      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      fields.push_back({"visibility.timeSamples", ts_crate_val});
    }
  }

  if (purpose.authored()) {
    crate::CrateValue purpose_crate_val;
    purpose_crate_val.Set(value::token(to_string(purpose.get_value())));
    fields.push_back({"purpose", purpose_crate_val});
  }

  return true;
}

// ============================================================================
// Scope Property Extraction
// ============================================================================

// Scope is imageable but NOT a GPrim, so ExtractGPrimProperties skips it and its
// typed `visibility` / `purpose` would never reach the crate. Authoring purpose
// on a Scope is the standard way to ship a render/proxy pair, so losing it drops
// the asset's whole purpose structure on write.
bool CrateWriter::ExtractScopeProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  (void)prim_path;
  const Scope* scope = prim.data().as<Scope>();
  if (!scope) {
    if (err) *err = "Failed to cast prim to Scope";
    return false;
  }

  return ExtractImageableAttrs(scope->visibility, scope->purpose, fields, err);
}

// ============================================================================
// GPrim Common Property Extraction
// ============================================================================

bool CrateWriter::ExtractGPrimProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  // Extract xformOps (GPrim inherits from Xformable)
  if (!ExtractXformOpsFromXformable(prim, prim_path, fields, err)) {
  }

  // Try to get as GPrim to access common properties.
  // value::Value::as<GPrim> requires the *exact* stored type to be GPrim, so
  // typed subclasses (Xform, GeomMesh, etc.) won't match here. Probe each
  // subclass and grab the GPrim base via a static_cast through the typed
  // pointer.
  const GPrim* gprim = prim.data().as<GPrim>();
  if (!gprim) {
    // Each probe is wrapped in its own block so the local pointer goes out
    // of scope before the next macro expansion — avoids -Wshadow on
    // chained `if (auto *t = …) else if (auto *t = …)`.
#define TRY_AS_GPRIM(__TY) if (!gprim) { if (auto *p = prim.data().as<__TY>()) gprim = static_cast<const GPrim *>(p); }
    TRY_AS_GPRIM(Xform)
    TRY_AS_GPRIM(Volume)
    TRY_AS_GPRIM(GeomMesh)
    TRY_AS_GPRIM(GeomSphere)
    TRY_AS_GPRIM(GeomCube)
    TRY_AS_GPRIM(GeomCylinder)
    TRY_AS_GPRIM(GeomCone)
    TRY_AS_GPRIM(GeomCapsule)
    TRY_AS_GPRIM(GeomCamera)
    TRY_AS_GPRIM(GeomPoints)
    TRY_AS_GPRIM(GeomBasisCurves)
    TRY_AS_GPRIM(GeomNurbsCurves)
    TRY_AS_GPRIM(GeomHermiteCurves)
    TRY_AS_GPRIM(GeomPlane)
    TRY_AS_GPRIM(GeomCylinder_1)
    TRY_AS_GPRIM(GeomCapsule_1)
    TRY_AS_GPRIM(GeomTetMesh)
    TRY_AS_GPRIM(GeomNurbsPatch)
    TRY_AS_GPRIM(GeomPointInstancer)
#undef TRY_AS_GPRIM
  }
  if (!gprim) {
    // Not a GPrim, that's okay
    return true;
  }

  // Extract the imageable attributes (visibility / purpose) shared with every
  // other imageable prim -- Scope, the lights, Material/NodeGraph.
  if (!ExtractImageableAttrs(gprim->visibility, gprim->purpose, fields, err)) {
    return false;
  }

  // Extract proxyPrim (rel proxyPrim). Parsed into a typed field on every
  // GPrim but never re-emitted by this function, so it was silently dropped
  // on USDC round-trip -- same class of bug as visibility/purpose above.
  if (gprim->proxyPrim.authored()) {
    if (!ConvertRelationshipToFields("proxyPrim", gprim->proxyPrim.relationship(),
                                     prim_path, err)) {
      return false;
    }
  }

  // Extract extent (bounding box) - stored as float3[2] in USD
  //
  // `has_value()` is not an authored test: a DECLARED-but-value-less
  // `float3[] extent` has no value, so the guard skipped it and the declaration
  // was dropped outright.
  if (gprim->extent.authored() && gprim->extent.is_value_empty()) {
    sconv_detail::EmitAttrDeclaration("extent", "float3[]", fields);
  }

  if (gprim->extent.has_value()) {
    auto extent_animatable = gprim->extent.get_value();
    if (extent_animatable && extent_animatable->has_default()) {
      Extent extent_val;
      if (extent_animatable->get_default(&extent_val)) {
        // Convert Extent struct to float3[2]
        std::vector<value::float3> extent_vec = {extent_val.lower, extent_val.upper};
        crate::CrateValue extent_crate_val;
        value::Value extent_value(extent_vec);
        if (ConvertValue(extent_value, extent_crate_val, err)) {
          fields.push_back({"extent", extent_crate_val});
          // Preserve the role spelling degraded by ConvertValue (point3f[] ->
          // float3[] crate value): declare it explicitly or the attribute is
          // re-typed on the wire (see AddArrayAttribute).
          if (extent_value.type_name() != extent_crate_val.type_name()) {
            crate::CrateValue ty_val;
            ty_val.Set(value::token(extent_value.type_name()));
            fields.push_back({std::string("extent") + ".typeName", ty_val});
          }
        }
      }
    }

    // ANIMATED extent. This branch did not exist: the writer emitted only the
    // default, so `extent.timeSamples` was dropped wholesale. Each sample is an
    // Extent struct in memory and float3[2] on disk, the same shape conversion
    // the default above does.
    if (extent_animatable && extent_animatable->has_timesamples()) {
      const value::TimeSamples *ext_ts = extent_animatable->get_timesamples_ptr();

      value::TimeSamples ts;
      const auto &samples = ext_ts->get_samples();
      for (size_t i = 0; i < samples.size(); i++) {
        if (samples[i].blocked) {
          ts.add_blocked_sample(samples[i].t, value::Value());
        } else if (const Extent *ev = samples[i].value.as<Extent>()) {
          std::vector<value::float3> ev_vec = {ev->lower, ev->upper};
          ts.add_sample(samples[i].t, value::Value(ev_vec));
        } else {
          // Already float3[2] (e.g. a programmatically-built prim).
          ts.add_sample(samples[i].t, samples[i].value);
        }
      }

      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(ts);
      fields.push_back({"extent.timeSamples", ts_crate_val});
    }
  }

  // doubleSided is handled in ConvertSinglePrim via ConvertPropertyToFields
  // (routing through the fields path doesn't preserve the bool type correctly)

  // Extract orientation
  if (gprim->orientation.authored()) {
    Orientation orient_val = gprim->orientation.get_value();
    {  // Write it even when it EQUALS the default: `uniform token orientation =
       // "rightHanded"` is an authored opinion, and an authored opinion blocks
       // weaker ones during composition. Skipping it "because it looks like the
       // default" silently changed what the layer means.
      crate::CrateValue orient_crate_val;
      value::token orient_tok(to_string(orient_val));
      orient_crate_val.Set(orient_tok);
      fields.push_back({"orientation", orient_crate_val});
    }
  }

  return true;
}

bool CrateWriter::AddMaterialBindingSpecs(
  const Prim& prim,
  const Path& prim_path,
  std::string* err
) {
  // Extract and write material binding relationships from a Prim
  // Material bindings are relationships that need separate specs in Crate format

  // Try to get MaterialBinding interface from the prim data
  // Geometry types and Group types inherit from GPrim which inherits from MaterialBinding
  const GeomSphere* geom_sphere = prim.data().as<GeomSphere>();
  const GeomCone* geom_cone = nullptr;
  const GeomCapsule* geom_capsule = nullptr;
  const GeomPoints* geom_points = nullptr;
  const GeomCube* geom_cube = nullptr;
  const GeomCylinder* geom_cylinder = nullptr;
  const GeomMesh* geom_mesh = nullptr;
  const GeomSubset* geom_subset = nullptr;
  const GeomCamera* geom_camera = nullptr;
  const GeomBasisCurves* geom_basis_curves = nullptr;
  const GeomNurbsCurves* geom_nurbs_curves = nullptr;
  const GeomPointInstancer* geom_point_instancer = nullptr;
  const Xform* xform = nullptr;
  const Model* model = nullptr;
  const Scope* scope = nullptr;

  // Determine which type the prim is and get the MaterialBinding interface
  const MaterialBinding* mat_binding = nullptr;

  if (geom_sphere) {
    mat_binding = static_cast<const MaterialBinding*>(geom_sphere);
  } else if ((geom_cone = prim.data().as<GeomCone>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_cone);
  } else if ((geom_capsule = prim.data().as<GeomCapsule>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_capsule);
  } else if ((geom_points = prim.data().as<GeomPoints>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_points);
  } else if ((geom_cube = prim.data().as<GeomCube>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_cube);
  } else if ((geom_cylinder = prim.data().as<GeomCylinder>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_cylinder);
  } else if ((geom_mesh = prim.data().as<GeomMesh>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_mesh);
  } else if ((geom_subset = prim.data().as<GeomSubset>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_subset);
  } else if ((geom_camera = prim.data().as<GeomCamera>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_camera);
  } else if ((geom_basis_curves = prim.data().as<GeomBasisCurves>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_basis_curves);
  } else if ((geom_nurbs_curves = prim.data().as<GeomNurbsCurves>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_nurbs_curves);
  } else if (auto* geom_plane = prim.data().as<GeomPlane>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_plane);
  } else if (auto* geom_cylinder1 = prim.data().as<GeomCylinder_1>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_cylinder1);
  } else if (auto* geom_capsule1 = prim.data().as<GeomCapsule_1>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_capsule1);
  } else if (auto* geom_tetmesh = prim.data().as<GeomTetMesh>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_tetmesh);
  } else if (auto* geom_nurbspatch = prim.data().as<GeomNurbsPatch>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_nurbspatch);
  } else if (auto* geom_hermite = prim.data().as<GeomHermiteCurves>()) {
    mat_binding = static_cast<const MaterialBinding*>(geom_hermite);
  } else if ((geom_point_instancer = prim.data().as<GeomPointInstancer>())) {
    mat_binding = static_cast<const MaterialBinding*>(geom_point_instancer);
  } else if ((xform = prim.data().as<Xform>())) {
    mat_binding = static_cast<const MaterialBinding*>(xform);
  } else if ((model = prim.data().as<Model>())) {
    mat_binding = static_cast<const MaterialBinding*>(model);
  } else if ((scope = prim.data().as<Scope>())) {
    mat_binding = static_cast<const MaterialBinding*>(scope);
  }

  if (!mat_binding) {
    // Prim type doesn't support material bindings
    return true;
  }

  // Add material:binding relationship if present
  if (mat_binding->has_materialBinding()) {
    const Relationship& binding = mat_binding->materialBinding.relationship();
    if (!ConvertRelationshipToFields("material:binding", binding, prim_path, err)) {
      if (err) *err = "Failed to add material:binding relationship: " + *err;
      return false;
    }
  }

  // Add material:binding:preview relationship if present
  if (mat_binding->has_materialBindingPreview()) {
    const Relationship& binding = mat_binding->materialBindingPreview.relationship();
    if (!ConvertRelationshipToFields("material:binding:preview", binding, prim_path, err)) {
      if (err) *err = "Failed to add material:binding:preview relationship: " + *err;
      return false;
    }
  }

  // Add material:binding:full relationship if present
  if (mat_binding->has_materialBindingFull()) {
    const Relationship& binding = mat_binding->materialBindingFull.relationship();
    if (!ConvertRelationshipToFields("material:binding:full", binding, prim_path, err)) {
      if (err) *err = "Failed to add material:binding:full relationship: " + *err;
      return false;
    }
  }

  // Add other purpose-specific material bindings from materialBindingMap
  for (const auto& binding_entry : mat_binding->materialBindingMap()) {
    const std::string& purpose = binding_entry.first;
    const Relationship& rel = binding_entry.second;

    std::string rel_name = "material:binding";
    if (!purpose.empty()) {
      rel_name += ":" + purpose;
    }

    if (!ConvertRelationshipToFields(rel_name, rel, prim_path, err)) {
      if (err) *err = "Failed to add " + rel_name + " relationship: " + *err;
      return false;
    }
  }

  // Add collection-based material bindings. The map's outer key is the
  // binding/collection NAME (e.g. "beauty"); the inner dict's key is the
  // material PURPOSE ("" for no purpose). The property name syntax orders
  // these `material:binding:collection:<purpose>:<name>` -- purpose FIRST --
  // when a purpose is present. The old code always wrote name-then-purpose
  // (correct only for the no-purpose case, where there is nothing to put
  // first), so a purpose-qualified binding like
  // `material:binding:collection:mypurpose:beauty` came back on USDC
  // round-trip as `material:binding:collection:beauty:mypurpose`.
  for (const auto& coll_entry : mat_binding->materialBindingCollectionMap()) {
    const std::string& coll_name = coll_entry.first;
    const auto& purpose_map = coll_entry.second;

    for (const auto& purpose : purpose_map.keys()) {
      const Relationship* rel = nullptr;
      if (purpose_map.at(purpose, &rel)) {
        std::string rel_name = "material:binding:collection:";
        if (!purpose.empty()) {
          rel_name += purpose + ":";
        }
        rel_name += coll_name;

        if (!ConvertRelationshipToFields(rel_name, *rel, prim_path, err)) {
          if (err) *err = "Failed to add " + rel_name + " relationship: " + *err;
          return false;
        }
      }
    }
  }

  return true;
}


// ============================================================================
// SkelBindingAPI relationship specs
// ============================================================================
//
// skel:animationSource, skel:skeleton and skel:blendShapeTargets are stored as
// typed `nonstd::optional<Relationship>` fields on Skeleton/SkelRoot/GeomMesh
// rather than in the generic `props` map, so the generic props serialization in
// ConvertSinglePrim() never visits them. Without this, a USDC roundtrip silently
// drops e.g. the SkelRoot/Skeleton -> SkelAnimation binding, losing all skeletal
// animation even though the SkelAnimation prim itself survives.
bool CrateWriter::AddSkelBindingSpecs(
  const Prim& prim,
  const Path& prim_path,
  std::string* err
) {
  auto emit = [&](const char* rel_name,
                  const nonstd::optional<Relationship>& rel) -> bool {
    if (!rel.has_value()) {
      return true;
    }
    if (!ConvertRelationshipToFields(rel_name, rel.value(), prim_path, err)) {
      if (err) *err = std::string("Failed to add ") + rel_name + " relationship: " + *err;
      return false;
    }
    return true;
  };

  if (const Skeleton* skel = prim.data().as<Skeleton>()) {
    return emit("skel:animationSource", skel->animationSource);
  }
  if (const SkelRoot* skel_root = prim.data().as<SkelRoot>()) {
    return emit("skel:animationSource", skel_root->animationSource) &&
           emit("skel:skeleton", skel_root->skeleton);
  }
  if (const GeomMesh* mesh = prim.data().as<GeomMesh>()) {
    return emit("skel:skeleton", mesh->skeleton) &&
           emit("skel:blendShapeTargets", mesh->blendShapeTargets);
  }

  return true;
}


// ============================================================================
// PointInstancer prototypes relationship spec
// ============================================================================

bool CrateWriter::AddPointInstancerPrototypesSpec(
  const Prim& prim,
  const Path& prim_path,
  std::string* err
) {
  const GeomPointInstancer* instancer = prim.data().as<GeomPointInstancer>();
  if (!instancer) {
    return true;
  }

  if (instancer->prototypes.has_value()) {
    const Relationship& protos = instancer->prototypes.value();
    if (!ConvertRelationshipToFields("prototypes", protos, prim_path, err)) {
      if (err) *err = "Failed to add prototypes relationship: " + *err;
      return false;
    }
  }

  return true;
}

// ============================================================================
// GeomPlane Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomPlaneProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomPlane* plane = prim.data().as<GeomPlane>();
  if (!plane) {
    if (err) *err = "Failed to cast prim to GeomPlane";
    return false;
  }

  if (!EmitTypedAnimatableAttr("width", plane->width, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("length", plane->length, fields, err)) {
    return false;
  }

  // Extract axis (uniform token, non-animatable)
  if (plane->axis.authored()) {
    const Axis& axis_val = plane->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// GeomCylinder_1 Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomCylinder1Properties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCylinder_1* cylinder = prim.data().as<GeomCylinder_1>();
  if (!cylinder) {
    if (err) *err = "Failed to cast prim to GeomCylinder_1";
    return false;
  }

  if (!EmitTypedAnimatableAttr("height", cylinder->height, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("radiusTop", cylinder->radiusTop, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("radiusBottom", cylinder->radiusBottom, fields, err)) {
    return false;
  }

  // Extract axis (uniform token, non-animatable)
  if (cylinder->axis.authored()) {
    const Axis& axis_val = cylinder->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// GeomCapsule_1 Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomCapsule1Properties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomCapsule_1* capsule = prim.data().as<GeomCapsule_1>();
  if (!capsule) {
    if (err) *err = "Failed to cast prim to GeomCapsule_1";
    return false;
  }

  if (!EmitTypedAnimatableAttr("height", capsule->height, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("radiusTop", capsule->radiusTop, fields, err)) {
    return false;
  }
  if (!EmitTypedAnimatableAttr("radiusBottom", capsule->radiusBottom, fields, err)) {
    return false;
  }

  // Extract axis (uniform token, non-animatable)
  if (capsule->axis.authored()) {
    const Axis& axis_val = capsule->axis.get_value();
    std::string axis_str = to_string(axis_val);
    crate::CrateValue axis_crate_val;
    value::token axis_token(axis_str);
    axis_crate_val.Set(axis_token);
    fields.push_back({"axis", axis_crate_val});
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// GeomTetMesh Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomTetMeshProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomTetMesh* tetmesh = prim.data().as<GeomTetMesh>();
  if (!tetmesh) {
    if (err) *err = "Failed to cast prim to GeomTetMesh";
    return false;
  }

  // Extract points (point3f[])
  if (tetmesh->points.authored()) {
    auto points_opt = tetmesh->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[])
  if (tetmesh->velocities.authored()) {
    auto velocities_opt = tetmesh->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[])
  if (tetmesh->accelerations.authored()) {
    auto accelerations_opt = tetmesh->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract normals (normal3f[])
  if (tetmesh->normals.authored()) {
    auto normals_opt = tetmesh->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract tetVertexIndices (int4[])
  if (tetmesh->tetVertexIndices.authored()) {
    auto indices_opt = tetmesh->tetVertexIndices.get_value();
    if (indices_opt.has_value()) {
      const Animatable<std::vector<value::int4>>& indices_anim = indices_opt.value();
      if (indices_anim.has_default()) {
        std::vector<value::int4> indices_val;
        if (indices_anim.get_default(&indices_val)) {
          value::Value indices_value(indices_val);
          if (!AddArrayAttribute("tetVertexIndices", indices_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract surfaceFaceVertexIndices (int3[])
  if (tetmesh->surfaceFaceVertexIndices.authored()) {
    auto indices_opt = tetmesh->surfaceFaceVertexIndices.get_value();
    if (indices_opt.has_value()) {
      const Animatable<std::vector<value::int3>>& indices_anim = indices_opt.value();
      if (indices_anim.has_default()) {
        std::vector<value::int3> indices_val;
        if (indices_anim.get_default(&indices_val)) {
          value::Value indices_value(indices_val);
          if (!AddArrayAttribute("surfaceFaceVertexIndices", indices_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// GeomNurbsPatch Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomNurbsPatchProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomNurbsPatch* patch = prim.data().as<GeomNurbsPatch>();
  if (!patch) {
    if (err) *err = "Failed to cast prim to GeomNurbsPatch";
    return false;
  }

  // Extract points (point3f[])
  if (patch->points.authored()) {
    auto points_opt = patch->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[])
  if (patch->velocities.authored()) {
    auto velocities_opt = patch->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[])
  if (patch->accelerations.authored()) {
    auto accelerations_opt = patch->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract normals (normal3f[])
  if (patch->normals.authored()) {
    auto normals_opt = patch->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract uVertexCount (int)
  if (patch->uVertexCount.authored()) {
    auto opt = patch->uVertexCount.get_value();
    if (opt.has_value()) {
      const Animatable<int>& anim = opt.value();
      if (anim.has_default()) {
        int val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"uVertexCount", crate_val});
        }
      }
    }
  }

  // Extract vVertexCount (int)
  if (patch->vVertexCount.authored()) {
    auto opt = patch->vVertexCount.get_value();
    if (opt.has_value()) {
      const Animatable<int>& anim = opt.value();
      if (anim.has_default()) {
        int val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"vVertexCount", crate_val});
        }
      }
    }
  }

  // Extract uOrder (int)
  if (patch->uOrder.authored()) {
    auto opt = patch->uOrder.get_value();
    if (opt.has_value()) {
      const Animatable<int>& anim = opt.value();
      if (anim.has_default()) {
        int val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"uOrder", crate_val});
        }
      }
    }
  }

  // Extract vOrder (int)
  if (patch->vOrder.authored()) {
    auto opt = patch->vOrder.get_value();
    if (opt.has_value()) {
      const Animatable<int>& anim = opt.value();
      if (anim.has_default()) {
        int val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"vOrder", crate_val});
        }
      }
    }
  }

  // Extract uKnots (double[])
  if (patch->uKnots.authored()) {
    auto opt = patch->uKnots.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<double>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<double> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("uKnots", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract vKnots (double[])
  if (patch->vKnots.authored()) {
    auto opt = patch->vKnots.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<double>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<double> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("vKnots", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract uForm (uniform token with fallback)
  if (patch->uForm.authored()) {
    const value::token& tok = patch->uForm.get_value();
    crate::CrateValue crate_val;
    crate_val.Set(tok);
    fields.push_back({"uForm", crate_val});
  }

  // Extract vForm (uniform token with fallback)
  if (patch->vForm.authored()) {
    const value::token& tok = patch->vForm.get_value();
    crate::CrateValue crate_val;
    crate_val.Set(tok);
    fields.push_back({"vForm", crate_val});
  }

  // Extract uRange (double2)
  if (patch->uRange.authored()) {
    auto opt = patch->uRange.get_value();
    if (opt.has_value()) {
      const Animatable<value::double2>& anim = opt.value();
      if (anim.has_default()) {
        value::double2 val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"uRange", crate_val});
        }
      }
    }
  }

  // Extract vRange (double2)
  if (patch->vRange.authored()) {
    auto opt = patch->vRange.get_value();
    if (opt.has_value()) {
      const Animatable<value::double2>& anim = opt.value();
      if (anim.has_default()) {
        value::double2 val;
        if (anim.get_default(&val)) {
          crate::CrateValue crate_val;
          crate_val.Set(val);
          fields.push_back({"vRange", crate_val});
        }
      }
    }
  }

  // Extract pointWeights (double[])
  if (patch->pointWeights.authored()) {
    auto opt = patch->pointWeights.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<double>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<double> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("pointWeights", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract trim curve properties
  if (patch->trimCurve_counts.authored()) {
    auto opt = patch->trimCurve_counts.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<int>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<int> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:counts", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  if (patch->trimCurve_orders.authored()) {
    auto opt = patch->trimCurve_orders.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<int>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<int> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:orders", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  if (patch->trimCurve_vertexCounts.authored()) {
    auto opt = patch->trimCurve_vertexCounts.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<int>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<int> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:vertexCounts", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  if (patch->trimCurve_knots.authored()) {
    auto opt = patch->trimCurve_knots.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<double>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<double> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:knots", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  if (patch->trimCurve_ranges.authored()) {
    auto opt = patch->trimCurve_ranges.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<value::double2>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<value::double2> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:ranges", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  if (patch->trimCurve_points.authored()) {
    auto opt = patch->trimCurve_points.get_value();
    if (opt.has_value()) {
      const Animatable<std::vector<value::double3>>& anim = opt.value();
      if (anim.has_default()) {
        std::vector<value::double3> val;
        if (anim.get_default(&val)) {
          value::Value v(val);
          if (!AddArrayAttribute("trimCurve:points", v, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// GeomHermiteCurves Property Extraction
// ============================================================================

bool CrateWriter::ExtractGeomHermiteCurvesProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const GeomHermiteCurves* hermite = prim.data().as<GeomHermiteCurves>();
  if (!hermite) {
    if (err) *err = "Failed to cast prim to GeomHermiteCurves";
    return false;
  }

  // Extract points (point3f[])
  if (hermite->points.authored()) {
    auto points_opt = hermite->points.get_value();
    if (points_opt.has_value()) {
      const Animatable<std::vector<value::point3f>>& points_anim = points_opt.value();
      if (points_anim.has_default()) {
        std::vector<value::point3f> points_val;
        if (points_anim.get_default(&points_val)) {
          value::Value points_value(points_val);
          if (!AddArrayAttribute("points", points_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract velocities (vector3f[])
  if (hermite->velocities.authored()) {
    auto velocities_opt = hermite->velocities.get_value();
    if (velocities_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& velocities_anim = velocities_opt.value();
      if (velocities_anim.has_default()) {
        std::vector<value::vector3f> velocities_val;
        if (velocities_anim.get_default(&velocities_val)) {
          value::Value velocities_value(velocities_val);
          if (!AddArrayAttribute("velocities", velocities_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract accelerations (vector3f[])
  if (hermite->accelerations.authored()) {
    auto accelerations_opt = hermite->accelerations.get_value();
    if (accelerations_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& accelerations_anim = accelerations_opt.value();
      if (accelerations_anim.has_default()) {
        std::vector<value::vector3f> accelerations_val;
        if (accelerations_anim.get_default(&accelerations_val)) {
          value::Value accelerations_value(accelerations_val);
          if (!AddArrayAttribute("accelerations", accelerations_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract normals (normal3f[])
  if (hermite->normals.authored()) {
    auto normals_opt = hermite->normals.get_value();
    if (normals_opt.has_value()) {
      const Animatable<std::vector<value::normal3f>>& normals_anim = normals_opt.value();
      if (normals_anim.has_default()) {
        std::vector<value::normal3f> normals_val;
        if (normals_anim.get_default(&normals_val)) {
          value::Value normals_value(normals_val);
          if (!AddArrayAttribute("normals", normals_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract curveVertexCounts (int[])
  if (hermite->curveVertexCounts.authored()) {
    auto counts_opt = hermite->curveVertexCounts.get_value();
    if (counts_opt.has_value()) {
      const Animatable<std::vector<int>>& counts_anim = counts_opt.value();
      if (counts_anim.has_default()) {
        std::vector<int> counts_val;
        if (counts_anim.get_default(&counts_val)) {
          value::Value counts_value(counts_val);
          if (!AddArrayAttribute("curveVertexCounts", counts_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract widths (float[])
  if (hermite->widths.authored()) {
    auto widths_opt = hermite->widths.get_value();
    if (widths_opt.has_value()) {
      const Animatable<std::vector<float>>& widths_anim = widths_opt.value();
      if (widths_anim.has_default()) {
        std::vector<float> widths_val;
        if (widths_anim.get_default(&widths_val)) {
          value::Value widths_value(widths_val);
          if (!AddArrayAttribute("widths", widths_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  // Extract tangents (vector3f[]) - HermiteCurves-specific
  if (hermite->tangents.authored()) {
    auto tangents_opt = hermite->tangents.get_value();
    if (tangents_opt.has_value()) {
      const Animatable<std::vector<value::vector3f>>& tangents_anim = tangents_opt.value();
      if (tangents_anim.has_default()) {
        std::vector<value::vector3f> tangents_val;
        if (tangents_anim.get_default(&tangents_val)) {
          value::Value tangents_value(tangents_val);
          if (!AddArrayAttribute("tangents", tangents_value, fields, err)) {
            return false;
          }
        }
      }
    }
  }

  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
