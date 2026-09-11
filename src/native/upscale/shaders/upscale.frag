#version 450
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
layout(push_constant) uniform Settings { uint encode; } settings;
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec3 color=imageLoad(upscaledColor,pixel).rgb;
  if(settings.encode!=0) {
    color=max(color,vec3(0));
    color=mix(color*12.92,1.055*pow(color,vec3(1.0/2.4))-0.055,greaterThan(color,vec3(0.0031308)));
  }
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
