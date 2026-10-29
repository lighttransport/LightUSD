// SPDX-License-Identifier: Apache-2.0
#ifndef LIGHTUSD_PHYSICS_C_H_
#define LIGHTUSD_PHYSICS_C_H_
#include "lightusd-c.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lightusd_physics_snapshot lightusd_physics_snapshot;
typedef enum lightusd_physics_group {
  LIGHTUSD_PHYSICS_SCENES, LIGHTUSD_PHYSICS_RIGID_BODIES, LIGHTUSD_PHYSICS_COLLIDERS,
  LIGHTUSD_PHYSICS_JOINTS, LIGHTUSD_PHYSICS_MATERIALS, LIGHTUSD_PHYSICS_ARTICULATION_ROOTS,
  LIGHTUSD_PHYSICS_FILTERED_PAIRS, LIGHTUSD_PHYSICS_ACTUATORS, LIGHTUSD_PHYSICS_SITES,
  LIGHTUSD_PHYSICS_TENDONS, LIGHTUSD_PHYSICS_EQUALITIES, LIGHTUSD_PHYSICS_SENSORS,
  LIGHTUSD_PHYSICS_CONTACTS, LIGHTUSD_PHYSICS_KEYFRAMES, LIGHTUSD_PHYSICS_EXTENSIONS,
  LIGHTUSD_PHYSICS_COLLISION_GROUPS, LIGHTUSD_PHYSICS_GROUP_COUNT
} lightusd_physics_group;
#define LIGHTUSD_PHYSICS_DEFAULT_TIME 1u
typedef struct lightusd_physics_query_options {
  uint32_t struct_size, flags;
  uint64_t max_memory_bytes, max_records;
  double time_code;
} lightusd_physics_query_options;
typedef struct lightusd_physics_record_info {
  lightusd_sv prim_path, type_name;
  uint64_t groups; /* bit (1 << lightusd_physics_group); records may overlap */
  size_t schema_count, property_count, relationship_count;
} lightusd_physics_record_info;
typedef struct lightusd_physics_property_info {
  lightusd_sv name, type_name;
  uint8_t uniform, blocked;
} lightusd_physics_property_info;
LIGHTUSD_API void lightusd_physics_query_options_init(lightusd_physics_query_options*);
/* Authored data at one time, including unknown engine extension properties.
 * Independent of render conversion. Snapshot owns immutable values and is valid
 * after stage mutation/destruction. Null options use numeric time 0 and finite
 * defaults (512 MiB accounting, 1,000,000 records). */
LIGHTUSD_API lightusd_status lightusd_stage_query_physics(const lightusd_stage*,
    const lightusd_physics_query_options*, lightusd_physics_snapshot** out);
LIGHTUSD_API void lightusd_physics_snapshot_destroy(lightusd_physics_snapshot*);
LIGHTUSD_API size_t lightusd_physics_snapshot_size(const lightusd_physics_snapshot*);
LIGHTUSD_API const char* lightusd_physics_group_name(uint32_t group);
/* All svs below are borrowed from the snapshot. Invalid indexes return
 * NOT_FOUND and clear outputs. The value getter returns a separate owned handle. */
LIGHTUSD_API lightusd_status lightusd_physics_snapshot_record(
    const lightusd_physics_snapshot*, size_t, lightusd_physics_record_info*);
LIGHTUSD_API lightusd_status lightusd_physics_snapshot_schema(
    const lightusd_physics_snapshot*, size_t record, size_t schema, lightusd_sv*);
LIGHTUSD_API lightusd_status lightusd_physics_snapshot_property(
    const lightusd_physics_snapshot*, size_t record, size_t property,
    lightusd_physics_property_info*, lightusd_value** value);
LIGHTUSD_API lightusd_status lightusd_physics_snapshot_relationship(
    const lightusd_physics_snapshot*, size_t record, size_t relationship,
    lightusd_sv* name, lightusd_strlist** targets);

/* Physics-aware unit conversion, in place (the native lightusd.units).
 * Attributes are scaled by L^a * M^b (L = old/new metersPerUnit, M = old/new
 * kilogramsPerUnit) for their length/mass dimension: gprim sizes, points,
 * extents, translations, mass properties, joint frames, linear limits, drives,
 * joint state, gravity, density, NewtonJointAPI/NewtonMassAPI, Newton SDF
 * narrow bands, joint break force/torque, distance-joint limits and
 * deformable masses. OpenVDBAsset fields get an innermost
 * xformOp:scale:units (their .vdb voxels keep the file's units). Angles stay
 * in degrees and mjc:* keeps MuJoCo units. Time samples
 * convert with defaults. A different up_axis ("Y"/"Z") prepends
 * xformOp:rotateX:upAxis to the root prims (a reverse conversion removes it)
 * and rotates PhysicsScene gravity directions. meters_per_unit /
 * kilograms_per_unit of 0 and a NULL/empty up_axis keep the stage's value
 * (unauthored: 0.01 / 1 / "Y"). report and skipped may be NULL; skipped lists
 * "<prim>.<attr>" DOF attributes whose linear/angular kind is unknown.
 * Attributes are converted in place one at a time (no stage-sized copy), so
 * an error return leaves the stage partially converted: discard it. */
typedef struct lightusd_unit_conversion {
  double length_scale, mass_scale;
  size_t converted, rotated_roots;
} lightusd_unit_conversion;
LIGHTUSD_API lightusd_status lightusd_stage_convert_units(lightusd_stage*,
    double meters_per_unit, double kilograms_per_unit, const char* up_axis,
    lightusd_unit_conversion* report, lightusd_strlist** skipped);
#ifdef __cplusplus
}
#endif
#endif
