#include <iostream>
#include <fstream>
#include <string>

// LightUSD core
#include "lightusd.hh"
#include "pprinter.hh"

#if defined(LIGHTUSD_WITH_QJS)
#include "tydra/js-script.hh"
#endif

static std::string LoadJavaScriptFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return "";
    }

    std::string content;
    std::string line;
    while (std::getline(file, line)) {
        content += line + "\n";
    }
    return content;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <USD_FILE> <JS_SCRIPT>" << std::endl;
        std::cout << "Example: " << argv[0] << " model.usd query_layer.js" << std::endl;
        return -1;
    }

    std::string usd_filename = argv[1];
    std::string js_filename = argv[2];

    std::cout << "Loading USD file: " << usd_filename << std::endl;
    std::cout << "Loading JavaScript: " << js_filename << std::endl;

#if defined(LIGHTUSD_WITH_QJS)

    // Load USD file as Stage (which contains Layer metadata)
    lightusd::legacy::Stage stage;
    std::string warn, err;

    bool ret = lightusd::legacy::LoadUSDFromFile(usd_filename, &stage, &warn, &err);

    if (!warn.empty()) {
        std::cerr << "WARN: " << warn << std::endl;
    }

    if (!ret) {
        std::cerr << "ERROR: Failed to load USD file: " << err << std::endl;
        return -1;
    }

    std::cout << "Successfully loaded USD file" << std::endl;

    // Get LayerMetas from the Stage
    const lightusd::legacy::LayerMetas& layer_metas = stage.metas();

    std::cout << "Stage LayerMetas loaded" << std::endl;
    std::cout << "  - upAxis: " << lightusd::legacy::to_string(layer_metas.upAxis.get_value()) << std::endl;
    std::cout << "  - defaultPrim: " << layer_metas.defaultPrim.str() << std::endl;
    std::cout << "  - metersPerUnit: " << layer_metas.metersPerUnit.get_value() << std::endl;
    std::cout << "  - timeCodesPerSecond: " << layer_metas.timeCodesPerSecond.get_value() << std::endl;

    // Load JavaScript code
    std::string js_code = LoadJavaScriptFile(js_filename);
    if (js_code.empty()) {
        std::cerr << "ERROR: Failed to load JavaScript file: " << js_filename << std::endl;
        return -1;
    }

    std::cout << "Loaded JavaScript code (" << js_code.length() << " bytes)" << std::endl;

    // Execute JavaScript with LayerMetas access
    std::cout << "Executing JavaScript with LayerMetas access..." << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    std::string js_err;
    bool js_ret = lightusd::legacy::tydra::RunJSScriptWithLayerMetas(js_code, &layer_metas, js_err);

    std::cout << "----------------------------------------" << std::endl;

    if (!js_ret) {
        std::cerr << "ERROR: JavaScript execution failed: " << js_err << std::endl;
        return -1;
    }

    std::cout << "JavaScript executed successfully" << std::endl;

#else
    std::cerr << "ERROR: This example requires QuickJS support (LIGHTUSD_WITH_QJS=ON)" << std::endl;
    std::cerr << "Please rebuild LightUSD with QuickJS enabled." << std::endl;
    return -1;
#endif

    return 0;
}