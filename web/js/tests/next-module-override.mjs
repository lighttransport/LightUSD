// SPDX-License-Identifier: Apache-2.0
// Point existing regression suites at an isolated measurement build without
// replacing the application's generated modules. Use with node --loader.
import { pathToFileURL } from 'node:url';
import path from 'node:path';

export async function resolve(specifier, context, nextResolve) {
  const override = process.env.LIGHTUSD_NEXT_MODULE;
  if (override && /(?:^|\/)lightusd_next(?:_64)?\.js$/.test(specifier)) {
    return {url: 'lightusd-next-override:' + encodeURIComponent(
      pathToFileURL(path.resolve(override)).href), shortCircuit: true};
  }
  return nextResolve(specifier, context);
}

export async function load(url, context, nextLoad) {
  if (!url.startsWith('lightusd-next-override:')) return nextLoad(url, context);
  const target = decodeURIComponent(url.slice('lightusd-next-override:'.length));
  const wasm = target.replace(/\.js$/, '.wasm');
  // Tests and loaders may supply locateFile or wasmBinary pointing at the
  // application build. Keep the overridden JS and WASM an inseparable pair.
  const source = `import factory from ${JSON.stringify(target + '?actual-next-module')};
    import fs from 'node:fs';
    export default function(options = {}) {
      return factory({...options, wasmBinary: fs.readFileSync(new URL(${JSON.stringify(wasm)}))});
    }`;
  return {format: 'module', source, shortCircuit: true};
}
