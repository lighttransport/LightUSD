// simple USDC parser
#include <iostream>
#include <fstream>

#include "stream-reader.hh"
#include "usdc-reader.hh"
#include "io-util.hh"
#include "str-util.hh"
#include "pprinter.hh"

struct CompositionFeatures {
  bool subLayers{true};
  bool inherits{true};
  bool variantSets{true};
  bool references{true};
  bool payload{true};
  bool specializes{true};
};

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "usdc_parser [--flatten] [--composition=list] input.usda\n";
    std::cout << "  --flatten: (Not implemented yet) Similar to --flatten in "
                 "usdview from pxrUSD.\n";
    std::cout << "  --composition: Specify which composition feature to be "
                 "enabled(valid when `--flatten` is supplied). Comma separated "
                 "list. \n    l "
                 "`subLayers`, i `inherits`, v `variantSets`, r `references`, "
                 "p `payload`, s `specializes`. \n    Example: "
                 "--composition=r,p --composition=references,subLayers\n";
    exit(-1);
  }

  std::string filename;
  // = argv[1];

  bool do_compose = false;
  size_t input_idx = 0;  // 0 = invalid
  CompositionFeatures comp_features;

  for (size_t i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--flatten") {
      do_compose = true;
    } else if (lightusd::legacy::startsWith(arg, "--composition=")) {
      std::string value_str = lightusd::legacy::removePrefix(arg, "--composition=");
      if (value_str.empty()) {
        std::cerr << "No values specified to --composition.\n";
        exit(-1);
      }

      std::vector<std::string> items = lightusd::legacy::split(value_str, ",");
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

    } else if (input_idx == 0) {
      input_idx = i;
    }
  }

  if (input_idx == 0) {
    std::cerr << "No USD filename given.\n";
    exit(-1);
  }

  filename = argv[input_idx];

  std::string base_dir;
  base_dir = lightusd::io::GetBaseDir(filename);

  if (!lightusd::io::USDFileExists(filename)) {
    std::cerr << "Input file does not exist or failed to read: " << filename << "\n";
    return -1;
  }

  if (!lightusd::legacy::IsUSDC(filename)) {
    std::cerr << "Input file isn't a USDC file: " << filename << "\n";
    return -1;
  }

  std::vector<uint8_t> data;
  std::string err;
  if (!lightusd::io::ReadWholeFile(&data, &err, filename, /* filesize_max */0)) {
    std::cerr << "Failed to open file: " << filename << ":" << err << "\n";
  }

  lightusd::legacy::StreamReader sr(data.data(), data.size(), /* swap endian */ false);
  lightusd::legacy::usdc::USDCReader reader(&sr);

  //std::cout << "Basedir = " << base_dir << "\n";
  //reader.SetBaseDir(base_dir);

  {
    bool ret = reader.ReadUSDC();

    if (!ret) {

      if (reader.GetWarning().size()) {
        std::cout << "WARN: " << reader.GetWarning() << "\n";
      }

      std::cerr << "Failed to parse .usdc: \n";
      std::cerr << reader.GetError() << "\n";
      return -1;
    }
  }

  // Composite or Dump
  if (do_compose) {
    lightusd::legacy::Layer root_layer;
    bool ret = reader.get_as_layer(&root_layer);

    std::cout << "# input\n";
    std::cout << root_layer << "\n";

    lightusd::legacy::Stage stage;
    stage.metas() = root_layer.metas();

    std::string warn;

    lightusd::legacy::AssetResolutionResolver resolver;
    resolver.set_search_paths({base_dir});

    //
    // LIVRPS strength ordering
    // - [x] Local(subLayers)
    // - [ ] Inherits
    // - [ ] VariantSets
    // - [x] References
    // - [x] Payload
    // - [ ] Specializes
    //

    lightusd::legacy::Layer src_layer = root_layer;
    if (comp_features.subLayers) {
      lightusd::legacy::Layer composited_layer;
      if (!lightusd::legacy::CompositeSublayers(resolver, src_layer, &composited_layer, &warn, &err)) {
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

    if (comp_features.references) {
      lightusd::legacy::Layer composited_layer;
      if (!lightusd::legacy::CompositeReferences(resolver, src_layer, &composited_layer, &warn, &err)) {
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

    if (comp_features.payload) {
      lightusd::legacy::Layer composited_layer;
      if (!lightusd::legacy::CompositePayload(resolver, src_layer, &composited_layer, &warn, &err)) {
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

    // TODO...
  } else {
    lightusd::legacy::Stage stage;
    bool ret = reader.ReconstructStage(&stage);
    if (!ret) {

      if (reader.GetWarning().size()) {
        std::cout << "WARN: " << reader.GetWarning() << "\n";
      }

      std::cerr << "Failed to reconstruct Stage: \n";
      std::cerr << reader.GetError() << "\n";
      return -1;
    }

    if (reader.GetWarning().size()) {
      std::cout << "WARN: " << reader.GetWarning() << "\n";
    }

    // There may be error but not fatal.
    if (reader.GetError().size()) {
      std::cerr << reader.GetError() << "\n";
    }

    std::cout << stage.ExportToString() << "\n";
  }

  return 0;
}
