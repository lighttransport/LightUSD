// Standalone acutest runner for the `next` schema tests.
//
// These live in their OWN executable rather than in unit-test-lightusd: linking
// lightusd_next into that binary alongside lightusd_static made its concurrency
// tests fail intermittently (the two static libs carry overlapping objects), so
// the next tests are kept isolated instead of destabilizing 997 existing ones.
#include "acutest.h"

#include "unit-next-usdskel.h"

TEST_LIST = {
    {"next_skel_bind_rest_transforms", test_next_skel_bind_rest_transforms},
    {"next_skelanim_joints_is_an_array", test_next_skelanim_joints_is_an_array},
    {"next_skelanim_rotations_are_real_first",
     test_next_skelanim_rotations_are_real_first},
    {nullptr, nullptr}};
