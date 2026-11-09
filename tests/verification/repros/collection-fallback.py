#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report AOUSD includeRoot fallback and SDK collection membership separately.

Run with OpenUSD Python. AOUSD Core 1.0.1 section includeRoot specifies false;
USD's generated schema can expose None even when membership treats it as false.
This is a value-resolution discrepancy, not proof of a simulation defect.
"""
import json
from pxr import Usd, UsdPhysics

stage=Usd.Stage.CreateInMemory()
group=UsdPhysics.CollisionGroup.Define(stage,'/Group')
collection=group.GetCollidersCollectionAPI()
attribute=collection.GetIncludeRootAttr()
query=collection.ComputeMembershipQuery()
print(json.dumps({'contract':'AOUSD Core 1.0.1 / 1.1 includeRoot fallback false',
                  'resolved':attribute.Get(),
                  'authored':attribute.HasAuthoredValueOpinion(),
                  'root_included':query.IsPathIncluded('/'),
                  'resolution_matches_contract':attribute.Get() is False,
                  'membership_matches_contract':not query.IsPathIncluded('/')},indent=2))
