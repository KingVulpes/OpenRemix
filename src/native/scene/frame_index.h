#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace remix::scene {
  // Keyed per-frame records, stored in submission order without per-entry
  // allocations. Callers usually resubmit in the same order, so find() first
  // tries the entry after the previous hit; seal() builds a sorted index for
  // reordered lookups. A repeated key keeps its last value, as map assignment did.
  template <class Key, class Value> class FrameIndex {
  public:
    struct Entry {
      Key key;
      Value value;
    };
    void clear() {
      m_entries.clear();
      m_sorted.clear();
      m_cursor = 0;
      m_sealed = false;
    }
    void insert(const Key &key, Value value) {
      m_entries.push_back({key, std::move(value)});
      m_sealed = false;
    }
    void seal() {
      m_sorted.resize(m_entries.size());
      for (uint32_t i = 0; i < m_sorted.size(); ++i)
        m_sorted[i] = i;
      std::stable_sort(m_sorted.begin(), m_sorted.end(),
                       [&](uint32_t a, uint32_t b) { return m_entries[a].key < m_entries[b].key; });
      // Keep only the last of equal keys; the cursor must not return shadowed entries.
      m_unique = true;
      size_t out = 0;
      for (size_t i = 0; i < m_sorted.size(); ++i) {
        if (i + 1 < m_sorted.size() && !(m_entries[m_sorted[i]].key < m_entries[m_sorted[i + 1]].key)) {
          m_unique = false;
          continue;
        }
        m_sorted[out++] = m_sorted[i];
      }
      m_sorted.resize(out);
      m_cursor = 0;
      m_sealed = true;
    }
    const Value *find(const Key &key) const {
      if (!m_sealed)
        return nullptr;
      if (m_unique && m_cursor < m_entries.size() && m_entries[m_cursor].key == key)
        return &m_entries[m_cursor++].value;
      auto it = std::lower_bound(m_sorted.begin(), m_sorted.end(), key,
                                 [&](uint32_t index, const Key &k) { return m_entries[index].key < k; });
      if (it == m_sorted.end() || m_entries[*it].key != key)
        return nullptr;
      m_cursor = *it + 1;
      return &m_entries[*it].value;
    }
    size_t size() const { return m_entries.size(); }
    bool empty() const { return m_entries.empty(); }

  private:
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_sorted;
    mutable size_t m_cursor = 0;
    bool m_sealed = false, m_unique = true;
  };
} // namespace remix::scene
