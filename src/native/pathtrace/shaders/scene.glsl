struct Vertex { vec4 position; vec4 uv; vec4 color; };
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer Vertices { Vertex v[]; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer Indices { uint i[]; };
struct Surface {
  Vertices vertices;
  Indices indices;
  vec4 tint;
  vec4 uv;
  vec4 factor;
  uvec4 state; // texture operations, compare, reference bits, flags
  uvec4 textureState; // texture index
  uint64_t previousVertices;
  uvec2 debugIdentity;
  mat4 previousModel;
  vec4 emission;
};
layout(set=0,binding=2,std430) readonly buffer Surfaces { Surface surfaces[]; };
layout(set=0,binding=3,std140) uniform Frame {
  mat4 inverseCamera;
  mat4 camera;
  vec4 eye;
  vec4 lightDirection;
  vec4 lightRadiance;
  uvec4 config; // samples, indirect bounces, random sequence, accumulated frames
  uvec4 target; // width, height, structure mask, raster primary visibility
  vec4 jitter; // primary sample offset; z water indirect reuse; w reserved
  vec4 skyRadiance; // linear uniform environment; w reserved
  uint64_t sharcHash, sharcAccumulation, sharcResolved, sharcCounters;
  uvec4 sharcConfig; // active, capacity, update downscale, debug view
  vec4 sharcGrid; // scene scale, level bias, accumulation frames, stale frames
  vec4 sharcPreviousCamera;
  vec4 sun;
  uint64_t lightNodes, lightEmitters;
  uvec4 localConfig; // active lights, samples, generation low, generation high + adaptive bit
  vec4 localScale; // radiance multiplier, reach, guiding requested, inverse reach squared
  uint64_t lightGuideRead, lightGuideWrite;
  uvec4 lightGuideConfig; // capacity, node count, reserved
  uint64_t torchNodes, torchEmitters, emissiveNodes, emissiveEmitters;
  uvec4 splitLocal;
} frame;
layout(set=0,binding=6) uniform sampler2D textures[512];
vec3 materialEmission(Surface s,vec2 uv) {
  if(!any(greaterThan(s.emission.rgb,vec3(0))))return vec3(0);
  return s.emission.rgb*(s.textureState.z==0?vec3(1):
      textureLod(textures[nonuniformEXT(s.textureState.z-1)],uv*s.uv.xy+s.uv.zw,0).rgb);
}
vec3 linearColor(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec4 arg(uint index, vec4 texel, vec4 vertex, vec4 factor) {
  return index == 1 ? texel : index == 2 ? vertex : index == 3 ? factor : vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) { return op == 1 ? a : op == 2 ? b : a * b; }
// Candidate intersections need opacity only. In particular, do not linearize
// vertex RGB or evaluate the color combiner before knowing the closest hit.
float materialAlpha(Surface s, vec2 uv, float vertex) {
  if ((s.state.w&1)!=0) return 1;
  uint op=s.state.x;
  float texel=textureLod(textures[nonuniformEXT(s.textureState.x)],uv*s.uv.xy+s.uv.zw,0).a;
  uint first=(op>>8)&3,second=(op>>10)&3;
  float a=first==1?texel:first==2?vertex:first==3?s.factor.a:1;
  float b=second==1?texel:second==2?vertex:second==3?s.factor.a:1;
  uint combineOp=(op>>6)&3;
  float alpha=(combineOp==1?a:combineOp==2?b:a*b)*s.tint.a;
  if ((s.state.w&8)!=0) alpha*=.2;
  return clamp(alpha,0,1);
}
vec4 materialColor(Surface s, vec2 uv, vec4 vertex) {
  vec4 texel = textureLod(textures[nonuniformEXT(s.textureState.x)], uv * s.uv.xy + s.uv.zw, 0);
  vertex.rgb = linearColor(vertex.rgb);
  uint op = s.state.x;
  vec4 color = combine(op&3, arg((op>>2)&3,texel,vertex,s.factor), arg((op>>4)&3,texel,vertex,s.factor));
  vec4 alpha = combine((op>>6)&3, arg((op>>8)&3,texel,vertex,s.factor), arg((op>>10)&3,texel,vertex,s.factor));
  vec4 result = vec4(color.rgb, alpha.a)*s.tint;
  if ((s.state.w&8)!=0) result.a*=0.2; // Basic thin tinted-sheet approximation.
  if ((s.state.w&1)!=0) result.a=1;
  return clamp(result,0,1);
}
bool alphaPass(Surface s, float a) {
  float ref=uintBitsToFloat(s.state.z); uint op=s.state.y;
  return op==7 || (op==1&&a<ref) || (op==2&&a==ref) || (op==3&&a<=ref) ||
    (op==4&&a>ref) || (op==5&&a!=ref) || (op==6&&a>=ref);
}
