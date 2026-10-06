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
#include "runtime/frame_snapshot.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace remix {
void RemixNativeRuntime::presented() {
  const auto now = std::chrono::steady_clock::now();
  if (m_lastPresentation != std::chrono::steady_clock::time_point{})
    m_longestPresentationGap = std::max(
        m_longestPresentationGap,
        std::chrono::duration<double, std::milli>(now - m_lastPresentation)
            .count());
  m_lastPresentation = now;
  ++m_presentationCount;
}
  namespace {
    struct CompletePerformance {
      remixapi_FramePerformanceEXT &out;
      remixapi_FrameBreakdownEXT &breakdown;
      PathTracer::StaticStatistics &statics;
      const std::unique_ptr<PathTracer> &tracer;
      const RasterRenderer &raster;
      const uint64_t &presentations;
      ~CompletePerformance() {
        out.presented = presentations != out.presentations;
        out.presentations = presentations;
        // Runtime-owned phases were accumulated in breakdown during recording.
        auto frame = breakdown;
        if (tracer) {
          frame = tracer->breakdown();
          frame.traceBeginMilliseconds = breakdown.traceBeginMilliseconds;
          frame.compositeMilliseconds = breakdown.compositeMilliseconds;
          frame.uiMilliseconds = breakdown.uiMilliseconds;
          frame.snapshotMilliseconds = breakdown.snapshotMilliseconds;
          frame.presentSetupMilliseconds = breakdown.presentSetupMilliseconds;
          frame.presentReleaseMilliseconds = breakdown.presentReleaseMilliseconds;
        }
        const auto &r = raster.profile();
        frame.rasterPrepareMilliseconds = breakdown.rasterPrepareMilliseconds ? r.prepareMilliseconds : 0;
        frame.rasterSkinningMilliseconds = breakdown.rasterPrepareMilliseconds ? r.skinningMilliseconds : 0;
        frame.rasterDraws = r.draws;
        frame.rasterSkinnedVertices = r.skinnedVertices;
        frame.rasterUploadBytes = r.uploadBytes;
        const double covered =
            frame.rasterPrepareMilliseconds + frame.traceBeginMilliseconds + frame.traceSetupMilliseconds +
            frame.traceSurfacesMilliseconds + frame.retireMilliseconds + frame.traceAccelerationMilliseconds +
            frame.tableMilliseconds + frame.lightingMilliseconds + frame.sharcMilliseconds +
            frame.signatureMilliseconds + frame.descriptorMilliseconds + frame.primaryMilliseconds +
            frame.passesMilliseconds + frame.postMilliseconds + frame.compositeMilliseconds +
            frame.uiMilliseconds + frame.snapshotMilliseconds;
        frame.otherMilliseconds = std::max(0.0, out.recordMilliseconds - covered);
        breakdown = frame;
        statics = tracer ? tracer->staticStatistics() : PathTracer::StaticStatistics{};
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
    // "rtx.native.asyncPresent". The headless editor acknowledges each frame
    // only once it is complete, so it always presents synchronously.
    const bool async = m_profiles.current().asyncPresent && !m_headless;
    if (!async && !m_asyncActive)
      return presentFrame(info);
    // At most one frame in flight on the CPU side: the render thread finishes
    // the previous one before this Present touches anything it owns.
    const auto start = std::chrono::steady_clock::now();
    const double waited = waitRenderThread();
    if (async != m_asyncActive) {
      try {
        switchPresentation(async);
      } catch (...) {
        return fail();
      }
    }
    if (!async)
      return presentFrame(info);
    return presentAsync(info, start, waited);
  }

  double RemixNativeRuntime::waitRenderThread() {
    if (!m_renderThread.started())
      return 0;
    const double waited = m_renderThread.wait();
    // renderFrame handles its own failures; anything escaping is a failure too.
    if (m_renderThread.takeError())
      fail();
    return waited;
  }

  void RemixNativeRuntime::switchPresentation(bool async) {
    // The render thread is idle. The GPU state (caches, pipelines, target)
    // follows the frame being rendered; the static set starts a new epoch, so
    // the tracer's copy is rebuilt from whichever table it now syncs from.
    if (async) {
      if (!m_frameScene)
        m_frameScene = std::make_unique<RasterRenderer>();
      m_frameScene->reset();
      m_frameScene->adoptGpu(m_raster);
    } else {
      m_raster.adoptGpu(*m_frameScene);
      m_frameScene->reset(); // Releases the last published frame and the replica.
    }
    m_raster.restartStatic();
    m_raster.setConcurrentConsumer(async);
    m_asyncActive = async;
    m_asyncCaller.active = async;
    SDL_Log("OpenRemix presentation: %s", async ? "asynchronous (render thread)" : "synchronous");
  }

  remixapi_ErrorCode RemixNativeRuntime::presentAsync(const remixapi_PresentInfo *info,
                                                      std::chrono::steady_clock::time_point start,
                                                      double waited) {
    // The render thread's last frame is complete: Diagnostics shows it, and
    // the getters return it while the next one renders.
    m_previousPerformance = m_framePerformance;
    m_previousBreakdown = m_frameBreakdown;
    m_previousStatic = m_staticStatistics;
    m_previousAsyncCaller = m_asyncCaller;
    m_asyncCaller = {.active = true, .waitMilliseconds = waited, .handedOff = m_asyncCaller.handedOff};
    struct CallTime {
      double &total;
      std::chrono::steady_clock::time_point start;
      ~CallTime() {
        total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      }
    } callTime{m_asyncCaller.callMilliseconds, start};
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
    auto &frame = *m_frameScene;
    // A frame that is not handed over ends here, as a synchronous Present
    // would end it: the API's draw lists, and the published copy if any.
    struct Unsent {
      NativeResources &api, &frame;
      bool published = false, handedOff = false;
      ~Unsent() {
        if (handedOff)
          return;
        api.finishFrame();
        if (published)
          frame.finishFrame();
      }
    } unsent{m_raster, frame};
    Stopwatch setupTime{m_asyncCaller.setupMilliseconds};
    if (!m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    applySettings();
    updateMemoryPressure();
    if (state.minimized || !state.drawableWidth || !state.drawableHeight) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    try {
      if (m_recreateSurface || m_rebuild || !m_swapchain || m_vsync != m_presenterVsync ||
          state.drawableWidth != m_swapchainExtent.width || state.drawableHeight != m_swapchainExtent.height) {
        status = recreateSwapchain();
        if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
          return fail();
        }
      }
      if (!m_swapchain) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      updateFreeCamera();
      m_raster.applyFreeCamera(m_freeCamera);
      // The frame snapshot: from here on API calls change the next frame.
      setupTime.stop();
      measured(m_asyncCaller.captureMilliseconds, [&] { m_raster.publishFrame(frame); });
      unsent.published = true;
      setupTime.start();
      // As the synchronous path, reading the published frame.
      prepareSettings();
      {
        const auto &settings = m_profiles.current();
        const bool traceStatic = m_pathTracer && m_targetBackend && m_backend != 0 && !m_loading;
        if (!traceStatic)
          frame.expandStatic();
        // From the next replay on; the tracer reads the setting this frame.
        m_raster.setPersistentStatic(settings.persistentStatic && traceStatic);
        m_raster.publishStaticSetting(frame);
        if (m_pathTracer)
          m_pathTracer->setStaticValidation(settings.persistentStatic && settings.persistentStaticValidation);
      }
      frame.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
      m_rasterPreparing = !frame.preparePipelines(m_backend != 0);
      m_loading |= m_rasterPreparing;
      if (m_loading && m_postProcessing) m_postProcessing->freeze();
      // The developer UI reads and edits settings and statistics, and SDL
      // input capture belongs to this thread: build its frame here. The
      // render thread only records the finished draw data (and ImGui's
      // current context is per thread).
      measured(m_asyncCaller.uiMilliseconds, [&] { drawUi(VK_NULL_HANDLE); });
      publishState();
      setupTime.stop();
      m_handoff = {.callerMilliseconds = m_asyncCaller.setupMilliseconds + m_asyncCaller.captureMilliseconds,
                   .memoryStatistics = m_profiles.current().vramStatistics};
      m_renderThread.submit([this] { renderFrame(); });
      // From here the render thread owns the published frame.
      unsent.handedOff = true;
      ++m_asyncCaller.handedOff;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (VkResult result) {
      return fail(result);
    } catch (...) {
      return fail();
    }
  }

  // The render thread's part of an asynchronous Present: what the synchronous
  // path does after its setup, from the published frame. Touches only
  // render-owned state (runtime.h); failures are recorded (m_failed) for the
  // next Present, since this frame's Present has already returned.
  void RemixNativeRuntime::renderFrame() {
    m_asyncRender.renderMilliseconds = 0;
    ScopedMilliseconds renderTime{m_asyncRender.renderMilliseconds};
    auto &frame = *m_frameScene;
    // GetVramStats' raster figures walk every cached texture and mesh: taken
    // here, off the caller's thread, for the next handoff to publish.
    struct Memory {
      const RasterRenderer &frame;
      RasterRenderer::MemoryStatistics &out;
      bool enabled;
      ~Memory() {
        if (enabled)
          out = frame.memoryStatistics();
      }
    } memory{frame, m_renderedMemory, m_handoff.memoryStatistics};
    auto &p = m_framePerformance;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    m_frameBreakdown = {.structSize = sizeof(m_frameBreakdown)};
    // The caller's setup and capture are this frame's Present setup.
    p.presentMilliseconds = m_handoff.callerMilliseconds;
    m_frameBreakdown.presentSetupMilliseconds = m_handoff.callerMilliseconds;
    // Destroyed after presentTime, so recordMilliseconds is final.
    CompletePerformance completePerformance{p,        m_frameBreakdown, m_staticStatistics,
                                            m_pathTracer, frame,          m_presentationCount};
    ScopedMilliseconds presentTime{p.presentMilliseconds};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
    struct EndFrame {
      RasterRenderer &raster;
      double &release;
      ~EndFrame() { measured(release, [&] { raster.finishFrame(); }); }
    } endFrame{frame, m_frameBreakdown.presentReleaseMilliseconds};
    Stopwatch setupTime{m_frameBreakdown.presentSetupMilliseconds};
    try {
      using Clock = std::chrono::steady_clock;
      struct ReleaseEnd {
        Clock::time_point &start;
        double &release;
        ~ReleaseEnd() {
          if (start != Clock::time_point{})
            release += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
      };
      Clock::time_point releaseStart{};
      ReleaseEnd releaseEnd{releaseStart, m_frameBreakdown.presentReleaseMilliseconds};
      std::optional<scene::TraceFrame> traceScene;
      struct ReleaseStart {
        Clock::time_point &start;
        ~ReleaseStart() { start = Clock::now(); }
      } releaseStartStamp{releaseStart};
      setupTime.stop();
      if (m_pathTracer && m_targetBackend && !m_loading)
        traceScene = measured(p.sceneMilliseconds, [&] { return m_pathTracer->prepareScene(frame, m_traceOptions); });
      VkResult result = measured(p.fenceMilliseconds, [&] {
        return vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 1000000000);
      });
      if (result == VK_TIMEOUT)
        return;
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      setupTime.start();
      m_frames[m_frameIndex].snapshot.reset();
      frame.selectFrame(m_frameIndex);
      if (m_postProcessing) m_postProcessing->selectFrame(m_frameIndex);
      if (m_pathTracer)
        m_pathTracer->selectFrame(m_frameIndex);
      uint32_t imageIndex = 0;
      m_screenshot.complete(m_device, true);
      setupTime.stop();
      result = measured(p.acquireMilliseconds, [&] {
        return vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000, m_frames[m_frameIndex].imageAvailable,
                                     VK_NULL_HANDLE, &imageIndex);
      });
      if (result == VK_TIMEOUT || result == VK_NOT_READY)
        return;
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        // The next Present recreates the swapchain (render thread idle).
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return;
      }
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fail(result);
        return;
      }
      m_rebuild |= result == VK_SUBOPTIMAL_KHR;
      m_frameStarted = true;
      const auto status = measured(p.recordMilliseconds,
                                   [&] { return recordFrame(imageIndex, traceScene ? &*traceScene : nullptr); });
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        fail();
        return;
      }
      result = vkResetFences(m_device, 1, &m_frames[m_frameIndex].fence);
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      const VkPipelineStageFlags waitStage =
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &m_frames[m_frameIndex].imageAvailable,
          .pWaitDstStageMask = &waitStage,
          .commandBufferCount = 1,
          .pCommandBuffers = &m_commandBuffers[m_frameIndex],
          .signalSemaphoreCount = 1,
          .pSignalSemaphores = &m_renderFinished[imageIndex],
      };
      result = measured(p.submitMilliseconds,
                        [&] { return vkQueueSubmit(m_queue, 1, &submit, m_frames[m_frameIndex].fence); });
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      m_screenshot.submitted(m_frames[m_frameIndex].fence);
      if (m_pathTracer && !m_loading)
        measured(m_frameBreakdown.presentReleaseMilliseconds, [&] { m_pathTracer->submitted(); });
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
      if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        presented();
        ++m_asyncRender.rendered;
      }
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
          result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return;
      }
      if (result != VK_SUCCESS)
        fail(result);
    } catch (VkResult result) {
      fail(result);
    } catch (...) {
      fail();
    }
  }

  // Synchronous presentation (asynchronous presentation off, render thread
  // idle): m_raster is both the API's scene and the renderer.
  remixapi_ErrorCode RemixNativeRuntime::presentFrame(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    auto &p = m_framePerformance;
    m_previousPerformance = p;
    m_previousBreakdown = m_frameBreakdown;
    m_previousStatic = m_staticStatistics;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    m_frameBreakdown = {.structSize = sizeof(m_frameBreakdown)};
    // Destroyed after presentTime, so recordMilliseconds is final.
    CompletePerformance completePerformance{p,        m_frameBreakdown, m_staticStatistics,
                                            m_pathTracer, m_raster,       m_presentationCount};
    ScopedMilliseconds presentTime{p.presentMilliseconds};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
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
      double &release;
      ~EndFrame() { measured(release, [&] { raster.finishFrame(); }); }
    } endFrame{m_raster, m_frameBreakdown.presentReleaseMilliseconds};
    Stopwatch setupTime{m_frameBreakdown.presentSetupMilliseconds};
    if (!m_headless && !m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    applySettings();
    updateMemoryPressure();
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
      updateFreeCamera();
      m_raster.applyFreeCamera(m_freeCamera);
      prepareSettings();
      {
        // A persistent static set is traced from the tracer's own copy of it.
        // Any other consumer of this frame (raster rendering, a frame without
        // the tracer) takes the set into the draw list, as without it.
        const auto &settings = m_profiles.current();
        const bool traceStatic = m_pathTracer && m_targetBackend && m_backend != 0 && !m_loading;
        if (!traceStatic)
          m_raster.expandStatic();
        // From the next replay on.
        m_raster.setPersistentStatic(settings.persistentStatic && traceStatic);
        if (m_pathTracer)
          m_pathTracer->setStaticValidation(settings.persistentStatic && settings.persistentStaticValidation);
      }
      m_raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass()
                                         : VK_NULL_HANDLE);
      m_rasterPreparing = !m_raster.preparePipelines(m_backend != 0);
      m_loading |= m_rasterPreparing;
      if (m_loading && m_postProcessing) m_postProcessing->freeze();
      // Scene snapshots, emission reduction and staged light hierarchies use
      // no GPU resources. Prepare while submitted frames run; commit lighting,
      // upload and retire resources only after the reused slot's fence.
      // The trace frame's release is timed from ReleaseStart's destruction
      // (declared after it) to ReleaseEnd's (declared before it).
      using Clock = std::chrono::steady_clock;
      struct ReleaseEnd {
        Clock::time_point &start;
        double &release;
        ~ReleaseEnd() {
          if (start != Clock::time_point{})
            release += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
      };
      Clock::time_point releaseStart{};
      ReleaseEnd releaseEnd{releaseStart, m_frameBreakdown.presentReleaseMilliseconds};
      std::optional<scene::TraceFrame> traceScene;
      struct ReleaseStart {
        Clock::time_point &start;
        ~ReleaseStart() { start = Clock::now(); }
      } releaseStartStamp{releaseStart};
      setupTime.stop();
      if (m_pathTracer && m_targetBackend && !m_loading)
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
      setupTime.start();
      m_frames[m_frameIndex].snapshot.reset();
      m_raster.selectFrame(m_frameIndex);
      if (m_postProcessing) m_postProcessing->selectFrame(m_frameIndex);
      if (m_pathTracer)
        m_pathTracer->selectFrame(m_frameIndex);
      uint32_t imageIndex = 0;
      // Preserve next-presentation capture completion. Event polling stays
      // nonblocking; only a pending screenshot needs the preceding frame.
      m_screenshot.complete(m_device, true);
      setupTime.stop();
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
          m_headless ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                     : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
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
      if (m_pathTracer && !m_loading)
        measured(m_frameBreakdown.presentReleaseMilliseconds, [&] { m_pathTracer->submitted(); });
      if (m_headless) {
        // Do not acknowledge until ready is actually signaled. A worker crash
        // after the response cannot leave Godot waiting for an unsubmitted frame.
        result = vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 30000000000ull);
        if (result != VK_SUCCESS)
          return fail(result);
        m_sharedPresented = true;
        presented();
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
        presented();
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
    // GPU pass timers: this slot's previous frame has completed (its fence);
    // its timestamps are read without waiting before the slot is reused.
    m_gpuTimers.beginFrame(buffer, m_frameIndex, m_gpuTimersEnabled, ++m_recordedFrames,
                           m_pathTracer && !m_loading ? m_pathTracer->renderExtent() : m_swapchainExtent,
                           m_swapchainExtent);
    struct AbandonTimers {
      gpu::GpuTimers &timers;
      ~AbandonTimers() { timers.abandon(); } // no-op after endFrame
    } abandonTimers{m_gpuTimers};
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
    auto &breakdown = m_frameBreakdown;
    // The API's scene, or the published frame (asynchronous presentation).
    auto &raster = renderer();
    if (m_pathTracer && !m_loading)
      measured(breakdown.traceBeginMilliseconds, [&] { m_pathTracer->beginFrame(m_traceOptions, raster); });
    raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
    if (!m_loading) {
      gpu::GpuScope scope(buffer, "Raster uploads and skinning");
      raster.prepare(buffer, m_backend != 0);
      breakdown.rasterPrepareMilliseconds = 1; // Replaced by the raster profile.
    }
    if (m_pathTracer && !m_loading) {
      // A newly enabled tracer did not exist during CPU preparation.
      std::optional<scene::TraceFrame> prepared;
      if (!traceScene) {
        prepared = measured(m_framePerformance.sceneMilliseconds,
                            [&] { return m_pathTracer->prepareScene(raster, m_traceOptions); });
        traceScene = &*prepared;
      }
      // The tracer's passes nest in this range; its own time is what they
      // leave out (barriers, small uploads and readbacks between passes).
      gpu::GpuScope scope(buffer, "Path tracer: barriers, copies, other");
      m_pathTracer->prepare(buffer, raster, m_traceOptions, *traceScene);
    }
    VkClearValue clear[2] = {};
    std::copy(std::begin(m_clearColor), std::end(m_clearColor), clear[0].color.float32);
    clear[1].depthStencil.depth = 1;
    const auto image = m_swapchainImages[imageIndex];
    const auto layout =
        m_headless ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bool capture =
        !m_loading && m_snapshotSupported && !m_snapshotAllocationFailed;
    auto lapStart = std::chrono::steady_clock::now();
    auto lap = [&](double &phase) {
      const auto now = std::chrono::steady_clock::now();
      phase += std::chrono::duration<double, std::milli>(now - lapStart).count();
      lapStart = now;
    };
    if (capture && (!m_lastFrame || !m_lastFrame->matches(m_swapchainFormat,
                                                          m_swapchainExtent))) {
      try {
        m_lastFrame = FrameSnapshot::create(
            m_physicalDevice, m_device, m_swapchainFormat, m_swapchainExtent);
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        m_snapshotAllocationFailed = true;
        capture = false;
        SDL_Log("Last-frame capture unavailable (Vulkan %d)", int(error));
      } catch (const std::bad_alloc &) {
        m_snapshotAllocationFailed = true;
        capture = false;
        SDL_Log("Last-frame capture allocation failed");
      }
    }
    const bool restore = m_loading && m_snapshotSupported && bool(m_lastFrame);
    if (capture || restore)
      m_frames[m_frameIndex].snapshot = m_lastFrame;
    if (restore) {
      gpu::GpuScope scope(buffer, "Last-frame snapshot restore", m_swapchainExtent);
      m_lastFrame->restore(buffer, image, m_swapchainExtent, layout);
    }
    lap(breakdown.snapshotMilliseconds);
    VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = restore ? m_overlayPass : m_renderPass,
        .framebuffer = m_framebuffers[imageIndex],
        .renderArea = {{0, 0}, m_swapchainExtent},
        .clearValueCount = 2,
        .pClearValues = clear,
    };
    // Font uploads are recorded by the backend before the render pass. With
    // asynchronous presentation, Present built the UI frame on its thread.
    if (!m_asyncActive)
      drawUi(VK_NULL_HANDLE);
    lap(breakdown.uiMilliseconds);
    {
      // Includes the swapchain pass's clear and, when the GPU runs ahead of
      // presentation, its wait for the acquired image.
      gpu::GpuScope scope(buffer, m_pathTracer && !m_loading ? "Composite, tonemap and prism" : "Raster scene",
                          m_swapchainExtent);
      vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
      // Halo outlines (remixapi_InstanceInfoHaloEXT) composite over the world
      // layers, under the view model and the UI: the frame's layers are split
      // around them. Without halos this is the single pass it always was.
      const uint32_t halos = m_loading ? 0 : raster.haloCount();
      const int sceneLayers = halos ? 1 : 3;
      if (!m_loading && m_pathTracer) {
        m_pathTracer->record(buffer);
        scope.close();
        gpu::GpuScope overlay(buffer, "Raster overlay (HUD, effects)", m_swapchainExtent);
        raster.record(buffer, false, true, nullptr, {}, 0, sceneLayers);
      } else if (!m_loading)
        raster.record(buffer, false, false, nullptr, {}, 0, sceneLayers);
      scope.close();
      if (halos) {
        {
          gpu::GpuScope haloTime(buffer, "Halo outlines (mask, blur, composite)", m_swapchainExtent);
          pass.renderPass = m_resumePass;
          for (uint32_t group = 0; group < halos; ++group) {
            // The scene's depth stays in the target's depth attachment.
            vkCmdEndRenderPass(buffer);
            raster.recordHaloMask(buffer, group);
            vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
            raster.recordHaloComposite(buffer, group);
          }
        }
        gpu::GpuScope overlay(buffer, "Raster overlay (view model, HUD)", m_swapchainExtent);
        raster.record(buffer, false, true, nullptr, {}, 2, 3);
      }
    }
    lap(breakdown.compositeMilliseconds);
    if (capture) {
      gpu::GpuScope scope(buffer, "Last-frame snapshot capture", m_swapchainExtent);
      vkCmdEndRenderPass(buffer);
      m_lastFrame->capture(buffer, image, layout);
      pass.renderPass = m_overlayPass;
      vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    }
    lap(breakdown.snapshotMilliseconds);
    {
      gpu::GpuScope scope(buffer, "Developer UI", m_swapchainExtent);
      drawUi(buffer);
      vkCmdEndRenderPass(buffer);
    }
    lap(breakdown.uiMilliseconds);
    if (m_screenshotSupported) {
      gpu::GpuScope scope(buffer, "Screenshot readback", m_swapchainExtent);
      m_screenshot.record(m_physicalDevice, m_device, buffer, m_swapchainImages[imageIndex],
                          m_swapchainExtent, m_swapchainFormat);
    }
    if (m_headless)
      sharedBarrier(buffer, false);
    m_gpuTimers.endFrame(buffer);
    result = vkEndCommandBuffer(buffer);
    return result == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS : fail(result);
  }

  remixapi_ErrorCode RemixNativeRuntime::requestScreenshot(const char *path) {
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    // The render thread records and completes readbacks: a request is taken
    // between its frames (rare; waits for the frame in flight).
    if (m_asyncActive)
      waitRenderThread();
    if (m_failed)
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    return m_screenshot.request(path, m_screenshotSupported);
  }

  void RemixNativeRuntime::completeScreenshot() {
    // A busy render thread completes the readback itself before recording
    // the next capture (renderFrame); PumpEvents never waits for it.
    if (busyState())
      return;
    if (m_initialized && !m_failed)
      m_screenshot.complete(m_device);
  }
} // namespace remix
