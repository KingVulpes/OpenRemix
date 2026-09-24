#include "runtime/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawRenderingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive =
        m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
    int backend = int(settings.backend);
    if (ui::Combo("Requested backend", &backend, "Raster\0Path trace\0"))
      settings.backend = uint32_t(backend);
    ImGui::TextWrapped("Active backend: %s", m_backend ? "Path trace" : "Raster");
    if (!m_traceUnavailable.empty())
      ImGui::TextWrapped("Path tracing unavailable: %s", m_traceUnavailable.c_str());
    ui::Checkbox("VSync", &settings.vsync);
    if (ImGui::CollapsingHeader("Sampling and visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
      int primary = options.rasterPrimary ? 0 : 1;
      if (ui::Combo("Primary visibility", &primary, "Rasterized G-buffer\0Ray traced\0"))
        options.rasterPrimary = primary == 0;
      int samples = int(options.samples), bounces = int(options.bounces);
      if (ui::SliderInt("Samples / pixel", &samples, 1, 16))
        options.samples = uint32_t(samples);
      if (ui::SliderInt("Maximum indirect bounces", &bounces, 0, 4))
        options.bounces = uint32_t(bounces);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum traced indirect surface interactions per sample.\nMisses and cache hits "
                          "can end tracing earlier; SHARC may supply a longer cached tail.");
      ui::Checkbox("Adaptive indirect termination", &options.russianRoulette);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Probabilistically stop low-contribution paths and compensate survivors.\nMay "
                          "reduce tracing work but increase noise; the maximum bounce limit always applies.");
      ImGui::BeginDisabled(!options.russianRoulette);
      int minimumBounces = int(options.rouletteMinBounces);
      if (ui::SliderInt("Bounces before adaptive termination", &minimumBounces, 1, 4))
        options.rouletteMinBounces = uint32_t(minimumBounces);
      ImGui::EndDisabled();
      if (options.russianRoulette && options.rouletteMinBounces >= options.bounces)
        ImGui::TextWrapped("%s", "Adaptive termination inactive: minimum reaches the maximum bounce limit.");
      if (options.russianRoulette && m_pathTracer && !m_pathTracer->adaptiveTerminationAvailable())
        ImGui::TextWrapped("%s", "Adaptive termination unavailable: using fixed-limit tracing.");
      ImGui::BeginDisabled(rrActive || options.indirectClamp > 0);
      ui::Checkbox("Reuse water indirect samples", &options.waterIndirectReuse);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shares diffuse lighting across small flat water patches with TAA or FSR.\nEdges, "
                          "overlapping layers and raw reference rendering retain full sampling.");
      ImGui::BeginDisabled(rrActive || options.localDirectClamp > 0 || options.indirectClamp > 0);
      ui::Checkbox("Experimental PBR lighting reuse", &options.pbrLocalReuse);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reuses local lighting on closely matching rough surfaces.\nMay change lighting "
                          "and temporal noise; disabled by default.");
    }
    if (ImGui::CollapsingHeader("Reconstruction", ImGuiTreeNodeFlags_DefaultOpen)) {
      int upscaler = int(options.upscaler);
      if (ui::Combo("Requested upscaler", &upscaler, "Off\0Auto\0FSR 3.1\0DLSS\0"))
        options.upscaler = upscale::Mode(upscaler);
      const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
      if (options.upscaler != upscale::Mode::Off) {
        const bool amd = options.upscaler == upscale::Mode::Fsr31;
        auto &savedQuality = amd ? options.fsrQuality : options.dlssQuality;
        int quality = int(savedQuality);
        if (ui::Combo("Quality preset", &quality,
                      "Native AA / DLAA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
          savedQuality = uint32_t(quality);
        if (amd)
          ui::SliderFloat("FSR sharpening", &options.fsrSharpness, 0.f, 1.f, "%.2f");
        else
          ui::Checkbox("DLSS Ray Reconstruction", &options.dlssRayReconstruction);
      }
      ImGui::TextWrapped("Active: %s", upscale::name(fsr.backend));
      ImGui::TextWrapped("%ux%u -> %ux%u | reconstruction %.2f ms", fsr.render.width, fsr.render.height,
                         fsr.display.width, fsr.display.height, fsr.milliseconds);
      if (!fsr.failure.empty())
        ImGui::TextWrapped("%s", fsr.failure.c_str());
      if (rrActive)
        ImGui::TextWrapped("Ray Reconstruction manages denoising and independent sampling. Saved settings "
                           "resume when it is disabled.");
    }
  }
} // namespace remix
