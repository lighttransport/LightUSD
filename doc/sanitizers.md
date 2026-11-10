# Sanitizer Builds: AddressSanitizer and ThreadSanitizer

Build and run instructions for the ASan and TSan configurations, the known
i386 ASan limit for `lusdcat`, and the environment traps that make TSan fail
*before your code even runs* — both produce cryptic startup errors that look
like bugs in lightusd but are not.

## AddressSanitizer (ASan)

### Configure ASan

Use `SANITIZE_ADDRESS` when configuring CMake:

```sh
cmake -S . -B build_asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSANITIZE_ADDRESS=1 \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_BUILD_EXAMPLES=ON
cmake --build build_asan
ctest --test-dir build_asan --output-on-failure
```

For the 32-bit Linux diagnostic build used to reproduce the `lusdcat` issue:

```sh
cmake -S . -B build_clang21_m32_asan -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/linux_i386.toolchain.cmake \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSANITIZE_ADDRESS=1 \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_BUILD_EXAMPLES=ON
cmake --build build_clang21_m32_asan --target lusdcat unit-test-lightusd
```

### Known i386 `lusdcat` Limit

`lusdcat` is too large for the i386 ASan runtime/allocator address-space layout.
With clang-21, a 32-bit ASan build of the full `lusdcat` binary aborts at
startup or first output, before it can do useful validation work:

```text
ERROR: AddressSanitizer: out of memory: failed to allocate 0x110000
bytes of InternalMmapVector
ERROR: Failed to mmap
```

This is not a single large LightUSD allocation, and it is not specific to GCC
libasan. The same failure reproduces with clang-21/compiler-rt ASan. Debugging
showed the primary allocation failure was a normal 4 KiB libc stdout buffer
allocation reached from `print_help()`. The larger `InternalMmapVector` failure
is a secondary failure while ASan is trying to report the original allocator
OOM.

The failure is caused by the ASan runtime needing large shadow, metadata,
quarantine, fake-stack, and allocator regions inside the small 32-bit virtual
address space, while the full `lusdcat` executable also carries a large text,
data, and bss footprint. In the reproduced clang-21 build, the binary was an
ELF32 executable with roughly:

| Section | Size |
| --- | ---: |
| `.text` | 43 MB |
| `.data` | 474 KB |
| `.bss` | 9 MB |
| total | 53 MB |

ASan startup mapped large low/high shadow regions, then normal allocator mmap
requests failed with `ENOMEM`.

### Mitigations Tested

These options did not make full i386 `lusdcat` ASan validation reliable:

- `ASAN_OPTIONS=quarantine_size_mb=0`
- `ASAN_OPTIONS=detect_leaks=0`
- `ASAN_OPTIONS=malloc_context_size=5`
- `ASAN_OPTIONS=detect_stack_use_after_return=0`
- disabling ASan global instrumentation with `-mllvm -asan-globals=0`
- building non-PIE with `-fno-pie` and `-no-pie`
- `setarch i386` / `linux32` address-layout variants

`ASAN_OPTIONS=allocator_may_return_null=1` can sometimes let trivial `--help`
output continue, but it only converts the ASan allocator abort into null-return
behavior. It does not make real `lusdcat` parse or validation paths reliable and
should not be used as a CI fix.

### Repo Policy

The supported coverage split is:

- Use full ASan `lusdcat` validation on 64-bit builds.
- Use 32-bit non-ASan builds for i386 tool coverage.
- Use 32-bit ASan builds for smaller tests such as `unit-test-lightusd` and
  parser-focused executables.

`CMakeLists.txt` skips the `lusdcat` end-to-end validation tests when both
conditions are true:

- `CMAKE_SIZEOF_VOID_P EQUAL 4`
- `SANITIZE_ADDRESS` is enabled

The validation logic is still exercised on non-ASan 32-bit builds and on 64-bit
ASan builds. If i386 ASan validation coverage is required, add a smaller
dedicated validation executable that links only the parser and validation code
needed for the target fixture set, instead of using full `lusdcat`.

Possible size-reduction flags such as `-O1`,
`-fsanitize-address-use-after-return=never`, or
`-fsanitize-address-outline-instrumentation` may be useful for diagnostics, but
they should be treated as experimental until the resulting binary is verified
with the actual i386 ASan validation workload.

## ThreadSanitizer (TSan)

### Build

```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS=-fsanitize=thread \
      -DCMAKE_C_FLAGS=-fsanitize=thread \
      -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
      -DLIGHTUSD_ENABLE_THREAD=ON \
      -DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_WITH_TYDRA=ON
cmake --build build_tsan -j
```

Notes:

- `LIGHTUSD_ENABLE_THREAD=ON` is required — it enables the parallel code
  paths TSan is supposed to check (parallel USDC ReconstructStage, parallel
  stage→specs crate conversion, parallel LayerToStage, parallel tydra mesh /
  animation conversion).
- Do NOT combine with `LIGHTUSD_WITH_TCMALLOC` (jemalloc/tcmalloc link):
  sanitizers must own the allocator.
- `src/imageproc/simd.cc` self-disables its `target_clones`
  multi-versioning under TSan — ifunc resolvers run before the TSan runtime
  is initialized and crash otherwise. Nothing to do, just don't "fix" the
  missing MV dispatch under TSan.
- The `SpinMutex` used for diagnostics merging calls `sched_yield()` under
  TSan (see `src/spin-mutex.hh`); without it, contended spins livelock for
  minutes under the TSan scheduler.

### Trap 1: kernel ASLR entropy (`vm.mmap_rnd_bits`)

**Symptom** — every TSan binary, even a trivial one, dies at startup:

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x5a77a7a67000-...
```

or

```
==PID==ERROR: ThreadSanitizer: out of memory: allocator is trying to allocate 0x7b bytes
==PID==ERROR: ThreadSanitizer: out of memory: failed to allocate 0x1000 (4096) bytes of InternalMmapVector (error code: 12)
ERROR: Failed to mmap
```

**Cause** — Ubuntu kernels ≥ 6.5 (observed on 6.8.0) default to
`vm.mmap_rnd_bits = 32`. The TSan runtime's fixed shadow-memory layout only
tolerates up to 28 bits of mmap randomization; with 32 bits, PIE images and
mappings land outside the ranges the runtime expects.

**Fix** (root required):

```bash
sudo sysctl vm.mmap_rnd_bits=28
# persist:
echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/99-tsan.conf
```

**Verify** with a trivial program before blaming the real target:

```bash
printf '#include <thread>\nint x; int main(){ std::thread t([]{x=1;}); t.join(); return x; }\n' > /tmp/t.cc
g++ -fsanitize=thread /tmp/t.cc -o /tmp/t && /tmp/t; echo "rc=$? (1 = OK, it returns x)"
```

`setarch $(uname -m) -R <cmd>` (disable ASLR for one run) is a per-run
workaround, but on some kernel/toolchain combinations it is not sufficient —
prefer the sysctl.

### Trap 2: `RLIMIT_AS` caps (self-imposed or inherited)

**Symptom** — the trivial program works, but a specific tool (historically
`lusdcat`) still dies with the same
`out of memory: failed to allocate ... InternalMmapVector` error, while other
binaries from the same build (e.g. `tydra_to_renderscene`) start fine.

**Cause** — TSan reserves tens of **terabytes** of virtual address space at
startup (shadow memory, internal allocators — reservations, not committed
RAM). Any `RLIMIT_AS` cap kills it. `lusdcat` sets a 32 GB `RLIMIT_AS` in
`main()` as an OOM/thrash guard; that guard is now compiled out under
`__SANITIZE_THREAD__` / `__SANITIZE_ADDRESS__` (see
`src/legacy/examples/lusdcat/main.cc`). If you add an address-space cap to another tool,
gate it the same way. The same applies to caps inherited from the
environment (`ulimit -v`, container limits).

### Running

```bash
# large scenes are slow under TSan (10-20x); start with mid-size models
./build_tsan/lusdcat -f model.usdz -o /tmp/out.usdc
echo $?   # 66 = TSan reported an error; grep the output for "WARNING: ThreadSanitizer"
```

- TSan exit code 66 means a race (or runtime error) was reported even if the
  tool otherwise completed.
- Byte-compare the output against a non-TSan build's output — a data race
  that corrupts data often shows up as a byte diff or run-to-run
  nondeterminism even when the race report itself is unclear.
- Complement TSan with a determinism check on the normal thread build
  (10 runs, compare hashes) — it has caught reordering bugs TSan cannot see
  (TSan checks synchronization, not output ordering).

### History

- 2026-06: parallel ReconstructStage TSan pass found real bugs (shared
  `GetPrimMeta` static clobber, crate-reader cursor race — see git log
  9550a9fc2).
- 2026-07: parallel USDC crate-reader fieldset race fixed (TSan-confirmed,
  dropped whole subtrees — "Parallel-parse data-loss race" in project notes).
- 2026-07-06: both traps above hit in one session (kernel had
  `mmap_rnd_bits=32`; lusdcat's own 32 GB `RLIMIT_AS` masked as a TSan OOM).
