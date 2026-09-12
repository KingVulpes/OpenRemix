// Previous-frame world-cell feedback changes only the proposal. Every selected
// light gets a fresh visibility ray and the complete mixture PDF. The read bank
// stays immutable during the frame, including across all samples at a pixel.
layout(buffer_reference,std430,buffer_reference_align=16) buffer LightGuideCache { uvec4 data[]; };
float lightSplit(float a,float b) {
  float scale=max(a,b);
  return a==0?0:b==0?1:clamp((a/scale)/(a/scale+b/scale),1e-6,1-1e-6);
}
uint lightGuideFace(vec3 n) {
  vec3 a=abs(n);
  uint axis=a.x>a.y?(a.x>a.z?0u:2u):(a.y>a.z?1u:2u);
  return axis*2u+(n[axis]>0?1u:0u);
}
uint lightGuideCellKey(vec3 cell,uint face) {
  // Canonicalize signed zero before hashing integer-valued float coordinates.
  uvec3 bits=mix(floatBitsToUint(cell),uvec3(0),equal(cell,vec3(0)));
  return max(1u,hash(hash(bits.x)^hash(bits.y+0x9e3779b9u)^hash(bits.z+face*0x85ebca6bu)));
}
uint lightGuideKey(Hit hit) {
  // Grid-aligned walls sit exactly on cell boundaries. Interpolation and
  // intersection rounding can place the same plane on either side, changing
  // a whole wall's proposal under camera motion. Use the same surface-safe
  // position as the visibility ray for the guide lookup only; physical light
  // evaluation and its PDF still use the original hit position.
  return lightGuideCellKey(floor(offsetRay(hit.p,hit.n)*.5),lightGuideFace(hit.n));
}
uint lightGuideQueryKey(Hit hit,uint state) {
  uint face=lightGuideFace(hit.n),axis=face/2;
  vec3 grid=offsetRay(hit.p,hit.n)*.5,cell=floor(grid);
  // Point lookup changes the entire proposal at two-block cell boundaries.
  // Different proposal variances become colored patches after tonemapping
  // and temporal clipping, even though their linear expectations agree.
  // Stochastically interpolate neighboring cell centers in the tangent plane.
  // Keep the normal-axis layer exact; never interpolate across parallel layers.
  // This random dimension is independent of light selection and transport.
  uint lookupState=hash(state^0xb5297a4du);
  for(uint i=1;i<=2;++i) {
    uint tangent=(axis+i)%3;
    float coordinate=grid[tangent]-.5;
    cell[tangent]=floor(coordinate)+float(lightRandom(lookupState)<fract(coordinate));
  }
  return lightGuideCellKey(cell,face);
}
float lightGuideProposal(LightNodes nodes,Hit hit,uint state,out uint preferred,bool resampled) {
  preferred=0;
  if(frame.lightGuideRead==0)return 0;
  uint key=lightGuideQueryKey(hit,state);
  uvec4 previous=LightGuideCache(frame.lightGuideRead).data[key&(frame.lightGuideConfig.x-1)];
  if(previous.x!=key||previous.y==0||previous.z<4)return 0;
  float blocked=clamp(1-float(previous.w)/(255*float(previous.z)),0,1);
  // RIS winner visibility is a heuristic for whether guiding is useful, not
  // an estimate of base occlusion. Avoid concentrating proposals in fully
  // visible scenes, where whole-tree stratification already has low variance.
  float mixWeight=.75*(resampled?smoothstep(.02,.2,blocked):blocked);
  if(mixWeight<.05)return 0;
  preferred=(previous.y&0xfffffu)-1u;
  if(preferred>=frame.lightGuideConfig.y||nodes.data[preferred].links.w!=1||
     lightWeight(nodes.data[preferred],hit.p,hit.n)<=0)return 0;
  return mixWeight;
}
float lightBasePdf(LightNodes nodes,Hit hit,uint preferred) {
  // Preorder subtrees occupy contiguous node ranges; no extra emitter path or
  // parent buffer is needed to evaluate the base PDF of a cached leaf.
  uint index=0;float pdf=1;
  while(nodes.data[index].links.w>1) {
    uvec4 links=nodes.data[index].links;
    float a=lightWeight(nodes.data[links.x],hit.p,hit.n),b=lightWeight(nodes.data[links.y],hit.p,hit.n);
    if(a==0&&b==0)return 0;
    float split=lightSplit(a,b);
    bool right=preferred>=links.y;
    pdf*=right?1-split:split;
    index=right?links.y:links.x;
  }
  return index==preferred?pdf:0;
}
void trainLightGuide(uint key,uint leaf,bool guided,vec3 visibility,vec3 physical,bool resampled) {
  LightGuideCache next=LightGuideCache(frame.lightGuideWrite);
  uint capacity=frame.lightGuideConfig.x;
  float visible=clamp(dot(visibility,vec3(.2126,.7152,.0722)),0,1);
  atomicAdd(next.data[capacity].x,1u);
  if(guided)atomicAdd(next.data[capacity].y,1u);
  if(visible>0)atomicAdd(next.data[capacity].z,1u);
  uint slot=key&(capacity-1),owner=atomicCompSwap(next.data[slot].x,0u,key);
  if(owner!=0&&owner!=key)return; // A collision loses guidance, never energy.
  // Carry a bounded amount of evidence into an observed cell. With only one
  // frame of base feedback, enabling the guide removes most base observations
  // and can disable the same guide on the next frame. Initialize through
  // atomics: other trainers may already be adding to this newly claimed slot.
  if(owner==0&&frame.lightGuideRead!=0) {
    uvec4 previous=LightGuideCache(frame.lightGuideRead).data[slot];
    if(previous.x==key) {
      uint retained=min(previous.z,resampled?256u:32u);
      atomicAdd(next.data[slot].z,retained);
      atomicAdd(next.data[slot].w,previous.z>0?uint(round(float(previous.w)*float(retained)/float(previous.z))):0u);
      // Decay the old contribution score, so a newly visible emitter can
      // replace it; cached visibility never substitutes for a shadow ray.
      uint score=previous.y>>20;
      if(score>1)atomicMax(next.data[slot].y,((score-1u)<<20)|(previous.y&0xfffffu));
    }
  }
  // Ordinary guiding estimates the blocked fraction only from base draws.
  // RIS instead counts winner observations for confidence; its bounded mixture
  // never interprets these visibility counts as base-distribution evidence.
  if(!guided||resampled) {
    atomicAdd(next.data[slot].z,1u);
    atomicAdd(next.data[slot].w,uint(round(visible*255)));
  }
  float contribution=dot(physical*visibility,vec3(.2126,.7152,.0722));
  if(contribution>0) {
    uint score=min(floatBitsToUint(contribution)>>20,0xfffu);
    atomicMax(next.data[slot].y,(score<<20)|(leaf+1u));
  }
}
