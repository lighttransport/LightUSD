// Generated from aousd/core/1.0.1/core_spec.md by
// scripts/generate-next-aousd-tables.py. Do not edit by hand.
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "name", "token",
                      Value::MakeToken("custom"))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "redChroma", "float2",
                      Value::MakeFloat2(1.0f, 0.0f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "greenChroma", "float2",
                      Value::MakeFloat2(0.0f, 1.0f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "blueChroma", "float2",
                      Value::MakeFloat2(0.0f, 0.0f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "whitePoint", "float2",
                      Value::MakeFloat2(0.33333333f, 0.33333333f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "gamma", "float", Value(1.0f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceDefinitionAPI", "linearBias", "float", Value(0.0f))
AOUSD_SCHEMA_FALLBACK("ColorSpaceAPI", "colorSpace:name", "token",
                      Value::MakeToken(""))
AOUSD_SCHEMA_FALLBACK("CollectionAPI", "collection:__INSTANCE__:expansionRule",
                      "token", Value::MakeToken("expandPrims"))
AOUSD_SCHEMA_FALLBACK("CollectionAPI", "collection:__INSTANCE__:includeRoot",
                      "bool", Value(false))
AOUSD_SCHEMA_DECLARE("CollectionAPI", "collection:__INSTANCE__:includes",
                     "relationship")
AOUSD_SCHEMA_DECLARE("CollectionAPI", "collection:__INSTANCE__:excludes",
                     "relationship")
AOUSD_SCHEMA_DECLARE("CollectionAPI", "collection:__INSTANCE__",
                     "opaque")
