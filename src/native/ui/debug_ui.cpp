#include "runtime/runtime.h"
#include "ui/context.h"

#include <algorithm>
#include <cmath>
#include <utility>
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
          const bool rrActive = m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
          int primary = m_traceOptions.rasterPrimary ? 0 : 1;
          if (ImGui::Combo("Primary visibility", &primary, "Rasterized G-buffer\0Ray traced\0"))
            m_traceOptions.rasterPrimary = primary == 0;
          if (ImGui::CollapsingHeader("Diagnostics")) {
            int view = int(m_traceOptions.debugView);
            if (ImGui::Combo("Debug view", &view,
                "Final lighting\0Albedo\0Shading normals\0Triangle IDs\0Depth bands\0World position grid\0"
                "Raw local lighting\0Raw indirect lighting\0Motion vectors\0Motion validity\0"
                "Indirect lighting confidence\0TAA current-frame weight\0"))
              m_traceOptions.debugView = uint32_t(view);
            static const char *help[] = {
              "Start with Triangle IDs and Shading normals at the problem angle, then compare Raw local lighting. Compare both Primary visibility options above.",
              "Unlit base color of the first visible surface. Flicker here points to visibility or material data.",
              "World-space shading normals: RGB = XYZ mapped to 0..1. Includes authored vertex normals and material normal maps; missing vertex normals fall back to flat faces.",
              "Colors identify submitted triangles. Coplanar surfaces swapping visibility change color. Remeshing can also change IDs; palette collisions are possible.",
              "Distance from the camera, repeating black to white every 4 world units. Sudden patches can reveal visibility or position jumps.",
              "World position as RGB, repeating every world unit. Look for moving discontinuities inside a flat face.",
              "Unfiltered direct local illumination on opaque receivers, including torches and emissives, before albedo and transparent tint. Uses the current display tonemapper.",
              "Unfiltered indirect illumination before albedo and transparent tint. Includes SHARC when enabled; zero bounces is black.",
              "Opaque motion guides: red/green = horizontal/vertical pixels, gray = zero, full range = +/-16 pixels. Magenta = invalid motion.",
              "Opaque motion correspondence: green = available, red = invalid/reset. This precedes per-pixel depth/normal history rejection.",
              "REBLUR indirect confidence: white = high trust, black = low. This is not the local-light denoiser's history.",
              "Actual native TAA blend weight: white = current frame only; darker = more history. Unavailable with FSR or TAA off."
            };
            ImGui::TextWrapped("%s", help[m_traceOptions.debugView]);
            if (m_traceOptions.debugView) {
              ImGui::TextWrapped("Diagnostics display live internal pixels without temporal filtering. Lighting histories and requested settings keep running. FSR pixels are enlarged without filtering.");
              ImGui::TextWrapped("Purple checkerboard means the selected signal is unavailable. Normal subpixel jitter remains visible at edges.");
            }
          }
          int samples = int(m_traceOptions.samples), bounces = int(m_traceOptions.bounces);
          if (ImGui::SliderInt("Samples / pixel", &samples, 1, 16))
            m_traceOptions.samples = uint32_t(samples);
          if (ImGui::SliderInt("Indirect bounces", &bounces, 0, 4))
            m_traceOptions.bounces = uint32_t(bounces);
          ImGui::BeginDisabled(rrActive);
          ImGui::Checkbox("REBLUR diffuse SH", &m_traceOptions.denoiser);
          ImGui::Checkbox("SIGMA sun shadows", &m_traceOptions.sigma);
          ImGui::Checkbox("Blue noise sun shadows", &m_traceOptions.blueNoiseShadows);
          ImGui::EndDisabled();
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
          int upscaler = int(m_traceOptions.upscaler);
          ImGui::Checkbox("Prism tonemapping", &m_traceOptions.prism);
          ImGui::Checkbox("Gamma 2.2 output", &m_traceOptions.gamma22);
          if (ImGui::Combo("Requested upscaler", &upscaler, "Off\0Auto\0FSR 3.1\0DLSS\0"))
            m_traceOptions.upscaler = upscale::Mode(upscaler);
          const auto fsr = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
          if (m_traceOptions.upscaler != upscale::Mode::Off) {
            const bool amd = m_traceOptions.upscaler == upscale::Mode::Fsr31;
            auto &savedQuality = amd ? m_traceOptions.fsrQuality : m_traceOptions.dlssQuality;
            int quality = int(savedQuality);
            if (ImGui::Combo("Quality preset", &quality, "Native AA / DLAA\0Quality\0Balanced\0Performance\0Ultra Performance\0"))
              savedQuality = uint32_t(quality);
            if (amd) ImGui::SliderFloat("FSR sharpening", &m_traceOptions.fsrSharpness, 0.f, 1.f, "%.2f");
            else ImGui::Checkbox("DLSS Ray Reconstruction", &m_traceOptions.dlssRayReconstruction);
          }
          ImGui::Text("Active: %s", upscale::name(fsr.backend));
          ImGui::Text("%ux%u -> %ux%u | reconstruction %.2f ms", fsr.render.width, fsr.render.height,
                      fsr.display.width, fsr.display.height, fsr.milliseconds);
          if (!fsr.failure.empty()) ImGui::TextWrapped("%s", fsr.failure.c_str());
          if (rrActive) ImGui::TextWrapped("Ray Reconstruction manages denoising and independent sampling. Saved settings resume when it is disabled.");
          ImGui::BeginDisabled(fsr.active);
          ImGui::Checkbox("Native resolution TAA", &m_traceOptions.taa);
          ImGui::EndDisabled();
          ImGui::BeginDisabled(rrActive || m_traceOptions.indirectClamp > 0);
          ImGui::Checkbox("Reuse water indirect samples", &m_traceOptions.waterIndirectReuse);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shares diffuse lighting across small flat water patches with TAA or FSR.\nEdges, overlapping layers and raw reference rendering retain full sampling.");
          ImGui::BeginDisabled(rrActive || m_traceOptions.localDirectClamp > 0 || m_traceOptions.indirectClamp > 0);
          ImGui::Checkbox("Experimental PBR lighting reuse", &m_traceOptions.pbrLocalReuse);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reuses local lighting on closely matching rough surfaces.\nMay change lighting and temporal noise; disabled by default.");
          ImGui::BeginDisabled(rrActive);
          ImGui::Checkbox("NRD validation", &m_traceOptions.validation);
          ImGui::EndDisabled();
          ImGui::Checkbox("Progressive accumulation", &m_traceOptions.accumulate);
          if (m_traceOptions.accumulate)
            ImGui::TextUnformatted("Raw reference: requested denoiser and TAA resume afterward.");
          for (auto [label, threshold] : {std::pair{"Local direct clamp", &m_traceOptions.localDirectClamp},
                                         std::pair{"Indirect clamp", &m_traceOptions.indirectClamp}}) {
            float value = *threshold;
            if (ImGui::InputFloat(label, &value, 0.f, 0.f, "%.6g") && std::isfinite(value))
              *threshold = std::clamp(value, 0.f, 1000000.f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Linear scene luminance, 0 disables (maximum 1,000,000).\nLower values suppress more brightness, including legitimate highlights.");
          }
          auto denoising = denoisingStatistics();
          const bool fireflyFilterActive = denoising.activeDenoiser && !rrActive &&
              (m_traceOptions.localDenoiser || m_traceOptions.bounces > 0) &&
              m_traceOptions.localLights && m_traceOptions.localIntensity > 0 &&
              localLightStatistics().activeLights > 0;
          ImGui::BeginDisabled(!fireflyFilterActive);
          ImGui::Checkbox("PBR local-light firefly filter (REBLUR)", &m_traceOptions.localFireflyFilter);
          ImGui::EndDisabled();
          if (!fireflyFilterActive)
            ImGui::TextUnformatted("PBR local-light firefly filter inactive: requires REBLUR receiving local lighting.");
          if (m_traceOptions.localDirectClamp > 0 || m_traceOptions.indirectClamp > 0)
            ImGui::TextUnformatted("Clamping bypasses experimental PBR lighting reuse.");
          if (m_traceOptions.indirectClamp > 0)
            ImGui::TextUnformatted("Indirect clamping bypasses water irradiance sharing.");
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
              ImGui::SetTooltip("Smoothly raises the local-light sample budget toward four near important light clusters. Configured sample and emissive candidate counts remain minimums.");
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
            ImGui::BeginDisabled(rrActive);
            ImGui::Checkbox("Denoise local lighting", &m_traceOptions.localDenoiser);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(rrActive);
            ImGui::Checkbox("Blue noise local lights", &m_traceOptions.blueNoiseLocalLights);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Uses blue noise for primary torch and emissive sample positions with local denoising.\nPreserves candidate and shadow-ray budgets.");
            ImGui::BeginDisabled(rrActive);
            int separation = int(m_traceOptions.separateLocalLighting);
            const char *separationModes[] = {"Combined", "Separate - shared ray budget", "Separate - samples per family", "Combined - separate pass"};
            if (ImGui::Combo("Torch / emissive lighting", &separation, separationModes, 4))
              m_traceOptions.separateLocalLighting = uint32_t(separation);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Separate families use distinct direct sampling and denoiser histories.\nShared budget keeps the shadow-ray budget. Per-family mode can trace twice as many.\nCombined separate pass retains the combined sampler and budget in a dedicated dispatch.\nIndirect lighting remains combined.");
            ImGui::EndDisabled();
            if (rrActive) ImGui::TextUnformatted("RR samples each pixel; shared family budgets use combined sampling.");
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
            ImGui::Text("%s | %u / %u entries | %.1f MiB", cache.active ? (cache.updatePaths ? "Caching" : "Enabled (idle)") : "Inactive",
                        cache.occupiedEntries, cache.capacity, double(cache.memoryBytes) / (1024 * 1024));
            ImGui::Text("Update paths %u | sampled query hits %.1f%% | resets %llu", cache.updatePaths,
                        cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
                        (unsigned long long)cache.resets);
            if (cache.gpuTimingsAvailable)
              ImGui::Text("Update %.3f / resolve %.3f ms", cache.gpuUpdateMilliseconds, cache.gpuResolveMilliseconds);
            if (cache.unavailableReason[0])
              ImGui::TextWrapped("%s", cache.unavailableReason);
            if (cache.active && !cache.updatePaths)
              ImGui::TextWrapped("No cache update paths. Requires static opaque diffuse or rough PBR surfaces.");
            ImGui::TextWrapped("Sharp PBR surfaces continue tracing; rough PBR surfaces can reuse cached lighting.");
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
