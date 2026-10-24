// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Private URDF/MJCF authoring helpers. Not installed.
#pragma once
#include "urdf-to-usd.hh"
#include "tydra/urdf-payload.hh"
#include "next/types/type-id.hh"
#include <array>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace lightusd { namespace next { class Layer; class PrimSpec; class Value; } }

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {
using Json = ::lightusd::tydra::detail::JSONValue;
namespace tn = ::lightusd::next;
using NameMap = std::map<std::string, std::string>;
using Matrix4 = std::array<double, 16>;  // row-major, translation in row 3

// Output profile. `legacy` reproduces the historical converter output byte for
// byte (the JSON bridge default). `newton` matches Newton 1.8 /
// newton-usd-schemas 0.6 / mujoco-usd-converter 0.8 consumers and is the native
// importer default. `openusd` authors only standard UsdPhysics/UsdGeom/UsdShade
// data: engine extensions (mjc:*, newton:*, physx*) are omitted and reported.
struct Profile {
  const char *name = "legacy";
  bool legacy = true;   // historical names, variability and aliases
  bool newton = false;  // Newton 1.8 parity APIs (NewtonJointAPI, ...)
  bool engine = true;   // author MuJoCo/Newton/PhysX extension data at all
};
bool ParseProfile(const std::string &name, Profile *out);
const Profile &ActiveProfile();
class ProfileScope {
 public:
  explicit ProfileScope(const Profile &profile);
  ~ProfileScope();
  ProfileScope(const ProfileScope &) = delete;
  ProfileScope &operator=(const ProfileScope &) = delete;
 private:
  const Profile *previous_;
};
// Generated UsdPhysics/MuJoCo/Newton descriptors (doc/generated/*.json).
struct SchemaProperty {
  std::string type;
  bool uniform = false;
  bool relationship = false;
};
const SchemaProperty *FindSchemaProperty(const std::string &name);
bool IsEngineName(const std::string &name);
// Deduplicated MuJoCo friction triplets as bound UsdShade physics materials
// (non-legacy profiles). Active for the duration of one conversion.
class PhysicsMaterials {
 public:
  explicit PhysicsMaterials(tn::Layer *layer);
  ~PhysicsMaterials();
  PhysicsMaterials(const PhysicsMaterials &) = delete;
  PhysicsMaterials &operator=(const PhysicsMaterials &) = delete;
  // Friction triplet plus geom solref (contact stiffness/damping).
  std::string Bind(tn::PrimSpec *collider, std::vector<double> friction,
                   const std::vector<double> &solref = {});
 private:
  tn::Layer *layer_;
  PhysicsMaterials *previous_;
  std::map<std::string, std::string> by_key_;
  bool defined_ = false;
};
PhysicsMaterials *ActivePhysicsMaterials();

// MuJoCo contype/conaffinity bitmasks as UsdPhysics collision groups (the
// openusd profile, which has no mjc:* attributes): one group per distinct
// (contype, conaffinity) pair, each filtering the groups it cannot touch
// under MuJoCo's rule (ct_a & ca_b) || (ct_b & ca_a).
class CollisionFilterGroups {
 public:
  CollisionFilterGroups();
  ~CollisionFilterGroups();
  CollisionFilterGroups(const CollisionFilterGroups &) = delete;
  CollisionFilterGroups &operator=(const CollisionFilterGroups &) = delete;
  void Record(const std::string &collider, int32_t contype, int32_t conaffinity);
  // Writes /World/CollisionGroups; nothing when every pair collides.
  size_t Write(tn::Layer *layer) const;
 private:
  CollisionFilterGroups *previous_;
  std::map<std::pair<int32_t, int32_t>, std::vector<std::string>> members_;
};
CollisionFilterGroups *ActiveCollisionFilterGroups();
bool IsEngineSchema(const std::string &schema);

// Omit empty scopes and define each populated scope at its first child.
class Scope {
 public:
  Scope(tn::Layer* layer, const std::string& name);
  tn::PrimSpec* Add(const std::string& name, const std::string& type);
 private:
  tn::Layer* layer_;
  std::string path_;
  bool defined_{false};
};

std::string JsonString(const Json &j, const char *key,
                       const std::string &fallback = std::string());
bool ReadNumber(const Json &j, const char *key, double *out);
bool ReadNumber(const Json &j, const char *key, float *out);
bool ReadBool(const Json &j, const char *key, bool *out);
const Json *JsonObject(const Json &j, const char *key);
const Json *JsonArray(const Json &j, const char *key);
int32_t JsonInt(const Json &j, const char *key, int32_t fallback);
bool ReadIntFrom(const Json &src, const char *object_key, const char *key,
                 int32_t *out);
int32_t JsonIntFrom(const Json &src, const char *object_key, const char *key,
                    int32_t fallback);
bool ReadNumberFrom(const Json &src, const char *object_key, const char *key,
                    double *out);
double JsonNumberFrom(const Json &src, const char *object_key, const char *key,
                      double fallback);
std::vector<float> JsonFloats(const Json &j, const char *key);
std::vector<double> JsonDoubles(const Json &j, const char *key);
std::vector<double> JsonDoublesFrom(const Json &src, const char *object_key,
                                    const char *key);
std::vector<int32_t> JsonInts(const Json &j, const char *key);
std::vector<std::string> JsonStrings(const Json &j, const char *key);
bool ReadRange(const Json &j, const char *key, double *lo, double *hi);
std::string Sanitize(const std::string &source, const std::string &fallback);
std::string Unique(const std::string &source, const std::string &fallback,
                   std::set<std::string> *used);
std::string ChildName(const std::string &name, std::set<std::string> *siblings);
// Non-legacy profiles keep a source name that sanitizing or uniquifying
// changed as the prim's displayName (as mujoco-usd-converter does).
void SetSourceDisplayName(tn::PrimSpec *prim, const std::string &source);
// URDF elements/attributes without a USD mapping (`urdfCustom` payload
// members) -> urdf:<attribute> strings, urdf:text and child Scopes.
void AddURDFCustom(tn::Layer *layer, const std::string &path, tn::PrimSpec *prim,
                   const Json *custom, std::set<std::string> *siblings);
void AppendWarn(std::string *warn, const std::string &message);
void AddAPI(tn::PrimSpec *prim, const std::string &schema);
void AddAPIs(tn::PrimSpec *prim,
             std::initializer_list<const char *> schemas);
void Set(tn::PrimSpec *prim, const std::string &name, tn::Value value,
         const std::string &type_name, bool uniform = false);
void SetToken(tn::PrimSpec *prim, const std::string &name,
              const std::string &value, bool uniform = false);
void SetDoubles(tn::PrimSpec *prim, const std::string &name,
                std::vector<double> values, bool uniform = false);
void Declare(tn::PrimSpec *prim, const std::string &name, tn::TypeId type_id,
             const std::string &type_name);
void Connect(tn::PrimSpec *prim, const std::string &name, tn::TypeId type_id,
             const std::string &type_name, const std::string &target);
void SetRel(tn::PrimSpec *prim, const std::string &name,
            const std::vector<std::string> &targets);
void SetInterpolation(tn::PrimSpec *prim, const std::string &name,
                      const char *interpolation);
tn::PrimSpec *Define(tn::Layer *layer, const std::string &path,
                     const std::string &type);
Matrix4 IdentityMatrix();
Matrix4 MatrixFromArray(const std::vector<double> &flat);
void SetTransform(tn::PrimSpec *prim, const Matrix4 &m);
void SetTransformFromJson(tn::PrimSpec *prim, const Json &source);
std::string AxisFromToken(const std::string &axis);
void AddCollisionAPIs(tn::PrimSpec *prim, bool mesh_collision,
                      const Json &src, bool mjcf_source);
bool AddMeshFromJson(tn::Layer *layer, const std::string &link_path,
                     std::set<std::string> *siblings, const Json &mesh_json,
                     const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
                     const std::string &fallback_name, bool collision,
                     bool mjcf_source, std::string *warn, std::string *err);
bool AddNativeCollisionShapeFromJson(tn::Layer *layer,
                                     const std::string &link_path,
                                     std::set<std::string> *siblings,
                                     const Json &shape_json,
                                     const std::string &fallback_name,
                                     bool mjcf_source, std::string *warn,
                                     std::string *err, bool collision = true,
                                     std::string *out_name = nullptr);
void JacobiEigenSymmetric3(const double in[3][3], double eval[3],
                           double evec[3][3]);
tn::Value RotationMatrixToQuatf(double r[3][3]);
void AuthorInertia(tn::PrimSpec *prim, const Json &inertial);
std::string AxisToken(const Json &joint);
std::string JointDofName(const std::string &axis, bool rotational);
void SetPoint3(tn::PrimSpec *prim, const std::string &name,
               const std::vector<float> &v, bool uniform);
void AssignJointBase(tn::PrimSpec *prim, const Json &joint,
                     const std::string &parent, const std::string &child,
                     const NameMap &joint_names);
void ApplyMjcSceneOptions(tn::PrimSpec *scene, const Json &root);
void AddPhysicsScene(tn::Layer *layer, const Json &root);
std::vector<std::string> NewtonActuatorSchemas(const Json &act);
void AddNewtonActuatorFromJson(Scope *scope, std::set<std::string> *siblings,
                               const Json &act, const NameMap &joint_names,
                               size_t index, std::string *warn);
void AddMjcSiteFromJson(Scope *scope, const Json &site,
                        const std::string &name);
void AuthorMjcSite(tn::PrimSpec *prim, const Json &site, const Matrix4 *local);
Matrix4 InverseRigid(const Matrix4 &m);
Matrix4 MultiplyRowMajor(const Matrix4 &a, const Matrix4 &b);
// MJCF flexcomp soft bodies (payload "deformables") as UsdPhysics
// deformable prims under /World/Deformables.
void AddDeformablesFromJson(tn::Layer *layer, const Json &items,
                            const NameMap &link_names, std::string *warn);
void AddMjcTendonFromJson(Scope *scope, std::set<std::string> *siblings,
                          const Json &t, const NameMap &joint_names,
                          const NameMap &site_names, const NameMap &geom_names,
                          NameMap *tendon_names, size_t index,
                          std::string *warn);
// Non-legacy MJCF equalities: connect -> PhysicsSphericalJoint, weld ->
// PhysicsFixedJoint (both excluded from articulation), joint -> NewtonMimicAPI
// + MjcEqualityJointAPI on the follower joint.
void AddEqualityConstraint(tn::Layer *layer, Scope *scope,
                           std::set<std::string> *siblings, const Json &e,
                           size_t index, const NameMap &link_names,
                           const std::map<std::string, Matrix4> &link_worlds,
                           const std::set<std::string> &static_links,
                           const NameMap &joint_names, const Json &sites,
                           std::string *warn);
void AddMjcEqualityFromJson(Scope *scope, std::set<std::string> *siblings,
                            const Json &e, const NameMap &link_names,
                            const NameMap &joint_names, size_t index);
void AddMjcActuatorFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &a, const NameMap &joint_names,
                            const NameMap &tendon_names,
                            const NameMap &site_names,
                            const NameMap &link_names, size_t index,
                            std::string *warn);
void AddMjcKeyframeFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &k, size_t index);
void AddMjcSensorFromJson(Scope *scope, std::set<std::string> *used,
                          const Json &s, size_t index);
void AddContactPairFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &p, size_t index);
Matrix4 LightTransformMatrix(const Matrix4 &world,
                             const std::array<double, 3> &local_dir);
void AddLightFromJson(Scope *scope, std::set<std::string> *used,
                      const Json &l, size_t index);
// MJCF skybox -> DomeLight with a latitude-longitude texture (+Y pole,
// rotated onto +Z for Z-up stages).
void AddSkyboxFromJson(Scope *scope, std::set<std::string> *used, const Json &skybox,
                       bool z_up);
void AddCameraFromJson(Scope *scope, std::set<std::string> *used,
                       const Json &c, size_t index);
void AddMaterialFromJson(Scope *scope, std::set<std::string> *used,
                         const Json &m, size_t index);
}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
