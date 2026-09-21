// Shadow queries carry distance and transmission, without diffuse shading,
// motion vertices or cache metadata. SIGMA needs the nearest blocker; ordinary
// visibility can terminate at any accepted opaque blocker.
struct VisibilityHit { float t; bool blocked; vec3 p; vec3 n; vec3 transmission; };
void queryVisibility(accelerationStructureEXT scene,vec3 origin,vec3 direction,inout VisibilityHit hit,bool anyOpaque) {
  rayQueryEXT query;
  profileRay(8);
  rayQueryInitializeEXT(query,scene,gl_RayFlagsNoneEXT,255,origin,0,direction,hit.t);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    profileRay(9);
    Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false)];
    s.indices=s.traceIndices;
    if((s.state.w&4)==0&&!rayQueryGetIntersectionFrontFaceEXT(query,false)){profileRay(12);continue;}
    profileAlpha(s,8);
    if(!acceptCandidate(s,rayQueryGetIntersectionPrimitiveIndexEXT(query,false),rayQueryGetIntersectionBarycentricsEXT(query,false))){profileRay(11);continue;}
    if(anyOpaque&&(s.state.w&2)==0) {hit.blocked=true;rayQueryTerminateEXT(query);return;}
    rayQueryConfirmIntersectionEXT(query);
  }
  if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return;
  profileRay(13);
  hit.t=rayQueryGetIntersectionTEXT(query,true);
  Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+rayQueryGetIntersectionGeometryIndexEXT(query,true)];
  s.indices=s.traceIndices;
  hit.blocked=(s.state.w&2)==0;
  if(hit.blocked)return;
  uint tri=rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3;
  vec3 a=s.vertices.v[s.indices.i[tri]].position.xyz,b=s.vertices.v[s.indices.i[tri+1]].position.xyz,c=s.vertices.v[s.indices.i[tri+2]].position.xyz;
  vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
  mat4x3 model=rayQueryGetIntersectionObjectToWorldEXT(query,true);
  hit.p=model*vec4(a+bary.x*(b-a)+bary.y*(c-a),1);
  hit.n=normalize(cross(model*vec4(b-a,0),model*vec4(c-a,0)));
  if(dot(hit.n,direction)>0)hit.n=-hit.n;
  vec4 color=albedo(s,tri/3,bary);
  hit.transmission=color.rgb*(1-color.a);
}
VisibilityHit visibilityHit(vec3 origin,vec3 direction,bool anyOpaque) {
  VisibilityHit hit;hit.t=1e30;hit.blocked=false;
  if((frame.target.z&1)!=0)queryVisibility(staticScene,origin,direction,hit,anyOpaque);
  if(anyOpaque&&hit.blocked)return hit;
  if((frame.target.z&2)!=0)queryVisibility(dynamicScene,origin,direction,hit,anyOpaque);
  return hit;
}
