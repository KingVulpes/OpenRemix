struct Vertex { vec4 position; vec4 uv; vec4 color; };
vec3 vertexNormal(Vertex v) { return vec3(v.position.w,v.uv.zw); }
vec3 shadingNormal(vec3 n,vec3 geometric,vec3 wo) {
  if(any(isnan(n))||any(isinf(n))||dot(n,n)<1e-12)return geometric;
  n=normalize(n);
  if(dot(n,geometric)<0)n=-n;
  return dot(n,wo)>1e-4?n:geometric;
}
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
  uint64_t triangleMap;
  Indices traceIndices;
  uint64_t previousVertices;
  uvec2 debugIdentity;
  mat4 previousModel;
  vec4 emission;
  vec4 pbr;
  uvec4 pbrTextures;
  // Fork material extensions (scene::MaterialDecode): flags, packed specular
  // metal parameters, albedo tint as halves (r|g<<16, b).
  uvec4 material;
};
// Shading reads the AS index order directly. Only debug identity needs the
// original triangle mapping; motion still addresses the original vertex array.
uint originalTriangle(Surface s,uint triangle) {
  return s.triangleMap==0?triangle:Indices(s.triangleMap).i[triangle];
}
layout(set=0,binding=2,std430) readonly buffer Surfaces { Surface surfaces[]; };
layout(set=0,binding=3,std140) uniform Frame {
  mat4 inverseCamera;
  mat4 camera;
  vec4 eye;
  vec4 lightDirection;
  vec4 lightRadiance;
  uvec4 config; // samples, indirect bounces, random sequence, accumulated frames
  uvec4 target; // width, height, structure mask, raster primary visibility
  vec4 jitter; // primary offset; z water reuse; w blue-noise sun/local bits
  vec4 skyRadiance; // linear uniform environment; w reserved
  uint64_t sharcHash, sharcAccumulation, sharcResolved, sharcCounters;
  uvec4 sharcConfig; // active, capacity, update downscale, debug view
  vec4 sharcGrid; // scene scale, level bias, accumulation frames, stale frames
  vec4 sharcPreviousCamera;
  vec4 sun;
  uint64_t lightNodes, lightEmitters;
  uvec4 localConfig; // active lights, samples, generation low, generation high + adaptive (bit 31) + guide spheres (bit 30)
  vec4 localScale; // radiance multiplier, reach, guiding requested, inverse reach squared
  uint64_t lightGuideRead, lightGuideWrite;
  uvec4 lightGuideConfig; // capacity, node count, inverse cell size (float bits), reserved
  uint64_t torchNodes, torchEmitters, emissiveNodes, emissiveEmitters;
  uvec4 splitLocal;
  uint64_t reuseRead, reuseWrite;
  uvec4 reuseConfig;
  uint64_t indirectResults, blueNoise;
  vec4 firefly; // outgoing local direct / indirect luminance limits; reserved
  uvec4 pathConfig; // adaptive termination, minimum completed bounces, sheet flags (1 traced indirect, 2 local lights), secondary ray mask
  vec4 domeTransform[3];
  uint64_t visibilityNodes, visibilityLeaves, visibilityTable, movedLights;
  uvec4 visibilityConfig; // table row words, moved light count, reserved
  vec4 visibilityProbe; // world units: x probe offset above the surface, y decal attachment distance; z texture footprint scale (render/output size)
  uint64_t restirCurrent, restirHistoryRead, restirHistoryWrite, restirRemap;
  uvec4 restirConfig; // candidates, spatial samples, temporal M cap, flags (1 initial visibility)
  vec4 restirParams; // spatial radius (pixels), emitter count, reserved
  uint64_t regirRead, regirWrite, regirSlots, regirReserved;
  uvec4 regirConfig; // cell capacity, slots per cell, cells per world unit (float bits), active
  uint64_t orcaTable, orcaReserved;
  uvec4 orcaConfig; // entry capacity, sparse tile size, voxel levels, active
  vec4 orcaParams; // smallest voxel (world units), level growth, minimum samples, reserved
  uvec4 cutConfig; // first cut coverage texture + 1 (0: nothing cuts), coverage textures
  uvec4 cutSlots; // four bits per mask group (1..32): its coverage texture (from the first) + 1, or 0
} frame;
layout(set=0,binding=6) uniform sampler2D textures[4096];
// remixapi_InstanceInfoMaskEXT CUT: the camera does not see a surface where
// its mask group's coverage is set (state.w bits 16-21: the group). Coverage
// is one output-resolution texture per cutting group, zero or one.
// Bit i: coverage texture i is set at uv (the unjittered screen position of
// the camera sample), so the hole's edge is sampled like a geometry edge.
uint cutCoverage(vec2 uv) {
  uint bits=0u;
  for(uint i=0u;i<frame.cutConfig.y;++i) {
    uint index=frame.cutConfig.x-1u+i;
    ivec2 size=textureSize(textures[nonuniformEXT(index)],0);
    if(texelFetch(textures[nonuniformEXT(index)],clamp(ivec2(floor(uv*vec2(size))),ivec2(0),size-1),0).r>0)bits|=1u<<i;
  }
  return bits;
}
// A surface with these state flags (Surface.state.w) is hidden from a camera
// sample with these coverage bits.
bool cutSurface(uint flags,uint bits) {
  uint group=(flags>>16)&63u;
  if(bits==0u||group==0u)return false;
  uint slot=(frame.cutSlots[(group-1u)>>3]>>(((group-1u)&7u)*4u))&15u;
  return slot!=0u&&((bits>>(slot-1u))&1u)!=0u;
}
// UV change per output pixel along two footprint axes at the surface being
// shaded (ray differentials, or dFdx/dFdy when rasterized). Zero selects the
// finest mip: visibility tests during traversal keep full-resolution coverage.
vec2 uvGradX=vec2(0),uvGradY=vec2(0);
// A material texture at mesh UV `uv`, filtered over the footprint.
vec4 materialTexel(Surface s,uint index,vec2 uv) {
  return textureGrad(textures[nonuniformEXT(index)],uv*s.uv.xy+s.uv.zw,uvGradX*s.uv.xy,uvGradY*s.uv.xy);
}
vec3 environmentRadiance(vec3 direction) {
  if(frame.skyRadiance.w<=0)return frame.skyRadiance.rgb;
  vec3 d=normalize(vec3(dot(frame.domeTransform[0].xyz,direction),dot(frame.domeTransform[1].xyz,direction),dot(frame.domeTransform[2].xyz,direction)));
  vec2 uv=vec2(atan(d.z,d.x)/(2*3.14159265359)+.5,acos(clamp(d.y,-1,1))/3.14159265359);
  return frame.skyRadiance.rgb*textureLod(textures[nonuniformEXT(uint(frame.skyRadiance.w)-1)],uv,0).rgb;
}
// Metalness from a spec/gloss map's specular level (red, gamma), as the
// dxvk-remix fork derives it (remixapi_MaterialInfoOpaqueSpecGlossEXT). p holds
// the fork's packed parameters; metal texels take spec * tint as their albedo.
float specularMetal(uint p,float spec,inout vec3 albedo) {
  float low=float(p&15u)/15,high=float((p>>4)&15u)/15,strength=float((p>>12)&15u)/15;
  uint lumCode=(p>>8)&15u,t=p>>16;
  vec3 tint=vec3(float(t&31u)/31,float((t>>5)&63u)/63,float((t>>11)&31u)/31);
  float level=t!=0u?spec*max(tint.r,max(tint.g,tint.b)):spec;
  float mask=clamp((level-low)/max(high-low,1.0/255),0,1);
  if(lumCode<15u) {
    // Code 0 has equal smoothstep edges; it cuts every non-black texel.
    float lumMax=pow(float(lumCode)/15,2.2),lum=dot(albedo,vec3(.2126,.7152,.0722));
    mask*=1-(lumMax>0?smoothstep(.25*lumMax,lumMax,lum):float(lum>0));
  }
  float metal=strength*mask;
  if(t!=0u)albedo=mix(albedo,pow(clamp(spec*tint,0,1),vec3(2.2)),metal);
  return metal;
}
// Perceptual roughness and metalness; albedo is the linear material colour.
// A smoothness map (spec/gloss extension) decodes as max(1 - smoothness,
// roughness constant), smoothness from green or red.
vec2 materialParameters(Surface s,vec2 uv,inout vec3 albedo) {
  float metallic=s.pbrTextures.y==0?s.pbr.y:materialTexel(s,s.pbrTextures.y-1,uv).r;
  if(s.pbrTextures.x==0)return clamp(vec2(s.pbr.x,metallic),0,1);
  vec4 map=materialTexel(s,s.pbrTextures.x-1,uv);
  uint flags=s.material.x;
  float roughness=(flags&1u)!=0?max(1-((flags&2u)!=0?map.g:map.r),s.pbr.x):map.r;
  if((flags&4u)!=0)metallic=max(metallic,specularMetal(s.material.y,map.r,albedo));
  return clamp(vec2(roughness,metallic),0,1);
}
mat3 materialBasis(vec3 n,vec3 e1,vec3 e2,vec2 u1,vec2 u2) {
  float det=u1.x*u2.y-u1.y*u2.x;
  vec3 t=abs(det)>1e-10?(e1*u2.y-e2*u1.y)/det:vec3(0);
  t-=n*dot(n,t);
  if(dot(t,t)<1e-12)t=cross(abs(n.z)<.999?vec3(0,0,1):vec3(0,1,0),n);
  t=normalize(t);
  vec3 b=cross(n,t);
  if(abs(det)>1e-10&&dot(b,(e2*u1.x-e1*u2.x)/det)<0)b=-b;
  return mat3(t,b,n);
}
vec3 materialNormal(Surface s,vec2 uv,mat3 basis,vec3 wo) {
  if(s.pbrTextures.z==0)return basis[2];
  vec3 local=materialTexel(s,s.pbrTextures.z-1,uv).xyz*2-1;
  if((s.pbrTextures.w&1)!=0)local.z=sqrt(max(0,1-dot(local.xy,local.xy)));
  if(s.pbrTextures.w==2) { local.xy=vec2(local.x+local.y,local.x-local.y)*.5; local.z=1-abs(local.x)-abs(local.y); }
  if((s.pbrTextures.w&4)!=0)local.y=-local.y;
  if(dot(local,local)<1e-8)return basis[2];
  vec3 n=normalize(basis*vec3(local.xy,max(local.z,.001)));
  // A backfacing mapped normal cannot define a reflective hemisphere for wo.
  return dot(n,wo)>1e-4?n:basis[2];
}
vec2 encodeGeometricNormal(vec3 n) {
  n/=abs(n.x)+abs(n.y)+abs(n.z);
  return n.z>=0?n.xy:(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
}
vec3 decodeGeometricNormal(vec2 e) {
  vec3 n=vec3(e,1-abs(e.x)-abs(e.y));
  if(n.z<0)n.xy=(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
  return normalize(n);
}
vec3 materialEmission(Surface s,vec2 uv) {
  if(!any(greaterThan(s.emission.rgb,vec3(0))))return vec3(0);
  return s.emission.rgb*(s.textureState.z==0?vec3(1):
      materialTexel(s,s.textureState.z-1,uv).rgb);
}
vec3 linearColor(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec4 arg(uint index, vec4 texel, vec4 vertex, vec4 factor) {
  return index == 1 ? texel : index == 2 ? vertex : index == 3 ? factor : vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) { return op == 1 ? a : op == 2 ? b : op == 6 ? min(a+b,vec4(1)) : min(a*b*(op==4||op==7?2:op==5?4:1),vec4(1)); }
// Captured alpha blending represents coverage: uncovered texels transmit the
// background unchanged, regardless of their RGB. Only authored thin-sheet
// materials (constant opacity or glass) retain the colored-transmission model.
vec3 transmissionColor(vec3 color, bool tinted) { return tinted ? color : vec3(1); }
// Candidate intersections need opacity only. In particular, do not linearize
// vertex RGB or evaluate the color combiner before knowing the closest hit.
// Bit 0: the combiner reads the texture; bit 1: it reads vertex alpha.
uint alphaOperands(Surface s) {
  uint op=s.state.x,first=(op>>8)&3,second=(op>>10)&3,combineOp=((op>>6)&3)|((op>>11)&4);
  uint a=combineOp==2?0u:first,b=combineOp==1?0u:second;
  return uint(a==1||b==1)|uint(a==2||b==2)<<1;
}
float materialAlpha(Surface s, vec2 uv, float vertex, bool sampled) {
  if ((s.state.w&1)!=0) return 1;
  uint op=s.state.x;
  float texel=sampled?textureLod(textures[nonuniformEXT(s.textureState.x)],uv*s.uv.xy+s.uv.zw,0).a:1;
  uint first=(op>>8)&3,second=(op>>10)&3;
  float a=first==1?texel:first==2?vertex:first==3?s.factor.a:1;
  float b=second==1?texel:second==2?vertex:second==3?s.factor.a:1;
  uint combineOp=((op>>6)&3)|((op>>11)&4);
  float alpha=(combineOp==1?a:combineOp==2?b:combineOp==6?min(a+b,1):min(a*b*(combineOp==4||combineOp==7?2:combineOp==5?4:1),1))*s.tint.a;
  if ((s.state.w&8)!=0) alpha*=.2;
  return clamp(alpha,0,1);
}
// remixapi_MaterialInfoOpaqueAlbedoTintEXT (fork semantics): a multiply on the
// texture's gamma values. An sRGB view samples linear, so the tint is linearized.
vec3 albedoTint(Surface s) {
  vec3 tint=vec3(unpackHalf2x16(s.material.z),unpackHalf2x16(s.material.w).x);
  return (s.material.x&16u)!=0?pow(tint,vec3(2.2)):tint;
}
vec4 materialColor(Surface s, vec2 uv, vec4 vertex) {
  vec4 texel = materialTexel(s, s.textureState.x, uv);
  if ((s.material.x&8u)!=0) texel.rgb*=albedoTint(s);
  vertex.rgb = linearColor(vertex.rgb);
  uint op = s.state.x;
  vec4 color = combine((op&3)|((op>>10)&4), arg((op>>2)&3,texel,vertex,s.factor), arg((op>>4)&3,texel,vertex,s.factor));
  vec4 alpha = combine(((op>>6)&3)|((op>>11)&4), arg((op>>8)&3,texel,vertex,s.factor), arg((op>>10)&3,texel,vertex,s.factor));
  vec4 result = vec4(color.rgb, alpha.a)*s.tint;
  if ((s.state.w&8)!=0) result.a*=0.2; // Basic thin tinted-sheet approximation.
  if ((s.state.w&1)!=0) result.a=1;
  return clamp(result,0,1);
}
bool decalSurface(Surface s) { return (s.state.w&64u)!=0; }
bool coverageBlended(Surface s) { return (s.state.w&34u)==2u; }
bool blendedAtAlpha(Surface s,float a) { return (s.state.w&2u)!=0&&a<1; }
bool alphaPass(Surface s, float a) {
  float ref=uintBitsToFloat(s.state.z); uint op=s.state.y;
  return op==7 || (op==1&&a<ref) || (op==2&&a==ref) || (op==3&&a<=ref) ||
    (op==4&&a>ref) || (op==5&&a!=ref) || (op==6&&a>=ref);
}

// Zero-coverage texels do not represent an interaction. A tinted sheet still
// transmits its color at zero opacity and must remain in the layer traversal.
bool visibleAlpha(Surface s,float a) {
  return alphaPass(s,a)&&(!coverageBlended(s)||a>0);
}
