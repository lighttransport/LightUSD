# Development Memo: LightUSD npm package with Vite

## Overview

Production builds resolve LightUSD from the package in
`node_modules/lightusd`. The Vite development server resolves the local
`web/js/src/lightusd` modules instead so core-backend changes can be tested
before npm publication.

Current package target:

```bash
lightusd@0.9.9-rc1
```

## Setup

```bash
cd web/demo
npm install
npm run dev
```

The `prepare:local-lightusd` script configures and incrementally builds:

* `web/build_ninja` for the local core module (plus the deprecated combined module)

The core distribution target emits its glue and WASM files into `web/js/src/lightusd`.
Ninja does no work when the outputs are current.

The GitHub Pages workflow uses `bun install` followed by
`bash web/site/build-pages.sh`. That production build still resolves
`lightusd` from `node_modules` and defaults to the legacy backend.

## Vite Resolution

`vite.config.js` selects the alias from the Vite command and mode. Development
uses `../js/src/lightusd`; production uses `node_modules/lightusd`, so imports
like:

```js
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
```

come from the selected source. Vite emits the WASM files referenced by the
selected module into the static bundle.

## Updating LightUSD

```bash
cd web/demo
npm install lightusd@<version> --save-exact
npm run build
```

Do not symlink `node_modules/lightusd`; development source selection is handled
by Vite.
