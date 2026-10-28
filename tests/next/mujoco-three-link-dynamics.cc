// SPDX-License-Identifier: Apache-2.0
// CPU-only trajectory sampler for tests/feat/physics-oracle/three-link-chain.xml.
// Build against MuJoCo with:
//   c++ -std=c++17 -I$MUJOCO_SOURCE/include \
//       tests/next/mujoco-three-link-dynamics.cc \
//       -L$MUJOCO_BUILD/lib -Wl,-rpath,$MUJOCO_BUILD/lib -lmujoco \
//       -o /tmp/mujoco-three-link-dynamics
#include <mujoco/mujoco.h>

#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s model.xml\n", argv[0]);
    return 2;
  }

  char error[2048] = {};
  mjModel* model = mj_loadXML(argv[1], nullptr, error, sizeof(error));
  if (!model) {
    std::fprintf(stderr, "mj_loadXML: %s\n", error);
    return 1;
  }
  const int body_ids[] = {mj_name2id(model, mjOBJ_BODY, "link1"),
                          mj_name2id(model, mjOBJ_BODY, "link2"),
                          mj_name2id(model, mjOBJ_BODY, "link3")};
  for (int id : body_ids) {
    if (id < 0) {
      std::fprintf(stderr, "model is missing a named link body\n");
      mj_deleteModel(model);
      return 1;
    }
  }

  mjData* data = mj_makeData(model);
  if (!data) {
    std::fprintf(stderr, "mj_makeData failed\n");
    mj_deleteModel(model);
    return 1;
  }
  const int sample_steps[] = {30, 60, 90, 120};
  int next_sample = 0;
  for (int step = 1; step <= sample_steps[3]; ++step) {
    mj_step(model, data);
    if (step != sample_steps[next_sample]) continue;
    std::printf("step %d", step);
    for (int body_id : body_ids) {
      const mjtNum* position = data->xpos + 3 * body_id;
      std::printf(" %.9f %.9f %.9f", position[0], position[1], position[2]);
    }
    std::putchar('\n');
    if (++next_sample == 4) break;
  }
  mj_deleteData(data);
  mj_deleteModel(model);
  return 0;
}
