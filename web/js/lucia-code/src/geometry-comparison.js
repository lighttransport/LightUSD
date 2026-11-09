function finite(value) { return Number.isFinite(value); }

export function compareGeometrySnapshots(before, current, { maxSamples = 512 } = {}) {
  const source = before instanceof Float32Array ? before : new Float32Array(before || []), target = current instanceof Float32Array ? current : new Float32Array(current || []), sourceCount = Math.floor(source.length / 3), targetCount = Math.floor(target.length / 3);
  if (!sourceCount || !targetCount) return { compared: 0, samples: [], meanDistance: null, maxDistance: null, normalizedMean: null, normalizedMax: null, scale: 0 };
  let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (const values of [source, target]) for (let i = 0; i + 2 < values.length; i += 3) if ([values[i], values[i + 1], values[i + 2]].every(finite)) { minX = Math.min(minX, values[i]); minY = Math.min(minY, values[i + 1]); minZ = Math.min(minZ, values[i + 2]); maxX = Math.max(maxX, values[i]); maxY = Math.max(maxY, values[i + 1]); maxZ = Math.max(maxZ, values[i + 2]); }
  const scale = Math.max(maxX - minX, maxY - minY, maxZ - minZ, 1e-12), stride = Math.max(1, Math.ceil(sourceCount / Math.max(1, Math.floor(maxSamples)))), samples = [];
  let distanceSum = 0, maximumDistance = 0;
  for (let i = 0; i < sourceCount; i += stride) {
    const x = source[i * 3], y = source[i * 3 + 1], z = source[i * 3 + 2]; if (![x, y, z].every(finite)) continue;
    let nearest = Infinity; for (let j = 0; j < targetCount; j++) { const dx = x - target[j * 3], dy = y - target[j * 3 + 1], dz = z - target[j * 3 + 2], distance = dx * dx + dy * dy + dz * dz; if (distance < nearest) nearest = distance; }
    const distance = Math.sqrt(nearest); samples.push({ position: [x, y, z], distance, normalized: distance / scale }); distanceSum += distance; maximumDistance = Math.max(maximumDistance, distance);
  }
  const meanDistance = samples.length ? distanceSum / samples.length : null;
  return { compared: samples.length, samples, meanDistance, maxDistance: samples.length ? maximumDistance : null, normalizedMean: meanDistance == null ? null : meanDistance / scale, normalizedMax: samples.length ? maximumDistance / scale : null, scale };
}
