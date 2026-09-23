/** @file src/managed_tls_identity.h
 * @brief Binds verified leaf fingerprints to shared connection ownership, not endpoints.
 */
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace managed_pairing {
  class tls_identity_registry_t {
  public:
    void remember(const std::shared_ptr<const void> &connection, std::string fingerprint) {
      if (!connection || fingerprint.size() != 64) return;
      std::lock_guard lock(mutex_);
      for (auto it = bindings_.begin(); it != bindings_.end();) {
        if (it->first.expired()) it = bindings_.erase(it); else ++it;
      }
      if (bindings_.size() < 256) bindings_[connection] = std::move(fingerprint);
    }
    std::string find(const std::shared_ptr<const void> &connection) const {
      if (!connection) return {};
      std::lock_guard lock(mutex_);
      const auto found = bindings_.find(connection);
      return found == bindings_.end() ? std::string() : found->second;
    }
  private:
    using identity_t = std::weak_ptr<const void>;
    mutable std::mutex mutex_;
    std::map<identity_t, std::string, std::owner_less<identity_t>> bindings_;
  };
}
