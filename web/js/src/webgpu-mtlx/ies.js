// SPDX-License-Identifier: Apache-2.0
// Bounded LM-63 photometric profile reader used by measured_edf.
export function parseIES(input) {
  const text = typeof input === 'string' ? input : new TextDecoder().decode(input);
  const tiltMatch = text.match(/(?:^|\n)\s*\[?TILT\s*=\s*([^\]\r\n]+)/i);
  if (!tiltMatch) throw new Error('IES profile is missing TILT data');
  if (tiltMatch[1].trim().toUpperCase() !== 'NONE') throw new Error('IES TILT=INCLUDE profiles are unsupported');
  const body = text.slice(text.indexOf('\n', tiltMatch.index) + 1);
  const values = body.match(/[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/g)?.map(Number) || [];
  if (values.length < 13 || values.some(v => !Number.isFinite(v))) throw new Error('Invalid IES numeric data');
  let i = 0;
  const lampCount = values[i++], lumens = values[i++], multiplier = values[i++];
  const verticalCount = values[i++], horizontalCount = values[i++];
  i += 8; // photometric type, units, dimensions, ballast, future use and watts
  if (![lampCount, verticalCount, horizontalCount].every(Number.isInteger) || lampCount < 1 || verticalCount < 2 || verticalCount > 181 || horizontalCount < 1 || horizontalCount > 73 || multiplier < 0 || lumens < 0) throw new Error('IES profile exceeds bounds');
  const angles = values.slice(i, i += verticalCount);
  const horizontal = values.slice(i, i += horizontalCount);
  if (angles.some((v, n) => v < 0 || v > 180 || n && v <= angles[n - 1]) || horizontal.some((v, n) => n && v <= horizontal[n - 1])) throw new Error('Invalid IES angles');
  const count = verticalCount * horizontalCount;
  if (values.length < i + count) throw new Error('IES candela table is truncated');
  const candela = values.slice(i, i + count).map(v => v * multiplier);
  for (let h = 1; h < horizontalCount; h++) for (let v = 0; v < verticalCount; v++) {
    const reference = candela[v], value = candela[h * verticalCount + v];
    if (Math.abs(value - reference) > 1e-5 * Math.max(1, Math.abs(reference), Math.abs(value))) throw new Error('IES azimuthal profiles are unsupported');
  }
  const samples = angles.map((angle, n) => {
    let peak = 0;
    for (let h = 0; h < horizontalCount; h++) peak = Math.max(peak, candela[h * verticalCount + n]);
    return [angle, peak];
  });
  const max = Math.max(...samples.map(([, value]) => value));
  if (!(max > 0)) throw new Error('IES profile has no positive candela values');
  return { samples: samples.map(([angle, value]) => [angle, value / max]), lumens, lampCount };
}
