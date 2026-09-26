// Reference UsdSkel evaluation for tests/test_schemas.py (MINIUSD_SKEL_ORACLE).
// Build against an OpenUSD install, e.g.:
//   g++ -std=c++17 -I$USD/include -I<python-include> skel_oracle.cpp -o skel_oracle \
//     -L$USD/lib -lusd_usdSkel -lusd_usdGeom -lusd_usd -lusd_sdf -lusd_tf -lusd_vt -lusd_gf \
//     -lusd_arch -lusd_pcp -lusd_trace -ltbb [-lboost_pythonXY -lpythonX.Y]
// Usage: skel_oracle file.usd [time]
// Prints UsdSkel-deformed points (skeleton space) of every skinned mesh at a time.
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShapeQuery.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <cstdio>
#include <cstdlib>
PXR_NAMESPACE_USING_DIRECTIVE
int main(int argc, char** argv) {
  auto stage = UsdStage::Open(argv[1]);
  double t = argc > 2 ? atof(argv[2]) : UsdTimeCode::Default().GetValue();
  UsdTimeCode tc = argc > 2 ? UsdTimeCode(t) : UsdTimeCode::Default();
  UsdSkelCache cache;
  for (const UsdPrim& p : stage->Traverse()) {
    if (!p.IsA<UsdSkelRoot>()) continue;
    UsdSkelRoot root(p);
    cache.Populate(root, UsdTraverseInstanceProxies());
    std::vector<UsdSkelBinding> bindings;
    cache.ComputeSkelBindings(root, &bindings, UsdTraverseInstanceProxies());
    for (const auto& b : bindings) {
      UsdSkelSkeletonQuery sq = cache.GetSkelQuery(b.GetSkeleton());
      VtMatrix4dArray xf;
      sq.ComputeSkinningTransforms(&xf, tc);
      for (const UsdSkelSkinningQuery& q : b.GetSkinningTargets()) {
        UsdPrim mp = q.GetPrim();
        VtVec3fArray pts;
        UsdGeomMesh(mp).GetPointsAttr().Get(&pts, tc);
        // blend shapes
        if (q.HasBlendShapes() && sq.GetAnimQuery()) {
          VtFloatArray w, sw; VtUIntArray si, ss;
          sq.GetAnimQuery().ComputeBlendShapeWeights(&w, tc);
          VtFloatArray mw;
          if (q.GetBlendShapeMapper()) q.GetBlendShapeMapper()->Remap(w, &mw); else mw = w;
          UsdSkelBlendShapeQuery bq{UsdSkelBindingAPI(mp)};
          VtFloatArray subW; VtUIntArray bIdx, subIdx;
          bq.ComputeSubShapeWeights(mw, &subW, &bIdx, &subIdx);
          bq.ComputeDeformedPoints(subW, bIdx, subIdx, bq.ComputeBlendShapePointIndices(),
                                   bq.ComputeSubShapePointOffsets(), pts);
        }
        VtMatrix4dArray mx = xf;
        if (q.GetJointMapper()) q.GetJointMapper()->RemapTransforms(xf, &mx);
        q.ComputeSkinnedPoints(mx, &pts, tc);
        printf("%s %zu\n", mp.GetPath().GetText(), pts.size());
        for (auto& v : pts) printf("%.6f %.6f %.6f\n", v[0], v[1], v[2]);
      }
    }
  }
}
