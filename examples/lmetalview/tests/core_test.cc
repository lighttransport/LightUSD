// SPDX-License-Identifier: Apache-2.0
//
// lmetalview core test: exercises the next+tydra_next preview scene ABI without
// a GPU. Verifies geometry, materials, decoded textures, animation scrubbing and
// the shared camera controller, plus the GPU ABI constants used by the Swift
// side.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lmv.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

bool Finite3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool FiniteMatrix(const float* m) {
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(m[i])) return false;
  }
  return true;
}

void TestAbi() {
  CHECK(sizeof(lmv_vertex) == 56);
  CHECK(offsetof(lmv_vertex, nx) == 12);
  CHECK(offsetof(lmv_vertex, u) == 24);
  CHECK(offsetof(lmv_vertex, u1) == 32);
  CHECK(offsetof(lmv_vertex, r) == 40);
  CHECK(sizeof(lmv_subset) == 16);
  CHECK(LMV_PBR_FLOATS == 80);
  CHECK(sizeof(lmv_material_view) > 0);
}

lmv_scene* Load(const char* path) {
  lmv_load_options options;
  lmv_load_options_init(&options);
  lmv_scene* scene = nullptr;
  char error[512] = {0};
  const lmv_status status = lmv_load(path, &options, &scene, error, sizeof(error));
  if (status != LMV_OK) {
    std::fprintf(stderr, "load %s failed (%d): %s\n", path, status, error);
    CHECK(status == LMV_OK);
    return nullptr;
  }
  CHECK(scene != nullptr);
  return scene;
}

void TestSimpleScene(const char* path) {
  lmv_scene* scene = Load(path);
  if (!scene) return;

  CHECK(lmv_scene_mesh_count(scene) > 0);
  CHECK(lmv_scene_draw_count(scene) > 0);
  CHECK(lmv_scene_material_count(scene) > 0);

  float low[3] = {0, 0, 0};
  float high[3] = {0, 0, 0};
  lmv_scene_bounds(scene, low, high);
  CHECK(Finite3(low) && Finite3(high));
  CHECK(high[0] >= low[0] && high[1] >= low[1] && high[2] >= low[2]);

  for (uint32_t i = 0; i < lmv_scene_mesh_count(scene); ++i) {
    lmv_mesh_view mesh{};
    if (lmv_scene_mesh(scene, i, &mesh) != LMV_OK) continue;
    CHECK(mesh.vertex_count > 0);
    CHECK(mesh.index_count > 0 && mesh.index_count % 3 == 0);
    CHECK(mesh.subset_count > 0);
    for (uint32_t k = 0; k + 2 < mesh.index_count; k += 3) {
      CHECK(mesh.indices[k + 0] < mesh.vertex_count);
      CHECK(mesh.indices[k + 1] < mesh.vertex_count);
      CHECK(mesh.indices[k + 2] < mesh.vertex_count);
    }
    for (uint32_t s = 0; s < mesh.subset_count; ++s) {
      CHECK(mesh.subsets[s].first_index + mesh.subsets[s].index_count <=
            mesh.index_count);
    }
    // Normals must be unit-ish after the converter / fallback generation.
    for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
      const lmv_vertex& vertex = mesh.vertices[v];
      const float len = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny +
                                  vertex.nz * vertex.nz);
      CHECK(std::isfinite(len) && len > 0.5f && len < 1.5f);
    }
  }

  for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
    lmv_draw_view draw{};
    if (lmv_scene_draw(scene, i, &draw) != LMV_OK) continue;
    CHECK(draw.mesh_id >= 0 && static_cast<uint32_t>(draw.mesh_id) <
                                  lmv_scene_mesh_count(scene));
    CHECK(draw.world != nullptr);
    CHECK(FiniteMatrix(draw.world));
  }

  for (uint32_t i = 0; i < lmv_scene_material_count(scene); ++i) {
    lmv_material_view material{};
    if (lmv_scene_material(scene, i, &material) != LMV_OK) continue;
    CHECK(material.pbr != nullptr);
    for (int k = 0; k < LMV_PBR_FLOATS; ++k) {
      CHECK(std::isfinite(material.pbr[k]));
    }
    const float opacity = material.pbr[LMV_PBR_OPACITY];
    CHECK(opacity >= 0.0f && opacity <= 1.0f);
    const float metalness = material.pbr[LMV_PBR_METALNESS];
    CHECK(metalness >= 0.0f && metalness <= 1.0f);
  }

  // Authored camera placement (the viewer's View > Use Authored Camera).
  if (lmv_scene_camera_count(scene) > 0) {
    lmv_camera_view authored{};
    CHECK(lmv_scene_camera(scene, 0, &authored) == LMV_OK);
    CHECK(authored.transform != nullptr);
    CHECK(FiniteMatrix(authored.transform));
    lmv_camera* camera = lmv_camera_create();
    CHECK(camera != nullptr);
    if (camera) {
      lmv_camera_set_up_axis(camera, lmv_scene_up_axis(scene));
      lmv_camera_set_from_camera(camera, &authored);
      lmv_camera_frame frame{};
      lmv_camera_get_frame(camera, &frame);
      CHECK(FiniteMatrix(frame.view));
      CHECK(FiniteMatrix(frame.proj));
      CHECK(frame.far_clip > frame.near_clip);
      lmv_camera_destroy(camera);
    }
  }

  lmv_scene_free(scene);
}

void TestTextureDecode(const char* path) {
  lmv_scene* scene = Load(path);
  if (!scene) return;
  CHECK(lmv_scene_texture_count(scene) > 0);

  int decoded = 0;
  int bound = 0;
  for (uint32_t i = 0; i < lmv_scene_texture_count(scene); ++i) {
    lmv_texture_view texture{};
    if (lmv_scene_texture(scene, i, &texture) != LMV_OK) continue;
    CHECK(texture.uv_scale[0] > 0.0f);
    CHECK(texture.wrap_s <= LMV_WRAP_BLACK);
    if (texture.image_id < 0) continue;
    lmv_image_view image{};
    if (lmv_scene_image(scene, static_cast<uint32_t>(texture.image_id),
                        &image) == LMV_OK) {
      ++decoded;
      CHECK(image.width > 0 && image.height > 0);
      CHECK(image.pixels != nullptr && image.nbytes > 0);
      CHECK(image.channels == 3 || image.channels == 4);
    }
    ++bound;
  }
  CHECK(bound > 0);
  CHECK(decoded > 0);

  // MaterialX / UsdPreviewSurface must have produced a usable PBR block with
  // at least one authored texture slot.
  bool saw_shader = false;
  bool saw_texture_slot = false;
  for (uint32_t i = 0; i < lmv_scene_material_count(scene); ++i) {
    lmv_material_view material{};
    if (lmv_scene_material(scene, i, &material) != LMV_OK) continue;
    if (material.shader_type != 0) saw_shader = true;
    if (material.tex_base_color >= 0 || material.tex_metallic >= 0 ||
        material.tex_roughness >= 0 || material.tex_normal >= 0 ||
        material.tex_emission >= 0 || material.tex_opacity >= 0) {
      saw_texture_slot = true;
    }
  }
  CHECK(saw_shader);
  CHECK(saw_texture_slot);
  lmv_scene_free(scene);
}

void TestAnimationScrub(const char* path) {
  lmv_scene* scene = Load(path);
  if (!scene) return;
  const double start = lmv_scene_start_time(scene);
  const double end = lmv_scene_end_time(scene);
  CHECK(end > start);

  float first[16] = {0};
  bool have_first = false;
  for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
    lmv_draw_view draw{};
    if (lmv_scene_draw(scene, i, &draw) != LMV_OK || !draw.world) continue;
    std::memcpy(first, draw.world, sizeof(first));
    have_first = true;
    break;
  }
  CHECK(have_first);

  char error[512] = {0};
  const lmv_status status = lmv_scene_set_time(scene, end, error, sizeof(error));
  CHECK(status == LMV_OK);
  CHECK(lmv_scene_time(scene) == end);

  bool changed = false;
  for (uint32_t i = 0; i < lmv_scene_draw_count(scene) && !changed; ++i) {
    lmv_draw_view draw{};
    if (lmv_scene_draw(scene, i, &draw) != LMV_OK || !draw.world) continue;
    for (int k = 0; k < 16; ++k) {
      if (std::fabs(draw.world[k] - first[k]) > 1.0e-5f) {
        changed = true;
        break;
      }
    }
  }
  CHECK(changed);
  CHECK(lmv_scene_has_animation(scene) != 0);
  lmv_scene_free(scene);
}

// Fast playback path: node transforms, skin palettes and blend-shape weights
// update in place without a re-conversion.
void TestFastAnimation(const char* xform_path, const char* skinned_path,
                       const char* light_path, const char* instancer_path,
                       const char* camera_path, const char* visibility_path,
                       const char* material_path,
                       const char* instance_visibility_path,
                       const char* proto_path, const char* count_path,
                       const char* points_path) {
  if (xform_path) {
    lmv_scene* scene = Load(xform_path);
    if (scene) {
      float first[16] = {0};
      CHECK(lmv_scene_draw_count(scene) > 0);
      lmv_draw_view draw{};
      CHECK(lmv_scene_draw(scene, 0, &draw) == LMV_OK);
      if (draw.world) std::memcpy(first, draw.world, sizeof(first));
      const double end = lmv_scene_end_time(scene);
      char error[512] = {0};
      const lmv_status status =
          lmv_scene_update_animation(scene, end, error, sizeof(error));
      CHECK(status == LMV_OK);
      CHECK(lmv_scene_time(scene) == end);
      CHECK(lmv_scene_draw(scene, 0, &draw) == LMV_OK);
      bool changed = false;
      if (draw.world) {
        for (int k = 0; k < 16; ++k) {
          if (std::fabs(draw.world[k] - first[k]) > 1.0e-5f) changed = true;
        }
      }
      CHECK(changed);
      lmv_scene_free(scene);
    }
  }

  if (skinned_path) {
    lmv_scene* scene = Load(skinned_path);
    if (scene) {
      bool have_skin = false;
      for (uint32_t i = 0; i < lmv_scene_mesh_count(scene); ++i) {
        lmv_mesh_view mesh{};
        if (lmv_scene_mesh(scene, i, &mesh) != LMV_OK) continue;
        if (mesh.skinned) have_skin = true;
      }
      CHECK(have_skin);

      std::vector<float> palette;
      std::vector<float> morph_weights;
      for (uint32_t i = 0; i < lmv_scene_mesh_count(scene); ++i) {
        lmv_mesh_view mesh{};
        if (lmv_scene_mesh(scene, i, &mesh) != LMV_OK) continue;
        if (palette.empty()) {
          uint32_t count = 0;
          const float* values = lmv_scene_mesh_skin_matrices(scene, i, &count);
          if (values && count > 0) palette.assign(values, values + count * 16);
        }
        if (morph_weights.empty() && mesh.morph_targets > 0) {
          uint32_t count = 0;
          const float* values = lmv_scene_mesh_morph_weights(scene, i, &count);
          if (values && count > 0) {
            morph_weights.assign(values, values + count);
          }
        }
      }
      CHECK(!palette.empty());
      CHECK(!morph_weights.empty());

      char error[512] = {0};
      const double end = lmv_scene_end_time(scene);
      const lmv_status status =
          lmv_scene_update_animation(scene, end, error, sizeof(error));
      CHECK(status == LMV_OK);

      bool palette_changed = false;
      for (uint32_t i = 0; i < lmv_scene_mesh_count(scene); ++i) {
        uint32_t count = 0;
        const float* values = lmv_scene_mesh_skin_matrices(scene, i, &count);
        if (!values || count * 16 != palette.size()) continue;
        for (size_t k = 0; k < palette.size(); ++k) {
          if (std::fabs(values[k] - palette[k]) > 1.0e-5f) {
            palette_changed = true;
            break;
          }
        }
      }
      CHECK(palette_changed);

      bool morph_changed = false;
      for (uint32_t i = 0; i < lmv_scene_mesh_count(scene); ++i) {
        uint32_t count = 0;
        const float* values = lmv_scene_mesh_morph_weights(scene, i, &count);
        if (!values || count != morph_weights.size()) continue;
        for (size_t k = 0; k < morph_weights.size(); ++k) {
          if (std::fabs(values[k] - morph_weights[k]) > 1.0e-5f) {
            morph_changed = true;
            break;
          }
        }
      }
      CHECK(morph_changed);
      lmv_scene_free(scene);
    }
  }

  if (instance_visibility_path) {
    lmv_scene* scene = Load(instance_visibility_path);
    if (scene) {
      CHECK(lmv_scene_draw_count(scene) > 0);
      std::vector<uint32_t> before;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        before.push_back(lmv_scene_draw(scene, i, &draw) == LMV_OK
                             ? draw.visible
                             : 0u);
      }
      char error[512] = {0};
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      const lmv_status status =
          lmv_scene_update_animation(scene, mid, error, sizeof(error));
      CHECK(status == LMV_OK);
      bool changed = false;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        if (lmv_scene_draw(scene, i, &draw) != LMV_OK) continue;
        if (draw.visible != before[i]) changed = true;
      }
      CHECK(changed);
      lmv_scene_free(scene);
    }
  }

  if (proto_path) {
    lmv_scene* scene = Load(proto_path);
    if (scene) {
      std::vector<int32_t> before;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        before.push_back(lmv_scene_draw(scene, i, &draw) == LMV_OK
                             ? draw.mesh_id
                             : -1);
      }
      char error[512] = {0};
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      CHECK(lmv_scene_update_animation(scene, mid, error, sizeof(error)) ==
            LMV_OK);
      bool changed = false;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        if (lmv_scene_draw(scene, i, &draw) != LMV_OK) continue;
        if (draw.mesh_id != before[i]) changed = true;
      }
      CHECK(changed);
      lmv_scene_free(scene);
    }
  }

  if (count_path) {
    lmv_scene* scene = Load(count_path);
    if (scene) {
      CHECK(lmv_scene_draw_count(scene) >= 3);
      std::vector<uint32_t> before;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        before.push_back(lmv_scene_draw(scene, i, &draw) == LMV_OK
                             ? draw.visible
                             : 0u);
      }
      char error[512] = {0};
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      CHECK(lmv_scene_update_animation(scene, mid, error, sizeof(error)) ==
            LMV_OK);
      bool appeared = false;
      std::vector<uint32_t> mid_visible;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        const bool ok = lmv_scene_draw(scene, i, &draw) == LMV_OK;
        mid_visible.push_back(ok ? draw.visible : 0u);
        if (ok && draw.visible && !before[i]) appeared = true;
      }
      CHECK(appeared);
      CHECK(lmv_scene_update_animation(scene, lmv_scene_end_time(scene), error,
                                       sizeof(error)) == LMV_OK);
      bool disappeared = false;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        if (lmv_scene_draw(scene, i, &draw) != LMV_OK) continue;
        if (!draw.visible && mid_visible[i]) disappeared = true;
      }
      CHECK(disappeared);
      lmv_scene_free(scene);
    }
  }

  if (points_path) {
    lmv_scene* scene = Load(points_path);
    if (scene) {
      char error[512] = {0};
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      // Points/curves are baked into splats/ribbons; animated transforms on
      // them or their ancestors must use the re-conversion fallback.
      CHECK(lmv_scene_update_animation(scene, mid, error, sizeof(error)) ==
            LMV_ERR_UNSUPPORTED);
      CHECK(lmv_scene_set_time(scene, mid, error, sizeof(error)) == LMV_OK);
      lmv_scene_free(scene);
    }
  }

  if (material_path) {
    lmv_scene* scene = Load(material_path);
    if (scene) {
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      char error[512] = {0};
      // Animated material inputs are not owned by the fast path: force a full
      // re-conversion instead of silently rendering a stale material.
      CHECK(lmv_scene_update_animation(scene, mid, error, sizeof(error)) ==
            LMV_ERR_UNSUPPORTED);
      CHECK(lmv_scene_set_time(scene, mid, error, sizeof(error)) == LMV_OK);
      CHECK(lmv_scene_mesh_count(scene) > 0);
      lmv_scene_free(scene);
    }
  }

  if (visibility_path) {
    lmv_scene* scene = Load(visibility_path);
    if (scene) {
      CHECK(lmv_scene_draw_count(scene) > 0);
      std::vector<uint32_t> before;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        before.push_back(lmv_scene_draw(scene, i, &draw) == LMV_OK
                             ? draw.visible
                             : 0u);
      }
      char error[512] = {0};
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      const lmv_status status =
          lmv_scene_update_animation(scene, mid, error, sizeof(error));
      CHECK(status == LMV_OK);
      bool changed = false;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        if (lmv_scene_draw(scene, i, &draw) != LMV_OK) continue;
        if (draw.visible != before[i]) changed = true;
      }
      CHECK(changed);
      lmv_scene_free(scene);
    }
  }

  if (instancer_path) {
    lmv_scene* scene = Load(instancer_path);
    if (scene) {
      CHECK(lmv_scene_draw_count(scene) > 0);
      std::vector<std::vector<float>> before;
      before.reserve(lmv_scene_draw_count(scene));
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene); ++i) {
        lmv_draw_view draw{};
        std::vector<float> world(16, 0.0f);
        if (lmv_scene_draw(scene, i, &draw) == LMV_OK && draw.world) {
          std::memcpy(world.data(), draw.world, 16 * sizeof(float));
        }
        before.push_back(std::move(world));
      }
      const double mid =
          (lmv_scene_start_time(scene) + lmv_scene_end_time(scene)) * 0.5;
      char error[512] = {0};
      const lmv_status status =
          lmv_scene_update_animation(scene, mid, error, sizeof(error));
      CHECK(status == LMV_OK);
      bool changed = false;
      for (uint32_t i = 0; i < lmv_scene_draw_count(scene) && !changed; ++i) {
        lmv_draw_view draw{};
        if (lmv_scene_draw(scene, i, &draw) != LMV_OK || !draw.world) continue;
        for (int k = 0; k < 16; ++k) {
          if (std::fabs(draw.world[k] - before[i][k]) > 1.0e-5f) {
            changed = true;
            break;
          }
        }
      }
      CHECK(changed);
      lmv_scene_free(scene);
    }
  }

  if (camera_path) {
    lmv_scene* scene = Load(camera_path);
    if (scene) {
      CHECK(lmv_scene_camera_count(scene) > 0);
      float first[16] = {0};
      lmv_camera_view authored{};
      CHECK(lmv_scene_camera(scene, 0, &authored) == LMV_OK);
      if (authored.transform) {
        std::memcpy(first, authored.transform, sizeof(first));
      }
      const float first_focal = authored.focal_length;
      char error[512] = {0};
      const double end = lmv_scene_end_time(scene);
      const lmv_status status =
          lmv_scene_update_animation(scene, end, error, sizeof(error));
      CHECK(status == LMV_OK);
      CHECK(lmv_scene_camera(scene, 0, &authored) == LMV_OK);
      bool changed = false;
      if (authored.transform) {
        for (int k = 0; k < 16; ++k) {
          if (std::fabs(authored.transform[k] - first[k]) > 1.0e-5f) {
            changed = true;
          }
        }
      }
      CHECK(changed);
      CHECK(std::fabs(authored.focal_length - first_focal) > 1.0e-5f);
      lmv_scene_free(scene);
    }
  }

  if (light_path) {
    lmv_scene* scene = Load(light_path);
    if (scene) {
      CHECK(lmv_scene_light_count(scene) > 0);
      float first[16] = {0};
      lmv_light_view light{};
      CHECK(lmv_scene_light(scene, 0, &light) == LMV_OK);
      if (light.transform) std::memcpy(first, light.transform, sizeof(first));
      const float first_intensity = light.intensity;
      char error[512] = {0};
      const double end = lmv_scene_end_time(scene);
      const lmv_status status =
          lmv_scene_update_animation(scene, end, error, sizeof(error));
      CHECK(status == LMV_OK);
      CHECK(lmv_scene_light(scene, 0, &light) == LMV_OK);
      bool changed = false;
      if (light.transform) {
        for (int k = 0; k < 16; ++k) {
          if (std::fabs(light.transform[k] - first[k]) > 1.0e-5f) changed = true;
        }
      }
      CHECK(changed);
      CHECK(std::fabs(light.intensity - first_intensity) > 1.0e-5f);
      lmv_scene_free(scene);
    }
  }
}

// Texture-format fixtures: UDIM, Ptex, EXR and KTX2 companions.
void TestFormatTextures(const char* udim_path, const char* ptex_path,
                        const char* exr_path, const char* ktx2_path,
                        const char* ktx2_bc1_path) {
  struct Expect {
    const char* path;
    bool udim = false;
    bool ptex = false;
    uint32_t udim_tiles = 0;
    uint32_t ptex_faces = 0;
    uint32_t min_decoded_images = 1;
    bool compressed = false;
  };
  const Expect cases[] = {
      {udim_path, true, false, 4, 0, 1, false},
      {ptex_path, false, true, 0, 4, 1, false},
      {exr_path, false, false, 0, 0, 1, false},
      {ktx2_path, false, false, 0, 0, 1, false},
      {ktx2_bc1_path, false, false, 0, 0, 1, true},
  };
  for (const Expect& expect : cases) {
    if (!expect.path || !*expect.path) continue;
    lmv_scene* scene = Load(expect.path);
    if (!scene) continue;
    CHECK(lmv_scene_texture_count(scene) > 0);
    bool saw_format = false;
    int decoded_images = 0;
    for (uint32_t i = 0; i < lmv_scene_texture_count(scene); ++i) {
      lmv_texture_view texture{};
      if (lmv_scene_texture(scene, i, &texture) != LMV_OK) continue;
      if (expect.udim && texture.udim_tile_count == expect.udim_tiles) {
        saw_format = true;
        CHECK(texture.udim_tiles != nullptr);
      }
      if (expect.ptex && texture.is_ptex != 0) {
        saw_format = true;
        CHECK(texture.ptex_face_count >= expect.ptex_faces);
        CHECK(texture.ptex_rects != nullptr);
      }
      if (texture.image_id >= 0) {
        lmv_image_view image{};
        if (lmv_scene_image(scene, static_cast<uint32_t>(texture.image_id),
                            &image) == LMV_OK &&
            image.width > 0 &&
            (image.pixels || image.compressed_format != 0)) {
          ++decoded_images;
          if (expect.compressed) {
            CHECK(image.compressed_format != 0);
            CHECK(image.mip_levels > 0);
            CHECK(image.levels != nullptr);
          }
        }
      }
    }
    if (expect.udim || expect.ptex) {
      CHECK(saw_format);
    }
    if (decoded_images < static_cast<int>(expect.min_decoded_images)) {
      std::fprintf(stderr, "format fixture %s decoded %d image(s)\n",
                   expect.path, decoded_images);
    }
    CHECK(decoded_images >= static_cast<int>(expect.min_decoded_images));
    lmv_scene_free(scene);
  }
}

void TestCamera() {
  lmv_camera* camera = lmv_camera_create();
  CHECK(camera != nullptr);
  if (!camera) return;
  lmv_camera_set_aspect(camera, 16.0f / 9.0f);
  lmv_camera_set_up_axis(camera, 0);
  const float low[3] = {-1.0f, -1.0f, -1.0f};
  const float high[3] = {1.0f, 1.0f, 1.0f};
  lmv_camera_frame_bounds(camera, low, high);
  lmv_camera_orbit(camera, 5.0f, 3.0f);
  lmv_camera_pan(camera, 2.0f, 1.0f);
  lmv_camera_dolly(camera, 0.5f);

  lmv_camera_frame frame{};
  lmv_camera_get_frame(camera, &frame);
  CHECK(FiniteMatrix(frame.view));
  CHECK(FiniteMatrix(frame.proj));
  CHECK(std::isfinite(frame.eye[0]) && std::isfinite(frame.eye[1]) &&
        std::isfinite(frame.eye[2]));
  CHECK(frame.near_clip > 0.0f);
  CHECK(frame.far_clip > frame.near_clip);
  lmv_camera_destroy(camera);
}

}  // namespace

int main(int argc, char** argv) {
  TestAbi();
  TestCamera();
  if (argc > 1) TestSimpleScene(argv[1]);
  if (argc > 2) TestTextureDecode(argv[2]);
  if (argc > 3) TestAnimationScrub(argv[3]);
  if (argc > 8) {
    TestFormatTextures(argv[4], argv[5], argv[6], argv[7], argv[8]);
  }
  TestFastAnimation(argc > 3 ? argv[3] : nullptr,
                    argc > 9 ? argv[9] : nullptr,
                    argc > 10 ? argv[10] : nullptr,
                    argc > 11 ? argv[11] : nullptr,
                    argc > 12 ? argv[12] : nullptr,
                    argc > 13 ? argv[13] : nullptr,
                    argc > 14 ? argv[14] : nullptr,
                    argc > 15 ? argv[15] : nullptr,
                    argc > 16 ? argv[16] : nullptr,
                    argc > 17 ? argv[17] : nullptr,
                    argc > 18 ? argv[18] : nullptr);
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("lmetalview core: all checks passed\n");
  return 0;
}
