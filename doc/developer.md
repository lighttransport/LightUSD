# Developer Guide: Build & Publish Workflows

How the Python (PyPI) and npm (WASM/JS) packages are built and published, and
how to cut a release end-to-end. The git tag/branch rules and version-bump
step-by-step live in [`ci.md`](ci.md) — read it first; this document focuses on
the two publish pipelines and how to verify them locally.

## 1. The two publish channels

| Channel | Artifact | Trigger | Publish gate |
|---|---|---|---|
| **PyPI** (`lightusd` wheel) | CPython abi3 + free-threaded wheels, sdist | Tag push `v*.*.*` → `.github/workflows/wheels.yml` | Final tags only (no `-` suffix) |
| **npm** (`lightusd` package) | WASM/JS loaders (4 variants) | Manual `workflow_dispatch` on `.github/workflows/wasmPublish.yml` | Final versions only (no `-` suffix) |

Both channels **build and upload artifacts for pre-releases** but **skip the
registry publish** for any hyphenated (pre-release) version — by design, so an
RC never lands on PyPI/npm by accident. See [§4](#4-pre-release-rc-behavior)
for how to distribute pre-releases.

## 2. Version sources

| Component | File | How version is set |
|---|---|---|
| C++ library (compile-time constants) | `src/lightusd.hh` (`version_major/minor/micro/rev` near line 51) | Hand-edit |
| Python wheel | git tag via `setuptools_scm` (`[tool.setuptools_scm]` in `pyproject.toml`; writes `python/lightusd/_version.py` at build time) | **Automatic from tag — never hand-edit** |
| npm package (published manifest) | `web/npm/package.json` `"version"` | Hand-edit as baseline; the workflow's `release_version` input overrides it at stage time |
| npm demo app (NOT published) | `web/js/package.json` `"version"` | Hand-edit for parity only — the published package is `web/npm/` |

Notes:

- The npm workflow always stamps the published version from its
  `release_version` input (`web/npm/scripts/stage-package.mjs` rewrites the
  staged manifest; the checked-in `web/npm/package.json` is the fallback
  baseline when the input matches it).
- Keep `src/lightusd.hh`, `web/npm/package.json`, and `web/js/package.json`
  mutually consistent with the version you are cutting. As of the 1.0.0 bump
  they are in sync at 1.0.0.

## 3. PyPI: wheel build + publish (`wheels.yml`)

### 3.1 Triggers

- `push` of a tag matching `v*.*.*` — builds **and** publishes (if final).
- `workflow_dispatch` with an optional `sha` input — **build only**, no publish
  (the `publish` job requires a tag push). Use this to pre-build a commit
  before tagging.

### 3.2 Build matrix (cibuildwheel v3.2, config in `pyproject.toml`)

| Platform | Arch |
|---|---|
| ubuntu-latest | x86_64 |
| ubuntu-24.04-arm | aarch64 |
| macos-14 | arm64 |
| windows-latest | AMD64 |
| windows-11-arm | ARM64 |

Per platform two wheels are produced (`[tool.cibuildwheel]`):

- `cp310-abi3` — limited API (`Py_LIMITED_API=0x030A0000`), covers CPython ≥ 3.10
- `cp314t` — free-threaded CPython 3.14 (`Py_mod_gil` declared; tests run with
  `PYTHON_GIL=0` so a module that silently re-enables the GIL fails loudly)

Skipped: PyPy, musllinux/windows i686, win32. Linux uses manylinux_2_28 images.

### 3.3 Publish job

- Runs only when: `github.event_name == 'push'` **and** tag starts with `v`
  **and** the tag contains **no `-`** (i.e. final `vX.Y.Z` only).
- Uses **PyPI Trusted Publishing (OIDC)** — no API token. Prerequisite: the
  `pypi` environment must exist on GitHub and PyPI must have the trusted
  publisher registered (`owner: lighttransport`, `repo: lightusd`,
  `workflow: wheels.yml`, `env: pypi`).
- Emits PEP 740 attestations (`attestations: true`).
- Pre-release tags: wheels + sdist still build and land in the run's
  **artifacts** — download and upload manually if you must distribute an RC.

## 4. npm: WASM/JS build + publish (`wasmPublish.yml`)

### 4.1 Manual dispatch inputs

| Input | Required | Default | Meaning |
|---|---|---|---|
| `release_version` | yes | — | Semver of the published package (e.g. `1.0.0` or `1.0.0-rc1`); validated by `stage-package.mjs` (`SEMVER_RE`) |
| `npm_tag` | yes | `preview` | npm dist-tag (`latest` for stable, `preview` for RCs) |

Run it from the GitHub UI (Actions → "Build and publish wasm" → Run workflow),
or via CLI — **`--ref` matters**:

```bash
# RCs are cut from dev; stable from release. The workflow file (and its
# inputs) is resolved from the ref you pass.
gh workflow run wasmPublish.yml --ref dev \
  -f release_version=1.0.0-rc1 -f npm_tag=preview
```

Using a ref whose `wasmPublish.yml` copy lacks the inputs you set fails with
`HTTP 422: Unexpected inputs provided`.

### 4.2 What the workflow builds

Four WASM variants (Emscripten SDK 4.0.9), then packages all of them:

| Variant | CMake dir | Flags |
|---|---|---|
| WASM32 legacy | `web/cmake-build` | `-DLIGHTUSD_WASM_PRODUCT=legacy` |
| WASM64 legacy | `web/cmake-build64` | `-DLIGHTUSD_WASM_PRODUCT=legacy -DLIGHTUSD_WASM64=1` |
| WASM32 core | `web/cmake-build-core` | `-DLIGHTUSD_WASM_PRODUCT=core` |
| WASM64 core | `web/cmake-build-core64` | `-DLIGHTUSD_WASM_PRODUCT=core -DLIGHTUSD_WASM64=1` |

Gates before publish:

1. **Smoke test**: `web/js/tests/usdzconvert-core.test.mjs` on both 32- and
   64-bit (a core-pipeline regression blocks publish).
2. **Staging**: `npm run build:stage -- --release-version=<input>` — copies the
   8 required files (`lightusd*.js`/`*.wasm`/`*_64*`/`*_next*`, zstd-compressed
   variants) into `web/npm/dist`, rewrites `../` imports, and stamps the
   manifest version.
3. **Validation**: `npm run validate` — checks staged files + publishable
   contents, `--pack-only` does a `npm pack` dry run.

### 4.3 Publish step

- Skips when `release_version` contains `-` (pre-release).
- `npm publish --provenance --access public --tag <npm_tag>` — provenance
  attestation requires the `id-token: write` permission already granted to the
  job; npm must be configured for OIDC provenance on the `lighttransport` org.
- Artifacts are always uploaded (`lightusd-npm-<release_version>`), including
  for pre-releases.

## 5. Local verification before cutting a release

### 5.1 Python

```bash
# Editable install (builds into build_py_ext/; re-run after touching
# src/python/py-*.c or any header it includes transitively):
pip install -e . --no-build-isolation
cd python && python3 -m pytest tests/ -q

# Wheel build sanity (same backend the CI uses):
python -m pip install --upgrade build
python -m build --sdist --wheel
# Verify the wheel version matches the intended tag (setuptools_scm):
unzip -p dist/lightusd-*.whl lightusd/_version.py | head
```

### 5.2 npm

```bash
cd web/npm
npm ci
# Stage with the intended version (no WASM rebuild unless you changed web/):
npm run build:stage -- --release-version=1.0.0
npm run validate
npm run pack:dry-run      # npm pack dry run
# Full local WASM rebuild (needs Node 24 + emsdk; see build-wasm.sh):
npm run build:wasm
```

## 6. End-to-end release checklist

1. Bump versions per [`ci.md`](ci.md) §1 (`src/lightusd.hh`,
   `web/npm/package.json`, `web/js/package.json`); commit on the right branch
   (`release` for stable, `dev` for RCs).
2. Run the full regression gate (AGENTS.md pre-merge checklist).
3. Local verification per §5 above.
4. Tag and push the tag:

   ```bash
   git tag -a vX.Y.Z -m "Release vX.Y.Z"
   git push origin vX.Y.Z        # PyPI wheels.yml triggers here
   ```

5. Watch `wheels.yml`; confirm the `publish` job uploaded final or RC artifacts
   to PyPI. RC tags such as `v1.0.0-rc4` are normalized to `1.0.0rc4`.
6. Trigger `wasmPublish.yml` manually (UI or `gh workflow run`, §4.1) with the
   matching `release_version` and `npm_tag` (`latest` for stable).
7. Confirm on PyPI (https://pypi.org/p/lightusd) and npm
   (https://www.npmjs.com/package/lightusd, dist-tags).
8. Write GitHub Release notes on the UI.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `wheels.yml` built but nothing on PyPI | Confirm the run was triggered by a pushed `v*.*.*` tag and that the PyPI trusted-publisher environment approved the publish job |
| `HTTP 422: Unexpected inputs provided` | `gh workflow run` resolved `wasmPublish.yml` from the wrong ref (e.g. `release` lacks the inputs); pass `--ref dev` |
| Wheel version wrong | `setuptools_scm` reads the git tag — push the tag first, use `fetch-depth: 0` locally (`git fetch --tags`), never hand-edit `python/lightusd/_version.py` |
| npm publish needs auth/provenance | OIDC provenance must be configured for the org/package; locally you cannot publish from a dev machine without an npm token (not supported by this workflow) |
| WASM smoke test fails | A `core` pipeline regression — fix before publishing; the gate intentionally blocks `wasmPublish.yml` |
