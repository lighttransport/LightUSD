// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
//
// Regression test for the next-core URDF/MJCF JSON -> USD converter
// (src/tydra/next/urdf-to-usd.cc). The payload exercises every builder
// ported from the legacy converter (links/inertia, meshes, native collision
// shapes, joints + limits/frames/mimic, physics scene options, Newton
// actuators, sites, tendons, equalities, MjcActuators, keyframes, sensors,
// contact pairs, lights, cameras, materials, custom and plugin scopes) and
// asserts the authored names, values, variability and relationships that the
// legacy stage authors for the same payload.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "tydra/next/urdf-to-usd.hh"

using namespace lightusd::next;

namespace {

const char *kPayload = R"JSON({
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "gravity": [0, 0, -1],
  "timestep": 0.004,
  "newton": {"selfCollisionEnabled": false, "maxSolverIterations": 20},
  "mjcScene": {
    "option": {"impratio": 10, "iterations": 50, "integrator": "implicitfast",
               "wind": [1, 2, 3], "o_solref": [0.02, 1]},
    "flag": {"eulerdamp": false},
    "compiler": {"autolimits": true, "angle": "radian",
                 "inertiagrouprange_max": 3}
  },
  "links": [
    {
      "name": "base-link",
      "floating": true,
      "inertial": {"mass": 2.5, "centerOfMass": [0.1, 0.2, 0.3],
                   "fullInertia": [2, 3, 4, 0, 0, 0]},
      "visuals": [
        {"name": "geom", "material": "body-mat", "group": 1,
         "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0.5,1],
         "geometry": {"positions": [0,0,0, 1,0,0, 0,1,0, 1,1,0],
                      "normals": [0,0,1, 0,0,1, 0,0,1, 0,0,1],
                      "uvs": [0,0, 1,0, 0,1, 1,1],
                      "indices": [0,1,2, 2,1,3]}},
        {"name": "broken", "geometry": {"positions": [0, 0, 0]}}
      ],
      "collisions": [
        {"name": "geom", "shape": {"type": "box"},
         "mjc": {"group": 3, "condim": 1, "contype": 2, "conaffinity": 4,
                 "margin": 0.01, "friction": [1, 0.005, 0.0001]}},
        {"name": "geom", "shape": {"type": "sphere", "radius": 0.25}},
        {"name": "tube", "shape": {"type": "cylinder", "radius": 0.1,
                                   "height": 0.3, "axis": "x"}},
        {"name": "pill", "shape": {"type": "capsule", "radius": 0.05,
                                   "height": 0.2}},
        {"name": "floor", "shape": {"type": "plane", "width": 4,
                                    "length": 5}},
        {"name": "hull", "approximation": "convexDecomposition",
         "geometry": {"positions": [0,0,0, 1,0,0, 0,1,0]}}
      ]
    },
    {"name": "arm", "inertial": {"mass": 1, "diagonalInertia": [1, 2, 3]}},
    {"name": "slider"},
    {"name": "ball"},
    {"name": "wheel"},
    {"name": "tip"}
  ],
  "filteredPairs": [{"body1": "base-link", "body2": "arm"},
                    {"body1": "base-link", "body2": "tip"}],
  "joints": [
    {"name": "hinge", "type": "revolute", "parent": "base-link",
     "child": "arm", "axis": [0, 1, 0], "localPos0": [0, 0, 0.5],
     "localRot0": [0.7071068, 0, 0, 0.7071068],
     "limit": {"lower": -1, "upper": 1}, "initPosition": 0.5,
     "dynamics": {"damping": 0.5, "friction": 0.2, "stiffness": 3,
                  "armature": 0.02}},
    {"name": "slide", "type": "prismatic", "parent": "arm",
     "child": "slider", "axis": [0, 0, 1],
     "limit": {"lower": -0.1, "upper": 0.2},
     "mimic": {"joint": "hinge", "offset": 0.1, "multiplier": 2}},
    {"name": "socket", "type": "spherical", "parent": "arm",
     "child": "ball", "originMatrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 1,2,3,1]},
    {"name": "spin", "type": "continuous", "parent": "arm", "child": "wheel",
     "axis": [1, 0, 0], "limit": {"lower": -1, "upper": 1}},
    {"name": "weld", "type": "fixed", "parent": "arm", "child": "tip"},
    {"name": "orphan", "type": "revolute", "parent": "arm", "child": "nope"}
  ],
  "actuators": [
    {"name": "drive", "joint": "hinge", "kp": 100, "kd": 5, "maxEffort": 20},
    {"name": "drive", "targets": ["slide", "/World/Joints/spin"],
     "control": "pid", "ki": 1, "delaySteps": 2},
    {"name": "dangling", "joint": "missing"}
  ],
  "sites": [
    {"name": "s0", "size": 0.01, "group": 4,
     "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0.1,0,0,1]},
    {"name": "s1"},
    {"name": "s1"}
  ],
  "tendons": [
    {"name": "fixed-t", "type": "fixed",
     "joints": [{"joint": "hinge", "coef": 0.5}, {"joint": "slide"}],
     "range": [0, 1], "stiffness": 7, "rgba": [1, 0, 0, 1]},
    {"name": "cable", "type": "spatial",
     "path": [{"site": "s0"}, {"sidesite": "s1"}],
     "width": 0.002},
    {"name": "lost", "joints": [{"joint": "missing"}]}
  ],
  "equalities": [
    {"name": "couple", "type": "joint", "joint1": "hinge", "joint2": "slide",
     "polycoef": [0, 1, 0, 0, 0], "solref": [0.01, 1]},
    {"name": "couple", "type": "connect", "body1": "arm", "body2": "tip",
     "anchor": [0, 0, 1]},
    {"name": "glue", "type": "weld", "body1": "arm", "torquescale": 0.5}
  ],
  "mjcActuators": [
    {"name": "muscle", "targetTendon": "cable", "gainPrm": [1, 2],
     "lengthRange": [0.1, 0.2], "ctrlRange": [0, 1], "dynType": "muscle",
     "refSite": "s0"},
    {"name": "motor", "targetJoint": "hinge", "gear": [2], "ctrlRange_max": 5,
     "group": 1},
    {"name": "thrust", "targetSite": "s1"}
  ],
  "keyframes": [{"name": "home", "qpos": [0, 1], "ctrl": [0.5]}, {"qvel": [1]}],
  "sensors": [
    {"name": "gyro", "type": "gyro", "site": "s0"},
    {"name": "frame", "type": "framepos", "objtype": "body", "objname": "arm",
     "refsite": "s1", "noise": 0.1, "user": [1, 2]}
  ],
  "contactPairs": [{"geom1": "a", "geom2": "b", "condim": 3,
                    "friction": [1, 1], "margin": 0.01}],
  "lights": [
    {"name": "spot", "type": "spot", "cutoff": 30, "color": [1, 0.5, 0.25],
     "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,2,1]},
    {"name": "sun", "type": "directional", "castshadow": false}
  ],
  "cameras": [{"name": "track", "fovy": 90,
               "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,1,1]},
              {"name": "ortho", "orthographic": true}],
  "materials": [
    {"name": "body-mat", "rgba": [0.2, 0.4, 0.6, 0.5], "roughness": 0.3,
     "emission": 2},
    {"name": "tex", "rgba": [1, 1, 1, 1], "texture": "assets/tex.png"},
    {"name": "tex"}
  ],
  "custom": {"numeric": [{"name": "max_contact_points", "data": [4]}],
             "text": [{"name": "note", "data": "hello"}]},
  "plugins": [{"instance": "pid", "plugin": "mujoco.pid",
               "config": {"kp": "10"}}]
})JSON";

const Layer *g_layer = nullptr;

const PrimSpec &Prim(const std::string &path) {
  const PrimSpec *prim = g_layer->prim_at_path(path);
  if (!prim) std::cerr << "missing prim " << path << "\n";
  assert(prim);
  return *prim;
}

bool HasPrim(const std::string &path) {
  return g_layer->prim_at_path(path) != nullptr;
}

const Value &Val(const PrimSpec &prim, const std::string &name) {
  const Value *value = prim.property_value(name);
  if (!value) std::cerr << "missing property " << prim.path().str() << "." << name << "\n";
  assert(value);
  return *value;
}

bool Uniform(const PrimSpec &prim, const std::string &name) {
  const PropSlot *slot = prim.property(name);
  assert(slot);
  return slot->is_uniform();
}

std::string TypeName(const PrimSpec &prim, const std::string &name) {
  const std::string *type_name = prim.property_type_name(name);
  assert(type_name);
  return *type_name;
}

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

double D(const PrimSpec &prim, const std::string &name) {
  const Value &v = Val(prim, name);
  if (const double *d = v.as_double()) return *d;
  if (const float *f = v.as_float()) return *f;
  if (const int32_t *i = v.as_int()) return *i;
  assert(false && "not a scalar number");
  return 0.0;
}

std::string Tok(const PrimSpec &prim, const std::string &name) {
  const std::string *t = Val(prim, name).as_token();
  assert(t);
  return *t;
}

std::vector<std::string> Targets(const PrimSpec &prim, const std::string &name) {
  std::vector<std::string> out;
  const std::vector<Path> *targets = prim.relationship(name);
  if (!targets) return out;
  for (const Path &p : *targets) out.push_back(p.str());
  return out;
}

std::vector<std::string> APIs(const PrimSpec &prim) {
  return prim.meta().apiSchemas();
}

std::vector<std::string> Children(const std::string &path) {
  std::vector<std::string> out;
  for (const PrimSpec *child : g_layer->children(g_layer->index_at_path(path))) {
    out.push_back(child->name());
  }
  return out;
}

std::vector<double> Doubles(const PrimSpec &prim, const std::string &name) {
  const std::vector<double> *values = Val(prim, name).as_double_array();
  assert(values);
  return *values;
}

using Strings = std::vector<std::string>;

void TestScopesAndScene() {
  // Scope order and omission of empty scopes follow the legacy stage.
  assert(Children("/World") ==
         Strings({"PhysicsScene", "Links", "Joints", "Actuators", "Tendons",
                  "Equalities", "Sites", "MjcActuators", "Keyframes",
                  "Lights", "Cameras", "Materials", "Sensors", "Contacts",
                  "MjcCustom", "MjcPlugins"}));
  const PrimSpec &scene = Prim("/World/PhysicsScene");
  assert(APIs(scene) == Strings({"MjcSceneAPI", "NewtonSceneAPI"}));
  assert(TypeName(scene, "physics:gravityDirection") == "vector3f");
  assert(Uniform(scene, "physics:gravityDirection"));
  assert(Near(D(scene, "mjc:option:timestep"), 0.004));
  assert(Near(D(scene, "mjc:timestep"), 0.004));  // next-only flat name
  assert(Near(D(scene, "mjc:option:impratio"), 10));
  assert(*Val(scene, "mjc:option:iterations").as_int() == 50);
  assert(Tok(scene, "mjc:option:integrator") == "implicitfast");
  assert(TypeName(scene, "mjc:option:wind") == "double3");
  assert(Doubles(scene, "mjc:option:o_solref") == std::vector<double>({0.02, 1}));
  assert(*Val(scene, "mjc:flag:eulerdamp").as_bool() == false);
  assert(*Val(scene, "mjc:compiler:autoLimits").as_bool() == true);
  assert(Tok(scene, "mjc:compiler:angle") == "radian");
  assert(*Val(scene, "mjc:compiler:inertiaGroupRange:max").as_int() == 3);
  assert(*Val(scene, "newton:timeStepsPerSecond").as_int() == 250);
  assert(*Val(scene, "newton:maxSolverIterations").as_int() == 20);
  assert(*Val(scene, "newton:gravityEnabled").as_bool() == true);
  assert(Uniform(scene, "newton:timeStepsPerSecond"));
}

void TestLinks() {
  assert(Children("/World/Links") ==
         Strings({"base_link", "arm", "slider", "ball", "wheel", "tip"}));
  const PrimSpec &base = Prim("/World/Links/base_link");
  assert(APIs(base) ==
         Strings({"PhysicsRigidBodyAPI", "PhysicsMassAPI",
                  "PhysicsArticulationRootAPI", "NewtonArticulationRootAPI",
                  "PhysicsFilteredPairsAPI"}));
  assert(*Val(base, "physics:rigidBodyEnabled").as_bool());
  assert(*Val(base, "newton:selfCollisionEnabled").as_bool() == false);
  assert(*Val(base, "mjc:freeJoint").as_bool() && Uniform(base, "mjc:freeJoint"));
  assert(Near(D(base, "physics:mass"), 2.5));
  assert(TypeName(base, "physics:centerOfMass") == "point3f");
  // fullInertia is diagonalized; an already-diagonal tensor keeps its
  // moments and an identity principal-axes rotation.
  const float *moments = Val(base, "physics:diagonalInertia").as_float3();
  assert(moments && Near(moments[0], 2) && Near(moments[1], 3) &&
         Near(moments[2], 4));
  const float *axes = Val(base, "physics:principalAxes").as_float4();
  assert(axes && Near(axes[0], 1) && Near(axes[1], 0));  // real first
  assert(Targets(base, "physics:filteredPairs") ==
         Strings({"/World/Links/arm", "/World/Links/tip"}));
  const PrimSpec &arm = Prim("/World/Links/arm");
  assert(APIs(arm) == Strings({"PhysicsRigidBodyAPI", "PhysicsMassAPI"}));
  assert(!arm.property_value("physics:principalAxes"));

  // Visuals and collisions share the child namespace; duplicates get the
  // legacy "1" suffix and skipped geometry leaves no prim.
  assert(Children("/World/Links/base_link") ==
         Strings({"geom", "geom1", "geom11", "tube", "pill", "floor", "hull"}));
  const PrimSpec &mesh = Prim("/World/Links/base_link/geom");
  assert(mesh.type_name() == "Mesh");
  assert(APIs(mesh) == Strings({"MjcImageableAPI", "MaterialBindingAPI"}));
  assert(Targets(mesh, "material:binding") ==
         Strings({"/World/Materials/body_mat"}));
  assert(*Val(mesh, "mjc:group").as_int() == 1 && Uniform(mesh, "mjc:group"));
  assert(Tok(mesh, "subdivisionScheme") == "none");
  // Non-identity indices expand normals/UVs to faceVarying.
  assert(Val(mesh, "normals").array_size() == 6);
  assert(Val(mesh, "primvars:st").array_size() == 6);
  assert(mesh.property_meta("primvars:st")->interpolation == "faceVarying");
  assert(TypeName(mesh, "primvars:st") == "texCoord2f[]");
  const double *xf = Val(mesh, "xformOp:transform").as_matrix4d();
  assert(xf && Near(xf[14], 0.5));
  assert(!HasPrim("/World/Links/base_link/broken"));

  const PrimSpec &box = Prim("/World/Links/base_link/geom1");
  assert(box.type_name() == "Cube");
  assert(APIs(box) == Strings({"PhysicsCollisionAPI", "MjcCollisionAPI",
                               "MjcImageableAPI", "NewtonCollisionAPI"}));
  assert(Near(D(box, "size"), 2.0));
  assert(*Val(box, "physics:collisionEnabled").as_bool());
  assert(Tok(box, "purpose") == "guide" && Uniform(box, "purpose"));
  assert(*Val(box, "mjc:group").as_int() == 3);
  assert(*Val(box, "mjc:condim").as_int() == 1);
  assert(*Val(box, "mjc:geomContype").as_int() == 2);
  assert(*Val(box, "mjc:geomConaffinity").as_int() == 4);
  assert(*Val(box, "mjc:contype").as_int() == 2);       // next-only
  assert(*Val(box, "mjc:conaffinity").as_int() == 4);   // next-only
  assert(Near(D(box, "mjc:margin"), 0.01));
  assert(Near(D(box, "newton:contactMargin"), 0.01));
  assert(Doubles(box, "mjc:geomFriction") ==
         std::vector<double>({1, 0.005, 0.0001}));
  const PrimSpec &sphere = Prim("/World/Links/base_link/geom11");
  assert(sphere.type_name() == "Sphere" && Near(D(sphere, "radius"), 0.25));
  // MJCF colliders without authored groups keep mjc:group unauthored.
  assert(!sphere.property_value("mjc:group"));
  const PrimSpec &tube = Prim("/World/Links/base_link/tube");
  assert(tube.type_name() == "Cylinder" && Tok(tube, "axis") == "X");
  assert(Near(D(tube, "height"), 0.3) && Uniform(tube, "axis"));
  const PrimSpec &pill = Prim("/World/Links/base_link/pill");
  assert(pill.type_name() == "Capsule" && Tok(pill, "axis") == "Z");
  const PrimSpec &floor = Prim("/World/Links/base_link/floor");
  assert(floor.type_name() == "Plane" && Near(D(floor, "length"), 5));
  const PrimSpec &hull = Prim("/World/Links/base_link/hull");
  assert(APIs(hull) ==
         Strings({"PhysicsCollisionAPI", "MjcCollisionAPI", "MjcImageableAPI",
                  "NewtonCollisionAPI", "PhysicsMeshCollisionAPI",
                  "MjcMeshCollisionAPI", "NewtonMeshCollisionAPI"}));
  assert(Tok(hull, "physics:approximation") == "convexDecomposition");
  assert(Tok(hull, "mjc:inertia") == "legacy");
  assert(*Val(hull, "newton:maxHullVertices").as_int() == 64);
}

void TestJoints() {
  // The joint with an unknown child link is skipped.
  assert(Children("/World/Joints") ==
         Strings({"hinge", "slide", "socket", "spin", "weld"}));
  const PrimSpec &hinge = Prim("/World/Joints/hinge");
  assert(hinge.type_name() == "PhysicsRevoluteJoint");
  assert(APIs(hinge) == Strings({"MjcJointAPI", "PhysicsLimitAPI:rotY"}));
  assert(Targets(hinge, "physics:body0") == Strings({"/World/Links/base_link"}));
  assert(Targets(hinge, "physics:body1") == Strings({"/World/Links/arm"}));
  assert(Tok(hinge, "physics:axis") == "Y" && Uniform(hinge, "physics:axis"));
  const float *pos0 = Val(hinge, "physics:localPos0").as_float3();
  assert(pos0 && Near(pos0[2], 0.5) && Uniform(hinge, "physics:localPos0"));
  const float *rot0 = Val(hinge, "physics:localRot0").as_float4();
  assert(rot0 && Near(rot0[0], 0.7071068) && Near(rot0[3], 0.7071068));
  const float *rot1 = Val(hinge, "physics:localRot1").as_float4();
  assert(rot1 && Near(rot1[0], 1) && Near(rot1[1], 0));
  assert(*Val(hinge, "physics:jointEnabled").as_bool());
  assert(*Val(hinge, "physics:collisionEnabled").as_bool() == false);
  const double deg = 57.2957795130823208768;
  assert(Near(D(hinge, "physics:lowerLimit"), -deg, 1e-4));
  assert(Uniform(hinge, "physics:lowerLimit"));
  assert(Near(D(hinge, "physics:limit:rotY:low"), -deg, 1e-4));
  assert(Near(D(hinge, "physics:limit:rotY:high"), deg, 1e-4));
  assert(!Uniform(hinge, "physics:limit:rotY:high"));
  // damping/frictionloss are MjcJointAPI (uniform) attributes; the PhysX
  // mirrors use the angular namespace for revolute joints.
  assert(Near(D(hinge, "mjc:damping"), 0.5) && Uniform(hinge, "mjc:damping"));
  assert(Near(D(hinge, "mjc:frictionloss"), 0.2));
  assert(Near(D(hinge, "physxJoint:jointFriction"), 0.2));
  assert(Near(D(hinge, "physxLimit:angular:damping"), 0.5));
  assert(Near(D(hinge, "physxLimit:angular:stiffness"), 3));
  assert(Near(D(hinge, "mjc:armature"), 0.02) && !Uniform(hinge, "mjc:armature"));
  assert(Near(D(hinge, "physxJoint:armature"), 0.02));
  assert(Near(D(hinge, "state:angular:physics:position"), 0.5 * deg, 1e-4));

  const PrimSpec &slide = Prim("/World/Joints/slide");
  assert(slide.type_name() == "PhysicsPrismaticJoint");
  assert(APIs(slide) ==
         Strings({"MjcJointAPI", "NewtonMimicAPI", "PhysicsLimitAPI:transZ"}));
  assert(Near(D(slide, "physics:limit:transZ:high"), 0.2));
  assert(Targets(slide, "newton:mimicJoint") == Strings({"/World/Joints/hinge"}));
  assert(Near(D(slide, "newton:mimicCoef0"), 0.1));
  assert(Near(D(slide, "newton:mimicCoef1"), 2));
  assert(*Val(slide, "newton:mimicEnabled").as_bool());

  const PrimSpec &socket = Prim("/World/Joints/socket");
  assert(socket.type_name() == "PhysicsSphericalJoint");
  // localPos0 falls back to the originMatrix translation.
  const float *spos = Val(socket, "physics:localPos0").as_float3();
  assert(spos && Near(spos[0], 1) && Near(spos[1], 2) && Near(spos[2], 3));

  // Continuous joints are unlimited revolute joints.
  const PrimSpec &spin = Prim("/World/Joints/spin");
  assert(spin.type_name() == "PhysicsRevoluteJoint");
  assert(APIs(spin) == Strings({"MjcJointAPI"}));
  assert(!spin.property_value("physics:lowerLimit"));

  const PrimSpec &weld = Prim("/World/Joints/weld");
  assert(weld.type_name() == "PhysicsFixedJoint");
  assert(!weld.property_value("physics:axis"));
}

void TestActuatorsSitesTendons() {
  assert(Children("/World/Actuators") == Strings({"drive", "drive1"}));
  const PrimSpec &pd = Prim("/World/Actuators/drive");
  assert(pd.type_name() == "NewtonActuator");
  assert(APIs(pd) == Strings({"NewtonPDControlAPI", "NewtonMaxEffortClampingAPI"}));
  assert(Targets(pd, "newton:targets") == Strings({"/World/Joints/hinge"}));
  assert(Near(D(pd, "newton:kp"), 100) && Uniform(pd, "newton:kp"));
  assert(Near(D(pd, "newton:maxEffort"), 20));
  const PrimSpec &pid = Prim("/World/Actuators/drive1");
  assert(APIs(pid) == Strings({"NewtonPIDControlAPI", "NewtonActuatorDelayAPI"}));
  assert(Targets(pid, "newton:targets") ==
         Strings({"/World/Joints/slide", "/World/Joints/spin"}));
  assert(*Val(pid, "newton:delaySteps").as_int() == 2);

  assert(Children("/World/Sites") == Strings({"s0", "s1", "s1_1"}));
  const PrimSpec &s0 = Prim("/World/Sites/s0");
  assert(s0.type_name() == "Sphere");
  assert(APIs(s0) == Strings({"MjcSiteAPI"}));
  assert(Near(D(s0, "radius"), 0.01));
  assert(*Val(s0, "mjc:group").as_int() == 4);
  assert(Tok(s0, "purpose") == "guide");
  assert(Near(Val(s0, "xformOp:transform").as_matrix4d()[12], 0.1));
  const PrimSpec &s1 = Prim("/World/Sites/s1");
  assert(Near(D(s1, "radius"), 0.005) && !s1.property_value("xformOp:transform"));

  // The tendon without any exported joint is skipped.
  assert(Children("/World/Tendons") == Strings({"fixed_t", "cable"}));
  const PrimSpec &fixed = Prim("/World/Tendons/fixed_t");
  assert(fixed.type_name() == "MjcTendon");
  assert(Tok(fixed, "mjc:type") == "fixed" && Uniform(fixed, "mjc:type"));
  assert(Targets(fixed, "mjc:path") ==
         Strings({"/World/Joints/hinge", "/World/Joints/slide"}));
  assert(Doubles(fixed, "mjc:path:coef") == std::vector<double>({0.5, 1.0}));
  assert(Near(D(fixed, "mjc:range:max"), 1));
  assert(Tok(fixed, "mjc:limited") == "true");
  assert(TypeName(fixed, "mjc:rgba") == "color4f");
  const PrimSpec &cable = Prim("/World/Tendons/cable");
  assert(Tok(cable, "mjc:type") == "spatial");
  // Duplicate site source names resolve to the last site (legacy map).
  assert(Targets(cable, "mjc:path") ==
         Strings({"/World/Sites/s0", "/World/Sites/s1_1"}));
  assert(Doubles(cable, "mjc:path:coef") == std::vector<double>({1, 1}));
  assert(Near(D(cable, "mjc:width"), 0.002));
}

void TestEqualitiesAndMjcActuators() {
  assert(Children("/World/Equalities") == Strings({"couple", "couple1", "glue"}));
  const PrimSpec &joint_eq = Prim("/World/Equalities/couple");
  assert(joint_eq.type_name() == "Xform");
  assert(APIs(joint_eq) == Strings({"MjcEqualityJointAPI"}));
  assert(Targets(joint_eq, "mjc:target") ==
         Strings({"/World/Joints/hinge", "/World/Joints/slide"}));
  assert(Near(D(joint_eq, "mjc:coef1"), 1) && !Uniform(joint_eq, "mjc:coef1"));
  assert(Doubles(joint_eq, "mjc:solref") == std::vector<double>({0.01, 1}));
  const PrimSpec &connect = Prim("/World/Equalities/couple1");
  assert(APIs(connect) == Strings({"MjcEqualityConnectAPI"}));
  assert(Targets(connect, "mjc:target") ==
         Strings({"/World/Links/arm", "/World/Links/tip"}));
  assert(Doubles(connect, "mjc:anchor") == std::vector<double>({0, 0, 1}));
  const PrimSpec &weld = Prim("/World/Equalities/glue");
  assert(APIs(weld) == Strings({"MjcEqualityWeldAPI"}));
  assert(Near(D(weld, "mjc:torqueScale"), 0.5));

  const PrimSpec &muscle = Prim("/World/MjcActuators/muscle");
  assert(muscle.type_name() == "MjcActuator");
  assert(Targets(muscle, "mjc:target") == Strings({"/World/Tendons/cable"}));
  assert(Doubles(muscle, "mjc:gainPrm") == std::vector<double>({1, 2}));
  assert(Near(D(muscle, "mjc:lengthRange:min"), 0.1));
  assert(Near(D(muscle, "mjc:ctrlRange:max"), 1) &&
         Uniform(muscle, "mjc:ctrlRange:max"));
  assert(Tok(muscle, "mjc:dynType") == "muscle");
  assert(Targets(muscle, "mjc:refSite") == Strings({"/World/Sites/s0"}));
  const PrimSpec &motor = Prim("/World/MjcActuators/motor");
  assert(Targets(motor, "mjc:target") == Strings({"/World/Joints/hinge"}));
  assert(Near(D(motor, "mjc:ctrlRange:max"), 5));
  assert(!motor.property_value("mjc:ctrlRange:min"));
  assert(*Val(motor, "mjc:group").as_int() == 1);
  assert(Targets(Prim("/World/MjcActuators/thrust"), "mjc:target") ==
         Strings({"/World/Sites/s1_1"}));
}

void TestMiscScopes() {
  assert(Children("/World/Keyframes") == Strings({"home", "key_1"}));
  const PrimSpec &home = Prim("/World/Keyframes/home");
  assert(home.type_name() == "MjcKeyframe");
  assert(Doubles(home, "mjc:qpos") == std::vector<double>({0, 1}));
  assert(Uniform(home, "mjc:ctrl"));

  const PrimSpec &gyro = Prim("/World/Sensors/gyro");
  assert(gyro.type_name() == "MjcSensor");
  assert(Tok(gyro, "mjc:type") == "gyro");
  assert(Tok(gyro, "mjc:objtype") == "site" && Tok(gyro, "mjc:objname") == "s0");
  const PrimSpec &frame = Prim("/World/Sensors/frame");
  assert(Tok(frame, "mjc:objtype") == "body");
  assert(Tok(frame, "mjc:reftype") == "site" && Tok(frame, "mjc:refname") == "s1");
  assert(Near(D(frame, "mjc:noise"), 0.1));
  assert(Doubles(frame, "mjc:user") == std::vector<double>({1, 2}));

  const PrimSpec &pair = Prim("/World/Contacts/pair_0");
  assert(Tok(pair, "mjc:geom1") == "a" && Uniform(pair, "mjc:geom1"));
  assert(*Val(pair, "mjc:condim").as_int() == 3);
  assert(Doubles(pair, "mjc:friction") == std::vector<double>({1, 1}));
  assert(Near(D(pair, "mjc:margin"), 0.01) && !Uniform(pair, "mjc:margin"));

  const PrimSpec &spot = Prim("/World/Lights/spot");
  assert(spot.type_name() == "SphereLight");
  assert(Near(D(spot, "inputs:shaping:cone:angle"), 30));
  assert(Near(D(spot, "inputs:radius"), 0.02));
  assert(TypeName(spot, "inputs:color") == "color3f");
  // Default dir (0,0,-1) keeps the light's -Z axis; translation from matrix.
  const double *lm = Val(spot, "xformOp:transform").as_matrix4d();
  assert(lm && Near(lm[10], 1) && Near(lm[14], 2));
  const PrimSpec &sun = Prim("/World/Lights/sun");
  assert(sun.type_name() == "DistantLight");
  assert(*Val(sun, "inputs:shadow:enable").as_bool() == false);
  assert(!sun.property_value("inputs:radius"));

  const PrimSpec &track = Prim("/World/Cameras/track");
  assert(track.type_name() == "Camera");
  assert(Near(D(track, "focalLength"), 50));
  assert(Near(D(track, "verticalAperture"), 100, 1e-3));  // 2*50*tan(45deg)
  assert(Near(Val(track, "xformOp:transform").as_matrix4d()[13], -1));
  const PrimSpec &ortho = Prim("/World/Cameras/ortho");
  assert(Tok(ortho, "projection") == "orthographic");
  assert(ortho.property_value("xformOp:transform"));  // identity is authored

  // Materials: PreviewSurface network; the duplicate `tex` is dropped.
  assert(Children("/World/Materials") == Strings({"body_mat", "tex"}));
  const PrimSpec &body = Prim("/World/Materials/body_mat");
  assert(body.type_name() == "Material");
  const std::vector<Path> *surface = body.connection("outputs:surface");
  assert(surface && surface->size() == 1 &&
         (*surface)[0].str() ==
             "/World/Materials/body_mat/PreviewSurface.outputs:surface");
  const PrimSpec &ps = Prim("/World/Materials/body_mat/PreviewSurface");
  assert(Tok(ps, "info:id") == "UsdPreviewSurface" && Uniform(ps, "info:id"));
  const float *diffuse = Val(ps, "inputs:diffuseColor").as_float3();
  assert(diffuse && Near(diffuse[1], 0.4));
  assert(Near(D(ps, "inputs:opacity"), 0.5));
  assert(Near(D(ps, "inputs:roughness"), 0.3));
  const float *emissive = Val(ps, "inputs:emissiveColor").as_float3();
  assert(emissive && Near(emissive[2], 1.2));
  assert(ps.property("outputs:surface") && !ps.property_value("outputs:surface"));
  assert(Children("/World/Materials/tex") ==
         Strings({"PreviewSurface", "stReader", "DiffuseTexture"}));
  const PrimSpec &tex_ps = Prim("/World/Materials/tex/PreviewSurface");
  assert(!tex_ps.property_value("inputs:diffuseColor"));
  const std::vector<Path> *diffuse_src = tex_ps.connection("inputs:diffuseColor");
  assert(diffuse_src &&
         (*diffuse_src)[0].str() ==
             "/World/Materials/tex/DiffuseTexture.outputs:rgb");
  const PrimSpec &reader = Prim("/World/Materials/tex/stReader");
  assert(Tok(reader, "info:id") == "UsdPrimvarReader_float2");
  assert(*Val(reader, "inputs:varname").as_string() == "st");
  assert(TypeName(reader, "outputs:result") == "float2");
  const PrimSpec &texture = Prim("/World/Materials/tex/DiffuseTexture");
  assert(*Val(texture, "inputs:file").as_asset_path() == "assets/tex.png");
  assert(Tok(texture, "inputs:wrapS") == "repeat");
  assert(Tok(texture, "inputs:sourceColorSpace") == "sRGB");
  const std::vector<Path> *st = texture.connection("inputs:st");
  assert(st && (*st)[0].str() == "/World/Materials/tex/stReader.outputs:result");

  const PrimSpec &custom = Prim("/World/MjcCustom");
  assert(Doubles(custom, "mjc:custom:max_contact_points") ==
         std::vector<double>({4}));
  assert(Tok(custom, "mjc:customtext:note") == "hello");
  const PrimSpec &plugins = Prim("/World/MjcPlugins");
  assert(Tok(plugins, "mjc:plugin:pid:plugin") == "mujoco.pid");
  assert(Tok(plugins, "mjc:plugin:pid:config:kp") == "10");
}

void TestUrdfDefaults() {
  // A URDF (non-MJCF) source gets the legacy URDF collider/visual defaults,
  // and a Y-up stage the corrective root rotation.
  const char *urdf = R"JSON({
    "links": [{"name": "base",
      "visuals": [{"geometry": {"positions": [0,0,0, 1,0,0, 0,1,0]}}],
      "collisions": [{"shape": {"type": "sphere"}}]}]
  })JSON";
  Stage stage;
  std::string warn, err;
  assert(lightusd::tydra::next::ConvertURDFJsonToUSDStage(urdf, &stage, &warn,
                                                          &err));
  const Layer *layer = stage.GetRootLayer();
  assert(layer->meta().upAxis == "Y");
  const PrimSpec *world = layer->prim_at_path("/World");
  assert(world && world->property_value("xformOp:rotateX"));
  const PrimSpec *visual = layer->prim_at_path("/World/Links/base/visual_0");
  assert(visual && *visual->property_value("mjc:group")->as_int() == 2);
  const PrimSpec *collider = layer->prim_at_path("/World/Links/base/collision_0");
  assert(collider && *collider->property_value("mjc:group")->as_int() == 3);
  assert(*collider->property_value("mjc:condim")->as_int() == 3);
  assert(*collider->property_value("mjc:solmix")->as_double() == 1.0);
  assert(*collider->property_value("newton:contactGap")->as_float() == 0.0f);
  // Articulation root without joints, default self-collision.
  const PrimSpec *base = layer->prim_at_path("/World/Links/base");
  assert(*base->property_value("newton:selfCollisionEnabled")->as_bool());
  // No MJCF scopes are authored for an empty payload section.
  assert(!layer->prim_at_path("/World/Sites"));
}

}  // namespace

int main() {
  std::cout << "Testing next URDF/MJCF JSON -> USD converter...\n";
  Stage stage;
  std::string warn, err;
  const bool ok =
      lightusd::tydra::next::ConvertURDFJsonToUSDStage(kPayload, &stage, &warn,
                                                       &err);
  if (!ok) std::cerr << err << "\n";
  assert(ok);
  assert(err.empty());
  // Skipped entities are reported.
  assert(warn.find("Skipping mesh `visual_1`") != std::string::npos);
  assert(warn.find("Skipping Newton actuator `dangling`") != std::string::npos);
  assert(warn.find("Skipping tendon `lost`") != std::string::npos);
  assert(warn.find("Skipping joint `orphan`") != std::string::npos);
  g_layer = stage.GetRootLayer();
  assert(g_layer);
  assert(g_layer->meta().defaultPrim == "World");
  assert(g_layer->meta().upAxis == "Z");

  TestScopesAndScene();
  TestLinks();
  TestJoints();
  TestActuatorsSitesTendons();
  TestEqualitiesAndMjcActuators();
  TestMiscScopes();
  TestUrdfDefaults();
  std::cout << "All next URDF/MJCF converter tests passed\n";
  return 0;
}
