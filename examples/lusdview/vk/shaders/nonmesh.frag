#version 450

// Non-mesh fragment shader for Points (billboard with spherical normal) and
// Curves (flat ribbon normals).  Full PBR evaluation matching mesh.frag.

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec3 vView;
layout(location = 4) flat in int vInstanceId;
layout(location = 5) in vec3 vTangent;

// Frame UBO (set 2, binding 0) — same layout as mesh.frag
struct RasterLight { vec4 positionType; vec4 directionAngle;
                     vec4 colorDiffuse; vec4 specularShape; vec4 areaParams;
                     vec4 iesAxisX; vec4 iesAxisY; vec4 iesProfile[6]; };
layout(set = 2, binding = 0) uniform Frame {
  vec4 disp;
  mat4 viewProj;
  vec4 camPos;
  vec4 sceneMin;
  vec4 sceneExtent;
  vec4 lightDir;
  vec4 lightColor;
  RasterLight rasterLights[16];
  uvec4 rasterLightInfo;
  ivec4 mode;
  mat4 envRot;
  vec4 iblColor;
  vec4 iblParams;
  mat4 shadowViewProj;
  vec4 pointShadowLight;
  mat4 pointShadowViewProj[6];
} fr;

layout(push_constant) uniform Push {
  ivec4 ids;
  ivec4 mode;
  vec4 cameraRight;
  vec4 cameraUp;
  uvec4 lightMask;
  vec4 shadowEye;
} pc;

#ifdef LUSDVIEW_OIT
layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;
vec4 fragColor;
#else
layout(location = 0) out vec4 fragColor;
#endif

vec3 idColor(int id) {
  uint h = (uint(max(id, 0)) + 1u) * 2654435761u;
  return vec3(float(h & 255u), float((h >> 8u) & 255u),
              float((h >> 16u) & 255u)) * (1.0 / 255.0);
}

vec3 linearToSrgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  vec3 lo = c * 12.92;
  vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, vec3(greaterThan(c, vec3(0.0031308))));
}

float D(float nh, float r) {
  float a = max(r * r, 0.002);
  float a2 = a * a;
  float d = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / max(3.14159265 * d * d, 1e-6);
}

float G1(float nx, float r) {
  float k = (r + 1.0) * (r + 1.0) * 0.125;
  return nx / max(nx * (1.0 - k) + k, 1e-6);
}

vec3 F(float vh, vec3 f0) {
  return f0 + (vec3(1.0) - f0) * pow(1.0 - clamp(vh, 0.0, 1.0), 5.0);
}

// Bounded real-time approximation of UE HairBsdf's longitudinal R/TT/TRT
// response.  The strand tangent, rather than the camera ribbon normal, drives
// the shifted lobes; vColor acts as the melanin/absorption result authored by
// the MaterialX hair network.
float hairLobe(vec3 T, vec3 V, vec3 L, float shift, float width) {
  float sinV = dot(T, V);
  float sinL = dot(T, L);
  float longitudinal = sinV * sinL +
      sqrt(max(0.0, 1.0 - sinV * sinV)) *
      sqrt(max(0.0, 1.0 - sinL * sinL));
  longitudinal = clamp(longitudinal + shift, 0.0, 1.0);
  return pow(longitudinal, max(2.0, 2.0 / max(width * width, 1e-3)));
}

void shadeFragment() {
  vec3 N;
  float edgeCoverage = 1.0;
  if (pc.ids.x == 0 || pc.ids.x == 2 || pc.ids.x == 3) {
    // Point billboard: circular discard + spherical normal proxy
    float rr = dot(vLocal, vLocal);
    if (rr > 1.0) discard;
    vec3 camRight = normalize(pc.cameraRight.xyz);
    vec3 camUp = normalize(pc.cameraUp.xyz);
    N = normalize(camRight * vLocal.x + camUp * vLocal.y +
                  normalize(vView) * sqrt(max(0.0, 1.0 - rr)));
    if (pc.ids.x == 3) N = normalize(vTangent);
  } else {
    // Curve ribbon: view direction as normal
    N = normalize(vView);
    // Analytic one-pixel coverage for camera-facing ribbons. The old hard
    // triangle edge made subpixel whiskers staircase and flicker even though
    // their centerline tessellation was smooth.
    float edge = abs(vLocal.x);
    float aa = max(fwidth(edge), 1.0e-4);
    edgeCoverage = 1.0 - smoothstep(1.0 - aa, 1.0, edge);
    if (edgeCoverage <= 1.0e-3) discard;
  }

  if (pc.mode.y != 0) { fragColor = vec4(0.0); return; }

  // Render mode AOVs
  if (pc.mode.x == 2) { fragColor = vec4(N * 0.5 + 0.5, 1); return; }
  if (pc.mode.x == 3) { fragColor = vec4(idColor(pc.ids.y), 1); return; }
  if (pc.mode.x == 15) { fragColor = vec4(idColor(vInstanceId), 1); return; }
  if (pc.mode.x == 16) { fragColor = vec4(idColor(pc.ids.z), 1); return; }
  if (pc.mode.x == 18) {
    vec3 c = pc.ids.w == 1 ? vec3(0.3,0.7,1) :
             pc.ids.w == 2 ? vec3(1,0.6,0.2) :
             pc.ids.w == 3 ? vec3(0.8,0.3,1) : vec3(0.7);
    fragColor = vec4(c, 1); return;
  }
  if (pc.mode.x == 7) { fragColor = vec4(vColor.rgb, 1); return; }
  if (pc.mode.x == 12) { fragColor = vec4(vec3(vColor.a), 1); return; }
  if (pc.mode.x != 0) { fragColor = vec4(0.18, 0.18, 0.18, 1); return; }

  // Oriented ParticleField/Gaussian carriers carry authored SH radiance.
  // Applying the scene lights again can saturate an entire interior preview.
  if (pc.ids.x == 2 || pc.ids.x == 3) {
    float alpha = clamp(vColor.a, 0.0, 1.0);
    vec3 display = vColor.rgb * exp2(fr.iblParams.y);
    // Converted surface previews use a camera light to reveal otherwise
    // uniformly colored walls; authored SH radiance remains unlit.
    if (pc.ids.x == 3)
      display *= 0.25 + 0.75 * abs(dot(N, normalize(vView)));
#ifndef LUSDVIEW_OIT
    display = linearToSrgb(display) * alpha;
#endif
    fragColor = vec4(display, alpha);
    return;
  }

  vec3 V = normalize(vView);
  vec3 T = normalize(vTangent);
  float nv = max(dot(N, V), 1e-4);
  float roughness = 0.5;
  vec3 direct = vec3(0);

  for (int li = 0; li < 16; ++li) {
    if (li >= int(fr.rasterLightInfo.x)) break;
    uvec4 info = fr.rasterLightInfo;
    if ((pc.lightMask.x & (1u << uint(li))) == 0u) continue;

    vec4 pt = fr.rasterLights[li].positionType;
    vec4 da = fr.rasterLights[li].directionAngle;
    vec4 lc = fr.rasterLights[li].colorDiffuse;
    vec4 ss = fr.rasterLights[li].specularShape;
    int lt = int(pt.w + 0.5);

    vec3 L;
    float att = 1.0;
    if (lt == 5) {
      L = normalize(da.xyz);
    } else {
      vec3 q = pt.xyz - vWorldPos;
      float d2 = max(dot(q, q), 1e-6);
      L = q * inversesqrt(d2);
      att = 1.0 / d2;
    }

    float shape = 1.0;
    if (ss.w > 0.5 && lt != 5) {
      float cc = dot(normalize(da.xyz), -L);
      float o = cos(radians(clamp(da.w, 0, 180)));
      float inn = cos(radians(clamp(da.w * (1 - clamp(ss.y, 0, 1)), 0, 180)));
      shape = smoothstep(o, max(inn, o + 1e-5), cc) *
              pow(max(cc, 0), max(ss.z, 0));
    }
    float ies = 1.0;
    if (lt != 5 && dot(fr.rasterLights[li].iesProfile[0],
                       fr.rasterLights[li].iesProfile[0]) > 1e-8) {
      vec3 iesDir = normalize(-L);
      float v = degrees(acos(clamp(dot(iesDir, normalize(da.xyz)), -1.0, 1.0)));
      float fy = clamp(v / 60.0, 0.0, 3.0);
      int y0 = int(floor(fy));
      int y1 = min(y0 + 1, 3);
      float az = degrees(atan(dot(iesDir, fr.rasterLights[li].iesAxisY.xyz),
                              dot(iesDir, fr.rasterLights[li].iesAxisX.xyz)));
      if (az < 0.0) az += 360.0;
      float fx = az / 60.0;
      int x0 = min(int(floor(fx)), 5);
      int x1 = (x0 + 1) % 6;
      float tx = fx - float(x0);
      float a0 = mix(fr.rasterLights[li].iesProfile[y0][x0],
                     fr.rasterLights[li].iesProfile[y0][x1], tx);
      float a1 = mix(fr.rasterLights[li].iesProfile[y1][x0],
                     fr.rasterLights[li].iesProfile[y1][x1], tx);
      ies = mix(a0, a1, fy - float(y0));
    }

    float nl = max(dot(N, L), 0);
    if (nl <= 0 || shape <= 0) continue;

    if (pc.ids.x == 1) {
      float r = hairLobe(T, V, L, -0.025, 0.32);
      float tt = hairLobe(T, V, L, 0.055, 0.48);
      float trt = hairLobe(T, V, L, 0.11, 0.68);
      float wrap = 0.35 + 0.65 * sqrt(max(0.0, 1.0 - dot(T, L) * dot(T, L)));
      vec3 absorption = max(vColor.rgb, vec3(0.002));
      vec3 hairSpec = vec3(r) * 0.7 + sqrt(absorption) * tt * 0.55 +
                      absorption * trt * 0.35;
      direct += (absorption * 0.18 * wrap + hairSpec * 0.12) * lc.rgb *
                (att * shape * ies);
      continue;
    }

    vec3 H = normalize(V + L);
    float nh = max(dot(N, H), 0);
    float vh = max(dot(V, H), 0);

    vec3 ff = F(vh, vec3(0.04));
    float spec = D(nh, roughness) * G1(nv, roughness) * G1(nl, roughness) *
                 ff.x / max(4.0 * nv * nl, 1e-5);
    vec3 diff = (vec3(1.0) - ff) * vColor.rgb * (1.0 / 3.14159265);

    direct += (diff * lc.w + spec * ss.x) * lc.rgb *
              (att * shape * ies * nl);
  }

  // Fallback to single key light when no multi-light data
  if (fr.rasterLightInfo.x == 0) {
    vec3 L = normalize(fr.lightDir.xyz);
    if (pc.ids.x == 1) {
      float r = hairLobe(T, V, L, -0.025, 0.32);
      float tt = hairLobe(T, V, L, 0.055, 0.48);
      float trt = hairLobe(T, V, L, 0.11, 0.68);
      vec3 absorption = max(vColor.rgb, vec3(0.002));
      direct = ((vec3(r) * 0.7 + sqrt(absorption) * tt * 0.55 +
                absorption * trt * 0.35) * 0.12 + absorption * 0.08) *
               fr.lightColor.xyz;
    } else {
      vec3 H = normalize(V + L);
      float nl = max(dot(N, L), 0);
      float nh = max(dot(N, H), 0);
      float vh = max(dot(V, H), 0);
      vec3 ff = F(vh, vec3(0.04));
      float spec = D(nh, roughness) * G1(nv, roughness) * G1(nl, roughness) *
                   ff.x / max(4.0 * nv * nl, 1e-5);
      vec3 diff = (vec3(1.0) - ff) * vColor.rgb * (1.0 / 3.14159265);
      direct = (diff + spec) * fr.lightColor.xyz * nl;
    }
  }

  // Curves this thin are dominated by subpixel coverage; a very dark ambient
  // term turns a continuous white whisker into apparent dashes whenever the
  // narrow hair lobe misses the light. Keep enough authored color to make the
  // antialiased centerline visually continuous, while retaining direct lobes.
  float ambientWeight = pc.ids.x == 1 ? 0.35 : 0.20;
  vec3 col = vColor.rgb * ambientWeight + direct;
  float alpha = clamp(vColor.a * edgeCoverage, 0.0, 1.0);
  vec3 display = col * exp2(fr.iblParams.y);
#ifndef LUSDVIEW_OIT
  display = linearToSrgb(display);
  display *= alpha;
#endif
  fragColor = vec4(display, alpha);
}

#ifdef LUSDVIEW_OIT
void main() {
  shadeFragment();
  float alpha = clamp(fragColor.a, 0.0, 1.0);
  if (alpha <= 1.0e-4) discard;
  float weight = clamp(
      pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1.0e8 *
          pow(1.0 - gl_FragCoord.z * 0.9, 3.0),
      1.0e-2, 3.0e3);
  outAccum = vec4(fragColor.rgb * alpha, alpha) * weight;
  outReveal = alpha;
}
#else
void main() { shadeFragment(); }
#endif
