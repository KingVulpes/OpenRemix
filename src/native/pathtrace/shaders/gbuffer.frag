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
  vec4 reverseDepth;
} draw;
layout(location=0) in vec3 position;
layout(location=1) flat in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 color;
layout(location=4) in vec3 previousPosition;
layout(location=5) flat in float triangleIdentity;
layout(location=6) flat in vec3 tangent;
layout(location=7) flat in vec3 bitangent;
layout(location=8) in vec3 authoredNormal;
#ifdef STABLE_DEPTH
layout(location=0) out float outDepth;
#else
layout(location=0) out vec4 outPosition;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outAlbedo;
layout(location=3) out vec4 outPrevious;
layout(location=4) out vec4 outEmission;
layout(location=5) out vec4 outPbr;
#endif
void main() {
  Surface s=surfaces[draw.surfaceIndex];
  // Match world-space ray facing independently of projection handedness.
  // The geometric normal includes mirrored instance transforms.
  vec4 projected=frame.camera*vec4(position,1);
  vec4 nearPoint=frame.inverseCamera*vec4(projected.xy/projected.w,0,1);
  vec3 direction=position-nearPoint.xyz/nearPoint.w;
  if((s.state.w&4)==0&&dot(normal,direction)<=0)discard;
  vec4 albedo=materialColor(s,uv,color);
  if(!alphaPass(s,albedo.a))discard;
#ifdef STABLE_DEPTH
  // Blended sheets do not occlude raster particles or coplanar feedback.
  if((s.state.w&2)!=0)discard;
  outDepth=1-gl_FragCoord.z;
#else
  vec3 n=normalize(normal);
  if(dot(n,direction)>0)n=-n;
  outPosition=vec4(position,1-gl_FragCoord.z);
  // Zero is the cleared background, one opaque/cutout, two a blended sheet.
  vec3 smoothNormal=shadingNormal(authoredNormal,n,normalize(-direction));
  vec3 t=tangent-smoothNormal*dot(smoothNormal,tangent);
  if(dot(t,t)<1e-12)t=cross(abs(smoothNormal.z)<.999?vec3(0,0,1):vec3(0,1,0),smoothNormal);
  t=normalize(t);
  vec3 b=cross(smoothNormal,t);
  if(dot(b,bitangent)<0)b=-b;
  outNormal=vec4(materialNormal(s,uv,mat3(t,b,smoothNormal),normalize(-direction)),(s.state.w&2)!=0?2:1);
  vec2 rm=materialParameters(s,uv);
  outPbr=vec4(s.pbr.z!=0?rm.x:-1,rm.y,encodeGeometricNormal(n));
  outAlbedo=albedo;
  outPrevious=vec4(previousPosition,s.previousVertices!=0?1:0);
  outEmission=vec4(materialEmission(s,uv),triangleIdentity);
#endif
}
