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

#include <string>
#include <thread>
#include <vector>
#include <deque>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

namespace remix {

  class RemixSdlWindow {
  public:
    using EventHandler = bool (*)(const SDL_Event *event, void *userData);

    RemixSdlWindow() = default;
    ~RemixSdlWindow();

    RemixSdlWindow(const RemixSdlWindow &) = delete;
    RemixSdlWindow &operator=(const RemixSdlWindow &) = delete;

    remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
    void shutdown();

    remixapi_ErrorCode pumpEvents();
    remixapi_ErrorCode getWindowState(remixapi_WindowState &state) const;
    remixapi_ErrorCode pollMouseState(remixapi_MouseState &state);
    remixapi_ErrorCode isKeyDown(remixapi_Key key, remixapi_Bool &isDown) const;
    remixapi_ErrorCode setMouseGrabbed(bool grabbed);
    remixapi_ErrorCode setCursorPosition(float x, float y);
    remixapi_ErrorCode setFullscreen(bool fullscreen);
    remixapi_ErrorCode pollInputEvents(remixapi_InputEvent *events, uint32_t capacity, uint32_t &count);
    remixapi_ErrorCode getWindowMetrics(remixapi_WindowMetrics &metrics) const;
    remixapi_ErrorCode setTextInputEnabled(bool enabled);
    void syncTextInput(bool guiWantsText);

    std::vector<const char *> getVulkanInstanceExtensions() const;
    remixapi_ErrorCode createVulkanSurface(VkInstance instance, const VkAllocationCallbacks *allocator,
                                           VkSurfaceKHR &surface) const;

    void setEventHandler(EventHandler handler, void *userData);
    void setGuiCapture(bool keyboard, bool mouse);
    void requestClose() { m_closeRequested = true; }

    SDL_Window *handle() const { return m_window; }

    bool isSurfaceAvailable() const;
    bool isVulkanSurfaceCurrent() const;

    const std::string &lastError() const { return m_lastError; }

  private:
    remixapi_ErrorCode checkThread() const;
    void setSdlError(const char *operation) const;
    static SDL_Scancode toScancode(remixapi_Key key);
    static uint32_t toMouseButtons(SDL_MouseButtonFlags buttons);
    void queueInput(const remixapi_InputEvent &event);
    void processInput(const SDL_Event &event);

    SDL_Window *m_window = nullptr;
    std::thread::id m_ownerThread;
    bool m_ownsVideoSubsystem = false;
    bool m_appInBackground = false;
    mutable void *m_surfaceWindow = nullptr;
    bool m_closeRequested = false;
    bool m_guiWantsKeyboard = false;
    bool m_guiWantsMouse = false;
    bool m_gameWantsText = false;
    bool m_guiWantsText = false;
    std::deque<remixapi_InputEvent> m_inputEvents;
    float m_relativeX = 0.0f;
    float m_relativeY = 0.0f;
    float m_wheelX = 0.0f;
    float m_wheelY = 0.0f;
    EventHandler m_eventHandler = nullptr;
    void *m_eventUserData = nullptr;
    mutable std::string m_lastError;
  };

} // namespace remix
