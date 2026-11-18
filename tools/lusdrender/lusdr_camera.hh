// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lusdr_types.hh"
#include "light3d/math.h"
namespace lusdr {
inline light3d::Mat4 CameraProjectionZeroOne(const CameraFrame& camera, float aspect) {
  const float zn=std::max(camera.znear,1.0e-5f), zf=std::max(camera.zfar,zn+1.0e-5f);
  if (!camera.ortho) {
    auto p=light3d::perspectiveZeroOne(camera.yfov,aspect,zn,zf);
    p.m[8]=camera.shift_x; p.m[9]=camera.shift_y;
    return p;
  }
  light3d::Mat4 p;
  p.m[0]=2.0f/std::max(camera.xmag,1.0e-6f);
  p.m[5]=2.0f/std::max(camera.ymag,1.0e-6f);
  p.m[10]=-1.0f/(zf-zn); p.m[14]=-zn/(zf-zn); p.m[15]=1.0f;
  p.m[12]=-camera.shift_x; p.m[13]=-camera.shift_y;
  return p;
}
}
