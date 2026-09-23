/** @file src/managed_media_observer.h
 *  @brief Metadata-only observations from actual streaming, never encoder probes.
 */
#pragma once
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace managed_media {
  struct session_t {
    std::string id;
    std::uint32_t launch_id = 0;
    std::string status = "starting";
    bool video_ended = false, stop_complete = false;
    std::uint64_t generation = 0;
    std::uint64_t capture_generation = 0;
    const void *display = nullptr; // internal identity only; never serialized
    std::string backend, output;
    int output_width = 0, output_height = 0, logical_width = 0, logical_height = 0;
    int offset_x = 0, offset_y = 0;
    int captured_width = 0, captured_height = 0;
    std::uint64_t captured_frames = 0, encoded_packets = 0, last_frame = 0;
    int encoded_width = 0, encoded_height = 0;
    std::string codec, geometry_source, video_codec, encoder_backend;
    // The authenticated client this stream belongs to. Internal identity only:
    // it is what a persistent certificate revoke has to disconnect, and it is
    // never serialized into a media observation.
    std::string client_cert_sha256;
    std::function<void()> stop_callback;
  };

  // Registry includes only live videoThread channels. Null channel_data used by
  // validate_config() and retired pointers cannot publish observations.
  class registry_t {
  public:
    void enable(bool enabled);
    void begin(const void *channel, std::string id, std::uint32_t launch_id, std::function<void()> stop_callback = {},
               std::string client_cert_sha256 = {});
    bool request_stop(const std::string &session_id);
    // Stop every live stream authorized by exactly this certificate. Used only
    // by a persistent revoke; it never touches another client's session.
    std::size_t request_stop_for_client(const std::string &client_cert_sha256);
    void end(const void *channel);
    void stopped(const void *channel);
    std::uint64_t select(const void *channel, const void *display, std::uint64_t capture_generation, std::string backend, std::string output,
                int width, int height, int logical_width, int logical_height, int offset_x, int offset_y);
    void captured(const void *channel, const void *display, std::uint64_t generation, std::uint64_t capture_generation, int width, int height);
    void encoded(const void *channel, std::uint64_t generation, std::uint64_t frame, int width, int height,
                 std::string codec, std::string geometry_source, std::string video_codec = "unknown");
    nlohmann::json list() const;
    nlohmann::json get(const std::string &id) const;
  private:
    static nlohmann::json serialize(const session_t &session);
    mutable std::mutex mutex_;
    std::unordered_map<const void *, session_t> sessions_;
    std::uint64_t next_generation_ = 0;
    std::atomic_bool enabled_ {false};
  };
  registry_t &registry();
}
