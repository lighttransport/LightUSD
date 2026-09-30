// Comprehensive LightUSD `next` API example.
//
// Demonstrates, in one pass: loading and composition, stage traversal,
// typed schema access, attribute evaluation at a timecode, and writing the
// result back out as USDA and USDC.
//
// Build: see examples/next_workflows/CMakeLists.txt (target: comprehensive_example)
// Run:   comprehensive_example <file.usd[ac]>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "next/eval/attribute-eval.hh"
#include "next/load-usd.hh"
#include "next/lightusd-next.hh"
#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-xform.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"

using namespace lightusd::next;

static void PrintStageInfo(const Stage& stage) {
  std::cout << "Stage loaded successfully\n";
  std::cout << "  Root layer: " << (stage.GetRootLayer() ? "present" : "absent")
            << "\n";

  size_t prim_count = 0;
  stage.Traverse([&prim_count](const UsdPrim&) {
    ++prim_count;
    return true;
  });
  std::cout << "  Total prims: " << prim_count << "\n";
}

static void DemonstrateSchemaAccess(const Stage& stage) {
  std::cout << "\n=== Schema Access ===\n";

  size_t mesh_count = 0;
  stage.Traverse([&mesh_count](const UsdPrim& prim) {
    const std::string& type = prim.GetTypeName();
    const std::string path = prim.GetPath().str();

    if (type == "Mesh") {
      UsdGeomMesh mesh(prim);
      if (mesh.IsValid()) {
        ++mesh_count;
        const std::vector<float> points = mesh.GetPoints();
        // points is a flat xyz triple array.
        std::cout << "  Mesh " << path << ": " << (points.size() / 3)
                  << " points\n";
      }
    } else if (type == "Xform") {
      UsdGeomXform xform(prim);
      if (xform.IsValid()) {
        std::cout << "  Xform " << path << "\n";
      }
    } else if (type == "Material" || type == "Shader" ||
               type.find("Light") != std::string::npos) {
      std::cout << "  " << type << " " << path << "\n";
    }
    return true;
  });

  if (mesh_count == 0) {
    std::cout << "  (no Mesh prims found)\n";
  }
}

static void DemonstrateEvaluation(const Stage& stage) {
  std::cout << "\n=== Attribute Evaluation (t = 1.0) ===\n";

  AttributeEval eval(&stage);
  eval.SetTime(1.0);

  size_t evaluated = 0;
  stage.Traverse([&](const UsdPrim& prim) {
    if (evaluated >= 5) {
      return false;  // stop the traversal
    }
    for (const std::string& name : prim.GetPropertyNames()) {
      const EvalResult result = eval.Eval(prim, name);
      if (result.success) {
        std::cout << "  " << prim.GetPath().str() << "." << name << "\n";
        ++evaluated;
        if (evaluated >= 5) {
          return false;
        }
      }
    }
    return true;
  });

  if (evaluated == 0) {
    std::cout << "  (no attributes resolved at t=1.0)\n";
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <file.usd[ac]>\n";
    return EXIT_FAILURE;
  }

  const std::string filename = argv[1];
  std::cout << "Loading: " << filename << "\n";

  // LoadUSDOptions is fail-closed by default (InputPolicy::Untrusted), which
  // is the right posture for untrusted input. Compose the stage so that
  // references, payloads and variants are resolved before traversal.
  LoadUSDOptions options;
  Stage stage;
  std::string warn;
  std::string err;
  if (!LoadUSDComposed(filename, &stage, &warn, &err)) {
    std::cerr << "Failed to load: " << filename << "\n";
    if (!err.empty()) {
      std::cerr << "  error: " << err << "\n";
    }
    if (!warn.empty()) {
      std::cerr << "  warning: " << warn << "\n";
    }
    return EXIT_FAILURE;
  }
  if (!warn.empty()) {
    std::cout << "  warning: " << warn << "\n";
  }

  PrintStageInfo(stage);
  DemonstrateSchemaAccess(stage);
  DemonstrateEvaluation(stage);

  std::cout << "\n=== Writing ===\n";

  USDAWriteOptions usda_opts;
  usda_opts.compact = false;
  usda_opts.float_precision = 6;
  const USDAWriteResult usda_result =
      WriteUSDAToFile("output.usda", stage, usda_opts);
  if (usda_result.success) {
    std::cout << "  Wrote output.usda\n";
  } else {
    std::cerr << "  Failed to write output.usda: " << usda_result.error << "\n";
  }

  const USDCWriteResult usdc_result =
      WriteUSDCToFile("output.usdc", stage, USDCWriteOptions());
  if (usdc_result.success) {
    std::cout << "  Wrote output.usdc\n";
  } else {
    std::cerr << "  Failed to write output.usdc: " << usdc_result.error << "\n";
  }

  std::cout << "\nDone.\n";
  return EXIT_SUCCESS;
}
