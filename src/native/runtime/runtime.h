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

#include "pathtrace/renderer.h"
#include "platform/gpu_frames.h"
#include "platform/window.h"
#include "raster/renderer.h"
#include "runtime/pipelines.h"
#include "runtime/profiles.h"
#include "runtime/screenshot.h"
#include "ui/profiles.h"
#include <remix/remix_c.h>
#include <remix/remix_editor_ext.h>
#include <vector>
#include <vulkan/vulkan.h>

struct ImGuiContext;

namespace remix {
class FrameSnapshot;
class RemixNativeRuntime {
public:
  RemixNativeRuntime() = default;
  ~RemixNativeRuntime();
  RemixNativeRuntime(const RemixNativeRuntime &) = delete;
  RemixNativeRuntime &operator=(const RemixNativeRuntime &) = delete;

  remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
  remixapi_ErrorCode shutdown();
  upscale::Statistics reconstructionStatistics() const;
  postfx::Statistics postProcessingStatistics() const {
    return m_postProcessing ? m_postProcessing->statistics() : postfx::Statistics{};
  }
  const std::string &postProcessingFailure() const { return m_postFailure; }
  remixapi_ErrorCode present(const remixapi_PresentInfo *info);
  remixapi_ErrorCode requestScreenshot(const char *path);
  void completeScreenshot();
  int configureSharedFrame(remixapi_EditorSharedFrame &frame);
  bool modsDisabled() const { return m_disableMods; }
  bool isHeadless() const { return m_headless; }
  bool isInitialized() const { return m_initialized; }
  uint64_t presentationCount() const { return m_presentationCount; }
  remixapi_FramePerformanceEXT framePerformance() const {
    return m_framePerformance;
  }
  struct PreparationStatus {
    bool pending, loading;
    pipeline::Statistics pipelines;
    std::string failure;
  };
  PreparationStatus preparationStatus() const {
    return {preparationPending(), m_loading,
            m_pipelines ? m_pipelines->statistics() : pipeline::Statistics{},
            m_preparationFailure};
  }
  bool hasStartedFrame() const { return m_frameStarted; }
  bool hasFailed() const { return m_failed; }
  RasterRenderer &raster() { return m_raster; }
  RemixSdlWindow &window() { return m_window; }
  remixapi_FeatureAvailability getFeatureAvailability() const;
  remixapi_DenoisingCapabilitiesEXT denoisingCapabilities() const {
    return m_denoiseCapabilities;
  }
  remixapi_ShadowStatisticsEXT shadowStatistics() const;
  remixapi_LocalLightStatisticsEXT localLightStatistics() const;
  remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
  remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
  remixapi_ErrorCode selectBackend(const char *value);
  remixapi_ErrorCode configureTracing(const char *key, const char *value);
  remixapi_BackendCapabilitiesEXT backendCapabilities() const;
  remixapi_TracingStatisticsEXT tracingStatistics() const;
  remixapi_SharcStatisticsEXT sharcStatistics() const;
  remixapi_ErrorCode configureSetting(const char *key, const char *value);
  // GetVramStats, gated by the "Report VRAM statistics" setting.
  remixapi_ErrorCode vramStatistics(remixapi_VramStats &stats) const;
  remixapi_UIState uiState() const { return m_uiState; }
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
  remixapi_ErrorCode recordFrame(uint32_t imageIndex,
                                 scene::TraceFrame *traceScene);
  remixapi_ErrorCode fail(VkResult result = VK_ERROR_UNKNOWN);
  void destroySharedFrame();
  void sharedBarrier(VkCommandBuffer command, bool acquire);
  void destroySwapchain();
  void destroyVulkan();
  bool supportsDeviceExtension(VkPhysicalDevice device, const char *name) const;
  VkSurfaceFormatKHR
  chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
  VkPresentModeKHR
  choosePresentMode(const std::vector<VkPresentModeKHR> &modes) const;
  bool initializeUi();
  void shutdownUi();
  bool initializeUiVulkan();
  void shutdownUiVulkan();
  void drawUi(VkCommandBuffer commandBuffer);
  void drawRenderingTab(config::Settings &settings);
  void drawLightingTab(config::Settings &settings);
  void drawPostProcessingTab(config::Settings &settings);
  void drawDiagnosticsTab(config::Settings &settings);
  void applySettings();
  void prepareSettings();
  void preparePostProcessing();
  void presented();
  void retireTracer(std::unique_ptr<PathTracer> tracer,
                    std::shared_ptr<void> depth);
  bool preparationPending() const;
  static bool processEvent(const SDL_Event *event, void *userData);

  RemixSdlWindow m_window;
  RasterRenderer m_raster;
  std::shared_ptr<upscale::Ngx> m_ngx;
  std::unique_ptr<PathTracer> m_pathTracer;
  std::shared_ptr<postfx::State> m_exposureState;
  std::unique_ptr<postfx::Renderer> m_postProcessing;
  struct PreparedPost {
    pipeline::PreparationQueue::Ticket job;
    std::unique_ptr<postfx::Renderer> renderer;
    std::string key;
  };
  std::shared_ptr<PreparedPost> m_postPreparation;
  std::vector<std::shared_ptr<PreparedPost>> m_obsoletePost;
  std::string m_postFailure, m_failedPostKey;
  TraceOptions m_postOptions;
  uint64_t m_postGeneration = 0;
  std::unique_ptr<pipeline::Service> m_pipelines;
  struct PreparedConfiguration {
    pipeline::PreparationQueue::Ticket job;
    std::unique_ptr<PathTracer> tracer;
    std::shared_ptr<void> depth;
    std::string key;
    uint64_t generation{}, revision{};
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
  };
  std::shared_ptr<PreparedConfiguration> m_preparation;
  // Obsolete jobs may still be inside an uncancellable driver call. They
  // cannot activate, and must keep their dependencies until completion.
  std::vector<std::shared_ptr<PreparedConfiguration>> m_obsoletePreparations;
  std::string m_activePreparationKey, m_failedPreparationKey,
      m_preparationFailure;
  pipeline::LatestConfiguration m_configurationRevision;
  uint64_t m_preparationGeneration = 0;
  bool m_loading = false, m_rasterPreparing = false;
  uint32_t m_reconstructionFailureMask = 0;
  std::string m_reconstructionFailureKey, m_reconstructionFailure;
  bool m_localSourcesSeen = false;
  std::shared_ptr<std::atomic<unsigned>> m_ngxPreparing =
      std::make_shared<std::atomic<unsigned>>(0);
  TraceOptions m_traceOptions;
  config::Profiles m_profiles;
  config::Settings m_appliedSettings;
  ui::ProfileUi m_profileUi;
  uint64_t m_sharcReset = 0;
  uint32_t m_targetBackend = 0;
  bool m_backendFailed = false;
  uint32_t m_backend = 0, m_requestedBackend = 0,
           m_instanceApiVersion = VK_API_VERSION_1_1;
  bool m_traceSupported = false;
  bool m_textureCompression = false; // textureCompressionBC enabled on m_device.
  bool m_memoryBudget = false;       // VK_EXT_memory_budget queries available.
  remixapi_DenoisingCapabilitiesEXT m_denoiseCapabilities{
      .structSize = sizeof(remixapi_DenoisingCapabilitiesEXT)};
  std::string m_traceUnavailable;
  PresentedScreenshot m_screenshot;
  bool m_screenshotSupported = false;
  std::shared_ptr<FrameSnapshot> m_lastFrame;
  bool m_snapshotSupported = false, m_snapshotAllocationFailed = false;
  bool m_initialized = false;
  uint64_t m_presentationCount = 0;
  std::chrono::steady_clock::time_point m_lastPresentation{};
  double m_longestPresentationGap = 0;
  remixapi_FramePerformanceEXT m_framePerformance{
      .structSize = sizeof(remixapi_FramePerformanceEXT)};
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
  bool m_headless = false, m_disableProfiles = false, m_disableMods = false;
  VkImage m_sharedImage = VK_NULL_HANDLE;
  VkDeviceMemory m_sharedMemory = VK_NULL_HANDLE;
  VkSemaphore m_sharedReady = VK_NULL_HANDLE, m_sharedReleased = VK_NULL_HANDLE;
  uint64_t m_sharedSize = 0;
  uint32_t m_sharedMemoryType = 0;
  bool m_sharedPresented = false;
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D m_swapchainExtent = {};
  std::vector<VkImage> m_swapchainImages;
  std::vector<VkImageView> m_imageViews;
  std::vector<VkFramebuffer> m_framebuffers;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkRenderPass m_overlayPass = VK_NULL_HANDLE;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> m_commandBuffers;
  struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::shared_ptr<FrameSnapshot> snapshot;
  } m_frames[gpuFrameCount];
  uint32_t m_frameIndex = 0, m_frameCount = gpuFrameCount;
  // Presentation may still consume a semaphore after the submission fence
  // signals.
  std::vector<VkSemaphore> m_renderFinished;
};
} // namespace remix
