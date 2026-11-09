// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Round-trip fidelity test
// Verifies that a rich scene survives USDA->write->USDA and USDC->read->USDC
// round-trips with identical structure, values, metadata, dictionaries and
// per-property metadata.

#include <iostream>
#include <cassert>
#include <string>
#include <vector>

#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"

using namespace lightusd::next;

static const char* kScene = R"USD(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
    framesPerSecond = 24
    customLayerData = {
        string creator = "lightusd"
        dictionary stats = {
            int prims = 2
        }
    }
)

def Xform "World" (
    kind = "component"
    displayName = "Root"
    customData = {
        string note = "hello"
        int count = 7
    }
)
{
    def Mesh "Mesh" (
        assetInfo = {
            string name = "M"
        }
    )
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        color3f[] primvars:displayColor = [(1, 0, 0)] (
            interpolation = "constant"
            elementSize = 1
        )
        token[] joints = ["root", "hip"]
        string[] labels = ["a", "b", "c"]
        double scale = 2.5
        rel material:binding = </World/Mat>
    }

    def Material "Mat" {
        token outputs:surface
    }
}
)USD";

// Asserts the structural / value facts that every backing format must preserve.
static void CheckCore(const Stage& s, const char* via) {
  std::vector<UsdPrim> roots = s.GetRootPrims();
  assert(roots.size() == 1);
  UsdPrim world = roots[0];
  assert(world.GetName() == "World");
  assert(world.GetTypeName() == "Xform");
  assert(world.GetChildCount() == 2);

  UsdPrim mesh = world.GetChild("Mesh");
  assert(mesh && mesh.GetTypeName() == "Mesh");

  const Value* pts = mesh.GetPropertyValue("points");
  assert(pts && pts->as_float_array() && pts->as_float_array()->size() == 9);

  const Value* joints = mesh.GetPropertyValue("joints");
  assert(joints && joints->as_token_array() &&
         joints->as_token_array()->size() == 2 &&
         (*joints->as_token_array())[1] == "hip");

  const Value* labels = mesh.GetPropertyValue("labels");
  assert(labels && labels->as_token_array() &&
         labels->as_token_array()->size() == 3);

  const Value* scale = mesh.GetPropertyValue("scale");
  assert(scale && scale->as_double() && *scale->as_double() == 2.5);

  const std::vector<Path>* bind = mesh.GetRelationship("material:binding");
  assert(bind && bind->size() == 1 && (*bind)[0].str() == "/World/Mat");

  std::cout << "    core fidelity ok (" << via << ")" << std::endl;
}

// Asserts the metadata / dictionary / per-property-metadata facts.
static void CheckMeta(const Stage& s, const char* via) {
  // Stage-level metadata (framesPerSecond + customLayerData dict).
  assert(s.GetRootLayer());
  const LayerMeta& lm = s.GetRootLayer()->meta();
  assert(lm.framesPerSecond_set && lm.framesPerSecond == 24);
  assert(lm.customLayerData.is_dictionary());
  auto cld = lm.customLayerData.as_dictionary();
  assert(cld && cld->find("creator") && cld->find("creator")->as_string() &&
         *cld->find("creator")->as_string() == "lightusd");
  auto stats = cld->find("stats");
  assert(stats && stats->is_dictionary() &&
         stats->as_dictionary()->find("prims") &&
         *stats->as_dictionary()->find("prims")->as_int() == 2);

  UsdPrim world = s.GetRootPrims()[0];
  assert(world.GetMeta().kind() == "component");
  assert(world.GetMeta().displayName() == "Root");

  assert(world.GetMeta().customData().is_dictionary());
  auto wcd = world.GetMeta().customData().as_dictionary();
  assert(wcd && wcd->find("note") && wcd->find("note")->as_string() &&
         *wcd->find("note")->as_string() == "hello");
  assert(wcd->find("count") && wcd->find("count")->as_int() &&
         *wcd->find("count")->as_int() == 7);

  UsdPrim mesh = world.GetChild("Mesh");
  assert(mesh.GetMeta().assetInfo().is_dictionary());

  const PrimSpec* mspec = mesh.GetPrimSpec();
  assert(mspec);
  const PropMeta* pm = mspec->property_meta("primvars:displayColor");
  assert(pm && (pm->authored & PropMeta::kInterpolation));
  assert(pm->interpolation == "constant");
  assert(pm->authored & PropMeta::kElementSize);
  assert(pm->elementSize == 1);

  std::cout << "    metadata/dict/prop-meta fidelity ok (" << via << ")"
            << std::endl;
}

static Stage ParseUSDA(const char* src) {
  LoadResult r = LoadUSDAFromString(src);
  if (!r.success) {
    std::cerr << "USDA parse failed: " << r.error_summary << std::endl;
    assert(false);
  }
  return std::move(r.stage);
}

void test_usda_roundtrip() {
  std::cout << "Testing USDA round-trip fidelity..." << std::endl;
  Stage s0 = ParseUSDA(kScene);
  CheckCore(s0, "usda parse");
  CheckMeta(s0, "usda parse");

  std::string out = WriteUSDAToString(s0);
  Stage s1 = ParseUSDA(out.c_str());
  CheckCore(s1, "usda roundtrip");
  CheckMeta(s1, "usda roundtrip");
  std::cout << "  USDA round-trip passed!" << std::endl;
}

void test_usdc_roundtrip() {
  std::cout << "Testing USDC round-trip fidelity..." << std::endl;
  Stage s0 = ParseUSDA(kScene);

  std::vector<uint8_t> buf;
  USDCWriteResult wr = WriteUSDCToMemory(buf, s0);
  if (!wr.success) {
    std::cerr << "USDC write failed: " << wr.error << std::endl;
    assert(false);
  }
  assert(!buf.empty());

  USDCLoadResult lr = LoadUSDCFromMemory(buf.data(), buf.size());
  if (!lr.success) {
    std::cerr << "USDC read failed: " << lr.error_summary << std::endl;
    assert(false);
  }
  CheckCore(lr.stage, "usdc roundtrip");
  CheckMeta(lr.stage, "usdc roundtrip");
  std::cout << "  USDC round-trip passed!" << std::endl;
}

int main() {
  std::cout << "=== LightUSD Next Round-trip Fidelity Tests ===" << std::endl;
  std::cout << std::endl;
  try {
    test_usda_roundtrip();
    test_usdc_roundtrip();
    std::cout << std::endl << "All round-trip fidelity tests passed!" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  }
}
