/* SPDX-License-Identifier: Apache-2.0 */
#include "lightusd-c.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s: %s\n", __LINE__, #x, lightusd_last_error()); return 1; } } while (0)
int main(void) {
  lightusd_asset_resolver* resolver = NULL;
  CHECK(lightusd_asset_resolver_create(&resolver) == LIGHTUSD_OK);
  CHECK(lightusd_asset_resolver_set_filesystem_access(resolver, 0) == LIGHTUSD_OK);
  CHECK(lightusd_asset_resolver_set_filesystem_access(resolver, 2) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_string* bytes = NULL;
  CHECK(lightusd_asset_resolver_read(resolver, __FILE__, &bytes) == LIGHTUSD_ERR_IO);
  CHECK(bytes == NULL);
  const char source[] = "#usda 1.0\ndef Xform \"Root\" {}\n";
  CHECK(lightusd_asset_resolver_register_memory(resolver, "mem:/root.usda",
      (const uint8_t*)source, sizeof(source)-1) == LIGHTUSD_OK);
  CHECK(lightusd_asset_resolver_read(resolver, "mem:/root.usda", &bytes) == LIGHTUSD_OK);
  const lightusd_sv view = lightusd_string_view(bytes);
  CHECK(view.len == sizeof(source)-1 && memcmp(view.data, source, view.len) == 0);
  lightusd_string_destroy(bytes);
  lightusd_load_options options;
  lightusd_load_options_init(&options);
  options.resolver = resolver;
  lightusd_stage* stage = NULL;
  CHECK(lightusd_stage_load("mem:/root.usda", &options, &stage) == LIGHTUSD_OK);
  CHECK(stage != NULL);
  lightusd_stage_destroy(stage);
  stage = NULL;
  options.format = LIGHTUSD_FORMAT_USDA;
  CHECK(lightusd_stage_load("mem:/root.usda", &options, &stage) == LIGHTUSD_OK);
  lightusd_stage_destroy(stage);
  stage = NULL;
  CHECK(lightusd_stage_load(__FILE__, &options, &stage) != LIGHTUSD_OK);
  CHECK(stage == NULL);
  lightusd_asset_resolver_destroy(resolver);
  puts("PASS: C API filesystem denial and memory-only stage load");
  return 0;
}
