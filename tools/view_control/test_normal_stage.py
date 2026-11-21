import hashlib
from pathlib import Path
import tempfile
import unittest
try:
    from pxr import Usd,UsdGeom
except ImportError:
    Usd=UsdGeom=None
from validation_stage import normal_aov

@unittest.skipIf(Usd is None,'requires SDK Python')
class NormalStageTests(unittest.TestCase):
    def test_display_override_keeps_animated_source_attributes(self):
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);source=root/'source.usda'
            source.write_text('''#usda 1.0
 def Mesh "M" {
  uniform token subdivisionScheme = "none"
  bool doubleSided = true
  point3f[] points.timeSamples = {1: [(0,0,0)], 2: [(1,0,0)]}
  normal3f[] normals = [(0,0,1)]
 }
''')
            before=hashlib.sha256(source.read_bytes()).hexdigest()
            result=normal_aov(str(source),root/'normal.usda')
            self.assertTrue(result['supported'])
            self.assertEqual(result['double_sided_overrides'],['/M'])
            stage=Usd.Stage.Open(result['wrapper']);mesh=stage.GetPrimAtPath('/M')
            self.assertFalse(mesh.GetAttribute('doubleSided').Get())
            self.assertEqual(mesh.GetAttribute('points').GetTimeSamples(),[1,2])
            self.assertEqual(tuple(mesh.GetAttribute('points').Get(2)[0]),(1,0,0))
            self.assertEqual(set(mesh.GetPrimStack()[0].attributes.keys()),{'doubleSided'})
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)

    def test_instance_display_override_preserves_source_animation(self):
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);source=root/'source.usda'
            source.write_text('''#usda 1.0
 def Xform "Asset" {
  def Mesh "M" {
   bool doubleSided = true
   point3f[] points.timeSamples = {1: [(0,0,0)], 2: [(1,0,0)]}
  }
 }
 def Xform "Instance" (references = </Asset>
 instanceable = true) {}
''')
            before=hashlib.sha256(source.read_bytes()).hexdigest()
            result=normal_aov(str(source),root/'normal.usda')
            self.assertTrue(result['supported'])
            self.assertEqual(result['deinstanced_for_display_override'],['/Instance'])
            stage=Usd.Stage.Open(result['wrapper'])
            mesh=stage.GetPrimAtPath('/Instance/M')
            self.assertFalse(stage.GetPrimAtPath('/Instance').IsInstance())
            self.assertFalse(mesh.GetAttribute('doubleSided').Get())
            self.assertEqual(tuple(mesh.GetAttribute('points').Get(2)[0]),(1,0,0))
            override=stage.GetRootLayer().GetPrimAtPath('/Instance/M')
            self.assertTrue(override is None or 'points' not in override.attributes)
            original=Usd.Stage.Open(str(source))
            self.assertTrue(original.GetPrimAtPath('/Instance').IsInstance())
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)

    def test_curves_cannot_count_as_calibrated_mesh_normals(self):
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);source=root/'source.usda'
            source.write_text('#usda 1.0\ndef BasisCurves "C" {}\n')
            result=normal_aov(str(source),root/'normal.usda')
            self.assertFalse(result['supported']);self.assertEqual(len(result['unsupported']),1)


@unittest.skipIf(Usd is None,'requires SDK Python')
class GeometryStageTests(unittest.TestCase):
    def test_root_binding_overrides_descendant_opacity_without_geometry_bake(self):
        from validation_stage import geometry_aov
        from pxr import UsdShade
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);source=root/'source.usda'
            source.write_text('''#usda 1.0
 def Xform "World" {
  def Mesh "M" (prepend apiSchemas = ["MaterialBindingAPI"]) {
   rel material:binding = </World/Old>
   rel material:binding:preview = </World/Old>
   point3f[] points.timeSamples = {1: [(0,0,0)], 2: [(1,0,0)]}
  }
  def Material "Old" {
   token outputs:surface.connect = </World/S.outputs:surface>
  }
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   float inputs:opacity = 0
   token outputs:surface
  }
 }
''')
            result=geometry_aov(str(source),root/'opaque.usda')
            self.assertTrue(result['supported'])
            stage=Usd.Stage.Open(result['wrapper']);mesh=stage.GetPrimAtPath('/World/M')
            self.assertEqual(mesh.GetAttribute('points').GetTimeSamples(),[1,2])
            for purpose in (UsdShade.Tokens.allPurpose,UsdShade.Tokens.preview):
                bound,_=UsdShade.MaterialBindingAPI(mesh).ComputeBoundMaterial(purpose)
                self.assertEqual(str(bound.GetPath()),'/__view_control_opaque/Material')
            self.assertNotIn('M',stage.GetRootLayer().GetPrimAtPath('/World').nameChildren)


if __name__ == "__main__": unittest.main()
