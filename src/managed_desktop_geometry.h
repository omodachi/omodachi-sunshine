#pragma once
// Ordinary profile/encoder geometry contract only. No authentication, TLS,
// pairing or session-identity implementation lives in this helper.
#include <cmath>
#include <string>
#include <nlohmann/json.hpp>

namespace managed_desktop::geometry {
  inline bool exact(const nlohmann::json &value, std::initializer_list<const char *> fields) {
    if (!value.is_object() || value.size() != fields.size()) return false;
    for (const auto field : fields) if (!value.contains(field)) return false;
    return true;
  }
  inline bool integer(const nlohmann::json &value, int low, int high) {
    return value.is_number_integer() && value >= low && value <= high;
  }
  inline bool number(const nlohmann::json &value, double low, double high) {
    return value.is_number() && std::isfinite(value.get<double>()) && value >= low && value <= high;
  }
  inline bool pixels(const nlohmann::json &value) {
    return exact(value,{"width","height"}) && integer(value["width"],2,16384) && integer(value["height"],2,16384);
  }
  inline bool valid_profile(const nlohmann::json &p, const std::string &output) {
    if (!exact(p,{"output_id","output_mode_pixels","output_scale","logical_size","stream_pixels","fps","bitrate_kbps","codec","dynamic_range","transform","strategy"})
        || p["output_id"] != output || p["strategy"] != "headless"
        || (p["codec"] != "h264" && p["codec"] != "hevc") || p["dynamic_range"] != "sdr"
        || !integer(p["transform"],0,0) || !pixels(p["output_mode_pixels"]) || !pixels(p["stream_pixels"])
        || !number(p["output_scale"],.25,8) || !integer(p["fps"],1,240) || !integer(p["bitrate_kbps"],64,250000)
        || !exact(p["logical_size"],{"width","height"})
        || !number(p["logical_size"]["width"],1,16384) || !number(p["logical_size"]["height"],1,16384)) return false;
    const auto sw=p["stream_pixels"]["width"].get<int>(), sh=p["stream_pixels"]["height"].get<int>();
    const auto ow=p["output_mode_pixels"]["width"].get<int>(), oh=p["output_mode_pixels"]["height"].get<int>();
    const auto scale=p["output_scale"].get<double>();
    if (sw%2 || sh%2 || sw>ow || sh>oh || std::abs((double(sw)/sh)/(double(ow)/oh)-1)>.01) return false;
    return std::abs(p["logical_size"]["width"].get<double>()-ow/scale)<.000001
        && std::abs(p["logical_size"]["height"].get<double>()-oh/scale)<.000001;
  }
  // What desktop.status reports as `encoders`: the codecs a prepared profile may
  // name *and* the probed encoder actually produces. H.264 is always listed (the
  // contract floor); HEVC only when the configured encoder is available and the
  // startup probe passed HEVC (video::active_hevc_mode >= 2).
  inline nlohmann::json served_codecs(bool encoder_available, bool hevc_available) {
    auto codecs = nlohmann::json::array({"h264"});
    if (encoder_available && hevc_available) codecs.push_back("hevc");
    return codecs;
  }
  inline bool matches_output(const nlohmann::json &p,const std::string &name,int width,int height) {
    if (p.is_null()) return true;
    return p.is_object() && p.contains("output_id") && p["output_id"].is_string()
        && valid_profile(p,p["output_id"].get<std::string>()) && p["output_id"]==name
        && p["output_mode_pixels"]["width"]==width && p["output_mode_pixels"]["height"]==height;
  }
  // GameStream's x-nv-vqos[0].bitStreamFormat: 0 = H.264, 1 = HEVC (2 = AV1, never prepared).
  inline int stream_format(const nlohmann::json &codec) { return codec == "hevc" ? 1 : 0; }
  inline bool matches_stream(const nlohmann::json &p,int width,int height,int fps,int bitrate,int codec,int range,int chroma) {
    if (p.is_null()) return true; // Ordinary unmanaged stream: preserve existing behavior.
    if (!p.is_object() || !p.contains("output_id") || !p["output_id"].is_string()
        || !valid_profile(p,p["output_id"].get<std::string>())) return false;
    return width==p["stream_pixels"]["width"] && height==p["stream_pixels"]["height"]
        && fps==p["fps"] && bitrate>=64 && bitrate<=p["bitrate_kbps"]
        && codec==stream_format(p["codec"]) && range==0 && chroma==0;
  }
}
