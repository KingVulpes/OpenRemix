#pragma once
#include "scene/types.h"

namespace remix::scene {
// Cached, deterministic area quadrature in UV space. No emission texture reads
// occur in lighting rays. Texture snapshots own the cache's lifetime.
std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *uvTransform);
SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance);
}
