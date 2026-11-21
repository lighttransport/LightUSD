#pragma once

// Half-precision float print->parse roundtrip tests
// Verifies that dtos(half) produces strings that parse back to the same
// binary half value for all 65536 possible half-precision values.

void half_roundtrip_exhaustive_test(void);
void half_roundtrip_edge_cases_test(void);
void half_shortest_representation_test(void);
