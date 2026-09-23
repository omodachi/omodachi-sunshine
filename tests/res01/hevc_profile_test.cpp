// STREAM-1b: the owned-output contract accepts HEVC, and only as prepared.
//
// Before STREAM-1b `valid_profile()` refused any codec but "h264" and
// `matches_stream()` required GameStream bitStreamFormat 0, so a client that
// asked for HEVC was answered `409 Prepared geometry mismatch` even though the
// host's VAAPI encoder had passed its HEVC probe. The prepared codec is now part
// of the geometry contract: a launch must request exactly the codec that was
// prepared (0 = H.264, 1 = HEVC), in either direction. AV1 is still never
// preparable. `desktop.status.encoders` is what the caller may prepare.
//
//   g++ -std=c++20 -I src -I <nlohmann include> tests/res01/hevc_profile_test.cpp -o /tmp/hevc_profile_test
#include "managed_desktop_geometry.h"

#include <cassert>
#include <iostream>

using nlohmann::json;
namespace g = managed_desktop::geometry;

namespace {
  const std::string output = "OMODACHI-0123456789abcdef";
  json profile(const char *codec) {
    return json {{"output_id", output}, {"output_mode_pixels", {{"width", 2408}, {"height", 1660}}},
      {"output_scale", 2}, {"logical_size", {{"width", 1204}, {"height", 830}}},
      {"stream_pixels", {{"width", 2408}, {"height", 1660}}}, {"fps", 30}, {"bitrate_kbps", 8000},
      {"codec", codec}, {"dynamic_range", "sdr"}, {"transform", 0}, {"strategy", "headless"}};
  }
  // width, height, fps, bitrate as rtsp.cpp hands them over after its FEC/audio deduction.
  bool launch(const json &p, int format) { return g::matches_stream(p, 2408, 1660, 30, 5708, format, 0, 0); }
}

int main() {
  // 1. A headless SDR profile may name h264 or hevc; nothing else.
  assert(g::valid_profile(profile("h264"), output));
  assert(g::valid_profile(profile("hevc"), output));
  assert(!g::valid_profile(profile("av1"), output));
  assert(!g::valid_profile(profile("HEVC"), output));
  assert(!g::valid_profile(profile("h265"), output));
  auto numeric = profile("h264"); numeric["codec"] = 1;
  assert(!g::valid_profile(numeric, output));
  assert(g::matches_output(profile("hevc"), output, 2408, 1660));

  // 2. The RTSP launch must request the prepared codec, in both directions.
  assert(launch(profile("h264"), 0));
  assert(!launch(profile("h264"), 1));
  assert(launch(profile("hevc"), 1));
  assert(!launch(profile("hevc"), 0));
  assert(!launch(profile("hevc"), 2));
  assert(!launch(profile("h264"), 2));
  // The rest of the contract still applies to an HEVC profile.
  assert(!g::matches_stream(profile("hevc"), 2408, 1660, 60, 5708, 1, 0, 0));
  assert(!g::matches_stream(profile("hevc"), 2408, 1660, 30, 9000, 1, 0, 0));
  assert(!g::matches_stream(profile("hevc"), 2408, 1660, 30, 5708, 1, 1, 0));
  assert(!g::matches_stream(profile("hevc"), 2400, 1660, 30, 5708, 1, 0, 0));
  // An unmanaged stream (no prepared profile) keeps upstream behaviour for every codec.
  assert(g::matches_stream(nullptr, 1920, 1080, 60, 20000, 1, 0, 0));

  // 3. desktop.status.encoders: h264 always first, hevc only when the encoder is up and HEVC probed.
  assert(g::served_codecs(true, true) == json::array({"h264", "hevc"}));
  assert(g::served_codecs(true, false) == json::array({"h264"}));
  assert(g::served_codecs(false, true) == json::array({"h264"}));
  assert(g::served_codecs(false, false) == json::array({"h264"}));

  std::cout << "hevc_profile_contract: PASS\n";
}
