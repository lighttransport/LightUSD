import json
from pathlib import Path
import tempfile
import unittest
try:
    from pxr import Usd,UsdShade,UsdGeom,Sdf,Gf
except ImportError:
    Usd=None
from validation_stage import material_aov,normal_aov,probe,isolate,evaluated_snapshot


@unittest.skipIf(Usd is None,'requires OpenUSD SDK Python')
class ValidationStageTests(unittest.TestCase):
    def test_reference_population_closes_external_resources(self):
        from headless_stage import closed_population_mask
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'source.usda';stage=Usd.Stage.CreateNew(str(source))
            mesh=UsdGeom.Mesh.Define(stage,'/World/Selected/Mesh')
            UsdGeom.Cube.Define(stage,'/World/Other')
            material=UsdShade.Material.Define(stage,'/Materials/M')
            shader=UsdShade.Shader.Define(stage,'/Materials/M/S');shader.CreateIdAttr('UsdPreviewSurface')
            shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
            UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
            texture=UsdShade.Shader.Define(stage,'/Textures/T');texture.CreateIdAttr('UsdUVTexture')
            texture.CreateOutput('rgb',Sdf.ValueTypeNames.Float3)
            shader.CreateInput('diffuseColor',Sdf.ValueTypeNames.Color3f).ConnectToSource(texture.ConnectableAPI(),'rgb')
            prototype=UsdGeom.Sphere.Define(stage,'/Resources/Prototype')
            instancer=UsdGeom.PointInstancer.Define(stage,'/World/Selected/Instances')
            instancer.CreatePrototypesRel().SetTargets([prototype.GetPath()])
            UsdGeom.Camera.Define(stage,'/__ViewControl/Camera')
            stage.GetRootLayer().Save();before=source.read_bytes()
            result=closed_population_mask(stage,['/World/Selected'])
            mask=Usd.StagePopulationMask()
            for path in result['paths']:mask.Add(path)
            selected=Usd.Stage.OpenMasked(str(source),mask)
            for path in ('/World/Selected/Mesh','/Materials/M/S','/Textures/T','/Resources/Prototype','/__ViewControl/Camera'):
                self.assertTrue(selected.GetPrimAtPath(path),path)
            self.assertFalse(selected.GetPrimAtPath('/World/Other'))
            self.assertEqual(source.read_bytes(),before)
            for paths in (['/'],['/Missing']):
                with self.assertRaises(ValueError):closed_population_mask(stage,paths)

    def test_scalar_texture_adapter_preserves_channels_and_parameters(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('requires Pillow in SDK Python')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.usda'
            image=Image.new('RGBA',(2,1));image.putdata([(10,50,90,130),(20,60,100,140)])
            image.save(root/'input.png');image_before=(root/'input.png').read_bytes()
            stage=Usd.Stage.CreateNew(str(source))
            material=UsdShade.Material.Define(stage,'/M')
            shader=UsdShade.Shader.Define(stage,'/M/S');shader.CreateIdAttr('UsdPreviewSurface')
            shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
            mesh=UsdGeom.Mesh.Define(stage,'/Mesh');UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
            texture=UsdShade.Shader.Define(stage,'/M/T');texture.CreateIdAttr('UsdUVTexture')
            texture.CreateInput('file',Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath('input.png'))
            texture.CreateInput('sourceColorSpace',Sdf.ValueTypeNames.Token).Set('sRGB')
            texture.CreateInput('wrapS',Sdf.ValueTypeNames.Token).Set('clamp')
            texture.CreateInput('scale',Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(2,3,4,5))
            texture.CreateInput('bias',Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(.1,.2,.3,.4))
            reader=UsdShade.Shader.Define(stage,'/M/UV');reader.CreateIdAttr('UsdPrimvarReader_float2')
            reader.CreateInput('varname',Sdf.ValueTypeNames.Token).Set('UV1');reader.CreateOutput('result',Sdf.ValueTypeNames.Float2)
            texture.CreateInput('st',Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(),'result')
            for lane,channel in enumerate('rgba'):
                texture.CreateOutput(channel,Sdf.ValueTypeNames.Float)
                shader.CreateInput('roughness',Sdf.ValueTypeNames.Float).ConnectToSource(texture.ConnectableAPI(),channel)
                stage.GetRootLayer().Save();before=source.read_bytes()
                result=material_aov(str(source),root/(channel+'.usda'),0,'roughness')
                self.assertTrue(result['supported'],result['unsupported'])
                adapter=result['texture_adapters'][0]
                with Image.open(adapter['derived']) as derived:
                    self.assertEqual(list(derived.getdata()),[(10+40*lane,)*3,(20+40*lane,)*3])
                wrapped=Usd.Stage.Open(result['wrapper'])
                target=UsdShade.Shader(wrapped.GetPrimAtPath('/__ValidationScalarTextures/T0'))
                self.assertEqual(target.GetInput('st').GetAttr().GetConnections(),[Sdf.Path('/M/UV.outputs:result')])
                self.assertEqual(target.GetInput('wrapS').Get(),'clamp')
                self.assertEqual(target.GetInput('sourceColorSpace').Get(),'raw' if lane==3 else 'sRGB')
                self.assertEqual(tuple(target.GetInput('scale').Get())[:3],(lane+2,)*3)
                self.assertAlmostEqual(target.GetInput('bias').Get()[0],.1*(lane+1))
                self.assertEqual(source.read_bytes(),before)
                self.assertEqual((root/'input.png').read_bytes(),image_before)

    def test_material_values_are_sampled_without_source_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.usda';stage=Usd.Stage.CreateNew(str(source))
            shader=UsdShade.Shader.Define(stage,'/Material/Surface');shader.CreateIdAttr('UsdPreviewSurface')
            shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            material=UsdShade.Material.Define(stage,'/Material')
            material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
            mesh=stage.DefinePrim('/Geometry','Mesh')
            UsdShade.MaterialBindingAPI.Apply(mesh).Bind(material)
            roughness=shader.CreateInput('roughness',Sdf.ValueTypeNames.Float);roughness.Set(.2,1);roughness.Set(.8,3)
            diffuse=shader.CreateInput('diffuseColor',Sdf.ValueTypeNames.Color3f);diffuse.Set(Gf.Vec3f(.2,.3,.4))
            texture=UsdShade.Shader.Define(stage,'/Texture');texture.CreateIdAttr('UsdUVTexture');texture.CreateOutput('rgb',Sdf.ValueTypeNames.Float3)
            diffuse.ConnectToSource(texture.ConnectableAPI(),'rgb')
            guide=stage.DefinePrim('/Guide','Mesh');guide.CreateAttribute('purpose',Sdf.ValueTypeNames.Token).Set('guide')
            stage.GetRootLayer().Save();before=source.read_bytes()
            inventory=probe(str(source));self.assertEqual((inventory['start'],inventory['end']),(1,3))
            result=material_aov(str(source),root/'roughness.usda',2,'roughness')
            self.assertTrue(result['supported'])
            with_guide=material_aov(str(source),root/'with-guide.usda',2,'roughness',['default','render','guide'])
            self.assertFalse(with_guide['supported'])
            wrapped=Usd.Stage.Open(result['wrapper']);sample=UsdShade.Shader(wrapped.GetPrimAtPath('/Material/Surface')).GetInput('emissiveColor').Get()
            self.assertAlmostEqual(sample[0],.5);self.assertEqual(source.read_bytes(),before)
            final_shader=UsdShade.Shader(wrapped.GetPrimAtPath('/Material/Surface'))
            self.assertEqual(final_shader.GetInput('diffuseColor').GetAttr().GetConnections(),[])
            base=material_aov(str(source),root/'base.usda',2,'baseColor')
            base_stage=Usd.Stage.Open(base['wrapper']);base_shader=UsdShade.Shader(base_stage.GetPrimAtPath('/Material/Surface'))
            self.assertEqual(base_shader.GetInput('emissiveColor').GetAttr().GetConnections(),[Sdf.Path('/Texture.outputs:rgb')])
            self.assertEqual(base_shader.GetInput('diffuseColor').GetAttr().GetConnections(),[])
            roughness.ConnectToSource(shader.ConnectableAPI(),'scalar',UsdShade.AttributeType.Output)
            stage.GetRootLayer().Save()
            result=material_aov(str(source),root/'unsupported.usda',2,'roughness')
            self.assertFalse(result['supported']);self.assertTrue(result['unsupported'])

    def test_isolation_and_evaluated_snapshot_do_not_modify_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.usda';stage=Usd.Stage.CreateNew(str(source))
            stage.DefinePrim('/World','Xform');a=stage.DefinePrim('/World/Character','Xform');stage.DefinePrim('/World/Environment','Xform')
            attr=a.CreateAttribute('customValue',Sdf.ValueTypeNames.Float);attr.Set(2,0);attr.Set(6,1)
            stage.GetRootLayer().Save();before=source.read_bytes()
            result=isolate(str(source),root/'isolate.usda',['/World/Character'])
            loaded=Usd.Stage.Open(result['wrapper']);self.assertTrue(loaded.GetPrimAtPath('/World/Environment').IsActive());self.assertEqual(UsdGeom.Imageable(loaded.GetPrimAtPath('/World/Environment')).ComputeVisibility(),'invisible')
            result=evaluated_snapshot(result['wrapper'],root/'snapshot.usdc',.5)
            loaded=Usd.Stage.Open(result['wrapper']);value=loaded.GetPrimAtPath('/World/Character').GetAttribute('customValue')
            self.assertEqual(value.Get(),4);self.assertEqual(value.GetNumTimeSamples(),0)
            self.assertEqual(source.read_bytes(),before)
            self.assertTrue(loaded.GetPrimAtPath('/World/Environment'))

    def test_isolation_preserves_external_materials_and_prototypes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.usda';stage=Usd.Stage.CreateNew(str(source))
            stage.DefinePrim('/World','Xform');stage.DefinePrim('/World/Selected','Xform')
            mesh=UsdGeom.Mesh.Define(stage,'/World/Selected/Mesh')
            stage.DefinePrim('/World/Library','Xform')
            material=UsdShade.Material.Define(stage,'/World/Library/Material')
            shader=UsdShade.Shader.Define(stage,'/World/Library/Material/Shader')
            shader.CreateIdAttr('UsdPreviewSurface');shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
            UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
            instancer=UsdGeom.PointInstancer.Define(stage,'/World/Selected/Instances')
            UsdGeom.Cube.Define(stage,'/World/Library/Prototype')
            instancer.CreatePrototypesRel().SetTargets(['/World/Library/Prototype'])
            UsdGeom.Cube.Define(stage,'/World/Library/Unselected')
            stage.GetRootLayer().Save();before=source.read_bytes()
            result=isolate(str(source),root/'isolated.usda',['/World/Selected'])
            loaded=Usd.Stage.Open(result['wrapper'])
            bound,_=UsdShade.MaterialBindingAPI(loaded.GetPrimAtPath('/World/Selected/Mesh')).ComputeBoundMaterial()
            self.assertEqual(str(bound.GetPath()),'/World/Library/Material')
            self.assertTrue(loaded.GetPrimAtPath('/World/Library/Material/Shader').IsActive())
            self.assertEqual(UsdGeom.Imageable(loaded.GetPrimAtPath('/World/Library/Prototype')).ComputeVisibility(),'inherited')
            self.assertEqual(UsdGeom.Imageable(loaded.GetPrimAtPath('/World/Library/Unselected')).ComputeVisibility(),'invisible')
            self.assertEqual(source.read_bytes(),before)

    def test_hidden_curves_and_unused_surfaces_do_not_poison_diagnostics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source.usda';stage=Usd.Stage.CreateNew(str(source))
            stage.DefinePrim('/World','Xform')
            mesh=UsdGeom.Mesh.Define(stage,'/World/Selected')
            material=UsdShade.Material.Define(stage,'/World/Library/Material')
            shader=UsdShade.Shader.Define(stage,'/World/Library/Material/Shader')
            shader.CreateIdAttr('UsdPreviewSurface');shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
            UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material,materialPurpose=UsdShade.Tokens.preview)
            UsdGeom.BasisCurves.Define(stage,'/World/Other/Curves')
            unused=UsdShade.Shader.Define(stage,'/World/Other/Shader')
            unused.CreateIdAttr('UnsupportedSurface');unused.CreateOutput('surface',Sdf.ValueTypeNames.Token)
            stage.GetRootLayer().Save()
            isolated=isolate(str(source),root/'isolate.usda',['/World/Selected'])
            normal=normal_aov(isolated['wrapper'],root/'normal.usda',2)
            self.assertTrue(normal['supported'],normal['unsupported'])
            diagnostic=material_aov(isolated['wrapper'],root/'material.usda',2,'baseColor')
            self.assertTrue(diagnostic['supported'],diagnostic['unsupported'])
            self.assertEqual(diagnostic['converted'],['/World/Library/Material/Shader'])

    def test_authored_sampling_range_avoids_unneeded_clip_inventory(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'range.usda';stage=Usd.Stage.CreateNew(str(path))
            stage.SetStartTimeCode(10);stage.SetEndTimeCode(20)
            attr=stage.DefinePrim('/Animated','Xform').CreateAttribute('value',Sdf.ValueTypeNames.Float)
            attr.Set(1,1);attr.Set(2,100);stage.GetRootLayer().Save()
            inventory=probe(str(path))
            self.assertEqual((inventory['start'],inventory['end']),(10,20))
            self.assertIsNone(inventory['animated_attributes'])
            self.assertIsNone(inventory['authored_sample_range'])
            self.assertIn('authored-stage-range',inventory['inventory_mode'])

if __name__=='__main__':unittest.main()
