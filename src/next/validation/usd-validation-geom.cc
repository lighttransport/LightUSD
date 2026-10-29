// SPDX-License-Identifier: Apache-2.0
// Geom / topology / skeleton validation rules, extracted from
// usd-validation.cc for compile-time parallelism. Part of the
// validation_detail namespace shared through usd-validation-internal.hh.

#include "usd-validation-internal.hh"
#include "../schema/schema-registry.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace validation_detail {

// ---------------------------------------------------------------------------
// geom rules
// ---------------------------------------------------------------------------

// Ancestor-derived context threaded through the recursive prim walk.

void ValidateGeomCommonProperties(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  static const std::set<std::string> kVisibility = {"inherited", "invisible"};
  static const std::set<std::string> kPurpose = {"default", "render", "proxy",
                                                 "guide"};
  static const std::set<std::string> kOrientation = {"rightHanded",
                                                     "leftHanded"};

  ValidateTokenSetProperty(ps, "visibility", kVisibility,
                           "geom.gprim.visibility", prim_location, result);
  ValidateTokenSetProperty(ps, "purpose", kPurpose, "geom.gprim.purpose",
                           prim_location, result);
  ValidateTokenSetProperty(ps, "orientation", kOrientation,
                           "geom.gprim.orientation", prim_location, result);

  std::vector<std::array<float, 3>> extent;
  if (GetExtentProperty(ps, "extent", &extent)) {
    const std::string location = MakePropertyLocation(prim_location, "extent");
    if (extent.size() != 2) {
      AddError(result, "geom.gprim.extent", location,
               "extent must contain exactly 2 points");
    } else {
      auto finite_point = [](const std::array<float, 3> &p) {
        return std::isfinite(p[0]) && std::isfinite(p[1]) &&
               std::isfinite(p[2]);
      };
      if (!finite_point(extent[0]) || !finite_point(extent[1])) {
        AddError(result, "geom.gprim.extent", location,
                 "extent bounds must be finite");
      }
      for (size_t axis = 0; axis < 3; axis++) {
        if (extent[0][axis] > extent[1][axis]) {
          AddError(result, "geom.gprim.extent", location,
                   "extent minimum must be <= maximum on every axis");
          break;
        }
      }
    }
  }

  size_t point_count = 0;
  const std::string &type_name = ps.type_name();
  if (type_name != "Points" && type_name != "PointInstancer" &&
      type_name != "BasisCurves" && type_name != "NurbsCurves" &&
      type_name != "HermiteCurves" && type_name != "NurbsPatch" &&
      GetPoint3ArrayLen(ps, "points", &point_count)) {
    ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
    ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
  }
}

void ValidateGeomPrimitive(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAxis = {"X", "Y", "Z"};
  const std::string &type_name = ps.type_name();

  if (type_name == "Sphere") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cube") {
    ValidatePositiveNumericProperty(ps, "size", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cone" || type_name == "Cylinder" ||
             type_name == "Capsule") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Cylinder_1" || type_name == "Capsule_1") {
    ValidatePositiveNumericProperty(ps, "radiusTop", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "radiusBottom", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Plane") {
    ValidatePositiveNumericProperty(ps, "width", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "length", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  }
}

void ValidateGeomPoints(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t point_count = 0;
  if (!GetPoint3ArrayLen(ps, "points", &point_count)) {
    return;
  }

  ValidateArrayLengthProperty(ps, "ids", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "widths", point_count, true,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "normals", point_count, true,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.points.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }
  }
}

void ValidateGeomCurves(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kBasisCurveTypes = {"linear", "cubic"};
  static const std::set<std::string> kBasis = {"bezier", "bspline",
                                               "catmullRom"};
  static const std::set<std::string> kWrap = {"nonperiodic", "periodic",
                                              "pinned"};
  static const std::set<std::string> kForm = {"open", "closed", "periodic"};

  if (ps.type_name() == "BasisCurves") {
    ValidateTokenSetProperty(ps, "type", kBasisCurveTypes,
                             "geom.curves.tokens", prim_location, result);
    ValidateTokenSetProperty(ps, "basis", kBasis, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "wrap", kWrap, "geom.curves.tokens",
                             prim_location, result);
  } else if (ps.type_name() == "NurbsPatch") {
    ValidateTokenSetProperty(ps, "uForm", kForm, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "vForm", kForm, "geom.curves.tokens",
                             prim_location, result);
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
  std::vector<int32_t> counts;
  const bool have_counts =
      GetIntArrayProperty(ps, "curveVertexCounts", &counts);
  if (have_counts) {
    size_t total = 0;
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.curves.topology",
                 MakePropertyLocation(prim_location, "curveVertexCounts"),
                 "curveVertexCounts[" + std::to_string(i) +
                     "] must be positive");
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
    if (have_points && total != point_count) {
      AddError(result, "geom.curves.topology",
               MakePropertyLocation(prim_location, "curveVertexCounts"),
               "sum(curveVertexCounts) is " + std::to_string(total) +
                   " but points has " + std::to_string(point_count) +
                   " elements");
    }
  }

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.curves.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }

    const bool length_ok =
        widths.size() == 1 || (have_points && widths.size() == point_count) ||
        (have_counts && widths.size() == counts.size());
    if (!length_ok) {
      AddError(result, "geom.curves.widths",
               MakePropertyLocation(prim_location, "widths"),
               "widths length must be 1, match points, or match curve count");
    }
  }
}

// ---------------------------------------------------------------------------
// UsdVol / UsdRender structural checks (PRODUCT PARITY, not AOUSD Core):
// vol.volume.fieldRel / vol.fieldAsset.* / render.*
// ---------------------------------------------------------------------------

void ValidateVolume(const PrimSpec &ps, const std::string &prim_location,
                    const PrimTypeByPath &prim_types, USDValidationResult *result) {
  // Volume binds its fields through `field:<name>` RELATIONSHIPS; an
  // attribute in that namespace is an authoring error, and a bound field
  // relationship without a target renders nothing. Attributes live in the
  // property slots; relationships (including declared-only ones) live in the
  // prim's relationship map, so both containers are walked.
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (prop_name.compare(0, 6, "field:") != 0) continue;
    if (slot.is_relationship()) continue;  // targets checked below
    AddError(result, "vol.volume.fieldRel",
             MakePropertyLocation(prim_location, prop_name),
             "Volume `" + prop_name +
                 "` must be a relationship targeting a field prim");
  }
  for (const std::string &rel_name : ps.relationship_names()) {
    if (rel_name.compare(0, 6, "field:") != 0) continue;
    const std::vector<Path> *targets = ps.relationship(rel_name);
    if (!targets || targets->empty()) {
      AddWarning(result, "vol.volume.fieldRel",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name +
                     "` has no target");
      continue;
    }
    // UsdVolVolume::GetFieldPaths ignores a field relationship unless it has
    // exactly one target.
    if (targets->size() != 1) {
      AddWarning(result, "vol.volume.fieldTargetCount",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name + "` has " +
                     std::to_string(targets->size()) +
                     " targets; UsdVol uses a field only with exactly one");
      continue;
    }
    // Same-layer targets must be volume fields (VolumeFieldBase).
    const std::string target = (*targets)[0].str();
    const std::string *type = FindPrimType(prim_types, target);
    if (!type || type->empty()) continue;
    const SchemaRegistry &registry = GetSchemaRegistry();
    if (registry.HasParentEntry(*type) &&
        !registry.InheritsFrom(*type, "VolumeFieldBase")) {
      AddWarning(result, "vol.volume.fieldTargetType",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name + "` targets `" + target +
                     "`, a " + *type + ", not a volume field (VolumeFieldBase)");
    }
  }
}

void ValidateFieldAsset(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kVdbDataTypes = {
      "half",  "float",  "double", "int",     "uint",     "int64",
      "half2", "float2", "double2", "int2",   "half3",    "float3",
      "double3", "int3", "matrix3d", "matrix4d", "quatd", "bool",
      "mask",  "string"};
  static const std::set<std::string> kField3dDataTypes = {
      "half", "float", "double", "half3", "float3", "double3"};
  static const std::set<std::string> kFieldClass = {"levelSet", "fogVolume",
                                                    "staggered", "unknown"};
  static const std::set<std::string> kVectorRole = {"None", "Point", "Normal",
                                                    "Vector", "Color"};

  std::string file;
  if (!HasAttributeProp(ps, "filePath")) {
    AddWarning(result, "vol.fieldAsset.filePath",
               MakePropertyLocation(prim_location, "filePath"),
               ps.type_name() + " should author filePath");
  } else {
    // A time-sampled filePath is the canonical animated-volume authoring
    // (one asset per sample); only a sample-less, default-less (or empty)
    // filePath warrants a diagnostic.
    const std::vector<std::pair<double, uint32_t>> *file_samples =
        GetTimeSamplesOf(ps, "filePath");
    if ((!file_samples || file_samples->empty()) &&
        (!GetAssetPathProperty(ps, "filePath", &file) || file.empty())) {
      AddWarning(result, "vol.fieldAsset.filePath",
                 MakePropertyLocation(prim_location, "filePath"),
                 ps.type_name() + " filePath should be a non-empty asset path");
    }
  }
  // pxr usdVol schema: for fieldDataType, "A missing value is considered an
  // error." That statement is about the COMPOSED stage; this checker sees one
  // layer, where the value may legitimately arrive from a sublayer or another
  // arc — so a missing VALUE (no default, no timeSamples; the registry has no
  // fallback either) reports as a warning, not a hard error.
  {
    const std::vector<std::pair<double, uint32_t>> *dt_samples =
        GetTimeSamplesOf(ps, "fieldDataType");
    if (!GetAttrValue(ps, "fieldDataType") &&
        (!dt_samples || dt_samples->empty())) {
      AddWarning(result, "vol.fieldAsset.dataType",
                 MakePropertyLocation(prim_location, "fieldDataType"),
                 ps.type_name() + " does not author a fieldDataType value "
                 "(pxr: a missing value is an error on the composed stage)");
    }
  }
  if (ps.type_name() == "OpenVDBAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kVdbDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
    ValidateTokenSetProperty(ps, "fieldClass", kFieldClass,
                             "vol.fieldAsset.fieldClass", prim_location,
                             result);
  } else if (ps.type_name() == "Field3DAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kField3dDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
  }
  ValidateTokenSetProperty(ps, "vectorDataRoleHint", kVectorRole,
                           "vol.fieldAsset.vectorDataRoleHint", prim_location,
                           result);
}

void ValidateParticleField(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  const bool gaussian = ps.type_name() == "ParticleField3DGaussianSplat";
  if (gaussian) {
    ValidateTokenSetProperty(ps, "projectionModeHint",
                             {"perspective", "tangential"},
                             "vol.particleField.projectionModeHint",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "sortingModeHint",
                             {"zDepth", "cameraDistance", "rayHitDistance"},
                             "vol.particleField.sortingModeHint",
                             prim_location, result);
  }

  const bool has_position_api =
      gaussian || HasAppliedSchema(applied_schemas,
                                   "ParticleFieldPositionAttributeAPI");
  const bool has_kernel_api =
      gaussian ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianEllipsoidAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianSurfletAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelConstantSurfletAPI");
  if (!has_position_api) {
    AddWarning(result, "vol.particleField.position", prim_location,
               "ParticleField has no applied position provider API");
  }
  if (!has_kernel_api) {
    AddWarning(result, "vol.particleField.kernel", prim_location,
               "ParticleField has no applied kernel API");
  }

  size_t particle_count = 0;
  bool have_positions = GetArrayLengthProperty(ps, "positions", &particle_count);
  if (!have_positions) {
    have_positions =
        GetArrayLengthProperty(ps, "positionsh", &particle_count);
  }
  if (!have_positions) return;

  // OpenUSD discards a short auxiliary array and truncates a long one. Report
  // the authored mismatch without treating that defined fallback behavior as
  // a fatal validation error.
  for (const char *prop_name :
       {"orientations", "orientationsh", "scales", "scalesh", "opacities",
        "opacitiesh"}) {
    size_t value_count = 0;
    if (GetArrayLengthProperty(ps, prop_name, &value_count) &&
        value_count != particle_count) {
      AddWarning(result, "vol.particleField.arraySize",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has " +
                     std::to_string(value_count) + " elements for " +
                     std::to_string(particle_count) +
                     " particles; OpenUSD will " +
                     (value_count < particle_count ? "discard" : "truncate") +
                     " it");
    }
  }

  // Spherical harmonics: (degree + 1)^2 coefficients per particle.
  int64_t degree = 3;
  if (const Value *v = GetAttrValue(ps, "radiance:sphericalHarmonicsDegree")) {
    if (const int32_t *i = v->as_int()) degree = *i;
  }
  if (degree < 0) {
    AddWarning(result, "vol.particleField.shDegree",
               MakePropertyLocation(prim_location, "radiance:sphericalHarmonicsDegree"),
               "spherical harmonics degree must not be negative");
    return;
  }
  const uint64_t per_particle = uint64_t(degree + 1) * uint64_t(degree + 1);
  for (const char *prop_name : {"radiance:sphericalHarmonicsCoefficients",
                                "radiance:sphericalHarmonicsCoefficientsh"}) {
    size_t value_count = 0;
    if (GetArrayLengthProperty(ps, prop_name, &value_count) &&
        uint64_t(value_count) != per_particle * particle_count) {
      AddWarning(result, "vol.particleField.shCount",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has " + std::to_string(value_count) +
                     " coefficients; degree " + std::to_string(degree) + " with " +
                     std::to_string(particle_count) + " particles needs " +
                     std::to_string(per_particle * particle_count));
    }
  }
}

void ValidateOpenUsd2608GeomAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  if (HasAppliedSchema(applied_schemas, "GeomModelAPI")) {
    ValidateTokenSetProperty(ps, "model:cardVisibility",
                             {"full", "simple", "inherited"},
                             "geom.model.cardVisibility", prim_location,
                             result);
  }
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "BackPlateAPI" || schema.instance_name.empty()) continue;
    const std::string prop_name = "backPlate:" + schema.instance_name +
                                  ":plateVisibility";
    ValidateTokenSetProperty(ps, prop_name, {"all", "solo", "mute"},
                             "geom.backPlate.visibility", prim_location,
                             result);
  }
}

// pxr usdValidation:AttributeTypeMismatch for schema attributes whose
// DECLARED type must match the registry exactly. Empty means "not declared
// locally" and is fine.
void CheckDeclaredAttrType(const PrimSpec &ps, const char *prop_name,
                           const char *expected,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  const std::string authored = AttrTypeNameOf(ps, prop_name);
  if (!authored.empty() && authored != expected) {
    AddWarning(result, "core.schema.attributeType",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " is declared `" + authored +
                   "` but the schema attribute type is `" + expected + "`");
  }
}

void ValidateRenderPrim(const PrimSpec &ps, const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result) {
  static const std::set<std::string> kConformPolicy = {
      "expandAperture", "cropAperture", "adjustApertureWidth",
      "adjustApertureHeight", "adjustPixelAspectRatio"};
  static const std::set<std::string> kSourceType = {"raw", "primvar", "lpe",
                                                    "intrinsic"};
  static const std::set<std::string> kCameraType = {"Camera"};
  static const std::set<std::string> kProductType = {"RenderProduct"};
  static const std::set<std::string> kVarType = {"RenderVar"};
  static const std::set<std::string> kRenderSourceTypes = {
      "RenderSettings", "RenderProduct"};
  static const std::set<std::string> kPassType = {"RenderPass"};
  const std::string &type_name = ps.type_name();
  if (type_name == "RenderSettings" || type_name == "RenderProduct") {
    // Rule ids name the prim type they fire on (render.settings.* vs
    // render.product.*) so id-based filtering points at the right schema.
    const bool is_settings = type_name == "RenderSettings";
    ValidateTokenSetProperty(ps, "aspectRatioConformPolicy", kConformPolicy,
                             is_settings
                                 ? "render.settings.aspectRatioConformPolicy"
                                 : "render.product.aspectRatioConformPolicy",
                             prim_location, result);
    // Per pxr, `products` is a RenderSettings property and `orderedVars` a
    // RenderProduct property; the same name on the OTHER type is an ordinary
    // custom property and must not be kind-checked.
    const char *rels[2] = {"camera",
                           is_settings ? "products" : "orderedVars"};
    for (const char *rel : rels) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result,
                 is_settings ? "render.settings.relationship"
                             : "render.product.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    if (!is_settings) {
      CheckDeclaredAttrType(ps, "productName", "token", prim_location, result);
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "camera",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location, "camera"), prim_types, kCameraType,
        result);
    ValidateRelationshipTargetPrimTypes(
        ps, is_settings ? "products" : "orderedVars",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location,
                             is_settings ? "products" : "orderedVars"),
        prim_types, is_settings ? kProductType : kVarType, result);
  } else if (type_name == "RenderVar") {
    ValidateTokenSetProperty(ps, "sourceType", kSourceType,
                             "render.var.sourceType", prim_location, result);
    CheckDeclaredAttrType(ps, "sourceName", "string", prim_location, result);
  } else if (type_name == "RenderPass") {
    for (const char *rel : {"renderSource", "inputPasses"}) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result, "render.pass.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "renderSource", "render.pass.targetType",
        MakePropertyLocation(prim_location, "renderSource"), prim_types,
        kRenderSourceTypes, result);
    ValidateRelationshipTargetPrimTypes(
        ps, "inputPasses", "render.pass.targetType",
        MakePropertyLocation(prim_location, "inputPasses"), prim_types,
        kPassType, result);
  }
}

void ValidatePointInstancer(const PrimSpec &ps,
                            const std::string &prim_location,
                            USDValidationResult *result) {
  size_t position_count = 0;
  const bool have_positions =
      GetPoint3ArrayLen(ps, "positions", &position_count);

  size_t prototype_count = 0;
  if (!HasProperty(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "PointInstancer must author rel prototypes");
  } else if (!IsRelationshipProp(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "prototypes must be a relationship");
  } else {
    const std::vector<Path> targets = RelTargets(ps, "prototypes");
    prototype_count = targets.size();
    if (targets.empty()) {
      AddError(result, "geom.pointInstancer.prototypes",
               MakePropertyLocation(prim_location, "prototypes"),
               "prototypes relationship must target at least one prototype");
    }
    for (const Path &target : targets) {
      std::string err;
      if (!IsValidScenePathString(target.str(), &err)) {
        AddError(result, "geom.pointInstancer.prototypes",
                 MakePropertyLocation(prim_location, "prototypes"),
                 "prototypes target `" + target.str() +
                     "` is invalid: " + err);
      }
    }
  }

  std::vector<int32_t> proto_indices;
  if (GetIntArrayProperty(ps, "protoIndices", &proto_indices)) {
    if (have_positions && proto_indices.size() != position_count) {
      AddError(result, "geom.pointInstancer.indices",
               MakePropertyLocation(prim_location, "protoIndices"),
               "protoIndices length must match positions length");
    }
    for (size_t i = 0; i < proto_indices.size(); i++) {
      if (proto_indices[i] < 0 ||
          static_cast<size_t>(proto_indices[i]) >= prototype_count) {
        AddError(result, "geom.pointInstancer.indices",
                 MakePropertyLocation(prim_location, "protoIndices"),
                 "protoIndices[" + std::to_string(i) +
                     "] does not reference an authored prototype");
      }
    }
  }

  if (have_positions) {
    for (const char *prop_name :
         {"orientations", "scales", "ids", "velocities", "accelerations",
          "angularVelocities"}) {
      ValidateArrayLengthProperty(ps, prop_name, position_count, false,
                                  "geom.pointInstancer.arraySize",
                                  prim_location, "positions", result);
    }
  }
}

void ValidateCamera(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result) {
  static const std::set<std::string> kProjection = {"perspective",
                                                    "orthographic"};
  static const std::set<std::string> kStereoRole = {"mono", "left", "right"};

  ValidateTokenSetProperty(ps, "projection", kProjection,
                           "geom.camera.tokens", prim_location, result);
  ValidateTokenSetProperty(ps, "stereoRole", kStereoRole,
                           "geom.camera.tokens", prim_location, result);

  const Value *cr = GetAttrValue(ps, "clippingRange");
  std::array<float, 2> clipping_range = {{0.0f, 0.0f}};
  if (cr && ValueToFloat2(*cr, &clipping_range)) {
    if (!std::isfinite(clipping_range[0]) ||
        !std::isfinite(clipping_range[1]) || clipping_range[0] < 0.0f ||
        clipping_range[1] <= clipping_range[0]) {
      AddError(result, "geom.camera.range",
               MakePropertyLocation(prim_location, "clippingRange"),
               "clippingRange must be finite with near >= 0 and far > near");
    }
  }

  ValidatePositiveNumericProperty(ps, "focalLength", "geom.camera.optics",
                                  prim_location, result);
  ValidatePositiveNumericProperty(ps, "horizontalAperture",
                                  "geom.camera.optics", prim_location, result);
  ValidatePositiveNumericProperty(ps, "verticalAperture",
                                  "geom.camera.optics", prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "fStop", "geom.camera.optics",
                                     prim_location, result);

  double shutter_open = 0.0;
  double shutter_close = 0.0;
  const bool have_open =
      GetNumericScalarProperty(ps, "shutter:open", &shutter_open) ||
      GetNumericScalarProperty(ps, "shutterOpen", &shutter_open);
  const bool have_close =
      GetNumericScalarProperty(ps, "shutter:close", &shutter_close) ||
      GetNumericScalarProperty(ps, "shutterClose", &shutter_close);
  if (have_open && have_close &&
      (!std::isfinite(shutter_open) || !std::isfinite(shutter_close) ||
       shutter_close < shutter_open)) {
    AddError(result, "geom.camera.shutter", prim_location,
             "camera shutter close must be finite and >= shutter open");
  }
}

void ValidateMeshTopology(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result,
                          size_t *out_face_count) {
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  const bool have_counts = GetIntArrayProperty(ps, "faceVertexCounts", &counts);
  const bool have_indices =
      GetIntArrayProperty(ps, "faceVertexIndices", &indices);

  if (out_face_count && have_counts) {
    *out_face_count = counts.size();
  }

  if (!have_counts && !have_indices) {
    return;
  }

  size_t total = 0;
  if (have_counts) {
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.mesh.topology.count",
                 MakePropertyLocation(prim_location, "faceVertexCounts"),
                 "faceVertexCounts[" + std::to_string(i) + "] must be "
                 "positive, but is " + std::to_string(counts[i]));
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
  }

  if (have_counts && have_indices && total != indices.size()) {
    AddError(result, "geom.mesh.topology.size",
             MakePropertyLocation(prim_location, "faceVertexIndices"),
             "sum(faceVertexCounts) is " + std::to_string(total) +
                 " but faceVertexIndices has " +
                 std::to_string(indices.size()) + " elements");
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
  if (have_indices && have_points) {
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 || static_cast<size_t>(indices[i]) >= point_count) {
        AddError(result, "geom.mesh.topology.index",
                 MakePropertyLocation(prim_location, "faceVertexIndices"),
                 "faceVertexIndices[" + std::to_string(i) + "] = " +
                     std::to_string(indices[i]) +
                     " is outside points range [0, " +
                     std::to_string(point_count) + ")");
      }
    }
  }
}

void ValidateGeomSubsetTopology(const PrimSpec &ps,
                                const std::string &prim_location,
                                const AncestorContext &ancestors,
                                USDValidationResult *result) {
  if (ancestors.parent_type != "Mesh" ||
      !ancestors.parent_mesh_face_count_known) {
    return;
  }

  std::string element_type = "face";
  (void)GetTokenProperty(ps, "elementType", &element_type);
  if (element_type != "face") {
    return;
  }

  std::vector<int32_t> indices;
  if (!GetIntArrayProperty(ps, "indices", &indices)) {
    return;
  }

  std::set<int32_t> seen;
  for (size_t i = 0; i < indices.size(); i++) {
    const int32_t index = indices[i];
    if (index < 0 ||
        static_cast<size_t>(index) >= ancestors.parent_mesh_face_count) {
      AddError(result, "geom.subset.indices",
               MakePropertyLocation(prim_location, "indices"),
               "indices[" + std::to_string(i) + "] = " +
                   std::to_string(index) +
                   " is outside parent face range [0, " +
                   std::to_string(ancestors.parent_mesh_face_count) + ")");
      continue;
    }
    if (!seen.insert(index).second) {
      AddWarning(result, "geom.subset.indices",
                 MakePropertyLocation(prim_location, "indices"),
                 "indices contains duplicate element " +
                     std::to_string(index));
    }
  }
}

void ValidatePrimvars(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result) {
  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!IsPrimvarPropertyName(prop_name)) {
      continue;
    }
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    const PropMeta *meta = ps.property_meta(prop_name);
    if (meta && (meta->authored & PropMeta::kInterpolation) &&
        !IsValidPrimvarInterpolation(meta->interpolation)) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "invalid primvar interpolation `" + meta->interpolation + "`");
    }
    if (meta && (meta->authored & PropMeta::kElementSize) &&
        meta->elementSize <= 0) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "primvar elementSize must be greater than 0");
    }

    const std::string indices_name = prop_name + ":indices";
    if (!HasProperty(ps, indices_name)) {
      continue;
    }
    const std::string indices_location =
        MakePropertyLocation(prim_location, indices_name);
    if (IsRelationshipProp(ps, indices_name)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must be an int[] attribute");
      continue;
    }

    std::vector<int32_t> indices;
    if (!GetIntArrayProperty(ps, indices_name, &indices)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must have int[] values");
      continue;
    }

    size_t value_count = 0;
    const bool have_value_count =
        GetArrayLengthProperty(ps, prop_name, &value_count);
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 ||
          (have_value_count &&
           static_cast<size_t>(indices[i]) >= value_count)) {
        std::string msg = "indices[" + std::to_string(i) + "] = " +
                          std::to_string(indices[i]);
        if (have_value_count) {
          msg += " is outside primvar value range [0, " +
                 std::to_string(value_count) + ")";
        } else {
          msg += " must be non-negative";
        }
        AddError(result, "geom.primvar.indices", indices_location, msg);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// skel rules
// ---------------------------------------------------------------------------

void ValidateSkeletonTopology(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result,
                              size_t *out_joint_count) {
  std::vector<std::string> joints;
  if (!GetTokenArrayProperty(ps, "joints", &joints)) {
    AddWarning(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should author uniform token[] joints");
    return;
  }

  if (out_joint_count) {
    *out_joint_count = joints.size();
  }

  if (joints.empty()) {
    AddError(result, "geom.skel.skeleton.joints",
             MakePropertyLocation(prim_location, "joints"),
             "Skeleton joints must not be empty");
    return;
  }

  std::unordered_map<std::string, size_t> joint_to_index;
  size_t root_count = 0;
  for (size_t i = 0; i < joints.size(); i++) {
    const std::string &joint = joints[i];
    if (joint.empty() || joint[0] == '/' || EndsWith(joint, "/") ||
        joint.find("//") != std::string::npos) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "joints[" + std::to_string(i) +
                   "] must be a relative joint path, but is `" + joint + "`");
      continue;
    }
    if (!joint_to_index.emplace(joint, i).second) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "duplicate joint path `" + joint + "`");
      continue;
    }

    const size_t slash = joint.find_last_of('/');
    if (slash == std::string::npos) {
      root_count++;
      continue;
    }

    const std::string parent = joint.substr(0, slash);
    auto parent_it = joint_to_index.find(parent);
    if (parent_it == joint_to_index.end() || parent_it->second >= i) {
      AddError(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "joint `" + joint +
                   "` must appear after its parent joint `" + parent + "`");
    }
  }

  if (root_count != 1) {
    AddWarning(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should have one root joint, but has " +
                   std::to_string(root_count));
  }

  if (HasAttributeProp(ps, "jointNames")) {
    size_t count = 0;
    if (GetArrayLengthProperty(ps, "jointNames", &count) &&
        count != joints.size()) {
      AddError(result, "geom.skel.skeleton.jointNames",
               MakePropertyLocation(prim_location, "jointNames"),
               "jointNames has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
  }

  for (const char *prop_name : {"bindTransforms", "restTransforms"}) {
    if (!HasAttributeProp(ps, prop_name)) {
      continue;
    }
    size_t count = 0;
    if (!GetArrayLengthProperty(ps, prop_name, &count)) {
      AddWarning(result, "geom.skel.skeleton.transforms",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " should be an authored matrix4d[] value");
      continue;
    }
    if (count != joints.size()) {
      AddError(result, "geom.skel.skeleton.transforms",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
    ValidateTimeSamplesProp(ps, prop_name, "geom.skel.skeleton.transforms",
                            MakePropertyLocation(prim_location, prop_name),
                            result);
  }
}

void ValidateSkelAnimation(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  std::vector<std::string> joints;
  const bool have_joints = GetTokenArrayProperty(ps, "joints", &joints);
  if (!have_joints) {
    AddWarning(result, "geom.skel.animation.joints",
               MakePropertyLocation(prim_location, "joints"),
               "SkelAnimation should author uniform token[] joints");
  }

  if (have_joints) {
    for (const char *prop_name : {"translations", "rotations", "scales"}) {
      if (!HasAttributeProp(ps, prop_name)) {
        continue;
      }
      ValidateArrayLengthAtAllSamples(
          ps, prop_name, "geom.skel.animation.transforms",
          MakePropertyLocation(prim_location, prop_name), joints.size(),
          "joints", result);
    }
  }

  std::vector<std::string> blend_shapes;
  if (GetTokenArrayProperty(ps, "blendShapes", &blend_shapes)) {
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < blend_shapes.size(); i++) {
      const std::string &name = blend_shapes[i];
      if (name.empty()) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "blendShapes[" + std::to_string(i) + "] must not be empty");
      } else if (!seen.emplace(name).second) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "duplicate blendShape token `" + name + "`");
      }
    }

    if (HasAttributeProp(ps, "blendShapeWeights")) {
      ValidateArrayLengthAtAllSamples(
          ps, "blendShapeWeights", "geom.skel.animation.blendShapes",
          MakePropertyLocation(prim_location, "blendShapeWeights"),
          blend_shapes.size(), "blendShapes", result);
    }
  } else if (HasAttributeProp(ps, "blendShapeWeights")) {
    AddWarning(result, "geom.skel.animation.blendShapes",
               MakePropertyLocation(prim_location, "blendShapeWeights"),
               "blendShapeWeights is authored without uniform token[] "
               "blendShapes");
  }
}

void ValidateBlendShape(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t offset_count = 0;
  bool have_offsets = false;
  if (!HasAttributeProp(ps, "offsets")) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape must author uniform vector3f[] offsets");
  } else if (!GetArrayLengthProperty(ps, "offsets", &offset_count)) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape offsets must be an authored array value");
  } else {
    have_offsets = true;
    if (offset_count == 0) {
      AddWarning(result, "geom.skel.blendShape.offsets",
                 MakePropertyLocation(prim_location, "offsets"),
                 "BlendShape offsets should not be empty");
    }
  }

  if (HasAttributeProp(ps, "normalOffsets")) {
    ValidateTimeSamplesProp(
        ps, "normalOffsets", "geom.skel.blendShape.normalOffsets",
        MakePropertyLocation(prim_location, "normalOffsets"), result);
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "normalOffsets", "geom.skel.blendShape.normalOffsets",
          MakePropertyLocation(prim_location, "normalOffsets"), offset_count,
          "offsets", result);
    }
  }

  if (HasAttributeProp(ps, "pointIndices")) {
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "pointIndices", "geom.skel.blendShape.pointIndices",
          MakePropertyLocation(prim_location, "pointIndices"), offset_count,
          "offsets", result);
    }

    std::vector<int32_t> indices;
    if (GetIntArrayProperty(ps, "pointIndices", &indices)) {
      std::unordered_set<int32_t> seen;
      for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] < 0) {
          AddError(result, "geom.skel.blendShape.pointIndices",
                   MakePropertyLocation(prim_location, "pointIndices"),
                   "pointIndices[" + std::to_string(i) +
                       "] must be non-negative");
        } else if (!seen.emplace(indices[i]).second) {
          AddWarning(result, "geom.skel.blendShape.pointIndices",
                     MakePropertyLocation(prim_location, "pointIndices"),
                     "duplicate pointIndices value " +
                         std::to_string(indices[i]));
        }
      }
    }
  }

  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!StartsWith(prop_name, "inbetweens:")) {
      continue;
    }
    const std::string location =
        MakePropertyLocation(prim_location, prop_name);
    const PropMeta *meta = ps.property_meta(prop_name);
    if (!meta || !(meta->authored & PropMeta::kWeight)) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` must author weight metadata");
    } else if (!std::isfinite(meta->weight)) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` has a non-finite weight");
    }
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(ps, prop_name,
                                      "geom.skel.blendShape.inbetween",
                                      location, offset_count, "offsets",
                                      result);
    }
  }
}

std::vector<std::string> AllPropertyNames(const PrimSpec &ps);

void ValidateSkelBinding(const PrimSpec &ps,
                         const std::vector<AppliedSchema> &applied_schemas,
                         const std::string &prim_location,
                         const PrimTypeByPath &prim_types,
                         const AncestorContext &ancestors,
                         USDValidationResult *result) {
  bool has_binding_property = false;
  // AllPropertyNames, not slots: a relationship WITH targets (`rel
  // skel:skeleton = </Skel>`) lives only in the relationships map, so the
  // slot-only loop never saw the most common UsdSkelBinding spelling and the
  // missing-SkelBindingAPI check was silent on exactly the prims it existed
  // for (OpenUSD's SkelBindingApiAppliedValidator flags these).
  for (const std::string &name : AllPropertyNames(ps)) {
    if (StartsWith(name, "skel:") || StartsWith(name, "primvars:skel:")) {
      has_binding_property = true;
      break;
    }
  }
  const bool has_binding_api =
      HasAppliedSchema(applied_schemas, "SkelBindingAPI");
  if (has_binding_property && !has_binding_api) {
    AddError(result, "geom.skel.binding.api", prim_location,
             "SkelBinding property is authored without applying "
             "SkelBindingAPI");
  }
  if (has_binding_api && ps.type_name() != "SkelRoot" &&
      !ancestors.has_skel_root_ancestor) {
    AddError(result, "geom.skel.binding.root", prim_location,
             "SkelBindingAPI must be applied on a SkelRoot or beneath one");
  }

  if (HasProperty(ps, "skel:skeleton")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:skeleton", "geom.skel.binding.skeleton",
        MakePropertyLocation(prim_location, "skel:skeleton"), result);
    if (IsRelationshipProp(ps, "skel:skeleton")) {
      const std::vector<Path> targets = RelTargets(ps, "skel:skeleton");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "Skeleton") {
          AddWarning(result, "geom.skel.binding.skeleton",
                     MakePropertyLocation(prim_location, "skel:skeleton"),
                     "skel:skeleton target `" + target.str() +
                         "` has type `" + *target_type +
                         "`, expected Skeleton");
        }
      }
    }
  }

  if (HasProperty(ps, "skel:animationSource")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:animationSource", "geom.skel.binding.animationSource",
        MakePropertyLocation(prim_location, "skel:animationSource"), result);
    if (IsRelationshipProp(ps, "skel:animationSource")) {
      const std::vector<Path> targets =
          RelTargets(ps, "skel:animationSource");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "SkelAnimation") {
          AddWarning(
              result, "geom.skel.binding.animationSource",
              MakePropertyLocation(prim_location, "skel:animationSource"),
              "skel:animationSource target `" + target.str() +
                  "` has type `" + *target_type + "`, expected SkelAnimation");
        }
      }
    }
  }
}

void ValidateSkinningPrimvars(const PrimSpec &ps,
                              const std::string &prim_location,
                              const PrimTypeByPath &prim_types,
                              const SkeletonJointCountByPath &skeleton_joints,
                              USDValidationResult *result) {
  const bool have_indices_attr =
      HasAttributeProp(ps, "primvars:skel:jointIndices");
  const bool have_weights_attr =
      HasAttributeProp(ps, "primvars:skel:jointWeights");
  if (!have_indices_attr && !have_weights_attr) {
    return;
  }

  if (!have_indices_attr || !have_weights_attr) {
    AddError(result, "geom.skel.skinning.pair", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "be authored together");
    return;
  }

  const PropMeta *indices_meta = ps.property_meta("primvars:skel:jointIndices");
  const PropMeta *weights_meta = ps.property_meta("primvars:skel:jointWeights");

  const auto validate_skin_interp = [&](const PropMeta *meta,
                                        const std::string &name) {
    const std::string location = MakePropertyLocation(prim_location, name);
    if (!meta || !(meta->authored & PropMeta::kInterpolation)) {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " must author interpolation metadata");
      return;
    }
    if (meta->interpolation != "vertex" && meta->interpolation != "varying") {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " interpolation must be vertex or varying, but is `" +
                   meta->interpolation + "`");
    }
  };
  validate_skin_interp(indices_meta, "primvars:skel:jointIndices");
  validate_skin_interp(weights_meta, "primvars:skel:jointWeights");

  const bool indices_es_ok = indices_meta &&
                             (indices_meta->authored & PropMeta::kElementSize) &&
                             indices_meta->elementSize > 0;
  const bool weights_es_ok = weights_meta &&
                             (weights_meta->authored & PropMeta::kElementSize) &&
                             weights_meta->elementSize > 0;
  if (!indices_es_ok || !weights_es_ok) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "skinning primvars must author non-zero elementSize metadata");
  } else if (indices_meta->elementSize != weights_meta->elementSize) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "use the same elementSize");
  }

  std::vector<int32_t> joint_indices;
  std::vector<float> joint_weights;
  if (GetIntArrayProperty(ps, "primvars:skel:jointIndices", &joint_indices) &&
      GetFloatArrayProperty(ps, "primvars:skel:jointWeights",
                            &joint_weights)) {
    if (joint_indices.size() != joint_weights.size()) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "jointIndices has " + std::to_string(joint_indices.size()) +
                   " elements, but jointWeights has " +
                   std::to_string(joint_weights.size()) + " elements");
    }

    size_t local_joint_count = 0;
    bool have_local_joint_count = false;
    if (IsRelationshipProp(ps, "skel:skeleton")) {
      const std::vector<Path> targets = RelTargets(ps, "skel:skeleton");
      if (targets.size() == 1) {
        std::string prim_part;
        SplitScenePathString(targets[0].str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type == "Skeleton") {
          auto joint_count_it = skeleton_joints.find(prim_part);
          if (joint_count_it != skeleton_joints.end()) {
            local_joint_count = joint_count_it->second;
            have_local_joint_count = true;
          }
        }
      }
    }

    for (size_t i = 0; i < joint_indices.size(); i++) {
      if (joint_indices[i] < 0) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) +
                     "] must be non-negative");
      } else if (have_local_joint_count &&
                 static_cast<size_t>(joint_indices[i]) >= local_joint_count) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) + "] references joint " +
                     std::to_string(joint_indices[i]) + ", but skeleton has " +
                     std::to_string(local_joint_count) + " joints");
      }
    }

    if (weights_meta && (weights_meta->authored & PropMeta::kElementSize)) {
      const size_t element_size =
          weights_meta->elementSize > 0
              ? static_cast<size_t>(weights_meta->elementSize)
              : 0;
      if (element_size != 0 && (joint_weights.size() % element_size) == 0) {
        for (size_t group = 0; group < joint_weights.size() / element_size;
             group++) {
          double sum = 0.0;
          bool all_finite = true;
          for (size_t item = 0; item < element_size; item++) {
            const float weight = joint_weights[group * element_size + item];
            if (!std::isfinite(weight)) {
              all_finite = false;
            }
            sum += static_cast<double>(weight);
          }
          if (!all_finite) {
            AddError(result, "geom.skel.skinning.weight",
                     MakePropertyLocation(prim_location,
                                          "primvars:skel:jointWeights"),
                     "jointWeights group " + std::to_string(group) +
                         " contains a non-finite value");
          } else if (sum <= 0.0) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " has no positive influence");
          } else if (std::fabs(sum - 1.0) > 1.0e-3) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " sums to " + std::to_string(sum) +
                           " instead of 1");
          }
        }
      }
    }
  }
}


}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
