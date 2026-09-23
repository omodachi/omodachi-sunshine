#include "managed_desktop_control.h"
#include "managed_media_observer.h"
#include "managed_desktop_geometry.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <regex>
#include <thread>

namespace managed_desktop {
  namespace {
    bool exact(const nlohmann::json &value, std::initializer_list<const char *> fields) {
      if (!value.is_object() || value.size() != fields.size()) return false;
      for (const auto field : fields) if (!value.contains(field)) return false;
      return true;
    }
    bool text(const nlohmann::json &value, const char *pattern) {
      return value.is_string() && value.get_ref<const std::string &>().size() <= 128
        && std::regex_match(value.get_ref<const std::string &>(), std::regex(pattern));
    }
    bool positive(const nlohmann::json &value) { return value.is_number_integer() && value.get<std::int64_t>() > 0 && value.get<std::int64_t>() <= 9007199254740991LL; }
    bool valid_lease(const nlohmann::json &value) {
      return exact(value, {"lease_id", "lease_epoch", "owner_device_id", "client_cert_sha256"})
        && text(value["lease_id"], "[A-Za-z0-9_-]{1,128}") && positive(value["lease_epoch"])
        && text(value["owner_device_id"], "[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}")
        && text(value["client_cert_sha256"], "[a-f0-9]{64}");
    }
    bool dimensions(const nlohmann::json &value) {
      return exact(value, {"width", "height"}) && value["width"].is_number_integer() && value["height"].is_number_integer()
        && value["width"].get<int>() >= 2 && value["width"].get<int>() <= 16384
        && value["height"].get<int>() >= 2 && value["height"].get<int>() <= 16384;
    }
  }
  control_t &control() { static control_t instance; return instance; }
  nlohmann::json control_t::status_locked() const {
    auto result = nlohmann::json {{"lease", lease_}, {"identity", identity_}, {"configured_output_id", output_},
      {"prepared", !identity_.is_null()}, {"session_count", launch_pending_ || channel_ ? 1 : 0},
      {"stopped", !launch_pending_ && !channel_}, {"media_session_id", session_id_.empty() ? nlohmann::json(nullptr) : nlohmann::json(session_id_)},
      {"media_generation", nullptr}, {"capture_generation", nullptr}};
    if (!session_id_.empty()) {
      const auto observed = managed_media::registry().get(session_id_);
      if (observed.value("ok", false)) {
        result["media_generation"] = observed["session"]["generation"];
        result["capture_generation"] = observed["session"]["capture_generation"];
      }
    }
    return result;
  }
  nlohmann::json control_t::request(const nlohmann::json &request, bool backend_available, bool encoder_available, bool hevc_available) {
    std::unique_lock lock(mutex_);
    const auto error = [](const char *code) { return nlohmann::json {{"protocol", PROTOCOL}, {"ok", false}, {"error", {{"code", code}}}}; };
    const auto ok = [](nlohmann::json value) { return nlohmann::json {{"protocol", PROTOCOL}, {"ok", true}, {"result", std::move(value)}}; };
    if (!request.is_object() || request.value("protocol", "") != PROTOCOL || !request.contains("op") || !request["op"].is_string()) return error("binding_mismatch");
    const auto op = request["op"].get<std::string>();
    if (op == "desktop.status") {
      if (!exact(request, {"op", "protocol"})) return error("binding_mismatch");
      return ok({{"available", backend_available && encoder_available}, {"backend", backend_available ? "wlr" : "unavailable"},
        {"encoder", encoder_available ? "vaapi" : "unavailable"}, {"encoders", geometry::served_codecs(encoder_available, hevc_available)}});
    }
    if (!request.contains("lease") || !valid_lease(request["lease"])) return error("binding_mismatch");
    const auto &lease = request["lease"];
    if (op == "desktop.claim") {
      if (!exact(request, {"op", "protocol", "lease", "output_id"}) || !text(request["output_id"], "OMODACHI-[a-f0-9]{16}")) return error("binding_mismatch");
      if (!lease_.is_null() && lease_ != lease) return error("desktop_busy");
      if (lease_.is_null()) {
        if (!unclaimed_launches_.empty()) return error("desktop_busy");
        const auto observed = managed_media::registry().list();
        for (const auto &session : observed["sessions"]) if (!session["stop_complete"].get<bool>()) return error("desktop_busy");
        lease_ = lease;
        output_ = request["output_id"].get<std::string>();
      } else if (output_ != request["output_id"].get<std::string>()) return error("binding_mismatch");
      return ok(status_locked());
    }
    if (lease_.is_null() || lease_ != lease) return error("binding_mismatch");
    if (op == "desktop.prepare") {
      if (!exact(request, {"op", "protocol", "lease", "identity", "output_id", "profile"}) || request["output_id"] != output_) return error("binding_mismatch");
      if (launch_pending_ || channel_) return error("session_not_stopped");
      const auto &id = request["identity"];
      if (!exact(id, {"lease_id", "lease_epoch", "transition_id", "geometry_epoch", "connection_generation"})
          || id["lease_id"] != lease["lease_id"] || id["lease_epoch"] != lease["lease_epoch"]
          || !text(id["transition_id"], "[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}") || !positive(id["geometry_epoch"]) || !positive(id["connection_generation"])) return error("binding_mismatch");
      if (!identity_.is_null() && id != identity_ && (id["geometry_epoch"].get<std::uint64_t>() <= identity_["geometry_epoch"].get<std::uint64_t>()
          || id["connection_generation"].get<std::uint64_t>() <= identity_["connection_generation"].get<std::uint64_t>())) return error("stale_generation");
      const auto &p = request["profile"];
      if (!geometry::valid_profile(p, output_)) return error("capture_unavailable");
      if (id == identity_ && p != profile_) return error("stale_generation");
      identity_ = id; profile_ = p; session_id_.clear();
      return ok(status_locked());
    }
    if (!exact(request, {"op", "protocol", "lease"})) return error("binding_mismatch");
    if (op == "desktop.session") return ok(status_locked());
    if (op == "desktop.stop") {
      // Actual stop is signalled by the observer's per-session stop callback,
      // never by terminating a host app or every unrelated GameStream session.
      const auto session_id = session_id_;
      lock.unlock();
      managed_media::registry().request_stop(session_id);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1400);
      do {
        lock.lock();
        if (!launch_pending_ && !channel_) return ok(status_locked());
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (std::chrono::steady_clock::now() < deadline);
      return error("session_not_stopped");
    }
    if (op == "desktop.release") {
      if (launch_pending_ || channel_) return error("session_not_stopped");
      auto result = status_locked(); result["released"] = true;
      lease_ = nullptr; identity_ = nullptr; profile_ = nullptr; output_.clear(); session_id_.clear();
      return ok(result);
    }
    return error("binding_mismatch");
  }
  bool control_t::admit_launch(const std::string &fingerprint) {
    std::lock_guard lock(mutex_);
    if (lease_.is_null()) { ++unclaimed_launches_[fingerprint]; return true; }
    if (identity_.is_null() || fingerprint != lease_["client_cert_sha256"].get<std::string>() || launch_pending_ || channel_) return false;
    launch_pending_ = true;
    return true;
  }
  void control_t::abandon_launch(const std::string &fingerprint) {
    std::lock_guard lock(mutex_);
    if (!lease_.is_null() && lease_["client_cert_sha256"] == fingerprint && !channel_) launch_pending_ = false;
    const auto pending = unclaimed_launches_.find(fingerprint);
    if (pending != unclaimed_launches_.end() && --pending->second <= 0) unclaimed_launches_.erase(pending);
  }
  bool control_t::register_session(const void *channel, const std::string &id, const std::string &fingerprint) {
    std::lock_guard lock(mutex_);
    if (lease_.is_null()) {
      const auto pending = unclaimed_launches_.find(fingerprint);
      if (pending != unclaimed_launches_.end() && --pending->second <= 0) unclaimed_launches_.erase(pending);
      return true;
    }
    if (!launch_pending_ || lease_["client_cert_sha256"] != fingerprint || channel_) return false;
    channel_ = channel; session_id_ = id; launch_pending_ = false;
    return true;
  }
  void control_t::session_stopped(const void *channel) {
    std::lock_guard lock(mutex_); if (channel_ == channel) channel_ = nullptr;
  }
  bool control_t::matches_prepared_output(const std::string &name, int width, int height) const {
    std::lock_guard lock(mutex_);
    return geometry::matches_output(profile_, name, width, height);
  }
  bool control_t::matches_prepared_stream(int width, int height, int fps, int bitrate, int codec, int range, int chroma) const {
    std::lock_guard lock(mutex_);
    return geometry::matches_stream(profile_, width, height, fps, bitrate, codec, range, chroma);
  }
  std::string control_t::selected_output() const {
    std::lock_guard lock(mutex_); return identity_.is_null() ? std::string() : output_;
  }
}
