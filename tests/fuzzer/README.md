## Requirements

clang with fuzzer support(`-fsanitize=fuzzer`. at least clang 8.0 should work)

## Status

Currently no open issue found by fuzzer.
(except for third party library, especially stb_image)

## Setup

### Ubuntu 18.04

```
$ sudo apt install clang++-8
$ sudo apt install libfuzzer-8-dev
```

Optionally, if you didn't set `update-alternatives` you can set `clang++` to point to `clang++8`(if your clang version is less than 8.0)

```
$ sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-8 10
$ sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-8 10
```

## How to compile

```
$ CXX=clang++ CC=clang meson build -Db_sanitize=address
$ cd build
$ ninja
```


## How to run

Set input size and run fuzz main.

```
$ ./fuzz_lightusd -max_len=128m
```

for fuzzing `fuzz_intcoding_decompress`, capping max memory is required(otherwise oom happens).
(Currently `fuzz_intcoding_decompress` does HARD limit of compressed data up to 2GB)

Use `-rss_limit_mb=8192`(or more if you encounter oom and have enough memory) to limit memory usage.


```
$ ./fuzz_intcoding_decompress -rss_limit_mb=8192 -jobs 4
```

## src/next harnesses

The `next` module has four of its own libFuzzer harnesses -- `next_usdc`
(crate reader), `next_usda` (ASCII parser), `next_compose` (composition), and
`next_roundtrip` (USDA/USDC/USDZ writer and reader round trips).
They are built from `src/next`, not from the top-level project, and are off by
default because they require clang:

```
$ cmake -S src/next -B build-fuzz -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build build-fuzz -j
```

The native minijson parser/serializer has a dedicated `fuzz_minijson` target.
It applies bounded input, nesting, string, array, and object limits, then
exercises the parse-to-serialize boundary:

```sh
cmake -S src/next -B build-fuzz -G Ninja \
  -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_minijson -j
build-fuzz/fuzz_minijson -runs=1000 tests/fuzzer/minijson-seeds
```

For containerized runs where LeakSanitizer is unavailable, use
`LSAN_OPTIONS=detect_leaks=0`; sanitizer-enabled CI should keep leak checking
enabled.

The library itself is compiled with `-fsanitize=fuzzer-no-link,address,
undefined`, so coverage-guided exploration reaches the reader and not just the
harness body.

### Seeding next_usdc

`next_usdc_fuzzmain.cc` re-prepends the 8-byte `PXR-USDC` magic, so seeds must
have it **stripped** -- otherwise every input carries a doubled magic and the
parser rejects it immediately:

```
$ mkdir -p /tmp/usdc-corpus
$ python3 -c "
import glob, os
for f in glob.glob('tests/usdc/*.usdc') + glob.glob('models/*.usdc'):
    d = open(f,'rb').read()
    if d[:8] == b'PXR-USDC':
        open('/tmp/usdc-corpus/'+os.path.basename(f)+'.seed','wb').write(d[8:])
"
$ ./build-fuzz/fuzz_next_usdc -jobs=8 -workers=8 -max_total_time=1500 \
    -max_len=131072 -artifact_prefix=/tmp/artifacts/ /tmp/usdc-corpus
```

Check a corpus without fuzzing (fast, useful in CI) with `-runs=0`.

`scripts/run-next-checks.sh RUN_FUZZ=1` does exactly that: it builds the four
harnesses and replays the seed corpus through each, which keeps them compiling
and the seeds clean without a long fuzzing session.

`next_roundtrip` consumes the USDA seed corpus directly. Each parseable seed is
serialized to USDA, USDC, and USDZ, then the generated outputs are read back;
expected parse or writer rejection is ignored, while crashes and sanitizer
failures are reported by libFuzzer.

## PoC and regressesions

PoC/regression dataset is managed in separate git repo. https://github.com/lighttransport/usd-fuzz
