#include <fstream>
#include <iostream>

#include "asset-resolution.hh"
#include "io-util.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "usda-reader.hh"
#include "composition.hh"
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
    std::cout << "usdaparser [--flatten] [--composition=list] input.usda\n";
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
    std::cerr << "Input file does not exist or invalid: " << filename << "\n";
    return -1;
  }

  if (!lightusd::legacy::IsUSDA(filename)) {
    std::cerr << "Input file isn't a USDA file: " << filename << "\n";
    return -1;
  }

  std::vector<uint8_t> data;
  std::string err;
  if (!lightusd::io::ReadWholeFile(&data, &err, filename,
                                   /* filesize_max */ 0)) {
    std::cerr << "Failed to open file: " << filename << ":" << err << "\n";
    return -1;
  }

  lightusd::legacy::StreamReader sr(data.data(), data.size(), /* swap endian */ false);
  lightusd::legacy::usda::USDAReader reader(&sr);

#if !defined(LIGHTUSD_PRODUCTION_BUILD)
  std::cout << "Basedir = " << base_dir << "\n";
#endif
  //reader.SetBaseDir(base_dir);

  uint32_t load_states = static_cast<uint32_t>(lightusd::legacy::LoadState::Toplevel);
#if 0
  if (do_compose) {
    if (comp_features.subLayers) {
      load_states |= static_cast<uint32_t>(lightusd::legacy::LoadState::Sublayer);
      std::cout << "Enable subLayers.\n";
    }
    if (comp_features.payloads) {
      load_states |= static_cast<uint32_t>(lightusd::legacy::LoadState::Payload);
      std::cout << "Enable payloads.\n";
    }
    if (comp_features.references) {
      load_states |= static_cast<uint32_t>(lightusd::legacy::LoadState::Reference);
      std::cout << "Enable references.\n";
    }
  }
#endif

  bool as_primspec = do_compose ? true : false;

  {
    // TODO: ReaderConfig.
    bool ret = reader.read(load_states, as_primspec);

    if (!ret) {
      std::cerr << "Failed to parse .usda: \n";
      std::cerr << reader.GetError() << "\n";
      return -1;
    } else {
#if !defined(LIGHTUSD_PRODUCTION_BUILD)
      std::cout << "ok\n";
#endif
    }
  }

  if (do_compose) {
    lightusd::legacy::Layer root_layer;
    bool ret = reader.get_as_layer(&root_layer);
    if (!ret) {
      std::cerr << "Failed to get USD data as Layer: \n";
      std::cerr << reader.get_error() << "\n";
      return -1;
    }

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
    bool ret = reader.ReconstructStage();
    if (!ret) {
      std::cerr << "Failed to reconstruct Stage: \n";
      std::cerr << reader.get_error() << "\n";
      return -1;
    }

    lightusd::legacy::Stage stage = reader.get_stage();
    std::cout << stage.ExportToString() << "\n";
  }

  return 0;
}
