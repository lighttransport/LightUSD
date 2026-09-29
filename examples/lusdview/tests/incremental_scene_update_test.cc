// SPDX-License-Identifier: Apache-2.0
#include "incremental_scene_update.hh"


#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #expr);                                                   \
      ++failures;                                                            \
    }                                                                        \
  } while (false)

lusdview::DrawMeshCPU MakeMesh(const char* path, float x) {
  lusdview::DrawMeshCPU mesh;
  mesh.absPath = path;
  mesh.vertices = {{x, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                   {x + 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
                   {x, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f}};
  mesh.indices = {0, 1, 2};
  mesh.submeshes.push_back({0, 3, -1, -1});
  std::memset(mesh.world, 0, sizeof(mesh.world));
  mesh.world[0] = mesh.world[5] = mesh.world[10] = mesh.world[15] = 1.0f;
  lusdview::CaptureMeshUploadIdentity(&mesh);
  return mesh;
}

struct TestChanges {
  uint64_t base_revision{0}, new_revision{0};
  struct Prim {
    const char* path{""};
    uint32_t flags{0};
    std::vector<lightusd_sv> properties;
  };
  std::vector<Prim> prims;
};

TestChanges TransformChange(uint64_t base) {
  return {base, base + 1, {{"", LIGHTUSD_CHANGE_TRANSFORM, {}}}};
}

lusdview::IncrementalSceneUpdatePlan Plan(
    const lusdview::DrawScene& current, lusdview::DrawScene* next,
    const TestChanges& changes, uint64_t displayed, int meshes) {
  std::vector<lightusd_prim_change> prims;
  for (const auto& prim : changes.prims)
    prims.push_back({prim.path, prim.flags, prim.properties.data(), prim.properties.size()});
  lightusd_render_change_set view{};
  view.struct_size = sizeof(view);
  view.base_revision = changes.base_revision;
  view.prims = prims.data();
  view.prim_count = prims.size();
  return lusdview::PlanIncrementalSceneUpdate(current, next, view, changes.new_revision, displayed, meshes);
}

void TestTargetsOnlyChangedSlots() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/A", 0.0f));
  current.meshes.push_back(MakeMesh("/B", 2.0f));
  lusdview::DrawScene next = current;
  next.meshes[0].vertices[0].px = 0.25f;
  next.meshes[1].world[12] = 3.0f;

  const auto plan = Plan(
      current, &next, TransformChange(7), 7, 2);
  CHECK(plan.compatible);
  CHECK((plan.vertexUpdates == std::vector<size_t>{0}));
  CHECK((plan.worldUpdates == std::vector<size_t>{1}));
}

void TestRejectsStaleOrStructuralChanges() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/A", 0.0f));
  lusdview::DrawScene next = current;
  CHECK(!Plan(
             current, &next, TransformChange(6), 7, 1)
             .compatible);

  auto topology = TransformChange(7);
  topology.prims[0].flags = LIGHTUSD_CHANGE_TOPOLOGY;
  CHECK(Plan(current, &next, topology, 7, 1)
            .compatible);

  auto transform = TransformChange(7);
  next.meshes[0].indices = {0, 2, 1};
  CHECK(!Plan(current, &next, transform, 7, 1)
             .compatible);
  const auto replacement = Plan(
      current, &next, topology, 7, 1);
  CHECK(replacement.compatible);
  CHECK((replacement.replacementUpdates == std::vector<size_t>{0}));

  next = current;
  next.meshes[0].instanceXforms.resize(12);
  const auto instanced = Plan(
      current, &next, transform, 7, 1);
  CHECK(instanced.compatible);
  CHECK((instanced.replacementUpdates == std::vector<size_t>{0}));

  next = current;
  current.meshes[0].jointIdx = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  current.meshes[0].jointWt = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  next = current;
  next.meshes[0].vertices[0].px = 0.5f;
  auto deform = Plan(
      current, &next, transform, 7, 1);
  CHECK(deform.compatible);
  CHECK((deform.vertexUpdates == std::vector<size_t>{0}));
  next.meshes[0].jointIdx[0] = 1;
  CHECK(!Plan(current, &next, transform, 7, 1)
             .compatible);
}

void TestInstancedSlotUpdate() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/Instances", 0.0f));
  current.meshes[0].instanceXforms = {
      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
      1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0};
  current.meshes[0].instanceColors = {1, 0, 0, 0, 1, 0};
  lusdview::DrawScene next = current;
  next.meshes[0].instanceXforms[15] = 3.0f;

  const auto plan = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(plan.compatible);
  CHECK((plan.instanceUpdates == std::vector<size_t>{0}));
  CHECK(plan.replacementUpdates.empty());
  CHECK(plan.vertexUpdates.empty());
  CHECK(plan.worldUpdates.empty());

  next = current;
  const auto unchanged = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(unchanged.compatible);
  CHECK(unchanged.replacementUpdates.empty());
  CHECK(unchanged.instanceUpdates.empty());

  next = current;
  next.meshes[0].instanceColors[0] = 0.25f;
  auto colors = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(colors.compatible);
  CHECK((colors.instanceUpdates == std::vector<size_t>{0}));

  current.meshes[0].instanceOpacities = {1.0f, 0.5f};
  next = current;
  next.meshes[0].instanceOpacities[1] = 0.75f;
  auto opacities = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(opacities.compatible);
  CHECK((opacities.instanceUpdates == std::vector<size_t>{0}));

  current.meshes[0].instanceOpacities = {1.0f, 1.0f};
  next = current;
  next.meshes[0].instanceOpacities[1] = 0.75f;
  auto opacity_class = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(opacity_class.compatible);
  CHECK((opacity_class.replacementUpdates == std::vector<size_t>{0}));

  next = current;
  next.meshes[0].instanceColors.clear();
  auto color_layout = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(color_layout.compatible);
  CHECK((color_layout.replacementUpdates == std::vector<size_t>{0}));

  next = current;
  next.meshes[0].instanceXforms.resize(36, 0.0f);
  next.meshes[0].instanceColors.resize(9, 1.0f);
  next.meshes[0].instanceOpacities.resize(3, 1.0f);
  auto count_layout = Plan(
      current, &next, TransformChange(40), 40, 1);
  CHECK(count_layout.compatible);
  CHECK((count_layout.replacementUpdates == std::vector<size_t>{0}));
}

void TestStructuralSlotRemap() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/A", 0.0f));
  current.meshes.push_back(MakeMesh("/B", 2.0f));
  auto changes = TransformChange(12);
  changes.prims[0].flags = LIGHTUSD_CHANGE_RESYNC;

  lusdview::DrawScene replaced;
  replaced.meshes.push_back(MakeMesh("/B", 2.0f));
  replaced.meshes.push_back(MakeMesh("/C", 4.0f));
  auto plan = Plan(
      current, &replaced, changes, 12, 2);
  CHECK(plan.compatible);
  CHECK(replaced.meshes.size() == 2);
  CHECK(replaced.meshes[0].absPath == "/C");
  CHECK(replaced.meshes[1].absPath == "/B");
  CHECK((plan.replacementUpdates == std::vector<size_t>{0}));

  lusdview::DrawScene removed;
  removed.meshes.push_back(MakeMesh("/B", 2.0f));
  plan = Plan(current, &removed, changes, 12, 2);
  CHECK(plan.compatible);
  CHECK(removed.meshes.size() == 2);
  CHECK(removed.meshes[0].absPath.empty());
  CHECK(removed.meshes[1].absPath == "/B");
  CHECK((plan.replacementUpdates == std::vector<size_t>{0}));

  lusdview::DrawScene added = current;
  added.meshes.push_back(MakeMesh("/C", 4.0f));
  plan = Plan(current, &added, changes, 12, 2);
  CHECK(plan.compatible);
  CHECK(added.meshes.size() == 3);
  CHECK((plan.replacementUpdates == std::vector<size_t>{2}));
}

void TestMaterialSlotUpdate() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/Mesh", 0.0f));
  current.materials.resize(1);
  current.materials[0].absPath = "/Looks/Mat";
  lusdview::DrawScene next = current;
  next.materials[0].baseColor[0] = 0.25f;

  auto changes = TransformChange(20);
  changes.prims[0].path = "/Looks/Mat/Shader";
  changes.prims[0].flags = LIGHTUSD_CHANGE_MATERIAL;
  const auto plan = Plan(
      current, &next, changes, 20, 1);
  CHECK(plan.compatible);
  CHECK((plan.materialUpdates == std::vector<size_t>{0}));
  CHECK(plan.vertexUpdates.empty());
  CHECK(plan.replacementUpdates.empty());
}

lusdview::DrawTextureCPU MakeTexture(uint8_t red) {
  lusdview::DrawTextureCPU texture;
  texture.image.width = 1;
  texture.image.height = 1;
  texture.image.channels = 4;
  texture.image.data = {red, 20, 30, 255};
  texture.assetIdentifier = "color.png";
  lusdview::CaptureTextureUploadIdentity(&texture);
  return texture;
}

void TestTextureSlotUpdate() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/Mesh", 0.0f));
  current.textures.push_back(MakeTexture(10));
  current.textures.push_back(MakeTexture(40));
  lusdview::DrawScene next = current;
  next.textures[1].image.data[0] = 80;

  auto changes = TransformChange(30);
  changes.prims[0].path = "/Looks/Texture";
  changes.prims[0].flags = LIGHTUSD_CHANGE_TEXTURE |
                           LIGHTUSD_CHANGE_MATERIAL;
  changes.prims[0].properties = {{"inputs:file", 11}};
  const auto plan = Plan(
      current, &next, changes, 30, 1);
  CHECK(plan.compatible);
  CHECK((plan.textureUpdates == std::vector<size_t>{1}));
  CHECK(plan.estimatedUploadBytes == 4);
  CHECK(plan.materialUpdates.empty());
  CHECK(plan.replacementUpdates.empty());

  // The public property span does not require a terminator. A suffix beyond
  // its length must not affect Material-versus-Texture classification.
  const char counted[] = {'f','i','l','e'};
  changes.prims[0].properties = {{counted, sizeof(counted)}};
  CHECK(Plan(current, &next, changes, 30, 1).compatible);
  const char misleading[] = "roughnessfile";
  changes.prims[0].properties = {{misleading, 9}};
  CHECK(!Plan(current, &next, changes, 30, 1).compatible);
  changes.prims[0].properties = {{"inputs:file", 11}};

  next = current;
  next.textures[0].isUdim = true;
  CHECK(!Plan(current, &next, changes, 30, 1)
             .compatible);
}

void TestCChangeRecords() {
  lusdview::DrawScene current;
  current.meshes.push_back(MakeMesh("/A", 0.0f));
  lusdview::DrawScene next = current;
  next.meshes[0].world[12] = 4.0f;
  lightusd_prim_change prim{"/A", LIGHTUSD_CHANGE_TRANSFORM, nullptr, 0};
  lightusd_render_change_set changes{};
  changes.struct_size = sizeof(changes);
  changes.base_revision = 7;
  changes.prims = &prim;
  changes.prim_count = 1;
  const auto plan = [&] {
    return lusdview::PlanIncrementalSceneUpdate(current, &next, changes, 8, 7, 1);
  };
  CHECK(plan().compatible);
  changes.struct_size -= 1;
  CHECK(!plan().compatible);
  changes.struct_size = sizeof(changes);
  changes.prims = nullptr;
  CHECK(!plan().compatible);
  changes.prims = &prim;
  prim.prim_path = nullptr;
  CHECK(!plan().compatible);
  prim.prim_path = "/A";
  prim.property_count = 1;
  CHECK(!plan().compatible);
  lightusd_sv property{nullptr, 1};
  prim.properties = &property;
  CHECK(!plan().compatible);
  property.len = 0;
  CHECK(plan().compatible);
  prim.flags = 1u << 31;
  CHECK(!plan().compatible);
  prim.flags = LIGHTUSD_CHANGE_TRANSFORM;
  changes.full_resync = 1;
  CHECK(!plan().compatible);
  changes.full_resync = 0;
  changes.stage_metadata_changed = 1;
  CHECK(!plan().compatible);
  changes.stage_metadata_changed = 0;
  CHECK(!lusdview::PlanIncrementalSceneUpdate(current, &next, changes, 7, 7, 1).compatible);
  CHECK(!lusdview::PlanIncrementalSceneUpdate(current, nullptr, changes, 8, 7, 1).compatible);
  CHECK(current.meshes[0].world[12] == 0.0f);
}

}  // namespace

int main() {
  TestCChangeRecords();
  TestTargetsOnlyChangedSlots();
  TestRejectsStaleOrStructuralChanges();
  TestStructuralSlotRemap();
  TestMaterialSlotUpdate();
  TestTextureSlotUpdate();
  TestInstancedSlotUpdate();
  return failures == 0 ? 0 : 1;
}
