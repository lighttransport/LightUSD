// obj_subd_dump SCENE LEVEL — refined points of every subdivision mesh, as
// LightUSD renders them (Tydra RenderSceneConverter, tsd refinement at LEVEL).
//
// Prints, per mesh with an authored subdivisionScheme other than "none":
//   shape <prim name> <number of points>
//   x y z
// Used by run_osd_cross.py to compare with OpenSubdiv (osd_refine).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "load-usd.hh"
#include "stage/stage.hh"
#include "tydra/render-converter.hh"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: obj_subd_dump SCENE.usda LEVEL\n");
    return 2;
  }
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(argv[1], &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  if (!warn.empty()) std::fprintf(stderr, "%s", warn.c_str());
  lightusd::tydra::ConverterConfig config;
  config.material.load_textures = false;
  config.animation.enabled = false;
  config.mesh.retain_geometry = true;
  config.mesh.subdivision_level = std::atoi(argv[2]);
  lightusd::tydra::RenderSceneConverter converter(config);
  auto result = converter.Convert(stage);
  if (!result.success) {
    std::fprintf(stderr, "conversion failed\n");
    return 1;
  }
  for (const auto& w : result.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
  for (const auto& mesh : result.scene.meshes) {
    if (mesh.subdivision_face_source.empty()) continue;  // not subdivided
    const std::string& path = mesh.prim_path;
    const std::string name = path.substr(path.find_last_of('/') + 1);
    const std::vector<float> p = mesh.points.flatten();
    // Only points used by faces (hole faces are removed, their points may stay).
    std::vector<char> used(p.size() / 3, 0);
    for (uint32_t v : mesh.face_vertex_indices.flatten())
      if (v < used.size()) used[v] = 1;
    size_t count = 0;
    for (char u : used) count += u;
    std::printf("shape %s %zu\n", name.c_str(), count);
    for (size_t i = 0; i < used.size(); i++)
      if (used[i]) std::printf("%.7g %.7g %.7g\n", p[3 * i], p[3 * i + 1], p[3 * i + 2]);
  }
  return 0;
}
