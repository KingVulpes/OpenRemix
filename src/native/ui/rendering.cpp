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
    ui::Checkbox("Asynchronous present", &settings.asyncPresent);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Present captures the frame and returns; OpenRemix's render thread prepares, records,\n"
                        "submits and presents it while the game starts its next frame. At most one frame is\n"
                        "in flight: the next Present waits only for that handoff. Statistics describe the last\n"
                        "completed frame. Off: the whole frame runs inside Present, as before.\n"
                        "Applies at the next Present (the render thread is drained first).");
    if (settings.asyncPresent && m_headless)
      ImGui::TextDisabled("%s", "The headless editor always presents synchronously.");
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
      ui::Checkbox("Denoise translucent surfaces", &options.translucencyDenoiser);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filters single-layer surface lighting with its own motion and depth.\n"
                         "Uses NRD, including before DLSS RR; overlapping layers retain their existing rendering.");
      if (options.translucencyDenoiser)
        ImGui::TextDisabled("Surface filter: %s", m_pathTracer && m_pathTracer->translucencyDenoiserActive()
                                               ? "active" : "inactive (requires NRD and translucent surfaces)");
      ImGui::BeginDisabled(rrActive || options.indirectClamp > 0);
      ui::Checkbox("Reuse opaque lighting through translucency", &options.translucencyReuse);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses filtered lighting at closely matching opaque ray hits.\n"
                         "Requires NRD; unmatched surfaces and raw references keep full tracing.\n"
                         "Optional: its performance benefit depends on the scene.");
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
    if (ImGui::CollapsingHeader("Memory")) {
      ui::Checkbox("Keep BC textures compressed", &settings.compressedTextures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Upload BC1-BC7 textures as GPU BC images instead of decoding them to RGBA on the CPU.\n"
                          "Uses 4-8x less VRAM and system memory and removes the decode from CreateTexture.\n"
                          "Applies to textures created after the change; no restart needed.");
      if (settings.compressedTextures && !m_textureCompression)
        ImGui::TextWrapped("%s", "This GPU cannot sample BC formats: textures are decoded once at upload.");
      ui::Checkbox("Report VRAM statistics", &settings.vramStatistics);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Answer GetVramStats with OpenRemix's texture, geometry, acceleration-structure\n"
                          "and denoiser allocations, plus the driver's device-local usage and budget.");
      ui::Checkbox("Report memory pressure", &settings.memoryPressure);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Answer remixapi_GetMemoryPressure from the driver's device-local usage and budget:\n"
                          "levels at 90/94/97%% of the budget; at the critical level the host is asked to\n"
                          "release far content. Off: the host always reads no pressure.");
      if (settings.memoryPressure) {
        if (!m_memoryBudget)
          ImGui::TextWrapped("%s", "VK_EXT_memory_budget is unavailable: no pressure is reported.");
        const auto pressure = m_memoryPressure.sample();
        static const char *const levels[] = {"none", "elevated", "high", "critical"};
        ImGui::TextDisabled("Pressure: %s | %.0f / %.0f MiB | release %.0f MiB%s", levels[pressure.level & 3],
                            pressure.usageBytes / 1048576.0, pressure.budgetBytes / 1048576.0,
                            pressure.bytesToRelease / 1048576.0,
                            pressure.requestHostRelease ? " | host asked to release" : "");
      }
      const auto memory = renderer().memoryStatistics();
      ImGui::TextDisabled("Textures: %llu, %.1f MiB (%.1f MiB BC) | geometry %.1f MiB",
                          (unsigned long long)memory.textureCount, memory.textureBytes / 1048576.0,
                          memory.compressedTextureBytes / 1048576.0, memory.geometryBytes / 1048576.0);
    }
    if (ImGui::CollapsingHeader("Geometry")) {
      ui::Checkbox("Refresh mesh geometry in place", &settings.refreshGeometry);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Honour remixapi_MeshInfoRefreshGeometryEXT: a host refreshing a mesh's vertices\n"
                          "(face morphs, particles) keeps its identity, so the path tracer refits the\n"
                          "existing BLAS and motion vectors follow the surface. Off: the refresh replaces\n"
                          "the mesh (full BLAS build). Applies to refreshes after the change.");
      ui::Checkbox("Persistent static scene", &settings.persistentStatic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Retained static instances (DrawRetainedInstances) stay in the path tracer's own\n"
                          "copy, updated only where they change: unchanged ones cost no per-frame work in\n"
                          "the draw list, scene preparation, surface table, temporal history or scene\n"
                          "signature. The same output as without it. Path tracing only; applies from the\n"
                          "next frame.");
      ImGui::BeginDisabled(!settings.persistentStatic);
      ui::Checkbox("Validate persistent static scene", &settings.persistentStaticValidation);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Diagnostic: every frame, rebuild the kept static tables from scratch and compare\n"
                          "them. The first difference is logged and shown in Diagnostics > Present CPU\n"
                          "breakdown. Costs about what the setting saves.");
      ImGui::EndDisabled();
    }
  }
} // namespace remix
