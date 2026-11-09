// SPDX-License-Identifier: Apache-2.0
// lusdrender — low-level ray/primitive intersection: triangle (Moller-Trumbore),
// tetrahedron, and implicit shapes (cylinder/cone/capsule), plus the LightRT
// user-intersect callbacks for tets and curves.
#include <algorithm>
#include <cmath>

#include "lusdr_context.hh"

namespace lusdr {

bool BuildNodeMatrixMap(const Node &node,
                        std::unordered_map<std::string, matrix4d> *map) {
  if (!map) return false;
  if (!node.abs_path.empty()) {
    (*map)[node.abs_path] = node.global_matrix;
  }
  for (const Node &child : node.children) {
    BuildNodeMatrixMap(child, map);
  }
  return true;
}

std::unordered_map<std::string, matrix4d> BuildNodeMatrixMap(
    const RenderScene &scene) {
  std::unordered_map<std::string, matrix4d> map;
  for (const Node &root : scene.nodes) {
    BuildNodeMatrixMap(root, &map);
  }
  return map;
}

matrix4d MatrixForPath(const std::unordered_map<std::string, matrix4d> &map,
                       const std::string &path) {
  auto it = map.find(path);
  return (it == map.end()) ? matrix4d::identity() : it->second;
}

Vec3 TransformNormal(const matrix4d &inv_world, const Vec3 &n) {
  return Normalize(Vec3{
      float(inv_world.m[0][0] * double(n.x) + inv_world.m[0][1] * double(n.y) +
            inv_world.m[0][2] * double(n.z)),
      float(inv_world.m[1][0] * double(n.x) + inv_world.m[1][1] * double(n.y) +
            inv_world.m[1][2] * double(n.z)),
      float(inv_world.m[2][0] * double(n.x) + inv_world.m[2][1] * double(n.y) +
            inv_world.m[2][2] * double(n.z))});
}

int AxisIndex(lightusd::legacy::Axis axis) {
  switch (axis) {
    case lightusd::legacy::Axis::X: return 0;
    case lightusd::legacy::Axis::Y: return 1;
    case lightusd::legacy::Axis::Z: return 2;
    case lightusd::legacy::Axis::Invalid: break;
  }
  return 2;
}

Vec3 AxisVec(lightusd::legacy::Axis axis) {
  switch (axis) {
    case lightusd::legacy::Axis::X: return Vec3{1.0f, 0.0f, 0.0f};
    case lightusd::legacy::Axis::Y: return Vec3{0.0f, 1.0f, 0.0f};
    case lightusd::legacy::Axis::Z: return Vec3{0.0f, 0.0f, 1.0f};
    case lightusd::legacy::Axis::Invalid: break;
  }
  return Vec3{0.0f, 0.0f, 1.0f};
}

float Coord(const Vec3 &v, int axis) {
  return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

Vec3 WithCoord(Vec3 v, int axis, float c) {
  if (axis == 0) v.x = c;
  if (axis == 1) v.y = c;
  if (axis == 2) v.z = c;
  return v;
}

Vec3 RadialPart(Vec3 v, int axis) {
  return WithCoord(v, axis, 0.0f);
}

bool SolveQuadratic(float a, float b, float c, float *t0, float *t1) {
  if (std::abs(a) < 1.0e-12f) {
    if (std::abs(b) < 1.0e-12f) return false;
    *t0 = *t1 = -c / b;
    return true;
  }
  float disc = b * b - 4.0f * a * c;
  if (disc < 0.0f) return false;
  float s = std::sqrt(disc);
  float q = -0.5f * (b + std::copysign(s, b));
  *t0 = q / a;
  *t1 = (std::abs(q) > 1.0e-20f) ? c / q : (-b + s) / (2.0f * a);
  if (*t0 > *t1) std::swap(*t0, *t1);
  return true;
}

bool IntersectTriangleMT(const Vec3 &o, const Vec3 &d, const Vec3 &a,
                         const Vec3 &b, const Vec3 &c, float tmin,
                         float tmax, float *t) {
  Vec3 e1 = Sub(b, a);
  Vec3 e2 = Sub(c, a);
  Vec3 p = Cross(d, e2);
  float det = Dot(e1, p);
  if (std::abs(det) < 1.0e-12f) return false;
  float inv_det = 1.0f / det;
  Vec3 s = Sub(o, a);
  float u = inv_det * Dot(s, p);
  if (u < 0.0f || u > 1.0f) return false;
  Vec3 q = Cross(s, e1);
  float v = inv_det * Dot(d, q);
  if (v < 0.0f || u + v > 1.0f) return false;
  float tt = inv_det * Dot(e2, q);
  if (tt < tmin || tt > tmax) return false;
  if (t) *t = tt;
  return true;
}

bool IntersectTetPrim(const TetPrim &tet, const Vec3 &o, const Vec3 &d,
                      float tmin, float tmax, float *best_t, Vec3 *normal) {
  const int faces[4][4] = {
      {0, 2, 1, 3}, {0, 1, 3, 2}, {0, 3, 2, 1}, {1, 2, 3, 0},
  };
  bool hit = false;
  float best = tmax;
  Vec3 best_n{0.0f, 1.0f, 0.0f};
  for (const auto &f : faces) {
    const Vec3 &a = tet.p[f[0]];
    const Vec3 &b = tet.p[f[1]];
    const Vec3 &c = tet.p[f[2]];
    const Vec3 &opp = tet.p[f[3]];
    Vec3 n = Cross(Sub(b, a), Sub(c, a));
    if (Dot(n, Sub(opp, a)) > 0.0f) n = Mul(n, -1.0f);
    n = Normalize(n);
    float t = 0.0f;
    if (IntersectTriangleMT(o, d, a, b, c, tmin, best, &t)) {
      best = t;
      best_n = (Dot(n, d) > 0.0f) ? Mul(n, -1.0f) : n;
      hit = true;
    }
  }
  if (!hit) return false;
  if (best_t) *best_t = best;
  if (normal) *normal = best_n;
  return true;
}

int TetUserIntersect(const lrt_ray *ray, uint32_t prim_id, void *user,
                     float *t, float *u, float *v) {
  const std::vector<TetPrim> *tets =
      reinterpret_cast<const std::vector<TetPrim> *>(user);
  if (!ray || !tets || prim_id >= tets->size()) return 0;
  Vec3 o{ray->org[0], ray->org[1], ray->org[2]};
  Vec3 d{ray->dir[0], ray->dir[1], ray->dir[2]};
  float tt = ray->tmax;
  Vec3 n;
  if (!IntersectTetPrim((*tets)[prim_id], o, d, ray->tmin, ray->tmax, &tt, &n)) {
    return 0;
  }
  if (t) *t = tt;
  if (u) *u = 0.0f;
  if (v) *v = 0.0f;
  return 1;
}

int TetUserOccluded(const lrt_ray *ray, uint32_t prim_id, void *user) {
  return TetUserIntersect(ray, prim_id, user, nullptr, nullptr, nullptr);
}

bool AcceptT(float t, float tmin, float tmax, float *best) {
  if (t >= tmin && t <= tmax && t < *best) {
    *best = t;
    return true;
  }
  return false;
}

bool IntersectDirectShape(const DirectShape &shape, const Vec3 &ray_org,
                          const Vec3 &ray_dir, float tmin, float tmax,
                          DirectHit *hit) {
  Vec3 o = TransformPoint(shape.inv_world, ray_org);
  Vec3 d = TransformVector(shape.inv_world, ray_dir);
  const int ax = AxisIndex(shape.axis);
  const float half_h = float(std::max(0.0, shape.height) * 0.5);
  const float radius = float(std::max(0.0, shape.radius));
  float best = tmax;
  Vec3 nlocal{0.0f, 1.0f, 0.0f};
  bool found = false;

  if (shape.type == DirectShape::Type::Cylinder) {
    Vec3 ro = RadialPart(o, ax);
    Vec3 rd = RadialPart(d, ax);
    float t0, t1;
    if (SolveQuadratic(Dot(rd, rd), 2.0f * Dot(ro, rd),
                       Dot(ro, ro) - radius * radius, &t0, &t1)) {
      for (float t : {t0, t1}) {
        float y = Coord(o, ax) + t * Coord(d, ax);
        if (y >= -half_h && y <= half_h && AcceptT(t, tmin, best, &best)) {
          nlocal = Normalize(RadialPart(Add(o, Mul(d, t)), ax));
          found = true;
        }
      }
    }
    for (float cap : {-half_h, half_h}) {
      float denom = Coord(d, ax);
      if (std::abs(denom) < 1.0e-12f) continue;
      float t = (cap - Coord(o, ax)) / denom;
      Vec3 p = Add(o, Mul(d, t));
      if (Dot(RadialPart(p, ax), RadialPart(p, ax)) <= radius * radius &&
          AcceptT(t, tmin, best, &best)) {
        nlocal = Mul(AxisVec(shape.axis), cap < 0.0f ? -1.0f : 1.0f);
        found = true;
      }
    }
  } else if (shape.type == DirectShape::Type::Cone) {
    const float apex = half_h;
    const float base = -half_h;
    const float k = (half_h > 0.0f) ? radius / (2.0f * half_h) : 0.0f;
    const float oy = Coord(o, ax);
    const float dy = Coord(d, ax);
    Vec3 ro = RadialPart(o, ax);
    Vec3 rd = RadialPart(d, ax);
    float t0, t1;
    if (SolveQuadratic(Dot(rd, rd) - k * k * dy * dy,
                       2.0f * (Dot(ro, rd) - k * k * (oy - apex) * dy),
                       Dot(ro, ro) - k * k * (oy - apex) * (oy - apex),
                       &t0, &t1)) {
      for (float t : {t0, t1}) {
        float y = oy + t * dy;
        if (y >= base && y <= apex && AcceptT(t, tmin, best, &best)) {
          Vec3 p = Add(o, Mul(d, t));
          Vec3 radial = RadialPart(p, ax);
          nlocal = Normalize(Add(radial, Mul(AxisVec(shape.axis), k * Length(radial))));
          found = true;
        }
      }
    }
    if (std::abs(dy) > 1.0e-12f) {
      float t = (base - oy) / dy;
      Vec3 p = Add(o, Mul(d, t));
      if (Dot(RadialPart(p, ax), RadialPart(p, ax)) <= radius * radius &&
          AcceptT(t, tmin, best, &best)) {
        nlocal = Mul(AxisVec(shape.axis), -1.0f);
        found = true;
      }
    }
  } else {
    Vec3 a = Mul(AxisVec(shape.axis), -half_h);
    Vec3 b = Mul(AxisVec(shape.axis), half_h);
    Vec3 ba = Sub(b, a);
    Vec3 oa = Sub(o, a);
    float baba = Dot(ba, ba);
    float bard = Dot(ba, d);
    float baoa = Dot(ba, oa);
    float rdoa = Dot(d, oa);
    float oaoa = Dot(oa, oa);
    float A = baba - bard * bard;
    float B = baba * rdoa - baoa * bard;
    float C = baba * oaoa - baoa * baoa - radius * radius * baba;
    float h = B * B - A * C;
    if (h >= 0.0f && std::abs(A) > 1.0e-12f) {
      float t = (-B - std::sqrt(h)) / A;
      float y = baoa + t * bard;
      if (y > 0.0f && y < baba && AcceptT(t, tmin, best, &best)) {
        Vec3 p = Add(oa, Mul(d, t));
        nlocal = Normalize(Sub(p, Mul(ba, y / baba)));
        found = true;
      }
    }
    for (Vec3 c : {a, b}) {
      Vec3 oc = Sub(o, c);
      float t0, t1;
      if (SolveQuadratic(Dot(d, d), 2.0f * Dot(oc, d),
                         Dot(oc, oc) - radius * radius, &t0, &t1)) {
        for (float t : {t0, t1}) {
          if (AcceptT(t, tmin, best, &best)) {
            nlocal = Normalize(Sub(Add(o, Mul(d, t)), c));
            found = true;
          }
        }
      }
    }
  }

  if (!found || !hit) return false;
  hit->t = best;
  hit->n = TransformNormal(shape.inv_world, nlocal);
  hit->base_color = shape.base_color;
  hit->emission = shape.emission;
  hit->hit = true;
  return true;
}

float TriangleArea(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2) {
  return 0.5f * Length(Cross(Sub(p1, p0), Sub(p2, p0)));
}

// Extend `b` with the world-space AABB (8 transformed corners) of every volume.
// Shared by the next and legacy loaders so a volume-only scene — or one whose
// volume sits away from the origin — still contributes to the camera-framing
// bounds (without bounds.valid the auto-camera would frame the empty origin).
void ExpandBoundsByVolume(const std::vector<VolumeData> &vols, Bounds *b) {
  if (!b) return;
  for (const VolumeData &vd : vols) {
    matrix4d world;
    if (!lightusd::legacy::inverse(vd.inv_world, world, 1.0e-12))
      world = matrix4d::identity();
    for (int c = 0; c < 8; c++) {
      Vec3 corner{(c & 1) ? vd.bmax.x : vd.bmin.x,
                  (c & 2) ? vd.bmax.y : vd.bmin.y,
                  (c & 4) ? vd.bmax.z : vd.bmin.z};
      Vec3 w = TransformPoint(world, corner);
      b->lo.x = std::min(b->lo.x, w.x);
      b->lo.y = std::min(b->lo.y, w.y);
      b->lo.z = std::min(b->lo.z, w.z);
      b->hi.x = std::max(b->hi.x, w.x);
      b->hi.y = std::max(b->hi.y, w.y);
      b->hi.z = std::max(b->hi.z, w.z);
      b->valid = true;
    }
  }
}

// USD upAxis token -> lightusd::legacy::Axis (default Y). Shared by both loaders.
lightusd::legacy::Axis GetUpAxis(const std::string &up) {
  if (up == "X") return lightusd::legacy::Axis::X;
  if (up == "Z") return lightusd::legacy::Axis::Z;
  return lightusd::legacy::Axis::Y;
}

}  // namespace lusdr
