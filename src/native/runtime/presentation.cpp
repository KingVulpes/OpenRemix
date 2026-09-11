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
  remixapi_ErrorCode RemixNativeRuntime::present(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    remixapi_WindowState state = {};
    auto status = m_window.getWindowState(state);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      return status;
    }
#if defined(_WIN32)
    if (info && info->hwndOverride &&
        info->hwndOverride != SDL_GetPointerProperty(SDL_GetWindowProperties(m_window.handle()),
                                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
#endif
    if (m_failed) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    struct EndFrame {
      RasterRenderer &raster;
      ~EndFrame() { raster.finishFrame(); }
    } endFrame{m_raster};
    if (!m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    if (state.minimized || !state.drawableWidth || !state.drawableHeight) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    try {
      if (m_recreateSurface || m_rebuild || !m_swapchain || m_vsync != m_presenterVsync ||
          state.drawableWidth != m_swapchainExtent.width ||
          state.drawableHeight != m_swapchainExtent.height) {
        status = recreateSwapchain();
        if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
          return fail();
        }
      }
      if (!m_swapchain) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      // Bounded waits keep unavailable surfaces and a stalled GPU from blocking the caller indefinitely.
      VkResult result = vkWaitForFences(m_device, 1, &m_frameFence, VK_TRUE, 1000000000);
      if (result == VK_TIMEOUT) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      if (m_backend != m_requestedBackend) {
        if (m_requestedBackend) {
          try {
            auto tracer = std::make_unique<PathTracer>();
            tracer->initialize(m_physicalDevice, m_device, m_queueFamily);
            tracer->configureTarget(m_renderPass, m_swapchainExtent,
                                    m_srgb && m_swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB &&
                                        m_swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB,
                                    m_raster.depthFormat(), m_raster.depthView(),
                                    m_swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB ||
                                        m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB);
            m_pathTracer = std::move(tracer);
            m_backend = 1;
            m_traceUnavailable.clear();
          } catch (VkResult failure) {
            if (failure == VK_ERROR_DEVICE_LOST)
              throw;
            m_traceUnavailable = "Path tracing resource initialization failed; raster remains active";
            SDL_Log("%s", m_traceUnavailable.c_str());
            m_requestedBackend = 0;
          } catch (...) {
            m_traceUnavailable = "Path tracing resource initialization failed; raster remains active";
            SDL_Log("%s", m_traceUnavailable.c_str());
            m_requestedBackend = 0;
          }
        } else {
          m_pathTracer.reset();
          m_backend = 0;
        }
      }
      uint32_t imageIndex = 0;
      completeScreenshot();
      result = vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000, m_imageAvailable, VK_NULL_HANDLE,
                                     &imageIndex);
      if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return fail(result);
      }
      m_rebuild |= result == VK_SUBOPTIMAL_KHR;
      m_frameStarted = true;
      status = recordFrame(imageIndex);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return fail();
      }
      result = vkResetFences(m_device, 1, &m_frameFence);
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &m_imageAvailable,
          .pWaitDstStageMask = &waitStage,
          .commandBufferCount = 1,
          .pCommandBuffers = &m_commandBuffers[imageIndex],
          .signalSemaphoreCount = 1,
          .pSignalSemaphores = &m_renderFinished[imageIndex],
      };
      result = vkQueueSubmit(m_queue, 1, &submit, m_frameFence);
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      if (m_pathTracer)
        m_pathTracer->submitted();
      const VkPresentInfoKHR present = {
          .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &m_renderFinished[imageIndex],
          .swapchainCount = 1,
          .pSwapchains = &m_swapchain,
          .pImageIndices = &imageIndex,
      };
      result = vkQueuePresentKHR(m_queue, &present);
      if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        ++m_presentationCount;
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
          result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      return result == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS : fail(result);
    } catch (VkResult result) {
      return fail(result);
    } catch (...) {
      return fail();
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::createFrameResources() {
    const VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_queueFamily,
    };
    if (vkCreateCommandPool(m_device, &pool, nullptr, &m_commandPool) != VK_SUCCESS) {
      return fail();
    }
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(m_device, &semaphore, nullptr, &m_imageAvailable) != VK_SUCCESS) {
      return fail();
    }
    const VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    return vkCreateFence(m_device, &fence, nullptr, &m_frameFence) == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS
                                                                                 : fail();
  }

  remixapi_ErrorCode RemixNativeRuntime::recordFrame(uint32_t imageIndex) {
    auto buffer = m_commandBuffers[imageIndex];
    VkResult result = vkResetCommandBuffer(buffer, 0);
    if (result != VK_SUCCESS) {
      return fail(result);
    }
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(buffer, &begin);
    if (result != VK_SUCCESS) {
      return fail(result);
    }
    if (m_pathTracer)
      m_pathTracer->beginFrame(m_traceOptions, m_raster);
    m_raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
    m_raster.prepare(buffer, m_backend != 0);
    if (m_pathTracer)
      m_pathTracer->prepare(buffer, m_raster, m_traceOptions);
    VkClearValue clear[2] = {};
    std::copy(std::begin(m_clearColor), std::end(m_clearColor), clear[0].color.float32);
    clear[1].depthStencil.depth = 1;
    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m_renderPass,
        .framebuffer = m_framebuffers[imageIndex],
        .renderArea = {{0, 0}, m_swapchainExtent},
        .clearValueCount = 2,
        .pClearValues = clear,
    };
    // Font uploads are recorded by the backend before the render pass.
    drawUi(VK_NULL_HANDLE);
    vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (m_pathTracer) {
      m_pathTracer->record(buffer);
      m_raster.record(buffer, false, true);
    } else
      m_raster.record(buffer);
    drawUi(buffer);
    vkCmdEndRenderPass(buffer);
    if (m_screenshotSupported) {
      m_screenshot.record(m_physicalDevice, m_device, buffer, m_swapchainImages[imageIndex],
                          m_swapchainExtent, m_swapchainFormat);
    }
    result = vkEndCommandBuffer(buffer);
    return result == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS : fail(result);
  }

  remixapi_ErrorCode RemixNativeRuntime::requestScreenshot(const char *path) {
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (m_failed)
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    return m_screenshot.request(path, m_screenshotSupported);
  }

  void RemixNativeRuntime::completeScreenshot() {
    if (m_initialized && !m_failed)
      m_screenshot.complete(m_device, m_frameFence);
  }
} // namespace remix
