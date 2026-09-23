/**
 * @file src/managed_pairing_ipc.cpp
 * @brief Bounded one-request JSONL transport for a private same-UID Unix socket.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE
#endif
#include "managed_pairing_ipc.h"

#include <chrono>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace managed_pairing {
  local_server_t::local_server_t(handler_t handler, expiry_t expire):
      handler_(std::move(handler)), expire_(std::move(expire)) {}
  local_server_t::~local_server_t() { stop(); }
  std::string local_server_t::path() const { return path_; }

#if defined(__linux__) || defined(__APPLE__)
  namespace {
    constexpr std::size_t MAX_REQUEST = 4096;
    constexpr std::size_t MAX_RESPONSE = 32768;
    constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(2);

    bool private_directory(int fd) {
      struct stat info {};
      return fd >= 0 && ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0;
    }
    bool same_user_peer(int fd) {
#if defined(__linux__)
      struct ucred peer {};
      socklen_t length = sizeof(peer);
      return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0
        && length == sizeof(peer) && peer.uid == ::geteuid();
#else
      uid_t uid = static_cast<uid_t>(-1);
      gid_t gid = static_cast<gid_t>(-1);
      return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::geteuid();
#endif
    }
    bool wait_ready(int fd, short event, std::chrono::steady_clock::time_point deadline) {
      while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd item {fd, event, 0};
        const auto result = ::poll(&item, 1, static_cast<int>(remaining));
        if (result > 0) return (item.revents & event) != 0;
        if (result == 0 || errno != EINTR) return false;
      }
      return false;
    }
  }

  bool local_server_t::start(const std::string &runtime_dir) {
    // The feature is for a normal user service, never a setuid/root daemon.
    if (listener_ >= 0 || ::geteuid() != ::getuid() || ::geteuid() == 0) return false;
    const int root = ::open(runtime_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!private_directory(root)) { if (root >= 0) ::close(root); return false; }
    if (::mkdirat(root, "omodachi-sunshine", 0700) != 0 && errno != EEXIST) { ::close(root); return false; }
    const int directory = ::openat(root, "omodachi-sunshine", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(root);
    if (!private_directory(directory)) { if (directory >= 0) ::close(directory); return false; }
    path_ = runtime_dir + "/omodachi-sunshine/pairing.sock";
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path)) { ::close(directory); return false; }
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    // Never unlink an existing endpoint to make startup succeed.
    struct stat previous {};
    if (::fstatat(directory, "pairing.sock", &previous, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) { ::close(directory); return false; }
    listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0) { ::close(directory); return false; }
    ::fcntl(listener_, F_SETFD, FD_CLOEXEC);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      ::close(listener_); listener_ = -1; ::close(directory); return false;
    }
    // Parent is private before bind; there is no global umask race.
    struct stat owned {};
    if (::fstatat(directory, "pairing.sock", &owned, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(owned.st_mode)) {
      ::close(listener_); listener_ = -1; ::close(directory); return false;
    }
    socket_device_ = owned.st_dev; socket_inode_ = owned.st_ino;
    const bool ready = ::fchmodat(directory, "pairing.sock", 0600, 0) == 0 && ::listen(listener_, 4) == 0;
    ::close(directory);
    if (!ready) { stop(); return false; }
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void local_server_t::serve(int client) {
    if (!same_user_peer(client)) return;
    ::fcntl(client, F_SETFD, FD_CLOEXEC);
    ::fcntl(client, F_SETFL, ::fcntl(client, F_GETFL) | O_NONBLOCK);
#if defined(__APPLE__)
    int no_sigpipe = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    const auto deadline = std::chrono::steady_clock::now() + REQUEST_TIMEOUT;
    std::string request;
    while (!stopping_ && request.size() <= MAX_REQUEST) {
      if (!wait_ready(client, POLLIN, deadline)) return;
      char buffer[1024];
      const auto size = ::recv(client, buffer, sizeof(buffer), 0);
      if (size <= 0) return;
      request.append(buffer, size);
      const auto newline = request.find('\n');
      if (newline == std::string::npos) continue;
      if (newline != request.size() - 1 || request.size() > MAX_REQUEST) return;
      request.pop_back();
      std::string response;
      try { response = handler_(request); }
      catch (...) { response = R"({"ok":false,"error":{"code":"internal_error"}})"; }
      if (response.size() > MAX_RESPONSE) return;
      response += '\n';
      std::size_t offset = 0;
      while (!stopping_ && offset < response.size()) {
        if (!wait_ready(client, POLLOUT, deadline)) return;
#if defined(__linux__)
        const auto sent = ::send(client, response.data() + offset, response.size() - offset, MSG_NOSIGNAL);
#else
        const auto sent = ::send(client, response.data() + offset, response.size() - offset, 0);
#endif
        if (sent <= 0) return;
        offset += sent;
      }
      return;
    }
  }

  void local_server_t::run() {
    while (!stopping_) {
      // Expiration is also checked synchronously by each protocol/API action.
      try { expire_(); } catch (...) { /* No request/PIN/cert contents in logs. */ }
      pollfd item {listener_, POLLIN, 0};
      if (::poll(&item, 1, 250) <= 0 || !(item.revents & POLLIN)) continue;
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) continue;
      serve(client);
      ::close(client);
    }
  }

  void local_server_t::stop() {
    stopping_ = true;
    if (listener_ >= 0) ::shutdown(listener_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) { ::close(listener_); listener_ = -1; }
    struct stat info {};
    if (!path_.empty() && ::lstat(path_.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)
        && static_cast<unsigned long long>(info.st_dev) == socket_device_
        && static_cast<unsigned long long>(info.st_ino) == socket_inode_) ::unlink(path_.c_str());
  }
#else
  bool local_server_t::start(const std::string &) { return false; }
  void local_server_t::run() {}
  void local_server_t::serve(int) {}
  void local_server_t::stop() {}
#endif
}
