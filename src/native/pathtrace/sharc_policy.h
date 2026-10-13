#ifndef REMIX_SHARC_POLICY_H
#define REMIX_SHARC_POLICY_H

// Shared by the CPU allocator/scene eligibility and GLSL cache passes.
// SHARC's directional representation cannot resolve sharp outgoing highlights:
// a smoother PBR surface holds no entry, or with sharcGlossySurfaces one lit
// as if it were this rough.
#define REMIX_SHARC_PBR_MIN_ROUGHNESS 0.5
#define REMIX_SHARC_ACCUMULATION_STRIDE 32u
#define REMIX_SHARC_RESOLVED_STRIDE 24u
#endif
