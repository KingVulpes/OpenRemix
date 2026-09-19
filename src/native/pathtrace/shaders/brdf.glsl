#include "eon.glsl"
mat3 shadingBasis(vec3 n) {
  vec3 t=normalize(cross(abs(n.z)<.999?vec3(0,0,1):vec3(0,1,0),n));
  return mat3(t,cross(n,t),n);
}
// Retain an albedo-independent dielectric reflection, including black plastics.
vec3 materialFactor(Hit hit) { return hit.pbr?max(hit.color.rgb,vec3(.04)):hit.color.rgb; }
vec3 fresnel(vec3 f0,float cosine) { return f0+(1-f0)*pow(clamp(1-cosine,0,1),5); }
float ggxD(float nh,float alpha) {
  float a2=alpha*alpha,den=(1-nh*nh)+a2*nh*nh;
  return a2/(eonPi*den*den);
}
float ggxG1(float nv,float alpha) {
  return 2*nv/(nv+sqrt(alpha*alpha+(1-alpha*alpha)*nv*nv));
}
void materialLobes(Hit hit,vec3 wi,out vec3 diffuse,out vec3 specular) {
  diffuse=specular=vec3(0);
  float ni=dot(hit.n,wi),no=dot(hit.n,hit.wo);
  if(ni<=0||no<=0||dot(hit.ng,wi)<=0)return;
  if(!hit.pbr){diffuse=hit.color.rgb/eonPi;return;}
  mat3 basis=shadingBasis(hit.n);
  diffuse=eonBrdf(hit.color.rgb,hit.rm.x,transpose(basis)*wi,transpose(basis)*hit.wo);
  vec3 h=normalize(wi+hit.wo);
  vec3 f=fresnel(mix(vec3(.04),hit.color.rgb,hit.rm.y),max(0,dot(hit.wo,h)));
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  specular=f*(ggxD(max(0,dot(hit.n,h)),alpha)*ggxG1(ni,alpha)*ggxG1(no,alpha)/(4*ni*no));
  diffuse*=(1-hit.rm.y)*(1-f);
}
vec3 materialBrdf(Hit hit,vec3 wi) {
  vec3 diffuse,specular;materialLobes(hit,wi,diffuse,specular);
  return diffuse+specular;
}
// All NEE estimators previously include cos/pi. Replace that Lambert factor
// with the same BRDF used by continuation; keep the existing light PDFs.
vec3 materialResponse(Hit hit,vec3 wi) {
  if(!hit.pbr)return vec3(dot(hit.ng,wi)>0?1:0);
  return materialBrdf(hit,wi)*eonPi/max(materialFactor(hit),vec3(1e-6));
}
vec3 materialResponse(Hit hit,vec3 wi,out vec3 specular) {
  specular=vec3(0);
  if(!hit.pbr)return materialResponse(hit,wi);
  vec3 diffuse;materialLobes(hit,wi,diffuse,specular);
  vec3 scale=eonPi/max(materialFactor(hit),vec3(1e-6));
  specular*=scale;
  return diffuse*scale+specular;
}
float specularProbability(Hit hit) {
  return clamp(.25+.5*hit.rm.y,.25,.75);
}
// Heitz 2018 GGX visible-normal sampling (isotropic), followed by reflection.
vec3 sampleGgx(vec3 wo,float alpha,vec2 u) {
  vec3 v=normalize(vec3(alpha*wo.xy,wo.z));
  float lensq=dot(v.xy,v.xy);
  vec3 t1=lensq>0?vec3(-v.y,v.x,0)/sqrt(lensq):vec3(1,0,0);
  vec3 t2=cross(v,t1);
  float radius=sqrt(u.x),phi=2*eonPi*u.y;
  float x=radius*cos(phi),y=radius*sin(phi),s=.5*(1+v.z);
  y=mix(sqrt(max(0,1-x*x)),y,s);
  vec3 nh=x*t1+y*t2+sqrt(max(0,1-x*x-y*y))*v;
  vec3 h=normalize(vec3(alpha*nh.xy,max(0,nh.z)));
  return reflect(-wo,h);
}
vec3 materialSample(Hit hit,out vec3 direction,out vec3 specularWeight) {
  specularWeight=vec3(0);
  if(!hit.pbr) {
    direction=diffuseDirection(hit.n);
    return dot(direction,hit.ng)>0?hit.color.rgb:vec3(0);
  }
  mat3 basis=shadingBasis(hit.n);
  vec3 wo=transpose(basis)*hit.wo;
  if(wo.z<=0){direction=hit.n;return vec3(0);}
  float p=specularProbability(hit),choice=random();
  vec2 u=vec2(random(),random());
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  vec3 wi=choice<p?sampleGgx(wo,alpha,u):eonSample(wo,hit.rm.x,u.x,u.y).xyz;
  direction=basis*wi;
  if(wi.z<=0||dot(direction,hit.ng)<=0)return vec3(0);
  vec3 h=normalize(wi+wo);
  float specPdf=ggxD(max(0,h.z),alpha)*ggxG1(wo.z,alpha)/(4*wo.z);
  float pdf=p*specPdf+(1-p)*eonPdf(wo,wi,hit.rm.x);
  vec3 diffuse,specular;materialLobes(hit,direction,diffuse,specular);
  float scale=wi.z/max(pdf,1e-20);
  specularWeight=specular*scale;
  return (diffuse+specular)*scale;
}
vec3 materialSample(Hit hit,out vec3 direction) {
  vec3 specularWeight;
  return materialSample(hit,direction,specularWeight);
}
