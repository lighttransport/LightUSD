#pragma once

// Regression tests for the `next` UsdSkel schema readers (src/next/schema/usd-skel.cc).
void test_next_skel_bind_rest_transforms(void);
void test_next_skelanim_joints_is_an_array(void);
void test_next_skelanim_rotations_are_real_first(void);
