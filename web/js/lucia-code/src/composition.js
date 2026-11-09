import { LuciaError, encoder } from './utils.js';

export async function composeProject(source, assets, { resolveAsset = null, signal = null, module = null } = {}) {
  module ||= await import('../../src/lightusd/lightusd_next.js').then(({ default: factory }) => factory());
  const session = new module.NextFlattenSession(), supplied = new Set();
  const checked = value => { if (!value.success) throw new LuciaError('LUCIA_COMPOSITION', value.error || 'Composition failed.'); return value; };
  try {
    checked(session.setMaxInputBytes(256 * 1024 * 1024));
    checked(session.setMaxOutputBytes(256 * 1024 * 1024));
    checked(session.begin(encoder.encode(source), '/lucia/scene.usda', true));
    for (let step = 0; step < 10000; step++) {
      if (signal?.aborted) throw new LuciaError('LUCIA_CANCELLED', 'Composition was cancelled.');
      const result = checked(session.step(null));
      if (result.status === 'done') {
        if (result.compositionErrors?.length) throw new LuciaError('LUCIA_COMPOSITION', result.compositionErrors.join('\n'));
        const document = new module.LayerDocument();
        try { checked(document.load(result.data)); return checked(document.exportUSDA()).text; }
        finally { document.delete(); }
      }
      if (result.status === 'need-layer') {
        if (supplied.has(result.key)) throw new LuciaError('LUCIA_COMPOSITION', `Unresolved dependency: ${result.key}`);
        let path = result.key.replace(/^\/lucia\//, '').replace(/^\.\//, '');
        const safe = !path.startsWith('/') && !path.split('/').includes('..') && !/[\\\u0000-\u001f]/.test(path);
        let bytes = safe ? assets.get(path)?.bytes : null;
        if (!bytes && resolveAsset) bytes = await resolveAsset(result.key, { signal });
        if (!(bytes instanceof Uint8Array)) throw new LuciaError('LUCIA_COMPOSITION_ASSET', `Dependency requires loaded safe package-relative data or a host resolver: ${result.key}`);
        checked(session.provideLayer(result.key, bytes)); supplied.add(result.key);
      }
      // Give cancellation and UI events a turn between dependency steps.
      await new Promise(resolve => setTimeout(resolve, 0));
    }
    throw new LuciaError('LUCIA_COMPOSITION_LIMIT', 'Composition exceeded the dependency-step limit.');
  } finally { session.delete(); }
}
