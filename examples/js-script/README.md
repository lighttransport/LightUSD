# Core JavaScript metadata example

Configure with `-DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_WITH_QJS=ON`, then build
`js-script`. The example embeds the vendored QuickJS runtime and loads USDA,
USDC or USDZ with the core library. It does not link the legacy library.

```
build_ninja/js-script scene.usda examples/js-script/query_layer.js
```

Scripts can call `getLayerMetas()` and `console.log()`. Metadata describes the
authored root layer, including sublayer paths/offsets and root prim names;
external layers are not composed for this metadata query. An unauthored end time
appears as `null`. USD unit defaults follow the core layer metadata API.

The host limits scripts to 16 MiB, QuickJS memory to 64 MiB, and execution
(including pending Promise jobs) to five seconds. Script errors and exceeded
limits return nonzero. The runtime exposes no filesystem or network APIs.
