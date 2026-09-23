// REMOTE-2 item 2: where a native touch coordinate actually lands.
//
// The arithmetic under test is src/absolute_pointer.h's monitor_port(), which
// src/input.cpp's monitor_touch_port() uses verbatim for both the touch and the
// pen passthrough device. inputtino consumes the 0..1 pair directly
// (`platf::touch::update()` reads only `touch.x`/`touch.y`), so this fraction is
// the whole mapping: whatever it says is where the finger lands on the captured
// output.
//
// Before REMOTE-2 the divisor was the *streamed* size rather than the captured
// output's logical size. Those differ by the letterbox that `make_port()` adds
// when the host plans the stream for the client's aspect ratio and the captured
// output keeps its own: `client_to_touchport()` has already taken the padding
// out of the coordinate, so leaving it in the divisor shrinks every coordinate
// on the padded axis towards the origin by a constant factor.
//
// This is *not* the 10% y loss INPUT-2 §5.1 reported. That report divided the
// `Touch passthrough` device's ABS_Y by 12000, which is the absolute *mouse*'s
// range (third-party/inputtino/src/uinput/mouse.cpp:23); the touchscreen
// declares `TOUCH_MAX_Y = 10800` (touchscreen.cpp:20). Its two samples,
// `y=5399` and `y=533`, are 0.49991 and 0.04935 of 10800 against client
// fractions of 0.50000 and ~0.0494 — exact, not 0.9011x. The bug this file
// pins is the letterbox one below, which those sessions never met because
// their stream and their captured output had the same aspect ratio.
#include "src/absolute_pointer.h"

#include <cassert>
#include <cmath>
#include <utility>

namespace {
  // Mirrors video::make_port() for a wlr capture, as the INPUT-1 test does.
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

  // A fraction of the *picture the user sees* expressed as a fraction of the
  // encoded frame: the client's `fx`/`fy` are fractions of the decoded frame,
  // black bars included, because that whole frame is what it draws.
  std::pair<float, float> content(const input::absolute::geometry_t &g, float cx, float cy) {
    return {(g.client_offset_x + cx * (g.width - 2.0f * g.client_offset_x)) / g.width,
            (g.client_offset_y + cy * (g.height - 2.0f * g.client_offset_y)) / g.height};
  }

  // input::monitor_touch_port(), in the two forms: `normalize()` is what the
  // fork does now, `normalize_by_stream()` is what it did before.
  std::pair<float, float> normalize(const input::absolute::geometry_t &g, float fx, float fy,
                                    float size_w, float size_h) {
    const auto tp = input::absolute::client_to_touchport(g, {fx * size_w, fy * size_h}, {size_w, size_h});
    const auto port = input::absolute::monitor_port(g);
    assert(port);
    return {(tp.x - g.offset_x) / port->width, (tp.y - g.offset_y) / port->height};
  }

  std::pair<float, float> normalize_by_stream(const input::absolute::geometry_t &g, float fx, float fy,
                                              float size_w, float size_h) {
    const auto tp = input::absolute::client_to_touchport(g, {fx * size_w, fy * size_h}, {size_w, size_h});
    const float w = (g.width * g.scalar_inv) / g.scalar_tpcoords;
    const float h = (g.height * g.scalar_inv) / g.scalar_tpcoords;
    return {(tp.x - g.offset_x) / w, (tp.y - g.offset_y) / h};
  }

  void near(double value, double expected, double tolerance) {
    assert(std::fabs(value - expected) <= tolerance);
  }
}  // namespace

int main() {
  // --- the geometry INPUT-1/INPUT-2 measured: no letterbox -----------------
  // OMODACHI-*, 2560x1850 pixels at scale 2 (logical 1280x925) beside eDP-1,
  // streaming 2352x1700 — the same aspect, so `client_offset_*` is zero and the
  // two forms agree. This is the regression guard: the fix must not move the
  // case that was already right.
  {
    const auto extended = make_port(2560, 1850, 1280, 925, 2304, 0, 0, 0, 3584, 1440, 2352, 1700);
    near(extended.client_offset_x, 0.0, 0.5);
    near(extended.client_offset_y, 0.0, 0.5);
    for (float f : {0.05f, 0.5f, 0.95f}) {
      const auto fixed = normalize(extended, f, f, 2352, 1700);
      const auto old = normalize_by_stream(extended, f, f, 2352, 1700);
      near(fixed.first, f, 0.001);
      near(fixed.second, f, 0.001);
      near(old.first, fixed.first, 0.001);
      near(old.second, fixed.second, 0.001);
    }
  }

  // --- a stream taller in aspect than the output ---------------------------
  // 1920x1080 at scale 1, streamed as 1920x1200. `make_port()` pads 60 rows top
  // and bottom; the width is untouched. The old divisor was 1200 logical rows
  // for an output that has 1080, so the picture covered only 1080/1200 = 0.9 of
  // the output's height: every touch landed short of its target, by more the
  // further down the picture it was, and the bottom tenth was unreachable.
  {
    const auto padded = make_port(1920, 1080, 1920, 1080, 0, 0, 0, 0, 1920, 1080, 1920, 1200);
    near(padded.client_offset_x, 0.0, 0.5);
    near(padded.client_offset_y, 60.0, 0.5);
    near(input::absolute::output_width(padded), 1920.0, 0.5);
    near(input::absolute::output_height(padded), 1080.0, 0.5);

    for (float c : {0.0f, 0.05f, 0.5f, 0.95f, 1.0f}) {
      const auto frame = content(padded, c, c);
      const auto fixed = normalize(padded, frame.first, frame.second, 1920, 1200);
      const auto old = normalize_by_stream(padded, frame.first, frame.second, 1920, 1200);
      near(fixed.first, c, 0.001);   // x was never wrong
      near(fixed.second, c, 0.001);  // y is now exact
      near(old.first, c, 0.001);
      near(old.second, c * 0.9, 0.001);  // what the device used to receive
    }

    // The bar clock: a tap 1.6% down the picture used to arrive 1.44% down.
    const auto clock = content(padded, 0.5f, 0.016f);
    near(normalize(padded, clock.first, clock.second, 1920, 1200).second, 0.016, 0.0005);
    near(normalize_by_stream(padded, clock.first, clock.second, 1920, 1200).second, 0.0144, 0.0005);
  }

  // --- a stream wider in aspect than the output ----------------------------
  // The padding moves to the x axis; the fix has to be symmetric.
  {
    const auto padded = make_port(1080, 1920, 1080, 1920, 0, 0, 0, 0, 1080, 1920, 1400, 1920);
    near(padded.client_offset_x, 160.0, 0.5);
    near(padded.client_offset_y, 0.0, 0.5);
    for (float c : {0.05f, 0.5f, 0.95f, 1.0f}) {
      const auto frame = content(padded, c, c);
      const auto fixed = normalize(padded, frame.first, frame.second, 1400, 1920);
      near(fixed.first, c, 0.001);
      near(fixed.second, c, 0.001);
      near(normalize_by_stream(padded, frame.first, frame.second, 1400, 1920).first,
           c * (1080.0f / 1400.0f), 0.001);
    }
  }

  // --- the captured output's origin is still subtracted --------------------
  // In extend the touch port carries +2304; a fraction must normalize to itself
  // whatever the output's place in the desktop is.
  {
    const auto extended = make_port(2560, 1850, 1280, 925, 2304, 0, 0, 0, 3584, 1440, 1536, 1200);
    for (float c : {0.0f, 0.25f, 1.0f}) {
      const auto frame = content(extended, c, c);
      const auto fixed = normalize(extended, frame.first, frame.second, 1536, 1200);
      near(fixed.first, c, 0.002);
      near(fixed.second, c, 0.002);
    }
  }

  // A geometry with no usable output size is refused rather than divided by.
  {
    auto degenerate = make_port(2560, 1850, 1280, 925, 2304, 0, 0, 0, 3584, 1440, 2352, 1700);
    degenerate.width = 0;
    degenerate.height = 0;
    assert(!input::absolute::monitor_port(degenerate));
  }

  return 0;
}
