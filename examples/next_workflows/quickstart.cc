// SPDX-License-Identifier: Apache-2.0
// Public C ABI consumer example; no internal next/Tydra headers.
#include "lightusd-cpp.hh"
#include <cstdio>
#include <cstring>

static void PrintPrim(const lightusd::api::Prim& prim) {
  if (!prim) return;
  const lightusd_sv path = prim.path();
  const lightusd_sv type = prim.type_name();
  std::printf("%.*s %.*s\n", static_cast<int>(path.len), path.data,
              static_cast<int>(type.len), type.data);
  for (size_t i = 0; i < prim.child_count(); ++i) PrintPrim(prim.child(i));
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: next_quickstart INPUT OUTPUT.usda|OUTPUT.usdc\n");
    return 2;
  }
  lightusd::api::Stage stage;
  if (stage.load(argv[1]) != LIGHTUSD_OK) {
    std::fprintf(stderr, "Load failed: %s\n", lightusd::api::LastError());
    return 1;
  }
  for (size_t i = 0; i < stage.root_prim_count(); ++i)
    PrintPrim(stage.root_prim(i));

  lightusd_save_options save;
  lightusd::api::InitSaveOptions(&save);
  const size_t output_len = std::strlen(argv[2]);
  save.format = (output_len >= 5 &&
                 std::strcmp(argv[2] + output_len - 5, ".usda") == 0)
                    ? LIGHTUSD_FORMAT_USDA
                    : LIGHTUSD_FORMAT_USDC;
  if (stage.save(argv[2], &save) != LIGHTUSD_OK) {
    std::fprintf(stderr, "Save failed: %s\n", lightusd::api::LastError());
    return 1;
  }
  lightusd::api::Stage reopened;
  if (reopened.load(argv[2]) != LIGHTUSD_OK) {
    std::fprintf(stderr, "Reopen failed: %s\n", lightusd::api::LastError());
    return 1;
  }
  std::printf("Reopened %s\n", argv[2]);
  return 0;
}
