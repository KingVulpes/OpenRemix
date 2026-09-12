#version 450
#extension GL_GOOGLE_include_directive : require
#include "display.glsl"
layout(set=0,binding=4,rgba32f) readonly uniform image2D worldColor;
layout(set=0,binding=5,rgba32ui) readonly uniform uimage2D backgroundTransmission;
layout(push_constant) uniform Composite { uint additive; uint encodeOutput; uint prism; uint gamma22; uint hardwareSrgb; uint width; uint height; } mode;
layout(location=0) out vec4 result;
void main() {
  // Diagnostics use unfiltered internal pixels even while FSR remains active.
  ivec2 pixel=ivec2(gl_FragCoord.xy*vec2(imageSize(worldColor))/vec2(mode.width,mode.height));
  vec4 world=imageLoad(worldColor,pixel);
  vec3 color=mode.additive!=0 ? world.rgb : uintBitsToFloat(imageLoad(backgroundTransmission,pixel).rgb);
  if (mode.additive!=0)
    color=displayColor(color,mode.prism,mode.gamma22,mode.encodeOutput,mode.hardwareSrgb);
  result=vec4(color,1);
  gl_FragDepth=world.a;
}
