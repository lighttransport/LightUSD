// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "c-api/lightusd-import-c.h"
#include "minijson.hh"

static lightusd::minijson::Value Report(lightusd_stage* stage) {
  lightusd_string* text = nullptr;
  assert((stage ? lightusd_stage_import_report(stage, &text)
                : lightusd_import_last_report(&text)) == LIGHTUSD_OK);
  auto view = lightusd_string_view(text);
  lightusd::minijson::Value report;
  assert(lightusd::minijson::Parse(std::string(view.data, view.len), &report));
  lightusd_string_destroy(text);
  return report;
}

static lightusd_stage* Import(const char* text, uint32_t format) {
  lightusd_import_options options;
  lightusd_import_options_init(&options);
  options.format = format;
  lightusd_stage* stage = nullptr;
  auto status = lightusd_import_memory(reinterpret_cast<const uint8_t*>(text),
                                       std::strlen(text), "", &options, &stage);
  if (status != LIGHTUSD_OK) std::cerr << lightusd_last_error() << "\n";
  assert(status == LIGHTUSD_OK && stage);
  return stage;
}
int main() {
  auto* stage = Import("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n",
                       LIGHTUSD_IMPORT_OBJ);
  auto mesh = lightusd_stage_prim_at_path(stage, "/World/Links/Geometry/mesh");
  assert(lightusd_prim_is_valid(mesh));
  lightusd_value_view counts{};
  assert(lightusd_attr_get(mesh, "faceVertexCounts", &counts) == LIGHTUSD_OK);
  assert(counts.count == 1 && static_cast<const int32_t*>(counts.data)[0] == 4);
  auto report = Report(stage);
  assert(report["version"].get<int>() == 1);
  assert(report["status"].get<std::string>() == "success");
  assert(report["counts"]["output_vertices"].get<int>() == 4);
  lightusd_string* warnings = nullptr;
  assert(lightusd_stage_take_warnings(stage, &warnings) == LIGHTUSD_OK);
  lightusd_string_destroy(warnings);
  assert(Report(stage).dump() == report.dump());
  lightusd_stage_destroy(stage);
  const char* solver = R"(<mujoco><option solver="Newton" integrator="RK4" noslip_tolerance="0"/>
<default><geom solimp=".8 .9 .02 .4 3" solref=".03 .8"/>
<default class="child"><geom solimp=".7 .85 .01" solref=".05"/></default></default>
<worldbody><body name="body"><joint name="j"/>
<geom name="shape" class="child" type="sphere" size=".2" solimp=".6 .8"/>
</body></worldbody></mujoco>)";
  stage = Import(solver, LIGHTUSD_IMPORT_MJCF);
  // Visual and collider share a source name; the collider gets the next name.
  const auto shape = lightusd_stage_prim_at_path(stage, "/World/Links/body/shape1");
  lightusd_value_view values{};
  assert(lightusd_attr_get(shape, "mjc:solimp", &values) == LIGHTUSD_OK);
  assert(values.is_array && values.storage == LIGHTUSD_COMP_FLOAT64 && values.count == 5);
  const double expected[] = {.6, .8, .01, .4, 3.};
  for (size_t i = 0; i < 5; ++i)
    assert(std::abs(static_cast<const double*>(values.data)[i] - expected[i]) < 1e-12);
  assert(lightusd_attr_get(shape, "mjc:solref", &values) == LIGHTUSD_OK);
  assert(values.is_array && values.storage == LIGHTUSD_COMP_FLOAT64 && values.count == 2);
  assert(std::abs(static_cast<const double*>(values.data)[0] - .05) < 1e-12);
  assert(std::abs(static_cast<const double*>(values.data)[1] - .8) < 1e-12);
  assert(Report(stage)["counts"]["completed_solver_arrays"].get<int>() > 0);
  lightusd_stage_destroy(stage);
  stage = Import("<robot><link name='base'/></robot>", LIGHTUSD_IMPORT_URDF);
  auto base = lightusd_stage_prim_at_path(stage, "/World/Links/base");
  lightusd_value* schemas = nullptr;
  assert(lightusd_prim_get_metadata(base, "apiSchemas", &schemas) ==
         LIGHTUSD_OK);
  lightusd_value_destroy(schemas);
  lightusd_stage_destroy(stage);
  lightusd_import_options options;
  lightusd_import_options_init(&options);
  options.format = LIGHTUSD_IMPORT_OBJ;
  options.max_memory_bytes = 4;
  stage = reinterpret_cast<lightusd_stage*>(uintptr_t(1));
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>("invalid"), 7,
                                "", &options,
                                &stage) == LIGHTUSD_ERR_RESOURCE_LIMIT);
  assert(!stage);
  report = Report(nullptr);
  assert(report["status"].get<std::string>() == "failed");
  assert(report["limits"]["max_memory_bytes"].get<int>() == 4);
  options.struct_size = 0;
  assert(lightusd_import_memory(nullptr, 0, "", &options, &stage) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(!stage);
  assert(Report(nullptr).empty());
  lightusd_string* invalid = reinterpret_cast<lightusd_string*>(uintptr_t(1));
  assert(lightusd_stage_import_report(nullptr, &invalid) == LIGHTUSD_ERR_INVALID_ARG);
  assert(!invalid);
  assert(lightusd_import_last_report(nullptr) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_import_options_init(&options);
  options.format = LIGHTUSD_IMPORT_MJCF;
  const char* oversized = "<mujoco><worldbody><body><geom size='.1' solref='1 2 3'/></body></worldbody></mujoco>";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(oversized),
      std::strlen(oversized), "memory:physics.xml", &options, &stage) == LIGHTUSD_ERR_PARSE);
  assert(!stage);
  assert(Report(nullptr)["error"].get<std::string>().find("Too many MJCF solver values") != std::string::npos);
  options.settings_json = "{\"unsupported_policy\":\"error\"}";
  const char* unsupported = "<mujoco><worldbody><body name='base'/><flexcomp name='cloth' type='gmsh'/></worldbody></mujoco>";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(unsupported),
      std::strlen(unsupported), "memory:physics.xml", &options, &stage) == LIGHTUSD_ERR_PARSE);
  assert(!stage);
  report = Report(nullptr);
  assert(report["status"].get<std::string>() == "failed");
  assert(report["features"][size_t(0)]["disposition"].get<std::string>() == "rejected");
  options.settings_json = "{\"unsupported_policy\":\"preserve\"}";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(unsupported),
      std::strlen(unsupported), "memory:physics.xml", &options, &stage) == LIGHTUSD_OK);
  assert(Report(stage)["features"][size_t(0)]["disposition"].get<std::string>() == "preserved");
  lightusd_string* text = nullptr;
  assert(lightusd_stage_export_usda(stage, &text) == LIGHTUSD_OK);
  const auto view = lightusd_string_view(text);
  assert(std::string(view.data, view.len).find("lightusdImportUnsupported") != std::string::npos);
  lightusd_string_destroy(text);
  lightusd_stage_destroy(stage);

  // Newton profile paths in one model (also the sanitizer target): inferred
  // inertia, stacked joints, expanded actuators, sites below bodies, every
  // equality kind and physics materials; then the legacy and openusd profiles.
  const char* robot = R"(<mujoco><compiler inertiagrouprange="0 4"/><worldbody>
<body name="a" pos="0 0 1"><freejoint/><geom type="capsule" fromto="0 0 0 .2 0 0" size=".05" friction=".8"/>
<geom type="box" size=".1 .1 .1" contype="0" conaffinity="0" group="4"/><site name="sa" pos=".1 0 0"/>
<body name="b" pos=".3 0 0"><joint name="j1" axis="0 1 0" range="-30 30" damping="2" armature=".1"/>
<joint name="j2" type="slide" axis="1 0 0" limited="false" range="-1 1"/>
<geom type="ellipsoid" size=".05 .06 .07" density="300"/><site name="sb" pos="-.1 0 0"/></body>
<body name="c" pos="0 .3 0"><joint name="j3" axis="1 0 0"/><geom type="sphere" size=".04"/></body></body>
</worldbody><equality><connect site1="sa" site2="sb"/><weld body1="c"/>
<joint joint1="j3" joint2="j1" polycoef="0 -1 0 0 0" active="false"/></equality>
<actuator><position joint="j1" kp="5" kv=".5"/><motor joint="j3" gear="2"/>
<velocity joint="j2"/></actuator></mujoco>)";
  for (const char* profile : {"newton", "legacy", "openusd"}) {
    lightusd_import_options_init(&options);
    options.format = LIGHTUSD_IMPORT_MJCF;
    const std::string settings = std::string("{\"profile\":\"") + profile + "\"}";
    options.settings_json = settings.c_str();
    assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(robot), std::strlen(robot),
                                  "memory:robot.xml", &options, &stage) == LIGHTUSD_OK);
    const bool legacy = std::string(profile) == "legacy";
    auto a = lightusd_stage_prim_at_path(stage, "/World/Links/a");
    lightusd_value_view mass{};
    assert((lightusd_attr_get(a, "physics:mass", &mass) == LIGHTUSD_OK) == !legacy);
    auto site = lightusd_stage_prim_at_path(stage, "/World/Links/a/sa");
    assert(lightusd_prim_is_valid(site) == !legacy);
    // Unnamed equalities are numbered in kept order (legacy skips the site
    // connect), so find the weld by name prefix.
    auto weld = lightusd_stage_prim_at_path(stage, legacy ? "/World/Equalities/weld_0"
                                                          : "/World/Equalities/weld_1");
    assert(lightusd_prim_is_valid(weld));
    auto drive = lightusd_stage_prim_at_path(stage, legacy ? "/World/Actuators/position_j1"
                                                           : "/World/MjcActuators/position_j1");
    assert(lightusd_prim_is_valid(drive) == (std::string(profile) != "openusd"));
    lightusd_string* usda = nullptr;
    assert(lightusd_stage_export_usda(stage, &usda) == LIGHTUSD_OK);
    const auto out = lightusd_string_view(usda);
    const std::string body(out.data, out.len);
    assert((body.find("newton:") != std::string::npos) == (std::string(profile) != "openusd"));
    assert((body.find("PhysicsMaterialAPI") != std::string::npos) == !legacy);
    lightusd_string_destroy(usda);
    lightusd_stage_destroy(stage);
  }
  options.settings_json = "{\"profile\":\"bogus\"}";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(robot), std::strlen(robot),
                                "memory:robot.xml", &options, &stage) == LIGHTUSD_ERR_INVALID_ARG);
  // Macro elements and soft bodies (newton profile): replicate, a cable
  // composite, a pinned flexcomp cloth and a spatial tendon with a wrap
  // cylinder, side site and pulley.
  const char* soft = R"(<mujoco><worldbody>
    <replicate count="2" offset=".3 0 0" sep="r"><body name="ball" pos="0 0 1"><freejoint/>
      <geom size=".05"/><site name="tip"/></body></replicate>
    <body name="hook" pos="0 1 1">
      <composite type="cable" prefix="c" curve="s" count="5 1 1" size=".4">
        <geom type="capsule" size=".01"/></composite>
      <flexcomp name="cloth" type="grid" dim="2" count="3 3 1" spacing=".1 .1 .1"><pin id="0"/></flexcomp>
      <site name="s0" pos="0 0 .2"/>
      <geom name="wrap" type="cylinder" size=".03 .02" pos=".1 0 0" contype="0" conaffinity="0"/>
      <site name="side" pos=".1 .05 0"/></body>
    <body name="arm" pos="0 1 0"><joint name="h" axis="0 1 0"/><geom size=".05"/>
      <site name="s1" pos="0 0 .2"/><site name="s2" pos=".1 0 .2"/></body>
  </worldbody>
  <tendon><spatial name="rope"><site site="s0"/><site site="tip"/></spatial>
    <spatial name="t"><site site="s0"/><geom geom="wrap" sidesite="side"/><site site="s1"/>
      <pulley divisor="2"/><site site="s1"/><site site="s2"/></spatial></tendon></mujoco>)";
  options.settings_json = "{\"profile\":\"newton\"}";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(soft), std::strlen(soft),
                                "memory:soft.xml", &options, &stage) == LIGHTUSD_OK);
  for (const char* path : {"/World/Links/ballr0", "/World/Links/ballr1", "/World/Links/cB_first",
                           "/World/Links/cB_last", "/World/Tendons/roper0", "/World/Tendons/roper1",
                           "/World/Deformables/cloth", "/World/Deformables/cloth_pins"})
    assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage, path)));
  auto tendon = lightusd_stage_prim_at_path(stage, "/World/Tendons/t");
  assert(lightusd_rel_target_count(tendon, "mjc:path") == 4);
  lightusd_value_view segments{};
  assert(lightusd_attr_get(tendon, "mjc:path:segments", &segments) == LIGHTUSD_OK);
  assert(segments.count == 5 && static_cast<const int32_t*>(segments.data)[4] == 1);
  lightusd_stage_destroy(stage);
  std::cout << "Native import C API tests passed\n";
}
