#pragma once

// Regression tests for the core UsdSkel schema readers (src/schema/usd-skel.cc).
void test_core_skel_bind_rest_transforms(void);
void test_core_skelanim_joints_is_an_array(void);
void test_core_skelanim_rotations_are_real_first(void);
