layout(set=1,binding=0,std430) readonly buffer ExposureState { vec4 exposureState; } postState;
layout(set=1,binding=1) uniform sampler2D bloomImage;
layout(set=1,binding=2,std140) uniform PostSettings { vec4 values; } postSettings;
vec3 postColor(vec3 color, vec2 uv) {
  if(postSettings.values.y>0)
    color=mix(color,textureLod(bloomImage,uv,0).rgb,postSettings.values.y);
  float automatic=postSettings.values.z>0 ? postState.exposureState.x : 1;
  return color*(postSettings.values.x*automatic);
}
