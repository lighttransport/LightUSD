#!/usr/bin/env python3
"""Independent authored physics-property snapshots after loading USD.

Run once with LightUSD Python and once with SDK Python. Values are evaluated at
explicit USD times; no text roundtrip or physics solver is used by this worker.
"""
import argparse
import json
import math
import hashlib
import struct
from pathlib import Path

PREFIXES=('physics:','physx','mjc:','mujoco:','newton:')
SCHEMA_FIELDS={
    'PhysicsRigidBodyAPI':('rigidBodyEnabled','kinematicEnabled','startsAsleep','velocity','angularVelocity'),
    'PhysicsCollisionAPI':('collisionEnabled',),
    'PhysicsMeshCollisionAPI':('approximation',),
    'PhysicsCollisionGroup':('invertFilteredGroups','mergeGroup'),
    'PhysicsMassAPI':('mass','density','centerOfMass','diagonalInertia','principalAxes'),
    'PhysicsMaterialAPI':('staticFriction','dynamicFriction','restitution','density'),
    'PhysicsScene':('gravityDirection','gravityMagnitude'),
    'PhysicsJoint':('jointEnabled','collisionEnabled','excludeFromArticulation','breakForce','breakTorque','localPos0','localPos1','localRot0','localRot1'),
    'PhysicsRevoluteJoint':('axis','lowerLimit','upperLimit'),
    'PhysicsPrismaticJoint':('axis','lowerLimit','upperLimit'),
    'PhysicsSphericalJoint':('axis','coneAngle0Limit','coneAngle1Limit'),
    'PhysicsDistanceJoint':('minDistance','maxDistance'),
}


def effective_names(type_name,schemas):
    names=set()
    for schema in [type_name,*schemas]+(['PhysicsJoint'] if type_name.endswith('Joint') and type_name.startswith('Physics') else []):
        names.update('physics:'+field for field in SCHEMA_FIELDS.get(schema,()))
        if schema.startswith(('PhysicsDriveAPI:','PhysicsLimitAPI:')):
            kind,instance=schema.split(':',1)
            fields=('type','maxForce','targetPosition','targetVelocity','damping','stiffness') if kind=='PhysicsDriveAPI' else ('low','high')
            names.update(('drive:' if kind=='PhysicsDriveAPI' else 'limit:')+instance+':physics:'+field for field in fields)
    if type_name=='PhysicsCollisionGroup':
        names.update(('collection:colliders:expansionRule','collection:colliders:includeRoot'))
    return sorted(names)


def sdk_default_state(attribute):
    from pxr import Sdf,Usd
    for spec in attribute.GetPropertyStack(Usd.TimeCode.Default()):
        if spec.HasInfo('default'):
            return 'blocked' if isinstance(spec.GetInfo('default'),Sdf.ValueBlock) else 'value'
    return 'absent'


def physics_property(name):
    return name.startswith(PREFIXES) or ':physics:' in name or name.startswith(('material:binding:physics','collection:colliders:'))

def plain(value):
    if value is None or isinstance(value,(str,bool,int)):return value
    if isinstance(value,float):
        if not math.isfinite(value):return {'nonfinite':str(value)}
        return value
    if hasattr(value,'GetReal'):return [float(value.GetReal()),*[float(x) for x in value.GetImaginary()]]
    if hasattr(value,'path'):return value.path
    if isinstance(value,dict):return {str(k):plain(v) for k,v in value.items()}
    try:return [plain(v) for v in value]
    except TypeError:return str(value)


GEOMETRY=('points','faceVertexIndices','faceVertexCounts','holeIndices','subdivisionScheme','orientation','size','radius','height','axis','extent')

def geometry_value(name,value):
    if value is None:return None
    if name not in ('points','faceVertexIndices','faceVertexCounts','holeIndices','extent'):return plain(value)
    digest=hashlib.sha256();components=0
    def append(item):
        nonlocal components
        if isinstance(item,(float,int)):
            number=float(item) if name in ('points','extent') else int(item)
            if isinstance(number,float) and not math.isfinite(number):raise ValueError('nonfinite collider geometry')
            if number==0:number=0 # canonicalize signed zero
            digest.update(struct.pack('<f' if name in ('points','extent') else '<q',number));components+=1
        else:
            for sub in item:append(sub)
    append(value)
    return {'elements':len(value),'components':components,'sha256':digest.hexdigest(),'encoding':'float32_le' if name in ('points','extent') else 'int64_le'}


def snapshot(scene,engine,times):
    if engine not in ('openusd','lightusd'):raise ValueError('unknown snapshot engine')
    if not 1<=len(times)<=129 or any(t is not None and (type(t) not in (int,float) or not math.isfinite(t)) for t in times):raise ValueError('invalid bounded snapshot time codes')
    records={}
    if engine=='openusd':
        from pxr import Usd,UsdGeom
        stage=Usd.Stage.Open(str(scene))
        metadata={k:plain(stage.GetMetadata(k)) for k in ('upAxis','metersPerUnit','kilogramsPerUnit','timeCodesPerSecond')}
        for prim in Usd.PrimRange.Stage(stage,Usd.TraverseInstanceProxies()):
            # GetAppliedSchemas omits schemas unknown to this SDK installation.
            # Compare the composed authored list-op instead, preserving extension tokens.
            operation=prim.GetMetadata('apiSchemas')
            schemas=[str(s) for s in operation.GetAppliedItems()] if operation else []
            attributes=[a for a in prim.GetAuthoredAttributes() if physics_property(a.GetName())]
            relationships=[r for r in prim.GetAuthoredRelationships() if physics_property(r.GetName())]
            if not attributes and not relationships and not prim.GetTypeName().startswith(('Physics','Mjc','Newton')) and not any('Physics' in s or 'Mjc' in s or 'Mujoco' in s or 'Newton' in s for s in schemas):continue
            record={'type':prim.GetTypeName(),'schemas':schemas,'attributes':{},'relationships':{r.GetName():[str(p) for p in r.GetTargets()] for r in relationships}}
            for a in attributes:
                record['attributes'][a.GetName()]={'type':str(a.GetTypeName()),'uniform':str(a.GetVariability())=='Sdf.VariabilityUniform',
                    'authored_default_state':sdk_default_state(a),
                    'connections':[str(p) for p in a.GetConnections()],
                    'values':{str(t):plain(a.Get(Usd.TimeCode.Default() if t is None else Usd.TimeCode(t))) for t in times}}
            record['effective_attributes']={}
            for name in effective_names(prim.GetTypeName(),schemas):
                attribute=prim.GetAttribute(name)
                record['effective_attributes'][name]={'status':'available' if attribute else 'unavailable',
                    'values':{str(t):plain(attribute.Get(Usd.TimeCode.Default() if t is None else Usd.TimeCode(t))) if attribute else None for t in times}}
            if 'PhysicsCollisionAPI' in schemas:
                geometry={}
                for name in GEOMETRY:
                    attr=prim.GetAttribute(name)
                    if attr and attr.HasAuthoredValueOpinion():
                        sample_times=times if attr.GetNumTimeSamples() else [None]
                        geometry[name]={'type':str(attr.GetTypeName()),'values':{str(t):geometry_value(name,attr.Get(Usd.TimeCode.Default() if t is None else Usd.TimeCode(t))) for t in sample_times}}
                record['collision_geometry']=geometry
                record['world_transform']={str(t):plain(UsdGeom.XformCache(Usd.TimeCode(t)).GetLocalToWorldTransform(prim)) for t in times if t is not None}
            records[str(prim.GetPath())]=record
    else:
        import lightusd
        stage=lightusd.load(scene,max_memory=4*1024**3)
        metadata={k:plain(stage.get_metadata(k)) for k in ('upAxis','metersPerUnit','kilogramsPerUnit','timeCodesPerSecond')}
        for prim in stage:
            schemas=list(prim.metadata('apiSchemas') or [])
            attributes=[a for a in prim.attributes if physics_property(a)]
            relationships=[r for r in prim.relationships if physics_property(r)]
            if not attributes and not relationships and not prim.type_name.startswith(('Physics','Mjc','Newton')) and not any('Physics' in s or 'Mjc' in s or 'Mujoco' in s or 'Newton' in s for s in schemas):continue
            record={'type':prim.type_name,'schemas':schemas,'attributes':{},'relationships':{r:list(prim.relationship(r).targets) for r in relationships}}
            for name in attributes:
                a=prim.attribute(name)
                record['attributes'][name]={'type':a.type_name,'uniform':a.is_uniform,'connections':list(a.connections),
                    'authored_default_state':'blocked' if a.is_default_blocked else 'value' if a.has_authored_default else 'absent',
                    'values':{str(t):plain(a.resolve(time=t)) for t in times}}
            record['effective_attributes']={}
            for name in effective_names(prim.type_name,schemas):
                try:attribute=prim.attribute(name)
                except KeyError:attribute=None
                record['effective_attributes'][name]={'status':'available' if attribute is not None else 'unavailable',
                    'values':{str(t):plain(attribute.resolve(time=t)) if attribute is not None else None for t in times}}
            if 'PhysicsCollisionAPI' in schemas:
                geometry={}
                for name in GEOMETRY:
                    if name in prim.attributes:
                        attr=prim.attribute(name);sample_times=times if attr.has_timesamples else [None]
                        geometry[name]={'type':attr.type_name,'values':{str(t):geometry_value(name,attr.resolve(time=t)) for t in sample_times}}
                record['collision_geometry']=geometry
                record['world_transform']={str(t):plain(prim.world_transform(time=t)) for t in times if t is not None}
            records[prim.path]=record
        stage.close()
    return {'engine':engine,'scene':str(scene),'times':times,'metadata':metadata,'prims':records,
            'snapshot_revision':3,'scope':'Authored defaults distinguish value/block/absence; explicit DefaultTime versus numeric evaluation; selected effective UsdPhysics schema attributes including fallbacks; collider geometry digests/world transforms; extension properties, relationships and applied schemas. Agreement is not an independent semantic oracle or solver equivalence.'}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--engine',choices=('openusd','lightusd'),required=True);parser.add_argument('--scene',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);parser.add_argument('--times',nargs='+',type=float,default=[0])
    args=parser.parse_args();result=snapshot(args.scene,args.engine,[None]+args.times);args.out.write_text(json.dumps(result,allow_nan=False,indent=2)+'\n')
