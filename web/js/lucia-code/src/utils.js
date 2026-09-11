export const encoder = new TextEncoder();
export const decoder = new TextDecoder();

export function bytesToBase64(bytes) {
  let out = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    out += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(out);
}

export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 0);
}

export function validIdentifier(name) {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(String(name));
}

export function validPrimPath(path) {
  return path === '/' || /^\/(?:[A-Za-z_][A-Za-z0-9_]*)(?:\/[A-Za-z_][A-Za-z0-9_]*)*$/.test(String(path));
}

export function basename(path) {
  return String(path).split('/').filter(Boolean).at(-1) || 'scene';
}

export function escapeRegExp(text) {
  return String(text).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

export class LuciaError extends Error {
  constructor(code, message, details = {}) {
    super(message);
    this.name = 'LuciaError';
    this.code = code;
    this.details = details;
  }
}
