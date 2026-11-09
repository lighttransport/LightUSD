function normalize(value, min, span) { return span > 1e-12 ? (value - min) / span : .5; }

export function projectUVs({ positions, mode = 'planar' }) {
  const count = Math.floor(positions.length / 3), uvs = new Float32Array(count * 2);
  let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i < count; i++) { const x = positions[i * 3], y = positions[i * 3 + 1], z = positions[i * 3 + 2]; minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z); maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z); }
  const sx = maxX - minX, sy = maxY - minY, sz = maxZ - minZ;
  for (let i = 0; i < count; i++) {
    const x = positions[i * 3], y = positions[i * 3 + 1], z = positions[i * 3 + 2], o = i * 2;
    let u, v;
    if (mode === 'cylindrical') { u = (Math.atan2(z, x) / (Math.PI * 2) + .5); v = normalize(y, minY, sy); }
    else if (mode === 'spherical') { u = (Math.atan2(z, x) / (Math.PI * 2) + .5); const radius = Math.hypot(x, y, z) || 1; v = Math.asin(Math.max(-1, Math.min(1, y / radius))) / Math.PI + .5; }
    else if (mode === 'box') { const ax = Math.abs(x / (sx || 1)), ay = Math.abs(y / (sy || 1)), az = Math.abs(z / (sz || 1)); if (ax >= ay && ax >= az) { u = normalize(z, minZ, sz); v = normalize(y, minY, sy); } else if (ay >= az) { u = normalize(x, minX, sx); v = normalize(z, minZ, sz); } else { u = normalize(x, minX, sx); v = normalize(y, minY, sy); } }
    else { u = normalize(x, minX, sx); v = normalize(y, minY, sy); }
    uvs[o] = Number.isFinite(u) ? u : .5; uvs[o + 1] = Number.isFinite(v) ? v : .5;
  }
  return uvs;
}
