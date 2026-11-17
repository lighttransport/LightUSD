// SPDX-License-Identifier: Apache-2.0
#pragma once
// UsdSkel authoring shared by the FBX importer and the OBJ importer's
// skeleton lines (tinyobjloader skel-obj.md): Skeleton, SkelAnimation and the
// mesh skin binding.
#include <array>
#include <string>
#include <vector>

#include "layer/layer.hh"

namespace lightusd {
namespace skel {

// matrix4d, row-major, translation in the last row (USD convention).
using Mat = std::array<double, 16>;
Mat Identity();
Mat Mul(const Mat& a, const Mat& b);  // a then b (row vectors): v * a * b
bool Invert(const Mat& m, Mat* out);

struct Skeleton {
  std::vector<std::string> names;  // joint names (any text; sanitized for paths)
  std::vector<int> parent;         // index of the parent joint (earlier), or -1
  std::vector<Mat> bind;           // world-space bind transforms
  std::vector<Mat> rest;           // parent-relative rest transforms
};

// Joint-local transforms sampled at `times` (time codes). Rotations are
// real-first quaternions (w, x, y, z), as USD quatf.
struct Animation {
  std::string name;
  double fps = 24.0;
  std::vector<double> times;
  std::vector<std::vector<float>> translations;  // per time: 3 * joints
  std::vector<std::vector<float>> rotations;     // per time: 4 * joints
  std::vector<std::vector<float>> scales;        // per time: 3 * joints
};

// USD joint paths ("a", "a/b", ...) from names and parents: names become
// valid identifiers, made unique among siblings.
std::vector<std::string> JointPaths(const Skeleton& s);

// Define `skel_path` (Skeleton) and, with an animation, `skel_path/Anim`
// (SkelAnimation) with skel:animationSource. Sets the layer's time codes
// per second and range from the animation.
bool AuthorSkeleton(Layer* layer, const std::string& skel_path, const Skeleton& s,
                    const Animation* anim, std::string* err);

// Apply SkelBindingAPI to a mesh and point skel:skeleton at `skel_path`. Skin
// weight primvars are authored by the caller.
void BindMesh(PrimSpec* mesh, const std::string& skel_path);

// Local transform -> (translation, real-first rotation, scale).
void Decompose(const Mat& m, float t[3], float q[4], float s[3]);

}  // namespace skel
}  // namespace lightusd
