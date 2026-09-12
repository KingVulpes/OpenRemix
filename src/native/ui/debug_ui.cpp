#include "runtime/runtime.h"
#include "ui/context.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace remix {
  using ui::UiScope;

  void RemixNativeRuntime::drawUi(VkCommandBuffer commandBuffer) {
    UiScope scope(m_gui);
    if (commandBuffer) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
      return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (m_uiState != REMIXAPI_UI_STATE_NONE) {
      ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSize(ImVec2(470, 580), ImGuiCond_FirstUseEver);
      bool open = true;
      if (ImGui::Begin("Remix", &open)) {
        ImGui::TextWrapped("%s", m_properties.deviceName);
        ImGui::Text("Vulkan %u.%u.%u", VK_VERSION_MAJOR(m_properties.apiVersion),
                    VK_VERSION_MINOR(m_properties.apiVersion), VK_VERSION_PATCH(m_properties.apiVersion));
        const auto &io = ImGui::GetIO();
        ImGui::Text("Presentation: %.2f ms / %.1f FPS", io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f,
                    io.Framerate);
        ImGui::Text("Scene: %zu surface draws", m_raster.pendingDrawCount());
        int backend = int(m_requestedBackend);
        if (ImGui::Combo("Backend", &backend, "Raster\0Path trace\0"))
          selectBackend(backend ? "pathtrace" : "raster");
        if (!m_traceUnavailable.empty())
          ImGui::TextWrapped("Path tracing: %s", m_traceUnavailable.c_str());
        if (m_backend || m_requestedBackend) {
          int primary = m_traceOptions.rasterPrimary ? 0 : 1;
          if (ImGui::Combo("Primary visibility", &primary, "Rasterized G-buffer\0Ray traced\0"))
            m_traceOptions.rasterPrimary = primary == 0;
          int samples = int(m_traceOptions.samples), bounces = int(m_traceOptions.bounces);
          if (ImGui::SliderInt("Samples / pixel", &samples, 1, 16))
            m_traceOptions.samples = uint32_t(samples);
          if (ImGui::SliderInt("Indirect bounces", &bounces, 0, 4))
            m_traceOptions.bounces = uint32_t(bounces);
          ImGui::Checkbox("REBLUR diffuse SH", &m_traceOptions.denoiser);
          ImGui::Checkbox("SIGMA sun shadows", &m_traceOptions.sigma);
          const auto shadows = shadowStatistics();
          ImGui::Text("SIGMA: %s", shadows.active ? "active" : "inactive");
          if (shadows.active && shadows.gpuTimingsAvailable)
            ImGui::Text("Sun shadow filter %.3f ms", shadows.gpuSigmaMilliseconds);
          if (shadows.unavailableReason[0])
            ImGui::TextWrapped("%s", shadows.unavailableReason);
          bool overrideSize = m_traceOptions.sunAngularDiameter >= 0;
          if (ImGui::Checkbox("Override sun size", &overrideSize))
            m_traceOptions.sunAngularDiameter = overrideSize ? .533f : -1.f;
          if (overrideSize)
            ImGui::SliderFloat("Sun diameter (degrees)", &m_traceOptions.sunAngularDiameter, 0.f, 3.f, "%.3f");
          int shadowView = int(m_traceOptions.shadowDebug);
          if (ImGui::Combo("Shadow view", &shadowView, "Final lighting\0Raw visibility\0SIGMA visibility\0"))
            m_traceOptions.shadowDebug = uint32_t(shadowView);
          int upscaler = m_traceOptions.fsr ? 1 : 0;
          ImGui::Checkbox("Prism tonemapping", &m_traceOptions.prism);
          ImGui::Checkbox("Gamma 2.2 output", &m_traceOptions.gamma22);
          if (ImGui::Combo("Upscaling", &upscaler, "Off\0FSR 3.1\0"))
            m_traceOptions.fsr = upscaler != 0;
          const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
          if (m_traceOptions.fsr) {
            int quality = int(m_traceOptions.fsrQuality);
            if (ImGui::Combo("FSR preset", &quality, "Native AA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
              m_traceOptions.fsrQuality = uint32_t(quality);
            ImGui::SliderFloat("FSR sharpening", &m_traceOptions.fsrSharpness, 0.f, 1.f, "%.2f");
            if (fsr.active)
              ImGui::Text("FSR: %ux%u -> %ux%u | %.2f ms", fsr.render.width, fsr.render.height,
                          fsr.display.width, fsr.display.height, fsr.milliseconds);
            if (!fsr.failure.empty()) ImGui::TextWrapped("%s", fsr.failure.c_str());
          }
          ImGui::BeginDisabled(fsr.active);
          ImGui::Checkbox("Native resolution TAA", &m_traceOptions.taa);
          ImGui::EndDisabled();
          ImGui::Checkbox("Reuse water indirect samples", &m_traceOptions.waterIndirectReuse);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shares diffuse lighting across small flat water patches with TAA or FSR.\nEdges, overlapping layers and raw reference rendering retain full sampling.");
          ImGui::Checkbox("NRD validation", &m_traceOptions.validation);
          ImGui::Checkbox("Progressive accumulation", &m_traceOptions.accumulate);
          if (m_traceOptions.accumulate)
            ImGui::TextUnformatted("Raw reference: requested denoiser and TAA resume afterward.");
          auto denoising = denoisingStatistics();
          if (denoising.unavailableReason[0])
            ImGui::TextWrapped("%s", denoising.unavailableReason);
          ImGui::Text("NRD %s, TAA %s | history %llu, resets %llu", denoising.activeDenoiser ? "on" : "off",
                      denoising.activeTaa ? "on" : "off", (unsigned long long)denoising.historyFrames,
                      (unsigned long long)denoising.historyResets);
          ImGui::Text("Confidence %.2f / NRD %.2f / TAA %.2f ms | %.1f MiB",
                      denoising.gpuConfidenceMilliseconds, denoising.gpuDenoiseMilliseconds,
                      denoising.gpuTaaMilliseconds, double(denoising.memoryBytes) / (1024 * 1024));
          ImGui::Checkbox("Force dynamic TLAS rebuild", &m_traceOptions.forceRebuild);
          ImGui::Checkbox("Sky illumination", &m_traceOptions.sky);
          if (ImGui::CollapsingHeader("Local lighting")) {
            ImGui::Checkbox("Local lights", &m_traceOptions.localLights);
            ImGui::Checkbox("Torch lights", &m_traceOptions.torchLights);
            ImGui::Checkbox("Emissive lighting", &m_traceOptions.emissiveLights);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Controls light cast onto the world. Emissive textures remain visible.");
            ImGui::Checkbox("Adaptive local-light sampling", &m_traceOptions.adaptiveLocalSampling);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Uses the configured local-light budget as a baseline and adds up to four samples near important lights. Emissive candidates are reduced in low-contribution pixels.");
            ImGui::Checkbox("Stabilize animated emissive lighting", &m_traceOptions.stabilizeEmissiveLighting);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Experimental: smooths animation-driven cast light over half a second. Visible emissive textures keep animating.\nCombined lighting and the shadow-ray budget are preserved.");
            ImGui::Checkbox("Improve nearby emissive lighting", &m_traceOptions.emissiveSolidAngleSampling);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Experimental: samples nearby area lights with the same shadow-ray count.\nAdds some GPU work; disable to compare performance.");
            int candidates = int(m_traceOptions.emissiveCandidates);
            if (ImGui::SliderInt("Emissive candidates", &candidates, 1, 8))
              m_traceOptions.emissiveCandidates = uint32_t(candidates);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Compares unshadowed candidates at opaque primary surfaces in larger emissive light sets.\nKeeps one shadow ray per local light sample; 1 restores basic BVH sampling.");
            ImGui::Checkbox("Denoise local lighting", &m_traceOptions.localDenoiser);
            int separation = int(m_traceOptions.separateLocalLighting);
            const char *separationModes[] = {"Combined", "Separate - shared ray budget", "Separate - samples per family"};
            if (ImGui::Combo("Torch / emissive lighting", &separation, separationModes, 3))
              m_traceOptions.separateLocalLighting = uint32_t(separation);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Separates direct torch and emissive sampling and denoiser histories.\nShared budget keeps the shadow-ray budget. Per-family mode can trace twice as many.\nIndirect lighting remains combined.");
            int samples = int(m_traceOptions.localSamples);
            if (ImGui::SliderInt("Local light samples", &samples, 1, 16))
              m_traceOptions.localSamples = uint32_t(samples);
            ImGui::SliderFloat("Local light strength", &m_traceOptions.localIntensity, 0.f, 4.f);
            ImGui::SliderFloat("Light reach (0 = unlimited)", &m_traceOptions.localRange, 0.f, 64.f, "%.1f");
            if (m_traceOptions.localRange > 0 && m_traceOptions.localRange < .01f)
              m_traceOptions.localRange = .01f;
            ImGui::Checkbox("Guide by recent visibility", &m_traceOptions.localGuiding);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Guides opaque direct-light samples while retaining exploration.\nOne active light needs no visibility history.\nEmissive reservoirs use visible groups while checking shadows afresh.");
            const auto guiding = localLightGuidingStatistics();
            if (guiding.active)
              ImGui::Text("Guided %u / %u observed samples | %.1f MiB history", guiding.sampledGuided,
                          guiding.sampledQueries, double(guiding.memoryBytes) / (1024 * 1024));
            if (guiding.unavailableReason[0])
              ImGui::TextWrapped("%s", guiding.unavailableReason);
            const auto local = localLightStatistics();
            ImGui::Text("%u active / %u lights | tree depth %u", local.activeLights, local.lightCount, local.treeDepth);
            ImGui::Text("BVH builds %llu / refits %llu | %.3f ms CPU", (unsigned long long)local.builds,
                        (unsigned long long)local.refits, local.cpuUpdateMilliseconds);
            if (local.denoiserActive && local.gpuTimingsAvailable)
              ImGui::Text("Local light filter %.3f ms", local.gpuDenoiseMilliseconds);
            if (local.unavailableReason[0]) ImGui::TextWrapped("%s", local.unavailableReason);
          }
          if (ImGui::CollapsingHeader("SHARC radiance cache")) {
            ImGui::Checkbox("SHARC", &m_traceOptions.sharc);
            ImGui::Checkbox("Show cached radiance (raw)", &m_traceOptions.sharcDebug);
            if (ImGui::Button("Reset cache"))
              ++m_traceOptions.sharcReset;
            int scale = int(m_traceOptions.sharcSceneScale), downscale = int(m_traceOptions.sharcDownscale);
            if (ImGui::SliderInt("Cache scene scale", &scale, 1, 200))
              m_traceOptions.sharcSceneScale = uint32_t(scale);
            if (ImGui::SliderInt("Update downscale", &downscale, 1, 16))
              m_traceOptions.sharcDownscale = uint32_t(downscale);
            auto cache = sharcStatistics();
            ImGui::Text("%s | %u / %u entries | %.1f MiB", cache.active ? "Active" : "Inactive",
                        cache.occupiedEntries, cache.capacity, double(cache.memoryBytes) / (1024 * 1024));
            ImGui::Text("Update paths %u | sampled query hits %.1f%% | resets %llu", cache.updatePaths,
                        cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
                        (unsigned long long)cache.resets);
            if (cache.gpuTimingsAvailable)
              ImGui::Text("Update %.3f / resolve %.3f ms", cache.gpuUpdateMilliseconds, cache.gpuResolveMilliseconds);
            if (cache.unavailableReason[0])
              ImGui::TextWrapped("%s", cache.unavailableReason);
            if (m_traceOptions.accumulate)
              ImGui::TextUnformatted("Progressive reference rendering bypasses SHARC.");
          }
          ImGui::Checkbox("Override daylight", &m_traceOptions.overrideLight);
          if (m_traceOptions.overrideLight) {
            ImGui::SliderFloat3("Light direction", m_traceOptions.lightDirection, -1, 1);
            ImGui::ColorEdit3("Light color", m_traceOptions.lightColor);
            ImGui::SliderFloat("Light intensity", &m_traceOptions.lightIntensity, 0, 20);
          }
          auto s = tracingStatistics();
          ImGui::Text("Accumulated frames: %llu", (unsigned long long)s.accumulationCount);
          ImGui::Text("BLAS builds / refits / compactions: %llu / %llu / %llu",
                      (unsigned long long)s.blasBuilds, (unsigned long long)s.blasRefits,
                      (unsigned long long)s.blasCompactions);
          ImGui::Text("Static TLAS builds: %llu", (unsigned long long)s.staticTlasBuilds);
          ImGui::Text("Dynamic TLAS builds / refits: %llu / %llu", (unsigned long long)s.dynamicTlasBuilds,
                      (unsigned long long)s.dynamicTlasRefits);
          ImGui::Text("Rebuild reason: %u; refits since build: %u", s.dynamicRebuildReason,
                      s.dynamicRefitCount);
          ImGui::Text("Acceleration memory: %.1f MiB", double(s.accelerationBytes) / (1024 * 1024));
          if (s.gpuTimingsAvailable) {
            ImGui::Text("GPU builds %.3f ms; tracing %.3f ms", s.gpuBuildMilliseconds,
                        s.gpuTraceMilliseconds);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Tracing includes sun and local shadows, indirect rays, and transparent sheets.\nDenoiser timings are reported separately.");
          }
        }
        ImGui::ColorEdit3("Clear color", m_clearColor);
        ImGui::Checkbox("VSync", &m_vsync);
        ImGui::InputText("Input", m_input, sizeof(m_input));
      }
      ImGui::End();
      if (!open) {
        m_uiState = REMIXAPI_UI_STATE_NONE;
      }
    }
    ImGui::Render();
    const auto &io = ImGui::GetIO();
    m_window.setGuiCapture(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureKeyboard,
                           m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureMouse);
    m_window.syncTextInput(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantTextInput);
    for (auto *texture : ImGui::GetPlatformIO().Textures) {
      if (texture->Status != ImTextureStatus_OK) {
        ImGui_ImplVulkan_UpdateTexture(texture);
      }
    }
  }
} // namespace remix
