# Progressive composition

This core example prints the authored layer, then cumulative LIVRPS composition
passes and a final layer. Each pass recomposes from the authored root so earlier
flattening does not change strength ordering. USDA, USDC and USDZ inputs use the
core loader and resolver; no legacy library is required.

```sh
./build_ninja/progressive_composition scene.usda
./build_ninja/progressive_composition --composition=references,payload scene.usda
./build_ninja/progressive_composition --noflatten --extract-variants scene.usdc
./build_ninja/progressive_composition --loadOnly scene.usdz
```

Selective composition preserves disabled arc opinions and their list-edit
qualifiers. `--relative` prints mesh diagnostic paths without their leading
slash. `--extract-variants` reports root-layer variant set and option names as
JSON before the layer output. Composition failures return a nonzero status.
