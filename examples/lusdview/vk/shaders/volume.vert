#version 450

// UsdVol volume proxy-box vertex shader. Maps a unit cube [0,1]^3 to the
// volume's object-space AABB, then to world and clip space.

layout(location = 0) in vec3 aPos;  // unit cube corner [0,1]^3

layout(set = 0, binding = 0, std140) uniform VolumeUBO {
  mat4 vp;        // proj * view
  mat4 model;     // object -> world
  mat4 invModel;  // world -> object
  vec4 camPos;    // .xyz world camera position
  vec4 bmin;      // object-space AABB min (.xyz)
  vec4 bmax;      // object-space AABB max (.xyz)
  vec4 albedo;    // .xyz albedo, .w densityScale
  vec4 emission;  // .xyz emission, .w background
  vec4 params;    // .x 1 = level-set isosurface, .y isovalue (fragment only)
} u;

layout(location = 0) out vec3 vWorld;

void main() {
  vec3 objp = u.bmin.xyz + aPos * (u.bmax.xyz - u.bmin.xyz);
  vec4 wp = u.model * vec4(objp, 1.0);
  vWorld = wp.xyz;
  gl_Position = u.vp * wp;
}
