// SPDX-License-Identifier: Apache-2.0
// Cycles Gaussian intersection and SH emission, shared by Vulkan tracers.
// Shutter time in [0,1] of this pixel's sample; camera, shadow and bounce
// rays of the sample all see the motion-blurred splats at it (Cycles).
float g_gsTime = 0.5;

float gsHash(vec3 p, uint seed) {
  uint h = 0x9e3779b9u ^ (seed * 0x85ebca6bu);
  for (int i = 0; i < 3; i++) {
    uint b = floatBitsToUint(p[i]);
    h ^= b + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h = h * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
  }
  return float(h >> 8) * (1.0 / 16777216.0);
}

bool gsBox(uint n, vec3 o, vec3 inv, float tmax, out float tnear) {
  uint b = n * 8u;
  vec3 lo = vec3(uintBitsToFloat(gsN[b]), uintBitsToFloat(gsN[b + 1u]),
                 uintBitsToFloat(gsN[b + 2u]));
  vec3 hi = vec3(uintBitsToFloat(gsN[b + 3u]), uintBitsToFloat(gsN[b + 4u]),
                 uintBitsToFloat(gsN[b + 5u]));
  vec3 a = (lo - o) * inv, c = (hi - o) * inv;
  vec3 tn = min(a, c), tf = max(a, c);
  float t0 = max(max(tn.x, tn.y), max(tn.z, 0.0));
  float t1 = min(min(tf.x, tf.y), min(tf.z, tmax));
  tnear = t0;
  return t0 <= t1;
}

float traceCyclesSplats(vec3 o, vec3 d, float tmax, uint seed, out int hit) {
  hit = -1;
  uint chunkCount = gsChunkCount();
  if (chunkCount == 0u) return 1e30;
  uint K = gsKeyCount();
  uint kstep = 0u; float kt = 0.0;
  if (K > 1u) {
    float f = clamp(g_gsTime, 0.0, 1.0) * float(K - 1u);
    kstep = min(uint(f), K - 2u);
    kt = f - float(kstep);
  }
  vec3 inv;
  for (int k = 0; k < 3; k++) {
    float v = 1.0 / d[k];
    if (!(v >= -1e18 && v <= 1e18)) v = (d[k] < 0.0) ? -1e18 : 1e18;
    inv[k] = v;
  }
  float dd = dot(d, d);
  float best = tmax;
  for (uint c = 0u; c < chunkCount; ++c) {
    uint nb = uint(gsC[c * 2u]), sb0 = uint(gsC[c * 2u + 1u]);
    uint stk[64]; int sp = 0; float tn;
    if (gsBox(nb, o, inv, best, tn)) stk[sp++] = 0u;
    while (sp > 0) {
      uint n = nb + stk[--sp];
      uint first = gsN[n * 8u + 6u], count = gsN[n * 8u + 7u];
      if (count == 0u) {
        float ta, tb;
        bool ha = gsBox(nb + first, o, inv, best, ta);
        bool hb = gsBox(nb + first + 1u, o, inv, best, tb);
        if (ha && hb && sp + 2 <= 64) {
          if (ta <= tb) { stk[sp++] = first + 1u; stk[sp++] = first; }
          else          { stk[sp++] = first;      stk[sp++] = first + 1u; }
        } else if (ha && sp < 64) stk[sp++] = first;
        else if (hb && sp < 64) stk[sp++] = first + 1u;
        continue;
      }
      for (uint i = sb0 + first; i < sb0 + first + count; i++) {
        uint s = (i * K + kstep) * 16u;
        uint s2 = K > 1u ? s + 16u : s;
        #define GSK(o) mix(gsS[s + (o)], gsS[s2 + (o)], kt)
        vec3 mu = vec3(GSK(0u), GSK(1u), GSK(2u));
        float radius = GSK(13u);
        if (!(radius > 0.0)) continue;
        vec3 oc = o - mu;
        float b = dot(oc, d);
        if (b * b - dd * (dot(oc, oc) - radius * radius) < 0.0) continue;
        vec3 r0 = vec3(GSK(4u), GSK(5u), GSK(6u));
        vec3 r1 = vec3(GSK(7u), GSK(8u), GSK(9u));
        vec3 r2 = vec3(GSK(10u), GSK(11u), GSK(12u));
        vec3 og = vec3(dot(r0, oc), dot(r1, oc), dot(r2, oc));
        vec3 dg = vec3(dot(r0, d), dot(r1, d), dot(r2, d));
        float t = -dot(og, dg) / dot(dg, dg);
        if (!(t > 0.0 && t <= best)) continue;
        vec3 e = og + dg * t;
        float alpha = GSK(3u) * exp(-0.5 * dot(e, e));
        #undef GSK
        if (gsHash(o + d * t, seed) > alpha) continue;
        best = t; hit = int(i);
      }
    }
  }
  return best;
}

float gsSrgbToLinear(float c) {
  return c < 0.04045 ? max(c, 0.0) * (1.0 / 12.92)
                     : pow((c + 0.055) * (1.0 / 1.055), 2.4);
}

// SH emission (+0.5) evaluated along the object-space view direction.
vec3 cyclesSplatRadiance(int id, vec3 d) {
  uint s = uint(id) * gsKeyCount() * 16u;
  uint shOff = floatBitsToUint(gsS[s + 14u]);
  uint meta = floatBitsToUint(gsS[s + 15u]);
  uint deg = meta & 3u, field = meta >> 8u;
  uint f = floatBitsToUint(gsF[2]) + field * 12u;  // header: field base
  vec3 v = vec3(gsF[f] * d.x + gsF[f + 1u] * d.y + gsF[f + 2u] * d.z,
                gsF[f + 3u] * d.x + gsF[f + 4u] * d.y + gsF[f + 5u] * d.z,
                gsF[f + 6u] * d.x + gsF[f + 7u] * d.y + gsF[f + 8u] * d.z);
  float l = length(v);
  v = l > 0.0 ? v / l : vec3(0, 0, 1);
  float x = v.x, y = v.y, z = v.z;
  float bs[16];
  bs[0] = 0.282094792;
  uint nb = 1u;
  if (deg >= 1u) {
    bs[1] = -0.488602512 * y; bs[2] = 0.488602512 * z; bs[3] = -0.488602512 * x;
    nb = 4u;
  }
  if (deg >= 2u) {
    bs[4] = 1.092548431 * x * y; bs[5] = -1.092548431 * y * z;
    bs[6] = 0.315391565 * (3.0 * z * z - 1.0); bs[7] = -1.092548431 * x * z;
    bs[8] = 0.546274215 * (x * x - y * y);
    nb = 9u;
  }
  if (deg >= 3u) {
    bs[9] = -0.590043589 * y * (3.0 * x * x - y * y);
    bs[10] = 2.890611442 * x * y * z;
    bs[11] = -0.457045799 * y * (5.0 * z * z - 1.0);
    bs[12] = 0.373176332 * z * (5.0 * z * z - 3.0);
    bs[13] = -0.457045799 * x * (5.0 * z * z - 1.0);
    bs[14] = 1.445305721 * z * (x * x - y * y);
    bs[15] = -0.590043590 * x * (x * x - 3.0 * y * y);
    nb = 16u;
  }
  vec3 c = vec3(0.5);
  for (uint i = 0u; i < nb; ++i)
    c += vec3(gsF[shOff + i * 3u], gsF[shOff + i * 3u + 1u],
              gsF[shOff + i * 3u + 2u]) * bs[i];
  // Animated DC colour (Cycles motion radiance_base): swap band 0 for the
  // colour interpolated at this sample's shutter time.
  uint dcBase = floatBitsToUint(gsF[0]);
  uint K = gsKeyCount();
  if (dcBase != 0u && K > 1u) {
    float fk = clamp(g_gsTime, 0.0, 1.0) * float(K - 1u);
    uint st = min(uint(fk), K - 2u);
    float tk = fk - float(st);
    uint d0 = dcBase + (uint(id) * K + st) * 3u;
    vec3 dc = mix(vec3(gsF[d0], gsF[d0 + 1u], gsF[d0 + 2u]),
                  vec3(gsF[d0 + 3u], gsF[d0 + 4u], gsF[d0 + 5u]), tk);
    vec3 dc0 = vec3(gsF[shOff], gsF[shOff + 1u], gsF[shOff + 2u]);
    c += (dc - dc0) * bs[0];
  }
  if ((meta & 4u) != 0u)
    return vec3(gsSrgbToLinear(c.x), gsSrgbToLinear(c.y), gsSrgbToLinear(c.z));
  return max(c, vec3(0.0));
}
