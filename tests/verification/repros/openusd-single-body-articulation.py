#!/usr/bin/env python3
"""Upstream-ready regression: one real body must survive articulation parsing.

The fixed-base world sentinel is retained for API compatibility; it must not
replace the only real body. No renderer, asset, converter or solver is involved.
"""
import unittest
from pxr import Usd,UsdGeom,UsdPhysics,Sdf


class SingleBodyArticulation(unittest.TestCase):
    def check_case(self,root_kind):
        stage=Usd.Stage.CreateInMemory()
        root=UsdGeom.Xform.Define(stage,'/World')
        body=UsdGeom.Xform.Define(stage,'/World/Body')
        UsdPhysics.RigidBodyAPI.Apply(body.GetPrim())
        if root_kind=='floating':
            UsdPhysics.ArticulationRootAPI.Apply(body.GetPrim())
        else:
            joint=UsdPhysics.RevoluteJoint.Define(stage,'/World/Joint')
            joint.CreateBody1Rel().SetTargets([body.GetPath()])
            target=joint.GetPrim() if root_kind=='joint' else root.GetPrim()
            UsdPhysics.ArticulationRootAPI.Apply(target)
        parsed=UsdPhysics.UsdPhysicsLoadStageFromPrimRange(stage,[Sdf.Path.absoluteRootPath])
        paths,descs=parsed[UsdPhysics.ObjectType.Articulation]
        self.assertEqual(len(descs),1)
        self.assertTrue(descs[0].isValid)
        real_bodies=[path for path in descs[0].articulatedBodies if path!=Sdf.Path.emptyPath]
        self.assertEqual(real_bodies,[body.GetPath()])
        if root_kind!='floating':self.assertIn(Sdf.Path('/World/Joint'),descs[0].articulatedJoints)

    def test_floating_single_body(self):self.check_case('floating')
    def test_world_anchored_root_joint(self):self.check_case('joint')
    def test_world_anchored_root_ancestor(self):self.check_case('ancestor')


if __name__=='__main__':unittest.main()
