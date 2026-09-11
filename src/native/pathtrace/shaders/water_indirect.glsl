// Reconstruct only low-frequency diffuse irradiance inside a compatible 2x2
// patch. Tint, opacity, direct light, silhouettes and background GI stay per
// pixel. Alternating diagonals visit all four sites; TAA retains its normal input.
// This spatial approximation is disabled for raw/progressive reference modes.
shared vec4 waterPositions[64];
shared vec4 waterNormals[64];
shared vec3 waterIrradiance[64];
vec4 waterIndirect(Hit primary,bool eligible,ivec2 pixel) {
  uint lane=gl_LocalInvocationIndex;
  waterPositions[lane]=vec4(primary.p,eligible?1:0);
  waterNormals[lane]=vec4(primary.n,0);
  waterIrradiance[lane]=vec3(0);
  barrier();
  uint base=(gl_LocalInvocationID.y&~1u)*8+(gl_LocalInvocationID.x&~1u);
  vec3 p=waterPositions[base].xyz,n=waterNormals[base].xyz;
  bool compatible=true;
  for(uint i=0;i<4;++i) {
    uint other=base+(i&1u)+(i>>1u)*8;
    vec3 delta=waterPositions[other].xyz-p;
    compatible=compatible&&waterPositions[other].w!=0&&dot(waterNormals[other].xyz,n)>.9999&&
               abs(dot(delta,n))<.0001&&dot(delta,delta)<.0625;
  }
  uint phase=(frame.config.z+hash(uint(pixel.x/2)+uint(pixel.y/2)*((frame.target.x+1)/2)))&1u;
  uint source=base+phase,second=base+9-phase;
  if(compatible&&(lane==source||lane==second)) {
    vec3 direction;float distance;
    waterIrradiance[lane]=continuation(primary,direction,distance);
  }
  barrier();
  if(!compatible)return vec4(0);
  // Retain the traced signal at its own pixel. Only the two missing sites use
  // the diagonal mean, preserving spatial variance for TAA's neighborhood clip.
  return vec4(lane==source||lane==second?waterIrradiance[lane]:
              (waterIrradiance[source]+waterIrradiance[second])*.5,1);
}
