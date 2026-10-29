#version 450

// Non-mesh vertex shader for Points (camera-facing billboards) and Curves
// (camera-facing ribbons).  Instanced: 4 vertices per instance (GL_TRIANGLE_STRIP).
// gl_VertexIndex % 4 selects the corner: (-1,-1), (1,-1), (-1,1), (1,1).

layout(location = 0) in vec3 aP0;       // point center or curve segment start
layout(location = 1) in vec3 aP1;       // point center (same as aP0) or curve end
layout(location = 2) in vec3 aPrev;     // previous sampled curve point
layout(location = 3) in vec3 aNext;     // next sampled curve point
layout(location = 4) in vec2 aWidths;   // endpoint widths
layout(location = 5) in vec4 aColor0;   // endpoint rgba
layout(location = 6) in vec4 aColor1;

// Frame UBO (set 2, binding 0) — same layout as mesh.vert
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
  ivec4 ids;       // kind, material id, carrier id, purpose
  ivec4 mode;      // render mode, reserved
  vec4 cameraRight;
  vec4 cameraUp;
  uvec4 lightMask;
  vec4 shadowEye;
} pc;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec3 vView;
layout(location = 4) flat out int vInstanceId;
layout(location = 5) out vec3 vTangent;

void main() {
  vec3 camRight = pc.cameraRight.xyz;
  vec3 camUp = pc.cameraUp.xyz;

  int cornerId = gl_VertexIndex & 3;
  vec2 corner = cornerId == 0 ? vec2(-1,-1) :
                cornerId == 1 ? vec2( 1,-1) :
                cornerId == 2 ? vec2(-1, 1) : vec2(1,1);

  vec3 p;
  vec3 ribbonCenter;
  if (pc.ids.x == 0) {
    // Point billboard: camera-facing quad
    p = aP0 + (camRight * corner.x + camUp * corner.y) *
                  (0.5 * aWidths.x);
    vLocal = corner;
    vColor = aColor0;
    vTangent = camUp;
  } else if (pc.ids.x == 2) {
    // Gaussian carrier: aPrev/aNext contain the transformed major/minor
    // covariance semi-axes. This preserves the authored 3-D
    // orientation while perspective projection turns the disk into an ellipse.
    p = aP0 + (aPrev * corner.x + aNext * corner.y);
    vLocal = corner;
    vColor = aColor0;
    vTangent = normalize(aPrev);
  } else {
    // Curve ribbon: derive endpoint sides from neighbouring samples. Adjacent
    // records therefore calculate the same edge at their shared endpoint,
    // avoiding the triangular sawtooth gaps of independent segment quads.
    float along = corner.y * 0.5 + 0.5;
    vec3 center = mix(aP0, aP1, along);
    ribbonCenter = center;
    vec3 eye = pc.mode.y == 0 ? fr.camPos.xyz : pc.shadowEye.xyz;
    vec3 view0 = normalize(eye - aP0);
    vec3 view1 = normalize(eye - aP1);
    vec3 side0 = cross(aP1 - aPrev, view0);
    vec3 side1 = cross(aNext - aP0, view1);
    side0 = dot(side0, side0) < 1e-10 ? camRight : normalize(side0);
    side1 = dot(side1, side1) < 1e-10 ? camRight : normalize(side1);
    if (dot(side0, side1) < 0.0) side1 = -side1;
    vec3 side = normalize(mix(side0, side1, along));
    float width = mix(aWidths.x, aWidths.y, along);
    p = center + side * (corner.x * 0.5 * width);
    vLocal = vec2(corner.x, along);
    vColor = mix(aColor0, aColor1, along);
    vTangent = normalize(mix(aP1 - aPrev, aNext - aP0, along));
  }

  vWorldPos = p;
  vec3 eye = pc.mode.y == 0 ? fr.camPos.xyz : pc.shadowEye.xyz;
  vView = normalize(eye - p);
  vInstanceId = gl_InstanceIndex;
  mat4 vp = pc.mode.y >= 2
      ? fr.pointShadowViewProj[pc.mode.y - 2]
      : (pc.mode.y == 1 ? fr.shadowViewProj : fr.viewProj);
  gl_Position = vp * vec4(p, 1.0);

  // Very thin ribbons otherwise become unstable sub-pixel triangles before
  // the fragment shader gets a chance to apply analytic edge coverage. Keep
  // at least a 1.3-pixel raster footprint in the color pass; vLocal still
  // carries the authored edge, so the fragment shader softens the expansion.
  if (pc.ids.x == 1 && pc.mode.y == 0 &&
      pc.cameraRight.w > 0.0 && pc.cameraUp.w > 0.0) {
    vec4 centerClip = vp * vec4(ribbonCenter, 1.0);
    if (abs(centerClip.w) > 1e-6 && abs(gl_Position.w) > 1e-6) {
      vec2 centerNdc = centerClip.xy / centerClip.w;
      vec2 edgeNdc = gl_Position.xy / gl_Position.w;
      vec2 viewport = vec2(pc.cameraRight.w, pc.cameraUp.w);
      vec2 deltaPixels = (edgeNdc - centerNdc) * viewport * 0.5;
      float halfWidthPixels = length(deltaPixels);
      if (halfWidthPixels > 1e-5 && halfWidthPixels < 0.65) {
        edgeNdc = centerNdc + (edgeNdc - centerNdc) *
                                  (0.65 / halfWidthPixels);
      }
      // Adjacent curve segments are separate ribbon instances. A half-pixel
      // overlap closes rasterization-rule pinholes at their shared endpoints,
      // which otherwise turn distant whiskers into dotted lines.
      vec4 p0Clip = vp * vec4(aP0, 1.0);
      vec4 p1Clip = vp * vec4(aP1, 1.0);
      if (abs(p0Clip.w) > 1e-6 && abs(p1Clip.w) > 1e-6) {
        vec2 segmentPixels =
            (p1Clip.xy / p1Clip.w - p0Clip.xy / p0Clip.w) * viewport * 0.5;
        if (dot(segmentPixels, segmentPixels) > 1e-6) {
          vec2 segmentDirPixels = normalize(segmentPixels);
          edgeNdc += segmentDirPixels * corner.y / viewport;
        }
      }
      gl_Position.xy = edgeNdc * gl_Position.w;
    }
  }
}
