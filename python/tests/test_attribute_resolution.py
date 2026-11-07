# SPDX-License-Identifier: Apache-2.0
import math
import pytest
import lightusd


def test_explicit_resolution_distinguishes_default_samples_and_blocks():
    stage=lightusd.loads('''#usda 1.0
 def Xform "P" {
  float sample.timeSamples = {1: 2, 3: 6}
  float declared
  float blocked = None
  float value = 7
 }
''')
    prim=stage.prim_at('/P')
    sample=prim.attribute('sample')
    assert not sample.has_authored_default
    assert sample.resolve() is None
    assert sample.resolve(time=2)==4
    assert sample.resolve(time=2,interpolation='held')==2
    assert not prim.attribute('declared').has_authored_default
    assert prim.attribute('declared').resolve() is None
    blocked=prim.attribute('blocked')
    assert blocked.has_authored_default and blocked.is_default_blocked
    assert blocked.resolve() is None
    value=prim.attribute('value')
    assert value.has_authored_default and not value.is_default_blocked
    assert value.resolve()==7
    for time in (math.inf,math.nan):
        with pytest.raises(ValueError):sample.resolve(time=time)
    with pytest.raises(ValueError):sample.resolve(interpolation='nearest')


def test_resolve_schema_fallback_without_authoring_it():
    stage=lightusd.loads('''#usda 1.0
 def Xform "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]) {}
''')
    body=stage.prim_at('/Body')
    enabled=body.attribute('physics:rigidBodyEnabled')
    assert not enabled.has_authored_default
    assert enabled.resolve() is True
    mass=body.attribute('physics:mass')
    assert not mass.has_authored_default
    assert mass.resolve()==0


def test_physics_fallback_contracts_and_multiapply_names():
    stage=lightusd.loads('''#usda 1.0
 def Xform "Mass" (prepend apiSchemas = ["PhysicsMassAPI"]) {}
 def PhysicsRevoluteJoint "Joint" (prepend apiSchemas = ["PhysicsDriveAPI:angular", "PhysicsLimitAPI:transX"]) {}
''')
    mass=stage.prim_at('/Mass')
    assert tuple(mass.attribute('physics:principalAxes').resolve())==(0,0,0,0)
    assert all(v==-math.inf for v in mass.attribute('physics:centerOfMass').resolve())
    joint=stage.prim_at('/Joint')
    expected={'physics:jointEnabled':True,'physics:collisionEnabled':False,
              'physics:excludeFromArticulation':False,'physics:breakForce':math.inf,
              'physics:breakTorque':math.inf,'physics:axis':'X',
              'physics:lowerLimit':-math.inf,'physics:upperLimit':math.inf,
              'drive:angular:physics:type':'force','drive:angular:physics:maxForce':math.inf,
              'drive:angular:physics:stiffness':0,'limit:transX:physics:low':-math.inf,
              'limit:transX:physics:high':math.inf}
    for name,value in expected.items():
        attribute=joint.attribute(name)
        assert not attribute.has_authored_default
        assert attribute.resolve()==value
    assert tuple(joint.attribute('physics:localRot0').resolve())==(1,0,0,0)
    with pytest.raises(KeyError):joint.attribute('drive:linear:physics:type')


def test_collision_group_declarations_and_aousd_collection_fallbacks():
    stage=lightusd.loads('''#usda 1.0
 def PhysicsCollisionGroup "Group" {}
 def Mesh "Collider" (prepend apiSchemas = ["PhysicsMeshCollisionAPI"]) {}
''')
    group=stage.prim_at('/Group')
    # Declared without defaults in the UsdPhysics schema: do not invent values.
    for name in ('physics:invertFilteredGroups','physics:mergeGroup'):
        attr=group.attribute(name)
        assert not attr.has_authored_default
        assert attr.resolve() is None
    assert group.attribute('collection:colliders:expansionRule').resolve()=='expandPrims'
    # AOUSD Core 1.0.1 and 1.1 explicitly specify this fallback.
    assert group.attribute('collection:colliders:includeRoot').resolve() is False
    assert stage.prim_at('/Collider').attribute('physics:approximation').resolve()=='none'


def test_adjacent_clips_interpolate_without_missing_value_option(tmp_path, monkeypatch):
    # Each file has just one sample: their active start times form a continuous
    # timeline. Linear sampling must bracket across the file boundary.
    (tmp_path / 'a.usda').write_text('''#usda 1.0
 def Xform "Root" {
  float x.timeSamples = {10: 2}
  normal3f[] normals.timeSamples = {10: [(1,0,0), (0,1,0)]}
 }
''')
    (tmp_path / 'b.usda').write_text('''#usda 1.0
 def Xform "Root" {
  float x.timeSamples = {20: 6}
  normal3f[] normals.timeSamples = {20: [(0,1,0), (0,0,1)]}
 }
''')
    root = tmp_path / 'root.usda'
    root.write_text('''#usda 1.0
 def Xform "Root" (clips = { dictionary default = {
  asset[] assetPaths = [@a.usda@, @b.usda@]
  double2[] active = [(0,0), (2,1)]
  double2[] times = [(0,10), (2,20)]
  string primPath = "/Root"
 } }) {
  float x
  normal3f[] normals
 }
''')
    monkeypatch.chdir(tmp_path)
    with lightusd.load(root.name) as stage:
        monkeypatch.chdir(tmp_path.parent)
        prim = stage.prim_at('/Root')
        assert prim.attribute('x').resolve(time=1) == 4
        assert prim.attribute('x').resolve(time=1, interpolation='held') == 2
        assert list(prim.attribute('normals').resolve(time=1)) == [(0.5,0.5,0), (0,0.5,0.5)]
        assert prim.attribute('x').resolve(time=0) == 2
        assert prim.attribute('x').resolve(time=2) == 6
    root.write_text(root.read_text().replace('[(0,10), (2,20)]',
                                              '[(0,10), (2,20), (2,0), (4,20)]'))
    with lightusd.load(root) as stage:
        x = stage.prim_at('/Root').attribute('x')
        assert x.resolve(time=1) == 2
        assert x.resolve(time=2-1e-9) == 2
        assert x.resolve(time=2) == 6


def test_file_clip_evaluation_retains_explicit_array_limit(tmp_path):
    values=', '.join(str(i) for i in range(1024))
    with lightusd.loads('#usda 1.0\ndef Xform "Root" {\n int[] values.timeSamples = {0: ['+values+']}\n}') as clip:
        clip.save(str(tmp_path/'clip.usdc'))
    root=tmp_path/'root.usda'
    root.write_text('''#usda 1.0
 def Xform "Root" (clips = { dictionary default = {
  asset[] assetPaths = [@clip.usdc@]
  double2[] active = [(0,0)]
  string primPath = "/Root"
 } }) {
  int[] values
 }
''')
    with lightusd.load(root,max_array_elements=16) as stage:
        with pytest.raises(lightusd.UsdError,match='max_array_elements'):
            stage.prim_at('/Root').attribute('values').resolve(time=0)
    with lightusd.load(root,max_array_elements=2048) as stage:
        assert list(stage.prim_at('/Root').attribute('values').resolve(time=0)) == list(range(1024))


def test_package_root_and_clip_keep_archive_anchor(tmp_path):
    import zipfile
    path=tmp_path/'clips.usdz'
    with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('nested/root.usda','''#usda 1.0
 def Xform "Root" (clips = { dictionary default = {
  asset[] assetPaths = [@../clips/a.usda@, @../clips/b.usda@]
  double2[] active = [(0,0), (2,1)]
  string primPath = "/Root"
 } }) {
  float value
 }
''')
        archive.writestr('clips/a.usda','#usda 1.0\ndef Xform "Root" {\n float value.timeSamples = {0: 2}\n}\n')
        archive.writestr('clips/b.usda','#usda 1.0\ndef Xform "Root" {\n float value.timeSamples = {2: 6}\n}\n')
    for source in (str(path),str(path)+'[nested/root.usda]'):
        with lightusd.load(source) as stage:
            assert stage.prim_at('/Root').attribute('value').resolve(time=1) == 4


def test_connection_to_clip_keeps_lazy_file_loader(tmp_path):
    (tmp_path/'clip.usda').write_text('''#usda 1.0
 def Xform "Root" {
  float x.timeSamples = {0: 2, 2: 6}
 }
''')
    root=tmp_path/'root.usda'
    root.write_text('''#usda 1.0
 def Xform "Root" (clips = { dictionary default = {
  asset[] assetPaths = [@clip.usda@]
  double2[] active = [(0,0)]
  string primPath = "/Root"
 } }) {
  float x
 }
 def Xform "Consumer" {
  float x.connect = </Root.x>
 }
''')
    with lightusd.load(root) as stage:
        value=stage.prim_at('/Consumer').attribute('x')
        assert value.resolve(time=1,follow_connections=True) == 4
        assert value.resolve(time=1,follow_connections=False) is None
        (tmp_path/'clip.usda').write_text('#usda 1.0\ndef Xform \"Root\" {\n float x.timeSamples = {0: 4, 2: 8}\n}')
        assert value.resolve(time=1,follow_connections=True) == 6
