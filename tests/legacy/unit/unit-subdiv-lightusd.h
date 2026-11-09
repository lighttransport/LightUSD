#pragma once

// Validation & Conversion
void subdiv_validate_halfedge_test(void);
void subdiv_convert_to_halfedge_test(void);
void subdiv_convert_from_halfedge_test(void);
void subdiv_invalid_mesh_rejected_test(void);

// Catmull-Clark
void subdiv_cc_quad_test(void);
void subdiv_cc_quad_l2_test(void);
void subdiv_cc_cube_test(void);
void subdiv_cc_cube_l2_test(void);
void subdiv_cc_boundary_test(void);
void subdiv_cc_facepoint_position_test(void);

// Loop
void subdiv_loop_triangle_test(void);
void subdiv_loop_triangle_l2_test(void);
void subdiv_loop_tetrahedron_test(void);
void subdiv_loop_rejects_quads_test(void);
void subdiv_loop_edge_vertex_position_test(void);

// Bilinear
void subdiv_bilinear_quad_test(void);
void subdiv_bilinear_quad_l2_test(void);
void subdiv_bilinear_triangle_test(void);
void subdiv_bilinear_mixed_test(void);

// Edge cases
void subdiv_level0_no_change_test(void);
void subdiv_max_level_clamped_test(void);
void subdiv_boundary_interpolation_modes_test(void);
