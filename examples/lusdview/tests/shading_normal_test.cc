// SPDX-License-Identifier: Apache-2.0
// Analytic checks for the CPU tangent-space normal-mapping transform.
//
// The Vulkan RT shader reconstructs T/B from the hit triangle's positions and
// authored UVs; this locks the CPU equivalent to exact vector math so a normal
// map cannot silently become a no-op or an incorrectly-oriented perturbation.

#include "cpu/shading-normal.hh"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

bool Near(float a, float b, float eps = 1.0e-5f) {
  return std::fabs(a - b) <= eps;
}

bool NearVec(const float v[3], float x, float y, float z,
             float eps = 1.0e-5f) {
  return Near(v[0], x, eps) && Near(v[1], y, eps) && Near(v[2], z, eps);
}

bool Unit(const float v[3]) {
  return Near(v[0] * v[0] + v[1] * v[1] + v[2] * v[2], 1.0f, 1.0e-5f);
}

}  // namespace

int main() {
  using namespace lusdview;

  // 1. Unit quad on the XZ plane: +Y normal, U along +X, V along +Z. The
  //    bitangent is N x T, so it points along -Z for a right-handed frame.
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 0, 1};
    const float uv0[2] = {0, 0}, uv1[2] = {1, 0}, uv2[2] = {0, 1};
    const float n[3] = {0, 1, 0};
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);
    Check(NearVec(f.tangent, 1, 0, 0), "XZ tangent is +X");
    Check(NearVec(f.bitangent, 0, 0, -1), "XZ bitangent is -Z");
    Check(NearVec(f.normal, 0, 1, 0), "XZ normal is +Y");

    float out[3];
    const float flat[3] = {0, 0, 1};
    ApplyShadingTangentNormal(f, flat, out);
    Check(NearVec(out, 0, 1, 0), "flat map reproduces the geometric normal");

    const float tilt[3] = {0, 0.6f, 0.8f};
    ApplyShadingTangentNormal(f, tilt, out);
    Check(NearVec(out, 0, 0.8f, -0.6f),
          "tilt about T lands on the expected axis");

    const float along_t[3] = {1, 0, 0};
    ApplyShadingTangentNormal(f, along_t, out);
    Check(NearVec(out, 1, 0, 0), "map +X maps to the tangent");
    const float along_b[3] = {0, 1, 0};
    ApplyShadingTangentNormal(f, along_b, out);
    Check(NearVec(out, 0, 0, -1), "map +Y maps to the bitangent");
  }

  // 2. Sheared UVs: the frame must be orthonormal and still align with the
  //    authored texture axes (T is Gram-Schmidt orthogonalized against N).
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {1, 0, 1};
    const float uv0[2] = {0, 0}, uv1[2] = {1, 0}, uv2[2] = {1.5f, 1};
    const float n[3] = {0, 2, 0};  // unnormalized input must be normalized
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);
    Check(Unit(f.tangent), "sheared tangent is unit length");
    Check(Unit(f.bitangent), "sheared bitangent is unit length");
    Check(Unit(f.normal), "sheared normal is unit length");
    const float dot_tn = f.tangent[0] * f.normal[0] +
                         f.tangent[1] * f.normal[1] +
                         f.tangent[2] * f.normal[2];
    Check(Near(dot_tn, 0.0f), "sheared tangent is orthogonal to the normal");
    Check(NearVec(f.normal, 0, 1, 0), "unnormalized input normal is normalized");

    float out[3];
    const float axis_n[3] = {0, 0, 1};
    ApplyShadingTangentNormal(f, axis_n, out);
    Check(NearVec(out, f.normal[0], f.normal[1], f.normal[2]),
          "map +Z returns the frame normal");
    const float axis_t[3] = {1, 0, 0};
    ApplyShadingTangentNormal(f, axis_t, out);
    Check(NearVec(out, f.tangent[0], f.tangent[1], f.tangent[2]),
          "map +X returns the frame tangent");
    const float axis_b[3] = {0, 1, 0};
    ApplyShadingTangentNormal(f, axis_b, out);
    Check(NearVec(out, f.bitangent[0], f.bitangent[1], f.bitangent[2]),
          "map +Y returns the frame bitangent");
  }

  // 2b. Mirrored UVs (negative determinant): the bitangent flips so a
  //     tangent-space +Y tilt points the opposite way, matching the shaders.
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 0, 1};
    const float uv0[2] = {0, 0}, uv1[2] = {0, 1}, uv2[2] = {1, 0};
    const float n[3] = {0, 1, 0};
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);
    Check(NearVec(f.tangent, 0, 0, 1), "mirrored tangent is +Z");
    Check(NearVec(f.bitangent, -1, 0, 0), "mirrored bitangent flips to -X");
    Check(NearVec(f.normal, 0, 1, 0), "mirrored normal is +Y");

    const float tilt[3] = {0, 0.6f, 0.8f};
    float out[3];
    ApplyShadingTangentNormal(f, tilt, out);
    Check(NearVec(out, -0.6f, 0.8f, 0),
          "mirrored tilt uses the flipped bitangent");
  }

  // 3. Degenerate UVs (zero area): the fallback tangent is built from an axis
  //    that is not parallel to the normal, and the frame stays orthonormal.
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 0, 1};
    const float uv0[2] = {0.25f, 0.25f}, uv1[2] = {0.25f, 0.25f},
                uv2[2] = {0.25f, 0.25f};
    const float n[3] = {0, 1, 0};
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);
    Check(Unit(f.tangent) && Unit(f.bitangent) && Unit(f.normal),
          "degenerate-UV frame is orthonormal");
    Check(NearVec(f.tangent, -1, 0, 0), "degenerate-UV fallback tangent is -X");
    Check(NearVec(f.bitangent, 0, 0, 1), "degenerate-UV bitangent is +Z");

    const float flat[3] = {0, 0, 1};
    float out[3];
    ApplyShadingTangentNormal(f, flat, out);
    Check(NearVec(out, 0, 1, 0), "degenerate-UV flat map is still neutral");
  }

  // 4. A Z-facing degenerate frame must not divide by zero; the fallback axis
  //    switches to +Y when the normal is nearly parallel to +Z.
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 1, 0};
    const float uv0[2] = {0, 0}, uv1[2] = {0, 0}, uv2[2] = {0, 0};
    const float n[3] = {0, 0, 1};
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);
    Check(NearVec(f.normal, 0, 0, 1), "Z-facing normal is preserved");
    Check(NearVec(f.tangent, 1, 0, 0), "Z-facing fallback tangent is +X");
    Check(NearVec(f.bitangent, 0, 1, 0), "Z-facing bitangent is +Y");
    Check(Unit(f.tangent) && Unit(f.bitangent), "Z-facing frame is orthonormal");
  }

  // 5. Malformed tangent-space input: a zero vector falls back to the frame
  //    normal, and a non-unit vector is renormalized.
  {
    const float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 0}, p2[3] = {0, 0, 1};
    const float uv0[2] = {0, 0}, uv1[2] = {1, 0}, uv2[2] = {0, 1};
    const float n[3] = {0, 1, 0};
    ShadingTangentFrame f;
    BuildShadingTangentFrame(p0, p1, p2, uv0, uv1, uv2, n, &f);

    const float zero[3] = {0, 0, 0};
    float out[3];
    ApplyShadingTangentNormal(f, zero, out);
    Check(NearVec(out, 0, 1, 0), "zero tangent normal falls back to N");

    const float length5[3] = {0, 3, 4};
    ApplyShadingTangentNormal(f, length5, out);
    Check(Unit(out), "non-unit tangent normal is renormalized");
    Check(NearVec(out, 0, 0.8f, -0.6f), "renormalization keeps the direction");
  }

  // 5. Authored tangent basis: projection, handedness and degenerate fallback.
  {
    const float n[3] = {0, 1, 0};
    ShadingTangentFrame f;
    const float t[3] = {0, 0, 1};
    const float b[3] = {1, 0, 0};
    Check(BuildAuthoredShadingFrame(t, b, n, &f),
          "authored frame accepted");
    Check(NearVec(f.tangent, 0, 0, 1), "authored tangent kept");
    Check(NearVec(f.bitangent, 1, 0, 0), "authored bitangent kept");
    Check(NearVec(f.normal, 0, 1, 0), "authored normal is the surface normal");

    float out[3];
    const float tilt[3] = {0.6f, 0, 0.8f};
    ApplyShadingTangentNormal(f, tilt, out);
    Check(NearVec(out, 0, 0.8f, 0.6f),
          "authored tilt maps through the +Z/+X/+Y frame");

    const float neg[3] = {-1, 0, 0};
    Check(BuildAuthoredShadingFrame(t, neg, n, &f),
          "authored frame with flipped handedness");
    Check(NearVec(f.bitangent, -1, 0, 0),
          "flipped authored bitangent is preserved");

    const float sheared[3] = {0.4f, 0.9f, 0.8f};
    Check(BuildAuthoredShadingFrame(sheared, b, n, &f),
          "authored tangent with a normal component");
    Check(Unit(f.tangent) &&
              Near(f.tangent[0] * f.normal[0] + f.tangent[1] * f.normal[1] +
                       f.tangent[2] * f.normal[2],
                   0.0f),
          "authored tangent is projected onto the normal plane");

    const float parallel[3] = {0, 1, 0};
    Check(!BuildAuthoredShadingFrame(parallel, b, n, &f),
          "degenerate authored tangent is rejected");
  }

  if (failures) {
    std::fprintf(stderr, "shading normal: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("shading normal: PASSED\n");
  return 0;
}
