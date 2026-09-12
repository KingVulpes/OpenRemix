#extension GL_GOOGLE_include_directive : require
#define NRD_GLSL 1
#include "NRD.hlsli"
layout(set=1,binding=0,rgba16f) uniform image2D sh0;
layout(set=1,binding=1,rgba16f) uniform image2D sh1;
layout(set=1,binding=2,rgb10_a2) uniform image2D guideNormal;
layout(set=1,binding=3,r32f) uniform image2D guideDepth;
layout(set=1,binding=4,rgba32f) uniform image2D guideMotion;
layout(set=1,binding=5,rgba16f) uniform image2D guideAlbedo;
layout(set=1,binding=6,rgba16f) uniform image2D directLight;
layout(set=1,binding=7,rgba16f) uniform image2D transparentLight;
layout(set=1,binding=8,rgba16f) uniform image2D transmittance;
layout(set=1,binding=9,rgba16f) uniform image2D outSh0;
layout(set=1,binding=10,rgba16f) uniform image2D outSh1;
layout(set=1,binding=11,r32f) uniform image2D confidence;
layout(set=1,binding=12,rgba16f) uniform image2D gradientA;
layout(set=1,binding=13,rgba16f) uniform image2D gradientB;
layout(set=1,binding=14,rgba16f) uniform image2D history0;
layout(set=1,binding=15,rgba16f) uniform image2D history1;
layout(set=1,binding=16,r32f) uniform image2D previousDepth;
layout(set=1,binding=17,rgb10_a2) uniform image2D previousNormal;
layout(set=1,binding=18,rgba16f) uniform image2D previousLighting;
layout(set=1,binding=19,rgba16f) uniform image2D lighting;
layout(set=1,binding=20,rgba16f) uniform image2D composed;
layout(set=1,binding=21,rgba16f) uniform image2D validation;
layout(set=1,binding=22,rgba32f) uniform image2D finalWorld;
layout(set=1,binding=23,rgba32f) uniform image2D skyColor;
#ifdef SIGMA_PASS
layout(set=1,binding=26,r16f) uniform image2D shadowPenumbra;
layout(set=1,binding=27,rgba8) uniform image2D shadowInput;
layout(set=1,binding=28,rgba8) uniform image2D shadowOutput;
#endif
#ifdef LOCAL_PASS
layout(set=1,binding=29,rgba16f) uniform image2D localInput;
layout(set=1,binding=30,rgba16f) uniform image2D localOutput;
#endif
#ifdef SPLIT_LOCAL_PASS
layout(set=1,binding=31,rgba16f) uniform image2D emissiveInput;
layout(set=1,binding=32,rgba16f) uniform image2D emissiveOutput;
#endif
layout(set=1,binding=25,std140) uniform TemporalFrame {
  mat4 view;
  mat4 previousView;
  mat4 previousCamera;
  mat4 previousInverseCamera;
  mat4 inverseCamera;
  vec4 jitter;
  vec4 hitDistance;
  uvec4 flags;
  uvec4 target;
  vec4 timing;
  vec4 sun;
  uvec4 shadow;
  uvec4 local;
  uvec4 families;
} temporal;
vec3 safeRadiance(vec3 v) { return any(isnan(v))||any(isinf(v))?vec3(0):clamp(v,vec3(0),vec3(65000)); }
vec2 encodeNormal(vec3 n) {
  n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-10);
  return n.z>=0?n.xy:(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
}
vec3 decodeNormal(vec2 e) {
  vec3 n=vec3(e,1-abs(e.x)-abs(e.y));float t=clamp(-n.z,0,1);
  n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));return normalize(n);
}
