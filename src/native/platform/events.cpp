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

#include "platform/window.h"

#include <algorithm>
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace remix {

  remixapi_ErrorCode RemixSdlWindow::pumpEvents() {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (m_eventHandler != nullptr) {
        m_eventHandler(&event, m_eventUserData);
      }
      processInput(event);

#if defined(__ANDROID__)
      if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        m_appInBackground = true;
        m_surfaceWindow = nullptr;
      }
      if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        m_appInBackground = false;
      }
#endif
      if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                           event.window.windowID == SDL_GetWindowID(m_window))) {
        m_closeRequested = true;
      } else if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == SDL_GetWindowID(m_window)) {
        m_relativeX += event.motion.xrel;
        m_relativeY += event.motion.yrel;
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.windowID == SDL_GetWindowID(m_window)) {
        m_wheelX += event.wheel.x;
        m_wheelY += event.wheel.y;
      }
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void RemixSdlWindow::setEventHandler(EventHandler handler, void *userData) {
    m_eventHandler = handler;
    m_eventUserData = userData;
  }

  void RemixSdlWindow::setGuiCapture(bool keyboard, bool mouse) {
    if (keyboard != m_guiWantsKeyboard || mouse != m_guiWantsMouse) {
      queueInput({.type = REMIXAPI_INPUT_RESET});
    }
    m_guiWantsKeyboard = keyboard;
    m_guiWantsMouse = mouse;
  }

  void RemixSdlWindow::queueInput(const remixapi_InputEvent &event) {
    if (m_inputEvents.size() >= 4096) {
      m_inputEvents.clear();
      m_inputEvents.push_back({.type = REMIXAPI_INPUT_RESET});
    }
    m_inputEvents.push_back(event);
  }

  void RemixSdlWindow::processInput(const SDL_Event &event) {
    if (SDL_GetWindowFromEvent(&event) != m_window) {
      return;
    }
    remixapi_InputEvent input = {};
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      input.type = REMIXAPI_INPUT_RESET;
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      input.type = REMIXAPI_INPUT_KEY;
      for (uint32_t key = REMIXAPI_KEY_A; key <= REMIXAPI_KEY_PAGE_DOWN; ++key) {
        if (toScancode(static_cast<remixapi_Key>(key)) == event.key.scancode) {
          input.code = key;
          break;
        }
      }
      if (!input.code)
        return;
      input.down = event.key.down;
      input.repeat = event.key.repeat;
      break;
    case SDL_EVENT_TEXT_INPUT: {
      if (!m_gameWantsText || !event.text.text)
        return;
      input.type = REMIXAPI_INPUT_TEXT;
      const char *text = event.text.text;
      while (*text) {
        input.code = SDL_StepUTF8(&text, nullptr);
        queueInput(input);
      }
      return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      input.type = REMIXAPI_INPUT_MOUSE_BUTTON;
      if (event.button.button < 1 || event.button.button > 5)
        return;
      input.code = toMouseButtons(SDL_BUTTON_MASK(event.button.button));
      input.down = event.button.down;
      input.x = event.button.x;
      input.y = event.button.y;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      input.type = REMIXAPI_INPUT_MOUSE_MOTION;
      input.x = event.motion.x;
      input.y = event.motion.y;
      input.dx = event.motion.xrel;
      input.dy = event.motion.yrel;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      input.type = REMIXAPI_INPUT_MOUSE_WHEEL;
      input.x = event.wheel.x;
      input.y = event.wheel.y;
      if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        input.x = -input.x;
        input.y = -input.y;
      }
      break;
    default:
      return;
    }
    queueInput(input);
  }

  remixapi_ErrorCode RemixSdlWindow::pollInputEvents(remixapi_InputEvent *events, uint32_t capacity,
                                                     uint32_t &count) {
    count = 0;
    const auto status = checkThread();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      return status;
    if (capacity && !events)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!capacity) {
      count = static_cast<uint32_t>(m_inputEvents.size());
    } else {
      while (count < capacity && !m_inputEvents.empty()) {
        events[count++] = m_inputEvents.front();
        m_inputEvents.pop_front();
      }
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

} // namespace remix
