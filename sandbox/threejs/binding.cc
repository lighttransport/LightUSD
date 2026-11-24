#include <emscripten/bind.h>

#include <vector>

#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"

using namespace emscripten;

namespace detail {

// To RGBA
bool ToRGBA(const std::vector<uint8_t> &src, int channels,
  std::vector<uint8_t> &dst) {

  uint32_t npixels = src.size() / channels;
  dst.resize(npixels * 4);

  if (channels == 1) { // grayscale
    for (size_t i = 0; i < npixels; i++) {
      dst[4 * i + 0] = src[i];
      dst[4 * i + 1] = src[i];
      dst[4 * i + 2] = src[i];
      dst[4 * i + 3] = 1.0f;
    }
  } else if (channels == 2) { // assume luminance + alpha
    for (size_t i = 0; i < npixels; i++) {
      dst[4 * i + 0] = src[2*i+0];
      dst[4 * i + 1] = src[2*i+0];
      dst[4 * i + 2] = src[2*i+0];
      dst[4 * i + 3] = src[2*i+1];
    }
  } else if (channels == 3) {
    for (size_t i = 0; i < npixels; i++) {
      dst[4 * i + 0] = src[3*i+0];
      dst[4 * i + 1] = src[3*i+1];
      dst[4 * i + 2] = src[3*i+2];
      dst[4 * i + 3] = 1.0f;
    }
  } else if (channels == 4) {
    dst = src;
  } else {
    return false;
  }

  return true;
}

}

///
/// Simple C++ wrapper class for Emscripten
///
class LightUSDLoader {
 public:
  ///
  /// `binary` is the buffer for LightUSD binary(e.g. buffer read by
  /// fs.readFileSync) std::string can be used as UInt8Array in JS layer.
  ///
  LightUSDLoader(const std::string &binary) {
    lightusd::legacy::Stage stage;

    loaded_ = lightusd::legacy::LoadUSDFromMemory(
        reinterpret_cast<const uint8_t *>(binary.c_str()), binary.size(),
        "dummy.usda", &stage, &warn_, &error_);

    bool is_usdz = lightusd::legacy::IsUSDZ(
        reinterpret_cast<const uint8_t *>(binary.c_str()), binary.size());

    lightusd::legacy::tydra::RenderSceneConverterEnv env(stage);

    env.material_config.preserve_texel_bitdepth = true;

    if (is_usdz) {
      // Setup AssetResolutionResolver to read a asset(file) from memory.
      bool asset_on_memory =
          false;  // duplicate asset data from USDZ(binary) to UDSZAsset struct.

      if (!lightusd::legacy::ReadUSDZAssetInfoFromMemory(
              reinterpret_cast<const uint8_t *>(binary.c_str()), binary.size(),
              asset_on_memory, &usdz_asset_, &warn_, &error_)) {
        std::cerr << "Failed to read USDZ assetInfo. \n";
        loaded_ = false;
      }

      lightusd::legacy::AssetResolutionResolver arr;

      // NOTE: Pointer address of usdz_asset must be valid until the call of
      // RenderSceneConverter::ConvertToRenderScene.
      if (!lightusd::legacy::SetupUSDZAssetResolution(arr, &usdz_asset_)) {
        std::cerr << "Failed to setup AssetResolution for USDZ asset\n";
        loaded_ = false;
      }

      env.asset_resolver = arr;
    }

    // RenderScene: Scene graph object which is suited for GL/Vulkan renderer
    lightusd::legacy::tydra::RenderSceneConverter converter;

    // env.timecode = timecode; // TODO
    loaded_ = converter.ConvertToRenderScene(env, &render_scene_);
    if (!loaded_) {
      std::cerr << "Failed to convert USD Stage to RenderScene: \n"
                << converter.GetError() << "\n";
    }
  }
  ~LightUSDLoader() {}

  int numMeshes() const {
    return render_scene_.meshes.size();
  }

  emscripten::val getMaterial(int mat_id) const {

    emscripten::val mat = emscripten::val::object();

    if (!loaded_) {
      return mat;
    }

    if (mat_id >= render_scene_.materials.size()) {
      return mat;
    }

    const auto &m = render_scene_.materials[mat_id];

    if (m.surfaceShader.diffuseColor.is_texture()) {
      mat.set("diffuseColorTextureId", m.surfaceShader.diffuseColor.texture_id);
    } else {
      // TODO
      //mat.set("diffuseColor", m.surfaceShader.diffuseColor);
    }

    return mat;
  }

  emscripten::val getTexture(int tex_id) const {

    emscripten::val tex = emscripten::val::object();

    if (!loaded_) {
      return tex;
    }

    if (tex_id >= render_scene_.textures.size()) {
      return tex;
    }

    const auto &t = render_scene_.textures[tex_id];

    tex.set("textureImageId", int(t.texture_image_id));
    //tex.set("wrapS", to_string(t.wrapS));
    //tex.set("wrapT", to_string(t.wrapT));
    // TOOD: bias, scale, rot/scale/trans

    return tex;
  }

  emscripten::val getImage(int img_id) const {

    emscripten::val img = emscripten::val::object();

    if (!loaded_) {
      return img;
    }

    if (img_id >= render_scene_.images.size()) {
      return img;
    }

    const auto &i = render_scene_.images[img_id];

    if ((i.buffer_id >= 0) && (i.buffer_id < render_scene_.buffers.size())) {
      const auto &b = render_scene_.buffers[i.buffer_id];

      // TODO: Support HDR

      img.set("data", emscripten::typed_memory_view(b.data.size(), b.data.data()));
      img.set("width", int(i.width));
      img.set("height", int(i.height));
      img.set("channels", int(i.channels));
    }


    return img;
  }

  emscripten::val getMesh(int mesh_id) const {
    emscripten::val mesh = emscripten::val::object();

    if (!loaded_) {
      return mesh;
    }

    if (mesh_id >= render_scene_.meshes.size()) {
      return mesh;
    }

    const lightusd::legacy::tydra::RenderMesh &rmesh = render_scene_.meshes[size_t(mesh_id)];

    // TODO: Use three.js scene description format?
    mesh.set("prim_name", rmesh.prim_name);
    mesh.set("display_name", rmesh.prim_name);
    mesh.set("abs_path", rmesh.abs_path);
    const uint32_t *indices_ptr = rmesh.faceVertexIndices().data();
    mesh.set("faceVertexIndices", emscripten::typed_memory_view(rmesh.faceVertexIndices().size(), indices_ptr));
    const uint32_t *counts_ptr = rmesh.faceVertexCounts().data();
    mesh.set("faceVertexCounts", emscripten::typed_memory_view(rmesh.faceVertexCounts().size(), counts_ptr));
    const float *points_ptr = reinterpret_cast<const float *>(rmesh.points.data());
    // vec3
    mesh.set("points", emscripten::typed_memory_view(rmesh.points.size() * 3, points_ptr));

    {
      // slot 0 hardcoded.
      uint32_t uvSlotId = 0;
      if (rmesh.texcoords.count(uvSlotId)) {
        const float *uvs_ptr = reinterpret_cast<const float *>(rmesh.texcoords.at(uvSlotId).data.data());

        // assume vec2
        mesh.set("texcoords", emscripten::typed_memory_view(rmesh.texcoords.at(uvSlotId).vertex_count() * 2, uvs_ptr));
      }
    }

    mesh.set("materialId", rmesh.material_id);


    return mesh;
  }

  bool ok() const { return loaded_; }

  const std::string error() const { return error_; }

 private:
  bool loaded_{false};
  std::string warn_;
  std::string error_;

  lightusd::legacy::tydra::RenderScene render_scene_;
  lightusd::legacy::USDZAsset usdz_asset_;
};

// Register STL
EMSCRIPTEN_BINDINGS(stl_wrappters) {
  register_vector<float>("VectorFloat");
  register_vector<int>("VectorInt");
  register_vector<uint32_t>("VectorUInt");
}

EMSCRIPTEN_BINDINGS(lightusd_module) {
  class_<LightUSDLoader>("LightUSDLoader")
      .constructor<const std::string &>()
      .function("getMesh", &LightUSDLoader::getMesh)
      .function("numMeshes", &LightUSDLoader::numMeshes)
      .function("getMaterial", &LightUSDLoader::getMaterial)
      .function("getTexture", &LightUSDLoader::getTexture)
      .function("getImage", &LightUSDLoader::getImage)
      .function("ok", &LightUSDLoader::ok)
      .function("error", &LightUSDLoader::error);
}
