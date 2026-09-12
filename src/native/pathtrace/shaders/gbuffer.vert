#version 460
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
#include "debug.glsl"
layout(push_constant) uniform Draw {
  mat4 model;
  vec2 jitter;
  uint surfaceIndex;
  uint stableDepth;
} draw;
layout(location=0) out vec3 position;
layout(location=1) flat out vec3 normal;
layout(location=2) out vec2 uv;
layout(location=3) out vec4 color;
layout(location=4) out vec3 previousPosition;
layout(location=5) flat out float triangleIdentity;
void main() {
  Surface s=surfaces[draw.surfaceIndex];
  uint index=s.indices.i[gl_VertexIndex];
  Vertex v=s.vertices.v[index];
  position=(draw.model*vec4(v.position.xyz,1)).xyz;
  uint triangle=uint(gl_VertexIndex)/3*3;
  triangleIdentity=frame.splitLocal.w==3?debugTriangle(s.debugIdentity,triangle/3):0;
  vec3 a=s.vertices.v[s.indices.i[triangle]].position.xyz;
  vec3 b=s.vertices.v[s.indices.i[triangle+1]].position.xyz;
  vec3 c=s.vertices.v[s.indices.i[triangle+2]].position.xyz;
  // Match ray-hit geometric normals, including mirrored and skinned triangles.
  normal=cross(mat3(draw.model)*(b-a),mat3(draw.model)*(c-a));
  uv=v.uv.xy;color=v.color;previousPosition=position;
  if(s.previousVertices!=0) {
    Vertices previous=Vertices(s.previousVertices);
    previousPosition=(s.previousModel*vec4(previous.v[index].position.xyz,1)).xyz;
  }
  gl_Position=frame.camera*vec4(position,1);
  gl_Position.xy-=2*draw.jitter/vec2(frame.target.xy)*gl_Position.w;
}
