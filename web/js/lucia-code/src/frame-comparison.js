const finite = (value) => Number.isFinite(value);
const angleBetween = (left, right) => {
  const leftLength = Math.hypot(left[0], left[1], left[2]), rightLength = Math.hypot(right[0], right[1], right[2]);
  if (!Number.isFinite(leftLength) || !Number.isFinite(rightLength) || leftLength <= 1e-12 || rightLength <= 1e-12) return null;
  return Math.acos(Math.max(-1, Math.min(1, (left[0] * right[0] + left[1] * right[1] + left[2] * right[2]) / (leftLength * rightLength)))) * 180 / Math.PI;
};

export function compareFrameSnapshots(before, current) {
  const count = Math.min(before?.count || 0, current?.count || 0), result = { compared: count, normals: null, tangents: null };
  for (const kind of ['normals', 'tangents']) {
    if (!before?.[kind] || !current?.[kind]) continue;
    let sum = 0, max = 0, compared = 0; const samples = [];
    for (let i = 0; i < count; i++) { const angle = angleBetween(before[kind].slice(i * 3, i * 3 + 3), current[kind].slice(i * 3, i * 3 + 3)); if (angle == null) continue; sum += angle; max = Math.max(max, angle); compared++; const position = current.positions?.slice(i * 3, i * 3 + 3); if (position?.length === 3 && [...position].every(finite)) samples.push({ position: [...position], degrees: angle, normalized: angle / 180 }); }
    result[kind] = { compared, meanDegrees: compared ? sum / compared : null, maxDegrees: compared ? max : null, samples };
  }
  return result;
}
