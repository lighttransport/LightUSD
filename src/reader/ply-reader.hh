// SPDX-License-Identifier: Apache-2.0
//
// usdPly-style layer reader for 3D Gaussian splat PLY files (the reference
// 3DGS / INRIA layout, as written by gsplat/nerfstudio/PostShot and imported
// by Blender 5.3). A `.ply` layer contains one ParticleField3DGaussianSplat
// prim `/Splats` (the default prim), so it can be opened directly or used as
// a reference/payload target: @splats.ply@.
//
// Decoding (matches the reference 3DGS code and Blender's PLY importer):
//   x y z                       -> positions
//   rot_0..3 (w x y z)          -> orientations (normalized)
//   scale_0..2 (log)            -> scales = exp(scale)
//   opacity (logit)             -> opacities = sigmoid(opacity)
//   f_dc_0..2, f_rest_* (channel-major: all R, then G, then B)
//                               -> radiance:sphericalHarmonicsCoefficients,
//                                  (degree+1)^2 RGB triples per particle
// Binary little/big endian and ASCII are accepted; any numeric property type
// is converted to float.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "layer/layer.hh"

namespace lightusd {

std::shared_ptr<Layer> LoadPLYLayerFromMemory(const std::string& source,
                                              const uint8_t* data, size_t size,
                                              std::string* warn,
                                              std::string* err);
std::shared_ptr<Layer> LoadPLYLayerFromFile(const std::string& path,
                                            std::string* warn,
                                            std::string* err);

}  // namespace lightusd
