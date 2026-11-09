#pragma once

// DAG-based composition engine tests
// Uses pseudo-random USD generation with fixed seed for reproducibility

// Core DAG construction tests
void compgraph_basic_prim_index_test(void);
void compgraph_strength_order_test(void);
void compgraph_nested_strength_order_test(void);
void compgraph_inherits_dag_test(void);
void compgraph_specializes_globally_weak_test(void);
void compgraph_references_dag_test(void);
void compgraph_variants_deferred_test(void);
void compgraph_cycle_detection_test(void);
void compgraph_implied_inherits_test(void);

// Instancing tests
void compgraph_instance_key_identical_test(void);
void compgraph_instance_key_different_test(void);

// Lazy payload tests
void compgraph_payload_deferred_test(void);

// BuildStage correctness: compare DAG vs iterative pipeline
void compgraph_build_stage_simple_test(void);
void compgraph_build_stage_no_duplicate_children_test(void);
void compgraph_validate_no_duplicate_sibling_names_test(void);
void compgraph_build_stage_wide_deep_test(void);
void compgraph_build_stage_inherits_test(void);

// Random USD generation tests (AOUSD spec based)
void compgraph_random_flat_prims_test(void);
void compgraph_random_deep_hierarchy_test(void);
void compgraph_random_inherits_chain_test(void);
void compgraph_random_mixed_arcs_test(void);
void compgraph_random_specializes_vs_inherits_test(void);
