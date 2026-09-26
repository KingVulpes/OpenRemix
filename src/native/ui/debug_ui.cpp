#include "runtime/runtime.h"
#include "ui/context.h"
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
    if (commandBuffer) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
      return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
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
    ImGui::Render();
    const auto &io = ImGui::GetIO();
    m_window.setGuiCapture(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureKeyboard,
                           m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureMouse);
    m_window.syncTextInput(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantTextInput);
    for (auto *texture : ImGui::GetPlatformIO().Textures) {
      if (texture->Status != ImTextureStatus_OK)
        ImGui_ImplVulkan_UpdateTexture(texture);
    }
  }
} // namespace remix
