// SPDX-License-Identifier: Apache-2.0
// Internal declarations shared by the MJCF/URDF frontend sources
// (mjcf*.cc, urdf.cc). Not part of the importer API (import.hh).
#pragma once

#include <algorithm>
#include <deque>
#include <atomic>
#include <charconv>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include "external/filesystem/include/ghc/filesystem.hpp"
#include "external/pugixml.hpp"
#include "convex-hull.hh"
#include "import.hh"
#include "volumetric.hh"

namespace fs = ghc::filesystem;

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
// Per-geom mass properties in the owning body's frame, gathered while a body's
// geoms are converted when MuJoCo would infer the body inertia from them.
struct GeomMass {
  double mass = 0;
  std::array<double, 3> com{{0, 0, 0}};
  double inertia[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};  // about com
};

constexpr double kPi = 3.14159265358979323846;

struct MeshAsset {
  fs::path path;
  std::array<double, 3> scale{{1.0, 1.0, 1.0}};
  std::array<double, 3> refpos{{0.0, 0.0, 0.0}};  // <mesh refpos="...">
  std::array<double, 4> refquat{
      {1.0, 0.0, 0.0, 0.0}};  // <mesh refquat="..."> wxyz
  bool has_refpos = false;
  bool has_refquat = false;
  bool smoothnormal = false;  // <mesh smoothnormal>; MuJoCo default false
  std::string inertia = "legacy";  // <mesh inertia>: legacy|exact|convex|shell
  int maxhullvert = -1;            // <mesh maxhullvert>; -1 = no limit
};


struct UnusedMeshData {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float>
      uvs;  // 2 per vertex (st), parallel to positions; empty if none
  std::vector<int32_t> indices;
};


// <asset><hfield>: a row-major nrow x ncol grid of elevations normalized to
// [0,1], plus size = (radius_x, radius_y, elevation_z, base_z).
struct HFieldAsset {
  int nrow{0};
  int ncol{0};
  std::array<double, 4> size{{1.0, 1.0, 1.0, 0.1}};
  std::vector<float> data;  // nrow*ncol, normalized [0,1], row 0 = -y edge
};


struct MeshPayload {
  std::string name;
  std::vector<double> matrix;
  Mesh mesh;
  Json shape;
};


struct Stats {
  size_t links{0};
  size_t joints{0};
  size_t visuals{0};
  size_t collisions{0};
  size_t actuators{0};
  size_t tendons{0};
  size_t equalities{0};
  size_t contact_excludes{0};
  size_t sites{0};
  size_t keyframes{0};
  size_t materials{0};
  size_t sensors{0};
  size_t contact_pairs{0};
};


// Resolved attribute map (attr name -> value) for MuJoCo <default> classes.
using AttrMap = std::map<std::string, std::string>;


struct Defaults {
  std::map<std::string, AttrMap> geom;   // class name -> merged geom attrs
  std::map<std::string, AttrMap> joint;  // class name -> merged joint attrs
  AttrMap root_geom;                     // unnamed (top-level) default
  AttrMap root_joint;
  // MuJoCo keeps one actuator default per class: every actuator element in
  // a <default> merges into it, and a shortcut (<muscle>, <position>, ...)
  // also sets its kind, recorded as "__type".
  std::map<std::string, AttrMap> actuator;
  AttrMap root_actuator;
  // The slot's type is the last actuator element written into it (or an
  // ancestor class); its parent class for MuJoCo's shortcut source search.
  std::map<std::string, std::string> actuator_type, actuator_parent;
  std::string root_actuator_type;
};


struct Context {
  // <compiler angle="...">: degrees by default; radians otherwise.
  double angle_to_rad = 0.017453292519943295;  // pi/180
  std::string eulerseq = "xyz";                // <compiler eulerseq="...">
  bool autolimits = true;                      // <compiler autolimits="...">
  std::string inertiafromgeom = "auto";        // <compiler inertiafromgeom>
  int inertia_group_min = 0, inertia_group_max = 5;  // inertiagrouprange
  Defaults defaults;
  // Geoms named by <spatial><geom geom=...>: wrap surfaces must stay native
  // Sphere/Cylinder prims that tendons can reference.
  std::set<std::string> wrap_geoms;
};


// Nested <include> files expand one call per file (each its own document).
constexpr size_t kMaxIncludeNesting = 64;


// Quaternions are stored MuJoCo-style: {w, x, y, z}.
using Quat = std::array<double, 4>;


// Polygon faces -> triangles as MuJoCo's OBJ decoder makes them
// (ClipPolygon), keeping per-corner normals/UVs and per-face materials.
// MuJoCo 3.16's decoder (the PyPI build) also drops every polygon of more
// than 100 vertices, keeping the other faces; so do we, with a warning.
constexpr int32_t kMuJoCoMaxObjPolygon = 100;

// Meshes MuJoCo generates from an SDF plugin (<mesh><plugin instance>), by
// the synthetic path their MeshAsset carries.
constexpr char kPluginMeshPrefix[] = "mjcf-plugin-mesh:";

// Marks a <geom type="sdf"> after it is rewritten to a mesh geom.
constexpr char kSdfGeomMarker[] = "lightusd-sdf";


// Mesh assets parsed ahead of use by worker threads with private readers.
// Only a load whose side effects were reads, budget charges, warnings and
// OBJ materials is kept; they are replayed into the import's reader at first
// use (materials renumbered after the import's own), together with its
// warnings, so traces, diagnostics, budgets and outputs match a serial
// import. Anything else (textures, failures) is parsed again serially there.
struct PreloadedMesh {
  Mesh mesh;
  OBJ obj;               // URDF OBJ/DAE parse (all parts, own materials)
  std::string resolved;  // URDF: the mesh's resolved identifier
  Json materials = Json::array();  // OBJ/MTL materials, numbered from 0
  std::vector<Reader::Access> accesses;
  std::vector<Reader::Warning> warnings;
  size_t bytes = 0, peak = 0;
};


using PreloadKey = std::pair<std::string, bool>;

using PreloadJob = std::function<bool(PreloadedMesh*, std::string*)>;


// A GMSH file as a <mesh> asset (MuJoCo 3.16's GMSH decoder): a 2D file's
// triangles, or the boundary of a 3D file's tetrahedra (each tet oriented
// positively; faces that occur once; boundary vertices in node order).
// Binary mesh formats copy floats verbatim; reject NaN/Inf before they reach
// normals, mass properties or ordered vertex maps.
template <typename T>
bool AllFinite(const std::vector<T>& values) {
  for (const T v : values)
    if (!std::isfinite(v)) return false;
  return true;
}


// A primitive geom with a mesh= attribute is fitted to the mesh by MuJoCo's
// compiler (mjCMesh::FitGeom): sized from the mesh's equivalent inertia box
// (or its AABB with <compiler fitaabb>) and placed in the mesh's COM /
// principal-axes frame. Returns the geom-local frame and fitted sizes.
struct MeshFit {
  std::array<double, 3> size{{0, 0, 0}};
  std::vector<double> frame;  // relative to the geom's own pos/orientation
};


// MJCF <keyframe><key qpos/qvel/act/ctrl/mpos/mquat> -> JSON keyframe entries
// consumed by the converter's AddMjcKeyframeFromJson -> MjcKeyframe prim.
// MuJoCo's binary SKN skin file (mjCSkin::LoadSKN): int nvert, ntexcoord,
// nface, nbone; float vert[3n], texcoord[2n]; int face[3n]; then per bone a
// 40-byte body name, float bindpos[3], bindquat[4], int count, int
// vertid[count], float vertweight[count].
struct MjcfSkinData {
  std::vector<float> vert, texcoord;
  std::vector<int32_t> face;
  std::vector<std::string> body;
  std::vector<float> bindpos, bindquat;
  std::vector<std::vector<int32_t>> vertid;
  std::vector<std::vector<float>> vertweight;
};


// A body still to visit, with what its parent hands down.
struct MujocoBodyJob {
  pugi::xml_node node;
  std::string parent_name, childclass;
  std::vector<double> parent_world;
};


// MJCF macro expansion (<attach>, cable composites, <replicate>) shares one
// element budget per import, and every added element is charged as memory
// (a pugixml node plus a few attributes), so a small file cannot expand
// without bound.
constexpr size_t kExpandedElementBytes = 160;


// MJCF <composite type="cable"> expanded into the body chain MuJoCo's
// mjCComposite::MakeCable builds: count-1 bodies along the curve (or the
// explicit vertices), each a capsule/cylinder/box segment on a ball joint (a
// free or no joint for the first per `initial`) in a parallel-transported
// frame, boundary sites S_first/S_last and contact excludes between
// neighbours. Names follow MuJoCo (prefix + B_first, B_<i>, B_last, J_*, G<i>).
// A cable nests one body per segment; bound the chain so body traversal
// stays stack-safe (the XML depth limit is re-checked after expansion).
constexpr int kMaxCableVertices = 512;

// Import-wide context, set by the entry points (defined in mjcf.cc).
extern thread_local Reader* active_reader;
extern thread_local Json* active_materials;
extern thread_local std::string active_world_name;
extern thread_local std::map<const void*, std::string> active_body_names;
extern thread_local std::vector<GeomMass>* active_geom_masses;
extern thread_local std::set<std::string>* stacked_joint_bodies;
extern thread_local std::map<std::pair<std::string, bool>, Mesh>* active_mesh_cache;
extern thread_local std::map<std::string, Mesh>* active_plugin_meshes;
extern thread_local std::map<std::pair<std::string, bool>, PreloadedMesh>* active_mesh_preload;
extern thread_local std::map<std::string, MeshFit>* active_mesh_fits;
extern thread_local bool active_fitaabb;
extern thread_local std::set<const void*>* active_flex_bodies;

std::string ToLower(std::string s);
bool ParseInt(const std::string& text, int* out);
int32_t ToInt32(double value, const char* what);
bool ReadFile(const fs::path& filename, std::string* text, std::string* err);
bool WriteFile(const fs::path& filename, const std::vector<uint8_t>& bytes,
               std::string* err);
std::vector<double> ParseDoubles(const std::string& text);
std::array<double, 3> ParseDouble3(const std::string& text,
                                   std::array<double, 3> fallback);
double ParseDoubleAttr(const pugi::xml_node& node, const std::string& name,
                       double fallback);
bool HasAttr(const pugi::xml_node& node, const std::string& name);
std::string Attr(const pugi::xml_node& node, const std::string& name,
                 const std::string& fallback = std::string());
std::vector<pugi::xml_node> Children(const pugi::xml_node& node,
                                     const std::string& name);
pugi::xml_node Child(const pugi::xml_node& node, const std::string& name);
std::string StripMujocoRoot(const std::string& xml);
std::string AttributeFromTag(const std::string& tag, const std::string& name);
bool ValidateXMLDepth(pugi::xml_node root, size_t limit, const std::string& source,
                       std::string* err);
bool UnsupportedXML(pugi::xml_node node, const std::string& message,
    const char* code, const char* feature, const std::string& source,
    bool approximated = false);
bool ExpandIncludes(const std::string& xml, const fs::path& base_dir,
                    std::set<fs::path>* seen, std::string* expanded,
                    std::string* err, const std::string& source);
Quat QuatNormalize(const Quat& q);
Quat QuatMul(const Quat& a, const Quat& b);
Quat QuatFromAxisAngle(const std::array<double, 3>& axis, double angle);
Quat QuatFromTwoVecs(const std::array<double, 3>& from,
                     const std::array<double, 3>& to);
Quat QuatFromBasis(const std::array<double, 3>& cx,
                   const std::array<double, 3>& cy,
                   const std::array<double, 3>& cz);
Quat OrientationQuat(const std::string& quat_str,
                     const std::string& axisangle_str,
                     const std::string& euler_str,
                     const std::string& xyaxes_str,
                     const std::string& zaxis_str, const Context& ctx);
std::vector<double> MatrixFromPosQuat(const std::array<double, 3>& pos,
                                      const Quat& q);
std::vector<double> PoseMatrix(const pugi::xml_node& node, const Context& ctx);
std::vector<double> MultiplyMatrix(const std::vector<double>& a,
                                   const std::vector<double>& b);
std::vector<double> IdentityMatrix();
std::vector<double> ScaleMatrix(const std::array<double, 3>& scale);
std::vector<double> TranslationMatrix(const std::array<double, 3>& t);
std::vector<double> MeshRefMatrix(const MeshAsset& asset);
void AppendTri(Mesh* mesh, const std::array<float, 3>& a,
               const std::array<float, 3>& b, const std::array<float, 3>& c);
Mesh MakeBoxMesh(const std::array<double, 3>& half);
Mesh MakeSphereMesh(double radius, int slices = 16, int stacks = 8);
Mesh MakeCapsuleMesh(double radius, double half_length, int slices = 16, int stacks = 8);
Mesh MakeCylinderMesh(double radius, double half_length, int slices = 20);
float ReadFloatLE(const uint8_t* bytes, size_t offset);
void ComputeMujocoVertexNormals(Mesh* mesh, bool smoothnormal);
bool LoadASCIISTL(std::string_view text, const fs::path& filename, bool smoothnormal,
                  Mesh* mesh, std::string* err);
bool LoadBinarySTL(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                   std::string* err);
bool ClipPolygon(const std::vector<float>& pos, const int32_t* poly, size_t n,
                 std::vector<int32_t>* tri, size_t* work = nullptr, bool* exhausted = nullptr);
bool TriangulateLikeMuJoCo(Mesh* mesh);
bool LoadOBJ(const fs::path& filename, bool smoothnormal, Mesh* mesh, std::string* err);
void PreloadJobs(const std::vector<std::pair<PreloadKey, PreloadJob>>& jobs);
void PreloadMeshFiles(const std::vector<PreloadKey>& keys);
bool TakePreloaded(const PreloadKey& key, PreloadedMesh* out, std::string* err,
                   bool keep = false);
bool TakePreloadedMesh(const PreloadKey& key, Mesh* mesh, std::string* err);
const std::string& VolumetricDevice();
bool MjcfSdfLevelSet(const std::string& mesh_name, const Mesh& mesh, Json* out,
                     std::string* why);
bool LoadMeshFile(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                  std::string* err);
bool LoadGMSHMesh(const fs::path& filename, Mesh* mesh, std::string* err);
bool LoadLegacyMSH(const fs::path& filename, Mesh* mesh, std::string* err);
bool LoadLevelSetVDB(const fs::path& filename, LevelSet* out, std::string* err);
bool LoadVDBMesh(const fs::path& filename, Mesh* mesh, std::string* err);
bool LoadMeshFileUncached(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                          std::string* err);
Json MeshPayloadToJson(const MeshPayload& payload);
std::string Eff(const pugi::xml_node& node, const AttrMap& cls,
                const std::string& name, const std::string& fallback = "");
bool HasEff(const pugi::xml_node& node, const AttrMap& cls,
            const std::string& name);
double EffDouble(const pugi::xml_node& node, const AttrMap& cls,
                 const std::string& name, double fallback);
bool IsMjcfActuatorTag(const std::string& tag);
bool IsMjcfShortcutParam(const std::string& key);
void WalkDefault(const pugi::xml_node& top, Defaults* d);
Defaults ParseDefaults(const pugi::xml_node& root);
const AttrMap& ResolveGeomClass(const pugi::xml_node& geom_node,
                                const Defaults& defaults,
                                const std::string& childclass);
const AttrMap& ResolveJointClass(const pugi::xml_node& joint_node,
                                 const Defaults& defaults,
                                 const std::string& childclass);
void AddGeomPhysicsAttrs(const pugi::xml_node& geom_node, const AttrMap& cls,
                         Json* geom_json);
std::map<std::string, fs::path> BuildMeshIndex(const fs::path&);
fs::path ResolveMeshPath(const fs::path& base_dir, const fs::path& mesh_dir,
                         const std::string& file,
                         const std::map<std::string, fs::path>& index);
std::map<std::string, MeshAsset> CollectMujocoAssets(const pugi::xml_node& root,
                                                     const fs::path& base_dir);
void NormalizeHField(std::vector<float>* data);
std::map<std::string, HFieldAsset> CollectMujocoHFields(
    const pugi::xml_node& root, const fs::path& base_dir);
Mesh TessellateHField(const HFieldAsset& hf);
void EigenDescending(const double in[3][3], double eval[3], double evec[3][3]);
bool MeshTriangles(const Mesh& mesh, std::vector<int32_t>* tri);
bool FitMeshGeom(const pugi::xml_node& geom_node, const AttrMap& cls,
                 const std::map<std::string, MeshAsset>& assets,
                 const std::string& type, MeshFit* fit, std::string* err);
void ApplyFromtoFrame(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::vector<double>& body_world, size_t axis,
                      std::array<double, 3>* size, std::vector<double>* matrix);
bool BuildGeomPayload(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::string& cls_name,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world,
                      MeshPayload* payload, bool* is_visual, std::string* err,
                      bool force_collision = false, bool native_shape = false);
std::string AxisToken(const std::array<double, 3>& axis);
Json InertialToJson(const pugi::xml_node& body_node, const Context& ictx);
bool IsSupportedFlexcompType(const std::string& type);
fs::path FlexMeshDir(const pugi::xml_node& root, const fs::path& base_dir);
void MakeFlexBox(const std::string& type, int dim, const std::array<int, 3>& count,
                 const std::array<double, 3>& spacing, std::vector<double>* point,
                 std::vector<int>* element);
bool LoadFlexMesh(const fs::path& path, const std::string& text, int dim,
                  const std::array<double, 3>& origin, bool has_origin,
                  std::vector<double>* point, std::vector<int>* element, std::string* err,
                  bool* stuffed = nullptr);
bool ParseGMSHBinary(const std::string& text, long version, int* dim, std::vector<double>* point,
                     std::vector<int>* element, std::string* err);
bool ParseGMSH(const std::string& text, int* dim, std::vector<double>* point,
               std::vector<int>* element, std::string* err);
void ReadFlexProperties(const pugi::xml_node& f, Json* out);
bool CollectBodyFlexcompsJson(const pugi::xml_node& body_node,
                              const std::vector<double>& body_world,
                              const Context& ctx, bool is_world,
                              const fs::path& mesh_dir, Json* out,
                              std::string* err);
bool CollectMujocoFlexcompsJson(const pugi::xml_node& worldbody, const Context& ctx,
                                const fs::path& mesh_dir, Json* out, std::string* err);
bool CollectMujocoRawFlexesJson(const pugi::xml_node& root,
                                const std::vector<pugi::xml_node>& worldbodies,
                                const Context& ctx, Json* out, std::set<const void*>* consumed,
                                std::string* err);
void AddJointFromNode(const pugi::xml_node& joint_node, const AttrMap& jcls,
                      const Context& ctx, const std::string& parent_name,
                      const std::string& child_name,
                      const std::vector<double>& origin_matrix,
                      const std::array<double, 3>& origin_pos, Json* joints);
bool ExpandMjcfActuator(const pugi::xml_node& node, const std::string& type,
                        Json* a);
AttrMap MjcfShortcutSource(const Defaults& d, std::string cls, const std::string& kind);
void AddMujocoActuatorsJson(const pugi::xml_node& root, Json* actuators,
                            Json* mjc_actuators, Stats* stats);
void AddMujocoTendonsJson(const pugi::xml_node& root, const Context& ctx,
                          Json* tendons, Stats* stats);
void CollectMujocoSitesJson(const pugi::xml_node& top,
                            const std::vector<double>& top_parent_world,
                            const Context& ctx, Json* sites, Stats* stats);
void AddMujocoSensorsJson(const pugi::xml_node& root, Json* sensors,
                          Stats* stats);
void AddMujocoCustomJson(const pugi::xml_node& root, Json* custom);
void AddMujocoPluginsJson(const pugi::xml_node& root, Json* plugins);
void CollectMujocoLightsCamerasJson(const pugi::xml_node& top,
                                    const std::vector<double>& top_world,
                                    const Context& ctx, Json* lights,
                                    Json* cameras);
void AddMujocoEqualityJson(const pugi::xml_node& root, Json* equalities,
                           Stats* stats);
void AddMujocoContactExcludesJson(const pugi::xml_node& root,
                                  Json* filtered_pairs, Stats* stats);
void AddMujocoContactPairsJson(const pugi::xml_node& root, Json* pairs,
                               Stats* stats);
bool MjcBoolAttr(const std::string& v);
Json BuildMjcSceneJson(const pugi::xml_node& root);
bool GenerateBuiltinTexture(const pugi::xml_node& node, const std::string& name, bool skybox,
                            std::string* key, std::string* why);
void AddMujocoMaterialsJson(const pugi::xml_node& root,
                            const fs::path& base_dir, Json* materials,
                            Json* skybox, Stats* stats);
bool ParseSKN(const std::vector<uint8_t>& bytes, MjcfSkinData* skin, std::string* err);
bool AddMujocoSkinsJson(const pugi::xml_node& root, const fs::path& base_dir, Json* skins,
                        std::string* err);
void AddMujocoKeyframesJson(const pugi::xml_node& root, Json* keyframes,
                            Stats* stats);
void FrameRotation(const std::vector<double>& m, double r[3][3]);
void RotateInertia(const double r[3][3], const double local[3], double out[3][3]);
bool PrimitiveMassProperties(const std::string& type, const std::array<double, 3>& size,
                             bool shell, double* volume, double unit[3]);
bool MeshMassProperties(const std::vector<std::array<double, 3>>& v,
                        const std::vector<int32_t>& tri, const std::string& mode,
                        double* volume, std::array<double, 3>* com, double inert[3][3]);
bool GeomMassProperties(const pugi::xml_node& geom_node, const AttrMap& cls,
                        const Context& ctx, const std::map<std::string, MeshAsset>& assets,
                        const MeshPayload* mesh_payload, GeomMass* out);
Json CombineGeomMasses(const std::vector<GeomMass>& geoms);
bool AppendGeomToLink(const pugi::xml_node& geom_node, const std::string& cc,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world, Json* link,
                      Stats* stats, std::string* err,
                      bool dual_collider = false);
bool AddWorldbodyGeomsLink(const std::vector<pugi::xml_node>& worldbodies,
                           const std::map<std::string, MeshAsset>& assets,
                           const std::map<std::string, HFieldAsset>& hfields,
                           const Options& opts, const Context& ctx, Json* links,
                           Stats* stats, std::string* err);
bool VisitMujocoBodyOne(const pugi::xml_node& body_node,
                        const std::string& parent_name,
                        const std::string& childclass,
                        const std::vector<double>& parent_world,
                        const std::map<std::string, MeshAsset>& assets,
                        const std::map<std::string, HFieldAsset>& hfields,
                        const Options& opts, const Context& ctx, Json* links,
                        Json* joints, Stats* stats, std::vector<MujocoBodyJob>* children,
                        std::string* err);
bool VisitMujocoBody(const pugi::xml_node& body_node,
                     const std::string& parent_name,
                     const std::string& childclass,
                     const std::vector<double>& parent_world,
                     const std::map<std::string, MeshAsset>& assets,
                     const std::map<std::string, HFieldAsset>& hfields,
                     const Options& opts, const Context& ctx, Json* links,
                     Json* joints, Stats* stats, std::string* err);
const std::set<std::string>& AttachRefAttrs();
void PrefixAttachNode(pugi::xml_node node, const std::string& prefix,
                      double angle_scale);
void PrefixAttachSubtree(pugi::xml_node node, const std::string& prefix,
                         double angle_scale);
void CollectAttachNodes(pugi::xml_node n, std::vector<pugi::xml_node>* out);
pugi::xml_node FindBodyByName(pugi::xml_node n, const std::string& name);
bool ChargeExpansion(size_t elements, const char* what, std::string* err);
size_t CountElements(pugi::xml_node node);
bool ExpandAttachments(pugi::xml_node root, const fs::path& base_dir,
                       std::string* err);
void SetNodeAttr(pugi::xml_node n, const char* name, const char* val);
void DecomposePosQuat(const std::vector<double>& m, std::array<double, 3>* pos,
                      Quat* q);
bool ExpandCableComposites(pugi::xml_node root, const Context& ctx, std::string* err);
bool ExpandReplicates(pugi::xml_node root, const Context& ctx, std::string* err);
void DissolveFramesUnder(pugi::xml_node parent, const Context& ctx);
void FlattenFramesIn(pugi::xml_node parent, const Context& ctx);
bool IsSolverArray(std::string_view name);
std::string CompleteSolverArray(const std::string& name, const std::string& value,
                                const std::string& inherited, const std::string& kind);
void InlineMJCFDefaults(pugi::xml_node root);
bool BuildMujocoPayload(const std::string& xml, const fs::path& input_filename,
                        const Options& opts, Json* payload, Stats* stats,
                        std::string* err);
std::vector<double> URDFPose(const pugi::xml_node& origin);
bool LoadURDFObj(const std::string& file, const std::string& source, OBJ* obj,
                 std::string* resolved, std::string* err);
PreloadKey URDFObjKey(const std::string& file, const std::string& source);
bool ResolveURDFMesh(const std::string& file, const std::string& source, std::string* resolved,
                     std::string* err);
bool URDFGeometry(const pugi::xml_node& node, const std::string& source,
                  Json* geometries, std::string* err);
std::vector<double> InverseRigidMatrix(const std::vector<double>& m);
size_t MergeDummyInertiaLinks(Json* links, Json* joints,
                              const std::map<std::string, std::string>& parent,
                              const std::map<std::string, std::vector<double>>& origins);
size_t CollapseGhostLinks(Json* links, Json* joints, const std::map<std::string, std::string>& parent,
                          const std::map<std::string, std::vector<double>>& worlds);
Json URDFCustomElement(const pugi::xml_node& node, size_t depth);
Json URDFCustomJson(const pugi::xml_node& node, std::initializer_list<const char*> known_children,
                    std::initializer_list<const char*> known_attributes);
void DiscoverURDFPackages(const pugi::xml_node& root, const std::string& source);
bool BuildURDF(const pugi::xml_node& root, const std::string& source,
               const Options& opts, Json* payload, std::string* err);
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
