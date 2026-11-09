// Dependency-free color-space matrix math shared by LightUSD and Lucia.

const D65 = [0.3127, 0.3290];

export const LINEAR_COLOR_SPACES = Object.freeze({
  lin_ap0_scene: [[0.73485524337371, 0.26422532524554],
    [-0.0061709124786224, 1.0113149590212864],
    [0.015967559255041, -0.064235503128551], D65],
  lin_ap1_scene: [[0.71319588766205, 0.29268891446333],
    [0.15950855654178, 0.83878851615096],
    [0.128672995285350, 0.043895571160528], D65],
  lin_rec709_scene: [[0.64, 0.33], [0.30, 0.60], [0.15, 0.06], D65],
  lin_p3d65_scene: [[0.68, 0.32], [0.265, 0.69], [0.15, 0.06], D65],
  lin_rec2020_scene: [[0.708, 0.292], [0.17, 0.797], [0.131, 0.046], D65],
  lin_adobergb_scene: [[0.64, 0.33], [0.21, 0.71], [0.15, 0.06], D65]
});

function invert3(m) {
  const d = m[0] * (m[4] * m[8] - m[5] * m[7]) -
    m[1] * (m[3] * m[8] - m[5] * m[6]) +
    m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (!Number.isFinite(d) || Math.abs(d) <= 1e-15) throw new Error('singular color-space matrix');
  return [(m[4] * m[8] - m[5] * m[7]) / d,
    (m[2] * m[7] - m[1] * m[8]) / d,
    (m[1] * m[5] - m[2] * m[4]) / d,
    (m[5] * m[6] - m[3] * m[8]) / d,
    (m[0] * m[8] - m[2] * m[6]) / d,
    (m[2] * m[3] - m[0] * m[5]) / d,
    (m[3] * m[7] - m[4] * m[6]) / d,
    (m[1] * m[6] - m[0] * m[7]) / d,
    (m[0] * m[4] - m[1] * m[3]) / d];
}

function mul3(a, b) {
  return Array.from({ length: 9 }, (_, i) => {
    const row = Math.floor(i / 3), column = i % 3;
    return a[row * 3] * b[column] + a[row * 3 + 1] * b[column + 3] + a[row * 3 + 2] * b[column + 6];
  });
}

function mulv(m, v) {
  return [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]];
}

function rgbToXyz(space) {
  const [r, g, b, white] = LINEAR_COLOR_SPACES[space], basis = [r[0], g[0], b[0], r[1], g[1], b[1], 1 - r[0] - r[1], 1 - g[0] - g[1], 1 - b[0] - b[1]], scale = mulv(invert3(basis), [white[0] / white[1], 1, (1 - white[0] - white[1]) / white[1]]);
  return basis.map((value, index) => value * scale[index % 3]);
}

export function linearColorTransformMatrix(source = 'lin_ap0_scene', destination = 'lin_rec709_scene') {
  if (!LINEAR_COLOR_SPACES[source] || !LINEAR_COLOR_SPACES[destination]) throw new Error(`unsupported color space: ${source} -> ${destination}`);
  return mul3(invert3(rgbToXyz(destination)), rgbToXyz(source));
}

export function transformLinearColor(rgb, source = 'lin_ap0_scene', destination = 'lin_rec709_scene') {
  return mulv(linearColorTransformMatrix(source, destination), rgb);
}
