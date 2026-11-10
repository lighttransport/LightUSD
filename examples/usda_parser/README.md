# USDA reader example

`usda_parser` uses the core USDA reader and exports the authored layer as USDA.
Build it with `LIGHTUSD_BUILD_EXAMPLES=ON`, or enable only this program with
`LIGHTUSD_WITH_TOOL_USDA_PARSER=ON`. It requires C++17 and no legacy library.

```sh
./build_ninja/usda_parser scene.usda
./build_ninja/usda_parser --flatten scene.usda
./build_ninja/usda_parser --flatten --composition=references,payload scene.usda
```

Composition accepts all six LIVRPS arc types (short names `l,i,v,r,p,s`). Disabled
arcs and their list edits remain authored in selective output. Reader and
composition diagnostics go to stderr; successful stdout is a reloadable USDA
layer. Invalid arguments, wrong-format input and failed composition return a
nonzero status. CLI handling is shared with `usdc_parser` in `examples/parser/`.
