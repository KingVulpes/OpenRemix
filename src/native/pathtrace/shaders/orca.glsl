// ORCA (Online Radiance Cache Acceleration; Greenberg, SIGGRAPH 2026
// Advances): a radiance cache rebuilt from scratch every frame. A sparse pass
// traces full indirect paths from one pixel per tile and stores the radiance
// leaving each path's first hit into a hash map at every voxel size
// (Binder 2018 hashing with a checksum and linear probing). Every other path
// reads its first hit from the smallest voxel holding enough samples instead
// of tracing on: one ray per pixel and no history, so lighting changes appear
// the frame they happen.
//
// Voxels are keyed by size level, position and a bin: the side of the
// surface the ray hit (6 faces; keeps the two sides of a thin wall apart) and,
// on glossy surfaces only, a coarse incoming direction (4x4 octahedral).
struct OrcaEntry { uint64_t r,g,b; uint count,key; };
layout(buffer_reference,std430,buffer_reference_align=8) buffer OrcaTable { OrcaEntry data[]; };
const uint orcaProbes=8u;
const float orcaFixedPoint=4096.0; // radiance scale of the 64-bit sums
uint orcaBin(Hit hit,vec3 direction) {
  vec3 n=dot(hit.ng,direction)<0?hit.ng:-hit.ng;
  vec3 a=abs(n);
  uint axis=a.x>a.y?(a.x>a.z?0u:2u):(a.y>a.z?1u:2u);
  uint bin=axis*2u+(n[axis]>0?1u:0u);
  if(hit.pbr&&hit.rm.x<.5) {
    vec2 e=encodeGeometricNormal(direction)*.5+.5;
    uvec2 q=min(uvec2(e*4),uvec2(3));
    bin+=6u*(1u+q.x+4u*q.y);
  }
  return bin;
}
float orcaCellSize(uint level) { return frame.orcaParams.x*pow(frame.orcaParams.y,float(level)); }
// Address and nonzero checksum of a voxel (two independent hashes).
uvec2 orcaHash(ivec3 cell,uint level,uint bin) {
  uint a=hash(uint(cell.x)^hash(uint(cell.y)+0x9e3779b9u)^hash(uint(cell.z)+0x85ebca6bu)^hash(level*131u+bin*7919u));
  uint b=hash(a^0x5bd1e995u^hash(uint(cell.x)*73856093u^uint(cell.y)*19349663u^uint(cell.z)*83492791u^(level<<24)^bin));
  return uvec2(a,max(b,1u));
}
#ifdef ORCA_SPARSE_PASS
void orcaInsert(Hit hit,vec3 direction,vec3 radiance) {
  if(any(isnan(radiance))||any(isinf(radiance)))return;
  OrcaTable table=OrcaTable(frame.orcaTable);
  uint mask=frame.orcaConfig.x-1,bin=orcaBin(hit,direction);
  u64vec3 fixedRadiance=u64vec3(min(max(radiance,vec3(0)),vec3(65000))*orcaFixedPoint);
  for(uint level=0;level<frame.orcaConfig.z;++level) {
    ivec3 cell=ivec3(floor(hit.p/orcaCellSize(level)));
    uvec2 h=orcaHash(cell,level,bin);
    for(uint i=0;i<orcaProbes;++i) {
      uint slot=(h.x+i)&mask;
      uint owner=atomicCompSwap(table.data[slot].key,0u,h.y);
      if(owner!=0u&&owner!=h.y)continue;
      atomicAdd(table.data[slot].r,fixedRadiance.r);
      atomicAdd(table.data[slot].g,fixedRadiance.g);
      atomicAdd(table.data[slot].b,fixedRadiance.b);
      atomicAdd(table.data[slot].count,1u);
      break;
    }
  }
}
#endif
// Mean radiance leaving the hit toward the ray, from the smallest voxel with
// at least the configured sample count; the voxel grid is dithered per
// lookup so voxel edges do not show.
bool orcaLookup(Hit hit,vec3 direction,out vec3 radiance) {
  radiance=vec3(0);
  OrcaTable table=OrcaTable(frame.orcaTable);
  uint mask=frame.orcaConfig.x-1,bin=orcaBin(hit,direction);
  uint minimum=uint(frame.orcaParams.z);
  for(uint level=0;level<frame.orcaConfig.z;++level) {
    vec3 dither=vec3(random(),random(),random())-.5;
    ivec3 cell=ivec3(floor(hit.p/orcaCellSize(level)+dither));
    uvec2 h=orcaHash(cell,level,bin);
    for(uint i=0;i<orcaProbes;++i) {
      uint slot=(h.x+i)&mask;
      uint key=table.data[slot].key;
      if(key==0u)break;
      if(key!=h.y)continue;
      uint count=table.data[slot].count;
      if(count<minimum)break;
      radiance=vec3(table.data[slot].r,table.data[slot].g,table.data[slot].b)/(orcaFixedPoint*float(count));
      return true;
    }
  }
  return false;
}
