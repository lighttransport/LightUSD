// SPDX-License-Identifier: Apache-2.0
#include "lightusd-cpp.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: animation_sampling FILE /Prim.attribute\n";
    return 2;
  }
  const std::string property_path(argv[2]);
  const size_t dot = property_path.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == property_path.size())
    return 2;

  lightusd::api::Stage stage;
  if (stage.load(argv[1]) != LIGHTUSD_OK) {
    std::cerr << lightusd::api::LastError() << '\n';
    return 1;
  }
  const std::string prim_path = property_path.substr(0, dot);
  const std::string property_name = property_path.substr(dot + 1);
  lightusd::api::Prim prim = stage.prim(prim_path.c_str());
  if (!prim) return 2;

  for (uint8_t mode : {uint8_t(0), uint8_t(1)}) {
    for (double time : {(std::numeric_limits<double>::quiet_NaN)(),
                        0.0, 0.5, 1.0}) {
      lightusd::api::Value value;
      if (prim.evaluate(property_name.c_str(), time, mode, true, &value) !=
          LIGHTUSD_OK) {
        std::cerr << lightusd::api::LastError() << '\n';
        return 1;
      }
      lightusd::api::String printed;
      if (lightusd::api::ValueToUSDA(value, &printed) != LIGHTUSD_OK) {
        std::cerr << lightusd::api::LastError() << '\n';
        return 1;
      }
      const lightusd_sv text = lightusd::api::StringView(printed);
      std::cout << (mode == 0 ? "held " : "linear ")
                << (std::isnan(time) ? "default" : std::to_string(time))
                << " = " << std::string_view(text.data, text.len) << '\n';
    }
  }

  std::vector<lightusd::api::Prim> pending;
  for (size_t i = 0; i < stage.root_prim_count(); ++i)
    pending.push_back(stage.root_prim(i));
  while (!pending.empty()) {
    const lightusd::api::Prim current = std::move(pending.back());
    pending.pop_back();
    const lightusd_sv type = current.type_name();
    if (std::string_view(type.data, type.len) == "SkelAnimation") {
      size_t joints = 0;
      if (current.skel_animation_joint_count_at_time(0.5, &joints) == LIGHTUSD_OK) {
        const lightusd_sv path = current.path();
        std::cout << "skeleton sample " << std::string_view(path.data, path.len)
                  << " joints=" << joints << '\n';
      }
    }
    for (size_t i = 0; i < current.child_count(); ++i)
      pending.push_back(current.child(i));
  }
  return 0;
}
