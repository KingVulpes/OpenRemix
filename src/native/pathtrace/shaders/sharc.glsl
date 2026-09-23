// Pinned NVIDIA SHARC 1.8.3.0. See third_party/sharc/License.md.
#include "../sharc_policy.h"
#define SHARC_ENABLE_GLSL 1
#ifdef SHARC_UPDATE_PASS
#define SHARC_UPDATE 1
#else
#define SHARC_UPDATE 0
#endif
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_ENABLE_SH_ENCODING 1
#define SHARC_ENABLE_CACHE_RESAMPLING 1
#define SHARC_SAMPLE_NUM_THRESHOLD 3
// Each resolve invocation owns exactly one entry. Disable SDK cross-entry
// history reads during in-place resolve; LOD changes warm through normal misses.
#define SHARC_BLEND_ADJACENT_LEVELS 0
#define SHARC_LINEAR_PROBE_WINDOW_SIZE 0
// NRD's HLSL compatibility aliases are already expanded in its declarations.
#undef rcp
#undef saturate
#undef lerp
#undef countbits
#undef asfloat
#undef asuint
#undef f32tof16
#undef f16tof32
#include "SharcGlslHelpers.h"
#include "SharcCommon.h"
layout(buffer_reference,std430,buffer_reference_align=4) buffer CacheCounters { uint data[]; };
SharcParameters cacheParameters() {
  SharcParameters p;
  p.hashGridParameters.cameraPosition=frame.eye.xyz;
  p.hashGridParameters.logarithmBase=2;
  p.hashGridParameters.sceneScale=frame.sharcGrid.x;
  p.hashGridParameters.levelBias=frame.sharcGrid.y;
  p.hashGridData.capacity=frame.sharcConfig.y;
  p.hashGridData.hashEntriesBuffer=RWStructuredBuffer_uint64_t(frame.sharcHash);
  p.radianceScale=1000;
  p.accumulationBuffer=RWStructuredBuffer_SharcAccumulationData(frame.sharcAccumulation);
  p.resolvedBuffer=RWStructuredBuffer_SharcPackedData(frame.sharcResolved);
  return p;
}
