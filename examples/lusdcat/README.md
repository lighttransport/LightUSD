# Core USD CLI

`lusdcat` loads, composes and writes USD through the core library. The repository
build defines its canonical target in `src/CMakeLists.txt`; this directory also
supports a small standalone build:

```sh
cmake -S examples/lusdcat -B build_ninja/lusdcat-example -G Ninja
cmake --build build_ninja/lusdcat-example --target lusdcat
```

Use `lusdcat --rewrite-layer scene.usda` for an authored-layer rewrite,
`lusdcat -f scene.usda` for composition, or
`lusdcat --explain /World.radius --time 0.5 --json scene.usda` for the property
opinion trace. The workflow regression checks variants, sampled values and
connection chains through this same core CLI.

The historical implementation is archived under `src/legacy/examples/lusdcat`.
