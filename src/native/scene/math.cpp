#include "scene/validation.h"
#include <algorithm>
#include <cmath>

namespace remix::scene {
  using namespace detail;
  Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
  Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix out = {};
    for (int col = 0; col < 4; ++col) {
      for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 4; ++k) {
          out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
        }
      }
    }
    return out;
  }
  Matrix transform(const remixapi_Transform &value) {
    for (const auto &row : value.matrix) {
      finite(row, 4);
    }
    Matrix out = identity();
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 3; ++r) {
        out[c * 4 + r] = value.matrix[r][c];
      }
    }
    return out;
  }
  Matrix inverse(const Matrix &matrix) {
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c)
        a[r][c] = matrix[c * 4 + r];
      a[r][r + 4] = 1;
    }
    for (int col = 0; col < 4; ++col) {
      int pivot = col;
      for (int r = col + 1; r < 4; ++r)
        if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
          pivot = r;
      if (std::abs(a[pivot][col]) < 1e-15)
        throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      for (int c = 0; c < 8; ++c)
        std::swap(a[col][c], a[pivot][c]);
      const double scale = a[col][col];
      for (double &v : a[col])
        v /= scale;
      for (int r = 0; r < 4; ++r)
        if (r != col) {
          const double factor = a[r][col];
          for (int c = 0; c < 8; ++c)
            a[r][c] -= factor * a[col][c];
        }
    }
    Matrix result;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        result[c * 4 + r] = float(a[r][c + 4]);
    return result;
  }
} // namespace remix::scene
