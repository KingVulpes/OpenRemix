#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/display.glsl"
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
layout(push_constant) uniform Settings { uint encode; uint prism; uint gamma22; uint hardwareSrgb; } settings;
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec3 color=imageLoad(upscaledColor,pixel).rgb;
  color=displayColor(color,settings.prism,settings.gamma22,settings.encode,settings.hardwareSrgb);
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
