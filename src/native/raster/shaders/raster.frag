#version 450
layout(location=0) in vec2 uv;
layout(location=1) in vec4 vertexColor;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D albedo;
layout(push_constant) uniform Draw {
  mat4 mvp;
  vec4 tint;
  vec4 uvTransform;
  uvec4 state;
  vec4 factor;
} draw;
vec3 linearColor(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec4 arg(uint index, vec4 texel, vec4 vertex) {
  if (index == 1) return texel;
  if (index == 2) return vertex;
  if (index == 3) return draw.factor;
  return vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) {
  return op == 1 ? a : op == 2 ? b : op == 6 ? min(a+b,vec4(1)) : min(a*b*(op==4||op==7?2:op==5?4:1),vec4(1));
}
void main() {
  vec4 texel = texture(albedo, uv);
  // remixapi_MaterialInfoOpaqueAlbedoTintEXT (scene::MaterialDecode): half
  // tints in the high words of state.x/y/w. A multiply on the texture's gamma
  // values; an sRGB view samples linear, so the tint is linearized to match.
  if ((draw.state.w & 4) != 0) {
    vec3 tint = vec3(unpackHalf2x16(draw.state.x >> 16).x, unpackHalf2x16(draw.state.y >> 16).x,
                     unpackHalf2x16(draw.state.w >> 16).x);
    texel.rgb *= (draw.state.w & 8) != 0 ? pow(tint, vec3(2.2)) : tint;
  }
  vec4 vertex = vec4(linearColor(vertexColor.rgb), vertexColor.a);
  uint s = draw.state.x;
  vec4 a = combine((s & 3) | ((s >> 10) & 4), arg((s >> 2) & 3, texel, vertex), arg((s >> 4) & 3, texel, vertex));
  vec4 b = combine(((s >> 6) & 3) | ((s >> 11) & 4), arg((s >> 8) & 3, texel, vertex), arg((s >> 10) & 3, texel, vertex));
  result = vec4(a.rgb, b.a) * draw.tint;
  if ((draw.state.w & 1) != 0) result.a = 1;
  float ref = uintBitsToFloat(draw.state.z);
  uint op = draw.state.y & 0xffffu;
  bool pass = op == 7 || (op == 1 && result.a < ref) || (op == 2 && result.a == ref) ||
    (op == 3 && result.a <= ref) || (op == 4 && result.a > ref) ||
    (op == 5 && result.a != ref) || (op == 6 && result.a >= ref);
  if (!pass) discard;
#ifdef HALO_MASK
  // Halo silhouette (halo.frag): coverage only, after the material's alpha test.
  result = vec4(1);
#else
  // UNORM-only swapchains need explicit output encoding. Their fixed-function
  // blending operates in display space; SRGB targets retain linear blending.
  if ((draw.state.w & 2) != 0) {
    vec3 c = max(result.rgb, vec3(0));
    result.rgb = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
                     greaterThan(c, vec3(0.0031308)));
  }
#endif
}
