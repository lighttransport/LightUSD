// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#if defined(__linux__)
#include <pthread.h>
#endif
#include <string>

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
  // A visible collider is one prim (its visual and collider roles merged).
  const auto shape = lightusd_stage_prim_at_path(stage, "/World/Links/body/shape");
  assert(!lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage, "/World/Links/body/shape1")));
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
  // A link without inertia or shapes would be a ghost frame (no APIs).
  stage = Import("<robot><link name='base'><inertial><mass value='1'/>"
                 "<inertia ixx='1' iyy='1' izz='1' ixy='0' ixz='0' iyz='0'/></inertial>"
                 "</link></robot>", LIGHTUSD_IMPORT_URDF);
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
  const char* unsupported = "<mujoco><worldbody><body name='base'/></worldbody><deformable><flex name='cloth'/></deformable></mujoco>";
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
  // The longest accepted cable nests 511 bodies: every tree walker must cope.
  const char* cable = "<mujoco><worldbody><body name='h'><composite type='cable' curve='s' "
                      "count='512 1 1' size='10'><geom type='capsule' size='.001'/></composite>"
                      "</body></worldbody></mujoco>";
  assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(cable), std::strlen(cable),
                                "memory:cable.xml", &options, &stage) == LIGHTUSD_OK);
  assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage, "/World/Links/B_last")));
  lightusd_stage_destroy(stage);
  // Progress callbacks: cancellation and callers built before the fields.
  {
    lightusd_import_options progress_options;
    lightusd_import_options_init(&progress_options);
    progress_options.format = LIGHTUSD_IMPORT_MJCF;
    static int calls = 0;
    progress_options.progress = [](void*, const char* phase, uint64_t, uint64_t) {
      ++calls;
      return std::strcmp(phase, "convert") == 0 ? 1 : 0;
    };
    assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(cable), std::strlen(cable),
                                  "memory:cable.xml", &progress_options, &stage) == LIGHTUSD_ERR_CANCELLED);
    assert(!stage && calls > 0);
    lightusd_import_options old_options = progress_options;
    old_options.struct_size = static_cast<uint32_t>(offsetof(lightusd_import_options, progress));
    old_options.progress = nullptr;
    assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(cable), std::strlen(cable),
                                  "memory:cable.xml", &old_options, &stage) == LIGHTUSD_OK);
    lightusd_stage_destroy(stage);
  }
  // Hostile inputs fail cleanly and quickly instead of expanding, spinning or
  // reading non-finite data.
  {
    lightusd_import_options limits;
    lightusd_import_options_init(&limits);
    limits.format = LIGHTUSD_IMPORT_MJCF;
    auto rejected = [&](const std::string& xml, uint64_t max_memory = 0) {
      lightusd_import_options o = limits;
      if (max_memory) o.max_memory_bytes = max_memory;
      lightusd_stage* s = nullptr;
      const lightusd_status st = lightusd_import_memory(
          reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), "memory:bad.xml", &o, &s);
      if (s) lightusd_stage_destroy(s);
      return st != LIGHTUSD_OK && !s;
    };
    // An empty <replicate> still adds one <frame> per copy.
    assert(rejected("<mujoco><worldbody><replicate count='2000000'/></worldbody></mujoco>"));
    // Cable composites share the expansion budget (each is 512 vertices).
    std::string cables = "<mujoco><worldbody>";
    for (int i = 0; i < 300; ++i)
      cables += "<body name='h" + std::to_string(i) + "'><composite prefix='c" + std::to_string(i) +
                "' type='cable' curve='s' count='512 1 1' size='1'><geom type='capsule' "
                "size='.001'/></composite></body>";
    cables += "</worldbody></mujoco>";
    assert(rejected(cables));
    // A pin range far outside the lattice does not iterate it.
    assert(rejected("<mujoco><worldbody><flexcomp name='f' type='grid' dim='2' count='3 3 1' "
                    "spacing='.1 .1 .1'><pin gridrange='0 0 2147483647 2'/></flexcomp>"
                    "</worldbody></mujoco>"));
    for (const char* pin : {"<pin id='1.5'/>", "<pin id='2147483648'/>",
                            "<pin range='0'/>", "<pin grid='0'/>",
                            "<pin gridrange='0 0 1'/>", "<pin range='99 -1'/>",
                            "<pin gridrange='99 0 -1 2'/>"}) {
      const std::string bad = std::string("<mujoco><worldbody><flexcomp name='f' type='grid' ") +
          "dim='2' count='3 3 1' spacing='.1 .1 .1'>" + pin +
          "</flexcomp></worldbody></mujoco>";
      assert(rejected(bad));
    }
    // Connectivity is integral: truncating 2.5 would silently turn a bad
    // direct triangle into a different, otherwise valid triangle.
    for (const char* elements : {"0 1 2.5", "0 1 2147483648", "0 1 -1"}) {
      const std::string bad = std::string("<mujoco><worldbody><flexcomp name='f' type='direct' ") +
          "dim='2' point='0 0 0 1 0 0 0 1 0' element='" + elements +
          "'/></worldbody></mujoco>";
      assert(rejected(bad));
    }
    const char* direct = "<mujoco><worldbody><flexcomp name='f' type='direct' dim='2' "
                         "point='0 0 0 1 0 0 0 1 0' element='0 1 2'/>"
                         "</worldbody></mujoco>";
    lightusd_stage* direct_stage = nullptr;
    assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(direct), std::strlen(direct),
                                  "memory:direct.xml", &limits, &direct_stage) == LIGHTUSD_OK);
    assert(direct_stage);
    lightusd_stage_destroy(direct_stage);
    // MuJoCo combines pin attributes across elements before parsing tuples.
    const char* split_pins = "<mujoco><worldbody><flexcomp name='f' type='grid' "
        "dim='2' count='3 3 1' spacing='.1 .1 .1'><pin range='0'/><pin range='1'/>"
        "<pin grid='1'/><pin grid='2'/><pin gridrange='2 0'/><pin gridrange='2 1'/>"
        "<pin range='2 1' gridrange='1 2 0 1'/>"
        "</flexcomp></worldbody></mujoco>";
    lightusd_stage* pin_stage = nullptr;
    assert(lightusd_import_memory(reinterpret_cast<const uint8_t*>(split_pins),
                                  std::strlen(split_pins), "memory:pins.xml", &limits,
                                  &pin_stage) == LIGHTUSD_OK);
    auto pins = lightusd_stage_prim_at_path(pin_stage, "/World/Deformables/f_pins");
    assert(lightusd_prim_is_valid(pins));
    lightusd_value_view pin_indices{};
    assert(lightusd_attr_get(pins, "physics:indices0", &pin_indices) == LIGHTUSD_OK);
    const int32_t expected_pins[] = {0, 1, 5, 6, 7};
    assert(pin_indices.count == 5 &&
           std::memcmp(pin_indices.data, expected_pins, sizeof(expected_pins)) == 0);
    lightusd_stage_destroy(pin_stage);
    // A 10M-point lattice is charged before it is built.
    assert(rejected("<mujoco><worldbody><flexcomp name='f' type='grid' dim='3' "
                    "count='215 215 215' spacing='.1 .1 .1'/></worldbody></mujoco>",
                    64ull << 20));
    // A binary STL with a NaN vertex is rejected, not welded into the mesh.
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    const std::string stl = dir + "/lightusd_nan_" + std::to_string(reinterpret_cast<uintptr_t>(&limits)) + ".stl";
    const std::string xml = stl.substr(0, stl.size() - 4) + ".xml";
    {
      unsigned char bytes[84 + 50] = {};
      const uint32_t one = 1;
      std::memcpy(bytes + 80, &one, 4);
      const float v[9] = {0, 0, 0, 1, 0, 0, 0, std::nanf(""), 0};
      std::memcpy(bytes + 84 + 12, v, sizeof(v));
      FILE* f = std::fopen(stl.c_str(), "wb");
      const size_t wrote = f ? std::fwrite(bytes, 1, sizeof(bytes), f) : 0;
      assert(wrote == sizeof(bytes));
      if (f) std::fclose(f);
      const std::string model = "<mujoco><asset><mesh name='m' file='" + stl +
                                "'/></asset><worldbody><geom type='mesh' mesh='m'/></worldbody></mujoco>";
      f = std::fopen(xml.c_str(), "wb");
      const size_t wrote_xml = f ? std::fwrite(model.data(), 1, model.size(), f) : 0;
      assert(wrote_xml == model.size());
      if (f) std::fclose(f);
    }
    lightusd_stage* s = nullptr;
    const lightusd_status nan_status = lightusd_import_file(xml.c_str(), &limits, &s);
    assert(nan_status != LIGHTUSD_OK && !s);
    assert(std::string(lightusd_last_error()).find("not finite") != std::string::npos);
    std::remove(stl.c_str());
    std::remove(xml.c_str());
  }
#if defined(__linux__)
  // The walkers are iterative: a 1000-deep body chain plus a 512-vertex cable
  // imports on a 512 KB thread stack.
  {
    struct Job {
      std::string xml, error;
      lightusd_status status = LIGHTUSD_OK;
    } job;
    job.xml = "<mujoco><worldbody>";
    for (int i = 0; i < 1000; ++i) job.xml += "<body name='b" + std::to_string(i) + "'><geom size='.01'/>";
    job.xml += "<composite type='cable' curve='s' count='512 1 1' size='1'><geom type='capsule' "
               "size='.001'/></composite>";
    for (int i = 0; i < 1000; ++i) job.xml += "</body>";
    job.xml += "</worldbody></mujoco>";
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t thread;
    const int created = pthread_create(&thread, &attr, [](void* arg) -> void* {
      auto* j = static_cast<Job*>(arg);
      lightusd_import_options o;
      lightusd_import_options_init(&o);
      o.format = LIGHTUSD_IMPORT_MJCF;
      o.max_depth = 1024;
      lightusd_stage* s = nullptr;
      j->status = lightusd_import_memory(reinterpret_cast<const uint8_t*>(j->xml.data()),
                                         j->xml.size(), "memory:deep.xml", &o, &s);
      j->error = lightusd_last_error();  // per thread
      if (s) lightusd_stage_destroy(s);
      return nullptr;
    }, &job);
    assert(created == 0);
    if (created == 0) pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    if (job.status != LIGHTUSD_OK) std::cerr << job.error << "\n";
    assert(job.status == LIGHTUSD_OK);
  }
#endif
  std::cout << "Native import C API tests passed\n";
}
