#pragma once
#include "scene/light_bvh.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace remix::scene {
  // A host's potentially-visible-set partition of space
  // (remixapi_SetVisibilityPartitionEXT): a binary space partition whose
  // leaves belong to clusters, and a conservative cluster-to-cluster
  // visibility matrix. Lights outside a shading point's potentially visible
  // clusters cannot illuminate it and are removed from light selection.
  struct VisibilityPartition {
    // std430 record shared with light_bvh.glsl. Point p is in front of the
    // plane when dot(normal, p) - distance >= 0. Children >= 0 are nodes,
    // < 0 are leaves (-1 - leaf index).
    struct Node {
      float plane[4];
      int32_t children[2];
      int32_t reserved[2];
    };
    std::vector<Node> nodes;
    std::vector<int32_t> leafClusters; // -1: no cluster (solid or outside)
    uint32_t clusterCount = 0, rowBytes = 0;
    std::vector<uint8_t> visibility; // clusterCount rows of rowBytes
    uint64_t version = 0;

    // Cluster of a point, or -1.
    int32_t cluster(const float point[3]) const;
    bool visible(int32_t from, int32_t to) const {
      return (visibility[size_t(from) * rowBytes + uint32_t(to) / 8] >> (uint32_t(to) % 8)) & 1;
    }
  };
  static_assert(sizeof(VisibilityPartition::Node) == 32);

  // Per (cluster, light-tree node): whether the node holds a light potentially
  // visible from the cluster. Lights without a cluster, and lights that moved
  // since the tree was built (carried or animated, e.g. a flashlight), are
  // visible from everywhere, so that moving lights never rebuild the table.
  class LightVisibility {
  public:
    // Returns whether the table changed.
    // probe: world distance around a light whose center lies outside every
    // cluster (a fixture embedded in a wall) at which its clusters are sought.
    bool update(const std::shared_ptr<const VisibilityPartition> &partition, const LightBvh &tree, bool enabled,
                float probe);
    const std::shared_ptr<const std::vector<uint32_t>> &table() const { return m_table; }
    const std::shared_ptr<const VisibilityPartition> &partition() const { return m_partition; }
    uint32_t rowWords() const { return m_rowWords; }
    uint64_t version() const { return m_version; }
    // Statistics: lights with clusters, lights visible from everywhere.
    uint32_t clustered = 0, unrestricted = 0;

  private:
    std::shared_ptr<const VisibilityPartition> m_partition;
    std::shared_ptr<const std::vector<uint32_t>> m_table;
    uint64_t m_partitionVersion = ~0ull, m_treeBuilds = ~0ull, m_version = 0;
    uint32_t m_rowWords = 0;
    // Per emitter: up to kClusters clusters it may occupy (-1: unused); all -1
    // with m_unrestricted set: visible from everywhere.
    static constexpr uint32_t kClusters = 4;
    std::vector<int32_t> m_lightClusters;
    std::vector<uint8_t> m_unrestricted;
    void locate(const VisibilityPartition &partition, size_t light, const float *position, float probe);
    std::vector<float> m_positions;       // per emitter, since the tree was built
  };
} // namespace remix::scene
