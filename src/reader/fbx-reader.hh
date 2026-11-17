// SPDX-License-Identifier: Apache-2.0
#pragma once
// FBX import through ufbx (src/external/ufbx): meshes, skin clusters,
// skeleton and animation -> UsdGeom/UsdSkel layers. Also reads the skeleton
// and animation of an OBJ's skel_lib/skel_anim sidecar (skel-obj.md).
#include <string>
#include <vector>

#include "layer/layer.hh"
#include "reader/skel-author.hh"
#include "tydra/asset-import/import.hh"

namespace lightusd {

struct FBXMesh {
  std::string name;
  std::vector<float> points;  // world space at bind time, xyz
  std::vector<int32_t> counts, indices;
  std::vector<float> normals, uvs;  // face-varying tables
  std::vector<int32_t> normal_indices, uv_indices;
  std::vector<int32_t> joint_indices;  // influences per point
  std::vector<float> joint_weights;
  int32_t influences = 0;
  // Blend shape channels (ufbx blend deformers). `name` is the channel token
  // (unique in the scene); offsets are in the world space of `points`.
  std::vector<skel::BlendShape> blend_shapes;
};

struct FBXScene {
  skel::Skeleton skeleton;
  bool has_animation = false;
  skel::Animation animation;
  std::vector<FBXMesh> meshes;
};

// Parse FBX bytes (Y-up, meters). `take` selects an animation stack by name
// (empty: the first one). `with_meshes` = false reads only the skeleton and
// animation. `max_frames` bounds the sampled animation.
bool ReadFBXScene(const uint8_t* data, size_t size, const std::string& take,
                  bool with_meshes, FBXScene* out, std::string* warn, std::string* err,
                  size_t max_frames = 100000);

std::shared_ptr<Layer> LoadFBXLayerFromMemory(const std::string& source, const uint8_t* data,
                                              size_t size,
                                              const tydra::import_asset::Options& options,
                                              std::string* warn, std::string* err);
std::shared_ptr<Layer> LoadFBXLayerFromFile(const std::string& source,
                                            const tydra::import_asset::Options& options,
                                            std::string* warn, std::string* err);

}  // namespace lightusd
