# Structured physics validation

`lightusd.physics.validate()` runs the native semantic validator and returns an
immutable report. The same checks are available through the C and C++ APIs;
Python does not implement separate physics rules.

```python
from lightusd import physics

report = physics.validate(stage, samples=True, max_issues=10000)
for issue in report.issues:
    print(issue.severity, issue.rule_id, issue.location, issue.time, issue.message)
print(report.valid, report.complete, report.checked_groups)
```

Findings return a report, including invalid physics. Invalid API arguments and
closed stages raise exceptions. Reports own their strings and remain usable
after stage edits or closure. Validation does not modify the input stage.

## Coverage and sampling

The default enables the native `core` and `physics` rule groups against the
stage's root layer, including authored variant content as supported by the
native layer validator. Use `include_core=False` for physics rules alone.
Core checks include schema property types, relationships and authoring metadata.
Physics checks include the existing mass/inertia, joint, collision, articulation,
MuJoCo and Newton constraints implemented by the native validator.

`samples=False` checks authored/default values using existing native behavior.
`samples=True` additionally evaluates coherent snapshots at authored sample
times and spline knots. Sample findings carry numeric `time`; default findings
carry `None`. This is a check at sampled times, not a proof over every instant
between samples. Clip evaluation needs an asset loader; the stage-only C/Python
boundary does not provide one, so required clip coverage is reported incomplete.

`require_complete=True` also requests native coverage diagnostics for missing
schema definitions. Recognition of an annotation schema does not imply complete
solver support. This API performs semantic validation; it does not compile a
MuJoCo model, run Newton, infer missing mass/inertia, or establish simulation
equivalence. Package/crate byte validation is outside this stage-only API.

## Report fields and limits

`PhysicsValidationReport` contains:

- `issues`: immutable records with `severity`, `rule_id`, `location`, `message`,
  optional `source_asset` and `variants`, and `time` (`None` for default checks);
- `checked_groups`: the native groups that ran;
- `error_count` and `warning_count`: counts of **retained** findings;
- `omitted_issues`: findings omitted by diagnostic limits;
- `complete`: whether requested native coverage/reporting completed;
- `valid`: `complete` with no retained errors; `bool(report)` returns this value.

Warnings alone do not fail validation. Any report-limit omission forces both
`complete` and `valid` false, including when all errors were omitted and
`error_count` is zero. An empty issue list therefore must not be used as the
success test.

Defaults are 10,000 retained issues, 8 MiB conservative retained-diagnostic
accounting (`max_report_bytes`) and 256 sampled times (`max_samples`). Limits
must be positive. The native collector enforces issue limits while collecting
and merging, including variant and sampled results. Oversized individual
findings are omitted, rather than silently shortening their locations/messages.
Reaching the sampled-time limit produces a coverage diagnostic and skips the
numeric pass. These limits do not measure process RSS or bound the loaded stage,
evaluation scratch, custom callback allocations or total validation time.

Issue order matches the native presentation order: errors before warnings,
layer findings first, then location and rule. `source_asset`/`variants` may be
empty when the validator has no such provenance; locations remain available.

## Native APIs

Include `lightusd-validation-c.h` and link `lightusd_validation_c`, or use the
shared C ABI. This component remains optional for core/query/render consumers.

```c
lightusd_validation_options options;
lightusd_validation_options_init(&options);
options.groups = LIGHTUSD_VALIDATE_CORE | LIGHTUSD_VALIDATE_PHYSICS;
options.flags = LIGHTUSD_VALIDATE_SAMPLES;
lightusd_validation_report *report = NULL;
if (lightusd_stage_validate(stage, &options, &report) == LIGHTUSD_OK) {
    lightusd_validation_summary summary;
    lightusd_validation_report_summary(report, &summary);
    /* Findings return OK; inspect summary.valid/complete and enumerate issues. */
    lightusd_validation_report_destroy(report);
}
```

Issue string views borrow from the report. Invalid indexes return
`LIGHTUSD_ERR_NOT_FOUND` and clear outputs; query failures clear the owning
report output. The previous `lightusd_stage_validate_core()` count API keeps
its behavior. The C++ ownership facade adds `api::ValidationReport` and
`api::validate()`.

Native C++ callers can use `ValidationOptions::max_issues` and
`max_report_bytes` with the existing validator. Their defaults remain unlimited
for compatibility. `USDValidationResult::AddIssue()` and
`MergeValidationResults()` apply the destination limits and preserve omissions.

## Dataset regression

The Menagerie runner has an opt-in semantic validation gate:

```sh
PYTHONPATH="$ISOLATED_PYTHON_PACKAGE" python3 tests/core/run-menagerie-import.py \
  --menagerie "$MENAGERIE_ROOT" --out build_ninja/physics-validation-menagerie \
  --only xml --reference-only --validate-physics --workers 4
```

Import success, existing MuJoCo reference comparisons and semantic validation
are reported separately. Validation findings retain their native codes in
`physics_validation`; `physics_validation_failures` counts invalid/incomplete
reports and causes the opt-in gate to fail. This does not change the runner's
default profile.

Add `--tessellate-collision-shapes` for the explicit mesh-collider profile.
At Menagerie revision `cda5236f98383f6fbaa4820c6b18dba892cd087f`, all 223
standalone entries passed import and the existing MuJoCo 3.16.0 rest-frame,
joint-anchor/limit and record-count comparisons. All 223 also passed semantic
validation with mesh colliders. With the unchanged analytic defaults, 10
entries failed `physics.collider.nonUniformScale` on nonuniform ellipsoid
colliders (50 findings). Import diagnostics now expose that approximation;
see [import policies](import-report.md). Mesh tessellation does not establish
analytic collision or dynamics equivalence.

This sweep also covers inherited partial solver vectors, canonical scene option
tokens, zero solver tolerances and the `jointactuatorfrc` sensor. Inertia and
principal-axis fallback values are validated independently, matching OpenUSD's
mass-property fallback behavior. Zero quaternions remain invalid for joint
rotations. These checks do not infer missing physical properties.

## Comparing physics after loading

Semantic validity, data preservation and simulation agreement are separate gates.
The headless validation tooling now implements independent after-load snapshots:
[`physics_snapshot.py`](../tools/view_control/physics_snapshot.py) loads a file in
either OpenUSD or LightUSD and captures composed authored physics fields at
explicit time codes. [`physics_compare.py`](../tools/view_control/physics_compare.py)
compares the snapshots with bounded diagnostics and numeric tolerances. It checks
prim presence/types, ordered applied schema tokens, property types/variability,
connections, values and resolved relationship targets. It includes `mjc:` and
`newton:` fields, multi-apply `drive:<axis>:physics:*` / `limit:*`, physics material
bindings and collider collections. Empty coverage cannot pass.

Do not use SDK `GetAppliedSchemas()` alone to test extension-token preservation:
an SDK without those plugins can omit unregistered schemas. The snapshot compares
the composed `apiSchemas` list-op and still reads unknown authored properties.
This establishes authored-field fidelity, not plugin fallback or solver support.
The generic native property query is an authored-data interface; an absent value
must not be mistaken for the effective UsdPhysics schema fallback.

The recommended matrix is:

1. Compare the native in-memory import against the source compiler for mapped
   semantics, with explicit expected/check counts so skipped bodies or joint
   anchors cannot produce a pass. Check rest poses, joint limits in degrees,
   target relationships and engine-record counts.
2. Save USDA, USDC and USDZ; independently load each in both libraries. Compare
   each loaded snapshot against its independent SDK snapshot and against the
   original composed reference. A serializer and its reader agreeing on the same
   mistaken value is insufficient evidence.
3. Run semantic validation separately, with coherent sampled times, coverage and
   omission flags checked. Invalid physics that roundtrips perfectly is still
   invalid. Valid `PhysicsDriveAPI:linear` and `:angular` instances are accepted;
   `distance` remains a limit instance, not a drive instance.
4. Compare physical meaning in common units: SI mass, center of mass and inertia
   tensor. A tensor comparison handles principal-axis quaternion sign and
   eigenvalue ordering without hiding a changed inertia. Keep raw authored-value
   differences visible as a separate fidelity result.
5. Extend effective-value coverage using schema fallbacks from each engine,
   distinguishing missing, blocked and authored values and unknown definitions.
   Add contacts/material combine rules, joint/drive context, collection expansion,
   instancing, payloads, clips and authored variant combinations. Missing required
   coverage is incomplete, not a pass.
6. For dynamics, initialize the same articulated state and drive/contact settings
   in independent solvers, then compare short deterministic trajectories and
   invariants with declared tolerances. USD property/mesh comparisons and static
   MuJoCo compiler checks do not establish solver equivalence.

[`menagerie_validate.py`](../tools/view_control/menagerie_validate.py) implements
the conversion, compiler, semantic and per-format loaded-field checks for selected
Menagerie models. Its compiler checks include explicit mass and center of mass
and reconstruct body-space inertia tensors in SI units. The preserved `newton`
profile and reduced `openusd` profile are reported separately.

The fixtures `tests/usda/physics-load-compare-{base,root}.usda` exercise references,
variant selection, centimeter/milligram metadata, body targets, angular drives
and interpolation at a non-authored time. Mutation tests ensure lost fields,
MJC data, relationship targets, units and sample values fail comparison. See
[the measured Menagerie report](validation/menagerie-2026-11-02.md) for the scope
actually checked; the broader effective-value/dynamics matrix remains future work.

Snapshot revision 2 extends the file-load gate with collider shape attributes,
canonical point/topology digests and evaluated world transforms. This catches a
changed vertex or index even when mesh/physics counts match. The digest is an
exact loaded-data comparison; SI tensor and compiler comparisons remain separate
semantic tests. The reference/variant fixture also animates a parent transform,
so the midpoint check exercises a composed collider world matrix after loading.

Original MJCF EGL captures with the shared USD camera add an independent geometry
oracle to the source-compiler checks. The measured scenes expose infinite source
planes versus finite USD previews in full-frame depth coverage. Thus matching
mass/inertia and perfectly preserved USD properties still do not establish
complete conversion or simulation equivalence.

### Dataset gap closure gates

The staged catalogue is `tests/verification/dataset-gaps.json`. Required gates
remain open until supported by contracts and evidence. The assertion aggregator
in `tools/view_control/verification.py` treats controls and unsupported/missing
coverage as incomplete. It does not establish truth by counting agreeing engines.
Performance eligibility additionally requires equal workload/quality fingerprints,
successful correctness checks and uncontended measurements. The configured
30 GiB host / 12 GiB GPU budgets are ceilings, not evidence of resource parity.
Mass-property compiler comparisons reject invalid authored unit scales instead of
substituting a default for zero kilograms-per-unit.

The [contact-response controls](validation/physics-contact-response-2026-11-03.md)
retain source defaults and add an explicitly configured stiffer case without
changing quality thresholds. Typed after-load `mjc:solref`/gap checks, dense
impact samples and separate solver outcomes distinguish conversion fidelity,
compliance choices and consumer capability. A matching failed trajectory never
becomes a pass merely because two loaders agree.
