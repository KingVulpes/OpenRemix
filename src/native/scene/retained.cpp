#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>

// Retained instances: a draw is validated and resolved once, when it is
// created or re-described, into ready Draw records kept in a dense table.
// drawRetained() then copies the visible records into the frame.
//
// Lifetime (matches the dxvk-remix fork, which keeps handles and looks them up
// at replay): an entry names its mesh and materials by handle. It holds the
// objects those handles resolved to, so a DestroyMesh/DestroyMaterial while it
// is registered never leaves a dangling reference. A destroyed or replaced mesh
// or material is marked retired; the entry resolves its handles again at the
// next replay. A mesh that is gone draws nothing until a mesh is created under
// the same hash again. A surface material that is gone falls back to the
// default material, a dead override to the mesh's own material.
//
// Identity: entries without an InstanceIdentityEXT get a stable instance id
// derived from their handle, so temporal history and the TLAS follow the
// object across frames. They are classified static; a placement change demotes
// the entry to dynamic until it has not moved for retainedSettleFrames replays.
namespace remix {
  using namespace scene;
  using namespace scene::detail;

  namespace {
    constexpr uint64_t retainedIdSeed = 0x52657461696e6564ull; // "Retained"

    uint64_t retainedInstanceId(uint64_t handle) {
      const uint64_t id = hashBytes(&handle, sizeof(handle), retainedIdSeed);
      return id ? id : 1;
    }

    bool allFinite(const Matrix &m) {
      return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
    }

    // DrawInstance validation (material override included); copies the borrowed
    // GPU instancing transforms so nothing refers to caller memory.
    void describeRetained(const remixapi_InstanceInfo &info, InstanceDescription &d, std::vector<Matrix> &locals) {
      describeInstance(info, d);
      if (!d.hidden) {
        if (d.instanced) {
          locals.resize(d.instanceCount);
          for (uint32_t i = 0; i < d.instanceCount; ++i)
            locals[i] = transform(d.instanceValues[i]);
        }
      }
      d.instanceValues = nullptr;
    }

    // Runtime identity for entries without a caller identity.
    void identify(RetainedInstance &e) {
      e.callerIdentity = e.description.hasIdentity;
      if (e.callerIdentity)
        return;
      e.description.instanceId = retainedInstanceId(e.handle);
      e.description.classification = e.moving ? REMIXAPI_INSTANCE_DYNAMIC_EXT : REMIXAPI_INSTANCE_STATIC_EXT;
      e.description.rasterVisible = true;
      e.description.traceIdentity = false;
    }
  } // namespace

  // Rebuilds the records. report: throw what DrawInstance would (create and
  // update); otherwise the caller catches and the entry draws nothing.
  void NativeResources::resolveRetained(RetainedInstance &e, bool report) {
    e.records.clear();
    e.resolved.clear();
    e.mesh.reset();
    e.surfaceCount = 0;
    e.watch = false;
    e.placed = true;
    const auto &d = e.description;
    if (d.hidden)
      return;
    const auto mesh = m_meshes.find(d.mesh);
    if (mesh == m_meshes.end()) {
      e.watch = true;
      return;
    }
    bool complete = true;
    auto override = overrideMaterial(d);
    if (d.materialOverride && !override)
      complete = false;
    const uint32_t count = e.instanceCount();
    const size_t surfaces = mesh->second->surfaces.size();
    if (uint64_t(count) * surfaces > 65536)
      throw invalid; // DrawInstance could never submit it either.
    m_modelScratch.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      auto &model = m_modelScratch[i];
      model = d.instanced ? multiply(d.model, e.locals[i]) : d.model;
      if (!allFinite(model)) {
        if (report)
          throw invalid;
        e.placed = false;
      }
    }
    std::vector<Draw> records;
    records.reserve(size_t(count) * surfaces);
    std::vector<std::shared_ptr<Material>> resolved;
    complete &= buildDraws(d, mesh->second, m_modelScratch.data(), count, override, false, records, &resolved);
    if (override)
      resolved.push_back(std::move(override));
    std::sort(resolved.begin(), resolved.end());
    resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
    e.records = std::move(records);
    e.resolved = std::move(resolved);
    e.mesh = mesh->second;
    e.surfaceCount = uint32_t(surfaces);
    e.watch = !complete;
  }

  bool NativeResources::retainedStale(const RetainedInstance &e) const {
    if (e.mesh && e.mesh->retired)
      return true;
    for (const auto &material : e.resolved)
      if (material->retired)
        return true;
    return false;
  }

  void NativeResources::setRetainedWatch(RetainedInstance &e, bool watch) {
    if (e.watch != watch) {
      e.watch = watch;
      watch ? ++m_retainedWatch : --m_retainedWatch;
    }
  }

  void NativeResources::placeRetained(RetainedInstance &e) {
    const uint32_t count = e.instanceCount();
    const bool resolved = e.records.size() == size_t(count) * e.surfaceCount;
    e.placed = true;
    for (uint32_t i = 0; i < count; ++i) {
      const Matrix model = e.description.instanced ? multiply(e.description.model, e.locals[i]) : e.description.model;
      e.placed &= allFinite(model);
      if (resolved)
        for (uint32_t s = 0; s < e.surfaceCount; ++s)
          e.records[size_t(i) * e.surfaceCount + s].model = model;
    }
  }

  void NativeResources::classifyRetained(RetainedInstance &e, uint32_t classification) {
    e.description.classification = classification;
    for (auto &record : e.records)
      record.classification = classification;
  }

  // A moved static object leaves the static TLAS and SHARC's static geometry
  // signature alone while it moves: dynamic until it settles.
  void NativeResources::moveRetained(RetainedInstance &e) {
    if (e.callerIdentity)
      return;
    e.lastMove = m_retainedSerial;
    if (!e.moving) {
      e.moving = true;
      classifyRetained(e, REMIXAPI_INSTANCE_DYNAMIC_EXT);
    }
  }

  uint64_t NativeResources::createRetained(const remixapi_InstanceInfo &info) {
    if (!info.mesh)
      throw invalid;
    RetainedInstance e;
    describeRetained(info, e.description, e.locals);
    e.handle = m_nextRetained + 1;
    identify(e);
    resolveRetained(e, true);
    ++m_nextRetained;
    m_retainedIndex.emplace(e.handle, uint32_t(m_retained.size()));
    m_retainedWatch += e.watch;
    m_retained.push_back(std::move(e));
    return m_nextRetained;
  }

  void NativeResources::updateRetained(uint64_t handle, const remixapi_InstanceInfo &info) {
    if (!handle || !info.mesh)
      throw invalid;
    RetainedInstance next;
    describeRetained(info, next.description, next.locals);
    next.handle = handle;
    const auto found = m_retainedIndex.find(handle);
    RetainedInstance *current = found != m_retainedIndex.end() ? &m_retained[found->second] : nullptr;
    if (current) {
      // A re-description keeps visibility (and the motion state).
      next.visible = current->visible;
      next.moving = current->moving;
      next.lastMove = current->lastMove;
    }
    identify(next);
    // New placements for the same instances are motion; a changed instance
    // count is new membership (streaming), which static geometry absorbs.
    if (current && !next.callerIdentity && !current->callerIdentity &&
        current->instanceCount() == next.instanceCount() &&
        (current->description.model != next.description.model || current->locals != next.locals)) {
      next.lastMove = m_retainedSerial;
      next.moving = true;
      next.description.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    }
    resolveRetained(next, true);
    if (current) {
      m_retainedWatch -= current->watch;
      *current = std::move(next);
      m_retainedWatch += current->watch;
      return;
    }
    // As in the fork, an unknown handle registers a new instance under it. The
    // counter moves past it so CreateRetainedInstance never hands it out.
    m_nextRetained = std::max(m_nextRetained, handle);
    m_retainedIndex.emplace(handle, uint32_t(m_retained.size()));
    m_retainedWatch += next.watch;
    m_retained.push_back(std::move(next));
  }

  void NativeResources::updateRetained(const remixapi_RetainedInstanceUpdate *updates, uint32_t count) {
    if (!count)
      return;
    if (!updates)
      throw invalid;
    // Validate the whole batch before applying any of it.
    for (uint32_t i = 0; i < count; ++i) {
      if (!updates[i].handle)
        throw invalid;
      if (updates[i].flags & REMIXAPI_RETAINED_UPDATE_TRANSFORM)
        for (const auto &row : updates[i].transform.matrix)
          finite(row, 4);
    }
    for (uint32_t i = 0; i < count; ++i) {
      const auto &update = updates[i];
      const auto found = m_retainedIndex.find(update.handle);
      if (found == m_retainedIndex.end())
        continue; // Unknown handles are ignored, as in the fork.
      auto &e = m_retained[found->second];
      if (update.flags & REMIXAPI_RETAINED_UPDATE_TRANSFORM) {
        const Matrix model = transform(update.transform);
        if (model != e.description.model) {
          e.description.model = model;
          placeRetained(e);
          moveRetained(e);
        }
      }
      if (update.flags & REMIXAPI_RETAINED_UPDATE_VISIBILITY)
        e.visible = update.visible != 0;
    }
  }

  void NativeResources::destroyRetained(const uint64_t *handles, uint32_t count) {
    if (!count)
      return;
    if (!handles)
      throw invalid;
    for (uint32_t i = 0; i < count; ++i) {
      const auto found = m_retainedIndex.find(handles[i]);
      if (found == m_retainedIndex.end())
        continue; // Unknown or already destroyed.
      const uint32_t index = found->second;
      m_retainedIndex.erase(found);
      m_retainedWatch -= m_retained[index].watch;
      if (index + 1 != m_retained.size()) {
        m_retained[index] = std::move(m_retained.back());
        m_retainedIndex[m_retained[index].handle] = index;
      }
      m_retained.pop_back();
    }
  }

  void NativeResources::drawRetained(const float *sceneOrigin) {
    float origin[3] = {};
    if (sceneOrigin) {
      finite(sceneOrigin, 3);
      std::copy_n(sceneOrigin, 3, origin);
    }
    const bool rebase = origin[0] != 0 || origin[1] != 0 || origin[2] != 0;
    ++m_retainedSerial;
    RetainedStatistics stats;
    stats.registered = m_retained.size();
    const bool modsChanged = m_modGeneration != m_retainedModGeneration;
    const bool resourcesChanged = m_resourceGeneration != m_retainedResourceGeneration && m_retainedWatch;
    m_retainedModGeneration = m_modGeneration;
    m_retainedResourceGeneration = m_resourceGeneration;
    for (auto &e : m_retained) {
      if (modsChanged || (resourcesChanged && e.watch) || (e.visible && retainedStale(e))) {
        const bool watch = e.watch;
        try {
          resolveRetained(e, false);
        } catch (...) {
          e.records.clear();
          e.resolved.clear();
          e.mesh.reset();
          e.watch = true;
        }
        const bool now = e.watch;
        e.watch = watch;
        setRetainedWatch(e, now);
        ++stats.resolved;
      }
      if (e.moving && m_retainedSerial - e.lastMove >= retainedSettleFrames) {
        e.moving = false;
        classifyRetained(e, REMIXAPI_INSTANCE_STATIC_EXT);
      }
      stats.moving += e.moving;
      if (!e.visible || !e.placed || e.records.empty())
        continue;
      if (m_draws.size() + e.records.size() > 65536) {
        ++stats.overCapacity; // DrawInstance's per-frame limit.
        continue;
      }
      if (e.description.traceIdentity && !m_frameIds.insert(e.description.instanceId).second)
        continue; // Caller identity already drawn this frame.
      const size_t first = m_draws.size();
      m_draws.insert(m_draws.end(), e.records.begin(), e.records.end());
      if (rebase)
        for (size_t i = first; i < m_draws.size(); ++i)
          for (int c = 0; c < 3; ++c)
            m_draws[i].model[12 + c] -= origin[c];
      ++stats.appended;
      stats.draws += e.records.size();
    }
    m_retainedStats = stats;
  }
} // namespace remix
