#version 450
layout(set=0,binding=4,rgba32f) readonly uniform image2D worldColor;
layout(set=0,binding=5,rgba32ui) readonly uniform uimage2D backgroundTransmission;
layout(push_constant) uniform Composite { uint additive; uint encodeOutput; } mode;
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec4 world=imageLoad(worldColor,pixel);
  vec3 color=mode.additive!=0 ? world.rgb : uintBitsToFloat(imageLoad(backgroundTransmission,pixel).rgb);
  if (mode.encodeOutput!=0 && mode.additive!=0) {
    color=max(color,vec3(0));
    color=mix(color*12.92,1.055*pow(color,vec3(1.0/2.4))-0.055,greaterThan(color,vec3(0.0031308)));
  }
  result=vec4(color,1);
  gl_FragDepth=world.a;
}
