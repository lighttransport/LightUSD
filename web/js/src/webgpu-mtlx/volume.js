// SPDX-License-Identifier: Apache-2.0
// Homogeneous spectral/RGB free-flight and Henyey-Greenstein scattering.
export const volumeWGSL = /* wgsl */`
fn hgPhase(cosine: f32, g: f32) -> f32 {
  let d=1.0+g*g-2.0*g*cosine;
  return (1.0-g*g)/(4.0*PI*d*sqrt(d));
}
fn hgDirection(d:vec3f,g:f32,u:vec2f)->vec3f {
  var c=1.0-2.0*u.x;
  if(abs(g)>0.001) { let q=(1.0-g*g)/(1.0-g+2.0*g*u.x); c=clamp((1.0+g*g-q*q)/(2.0*g),-1.0,1.0); }
  let s=sqrt(max(0.0,1.0-c*c));let phi=2.0*PI*u.y;
  return transportFrame(d)*vec3f(s*cos(phi),s*sin(phi),c);
}
fn mediumAt(id:u32,ctx:ShadingContext,lambda:f32,spectral:bool)->Medium {
  var medium=getMedium(id,ctx);
  if(spectral) {
    medium.absorption=vec3f(rgbSpectrum(medium.absorption,lambda,false));
    medium.scattering=vec3f(rgbSpectrum(medium.scattering,lambda,false));
  }
  return medium;
}
`;
