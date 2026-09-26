#include "scene/temporal.h"

namespace remix::scene {
  void TemporalHistory::begin(TemporalEpoch epoch, uint64_t integrator, uint32_t width, uint32_t height) {
    m_pending.clear();
    m_nextEpoch = epoch;
    m_nextIntegrator = integrator;
    m_nextWidth = width;
    m_nextHeight = height;
    m_reset =
        !m_frames || epoch != m_epoch || integrator != m_integrator || width != m_width || height != m_height;
  }
  uint64_t TemporalHistory::materialSignature(const Draw &draw) {
    const auto &m = *draw.material;
    uint64_t hash = m.texture->fingerprint();
    auto add = [&](const auto &v) { hash = hashBytes(&v, sizeof(v), hash); };
    add(m.tint);
    add(m.emission);
    add(m.suppressEmissionLight);
    add(m.roughness);
    add(m.metallic);
    add(m.pbr);
    add(m.normalEncoding);
    if (m.roughnessTexture)
      add(m.roughnessTexture->fingerprint());
    if (m.metallicTexture)
      add(m.metallicTexture->fingerprint());
    if (m.normalTexture)
      add(m.normalTexture->fingerprint());
    if (m.emissionTexture)
      add(m.emissionTexture->fingerprint());
    add(m.compare);
    add(m.reference);
    add(m.drawAlpha);
    add(m.translucent);
    add(m.transmission);
    add(m.filter);
    add(m.wrapU);
    add(m.wrapV);
    add(m.rows);
    add(m.columns);
    add(m.fps);
    const auto &b = draw.blend;
    add(b.enabled);
    add(b.mask);
    add(b.colorOp);
    add(b.color1);
    add(b.color2);
    add(b.alphaOpTexture);
    add(b.alpha1);
    add(b.alpha2);
    add(b.factor);
    add(draw.ignoreAlpha);
    add(draw.decal);
    add(draw.alphaOverride);
    add(draw.compare);
    add(draw.reference);
    add(draw.doubleSided);
    return hash;
  }
  const TemporalHistory::SurfaceHistory *TemporalHistory::previous(const Draw &draw) const {
    const auto *p = previousGeometry(draw);
    return p && p->material == materialSignature(draw) ? p : nullptr;
  }
  const TemporalHistory::SurfaceHistory *TemporalHistory::previousGeometry(const Draw &draw) const {
    if (m_reset || !draw.instanceId)
      return nullptr;
    auto it = m_previous.find({draw.instanceId, draw.instanceIndex, draw.surfaceIndex});
    if (it == m_previous.end())
      return nullptr;
    const auto &p = it->second;
    // Retained meshes preserve vertex identity, including changed bones. Streamed
    // replacements need both an explicit correspondence guarantee and exact
    // topology lineage; an update/storage hint alone is insufficient.
    const bool retained = p.draw.mesh == draw.mesh && p.draw.surface == draw.surface;
    const bool corresponding = draw.mesh && p.draw.mesh && draw.mesh->motionCorrespondence &&
                               p.draw.mesh->motionCorrespondence && draw.mesh->updateStream &&
                               draw.mesh->updateStream == p.draw.mesh->updateStream;
    if ((!retained && !corresponding) || p.draw.classification != draw.classification)
      return nullptr;
    return &p;
  }
  void TemporalHistory::stage(const Draw &draw, uint32_t textureFrame) {
    if (draw.instanceId)
      m_pending[{draw.instanceId, draw.instanceIndex, draw.surfaceIndex}] = {draw, materialSignature(draw),
                                                                             textureFrame};
  }
  void TemporalHistory::commit() {
    m_previous = std::move(m_pending);
    m_epoch = m_nextEpoch;
    m_integrator = m_nextIntegrator;
    m_width = m_nextWidth;
    m_height = m_nextHeight;
    ++m_frames;
  }
  void TemporalHistory::clear() {
    m_previous.clear();
    m_pending.clear();
    m_frames = 0;
    m_reset = true;
  }
} // namespace remix::scene
