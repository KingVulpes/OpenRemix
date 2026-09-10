#version 460
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(push_constant) uniform Draw {
  mat4 model;
  vec2 jitter;
  uint surfaceIndex;
  uint stableDepth;
} draw;
layout(location=0) in vec3 position;
layout(location=1) flat in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 color;
layout(location=4) in vec3 previousPosition;
#ifdef STABLE_DEPTH
layout(location=0) out float outDepth;
#else
layout(location=0) out vec4 outPosition;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outAlbedo;
layout(location=3) out vec4 outPrevious;
#endif
void main() {
  Surface s=surfaces[draw.surfaceIndex];
  if((s.state.w&4)==0&&!gl_FrontFacing)discard;
  vec4 albedo=materialColor(s,uv,color);
  if(!alphaPass(s,albedo.a))discard;
#ifdef STABLE_DEPTH
  // Blended sheets do not occlude raster particles or coplanar feedback.
  if((s.state.w&2)!=0)discard;
  outDepth=gl_FragCoord.z;
#else
  vec2 ndc=(gl_FragCoord.xy+draw.jitter)/vec2(frame.target.xy)*2-1;
  vec4 nearPoint=frame.inverseCamera*vec4(ndc,0,1);
  vec3 direction=position-nearPoint.xyz/nearPoint.w;
  vec3 n=normalize(normal);
  if(dot(n,direction)>0)n=-n;
  outPosition=vec4(position,gl_FragCoord.z);
  // Zero is the cleared background, one opaque/cutout, two a blended sheet.
  outNormal=vec4(n,(s.state.w&2)!=0?2:1);
  outAlbedo=albedo;
  outPrevious=vec4(previousPosition,s.previousVertices!=0?1:0);
#endif
}
