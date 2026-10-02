"""Small generated reference fixtures; keep binary packages out of source control."""
from pathlib import Path
import zipfile

HEADER = '#usda 1.0\n(\n defaultPrim = "World"\n upAxis = "Y"\n metersPerUnit = 1\n)\n'


def generate(root: Path):
    cases = {
        'clean': 'def Xform "World" {}',
        'physics-values': '''def Xform "World" {
 def Sphere "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]) {
  float physics:mass = -1
  float physics:density = -1
  float3 physics:diagonalInertia = (1,1,1)
 }
 def Sphere "Collider" (prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsMassAPI"]) {
  float physics:mass = -1
  float physics:density = -1
  float3 physics:diagonalInertia = (1,1,1)
  double3 xformOp:scale = (1,2,3)
  uniform token[] xformOpOrder = ["xformOp:scale"]
 }
 def Points "Points" (prepend apiSchemas = ["PhysicsCollisionAPI"]) {}
 def Scope "ScopeBody" (prepend apiSchemas = ["PhysicsRigidBodyAPI"]) {}
 def PhysicsFixedJoint "Joint" {
  rel physics:body0 = [</World/Body>, </World/Collider>]
  rel physics:body1 = </World/Missing>
 }
 def Xform "Articulation" (prepend apiSchemas = ["PhysicsArticulationRootAPI", "PhysicsRigidBodyAPI"]) {
  bool physics:rigidBodyEnabled = false
  def Xform "Nested" (prepend apiSchemas = ["PhysicsArticulationRootAPI"]) {}
 }
}''',
        'physics-valid': '''def Xform "World" {
 def Sphere "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsCollisionAPI"]) {
  float physics:mass = 1
  float physics:density = 1
  float3 physics:diagonalInertia = (1,1,1)
  quatf physics:principalAxes = (1,0,0,0)
 }
}''',
        'physics-oriented-scale': '''def Xform "World" {
 double3 xformOp:scale = (1,2,3)
 uniform token[] xformOpOrder = ["xformOp:scale"]
 def Sphere "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI"]) {
  float xformOp:rotateZ = 45
  uniform token[] xformOpOrder = ["xformOp:rotateZ"]
 }
}''',
        'physics-instance': '''def Xform "World" {
 def Xform "Source" {
  def Sphere "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI"]) {}
 }
 def Xform "Instance" (references = </World/Source> instanceable = true) {}
}'''.replace('references = </World/Source> instanceable', 'references = </World/Source>\n instanceable'),
        'shader-implementation': '''def Xform "World" {
 def Material "Material" {
  def Shader "Shader" { uniform token info:implementationSource = "invalid" }
 }
}''',
        'collection-target': '''def Xform "World" (prepend apiSchemas = ["MaterialBindingAPI"]) {
 rel material:binding:collection:test = [</World.collection:missing>, </World/M>]
 def Material "M" {}
}''',
        'normal-missing-file': '''def Xform "World" {
 def Material "M" {
  def Shader "Surface" {
   uniform token info:id = "UsdPreviewSurface"
   normal3f inputs:normal.connect = </World/M/Texture.outputs:rgb>
  }
  def Shader "Texture" {
   uniform token info:id = "UsdUVTexture"
   float3 outputs:rgb
  }
 }
}''',
    }
    paths = []
    for name, body in cases.items():
        path = root / (name + '.usda')
        path.write_text(HEADER + body + '\n')
        paths.append(path)
    for compressed in (False, True):
        path = root / ('compressed.usdz' if compressed else 'misaligned.usdz')
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED) as archive:
            archive.writestr('root.usda', HEADER + cases['clean'])
        paths.append(path)
    return paths
