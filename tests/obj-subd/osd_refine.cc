// osd_refine OBJ LEVEL — OpenSubdiv (Far, CPU) reference for OBJ subdivision
// tags. Port of tinyobjloader blender/tests/osd_refine.cc to the vendored C11
// loader (src/external/tinyobjloader-c).
//
// Loads OBJ (triangulate = false), checks each shape's `t` lines with
// tobj_subd, builds a Far::TopologyRefiner with the scheme, boundary rule,
// creases, corners and holes, refines uniformly to LEVEL and prints per shape
//   shape <name> <number of points>
//   x y z
// A shape uses only the vertices its faces refer to. Absent tags take
// OpenSubdiv's Sdc::Options defaults; rejected shapes are skipped (stderr).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>

#include "external/tinyobjloader-c/tiny_obj_c.h"
#include "external/tinyobjloader-c/tobj_subd.h"

using namespace OpenSubdiv;

struct Point {
  float p[3];
  void Clear(void* = nullptr) { p[0] = p[1] = p[2] = 0.0f; }
  void AddWithWeight(Point const& s, float w) {
    for (int k = 0; k < 3; k++) p[k] += w * s.p[k];
  }
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: osd_refine FILE.obj LEVEL\n");
    return 2;
  }
  const int level = std::atoi(argv[2]);
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 2;
  std::vector<uint8_t> buf;
  for (int c; (c = std::fgetc(f)) != EOF;) buf.push_back(uint8_t(c));
  std::fclose(f);
  tobj_load_config cfg = tobj_default_config();
  cfg.triangulate = false;
  tobj_scene_d sc;
  std::memset(&sc, 0, sizeof(sc));
  if (tobj_load_obj_from_memory_d(&sc, buf.data(), buf.size(), &cfg, nullptr) != TOBJ_OK) {
    std::fprintf(stderr, "load failed\n");
    return 1;
  }
  for (size_t si = 0; si < sc.num_shapes; si++) {
    const tobj_mesh_d& m = sc.shapes[si].mesh;
    std::vector<int> vi(m.num_indices);
    for (size_t i = 0; i < m.num_indices; i++) vi[i] = m.indices[i].vertex_index;
    tobj_subd s;
    if (tobj_subd_parse_lines(m.tag_lines, m.num_tag_lines, m.num_face_vertices, m.num_faces,
                              vi.data(), &s))
      return 1;
    if (!m.num_tag_lines || !s.ok) {
      if (m.num_tag_lines) std::fprintf(stderr, "shape %s: tags rejected\n", sc.shapes[si].name);
      tobj_subd_free(&s);
      continue;
    }
    std::map<int, int> local;
    for (int v : vi) local[v] = 0;
    int n = 0;
    for (auto& kv : local) kv.second = n++;
    std::vector<Far::Index> fv;
    for (int v : vi) fv.push_back(local[v]);
    std::vector<int> nverts(m.num_face_vertices, m.num_face_vertices + m.num_faces);
    std::vector<Far::Index> creasePairs, corners, holes;
    std::vector<float> creaseW, cornerW;
    for (size_t i = 0; i < s.num_creases; i++) {
      creasePairs.push_back(local[(int)s.creases[i].v0]);
      creasePairs.push_back(local[(int)s.creases[i].v1]);
      creaseW.push_back((float)tobj_subd_sharpness(s.creases[i].sharpness));
    }
    for (size_t i = 0; i < s.num_corners; i++) {
      corners.push_back(local[(int)s.corners[i].v]);
      cornerW.push_back((float)tobj_subd_sharpness(s.corners[i].sharpness));
    }
    for (size_t i = 0; i < s.num_holes; i++) holes.push_back((Far::Index)s.holes[i]);

    Sdc::SchemeType scheme = s.scheme == TOBJ_SUBD_LOOP       ? Sdc::SCHEME_LOOP
                             : s.scheme == TOBJ_SUBD_BILINEAR ? Sdc::SCHEME_BILINEAR
                                                              : Sdc::SCHEME_CATMARK;
    Sdc::Options opt;
    // Absent tags: OpenSubdiv's OBJ reader defaults (edge-only boundary; the
    // other Sdc defaults already match).
    opt.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    // OpenSubdiv's OBJ numbering (regression/common/far_utils.h): 1 = edge and
    // corner, 2 = edge only (not the Sdc enum order).
    if (s.interp_boundary == 0) opt.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_NONE);
    if (s.interp_boundary == 1)
      opt.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);
    if (s.interp_boundary == 2)
      opt.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    if (s.fvar_interp_boundary != TOBJ_SUBD_UNSET)
      opt.SetFVarLinearInterpolation(
          (Sdc::Options::FVarLinearInterpolation)s.fvar_interp_boundary);
    if (s.crease_method != TOBJ_SUBD_UNSET)
      opt.SetCreasingMethod((Sdc::Options::CreasingMethod)s.crease_method);
    if (s.smooth_triangles != TOBJ_SUBD_UNSET)
      opt.SetTriangleSubdivision((Sdc::Options::TriangleSubdivision)s.smooth_triangles);

    Far::TopologyDescriptor desc;
    desc.numVertices = n;
    desc.numFaces = (int)nverts.size();
    desc.numVertsPerFace = nverts.data();
    desc.vertIndicesPerFace = fv.data();
    desc.numCreases = (int)creaseW.size();
    desc.creaseVertexIndexPairs = creasePairs.data();
    desc.creaseWeights = creaseW.data();
    desc.numCorners = (int)cornerW.size();
    desc.cornerVertexIndices = corners.data();
    desc.cornerWeights = cornerW.data();
    desc.numHoles = (int)holes.size();
    desc.holeIndices = holes.data();
    typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
    Far::TopologyRefiner* refiner = Factory::Create(desc, Factory::Options(scheme, opt));
    if (!refiner) {
      tobj_subd_free(&s);
      continue;
    }
    refiner->RefineUniform(Far::TopologyRefiner::UniformOptions(level));
    std::vector<Point> pts(refiner->GetNumVerticesTotal());
    Point* src = pts.data();
    for (auto& kv : local)
      for (int k = 0; k < 3; k++)
        src[kv.second].p[k] = (float)sc.attrib.vertices.ptr[3 * kv.first + k];
    Far::PrimvarRefiner primvar(*refiner);
    for (int l = 1; l <= level; l++) {
      Point* dst = src + refiner->GetLevel(l - 1).GetNumVertices();
      primvar.Interpolate(l, src, dst);
      src = dst;
    }
    // Hole faces are dropped by LightUSD; report only the points of the
    // remaining faces so both sides compare the same set.
    const Far::TopologyLevel& last = refiner->GetLevel(level);
    std::vector<char> used(last.GetNumVertices(), holes.empty() ? 1 : 0);
    if (!holes.empty())
      for (int fi = 0; fi < last.GetNumFaces(); fi++) {
        if (last.IsFaceHole(fi)) continue;
        Far::ConstIndexArray fvi = last.GetFaceVertices(fi);
        for (int k = 0; k < fvi.size(); k++) used[fvi[k]] = 1;
      }
    int count = 0;
    for (char u : used) count += u;
    std::printf("shape %s %d\n", sc.shapes[si].name, count);
    for (int v = 0; v < last.GetNumVertices(); v++)
      if (used[v]) std::printf("%.7g %.7g %.7g\n", src[v].p[0], src[v].p[1], src[v].p[2]);
    delete refiner;
    tobj_subd_free(&s);
  }
  tobj_scene_free_d(&sc);
  return 0;
}
