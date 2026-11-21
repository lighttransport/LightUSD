#include <algorithm>
#include <cctype>
#include <iostream>
#include "lightusd.hh"

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

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Read USD file and check if reading is working well(TODO: use lusdcat and deprecate this test_lightusd program)\n" << std::endl;
    std::cout << "Usage input.[usd|usda|usdc|usdz] (--verbose)\n" << std::endl;
    return EXIT_FAILURE;
  }

  bool verbose = false; // default false for unit batch test.

  if (argc > 2) {
    std::string arg(argv[2]);
    if (arg.compare("--verbose") == 0) {
      verbose = true;
    }
  }

  std::string filepath = argv[1];
  std::string warn;
  std::string err;

  std::string ext = str_tolower(GetFileExtension(filepath));

  lightusd::legacy::Stage stage;

  if (ext.compare("usdc") == 0) {
    bool ret = lightusd::legacy::LoadUSDCFromFile(filepath, &stage, &warn, &err);
    if (!warn.empty()) {
      std::cerr << "WARN : " << warn << "\n";
    }
    if (!err.empty()) {
      std::cerr << "ERR : " << err << "\n";
      //return EXIT_FAILURE;
    }

    if (!ret) {
      std::cerr << "Failed to load USDC file: " << filepath << "\n";
      return EXIT_FAILURE;
    }
  } else if (ext.compare("usda") == 0) {
    bool ret = lightusd::legacy::LoadUSDAFromFile(filepath, &stage, &warn, &err);
    if (!warn.empty()) {
      std::cerr << "WARN : " << warn << "\n";
    }
    if (!err.empty()) {
      std::cerr << "ERR : " << err << "\n";
      //return EXIT_FAILURE;
    }

    if (!ret) {
      std::cerr << "Failed to load USDA file: " << filepath << "\n";
      return EXIT_FAILURE;
    }
  } else if (ext.compare("usdz") == 0) {
    //std::cout << "usdz\n";
    bool ret = lightusd::legacy::LoadUSDZFromFile(filepath, &stage, &warn, &err);
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
  } else {
    // try to auto detect format.
    bool ret = lightusd::legacy::LoadUSDFromFile(filepath, &stage, &warn, &err);
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
  }

  if (verbose) {
    std::cout<< "Load OK: " << filepath << "\n";
  }

  return EXIT_SUCCESS;
}
