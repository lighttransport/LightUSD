// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
namespace lusdview {
class Renderer;
class ComparePanel {
 public:
  ComparePanel();
  ~ComparePanel();
  void open(int mode = -1);
  void draw(Renderer*, unsigned long long generation, bool threaded, const char* focusedPath);
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}
