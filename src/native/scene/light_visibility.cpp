#include "scene/light_visibility.h"

#include <algorithm>

namespace remix::scene {
  int32_t VisibilityPartition::cluster(const float p[3]) const {
    if (nodes.empty())
      return -1;
    int32_t index = 0;
    for (uint32_t guard = 0; index >= 0 && guard < 1024; ++guard) {
      if (size_t(index) >= nodes.size())
        return -1;
      const auto &n = nodes[index];
      index = n.plane[0] * p[0] + n.plane[1] * p[1] + n.plane[2] * p[2] - n.plane[3] >= 0 ? n.children[0]
                                                                                          : n.children[1];
    }
    if (index >= 0)
      return -1;
    const size_t leaf = size_t(-1 - int64_t(index));
    if (leaf >= leafClusters.size())
      return -1;
    const int32_t c = leafClusters[leaf];
    return c >= 0 && uint32_t(c) < clusterCount ? c : -1;
  }

  void LightVisibility::locate(const VisibilityPartition &partition, size_t light, const float *p, float probe) {
    int32_t *clusters = &m_lightClusters[light * kClusters];
    std::fill_n(clusters, kClusters, -1);
    m_unrestricted[light] = 0;
    if (const int32_t c = partition.cluster(p); c >= 0) {
      clusters[0] = c;
      return;
    }
    // Center in solid space: the clusters around it, found along the axes.
    uint32_t found = 0;
    for (int axis = 0; axis < 3; ++axis)
      for (float sign : {-1.f, 1.f}) {
        float q[3] = {p[0], p[1], p[2]};
        q[axis] += sign * probe;
        const int32_t c = partition.cluster(q);
        if (c < 0 || std::find(clusters, clusters + found, c) != clusters + found)
          continue;
        if (found == kClusters) {
          m_unrestricted[light] = 1;
          return;
        }
        clusters[found++] = c;
      }
    if (!found)
      m_unrestricted[light] = 1;
  }

  bool LightVisibility::update(const std::shared_ptr<const VisibilityPartition> &partition, const LightBvh &tree,
                               bool enabled, float probe) {
    const auto &nodes = tree.nodes();
    const auto &emitters = tree.emitters();
    if (!enabled || !partition || partition->clusterCount == 0 || nodes.empty()) {
      const bool changed = m_table != nullptr;
      m_table.reset();
      m_partition.reset();
      m_partitionVersion = m_treeBuilds = ~0ull;
      clustered = unrestricted = 0;
      if (changed)
        ++m_version;
      return changed;
    }
    // A rebuilt tree or a new partition assigns every light afresh; only
    // lights that move afterwards become unrestricted.
    bool rebuild = partition->version != m_partitionVersion || tree.builds != m_treeBuilds ||
                   m_unrestricted.size() != emitters.size();
    if (rebuild) {
      m_partition = partition;
      m_partitionVersion = partition->version;
      m_treeBuilds = tree.builds;
      m_lightClusters.assign(emitters.size() * kClusters, -1);
      m_unrestricted.assign(emitters.size(), 0);
      m_positions.assign(emitters.size() * 3, 0);
      for (size_t i = 0; i < emitters.size(); ++i) {
        const float *p = emitters[i].positionRadius;
        std::copy_n(p, 3, &m_positions[i * 3]);
        locate(*partition, i, p, probe + emitters[i].positionRadius[3]);
      }
    } else {
      for (size_t i = 0; i < emitters.size(); ++i) {
        const float *p = emitters[i].positionRadius;
        float *last = &m_positions[i * 3];
        if (!m_unrestricted[i] && (p[0] != last[0] || p[1] != last[1] || p[2] != last[2])) {
          m_unrestricted[i] = 1;
          rebuild = true;
        }
        std::copy_n(p, 3, last);
      }
    }
    if (!rebuild)
      return false;
    clustered = unrestricted = 0;
    for (uint8_t u : m_unrestricted)
      ++(u ? unrestricted : clustered);
    // Rows of node bits. Children always follow their parent in node order,
    // so one reverse pass per cluster fills internal nodes from their leaves.
    const uint32_t clusters = partition->clusterCount;
    m_rowWords = uint32_t((nodes.size() + 31) / 32);
    auto table = std::make_shared<std::vector<uint32_t>>(size_t(clusters) * m_rowWords, 0u);
    std::vector<uint8_t> bits(nodes.size());
    for (uint32_t s = 0; s < clusters; ++s) {
      for (size_t n = nodes.size(); n-- > 0;) {
        const auto &links = nodes[n].links;
        if (links[3] <= 1) {
          const size_t light = links[2];
          if (light >= m_unrestricted.size() || m_unrestricted[light]) {
            bits[n] = 1;
          } else {
            bits[n] = 0;
            for (uint32_t k = 0; k < kClusters && m_lightClusters[light * kClusters + k] >= 0; ++k)
              bits[n] |= partition->visible(int32_t(s), m_lightClusters[light * kClusters + k]);
          }
        } else {
          bits[n] = bits[links[0]] | bits[links[1]];
        }
      }
      uint32_t *row = table->data() + size_t(s) * m_rowWords;
      for (size_t n = 0; n < nodes.size(); ++n)
        row[n / 32] |= uint32_t(bits[n]) << (n % 32);
    }
    m_table = std::move(table);
    ++m_version;
    return true;
  }
} // namespace remix::scene
