# mimalloc (vendored)

Upstream: https://github.com/microsoft/mimalloc, tag `v3.6.0` (commit
b18ab4eef383f655e760881b02eff8af02800879). MIT license (`LICENSE`).

Vendored sources: `include/` and `src/`. The Windows and macOS platform
layers (`src/prim/windows`, `src/prim/osx`) are dropped: the next CLI tools
link an allocator only on Linux/Unix.

Local patch: `src/util/options.c` takes the `purge_delay` default from a
`MI_DEFAULT_PURGE_DELAY` macro (upstream hard-codes 1000 ms). The next CLI
build sets it to 0: freed memory goes back to the OS immediately, which on
the full-LOD benchmark scene flatten matches jemalloc's speed at a lower peak RSS
(the default delay is ~9% faster but uses ~18% more RSS). The
`MIMALLOC_PURGE_DELAY` environment variable still overrides it.

The whole allocator builds as one translation unit, `src/static.c`. With
`MI_MALLOC_OVERRIDE` it also defines malloc/free/new/delete, replacing the C
library allocator in the executable it is linked into. It is selected with
`-DLIGHTUSD_NEXT_ALLOCATOR=mimalloc` (src/next/CMakeLists.txt). The
allocator evaluation is in doc/large-scene.md.
