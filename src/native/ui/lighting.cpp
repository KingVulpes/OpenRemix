#include "runtime/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawLightingTab(config::Settings &settings) {
    auto &options = settings.trace;
    const bool rrActive =
        m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
    if (ImGui::CollapsingHeader("Sun and sky", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("Override daylight", &options.overrideLight);
      if (options.overrideLight) {
        ui::SliderFloat3("Light direction", options.lightDirection, -1, 1);
        ui::ColorEdit3("Light color", options.lightColor);
        ui::SliderFloat("Light intensity", &options.lightIntensity, 0, 20);
      }
      bool overrideSize = options.sunAngularDiameter >= 0;
      if (ui::Checkbox("Override sun size", &overrideSize))
        options.sunAngularDiameter = overrideSize ? .533f : -1.f;
      if (overrideSize)
        ui::SliderFloat("Sun diameter (degrees)", &options.sunAngularDiameter, 0.f, 3.f, "%.3f");
      ui::Checkbox("Sky illumination", &options.sky);
    }
    if (ImGui::CollapsingHeader("Local lighting")) {
      ui::Checkbox("Local lights", &options.localLights);
      ui::Checkbox("Torch lights", &options.torchLights);
      ui::Checkbox("Emissive lighting", &options.emissiveLights);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Controls light cast onto the world. Emissive textures remain visible.");
      ui::Checkbox("Adaptive local-light sampling", &options.adaptiveLocalSampling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smoothly raises the local-light sample budget toward four near important light "
                          "clusters. Configured sample and emissive candidate counts remain minimums.");
      ui::Checkbox("Stabilize animated emissive lighting", &options.stabilizeEmissiveLighting);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Experimental: smooths animation-driven cast light over half a second. Visible emissive textures "
            "keep animating.\nCombined lighting and the shadow-ray budget are preserved.");
      ui::Checkbox("Improve nearby emissive lighting", &options.emissiveSolidAngleSampling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Experimental: samples nearby area lights with the same shadow-ray count.\nAdds "
                          "some GPU work; disable to compare performance.");
      int candidates = int(options.emissiveCandidates);
      if (ui::SliderInt("Emissive candidates", &candidates, 1, 8))
        options.emissiveCandidates = uint32_t(candidates);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Compares unshadowed candidates at opaque primary surfaces in larger emissive light sets.\nKeeps "
            "one shadow ray per local light sample; 1 restores basic BVH sampling.");
      int sphereCandidates = int(options.sphereCandidates);
      if (ui::SliderInt("Light candidates (no emissives)", &sphereCandidates, 1, 8))
        options.sphereCandidates = uint32_t(sphereCandidates);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Scenes lit only by API lights: compares unshadowed candidates (spot cones included) before the\n"
            "one shadow ray. Helps a dominant nearby light (a flashlight) among many distant ones.");
      ImGui::BeginDisabled(rrActive);
      ui::Checkbox("Blue noise local lights", &options.blueNoiseLocalLights);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uses blue noise for primary torch and emissive sample positions with local "
                          "denoising.\nPreserves candidate and shadow-ray budgets.");
      ImGui::BeginDisabled(rrActive);
      int separation = int(options.separateLocalLighting);
      const char *separationModes[] = {"Combined", "Separate - shared ray budget",
                                       "Separate - samples per family", "Combined - separate pass"};
      if (ui::Combo("Torch / emissive lighting", &separation, separationModes, 4))
        options.separateLocalLighting = uint32_t(separation);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Separate families use distinct direct sampling and denoiser histories.\nShared budget keeps the "
            "shadow-ray budget. Per-family mode can trace twice as many.\nCombined separate pass retains the "
            "combined sampler and budget in a dedicated dispatch.\nIndirect lighting remains combined.");
      ImGui::EndDisabled();
      if (rrActive)
        ImGui::TextWrapped("%s", "RR samples each pixel; shared family budgets use combined sampling.");
      int samples = int(options.localSamples);
      if (ui::SliderInt("Local light samples", &samples, 1, 16))
        options.localSamples = uint32_t(samples);
      ui::SliderFloat("Local light strength", &options.localIntensity, 0.f, 4.f);
      ui::SliderFloat("Light reach (0 = unlimited)", &options.localRange, 0.f, 64.f, "%.1f");
      if (options.localRange > 0 && options.localRange < .01f)
        options.localRange = .01f;
      {
        // Logarithmic: hosts range from inches (0.0254) to meters and beyond.
        ui::DragFloat("Scene scale (m per unit)", &options.sceneScale, .0005f, .0001f, 1000, "%.4f",
                      ImGuiSliderFlags_Logarithmic);
        options.sceneScale = std::clamp(options.sceneScale, .0001f, 1000.f);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Meters per world unit (1 for meter-scale hosts, 0.0254 for Source inches).\n"
                            "Converts the denoiser's hit distances and the visibility-guide cell size.");
      }
      ui::Checkbox("ReSTIR DI (local lights)", &options.restir);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reservoir resampling of primary local lighting: light samples are reused across\n"
                          "frames and neighboring pixels, and occluded ones discarded. Needs the NRD denoiser\n"
                          "(not used with DLSS Ray Reconstruction). Uses the separate local-lighting pass.");
      if (options.restir) {
        int candidates = int(options.restirCandidates), spatial = int(options.restirSpatialSamples),
            history = int(options.restirTemporalHistory);
        if (ui::SliderInt("ReSTIR candidates", &candidates, 1, 32))
          options.restirCandidates = uint32_t(candidates);
        if (ui::SliderInt("ReSTIR spatial samples", &spatial, 0, 8))
          options.restirSpatialSamples = uint32_t(spatial);
        ui::SliderFloat("ReSTIR spatial radius", &options.restirSpatialRadius, 1, 100, "%.0f px");
        if (ui::SliderInt("ReSTIR temporal history", &history, 0, 64))
          options.restirTemporalHistory = uint32_t(history);
        ui::Checkbox("ReSTIR initial visibility", &options.restirInitialVisibility);
        ui::Checkbox("ReSTIR permutation sampling", &options.restirPermutation);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Reuses a shuffled neighbor's history each frame: less temporally\n"
                            "correlated (frozen) noise for the denoiser.");
      }
      ui::Checkbox("Cull lights by visibility sets", &options.lightVisibilityCulling);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When the host supplies its map's potentially visible sets, lights in rooms that\n"
                          "cannot be seen from a surface are never sampled for it.");
      ui::Checkbox("Guide by recent visibility", &options.localGuiding);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Guides opaque direct-light samples while retaining exploration.\nOne active light needs no "
            "visibility history.\nEmissive reservoirs use visible groups while checking shadows afresh.");
      ImGui::BeginDisabled(!options.localGuiding);
      ui::DragFloat("Guide cell size", &options.localGuideCellSize, .01f, .05f, 100, "%.2f m");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Size of a visibility cell in meters (converted with the scene scale).");
      options.localGuideCellSize = std::clamp(options.localGuideCellSize, .05f, 100.f);
      const auto guiding = localLightGuidingStatistics();
      if (guiding.active)
        ImGui::TextWrapped("Guided %u / %u observed samples | %.1f MiB history", guiding.sampledGuided,
                           guiding.sampledQueries, double(guiding.memoryBytes) / (1024 * 1024));
      if (guiding.unavailableReason[0])
        ImGui::TextWrapped("%s", guiding.unavailableReason);
      const auto local = localLightStatistics();
      ImGui::TextWrapped("%u active / %u lights | tree depth %u", local.activeLights, local.lightCount,
                         local.treeDepth);
      ImGui::TextWrapped("BVH builds %llu / refits %llu | %.3f ms CPU", (unsigned long long)local.builds,
                         (unsigned long long)local.refits, local.cpuUpdateMilliseconds);
      if (local.denoiserActive && local.gpuTimingsAvailable)
        ImGui::TextWrapped("Local light filter %.3f ms", local.gpuDenoiseMilliseconds);
      if (local.unavailableReason[0])
        ImGui::TextWrapped("%s", local.unavailableReason);
    }
    if (ImGui::CollapsingHeader("SHARC radiance cache")) {
      ui::Checkbox("SHARC", &options.sharc);
      ui::Checkbox("Show cached radiance (raw)", &options.sharcDebug);
      if (ImGui::Button("Reset cache"))
        ++m_sharcReset;
      int scale = int(options.sharcSceneScale), downscale = int(options.sharcDownscale);
      if (ui::SliderInt("Cache scene scale", &scale, 1, 200))
        options.sharcSceneScale = uint32_t(scale);
      if (ui::SliderInt("Update downscale", &downscale, 1, 16))
        options.sharcDownscale = uint32_t(downscale);
      auto cache = sharcStatistics();
      ImGui::TextWrapped("%s | %u / %u entries | %.1f MiB",
                         cache.active ? (cache.updatePaths ? "Caching" : "Enabled (idle)") : "Inactive",
                         cache.occupiedEntries, cache.capacity, double(cache.memoryBytes) / (1024 * 1024));
      ImGui::TextWrapped("Update paths %u | sampled query hits %.1f%% | resets %llu", cache.updatePaths,
                         cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
                         (unsigned long long)cache.resets);
      if (cache.gpuTimingsAvailable)
        ImGui::TextWrapped("Update %.3f / resolve %.3f ms", cache.gpuUpdateMilliseconds,
                           cache.gpuResolveMilliseconds);
      if (cache.unavailableReason[0])
        ImGui::TextWrapped("%s", cache.unavailableReason);
      if (cache.active && !cache.updatePaths)
        ImGui::TextWrapped("No cache update paths. Requires static opaque diffuse or rough PBR surfaces.");
      ImGui::TextWrapped(
          "Sharp PBR surfaces continue tracing; rough PBR surfaces can reuse cached lighting.");
      if (options.accumulate)
        ImGui::TextWrapped("%s", "Progressive reference rendering bypasses SHARC.");
    }
  }
} // namespace remix
