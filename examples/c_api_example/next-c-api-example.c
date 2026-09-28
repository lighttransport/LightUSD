/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal consumer of the portable LightUSD C ABI. */
#include <stdio.h>
#include <stdlib.h>

#include "lightusd-c.h"

static void print_prim(lightusd_prim prim, unsigned depth) {
  if (!lightusd_prim_is_valid(prim)) return;
  for (unsigned i = 0; i < depth; ++i) fputs("  ", stdout);
  lightusd_sv path = lightusd_prim_path(prim);
  lightusd_sv type = lightusd_prim_type_name(prim);
  printf("%.*s (%.*s)\n", (int)path.len, path.data, (int)type.len, type.data);
  for (size_t i = 0; i < lightusd_prim_child_count(prim); ++i)
    print_prim(lightusd_prim_child(prim, i), depth + 1);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: next_c_api_example FILE.usda\n");
    return 2;
  }
  lightusd_stage* stage = NULL;
  lightusd_status status = lightusd_stage_load(argv[1], NULL, &stage);
  if (status != LIGHTUSD_OK) {
    fprintf(stderr, "Load failed: %s\n", lightusd_last_error());
    return 1;
  }
  for (size_t i = 0; i < lightusd_stage_root_prim_count(stage); ++i)
    print_prim(lightusd_stage_root_prim(stage, i), 0);
  lightusd_string* usda = NULL;
  status = lightusd_stage_export_usda(stage, &usda);
  if (status == LIGHTUSD_OK) {
    lightusd_sv text = lightusd_string_view(usda);
    printf("Exported %zu USDA bytes\n", text.len);
    lightusd_string_destroy(usda);
  }
  lightusd_stage_destroy(stage);
  return status == LIGHTUSD_OK ? 0 : 1;
}
