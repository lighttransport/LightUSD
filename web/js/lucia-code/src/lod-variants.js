import { LuciaError, validIdentifier, validPrimPath } from './utils.js';
export function authorLODVariants(source, parent, paths, block, variantSet = 'luciaLOD') {
  if (!validPrimPath(parent) || parent === '/' || !validIdentifier(variantSet) || !Array.isArray(paths) || paths.length < 2 || paths.length > 16 || new Set(paths).size !== paths.length || paths.some(path => !validPrimPath(path) || path.slice(0, path.lastIndexOf('/')) !== parent))
    throw new LuciaError('LUCIA_LOD_VARIANT', 'LOD variants require 2–16 unique sibling prims inside a named parent.');
  const body = source.slice(block.start, block.end);
  if (new RegExp(`\\bvariantSet\\s+"${variantSet}"`).test(body)) throw new LuciaError('LUCIA_LOD_VARIANT', 'LOD variant set already exists.');
  let declaration = source.slice(block.declarationStart, block.declarationEnd);
  const sets = /((?:(?:prepend|append|add|delete|reorder)\s+)?variantSets\s*=\s*)("[^"]*"|\[[^\]]*\]|None)/g;
  let match, registered = false;
  declaration = declaration.replace(sets, (assignment, prefix, values) => {
    if (/^(delete|reorder)\s/.test(prefix)) return assignment;
    const names = [...values.matchAll(/"([^"]*)"/g)].map(value => value[1]);
    if (!names.includes(variantSet)) names.push(variantSet);
    registered = true; return `${prefix}[${names.map(name => JSON.stringify(name)).join(', ')}]`;
  });
  const selection = /variants\s*=\s*\{([^}]*)\}/;
  if (selection.test(declaration)) {
    declaration = declaration.replace(selection, (assignment, entries) => {
      if (new RegExp(`\\b${variantSet}\\b`).test(entries)) throw new LuciaError('LUCIA_LOD_VARIANT', 'LOD selection already exists.');
      return `variants = {${entries}\n string ${variantSet} = "LOD0"\n}`;
    });
  } else match = `variants = { string ${variantSet} = "LOD0" }`;
  const metadata = `${registered ? '' : `prepend variantSets = "${variantSet}"\n`}${match || ''}`;
  if (metadata.trim()) {
    const last = declaration.lastIndexOf(')');
    declaration = last >= 0 ? declaration.slice(0, last) + `\n${metadata}\n` + declaration.slice(last) : `${declaration} (\n${metadata}\n)`;
  }
  const variants = paths.map((selected, index) => `"LOD${index}" {\n${paths.map(path => `over "${path.split('/').at(-1)}" { token visibility = "${path === selected ? 'inherited' : 'invisible'}" }`).join('\n')}\n}`).join('\n');
  return source.slice(0, block.declarationStart) + declaration + source.slice(block.declarationEnd, block.end) +
    `\nvariantSet "${variantSet}" = {\n${variants}\n}\n` + source.slice(block.end);
}
