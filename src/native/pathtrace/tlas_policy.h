#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>

namespace remix::trace {
  struct InstanceInput {
    uint64_t id = 0, geometry = 0;
    uint32_t sub = 0, customIndex = 0;
    scene::Matrix model = scene::identity();
    float diagonal = 0;
    uint64_t boundsRevision = 0;
    // No surface of the instance is double-sided (doubleSided belongs to the
    // whole instance). Selects hardware facing culling; see instanceFlags.
    bool oneSided = false;
    bool operator==(const InstanceInput &) const = default;
  };
  // VkGeometryInstanceFlagBitsKHR values (static_assert-ed in tlas.cpp):
  // TRIANGLE_FACING_CULL_DISABLE and TRIANGLE_FRONT_COUNTERCLOCKWISE.
  constexpr uint32_t instanceFacingCullDisable = 1, instanceFrontCounterClockwise = 2;
  // An instance's flags: the model's handedness selects the front face (the
  // face rayQueryGetIntersectionFrontFaceEXT reports). Without hardware face
  // culling every instance disables facing culling and the shaders reject
  // one-sided back faces; with it (rtx.native.pathtrace.hardwareFaceCulling)
  // one-sided instances leave that to gl_RayFlagsCullBackFacingTrianglesEXT,
  // which culls exactly the triangles whose front-face query would be false.
  inline uint32_t instanceFlags(const InstanceInput &input, bool hardwareFaceCulling) {
    const auto &m = input.model;
    const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                      m[8] * (m[1] * m[6] - m[5] * m[2]);
    return (hardwareFaceCulling && input.oneSided ? 0u : instanceFacingCullDisable) |
           (det > 0 ? instanceFrontCounterClockwise : 0u);
  }
  struct TlasPolicy {
    std::vector<InstanceInput> previous, built;
    uint32_t refits = 0;
    bool initialized = false;
    // NONE + unchanged means no GPU work; NONE + changed means an UPDATE.
    uint32_t reason(const std::vector<InstanceInput> &inputs, bool dynamic, bool force) const;
    bool changed(const std::vector<InstanceInput> &inputs) const;
    void commit(const std::vector<InstanceInput> &inputs, uint32_t reason);
  };
} // namespace remix::trace
