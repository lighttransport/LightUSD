// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "rb-dynamics.h"
typedef struct {
  TydraPhysVec3 r_a, r_b;        /* vectors from body COM to contact point */
  float effective_mass_n;          /* normal effective mass */
  float effective_mass_t[2];       /* tangent effective masses */
  float bias;                      /* Baumgarte velocity bias */
  TydraPhysVec3 tangent[2];       /* tangent vectors */
} ContactConstraint;
