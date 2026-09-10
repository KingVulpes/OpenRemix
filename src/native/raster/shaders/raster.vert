#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 vertexColor;
layout(push_constant) uniform Draw {
  mat4 mvp;
  vec4 tint;
  vec4 uvTransform;
  uvec4 state;
  vec4 factor;
} draw;
void main() {
  gl_Position = draw.mvp * vec4(position, 1.0);
  uv = texcoord * draw.uvTransform.xy + draw.uvTransform.zw;
  vertexColor = color;
}
