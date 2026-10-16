#pragma once
#include "scene/types.h"
#include "scene/light_bvh.h"
#include "scene/light_visibility.h"
#include "scene/emission.h"
#include "scene/id_set.h"
#include "scene/opacity.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace remix::scene {
class StaticScene;
// CPU snapshots only: safe to construct while the previous GPU submission
// is running. Temporal history and GPU uploads consume this after its fence.
struct TraceFrame {
  struct Surface {
    Draw draw;
    uint32_t animationFrame = 0;
    std::array<float, 4> uv{};
    std::shared_ptr<const OpacityPartition> opacity;
  };
  struct Emitter {
    SphereLight light;
    uint64_t layout = 0;
    bool animated = false;
  };
  // Instances as ranges of one surface array: no allocation per instance.
  // instances[i] is a span of instance i's surfaces (mesh surface order).
  class InstanceList {
  public:
    using Span = std::span<Surface>;
    using ConstSpan = std::span<const Surface>;
    size_t size() const { return m_ranges.size(); }
    bool empty() const { return m_ranges.empty(); }
    Span operator[](size_t i) { return {m_surfaces.data() + m_ranges[i].first, m_ranges[i].second}; }
    ConstSpan operator[](size_t i) const { return {m_surfaces.data() + m_ranges[i].first, m_ranges[i].second}; }
    template <class List, class Value> struct Iterator {
      List *list;
      size_t index;
      Value operator*() const { return (*list)[index]; }
      Iterator &operator++() {
        ++index;
        return *this;
      }
      bool operator==(const Iterator &other) const { return index == other.index; }
    };
    auto begin() { return Iterator<InstanceList, Span>{this, 0}; }
    auto end() { return Iterator<InstanceList, Span>{this, size()}; }
    auto begin() const { return Iterator<const InstanceList, ConstSpan>{this, 0}; }
    auto end() const { return Iterator<const InstanceList, ConstSpan>{this, size()}; }
    // Building: surfaces are appended to the most recently started instance.
    void reserve(size_t surfaces) { m_surfaces.reserve(surfaces); }
    void start() { m_ranges.push_back({uint32_t(m_surfaces.size()), 0}); }
    void push(Surface &&surface) {
      m_surfaces.push_back(std::move(surface));
      ++m_ranges.back().second;
    }
    // Reorders or drops whole instances; their surfaces stay in place.
    template <class Order> bool sorted(Order order) const {
      for (size_t i = 1; i < size(); ++i)
        if (order((*this)[i], (*this)[i - 1]))
          return false;
      return true;
    }
    template <class Order> void sort(Order order) {
      std::sort(m_ranges.begin(), m_ranges.end(), [&](const auto &a, const auto &b) {
        return order(ConstSpan{m_surfaces.data() + a.first, a.second}, ConstSpan{m_surfaces.data() + b.first, b.second});
      });
    }
    template <class Predicate> void erase(Predicate predicate) {
      std::erase_if(m_ranges, [&](const auto &range) {
        return predicate(ConstSpan{m_surfaces.data() + range.first, range.second});
      });
    }

  private:
    std::vector<Surface> m_surfaces;
    std::vector<std::pair<uint32_t, uint32_t>> m_ranges; // first surface, count
  };
  double seconds = 0;
  InstanceList instances;
  // Each instance's first emitter (instances.size() + 1 entries).
  std::vector<uint32_t> emitterFirst;
  // The persistent static set (StaticScene) is traced with these instances;
  // instances then holds only the frame's other draws.
  bool staticUnits = false;
  IdSet persistentIds; // nonzero caller identities of the frame
  std::vector<Emitter> emitters;
  // Staged copies leave the last submitted lighting intact if presentation is skipped.
  struct Lighting {
    LightBvh combined, torch, emissive;
    LightVisibility visibility; // for combined
    EmissionHistory history;
    bool hasEmissive = false;
    // Statistics: see emittingInstances below; CPU time of the light update.
    uint32_t emittingInstances = 0, dynamicEmittingInstances = 0;
    double updateMilliseconds = 0;
  } lighting;
  // Instances with emitters (outside the static set), and those among them
  // the host identified as dynamic.
  uint32_t emittingInstances = 0, dynamicEmittingInstances = 0;
  bool viewModel = false; // a traced view-model draw (Draw::viewModel) is in the frame
};

// statics: a persistent static set traced alongside draws. Its identities
// count as the frame's (legacy identities avoid them, duplicates are rejected).
TraceFrame prepareTraceFrame(const std::vector<Draw> &draws, double seconds, bool filterEmission,
                             SurfaceEmissionCache &emissionCache, const StaticScene *statics = nullptr);
} // namespace remix::scene
