// SPDX-License-Identifier: Apache-2.0
// Point existing regression suites at an isolated measurement build without
// replacing the application's generated modules. Use with node --loader.
import { pathToFileURL } from 'node:url';
import path from 'node:path';

export async function resolve(specifier, context, nextResolve) {
  const override = /(?:^|\/)lightusd(?:_64)?\.js$/.test(specifier)
    ? process.env.LIGHTUSD_MODULE
    : undefined;
  if (override) {
    return {url: 'lightusd-module-override:' + encodeURIComponent(
      pathToFileURL(path.resolve(override)).href), shortCircuit: true};
  }
  return nextResolve(specifier, context);
}

export async function load(url, context, nextLoad) {
  if (!url.startsWith('lightusd-module-override:')) return nextLoad(url, context);
  const target = decodeURIComponent(url.slice('lightusd-module-override:'.length));
  const wasm = target.replace(/\.js$/, '.wasm');
  // Tests and loaders may supply locateFile or wasmBinary pointing at the
  // application build. Keep the overridden JS and WASM an inseparable pair.
  const source = `import factory from ${JSON.stringify(target + '?actual-core-module')};
    export default function(options = {}) {
      const {wasmBinary: ignoredBinary, locateFile: callerLocate, ...rest} = options;
      return factory({...rest, locateFile(name, prefix) {
        if (name.endsWith('.wasm')) return ${JSON.stringify(wasm)};
        return callerLocate ? callerLocate(name, prefix) : prefix + name;
      }});
    }`;
  return {format: 'module', source, shortCircuit: true};
}
