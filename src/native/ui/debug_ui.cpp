#include "runtime/runtime.h"
#include "ui/context.h"
#include "api/imgui_compat.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace remix {
  void RemixNativeRuntime::drawUi(VkCommandBuffer commandBuffer) {
    if (!m_gui)
      return;
    ui::UiScope scope(m_gui);
    // Asynchronous presentation: Present built this frame's UI on its thread
    // (commandBuffer null); the render thread records it only if it did.
    if (commandBuffer && m_asyncActive && !m_uiFrameBuilt)
      return;
    if (!commandBuffer)
      m_uiFrameBuilt = false;
    // The bootstrap UI pipeline is prepared by the same worker. Surface
    // presentation and SDL input remain available while its first job runs.
    if (!ui::prepareVulkanPipeline(commandBuffer == VK_NULL_HANDLE))
      return;
    if (commandBuffer) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
      return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // A host window usually hides the OS cursor on its thread (games do), so
    // draw ImGui's own while the menu is open.
    ImGui::GetIO().MouseDrawCursor = m_window.embedded() && m_uiState != REMIXAPI_UI_STATE_NONE;
    ImGui::NewFrame();
    if (preparationPending() || !m_preparationFailure.empty()) {
      const auto stats = m_pipelines->statistics();
      const auto &io = ImGui::GetIO();
      ImGui::SetNextWindowPos(
          m_loading ? ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f)
                    : ImVec2(16, 16),
          ImGuiCond_Always, m_loading ? ImVec2(.5f, .5f) : ImVec2(0, 0));
      ImGui::SetNextWindowBgAlpha(.94f);
      ImGui::Begin(
          "Renderer preparation", nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
      ImGui::TextUnformatted(m_loading ? "Preparing renderer"
                                       : "Preparing requested settings");
      ImGui::Text("Pipelines: %llu / %llu completed",
                  (unsigned long long)stats.completed,
                  (unsigned long long)stats.requested);
      if (m_preparation) {
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          m_preparation->started)
                .count();
        ImGui::Text("Elapsed: %.1f s", seconds);
      }
      if (!m_preparationFailure.empty())
        ImGui::TextWrapped("%s", m_preparationFailure.c_str());
      ImGui::TextUnformatted("F12: settings and diagnostics");
      ImGui::End();
    }
    if (m_uiState != REMIXAPI_UI_STATE_NONE) {
      const bool open =
          ui::drawPanel(m_profiles, m_profileUi, this, [](void *user, int tab, config::Settings &settings) {
            auto &runtime = *static_cast<RemixNativeRuntime *>(user);
            switch (tab) {
            case 0:
              runtime.drawRenderingTab(settings);
              break;
            case 1:
              runtime.drawLightingTab(settings);
              break;
            case 2:
              runtime.drawPostProcessingTab(settings);
              break;
            case 3:
              runtime.drawDiagnosticsTab(settings);
              break;
            }
          });
      if (!open)
        setUiState(REMIXAPI_UI_STATE_NONE);
    }
    remixapi_imgui_InvokeOverlayCallback();
    ImGui::Render();
    const auto &io = ImGui::GetIO();
    m_window.setGuiCapture(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureKeyboard,
                           m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureMouse);
    m_window.syncTextInput(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantTextInput);
    for (auto *texture : ImGui::GetPlatformIO().Textures) {
      if (texture->Status != ImTextureStatus_OK)
        ImGui_ImplVulkan_UpdateTexture(texture);
    }
    m_uiFrameBuilt = true;
  }
} // namespace remix
