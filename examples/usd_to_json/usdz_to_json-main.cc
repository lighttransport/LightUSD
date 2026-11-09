#include <fstream>
#include <iostream>

#include "io-util.hh"
#include "stream-reader.hh"
#include "usd-to-json.hh"
#include "usda-reader.hh"

void print_usage(const char* program_name) {
  std::cout << "Usage: " << program_name << " <input.usdz> [output_prefix]\n";
  std::cout << "\nConverts USDZ to separate JSON files:\n";
  std::cout << "  - <output_prefix>_usd.json    : USD scene content\n";
  std::cout << "  - <output_prefix>_assets.json : Asset data (base64 encoded)\n";
  std::cout << "\nExample:\n";
  std::cout << "  " << program_name << " model.usdz output\n";
  std::cout << "  -> creates output_usd.json and output_assets.json\n";
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string filename = argv[1];

  // Check if file is USDZ
  std::string ext = lightusd::io::GetFileExtension(filename);
  bool is_usdz = (ext == "usdz" || ext == "USDZ");

  if (!is_usdz) {
    std::cerr << "This tool is specifically for USDZ files. For other USD formats, use the regular USD to JSON converter.\n";
    return 1;
  }

  // Determine output prefix
  std::string output_prefix;
  if (argc >= 3) {
    output_prefix = argv[2];
  } else {
    // Use input filename without extension as prefix
    size_t dot_pos = filename.find_last_of('.');
    size_t slash_pos = filename.find_last_of("/\\");
    if (slash_pos == std::string::npos) slash_pos = 0; else slash_pos++;

    if (dot_pos != std::string::npos && dot_pos > slash_pos) {
      output_prefix = filename.substr(slash_pos, dot_pos - slash_pos);
    } else {
      output_prefix = filename.substr(slash_pos);
    }
  }

  std::cout << "Converting USDZ to JSON: " << filename << std::endl;

  // Convert USDZ to JSON
  lightusd::legacy::USDZToJSONResult result;
  std::string warn, err;
  lightusd::legacy::USDToJSONOptions options;

  bool success = lightusd::legacy::USDZToJSON(filename, &result, &warn, &err, options);

  if (!warn.empty()) {
    std::cerr << "Warnings: " << warn << std::endl;
  }

  if (!success) {
    std::cerr << "Failed to convert USDZ to JSON: " << err << std::endl;
    return 1;
  }

  std::cout << "Successfully converted USDZ!" << std::endl;
  std::cout << "Main USD file: " << result.main_usd_filename << std::endl;
  std::cout << "Total assets: " << result.asset_filenames.size() << std::endl;

  // Write USD content to JSON file
  std::string usd_json_filename = output_prefix + "_usd.json";
  std::ofstream usd_file(usd_json_filename);
  if (!usd_file.is_open()) {
    std::cerr << "Failed to create USD JSON file: " << usd_json_filename << std::endl;
    return 1;
  }

  usd_file << result.usd_json;
  usd_file.close();
  std::cout << "USD content written to: " << usd_json_filename << std::endl;

  // Write assets to JSON file
  std::string assets_json_filename = output_prefix + "_assets.json";
  std::ofstream assets_file(assets_json_filename);
  if (!assets_file.is_open()) {
    std::cerr << "Failed to create assets JSON file: " << assets_json_filename << std::endl;
    return 1;
  }

  assets_file << result.assets_json;
  assets_file.close();
  std::cout << "Assets written to: " << assets_json_filename << std::endl;

  // Print summary
  std::cout << "\nAsset summary:" << std::endl;
  for (const auto& asset_filename : result.asset_filenames) {
    std::cout << "  - " << asset_filename << std::endl;
  }

  std::cout << "\nConversion complete!" << std::endl;

  return 0;
}
