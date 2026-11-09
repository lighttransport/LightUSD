// Lexical dependency discovery. Assignment and bracket scopes distinguish
// composition arcs from asset-valued properties without a look-behind window.
export function scanUSDAssetReferences(source, { locations = false } = {}) {
  const text = String(source || ''), references = [], scopes = [];
  let kind = 'asset', identifier = '';
  for (let i = 0; i < text.length;) {
    const c = text[i];
    if (c === '#') { while (i < text.length && text[i] !== '\n') i++; continue; }
    if (c === '"' || c === "'") {
      const quote = text.startsWith(c.repeat(3), i) ? c.repeat(3) : c;
      i += quote.length;
      while (i < text.length && !text.startsWith(quote, i)) { if (text[i] === '\\') i++; i++; }
      i += quote.length;
      continue;
    }
    if (c === '@') {
      const delimiter = text.startsWith('@@@', i) ? '@@@' : '@';
      const start = i + delimiter.length;
      const end = text.indexOf(delimiter, start);
      if (end < 0) break;
      const path = text.slice(start, end).trim();
      if (path) references.push({ path, kind, ...(locations ? { start, end } : {}) });
      i = end + delimiter.length; continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      const start = i++;
      while (i < text.length && /[\w:]/.test(text[i])) i++;
      identifier = text.slice(start, i); continue;
    }
    if (c === '=') kind = ['references', 'payload', 'subLayers'].includes(identifier) ? identifier.toLowerCase() : 'asset';
    else if ('[({'.includes(c)) { scopes.push(kind); if (c === '{') kind = 'asset'; }
    else if ('])}'.includes(c)) kind = scopes.pop() || 'asset';
    i++;
  }
  return references;
}
