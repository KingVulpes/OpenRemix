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

#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <charconv>

namespace remix {
  RemixNativeRuntime::~RemixNativeRuntime() {
    if (m_device && !m_deviceLost) {
      vkDeviceWaitIdle(m_device);
    }
    destroyVulkan();
    m_window.shutdown();
  }

  remixapi_ErrorCode RemixNativeRuntime::initialize(const remixapi_StartupInfo &info) {
    if (isInitialized()) {
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }
#if defined(_WIN32)
    if (info.forceNoVkSwapchain) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
#endif
    m_failed = false;
    m_deviceLost = false;
    m_presentationCount = 0;
    m_srgb = !info.disableSrgbConversionForOutput;
    try {
      auto status = m_window.initialize(info);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
      auto finish = [&](remixapi_ErrorCode error) {
        if (error != REMIXAPI_ERROR_CODE_SUCCESS) {
          shutdown();
        }
        return error;
      };
      status = createInstance();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      status = m_window.createVulkanSurface(m_instance, nullptr, m_surface);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      QueueSelection selection;
      status = selectPhysicalDevice(selection);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      status = createDevice(selection);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      m_raster.initializeGpu(m_physicalDevice, m_device);
      status = createFrameResources();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      if (!initializeUi()) {
        return finish(REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
      }
      status = recreateSwapchain();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      m_initialized = true;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (...) {
      shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::shutdown() {
    if (m_window.handle()) {
      remixapi_WindowState state = {};
      auto status = m_window.getWindowState(state);
      if (status == REMIXAPI_ERROR_CODE_WRONG_THREAD) {
        return status;
      }
    }
    if (m_device && !m_deviceLost) {
      vkDeviceWaitIdle(m_device);
    }
    destroyVulkan();
    m_window.shutdown();
    m_initialized = false;
    m_frameStarted = false;
    m_failed = false;
    m_deviceLost = false;
    m_recreateSurface = false;
    m_rebuild = false;
    m_vsync = false;
    m_backend = m_requestedBackend = 0;
    m_traceOptions = {};
    m_traceSupported = false;
    m_denoiseCapabilities = {.structSize = sizeof(remixapi_DenoisingCapabilitiesEXT)};
    m_uiState = REMIXAPI_UI_STATE_NONE;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::fail(VkResult result) {
    m_failed = true;
    m_deviceLost |= result == VK_ERROR_DEVICE_LOST;
    SDL_Log("Remix Vulkan failure: %d", static_cast<int>(result));
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  void RemixNativeRuntime::destroyVulkan() {
    destroySwapchain();
    m_screenshot.reset(m_device);
    shutdownUi();
    m_pathTracer.reset();
    m_raster.shutdownGpu();
    if (m_device) {
      if (m_frameFence) {
        vkDestroyFence(m_device, m_frameFence, nullptr);
      }
      if (m_imageAvailable) {
        vkDestroySemaphore(m_device, m_imageAvailable, nullptr);
      }
      if (m_commandPool) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
      }
      vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface) {
      vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }
    if (m_instance) {
      vkDestroyInstance(m_instance, nullptr);
    }
    m_frameFence = VK_NULL_HANDLE;
    m_imageAvailable = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_surface = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
  }
} // namespace remix
