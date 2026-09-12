#ifndef REMIX_DEBUG_GLSL
#define REMIX_DEBUG_GLSL
// Keep the view numbers in sync with the F12 selector and debugView config.
uint debugHash(uint x) {
  x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);
}
float debugTriangle(uvec2 identity,uint triangle) {
  // Exact in the existing RGBA16F emission attachment. Zero denotes sky.
  return float(1u+(debugHash(identity.x^debugHash(identity.y)^debugHash(triangle))&1023u));
}
vec3 debugPalette(float identity) {
  uint h=debugHash(uint(identity));
  return .1+.8*vec3(h&255u,(h>>8)&255u,(h>>16)&255u)/255;
}
vec3 debugUnavailable(ivec2 p) {
  return ((p.x/16+p.y/16)&1)==0?vec3(.35,0,.35):vec3(.035);
}
#endif
