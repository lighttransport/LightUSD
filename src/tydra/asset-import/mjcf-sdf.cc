// SPDX-License-Identifier: Apache-2.0
// MuJoCo's analytic SDF plugins (plugin/sdf: torus, bowl, gear, nut, bolt)
// meshed as MuJoCo's compiler meshes a plugin mesh asset (mjCMesh::LoadSDF):
// the plugin's distance sampled on a grid of about 300 cells over its AABB,
// then MarchingCubeCpp. The distance functions are ports of MuJoCo 3.16's
// (Copyright 2023 DeepMind Technologies Limited, Apache-2.0), kept
// expression for expression so the generated mesh matches MuJoCo's.
#include <cmath>
#include <cstdlib>

#include "import.hh"

#define MC_IMPLEM_ENABLE
#include "external/marchingcubecpp/MC.h"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {

constexpr double kPi = 3.14159265358979323846;  // mjPI

double Norm2(const double v[2]) { return std::sqrt(0.0 + v[0] * v[0] + v[1] * v[1]); }
double Union(double a, double b) { return std::min(a, b); }
double Intersection(double a, double b) { return std::max(a, b); }
double Subtraction(double a, double b) { return std::max(a, -b); }
double Fract(double x) { return x - std::floor(x); }

double Torus(const double p[3], const double* a) {
  const double q = std::sqrt(p[0] * p[0] + p[1] * p[1]) - a[0];
  return std::sqrt(q * q + p[2] * p[2]) - a[1];
}

double Bowl(const double point[3], const double* a) {
  const double height = a[0], radius = a[1], thick = a[2];
  const double width = std::sqrt(radius * radius - height * height);
  const double q[2] = {Norm2(point), point[2]};
  const double qdiff[2] = {q[0] - width, q[1] - height};
  return ((height * q[0] < width * q[1]) ? Norm2(qdiff) : std::fabs(Norm2(q) - radius)) - thick;
}

// The hexagonal head shared by nut and bolt.
double ScrewHead(const double p[3], double radius) {
  const double sqrt12 = std::sqrt(2.) / 2.;
  const double k = 6. / kPi / 2.;
  const double angle = -std::floor((std::atan2(p[1], p[0])) * k + .5) / k;
  const double s[2] = {std::sin(angle), std::sin(angle + kPi * .5)};
  const double res[2] = {0.0 + s[1] * p[0] + -s[0] * p[1], 0.0 + s[0] * p[0] + s[1] * p[1]};
  double head = res[0] - .5;
  head = Intersection(head, std::abs(p[2] + .25) - .25);
  return Intersection(head, (p[2] + radius - .22) * sqrt12);
}

double Nut(const double p[3], const double* a) {
  const double screw = 12;
  const double radius2 = std::sqrt(p[0] * p[0] + p[1] * p[1]) - a[0];
  const double sqrt12 = std::sqrt(2.) / 2.;
  const double azimuth = std::atan2(p[1], p[0]);
  const double triangle = std::abs(Fract(p[2] * screw - azimuth / kPi / 2.) - .5);
  const double thread2 = (radius2 - triangle / screw) * sqrt12;
  const double cone2 = (p[2] - radius2) * sqrt12;
  double hole = Subtraction(thread2, cone2 + .5 * sqrt12);
  hole = Union(hole, -cone2 - .05 * sqrt12);
  return Subtraction(ScrewHead(p, radius2), hole);
}

double Bolt(const double p[3], const double* a) {
  const double screw = 12;
  const double radius = std::sqrt(p[0] * p[0] + p[1] * p[1]) - a[0];
  const double sqrt12 = std::sqrt(2.) / 2.;
  const double azimuth = std::atan2(p[1], p[0]);
  const double triangle = std::abs(Fract(p[2] * screw - azimuth / kPi / 2.) - .5);
  const double thread = (radius - triangle / screw) * sqrt12;
  double bolt = Subtraction(thread, .5 - std::abs(p[2] + .5));
  const double cone = (p[2] - radius) * sqrt12;
  bolt = Subtraction(bolt, cone + 1. * sqrt12);
  return Union(bolt, ScrewHead(p, radius));
}

double SmoothUnion(double a, double b, double k) {
  const double h = std::min(std::max(0.5 + 0.5 * (b - a) / k, 0.0), 1.0);
  return b * (1. - h) + a * h - k * h * (1. - h);
}

double SmoothIntersection(double a, double b, double k) {
  return Subtraction(Intersection(a, b), SmoothUnion(Subtraction(a, b), Subtraction(b, a), k));
}

double Mod(double x, double y) { return x - y * std::floor(x / y); }

// attributes: alpha, diameter, teeth, thickness, innerdiameter.
double Gear2D(const double p[3], const double* a) {
  const double D = a[1], N = a[2];
  const double psi = 3.096e-5 * N * N - 6.557e-3 * N + 0.551;  // pressure angle
  const double alpha = a[0];
  const double R = D / 2.0;
  const double rho = Norm2(p);
  const double Pd = N / D;
  const double P = kPi / Pd;
  const double add = 1.0 / Pd;
  const double Do = D + 2.0 * add;
  const double Ro = Do / 2.0;
  const double h = 2.2 / Pd;
  double innerR = Ro - h - 0.14 * D;
  if (a[4] >= 0.0) innerR = a[4] / 2.0;
  if (innerR - rho > 0.0) return innerR - rho;
  if (Ro - rho < -0.2) return rho - Ro;
  const double Db = D * std::cos(psi);
  const double Rb = Db / 2.0;
  const double fi = std::atan2(p[1], p[0]) + alpha;
  const double alphaStride = P / R;
  const double invAlpha = std::acos(Rb / R);
  const double invPhi = std::tan(invAlpha) - invAlpha;
  const double shift = alphaStride / 2.0 - 2.0 * invPhi;
  const double fia = Mod(fi + shift / 2.0, alphaStride) - shift / 2.0;
  const double fib = Mod(-fi - shift + shift / 2.0, alphaStride) - shift / 2.0;
  double dista = -1.0e6, distb = -1.0e6;
  if (Rb < rho) {
    const double acos_rbRho = std::acos(Rb / rho);
    const double thetaa = fia + acos_rbRho;
    const double thetab = fib + acos_rbRho;
    const double ta = std::sqrt(rho * rho - Rb * Rb);
    dista = ta - Rb * thetaa;
    distb = ta - Rb * thetab;
  }
  const double gearOuter = rho - Ro;
  const double gearLowBase = rho - (Ro - h);
  const double crownBase = rho - innerR;
  double cogs = Intersection(dista, distb);
  const double baseWalls = Intersection(fia - (alphaStride - shift), fib - (alphaStride - shift));
  cogs = Intersection(baseWalls, cogs);
  cogs = SmoothIntersection(gearOuter, cogs, 0.0035 * D);
  cogs = SmoothUnion(gearLowBase, cogs, Rb - Ro + h);
  return Subtraction(cogs, crownBase);
}

double Gear(const double p[3], const double* a) {
  const double w[2] = {Gear2D(p, a), std::abs(p[2]) - a[3] / 2.};
  const double w_abs[2] = {std::max(w[0], 0.0), std::max(w[1], 0.0)};
  return std::min(std::max(w[0], w[1]), 0.) + Norm2(w_abs);
}

struct SdfPlugin {
  const char* name;
  double (*distance)(const double p[3], const double* attributes);
  std::vector<const char*> keys;
  std::vector<double> defaults;
  // Half extents of the AABB (its center is the origin for every plugin).
  void (*aabb)(const double* attributes, double half[3]);
};

const std::vector<SdfPlugin>& Plugins() {
  static const std::vector<SdfPlugin> plugins = {
      {"mujoco.sdf.torus", Torus, {"radius1", "radius2"}, {.35, .15},
       [](const double* a, double h[3]) {
         h[0] = h[1] = a[0] + a[1];
         h[2] = a[1];
       }},
      {"mujoco.sdf.bowl", Bowl, {"height", "radius", "thickness"}, {.4, 1., .02},
       [](const double* a, double h[3]) { h[0] = h[1] = h[2] = a[1] + a[2]; }},
      {"mujoco.sdf.gear", Gear, {"alpha", "diameter", "teeth", "thickness", "innerdiameter"},
       {0, 2.8, 25, .2, -1},
       [](const double* a, double h[3]) {
         h[0] = h[1] = a[1] / 2. * 1.25;
         h[2] = a[3] / 2. * 1.1;
       }},
      {"mujoco.sdf.nut", Nut, {"radius"}, {0.26},
       [](const double*, double h[3]) {
         h[0] = h[1] = .6;
         h[2] = 1;
       }},
      {"mujoco.sdf.bolt", Bolt, {"radius"}, {0.26},
       [](const double*, double h[3]) {
         h[0] = h[1] = .6;
         h[2] = 1;
       }},
  };
  return plugins;
}

const SdfPlugin* FindPlugin(const std::string& name) {
  for (const auto& plugin : Plugins())
    if (name == plugin.name) return &plugin;
  return nullptr;
}

}  // namespace

bool IsMjcfSdfPlugin(const std::string& plugin) { return FindPlugin(plugin) != nullptr; }

bool GenerateMjcfSdfMesh(const std::string& plugin_name,
                         const std::map<std::string, std::string>& config, size_t max_bytes,
                         Mesh* mesh, std::string* err) {
  const SdfPlugin* plugin = FindPlugin(plugin_name);
  if (!plugin) {
    if (err) *err = "unsupported SDF plugin " + plugin_name;
    return false;
  }
  std::vector<double> attributes = plugin->defaults;
  for (size_t i = 0; i < plugin->keys.size(); ++i) {
    const auto it = config.find(plugin->keys[i]);
    if (it == config.end() || it->second.empty()) continue;
    char* end = nullptr;
    attributes[i] = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str()) {
      if (err) *err = "invalid attribute value for '" + std::string(plugin->keys[i]) + "'";
      return false;
    }
  }
  double half[3] = {0, 0, 0};
  plugin->aabb(attributes.data(), half);
  const double total = half[0] + half[1] + half[2];
  if (!(std::isfinite(total) && total > 0 && half[0] > 0 && half[1] > 0 && half[2] > 0)) {
    if (err) *err = plugin_name + " has an empty bounding box";
    return false;
  }
  const double n = 300;
  const int nx = int(std::floor(n / total * half[0])) + 1;
  const int ny = int(std::floor(n / total * half[1])) + 1;
  const int nz = int(std::floor(n / total * half[2])) + 1;
  if (nx < 2 || ny < 2 || nz < 2) {
    if (err) *err = plugin_name + " bounding box is too flat to mesh";
    return false;
  }
  const size_t cells = size_t(nx) * size_t(ny) * size_t(nz);
  if (cells * sizeof(MC::MC_FLOAT) > max_bytes) {
    if (err) *err = plugin_name + " SDF grid exceeds the memory budget";
    return false;
  }
  std::vector<MC::MC_FLOAT> field(cells);
  for (int i = 0; i < nx; i++) {
    for (int j = 0; j < ny; j++) {
      for (int k = 0; k < nz; k++) {
        const double point[] = {0 - half[0] + 2 * half[0] * i / (nx - 1),
                                0 - half[1] + 2 * half[1] * j / (ny - 1),
                                0 - half[2] + 2 * half[2] * k / (nz - 1)};
        field[(size_t(k) * ny + j) * nx + i] =
            static_cast<MC::MC_FLOAT>(plugin->distance(point, attributes.data()));
      }
    }
  }
  MC::mcMesh out;
  MC::marching_cube(field.data(), nx, ny, nz, out);
  mesh->positions.clear();
  mesh->normals.clear();
  mesh->indices.clear();
  mesh->positions.reserve(out.vertices.size() * 3);
  for (const auto& v : out.vertices) {
    mesh->positions.push_back(float(2 * half[0] * v.x / (nx - 1) + 0 - half[0]));
    mesh->positions.push_back(float(2 * half[1] * v.y / (ny - 1) + 0 - half[1]));
    mesh->positions.push_back(float(2 * half[2] * v.z / (nz - 1) + 0 - half[2]));
  }
  mesh->normals.reserve(out.normals.size() * 3);
  for (const auto& v : out.normals) {
    mesh->normals.push_back(v.x);
    mesh->normals.push_back(v.y);
    mesh->normals.push_back(v.z);
  }
  mesh->indices.reserve(out.indices.size());
  for (MC::muint index : out.indices) mesh->indices.push_back(int32_t(index));
  if (mesh->indices.empty()) {
    if (err) *err = plugin_name + " SDF has no surface inside its bounding box";
    return false;
  }
  return true;
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
