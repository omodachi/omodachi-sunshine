// Standalone smoke/security checks for the optional IPC boundary and identity gate.
// Build independently of Sunshine's GPU/media dependencies. No real pairing state.
#include "../../src/managed_pairing_guard.h"
#include "../../src/managed_pairing_ipc.h"
#include "../../src/managed_media_observer.h"
#include "../../src/managed_desktop_control.h"
#include "../../src/managed_tls_identity.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::chrono_literals;

static std::string exchange(const std::string &path, const std::string &payload) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  assert(path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  assert(send(fd, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size()));
  timeval timeout {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char data[4096];
  const auto size = recv(fd, data, sizeof(data), 0);
  close(fd);
  return size > 0 ? std::string(data, size) : std::string();
}

int main() {
  managed_pairing::tls_identity_registry_t tls;
  auto connection_a = std::make_shared<int>(1), connection_b = std::make_shared<int>(2);
  tls.remember(connection_a, std::string(64, 'a')); tls.remember(connection_b, std::string(64, 'b'));
  assert(tls.find(connection_a) == std::string(64, 'a'));
  assert(tls.find(connection_b) == std::string(64, 'b'));
  // Keepalive requests share ownership, even if they alias a different pointer.
  std::shared_ptr<const void> keepalive(connection_a, static_cast<const void *>(connection_a.get()));
  assert(tls.find(keepalive) == std::string(64, 'a'));
  // A distinct control block at the same raw address must not inherit identity.
  std::shared_ptr<const void> recycled(connection_a.get(), [](const void *) {});
  assert(tls.find(recycled).empty());
  assert(tls.find({}).empty());

  using request = managed_pairing::request_t;
  const auto now = request::clock::now();
  request first {"request-a", std::string(64, 'a'), now + 10s, false, {}};
  request second {"request-b", std::string(64, 'b'), now + 10s, false, {}};
  assert(first.can_approve(first.id, first.client_cert_sha256, now).empty());
  assert(first.can_approve(second.id, first.client_cert_sha256, now) == "pairing_binding_mismatch");
  assert(first.can_approve(first.id, second.client_cert_sha256, now) == "pairing_binding_mismatch");
  assert(first.can_approve(first.id, first.client_cert_sha256, now + 10s) == "pairing_request_expired");
  first.pin_submitted = true;
  assert(first.can_approve(first.id, first.client_cert_sha256, now) == "pairing_already_approved");
  assert(first.status(now) == "awaiting_client_proof");
  assert(second.status(now) == "pending");
  first.outcome = "cancelled";
  assert(first.can_approve(first.id, first.client_cert_sha256, now) == "pairing_request_settled");
  assert(request::valid_pin("0000") && request::valid_pin("9999"));
  assert(!request::valid_pin("123") && !request::valid_pin("12345") && !request::valid_pin("+123") && !request::valid_pin("12\n3"));

  managed_media::registry_t observer;
  observer.enable(true);
  int live_channel = 0, unknown_channel = 0, display_a = 0, display_b = 0;
  observer.select(nullptr, &display_a, 1, "wlr", "HEADLESS-1", 1280, 720, 1280, 720, 0, 0);
  observer.encoded(&unknown_channel, 1, 1, 1280, 720, "h264_vaapi", "avcodec_frame_after_packet");
  assert(observer.list()["sessions"].empty()); // probes/unregistered channels excluded
  observer.begin(&live_channel, "media-session-a", 7);
  auto generation = observer.select(&live_channel, &display_a, 1, "wlr", "HEADLESS-1", 1280, 720, 1280, 720, 2304, 0);
  assert(generation > 0);
  auto selected = observer.get("media-session-a")["session"];
  assert(selected["selected_output"] == "HEADLESS-1" && selected["encoded_pixels"].is_null());
  observer.encoded(&live_channel, generation, 1, 1280, 720, "h264_vaapi", "avcodec_frame_after_packet");
  assert(observer.get("media-session-a")["session"]["status"] == "encoding_without_capture_yet");
  observer.captured(&live_channel, &display_a, generation, 1, 1280, 720);
  observer.encoded(&live_channel, generation, 2, 1280, 720, "h264_vaapi", "avcodec_frame_after_packet");
  assert(observer.get("media-session-a")["session"]["status"] == "encoding_captured_frames");
  auto next_generation = observer.select(&live_channel, &display_b, 2, "wlr", "HEADLESS-1", 720, 1280, 720, 1280, 2304, 0);
  assert(next_generation > generation);
  observer.captured(&live_channel, &display_a, generation, 1, 1280, 720);
  observer.encoded(&live_channel, generation, 99, 1280, 720, "h264_vaapi", "avcodec_frame_after_packet");
  auto rotated = observer.get("media-session-a")["session"];
  assert(rotated["captured_frames"] == 0 && rotated["encoded_packets"] == 0);
  observer.captured(&live_channel, &display_b, next_generation, 2, 720, 1280);
  observer.encoded(&live_channel, next_generation, 3, 720, 1280, "h264_vaapi", "avcodec_frame_after_packet");
  observer.end(&live_channel);
  assert(observer.get("media-session-a")["session"]["stop_complete"] == false);
  observer.stopped(&live_channel);
  assert(observer.get("media-session-a")["session"]["stop_complete"] == true);
  assert(observer.get("media-session-a")["session"]["client_presented"].is_null());
  observer.encoded(&live_channel, next_generation, 4, 720, 1280, "h264_vaapi", "avcodec_frame_after_packet");
  assert(observer.get("media-session-a")["session"]["encoded_packets"] == 1);
  observer.begin(&live_channel, "media-session-b", 8);
  auto reused = observer.select(&live_channel, &display_a, 1, "wlr", "HEADLESS-2", 1280, 720, 1280, 720, 0, 0);
  assert(reused > next_generation); // reused pointer cannot reuse a generation

  managed_media::registry().enable(true);
  managed_desktop::control_t desktop;
  const nlohmann::json lease {{"lease_id", "_lease-a"}, {"lease_epoch", 1}, {"owner_device_id", "ipad-a"}, {"client_cert_sha256", std::string(64, 'a')}};
  const std::string output = "OMODACHI-0123456789abcdef";
  const auto command = [&](const char *op) { return nlohmann::json {{"op", op}, {"protocol", managed_desktop::PROTOCOL}, {"lease", lease}}; };
  auto claim = command("desktop.claim"); claim["output_id"] = output;
  assert(desktop.request(claim, true, true)["ok"] == true);
  auto foreign = claim; foreign["lease"]["owner_device_id"] = "ipad-b";
  assert(desktop.request(foreign, true, true)["error"]["code"] == "desktop_busy");
  nlohmann::json identity {{"lease_id", "_lease-a"}, {"lease_epoch", 1}, {"transition_id", "rotation-a"}, {"geometry_epoch", 1}, {"connection_generation", 1}};
  auto prepare = command("desktop.prepare"); prepare["identity"] = identity; prepare["output_id"] = output;
  prepare["profile"] = {{"output_id", output}, {"strategy", "headless"}, {"codec", "h264"}, {"dynamic_range", "sdr"}, {"transform", 0},
    {"output_mode_pixels", {{"width", 1280}, {"height", 720}}}, {"stream_pixels", {{"width", 1280}, {"height", 720}}}};
  assert(desktop.request(prepare, true, true)["result"]["prepared"] == true);
  assert(desktop.selected_output() == output);
  assert(!desktop.admit_launch(std::string(64, 'b')));
  assert(desktop.admit_launch(std::string(64, 'a')));
  assert(desktop.request(prepare, true, true)["error"]["code"] == "session_not_stopped");
  int channel = 0;
  managed_media::registry().begin(&channel, "owned-media-session", 23, [&]() {
    managed_media::registry().end(&channel);
    assert(desktop.request(command("desktop.session"), true, true)["result"]["stopped"] == false);
    managed_media::registry().stopped(&channel);
    desktop.session_stopped(&channel);
  });
  assert(desktop.register_session(&channel, "owned-media-session", std::string(64, 'a')));
  const auto capture_generation = managed_media::registry().select(&channel, &display_a, 1, "wlr", output, 1280, 720, 1280, 720, 2304, 0);
  managed_media::registry().captured(&channel, &display_a, capture_generation, 1, 1280, 720);
  managed_media::registry().encoded(&channel, capture_generation, 1, 1280, 720, "h264_vaapi", "avcodec_frame_after_packet");
  auto bound = desktop.request(command("desktop.session"), true, true)["result"];
  assert(bound["lease"] == lease && bound["identity"] == identity && bound["session_count"] == 1 && bound["stopped"] == false);
  assert(bound["media_session_id"] == "owned-media-session" && bound["media_generation"] == capture_generation);
  assert(desktop.request(command("desktop.release"), true, true)["error"]["code"] == "session_not_stopped");
  assert(desktop.request(command("desktop.stop"), true, true)["result"]["stopped"] == true);
  assert(desktop.request(command("desktop.release"), true, true)["result"]["released"] == true);
  assert(desktop.selected_output().empty());
  // An admitted unrelated launch prevents claim before its video thread exists.
  assert(desktop.admit_launch(std::string(64, 'b')));
  assert(desktop.request(claim, true, true)["error"]["code"] == "desktop_busy");
  desktop.abandon_launch(std::string(64, 'b'));
  assert(desktop.request(claim, true, true)["ok"] == true);

  char directory[] = "/tmp/oma-ipc-XXXXXX";
  assert(mkdtemp(directory));
  chmod(directory, 0700);
  std::atomic<int> callbacks {0};
  std::atomic<int> expiry_ticks {0};
  managed_pairing::local_server_t server(
    [&](const std::string &input) {
      callbacks++;
      assert(input == "{\"op\":\"pairing.list\"}");
      return std::string("{\"ok\":true,\"requests\":[]}");
    }, [&] { expiry_ticks++; });
  assert(server.start(directory));
  struct stat info {};
  assert(lstat(server.path().c_str(), &info) == 0);
  assert(S_ISSOCK(info.st_mode) && (info.st_mode & 0777) == 0600 && info.st_uid == geteuid());
  assert(exchange(server.path(), "{\"op\":\"pairing.list\"}\n") == "{\"ok\":true,\"requests\":[]}\n");
  assert(callbacks == 1);
  assert(exchange(server.path(), std::string(4097, 'x') + "\n").empty());
  assert(exchange(server.path(), "{}\n{}\n").empty());
  assert(callbacks == 1);
  managed_pairing::local_server_t duplicate([](const std::string &) { return "{}"; }, [] {});
  assert(!duplicate.start(directory));
  duplicate.stop();
  assert(std::filesystem::exists(server.path()));
  std::this_thread::sleep_for(300ms);
  assert(expiry_ticks > 0);
  server.stop();
  assert(!std::filesystem::exists(server.path()));

  // Never follow/reuse an unsafe or symlinked private child directory.
  std::filesystem::remove(std::string(directory) + "/omodachi-sunshine");
  std::filesystem::create_directory(std::string(directory) + "/unsafe");
  chmod((std::string(directory) + "/unsafe").c_str(), 0777);
  std::filesystem::create_directory_symlink(std::string(directory) + "/unsafe", std::string(directory) + "/omodachi-sunshine");
  assert(!server.start(directory));
  std::filesystem::remove_all(directory);
  std::cout << "managed-local standalone: identity, expiry, single approval, private socket, bounded frame, stale observer generation, probe exclusion and stop-complete checks passed\n";
}
