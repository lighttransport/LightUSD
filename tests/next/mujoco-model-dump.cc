// SPDX-License-Identifier: Apache-2.0
// Compile a MuJoCo model (MJCF, or USD through MuJoCo's USD decoder plugin)
// and print its compiled element data as JSON, for
// tests/next/run-mujoco-usd-compare.py.
//
// Build against a MuJoCo built with -DMUJOCO_WITH_USD=ON:
//   c++ -std=c++17 -I$MJ/include mujoco-model-dump.cc -L$MJ/build/lib -lmujoco
//       -Wl,-rpath,$MJ/build/lib -o mujoco-model-dump
// Usage: mujoco-model-dump <model.xml|model.usda> [usd_decoder_plugin.so]
#include <mujoco/mujoco.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void Escaped(const char* text) {
  for (const char* c = text ? text : ""; *c; ++c) {
    const unsigned char u = static_cast<unsigned char>(*c);
    if (u < 0x20) {
      std::printf("\\u%04x", u);
      continue;
    }
    if (*c == '"' || *c == '\\') std::putchar('\\');
    std::putchar(*c);
  }
}

void Name(const mjModel* m, mjtObj type, int id) {
  std::printf("\"name\":\"");
  Escaped(mj_id2name(m, type, id));
  std::printf("\"");
}

void Array(const char* key, const mjtNum* v, int n) {
  std::printf(",\"%s\":[", key);
  for (int i = 0; i < n; ++i) {
    const double x = v[i];
    if (std::isfinite(x)) std::printf("%s%.10g", i ? "," : "", x);
    else std::printf("%s%s", i ? "," : "", x > 0 ? "1e308" : "-1e308");
  }
  std::printf("]");
}

void Int(const char* key, int v) { std::printf(",\"%s\":%d", key, v); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s model [usd_decoder_plugin.so]\n", argv[0]);
    return 2;
  }
  // Engine plugins (pid, elasticity, ...) for MJCF models that use them.
  if (const char* dir = std::getenv("MUJOCO_PLUGIN_DIR")) mj_loadAllPluginLibraries(dir, nullptr);
  if (argc > 2) mj_loadPluginLibrary(argv[2]);
  const std::string path = argv[1];
  const bool usd = path.size() > 5 && (path.rfind(".usda") == path.size() - 5 ||
                                       path.rfind(".usdc") == path.size() - 5 ||
                                       path.rfind(".usd") == path.size() - 4);
  char error[2048] = "";
  mjSpec* spec = mj_parse(path.c_str(), usd ? "model/usd" : nullptr, nullptr, error, sizeof(error));
  if (!spec) {
    std::printf("{\"error\":\"parse: ");
    Escaped(error);
    std::printf("\"}\n");
    return 1;
  }
  mjModel* m = mj_compile(spec, nullptr);
  if (!m) {
    std::printf("{\"error\":\"compile: ");
    Escaped(mjs_getError(spec));
    std::printf("\"}\n");
    return 1;
  }
  long colliding = 0;
  for (int g = 0; g < m->ngeom; ++g) colliding += m->geom_contype[g] || m->geom_conaffinity[g];
  std::printf("{\"counts\":{\"nbody\":%ld,\"njnt\":%ld,\"ngeom\":%ld,\"nsite\":%ld,\"nu\":%ld,"
              "\"ntendon\":%ld,\"neq\":%ld,\"nmesh\":%ld,\"npair\":%ld,\"nexclude\":%ld,"
              "\"nkey\":%ld,\"nflex\":%ld,\"ncolliding\":%ld}",
              long(m->nbody), long(m->njnt), long(m->ngeom), long(m->nsite), long(m->nu),
              long(m->ntendon), long(m->neq), long(m->nmesh), long(m->npair), long(m->nexclude),
              long(m->nkey), long(m->nflex), colliding);
  std::printf(",\"option\":{");
  std::printf("\"timestep\":%.10g,\"iterations\":%ld,\"integrator\":%d,\"cone\":%d,\"solver\":%d",
              m->opt.timestep, long(m->opt.iterations), m->opt.integrator, m->opt.cone, m->opt.solver);
  Array("gravity", m->opt.gravity, 3);
  std::printf("}");
  std::printf(",\"bodies\":[");
  for (int b = 1; b < m->nbody; ++b) {
    std::printf("%s{", b > 1 ? "," : "");
    Name(m, mjOBJ_BODY, b);
    Array("mass", m->body_mass + b, 1);
    Array("inertia", m->body_inertia + 3 * b, 3);
    Array("ipos", m->body_ipos + 3 * b, 3);
    Array("gravcomp", m->body_gravcomp + b, 1);
    std::printf("}");
  }
  std::printf("],\"joints\":[");
  for (int j = 0; j < m->njnt; ++j) {
    std::printf("%s{", j ? "," : "");
    Name(m, mjOBJ_JOINT, j);
    Int("type", m->jnt_type[j]);
    Int("limited", m->jnt_limited[j]);
    Array("range", m->jnt_range + 2 * j, 2);
    Array("axis", m->jnt_axis + 3 * j, 3);
    const int dof = m->jnt_dofadr[j];
    Array("armature", m->dof_armature + dof, 1);
    Array("damping", m->dof_damping + dof, 1);
    Array("frictionloss", m->dof_frictionloss + dof, 1);
    Array("stiffness", m->jnt_stiffness + j, 1);
    Array("springref", m->qpos_spring + m->jnt_qposadr[j], 1);
    std::printf("}");
  }
  std::printf("],\"geoms\":[");
  for (int g = 0; g < m->ngeom; ++g) {
    std::printf("%s{", g ? "," : "");
    Name(m, mjOBJ_GEOM, g);
    Int("type", m->geom_type[g]);
    Int("contype", m->geom_contype[g]);
    Int("conaffinity", m->geom_conaffinity[g]);
    Int("condim", m->geom_condim[g]);
    Int("group", m->geom_group[g]);
    Int("priority", m->geom_priority[g]);
    Array("size", m->geom_size + 3 * g, 3);
    Array("friction", m->geom_friction + 3 * g, 3);
    Array("solref", m->geom_solref + mjNREF * g, mjNREF);
    Array("solimp", m->geom_solimp + mjNIMP * g, mjNIMP);
    Array("margin", m->geom_margin + g, 1);
    Array("gap", m->geom_gap + g, 1);
    Array("solmix", m->geom_solmix + g, 1);
    std::printf("}");
  }
  std::printf("],\"actuators\":[");
  for (int u = 0; u < m->nu; ++u) {
    std::printf("%s{", u ? "," : "");
    Name(m, mjOBJ_ACTUATOR, u);
    Int("trntype", m->actuator_trntype[u]);
    Int("dyntype", m->actuator_dyntype[u]);
    Int("gaintype", m->actuator_gaintype[u]);
    Int("biastype", m->actuator_biastype[u]);
    Array("gainprm", m->actuator_gainprm + mjNGAIN * u, 3);
    Array("biasprm", m->actuator_biasprm + mjNBIAS * u, 3);
    Array("dynprm", m->actuator_dynprm + mjNDYN * u, 3);
    Array("gear", m->actuator_gear + 6 * u, 6);
    Array("ctrlrange", m->actuator_ctrlrange + 2 * u, 2);
    Array("forcerange", m->actuator_forcerange + 2 * u, 2);
    Int("ctrllimited", m->actuator_ctrllimited[u]);
    Int("forcelimited", m->actuator_forcelimited[u]);
    std::printf("}");
  }
  std::printf("],\"tendons\":[");
  for (int t = 0; t < m->ntendon; ++t) {
    std::printf("%s{", t ? "," : "");
    Name(m, mjOBJ_TENDON, t);
    Int("num", m->tendon_num[t]);
    Int("limited", m->tendon_limited[t]);
    Array("range", m->tendon_range + 2 * t, 2);
    Array("stiffness", m->tendon_stiffness + t, 1);
    Array("damping", m->tendon_damping + t, 1);
    Array("length0", m->tendon_length0 + t, 1);
    std::printf("}");
  }
  std::printf("],\"equalities\":[");
  for (int e = 0; e < m->neq; ++e) {
    std::printf("%s{", e ? "," : "");
    Name(m, mjOBJ_EQUALITY, e);
    Int("type", m->eq_type[e]);
    Int("active", m->eq_active0[e]);
    Array("solref", m->eq_solref + mjNREF * e, mjNREF);
    std::printf("}");
  }
  std::printf("]}\n");
  mj_deleteModel(m);
  mj_deleteSpec(spec);
  return 0;
}
