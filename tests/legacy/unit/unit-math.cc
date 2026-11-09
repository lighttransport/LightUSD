#ifdef _MSC_VER
#define NOMINMAX
#endif

#include <iostream>

#define TEST_NO_MAIN
#include "acutest.h"

#include "lightusd.hh"
#include "value-types.hh"
#include "unit-value-types.h"
#include "core/prim.hh"
#include "core/xform-op.hh"
#include "math-util.inc"
#include "xform.hh"
#include "unit-common.hh"

#include <cmath>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd_test;

// Helper: check if two quatf are close (component-wise)
static bool quatf_close(const lightusd::legacy::value::quatf &a, const lightusd::legacy::value::quatf &b,
                        float eps = 1e-5f) {
  // Quaternion q and -q represent the same rotation, so check both signs
  bool same_sign = (std::fabs(a.imag[0] - b.imag[0]) < eps) &&
                   (std::fabs(a.imag[1] - b.imag[1]) < eps) &&
                   (std::fabs(a.imag[2] - b.imag[2]) < eps) &&
                   (std::fabs(a.real - b.real) < eps);
  bool flip_sign = (std::fabs(a.imag[0] + b.imag[0]) < eps) &&
                   (std::fabs(a.imag[1] + b.imag[1]) < eps) &&
                   (std::fabs(a.imag[2] + b.imag[2]) < eps) &&
                   (std::fabs(a.real + b.real) < eps);
  return same_sign || flip_sign;
}

// Helper: compute quaternion length
static float quat_length(const lightusd::legacy::value::quatf &q) {
  return std::sqrt(q.imag[0] * q.imag[0] + q.imag[1] * q.imag[1] +
                   q.imag[2] * q.imag[2] + q.real * q.real);
}

// Helper: check if a 4x4 matrix upper-left 3x3 matches (for rotation comparison)
static bool matrix3x3_close(const lightusd::legacy::value::matrix4d &a, const lightusd::legacy::value::matrix4d &b,
                            double eps = 1e-6) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      if (std::fabs(a.m[i][j] - b.m[i][j]) > eps) return false;
    }
  }
  return true;
}

void math_sin_pi_test(void) {

  double kPI = math::constants<double>::pi();

  for (int i = -360; i <= 360; i++) {
    double ref_v = std::sin(double(i) * kPI / 180.0);
    double sin_v = math::sin_pi(double(i)/180.0);
    // TODO: dyanmically change eps based on input degree value.
    if (!math::is_close(ref_v, sin_v, 5*std::numeric_limits<double>::epsilon())) {
      printf("sin(%d degree) differes: ref = %lf, sin_pi = %lf\n", i, ref_v, sin_v);
      TEST_CHECK(0);
    }
  }
  TEST_CHECK(1); // ok
}

void math_cos_pi_test(void) {
  double kPI = math::constants<double>::pi();

  for (int i = -360; i <= 360; i++) {
    double ref_v = std::cos(double(i) * kPI / 180.0);
    double cos_v = math::cos_pi(double(i)/180.0);
    // TODO: dyanmically change eps based on input degree value.
    if (!math::is_close(ref_v, cos_v, 4*std::numeric_limits<double>::epsilon())) {
      printf("cos(%d degree) differes: ref = %lf, sin_pi = %lf\n", i, ref_v, cos_v);
      TEST_CHECK(0);
    }
  }
  TEST_CHECK(1); // ok
}

void math_sin_cos_pi_test(void) {

  // should exactly match(whereas std::sin/cos is not)
  TEST_CHECK(math::is_close(math::cos_pi(315.0/180.0), -math::sin_pi(315.0/180.0), 0.0));
  TEST_MSG("cos(315) = %lf, sin(315) = %lf", math::cos_pi(315.0/180.0), math::sin_pi(315.0/180.0));

  TEST_CHECK(math::is_close(math::cos_pi(225.0/180.0), math::sin_pi(225.0/180.0), 0.0));
  TEST_MSG("cos(225) = %lf, sin(225) = %lf", math::cos_pi(225.0/180.0), math::sin_pi(225.0/180.0));

  TEST_CHECK(math::is_close(-math::cos_pi(135.0/180.0), math::sin_pi(135.0/180.0), 0.0));
  TEST_MSG("cos(135) = %lf, sin(135) = %lf", math::cos_pi(135.0/180.0), math::sin_pi(135.0/180.0));

  TEST_CHECK(math::is_close(math::cos_pi(45.0/180.0), math::sin_pi(45.0/180.0), 0.0));
  TEST_MSG("cos(45) = %lf, sin(45) = %lf", math::cos_pi(45.0/180.0), math::sin_pi(45.0/180.0));

  TEST_CHECK(math::is_close(math::cos_pi(-45.0/180.0), -math::sin_pi(-45.0/180.0), 0.0));
  TEST_MSG("cos(-45) = %lf, sin(-45) = %lf", math::cos_pi(-45.0/180.0), math::sin_pi(-45.0/180.0));

  TEST_CHECK(math::is_close(math::cos_pi(-135.0/180.0), math::sin_pi(-135.0/180.0), 0.0));
  TEST_MSG("cos(-135) = %lf, sin(-135) = %lf", math::cos_pi(-135.0/180.0), math::sin_pi(-135.0/180.0));

  TEST_CHECK(math::is_close(-math::cos_pi(-225.0/180.0), math::sin_pi(-225.0/180.0), 0.0));
  TEST_MSG("cos(-225) = %lf, sin(-225) = %lf", math::cos_pi(-225.0/180.0), math::sin_pi(-225.0/180.0));

  TEST_CHECK(math::is_close(math::cos_pi(-315.0/180.0), math::sin_pi(-315.0/180.0), 0.0));
  TEST_MSG("cos(-315) = %lf, sin(-315) = %lf", math::cos_pi(-315.0/180.0), math::sin_pi(-315.0/180.0));

  // must be exactly zero
  TEST_CHECK(math::is_close(math::cos_pi(90.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::cos_pi(270.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::cos_pi(-90.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::cos_pi(-270.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::sin_pi(0.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::sin_pi(360.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::sin_pi(180.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::sin_pi(-180.0/180.0), 0.0, 0.0));
  TEST_CHECK(math::is_close(math::sin_pi(-360.0/180.0), 0.0, 0.0));
}

// ---- Quaternion tests ----

void quat_to_quaternion_test(void) {
  // to_quaternion(axis, angle) creates a quaternion from axis-angle representation
  // where angle is in degrees.

  // Identity: 0 degree rotation about any axis -> (0,0,0,1)
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{1.0f, 0.0f, 0.0f}, 0.0f);
    TEST_CHECK(float_equals(q.imag[0], 0.0f));
    TEST_CHECK(float_equals(q.imag[1], 0.0f));
    TEST_CHECK(float_equals(q.imag[2], 0.0f));
    TEST_CHECK(float_equals(q.real, 1.0f));
    TEST_MSG("Identity quat: (%f, %f, %f, %f)", q.imag[0], q.imag[1], q.imag[2], q.real);
  }

  // 180 degrees about X: (1,0,0,0)
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{1.0f, 0.0f, 0.0f}, 180.0f);
    TEST_CHECK(float_equals(q.imag[0], 1.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[1], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[2], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.real, 0.0f, 1e-6f));
    TEST_MSG("180 about X: (%f, %f, %f, %f)", q.imag[0], q.imag[1], q.imag[2], q.real);
  }

  // 90 degrees about Y: (0, sin(45), 0, cos(45)) = (0, sqrt(2)/2, 0, sqrt(2)/2)
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{0.0f, 1.0f, 0.0f}, 90.0f);
    float s2 = std::sqrt(2.0f) / 2.0f;
    TEST_CHECK(float_equals(q.imag[0], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[1], s2, 1e-6f));
    TEST_CHECK(float_equals(q.imag[2], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.real, s2, 1e-6f));
    TEST_MSG("90 about Y: (%f, %f, %f, %f)", q.imag[0], q.imag[1], q.imag[2], q.real);
  }

  // 90 degrees about Z: (0, 0, sin(45), cos(45))
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{0.0f, 0.0f, 1.0f}, 90.0f);
    float s2 = std::sqrt(2.0f) / 2.0f;
    TEST_CHECK(float_equals(q.imag[0], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[1], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[2], s2, 1e-6f));
    TEST_CHECK(float_equals(q.real, s2, 1e-6f));
  }

  // -90 degrees about X: (-sin(45), 0, 0, cos(45))
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{1.0f, 0.0f, 0.0f}, -90.0f);
    float s2 = std::sqrt(2.0f) / 2.0f;
    TEST_CHECK(float_equals(q.imag[0], -s2, 1e-6f));
    TEST_CHECK(float_equals(q.imag[1], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.imag[2], 0.0f, 1e-6f));
    TEST_CHECK(float_equals(q.real, s2, 1e-6f));
  }

  // All quaternions from to_quaternion should be unit quaternions
  {
    float test_angles[] = {0, 30, 45, 60, 90, 120, 135, 180, 270, 360, -45, -90, -180};
    lightusd::legacy::value::float3 axes[] = {{1,0,0}, {0,1,0}, {0,0,1}};
    for (auto &axis : axes) {
      for (float angle : test_angles) {
        lightusd::legacy::value::quatf q = to_quaternion(axis, angle);
        float len = quat_length(q);
        TEST_CHECK(float_equals(len, 1.0f, 1e-5f));
        if (!float_equals(len, 1.0f, 1e-5f)) {
          TEST_MSG("Non-unit quat for axis (%f,%f,%f) angle %f: len=%f",
                   axis[0], axis[1], axis[2], angle, len);
        }
      }
    }
  }

  // Double-precision version
  {
    lightusd::legacy::value::quatd q = to_quaternion(lightusd::legacy::value::double3{0.0, 1.0, 0.0}, 90.0);
    double s2 = std::sqrt(2.0) / 2.0;
    double eps = std::numeric_limits<double>::epsilon() * 10;
    TEST_CHECK(float_equals(q.imag[0], 0.0, eps));
    TEST_CHECK(float_equals(q.imag[1], s2, eps));
    TEST_CHECK(float_equals(q.imag[2], 0.0, eps));
    TEST_CHECK(float_equals(q.real, s2, eps));
  }
}

void quat_to_matrix_roundtrip_test(void) {
  // to_quaternion -> to_matrix should produce the same rotation matrix as
  // directly building the rotation matrix via xformOp evaluation.

  // 90 degrees about X
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{1.0f, 0.0f, 0.0f}, 90.0f);
    lightusd::legacy::value::matrix4d mq = to_matrix(q);

    // Expected: Rx(90) = (1,0,0; 0,0,-1; 0,1,0)
    // Note: row-major convention in lightusd, but rotation matrices from quat
    // should match EvaluateXformOps result.
    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.set_value(lightusd::legacy::value::double3{90.0, 0.0, 0.0});
    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d mref;
    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &mref, &resetXformStack, &err);
    TEST_CHECK(ret);
    TEST_CHECK(matrix3x3_close(mq, mref, 1e-6));
    if (!matrix3x3_close(mq, mref, 1e-6)) {
      TEST_MSG("Rx(90) quat->matrix mismatch");
    }
  }

  // 45 degrees about Y
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{0.0f, 1.0f, 0.0f}, 45.0f);
    lightusd::legacy::value::matrix4d mq = to_matrix(q);

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.set_value(lightusd::legacy::value::double3{0.0, 45.0, 0.0});
    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d mref;
    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &mref, &resetXformStack, &err);
    TEST_CHECK(ret);
    TEST_CHECK(matrix3x3_close(mq, mref, 1e-6));
  }

  // Arbitrary: 33.3 degrees about Z
  {
    lightusd::legacy::value::quatf q = to_quaternion(lightusd::legacy::value::float3{0.0f, 0.0f, 1.0f}, 33.3f);
    lightusd::legacy::value::matrix4d mq = to_matrix(q);

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.set_value(lightusd::legacy::value::double3{0.0, 0.0, 33.3});
    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d mref;
    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &mref, &resetXformStack, &err);
    TEST_CHECK(ret);
    TEST_CHECK(matrix3x3_close(mq, mref, 1e-6));
  }
}

void quat_operator_bracket_test(void) {
  // Test that quatf operator[] gives correct indexing (x,y,z,w)
  lightusd::legacy::value::quatf q;
  q.imag[0] = 1.0f;
  q.imag[1] = 2.0f;
  q.imag[2] = 3.0f;
  q.real = 4.0f;

  TEST_CHECK(float_equals(q[0], 1.0f));
  TEST_CHECK(float_equals(q[1], 2.0f));
  TEST_CHECK(float_equals(q[2], 3.0f));
  TEST_CHECK(float_equals(q[3], 4.0f));

  // Test write through operator[]
  q[0] = 10.0f;
  q[1] = 20.0f;
  q[2] = 30.0f;
  q[3] = 40.0f;
  TEST_CHECK(float_equals(q.imag[0], 10.0f));
  TEST_CHECK(float_equals(q.imag[1], 20.0f));
  TEST_CHECK(float_equals(q.imag[2], 30.0f));
  TEST_CHECK(float_equals(q.real, 40.0f));

  // Same for quatd
  lightusd::legacy::value::quatd qd;
  qd.imag[0] = 1.0;
  qd.imag[1] = 2.0;
  qd.imag[2] = 3.0;
  qd.real = 4.0;
  TEST_CHECK(float_equals(qd[0], 1.0));
  TEST_CHECK(float_equals(qd[1], 2.0));
  TEST_CHECK(float_equals(qd[2], 3.0));
  TEST_CHECK(float_equals(qd[3], 4.0));
}

void quat_decompose_roundtrip_test(void) {
  // Build a TRS matrix, decompose it, check the quaternion matches

  // Pure rotation: RotateXYZ(30, 45, 60)
  {
    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.set_value(lightusd::legacy::value::double3{30.0, 45.0, 60.0});
    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &m, &resetXformStack, &err);
    TEST_CHECK(ret);

    lightusd::legacy::value::double3 t, s;
    lightusd::legacy::value::quatd rot;
    ret = decompose(m, &t, &rot, &s);
    TEST_CHECK(ret);

    // Translation should be zero
    TEST_CHECK(float_equals(t[0], 0.0, 1e-10));
    TEST_CHECK(float_equals(t[1], 0.0, 1e-10));
    TEST_CHECK(float_equals(t[2], 0.0, 1e-10));

    // Scale should be (1,1,1)
    TEST_CHECK(float_equals(s[0], 1.0, 1e-10));
    TEST_CHECK(float_equals(s[1], 1.0, 1e-10));
    TEST_CHECK(float_equals(s[2], 1.0, 1e-10));

    // Rotation quaternion -> matrix should match original
    // Use relaxed tolerance: decompose extracts rotation via Gram-Schmidt
    // which accumulates errors through sqrt and normalization
    lightusd::legacy::value::matrix4d mrot = to_matrix(rot);
    double max_err = 0.0;
    for (int ri = 0; ri < 3; ri++) {
      for (int ci = 0; ci < 3; ci++) {
        double d = std::fabs(m.m[ri][ci] - mrot.m[ri][ci]);
        if (d > max_err) max_err = d;
      }
    }
    TEST_CHECK(max_err < 1e-10);
    if (max_err >= 1e-10) {
      TEST_MSG("decompose roundtrip failed for RotateXYZ(30,45,60), max_err=%e", max_err);
    }
  }

  // TRS: translate(1,2,3) + rotate(10,20,30) + scale(2,3,4)
  {
    Xformable x;
    {
      XformOp op;
      op.op_type = XformOp::OpType::Translate;
      op.set_value(lightusd::legacy::value::double3{1.0, 2.0, 3.0});
      x.xformOps.push_back(op);
    }
    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateXYZ;
      op.set_value(lightusd::legacy::value::double3{10.0, 20.0, 30.0});
      x.xformOps.push_back(op);
    }
    {
      XformOp op;
      op.op_type = XformOp::OpType::Scale;
      op.set_value(lightusd::legacy::value::double3{2.0, 3.0, 4.0});
      x.xformOps.push_back(op);
    }

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &m, &resetXformStack, &err);
    TEST_CHECK(ret);

    lightusd::legacy::value::double3 t, s;
    lightusd::legacy::value::quatd rot;
    ret = decompose(m, &t, &rot, &s);
    TEST_CHECK(ret);

    // Translation
    TEST_CHECK(float_equals(t[0], 1.0, 1e-10));
    TEST_CHECK(float_equals(t[1], 2.0, 1e-10));
    TEST_CHECK(float_equals(t[2], 3.0, 1e-10));

    // Scale
    TEST_CHECK(float_equals(s[0], 2.0, 1e-6));
    TEST_CHECK(float_equals(s[1], 3.0, 1e-6));
    TEST_CHECK(float_equals(s[2], 4.0, 1e-6));
  }
}

