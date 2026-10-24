# Native import reports and policies

OBJ, URDF and MJCF imports use one C++ frontend for the native C and Python
APIs. Successful stages retain a versioned report, independent of the warning
string. Python returns a fresh dictionary on every query:

```python
import lightusd

stage = lightusd.import_mjcf(
    "robot.xml", max_memory=1024**3, max_input=128 * 1024**2,
    max_depth=64, unsupported_policy="preserve",
)
report = stage.import_report()
print(report["counts"], report["diagnostics"])

try:
    lightusd.load_bytes(b"v invalid 0 0\n", format="obj")
except lightusd.UsdError as error:
    print(error.import_report["error"])
```

Resolver exceptions keep their original Python type and message and receive
the failed attempt's `import_report` when the exception permits attributes.
Invalid Python arguments or native options may fail before an import starts;
they have no populated report. Runtime reports are not serialized into USD.
Created stages and stages reopened through USD-only APIs return `{}`.
Draining `stage.warnings()` does not change a report, and subsequent imports
do not change reports already attached to C/Python stages or exceptions.

## Version 1 fields

| Field | Meaning |
|---|---|
| `version`, `source`, `format`, `status`, `error` | Contract version, input identifier, detected format, `success`/`failed`, and fatal text |
| `diagnostics` | Records with stable `code`, `severity`, `message`, `source`, `line` and `prim_path` |
| `dependencies` | Requested `name`, `anchor`, resolved `identifier`, byte count, `kind` (`asset`/`texture`), `read`, `cached`, `success` |
| `features` | Feature name and `imported`, `approximated`, `skipped`, `rejected` or `preserved` disposition, source and line |
| `counts` | Available format-specific conversion counts, such as links, joints, materials and OBJ output vertices/indices |
| `input_bytes` | Primary source size when available |
| `elapsed_seconds` | Wall time in the outer import call; excludes serializing/querying the report |
| `estimated_peak_memory_bytes` | Peak importer allocation/budget bookkeeping; excludes report storage and output-stage allocations |
| `telemetry_estimated_bytes` | Separate bounded telemetry record estimate |
| `retained_texture_bytes` | Unique retained texture payload bytes within the import |
| `dependency_reads`, `dependency_bytes`, `texture_cache_hits` | Read attempts, returned byte sizes and cache hits, including records omitted by the telemetry bound |
| `limits` | Effective `max_memory_bytes`, `max_input_bytes`, `max_depth` |
| `dropped_records`, `truncated_fields` | Explicit telemetry omissions/truncations |

`line` is one-based when supplied by the parser; zero means unavailable.
`prim_path` is empty when a source record has no authored USD location.
Failed reads can have zero bytes, and a legacy combined resolver may perform
I/O to discover that an alias is already cached. `dependency_bytes` includes
bytes returned by failed reads. A cache hit's `bytes` describes the shared
payload without contributing another read or byte charge.

Reports do not claim complete detection of every unsupported input directive.
Some conversion warnings still use the general `import.warning` or
`import.conversion_warning` codes. Current specific codes include
`obj.parse_error`, `obj.parser_warning`, `obj.material_unresolved`,
`import.texture_unresolved`, `import.memory_limit`, `import.input_limit`,
`import.depth_limit`, `mjcf.element_unsupported`, `mjcf.equality_unsupported`,
`mjcf.tendon_wrap_type` (a wrap geom that is not a sphere or cylinder) and
`mjcf.ellipsoid_collision_approximated`. Soft bodies report
`mjcf.flexcomp_dof_approximated`, `mjcf.flexcomp_plugin`,
`mjcf.flexcomp_rigid` and `mjcf.cable_plugin_approximated` (cable bending as ball-joint stiffness E·I/L); the `deformables`
count lists converted flexcomps. URDF imports count `ghost_links` and
`merged_dummy_inertia_links`; `mjcf.texture_builtin_unsupported` reports a
builtin texture that is not generated (a cube texture on a material, invalid
size, or the `legacy` profile).
Further codes: `mjcf.integer_range` (an integer attribute outside int32 was
clamped), `mjcf.hfield_size`, `mjcf.unknown_class`, `mjcf.obj_first_object`
(MuJoCo converts only the first object of an OBJ mesh asset),
`mjcf.actuator_type_unsupported` (gain/bias/dynamics without an MjcPhysics
token), `mjcf.equality_site_unsupported` (legacy profile only),
`mjcf.inertia_convex_approximated`, `urdf.geometry_unsupported` (missing or
unknown URDF geometry, skipped under `warn`), `dae.up_axis_ignored`,
`dae.geometry_unsupported`, `dae.primitive_unsupported`,
`urdf.package_discovered` (see below), `urdf.package_root` (a URDF in a ROS
package's `urdf/` or `robots/` directory: the package directory, holding
`package.xml` or `meshes/`, is an allowed asset root, so `../meshes/...`
resolves), `urdf.mimic_chain_resolved` (a mimic joint following another mimic
joint is written against the chain's leading joint), `urdf.transmission_duplicate` (a
joint with several transmissions keeps the first) and
`mjcf.frame_scale_unsupported` (a MuJoCo 3.16 `<frame scale/massscale>`; the
frame's pose is applied, its scale is not) and `mjcf.obj_polygon_fan` (OBJ
polygons beyond the MuJoCo-style triangulation work limit became fans) and
`mjcf.obj_polygon_dropped` (OBJ polygons of more than 100 vertices, dropped as
MuJoCo's decoder drops them).
`import.failed` records the final failure.

`counts.inferred_inertia_bodies` counts MJCF bodies whose mass properties were
computed from their geoms (MuJoCo `inertiafromgeom`; see
[USD physics](usd-physics.md#output-profiles)).
The top-level `error` remains available if the diagnostic record budget is full.

Each record collection has at most 1,024 entries. Their shared accounting quota
is the smaller of 1 MiB and one sixteenth of `max_memory`; individual text
fields are limited to 4,096 bytes. Malformed UTF-8 in diagnostic text is escaped.
These are bounded accounting estimates, not measurements or guarantees of
process RSS. Preserved source data uses the ordinary import memory budget and
is never silently truncated with telemetry.

## Independent limits

Python `max_input` caps each primary source and dependency in bytes.
Omitting it retains the existing behavior: a positive `max_memory` also sets
the input cap, otherwise the input cap is 512 MiB. Explicit `max_input=0`
selects the finite 512 MiB default independently of `max_memory`.
`max_memory=0` keeps the 1 GiB import-accounting default.
`max_depth` defaults to 64 and accepts 1 through 1,024. It bounds XML nesting
and include expansion, including skipped subtrees and the expanded document.
Filesystem assets are size-checked before allocation; custom resolvers supply
bytes before their size can be checked. Neither limit replaces the resolver's
own I/O or security policy.

## URDF packages

`package://NAME/path` resolves through `packages` (Python), `"packages"` in
the C `settings_json` or `--package NAME=DIR`. An unmapped package is
discovered as Newton's and urdf-usd-converter's URDF importers do: the nearest
ancestor of the URDF's directory named `NAME`, otherwise the nearest ancestor
`DIR` for which `DIR/path` exists. Paths containing `..` are not searched, and
discovery needs a filesystem source (no resolver callbacks). Each discovered
root is reported as `urdf.package_discovered` and becomes an allowed asset root
for that package's files.

## Progress and cancellation

`lightusd_import_options.progress(user_data, phase, done, total)` (C; the
fields follow `settings_json`, and callers built against the shorter struct
keep working) and `progress=callable` (Python `import_obj/urdf/mjcf`) report
the import as it proceeds: phase `"import"` with a monotonic count of bytes
accounted against the memory budget (at most once per MiB) and the budget as
`total`, then `"convert"` before the USD stage is built. A nonzero C return,
or a Python `False`, cancels: the import fails with `LIGHTUSD_ERR_CANCELLED`
(Python `UsdError`, message "Import cancelled") and the report records the
failure. An exception raised by the Python callback cancels too and is
re-raised from the import call. The callback runs on the importing thread
(Python re-acquires the GIL for it).

## Parallel mesh parsing

MJCF and URDF imports parse their mesh assets (OBJ, STL, DAE) on worker
threads before the model is built: `threads=0` (the default; C `settings_json` `"threads"`)
uses up to 8, `1` parses serially, larger values set the count. Workers use
private readers. A parsed mesh is kept only when its side effects were
dependency reads, budget charges, warnings and OBJ/MTL materials, and those
are replayed into the import at the mesh's first use: the stage, warnings,
report (dependencies, diagnostics, peak memory) and budget failures are the
same as a serial import. A mesh that read textures or failed, or whose
transient peak no longer fits the remaining budget, is parsed again serially
at that point. Workers run only without resolver callbacks, and they never
call `progress`. Across Menagerie (220 MJCF files, unlimited and 24 MiB
budgets) serial and parallel imports agree exactly; fruitfly takes 0.29 s
instead of 0.86 s, panda 0.09 s instead of 0.34 s, stretch 0.30 s instead
of 0.70 s. URDF OBJ/DAE meshes, which are parsed per use, replay once per
use. A 63-mesh URDF (OBJ, STL and DAE, each used as visual and collision)
imports in 0.15 s instead of 0.60 s, identical at every budget around its
peak, as is the urdf-usd-converter test corpus. URDF STL meshes are now read
once, so each appears once in `dependencies` and is charged once (they were
previously read for resolution and again by the mesh loader).

## Unsupported-feature policy

The default `unsupported_policy="warn"` retains previous behavior. `"error"`
rejects known unconverted data. `"preserve"` retains source records under the
layer's `customLayerData["lightusdImportUnsupported"]` as a JSON string.
Each entry has `feature`, `source`, `line`, `encoding`, `data` and
`executable: false`. XML is normalized by the XML parser; material data is a
JSON record. This data survives USD export but does not add solver or shader
execution support. Memory exhaustion during preservation fails the import.

The policy currently covers:

- MJCF `flex`, `flexcomp`, `deformable` and `skin`, including included files;
- unsupported MJCF equality types and spherical joint cone limits;
- spatial tendon surface wrapping and pulley approximation;
- nonuniform ellipsoid colliders retained as scaled analytic spheres;
- OBJ/MTL bump maps and unconverted displacement, sheen and anisotropy fields.

For tendons, `preserve` also retains the source tendon record while the existing
approximate path remains in the scene. Source-relative references inside
preserved records are not resolved or packaged. Unknown directives, missing
assets and other conversion warnings retain their existing handling; this
policy does not make all source schemas executable or exhaustively validated.

Nonuniform ellipsoid colliders retain the existing scaled-sphere representation
by default. The native physics validator flags that representation. Set
`tessellate_collision_shapes=True` (CLI: `--tessellate-collision-shapes`) to use
a polygon mesh approximation instead. The unsupported-feature policy applies
to the analytic approximation: `error` rejects it and `preserve` also retains
the source geom record.

MJCF partial solver vectors are completed using inherited class values, followed
by MJCF defaults. `mjcf.solver_defaults` and the `completed_solver_arrays` count
report these completions; excess entries fail the import. Scene option enums
use the USD schema spelling, such as `newton`, `euler` and `rk4`.
`mjcf.token_canonicalized` records changes from XML spelling. These conversions
preserve the source solver settings rather than adding a simulation engine.

## Native APIs

Include `c-api/lightusd-import-c.h` and link `lightusd_render_c` (or the shared
C ABI). The existing `lightusd_import_options` layout is unchanged; its JSON
settings accept `unsupported_policy`. `max_input_bytes`, `max_memory_bytes`
and `max_depth` remain separate fields. Query reports with:

```c
lightusd_string *report = NULL;
lightusd_stage_import_report(stage, &report); /* owned JSON */
/* inspect lightusd_string_view(report), then lightusd_string_destroy(report) */
```

After an import fails, `lightusd_import_last_report(&report)` returns the last
attempt on the calling thread. Copy it before another import on that thread.
Invalid native arguments/options reset it to `{}`. Ordinary USD loads do not
replace the last import report.

C++ uses `import_asset::Options::report`, a shared `ImportReport` collector,
and `Result::report`. Supply a collector to inspect failed imports or direct
OBJ layer loading. Outermost calls reset reused collectors, while nested
file/parse/conversion calls contribute to the same report. A collector belongs
to one synchronous import at a time; use separate collectors concurrently.
