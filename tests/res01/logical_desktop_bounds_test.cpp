#include "src/platform/linux/logical_desktop_bounds.h"
#include <cassert>
#include <cmath>

int main() {
  // Physical 3072x1920 @4/3 alongside portrait 1764x2560 @2.
  wl::logical_desktop_bounds_t mixed;
  mixed.include(0, 0, 2304, 1440);
  mixed.include(-882, 0, 882, 1280);
  assert(mixed.left == -882 && mixed.width() == 3186 && mixed.height() == 1440);
  const double normalizedInput = ((1660 * .84) * (1764.0 / 1660) / 2 + (-882 - mixed.left)) / mixed.width();
  const double compositorX = mixed.left + normalizedInput * mixed.width();
  assert(std::abs(compositorX - (-882 + 882 * .84)) < 0.00001);
  wl::logical_desktop_bounds_t top;
  top.include(0, 0, 1920, 1080); top.include(0, -1280, 882, 1280);
  assert(top.top == -1280 && top.height() == 2360);
  wl::logical_desktop_bounds_t positive;
  positive.include(100, 200, 1920, 1080);
  assert(positive.left == 100 && positive.top == 200 && positive.width() == 1920 && positive.height() == 1080);
  wl::logical_desktop_bounds_t normal;
  normal.include(0, 0, 1920, 1080); normal.include(1920, 0, 1280, 720);
  assert(normal.left == 0 && normal.top == 0 && normal.width() == 3200 && normal.height() == 1080);
  wl::logical_desktop_bounds_t invalid;
  invalid.include(0, 0, 0, 0); assert(!invalid.valid);
}
