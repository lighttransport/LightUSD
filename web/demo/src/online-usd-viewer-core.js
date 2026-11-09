export const MAX_ASSET_BYTES = 200 * 1024 * 1024;

export function githubContentsRequest(url) {
  let parsed;
  try { parsed = new URL(url); } catch { return null; }
  const parts = parsed.pathname.split('/').filter(Boolean);
  let owner = '', repo = '', ref = '', fileParts = [];
  if (parsed.hostname === 'raw.githubusercontent.com' && parts.length >= 4) {
    [owner, repo, ref] = parts; fileParts = parts.slice(3);
  } else if (parsed.hostname === 'github.com' && parts.length >= 5 && (parts[2] === 'blob' || parts[2] === 'raw')) {
    [owner, repo] = parts; ref = parts[3]; fileParts = parts.slice(4);
  } else return null;
  const path = fileParts.map((part) => encodeURIComponent(decodeURIComponent(part))).join('/');
  return {
    url: `https://api.github.com/repos/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}/contents/${path}?ref=${encodeURIComponent(ref)}`,
    headers: { Accept: 'application/vnd.github.raw+json', 'X-GitHub-Api-Version': '2022-11-28' }
  };
}

export function normalizeAssetKey(value) {
  return String(value || '').replaceAll('\\', '/').replace(/^\.\//, '').replace(/^\/+/, '');
}

export function formatBytes(bytes) {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${bytes} B`;
}

export function chooseRootFile(files) {
  const usd = files.filter((file) => /\.(usd|usda|usdc|usdz)$/i.test(file.name || file.webkitRelativePath || ''));
  if (!usd.length) throw new Error('No USD, USDA, USDC, or USDZ file was found.');
  const score = (file) => {
    const key = normalizeAssetKey(file.webkitRelativePath || file.name);
    const name = key.split('/').pop().toLowerCase();
    if (/^root\.(usd|usda|usdc|usdz)$/.test(name)) return 0;
    if (/^default\.(usd|usda|usdc|usdz)$/.test(name)) return 1;
    if (/^(scene|main)\.(usd|usda|usdc|usdz)$/.test(name)) return 2;
    return key.includes('/') ? 4 : 3;
  };
  return [...usd].sort((a, b) => score(a) - score(b) || String(a.name).localeCompare(String(b.name)))[0];
}

export class AssetBudget {
  constructor(maxBytes = MAX_ASSET_BYTES) { this.maxBytes = maxBytes; this.used = 0; this.records = new Map(); }
  claim(key, bytes) {
    const canonical = String(key || ''); if (this.records.has(canonical)) return false;
    const amount = Number(bytes) || 0;
    if (this.used + amount > this.maxBytes) throw new Error(`Asset budget exceeded (${formatBytes(this.used + amount)} > ${formatBytes(this.maxBytes)}).`);
    this.records.set(canonical, amount); this.used += amount; return true;
  }
  label() { return `${formatBytes(this.used)} / ${formatBytes(this.maxBytes)}`; }
}
