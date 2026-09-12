// Analytic sphere next-event estimation, RT Gems I chapter 18. The selection
// PDF and solid-angle PDF both belong in the estimator. No light-count ray loop.
struct LightEmitter { vec4 positionRadius; vec4 radiance; vec4 corners[3]; };
struct LightNode { vec4 minimumPower; vec4 maximum; uvec4 links; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightEmitters { LightEmitter data[]; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightNodes { LightNode data[]; };
float lightRandom(inout uint state) {
  state=hash(state+0x9e3779b9u);return float(state>>8)*(1.0/16777216.0);
}
float localFalloff(float distance2) {
  float reach=frame.localScale.y;
  if(reach<=0)return 1;
  // Preserve the inner 75%; a C1 fade reaches exactly zero at the bound.
  float t=clamp((distance2*frame.localScale.w-.5625)*(16.0/7.0),0,1);
  return (1-t)*(1-t)*(1+2*t);
}
float lightWeight(LightNode node,vec3 p,vec3 n) {
  vec3 halfSize=.5*(node.maximum.xyz-node.minimumPower.xyz);
  vec3 delta=.5*(node.maximum.xyz+node.minimumPower.xyz)-p;
  float radius2=dot(halfSize,halfSize);
  if(dot(delta,n)+sqrt(radius2)<=0)return 0;
  vec3 nearest=max(abs(delta)-halfSize,vec3(0));
  float cosine=min(1,(dot(delta,n)+sqrt(radius2))*inversesqrt(max(max(dot(delta,delta),radius2),1e-12)));
  // Nearest-box distance keeps nearby lights competitive, but becomes zero
  // anywhere inside a cluster. Do not concentrate the entire cluster's flux
  // into its smallest emitter-sized distance: that starves sibling clusters
  // and produces enormous inverse-PDF samples. A tenth of the cluster radius
  // regularizes this importance estimate; emitter radiance is unchanged.
  float distance2=max(dot(nearest,nearest),.01*radius2);
  return localFalloff(dot(nearest,nearest))*node.minimumPower.w*cosine/max(max(distance2,node.maximum.w),1e-12);
}
// Visibility needs the nearest transmitting sheet, but any accepted opaque
// intersection blocks the whole segment. Avoid closest-hit shading (including
// motion/previous vertices) and texture fetches for unconditional opaque hits.
struct LightOccluder { float t; vec3 p; vec3 n; vec3 transmission; };
bool localOccluder(accelerationStructureEXT scene,vec3 origin,vec3 direction,inout LightOccluder hit) {
  rayQueryEXT query;
  rayQueryInitializeEXT(query,scene,gl_RayFlagsNoOpaqueEXT,255,origin,0,direction,hit.t);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false)];
    if((s.state.w&4)==0&&!rayQueryGetIntersectionFrontFaceEXT(query,false))continue;
    if(!acceptCandidate(s,rayQueryGetIntersectionPrimitiveIndexEXT(query,false),rayQueryGetIntersectionBarycentricsEXT(query,false)))continue;
    if((s.state.w&2)==0) {rayQueryTerminateEXT(query);return true;}
    rayQueryConfirmIntersectionEXT(query);
  }
  if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return false;
  hit.t=rayQueryGetIntersectionTEXT(query,true);
  Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+rayQueryGetIntersectionGeometryIndexEXT(query,true)];
  uint tri=rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3;
  vec3 a=s.vertices.v[s.indices.i[tri]].position.xyz;
  vec3 b=s.vertices.v[s.indices.i[tri+1]].position.xyz;
  vec3 c=s.vertices.v[s.indices.i[tri+2]].position.xyz;
  vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
  mat4x3 model=rayQueryGetIntersectionObjectToWorldEXT(query,true);
  hit.p=model*vec4(a+bary.x*(b-a)+bary.y*(c-a),1);
  hit.n=normalize(cross(model*vec4(b-a,0),model*vec4(c-a,0)));
  if(dot(hit.n,direction)>0)hit.n=-hit.n;
  vec4 color=albedo(s,tri/3,bary);
  hit.transmission=color.rgb*(1-color.a);
  return false;
}
vec3 localVisibility(vec3 origin,vec3 endpoint) {
  vec3 transmission=vec3(1);
  for(uint layer=0;layer<32;++layer) {
    vec3 delta=endpoint-origin;float distance=length(delta);
    if(distance<=1e-6)return transmission;
    vec3 direction=delta/distance;
    LightOccluder hit;hit.t=distance;
    if((frame.target.z&1)!=0&&localOccluder(staticScene,origin,direction,hit))return vec3(0);
    if((frame.target.z&2)!=0&&localOccluder(dynamicScene,origin,direction,hit))return vec3(0);
    if(hit.t==distance)return transmission;
    transmission*=hit.transmission;
    origin=offsetRay(hit.p,-hit.n);
  }
  return vec3(0);
}
#include "light_guiding.glsl"
// A sphere candidate includes solid-angle weighting, but not discrete selection.
struct LocalCandidate { vec3 physical; vec3 endpoint; float distance; };
LocalCandidate triangleCandidate(LightEmitter light,Hit hit,inout uint state) {
  LocalCandidate result;result.physical=vec3(0);result.endpoint=hit.p;result.distance=0;
  vec3 a=light.corners[0].xyz,b=light.corners[1].xyz,c=light.corners[2].xyz;
  vec3 normal=cross(b-a,c-a);float twiceArea=length(normal);
  if(twiceArea<=1e-12)return result;
  normal/=twiceArea;
  float u=sqrt(lightRandom(state)),v=lightRandom(state);
  vec3 endpoint=a+u*((1-v)*(b-a)+v*(c-a));
  vec3 delta=endpoint-hit.p;float distance2=dot(delta,delta);
  if(distance2<=1e-12)return result;
  vec3 direction=delta*inversesqrt(distance2);
  float emitterCosine=dot(normal,-direction);
  if(light.radiance.w>1)emitterCosine=abs(emitterCosine);
  float geometry=max(0,dot(hit.n,direction))*max(0,emitterCosine);
  // Uniform area PDF 1/A, converted to solid angle: d^2/(A*cosEmitter).
  result.physical=light.radiance.rgb*(.5*twiceArea*geometry/(3.14159265359*distance2))
      *localFalloff(dot(light.positionRadius.xyz-hit.p,light.positionRadius.xyz-hit.p))*frame.localScale.x;
  result.endpoint=offsetRay(endpoint,dot(normal,-direction)>0?normal:-normal);
  result.distance=sqrt(distance2);
  return result;
}
LocalCandidate localCandidate(LightEmitter light,Hit hit,inout uint state) {
  if(light.radiance.w!=0)return triangleCandidate(light,hit,state);
  LocalCandidate result;result.physical=vec3(0);result.endpoint=hit.p;result.distance=0;
  vec3 delta=light.positionRadius.xyz-hit.p;
  float distance2=dot(delta,delta),radius=light.positionRadius.w;
  float attenuation=localFalloff(distance2);
  if(attenuation==0)return result;
  bool inside=distance2<=radius*radius;
  vec3 axis=distance2>1e-20?delta*inversesqrt(distance2):hit.n;
  float sinMax2=min(1,radius*radius/max(distance2,1e-20));
  // Stable for small, distant lights: 1-sqrt(1-x) = x/(1+sqrt(1-x)).
  float oneMinusCos=inside?2:sinMax2/(1+sqrt(max(0,1-sinMax2)));
  float cosTheta=1-lightRandom(state)*oneMinusCos;
  float sinTheta=sqrt(max(0,(1-cosTheta)*(1+cosTheta)));
  float phi=6.28318530718*lightRandom(state);
  vec3 tangent=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(0,1,0),axis));
  vec3 direction=normalize(axis*cosTheta+sinTheta*(cos(phi)*tangent+sin(phi)*cross(axis,tangent)));
  float cosine=max(0,dot(hit.n,direction));
  if(cosine==0)return result;
  float projected=dot(delta,direction);
  vec3 perpendicular=delta-direction*projected;
  float root=sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
  float distance=max(0,inside?projected+root:projected-root);
  result.physical=light.radiance.rgb*(2*oneMinusCos*cosine*attenuation)*frame.localScale.x;
  result.endpoint=hit.p+direction*distance;result.distance=distance;
  return result;
}
float localLuminance(vec3 value) { return dot(value,vec3(.2126,.7152,.0722)); }
// Stratified light candidates for very small sets (RT Gems II, ch. 22-23).
// One sphere direction per light, then a weighted reservoir and ONE visibility
// query. E[physical / p] is the sum of these candidates; their expectation is
// the original sphere integral. No radiance clamp or extra visibility samples.
vec3 smallLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,uint samples,
                        uint guideKey,uint preferred,float guideMix,out float lightDistance) {
  vec3 result=vec3(0);float distanceWeight=0;lightDistance=0;
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    LocalCandidate selected,preferredSample;
    selected.physical=vec3(0);selected.distance=0;selected.endpoint=hit.p;
    preferredSample=selected;
    uint selectedIndex=0;float total=0;
    uint nodeCount=2*nodes.data[0].links.w-1;
    for(uint index=0;index<nodeCount;++index) {
      if(nodes.data[index].links.w!=1)continue;
      LocalCandidate candidate=localCandidate(emitters.data[nodes.data[index].links.z],hit,state);
      float weight=localLuminance(candidate.physical);total+=weight;
      if(weight>0&&lightRandom(state)*total<weight) {selected=candidate;selectedIndex=index;}
      if(index==preferred)preferredSample=candidate;
    }
    if(total<=0)continue;
    bool guided=guideMix>0&&localLuminance(preferredSample.physical)>0&&lightRandom(state)<guideMix;
    float mixWeight=localLuminance(preferredSample.physical)>0?guideMix:0;
    if(guided) {selected=preferredSample;selectedIndex=preferred;}
    float pdf=(1-mixWeight)*localLuminance(selected.physical)/total+(selectedIndex==preferred?mixWeight:0);
    vec3 estimate=selected.physical/pdf;
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.n),selected.endpoint);
    result+=estimate*visibility;
    if(enableLocalGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,selectedIndex,guided,visibility,selected.physical,false);
    float weight=localLuminance(estimate);lightDistance+=selected.distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  return result/float(samples);
}
// Bounded stratified RIS for sets containing area emitters. Compare M fresh
// candidates using their actual unoccluded contribution, then trace only the
// reservoir winner. For e_i = physical_i / proposalPdf_i, weight_i = lum(e_i):
// E[winner * sum(weight)/(M*weight_winner) | candidates] = sum(e_i)/M.
// Stratifying the BVH variate preserves that expectation and reduces repeated
// draws from the same bright cluster. Visibility is always evaluated afresh;
// zero candidates remain in M. History changes only the proposal distribution;
// no radiance/shadow reuse, clamping or extra visibility rays.
vec3 resampledLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,
                           uint samples,out float lightDistance,bool useGuide) {
  uint count=uint(frame.localScale.z);
  vec3 result=vec3(0);float distanceWeight=0;lightDistance=0;
  uint guideKey=0,preferred=0,guideRoot=0;float guideMix=0,rootPdf=1;
  if(enableLocalGuiding&&useGuide&&frame.lightGuideWrite!=0) {
    guideKey=lightGuideKey(hit);
    guideMix=lightGuideProposal(nodes,hit,guideKey,preferred,true);
    // Keep the stratified whole-tree proposal on torch-dominated cells. A
    // single cached torch is a poor substitute for the other visible torches.
    if(guideMix>0&&emitters.data[nodes.data[preferred].links.z].radiance.w==0)guideMix=0;
    if(guideMix>0) {
      // A visible area light guides its local subtree, not just one triangle
      // of a tessellated surface.
      uint limit=32u;
      while(nodes.data[guideRoot].links.w>limit) {
        uvec4 links=nodes.data[guideRoot].links;
        float a=lightWeight(nodes.data[links.x],hit.p,hit.n),b=lightWeight(nodes.data[links.y],hit.p,hit.n);
        if(a==0&&b==0) {rootPdf=0;break;}
        float split=lightSplit(a,b);bool right=preferred>=links.y;
        rootPdf*=right?1-split:split;guideRoot=right?links.y:links.x;
      }
      if(rootPdf<=0)guideMix=0;
    }
  }
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    LocalCandidate selected;
    selected.physical=vec3(0);selected.endpoint=hit.p;selected.distance=0;
    float total=0;uint selectedIndex=0;bool selectedGuided=false;vec3 selectedPhysical=vec3(0);
    for(uint candidateIndex=0;candidateIndex<count;++candidateIndex) {
      bool guided=guideMix>0&&lightRandom(state)<guideMix;
      uint index=guided?guideRoot:0;float pdf=guided?rootPdf:1;
      float u=(float(candidateIndex)+lightRandom(state))/float(count);
      while(nodes.data[index].links.w>1) {
        uvec4 links=nodes.data[index].links;
        float a=lightWeight(nodes.data[links.x],hit.p,hit.n),b=lightWeight(nodes.data[links.y],hit.p,hit.n);
        if(a==0&&b==0) {pdf=0;break;}
        float probability=lightSplit(a,b);
        if(u<probability) {index=links.x;pdf*=probability;u/=probability;}
        else {index=links.y;pdf*=1-probability;u=(u-probability)/(1-probability);}
        u=min(u,.99999994);
      }
      bool inCluster=index>=guideRoot&&index<guideRoot+2*nodes.data[guideRoot].links.w-1;
      pdf*=1-guideMix+(guideMix>0&&inCluster?guideMix/rootPdf:0);
      if(pdf<=0)continue;
      LocalCandidate candidate=localCandidate(emitters.data[nodes.data[index].links.z],hit,state);
      vec3 physical=candidate.physical;
      candidate.physical/=pdf;
      float weight=localLuminance(candidate.physical);total+=weight;
      if(weight>0&&lightRandom(state)*total<weight) {
        selected=candidate;selectedIndex=index;selectedGuided=guided;selectedPhysical=physical;
      }
    }
    if(total<=0)continue;
    vec3 estimate=selected.physical*(total/(float(count)*localLuminance(selected.physical)));
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.n),selected.endpoint);
    result+=estimate*visibility;
    if(enableLocalGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,selectedIndex,selectedGuided,visibility,selectedPhysical,true);
    float weight=localLuminance(estimate);
    lightDistance+=selected.distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  return result/float(samples);
}
vec3 familyLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,
                         out float distance,bool guide) {
  if(nodes.data[0].links.w<=4)
    return smallLocalLighting(nodes,emitters,hit,state,1u,0u,0u,0,distance);
  return resampledLocalLighting(nodes,emitters,hit,state,1u,distance,guide);
}
// Separate direct signals. Equal-budget mode chooses a family with known
// probability and writes zero to the other signal. Per-family mode samples
// both. Each estimator retains its own PDF, hit distance and temporal history.
void splitLocalLighting(Hit hit,uint state,uint samples,out vec3 torch,out vec3 emission,
                        out float torchDistance,out float emissionDistance) {
  torch=emission=vec3(0);torchDistance=emissionDistance=0;
  if(!enableLocalLights||frame.localConfig.x==0||samples==0)return;
  bool hasTorch=false,hasEmission=false;
  if(frame.splitLocal.y>0)
    hasTorch=lightWeight(LightNodes(frame.torchNodes).data[0],hit.p,hit.n)>0;
  if(frame.splitLocal.z>0)
    hasEmission=lightWeight(LightNodes(frame.emissiveNodes).data[0],hit.p,hit.n)>0;
  float torchWeight=0,emissionWeight=0;
  state=hash(state^0x931a4bf5u);
  for(uint i=0;i<samples;++i) {
    bool both=hasTorch&&hasEmission;
    // Complementary pixels provide each history with nearby valid samples.
    // A frame-wide random phase avoids a persistent spatial selection bias.
    uint phase=hash(frame.config.z+i*0x9e3779b9u)&1u;
    bool chooseTorch=frame.splitLocal.x==2||!both||((gl_GlobalInvocationID.x+gl_GlobalInvocationID.y+phase)&1u)==0u;
    float scale=frame.splitLocal.x==1&&both?2:1;
    if(hasTorch&&chooseTorch) {
      float distance;
      vec3 value=familyLocalLighting(LightNodes(frame.torchNodes),LightEmitters(frame.torchEmitters),
          hit,hash(state^0x746f7263u),distance,false)*scale;
      torch+=value;torchDistance+=distance;torchWeight+=distance>0?1:0;
    }
    if(hasEmission&&(frame.splitLocal.x==2||!both||!chooseTorch)) {
      float distance;
      vec3 value=familyLocalLighting(LightNodes(frame.emissiveNodes),LightEmitters(frame.emissiveEmitters),
          hit,hash(state^0xe315ac47u),distance,true)*scale;
      emission+=value;emissionDistance+=distance;emissionWeight+=distance>0?1:0;
    }
    state=hash(state+0x9e3779b9u);
  }
  torch/=float(samples);emission/=float(samples);
  // Occlusion is a valid black sample, not a missing sample. Keep its distance
  // so the filter does not mistake blocked rays for skipped family selections.
  torchDistance=torchWeight>0?torchDistance/torchWeight:0;
  emissionDistance=emissionWeight>0?emissionDistance/emissionWeight:0;
}
// Returns irradiance/pi, ready to multiply by the receiver's linear albedo.
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance,bool useGuiding) {
  lightDistance=0;
  if(!enableLocalLights||frame.localConfig.x==0||samples==0)return vec3(0);
  LightNodes nodes=LightNodes(frame.lightNodes);
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  if(lightWeight(nodes.data[0],hit.p,hit.n)<=0)return vec3(0);
  // The opaque primary vertex has its own signal/filter. Keep the bounded
  // continuation and transparent-sheet stack on the established estimator.
  if(enableLightResampling&&useGuiding&&frame.localScale.z>1)
    return resampledLocalLighting(nodes,emitters,hit,hash(state^0x746f7263u),samples,lightDistance,true);
  uint guideKey=0,preferred=0;
  float guideMix=0,preferredPdf=-1;
  if(enableLocalGuiding&&useGuiding&&frame.lightGuideWrite!=0) {
    guideKey=lightGuideKey(hit);
    guideMix=lightGuideProposal(nodes,hit,guideKey,preferred,false);
  }
  state=hash(state^0x746f7263u);
  if(enableSmallLights&&nodes.data[0].links.w<=4)
    return smallLocalLighting(nodes,emitters,hit,state,samples,guideKey,preferred,guideMix,lightDistance);
  vec3 result=vec3(0);float distanceWeight=0;
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    bool guided=guideMix>0&&lightRandom(state)<guideMix;
    uint index=0;float selectionPdf=1,u=lightRandom(state);
    if(guided) {
      if(preferredPdf<0)preferredPdf=lightBasePdf(nodes,hit,preferred);
      index=preferred;selectionPdf=preferredPdf;
    }
    else while(nodes.data[index].links.w>1) {
      uvec4 links=nodes.data[index].links;
      float a=lightWeight(nodes.data[links.x],hit.p,hit.n),b=lightWeight(nodes.data[links.y],hit.p,hit.n);
      if(a==0&&b==0){selectionPdf=0;break;}
      float probability=lightSplit(a,b);
      if(u<probability) {index=links.x;selectionPdf*=probability;u/=probability;}
      else {index=links.y;selectionPdf*=1-probability;u=(u-probability)/(1-probability);}
      u=min(u,0.99999994);
    }
    // Full mixture PDF, including the BVH component even for a guided draw.
    selectionPdf=(1-guideMix)*selectionPdf+(index==preferred?guideMix:0);
    if(selectionPdf<=0)continue;
    LightEmitter light=emitters.data[nodes.data[index].links.z];
    if(light.radiance.w!=0) {
      LocalCandidate candidate=triangleCandidate(light,hit,state);
      if(localLuminance(candidate.physical)<=0)continue;
      vec3 estimate=candidate.physical/selectionPdf;
      vec3 visibility=localVisibility(offsetRay(hit.p,hit.n),candidate.endpoint);
      result+=estimate*visibility;
      if(enableLocalGuiding&&useGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
        trainLightGuide(guideKey,index,guided,visibility,candidate.physical,false);
      float weight=localLuminance(estimate);
      lightDistance+=candidate.distance*weight;distanceWeight+=weight;
      continue;
    }
    // Keep the established sphere math in this fast path. Factoring its
    // early-outs through a candidate return changes the driver's generated
    // control flow at every secondary vertex in large-light scenes.
    vec3 delta=light.positionRadius.xyz-hit.p;
    float distance2=dot(delta,delta),radius=light.positionRadius.w;
    float attenuation=localFalloff(distance2);
    if(attenuation==0)continue;
    bool inside=distance2<=radius*radius;
    vec3 axis=distance2>1e-20?delta*inversesqrt(distance2):hit.n;
    float sinMax2=min(1,radius*radius/max(distance2,1e-20));
    // Stable for small, distant lights: 1-sqrt(1-x) = x/(1+sqrt(1-x)).
    float oneMinusCos=inside?2:sinMax2/(1+sqrt(max(0,1-sinMax2)));
    float cosTheta=1-lightRandom(state)*oneMinusCos;
    float sinTheta=sqrt(max(0,(1-cosTheta)*(1+cosTheta)));
    float phi=6.28318530718*lightRandom(state);
    vec3 tangent=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(0,1,0),axis));
    vec3 direction=normalize(axis*cosTheta+sinTheta*(cos(phi)*tangent+sin(phi)*cross(axis,tangent)));
    float cosine=max(0,dot(hit.n,direction));
    if(cosine==0)continue;
    float projected=dot(delta,direction);
    vec3 perpendicular=delta-direction*projected;
    float root=sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
    float distance=max(0,inside?projected+root:projected-root);
    vec3 physical=light.radiance.rgb*(2*oneMinusCos*cosine*attenuation)*frame.localScale.x;
    vec3 estimate=physical/selectionPdf;
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.n),hit.p+direction*distance);
    result+=estimate*visibility;
    if(enableLocalGuiding&&useGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,index,guided,visibility,physical,false);
    float weight=dot(estimate,vec3(.2126,.7152,.0722));
    lightDistance+=distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  return result/float(samples);
}
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance) {
  return localLighting(hit,state,samples,lightDistance,false);
}
