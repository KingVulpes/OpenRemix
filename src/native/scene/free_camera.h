#pragma once
#include "scene/types.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace remix::scene {

  struct FreeCameraState {
    bool enabled = false;
    bool locked = false;
    bool invertY = false;
    std::array<float, 3> positionOffset = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float speed = 10.0f;
    float turningSpeed = 1.0f;
    std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();

    void reset() {
      positionOffset = {0.0f, 0.0f, 0.0f};
      yaw = 0.0f;
      pitch = 0.0f;
    }
  };

  inline Matrix getEulerRotationMatrix(float pitch, float yaw) {
    const float cosP = std::cos(pitch);
    const float sinP = std::sin(pitch);
    const float cosY = std::cos(yaw);
    const float sinY = std::sin(yaw);

    Matrix customTransform = identity();
    // Column 0: Right
    customTransform[0] = cosY;
    customTransform[1] = 0.0f;
    customTransform[2] = -sinY;
    customTransform[3] = 0.0f;

    // Column 1: Up
    customTransform[4] = sinY * sinP;
    customTransform[5] = cosP;
    customTransform[6] = cosY * sinP;
    customTransform[7] = 0.0f;

    // Column 2: Forward
    customTransform[8] = sinY * cosP;
    customTransform[9] = -sinP;
    customTransform[10] = cosP * cosY;
    customTransform[11] = 0.0f;

    return customTransform;
  }

} // namespace remix::scene
