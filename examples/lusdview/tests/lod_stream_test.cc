// SPDX-License-Identifier: Apache-2.0
#include "lod_stream.hh"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  return 1; } } while (false)

int main() {
  namespace fs = std::filesystem;
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path input = fs::temp_directory_path() /
                         ("lusdview-lod-test-" + std::to_string(nonce) + ".usda");
  {
    std::ofstream scene(input);
    scene << R"USD(#usda 1.0
(upAxis = "Z")
def Xform "World" {
  def Xform "Districts" {
    def Xform "Near" {
      def Mesh "Mesh" {
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
      }
    }
    def Xform "Far" {
      double3 xformOp:translate = (0, 0, -100)
      uniform token[] xformOpOrder = ["xformOp:translate"]
      def Mesh "Mesh" {
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
      }
    }
  }
  def Camera "View" {
    double3 xformOp:translate = (0, 0, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
)USD";
    CHECK(scene.good());
  }
  lusdview::LodStreamOptions options;
  options.container = "Districts";
  options.camera = "View";
  options.minVerts = 1;
  options.maxMemGiB = 1;
  options.maxVramGiB = 1;
  options.districtMemGiB = 1;
  options.districtVramGiB = 1;
  const std::string wrapper = lusdview::PrepareLodStream(input.string(), options);
  CHECK(!wrapper.empty());
  std::ifstream generated(wrapper);
  const std::string text((std::istreambuf_iterator<char>(generated)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("upAxis = \"Z\"") != std::string::npos);
  CHECK(text.find("over \"Near\"") != std::string::npos);
  CHECK(text.find("over \"Far\"") == std::string::npos);
  std::error_code error;
  fs::remove(wrapper, error);
  fs::remove(input, error);
}
