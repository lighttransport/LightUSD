#pragma once

//
// pretty-print routine(using iostream) in non-intrusive way.
// Some build configuration may not want I/O module(e.g. mobile/embedded
// device), so provide print routines in separated file.
//
//

#include <ostream>
#include <sstream>
#include <string>

// Sub-headers (pprint-meta.hh transitively includes pprint-enum.hh)
#include "pprint-meta.hh"

// Domain headers (for prim-type to_string declarations)
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"
#include "mjcPhysics.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "core/layer-types.hh"  // LayerMetas
#include "layer.hh"             // Layer

namespace lightusd {

//
// Setting `closing_brace` false won't emit `}`(for printing USD scene graph
// recursively).
//

// Geom prim to_string (defined in pprint-geom.cc)
std::string to_string(const Model &model, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Scope &scope, const uint32_t indent = 0,
                      bool closing_brace = true);
// UsdRender / UsdProc placeholder prim types.
std::string to_string(const RenderSettings &v, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const RenderProduct &v, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const RenderVar &v, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GenerativeProcedural &v, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GPrim &gprim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Xform &xform, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomSphere &sphere, const uint32_t indent = 0,
                      bool closing_brace = true);
// UsdVol
std::string to_string(const Volume &volume, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const VolumeFieldBase &field, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const FieldBase &field, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const VolumeFieldAsset &field, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const FieldAsset &field, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const OpenVDBAsset &asset, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Field3DAsset &asset, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomMesh &mesh, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomPoints &pts, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomBasisCurves &curves, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomNurbsCurves &curves, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCapsule &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCone &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCylinder &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCube &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCamera &camera, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomSubset &subset, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomSubset::ElementType ty);
std::string to_string(const GeomSubset::FamilyType ty);
std::string to_string(const GeomBasisCurves::Wrap &v);
std::string to_string(const GeomBasisCurves::Type &v);
std::string to_string(const GeomBasisCurves::Basis &v);
std::string to_string(const GeomPlane &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCylinder_1 &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomCapsule_1 &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomTetMesh &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomNurbsPatch &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomHermiteCurves &geom, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeomPointInstancer &instancer,
                      const uint32_t indent = 0, bool closing_brace = true);

std::string to_string(const GeomMesh::InterpolateBoundary interp_boundary);
std::string to_string(const GeomMesh::SubdivisionScheme subd_scheme);
std::string to_string(const GeomMesh::FaceVaryingLinearInterpolation fv);
std::string to_string(const GeomMesh::TriangleSubdivisionRule tri_subdiv_rule);

std::string to_string(const GeomCamera::Projection &proj);
std::string to_string(const GeomCamera::StereoRole &role);

// Skel prim to_string (defined in pprint-skel.cc)
std::string to_string(const SkelRoot &root, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Skeleton &skel, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const SkelAnimation &anim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const BlendShape &bs, const uint32_t indent = 0,
                      bool closing_brace = true);

// Physics prim to_string (defined in pprint-physics.cc)
std::string to_string(const PhysicsJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsScene &scene, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsRevoluteJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsPrismaticJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsSphericalJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsFixedJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsDistanceJoint &joint, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PhysicsCollisionGroup &group, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const MjcActuator &actuator, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const NewtonActuator &actuator, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const MjcTendon &tendon, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const MjcKeyframe &keyframe, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const MjcSensor &sensor, const uint32_t indent = 0,
                      bool closing_brace = true);

// AR/Interactive prim to_string (defined in pprint-ar.cc)
std::string to_string(const Preliminary_PhysicsGravitationalForce &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_InfiniteColliderPlane &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_ReferenceImage &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_Behavior &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_Trigger &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_Action &prim, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Preliminary_Text &prim, const uint32_t indent = 0,
                      bool closing_brace = true);

// usdMedia prim to_string (defined in pprint-media.cc)
std::string to_string(const SpatialAudio &prim, const uint32_t indent = 0,
                      bool closing_brace = true);

// Light prim to_string (defined in pprint-light.cc)
std::string to_string(const SphereLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const DomeLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const DiskLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const DistantLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const CylinderLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const RectLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const GeometryLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PortalLight &light, const uint32_t indent = 0,
                      bool closing_brace = true);

std::string to_string(const DomeLight_1 &light, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const LightFilter &filter, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PluginLightFilter &filter, const uint32_t indent = 0,
                      bool closing_brace = true);

std::string to_string(const DomeLight::TextureFormat &texformat);

// Shader/Material to_string (defined in pprint-shader.cc)
std::string to_string(const Material &material, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const NodeGraph &nodegraph, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const Shader &shader, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const UsdPreviewSurface &shader,
                      const uint32_t indent = 0, bool closing_brace = true);
std::string to_string(const UsdUVTexture &shader, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const UsdPrimvarReader_float2 &shader,
                      const uint32_t indent = 0, bool closing_brace = true);

std::string to_string(const UsdPreviewSurface::OpacityMode v);
std::string to_string(const UsdUVTexture::SourceColorSpace v);
std::string to_string(const UsdUVTexture::Wrap v);

// Layer printing (defined in pprinter.cc)
std::string print_layer_metas(const LayerMetas &metas, const uint32_t indent);
std::string print_layer(const Layer &layer, const uint32_t indent,
                        bool parallel = false);

std::string to_string(const Layer &layer, const uint32_t indent = 0,
                      bool closing_brace = true);
std::string to_string(const PrimSpec &primspec, const uint32_t indent = 0,
                      bool closing_brace = true);

}  // namespace lightusd
