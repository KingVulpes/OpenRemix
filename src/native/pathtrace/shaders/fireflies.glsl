// Apply only after all sampling/visibility weights, before sample averaging.
// Match RTX Remix luminance scaling, measuring outgoing (remodulated) radiance.
float fireflyScale(Hit hit,vec3 total,float threshold) {
  if(threshold<=0)return 1;
  float luminance=dot(total*materialFactor(hit),vec3(.2126,.7152,.0722));
  return luminance>threshold?threshold/luminance:1;
}
void clampLighting(Hit hit,inout vec3 total,inout vec3 specular,float threshold) {
  if(threshold<=0)return;
  float scale=fireflyScale(hit,total,threshold);
  total*=scale;specular*=scale;
}
