#include "runtime/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace remix {
  void RemixNativeRuntime::drawDiagnosticsTab(config::Settings &settings) {
    if (m_pipelines) {
      const auto s = m_pipelines->statistics();
      ImGui::Text("Pipeline preparation: %llu / %llu completed",
                  (unsigned long long)s.completed,
                  (unsigned long long)s.requested);
      ImGui::Text("Created %llu, reused %llu, failed %llu; %.1f ms compiling",
                  (unsigned long long)s.created, (unsigned long long)s.reused,
                  (unsigned long long)s.failed, s.milliseconds);
      ImGui::Text("Pending requests deduplicated: %llu",
                  (unsigned long long)s.deduplicated);
      ImGui::Text("Driver cache: %s; feedback: %s; hits: %llu",
                  s.cacheLoaded ? "loaded" : "cold",
                  s.feedback ? "supported" : "unavailable",
                  (unsigned long long)s.cacheHits);
      if (s.feedback)
        ImGui::Text("Driver creation time: %.1f ms",
                    double(s.driverNanoseconds) / 1e6);
      if (s.cacheWriteFailed)
        ImGui::TextUnformatted(
            "Pipeline cache could not be saved; rendering remains available.");
      if (m_preparation) {
        ImGui::Text("Pending configuration #%llu (profile %s)",
                    (unsigned long long)m_preparation->generation,
                    m_profiles.active().c_str());
        ImGui::Text(
            "Elapsed %.1f s; totals include jobs discovered during preparation",
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          m_preparation->started)
                .count());
      }
      if (!m_preparationFailure.empty())
        ImGui::TextWrapped("%s", m_preparationFailure.c_str());
      ImGui::Separator();
      ImGui::Text(
          "Longest presentation gap: %.1f ms (includes application idle time)",
          m_longestPresentationGap);
    }
    auto &options = settings.trace;
    const bool rrActive =
        m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
    if (ImGui::CollapsingHeader("Debug views", ImGuiTreeNodeFlags_DefaultOpen)) {
      int view = int(options.debugView);
      if (ui::Combo(
              "Debug view", &view,
              "Final lighting\0Albedo\0Shading normals\0Triangle IDs\0Depth bands\0World position grid\0"
              "Raw local lighting\0Raw indirect lighting\0Motion vectors\0Motion validity\0"
              "Indirect lighting confidence\0TAA current-frame weight\0"))
        options.debugView = uint32_t(view);
      static const char *help[] = {
          "Start with Triangle IDs and Shading normals at the problem angle, then compare Raw local "
          "lighting. Compare both Primary visibility options above.",
          "Unlit base color of the first visible surface. Flicker here points to visibility or material "
          "data.",
          "World-space shading normals: RGB = XYZ mapped to 0..1. Includes authored vertex normals and "
          "material normal maps; missing vertex normals fall back to flat faces.",
          "Colors identify submitted triangles. Coplanar surfaces swapping visibility change color. "
          "Remeshing can also change IDs; palette collisions are possible.",
          "Distance from the camera, repeating black to white every 4 world units. Sudden patches can reveal "
          "visibility or position jumps.",
          "World position as RGB, repeating every world unit. Look for moving discontinuities inside a flat "
          "face.",
          "Unfiltered direct local illumination on opaque receivers, including torches and emissives, before "
          "albedo and transparent tint. Uses the current display tonemapper.",
          "Unfiltered indirect illumination before albedo and transparent tint. Includes SHARC when enabled; "
          "zero bounces is black.",
          "Opaque motion guides: red/green = horizontal/vertical pixels, gray = zero, full range = +/-16 "
          "pixels. Magenta = invalid motion.",
          "Opaque motion correspondence: green = available, red = invalid/reset. This precedes per-pixel "
          "depth/normal history rejection.",
          "REBLUR indirect confidence: white = high trust, black = low. This is not the local-light "
          "denoiser's history.",
          "Actual native TAA blend weight: white = current frame only; darker = more history. Unavailable "
          "with FSR or TAA off."};
      ImGui::TextWrapped("%s", help[options.debugView]);
      if (options.debugView) {
        ImGui::TextWrapped(
            "Diagnostics display live internal pixels without temporal filtering. Lighting histories and "
            "requested settings keep running. FSR pixels are enlarged without filtering.");
        ImGui::TextWrapped("Purple checkerboard means the selected signal is unavailable. Normal subpixel "
                           "jitter remains visible at edges.");
      }
    }
    int shadowView = int(options.shadowDebug);
    if (ui::Combo("Shadow view", &shadowView, "Final lighting\0Raw visibility\0SIGMA visibility\0"))
      options.shadowDebug = uint32_t(shadowView);
    ImGui::BeginDisabled(rrActive);
    ui::Checkbox("NRD validation", &options.validation);
    ImGui::EndDisabled();
    ui::Checkbox("Progressive accumulation", &options.accumulate);
    if (options.accumulate)
      ImGui::TextWrapped("%s", "Raw reference: requested denoiser and TAA resume afterward.");
    ui::Checkbox("Force dynamic TLAS rebuild", &options.forceRebuild);
    ui::Checkbox("Freeze emissive radiance", &options.freezeEmissiveRadiance);
    if (ImGui::CollapsingHeader("Performance statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextWrapped("%s", m_properties.deviceName);
      ImGui::TextWrapped("Vulkan %u.%u.%u", VK_VERSION_MAJOR(m_properties.apiVersion),
                         VK_VERSION_MINOR(m_properties.apiVersion),
                         VK_VERSION_PATCH(m_properties.apiVersion));
      const auto &io = ImGui::GetIO();
      ImGui::TextWrapped("Presentation: %.2f ms / %.1f FPS", io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f,
                         io.Framerate);
      ImGui::TextWrapped("Scene: %zu surface draws", m_raster.pendingDrawCount());
      auto s = tracingStatistics();
      ImGui::TextWrapped("Accumulated frames: %llu", (unsigned long long)s.accumulationCount);
      ImGui::TextWrapped("BLAS builds / refits / compactions: %llu / %llu / %llu",
                         (unsigned long long)s.blasBuilds, (unsigned long long)s.blasRefits,
                         (unsigned long long)s.blasCompactions);
      ImGui::TextWrapped("Static TLAS builds: %llu", (unsigned long long)s.staticTlasBuilds);
      ImGui::TextWrapped("Dynamic TLAS builds / refits: %llu / %llu", (unsigned long long)s.dynamicTlasBuilds,
                         (unsigned long long)s.dynamicTlasRefits);
      ImGui::TextWrapped("Rebuild reason: %u; refits since build: %u", s.dynamicRebuildReason,
                         s.dynamicRefitCount);
      ImGui::TextWrapped("Acceleration memory: %.1f MiB", double(s.accelerationBytes) / (1024 * 1024));
      if (s.gpuTimingsAvailable) {
        ImGui::TextWrapped("GPU builds %.3f ms; tracing %.3f ms", s.gpuBuildMilliseconds,
                           s.gpuTraceMilliseconds);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Tracing includes sun and local shadows, indirect rays, and transparent "
                            "sheets.\nDenoiser timings are reported separately.");
      }
    }
    if (ImGui::CollapsingHeader("Present CPU breakdown")) {
      // Previous Present; this panel is drawn while the current one records.
      const auto &p = m_previousPerformance;
      const auto &b = m_previousBreakdown;
      ImGui::TextWrapped("Present %.2f ms: scene %.2f, fence %.2f, acquire %.2f, record %.2f, submit %.2f, "
                         "queue present %.2f",
                         p.presentMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds, p.acquireMilliseconds,
                         p.recordMilliseconds, p.submitMilliseconds, p.queuePresentMilliseconds);
      struct Row {
        const char *name;
        double value;
      };
      const Row rows[] = {{"Raster prepare", b.rasterPrepareMilliseconds},
                          {"  raster skinning", b.rasterSkinningMilliseconds},
                          {"Tracer begin", b.traceBeginMilliseconds},
                          {"Tracer setup", b.traceSetupMilliseconds},
                          {"Trace surfaces", b.traceSurfacesMilliseconds},
                          {"  geometry", p.geometryMilliseconds},
                          {"  materials", b.materialMilliseconds},
                          {"  temporal", b.temporalMilliseconds},
                          {"Retire", b.retireMilliseconds},
                          {"Acceleration", b.traceAccelerationMilliseconds},
                          {"Surface table", b.tableMilliseconds},
                          {"Lighting", b.lightingMilliseconds},
                          {"SHARC", b.sharcMilliseconds},
                          {"Scene signature", b.signatureMilliseconds},
                          {"Descriptors", b.descriptorMilliseconds},
                          {"Primary G-buffer", b.primaryMilliseconds},
                          {"Passes", b.passesMilliseconds},
                          {"Post-processing", b.postMilliseconds},
                          {"Composite", b.compositeMilliseconds},
                          {"Developer UI", b.uiMilliseconds},
                          {"Snapshot", b.snapshotMilliseconds},
                          {"Other", b.otherMilliseconds}};
      for (const auto &row : rows)
        ImGui::Text("%-18s %7.3f ms", row.name, row.value);
      ImGui::TextWrapped("Instances %llu, surfaces %llu, primary draws %llu, raster draws %llu, textures %llu",
                         (unsigned long long)b.traceInstances, (unsigned long long)b.traceSurfaces,
                         (unsigned long long)b.primaryDraws, (unsigned long long)b.rasterDraws,
                         (unsigned long long)b.textureDescriptors);
      ImGui::TextWrapped("Geometry conversions %llu, uploads %.2f MiB (raster %.2f MiB), raster skinned "
                         "vertices %llu",
                         (unsigned long long)p.geometryConversions, double(p.uploadBytes) / (1024 * 1024),
                         double(b.rasterUploadBytes) / (1024 * 1024),
                         (unsigned long long)b.rasterSkinnedVertices);
    }
    if (ImGui::CollapsingHeader("Input and clear color", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::ColorEdit3("Clear color", settings.clearColor.data());
      ui::InputText("Input", m_input, sizeof(m_input));
    }
  }
} // namespace remix
