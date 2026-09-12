/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "raster/renderer.h"
#include "pathtrace/renderer.h"
#include "platform/window.h"
#include "runtime/screenshot.h"
#include "platform/gpu_frames.h"
#include <remix/remix_c.h>
#include <vector>
#include <vulkan/vulkan.h>

struct ImGuiContext;

namespace remix {
  class RemixNativeRuntime {
  public:
    RemixNativeRuntime() = default;
    ~RemixNativeRuntime();
    RemixNativeRuntime(const RemixNativeRuntime &) = delete;
    RemixNativeRuntime &operator=(const RemixNativeRuntime &) = delete;

    remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
    remixapi_ErrorCode shutdown();
    remixapi_ErrorCode present(const remixapi_PresentInfo *info);
    remixapi_ErrorCode requestScreenshot(const char *path);
    void completeScreenshot();
    bool isInitialized() const { return m_initialized; }
    uint64_t presentationCount() const { return m_presentationCount; }
    bool hasStartedFrame() const { return m_frameStarted; }
    bool hasFailed() const { return m_failed; }
    RasterRenderer &raster() { return m_raster; }
    RemixSdlWindow &window() { return m_window; }
    remixapi_FeatureAvailability getFeatureAvailability() const;
    remixapi_DenoisingCapabilitiesEXT denoisingCapabilities() const { return m_denoiseCapabilities; }
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    remixapi_ErrorCode selectBackend(const char *value);
    remixapi_ErrorCode configureTracing(const char *key, const char *value);
    remixapi_BackendCapabilitiesEXT backendCapabilities() const;
    remixapi_TracingStatisticsEXT tracingStatistics() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    void setVsync(bool enabled) { m_vsync = enabled; }
    void setUiState(remixapi_UIState state) {
      m_uiState = state;
      if (state == REMIXAPI_UI_STATE_NONE) {
        m_window.setGuiCapture(false, false);
      }
    }

  private:
    struct QueueSelection {
      VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
      uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    };
    remixapi_ErrorCode createInstance();
    remixapi_ErrorCode selectPhysicalDevice(QueueSelection &selection) const;
    remixapi_ErrorCode createDevice(const QueueSelection &selection);
    remixapi_ErrorCode createSwapchain();
    remixapi_ErrorCode recreateSwapchain();
    remixapi_ErrorCode createFrameResources();
    remixapi_ErrorCode createAttachments();
    remixapi_ErrorCode recordFrame(uint32_t imageIndex, scene::TraceFrame *traceScene);
    remixapi_ErrorCode fail(VkResult result = VK_ERROR_UNKNOWN);
    void destroySwapchain();
    void destroyVulkan();
    bool supportsDeviceExtension(VkPhysicalDevice device, const char *name) const;
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> &modes) const;
    bool initializeUi();
    void shutdownUi();
    bool initializeUiVulkan();
    void shutdownUiVulkan();
    void drawUi(VkCommandBuffer commandBuffer);
    static bool processEvent(const SDL_Event *event, void *userData);

    RemixSdlWindow m_window;
    RasterRenderer m_raster;
    std::unique_ptr<PathTracer> m_pathTracer;
    TraceOptions m_traceOptions;
    uint32_t m_backend = 0, m_requestedBackend = 0, m_instanceApiVersion = VK_API_VERSION_1_1;
    bool m_traceSupported = false;
    remixapi_DenoisingCapabilitiesEXT m_denoiseCapabilities{.structSize =
                                                                sizeof(remixapi_DenoisingCapabilitiesEXT)};
    std::string m_traceUnavailable;
    PresentedScreenshot m_screenshot;
    bool m_screenshotSupported = false;
    bool m_initialized = false;
    uint64_t m_presentationCount = 0;
    bool m_frameStarted = false;
    bool m_failed = false;
    bool m_deviceLost = false;
    bool m_rebuild = false;
    bool m_recreateSurface = false;
    bool m_vsync = false;
    bool m_presenterVsync = false;
    bool m_srgb = true;
    remixapi_UIState m_uiState = REMIXAPI_UI_STATE_NONE;
    float m_clearColor[4] = {0.025f, 0.035f, 0.055f, 1.0f};
    char m_input[256] = {};
    ImGuiContext *m_gui = nullptr;
    VkPhysicalDeviceProperties m_properties = {};
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent = {};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    struct FrameSync {
      VkSemaphore imageAvailable = VK_NULL_HANDLE;
      VkFence fence = VK_NULL_HANDLE;
    } m_frames[gpuFrameCount];
    uint32_t m_frameIndex = 0, m_frameCount = gpuFrameCount;
    // Presentation may still consume a semaphore after the submission fence signals.
    std::vector<VkSemaphore> m_renderFinished;
  };
} // namespace remix
