// SPDX-License-Identifier: Apache-2.0
// CPU-only trajectory sampler for the checked-in MuJoCo chain fixtures.
// Build against MuJoCo with:
//   c++ -std=c++17 -I$MUJOCO_SOURCE/include \
//       tests/next/mujoco-chain-dynamics.cc \
//       -L$MUJOCO_BUILD/lib -Wl,-rpath,$MUJOCO_BUILD/lib -lmujoco \
//       -o /tmp/mujoco-chain-dynamics
#include <mujoco/mujoco.h>

#include <cstdio>
#include <string>
#include <vector>

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
  std::vector<int> body_ids;
  for (int link = 1; link <= 16; ++link) {
    const std::string name = "link" + std::to_string(link);
    const int id = mj_name2id(model, mjOBJ_BODY, name.c_str());
    if (id < 0) break;
    body_ids.push_back(id);
  }
  if (body_ids.size() < 3) {
    std::fprintf(stderr, "model must contain a chain named link1..linkN (N >= 3)\n");
    mj_deleteModel(model);
    return 1;
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
