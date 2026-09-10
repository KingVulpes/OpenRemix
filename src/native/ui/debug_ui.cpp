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
          ImGui::Checkbox("Native resolution TAA", &m_traceOptions.taa);
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
          if (s.gpuTimingsAvailable)
            ImGui::Text("GPU builds %.3f ms; tracing %.3f ms", s.gpuBuildMilliseconds,
                        s.gpuTraceMilliseconds);
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
