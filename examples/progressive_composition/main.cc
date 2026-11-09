#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>

#include "lightusd.hh"
#include "layer.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "pprinter.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "usdGeom.hh"

struct CompositionFeatures {
  bool subLayers{true};
  bool inherits{true};
  bool variantSets{true};
  bool references{true};
  bool payload{true}; // Not 'payloads'
  bool specializes{true};
};

static void CollectMeshPaths(const lightusd::Prim& root,
                             std::vector<std::string>* paths) {
  struct Frame {
    const lightusd::Prim* prim;
    size_t child_index;
    size_t parent_path_length;
  };
  std::vector<Frame> stack;
  std::string path = "/" + root.local_path().full_path_name();
  if (root.is<lightusd::GeomMesh>()) paths->push_back(path);
  if (!root.children().empty()) stack.push_back({&root, 0, 0});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.child_index >= frame.prim->children().size()) {
      path.resize(frame.parent_path_length);
      stack.pop_back();
      continue;
    }
    const lightusd::Prim& child = frame.prim->children()[frame.child_index++];
    const size_t current_path_length = path.size();
    path += "/";
    path += child.local_path().full_path_name();
    if (child.is<lightusd::GeomMesh>()) paths->push_back(path);
    if (!child.children().empty()) {
      stack.push_back({&child, 0, current_path_length});
    } else {
      path.resize(current_path_length);
    }
  }
}

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); }
  );
  return s;
}

void print_help() {
    std::cout << "Usage progressive_composition [--noflatten] [--loadOnly] [--composition=STRLIST] [--relative] [--extract-variants] input.usda/usdc/usdz\n";
    std::cout << "\n --noflatten Do not do composition";
    std::cout << "  --composition: Specify which composition feature to be "
                 "enabled(valid when `--flatten` is supplied). Comma separated "
                 "list. \n    l "
                 "`subLayers`, i `inherits`, v `variantSets`, r `references`, "
                 "p `payload`, s `specializes`. \n    Example: "
                 "--composition=r,p --composition=references,subLayers\n";
    std::cout << "\n --extract-variants (w.i.p) Dump variants information to .json\n";
    std::cout << "\n --relative (not implemented yet) Print Path as relative Path\n";
    std::cout << "\n -l, --loadOnly Load(Parse) USD file only(Check if input USD is valid or not)\n";

}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_help();
    return EXIT_FAILURE;
  }

  bool has_flatten{true};
  bool has_relative{false};
  bool has_extract_variants{false};
  bool load_only{false};

  constexpr int kMaxIteration = 128;

  std::string filepath;

  int input_index = -1;
  CompositionFeatures comp_features;

  for (size_t i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if ((arg.compare("-h") == 0) || (arg.compare("--help") ==0)) {
      print_help();
      return EXIT_FAILURE;
    } else if ((arg.compare("-f") == 0) || (arg.compare("--noflatten") == 0)) {
      has_flatten = false;
    } else if (arg.compare("--relative") == 0) {
      has_relative = true;
    } else if ((arg.compare("-l") == 0) || (arg.compare("--loadOnly") == 0)) {
      load_only = true;
    } else if (arg.compare("--extract-variants") == 0) {
      has_extract_variants = true;
    } else if (lightusd::startsWith(arg, "--composition=")) {
      std::string value_str = lightusd::removePrefix(arg, "--composition=");
      if (value_str.empty()) {
        std::cerr << "No values specified to --composition.\n";
        exit(-1);
      }

      std::vector<std::string> items = lightusd::split(value_str, ",");
      comp_features.subLayers = false;
      comp_features.inherits = false;
      comp_features.variantSets = false;
      comp_features.references = false;
      comp_features.payload = false;
      comp_features.specializes = false;

      for (const auto &item : items) {
        if ((item == "l") || (item == "subLayers")) {
          comp_features.subLayers = true;
        } else if ((item == "i") || (item == "inherits")) {
          comp_features.inherits = true;
        } else if ((item == "v") || (item == "variantSets")) {
          comp_features.variantSets = true;
        } else if ((item == "r") || (item == "references")) {
          comp_features.references = true;
        } else if ((item == "p") || (item == "payload")) {
          comp_features.payload = true;
        } else if ((item == "s") || (item == "specializes")) {
          comp_features.specializes = true;
        } else {
          std::cerr << "Invalid string for --composition : " << item << "\n";
          exit(-1);
        }
      }

    } else {
      filepath = arg;
      input_index = i;
    }
  }

  if (filepath.empty() || (input_index < 0)) {
    std::cout << "Input USD filename missing.\n";
    return EXIT_FAILURE;
  }

  std::string warn;
  std::string err;

  std::string ext = str_tolower(GetFileExtension(filepath));
  std::string base_dir;
  base_dir = lightusd::io::GetBaseDir(filepath);

  if (has_flatten) {

    if (load_only) {
      std::cerr << "--flatten and --loadOnly cannot be specified at a time\n";
      return EXIT_FAILURE;
    }

    // TODO: flatten for USDZ
    if (lightusd::IsUSDZ(filepath)) {

      std::cout << "--flatten is ignored for USDZ at the moment.\n";

      lightusd::Stage stage;

      bool ret = lightusd::LoadUSDZFromFile(filepath, &stage, &warn, &err);
      if (!warn.empty()) {
        std::cerr << "WARN : " << warn << "\n";
      }
      if (!err.empty()) {
        std::cerr << "ERR : " << err << "\n";
        //return EXIT_FAILURE;
      }

      if (!ret) {
        std::cerr << "Failed to load USDZ file: " << filepath << "\n";
        return EXIT_FAILURE;
      }

      std::cout << to_string(stage) << "\n";

      return EXIT_SUCCESS;
    }

    lightusd::Layer root_layer;
    bool ret = lightusd::LoadLayerFromFile(filepath, &root_layer, &warn, &err);
    if (warn.size()) {
      std::cout << "WARN: " << warn << "\n";
    }

    if (!ret) {
      std::cerr << "Failed to read USD data as Layer: \n";
      std::cerr << err << "\n";
      return -1;
    }

    std::cout << "# input\n";
    std::cout << root_layer << "\n";

    lightusd::Stage stage;
    stage.metas() = root_layer.metas();

    std::string warn;

    lightusd::AssetResolutionResolver resolver;
    resolver.set_current_working_path(base_dir);
    resolver.set_search_paths({base_dir});

    //
    // LIVRPS strength ordering
    // - [x] Local(subLayers)
    // - [x] Inherits
    // - [x] VariantSets
    // - [x] References
    // - [x] Payload
    // - [ ] Specializes
    //

    lightusd::Layer src_layer = root_layer;
    if (comp_features.subLayers) {

      // NOTE: subLayers are resolved recursively.

      std::vector<std::string> subLayerAssetPaths = lightusd::ExtractSublayerAssetPaths(src_layer);
      for (size_t i = 0; i < subLayerAssetPaths.size(); i++) {
          std::cout << "subLayer asset path: " << subLayerAssetPaths[i] << "\n";
      }

      lightusd::Layer composited_layer;
      if (!lightusd::CompositeSublayers(resolver, src_layer, &composited_layer, &warn, &err)) {
        std::cerr << "Failed to composite subLayers: " << err << "\n";
        return -1;
      }

      if (warn.size()) {
        std::cout << "WARN: " << warn << "\n";
      }

      std::cout << "# `subLayers` composited\n";
      std::cout << composited_layer << "\n";

      src_layer = std::move(composited_layer);
    }

    // TODO: Find more better way to Recursively resolve references/payload/variants
    for (int i = 0; i < kMaxIteration; i++) {

      bool has_unresolved = false;

      if (comp_features.references) {
        if (!src_layer.check_unresolved_references()) {
          std::cout << "# iter " << i << ": no unresolved references.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositeReferences(resolver, src_layer, &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `references`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cout << "WARN: " << warn << "\n";
          }

          std::cout << "# `references` composited\n";
          std::cout << composited_layer << "\n";

          src_layer = std::move(composited_layer);
        }
      }

      if (comp_features.payload) {
        if (!src_layer.check_unresolved_payload()) {
          std::cout << "# iter " << i << ": no unresolved payload.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositePayload(resolver, src_layer, &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `payload`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cout << "WARN: " << warn << "\n";
          }

          std::cout << "# `payload` composited\n";
          std::cout << composited_layer << "\n";

          src_layer = std::move(composited_layer);
        }
      }

      if (comp_features.inherits) {
        if (!src_layer.check_unresolved_inherits()) {
          std::cout << "# iter " << i << ": no unresolved inherits.\n";
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositeInherits(src_layer, &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `inherits`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cout << "WARN: " << warn << "\n";
          }

          std::cout << "# `inherits` composited\n";
          std::cout << composited_layer << "\n";

          src_layer = std::move(composited_layer);
        }
      }

      if (comp_features.variantSets) {
        // AOUSD Core Spec 10.3.2.5: defer variant composition until references
        // and payloads are resolved (see ShouldDeferVariantComposition).
        if (!src_layer.check_unresolved_variant()) {
          std::cout << "# iter " << i << ": no unresolved variant.\n";
        } else if (lightusd::ShouldDeferVariantComposition(
                       src_layer, comp_features.references,
                       comp_features.payload)) {
          std::cout << "# iter " << i
                    << ": variant resolution deferred (refs/payloads pending).\n";
          has_unresolved = true;
        } else {
          has_unresolved = true;

          lightusd::Layer composited_layer;
          if (!lightusd::CompositeVariant(src_layer, &composited_layer, &warn, &err)) {
            std::cerr << "Failed to composite `variantSet`: " << err << "\n";
            return -1;
          }

          if (warn.size()) {
            std::cout << "WARN: " << warn << "\n";
          }

          std::cout << "# `variantSet` composited\n";
          std::cout << composited_layer << "\n";

          src_layer = std::move(composited_layer);
        }
      }

      // TODO
      // - [ ] specializes
      // - [ ] `class` Prim?

      std::cout << "# has_unresolved_references: " << src_layer.check_unresolved_references() << "\n";
      std::cout << "# all resolved? " << !has_unresolved << "\n";

      if (!has_unresolved) {
        std::cout << "# of composition iteration to resolve fully: " << (i + 1) << "\n";
        break;
      }

    }

    if (has_extract_variants) {
      lightusd::Dictionary dict;
      if (!lightusd::ExtractVariants(src_layer, &dict, &err)) {
        std::cerr << "Failed to extract variants info: " << err;
      } else {
        std::cout << "== Variants info ==\n" << lightusd::to_string(dict) << "\n";
      }

    }

    lightusd::Stage comp_stage;
    ret = LayerToStage(std::move(src_layer), &comp_stage, &warn, &err);
    if (warn.size()) {
      std::cout << warn<< "\n";
    }

    if (!ret) {
      std::cerr << err << "\n";
    }

    std::cout << comp_stage.ExportToString() << "\n";

    std::vector<std::string> mesh_paths;
    for (const lightusd::Prim& root : comp_stage.root_prims()) {
      CollectMeshPaths(root, &mesh_paths);
    }
    std::sort(mesh_paths.begin(), mesh_paths.end());
    for (const std::string& path : mesh_paths) std::cout << "Prim : " << path << "\n";

  } else {

    lightusd::Stage stage;

    lightusd::USDLoadOptions options;

    // auto detect format.
    bool ret = lightusd::LoadUSDFromFile(filepath, &stage, &warn, &err, options);
    if (!warn.empty()) {
      std::cerr << "WARN : " << warn << "\n";
    }
    if (!err.empty()) {
      std::cerr << "ERR : " << err << "\n";
      //return EXIT_FAILURE;
    }

    if (!ret) {
      std::cerr << "Failed to load USD file: " << filepath << "\n";
      return EXIT_FAILURE;
    }

    if (load_only) {
      return EXIT_SUCCESS;
    }

    std::string s = stage.ExportToString(has_relative);
    std::cout << s << "\n";

    if (has_extract_variants) {
      lightusd::Dictionary dict;
      if (!lightusd::ExtractVariants(stage, &dict, &err)) {
        std::cerr << "Failed to extract variants info: " << err;
      } else {
        std::cout << "== Variants info ==\n" << lightusd::to_string(dict) << "\n";
      }

    }
  }

  return EXIT_SUCCESS;
}
