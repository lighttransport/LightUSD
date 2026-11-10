// SPDX-License-Identifier: Apache-2.0
#include "scene_gizmos.hh"

#include <array>
#include <iostream>
#include <limits>
#include <vector>
#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      return 1;                              \
    }                                        \
  } while (0)
int main() {
  lusdview::DrawScene scene;
  lusdview::DrawLightCPU light;
  light.type = lusdview::DrawLightCPU::Type::Rect;
  light.position[0] = 10;
  light.position[1] = 20;
  light.position[2] = 30;
  light.transform[0] = 2;
  light.transform[5] = 3;
  light.transform[10] = 4;
  light.transform[15] = 1;
  light.width = 4;
  light.height = 2;
  scene.lights.push_back(light);
  using Segment = std::array<float, 9>;
  std::vector<Segment> lines;
  const auto emit = [&](float ax, float ay, float az, float bx, float by,
                        float bz, float r, float g, float b) {
    lines.push_back({ax, ay, az, bx, by, bz, r, g, b});
  };
  lusdview::AppendSceneGizmos(scene, false, false, 1, emit);
  CHECK(lines.empty());
  lusdview::AppendSceneGizmos(scene, true, false, 1, emit);
  CHECK(lines.size() == 10);
  CHECK(lines[6][0] == 14 && lines[6][1] == 23 && lines[6][2] == 30);
  CHECK(lines[6][3] == 6 && lines[6][4] == 23);
  scene.lights[0].type = lusdview::DrawLightCPU::Type::Sphere;
  scene.lights[0].radius = 2;
  lines.clear();
  lusdview::AppendSceneGizmos(scene, true, false, 1, emit);
  CHECK(lines.size() == 99);
  CHECK(lines[3][0] == 14 && lines[3][1] == 20 && lines[3][2] == 30);
  scene.lights[0].type = lusdview::DrawLightCPU::Type::Cylinder;
  lines.clear();
  lusdview::AppendSceneGizmos(scene, true, false, 1, emit);
  CHECK(lines.size() == 71);
  lusdview::DrawCameraCPU camera;
  camera.eye[0] = 10;
  camera.eye[1] = 20;
  camera.eye[2] = 30;
  camera.horizontalAperture = 4;
  camera.verticalAperture = 2;
  camera.focalLength = 2;
  camera.horizontalApertureOffset = 1;
  scene.cameras.push_back(camera);
  lines.clear();
  lusdview::AppendSceneGizmos(scene, false, true, 2, emit);
  CHECK(lines.size() == 8);
  CHECK(lines[0][0] == 13 && lines[0][1] == 21 && lines[0][2] == 28);
  CHECK(lines[1][0] == 10 && lines[1][1] == 20 && lines[1][2] == 30);
  scene.cameras[0].projection =
      lusdview::DrawCameraCPU::Projection::Orthographic;
  scene.cameras[0].focalLength = 0;
  lines.clear();
  lusdview::AppendSceneGizmos(scene, false, true, 2, emit);
  CHECK(lines.size() == 8);
  CHECK(lines[1][0] == lines[1][3] && lines[1][1] == lines[1][4]);
  scene.cameras[0].eye[0] = std::numeric_limits<float>::infinity();
  lines.clear();
  lusdview::AppendSceneGizmos(scene, false, true, 2, emit);
  CHECK(lines.empty());
  lusdview::AppendSceneGizmos(scene, true, true,
                              std::numeric_limits<float>::quiet_NaN(), emit);
  CHECK(lines.empty());
  lines.clear();
  lusdview::AppendSceneGizmos(scene, true, true, 1, emit, 7);
  CHECK(lines.size() == 7);
  lines.clear();
  lusdview::AppendSceneGizmos(scene, true, true, 1, emit, 0);
  CHECK(lines.empty());
  std::cout << "PASS core scene light/camera overlays, world transforms, "
               "projections and finite guards\n";
}
