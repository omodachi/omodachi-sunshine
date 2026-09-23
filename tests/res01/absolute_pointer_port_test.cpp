// INPUT-1: where an absolute pointer coordinate actually lands.
//
// The arithmetic under test is src/absolute_pointer.h, which src/input.cpp
// uses verbatim. The expected numbers are not chosen here: they were measured
// on omarchy (Hyprland 0.56.2) by feeding the values below into a uinput device
// that copies inputtino's absolute mouse and reading `hyprctl cursorpos` back.
// Hyprland resolves an absolute pointer against the bounding box of its enabled
// monitors and offers no per-device `output` binding for a pointer, so that box
// is the contract: the port is the whole virtual desktop and the coordinates
// carry the captured output's origin inside it.
#include "src/absolute_pointer.h"

#include <cassert>
#include <cmath>

namespace {
  // third-party/inputtino/src/uinput/mouse.cpp: the absolute mouse declares a
  // fixed range and Mouse::move_abs() scales by the port dimensions alone.
  constexpr int ABS_MAX_WIDTH = 19200;
  constexpr int ABS_MAX_HEIGHT = 12000;

  long scaled_abs(int abs_max, float value, int port_dim) {
    return std::lround((abs_max / (double) port_dim) * value);
  }

  // Mirrors video::make_port() for a wlr capture.
  input::absolute::geometry_t make_port(
    float display_w,
    float display_h,
    float logical_w,
    float logical_h,
    float output_offset_x,
    float output_offset_y,
    float desktop_origin_x,
    float desktop_origin_y,
    int desktop_w,
    int desktop_h,
    float config_w,
    float config_h
  ) {
    const float scalar = std::fmin(config_w / display_w, config_h / display_h);
    const float scalar_tpcoords = std::fmin(display_w / logical_w, display_h / logical_h);
    return input::absolute::geometry_t {
      config_w,
      config_h,
      (config_w - scalar * display_w) * 0.5f,
      (config_h - scalar * display_h) * 0.5f,
      1.0f / scalar,
      scalar_tpcoords,
      output_offset_x - desktop_origin_x,
      output_offset_y - desktop_origin_y,
      desktop_w,
      desktop_h
    };
  }

  // One client coordinate, through the uinput device, to the compositor: the
  // desktop coordinate Hyprland derives from the absolute value it receives.
  struct landed_t {
    long abs_x;
    long abs_y;
    double desktop_x;
    double desktop_y;
  };

  landed_t land(const input::absolute::geometry_t &port, float fx, float fy,
                float size_w, float size_h, double layout_left, double layout_top) {
    const auto tp = input::absolute::client_to_touchport(port, {fx * size_w, fy * size_h}, {size_w, size_h});
    const auto abs = input::absolute::desktop_port(port, tp);
    assert(abs);
    const long ax = scaled_abs(ABS_MAX_WIDTH, abs->coords.x, abs->width);
    const long ay = scaled_abs(ABS_MAX_HEIGHT, abs->coords.y, abs->height);
    // Hyprland: normalized position inside the layout's bounding box.
    return {ax, ay,
            layout_left + (ax / (double) ABS_MAX_WIDTH) * abs->width,
            layout_top + (ay / (double) ABS_MAX_HEIGHT) * abs->height};
  }

  void near(double value, double expected, double tolerance) {
    assert(std::fabs(value - expected) <= tolerance);
  }
}  // namespace

int main() {
  // --- takeover: the captured output is the whole desktop ------------------
  // OMODACHI-*, 2560x1850 pixels at scale 2 (logical 1280x925), placed at
  // logical x=2304 with eDP-1 disabled, so the desktop union is the output
  // itself and its origin is 2304. The client streams 2352x1700.
  const auto takeover = make_port(2560, 1850, 1280, 925, 2304, 0, 2304, 0, 1280, 925, 2352, 1700);
  {
    const auto tl = land(takeover, 0.05f, 0.05f, 2352, 1700, 2304, 0);
    assert(tl.abs_x == 960 && tl.abs_y == 599);  // measured: cursorpos 2367, 46
    near(tl.desktop_x, 2304 + 0.05 * 1280, 1.0);
    near(tl.desktop_y, 0.05 * 925, 1.0);

    const auto br = land(takeover, 0.95f, 0.95f, 2352, 1700, 2304, 0);
    assert(br.abs_x == 18240 && br.abs_y == 11401);  // measured: 3519, 878
    near(br.desktop_x, 2304 + 0.95 * 1280, 1.0);
    near(br.desktop_y, 0.95 * 925, 1.0);

    const auto centre = land(takeover, 0.5f, 0.5f, 2352, 1700, 2304, 0);
    assert(centre.abs_x == 9600 && centre.abs_y == 6000);  // measured: 2943, 462
    near(centre.desktop_x, 2944, 1.0);
    near(centre.desktop_y, 462.5, 1.0);

    // The far corner is exactly full scale, not a clamp.
    const auto corner = land(takeover, 1.0f, 1.0f, 2352, 1700, 2304, 0);
    assert(corner.abs_x == ABS_MAX_WIDTH && corner.abs_y == ABS_MAX_HEIGHT);
  }

  // --- extend: eDP-1 is enabled at the origin ------------------------------
  // The desktop union is 3584x1440 and the captured output keeps its +2304
  // offset. The same client fractions must land on the same desktop pixels.
  const auto extended = make_port(2560, 1850, 1280, 925, 2304, 0, 0, 0, 3584, 1440, 2352, 1700);
  {
    const auto tl = land(extended, 0.05f, 0.05f, 2352, 1700, 0, 0);
    assert(tl.abs_x == 12686 && tl.abs_y == 385);  // measured: cursorpos 2367, 46
    near(tl.desktop_x, 2304 + 0.05 * 1280, 1.0);
    near(tl.desktop_y, 0.05 * 925, 1.0);

    const auto tr = land(extended, 0.95f, 0.05f, 2352, 1700, 0, 0);
    assert(tr.abs_x == 18857 && tr.abs_y == 385);  // measured: 3519, 46
    near(tr.desktop_x, 2304 + 0.95 * 1280, 1.0);

    const auto centre = land(extended, 0.5f, 0.5f, 2352, 1700, 0, 0);
    assert(centre.abs_x == 15771 && centre.abs_y == 3854);  // measured: 2943, 462
    near(centre.desktop_x, 2944, 1.0);
    near(centre.desktop_y, 462.5, 1.0);
  }

  // Normalizing to the captured output instead — the shape INPUT-1 first tried
  // — collapses the stream onto the left third of the layout, because nothing
  // in the pipeline adds the output's origin back. Measured: 179, 71 for the
  // top-left corner instead of 2367, 46.
  {
    const auto tp = input::absolute::client_to_touchport(extended, {0.05f * 2352, 0.05f * 1700}, {2352, 1700});
    const float local_x = tp.x - extended.offset_x;
    const long output_scaled = scaled_abs(ABS_MAX_WIDTH, local_x,
                                          (int) std::lround(input::absolute::output_width(extended)));
    assert(output_scaled == 960);
    near((output_scaled / (double) ABS_MAX_WIDTH) * 3584, 179.0, 1.0);
  }

  // --- origin-independence -------------------------------------------------
  // A portrait panel to the left of the primary puts the union's origin at a
  // negative x; `a41e122`'s origin is what keeps the mapping honest there.
  {
    const auto negative = make_port(2560, 1850, 1280, 925, 0, 0, -882, 0, 2162, 1440, 2352, 1700);
    const auto centre = land(negative, 0.5f, 0.5f, 2352, 1700, -882, 0);
    near(centre.desktop_x, 640, 1.0);
    near(centre.desktop_y, 462.5, 1.0);
  }

  // --- unscaled, letterboxed ----------------------------------------------
  {
    const auto unscaled = make_port(1920, 1080, 1920, 1080, 0, 0, 0, 0, 1920, 1080, 1920, 1200);
    const auto corner = land(unscaled, 1.0f, 1.0f, 1920, 1200, 0, 0);
    assert(corner.abs_x == ABS_MAX_WIDTH && corner.abs_y == ABS_MAX_HEIGHT);
    const auto centre = land(unscaled, 0.5f, 0.5f, 1920, 1200, 0, 0);
    near(centre.desktop_x, 960, 1.0);
    near(centre.desktop_y, 540, 1.0);
  }

  // A backend that reports no desktop size is refused rather than divided by.
  {
    auto degenerate = takeover;
    degenerate.desktop_width = 0;
    assert(!input::absolute::desktop_port(degenerate, {0.0f, 0.0f}));
  }

  return 0;
}
