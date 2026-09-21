// Experimental first-level cache: only fresh primary local lighting is stored.
// Never cache a path tail, emission, or sunlight. EON/GGX remain view dependent;
// close view/material agreement reduces error but does not make reuse unbiased.
struct ReuseEntry { vec4 position; vec4 normal; vec4 albedo; vec4 radiance; };
layout(buffer_reference,std430,buffer_reference_align=16) buffer ReuseBuffer {
  uvec4 counts;
  ReuseEntry entries[];
};
void storeReuse(ivec2 pixel,Hit hit,vec3 local) {
#if defined(TEMPORAL) && !defined(CONFIDENCE_PASS) && !defined(TRANSPARENCY_PASS)
  if(!enablePbrReuse||frame.reuseWrite==0)return;
  ReuseEntry entry;
  entry.position=entry.normal=entry.albedo=entry.radiance=vec4(0);
  if(hit.t<1e30&&hit.pbr&&!hit.blended&&hit.rm.x>=.6&&hit.rm.y<=.05) {
    entry.position=vec4(hit.p,hit.rm.x);
    entry.normal=vec4(hit.n,hit.rm.y);
    entry.albedo=vec4(hit.color.rgb,1);
    entry.radiance=vec4(local,1);
  }
  ReuseBuffer(frame.reuseWrite).entries[uint(pixel.y)*frame.target.x+uint(pixel.x)]=entry;
#endif
}
bool reuseLocal(Hit hit,out vec3 local) {
#if defined(TEMPORAL) && !defined(CONFIDENCE_PASS) && !defined(TRANSPARENCY_PASS) && !defined(SPLIT_LIGHTING_PASS)
  if(!enablePbrReuse||frame.reuseWrite==0||!hit.pbr||hit.blended||hit.rm.x<.6||hit.rm.y>.05)return false;
  if(frame.reuseConfig.y!=0)atomicAdd(ReuseBuffer(frame.reuseWrite).counts.x,1u);
  if(frame.reuseConfig.x==0)return false;
  vec4 clip=temporal.previousCamera*vec4(hit.p,1);
  if(clip.w<=0)return false;
  vec2 coord=(clip.xy/clip.w*.5+.5)*vec2(frame.target.xy)-temporal.jitter.zw;
  ivec2 pixel=ivec2(floor(coord));
  if(any(lessThan(pixel,ivec2(1)))||any(greaterThanEqual(pixel,ivec2(frame.target.xy)-1)))return false;
  ReuseEntry old=ReuseBuffer(frame.reuseRead).entries[uint(pixel.y)*frame.target.x+uint(pixel.x)];
  if(old.albedo.w==0||distance(old.position.xyz,hit.p)>.025||dot(old.normal.xyz,hit.n)<.9995||
     dot(old.normal.xyz,hit.ng)<.98||abs(old.position.w-hit.rm.x)>.005||abs(old.normal.w-hit.rm.y)>.005||
     any(greaterThan(abs(old.albedo.rgb-hit.color.rgb),vec3(.01)))) {
    if(frame.reuseConfig.y!=0)atomicAdd(ReuseBuffer(frame.reuseWrite).counts.z,1u);
    return false;
  }
  vec2 ndc=(vec2(pixel)+.5+temporal.jitter.zw)/vec2(frame.target.xy)*2-1;
  vec4 nearPoint=temporal.previousInverseCamera*vec4(ndc,0,1);
  vec4 farPoint=temporal.previousInverseCamera*vec4(ndc,.99999,1);
  vec3 previousView=normalize(nearPoint.xyz/nearPoint.w-farPoint.xyz/farPoint.w);
  if(dot(previousView,hit.wo)<.995) {
    if(frame.reuseConfig.y!=0)atomicAdd(ReuseBuffer(frame.reuseWrite).counts.w,1u);
    return false;
  }
  local=old.radiance.rgb;
  if(any(isnan(local))||any(isinf(local))||any(lessThan(local,vec3(0))))return false;
  if(frame.reuseConfig.y!=0)atomicAdd(ReuseBuffer(frame.reuseWrite).counts.y,1u);
  return true;
#else
  return false;
#endif
}
