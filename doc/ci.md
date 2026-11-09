# CI / Release Procedure

Version-bump, tagging, and publish procedure for LightUSD.

> Workflow-level detail (the `wheels.yml` / `wasmPublish.yml` pipeline
> internals, build matrix, publish gates, and local verification commands)
> lives in [developer.md](developer.md). This file covers versions, tags, and
> the step order.

## Version sources

| Component | File | How version is set |
|---|---|---|
| C++ library (compile-time constants) | `src/lightusd.hh` (the `version_major/minor/micro/rev` constants near line 51) | Hand-edit |
| Python wheel (`lightusd` on PyPI) | derived from git tag via `setuptools_scm` (config in `pyproject.toml` `[tool.setuptools_scm]`; written to `python/lightusd/_version.py` at build time) | Automatic from tag |
| NPM package (`lightusd` on npmjs.org) | `web/npm/package.json` `"version"` (source-of-truth) and `web/js/package.json` `"version"` | Hand-edit; workflow can override via `release_version` input |

There is no `CHANGELOG.md` / release-notes file in the repo. GitHub Release notes are written on the GitHub UI when cutting the release.

## 1. Pre-release: bump versions

Prepare stable and pre-release versions on `main`, the repository's default and
release branch. Pull requests target `main`, and release tags are cut from an
audited commit on `main`.

### 1a. C++ version constants

Edit `src/lightusd.hh`:

```cpp
constexpr int version_major = 0;
constexpr int version_minor = 9;
constexpr int version_micro = 9;    // bump
constexpr auto version_rev = "rc6"; // e.g. "rc1" for pre-releases, "" for stable
```

These are the only C++ version constants; `CMakeLists.txt` does not hardcode a version.

### 1b. NPM package version

Edit both:

- `web/npm/package.json` — `"version": "x.y.z"`
- `web/js/package.json`  — `"version": "x.y.z"`

These are the source-of-truth for the npm package. The `wasmPublish.yml` workflow can override the published version via its `release_version` workflow input (see step 4); when used, `web/npm/scripts/stage-package.mjs` rewrites the staged manifest at publish time. Keep the checked-in files in sync with the version you actually publish.

### 1c. Python version

Do **not** edit any file. `setuptools_scm` derives the Python wheel version from the `v*.*.*` git tag pushed in step 3. Confirm `pyproject.toml` still has:

```toml
dynamic = ["version"]
[tool.setuptools_scm]
version_file = "python/lightusd/_version.py"
```

### 1d. Commit

```bash
git checkout main
git pull
# edit src/lightusd.hh, web/npm/package.json, web/js/package.json
git add src/lightusd.hh web/npm/package.json web/js/package.json
git commit -m "Bump version to x.y.z"
git push origin main
```

## 2. Sanity checks before tagging

```bash
# Native build + tests
cmake -S . -B build -DLIGHTUSD_BUILD_TESTS=ON
cmake --build build -j16
cd build && ctest --output-on-failure && cd ..

# Roundtrip
bash tests/run-usdcat-compare.sh

# Python wheel (local smoke test; full matrix runs in CI)
python -m pip install --upgrade build
python -m build --sdist
```

## 3. Tag and push — triggers PyPI publish

The PyPI workflow (`.github/workflows/wheels.yml`) is triggered by **any push of a tag matching `v*.*.*`**.

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin vX.Y.Z
```

What this triggers in `wheels.yml`:

1. `build_wheels` matrix — builds CPython abi3 wheels via cibuildwheel for:
   - `ubuntu-latest` × x86_64
   - `ubuntu-24.04-arm` × aarch64
   - `macos-14` × arm64
   - `windows-latest` × AMD64
   - `windows-11-arm` × ARM64
2. `build_sdist` — produces source distribution.
3. `publish` job (only on `push` of `refs/tags/v*`):
   - Uses **PyPI Trusted Publishing (OIDC)** — no API token in repo secrets.
   - The pypi.org project must have a matching publisher registered:
     - owner: `lighttransport`, repo: `LightUSD`, workflow: `wheels.yml`, environment: `pypi`.
   - GitHub environment `pypi` must exist on the repo with `id-token: write` allowed.
   - Emits PEP 740 attestations.

Watch the run at: https://github.com/lighttransport/LightUSD/actions/workflows/wheels.yml

If the publish step fails after wheels build successfully, wheels are kept as workflow artifacts — re-running just the `publish` job is safe (PyPI rejects duplicates).

### Pre-release / RC tags

The `v*.*.*` tag pattern matches pre-release tags too (`v1.0.0-rc4`). The
`publish` job uploads both final and pre-release tags. `setuptools_scm`
normalizes the hyphenated Git tag to PEP 440 form, so `v1.0.0-rc4` is published
to PyPI as `1.0.0rc4`. To publish an RC to both PyPI and npm:

1. Set `version_rev = "rc4"` in `src/lightusd.hh` (cosmetic; C++ side only).
2. Set `"version": "1.0.0-rc4"` in `web/npm/package.json` and `web/js/package.json` (npm/semver uses the hyphenated pre-release form; do not strip the hyphen).
3. Push tag `v1.0.0-rc4` from the audited release commit on `main`.
4. After the trusted-publishing workflow succeeds, install it with `pip install --pre lightusd==1.0.0rc4`.

Use the hyphenated SemVer form (`1.0.0-rc4`) for Git and npm. Do not manually
strip the hyphen for Python; `setuptools_scm` performs the PEP 440 normalization.

## 4. NPM publish — manual workflow dispatch

The npm publish workflow (`.github/workflows/wasmPublish.yml`) is **not** triggered by tags. Run it manually:

GitHub UI: Actions → "Build and publish wasm" → Run workflow.

Or via CLI:

```bash
gh workflow run wasmPublish.yml --ref main \
  -f release_version=1.0.0-rc4 \
  -f npm_tag=next
```

**`--ref main` matters.** `gh workflow run` resolves the workflow definition
(and its accepted inputs) from that ref. Use the audited `main` workflow so the
publish inputs and package sources match the release commit.

Inputs:

- `release_version` — semver, e.g. `0.9.9` or `0.9.9-rc1` (rewrites the staged `package.json`; should match what you committed in 1b).
- `npm_tag` — npm dist-tag, e.g. `latest` for a real release, `preview` / `next` / `rc` for pre-releases. **Default is `preview`** — change it to `latest` for stable releases. Verify after publish: `npm view lightusd dist-tags`.

The job:

1. Sets up Node 24 with `registry-url: https://registry.npmjs.org`.
2. Installs Emscripten 4.0.14 and builds WASM32 + WASM64 (MinSizeRel).
3. `npm run build:stage -- --release-version=<input>` (runs `web/npm/scripts/stage-package.mjs`).
4. `npm run validate` (runs `web/npm/scripts/validate-package.mjs`).
5. `npm publish --provenance --access public --tag <npm_tag>` from `web/npm/dist`.

Publishing requires npm provenance (OIDC, `id-token: write`) — the npmjs `lightusd` package must trust this workflow. No npm token in repo secrets.

The npm staging scripts live in `web/npm/`:

- `build-package.sh` — top-level: ensures Node/emsdk, runs `build-wasm.sh`, then stages + validates.
- `build-wasm.sh` — emcmake/ninja build of WASM32 + WASM64 in MinSizeRel.
- `scripts/stage-package.mjs` — copies wasm/js into `web/npm/dist`, rewrites manifest version.
- `scripts/validate-package.mjs` — `npm pack --dry-run` + structural check on `dist/`.

These are the actual filenames — no numeric prefixes. The `package.json` `scripts` field points at these names directly.

To publish locally instead (not recommended; loses provenance attestation):

```bash
cd web/npm
npm ci
./build-package.sh              # builds wasm + stages dist
npm run build:stage -- --release-version=X.Y.Z
npm run validate
cd dist && npm publish --access public --tag latest
```

## 5. GitHub Release

After the PyPI run is green:

```bash
# Stable release
gh release create vX.Y.Z --title "vX.Y.Z" --notes "…release notes…" --target main

# Pre-release / RC
gh release create vX.Y.Z-rcN --title "vX.Y.Z-rcN" --notes "…" --target main --prerelease
```

Or via the GitHub UI on the tag page. The release-notes body is hand-written (no auto-generated CHANGELOG in the repo). `--target` only matters if the tag does not yet exist on the remote — if you already pushed the tag in step 3, `gh release create` uses the commit the tag points to and `--target` is redundant.

## 6. Post-release

- Bump `version_micro` (or `version_minor`) in `src/lightusd.hh` on `main` to the next pre-release (e.g. `1.0.1` with `version_rev = "dev1"`).
- Optionally bump `web/npm/package.json` + `web/js/package.json` to a `-dev` version to make accidental publishes of intermediate builds easy to spot.

## Summary: files touched per release

```
src/lightusd.hh              # C++ version constants
web/npm/package.json         # npm package version
web/js/package.json          # JS module version (keep in sync with web/npm)
```

Files **not** touched by a version bump (handled automatically):

```
pyproject.toml               # dynamic version via setuptools_scm
python/lightusd/_version.py  # generated at build time
CMakeLists.txt               # no hardcoded version
```

## Workflow reference

| Workflow file | Trigger | Publishes to | Auth |
|---|---|---|---|
| `.github/workflows/wheels.yml` | push tag `v*.*.*` (or `workflow_dispatch`) | PyPI `lightusd` | OIDC trusted publisher (env `pypi`) |
| `.github/workflows/wasmPublish.yml` | `workflow_dispatch` only | npmjs `lightusd` | OIDC provenance |
