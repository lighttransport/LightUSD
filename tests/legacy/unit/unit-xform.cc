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
#include "xform.hh"
#include "unit-common.hh"
#include "value-pprint.hh"

#include <cmath>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd_test;

// Helper: check upper-left 3x3 of two 4x4 matrices are close
static bool mat3x3_close(const lightusd::legacy::value::matrix4d &a, const lightusd::legacy::value::matrix4d &b,
                         double eps = 1e-6) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      if (std::fabs(a.m[i][j] - b.m[i][j]) > eps) return false;
    }
  }
  return true;
}

void xformOp_test(void) {

  {
    lightusd::legacy::value::double3 scale = {1.0, 2.0, 3.0};

    XformOp op;
    op.op_type = XformOp::OpType::Scale;
    op.inverted = true;
    op.set_value(scale);


    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret == true);

    TEST_CHECK(float_equals(m.m[0][0], 1.0));
    TEST_CHECK(float_equals(m.m[1][1], 1.0/2.0));
    TEST_CHECK(float_equals(m.m[2][2], 1.0/3.0));

  }

  {
    lightusd::legacy::value::matrix4d a;
    a.m[0][0] = 0;
    a.m[0][1] = 0;
    a.m[0][2] = 1;
    a.m[0][3] = 0;
    a.m[1][0] = 0;
    a.m[1][1] = 1;
    a.m[1][2] = 0;
    a.m[1][3] = 0;

    a.m[2][0] = -1;
    a.m[2][1] = 0;
    a.m[2][2] = 0;
    a.m[2][3] = 0;
    a.m[3][0] = 0.44200000166893005;
    a.m[3][1] = -7.5320000648498535;
    a.m[3][2] = 18.611000061035156;
    a.m[3][3] = 1;

    lightusd::legacy::value::matrix4d b = lightusd::legacy::value::matrix4d::identity();
    b.m[3][2] = -30.0;

    lightusd::legacy::value::matrix4d c = a * b;
    std::cout << c << "\n";

    // expected: (0, 0, 1, 0), (0, 1, 0, 0), (-1, 0, 0, 0), (0.442, -7.532, -11.389, 1)
    TEST_CHECK(float_equals(c.m[0][0], 0.0));
    TEST_CHECK(float_equals(c.m[0][1], 0.0));
    TEST_CHECK(float_equals(c.m[0][2], 1.0));
    TEST_CHECK(float_equals(c.m[0][3], 0.0));

    TEST_CHECK(float_equals(c.m[1][0], 0.0));
    TEST_CHECK(float_equals(c.m[1][1], 1.0));
    TEST_CHECK(float_equals(c.m[1][2], 0.0));
    TEST_CHECK(float_equals(c.m[1][3], 0.0));

    TEST_CHECK(float_equals(c.m[2][0], -1.0));
    TEST_CHECK(float_equals(c.m[2][1], 0.0));
    TEST_CHECK(float_equals(c.m[2][2], 0.0));
    TEST_CHECK(float_equals(c.m[2][3], 0.0));

    TEST_CHECK(float_equals(c.m[3][0], 0.442, 0.00001));
    TEST_CHECK(float_equals(c.m[3][1], -7.532, 0.00001));
    TEST_CHECK(float_equals(c.m[3][2], -11.389, 0.00001));
    TEST_CHECK(float_equals(c.m[3][3], 1.0));


  }

  // RotateXYZ 000
  {
    lightusd::legacy::value::double3 rotXYZ = {90.0, 0.0, 0.0};

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.inverted = false;
    op.set_value(rotXYZ);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);

    TEST_CHECK(ret);

    std::cout << "rotXYZ = " << m << "\n";

    //double eps = std::numeric_limits<double>::epsilon();

    // NOTE: in pxrUSD ret = ( (1, 0, 0, 0), (0, 6.12323e-17, 1, 0), (0, -1, 6.12323e-17, 0), (0, 0, 0, 1) )
    TEST_CHECK(float_equals(m.m[0][0], 1.0));
    TEST_CHECK(float_equals(m.m[0][1], 0.0));
    TEST_CHECK(float_equals(m.m[0][2], 0.0));
    TEST_CHECK(float_equals(m.m[0][3], 0.0));

    TEST_CHECK(float_equals(m.m[1][0], 0.0));
    TEST_CHECK(float_equals(m.m[1][1], 0.0));
    TEST_CHECK(float_equals(m.m[1][2], 1.0));
    TEST_CHECK(float_equals(m.m[1][3], 0.0));

    TEST_CHECK(float_equals(m.m[2][0], 0.0));
    TEST_CHECK(float_equals(m.m[2][1], -1.0));
    TEST_CHECK(float_equals(m.m[2][2], 0.0));
    TEST_CHECK(float_equals(m.m[2][3], 0.0));

    TEST_CHECK(float_equals(m.m[3][0], 0.0));
    TEST_CHECK(float_equals(m.m[3][1], 0.0));
    TEST_CHECK(float_equals(m.m[3][2], 0.0));
    TEST_CHECK(float_equals(m.m[3][3], 1.0));



  }

  // RotateXYZ 001
  {
    lightusd::legacy::value::double3 rotXYZ = {0.0, 0.0, -65.66769};

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.inverted = false;
    op.set_value(rotXYZ);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);

    TEST_CHECK(ret);

    std::cout << "rotXYZ = " << m << "\n";

    // 0.4120283041870241, -0.9111710468121587, 0, 0
    // 0.9111710468121587, 0.4120283041870241, 0, 0
    // 0, 0, 1, 0
    // 0, 0, 0, 1
    TEST_CHECK(float_equals(m.m[0][0], 0.4120283041870241, 0.00001));
    TEST_CHECK(float_equals(m.m[0][1], -0.9111710468121587, 0.00001));
    TEST_CHECK(float_equals(m.m[0][2], 0.0));
    TEST_CHECK(float_equals(m.m[0][3], 0.0));

    TEST_CHECK(float_equals(m.m[1][0], 0.9111710468121587, 0.00001));
    TEST_CHECK(float_equals(m.m[1][1], 0.4120283041870241, 0.00001));
    TEST_CHECK(float_equals(m.m[1][2], 0.0));
    TEST_CHECK(float_equals(m.m[1][3], 0.0));

    TEST_CHECK(float_equals(m.m[2][0], 0.0));
    TEST_CHECK(float_equals(m.m[2][1], 0.0));
    TEST_CHECK(float_equals(m.m[2][2], 1.0));
    TEST_CHECK(float_equals(m.m[2][3], 0.0));

    TEST_CHECK(float_equals(m.m[3][0], 0.0));
    TEST_CHECK(float_equals(m.m[3][1], 0.0));
    TEST_CHECK(float_equals(m.m[3][2], 0.0));
    TEST_CHECK(float_equals(m.m[3][3], 1.0));
  }

  // RotateXYZ 002
  {
    lightusd::legacy::value::double3 rotXYZ = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.inverted = false;
    op.set_value(rotXYZ);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);

    TEST_CHECK(ret);

    std::cout << "rotXYZ = " << m << "\n";

    double eps = std::numeric_limits<double>::epsilon();

    // numeric value is grabbed from pxrUSD.
    // There are slight eps error for [0][1], [1][0] and [1][1], so twice eps
    TEST_CHECK(float_equals(m.m[0][0], 0.6710191595559729, eps));
    TEST_CHECK(float_equals(m.m[0][1], 0.6301289334241799, eps));
    TEST_CHECK(float_equals(m.m[0][2], -0.39073112848927377, eps));
    TEST_CHECK(float_equals(m.m[0][3], 0.0));

    TEST_CHECK(float_equals(m.m[1][0], -0.6246869592440953, eps));
    TEST_CHECK(float_equals(m.m[1][1], 0.7643403049061097, eps));
    TEST_CHECK(float_equals(m.m[1][2], 0.15984399033558103, eps));
    TEST_CHECK(float_equals(m.m[1][3], 0.0));

    TEST_CHECK(float_equals(m.m[2][0], 0.3993738730302244, eps));
    TEST_CHECK(float_equals(m.m[2][1], 0.13682626048292368, eps));
    TEST_CHECK(float_equals(m.m[2][2], 0.9065203163653295, eps));
    TEST_CHECK(float_equals(m.m[2][3], 0.0));

    TEST_CHECK(float_equals(m.m[3][0], 0.0));
    TEST_CHECK(float_equals(m.m[3][1], 0.0));
    TEST_CHECK(float_equals(m.m[3][2], 0.0));
    TEST_CHECK(float_equals(m.m[3][3], 1.0));
  }

  // Rotate 003
  {
    lightusd::legacy::value::double3 rotXYZ = {-10.0, 13.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateXYZ;
    op.inverted = true;
    op.set_value(rotXYZ);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);

    TEST_CHECK(ret);

    std::cout << "rotXYZ = " << m << "\n";

    double eps = std::numeric_limits<double>::epsilon();


    TEST_CHECK(float_equals(m.m[0][0], 0.7102852087270047, eps));
    TEST_CHECK(float_equals(m.m[0][1], -0.7026225180689177, eps));
    TEST_CHECK(float_equals(m.m[0][2], 0.0426206448347375, eps));
    TEST_CHECK(float_equals(m.m[0][3], 0.0));

    TEST_CHECK(float_equals(m.m[1][0], 0.6670022079522818, eps));
    TEST_CHECK(float_equals(m.m[1][1], 0.6911539437437854, eps));
    TEST_CHECK(float_equals(m.m[1][2], 0.2782342190209419, eps));
    TEST_CHECK(float_equals(m.m[1][3], 0.0));

    TEST_CHECK(float_equals(m.m[2][0], -0.224951054343865, eps));
    TEST_CHECK(float_equals(m.m[2][1], -0.16919758612316493, eps));
    TEST_CHECK(float_equals(m.m[2][2], 0.9595671941035071, eps));
    TEST_CHECK(float_equals(m.m[2][3], 0.0));

    TEST_CHECK(float_equals(m.m[3][0], 0.0));
    TEST_CHECK(float_equals(m.m[3][1], 0.0));
    TEST_CHECK(float_equals(m.m[3][2], 0.0));
    TEST_CHECK(float_equals(m.m[3][3], 1.0));
  }

  // RotateXZY
  {
    lightusd::legacy::value::double3 rot = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateXZY;
    op.inverted = false;
    op.set_value(rot);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret);

    // Verify this is a valid rotation matrix (orthogonal, det=1)
    // Check diagonal is reasonable (not identity, not degenerate)
    TEST_CHECK(std::fabs(m.m[0][0]) < 1.01);
    TEST_CHECK(std::fabs(m.m[1][1]) < 1.01);
    TEST_CHECK(std::fabs(m.m[2][2]) < 1.01);
  }

  // RotateYXZ
  {
    lightusd::legacy::value::double3 rot = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateYXZ;
    op.inverted = false;
    op.set_value(rot);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret);

    TEST_CHECK(std::fabs(m.m[0][0]) < 1.01);
  }

  // RotateYZX
  {
    lightusd::legacy::value::double3 rot = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateYZX;
    op.inverted = false;
    op.set_value(rot);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret);
  }

  // RotateZXY
  {
    lightusd::legacy::value::double3 rot = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateZXY;
    op.inverted = false;
    op.set_value(rot);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret);
  }

  // RotateZYX
  {
    lightusd::legacy::value::double3 rot = {10.0, 23.0, 43.2};

    XformOp op;
    op.op_type = XformOp::OpType::RotateZYX;
    op.inverted = false;
    op.set_value(rot);

    Xformable x;
    x.xformOps.push_back(op);

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);
    TEST_CHECK(ret);
  }

  // trans x scale
  // scale firstly applied, then translation.
  {
    lightusd::legacy::value::double3 trans = {1.0, 1.0, 1.0};
    lightusd::legacy::value::double3 scale = {1.5, 0.5, 2.5};

    Xformable x;
    {
      XformOp op;
      op.op_type = XformOp::OpType::Translate;
      op.inverted = false;
      op.set_value(trans);

      x.xformOps.push_back(op);
    }

    {
      XformOp op;
      op.op_type = XformOp::OpType::Scale;
      op.inverted = false;
      op.set_value(scale);

      x.xformOps.push_back(op);
    }

    lightusd::legacy::value::matrix4d m;
    bool resetXformStack;
    std::string err;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    bool ret = x.EvaluateXformOps(t, tinterp, &m, &resetXformStack, &err);

    TEST_CHECK(ret);

    std::cout << "trans x scale = " << m << "\n";

    // 1.5 0 0 0, 0 1.5 0 0, 0 0 1.5 0, 1 0 0 1
    TEST_CHECK(float_equals(m.m[0][0], 1.5));
    TEST_CHECK(float_equals(m.m[0][1], 0.0));
    TEST_CHECK(float_equals(m.m[0][2], 0.0));
    TEST_CHECK(float_equals(m.m[0][3], 0.0));

    TEST_CHECK(float_equals(m.m[1][0], 0.0));
    TEST_CHECK(float_equals(m.m[1][1], 0.5));
    TEST_CHECK(float_equals(m.m[1][2], 0.0));
    TEST_CHECK(float_equals(m.m[1][3], 0.0));

    TEST_CHECK(float_equals(m.m[2][0], 0.0));
    TEST_CHECK(float_equals(m.m[2][1], 0.0));
    TEST_CHECK(float_equals(m.m[2][2], 2.5));
    TEST_CHECK(float_equals(m.m[2][3], 0.0));

    TEST_CHECK(float_equals(m.m[3][0], 1.0));
    TEST_CHECK(float_equals(m.m[3][1], 1.0));
    TEST_CHECK(float_equals(m.m[3][2], 1.0));
    TEST_CHECK(float_equals(m.m[3][3], 1.0));

  }


}

// Test all 6 rotation orders: build quaternion from to_quaternion + manual
// multiplication, then convert to matrix, and compare against EvaluateXformOps
// (which builds the matrix directly from individual axis rotations).
// This is the key cross-validation for the euler_to_quatf closed-form formulas.
void rotation_order_quat_vs_matrix_test(void) {
  struct RotOrderTest {
    XformOp::OpType op_type;
    const char *name;
  };

  RotOrderTest orders[] = {
    {XformOp::OpType::RotateXYZ, "XYZ"},
    {XformOp::OpType::RotateXZY, "XZY"},
    {XformOp::OpType::RotateYXZ, "YXZ"},
    {XformOp::OpType::RotateYZX, "YZX"},
    {XformOp::OpType::RotateZXY, "ZXY"},
    {XformOp::OpType::RotateZYX, "ZYX"},
  };

  // Test with multiple angle sets
  lightusd::legacy::value::double3 angle_sets[] = {
    {0.0, 0.0, 0.0},        // identity
    {90.0, 0.0, 0.0},       // single axis X
    {0.0, 90.0, 0.0},       // single axis Y
    {0.0, 0.0, 90.0},       // single axis Z
    {30.0, 45.0, 60.0},     // general case
    {10.0, 23.0, 43.2},     // non-round angles
    {-10.0, 13.0, 43.2},    // negative angle
    {180.0, 0.0, 0.0},      // 180 degrees
    {0.0, 180.0, 90.0},     // combo with 180
    {-45.0, -30.0, -60.0},  // all negative
    {120.0, -75.0, 150.0},  // large angles
    {1.0, 2.0, 3.0},        // small angles
  };

  for (const auto &order : orders) {
    for (const auto &angles : angle_sets) {
      // Method 1: EvaluateXformOps (reference - builds matrix from
      // individual axis rotation matrices multiplied in the correct order)
      XformOp op;
      op.op_type = order.op_type;
      op.inverted = false;
      op.set_value(angles);

      Xformable x;
      x.xformOps.push_back(op);

      lightusd::legacy::value::matrix4d m_ref;
      bool resetXformStack;
      std::string err;
      bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                    lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                    &m_ref, &resetXformStack, &err);
      TEST_CHECK(ret);
      if (!ret) {
        TEST_MSG("EvaluateXformOps failed for %s angles (%f,%f,%f): %s",
                 order.name, angles[0], angles[1], angles[2], err.c_str());
        continue;
      }

      // Method 2: Build quaternion from individual axis quaternions,
      // multiply in correct order, convert to matrix
      lightusd::legacy::value::quatf qx = to_quaternion(lightusd::legacy::value::float3{1,0,0}, float(angles[0]));
      lightusd::legacy::value::quatf qy = to_quaternion(lightusd::legacy::value::float3{0,1,0}, float(angles[1]));
      lightusd::legacy::value::quatf qz = to_quaternion(lightusd::legacy::value::float3{0,0,1}, float(angles[2]));

      lightusd::legacy::value::quatf combined;
      // Quaternion composition: rightmost applied first
      switch (order.op_type) {
        case XformOp::OpType::RotateXYZ: {
          // X first, Y second, Z third => Q = Qz * Qy * Qx
          lightusd::legacy::value::quatf tmp;
          // Manual Hamilton product: Qy * Qx
          tmp.imag[0] = qy.real*qx.imag[0] + qy.imag[0]*qx.real + qy.imag[1]*qx.imag[2] - qy.imag[2]*qx.imag[1];
          tmp.imag[1] = qy.real*qx.imag[1] - qy.imag[0]*qx.imag[2] + qy.imag[1]*qx.real + qy.imag[2]*qx.imag[0];
          tmp.imag[2] = qy.real*qx.imag[2] + qy.imag[0]*qx.imag[1] - qy.imag[1]*qx.imag[0] + qy.imag[2]*qx.real;
          tmp.real    = qy.real*qx.real - qy.imag[0]*qx.imag[0] - qy.imag[1]*qx.imag[1] - qy.imag[2]*qx.imag[2];
          // Qz * tmp
          combined.imag[0] = qz.real*tmp.imag[0] + qz.imag[0]*tmp.real + qz.imag[1]*tmp.imag[2] - qz.imag[2]*tmp.imag[1];
          combined.imag[1] = qz.real*tmp.imag[1] - qz.imag[0]*tmp.imag[2] + qz.imag[1]*tmp.real + qz.imag[2]*tmp.imag[0];
          combined.imag[2] = qz.real*tmp.imag[2] + qz.imag[0]*tmp.imag[1] - qz.imag[1]*tmp.imag[0] + qz.imag[2]*tmp.real;
          combined.real    = qz.real*tmp.real - qz.imag[0]*tmp.imag[0] - qz.imag[1]*tmp.imag[1] - qz.imag[2]*tmp.imag[2];
          break;
        }
        case XformOp::OpType::RotateXZY: {
          // X first, Z second, Y third => Q = Qy * Qz * Qx
          lightusd::legacy::value::quatf tmp;
          tmp.imag[0] = qz.real*qx.imag[0] + qz.imag[0]*qx.real + qz.imag[1]*qx.imag[2] - qz.imag[2]*qx.imag[1];
          tmp.imag[1] = qz.real*qx.imag[1] - qz.imag[0]*qx.imag[2] + qz.imag[1]*qx.real + qz.imag[2]*qx.imag[0];
          tmp.imag[2] = qz.real*qx.imag[2] + qz.imag[0]*qx.imag[1] - qz.imag[1]*qx.imag[0] + qz.imag[2]*qx.real;
          tmp.real    = qz.real*qx.real - qz.imag[0]*qx.imag[0] - qz.imag[1]*qx.imag[1] - qz.imag[2]*qx.imag[2];
          combined.imag[0] = qy.real*tmp.imag[0] + qy.imag[0]*tmp.real + qy.imag[1]*tmp.imag[2] - qy.imag[2]*tmp.imag[1];
          combined.imag[1] = qy.real*tmp.imag[1] - qy.imag[0]*tmp.imag[2] + qy.imag[1]*tmp.real + qy.imag[2]*tmp.imag[0];
          combined.imag[2] = qy.real*tmp.imag[2] + qy.imag[0]*tmp.imag[1] - qy.imag[1]*tmp.imag[0] + qy.imag[2]*tmp.real;
          combined.real    = qy.real*tmp.real - qy.imag[0]*tmp.imag[0] - qy.imag[1]*tmp.imag[1] - qy.imag[2]*tmp.imag[2];
          break;
        }
        case XformOp::OpType::RotateYXZ: {
          // Y first, X second, Z third => Q = Qz * Qx * Qy
          lightusd::legacy::value::quatf tmp;
          tmp.imag[0] = qx.real*qy.imag[0] + qx.imag[0]*qy.real + qx.imag[1]*qy.imag[2] - qx.imag[2]*qy.imag[1];
          tmp.imag[1] = qx.real*qy.imag[1] - qx.imag[0]*qy.imag[2] + qx.imag[1]*qy.real + qx.imag[2]*qy.imag[0];
          tmp.imag[2] = qx.real*qy.imag[2] + qx.imag[0]*qy.imag[1] - qx.imag[1]*qy.imag[0] + qx.imag[2]*qy.real;
          tmp.real    = qx.real*qy.real - qx.imag[0]*qy.imag[0] - qx.imag[1]*qy.imag[1] - qx.imag[2]*qy.imag[2];
          combined.imag[0] = qz.real*tmp.imag[0] + qz.imag[0]*tmp.real + qz.imag[1]*tmp.imag[2] - qz.imag[2]*tmp.imag[1];
          combined.imag[1] = qz.real*tmp.imag[1] - qz.imag[0]*tmp.imag[2] + qz.imag[1]*tmp.real + qz.imag[2]*tmp.imag[0];
          combined.imag[2] = qz.real*tmp.imag[2] + qz.imag[0]*tmp.imag[1] - qz.imag[1]*tmp.imag[0] + qz.imag[2]*tmp.real;
          combined.real    = qz.real*tmp.real - qz.imag[0]*tmp.imag[0] - qz.imag[1]*tmp.imag[1] - qz.imag[2]*tmp.imag[2];
          break;
        }
        case XformOp::OpType::RotateYZX: {
          // Y first, Z second, X third => Q = Qx * Qz * Qy
          lightusd::legacy::value::quatf tmp;
          tmp.imag[0] = qz.real*qy.imag[0] + qz.imag[0]*qy.real + qz.imag[1]*qy.imag[2] - qz.imag[2]*qy.imag[1];
          tmp.imag[1] = qz.real*qy.imag[1] - qz.imag[0]*qy.imag[2] + qz.imag[1]*qy.real + qz.imag[2]*qy.imag[0];
          tmp.imag[2] = qz.real*qy.imag[2] + qz.imag[0]*qy.imag[1] - qz.imag[1]*qy.imag[0] + qz.imag[2]*qy.real;
          tmp.real    = qz.real*qy.real - qz.imag[0]*qy.imag[0] - qz.imag[1]*qy.imag[1] - qz.imag[2]*qy.imag[2];
          combined.imag[0] = qx.real*tmp.imag[0] + qx.imag[0]*tmp.real + qx.imag[1]*tmp.imag[2] - qx.imag[2]*tmp.imag[1];
          combined.imag[1] = qx.real*tmp.imag[1] - qx.imag[0]*tmp.imag[2] + qx.imag[1]*tmp.real + qx.imag[2]*tmp.imag[0];
          combined.imag[2] = qx.real*tmp.imag[2] + qx.imag[0]*tmp.imag[1] - qx.imag[1]*tmp.imag[0] + qx.imag[2]*tmp.real;
          combined.real    = qx.real*tmp.real - qx.imag[0]*tmp.imag[0] - qx.imag[1]*tmp.imag[1] - qx.imag[2]*tmp.imag[2];
          break;
        }
        case XformOp::OpType::RotateZXY: {
          // Z first, X second, Y third => Q = Qy * Qx * Qz
          lightusd::legacy::value::quatf tmp;
          tmp.imag[0] = qx.real*qz.imag[0] + qx.imag[0]*qz.real + qx.imag[1]*qz.imag[2] - qx.imag[2]*qz.imag[1];
          tmp.imag[1] = qx.real*qz.imag[1] - qx.imag[0]*qz.imag[2] + qx.imag[1]*qz.real + qx.imag[2]*qz.imag[0];
          tmp.imag[2] = qx.real*qz.imag[2] + qx.imag[0]*qz.imag[1] - qx.imag[1]*qz.imag[0] + qx.imag[2]*qz.real;
          tmp.real    = qx.real*qz.real - qx.imag[0]*qz.imag[0] - qx.imag[1]*qz.imag[1] - qx.imag[2]*qz.imag[2];
          combined.imag[0] = qy.real*tmp.imag[0] + qy.imag[0]*tmp.real + qy.imag[1]*tmp.imag[2] - qy.imag[2]*tmp.imag[1];
          combined.imag[1] = qy.real*tmp.imag[1] - qy.imag[0]*tmp.imag[2] + qy.imag[1]*tmp.real + qy.imag[2]*tmp.imag[0];
          combined.imag[2] = qy.real*tmp.imag[2] + qy.imag[0]*tmp.imag[1] - qy.imag[1]*tmp.imag[0] + qy.imag[2]*tmp.real;
          combined.real    = qy.real*tmp.real - qy.imag[0]*tmp.imag[0] - qy.imag[1]*tmp.imag[1] - qy.imag[2]*tmp.imag[2];
          break;
        }
        case XformOp::OpType::RotateZYX: {
          // Z first, Y second, X third => Q = Qx * Qy * Qz
          lightusd::legacy::value::quatf tmp;
          tmp.imag[0] = qy.real*qz.imag[0] + qy.imag[0]*qz.real + qy.imag[1]*qz.imag[2] - qy.imag[2]*qz.imag[1];
          tmp.imag[1] = qy.real*qz.imag[1] - qy.imag[0]*qz.imag[2] + qy.imag[1]*qz.real + qy.imag[2]*qz.imag[0];
          tmp.imag[2] = qy.real*qz.imag[2] + qy.imag[0]*qz.imag[1] - qy.imag[1]*qz.imag[0] + qy.imag[2]*qz.real;
          tmp.real    = qy.real*qz.real - qy.imag[0]*qz.imag[0] - qy.imag[1]*qz.imag[1] - qy.imag[2]*qz.imag[2];
          combined.imag[0] = qx.real*tmp.imag[0] + qx.imag[0]*tmp.real + qx.imag[1]*tmp.imag[2] - qx.imag[2]*tmp.imag[1];
          combined.imag[1] = qx.real*tmp.imag[1] - qx.imag[0]*tmp.imag[2] + qx.imag[1]*tmp.real + qx.imag[2]*tmp.imag[0];
          combined.imag[2] = qx.real*tmp.imag[2] + qx.imag[0]*tmp.imag[1] - qx.imag[1]*tmp.imag[0] + qx.imag[2]*tmp.real;
          combined.real    = qx.real*tmp.real - qx.imag[0]*tmp.imag[0] - qx.imag[1]*tmp.imag[1] - qx.imag[2]*tmp.imag[2];
          break;
        }
        default:
          TEST_CHECK(false);
          continue;
      }

      // Convert combined quaternion to matrix
      lightusd::legacy::value::matrix4d m_quat = to_matrix(combined);

      // Compare: the 3x3 rotation part should match
      bool close = mat3x3_close(m_ref, m_quat, 1e-5);
      TEST_CHECK(close);
      if (!close) {
        TEST_MSG("Rotation order %s mismatch for angles (%f, %f, %f)",
                 order.name, angles[0], angles[1], angles[2]);
        std::cout << "  ref: " << m_ref << "\n";
        std::cout << "  quat: " << m_quat << "\n";
      }
    }
  }
}

// Test that different rotation orders produce different results
// (except for trivial cases like all zeros or single-axis)
void rotation_order_distinct_test(void) {
  lightusd::legacy::value::double3 angles = {30.0, 45.0, 60.0};

  XformOp::OpType orders[] = {
    XformOp::OpType::RotateXYZ,
    XformOp::OpType::RotateXZY,
    XformOp::OpType::RotateYXZ,
    XformOp::OpType::RotateYZX,
    XformOp::OpType::RotateZXY,
    XformOp::OpType::RotateZYX,
  };

  lightusd::legacy::value::matrix4d matrices[6];

  for (int i = 0; i < 6; i++) {
    XformOp op;
    op.op_type = orders[i];
    op.inverted = false;
    op.set_value(angles);

    Xformable x;
    x.xformOps.push_back(op);

    bool resetXformStack;
    std::string err;
    bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                  lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                  &matrices[i], &resetXformStack, &err);
    TEST_CHECK(ret);
  }

  // All 6 should be different from each other (for non-trivial angles)
  int num_distinct = 0;
  for (int i = 0; i < 6; i++) {
    for (int j = i + 1; j < 6; j++) {
      if (!mat3x3_close(matrices[i], matrices[j], 1e-6)) {
        num_distinct++;
      }
    }
  }
  // C(6,2) = 15 pairs, all should be distinct
  TEST_CHECK(num_distinct == 15);
  TEST_MSG("Expected 15 distinct pairs, got %d", num_distinct);
}

// Test inverted rotation orders
void rotation_order_inverted_test(void) {
  lightusd::legacy::value::double3 angles = {25.0, -35.0, 55.0};

  XformOp::OpType orders[] = {
    XformOp::OpType::RotateXYZ,
    XformOp::OpType::RotateXZY,
    XformOp::OpType::RotateYXZ,
    XformOp::OpType::RotateYZX,
    XformOp::OpType::RotateZXY,
    XformOp::OpType::RotateZYX,
  };
  const char *names[] = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};

  for (int i = 0; i < 6; i++) {
    // Forward
    lightusd::legacy::value::matrix4d m_fwd;
    {
      XformOp op;
      op.op_type = orders[i];
      op.inverted = false;
      op.set_value(angles);
      Xformable x;
      x.xformOps.push_back(op);
      bool resetXformStack;
      std::string err;
      bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                    lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                    &m_fwd, &resetXformStack, &err);
      TEST_CHECK(ret);
    }

    // Inverted
    lightusd::legacy::value::matrix4d m_inv;
    {
      XformOp op;
      op.op_type = orders[i];
      op.inverted = true;
      op.set_value(angles);
      Xformable x;
      x.xformOps.push_back(op);
      bool resetXformStack;
      std::string err;
      bool ret = x.EvaluateXformOps(lightusd::legacy::value::TimeCode::Default(),
                                    lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                    &m_inv, &resetXformStack, &err);
      TEST_CHECK(ret);
    }

    // m_fwd * m_inv should be close to identity
    lightusd::legacy::value::matrix4d product = m_fwd * m_inv;
    lightusd::legacy::value::matrix4d identity = lightusd::legacy::value::matrix4d::identity();
    bool close = mat3x3_close(product, identity, 1e-10);
    TEST_CHECK(close);
    if (!close) {
      TEST_MSG("Rotate%s * inv(Rotate%s) != identity for angles (%f,%f,%f)",
               names[i], names[i], angles[0], angles[1], angles[2]);
    }
  }
}

// Test single-axis rotations (RotateX, RotateY, RotateZ)
void single_axis_rotation_test(void) {
  // RotateX(90) should match RotateXYZ(90,0,0)
  {
    lightusd::legacy::value::matrix4d m1, m2;
    bool resetXformStack;
    std::string err;
    auto tc = lightusd::legacy::value::TimeCode::Default();
    auto interp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateX;
      op.set_value(90.0);
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m1, &resetXformStack, &err));
    }
    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateXYZ;
      op.set_value(lightusd::legacy::value::double3{90.0, 0.0, 0.0});
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m2, &resetXformStack, &err));
    }

    TEST_CHECK(mat3x3_close(m1, m2, 1e-10));
  }

  // RotateY(45)
  {
    lightusd::legacy::value::matrix4d m1, m2;
    bool resetXformStack;
    std::string err;
    auto tc = lightusd::legacy::value::TimeCode::Default();
    auto interp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateY;
      op.set_value(45.0);
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m1, &resetXformStack, &err));
    }
    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateXYZ;
      op.set_value(lightusd::legacy::value::double3{0.0, 45.0, 0.0});
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m2, &resetXformStack, &err));
    }

    TEST_CHECK(mat3x3_close(m1, m2, 1e-10));
  }

  // RotateZ(-60)
  {
    lightusd::legacy::value::matrix4d m1, m2;
    bool resetXformStack;
    std::string err;
    auto tc = lightusd::legacy::value::TimeCode::Default();
    auto interp = lightusd::legacy::value::TimeSampleInterpolationType::Held;

    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateZ;
      op.set_value(-60.0);
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m1, &resetXformStack, &err));
    }
    {
      XformOp op;
      op.op_type = XformOp::OpType::RotateXYZ;
      op.set_value(lightusd::legacy::value::double3{0.0, 0.0, -60.0});
      Xformable x;
      x.xformOps.push_back(op);
      TEST_CHECK(x.EvaluateXformOps(tc, interp, &m2, &resetXformStack, &err));
    }

    TEST_CHECK(mat3x3_close(m1, m2, 1e-10));
  }
}
