#pragma once
#include "scene/types.h"
#include <map>
#include <utility>

namespace remix::scene {
  struct TemporalEpoch {
    uint64_t world = 0, reset = 0;
    bool operator==(const TemporalEpoch &) const = default;
  };
  // A transaction is committed only after vkQueueSubmit succeeds. API submissions
  // and discarded/minimized frames never become motion-vector history.
  class TemporalHistory {
  public:
    struct SurfaceHistory {
      Draw draw;
      uint64_t material = 0;
      uint32_t textureFrame = 0;
    };
    using Key = std::tuple<uint64_t, uint32_t, uint32_t>;
    void begin(TemporalEpoch epoch, uint64_t integrator, uint32_t width, uint32_t height);
    const SurfaceHistory *previous(const Draw &draw) const;
    // Motion correspondence survives shading changes. Consumers must validate
    // reprojected shading separately (depth/normal, radiance and color clipping).
    const SurfaceHistory *previousGeometry(const Draw &draw) const;
    void stage(const Draw &draw, uint32_t textureFrame = 0);
    void commit();
    void clear();
    bool reset() const { return m_reset; }
    uint64_t frames() const { return m_frames; }
    static uint64_t materialSignature(const Draw &draw);

  private:
    std::map<Key, SurfaceHistory> m_previous, m_pending;
    TemporalEpoch m_epoch{}, m_nextEpoch{};
    uint64_t m_integrator = 0, m_nextIntegrator = 0, m_frames = 0;
    uint32_t m_width = 0, m_height = 0, m_nextWidth = 0, m_nextHeight = 0;
    bool m_reset = true;
  };
} // namespace remix::scene
