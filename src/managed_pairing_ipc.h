/**
 * @file src/managed_pairing_ipc.h
 * @brief Optional same-user local pairing authority. No network listener.
 * Omodachi managed-Sunshine draft; kept separate from GameStream crypto.
 */
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace managed_pairing {
  class local_server_t {
  public:
    using handler_t = std::function<std::string(const std::string &)>;
    using expiry_t = std::function<void()>;
    local_server_t(handler_t handler, expiry_t expire);
    ~local_server_t();
    local_server_t(const local_server_t &) = delete;
    local_server_t &operator=(const local_server_t &) = delete;

    // Explicit activation only. runtime_dir must already be same-UID/private.
    // Creates only runtime_dir/omodachi-sunshine/pairing.sock, mode 0600.
    bool start(const std::string &runtime_dir);
    void stop();
    std::string path() const;

  private:
    void run();
    void serve(int client);
    handler_t handler_;
    expiry_t expire_;
    std::string path_;
    int listener_ = -1;
    unsigned long long socket_device_ = 0, socket_inode_ = 0;
    std::atomic_bool stopping_ {false};
    std::thread thread_;
  };
}
