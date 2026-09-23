#pragma once
#include <algorithm>

namespace wl {
  // All dimensions are compositor logical coordinates, independently of the
  // pixel density of each output. Do not anchor the union to an imaginary (0,0).
  struct logical_desktop_bounds_t {
    bool valid = false;
    int left = 0, top = 0, right = 0, bottom = 0;
    void include(int x, int y, int width, int height) {
      if (width <= 0 || height <= 0) return;
      if (!valid) {
        left = x; top = y; right = x + width; bottom = y + height;
        valid = true;
      } else {
        left = std::min(left, x); top = std::min(top, y);
        right = std::max(right, x + width); bottom = std::max(bottom, y + height);
      }
    }
    int width() const { return right - left; }
    int height() const { return bottom - top; }
  };
}
