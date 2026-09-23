/**
 * @file src/absolute_pointer.h
 * @brief Pure geometry for absolute pointer input: client coordinates onto the touch port,
 *        and the touch port onto the captured output. No I/O, no logging, unit-testable.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cmath>
#include <optional>

namespace input::absolute {

  struct point_t {
    float x;
    float y;
  };

  /**
   * @brief The streamed touch port reduced to the numbers the mapping needs.
   *
   * `width`/`height` are the client's stream pixels (`config.width`/`config.height`).
   * `scalar_inv` converts them to captured pixels, `scalar_tpcoords` converts captured
   * pixels to compositor logical units (1.0 when the backend reports no logical geometry).
   * `offset_x`/`offset_y` are the captured output's origin inside the virtual desktop,
   * already expressed in the unit `client_to_touchport()` returns.
   */
  struct geometry_t {
    float width;
    float height;
    float client_offset_x;
    float client_offset_y;
    float scalar_inv;
    float scalar_tpcoords;
    float offset_x;
    float offset_y;
    /// The virtual desktop, in the same unit as the coordinates: the compositor's
    /// logical desktop union when the backend reports one (wlgrab/kmsgrab fill
    /// `env_logical_*`), the pixel desktop (`env_*`) otherwise.
    int desktop_width;
    int desktop_height;
  };

  /**
   * @brief Map a client coordinate pair onto the touch port. Upstream arithmetic, unchanged.
   * @param g The touch port geometry.
   * @param val The client coordinate pair.
   * @param size The client reference size the pair is expressed in.
   * @return The coordinate pair in touch-port coordinates: compositor logical units offset by
   *         the captured output's origin when the backend reports logical geometry, captured
   *         pixels offset by the output's pixel origin otherwise.
   */
  inline point_t client_to_touchport(const geometry_t &g, point_t val, point_t size) {
    auto scalarX = g.width / size.x;
    auto scalarY = g.height / size.y;

    float x = std::clamp(val.x, 0.0f, size.x) * scalarX;
    float y = std::clamp(val.y, 0.0f, size.y) * scalarY;

    auto offsetX = g.client_offset_x;
    auto offsetY = g.client_offset_y;

    x = std::clamp(x, offsetX, (size.x * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.y * scalarY) - offsetY);

    x = (x - offsetX) * g.scalar_inv;
    y = (y - offsetY) * g.scalar_inv;

    return {
      (x + g.offset_x * g.scalar_tpcoords) / g.scalar_tpcoords,
      (y + g.offset_y * g.scalar_tpcoords) / g.scalar_tpcoords
    };
  }

  /// The captured output's width, in the unit `client_to_touchport()` returns.
  inline float output_width(const geometry_t &g) {
    return ((g.width - 2.0f * g.client_offset_x) * g.scalar_inv) / g.scalar_tpcoords;
  }

  /// The captured output's height, in the same unit.
  inline float output_height(const geometry_t &g) {
    return ((g.height - 2.0f * g.client_offset_y) * g.scalar_inv) / g.scalar_tpcoords;
  }

  /**
   * @brief The captured output's logical size: the box a touch or pen device is
   *        normalized against.
   *
   * REMOTE-2 item 2. This is `output_width()`/`output_height()` and not the
   * streamed `width`/`height`, which is what `monitor_touch_port()` divided by
   * before. Those two differ by exactly the letterbox: the host plans the
   * stream for the client's aspect ratio while the captured output keeps its
   * own, so `make_port()` pads one axis (`client_offset_*`) and the un-padded
   * axis is the only one whose streamed size still equals the output.
   *
   * With the padding left in the divisor, the picture covers only
   * `1 - 2*offset/size` of the padded axis: every coordinate lands short of its
   * target in proportion to its distance from the origin, and the far edge
   * cannot be reached at all. The unpadded axis is exact, so it shows up as an
   * error on one axis only.
   *
   * This is not INPUT-2 §5.1's reported 10% y loss; see FORK.md, "Touch and pen
   * normalization". That measurement read the touchscreen's `ABS_Y` against the
   * absolute mouse's 12000 rather than the touchscreen's own 10800.
   */
  struct touch_port_size_t {
    float width;
    float height;
  };

  /**
   * @brief The touch/pen port for a touch-port geometry.
   * @return `std::nullopt` when the captured output has no usable size.
   */
  inline std::optional<touch_port_size_t> monitor_port(const geometry_t &g) {
    const float width = output_width(g);
    const float height = output_height(g);
    if (!(width > 0.0f) || !(height > 0.0f)) {
      return std::nullopt;
    }
    return touch_port_size_t {width, height};
  }

  /**
   * @brief The port an absolute pointer device is normalized against.
   *
   * The whole virtual desktop, with the coordinates left where
   * `client_to_touchport()` put them: offset by the captured output's origin
   * inside that desktop. This is not a choice — it is the box the compositor
   * maps the device onto. `inputtino::Mouse::move_abs()` only divides by these
   * dimensions and adds no offset, and Hyprland (0.56.2, measured for INPUT-1)
   * resolves an absolute pointer against the bounding box of its enabled
   * monitors; it has no per-device `output` binding for a pointer at all, so
   * the coordinates have to be desktop-relative for the pointer to reach the
   * captured output. `a41e122` is what makes the origin of that box real
   * instead of an assumed (0,0).
   *
   * Touch and pen are the opposite case and are normalized to the captured
   * output by `monitor_touch_port()`: for those devices the compositor does
   * take an output binding, and inputtino consumes 0..1 coordinates.
   */
  struct abs_port_t {
    int width;
    int height;
    point_t coords;
  };

  /**
   * @brief The absolute-pointer port for a touch-port geometry.
   * @return `std::nullopt` when the backend reported no usable desktop size.
   */
  inline std::optional<abs_port_t> desktop_port(const geometry_t &g, point_t tp) {
    if (g.desktop_width <= 0 || g.desktop_height <= 0) {
      return std::nullopt;
    }
    return abs_port_t {g.desktop_width, g.desktop_height, tp};
  }

}  // namespace input::absolute
