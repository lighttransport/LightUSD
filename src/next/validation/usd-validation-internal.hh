// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
#pragma once
#include "usd-validation.hh"
#include <array>
#include <set>
#include <unordered_map>
#include <unordered_set>
namespace lightusd {
namespace next {
namespace validation_detail {

struct AppliedSchema {
  std::string name;
  std::string instance_name;
};

struct AncestorContext {
  bool has_gprim_ancestor{false};
  std::string nearest_gprim_path;  // valid only when has_gprim_ancestor
  std::string parent_type;         // immediate parent typeName ("" at root)
  bool parent_mesh_face_count_known{false};
  size_t parent_mesh_face_count{0};
  bool has_skel_root_ancestor{false};
  bool has_articulation_ancestor{false};
  bool has_material_ancestor{false};
  std::string nearest_material_path;  // valid only when has_material_ancestor
};

using ColorSpaceSet = std::set<std::string>;
using PrimTypeByPath = std::unordered_map<std::string, std::string>;
using PrimSpecifierByPath = std::unordered_map<std::string, PrimSpecifier>;
using SkeletonJointCountByPath = std::unordered_map<std::string, size_t>;

bool StartsWith(const std::string &s, const std::string &prefix);
std::string MakePropertyLocation(const std::string &prim_location,
                                 const std::string &prop_name);
void AddError(USDValidationResult *result, const std::string &rule_id,
              const std::string &location, const std::string &message);
void AddWarning(USDValidationResult *result, const std::string &rule_id,
                const std::string &location, const std::string &message);
bool IsXformableTypeName(const std::string &type_name);
bool IsPhysicsPrimTypeName(const std::string &type_name);
bool IsPhysicsSchemaName(const std::string &schema_name);
bool IsRelationshipProp(const PrimSpec &ps, const std::string &name);
bool HasProperty(const PrimSpec &ps, const std::string &name);
const Value *GetAttrValue(const PrimSpec &ps, const std::string &name);
bool ValueToVec3(const Value &v, std::array<double, 3> *out);
bool GetTokenProperty(const PrimSpec &ps, const std::string &name,
                      std::string *out);
bool GetNumericScalarProperty(const PrimSpec &ps, const std::string &name,
                              double *out);
bool GetDoubleArrayProperty(const PrimSpec &ps, const std::string &name,
                            std::vector<double> *out);
bool GetFloatArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<float> *out);
bool GetPoint3ArrayLen(const PrimSpec &ps, const std::string &name,
                       size_t *size);
void ValidateTokenSetProperty(const PrimSpec &ps, const std::string &prop_name,
                              const std::set<std::string> &allowed,
                              const std::string &rule_id,
                              const std::string &prim_location,
                              USDValidationResult *result);
void ValidatePositiveNumericProperty(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &prim_location,
                                     USDValidationResult *result);
void ValidateNonNegativeNumericProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result);
void ValidateNonNegativeIntegerProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result);
void ValidateFiniteVec3Property(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result,
                                bool require_nonzero);
void ValidateFiniteNonNegativeVec3Property(const PrimSpec &ps,
                                           const std::string &prop_name,
                                           const std::string &rule_id,
                                           const std::string &prim_location,
                                           USDValidationResult *result);
void ValidateQuaternionProperty(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result);
void ValidateNumericArrayLengthProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        size_t expected,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result);
void ValidateNumericArrayFiniteProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result);
void ValidateIntArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result);
void ValidateNumericArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result);
void ValidateNumericMinMaxPair(const PrimSpec &ps, const std::string &min_prop,
                               const std::string &max_prop,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result);
void ValidateNumericRangeProperty(const PrimSpec &ps,
                                  const std::string &prop_name,
                                  double min_value, double max_value,
                                  const std::string &rule_id,
                                  const std::string &prim_location,
                                  USDValidationResult *result);
void ValidateNumericMinProperty(const PrimSpec &ps,
                                const std::string &prop_name, double min_value,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result);
void ValidateFiniteNumericProperty(const PrimSpec &ps,
                                   const std::string &prop_name,
                                   const std::string &rule_id,
                                   const std::string &prim_location,
                                   USDValidationResult *result);
void ValidateAssetPathProperty(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result);
void ValidateRelationshipTargetPaths(const PrimSpec &ps,
                                     const std::string &rel_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     USDValidationResult *result);
void ValidateRelationshipTargetPrimTypes(
    const PrimSpec &ps, const std::string &rel_name,
    const std::string &rule_id, const std::string &location,
    const PrimTypeByPath &prim_types,
    const std::set<std::string> &expected_types, USDValidationResult *result);
void CheckDeclaredAttrType(const PrimSpec &ps, const char *prop_name,
                           const char *expected,
                           const std::string &prim_location,
                           USDValidationResult *result);
bool HasAppliedSchema(const std::vector<AppliedSchema> &schemas,
                      const std::string &schema_name);
void ValidateLuxLight(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result);
std::vector<std::string> AllPropertyNames(const PrimSpec &ps);
void ValidatePhysics(const PrimSpec &ps,
                     const std::vector<AppliedSchema> &applied_schemas,
                     const std::string &prim_location,
                     const PrimTypeByPath &prim_types,
                     const AncestorContext &ancestors,
                     USDValidationResult *result);

// Other domain entry points remain compiled in usd-validation.cc for now.
struct PrimChildValidationState;
std::vector<std::string> SplitString(const std::string &s, char delim);
bool EndsWith(const std::string &s, const std::string &suffix);
void AddIssue(USDValidationResult *result, USDValidationSeverity severity,
              const std::string &rule_id, const std::string &location,
              const std::string &message);
bool IsGprimTypeName(const std::string &type_name);
bool IsShadeContainerTypeName(const std::string &type_name);
bool IsShadeConnectableTypeName(const std::string &type_name);
bool IsShadeConnectionTargetTypeName(const std::string &type_name);
bool IsLuxLightTypeName(const std::string &type_name);
bool IsShaderInputName(const std::string &prop_name);
bool IsShaderOutputName(const std::string &prop_name);
bool IsMaterialTerminalOutputName(const std::string &prop_name);
bool IsUsdPrimvarReaderId(const std::string &shader_id);
const std::string *UsdPrimvarReaderResultType(const std::string &shader_id);
bool IsMaterialXShaderId(const std::string &shader_id);
bool IsMaterialXAssetPath(const std::string &asset_path);
bool IsMultipleApplySchemaName(const std::string &schema_name);
bool IsKnownAPISchemaName(const std::string &schema_name);
bool IsNonConnectableImageableTypeName(const std::string &type_name);
const std::unordered_map<std::string, std::string>& UsdPreviewSurfaceInputTypes();
std::string BaseValueType(const std::string &type_name);
bool ValueTypesAgree(const std::string &authored, const std::string &expected);
void SplitScenePathString(const std::string &path, std::string *prim_part,
                          std::string *prop_part);
bool IsValidAbsolutePrimPathString(const std::string &path, std::string *err);
bool IsValidAbsolutePrimTargetPathString(const std::string &path,
                                         std::string *err);
bool IsValidScenePathString(const std::string &path, std::string *err);
bool IsValidConnectionTargetPathString(const std::string &path,
                                       std::string *err);
const PropSlot *GetSlot(const PrimSpec &ps, const std::string &name);
bool HasAttributeProp(const PrimSpec &ps, const std::string &name);
bool HasConnections(const PrimSpec &ps, const std::string &name);
const std::vector<Path> &RelTargets(const PrimSpec &ps,
                                    const std::string &name);
std::string AttrTypeNameOf(const PrimSpec &ps, const std::string &name);
bool ValueToDouble(const Value &v, double *out);
bool ValueToInt64(const Value &v, int64_t *out);
bool IsFloatTriple(TypeId id);
bool IsDoubleTriple(TypeId id);
bool ValueToQuat(const Value &v, std::array<double, 4> *out);
bool ValueToFloat2(const Value &v, std::array<float, 2> *out);
bool GetStringProperty(const PrimSpec &ps, const std::string &name,
                       std::string *out);
bool GetStringLikeProperty(const PrimSpec &ps, const std::string &name,
                           std::string *out);
bool GetAssetPathProperty(const PrimSpec &ps, const std::string &name,
                          std::string *out);
bool GetIntegerScalarProperty(const PrimSpec &ps, const std::string &name,
                              int64_t *out);
bool GetIntArrayProperty(const PrimSpec &ps, const std::string &name,
                         std::vector<int32_t> *out);
bool ValueToDoubleScalarArray(const Value &v, std::vector<double> *out);
bool GetTokenArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<std::string> *out);
bool GetValueArrayLength(const Value &v, size_t *size);
bool GetArrayLengthProperty(const PrimSpec &ps, const std::string &name,
                            size_t *size);
bool GetExtentProperty(const PrimSpec &ps, const std::string &name,
                       std::vector<std::array<float, 3>> *out);
const std::vector<std::pair<double, uint32_t>> *GetTimeSamplesOf(
    const PrimSpec &ps, const std::string &name);
void ValidateTimeSamplesProp(const PrimSpec &ps, const std::string &prop_name,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result);
bool IsLengthCompatible(size_t count, size_t expected, bool allow_singleton);
void ValidateArrayLengthAtAllSamples(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     size_t expected,
                                     const std::string &expected_name,
                                     USDValidationResult *result);
bool IsPrimvarPropertyName(const std::string &prop_name);
bool IsValidPrimvarInterpolation(const std::string &interp);
bool GetRelationshipTargetCount(const PrimSpec &ps,
                                const std::string &rel_name, size_t *count);
void ValidateArrayLengthProperty(const PrimSpec &ps,
                                 const std::string &prop_name, size_t expected,
                                 bool allow_singleton,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const std::string &expected_name,
                                 USDValidationResult *result);
const std::string *FindPrimType(const PrimTypeByPath &prim_types,
                                const std::string &prim_path);
const PrimSpecifier *FindPrimSpecifier(
    const PrimSpecifierByPath &prim_specifiers, const std::string &prim_path);
void ValidateRelationshipScenePaths(const PrimSpec &ps,
                                    const std::string &rel_name,
                                    const std::string &rule_id,
                                    const std::string &location,
                                    USDValidationResult *result);
void ValidateConnectionTargets(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &location,
                               const PrimTypeByPath &prim_types,
                               USDValidationResult *result,
                               bool require_output_target);
void ValidateGeomCommonProperties(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result);
void ValidateGeomPrimitive(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result);
void ValidateGeomPoints(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result);
void ValidateGeomCurves(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result);
void ValidateVolume(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result);
void ValidateFieldAsset(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result);
void ValidateParticleField(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result);
void ValidateOpenUsd2608GeomAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result);
void ValidateRenderPrim(const PrimSpec &ps, const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result);
void ValidatePointInstancer(const PrimSpec &ps,
                            const std::string &prim_location,
                            USDValidationResult *result);
void ValidateCamera(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result);
void ValidateMeshTopology(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result,
                          size_t *out_face_count);
void ValidateGeomSubsetTopology(const PrimSpec &ps,
                                const std::string &prim_location,
                                const AncestorContext &ancestors,
                                USDValidationResult *result);
void ValidatePrimvars(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result);
void ValidateSkeletonTopology(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result,
                              size_t *out_joint_count);
void ValidateSkelAnimation(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result);
void ValidateBlendShape(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result);
void ValidateSkelBinding(const PrimSpec &ps,
                         const std::vector<AppliedSchema> &applied_schemas,
                         const std::string &prim_location,
                         const PrimTypeByPath &prim_types,
                         const AncestorContext &ancestors,
                         USDValidationResult *result);
void ValidateSkinningPrimvars(const PrimSpec &ps,
                              const std::string &prim_location,
                              const PrimTypeByPath &prim_types,
                              const SkeletonJointCountByPath &skeleton_joints,
                              USDValidationResult *result);
std::string GetShaderInfoId(const PrimSpec &ps);
void ValidateUsdPreviewSurface(const PrimSpec &ps,
                               const std::string &prim_location,
                               USDValidationResult *result);
bool IsUsdUVTextureWrapToken(const std::string &token);
bool IsUsdUVTextureColorSpaceToken(const std::string &token);
void ValidateUsdUVTexture(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result);
void ValidateUsdPrimvarReader(const PrimSpec &ps, const std::string &shader_id,
                              const std::string &prim_location,
                              USDValidationResult *result);
void ValidateMaterialXConfig(const PrimSpec &ps,
                             const std::vector<AppliedSchema> &applied_schemas,
                             const std::string &prim_location,
                             USDValidationResult *result);
void ValidateMaterialXShader(const PrimSpec &ps, const std::string &shader_id,
                             const std::string &prim_location,
                             USDValidationResult *result);
void ValidateMaterialXSynthesizedMaterial(
    const PrimSpec &ps, const std::string &prim_location,
    const PrimTypeByPath &prim_types, USDValidationResult *result);
void ValidateLuxRelationships(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result);
bool IsPhysicsDofName(const std::string &dof);
bool HasAppliedPhysicsSchema(const std::vector<AppliedSchema> &schemas);
bool HasSchemaOrPropertyPrefix(const PrimSpec &ps,
                               const std::vector<AppliedSchema> &schemas,
                               const std::string &schema_prefix,
                               const std::string &prop_prefix);
void ValidatePhysicsRelationship(const PrimSpec &ps,
                                 const std::string &prop_name,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const PrimTypeByPath &prim_types,
                                 const std::set<std::string> &target_types,
                                 USDValidationResult *result);
void ValidatePhysicsScene(const PrimSpec &ps,
                          const std::string &prim_location,
                          USDValidationResult *result);
void ValidatePhysicsInertiaAndMotion(const PrimSpec &ps,
                                     const std::string &prim_location,
                                     USDValidationResult *result);
void ValidatePhysicsJointTransforms(const PrimSpec &ps,
                                    const std::string &prim_location,
                                    USDValidationResult *result);
void ValidatePhysicsScalarProperties(const PrimSpec &ps,
                                     const std::string &prim_location,
                                     USDValidationResult *result);
void ValidatePhysicsCollisionGroup(const PrimSpec &ps,
                                   const std::string &prim_location,
                                   const PrimTypeByPath &prim_types,
                                   USDValidationResult *result);
void ValidatePhysicsApproximation(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result);
void ValidatePhysicsJointLimits(const PrimSpec &ps,
                                const std::string &prim_location,
                                USDValidationResult *result);
void ValidatePhysicsSchemaPlacement(
    const PrimSpec &ps, const std::vector<AppliedSchema> &schemas,
    const std::string &prim_location, USDValidationResult *result);
void ValidatePhysicsDriveAndLimitAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &schemas,
    const std::string &prim_location, USDValidationResult *result);
void ValidateMjcTokenEnums(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result);
void ValidateMjcPhysics(const PrimSpec &ps,
                        const std::vector<AppliedSchema> &schemas,
                        const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result);
void ValidateNewtonPhysics(const PrimSpec &ps,
                           const std::vector<AppliedSchema> &schemas,
                           const std::string &prim_location,
                           const PrimTypeByPath &prim_types,
                           USDValidationResult *result);
void ValidatePreliminaryPhysics(const PrimSpec &ps,
                                const std::vector<AppliedSchema> &schemas,
                                const std::string &prim_location,
                                const PrimTypeByPath &prim_types,
                                USDValidationResult *result);
bool GetMetaDouble(const Dict &dict, const std::string &key, double *out);
bool GetMetaAssetPath(const Dict &dict, const std::string &key,
                      std::string *out);
bool GetMetaString(const Dict &dict, const std::string &key,
                   std::string *out);
bool GetMetaStringArray(const Dict &dict, const std::string &key,
                        std::vector<std::string> *out);
bool GetMetaDouble2Array(const Dict &dict, const std::string &key,
                         std::vector<std::array<double, 2>> *out);
void ValidateDictionaryKeys(const Dict &dict, const std::string &rule_id,
                            const std::string &location,
                            USDValidationResult *result);
void ValidateDictValue(const Value &v, const std::string &rule_id,
                       const std::string &location,
                       USDValidationResult *result);
bool HasCompositionArc(const PrimSpec &ps);
bool IsOverride(const PrimSpec &ps);
std::vector<std::string> GatherArcStrings(const std::vector<std::string> &inline_items,
                                          const ArcEdit *edit);
bool ParseFiniteLayerOffsetNumber(const char *begin, const char *end,
                                  double *out);
bool ParseRawLayerOffset(const std::string &text, double *offset,
                         double *scale);
void ValidateLayerOffset(double offset, double scale,
                         const std::string &rule_id,
                         const std::string &location,
                         USDValidationResult *result);
void ValidateRefArcString(const std::string &arc_str, bool is_payload,
                          const std::string &rule_id,
                          const std::string &prim_location,
                          USDValidationResult *result);
void ValidatePathArcString(const std::string &arc_str,
                           const std::string &rule_id,
                           const std::string &prim_location,
                           const PrimSpecifierByPath &prim_specifiers,
                           USDValidationResult *result,
                           bool expect_class_target);
void ValidateVariantSetsAndSelections(const PrimSpec &ps,
                                      const std::string &prim_location,
                                      USDValidationResult *result);
void ValidateRelocates(const PrimSpec &ps, const std::string &prim_location,
                       USDValidationResult *result);
void ValidateCompositionMetadata(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 const PrimSpecifierByPath &prim_specifiers,
                                 USDValidationResult *result);
std::vector<AppliedSchema> CollectAppliedSchemas(const PrimSpec &ps);
std::set<std::string> CollectAppliedCollectionInstances(
    const std::vector<AppliedSchema> &schemas);
void ValidateAPISchemasMetadata(const std::vector<AppliedSchema> &schemas,
                                const std::string &prim_location,
                                USDValidationResult *result);
void ValidatePrimMetadata(const PrimSpec &ps, const std::string &prim_location,
                          const std::vector<AppliedSchema> &applied_schemas,
                          USDValidationResult *result);
void ValidateMaterialXReferenceConventions(const PrimSpec &ps,
                                           const std::string &prim_location,
                                           USDValidationResult *result);
void ValidateClipPairArray(const std::vector<std::array<double, 2>> &pairs,
                           const std::string &rule_id,
                           const std::string &location,
                           const std::string &field_name,
                           USDValidationResult *result);
void ValidateClipSetMetadata(const std::string &clip_set_name,
                             const Dict &clip_set,
                             const std::string &prim_location,
                             USDValidationResult *result);
void ValidateClipsMetadata(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result);
void ValidateTokenAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result);
void ValidateExactAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &expected,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result);
void ValidateUniformAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result);
void ValidateVaryingAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result);
void ValidateAttributeMetadata(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &location,
                               USDValidationResult *result);
void ValidateRelationshipMetadata(const PrimSpec &ps,
                                  const std::string &rel_name,
                                  const std::string &location,
                                  USDValidationResult *result);
bool IsCanonicalColorSpaceToken(const std::string &token);
bool IsColorSpaceDefinitionPropertyName(const std::string &prop_name);
bool ParseColorSpaceDefinitionPropertyName(const std::string &prop_name,
                                           std::string *instance,
                                           std::string *field);
ColorSpaceSet CollectLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas);
bool IsKnownColorSpaceToken(const std::string &token,
                            const ColorSpaceSet &visible_custom_spaces);
void ValidateLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &location, USDValidationResult *result);
void ValidateColorSpaceToken(const std::string &token,
                             const ColorSpaceSet &visible_custom_spaces,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result);
void ValidateColorSpaceDefinitionProperty(const PrimSpec &ps,
                                          const std::string &prop_name,
                                          const std::string &location,
                                          USDValidationResult *result);
void ValidateCollectionProperty(const PrimSpec &ps,
                                const std::vector<std::string> &segments,
                                const std::string &prop_name,
                                const std::set<std::string> &collection_instances,
                                const std::string &location,
                                USDValidationResult *result);
void ValidateLayerMetas(const Layer &layer, USDValidationResult *result);
void BuildPrimIndex(const Layer &layer, uint32_t prim_index,
                    const std::string &prim_path,
                    PrimTypeByPath *prim_types,
                    PrimSpecifierByPath *prim_specifiers,
                    SkeletonJointCountByPath *skeleton_joints,
                    std::unordered_set<uint32_t> *visited);
bool IsArkitAllowedPrimTypeName(const std::string &type_name);
std::string LowerAssetExtension(const std::string &asset_path);
bool IsArkitTextureExtension(const std::string &ext);
bool IsDecodableButNonPortableTextureExtension(const std::string &ext);
bool IsEightBitTextureExtension(const std::string &ext);
bool IsArkitLayerExtension(const std::string &ext);
bool IsArkitShaderId(const std::string &shader_id);
bool IsBuiltinRegistryShaderId(const std::string &shader_id);
bool ValueToFloat4(const Value &v, std::array<double, 4> *out);
bool GetFloat4Property(const PrimSpec &ps, const std::string &name,
                       std::array<double, 4> *out);
bool Float4Equals(const std::array<double, 4> &v, double x, double y, double z,
                  double w);
std::string FormatFloat4(const std::array<double, 4> &v);
void ValidateArkitTextureFormats(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 USDValidationResult *result);
void ValidateNormalMapTextureImpl(const Layer &layer, const PrimSpec &surface,
                                  const std::string &surface_location,
                                  const char *rule_id,
                                  USDValidationSeverity severity,
                                  USDValidationResult *result);
void ValidateArkitShader(const Layer &layer, const PrimSpec &ps,
                         const std::string &prim_location,
                         USDValidationResult *result);
void ValidateArkitMaterialBinding(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  const PrimTypeByPath &prim_types,
                                  bool layer_self_contained,
                                  USDValidationResult *result);
void ValidateArkitCompositionAssetPaths(const PrimSpec &ps,
                                        const std::string &prim_location,
                                        USDValidationResult *result);
void ValidateArkitPrim(const Layer &layer, const PrimSpec &ps,
                       const std::string &prim_location,
                       const PrimTypeByPath &prim_types,
                       bool layer_self_contained,
                       USDValidationResult *result);
void ValidateArkitLayerMetas(const Layer &layer, USDValidationResult *result);
bool IsSelfContainedLayer(const Layer &layer);
bool ValidateOnePrimSpec(const Layer &layer, uint32_t prim_index,
                         const std::string &prim_location,
                         const ColorSpaceSet &inherited_color_spaces,
                         const AncestorContext &ancestors,
                         const ValidationOptions &options,
                         const PrimTypeByPath &prim_types,
                         const PrimSpecifierByPath &prim_specifiers,
                         const SkeletonJointCountByPath &skeleton_joints,
                         bool layer_self_contained,
                         std::unordered_set<uint32_t> *visited,
                         USDValidationResult *result,
                         PrimChildValidationState *child_state);
void ValidatePrimSpecs(const Layer &layer, uint32_t prim_index,
                       const std::string &prim_location,
                       const ColorSpaceSet &inherited_color_spaces,
                       const AncestorContext &ancestors,
                       const ValidationOptions &options,
                       const PrimTypeByPath &prim_types,
                       const PrimSpecifierByPath &prim_specifiers,
                       const SkeletonJointCountByPath &skeleton_joints,
                       bool layer_self_contained,
                       std::unordered_set<uint32_t> *visited,
                       USDValidationResult *result);

}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
