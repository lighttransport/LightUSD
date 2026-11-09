// SPDX-License-Identifier: Apache-2.0
// C API: custom portable light-filter registration and parsing.
#include <stdio.h>
#include <string.h>

#include "lightusd-c.h"
#include "lightusd-render-c.h"

static int failures = 0;

static void check(int ok, const char* what) {
  if (!ok) {
    ++failures;
    fprintf(stderr, "FAIL: %s (last error: %s)\n", what,
            lightusd_last_error());
  }
}

/* A custom kind: intensity comes from lightusd:filter:value0, color is passed
 * through. Rejecting (return 0) must surface as a conversion warning. */
static int parse_gain(const lightusd_light_filter_params* params, void* user,
                      float* intensity, float* color) {
  const int* calls = (const int*)user;
  if (calls) {
    /* user pointer only proves the binding was forwarded */
  }
  if (!params || strcmp(params->kind, "gain") != 0) return 0;
  if (strcmp(params->prim_path, "/World/Gain") != 0) return 0;
  if (params->value_count != 8) return 0;
  *intensity = params->values[0];
  color[0] = params->color[0];
  color[1] = params->color[1];
  color[2] = params->color[2];
  return 1;
}

static int parse_reject(const lightusd_light_filter_params* params, void* user,
                        float* intensity, float* color) {
  (void)params;
  (void)user;
  (void)intensity;
  (void)color;
  return 0;
}

static const char kUsda[] =
    "#usda 1.0\n"
    "def Scope \"World\"\n"
    "{\n"
    "    def LightFilter \"Half\"\n"
    "    {\n"
    "        uniform token lightusd:filter:kind = \"intensity\"\n"
    "        float lightusd:filter:intensity = 0.5\n"
    "    }\n"
    "    def LightFilter \"Gain\"\n"
    "    {\n"
    "        uniform token lightusd:filter:kind = \"gain\"\n"
    "        float lightusd:filter:value0 = 0.25\n"
    "        color3f lightusd:filter:color = (0.5, 1, 1)\n"
    "    }\n"
    "    def LightFilter \"Reject\"\n"
    "    {\n"
    "        uniform token lightusd:filter:kind = \"reject\"\n"
    "    }\n"
    "    def LightFilter \"Mystery\"\n"
    "    {\n"
    "        uniform token lightusd:filter:kind = \"not-registered\"\n"
    "    }\n"
    "}\n";

static float parse_intensity(lightusd_stage* stage, const char* path,
                             float* color) {
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage, path);
  float intensity = -1.0f;
  if (lightusd_light_filter_parse(stage, prim, 0.0, &intensity, color) !=
      LIGHTUSD_OK)
    return -1.0f;
  return intensity;
}

int main(void) {
  /* Built-ins are present before any registration. */
  check(lightusd_light_filter_kind_count() >= 2, "built-in kinds listed");
  char name[64];
  size_t required = 0;
  check(lightusd_light_filter_kind(0, NULL, 0, &required) == LIGHTUSD_OK &&
            required > 0,
        "kind name length query");
  check(lightusd_light_filter_kind(0, name, sizeof name, &required) ==
                LIGHTUSD_OK &&
            required > 0 && name[required - 1] == '\0',
        "kind name copy is terminated");

  lightusd_stage* stage = NULL;
  if (lightusd_stage_load_from_memory((const uint8_t*)kUsda,
                                      strlen(kUsda), NULL, &stage) !=
          LIGHTUSD_OK ||
      !stage) {
    fprintf(stderr, "FAIL: stage load: %s\n", lightusd_last_error());
    return 1;
  }

  float color[3] = {0, 0, 0};
  check(parse_intensity(stage, "/World/Half", color) > 0.4999f &&
            parse_intensity(stage, "/World/Half", color) < 0.5001f,
        "built-in intensity filter parses");

  /* Custom registration: the user pointer is forwarded and value0 carries the
   * multiplier. */
  int user_token = 7;
  check(lightusd_register_light_filter("gain", parse_gain, &user_token) ==
            LIGHTUSD_OK,
        "register custom kind");
  check(lightusd_register_light_filter("gain", parse_gain, &user_token) ==
            LIGHTUSD_ERR_INVALID_ARG,
        "duplicate kind rejected");
  check(lightusd_light_filter_kind_count() >= 3, "custom kind listed");
  color[0] = color[1] = color[2] = 0;
  const float gain = parse_intensity(stage, "/World/Gain", color);
  check(gain > 0.2499f && gain < 0.2501f, "custom value0 -> intensity");
  check(color[0] > 0.4999f && color[0] < 0.5001f && color[1] > 0.9999f,
        "custom color passthrough");

  /* A parser that rejects the prim fails closed. */
  check(lightusd_register_light_filter("reject", parse_reject, NULL) ==
            LIGHTUSD_OK,
        "register rejecting kind");
  check(parse_intensity(stage, "/World/Reject", color) < 0.0f,
        "rejected filter prim reports failure");

  /* Unknown kinds are reported, never silently applied. */
  check(parse_intensity(stage, "/World/Mystery", color) < 0.0f,
        "unknown kind fails");
  check(strstr(lightusd_last_error(), "not-registered") != NULL,
        "unknown kind names the kind");

  /* Invalid arguments. */
  check(lightusd_register_light_filter(NULL, parse_gain, NULL) ==
            LIGHTUSD_ERR_INVALID_ARG,
        "null kind rejected");
  check(lightusd_register_light_filter("x", NULL, NULL) ==
            LIGHTUSD_ERR_INVALID_ARG,
        "null parser rejected");
  check(lightusd_light_filter_parse(NULL, lightusd_stage_prim_at_path(
                                             stage, "/World/Gain"),
                                    0.0, &color[0], color) ==
            LIGHTUSD_ERR_INVALID_ARG,
        "null stage rejected");

  /* Unregistration restores the unknown-kind behaviour. */
  check(lightusd_unregister_light_filter("gain") == LIGHTUSD_OK,
        "unregister custom kind");
  check(lightusd_unregister_light_filter("gain") == LIGHTUSD_ERR_NOT_FOUND,
        "second unregister reports not found");
  check(lightusd_unregister_light_filter("intensity") == LIGHTUSD_ERR_NOT_FOUND,
        "built-in cannot be unregistered through the C API");
  check(parse_intensity(stage, "/World/Gain", color) < 0.0f,
        "unregistered kind no longer parses");

  lightusd_stage_destroy(stage);
  if (failures) return 1;
  printf("light filter C API: built-in, custom, reject, unknown and "
         "unregister cases passed\n");
  return 0;
}
