// Standalone acutest runner for the core schema tests.
//
// These live in their OWN executable rather than in unit-test-lightusd: linking
// lightusd_core into that binary alongside lightusd_static made its concurrency
// tests fail intermittently (the two static libs carry overlapping objects), so
// the core tests are kept isolated instead of destabilizing the legacy suite.
#include "acutest.h"

#include "unit-core-usdskel.h"

TEST_LIST = {
    {"core_skel_bind_rest_transforms", test_core_skel_bind_rest_transforms},
    {"core_skelanim_joints_is_an_array", test_core_skelanim_joints_is_an_array},
    {"core_skelanim_rotations_are_real_first",
     test_core_skelanim_rotations_are_real_first},
    {nullptr, nullptr}};
