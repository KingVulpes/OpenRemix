// Analytic sphere next-event estimation, RT Gems I chapter 18. The selection
// PDF and solid-angle PDF both belong in the estimator. No light-count ray loop.
struct LightEmitter { vec4 positionRadius; vec4 radiance; };
struct LightNode { vec4 minimumPower; vec4 maximum; uvec4 links; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightEmitters { LightEmitter data[]; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightNodes { LightNode data[]; };
float lightRandom(inout uint state) {
  state=hash(state+0x9e3779b9u);return float(state>>8)*(1.0/16777216.0);
}
float lightWeight(LightNode node,vec3 p,vec3 n) {
  vec3 halfSize=.5*(node.maximum.xyz-node.minimumPower.xyz);
  vec3 delta=.5*(node.maximum.xyz+node.minimumPower.xyz)-p;
  float radius2=dot(halfSize,halfSize);
  if(dot(delta,n)+sqrt(radius2)<=0)return 0;
  vec3 nearest=max(abs(delta)-halfSize,vec3(0));
  float cosine=min(1,(dot(delta,n)+sqrt(radius2))*inversesqrt(max(max(dot(delta,delta),radius2),1e-12)));
  // Distance to the box prioritizes nearby clusters. Center/radius distance
  // badly under-samples a dominant nearby torch in a large, flat light field.
  return node.minimumPower.w*cosine/max(max(dot(nearest,nearest),node.maximum.w),1e-12);
}
vec3 localVisibility(vec3 origin,vec3 endpoint) {
  vec3 transmission=vec3(1);
  for(uint layer=0;layer<32;++layer) {
    vec3 delta=endpoint-origin;float distance=length(delta);
    if(distance<=1e-6)return transmission;
    Hit hit=closest(origin,delta/distance,distance);
    if(hit.t==1e30)return transmission;
    if(!hit.blended)return vec3(0);
    transmission*=hit.color.rgb*(1-hit.color.a);
    origin=offsetRay(hit.p,-hit.n);
  }
  return vec3(0);
}
// Returns irradiance/pi, ready to multiply by the receiver's linear albedo.
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance) {
  lightDistance=0;
  if(!enableLocalLights||frame.localConfig.x==0||samples==0)return vec3(0);
  LightNodes nodes=LightNodes(frame.lightNodes);
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  vec3 result=vec3(0);float distanceWeight=0;
  state=hash(state^0x746f7263u);
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    uint index=0;float selectionPdf=1,u=lightRandom(state);
    if(lightWeight(nodes.data[0],hit.p,hit.n)<=0)continue;
    while(nodes.data[index].links.w>1) {
      uvec4 links=nodes.data[index].links;
      float a=lightWeight(nodes.data[links.x],hit.p,hit.n),b=lightWeight(nodes.data[links.y],hit.p,hit.n);
      if(a==0&&b==0){selectionPdf=0;break;}
      float scale=max(a,b);
      float probability=a==0?0:b==0?1:clamp((a/scale)/(a/scale+b/scale),1e-6,1-1e-6);
      if(u<probability) {index=links.x;selectionPdf*=probability;u/=probability;}
      else {index=links.y;selectionPdf*=1-probability;u=(u-probability)/(1-probability);}
      u=min(u,0.99999994);
    }
    if(selectionPdf<=0)continue;
    LightEmitter light=emitters.data[nodes.data[index].links.z];
    vec3 delta=light.positionRadius.xyz-hit.p;
    float distance2=dot(delta,delta),radius=light.positionRadius.w;
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
    // Solve from the angular offset to avoid cancellation of large squared distances.
    float projected=dot(delta,direction);
    vec3 perpendicular=delta-direction*projected;
    float root=sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
    float distance=max(0,inside?projected+root:projected-root);
    vec3 estimate=light.radiance.rgb*(2*oneMinusCos*cosine/selectionPdf)*frame.localScale.x;
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.n),hit.p+direction*distance);
    result+=estimate*visibility;
    float weight=dot(estimate,vec3(.2126,.7152,.0722));
    lightDistance+=distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  return result/float(samples);
}
