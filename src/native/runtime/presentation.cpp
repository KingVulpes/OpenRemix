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

#include "platform/timing.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace remix {
  namespace {
    struct CompletePerformance {
      remixapi_FramePerformanceEXT &out;
      const std::unique_ptr<PathTracer> &tracer;
      const uint64_t &presentations;
      ~CompletePerformance() {
        out.presented = presentations != out.presentations;
        out.presentations = presentations;
        if (!tracer)
          return;
        const auto p = tracer->performance();
        out.geometryMilliseconds = p.geometryMilliseconds;
        out.allocationMilliseconds = p.allocationMilliseconds;
        out.uploadMilliseconds = p.uploadMilliseconds;
        out.accelerationMilliseconds = p.accelerationMilliseconds;
        out.geometryConversions = p.geometryConversions;
        out.geometryCacheHits = p.geometryCacheHits;
        out.uploadBytes = p.uploadBytes;
        out.bufferAllocations = p.bufferAllocations;
        out.allocationBytes = p.allocationBytes;
        out.bufferCacheHits = p.bufferCacheHits;
        out.memoryAllocations = p.memoryAllocations;
        out.memoryAllocationBytes = p.memoryAllocationBytes;
      }
    };
  } // namespace
  remixapi_ErrorCode RemixNativeRuntime::present(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    auto &p = m_framePerformance;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    ScopedMilliseconds presentTime{p.presentMilliseconds};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
    CompletePerformance completePerformance{p, m_pathTracer, m_presentationCount};
    remixapi_WindowState state = {};
    auto status = m_headless ? REMIXAPI_ERROR_CODE_SUCCESS : m_window.getWindowState(state);
    if (m_headless) {
      state.drawableWidth = m_swapchainExtent.width;
      state.drawableHeight = m_swapchainExtent.height;
    }
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
    if (!m_headless && !m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    applySettings();
    if (state.minimized || !state.drawableWidth || !state.drawableHeight) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    try {
      if (!m_headless && (m_recreateSurface || m_rebuild || !m_swapchain || m_vsync != m_presenterVsync ||
                          state.drawableWidth != m_swapchainExtent.width ||
                          state.drawableHeight != m_swapchainExtent.height)) {
        status = recreateSwapchain();
        if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
          return fail();
        }
      }
      if (!m_swapchain && !m_sharedImage) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      // Scene snapshots, emission reduction and staged light hierarchies use
      // no GPU resources. Prepare while submitted frames run; commit lighting,
      // upload and retire resources only after the reused slot's fence.
      std::optional<scene::TraceFrame> traceScene;
      if (m_pathTracer && m_targetBackend)
        traceScene = measured(p.sceneMilliseconds,
                              [&] { return m_pathTracer->prepareScene(m_raster, m_traceOptions); });
      // Bounded waits keep unavailable surfaces and a stalled GPU from blocking the caller indefinitely.
      VkResult result = measured(p.fenceMilliseconds, [&] {
        return vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 1000000000);
      });
      if (result == VK_TIMEOUT) {
        return m_headless ? fail(result) : REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      if (m_backend != m_targetBackend) {
        result = vkDeviceWaitIdle(m_device);
        if (result != VK_SUCCESS)
          return fail(result);
        if (m_targetBackend) {
          try {
            auto tracer = std::make_unique<PathTracer>();
            tracer->initialize(m_physicalDevice, m_device, m_queueFamily);
            tracer->setNgx(m_ngx);
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
            m_targetBackend = 0;
            m_backendFailed = true;
          } catch (...) {
            m_traceUnavailable = "Path tracing resource initialization failed; raster remains active";
            SDL_Log("%s", m_traceUnavailable.c_str());
            m_targetBackend = 0;
            m_backendFailed = true;
          }
        } else {
          m_pathTracer.reset();
          m_backend = 0;
        }
      }
      m_raster.selectFrame(m_frameIndex);
      if (m_pathTracer)
        m_pathTracer->selectFrame(m_frameIndex);
      uint32_t imageIndex = 0;
      // Preserve next-presentation capture completion. Event polling stays
      // nonblocking; only a pending screenshot needs the preceding frame.
      m_screenshot.complete(m_device, true);
      result = measured(p.acquireMilliseconds, [&] {
        if (m_headless)
          return VK_SUCCESS;
        return vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000, m_frames[m_frameIndex].imageAvailable,
                                     VK_NULL_HANDLE, &imageIndex);
      });
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
      status = measured(p.recordMilliseconds,
                        [&] { return recordFrame(imageIndex, traceScene ? &*traceScene : nullptr); });
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return fail();
      }
      result = vkResetFences(m_device, 1, &m_frames[m_frameIndex].fence);
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      const VkPipelineStageFlags waitStage =
          m_headless ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = m_headless ? uint32_t(m_sharedPresented) : 1u,
          .pWaitSemaphores = m_headless ? &m_sharedReleased : &m_frames[m_frameIndex].imageAvailable,
          .pWaitDstStageMask = &waitStage,
          .commandBufferCount = 1,
          .pCommandBuffers = &m_commandBuffers[m_frameIndex],
          .signalSemaphoreCount = 1,
          .pSignalSemaphores = m_headless ? &m_sharedReady : &m_renderFinished[imageIndex],
      };
      result = measured(p.submitMilliseconds,
                        [&] { return vkQueueSubmit(m_queue, 1, &submit, m_frames[m_frameIndex].fence); });
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      m_screenshot.submitted(m_frames[m_frameIndex].fence);
      if (m_pathTracer)
        m_pathTracer->submitted();
      if (m_headless) {
        // Do not acknowledge until ready is actually signaled. A worker crash
        // after the response cannot leave Godot waiting for an unsubmitted frame.
        result = vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 30000000000ull);
        if (result != VK_SUCCESS)
          return fail(result);
        m_sharedPresented = true;
        ++m_presentationCount;
        m_frameIndex = (m_frameIndex + 1) % m_frameCount;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      const VkPresentInfoKHR present = {
          .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &m_renderFinished[imageIndex],
          .swapchainCount = 1,
          .pSwapchains = &m_swapchain,
          .pImageIndices = &imageIndex,
      };
      result = measured(p.queuePresentMilliseconds, [&] { return vkQueuePresentKHR(m_queue, &present); });
      m_frameIndex = (m_frameIndex + 1) % m_frameCount;
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
    const VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (auto &frame : m_frames) {
      auto result = vkCreateSemaphore(m_device, &semaphore, nullptr, &frame.imageAvailable);
      if (result != VK_SUCCESS)
        return fail(result);
      result = vkCreateFence(m_device, &fence, nullptr, &frame.fence);
      if (result != VK_SUCCESS)
        return fail(result);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::recordFrame(uint32_t imageIndex, scene::TraceFrame *traceScene) {
    auto buffer = m_commandBuffers[m_frameIndex];
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
    if (m_headless)
      sharedBarrier(buffer, true);
    // GPU-only targets, histories and acceleration structures stay ordered on
    // the queue. The CPU may record ahead using its completed submission slot.
    const VkMemoryBarrier previousFrame{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &previousFrame, 0, nullptr, 0, nullptr);
    if (m_pathTracer)
      m_pathTracer->beginFrame(m_traceOptions, m_raster);
    m_raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
    m_raster.prepare(buffer, m_backend != 0);
    if (m_pathTracer) {
      // A newly enabled tracer did not exist during CPU preparation.
      std::optional<scene::TraceFrame> prepared;
      if (!traceScene) {
        prepared = measured(m_framePerformance.sceneMilliseconds,
                            [&] { return m_pathTracer->prepareScene(m_raster, m_traceOptions); });
        traceScene = &*prepared;
      }
      m_pathTracer->prepare(buffer, m_raster, m_traceOptions, *traceScene);
    }
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
    if (m_headless)
      sharedBarrier(buffer, false);
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
      m_screenshot.complete(m_device);
  }
} // namespace remix
