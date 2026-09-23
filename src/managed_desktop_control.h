#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace managed_desktop {
  constexpr const char *PROTOCOL = "omodachi.sunshine.desktop.v1";
  class control_t {
  public:
    nlohmann::json request(const nlohmann::json &request, bool backend_available, bool encoder_available, bool hevc_available);
    // TLS-authenticated launch fingerprint only; never a query-string uniqueid.
    bool admit_launch(const std::string &client_cert_sha256);
    void abandon_launch(const std::string &client_cert_sha256);
    bool register_session(const void *channel, const std::string &session_id, const std::string &client_cert_sha256);
    void session_stopped(const void *channel);
    std::string selected_output() const;
    bool matches_prepared_output(const std::string &name, int width, int height) const;
    bool matches_prepared_stream(int width, int height, int fps, int bitrate, int codec, int range, int chroma) const;
  private:
    nlohmann::json status_locked() const;
    mutable std::mutex mutex_;
    nlohmann::json lease_, identity_, profile_;
    std::string output_, session_id_;
    const void *channel_ = nullptr;
    bool launch_pending_ = false;
    std::unordered_map<std::string, int> unclaimed_launches_;
  };
  control_t &control();
}
