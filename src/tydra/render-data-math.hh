// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

namespace lightusd { namespace tydra { namespace next {

//
// Basic math types (GPU-aligned)
//

struct alignas(16) Float3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float _pad = 0.0f;

  Float3() = default;
  Float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct Float2 {
  float x = 0.0f, y = 0.0f;

  Float2() = default;
  Float2(float x_, float y_) : x(x_), y(y_) {}
};

struct alignas(16) Float4 {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

  Float4() = default;
  Float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct alignas(64) Matrix4 {
  float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  static Matrix4 Identity() { return Matrix4(); }
  float& operator()(int row, int col) { return m[row * 4 + col]; }
  float operator()(int row, int col) const { return m[row * 4 + col]; }
};

// UsdSkel's bind/rest transforms are matrix4d authored data. Retain these as
// doubles in RenderSkeleton so the public skeleton query does not silently
// round them through the renderer's float-precision transform type.
struct Matrix4d {
  double m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  static Matrix4d Identity() { return Matrix4d(); }
};

}}} // namespace lightusd::tydra::next
