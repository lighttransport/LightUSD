// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>
#include "lightusd-c.h"

namespace lightusd {
class Stage;
}

namespace lusdview {

struct VcharControl {
  std::string name;
  std::string blendshape;
  float minimum{-1.0f};
  float maximum{1.0f};
  float defaultValue{0.0f};
};

std::vector<VcharControl> ReadVcharControls(const lightusd::Stage& stage);
std::vector<VcharControl> ReadVcharControls(const lightusd_stage* stage);

}  // namespace lusdview
