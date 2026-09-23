/**
 * @file src/managed_pairing_guard.h
 * @brief Transport-independent identity/deadline gate for local PIN approval.
 */
#pragma once
#include <chrono>
#include <string>
#include <string_view>

namespace managed_pairing {
  struct request_t {
    using clock = std::chrono::steady_clock;
    std::string id;
    std::string client_cert_sha256;
    clock::time_point deadline = clock::time_point::max();
    bool pin_submitted = false;
    std::string outcome; // empty until paired/failed/cancelled/expired

    bool expired(clock::time_point now = clock::now()) const { return now >= deadline; }
    std::string status(clock::time_point now = clock::now()) const {
      if (!outcome.empty()) return outcome;
      if (expired(now)) return "expired";
      return pin_submitted ? "awaiting_client_proof" : "pending";
    }
    std::string can_approve(std::string_view request_id, std::string_view fingerprint, clock::time_point now = clock::now()) const {
      if (request_id != id || fingerprint != client_cert_sha256) return "pairing_binding_mismatch";
      if (!outcome.empty()) return "pairing_request_settled";
      if (expired(now)) return "pairing_request_expired";
      if (pin_submitted) return "pairing_already_approved";
      return {};
    }
    static bool valid_pin(std::string_view pin) {
      if (pin.size() != 4) return false;
      for (const char c : pin) if (c < '0' || c > '9') return false;
      return true;
    }
  };
}
