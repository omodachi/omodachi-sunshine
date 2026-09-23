#include "managed_media_observer.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace managed_media {
  namespace {
    bool dimension(int value) { return value > 0 && value <= 16384; }
    bool identifier(const std::string &value) {
      return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
      });
    }
  }
  registry_t &registry() { static registry_t instance; return instance; }
  void registry_t::enable(bool enabled) {
    enabled_ = enabled;
    if (!enabled) { std::lock_guard lock(mutex_); sessions_.clear(); }
  }
  void registry_t::begin(const void *channel, std::string id, std::uint32_t launch_id, std::function<void()> stop_callback,
                         std::string client_cert_sha256) {
    if (!enabled_ || !channel || !identifier(id)) return;
    std::lock_guard lock(mutex_);
    const auto previous = sessions_.find(channel);
    if (previous != sessions_.end()) {
      if (!previous->second.stop_complete) return;
      sessions_.erase(previous);
    }
    if (sessions_.size() >= 16) {
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.stop_complete) it = sessions_.erase(it); else ++it;
      }
      if (sessions_.size() >= 16) return;
    }
    session_t session; session.id = std::move(id); session.launch_id = launch_id; session.stop_callback = std::move(stop_callback);
    session.client_cert_sha256 = std::move(client_cert_sha256);
    sessions_.emplace(channel, std::move(session));
  }
  std::size_t registry_t::request_stop_for_client(const std::string &client_cert_sha256) {
    if (client_cert_sha256.size() != 64) return 0;
    std::vector<std::function<void()>> callbacks;
    { std::lock_guard lock(mutex_);
      for (const auto &[_, session] : sessions_) {
        if (session.client_cert_sha256 == client_cert_sha256 && !session.stop_complete && session.stop_callback) {
          callbacks.push_back(session.stop_callback);
        }
      }
    }
    for (const auto &callback : callbacks) callback();
    return callbacks.size();
  }
  bool registry_t::request_stop(const std::string &id) {
    std::function<void()> callback;
    { std::lock_guard lock(mutex_);
      for (const auto &[_, session] : sessions_) if (session.id == id && !session.stop_complete) callback = session.stop_callback;
    }
    if (!callback) return false;
    callback(); return true;
  }
  void registry_t::end(const void *channel) {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(channel); if (found == sessions_.end()) return;
    found->second.video_ended = true; found->second.status = "video_ended";
  }
  void registry_t::stopped(const void *channel) {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(channel); if (found == sessions_.end()) return;
    found->second.video_ended = true; found->second.stop_complete = true; found->second.status = "stopped"; found->second.stop_callback = {};
  }
  std::uint64_t registry_t::select(const void *channel, const void *display, std::uint64_t capture_generation, std::string backend, std::string output,
                          int width, int height, int logical_width, int logical_height, int offset_x, int offset_y) {
    if (!enabled_ || !channel || !display || !dimension(width) || !dimension(height)) return 0;
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(channel); if (found == sessions_.end() || found->second.video_ended) return 0;
    auto &session = found->second;
    const bool named = backend == "wlr" && identifier(output);
    if (session.display == display && session.capture_generation == capture_generation && session.output == output && session.output_width == width && session.output_height == height) return session.generation;
    session.generation = ++next_generation_; session.display = display; session.capture_generation = capture_generation;
    session.backend = named ? std::move(backend) : "unobserved";
    session.output = named ? std::move(output) : "";
    session.output_width = width; session.output_height = height;
    session.logical_width = dimension(logical_width) ? logical_width : 0;
    session.logical_height = dimension(logical_height) ? logical_height : 0;
    session.offset_x = offset_x; session.offset_y = offset_y;
    session.captured_width = session.captured_height = session.encoded_width = session.encoded_height = 0;
    session.captured_frames = session.encoded_packets = session.last_frame = 0;
    session.codec.clear(); session.geometry_source.clear(); session.video_codec.clear(); session.encoder_backend.clear(); session.status = "selected";
    return session.generation;
  }
  void registry_t::captured(const void *channel, const void *display, std::uint64_t generation, std::uint64_t capture_generation, int width, int height) {
    if (!enabled_ || !capture_generation || !dimension(width) || !dimension(height)) return;
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(channel); if (found == sessions_.end() || found->second.video_ended || found->second.display != display || found->second.generation != generation || found->second.capture_generation != capture_generation || found->second.output_width != width || found->second.output_height != height) return;
    auto &session = found->second;
    session.captured_width = width; session.captured_height = height; ++session.captured_frames;
  }
  void registry_t::encoded(const void *channel, std::uint64_t generation, std::uint64_t frame, int width, int height,
                           std::string codec, std::string geometry_source, std::string video_codec) {
    if (!enabled_ || !dimension(width) || !dimension(height) || !identifier(codec)) return;
    if (geometry_source != "avcodec_frame_after_packet" && geometry_source != "nvenc_picture_after_bitstream") return;
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(channel); if (found == sessions_.end()) return;
    auto &session = found->second;
    if (!session.display || session.video_ended || session.generation != generation || frame < session.last_frame) return;
    session.encoded_width = width; session.encoded_height = height; session.last_frame = frame;
    session.encoder_backend = codec.find("vaapi") != std::string::npos ? "vaapi" : codec.find("nvenc") != std::string::npos ? "nvenc" : "avcodec";
    session.video_codec = video_codec == "h264" || video_codec == "hevc" || video_codec == "av1" ? std::move(video_codec) : "unknown";
    session.codec = std::move(codec); session.geometry_source = std::move(geometry_source);
    ++session.encoded_packets;
    session.status = session.captured_frames ? "encoding_captured_frames" : "encoding_without_capture_yet";
  }
  nlohmann::json registry_t::serialize(const session_t &session) {
    nlohmann::json value {{"session_id", session.id}, {"launch_id", session.launch_id}, {"generation", session.generation}, {"capture_generation", session.capture_generation},
      {"status", session.status}, {"video_ended", session.video_ended}, {"stop_complete", session.stop_complete}, {"capture_backend", session.backend},
      {"selected_output", session.output.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.output)},
      {"selection_source", session.output.empty() ? "unobserved" : "selected_wlr_display_instance"},
      {"output_pixels", {{"width", session.output_width}, {"height", session.output_height}}},
      {"logical_size", {{"width", session.logical_width}, {"height", session.logical_height}}},
      {"output_offset", {{"x", session.offset_x}, {"y", session.offset_y}}},
      {"captured_frames", session.captured_frames}, {"encoded_packets", session.encoded_packets},
      {"last_encoded_frame", session.last_frame}, {"client_presented", nullptr}};
    value["captured_pixels"] = session.captured_frames ? nlohmann::json {{"width", session.captured_width}, {"height", session.captured_height}} : nlohmann::json(nullptr);
    value["encoded_pixels"] = session.encoded_packets ? nlohmann::json {{"width", session.encoded_width}, {"height", session.encoded_height}} : nlohmann::json(nullptr);
    value["codec"] = session.codec.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.codec); // legacy encoder-name field
    value["encoder_name"] = value["codec"];
    value["encoder_backend"] = session.encoder_backend.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.encoder_backend);
    value["video_codec"] = session.video_codec.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.video_codec);
    value["encoded_geometry_source"] = session.geometry_source.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.geometry_source);
    // Encoded dimensions come from a frame/picture accepted by the real encoder
    // after nonempty packet output, not an independently decoded SPS or client display.
    value["bitstream_dimensions_independently_parsed"] = false;
    return value;
  }
  nlohmann::json registry_t::list() const {
    std::lock_guard lock(mutex_);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto &[_, session] : sessions_) rows.push_back(serialize(session));
    return {{"ok", true}, {"sessions", rows}};
  }
  nlohmann::json registry_t::get(const std::string &id) const {
    std::lock_guard lock(mutex_);
    for (const auto &[_, session] : sessions_) if (session.id == id) return {{"ok", true}, {"session", serialize(session)}};
    return {{"ok", false}, {"error", {{"code", "media_session_not_found"}}}};
  }
}
