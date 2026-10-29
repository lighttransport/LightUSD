# Typed physics snapshots

`physics.inspect()` uses the shared native C++/C snapshot query. It collects
authored UsdPhysics, MuJoCo, Newton and other engine properties without render
conversion. Unknown properties in the supported namespaces retain their
authored names, types and values.

```python
from lightusd import physics

snapshot = physics.inspect(stage, time=0, array_views=True,
                           max_memory=512 * 1024**2)
for body in snapshot.rigid_bodies:
    print(body.prim_path, body.attributes)
    print(body.property_types, body.uniform_properties)
```

`time=None` requests USD DefaultTime; numeric time evaluates authored defaults
and time samples coherently, including interpolation. Records include property types,
uniform property names, blocked property names, applied APIs and relationship
targets. A value block remains `lightusd.ValueBlock` when no fallback resolves
beneath it. Declared attributes with no resolved value are `None`. Unauthored
schema properties are not added to the snapshot.

The default preserves the existing immutable tuple representation. With
`array_views=True`, top-level numeric array attributes use read-only
`lightusd.Array` buffers. NumPy can view these without per-element Python
objects. Strings and string arrays remain Python strings and tuples; dictionary
metadata is recursively frozen. Numeric array storage is retained independently
of both the stage and temporary native snapshot, and survives stage edits and
closure. Interpolation or lazy decoding can create new native array storage.

Records can appear in several groups; the same record object is shared between
those groups. Groups include scenes, bodies/mass, colliders, joints, materials,
articulations, filtered pairs, actuators, sites, tendons, equalities, sensors,
contacts, keyframes, collision groups and other extensions. Body/mass grouping
does not imply an enabled dynamic rigid body. The query preserves descriptive
data; it does not validate or execute a physics simulation.

`max_memory` bounds conservative native snapshot bookkeeping, including retained
array payload estimates; zero selects 512 MiB. `max_records` defaults to
1,000,000 and must be positive. These are not process RSS limits or limits on
Python tuple expansion. Use array views for large state arrays. Numeric time
must be finite. Importing and querying the same stage must not overlap with
mutations from another thread, following the ordinary C API threading contract.

## Native C++ and C

C++ uses `next::QueryPhysics(stage, PhysicsQueryOptions)` from
`next/schema/physics-query.hh`. It returns an `expected<PhysicsSnapshot,
std::string>`. Records hold owned typed `Value`s and explicit group masks.
Read arrays through const values to retain shared storage; mutable Value
accessors intentionally detach through copy-on-write.

The dependency-free C query is in `c-api/lightusd-physics-c.h`; link
`lightusd_c` (no Tydra or engine requirement):

```c
lightusd_physics_query_options options;
lightusd_physics_query_options_init(&options);
options.time_code = 0.0;
lightusd_physics_snapshot *snapshot = NULL;
if (lightusd_stage_query_physics(stage, &options, &snapshot) == LIGHTUSD_OK) {
    size_t count = lightusd_physics_snapshot_size(snapshot);
    /* record/schema/property/relationship accessors enumerate typed data */
    (void)count;
    lightusd_physics_snapshot_destroy(snapshot);
}
```

Record and property descriptors borrow strings from the snapshot. Property
queries return separately owned `lightusd_value` handles, and relationship
queries return owned target lists. Destroy each with its ordinary destructor.
An owned value remains valid after the snapshot is destroyed. The generic
`lightusd_value_get_dictionary()` cursor borrows from its owned value.
Null outputs are invalid arguments; invalid indexes clear outputs and return
`LIGHTUSD_ERR_NOT_FOUND`. Query failures clear the snapshot output.

The existing seven-group `RenderScene.physics` annotation API remains available
for render consumers. Typed stage snapshots cover the additional authored
engine records and arrays directly. [Structured native validation](physics-validation.md)
is available through `physics.validate()`.

## Schema descriptors

`physics.schema_definitions()` exposes immutable UsdPhysics, MuJoCo and Newton
definitions from the generated native C11 tables. Each schema records its parent,
local properties, multiple-apply namespace, defining source file and
`provenance` (upstream project, pinned version and commit). Properties record
their USD type, variability, relationship kind and allowed token values. The
tables use the repository's frozen schema manifests; recognition describes the
schema inventory rather than solver execution support.

Python authoring uses these definitions for standard property types and
variability. Drive and limit properties use their instance namespaces, for
example `drive:angular:physics:stiffness` and `limit:rotX:physics:low`.
`PhysicsDriveAPI`/`PhysicsLimitAPI` require a nonempty instance, while single-apply
schemas reject an instance suffix. Engine extension authoring also uses the
generated descriptors, with an explicit type inventory for legacy properties
outside those schemas.
Native C consumers can query the same permanent descriptor views through the
`lu_validation_schema_*` and `lu_validation_property_*` C11 APIs without linking
the C++ validation engine.
