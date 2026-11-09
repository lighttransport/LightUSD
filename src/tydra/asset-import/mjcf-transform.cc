// SPDX-License-Identifier: Apache-2.0
// Quaternions, pose matrices and primitive meshes.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
Quat QuatNormalize(const Quat& q) {
  const double n =
      std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  return {{q[0] / n, q[1] / n, q[2] / n, q[3] / n}};
}

// Hamilton product a * b ({w,x,y,z}).
Quat QuatMul(const Quat& a, const Quat& b) {
  return {{a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
           a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
           a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
           a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]}};
}

Quat QuatFromAxisAngle(const std::array<double, 3>& axis, double angle) {
  const double len =
      std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (len < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  const double s = std::sin(angle * 0.5) / len;
  return {{std::cos(angle * 0.5), axis[0] * s, axis[1] * s, axis[2] * s}};
}

// Rotation that maps unit vector `from` onto unit vector `to`.
Quat QuatFromTwoVecs(const std::array<double, 3>& from,
                     const std::array<double, 3>& to) {
  auto norm = [](std::array<double, 3> v) -> std::array<double, 3> {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1.0e-12) return {{0.0, 0.0, 1.0}};
    return {{v[0] / n, v[1] / n, v[2] / n}};
  };
  const std::array<double, 3> a = norm(from);
  const std::array<double, 3> b = norm(to);
  const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  if (dot > 0.999999) return {{1.0, 0.0, 0.0, 0.0}};
  if (dot < -0.999999) {
    // Antiparallel: rotate pi about an axis orthogonal to `a`. For +-Z this
    // is X, matching MuJoCo's mju_quatZ2Vec (zaxis/fromto pointing down).
    if (std::abs(a[2]) >= 0.9)
      return QuatFromAxisAngle({{1.0, 0.0, 0.0}}, 3.14159265358979323846);
    const std::array<double, 3> ortho =
        std::abs(a[0]) < 0.9 ? std::array<double, 3>{{1.0, 0.0, 0.0}}
                             : std::array<double, 3>{{0.0, 1.0, 0.0}};
    const std::array<double, 3> axis{{a[1] * ortho[2] - a[2] * ortho[1],
                                      a[2] * ortho[0] - a[0] * ortho[2],
                                      a[0] * ortho[1] - a[1] * ortho[0]}};
    return QuatFromAxisAngle(axis, 3.14159265358979323846);
  }
  const std::array<double, 3> axis{{a[1] * b[2] - a[2] * b[1],
                                    a[2] * b[0] - a[0] * b[2],
                                    a[0] * b[1] - a[1] * b[0]}};
  return QuatNormalize({{1.0 + dot, axis[0], axis[1], axis[2]}});
}

// Quaternion from a rotation matrix given as its three column basis vectors.
Quat QuatFromBasis(const std::array<double, 3>& cx,
                   const std::array<double, 3>& cy,
                   const std::array<double, 3>& cz) {
  const double m00 = cx[0], m10 = cx[1], m20 = cx[2];
  const double m01 = cy[0], m11 = cy[1], m21 = cy[2];
  const double m02 = cz[0], m12 = cz[1], m22 = cz[2];
  const double trace = m00 + m11 + m22;
  Quat q{{1.0, 0.0, 0.0, 0.0}};
  if (trace > 0.0) {
    const double s = 0.5 / std::sqrt(trace + 1.0);
    q = {{0.25 / s, (m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s}};
  } else if (m00 > m11 && m00 > m22) {
    const double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
    q = {{(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s}};
  } else if (m11 > m22) {
    const double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
    q = {{(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s}};
  } else {
    const double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
    q = {{(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s}};
  }
  return QuatNormalize(q);
}

// Resolve a MuJoCo orientation specifier (quat / axisangle / euler / xyaxes /
// zaxis) into a {w,x,y,z} quaternion, honoring <compiler angle/eulerseq>.
Quat OrientationQuat(const std::string& quat_str,
                     const std::string& axisangle_str,
                     const std::string& euler_str,
                     const std::string& xyaxes_str,
                     const std::string& zaxis_str, const Context& ctx) {
  const std::vector<double> q = ParseDoubles(quat_str);
  if (q.size() >= 4) return QuatNormalize({{q[0], q[1], q[2], q[3]}});

  const std::vector<double> aa = ParseDoubles(axisangle_str);
  if (aa.size() >= 4) {
    return QuatFromAxisAngle({{aa[0], aa[1], aa[2]}}, aa[3] * ctx.angle_to_rad);
  }

  const std::vector<double> xy = ParseDoubles(xyaxes_str);
  if (xy.size() >= 6) {
    auto norm = [](std::array<double, 3> v) -> std::array<double, 3> {
      const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (n < 1.0e-12) return {{1.0, 0.0, 0.0}};
      return {{v[0] / n, v[1] / n, v[2] / n}};
    };
    const std::array<double, 3> x = norm({{xy[0], xy[1], xy[2]}});
    std::array<double, 3> y{{xy[3], xy[4], xy[5]}};
    const double d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    y = norm({{y[0] - d * x[0], y[1] - d * x[1], y[2] - d * x[2]}});
    const std::array<double, 3> z{{x[1] * y[2] - x[2] * y[1],
                                   x[2] * y[0] - x[0] * y[2],
                                   x[0] * y[1] - x[1] * y[0]}};
    return QuatFromBasis(x, y, z);
  }

  const std::vector<double> za = ParseDoubles(zaxis_str);
  if (za.size() >= 3) {
    return QuatFromTwoVecs({{0.0, 0.0, 1.0}}, {{za[0], za[1], za[2]}});
  }

  const std::vector<double> e = ParseDoubles(euler_str);
  if (e.size() >= 3) {
    Quat out{{1.0, 0.0, 0.0, 0.0}};
    for (size_t i = 0; i < 3 && i < ctx.eulerseq.size(); i++) {
      const char c = ctx.eulerseq[i];
      const char lower =
          static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::array<double, 3> axis{{0.0, 0.0, 0.0}};
      if (lower == 'x')
        axis = {{1.0, 0.0, 0.0}};
      else if (lower == 'y')
        axis = {{0.0, 1.0, 0.0}};
      else if (lower == 'z')
        axis = {{0.0, 0.0, 1.0}};
      else
        continue;
      const Quat qi = QuatFromAxisAngle(axis, e[i] * ctx.angle_to_rad);
      // Lowercase = intrinsic (post-multiply); uppercase = extrinsic (pre).
      out = std::islower(static_cast<unsigned char>(c)) ? QuatMul(out, qi)
                                                        : QuatMul(qi, out);
    }
    return QuatNormalize(out);
  }

  return {{1.0, 0.0, 0.0, 0.0}};
}

// Three.js Matrix4.elements (column-major) flat array from pos + quat.
std::vector<double> MatrixFromPosQuat(const std::array<double, 3>& pos,
                                      const Quat& q) {
  const double w = q[0];
  const double x = q[1];
  const double y = q[2];
  const double z = q[3];
  const double x2 = x + x;
  const double y2 = y + y;
  const double z2 = z + z;
  const double xx = x * x2;
  const double xy = x * y2;
  const double xz = x * z2;
  const double yy = y * y2;
  const double yz = y * z2;
  const double zz = z * z2;
  const double wx = w * x2;
  const double wy = w * y2;
  const double wz = w * z2;
  return {1.0 - (yy + zz), xy + wz,         xz - wy,         0.0,
          xy - wz,         1.0 - (xx + zz), yz + wx,         0.0,
          xz + wy,         yz - wx,         1.0 - (xx + yy), 0.0,
          pos[0],          pos[1],          pos[2],          1.0};
}

// Pose matrix for a body/geom node, reading any MuJoCo orientation specifier
// directly off the node (orientation is authored on the element, not in
// <default> classes).
std::vector<double> PoseMatrix(const pugi::xml_node& node, const Context& ctx) {
  const std::array<double, 3> pos =
      ParseDouble3(node ? Attr(node, "pos") : "", {{0.0, 0.0, 0.0}});
  const Quat q =
      node ? OrientationQuat(Attr(node, "quat"), Attr(node, "axisangle"),
                             Attr(node, "euler"), Attr(node, "xyaxes"),
                             Attr(node, "zaxis"), ctx)
           : Quat{{1.0, 0.0, 0.0, 0.0}};
  return MatrixFromPosQuat(pos, q);
}

// Column-major (Three.js Matrix4.elements) product a * b: index = col*4 + row.
// As a transform this applies `b` first, then `a` (a*(b*v)) -- so the rightmost
// argument is the innermost/local transform, matching Three.js
// Matrix4.multiply.
std::vector<double> MultiplyMatrix(const std::vector<double>& a,
                                   const std::vector<double>& b) {
  std::vector<double> out(16, 0.0);
  for (size_t c = 0; c < 4; c++) {
    for (size_t r = 0; r < 4; r++) {
      double s = 0.0;
      for (size_t k = 0; k < 4; k++) {
        s += a[k * 4 + r] * b[c * 4 + k];
      }
      out[c * 4 + r] = s;
    }
  }
  return out;
}

// 4x4 identity in the column-major flat layout used throughout.
std::vector<double> IdentityMatrix() {
  return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
          0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
}

std::vector<double> ScaleMatrix(const std::array<double, 3>& scale) {
  return {scale[0], 0.0, 0.0,      0.0, 0.0, scale[1], 0.0, 0.0,
          0.0,      0.0, scale[2], 0.0, 0.0, 0.0,      0.0, 1.0};
}

std::vector<double> TranslationMatrix(const std::array<double, 3>& t) {
  return {1.0, 0.0, 0.0, 0.0, 0.0,  1.0,  0.0,  0.0,
          0.0, 0.0, 1.0, 0.0, t[0], t[1], t[2], 1.0};
}

// Mesh frame transform from MJCF <mesh refpos/refquat>: vertices are first
// translated by -refpos, then rotated by conjugate(refquat). Returned as
// R(conj(refquat)) * T(-refpos) so it can left-compose with the geom matrix.
std::vector<double> MeshRefMatrix(const MeshAsset& asset) {
  std::vector<double> m = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                           0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
  if (asset.has_refquat) {
    const Quat conj{{asset.refquat[0], -asset.refquat[1], -asset.refquat[2],
                     -asset.refquat[3]}};
    m = MatrixFromPosQuat({{0.0, 0.0, 0.0}}, QuatNormalize(conj));
  }
  if (asset.has_refpos) {
    m = MultiplyMatrix(
        m, TranslationMatrix(
               {{-asset.refpos[0], -asset.refpos[1], -asset.refpos[2]}}));
  }
  return m;
}

void AppendTri(Mesh* mesh, const std::array<float, 3>& a,
               const std::array<float, 3>& b, const std::array<float, 3>& c) {
  const int32_t base = static_cast<int32_t>(mesh->positions.size() / 3);
  const std::array<std::array<float, 3>, 3> tri{{a, b, c}};
  for (size_t i = 0; i < tri.size(); i++) {
    const auto& v = tri[i];
    mesh->positions.push_back(v[0]);
    mesh->positions.push_back(v[1]);
    mesh->positions.push_back(v[2]);
    mesh->indices.push_back(base + static_cast<int32_t>(i));
  }
}

Mesh MakeBoxMesh(const std::array<double, 3>& half) {
  Mesh mesh;
  const float x = static_cast<float>(std::max(half[0], 1.0e-6));
  const float y = static_cast<float>(std::max(half[1], 1.0e-6));
  const float z = static_cast<float>(std::max(half[2], 1.0e-6));
  const std::array<std::array<float, 3>, 8> v{{
      {{-x, -y, -z}},
      {{x, -y, -z}},
      {{x, y, -z}},
      {{-x, y, -z}},
      {{-x, -y, z}},
      {{x, -y, z}},
      {{x, y, z}},
      {{-x, y, z}},
  }};
  const int faces[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
                            {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
                            {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
  for (const auto& f : faces) AppendTri(&mesh, v[f[0]], v[f[1]], v[f[2]]);
  return mesh;
}

Mesh MakeSphereMesh(double radius, int slices, int stacks) {
  Mesh mesh;
  const float r = static_cast<float>(std::max(radius, 1.0e-6));
  std::vector<std::array<float, 3>> verts;
  for (int iy = 0; iy <= stacks; iy++) {
    const double v = double(iy) / double(stacks);
    const double phi = v * 3.14159265358979323846;
    for (int ix = 0; ix <= slices; ix++) {
      const double u = double(ix) / double(slices);
      const double theta = u * 2.0 * 3.14159265358979323846;
      verts.push_back({{static_cast<float>(r * std::sin(phi) * std::cos(theta)),
                        static_cast<float>(r * std::sin(phi) * std::sin(theta)),
                        static_cast<float>(r * std::cos(phi))}});
    }
  }
  for (int iy = 0; iy < stacks; iy++) {
    for (int ix = 0; ix < slices; ix++) {
      const int a = iy * (slices + 1) + ix;
      const int b = a + 1;
      const int c = a + slices + 1;
      const int d = c + 1;
      AppendTri(&mesh, verts[a], verts[c], verts[b]);
      AppendTri(&mesh, verts[b], verts[c], verts[d]);
    }
  }
  return mesh;
}

// Capsule along Z: the sphere's upper and lower hemispheres shifted by
// +-half_length, joined at the equator (a cylinder side between them).
Mesh MakeCapsuleMesh(double radius, double half_length, int slices, int stacks) {
  Mesh mesh;
  const double r = std::max(radius, 1.0e-6);
  const double h = std::max(half_length, 0.0);
  std::vector<std::array<float, 3>> verts;
  // stacks + 2 rings: the equator is duplicated, once per hemisphere.
  for (int iy = 0; iy <= stacks + 1; iy++) {
    const int k = iy <= stacks / 2 ? iy : iy - 1;
    const double phi = double(k) / double(stacks) * kPi;
    const double z = r * std::cos(phi) + (iy <= stacks / 2 ? h : -h);
    for (int ix = 0; ix <= slices; ix++) {
      const double theta = double(ix) / double(slices) * 2.0 * kPi;
      verts.push_back({{static_cast<float>(r * std::sin(phi) * std::cos(theta)),
                        static_cast<float>(r * std::sin(phi) * std::sin(theta)),
                        static_cast<float>(z)}});
    }
  }
  for (int iy = 0; iy <= stacks; iy++) {
    for (int ix = 0; ix < slices; ix++) {
      const int a = iy * (slices + 1) + ix;
      const int b = a + 1;
      const int c = a + slices + 1;
      const int d = c + 1;
      AppendTri(&mesh, verts[a], verts[c], verts[b]);
      AppendTri(&mesh, verts[b], verts[c], verts[d]);
    }
  }
  return mesh;
}

Mesh MakeCylinderMesh(double radius, double half_length, int slices) {
  Mesh mesh;
  const float r = static_cast<float>(std::max(radius, 1.0e-6));
  const float h = static_cast<float>(std::max(half_length, 1.0e-6));
  const std::array<float, 3> top{{0.0f, 0.0f, h}};
  const std::array<float, 3> bottom{{0.0f, 0.0f, -h}};
  for (int i = 0; i < slices; i++) {
    const double a0 = 2.0 * 3.14159265358979323846 * double(i) / double(slices);
    const double a1 =
        2.0 * 3.14159265358979323846 * double(i + 1) / double(slices);
    const std::array<float, 3> p0{{static_cast<float>(r * std::cos(a0)),
                                   static_cast<float>(r * std::sin(a0)), -h}};
    const std::array<float, 3> p1{{static_cast<float>(r * std::cos(a1)),
                                   static_cast<float>(r * std::sin(a1)), -h}};
    const std::array<float, 3> p2{{p0[0], p0[1], h}};
    const std::array<float, 3> p3{{p1[0], p1[1], h}};
    AppendTri(&mesh, p0, p2, p1);
    AppendTri(&mesh, p1, p2, p3);
    AppendTri(&mesh, bottom, p1, p0);
    AppendTri(&mesh, top, p2, p3);
  }
  return mesh;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
