#version 450

// UsdVol volume fragment shader. Two modes, selected by u.params.x:
//   0: emission/absorption raymarch of a dense density grid (3D texture).
//   1: level-set field: the first sign change along the ray is an isosurface
//      hit, shaded with the field gradient (UsdVol fieldClass "levelSet").
// Outputs premultiplied color (composited "over" the surface via
// VK_BLEND_FACTOR_ONE / ONE_MINUS_SRC_ALPHA).

layout(location = 0) in vec3 vWorld;
#ifdef LUSDVIEW_OIT
layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;
#else
layout(location = 0) out vec4 fragColor;
#endif

layout(set = 0, binding = 0, std140) uniform VolumeUBO {
  mat4 vp;
  mat4 model;
  mat4 invModel;
  vec4 camPos;
  vec4 bmin;
  vec4 bmax;
  vec4 albedo;    // .xyz albedo, .w densityScale
  vec4 emission;  // .xyz emission, .w background
  vec4 params;    // .x 1 = level-set isosurface, .y isovalue
} u;

// R=density, G=emission/flame, B=temperature. bmin.w/bmax.w indicate
// whether the optional channels are present.
layout(set = 0, binding = 1) uniform sampler3D uFields;

vec3 blackbody(float value) {
  float k = value > 100.0 ? value : 1000.0 + 5500.0 * max(value, 0.0);
  float t = clamp(k / 100.0, 10.0, 400.0);
  float r = t <= 66.0 ? 1.0 : 1.2929362 * pow(t - 60.0, -0.13320476);
  float g = t <= 66.0 ? 0.39008158 * log(t) - 0.63184144
                      : 1.1298909 * pow(t - 60.0, -0.07551485);
  float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0
                                         : 0.5432068 * log(t - 10.0) - 1.1962541);
  return clamp(vec3(r, g, b), 0.0, 1.0);
}

bool rayAABB(vec3 o, vec3 d, vec3 lo, vec3 hi, out float t0, out float t1) {
  vec3 inv = 1.0 / d;
  vec3 ta = (lo - o) * inv;
  vec3 tb = (hi - o) * inv;
  vec3 tmin = min(ta, tb);
  vec3 tmax = max(ta, tb);
  t0 = max(max(tmin.x, tmin.y), tmin.z);
  t1 = min(min(tmax.x, tmax.y), tmax.z);
  return t1 > max(t0, 0.0);
}

void main() {
  gl_FragDepth = gl_FragCoord.z;
  vec3 bmin = u.bmin.xyz;
  vec3 bmax = u.bmax.xyz;
  float densityScale = u.albedo.w;
  float background = u.emission.w;

  // World ray -> object space.
  vec3 oo = (u.invModel * vec4(u.camPos.xyz, 1.0)).xyz;
  vec3 od = normalize((u.invModel * vec4(vWorld, 1.0)).xyz - oo);

  float t0, t1;
  if (!rayAABB(oo, od, bmin, bmax, t0, t1)) discard;
  t0 = max(t0, 0.0);

  vec3 ext = bmax - bmin;
  vec3 invExt = 1.0 / max(ext, vec3(1.0e-20));

  if (u.params.x > 0.5) {
    // Level set: march to the first crossing of the isovalue, refine it and
    // shade the surface with the field gradient (the field is a signed
    // distance, so the gradient points out of the interior).
    float iso = u.params.y;
    float step = max((t1 - t0) / 512.0, min(ext.x, min(ext.y, ext.z)) / 256.0);
    if (step <= 0.0) step = (t1 - t0) / 256.0;
    float prev = texture(uFields, (oo + od * t0 - bmin) * invExt).r - iso;
    float t = t0;
    float hit = -1.0;
    for (int i = 0; i < 512; i++) {
      float tn = min(t0 + float(i + 1) * step, t1);
      float f = texture(uFields, (oo + od * tn - bmin) * invExt).r - iso;
      if ((f < 0.0 && prev > 0.0) || (f > 0.0 && prev < 0.0)) {
        float lo = t, hi = tn, flo = prev;
        for (int k = 0; k < 16; k++) {
          float mid = 0.5 * (lo + hi);
          float fm = texture(uFields, (oo + od * mid - bmin) * invExt).r - iso;
          if (fm == 0.0) { lo = hi = mid; break; }
          if ((fm > 0.0) == (flo > 0.0)) { lo = mid; flo = fm; } else { hi = mid; }
        }
        hit = 0.5 * (lo + hi);
        break;
      }
      /* An exact zero is not a hit: narrow-band VDB leaves store 0 in
       * inactive voxels outside the band, so only a sign change marks the
       * surface. */
      if (f != 0.0) { prev = f; t = tn; }
      else if (prev == 0.0) t = tn;
      if (tn >= t1) break;
    }
    if (hit < 0.0) discard;
    vec3 p = oo + od * hit;
    vec3 uvw = (p - bmin) * invExt;
    vec3 e = 1.0 / vec3(textureSize(uFields, 0));
    vec3 g = vec3(
        texture(uFields, uvw + vec3(e.x, 0.0, 0.0)).r -
            texture(uFields, uvw - vec3(e.x, 0.0, 0.0)).r,
        texture(uFields, uvw + vec3(0.0, e.y, 0.0)).r -
            texture(uFields, uvw - vec3(0.0, e.y, 0.0)).r,
        texture(uFields, uvw + vec3(0.0, 0.0, e.z)).r -
            texture(uFields, uvw - vec3(0.0, 0.0, e.z)).r);
    vec3 n = transpose(mat3(u.invModel)) * (g * invExt / e);
    vec3 worldPoint = (u.model * vec4(p, 1.0)).xyz;
    vec3 viewDir = normalize(u.camPos.xyz - worldPoint);
    if (dot(n, n) > 0.0) n = normalize(n); else n = viewDir;
    float ndl = abs(dot(n, viewDir));
    vec4 clip = u.vp * vec4(worldPoint, 1.0);
    gl_FragDepth = clip.z / clip.w;
    vec3 srgb = u.albedo.xyz * (0.15 + 0.85 * ndl) + u.emission.xyz;
#ifdef LUSDVIEW_OIT
    float weight = clamp(
        pow(min(1.0, 1.0 * 10.0) + 0.01, 3.0) * 1.0e8 *
            pow(1.0 - gl_FragCoord.z * 0.9, 3.0),
        1.0e-2, 3.0e3);
    outAccum = vec4(srgb, 1.0) * weight;
    outReveal = 1.0;
#else
    fragColor = vec4(srgb, 1.0);  // premultiplied, opaque
#endif
    return;
  }

  float step = min(ext.x, min(ext.y, ext.z)) / 128.0;
  if (step <= 0.0) step = (t1 - t0) / 256.0;
  float T = 1.0;
  vec3 L = vec3(0.0);
  float t = t0;
  for (int i = 0; i < 256; i++) {
    if (t >= t1) break;
    vec3 p = oo + od * (t + 0.5 * step);
    vec3 uvw = (p - bmin) / ext;
    vec3 fields = texture(uFields, uvw).rgb;
    float dens = (fields.r - background) * densityScale;
    if (dens > 0.0) {
      float a = 1.0 - exp(-dens * step);
      float ew = u.bmin.w > 0.5 ? max(fields.g, 0.0) : dens;
      vec3 ec = u.emission.xyz;
      if (u.bmax.w > 0.5) {
        float temp = max(fields.b, 0.0);
        vec3 tint = blackbody(temp);
        ec = dot(ec, vec3(1.0)) > 0.0 ? ec * tint : tint;
        ew = max(ew, temp);
      }
      vec3 src = u.albedo.xyz * a + ec * (ew * step);
      L += T * src;
      T *= (1.0 - a);
      if (T < 0.003) break;
    }
    t += step;
  }
  float alpha = 1.0 - T;
  if (alpha <= 0.001) discard;
#ifdef LUSDVIEW_OIT
  vec3 color = L / max(alpha, 1.0e-5);
  float weight = clamp(
      pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1.0e8 *
          pow(1.0 - gl_FragCoord.z * 0.9, 3.0),
      1.0e-2, 3.0e3);
  outAccum = vec4(color * alpha, alpha) * weight;
  outReveal = alpha;
#else
  fragColor = vec4(L, alpha);  // premultiplied
#endif
}
