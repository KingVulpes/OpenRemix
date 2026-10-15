// Reconstruct only low-frequency diffuse irradiance inside a compatible 2x2
// patch. Tint, opacity, direct light, silhouettes and background GI stay per
// pixel. Alternating diagonals visit all four sites; TAA retains its normal input.
// This spatial approximation is disabled for raw/progressive reference modes.
// Two phases around the caller's own path tracing (its one continuation call
// site): waterPrepare names the two lanes that trace for the patch, waterShare
// hands every lane its share. Every invocation of the group reaches both.
shared vec4 waterPositions[64];
shared vec4 waterNormals[64];
shared vec4 waterIrradiance[64];
uint waterBase() { return (gl_LocalInvocationID.y&~1u)*8+(gl_LocalInvocationID.x&~1u); }
uint waterPhase(ivec2 pixel) { return (frame.config.z+hash(uint(pixel.x/2)+uint(pixel.y/2)*((frame.target.x+1)/2)))&1u; }
bool waterPrepare(Hit primary,bool eligible,ivec2 pixel,out bool compatible) {
  uint lane=gl_LocalInvocationIndex;
  waterPositions[lane]=vec4(primary.p,eligible&&!primary.pbr?1:0);
  waterNormals[lane]=vec4(primary.n,0);
  barrier();
  uint base=waterBase();
  vec3 p=waterPositions[base].xyz,n=waterNormals[base].xyz;
  compatible=true;
  for(uint i=0;i<4;++i) {
    uint other=base+(i&1u)+(i>>1u)*8;
    vec3 delta=waterPositions[other].xyz-p;
    compatible=compatible&&waterPositions[other].w!=0&&dot(waterNormals[other].xyz,n)>.9999&&
               abs(dot(delta,n))<.0001&&dot(delta,delta)<.0625;
  }
  uint phase=waterPhase(pixel),source=base+phase,second=base+9-phase;
  return compatible&&(lane==source||lane==second);
}
// traced: the tracing lanes' path radiance and distance (zero elsewhere).
vec4 waterShare(ivec2 pixel,bool compatible,vec4 traced,out float sharedDistance) {
  sharedDistance=0;
  uint lane=gl_LocalInvocationIndex;
  waterIrradiance[lane]=traced;
  barrier();
  if(!compatible)return vec4(0);
  // Retain the traced signal at its own pixel. Only the two missing sites use
  // the diagonal mean, preserving spatial variance for TAA's neighborhood clip.
  uint base=waterBase(),phase=waterPhase(pixel),source=base+phase,second=base+9-phase;
  vec4 signal=lane==source||lane==second?waterIrradiance[lane]:
              (waterIrradiance[source]+waterIrradiance[second])*.5;
  sharedDistance=signal.w;
  return vec4(signal.rgb,1);
}
