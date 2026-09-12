// Arvo spherical-triangle sampling, adapted from pbrt-v4/util/sampling.cpp.
// pbrt Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// SPDX-License-Identifier: Apache-2.0 (see LICENSE-pbrt.txt).
// Geometry-only rejection chooses the ordinary area proposal. A rejected
// random sample must never silently change the proposal or its PDF.
bool sphericalTriangle(vec3 v0,vec3 v1,vec3 v2,vec3 p,vec2 u,out vec3 endpoint,out float omega) {
  vec3 a=v0-p,b=v1-p,c=v2-p;
  if(min(dot(a,a),min(dot(b,b),dot(c,c)))<1e-12)return false;
  a=normalize(a);b=normalize(b);c=normalize(c);
  vec3 ab=cross(a,b),bc=cross(b,c),ca=cross(c,a);
  if(min(dot(ab,ab),min(dot(bc,bc),dot(ca,ca)))<1e-10)return false;
  ab=normalize(ab);bc=normalize(bc);ca=normalize(ca);
  // atan2 avoids cancellation in the sum of three angles for thin triangles.
  float alpha=atan(length(cross(ab,ca)),-dot(ab,ca));
  omega=2*atan(abs(dot(a,cross(b,c))),1+dot(a,b)+dot(b,c)+dot(c,a));
  float sa=sin(alpha),co=cos(alpha);
  if(omega<3e-4||omega>6.22||abs(sa)<1e-5)return false;
  float phi=3.14159265359+u.x*omega-alpha;
  float sp=sin(phi),cp=cos(phi);
  float k1=cp+co,k2=sp-sa*dot(a,b);
  float q=clamp((k2+(k2*cp-k1*sp)*co)/((k2*sp+k1*cp)*sa),-1,1);
  vec3 edge=q*a+sqrt(max(0,1-q*q))*normalize(c-dot(c,a)*a);
  float z=1-u.y*(1-dot(edge,b));
  vec3 tangent=edge-dot(edge,b)*b;
  vec3 direction=z*b+sqrt(max(0,1-z*z))*tangent*inversesqrt(max(dot(tangent,tangent),1e-30));
  vec3 e1=v1-v0,e2=v2-v0,s=p-v0,h=cross(direction,e2);
  float inverse=1/dot(h,e1);
  vec2 bary=clamp(vec2(dot(s,h),dot(direction,cross(s,e1)))*inverse,0,1);
  bary/=max(1,bary.x+bary.y);
  endpoint=v0+bary.x*e1+bary.y*e2;
  return true;
}
