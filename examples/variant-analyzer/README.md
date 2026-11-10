# Variant analyzer

Build `variant-analyzer` with `LIGHTUSD_BUILD_EXAMPLES=ON`. It uses core layer
storage for USDA, USDC and USDZ; no legacy library or render conversion is needed.

```
build_ninja/variant-analyzer --detailed tests/usda/variantSet-002.usda
build_ninja/variant-analyzer --json tests/usda/variantSet-002.usda
```

Counts describe authored sets on the root layer's prims, before composition.
Nesting depth includes nested option sets and sets on option-created child prims.
The combination count multiplies the top-level option counts as independent
choices; it does not enumerate conditional nested choices. Counts above one
million appear as `null` in JSON. An empty layer still produces a JSON report.
