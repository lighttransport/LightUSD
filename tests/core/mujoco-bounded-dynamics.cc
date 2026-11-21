// SPDX-License-Identifier: Apache-2.0
// Bounded trajectory worker for independent MJCF and USD consumer checks.
#include <mujoco/mujoco.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void Array(const mjtNum* values, int count) {
  std::putchar('[');
  for (int i = 0; i < count; ++i) std::printf("%s%.17g", i ? "," : "", double(values[i]));
  std::putchar(']');
}
int main(int argc, char** argv) {
  // scene steps dt sampleEvery force initialQ initialV [USD decoder library]
  if (argc < 8 || argc > 9) return 2;
  char* end = nullptr;
  const long steps = std::strtol(argv[2], &end, 10);
  if (!end || *end || steps < 1 || steps > 100000) return 2;
  const double dt = std::strtod(argv[3], &end);
  if (!end || *end || !std::isfinite(dt) || dt < 1e-6 || dt > .01) return 2;
  const long every = std::strtol(argv[4], &end, 10);
  if (!end || *end || every < 1 || every > steps) return 2;
  const double force = std::strtod(argv[5], &end);
  if (!end || *end || !std::isfinite(force) || std::abs(force) > 1e6) return 2;
  const double initial_q = std::strtod(argv[6], &end);
  if (!end || *end || !std::isfinite(initial_q)) return 2;
  const double initial_v = std::strtod(argv[7], &end);
  if (!end || *end || !std::isfinite(initial_v)) return 2;
  if (argc == 9) mj_loadPluginLibrary(argv[8]);
  const std::string path = argv[1];
  const bool usd = path.size() >= 4 && path.substr(path.size()-4).find("usd") != std::string::npos;
  char error[2048] = {};
  mjSpec* spec = mj_parse(path.c_str(), usd ? "model/usd" : nullptr, nullptr, error, sizeof(error));
  if (!spec) { std::fprintf(stderr,"parse: %s\n",error); return 1; }
  mjModel* model = mj_compile(spec, nullptr);
  if (!model) { std::fprintf(stderr,"compile: %s\n",mjs_getError(spec)); mj_deleteSpec(spec); return 1; }
  mj_deleteSpec(spec);
  mjData* data = mj_makeData(model);
  if (!data) { mj_deleteModel(model); return 1; }
  int body = -1;
  for (int i = 1; i < model->nbody; ++i) {
    const char* label = mj_id2name(model,mjOBJ_BODY,i);
    if (label && (std::strcmp(label,"body")==0 ||
        (std::strlen(label)>5 && std::strcmp(label+std::strlen(label)-5,"/body")==0))) { body=i; break; }
  }
  if (body < 0) { std::fprintf(stderr,"required body unavailable\n"); mj_deleteData(data); mj_deleteModel(model); return 1; }
  model->opt.timestep = dt;
  // Initial scalar state is explicit, identical across consumers. Free joints
  // retain their authored world transform and zero initial velocity.
  if (model->nq == 1 && model->nv == 1) { data->qpos[0]=initial_q; data->qvel[0]=initial_v; }
  else if (force != 0 || initial_q != 0 || initial_v != 0) {
    std::fprintf(stderr,"scalar controls require exactly one scalar DOF\n");
    mj_deleteData(data); mj_deleteModel(model); return 1;
  }
  std::printf("{\"engine\":\"mujoco\",\"version\":\"%s\",\"dt\":%.17g,\"steps\":%ld,\"samples\":[",mj_versionString(),dt,steps);
  bool first=true;
  for (long step=0; step<=steps; ++step) {
    if (step % every == 0 || step == steps) {
      mj_forward(model,data);
      for (int i=0;i<model->nq;++i) if (!std::isfinite(data->qpos[i])) return 1;
      mjtNum velocity[6]; mj_objectVelocity(model,data,mjOBJ_BODY,body,velocity,0);
      std::printf("%s{\"time\":%.17g,\"position\":",first ? "" : ",",step*dt);first=false;
      Array(data->xpos+body*3,3);std::printf(",\"rotation_wxyz\":");Array(data->xquat+body*4,4);
      std::printf(",\"linear_velocity\":");Array(velocity+3,3);
      std::printf(",\"angular_velocity\":");Array(velocity,3);
      std::printf(",\"joint_q\":");Array(data->qpos,model->nq);
      std::printf(",\"joint_qd\":");Array(data->qvel,model->nv);
      std::printf(",\"contact_count\":%d}",data->ncon);
    }
    if (step < steps) {
      if (model->nv == 1) data->qfrc_applied[0]=force;
      mj_step(model,data);
    }
  }
  std::printf("]}\n");mj_deleteData(data);mj_deleteModel(model);return 0;
}
