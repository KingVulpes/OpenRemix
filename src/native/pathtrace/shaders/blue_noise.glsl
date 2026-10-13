// Heitz et al. Owen-scrambled Sobol, optimized for 8/32 samples respectively.
// Both immutable tiles pack dimensions 0/1; see LICENSE-blue-noise.txt.
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer BlueNoise {
  uint data[];
};
vec2 blueNoiseSample(ivec2 pixel,uint sequence,uint base,uint mask) {
  BlueNoise noise=BlueNoise(frame.blueNoise);
  uint index=(uint(pixel.x)&127u)+128u*(uint(pixel.y)&127u);
  uint tile=noise.data[base+index];
  uint sampleIndex=sequence&mask;
  uint x=noise.data[base+16384u+(sampleIndex^((tile>>16)&mask))]&255u;
  uint y=(noise.data[base+16384u+(sampleIndex^(tile>>24))]>>8)&255u;
  return (vec2(x^(tile&255u),y^((tile>>8)&255u))+.5)/256.0;
}
vec2 shadowBlueNoise(ivec2 pixel,uint sequence) {
  return blueNoiseSample(pixel,sequence,0u,7u);
}
vec2 localEmitterNoise(bool primary) {
  if(!primary||(uint(frame.jitter.w)&2u)==0)return vec2(-1);
  return blueNoiseSample(shadingPixel,frame.config.z,16392u,31u);
}
// Only the screen-uniform stream index rotates the pattern. Per-pixel hashes
// would destroy its spatial error distribution. Separate candidates/samples and
// light families use different rotations; sun uses an independent table.
vec2 localEmitterSample(vec2 original,vec2 blue,uint stream) {
  if(blue.x<0)return original;
  uint rotation=hash(stream^0x6c6f6361u);
  vec2 shift=vec2(rotation>>8,hash(rotation)>>8)*(1.0/16777216.0);
  return fract(blue+shift);
}
