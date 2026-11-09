// SPDX-License-Identifier: Apache 2.0
// USD Physics and MuJoCo physics annotation unit tests

#pragma once

void physics_scene_reconstruct_test(void);
void physics_scene_mjc_scene_api_test(void);
void physics_scene_newton_api_test(void);
void physics_scene_newton_xpbd_api_test(void);
void physics_revolute_joint_test(void);
void physics_prismatic_joint_test(void);
void physics_fixed_joint_test(void);
void physics_distance_joint_test(void);
void physics_distance_joint_json_usdc_roundtrip_test(void);
void physics_joint_mjc_api_test(void);
void physics_joint_mjc_full_json_usdc_roundtrip_test(void);
void physics_mjc_joint_authorship_usdc_roundtrip_test(void);
void physics_joint_physx_state_mirror_test(void);
void physics_joint_physx_state_usdc_roundtrip_test(void);
void physics_prismatic_state_init_test(void);
void physics_joint_physx_state_to_json_test(void);
void physics_joint_mjc_usdc_roundtrip_test(void);
void physics_joint_newton_mimic_api_test(void);
void mjc_actuator_test(void);
void newton_actuator_test(void);
void newton_actuator_extended_api_test(void);
void mjc_tendon_test(void);
void physics_mjc_tendon_full_usdc_roundtrip_test(void);
void mjc_keyframe_test(void);
void physics_pprint_roundtrip_test(void);
void physics_spherical_joint_test(void);
void physics_to_json_test(void);
void physics_to_json_include_defaults_test(void);
void physics_mjc_tendon_full_to_json_test(void);
void physics_collision_group_test(void);
void physics_collision_group_colliders_test(void);
void physics_filtered_pairs_api_test(void);
void physics_collision_group_invert_test(void);
void physics_drive_limit_api_test(void);
void physics_drive_limit_multi_dof_parse_test(void);
void physics_schema_defaults_and_owner_test(void);
void physics_spherical_schema_names_fixture_test(void);
void physics_mesh_collider_convention_test(void);
void physics_newton_collision_material_api_test(void);
void urdf_json_spherical_joint_export_test(void);
void urdf_json_newton_api_export_test(void);
void urdf_json_mjcf_contact_export_test(void);
void physics_urdf_upaxis_axis_invariant_test(void);

// Large-scene crate-writer regression canaries
void physics_rigidbody_mass_usdc_roundtrip_test(void);
void physics_collision_material_usdc_roundtrip_test(void);
void physics_drive_limit_usdc_roundtrip_test(void);
void physics_joints_localframe_usdc_roundtrip_test(void);
void physics_collision_group_usdc_roundtrip_test(void);
void physics_scene_full_mjc_newton_usdc_roundtrip_test(void);
void physics_to_json_after_usdc_test(void);
void physx_scene_rigidbody_roundtrip_test(void);

// Phase 1d: MJCF tendon / equality conversion (JSON->USD)
void urdf_json_mjcf_tendon_export_test(void);
void urdf_json_mjcf_tendon_full_export_test(void);
void urdf_json_mjcf_tendon_alias_export_test(void);
void urdf_json_mjc_actuator_full_export_test(void);
void urdf_json_mjcf_equality_export_test(void);

// Phase 1c: full inertia tensor diagonalization
void urdf_json_fullinertia_diagonalize_test(void);

// Phase 3b: schema-coverage tests
void physics_articulation_root_api_test(void);
void mjc_equality_api_test(void);

// Spatial (muscle) tendon + sites + muscle actuator conversion
void urdf_json_mjcf_muscle_export_test(void);

// Full <option>/<flag>/<compiler> -> MjcSceneAPI
void urdf_json_mjc_scene_options_test(void);

// <keyframe> -> MjcKeyframe
void urdf_json_mjc_keyframe_export_test(void);

// <light>/<camera> -> UsdLux + GeomCamera
void urdf_json_mjc_lights_cameras_test(void);

// <asset><material> -> UsdShade Material + binding
void urdf_json_mjc_materials_test(void);

// <sensor> -> MjcSensor typed prim
void urdf_json_mjc_sensors_test(void);
void urdf_json_mjc_sensor_aliases_test(void);

// adhesion/cylinder/... actuator coverage
void urdf_json_mjc_actuator_types_test(void);

// <contact><pair> conversion
void urdf_json_mjc_contact_pair_test(void);

// mocap body + <custom> conversion
void urdf_json_mjc_mocap_custom_test(void);

// MjcActuator full-field USDC round-trip (ctrlRange/forceRange/gainPrm/...)
void physics_mjc_actuator_usdc_roundtrip_test(void);

// <extension><plugin> + <actuator><plugin> -> MjcPlugins + MjcActuator plugin
void urdf_json_mjc_plugin_test(void);

// static "world" link (worldbody-level geoms) -> static collider, no artic root
void physics_static_world_link_test(void);

// <freejoint> -> floating-base articulation root with mjc:freeJoint marker
void physics_freejoint_floating_base_test(void);

void physics_obj_c11_import_test(void);
void physics_obj_export_facevarying_test(void);

void physics_obj_composition_test(void);
