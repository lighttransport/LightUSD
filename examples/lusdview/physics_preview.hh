// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "lightusd-cpp.hh"
#include "tydra/rb-dynamics.h"
namespace lusdview {
// Diagnostic rigid-body preview. Owns an editable core stage; document
// snapshots stay immutable. All USD access goes through the public C API.
class PhysicsPreview {
 public:
  PhysicsPreview();
  ~PhysicsPreview();
  PhysicsPreview(const PhysicsPreview&) = delete;
  PhysicsPreview& operator=(const PhysicsPreview&) = delete;
  bool initialize(const lightusd_stage*, double time, std::string* error);
  bool step(const lightusd_stage* authored, double time, int steps, float dt,
            std::string* error);
  void clear();
  const TydraPhysWorld& world() const;
  const lightusd_stage* stage() const;
  const std::vector<std::string>& bodyPaths() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace lusdview
